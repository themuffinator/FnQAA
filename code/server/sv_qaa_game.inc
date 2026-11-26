/*
===========================================================================
FnQAA: hosting QAA's recompiled qagame (milestone M4).

Copyright (C) 2026 FnQAA contributors.
This file is part of FnQAA, licensed under the GNU GPL v2 or later.

Included at the end of code/server/sv_game.cpp, whose helpers it uses.

The game module is QAA's own qagameXe.dll, recompiled from the user's
package (tools/recomp/build.py) and run on the guest runtime. Its data is
big-endian and laid out as Quake III 1.32 lays it out; the engine's is
FnQL's (Quake Live's). This file is the boundary (facts: docs/ABI.md):

- vmMain: FnQL's command numbers become QAA's (9 and 10 move to 10 and 12;
  9 and 11 are QAA's own), and the engine calls QAA's 11 after every client
  begin and disconnect, as retail does.
- Traps: QAA's numbering (game_traps.tsv), each argument converted.
- The engine function table QAA's engine hands qagame through vmMain 9.
- The botlib traps (sv_qaa_botlib.inc): FnQL's botlib with QAA's structures.
- Entities and player states: the engine works on host copies in FnQL's
  layout. A copy is refreshed from guest memory whenever the engine asks
  for it (SV_GentityNum, SV_GameClientNum) and after every call into the
  module, and written back after every engine call that changes it (link,
  unlink, brush model, bot slot).
===========================================================================
*/

#include "../qaa/qaa_structs.h"

/* ---- the module and its entities --------------------------------------- */

static qaa_module *qaaGame;
static uint32_t qaaGameEngineTable;	// guest address of the 10-entry engine table
static uint32_t qaaGEnts, qaaGEntSize, qaaGClients, qaaGClientSize;
static int qaaNumGEnts;
static sharedEntity_t qaaHostEnts[MAX_GENTITIES];
static playerState_t qaaHostClients[MAX_CLIENTS];
static int qaaRefreshHold;	// >0 while an engine call is changing a host copy

static cvar_t *qaa_game;

static inline bool QAA_SV_Active( void ) {
	return qaaGame != nullptr && qaaGEnts != 0;
}

static void QAA_EntityFromGuest( int num ) {
	using namespace qaa_layout;
	const uint32_t a = qaaGEnts + static_cast<uint32_t>( num ) * qaaGEntSize;
	sharedEntity_t *e = &qaaHostEnts[num];
	entityShared_t *r = &e->r;
	QAA_EntityStateFromGuest( a, &e->s );
	r->linked = static_cast<qboolean>( QAA_LoadS32( a + oSE_R + oR_LINKED ) );
	r->linkcount = QAA_LoadS32( a + oSE_R + oR_LINKCOUNT );
	r->svFlags = QAA_LoadS32( a + oSE_R + oR_SVFLAGS );
	r->singleClient = QAA_LoadS32( a + oSE_R + oR_SINGLECLIENT );
	r->bmodel = static_cast<qboolean>( QAA_LoadS32( a + oSE_R + oR_BMODEL ) );
	QAA_LoadVec3( a + oSE_R + oR_MINS, r->mins );
	QAA_LoadVec3( a + oSE_R + oR_MAXS, r->maxs );
	r->contents = QAA_LoadS32( a + oSE_R + oR_CONTENTS );
	QAA_LoadVec3( a + oSE_R + oR_ABSMIN, r->absmin );
	QAA_LoadVec3( a + oSE_R + oR_ABSMAX, r->absmax );
	QAA_LoadVec3( a + oSE_R + oR_CURRENTORIGIN, r->currentOrigin );
	QAA_LoadVec3( a + oSE_R + oR_CURRENTANGLES, r->currentAngles );
	r->ownerNum = QAA_LoadS32( a + oSE_R + oR_OWNERNUM );
}

static void QAA_EntityToGuest( int num ) {
	using namespace qaa_layout;
	const uint32_t a = qaaGEnts + static_cast<uint32_t>( num ) * qaaGEntSize;
	const sharedEntity_t *e = &qaaHostEnts[num];
	const entityShared_t *r = &e->r;
	QAA_EntityStateToGuest( a, &e->s );
	QAA_StoreS32( a + oSE_R + oR_LINKED, r->linked );
	QAA_StoreS32( a + oSE_R + oR_LINKCOUNT, r->linkcount );
	QAA_StoreS32( a + oSE_R + oR_SVFLAGS, r->svFlags );
	QAA_StoreS32( a + oSE_R + oR_SINGLECLIENT, r->singleClient );
	QAA_StoreS32( a + oSE_R + oR_BMODEL, r->bmodel );
	QAA_StoreVec3( a + oSE_R + oR_MINS, r->mins );
	QAA_StoreVec3( a + oSE_R + oR_MAXS, r->maxs );
	QAA_StoreS32( a + oSE_R + oR_CONTENTS, r->contents );
	QAA_StoreVec3( a + oSE_R + oR_ABSMIN, r->absmin );
	QAA_StoreVec3( a + oSE_R + oR_ABSMAX, r->absmax );
	QAA_StoreVec3( a + oSE_R + oR_CURRENTORIGIN, r->currentOrigin );
	QAA_StoreVec3( a + oSE_R + oR_CURRENTANGLES, r->currentAngles );
	QAA_StoreS32( a + oSE_R + oR_OWNERNUM, r->ownerNum );
}

