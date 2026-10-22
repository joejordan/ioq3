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
// cl_scrn.c -- master for refresh, status bar, console, chat, notify, etc

#include "client.h"

qboolean	scr_initialized;		// ready to draw

cvar_t		*cl_timegraph;
cvar_t		*cl_debuggraph;
cvar_t		*cl_graphheight;
cvar_t		*cl_graphscale;
cvar_t		*cl_graphshift;
cvar_t		*cl_batchText;		// spike: the console's text in batches of triangles
cvar_t		*cl_test2D;			// spike: draws a test of the renderer's 2D triangles

/*
================
SCR_DrawNamedPic

Coordinates are 640*480 virtual values
=================
*/
void SCR_DrawNamedPic( float x, float y, float width, float height, const char *picname ) {
	qhandle_t	hShader;

	assert( width != 0 );

	hShader = re.RegisterShader( picname );
	SCR_AdjustFrom640( &x, &y, &width, &height );
	re.DrawStretchPic( x, y, width, height, 0, 0, 1, 1, hShader );
}


// where SCR_AdjustFrom640 puts what's drawn next; when not stretching, the
// window's safe area it places against and the scale 640x480 fits it at
static screenPlacement_t	scr_placeX = PLACE_STRETCH, scr_placeY = PLACE_STRETCH;
static int					scr_area[4];
static float				scr_fitScale;

/*
================
SCR_SetPlacement

Sets how SCR_AdjustFrom640 maps 640x480 to the window on each axis:
stretched over it, the default, as the console and cinematics want; or at
one scale on both axes, so text and pictures keep their shape, against an
edge of the window's safe area or centered in it
================
*/
void SCR_SetPlacement( screenPlacement_t horizontal, screenPlacement_t vertical ) {
	scr_placeX = horizontal;
	scr_placeY = vertical;
	if ( horizontal != PLACE_STRETCH || vertical != PLACE_STRETCH ) {
		CL_GetSafeArea( scr_area );
		scr_fitScale = MIN( scr_area[2] / (float)SCREEN_WIDTH, scr_area[3] / (float)SCREEN_HEIGHT );
	}
}

/*
================
SCR_PlaceAxis

One axis of SCR_AdjustFrom640: the scale and offset from 640x480 units to a
window side of windowSize pixels, whose safe part starts at start and is
size long
================
*/
static void SCR_PlaceAxis( screenPlacement_t place, int windowSize, int start, int size, int virtualSize,
	float *scale, float *bias ) {
	if ( place == PLACE_STRETCH ) {
		*scale = windowSize / (float)virtualSize;
		*bias = 0.0f;
		return;
	}

	*scale = scr_fitScale;
	*bias = start;
	if ( place == PLACE_CENTER ) {
		*bias += 0.5f * ( size - virtualSize * scr_fitScale );
	} else if ( place == PLACE_RIGHT ) {
		*bias += size - virtualSize * scr_fitScale;
	}
}

/*
================
SCR_AdjustFrom640

From the 640x480 virtual screen to the window's pixels, as SCR_SetPlacement
says
================
*/
void SCR_AdjustFrom640( float *x, float *y, float *w, float *h ) {
	float	xscale, yscale, xbias, ybias;

	SCR_PlaceAxis( scr_placeX, cls.glconfig.vidWidth, scr_area[0], scr_area[2], SCREEN_WIDTH, &xscale, &xbias );
	SCR_PlaceAxis( scr_placeY, cls.glconfig.vidHeight, scr_area[1], scr_area[3], SCREEN_HEIGHT, &yscale, &ybias );

	if ( x ) {
		*x = *x * xscale + xbias;
	}
	if ( y ) {
		*y = *y * yscale + ybias;
	}
	if ( w ) {
		*w *= xscale;
	}
	if ( h ) {
		*h *= yscale;
	}
}

/*
================
SCR_FillRect

Coordinates are 640*480 virtual values
=================
*/
void SCR_FillRect( float x, float y, float width, float height, const float *color ) {
	re.SetColor( color );

	SCR_AdjustFrom640( &x, &y, &width, &height );
	re.DrawStretchPic( x, y, width, height, 0, 0, 0, 0, cls.whiteShader );

	re.SetColor( NULL );
}


/*
================
SCR_DrawPic

Coordinates are 640*480 virtual values
=================
*/
void SCR_DrawPic( float x, float y, float width, float height, qhandle_t hShader ) {
	SCR_AdjustFrom640( &x, &y, &width, &height );
	re.DrawStretchPic( x, y, width, height, 0, 0, 1, 1, hShader );
}



