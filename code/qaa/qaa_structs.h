/*
===========================================================================
FnQAA: QAA's shared structures, as the guest lays them out.

Copyright (C) 2026 FnQAA contributors.
This file is part of FnQAA, licensed under the GNU GPL v2 or later.

QAA's game modules use Quake III 1.32's structures, big-endian with 32-bit
pointers; the engine uses FnQL's (Quake Live's). These are the guest
offsets (docs/ABI.md, data/abi/structs.tsv) and the conversions between the
two, for the game (sv_qaa_game.inc) and cgame (cl_qaa_cgame.inc) bridges.

Include after q_shared.h.
===========================================================================
*/
#ifndef QAA_STRUCTS_H
#define QAA_STRUCTS_H

#include "qaa_guest_host.h"

#include <cmath>
#include <vector>

/* ---- guest layouts (Quake III 1.32, big-endian) ------------------------ */

namespace qaa_layout {
// trajectory_t, 0x24
constexpr uint32_t oTR_TYPE = 0x00, oTR_TIME = 0x04, oTR_DURATION = 0x08, oTR_BASE = 0x0C, oTR_DELTA = 0x18;
// entityState_t, 0xD0
constexpr uint32_t oES_SIZE = 0xD0;
constexpr uint32_t oES_NUMBER = 0x00, oES_ETYPE = 0x04, oES_EFLAGS = 0x08, oES_POS = 0x0C, oES_APOS = 0x30,
	oES_TIME = 0x54, oES_TIME2 = 0x58, oES_ORIGIN = 0x5C, oES_ORIGIN2 = 0x68, oES_ANGLES = 0x74, oES_ANGLES2 = 0x80,
	oES_OTHERENTITYNUM = 0x8C, oES_OTHERENTITYNUM2 = 0x90, oES_GROUNDENTITYNUM = 0x94, oES_CONSTANTLIGHT = 0x98,
	oES_LOOPSOUND = 0x9C, oES_MODELINDEX = 0xA0, oES_MODELINDEX2 = 0xA4, oES_CLIENTNUM = 0xA8, oES_FRAME = 0xAC,
	oES_SOLID = 0xB0, oES_EVENT = 0xB4, oES_EVENTPARM = 0xB8, oES_POWERUPS = 0xBC, oES_WEAPON = 0xC0,
	oES_LEGSANIM = 0xC4, oES_TORSOANIM = 0xC8, oES_GENERIC1 = 0xCC;
// sharedEntity_t, 0x204: s at 0, r (entityShared_t, 0x134) at 0xD0, whose
// first 0xD0 bytes are Quake III's unused entityState_t
constexpr uint32_t oSE_R = 0xD0;
constexpr uint32_t oR_LINKED = 0xD0, oR_LINKCOUNT = 0xD4, oR_SVFLAGS = 0xD8, oR_SINGLECLIENT = 0xDC, oR_BMODEL = 0xE0,
	oR_MINS = 0xE4, oR_MAXS = 0xF0, oR_CONTENTS = 0xFC, oR_ABSMIN = 0x100, oR_ABSMAX = 0x10C,
	oR_CURRENTORIGIN = 0x118, oR_CURRENTANGLES = 0x124, oR_OWNERNUM = 0x130;
// playerState_t, 0x1D8
constexpr uint32_t oPS_SIZE = 0x1D8;
constexpr uint32_t oPS_COMMANDTIME = 0x00, oPS_PM_TYPE = 0x04, oPS_BOBCYCLE = 0x08, oPS_PM_FLAGS = 0x0C,
	oPS_PM_TIME = 0x10, oPS_ORIGIN = 0x14, oPS_VELOCITY = 0x20, oPS_WEAPONTIME = 0x2C, oPS_GRAVITY = 0x30,
	oPS_SPEED = 0x34, oPS_DELTA_ANGLES = 0x38, oPS_GROUNDENTITYNUM = 0x44, oPS_LEGSTIMER = 0x48, oPS_LEGSANIM = 0x4C,
	oPS_TORSOTIMER = 0x50, oPS_TORSOANIM = 0x54, oPS_MOVEMENTDIR = 0x58, oPS_GRAPPLEPOINT = 0x5C, oPS_EFLAGS = 0x68,
	oPS_EVENTSEQUENCE = 0x6C, oPS_EVENTS = 0x70, oPS_EVENTPARMS = 0x78, oPS_EXTERNALEVENT = 0x80,
	oPS_EXTERNALEVENTPARM = 0x84, oPS_EXTERNALEVENTTIME = 0x88, oPS_CLIENTNUM = 0x8C, oPS_WEAPON = 0x90,
	oPS_WEAPONSTATE = 0x94, oPS_VIEWANGLES = 0x98, oPS_VIEWHEIGHT = 0xA4, oPS_DAMAGEEVENT = 0xA8, oPS_DAMAGEYAW = 0xAC,
	oPS_DAMAGEPITCH = 0xB0, oPS_DAMAGECOUNT = 0xB4, oPS_STATS = 0xB8, oPS_PERSISTANT = 0xF8, oPS_POWERUPS = 0x138,
	oPS_AMMO = 0x178, oPS_GENERIC1 = 0x1B8, oPS_LOOPSOUND = 0x1BC, oPS_JUMPPAD_ENT = 0x1C0, oPS_PING = 0x1C4,
	oPS_PMOVE_FRAMECOUNT = 0x1C8, oPS_JUMPPAD_FRAME = 0x1CC, oPS_ENTITYEVENTSEQUENCE = 0x1D0,
	oPS_QAA_WEAPONSDISABLED = 0x1D4;
constexpr int oPS_ARRAY = 16;	// stats, persistant, powerups, ammo
// usercmd_t, 0x18
constexpr uint32_t oUC_SERVERTIME = 0x00, oUC_ANGLES = 0x04, oUC_BUTTONS = 0x10, oUC_WEAPON = 0x14,
	oUC_FORWARDMOVE = 0x15, oUC_RIGHTMOVE = 0x16, oUC_UPMOVE = 0x17;
// trace_t, 0x38
constexpr uint32_t oTRACE_ALLSOLID = 0x00, oTRACE_STARTSOLID = 0x04, oTRACE_FRACTION = 0x08, oTRACE_ENDPOS = 0x0C,
	oTRACE_PLANE_NORMAL = 0x18, oTRACE_PLANE_DIST = 0x24, oTRACE_PLANE_TYPE = 0x28, oTRACE_PLANE_SIGNBITS = 0x29,
	oTRACE_SURFACEFLAGS = 0x2C, oTRACE_CONTENTS = 0x30, oTRACE_ENTITYNUM = 0x34;
// vmCvar_t, 0x110: handle, modificationCount, value, integer, string[256]
constexpr uint32_t oCVAR_HEADER_WORDS = 4, oCVAR_STRING = 0x10, oCVAR_SIZE = 0x110;
}

