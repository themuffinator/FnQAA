/*
===========================================================================
openQAA: Quake Arena Arcade .bpk archive reader.

Copyright (C) 2026 openQAA contributors.
This file is part of openQAA, licensed under the GNU GPL v2 or later.

Layout (verified over baseq3/pak0.bpk, docs/FORMATS.md), all big-endian:
  u32 version (1), u32 count
  count x { u64 name hash, u32 offset, u32 decoded size, u32 flags,
            u32 stored size }
  flags 1: the stored bytes are a native XMemCompress stream (qaa_lzx.c)
===========================================================================
*/
#include "qaa_bpk_archive.h"
#include "qaa_lzx.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BPK_HEADER_SIZE		8
#define BPK_RECORD_SIZE		24
#define BPK_MAX_RECORDS		( 1u << 20 )

struct qaaBpk_s {
	qaaIo_t io;
	uint32_t version;
	uint32_t count;
	uint32_t tableCrc;
	qaaBpkEntry_t *entries;
	uint32_t *byHash;		/* record indices sorted by hash, then index */
};

static void SetError( char *error, size_t errorSize, const char *text ) {
	if ( error && errorSize ) {
		snprintf( error, errorSize, "%s", text );
	}
}

static const qaaBpkEntry_t *sortEntries;

static int CompareByHash( const void *a, const void *b ) {
	const uint32_t ia = *(const uint32_t *)a, ib = *(const uint32_t *)b;
	const uint64_t ha = sortEntries[ia].hash, hb = sortEntries[ib].hash;

	if ( ha != hb ) {
		return ha < hb ? -1 : 1;
	}
	return ia < ib ? -1 : ( ia > ib ? 1 : 0 );
}

qaaBpk_t *QAA_BpkOpen( qaaIo_t *io, char *error, size_t errorSize ) {
	uint8_t header[BPK_HEADER_SIZE];
	uint8_t *table;
	qaaBpk_t *bpk;
	uint32_t i;
	size_t tableSize;

	SetError( error, errorSize, "" );
	if ( !io || QAA_IoRead( io, 0, header, sizeof( header ) ) != 0 ) {
		SetError( error, errorSize, "file too small for a BPK header" );
		return NULL;
	}
	bpk = (qaaBpk_t *)calloc( 1, sizeof( *bpk ) );
	if ( !bpk ) {
		SetError( error, errorSize, "out of memory" );
		return NULL;
	}
	bpk->version = QAA_ReadBE32( header );
	bpk->count = QAA_ReadBE32( header + 4 );
	if ( bpk->version != 1 ) {
		free( bpk );
		SetError( error, errorSize, "unknown BPK version (expected 1)" );
		return NULL;
	}
	if ( bpk->count > BPK_MAX_RECORDS
		|| BPK_HEADER_SIZE + (uint64_t)bpk->count * BPK_RECORD_SIZE > io->size ) {
		free( bpk );
		SetError( error, errorSize, "BPK record table is out of bounds" );
		return NULL;
	}
	tableSize = (size_t)bpk->count * BPK_RECORD_SIZE;
	table = (uint8_t *)malloc( tableSize ? tableSize : 1 );
	bpk->entries = (qaaBpkEntry_t *)calloc( bpk->count ? bpk->count : 1, sizeof( qaaBpkEntry_t ) );
	bpk->byHash = (uint32_t *)malloc( ( bpk->count ? bpk->count : 1 ) * sizeof( uint32_t ) );
	if ( !table || !bpk->entries || !bpk->byHash ) {
		free( table );
		free( bpk->entries );
		free( bpk->byHash );
		free( bpk );
		SetError( error, errorSize, "out of memory" );
		return NULL;
	}
	if ( QAA_IoRead( io, BPK_HEADER_SIZE, table, tableSize ) != 0 ) {
		free( table );
		free( bpk->entries );
		free( bpk->byHash );
		free( bpk );
		SetError( error, errorSize, "cannot read the BPK record table" );
		return NULL;
	}
	bpk->tableCrc = QAA_Crc32( QAA_Crc32( 0, header, sizeof( header ) ), table, tableSize );
	for ( i = 0; i < bpk->count; i++ ) {
		const uint8_t *p = table + (size_t)i * BPK_RECORD_SIZE;
		qaaBpkEntry_t *e = &bpk->entries[i];

		e->hash = QAA_ReadBE64( p );
		e->offset = QAA_ReadBE32( p + 8 );
		e->size = QAA_ReadBE32( p + 12 );
		e->flags = QAA_ReadBE32( p + 16 );
		e->storedSize = QAA_ReadBE32( p + 20 );
		if ( (uint64_t)e->offset + e->storedSize > io->size ) {
			free( table );
			free( bpk->entries );
			free( bpk->byHash );
			free( bpk );
			SetError( error, errorSize, "a BPK record points outside the archive" );
			return NULL;
		}
		bpk->byHash[i] = i;
	}
	free( table );
	sortEntries = bpk->entries;
	qsort( bpk->byHash, bpk->count, sizeof( uint32_t ), CompareByHash );
	sortEntries = NULL;
	bpk->io = *io;
	memset( io, 0, sizeof( *io ) );
	return bpk;
}

