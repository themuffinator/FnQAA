/*
===========================================================================
openQAA: hosting QAA's recompiled cgame (milestone M5).

Copyright (C) 2026 openQAA contributors.
This file is part of openQAA, licensed under the GNU GPL v2 or later.

Included at the end of code/client/cl_cgame.cpp, whose helpers it uses.

QAA's cgameXe.dll, recompiled from the user's package, runs on the guest
runtime (code/qaa/qaa_guest_host.cpp). This file is its boundary with the
engine (facts: docs/ABI.md):

- vmMain: FnQL's command numbers become QAA's (CG_EVENT_HANDLING moves from
  8 to 9; QAA's 12 hands over the engine function table before CG_INIT).
- Traps: QAA's numbering (cgame_traps.tsv). Each is converted to host form
  and handed to FnQL's own dispatcher under FnQL's number, so FnQL's client
  behaviour applies wherever QAA's matches it; QAA's differences are applied
  here and marked.
- The 18-entry engine function table (vmMain 12).
===========================================================================
*/

#include "../qaa/qaa_structs.h"

#include <algorithm>
#include <initializer_list>

namespace qaa_cg_layout {
// refEntity_t, 0xC0: Quake III's fields, with a QAA block at 0x1C..0x4F that
// nothing reads (docs/ABI.md), so everything from axis on sits 0x34 later
constexpr uint32_t oRE_RETYPE = 0x00, oRE_RENDERFX = 0x04, oRE_HMODEL = 0x08, oRE_LIGHTINGORIGIN = 0x0C,
	oRE_SHADOWPLANE = 0x18, oRE_AXIS = 0x50, oRE_NONNORMALIZEDAXES = 0x74, oRE_ORIGIN = 0x78, oRE_FRAME = 0x84,
	oRE_OLDORIGIN = 0x88, oRE_OLDFRAME = 0x94, oRE_BACKLERP = 0x98, oRE_SKINNUM = 0x9C, oRE_CUSTOMSKIN = 0xA0,
	oRE_CUSTOMSHADER = 0xA4, oRE_SHADERRGBA = 0xA8, oRE_SHADERTEXCOORD = 0xAC, oRE_SHADERTIME = 0xB4,
	oRE_RADIUS = 0xB8, oRE_ROTATION = 0xBC;
// refdef_t, 0x170, as Quake III
constexpr uint32_t oRD_X = 0x00, oRD_FOVX = 0x10, oRD_VIEWORG = 0x18, oRD_VIEWAXIS = 0x24, oRD_TIME = 0x48,
	oRD_RDFLAGS = 0x4C, oRD_AREAMASK = 0x50, oRD_TEXT = 0x70;
// snapshot_t, 0xD210
constexpr uint32_t oSN_SNAPFLAGS = 0x00, oSN_PING = 0x04, oSN_SERVERTIME = 0x08, oSN_AREAMASK = 0x0C,
	oSN_PS = 0x2C, oSN_NUMENTITIES = 0x204, oSN_ENTITIES = 0x208, oSN_NUMSERVERCOMMANDS = 0xD208,
	oSN_SERVERCOMMANDSEQUENCE = 0xD20C;
constexpr int SN_MAX_ENTITIES = 256;
// gameState_t, 0x4E84
constexpr uint32_t oGS_DATA = 0x1000, oGS_DATACOUNT = 0x4E80;
constexpr int GS_STRINGS = 1024, GS_CHARS = 16000;
// glconfig_t, 0x2C54: Quake III's up to isFullscreen, then QAA's
constexpr uint32_t oGL_RENDERER = 0x0, oGL_VENDOR = 0x400, oGL_VERSION = 0x800, oGL_EXTENSIONS = 0xC00,
	oGL_MAXTEXTURESIZE = 0x2C00, oGL_WIDESCREEN = 0x2C3C, oGL_HEIGHT_OVER_WIDTH = 0x2C40, oGL_ZERO = 0x2C44,
	oGL_SMPACTIVE = 0x2C48, oGL_INTERLACED = 0x2C4C, oGL_DISPLAYWIDTH = 0x2C50, GL_SIZE = 0x2C54;
// polyVert_t 0x18, markFragment_t 0x8, orientation_t 0x30, fontInfo_t
constexpr uint32_t POLYVERT_SIZE = 0x18, GLYPH_SIZE = 0x50, GLYPH_WORDS = 12;
}

/* ---- conversions -------------------------------------------------------- */

static void QAA_RefEntityFromGuest( uint32_t a, refEntity_t *re ) {
	using namespace qaa_cg_layout;
	Com_Memset( re, 0, sizeof( *re ) );
	re->reType = static_cast<refEntityType_t>( QAA_LoadS32( a + oRE_RETYPE ) );
	re->renderfx = QAA_LoadS32( a + oRE_RENDERFX );
	re->hModel = QAA_LoadS32( a + oRE_HMODEL );
	QAA_LoadVec3( a + oRE_LIGHTINGORIGIN, re->lightingOrigin );
	re->shadowPlane = QAA_LoadF( a + oRE_SHADOWPLANE );
	for ( int i = 0; i < 3; i++ ) {
		QAA_LoadVec3( a + oRE_AXIS + 12 * i, re->axis[i] );
	}
	re->nonNormalizedAxes = static_cast<qboolean>( QAA_LoadS32( a + oRE_NONNORMALIZEDAXES ) );
	QAA_LoadVec3( a + oRE_ORIGIN, re->origin );
	re->frame = QAA_LoadS32( a + oRE_FRAME );
	QAA_LoadVec3( a + oRE_OLDORIGIN, re->oldorigin );
	re->oldframe = QAA_LoadS32( a + oRE_OLDFRAME );
	re->backlerp = QAA_LoadF( a + oRE_BACKLERP );
	re->skinNum = QAA_LoadS32( a + oRE_SKINNUM );
	re->customSkin = QAA_LoadS32( a + oRE_CUSTOMSKIN );
	re->customShader = QAA_LoadS32( a + oRE_CUSTOMSHADER );
	for ( int i = 0; i < 4; i++ ) {
		re->shader.rgba[i] = QAA_LoadU8( a + oRE_SHADERRGBA + i );
	}
	re->shaderTexCoord[0] = QAA_LoadF( a + oRE_SHADERTEXCOORD );
	re->shaderTexCoord[1] = QAA_LoadF( a + oRE_SHADERTEXCOORD + 4 );
	re->shaderTime.f = QAA_LoadF( a + oRE_SHADERTIME );
	re->radius = QAA_LoadF( a + oRE_RADIUS );
	re->rotation = QAA_LoadF( a + oRE_ROTATION );
}