/* ---- conversions -------------------------------------------------------- */

static inline void QAA_TrajectoryFromGuest( uint32_t a, trajectory_t *t ) {
	using namespace qaa_layout;
	t->trType = static_cast<trType_t>( QAA_LoadS32( a + oTR_TYPE ) );
	t->trTime = QAA_LoadS32( a + oTR_TIME );
	t->trDuration = QAA_LoadS32( a + oTR_DURATION );
	QAA_LoadVec3( a + oTR_BASE, t->trBase );
	QAA_LoadVec3( a + oTR_DELTA, t->trDelta );
	t->gravity = 0;	// Quake Live's per-trajectory gravity: not in QAA
}

static inline void QAA_TrajectoryToGuest( uint32_t a, const trajectory_t *t ) {
	using namespace qaa_layout;
	QAA_StoreS32( a + oTR_TYPE, t->trType );
	QAA_StoreS32( a + oTR_TIME, t->trTime );
	QAA_StoreS32( a + oTR_DURATION, t->trDuration );
	QAA_StoreVec3( a + oTR_BASE, t->trBase );
	QAA_StoreVec3( a + oTR_DELTA, t->trDelta );
}

static inline void QAA_EntityStateFromGuest( uint32_t a, entityState_t *s ) {
	using namespace qaa_layout;
	Com_Memset( s, 0, sizeof( *s ) );	// Quake Live's fields (health, armor, location, jumps) stay zero
	s->number = QAA_LoadS32( a + oES_NUMBER );
	s->eType = QAA_LoadS32( a + oES_ETYPE );
	s->eFlags = QAA_LoadS32( a + oES_EFLAGS );
	QAA_TrajectoryFromGuest( a + oES_POS, &s->pos );
	QAA_TrajectoryFromGuest( a + oES_APOS, &s->apos );
	s->time = QAA_LoadS32( a + oES_TIME );
	s->time2 = QAA_LoadS32( a + oES_TIME2 );
	QAA_LoadVec3( a + oES_ORIGIN, s->origin );
	QAA_LoadVec3( a + oES_ORIGIN2, s->origin2 );
	QAA_LoadVec3( a + oES_ANGLES, s->angles );
	QAA_LoadVec3( a + oES_ANGLES2, s->angles2 );
	s->otherEntityNum = QAA_LoadS32( a + oES_OTHERENTITYNUM );
	s->otherEntityNum2 = QAA_LoadS32( a + oES_OTHERENTITYNUM2 );
	s->groundEntityNum = QAA_LoadS32( a + oES_GROUNDENTITYNUM );
	s->constantLight = QAA_LoadS32( a + oES_CONSTANTLIGHT );
	s->loopSound = QAA_LoadS32( a + oES_LOOPSOUND );
	s->modelindex = QAA_LoadS32( a + oES_MODELINDEX );
	s->modelindex2 = QAA_LoadS32( a + oES_MODELINDEX2 );
	s->clientNum = QAA_LoadS32( a + oES_CLIENTNUM );
	s->frame = QAA_LoadS32( a + oES_FRAME );
	s->solid = QAA_LoadS32( a + oES_SOLID );
	s->event = QAA_LoadS32( a + oES_EVENT );
	s->eventParm = QAA_LoadS32( a + oES_EVENTPARM );
	s->powerups = QAA_LoadS32( a + oES_POWERUPS );
	s->weapon = QAA_LoadS32( a + oES_WEAPON );
	s->legsAnim = QAA_LoadS32( a + oES_LEGSANIM );
	s->torsoAnim = QAA_LoadS32( a + oES_TORSOANIM );
	s->generic1 = QAA_LoadS32( a + oES_GENERIC1 );
}

