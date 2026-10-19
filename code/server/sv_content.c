/*
===========================================================================
Copyright (C) 1999-2005 Id Software, Inc.

This file is part of Quake III Arena source code.

Quake III Arena source code is free software; you can redistribute it
and/or modify it under the terms of the GNU General Public License as
published by the Free Software Foundation; either version 2 of the License,
or (at your option) any later version.

Quake III Arena source code is distributed in the hope that it will be
useful, but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with Quake III Arena source code; if not, write to the Free Software
Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
===========================================================================
*/
// sv_content.c -- the server's content manifest: the paks a client may
// fetch from it, by SHA-256 (content.h), built at each map load and again
// as the pak index learns more hashes. The current one and the one before
// are kept, so a transfer that spans a change still finds its hash, each
// with a table of where its paks are, which never goes on the wire.
// Then the fast downloads themselves (download-protocol.md sections 6 to
// 8): the clients' CONTROLs, what they may fetch, and the datagrams sent
// between frames, over each client's transfers (sv_transfer.c)

#include "server.h"
#include "sv_transfer.h"
#include "../qcommon/content.h"

typedef struct {
	byte		sha256[SHA256_DIGEST_SIZE];
	char		*ospath;
	int64_t		size;
	int64_t		mtime;
} svManifestPak_t;

typedef struct {
	byte			*data;
	int				length;
	byte			sha256[SHA256_DIGEST_SIZE];
	uint64_t		requiredBytes;
	svManifestPak_t	*paks;			// its entries with a hash
	int				numPaks;
} svManifest_t;

static svManifest_t	svManifests[2];		// the current one, then the one before
static byte			svBuildBuffer[OFCM_MAX_SIZE];
static qboolean		svHashQueued;		// a pak changed since the last build: FS_HashPaks ran

static void SV_ContentCloseGone( void );

// the paks left out of a content manifest already said, by checksum and
// why: an open addressing set, which stops taking more once nearly full
#define LEFT_OUT_SLOTS	8192
static struct {
	unsigned int		checksum;
	manifestListing_t	why;		// MANIFEST_LISTED for an empty slot
} svLeftOut[LEFT_OUT_SLOTS];
static int	svNumLeftOut;

/*
=================
SV_ContentLeftOutReason
=================
*/
static const char *SV_ContentLeftOutReason( manifestListing_t why ) {
	switch ( why ) {
	case MANIFEST_UNHASHED:
		return "isn't hashed yet";
	case MANIFEST_UNREADABLE:
		return "can't be read";
	case MANIFEST_BAD_NAME:
		return "has a name no download may have";
	case MANIFEST_SHADOWED:
		return "has the name of a pak found before it";
	case MANIFEST_TOO_MANY:
		return "is past the 4,096 paks a content manifest holds";
	default:
		return NULL;
	}
}

/*
=================
SV_ContentNoteLeftOut

Says once why a pak isn't offered by its hash; a client that needs it
gets it the classic way
=================
*/
static void SV_ContentNoteLeftOut( const contentPak_t *pak, manifestListing_t why ) {
	const char		*reason = SV_ContentLeftOutReason( why );
	unsigned int	slot;

	if ( !reason ) {
		return;
	}
	for ( slot = ( pak->checksum * 7 + why ) % LEFT_OUT_SLOTS; svLeftOut[slot].why != MANIFEST_LISTED;
		slot = ( slot + 1 ) % LEFT_OUT_SLOTS ) {
		if ( svLeftOut[slot].checksum == pak->checksum && svLeftOut[slot].why == why ) {
			return;
		}
	}
	if ( svNumLeftOut < LEFT_OUT_SLOTS * 7 / 8 ) {
		svLeftOut[slot].checksum = pak->checksum;
		svLeftOut[slot].why = why;
		svNumLeftOut++;
	}
	Com_Printf( "Content manifest: %s/%s.pk3 %s, so it's sent the classic way\n", pak->gameDir, pak->name, reason );
}

