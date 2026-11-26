/*
===========================================================================
openQAA: Xbox LZX (XMemCompress) decoder.

Copyright (C) 2026 openQAA contributors.
This file is part of openQAA, licensed under the GNU GPL v2 or later.

An independent implementation of the LZX format as Microsoft documents it,
with the framing the Xbox 360 XMemCompress library and XEX2 images use:

  - the bitstream is 16-bit little-endian words read MSB first;
  - output is produced in frames of at most 32 KiB, each stored as its own
    byte-aligned chunk, so the bit reader restarts with every chunk while the
    window, the repeated offsets, the code lengths and the current block
    carry over;
  - the native (0x0FF512EE) container compresses each of its blocks as an
    independent stream, so all state resets at every block (observed: each
    block of a multi-block pak0.bpk entry restarts with a stream header);
  - a chunk is prefixed with a big-endian 16-bit compressed size, or with
    0xFF, a 16-bit uncompressed size and a 16-bit compressed size when the
    frame is shorter than 32 KiB.

The whole output buffer serves as the window, so a match may reach back as
far as the stream allows but never before the start of the output.
===========================================================================
*/
#include "qaa_lzx.h"

#include <stdlib.h>
#include <string.h>

enum {
	LZX_OK = 0,
	LZX_ERR_ARGS = -1,
	LZX_ERR_HEADER = -2,
	LZX_ERR_TRUNCATED = -3,
	LZX_ERR_BLOCKTYPE = -4,
	LZX_ERR_TREE = -5,
	LZX_ERR_SYMBOL = -6,
	LZX_ERR_OFFSET = -7,
	LZX_ERR_OVERRUN = -8,
	LZX_ERR_NOMEM = -9,
	LZX_ERR_WINDOW = -10,
};

#define LZX_FRAME_SIZE      0x8000
#define LZX_NUM_CHARS       256
#define LZX_PRETREE_SYMS    20
#define LZX_LENGTH_SYMS     249
#define LZX_ALIGNED_SYMS    8
#define LZX_MAX_POS_SLOTS   51
#define LZX_MAIN_MAX        ( LZX_NUM_CHARS + LZX_MAX_POS_SLOTS * 8 )
#define LZX_MIN_MATCH       2
#define LZX_TABLE_BITS      16

enum { BLOCK_VERBATIM = 1, BLOCK_ALIGNED = 2, BLOCK_UNCOMPRESSED = 3 };

typedef struct {
	uint16_t sym[1u << LZX_TABLE_BITS];
	uint8_t  len[1u << LZX_TABLE_BITS];
	int      empty;
} lzxTree_t;

typedef struct {
	const uint8_t *p;
	const uint8_t *end;
	uint32_t buf;     /* unread bits, MSB aligned */
	int      bits;
} lzxBits_t;

typedef struct {
	uint8_t *out;
	size_t   outSize;
	size_t   pos;
	size_t   base;    /* output position where the current stream began */

	unsigned windowBits;
	unsigned posSlots;
	unsigned mainSyms;

	uint32_t R0, R1, R2;
	int      headerRead;
	int      blockType;
	uint32_t blockLength;
	uint32_t blockRemaining;

	uint8_t  mainLen[LZX_MAIN_MAX];
	uint8_t  lengthLen[LZX_LENGTH_SYMS];
	uint8_t  alignedLen[LZX_ALIGNED_SYMS];

	lzxTree_t pretree, mainTree, lengthTree, alignedTree;
} lzxState_t;

static uint8_t  extraBits[LZX_MAX_POS_SLOTS];
static uint32_t positionBase[LZX_MAX_POS_SLOTS];
static int      tablesReady;

static void LZX_InitTables( void ) {
	unsigned i, j = 0;
	if ( tablesReady ) {
		return;
	}
	for ( i = 0; i < LZX_MAX_POS_SLOTS; i += 2 ) {
		extraBits[i] = (uint8_t)j;
		if ( i + 1 < LZX_MAX_POS_SLOTS ) {
			extraBits[i + 1] = (uint8_t)j;
		}
		if ( i != 0 && j < 17 ) {
			j++;
		}
	}
	positionBase[0] = 0;
	for ( i = 1; i < LZX_MAX_POS_SLOTS; i++ ) {
		positionBase[i] = positionBase[i - 1] + ( 1u << extraBits[i - 1] );
	}
	tablesReady = 1;
}