static void QAA_RefdefFromGuest( uint32_t a, refdef_t *rd ) {
	using namespace qaa_cg_layout;
	Com_Memset( rd, 0, sizeof( *rd ) );
	rd->x = QAA_LoadS32( a + oRD_X );
	rd->y = QAA_LoadS32( a + oRD_X + 4 );
	rd->width = QAA_LoadS32( a + oRD_X + 8 );
	rd->height = QAA_LoadS32( a + oRD_X + 12 );
	rd->fov_x = QAA_LoadF( a + oRD_FOVX );
	rd->fov_y = QAA_LoadF( a + oRD_FOVX + 4 );
	QAA_LoadVec3( a + oRD_VIEWORG, rd->vieworg );
	for ( int i = 0; i < 3; i++ ) {
		QAA_LoadVec3( a + oRD_VIEWAXIS + 12 * i, rd->viewaxis[i] );
	}
	rd->time = QAA_LoadS32( a + oRD_TIME );
	rd->rdflags = QAA_LoadS32( a + oRD_RDFLAGS );
	std::memcpy( rd->areamask, QAA_Ptr( a + oRD_AREAMASK ), sizeof( rd->areamask ) );
	std::memcpy( rd->text, QAA_Ptr( a + oRD_TEXT ), sizeof( rd->text ) );
}

static void QAA_SnapshotToGuest( uint32_t a, const snapshot_t *snap ) {
	using namespace qaa_cg_layout;
	QAA_StoreS32( a + oSN_SNAPFLAGS, snap->snapFlags );
	QAA_StoreS32( a + oSN_PING, snap->ping );
	QAA_StoreS32( a + oSN_SERVERTIME, snap->serverTime );
	std::memcpy( QAA_Ptr( a + oSN_AREAMASK ), snap->areamask, MAX_MAP_AREA_BYTES );
	QAA_PlayerStateToGuest( a + oSN_PS, &snap->ps );
	const int count = snap->numEntities < SN_MAX_ENTITIES ? snap->numEntities : SN_MAX_ENTITIES;
	QAA_StoreS32( a + oSN_NUMENTITIES, count );
	for ( int i = 0; i < count; i++ ) {
		QAA_EntityStateToGuest( a + oSN_ENTITIES + qaa_layout::oES_SIZE * static_cast<uint32_t>( i ), &snap->entities[i] );
	}
	QAA_StoreS32( a + oSN_NUMSERVERCOMMANDS, snap->numServerCommands );
	QAA_StoreS32( a + oSN_SERVERCOMMANDSEQUENCE, snap->serverCommandSequence );
}

static void QAA_GameStateToGuest( uint32_t a, const gameState_t *gs ) {
	using namespace qaa_cg_layout;
	for ( int i = 0; i < GS_STRINGS; i++ ) {
		QAA_StoreS32( a + 4 * static_cast<uint32_t>( i ), gs->stringOffsets[i] );
	}
	std::memcpy( QAA_Ptr( a + oGS_DATA ), gs->stringData, GS_CHARS );
	QAA_StoreS32( a + oGS_DATACOUNT, gs->dataCount );
}

static void QAA_GlconfigToGuest( uint32_t a, const glconfig_t *gl ) {
	using namespace qaa_cg_layout;
	std::memset( QAA_Ptr( a ), 0, GL_SIZE );
	std::memcpy( QAA_Ptr( a + oGL_RENDERER ), gl->renderer_string, 0x400 );
	std::memcpy( QAA_Ptr( a + oGL_VENDOR ), gl->vendor_string, 0x400 );
	std::memcpy( QAA_Ptr( a + oGL_VERSION ), gl->version_string, 0x400 );
	std::memcpy( QAA_Ptr( a + oGL_EXTENSIONS ), gl->extensions_string, 0x2000 );
	const int32_t words[15] = {
		gl->maxTextureSize, gl->numTextureUnits, gl->colorBits, gl->depthBits, gl->stencilBits,
		gl->driverType, gl->hardwareType, gl->deviceSupportsGamma, gl->textureCompression,
		gl->textureEnvAddAvailable, gl->vidWidth, gl->vidHeight, 0 /* windowAspect, below */,
		gl->displayFrequency, gl->isFullscreen,
	};
	for ( int i = 0; i < 15; i++ ) {
		QAA_StoreS32( a + oGL_MAXTEXTURESIZE + 4 * i, words[i] );
	}
	QAA_StoreF( a + oGL_MAXTEXTURESIZE + 4 * 12, gl->windowAspect );
	// QAA's own fields (GLimp_Init 0x8213D6E8). On the PC the "Xbox video
	// mode" is the window: widescreen when wider than 4:3, never interlaced.
	QAA_StoreS32( a + oGL_WIDESCREEN, gl->windowAspect > 4.0f / 3.0f + 0.01f ? 1 : 0 );
	QAA_StoreF( a + oGL_HEIGHT_OVER_WIDTH, gl->vidWidth ? static_cast<float>( gl->vidHeight / gl->vidWidth ) : 0.0f );
	QAA_StoreS32( a + oGL_ZERO, 0 );
	QAA_StoreS32( a + oGL_SMPACTIVE, gl->smpActive );
	QAA_StoreS32( a + oGL_INTERLACED, 0 );
	QAA_StoreS32( a + oGL_DISPLAYWIDTH, gl->vidWidth );
}