static void SV_ContentFree( svManifest_t *manifest ) {
	int		i;

	if ( manifest->data ) {
		Z_Free( manifest->data );
	}
	for ( i = 0; i < manifest->numPaks; i++ ) {
		Z_Free( manifest->paks[i].ospath );
	}
	if ( manifest->paks ) {
		Z_Free( manifest->paks );
	}
	Com_Memset( manifest, 0, sizeof( *manifest ) );
}

static int SV_ContentCompareChecksums( const void *a, const void *b ) {
	unsigned int	ca = *(const unsigned int *)a, cb = *(const unsigned int *)b;

	return ca < cb ? -1 : ca > cb;
}

/*
=================
SV_ContentReferenced

Marks as required the paks whose checksums sv_referencedPaks lists, as
the map load set it. The paks' own references grow after that as files
are read (a listen server's client loading the map, a bot joining), and
a content manifest built after a hashing job must still require what the
clients were told to have, no more
=================
*/
static void SV_ContentReferenced( contentPak_t *paks, int count ) {
	const char		*s = Cvar_VariableString( "sv_referencedPaks" );
	unsigned int	*listed;
	char			*end;
	long			value;
	int				i, numListed = 0;

	listed = Z_Malloc( ( strlen( s ) / 2 + 1 ) * sizeof( *listed ) );	// "%i " each
	for ( ;; s = end ) {
		value = strtol( s, &end, 10 );
		if ( end == s ) {
			break;
		}
		listed[numListed++] = (unsigned int)(int)value;
	}
	qsort( listed, numListed, sizeof( *listed ), SV_ContentCompareChecksums );
	for ( i = 0; i < count; i++ ) {
		paks[i].referenced = bsearch( &paks[i].checksum, listed, numListed, sizeof( *listed ),
			SV_ContentCompareChecksums ) != NULL;
	}
	Z_Free( listed );
}

/*
=================
SV_ContentBuildManifest

At a map load, after the referenced paks are known, and whenever the pak
index learns hashes
=================
*/
void SV_ContentBuildManifest( void ) {
	contentPak_t		*paks;
	manifestListing_t	*listing;
	ofcmHeader_t		header;
	svManifest_t		built;
	const char			*error = NULL;
	char				hex[SHA256_HEX_SIZE];
	int					i, count, unhashed = 0;

	count = FS_ContentPaks( NULL, 0 );
	paks = Z_Malloc( ( count + 1 ) * sizeof( *paks ) );
	listing = Z_Malloc( ( count + 1 ) * sizeof( *listing ) );
	count = FS_ContentPaks( paks, count );
	SV_ContentReferenced( paks, count );

	Com_Memset( &built, 0, sizeof( built ) );
	built.length = Content_BuildManifest( paks, count, FS_ValidDownloadPak, listing, &header,
		svBuildBuffer, sizeof( svBuildBuffer ), &error );
	if ( built.length && ( svManifests[0].length != built.length
		|| memcmp( svManifests[0].data, svBuildBuffer, built.length ) ) ) {
		built.data = Z_Malloc( built.length );
		Com_Memcpy( built.data, svBuildBuffer, built.length );
		Com_SHA256Buffer( built.data, built.length, built.sha256 );
		built.requiredBytes = header.requiredBytes;
		built.paks = Z_Malloc( ( header.numEntries + 1 ) * sizeof( *built.paks ) );
	}
	for ( i = 0; i < count; i++ ) {
		SV_ContentNoteLeftOut( &paks[i], listing[i] );
		unhashed += listing[i] == MANIFEST_UNHASHED;
		if ( built.data && listing[i] == MANIFEST_LISTED && paks[i].sha256 && !paks[i].refused ) {
			svManifestPak_t	*p = &built.paks[built.numPaks++];

			Com_Memcpy( p->sha256, paks[i].sha256, SHA256_DIGEST_SIZE );
			p->ospath = CopyString( paks[i].ospath );
			p->size = paks[i].size;
			p->mtime = paks[i].mtime;
		}
	}
	Z_Free( listing );
	Z_Free( paks );

	if ( !built.length ) {
		// the one before stays for transfers that span the change, but
		// getinfo no longer offers it as this map's
		if ( svManifests[0].data ) {
			SV_ContentFree( &svManifests[1] );
			svManifests[1] = svManifests[0];
			Com_Memset( &svManifests[0], 0, sizeof( svManifests[0] ) );
			SV_ContentCloseGone();
		}
		Com_Printf( S_COLOR_YELLOW "WARNING: no content manifest: %s, so every pak is sent the classic way\n", error );
		return;
	}
	if ( !built.data ) {
		return;		// the same as the current one
	}

	SV_ContentFree( &svManifests[1] );
	svManifests[1] = svManifests[0];
	svManifests[0] = built;
	svHashQueued = qfalse;
	SV_ContentCloseGone();

	Com_SHA256Hex( built.sha256, hex );
	Com_Printf( "Content manifest %.12s: %d paks required (%llu bytes), %d optional; %d not hashed yet\n",
		hex, header.numRequired, (unsigned long long)header.requiredBytes, header.numOptional, unhashed );
}