/*
** SCR_DrawChar
** chars are drawn at 640*480 virtual screen size
*/
static void SCR_DrawChar( int x, int y, float size, int ch ) {
	int row, col;
	float frow, fcol;
	float	ax, ay, aw, ah;

	ch &= 255;

	if ( ch == ' ' ) {
		return;
	}

	if ( y < -size ) {
		return;
	}

	ax = x;
	ay = y;
	aw = size;
	ah = size;
	SCR_AdjustFrom640( &ax, &ay, &aw, &ah );

	row = ch>>4;
	col = ch&15;

	frow = row*0.0625;
	fcol = col*0.0625;
	size = 0.0625;

	re.DrawStretchPic( ax, ay, aw, ah,
					   fcol, frow, 
					   fcol + size, frow + size, 
					   cls.charSetShader );
}

/*
==============================================================================

TEXT BATCHES (spike)

With cl_batchText 1, the small characters drawn between SCR_BeginTextBatch
and SCR_EndTextBatch go to the renderer as triangles in batches of up to
SCR_TEXT_BATCH characters, each with the color SCR_SetTextColor last gave,
rather than as a picture and a color command each; with 2, as quads,
without indexes (TRIS2D_QUADS).

==============================================================================
*/

#define SCR_TEXT_BATCH	2048

static polyVert2D_t	scr_textVerts[SCR_TEXT_BATCH * 4];
static int			scr_textIndexes[SCR_TEXT_BATCH * 6];
static int			scr_textChars;
static qboolean		scr_textBatching;
static byte			scr_textColor[4];
static int			scr_smallChars;		// r_stats2D: small characters drawn

static void SCR_FlushTextBatch( void ) {
	tris2D_t	tris;

	if ( !scr_textChars ) {
		return;
	}

	Com_Memset( &tris, 0, sizeof( tris ) );
	tris.hShader = cls.charSetShader;
	tris.verts = scr_textVerts;
	tris.numVerts = scr_textChars * 4;
	if ( cl_batchText->integer == 2 ) {
		tris.flags = TRIS2D_QUADS;
	} else {
		tris.indexes = scr_textIndexes;
		tris.numIndexes = scr_textChars * 6;
	}
	re.DrawTriangles2D( &tris );

	scr_textChars = 0;
}

void SCR_BeginTextBatch( void ) {
	int		i;

	scr_textBatching = cl_batchText->integer && re.DrawTriangles2D;
	scr_textChars = 0;
	Com_Memset( scr_textColor, 255, sizeof( scr_textColor ) );

	// the quads' corners in the order RB_StretchPic gives them
	if ( scr_textBatching && !scr_textIndexes[1] ) {
		for ( i = 0; i < SCR_TEXT_BATCH; i++ ) {
			scr_textIndexes[i * 6 + 0] = i * 4 + 3;
			scr_textIndexes[i * 6 + 1] = i * 4 + 0;
			scr_textIndexes[i * 6 + 2] = i * 4 + 2;
			scr_textIndexes[i * 6 + 3] = i * 4 + 2;
			scr_textIndexes[i * 6 + 4] = i * 4 + 0;
			scr_textIndexes[i * 6 + 5] = i * 4 + 1;
		}
	}
}

void SCR_EndTextBatch( void ) {
	SCR_FlushTextBatch();
	scr_textBatching = qfalse;
}

// as re.SetColor, for the characters of a batch as well; NULL is white
void SCR_SetTextColor( const float *rgba ) {
	int		i;

	if ( !scr_textBatching ) {
		re.SetColor( rgba );
		return;
	}

	// as the back end converts a color
	for ( i = 0; i < 4; i++ ) {
		scr_textColor[i] = rgba ? (byte)( rgba[i] * 255 ) : 255;
	}
}

static void SCR_AddTextQuad( float x, float y, float w, float h, float s1, float t1, float s2, float t2 ) {
	polyVert2D_t	*v;
	int		i;

	if ( scr_textChars == SCR_TEXT_BATCH ) {
		SCR_FlushTextBatch();
	}

	v = &scr_textVerts[scr_textChars * 4];
	v[0].xy[0] = x;		v[0].xy[1] = y;		v[0].st[0] = s1;	v[0].st[1] = t1;
	v[1].xy[0] = x + w;	v[1].xy[1] = y;		v[1].st[0] = s2;	v[1].st[1] = t1;
	v[2].xy[0] = x + w;	v[2].xy[1] = y + h;	v[2].st[0] = s2;	v[2].st[1] = t2;
	v[3].xy[0] = x;		v[3].xy[1] = y + h;	v[3].st[0] = s1;	v[3].st[1] = t2;
	for ( i = 0; i < 4; i++ ) {
		Byte4Copy( scr_textColor, v[i].modulate );
	}

	scr_textChars++;
}