static inline void QAA_EntityStateToGuest( uint32_t a, const entityState_t *s ) {
	using namespace qaa_layout;
	QAA_StoreS32( a + oES_NUMBER, s->number );
	QAA_StoreS32( a + oES_ETYPE, s->eType );
	QAA_StoreS32( a + oES_EFLAGS, s->eFlags );
	QAA_TrajectoryToGuest( a + oES_POS, &s->pos );
	QAA_TrajectoryToGuest( a + oES_APOS, &s->apos );
	QAA_StoreS32( a + oES_TIME, s->time );
	QAA_StoreS32( a + oES_TIME2, s->time2 );
	QAA_StoreVec3( a + oES_ORIGIN, s->origin );
	QAA_StoreVec3( a + oES_ORIGIN2, s->origin2 );
	QAA_StoreVec3( a + oES_ANGLES, s->angles );
	QAA_StoreVec3( a + oES_ANGLES2, s->angles2 );
	QAA_StoreS32( a + oES_OTHERENTITYNUM, s->otherEntityNum );
	QAA_StoreS32( a + oES_OTHERENTITYNUM2, s->otherEntityNum2 );
	QAA_StoreS32( a + oES_GROUNDENTITYNUM, s->groundEntityNum );
	QAA_StoreS32( a + oES_CONSTANTLIGHT, s->constantLight );
	QAA_StoreS32( a + oES_LOOPSOUND, s->loopSound );
	QAA_StoreS32( a + oES_MODELINDEX, s->modelindex );
	QAA_StoreS32( a + oES_MODELINDEX2, s->modelindex2 );
	QAA_StoreS32( a + oES_CLIENTNUM, s->clientNum );
	QAA_StoreS32( a + oES_FRAME, s->frame );
	QAA_StoreS32( a + oES_SOLID, s->solid );
	QAA_StoreS32( a + oES_EVENT, s->event );
	QAA_StoreS32( a + oES_EVENTPARM, s->eventParm );
	QAA_StoreS32( a + oES_POWERUPS, s->powerups );
	QAA_StoreS32( a + oES_WEAPON, s->weapon );
	QAA_StoreS32( a + oES_LEGSANIM, s->legsAnim );
	QAA_StoreS32( a + oES_TORSOANIM, s->torsoAnim );
	QAA_StoreS32( a + oES_GENERIC1, s->generic1 );
}

