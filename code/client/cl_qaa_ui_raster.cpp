/*
===========================================================================
FnQAA: software rasteriser for QAA's Scaleform front end.

Copyright (C) 2026 FnQAA contributors.
This file is part of FnQAA, licensed under the GNU GPL v2 or later.

See cl_qaa_ui_raster.h.
===========================================================================
*/
#include "cl_qaa_ui_raster.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <mutex>
#include <thread>
#include <type_traits>

#if defined( _M_X64 ) || defined( _M_AMD64 ) || defined( __SSE2__ ) || ( defined( _M_IX86_FP ) && _M_IX86_FP >= 2 )
#define QAA_UI_SSE2 1
#include <emmintrin.h>
#endif

namespace qaa_ui {

/* ---- worker pool ------------------------------------------------------------

Flush() splits the canvas into bands of rows and runs them on a few worker
threads and the calling thread. One dispatch per flush (normally one per
frame), so the workers sleep between frames instead of spinning. The pool
is created on first use and never destroyed: its threads wait for work and
end with the process. QAA_UI_THREADS (total threads, 1 = none) overrides
the count, for diagnostics. */
class Workers {
public:
	static Workers &Get() {
		static Workers *pool = new Workers();
		return *pool;
	}
	int Threads() const { return threads_ + 1; }
	/* Run fn(i) for every i in [0, count), and wait for all of them. */
	void Run( int count, const std::function<void( int )> &fn ) {
		if ( threads_ == 0 || count <= 1 ) {
			for ( int i = 0; i < count; i++ ) {
				fn( i );
			}
			return;
		}
		std::lock_guard<std::mutex> serial( run_ );	// one dispatch at a time
		{
			std::lock_guard<std::mutex> lock( mutex_ );
			job_ = &fn;
			count_ = count;
			next_.store( 0, std::memory_order_relaxed );
			busy_ = threads_;
			generation_++;
		}
		wake_.notify_all();
		Take( fn, count );
		std::unique_lock<std::mutex> lock( mutex_ );
		done_.wait( lock, [this] { return busy_ == 0; } );
		job_ = nullptr;
	}

private:
	Workers() {
		const int hw = static_cast<int>( std::thread::hardware_concurrency() );
		// leave a core for the engine's other threads (sound, the GL driver)
		int threads = std::max( 0, std::min( 11, hw - 2 ) );
		if ( const char *env = std::getenv( "QAA_UI_THREADS" ) ) {
			threads = std::max( 0, std::min( 31, std::atoi( env ) - 1 ) );
		}
		threads_ = threads;
		for ( int i = 0; i < threads; i++ ) {
			std::thread( [this] { Work(); } ).detach();
		}
	}
	void Take( const std::function<void( int )> &fn, int count ) {
		for ( ;; ) {
			const int i = next_.fetch_add( 1, std::memory_order_relaxed );
			if ( i >= count ) {
				return;
			}
			fn( i );
		}
	}
	void Work() {
		unsigned seen = 0;
		for ( ;; ) {
			const std::function<void( int )> *job;
			int count;
			{
				std::unique_lock<std::mutex> lock( mutex_ );
				wake_.wait( lock, [&] { return generation_ != seen; } );
				seen = generation_;
				job = job_;
				count = count_;
			}
			Take( *job, count );
			std::lock_guard<std::mutex> lock( mutex_ );
			if ( --busy_ == 0 ) {
				done_.notify_one();
			}
		}
	}
	int threads_ = 0;
	std::mutex run_;
	std::mutex mutex_;
	std::condition_variable wake_, done_;
	unsigned generation_ = 0;
	const std::function<void( int )> *job_ = nullptr;
	int count_ = 0;
	int busy_ = 0;
	std::atomic<int> next_{ 0 };
};


Matrix Matrix::Mul( const Matrix &a, const Matrix &b ) {
	Matrix r;
	r.m[0] = a.m[0] * b.m[0] + a.m[1] * b.m[3];
	r.m[1] = a.m[0] * b.m[1] + a.m[1] * b.m[4];
	r.m[2] = a.m[0] * b.m[2] + a.m[1] * b.m[5] + a.m[2];
	r.m[3] = a.m[3] * b.m[0] + a.m[4] * b.m[3];
	r.m[4] = a.m[3] * b.m[1] + a.m[4] * b.m[4];
	r.m[5] = a.m[3] * b.m[2] + a.m[4] * b.m[5] + a.m[5];
	return r;
}

static inline float Clamp01( float v ) {
	return v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : v;
}

/* ---- textures ------------------------------------------------------------- */

static inline void Texel( const Texture &t, int x, int y, bool clamp, float out[4] ) {
	if ( static_cast<unsigned>( x ) < static_cast<unsigned>( t.width )
		&& static_cast<unsigned>( y ) < static_cast<unsigned>( t.height ) ) {
		// inside the texture: no clamp or wrap needed
	} else if ( clamp ) {
		x = x < 0 ? 0 : x >= t.width ? t.width - 1 : x;
		y = y < 0 ? 0 : y >= t.height ? t.height - 1 : y;
	} else {
		x %= t.width;
		y %= t.height;
		if ( x < 0 ) {
			x += t.width;
		}
		if ( y < 0 ) {
			y += t.height;
		}
	}
	const uint8_t *p = &t.rgba[( static_cast<size_t>( y ) * t.width + x ) * 4];
	out[0] = p[0] * ( 1.0f / 255.0f );
	out[1] = p[1] * ( 1.0f / 255.0f );
	out[2] = p[2] * ( 1.0f / 255.0f );
	out[3] = p[3] * ( 1.0f / 255.0f );
}

void SampleTexture( const TextureFill &t, float u, float v, float out[4] ) {
	const Texture *tex = t.texture;
	if ( !tex || tex->width <= 0 || tex->height <= 0 || tex->rgba.empty() ) {
		out[0] = out[1] = out[2] = out[3] = 1.0f;
		return;
	}
	const float fx = u * tex->width - 0.5f;
	const float fy = v * tex->height - 0.5f;
	if ( !t.linear ) {
		Texel( *tex, static_cast<int>( std::floor( fx + 0.5f ) ), static_cast<int>( std::floor( fy + 0.5f ) ), t.clamp, out );
		return;
	}
	const float x0f = std::floor( fx ), y0f = std::floor( fy );
	const int x0 = static_cast<int>( x0f ), y0 = static_cast<int>( y0f );
	const float ax = fx - x0f, ay = fy - y0f;
	float c00[4], c10[4], c01[4], c11[4];
	Texel( *tex, x0, y0, t.clamp, c00 );
	Texel( *tex, x0 + 1, y0, t.clamp, c10 );
	Texel( *tex, x0, y0 + 1, t.clamp, c01 );
	Texel( *tex, x0 + 1, y0 + 1, t.clamp, c11 );
	for ( int k = 0; k < 4; k++ ) {
		const float top = c00[k] + ( c10[k] - c00[k] ) * ax;
		const float bottom = c01[k] + ( c11[k] - c01[k] ) * ax;
		out[k] = top + ( bottom - top ) * ay;
	}
}

/* ---- DXT ---------------------------------------------------------------------- */

static void Rgb565( uint16_t c, uint8_t out[3] ) {
	out[0] = static_cast<uint8_t>( ( ( c >> 11 ) & 31 ) * 255 / 31 );
	out[1] = static_cast<uint8_t>( ( ( c >> 5 ) & 63 ) * 255 / 63 );
	out[2] = static_cast<uint8_t>( ( c & 31 ) * 255 / 31 );
}

static void DecodeColorBlock( const uint8_t *b, bool dxt1, uint8_t out[16][4] ) {
	const uint16_t c0 = static_cast<uint16_t>( b[0] | ( b[1] << 8 ) );
	const uint16_t c1 = static_cast<uint16_t>( b[2] | ( b[3] << 8 ) );
	uint8_t pal[4][4];
	Rgb565( c0, pal[0] );
	Rgb565( c1, pal[1] );
	pal[0][3] = pal[1][3] = 255;
	if ( c0 > c1 || !dxt1 ) {
		for ( int k = 0; k < 3; k++ ) {
			pal[2][k] = static_cast<uint8_t>( ( 2 * pal[0][k] + pal[1][k] ) / 3 );
			pal[3][k] = static_cast<uint8_t>( ( pal[0][k] + 2 * pal[1][k] ) / 3 );
		}
		pal[2][3] = pal[3][3] = 255;
	} else {
		for ( int k = 0; k < 3; k++ ) {
			pal[2][k] = static_cast<uint8_t>( ( pal[0][k] + pal[1][k] ) / 2 );
			pal[3][k] = 0;
		}
		pal[2][3] = 255;
		pal[3][3] = 0;
	}
	const uint32_t bits = static_cast<uint32_t>( b[4] | ( b[5] << 8 ) | ( b[6] << 16 ) | ( b[7] << 24 ) );
	for ( int i = 0; i < 16; i++ ) {
		std::memcpy( out[i], pal[( bits >> ( 2 * i ) ) & 3], 4 );
	}
}

bool DecodeDXT( int format, const uint8_t *data, int width, int height, std::vector<uint8_t> &rgba ) {
	if ( width <= 0 || height <= 0 || !data ) {
		return false;
	}
	const int blockBytes = format == 10 ? 8 : 16;
	const int bw = ( width + 3 ) / 4, bh = ( height + 3 ) / 4;
	rgba.assign( static_cast<size_t>( width ) * height * 4, 0 );
	for ( int by = 0; by < bh; by++ ) {
		for ( int bx = 0; bx < bw; bx++ ) {
			const uint8_t *b = data + ( static_cast<size_t>( by ) * bw + bx ) * blockBytes;
			uint8_t px[16][4];
			if ( format == 10 ) {
				DecodeColorBlock( b, true, px );
			} else {
				DecodeColorBlock( b + 8, false, px );
				if ( format == 11 ) {	// DXT3: explicit 4-bit alpha
					for ( int i = 0; i < 16; i++ ) {
						const int nib = ( b[i / 2] >> ( 4 * ( i & 1 ) ) ) & 15;
						px[i][3] = static_cast<uint8_t>( nib * 17 );
					}
				} else {				// DXT5: interpolated alpha
					uint8_t a[8];
					a[0] = b[0];
					a[1] = b[1];
					if ( a[0] > a[1] ) {
						for ( int k = 1; k < 7; k++ ) {
							a[k + 1] = static_cast<uint8_t>( ( ( 7 - k ) * a[0] + k * a[1] ) / 7 );
						}
					} else {
						for ( int k = 1; k < 5; k++ ) {
							a[k + 1] = static_cast<uint8_t>( ( ( 5 - k ) * a[0] + k * a[1] ) / 5 );
						}
						a[6] = 0;
						a[7] = 255;
					}
					uint64_t bits = 0;
					for ( int k = 0; k < 6; k++ ) {
						bits |= static_cast<uint64_t>( b[2 + k] ) << ( 8 * k );
					}
					for ( int i = 0; i < 16; i++ ) {
						px[i][3] = a[( bits >> ( 3 * i ) ) & 7];
					}
				}
			}
			for ( int i = 0; i < 16; i++ ) {
				const int x = bx * 4 + ( i & 3 ), y = by * 4 + ( i >> 2 );
				if ( x < width && y < height ) {
					std::memcpy( &rgba[( static_cast<size_t>( y ) * width + x ) * 4], px[i], 4 );
				}
			}
		}
	}
	return true;
}

/* ---- canvas ---------------------------------------------------------------------- */

namespace {

enum { OP_TRIANGLE, OP_CLEAR_STENCIL };
/* What a triangle's pixels take (see Canvas::Raster). */
enum { SHADE_SUBMIT, SHADE_COLOR, SHADE_TEXTURE, SHADE_GOURAUD, SHADE_GOURAUD_TEX, SHADE_GOURAUD_TEX2, SHADE_GLYPH, SHADE_COUNT };
/* Canvas's mask states, in its enum's order */
enum { M_OFF, M_SUBMIT_INCR, M_SUBMIT_DECR, M_TEST };

/* Four floats, one pixel's RGBA. SSE2 where the target has it (every
 * x86-64), else plain floats; the arithmetic is the same either way, lane
 * by lane, in the same order as the scalar code it replaced. */
#ifdef QAA_UI_SSE2
struct F4 {
	__m128 v;
};
inline F4 Load4( const float *p ) { return { _mm_loadu_ps( p ) }; }
inline void Store4( F4 a, float *p ) { _mm_storeu_ps( p, a.v ); }
inline F4 Splat( float f ) { return { _mm_set1_ps( f ) }; }
inline F4 Set4( float a, float b, float c, float d ) { return { _mm_set_ps( d, c, b, a ) }; }
inline F4 operator+( F4 a, F4 b ) { return { _mm_add_ps( a.v, b.v ) }; }
inline F4 operator-( F4 a, F4 b ) { return { _mm_sub_ps( a.v, b.v ) }; }
inline F4 operator*( F4 a, F4 b ) { return { _mm_mul_ps( a.v, b.v ) }; }
inline F4 Clamp4( F4 a ) { return { _mm_min_ps( _mm_max_ps( a.v, _mm_setzero_ps() ), _mm_set1_ps( 1.0f ) ) }; }
inline float Alpha( F4 a ) { return _mm_cvtss_f32( _mm_shuffle_ps( a.v, a.v, _MM_SHUFFLE( 3, 3, 3, 3 ) ) ); }
/* four bytes as four floats, 0..255 */
inline F4 Bytes( const uint8_t *p ) {
	int32_t u;
	std::memcpy( &u, p, 4 );
	const __m128i z = _mm_setzero_si128();
	const __m128i i = _mm_unpacklo_epi16( _mm_unpacklo_epi8( _mm_cvtsi32_si128( u ), z ), z );
	return { _mm_cvtepi32_ps( i ) };
}
/* four floats in 0..255.5, truncated to bytes */
inline void PutBytes( F4 a, uint8_t *p ) {
	__m128i i = _mm_cvttps_epi32( a.v );
	i = _mm_packs_epi32( i, i );
	i = _mm_packus_epi16( i, i );
	const int32_t u = _mm_cvtsi128_si32( i );
	std::memcpy( p, &u, 4 );
}
#else
struct F4 {
	float v[4];
};
inline F4 Load4( const float *p ) { return { { p[0], p[1], p[2], p[3] } }; }
inline void Store4( F4 a, float *p ) { std::memcpy( p, a.v, sizeof( a.v ) ); }
inline F4 Splat( float f ) { return { { f, f, f, f } }; }
inline F4 Set4( float a, float b, float c, float d ) { return { { a, b, c, d } }; }
inline F4 operator+( F4 a, F4 b ) { return { { a.v[0] + b.v[0], a.v[1] + b.v[1], a.v[2] + b.v[2], a.v[3] + b.v[3] } }; }
inline F4 operator-( F4 a, F4 b ) { return { { a.v[0] - b.v[0], a.v[1] - b.v[1], a.v[2] - b.v[2], a.v[3] - b.v[3] } }; }
inline F4 operator*( F4 a, F4 b ) { return { { a.v[0] * b.v[0], a.v[1] * b.v[1], a.v[2] * b.v[2], a.v[3] * b.v[3] } }; }
inline F4 Clamp4( F4 a ) { return { { Clamp01( a.v[0] ), Clamp01( a.v[1] ), Clamp01( a.v[2] ), Clamp01( a.v[3] ) } }; }
inline float Alpha( F4 a ) { return a.v[3]; }
inline F4 Bytes( const uint8_t *p ) {
	return { { static_cast<float>( p[0] ), static_cast<float>( p[1] ), static_cast<float>( p[2] ), static_cast<float>( p[3] ) } };
}
inline void PutBytes( F4 a, uint8_t *p ) {
	for ( int k = 0; k < 4; k++ ) {
		p[k] = static_cast<uint8_t>( a.v[k] );
	}
}
#endif

inline float Edge( float ax, float ay, float bx, float by, float px, float py ) {
	return ( bx - ax ) * ( py - ay ) - ( by - ay ) * ( px - ax );
}

/* Top-left rule: a pixel exactly on a shared edge belongs to one triangle. */
inline bool TopLeft( float ax, float ay, float bx, float by ) {
	return ( ay == by && bx < ax ) || by < ay;
}

using Plane = Canvas::Plane;

/* The planes of the three barycentric weights of (v0, v1, v2), whose
 * signed area (Edge(v0, v1, v2)) is 1 / inv. Weight i is Edge of the
 * opposite edge over the area, as the per-pixel coverage test uses. */
void WeightPlanes( const float sx[3], const float sy[3], float inv, Plane w[3] ) {
	for ( int i = 0; i < 3; i++ ) {
		const int a = ( i + 1 ) % 3, b = ( i + 2 ) % 3;
		// Edge(a, b, p) = (bx - ax)(py - ay) - (by - ay)(px - ax)
		w[i].dx = -( sy[b] - sy[a] ) * inv;
		w[i].dy = ( sx[b] - sx[a] ) * inv;
		w[i].c = ( ( sy[b] - sy[a] ) * sx[a] - ( sx[b] - sx[a] ) * sy[a] ) * inv;
	}
}

Plane Interpolate( const Plane w[3], float f0, float f1, float f2 ) {
	Plane p;
	p.dx = w[0].dx * f0 + w[1].dx * f1 + w[2].dx * f2;
	p.dy = w[0].dy * f0 + w[1].dy * f1 + w[2].dy * f2;
	p.c = w[0].c * f0 + w[1].c * f1 + w[2].c * f2;
	return p;
}

/* Texture coordinate planes: the UV matrix applied to the shape-space planes. */
void UvPlanes( const Matrix &m, const Plane &x, const Plane &y, Plane *u, Plane *v ) {
	u->dx = m.m[0] * x.dx + m.m[1] * y.dx;
	u->dy = m.m[0] * x.dy + m.m[1] * y.dy;
	u->c = m.m[0] * x.c + m.m[1] * y.c + m.m[2];
	v->dx = m.m[3] * x.dx + m.m[4] * y.dx;
	v->dy = m.m[3] * x.dy + m.m[4] * y.dy;
	v->c = m.m[3] * x.c + m.m[4] * y.c + m.m[5];
}

inline int FloorInt( float f ) {
	const int i = static_cast<int>( f );
	return f < static_cast<float>( i ) ? i - 1 : i;
}

/* A texel coordinate inside [0, size): clamped, or wrapped for Wrap_Repeat. */
inline int Address( int i, int size, bool clamp ) {
	if ( static_cast<unsigned>( i ) < static_cast<unsigned>( size ) ) {
		return i;
	}
	if ( clamp ) {
		return i < 0 ? 0 : size - 1;
	}
	i %= size;
	return i < 0 ? i + size : i;
}

/* A texture as the inner loops read it: SampleTexture's results, in
 * integer texel addressing, from locals (a pixel store may alias anything
 * reached through a pointer, which would reload it per pixel). */
struct Sampler {
	const uint8_t *base = nullptr;
	int w = 0, h = 0;
	float fw = 0, fh = 0;
	bool clamp = false, linear = true, valid = false;

