/*
===========================================================================
openQAA: synthetic-data tests for the STFS and BPK readers (code/qaa).

Copyright (C) 2026 openQAA contributors.
This file is part of openQAA, licensed under the GNU GPL v2 or later.

Builds a small read-only STFS package in memory, independently of the
reader's block arithmetic: data block n lives at backing block n + 1 below
0xAA (block 0 is the first level-0 hash table) and at n + 3 from 0xAA up to
0x154 (the level-1 table and the second level-0 table come first). The
package holds a fragmented baseq3/pak0.bpk whose records are stored raw, as
an XMemCompress stream (one uncompressed LZX block) and with an unknown flag.
===========================================================================
*/
#include "qaa_bpk.h"
#include "qaa_stfs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

#define CHECK( cond ) do { \
	if ( !( cond ) ) { \
		fprintf( stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #cond ); \
		failures++; \
	} \
} while ( 0 )

#define BLOCK 0x1000
#define HEADER_SIZE 0x1000		/* data blocks start at 0x1000 */
#define MAX_DATA_BLOCK 0x153

static void PutBE16( uint8_t *p, uint32_t v ) { p[0] = (uint8_t)( v >> 8 ); p[1] = (uint8_t)v; }
static void PutBE24( uint8_t *p, uint32_t v ) { p[0] = (uint8_t)( v >> 16 ); p[1] = (uint8_t)( v >> 8 ); p[2] = (uint8_t)v; }
static void PutBE32( uint8_t *p, uint32_t v ) { PutBE16( p, v >> 16 ); PutBE16( p + 2, v ); }
static void PutBE64( uint8_t *p, uint64_t v ) { PutBE32( p, (uint32_t)( v >> 32 ) ); PutBE32( p + 4, (uint32_t)v ); }
static void PutLE16( uint8_t *p, uint32_t v ) { p[0] = (uint8_t)v; p[1] = (uint8_t)( v >> 8 ); }
static void PutLE24( uint8_t *p, uint32_t v ) { p[0] = (uint8_t)v; p[1] = (uint8_t)( v >> 8 ); p[2] = (uint8_t)( v >> 16 ); }
static void PutLE32( uint8_t *p, uint32_t v ) { PutLE16( p, v ); PutLE16( p + 2, v >> 16 ); }

static uint32_t Backing( uint32_t n ) {
	return n < 0xAA ? n + 1 : n + 3;
}

static uint8_t *DataBlock( uint8_t *package, uint32_t n ) {
	return package + HEADER_SIZE + (size_t)Backing( n ) * BLOCK;
}

static void SetNext( uint8_t *package, uint32_t n, uint32_t next ) {
	/* the level-0 table for blocks 0..0xA9 is backing block 0 */
	PutBE24( package + HEADER_SIZE + n * 0x18 + 0x15, next );
}

static void FileEntry( uint8_t *table, int index, const char *name, int directory, int contiguous,
	uint32_t start, uint32_t valid, int parent, uint32_t size ) {
	uint8_t *e = table + index * 0x40;
	const size_t len = strlen( name );

	memcpy( e, name, len );
	e[0x28] = (uint8_t)( len | ( contiguous ? 0x40 : 0 ) | ( directory ? 0x80 : 0 ) );
	PutLE24( e + 0x29, valid );
	PutLE24( e + 0x2C, valid );
	PutLE24( e + 0x2F, start );
	PutBE16( e + 0x32, (uint32_t)(uint16_t)(int16_t)parent );
	PutBE32( e + 0x34, size );
}

/* A native XMemCompress stream holding `data` as one uncompressed LZX block. */
static size_t MakeXMem( uint8_t *out, const uint8_t *data, uint32_t length ) {
	const uint32_t frame = 4 + 12 + length + ( length & 1 );
	const uint32_t block = 5 + frame;
	const uint32_t bits = ( 3u << 28 ) | ( length << 4 );	/* E8 flag 0, type 3, 24-bit length */
	uint8_t *p = out;

	memset( out, 0, 0x30 );
	PutBE32( p, 0x0FF512EEu );
	PutBE16( p + 4, 0x0103 );
	PutBE32( p + 0x10, 0x8000 );	/* window */
	PutBE32( p + 0x14, 0x8000 );	/* partition */
	PutBE64( p + 0x18, length );
	PutBE64( p + 0x20, block + 4 );
	PutBE32( p + 0x28, 0x8000 );	/* block size */
	PutBE32( p + 0x2C, block );
	p += 0x30;
	PutBE32( p, block );
	p += 4;
	p[0] = 0xFF;
	PutBE16( p + 1, length );
	PutBE16( p + 3, frame );
	p += 5;
	PutLE16( p, bits >> 16 );		/* 16-bit little-endian words, MSB first */
	PutLE16( p + 2, bits & 0xFFFF );
	p += 4;
	PutLE32( p, 1 );				/* R0, R1, R2 */
	PutLE32( p + 4, 1 );
	PutLE32( p + 8, 1 );
	p += 12;
	memcpy( p, data, length );
	p += length;
	if ( length & 1 ) {
		*p++ = 0;
	}
	return (size_t)( p - out );
}

