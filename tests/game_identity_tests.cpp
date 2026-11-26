/*
===========================================================================
Copyright (C) 2026 FnQL contributors

This file is part of FnQL.

FnQL is free software; you can redistribute it and/or modify it under the
terms of the GNU General Public License as published by the Free Software
Foundation; either version 2 of the License, or (at your option) any later
version.
===========================================================================
*/

// The 64-bit player id the engine reports to a Quake Live game module
// (import slot 200).  The game keys admin, mod and ban entries by it, so no
// two human connections may share a value, and bots, decided by the server
// alone, stay at retail's 0.

#include "../code/server/game_identity.hpp"

#include <cstdint>
#include <cstdio>
#include <set>

namespace identity = fnql::server::identity;

namespace {

int failures = 0;

void Check( bool condition, const char *message ) {
	if ( !condition ) {
		std::fprintf( stderr, "FAIL: %s\n", message );
		++failures;
	}
}

// A SteamID64 for a public individual account, as a Quake Live server's saved
// access list may hold.
constexpr std::uint64_t Individual( std::uint32_t account ) {
	return ( identity::UniversePublic << identity::UniverseShift ) |
		( std::uint64_t{ 1 } << identity::AccountTypeShift ) |
		( std::uint64_t{ 1 } << 32 ) | account;
}

static_assert( Individual( 22202 ) == 76561197960287930ull, "individual SteamID64 layout" );
static_assert( !identity::IsAnonymous( Individual( 1 ) ), "an individual id is never anonymous" );
static_assert( identity::IsAnonymous( identity::MakeAnonymous( 0 ) ), "serial 0 is still a non-zero id" );
static_assert( identity::MakeAnonymous( 0 ) != 0, "an id is never 0" );

// The access-list hazard: humans all used to report 0.
void TestHumansDoNotShareAnId() {
	identity::AnonymousIdAllocator allocator;
	allocator.Seed( 0 );
	std::set<std::uint64_t> seen;
	for ( int i = 0; i < 4096; ++i ) {
		const std::uint64_t reported = identity::GameId( false, allocator.Next() );
		Check( reported != 0, "a human never reports 0" );
		Check( identity::IsAnonymous( reported ), "a human reports an anonymous-range id" );
		Check( seen.insert( reported ).second, "no two humans share an id" );
	}
}

// Bot-ness comes from the server; nothing a client sends changes it.
void TestBotsAreZero() {
	Check( identity::GameId( true, 0 ) == 0, "a bot reports 0" );
	Check( identity::GameId( true, identity::MakeAnonymous( 3 ) ) == 0,
		"a bot reports 0 whatever id its slot holds" );
	Check( identity::GameId( false, identity::MakeAnonymous( 9 ) ) == identity::MakeAnonymous( 9 ),
		"a non-bot slot is answered with its own id" );
}

// A slot that never connected (or was dropped) answers 0, not a stale id.
void TestUnassignedSlots() {
	Check( identity::GameId( false, 0 ) == 0, "an unassigned slot reports 0" );
	Check( identity::GameId( false, Individual( 5 ) ) == 0,
		"only an anonymous-range value is accepted as a slot's id" );
}

void TestAllocatorSeedingAndWrap() {
	identity::AnonymousIdAllocator a;
	identity::AnonymousIdAllocator b;
	Check( !a.Seeded(), "an allocator starts unseeded" );
	a.Seed( 77 );
	b.Seed( 77 );
	Check( a.Seeded(), "Seed marks the allocator seeded" );
	for ( int i = 0; i < 16; ++i ) {
		Check( a.Next() == b.Next(), "equal seeds give equal sequences (sv_lockstep determinism)" );
	}

	// The serial wraps inside the 52-bit space without leaving the
	// anonymous range or producing 0.
	identity::AnonymousIdAllocator edge;
	edge.Seed( ~std::uint64_t{ 0 } );
	const std::uint64_t last = edge.Next();
	const std::uint64_t wrapped = edge.Next();
	Check( last == identity::MakeAnonymous( identity::AnonymousSerialMask ), "a seed is masked to the serial space" );
	Check( wrapped == identity::MakeAnonymous( 0 ), "the serial wraps to 0" );
	Check( identity::IsAnonymous( last ) && identity::IsAnonymous( wrapped ), "wrapped ids stay anonymous" );
	Check( last != wrapped && wrapped != 0, "wrapping produces neither a repeat nor 0" );
}

} // namespace

int main() {
	TestHumansDoNotShareAnId();
	TestBotsAreZero();
	TestUnassignedSlots();
	TestAllocatorSeedingAndWrap();
	if ( failures != 0 ) {
		std::fprintf( stderr, "%d test(s) failed\n", failures );
		return 1;
	}
	std::puts( "game identity tests passed" );
	return 0;
}