	explicit Sampler( const Canvas::TexRef &t ) : clamp( t.clamp ), linear( t.linear ) {
		const Texture *tex = t.texture;
		if ( tex && tex->width > 0 && tex->height > 0 && !tex->rgba.empty() ) {
			base = tex->rgba.data();
			w = tex->width;
			h = tex->height;
			fw = static_cast<float>( w );
			fh = static_cast<float>( h );
			valid = true;
		}
	}
	/* the texel point sampling takes: SampleTexture's floor(u * w - 0.5 + 0.5) */
	const uint8_t *Point( float u, float v ) const {
		const int x = Address( FloorInt( u * fw ), w, clamp );
		const int y = Address( FloorInt( v * fh ), h, clamp );
		return base + ( static_cast<size_t>( y ) * w + x ) * 4;
	}
	F4 Sample( float u, float v ) const {
		if ( !valid ) {
			return Splat( 1.0f );
		}
		if ( !linear ) {
			return Bytes( Point( u, v ) ) * Splat( 1.0f / 255.0f );
		}
		const float fx = u * fw - 0.5f, fy = v * fh - 0.5f;
		const int ix = FloorInt( fx ), iy = FloorInt( fy );
		const float ax = fx - ix, ay = fy - iy;
		int xa, xb, ya, yb;
		if ( static_cast<unsigned>( ix ) < static_cast<unsigned>( w - 1 ) && static_cast<unsigned>( iy ) < static_cast<unsigned>( h - 1 ) ) {
			xa = ix;
			xb = ix + 1;
			ya = iy;
			yb = iy + 1;
		} else {
			xa = Address( ix, w, clamp );
			xb = Address( ix + 1, w, clamp );
			ya = Address( iy, h, clamp );
			yb = Address( iy + 1, h, clamp );
		}
		const uint8_t *rowA = base + static_cast<size_t>( ya ) * w * 4, *rowB = base + static_cast<size_t>( yb ) * w * 4;
		const float w11 = ax * ay, w10 = ax - w11, w01 = ay - w11, w00 = 1.0f - ax - ay + w11;
		return ( Bytes( rowA + xa * 4 ) * Splat( w00 ) + Bytes( rowA + xb * 4 ) * Splat( w10 )
			+ Bytes( rowB + xa * 4 ) * Splat( w01 ) + Bytes( rowB + xb * 4 ) * Splat( w11 ) ) * Splat( 1.0f / 255.0f );
	}
};

/* Blend a premultiplied pixel by a mode other than normal (the retail
 * shaders' blend states), straight into p. */
void BlendSpecial( uint8_t *p, const float s[4], int mode ) {
	float d[4] = { p[0] * ( 1.0f / 255.0f ), p[1] * ( 1.0f / 255.0f ), p[2] * ( 1.0f / 255.0f ), p[3] * ( 1.0f / 255.0f ) };
	float o[4];
	switch ( mode ) {
	case BLEND_ADD:
		for ( int k = 0; k < 4; k++ ) {
			o[k] = d[k] + s[k];
		}
		o[3] = s[3] + d[3] * ( 1.0f - s[3] );
		break;
	case BLEND_SUBTRACT:
		for ( int k = 0; k < 3; k++ ) {
			o[k] = d[k] - s[k];
		}
		o[3] = d[3];
		break;
	case BLEND_MULTIPLY:
	case BLEND_DARKEN:
	case BLEND_LIGHTEN:
	case BLEND_SCREEN: {
		// the retail shaders' lerp(1, colour, alpha): the source mixed
		// toward white by its coverage, in straight colour
		float c[3];
		for ( int k = 0; k < 3; k++ ) {
			const float straight = s[3] > 0.0f ? s[k] / s[3] : 0.0f;
			c[k] = 1.0f + ( straight - 1.0f ) * s[3];
		}
		for ( int k = 0; k < 3; k++ ) {
			if ( mode == BLEND_MULTIPLY ) {
				o[k] = d[k] * c[k];
			} else if ( mode == BLEND_DARKEN ) {
				o[k] = std::min( d[k], c[k] );
			} else if ( mode == BLEND_LIGHTEN ) {
				o[k] = std::max( d[k], s[k] + d[k] * ( 1.0f - s[3] ) );
			} else {
				o[k] = s[k] + d[k] * ( 1.0f - s[k] );
			}
		}
		o[3] = d[3] + s[3] * ( 1.0f - d[3] );
		break;
	}
	default:
		for ( int k = 0; k < 4; k++ ) {
			o[k] = s[k] + d[k] * ( 1.0f - s[3] );
		}
		break;
	}
	for ( int k = 0; k < 4; k++ ) {
		p[k] = static_cast<uint8_t>( Clamp01( o[k] ) * 255.0f + 0.5f );
	}
}

/* Blend modes the retail shaders treat as normal (GFx draws most shapes as
 * "none" or "layer"). */
inline bool NormalBlend( int mode ) {
	return mode != BLEND_ADD && mode != BLEND_SUBTRACT && mode != BLEND_MULTIPLY && mode != BLEND_DARKEN
		&& mode != BLEND_LIGHTEN && mode != BLEND_SCREEN;
}

/*
 * One queued triangle over rows [ya, yb). Each row is walked only over the
 * span the triangle can cover (from each edge's crossing, widened by a
 * pixel); pixels a pixel inside every edge skip the coverage test, the rest
 * take the exact test with the top-left rule. Values are planes stepped
 * along the row. SHADE picks the fill; NORMAL blends inline.
 *
 * Everything the loops read is copied to locals first: the pixel stores
 * are bytes, which may alias anything, so values read through op would be
 * loaded again for every pixel.
 *
 * A pixel whose new bytes do not depend on what was under it (an opaque
 * write, or a near-opaque one that comes out the same over black and over
 * white: the blend is monotonic in the pixel) is sealed: its seal becomes
 * sealPos, the triangle's place in its band's list plus one (0: none).
 */
template <int SHADE, bool NORMAL>
void RasterRows( const Canvas::Op &op, uint8_t *pixels, uint8_t *stencil, uint16_t *seal, uint16_t sealPos, int width,
	int ya, int yb, int64_t &testedOut ) {
	constexpr bool colour = SHADE == SHADE_GLYPH || SHADE == SHADE_GOURAUD || SHADE == SHADE_GOURAUD_TEX;
	constexpr bool gouraud = SHADE == SHADE_GOURAUD || SHADE == SHADE_GOURAUD_TEX || SHADE == SHADE_GOURAUD_TEX2;
	constexpr bool tex0 = SHADE == SHADE_TEXTURE || SHADE == SHADE_GOURAUD_TEX || SHADE == SHADE_GOURAUD_TEX2 || SHADE == SHADE_GLYPH;
	constexpr bool tex1 = SHADE == SHADE_GOURAUD_TEX2;
	const float sx[3] = { op.sx[0], op.sx[1], op.sx[2] }, sy[3] = { op.sy[0], op.sy[1], op.sy[2] };
	const bool tl[3] = { op.tl[0], op.tl[1], op.tl[2] };
	const int opX0 = op.x0, opX1 = op.x1;
	static const int ea[3] = { 1, 2, 0 }, eb[3] = { 2, 0, 1 };
	const int mask = op.mask, mode = op.mode;
	const bool stencilTest = mask == M_TEST;
	const uint8_t level = op.level;
	const F4 mul = Load4( op.cx.mul ), add = Load4( op.cx.add );
	const bool identity = op.cx.mul[0] == 1.0f && op.cx.mul[1] == 1.0f && op.cx.mul[2] == 1.0f && op.cx.mul[3] == 1.0f
		&& op.cx.add[0] == 0.0f && op.cx.add[1] == 0.0f && op.cx.add[2] == 0.0f && op.cx.add[3] == 0.0f;
	const F4 fillColour = Load4( op.color );
	const Plane col[4] = { op.col[0], op.col[1], op.col[2], op.col[3] };
	const F4 colDx = Set4( col[0].dx, col[1].dx, col[2].dx, col[3].dx );
	const Plane facLerp = op.facLerp, facAlpha = op.facAlpha, pu0 = op.u0, pv0 = op.v0, pu1 = op.u1, pv1 = op.v1;
	const Sampler s0( op.tex0 ), s1( op.tex1 );

	// A solid colour blended normally is the same for every pixel: nothing
	// when transparent, and when opaque the same bytes whatever is under it
	// (checked over black and white: the blend is monotonic in the pixel).
	bool solid = false;
	uint8_t solidPx[4] = {};
	if constexpr ( SHADE == SHADE_COLOR && NORMAL ) {
		F4 out = Clamp4( fillColour * mul + add );
		const float alpha = Alpha( out );
		if ( alpha <= 0.0f ) {
			return;
		}
		out = out * Set4( alpha, alpha, alpha, 1.0f );
		const float keep = ( 1.0f - alpha ) * ( 1.0f / 255.0f );
		uint8_t over[2][4];
		for ( int k = 0; k < 2; k++ ) {
			const uint8_t v = static_cast<uint8_t>( k * 255 );
			const uint8_t under[4] = { v, v, v, v };
			PutBytes( Clamp4( out + Bytes( under ) * Splat( keep ) ) * Splat( 255.0f ) + Splat( 0.5f ), over[k] );
		}
		if ( !std::memcmp( over[0], over[1], 4 ) ) {
			solid = true;
			std::memcpy( solidPx, over[0], 4 );
		}
	}
	// Point-sampled texels with no colour transform: an opaque texel is its
	// own bytes (the general path gives the same), a transparent one nothing.
	const bool copyTexels = SHADE == SHADE_TEXTURE && NORMAL && identity && s0.valid && !s0.linear;

	int64_t tested = 0;
	for ( int y = ya; y < yb; y++ ) {
		const float py = y + 0.5f;
		// Where each edge crosses the row: px <= cross (dy > 0) or px >= cross
		// (dy < 0). [xa, xb) widens that by a pixel and [inA, inB) narrows it
		// by one: pixels in [inA, inB) are inside every edge and skip the test.
		float lo = static_cast<float>( opX0 ), hi = static_cast<float>( opX1 );
		bool interior = true;
		for ( int e = 0; e < 3; e++ ) {
			const int a = ea[e], b = eb[e];
			// Edge(a, b, px, py) = (b.x - a.x)(py - a.y) - (b.y - a.y)(px - a.x) >= 0
			const float dy = sy[b] - sy[a];
			const float k = ( sx[b] - sx[a] ) * ( py - sy[a] );
			if ( dy == 0 ) {
				interior = interior && k > 0;	// the same sign along the whole row
				continue;
			}
			const float cross = sx[a] + k / dy;
			if ( !( std::fabs( cross ) < 1.0e6f ) ) {
				interior = false;	// too far out to trust to a pixel
				continue;
			}
			if ( dy > 0 ) {
				hi = std::min( hi, cross );
			} else {
				lo = std::max( lo, cross );
			}
		}
		const int xa = std::max( opX0, static_cast<int>( std::floor( lo - 1.0f ) ) );
		const int xb = std::min( opX1, static_cast<int>( std::ceil( hi + 1.0f ) ) + 1 );
		if ( xa >= xb ) {
			continue;
		}
		const int inA = interior ? std::max( xa, static_cast<int>( std::ceil( lo + 1.0f ) ) ) : xb;
		const int inB = interior ? std::min( xb, static_cast<int>( std::floor( hi - 1.0f ) ) ) : xb;
		tested += xb - xa;
		// the planes at the row's first pixel, stepped by dx per pixel
		const float pxa = xa + 0.5f;
		F4 c = Splat( 0.0f );
		float fl = 0, fa = 0, uu = 0, vv = 0, uu1 = 0, vv1 = 0;
		if constexpr ( colour ) {
			c = Set4( col[0].At( pxa, py ), col[1].At( pxa, py ), col[2].At( pxa, py ), col[3].At( pxa, py ) );
		}
		if constexpr ( gouraud ) {
			fl = facLerp.At( pxa, py );
			fa = facAlpha.At( pxa, py );
		}
		if constexpr ( tex0 ) {
			uu = pu0.At( pxa, py );
			vv = pv0.At( pxa, py );
		}
		if constexpr ( tex1 ) {
			uu1 = pu1.At( pxa, py );
			vv1 = pv1.At( pxa, py );
		}
		uint8_t *row = pixels + static_cast<size_t>( y ) * width * 4;
		uint8_t *srow = stencil + static_cast<size_t>( y ) * width;
		uint16_t *sealRow = seal + static_cast<size_t>( y ) * width;
		for ( int x = xa; x < xb; x++ ) {
			if ( x != xa ) {
				if constexpr ( colour ) {
					c = c + colDx;
				}
				if constexpr ( gouraud ) {
					fl += facLerp.dx;
					fa += facAlpha.dx;
				}
				if constexpr ( tex0 ) {
					uu += pu0.dx;
					vv += pv0.dx;
				}
				if constexpr ( tex1 ) {
					uu1 += pu1.dx;
					vv1 += pv1.dx;
				}
			}
			if ( x < inA || x >= inB ) {
				const float px = x + 0.5f;
				const float e0 = Edge( sx[1], sy[1], sx[2], sy[2], px, py );
				const float e1 = Edge( sx[2], sy[2], sx[0], sy[0], px, py );
				const float e2 = Edge( sx[0], sy[0], sx[1], sy[1], px, py );
				if ( e0 < 0 || e1 < 0 || e2 < 0 ) {
					continue;
				}
				if ( ( e0 == 0 && !tl[0] ) || ( e1 == 0 && !tl[1] ) || ( e2 == 0 && !tl[2] ) ) {
					continue;
				}
			}
			if constexpr ( SHADE == SHADE_SUBMIT ) {
				uint8_t &s = srow[x];
				if ( mask == M_SUBMIT_INCR ) {
					if ( s == level ) {
						s = static_cast<uint8_t>( level + 1 );
					}
				} else if ( s == level && level > 0 ) {
					s = static_cast<uint8_t>( level - 1 );
				}
				continue;
			} else {
				if ( stencilTest && srow[x] != level ) {
					continue;
				}
				uint8_t *p = row + x * 4;
				if constexpr ( SHADE == SHADE_COLOR && NORMAL ) {
					if ( solid ) {
						std::memcpy( p, solidPx, 4 );
						sealRow[x] = sealPos;
						continue;
					}
				}
				if constexpr ( SHADE == SHADE_TEXTURE && NORMAL ) {
					if ( copyTexels ) {
						const uint8_t *t = s0.Point( uu, vv );
						if ( t[3] == 255 ) {
							std::memcpy( p, t, 4 );
							sealRow[x] = sealPos;
							continue;
						}
						if ( t[3] == 0 ) {
							continue;
						}
					}
				}
				F4 out;
				if constexpr ( SHADE == SHADE_GLYPH ) {
					// DrawBitmaps (glyphs): the quad's colour, alpha from the texture
					out = c * Set4( 1.0f, 1.0f, 1.0f, Alpha( s0.Sample( uu, vv ) ) );
				} else if constexpr ( SHADE == SHADE_COLOR ) {
					out = fillColour;
				} else if constexpr ( SHADE == SHADE_TEXTURE ) {
					out = s0.Sample( uu, vv );
				} else if constexpr ( SHADE == SHADE_GOURAUD ) {
					out = c;
				} else {
					const F4 t0 = s0.Sample( uu, vv );
					F4 other;
					if constexpr ( SHADE == SHADE_GOURAUD_TEX2 ) {
						other = s1.Sample( uu1, vv1 );
					} else {
						other = c;
					}
					out = other + ( t0 - other ) * Splat( fl );
				}
				out = Clamp4( out * mul + add );
				if constexpr ( gouraud ) {
					out = out * Set4( 1.0f, 1.0f, 1.0f, Clamp01( fa ) );	// edge anti-aliasing
				}
				const float alpha = Alpha( out );
				if ( alpha <= 0.0f ) {
					continue;
				}
				out = out * Set4( alpha, alpha, alpha, 1.0f );
				if constexpr ( NORMAL ) {
					const float keep = ( 1.0f - alpha ) * ( 1.0f / 255.0f );
					if ( keep == 0.0f ) {
						sealRow[x] = sealPos;
					} else if ( alpha > 0.996f ) {
						// within a step of opaque: the same over black and white?
						uint8_t over[2][4];
						PutBytes( Clamp4( out ) * Splat( 255.0f ) + Splat( 0.5f ), over[0] );
						PutBytes( Clamp4( out + Splat( 255.0f ) * Splat( keep ) ) * Splat( 255.0f ) + Splat( 0.5f ), over[1] );
						if ( !std::memcmp( over[0], over[1], 4 ) ) {
							sealRow[x] = sealPos;
						}
					}
					PutBytes( Clamp4( out + Bytes( p ) * Splat( keep ) ) * Splat( 255.0f ) + Splat( 0.5f ), p );
				} else {
					float o[4];
					Store4( out, o );
					BlendSpecial( p, o, mode );
				}
			}
		}
	}
	testedOut += tested;
}

using RasterFn = void ( * )( const Canvas::Op &, uint8_t *, uint8_t *, uint16_t *, uint16_t, int, int, int, int64_t & );

template <int SHADE>
constexpr RasterFn Pick( bool normal ) {
	return normal ? &RasterRows<SHADE, true> : &RasterRows<SHADE, false>;
}

RasterFn RasterFor( int shade, bool normal ) {
	switch ( shade ) {
	case SHADE_SUBMIT: return Pick<SHADE_SUBMIT>( normal );
	case SHADE_COLOR: return Pick<SHADE_COLOR>( normal );
	case SHADE_TEXTURE: return Pick<SHADE_TEXTURE>( normal );
	case SHADE_GOURAUD: return Pick<SHADE_GOURAUD>( normal );
	case SHADE_GOURAUD_TEX: return Pick<SHADE_GOURAUD_TEX>( normal );
	case SHADE_GOURAUD_TEX2: return Pick<SHADE_GOURAUD_TEX2>( normal );
	default: return Pick<SHADE_GLYPH>( normal );
	}
}

/* A queued triangle's bytes, hashed: every field is set the same way for
 * the same triangle (NewOp zeroes the padding, Raster leaves the fields its
 * fill does not use at zero), so equal hashes mean the same pixels. */
uint64_t HashOp( const Canvas::Op &op ) {
	static_assert( sizeof( Canvas::Op ) % 8 == 0, "an Op hashes in 8-byte words" );
	const unsigned char *p = reinterpret_cast<const unsigned char *>( &op );
	uint64_t h = 0x243F6A8885A308D3ull;
	for ( size_t i = 0; i < sizeof( Canvas::Op ); i += 8 ) {
		uint64_t v;
		std::memcpy( &v, p + i, 8 );
		h = ( h ^ v ) * 0x9E3779B97F4A7C15ull;
		h ^= h >> 32;
	}
	return h;
}

/* Whether every value in [x0, x1) x [y0, y1) of a map (seals, stride values
 * a row) is above pos. */
bool AllAbove( const uint16_t *map, int stride, int x0, int y0, int x1, int y1, size_t pos ) {
	for ( int y = y0; y < y1; y++ ) {
		const uint16_t *row = map + static_cast<size_t>( y ) * stride;
		uint16_t least = 0xFFFF;
		for ( int x = x0; x < x1; x++ ) {
			least = std::min( least, row[x] );
		}
		if ( least <= pos ) {
			return false;
		}
	}
	return true;
}

/* Premultiplied to straight colour: min(255, c * 255 / a), by alpha then
 * colour. */
const uint8_t *Unpremultiply() {
	static const std::vector<uint8_t> table = [] {
		std::vector<uint8_t> t( 256 * 256 );
		for ( int a = 1; a < 256; a++ ) {
			for ( int c = 0; c < 256; c++ ) {
				t[a * 256 + c] = static_cast<uint8_t>( std::min( 255, c * 255 / a ) );
			}
		}
		return t;
	}();
	return table.data();
}

}	// namespace

void Canvas::Resize( int width, int height ) {
	if ( width == width_ && height == height_ ) {
		return;
	}
	width_ = std::max( width, 1 );
	height_ = std::max( height, 1 );
	pixels_.assign( static_cast<size_t>( width_ ) * height_ * 4, 0 );
	stencil_.assign( static_cast<size_t>( width_ ) * height_, 0 );
	seal_.assign( static_cast<size_t>( width_ ) * height_, 0 );
	rowBlank_.assign( static_cast<size_t>( height_ ), 1 );
	straightBlank_.assign( static_cast<size_t>( height_ ), 0 );
	lastStraight_ = nullptr;
	lastStraightSize_ = 0;
	ops_.clear();
	clearPending_ = false;
	const int bands = ( height_ + BAND_ROWS - 1 ) / BAND_ROWS;
	bandHash_.assign( static_cast<size_t>( bands ), 0 );
	bandStale_.assign( static_cast<size_t>( bands ), 1 );
	bandLast_.assign( static_cast<size_t>( bands ), {} );
	checkpoints_.assign( static_cast<size_t>( bands ), {} );
	bandsValid_ = false;	// the old frame is gone
	clip_[0] = clip_[1] = 0;
	clip_[2] = width_;
	clip_[3] = height_;
}

void Canvas::Clear() {
	ops_.clear();
	verifyOps_.clear();
	clearPending_ = true;
	blend_.clear();
	stencilLevel_ = 0;
	maskState_ = MASK_OFF;
	ClearMaskBounds();
	matrix_ = Matrix();
	user_ = Matrix();
	cx_ = Cxform();
	triangles_ = 0;
	pixelsTested_ = 0;
	rowsDrawn_ = 0;
	rowsKept_ = 0;
}

void Canvas::ClearMaskBounds() {
	maskBoundsTop_ = 0;
	maskBoundsKnown_ = true;
}

void Canvas::ToStraight( std::vector<uint8_t> &out ) const {
	Flush( &out );
}

/* Whether a band whose triangles changed since it was last drawn would
 * come out the same: its list is as long as then, and every triangle that
 * differs, then and now, writes pixels only and lies wholly over pixels
 * sealed by a later triangle (one that is the same). Those pixels depend
 * only on the later triangles, and the rest on the same triangles as then,
 * over the same stencil. */
bool Canvas::Sealed( int band ) const {
	const std::vector<uint32_t> &list = bandOps_[band];
	const std::vector<BandOp> &last = bandLast_[band];
	if ( list.size() != last.size() || list.size() >= 0xFFFF ) {
		return false;
	}
	const int ya = band * BAND_ROWS, yb = std::min( height_, ya + BAND_ROWS );
	// whether every pixel of a rectangle is sealed after position pos
	auto covered = [&]( int x0, int y0, int x1, int y1, size_t pos ) {
		return AllAbove( seal_.data(), width_, x0, std::max( y0, ya ), x1, std::min( y1, yb ), pos );
	};
	// Changed triangles come in runs (a shape whose colour transform
	// changed): each run is checked as one rectangle, after its last
	// position, and only a run that fails that triangle by triangle.
	for ( size_t k = 0; k < list.size(); ) {
		if ( opHash_[list[k]] == last[k].hash ) {
			k++;
			continue;
		}
		size_t end = k;
		int x0 = INT32_MAX, y0 = INT32_MAX, x1 = INT32_MIN, y1 = INT32_MIN;
		for ( ; end < list.size() && opHash_[list[end]] != last[end].hash; end++ ) {
			const Op &op = ops_[list[end]];
			const BandOp &then = last[end];
			if ( !then.pixels || op.type != OP_TRIANGLE || op.shade == SHADE_SUBMIT ) {
				return false;
			}
			x0 = std::min( { x0, op.wx0, static_cast<int>( then.x0 ) } );
			y0 = std::min( { y0, op.y0, static_cast<int>( then.y0 ) } );
			x1 = std::max( { x1, op.wx1, static_cast<int>( then.x1 ) } );
			y1 = std::max( { y1, op.y1, static_cast<int>( then.y1 ) } );
		}
		if ( !covered( x0, y0, x1, y1, end ) ) {
			for ( size_t j = k; j < end; j++ ) {
				const Op &op = ops_[list[j]];
				const BandOp &then = last[j];
				if ( !covered( op.wx0, op.y0, op.wx1, op.y1, j + 1 ) || !covered( then.x0, then.y0, then.x1, then.y1, j + 1 ) ) {
					return false;
				}
			}
		}
		k = end;
	}
	return true;
}

/* One band, by action: BAND_RASTER, the pending clear and the queued
 * triangles that reach it, in order; BAND_CHANGED, the same unless Sealed()
 * shows they would come out as the band is, leaving out the triangles that
 * would be drawn over and resuming from the band's checkpoint where it
 * still holds. Then, when it changed since the straight copy was last made
 * (or the copy goes to another buffer), its straight copy. Rows no triangle
 * touches and that are already transparent are left alone, in the pixels
 * and, when the copy goes to the same buffer as last time, in the copy. */
void Canvas::RunBand( int band, int action, bool clear, std::vector<uint8_t> *straight, bool sameStraight,
	BandResult *out ) const {
	const int ya = band * BAND_ROWS, yb = std::min( height_, ya + BAND_ROWS );
	const size_t rowBytes = static_cast<size_t>( width_ ) * 4;
	const std::vector<uint32_t> &list = bandOps_[band];
	// what the band now holds, for the next frame's Sealed()
	auto remember = [&] {
		std::vector<BandOp> &last = bandLast_[band];
		last.resize( list.size() );
		for ( size_t k = 0; k < list.size(); k++ ) {
			const Op &op = ops_[list[k]];
			last[k] = { opHash_[list[k]], static_cast<int16_t>( op.wx0 ), static_cast<int16_t>( op.y0 ),
				static_cast<int16_t>( op.wx1 ), static_cast<int16_t>( op.y1 ), op.type == OP_TRIANGLE && op.shade != SHADE_SUBMIT };
		}
	};
	if ( action == BAND_CHANGED && Sealed( band ) ) {
		remember();
		out->kept = yb - ya;
		action = BAND_KEEP;
	}
	if ( action != BAND_KEEP ) {
		const size_t bandPixels = static_cast<size_t>( yb - ya ) * width_;
		uint8_t *bandRgba = pixels_.data() + static_cast<size_t>( ya ) * rowBytes;
		uint8_t *bandStencil = stencil_.data() + static_cast<size_t>( ya ) * width_;
		uint16_t *bandSeal = seal_.data() + static_cast<size_t>( ya ) * width_;
		// A band that changed is drawn without the triangles whose pixels
		// were all sealed last time by a later triangle that is the same
		// now: it writes them again whatever is under it. Only while the
		// stencil goes as then (every triangle that changed writes pixels
		// only), so the sealing triangles pass the same mask tests.
		thread_local std::vector<uint16_t> hidden;	// per pixel of the band: such a seal, or 0
		thread_local std::vector<uint8_t> differs, culled;
		culled.assign( list.size(), 0 );
		bool cull = false;
		size_t firstChange = list.size();	// the first triangle that changed and is drawn
		if ( action == BAND_CHANGED ) {
			const std::vector<BandOp> &last = bandLast_[band];
			cull = list.size() == last.size() && list.size() < 0xFFFF;
			differs.assign( list.size(), 0 );
			for ( size_t k = 0; cull && k < list.size(); k++ ) {
				if ( opHash_[list[k]] != last[k].hash ) {
					const Op &op = ops_[list[k]];
					differs[k] = 1;
					cull = last[k].pixels && op.type == OP_TRIANGLE && op.shade != SHADE_SUBMIT;
				}
			}
			if ( cull ) {
				hidden.resize( bandPixels );
				for ( size_t i = 0; i < bandPixels; i++ ) {
					const uint16_t s = bandSeal[i];
					hidden[i] = s && !differs[s - 1] ? s : 0;
				}
				const uint16_t *map = hidden.data() - static_cast<size_t>( ya ) * width_;
				for ( size_t k = 0; k < list.size(); k++ ) {
					const Op &op = ops_[list[k]];
					if ( op.type == OP_TRIANGLE && op.shade != SHADE_SUBMIT
						&& AllAbove( map, width_, op.wx0, std::max( ya, op.y0 ), op.wx1, std::min( yb, op.y1 ), k + 1 ) ) {
						culled[k] = 1;
					} else if ( differs[k] && firstChange == list.size() ) {
						firstChange = k;
					}
				}
			}
		}
		// The checkpoint: the band as drawn up to a position, kept from a
		// frame that drew it. It stands for this frame when every triangle
		// before it is left out as then, or is the same as then and drawn:
		// the same triangles drawn over the same start.
		Checkpoint &cp = checkpoints_[band];
		size_t start = 0;
		if ( !cull ) {
			cp.valid = false;
		} else if ( cp.valid && cp.pos <= firstChange && cp.pos <= list.size() ) {
			bool same = true;
			for ( size_t j = 0; same && j < cp.pos; j++ ) {
				same = culled[j] == cp.culled[j] && ( culled[j] || opHash_[list[j]] == cp.hash[j] );
			}
			if ( same ) {
				std::memcpy( bandRgba, cp.rgba.data(), bandPixels * 4 );
				std::memcpy( bandStencil, cp.stencil.data(), bandPixels );
				std::memcpy( bandSeal, cp.seal.data(), bandPixels * sizeof( uint16_t ) );
				start = cp.pos;
			}
		}
		// the rows the triangles cover
		uint8_t touched[BAND_ROWS] = {};
		for ( uint32_t i : list ) {
			const Op &op = ops_[i];
			if ( op.type == OP_TRIANGLE ) {
				const int r0 = std::max( ya, op.y0 ), r1 = std::min( yb, op.y1 );
				std::memset( touched + ( r0 - ya ), 1, static_cast<size_t>( r1 - r0 ) );
			}
		}
		for ( int y = ya; y < yb; y++ ) {
			const bool used = touched[y - ya] != 0;
			if ( clear ) {
				if ( !start && ( used || !rowBlank_[y] ) ) {
					std::memset( pixels_.data() + y * rowBytes, 0, rowBytes );
				}
				rowBlank_[y] = !used;
			} else if ( used ) {
				rowBlank_[y] = 0;
			}
		}
		// The stencil is cleared at a frame's start and by a mask's first
		// submit; it is only written when a triangle reads it.
		bool stencilZero = clear;
		if ( start ) {
			stencilZero = cp.stencilZero;
		} else {
			std::memset( bandSeal, 0, bandPixels * sizeof( uint16_t ) );
		}
		// a new checkpoint before the first change, unless resumed from there
		const size_t takeAt = cull && firstChange != start ? firstChange : list.size();
		for ( size_t k = start; k < list.size(); k++ ) {
			if ( k == takeAt ) {
				cp.valid = true;
				cp.pos = k;
				cp.stencilZero = stencilZero;
				cp.hash.resize( k );
				for ( size_t j = 0; j < k; j++ ) {
					cp.hash[j] = opHash_[list[j]];
				}
				cp.culled.assign( culled.begin(), culled.begin() + k );
				cp.rgba.assign( bandRgba, bandRgba + bandPixels * 4 );
				cp.stencil.assign( bandStencil, bandStencil + bandPixels );
				cp.seal.assign( bandSeal, bandSeal + bandPixels );
			}
			const Op &op = ops_[list[k]];
			if ( op.type == OP_CLEAR_STENCIL ) {
				stencilZero = true;
				continue;
			}
			if ( culled[k] ) {
				continue;
			}
			if ( op.mask != M_OFF && stencilZero ) {
				std::memset( bandStencil, 0, bandPixels );
				stencilZero = false;
			}
			const RasterFn fn = RasterFor( op.shade, NormalBlend( op.mode ) );
			const uint16_t sealPos = k + 1 < 0xFFFF ? static_cast<uint16_t>( k + 1 ) : 0;
			fn( op, pixels_.data(), stencil_.data(), seal_.data(), sealPos, width_, std::max( ya, op.y0 ), std::min( yb, op.y1 ),
				out->tested );
		}
		if ( clear ) {
			remember();
		}
		out->rows = yb - ya;
		bandStale_[band] = 1;
	}
	if ( !straight || ( sameStraight && !bandStale_[band] ) ) {
		return;
	}
	bandStale_[band] = 0;
	const uint8_t *unpremultiply = Unpremultiply();
	auto mark = [out]( int y ) {
		if ( !out->changed.empty() && out->changed.back() == y ) {
			out->changed.back() = y + 1;
		} else {
			out->changed.push_back( y );
			out->changed.push_back( y + 1 );
		}
	};
	for ( int y = ya; y < yb; y++ ) {
		uint8_t *o = straight->data() + y * rowBytes;
		if ( rowBlank_[y] ) {
			if ( sameStraight && straightBlank_[y] ) {
				continue;	// transparent before and now
			}
			std::memset( o, 0, rowBytes );
			straightBlank_[y] = 1;
		} else {
			const uint8_t *p = pixels_.data() + y * rowBytes;
			for ( int x = 0; x < width_; x++, p += 4, o += 4 ) {
				const int a = p[3];
				if ( a == 0 ) {
					std::memset( o, 0, 4 );
				} else if ( a == 255 ) {
					std::memcpy( o, p, 4 );
				} else {
					const uint8_t *t = unpremultiply + a * 256;
					o[0] = t[p[0]];
					o[1] = t[p[1]];
					o[2] = t[p[2]];
					o[3] = static_cast<uint8_t>( a );
				}
			}
			straightBlank_[y] = 0;
		}
		mark( y );
	}
}

void Canvas::Flush( std::vector<uint8_t> *straight ) const {
	const bool sameStraight = straight && straight->data() == lastStraight_ && straight->size() == pixels_.size()
		&& lastStraightSize_ == pixels_.size();
	if ( straight ) {
		straight->resize( pixels_.size() );
	}
	const bool clear = clearPending_;
	if ( !clear && ops_.empty() && !straight ) {
		return;
	}
	// a frame's first flush, for SetVerify: the queue as it was, with the
	// triangles left out for their masks
	std::vector<Op> verifyOps;
	if ( verify_ && clear ) {
		verifyOps.swap( verifyOps_ );
	}
	verifyOps_.clear();
	const int bands = ( height_ + BAND_ROWS - 1 ) / BAND_ROWS;
	bandOps_.resize( static_cast<size_t>( bands ) );
	for ( std::vector<uint32_t> &list : bandOps_ ) {
		list.clear();
	}
	for ( size_t i = 0; i < ops_.size(); i++ ) {
		const Op &op = ops_[i];
		for ( int b = op.y0 / BAND_ROWS; b <= ( op.y1 - 1 ) / BAND_ROWS; b++ ) {
			bandOps_[b].push_back( static_cast<uint32_t>( i ) );
		}
	}
	// Which bands to rasterise. A frame flushed in one go after Clear()
	// leaves alone the bands whose triangles hash the same as the last
	// frame's; a flush that continues a frame invalidates every band, as
	// their hashes then describe only part of what they hold.
	// Bands whose hash changed are rasterised unless Sealed() shows the
	// change is hidden.
	std::vector<uint8_t> raster( static_cast<size_t>( bands ), BAND_KEEP );
	if ( clear ) {
		const bool reuse = bandsValid_;
		// on the workers: every op, in chunks, then every band's list
		opHash_.resize( ops_.size() );
		std::vector<uint64_t> hashes( static_cast<size_t>( bands ) );
		const int chunks = static_cast<int>( std::min<size_t>( 64, ( ops_.size() + 1023 ) / 1024 ) );
		auto hashOps = [&]( int c ) {
			for ( size_t i = ops_.size() * c / chunks; i < ops_.size() * ( c + 1 ) / chunks; i++ ) {
				opHash_[i] = HashOp( ops_[i] );
			}
		};
		auto hashBand = [&]( int b ) {
			uint64_t h = 0x13198A2E03707344ull;
			for ( uint32_t i : bandOps_[b] ) {
				h = ( h ^ opHash_[i] ) * 0x9E3779B97F4A7C15ull;
				h ^= h >> 29;
			}
			hashes[b] = h;
		};
		Workers &pool = Workers::Get();
		if ( pool.Threads() > 1 && chunks > 1 ) {
			pool.Run( chunks, hashOps );
			pool.Run( bands, hashBand );
		} else {
			for ( int c = 0; c < chunks; c++ ) {
				hashOps( c );
			}
			for ( int b = 0; b < bands; b++ ) {
				hashBand( b );
			}
		}
		for ( int b = 0; b < bands; b++ ) {
			raster[b] = static_cast<uint8_t>( !reuse ? BAND_RASTER : hashes[b] != bandHash_[b] ? BAND_CHANGED : BAND_KEEP );
			bandHash_[b] = hashes[b];
		}
		bandsValid_ = true;
	} else if ( !ops_.empty() ) {
		for ( int b = 0; b < bands; b++ ) {
			raster[b] = static_cast<uint8_t>( bandOps_[b].empty() ? BAND_KEEP : BAND_RASTER );
		}
		bandsValid_ = false;
	}
	std::vector<int> work;
	for ( int b = 0; b < bands; b++ ) {
		if ( raster[b] || ( straight && ( !sameStraight || bandStale_[b] ) ) ) {
			work.push_back( b );
		}
	}
	std::vector<BandResult> results( work.size() );
	Workers &workers = Workers::Get();
	auto run = [&]( int i ) {
		RunBand( work[i], raster[work[i]], clear, straight, sameStraight, &results[i] );
	};
	if ( workers.Threads() <= 1 || work.size() <= 1 || static_cast<int64_t>( width_ ) * height_ < ( 1 << 16 ) ) {
		for ( size_t i = 0; i < work.size(); i++ ) {
			run( static_cast<int>( i ) );
		}
	} else {
		workers.Run( static_cast<int>( work.size() ), run );
	}
	if ( straight ) {
		// ranges of written rows, joined over small gaps (an upload per
		// range costs more than a few extra rows)
		changed_.clear();
		for ( const BandResult &r : results ) {
			for ( size_t i = 0; i + 1 < r.changed.size(); i += 2 ) {
				if ( !changed_.empty() && r.changed[i] - changed_.back() <= 16 ) {
					changed_.back() = r.changed[i + 1];
				} else {
					changed_.push_back( r.changed[i] );
					changed_.push_back( r.changed[i + 1] );
				}
			}
		}
		lastStraight_ = straight->data();
		lastStraightSize_ = straight->size();
	}
	for ( const BandResult &r : results ) {
		pixelsTested_ += r.tested;
		rowsDrawn_ += r.rows;
		rowsKept_ += r.kept;
	}
	ops_.clear();
	clearPending_ = false;
	if ( verify_ && clear ) {
		// the same triangles on a canvas with no bands to reuse
		static Canvas fresh;
		fresh.Resize( width_, height_ );
		fresh.bandsValid_ = false;
		fresh.ops_ = std::move( verifyOps );
		fresh.clearPending_ = true;
		std::vector<uint8_t> freshStraight;
		fresh.Flush( straight ? &freshStraight : nullptr );
		bool same = fresh.pixels_ == pixels_;
		if ( straight ) {
			same = same && freshStraight == *straight;
		}
		verified_++;
		mismatches_ += same ? 0 : 1;
	}
}

void Canvas::SetView( const Matrix &toPixels, int clipX0, int clipY0, int clipX1, int clipY1 ) {
	view_ = toPixels;
	clip_[0] = std::max( 0, clipX0 );
	clip_[1] = std::max( 0, clipY0 );
	clip_[2] = std::min( width_, clipX1 );
	clip_[3] = std::min( height_, clipY1 );
}

void Canvas::PushBlend( int mode ) {
	blend_.push_back( mode );
}

void Canvas::PopBlend() {
	if ( !blend_.empty() ) {
		blend_.pop_back();
	}
}

void Canvas::BeginSubmitMask( int mode ) {
	if ( mode == 0 ) {
		Op &op = NewOp();
		op.type = OP_CLEAR_STENCIL;
		op.x0 = 0;
		op.y0 = 0;
		op.x1 = width_;
		op.y1 = height_;
		op.wx1 = width_;
		stencilLevel_ = 0;
		ClearMaskBounds();
		if ( verify_ ) {
			verifyOps_.push_back( op );
		}
	}
	maskState_ = mode == 2 ? MASK_SUBMIT_DECR : MASK_SUBMIT_INCR;
}

void Canvas::EndSubmitMask() {
	if ( maskState_ == MASK_SUBMIT_INCR ) {
		stencilLevel_++;
	} else if ( maskState_ == MASK_SUBMIT_DECR ) {
		stencilLevel_ = std::max( 0, stencilLevel_ - 1 );
	}
	maskState_ = stencilLevel_ > 0 ? MASK_TEST : MASK_OFF;
}

void Canvas::DisableMask() {
	maskState_ = MASK_OFF;
	stencilLevel_ = 0;
}

Matrix Canvas::Full() const {
	return Matrix::Mul( view_, Matrix::Mul( user_, matrix_ ) );
}

/* A queued op, all zero (padding too: the flush hashes its bytes). */
Canvas::Op &Canvas::NewOp() {
	static_assert( std::is_trivially_copyable<Op>::value, "an Op is plain data" );
	ops_.emplace_back();
	Op &op = ops_.back();
	std::memset( static_cast<void *>( &op ), 0, sizeof( op ) );
	return op;
}

void Canvas::Raster( const ScreenVertex &a, const ScreenVertex &b, const ScreenVertex &c, const Fill &fill, bool explicitUV ) {
	float area = Edge( a.sx, a.sy, b.sx, b.sy, c.sx, c.sy );
	if ( !( std::fabs( area ) > 1e-12f ) || !std::isfinite( area ) ) {
		return;
	}
	const ScreenVertex *v0 = &a, *v1 = &b, *v2 = &c;
	if ( area < 0 ) {
		std::swap( v1, v2 );
		area = -area;
	}
	int x0 = static_cast<int>( std::floor( std::min( { v0->sx, v1->sx, v2->sx } ) ) );
	int y0 = static_cast<int>( std::floor( std::min( { v0->sy, v1->sy, v2->sy } ) ) );
	int x1 = static_cast<int>( std::ceil( std::max( { v0->sx, v1->sx, v2->sx } ) ) );
	int y1 = static_cast<int>( std::ceil( std::max( { v0->sy, v1->sy, v2->sy } ) ) );
	x0 = std::max( x0, clip_[0] );
	y0 = std::max( y0, clip_[1] );
	x1 = std::min( x1, clip_[2] );
	y1 = std::min( y1, clip_[3] );
	if ( x0 >= x1 || y0 >= y1 ) {
		return;
	}
	// A mask at level L lets through only pixels whose stencil is L, which
	// lie inside what raised it from L - 1 since it was cleared: a triangle
	// outside those bounds is not queued, and the others only over their
	// rows (with SetVerify, the check draws them whole). Columns are kept:
	// a row's values are stepped from its first pixel, so moving that pixel
	// would round them differently.
	int rows[2] = { y0, y1 }, columns[2] = { x0, x1 };
	bool maskedOut = false;
	if ( maskState_ == MASK_TEST && maskBoundsKnown_ && stencilLevel_ >= 1 && stencilLevel_ <= MASK_LEVELS ) {
		if ( stencilLevel_ - 1 >= maskBoundsTop_ ) {
			maskedOut = true;
		} else {
			const int *m = maskBounds_[stencilLevel_ - 1];
			rows[0] = std::max( y0, m[1] );
			rows[1] = std::min( y1, m[3] );
			columns[0] = std::max( x0, m[0] );
			columns[1] = std::min( x1, m[2] );
			maskedOut = columns[0] >= columns[1] || rows[0] >= rows[1];
		}
	}
	if ( maskedOut && !verify_ ) {
		return;
	}
	const bool submit = maskState_ == MASK_SUBMIT_INCR || maskState_ == MASK_SUBMIT_DECR;
	if ( maskState_ == MASK_SUBMIT_INCR ) {
		if ( stencilLevel_ < 0 || stencilLevel_ >= MASK_LEVELS ) {
			maskBoundsKnown_ = false;	// past what the bounds keep (or wrapping)
		} else {
			for ( ; maskBoundsTop_ <= stencilLevel_; maskBoundsTop_++ ) {
				int *e = maskBounds_[maskBoundsTop_];
				e[0] = e[1] = INT32_MAX;
				e[2] = e[3] = INT32_MIN;
			}
			int *m = maskBounds_[stencilLevel_];
			m[0] = std::min( m[0], x0 );
			m[1] = std::min( m[1], y0 );
			m[2] = std::max( m[2], x1 );
			m[3] = std::max( m[3], y1 );
		}
	}
	if ( !submit && !explicitUV && fill.kind != FILL_COLOR && fill.kind != FILL_TEXTURE && fill.kind != FILL_GOURAUD ) {
		return;
	}
	Op &op = NewOp();
	op.x0 = x0;
	op.y0 = y0;
	op.x1 = x1;
	op.y1 = y1;
	Resolve( op, v0, v1, v2, area, fill, explicitUV, submit );
	if ( verify_ ) {
		verifyOps_.push_back( op );
	}
	if ( maskedOut ) {
		ops_.pop_back();
		return;
	}
	op.y0 = rows[0];
	op.y1 = rows[1];
	op.wx0 = columns[0];
	op.wx1 = columns[1];
	triangles_++;
}

/* A queued triangle's fields, from its vertices (counter-clockwise, of
 * signed area area) and the state. Only the fields the fill uses are set;
 * the rest stay zero, so state left over from earlier commands (the fill
 * colour under a gouraud fill, say) does not make a repeated triangle look
 * new. */
void Canvas::Resolve( Op &op, const ScreenVertex *v0, const ScreenVertex *v1, const ScreenVertex *v2, float area,
	const Fill &fill, bool explicitUV, bool submit ) const {
	op.type = OP_TRIANGLE;
	op.mask = static_cast<uint8_t>( maskState_ );
	const ScreenVertex *vs[3] = { v0, v1, v2 };
	for ( int k = 0; k < 3; k++ ) {
		op.sx[k] = vs[k]->sx;
		op.sy[k] = vs[k]->sy;
	}
	op.tl[0] = TopLeft( v1->sx, v1->sy, v2->sx, v2->sy );
	op.tl[1] = TopLeft( v2->sx, v2->sy, v0->sx, v0->sy );
	op.tl[2] = TopLeft( v0->sx, v0->sy, v1->sx, v1->sy );
	if ( submit ) {
		op.shade = SHADE_SUBMIT;
		op.level = static_cast<uint8_t>( stencilLevel_ );
		return;
	}
	if ( maskState_ == MASK_TEST ) {
		op.level = static_cast<uint8_t>( stencilLevel_ );
	}
	const int mode = blend_.empty() ? BLEND_NORMAL : blend_.back();
	op.mode = static_cast<uint8_t>( NormalBlend( mode ) ? BLEND_NORMAL : mode );
	op.cx = cx_;

	// The fill. Gouraud fills whose factors are the same at every vertex
	// are drawn as what they reduce to: texture only (factor.b 1), colour
	// only (factor.b 0), a solid colour (one colour); with factor.a 1 edge
	// anti-aliasing changes nothing.
	auto all = [&]( auto test ) {
		return test( *vs[0] ) && test( *vs[1] ) && test( *vs[2] );
	};
	const bool opaqueEdges = all( []( const ScreenVertex &v ) { return v.factor[3] >= 1.0f; } );
	const bool lerpTexture = all( []( const ScreenVertex &v ) { return v.factor[2] == 1.0f; } );
	const bool lerpColour = all( []( const ScreenVertex &v ) { return v.factor[2] == 0.0f; } );
	const bool oneColour = all( [&]( const ScreenVertex &v ) { return !std::memcmp( v.color, vs[0]->color, sizeof( v.color ) ); } );
	const bool gouraudTex = !explicitUV && fill.kind == FILL_GOURAUD && fill.texCount > 0 && fill.gouraud != GFILL_COLOR;
	const bool twoTex = gouraudTex && fill.texCount > 1
		&& ( fill.gouraud == GFILL_2TEXTURE || fill.gouraud == GFILL_2TEXTURE_COLOR || fill.gouraud == GFILL_3TEXTURE );
	if ( explicitUV ) {
		op.shade = SHADE_GLYPH;
	} else if ( fill.kind == FILL_COLOR ) {
		op.shade = SHADE_COLOR;
	} else if ( fill.kind == FILL_TEXTURE ) {
		op.shade = SHADE_TEXTURE;
	} else if ( gouraudTex && lerpTexture && opaqueEdges ) {
		op.shade = SHADE_TEXTURE;
	} else if ( twoTex ) {
		op.shade = SHADE_GOURAUD_TEX2;
	} else if ( gouraudTex && !lerpColour ) {
		op.shade = SHADE_GOURAUD_TEX;
	} else if ( oneColour && opaqueEdges ) {
		op.shade = SHADE_COLOR;
	} else {
		op.shade = SHADE_GOURAUD;
	}

	if ( op.shade == SHADE_COLOR ) {
		std::memcpy( op.color, fill.kind == FILL_COLOR ? fill.color : vs[0]->color, sizeof( op.color ) );
		return;
	}
	// What the fill needs, as planes over the triangle.
	Plane w[3];
	WeightPlanes( op.sx, op.sy, 1.0f / area, w );
	auto attr = [&]( float ScreenVertex::*field ) {
		return Interpolate( w, vs[0]->*field, vs[1]->*field, vs[2]->*field );
	};
	auto attrK = [&]( float ( ScreenVertex::*field )[4], int k ) {
		return Interpolate( w, ( vs[0]->*field )[k], ( vs[1]->*field )[k], ( vs[2]->*field )[k] );
	};
	auto ref = []( const TextureFill &t, TexRef *r, uint64_t *version ) {
		r->texture = t.texture;
		r->clamp = t.clamp;
		r->linear = t.linear;
		*version = t.texture ? t.texture->version : 0;
	};
	if ( op.shade == SHADE_GLYPH || op.shade == SHADE_GOURAUD || op.shade == SHADE_GOURAUD_TEX ) {
		for ( int k = 0; k < 4; k++ ) {
			op.col[k] = attrK( &ScreenVertex::color, k );
		}
	}
	if ( op.shade == SHADE_GOURAUD || op.shade == SHADE_GOURAUD_TEX || op.shade == SHADE_GOURAUD_TEX2 ) {
		op.facAlpha = attrK( &ScreenVertex::factor, 3 );
		if ( op.shade != SHADE_GOURAUD ) {
			op.facLerp = attrK( &ScreenVertex::factor, 2 );
		}
	}
	if ( op.shade == SHADE_GLYPH ) {
		op.u0 = attr( &ScreenVertex::u );
		op.v0 = attr( &ScreenVertex::v );
		ref( fill.tex[0], &op.tex0, &op.version0 );
	} else if ( op.shade != SHADE_GOURAUD ) {
		const Plane shapeX = attr( &ScreenVertex::x ), shapeY = attr( &ScreenVertex::y );
		UvPlanes( fill.tex[0].uv, shapeX, shapeY, &op.u0, &op.v0 );
		ref( fill.tex[0], &op.tex0, &op.version0 );
		if ( op.shade == SHADE_GOURAUD_TEX2 ) {
			UvPlanes( fill.tex[1].uv, shapeX, shapeY, &op.u1, &op.v1 );
			ref( fill.tex[1], &op.tex1, &op.version1 );
		}
	}
}


void Canvas::DrawTriangles( const Vertex *v, const uint32_t *indices, int triangles, const Fill &fill ) {
	if ( fill.kind == FILL_NONE && maskState_ != MASK_SUBMIT_INCR && maskState_ != MASK_SUBMIT_DECR ) {
		return;
	}
	const Matrix m = Full();
	for ( int t = 0; t < triangles; t++ ) {
		ScreenVertex sv[3];
		for ( int k = 0; k < 3; k++ ) {
			const Vertex &src = v[indices[t * 3 + k]];
			ScreenVertex &d = sv[k];
			m.Apply( src.x, src.y, &d.sx, &d.sy );
			d.x = src.x;
			d.y = src.y;
			std::memcpy( d.color, src.color, sizeof( d.color ) );
			std::memcpy( d.factor, src.factor, sizeof( d.factor ) );
			d.u = d.v = 0;
		}
		Raster( sv[0], sv[1], sv[2], fill, false );
	}
}

void Canvas::DrawLineStrip( const Vertex *v, int count, const float color[4] ) {
	const Matrix m = Full();
	Fill fill;
	fill.kind = FILL_COLOR;
	std::memcpy( fill.color, color, sizeof( fill.color ) );
	for ( int i = 0; i + 1 < count; i++ ) {
		float ax, ay, bx, by;
		m.Apply( v[i].x, v[i].y, &ax, &ay );
		m.Apply( v[i + 1].x, v[i + 1].y, &bx, &by );
		float dx = bx - ax, dy = by - ay;
		const float len = std::sqrt( dx * dx + dy * dy );
		if ( len < 1e-6f ) {
			continue;
		}
		// a hairline: one pixel wide
		const float nx = -dy / len * 0.5f, ny = dx / len * 0.5f;
		ScreenVertex q[4] = {};
		q[0].sx = ax + nx; q[0].sy = ay + ny;
		q[1].sx = bx + nx; q[1].sy = by + ny;
		q[2].sx = bx - nx; q[2].sy = by - ny;
		q[3].sx = ax - nx; q[3].sy = ay - ny;
		Raster( q[0], q[1], q[2], fill, false );
		Raster( q[0], q[2], q[3], fill, false );
	}
}

void Canvas::DrawBitmaps( const BitmapQuad *quads, int count, const Matrix &bm, const Texture *tex ) {
	const Matrix m = Matrix::Mul( view_, Matrix::Mul( user_, bm ) );
	Fill fill;
	fill.kind = FILL_TEXTURE;
	fill.texCount = 1;
	fill.tex[0].texture = tex;
	fill.tex[0].clamp = true;
	fill.tex[0].linear = true;
	for ( int i = 0; i < count; i++ ) {
		const BitmapQuad &b = quads[i];
		ScreenVertex q[4] = {};
		const float xs[4] = { b.x0, b.x1, b.x1, b.x0 };
		const float ys[4] = { b.y0, b.y0, b.y1, b.y1 };
		const float us[4] = { b.u0, b.u1, b.u1, b.u0 };
		const float vs[4] = { b.v0, b.v0, b.v1, b.v1 };
		for ( int k = 0; k < 4; k++ ) {
			m.Apply( xs[k], ys[k], &q[k].sx, &q[k].sy );
			q[k].u = us[k];
			q[k].v = vs[k];
			std::memcpy( q[k].color, b.color, sizeof( b.color ) );
		}
		Raster( q[0], q[1], q[2], fill, true );
		Raster( q[0], q[2], q[3], fill, true );
	}
}


/* ---- image files -------------------------------------------------------------- */

/* Uncompressed and run-length TGA, 24 and 32 bits (the bot pictures). */
bool DecodeTGA( const uint8_t *d, size_t n, qaa_ui::Texture &t ) {
	if ( n < 18 ) {
		return false;
	}
	const int idLen = d[0], type = d[2];
	const int w = d[12] | ( d[13] << 8 ), h = d[14] | ( d[15] << 8 );
	const int bpp = d[16], desc = d[17];
	if ( ( type != 2 && type != 10 ) || ( bpp != 24 && bpp != 32 ) || w <= 0 || h <= 0 ) {
		return false;
	}
	const int bytes = bpp / 8;
	size_t at = 18 + idLen;
	std::vector<uint8_t> px( static_cast<size_t>( w ) * h * 4 );
	size_t count = static_cast<size_t>( w ) * h;
	size_t i = 0;
	auto put = [&]( const uint8_t *s ) {
		px[i * 4 + 0] = s[2];
		px[i * 4 + 1] = s[1];
		px[i * 4 + 2] = s[0];
		px[i * 4 + 3] = bytes == 4 ? s[3] : 255;
		i++;
	};
	while ( i < count ) {
		if ( type == 2 ) {
			if ( at + bytes > n ) {
				return false;
			}
			put( d + at );
			at += bytes;
			continue;
		}
		if ( at >= n ) {
			return false;
		}
		const int hdr = d[at++];
		const int run = ( hdr & 0x7F ) + 1;
		if ( hdr & 0x80 ) {
			if ( at + bytes > n ) {
				return false;
			}
			for ( int k = 0; k < run && i < count; k++ ) {
				put( d + at );
			}
			at += bytes;
		} else {
			for ( int k = 0; k < run && i < count; k++ ) {
				if ( at + bytes > n ) {
					return false;
				}
				put( d + at );
				at += bytes;
			}
		}
	}
	t.width = w;
	t.height = h;
	t.rgba.resize( px.size() );
	const bool topDown = ( desc & 0x20 ) != 0;
	for ( int y = 0; y < h; y++ ) {
		const int sy = topDown ? y : h - 1 - y;
		std::memcpy( &t.rgba[static_cast<size_t>( y ) * w * 4], &px[static_cast<size_t>( sy ) * w * 4], static_cast<size_t>( w ) * 4 );
	}
	return true;
}

/* The DDS files QAA's menus load (ui/*.dds: the title and menu art, level
 * shots, character portraits): standard little-endian DirectDraw Surfaces,
 * DXT1, DXT2, DXT3 and DXT5, or uncompressed 32-bit ARGB (docs/FORMATS.md).
 * The top mip level, to straight RGBA; DXT2 is DXT3 with premultiplied
 * colour, taken back to straight. */
bool DecodeDDS( const uint8_t *d, size_t n, qaa_ui::Texture &t ) {
	auto u32 = [d]( size_t at ) {
		return static_cast<uint32_t>( d[at] | ( d[at + 1] << 8 ) | ( d[at + 2] << 16 ) | ( d[at + 3] << 24 ) );
	};
	if ( n < 128 || u32( 0 ) != 0x20534444u || u32( 4 ) != 124 ) {
		return false;
	}
	const int h = static_cast<int>( u32( 12 ) ), w = static_cast<int>( u32( 16 ) );
	if ( w <= 0 || h <= 0 || w > 8192 || h > 8192 ) {
		return false;
	}
	const uint32_t pfFlags = u32( 80 ), fourcc = u32( 84 ), bits = u32( 88 );
	const uint8_t *px = d + 128;
	const size_t avail = n - 128;
	t.width = w;
	t.height = h;
	t.alphaOnly = false;
	if ( pfFlags & 0x4 ) {	// DDPF_FOURCC
		int format;
		switch ( fourcc ) {
		case 0x31545844u: format = 10; break;				// DXT1
		case 0x32545844u: case 0x33545844u: format = 11; break;	// DXT2, DXT3
		case 0x35545844u: format = 12; break;				// DXT5
		default: return false;
		}
		const size_t need = static_cast<size_t>( ( w + 3 ) / 4 ) * ( ( h + 3 ) / 4 ) * ( format == 10 ? 8 : 16 );
		if ( avail < need || !DecodeDXT( format, px, w, h, t.rgba ) ) {
			return false;
		}
		if ( fourcc == 0x32545844u ) {
			for ( size_t i = 0; i < t.rgba.size(); i += 4 ) {
				const int a = t.rgba[i + 3];
				for ( int k = 0; k < 3; k++ ) {
					t.rgba[i + k] = static_cast<uint8_t>( a ? std::min( 255, t.rgba[i + k] * 255 / a ) : 0 );
				}
			}
		}
		return true;
	}
	if ( bits != 32 || u32( 92 ) != 0x00FF0000u || u32( 96 ) != 0x0000FF00u || u32( 100 ) != 0x000000FFu ) {
		return false;
	}
	if ( avail < static_cast<size_t>( w ) * h * 4 ) {
		return false;
	}
	const bool alpha = ( pfFlags & 0x1 ) && u32( 104 ) == 0xFF000000u;
	t.rgba.resize( static_cast<size_t>( w ) * h * 4 );
	for ( size_t i = 0; i < t.rgba.size(); i += 4 ) {	// B, G, R, A in memory
		t.rgba[i + 0] = px[i + 2];
		t.rgba[i + 1] = px[i + 1];
		t.rgba[i + 2] = px[i + 0];
		t.rgba[i + 3] = alpha ? px[i + 3] : 255;
	}
	return true;
}

}