/*
=================
SV_ContentManifest

The content manifest with that hash, the current one or the one before,
or the current one for NULL; NULL if there's none
=================
*/
static const svManifest_t *SV_ContentFindManifest( const byte *sha256 ) {
	int		i;

	for ( i = 0; i < (int)ARRAY_LEN( svManifests ); i++ ) {
		if ( svManifests[i].data && ( !sha256 || !memcmp( svManifests[i].sha256, sha256, SHA256_DIGEST_SIZE ) ) ) {
			return &svManifests[i];
		}
	}
	return NULL;
}

const byte *SV_ContentManifest( const byte *sha256, int *length ) {
	const svManifest_t	*manifest = SV_ContentFindManifest( sha256 );

	if ( !manifest ) {
		return NULL;
	}
	*length = manifest->length;
	return manifest->data;
}

/*
=================
SV_ContentFindPak

Where the pak with that hash is, as the current content manifest or the
one before lists it: its OS path, and the size and modification time it
had then, which the file must still have for its bytes to be the hash's;
NULL if neither lists it
=================
*/
const char *SV_ContentFindPak( const byte sha256[SHA256_DIGEST_SIZE], int64_t *size, int64_t *mtime ) {
	int		i, j;

	for ( i = 0; i < (int)ARRAY_LEN( svManifests ); i++ ) {
		for ( j = 0; j < svManifests[i].numPaks; j++ ) {
			if ( !memcmp( svManifests[i].paks[j].sha256, sha256, SHA256_DIGEST_SIZE ) ) {
				*size = svManifests[i].paks[j].size;
				*mtime = svManifests[i].paks[j].mtime;
				return svManifests[i].paks[j].ospath;
			}
		}
	}
	return NULL;
}

/*
=================
SV_ContentInfo

getinfo's content keys, for a client whose tokens ask for the content
protocol: the current content manifest's hash, and the bytes of its
required paks
=================
*/
void SV_ContentInfo( char *infostring ) {
	char	hex[SHA256_HEX_SIZE];

	if ( !svManifests[0].data ) {
		return;
	}
	Com_SHA256Hex( svManifests[0].sha256, hex );
	Info_SetValueForKey( infostring, "ofm", hex );
	Info_SetValueForKey( infostring, "ofr", va( "%llu", (unsigned long long)svManifests[0].requiredBytes ) );
}

/*
=================
SV_ContentFrame

Once a server frame: what the hash job finished goes into the pak index,
paused or not, and after the server has stopped, so the job it started is
finished; the content manifest is built again when the index learned
hashes
=================
*/
void SV_ContentFrame( void ) {
	if ( Content_PakIndex_Frame() && com_sv_running->integer ) {
		SV_ContentBuildManifest();
	}
}

