/*
===========================================================================
openQAA: Quake Arena Arcade .bpk archive reader.

Copyright (C) 2026 openQAA contributors.
This file is part of openQAA, licensed under the GNU GPL v2 or later.
===========================================================================
*/
#ifndef QAA_BPK_H
#define QAA_BPK_H

#include "qaa_io.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Record flags. All of pak0.bpk's records carry QAA_BPK_FLAG_XMEM (verified).
 * Flags 0 is read as stored data; that is an inference (tools/bpk.py reads it
 * the same way), not something pak0.bpk exercises. */
#define QAA_BPK_FLAG_XMEM	1u

typedef struct qaaBpkEntry_s {
	uint64_t hash;
	uint32_t offset;
	uint32_t size;			/* decoded size */
	uint32_t flags;
	uint32_t storedSize;
} qaaBpkEntry_t;

typedef struct qaaBpk_s qaaBpk_t;

enum {
	QAA_BPK_OK = 0,
	QAA_BPK_ERR_ARG = -1,
	QAA_BPK_ERR_IO = -2,
	QAA_BPK_ERR_MEMORY = -3,
	QAA_BPK_ERR_FLAGS = -4,
	QAA_BPK_ERR_DECODE = -5
};

/*
 * Open an archive. On success the archive owns `io`. On failure `io` is left
 * open, NULL is returned and `error`, when given, says why.
 */
qaaBpk_t *QAA_BpkOpen( qaaIo_t *io, char *error, size_t errorSize );
void QAA_BpkClose( qaaBpk_t *bpk );

uint32_t QAA_BpkVersion( const qaaBpk_t *bpk );
uint32_t QAA_BpkCount( const qaaBpk_t *bpk );

/* Records in file order; NULL past the end. */
const qaaBpkEntry_t *QAA_BpkEntry( const qaaBpk_t *bpk, uint32_t index );

/* Index of the first record with this name hash, or -1. */
int QAA_BpkFind( const qaaBpk_t *bpk, uint64_t hash );

/* Decode a record into `out`, which must hold exactly its decoded size. */
int QAA_BpkRead( const qaaBpk_t *bpk, uint32_t index, void *out, size_t outSize );

/* CRC-32 of the header and record table: identifies the archive. */
uint32_t QAA_BpkTableCrc( const qaaBpk_t *bpk );

const char *QAA_BpkError( int code );

/*
 * The 64-bit name hash that keys BPK records (qaa_bpk_hash.c). Every place
 * that turns a path into a BPK key calls this function and nothing else.
 * Returns 0 when the hash is not known, which matches no record.
 */
uint64_t QAA_BpkNameHash( const char *path );

/* Nonzero once QAA_BpkNameHash implements the retail algorithm. */
int QAA_BpkNameHashKnown( void );

/*
 * One line of a name list: "<16 hex digits> <path>" names the record with
 * that hash explicitly; "<path>" alone is hashed with QAA_BpkNameHash. Blank
 * lines and lines starting with '#' or "//" are skipped (returns 0). Returns
 * 1 with `hash` and `path` filled, or -1 for a malformed line.
 */
int QAA_BpkParseNameLine( const char *line, uint64_t *hash, char *path, size_t pathSize );

#ifdef __cplusplus
}
#endif

#endif