static void QAA_OrientationToGuest( uint32_t a, const orientation_t *o ) {
	QAA_StoreVec3( a, o->origin );
	for ( int i = 0; i < 3; i++ ) {
		QAA_StoreVec3( a + 12 + 12 * static_cast<uint32_t>( i ), o->axis[i] );
	}
}

static void QAA_FontInfoToGuest( uint32_t a, const fontInfo_t *font ) {
	using namespace qaa_cg_layout;
	for ( int g = 0; g < GLYPHS_PER_FONT; g++ ) {
		const glyphInfo_t *gi = &font->glyphs[g];
		const uint32_t b = a + GLYPH_SIZE * static_cast<uint32_t>( g );
		const int32_t ints[7] = { gi->height, gi->top, gi->bottom, gi->pitch, gi->xSkip, gi->imageWidth, gi->imageHeight };
		for ( int i = 0; i < 7; i++ ) {
			QAA_StoreS32( b + 4 * i, ints[i] );
		}
		QAA_StoreF( b + 28, gi->s );
		QAA_StoreF( b + 32, gi->t );
		QAA_StoreF( b + 36, gi->s2 );
		QAA_StoreF( b + 40, gi->t2 );
		QAA_StoreS32( b + 44, gi->glyph );
		std::memcpy( QAA_Ptr( b + 48 ), gi->shaderName, 32 );
	}
	const uint32_t tail = a + GLYPH_SIZE * GLYPHS_PER_FONT;
	QAA_StoreF( tail, font->glyphScale );
	std::memcpy( QAA_Ptr( tail + 4 ), font->name, MAX_QPATH );
}

/* ---- the module ---------------------------------------------------------- */

static qaa_module *qaaCgame;
static uint32_t qaaCgameEngineTable;
static bool qaaCgameWarnedLive;
extern byte qaa_scoreboardMailbox[0x24C];
extern int qaa_scoreboardMailboxSize;
extern bool qaa_scoreboardMailboxFull;

/* FnQL's dispatcher, under FnQL's trap number, with host arguments. */
static intptr_t QAA_CG_Forward( int trap, std::initializer_list<intptr_t> list ) {
	intptr_t args[16] = { trap };
	int i = 1;
	for ( intptr_t v : list ) {
		args[i++] = v;
	}
	return CL_CgameSystemCalls( args );
}

static intptr_t QAA_HostPtr( int32_t guestAddress ) {
	return reinterpret_cast<intptr_t>( QAA_Ptr( static_cast<uint32_t>( guestAddress ) ) );
}

/* QAA's math traps truncate the result to an int (fctiwz, docs/ABI.md). */
static int32_t QAA_CG_Trunc( double v ) {
	return static_cast<int32_t>( v );
}

