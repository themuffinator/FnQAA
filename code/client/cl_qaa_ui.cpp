/*
===========================================================================
FnQAA: QAA's Scaleform front end (milestone M6).

Copyright (C) 2026 FnQAA contributors.
This file is part of FnQAA, licensed under the GNU GPL v2 or later.

See cl_qaa_ui.h and docs/UI.md. Addresses are default.xex's: the retail
code was read for its behaviour, and nothing of it is stored here.

Structure:
  - guest memory: the front end's own C runtime heap (malloc/free in
    default.xex) serves every buffer the host hands it;
  - cvar mirrors: the guest reads cvar_t fields directly, so each cvar it
    asks for gets a guest copy that follows the engine's;
  - hooks: the engine functions the recompiled code calls (Cvar, Cmd,
    Cbuf, zone memory, file system, the ui syscalls of QAA's menu data
    layer, Xbox Live stubs);
  - D3D: the few texture calls SFUI makes itself (bot pictures);
  - the renderer back end: SFUI's GRenderer records draw commands; the
    object that replays them (QAA's "SFUI-Backend") is replaced by a host
    one that rasterises them (cl_qaa_ui_raster.cpp);
  - frames: SFUI's update thread runs one iteration per client frame in
    lockstep with the engine; its render runs on the main thread.
===========================================================================
*/
#include "client.h"
#include "cl_qaa_ui.h"
#include "cl_qaa_ui_raster.h"

#include "../qaa/qaa_guest_host.h"
#include "../qaa/qaa_structs.h"
#include "../qcommon/qaa_binds.h"

#include <algorithm>
#include <limits>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdarg>
#include <cstdlib>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace {

/* ==== addresses in default.xex ================================================ */

namespace addr {
// CRT start-up, in mainCRTStartup's order (0x821D7520); the engine's main is not run
constexpr uint32_t CRT_HEAP_INIT = 0x821D9748;
constexpr uint32_t CRT_MTINIT = 0x821DF7B0;
constexpr uint32_t CRT_PRE_C_INIT = 0x821D9520;
constexpr uint32_t CRT_CINIT = 0x821D9440;
constexpr uint32_t CRT_MALLOC = 0x821DDBA8;			// operator new (0x821DFB50) calls it
constexpr uint32_t CRT_FREE = 0x821DDB48;

// SFUI
constexpr uint32_t SFUI_INIT = 0x8218E048;			// called by the engine's start-up (0x82185348)
constexpr uint32_t SFUI_RENDER = 0x8218E5A8;		// SCR_UpdateScreen (0x8210E8E0) / render thread (0x82142448)
constexpr uint32_t SFUI_KEY_EVENT = 0x8218ED18;		// CL_KeyEvent (0x82106D20): key, down, time
constexpr uint32_t SFUI_STATE_PTR = 0x82A87958;		// the SFUI object (0x3627E0 bytes)
constexpr uint32_t SFUI_RENDERER = 0x21CC;			// +: the GRenderer (0x800280 bytes, ctor 0x82166B38)
constexpr uint32_t SFUI_THREAD = 0x361ED4;			// +: update thread handle (0x82195200)
constexpr uint32_t SFUI_FRAME_EVENT = 0x361ED8;		// +: the event the thread waits on (0x82196458)
constexpr uint32_t SFUI_MOVIES = 0x21E4;			// +: GFxMovieView *[], the top one takes the input (0x82198698)
constexpr uint32_t SFUI_MOVIE_COUNT = 0x21E8;		// +: how many

// the title's Win32 wrappers over the kernel's events (recompiled with SFUI)
constexpr uint32_t CREATE_EVENT = 0x821F1928;		// (attributes, manual reset, initially set, name): SFUI's start-up (0x82195ACC)
constexpr uint32_t WAIT_FOR_OBJECT = 0x821D29D0;	// (handle, milliseconds): 0 when set, 0x102 on timeout
constexpr uint32_t RESET_EVENT = 0x821F1A30;		// (handle)

// GFx 2.x GFxMovieView (vtable 0x820B0490)
constexpr uint32_t MOVIE_GET_VARIABLE = 0x30;		// vtable +: GetVariable(GFxValue *, path) (0x82655768)
constexpr uint32_t MOVIE_INVOKE = 0x48;				// vtable +: Invoke(path, GFxValue *result, args, count) (0x82658590)
constexpr uint32_t MOVIE_STAGE_SCALE_X = 0x70;		// +: viewport pixels to stage pixels: x * scale + offset
constexpr uint32_t MOVIE_STAGE_SCALE_Y = 0x74;		//   (as HandleEvent maps the mouse, 0x82650C10)
constexpr uint32_t MOVIE_STAGE_OFFSET_X = 0x78;
constexpr uint32_t MOVIE_STAGE_OFFSET_Y = 0x7C;

// engine data the front end reads
constexpr uint32_t CMD_ARGC = 0x828E0AD0;			// Cmd_Argc, read inline by the sf_* commands
constexpr uint32_t CMD_ARGV = 0x828DB2D0;			// char *cmd_argv[]
constexpr uint32_t CLS_GLCONFIG = 0x82F85634;		// cls.glconfig, QAA layout (0x2C54 bytes)
constexpr uint32_t RENDERER_GLCONFIG = 0x82DC0480;	// the renderer's glConfig, same layout
constexpr uint32_t LIVE_STATE_PTR = 0x82C01C04;		// the session object, polled by SFUI's thread
constexpr uint32_t PROFILE_PTR = 0x82A6EA58;		// the active profile (+0x20 user index)
constexpr uint32_t UI_DC_PTR = 0x82C01C78;			// ui_shared's displayContextDef_t *DC
constexpr uint32_t CLS_STATE = 0x82ED88C0;			// cls.state (connstate_t, as FnQL's)
constexpr uint32_t CLS_KEYCATCHERS = 0x82ED88C4;	// cls.keyCatchers: SFUI sets and clears KEYCATCH_UI itself
constexpr uint32_t CLS_REALTIME = 0x82ED89E8;		// cls.realtime: SFUI's event queue runs every 33 ms of it (0x82191398)
constexpr uint32_t SFUI_SET_GLOBAL_ARRAY = 0x82196D40;	// (sfui, count, path, type, values): CL_KeyEvent's in-game menu
constexpr uint32_t STR_UI_VALUES = 0x820D22E8;		// "_global.uiValues"
constexpr uint32_t SFUI_INGAME_MENU_VALUE = 0x361EC8;	// +: the value CL_KeyEvent publishes
}

/* the engine functions the module calls (data/recomp/ui.json "hooks") */
namespace hook {
constexpr uint32_t UI_SYSCALL = 0x8210F988;			// CL_UISystemCalls for the menu data layer
constexpr uint32_t CBUF_ADDTEXT = 0x82119708;
constexpr uint32_t CMD_ADDCOMMAND = 0x8211A158;
constexpr uint32_t Z_FREE = 0x8211B258;
constexpr uint32_t Z_TAGMALLOC = 0x8211B3E8;
constexpr uint32_t Z_MALLOC = 0x8211B548;
constexpr uint32_t HUNK_ALLOC_TEMP = 0x8211C180;
constexpr uint32_t HUNK_FREE_TEMP = 0x8211C2D0;
constexpr uint32_t CVAR_FINDVAR = 0x8211F548;
constexpr uint32_t CVAR_GET = 0x8211F6C8;
constexpr uint32_t CVAR_SET2 = 0x8211F908;
constexpr uint32_t CVAR_SETVALUE = 0x8211FBE8;
constexpr uint32_t FS_FCLOSEFILE = 0x82121B28;
constexpr uint32_t FS_FOPENFILEREAD = 0x821221B0;
constexpr uint32_t FS_READ = 0x82122278;
constexpr uint32_t FS_READFILE = 0x82122758;
constexpr uint32_t FS_FREEFILE = 0x82122890;
constexpr uint32_t FS_GETFILELIST = 0x82123080;
constexpr uint32_t R_GETCOMMANDBUFFER = 0x821408A8;
constexpr uint32_t SFUI_BACKEND_CTOR = 0x821687F0;
constexpr uint32_t S_STARTLOCALSOUND = 0x82173418;
constexpr uint32_t S_STARTBACKGROUNDTRACK = 0x82173470;
constexpr uint32_t S_REGISTERSOUND = 0x82173DC0;
constexpr uint32_t QNET_JOIN_WAIT = 0x82175A28;		// "Joining server", then the session thread
constexpr uint32_t QNET_LEAVE_GAME = 0x821783D0;	// flags the session to end, as FgApi.exitGame
constexpr uint32_t LANGUAGE_CODE = 0x82184DA8;
constexpr uint32_t COUNTRY_CODE = 0x82184E58;
constexpr uint32_t SYS_MILLISECONDS = 0x82185850;
constexpr uint32_t IS_FULL_VERSION = 0x821856C8;
constexpr uint32_t CAN_PLAY = 0x821859B8;
constexpr uint32_t USER_SIGNED_IN = 0x821869D8;
constexpr uint32_t SHOW_DEVICE_SELECTOR = 0x8218CA70;
constexpr uint32_t START_LOAD = 0x8218BCE8;
constexpr uint32_t START_SAVE = 0x8218BDB0;
constexpr uint32_t D3D_RELEASE = 0x822021E0;
constexpr uint32_t D3D_UNLOCK = 0x822023E8;
constexpr uint32_t D3D_LOCKRECT = 0x82203EA0;
constexpr uint32_t D3D_LEVELDESC = 0x82204030;
constexpr uint32_t D3D_CREATETEXTURE = 0x82204038;
constexpr uint32_t D3DX_TEXTURE_FROM_MEMORY = 0x8221D000;
}

cvar_t *qaa_uiEnable;
cvar_t *qaa_uiTrace;
cvar_t *qaa_uiTestFrames;
cvar_t *qaa_uiProfile;
cvar_t *qaa_uiTestVerify;
qaa_module *uiModule;
bool uiReady;
bool uiFailed;
uint32_t sfui;			// the SFUI object

const qaaGuestApi_t &Api() {
	return QAA_Host_Api();
}
qaa_guest *Guest() {
	return QAA_Host_Guest();
}

/* ==== calls into the guest ===================================================== */

/* A guest call that does not raise an engine error: a failure in the front
 * end turns the front end off instead of taking the client down. */
bool UiCall( uint32_t fn, std::initializer_list<uint64_t> r, uint64_t *r3 = nullptr,
	std::initializer_list<double> f = {}, double *f1 = nullptr ) {
	if ( uiFailed ) {
		return false;
	}
	uint64_t rv[8] = {};
	double fv[13] = {};
	int nr = 0, nf = 0;
	for ( uint64_t v : r ) {
		if ( nr < 8 ) {
			rv[nr++] = v;
		}
	}
	for ( double v : f ) {
		if ( nf < 13 ) {
			fv[nf++] = v;
		}
	}
	if ( Api().qaa_guest_call_regs( Guest(), fn, rv, nr, fv, nf, r3, f1 ) != QAA_OK ) {
		Com_Printf( S_COLOR_RED "QAA ui: guest call %08X failed: %s\n", fn, Api().qaa_guest_last_error( Guest() ) );
		// the guest call chain, from the faulting frame's back chain (each
		// frame saves its caller's LR at the caller's stack pointer - 8)
		const char *error = Api().qaa_guest_last_error( Guest() );
		const char *r1 = strstr( error, "r1 " );
		if ( r1 ) {
			uint32_t sp = static_cast<uint32_t>( strtoul( r1 + 3, nullptr, 16 ) );
			char chain[512] = "";
			for ( int i = 0; i < 16 && sp >= 0x70000000u && sp < 0x7E000000u; i++ ) {
				const uint32_t caller = QAA_LoadU32( sp );
				if ( caller <= sp || caller >= 0x7E000000u ) {
					break;
				}
				Q_strcat( chain, sizeof( chain ), va( " %08X", QAA_LoadU32( caller - 8 ) ) );
				sp = caller;
			}
			Com_Printf( S_COLOR_RED "QAA ui: guest return addresses:%s\n", chain );
		}
		uiFailed = true;
		return false;
	}
	return true;
}

/* ==== guest memory ================================================================ */

uint32_t GMalloc( uint32_t size ) {
	uint64_t r = 0;
	if ( !UiCall( addr::CRT_MALLOC, { size ? size : 1 }, &r ) ) {
		return 0;
	}
	return static_cast<uint32_t>( r );
}

uint32_t GCalloc( uint32_t size ) {
	const uint32_t p = GMalloc( size );
	if ( p ) {
		std::memset( QAA_Ptr( p ), 0, size );
	}
	return p;
}

void GFree( uint32_t p ) {
	if ( p ) {
		UiCall( addr::CRT_FREE, { p } );
	}
}

uint32_t GStrdup( const char *s ) {
	const size_t n = strlen( s ) + 1;
	const uint32_t p = GMalloc( static_cast<uint32_t>( n ) );
	if ( p ) {
		std::memcpy( QAA_Ptr( p ), s, n );
	}
	return p;
}

void GStrcpy( uint32_t dst, const char *src, int size ) {
	if ( dst && size > 0 ) {
		Q_strncpyz( reinterpret_cast<char *>( QAA_Ptr( dst ) ), src, size );
	}
}

std::string GStr( uint32_t a, size_t max = 4096 ) {
	if ( !a ) {
		return std::string();
	}
	const char *s = QAA_Str( a );
	return std::string( s, strnlen( s, max ) );
}

/* ==== cvar mirrors ============================================================== */

/*
 * QAA's cvar_t is Quake III's (0x2C bytes): name, string, resetString,
 * latchedString, flags, modified, modificationCount, value, integer, next,
 * hashNext (Cvar_Get 0x8211F6C8, Cvar_FindVar 0x8211F548). The front end
 * keeps cvar_t pointers and reads their fields, so every cvar it asks for
 * gets a guest copy, refreshed when the engine's changes.
 */
namespace gcvar {
constexpr uint32_t NAME = 0x00, STRING = 0x04, RESET = 0x08, LATCHED = 0x0C, FLAGS = 0x10, MODIFIED = 0x14,
	MODCOUNT = 0x18, VALUE = 0x1C, INTEGER = 0x20, SIZE = 0x2C;
}

struct Mirror {
	uint32_t guest = 0;
	cvar_t *var = nullptr;
	int modificationCount = -1;
	std::string string;
};
std::unordered_map<std::string, Mirror> mirrors;

std::string CvarKey( const char *name ) {
	std::string k( name );
	for ( char &c : k ) {
		c = static_cast<char>( tolower( static_cast<unsigned char>( c ) ) );
	}
	return k;
}

void SyncMirror( Mirror &m ) {
	cvar_t *v = m.var;
	if ( !v || !m.guest ) {
		return;
	}
	if ( v->modificationCount == m.modificationCount && m.string == v->string ) {
		return;
	}
	m.modificationCount = v->modificationCount;
	if ( m.string != v->string ) {
		const uint32_t old = QAA_LoadU32( m.guest + gcvar::STRING );
		m.string = v->string;
		QAA_StoreU32( m.guest + gcvar::STRING, GStrdup( v->string ) );
		GFree( old );
	}
	QAA_StoreU32( m.guest + gcvar::FLAGS, static_cast<uint32_t>( v->flags ) & 0xFFFF );
	QAA_StoreU32( m.guest + gcvar::MODIFIED, 1 );
	QAA_StoreU32( m.guest + gcvar::MODCOUNT, static_cast<uint32_t>( v->modificationCount ) );
	QAA_StoreF( m.guest + gcvar::VALUE, v->value );
	QAA_StoreS32( m.guest + gcvar::INTEGER, v->integer );
}

uint32_t MirrorCvar( cvar_t *v ) {
	if ( !v ) {
		return 0;
	}
	Mirror &m = mirrors[CvarKey( v->name )];
	if ( !m.guest ) {
		m.guest = GCalloc( gcvar::SIZE );
		if ( !m.guest ) {
			return 0;
		}
		m.var = v;
		QAA_StoreU32( m.guest + gcvar::NAME, GStrdup( v->name ) );
		QAA_StoreU32( m.guest + gcvar::RESET, GStrdup( v->resetString ? v->resetString : "" ) );
	}
	m.var = v;
	SyncMirror( m );
	return m.guest;
}

void SyncAllMirrors() {
	for ( auto &it : mirrors ) {
		SyncMirror( it.second );
	}
}

/* ==== hooks ======================================================================== */

std::set<uint32_t> warnedHooks;

/* Stack argument k (0-based, after r3..r10) of a hooked call. */
uint32_t StackArg( const qaa_hook_call *c, int k ) {
	return QAA_LoadU32( c->sp + 0x54 + 8 * k );
}

inline uint32_t R( const qaa_hook_call *c, int k ) {	// r3 + k
	return static_cast<uint32_t>( c->r[k] );
}

void Trace( const qaa_hook_call *c, const char *name ) {
	if ( qaa_uiTrace && qaa_uiTrace->integer ) {
		Com_Printf( "ui: %s(%08X, %08X, %08X, %08X) from %08X\n", name, R( c, 0 ), R( c, 1 ), R( c, 2 ), R( c, 3 ),
			c->return_address );
	}
}

/* An engine function with no host implementation: harmless to skip (Xbox
 * Live, profiles and storage). Logged once. */
void DefaultHook( void *, qaa_hook_call *c ) {
	if ( warnedHooks.insert( c->address ).second ) {
		Com_DPrintf( "QAA ui: unhandled engine call %08X (r3=%08X r4=%08X, from %08X)\n", c->address,
			R( c, 0 ), R( c, 1 ), c->return_address );
	}
	c->r[0] = 0;
	c->f[0] = 0.0;
}

/* An import the runtime does not implement: the title's Xbox services (XAM
 * UI, Live, storage, module loading). Kernel functions return NTSTATUS, so
 * the default is STATUS_UNSUCCESSFUL; XAM functions return Win32 errors or
 * values, so the default is 0. */
void DefaultImportHook( void *, qaa_hook_call *c ) {
	const bool xam = ( c->address >> 16 ) != 0;
	if ( warnedHooks.insert( 0xF0000000u | c->address ).second ) {
		Com_DPrintf( "QAA ui: unhandled %s import %u (from %08X)\n", xam ? "xam" : "xboxkrnl",
			c->address & 0xFFFF, c->return_address );
	}
	c->r[0] = xam ? 0 : 0xC0000001u;
}

/* -- commands and the command buffer ------------------------------------------------ */

std::map<std::string, uint32_t> guestCommands;
uint32_t argvBuffer;		// guest copies of the current command's arguments

void SetGuestArgs() {
	enum { ARGV_BYTES = 0x2000, MAX_ARGS = 64 };
	if ( !argvBuffer ) {
		argvBuffer = GCalloc( ARGV_BYTES );
		if ( !argvBuffer ) {
			return;
		}
	}
	const int argc = std::min( Cmd_Argc(), static_cast<int>( MAX_ARGS ) );
	uint32_t at = argvBuffer;
	for ( int i = 0; i < argc; i++ ) {
		const char *a = Cmd_Argv( i );
		const uint32_t n = static_cast<uint32_t>( strlen( a ) ) + 1;
		if ( at + n > argvBuffer + ARGV_BYTES ) {
			break;
		}
		std::memcpy( QAA_Ptr( at ), a, n );
		QAA_StoreU32( addr::CMD_ARGV + 4 * i, at );
		at += n;
	}
	QAA_StoreU32( addr::CMD_ARGC, static_cast<uint32_t>( argc ) );
}

void GuestCommand( void ) {
	auto it = guestCommands.find( CvarKey( Cmd_Argv( 0 ) ) );
	if ( it == guestCommands.end() || !QAA_UI_Active() ) {
		return;
	}
	SetGuestArgs();
	UiCall( it->second, {} );
}

/* QAA: PC deviation. The front end's command text (FSCommand "qcmd") binds
 * the pad under the Xbox 360's key names, and its unbindall clears the
 * Xbox's only bindings, the pad's: the Custom Controls menu runs
 * "unbindall;", then binds Back and the right trigger again. The keys are
 * renamed and unbindall is kept to the pad, so the keyboard, the mouse and
 * the sticks keep their binds (qaa_binds.h). */
void H_Cbuf_AddText( void *, qaa_hook_call *c ) {
	Trace( c, "Cbuf_AddText" );
	const char *text = QAA_Str( R( c, 0 ) );
	std::string rewritten( QAA_RewriteFrontEndCommands( text, nullptr, 0 ) + 1, '\0' );
	QAA_RewriteFrontEndCommands( text, rewritten.data(), rewritten.size() );
	Cbuf_AddText( rewritten.c_str() );
}

void H_Cmd_AddCommand( void *, qaa_hook_call *c ) {
	const std::string name = GStr( R( c, 0 ) );
	const uint32_t fn = R( c, 1 );
	if ( name.empty() ) {
		return;
	}
	guestCommands[CvarKey( name.c_str() )] = fn;
	if ( fn ) {
		Cmd_AddCommand( name.c_str(), GuestCommand );
	}
}

/* -- memory ----------------------------------------------------------------------------- */

void H_Z_TagMalloc( void *, qaa_hook_call *c ) {
	c->r[0] = GCalloc( R( c, 0 ) );
}

void H_Z_Free( void *, qaa_hook_call *c ) {
	GFree( R( c, 0 ) );
}

/* -- cvars ------------------------------------------------------------------------------- */

void H_Cvar_Get( void *, qaa_hook_call *c ) {
	Trace( c, "Cvar_Get" );
	const std::string name = GStr( R( c, 0 ) );
	if ( name.empty() ) {
		c->r[0] = 0;
		return;
	}
	cvar_t *v = Cvar_Get( name.c_str(), QAA_Str( R( c, 1 ) ), static_cast<int>( R( c, 2 ) ) & 0x7FF );
	c->r[0] = MirrorCvar( v );
}

void H_Cvar_FindVar( void *, qaa_hook_call *c ) {
	const std::string name = GStr( R( c, 0 ) );
	if ( name.empty() || Cvar_Flags( name.c_str() ) == CVAR_NONEXISTENT ) {
		c->r[0] = 0;
		return;
	}
	c->r[0] = MirrorCvar( Cvar_Get( name.c_str(), "", 0 ) );
}

void H_Cvar_Set2( void *, qaa_hook_call *c ) {
	Trace( c, "Cvar_Set2" );
	const std::string name = GStr( R( c, 0 ) );
	if ( name.empty() ) {
		c->r[0] = 0;
		return;
	}
	const char *value = R( c, 1 ) ? QAA_Str( R( c, 1 ) ) : nullptr;
	cvar_t *v = Cvar_Set2( name.c_str(), value, R( c, 2 ) ? qtrue : qfalse );
	c->r[0] = MirrorCvar( v );
}

