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
#ifndef FNQL_SERVER_CVAR_COMPAT_HPP
#define FNQL_SERVER_CVAR_COMPAT_HPP

#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>

namespace fnql::server::cvars {

/*
Retail sv_serverType ("0", archived; the Start Match page labels the values
Offline, LAN and Internet). Observed in quakelive_steam.exe 0.1.0.739:
- SV_PacketEvent processes a packet only when the cvar is nonzero or the
  sender is the in-process loopback address (0x4e4518);
- getchallenge from a non-LAN address is answered with a refusal print while
  the cvar is exactly 1 (0x4df440).
*/
inline constexpr char kLanOnlyChallengeReply[] =
	"print\nServer is for LAN clients only.\n";

[[nodiscard]] constexpr bool ServerTypeAcceptsPacket(
	int serverType, bool loopbackSender ) noexcept
{
	return serverType != 0 || loopbackSender;
}

[[nodiscard]] constexpr bool ServerTypeAcceptsChallenge(
	int serverType, bool lanSender ) noexcept
{
	return serverType != 1 || lanSender;
}

enum class ExitLevelAction {
	Continue,
	Shutdown,
	Quit
};

[[nodiscard]] constexpr ExitLevelAction SelectExitLevelAction(
	int policy ) noexcept
{
	return policy <= 0 ? ExitLevelAction::Continue
		: policy == 1 ? ExitLevelAction::Shutdown : ExitLevelAction::Quit;
}

[[nodiscard]] constexpr bool ShouldEscalateError(
	int policy, bool serverRunning, bool recoverable ) noexcept
{
	return recoverable && ( policy >= 2 || ( policy == 1 && serverRunning ) );
}

/*
Unsigned subtraction makes the timer deterministic across the 32-bit engine
clock wrap. A fired timer remains one-shot until activity or disabling the
corresponding policy rearms it.
*/
class InactivityTimer {
public:
	[[nodiscard]] bool Poll(
		bool inactive, std::uint32_t now, int delaySeconds ) noexcept
	{
		if ( !inactive || delaySeconds <= 0 ) {
			Reset();
			return false;
		}
		if ( fired_ ) {
			return false;
		}
		if ( !armed_ ) {
			armed_ = true;
			started_ = now;
			return false;
		}

		constexpr std::uint64_t maximumDelay =
			( std::numeric_limits<std::uint32_t>::max )();
		const std::uint64_t requestedDelay =
			static_cast<std::uint64_t>( delaySeconds ) * 1000u;
		const std::uint32_t delay = static_cast<std::uint32_t>(
			requestedDelay > maximumDelay ? maximumDelay : requestedDelay );
		if ( static_cast<std::uint32_t>( now - started_ ) < delay ) {
			return false;
		}
		fired_ = true;
		return true;
	}

	void Reset() noexcept
	{
		armed_ = false;
		fired_ = false;
		started_ = 0;
	}

private:
	std::uint32_t started_ = 0;
	bool armed_ = false;
	bool fired_ = false;
};

[[nodiscard]] inline bool IsSafeEntityPathComponent(
	std::string_view value ) noexcept
{
	if ( value.empty() || value == "." || value == ".." ) {
		return false;
	}
	for ( const unsigned char character : value ) {
		if ( character < 0x20u || character == 0x7fu || character == '/'
			|| character == '\\' || character == ':' ) {
			return false;
		}
	}
	return true;
}

/*
Retail's override is rooted in the virtual filesystem. Keep that contract,
but reject absolute paths, traversal, and platform separators so a server cvar
cannot escape its configured game directory on any supported platform.
*/
[[nodiscard]] inline std::optional<std::string> BuildEntityFilePath(
	std::string_view directory, std::string_view mapName,
	std::size_t maximumBytes )
{
	if ( !IsSafeEntityPathComponent( mapName ) || directory.empty()
		|| maximumBytes == 0 ) {
		return std::nullopt;
	}

	std::string path;
	std::size_t start = 0;
	while ( start < directory.size() ) {
		const std::size_t separator = directory.find( '/', start );
		const std::size_t end = separator == std::string_view::npos
			? directory.size() : separator;
		const std::string_view component = directory.substr( start, end - start );
		if ( !IsSafeEntityPathComponent( component ) ) {
			return std::nullopt;
		}
		if ( !path.empty() ) {
			path.push_back( '/' );
		}
		path.append( component );
		if ( separator == std::string_view::npos ) {
			break;
		}
		start = separator + 1;
	}

	if ( path.empty() ) {
		return std::nullopt;
	}
	path.push_back( '/' );
	path.append( mapName );
	path.append( ".ent" );
	if ( path.size() + 1 > maximumBytes ) {
		return std::nullopt;
	}
	return path;
}

// QAA: retail's Steam game-server identity and tags are removed (no Steam)

} // namespace fnql::server::cvars

#endif // FNQL_SERVER_CVAR_COMPAT_HPP