/*
** SCR_DrawSmallChar
** small chars are drawn at native screen resolution
*/
void SCR_DrawSmallChar( int x, int y, int ch ) {
	int row, col;
	float frow, fcol;
	float size;

	ch &= 255;

	if ( ch == ' ' ) {
		return;
	}

	if ( y < -g_smallchar_height ) {
		return;
	}

	row = ch>>4;
	col = ch&15;

	frow = row*0.0625;
	fcol = col*0.0625;
	size = 0.0625;

	scr_smallChars++;
	if ( scr_textBatching ) {
		SCR_AddTextQuad( x, y, g_smallchar_width, g_smallchar_height, fcol, frow, fcol + size, frow + size );
		return;
	}

	re.DrawStretchPic( x, y, g_smallchar_width, g_smallchar_height,
					   fcol, frow,
					   fcol + size, frow + size,
					   cls.charSetShader );
}


/*
==================
SCR_DrawBigString[Color]

Draws a multi-colored string with a drop shadow, optionally forcing
to a fixed color.

Coordinates are at 640 by 480 virtual resolution
==================
*/
void SCR_DrawStringExt( int x, int y, float size, const char *string, float *setColor, qboolean forceColor,
		qboolean noColorEscape ) {
	vec4_t		color;
	const char	*s;
	int			xx;

	// draw the drop shadow
	color[0] = color[1] = color[2] = 0;
	color[3] = setColor[3];
	re.SetColor( color );
	s = string;
	xx = x;
	while ( *s ) {
		if ( !noColorEscape && Q_IsColorString( s ) ) {
			s += 2;
			continue;
		}
		SCR_DrawChar( xx+2, y+2, size, *s );
		xx += size;
		s++;
	}


	// draw the colored text
	s = string;
	xx = x;
	re.SetColor( setColor );
	while ( *s ) {
		if ( Q_IsColorString( s ) ) {
			if ( !forceColor ) {
				Com_Memcpy( color, g_color_table[ColorIndex(*(s+1))], sizeof( color ) );
				color[3] = setColor[3];
				re.SetColor( color );
			}
			if ( !noColorEscape ) {
				s += 2;
				continue;
			}
		}
		SCR_DrawChar( xx, y, size, *s );
		xx += size;
		s++;
	}
	re.SetColor( NULL );
}


void SCR_DrawBigString( int x, int y, const char *s, float alpha, qboolean noColorEscape ) {
	float	color[4];

	color[0] = color[1] = color[2] = 1.0;
	color[3] = alpha;
	SCR_DrawStringExt( x, y, BIGCHAR_WIDTH, s, color, qfalse, noColorEscape );
}

void SCR_DrawBigStringColor( int x, int y, const char *s, vec4_t color, qboolean noColorEscape ) {
	SCR_DrawStringExt( x, y, BIGCHAR_WIDTH, s, color, qtrue, noColorEscape );
}


/*
==================
SCR_DrawSmallString[Color]

Draws a multi-colored string with a drop shadow, optionally forcing
to a fixed color.
==================
*/
void SCR_DrawSmallStringExt( int x, int y, const char *string, float *setColor, qboolean forceColor,
		qboolean noColorEscape ) {
	vec4_t		color;
	const char	*s;
	int			xx;

	// draw the colored text
	s = string;
	xx = x;
	SCR_SetTextColor( setColor );
	while ( *s ) {
		if ( Q_IsColorString( s ) ) {
			if ( !forceColor ) {
				Com_Memcpy( color, g_color_table[ColorIndex(*(s+1))], sizeof( color ) );
				color[3] = setColor[3];
				SCR_SetTextColor( color );
			}
			if ( !noColorEscape ) {
				s += 2;
				continue;
			}
		}
		SCR_DrawSmallChar( xx, y, *s );
		xx += g_smallchar_width;
		s++;
	}
	SCR_SetTextColor( NULL );
}



/*
** SCR_Strlen -- skips color escape codes
*/
static int SCR_Strlen( const char *str ) {
	const char *s = str;
	int count = 0;

	while ( *s ) {
		if ( Q_IsColorString( s ) ) {
			s += 2;
		} else {
			count++;
			s++;
		}
	}

	return count;
}

/*
** SCR_GetBigStringWidth
*/ 
int	SCR_GetBigStringWidth( const char *str ) {
	return SCR_Strlen( str ) * BIGCHAR_WIDTH;
}


//===============================================================================

