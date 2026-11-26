/*
===========================================================================
openQAA: the botlib bridge for QAA's recompiled qagame (milestone M4).

Copyright (C) 2026 openQAA contributors.
This file is part of openQAA, licensed under the GNU GPL v2 or later.

Included by sv_qaa_game.inc, before QAA_GameSyscall.

QAA's botlib traps (200..581) are Quake III 1.32's, number for number, and
each one calls the matching slot of the engine's botlib export table
(docs/ABI.md, game_traps.tsv). The engine runs FnQL's botlib, natively.
What this file adds is the boundary: every pointer argument is a guest
address, and every structure behind one is big-endian with 32-bit pointers.

Except for the two entity structures, the guest layouts are Quake III's and
equal FnQL's host layouts field for field (checked by the static_asserts
below), so a structure crosses as its 32-bit words byte-swapped, with its
char arrays copied as they are and its 16-bit fields swapped as halves.
Each one is read from the guest before the call and written back after it,
which leaves an input unchanged and delivers an output; a structure the
botlib did not touch round-trips exactly.

The entity structures are QAA's own (code, below): Quake III's prefix plus
Quake Live's extension without its trainingClear field.

QAA's botlib has no chat AI: retail stubs every chat-state call, so they are
answered here as retail answers them, and the botlib skips its chat setup.
===========================================================================
*/

#include "../botlib/be_ai_chat.h"
#include "../botlib/be_ai_goal.h"
#include "../botlib/be_ai_move.h"
#include "../botlib/be_ai_weap.h"

namespace qaa_bot {

/* ---- structure layouts --------------------------------------------------- */

enum class Kind : uint8_t { Bytes, Halves };
struct Span {
	uint32_t offset, size;
	Kind kind;
};
/* Everything outside the spans is 32-bit words. */
struct Layout {
	uint32_t size;
	const Span *spans;
	size_t count;
};

template <size_t N>
constexpr Layout MakeLayout( uint32_t size, const Span ( &spans )[N] ) {
	return Layout{ size, spans, N };
}
constexpr Layout MakeLayout( uint32_t size ) {
	return Layout{ size, nullptr, 0 };
}

#define QAA_BYTES( type, field ) Span{ static_cast<uint32_t>( offsetof( type, field ) ), static_cast<uint32_t>( sizeof( type::field ) ), Kind::Bytes }

// Quake III 1.32 sizes, which the guest uses
static_assert( sizeof( aas_areainfo_t ) == 0x34, "aas_areainfo_t" );
static_assert( sizeof( aas_clientmove_t ) == 0x54, "aas_clientmove_t" );
static_assert( sizeof( aas_predictroute_t ) == 0x24, "aas_predictroute_t" );
static_assert( sizeof( aas_altroutegoal_t ) == 0x18, "aas_altroutegoal_t" );
static_assert( sizeof( bot_input_t ) == 0x28, "bot_input_t" );
static_assert( sizeof( bot_goal_t ) == 0x38, "bot_goal_t" );
static_assert( sizeof( bot_moveresult_t ) == 0x34, "bot_moveresult_t" );
static_assert( sizeof( bot_initmove_t ) == 0x44, "bot_initmove_t" );
static_assert( sizeof( weaponinfo_t ) == 0x228, "weaponinfo_t" );
static_assert( sizeof( bot_match_t ) == 0x148, "bot_match_t" );
static_assert( sizeof( pc_token_t ) == 0x410, "pc_token_t" );

constexpr Layout kAreaInfo = MakeLayout( sizeof( aas_areainfo_t ) );
constexpr Layout kClientMove = MakeLayout( sizeof( aas_clientmove_t ) );
constexpr Layout kPredictRoute = MakeLayout( sizeof( aas_predictroute_t ) );
constexpr Span kAltRouteSpans[] = { { 0x10, 8, Kind::Halves } };	// three travel times and the padding
constexpr Layout kAltRouteGoal = MakeLayout( sizeof( aas_altroutegoal_t ), kAltRouteSpans );
constexpr Layout kInput = MakeLayout( sizeof( bot_input_t ) );
constexpr Layout kGoal = MakeLayout( sizeof( bot_goal_t ) );
constexpr Layout kMoveResult = MakeLayout( sizeof( bot_moveresult_t ) );
constexpr Layout kInitMove = MakeLayout( sizeof( bot_initmove_t ) );
constexpr Span kWeaponInfoSpans[] = {
	QAA_BYTES( weaponinfo_t, name ), QAA_BYTES( weaponinfo_t, model ), QAA_BYTES( weaponinfo_t, projectile ),
	{ static_cast<uint32_t>( offsetof( weaponinfo_t, proj ) + offsetof( projectileinfo_t, name ) ), MAX_STRINGFIELD, Kind::Bytes },
	{ static_cast<uint32_t>( offsetof( weaponinfo_t, proj ) + offsetof( projectileinfo_t, model ) ), MAX_STRINGFIELD, Kind::Bytes },
};
constexpr Layout kWeaponInfo = MakeLayout( sizeof( weaponinfo_t ), kWeaponInfoSpans );
constexpr Span kMatchSpans[] = {
	QAA_BYTES( bot_match_t, string ),
	// bot_matchvariable_t { char offset; int length; }: the char is a byte
	{ static_cast<uint32_t>( offsetof( bot_match_t, variables ) + 0 * sizeof( bot_matchvariable_t ) ), 1, Kind::Bytes },
	{ static_cast<uint32_t>( offsetof( bot_match_t, variables ) + 1 * sizeof( bot_matchvariable_t ) ), 1, Kind::Bytes },
	{ static_cast<uint32_t>( offsetof( bot_match_t, variables ) + 2 * sizeof( bot_matchvariable_t ) ), 1, Kind::Bytes },
	{ static_cast<uint32_t>( offsetof( bot_match_t, variables ) + 3 * sizeof( bot_matchvariable_t ) ), 1, Kind::Bytes },
	{ static_cast<uint32_t>( offsetof( bot_match_t, variables ) + 4 * sizeof( bot_matchvariable_t ) ), 1, Kind::Bytes },
	{ static_cast<uint32_t>( offsetof( bot_match_t, variables ) + 5 * sizeof( bot_matchvariable_t ) ), 1, Kind::Bytes },
	{ static_cast<uint32_t>( offsetof( bot_match_t, variables ) + 6 * sizeof( bot_matchvariable_t ) ), 1, Kind::Bytes },
	{ static_cast<uint32_t>( offsetof( bot_match_t, variables ) + 7 * sizeof( bot_matchvariable_t ) ), 1, Kind::Bytes },
};
static_assert( MAX_MATCHVARIABLES == 8, "kMatchSpans lists eight variables" );
constexpr Layout kMatch = MakeLayout( sizeof( bot_match_t ), kMatchSpans );
constexpr Span kTokenSpans[] = { QAA_BYTES( pc_token_t, string ) };
constexpr Layout kToken = MakeLayout( sizeof( pc_token_t ), kTokenSpans );

#undef QAA_BYTES

static void FromGuest( void *host, uint32_t a, const Layout &L ) {
	byte *h = static_cast<byte *>( host );
	for ( uint32_t o = 0; o + 4 <= L.size; o += 4 ) {
		const uint32_t w = QAA_LoadU32( a + o );
		std::memcpy( h + o, &w, 4 );
	}
	for ( size_t i = 0; i < L.count; i++ ) {
		const Span &s = L.spans[i];
		if ( s.kind == Kind::Bytes ) {
			std::memcpy( h + s.offset, QAA_Ptr( a + s.offset ), s.size );
		} else {
			for ( uint32_t o = s.offset; o + 2 <= s.offset + s.size; o += 2 ) {
				const uint16_t v = QAA_LoadU16( a + o );
				std::memcpy( h + o, &v, 2 );
			}
		}
	}
}

static void ToGuest( uint32_t a, const void *host, const Layout &L ) {
	const byte *h = static_cast<const byte *>( host );
	for ( uint32_t o = 0; o + 4 <= L.size; o += 4 ) {
		uint32_t w;
		std::memcpy( &w, h + o, 4 );
		QAA_StoreU32( a + o, w );
	}
	for ( size_t i = 0; i < L.count; i++ ) {
		const Span &s = L.spans[i];
		if ( s.kind == Kind::Bytes ) {
			std::memcpy( QAA_Ptr( a + s.offset ), h + s.offset, s.size );
		} else {
			for ( uint32_t o = s.offset; o + 2 <= s.offset + s.size; o += 2 ) {
				uint16_t v;
				std::memcpy( &v, h + o, 2 );
				QAA_StoreU16( a + o, v );
			}
		}
	}
}

/* ---- arguments ------------------------------------------------------------ */

/* A guest structure (or an array of them): read before the call, written back after it. */
template <typename T, size_t N = 1>
class Struct {
public:
	Struct( uint32_t a, const Layout &layout, int count = 1 ) : address( a ), L( layout ), n( Clamp( a, count ) ) {
		Com_Memset( host, 0, sizeof( host ) );
		for ( int i = 0; i < n; i++ ) {
			FromGuest( &host[i], Element( i ), L );
		}
	}
	~Struct() {
		for ( int i = 0; i < n; i++ ) {
			ToGuest( Element( i ), &host[i], L );
		}
	}
	Struct( const Struct & ) = delete;
	Struct &operator=( const Struct & ) = delete;
	T *p() { return address ? host : nullptr; }
	int Count() const { return n; }

private:
	static int Clamp( uint32_t a, int count ) {
		if ( !a || count <= 0 ) {
			return 0;
		}
		if ( count > static_cast<int>( N ) ) {
			Com_Error( ERR_DROP, "qagame: botlib call with %d structures (at most %d)", count, static_cast<int>( N ) );
		}
		return count;
	}
	uint32_t Element( int i ) const { return address + static_cast<uint32_t>( i ) * L.size; }

