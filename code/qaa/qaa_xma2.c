/*
 * openQAA: XMA2 audio decoder.
 *
 * Ported from FFmpeg 7.1, libavcodec/wmaprodec.c and wma.c (the WMA Pro
 * bitstream that XMA2 carries, and FFmpeg's XMA2 packet handling), cut down
 * to the one-stream, one- or two-channel XMA2 that Quake Arena Arcade uses,
 * with FFmpeg's bit reader, VLC, MDCT and DSP helpers replaced by small
 * local ones:
 *
 * Wmapro compatible decoder
 * Copyright (c) 2007 Baptiste Coudurier, Benjamin Larsson, Ulion
 * Copyright (c) 2008 - 2011 Sascha Sommer, Benjamin Larsson
 *
 * WMA compatible codec
 * Copyright (c) 2002-2007 The FFmpeg Project
 *
 * This file is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or (at
 * your option) any later version. openQAA uses it under the GNU GPL v2 or
 * later, as section 3 of the LGPL 2.1 allows.
 *
 * This file is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU Lesser General Public
 * License for more details.
 *
 * openQAA changes, all PC-side: FFmpeg's XMA stream interleaving, its
 * WMA Pro (non-XMA) paths and channel counts above two are left out; the
 * output is 16-bit PCM; the length is cut to the file's play region.
 */
#include "qaa_xma2.h"
#include "qaa_xma2_tables.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

enum {
	QAA_XMA2_OK = 0,
	QAA_XMA2_E_NOT_RIFF = -1,
	QAA_XMA2_E_NO_FMT = -2,
	QAA_XMA2_E_NOT_XMA2 = -3,
	QAA_XMA2_E_UNSUPPORTED = -4,
	QAA_XMA2_E_NO_DATA = -5,
	QAA_XMA2_E_NOMEM = -6,
	QAA_XMA2_E_INVALID = -7,
};

/* XMA2 as FFmpeg configures it (decode_init with decode_flags 0x10d6). */
#define SAMPLES_PER_FRAME	512
#define LOG2_FRAME_SIZE		15		/* av_log2( block_align 2048 ) + 4 */
#define MAX_NUM_SUBFRAMES	4		/* 1 << ( ( 0x10d6 & 0x38 ) >> 3 ) */
#define SUBFRAME_LEN_BITS	2
#define MIN_SAMPLES_PER_SUBFRAME ( SAMPLES_PER_FRAME / MAX_NUM_SUBFRAMES )
#define NUM_BLOCK_SIZES		3		/* 512, 256, 128 */
#define BITS_PER_SAMPLE		16

#define MAX_CHANNELS		2
#define MAX_SUBFRAMES		32
#define MAX_BANDS			29
#define MAX_FRAMESIZE		32768
#define OUT_SIZE			( SAMPLES_PER_FRAME + SAMPLES_PER_FRAME / 2 )

/* Frames held back until the end, so the end trim can still be cut. */
#define HOLDBACK_FRAMES		2048

/* ---- bit reader (MSB first; bytes past the buffer read as zero) --------- */

typedef struct bitreader {
	const uint8_t *buf;
	int bytes;
	int index;
} bitreader;

static void br_init( bitreader *b, const uint8_t *buf, int bytes ) {
	b->buf = buf;
	b->bytes = bytes;
	b->index = 0;
}

static uint32_t br_show( const bitreader *b, int n ) {
	uint64_t v = 0;
	int byte, i;
	if ( n <= 0 ) {
		return 0;
	}
	byte = b->index >> 3;
	for ( i = 0; i < 5; i++ ) {
		const int p = byte + i;
		v <<= 8;
		if ( p >= 0 && p < b->bytes ) {
			v |= b->buf[p];
		}
	}
	v <<= 24 + ( b->index & 7 );
	return (uint32_t)( v >> ( 64 - n ) );
}

static uint32_t br_get( bitreader *b, int n ) {
	const uint32_t v = br_show( b, n );
	if ( n > 0 ) {
		b->index += n;
	}
	return v;
}

static int br_get1( bitreader *b ) {
	return (int)br_get( b, 1 );
}

static int br_sbits( bitreader *b, int n ) {
	const uint32_t v = br_get( b, n );
	const uint32_t sign = 1u << ( n - 1 );
	return (int)( ( v ^ sign ) - sign );
}

/* ---- VLC: codes assigned from lengths, as ff_vlc_init_from_lengths -------- */

#define VLC_FAST_BITS 8

typedef struct vlc_node {
	int16_t child[2];
	int16_t leaf;
	int32_t sym;
} vlc_node;

typedef struct vlc_fast {
	int32_t v;		/* symbol, or the node reached after VLC_FAST_BITS */
	uint8_t len;
	uint8_t kind;	/* 0 invalid, 1 symbol, 2 node */
} vlc_fast;

typedef struct vlc_table {
	vlc_node nodes[2 * 280];
	int numNodes;
	vlc_fast fast[1 << VLC_FAST_BITS];
} vlc_table;

static int vlc_insert( vlc_table *t, uint32_t code, int len, int sym ) {
	int n = 0, i;
	for ( i = len - 1; i >= 0; i-- ) {
		const int bit = ( code >> i ) & 1;
		if ( t->nodes[n].leaf ) {
			return -1;
		}
		if ( t->nodes[n].child[bit] < 0 ) {
			if ( t->numNodes >= (int)( sizeof( t->nodes ) / sizeof( t->nodes[0] ) ) ) {
				return -1;
			}
			t->nodes[t->numNodes].child[0] = t->nodes[t->numNodes].child[1] = -1;
			t->nodes[t->numNodes].leaf = 0;
			t->nodes[n].child[bit] = (int16_t)t->numNodes++;
		}
		n = t->nodes[n].child[bit];
	}
	t->nodes[n].leaf = 1;
	t->nodes[n].sym = sym;
	return 0;
}

/* lens/syms are read with a stride; symSize is 1 or 2 bytes, little-endian. */
static int vlc_build( vlc_table *t, int count, const uint8_t *lens, int lensWrap,
	const void *syms, int symsWrap, int symSize, int offset ) {
	uint64_t code = 0;
	int i, v;
	const uint8_t *s = (const uint8_t *)syms;
	t->numNodes = 1;
	t->nodes[0].child[0] = t->nodes[0].child[1] = -1;
	t->nodes[0].leaf = 0;
	for ( i = 0; i < count; i++, lens += lensWrap, s += symsWrap ) {
		int len = (int8_t)*lens;
		if ( len > 0 ) {
			int sym = s[0];
			if ( symSize == 2 ) {
				uint16_t u;
				memcpy( &u, s, 2 );	/* native order, as AV_RN16 */
				sym = u;
			}
			if ( vlc_insert( t, (uint32_t)( code >> ( 32 - len ) ), len, sym + offset ) < 0 ) {
				return -1;
			}
		} else if ( len < 0 ) {
			len = -len;
		} else {
			continue;
		}
		if ( len > 32 || ( code & ( ( 1ull << ( 32 - len ) ) - 1 ) ) ) {
			return -1;
		}
		code += 1ull << ( 32 - len );
		if ( code > 0x100000000ull ) {
			return -1;
		}
	}
	for ( v = 0; v < ( 1 << VLC_FAST_BITS ); v++ ) {
		int n = 0, depth;
		vlc_fast *f = &t->fast[v];
		f->kind = 0;
		f->len = VLC_FAST_BITS;
		f->v = -1;
		for ( depth = 0; depth < VLC_FAST_BITS; depth++ ) {
			const int bit = ( v >> ( VLC_FAST_BITS - 1 - depth ) ) & 1;
			n = t->nodes[n].child[bit];
			if ( n < 0 ) {
				break;
			}
			if ( t->nodes[n].leaf ) {
				f->kind = 1;
				f->len = (uint8_t)( depth + 1 );
				f->v = t->nodes[n].sym;
				break;
			}
		}
		if ( n >= 0 && !t->nodes[n].leaf && depth == VLC_FAST_BITS ) {
			f->kind = 2;
			f->v = n;
		}
	}
	return 0;
}

