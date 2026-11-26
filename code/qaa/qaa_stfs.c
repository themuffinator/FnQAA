/*
===========================================================================
openQAA: Xbox 360 STFS (CON/LIVE/PIRS) package reader.

Copyright (C) 2026 openQAA contributors.
This file is part of openQAA, licensed under the GNU GPL v2 or later.

Layout (Free60 STFS documentation; tools/stfs.py is the reference reader
and docs/FORMATS.md records what the QAA package was verified to hold):
  0x000  magic 'CON ', 'LIVE' or 'PIRS'
  0x340  header size (BE u32); data blocks start at the next 4 KiB boundary
  0x344  content type (BE u32), 0x360 title ID (BE u32)
  0x37B  volume descriptor flags: bit 0 set = one hash table per level
         (read-only package), clear = two
  0x37C  file table block count (LE u16), 0x37E first block (LE u24)
  0x395  allocated block count (BE u32)
  file table entries are 0x40 bytes: name[0x28], flags (name length | 0x40
  contiguous | 0x80 directory), valid blocks (LE u24) at 0x29, allocated
  blocks (LE u24) at 0x2C, start block (LE u24) at 0x2F, parent (BE s16) at
  0x32, size (BE u32) at 0x34, timestamps.
  A level-0 hash table holds 0xAA records of 0x18 bytes; a record's next
  block number is a BE u24 at +0x15.

Like tools/stfs.py, the reader always follows the first hash table of each
level. That is exact for read-only packages, which XBLA packages are; for
packages with two tables per level it would need the active-table bit.
===========================================================================
*/
#include "qaa_stfs.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STFS_HEADER_READ	0x400
#define STFS_ENTRY_SIZE		0x40
#define STFS_PER_TABLE		0xAA
#define STFS_PER_L1			0x70E4
#define STFS_MAX_ENTRIES	65536

typedef struct {
	char name[0x29];
	int directory;
	int contiguous;
	uint32_t valid;
	uint32_t start;
	int parent;
	uint32_t size;
	char path[QAA_STFS_MAX_PATH];
} stfsEntry_t;

struct qaaStfs_s {
	qaaIo_t io;
	int refs;
	uint32_t magic;
	uint32_t titleId;
	uint32_t contentType;
	uint32_t headerSize;
	uint32_t shift;			/* 0: one table per level, 1: two */
	uint32_t step0;
	uint64_t base;
	int numEntries;
	stfsEntry_t *entries;
};

static void SetError( char *error, size_t errorSize, const char *text ) {
	if ( error && errorSize ) {
		snprintf( error, errorSize, "%s", text );
	}
}

static uint32_t BackingDataBlock( const qaaStfs_t *s, uint32_t n ) {
	const uint32_t sh = s->shift;
	uint32_t r = ( ( ( n + STFS_PER_TABLE ) / STFS_PER_TABLE ) << sh ) + n;

	if ( n < STFS_PER_TABLE ) {
		return r;
	}
	if ( n < STFS_PER_L1 ) {
		return r + ( ( ( n + STFS_PER_L1 ) / STFS_PER_L1 ) << sh );
	}
	return ( 1u << sh ) + r + ( ( ( n + STFS_PER_L1 ) / STFS_PER_L1 ) << sh );
}

static uint32_t Level0HashBlock( const qaaStfs_t *s, uint32_t n ) {
	const uint32_t sh = s->shift;
	uint32_t r;

	if ( n < STFS_PER_TABLE ) {
		return 0;
	}
	r = ( n / STFS_PER_TABLE ) * s->step0;
	r += ( ( n / STFS_PER_L1 ) + 1 ) << sh;
	if ( n / STFS_PER_L1 == 0 ) {
		return r;
	}
	return r + ( 1u << sh );
}

static uint64_t BlockAddress( const qaaStfs_t *s, uint32_t backing ) {
	return s->base + (uint64_t)backing * QAA_STFS_BLOCK_SIZE;
}

static int ReadDataBlock( const qaaStfs_t *s, uint32_t n, uint32_t offset, void *buffer, size_t length ) {
	return QAA_IoRead( &s->io, BlockAddress( s, BackingDataBlock( s, n ) ) + offset, buffer, length );
}