void H_Cvar_SetValue( void *, qaa_hook_call *c ) {
	const std::string name = GStr( R( c, 0 ) );
	if ( !name.empty() ) {
		Cvar_SetValue( name.c_str(), static_cast<float>( c->f[0] ) );
		MirrorCvar( Cvar_Get( name.c_str(), "", 0 ) );
	}
}

/* -- files --------------------------------------------------------------------------------- */

std::string Qpath( uint32_t a ) {
	std::string p = GStr( a, MAX_OSPATH );
	for ( char &ch : p ) {
		if ( ch == '\\' ) {
			ch = '/';
		}
	}
	return p;
}

void H_FS_FOpenFileRead( void *, qaa_hook_call *c ) {
	const std::string path = Qpath( R( c, 0 ) );
	fileHandle_t f = 0;
	const int len = FS_FOpenFileRead( path.c_str(), R( c, 1 ) ? &f : nullptr, qtrue );
	if ( R( c, 1 ) ) {
		QAA_StoreS32( R( c, 1 ), len >= 0 ? f : 0 );
	}
	Trace( c, va( "FS_FOpenFileRead %s -> %d", path.c_str(), len ) );
	c->r[0] = static_cast<uint32_t>( len );
}

void H_FS_Read( void *, qaa_hook_call *c ) {
	const int len = static_cast<int>( R( c, 1 ) );
	const fileHandle_t f = static_cast<fileHandle_t>( R( c, 2 ) );
	c->r[0] = static_cast<uint32_t>( f && len > 0 ? FS_Read( QAA_Ptr( R( c, 0 ) ), len, f ) : 0 );
}

void H_FS_FCloseFile( void *, qaa_hook_call *c ) {
	if ( R( c, 0 ) ) {
		FS_FCloseFile( static_cast<fileHandle_t>( R( c, 0 ) ) );
	}
}

void H_FS_ReadFile( void *, qaa_hook_call *c ) {
	const std::string path = Qpath( R( c, 0 ) );
	const uint32_t out = R( c, 1 );
	void *data = nullptr;
	const int len = FS_ReadFile( path.c_str(), out ? &data : nullptr );
	Trace( c, va( "FS_ReadFile %s -> %d", path.c_str(), len ) );
	if ( out ) {
		uint32_t buf = 0;
		if ( len >= 0 && data ) {
			buf = GMalloc( static_cast<uint32_t>( len ) + 1 );
			if ( buf ) {
				std::memcpy( QAA_Ptr( buf ), data, static_cast<size_t>( len ) );
				QAA_StoreU8( buf + static_cast<uint32_t>( len ), 0 );
			}
		}
		if ( data ) {
			FS_FreeFile( data );
		}
		QAA_StoreU32( out, buf );
	}
	c->r[0] = static_cast<uint32_t>( len );
}

void H_FS_FreeFile( void *, qaa_hook_call *c ) {
	GFree( R( c, 0 ) );
}

void H_FS_GetFileList( void *, qaa_hook_call *c ) {
	const std::string path = Qpath( R( c, 0 ) );
	const std::string ext = GStr( R( c, 1 ) );
	const int size = static_cast<int>( R( c, 3 ) );
	c->r[0] = static_cast<uint32_t>( R( c, 2 ) && size > 0
		? FS_GetFileList( path.c_str(), ext.c_str(), reinterpret_cast<char *>( QAA_Ptr( R( c, 2 ) ) ), size ) : 0 );
}

/* -- time, sound, locale ---------------------------------------------------------------- */

void H_Sys_Milliseconds( void *, qaa_hook_call *c ) {
	c->r[0] = static_cast<uint32_t>( Sys_Milliseconds() );
}

std::unordered_map<uint32_t, sfxHandle_t> sounds;	// guest handle -> engine

void H_S_RegisterSound( void *, qaa_hook_call *c ) {
	const std::string name = GStr( R( c, 0 ) );
	c->r[0] = name.empty() ? 0 : static_cast<uint32_t>( S_RegisterSound( name.c_str(), R( c, 1 ) ? qtrue : qfalse ) );
}

void H_S_StartLocalSound( void *, qaa_hook_call *c ) {
	if ( R( c, 0 ) ) {
		S_StartLocalSound( static_cast<sfxHandle_t>( R( c, 0 ) ), static_cast<int>( R( c, 1 ) ) );
	}
}

/* FgApi.setGameState's value (0x82190CF0): 1 while the start-up movies
 * play, 2 at the title, 3 in the menus. */
namespace gamestate {
constexpr uint32_t ADDRESS = 0x82C01C48;
constexpr uint32_t STARTUP = 1, TITLE = 2, POSTGAME = 7;
}

/* On the console a game cannot start before the title; on the PC it can,
 * from the command line (+map, +devmap) while the start-up movies still
 * play. */
bool GameStartedBeforeTitle() {
	return cls.state == CA_ACTIVE && QAA_LoadU32( gamestate::ADDRESS ) <= gamestate::TITLE;
}

/* 0x82173470(intro, loop): the background track, as the "playmusic"
 * fscommand starts it (the menus' music; the files are XMA2 .xma beside
 * the .wav names, FORMATS.md). PC deviation: the title's music is not
 * started over a game that began before the title, whose own music plays. */
void H_S_StartBackgroundTrack( void *, qaa_hook_call *c ) {
	const std::string intro = GStr( R( c, 0 ) ), loop = GStr( R( c, 1 ) );
	if ( GameStartedBeforeTitle() ) {
		return;
	}
	if ( !intro.empty() || !loop.empty() ) {
		S_StartBackgroundTrack( intro.c_str(), loop.c_str() );
	}
}

/* PC deviation, for a game started before the title: the first snapshot's
 * HUD scene (QAA_UI_EnterGame) was asked for while the start-up movie had
 * no scene manager to take it, and the title then loads over the game.
 * When the front end reaches the title with a game running, it is asked
 * again, as when a map is started from the title. */
void PollGameStartedBeforeTitle() {
	static uint32_t last;
	const uint32_t state = QAA_LoadU32( gamestate::ADDRESS );
	if ( state == gamestate::TITLE && last != gamestate::TITLE && cls.state == CA_ACTIVE ) {
		Com_DPrintf( "QAA ui: the game began before the title; showing the HUD\n" );
		QAA_UI_EnterGame();
	}
	last = state;
}

/* The two-letter codes QAA builds its localisation paths from (0x82184DA8:
 * XGetLanguage, 0x82184E58: XGetLocale). The language is English, as an
 * Xbox set to English. The country is the console's region: the startup
 * movie (ui/startup.gfx) shows the ESRB rating screen and the US legal text
 * only for "us", and the rest-of-world legal text without a rating
 * elsewhere. PC deviation: the region is the operating system's (Windows'
 * home location, else the language environment's territory), or
 * qaa_country when set. */
void H_LanguageCode( void *, qaa_hook_call *c ) {
	GStrcpy( R( c, 0 ), "en", 3 );
}

void H_CountryCode( void *, qaa_hook_call *c ) {
	GStrcpy( R( c, 0 ), QAA_UI_CountryCode(), 3 );
}

/* -- licence and profile ---------------------------------------------------------------- */

/* The per-controller profile records (0x498 bytes each; controller 0's
 * is published by PublishProfile). */
namespace profile {
constexpr uint32_t XUID = 0x82A751A8, FLAGS = 0x82A751B0, SIGNED_IN = 0x82A751B4, GAMERTAG = 0x82A751C0;
constexpr uint32_t STRIDE = 0x498;
constexpr int GAMERTAG_SIZE = 16;
}

/* 0x821856C8: the full-game licence (retail caches XamContentGetLicenseMask
 * bit 0 at 0x82CD9D6C); the menus' isFullVersion, achievements and saves
 * ask it. PC deviation: the user's own copy of the package is the full
 * game, so the licence is always held. */
void H_IsFullVersion( void *, qaa_hook_call *c ) {
	c->r[0] = 1;
}

/* XamContentGetLicenseMask (xam.xex ordinal 614)(mask *, overlapped): the
 * title's licence. 0x821856C8 above caches it, and ui_main's model count
 * (0x821A2FB8) asks it itself: without the full licence only the
 * characters free in the trial are listed. PC deviation, as above: bit 0,
 * the full game, is held. */
void H_XamContentGetLicenseMask( void *, qaa_hook_call *c ) {
	if ( R( c, 0 ) ) {
		QAA_StoreU32( R( c, 0 ), 1 );
	}
	c->r[0] = 0;	// ERROR_SUCCESS
}

/* 0x821859B8(controller): may this controller play, i.e. the full licence
 * or a trial with time left of its hour (records +0x440/+0x448). With the
 * licence always held this is retail's first branch. */
void H_CanPlay( void *, qaa_hook_call *c ) {
	const int32_t controller = static_cast<int32_t>( R( c, 0 ) );
	c->r[0] = controller >= 0 && controller < 4 ? 1 : 0;
}

/* 0x821869D8(controller): signed in to a profile that is not a guest
 * (retail: record +0xC non-zero and flags bit 1 clear). */
void H_UserSignedIn( void *, qaa_hook_call *c ) {
	const int32_t controller = static_cast<int32_t>( R( c, 0 ) );
	if ( controller < 0 || controller >= 4 ) {
		c->r[0] = 0;
		return;
	}
	const uint32_t at = controller * profile::STRIDE;
	c->r[0] = QAA_LoadU32( profile::SIGNED_IN + at ) != 0 && !( QAA_LoadU32( profile::FLAGS + at ) & 2 ) ? 1 : 0;
}

/* 0x8218CA70(controller, reshow): the storage device selector. Retail
 * opens XShowDeviceSelectorUI with the overlapped at 0x82A76B70, and its
 * engine-frame poll (0x8218C890) finishes the selection: the device id
 * goes to 0x82A7DA54[controller] (FgApi.hasChosenStorage reads it) and
 * the movies get DeviceSelectionComplete "%d %d", or, while 0x82C01C38 is
 * set, MainDeviceSelectionComplete is invoked. PC deviation: there is one
 * storage device, the user's home path, so the selector completes at once,
 * from QAA_UI_Frame as the poll would. */
namespace storage {
constexpr uint32_t DEVICE_IDS = 0x82A7DA54;			// per controller
constexpr uint32_t PENDING = 0x82C01C3C, MAIN_PENDING = 0x82C01C38, CONTROLLER = 0x827EC0A0;
constexpr uint32_t SAVE_DIRTY = 0x82C01C30;
constexpr uint32_t PC_DEVICE = 1;
constexpr uint32_t QUEUE_EVENT = 0x82191098, QUEUE_INVOKE_MAIN = 0x8218E8A8;
constexpr uint32_t S_COMPLETE = 0x820D13F8, S_D_D = 0x820BAD84;
constexpr uint32_t S_MAIN_COMPLETE = 0x820D13C4, S_S_S = 0x820BAD3C;
}

void H_ShowDeviceSelector( void *, qaa_hook_call *c ) {
	const int32_t controller = static_cast<int32_t>( R( c, 0 ) );
	if ( QAA_LoadU32( storage::PENDING ) || controller < 0 || controller >= 4 ) {
		return;
	}
	QAA_StoreU32( storage::SAVE_DIRTY, 1 );
	QAA_StoreU32( storage::DEVICE_IDS + 4 * controller, 0 );
	QAA_StoreU32( storage::CONTROLLER, static_cast<uint32_t>( controller ) );
	QAA_StoreU32( storage::PENDING, 1 );
}

/* The completion half of 0x8218C890, for H_ShowDeviceSelector. The second
 * value is retail's "the save container opened" (0x8218B1F0 == 0); the
 * PC's container always opens. */
void PollDeviceSelection() {
	if ( !QAA_LoadU32( storage::PENDING ) ) {
		return;
	}
	const uint32_t controller = QAA_LoadU32( storage::CONTROLLER );
	QAA_StoreU32( storage::PENDING, 0 );
	QAA_StoreU32( storage::DEVICE_IDS + 4 * controller, storage::PC_DEVICE );
	QAA_StoreU32( storage::SAVE_DIRTY, 0 );
	if ( QAA_LoadU32( storage::MAIN_PENDING ) == 1 ) {
		const uint32_t user = GStrdup( va( "%u", controller ) ), opened = GStrdup( "1" );
		UiCall( storage::QUEUE_INVOKE_MAIN, { storage::S_MAIN_COMPLETE, storage::S_S_S, user, opened } );
		GFree( user );
		GFree( opened );
	} else {
		UiCall( storage::QUEUE_EVENT, { storage::S_COMPLETE, storage::S_D_D, controller, 1 } );
	}
	QAA_StoreU32( storage::CONTROLLER, 0xFFFFFFFFu );
	QAA_StoreU32( storage::MAIN_PENDING, 0 );
}

/* The profile save container. Retail keeps each profile's settings in a
 * 0x2680-byte block (0x82A80318) in an Xbox save container: "startLoad"
 * (0x8218BCE8) fires ConfigLoadStarted and opens it, "startSave"
 * (0x8218BDB0) writes it, and the frame poll's state machine (0x8218C6xx)
 * reports "<op>Complete" "%d" with the result. PC deviation: the settings
 * the menus change are cvars the engine keeps in its own config, so the
 * container operations succeed with nothing to read or write; their
 * completion is reported from QAA_UI_Frame like the poll's. */
namespace config {
constexpr uint32_t SAVE_ENABLED = 0x827EC05C, APPLY_DEFAULTS = 0x82C01C10, APPLY_CONTROLLER = 0x827EC0F4;
constexpr uint32_t FIRE_EVENT = 0x82190FE8, QUEUE_EVENT = 0x82191098, S_D = 0x820BC7B8;
constexpr uint32_t S_SAVE_COMPLETE = 0x820D1320;
enum { NONE, LOAD, SAVE } pending = NONE;
uint32_t loadStarted, loadComplete;		// guest copies of the built names

uint32_t GuestName( uint32_t &slot, const char *name ) {
	if ( !slot ) {
		slot = GStrdup( name );
	}
	return slot;
}
}

void H_StartLoad( void *, qaa_hook_call * ) {
	UiCall( config::FIRE_EVENT, { config::GuestName( config::loadStarted, "ConfigLoadStarted" ) } );
	config::pending = config::LOAD;
}

/* (apply): returns whether a save began; retail returns its argument when
 * saving is off. */
void H_StartSave( void *, qaa_hook_call *c ) {
	const uint32_t apply = R( c, 0 );
	if ( !QAA_LoadU32( config::SAVE_ENABLED ) ) {
		c->r[0] = apply;
		return;
	}
	if ( apply ) {
		QAA_StoreU32( config::APPLY_DEFAULTS, 1 );
		const uint32_t live = QAA_LoadU32( addr::PROFILE_PTR );
		QAA_StoreU32( config::APPLY_CONTROLLER, live ? QAA_LoadU32( live + 0x20 ) : 0 );
	}
	config::pending = config::SAVE;
	c->r[0] = 1;
}

void PollConfigOperation() {
	if ( config::pending == config::NONE ) {
		return;
	}
	const uint32_t name = config::pending == config::LOAD
		? config::GuestName( config::loadComplete, "ConfigLoadComplete" ) : config::S_SAVE_COMPLETE;
	config::pending = config::NONE;
	UiCall( config::QUEUE_EVENT, { name, config::S_D, 0 } );
}

/* Leaving a game. FgApi.exitGame (0x82190D98) only flags the session
 * (bit 3 at +0x1FC of the session block at *0x82C01C04; bit 4 for Live
 * games) and signals it; retail's session manager (0x82176370) then ends
 * it (0x82177AB0): the client disconnects and the server stops, the menus
 * go back to the main menu's stack (0x821779E0) and the client state is
 * reset for them (0x821771F0). PC deviation: there are no Xbox Live
 * sessions, so the flag is polled here and the session end is FnQL's
 * disconnect; retail's course-results variant of the stack path (a
 * finished single-player course) is not taken. */
namespace session {
constexpr uint32_t FLAGS = 0x1FC, END = 0x08, END_LIVE = 0x10;
constexpr uint32_t GAME_MODE = 0x82C01C44, MODE_ARENA = 2;
constexpr uint32_t QUEUE_INVOKE_MAIN = 0x8218E8A8, S_GOTO_STACK_PATH = 0x820D1824, S_S_S = 0x820BAD3C;
constexpr uint32_t S_ARENA_STACK = 0x820CF744, S_SINGLEPLAYER_STACK = 0x820CF760, S_MAINMENU = 0x820D15C4;
}

void SyncKeyCatchers();

void PollSessionEnd() {
	const uint32_t block = QAA_LoadU32( addr::LIVE_STATE_PTR );
	if ( !block ) {
		return;
	}
	const uint32_t flags = QAA_LoadU32( block + session::FLAGS );
	if ( !( flags & ( session::END | session::END_LIVE ) ) ) {
		return;
	}
	QAA_StoreU32( block + session::FLAGS, flags & ~( session::END | session::END_LIVE ) );
	Cbuf_ExecuteText( EXEC_APPEND, "disconnect\n" );
	const uint32_t stack = QAA_LoadU32( session::GAME_MODE ) == session::MODE_ARENA
		? session::S_ARENA_STACK : session::S_SINGLEPLAYER_STACK;
	UiCall( session::QUEUE_INVOKE_MAIN, { session::S_GOTO_STACK_PATH, session::S_S_S, stack, session::S_MAINMENU } );
	Cvar_Set( "cl_paused", "0" );
	Key_SetCatcher( Key_GetCatcher() | KEYCATCH_UI );
	SyncKeyCatchers();
	Cvar_Set( "ui_messageReady", "1" );
	Cvar_Set( "team", "" );
}

/* (leave): the post-game's Exit Match (FSCommand "dismissPostGame") leaves
 * through 0x821783D0, which flags the session as exitGame does: bit 4 for a
 * Live game (ui_sessionType 3 or more), otherwise bit 3 when leave is set,
 * then signals it. The flag is polled (PollSessionEnd). */
void H_QnetLeaveGame( void *, qaa_hook_call *c ) {
	const uint32_t block = QAA_LoadU32( addr::LIVE_STATE_PTR );
	if ( !block ) {
		return;
	}
	const uint32_t flag = Cvar_VariableIntegerValue( "ui_sessionType" ) >= 3 ? session::END_LIVE
		: R( c, 0 ) ? session::END : 0;
	QAA_StoreU32( block + session::FLAGS, QAA_LoadU32( block + session::FLAGS ) | flag );
}

/* Hosting a match. Host Match's Start Match (FSCommand "startServer",
 * 0x82192740) signals QNET's session thread to host (the event at +0x778
 * of QNET's globals, 0x82C01E20) and calls 0x82175A28, which shows
 * "Joining server" (the WaitBegin event), gives the keys back to the game
 * and signals the session block's event at +0xBC. The session thread hosts
 * (0x8217D8D8), opens the lobby (JoinServer) and, when the host starts the
 * match there, reaches QNET_STATE_GAME_PLAY, where the host runs ui_main's
 * server start (0x8219F5D8: g_gametype, the bots, then "wait ; wait ; map
 * <arena>"; docs/NETWORK.md). The session's own steps close the wait box
 * again (WaitEnd, e.g. 0x82176E1C, or popWait, 0x82180C90). Within
 * the front end only startServer calls 0x82175A28 (its other callers are
 * QNET's and the engine's).
 * PC deviation: there is no QNET session layer yet (M8), so the match
 * starts as soon as it is hosted, as the host's session does on reaching
 * play: a listen server that others join with connect, with no lobby,
 * invites or session. */
namespace hosting {
constexpr uint32_t START_SERVER = 0x8219F5D8;		// ui_main: the host's server start
constexpr uint32_t QUEUE_EVENT = 0x82191098;		// (name, format, ...)
constexpr uint32_t FIRE_EVENT = 0x82190FE8;		// (name), queued after QUEUE_EVENT's
constexpr uint32_t S_WAIT_BEGIN = 0x820CF568, S_D_S = 0x820BAD10, S_JOINING_SERVER = 0x820CF558;
constexpr uint32_t S_WAIT_END = 0x820CF574;
bool pending;
}

void H_QnetJoinWait( void *, qaa_hook_call * ) {
	UiCall( hosting::QUEUE_EVENT, { hosting::S_WAIT_BEGIN, hosting::S_D_S, 0, hosting::S_JOINING_SERVER } );
	QAA_StoreU32( addr::CLS_KEYCATCHERS, QAA_LoadU32( addr::CLS_KEYCATCHERS ) & ~static_cast<uint32_t>( KEYCATCH_UI ) );
	hosting::pending = true;
}

void PollHostStart() {
	if ( !hosting::pending ) {
		return;
	}
	hosting::pending = false;
	UiCall( hosting::FIRE_EVENT, { hosting::S_WAIT_END } );
	UiCall( hosting::START_SERVER, {} );
}

/* -- R_GetCommandBuffer --------------------------------------------------------------- */

/* SFUI's renderer queues "render the UI" for the engine's back end at the
 * end of a frame (0x82166F70). FnQAA replays the frame right after
 * SFUI's render returns (QAA_UI_Draw), so the queued command only needs
 * somewhere to go. */
uint32_t renderCommandScratch;

void H_R_GetCommandBuffer( void *, qaa_hook_call *c ) {
	if ( !renderCommandScratch ) {
		renderCommandScratch = GCalloc( 0x400 );
	}
	c->r[0] = R( c, 0 ) <= 0x400 ? renderCommandScratch : 0;
}

/* ==== textures ===================================================================== */

/*
 * Host textures, by id (index + 1). SFUI creates, changes and frees them on
 * its update thread and on its own loader threads, which run alongside the
 * engine, while QAA_UI_Draw looks them up and the canvas samples them. So:
 * - the table is locked;
 * - every change gives the texture a new version once its texels are
 *   written (TextureChanged), and the canvas redraws the bands whose
 *   triangles sampled an older one;
 * - on the drawing thread, triangles the canvas has queued are rasterised
 *   before a texture they may sample changes or goes (TexturesChanging);
 * - a texture freed on another thread is kept until the next draw starts,
 *   when nothing queued can point at it.
 */
std::mutex textureLock;
std::vector<std::unique_ptr<qaa_ui::Texture>> textures;	// under textureLock
std::vector<std::unique_ptr<qaa_ui::Texture>> retired;		// under textureLock
std::atomic<uint64_t> textureVersions;
std::atomic<std::thread::id> drawThread;	// QAA_UI_Draw's

bool OnDrawThread() {
	return std::this_thread::get_id() == drawThread.load( std::memory_order_relaxed );
}

void TexturesChanging();

void TextureChanged( qaa_ui::Texture *t ) {
	if ( t ) {
		t->version = ++textureVersions;
	}
}

uint32_t NewTexture() {
	std::unique_ptr<qaa_ui::Texture> t( new qaa_ui::Texture );
	t->version = ++textureVersions;
	std::lock_guard<std::mutex> lock( textureLock );
	for ( size_t i = 0; i < textures.size(); i++ ) {
		if ( !textures[i] ) {
			textures[i] = std::move( t );
			return static_cast<uint32_t>( i + 1 );
		}
	}
	textures.push_back( std::move( t ) );
	return static_cast<uint32_t>( textures.size() );
}

