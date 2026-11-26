/*
===========================================================================
openQAA: Xbox 360 STFS (CON/LIVE/PIRS) package reader.

Copyright (C) 2026 openQAA contributors.
This file is part of openQAA, licensed under the GNU GPL v2 or later.
===========================================================================
*/
#ifndef QAA_STFS_H
#define QAA_STFS_H

#include "qaa_io.h"

#ifdef __cplusplus
extern "C" {
#endif

#define QAA_STFS_BLOCK_SIZE 0x1000
#define QAA_STFS_MAX_PATH 256

typedef struct qaaStfs_s qaaStfs_t;

typedef struct qaaStfsFileInfo_s {
	char path[QAA_STFS_MAX_PATH];	/* '/'-separated, as stored */
	int directory;
	int contiguous;
	uint32_t startBlock;
	uint32_t validBlocks;
	uint32_t size;
} qaaStfsFileInfo_t;

/*
 * Open a package. On success the package owns `io` (it is closed with the
 * package). On failure `io` is left open, NULL is returned and `error`, when
 * given, says why.
 */
qaaStfs_t *QAA_StfsOpen( qaaIo_t *io, char *error, size_t errorSize );

/* Drop the caller's reference. Files opened with QAA_StfsOpenFile keep the
 * package alive until they are closed too. */
void QAA_StfsClose( qaaStfs_t *stfs );

uint32_t QAA_StfsMagic( const qaaStfs_t *stfs );		/* 'LIVE', 'CON ' or 'PIRS' */
uint32_t QAA_StfsTitleId( const qaaStfs_t *stfs );
uint32_t QAA_StfsContentType( const qaaStfs_t *stfs );

int QAA_StfsFileCount( const qaaStfs_t *stfs );
int QAA_StfsFileInfo( const qaaStfs_t *stfs, int index, qaaStfsFileInfo_t *info );

/* Index of the entry with this path ('/' or '\\', case-insensitive), or -1. */
int QAA_StfsFind( const qaaStfs_t *stfs, const char *path );

/* A reader over one file's data. Returns 0 on success. */
int QAA_StfsOpenFile( qaaStfs_t *stfs, int index, qaaIo_t *io );

#ifdef __cplusplus
}
#endif

#endif
