/*
===========================================================================
FnQAA: Quake Arena Arcade .bpk archive reader (names: qaa_bpk.h).

Copyright (C) 2026 FnQAA contributors.
This file is part of FnQAA, licensed under the GNU GPL v2 or later.
===========================================================================
*/
#ifndef QAA_BPK_ARCHIVE_H
#define QAA_BPK_ARCHIVE_H

#include "qaa_bpk.h"
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

/* Index of the first record with this name hash (qaa_bpk_name_hash), or -1.
 * Like retail's linear scan, the first of equal hashes wins. */
int QAA_BpkFind( const qaaBpk_t *bpk, uint64_t hash );

/* Decode a record into `out`, which must hold exactly its decoded size. */
int QAA_BpkRead( const qaaBpk_t *bpk, uint32_t index, void *out, size_t outSize );

/* CRC-32 of the header and record table: identifies the archive. */
uint32_t QAA_BpkTableCrc( const qaaBpk_t *bpk );

const char *QAA_BpkError( int code );

#ifdef __cplusplus
}
#endif

#endif