qaa_ui::Texture *TextureById( uint32_t id ) {
	std::lock_guard<std::mutex> lock( textureLock );
	return id && id <= textures.size() ? textures[id - 1].get() : nullptr;
}

void FreeTexture( uint32_t id ) {
	if ( !id ) {
		return;
	}
	const bool here = OnDrawThread();
	if ( here ) {
		TexturesChanging();
	}
	std::unique_ptr<qaa_ui::Texture> gone;	// deleted after the lock is let go
	std::lock_guard<std::mutex> lock( textureLock );
	if ( id <= textures.size() ) {
		gone = std::move( textures[id - 1] );
		if ( !here && gone ) {
			retired.push_back( std::move( gone ) );
		}
	}
}

/* At the start of a draw, when nothing queued points at a retired texture. */
void ReleaseRetiredTextures() {
	std::vector<std::unique_ptr<qaa_ui::Texture>> gone;
	std::lock_guard<std::mutex> lock( textureLock );
	gone.swap( retired );
}

/* GFx 2.x GImage formats (GImageBase: format, width, height, pitch, data,
 * data size, mip levels; device texture 0x8216B6D8). */
enum { GIMG_RGBA = 1, GIMG_RGB = 2, GIMG_A8 = 9, GIMG_DXT1 = 10, GIMG_DXT3 = 11, GIMG_DXT5 = 12 };

bool LoadGImage( qaa_ui::Texture &t, uint32_t img ) {
	const int format = QAA_LoadS32( img + 0 );
	const int w = QAA_LoadS32( img + 4 ), h = QAA_LoadS32( img + 8 );
	const int pitch = QAA_LoadS32( img + 12 );
	const uint32_t data = QAA_LoadU32( img + 16 );
	if ( w <= 0 || h <= 0 || w > 8192 || h > 8192 || !data ) {
		return false;
	}
	t.width = w;
	t.height = h;
	t.alphaOnly = format == GIMG_A8;
	t.rgba.assign( static_cast<size_t>( w ) * h * 4, 255 );
	const uint8_t *src = QAA_Ptr( data ) ? static_cast<const uint8_t *>( QAA_Ptr( data ) ) : nullptr;
	switch ( format ) {
	case GIMG_RGBA:
		for ( int y = 0; y < h; y++ ) {
			std::memcpy( &t.rgba[static_cast<size_t>( y ) * w * 4], src + static_cast<size_t>( y ) * pitch, static_cast<size_t>( w ) * 4 );
		}
		return true;
	case GIMG_RGB:
		for ( int y = 0; y < h; y++ ) {
			const uint8_t *s = src + static_cast<size_t>( y ) * pitch;
			uint8_t *d = &t.rgba[static_cast<size_t>( y ) * w * 4];
			for ( int x = 0; x < w; x++ ) {
				d[x * 4 + 0] = s[x * 3 + 0];
				d[x * 4 + 1] = s[x * 3 + 1];
				d[x * 4 + 2] = s[x * 3 + 2];
				d[x * 4 + 3] = 255;
			}
		}
		return true;
	case GIMG_A8:
		for ( int y = 0; y < h; y++ ) {
			const uint8_t *s = src + static_cast<size_t>( y ) * pitch;
			for ( int x = 0; x < w; x++ ) {
				t.rgba[( static_cast<size_t>( y ) * w + x ) * 4 + 3] = s[x];
			}
		}
		return true;
	case GIMG_DXT1:
	case GIMG_DXT3:
	case GIMG_DXT5:
		return qaa_ui::DecodeDXT( format, src, w, h, t.rgba );
	default:
		Com_DPrintf( "QAA ui: GImage format %d not supported\n", format );
		return false;
	}
}

/* An update of part of a texture from a GImage (the glyph cache). */
void UpdateFromGImage( qaa_ui::Texture &t, uint32_t img, int dx, int dy, int sx0, int sy0, int sx1, int sy1 ) {
	const int format = QAA_LoadS32( img + 0 );
	const int pitch = QAA_LoadS32( img + 12 );
	const uint8_t *src = static_cast<const uint8_t *>( QAA_Ptr( QAA_LoadU32( img + 16 ) ) );
	if ( !src || t.rgba.empty() ) {
		return;
	}
	const int bpp = format == GIMG_RGBA ? 4 : format == GIMG_RGB ? 3 : 1;
	for ( int y = sy0; y < sy1; y++ ) {
		const int ty = dy + ( y - sy0 );
		if ( ty < 0 || ty >= t.height ) {
			continue;
		}
		for ( int x = sx0; x < sx1; x++ ) {
			const int tx = dx + ( x - sx0 );
			if ( tx < 0 || tx >= t.width ) {
				continue;
			}
			const uint8_t *s = src + static_cast<size_t>( y ) * pitch + static_cast<size_t>( x ) * bpp;
			uint8_t *d = &t.rgba[( static_cast<size_t>( ty ) * t.width + tx ) * 4];
			if ( bpp == 1 ) {
				d[0] = d[1] = d[2] = 255;
				d[3] = s[0];
			} else {
				d[0] = s[0];
				d[1] = s[1];
				d[2] = s[2];
				d[3] = bpp == 4 ? s[3] : 255;
			}
		}
	}
}

/* -- D3D textures SFUI makes itself --------------------------------------------------- */

/*
 * The bot pictures and the "who's that" silhouettes (0x82193B78, 0x82193CC0)
 * are D3D textures SFUI creates, locks and fills, or loads from TGA files
 * with D3DX, before handing them to GFx. FnQAA gives those calls host
 * textures. The guest sees an opaque block: 'QTEX', the host texture id,
 * width, height, a reference count and the locked pixels.
 */
namespace d3dtex {
constexpr uint32_t MAGIC = 0x51544558;	// 'QTEX'
constexpr uint32_t OFF_MAGIC = 0, OFF_ID = 4, OFF_W = 8, OFF_H = 12, OFF_REFS = 16, OFF_PIXELS = 20, SIZE = 0x40;
}

uint32_t NewD3DTexture( int w, int h ) {
	const uint32_t g = GCalloc( d3dtex::SIZE );
	if ( !g ) {
		return 0;
	}
	const uint32_t id = NewTexture();
	qaa_ui::Texture *t = TextureById( id );
	t->width = w;
	t->height = h;
	t->rgba.assign( static_cast<size_t>( w ) * h * 4, 0 );
	TextureChanged( t );
	QAA_StoreU32( g + d3dtex::OFF_MAGIC, d3dtex::MAGIC );
	QAA_StoreU32( g + d3dtex::OFF_ID, id );
	QAA_StoreS32( g + d3dtex::OFF_W, w );
	QAA_StoreS32( g + d3dtex::OFF_H, h );
	QAA_StoreU32( g + d3dtex::OFF_REFS, 1 );
	return g;
}

bool IsD3DTexture( uint32_t g ) {
	return g && QAA_LoadU32( g + d3dtex::OFF_MAGIC ) == d3dtex::MAGIC;
}

void H_D3D_CreateTexture( void *, qaa_hook_call *c ) {
	const int w = static_cast<int>( R( c, 0 ) ), h = static_cast<int>( R( c, 1 ) );
	c->r[0] = w > 0 && h > 0 && w <= 4096 && h <= 4096 ? NewD3DTexture( w, h ) : 0;
}

void H_D3D_LockRect( void *, qaa_hook_call *c ) {
	const uint32_t g = R( c, 0 ), out = R( c, 2 );
	if ( !IsD3DTexture( g ) || !out ) {
		c->r[0] = 0x8876086Cu;	// D3DERR_INVALIDCALL
		return;
	}
	const int w = QAA_LoadS32( g + d3dtex::OFF_W ), h = QAA_LoadS32( g + d3dtex::OFF_H );
	uint32_t px = QAA_LoadU32( g + d3dtex::OFF_PIXELS );
	if ( !px ) {
		px = GCalloc( static_cast<uint32_t>( w * h * 4 ) );
		QAA_StoreU32( g + d3dtex::OFF_PIXELS, px );
	}
	QAA_StoreS32( out, w * 4 );
	QAA_StoreU32( out + 4, px );
	c->r[0] = 0;
}

void H_D3D_Unlock( void *, qaa_hook_call *c ) {
	const uint32_t g = R( c, 0 );
	if ( !IsD3DTexture( g ) ) {
		return;
	}
	qaa_ui::Texture *t = TextureById( QAA_LoadU32( g + d3dtex::OFF_ID ) );
	const uint32_t px = QAA_LoadU32( g + d3dtex::OFF_PIXELS );
	if ( t && px && !t->rgba.empty() ) {
		TexturesChanging();
		std::memcpy( t->rgba.data(), QAA_Ptr( px ), t->rgba.size() );
		TextureChanged( t );
	}
}

void H_D3D_Release( void *, qaa_hook_call *c ) {
	const uint32_t g = R( c, 0 );
	if ( !IsD3DTexture( g ) ) {
		c->r[0] = 0;
		return;
	}
	const uint32_t refs = QAA_LoadU32( g + d3dtex::OFF_REFS );
	if ( refs > 1 ) {
		QAA_StoreU32( g + d3dtex::OFF_REFS, refs - 1 );
		c->r[0] = refs - 1;
		return;
	}
	FreeTexture( QAA_LoadU32( g + d3dtex::OFF_ID ) );
	GFree( QAA_LoadU32( g + d3dtex::OFF_PIXELS ) );
	QAA_StoreU32( g + d3dtex::OFF_MAGIC, 0 );
	GFree( g );
	c->r[0] = 0;
}

void H_D3D_LevelDesc( void *, qaa_hook_call *c ) {
	const uint32_t g = R( c, 0 ), desc = R( c, 2 );
	if ( !IsD3DTexture( g ) || !desc ) {
		return;
	}
	// D3DSURFACE_DESC as the Xbox SDK lays it out: Format .. Width, Height
	std::memset( QAA_Ptr( desc ), 0, 0x20 );
	QAA_StoreU32( desc + 0, 0x18280186 );	// D3DFMT_A8R8G8B8
	QAA_StoreS32( desc + 0x18, QAA_LoadS32( g + d3dtex::OFF_W ) );
	QAA_StoreS32( desc + 0x1C, QAA_LoadS32( g + d3dtex::OFF_H ) );
}

/* D3DXCreateTextureFromFileInMemoryEx(device, data, size, w, h, mips, usage,
 * format, pool, filter, mip filter, colour key, info, palette, texture*):
 * the last seven on the stack. SFUI's image loader (0x82194CE8) reads the
 * size from the D3DXIMAGE_INFO (Width, Height, Depth, MipLevels, Format,
 * ResourceType, ImageFileFormat) when the movie gives none. The files are
 * DDS (the menus) or TGA (the bot pictures). */
void H_D3DX_TextureFromMemory( void *, qaa_hook_call *c ) {
	const uint32_t data = R( c, 1 ), size = R( c, 2 );
	const uint32_t info = StackArg( c, 4 ), out = StackArg( c, 6 );
	qaa_ui::Texture decoded;
	const uint8_t *bytes = data ? static_cast<const uint8_t *>( QAA_Ptr( data ) ) : nullptr;
	const bool dds = bytes && size >= 4 && !std::memcmp( bytes, "DDS ", 4 );
	if ( !bytes || !size || !( dds ? qaa_ui::DecodeDDS( bytes, size, decoded ) : qaa_ui::DecodeTGA( bytes, size, decoded ) ) ) {
		if ( out ) {
			QAA_StoreU32( out, 0 );
		}
		c->r[0] = 0x8876086Cu;
		return;
	}
	if ( info ) {
		std::memset( QAA_Ptr( info ), 0, 0x1C );
		QAA_StoreS32( info + 0x00, decoded.width );
		QAA_StoreS32( info + 0x04, decoded.height );
		QAA_StoreS32( info + 0x08, 1 );
		QAA_StoreS32( info + 0x0C, 1 );
		QAA_StoreU32( info + 0x10, 0x18280186 );			// D3DFMT_A8R8G8B8
		QAA_StoreU32( info + 0x14, 3 );					// D3DRTYPE_TEXTURE
		QAA_StoreU32( info + 0x18, dds ? 4 : 2 );		// D3DXIFF_DDS, D3DXIFF_TGA
	}
	const uint32_t g = NewD3DTexture( decoded.width, decoded.height );
	if ( g ) {
		qaa_ui::Texture *t = TextureById( QAA_LoadU32( g + d3dtex::OFF_ID ) );
		t->rgba = std::move( decoded.rgba );
		TextureChanged( t );
	}
	if ( out ) {
		QAA_StoreU32( out, g );
	}
	c->r[0] = g ? 0 : 0x8876086Cu;
}

/* ==== the renderer back end ========================================================== */

/*
 * SFUI's GRenderer (vtable 0x820CB800) records commands into a double-
 * buffered list; the object at renderer +0x10 (QAA's "SFUI-Backend", ctor
 * 0x821687F0, vtable 0x820CE37C) creates the device textures and replays the
 * list on the render thread. Both are replaced by host objects here; the
 * recorder stays the retail one.
 *
 * Backend vtable: 0 CreateDeviceTexture(gtexture), 1 UpdateTexture(level,
 * rect, image, devtex), 2 InitDevice, 3 Execute, 4 RestoreDevice.
 * Device texture (0x1C bytes, vtable 0x820CE390): +4 the backend, +0xC
 * width, +0x10 height, +0x14 the texture (here a host texture id), +0x18
 * format. Methods: 0 InitDynamic(w, h, format, mips, usage, tw, th),
 * 1 InitFromImage(image, tw, th), 2 InitFromD3D(d3dtex, w, h), 3 width,
 * 4 height, 5 Update(level, rect, data, format), 6 valid, 7 destructor,
 * 8 field +8, 9 (unused), 10 texture.
 */
uint32_t backendObject;
uint32_t backendVtable;
uint32_t devtexVtable;
uint32_t sfuiRenderer;

namespace devtex {
constexpr uint32_t OWNER = 0x04, W = 0x0C, H = 0x10, TEX = 0x14, FORMAT = 0x18, SIZE = 0x1C;
}

int64_t ArgThis( const qaa_syscall *c ) {
	return static_cast<uint32_t>( c->trap );
}

/* qaa_uiTrace 4: every call the recorder and GFx make on the host objects */
void TraceCall( const char *name, const qaa_syscall *c ) {
	if ( qaa_uiTrace && qaa_uiTrace->integer > 3 ) {
		Com_Printf( "QAA ui: %s(%08X: %08X %08X %08X %08X %08X %08X)\n", name, static_cast<uint32_t>( c->trap ),
			c->args[0], c->args[1], c->args[2], c->args[3], c->args[4], c->args[5] );
	}
}

int64_t DT_InitDynamic( void *, qaa_module *, const qaa_syscall *c ) {
	TraceCall( "DT_InitDynamic", c );
	const uint32_t self = static_cast<uint32_t>( c->trap );
	int w = c->args[0], h = c->args[1];
	const int format = c->args[2];
	if ( c->args[4] ) {
		w = c->args[4];
	}
	if ( c->args[5] ) {
		h = c->args[5];
	}
	if ( w <= 0 || h <= 0 || w > 8192 || h > 8192 ) {
		return 0;
	}
	FreeTexture( QAA_LoadU32( self + devtex::TEX ) );
	const uint32_t id = NewTexture();
	qaa_ui::Texture *t = TextureById( id );
	t->width = w;
	t->height = h;
	t->alphaOnly = format == GIMG_A8;
	t->rgba.assign( static_cast<size_t>( w ) * h * 4, 0 );
	if ( t->alphaOnly ) {
		for ( size_t i = 0; i < t->rgba.size(); i += 4 ) {
			t->rgba[i] = t->rgba[i + 1] = t->rgba[i + 2] = 255;
		}
	}
	TextureChanged( t );
	QAA_StoreU32( self + devtex::TEX, id );
	QAA_StoreS32( self + devtex::W, w );
	QAA_StoreS32( self + devtex::H, h );
	QAA_StoreS32( self + devtex::FORMAT, format );
	return 1;
}

int64_t DT_InitFromImage( void *, qaa_module *, const qaa_syscall *c ) {
	TraceCall( "DT_InitFromImage", c );
	const uint32_t self = static_cast<uint32_t>( c->trap );
	const uint32_t img = static_cast<uint32_t>( c->args[0] );
	FreeTexture( QAA_LoadU32( self + devtex::TEX ) );
	QAA_StoreU32( self + devtex::TEX, 0 );
	if ( !img ) {
		return 1;
	}
	const uint32_t id = NewTexture();
	if ( !LoadGImage( *TextureById( id ), img ) ) {
		FreeTexture( id );
		return 0;
	}
	qaa_ui::Texture *t = TextureById( id );
	TextureChanged( t );
	QAA_StoreU32( self + devtex::TEX, id );
	QAA_StoreS32( self + devtex::W, c->args[1] ? c->args[1] : t->width );
	QAA_StoreS32( self + devtex::H, c->args[2] ? c->args[2] : t->height );
	QAA_StoreS32( self + devtex::FORMAT, QAA_LoadS32( img ) );
	return 1;
}

int64_t DT_InitFromD3D( void *, qaa_module *, const qaa_syscall *c ) {
	TraceCall( "DT_InitFromD3D", c );
	const uint32_t self = static_cast<uint32_t>( c->trap );
	const uint32_t g = static_cast<uint32_t>( c->args[0] );
	FreeTexture( QAA_LoadU32( self + devtex::TEX ) );
	QAA_StoreU32( self + devtex::TEX, 0 );
	if ( !IsD3DTexture( g ) ) {
		return 1;
	}
	// share the D3D texture's pixels: a copy, as the device texture owns its id
	const uint32_t id = NewTexture();
	qaa_ui::Texture *t = TextureById( id );
	if ( const qaa_ui::Texture *from = TextureById( QAA_LoadU32( g + d3dtex::OFF_ID ) ) ) {
		*t = *from;
	}
	TextureChanged( t );	// a version of its own
	QAA_StoreU32( self + devtex::TEX, id );
	QAA_StoreS32( self + devtex::W, c->args[1] );
	QAA_StoreS32( self + devtex::H, c->args[2] );
	return 1;
}

int64_t DT_Width( void *, qaa_module *, const qaa_syscall *c ) {
	return QAA_LoadU32( static_cast<uint32_t>( c->trap ) + devtex::W );
}
int64_t DT_Height( void *, qaa_module *, const qaa_syscall *c ) {
	return QAA_LoadU32( static_cast<uint32_t>( c->trap ) + devtex::H );
}
int64_t DT_Valid( void *, qaa_module *, const qaa_syscall *c ) {
	return QAA_LoadU32( static_cast<uint32_t>( c->trap ) + devtex::TEX ) != 0;
}
int64_t DT_Field8( void *, qaa_module *, const qaa_syscall *c ) {
	return QAA_LoadU32( static_cast<uint32_t>( c->trap ) + 8 );
}
int64_t DT_Texture( void *, qaa_module *, const qaa_syscall *c ) {
	return QAA_LoadU32( static_cast<uint32_t>( c->trap ) + devtex::TEX );
}
int64_t DT_Nothing( void *, qaa_module *, const qaa_syscall * ) {
	return 0;
}

/* Update(level, rect {left, top, right, bottom} at +8, data, format) */
int64_t DT_Update( void *, qaa_module *, const qaa_syscall *c ) {
	TraceCall( "DT_Update", c );
	const uint32_t self = static_cast<uint32_t>( c->trap );
	qaa_ui::Texture *t = TextureById( QAA_LoadU32( self + devtex::TEX ) );
	const uint32_t rect = static_cast<uint32_t>( c->args[1] );
	const uint8_t *src = static_cast<const uint8_t *>( QAA_Ptr( static_cast<uint32_t>( c->args[2] ) ) );
	const int format = c->args[3];
	if ( !t || !rect || !src ) {
		return 0;
	}
	TexturesChanging();
	const int l = QAA_LoadS32( rect + 8 ), tp = QAA_LoadS32( rect + 12 ), r = QAA_LoadS32( rect + 16 ), b = QAA_LoadS32( rect + 20 );
	const int bpp = format == GIMG_RGBA ? 4 : format == GIMG_RGB ? 3 : 1;
	const int w = r - l;
	for ( int y = tp; y < b; y++ ) {
		for ( int x = l; x < r; x++ ) {
			if ( x < 0 || y < 0 || x >= t->width || y >= t->height ) {
				continue;
			}
			const uint8_t *s = src + ( static_cast<size_t>( y - tp ) * w + ( x - l ) ) * bpp;
			uint8_t *d = &t->rgba[( static_cast<size_t>( y ) * t->width + x ) * 4];
			if ( bpp == 1 ) {
				d[0] = d[1] = d[2] = 255;
				d[3] = s[0];
			} else {
				d[0] = s[0];
				d[1] = s[1];
				d[2] = s[2];
				d[3] = bpp == 4 ? s[3] : 255;
			}
		}
	}
	TextureChanged( t );
	return 1;
}

/* The scalar deleting destructor (flag 1: free). */
int64_t DT_Destroy( void *, qaa_module *, const qaa_syscall *c ) {
	TraceCall( "DT_Destroy", c );
	const uint32_t self = static_cast<uint32_t>( c->trap );
	FreeTexture( QAA_LoadU32( self + devtex::TEX ) );
	QAA_StoreU32( self + devtex::TEX, 0 );
	if ( c->args[0] & 1 ) {
		GFree( self );
	}
	return self;
}

int64_t BE_CreateDeviceTexture( void *, qaa_module *, const qaa_syscall *c ) {
	TraceCall( "BE_CreateDeviceTexture", c );
	const uint32_t t = GCalloc( devtex::SIZE );
	if ( t ) {
		QAA_StoreU32( t, devtexVtable );
		QAA_StoreU32( t + devtex::OWNER, static_cast<uint32_t>( c->trap ) );
	}
	return t;
}

/* UpdateTexture(level, rect {dest x, y, src left, top, right, bottom}, image, devtex) */
int64_t BE_UpdateTexture( void *, qaa_module *, const qaa_syscall *c ) {
	TraceCall( "BE_UpdateTexture", c );
	const uint32_t rect = static_cast<uint32_t>( c->args[1] );
	const uint32_t img = static_cast<uint32_t>( c->args[2] );
	const uint32_t dev = static_cast<uint32_t>( c->args[3] );
	qaa_ui::Texture *t = dev ? TextureById( QAA_LoadU32( dev + devtex::TEX ) ) : nullptr;
	if ( !t || !rect || !img ) {
		return 0;
	}
	TexturesChanging();
	UpdateFromGImage( *t, img, QAA_LoadS32( rect + 0 ), QAA_LoadS32( rect + 4 ), QAA_LoadS32( rect + 8 ),
		QAA_LoadS32( rect + 12 ), QAA_LoadS32( rect + 16 ), QAA_LoadS32( rect + 20 ) );
	TextureChanged( t );
	return 1;
}