/*
=================
SV_ContentShutdown
=================
*/
void SV_ContentShutdown( void ) {
	int		i;

	for ( i = 0; i < (int)ARRAY_LEN( svManifests ); i++ ) {
		SV_ContentFree( &svManifests[i] );
	}
	Com_Memset( svLeftOut, 0, sizeof( svLeftOut ) );
	svNumLeftOut = 0;
}

/*
=============================================================================

FAST DOWNLOADS

=============================================================================
*/

#define CONTENT_BLOCK			16384		// what a pak's reads fetch at once
#define CONTENT_MAX_CLIENTS		32			// downloading by the fast path at once, whatever sv_maxDownloads
#define CONTENT_PER_ADDRESS		3			// downloading from one address not on the LAN
#define CONTENT_BAD_DATAGRAMS	16			// a client's that can't be read, before it's dropped
#define CONTENT_FLOOR			( 100 * 1024 )	// bytes a second the loading clients share at least
#define CONTENT_RETRY_BUSY		2000		// ms
#define CONTENT_RETRY_LIMIT		1000
#define CONTENT_SEND_MAX		64			// datagrams a call of SV_ContentSend
#define CONTENT_SEND_TIME		1000000		// ns a call of SV_ContentSend, checked every 8 datagrams

// an open transfer's file: a pak through its own FILE, or a content
// manifest, from memory, by its hash
typedef struct {
	qboolean	used;
	FILE		*fp;			// NULL for a content manifest
	byte		sha256[SHA256_DIGEST_SIZE];
	int64_t		blockStart;		// -1 for none read
	int			blockLength;
	byte		block[CONTENT_BLOCK];
} svContentFile_t;

struct svContent_s {
	svTransfers_t	t;
	svContentFile_t	files[SVT_TRANSFERS];
};

static dlccBucket_t	svContentCap;		// sv_dlMaxRate's, every fast transfer's
static int			svContentCapRate = -1;
static qboolean		svContentSocketFull;
static int			svContentNextClient;
static int			svContentIgnored;	// datagrams not counted against a client, for sv_showContent
static int64_t		svContentNextReport;

static int64_t SV_ContentNow( void ) {
	return Sys_Nanoseconds() / 1000;
}

/*
=================
SV_ContentAdmit

Section 6.1's answer to a request, after disabled and limit: what it
names, then whether it may be sent now, then whether it can be read
=================
*/
static void SV_ContentAdmit( void *arg, const ofdRequest_t *request, qboolean limited, svtAdmission_t *out );

static qboolean SV_ContentRead( void *arg, void *file, uint32_t offset, byte *out, int length ) {
	svContentFile_t	*f = file;
	int64_t			start;
	int				within, n;

	if ( !f->fp ) {
		const svManifest_t	*manifest = SV_ContentFindManifest( f->sha256 );

		if ( !manifest || offset + (int64_t)length > manifest->length ) {
			return qfalse;
		}
		Com_Memcpy( out, manifest->data + offset, length );
		return qtrue;
	}
	while ( length > 0 ) {
		start = offset - offset % CONTENT_BLOCK;
		if ( start != f->blockStart ) {
			f->blockStart = -1;
			if ( fseek( f->fp, (long)start, SEEK_SET ) ) {
				return qfalse;
			}
			f->blockLength = (int)fread( f->block, 1, CONTENT_BLOCK, f->fp );
			f->blockStart = start;
		}
		within = (int)( offset - start );
		n = MIN( length, f->blockLength - within );
		if ( n <= 0 ) {
			return qfalse;
		}
		Com_Memcpy( out, f->block + within, n );
		out += n;
		offset += n;
		length -= n;
	}
	return qtrue;
}

static void SV_ContentClose( void *arg, void *file ) {
	svContentFile_t	*f = file;

	if ( f->fp ) {
		fclose( f->fp );
	}
	f->fp = NULL;
	f->used = qfalse;
}

