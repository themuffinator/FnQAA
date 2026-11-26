/*
===========================================================================
FnQAA: random-access readers for the QAA package formats.

Copyright (C) 2026 FnQAA contributors.
This file is part of FnQAA, licensed under the GNU GPL v2 or later.
===========================================================================
*/
#ifndef QAA_IO_H
#define QAA_IO_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * A read-only byte source with random access: a host file, a memory block, or
 * a file inside an STFS package. Readers never write to their source.
 *
 * read() fills exactly `length` bytes from `offset` and returns 0, or returns
 * a nonzero value when the range is out of bounds or the read fails. A reader
 * is not thread-safe: use one from one thread at a time.
 */
typedef struct qaaIo_s {
	void *ctx;
	uint64_t size;
	int ( *read )( void *ctx, uint64_t offset, void *buffer, size_t length );
	void ( *close )( void *ctx );
} qaaIo_t;

/* Open a host file read-only (UTF-8 path). Returns 0 on success. */
int QAA_IoOpenFile( const char *path, qaaIo_t *io );

/* Wrap caller-owned memory, which must outlive the reader. */
void QAA_IoMemory( const void *data, size_t size, qaaIo_t *io );

int QAA_IoRead( const qaaIo_t *io, uint64_t offset, void *buffer, size_t length );

/* Release the reader; safe on a zeroed or already closed reader. */
void QAA_IoClose( qaaIo_t *io );

static inline uint16_t QAA_ReadBE16( const uint8_t *p ) {
	return (uint16_t)( ( p[0] << 8 ) | p[1] );
}

static inline uint32_t QAA_ReadBE24( const uint8_t *p ) {
	return ( (uint32_t)p[0] << 16 ) | ( (uint32_t)p[1] << 8 ) | p[2];
}

static inline uint32_t QAA_ReadLE24( const uint8_t *p ) {
	return ( (uint32_t)p[2] << 16 ) | ( (uint32_t)p[1] << 8 ) | p[0];
}

static inline uint32_t QAA_ReadBE32( const uint8_t *p ) {
	return ( (uint32_t)p[0] << 24 ) | ( (uint32_t)p[1] << 16 ) | ( (uint32_t)p[2] << 8 ) | p[3];
}

static inline uint64_t QAA_ReadBE64( const uint8_t *p ) {
	return ( (uint64_t)QAA_ReadBE32( p ) << 32 ) | QAA_ReadBE32( p + 4 );
}

/* CRC-32 (IEEE, as zlib.crc32); pass 0 to start. */
uint32_t QAA_Crc32( uint32_t crc, const void *data, size_t length );

#ifdef __cplusplus
}
#endif

#endif