/* Returns the symbol, or -1 for a code not in the table (as get_vlc2). */
static int vlc_get( bitreader *b, const vlc_table *t ) {
	const vlc_fast *f = &t->fast[br_show( b, VLC_FAST_BITS )];
	int n;
	b->index += f->len;
	if ( f->kind == 1 ) {
		return f->v;
	}
	if ( f->kind == 0 ) {
		return -1;
	}
	n = f->v;
	for ( ;; ) {
		n = t->nodes[n].child[br_get1( b )];
		if ( n < 0 ) {
			return -1;
		}
		if ( t->nodes[n].leaf ) {
			return t->nodes[n].sym;
		}
	}
}

/* ---- half-length inverse MDCT (av_tx inverse MDCT, imdct_half) ----------- */

typedef struct mdct_ctx {
	int n;					/* coefficients in, samples out */
	float scale;
	float pre[256][2];		/* e^(-i pi k / n), k < n/2 */
	float post[256][2];		/* e^(-i pi (k + 1/4) / n) */
	float tw[128][2];		/* e^(-2 pi i k / (n/2)) */
	uint16_t rev[256];
} mdct_ctx;

static void mdct_init( mdct_ctx *m, int n, float scale ) {
	const int half = n / 2;
	int bits = 0, k;
	while ( ( 1 << bits ) < half ) {
		bits++;
	}
	m->n = n;
	m->scale = scale;
	for ( k = 0; k < half; k++ ) {
		int r = 0, b;
		const double a = -M_PI * k / n, p = -M_PI * ( k + 0.25 ) / n;
		m->pre[k][0] = (float)cos( a );
		m->pre[k][1] = (float)sin( a );
		m->post[k][0] = (float)cos( p );
		m->post[k][1] = (float)sin( p );
		for ( b = 0; b < bits; b++ ) {
			r |= ( ( k >> b ) & 1 ) << ( bits - 1 - b );
		}
		m->rev[k] = (uint16_t)r;
	}
	for ( k = 0; k < half / 2; k++ ) {
		const double a = -2.0 * M_PI * k / half;
		m->tw[k][0] = (float)cos( a );
		m->tw[k][1] = (float)sin( a );
	}
}

/*
 * out[m] = scale * y[n-1-m], y the DCT-IV of in: the middle half of
 * FFmpeg's inverse MDCT, -sum_k in[k] cos(pi/n (j + 1/2 + n/2)(k + 1/2)),
 * j = n/2 + m. The DCT-IV runs as an n/2-point complex FFT.
 */
static void imdct_half( const mdct_ctx *m, float *out, const float *in ) {
	float z[256][2];
	const int n = m->n, half = n / 2;
	int k, size;
	for ( k = 0; k < half; k++ ) {
		const float re = in[2 * k], im = in[n - 1 - 2 * k];
		const int r = m->rev[k];
		z[r][0] = re * m->pre[k][0] - im * m->pre[k][1];
		z[r][1] = re * m->pre[k][1] + im * m->pre[k][0];
	}
	for ( size = 2; size <= half; size <<= 1 ) {
		const int h = size >> 1, step = half / size;
		int i, j;
		for ( i = 0; i < half; i += size ) {
			for ( j = 0; j < h; j++ ) {
				const float wr = m->tw[j * step][0], wi = m->tw[j * step][1];
				float *a = z[i + j], *b = z[i + j + h];
				const float br = b[0] * wr - b[1] * wi, bi = b[0] * wi + b[1] * wr;
				b[0] = a[0] - br;
				b[1] = a[1] - bi;
				a[0] += br;
				a[1] += bi;
			}
		}
	}
	for ( k = 0; k < half; k++ ) {
		const float re = z[k][0] * m->post[k][0] - z[k][1] * m->post[k][1];
		const float im = z[k][0] * m->post[k][1] + z[k][1] * m->post[k][0];
		out[n - 1 - 2 * k] = re * m->scale;		/* y[2k] */
		out[2 * k] = -im * m->scale;			/* y[n-1-2k] */
	}
}

/* ---- decoder state (WMAProDecodeCtx, one XMA2 stream) -------------------- */

typedef struct channel_ctx {
	int16_t prev_block_len;
	uint8_t transmit_coefs;
	uint8_t num_subframes;
	uint16_t subframe_len[MAX_SUBFRAMES];
	uint16_t subframe_offset[MAX_SUBFRAMES];
	uint8_t cur_subframe;
	uint16_t decoded_samples;
	uint8_t grouped;
	int quant_step;
	int8_t reuse_sf;
	int8_t scale_factor_step;
	int max_scale_factor;
	int saved_scale_factors[2][MAX_BANDS];
	int8_t scale_factor_idx;
	int *scale_factors;
	uint8_t table_idx;
	float *coeffs;
	uint16_t num_vec_coeffs;
	float out[OUT_SIZE];
} channel_ctx;

typedef struct channel_grp {
	uint8_t num_channels;
	int8_t transform;
	int8_t transform_band[MAX_BANDS];
	float decorrelation_matrix[MAX_CHANNELS * MAX_CHANNELS];
	float *channel_data[MAX_CHANNELS];
} channel_grp;

struct qaa_xma2_decoder {
	int nb_channels;
	int rate;

	/* tables */
	vlc_table sf_vlc, sf_rl_vlc, coef_vlc[2], vec4_vlc, vec2_vlc, vec1_vlc;
	float sin64[33];
	float windows[4][512];		/* sine windows of 64, 128, 256, 512 */
	mdct_ctx mdct[NUM_BLOCK_SIZES];	/* index: av_log2( len ) - 7 */
	int8_t num_sfb[NUM_BLOCK_SIZES];
	int16_t sfb_offsets[NUM_BLOCK_SIZES][MAX_BANDS];
	int8_t sf_offsets[NUM_BLOCK_SIZES][NUM_BLOCK_SIZES][MAX_BANDS];

	/* bit reservoir */
	uint8_t frame_data[MAX_FRAMESIZE + 16];
	int pb_bits;

	/* packet state */
	bitreader pgb;
	const uint8_t *packet;
	int num_saved_bits;
	int frame_offset;
	uint8_t packet_loss;
	uint8_t packet_done;
	uint8_t eof_done;

	/* frame state */
	bitreader gb;
	int buf_bit_size;
	int8_t skip_frame;
	int8_t parsed_all_subframes;
	uint16_t trim_start, trim_end;

	/* subframe state */
	int16_t subframe_len;
	int8_t channels_for_cur_subframe;
	int8_t channel_indexes_for_cur_subframe[MAX_CHANNELS];
	int8_t num_bands;
	int8_t transmit_num_vec_coeffs;
	int16_t *cur_sfb_offsets;
	uint8_t table_idx;
	int8_t esc_len;
	uint8_t num_chgroups;
	channel_grp chgroup[MAX_CHANNELS];
	channel_ctx channel[MAX_CHANNELS];
	float tmp[SAMPLES_PER_FRAME];

	/* XMA wrapper: the stream's last trims, start skip, output queue */
	int xma_trim_start, xma_trim_end;
	int skip_out;
	int ended;
	int16_t *pcm;
	int pcm_frames;
	int pcm_read;
	int pcm_capacity;
};

static int av_log2( unsigned v ) {
	int n = 0;
	while ( v > 1 ) {
		v >>= 1;
		n++;
	}
	return n;
}

static int min_i( int a, int b ) {
	return a < b ? a : b;
}

/* av_tx's float MDCT scale for WMA Pro block size n. */
static float mdct_scale( int n ) {
	return (float)( 1.0 / ( n >> 1 ) / ( 1ll << ( BITS_PER_SAMPLE - 1 ) ) );
}