void ExecuteCommands();

int64_t BE_Execute( void *, qaa_module *, const qaa_syscall * ) {
	ExecuteCommands();
	return 0;
}

/* The backend constructor (this, renderer): the host object instead. */
void H_BackendCtor( void *, qaa_hook_call *c ) {
	const uint32_t self = R( c, 0 );
	sfuiRenderer = R( c, 1 );
	if ( !backendVtable ) {
		static const qaa_syscall_fn be[5] = { BE_CreateDeviceTexture, BE_UpdateTexture, DT_Nothing, BE_Execute, DT_Nothing };
		static const qaa_syscall_fn dt[11] = { DT_InitDynamic, DT_InitFromImage, DT_InitFromD3D, DT_Width, DT_Height,
			DT_Update, DT_Valid, DT_Destroy, DT_Field8, DT_Nothing, DT_Texture };
		backendVtable = GCalloc( sizeof( be ) / sizeof( be[0] ) * 4 );
		devtexVtable = GCalloc( sizeof( dt ) / sizeof( dt[0] ) * 4 );
		for ( size_t i = 0; i < 5; i++ ) {
			QAA_StoreU32( backendVtable + 4 * static_cast<uint32_t>( i ), Api().qaa_module_host_function( uiModule, be[i], nullptr ) );
		}
		for ( size_t i = 0; i < 11; i++ ) {
			QAA_StoreU32( devtexVtable + 4 * static_cast<uint32_t>( i ), Api().qaa_module_host_function( uiModule, dt[i], nullptr ) );
		}
	}
	std::memset( QAA_Ptr( self ), 0, 0x22A4 );
	QAA_StoreU32( self, backendVtable );
	QAA_StoreU32( self + 0x260, sfuiRenderer );
	backendObject = self;
	c->r[0] = self;
}

/* ==== replay ======================================================================== */

qaa_ui::Canvas canvas;
std::vector<uint8_t> presentBuffer;
bool canvasDrawn;
bool presentValid;		// presentBuffer holds the last frame drawn

/* Before a texture changes or goes: on the drawing thread, rasterise the
 * triangles still queued, which may sample its old texels. */
void TexturesChanging() {
	if ( OnDrawThread() && canvas.HasQueued() ) {
		canvas.Flush();
	}
}

int dumpRequests;		// qaa_uiDump
int framesDrawn;
bool updatesPending;	// the movies advanced since the last draw

/* Writes the front end's last frame as a TGA (straight alpha), for tests
 * and for checking the replay without the renderer. */
void DumpCanvas( const char *name ) {
	const int w = canvas.Width(), h = canvas.Height();
	std::vector<uint8_t> straight;
	canvas.ToStraight( straight );
	std::vector<uint8_t> tga( 18 + static_cast<size_t>( w ) * h * 4 );
	tga[2] = 2;
	tga[12] = static_cast<uint8_t>( w & 255 );
	tga[13] = static_cast<uint8_t>( w >> 8 );
	tga[14] = static_cast<uint8_t>( h & 255 );
	tga[15] = static_cast<uint8_t>( h >> 8 );
	tga[16] = 32;
	tga[17] = 0x28;	// top-left origin, 8 alpha bits
	for ( size_t i = 0; i < static_cast<size_t>( w ) * h; i++ ) {
		tga[18 + i * 4 + 0] = straight[i * 4 + 2];
		tga[18 + i * 4 + 1] = straight[i * 4 + 1];
		tga[18 + i * 4 + 2] = straight[i * 4 + 0];
		tga[18 + i * 4 + 3] = straight[i * 4 + 3];
	}
	FS_WriteFile( name, tga.data(), static_cast<int>( tga.size() ) );
	Com_Printf( "QAA ui: wrote %s (%dx%d, %d triangles)\n", name, w, h, canvas.Triangles() );
}

void UI_Dump_f( void ) {
	dumpRequests++;
}

/* Recorded commands (the recorder, 0x82166F70..0x82167CC8, and the
 * replay, 0x8216AD08). */
enum {
	CMD_END = 0, CMD_BEGIN_DISPLAY = 1, CMD_END_DISPLAY = 2, CMD_DRAW_BITMAPS = 3, CMD_VERTEX_DATA = 4,
	CMD_INDEX_DATA = 5, CMD_DRAW_TRIANGLES = 6, CMD_DRAW_LINES = 7, CMD_FILL_DISABLE = 8, CMD_LINE_DISABLE = 9,
	CMD_FILL_COLOR = 10, CMD_LINE_COLOR = 11, CMD_FILL_BITMAP = 12, CMD_FILL_GOURAUD = 13, CMD_PUSH_BLEND = 14,
	CMD_POP_BLEND = 15, CMD_MATRIX = 16, CMD_USER_MATRIX = 17, CMD_CXFORM = 18, CMD_BEGIN_MASK = 19,
	CMD_END_MASK = 20, CMD_DISABLE_MASK = 21, CMD_FRAME_END = 22,
};

/* GFx 2.x vertex formats and sizes (0x821677B8) */
enum { VF_NONE = 0, VF_XY16I = 1, VF_XY32F = 2, VF_XY16I_C32 = 3, VF_XY16I_CF32 = 4 };

struct ReplayState {
	std::vector<qaa_ui::Vertex> vertices;
	std::vector<uint32_t> indices;
	qaa_ui::Fill fill;
	bool lineOn = false;
	float lineColor[4] = { 1, 1, 1, 1 };
};

/* GColor is 0xAARRGGBB. */
void UnpackColor( uint32_t c, float out[4] ) {
	out[0] = ( ( c >> 16 ) & 255 ) * ( 1.0f / 255.0f );
	out[1] = ( ( c >> 8 ) & 255 ) * ( 1.0f / 255.0f );
	out[2] = ( c & 255 ) * ( 1.0f / 255.0f );
	out[3] = ( c >> 24 ) * ( 1.0f / 255.0f );
}

qaa_ui::Matrix LoadMatrix( uint32_t a ) {
	qaa_ui::Matrix m;
	for ( int i = 0; i < 6; i++ ) {
		m.m[i] = QAA_LoadF( a + 4 * i );
	}
	return m;
}

const qaa_ui::Texture *TextureOfDevtex( uint32_t dev ) {
	return dev ? TextureById( QAA_LoadU32( dev + devtex::TEX ) ) : nullptr;
}

/* FillTexture: GTexture*, matrix (6), wrap mode, sample mode. */
void LoadFillTexture( uint32_t a, qaa_ui::TextureFill &t ) {
	const uint32_t gtex = QAA_LoadU32( a );
	const uint32_t dev = gtex ? QAA_LoadU32( gtex + 0x2C ) : 0;
	t.texture = TextureOfDevtex( dev );
	t.uv = LoadMatrix( a + 4 );
	// The matrix maps to texels: the retail back end divides its rows by the
	// device texture's width and height (texgen, 0x82169490).
	if ( dev ) {
		const int w = QAA_LoadS32( dev + devtex::W ), h = QAA_LoadS32( dev + devtex::H );
		for ( int k = 0; k < 3; k++ ) {
			t.uv.m[k] /= w > 0 ? static_cast<float>( w ) : 1.0f;
			t.uv.m[3 + k] /= h > 0 ? static_cast<float>( h ) : 1.0f;
		}
	}
	t.clamp = QAA_LoadU32( a + 28 ) != 0;
	t.linear = QAA_LoadU32( a + 32 ) != 0;
}

int displayW, displayH;	// the movie's viewport buffer, as the last frame set it

void BeginDisplay( uint32_t c ) {
	const float x0 = QAA_LoadF( c + 4 ), x1 = QAA_LoadF( c + 8 ), y0 = QAA_LoadF( c + 12 ), y1 = QAA_LoadF( c + 16 );
	const int bufW = QAA_LoadS32( c + 0x14 ), bufH = QAA_LoadS32( c + 0x18 );
	displayW = bufW;
	displayH = bufH;
	const int left = QAA_LoadS32( c + 0x1C ), top = QAA_LoadS32( c + 0x20 );
	const int width = QAA_LoadS32( c + 0x24 ), height = QAA_LoadS32( c + 0x28 );
	const uint32_t flags = QAA_LoadU32( c + 0x44 );
	if ( qaa_uiTrace->integer > 2 ) {
		Com_Printf( "QAA ui: display [%g %g]x[%g %g] buffer %dx%d viewport %d %d %dx%d flags %X\n", x0, x1, y0, y1,
			bufW, bufH, left, top, width, height, flags );
	}
	const float sx = bufW > 0 ? static_cast<float>( canvas.Width() ) / bufW : 1.0f;
	const float sy = bufH > 0 ? static_cast<float>( canvas.Height() ) / bufH : 1.0f;
	qaa_ui::Matrix v;
	const float dx = x1 - x0, dy = y1 - y0;
	v.m[0] = dx != 0 ? width * sx / dx : 1.0f;
	v.m[1] = 0;
	v.m[2] = left * sx - x0 * v.m[0];
	v.m[3] = 0;
	v.m[4] = dy != 0 ? height * sy / dy : 1.0f;
	v.m[5] = top * sy - y0 * v.m[4];
	int cx0 = static_cast<int>( left * sx ), cy0 = static_cast<int>( top * sy );
	int cx1 = static_cast<int>( std::ceil( ( left + width ) * sx ) ), cy1 = static_cast<int>( std::ceil( ( top + height ) * sy ) );
	if ( flags & 4 ) {	// scissor
		const int sl = QAA_LoadS32( c + 0x2C ), st = QAA_LoadS32( c + 0x30 );
		const int sw = QAA_LoadS32( c + 0x34 ), sh = QAA_LoadS32( c + 0x38 );
		cx0 = std::max( cx0, static_cast<int>( sl * sx ) );
		cy0 = std::max( cy0, static_cast<int>( st * sy ) );
		cx1 = std::min( cx1, static_cast<int>( std::ceil( ( sl + sw ) * sx ) ) );
		cy1 = std::min( cy1, static_cast<int>( std::ceil( ( st + sh ) * sy ) ) );
	}
	canvas.SetView( v, cx0, cy0, cx1, cy1 );
}

void LoadVertices( ReplayState &s, uint32_t data, int count, int format ) {
	s.vertices.resize( static_cast<size_t>( std::max( count, 0 ) ) );
	for ( int i = 0; i < count; i++ ) {
		qaa_ui::Vertex &v = s.vertices[i];
		v.color[0] = v.color[1] = v.color[2] = v.color[3] = 1.0f;
		v.factor[0] = v.factor[1] = v.factor[2] = v.factor[3] = 1.0f;
		switch ( format ) {
		case VF_XY32F:
			v.x = QAA_LoadF( data + 8 * i );
			v.y = QAA_LoadF( data + 8 * i + 4 );
			break;
		case VF_XY16I_C32:
		case VF_XY16I_CF32: {
			const uint32_t size = format == VF_XY16I_C32 ? 8 : 12;
			const uint32_t a = data + size * i;
			v.x = static_cast<int16_t>( QAA_LoadU16( a ) );
			v.y = static_cast<int16_t>( QAA_LoadU16( a + 2 ) );
			// Vertex colours come from GFx's tessellator in the order the Xbox
			// back end's vertex fetch wants: 0xAABBGGRR (its bitmap shader,
			// fed GColors in 0xAARRGGBB, swizzles .bgra to match).
			UnpackColor( QAA_LoadU32( a + 4 ), v.color );
			std::swap( v.color[0], v.color[2] );
			if ( format == VF_XY16I_CF32 ) {
				UnpackColor( QAA_LoadU32( a + 8 ), v.factor );
				std::swap( v.factor[0], v.factor[2] );
			}
			break;
		}
		default:
			v.x = static_cast<int16_t>( QAA_LoadU16( data + 4 * i ) );
			v.y = static_cast<int16_t>( QAA_LoadU16( data + 4 * i + 2 ) );
			break;
		}
	}
}

int commandCounts[32];
uint32_t lastGouraudPresent;

void ExecuteList( uint32_t at, uint32_t end ) {
	ReplayState s;
	int guard = 0;
	std::memset( commandCounts, 0, sizeof( commandCounts ) );
	while ( at + 4 <= end && guard++ < 1000000 ) {
		const uint32_t cmd = QAA_LoadU32( at );
		commandCounts[cmd & 31]++;
		switch ( cmd ) {
		case CMD_END:
		case CMD_FRAME_END:
			return;
		case CMD_BEGIN_DISPLAY:
			BeginDisplay( at );
			at += 0x4C;
			break;
		case CMD_END_DISPLAY:
			at += 4;
			break;
		case CMD_DRAW_BITMAPS: {
			const int count = QAA_LoadS32( at + 4 );
			const uint32_t dev = QAA_LoadU32( at + 8 );
			const qaa_ui::Matrix m = LoadMatrix( at + 12 );
			const uint32_t list = at + 0x24;
			std::vector<qaa_ui::BitmapQuad> quads( static_cast<size_t>( std::max( count, 0 ) ) );
			for ( int i = 0; i < count; i++ ) {
				const uint32_t b = list + 36 * i;
				qaa_ui::BitmapQuad &q = quads[i];
				q.x0 = QAA_LoadF( b ); q.y0 = QAA_LoadF( b + 4 ); q.x1 = QAA_LoadF( b + 8 ); q.y1 = QAA_LoadF( b + 12 );
				q.u0 = QAA_LoadF( b + 16 ); q.v0 = QAA_LoadF( b + 20 ); q.u1 = QAA_LoadF( b + 24 ); q.v1 = QAA_LoadF( b + 28 );
				UnpackColor( QAA_LoadU32( b + 32 ), q.color );
			}
			if ( qaa_uiTrace->integer > 2 ) {
				const qaa_ui::Texture *t = TextureOfDevtex( dev );
				Com_Printf( "QAA ui: bitmaps %d dev %08X tex %u (%dx%d%s)\n", count, dev,
					dev ? QAA_LoadU32( dev + devtex::TEX ) : 0, t ? t->width : 0, t ? t->height : 0,
					t && t->alphaOnly ? " A8" : "" );
			}
			canvas.DrawBitmaps( quads.data(), count, m, TextureOfDevtex( dev ) );
			at += 0x24 + 36 * static_cast<uint32_t>( std::max( count, 0 ) );
			break;
		}
		case CMD_VERTEX_DATA: {
			const int count = QAA_LoadS32( at + 4 );
			const int format = QAA_LoadS32( at + 8 );
			static const int sizes[5] = { 0, 4, 8, 8, 12 };
			const int size = format >= 0 && format < 5 ? sizes[format] : 0;
			LoadVertices( s, at + 12, count, format );
			at += 12 + static_cast<uint32_t>( size * std::max( count, 0 ) );
			break;
		}
		case CMD_INDEX_DATA: {
			const int count = QAA_LoadS32( at + 4 );
			const uint32_t bytes = QAA_LoadU32( at + 8 );
			const int format = QAA_LoadS32( at + 12 );
			s.indices.resize( static_cast<size_t>( std::max( count, 0 ) ) );
			for ( int i = 0; i < count; i++ ) {
				s.indices[i] = format == 2 ? QAA_LoadU32( at + 16 + 4 * i ) : QAA_LoadU16( at + 16 + 2 * i );
			}
			at += 16 + bytes;
			break;
		}
		case CMD_DRAW_TRIANGLES: {
			const int base = QAA_LoadS32( at + 4 );
			const int start = QAA_LoadS32( at + 16 );
			const int tris = QAA_LoadS32( at + 20 );
			std::vector<uint32_t> idx;
			idx.reserve( static_cast<size_t>( std::max( tris, 0 ) ) * 3 );
			bool ok = true;
			for ( int i = 0; i < tris * 3 && ok; i++ ) {
				const size_t k = static_cast<size_t>( start + i );
				if ( k >= s.indices.size() ) {
					ok = false;
					break;
				}
				const uint32_t vi = s.indices[k] + static_cast<uint32_t>( base );
				if ( vi >= s.vertices.size() ) {
					ok = false;
					break;
				}
				idx.push_back( vi );
			}
			if ( ok && qaa_uiTrace->integer > 4 ) {
				float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
				const qaa_ui::Matrix fm = canvas.FullMatrix();
				for ( uint32_t vi : idx ) {
					float px, py;
					fm.Apply( s.vertices[vi].x, s.vertices[vi].y, &px, &py );
					x0 = std::min( x0, px ); y0 = std::min( y0, py ); x1 = std::max( x1, px ); y1 = std::max( y1, py );
				}
				const qaa_ui::Texture *t = s.fill.tex[0].texture;
				const qaa_ui::Vertex &v0 = s.vertices[idx.empty() ? 0 : idx[0]];
				if ( s.fill.kind == qaa_ui::FILL_GOURAUD ) {
					const qaa_ui::Texture *t1 = s.fill.tex[1].texture;
					const qaa_ui::Cxform &cx = canvas.CurrentCxform();
					Com_Printf( "QAA ui:   texCount %d tex1 %dx%d present %X cx mul %g %g %g %g add %g %g %g %g\n",
						s.fill.texCount, t1 ? t1->width : 0, t1 ? t1->height : 0, lastGouraudPresent,
						cx.mul[0], cx.mul[1], cx.mul[2], cx.mul[3], cx.add[0], cx.add[1], cx.add[2], cx.add[3] );
					for ( int k = 0; k < s.fill.texCount; k++ ) {
						const qaa_ui::Texture *tk = s.fill.tex[k].texture;
						if ( tk && !tk->rgba.empty() ) {
							std::string px;
							for ( int i = 0; i < tk->width; i += std::max( 1, tk->width / 6 ) ) {
								const uint8_t *p = &tk->rgba[static_cast<size_t>( i ) * 4];
								px += va( " %02X%02X%02X%02X", p[0], p[1], p[2], p[3] );
							}
							const float *m = s.fill.tex[k].uv.m;
							Com_Printf( "QAA ui:   tex%d rgba%s uv %g %g %g / %g %g %g\n", k, px.c_str(), m[0], m[1], m[2], m[3], m[4], m[5] );
							if ( k == 0 && qaa_uiTrace->integer > 5 ) {
								for ( size_t vi = 0; vi < std::min<size_t>( idx.size(), 12 ); vi++ ) {
									const qaa_ui::Vertex &v = s.vertices[idx[vi]];
									float u, w;
									s.fill.tex[0].uv.Apply( v.x, v.y, &u, &w );
									Com_Printf( "QAA ui:     v %g %g col %.2f %.2f %.2f %.2f fac %.2f %.2f %.2f %.2f uv %.3f %.3f\n", v.x, v.y,
										v.color[0], v.color[1], v.color[2], v.color[3], v.factor[0], v.factor[1], v.factor[2], v.factor[3], u, w );
								}
							}
						}
					}
				}
				Com_Printf( "QAA ui: tris %d fill %d/%d color %.2f %.2f %.2f %.2f v0 %.2f %.2f %.2f %.2f f %.2f %.2f tex %dx%d blend %d mask %X box %.0f,%.0f-%.0f,%.0f\n",
					tris, s.fill.kind, s.fill.gouraud, s.fill.color[0], s.fill.color[1], s.fill.color[2], s.fill.color[3],
					v0.color[0], v0.color[1], v0.color[2], v0.color[3], v0.factor[2], v0.factor[3],
					t ? t->width : 0, t ? t->height : 0, canvas.BlendMode(), canvas.MaskState(), x0, y0, x1, y1 );
			}
			if ( ok ) {
				canvas.DrawTriangles( s.vertices.data(), idx.data(), tris, s.fill );
			}
			at += 24;
			break;
		}
		case CMD_DRAW_LINES: {
			const int base = QAA_LoadS32( at + 4 );
			const int lines = QAA_LoadS32( at + 8 );
			if ( s.lineOn && base >= 0 && static_cast<size_t>( base + lines + 1 ) <= s.vertices.size() ) {
				canvas.DrawLineStrip( s.vertices.data() + base, lines + 1, s.lineColor );
			}
			at += 12;
			break;
		}
		case CMD_FILL_DISABLE:
			s.fill.kind = qaa_ui::FILL_NONE;
			at += 4;
			break;
		case CMD_LINE_DISABLE:
			s.lineOn = false;
			at += 4;
			break;
		case CMD_FILL_COLOR:
			s.fill.kind = qaa_ui::FILL_COLOR;
			UnpackColor( QAA_LoadU32( at + 4 ), s.fill.color );
			at += 8;
			break;
		case CMD_LINE_COLOR:
			s.lineOn = true;
			UnpackColor( QAA_LoadU32( at + 4 ), s.lineColor );
			at += 8;
			break;
		case CMD_FILL_BITMAP:
			s.fill.kind = qaa_ui::FILL_TEXTURE;
			s.fill.texCount = 1;
			LoadFillTexture( at + 4, s.fill.tex[0] );
			at += 0x28;
			break;
		case CMD_FILL_GOURAUD: {
			const uint32_t present = QAA_LoadU32( at + 0x74 );
			lastGouraudPresent = present;
			s.fill.kind = qaa_ui::FILL_GOURAUD;
			s.fill.gouraud = QAA_LoadS32( at + 4 );
			s.fill.texCount = 0;
			for ( int k = 0; k < 3; k++ ) {
				if ( present & ( 1u << k ) ) {
					LoadFillTexture( at + 8 + 36 * k, s.fill.tex[k] );
					s.fill.texCount = k + 1;
					if ( qaa_uiTrace->integer > 3 ) {
						const uint32_t gtex = QAA_LoadU32( at + 8 + 36 * k );
						const uint32_t dev = gtex ? QAA_LoadU32( gtex + 0x2C ) : 0;
						const qaa_ui::Texture *t = s.fill.tex[k].texture;
						Com_Printf( "QAA ui: gouraud %d tex%d gtex %08X dev %08X id %u %dx%d%s\n", s.fill.gouraud, k, gtex,
							dev, dev ? QAA_LoadU32( dev + devtex::TEX ) : 0, t ? t->width : 0, t ? t->height : 0,
							t && t->alphaOnly ? " A8" : "" );
					}
				} else {
					s.fill.tex[k] = qaa_ui::TextureFill();
				}
			}
			at += 0x78;
			break;
		}
		case CMD_PUSH_BLEND:
			canvas.PushBlend( QAA_LoadS32( at + 4 ) );
			at += 8;
			break;
		case CMD_POP_BLEND:
			canvas.PopBlend();
			at += 4;
			break;
		case CMD_MATRIX:
			canvas.SetMatrix( LoadMatrix( at + 4 ) );
			at += 0x1C;
			break;
		case CMD_USER_MATRIX:
			canvas.SetUserMatrix( LoadMatrix( at + 4 ) );
			at += 0x1C;
			break;
		case CMD_CXFORM: {
			// GRenderer::Cxform M_[4][2]: (multiply, add) for R, G, B, A; add in 0..255
			qaa_ui::Cxform cx;
			for ( int k = 0; k < 4; k++ ) {
				cx.mul[k] = QAA_LoadF( at + 4 + 8 * k );
				cx.add[k] = QAA_LoadF( at + 8 + 8 * k ) * ( 1.0f / 255.0f );
			}
			canvas.SetCxform( cx );
			at += 0x24;
			break;
		}
		case CMD_BEGIN_MASK:
			canvas.BeginSubmitMask( QAA_LoadS32( at + 4 ) );
			at += 8;
			break;
		case CMD_END_MASK:
			canvas.EndSubmitMask();
			at += 4;
			break;
		case CMD_DISABLE_MASK:
			canvas.DisableMask();
			at += 4;
			break;
		default:
			Com_DPrintf( "QAA ui: unknown render command %u\n", cmd );
			return;
		}
	}
}

