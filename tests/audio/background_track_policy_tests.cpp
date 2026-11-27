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

// QAA: retail S_StartBackgroundTrack (0x82173470) keeps the track it plays
// when that track is asked for again (BackgroundTrackPolicy.h).

#include "BackgroundTrackPolicy.h"

#include <cstdio>

namespace {

namespace music = fnql_audio_music;

bool Check( bool condition, const char *test, int line, const char *expression ) {
	if ( condition ) {
		return true;
	}
	std::fprintf( stderr, "%s:%d: check failed: %s\n", test, line, expression );
	return false;
}

#define CHECK( expression ) do { if ( !Check( ( expression ), __func__, __LINE__, #expression ) ) return false; } while ( 0 )

constexpr const char *kMenuIntro = "music/ambient_2_mix3_intro.wav";
constexpr const char *kMenuLoop = "music/ambient_2_mix3_loop.wav";

// The main menu sends the same playmusic each time its scene is entered.
bool TheSameRequestKeepsTheTrack() {
	CHECK( music::KeepsPlayingTrack( kMenuLoop, kMenuIntro, kMenuLoop ) );
	return true;
}

// Retail compares the recorded loop with the new intro too.
bool ARequestStartingWithThePlayingLoopKeepsIt() {
	CHECK( music::KeepsPlayingTrack( kMenuLoop, kMenuLoop, "music/other.wav" ) );
	return true;
}

// Without a loop the intro is recorded, and a request for it alone matches.
bool AnIntroOnlyTrackMatchesItself() {
	CHECK( music::KeepsPlayingTrack( "music\\fla_mp05.wav", "music\\fla_mp05.wav", "" ) );
	CHECK( music::KeepsPlayingTrack( "music\\fla_mp05.wav", "music\\fla_mp05.wav", nullptr ) );
	return true;
}

bool ADifferentTrackReplacesIt() {
	CHECK( !music::KeepsPlayingTrack( kMenuLoop, "music/credits.wav", nullptr ) );
	CHECK( !music::KeepsPlayingTrack( "music\\fla_mp05.wav", kMenuIntro, kMenuLoop ) );
	return true;
}

// Only the recorded loop counts: the playing intro of another request does not.
bool OnlyTheRecordedLoopIsCompared() {
	CHECK( !music::KeepsPlayingTrack( kMenuLoop, kMenuIntro, "music/other_loop.wav" ) );
	return true;
}

bool NothingPlayingStartsTheTrack() {
	CHECK( !music::KeepsPlayingTrack( "", kMenuIntro, kMenuLoop ) );
	CHECK( !music::KeepsPlayingTrack( nullptr, kMenuIntro, kMenuLoop ) );
	return true;
}

// An empty intro is a stop, never a match.
bool AnEmptyIntroIsNotKept() {
	CHECK( !music::KeepsPlayingTrack( kMenuLoop, "", kMenuLoop ) );
	CHECK( !music::KeepsPlayingTrack( kMenuLoop, nullptr, kMenuLoop ) );
	return true;
}

// The cgame names files with '\', the front end with '/'.
bool NamesMatchAsTheFileSystemResolvesThem() {
	CHECK( music::SameMusicFile( "music\\Fla_MP05.wav", "MUSIC/fla_mp05.WAV" ) );
	CHECK( !music::SameMusicFile( "music/fla_mp05.wav", "music/fla_mp05.wa" ) );
	CHECK( !music::SameMusicFile( "music/a.wav", nullptr ) );
	return true;
}

struct TestCase {
	const char *name;
	bool ( *run )();
};

}

int main() {
	const TestCase tests[] = {
		{ "TheSameRequestKeepsTheTrack", TheSameRequestKeepsTheTrack },
		{ "ARequestStartingWithThePlayingLoopKeepsIt", ARequestStartingWithThePlayingLoopKeepsIt },
		{ "AnIntroOnlyTrackMatchesItself", AnIntroOnlyTrackMatchesItself },
		{ "ADifferentTrackReplacesIt", ADifferentTrackReplacesIt },
		{ "OnlyTheRecordedLoopIsCompared", OnlyTheRecordedLoopIsCompared },
		{ "NothingPlayingStartsTheTrack", NothingPlayingStartsTheTrack },
		{ "AnEmptyIntroIsNotKept", AnEmptyIntroIsNotKept },
		{ "NamesMatchAsTheFileSystemResolvesThem", NamesMatchAsTheFileSystemResolvesThem }
	};

	for ( const TestCase &test : tests ) {
		if ( !test.run() ) {
			std::fprintf( stderr, "%s failed\n", test.name );
			return 1;
		}
		std::printf( "%s passed\n", test.name );
	}
	return 0;
}