static unsigned LZX_PositionSlots( unsigned windowBits ) {
	/* 15..21 bits: two slots per bit; above that the slot count grows faster */
	static const unsigned slots[] = { 30, 32, 34, 36, 38, 42, 50 };
	if ( windowBits < 15 || windowBits > 21 ) {
		return 0;
	}
	return slots[windowBits - 15];
}

/* ---- bit reader ------------------------------------------------------ */

static void Bits_Init( lzxBits_t *b, const uint8_t *p, size_t size ) {
	b->p = p;
	b->end = p + size;
	b->buf = 0;
	b->bits = 0;
}

static void Bits_Ensure( lzxBits_t *b, int n ) {
	while ( b->bits < n ) {
		uint32_t word = 0;
		if ( b->p + 1 < b->end ) {
			word = (uint32_t)b->p[0] | ( (uint32_t)b->p[1] << 8 );
		} else if ( b->p < b->end ) {
			word = b->p[0];
		}
		b->p += 2;
		b->buf |= word << ( 16 - b->bits );
		b->bits += 16;
	}
}

static uint32_t Bits_Peek( const lzxBits_t *b, int n ) {
	return b->buf >> ( 32 - n );
}

static void Bits_Remove( lzxBits_t *b, int n ) {
	b->buf <<= n;
	b->bits -= n;
}

static uint32_t Bits_Read( lzxBits_t *b, int n ) {
	uint32_t v;
	if ( n == 0 ) {
		return 0;
	}
	if ( n > 16 ) {
		v = Bits_Read( b, 16 ) << ( n - 16 );
		return v | Bits_Read( b, n - 16 );
	}
	Bits_Ensure( b, n );
	v = Bits_Peek( b, n );
	Bits_Remove( b, n );
	return v;
}

static int Bits_Overread( const lzxBits_t *b ) {
	/* the reader may run up to one word ahead of the data it really uses */
	return b->p > b->end + 2;
}

/* ---- Huffman tables -------------------------------------------------- */

static int Tree_Build( lzxTree_t *t, const uint8_t *lens, unsigned count ) {
	unsigned bl, s, used = 0;
	uint32_t code = 0;

	t->empty = 1;
	memset( t->len, 0, sizeof( t->len ) );
	for ( bl = 1; bl <= LZX_TABLE_BITS; bl++ ) {
		for ( s = 0; s < count; s++ ) {
			uint32_t first, n;
			if ( lens[s] != bl ) {
				continue;
			}
			first = code << ( LZX_TABLE_BITS - bl );
			n = 1u << ( LZX_TABLE_BITS - bl );
			if ( first + n > ( 1u << LZX_TABLE_BITS ) ) {
				return LZX_ERR_TREE;
			}
			for ( uint32_t i = 0; i < n; i++ ) {
				t->sym[first + i] = (uint16_t)s;
				t->len[first + i] = (uint8_t)bl;
			}
			code++;
			used++;
		}
		code <<= 1;
	}
	for ( s = 0; s < count; s++ ) {
		if ( lens[s] > LZX_TABLE_BITS ) {
			return LZX_ERR_TREE;
		}
	}
	t->empty = ( used == 0 );
	return LZX_OK;
}

static int Tree_Decode( const lzxTree_t *t, lzxBits_t *b, unsigned *sym ) {
	uint32_t idx;
	Bits_Ensure( b, LZX_TABLE_BITS );
	idx = Bits_Peek( b, LZX_TABLE_BITS );
	if ( t->len[idx] == 0 ) {
		return LZX_ERR_SYMBOL;
	}
	*sym = t->sym[idx];
	Bits_Remove( b, t->len[idx] );
	return LZX_OK;
}