static int64_t QAA_CgameSyscall( void *, qaa_module *, const qaa_syscall *call ) {
	const int32_t *a = call->args;
	const auto P = []( int32_t v ) { return QAA_HostPtr( v ); };
	const auto I = []( int32_t v ) { return static_cast<intptr_t>( v ); };

	const int t = call->trap;
	switch ( t ) {
	// 0..6: Quake III's numbers
	case 0: return QAA_CG_Forward( CG_PRINT, { P( a[0] ) } );
	case 1: Com_Error( ERR_DROP, "%s", QAA_Str( a[0] ) );
	case 2: return QAA_CG_Forward( CG_MILLISECONDS, {} );
	case 3: {
		QAAVmCvar vc( a[0] );
		Cvar_RegisterLegacy( vc.Host(), QAA_Str( a[1] ), QAA_Str( a[2] ), a[3], cgvm->privateFlag );
		vc.WriteBack();
		return 0;
	}
	case 4: {
		QAAVmCvar vc( a[0] );
		Cvar_UpdateLegacy( vc.Host(), cgvm->privateFlag );
		vc.WriteBack();
		return 0;
	}
	case 5:	// QAA: force = 1
		Cvar_Set2( QAA_Str( a[0] ), QAA_Str( a[1] ), qtrue );
		return 0;
	case 6: return QAA_CG_Forward( CG_CVAR_VARIABLESTRINGBUFFER, { P( a[0] ), P( a[1] ), I( a[2] ) } );
	case 7:	// QAA: Cvar_VariableValue, truncated
		return QAA_CG_Trunc( Cvar_VariableValue( QAA_Str( a[0] ) ) );
	// 8..34: Quake III's 7..33
	case 8: return QAA_CG_Forward( CG_ARGC, {} );
	case 9: return QAA_CG_Forward( CG_ARGV, { I( a[0] ), P( a[1] ), I( a[2] ) } );
	case 10: return QAA_CG_Forward( CG_ARGS, { P( a[0] ), I( a[1] ) } );
	case 11: {
		fileHandle_t f = 0;
		const intptr_t r = QAA_CG_Forward( CG_FS_FOPENFILE, { P( a[0] ), a[1] ? reinterpret_cast<intptr_t>( &f ) : 0, I( a[2] ) } );
		if ( a[1] ) {
			QAA_StoreS32( a[1], f );
		}
		return r;
	}
	case 12: return QAA_CG_Forward( CG_FS_READ, { P( a[0] ), I( a[1] ), I( a[2] ) } );
	case 13: return QAA_CG_Forward( CG_FS_WRITE, { P( a[0] ), I( a[1] ), I( a[2] ) } );
	case 14: return QAA_CG_Forward( CG_FS_FCLOSEFILE, { I( a[0] ) } );
	case 15: return QAA_CG_Forward( CG_SENDCONSOLECOMMAND, { P( a[0] ) } );
	case 16: return QAA_CG_Forward( CG_ADDCOMMAND, { P( a[0] ) } );
	case 17: return QAA_CG_Forward( CG_SENDCLIENTCOMMAND, { P( a[0] ) } );
	case 18: return QAA_CG_Forward( CG_UPDATESCREEN, {} );
	case 19: return QAA_CG_Forward( CG_CM_LOADMAP, { P( a[0] ) } );
	case 20: return QAA_CG_Forward( CG_CM_NUMINLINEMODELS, {} );
	case 21: return QAA_CG_Forward( CG_CM_INLINEMODEL, { I( a[0] ) } );
	case 23:
	case 85: {
		QAAVec3 mins( a[0] ), maxs( a[1] );
		return QAA_CG_Forward( t == 23 ? CG_CM_TEMPBOXMODEL : CG_CM_TEMPCAPSULEMODEL,
			{ reinterpret_cast<intptr_t>( mins.v ), reinterpret_cast<intptr_t>( maxs.v ) } );
	}
	case 24: {
		QAAVec3 p( a[0] );
		return QAA_CG_Forward( CG_CM_POINTCONTENTS, { reinterpret_cast<intptr_t>( p.v ), I( a[1] ) } );
	}
	case 25: {
		QAAVec3 p( a[0] ), origin( a[2] ), angles( a[3] );
		return QAA_CG_Forward( CG_CM_TRANSFORMEDPOINTCONTENTS, { reinterpret_cast<intptr_t>( p.v ), I( a[1] ),
			reinterpret_cast<intptr_t>( origin.v ), reinterpret_cast<intptr_t>( angles.v ) } );
	}
	case 26:
	case 86:
	case 27:
	case 87: {
		trace_t tr;
		QAAVec3 start( a[1] ), end( a[2] ), mins( a[3] ), maxs( a[4] );
		const bool transformed = t == 27 || t == 87;
		const bool capsule = t == 86 || t == 87;
		if ( transformed ) {
			QAAVec3 origin( a[7] ), angles( a[8] );
			QAA_CG_Forward( capsule ? CG_CM_TRANSFORMEDCAPSULETRACE : CG_CM_TRANSFORMEDBOXTRACE, {
				reinterpret_cast<intptr_t>( &tr ), reinterpret_cast<intptr_t>( start.v ), reinterpret_cast<intptr_t>( end.v ),
				reinterpret_cast<intptr_t>( mins.p ), reinterpret_cast<intptr_t>( maxs.p ), I( a[5] ), I( a[6] ),
				reinterpret_cast<intptr_t>( origin.v ), reinterpret_cast<intptr_t>( angles.v ) } );
		} else {
			QAA_CG_Forward( capsule ? CG_CM_CAPSULETRACE : CG_CM_BOXTRACE, {
				reinterpret_cast<intptr_t>( &tr ), reinterpret_cast<intptr_t>( start.v ), reinterpret_cast<intptr_t>( end.v ),
				reinterpret_cast<intptr_t>( mins.p ), reinterpret_cast<intptr_t>( maxs.p ), I( a[5] ), I( a[6] ) } );
		}
		QAA_TraceToGuest( a[0], &tr );
		return 0;
	}
	case 28: {
		// CM_MarkFragments( numPoints, points, projection, maxPoints, pointBuffer, maxFragments, fragmentBuffer )
		const int numPoints = a[0], maxPoints = a[3], maxFragments = a[5];
		if ( numPoints <= 0 || maxPoints <= 0 || maxFragments <= 0 ) {
			return 0;
		}
		std::vector<float> points( static_cast<size_t>( numPoints ) * 3 ), out( static_cast<size_t>( maxPoints ) * 3 );
		std::vector<markFragment_t> frags( static_cast<size_t>( maxFragments ) );
		for ( int i = 0; i < numPoints; i++ ) {
			QAA_LoadVec3( a[1] + 12 * i, &points[3 * i] );
		}
		QAAVec3 projection( a[2] );
		const int n = static_cast<int>( QAA_CG_Forward( CG_CM_MARKFRAGMENTS, { I( numPoints ),
			reinterpret_cast<intptr_t>( points.data() ), reinterpret_cast<intptr_t>( projection.v ), I( maxPoints ),
			reinterpret_cast<intptr_t>( out.data() ), I( maxFragments ), reinterpret_cast<intptr_t>( frags.data() ) } ) );
		int usedPoints = 0;
		for ( int i = 0; i < n; i++ ) {
			QAA_StoreS32( a[6] + 8 * i, frags[i].firstPoint );
			QAA_StoreS32( a[6] + 8 * i + 4, frags[i].numPoints );
			usedPoints = std::max( usedPoints, frags[i].firstPoint + frags[i].numPoints );
		}
		for ( int i = 0; i < usedPoints && i < maxPoints; i++ ) {
			QAA_StoreVec3( a[4] + 12 * i, &out[3 * i] );
		}
		return n;
	}
	case 29: {
		QAAVec3 origin( a[0] );
		return QAA_CG_Forward( CG_S_STARTSOUND, { reinterpret_cast<intptr_t>( origin.p ), I( a[1] ), I( a[2] ), I( a[3] ) } );
	}
	case 30: return QAA_CG_Forward( CG_S_STARTLOCALSOUND, { I( a[0] ), I( a[1] ) } );
	case 31:	// QAA ignores the argument and clears every loop
		return QAA_CG_Forward( CG_S_CLEARLOOPINGSOUNDS, { I( qtrue ) } );
	case 32: {	// QAA drops the velocity
		QAAVec3 origin( a[1] );
		static const vec3_t still = { 0, 0, 0 };
		return QAA_CG_Forward( CG_S_ADDLOOPINGSOUND, { I( a[0] ), reinterpret_cast<intptr_t>( origin.v ),
			reinterpret_cast<intptr_t>( still ), I( a[3] ) } );
	}
	case 33: {
		QAAVec3 origin( a[1] );
		return QAA_CG_Forward( CG_S_UPDATEENTITYPOSITION, { I( a[0] ), reinterpret_cast<intptr_t>( origin.v ) } );
	}
	case 34: {	// QAA drops inwater
		QAAVec3 origin( a[1] );
		vec3_t axis[3];
		for ( int i = 0; i < 3; i++ ) {
			QAA_LoadVec3( a[2] + 12 * i, axis[i] );
		}
		return QAA_CG_Forward( CG_S_RESPATIALIZE, { I( a[0] ), reinterpret_cast<intptr_t>( origin.v ),
			reinterpret_cast<intptr_t>( axis ), 0 } );
	}
	case 35: return QAA_CG_Forward( CG_S_REGISTERSOUND, { P( a[0] ), 0 } );
	case 36:	// QAA: compressed = 1, result discarded
		QAA_CG_Forward( CG_S_REGISTERSOUND, { P( a[0] ), 1 } );
		return 0;
	// 37..47: Quake III's 35..45
	case 37: return QAA_CG_Forward( CG_S_STARTBACKGROUNDTRACK, { P( a[0] ), P( a[1] ) } );
	case 38: return QAA_CG_Forward( CG_R_LOADWORLDMAP, { P( a[0] ) } );
	case 39: return QAA_CG_Forward( CG_R_REGISTERMODEL, { P( a[0] ) } );
	case 40: return QAA_CG_Forward( CG_R_REGISTERSKIN, { P( a[0] ) } );
	case 41: return QAA_CG_Forward( CG_R_REGISTERSHADER, { P( a[0] ) } );
	case 42: return QAA_CG_Forward( CG_R_CLEARSCENE, {} );
	case 43: {
		refEntity_t re;
		QAA_RefEntityFromGuest( a[0], &re );
		return QAA_CG_Forward( CG_R_ADDREFENTITYTOSCENE, { reinterpret_cast<intptr_t>( &re ) } );
	}
	case 44:
	case 90: {
		const int numVerts = a[1];
		const int num = t == 90 ? a[3] : 1;
		if ( numVerts <= 0 || num <= 0 ) {
			return 0;
		}
		std::vector<polyVert_t> verts( static_cast<size_t>( numVerts ) * static_cast<size_t>( num ) );
		for ( size_t i = 0; i < verts.size(); i++ ) {
			const uint32_t v = static_cast<uint32_t>( a[2] ) + qaa_cg_layout::POLYVERT_SIZE * static_cast<uint32_t>( i );
			QAA_LoadVec3( v, verts[i].xyz );
			verts[i].st[0] = QAA_LoadF( v + 12 );
			verts[i].st[1] = QAA_LoadF( v + 16 );
			std::memcpy( verts[i].modulate.rgba, QAA_Ptr( v + 20 ), 4 );
		}
		return QAA_CG_Forward( t == 90 ? CG_R_ADDPOLYSTOSCENE : CG_R_ADDPOLYTOSCENE,
			{ I( a[0] ), I( numVerts ), reinterpret_cast<intptr_t>( verts.data() ), I( num ) } );
	}
	case 45:
	case 88: {
		QAAVec3 org( a[0] );
		return QAA_CG_Forward( t == 45 ? CG_R_ADDLIGHTTOSCENE : CG_R_ADDADDITIVELIGHTTOSCENE,
			{ reinterpret_cast<intptr_t>( org.v ), I( a[1] ), I( a[2] ), I( a[3] ), I( a[4] ) } );
	}
	case 46: {
		refdef_t rd;
		QAA_RefdefFromGuest( a[0], &rd );
		return QAA_CG_Forward( CG_R_RENDERSCENE, { reinterpret_cast<intptr_t>( &rd ) } );
	}
	case 47: {
		if ( !a[0] ) {
			return QAA_CG_Forward( CG_R_SETCOLOR, { 0 } );
		}
		float rgba[4];
		for ( int i = 0; i < 4; i++ ) {
			rgba[i] = QAA_LoadF( a[0] + 4 * i );
		}
		return QAA_CG_Forward( CG_R_SETCOLOR, { reinterpret_cast<intptr_t>( rgba ) } );
	}
	// 49..92: Quake III's 46..89
	case 49:
		// QAA's engine flips y for its own renderer (y' = vidHeight - y - h);
		// FnQL's renderer takes the cgame's top-down y as Quake III's does.
		return QAA_CG_Forward( CG_R_DRAWSTRETCHPIC, { I( a[0] ), I( a[1] ), I( a[2] ), I( a[3] ), I( a[4] ),
			I( a[5] ), I( a[6] ), I( a[7] ), I( a[8] ) } );
	case 50: {
		vec3_t mins, maxs;
		QAA_CG_Forward( CG_R_MODELBOUNDS, { I( a[0] ), reinterpret_cast<intptr_t>( mins ), reinterpret_cast<intptr_t>( maxs ) } );
		QAA_StoreVec3( a[1], mins );
		QAA_StoreVec3( a[2], maxs );
		return 0;
	}
	case 51: {
		orientation_t o;
		const intptr_t r = QAA_CG_Forward( CG_R_LERPTAG, { reinterpret_cast<intptr_t>( &o ), I( a[1] ), I( a[2] ),
			I( a[3] ), I( a[4] ), P( a[5] ) } );
		QAA_OrientationToGuest( a[0], &o );
		return r;
	}
	case 52: {
		glconfig_t gl;
		CL_GetGlconfig( &gl );
		QAA_GlconfigToGuest( a[0], &gl );
		return 0;
	}
	case 53: {
		gameState_t *gs = static_cast<gameState_t *>( Z_Malloc( sizeof( gameState_t ) ) );
		CL_GetGameState( gs );
		QAA_GameStateToGuest( a[0], gs );
		Z_Free( gs );
		return 0;
	}
	case 54: {
		int snapshotNumber = 0, serverTime = 0;
		CL_GetCurrentSnapshotNumber( &snapshotNumber, &serverTime );
		QAA_StoreS32( a[0], snapshotNumber );
		QAA_StoreS32( a[1], serverTime );
		return 0;
	}
	case 55: {
		snapshot_t *snap = static_cast<snapshot_t *>( Z_Malloc( sizeof( snapshot_t ) ) );
		const qboolean r = CL_GetSnapshot( a[0], snap );
		if ( r ) {
			QAA_SnapshotToGuest( a[1], snap );
		}
		Z_Free( snap );
		return r;
	}
	case 56: return QAA_CG_Forward( CG_GETSERVERCOMMAND, { I( a[0] ) } );
	case 57: return QAA_CG_Forward( CG_GETCURRENTCMDNUMBER, {} );
	case 58: {
		usercmd_t cmd;
		const qboolean r = CL_GetUserCmd( a[0], &cmd );
		if ( r ) {
			QAA_UsercmdToGuest( a[1], &cmd );
		}
		return r;
	}
	case 59:	// ( stateValue, sensitivityScale ): QAA has no primary weapon or fov
		CL_SetUserCmdValue( a[0], 0, QAA_ArgF( a[1] ), 0 );
		return 0;
	case 60: return QAA_CG_Forward( CG_R_REGISTERSHADERNOMIP, { P( a[0] ) } );
	case 61: return QAA_CG_Forward( CG_MEMORY_REMAINING, {} );
	case 62: {
		fontInfo_t *font = static_cast<fontInfo_t *>( Z_Malloc( sizeof( fontInfo_t ) ) );
		QAA_CG_Forward( CG_R_REGISTERFONT, { P( a[0] ), I( a[1] ), reinterpret_cast<intptr_t>( font ) } );
		QAA_FontInfoToGuest( a[2], font );
		Z_Free( font );
		return 0;
	}
	case 63: return QAA_CG_Forward( CG_KEY_ISDOWN, { I( a[0] ) } );
	case 64: return QAA_CG_Forward( CG_KEY_GETCATCHER, {} );
	case 65: return QAA_CG_Forward( CG_KEY_SETCATCHER, { I( a[0] ) } );
	case 66: return QAA_CG_Forward( CG_KEY_GETKEY, { P( a[0] ) } );
	case 67: return QAA_CG_Forward( CG_PC_ADD_GLOBAL_DEFINE, { P( a[0] ) } );
	case 68: return QAA_CG_Forward( CG_PC_LOAD_SOURCE, { P( a[0] ) } );
	case 69: return QAA_CG_Forward( CG_PC_FREE_SOURCE, { I( a[0] ) } );
	case 70: {
		pc_token_t token;
		const intptr_t r = QAA_CG_Forward( CG_PC_READ_TOKEN, { I( a[0] ), reinterpret_cast<intptr_t>( &token ) } );
		QAA_StoreS32( a[1], token.type );
		QAA_StoreS32( a[1] + 4, token.subtype );
		QAA_StoreS32( a[1] + 8, token.intvalue );
		QAA_StoreF( a[1] + 12, token.floatvalue );
		std::memcpy( QAA_Ptr( a[1] + 16 ), token.string, MAX_TOKENLENGTH );
		return r;
	}
	case 71: {
		int line = 0;
		const intptr_t r = QAA_CG_Forward( CG_PC_SOURCE_FILE_AND_LINE, { I( a[0] ), P( a[1] ), reinterpret_cast<intptr_t>( &line ) } );
		QAA_StoreS32( a[2], line );
		return r;
	}
	case 72: return QAA_CG_Forward( CG_S_STOPBACKGROUNDTRACK, {} );
	case 73: {
		qtime_t qt;
		const intptr_t r = QAA_CG_Forward( CG_REAL_TIME, { reinterpret_cast<intptr_t>( &qt ) } );
		const int fields[9] = { qt.tm_sec, qt.tm_min, qt.tm_hour, qt.tm_mday, qt.tm_mon, qt.tm_year, qt.tm_wday, qt.tm_yday, qt.tm_isdst };
		for ( int i = 0; i < 9 && a[0]; i++ ) {
			QAA_StoreS32( a[0] + 4 * i, fields[i] );
		}
		return r;
	}
	case 74:	// QAA floors each component
		for ( int i = 0; i < 3; i++ ) {
			QAA_StoreF( a[0] + 4 * i, floorf( QAA_LoadF( a[0] + 4 * i ) ) );
		}
		return 0;
	case 75: return QAA_CG_Forward( CG_REMOVECOMMAND, { P( a[0] ) } );
	case 76: {
		QAAVec3 point( a[0] );
		vec3_t ambient, directed, dir;
		const intptr_t r = QAA_CG_Forward( CG_R_LIGHTFORPOINT, { reinterpret_cast<intptr_t>( point.v ),
			reinterpret_cast<intptr_t>( ambient ), reinterpret_cast<intptr_t>( directed ), reinterpret_cast<intptr_t>( dir ) } );
		QAA_StoreVec3( a[1], ambient );
		QAA_StoreVec3( a[2], directed );
		QAA_StoreVec3( a[3], dir );
		return r;
	}
	case 77: case 78: case 79: case 80: case 81:	// QAA stubs cinematics
		return 0;
	case 82: return QAA_CG_Forward( CG_R_REMAP_SHADER, { P( a[0] ), P( a[1] ), P( a[2] ) } );
	case 83: {
		// QAA plays these through S_StartSound after a check (0x821737B0) not
		// yet identified; FnQL's real looping sound stands in until then.
		QAAVec3 origin( a[1] ), velocity( a[2] );
		return QAA_CG_Forward( CG_S_ADDREALLOOPINGSOUND, { I( a[0] ), reinterpret_cast<intptr_t>( origin.v ),
			reinterpret_cast<intptr_t>( velocity.v ), I( a[3] ) } );
	}
	case 84:	// QAA: a no-op
		return 0;
	case 89: return QAA_CG_Forward( CG_GET_ENTITY_TOKEN, { P( a[0] ), I( a[1] ) } );
	case 91: {
		QAAVec3 p1( a[0] ), p2( a[1] );
		return QAA_CG_Forward( CG_R_INPVS, { reinterpret_cast<intptr_t>( p1.v ), reinterpret_cast<intptr_t>( p2.v ) } );
	}
	case 92: return QAA_CG_Forward( CG_FS_SEEK, { I( a[0] ), I( a[1] ), I( a[2] ) } );
	case 93:	// QAA: turn around
		cl.viewangles[YAW] += 180.0f;
		return 0;

	case 100: std::memset( QAA_Ptr( a[0] ), a[1], static_cast<size_t>( a[2] ) ); return 0;
	case 101: std::memmove( QAA_Ptr( a[0] ), QAA_Ptr( a[1] ), static_cast<size_t>( a[2] ) ); return 0;
	case 102:
		strncpy( static_cast<char *>( QAA_Ptr( a[0] ) ), QAA_Str( a[1] ), static_cast<size_t>( a[2] ) );
		return a[0];
	case 103: return QAA_CG_Trunc( sin( QAA_ArgF( a[0] ) ) );
	case 104: return QAA_CG_Trunc( cos( QAA_ArgF( a[0] ) ) );
	case 105: return QAA_CG_Trunc( atan2( QAA_ArgF( a[0] ), QAA_ArgF( a[1] ) ) );
	case 106: return QAA_CG_Trunc( sqrt( QAA_ArgF( a[0] ) ) );
	case 107: return QAA_CG_Trunc( floor( QAA_ArgF( a[0] ) ) );
	case 108: return QAA_CG_Trunc( ceil( QAA_ArgF( a[0] ) ) );
	case 111: return QAA_CG_Trunc( Q_acos( QAA_ArgF( a[0] ) ) );

	case 115:	// the active controller: the PC has one user
		return 0;
	case 141: {	// the game's scoreboard mailbox (game trap 72)
		if ( !qaa_scoreboardMailboxFull ) {
			return 0;
		}
		std::memcpy( QAA_Ptr( a[0] ), qaa_scoreboardMailbox, static_cast<size_t>( qaa_scoreboardMailboxSize ) );
		qaa_scoreboardMailboxFull = false;
		return 1;
	}
	default:
		if ( t >= 112 && t <= 142 ) {
			// Rumble, achievements, Xbox Live and the Scaleform menus: the
			// menus come in M6; without Live the member queries answer 0, as
			// retail answers without a session.
			if ( !qaaCgameWarnedLive && ( t == 113 || t == 114 ) ) {
				qaaCgameWarnedLive = true;
				Com_DPrintf( "QAA: rumble and achievements are not on the PC yet\n" );
			}
			return 0;
		}
		Com_DPrintf( S_COLOR_YELLOW "cgame: unhandled trap %d (from %08X)\n", t, call->return_address );
		return 0;
	}
}