static int init_tables( qaa_xma2_decoder *s ) {
	int i, b, x;
	if ( vlc_build( &s->sf_vlc, HUFF_SCALE_SIZE, &scale_table[0][1], 2, &scale_table[0][0], 2, 1, -60 ) ||
		vlc_build( &s->sf_rl_vlc, HUFF_SCALE_RL_SIZE, &scale_rl_table[0][1], 2, &scale_rl_table[0][0], 2, 1, 0 ) ||
		vlc_build( &s->coef_vlc[0], HUFF_COEF0_SIZE, coef0_lens, 1, coef0_syms, 2, 2, 0 ) ||
		vlc_build( &s->coef_vlc[1], HUFF_COEF1_SIZE, &coef1_table[0][1], 2, &coef1_table[0][0], 2, 1, 0 ) ||
		vlc_build( &s->vec4_vlc, HUFF_VEC4_SIZE, vec4_lens, 1, vec4_syms, 2, 2, -1 ) ||
		vlc_build( &s->vec2_vlc, HUFF_VEC2_SIZE, &vec2_table[0][1], 2, &vec2_table[0][0], 2, 1, -1 ) ||
		vlc_build( &s->vec1_vlc, HUFF_VEC1_SIZE, &vec1_table[0][1], 2, &vec1_table[0][0], 2, 1, 0 ) ) {
		return -1;
	}
	for ( i = 0; i < 33; i++ ) {
		s->sin64[i] = (float)sin( i * M_PI / 64.0 );
	}
	for ( i = 0; i < 4; i++ ) {
		const int n = 64 << i;
		int k;
		for ( k = 0; k < n; k++ ) {
			s->windows[i][k] = (float)sin( ( k + 0.5 ) * ( M_PI / ( 2.0 * n ) ) );
		}
	}
	for ( i = 0; i < NUM_BLOCK_SIZES; i++ ) {
		const int n = SAMPLES_PER_FRAME >> ( NUM_BLOCK_SIZES - 1 - i );
		mdct_init( &s->mdct[i], n, mdct_scale( n ) );
	}

	/* scale factor bands (decode_init); XMA's rate is get_rate()'s */
	{
		int rate = s->rate > 44100 ? 48000 : s->rate > 32000 ? 44100 : s->rate > 24000 ? 32000 : 24000;
		for ( i = 0; i < NUM_BLOCK_SIZES; i++ ) {
			const int subframe_len = SAMPLES_PER_FRAME >> i;
			int band = 1;
			s->sfb_offsets[i][0] = 0;
			for ( x = 0; x < MAX_BANDS - 1 && s->sfb_offsets[i][band - 1] < subframe_len; x++ ) {
				int offset = ( subframe_len * 2 * critical_freq[x] ) / rate + 2;
				offset &= ~3;
				if ( offset > s->sfb_offsets[i][band - 1] ) {
					s->sfb_offsets[i][band++] = (int16_t)offset;
				}
				if ( offset >= subframe_len ) {
					break;
				}
			}
			s->sfb_offsets[i][band - 1] = (int16_t)subframe_len;
			s->num_sfb[i] = (int8_t)( band - 1 );
			if ( s->num_sfb[i] <= 0 ) {
				return -1;
			}
		}
	}
	for ( i = 0; i < NUM_BLOCK_SIZES; i++ ) {
		for ( b = 0; b < s->num_sfb[i]; b++ ) {
			const int offset = ( ( s->sfb_offsets[i][b] + s->sfb_offsets[i][b + 1] - 1 ) << i ) >> 1;
			for ( x = 0; x < NUM_BLOCK_SIZES; x++ ) {
				int v = 0;
				while ( v + 1 < MAX_BANDS && ( s->sfb_offsets[x][v + 1] << x ) < offset ) {
					v++;
				}
				s->sf_offsets[i][x][b] = (int8_t)v;
			}
		}
	}
	return 0;
}

static void init_state( qaa_xma2_decoder *s ) {
	int i;
	s->pb_bits = 0;
	s->num_saved_bits = 0;
	s->frame_offset = 0;
	s->packet_loss = 1;
	s->packet_done = 0;
	s->eof_done = 0;
	s->skip_frame = 1;
	s->trim_start = s->trim_end = 0;
	s->xma_trim_start = s->xma_trim_end = 0;
	s->skip_out = 64;
	s->ended = 0;
	s->pcm_frames = s->pcm_read = 0;
	memset( s->channel, 0, sizeof( s->channel ) );
	memset( s->chgroup, 0, sizeof( s->chgroup ) );
	for ( i = 0; i < s->nb_channels; i++ ) {
		s->channel[i].prev_block_len = SAMPLES_PER_FRAME;
	}
}

/* ---- WMA Pro bitstream (wmaprodec.c) -------------------------------------- */

static int decode_subframe_length( qaa_xma2_decoder *s, int offset ) {
	int frame_len_shift = 0, subframe_len;
	if ( offset == SAMPLES_PER_FRAME - MIN_SAMPLES_PER_SUBFRAME ) {
		return MIN_SAMPLES_PER_SUBFRAME;
	}
	if ( s->num_saved_bits - s->gb.index < 1 ) {
		return -1;
	}
	/* max_subframe_len_bit is set for 4 subframes */
	if ( br_get1( &s->gb ) ) {
		frame_len_shift = 1 + (int)br_get( &s->gb, SUBFRAME_LEN_BITS - 1 );
	}
	subframe_len = SAMPLES_PER_FRAME >> frame_len_shift;
	if ( subframe_len < MIN_SAMPLES_PER_SUBFRAME || subframe_len > SAMPLES_PER_FRAME ) {
		return -1;
	}
	return subframe_len;
}

static int decode_tilehdr( qaa_xma2_decoder *s ) {
	uint16_t num_samples[MAX_CHANNELS] = { 0 };
	uint8_t contains_subframe[MAX_CHANNELS];
	int channels_for_cur_subframe = s->nb_channels;
	int fixed_channel_layout = 0;
	int min_channel_len = 0;
	int c;

	for ( c = 0; c < s->nb_channels; c++ ) {
		s->channel[c].num_subframes = 0;
	}
	if ( br_get1( &s->gb ) ) {	/* max_num_subframes is 4, not 1 */
		fixed_channel_layout = 1;
	}
	do {
		int subframe_len;
		for ( c = 0; c < s->nb_channels; c++ ) {
			if ( num_samples[c] == min_channel_len ) {
				if ( fixed_channel_layout || channels_for_cur_subframe == 1 ||
					min_channel_len == SAMPLES_PER_FRAME - MIN_SAMPLES_PER_SUBFRAME ) {
					contains_subframe[c] = 1;
				} else {
					contains_subframe[c] = (uint8_t)br_get1( &s->gb );
				}
			} else {
				contains_subframe[c] = 0;
			}
		}
		if ( ( subframe_len = decode_subframe_length( s, min_channel_len ) ) <= 0 ) {
			return -1;
		}
		min_channel_len += subframe_len;
		for ( c = 0; c < s->nb_channels; c++ ) {
			channel_ctx *chan = &s->channel[c];
			if ( contains_subframe[c] ) {
				if ( chan->num_subframes >= MAX_SUBFRAMES ) {
					return -1;
				}
				chan->subframe_len[chan->num_subframes] = (uint16_t)subframe_len;
				num_samples[c] = (uint16_t)( num_samples[c] + subframe_len );
				++chan->num_subframes;
				if ( num_samples[c] > SAMPLES_PER_FRAME ) {
					return -1;
				}
			} else if ( num_samples[c] <= min_channel_len ) {
				if ( num_samples[c] < min_channel_len ) {
					channels_for_cur_subframe = 0;
					min_channel_len = num_samples[c];
				}
				++channels_for_cur_subframe;
			}
		}
	} while ( min_channel_len < SAMPLES_PER_FRAME );

	for ( c = 0; c < s->nb_channels; c++ ) {
		int i, offset = 0;
		for ( i = 0; i < s->channel[c].num_subframes; i++ ) {
			s->channel[c].subframe_offset[i] = (uint16_t)offset;
			offset += s->channel[c].subframe_len[i];
		}
	}
	return 0;
}

