/*
===========================================================================
openQAA: the engine side of the guest runtime.

Copyright (C) 2026 openQAA contributors.
This file is part of openQAA, licensed under the GNU GPL v2 or later.

QAA's game modules are recompiled from the user's XBLA package into native
libraries (tools/recomp/build.py) that run on the guest runtime
(code/guest). The runtime is built with clang and loaded at run time, so
the engine binds its C API (code/guest/include/qaa_guest.h) by name and
stays free to build with any compiler.

One guest address space serves both modules, as one did on the Xbox: a
listen server runs qagame and cgame side by side. Guest data is big-endian
and guest pointers are 32-bit guest addresses; the helpers below read and
write it.
===========================================================================
*/
#ifndef QAA_GUEST_HOST_H
#define QAA_GUEST_HOST_H

#include "../guest/include/qaa_guest.h"

#include <cstdint>
#include <cstring>

/* The runtime's C API, bound by name from the shared runtime library. */
struct qaaGuestApi_t {
#define QAA_GUEST_FN( name ) decltype( &::name ) name;
#include "qaa_guest_api.inc"
#undef QAA_GUEST_FN
};

/* Loads the runtime and creates the guest, once. qfalse if unavailable. */
bool QAA_Host_Init( void );
void QAA_Host_Shutdown( void );
bool QAA_Host_Available( void );

const qaaGuestApi_t &QAA_Host_Api( void );
qaa_guest *QAA_Host_Guest( void );

/* "qagame" or "cgame": loads qaa_<name> from the module path. */
qaa_module *QAA_Host_LoadModule( const char *name );
void QAA_Host_UnloadModule( qaa_module *module );

/*
 * Calls into a module. An engine error (Com_Error) raised while the guest
 * is running unwinds the guest first and is raised again here, once the
 * call is back on the engine side. A guest fault becomes ERR_DROP.
 */
intptr_t QAA_Host_VmMain( qaa_module *module, int command, const int32_t *args, int nargs );
int64_t QAA_Host_Call( uint32_t function, const int64_t *args, int nargs );

/*
 * QAA_Host_InGuest: nonzero while this thread is inside a call into the
 * guest. QAA_Host_ComErrorHook: called by Com_Error before it unwinds; inside
 * the guest it records the error and unwinds the guest (does not return).
 * Both are declared with C linkage in qcommon.h.
 */

/* ---- guest memory --------------------------------------------------- */

extern uint8_t *qaa_guestBase;	/* host address of guest address 0 */

static inline void *QAA_Ptr( uint32_t address ) {
	return address ? qaa_guestBase + address : nullptr;
}
static inline const char *QAA_Str( uint32_t address ) {
	return address ? reinterpret_cast<const char *>( qaa_guestBase + address ) : "";
}

static inline uint32_t QAA_Swap32( uint32_t v ) {
	return ( v >> 24 ) | ( ( v >> 8 ) & 0xFF00u ) | ( ( v << 8 ) & 0xFF0000u ) | ( v << 24 );
}
static inline uint16_t QAA_Swap16( uint16_t v ) {
	return static_cast<uint16_t>( ( v >> 8 ) | ( v << 8 ) );
}

static inline uint32_t QAA_LoadU32( uint32_t address ) {
	uint32_t v;
	std::memcpy( &v, qaa_guestBase + address, 4 );
	return QAA_Swap32( v );
}
static inline int32_t QAA_LoadS32( uint32_t address ) {
	return static_cast<int32_t>( QAA_LoadU32( address ) );
}
static inline float QAA_LoadF( uint32_t address ) {
	const uint32_t u = QAA_LoadU32( address );
	float f;
	std::memcpy( &f, &u, 4 );
	return f;
}
static inline uint16_t QAA_LoadU16( uint32_t address ) {
	uint16_t v;
	std::memcpy( &v, qaa_guestBase + address, 2 );
	return QAA_Swap16( v );
}
static inline uint8_t QAA_LoadU8( uint32_t address ) {
	return qaa_guestBase[address];
}
static inline void QAA_StoreU32( uint32_t address, uint32_t value ) {
	const uint32_t v = QAA_Swap32( value );
	std::memcpy( qaa_guestBase + address, &v, 4 );
}
static inline void QAA_StoreS32( uint32_t address, int32_t value ) {
	QAA_StoreU32( address, static_cast<uint32_t>( value ) );
}
static inline void QAA_StoreF( uint32_t address, float value ) {
	uint32_t u;
	std::memcpy( &u, &value, 4 );
	QAA_StoreU32( address, u );
}
static inline void QAA_StoreU16( uint32_t address, uint16_t value ) {
	const uint16_t v = QAA_Swap16( value );
	std::memcpy( qaa_guestBase + address, &v, 2 );
}
static inline void QAA_StoreU8( uint32_t address, uint8_t value ) {
	qaa_guestBase[address] = value;
}

static inline void QAA_LoadVec3( uint32_t address, float *v ) {
	v[0] = QAA_LoadF( address );
	v[1] = QAA_LoadF( address + 4 );
	v[2] = QAA_LoadF( address + 8 );
}
static inline void QAA_StoreVec3( uint32_t address, const float *v ) {
	QAA_StoreF( address, v[0] );
	QAA_StoreF( address + 4, v[1] );
	QAA_StoreF( address + 8, v[2] );
}

/* A float passed in an integer trap argument (PASSFLOAT). */
static inline float QAA_ArgF( int32_t arg ) {
	float f;
	std::memcpy( &f, &arg, 4 );
	return f;
}
/* A float result returned as its bits in r3. */
static inline int32_t QAA_RetF( float f ) {
	int32_t i;
	std::memcpy( &i, &f, 4 );
	return i;
}

#endif