static inline void QAA_PlayerStateFromGuest( uint32_t a, playerState_t *ps ) {
	using namespace qaa_layout;
	Com_Memset( ps, 0, sizeof( *ps ) );	// Quake Live's fields stay zero
	ps->commandTime = QAA_LoadS32( a + oPS_COMMANDTIME );
	ps->pm_type = QAA_LoadS32( a + oPS_PM_TYPE );
	ps->bobCycle = QAA_LoadS32( a + oPS_BOBCYCLE );
	ps->pm_flags = QAA_LoadS32( a + oPS_PM_FLAGS );
	ps->pm_time = QAA_LoadS32( a + oPS_PM_TIME );
	QAA_LoadVec3( a + oPS_ORIGIN, ps->origin );
	QAA_LoadVec3( a + oPS_VELOCITY, ps->velocity );
	ps->weaponTime = QAA_LoadS32( a + oPS_WEAPONTIME );
	ps->gravity = QAA_LoadS32( a + oPS_GRAVITY );
	ps->speed = QAA_LoadS32( a + oPS_SPEED );
	for ( int i = 0; i < 3; i++ ) {
		ps->delta_angles[i] = QAA_LoadS32( a + oPS_DELTA_ANGLES + 4 * i );
	}
	ps->groundEntityNum = QAA_LoadS32( a + oPS_GROUNDENTITYNUM );
	ps->legsTimer = QAA_LoadS32( a + oPS_LEGSTIMER );
	ps->legsAnim = QAA_LoadS32( a + oPS_LEGSANIM );
	ps->torsoTimer = QAA_LoadS32( a + oPS_TORSOTIMER );
	ps->torsoAnim = QAA_LoadS32( a + oPS_TORSOANIM );
	ps->movementDir = QAA_LoadS32( a + oPS_MOVEMENTDIR );
	QAA_LoadVec3( a + oPS_GRAPPLEPOINT, ps->grapplePoint );
	ps->eFlags = QAA_LoadS32( a + oPS_EFLAGS );
	ps->eventSequence = QAA_LoadS32( a + oPS_EVENTSEQUENCE );
	for ( int i = 0; i < MAX_PS_EVENTS; i++ ) {
		ps->events[i] = QAA_LoadS32( a + oPS_EVENTS + 4 * i );
		ps->eventParms[i] = QAA_LoadS32( a + oPS_EVENTPARMS + 4 * i );
	}
	ps->externalEvent = QAA_LoadS32( a + oPS_EXTERNALEVENT );
	ps->externalEventParm = QAA_LoadS32( a + oPS_EXTERNALEVENTPARM );
	ps->externalEventTime = QAA_LoadS32( a + oPS_EXTERNALEVENTTIME );
	ps->clientNum = QAA_LoadS32( a + oPS_CLIENTNUM );
	ps->weapon = QAA_LoadS32( a + oPS_WEAPON );
	ps->weaponstate = QAA_LoadS32( a + oPS_WEAPONSTATE );
	QAA_LoadVec3( a + oPS_VIEWANGLES, ps->viewangles );
	ps->viewheight = QAA_LoadS32( a + oPS_VIEWHEIGHT );
	ps->damageEvent = QAA_LoadS32( a + oPS_DAMAGEEVENT );
	ps->damageYaw = QAA_LoadS32( a + oPS_DAMAGEYAW );
	ps->damagePitch = QAA_LoadS32( a + oPS_DAMAGEPITCH );
	ps->damageCount = QAA_LoadS32( a + oPS_DAMAGECOUNT );
	for ( int i = 0; i < oPS_ARRAY; i++ ) {
		ps->stats[i] = QAA_LoadS32( a + oPS_STATS + 4 * i );
		ps->persistant[i] = QAA_LoadS32( a + oPS_PERSISTANT + 4 * i );
		ps->powerups[i] = QAA_LoadS32( a + oPS_POWERUPS + 4 * i );
		ps->ammo[i] = QAA_LoadS32( a + oPS_AMMO + 4 * i );
	}
	ps->generic1 = QAA_LoadS32( a + oPS_GENERIC1 );
	ps->loopSound = QAA_LoadS32( a + oPS_LOOPSOUND );
	ps->jumppad_ent = QAA_LoadS32( a + oPS_JUMPPAD_ENT );
	ps->ping = QAA_LoadS32( a + oPS_PING );
	ps->pmove_framecount = QAA_LoadS32( a + oPS_PMOVE_FRAMECOUNT );
	ps->jumppad_frame = QAA_LoadS32( a + oPS_JUMPPAD_FRAME );
	ps->entityEventSequence = QAA_LoadS32( a + oPS_ENTITYEVENTSEQUENCE );
	// oPS_QAA_WEAPONSDISABLED (0x1D4) is QAA's, not networked: the game and the
	// cgame each set it for their own Pmove (docs/ABI.md), so it is not carried.
}