static int decode_channel_transform( qaa_xma2_decoder *s ) {
	int i;
	s->num_chgroups = 0;
	if ( s->nb_channels > 1 ) {
		int remaining_channels = s->channels_for_cur_subframe;
		if ( br_get1( &s->gb ) ) {
			return -1;	/* "Channel transform bit": unsupported, as FFmpeg */
		}
		for ( s->num_chgroups = 0; remaining_channels && s->num_chgroups < s->channels_for_cur_subframe;
			s->num_chgroups++ ) {
			channel_grp *chgroup = &s->chgroup[s->num_chgroups];
			float **channel_data = chgroup->channel_data;
			chgroup->num_channels = 0;
			chgroup->transform = 0;

			/* remaining_channels is at most 2 */
			chgroup->num_channels = (uint8_t)remaining_channels;
			for ( i = 0; i < s->channels_for_cur_subframe; i++ ) {
				const int channel_idx = s->channel_indexes_for_cur_subframe[i];
				if ( !s->channel[channel_idx].grouped ) {
					*channel_data++ = s->channel[channel_idx].coeffs;
				}
				s->channel[channel_idx].grouped = 1;
			}

			if ( chgroup->num_channels == 2 ) {
				if ( br_get1( &s->gb ) ) {
					if ( br_get1( &s->gb ) ) {
						return -1;	/* unknown channel transform type */
					}
				} else {
					chgroup->transform = 1;
					/* nb_channels == 2 */
					chgroup->decorrelation_matrix[0] = 1.0f;
					chgroup->decorrelation_matrix[1] = -1.0f;
					chgroup->decorrelation_matrix[2] = 1.0f;
					chgroup->decorrelation_matrix[3] = 1.0f;
				}
			}

			if ( chgroup->transform ) {
				if ( !br_get1( &s->gb ) ) {
					for ( i = 0; i < s->num_bands; i++ ) {
						chgroup->transform_band[i] = (int8_t)br_get1( &s->gb );
					}
				} else {
					memset( chgroup->transform_band, 1, (size_t)s->num_bands );
				}
			}
			remaining_channels -= chgroup->num_channels;
		}
	}
	return 0;
}

static unsigned get_large_val( bitreader *gb ) {
	int n_bits = 8;
	if ( br_get1( gb ) ) {
		n_bits += 8;
		if ( br_get1( gb ) ) {
			n_bits += 8;
			if ( br_get1( gb ) ) {
				n_bits += 7;
			}
		}
	}
	return br_get( gb, n_bits );
}

/* ff_wma_run_level_decode, version 1 (WMA Pro). */
static int run_level_decode( bitreader *gb, const vlc_table *vlc, const float *level_table,
	const uint16_t *run_table, float *ptr, int offset, int num_coefs, int block_len, int frame_len_bits ) {
	const uint32_t *ilvl = (const uint32_t *)(const void *)level_table;
	const unsigned coef_mask = (unsigned)block_len - 1;
	for ( ; offset < num_coefs; offset++ ) {
		const int code = vlc_get( gb, vlc );
		if ( code > 1 ) {
			uint32_t bitsv;
			const uint32_t sign = (uint32_t)br_get1( gb ) - 1;
			offset += run_table[code];
			bitsv = ilvl[code] ^ ( sign & 0x80000000u );
			memcpy( &ptr[offset & coef_mask], &bitsv, 4 );
		} else if ( code == 1 ) {
			break;
		} else {
			int sign;
			const unsigned level = get_large_val( gb );
			if ( br_get1( gb ) ) {
				if ( br_get1( gb ) ) {
					if ( br_get1( gb ) ) {
						return -1;	/* broken escape sequence */
					}
					offset += (int)br_get( gb, frame_len_bits ) + 4;
				} else {
					offset += (int)br_get( gb, 2 ) + 1;
				}
			}
			sign = br_get1( gb ) - 1;
			ptr[offset & coef_mask] = (float)(int)( ( level ^ (unsigned)sign ) - (unsigned)sign );
		}
	}
	if ( offset > num_coefs ) {
		return -1;
	}
	return 0;
}

static int decode_coeffs( qaa_xma2_decoder *s, int c ) {
	static const uint32_t fval_tab[16] = {
		0x00000000, 0x3f800000, 0x40000000, 0x40400000,
		0x40800000, 0x40a00000, 0x40c00000, 0x40e00000,
		0x41000000, 0x41100000, 0x41200000, 0x41300000,
		0x41400000, 0x41500000, 0x41600000, 0x41700000,
	};
	channel_ctx *ci = &s->channel[c];
	int rl_mode = 0, cur_coeff = 0, num_zeros = 0;
	const int vlctable = br_get1( &s->gb );
	const vlc_table *vlc = &s->coef_vlc[vlctable];
	const uint16_t *run = vlctable ? coef1_run : coef0_run;
	const float *level = vlctable ? coef1_level : coef0_level;

	while ( ( s->transmit_num_vec_coeffs || !rl_mode ) && ( cur_coeff + 3 < ci->num_vec_coeffs ) ) {
		uint32_t vals[4];
		int i, idx = vlc_get( &s->gb, &s->vec4_vlc );
		if ( idx < 0 ) {
			for ( i = 0; i < 4; i += 2 ) {
				idx = vlc_get( &s->gb, &s->vec2_vlc );
				if ( idx < 0 ) {
					uint32_t v0, v1;
					float f0, f1;
					v0 = (uint32_t)vlc_get( &s->gb, &s->vec1_vlc );
					if ( v0 == HUFF_VEC1_SIZE - 1 ) {
						v0 += get_large_val( &s->gb );
					}
					v1 = (uint32_t)vlc_get( &s->gb, &s->vec1_vlc );
					if ( v1 == HUFF_VEC1_SIZE - 1 ) {
						v1 += get_large_val( &s->gb );
					}
					f0 = (float)v0;
					f1 = (float)v1;
					memcpy( &vals[i], &f0, 4 );
					memcpy( &vals[i + 1], &f1, 4 );
				} else {
					vals[i] = fval_tab[idx >> 4];
					vals[i + 1] = fval_tab[idx & 0xF];
				}
			}
		} else {
			vals[0] = fval_tab[idx >> 12];
			vals[1] = fval_tab[( idx >> 8 ) & 0xF];
			vals[2] = fval_tab[( idx >> 4 ) & 0xF];
			vals[3] = fval_tab[idx & 0xF];
		}
		for ( i = 0; i < 4; i++ ) {
			if ( vals[i] ) {
				const uint32_t sign = (uint32_t)br_get1( &s->gb ) - 1;
				const uint32_t v = vals[i] ^ sign << 31;
				memcpy( &ci->coeffs[cur_coeff], &v, 4 );
				num_zeros = 0;
			} else {
				ci->coeffs[cur_coeff] = 0;
				rl_mode |= ( ++num_zeros > s->subframe_len >> 8 );
			}
			++cur_coeff;
		}
	}
	if ( cur_coeff < s->subframe_len ) {
		memset( &ci->coeffs[cur_coeff], 0, sizeof( *ci->coeffs ) * (size_t)( s->subframe_len - cur_coeff ) );
		if ( run_level_decode( &s->gb, vlc, level, run, ci->coeffs, cur_coeff, s->subframe_len,
				s->subframe_len, s->esc_len ) < 0 ) {
			return -1;
		}
	}
	return 0;
}

