/*
===========================================================================
FnQAA: Quake Arena Arcade .bpk archive names.

Copyright (C) 2026 FnQAA contributors.
This file is part of FnQAA, licensed under the GNU GPL v2 or later.

The engine looks entries up by a hash of the path (default.xex: lookup at
0x82120C98, table setup at 0x82124EF0). The path is first normalised as the
retail lookup does it: '/' becomes '\', and A-Z are lowercased (ASCII only,
0x821DCAF0). The state starts at 0xFAC432B10CD5E44A, and every byte, sign
extended, is folded in MSB first:

    h = ( h << 8 ) ^ T[h >> 56] ^ c

where T[i] is the carry-less product of i and 0x0060034000F0D50B, mod 2^64.
This is not a textbook CRC table, and the sign extension matters for bytes
of 0x80 and above.
===========================================================================
*/
#include "qaa_bpk.h"

#define BPK_POLY UINT64_C( 0x0060034000F0D50B )
#define BPK_SEED UINT64_C( 0xFAC432B10CD5E44A )

static uint64_t bpkTable[256];
static int      bpkTableReady;

static void BPK_InitTable( void ) {
	for ( unsigned i = 0; i < 256; i++ ) {
		uint64_t c = 0;
		unsigned b = i;
		for ( int bit = 0; bit < 8; bit++ ) {
			c <<= 1;
			if ( b & 0x80 ) {
				c ^= BPK_POLY;
			}
			b <<= 1;
		}
		bpkTable[i] = c;
	}
	bpkTableReady = 1;
}

uint64_t qaa_bpk_name_hash( const char *path ) {
	uint64_t h = BPK_SEED;

	if ( !bpkTableReady ) {
		BPK_InitTable();
	}
	if ( !path ) {
		return h;
	}
	for ( const unsigned char *p = (const unsigned char *)path; *p; p++ ) {
		int c = *p;
		if ( c == '/' ) {
			c = '\\';
		} else if ( c >= 'A' && c <= 'Z' ) {
			c += 'a' - 'A';
		}
		/* the retail code sign-extends each byte before folding it in */
		h = ( h << 8 ) ^ bpkTable[h >> 56] ^ (uint64_t)(uint32_t)(int32_t)(signed char)c;
	}
	return h;
}

void qaa_bpk_name_hash_many( const char *names, size_t count, uint64_t *out ) {
	for ( size_t i = 0; i < count; i++ ) {
		out[i] = qaa_bpk_name_hash( names );
		while ( *names ) {
			names++;
		}
		names++;
	}
}