/* Delta-coded code lengths for lens[first..last), through a fresh pretree. */
static int LZX_ReadLengths( lzxState_t *s, lzxBits_t *b, uint8_t *lens, unsigned first, unsigned last ) {
	uint8_t pre[LZX_PRETREE_SYMS];
	unsigned i, x, z, run;
	int err;

	for ( i = 0; i < LZX_PRETREE_SYMS; i++ ) {
		pre[i] = (uint8_t)Bits_Read( b, 4 );
	}
	if ( ( err = Tree_Build( &s->pretree, pre, LZX_PRETREE_SYMS ) ) != LZX_OK ) {
		return err;
	}
	for ( x = first; x < last; ) {
		if ( ( err = Tree_Decode( &s->pretree, b, &z ) ) != LZX_OK ) {
			return err;
		}
		if ( z == 17 ) {
			run = Bits_Read( b, 4 ) + 4;
			while ( run-- && x < last ) {
				lens[x++] = 0;
			}
		} else if ( z == 18 ) {
			run = Bits_Read( b, 5 ) + 20;
			while ( run-- && x < last ) {
				lens[x++] = 0;
			}
		} else if ( z == 19 ) {
			run = Bits_Read( b, 1 ) + 4;
			if ( ( err = Tree_Decode( &s->pretree, b, &z ) ) != LZX_OK ) {
				return err;
			}
			if ( z > 16 ) {
				return LZX_ERR_TREE;
			}
			z = ( lens[x] + 17 - z ) % 17;
			while ( run-- && x < last ) {
				lens[x++] = (uint8_t)z;
			}
		} else {
			lens[x] = (uint8_t)( ( lens[x] + 17 - z ) % 17 );
			x++;
		}
	}
	return LZX_OK;
}

/* ---- block and frame decoding ---------------------------------------- */

static int LZX_ReadBlockHeader( lzxState_t *s, lzxBits_t *b ) {
	int err;
	unsigned i;

	s->blockType = (int)Bits_Read( b, 3 );
	s->blockLength = Bits_Read( b, 16 ) << 8;
	s->blockLength |= Bits_Read( b, 8 );
	s->blockRemaining = s->blockLength;

	switch ( s->blockType ) {
	case BLOCK_ALIGNED:
		for ( i = 0; i < LZX_ALIGNED_SYMS; i++ ) {
			s->alignedLen[i] = (uint8_t)Bits_Read( b, 3 );
		}
		if ( ( err = Tree_Build( &s->alignedTree, s->alignedLen, LZX_ALIGNED_SYMS ) ) != LZX_OK ) {
			return err;
		}
		/* fall through: the rest is laid out as a verbatim block */
	case BLOCK_VERBATIM:
		if ( ( err = LZX_ReadLengths( s, b, s->mainLen, 0, LZX_NUM_CHARS ) ) != LZX_OK ) {
			return err;
		}
		if ( ( err = LZX_ReadLengths( s, b, s->mainLen, LZX_NUM_CHARS, s->mainSyms ) ) != LZX_OK ) {
			return err;
		}
		if ( ( err = Tree_Build( &s->mainTree, s->mainLen, s->mainSyms ) ) != LZX_OK ) {
			return err;
		}
		if ( ( err = LZX_ReadLengths( s, b, s->lengthLen, 0, LZX_LENGTH_SYMS ) ) != LZX_OK ) {
			return err;
		}
		return Tree_Build( &s->lengthTree, s->lengthLen, LZX_LENGTH_SYMS );

	case BLOCK_UNCOMPRESSED: {
		uint32_t r[3];
		/* pad to the next 16-bit boundary: one to sixteen bits, never none */
		if ( b->bits == 0 ) {
			Bits_Ensure( b, 16 );
		}
		b->bits = 0;
		b->buf = 0;
		if ( b->p + 12 > b->end ) {
			return LZX_ERR_TRUNCATED;
		}
		for ( i = 0; i < 3; i++ ) {
			r[i] = (uint32_t)b->p[0] | ( (uint32_t)b->p[1] << 8 ) |
				( (uint32_t)b->p[2] << 16 ) | ( (uint32_t)b->p[3] << 24 );
			b->p += 4;
		}
		s->R0 = r[0];
		s->R1 = r[1];
		s->R2 = r[2];
		return LZX_OK;
	}

	default:
		return LZX_ERR_BLOCKTYPE;
	}
}

