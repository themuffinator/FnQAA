/*
===========================================================================
FnQAA: Xbox LZX (XMemCompress) decoder.

Copyright (C) 2026 FnQAA contributors.
This file is part of FnQAA, licensed under the GNU GPL v2 or later.
===========================================================================
*/
#ifndef QAA_LZX_H
#define QAA_LZX_H

#include <stddef.h>
#include <stdint.h>

#if defined( _WIN32 ) && defined( QAA_LZX_SHARED )
#define QAA_LZX_API __declspec( dllexport )
#else
#define QAA_LZX_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Decode a native XMemCompress stream: the 0x0FF512EE file header followed by
 * big-endian length-prefixed blocks, each holding one or more LZX frames.
 * Writes exactly `outSize` bytes. Returns 0 on success, a negative error code
 * otherwise (qaa_lzx_error describes it).
 */
QAA_LZX_API int qaa_lzx_decode_native( const uint8_t *in, size_t inSize, uint8_t *out, size_t outSize );

/* Parse the native header. Returns 0 and fills the sizes on success. */
QAA_LZX_API int qaa_lzx_native_info( const uint8_t *in, size_t inSize, uint64_t *uncompressed,
	uint64_t *compressed, uint32_t *windowSize );

/*
 * Decode a sequence of size-prefixed LZX frames sharing one stream: each
 * prefixed with a big-endian 16-bit compressed size, or with 0xFF, a 16-bit
 * uncompressed size and a 16-bit compressed size.
 */
QAA_LZX_API int qaa_lzx_decode_frames( const uint8_t *in, size_t inSize, uint8_t *out, size_t outSize,
	unsigned windowBits );

/*
 * Decode one continuous LZX bitstream, as XEX2 "normal" compression stores it
 * once its blocks are joined: frames of 32 KiB, each ending on a 16-bit
 * boundary, with no size prefixes.
 */
QAA_LZX_API int qaa_lzx_decode_raw( const uint8_t *in, size_t inSize, uint8_t *out, size_t outSize,
	unsigned windowBits );

QAA_LZX_API const char *qaa_lzx_error( int code );

#ifdef __cplusplus
}
#endif

#endif