/*
=================
SCR_DrawDemoRecording
=================
*/
void SCR_DrawDemoRecording( void ) {
	char	string[1024];
	int		pos;

	if ( !clc.demorecording ) {
		return;
	}
	if ( clc.spDemoRecording ) {
		return;
	}

	pos = FS_FTell( clc.demofile );
	sprintf( string, "RECORDING %s: %ik", clc.demoName, pos / 1024 );

	SCR_SetPlacement( PLACE_CENTER, PLACE_TOP );
	SCR_DrawStringExt( 320 - strlen( string ) * 4, 20, 8, string, g_color_table[7], qtrue, qfalse );
	SCR_SetPlacement( PLACE_STRETCH, PLACE_STRETCH );
}


#ifdef USE_VOIP
/*
=================
SCR_DrawVoipMeter
=================
*/
void SCR_DrawVoipMeter( void ) {
	char	buffer[16];
	char	string[256];
	int limit, i;

	if (!cl_voipShowMeter->integer)
		return;  // player doesn't want to show meter at all.
	else if (!cl_voipSend->integer)
		return;  // not recording at the moment.
	else if (clc.state != CA_ACTIVE)
		return;  // not connected to a server.
	else if (!clc.voipEnabled)
		return;  // server doesn't support VoIP.
	else if (clc.demoplaying)
		return;  // playing back a demo.
	else if (!CL_VoipOn())
		return;  // client has VoIP support disabled.

	limit = (int) (clc.voipPower * 10.0f);
	if (limit > 10)
		limit = 10;

	for (i = 0; i < limit; i++)
		buffer[i] = '*';
	while (i < 10)
		buffer[i++] = ' ';
	buffer[i] = '\0';

	sprintf( string, "VoIP: [%s]", buffer );
	SCR_SetPlacement( PLACE_CENTER, PLACE_TOP );
	SCR_DrawStringExt( 320 - strlen( string ) * 4, 10, 8, string, g_color_table[7], qtrue, qfalse );
	SCR_SetPlacement( PLACE_STRETCH, PLACE_STRETCH );
}
#endif




/*
===============================================================================

DEBUG GRAPH

===============================================================================
*/

static	int			current;
static	float		values[1024];

/*
==============
SCR_DebugGraph
==============
*/
void SCR_DebugGraph (float value)
{
	values[current] = value;
	current = (current + 1) % ARRAY_LEN(values);
}

/*
==============
SCR_DrawDebugGraph
==============
*/
void SCR_DrawDebugGraph (void)
{
	int		a, x, y, w, i, h;
	float	v;

	//
	// draw the graph
	//
	w = cls.glconfig.vidWidth;
	x = 0;
	y = cls.glconfig.vidHeight;
	re.SetColor( g_color_table[0] );
	re.DrawStretchPic(x, y - cl_graphheight->integer, 
		w, cl_graphheight->integer, 0, 0, 0, 0, cls.whiteShader );
	re.SetColor( NULL );

	for (a=0 ; a<w ; a++)
	{
		i = (ARRAY_LEN(values)+current-1-(a % ARRAY_LEN(values))) % ARRAY_LEN(values);
		v = values[i];
		v = v * cl_graphscale->integer + cl_graphshift->integer;
		
		if (v < 0)
			v += cl_graphheight->integer * (1+(int)(-v / cl_graphheight->integer));
		h = (int)v % cl_graphheight->integer;
		re.DrawStretchPic( x+w-1-a, y - h, 1, h, 0, 0, 0, 0, cls.whiteShader );
	}
}

/*
==============================================================================

2D TRIANGLES TEST (spike)

cl_test2D 1 draws, over everything but the console, at 640x480 stretched to
the window:
- a vertical gradient over the whole window, one quad;
- two overlapping translucent rounded rectangles, triangle fans with a
  gradient across their vertexes;
- a quad with a different color at each corner, scissored to the rectangle
  (420, 50, 180, 120), with a white frame drawn around that rectangle
  outside the scissor;
- a 64x64 image of 16 colored squares, filled one square a frame, drawn
  twice: at (400, 260) before its top left square is made red, and at
  (500, 260) after, then made green again for the next frame, so the left
  square's corner is green and the right's red when the updates keep their
  places among the draws;
- the same with a 256x320 image, too large for the command buffer, blue
  before and yellow after, at (400, 370) and (500, 370);
- the small image on a quad turned 30 degrees about (130, 400);
- "OMNIFRAG" in the console's font, turned -10 degrees about (240, 440),
  white at the top and orange at the bottom.

==============================================================================
*/

static qhandle_t	scr_testImage, scr_testBigImage;
static int			scr_testFrame;