static int LZX_DecodeRun( lzxState_t *s, lzxBits_t *b, uint32_t run ) {
	const size_t end = s->pos + run;
	int err;

	if ( s->blockType == BLOCK_UNCOMPRESSED ) {
		if ( b->p + run > b->end ) {
			return LZX_ERR_TRUNCATED;
		}
		memcpy( s->out + s->pos, b->p, run );
		b->p += run;
		s->pos = end;
		return LZX_OK;
	}

	while ( s->pos < end ) {
		unsigned sym, slot, len, extra;
		uint32_t offset;

		if ( ( err = Tree_Decode( &s->mainTree, b, &sym ) ) != LZX_OK ) {
			return err;
		}
		if ( sym < LZX_NUM_CHARS ) {
			s->out[s->pos++] = (uint8_t)sym;
			continue;
		}
		sym -= LZX_NUM_CHARS;
		len = sym & 7;
		if ( len == 7 ) {
			unsigned more;
			if ( ( err = Tree_Decode( &s->lengthTree, b, &more ) ) != LZX_OK ) {
				return err;
			}
			len += more;
		}
		len += LZX_MIN_MATCH;

		slot = sym >> 3;
		if ( slot == 0 ) {
			offset = s->R0;
		} else if ( slot == 1 ) {
			offset = s->R1;
			s->R1 = s->R0;
			s->R0 = offset;
		} else if ( slot == 2 ) {
			offset = s->R2;
			s->R2 = s->R0;
			s->R0 = offset;
		} else {
			extra = extraBits[slot];
			offset = positionBase[slot] - 2;
			if ( s->blockType == BLOCK_ALIGNED && extra >= 3 ) {
				unsigned aligned;
				offset += Bits_Read( b, (int)extra - 3 ) << 3;
				if ( ( err = Tree_Decode( &s->alignedTree, b, &aligned ) ) != LZX_OK ) {
					return err;
				}
				offset += aligned;
			} else if ( extra > 0 ) {
				offset += Bits_Read( b, (int)extra );
			} else {
				offset = 1;
			}
			s->R2 = s->R1;
			s->R1 = s->R0;
			s->R0 = offset;
		}

		if ( offset == 0 || offset > s->pos - s->base ) {
			return LZX_ERR_OFFSET;
		}
		if ( s->pos + len > end ) {
			return LZX_ERR_OVERRUN;
		}
		{
			const uint8_t *src = s->out + s->pos - offset;
			uint8_t *dst = s->out + s->pos;
			/* byte by byte: a match may overlap its own output */
			for ( unsigned i = 0; i < len; i++ ) {
				dst[i] = src[i];
			}
		}
		s->pos += len;
	}
	return Bits_Overread( b ) ? LZX_ERR_TRUNCATED : LZX_OK;
}

static int LZX_DecodeFrameBits( lzxState_t *s, lzxBits_t *b, uint32_t frameSize ) {
	int err;

	if ( frameSize > s->outSize - s->pos ) {
		frameSize = (uint32_t)( s->outSize - s->pos );
	}

	if ( !s->headerRead ) {
		/* E8 call translation is an x86 feature; Xbox streams never set it */
		if ( Bits_Read( b, 1 ) ) {
			Bits_Read( b, 16 );
			Bits_Read( b, 16 );
		}
		s->headerRead = 1;
	}

	while ( frameSize > 0 ) {
		uint32_t run;
		if ( s->blockRemaining == 0 ) {
			if ( ( err = LZX_ReadBlockHeader( s, b ) ) != LZX_OK ) {
				return err;
			}
		}
		run = s->blockRemaining < frameSize ? s->blockRemaining : frameSize;
		if ( ( err = LZX_DecodeRun( s, b, run ) ) != LZX_OK ) {
			return err;
		}
		s->blockRemaining -= run;
		frameSize -= run;
		if ( s->blockRemaining == 0 && s->blockType == BLOCK_UNCOMPRESSED && ( s->blockLength & 1 ) ) {
			b->p++;  /* uncompressed blocks are padded to an even length */
		}
	}
	return LZX_OK;
}