	uint32_t address;
	const Layout &L;
	int n;
	T host[N];
};

/* A guest vec3_t: read before the call, written back after it. */
class Vec3 {
public:
	explicit Vec3( uint32_t a ) : address( a ) {
		v[0] = v[1] = v[2] = 0;
		if ( a ) {
			QAA_LoadVec3( a, v );
		}
	}
	~Vec3() {
		if ( address ) {
			QAA_StoreVec3( address, v );
		}
	}
	Vec3( const Vec3 & ) = delete;
	Vec3 &operator=( const Vec3 & ) = delete;
	float *p() { return address ? v : nullptr; }

private:
	uint32_t address;
	vec3_t v;
};

/* A guest array of 32-bit words (int or float): read before the call, written back after it. */
template <typename T>
class Words {
	static_assert( sizeof( T ) == 4, "32-bit words" );

public:
	Words( uint32_t a, int count ) : address( a ) {
		if ( a && count > 0 ) {
			host.resize( static_cast<size_t>( count ) );
			for ( size_t i = 0; i < host.size(); i++ ) {
				const uint32_t w = QAA_LoadU32( a + 4 * static_cast<uint32_t>( i ) );
				std::memcpy( &host[i], &w, 4 );
			}
		}
	}
	~Words() {
		for ( size_t i = 0; i < host.size(); i++ ) {
			uint32_t w;
			std::memcpy( &w, &host[i], 4 );
			QAA_StoreU32( address + 4 * static_cast<uint32_t>( i ), w );
		}
	}
	Words( const Words & ) = delete;
	Words &operator=( const Words & ) = delete;
	T *p() { return address ? host.data() : nullptr; }

private:
	uint32_t address;
	std::vector<T> host;
};

/* A guest char buffer: the botlib reads and writes it in place. */
static inline char *Chars( uint32_t a ) {
	return static_cast<char *>( QAA_Ptr( a ) );
}

/* ---- the entity structures (QAA's own) ------------------------------------ */

/*
 * QAA's bot_entitystate_t is 0xCC bytes: Quake III's 0x70, then nextthink,
 * itemQuantity, itemType, itemTag, health, maxHealth, ammoFull[16] (all as
 * Quake Live's) and carriesFlag at 0xC8. Quake Live's trainingClear is not
 * in it. **code** qagame 0x880278E8 clears 0xCC bytes and fills them, and
 * stores carriesFlag (PW_REDFLAG or PW_BLUEFLAG held) at 0xC8; default.xex's
 * AAS_UpdateEntity 0x821BEB30 copies state 0xC8 to info 0xE4.
 *
 * QAA's aas_entityinfo_t is 0xE8 bytes: Quake III's 0x8C, the same
 * extension, and carriesFlag at 0xE4. **code** default.xex's AAS_EntityInfo
 * 0x821BEEE8 copies 0xE8 bytes.
 */
constexpr uint32_t kStateWords = 0xC8;	// guest = host up to carriesFlag
constexpr uint32_t kStateCarriesFlag = 0xC8;
constexpr uint32_t kInfoWords = 0xE4;
constexpr uint32_t kInfoCarriesFlag = 0xE4;
static_assert( offsetof( bot_entitystate_t, trainingClear ) == kStateWords, "bot_entitystate_t: QAA lacks trainingClear" );
static_assert( offsetof( aas_entityinfo_t, trainingClear ) == kInfoWords, "aas_entityinfo_t: QAA lacks trainingClear" );

static int UpdateEntity( int entnum, uint32_t a ) {
	if ( !a ) {
		return botlib_export->BotLibUpdateEntity( entnum, nullptr );
	}
	bot_entitystate_t state{};
	FromGuest( &state, a, MakeLayout( kStateWords ) );
	state.carriesFlag = QAA_LoadS32( a + kStateCarriesFlag );
	// trainingClear stays 0: QAA has no such field, and FnQL's botlib only
	// carries it into aas_entityinfo_t, which QAA's layout leaves it out of
	return botlib_export->BotLibUpdateEntity( entnum, &state );
}

static void EntityInfo( int entnum, uint32_t a ) {
	if ( !a ) {
		return;
	}
	aas_entityinfo_t info{};
	botlib_export->aas.AAS_EntityInfo( entnum, &info );
	ToGuest( a, &info, MakeLayout( kInfoWords ) );
	QAA_StoreS32( a + kInfoCarriesFlag, info.carriesFlag );
}

}	// namespace qaa_bot

