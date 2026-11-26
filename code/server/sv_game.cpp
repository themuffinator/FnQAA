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
// sv_game.cpp -- interface to the game dll

#include "server.h"
#include "server_cvar_compat.hpp"

#include "../botlib/botlib.h"
#include "../botlib/be_aas.h"

#if defined(_WIN32)
#include <windows.h>
#endif

#include <array>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <limits>
#include <string>
#include <type_traits>

botlib_export_t	*botlib_export;

namespace {

std::string serverEntityOverride;

std::string CurrentEntityMapName()
{
	std::array<char, MAX_QPATH> mapName{};
	const char *configured = sv_mapname ? sv_mapname->string
		: Cvar_VariableString( "mapname" );
	const char *baseName = configured;
	for ( const char *cursor = configured; cursor && *cursor; ++cursor ) {
		if ( *cursor == '/' || *cursor == '\\' ) baseName = cursor + 1;
	}
	Q_strncpyz( mapName.data(), baseName,
		static_cast<int>( mapName.size() ) );
	COM_StripExtension( mapName.data(), mapName.data(),
		static_cast<int>( mapName.size() ) );
	return mapName.data();
}

const char *SelectServerEntityString()
{
	serverEntityOverride.clear();
	const char *selected = CM_EntityString();
	if ( !selected ) selected = "";
	const std::string mapName = CurrentEntityMapName();

	if ( sv_altEntDir && sv_altEntDir->string[0] ) {
		const auto overridePath = fnql::server::cvars::BuildEntityFilePath(
			sv_altEntDir->string, mapName, MAX_QPATH );
		if ( !overridePath ) {
			Com_Printf( S_COLOR_YELLOW
				"Rejected unsafe or overlong sv_altEntDir entity path for map %s.\n",
				mapName.c_str() );
		}
		else {
			void *fileData = nullptr;
			const int fileLength = FS_ReadFile( overridePath->c_str(), &fileData );
			if ( fileLength >= 0 && fileData ) {
				const char *bytes = static_cast<const char *>( fileData );
				if ( std::memchr( bytes, '\0', static_cast<std::size_t>( fileLength ) ) ) {
					Com_Printf( S_COLOR_YELLOW
						"Ignored %s because its entity text contains an embedded NUL.\n",
						overridePath->c_str() );
				}
				else {
					serverEntityOverride.assign( bytes,
						static_cast<std::size_t>( fileLength ) );
					selected = serverEntityOverride.c_str();
					Com_Printf( "Loaded entity override %s (%d bytes).\n",
						overridePath->c_str(), fileLength );
				}
				FS_FreeFile( fileData );
			}
			else {
				Com_DPrintf( "No entity override found at %s; using BSP entities.\n",
					overridePath->c_str() );
			}
		}
	}

	if ( sv_dumpEntities && sv_dumpEntities->integer ) {
		const auto dumpPath = fnql::server::cvars::BuildEntityFilePath(
			"ents", mapName, MAX_QPATH );
		if ( dumpPath ) {
			const std::size_t length = std::strlen( selected );
			if ( length <= static_cast<std::size_t>(
				( std::numeric_limits<int>::max )() ) ) {
				FS_WriteFile( dumpPath->c_str(), selected, static_cast<int>( length ) );
				Com_Printf( "Dumped server entities to %s (%zu bytes).\n",
					dumpPath->c_str(), length );
			}
		}
	}

	return selected;
}

} // namespace

// QAA: QAA's recompiled qagame (sv_qaa_game.inc, included at the end)
static void QAA_SV_RefreshEntity( int num );
static void QAA_SV_RefreshClient( int num );
static vm_t *QAA_SV_LoadGameModule( void );

// these functions must be used instead of pointer arithmetic, because
// the game allocates gentities with private information after the server shared part
int	SV_NumForGentity( sharedEntity_t *ent ) {
	int		num;

	num = ( reinterpret_cast<byte *>( ent ) - reinterpret_cast<byte *>( sv.gentities ) ) / sv.gentitySize;

	return num;
}


sharedEntity_t *SV_GentityNum( int num ) {
	sharedEntity_t *ent;

	QAA_SV_RefreshEntity( num );	// QAA: the host copy follows the guest entity
	ent = reinterpret_cast<sharedEntity_t *>( reinterpret_cast<byte *>( sv.gentities ) + sv.gentitySize * num );

	return ent;
}


playerState_t *SV_GameClientNum( int num ) {
	playerState_t	*ps;

	QAA_SV_RefreshClient( num );	// QAA: the host copy follows the guest player state
	ps = reinterpret_cast<playerState_t *>( reinterpret_cast<byte *>( sv.gameClients ) + sv.gameClientSize * num );

	return ps;
}


svEntity_t	*SV_SvEntityForGentity( sharedEntity_t *gEnt ) {
	if ( !gEnt || gEnt->s.number < 0 || gEnt->s.number >= MAX_GENTITIES ) {
		Com_Error( ERR_DROP, "SV_SvEntityForGentity: bad gEnt" );
	}
	return &sv.svEntities[ gEnt->s.number ];
}


