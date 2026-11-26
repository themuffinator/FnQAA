/*
===========================================================================
FnQAA: null web front end.

Copyright (C) 2026 FnQAA contributors.
This file is part of FnQAA, licensed under the GNU GPL v2 or later.
===========================================================================
*/

// QAA: PC deviation. Quake Arena Arcade's menus are Scaleform GFx movies, not
// Quake Live's Awesomium web front end, so FnQAA does not import FnQL's WebUI
// (cl_webui.cpp, cl_webpak.cpp, awesomium_backend_win32.cpp). This file keeps
// the client's interface to it (client.h) with no web view: nothing is shown,
// no input is captured and every request fails. QAA has no Steam, so FnQL's
// Steam helpers from cl_webui.cpp are removed, not stubbed. QAA's menus are M6.

#include "../client/client.h"

static void ClearText( char *buffer, int bufferSize ) {
	if ( buffer && bufferSize > 0 ) {
		buffer[0] = '\0';
	}
}

extern "C" {

void CL_WebPak_Init( void ) {
}

void CL_WebPak_Shutdown( void ) {
}

qboolean CL_WebPak_Available( void ) {
	return qfalse;
}

qboolean CL_WebPak_Fetch( const char * /*virtualPath*/, void ** /*outBuffer*/, int * /*outLength*/ ) {
	return qfalse;
}

int CL_WebPak_GetFileList( const char * /*path*/, const char * /*extension*/, char * /*listbuf*/, int /*bufsize*/ ) {
	return 0;
}

qboolean CL_WebRequestResolve( const char * /*virtualPath*/, void ** /*outBuffer*/, int * /*outLength*/ ) {
	return qfalse;
}

qboolean CL_LauncherRequestData( const char * /*virtualPath*/, void ** /*outBuffer*/, int * /*outLength*/ ) {
	return qfalse;
}

void QLWebHost_RegisterCommands( void ) {
}

void QLWebHost_UnregisterCommands( void ) {
}

void CL_WebHost_Init( void ) {
}

void CL_WebHost_Shutdown( void ) {
}

void CL_WebHost_Frame( void ) {
}

void CL_WebHost_RefreshSurfaceSize( void ) {
}

void CL_WebHost_InvalidateFactoryCatalog( void ) {
}

void CL_WebHost_InvalidateMapCatalog( void ) {
}

void CL_WebHost_BootstrapAwesomiumMenu( void ) {
}

qboolean CL_WebHost_HasLiveView( void ) {
	return qfalse;
}

qboolean CL_WebHost_HasBoundWindowObject( void ) {
	return qfalse;
}

qboolean CL_WebHost_HasDrawableSurface( void ) {
	return qfalse;
}

void CL_WebHost_DrawBrowserSurface( void ) {
}

void *CL_WebHost_GetCursorHandle( void ) {
	return nullptr;
}

void *CL_WebHost_OnChangeCursor( int /*cursorType*/ ) {
	return nullptr;
}

void CL_WebHost_OnChangeTooltip( const char * /*tooltip*/ ) {
}

void CL_WebHost_SetCursorPosition( int /*x*/, int /*y*/ ) {
}

qboolean CL_WebHost_GetCursorPosition( int * /*x*/, int * /*y*/ ) {
	return qfalse;
}

qboolean CL_WebHost_RequestCursorPosition( int * /*x*/, int * /*y*/ ) {
	return qfalse;
}

qboolean CL_WebHost_ShowAfterDisconnect( void ) {
	return qfalse;
}

void CL_WebHost_HideBrowser( void ) {
}

void CL_WebHost_HideForGameTransition( void ) {
}

qboolean CL_WebHost_KeyCapturePending( void ) {
	return qfalse;
}

void CL_WebHost_NotifySoundsStopped( void ) {
}

void CL_WebHost_NotifyAppActivation( qboolean /*active*/ ) {
}

void CL_WebHost_InvalidateConfigSnapshot( void ) {
}

void CL_WebHost_InvalidateFriendSnapshot( void ) {
}

void CL_WebHost_OnServerInfoResponse( const netadr_t * /*address*/, const char * /*infoString*/, int /*ping*/ ) {
}

qboolean CL_WebHost_OnServerStatusResponseInfo( const netadr_t * /*address*/, const char * /*infoString*/ ) {
	return qfalse;
}

void CL_WebHost_OnServerStatusResponsePlayer( const netadr_t * /*address*/, const char * /*playerLine*/ ) {
}

void CL_WebHost_OnServerStatusResponseComplete( const netadr_t * /*address*/ ) {
}

void CL_WebView_PublishEvent( const char * /*name*/, const char * /*payload*/ ) {
}

void CL_WebView_InvokeCommNotice( const char * /*message*/ ) {
}

void CL_WebView_PublishTaggedInfoString( const char * /*messageType*/, const char * /*infoString*/ ) {
}

void CL_WebView_PublishGameError( const char * /*message*/ ) {
}

void CL_WebView_PublishGameEnd( void ) {
}

void CL_WebView_PublishCvarChange( const char * /*name*/, const char * /*value*/, qboolean /*replicate*/ ) {
}

void CL_WebView_PublishBindChanged( const char * /*name*/, const char * /*value*/ ) {
}

void CL_WebView_PublishGameStartForAddress( const netadr_t * /*serverAddress*/ ) {
}

void CL_WebView_PublishGameStart( void ) {
}

void CL_WebView_PublishGameDemo( const char * /*id*/, const char * /*name*/ ) {
}

void CL_WebView_PublishGameScreenshot( const char * /*id*/, const char * /*name*/ ) {
}

void CL_WebView_OnMouseMove( int /*x*/, int /*y*/ ) {
}

void CL_WebView_OnMouseButtonEvent( int /*key*/, qboolean /*down*/ ) {
}

void CL_WebView_OnMouseWheelEvent( int /*direction*/ ) {
}

qboolean CL_WebView_CaptureMouseKeyEvent( int /*key*/, qboolean /*down*/ ) {
	return qfalse;
}

void CL_WebView_OnKeyEvent( int /*key*/, qboolean /*down*/ ) {
}

void CL_WebView_OnCharEvent( int /*codepoint*/ ) {
}

qhandle_t CL_AdvertisementBridge_SetupAdvertCellShader( const char * /*defaultContent*/, const void * /*rect*/, int /*cellId*/ ) {
	return 0;
}

qhandle_t CL_AdvertisementBridge_RefreshAdvertCellShader( const char * /*defaultContent*/, const void * /*rect*/, int /*cellId*/ ) {
	return 0;
}

qhandle_t CL_AdvertisementBridge_SetupUIAdvertCellShader( const char * /*defaultContent*/, const void * /*rect*/, int /*cellId*/ ) {
	return 0;
}

qhandle_t CL_AdvertisementBridge_RefreshUIAdvertCellShader( const char * /*defaultContent*/, const void * /*rect*/, int /*cellId*/ ) {
	return 0;
}

void CL_AdvertisementBridge_InitCGame( void ) {
}

void CL_AdvertisementBridge_ShutdownCGame( void ) {
}

void CL_AdvertisementBridge_InitUI( void ) {
}

void CL_AdvertisementBridge_Reserved21C0( void ) {
}

void CL_AdvertisementBridge_SetActiveAdvert( int /*cellId*/ ) {
}

void CL_AdvertisementBridge_ActivateAdvert( int /*cellId*/ ) {
}

void CL_AdvertisementBridge_UpdateAdvert( int /*handleOrToken*/, int /*area*/ ) {
}

void CL_AdvertisementBridge_SetMapPath( const char * /*mapPath*/ ) {
}

void CL_AdvertisementBridge_UpdateViewParameters( void ) {
}

void CL_AdvertisementBridge_RefreshLoadingViewParameters( void ) {
}

void CL_AdvertisementBridge_UpdateLoadingViewParameters( void ) {
}

void CL_AdvertisementBridge_SetFrameTime( int /*frameTime*/ ) {
}

void CL_AdvertisementBridge_ClearDelay( void ) {
}

qboolean CL_AdvertisementBridge_IsDelayElapsed( void ) {
	return qfalse;
}

int CL_AdvertisementBridge_GetCellDisplayState( int /*cellId*/ ) {
	return 0;
}

void CL_AdvertisementBridge_GetCellLabel( int /*cellId*/, char *buffer, int bufferSize ) {
	ClearText( buffer, bufferSize );
}

int CL_AdvertisementBridge_GetLabelList1Count( void ) {
	return 0;
}

void CL_AdvertisementBridge_GetLabelList1Entry( int /*index*/, char *buffer, int bufferSize ) {
	ClearText( buffer, bufferSize );
}

int CL_AdvertisementBridge_GetLabelList2Count( void ) {
	return 0;
}

void CL_AdvertisementBridge_GetLabelList2Entry( int /*index*/, char *buffer, int bufferSize ) {
	ClearText( buffer, bufferSize );
}

qboolean CL_Awesomium_RequestResource( const char * /*virtualPath*/, void ** /*outBuffer*/, int * /*outLength*/ ) {
	return qfalse;
}

qboolean CL_Awesomium_Startup( const char * /*runtimePath*/, const char * /*basePath*/, const char * /*retailPath*/, const char * /*playerName*/, int /*width*/, int /*height*/, const char * /*initialConfigJson*/, const char * /*initialMapJson*/, const char * /*initialFactoryJson*/, const char * /*initialFriendJson*/ ) {
	return qfalse;
}

qboolean CL_Awesomium_OpenURL( const char * /*url*/ ) {
	return qfalse;
}

void CL_Awesomium_Update( void ) {
}

qboolean CL_Awesomium_Resize( int /*width*/, int /*height*/ ) {
	return qfalse;
}

int CL_Awesomium_SurfaceWidth( void ) {
	return 0;
}

int CL_Awesomium_SurfaceHeight( void ) {
	return 0;
}

qboolean CL_Awesomium_SurfaceDirty( void ) {
	return qfalse;
}

qboolean CL_Awesomium_IsLoading( void ) {
	return qfalse;
}

qboolean CL_Awesomium_IsCrashed( void ) {
	return qfalse;
}

int CL_Awesomium_LastErrorCode( void ) {
	return 0;
}

qboolean CL_Awesomium_ExecuteJavascript( const char * /*script*/, const char * /*frame*/ ) {
	return qfalse;
}

qboolean CL_Awesomium_ExecuteJavascriptInteger( const char * /*script*/, const char * /*frame*/, int * /*outValue*/ ) {
	return qfalse;
}

qboolean CL_Awesomium_PopJavascriptRequest( char *buffer, int bufferSize ) {
	ClearText( buffer, bufferSize );
	return qfalse;
}

qboolean CL_Awesomium_SetZoom( int /*zoomPercent*/ ) {
	return qfalse;
}

void CL_Awesomium_PauseRendering( void ) {
}

void CL_Awesomium_Unfocus( void ) {
}

qboolean CL_Awesomium_CopySurface( byte * /*destination*/, int /*width*/, int /*height*/, int /*rowSpan*/ ) {
	return qfalse;
}

void CL_Awesomium_InjectMouseMove( int /*x*/, int /*y*/ ) {
}

void CL_Awesomium_InjectMouseDown( int /*button*/ ) {
}

void CL_Awesomium_InjectMouseUp( int /*button*/ ) {
}

void CL_Awesomium_InjectMouseWheel( int /*direction*/ ) {
}

void CL_Awesomium_InjectKeyboardEvent( unsigned int /*eventType*/, unsigned int /*virtualKeyCode*/, long /*nativeKeyCode*/ ) {
}

void CL_Awesomium_Stop( void ) {
}

void CL_Awesomium_ClearCache( void ) {
}

void CL_Awesomium_Reload( qboolean /*ignoreCache*/ ) {
}

void CL_Awesomium_Shutdown( void ) {
}

const char *CL_Awesomium_LastError( void ) {
	return "";
}

} // extern "C"