static inline void QAA_PlayerStateToGuest( uint32_t a, const playerState_t *ps ) {
	using namespace qaa_layout;
	QAA_StoreS32( a + oPS_COMMANDTIME, ps->commandTime );
	QAA_StoreS32( a + oPS_PM_TYPE, ps->pm_type );
	QAA_StoreS32( a + oPS_BOBCYCLE, ps->bobCycle );
	QAA_StoreS32( a + oPS_PM_FLAGS, ps->pm_flags );
	QAA_StoreS32( a + oPS_PM_TIME, ps->pm_time );
	QAA_StoreVec3( a + oPS_ORIGIN, ps->origin );
	QAA_StoreVec3( a + oPS_VELOCITY, ps->velocity );
	QAA_StoreS32( a + oPS_WEAPONTIME, ps->weaponTime );
	QAA_StoreS32( a + oPS_GRAVITY, ps->gravity );
	QAA_StoreS32( a + oPS_SPEED, ps->speed );
	for ( int i = 0; i < 3; i++ ) {
		QAA_StoreS32( a + oPS_DELTA_ANGLES + 4 * i, ps->delta_angles[i] );
	}
	QAA_StoreS32( a + oPS_GROUNDENTITYNUM, ps->groundEntityNum );
	QAA_StoreS32( a + oPS_LEGSTIMER, ps->legsTimer );
	QAA_StoreS32( a + oPS_LEGSANIM, ps->legsAnim );
	QAA_StoreS32( a + oPS_TORSOTIMER, ps->torsoTimer );
	QAA_StoreS32( a + oPS_TORSOANIM, ps->torsoAnim );
	QAA_StoreS32( a + oPS_MOVEMENTDIR, ps->movementDir );
	QAA_StoreVec3( a + oPS_GRAPPLEPOINT, ps->grapplePoint );
	QAA_StoreS32( a + oPS_EFLAGS, ps->eFlags );
	QAA_StoreS32( a + oPS_EVENTSEQUENCE, ps->eventSequence );
	for ( int i = 0; i < MAX_PS_EVENTS; i++ ) {
		QAA_StoreS32( a + oPS_EVENTS + 4 * i, ps->events[i] );
		QAA_StoreS32( a + oPS_EVENTPARMS + 4 * i, ps->eventParms[i] );
	}
	QAA_StoreS32( a + oPS_EXTERNALEVENT, ps->externalEvent );
	QAA_StoreS32( a + oPS_EXTERNALEVENTPARM, ps->externalEventParm );
	QAA_StoreS32( a + oPS_EXTERNALEVENTTIME, ps->externalEventTime );
	QAA_StoreS32( a + oPS_CLIENTNUM, ps->clientNum );
	QAA_StoreS32( a + oPS_WEAPON, ps->weapon );
	QAA_StoreS32( a + oPS_WEAPONSTATE, ps->weaponstate );
	QAA_StoreVec3( a + oPS_VIEWANGLES, ps->viewangles );
	QAA_StoreS32( a + oPS_VIEWHEIGHT, ps->viewheight );
	QAA_StoreS32( a + oPS_DAMAGEEVENT, ps->damageEvent );
	QAA_StoreS32( a + oPS_DAMAGEYAW, ps->damageYaw );
	QAA_StoreS32( a + oPS_DAMAGEPITCH, ps->damagePitch );
	QAA_StoreS32( a + oPS_DAMAGECOUNT, ps->damageCount );
	for ( int i = 0; i < oPS_ARRAY; i++ ) {
		QAA_StoreS32( a + oPS_STATS + 4 * i, ps->stats[i] );
		QAA_StoreS32( a + oPS_PERSISTANT + 4 * i, ps->persistant[i] );
		QAA_StoreS32( a + oPS_POWERUPS + 4 * i, ps->powerups[i] );
		QAA_StoreS32( a + oPS_AMMO + 4 * i, ps->ammo[i] );
	}
	QAA_StoreS32( a + oPS_GENERIC1, ps->generic1 );
	QAA_StoreS32( a + oPS_LOOPSOUND, ps->loopSound );
	QAA_StoreS32( a + oPS_JUMPPAD_ENT, ps->jumppad_ent );
	QAA_StoreS32( a + oPS_PING, ps->ping );
	QAA_StoreS32( a + oPS_PMOVE_FRAMECOUNT, ps->pmove_framecount );
	QAA_StoreS32( a + oPS_JUMPPAD_FRAME, ps->jumppad_frame );
	QAA_StoreS32( a + oPS_ENTITYEVENTSEQUENCE, ps->entityEventSequence );
}