void QAA_BpkClose( qaaBpk_t *bpk ) {
	if ( bpk ) {
		QAA_IoClose( &bpk->io );
		free( bpk->entries );
		free( bpk->byHash );
		free( bpk );
	}
}

uint32_t QAA_BpkVersion( const qaaBpk_t *bpk ) {
	return bpk->version;
}

uint32_t QAA_BpkCount( const qaaBpk_t *bpk ) {
	return bpk->count;
}

const qaaBpkEntry_t *QAA_BpkEntry( const qaaBpk_t *bpk, uint32_t index ) {
	return index < bpk->count ? &bpk->entries[index] : NULL;
}

int QAA_BpkFind( const qaaBpk_t *bpk, uint64_t hash ) {
	uint32_t lo = 0, hi = bpk->count;

	while ( lo < hi ) {
		const uint32_t mid = lo + ( hi - lo ) / 2;
		if ( bpk->entries[bpk->byHash[mid]].hash < hash ) {
			lo = mid + 1;
		} else {
			hi = mid;
		}
	}
	if ( lo < bpk->count && bpk->entries[bpk->byHash[lo]].hash == hash ) {
		return (int)bpk->byHash[lo];
	}
	return -1;
}

int QAA_BpkRead( const qaaBpk_t *bpk, uint32_t index, void *out, size_t outSize ) {
	const qaaBpkEntry_t *e;
	uint8_t *stored;
	int result;

	if ( !bpk || index >= bpk->count || ( !out && outSize ) ) {
		return QAA_BPK_ERR_ARG;
	}
	e = &bpk->entries[index];
	if ( outSize != e->size ) {
		return QAA_BPK_ERR_ARG;
	}
	if ( e->flags == 0 ) {
		if ( e->storedSize != e->size ) {
			return QAA_BPK_ERR_FLAGS;
		}
		return QAA_IoRead( &bpk->io, e->offset, out, outSize ) == 0 ? QAA_BPK_OK : QAA_BPK_ERR_IO;
	}
	if ( e->flags != QAA_BPK_FLAG_XMEM ) {
		return QAA_BPK_ERR_FLAGS;
	}
	stored = (uint8_t *)malloc( e->storedSize ? e->storedSize : 1 );
	if ( !stored ) {
		return QAA_BPK_ERR_MEMORY;
	}
	if ( QAA_IoRead( &bpk->io, e->offset, stored, e->storedSize ) != 0 ) {
		free( stored );
		return QAA_BPK_ERR_IO;
	}
	result = qaa_lzx_decode_native( stored, e->storedSize, (uint8_t *)out, outSize );
	free( stored );
	return result == 0 ? QAA_BPK_OK : QAA_BPK_ERR_DECODE;
}

uint32_t QAA_BpkTableCrc( const qaaBpk_t *bpk ) {
	return bpk->tableCrc;
}

const char *QAA_BpkError( int code ) {
	switch ( code ) {
	case QAA_BPK_OK: return "no error";
	case QAA_BPK_ERR_ARG: return "bad argument or output size";
	case QAA_BPK_ERR_IO: return "read error";
	case QAA_BPK_ERR_MEMORY: return "out of memory";
	case QAA_BPK_ERR_FLAGS: return "unsupported record flags";
	case QAA_BPK_ERR_DECODE: return "XMemCompress stream did not decode";
	default: return "unknown error";
	}
}