static int decode_scale_factors( qaa_xma2_decoder *s ) {
	int i;
	for ( i = 0; i < s->channels_for_cur_subframe; i++ ) {
		const int c = s->channel_indexes_for_cur_subframe[i];
		channel_ctx *ch = &s->channel[c];
		int *sf, *sf_end;
		ch->scale_factors = ch->saved_scale_factors[!ch->scale_factor_idx];
		sf_end = ch->scale_factors + s->num_bands;

		if ( ch->reuse_sf ) {
			const int8_t *sf_offsets = s->sf_offsets[s->table_idx][ch->table_idx];
			int b;
			for ( b = 0; b < s->num_bands; b++ ) {
				ch->scale_factors[b] = ch->saved_scale_factors[ch->scale_factor_idx][*sf_offsets++];
			}
		}

		if ( !ch->cur_subframe || br_get1( &s->gb ) ) {
			if ( !ch->reuse_sf ) {
				int val;
				ch->scale_factor_step = (int8_t)( br_get( &s->gb, 2 ) + 1 );
				val = 45 / ch->scale_factor_step;
				for ( sf = ch->scale_factors; sf < sf_end; sf++ ) {
					val += vlc_get( &s->gb, &s->sf_vlc );
					*sf = val;
				}
			} else {
				int k;
				for ( k = 0; k < s->num_bands; k++ ) {
					int skip, val, sign;
					const int idx = vlc_get( &s->gb, &s->sf_rl_vlc );
					if ( !idx ) {
						const uint32_t code = br_get( &s->gb, 14 );
						val = (int)( code >> 6 );
						sign = (int)( code & 1 ) - 1;
						skip = (int)( code & 0x3f ) >> 1;
					} else if ( idx == 1 ) {
						break;
					} else {
						if ( idx < 0 || idx >= HUFF_SCALE_RL_SIZE ) {
							return -1;
						}
						skip = scale_rl_run[idx];
						val = scale_rl_level[idx];
						sign = br_get1( &s->gb ) - 1;
					}
					k += skip;
					if ( k >= s->num_bands ) {
						return -1;
					}
					ch->scale_factors[k] += ( val ^ sign ) - sign;
				}
			}
			ch->scale_factor_idx = (int8_t)!ch->scale_factor_idx;
			ch->table_idx = s->table_idx;
			ch->reuse_sf = 1;
		}

		ch->max_scale_factor = ch->scale_factors[0];
		for ( sf = ch->scale_factors + 1; sf < sf_end; sf++ ) {
			if ( *sf > ch->max_scale_factor ) {
				ch->max_scale_factor = *sf;
			}
		}
	}
	return 0;
}

static void inverse_channel_transform( qaa_xma2_decoder *s ) {
	int i;
	for ( i = 0; i < s->num_chgroups; i++ ) {
		if ( s->chgroup[i].transform ) {
			float data[MAX_CHANNELS];
			const int num_channels = s->chgroup[i].num_channels;
			float **ch_data = s->chgroup[i].channel_data;
			float **ch_end = ch_data + num_channels;
			const int8_t *tb = s->chgroup[i].transform_band;
			int16_t *sfb;
			for ( sfb = s->cur_sfb_offsets; sfb < s->cur_sfb_offsets + s->num_bands; sfb++ ) {
				int y;
				if ( *tb++ == 1 ) {
					for ( y = sfb[0]; y < min_i( sfb[1], s->subframe_len ); y++ ) {
						const float *mat = s->chgroup[i].decorrelation_matrix;
						const float *data_end = data + num_channels;
						float *data_ptr = data;
						float **ch;
						for ( ch = ch_data; ch < ch_end; ch++ ) {
							*data_ptr++ = ( *ch )[y];
						}
						for ( ch = ch_data; ch < ch_end; ch++ ) {
							float sum = 0;
							data_ptr = data;
							while ( data_ptr < data_end ) {
								sum += *data_ptr++ * *mat++;
							}
							( *ch )[y] = sum;
						}
					}
				} else if ( s->nb_channels == 2 ) {
					const int len = min_i( sfb[1], s->subframe_len ) - sfb[0];
					int k;
					for ( k = 0; k < len; k++ ) {
						ch_data[0][sfb[0] + k] *= 181.0f / 128;
						ch_data[1][sfb[0] + k] *= 181.0f / 128;
					}
				}
			}
		}
	}
}

/* vector_fmul_window: overlap-add with a sine window of 2 * len. */
static void fmul_window( float *dst, const float *src0, const float *src1, const float *win, int len ) {
	int i, j;
	dst += len;
	win += len;
	src0 += len;
	for ( i = -len, j = len - 1; i < 0; i++, j-- ) {
		const float s0 = src0[i], s1 = src1[j], wi = win[i], wj = win[j];
		dst[i] = s0 * wj - s1 * wi;
		dst[j] = s0 * wi + s1 * wj;
	}
}

static void wmapro_window( qaa_xma2_decoder *s ) {
	int i;
	for ( i = 0; i < s->channels_for_cur_subframe; i++ ) {
		const int c = s->channel_indexes_for_cur_subframe[i];
		int winlen = s->channel[c].prev_block_len;
		float *start = s->channel[c].coeffs - ( winlen >> 1 );
		const float *window;
		if ( s->subframe_len < winlen ) {
			start += ( winlen - s->subframe_len ) >> 1;
			winlen = s->subframe_len;
		}
		window = s->windows[av_log2( (unsigned)winlen ) - 6];
		winlen >>= 1;
		fmul_window( start, start, start + winlen, window, winlen );
		s->channel[c].prev_block_len = s->subframe_len;
	}
}