/* Called by SV_GentityNum and SV_GameClientNum. */
static void QAA_SV_RefreshEntity( int num ) {
	if ( QAA_SV_Active() && !qaaRefreshHold && num >= 0 && num < qaaNumGEnts ) {
		QAA_EntityFromGuest( num );
	}
}

static void QAA_SV_RefreshClient( int num ) {
	if ( QAA_SV_Active() && !qaaRefreshHold && qaaGClients && num >= 0 && num < sv_maxclients->integer && num < MAX_CLIENTS ) {
		QAA_PlayerStateFromGuest( qaaGClients + static_cast<uint32_t>( num ) * qaaGClientSize, &qaaHostClients[num] );
	}
}

static void QAA_SV_RefreshAll( void ) {
	if ( !QAA_SV_Active() ) {
		return;
	}
	for ( int i = 0; i < qaaNumGEnts; i++ ) {
		QAA_EntityFromGuest( i );
	}
	for ( int i = 0; i < sv_maxclients->integer && i < MAX_CLIENTS; i++ ) {
		QAA_SV_RefreshClient( i );
	}
}

/* The host copy of the guest entity at a; NULL when a is not one of them. */
static sharedEntity_t *QAA_HostEntity( uint32_t a, int *numOut = nullptr ) {
	if ( !a || !qaaGEnts || a < qaaGEnts || qaaGEntSize == 0 ) {
		return nullptr;
	}
	const uint32_t index = ( a - qaaGEnts ) / qaaGEntSize;
	if ( ( a - qaaGEnts ) % qaaGEntSize != 0 || index >= static_cast<uint32_t>( qaaNumGEnts ) ) {
		Com_Error( ERR_DROP, "qagame passed an entity pointer %08X outside its entities", a );
	}
	QAA_EntityFromGuest( static_cast<int>( index ) );
	if ( numOut ) {
		*numOut = static_cast<int>( index );
	}
	return &qaaHostEnts[index];
}

/* Runs an engine call that changes entity num, then writes it back. */
template <typename Fn>
static void QAA_WithEntity( uint32_t a, Fn &&fn ) {
	int num;
	sharedEntity_t *ent = QAA_HostEntity( a, &num );
	if ( !ent ) {
		return;
	}
	qaaRefreshHold++;
	fn( ent );
	qaaRefreshHold--;
	QAA_EntityToGuest( num );
}

/* ---- traps ------------------------------------------------------------- */

enum qaaGameTrap_t {
	QG_PRINT = 0, QG_ERROR, QG_MILLISECONDS, QG_CVAR_REGISTER, QG_CVAR_UPDATE, QG_CVAR_SET,
	QG_CVAR_VARIABLE_INTEGER_VALUE, QG_CVAR_VARIABLE_STRING_BUFFER, QG_ARGC, QG_ARGV,
	QG_FS_FOPEN_FILE, QG_FS_READ, QG_FS_WRITE, QG_FS_FCLOSE_FILE, QG_SEND_CONSOLE_COMMAND,
	QG_LOCATE_GAME_DATA, QG_DROP_CLIENT, QG_SEND_SERVER_COMMAND, QG_SET_CONFIGSTRING, QG_GET_CONFIGSTRING,
	QG_GET_USERINFO, QG_SET_USERINFO, QG_GET_SERVERINFO, QG_SET_BRUSH_MODEL, QG_TRACE, QG_POINT_CONTENTS,
	QG_IN_PVS, QG_IN_PVS_IGNORE_PORTALS, QG_ADJUST_AREA_PORTAL_STATE, QG_AREAS_CONNECTED,
	QG_ACTIVE_CLIENT_COUNT,	// 30, QAA's
	QG_LINKENTITY, QG_UNLINKENTITY, QG_ENTITIES_IN_BOX, QG_ENTITY_CONTACT, QG_BOT_ALLOCATE_CLIENT,
	QG_BOT_FREE_CLIENT, QG_GET_USERCMD, QG_GET_ENTITY_TOKEN, QG_FS_GETFILELIST, QG_DEBUG_POLYGON_CREATE,
	QG_DEBUG_POLYGON_DELETE, QG_REAL_TIME, QG_SNAPVECTOR, QG_TRACECAPSULE, QG_ENTITY_CONTACTCAPSULE,
	QG_MATCH_END = 46, QG_AWARD_ACHIEVEMENT, QG_UI_GOTO_STACK_PATH, QG_UI_INGAME_MENU,
	QG_LIVE_SESSION_INFO_4, QG_LIVE_SESSION_INFO_5, QG_LIVE_SET_PROPERTY,
	QG_LIVE_MEMBER_SET1 = 56, QG_LIVE_MEMBER_SET0, QG_LIVE_MEMBER_GET0,
	QG_STUB_61 = 61, QG_STUB_62, QG_STUB_63, QG_MATCH_ARM_END, QG_UI_COURSE_RESULTS,
	QG_LIVE_WRITE_LEADERBOARD, QG_LIVE_SESSION_INFO_1, QG_LIVE_SESSION_INFO_0, QG_UI_CACHE_SCORES,
	QG_LIVE_SET_GAMEMODE, QG_LIVE_SET_PROPERTY_1, QG_SEND_SCOREBOARD, QG_FS_SEEK,
	QG_MEMSET = 100, QG_MEMCPY, QG_STRNCPY, QG_SIN, QG_COS, QG_ATAN2, QG_SQRT, QG_MATRIXMULTIPLY,
	QG_ANGLEVECTORS, QG_PERPENDICULARVECTOR, QG_FLOOR, QG_CEIL,
	QG_BOTLIB_FIRST = 200, QG_BOTLIB_LAST = 581,
};

