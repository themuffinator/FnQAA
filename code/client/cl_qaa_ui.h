/*
===========================================================================
FnQAA: QAA's Scaleform front end (milestone M6).

Copyright (C) 2026 FnQAA contributors.
This file is part of FnQAA, licensed under the GNU GPL v2 or later.

Quake Arena Arcade has no ui module: its menus, HUD and scoreboards are
Scaleform GFx movies run by default.xex's SFUI layer. FnQAA recompiles
that layer and the GFx player from the user's default.xex into the qaa_ui
guest module (tools/recomp/build.py, data/recomp/ui.json) and hosts it
here: the engine functions it calls are implemented over FnQL, and the
draw commands its renderer records are replayed into the frame. See
docs/UI.md.
===========================================================================
*/
#ifndef CL_QAA_UI_H
#define CL_QAA_UI_H

#include <cstdint>

/* C linkage: some client sources include this inside extern "C" blocks. */
#ifdef __cplusplus
extern "C" {
#endif

/* Loads the module and runs SFUI's start-up (the startup and uimain
 * movies). False if the module is unavailable; the client then runs with
 * the console only, as before. */
bool QAA_UI_Init( void );
void QAA_UI_Shutdown( void );
bool QAA_UI_Active( void );

/* Whether a video mode is "widescreen" for QAA: 16:9 or wider. The front
 * end's glconfig flag (movie viewports) and r_widescreen (the HUD's layout)
 * both follow it. */
bool QAA_UI_IsWidescreen( int width, int height );

/* The console region the front end is told (XGetLocale's country code),
 * two lower-case letters: qaa_country if set, else the operating system's
 * region, else "us". */
const char *QAA_UI_CountryCode( void );

/* Once per client frame, before the screen update: one iteration of SFUI's
 * update thread (input, invokes, Advance). */
void QAA_UI_Frame( void );

/* In the screen update, after the cgame: SFUI's render and the replay of
 * what it recorded, over the frame. */
void QAA_UI_Draw( void );

/* A key event for the front end; true if it took it. Mouse buttons and
 * the wheel go to it while it owns the pointer. */
bool QAA_UI_KeyEvent( int key, bool down, unsigned time );

/* The front end owns the mouse pointer: its menus, and anything it shows
 * outside a game (start-up movies, title, loading). The platform layers
 * then show the system pointer and report absolute positions
 * (Key_GetPointerCatcher). PC deviation: the Xbox has no mouse. */
bool QAA_UI_WantsPointer( void );

/* The pointer's position in renderer pixels, while the front end owns it. */
void QAA_UI_PointerMove( int x, int y );

/* cgame traps that feed the front end (docs/ABI.md, cgame 116..140), with
 * their guest-address arguments. Returns the trap's result. */
intptr_t QAA_UI_CgameTrap( int trap, const int32_t *args );

/* The qagame's menu traps (docs/ABI.md, game 48, 49, 65, 69); installed as
 * QAA_SV_GameUiTrap while the front end runs. */
intptr_t QAA_UI_GameTrap( int trap, const int32_t *args );

/* The engine's own calls into the front end, where retail makes them: at
 * the end of CL_InitCGame, and when the first snapshot makes the client
 * active. */
void QAA_UI_ClientLoadComplete( void );
void QAA_UI_EnterGame( void );

#ifdef __cplusplus
}
#endif

#endif