static int LZX_DecodeFrame( lzxState_t *s, const uint8_t *in, size_t inSize, uint32_t frameSize ) {
	lzxBits_t b;
	Bits_Init( &b, in, inSize );
	return LZX_DecodeFrameBits( s, &b, frameSize );
}

/* Start a new LZX stream at the current output position. */
static void LZX_Reset( lzxState_t *s ) {
	s->base = s->pos;
	s->R0 = s->R1 = s->R2 = 1;
	s->headerRead = 0;
	s->blockType = 0;
	s->blockLength = 0;
	s->blockRemaining = 0;
	memset( s->mainLen, 0, sizeof( s->mainLen ) );
	memset( s->lengthLen, 0, sizeof( s->lengthLen ) );
	memset( s->alignedLen, 0, sizeof( s->alignedLen ) );
}

static int LZX_Begin( lzxState_t **out, uint8_t *dst, size_t dstSize, unsigned windowBits ) {
	lzxState_t *s;
	LZX_InitTables();
	if ( LZX_PositionSlots( windowBits ) == 0 ) {
		return LZX_ERR_WINDOW;
	}
	s = (lzxState_t *)calloc( 1, sizeof( *s ) );
	if ( !s ) {
		return LZX_ERR_NOMEM;
	}
	s->out = dst;
	s->outSize = dstSize;
	s->windowBits = windowBits;
	s->posSlots = LZX_PositionSlots( windowBits );
	s->mainSyms = LZX_NUM_CHARS + s->posSlots * 8;
	LZX_Reset( s );
	*out = s;
	return LZX_OK;
}

static uint16_t BE16( const uint8_t *p ) {
	return (uint16_t)( ( p[0] << 8 ) | p[1] );
}

static uint32_t BE32( const uint8_t *p ) {
	return ( (uint32_t)p[0] << 24 ) | ( (uint32_t)p[1] << 16 ) | ( (uint32_t)p[2] << 8 ) | p[3];
}

/*
 * Decode chunks from [*pp, end) until `want` more output bytes exist or a
 * zero-sized chunk ends the run.
 */
static int LZX_DecodeChunks( lzxState_t *s, const uint8_t **pp, const uint8_t *end, size_t want ) {
	const uint8_t *p = *pp;
	const size_t goal = s->pos + want;
	int err;

	while ( s->pos < goal ) {
		uint32_t frame = LZX_FRAME_SIZE, csize;
		if ( p + 2 > end ) {
			return LZX_ERR_TRUNCATED;
		}
		if ( p[0] == 0xFF ) {
			if ( p + 5 > end ) {
				return LZX_ERR_TRUNCATED;
			}
			frame = BE16( p + 1 );
			csize = BE16( p + 3 );
			p += 5;
		} else {
			csize = BE16( p );
			p += 2;
		}
		if ( csize == 0 || frame == 0 ) {
			break;
		}
		if ( p + csize > end ) {
			return LZX_ERR_TRUNCATED;
		}
		if ( ( err = LZX_DecodeFrame( s, p, csize, frame ) ) != LZX_OK ) {
			return err;
		}
		p += csize;
	}
	*pp = p;
	return LZX_OK;
}

static unsigned LZX_Log2( uint32_t v ) {
	unsigned n = 0;
	while ( v > 1 ) {
		v >>= 1;
		n++;
	}
	return n;
}

int qaa_lzx_native_info( const uint8_t *in, size_t inSize, uint64_t *uncompressed,
	uint64_t *compressed, uint32_t *windowSize ) {
	if ( !in || inSize < 0x30 || BE32( in ) != 0x0FF512EEu ) {
		return LZX_ERR_HEADER;
	}
	if ( uncompressed ) {
		*uncompressed = ( (uint64_t)BE32( in + 0x18 ) << 32 ) | BE32( in + 0x1C );
	}
	if ( compressed ) {
		*compressed = ( (uint64_t)BE32( in + 0x20 ) << 32 ) | BE32( in + 0x24 );
	}
	if ( windowSize ) {
		*windowSize = BE32( in + 0x10 );
	}
	return LZX_OK;
}

