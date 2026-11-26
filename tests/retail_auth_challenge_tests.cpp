// QAA: FnQL's Steam-ticket halves (the client packet builder, the server
// parser and its cache) are removed; protocol selection stays.
#include "../code/client/retail_challenge.hpp"

#include <cstdio>

namespace clientAuth = fnql::client::auth;

namespace {

int failures = 0;

void Check( bool condition, const char *message ) {
	if ( !condition ) {
		std::fprintf( stderr, "FAIL: %s\n", message );
		++failures;
	}
}

void TestProtocolSelection() {
	Check( clientAuth::SelectHandshakeProtocol(
		clientAuth::ChallengeRequestMode::RetailWithoutSteam, 0, 91, 68 ) == 91,
		"bare no-Steam retail response selects protocol 91" );
	Check( clientAuth::SelectHandshakeProtocol(
		clientAuth::ChallengeRequestMode::LegacyNonce, 0, 91, 68 ) == 68,
		"legacy challenge retains protocol 68 fallback" );
	Check( clientAuth::SelectHandshakeProtocol(
		clientAuth::ChallengeRequestMode::RetailWithoutSteam, 71, 91, 68 ) == 71,
		"explicit server protocol remains authoritative" );
}

} // namespace

int main() {
	TestProtocolSelection();
	if ( failures != 0 ) {
		std::fprintf( stderr, "%d test(s) failed\n", failures );
		return 1;
	}
	std::puts( "retail challenge tests passed" );
	return 0;
}