// call when the renderer starts, which forgets the images it made
void SCR_RendererStarted( void ) {
	scr_testImage = scr_testBigImage = 0;
	scr_testFrame = 0;
}

static void SCR_TestVert( polyVert2D_t *v, float x, float y, float s, float t, int r, int g, int b, int a ) {
	v->xy[0] = x * cls.glconfig.vidWidth / 640.0f;
	v->xy[1] = y * cls.glconfig.vidHeight / 480.0f;
	v->st[0] = s;
	v->st[1] = t;
	v->modulate[0] = r;
	v->modulate[1] = g;
	v->modulate[2] = b;
	v->modulate[3] = a;
}

// a quad from 640x480 units: corners top left, top right, bottom right,
// bottom left, each in its color
static void SCR_TestQuad( qhandle_t shader, float x, float y, float w, float h,
		const byte *c0, const byte *c1, const byte *c2, const byte *c3, int flags, const int *scissor, const float *transform ) {
	static const int quad[6] = { 0, 1, 2, 0, 2, 3 };
	polyVert2D_t	v[4];
	tris2D_t		tris;

	SCR_TestVert( &v[0], x, y, 0, 0, c0[0], c0[1], c0[2], c0[3] );
	SCR_TestVert( &v[1], x + w, y, 1, 0, c1[0], c1[1], c1[2], c1[3] );
	SCR_TestVert( &v[2], x + w, y + h, 1, 1, c2[0], c2[1], c2[2], c2[3] );
	SCR_TestVert( &v[3], x, y + h, 0, 1, c3[0], c3[1], c3[2], c3[3] );

	Com_Memset( &tris, 0, sizeof( tris ) );
	tris.hShader = shader;
	tris.verts = v;
	tris.numVerts = 4;
	tris.indexes = quad;
	tris.numIndexes = 6;
	tris.flags = flags;
	if ( scissor ) {
		Com_Memcpy( tris.scissor, scissor, sizeof( tris.scissor ) );
	}
	if ( transform ) {
		Com_Memcpy( tris.transform, transform, sizeof( tris.transform ) );
	}
	re.DrawTriangles2D( &tris );
}

// a rounded rectangle as a fan about its middle, top color to bottom color
static void SCR_TestRoundedRect( float x, float y, float w, float h, float radius,
		const byte *top, const byte *bottom, const byte *left, const byte *right ) {
	enum { SEGMENTS = 8, EDGE = 4 * ( SEGMENTS + 1 ) };
	polyVert2D_t	v[EDGE + 1];
	int				indexes[EDGE * 3];
	tris2D_t		tris;
	int				corner, i, n = 1, c[4];

	// the middle, between all four
	for ( i = 0; i < 4; i++ ) {
		c[i] = ( top[i] + bottom[i] + left[i] + right[i] ) / 4;
	}
	SCR_TestVert( &v[0], x + w / 2, y + h / 2, 0.5f, 0.5f, c[0], c[1], c[2], c[3] );

	// the corners clockwise from the top right, as y goes down
	for ( corner = 0; corner < 4; corner++ ) {
		float cx = ( corner == 0 || corner == 1 ) ? x + w - radius : x + radius;
		float cy = ( corner == 1 || corner == 2 ) ? y + h - radius : y + radius;
		float start = ( corner - 1 ) * M_PI / 2;

		for ( i = 0; i <= SEGMENTS; i++, n++ ) {
			float angle = start + i * ( M_PI / 2 ) / SEGMENTS;
			float px = cx + cos( angle ) * radius, py = cy + sin( angle ) * radius;
			float fy = ( py - y ) / h, fx = ( px - x ) / w;
			int k;

			// a blend of the vertical and the horizontal gradients
			for ( k = 0; k < 4; k++ ) {
				c[k] = ( top[k] * ( 1 - fy ) + bottom[k] * fy + left[k] * ( 1 - fx ) + right[k] * fx ) / 2;
			}
			SCR_TestVert( &v[n], px, py, fx, fy, c[0], c[1], c[2], c[3] );
		}
	}

	for ( i = 0; i < EDGE; i++ ) {
		indexes[i * 3 + 0] = 0;
		indexes[i * 3 + 1] = 1 + i;
		indexes[i * 3 + 2] = 1 + ( i + 1 ) % EDGE;
	}

	Com_Memset( &tris, 0, sizeof( tris ) );
	tris.hShader = cls.whiteShader;
	tris.verts = v;
	tris.numVerts = EDGE + 1;
	tris.indexes = indexes;
	tris.numIndexes = EDGE * 3;
	re.DrawTriangles2D( &tris );
}

