/*
===========================================================================
openQAA: XMA2 sound codec.

Copyright (C) 2026 openQAA contributors.
This file is part of openQAA, licensed under the GNU GPL v2 or later.

QAA: every sound and music track in Quake Arena Arcade's pak0.bpk is XMA2
in a RIFF WAVE file named *.xma (docs/FORMATS.md). The Xbox decodes it in
hardware through XAudio2; the PC decodes it here with code/qaa/qaa_xma2.c
into the 16-bit PCM the rest of the sound system takes. PC deviation: the
decoded length is the file's play region (PlayBegin, PlayLength), and
samples a lossy decode overshoots past full scale are clipped to 16 bits,
where XAudio2 mixes in float.
===========================================================================
*/

extern "C" {
#include "../../client.h"
#include "snd_codec.h"
#include "../../../qaa/qaa_xma2.h"
}

#include "../../client_cpp.h"

#include <algorithm>
#include <cstring>
#include <new>
#include <vector>

using fnql::AllocateZoneMemory;
using fnql::FileRead;
using fnql::OpenFileRead;
using fnql::ScopedFileHandle;
using fnql::ScopedTempMemory;
using fnql::ScopedZoneMemory;

namespace {

// Compressed sounds are small (the longest track is under 3 MiB), so a file
// is read whole and decoded from memory.
bool ReadWholeFile( const char *filename, std::vector<uint8_t> &data ) {
	ScopedFileHandle file;
	const int length = OpenFileRead( filename, file, qtrue );
	if ( !file || length <= 0 ) {
		return false;
	}
	data.resize( static_cast<size_t>( length ) );
	return FileRead( file.get(), data.data(), data.size() ) == length;
}

struct XmaStreamState {
	std::vector<uint8_t> data;
	qaa_xma2_info info;
	qaa_xma2_decoder *decoder;
	uint32_t nextPacket;	// byte offset of the next packet to feed
	uint32_t skip;			// frames before the play region still to drop
	uint32_t left;			// frames of the play region still to return
	bool fedEnd;
};

void FillInfo( const qaa_xma2_info &xma, int frames, snd_info_t *info ) {
	info->rate = xma.rate;
	info->width = 2;
	info->channels = xma.channels;
	info->samples = frames;
	info->size = frames * xma.channels * 2;
	info->dataofs = 0;
}

} // namespace

snd_codec_t xma_codec = {
	"xma",
	S_XMA_CodecLoad,
	S_XMA_CodecOpenStream,
	S_XMA_CodecReadStream,
	S_XMA_CodecCloseStream,
	nullptr
};

/*
=================
S_XMA_CodecLoad
=================
*/
extern "C" void *S_XMA_CodecLoad( const char *filename, snd_info_t *info ) {
	std::vector<uint8_t> data;
	if ( !ReadWholeFile( filename, data ) ) {
		return nullptr;
	}

	qaa_xma2_info xma;
	const int parsed = qaa_xma2_parse( data.data(), data.size(), &xma );
	if ( parsed < 0 ) {
		Com_Printf( S_COLOR_RED "ERROR: \"%s\": %s\n", filename, qaa_xma2_error( parsed ) );
		return nullptr;
	}

	const uint32_t frames = qaa_xma2_output_frames( &xma );
	if ( frames == 0 || frames > 0x7FFFFFFF / 4u ) {
		Com_Printf( S_COLOR_RED "ERROR: \"%s\": no samples\n", filename );
		return nullptr;
	}

	FillInfo( xma, static_cast<int>( frames ), info );
	auto bufferStorage = ScopedTempMemory::Allocate( static_cast<size_t>( info->size ) );
	auto *pcm = bufferStorage.as<int16_t>();
	if ( !pcm ) {
		Com_Printf( S_COLOR_RED "ERROR: Out of memory reading \"%s\"\n", filename );
		return nullptr;
	}

	const int decoded = qaa_xma2_decode_file( data.data(), data.size(), pcm, static_cast<int>( frames ) );
	if ( decoded < 0 ) {
		Com_Printf( S_COLOR_RED "ERROR: \"%s\": %s\n", filename, qaa_xma2_error( decoded ) );
		return nullptr;
	}
	if ( static_cast<uint32_t>( decoded ) < frames ) {
		// a short stream: the rest of the buffer is silence
		std::memset( pcm + static_cast<size_t>( decoded ) * xma.channels, 0,
			static_cast<size_t>( frames - static_cast<uint32_t>( decoded ) ) * xma.channels * 2 );
	}
	return bufferStorage.release();
}