sharedEntity_t *SV_GEntityForSvEntity( svEntity_t *svEnt ) {
	return SV_GentityNum( SV_SvEntityIndex( svEnt ) );
}


/*
===============
SV_GameSendServerCommand

Sends a command string to a client
===============
*/
static void SV_GameSendServerCommand( int clientNum, const char *text ) {
	SV_LockstepRecordServerCommand( clientNum, text );	// no-op unless sv_lockstep records

	if ( clientNum == -1 ) {
		SV_SendServerCommand( nullptr, "%s", text );
	} else {
		if ( clientNum < 0 || clientNum >= sv.maxclients ) {
			return;
		}
		SV_SendServerCommand( svs.clients + clientNum, "%s", text );
	}
}


/*
===============
SV_GameDropClient

Disconnects the client with a message
===============
*/
static void SV_GameDropClient( int clientNum, const char *reason ) {
	if ( clientNum < 0 || clientNum >= sv.maxclients ) {
		return;
	}
	SV_DropClient( svs.clients + clientNum, reason );
}


/*
=================
SV_SetBrushModel

sets mins and maxs for inline bmodels
=================
*/
static void SV_SetBrushModel( sharedEntity_t *ent, const char *name ) {
	clipHandle_t	h;
	vec3_t			mins, maxs;

	if ( !name ) {
		Com_Error( ERR_DROP, "SV_SetBrushModel: NULL" );
	}

	if ( name[0] != '*' ) {
		Com_Error( ERR_DROP, "SV_SetBrushModel: %s isn't a brush model", name );
	}

	ent->s.modelindex = SV_ParseInt( name + 1 );

	h = CM_InlineModel( ent->s.modelindex );
	CM_ModelBounds( h, mins, maxs );
	VectorCopy (mins, ent->r.mins);
	VectorCopy (maxs, ent->r.maxs);
	ent->r.bmodel = qtrue;

	ent->r.contents = -1;		// we don't know exactly what is in the brushes

	SV_LinkEntity( ent );		// FIXME: remove
}


/*
=================
SV_inPVS

Also checks portalareas so that doors block sight
=================
*/
qboolean SV_inPVS( const vec3_t p1, const vec3_t p2 )
{
	int		leafnum;
	int		cluster;
	int		area1, area2;
	byte	*mask;

	leafnum = CM_PointLeafnum (p1);
	cluster = CM_LeafCluster (leafnum);
	area1 = CM_LeafArea (leafnum);
	mask = CM_ClusterPVS (cluster);

	leafnum = CM_PointLeafnum (p2);
	cluster = CM_LeafCluster (leafnum);
	area2 = CM_LeafArea (leafnum);
	if ( mask && (!(mask[cluster>>3] & (1<<(cluster&7)) ) ) )
		return qfalse;
	if (!CM_AreasConnected (area1, area2))
		return qfalse;		// a door blocks sight
	return qtrue;
}


/*
=================
SV_inPVSIgnorePortals

Does NOT check portalareas
=================
*/
static bool SV_inPVSIgnorePortals( const vec3_t p1, const vec3_t p2 )
{
	int		leafnum;
	int		cluster;
	byte	*mask;

	leafnum = CM_PointLeafnum (p1);
	cluster = CM_LeafCluster (leafnum);
	mask = CM_ClusterPVS (cluster);

	leafnum = CM_PointLeafnum (p2);
	cluster = CM_LeafCluster (leafnum);

	if ( mask && (!(mask[cluster>>3] & (1<<(cluster&7)) ) ) )
		return false;

	return true;
}


/*
========================
SV_AdjustAreaPortalState
========================
*/
static void SV_AdjustAreaPortalState( sharedEntity_t *ent, bool open ) {
	svEntity_t	*svEnt;

	svEnt = SV_SvEntityForGentity( ent );
	if ( svEnt->areanum2 == -1 ) {
		return;
	}
	CM_AdjustAreaPortalState( svEnt->areanum, svEnt->areanum2, SV_QBool( open ) );
}


/*
==================
SV_EntityContact
==================
*/
static bool SV_EntityContact( const vec3_t mins, const vec3_t maxs, const sharedEntity_t *gEnt, bool capsule ) {
	const float	*origin, *angles;
	clipHandle_t	ch;
	trace_t			trace;

	// check for exact collision
	origin = gEnt->r.currentOrigin;
	angles = gEnt->r.currentAngles;

	ch = SV_ClipHandleForEntity( gEnt, SV_QBool( capsule ) );
	CM_TransformedBoxTrace( &trace, vec3_origin, vec3_origin, mins, maxs, ch, -1, origin, angles, SV_QBool( capsule ) );

	return trace.startsolid;
}