/* ---- the engine function table (vmMain 12) ------------------------------- */

/* A direct call's arguments in trap form: argument 1 is the call's r3. */
static int64_t QAA_CgameTableCall( const qaa_syscall *c, int trap ) {
	qaa_syscall t = *c;
	t.trap = trap;
	t.args[0] = c->trap;
	for ( int i = 1; i < QAA_SYSCALL_MAX_ARGS; i++ ) {
		t.args[i] = c->args[i - 1];
	}
	return QAA_CgameSyscall( nullptr, qaaCgame, &t );
}

#define QAA_CG_TABLE_TRAP( fn, trap ) \
	static int64_t fn( void *, qaa_module *, const qaa_syscall *c ) { return QAA_CgameTableCall( c, trap ); }
QAA_CG_TABLE_TRAP( QAA_CT_RegisterSound, 35 )
QAA_CG_TABLE_TRAP( QAA_CT_UpdateEntityPosition, 33 )
QAA_CG_TABLE_TRAP( QAA_CT_RegisterModel, 39 )
QAA_CG_TABLE_TRAP( QAA_CT_RegisterSkin, 40 )
QAA_CG_TABLE_TRAP( QAA_CT_RegisterShader, 41 )
QAA_CG_TABLE_TRAP( QAA_CT_RegisterShaderNoMip, 60 )
QAA_CG_TABLE_TRAP( QAA_CT_FOpenFile, 11 )
QAA_CG_TABLE_TRAP( QAA_CT_Read, 12 )
QAA_CG_TABLE_TRAP( QAA_CT_FCloseFile, 14 )