/* The frame SFUI's render just recorded: GetCommandList (renderer vtable
 * +0x88) gives its first command, ReleaseCommandList (+0x8C) hands it back. */
void ExecuteCommands() {
	if ( !sfuiRenderer ) {
		return;
	}
	const uint32_t vt = QAA_LoadU32( sfuiRenderer );
	uint64_t list = 0;
	if ( !UiCall( QAA_LoadU32( vt + 0x88 ), { sfuiRenderer }, &list ) || !list ) {
		return;
	}
	ExecuteList( static_cast<uint32_t>( list ), static_cast<uint32_t>( list ) + 0x400000 );
	UiCall( QAA_LoadU32( vt + 0x8C ), { sfuiRenderer } );
	canvasDrawn = true;
}

/* ==== the ui syscalls of QAA's menu data layer ========================================= */

/*
 * QAA's engine keeps Quake III's ui code (ui_main/ui_shared, 0x8219A000..)
 * for the menus' data: maps, game types, bots, models, server lists. It
 * calls trap_* through 0x821A2810, which packs the arguments into an array
 * for CL_UISystemCalls (0x8210F988). The numbering is Quake III's up to
 * 0x1D; from 0x1F on QAA adds one (S_RegisterSound has a compressed
 * variant at 0x20).
 */
enum {
	UIT_ERROR = 0, UIT_PRINT, UIT_MILLISECONDS, UIT_CVAR_SET, UIT_CVAR_VARIABLEVALUE, UIT_CVAR_VARIABLESTRINGBUFFER,
	UIT_CVAR_SETVALUE, UIT_CVAR_RESET, UIT_CVAR_CREATE, UIT_CVAR_INFOSTRINGBUFFER, UIT_ARGC, UIT_ARGV,
	UIT_CMD_EXECUTETEXT, UIT_FS_FOPENFILE, UIT_FS_READ, UIT_FS_WRITE, UIT_FS_FCLOSEFILE, UIT_FS_GETFILELIST,
	UIT_R_REGISTERMODEL, UIT_R_REGISTERSKIN, UIT_R_REGISTERSHADERNOMIP, UIT_R_CLEARSCENE, UIT_R_ADDREFENTITYTOSCENE,
	UIT_R_ADDPOLYTOSCENE, UIT_R_ADDLIGHTTOSCENE, UIT_R_RENDERSCENE, UIT_R_SETCOLOR, UIT_R_DRAWSTRETCHPIC,
	UIT_UPDATESCREEN, UIT_CM_LERPTAG,
	UIT_S_REGISTERSOUND = 0x1F, UIT_S_REGISTERSOUND_COMPRESSED, UIT_S_STARTLOCALSOUND, UIT_KEY_KEYNUMTOSTRINGBUF,
	UIT_KEY_GETBINDINGBUF, UIT_KEY_SETBINDING, UIT_KEY_ISDOWN, UIT_KEY_GETOVERSTRIKEMODE, UIT_KEY_SETOVERSTRIKEMODE,
	UIT_KEY_CLEARSTATES, UIT_KEY_GETCATCHER, UIT_KEY_SETCATCHER, UIT_GETCLIPBOARDDATA, UIT_GETGLCONFIG,
	UIT_GETCLIENTSTATE, UIT_GETCONFIGSTRING,
	UIT_CVAR_REGISTER = 0x33, UIT_CVAR_UPDATE,
};

std::set<int> warnedTraps;
int uiOverstrike;

void H_UiSyscall( void *, qaa_hook_call *c ) {
	const uint32_t a = R( c, 0 );
	auto arg = [&]( int k ) { return QAA_LoadU32( a + 4 * k ); };
	auto argf = [&]( int k ) { return QAA_LoadF( a + 4 * k ); };
	auto str = [&]( int k ) { return GStr( arg( k ) ); };
	const int trap = QAA_LoadS32( a );
	c->r[0] = 0;
	switch ( trap ) {
	case UIT_ERROR:
		Com_Printf( S_COLOR_RED "QAA ui error: %s\n", str( 1 ).c_str() );
		return;
	case UIT_PRINT:
		Com_Printf( "%s", str( 1 ).c_str() );
		return;
	case UIT_MILLISECONDS:
		c->r[0] = static_cast<uint32_t>( Sys_Milliseconds() );
		return;
	case UIT_CVAR_SET:
		Cvar_Set2( str( 1 ).c_str(), str( 2 ).c_str(), qtrue );
		return;
	case UIT_CVAR_VARIABLEVALUE: {
		const float v = Cvar_VariableValue( str( 1 ).c_str() );
		uint32_t bits;
		std::memcpy( &bits, &v, 4 );
		c->r[0] = bits;
		return;
	}
	case UIT_CVAR_VARIABLESTRINGBUFFER:
		GStrcpy( arg( 2 ), Cvar_VariableString( str( 1 ).c_str() ), static_cast<int>( arg( 3 ) ) );
		return;
	case UIT_CVAR_SETVALUE:
		Cvar_SetValue( str( 1 ).c_str(), argf( 2 ) );
		return;
	case UIT_CVAR_RESET:
		Cvar_Set2( str( 1 ).c_str(), nullptr, qfalse );
		return;
	case UIT_CVAR_CREATE:
		MirrorCvar( Cvar_Get( str( 1 ).c_str(), str( 2 ).c_str(), static_cast<int>( arg( 3 ) ) & 0x7FF ) );
		return;
	case UIT_CVAR_REGISTER: {
		// QAA: retail 0x82120A40: Cvar_Get, then the vmCvar_t (Quake III's
		// layout) from it. ui_main registers its cvar table this way
		// (UI_RegisterCvars 0x821A2680: ui_mapIndex, ui_currentMap,
		// ui_modelIndex...) and reads g_arenasFile and g_botsFile.
		QAAVmCvar vc( arg( 1 ) );
		Cvar_RegisterLegacy( vc.Host(), str( 2 ).c_str(), str( 3 ).c_str(), static_cast<int>( arg( 4 ) ) & 0x7FF, 0 );
		vc.WriteBack();
		MirrorCvar( Cvar_Get( str( 2 ).c_str(), str( 3 ).c_str(), 0 ) );
		return;
	}
	case UIT_CVAR_UPDATE: {
		// QAA: retail 0x82120AA8
		QAAVmCvar vc( arg( 1 ) );
		if ( vc.Host() ) {
			Cvar_UpdateLegacy( vc.Host(), 0 );
			vc.WriteBack();
		}
		return;
	}
	case UIT_CVAR_INFOSTRINGBUFFER:
		GStrcpy( arg( 2 ), Cvar_InfoString( static_cast<int>( arg( 1 ) ), nullptr ), static_cast<int>( arg( 3 ) ) );
		return;
	case UIT_ARGC:
		c->r[0] = static_cast<uint32_t>( Cmd_Argc() );
		return;
	case UIT_ARGV:
		GStrcpy( arg( 2 ), Cmd_Argv( static_cast<int>( arg( 1 ) ) ), static_cast<int>( arg( 3 ) ) );
		return;
	case UIT_CMD_EXECUTETEXT:
		Cbuf_ExecuteText( static_cast<cbufExec_t>( arg( 1 ) ), str( 2 ).c_str() );
		return;
	case UIT_FS_FOPENFILE: {
		fileHandle_t f = 0;
		std::string p = str( 1 );
		for ( char &ch : p ) {
			if ( ch == '\\' ) {
				ch = '/';
			}
		}
		const int len = FS_FOpenFileByMode( p.c_str(), &f, static_cast<fsMode_t>( arg( 3 ) ) );
		if ( arg( 2 ) ) {
			QAA_StoreS32( arg( 2 ), f );
		}
		c->r[0] = static_cast<uint32_t>( len );
		return;
	}
	case UIT_FS_READ:
		if ( arg( 3 ) ) {
			FS_Read( QAA_Ptr( arg( 1 ) ), static_cast<int>( arg( 2 ) ), static_cast<fileHandle_t>( arg( 3 ) ) );
		}
		return;
	case UIT_FS_WRITE:
		if ( arg( 3 ) ) {
			FS_Write( QAA_Ptr( arg( 1 ) ), static_cast<int>( arg( 2 ) ), static_cast<fileHandle_t>( arg( 3 ) ) );
		}
		return;
	case UIT_FS_FCLOSEFILE:
		if ( arg( 1 ) ) {
			FS_FCloseFile( static_cast<fileHandle_t>( arg( 1 ) ) );
		}
		return;
	case UIT_FS_GETFILELIST:
		c->r[0] = static_cast<uint32_t>( FS_GetFileList( str( 1 ).c_str(), str( 2 ).c_str(),
			reinterpret_cast<char *>( QAA_Ptr( arg( 3 ) ) ), static_cast<int>( arg( 4 ) ) ) );
		return;
	case UIT_R_REGISTERMODEL:
		c->r[0] = static_cast<uint32_t>( re.RegisterModel( str( 1 ).c_str() ) );
		return;
	case UIT_R_REGISTERSKIN:
		c->r[0] = static_cast<uint32_t>( re.RegisterSkin( str( 1 ).c_str() ) );
		return;
	case UIT_R_REGISTERSHADERNOMIP:
		c->r[0] = static_cast<uint32_t>( re.RegisterShaderNoMip( str( 1 ).c_str() ) );
		return;
	case UIT_R_CLEARSCENE:
	case UIT_R_ADDREFENTITYTOSCENE:
	case UIT_R_ADDPOLYTOSCENE:
	case UIT_R_ADDLIGHTTOSCENE:
	case UIT_R_RENDERSCENE:
	case UIT_R_SETCOLOR:
	case UIT_R_DRAWSTRETCHPIC:
	case UIT_UPDATESCREEN:
		// the menus draw through Scaleform; QAA's legacy ui draw calls are not used
		return;
	case UIT_S_REGISTERSOUND:
	case UIT_S_REGISTERSOUND_COMPRESSED:
		c->r[0] = static_cast<uint32_t>( S_RegisterSound( str( 1 ).c_str(), trap == UIT_S_REGISTERSOUND_COMPRESSED ? qtrue : qfalse ) );
		return;
	case UIT_S_STARTLOCALSOUND:
		if ( arg( 1 ) ) {
			S_StartLocalSound( static_cast<sfxHandle_t>( arg( 1 ) ), static_cast<int>( arg( 2 ) ) );
		}
		return;
	case UIT_KEY_KEYNUMTOSTRINGBUF:
		GStrcpy( arg( 2 ), Key_KeynumToString( static_cast<int>( arg( 1 ) ) ), static_cast<int>( arg( 3 ) ) );
		return;
	case UIT_KEY_GETBINDINGBUF: {
		const char *b = static_cast<int>( arg( 1 ) ) >= 0 ? Key_GetBinding( static_cast<int>( arg( 1 ) ) ) : nullptr;
		GStrcpy( arg( 2 ), b ? b : "", static_cast<int>( arg( 3 ) ) );
		return;
	}
	case UIT_KEY_SETBINDING:
		Key_SetBinding( static_cast<int>( arg( 1 ) ), str( 2 ).c_str() );
		return;
	case UIT_KEY_ISDOWN:
		c->r[0] = static_cast<uint32_t>( Key_IsDown( static_cast<int>( arg( 1 ) ) ) );
		return;
	case UIT_KEY_GETOVERSTRIKEMODE:
		c->r[0] = static_cast<uint32_t>( uiOverstrike );
		return;
	case UIT_KEY_SETOVERSTRIKEMODE:
		uiOverstrike = static_cast<int>( arg( 1 ) );
		return;
	case UIT_KEY_CLEARSTATES:
		Key_ClearStates();
		return;
	case UIT_KEY_GETCATCHER:
		c->r[0] = static_cast<uint32_t>( Key_GetCatcher() );
		return;
	case UIT_KEY_SETCATCHER:
		Key_SetCatcher( static_cast<int>( arg( 1 ) ) );
		return;
	case UIT_GETCLIPBOARDDATA:
		GStrcpy( arg( 1 ), "", static_cast<int>( arg( 2 ) ) );
		return;
	case UIT_GETCONFIGSTRING: {
		const int index = static_cast<int>( arg( 1 ) );
		const char *s = index >= 0 && index < MAX_CONFIGSTRINGS && cl.gameState.stringOffsets[index]
			? cl.gameState.stringData + cl.gameState.stringOffsets[index] : "";
		GStrcpy( arg( 2 ), s, static_cast<int>( arg( 3 ) ) );
		c->r[0] = 1;
		return;
	}
	default:
		if ( warnedTraps.insert( trap ).second ) {
			Com_DPrintf( "QAA ui: unhandled ui syscall %d (0x%X) from %08X\n", trap, trap, c->return_address );
		}
		return;
	}
}

/* ==== hook table ======================================================================= */

struct HookEntry {
	uint32_t address;
	qaa_hook_fn fn;
};

const HookEntry hookTable[] = {
	{ hook::UI_SYSCALL, H_UiSyscall },
	{ hook::CBUF_ADDTEXT, H_Cbuf_AddText },
	{ hook::CMD_ADDCOMMAND, H_Cmd_AddCommand },
	{ hook::Z_FREE, H_Z_Free },
	{ hook::Z_TAGMALLOC, H_Z_TagMalloc },
	{ hook::Z_MALLOC, H_Z_TagMalloc },
	{ hook::HUNK_ALLOC_TEMP, H_Z_TagMalloc },
	{ hook::HUNK_FREE_TEMP, H_Z_Free },
	{ hook::CVAR_FINDVAR, H_Cvar_FindVar },
	{ hook::CVAR_GET, H_Cvar_Get },
	{ hook::CVAR_SET2, H_Cvar_Set2 },
	{ hook::CVAR_SETVALUE, H_Cvar_SetValue },
	{ hook::FS_FCLOSEFILE, H_FS_FCloseFile },
	{ hook::FS_FOPENFILEREAD, H_FS_FOpenFileRead },
	{ hook::FS_READ, H_FS_Read },
	{ hook::FS_READFILE, H_FS_ReadFile },
	{ hook::FS_FREEFILE, H_FS_FreeFile },
	{ hook::FS_GETFILELIST, H_FS_GetFileList },
	{ hook::R_GETCOMMANDBUFFER, H_R_GetCommandBuffer },
	{ hook::SFUI_BACKEND_CTOR, H_BackendCtor },
	{ hook::S_STARTLOCALSOUND, H_S_StartLocalSound },
	{ hook::S_STARTBACKGROUNDTRACK, H_S_StartBackgroundTrack },
	{ hook::S_REGISTERSOUND, H_S_RegisterSound },
	{ hook::QNET_JOIN_WAIT, H_QnetJoinWait },
	{ hook::QNET_LEAVE_GAME, H_QnetLeaveGame },
	{ hook::LANGUAGE_CODE, H_LanguageCode },
	{ hook::COUNTRY_CODE, H_CountryCode },
	{ hook::SYS_MILLISECONDS, H_Sys_Milliseconds },
	{ hook::IS_FULL_VERSION, H_IsFullVersion },
	{ hook::CAN_PLAY, H_CanPlay },
	{ hook::USER_SIGNED_IN, H_UserSignedIn },
	{ hook::SHOW_DEVICE_SELECTOR, H_ShowDeviceSelector },
	{ hook::START_LOAD, H_StartLoad },
	{ hook::START_SAVE, H_StartSave },
	{ hook::D3D_RELEASE, H_D3D_Release },
	{ hook::D3D_UNLOCK, H_D3D_Unlock },
	{ hook::D3D_LOCKRECT, H_D3D_LockRect },
	{ hook::D3D_LEVELDESC, H_D3D_LevelDesc },
	{ hook::D3D_CREATETEXTURE, H_D3D_CreateTexture },
	{ hook::D3DX_TEXTURE_FROM_MEMORY, H_D3DX_TextureFromMemory },
};

void InstallHooks() {
	qaa_guest *g = Guest();
	Api().qaa_guest_set_default_hook( g, DefaultHook, nullptr );
	Api().qaa_guest_set_default_import_hook( g, DefaultImportHook, nullptr );
	Api().qaa_guest_set_import_hook( g, "xam.xex", 614, H_XamContentGetLicenseMask, nullptr );
	for ( const HookEntry &h : hookTable ) {
		Api().qaa_guest_set_hook( g, h.address, h.fn, nullptr );
	}
}

/* ==== tracing ActionScript's calls into the engine ====================================== */

/*
 * The movies reach the engine two ways: ExternalInterface.call (SFUI's
 * handler 0x82198DA0 looks the name up in its "Quake function" table at
 * 0x827EB9F0) and fscommand (0x82198D90, which calls 0x821915E8). Retail
 * stripped GFx's trace() log, so for development FnQAA puts host
 * wrappers in those two handlers' vtable slots (qaa_uiTrace 1): they print
 * the call and hand it on to the retail handler unchanged.
 */
namespace eiaddr {
constexpr uint32_t EXTERNAL_INTERFACE_SLOT = 0x820D26C0;	// vtable 0x820D26BC, method 1
constexpr uint32_t FSCOMMAND_SLOT = 0x820D26C8;				// vtable 0x820D26C4, method 1
constexpr uint32_t EXTERNAL_INTERFACE = 0x82198DA0;
constexpr uint32_t FSCOMMAND = 0x82198D90;
}

/* GFxValue (GFx 2.x): the type, then the value 8-aligned. */
std::string DescribeValue( uint32_t v ) {
	const uint32_t type = QAA_LoadU32( v );
	switch ( type ) {
	case 0: return "undefined";
	case 1: return "null";
	case 2: return QAA_LoadU32( v + 8 ) ? "true" : "false";
	case 3: {
		const uint64_t bits = ( static_cast<uint64_t>( QAA_LoadU32( v + 8 ) ) << 32 ) | QAA_LoadU32( v + 12 );
		double d;
		std::memcpy( &d, &bits, 8 );
		return va( "%g", d );
	}
	case 4: return "\"" + GStr( QAA_LoadU32( v + 8 ), 120 ) + "\"";
	default: return va( "<type %u>", type );
	}
}

int64_t TraceExternalInterface( void *, qaa_module *, const qaa_syscall *c ) {
	// (this, movie, method name, GFxValue args[], count)
	if ( qaa_uiTrace && qaa_uiTrace->integer > 0 ) {
		std::string args;
		const uint32_t argv = static_cast<uint32_t>( c->args[2] );
		for ( int i = 0; i < c->args[3] && i < 12; i++ ) {
			args += ( i ? ", " : "" ) + DescribeValue( argv + 16 * i );
		}
		Com_Printf( "ui call: %s(%s)\n", GStr( static_cast<uint32_t>( c->args[1] ), 80 ).c_str(), args.c_str() );
	}
	uint64_t r = 0;
	UiCall( eiaddr::EXTERNAL_INTERFACE, { static_cast<uint32_t>( c->trap ), static_cast<uint32_t>( c->args[0] ),
		static_cast<uint32_t>( c->args[1] ), static_cast<uint32_t>( c->args[2] ), static_cast<uint32_t>( c->args[3] ) }, &r );
	return static_cast<int64_t>( r );
}

int64_t TraceFSCommand( void *, qaa_module *, const qaa_syscall *c ) {
	// (this, movie, command, arguments)
	if ( qaa_uiTrace && qaa_uiTrace->integer > 0 ) {
		Com_Printf( "ui fscommand: %s %s\n", GStr( static_cast<uint32_t>( c->args[1] ), 80 ).c_str(),
			GStr( static_cast<uint32_t>( c->args[2] ), 200 ).c_str() );
	}
	uint64_t r = 0;
	UiCall( eiaddr::FSCOMMAND, { static_cast<uint32_t>( c->trap ), static_cast<uint32_t>( c->args[0] ),
		static_cast<uint32_t>( c->args[1] ), static_cast<uint32_t>( c->args[2] ) }, &r );
	return static_cast<int64_t>( r );
}

void InstallCallTracing() {
	QAA_StoreU32( eiaddr::EXTERNAL_INTERFACE_SLOT, Api().qaa_module_host_function( uiModule, TraceExternalInterface, nullptr ) );
	QAA_StoreU32( eiaddr::FSCOMMAND_SLOT, Api().qaa_module_host_function( uiModule, TraceFSCommand, nullptr ) );
}

/* ==== engine state the front end reads ================================================== */

/*
 * The video mode in QAA's glconfig layout, in both copies SFUI reads:
 * cls.glconfig and the renderer's (0x82DC0480). Movie loading (0x82197FF8)
 * sizes each movie's viewport from vidWidth, vidHeight and the widescreen
 * and interlaced flags: a 16:9 stage the height of the screen, centred
 * horizontally unless the display is widescreen, as on the Xbox.
 */
void PublishGlconfig() {
	const uint32_t copies[2] = { addr::CLS_GLCONFIG, addr::RENDERER_GLCONFIG };
	const int w = cls.glconfig.vidWidth, h = cls.glconfig.vidHeight;
	const bool widescreen = QAA_UI_IsWidescreen( w, h );
	for ( uint32_t g : copies ) {
		// GFx sizes its glyph cache from the renderer caps' maxTextureSize (0x82166E88)
		QAA_StoreS32( g + 0x2C00, 4096 );
		QAA_StoreS32( g + 0x2C28, w );
		QAA_StoreS32( g + 0x2C2C, h );
		QAA_StoreS32( g + 0x2C3C, widescreen ? 1 : 0 );
		QAA_StoreS32( g + 0x2C4C, 0 );
	}
}

/*
 * The signed-in profile. The engine keeps one record per controller
 * (0x498 bytes from 0x82A75150): XUID at +0x58, flags at +0x60, "signed in"
 * at +0x64 and the gamertag at +0x70. showSigninUI (0x82191588) and the
 * menus' getCurrentUser read them. PC deviation: controller 0 is always
 * signed in to a local (offline) profile named after the player's "name"
 * cvar, as an Xbox with a local profile and no Live connection.
 */