// a transform turning by degrees about (x, y) in 640x480 units
static void SCR_TestTurn( float *m, float degrees, float x, float y ) {
	float a = DEG2RAD( degrees ), c = cos( a ), s = sin( a );

	x *= cls.glconfig.vidWidth / 640.0f;
	y *= cls.glconfig.vidHeight / 480.0f;
	m[0] = c;
	m[1] = s;
	m[2] = -s;
	m[3] = c;
	m[4] = x - c * x + s * y;
	m[5] = y - s * x - c * y;
}

static void SCR_FillTestPixels( byte *rgba, int pixels, int r, int g, int b ) {
	int		i;

	for ( i = 0; i < pixels; i++ ) {
		rgba[i * 4 + 0] = r;
		rgba[i * 4 + 1] = g;
		rgba[i * 4 + 2] = b;
		rgba[i * 4 + 3] = 255;
	}
}

static void SCR_DrawTest2D( void ) {
	static const byte black[4] = { 0, 0, 0, 255 }, navy[4] = { 20, 30, 90, 255 };
	static const byte red[4] = { 255, 40, 40, 200 }, yellow[4] = { 255, 230, 40, 200 };
	static const byte green[4] = { 40, 220, 80, 160 }, blue[4] = { 40, 80, 255, 160 };
	static const byte white[4] = { 255, 255, 255, 255 }, orange[4] = { 255, 140, 0, 255 };
	static const byte c0[4] = { 255, 0, 0, 255 }, c1[4] = { 0, 255, 0, 255 };
	static const byte c2[4] = { 0, 0, 255, 255 }, c3[4] = { 255, 255, 0, 255 };
	static const char *word = "OMNIFRAG";
	static byte	square[16 * 16 * 4];
	static byte	*big;
	const int	bigWidth = 256, bigHeight = 320;
	float	sx = cls.glconfig.vidWidth / 640.0f, sy = cls.glconfig.vidHeight / 480.0f;
	float	turn[6];
	int		scissor[4];
	int		i;

	if ( !re.DrawTriangles2D ) {
		return;
	}

	if ( !scr_testImage ) {
		scr_testImage = re.CreateImage2D( "*test2D", 64, 64, NULL );
		scr_testBigImage = re.CreateImage2D( "*test2Dbig", bigWidth, bigHeight, NULL );
		if ( !big ) {
			big = Z_Malloc( bigWidth * bigHeight * 4 );
		}
	}

	// the image's squares, one a frame
	if ( scr_testFrame < 16 ) {
		int n = scr_testFrame;

		SCR_FillTestPixels( square, 16 * 16, ( n * 53 ) & 255, ( n * 97 + 60 ) & 255, ( n * 31 + 120 ) & 255 );
		re.UpdateImage2D( scr_testImage, ( n & 3 ) * 16, ( n >> 2 ) * 16, 16, 16, square );
	}
	scr_testFrame++;

	SCR_TestQuad( cls.whiteShader, 0, 0, 640, 480, navy, navy, black, black, 0, NULL, NULL );

	SCR_TestRoundedRect( 40, 60, 240, 160, 30, red, yellow, red, yellow );
	SCR_TestRoundedRect( 120, 140, 220, 150, 40, green, green, green, blue );

	// the scissor in window pixels, the frame outside it
	scissor[0] = 420 * sx;
	scissor[1] = 50 * sy;
	scissor[2] = 180 * sx;
	scissor[3] = 120 * sy;
	SCR_TestQuad( cls.whiteShader, 380, 20, 260, 200, c0, c1, c2, c3, TRIS2D_SCISSOR, scissor, NULL );
	SCR_TestQuad( cls.whiteShader, 416, 46, 188, 2, white, white, white, white, 0, NULL, NULL );
	SCR_TestQuad( cls.whiteShader, 416, 172, 188, 2, white, white, white, white, 0, NULL, NULL );
	SCR_TestQuad( cls.whiteShader, 416, 46, 2, 128, white, white, white, white, 0, NULL, NULL );
	SCR_TestQuad( cls.whiteShader, 602, 46, 2, 128, white, white, white, white, 0, NULL, NULL );

	// updates in their places among the draws
	SCR_TestQuad( scr_testImage, 400, 260, 80, 80, white, white, white, white, 0, NULL, NULL );
	SCR_FillTestPixels( square, 16 * 16, 255, 0, 0 );
	re.UpdateImage2D( scr_testImage, 0, 0, 16, 16, square );
	SCR_TestQuad( scr_testImage, 500, 260, 80, 80, white, white, white, white, 0, NULL, NULL );
	SCR_FillTestPixels( square, 16 * 16, 0, 255, 0 );
	re.UpdateImage2D( scr_testImage, 0, 0, 16, 16, square );

	SCR_FillTestPixels( big, bigWidth * bigHeight, 0, 0, 255 );
	re.UpdateImage2D( scr_testBigImage, 0, 0, bigWidth, bigHeight, big );
	SCR_TestQuad( scr_testBigImage, 400, 370, 60, 60, white, white, white, white, 0, NULL, NULL );
	SCR_FillTestPixels( big, bigWidth * bigHeight, 255, 255, 0 );
	re.UpdateImage2D( scr_testBigImage, 0, 0, bigWidth, bigHeight, big );
	SCR_TestQuad( scr_testBigImage, 500, 370, 60, 60, white, white, white, white, 0, NULL, NULL );

	SCR_TestTurn( turn, 30, 130, 400 );
	SCR_TestQuad( scr_testImage, 85, 355, 90, 90, white, white, white, white, TRIS2D_TRANSFORM, NULL, turn );

	// a word, one batch of eight quads, turned
	{
		polyVert2D_t	v[8 * 4];
		int				indexes[8 * 6];
		tris2D_t		tris;
		float			x = 176, y = 424, size = 16;

		for ( i = 0; i < 8; i++ ) {
			int ch = word[i];
			float s = ( ch & 15 ) * 0.0625f, t = ( ch >> 4 ) * 0.0625f;

			SCR_TestVert( &v[i * 4 + 0], x + i * size, y, s, t, white[0], white[1], white[2], 255 );
			SCR_TestVert( &v[i * 4 + 1], x + ( i + 1 ) * size, y, s + 0.0625f, t, white[0], white[1], white[2], 255 );
			SCR_TestVert( &v[i * 4 + 2], x + ( i + 1 ) * size, y + size, s + 0.0625f, t + 0.0625f, orange[0], orange[1], orange[2], 255 );
			SCR_TestVert( &v[i * 4 + 3], x + i * size, y + size, s, t + 0.0625f, orange[0], orange[1], orange[2], 255 );
			indexes[i * 6 + 0] = i * 4 + 0;
			indexes[i * 6 + 1] = i * 4 + 1;
			indexes[i * 6 + 2] = i * 4 + 2;
			indexes[i * 6 + 3] = i * 4 + 0;
			indexes[i * 6 + 4] = i * 4 + 2;
			indexes[i * 6 + 5] = i * 4 + 3;
		}

		SCR_TestTurn( turn, -10, 240, 440 );
		Com_Memset( &tris, 0, sizeof( tris ) );
		tris.hShader = cls.charSetShader;
		tris.verts = v;
		tris.numVerts = 8 * 4;
		tris.indexes = indexes;
		tris.numIndexes = 8 * 6;
		tris.flags = TRIS2D_TRANSFORM;
		Com_Memcpy( tris.transform, turn, sizeof( turn ) );
		re.DrawTriangles2D( &tris );
	}
}