#include "sv_qaa_botlib.inc"

/*
 * The game-to-cgame scoreboard mailbox (QAA trap 72 -> cgame trap 141). It
 * works without Xbox Live, so a local game needs it (docs/ABI.md).
 */
byte qaa_scoreboardMailbox[0x24C];
int qaa_scoreboardMailboxSize;
bool qaa_scoreboardMailboxFull;

/* The client's front end, for the menu traps (qcommon.h). */
intptr_t ( *QAA_SV_GameUiTrap )( int trap, const int32_t *args );

static int64_t QAA_GameSyscall( void *, qaa_module *, const qaa_syscall *call ) {
	const int32_t *a = call->args;	// a[0] is the first argument
	const auto vec = []( uint32_t p ) { return QAAVec3( p ); };

	if ( gvm && gvm->syscallCount++ >= 1024 * 1024 ) {
		Com_Error( ERR_DROP, "game VM syscall overflow - Loss of control in VM" );
	}

	switch ( call->trap ) {
	case QG_PRINT:
		Com_Printf( "%s", QAA_Str( a[0] ) );
		return 0;
	case QG_ERROR:
		Com_Error( ERR_DROP, "%s", QAA_Str( a[0] ) );
	case QG_MILLISECONDS:
		return SV_LockstepActive() ? SV_LockstepMilliseconds() : Sys_Milliseconds();
	case QG_CVAR_REGISTER: {
		QAAVmCvar vc( a[0] );
		Cvar_RegisterLegacy( vc.Host(), QAA_Str( a[1] ), QAA_Str( a[2] ), a[3], gvm->privateFlag );
		vc.WriteBack();
		return 0;
	}
	case QG_CVAR_UPDATE: {
		QAAVmCvar vc( a[0] );
		Cvar_UpdateLegacy( vc.Host(), gvm->privateFlag );
		vc.WriteBack();
		return 0;
	}
	case QG_CVAR_SET:
		// QAA: Cvar_Set2( name, value, force = 1 )
		Cvar_Set2( QAA_Str( a[0] ), QAA_Str( a[1] ), qtrue );
		return 0;
	case QG_CVAR_VARIABLE_INTEGER_VALUE:
		return Cvar_VariableIntegerValue( QAA_Str( a[0] ) );
	case QG_CVAR_VARIABLE_STRING_BUFFER:
		Cvar_VariableStringBufferSafe( QAA_Str( a[0] ), static_cast<char *>( QAA_Ptr( a[1] ) ), a[2], gvm->privateFlag );
		return 0;
	case QG_ARGC:
		return Cmd_Argc();
	case QG_ARGV:
		Cmd_ArgvBuffer( a[0], static_cast<char *>( QAA_Ptr( a[1] ) ), a[2] );
		return 0;
	case QG_FS_FOPEN_FILE: {
		fileHandle_t f = 0;
		const int length = FS_VM_OpenFile( QAA_Str( a[0] ), a[1] ? &f : nullptr, static_cast<fsMode_t>( a[2] ), H_QAGAME );
		if ( a[1] ) {
			QAA_StoreS32( a[1], f );
		}
		return length;
	}
	case QG_FS_READ:
		if ( a[1] <= 0 ) {
			return 0;
		}
		FS_VM_ReadFile( QAA_Ptr( a[0] ), a[1], a[2], H_QAGAME );
		return 0;
	case QG_FS_WRITE:
		FS_VM_WriteFile( QAA_Ptr( a[0] ), a[1], a[2], H_QAGAME );
		return 0;
	case QG_FS_FCLOSE_FILE:
		FS_VM_CloseFile( a[0], H_QAGAME );
		return 0;
	case QG_FS_SEEK:
		return FS_VM_SeekFile( a[0], a[1], static_cast<fsOrigin_t>( a[2] ), H_QAGAME );
	case QG_FS_GETFILELIST:
		return FS_GetFileList( QAA_Str( a[0] ), QAA_Str( a[1] ), static_cast<char *>( QAA_Ptr( a[2] ) ), a[3] );
	case QG_SEND_CONSOLE_COMMAND:
		if ( a[0] >= EXEC_NOW && a[0] <= EXEC_APPEND ) {
			Cbuf_ExecuteText( static_cast<cbufExec_t>( a[0] ), QAA_Str( a[1] ) );
		}
		return 0;

	case QG_LOCATE_GAME_DATA: {
		const int num = a[1];
		if ( num < 0 || num > MAX_GENTITIES ) {
			Com_Error( ERR_DROP, "qagame: LocateGameData with %d entities", num );
		}
		qaaGEnts = static_cast<uint32_t>( a[0] );
		qaaNumGEnts = num;
		qaaGEntSize = static_cast<uint32_t>( a[2] );
		qaaGClients = static_cast<uint32_t>( a[3] );
		qaaGClientSize = static_cast<uint32_t>( a[4] );
		for ( int i = 0; i < qaaNumGEnts; i++ ) {
			QAA_EntityFromGuest( i );
		}
		SV_LocateGameData( qaaHostEnts, num, sizeof( sharedEntity_t ), qaaHostClients, sizeof( playerState_t ) );
		return 0;
	}
	case QG_DROP_CLIENT:
		if ( a[0] >= 0 && a[0] < sv_maxclients->integer ) {	// QAA ignores other numbers
			SV_GameDropClient( a[0], QAA_Str( a[1] ) );
		}
		return 0;
	case QG_SEND_SERVER_COMMAND:
		SV_GameSendServerCommand( a[0], QAA_Str( a[1] ) );
		return 0;
	case QG_SET_CONFIGSTRING:
		SV_SetConfigstring( a[0], QAA_Str( a[1] ) );
		return 0;
	case QG_GET_CONFIGSTRING:
		SV_GetConfigstring( a[0], static_cast<char *>( QAA_Ptr( a[1] ) ), a[2] );
		return 0;
	case QG_GET_USERINFO:
		SV_GetUserinfo( a[0], static_cast<char *>( QAA_Ptr( a[1] ) ), a[2] );
		return 0;
	case QG_SET_USERINFO:
		SV_SetUserinfo( a[0], QAA_Str( a[1] ) );
		return 0;
	case QG_GET_SERVERINFO:
		SV_GetServerinfo( static_cast<char *>( QAA_Ptr( a[0] ) ), a[1] );
		return 0;
	case QG_SET_BRUSH_MODEL: {
		const char *name = QAA_Str( a[1] );
		QAA_WithEntity( a[0], [name]( sharedEntity_t *ent ) { SV_SetBrushModel( ent, name ); } );
		return 0;
	}
	case QG_TRACE:
	case QG_TRACECAPSULE: {
		trace_t tr;
		const QAAVec3 start = vec( a[1] ), mins = vec( a[2] ), maxs = vec( a[3] ), end = vec( a[4] );
		SV_Trace( &tr, start.v, mins.p, maxs.p, end.v, a[5], a[6], call->trap == QG_TRACECAPSULE ? qtrue : qfalse );
		QAA_TraceToGuest( a[0], &tr );
		return 0;
	}
	case QG_POINT_CONTENTS:
		return SV_PointContents( vec( a[0] ).v, a[1] );
	case QG_IN_PVS:
		return SV_inPVS( vec( a[0] ).v, vec( a[1] ).v );
	case QG_IN_PVS_IGNORE_PORTALS:
		return SV_inPVSIgnorePortals( vec( a[0] ).v, vec( a[1] ).v );
	case QG_ADJUST_AREA_PORTAL_STATE: {
		sharedEntity_t *ent = QAA_HostEntity( a[0] );
		if ( ent ) {
			SV_AdjustAreaPortalState( ent, a[1] != 0 );
		}
		return 0;
	}
	case QG_AREAS_CONNECTED:
		return CM_AreasConnected( a[0], a[1] );
	case QG_ACTIVE_CLIENT_COUNT: {
		// QAA: clients whose state is above CS_ZOMBIE
		int count = 0;
		for ( int i = 0; i < sv_maxclients->integer; i++ ) {
			if ( svs.clients[i].state > CS_ZOMBIE ) {
				count++;
			}
		}
		return count;
	}
	case QG_LINKENTITY:
		QAA_WithEntity( a[0], []( sharedEntity_t *ent ) { SV_LinkEntity( ent ); } );
		return 0;
	case QG_UNLINKENTITY:
		QAA_WithEntity( a[0], []( sharedEntity_t *ent ) { SV_UnlinkEntity( ent ); } );
		return 0;
	case QG_ENTITIES_IN_BOX: {
		const int maxcount = a[3];
		if ( maxcount <= 0 ) {
			return 0;
		}
		std::vector<int> list( static_cast<size_t>( maxcount ) );
		const int count = SV_AreaEntities( vec( a[0] ).v, vec( a[1] ).v, list.data(), maxcount );
		for ( int i = 0; i < count; i++ ) {
			QAA_StoreS32( a[2] + 4 * i, list[i] );
		}
		return count;
	}
	case QG_ENTITY_CONTACT:
	case QG_ENTITY_CONTACTCAPSULE: {
		const sharedEntity_t *ent = QAA_HostEntity( a[2] );
		return ent ? SV_EntityContact( vec( a[0] ).v, vec( a[1] ).v, ent, call->trap == QG_ENTITY_CONTACTCAPSULE ) : 0;
	}
	case QG_BOT_ALLOCATE_CLIENT: {
		qaaRefreshHold++;
		const int clientNum = SV_BotAllocateClient();
		qaaRefreshHold--;
		if ( clientNum >= 0 && clientNum < qaaNumGEnts ) {
			QAA_StoreS32( qaaGEnts + static_cast<uint32_t>( clientNum ) * qaaGEntSize + qaa_layout::oES_NUMBER, clientNum );
		}
		return clientNum;
	}
	case QG_BOT_FREE_CLIENT:
		SV_BotFreeClient( a[0] );
		return 0;
	case QG_GET_USERCMD: {
		usercmd_t cmd;
		SV_GetUsercmd( a[0], &cmd );
		QAA_UsercmdToGuest( a[1], &cmd );
		return 0;
	}
	case QG_GET_ENTITY_TOKEN: {
		const char *s = COM_Parse( &sv.entityParsePoint );
		char *dst = static_cast<char *>( QAA_Ptr( a[0] ) );
		const int size = a[1] - 1;
		if ( dst && size >= 0 ) {
			Q_strncpy( dst, const_cast<char *>( s ), size );
			dst[size] = '\0';
		}
		return ( !sv.entityParsePoint && s[0] == '\0' ) ? qfalse : qtrue;
	}
	case QG_DEBUG_POLYGON_CREATE: {
		const int numPoints = a[1];
		if ( numPoints <= 0 ) {
			return 0;
		}
		std::vector<float> points( static_cast<size_t>( numPoints ) * 3 );
		for ( int i = 0; i < numPoints; i++ ) {
			QAA_LoadVec3( a[2] + 12 * i, &points[3 * i] );
		}
		return BotImport_DebugPolygonCreate( a[0], numPoints, reinterpret_cast<vec3_t *>( points.data() ) );
	}
	case QG_DEBUG_POLYGON_DELETE:
		return 0;	// QAA: a no-op
	case QG_REAL_TIME: {
		qtime_t t;
		const int r = SV_LockstepActive() ? SV_LockstepRealTime( &t ) : Com_RealTime( &t );
		const int fields[9] = { t.tm_sec, t.tm_min, t.tm_hour, t.tm_mday, t.tm_mon, t.tm_year, t.tm_wday, t.tm_yday, t.tm_isdst };
		for ( int i = 0; i < 9 && a[0]; i++ ) {
			QAA_StoreS32( a[0] + 4 * i, fields[i] );
		}
		return r;
	}
	case QG_SNAPVECTOR: {
		// QAA floors each component (floorf); Quake III rounds to nearest
		for ( int i = 0; i < 3; i++ ) {
			QAA_StoreF( a[0] + 4 * i, floorf( QAA_LoadF( a[0] + 4 * i ) ) );
		}
		return 0;
	}

	// The Scaleform menus (M6): the client's front end, where there is one
	// (docs/UI.md); a dedicated server has no menus and returns retail's
	// values.
	case QG_UI_GOTO_STACK_PATH:
	case QG_UI_INGAME_MENU:
	case QG_UI_COURSE_RESULTS:
	case QG_UI_CACHE_SCORES:
		if ( QAA_SV_GameUiTrap ) {
			return QAA_SV_GameUiTrap( call->trap, a );
		}
		return call->trap == QG_UI_INGAME_MENU ? -1 : 0;

	// Xbox Live and achievements. On the PC there is no Live session: the
	// session-info words read 0, as retail reads without a session, and
	// property writes have nowhere to go (PC deviation).
	case QG_MATCH_END:
	case QG_AWARD_ACHIEVEMENT:
	case QG_LIVE_SET_PROPERTY:
	case QG_LIVE_MEMBER_SET1:
	case QG_LIVE_MEMBER_SET0:
	case QG_MATCH_ARM_END:
	case QG_LIVE_WRITE_LEADERBOARD:
	case QG_LIVE_SET_GAMEMODE:
	case QG_LIVE_SET_PROPERTY_1:
		return 0;
	case QG_LIVE_SESSION_INFO_4:
	case QG_LIVE_SESSION_INFO_5:
	case QG_LIVE_SESSION_INFO_1:
	case QG_LIVE_SESSION_INFO_0:
	case QG_LIVE_MEMBER_GET0:
	case QG_STUB_61:
	case QG_STUB_62:
	case QG_STUB_63:
		return 0;
	case QG_SEND_SCOREBOARD: {
		const int size = a[1];
		if ( size > 0 && size <= static_cast<int>( sizeof( qaa_scoreboardMailbox ) ) ) {
			std::memcpy( qaa_scoreboardMailbox, QAA_Ptr( a[0] ), static_cast<size_t>( size ) );
			qaa_scoreboardMailboxSize = size;
			qaa_scoreboardMailboxFull = true;
		}
		return 0;
	}

	case QG_MEMSET:
		std::memset( QAA_Ptr( a[0] ), a[1], static_cast<size_t>( a[2] ) );
		return 0;
	case QG_MEMCPY:
		std::memmove( QAA_Ptr( a[0] ), QAA_Ptr( a[1] ), static_cast<size_t>( a[2] ) );
		return 0;
	case QG_STRNCPY:
		strncpy( static_cast<char *>( QAA_Ptr( a[0] ) ), QAA_Str( a[1] ), static_cast<size_t>( a[2] ) );
		return a[0];
	case QG_SIN:
		return QAA_RetF( sinf( QAA_ArgF( a[0] ) ) );
	case QG_COS:
		return QAA_RetF( cosf( QAA_ArgF( a[0] ) ) );
	case QG_ATAN2:
		return QAA_RetF( atan2f( QAA_ArgF( a[0] ), QAA_ArgF( a[1] ) ) );
	case QG_SQRT:
		return QAA_RetF( sqrtf( QAA_ArgF( a[0] ) ) );
	case QG_FLOOR:
		return QAA_RetF( floorf( QAA_ArgF( a[0] ) ) );
	case QG_CEIL:
		return QAA_RetF( ceilf( QAA_ArgF( a[0] ) ) );
	case QG_MATRIXMULTIPLY: {
		float in1[3][3], in2[3][3], out[3][3];
		for ( int i = 0; i < 9; i++ ) {
			in1[i / 3][i % 3] = QAA_LoadF( a[0] + 4 * i );
			in2[i / 3][i % 3] = QAA_LoadF( a[1] + 4 * i );
		}
		MatrixMultiply( in1, in2, out );
		for ( int i = 0; i < 9; i++ ) {
			QAA_StoreF( a[2] + 4 * i, out[i / 3][i % 3] );
		}
		return 0;
	}
	case QG_ANGLEVECTORS: {
		vec3_t f, r, u;
		AngleVectors( vec( a[0] ).v, a[1] ? f : nullptr, a[2] ? r : nullptr, a[3] ? u : nullptr );
		if ( a[1] ) QAA_StoreVec3( a[1], f );
		if ( a[2] ) QAA_StoreVec3( a[2], r );
		if ( a[3] ) QAA_StoreVec3( a[3], u );
		return 0;
	}
	case QG_PERPENDICULARVECTOR: {
		vec3_t dst;
		PerpendicularVector( dst, vec( a[1] ).v );
		QAA_StoreVec3( a[0], dst );
		return 0;
	}

	default:
		if ( call->trap >= QG_BOTLIB_FIRST && call->trap <= QG_BOTLIB_LAST ) {
			bool handled;
			const int64_t r = QAA_BotlibSyscall( call->trap, a, &handled );
			if ( handled ) {
				return r;
			}
		}
		// QAA's own default returns -1 (docs/ABI.md)
		Com_DPrintf( S_COLOR_YELLOW "qagame: unhandled trap %d (from %08X)\n", call->trap, call->return_address );
		return -1;
	}
}