static const char kRaw[] = "stored record\n";
static const char kPacked[] = "hello, XMemCompress";	/* odd length: exercises the pad byte */
#define HASH_RAW	0x1111111111111111ull
#define HASH_PACKED	0x0123456789ABCDEFull
#define HASH_BAD	0xFFFF000000000000ull

static size_t MakeBpk( uint8_t *out ) {
	uint8_t packed[256];
	const size_t packedSize = MakeXMem( packed, (const uint8_t *)kPacked, (uint32_t)strlen( kPacked ) );
	const uint32_t table = 8 + 3 * 24;
	uint32_t offset = table;
	uint8_t *r;

	PutBE32( out, 1 );
	PutBE32( out + 4, 3 );
	/* records out of hash order, as nothing guarantees the order */
	r = out + 8;
	PutBE64( r, HASH_RAW ); PutBE32( r + 8, offset ); PutBE32( r + 12, (uint32_t)strlen( kRaw ) );
	PutBE32( r + 16, 0 ); PutBE32( r + 20, (uint32_t)strlen( kRaw ) );
	memcpy( out + offset, kRaw, strlen( kRaw ) );
	offset += (uint32_t)strlen( kRaw );
	r += 24;
	PutBE64( r, HASH_BAD ); PutBE32( r + 8, offset ); PutBE32( r + 12, 4 ); PutBE32( r + 16, 7 ); PutBE32( r + 20, 4 );
	memcpy( out + offset, "????", 4 );
	offset += 4;
	r += 24;
	PutBE64( r, HASH_PACKED ); PutBE32( r + 8, offset ); PutBE32( r + 12, (uint32_t)strlen( kPacked ) );
	PutBE32( r + 16, 1 ); PutBE32( r + 20, (uint32_t)packedSize );
	memcpy( out + offset, packed, packedSize );
	offset += (uint32_t)packedSize;
	return offset;
}

static uint8_t BigByte( uint32_t i ) {
	return (uint8_t)( ( i * 2654435761u ) >> 24 );
}

#define BIG_START	0x10
#define BIG_BLOCKS	0xA0	/* data blocks 0x10..0xAF: crosses the 0xAA table boundary */
#define BIG_SIZE	( BIG_BLOCKS * BLOCK - 123 )
static const uint32_t kBpkBlocks[3] = { 4, 2, 3 };	/* fragmented */

static uint8_t *MakePackage( size_t *size, uint8_t *bpk, size_t *bpkSize ) {
	const size_t total = HEADER_SIZE + (size_t)( Backing( MAX_DATA_BLOCK ) + 1 ) * BLOCK;
	uint8_t *package = (uint8_t *)calloc( 1, total );
	uint8_t *table;
	uint32_t i;

	memcpy( package, "LIVE", 4 );
	PutBE32( package + 0x340, HEADER_SIZE );
	PutBE32( package + 0x344, 0x000D0000 );
	PutBE32( package + 0x360, 0x584108C8 );
	package[0x379] = 0x24;
	package[0x37B] = 1;			/* read-only: one hash table per level */
	PutLE16( package + 0x37C, 1 );	/* file table: one block ... */
	PutLE24( package + 0x37E, 0 );	/* ... at data block 0 */
	PutBE32( package + 0x395, MAX_DATA_BLOCK + 1 );

	*bpkSize = MakeBpk( bpk );

	table = DataBlock( package, 0 );
	FileEntry( table, 0, "default.xex", 0, 1, 1, 1, -1, 100 );
	FileEntry( table, 1, "baseq3", 1, 1, 0, 0, -1, 0 );
	FileEntry( table, 2, "pak0.bpk", 0, 0, kBpkBlocks[0], 3, 1, (uint32_t)*bpkSize );
	FileEntry( table, 3, "big.bin", 0, 1, BIG_START, BIG_BLOCKS, -1, BIG_SIZE );

	for ( i = 0; i < 100; i++ ) {
		DataBlock( package, 1 )[i] = (uint8_t)( 'A' + i % 26 );
	}
	for ( i = 0; i < 3; i++ ) {
		const size_t at = (size_t)i * BLOCK;
		if ( at < *bpkSize ) {
			memcpy( DataBlock( package, kBpkBlocks[i] ), bpk + at, *bpkSize - at < BLOCK ? *bpkSize - at : BLOCK );
		}
	}
	SetNext( package, kBpkBlocks[0], kBpkBlocks[1] );
	SetNext( package, kBpkBlocks[1], kBpkBlocks[2] );
	for ( i = 0; i < BIG_SIZE; i++ ) {
		DataBlock( package, BIG_START + i / BLOCK )[i % BLOCK] = BigByte( i );
	}
	*size = total;
	return package;
}