static int64_t QAA_CT_RegisterSoundCompressed( void *, qaa_module *, const qaa_syscall *c ) {
	return QAA_CG_Forward( CG_S_REGISTERSOUND, { QAA_HostPtr( c->trap ), 1 } );	// the handle, unlike trap 36
}

static int64_t QAA_CT_LerpTag( void *, qaa_module *, const qaa_syscall *c ) {
	// ( tag, model, startFrame, endFrame, frac (f1, taking the r7 slot), tagName (r8) )
	orientation_t o;
	const float frac = static_cast<float>( c->fargs[0] );
	int32_t fracBits;
	std::memcpy( &fracBits, &frac, 4 );
	const intptr_t r = QAA_CG_Forward( CG_R_LERPTAG, { reinterpret_cast<intptr_t>( &o ), c->args[0], c->args[1],
		c->args[2], fracBits, QAA_HostPtr( c->args[4] ) } );
	QAA_OrientationToGuest( static_cast<uint32_t>( c->trap ), &o );
	return r;
}

/*
 * QAA's renderer lock (+0x2C/+0x30), its flag setters (+0x34..+0x3C) and the
 * engine event the model loader waits on (+0x40/+0x44) serve the cgame's
 * loading thread. openQAA loads models on the main thread
 * (cg_asyncModelLoading 0, see QAA_CgameCreate), so these have nothing to do.
 */
