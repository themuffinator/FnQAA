/*
 * FnQAA: XMA2 decoder tests.
 *
 * With no arguments: synthetic checks only (no retail data). The half
 * inverse MDCT against its direct formula, perfect reconstruction through
 * the decoder's window and overlap-add, every Huffman table decoding its
 * own codes, and the RIFF parser on a synthetic header.
 *
 * With file arguments: decode each XMA2 file and print one line of
 * statistics (used with the user's own package; nothing is written unless
 * -o <dir> is given, which writes 16-bit PCM WAVs there).
 */
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#include "../code/qaa/qaa_xma2.c"

#include <stdio.h>

static int failures;

#define CHECK( cond, ... ) do { if ( !( cond ) ) { failures++; printf( "FAIL %s:%d: ", __FILE__, __LINE__ ); printf( __VA_ARGS__ ); printf( "\n" ); } } while ( 0 )

static uint32_t rng = 12345;
static float frand( void ) {
	rng = rng * 1664525u + 1013904223u;
	return (float)( ( rng >> 8 ) / 16777216.0 * 2.0 - 1.0 );
}

static void test_imdct_reference( void ) {
	int s;
	for ( s = 0; s < NUM_BLOCK_SIZES; s++ ) {
		const int n = 128 << s;
		mdct_ctx m;
		float in[512], out[512];
		double maxErr = 0, maxRef = 0;
		int k, j;
		mdct_init( &m, n, 1.0f );
		for ( k = 0; k < n; k++ ) {
			in[k] = frand();
		}
		imdct_half( &m, out, in );
		for ( j = 0; j < n; j++ ) {
			/* FFmpeg's imdct reference: -sum, over the middle half */
			double sum = 0;
			const int i = n / 2 + j;
			for ( k = 0; k < n; k++ ) {
				sum += in[k] * cos( M_PI / n * ( i + 0.5 + n / 2.0 ) * ( k + 0.5 ) );
			}
			sum = -sum;
			if ( fabs( sum - out[j] ) > maxErr ) {
				maxErr = fabs( sum - out[j] );
			}
			if ( fabs( sum ) > maxRef ) {
				maxRef = fabs( sum );
			}
		}
		CHECK( maxErr < 1e-4 * maxRef, "imdct_half n=%d: max error %g of %g", n, maxErr, maxRef );
	}
}

/* Forward MDCT of 2n windowed samples, the matching analysis transform. */
static void mdct_forward( int n, const float *x, const float *win2n, float *X ) {
	int k, i;
	for ( k = 0; k < n; k++ ) {
		double sum = 0;
		for ( i = 0; i < 2 * n; i++ ) {
			sum += win2n[i] * x[i] * cos( M_PI / n * ( i + 0.5 + n / 2.0 ) * ( k + 0.5 ) );
		}
		X[k] = (float)sum;
	}
}

static void test_reconstruction( void ) {
	/* full-size blocks, laid out as decode_subframe / decode_frame do */
	enum { N = SAMPLES_PER_FRAME, FRAMES = 8 };
	static float x[( FRAMES + 2 ) * N], y[FRAMES * N];
	float win[2 * N], X[N], out[OUT_SIZE];
	mdct_ctx m;
	qaa_xma2_decoder *d = qaa_xma2_create( 1, 22050 );
	int t, i;
	double num = 0, den = 0, err = 0;
	CHECK( d != NULL, "create" );
	if ( !d ) {
		return;
	}
	mdct_init( &m, N, 2.0f / N );	/* perfect reconstruction, as WMA Pro scales */
	for ( i = 0; i < 2 * N; i++ ) {
		win[i] = (float)sin( ( i + 0.5 ) * M_PI / ( 2.0 * N ) );
	}
	for ( i = 0; i < ( FRAMES + 2 ) * N; i++ ) {
		x[i] = (float)( 0.5 * sin( i * 0.031 ) + 0.25 * sin( i * 0.77 ) ) + 0.1f * frand();
	}
	memset( out, 0, sizeof( out ) );
	for ( t = 0; t < FRAMES; t++ ) {
		float *coeffs = out + N / 2;
		mdct_forward( N, x + t * N, win, X );
		imdct_half( &m, coeffs, X );
		fmul_window( out, out, out + N / 2, d->windows[av_log2( N ) - 6], N / 2 );
		memcpy( y + t * N, out, N * sizeof( float ) );
		memcpy( out, out + N, N / 2 * sizeof( float ) );
	}
	/* frame t's output is input samples t*N .. t*N + N, from the second frame */
	for ( t = 1; t < FRAMES; t++ ) {
		for ( i = 0; i < N; i++ ) {
			const double ref = x[t * N + i];
			num += ref * y[t * N + i];
			den += ref * ref;
		}
	}
	{
		const double g = num / den;
		for ( t = 1; t < FRAMES; t++ ) {
			for ( i = 0; i < N; i++ ) {
				const double ref = x[t * N + i];
				const double e = y[t * N + i] - g * ref;
				err += e * e;
			}
		}
		CHECK( fabs( fabs( g ) - 1.0 ) < 1e-3, "reconstruction gain %g, expected +-1", g );
		CHECK( err < 1e-6 * den, "reconstruction error %g of %g", err, den );
	}
	qaa_xma2_destroy( d );
}

