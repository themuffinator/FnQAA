/*
===========================================================================
openQAA: random-access readers for the QAA package formats.

Copyright (C) 2026 openQAA contributors.
This file is part of openQAA, licensed under the GNU GPL v2 or later.
===========================================================================
*/
#include "qaa_io.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

typedef struct {
	FILE *file;
} qaaFileCtx_t;

static int QAA_FileSeek( FILE *file, uint64_t offset ) {
#ifdef _WIN32
	return _fseeki64( file, (__int64)offset, SEEK_SET );
#else
	return fseeko( file, (off_t)offset, SEEK_SET );
#endif
}

static int QAA_FileRead( void *ctx, uint64_t offset, void *buffer, size_t length ) {
	qaaFileCtx_t *f = (qaaFileCtx_t *)ctx;

	if ( !f || !f->file ) {
		return -1;
	}
	if ( length == 0 ) {
		return 0;
	}
	if ( QAA_FileSeek( f->file, offset ) != 0 ) {
		return -1;
	}
	return fread( buffer, 1, length, f->file ) == length ? 0 : -1;
}

static void QAA_FileClose( void *ctx ) {
	qaaFileCtx_t *f = (qaaFileCtx_t *)ctx;

	if ( f ) {
		if ( f->file ) {
			fclose( f->file );
		}
		free( f );
	}
}

static FILE *QAA_OpenReadOnly( const char *path ) {
#ifdef _WIN32
	wchar_t wide[1024];

	if ( MultiByteToWideChar( CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, wide, (int)( sizeof( wide ) / sizeof( wide[0] ) ) ) > 0 ) {
		return _wfopen( wide, L"rb" );
	}
	return fopen( path, "rb" );
#else
	return fopen( path, "rb" );
#endif
}

int QAA_IoOpenFile( const char *path, qaaIo_t *io ) {
	qaaFileCtx_t *f;
	int64_t size;

	memset( io, 0, sizeof( *io ) );
	if ( !path || !path[0] ) {
		return -1;
	}
	f = (qaaFileCtx_t *)calloc( 1, sizeof( *f ) );
	if ( !f ) {
		return -1;
	}
	f->file = QAA_OpenReadOnly( path );
	if ( !f->file ) {
		free( f );
		return -1;
	}
	/* only regular files: glibc's fopen also opens directories */
	{
#ifdef _WIN32
		struct _stat64 st;
		if ( _fstat64( _fileno( f->file ), &st ) != 0 || !( st.st_mode & _S_IFREG ) ) {
#else
		struct stat st;
		if ( fstat( fileno( f->file ), &st ) != 0 || !S_ISREG( st.st_mode ) ) {
#endif
			QAA_FileClose( f );
			return -1;
		}
	}
#ifdef _WIN32
	if ( _fseeki64( f->file, 0, SEEK_END ) != 0 ) {
		size = -1;
	} else {
		size = _ftelli64( f->file );
	}
#else
	if ( fseeko( f->file, 0, SEEK_END ) != 0 ) {
		size = -1;
	} else {
		size = (int64_t)ftello( f->file );
	}
#endif
	if ( size < 0 ) {
		QAA_FileClose( f );
		return -1;
	}
	io->ctx = f;
	io->size = (uint64_t)size;
	io->read = QAA_FileRead;
	io->close = QAA_FileClose;
	return 0;
}

typedef struct {
	const uint8_t *data;
	size_t size;
} qaaMemoryCtx_t;

static int QAA_MemoryRead( void *ctx, uint64_t offset, void *buffer, size_t length ) {
	const qaaMemoryCtx_t *m = (const qaaMemoryCtx_t *)ctx;

	if ( offset > m->size || length > m->size - offset ) {
		return -1;
	}
	if ( length ) {
		memcpy( buffer, m->data + offset, length );
	}
	return 0;
}

void QAA_IoMemory( const void *data, size_t size, qaaIo_t *io ) {
	qaaMemoryCtx_t *m = (qaaMemoryCtx_t *)calloc( 1, sizeof( *m ) );

	memset( io, 0, sizeof( *io ) );
	if ( !m ) {
		return;
	}
	m->data = (const uint8_t *)data;
	m->size = size;
	io->ctx = m;
	io->size = size;
	io->read = QAA_MemoryRead;
	io->close = free;
}

int QAA_IoRead( const qaaIo_t *io, uint64_t offset, void *buffer, size_t length ) {
	if ( !io || !io->read ) {
		return -1;
	}
	if ( offset > io->size || length > io->size - offset ) {
		return -1;
	}
	return io->read( io->ctx, offset, buffer, length );
}

void QAA_IoClose( qaaIo_t *io ) {
	if ( io && io->close ) {
		io->close( io->ctx );
	}
	if ( io ) {
		memset( io, 0, sizeof( *io ) );
	}
}

uint32_t QAA_Crc32( uint32_t crc, const void *data, size_t length ) {
	static uint32_t table[256];
	static int tableReady;
	const uint8_t *p = (const uint8_t *)data;
	size_t i;

	if ( !tableReady ) {
		uint32_t n, k, c;
		for ( n = 0; n < 256; n++ ) {
			c = n;
			for ( k = 0; k < 8; k++ ) {
				c = ( c & 1 ) ? 0xEDB88320u ^ ( c >> 1 ) : c >> 1;
			}
			table[n] = c;
		}
		tableReady = 1;
	}
	crc = ~crc;
	for ( i = 0; i < length; i++ ) {
		crc = table[( crc ^ p[i] ) & 0xFF] ^ ( crc >> 8 );
	}
	return ~crc;
}