void PublishProfile() {
	char name[MAX_NAME_LENGTH];
	Q_strncpyz( name, Cvar_VariableString( "name" ), sizeof( name ) );
	Q_CleanStr( name );
	if ( !name[0] ) {
		Q_strncpyz( name, "Player", sizeof( name ) );
	}
	QAA_StoreU32( profile::XUID, 0xE0000000u );		// an offline XUID's top bits
	QAA_StoreU32( profile::XUID + 4, 1 );
	QAA_StoreU32( profile::FLAGS, 0 );
	QAA_StoreU32( profile::SIGNED_IN, 1 );
	GStrcpy( profile::GAMERTAG, name, profile::GAMERTAG_SIZE );
}

/* Engine objects SFUI polls that have no meaning without Xbox Live: zeroed
 * blocks, so the polls find "nothing pending". */
void PublishEngineObjects() {
	const uint32_t blocks[] = { addr::LIVE_STATE_PTR, addr::PROFILE_PTR, addr::UI_DC_PTR };
	for ( uint32_t p : blocks ) {
		if ( !QAA_LoadU32( p ) ) {
			QAA_StoreU32( p, GCalloc( 0x10000 ) );
		}
	}
}

/* cls.state and cls.keyCatchers, which SFUI reads and writes in place
 * (KEYCATCH_* are Quake III's in both engines). */
void PublishClientState() {
	// The cgame pauses into the in-game menu while sys_titleFocused < 1
	// (retail sets it from the Xbox Guide's focus notifications). PC
	// deviation: it follows the game window's focus; a windowless client is
	// always focused.
	const char *focused = ( gw_active || Com_IsWindowless() ) ? "1" : "0";
	if ( strcmp( Cvar_VariableString( "sys_titleFocused" ), focused ) ) {
		Cvar_Set( "sys_titleFocused", focused );
	}
	QAA_StoreU32( addr::CLS_STATE, static_cast<uint32_t>( cls.state ) );
	QAA_StoreS32( addr::CLS_REALTIME, cls.realtime );
	SyncKeyCatchers();
}

/* Retail has one cls.keyCatchers, which SFUI's disable/restorePlayerInput
 * change in place; here the engine's and the guest's copies are merged.
 * Whichever side changed since the last merge wins, the guest's first: the
 * update thread may change it between the host's merges (it runs until it
 * waits for its next frame), and copying the host's value over the guest's
 * would undo a restorePlayerInput.
 * Key_SetCatcher releases the keys held at the change through CL_KeyEvent
 * before it stores the new catchers, and the front end takes those releases
 * (QAA_UI_KeyEvent), which merges again on its way out. That merge is
 * skipped: the change is already being applied. (Run, it saw the guest's
 * change still pending and applied it again, without end: Escape held down
 * while the in-game menu opened overflowed the stack.) */
void SyncKeyCatchers() {
	static int synced = -1;
	static bool applying;
	if ( applying ) {
		return;
	}
	const int guest = static_cast<int>( QAA_LoadU32( addr::CLS_KEYCATCHERS ) ) & 0xF;
	const int host = Key_GetCatcher();
	if ( synced >= 0 && guest != synced ) {
		synced = guest;
		if ( ( host & 0xF ) != guest ) {
			if ( qaa_uiTrace && qaa_uiTrace->integer ) {
				Com_Printf( "QAA ui: key catchers %X -> %X\n", host, ( host & ~0xF ) | guest );
			}
			applying = true;
			Key_SetCatcher( ( host & ~0xF ) | guest );
			applying = false;
		}
		return;
	}
	if ( ( host & 0xF ) != guest ) {
		QAA_StoreU32( addr::CLS_KEYCATCHERS, static_cast<uint32_t>( host ) & 0xF );
	}
	synced = host & 0xF;
}

void CollectKeyCatchers() {
	SyncKeyCatchers();
}

/* ==== start-up ================================================================================ */

bool RunCrtStartup() {
	// as mainCRTStartup: heap, per-thread data, pre-C and C/C++ initializers
	return UiCall( addr::CRT_HEAP_INIT, {} ) && UiCall( addr::CRT_MTINIT, {} )
		&& UiCall( addr::CRT_PRE_C_INIT, {} ) && UiCall( addr::CRT_CINIT, { 1 } );
}

/* QAA engine key numbers (Quake III's, its pad buttons at K_JOY1 = 185)
 * from FnQL's. PC deviation: the keyboard drives the menus as the pad does
 * (SFUI translates through the table at 0x827EBBC8). */
int QaaKey( int key ) {
	if ( key >= 'a' && key <= 'z' ) {
		return key - 'a' + 'A';
	}
	if ( key < K_MOUSE1 ) {
		return key;	// ASCII, then K_BACKSPACE .. K_KP_EQUALS as Quake III
	}
	switch ( key ) {
	case K_PAD0_SOUTH: return 185;
	case K_PAD0_EAST: return 186;
	case K_PAD0_WEST: return 187;
	case K_PAD0_NORTH: return 188;
	case K_PAD0_LEFTSHOULDER: return 189;
	case K_PAD0_RIGHTSHOULDER: return 190;
	case K_PAD0_BACK: return 191;
	case K_PAD0_START: return 192;
	case K_PAD0_DPAD_UP: case K_PAD0_LEFTSTICK_UP: return K_UPARROW;
	case K_PAD0_DPAD_DOWN: case K_PAD0_LEFTSTICK_DOWN: return K_DOWNARROW;
	case K_PAD0_DPAD_LEFT: case K_PAD0_LEFTSTICK_LEFT: return K_LEFTARROW;
	case K_PAD0_DPAD_RIGHT: case K_PAD0_LEFTSTICK_RIGHT: return K_RIGHTARROW;
	case K_PAD0_LEFTTRIGGER: return 160;
	case K_PAD0_RIGHTTRIGGER: return 161;
	default: return -1;
	}
}

/* ==== the mouse =============================================================================

PC deviation: the Xbox has no mouse. The movies' own mouse handlers do
not select or activate anything: QAA's FgUIComponent uses onRollOver,
onPress, onRollOut and onRelease only to show its _over, _down and _up
frames (activate() calls onActivate(), then onRelease() for the look), and
SFUI registers no mouse with GFx (the movie's mouse count, +0x9B8, is 0).
What selects and activates is the scene, FgScene: setSelection(i) moves
the selection over its selectionArray, and its onKeyDown takes A to
currentSelection.activate(), B to goBack() and the rest to the selection's
own onKeyDown (a list's rows, a slider's value). So the pointer drives the
scenes as the pad does:

- over a selectable component (its hitTest, by bounding box, in the
  stage's pixels; of overlapping boxes, the one the pointer is most
  central in) it selects the component, and inside a grid
  (FgGridSelector) or a list (FgListBox) the square or row too; off every
  component the selection stays;
- the left button presses A on what it is over (PointerPress for sliders
  and rotators, and the title);
- the right button (and the fourth) presses B, as Escape; the wheel moves
  the selection, as the arrow keys (a list scrolls).

Everything is queued and run before each step of the update thread, while
it is parked, through the movie's GetVariable and Invoke, so the movies'
ActionScript runs as it does for SFUI's own invokes; the keys then go
through SFUI's key queue (SFUI_KEY_EVENT). The pointer is the system's:
the platform layers show it and report absolute positions while the front
end owns it (Key_GetPointerCatcher). */

/* GFxValue (GFx 2.x): the type, then the value at +8 */
enum { GFXV_UNDEFINED = 0, GFXV_NULL = 1, GFXV_BOOL = 2, GFXV_NUMBER = 3, GFXV_STRING = 4 };
constexpr uint32_t GFXV_SIZE = 0x10;

struct PointerEvent {
	enum Kind { MOVE, CLICK, KEY } kind;
	int key;		// KEY: the engine key for SFUI
	bool down;
	float x, y;		// viewport pixels
};
std::vector<PointerEvent> pointerEvents;
float pointerX, pointerY;			// viewport pixels, as last reported
uint32_t mouseKeysTaken;			// bit per mouse key whose press the front end took
uint32_t gfxScratch;				// guest: a GFxValue result, arguments, a path

void QueuePointerEvent( const PointerEvent &e ) {
	if ( e.kind == PointerEvent::MOVE && !pointerEvents.empty() && pointerEvents.back().kind == PointerEvent::MOVE ) {
		pointerEvents.back() = e;	// moves join
	} else if ( pointerEvents.size() < 64 ) {
		pointerEvents.push_back( e );
	}
}

/* The movie that takes the input (SFUI's key handler, 0x82198698). */
uint32_t InputMovie() {
	const uint32_t count = QAA_LoadU32( sfui + addr::SFUI_MOVIE_COUNT ), movies = QAA_LoadU32( sfui + addr::SFUI_MOVIES );
	return count && movies ? QAA_LoadU32( movies + 4 * ( count - 1 ) ) : 0;
}

void StoreDouble( uint32_t a, double v ) {
	uint64_t u;
	std::memcpy( &u, &v, 8 );
	QAA_StoreU32( a, static_cast<uint32_t>( u >> 32 ) );
	QAA_StoreU32( a + 4, static_cast<uint32_t>( u ) );
}

double LoadDouble( uint32_t a ) {
	const uint64_t u = static_cast<uint64_t>( QAA_LoadU32( a ) ) << 32 | QAA_LoadU32( a + 4 );
	double v = 0;
	std::memcpy( &v, &u, 8 );
	return v;
}

/* A GFxValue that is a number or a boolean, as a number. */
bool ValueNumber( uint32_t v, double *out ) {
	switch ( QAA_LoadU32( v ) & 0xF ) {
	case GFXV_NUMBER:
		*out = LoadDouble( v + 8 );
		return true;
	case GFXV_BOOL:
		*out = QAA_LoadU8( v + 8 ) ? 1.0 : 0.0;
		return true;
	default:
		return false;
	}
}

bool ScratchReady() {
	return gfxScratch || ( gfxScratch = GCalloc( 0x200 ) ) != 0;
}

/* Invoke(path, result, args, count) (movie vtable +0x48) with number and
 * boolean arguments ({ value, is a boolean }); *out: the result as a number. */
bool GfxInvoke( uint32_t movie, const std::string &path, std::initializer_list<std::pair<double, bool>> args,
	double *out = nullptr ) {
	if ( !ScratchReady() || args.size() > 4 ) {
		return false;
	}
	const uint32_t result = gfxScratch, argv = gfxScratch + GFXV_SIZE, text = gfxScratch + 0x80;
	QAA_StoreU32( result, GFXV_UNDEFINED );
	uint32_t a = argv;
	for ( const auto &arg : args ) {
		QAA_StoreU32( a, arg.second ? GFXV_BOOL : GFXV_NUMBER );
		QAA_StoreU32( a + 4, 0 );
		if ( arg.second ) {
			QAA_StoreU32( a + 8, 0 );
			QAA_StoreU8( a + 8, arg.first != 0.0 );
		} else {
			StoreDouble( a + 8, arg.first );
		}
		a += GFXV_SIZE;
	}
	GStrcpy( text, path.c_str(), 0x180 );
	uint64_t ok = 0;
	if ( !UiCall( QAA_LoadU32( QAA_LoadU32( movie ) + addr::MOVIE_INVOKE ),
		{ movie, text, result, argv, static_cast<uint64_t>( args.size() ) }, &ok ) || !( ok & 0xFF ) ) {
		return false;
	}
	return !out || ValueNumber( result, out );
}

/* GetVariable(result, path) (movie vtable +0x30) into the scratch block:
 * the value's type, or -1 when the path does not resolve. */
int GfxType( uint32_t movie, const std::string &path ) {
	if ( !ScratchReady() ) {
		return -1;
	}
	const uint32_t result = gfxScratch, text = gfxScratch + 0x80;
	QAA_StoreU32( result, GFXV_UNDEFINED );
	GStrcpy( text, path.c_str(), 0x180 );
	uint64_t ok = 0;
	if ( !UiCall( QAA_LoadU32( QAA_LoadU32( movie ) + addr::MOVIE_GET_VARIABLE ), { movie, result, text }, &ok ) || !( ok & 0xFF ) ) {
		return -1;
	}
	return static_cast<int>( QAA_LoadU32( result ) & 0xF );
}

/* GetVariable: a number or a boolean. */
bool GfxNumber( uint32_t movie, const std::string &path, double *out ) {
	return GfxType( movie, path ) >= 0 && ValueNumber( gfxScratch, out );
}

/* The current scene, as _global.sceneManager.currentScene's getter finds
 * it (FgSceneManager2: movies[movies.length - 1].scene): GetVariable does
 * not run getters. Empty when there is none. */
std::string ScenePath( uint32_t movie ) {
	double movies = 0;
	if ( !GfxNumber( movie, "_global.sceneManager.movies.length", &movies ) || movies < 1 ) {
		return std::string();
	}
	return "_global.sceneManager.movies." + std::to_string( static_cast<int>( movies ) - 1 ) + ".scene";
}

/* The pointer, in the stage's pixels (as GFx maps its mouse, 0x82650C10). */
struct StagePoint {
	double x, y;
};

StagePoint ToStage( uint32_t movie, float x, float y ) {
	return { QAA_LoadF( movie + addr::MOVIE_STAGE_SCALE_X ) * x + QAA_LoadF( movie + addr::MOVIE_STAGE_OFFSET_X ),
		QAA_LoadF( movie + addr::MOVIE_STAGE_SCALE_Y ) * y + QAA_LoadF( movie + addr::MOVIE_STAGE_OFFSET_Y ) };
}

/* clip.hitTest(x, y) by bounding box, in stage pixels. */
bool HitTest( uint32_t movie, const std::string &clip, double x, double y ) {
	double hit = 0;
	return GfxInvoke( movie, clip + ".hitTest", { { x, false }, { y, false }, { 0.0, true } }, &hit ) && hit != 0.0;
}

/* A clip that is shown and, for a component, selectable. */
bool Usable( uint32_t movie, const std::string &clip ) {
	double v = 0;
	return !( GfxNumber( movie, clip + ".isSelectable", &v ) && v == 0.0 ) && !( GfxNumber( movie, clip + "._visible", &v ) && v == 0.0 );
}

/* Where clip's bounding box crosses the line through p along x (or y), in
 * stage pixels: in one span, whose ends are found by halving. */
void SpanThrough( uint32_t movie, const std::string &clip, StagePoint p, bool vertical, double limit, double *lo, double *hi ) {
	const double at = vertical ? p.y : p.x;
	auto hit = [&]( double v ) {
		return vertical ? HitTest( movie, clip, p.x, v ) : HitTest( movie, clip, v, p.y );
	};
	auto edge = [&]( double inside, double outside ) {
		if ( hit( outside ) ) {
			return outside;
		}
		for ( int i = 0; i < 11 && std::fabs( outside - inside ) > 1.0; i++ ) {
			const double mid = ( inside + outside ) * 0.5;
			( hit( mid ) ? inside : outside ) = mid;
		}
		return inside;
	};
	*lo = edge( at, 0.0 );
	*hi = edge( at, limit );
}

double SpanMiddle( uint32_t movie, const std::string &clip, StagePoint p, bool vertical, double limit ) {
	double lo, hi;
	SpanThrough( movie, clip, p, vertical, limit, &lo, &hi );
	return ( lo + hi ) * 0.5;
}

/* The stage's size in pixels. */
StagePoint StageSize( uint32_t movie ) {
	return { QAA_LoadF( movie + addr::MOVIE_STAGE_SCALE_X ) * ( displayW > 0 ? displayW : cls.glconfig.vidWidth ),
		QAA_LoadF( movie + addr::MOVIE_STAGE_SCALE_Y ) * ( displayH > 0 ? displayH : cls.glconfig.vidHeight ) };
}

/* The usable element of path[0 .. count) under p, -1 for none. Neighbours'
 * boxes overlap (a menu item's holds its highlight bar, often far wider
 * than its text), so of several the one p is most central in, measured in
 * each box's own half-width and half-height. */
int ElementAt( uint32_t movie, const std::string &path, int count, StagePoint p ) {
	int found[4], hits = 0;
	for ( int i = 0; i < count && i < 64 && hits < 4; i++ ) {
		const std::string clip = path + "." + std::to_string( i );
		if ( HitTest( movie, clip, p.x, p.y ) && Usable( movie, clip ) ) {
			found[hits++] = i;
		}
	}
	if ( hits <= 1 ) {
		return hits ? found[0] : -1;
	}
	const StagePoint size = StageSize( movie );
	int best = -1;
	double bestDistance = 0;
	for ( int k = 0; k < hits; k++ ) {
		const std::string clip = path + "." + std::to_string( found[k] );
		double x0, x1, y0, y1;
		SpanThrough( movie, clip, p, false, size.x, &x0, &x1 );
		SpanThrough( movie, clip, p, true, size.y, &y0, &y1 );
		const double dx = ( p.x - ( x0 + x1 ) * 0.5 ) / std::max( ( x1 - x0 ) * 0.5, 1.0 );
		const double dy = ( p.y - ( y0 + y1 ) * 0.5 ) / std::max( ( y1 - y0 ) * 0.5, 1.0 );
		const double distance = dx * dx + dy * dy;
		if ( best < 0 || distance < bestDistance ) {
			best = found[k];
			bestDistance = distance;
		}
	}
	return best;
}

/* What the pointer is over: the current scene's component, and inside a
 * grid (FgGridSelector: grid[], setGridSelection) or a list (FgListBox:
 * rowList[], setSelection) its cell. */
struct PointerTarget {
	std::string scene;			// empty: no scene
	int count = -1;				// the scene's components
	int selection = -1;			// the scene's selection
	int component = -1;			// under the pointer
	std::string path;			// the component's
	enum { PLAIN, GRID, LIST, SLIDER, ROTATOR } kind = PLAIN;
	int cells = 0, currentCell = -1, cell = -1;
};

/* What kind of component t->path is; with p, the cell under it. */
void Classify( uint32_t movie, PointerTarget *t, const StagePoint *p ) {
	double v = 0;
	if ( GfxNumber( movie, t->path + ".grid.length", &v ) ) {
		t->kind = PointerTarget::GRID;
		t->cells = static_cast<int>( v );
		t->currentCell = GfxNumber( movie, t->path + ".currentGridSelection", &v ) ? static_cast<int>( v ) : -1;
		t->cell = p ? ElementAt( movie, t->path + ".grid", t->cells, *p ) : -1;
	} else if ( GfxNumber( movie, t->path + ".rowList.length", &v ) ) {
		t->kind = PointerTarget::LIST;
		t->cells = static_cast<int>( v );
		t->currentCell = GfxNumber( movie, t->path + ".currentRow", &v ) ? static_cast<int>( v ) : -1;
		t->cell = p ? ElementAt( movie, t->path + ".rowList", t->cells, *p ) : -1;
	} else if ( GfxType( movie, t->path + ".moveLeft" ) > GFXV_UNDEFINED && GfxType( movie, t->path + ".line" ) > GFXV_UNDEFINED ) {
		t->kind = PointerTarget::SLIDER;	// FgSlider: line, the track; data, the thumb
	} else if ( GfxType( movie, t->path + ".setNext" ) > GFXV_UNDEFINED ) {
		t->kind = PointerTarget::ROTATOR;	// FgRotator: left and right step its value
	}
}

PointerTarget TargetAt( uint32_t movie, StagePoint p ) {
	PointerTarget t;
	t.scene = ScenePath( movie );
	double v = 0;
	if ( t.scene.empty() || !GfxNumber( movie, t.scene + ".selectionArray.length", &v ) ) {
		return t;
	}
	t.count = static_cast<int>( v );
	t.selection = GfxNumber( movie, t.scene + ".selectionIndex", &v ) ? static_cast<int>( v ) : -1;
	const std::string components = t.scene + ".selectionArray";
	t.component = ElementAt( movie, components, t.count, p );
	if ( t.component < 0 ) {
		return t;
	}
	t.path = components + "." + std::to_string( t.component );
	Classify( movie, &t, &p );
	return t;
}

/* Selects what the pointer is over, as the pad would: the component in
 * the scene, then the cell in its grid or list. */
void SelectTarget( uint32_t movie, const PointerTarget &t ) {
	if ( t.component < 0 ) {
		return;
	}
	if ( t.component != t.selection ) {
		if ( qaa_uiTrace && qaa_uiTrace->integer ) {
			Com_Printf( "QAA ui: pointer selects component %d of %d (was %d)\n", t.component, t.count, t.selection );
		}
		GfxInvoke( movie, t.scene + ".setSelection", { { static_cast<double>( t.component ), false } } );
	}
	if ( t.cell >= 0 && t.cell != t.currentCell ) {
		if ( qaa_uiTrace && qaa_uiTrace->integer ) {
			Com_Printf( "QAA ui: pointer selects %s %d of %d (was %d)\n", t.kind == PointerTarget::GRID ? "square" : "row",
				t.cell, t.cells, t.currentCell );
		}
		GfxInvoke( movie, t.path + ( t.kind == PointerTarget::GRID ? ".setGridSelection" : ".setSelection" ),
			{ { static_cast<double>( t.cell ), false } } );
	}
}

/* A key for SFUI, as QAA_UI_KeyEvent hands keys over. */
void SendUiKey( int key, bool down ) {
	PublishClientState();
	UiCall( addr::SFUI_KEY_EVENT, { static_cast<uint64_t>( key ), down ? 1u : 0u, 0 } );
}

/* Where clip's bounding box crosses the row y, in stage pixels: sampled
 * across the stage, then each edge found by halving. False if it does not. */
bool RowSpan( uint32_t movie, const std::string &clip, double y, double *left, double *right ) {
	const double width = StageSize( movie ).x;
	constexpr int SAMPLES = 48;
	int first = -1, last = -1;
	for ( int i = 0; i <= SAMPLES; i++ ) {
		if ( HitTest( movie, clip, width * i / SAMPLES, y ) ) {
			if ( first < 0 ) {
				first = i;
			}
			last = i;
		}
	}
	if ( first < 0 ) {
		return false;
	}
	auto edge = [&]( double inside, double outside ) {
		for ( int i = 0; i < 8 && std::fabs( outside - inside ) > 0.5; i++ ) {
			const double mid = ( inside + outside ) * 0.5;
			( HitTest( movie, clip, mid, y ) ? inside : outside ) = mid;
		}
		return inside;
	};
	*left = first > 0 ? edge( width * first / SAMPLES, width * ( first - 1 ) / SAMPLES ) : 0.0;
	*right = last < SAMPLES ? edge( width * last / SAMPLES, width * ( last + 1 ) / SAMPLES ) : width;
	return true;
}

/* A slider being dragged by its track: the value follows the pointer. */
struct SliderDrag {
	bool active = false;
	std::string path;
	double left = 0, right = 0;		// the track, stage pixels
	double value = 0;				// last set
} sliderDrag;

/* setValue at the pointer's place on the track, snapped to the slider's
 * step from its minimum (FgSlider: _minValue, _maxValue, _datastep). */
