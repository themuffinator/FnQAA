/*
===========================================================================
FnQAA: XMA2 audio decoder.

Copyright (C) 2026 FnQAA contributors.
This file is part of FnQAA, licensed under the GNU GPL v2 or later.

The decoder in qaa_xma2.c is ported from FFmpeg's WMA Pro / XMA decoder
(LGPL 2.1 or later); see that file for its notices.
===========================================================================
*/
#ifndef QAA_XMA2_H
#define QAA_XMA2_H

#include <stddef.h>
#include <stdint.h>

#if defined( _WIN32 ) && defined( QAA_XMA2_SHARED )
#define QAA_XMA2_API __declspec( dllexport )
#else
#define QAA_XMA2_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* An XMA2 stream is cut into packets of this many bytes. */
#define QAA_XMA2_PACKET_SIZE 2048

/* What a RIFF WAVE file with an XMA2WAVEFORMATEX (tag 0x166) describes. */
typedef struct qaa_xma2_info {
	int channels;				/* 1 or 2: one XMA2 stream */
	int rate;					/* Hz */
	uint32_t dataOffset;		/* byte offset of the data chunk's payload */
	uint32_t dataSize;			/* bytes in the data chunk, whole packets */
	uint32_t samplesEncoded;	/* XMA2WAVEFORMATEX fields, per channel */
	uint32_t playBegin;
	uint32_t playLength;
	uint32_t loopBegin;
	uint32_t loopLength;
	int loopCount;
} qaa_xma2_info;

/*
 * Parse a RIFF WAVE header holding one XMA2 stream. `size` may cover only
 * the start of the file, as long as it reaches the data chunk's header.
 * Returns 0 on success, a negative code otherwise (qaa_xma2_error).
 */
QAA_XMA2_API int qaa_xma2_parse( const void *file, size_t size, qaa_xma2_info *info );

/*
 * The number of sample frames qaa_xma2_decode_file produces at most: the
 * play region when the header gives one, otherwise the encoded length.
 */
QAA_XMA2_API uint32_t qaa_xma2_output_frames( const qaa_xma2_info *info );

typedef struct qaa_xma2_decoder qaa_xma2_decoder;

QAA_XMA2_API qaa_xma2_decoder *qaa_xma2_create( int channels, int rate );
QAA_XMA2_API void qaa_xma2_destroy( qaa_xma2_decoder *d );

/* Return to the state before the first packet (to restart a stream). */
QAA_XMA2_API void qaa_xma2_reset( qaa_xma2_decoder *d );

/*
 * Decode one packet of QAA_XMA2_PACKET_SIZE bytes, or end the stream with
 * NULL. Damaged frames are dropped as FFmpeg drops them; a negative return
 * reports that, and decoding can go on with the next packet.
 */
QAA_XMA2_API int qaa_xma2_feed( qaa_xma2_decoder *d, const uint8_t *packet );

/*
 * Take up to `maxFrames` decoded sample frames as interleaved 16-bit PCM.
 * Until the stream has ended, the last few frames are held back, since the
 * stream's end trim is known only then. Returns the frames written.
 */
QAA_XMA2_API int qaa_xma2_read( qaa_xma2_decoder *d, int16_t *out, int maxFrames );

/* Decode a whole file. Returns the frames written to `out`, or < 0. */
QAA_XMA2_API int qaa_xma2_decode_file( const void *file, size_t size, int16_t *out, int maxFrames );

QAA_XMA2_API const char *qaa_xma2_error( int code );

#ifdef __cplusplus
}
#endif

#endif /* QAA_XMA2_H */