static qboolean SV_ContentSendDatagram( void *arg, const byte *data, int length ) {
	const client_t	*cl = arg;

	if ( !NET_SendPacketChecked( NS_SERVER, length, data, cl->netchan.remoteAddress ) ) {
		svContentSocketFull = qtrue;
		return qfalse;
	}
	return qtrue;
}

static uint32_t SV_ContentRandom( void *arg ) {
	uint32_t	r;

	Com_RandomBytes( (byte *)&r, sizeof( r ) );
	return r;
}

static svTransferEnv_t SV_ContentEnv( client_t *cl ) {
	svTransferEnv_t	env;

	env.arg = cl;
	env.admit = SV_ContentAdmit;
	env.read = SV_ContentRead;
	env.close = SV_ContentClose;
	env.send = SV_ContentSendDatagram;
	env.rand32 = SV_ContentRandom;
	return env;
}

/*
=================
SV_ContentTransfers

The fast transfers a client has open
=================
*/
int SV_ContentTransfers( const client_t *cl ) {
	return cl->content ? SVT_OpenTransfers( &cl->content->t ) : 0;
}

/*
=================
SV_ClientDownloading

A classic download, or a pak transfer open for loading: no snapshots,
no VoIP
=================
*/
qboolean SV_ClientDownloading( const client_t *cl ) {
	return *cl->downloadName || ( cl->content && SVT_Loading( &cl->content->t ) );
}

/*
=================
SV_ContentBusy

Whether a pak may not be sent this client now: sv_maxDownloads clients
downloading by either path, the fast path's 32, or 3 from its address
when it isn't on the LAN. A client downloading already isn't held back
=================
*/
static qboolean SV_ContentBusy( const client_t *cl ) {
	const client_t	*other;
	int				i, downloading = 0, fast = 0, sameAddress = 0;

	if ( SV_ContentTransfers( cl ) ) {
		return qfalse;
	}
	for ( i = 0, other = svs.clients; i < sv_maxclients->integer; i++, other++ ) {
		qboolean	open = SV_ContentTransfers( other ) > 0;

		if ( other == cl || other->state < CS_CONNECTED || ( !open && !other->download ) ) {
			continue;
		}
		downloading++;
		fast += open;
		sameAddress += NET_CompareBaseAdr( other->netchan.remoteAddress, cl->netchan.remoteAddress );
	}
	return ( sv_maxDownloads->integer > 0 && downloading >= sv_maxDownloads->integer )
		|| fast >= CONTENT_MAX_CLIENTS
		|| ( sameAddress >= CONTENT_PER_ADDRESS && !Sys_IsLANAddress( cl->netchan.remoteAddress ) );
}