/* Encode each table's codes as the builder assigns them, then decode them. */
static void roundtrip( const char *name, const vlc_table *t, int count, const uint8_t *lens, int lensWrap,
	const void *syms, int symsWrap, int symSize, int offset ) {
	uint8_t buf[64];
	uint64_t code = 0;
	const uint8_t *s = (const uint8_t *)syms;
	int i, bad = 0;
	for ( i = 0; i < count; i++, lens += lensWrap, s += symsWrap ) {
		const int len = (int8_t)*lens;
		if ( len > 0 ) {
			bitreader b;
			int sym = s[0], got, k;
			const uint32_t c = (uint32_t)( code >> ( 32 - len ) );
			if ( symSize == 2 ) {
				uint16_t u;
				memcpy( &u, s, 2 );
				sym = u;
			}
			memset( buf, 0, sizeof( buf ) );
			for ( k = 0; k < len; k++ ) {
				if ( ( c >> ( len - 1 - k ) ) & 1 ) {
					buf[k >> 3] |= (uint8_t)( 0x80 >> ( k & 7 ) );
				}
			}
			br_init( &b, buf, sizeof( buf ) );
			got = vlc_get( &b, t );
			if ( got != sym + offset || b.index != len ) {
				bad++;
			}
		}
		code += 1ull << ( 32 - ( len < 0 ? -len : len ) );
	}
	CHECK( !bad, "%s: %d codes do not round-trip", name, bad );
}

static void test_vlc( void ) {
	qaa_xma2_decoder *d = qaa_xma2_create( 2, 44100 );
	CHECK( d != NULL, "create" );
	if ( !d ) {
		return;
	}
	roundtrip( "scale", &d->sf_vlc, HUFF_SCALE_SIZE, &scale_table[0][1], 2, &scale_table[0][0], 2, 1, -60 );
	roundtrip( "scale_rl", &d->sf_rl_vlc, HUFF_SCALE_RL_SIZE, &scale_rl_table[0][1], 2, &scale_rl_table[0][0], 2, 1, 0 );
	roundtrip( "coef0", &d->coef_vlc[0], HUFF_COEF0_SIZE, coef0_lens, 1, coef0_syms, 2, 2, 0 );
	roundtrip( "coef1", &d->coef_vlc[1], HUFF_COEF1_SIZE, &coef1_table[0][1], 2, &coef1_table[0][0], 2, 1, 0 );
	roundtrip( "vec4", &d->vec4_vlc, HUFF_VEC4_SIZE, vec4_lens, 1, vec4_syms, 2, 2, -1 );
	roundtrip( "vec2", &d->vec2_vlc, HUFF_VEC2_SIZE, &vec2_table[0][1], 2, &vec2_table[0][0], 2, 1, -1 );
	roundtrip( "vec1", &d->vec1_vlc, HUFF_VEC1_SIZE, &vec1_table[0][1], 2, &vec1_table[0][0], 2, 1, 0 );
	qaa_xma2_destroy( d );
}

static void put32( uint8_t *p, uint32_t v ) {
	p[0] = (uint8_t)v; p[1] = (uint8_t)( v >> 8 ); p[2] = (uint8_t)( v >> 16 ); p[3] = (uint8_t)( v >> 24 );
}

static void put16( uint8_t *p, uint16_t v ) {
	p[0] = (uint8_t)v; p[1] = (uint8_t)( v >> 8 );
}

static void test_parse( void ) {
	uint8_t f[12 + 8 + 52 + 8 + 16 + 8 + 2048];
	qaa_xma2_info info;
	uint8_t *p = f;
	memset( f, 0, sizeof( f ) );
	memcpy( p, "RIFF", 4 ); put32( p + 4, sizeof( f ) - 8 ); memcpy( p + 8, "WAVE", 4 ); p += 12;
	memcpy( p, "fmt ", 4 ); put32( p + 4, 52 ); p += 8;
	put16( p, 0x166 ); put16( p + 2, 2 ); put32( p + 4, 22050 ); put16( p + 14, 16 ); put16( p + 16, 34 );
	put16( p + 18, 1 ); put32( p + 24, 1024 ); put32( p + 36, 900 ); p += 52;
	memcpy( p, "seek", 4 ); put32( p + 4, 16 ); p += 8 + 16;
	memcpy( p, "data", 4 ); put32( p + 4, 2048 ); p += 8;
	CHECK( qaa_xma2_parse( f, sizeof( f ), &info ) == 0, "parse" );
	CHECK( info.channels == 2 && info.rate == 22050 && info.samplesEncoded == 1024 && info.playLength == 900,
		"fields %d %d %u %u", info.channels, info.rate, info.samplesEncoded, info.playLength );
	CHECK( info.dataOffset == (uint32_t)( p - f ) && info.dataSize == 2048, "data %u %u", info.dataOffset, info.dataSize );
	CHECK( qaa_xma2_output_frames( &info ) == 900, "output frames" );
	put16( f + 20, 1 );
	CHECK( qaa_xma2_parse( f, sizeof( f ), &info ) == QAA_XMA2_E_NOT_XMA2, "PCM is not XMA2" );
	/* a packet of zeros decodes without crashing */
	put16( f + 20, 0x166 );
	{
		int16_t pcm[2048 * 2];
		const int n = qaa_xma2_decode_file( f, sizeof( f ), pcm, 900 );
		CHECK( n >= 0, "decode of an empty packet: %d", n );
	}
}

