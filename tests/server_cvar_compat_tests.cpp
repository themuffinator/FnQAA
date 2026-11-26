#include <cstdint>
#include <iostream>
#include <limits>
#include <string>

#include "../code/server/server_cvar_compat.hpp"

namespace {

int failures;

void Check( bool condition, const char *expression, int line )
{
	if ( condition ) return;
	std::cerr << "line " << line << ": check failed: " << expression << '\n';
	++failures;
}

#define CHECK(expression) Check( ( expression ), #expression, __LINE__ )

void TestPolicies()
{
	using namespace fnql::server::cvars;
	CHECK( SelectExitLevelAction( 0 ) == ExitLevelAction::Continue );
	CHECK( SelectExitLevelAction( 1 ) == ExitLevelAction::Shutdown );
	CHECK( SelectExitLevelAction( 2 ) == ExitLevelAction::Quit );
	CHECK( SelectExitLevelAction( 99 ) == ExitLevelAction::Quit );
	CHECK( !ShouldEscalateError( 0, true, true ) );
	CHECK( ShouldEscalateError( 1, true, true ) );
	CHECK( !ShouldEscalateError( 1, false, true ) );
	CHECK( ShouldEscalateError( 2, false, true ) );
	CHECK( !ShouldEscalateError( 2, true, false ) );
}

void TestInactivityTimer()
{
	using fnql::server::cvars::InactivityTimer;
	InactivityTimer timer;
	CHECK( !timer.Poll( true, 100u, 1 ) );
	CHECK( !timer.Poll( true, 1099u, 1 ) );
	CHECK( timer.Poll( true, 1100u, 1 ) );
	CHECK( !timer.Poll( true, 2100u, 1 ) );
	CHECK( !timer.Poll( false, 2200u, 1 ) );
	CHECK( !timer.Poll( true, 2300u, 1 ) );
	CHECK( timer.Poll( true, 3300u, 1 ) );

	timer.Reset();
	constexpr std::uint32_t nearWrap =
		( std::numeric_limits<std::uint32_t>::max )() - 500u;
	CHECK( !timer.Poll( true, nearWrap, 1 ) );
	CHECK( timer.Poll( true, 499u, 1 ) );
	CHECK( !timer.Poll( true, 1500u, 0 ) );
}

void TestEntityPaths()
{
	using fnql::server::cvars::BuildEntityFilePath;
	CHECK( BuildEntityFilePath( "noHMG", "campgrounds", 64 )
		== std::optional<std::string>( "noHMG/campgrounds.ent" ) );
	CHECK( BuildEntityFilePath( "custom/entities", "qzdm6", 64 )
		== std::optional<std::string>( "custom/entities/qzdm6.ent" ) );
	CHECK( !BuildEntityFilePath( "", "qzdm6", 64 ) );
	CHECK( !BuildEntityFilePath( "../outside", "qzdm6", 64 ) );
	CHECK( !BuildEntityFilePath( "/absolute", "qzdm6", 64 ) );
	CHECK( !BuildEntityFilePath( "C:/absolute", "qzdm6", 64 ) );
	CHECK( !BuildEntityFilePath( "safe\\windows", "qzdm6", 64 ) );
	CHECK( !BuildEntityFilePath( "safe", "../qzdm6", 64 ) );
	CHECK( !BuildEntityFilePath( "safe", "qzdm6", 8 ) );
}

void TestServerType()
{
	using fnql::server::cvars::ServerTypeAcceptsChallenge;
	using fnql::server::cvars::ServerTypeAcceptsPacket;

	// 0 (offline) hears only the in-process loopback client.
	CHECK( ServerTypeAcceptsPacket( 0, true ) );
	CHECK( !ServerTypeAcceptsPacket( 0, false ) );
	CHECK( ServerTypeAcceptsPacket( 1, false ) );
	CHECK( ServerTypeAcceptsPacket( 2, false ) );
	CHECK( ServerTypeAcceptsPacket( -1, false ) );

	// Exactly 1 (LAN) refuses a challenge from beyond the LAN.
	CHECK( ServerTypeAcceptsChallenge( 1, true ) );
	CHECK( !ServerTypeAcceptsChallenge( 1, false ) );
	CHECK( ServerTypeAcceptsChallenge( 2, false ) );
	CHECK( ServerTypeAcceptsChallenge( 0, false ) );

	CHECK( std::string_view( fnql::server::cvars::kLanOnlyChallengeReply ) ==
		"print\nServer is for LAN clients only.\n" );
}

} // namespace

int main()
{
	TestPolicies();
	TestInactivityTimer();
	TestEntityPaths();
	TestServerType();
	return failures == 0 ? 0 : 1;
}