/* ---- the engine function table (vmMain 9) ------------------------------- */

/* A host function the guest calls directly: its first argument is "trap". */
#define QAA_ARG0( call ) ( (call)->trap )
#define QAA_ARGN( call, n ) ( (call)->args[(n) - 1] )

static int64_t QAA_GameTableCall( qaa_syscall copy, int trap ) {
	// shift the direct call's arguments into trap form: args[0] is argument 1
	qaa_syscall t = copy;
	t.trap = trap;
	t.args[0] = copy.trap;
	for ( int i = 1; i < QAA_SYSCALL_MAX_ARGS; i++ ) {
		t.args[i] = copy.args[i - 1];
	}
	return QAA_GameSyscall( nullptr, qaaGame, &t );
}

static int64_t QAA_GT_Unlink( void *, qaa_module *, const qaa_syscall *c ) { return QAA_GameTableCall( *c, QG_UNLINKENTITY ); }
static int64_t QAA_GT_Link( void *, qaa_module *, const qaa_syscall *c ) { return QAA_GameTableCall( *c, QG_LINKENTITY ); }
static int64_t QAA_GT_Trace( void *, qaa_module *, const qaa_syscall *c ) {
	// SV_Trace( results, start, mins, maxs, end, passEntityNum, contentmask, capsule )
	return QAA_GameTableCall( *c, QAA_ARGN( c, 7 ) ? QG_TRACECAPSULE : QG_TRACE );
}
static int64_t QAA_GT_PointContents( void *, qaa_module *, const qaa_syscall *c ) { return QAA_GameTableCall( *c, QG_POINT_CONTENTS ); }
static int64_t QAA_GT_CvarUpdate( void *, qaa_module *, const qaa_syscall *c ) { return QAA_GameTableCall( *c, QG_CVAR_UPDATE ); }
static int64_t QAA_GT_CvarSet( void *, qaa_module *, const qaa_syscall *c ) { return QAA_GameTableCall( *c, QG_CVAR_SET ); }
static int64_t QAA_GT_CvarInt( void *, qaa_module *, const qaa_syscall *c ) { return QAA_GameTableCall( *c, QG_CVAR_VARIABLE_INTEGER_VALUE ); }
// retail copies these three from the botlib export table (engine_tables.tsv)
static int64_t QAA_GT_AasPointContents( void *, qaa_module *, const qaa_syscall *c ) { return QAA_GameTableCall( *c, QB_AAS_POINT_CONTENTS ); }
static int64_t QAA_GT_AasInitialized( void *, qaa_module *, const qaa_syscall *c ) { return QAA_GameTableCall( *c, QB_AAS_INITIALIZED ); }
static int64_t QAA_GT_BotUpdateEntity( void *, qaa_module *, const qaa_syscall *c ) { return QAA_GameTableCall( *c, QB_UPDATENTITY ); }

