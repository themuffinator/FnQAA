/*
===========================================================================
openQAA: null Steam and Workshop services.

Copyright (C) 2026 openQAA contributors.
This file is part of openQAA, licensed under the GNU GPL v2 or later.
===========================================================================
*/

// QAA: PC deviation. Quake Arena Arcade has no Steam: it shipped on Xbox Live
// Arcade. openQAA does not import FnQL's Steam provider loader
// (code/platform/fnql_steam.cpp) or its Workshop adapter (fnql_workshop.cpp).
// This file implements the same interface (fnql_steam.h, Com_Workshop* in
// qcommon.h) with every service reported unavailable, which is the state FnQL
// already handles when no provider is present. Xbox Live replacements (M7, M8)
// belong behind their own interfaces, not here.

#include "../platform/fnql_steam.h"
#include "../qcommon/qcommon.h"

#include <cstring>

namespace {
constexpr const char *kNoSteam = "openQAA has no Steam integration.";
}

extern "C" {

void FNQL_Steam_Init( uint32_t /*roles*/ ) {
}

void FNQL_Steam_Reconfigure( uint32_t /*roles*/ ) {
}

void FNQL_Steam_Shutdown( void ) {
}

void FNQL_Steam_Pump( void ) {
}

void FNQL_Steam_SetEventSink( fnqlSteamEngineEventFn /*sink*/, void * /*context*/ ) {
}

qboolean FNQL_Steam_AddEventSink( fnqlSteamEngineEventFn /*sink*/, void * /*context*/ ) {
	return qfalse;
}

void FNQL_Steam_RemoveEventSink( fnqlSteamEngineEventFn /*sink*/, void * /*context*/ ) {
}

qboolean FNQL_Steam_Available( uint64_t /*capability*/ ) {
	return qfalse;
}

uint64_t FNQL_Steam_Capabilities( void ) {
	return 0;
}

qboolean FNQL_Steam_GetStatus( fnqlSteamStatus_t *status ) {
	if ( status && status->size >= sizeof( *status ) ) {
		memset( status, 0, sizeof( *status ) );
		status->size = sizeof( *status );
		Q_strncpyz( status->detail, kNoSteam, sizeof( status->detail ) );
	}
	return qfalse;
}

const char *FNQL_Steam_StatusDetail( void ) {
	return kNoSteam;
}

qboolean FNQL_Steam_IsSubscribedApp( uint32_t /*appId*/ ) {
	return qfalse;
}

fnqlSteamResult_t FNQL_Steam_OpenOverlayUrl( const char * /*url*/ ) {
	return FNQL_STEAM_RESULT_UNAVAILABLE;
}

fnqlSteamResult_t FNQL_Steam_OpenOverlayUser( const char * /*dialog*/, uint64_t /*steamId*/ ) {
	return FNQL_STEAM_RESULT_UNAVAILABLE;
}

fnqlSteamResult_t FNQL_Steam_SetRichPresence( const char * /*key*/, const char * /*value*/ ) {
	return FNQL_STEAM_RESULT_UNAVAILABLE;
}

fnqlSteamResult_t FNQL_Steam_RequestServers( uint32_t /*requestMode*/, uint32_t /*appId*/ ) {
	return FNQL_STEAM_RESULT_UNAVAILABLE;
}

fnqlSteamResult_t FNQL_Steam_RefreshServers( void ) {
	return FNQL_STEAM_RESULT_UNAVAILABLE;
}

void FNQL_Steam_CancelServers( void ) {
}

fnqlSteamResult_t FNQL_Steam_CreateLobby( uint32_t /*lobbyType*/, uint32_t /*maxMembers*/ ) {
	return FNQL_STEAM_RESULT_UNAVAILABLE;
}

fnqlSteamResult_t FNQL_Steam_JoinLobby( uint64_t /*lobbyId*/ ) {
	return FNQL_STEAM_RESULT_UNAVAILABLE;
}

fnqlSteamResult_t FNQL_Steam_LeaveLobby( uint64_t /*lobbyId*/ ) {
	return FNQL_STEAM_RESULT_UNAVAILABLE;
}

fnqlSteamResult_t FNQL_Steam_SetLobbyServer( uint64_t /*lobbyId*/, uint32_t /*ip*/, uint16_t /*port*/, uint64_t /*serverId*/ ) {
	return FNQL_STEAM_RESULT_UNAVAILABLE;
}

fnqlSteamResult_t FNQL_Steam_InviteToLobby( uint64_t /*lobbyId*/, uint64_t /*steamId*/ ) {
	return FNQL_STEAM_RESULT_UNAVAILABLE;
}

fnqlSteamResult_t FNQL_Steam_SendLobbyChat( uint64_t /*lobbyId*/, const void * /*bytes*/, uint32_t /*byteCount*/ ) {
	return FNQL_STEAM_RESULT_UNAVAILABLE;
}

fnqlSteamResult_t FNQL_Steam_GetSubscribedItems( uint64_t * /*itemIds*/, uint32_t /*capacity*/, uint32_t * /*count*/ ) {
	return FNQL_STEAM_RESULT_UNAVAILABLE;
}

fnqlSteamResult_t FNQL_Steam_GetItemInstallInfo( uint64_t /*itemId*/, char * /*folder*/, uint32_t /*folderCapacity*/, uint64_t * /*sizeOnDisk*/, uint32_t * /*timestamp*/ ) {
	return FNQL_STEAM_RESULT_UNAVAILABLE;
}

fnqlSteamResult_t FNQL_Steam_GetItemDownloadInfo( uint64_t /*itemId*/, uint64_t * /*downloaded*/, uint64_t * /*total*/ ) {
	return FNQL_STEAM_RESULT_UNAVAILABLE;
}

fnqlSteamResult_t FNQL_Steam_GetItemState( uint64_t /*itemId*/, uint32_t * /*stateFlags*/ ) {
	return FNQL_STEAM_RESULT_UNAVAILABLE;
}

fnqlSteamResult_t FNQL_Steam_GetAvatarRGBA( uint64_t /*steamId*/, uint32_t /*avatarSize*/, void * /*rgba*/, uint32_t /*rgbaCapacity*/, uint32_t * /*width*/, uint32_t * /*height*/, uint32_t * /*requiredSize*/ ) {
	return FNQL_STEAM_RESULT_UNAVAILABLE;
}

fnqlSteamResult_t FNQL_Steam_RequestAvatarRGBA( uint64_t /*steamId*/, uint32_t /*avatarSize*/ ) {
	return FNQL_STEAM_RESULT_UNAVAILABLE;
}

qboolean FNQL_Steam_TakeAvatarRGBA( fnqlSteamAvatarImage_t *image ) {
	if ( image ) {
		memset( image, 0, sizeof( *image ) );
	}
	return qfalse;
}

fnqlSteamResult_t FNQL_Steam_GetFriends( uint32_t /*friendFlags*/, fnqlSteamFriend_t * /*friends*/, uint32_t /*capacity*/, uint32_t * /*count*/ ) {
	return FNQL_STEAM_RESULT_UNAVAILABLE;
}

fnqlSteamResult_t FNQL_Steam_GetFriend( uint64_t /*steamId*/, fnqlSteamFriend_t * /*friendInfo*/ ) {
	return FNQL_STEAM_RESULT_UNAVAILABLE;
}

fnqlSteamResult_t FNQL_Steam_RequestUgcQuery( uint32_t /*rawFilter*/ ) {
	return FNQL_STEAM_RESULT_UNAVAILABLE;
}

fnqlSteamResult_t FNQL_Steam_GetUgcQueryResults( fnqlSteamUgcItem_t * /*items*/, uint32_t /*capacity*/, uint32_t * /*count*/, uint32_t * /*totalMatching*/ ) {
	return FNQL_STEAM_RESULT_UNAVAILABLE;
}

fnqlSteamResult_t FNQL_Steam_SendP2PPacket( uint32_t /*role*/, uint64_t /*remoteId*/, const void * /*data*/, uint32_t /*dataSize*/, uint32_t /*sendType*/, int32_t /*channel*/ ) {
	return FNQL_STEAM_RESULT_UNAVAILABLE;
}

fnqlSteamResult_t FNQL_Steam_PeekP2PPacket( uint32_t /*role*/, int32_t /*channel*/, uint32_t * /*packetSize*/ ) {
	return FNQL_STEAM_RESULT_UNAVAILABLE;
}

fnqlSteamResult_t FNQL_Steam_ReadP2PPacket( uint32_t /*role*/, int32_t /*channel*/, void * /*data*/, uint32_t /*capacity*/, uint32_t * /*dataSize*/, uint64_t * /*remoteId*/ ) {
	return FNQL_STEAM_RESULT_UNAVAILABLE;
}

fnqlSteamResult_t FNQL_Steam_AcceptP2PSession( uint32_t /*role*/, uint64_t /*remoteId*/ ) {
	return FNQL_STEAM_RESULT_UNAVAILABLE;
}

fnqlSteamResult_t FNQL_Steam_CloseP2PSession( uint32_t /*role*/, uint64_t /*remoteId*/ ) {
	return FNQL_STEAM_RESULT_UNAVAILABLE;
}

fnqlSteamResult_t FNQL_Steam_StartVoiceRecording( void ) {
	return FNQL_STEAM_RESULT_UNAVAILABLE;
}

void FNQL_Steam_StopVoiceRecording( void ) {
}

fnqlSteamResult_t FNQL_Steam_GetCompressedVoice( void * /*data*/, uint32_t /*capacity*/, uint32_t * /*dataSize*/ ) {
	return FNQL_STEAM_RESULT_UNAVAILABLE;
}

fnqlSteamResult_t FNQL_Steam_DecompressVoice( const void * /*compressed*/, uint32_t /*compressedSize*/, void * /*pcm*/, uint32_t /*pcmCapacity*/, uint32_t * /*pcmSize*/, uint32_t /*sampleRate*/ ) {
	return FNQL_STEAM_RESULT_UNAVAILABLE;
}

uint32_t FNQL_Steam_GetVoiceSampleRate( void ) {
	return 0;
}

fnqlSteamResult_t FNQL_Steam_GetGameServerSteamId( uint64_t * /*steamId*/ ) {
	return FNQL_STEAM_RESULT_UNAVAILABLE;
}

fnqlSteamResult_t FNQL_Steam_RequestClientUserStats( uint64_t /*steamId*/ ) {
	return FNQL_STEAM_RESULT_UNAVAILABLE;
}

fnqlSteamResult_t FNQL_Steam_GetClientUserStatI32( uint64_t /*steamId*/, const char * /*name*/, int32_t * /*value*/ ) {
	return FNQL_STEAM_RESULT_UNAVAILABLE;
}

fnqlSteamResult_t FNQL_Steam_GetClientUserStatF32( uint64_t /*steamId*/, const char * /*name*/, float * /*value*/ ) {
	return FNQL_STEAM_RESULT_UNAVAILABLE;
}

fnqlSteamResult_t FNQL_Steam_GetClientUserAchievement( uint64_t /*steamId*/, const char * /*name*/, qboolean * /*unlocked*/, uint32_t * /*unlockTime*/ ) {
	return FNQL_STEAM_RESULT_UNAVAILABLE;
}

fnqlSteamResult_t FNQL_Steam_GetAchievementDisplayAttribute( const char * /*name*/, const char * /*key*/, char * /*value*/, uint32_t /*valueCapacity*/ ) {
	return FNQL_STEAM_RESULT_UNAVAILABLE;
}

fnqlSteamResult_t FNQL_Steam_SetClientAchievement( const char * /*name*/ ) {
	return FNQL_STEAM_RESULT_UNAVAILABLE;
}

fnqlSteamResult_t FNQL_Steam_StoreClientStats( void ) {
	return FNQL_STEAM_RESULT_UNAVAILABLE;
}

fnqlSteamResult_t FNQL_Steam_ResetClientStats( qboolean /*includeAchievements*/ ) {
	return FNQL_STEAM_RESULT_UNAVAILABLE;
}

fnqlSteamResult_t FNQL_Steam_GetLobbyMembers( uint64_t /*lobbyId*/, fnqlSteamLobbyMember_t * /*members*/, uint32_t /*capacity*/, uint32_t * /*count*/, uint64_t * /*ownerId*/, uint32_t * /*memberLimit*/ ) {
	return FNQL_STEAM_RESULT_UNAVAILABLE;
}

fnqlSteamResult_t FNQL_Steam_GetLobbyData( uint64_t /*lobbyId*/, fnqlSteamLobbyData_t * /*items*/, uint32_t /*capacity*/, uint32_t * /*count*/ ) {
	return FNQL_STEAM_RESULT_UNAVAILABLE;
}

fnqlSteamResult_t FNQL_Steam_SetLobbyData( uint64_t /*lobbyId*/, const char * /*key*/, const char * /*value*/ ) {
	return FNQL_STEAM_RESULT_UNAVAILABLE;
}

fnqlSteamResult_t FNQL_Steam_InviteToGame( uint64_t /*steamId*/, const char * /*connectString*/ ) {
	return FNQL_STEAM_RESULT_UNAVAILABLE;
}

fnqlSteamResult_t FNQL_Steam_HandleGameServerPacket( const void * /*data*/, uint32_t /*dataSize*/, uint32_t /*sourceIp*/, uint16_t /*sourcePort*/ ) {
	return FNQL_STEAM_RESULT_UNAVAILABLE;
}

fnqlSteamResult_t FNQL_Steam_GetGameServerPacket( void * /*data*/, uint32_t /*capacity*/, uint32_t * /*dataSize*/, uint32_t * /*destinationIp*/, uint16_t * /*destinationPort*/ ) {
	return FNQL_STEAM_RESULT_UNAVAILABLE;
}

fnqlSteamResult_t FNQL_Steam_SetGameServerKeyValue( const char * /*key*/, const char * /*value*/ ) {
	return FNQL_STEAM_RESULT_UNAVAILABLE;
}

fnqlSteamResult_t FNQL_Steam_UpdateGameServerUser( uint64_t /*steamId*/, const char * /*playerName*/, uint32_t /*score*/ ) {
	return FNQL_STEAM_RESULT_UNAVAILABLE;
}

fnqlSteamResult_t FNQL_Steam_GetGameServerPublicIp( uint32_t * /*publicIp*/ ) {
	return FNQL_STEAM_RESULT_UNAVAILABLE;
}

fnqlSteamResult_t FNQL_Steam_CreateUnauthenticatedUser( uint64_t * /*steamId*/ ) {
	return FNQL_STEAM_RESULT_UNAVAILABLE;
}

fnqlSteamResult_t FNQL_Steam_SetFavoriteServer( uint32_t /*appId*/, uint32_t /*ip*/, uint16_t /*connectionPort*/, uint16_t /*queryPort*/, qboolean /*add*/ ) {
	return FNQL_STEAM_RESULT_UNAVAILABLE;
}

fnqlSteamResult_t FNQL_Steam_RequestServerDetails( uint32_t /*ip*/, uint16_t /*queryPort*/ ) {
	return FNQL_STEAM_RESULT_UNAVAILABLE;
}

void FNQL_Steam_CancelServerDetails( void ) {
}

fnqlSteamResult_t FNQL_Steam_SetGameServerBotCount( uint32_t /*botCount*/ ) {
	return FNQL_STEAM_RESULT_UNAVAILABLE;
}

fnqlSteamResult_t FNQL_Steam_SetLocalVoiceSpeaking( qboolean /*speaking*/ ) {
	return FNQL_STEAM_RESULT_UNAVAILABLE;
}

fnqlSteamResult_t FNQL_Steam_DownloadItem( uint64_t /*itemId*/, qboolean /*highPriority*/ ) {
	return FNQL_STEAM_RESULT_UNAVAILABLE;
}

fnqlSteamResult_t FNQL_Steam_SubscribeItem( uint64_t /*itemId*/ ) {
	return FNQL_STEAM_RESULT_UNAVAILABLE;
}

fnqlSteamResult_t FNQL_Steam_UnsubscribeItem( uint64_t /*itemId*/ ) {
	return FNQL_STEAM_RESULT_UNAVAILABLE;
}

fnqlSteamResult_t FNQL_Steam_RequestUserStats( uint64_t /*steamId*/ ) {
	return FNQL_STEAM_RESULT_UNAVAILABLE;
}

fnqlSteamResult_t FNQL_Steam_GetUserStatI32( uint64_t /*steamId*/, const char * /*name*/, int32_t * /*value*/ ) {
	return FNQL_STEAM_RESULT_UNAVAILABLE;
}

fnqlSteamResult_t FNQL_Steam_SetUserStatI32( uint64_t /*steamId*/, const char * /*name*/, int32_t /*value*/ ) {
	return FNQL_STEAM_RESULT_UNAVAILABLE;
}

fnqlSteamResult_t FNQL_Steam_GetUserAchievement( uint64_t /*steamId*/, const char * /*name*/, qboolean * /*unlocked*/ ) {
	return FNQL_STEAM_RESULT_UNAVAILABLE;
}

fnqlSteamResult_t FNQL_Steam_SetUserAchievement( uint64_t /*steamId*/, const char * /*name*/ ) {
	return FNQL_STEAM_RESULT_UNAVAILABLE;
}

fnqlSteamResult_t FNQL_Steam_StoreUserStats( uint64_t /*steamId*/ ) {
	return FNQL_STEAM_RESULT_UNAVAILABLE;
}

fnqlSteamResult_t FNQL_Steam_SerializeRetailJson( const void * /*retailValue*/, char * /*json*/, uint32_t /*jsonCapacity*/, uint32_t * /*jsonSize*/ ) {
	return FNQL_STEAM_RESULT_UNAVAILABLE;
}

fnqlSteamResult_t FNQL_Steam_GetAuthTicket( void * /*ticket*/, uint32_t /*ticketCapacity*/, uint32_t * /*ticketSize*/, uint32_t * /*ticketHandle*/ ) {
	return FNQL_STEAM_RESULT_UNAVAILABLE;
}

void FNQL_Steam_CancelAuthTicket( uint32_t /*ticketHandle*/ ) {
}

fnqlSteamResult_t FNQL_Steam_BeginAuthSession( const void * /*ticket*/, uint32_t /*ticketSize*/, uint64_t /*steamId*/ ) {
	return FNQL_STEAM_RESULT_UNAVAILABLE;
}

void FNQL_Steam_EndAuthSession( uint64_t /*steamId*/ ) {
}

fnqlSteamResult_t FNQL_Steam_StartGameServer( const fnqlSteamGameServerConfig_t * /*config*/ ) {
	return FNQL_STEAM_RESULT_UNAVAILABLE;
}

void FNQL_Steam_StopGameServer( void ) {
}

fnqlSteamResult_t FNQL_Steam_UpdateGameServer( const fnqlSteamGameServerConfig_t * /*config*/ ) {
	return FNQL_STEAM_RESULT_UNAVAILABLE;
}


void Com_WorkshopInit( void ) {
}

void Com_WorkshopProviderChanged( void ) {
}

void Com_WorkshopShutdown( void ) {
}

void Com_WorkshopFrame( void ) {
}

void Com_WorkshopClaimConnectionDownloads( void ) {
}

qboolean Com_WorkshopDownloadItem( uint64_t /*itemId*/ ) {
	return qfalse;
}

qboolean Com_WorkshopSubscribeItem( uint64_t /*itemId*/ ) {
	return qfalse;
}

qboolean Com_WorkshopUnsubscribeItem( uint64_t /*itemId*/ ) {
	return qfalse;
}

} // extern "C"