//=============================================================================

/*
==================
SCR_Init
==================
*/
void SCR_Init( void ) {
	cl_timegraph = Cvar_Get ("timegraph", "0", CVAR_CHEAT);
	cl_debuggraph = Cvar_Get ("debuggraph", "0", CVAR_CHEAT);
	cl_graphheight = Cvar_Get ("graphheight", "32", CVAR_CHEAT);
	cl_graphscale = Cvar_Get ("graphscale", "1", CVAR_CHEAT);
	cl_graphshift = Cvar_Get ("graphshift", "0", CVAR_CHEAT);
	cl_batchText = Cvar_Get( "cl_batchText", "0", CVAR_TEMP );
	cl_test2D = Cvar_Get( "cl_test2D", "0", CVAR_TEMP );

	scr_initialized = qtrue;
}


//=======================================================

/*
==================
SCR_DrawScreenField

This will be called twice if rendering in stereo mode
==================
*/
void SCR_DrawScreenField( stereoFrame_t stereoFrame ) {
	qboolean uiFullscreen;

	re.BeginFrame( stereoFrame );

	uiFullscreen = (uivm && VM_Call( uivm, UI_IS_FULLSCREEN ));

	// wide aspect ratio screens need to have the sides cleared
	// unless they are displaying game renderings
	if ( uiFullscreen || clc.state < CA_LOADING ) {
		if ( cls.glconfig.vidWidth * 480 > cls.glconfig.vidHeight * 640 ) {
			re.SetColor( g_color_table[0] );
			re.DrawStretchPic( 0, 0, cls.glconfig.vidWidth, cls.glconfig.vidHeight, 0, 0, 0, 0, cls.whiteShader );
			re.SetColor( NULL );
		}
	}

	// if the menu is going to cover the entire screen, we
	// don't need to render anything under it
	if ( uivm && !uiFullscreen ) {
		switch( clc.state ) {
		default:
			Com_Error( ERR_FATAL, "SCR_DrawScreenField: bad clc.state" );
			break;
		case CA_CINEMATIC:
			SCR_DrawCinematic();
			break;
		case CA_DISCONNECTED:
			// force menu up
			S_StopAllSounds();
			VM_Call( uivm, UI_SET_ACTIVE_MENU, UIMENU_MAIN );
			break;
		case CA_CONNECTING:
		case CA_CHALLENGING:
		case CA_CONNECTED:
			// connecting clients will only show the connection dialog
			// refresh to update the time
			VM_Call( uivm, UI_REFRESH, cls.realtime );
			VM_Call( uivm, UI_DRAW_CONNECT_SCREEN, qfalse );
			break;
		case CA_LOADING:
		case CA_PRIMED:
			// draw the game information screen and loading progress
			CL_CGameRendering(stereoFrame);

			// also draw the connection information, so it doesn't
			// flash away too briefly on local or lan games
			// refresh to update the time
			VM_Call( uivm, UI_REFRESH, cls.realtime );
			VM_Call( uivm, UI_DRAW_CONNECT_SCREEN, qtrue );
			break;
		case CA_ACTIVE:
			// always supply STEREO_CENTER as vieworg offset is now done by the engine.
			CL_CGameRendering(stereoFrame);
			SCR_DrawDemoRecording();
#ifdef USE_VOIP
			SCR_DrawVoipMeter();
#endif
			break;
		}
	}

	// the menu draws next
	if ( Key_GetCatcher( ) & KEYCATCH_UI && uivm ) {
		VM_Call( uivm, UI_REFRESH, cls.realtime );
	}

	if ( cl_test2D->integer ) {
		SCR_DrawTest2D();
	}

	// console draws next
	{
		static int		frames;
		static int64_t	ns, chars;
		int		statFrames = Cvar_VariableIntegerValue( "r_stats2D" );
		int64_t	start = Sys_Nanoseconds();

		scr_smallChars = 0;
		Con_DrawConsole ();

		// spike: with r_stats2D n, the time to draw the console and its
		// characters, averaged over n frames
		if ( statFrames > 0 ) {
			ns += Sys_Nanoseconds() - start;
			chars += scr_smallChars;
			if ( ++frames >= statFrames ) {
				Com_Printf( "stats2D: the console: %.1f us a frame, %.0f characters, cl_batchText %i\n",
					ns / 1000.0 / frames, chars / (double)frames, cl_batchText->integer );
				frames = 0;
				ns = 0;
				chars = 0;
			}
		}
	}

	// debug graph can be drawn on top of anything
	if ( cl_debuggraph->integer || cl_timegraph->integer || cl_debugMove->integer ) {
		SCR_DrawDebugGraph ();
	}
}

