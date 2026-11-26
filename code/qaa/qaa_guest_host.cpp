/*
===========================================================================
FnQAA: the engine side of the guest runtime.

Copyright (C) 2026 FnQAA contributors.
This file is part of FnQAA, licensed under the GNU GPL v2 or later.
===========================================================================
*/
#include "qaa_guest_host.h"

#include "../qcommon/q_shared.h"
#include "../qcommon/qcommon.h"

#include <mutex>
#include <vector>

uint8_t *qaa_guestBase;

namespace {

#ifdef _WIN32
constexpr const char *kLibPrefix = "";
constexpr const char *kLibSuffix = ".dll";
#elif defined( __APPLE__ )
constexpr const char *kLibPrefix = "lib";
constexpr const char *kLibSuffix = ".dylib";
#else
constexpr const char *kLibPrefix = "lib";
constexpr const char *kLibSuffix = ".so";
#endif

qaaGuestApi_t api;
void *runtimeLibrary;
qaa_guest *guest;
bool initTried;
cvar_t *qaa_modulePath;
char moduleDir[MAX_OSPATH];

/* Calls into the guest on this thread, and an engine error waiting to be raised again. */
thread_local int guestDepth;
thread_local bool errorPending;
thread_local int errorCode;
thread_local char errorMessage[MAXPRINTMSG];

/* ---- host services for the guest ------------------------------------- */

/*
 * Files the modules open through the kernel (the CRT's fopen) go to the
 * engine filesystem, read-only: "game:\baseq3\x\y.cfg" is "x/y.cfg".
 * Neither module opens one during its initialisation (docs/RECOMP.md); their
 * game files go through the FS traps instead.
 */
struct HostFile {
	void *data = nullptr;
	int64_t size = 0;
	bool used = false;
};
std::vector<HostFile> hostFiles;
std::mutex hostFileLock;	// the cgame's loader thread may open files too

bool GuestPathToQpath( const char *path, char *qpath, size_t size ) {
	const char *p = path;
	if ( !Q_stricmpn( p, "game:", 5 ) ) {
		p += 5;
	} else if ( !Q_stricmpn( p, "d:", 2 ) ) {
		p += 2;
	} else {
		return false;	// devices, cache and storage partitions: not the game's files
	}
	while ( *p == '\\' || *p == '/' ) {
		p++;
	}
	if ( !Q_stricmpn( p, BASEGAME, strlen( BASEGAME ) ) && ( p[strlen( BASEGAME )] == '\\' || p[strlen( BASEGAME )] == '/' ) ) {
		p += strlen( BASEGAME ) + 1;
	}
	Q_strncpyz( qpath, p, static_cast<int>( size ) );
	for ( char *c = qpath; *c; c++ ) {
		if ( *c == '\\' ) {
			*c = '/';
		}
	}
	return qpath[0] != '\0';
}

void HostLog( void *, int level, const char *message ) {
	switch ( level ) {
	case QAA_LOG_ERROR:
		Com_Printf( S_COLOR_RED "guest: %s\n", message );
		break;
	case QAA_LOG_WARNING:
		Com_Printf( S_COLOR_YELLOW "guest: %s\n", message );
		break;
	case QAA_LOG_INFO:
		Com_Printf( "guest: %s\n", message );
		break;
	default:
		Com_DPrintf( "guest: %s\n", message );
		break;
	}
}

int64_t HostFileOpen( void *, const char *path, int access, int disposition ) {
	char qpath[MAX_QPATH];
	if ( ( access & QAA_FILE_WRITE ) || ( disposition != QAA_FILE_OPEN && disposition != QAA_FILE_OPEN_IF ) ) {
		Com_DPrintf( "guest: refusing to write '%s' (PC deviation: game files are read-only)\n", path );
		return -1;
	}
	if ( !GuestPathToQpath( path, qpath, sizeof( qpath ) ) ) {
		return -1;
	}
	void *data = nullptr;
	const int length = FS_ReadFile( qpath, &data );
	if ( length < 0 || !data ) {
		return -1;
	}
	std::lock_guard<std::mutex> lock( hostFileLock );
	size_t i = 0;
	while ( i < hostFiles.size() && hostFiles[i].used ) {
		i++;
	}
	if ( i == hostFiles.size() ) {
		hostFiles.emplace_back();
	}
	hostFiles[i].data = data;
	hostFiles[i].size = length;
	hostFiles[i].used = true;
	return static_cast<int64_t>( i );
}

HostFile *HostFileFor( int64_t handle ) {
	if ( handle < 0 || static_cast<size_t>( handle ) >= hostFiles.size() || !hostFiles[handle].used ) {
		return nullptr;
	}
	return &hostFiles[handle];
}

int HostFileStat( void *, const char *path, qaa_file_info *info ) {
	char qpath[MAX_QPATH];
	if ( !GuestPathToQpath( path, qpath, sizeof( qpath ) ) ) {
		return -1;
	}
	const int length = FS_ReadFile( qpath, nullptr );
	if ( length < 0 ) {
		return -1;
	}
	info->size = length;
	info->is_directory = 0;
	return 0;
}

int64_t HostFileRead( void *, int64_t handle, int64_t offset, void *buffer, uint32_t length ) {
	std::lock_guard<std::mutex> lock( hostFileLock );
	const HostFile *f = HostFileFor( handle );
	if ( !f || offset < 0 ) {
		return -1;
	}
	if ( offset >= f->size ) {
		return 0;
	}
	const int64_t n = ( offset + length > f->size ) ? f->size - offset : length;
	std::memcpy( buffer, static_cast<const byte *>( f->data ) + offset, static_cast<size_t>( n ) );
	return n;
}

int64_t HostFileWrite( void *, int64_t, int64_t, const void *, uint32_t ) {
	return -1;
}

int64_t HostFileSize( void *, int64_t handle ) {
	std::lock_guard<std::mutex> lock( hostFileLock );
	const HostFile *f = HostFileFor( handle );
	return f ? f->size : -1;
}

void HostFileClose( void *, int64_t handle ) {
	std::lock_guard<std::mutex> lock( hostFileLock );
	HostFile *f = HostFileFor( handle );
	if ( f ) {
		FS_FreeFile( f->data );
		*f = HostFile();
	}
}

int HostDirEntry( void *, const char *, uint32_t, char *, size_t, qaa_file_info * ) {
	return -1;	// as for pak0.bpk, nothing is listed
}

int HostFileSetSize( void *, int64_t, int64_t ) {
	return -1;
}

const qaa_host hostServices = {
	nullptr,
	HostLog,
	HostFileOpen,
	HostFileStat,
	HostFileRead,
	HostFileWrite,
	HostFileSize,
	HostFileClose,
	HostDirEntry,
	HostFileSetSize,
};

/* ---- loading ---------------------------------------------------------- */

void LibraryPath( char *out, size_t size, const char *dir, const char *name ) {
	Com_sprintf( out, static_cast<int>( size ), "%s%c%s%s%s", dir, PATH_SEP, kLibPrefix, name, kLibSuffix );
}

bool FileExists( const char *path ) {
	fileOffset_t size;
	fileTime_t mtime, ctime;
	return Sys_GetFileStats( path, &size, &mtime, &ctime ) != qfalse;
}

/*
 * The libraries tools/recomp/build.py makes: qaa_modulePath if set, else
 * the install folder, else a source build's build/recomp/native/bin.
 */
bool FindModuleDir( void ) {
	char candidates[3][MAX_OSPATH];
	int count = 0;
	char probe[MAX_OSPATH];
	const char *base = Cvar_VariableString( "fs_basepath" );

	if ( qaa_modulePath->string[0] ) {
		Q_strncpyz( candidates[count++], qaa_modulePath->string, MAX_OSPATH );
	} else {
		Q_strncpyz( candidates[count++], base, MAX_OSPATH );
		Com_sprintf( candidates[count++], MAX_OSPATH, "%s%c..%crecomp%cnative%cbin", base, PATH_SEP, PATH_SEP,
			PATH_SEP, PATH_SEP, PATH_SEP );
	}
	for ( int i = 0; i < count; i++ ) {
		LibraryPath( probe, sizeof( probe ), candidates[i], "qaa_guest_runtime" );
		if ( FileExists( probe ) ) {
			Q_strncpyz( moduleDir, candidates[i], sizeof( moduleDir ) );
			return true;
		}
	}
	return false;
}

bool BindApi( void ) {
	bool ok = true;
#define QAA_GUEST_FN( name ) \
	api.name = reinterpret_cast<decltype( api.name )>( Sys_LoadFunction( runtimeLibrary, #name ) ); \
	if ( !api.name ) { \
		Com_Printf( S_COLOR_YELLOW "WARNING: the guest runtime has no %s\n", #name ); \
		ok = false; \
	}
#include "qaa_guest_api.inc"
#undef QAA_GUEST_FN
	return ok;
}

/* default.xex's XEX header: the modules' CRT reads its heap size from it. */
void LoadExecutableHeader( void ) {
	char path[MAX_OSPATH];
	Com_sprintf( path, sizeof( path ), "%s%cqaa_default_xex_header.bin", moduleDir, PATH_SEP );
	FILE *f = Sys_FOpen( path, "rb" );
	if ( !f ) {
		Com_Printf( S_COLOR_YELLOW "WARNING: %s is missing; the modules' CRT uses its default heap size\n", path );
		return;
	}
	std::vector<byte> header;
	byte chunk[4096];
	size_t n;
	while ( ( n = fread( chunk, 1, sizeof( chunk ), f ) ) > 0 ) {
		header.insert( header.end(), chunk, chunk + n );
	}
	fclose( f );
	if ( api.qaa_guest_set_executable_header( guest, header.data(), header.size() ) != QAA_OK ) {
		Com_Printf( S_COLOR_YELLOW "WARNING: default.xex header rejected: %s\n", api.qaa_guest_last_error( guest ) );
	}
}

}	// namespace

/* ---- public ------------------------------------------------------------ */

bool QAA_Host_Init( void ) {
	char path[MAX_OSPATH];
	char error[512];

	if ( guest ) {
		return true;
	}
	if ( initTried ) {
		return false;
	}
	initTried = true;

	qaa_modulePath = Cvar_Get( "qaa_modulePath", "", CVAR_INIT | CVAR_PROTECTED );
	Cvar_SetDescription( qaa_modulePath, "Folder holding the guest runtime and QAA's recompiled game modules "
		"(made from your package by tools/recomp/build.py). Empty: the install folder, then build/recomp/native/bin." );

	if ( !FindModuleDir() ) {
		Com_Printf( S_COLOR_YELLOW "WARNING: no recompiled QAA modules found (qaa_modulePath '%s'); "
			"run tools/recomp/build.py on your package\n", qaa_modulePath->string );
		return false;
	}
	LibraryPath( path, sizeof( path ), moduleDir, "qaa_guest_runtime" );
	runtimeLibrary = Sys_LoadLibrary( path );
	if ( !runtimeLibrary ) {
		Com_Printf( S_COLOR_YELLOW "WARNING: could not load the guest runtime %s\n", path );
		return false;
	}
	if ( !BindApi() ) {
		Sys_UnloadLibrary( runtimeLibrary );
		runtimeLibrary = nullptr;
		return false;
	}
	guest = api.qaa_guest_create( &hostServices, error, sizeof( error ) );
	if ( !guest ) {
		Com_Printf( S_COLOR_YELLOW "WARNING: could not create the guest: %s\n", error );
		Sys_UnloadLibrary( runtimeLibrary );
		runtimeLibrary = nullptr;
		return false;
	}
	qaa_guestBase = api.qaa_guest_base( guest );
	LoadExecutableHeader();
	Com_Printf( "QAA guest runtime: %s\n", path );
	return true;
}

void QAA_Host_Shutdown( void ) {
	if ( guest ) {
		api.qaa_guest_destroy( guest );
		guest = nullptr;
		qaa_guestBase = nullptr;
	}
	if ( runtimeLibrary ) {
		Sys_UnloadLibrary( runtimeLibrary );
		runtimeLibrary = nullptr;
	}
	initTried = false;
}

bool QAA_Host_Available( void ) {
	return guest != nullptr;
}

const qaaGuestApi_t &QAA_Host_Api( void ) {
	return api;
}

qaa_guest *QAA_Host_Guest( void ) {
	return guest;
}

qaa_module *QAA_Host_LoadModule( const char *name ) {
	char path[MAX_OSPATH];
	char file[64];
	char error[512];

	if ( !QAA_Host_Init() ) {
		return nullptr;
	}
	Com_sprintf( file, sizeof( file ), "qaa_%s", name );
	LibraryPath( path, sizeof( path ), moduleDir, file );
	qaa_module *module = api.qaa_module_load( guest, path, error, sizeof( error ) );
	if ( !module ) {
		Com_Printf( S_COLOR_YELLOW "WARNING: could not load %s: %s\n", path, error );
		return nullptr;
	}
	Com_Printf( "QAA %s: %s (image %08X-%08X)\n", name, path, api.qaa_module_base( module ),
		api.qaa_module_base( module ) + api.qaa_module_size( module ) );
	return module;
}

void QAA_Host_UnloadModule( qaa_module *module ) {
	if ( module ) {
		api.qaa_module_unload( module );
	}
}

extern "C" int QAA_Host_InGuest( void ) {
	return guestDepth;
}

namespace {
/* After a failed guest call: raise the recorded engine error, or the fault. */
void RaiseGuestFailure( const char *what ) {
	if ( errorPending ) {
		char message[MAXPRINTMSG];
		const int code = errorCode;
		Q_strncpyz( message, errorMessage, sizeof( message ) );
		errorPending = false;
		Com_Error( static_cast<errorParm_t>( code ), "%s", message );
	}
	Com_Error( ERR_DROP, "%s: %s", what, api.qaa_guest_last_error( guest ) );
}
}

intptr_t QAA_Host_VmMain( qaa_module *module, int command, const int32_t *args, int nargs ) {
	int64_t result = 0;
	guestDepth++;
	const int r = api.qaa_module_vm_main( module, command, args, nargs, &result );
	guestDepth--;
	if ( r != QAA_OK ) {
		RaiseGuestFailure( va( "%s vmMain(%d)", api.qaa_module_name( module ), command ) );
	}
	return static_cast<intptr_t>( static_cast<int32_t>( result ) );
}

int64_t QAA_Host_Call( uint32_t function, const int64_t *args, int nargs ) {
	int64_t result = 0;
	guestDepth++;
	const int r = api.qaa_guest_call( guest, function, args, nargs, &result );
	guestDepth--;
	if ( r != QAA_OK ) {
		RaiseGuestFailure( va( "guest call %08X", function ) );
	}
	return result;
}

extern "C" void QAA_Host_ComErrorHook( int code, const char *message ) {
	if ( guestDepth <= 0 || !guest || errorPending ) {
		return;
	}
	errorPending = true;
	errorCode = code;
	Q_strncpyz( errorMessage, message ? message : "", sizeof( errorMessage ) );
	// Unwinds to the innermost QAA_Host_VmMain/QAA_Host_Call, which raises
	// the error again on the engine side. A nested call re-enters this hook
	// for each guest call it unwinds.
	api.qaa_guest_abort( guest, errorMessage );
}