/* ---- real files ----------------------------------------------------------- */

static uint8_t *read_file( const char *path, size_t *size ) {
	FILE *fp = fopen( path, "rb" );
	uint8_t *b;
	long n;
	if ( !fp ) {
		return NULL;
	}
	fseek( fp, 0, SEEK_END );
	n = ftell( fp );
	fseek( fp, 0, SEEK_SET );
	b = (uint8_t *)malloc( (size_t)n + 1 );
	if ( b && fread( b, 1, (size_t)n, fp ) != (size_t)n ) {
		free( b );
		b = NULL;
	}
	fclose( fp );
	*size = (size_t)n;
	return b;
}

static void write_wav( const char *dir, const char *path, const int16_t *pcm, int frames, int ch, int rate ) {
	char out[1024];
	const char *base = path + strlen( path );
	FILE *fp;
	uint8_t h[44];
	const uint32_t bytes = (uint32_t)frames * ch * 2;
	while ( base > path && base[-1] != '/' && base[-1] != '\\' ) {
		base--;
	}
	snprintf( out, sizeof( out ), "%s/%s.wav", dir, base );
	fp = fopen( out, "wb" );
	if ( !fp ) {
		return;
	}
	memcpy( h, "RIFF", 4 ); put32( h + 4, 36 + bytes ); memcpy( h + 8, "WAVEfmt ", 8 ); put32( h + 16, 16 );
	put16( h + 20, 1 ); put16( h + 22, (uint16_t)ch ); put32( h + 24, (uint32_t)rate );
	put32( h + 28, (uint32_t)rate * ch * 2 ); put16( h + 32, (uint16_t)( ch * 2 ) ); put16( h + 34, 16 );
	memcpy( h + 36, "data", 4 ); put32( h + 40, bytes );
	fwrite( h, 1, 44, fp );
	fwrite( pcm, 1, bytes, fp );
	fclose( fp );
}

static int decode_files( int argc, char **argv ) {
	const char *outDir = NULL;
	int i, bad = 0;
	for ( i = 1; i < argc; i++ ) {
		size_t size;
		uint8_t *f;
		qaa_xma2_info info;
		int16_t *pcm;
		int n, r, c, clipped = 0;
		long peak = 0;
		double diff = 0, sum = 0;
		if ( !strcmp( argv[i], "-o" ) && i + 1 < argc ) {
			outDir = argv[++i];
			continue;
		}
		f = read_file( argv[i], &size );
		if ( !f ) {
			printf( "%s: unreadable\n", argv[i] );
			bad++;
			continue;
		}
		r = qaa_xma2_parse( f, size, &info );
		if ( r < 0 ) {
			printf( "%s: %s\n", argv[i], qaa_xma2_error( r ) );
			free( f );
			bad++;
			continue;
		}
		pcm = (int16_t *)malloc( (size_t)qaa_xma2_output_frames( &info ) * info.channels * 2 + 2 );
		n = qaa_xma2_decode_file( f, size, pcm, (int)qaa_xma2_output_frames( &info ) );
		for ( r = 0; r < n * info.channels; r++ ) {
			const long v = labs( (long)pcm[r] );
			peak = v > peak ? v : peak;
			clipped += v >= 32767;
		}
		/* smoothness: mean |x[t]-x[t-1]| against mean |x[t]| */
		for ( c = 0; c < info.channels; c++ ) {
			for ( r = 1; r < n; r++ ) {
				diff += fabs( (double)pcm[r * info.channels + c] - pcm[( r - 1 ) * info.channels + c] );
				sum += fabs( (double)pcm[r * info.channels + c] );
			}
		}
		printf( "%s: ch %d rate %d frames %d of %u (encoded %u) peak %ld clipped %d rough %.3f\n", argv[i],
			info.channels, info.rate, n, qaa_xma2_output_frames( &info ), info.samplesEncoded, peak, clipped,
			sum > 0 ? diff / sum : 0.0 );
		if ( n != (int)qaa_xma2_output_frames( &info ) ) {
			bad++;
		}
		if ( outDir && n > 0 ) {
			write_wav( outDir, argv[i], pcm, n, info.channels, info.rate );
		}
		free( pcm );
		free( f );
	}
	return bad ? 1 : 0;
}

int main( int argc, char **argv ) {
	if ( argc > 1 ) {
		return decode_files( argc, argv );
	}
	test_imdct_reference();
	test_reconstruction();
	test_vlc();
	test_parse();
	if ( failures ) {
		printf( "%d failure(s)\n", failures );
		return 1;
	}
	printf( "qaa_xma2: all synthetic checks passed\n" );
	return 0;
}
