/*
===========================================================================
Copyright (C) 2026

This file is part of FnQL.

FnQL is free software; you can redistribute it and/or modify it
under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
===========================================================================
*/

#pragma once

#include <cctype>

// QAA: retail behaviour. QAA's S_StartBackgroundTrack (0x82173470) leaves
// the background track alone when the one it already plays is asked for
// again. A start records the loop's sound (the intro's when there is no
// loop, +0x3890 of the sound state at 0x82A67F4C) and sets a playing flag
// (+0x3884) that only S_StopBackgroundTrack (0x821736E8) clears. While the
// flag is set, a start whose intro or loop is the recorded loop does
// nothing (0x82173554). Without this the front end's main menu, which
// sends "playmusic" each time its scene is entered (ui/mainmenu.gfx),
// restarted the menus' music from its intro on every return to it. The
// header is free of engine types so the rule can be tested on its own.
namespace fnql_audio_music {

// Retail compares registered sound handles. The names here are compared as
// QAA's file system resolves them (qaa_bpk_name_hash): case-insensitively
// and with '/' and '\' alike.
inline bool SameMusicFile( const char *a, const char *b ) {
	if ( a == nullptr || b == nullptr ) {
		return false;
	}
	for ( ;; a++, b++ ) {
		char ca = static_cast<char>( std::tolower( static_cast<unsigned char>( *a ) ) );
		char cb = static_cast<char>( std::tolower( static_cast<unsigned char>( *b ) ) );
		if ( ca == '\\' ) {
			ca = '/';
		}
		if ( cb == '\\' ) {
			cb = '/';
		}
		if ( ca != cb ) {
			return false;
		}
		if ( ca == '\0' ) {
			return true;
		}
	}
}

// playingLoop: the loop recorded by the last start, or null or empty when no
// track plays. intro, loop: the new request (an empty loop means the intro).
inline bool KeepsPlayingTrack( const char *playingLoop, const char *intro, const char *loop ) {
	if ( playingLoop == nullptr || playingLoop[0] == '\0' || intro == nullptr || intro[0] == '\0' ) {
		return false;
	}
	if ( loop == nullptr || loop[0] == '\0' ) {
		loop = intro;
	}
	return SameMusicFile( playingLoop, intro ) || SameMusicFile( playingLoop, loop );
}

}