void SlideTo( uint32_t movie, double x ) {
	double lo = 0, hi = 1, step = 0;
	if ( !GfxNumber( movie, sliderDrag.path + "._minValue", &lo ) || !GfxNumber( movie, sliderDrag.path + "._maxValue", &hi ) ) {
		return;
	}
	GfxNumber( movie, sliderDrag.path + "._datastep", &step );
	const double f = sliderDrag.right > sliderDrag.left
		? std::clamp( ( x - sliderDrag.left ) / ( sliderDrag.right - sliderDrag.left ), 0.0, 1.0 ) : 0.0;
	double v = lo + f * ( hi - lo );
	if ( step > 0 ) {
		v = std::clamp( lo + std::round( ( v - lo ) / step ) * step, std::min( lo, hi ), std::max( lo, hi ) );
	}
	if ( v != sliderDrag.value ) {
		sliderDrag.value = v;
		if ( qaa_uiTrace && qaa_uiTrace->integer ) {
			Com_Printf( "QAA ui: pointer sets slider to %g\n", v );
		}
		GfxInvoke( movie, sliderDrag.path + ".setValue", { { v, false } } );
	}
}

/* The left button's press: the key it became, released with it. */
int clickKey;

/* The left button: what it is over is selected, then pressed:
 * - a button, a list's row, a grid's square: A;
 * - a slider's track: its value there (and it follows while held); left
 *   or right of the track (the arrows): left or right;
 * - a rotator's value: left or right of its middle;
 * - a slider's or rotator's label: nothing more.
 * On a scene whose one component is a button (the title), a click anywhere
 * is on it; on a scene with none (the start-up movies), A. */
void PointerPress( uint32_t movie, float x, float y ) {
	int key = K_ENTER;
	if ( movie ) {
		const StagePoint p = ToStage( movie, x, y );
		PointerTarget t = TargetAt( movie, p );
		if ( t.component < 0 && t.count == 1 && Usable( movie, t.scene + ".selectionArray.0" ) ) {
			// the scene's only component, if a button (the title's "Press A
			// to start"); not a list, whose A would take its current row
			PointerTarget only = t;
			only.path = t.scene + ".selectionArray.0";
			Classify( movie, &only, nullptr );
			if ( only.kind == PointerTarget::PLAIN ) {
				t = only;
				t.component = 0;
			}
		}
		if ( qaa_uiTrace && qaa_uiTrace->integer ) {
			Com_Printf( "QAA ui: pointer click on component %d of %d, cell %d of %d\n", t.component, t.count, t.cell, t.cells );
		}
		if ( t.component < 0 && t.count > 0 ) {
			return;	// beside every component
		}
		SelectTarget( movie, t );
		if ( ( t.kind == PointerTarget::GRID || t.kind == PointerTarget::LIST ) && t.cell < 0 ) {
			return;	// a grid or list, beside its cells
		}
		if ( t.kind == PointerTarget::SLIDER || t.kind == PointerTarget::ROTATOR ) {
			if ( HitTest( movie, t.path + ".label", p.x, p.y ) ) {
				return;
			}
			// the control's row: the pointer's, else the middle of the component's
			const double rows[2] = { p.y, SpanMiddle( movie, t.path, p, true, StageSize( movie ).y ) };
			double left = 0, right = 0;
			bool found = false;
			for ( double row : rows ) {
				if ( t.kind == PointerTarget::SLIDER ) {
					found = RowSpan( movie, t.path + ".line", row, &left, &right );
				} else if ( RowSpan( movie, t.path, row, &left, &right ) ) {
					double labelLeft, labelRight;	// the value: right of the label
					if ( RowSpan( movie, t.path + ".label", row, &labelLeft, &labelRight ) && labelRight < right ) {
						left = std::max( left, labelRight );
					}
					found = true;
				}
				if ( found ) {
					break;
				}
			}
			if ( !found ) {
				return;
			}
			if ( t.kind == PointerTarget::SLIDER && p.x >= left && p.x <= right ) {
				sliderDrag.active = true;
				sliderDrag.path = t.path;
				sliderDrag.left = left;
				sliderDrag.right = right;
				sliderDrag.value = std::numeric_limits<double>::quiet_NaN();
				SlideTo( movie, p.x );
				return;
			}
			key = p.x < ( t.kind == PointerTarget::SLIDER ? left : ( left + right ) * 0.5 ) ? K_LEFTARROW : K_RIGHTARROW;
		}
	}
	clickKey = key;
	SendUiKey( key, true );
}

/* Before each step of the update thread: the pointer's events, in order. */
void RunPointerEvents() {
	if ( pointerEvents.empty() ) {
		return;
	}
	std::vector<PointerEvent> events;
	events.swap( pointerEvents );
	const uint32_t movie = InputMovie();
	for ( const PointerEvent &e : events ) {
		switch ( e.kind ) {
		case PointerEvent::MOVE:
			if ( !movie ) {
				break;
			}
			if ( sliderDrag.active ) {
				SlideTo( movie, ToStage( movie, e.x, e.y ).x );
			} else {
				SelectTarget( movie, TargetAt( movie, ToStage( movie, e.x, e.y ) ) );
			}
			break;
		case PointerEvent::CLICK:
			if ( e.down ) {
				PointerPress( movie, e.x, e.y );
			} else {
				sliderDrag.active = false;
				if ( clickKey ) {
					SendUiKey( clickKey, false );
					clickKey = 0;
				}
			}
			break;
		case PointerEvent::KEY:
			SendUiKey( e.key, e.down );
			break;
		}
	}
}

int MouseKeyBit( int key ) {
	switch ( key ) {
	case K_MOUSE1: return 1;
	case K_MOUSE2: return 2;
	case K_MOUSE3: return 4;
	case K_MOUSE4: return 8;
	case K_MOUSE5: return 16;
	case K_MWHEELUP: return 32;
	case K_MWHEELDOWN: return 64;
	default: return 0;
	}
}

/* A mouse button or the wheel: the front end takes a press while it owns
 * the pointer, and the release of every press it took (so a press the game
 * took is released in the game, and the reverse). */
bool MouseKeyEvent( int key, bool down ) {
	const uint32_t bit = static_cast<uint32_t>( MouseKeyBit( key ) );
	if ( down ) {
		if ( !QAA_UI_WantsPointer() || ( Key_GetCatcher() & KEYCATCH_CONSOLE ) ) {
			return false;
		}
		mouseKeysTaken |= bit;
	} else {
		if ( !( mouseKeysTaken & bit ) ) {
			return false;
		}
		mouseKeysTaken &= ~bit;
	}
	PointerEvent e = { PointerEvent::KEY, 0, down, pointerX, pointerY };
	switch ( key ) {
	case K_MOUSE1:
		e.kind = PointerEvent::CLICK;
		break;
	case K_MOUSE2:
	case K_MOUSE4:
		e.key = K_ESCAPE;
		break;
	case K_MWHEELUP:
		e.key = K_UPARROW;
		break;
	case K_MWHEELDOWN:
		e.key = K_DOWNARROW;
		break;
	default:
		return true;	// taken, not used
	}
	QueuePointerEvent( e );
	return true;
}

/* qaa_uiGet <path>: a variable of the movie that takes the input, as
 * GetVariable resolves the path (no getters: the current scene is
 * _global.sceneManager.movies.<n - 1>.scene). For working on the menus. */
void UI_Get_f( void ) {
	const uint32_t movie = InputMovie();
	const int type = movie ? GfxType( movie, Cmd_Argv( 1 ) ) : -1;
	const uint32_t value = gfxScratch;
	double n = 0;
	if ( type < 0 ) {
		Com_Printf( "%s: not found\n", Cmd_Argv( 1 ) );
	} else if ( type == GFXV_STRING ) {
		Com_Printf( "%s = \"%s\"\n", Cmd_Argv( 1 ), GStr( QAA_LoadU32( value + 8 ) ).c_str() );
	} else if ( ValueNumber( value, &n ) ) {
		Com_Printf( "%s = %g%s\n", Cmd_Argv( 1 ), n, type == GFXV_BOOL ? " (boolean)" : "" );
	} else {
		Com_Printf( "%s = %s\n", Cmd_Argv( 1 ), type == GFXV_NULL ? "null" : "undefined" );
	}
}

/* qaa_uiTestPointer x y: the pointer at renderer pixel (x, y), through the
 * engine's absolute pointer path (headless tests). */
void UI_TestPointer_f( void ) {
	CL_MouseAbsoluteEvent( atoi( Cmd_Argv( 1 ) ), atoi( Cmd_Argv( 2 ) ) );
}

bool updateStalled;	// the last step of the update thread did not finish

/*
 * SFUI's render (0x8218E5A8, through 0x82196E48) ends by setting the
 * update thread's frame event (0x8219706C): on the console the render
 * thread lets the update thread work out the next frame while it goes on.
 * Here that iteration ran beside the engine, which is single-threaded:
 * with a map on the command line it read the title's images while
 * SV_SpawnServer restarted the filesystem, and the thread died (a host
 * access violation in FS_Read), leaving the front end dead for the run.
 * QAA: PC deviation (FnQL is single-threaded): while the render runs the
 * frame event's slot (+0x361ED8) holds a private event, so the render sets
 * that one; the update thread is parked on the real event then and does
 * not read the slot. If the render set it, the iteration it lets go runs
 * as one step of the update thread before the draw goes on, with the
 * engine waiting. As before, each render lets one iteration go.
 */
bool RenderMovies() {
	static uint32_t release;	// the event the render sets instead
	const uint32_t slot = sfui + addr::SFUI_FRAME_EVENT;
	const uint32_t thread = QAA_LoadU32( sfui + addr::SFUI_THREAD );
	const uint32_t frame = QAA_LoadU32( slot );
	if ( !release ) {
		uint64_t handle = 0;
		if ( UiCall( addr::CREATE_EVENT, { 0, 1, 0, 0 }, &handle ) ) {
			release = static_cast<uint32_t>( handle );
		}
	}
	const bool redirect = thread && frame && release
		&& Api().qaa_guest_wait_thread_blocked( Guest(), thread, frame, updateStalled ? 0 : 5000 ) == QAA_OK;
	if ( redirect ) {
		QAA_StoreU32( slot, release );
	}
	const bool rendered = UiCall( addr::SFUI_RENDER, {} );
	if ( !redirect ) {
		return rendered;
	}
	QAA_StoreU32( slot, frame );
	uint64_t set = 0;
	if ( UiCall( addr::WAIT_FOR_OBJECT, { release, 0 }, &set ) && static_cast<uint32_t>( set ) == 0 ) {
		UiCall( addr::RESET_EVENT, { release } );
		const bool stalled = updateStalled;
		updateStalled = Api().qaa_guest_step_thread( Guest(), thread, frame, 5000 ) != QAA_OK;
		if ( updateStalled && !stalled ) {
			Com_Printf( S_COLOR_YELLOW "QAA ui: the update thread did not finish the frame the render began (%s)\n",
				Api().qaa_guest_last_error( Guest() ) );
		}
	}
	return rendered;
}

} // namespace

/* ==== public ================================================================================== */

bool QAA_UI_WantsPointer( void ) {
	// the menus, and everything the front end shows outside a game (start-up
	// movies, title, loading); in a game, only while a menu holds the keys
	return QAA_UI_Active() && ( cls.state != CA_ACTIVE || ( Key_GetCatcher() & KEYCATCH_UI )
		|| ( QAA_LoadU32( addr::CLS_KEYCATCHERS ) & KEYCATCH_UI ) );
}

void QAA_UI_PointerMove( int x, int y ) {
	if ( !QAA_UI_Active() ) {
		return;
	}
	// renderer pixels to the movie's viewport buffer (the video mode, as SFUI
	// sets it from the glconfig it is given)
	const int vidW = std::max( cls.glconfig.vidWidth, 1 ), vidH = std::max( cls.glconfig.vidHeight, 1 );
	pointerX = static_cast<float>( x ) * ( displayW > 0 ? displayW : vidW ) / vidW;
	pointerY = static_cast<float>( y ) * ( displayH > 0 ? displayH : vidH ) / vidH;
	QueuePointerEvent( { PointerEvent::MOVE, 0, false, pointerX, pointerY } );
}

bool QAA_UI_Init( void ) {
	if ( uiReady ) {
		return true;
	}
	qaa_uiEnable = Cvar_Get( "qaa_ui", "1", CVAR_LATCH );
	Cvar_SetDescription( qaa_uiEnable, "Run Quake Arena Arcade's Scaleform front end (menus, HUD, scoreboards), "
		"recompiled from the package's default.xex." );
	qaa_uiTrace = Cvar_Get( "qaa_uiTrace", "0", 0 );
	qaa_uiTestFrames = Cvar_Get( "qaa_uiTestFrames", "0", 0 );
	qaa_uiProfile = Cvar_Get( "qaa_uiProfile", "0", 0 );
	qaa_uiTestVerify = Cvar_Get( "qaa_uiTestVerify", "0", 0 );
	Cvar_SetDescription( qaa_uiTestVerify, "Check every frame the front end draws against a full redraw of the same "
		"triangles on a fresh canvas, reporting each that differs (slow; for tests)." );
	Cvar_SetDescription( Cvar_Get( "qaa_country", "", CVAR_ARCHIVE ), "The console region the front end sees, two "
		"letters (\"us\" shows the ESRB rating screen at start-up). Empty: the operating system's region." );
	Cvar_SetDescription( qaa_uiProfile, "Print the front end's average cost per client frame once a second: "
		"pointer events, update thread, SFUI render, recording, rasterising, upload." );
	Cvar_Get( "sys_titleFocused", "1", CVAR_ROM );
	// QAA: the Live state the menus test, as retail's Live start-up registers
	// it (0x82185E28..0x82185E68, ROM | TEMP) and its session code leaves it
	// without a Live sign-in: not connected, not in a party. The movies read
	// them with getCvarValue, a number from the cvar's string.
	for ( const char *name : { "xb_signinRunning", "xb_liveConnected", "xb_isInParty", "xb_primaryLiveConnected" } ) {
		Cvar_Get( name, "0", CVAR_ROM | CVAR_TEMP );
	}
	// the server browser flag (0x8217B6C0), which the menus set and clear
	Cvar_Get( "ui_inServerBrowser", "0", 0 );
	Cmd_AddCommand( "qaa_uiDump", UI_Dump_f );
	Cmd_AddCommand( "qaa_uiTestPointer", UI_TestPointer_f );
	Cmd_AddCommand( "qaa_uiGet", UI_Get_f );
	if ( !qaa_uiEnable->integer || uiFailed ) {
		return false;
	}
	uiModule = QAA_Host_LoadModule( "ui" );
	if ( !uiModule ) {
		uiFailed = true;
		return false;
	}
	InstallHooks();
	if ( !RunCrtStartup() ) {
		Com_Printf( S_COLOR_YELLOW "QAA ui: the C runtime start-up failed\n" );
		return false;
	}
	PublishGlconfig();
	PublishEngineObjects();
	PublishProfile();
	InstallCallTracing();
	if ( !UiCall( addr::SFUI_INIT, {} ) ) {
		return false;
	}
	sfui = QAA_LoadU32( addr::SFUI_STATE_PTR );
	if ( !sfui ) {
		Com_Printf( S_COLOR_YELLOW "QAA ui: SFUI did not start\n" );
		return false;
	}
	Com_Printf( "QAA ui: Scaleform front end started (SFUI %08X, renderer %08X)\n", sfui, sfuiRenderer );
	// QAA: retail CL_InitUI (0x8218E490) sets ui_ethernetActive from
	// XNetGetEthernetLinkStatus; System Link refuses to start without it
	// (ui_main registers it as "0"). PC deviation: the link is FnQL's
	// networking, up unless net_enabled turns it off.
	Cvar_Set( "ui_ethernetActive", Cvar_VariableIntegerValue( "net_enabled" ) ? "1" : "0" );
	uiReady = true;
	QAA_SV_GameUiTrap = QAA_UI_GameTrap;
	return true;
}

void QAA_UI_Shutdown( void ) {
	uiReady = false;
	QAA_SV_GameUiTrap = nullptr;
}

bool QAA_UI_Active( void ) {
	return uiReady && !uiFailed;
}

bool QAA_UI_IsWidescreen( int width, int height ) {
	return width * 9 >= height * 16 - 16;
}

#ifdef _WIN32
// kernel32 (Windows 10 1709 and later); declared here to keep windows.h out
extern "C" __declspec( dllimport ) int __stdcall GetUserDefaultGeoName( wchar_t *geoName, int geoNameCount );
#endif

/* Two letters of a region name ("GB", "en_GB.UTF-8" from its '_'), lower-cased. */
static bool CountryFrom( const char *name, char out[3] ) {
	if ( !name ) {
		return false;
	}
	if ( const char *territory = strchr( name, '_' ) ) {
		name = territory + 1;
	}
	if ( !isalpha( static_cast<unsigned char>( name[0] ) ) || !isalpha( static_cast<unsigned char>( name[1] ) )
		|| isalpha( static_cast<unsigned char>( name[2] ) ) ) {
		return false;
	}
	out[0] = static_cast<char>( tolower( static_cast<unsigned char>( name[0] ) ) );
	out[1] = static_cast<char>( tolower( static_cast<unsigned char>( name[1] ) ) );
	out[2] = 0;
	return true;
}

const char *QAA_UI_CountryCode( void ) {
	static char code[3];
	const char *forced = Cvar_VariableString( "qaa_country" );
	if ( forced[0] && CountryFrom( forced, code ) ) {
		return code;
	}
#ifdef _WIN32
	wchar_t geo[16] = {};
	char narrow[16] = {};
	if ( GetUserDefaultGeoName( geo, 16 ) > 0 ) {
		for ( int i = 0; i < 15 && geo[i]; i++ ) {
			narrow[i] = geo[i] < 128 ? static_cast<char>( geo[i] ) : '?';
		}
		if ( CountryFrom( narrow, code ) ) {
			return code;
		}
	}
#else
	for ( const char *var : { "LC_ALL", "LC_MESSAGES", "LANG" } ) {
		const char *value = getenv( var );
		if ( value && value[0] ) {
			// "en_GB.UTF-8": only a value with a territory names a region
			if ( strchr( value, '_' ) && CountryFrom( value, code ) ) {
				return code;
			}
			break;
		}
	}
#endif
	Q_strncpyz( code, "us", sizeof( code ) );
	return code;
}

/* qaa_uiProfile 1: average microseconds per client frame, once a second. */
namespace uiprof {
enum Stage { POINTER, UPDATE, RENDER, RECORD, RASTER, UPLOAD, COUNT };
const char *const names[COUNT] = { "pointer", "update", "render", "record", "raster", "upload" };
int64_t total[COUNT];
int frames, updates, redraws, repeats, started;
int64_t tested, tris, rowsDrawn, uploadedRows, rowsKept;

int64_t Now() {
	return Sys_Microseconds();
}

void Add( Stage stage, int64_t since ) {
	total[stage] += Now() - since;
}

void EndFrame() {
	if ( !qaa_uiProfile || !qaa_uiProfile->integer ) {
		return;
	}
	frames++;
	const int now = Sys_Milliseconds();
	if ( !started ) {
		started = now;
	}
	if ( now - started < 1000 ) {
		return;
	}
	std::string line;
	for ( int i = 0; i < COUNT; i++ ) {
		line += va( " %s %lld", names[i], static_cast<long long>( total[i] / frames ) );
		total[i] = 0;
	}
	Com_Printf( "QAA ui profile: %d frames/s, %d updates, %d redraws, %d repeated, us/frame:%s, per redraw %lld "
		"triangles %lld kpixels %lld rows drawn %lld rows uploaded, per frame %lld rows kept as hidden (canvas %dx%d)\n",
		frames, updates, redraws, repeats, line.c_str(),
		static_cast<long long>( redraws ? tris / redraws : 0 ),
		static_cast<long long>( redraws ? tested / redraws / 1000 : 0 ),
		static_cast<long long>( redraws ? rowsDrawn / redraws : 0 ),
		static_cast<long long>( redraws ? uploadedRows / redraws : 0 ), static_cast<long long>( rowsKept / frames ),
		canvas.Width(), canvas.Height() );
	frames = updates = redraws = repeats = 0;
	tested = tris = rowsDrawn = uploadedRows = rowsKept = 0;
	started = now;
}
}