static void TestCrc32( void ) {
	CHECK( QAA_Crc32( 0, "123456789", 9 ) == 0xCBF43926u );
	CHECK( QAA_Crc32( QAA_Crc32( 0, "1234", 4 ), "56789", 5 ) == 0xCBF43926u );
}

static void TestStfsAndBpk( void ) {
	static uint8_t bpkBytes[3 * BLOCK];
	size_t packageSize, bpkSize;
	uint8_t *package = MakePackage( &packageSize, bpkBytes, &bpkSize );
	uint8_t *big = (uint8_t *)malloc( BIG_SIZE );
	char error[128];
	char text[64];
	qaaStfsFileInfo_t info;
	qaaIo_t io, file;
	qaaStfs_t *stfs;
	qaaBpk_t *bpk;
	uint8_t buffer[64];
	uint32_t i;
	int ok;

	QAA_IoMemory( package, packageSize, &io );
	stfs = QAA_StfsOpen( &io, error, sizeof( error ) );
	CHECK( stfs != NULL );
	if ( !stfs ) {
		fprintf( stderr, "QAA_StfsOpen: %s\n", error );
		QAA_IoClose( &io );
		free( package );
		free( big );
		return;
	}
	CHECK( QAA_StfsMagic( stfs ) == 0x4C495645u );
	CHECK( QAA_StfsTitleId( stfs ) == 0x584108C8u );
	CHECK( QAA_StfsContentType( stfs ) == 0x000D0000u );
	CHECK( QAA_StfsFileCount( stfs ) == 4 );
	CHECK( QAA_StfsFind( stfs, "default.xex" ) == 0 );
	CHECK( QAA_StfsFind( stfs, "BASEQ3\\PAK0.BPK" ) == 2 );
	CHECK( QAA_StfsFind( stfs, "/baseq3/pak0.bpk" ) == 2 );
	CHECK( QAA_StfsFind( stfs, "pak0.bpk" ) == -1 );
	CHECK( QAA_StfsFileInfo( stfs, 2, &info ) == 0 && !strcmp( info.path, "baseq3/pak0.bpk" ) && !info.contiguous );
	CHECK( QAA_StfsFileInfo( stfs, 1, &info ) == 0 && info.directory );
	CHECK( QAA_StfsOpenFile( stfs, 1, &file ) != 0 );	/* a directory has no data */

	/* a contiguous file crossing the 0xAA boundary, in one read and in pieces */
	CHECK( QAA_StfsOpenFile( stfs, 3, &file ) == 0 );
	CHECK( file.size == BIG_SIZE );
	CHECK( QAA_IoRead( &file, 0, big, BIG_SIZE ) == 0 );
	for ( ok = 1, i = 0; i < BIG_SIZE; i++ ) {
		ok &= big[i] == BigByte( i );
	}
	CHECK( ok );
	CHECK( QAA_IoRead( &file, ( 0xAA - BIG_START ) * (uint64_t)BLOCK - 7, buffer, 20 ) == 0 );
	for ( ok = 1, i = 0; i < 20; i++ ) {
		ok &= buffer[i] == BigByte( ( 0xAA - BIG_START ) * BLOCK - 7 + i );
	}
	CHECK( ok );
	CHECK( QAA_IoRead( &file, BIG_SIZE - 1, buffer, 2 ) != 0 );	/* past the end */
	QAA_IoClose( &file );

	/* the fragmented BPK, through the package, after the package is closed */
	CHECK( QAA_StfsOpenFile( stfs, 2, &file ) == 0 );
	QAA_StfsClose( stfs );
	CHECK( file.size == bpkSize );
	bpk = QAA_BpkOpen( &file, error, sizeof( error ) );
	CHECK( bpk != NULL );
	if ( !bpk ) {
		fprintf( stderr, "QAA_BpkOpen: %s\n", error );
		QAA_IoClose( &file );
	} else {
		CHECK( QAA_BpkVersion( bpk ) == 1 );
		CHECK( QAA_BpkCount( bpk ) == 3 );
		CHECK( QAA_BpkFind( bpk, HASH_RAW ) == 0 );
		CHECK( QAA_BpkFind( bpk, HASH_BAD ) == 1 );
		CHECK( QAA_BpkFind( bpk, HASH_PACKED ) == 2 );
		CHECK( QAA_BpkFind( bpk, 42 ) == -1 );
		CHECK( QAA_BpkEntry( bpk, 3 ) == NULL );
		CHECK( QAA_BpkTableCrc( bpk ) == QAA_Crc32( 0, bpkBytes, 8 + 3 * 24 ) );

		memset( text, 0, sizeof( text ) );
		CHECK( QAA_BpkRead( bpk, 0, text, strlen( kRaw ) ) == QAA_BPK_OK && !strcmp( text, kRaw ) );
		memset( text, 0, sizeof( text ) );
		CHECK( QAA_BpkRead( bpk, 2, text, strlen( kPacked ) ) == QAA_BPK_OK && !strcmp( text, kPacked ) );
		CHECK( QAA_BpkRead( bpk, 2, text, strlen( kPacked ) - 1 ) == QAA_BPK_ERR_ARG );
		CHECK( QAA_BpkRead( bpk, 1, text, 4 ) == QAA_BPK_ERR_FLAGS );
		CHECK( QAA_BpkRead( bpk, 3, text, 4 ) == QAA_BPK_ERR_ARG );
		QAA_BpkClose( bpk );
	}

	/* a record pointing past the end is rejected at open */
	PutBE32( bpkBytes + 8 + 8, (uint32_t)bpkSize );
	QAA_IoMemory( bpkBytes, bpkSize, &io );
	CHECK( QAA_BpkOpen( &io, error, sizeof( error ) ) == NULL );
	QAA_IoClose( &io );

	/* not a package */
	QAA_IoMemory( bpkBytes, bpkSize, &io );
	CHECK( QAA_StfsOpen( &io, error, sizeof( error ) ) == NULL );
	QAA_IoClose( &io );

	free( package );
	free( big );
}