static void SV_ContentAdmit( void *arg, const ofdRequest_t *request, qboolean limited, svtAdmission_t *out ) {
	static const byte	zero[SHA256_DIGEST_SIZE];
	client_t			*cl = arg;
	const svManifest_t	*manifest = NULL;
	svContentFile_t		*f = NULL;
	const char			*ospath = NULL;
	int64_t				size, mtime, nowSize, nowMtime;
	int					i;

	if ( !( sv_allowDownload->integer & DLF_ENABLE ) || ( sv_allowDownload->integer & DLF_NO_UDP ) ) {
		out->status = OFD_STATUS_DISABLED;
		return;
	}
	if ( limited ) {
		out->status = OFD_STATUS_LIMIT;
		out->retryAfter = CONTENT_RETRY_LIMIT;
		return;
	}

	// the zero hash is the current content manifest
	if ( !memcmp( request->sha256, zero, sizeof( zero ) ) ) {
		manifest = svManifests[0].data ? &svManifests[0] : NULL;
	} else {
		manifest = SV_ContentFindManifest( request->sha256 );
	}
	if ( manifest ) {
		out->kind = OFD_KIND_MANIFEST;
		size = manifest->length;
		Com_Memcpy( out->sha256, manifest->sha256, SHA256_DIGEST_SIZE );
	} else if ( memcmp( request->sha256, zero, sizeof( zero ) )
		&& ( ospath = SV_ContentFindPak( request->sha256, &size, &mtime ) ) != NULL ) {
		out->kind = OFD_KIND_PAK;
		Com_Memcpy( out->sha256, request->sha256, SHA256_DIGEST_SIZE );
	} else {
		out->status = OFD_STATUS_NOT_FOUND;
		return;
	}

	if ( request->offset >= size ) {
		out->status = OFD_STATUS_BAD_REQUEST;
		return;
	}
	if ( ospath && SV_ContentBusy( cl ) ) {
		out->status = OFD_STATUS_BUSY;
		out->retryAfter = CONTENT_RETRY_BUSY;
		return;
	}

	for ( i = 0; i < SVT_TRANSFERS && !f; i++ ) {
		if ( !cl->content->files[i].used ) {
			f = &cl->content->files[i];
		}
	}
	if ( !f ) {
		out->status = OFD_STATUS_LIMIT;
		out->retryAfter = CONTENT_RETRY_LIMIT;
		return;
	}
	f->fp = NULL;
	if ( ospath ) {
		// a pak changed since it was hashed is hashed again
		if ( !Content_FileStat( ospath, &nowSize, &nowMtime ) || nowSize != size || nowMtime != mtime ) {
			if ( !svHashQueued ) {
				svHashQueued = qtrue;
				FS_HashPaks();
			}
			out->status = OFD_STATUS_READ_ERROR;
			return;
		}
		if ( !( f->fp = Sys_FOpen( ospath, "rb" ) ) ) {
			out->status = OFD_STATUS_READ_ERROR;
			return;
		}
		Com_Printf( "Fast download: client %d gets %s from byte %u\n", (int)( cl - svs.clients ),
			strrchr( ospath, PATH_SEP ) ? strrchr( ospath, PATH_SEP ) + 1 : ospath, request->offset );
	}
	f->used = qtrue;
	f->blockStart = -1;
	Com_Memcpy( f->sha256, out->sha256, SHA256_DIGEST_SIZE );
	out->status = OFD_STATUS_OK;
	out->fileSize = (uint32_t)size;
	out->file = f;
}

/*
=================
SV_ContentCloseGone

After a content manifest change: the transfers of files neither kept
content manifest lists get CLOSE reason 2
=================
*/
static void SV_ContentCloseGone( void ) {
	svTransferEnv_t	env;
	svtTransfer_t	*transfer;
	client_t		*cl;
	int64_t			size, mtime;
	int				i, j;

	if ( !svs.clients ) {
		return;
	}
	for ( i = 0, cl = svs.clients; i < sv_maxclients->integer; i++, cl++ ) {
		if ( !cl->content ) {
			continue;
		}
		env = SV_ContentEnv( cl );
		for ( j = 0; j < SVT_TRANSFERS; j++ ) {
			transfer = &cl->content->t.transfers[j];
			if ( transfer->serial && !( transfer->kind == OFD_KIND_MANIFEST
				? SV_ContentFindManifest( transfer->sha256 ) != NULL
				: SV_ContentFindPak( transfer->sha256, &size, &mtime ) != NULL ) ) {
				SVT_Close( &cl->content->t, transfer, OFD_CLOSE_GONE, SV_ContentNow(), &env );
			}
		}
	}
}

/*
=================
SV_ContentBad

A datagram that can't be read, or isn't a CONTROL: counted against the
client its address and key name, which is dropped at 16
=================
*/
static void SV_ContentBad( netadr_t from, const msg_t *msg ) {
	client_t	*cl;
	uint64_t	key = 0;
	int			i;

	for ( i = 0; i < 8; i++ ) {
		key |= (uint64_t)msg->data[12 + i] << ( 8 * i );
	}
	for ( i = 0, cl = svs.clients; i < sv_maxclients->integer; i++, cl++ ) {
		if ( cl->state >= CS_CONNECTED && ( cl->ofx & OFX_CONTENT ) && cl->contentKey == key
			&& NET_CompareAdr( from, cl->netchan.remoteAddress ) ) {
			if ( ++cl->contentBadDatagrams >= CONTENT_BAD_DATAGRAMS ) {
				SV_DropClient( cl, "bad content control" );
			}
			return;
		}
	}
	svContentIgnored++;
}