int qaa_lzx_decode_native( const uint8_t *in, size_t inSize, uint8_t *out, size_t outSize ) {
	uint64_t usize;
	uint32_t window, blockUSize;
	const uint8_t *p, *end = in + inSize;
	lzxState_t *s;
	int err;

	if ( !in || ( !out && outSize ) ) {
		return LZX_ERR_ARGS;
	}
	if ( ( err = qaa_lzx_native_info( in, inSize, &usize, NULL, &window ) ) != LZX_OK ) {
		return err;
	}
	if ( usize != outSize || window == 0 || ( window & ( window - 1 ) ) ) {
		return LZX_ERR_HEADER;
	}
	blockUSize = BE32( in + 0x28 );
	if ( blockUSize == 0 ) {
		return LZX_ERR_HEADER;
	}
	if ( ( err = LZX_Begin( &s, out, outSize, LZX_Log2( window ) ) ) != LZX_OK ) {
		return err;
	}

	p = in + 0x30;
	while ( err == LZX_OK && s->pos < outSize ) {
		uint32_t bsize;
		const uint8_t *bp, *bend;
		size_t want = outSize - s->pos;
		if ( p + 4 > end ) {
			err = LZX_ERR_TRUNCATED;
			break;
		}
		bsize = BE32( p );
		p += 4;
		if ( bsize == 0 || p + bsize > end ) {
			err = LZX_ERR_TRUNCATED;
			break;
		}
		bp = p;
		bend = p + bsize;
		/* every block is compressed as an independent stream */
		LZX_Reset( s );
		if ( want > blockUSize ) {
			want = blockUSize;
		}
		err = LZX_DecodeChunks( s, &bp, bend, want );
		p = bend;
	}
	free( s );
	return err;
}

int qaa_lzx_decode_frames( const uint8_t *in, size_t inSize, uint8_t *out, size_t outSize,
	unsigned windowBits ) {
	const uint8_t *p = in;
	lzxState_t *s;
	int err;

	if ( !in || ( !out && outSize ) ) {
		return LZX_ERR_ARGS;
	}
	if ( ( err = LZX_Begin( &s, out, outSize, windowBits ) ) != LZX_OK ) {
		return err;
	}
	err = LZX_DecodeChunks( s, &p, in + inSize, outSize );
	if ( err == LZX_OK && s->pos != outSize ) {
		err = LZX_ERR_TRUNCATED;
	}
	free( s );
	return err;
}

int qaa_lzx_decode_raw( const uint8_t *in, size_t inSize, uint8_t *out, size_t outSize,
	unsigned windowBits ) {
	lzxBits_t b;
	lzxState_t *s;
	int err;

	if ( !in || ( !out && outSize ) ) {
		return LZX_ERR_ARGS;
	}
	if ( ( err = LZX_Begin( &s, out, outSize, windowBits ) ) != LZX_OK ) {
		return err;
	}
	Bits_Init( &b, in, inSize );
	while ( err == LZX_OK && s->pos < outSize ) {
		err = LZX_DecodeFrameBits( s, &b, LZX_FRAME_SIZE );
		/* each frame ends on a 16-bit boundary */
		Bits_Remove( &b, b.bits & 15 );
		if ( err == LZX_OK && Bits_Overread( &b ) ) {
			err = LZX_ERR_TRUNCATED;
		}
	}
	free( s );
	return err;
}

const char *qaa_lzx_error( int code ) {
	switch ( code ) {
	case LZX_OK: return "ok";
	case LZX_ERR_ARGS: return "bad arguments";
	case LZX_ERR_HEADER: return "bad XMemCompress header";
	case LZX_ERR_TRUNCATED: return "truncated input";
	case LZX_ERR_BLOCKTYPE: return "bad block type";
	case LZX_ERR_TREE: return "bad Huffman code lengths";
	case LZX_ERR_SYMBOL: return "invalid Huffman symbol";
	case LZX_ERR_OFFSET: return "match offset before start of output";
	case LZX_ERR_OVERRUN: return "match runs past the end of its frame";
	case LZX_ERR_NOMEM: return "out of memory";
	case LZX_ERR_WINDOW: return "unsupported window size";
	default: return "unknown error";
	}
}