/*
===============
SV_GetServerinfo
===============
*/
static void SV_GetServerinfo( char *buffer, int bufferSize ) {

	if ( bufferSize < 1 ) {
		Com_Error( ERR_DROP, "SV_GetServerinfo: bufferSize == %i", bufferSize );
	}
	if ( sv.state != SS_GAME || !sv.configstrings[ CS_SERVERINFO ] ) {
		Q_strncpyz( buffer, Cvar_InfoString( CVAR_SERVERINFO, nullptr ), bufferSize );
	} else {
		Q_strncpyz( buffer, sv.configstrings[ CS_SERVERINFO ], bufferSize );
	}
}


/*
===============
SV_LocateGameData

===============
*/
static void SV_LocateGameData( sharedEntity_t *gEnts, int numGEntities, int sizeofGEntity_t, playerState_t *clients, int sizeofGameClient ) {

	sv.gentities = gEnts;
	sv.gentitySize = sizeofGEntity_t;
	sv.num_entities = numGEntities;

	sv.gameClients = clients;
	sv.gameClientSize = sizeofGameClient;
}


/*
===============
SV_GetUsercmd
===============
*/
static void SV_GetUsercmd( int clientNum, usercmd_t *cmd ) {
	if ( SV_IsClientIndex( clientNum ) ) {
		*cmd = SV_ClientForIndex( clientNum ).lastUsercmd;
	} else {
		Com_Error( ERR_DROP, "%s(): bad clientNum: %i", __func__, clientNum );
	}
}


//==============================================

/*
====================
GVM_ArgPtr

A pointer the hosted game returned through VM_Call: QAA's recompiled qagame
(sv_qaa_game.inc) hands back host pointers, so no translation is needed.
====================
*/
void *GVM_ArgPtr( intptr_t intValue )
{
	return reinterpret_cast<void *>( intValue );
}


/*
===============
SV_ShutdownGameProgs

Called every time a map changes
===============
*/
void SV_ShutdownGameProgs( void ) {
	if ( !gvm ) {
		serverEntityOverride.clear();
		return;
	}
	VM_Call( gvm, 1, GAME_SHUTDOWN, qfalse );
	VM_Free( gvm );
	gvm = nullptr;
	serverEntityOverride.clear();
	FS_VM_CloseFiles( H_QAGAME );
}


/*
==================
SV_InitGameVM

Called for both a full init and a restart
==================
*/
static void SV_InitGameVM( bool restart ) {
	// start the entity parsing at the beginning
	sv.entityParsePoint = SelectServerEntityString();

	// clear all gentity pointers that might still be set from
	// a previous level
	// https://zerowing.idsoftware.com/bugzilla/show_bug.cgi?id=522
	// now done before GAME_INIT call
	for ( client_t &client : SV_Clients() ) {
		client.gentity = nullptr;
	}

	// use the current msec count for a random seed
	// init for this gamestate (sv_lockstep derives it from sv_lockstepSeed)
	const int randomSeed = SV_LockstepActive() ? SV_LockstepGameSeed() : Com_Milliseconds();
	VM_Call( gvm, 3, GAME_INIT, sv.time, randomSeed, SV_QBool( restart ) );
}


/*
===================
SV_RestartGameProgs

Called on a map_restart, but not on a normal map change
===================
*/
void SV_RestartGameProgs( void ) {
	if ( !gvm ) {
		return;
	}
	VM_Call( gvm, 1, GAME_SHUTDOWN, qtrue );

	// do a restart instead of a free
	gvm = VM_Restart( gvm );
	if ( !gvm ) {
		Com_Error( ERR_DROP, "VM_Restart on game failed" );
	}

	SV_InitGameVM( true );

	// load userinfo filters
	SV_LoadFilters( sv_filter->string );
}


/*
===============
SV_InitGameProgs

Called on a normal map change, not on a map_restart
===============
*/
void SV_InitGameProgs( void ) {
	cvar_t	*var;
	//FIXME these are temp while I make bots run in vm
	extern int	bot_enable;

	var = Cvar_Get( "bot_enable", "1", CVAR_LATCH );
	if ( var ) {
		bot_enable = var->integer;
	}
	else {
		bot_enable = 0;
	}

	// QAA: Quake Arena Arcade's own qagame, recompiled from the user's
	// package (sv_qaa_game.inc); there is no other game module
	gvm = QAA_SV_LoadGameModule();
	if ( !gvm ) {
		Com_Error( ERR_DROP, "VM_Create on game failed" );
	}

	SV_InitGameVM( false );

	// load userinfo filters
	SV_LoadFilters( sv_filter->string );
}


/*
====================
SV_GameCommand

See if the current console command is claimed by the game
====================
*/
qboolean SV_GameCommand( void ) {
	if ( sv.state != SS_GAME ) {
		return qfalse;
	}

	return SV_QBool( VM_Call( gvm, 0, GAME_CONSOLE_COMMAND ) != 0 );
}


// QAA: hosting QAA's recompiled qagame
#include "sv_qaa_game.inc"
