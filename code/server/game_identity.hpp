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

#ifndef FNQL_SERVER_GAME_IDENTITY_HPP
#define FNQL_SERVER_GAME_IDENTITY_HPP

// The 64-bit player id the engine reports to a Quake Live game module (retail
// native import slot 200).  Retail qagame keys its access list -- admin, mod
// and ban entries, complaint tempbans included -- by this value and keeps an
// entry for 0 like any other, so reporting 0 for every human would let one
// `ban` or `addadmin` reach all of them.  Each human connection therefore
// gets its own id; a bot reports retail's 0.
//
// QAA: PC deviation.  Quake Arena Arcade has no Steam, so every human gets
// one of these ids.  They keep the SteamID64 "anonymous user" layout, which no
// real account has, so an access list saved by a Quake Live server never
// matches one.  An id lives only as long as its connection.

#include <cstdint>

namespace fnql::server::identity {

// SteamID64: universe (8 bits) | account type (4) | instance (20) | account (32).
inline constexpr unsigned AccountTypeShift = 52;
inline constexpr unsigned UniverseShift = 56;
inline constexpr std::uint64_t UniversePublic = 1;
inline constexpr std::uint64_t AccountTypeAnonUser = 10;
inline constexpr std::uint64_t AnonymousSerialMask = ( std::uint64_t{ 1 } << AccountTypeShift ) - 1u;
inline constexpr std::uint64_t AnonymousPrefix =
	( UniversePublic << UniverseShift ) | ( AccountTypeAnonUser << AccountTypeShift );

constexpr bool IsAnonymous( std::uint64_t id ) {
	return ( id & ~AnonymousSerialMask ) == AnonymousPrefix;
}

constexpr std::uint64_t MakeAnonymous( std::uint64_t serial ) {
	return AnonymousPrefix | ( serial & AnonymousSerialMask );
}

// Hands out ids from a 52-bit serial space, so a process never reuses one
// (2^52 connections would be needed to wrap).  The server seeds it from
// random bytes so ids persisted by one run (the game saves its access list)
// do not land on a different player in the next; deterministic tooling
// seeds it with a constant instead.
class AnonymousIdAllocator {
public:
	void Seed( std::uint64_t base ) {
		next_ = base & AnonymousSerialMask;
		seeded_ = true;
	}

	bool Seeded() const { return seeded_; }

	std::uint64_t Next() {
		const std::uint64_t id = MakeAnonymous( next_ );
		next_ = ( next_ + 1u ) & AnonymousSerialMask;
		return id;
	}

private:
	std::uint64_t next_ = 0;
	bool seeded_ = false;
};

// What slot 200 answers.  A bot is 0, as in retail; the server alone decides
// what a bot is (its NA_BOT address), never userinfo.  Anything else is the
// slot's own id (0 only for a slot that never connected).
constexpr std::uint64_t GameId( bool serverBot, std::uint64_t slotId ) {
	return !serverBot && IsAnonymous( slotId ) ? slotId : 0;
}

} // namespace fnql::server::identity

#endif // FNQL_SERVER_GAME_IDENTITY_HPP