static bool QAA_SV_BuildEngineTable( void ) {
	const qaaGuestApi_t &api = QAA_Host_Api();
	static const qaa_syscall_fn entries[10] = {
		QAA_GT_Unlink, QAA_GT_Link, QAA_GT_Trace, QAA_GT_PointContents, QAA_GT_CvarUpdate,
		QAA_GT_CvarSet, QAA_GT_CvarInt, QAA_GT_AasPointContents, QAA_GT_AasInitialized, QAA_GT_BotUpdateEntity,
	};
	if ( !qaaGameEngineTable ) {
		qaaGameEngineTable = api.qaa_guest_alloc( QAA_Host_Guest(), sizeof( entries ) / sizeof( entries[0] ) * 4 );
		if ( !qaaGameEngineTable ) {
			return false;
		}
	}
	for ( size_t i = 0; i < sizeof( entries ) / sizeof( entries[0] ); i++ ) {
		const uint32_t fn = api.qaa_module_host_function( qaaGame, entries[i], nullptr );
		if ( !fn ) {
			return false;
		}
		QAA_StoreU32( qaaGameEngineTable + 4 * static_cast<uint32_t>( i ), fn );
	}
	return true;
}

/* ---- vmMain -------------------------------------------------------------- */

enum qaaGameExport_t {
	QGAME_INIT = 0, QGAME_SHUTDOWN, QGAME_CLIENT_CONNECT, QGAME_CLIENT_BEGIN, QGAME_CLIENT_USERINFO_CHANGED,
	QGAME_CLIENT_DISCONNECT, QGAME_CLIENT_COMMAND, QGAME_CLIENT_THINK, QGAME_RUN_FRAME,
	QGAME_SET_ENGINE_TABLE = 9, QGAME_CONSOLE_COMMAND = 10, QGAME_UPDATE_SCORES = 11, QBOTAI_START_FRAME = 12,
};