/*
=================
S_XMA_CodecOpenStream
=================
*/
extern "C" snd_stream_t *S_XMA_CodecOpenStream( const char *filename ) {
	std::vector<uint8_t> data;
	if ( !ReadWholeFile( filename, data ) ) {
		return nullptr;
	}
	qaa_xma2_info xma;
	const int parsed = qaa_xma2_parse( data.data(), data.size(), &xma );
	if ( parsed < 0 ) {
		Com_Printf( S_COLOR_RED "ERROR: \"%s\": %s\n", filename, qaa_xma2_error( parsed ) );
		return nullptr;
	}

	snd_stream_t *stream = S_CodecUtilOpen( filename, &xma_codec );
	if ( !stream ) {
		return nullptr;
	}

	auto *state = new ( std::nothrow ) XmaStreamState();
	if ( !state ) {
		S_CodecUtilClose( &stream );
		return nullptr;
	}
	state->decoder = qaa_xma2_create( xma.channels, xma.rate );
	if ( !state->decoder ) {
		delete state;
		S_CodecUtilClose( &stream );
		return nullptr;
	}
	state->data = std::move( data );
	state->info = xma;
	state->nextPacket = xma.dataOffset;
	state->skip = xma.playBegin;
	state->left = qaa_xma2_output_frames( &xma );
	state->fedEnd = false;

	FillInfo( xma, static_cast<int>( state->left ), &stream->info );
	stream->ptr = state;
	return stream;
}

/*
=================
S_XMA_CodecCloseStream
=================
*/
extern "C" void S_XMA_CodecCloseStream( snd_stream_t *stream ) {
	auto *state = static_cast<XmaStreamState *>( stream->ptr );
	if ( state ) {
		qaa_xma2_destroy( state->decoder );
		delete state;
		stream->ptr = nullptr;
	}
	S_CodecUtilClose( &stream );
}

/*
=================
S_XMA_CodecReadStream
=================
*/
extern "C" int S_XMA_CodecReadStream( snd_stream_t *stream, int bytes, void *buffer ) {
	auto *state = static_cast<XmaStreamState *>( stream->ptr );
	if ( !state || bytes <= 0 ) {
		return 0;
	}
	const int channels = state->info.channels;
	const int frameBytes = channels * 2;
	auto *out = static_cast<int16_t *>( buffer );
	int wanted = bytes / frameBytes;
	int written = 0;
	const size_t end = std::min( state->data.size(),
		static_cast<size_t>( state->info.dataOffset ) + state->info.dataSize );

	while ( written < wanted && state->left > 0 ) {
		int16_t scratch[1024 * 2];
		int got;
		if ( state->skip > 0 ) {
			got = qaa_xma2_read( state->decoder, scratch, static_cast<int>( std::min<uint32_t>( state->skip, 1024 ) ) );
			state->skip -= static_cast<uint32_t>( got );
		} else {
			const int take = std::min( wanted - written, static_cast<int>( state->left ) );
			got = qaa_xma2_read( state->decoder, out + static_cast<size_t>( written ) * channels, take );
			written += got;
			state->left -= static_cast<uint32_t>( got );
		}
		if ( got > 0 ) {
			continue;
		}
		if ( state->fedEnd ) {
			break;
		}
		if ( static_cast<size_t>( state->nextPacket ) + QAA_XMA2_PACKET_SIZE <= end ) {
			qaa_xma2_feed( state->decoder, state->data.data() + state->nextPacket );
			state->nextPacket += QAA_XMA2_PACKET_SIZE;
		} else {
			qaa_xma2_feed( state->decoder, nullptr );
			state->fedEnd = true;
		}
	}

	const int decodedBytes = written * frameBytes;
	stream->pos += decodedBytes;
	return decodedBytes;
}