static int NextBlock( const qaaStfs_t *s, uint32_t n, uint32_t *next ) {
	uint8_t raw[3];
	const uint64_t at = BlockAddress( s, Level0HashBlock( s, n ) ) + ( n % STFS_PER_TABLE ) * 0x18 + 0x15;

	if ( QAA_IoRead( &s->io, at, raw, sizeof( raw ) ) != 0 ) {
		return -1;
	}
	*next = QAA_ReadBE24( raw );
	return 0;
}

static int ReadFileTable( qaaStfs_t *s, uint32_t firstBlock, uint32_t blockCount, char *error, size_t errorSize ) {
	uint8_t *raw;
	uint32_t i, block = firstBlock;
	int count = 0, e;

	if ( blockCount == 0 || blockCount > STFS_MAX_ENTRIES * STFS_ENTRY_SIZE / QAA_STFS_BLOCK_SIZE ) {
		SetError( error, errorSize, "bad STFS file table size" );
		return -1;
	}
	raw = (uint8_t *)malloc( (size_t)blockCount * QAA_STFS_BLOCK_SIZE );
	if ( !raw ) {
		SetError( error, errorSize, "out of memory" );
		return -1;
	}
	for ( i = 0; i < blockCount; i++ ) {
		if ( ReadDataBlock( s, block, 0, raw + (size_t)i * QAA_STFS_BLOCK_SIZE, QAA_STFS_BLOCK_SIZE ) != 0
			|| ( i + 1 < blockCount && NextBlock( s, block, &block ) != 0 ) ) {
			free( raw );
			SetError( error, errorSize, "STFS file table is out of bounds" );
			return -1;
		}
	}

	s->entries = (stfsEntry_t *)calloc( (size_t)blockCount * QAA_STFS_BLOCK_SIZE / STFS_ENTRY_SIZE, sizeof( stfsEntry_t ) );
	if ( !s->entries ) {
		free( raw );
		SetError( error, errorSize, "out of memory" );
		return -1;
	}
	for ( i = 0; i < blockCount * QAA_STFS_BLOCK_SIZE / STFS_ENTRY_SIZE; i++ ) {
		const uint8_t *p = raw + (size_t)i * STFS_ENTRY_SIZE;
		const uint32_t nameLength = p[0x28] & 0x3F;
		stfsEntry_t *entry = &s->entries[count];

		if ( nameLength == 0 ) {
			break;
		}
		memcpy( entry->name, p, nameLength );
		entry->name[nameLength] = '\0';
		entry->directory = ( p[0x28] & 0x80 ) ? 1 : 0;
		entry->contiguous = ( p[0x28] & 0x40 ) ? 1 : 0;
		entry->valid = QAA_ReadLE24( p + 0x29 );
		entry->start = QAA_ReadLE24( p + 0x2F );
		entry->parent = (int16_t)QAA_ReadBE16( p + 0x32 );
		entry->size = QAA_ReadBE32( p + 0x34 );
		count++;
	}
	free( raw );
	s->numEntries = count;

	/* Paths from the parent chain; entries are indexed by table position,
	 * which equals their index here because the table stops at the first
	 * empty record. */
	for ( e = 0; e < count; e++ ) {
		const char *parts[64];
		int numParts = 0, p = e;
		size_t used = 0;

		/* like tools/stfs.py, a parent outside the table ends the chain */
		while ( p >= 0 && p < count ) {
			if ( numParts == 64 ) {
				SetError( error, errorSize, "STFS directory tree is too deep or has a cycle" );
				return -1;
			}
			parts[numParts++] = s->entries[p].name;
			p = s->entries[p].parent;
		}
		while ( numParts-- > 0 ) {
			const size_t len = strlen( parts[numParts] );
			if ( used + len + 2 > sizeof( s->entries[e].path ) ) {
				SetError( error, errorSize, "STFS path too long" );
				return -1;
			}
			memcpy( s->entries[e].path + used, parts[numParts], len );
			used += len;
			if ( numParts ) {
				s->entries[e].path[used++] = '/';
			}
		}
		s->entries[e].path[used] = '\0';
	}
	return 0;
}