static int64_t QAA_CT_Nothing( void *, qaa_module *, const qaa_syscall * ) {
	return 0;
}

static bool QAA_CL_BuildEngineTable( void ) {
	const qaaGuestApi_t &api = QAA_Host_Api();
	static const qaa_syscall_fn entries[18] = {
		QAA_CT_RegisterSound, QAA_CT_RegisterSoundCompressed, QAA_CT_UpdateEntityPosition, QAA_CT_RegisterModel,
		QAA_CT_RegisterSkin, QAA_CT_RegisterShader, QAA_CT_RegisterShaderNoMip, QAA_CT_LerpTag,
		QAA_CT_FOpenFile, QAA_CT_Read, QAA_CT_FCloseFile, QAA_CT_Nothing, QAA_CT_Nothing, QAA_CT_Nothing,
		QAA_CT_Nothing, QAA_CT_Nothing, QAA_CT_Nothing, QAA_CT_Nothing,
	};
	if ( !qaaCgameEngineTable ) {
		qaaCgameEngineTable = api.qaa_guest_alloc( QAA_Host_Guest(), sizeof( entries ) / sizeof( entries[0] ) * 4 );
		if ( !qaaCgameEngineTable ) {
			return false;
		}
	}
	for ( size_t i = 0; i < sizeof( entries ) / sizeof( entries[0] ); i++ ) {
		const uint32_t fn = api.qaa_module_host_function( qaaCgame, entries[i], nullptr );
		if ( !fn ) {
			return false;
		}
		QAA_StoreU32( qaaCgameEngineTable + 4 * static_cast<uint32_t>( i ), fn );
	}
	return true;
}

