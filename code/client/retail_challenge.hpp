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

#ifndef FNQL_CLIENT_RETAIL_CHALLENGE_HPP
#define FNQL_CLIENT_RETAIL_CHALLENGE_HPP

#include <cstdint>

namespace fnql::client::auth {

enum class ChallengeRequestMode : std::uint8_t {
	None,
	LegacyNonce,
	// QAA: PC deviation. QAA has no Steam, so FnQL's Steam-ticket request
	// (RetailSteam) is removed: protocol 91 always sends retail's bare
	// no-Steam challenge.
	RetailWithoutSteam
};

[[nodiscard]] constexpr bool IsRetailRequest(
	ChallengeRequestMode mode ) noexcept {
	return mode == ChallengeRequestMode::RetailWithoutSteam;
}

[[nodiscard]] constexpr int SelectHandshakeProtocol(
	ChallengeRequestMode mode, int advertisedProtocol,
	int retailProtocol, int legacyProtocol ) noexcept {
	if ( advertisedProtocol > 0 ) {
		return advertisedProtocol;
	}
	return IsRetailRequest( mode ) ? retailProtocol : legacyProtocol;
}

} // namespace fnql::client::auth

#endif