/* ---- the traps ------------------------------------------------------------ */

enum qaaBotlibTrap_t {
	QB_SETUP = 200, QB_SHUTDOWN, QB_LIBVAR_SET, QB_LIBVAR_GET, QB_PC_ADD_GLOBAL_DEFINE, QB_START_FRAME,
	QB_LOAD_MAP, QB_UPDATENTITY, QB_TEST, QB_GET_SNAPSHOT_ENTITY, QB_GET_CONSOLE_MESSAGE, QB_USER_COMMAND,

	QB_AAS_ENABLE_ROUTING_AREA = 300, QB_AAS_BBOX_AREAS, QB_AAS_AREA_INFO, QB_AAS_ENTITY_INFO, QB_AAS_INITIALIZED,
	QB_AAS_PRESENCE_TYPE_BOUNDING_BOX, QB_AAS_TIME, QB_AAS_POINT_AREA_NUM, QB_AAS_TRACE_AREAS, QB_AAS_POINT_CONTENTS,
	QB_AAS_NEXT_BSP_ENTITY, QB_AAS_VALUE_FOR_BSP_EPAIR_KEY, QB_AAS_VECTOR_FOR_BSP_EPAIR_KEY,
	QB_AAS_FLOAT_FOR_BSP_EPAIR_KEY, QB_AAS_INT_FOR_BSP_EPAIR_KEY, QB_AAS_AREA_REACHABILITY,
	QB_AAS_AREA_TRAVEL_TIME_TO_GOAL_AREA, QB_AAS_SWIMMING, QB_AAS_PREDICT_CLIENT_MOVEMENT,

	QB_EA_SAY = 400, QB_EA_SAY_TEAM, QB_EA_COMMAND, QB_EA_ACTION, QB_EA_GESTURE, QB_EA_TALK, QB_EA_ATTACK,
	QB_EA_USE, QB_EA_RESPAWN, QB_EA_CROUCH, QB_EA_MOVE_UP, QB_EA_MOVE_DOWN, QB_EA_MOVE_FORWARD, QB_EA_MOVE_BACK,
	QB_EA_MOVE_LEFT, QB_EA_MOVE_RIGHT, QB_EA_SELECT_WEAPON, QB_EA_JUMP, QB_EA_DELAYED_JUMP, QB_EA_MOVE,
	QB_EA_VIEW, QB_EA_END_REGULAR, QB_EA_GET_INPUT, QB_EA_RESET_INPUT,