/*
=================
SV_ContentPacket

A content datagram (OFD_IsContent). Only a CONTROL comes this way; one
from a client that negotiated the content protocol, from its address
and with its key, is read, and anything else is dropped
=================
*/
void SV_ContentPacket( netadr_t from, msg_t *msg ) {
	ofdMessage_t	m;
	svTransferEnv_t	env;
	svtResult_t		result;
	client_t		*cl;

	if ( !OFD_Read( msg->data, msg->cursize, &m ) || m.type != OFD_TYPE_CONTROL ) {
		SV_ContentBad( from, msg );
		return;
	}
	if ( m.u.control.slot >= sv_maxclients->integer ) {
		svContentIgnored++;
		return;
	}
	cl = &svs.clients[m.u.control.slot];
	if ( !( cl->ofx & OFX_CONTENT ) || cl->state < CS_CONNECTED || m.key != cl->contentKey
		|| !NET_CompareAdr( from, cl->netchan.remoteAddress ) ) {
		svContentIgnored++;
		return;
	}

	env = SV_ContentEnv( cl );
	if ( !cl->content ) {
		cl->content = Z_Malloc( sizeof( *cl->content ) );
		SVT_Init( &cl->content->t, cl->contentKey, &env );
	}
	result = SVT_Control( &cl->content->t, &m.u.control, cl->state == CS_ACTIVE, SV_ContentNow(), &env );
	if ( result.drop ) {
		SV_DropClient( cl, result.drop );
		return;
	}
	// server commands wouldn't reach the client until it's done, so
	// they aren't queued; donedl sends a new gamestate, as for a classic
	// download
	if ( result.loadingPakOpened && cl->state == CS_PRIMED ) {
		Com_DPrintf( "Going to CS_CONNECTED for %s\n", cl->name );
		cl->state = CS_CONNECTED;
	}
}

/*
=================
SV_ContentFreeClient
=================
*/
void SV_ContentFreeClient( client_t *cl ) {
	svTransferEnv_t	env;

	if ( !cl->content ) {
		return;
	}
	env = SV_ContentEnv( cl );
	SVT_Free( &cl->content->t, &env );
	Z_Free( cl->content );
	cl->content = NULL;
}

/*
=================
SV_ContentReport

sv_showContent 1: once a second, each downloading client's datagrams,
rate and round trip, and the datagrams no client was charged with
=================
*/
static void SV_ContentReport( int64_t now ) {
	const client_t	*cl;
	int				i;

	if ( !sv_showContent->integer || now < svContentNextReport ) {
		return;
	}
	svContentNextReport = now + 1000000;
	for ( i = 0, cl = svs.clients; i < sv_maxclients->integer; i++, cl++ ) {
		const svTransfers_t	*t;

		if ( !SV_ContentTransfers( cl ) ) {
			continue;
		}
		t = &cl->content->t;
		Com_Printf( "content %d: %u datagrams, %u again, %lld KB/s, window %lld KB, rtt %lld ms, %d bad%s\n",
			i, t->datagramsSent, t->datagramsResent, (long long)( DLCC_PacingRate( &t->cc ) / 1024 ),
			(long long)( t->cc.cwnd / 1024 ), (long long)( t->cc.srtt / 1000 ), cl->contentBadDatagrams,
			SVT_Loading( t ) ? ", loading" : "" );
	}
	if ( svContentIgnored ) {
		Com_Printf( "content: %d datagrams ignored\n", svContentIgnored );
		svContentIgnored = 0;
	}
}