qaaStfs_t *QAA_StfsOpen( qaaIo_t *io, char *error, size_t errorSize ) {
	uint8_t hdr[STFS_HEADER_READ];
	qaaStfs_t *s;
	uint32_t magic, flags, ftCount, ftBlock;

	SetError( error, errorSize, "" );
	if ( !io || QAA_IoRead( io, 0, hdr, sizeof( hdr ) ) != 0 ) {
		SetError( error, errorSize, "file too small for an STFS header" );
		return NULL;
	}
	magic = QAA_ReadBE32( hdr );
	if ( magic != 0x434F4E20u && magic != 0x4C495645u && magic != 0x50495253u ) {
		SetError( error, errorSize, "not an STFS package (no CON/LIVE/PIRS magic)" );
		return NULL;
	}
	s = (qaaStfs_t *)calloc( 1, sizeof( *s ) );
	if ( !s ) {
		SetError( error, errorSize, "out of memory" );
		return NULL;
	}
	s->io = *io;
	s->refs = 1;
	s->magic = magic;
	s->headerSize = QAA_ReadBE32( hdr + 0x340 );
	s->contentType = QAA_ReadBE32( hdr + 0x344 );
	s->titleId = QAA_ReadBE32( hdr + 0x360 );
	flags = hdr[0x37B];
	s->shift = ( flags & 1 ) ? 0 : 1;
	s->step0 = s->shift == 0 ? 0xAB : 0xAC;
	ftCount = hdr[0x37C] | ( (uint32_t)hdr[0x37D] << 8 );
	ftBlock = QAA_ReadLE24( hdr + 0x37E );
	s->base = ( (uint64_t)s->headerSize + 0xFFF ) & ~(uint64_t)0xFFF;
	if ( s->headerSize < STFS_HEADER_READ || s->base >= io->size ) {
		free( s );
		SetError( error, errorSize, "bad STFS header size" );
		return NULL;
	}
	if ( ReadFileTable( s, ftBlock, ftCount, error, errorSize ) != 0 ) {
		free( s->entries );
		free( s );
		return NULL;
	}
	memset( io, 0, sizeof( *io ) );
	return s;
}

static void StfsRelease( qaaStfs_t *s ) {
	if ( s && --s->refs == 0 ) {
		QAA_IoClose( &s->io );
		free( s->entries );
		free( s );
	}
}

void QAA_StfsClose( qaaStfs_t *stfs ) {
	StfsRelease( stfs );
}

uint32_t QAA_StfsMagic( const qaaStfs_t *stfs ) {
	return stfs->magic;
}

uint32_t QAA_StfsTitleId( const qaaStfs_t *stfs ) {
	return stfs->titleId;
}

uint32_t QAA_StfsContentType( const qaaStfs_t *stfs ) {
	return stfs->contentType;
}

int QAA_StfsFileCount( const qaaStfs_t *stfs ) {
	return stfs->numEntries;
}

int QAA_StfsFileInfo( const qaaStfs_t *stfs, int index, qaaStfsFileInfo_t *info ) {
	const stfsEntry_t *e;

	if ( index < 0 || index >= stfs->numEntries ) {
		return -1;
	}
	e = &stfs->entries[index];
	memset( info, 0, sizeof( *info ) );
	memcpy( info->path, e->path, sizeof( info->path ) );
	info->directory = e->directory;
	info->contiguous = e->contiguous;
	info->startBlock = e->start;
	info->validBlocks = e->valid;
	info->size = e->size;
	return 0;
}

static int PathEqual( const char *a, const char *b ) {
	for ( ;; a++, b++ ) {
		int ca = ( *a == '\\' ) ? '/' : tolower( (unsigned char)*a );
		int cb = ( *b == '\\' ) ? '/' : tolower( (unsigned char)*b );
		if ( ca != cb ) {
			return 0;
		}
		if ( !ca ) {
			return 1;
		}
	}
}