	QB_AI_LOAD_CHARACTER = 500, QB_AI_FREE_CHARACTER, QB_AI_CHARACTERISTIC_FLOAT, QB_AI_CHARACTERISTIC_BFLOAT,
	QB_AI_CHARACTERISTIC_INTEGER, QB_AI_CHARACTERISTIC_BINTEGER, QB_AI_CHARACTERISTIC_STRING,
	QB_AI_ALLOC_CHAT_STATE, QB_AI_FREE_CHAT_STATE, QB_AI_QUEUE_CONSOLE_MESSAGE, QB_AI_REMOVE_CONSOLE_MESSAGE,
	QB_AI_NEXT_CONSOLE_MESSAGE, QB_AI_NUM_CONSOLE_MESSAGE, QB_AI_INITIAL_CHAT, QB_AI_REPLY_CHAT,
	QB_AI_CHAT_LENGTH, QB_AI_ENTER_CHAT, QB_AI_STRING_CONTAINS, QB_AI_FIND_MATCH, QB_AI_MATCH_VARIABLE,
	QB_AI_UNIFY_WHITE_SPACES, QB_AI_REPLACE_SYNONYMS, QB_AI_LOAD_CHAT_FILE, QB_AI_SET_CHAT_GENDER,
	QB_AI_SET_CHAT_NAME, QB_AI_RESET_GOAL_STATE, QB_AI_RESET_AVOID_GOALS, QB_AI_PUSH_GOAL, QB_AI_POP_GOAL,
	QB_AI_EMPTY_GOAL_STACK, QB_AI_DUMP_AVOID_GOALS, QB_AI_DUMP_GOAL_STACK, QB_AI_GOAL_NAME, QB_AI_GET_TOP_GOAL,
	QB_AI_GET_SECOND_GOAL, QB_AI_CHOOSE_LTG_ITEM, QB_AI_CHOOSE_NBG_ITEM, QB_AI_TOUCHING_GOAL,
	QB_AI_ITEM_GOAL_IN_VIS_BUT_NOT_VISIBLE, QB_AI_GET_LEVEL_ITEM_GOAL, QB_AI_AVOID_GOAL_TIME,
	QB_AI_INIT_LEVEL_ITEMS, QB_AI_UPDATE_ENTITY_ITEMS, QB_AI_LOAD_ITEM_WEIGHTS, QB_AI_FREE_ITEM_WEIGHTS,
	QB_AI_SAVE_GOAL_FUZZY_LOGIC, QB_AI_ALLOC_GOAL_STATE, QB_AI_FREE_GOAL_STATE, QB_AI_RESET_MOVE_STATE,
	QB_AI_MOVE_TO_GOAL, QB_AI_MOVE_IN_DIRECTION, QB_AI_RESET_AVOID_REACH, QB_AI_RESET_LAST_AVOID_REACH,
	QB_AI_REACHABILITY_AREA, QB_AI_MOVEMENT_VIEW_TARGET, QB_AI_ALLOC_MOVE_STATE, QB_AI_FREE_MOVE_STATE,
	QB_AI_INIT_MOVE_STATE, QB_AI_CHOOSE_BEST_FIGHT_WEAPON, QB_AI_GET_WEAPON_INFO, QB_AI_LOAD_WEAPON_WEIGHTS,
	QB_AI_ALLOC_WEAPON_STATE, QB_AI_FREE_WEAPON_STATE, QB_AI_RESET_WEAPON_STATE,
	QB_AI_GENETIC_PARENTS_AND_CHILD_SELECTION, QB_AI_INTERBREED_GOAL_FUZZY_LOGIC,
	QB_AI_MUTATE_GOAL_FUZZY_LOGIC, QB_AI_GET_NEXT_CAMP_SPOT_GOAL, QB_AI_GET_MAP_LOCATION_GOAL,
	QB_AI_NUM_INITIAL_CHATS, QB_AI_GET_CHAT_MESSAGE, QB_AI_REMOVE_FROM_AVOID_GOALS,
	QB_AI_PREDICT_VISIBLE_POSITION, QB_AI_SET_AVOID_GOAL_TIME, QB_AI_ADD_AVOID_SPOT,
	QB_AAS_ALTERNATIVE_ROUTE_GOAL, QB_AAS_PREDICT_ROUTE, QB_AAS_POINT_REACHABILITY_AREA_INDEX,
	QB_PC_LOAD_SOURCE, QB_PC_FREE_SOURCE, QB_PC_READ_TOKEN, QB_PC_SOURCE_FILE_AND_LINE,
};
static_assert( QB_SHUTDOWN == 201 && QB_USER_COMMAND == 211, "botlib traps 200.." );
static_assert( QB_AAS_PREDICT_CLIENT_MOVEMENT == 318, "botlib traps 300.." );
static_assert( QB_EA_RESET_INPUT == 423, "botlib traps 400.." );
static_assert( QB_AI_GET_WEAPON_INFO == 559 && QB_AI_ADD_AVOID_SPOT == 574 && QB_PC_SOURCE_FILE_AND_LINE == 581,
	"botlib traps 500.." );

/* MAX_ITEMS ints: the bot's inventory, as Quake III's bot_state_t holds it */
static constexpr int QAA_BOT_INVENTORY = MAX_ITEMS;

/*
 * The botlib traps, a[0] being the first argument. *handled is false for a
 * number QAA's engine does not handle (212..299, 319..399, 424..499).
 */
