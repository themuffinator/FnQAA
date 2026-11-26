#ifndef OPENQAA_VERSION_H_
#define OPENQAA_VERSION_H_

/*
 * openQAA version metadata: the single source of truth for the project name
 * and version used by the engine, the Windows resources and meson.build.
 * openQAA's engine is FnQL (see CREDITS.md for the imported commit).
 */

#define OPENQAA_PROJECT_NAME "openQAA"
#define OPENQAA_PROJECT_NAME_SHORT "openQAA"
#define OPENQAA_DISPLAY_NAME "openQAA"
#define OPENQAA_COMPATIBILITY_TARGET "Quake Arena Arcade (Xbox Live Arcade)"

#define OPENQAA_VERSION_MAJOR 0
#define OPENQAA_VERSION_MINOR 0
#define OPENQAA_VERSION_PATCH 2
#define OPENQAA_VERSION_TWEAK 0
#define OPENQAA_VERSION_STRING "0.0.2"

#define OPENQAA_ARTIFACT_PREFIX "openqaa"

/* User data folder: %APPDATA%\<name> with USE_PROFILES on Windows, ~/.<unix> on Linux. */
#define OPENQAA_HOMEPATH_NAME_WIN "openQAA"
#define OPENQAA_HOMEPATH_NAME_UNIX ".openqaa"
#define OPENQAA_HOMEPATH_NAME_MACOSX "openQAA"

#define OPENQAA_WINDOWS_FILE_VERSION 0,0,2,0
#define OPENQAA_WINDOWS_PRODUCT_VERSION 0,0,2,0

#endif