static int decode_subframe( qaa_xma2_decoder *s ) {
	int offset = SAMPLES_PER_FRAME;
	int subframe_len = SAMPLES_PER_FRAME;
	int total_samples = SAMPLES_PER_FRAME * s->nb_channels;
	int transmit_coeffs = 0;
	int i;

	for ( i = 0; i < s->nb_channels; i++ ) {
		s->channel[i].grouped = 0;
		if ( offset > s->channel[i].decoded_samples ) {
			offset = s->channel[i].decoded_samples;
			subframe_len = s->channel[i].subframe_len[s->channel[i].cur_subframe];
		}
	}

	s->channels_for_cur_subframe = 0;
	for ( i = 0; i < s->nb_channels; i++ ) {
		const int cur_subframe = s->channel[i].cur_subframe;
		total_samples -= s->channel[i].decoded_samples;
		if ( offset == s->channel[i].decoded_samples && subframe_len == s->channel[i].subframe_len[cur_subframe] ) {
			total_samples -= s->channel[i].subframe_len[cur_subframe];
			s->channel[i].decoded_samples = (uint16_t)( s->channel[i].decoded_samples +
				s->channel[i].subframe_len[cur_subframe] );
			s->channel_indexes_for_cur_subframe[s->channels_for_cur_subframe] = (int8_t)i;
			++s->channels_for_cur_subframe;
		}
	}
	if ( !total_samples ) {
		s->parsed_all_subframes = 1;
	}
	if ( subframe_len < MIN_SAMPLES_PER_SUBFRAME || !s->channels_for_cur_subframe ) {
		return -1;
	}

	s->table_idx = (uint8_t)av_log2( (unsigned)( SAMPLES_PER_FRAME / subframe_len ) );
	s->num_bands = s->num_sfb[s->table_idx];
	s->cur_sfb_offsets = s->sfb_offsets[s->table_idx];

	offset += SAMPLES_PER_FRAME >> 1;
	for ( i = 0; i < s->channels_for_cur_subframe; i++ ) {
		const int c = s->channel_indexes_for_cur_subframe[i];
		s->channel[c].coeffs = &s->channel[c].out[offset];
	}

	s->subframe_len = (int16_t)subframe_len;
	s->esc_len = (int8_t)( av_log2( (unsigned)( s->subframe_len - 1 ) ) + 1 );

	/* skip extended header if any */
	if ( br_get1( &s->gb ) ) {
		int num_fill_bits;
		if ( !( num_fill_bits = (int)br_get( &s->gb, 2 ) ) ) {
			const int len = (int)br_get( &s->gb, 4 );
			num_fill_bits = (int)br_get( &s->gb, len ) + 1;
		}
		if ( num_fill_bits >= 0 ) {
			if ( s->gb.index + num_fill_bits > s->num_saved_bits ) {
				return -1;
			}
			s->gb.index += num_fill_bits;
		}
	}

	if ( br_get1( &s->gb ) ) {
		return -1;	/* reserved bit */
	}

	if ( decode_channel_transform( s ) < 0 ) {
		return -1;
	}

	for ( i = 0; i < s->channels_for_cur_subframe; i++ ) {
		const int c = s->channel_indexes_for_cur_subframe[i];
		if ( ( s->channel[c].transmit_coefs = (uint8_t)br_get1( &s->gb ) ) ) {
			transmit_coeffs = 1;
		}
	}

	if ( transmit_coeffs ) {
		int step;
		int quant_step = 90 * BITS_PER_SAMPLE >> 4;
		if ( ( s->transmit_num_vec_coeffs = (int8_t)br_get1( &s->gb ) ) ) {
			const int num_bits = av_log2( (unsigned)( ( s->subframe_len + 3 ) / 4 ) ) + 1;
			for ( i = 0; i < s->channels_for_cur_subframe; i++ ) {
				const int c = s->channel_indexes_for_cur_subframe[i];
				const int num_vec_coeffs = (int)br_get( &s->gb, num_bits ) << 2;
				if ( num_vec_coeffs > s->subframe_len || num_vec_coeffs + offset > OUT_SIZE ) {
					return -1;
				}
				s->channel[c].num_vec_coeffs = (uint16_t)num_vec_coeffs;
			}
		} else {
			for ( i = 0; i < s->channels_for_cur_subframe; i++ ) {
				const int c = s->channel_indexes_for_cur_subframe[i];
				s->channel[c].num_vec_coeffs = (uint16_t)s->subframe_len;
			}
		}
		step = br_sbits( &s->gb, 6 );
		quant_step += step;
		if ( step == -32 || step == 31 ) {
			const int sign = ( step == 31 ) - 1;
			int quant = 0;
			while ( s->gb.index + 5 < s->num_saved_bits && ( step = (int)br_get( &s->gb, 5 ) ) == 31 ) {
				quant += 31;
			}
			quant_step += ( ( quant + step ) ^ sign ) - sign;
		}

		if ( s->channels_for_cur_subframe == 1 ) {
			s->channel[s->channel_indexes_for_cur_subframe[0]].quant_step = quant_step;
		} else {
			const int modifier_len = (int)br_get( &s->gb, 3 );
			for ( i = 0; i < s->channels_for_cur_subframe; i++ ) {
				const int c = s->channel_indexes_for_cur_subframe[i];
				s->channel[c].quant_step = quant_step;
				if ( br_get1( &s->gb ) ) {
					if ( modifier_len ) {
						s->channel[c].quant_step += (int)br_get( &s->gb, modifier_len ) + 1;
					} else {
						++s->channel[c].quant_step;
					}
				}
			}
		}

		if ( decode_scale_factors( s ) < 0 ) {
			return -1;
		}
	}

	for ( i = 0; i < s->channels_for_cur_subframe; i++ ) {
		const int c = s->channel_indexes_for_cur_subframe[i];
		if ( s->channel[c].transmit_coefs && s->gb.index < s->num_saved_bits ) {
			decode_coeffs( s, c );	/* errors are ignored, as FFmpeg does */
		} else {
			memset( s->channel[c].coeffs, 0, sizeof( *s->channel[c].coeffs ) * (size_t)subframe_len );
		}
	}

	if ( transmit_coeffs ) {
		const mdct_ctx *tx = &s->mdct[av_log2( (unsigned)subframe_len ) - 7];
		inverse_channel_transform( s );
		for ( i = 0; i < s->channels_for_cur_subframe; i++ ) {
			const int c = s->channel_indexes_for_cur_subframe[i];
			const int *sf = s->channel[c].scale_factors;
			int b;
			for ( b = 0; b < s->num_bands; b++ ) {
				const int end = min_i( s->cur_sfb_offsets[b + 1], s->subframe_len );
				const int e = s->channel[c].quant_step -
					( s->channel[c].max_scale_factor - *sf++ ) * s->channel[c].scale_factor_step;
				const float quant = (float)pow( 10.0, e / 20.0 );
				int k;
				for ( k = s->cur_sfb_offsets[b]; k < end; k++ ) {
					s->tmp[k] = s->channel[c].coeffs[k] * quant;
				}
			}
			imdct_half( tx, s->channel[c].coeffs, s->tmp );
		}
	}

	wmapro_window( s );

	for ( i = 0; i < s->channels_for_cur_subframe; i++ ) {
		const int c = s->channel_indexes_for_cur_subframe[i];
		if ( s->channel[c].cur_subframe >= s->channel[c].num_subframes ) {
			return -1;
		}
		++s->channel[c].cur_subframe;
	}
	return 0;
}

/* Decode one frame into out[]; returns more_frames, sets *got. */
static int decode_frame( qaa_xma2_decoder *s, float out[MAX_CHANNELS][SAMPLES_PER_FRAME], int *got ) {
	bitreader *gb = &s->gb;
	int len, i;

	*got = 0;
	len = (int)br_get( gb, LOG2_FRAME_SIZE );	/* len_prefix */

	if ( decode_tilehdr( s ) ) {
		s->packet_loss = 1;
		return 0;
	}

	if ( s->nb_channels > 1 && br_get1( gb ) ) {
		if ( br_get1( gb ) ) {
			gb->index += 4 * s->nb_channels * s->nb_channels;
		}
	}

	br_get( gb, 8 );	/* drc_gain: dynamic_range_compression is set */

	if ( br_get1( gb ) ) {
		if ( br_get1( gb ) ) {
			s->trim_start = (uint16_t)br_get( gb, av_log2( SAMPLES_PER_FRAME * 2 ) );
		}
		if ( br_get1( gb ) ) {
			s->trim_end = (uint16_t)br_get( gb, av_log2( SAMPLES_PER_FRAME * 2 ) );
		}
	} else {
		s->trim_start = s->trim_end = 0;
	}

	s->parsed_all_subframes = 0;
	for ( i = 0; i < s->nb_channels; i++ ) {
		s->channel[i].decoded_samples = 0;
		s->channel[i].cur_subframe = 0;
		s->channel[i].reuse_sf = 0;
	}

	while ( !s->parsed_all_subframes ) {
		if ( decode_subframe( s ) < 0 ) {
			s->packet_loss = 1;
			return 0;
		}
	}

	for ( i = 0; i < s->nb_channels; i++ ) {
		memcpy( out[i], s->channel[i].out, SAMPLES_PER_FRAME * sizeof( float ) );
		memcpy( &s->channel[i].out[0], &s->channel[i].out[SAMPLES_PER_FRAME],
			SAMPLES_PER_FRAME * sizeof( float ) >> 1 );
	}

	if ( s->skip_frame ) {
		s->skip_frame = 0;
	} else {
		*got = 1;
	}

	/* FFmpeg keeps the frame's samples even when its length is off */
	if ( len != ( gb->index - s->frame_offset ) + 2 ) {
		s->packet_loss = 1;
		return 0;
	}
	gb->index += len - ( gb->index - s->frame_offset ) - 1;

	return br_get1( gb );
}

/* put_bits / ff_copy_bits into the reservoir */
static void put_bits_n( qaa_xma2_decoder *s, int n, uint32_t value ) {
	int i;
	for ( i = n - 1; i >= 0; i-- ) {
		const int pos = s->pb_bits++;
		uint8_t *p = &s->frame_data[pos >> 3];
		const int shift = 7 - ( pos & 7 );
		if ( ( value >> i ) & 1 ) {
			*p |= (uint8_t)( 1u << shift );
		} else {
			*p &= (uint8_t)~( 1u << shift );
		}
	}
}