static int64_t QAA_BotlibSyscall( int trap, const int32_t *a, bool *handled ) {
	using namespace qaa_bot;
	using Goal = Struct<bot_goal_t>;
	const auto F = []( int32_t v ) { return QAA_ArgF( v ); };
	const auto ptr = []( int32_t v ) { return static_cast<uint32_t>( v ); };
	*handled = true;
	if ( trap == QB_SETUP ) {
		if ( botlib_export ) {
			// QAA: no chat AI, as retail (be_interface.cpp); libvars are freed at
			// every botlib shutdown, so this goes in before every setup
			botlib_export->BotLibVarSet( "qaa_nochatai", "1" );
		}
		return SV_BotLibSetup();	// -1 without a botlib, as retail (0x82131C28)
	}
	if ( !botlib_export ) {
		return 0;
	}

	switch ( trap ) {
	// QAA: retail's botlib stubs every chat-state call (its AI export table,
	// filled at default.xex 0x821A5020): these return 0 (0x8260F748) or do
	// nothing (0x826F9BC8), and leave their buffers as they were
	case QB_AI_ALLOC_CHAT_STATE:
	case QB_AI_NEXT_CONSOLE_MESSAGE:
	case QB_AI_NUM_CONSOLE_MESSAGE:
	case QB_AI_NUM_INITIAL_CHATS:
	case QB_AI_REPLY_CHAT:
	case QB_AI_CHAT_LENGTH:
	case QB_AI_LOAD_CHAT_FILE:
	case QB_AI_FREE_CHAT_STATE:
	case QB_AI_QUEUE_CONSOLE_MESSAGE:
	case QB_AI_REMOVE_CONSOLE_MESSAGE:
	case QB_AI_INITIAL_CHAT:
	case QB_AI_ENTER_CHAT:
	case QB_AI_GET_CHAT_MESSAGE:
	case QB_AI_SET_CHAT_GENDER:
	case QB_AI_SET_CHAT_NAME:
		return 0;
	default:
		break;
	}
	auto &aas = botlib_export->aas;
	auto &ea = botlib_export->ea;
	auto &ai = botlib_export->ai;

	switch ( trap ) {
	case QB_SHUTDOWN:
		return SV_BotLibShutdown();
	case QB_LIBVAR_SET:
		return botlib_export->BotLibVarSet( QAA_Str( a[0] ), QAA_Str( a[1] ) );
	case QB_LIBVAR_GET:
		return botlib_export->BotLibVarGet( QAA_Str( a[0] ), Chars( a[1] ), a[2] );
	case QB_PC_ADD_GLOBAL_DEFINE:
		return botlib_export->PC_AddGlobalDefine( Chars( a[0] ) );
	case QB_PC_LOAD_SOURCE:
		return botlib_export->PC_LoadSourceHandle( QAA_Str( a[0] ) );
	case QB_PC_FREE_SOURCE:
		return botlib_export->PC_FreeSourceHandle( a[0] );
	case QB_PC_READ_TOKEN: {
		Struct<pc_token_t> token( ptr( a[1] ), kToken );
		return botlib_export->PC_ReadTokenHandle( a[0], token.p() );
	}
	case QB_PC_SOURCE_FILE_AND_LINE: {
		Words<int> line( ptr( a[2] ), 1 );
		return botlib_export->PC_SourceFileAndLine( a[0], Chars( a[1] ), line.p() );
	}

	case QB_START_FRAME:
		return botlib_export->BotLibStartFrame( F( a[0] ) );
	case QB_LOAD_MAP:
		return botlib_export->BotLibLoadMap( QAA_Str( a[0] ) );
	case QB_UPDATENTITY:
		return UpdateEntity( a[0], ptr( a[1] ) );
	case QB_TEST: {
		Vec3 p2( ptr( a[2] ) ), p3( ptr( a[3] ) );
		return botlib_export->Test( a[0], Chars( a[1] ), p2.p(), p3.p() );
	}
	case QB_GET_SNAPSHOT_ENTITY:
		return SV_BotGetSnapshotEntity( a[0], a[1] );
	case QB_GET_CONSOLE_MESSAGE:
		return SV_BotGetConsoleMessage( a[0], Chars( a[1] ), a[2] );
	case QB_USER_COMMAND:
		if ( SV_IsClientIndex( a[0] ) && a[1] ) {
			usercmd_t cmd;
			QAA_UsercmdFromGuest( ptr( a[1] ), &cmd );
			SV_ClientThink( &SV_ClientForIndex( a[0] ), &cmd );
		}
		return 0;

	case QB_AAS_ENTITY_INFO:
		EntityInfo( a[0], ptr( a[1] ) );
		return 0;
	case QB_AAS_INITIALIZED:
		return aas.AAS_Initialized();
	case QB_AAS_PRESENCE_TYPE_BOUNDING_BOX: {
		Vec3 mins( ptr( a[1] ) ), maxs( ptr( a[2] ) );
		aas.AAS_PresenceTypeBoundingBox( a[0], mins.p(), maxs.p() );
		return 0;
	}
	case QB_AAS_TIME:
		return QAA_RetF( aas.AAS_Time() );
	case QB_AAS_POINT_AREA_NUM: {
		Vec3 point( ptr( a[0] ) );
		return aas.AAS_PointAreaNum( point.p() );
	}
	case QB_AAS_POINT_REACHABILITY_AREA_INDEX: {
		Vec3 point( ptr( a[0] ) );
		return aas.AAS_PointReachabilityAreaIndex( point.p() );
	}
	case QB_AAS_TRACE_AREAS: {
		Vec3 start( ptr( a[0] ) ), end( ptr( a[1] ) );
		Words<int> areas( ptr( a[2] ), a[4] );
		Words<float> points( ptr( a[3] ), a[4] * 3 );
		return aas.AAS_TraceAreas( start.p(), end.p(), areas.p(), reinterpret_cast<vec3_t *>( points.p() ), a[4] );
	}
	case QB_AAS_BBOX_AREAS: {
		Vec3 mins( ptr( a[0] ) ), maxs( ptr( a[1] ) );
		Words<int> areas( ptr( a[2] ), a[3] );
		return aas.AAS_BBoxAreas( mins.p(), maxs.p(), areas.p(), a[3] );
	}
	case QB_AAS_AREA_INFO: {
		Struct<aas_areainfo_t> info( ptr( a[1] ), kAreaInfo );
		return aas.AAS_AreaInfo( a[0], info.p() );
	}
	case QB_AAS_POINT_CONTENTS: {
		Vec3 point( ptr( a[0] ) );
		return aas.AAS_PointContents( point.p() );
	}
	case QB_AAS_NEXT_BSP_ENTITY:
		return aas.AAS_NextBSPEntity( a[0] );
	case QB_AAS_VALUE_FOR_BSP_EPAIR_KEY:
		return aas.AAS_ValueForBSPEpairKey( a[0], Chars( a[1] ), Chars( a[2] ), a[3] );
	case QB_AAS_VECTOR_FOR_BSP_EPAIR_KEY: {
		Vec3 v( ptr( a[2] ) );
		return aas.AAS_VectorForBSPEpairKey( a[0], Chars( a[1] ), v.p() );
	}
	case QB_AAS_FLOAT_FOR_BSP_EPAIR_KEY: {
		Words<float> value( ptr( a[2] ), 1 );
		return aas.AAS_FloatForBSPEpairKey( a[0], Chars( a[1] ), value.p() );
	}
	case QB_AAS_INT_FOR_BSP_EPAIR_KEY: {
		Words<int> value( ptr( a[2] ), 1 );
		return aas.AAS_IntForBSPEpairKey( a[0], Chars( a[1] ), value.p() );
	}
	case QB_AAS_AREA_REACHABILITY:
		return aas.AAS_AreaReachability( a[0] );
	case QB_AAS_AREA_TRAVEL_TIME_TO_GOAL_AREA: {
		Vec3 origin( ptr( a[1] ) );
		return aas.AAS_AreaTravelTimeToGoalArea( a[0], origin.p(), a[2], a[3] );
	}
	case QB_AAS_ENABLE_ROUTING_AREA:
		return aas.AAS_EnableRoutingArea( a[0], a[1] );
	case QB_AAS_PREDICT_ROUTE: {
		Struct<aas_predictroute_t> route( ptr( a[0] ), kPredictRoute );
		Vec3 origin( ptr( a[2] ) );
		return aas.AAS_PredictRoute( route.p(), a[1], origin.p(), a[3], a[4], a[5], a[6], a[7], a[8], a[9], a[10] );
	}
	case QB_AAS_ALTERNATIVE_ROUTE_GOAL: {
		Vec3 start( ptr( a[0] ) ), goal( ptr( a[2] ) );
		Struct<aas_altroutegoal_t, 256> goals( ptr( a[5] ), kAltRouteGoal, a[6] );
		return aas.AAS_AlternativeRouteGoals( start.p(), a[1], goal.p(), a[3], a[4], goals.p(), goals.Count(), a[7] );
	}
	case QB_AAS_SWIMMING: {
		Vec3 origin( ptr( a[0] ) );
		return aas.AAS_Swimming( origin.p() );
	}
	case QB_AAS_PREDICT_CLIENT_MOVEMENT: {
		Struct<aas_clientmove_t> move( ptr( a[0] ), kClientMove );
		Vec3 origin( ptr( a[2] ) ), velocity( ptr( a[5] ) ), cmdmove( ptr( a[6] ) );
		return aas.AAS_PredictClientMovement( move.p(), a[1], origin.p(), a[3], a[4], velocity.p(), cmdmove.p(),
			a[7], a[8], F( a[9] ), a[10], a[11], a[12] );
	}

	case QB_EA_SAY:
		ea.EA_Say( a[0], Chars( a[1] ) );
		return 0;
	case QB_EA_SAY_TEAM:
		ea.EA_SayTeam( a[0], Chars( a[1] ) );
		return 0;
	case QB_EA_COMMAND:
		ea.EA_Command( a[0], Chars( a[1] ) );
		return 0;
	case QB_EA_ACTION:
		ea.EA_Action( a[0], a[1] );
		return 0;
	case QB_EA_GESTURE:
		ea.EA_Gesture( a[0] );
		return 0;
	case QB_EA_TALK:
		ea.EA_Talk( a[0] );
		return 0;
	case QB_EA_ATTACK:
		ea.EA_Attack( a[0] );
		return 0;
	case QB_EA_USE:
		ea.EA_Use( a[0] );
		return 0;
	case QB_EA_RESPAWN:
		ea.EA_Respawn( a[0] );
		return 0;
	case QB_EA_CROUCH:
		ea.EA_Crouch( a[0] );
		return 0;
	case QB_EA_MOVE_UP:
		ea.EA_MoveUp( a[0] );
		return 0;
	case QB_EA_MOVE_DOWN:
		ea.EA_MoveDown( a[0] );
		return 0;
	case QB_EA_MOVE_FORWARD:
		ea.EA_MoveForward( a[0] );
		return 0;
	case QB_EA_MOVE_BACK:
		ea.EA_MoveBack( a[0] );
		return 0;
	case QB_EA_MOVE_LEFT:
		ea.EA_MoveLeft( a[0] );
		return 0;
	case QB_EA_MOVE_RIGHT:
		ea.EA_MoveRight( a[0] );
		return 0;
	case QB_EA_SELECT_WEAPON:
		ea.EA_SelectWeapon( a[0], a[1] );
		return 0;
	case QB_EA_JUMP:
		ea.EA_Jump( a[0] );
		return 0;
	case QB_EA_DELAYED_JUMP:
		ea.EA_DelayedJump( a[0] );
		return 0;
	case QB_EA_MOVE: {
		Vec3 dir( ptr( a[1] ) );
		ea.EA_Move( a[0], dir.p(), F( a[2] ) );
		return 0;
	}
	case QB_EA_VIEW: {
		Vec3 viewangles( ptr( a[1] ) );
		ea.EA_View( a[0], viewangles.p() );
		return 0;
	}
	case QB_EA_END_REGULAR:
		ea.EA_EndRegular( a[0], F( a[1] ) );
		return 0;
	case QB_EA_GET_INPUT: {
		Struct<bot_input_t> input( ptr( a[2] ), kInput );
		ea.EA_GetInput( a[0], F( a[1] ), input.p() );
		return 0;
	}
	case QB_EA_RESET_INPUT:
		ea.EA_ResetInput( a[0] );
		return 0;

	case QB_AI_LOAD_CHARACTER:
		return ai.BotLoadCharacter( Chars( a[0] ), F( a[1] ) );
	case QB_AI_FREE_CHARACTER:
		ai.BotFreeCharacter( a[0] );
		return 0;
	case QB_AI_CHARACTERISTIC_FLOAT:
		return QAA_RetF( ai.Characteristic_Float( a[0], a[1] ) );
	case QB_AI_CHARACTERISTIC_BFLOAT:
		return QAA_RetF( ai.Characteristic_BFloat( a[0], a[1], F( a[2] ), F( a[3] ) ) );
	case QB_AI_CHARACTERISTIC_INTEGER:
		return ai.Characteristic_Integer( a[0], a[1] );
	case QB_AI_CHARACTERISTIC_BINTEGER:
		return ai.Characteristic_BInteger( a[0], a[1], a[2], a[3] );
	case QB_AI_CHARACTERISTIC_STRING:
		ai.Characteristic_String( a[0], a[1], Chars( a[2] ), a[3] );
		return 0;

	case QB_AI_STRING_CONTAINS:
		return ai.StringContains( Chars( a[0] ), Chars( a[1] ), a[2] );
	case QB_AI_FIND_MATCH: {
		Struct<bot_match_t> match( ptr( a[1] ), kMatch );
		return ai.BotFindMatch( Chars( a[0] ), match.p(), static_cast<uint32_t>( a[2] ) );
	}
	case QB_AI_MATCH_VARIABLE: {
		Struct<bot_match_t> match( ptr( a[0] ), kMatch );
		ai.BotMatchVariable( match.p(), a[1], Chars( a[2] ), a[3] );
		return 0;
	}
	case QB_AI_UNIFY_WHITE_SPACES:
		ai.UnifyWhiteSpaces( Chars( a[0] ) );
		return 0;
	case QB_AI_REPLACE_SYNONYMS:
		// Quake III's call has no size. Without the chat AI no synonyms are
		// loaded, so the string never grows; the bound is Quake III's chat size
		ai.BotReplaceSynonyms( Chars( a[0] ), MAX_MESSAGE_SIZE, static_cast<uint32_t>( a[1] ) );
		return 0;
	case QB_AI_RESET_GOAL_STATE:
		ai.BotResetGoalState( a[0] );
		return 0;
	case QB_AI_RESET_AVOID_GOALS:
		ai.BotResetAvoidGoals( a[0] );
		return 0;
	case QB_AI_REMOVE_FROM_AVOID_GOALS:
		ai.BotRemoveFromAvoidGoals( a[0], a[1] );
		return 0;
	case QB_AI_PUSH_GOAL: {
		Goal goal( ptr( a[1] ), kGoal );
		ai.BotPushGoal( a[0], goal.p() );
		return 0;
	}
	case QB_AI_POP_GOAL:
		ai.BotPopGoal( a[0] );
		return 0;
	case QB_AI_EMPTY_GOAL_STACK:
		ai.BotEmptyGoalStack( a[0] );
		return 0;
	case QB_AI_DUMP_AVOID_GOALS:
		ai.BotDumpAvoidGoals( a[0] );
		return 0;
	case QB_AI_DUMP_GOAL_STACK:
		ai.BotDumpGoalStack( a[0] );
		return 0;
	case QB_AI_GOAL_NAME:
		ai.BotGoalName( a[0], Chars( a[1] ), a[2] );
		return 0;
	case QB_AI_GET_TOP_GOAL: {
		Goal goal( ptr( a[1] ), kGoal );
		return ai.BotGetTopGoal( a[0], goal.p() );
	}
	case QB_AI_GET_SECOND_GOAL: {
		Goal goal( ptr( a[1] ), kGoal );
		return ai.BotGetSecondGoal( a[0], goal.p() );
	}
	case QB_AI_CHOOSE_LTG_ITEM: {
		Vec3 origin( ptr( a[1] ) );
		Words<int> inventory( ptr( a[2] ), QAA_BOT_INVENTORY );
		return ai.BotChooseLTGItem( a[0], origin.p(), inventory.p(), a[3] );
	}
	case QB_AI_CHOOSE_NBG_ITEM: {
		Vec3 origin( ptr( a[1] ) );
		Words<int> inventory( ptr( a[2] ), QAA_BOT_INVENTORY );
		Goal ltg( ptr( a[4] ), kGoal );
		return ai.BotChooseNBGItem( a[0], origin.p(), inventory.p(), a[3], ltg.p(), F( a[5] ) );
	}
	case QB_AI_TOUCHING_GOAL: {
		Vec3 origin( ptr( a[0] ) );
		Goal goal( ptr( a[1] ), kGoal );
		return ai.BotTouchingGoal( origin.p(), goal.p() );
	}
	case QB_AI_ITEM_GOAL_IN_VIS_BUT_NOT_VISIBLE: {
		Vec3 eye( ptr( a[1] ) ), viewangles( ptr( a[2] ) );
		Goal goal( ptr( a[3] ), kGoal );
		return ai.BotItemGoalInVisButNotVisible( a[0], eye.p(), viewangles.p(), goal.p() );
	}
	case QB_AI_GET_LEVEL_ITEM_GOAL: {
		Goal goal( ptr( a[2] ), kGoal );
		return ai.BotGetLevelItemGoal( a[0], Chars( a[1] ), goal.p() );
	}
	case QB_AI_GET_NEXT_CAMP_SPOT_GOAL: {
		Goal goal( ptr( a[1] ), kGoal );
		return ai.BotGetNextCampSpotGoal( a[0], goal.p() );
	}
	case QB_AI_GET_MAP_LOCATION_GOAL: {
		Goal goal( ptr( a[1] ), kGoal );
		return ai.BotGetMapLocationGoal( Chars( a[0] ), goal.p() );
	}
	case QB_AI_AVOID_GOAL_TIME:
		return QAA_RetF( ai.BotAvoidGoalTime( a[0], a[1] ) );
	case QB_AI_SET_AVOID_GOAL_TIME:
		ai.BotSetAvoidGoalTime( a[0], a[1], F( a[2] ) );
		return 0;
	case QB_AI_INIT_LEVEL_ITEMS:
		ai.BotInitLevelItems();
		return 0;
	case QB_AI_UPDATE_ENTITY_ITEMS:
		ai.BotUpdateEntityItems();
		return 0;
	case QB_AI_LOAD_ITEM_WEIGHTS:
		return ai.BotLoadItemWeights( a[0], Chars( a[1] ) );
	case QB_AI_FREE_ITEM_WEIGHTS:
		ai.BotFreeItemWeights( a[0] );
		return 0;
	case QB_AI_INTERBREED_GOAL_FUZZY_LOGIC:
		ai.BotInterbreedGoalFuzzyLogic( a[0], a[1], a[2] );
		return 0;
	case QB_AI_SAVE_GOAL_FUZZY_LOGIC:
		ai.BotSaveGoalFuzzyLogic( a[0], Chars( a[1] ) );
		return 0;
	case QB_AI_MUTATE_GOAL_FUZZY_LOGIC:
		ai.BotMutateGoalFuzzyLogic( a[0], F( a[1] ) );
		return 0;
	case QB_AI_ALLOC_GOAL_STATE:
		return ai.BotAllocGoalState( a[0] );
	case QB_AI_FREE_GOAL_STATE:
		ai.BotFreeGoalState( a[0] );
		return 0;

	case QB_AI_RESET_MOVE_STATE:
		ai.BotResetMoveState( a[0] );
		return 0;
	case QB_AI_ADD_AVOID_SPOT: {
		Vec3 origin( ptr( a[1] ) );
		ai.BotAddAvoidSpot( a[0], origin.p(), F( a[2] ), a[3] );
		return 0;
	}
	case QB_AI_MOVE_TO_GOAL: {
		Struct<bot_moveresult_t> result( ptr( a[0] ), kMoveResult );
		Goal goal( ptr( a[2] ), kGoal );
		ai.BotMoveToGoal( result.p(), a[1], goal.p(), a[3] );
		return 0;
	}
	case QB_AI_MOVE_IN_DIRECTION: {
		Vec3 dir( ptr( a[1] ) );
		return ai.BotMoveInDirection( a[0], dir.p(), F( a[2] ), a[3] );
	}
	case QB_AI_RESET_AVOID_REACH:
		ai.BotResetAvoidReach( a[0] );
		return 0;
	case QB_AI_RESET_LAST_AVOID_REACH:
		ai.BotResetLastAvoidReach( a[0] );
		return 0;
	case QB_AI_REACHABILITY_AREA: {
		Vec3 origin( ptr( a[0] ) );
		return ai.BotReachabilityArea( origin.p(), a[1] );
	}
	case QB_AI_MOVEMENT_VIEW_TARGET: {
		Goal goal( ptr( a[1] ), kGoal );
		Vec3 target( ptr( a[4] ) );
		return ai.BotMovementViewTarget( a[0], goal.p(), a[2], F( a[3] ), target.p() );
	}
	case QB_AI_PREDICT_VISIBLE_POSITION: {
		Vec3 origin( ptr( a[0] ) ), target( ptr( a[4] ) );
		Goal goal( ptr( a[2] ), kGoal );
		return ai.BotPredictVisiblePosition( origin.p(), a[1], goal.p(), a[3], target.p() );
	}
	case QB_AI_ALLOC_MOVE_STATE:
		return ai.BotAllocMoveState();
	case QB_AI_FREE_MOVE_STATE:
		ai.BotFreeMoveState( a[0] );
		return 0;
	case QB_AI_INIT_MOVE_STATE: {
		Struct<bot_initmove_t> initmove( ptr( a[1] ), kInitMove );
		ai.BotInitMoveState( a[0], initmove.p() );
		return 0;
	}

	case QB_AI_CHOOSE_BEST_FIGHT_WEAPON: {
		Words<int> inventory( ptr( a[1] ), QAA_BOT_INVENTORY );
		return ai.BotChooseBestFightWeapon( a[0], inventory.p() );
	}
	case QB_AI_GET_WEAPON_INFO: {
		Struct<weaponinfo_t> info( ptr( a[2] ), kWeaponInfo );
		ai.BotGetWeaponInfo( a[0], a[1], info.p() );
		return 0;
	}
	case QB_AI_LOAD_WEAPON_WEIGHTS:
		return ai.BotLoadWeaponWeights( a[0], Chars( a[1] ) );
	case QB_AI_ALLOC_WEAPON_STATE:
		return ai.BotAllocWeaponState();
	case QB_AI_FREE_WEAPON_STATE:
		ai.BotFreeWeaponState( a[0] );
		return 0;
	case QB_AI_RESET_WEAPON_STATE:
		ai.BotResetWeaponState( a[0] );
		return 0;

	case QB_AI_GENETIC_PARENTS_AND_CHILD_SELECTION: {
		Words<float> ranks( ptr( a[1] ), a[0] );
		Words<int> parent1( ptr( a[2] ), 1 ), parent2( ptr( a[3] ), 1 ), child( ptr( a[4] ), 1 );
		return ai.GeneticParentsAndChildSelection( a[0], ranks.p(), parent1.p(), parent2.p(), child.p() );
	}

	default:
		*handled = false;
		return 0;
	}
}