static inline void QAA_UsercmdToGuest( uint32_t a, const usercmd_t *cmd ) {
	using namespace qaa_layout;
	QAA_StoreS32( a + oUC_SERVERTIME, cmd->serverTime );
	for ( int i = 0; i < 3; i++ ) {
		QAA_StoreS32( a + oUC_ANGLES + 4 * i, cmd->angles[i] );
	}
	QAA_StoreS32( a + oUC_BUTTONS, cmd->buttons );
	QAA_StoreU8( a + oUC_WEAPON, cmd->weapon );
	QAA_StoreU8( a + oUC_FORWARDMOVE, static_cast<uint8_t>( cmd->forwardmove ) );
	QAA_StoreU8( a + oUC_RIGHTMOVE, static_cast<uint8_t>( cmd->rightmove ) );
	QAA_StoreU8( a + oUC_UPMOVE, static_cast<uint8_t>( cmd->upmove ) );
}

static inline void QAA_UsercmdFromGuest( uint32_t a, usercmd_t *cmd ) {
	using namespace qaa_layout;
	Com_Memset( cmd, 0, sizeof( *cmd ) );	// Quake Live's weaponPrimary and fov stay zero
	cmd->serverTime = QAA_LoadS32( a + oUC_SERVERTIME );
	for ( int i = 0; i < 3; i++ ) {
		cmd->angles[i] = QAA_LoadS32( a + oUC_ANGLES + 4 * i );
	}
	cmd->buttons = QAA_LoadS32( a + oUC_BUTTONS );
	cmd->weapon = QAA_LoadU8( a + oUC_WEAPON );
	cmd->forwardmove = static_cast<signed char>( QAA_LoadU8( a + oUC_FORWARDMOVE ) );
	cmd->rightmove = static_cast<signed char>( QAA_LoadU8( a + oUC_RIGHTMOVE ) );
	cmd->upmove = static_cast<signed char>( QAA_LoadU8( a + oUC_UPMOVE ) );
}