/*
=================
SV_ContentSend

Between frames (section 8.2): the clients that can send, loading ones
first, a datagram each a pass, under sv_dlMaxRate, until 64 have gone,
1 ms has passed, or the socket is full. The milliseconds until one can
send again, 0 for now, INT_MAX for none
=================
*/
int SV_ContentSend( void ) {
	int64_t		start = Sys_Nanoseconds(), now = start / 1000, wait = -1, w;
	int			i, k, pass, sent = 0, loading = 0, bytes;
	client_t	*cl;
	svTransferEnv_t	env;
	qboolean	progress, capFree;

	if ( !svs.clients ) {
		return INT_MAX;
	}
	if ( svContentCapRate != sv_dlMaxRate->integer ) {
		svContentCapRate = sv_dlMaxRate->integer;
		DLCC_BucketInit( &svContentCap, (int64_t)MAX( svContentCapRate, 0 ) * 1024, now );
	}

	// the probe timeouts and stalls, then the loading clients' floor
	for ( i = 0, cl = svs.clients; i < sv_maxclients->integer; i++, cl++ ) {
		if ( !cl->content ) {
			continue;
		}
		env = SV_ContentEnv( cl );
		if ( ( w = SVT_Timer( &cl->content->t, now, &env ) ) >= 0 ) {
			wait = wait < 0 ? w : MIN( wait, w );
		}
		loading += SVT_Loading( &cl->content->t );
	}
	for ( i = 0, cl = svs.clients; i < sv_maxclients->integer; i++, cl++ ) {
		if ( cl->content && cl->content->t.ccStarted && SVT_Loading( &cl->content->t ) ) {
			DLCC_SetFloorRate( &cl->content->t.cc, CONTENT_FLOOR / loading );
		}
	}

	svContentSocketFull = qfalse;
	do {
		progress = qfalse;
		for ( pass = 0; pass < 2; pass++ ) {
			for ( k = 0; k < sv_maxclients->integer && sent < CONTENT_SEND_MAX; k++ ) {
				cl = &svs.clients[( svContentNextClient + k ) % sv_maxclients->integer];
				if ( !cl->content || SVT_Loading( &cl->content->t ) != !pass ) {
					continue;
				}
				if ( ( w = SVT_SendWait( &cl->content->t, now ) ) != 0 ) {
					if ( w > 0 ) {
						wait = wait < 0 ? w : MIN( wait, w );
					}
					continue;
				}
				if ( svContentCapRate > 0 ) {
					if ( ( w = DLCC_BucketWait( &svContentCap, now, OFD_MAX_DATAGRAM ) ) > 0 ) {
						wait = wait < 0 ? w : MIN( wait, w );
						goto done;
					}
					capFree = DLCC_BucketWait( &svContentCap, now, 2 * OFD_MAX_DATAGRAM ) == 0;
				} else {
					capFree = qtrue;
				}
				env = SV_ContentEnv( cl );
				if ( !( bytes = SVT_Send( &cl->content->t, now, capFree, &env ) ) ) {
					if ( svContentSocketFull ) {
						wait = 1000;
						goto done;
					}
					continue;
				}
				if ( svContentCapRate > 0 ) {
					DLCC_BucketTake( &svContentCap, bytes );
				}
				sent++;
				progress = qtrue;
			}
		}
		now = Sys_Nanoseconds() / 1000;
	} while ( progress && sent < CONTENT_SEND_MAX && ( sent % 8 || Sys_Nanoseconds() - start < CONTENT_SEND_TIME ) );
	if ( progress ) {
		wait = 0;	// stopped by the limits, with more to send
	}
done:
	svContentNextClient = ( svContentNextClient + 1 ) % sv_maxclients->integer;
	SV_ContentReport( now );
	if ( wait < 0 ) {
		return INT_MAX;
	}
	return (int)( ( wait + 999 ) / 1000 );
}