/*
==================
SCR_UpdateScreen

This is called every frame, and can also be called explicitly to flush
text to the screen.
==================
*/
void SCR_UpdateScreen( void ) {
	static int	recursive;
	static int64_t	lastSwap;

	if ( !scr_initialized ) {
		return;				// not initialized yet
	}

	if ( ++recursive > 2 ) {
		Com_Error( ERR_FATAL, "SCR_UpdateScreen: recursively called" );
	}
	recursive = 1;

	// If there is no VM, there are also no rendering commands issued. Stop the renderer in
	// that case.
	if( uivm || com_dedicated->integer )
	{
		// XXX
		int in_anaglyphMode = Cvar_VariableIntegerValue("r_anaglyphMode");
		// if running in stereo, we need to draw the frame twice
		if ( cls.glconfig.stereoEnabled || in_anaglyphMode) {
			SCR_DrawScreenField( STEREO_LEFT );
			SCR_DrawScreenField( STEREO_RIGHT );
		} else {
			SCR_DrawScreenField( STEREO_CENTER );
		}

		if ( com_speeds->integer ) {
			re.EndFrame( &time_frontend, &time_backend );
		} else {
			re.EndFrame( NULL, NULL );
		}

		// for com_speeds: the frame has been swapped (natively; a browser
		// shows it once the frame returns)
		{
			int64_t now = Sys_Nanoseconds();

			if ( lastSwap ) {
				time_swap = (int)( ( now - lastSwap ) / 1000 );
			}
			lastSwap = now;
		}
	}
	
	recursive = 0;
}