void QAA_UI_Frame( void ) {
	// headless tests: qaa_uiTestFrames N dumps the canvas after N frames and quits
	if ( qaa_uiTestFrames && qaa_uiTestFrames->integer > 0 ) {
		static int testFrames;
		testFrames++;
		// qaa_uiTestKeys "frame:KEY frame:KEY ...": scripted presses (key names as
		// bind); +KEY only presses, -KEY only releases
		char script[MAX_CVAR_VALUE_STRING];
		Cvar_VariableStringBuffer( "qaa_uiTestKeys", script, sizeof( script ) );
		for ( const char *p = script; *p; ) {
			while ( *p == ' ' ) {
				p++;
			}
			const int at = atoi( p );
			const char *colon = strchr( p, ':' );
			if ( !colon ) {
				break;
			}
			char name[64];
			int n = 0;
			const char *q = colon + 1;
			while ( *q && *q != ' ' && n < 63 ) {
				name[n++] = *q++;
			}
			name[n] = 0;
			if ( at == testFrames ) {
				const bool press = name[0] != '-', release = name[0] != '+';
				const int key = Key_StringToKeynum( name + ( press && release ? 0 : 1 ) );
				if ( key > 0 ) {
					Com_Printf( "QAA ui: test key %s at frame %d\n", name, testFrames );
					if ( press ) {
						CL_KeyEvent( key, qtrue, Sys_Milliseconds() );
					}
					if ( release ) {
						CL_KeyEvent( key, qfalse, Sys_Milliseconds() );
					}
				}
			}
			p = q;
		}
		// qaa_uiTestCmds "frame:command|frame:command": console commands at frames
		char cmds[MAX_CVAR_VALUE_STRING];
		Cvar_VariableStringBuffer( "qaa_uiTestCmds", cmds, sizeof( cmds ) );
		for ( char *p = cmds; p && *p; ) {
			char *next = strchr( p, '|' );
			if ( next ) {
				*next++ = 0;
			}
			char *colon = strchr( p, ':' );
			if ( colon && atoi( p ) == testFrames ) {
				Com_Printf( "QAA ui: test command '%s' at frame %d\n", colon + 1, testFrames );
				Cbuf_AddText( va( "%s\n", colon + 1 ) );
			}
			p = next;
		}
		// qaa_uiTestTraceFrame N: trace every draw of frame N
		const int traceFrame = Cvar_VariableIntegerValue( "qaa_uiTestTraceFrame" );
		if ( traceFrame > 0 && testFrames == traceFrame ) {
			Cvar_Set( "qaa_uiTrace", "6" );
		} else if ( traceFrame > 0 && testFrames == traceFrame + 1 ) {
			Cvar_Set( "qaa_uiTrace", "0" );
		}
		const int every = Cvar_VariableIntegerValue( "qaa_uiTestEvery" );
		if ( testFrames == qaa_uiTestFrames->integer || ( every > 0 && testFrames % every == 0 ) ) {
			dumpRequests++;
		} else if ( testFrames == qaa_uiTestFrames->integer + 2 ) {
			if ( qaa_uiTestVerify->integer ) {
				Com_Printf( "QAA ui: %d frames checked against a full redraw, %d differed\n", canvas.VerifiedFlushes(),
					canvas.VerifyMismatches() );
			}
			Cbuf_AddText( "quit\n" );
		}
	}
	if ( !QAA_UI_Active() ) {
		return;
	}
	PublishGlconfig();
	PublishClientState();
	PublishProfile();
	SyncAllMirrors();
	PollDeviceSelection();
	PollConfigOperation();
	PollSessionEnd();
	PollHostStart();
	PollGameStartedBeforeTitle();
	struct Collect {
		~Collect() { CollectKeyCatchers(); }
	} collect;
	const uint32_t thread = QAA_LoadU32( sfui + addr::SFUI_THREAD );
	const uint32_t frame = QAA_LoadU32( sfui + addr::SFUI_FRAME_EVENT );
	if ( !thread || !frame ) {
		return;
	}
	// SFUI's update thread (0x82196458) advances every movie by 1/60 s per
	// iteration, run by the engine's 60 Hz frame event: here one iteration
	// per 1/60 s of real time, at most four per client frame. Each runs the
	// queued invokes and input and advances the movies, with the engine
	// waiting. PC deviation: retail overlaps it with the render thread; here
	// the two never run at once, as FnQL is single-threaded.
	static int lastTime;
	static double pending;
	const int now = Sys_Milliseconds();
	pending += lastTime ? ( now - lastTime ) * ( 60.0 / 1000.0 ) : 1.0;
	lastTime = now;
	if ( pending > 4.0 ) {
		pending = 4.0;
	}
	if ( qaa_uiTestFrames->integer > 0 && pending < 1.0 ) {
		pending = 1.0;	// headless tests step every frame
	}
	while ( pending >= 1.0 ) {
		pending -= 1.0;
		int64_t stepStart = uiprof::Now();
		RunPointerEvents();	// before this iteration's input and Advance
		uiprof::Add( uiprof::POINTER, stepStart );
		stepStart = uiprof::Now();
		const int stepped = Api().qaa_guest_step_thread( Guest(), thread, frame, 5000 );
		uiprof::Add( uiprof::UPDATE, stepStart );
		uiprof::updates++;
		updateStalled = stepped != QAA_OK;
		if ( stepped != QAA_OK ) {
			Com_Printf( S_COLOR_YELLOW "QAA ui: the update thread did not finish its frame (%s)\n",
				Api().qaa_guest_last_error( Guest() ) );
			break;
		}
		updatesPending = true;
	}
}

void QAA_UI_Draw( void ) {
	if ( !QAA_UI_Active() ) {
		return;
	}
	struct EndFrame {
		~EndFrame() {
			// Nothing stays queued past the draw: queued triangles point at
			// textures, which may be freed before the next one.
			canvas.Flush();
			uiprof::EndFrame();
		}
	} endFrame;
	drawThread = std::this_thread::get_id();
	ReleaseRetiredTextures();
	// The canvas is the size of the video mode, up to 4096 pixels a side
	// (scaled to fit, keeping the aspect); the renderer stretches it over the
	// screen.
	const int vidW = cls.glconfig.vidWidth, vidH = cls.glconfig.vidHeight;
	const float fit = std::min( 1.0f, 4096.0f / static_cast<float>( std::max( { vidW, vidH, 1 } ) ) );
	const int canvasW = std::max( 1, static_cast<int>( vidW * fit ) ), canvasH = std::max( 1, static_cast<int>( vidH * fit ) );
	// with the ranges of rows of presentBuffer that changed since the last call
	static const std::vector<int> unchanged;
	auto present = [&]( const std::vector<int> &changed ) {
		re.DrawWebUISurface( 0, 0, vidW, vidH, canvas.Width(), canvas.Height(), presentBuffer.data(),
			changed.data(), static_cast<int>( changed.size() / 2 ) );
	};
	// a frame with no update shows the last one again
	if ( !updatesPending && presentValid && canvas.Width() == canvasW && canvas.Height() == canvasH ) {
		present( unchanged );
		return;
	}
	updatesPending = false;
	presentValid = false;
	int64_t stageStart = uiprof::Now();
	canvas.Resize( canvasW, canvasH );
	canvas.SetVerify( qaa_uiTestVerify && qaa_uiTestVerify->integer );
	canvas.Clear();
	canvasDrawn = false;
	if ( !RenderMovies() ) {
		return;
	}
	uiprof::Add( uiprof::RENDER, stageStart );
	stageStart = uiprof::Now();
	ExecuteCommands();
	uiprof::Add( uiprof::RECORD, stageStart );
	if ( qaa_uiTrace->integer > 1 ) {
		Com_Printf( "QAA ui: draw %s, %d triangles, movies %d\n", canvasDrawn ? "recorded" : "empty",
			canvas.Triangles(), QAA_LoadS32( sfui + 0x21E8 ) );
		std::string counts;
		for ( int i = 0; i < 23; i++ ) {
			if ( commandCounts[i] ) {
				counts += va( " %d:%d", i, commandCounts[i] );
			}
		}
		Com_Printf( "QAA ui: commands%s (view %dx%d)\n", counts.c_str(), canvas.Width(), canvas.Height() );
	}
	if ( !canvasDrawn ) {
		return;
	}
	framesDrawn++;
	if ( dumpRequests > 0 ) {
		dumpRequests--;
		DumpCanvas( va( "screenshots/qaa_ui_%04d.tga", framesDrawn ) );
	}
	// Rasterises the bands whose triangles differ from the last frame's
	// (none when the frame repeats), then converts what changed.
	stageStart = uiprof::Now();
	const int mismatchesBefore = canvas.VerifyMismatches();
	canvas.ToStraight( presentBuffer );
	if ( canvas.VerifyMismatches() != mismatchesBefore ) {
		Com_Printf( S_COLOR_YELLOW "QAA ui: frame %d differs from a full redraw of its triangles\n", framesDrawn );
	}
	uiprof::Add( uiprof::RASTER, stageStart );
	uiprof::rowsKept += canvas.RowsKept();
	if ( canvas.RowsDrawn() ) {
		uiprof::redraws++;
		uiprof::tris += canvas.Triangles();
		uiprof::tested += canvas.PixelsTested();
		uiprof::rowsDrawn += canvas.RowsDrawn();
	} else {
		uiprof::repeats++;
	}
	stageStart = uiprof::Now();
	const std::vector<int> &changed = canvas.ChangedRows();
	for ( size_t i = 0; i + 1 < changed.size(); i += 2 ) {
		uiprof::uploadedRows += changed[i + 1] - changed[i];
	}
	present( changed );
	presentValid = true;
	uiprof::Add( uiprof::UPLOAD, stageStart );
}

/*
 * As retail CL_KeyEvent (0x82106D20): while SFUI holds KEYCATCH_UI it gets
 * every key; otherwise Escape (or Start) in a game opens the in-game menu,
 * and every other key goes to the engine's bindings. The console keeps its
 * keys (PC deviation: the Xbox has no console).
 */
bool QAA_UI_KeyEvent( int key, bool down, unsigned time ) {
	if ( QAA_UI_Active() && MouseKeyBit( key ) ) {
		return MouseKeyEvent( key, down );
	}
	if ( !QAA_UI_Active() || ( Key_GetCatcher() & KEYCATCH_CONSOLE ) || key == K_CONSOLE ) {
		return false;
	}
	const int k = QaaKey( key );
	if ( k < 0 || k >= 0xC4 ) {
		return false;
	}
	PublishClientState();
	struct Collect {
		~Collect() { CollectKeyCatchers(); }
	} collect;
	const bool uiHasKeys = ( QAA_LoadU32( addr::CLS_KEYCATCHERS ) & KEYCATCH_UI ) != 0;
	if ( down && qaa_uiTrace && qaa_uiTrace->integer ) {
		Com_Printf( "QAA ui: key %d (%d) catchers %X state %d paused %d\n", key, k,
			QAA_LoadU32( addr::CLS_KEYCATCHERS ), cls.state, Cvar_VariableIntegerValue( "cl_paused" ) );
	}
	if ( !uiHasKeys ) {
		if ( down && ( key == K_ESCAPE || key == K_PAD0_START ) && cls.state == CA_ACTIVE ) {
			// retail opens the menu only while the game is not paused, and
			// takes the key either way
			if ( !Cvar_VariableIntegerValue( "cl_paused" ) ) {
				QAA_StoreU32( sfui + addr::SFUI_INGAME_MENU_VALUE, 1 );
				UiCall( addr::SFUI_SET_GLOBAL_ARRAY, { sfui, 1, addr::STR_UI_VALUES, 4, sfui + addr::SFUI_INGAME_MENU_VALUE } );
			}
			return true;
		}
		if ( cls.state == CA_ACTIVE ) {
			return false;
		}
	}
	// QAA: PC deviation. Match Results leaves on Start (its scene's
	// onUnhandledKey sends dismissPostGame) and turns Back off (disableBack),
	// so Escape, which SFUI reads as Back, did nothing there, and a keyboard
	// without a keypad has no Start (keypad Enter). Escape is Start on that
	// screen; its release follows its press.
	static bool escapeAsStart;
	int sent = k;
	if ( key == K_ESCAPE ) {
		if ( down ) {
			escapeAsStart = QAA_LoadU32( gamestate::ADDRESS ) == gamestate::POSTGAME;
		}
		if ( escapeAsStart ) {
			sent = QaaKey( K_PAD0_START );
		}
	}
	// (key, down, controller): the PC has one player, on controller 0
	UiCall( addr::SFUI_KEY_EVENT, { static_cast<uint64_t>( sent ), down ? 1u : 0u, 0 } );
	return true;
}

/*
 * The cgame traps that feed the front end, as retail's CL_CgameSystemCalls
 * (0x821008B0) handles them: most queue an SFUI event ("on<name>" in the
 * movies, 0x82191098, with a format string) or publish a _global array
 * (0x82196D40); the score blocks are copied where SFUI's menus read them.
 * Guest addresses pass through unchanged: the cgame and the front end share
 * one guest address space. Rumble, achievements and Xbox Live are not here.
 */
namespace cgui {
constexpr uint32_t QUEUE_EVENT = 0x82191098;		// (name, format, ...)
constexpr uint32_t SET_GLOBAL_ARRAY = 0x82196D40;	// (sfui, count, path, bytes, values)
constexpr uint32_t SET_TOURNAMENT = 0x821968E0;		// (c1 name, c2 name, data)
constexpr uint32_t CACHE_SCORES = 0x82196A40;		// (scores, host name)
constexpr uint32_t SET_KILLER = 0x82196C68;			// (name, info)
constexpr uint32_t FIRE_EVENT = 0x82190FE8;			// (name)
constexpr uint32_t SET_SCORE_DETAIL = 0x8218E670;	// (block 0x240, name 0x100, block 0x200)
constexpr uint32_t QUEUE_INVOKE = 0x8218E770;		// (menu id, path, format, ...)
constexpr uint32_t MOVIE_NAMES = 0x82C44288;		// 0x40 bytes per menu id
constexpr uint32_t S_CLIENT_LOAD_COMPLETE = 0x820BADF4, S_GOTO_SCENE = 0x820D02A8;
// format strings and names in default.xex
constexpr uint32_t S_WAITBEGIN = 0x820CF568, S_D_S = 0x820BAD10, S_LOADING_DOTS = 0x820CF60C;
constexpr uint32_t S_ITEMPICKUP = 0x820BAD18, S_OBITUARY = 0x820BAD30, S_D_S_S = 0x820BAD24;
constexpr uint32_t S_S = 0x820BABA0, S_S_S = 0x820BAD3C, S_S_S_D_D = 0x820BAD44;
constexpr uint32_t S_FOLLOWING = 0x820BAD58, S_S_D = 0x820BAD50, S_VOICE = 0x820BAD74, S_D_D_S = 0x820BAD68;
constexpr uint32_t S_REWARD = 0x820BAD8C, S_D_D = 0x820BAD84, S_HUDDATA = 0x820D2288;
// data the menus read
constexpr uint32_t POSTGAME_PLAYERS = 0x82A766F0;	// 16 x 0x48 (cgame 117)
constexpr uint32_t POSTGAME_STATS = 0x82A76B90;		// 16 x 0x6B8
constexpr uint32_t POSTGAME_A = 0x82C01C1C, POSTGAME_B = 0x82C01C20;
constexpr uint32_t SCOREBOARD_A = 0x82CEAB00;		// 0x6B80 (cgame 121)
constexpr uint32_t SCOREBOARD_B = 0x82CF5E20;		// 0x480
constexpr uint32_t SCOREBOARD_N1 = 0x82CF3190, SCOREBOARD_N2 = 0x82CF8570;
constexpr uint32_t SFUI_HUDDATA = 0x361EDC, SFUI_HUDDATA_COPY = 0x361FE4;	// +: 0x108 bytes each
constexpr uint32_t SFUI_UIVALUES = 0x361EC8;		// +: _global.uiValues
}

void QAA_UI_ClientLoadComplete( void ) {
	if ( QAA_UI_Active() ) {
		UiCall( cgui::FIRE_EVENT, { cgui::S_CLIENT_LOAD_COMPLETE } );
	}
}

void QAA_UI_EnterGame( void ) {
	if ( QAA_UI_Active() ) {
		// menu id 4: the "hud" entry of SFUI's movie table (filled at 0x8218E048)
		UiCall( cgui::QUEUE_INVOKE, { 4, cgui::S_GOTO_SCENE, cgui::S_S, cgui::MOVIE_NAMES + 4 * 0x40 } );
	}
}

intptr_t QAA_UI_CgameTrap( int trap, const int32_t *a ) {
	if ( !QAA_UI_Active() ) {
		return 0;
	}
	auto u = [&]( int k ) { return static_cast<uint64_t>( static_cast<uint32_t>( a[k] ) ); };
	auto copy = [&]( uint32_t dst, uint32_t src, uint32_t size ) {
		if ( src ) {
			std::memmove( QAA_Ptr( dst ), QAA_Ptr( src ), size );
		}
	};
	if ( qaa_uiTrace && qaa_uiTrace->integer > 1 ) {
		Com_Printf( "QAA ui: cgame trap %d (%08X %08X %08X)\n", trap, a[0], a[1], a[2] );
	}
	switch ( trap ) {
	case 116:	// QAA_CG_UI_POSTGAME_PLAYER: the local player's record (an engine helper keeps it)
		return 0;
	case 117:	// QAA_CG_UI_117: the post-game tables
		for ( uint32_t i = 0; i < 16; i++ ) {
			copy( cgui::POSTGAME_PLAYERS + 0x48 * i, a[0] ? static_cast<uint32_t>( a[0] ) + 0x48 * i : 0, 0x48 );
			copy( cgui::POSTGAME_STATS + 0x6B8 * i, a[1] ? static_cast<uint32_t>( a[1] ) + 0x6B8 * i : 0, 0x6B8 );
		}
		QAA_StoreU32( cgui::POSTGAME_A, static_cast<uint32_t>( a[2] ) );
		QAA_StoreU32( cgui::POSTGAME_B, static_cast<uint32_t>( a[3] ) );
		return 0;
	case 118:	// QAA_CG_UI_WAIT_BEGIN
		UiCall( cgui::QUEUE_EVENT, { cgui::S_WAITBEGIN, cgui::S_D_S, 0, cgui::S_LOADING_DOTS } );
		return 0;
	case 119:	// QAA_CG_UI_HUD_DATA: 0x108 bytes as _global.hudData
		if ( a[0] ) {
			copy( sfui + cgui::SFUI_HUDDATA, static_cast<uint32_t>( a[0] ), 0x108 );
			copy( sfui + cgui::SFUI_HUDDATA_COPY, static_cast<uint32_t>( a[0] ), 0x108 );
			UiCall( cgui::SET_GLOBAL_ARRAY, { sfui, 1, cgui::S_HUDDATA, 0x108, sfui + cgui::SFUI_HUDDATA } );
		}
		return 0;
	case 120:	// QAA_CG_UI_CACHE_SCORES (host name, scores)
		if ( a[0] && a[1] ) {
			UiCall( cgui::CACHE_SCORES, { u( 1 ), u( 0 ) } );
		}
		return 0;
	case 121:	// QAA_CG_UI_SCOREBOARD (n, block 0x6B80, n, block 0x480)
		QAA_StoreU32( cgui::SCOREBOARD_N1, static_cast<uint32_t>( a[0] ) );
		copy( cgui::SCOREBOARD_A, static_cast<uint32_t>( a[1] ), 0x6B80 );
		copy( cgui::SCOREBOARD_B, static_cast<uint32_t>( a[3] ), 0x480 );
		QAA_StoreU32( cgui::SCOREBOARD_N2, static_cast<uint32_t>( a[2] ) );
		return 0;
	case 122:	// QAA_CG_UI_EVENT (name)
		UiCall( cgui::FIRE_EVENT, { u( 0 ) } );
		return 0;
	case 123:	// ItemPickup
		UiCall( cgui::QUEUE_EVENT, { cgui::S_ITEMPICKUP, cgui::S_D_S, u( 0 ), u( 1 ) } );
		return 0;
	case 124:	// Obituary
		UiCall( cgui::QUEUE_EVENT, { cgui::S_OBITUARY, cgui::S_D_S_S, u( 0 ), u( 1 ), u( 2 ) } );
		return 0;
	case 125:	// QAA_CG_UI_KILLER_STATS (name, info)
		if ( a[1] ) {
			UiCall( cgui::SET_KILLER, { u( 0 ), u( 1 ) } );
		}
		return 0;
	case 126:	// event with a string
		UiCall( cgui::QUEUE_EVENT, { u( 0 ), cgui::S_S, u( 1 ) } );
		return 0;
	case 127:	// event with two strings
		UiCall( cgui::QUEUE_EVENT, { u( 0 ), cgui::S_S_S, u( 1 ), u( 2 ) } );
		return 0;
	case 128:	// event with two strings and two numbers
		UiCall( cgui::QUEUE_EVENT, { u( 0 ), cgui::S_S_S_D_D, u( 1 ), u( 2 ), u( 3 ), u( 4 ) } );
		return 0;
	case 129:	// FollowingPlayer (name, on)
		if ( a[1] == 1 ) {
			UiCall( cgui::QUEUE_EVENT, { cgui::S_FOLLOWING, cgui::S_S_D, u( 0 ), 1 } );
		}
		return 0;
	case 130:	// QAA_CG_UI_TOURNAMENT
		if ( a[0] ) {
			UiCall( cgui::SET_TOURNAMENT, { u( 0 ), u( 1 ), u( 2 ) } );
		}
		return 0;
	case 131:	// PlayerVoiceInfo
		UiCall( cgui::QUEUE_EVENT, { cgui::S_VOICE, cgui::S_D_D_S, u( 0 ), u( 1 ), u( 2 ) } );
		return 0;
	case 132:	// QAA_CG_UI_SET_VALUE (index, value)
		QAA_StoreS32( sfui + cgui::SFUI_UIVALUES + 4 * a[0], a[1] );
		UiCall( cgui::SET_GLOBAL_ARRAY, { sfui, 1, addr::STR_UI_VALUES, 4, sfui + cgui::SFUI_UIVALUES } );
		return 0;
	case 135:	// Reward
		UiCall( cgui::QUEUE_EVENT, { cgui::S_REWARD, cgui::S_D_D, u( 0 ), u( 1 ) } );
		return 0;
	case 140:	// QAA_CG_UI_140
		UiCall( cgui::SET_SCORE_DETAIL, { u( 0 ), u( 1 ), u( 2 ) } );
		return 0;
	default:
		return 0;
	}
}

/*
 * The qagame's menu traps, as retail's G_SystemCalls (0x82130438) handles
 * them: 48 goes to a stack path from the main menu, 49 opens an in-game
 * menu (0x8218EC70), 65 publishes a single-player course's results as
 * _global.courseResults and 69 caches the host name and scores, as cgame
 * 120 does. The qagame shares the front end's guest address space.
 */
namespace ggui {
constexpr uint32_t QUEUE_INVOKE_MAIN = 0x8218E8A8, S_GOTO_STACK_PATH = 0x820D1824, S_S_S = 0x820BAD3C;
constexpr uint32_t S_MAINMENU = 0x820D15C4;
constexpr uint32_t OPEN_MENU = 0x8218EC70, CACHE_SCORES = 0x8218E610;
constexpr uint32_t SFUI_COURSE_RESULTS = 0x3626F8, COURSE_RESULTS_SIZE = 0x4C, S_COURSE_RESULTS = 0x820D2398;
}

intptr_t QAA_UI_GameTrap( int trap, const int32_t *a ) {
	if ( !QAA_UI_Active() ) {
		return trap == 49 ? -1 : 0;
	}
	const uint64_t a0 = static_cast<uint32_t>( a[0] ), a1 = static_cast<uint32_t>( a[1] );
	if ( qaa_uiTrace && qaa_uiTrace->integer > 1 ) {
		Com_Printf( "QAA ui: game trap %d (%08X %08X)\n", trap, a[0], a[1] );
	}
	switch ( trap ) {
	case 48:	// QAA_G_UI_GOTO_STACK_PATH (path)
		if ( a0 ) {
			UiCall( ggui::QUEUE_INVOKE_MAIN, { ggui::S_GOTO_STACK_PATH, ggui::S_S_S, a0, ggui::S_MAINMENU } );
		}
		return 0;
	case 49:	// QAA_G_UI_INGAME_MENU (menu id)
		UiCall( ggui::OPEN_MENU, { a0 } );
		return -1;
	case 65:	// QAA_G_UI_COURSE_RESULTS (0x4C bytes)
		if ( a0 ) {
			std::memmove( QAA_Ptr( sfui + ggui::SFUI_COURSE_RESULTS ), QAA_Ptr( static_cast<uint32_t>( a0 ) ),
				ggui::COURSE_RESULTS_SIZE );
			UiCall( addr::SFUI_SET_GLOBAL_ARRAY, { sfui, 1, ggui::S_COURSE_RESULTS, ggui::COURSE_RESULTS_SIZE,
				sfui + ggui::SFUI_COURSE_RESULTS } );
		}
		return 0;
	case 69:	// QAA_G_UI_CACHE_SCORES (host name, scores)
		UiCall( ggui::CACHE_SCORES, { a0, a1 } );
		return 0;
	default:
		return 0;
	}
}