static int32_t QAA_GameCall( int command, int nargs, const int32_t *args ) {
	const intptr_t r = QAA_Host_VmMain( qaaGame, command, args, nargs );
	QAA_SV_RefreshAll();
	return static_cast<int32_t>( r );
}

/* VM_Call's entry point: FnQL's numbering in, QAA's out. */
static intptr_t QDECL QAA_GameEntry( int command, ... ) {
	int32_t args[3];
	va_list ap;
	va_start( ap, command );
	for ( int i = 0; i < 3; i++ ) {
		args[i] = va_arg( ap, int32_t );
	}
	va_end( ap );

	switch ( command ) {
	case GAME_INIT:
	case GAME_SHUTDOWN:
	case GAME_CLIENT_USERINFO_CHANGED:
	case GAME_CLIENT_COMMAND:
	case GAME_CLIENT_THINK:
	case GAME_RUN_FRAME:
		return QAA_GameCall( command, 3, args );
	case GAME_CLIENT_CONNECT: {
		// returns a guest string (the refusal reason) or NULL
		const int32_t r = QAA_GameCall( command, 3, args );
		return r ? reinterpret_cast<intptr_t>( QAA_Str( static_cast<uint32_t>( r ) ) ) : 0;
	}
	case GAME_CLIENT_BEGIN:
	case GAME_CLIENT_DISCONNECT: {
		const int32_t r = QAA_GameCall( command, 3, args );
		// retail sends the scores to the menus after each (0x8212EC90, 0x8212E8B8)
		QAA_GameCall( QGAME_UPDATE_SCORES, 1, args );
		return r;
	}
	case GAME_CONSOLE_COMMAND:
		return QAA_GameCall( QGAME_CONSOLE_COMMAND, 0, args );
	case BOTAI_START_FRAME:
		return QAA_GameCall( QBOTAI_START_FRAME, 1, args );
	default:
		// Quake Live's additions; FnQL only asks a module with native exports
		Com_DPrintf( "QAA: game command %d has no QAA equivalent\n", command );
		return 0;
	}
}

