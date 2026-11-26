#ifndef FNQAA_VERSION_H_
#define FNQAA_VERSION_H_

/*
 * FnQAA version metadata: the single source of truth for the project name
 * and version used by the engine, the Windows resources and meson.build.
 * FnQAA's engine is FnQL (see CREDITS.md for the imported commit).
 */

#define FNQAA_PROJECT_NAME "FnQAA"
#define FNQAA_PROJECT_NAME_SHORT "FnQAA"
#define FNQAA_DISPLAY_NAME "FnQAA"
#define FNQAA_COMPATIBILITY_TARGET "Quake Arena Arcade (Xbox Live Arcade)"

#define FNQAA_VERSION_MAJOR 0
#define FNQAA_VERSION_MINOR 0
#define FNQAA_VERSION_PATCH 2
#define FNQAA_VERSION_TWEAK 0
#define FNQAA_VERSION_STRING "0.0.2"

/* Release builds stamp FNQAA_VERSION_TWEAK and the version strings with the
 * build number (scripts/manual_release.py stamp-version): the commits since
 * the latest stable tag <FNQAA_TAG_PREFIX><major>.<minor>.<patch>. */
#define FNQAA_TAG_PREFIX "v"
#define FNQAA_ARTIFACT_PREFIX "fnqaa"

/* User data folder: %APPDATA%\<name> with USE_PROFILES on Windows, ~/.<unix> on Linux. */
#define FNQAA_HOMEPATH_NAME_WIN "FnQAA"
#define FNQAA_HOMEPATH_NAME_UNIX ".fnqaa"
#define FNQAA_HOMEPATH_NAME_MACOSX "FnQAA"

#define FNQAA_WINDOWS_FILE_VERSION 0,0,2,0
#define FNQAA_WINDOWS_PRODUCT_VERSION 0,0,2,0

#endif
