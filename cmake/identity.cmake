set(PROJECT_NAME ioq3)
set(PROJECT_VERSION 1.36)

# A parent project that includes this one with add_subdirectory() may set
# any of the following beforehand to rebrand the build.
macro(set_identity NAME VALUE)
    if(NOT DEFINED ${NAME})
        set(${NAME} "${VALUE}")
    endif()
endmacro()

set_identity(SERVER_NAME ioq3ded)
set_identity(CLIENT_NAME ioquake3)

set_identity(BASEGAME baseq3)

set_identity(CGAME_MODULE cgame)
set_identity(GAME_MODULE qagame)
set_identity(UI_MODULE ui)

set_identity(WINDOWS_ICON_PATH ${CMAKE_CURRENT_LIST_DIR}/../misc/windows/quake3.ico)

set_identity(MACOS_ICON_PATH ${CMAKE_CURRENT_LIST_DIR}/../misc/macos/quake3_flat.icns)
set_identity(MACOS_BUNDLE_ID org.ioquake.${CLIENT_NAME})

set_identity(COPYRIGHT "QUAKE III ARENA Copyright © 1999-2000 id Software, Inc. All rights reserved.")

set_identity(CONTACT_EMAIL "info@ioquake.org")
set_identity(PROTOCOL_HANDLER_SCHEME quake3)