/* ---- vmMain -------------------------------------------------------------- */

enum {
	QCG_EVENT_HANDLING = 9, QCG_SET_ENGINE_TABLE = 12,
};

static intptr_t QDECL QAA_CgameEntry( int command, ... ) {
	int32_t args[3];
	va_list ap;
	va_start( ap, command );
	for ( int i = 0; i < 3; i++ ) {
		args[i] = va_arg( ap, int32_t );
	}
	va_end( ap );

	switch ( command ) {
	case CG_INIT: {
		// as retail's CL_InitCGame (0x821023F8): the engine table first
		const int32_t table[1] = { static_cast<int32_t>( qaaCgameEngineTable ) };
		QAA_Host_VmMain( qaaCgame, QCG_SET_ENGINE_TABLE, table, 1 );
		return QAA_Host_VmMain( qaaCgame, CG_INIT, args, 3 );
	}
	case CG_SHUTDOWN:
	case CG_CONSOLE_COMMAND:
	case CG_DRAW_ACTIVE_FRAME:
	case CG_CROSSHAIR_PLAYER:
	case CG_LAST_ATTACKER:
	case CG_KEY_EVENT:
	case CG_MOUSE_EVENT:
		return QAA_Host_VmMain( qaaCgame, command, args, 3 );
	case CG_EVENT_HANDLING:
		return QAA_Host_VmMain( qaaCgame, QCG_EVENT_HANDLING, args, 1 );
	default:
		// Quake Live's additions (chat field, tracked players ...): not in QAA
		return 0;
	}
}

static void QAA_CgameDestroy( vm_t * ) {
	if ( qaaCgame ) {
		QAA_Host_UnloadModule( qaaCgame );
		qaaCgame = nullptr;
	}
	qaaCgameEngineTable = 0;
	qaaCgameWarnedLive = false;
}

static vm_t *QAA_CgameCreate( void );

static vm_t *QAA_CgameRestart( vm_t *vm ) {
	VM_Free( vm );
	return QAA_CgameCreate();
}

static vm_t *QAA_CgameCreate( void ) {
	// PC deviation: the cgame's model loading thread (cg_asyncModelLoading,
	// default 1) calls the renderer from a second thread under QAA's own
	// renderer lock; FnQL's renderer is not thread-safe, so models load on
	// the main thread. Set before the cgame registers the cvar.
	Cvar_Get( "cg_asyncModelLoading", "0", 0 );
	Cvar_Set( "cg_asyncModelLoading", "0" );

	qaaCgame = QAA_Host_LoadModule( "cgame" );
	if ( !qaaCgame ) {
		return nullptr;
	}
	const qaaGuestApi_t &api = QAA_Host_Api();
	if ( api.qaa_module_dll_entry( qaaCgame, QAA_CgameSyscall, nullptr ) != QAA_OK ) {
		Com_Printf( S_COLOR_YELLOW "WARNING: cgame dllEntry failed: %s\n", api.qaa_guest_last_error( QAA_Host_Guest() ) );
		QAA_CgameDestroy( nullptr );
		return nullptr;
	}
	if ( !QAA_CL_BuildEngineTable() ) {
		Com_Printf( S_COLOR_YELLOW "WARNING: could not build cgame's engine table\n" );
		QAA_CgameDestroy( nullptr );
		return nullptr;
	}
	return VM_CreateHosted( VM_CGAME, QAA_CgameEntry, QAA_CgameDestroy, QAA_CgameRestart );
}

static bool QAA_CL_UseRecompiledCgame( void ) {
	cvar_t *var = Cvar_Get( "qaa_game", "1", CVAR_LATCH );
	return var->integer != 0;
}

static vm_t *QAA_CL_LoadCGameVM( void ) {
	return QAA_CL_UseRecompiledCgame() ? QAA_CgameCreate() : nullptr;
}