static void copy_bits( qaa_xma2_decoder *s, bitreader *src, int n ) {
	/* whole bytes when the reservoir is byte-aligned */
	while ( n >= 8 && !( s->pb_bits & 7 ) ) {
		s->frame_data[s->pb_bits >> 3] = (uint8_t)br_get( src, 8 );
		s->pb_bits += 8;
		n -= 8;
	}
	while ( n > 0 ) {
		const int chunk = n > 24 ? 24 : n;
		put_bits_n( s, chunk, br_get( src, chunk ) );
		n -= chunk;
	}
}

static void save_bits( qaa_xma2_decoder *s, bitreader *gb, int len, int append ) {
	int buflen;
	if ( !append ) {
		s->frame_offset = gb->index & 7;
		s->num_saved_bits = s->frame_offset;
		s->pb_bits = 0;
		buflen = ( s->num_saved_bits + len + 7 ) >> 3;
	} else {
		buflen = ( s->pb_bits + len + 7 ) >> 3;
	}
	if ( len <= 0 || buflen > MAX_FRAMESIZE ) {
		s->packet_loss = 1;
		return;
	}
	s->num_saved_bits += len;
	if ( !append ) {
		bitreader from = *gb;
		from.index &= ~7;
		copy_bits( s, &from, s->num_saved_bits );
		gb->index += len;
	} else {
		copy_bits( s, gb, len );
	}
	br_init( &s->gb, s->frame_data, ( s->num_saved_bits + 7 ) >> 3 );
	s->gb.index = s->frame_offset;
}

static int remaining_bits( const qaa_xma2_decoder *s, const bitreader *gb ) {
	return s->buf_bit_size - gb->index;
}

/*
 * decode_packet for XMA2: one call per frame, as FFmpeg's decode loop feeds
 * the rest of a packet back. Returns < 0 on a damaged packet.
 */
static int decode_packet_step( qaa_xma2_decoder *s, int first, float out[MAX_CHANNELS][SAMPLES_PER_FRAME], int *got ) {
	bitreader *gb = &s->pgb;
	*got = 0;

	if ( first ) {
		int num_bits_prev_frame;
		s->packet_done = 0;
		s->buf_bit_size = QAA_XMA2_PACKET_SIZE << 3;
		br_init( gb, s->packet, QAA_XMA2_PACKET_SIZE );
		br_get( gb, 6 );	/* number of frames */
		num_bits_prev_frame = (int)br_get( gb, LOG2_FRAME_SIZE );
		br_get( gb, 3 );
		br_get( gb, 8 );	/* skip packets: one stream */

		if ( num_bits_prev_frame > 0 ) {
			const int remaining_packet_bits = s->buf_bit_size - gb->index;
			if ( num_bits_prev_frame >= remaining_packet_bits ) {
				num_bits_prev_frame = remaining_packet_bits;
				s->packet_done = 1;
			}
			save_bits( s, gb, num_bits_prev_frame, 1 );
			if ( !s->packet_loss ) {
				decode_frame( s, out, got );
			}
		}
		if ( s->packet_loss ) {
			s->num_saved_bits = 0;
			s->packet_loss = 0;
		}
	} else {
		int frame_size;
		if ( remaining_bits( s, gb ) > LOG2_FRAME_SIZE &&
			( frame_size = (int)br_show( gb, LOG2_FRAME_SIZE ) ) != 0 &&
			frame_size <= remaining_bits( s, gb ) ) {
			save_bits( s, gb, frame_size, 0 );
			if ( !s->packet_loss ) {
				s->packet_done = (uint8_t)!decode_frame( s, out, got );
			}
		} else {
			s->packet_done = 1;
		}
	}

	if ( remaining_bits( s, gb ) < 0 ) {
		s->packet_loss = 1;
	}
	if ( s->packet_done && !s->packet_loss && remaining_bits( s, gb ) > 0 ) {
		save_bits( s, gb, remaining_bits( s, gb ), 0 );
	}
	if ( s->packet_loss ) {
		return QAA_XMA2_E_INVALID;
	}
	return 0;
}

/* ---- output queue --------------------------------------------------------- */

static int pcm_reserve( qaa_xma2_decoder *s, int frames ) {
	if ( s->pcm_read > 0 && s->pcm_read == s->pcm_frames ) {
		s->pcm_read = s->pcm_frames = 0;
	}
	if ( s->pcm_frames + frames > s->pcm_capacity ) {
		int16_t *p;
		int cap;
		if ( s->pcm_read > 0 ) {
			memmove( s->pcm, s->pcm + (size_t)s->pcm_read * s->nb_channels,
				(size_t)( s->pcm_frames - s->pcm_read ) * s->nb_channels * sizeof( int16_t ) );
			s->pcm_frames -= s->pcm_read;
			s->pcm_read = 0;
		}
		if ( s->pcm_frames + frames <= s->pcm_capacity ) {
			return 0;
		}
		cap = s->pcm_capacity ? s->pcm_capacity : 8192;
		while ( cap < s->pcm_frames + frames ) {
			cap *= 2;
		}
		p = (int16_t *)realloc( s->pcm, (size_t)cap * s->nb_channels * sizeof( int16_t ) );
		if ( !p ) {
			return -1;
		}
		s->pcm = p;
		s->pcm_capacity = cap;
	}
	return 0;
}

static int16_t to_pcm16( float v ) {
	float f = v * 32768.0f;
	if ( f >= 32767.0f ) {
		return 32767;
	}
	if ( f <= -32768.0f ) {
		return -32768;
	}
	return (int16_t)lrintf( f );
}

static int push_frame( qaa_xma2_decoder *s, float in[MAX_CHANNELS][SAMPLES_PER_FRAME], int count ) {
	int start = 0, i, c;
	if ( s->skip_out > 0 ) {
		start = min_i( s->skip_out, count );
		s->skip_out -= start;
	}
	if ( start >= count ) {
		return 0;
	}
	if ( pcm_reserve( s, count - start ) < 0 ) {
		return QAA_XMA2_E_NOMEM;
	}
	for ( i = start; i < count; i++ ) {
		int16_t *dst = s->pcm + (size_t)s->pcm_frames * s->nb_channels;
		for ( c = 0; c < s->nb_channels; c++ ) {
			dst[c] = to_pcm16( in[c][i] );
		}
		s->pcm_frames++;
	}
	return 0;
}

/* ---- public API ----------------------------------------------------------- */

static uint32_t rd32( const uint8_t *p ) {
	return (uint32_t)p[0] | ( (uint32_t)p[1] << 8 ) | ( (uint32_t)p[2] << 16 ) | ( (uint32_t)p[3] << 24 );
}

static uint16_t rd16( const uint8_t *p ) {
	return (uint16_t)( p[0] | ( p[1] << 8 ) );
}