static inline void QAA_TraceToGuest( uint32_t a, const trace_t *tr ) {
	using namespace qaa_layout;
	QAA_StoreS32( a + oTRACE_ALLSOLID, tr->allsolid );
	QAA_StoreS32( a + oTRACE_STARTSOLID, tr->startsolid );
	QAA_StoreF( a + oTRACE_FRACTION, tr->fraction );
	QAA_StoreVec3( a + oTRACE_ENDPOS, tr->endpos );
	QAA_StoreVec3( a + oTRACE_PLANE_NORMAL, tr->plane.normal );
	QAA_StoreF( a + oTRACE_PLANE_DIST, tr->plane.dist );
	QAA_StoreU8( a + oTRACE_PLANE_TYPE, tr->plane.type );
	QAA_StoreU8( a + oTRACE_PLANE_SIGNBITS, tr->plane.signbits );
	QAA_StoreU16( a + oTRACE_PLANE_SIGNBITS + 1, 0 );
	QAA_StoreS32( a + oTRACE_SURFACEFLAGS, tr->surfaceFlags );
	QAA_StoreS32( a + oTRACE_CONTENTS, tr->contents );
	QAA_StoreS32( a + oTRACE_ENTITYNUM, tr->entityNum );
}

/* A guest vec3 argument, read into host floats; NULL for guest NULL. */
struct QAAVec3 {
	vec3_t v;
	const float *p;
	explicit QAAVec3( uint32_t a ) : v{ 0, 0, 0 }, p( nullptr ) {
		if ( a ) {
			QAA_LoadVec3( a, v );
			p = v;
		}
	}
};

/* vmCvar_t: FnQL's legacy layout is Quake III's, host-endian. */
struct QAAVmCvar {
	uint32_t address;
	byte host[qaa_layout::oCVAR_SIZE];
	explicit QAAVmCvar( uint32_t a ) : address( a ) {
		Com_Memset( host, 0, sizeof( host ) );
		if ( !a ) {
			return;
		}
		for ( uint32_t i = 0; i < qaa_layout::oCVAR_HEADER_WORDS; i++ ) {
			const uint32_t w = QAA_LoadU32( a + 4 * i );
			std::memcpy( host + 4 * i, &w, 4 );
		}
		std::memcpy( host + qaa_layout::oCVAR_STRING, QAA_Ptr( a + qaa_layout::oCVAR_STRING ),
			qaa_layout::oCVAR_SIZE - qaa_layout::oCVAR_STRING );
	}
	void *Host() { return address ? host : nullptr; }
	void WriteBack() {
		if ( !address ) {
			return;
		}
		for ( uint32_t i = 0; i < qaa_layout::oCVAR_HEADER_WORDS; i++ ) {
			uint32_t w;
			std::memcpy( &w, host + 4 * i, 4 );
			QAA_StoreU32( address + 4 * i, w );
		}
		std::memcpy( QAA_Ptr( address + qaa_layout::oCVAR_STRING ), host + qaa_layout::oCVAR_STRING,
			qaa_layout::oCVAR_SIZE - qaa_layout::oCVAR_STRING );
	}
};

#endif