int QAA_StfsFind( const qaaStfs_t *stfs, const char *path ) {
	int i;

	while ( *path == '/' || *path == '\\' ) {
		path++;
	}
	for ( i = 0; i < stfs->numEntries; i++ ) {
		if ( PathEqual( stfs->entries[i].path, path ) ) {
			return i;
		}
	}
	return -1;
}

typedef struct {
	qaaStfs_t *stfs;
	uint32_t numBlocks;
	uint32_t start;			/* contiguous files: first data block */
	uint32_t *blocks;		/* fragmented files: data block of each 4 KiB */
} stfsFileCtx_t;

static uint32_t FileDataBlock( const stfsFileCtx_t *f, uint32_t i ) {
	return f->blocks ? f->blocks[i] : f->start + i;
}

static int StfsFileRead( void *ctx, uint64_t offset, void *buffer, size_t length ) {
	const stfsFileCtx_t *f = (const stfsFileCtx_t *)ctx;
	uint8_t *out = (uint8_t *)buffer;

	while ( length ) {
		const uint32_t index = (uint32_t)( offset / QAA_STFS_BLOCK_SIZE );
		const uint32_t within = (uint32_t)( offset % QAA_STFS_BLOCK_SIZE );
		uint64_t address;
		size_t run;
		uint32_t next;

		if ( index >= f->numBlocks ) {
			return -1;
		}
		address = BlockAddress( f->stfs, BackingDataBlock( f->stfs, FileDataBlock( f, index ) ) );
		run = QAA_STFS_BLOCK_SIZE - within;
		/* extend the read over data blocks that are adjacent on disk */
		for ( next = index + 1; run < length && next < f->numBlocks; next++ ) {
			if ( BlockAddress( f->stfs, BackingDataBlock( f->stfs, FileDataBlock( f, next ) ) )
				!= address + (uint64_t)( next - index ) * QAA_STFS_BLOCK_SIZE ) {
				break;
			}
			run += QAA_STFS_BLOCK_SIZE;
		}
		if ( run > length ) {
			run = length;
		}
		if ( QAA_IoRead( &f->stfs->io, address + within, out, run ) != 0 ) {
			return -1;
		}
		out += run;
		offset += run;
		length -= run;
	}
	return 0;
}

static void StfsFileClose( void *ctx ) {
	stfsFileCtx_t *f = (stfsFileCtx_t *)ctx;

	if ( f ) {
		StfsRelease( f->stfs );
		free( f->blocks );
		free( f );
	}
}

int QAA_StfsOpenFile( qaaStfs_t *stfs, int index, qaaIo_t *io ) {
	const stfsEntry_t *e;
	stfsFileCtx_t *f;
	uint32_t i;

	memset( io, 0, sizeof( *io ) );
	if ( index < 0 || index >= stfs->numEntries ) {
		return -1;
	}
	e = &stfs->entries[index];
	if ( e->directory || (uint64_t)e->size > (uint64_t)e->valid * QAA_STFS_BLOCK_SIZE ) {
		return -1;
	}
	f = (stfsFileCtx_t *)calloc( 1, sizeof( *f ) );
	if ( !f ) {
		return -1;
	}
	f->stfs = stfs;
	f->numBlocks = e->valid;
	f->start = e->start;
	if ( !e->contiguous && e->valid ) {
		f->blocks = (uint32_t *)malloc( (size_t)e->valid * sizeof( uint32_t ) );
		if ( !f->blocks ) {
			free( f );
			return -1;
		}
		f->blocks[0] = e->start;
		for ( i = 1; i < e->valid; i++ ) {
			if ( NextBlock( stfs, f->blocks[i - 1], &f->blocks[i] ) != 0 ) {
				free( f->blocks );
				free( f );
				return -1;
			}
		}
	}
	/* the last block must lie inside the package */
	if ( e->valid && BlockAddress( stfs, BackingDataBlock( stfs, FileDataBlock( f, e->valid - 1 ) ) )
		>= stfs->io.size ) {
		free( f->blocks );
		free( f );
		return -1;
	}
	stfs->refs++;
	io->ctx = f;
	io->size = e->size;
	io->read = StfsFileRead;
	io->close = StfsFileClose;
	return 0;
}