int qaa_xma2_parse( const void *file, size_t size, qaa_xma2_info *info ) {
	const uint8_t *b = (const uint8_t *)file;
	size_t p = 12;
	int haveFmt = 0;
	memset( info, 0, sizeof( *info ) );
	if ( !b || size < 12 || memcmp( b, "RIFF", 4 ) || memcmp( b + 8, "WAVE", 4 ) ) {
		return QAA_XMA2_E_NOT_RIFF;
	}
	while ( p + 8 <= size ) {
		const uint8_t *id = b + p;
		const uint32_t len = rd32( b + p + 4 );
		const uint8_t *body = b + p + 8;
		if ( !memcmp( id, "fmt ", 4 ) ) {
			if ( len < 18 || p + 8 + 18 > size ) {
				return QAA_XMA2_E_NO_FMT;
			}
			if ( rd16( body ) != 0x166 ) {
				return QAA_XMA2_E_NOT_XMA2;
			}
			if ( len < 52 || rd16( body + 16 ) < 34 || p + 8 + 52 > size ) {
				return QAA_XMA2_E_UNSUPPORTED;
			}
			info->channels = rd16( body + 2 );
			info->rate = (int)rd32( body + 4 );
			if ( rd16( body + 18 ) != 1 || info->channels < 1 || info->channels > MAX_CHANNELS ||
				info->rate <= 0 || info->rate > 48000 ) {
				return QAA_XMA2_E_UNSUPPORTED;	/* one stream of 1 or 2 channels */
			}
			info->samplesEncoded = rd32( body + 24 );
			info->playBegin = rd32( body + 32 );
			info->playLength = rd32( body + 36 );
			info->loopBegin = rd32( body + 40 );
			info->loopLength = rd32( body + 44 );
			info->loopCount = body[48];
			haveFmt = 1;
		} else if ( !memcmp( id, "data", 4 ) ) {
			if ( !haveFmt ) {
				/* fmt after data: keep looking for fmt, remember data */
				info->dataOffset = (uint32_t)( p + 8 );
				info->dataSize = len;
			} else {
				info->dataOffset = (uint32_t)( p + 8 );
				info->dataSize = len;
				return QAA_XMA2_OK;
			}
		}
		if ( haveFmt && info->dataOffset ) {
			return QAA_XMA2_OK;
		}
		p += 8 + (size_t)len + ( len & 1 );
	}
	if ( !haveFmt ) {
		return QAA_XMA2_E_NO_FMT;
	}
	return info->dataOffset ? QAA_XMA2_OK : QAA_XMA2_E_NO_DATA;
}

uint32_t qaa_xma2_output_frames( const qaa_xma2_info *info ) {
	return info->playLength ? info->playLength : info->samplesEncoded;
}

qaa_xma2_decoder *qaa_xma2_create( int channels, int rate ) {
	qaa_xma2_decoder *s;
	if ( channels < 1 || channels > MAX_CHANNELS || rate <= 0 ) {
		return NULL;
	}
	s = (qaa_xma2_decoder *)calloc( 1, sizeof( *s ) );
	if ( !s ) {
		return NULL;
	}
	s->nb_channels = channels;
	s->rate = rate;
	if ( init_tables( s ) < 0 ) {
		free( s );
		return NULL;
	}
	init_state( s );
	return s;
}

void qaa_xma2_destroy( qaa_xma2_decoder *d ) {
	if ( d ) {
		free( d->pcm );
		free( d );
	}
}

void qaa_xma2_reset( qaa_xma2_decoder *d ) {
	init_state( d );
}

int qaa_xma2_feed( qaa_xma2_decoder *s, const uint8_t *packet ) {
	float out[MAX_CHANNELS][SAMPLES_PER_FRAME];
	int got, ret = 0, first = 1, lastIndex = -1;

	if ( s->ended ) {
		return 0;
	}
	if ( !packet ) {
		/* end of stream: the last frame's overlap, then the end trim */
		if ( !s->eof_done ) {
			int c;
			memset( out, 0, sizeof( out ) );
			for ( c = 0; c < s->nb_channels; c++ ) {
				memcpy( out[c], s->channel[c].out, SAMPLES_PER_FRAME * sizeof( float ) >> 1 );
			}
			s->eof_done = 1;
			if ( push_frame( s, out, SAMPLES_PER_FRAME ) < 0 ) {
				return QAA_XMA2_E_NOMEM;
			}
		}
		{
			const int queued = s->pcm_frames - s->pcm_read;
			int trim = s->xma_trim_end + s->xma_trim_start - 128 - 64;
			if ( trim < 0 ) {
				trim = 0;
			}
			if ( trim > queued ) {
				trim = queued;
			}
			s->pcm_frames -= trim;
		}
		s->ended = 1;
		return 0;
	}

	s->packet = packet;
	for ( ;; ) {
		const int r = decode_packet_step( s, first || s->packet_done || s->packet_loss, out, &got );
		first = 0;
		if ( s->trim_start ) {
			s->xma_trim_start = s->trim_start;
		}
		if ( s->trim_end ) {
			s->xma_trim_end = s->trim_end;
		}
		if ( got && push_frame( s, out, SAMPLES_PER_FRAME ) < 0 ) {
			return QAA_XMA2_E_NOMEM;
		}
		if ( r < 0 ) {
			ret = r;	/* the rest of the packet is dropped */
			break;
		}
		if ( ( s->pgb.index >> 3 ) >= QAA_XMA2_PACKET_SIZE || s->packet_done ) {
			break;
		}
		if ( s->pgb.index == lastIndex ) {
			break;	/* no progress */
		}
		lastIndex = s->pgb.index;
	}
	s->packet_done = 1;	/* the next packet starts with its header */
	return ret;
}

int qaa_xma2_read( qaa_xma2_decoder *s, int16_t *out, int maxFrames ) {
	int avail = s->pcm_frames - s->pcm_read;
	if ( !s->ended ) {
		avail -= HOLDBACK_FRAMES;
	}
	if ( avail <= 0 || maxFrames <= 0 ) {
		return 0;
	}
	if ( avail > maxFrames ) {
		avail = maxFrames;
	}
	memcpy( out, s->pcm + (size_t)s->pcm_read * s->nb_channels, (size_t)avail * s->nb_channels * sizeof( int16_t ) );
	s->pcm_read += avail;
	return avail;
}

int qaa_xma2_decode_file( const void *file, size_t size, int16_t *out, int maxFrames ) {
	qaa_xma2_info info;
	qaa_xma2_decoder *d;
	const uint8_t *b = (const uint8_t *)file;
	uint32_t limit, skip, written = 0, pos;
	int ret = qaa_xma2_parse( file, size, &info );
	size_t end;
	if ( ret < 0 ) {
		return ret;
	}
	d = qaa_xma2_create( info.channels, info.rate );
	if ( !d ) {
		return QAA_XMA2_E_NOMEM;
	}
	limit = qaa_xma2_output_frames( &info );
	if ( (uint32_t)maxFrames < limit ) {
		limit = (uint32_t)maxFrames;
	}
	skip = info.playBegin;
	end = (size_t)info.dataOffset + info.dataSize;
	if ( end > size ) {
		end = size;
	}
	for ( pos = info.dataOffset; ; pos += QAA_XMA2_PACKET_SIZE ) {
		const int last = (size_t)pos + QAA_XMA2_PACKET_SIZE > end;
		int n;
		qaa_xma2_feed( d, last ? NULL : b + pos );
		for ( ;; ) {
			int16_t scratch[1024 * MAX_CHANNELS];
			if ( skip ) {
				n = qaa_xma2_read( d, scratch, (int)( skip < 1024 ? skip : 1024 ) );
				skip -= (uint32_t)n;
			} else {
				if ( written >= limit ) {
					break;
				}
				n = qaa_xma2_read( d, out + (size_t)written * info.channels, (int)( limit - written ) );
				written += (uint32_t)n;
			}
			if ( n <= 0 ) {
				break;
			}
		}
		if ( last || written >= limit ) {
			break;
		}
	}
	qaa_xma2_destroy( d );
	return (int)written;
}

const char *qaa_xma2_error( int code ) {
	switch ( code ) {
	case QAA_XMA2_OK: return "ok";
	case QAA_XMA2_E_NOT_RIFF: return "not a RIFF WAVE file";
	case QAA_XMA2_E_NO_FMT: return "no usable fmt chunk";
	case QAA_XMA2_E_NOT_XMA2: return "not XMA2";
	case QAA_XMA2_E_UNSUPPORTED: return "unsupported XMA2 layout (one stream of 1 or 2 channels)";
	case QAA_XMA2_E_NO_DATA: return "no data chunk";
	case QAA_XMA2_E_NOMEM: return "out of memory";
	case QAA_XMA2_E_INVALID: return "damaged XMA2 packet";
	default: return "unknown error";
	}
}