static void TestNameLines( void ) {
	char path[64];
	uint64_t hash = 0;

	CHECK( QAA_BpkParseNameLine( "0123456789abcdef maps/arena.bsp\r\n", &hash, path, sizeof( path ) ) == 1 );
	CHECK( hash == 0x0123456789ABCDEFull && !strcmp( path, "maps/arena.bsp" ) );
	CHECK( QAA_BpkParseNameLine( "  FEDCBA9876543210\tscripts/a b.shader  ", &hash, path, sizeof( path ) ) == 1 );
	CHECK( hash == 0xFEDCBA9876543210ull && !strcmp( path, "scripts/a b.shader" ) );
	CHECK( QAA_BpkParseNameLine( "default.cfg", &hash, path, sizeof( path ) ) == 1 );
	CHECK( !strcmp( path, "default.cfg" ) && hash == QAA_BpkNameHash( "default.cfg" ) );
	CHECK( QAA_BpkParseNameLine( "0123 short.txt", &hash, path, sizeof( path ) ) == 1 && !strcmp( path, "0123 short.txt" ) );
	CHECK( QAA_BpkParseNameLine( "", &hash, path, sizeof( path ) ) == 0 );
	CHECK( QAA_BpkParseNameLine( "# comment", &hash, path, sizeof( path ) ) == 0 );
	CHECK( QAA_BpkParseNameLine( "// comment", &hash, path, sizeof( path ) ) == 0 );
	CHECK( QAA_BpkParseNameLine( "0123456789abcdef ", &hash, path, sizeof( path ) ) == 1
		&& !strcmp( path, "0123456789abcdef" ) );
	CHECK( QAA_BpkParseNameLine( "a/very/long/path/that/does/not/fit/in/the/small/buffer/given/here.txt",
		&hash, path, 16 ) == -1 );
	/* the placeholder hash matches no record */
	if ( !QAA_BpkNameHashKnown() ) {
		CHECK( QAA_BpkNameHash( "default.cfg" ) == 0 );
	}
}

int main( void ) {
	TestCrc32();
	TestStfsAndBpk();
	TestNameLines();
	if ( failures ) {
		fprintf( stderr, "%d check(s) failed\n", failures );
		return 1;
	}
	printf( "qaa package tests passed\n" );
	return 0;
}