static void QAA_SV_ResetModuleState( void ) {
	qaaGEnts = qaaGEntSize = qaaGClients = qaaGClientSize = 0;
	qaaNumGEnts = 0;
	qaaGameEngineTable = 0;
	qaaRefreshHold = 0;
	qaa_scoreboardMailboxFull = false;
}

static void QAA_GameDestroy( vm_t * ) {
	if ( qaaGame ) {
		QAA_Host_UnloadModule( qaaGame );
		qaaGame = nullptr;
	}
	QAA_SV_ResetModuleState();
}

static vm_t *QAA_GameCreate( void );

static vm_t *QAA_GameRestart( vm_t *vm ) {
	VM_Free( vm );	// QAA_GameDestroy unloads the module, as a native DLL restarts
	return QAA_GameCreate();
}

static vm_t *QAA_GameCreate( void ) {
	QAA_SV_ResetModuleState();
	qaaGame = QAA_Host_LoadModule( "qagame" );
	if ( !qaaGame ) {
		return nullptr;
	}
	const qaaGuestApi_t &api = QAA_Host_Api();
	if ( api.qaa_module_dll_entry( qaaGame, QAA_GameSyscall, nullptr ) != QAA_OK ) {
		Com_Printf( S_COLOR_YELLOW "WARNING: qagame dllEntry failed: %s\n", api.qaa_guest_last_error( QAA_Host_Guest() ) );
		QAA_GameDestroy( nullptr );
		return nullptr;
	}
	if ( !QAA_SV_BuildEngineTable() ) {
		Com_Printf( S_COLOR_YELLOW "WARNING: could not build qagame's engine table\n" );
		QAA_GameDestroy( nullptr );
		return nullptr;
	}
	vm_t *vm = VM_CreateHosted( VM_GAME, QAA_GameEntry, QAA_GameDestroy, QAA_GameRestart );
	gvm = vm;	// the module may make engine calls from here on
	// as retail's SV_InitGameVM (0x82133858): the engine table goes in before GAME_INIT
	const int32_t tableArg[1] = { static_cast<int32_t>( qaaGameEngineTable ) };
	QAA_GameCall( QGAME_SET_ENGINE_TABLE, 1, tableArg );
	return vm;
}

/* SV_LoadGameModule: QAA's recompiled qagame when qaa_game is set and available. */
static vm_t *QAA_SV_LoadGameModule( void ) {
	if ( !qaa_game ) {
		qaa_game = Cvar_Get( "qaa_game", "1", CVAR_LATCH );
		Cvar_SetDescription( qaa_game, "Run Quake Arena Arcade's own game code, recompiled from your package "
			"(1, the default), or look for a game module as FnQL does (0)." );
	}
	if ( !qaa_game->integer ) {
		return nullptr;
	}
	return QAA_GameCreate();
}
