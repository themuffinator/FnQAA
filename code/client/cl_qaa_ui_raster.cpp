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

namespace qaa_ui {

/* ---- row bands --------------------------------------------------------------

Large triangles (full-screen backgrounds and panels, most of a menu's cost)
are split into bands of rows run on a few worker threads. A triangle's rows
never overlap, each pixel is written by one band, and the triangles still
run one after another, so the result is the same as on one thread. The pool
is created on first use and never destroyed: its threads wait for work and
end with the process. */
class RowPool {
public:
	static RowPool &Get() {
		static RowPool *pool = new RowPool();
		return *pool;
	}
	int Bands() const { return bands_; }
	/* Run fn(y0, y1) over [y0, y1) in pieces and wait for all of them. Rows
	 * are handed out in small chunks, so bands of uneven cost (a triangle's
	 * rows differ in length) still finish together. */
	void Run( int y0, int y1, const std::function<void( int, int )> &fn ) {
		if ( bands_ <= 1 ) {
			fn( y0, y1 );
			return;
		}
		job_ = &fn;
		end_ = y1;
		chunk_ = std::max( 1, ( y1 - y0 ) / ( bands_ * 8 ) );
		next_.store( y0, std::memory_order_relaxed );
		busy_.store( bands_ - 1, std::memory_order_relaxed );
		{
			std::lock_guard<std::mutex> lock( mutex_ );
			generation_.fetch_add( 1, std::memory_order_release );
		}
		wake_.notify_all();
		Take();
		// the workers finish their last chunks; spin briefly, they are short
		while ( busy_.load( std::memory_order_acquire ) > 0 ) {
			std::this_thread::yield();
		}
		job_ = nullptr;
	}

private:
	RowPool() {
		const unsigned hw = std::thread::hardware_concurrency();
		int threads = std::max( 0, std::min( 7, static_cast<int>( hw ) - 1 ) );
		if ( const char *env = std::getenv( "QAA_UI_THREADS" ) ) {	// diagnostics: total threads, 1 = none
			threads = std::max( 0, std::min( 31, std::atoi( env ) - 1 ) );
		}
		bands_ = threads + 1;
		for ( int i = 0; i < threads; i++ ) {
			std::thread( [this] { Work(); } ).detach();
		}
	}
	/* Run chunks of rows until none are left. */
	void Take() {
		for ( ;; ) {
			const int a = next_.fetch_add( chunk_, std::memory_order_relaxed );
			if ( a >= end_ ) {
				return;
			}
			( *job_ )( a, std::min( end_, a + chunk_ ) );
		}
	}
	void Work() {
		unsigned seen = 0;
		for ( ;; ) {
			// Spin a little before sleeping: the next triangle usually comes
			// within microseconds, and waking a sleeping thread takes longer.
			unsigned gen = generation_.load( std::memory_order_acquire );
			for ( int spin = 0; gen == seen && spin < 4000; spin++ ) {
				std::this_thread::yield();
				gen = generation_.load( std::memory_order_acquire );
			}
			if ( gen == seen ) {
				std::unique_lock<std::mutex> lock( mutex_ );
				wake_.wait( lock, [&] { return generation_.load( std::memory_order_acquire ) != seen; } );
				gen = generation_.load( std::memory_order_acquire );
			}
			seen = gen;
			Take();
			busy_.fetch_sub( 1, std::memory_order_acq_rel );
		}
	}
	int bands_ = 1;
	std::mutex mutex_;
	std::condition_variable wake_;
	std::atomic<unsigned> generation_{ 0 };
	std::atomic<int> next_{ 0 }, busy_{ 0 };
	const std::function<void( int, int )> *job_ = nullptr;
	int end_ = 0, chunk_ = 1;
};

/* Triangles covering at least this many pixels of their bounding box are
 * split into row bands. */
static const int PARALLEL_PIXELS = 1 << 15;

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

void Canvas::Resize( int width, int height ) {
	if ( width == width_ && height == height_ ) {
		return;
	}
	width_ = std::max( width, 1 );
	height_ = std::max( height, 1 );
	pixels_.assign( static_cast<size_t>( width_ ) * height_ * 4, 0 );
	stencil_.assign( static_cast<size_t>( width_ ) * height_, 0 );
	clip_[0] = clip_[1] = 0;
	clip_[2] = width_;
	clip_[3] = height_;
}

void Canvas::Clear() {
	std::fill( pixels_.begin(), pixels_.end(), uint8_t( 0 ) );
	std::fill( stencil_.begin(), stencil_.end(), uint8_t( 0 ) );
	blend_.clear();
	stencilLevel_ = 0;
	maskState_ = MASK_OFF;
	matrix_ = Matrix();
	user_ = Matrix();
	cx_ = Cxform();
	triangles_ = 0;
	pixelsTested_ = 0;
}

void Canvas::ToStraight( std::vector<uint8_t> &out ) const {
	out.resize( pixels_.size() );
	const size_t n = pixels_.size() / 4;
	for ( size_t i = 0; i < n; i++ ) {
		const uint8_t *p = &pixels_[i * 4];
		uint8_t *o = &out[i * 4];
		const int a = p[3];
		if ( a == 0 ) {
			o[0] = o[1] = o[2] = o[3] = 0;
		} else if ( a == 255 ) {
			std::memcpy( o, p, 4 );
		} else {
			o[0] = static_cast<uint8_t>( std::min( 255, p[0] * 255 / a ) );
			o[1] = static_cast<uint8_t>( std::min( 255, p[1] * 255 / a ) );
			o[2] = static_cast<uint8_t>( std::min( 255, p[2] * 255 / a ) );
			o[3] = static_cast<uint8_t>( a );
		}
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
		std::fill( stencil_.begin(), stencil_.end(), uint8_t( 0 ) );
		stencilLevel_ = 0;
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

void Canvas::Shade( int x, int y, const float s[4], int mode ) {
	const size_t i = static_cast<size_t>( y ) * width_ + x;
	switch ( maskState_ ) {
	case MASK_SUBMIT_INCR:
		if ( stencil_[i] == stencilLevel_ ) {
			stencil_[i] = static_cast<uint8_t>( stencilLevel_ + 1 );
		}
		return;
	case MASK_SUBMIT_DECR:
		if ( stencil_[i] == stencilLevel_ && stencilLevel_ > 0 ) {
			stencil_[i] = static_cast<uint8_t>( stencilLevel_ - 1 );
		}
		return;
	case MASK_TEST:
		if ( stencil_[i] != stencilLevel_ ) {
			return;
		}
		break;
	default:
		break;
	}
	uint8_t *p = &pixels_[i * 4];
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

static inline float Edge( float ax, float ay, float bx, float by, float px, float py ) {
	return ( bx - ax ) * ( py - ay ) - ( by - ay ) * ( px - ax );
}

/* Top-left rule: a pixel exactly on a shared edge belongs to one triangle. */
static inline bool TopLeft( float ax, float ay, float bx, float by ) {
	return ( ay == by && bx < ax ) || by < ay;
}

/* A value interpolated over a triangle, as a plane in canvas pixels:
 * value(px, py) = dx * px + dy * py + c. */
struct Plane {
	float dx = 0, dy = 0, c = 0;
	float At( float px, float py ) const { return dx * px + dy * py + c; }
};

/* The planes of the three barycentric weights of (v0, v1, v2), whose
 * signed area (Edge(v0, v1, v2)) is 1 / inv. Weight i is Edge of the
 * opposite edge over the area, as the per-pixel coverage test uses. */
static void WeightPlanes( const float sx[3], const float sy[3], float inv, Plane w[3] ) {
	for ( int i = 0; i < 3; i++ ) {
		const int a = ( i + 1 ) % 3, b = ( i + 2 ) % 3;
		// Edge(a, b, p) = (bx - ax)(py - ay) - (by - ay)(px - ax)
		w[i].dx = -( sy[b] - sy[a] ) * inv;
		w[i].dy = ( sx[b] - sx[a] ) * inv;
		w[i].c = ( ( sy[b] - sy[a] ) * sx[a] - ( sx[b] - sx[a] ) * sy[a] ) * inv;
	}
}

static Plane Interpolate( const Plane w[3], float f0, float f1, float f2 ) {
	Plane p;
	p.dx = w[0].dx * f0 + w[1].dx * f1 + w[2].dx * f2;
	p.dy = w[0].dy * f0 + w[1].dy * f1 + w[2].dy * f2;
	p.c = w[0].c * f0 + w[1].c * f1 + w[2].c * f2;
	return p;
}

/* Texture coordinate planes: the UV matrix applied to the shape-space planes. */
static void UvPlanes( const Matrix &m, const Plane &x, const Plane &y, Plane *u, Plane *v ) {
	u->dx = m.m[0] * x.dx + m.m[1] * y.dx;
	u->dy = m.m[0] * x.dy + m.m[1] * y.dy;
	u->c = m.m[0] * x.c + m.m[1] * y.c + m.m[2];
	v->dx = m.m[3] * x.dx + m.m[4] * y.dx;
	v->dy = m.m[3] * x.dy + m.m[4] * y.dy;
	v->c = m.m[3] * x.c + m.m[4] * y.c + m.m[5];
}

static inline int FloorInt( float f ) {
	const int i = static_cast<int>( f );
	return f < static_cast<float>( i ) ? i - 1 : i;
}

/* A texel coordinate inside [0, size): clamped, or wrapped for Wrap_Repeat. */
static inline int Address( int i, int size, bool clamp ) {
	if ( static_cast<unsigned>( i ) < static_cast<unsigned>( size ) ) {
		return i;
	}
	if ( clamp ) {
		return i < 0 ? 0 : size - 1;
	}
	i %= size;
	return i < 0 ? i + size : i;
}

/* SampleTexture, in integer texel addressing: the same results, without
 * per-texel float conversion and function calls. */
static inline void Sample( const TextureFill &t, float u, float v, float out[4] ) {
	const Texture *tex = t.texture;
	if ( !tex || tex->width <= 0 || tex->height <= 0 || tex->rgba.empty() ) {
		out[0] = out[1] = out[2] = out[3] = 1.0f;
		return;
	}
	const int w = tex->width, h = tex->height;
	const uint8_t *base = tex->rgba.data();
	if ( !t.linear ) {
		// SampleTexture's floor(u * w - 0.5 + 0.5)
		const int x = Address( FloorInt( u * w ), w, t.clamp );
		const int y = Address( FloorInt( v * h ), h, t.clamp );
		const uint8_t *p = base + ( static_cast<size_t>( y ) * w + x ) * 4;
		for ( int k = 0; k < 4; k++ ) {
			out[k] = p[k] * ( 1.0f / 255.0f );
		}
		return;
	}
	const float fx = u * w - 0.5f, fy = v * h - 0.5f;
	const int ix = FloorInt( fx ), iy = FloorInt( fy );
	const float ax = fx - ix, ay = fy - iy;
	const int xa = Address( ix, w, t.clamp ), xb = Address( ix + 1, w, t.clamp );
	const int ya = Address( iy, h, t.clamp ), yb = Address( iy + 1, h, t.clamp );
	const uint8_t *p00 = base + ( static_cast<size_t>( ya ) * w + xa ) * 4;
	const uint8_t *p10 = base + ( static_cast<size_t>( ya ) * w + xb ) * 4;
	const uint8_t *p01 = base + ( static_cast<size_t>( yb ) * w + xa ) * 4;
	const uint8_t *p11 = base + ( static_cast<size_t>( yb ) * w + xb ) * 4;
	const float w11 = ax * ay, w10 = ax - w11, w01 = ay - w11, w00 = 1.0f - ax - ay + w11;
	for ( int k = 0; k < 4; k++ ) {
		out[k] = ( p00[k] * w00 + p10[k] * w10 + p01[k] * w01 + p11[k] * w11 ) * ( 1.0f / 255.0f );
	}
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
	const bool submit = maskState_ == MASK_SUBMIT_INCR || maskState_ == MASK_SUBMIT_DECR;
	if ( !submit && !explicitUV && fill.kind != FILL_COLOR && fill.kind != FILL_TEXTURE && fill.kind != FILL_GOURAUD ) {
		return;
	}
	triangles_++;
	const bool tl0 = TopLeft( v1->sx, v1->sy, v2->sx, v2->sy );
	const bool tl1 = TopLeft( v2->sx, v2->sy, v0->sx, v0->sy );
	const bool tl2 = TopLeft( v0->sx, v0->sy, v1->sx, v1->sy );
	const int mode = blend_.empty() ? BLEND_NORMAL : blend_.back();
	// Blend modes Shade treats as normal (GFx draws most shapes as "none" or
	// "layer"), unmasked or stencil-tested: blended inline below.
	const bool normalBlend = mode != BLEND_ADD && mode != BLEND_SUBTRACT && mode != BLEND_MULTIPLY
		&& mode != BLEND_DARKEN && mode != BLEND_LIGHTEN && mode != BLEND_SCREEN;
	const bool plain = normalBlend && ( maskState_ == MASK_OFF || maskState_ == MASK_TEST );
	const bool stencilTest = maskState_ == MASK_TEST;
	const uint8_t level = static_cast<uint8_t>( stencilLevel_ );

	// What the fill needs, as planes over the triangle.
	const ScreenVertex *vs[3] = { v0, v1, v2 };
	const float sx[3] = { v0->sx, v1->sx, v2->sx }, sy[3] = { v0->sy, v1->sy, v2->sy };
	Plane w[3];
	WeightPlanes( sx, sy, 1.0f / area, w );
	auto attr = [&]( float ScreenVertex::*field ) {
		return Interpolate( w, vs[0]->*field, vs[1]->*field, vs[2]->*field );
	};
	auto attrK = [&]( float ( ScreenVertex::*field )[4], int k ) {
		return Interpolate( w, ( vs[0]->*field )[k], ( vs[1]->*field )[k], ( vs[2]->*field )[k] );
	};
	const bool gouraudTex = !explicitUV && fill.kind == FILL_GOURAUD && fill.texCount > 0 && fill.gouraud != GFILL_COLOR;
	const bool twoTex = gouraudTex && fill.texCount > 1
		&& ( fill.gouraud == GFILL_2TEXTURE || fill.gouraud == GFILL_2TEXTURE_COLOR || fill.gouraud == GFILL_3TEXTURE );
	const bool needColor = explicitUV || ( fill.kind == FILL_GOURAUD && !twoTex );
	Plane col[4], facLerp, facAlpha, u0, v0p, u1, v1p;
	if ( needColor ) {
		for ( int k = 0; k < 4; k++ ) {
			col[k] = attrK( &ScreenVertex::color, k );
		}
	}
	if ( !explicitUV && fill.kind == FILL_GOURAUD ) {
		facLerp = attrK( &ScreenVertex::factor, 2 );
		facAlpha = attrK( &ScreenVertex::factor, 3 );
	}
	if ( explicitUV ) {
		u0 = attr( &ScreenVertex::u );
		v0p = attr( &ScreenVertex::v );
	} else if ( fill.kind == FILL_TEXTURE || gouraudTex ) {
		const Plane shapeX = attr( &ScreenVertex::x ), shapeY = attr( &ScreenVertex::y );
		UvPlanes( fill.tex[0].uv, shapeX, shapeY, &u0, &v0p );
		if ( twoTex ) {
			UvPlanes( fill.tex[1].uv, shapeX, shapeY, &u1, &v1p );
		}
	}
	const Cxform cx = cx_;

	// The x range a row can cover, from each edge's half-plane, widened by a
	// pixel: the exact per-pixel test below still decides every pixel.
	const ScreenVertex *ev[3][2] = { { v1, v2 }, { v2, v0 }, { v0, v1 } };
	std::atomic<int64_t> testedAll{ 0 };
	auto rows = [&]( int ya, int yb ) {
		int64_t tested = 0;
		for ( int y = ya; y < yb; y++ ) {
			const float py = y + 0.5f;
			// Where each edge crosses the row: px <= cross (dy > 0) or px >= cross
			// (dy < 0). [xa, xb) widens that by a pixel and [inA, inB) narrows it
			// by one: pixels in [inA, inB) are inside every edge and skip the test.
			float lo = static_cast<float>( x0 ), hi = static_cast<float>( x1 );
			bool interior = true;
			for ( const auto &e : ev ) {
				// Edge(a, b, px, py) = (b.x - a.x)(py - a.y) - (b.y - a.y)(px - a.x) >= 0
				const float dy = e[1]->sy - e[0]->sy;
				const float k = ( e[1]->sx - e[0]->sx ) * ( py - e[0]->sy );
				if ( dy == 0 ) {
					interior = interior && k > 0;	// the same sign along the whole row
					continue;
				}
				const float cross = e[0]->sx + k / dy;
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
			const int xa = std::max( x0, static_cast<int>( std::floor( lo - 1.0f ) ) );
			const int xb = std::min( x1, static_cast<int>( std::ceil( hi + 1.0f ) ) + 1 );
			if ( xa >= xb ) {
				continue;
			}
			const int inA = interior ? std::max( xa, static_cast<int>( std::ceil( lo + 1.0f ) ) ) : xb;
			const int inB = interior ? std::min( xb, static_cast<int>( std::floor( hi - 1.0f ) ) ) : xb;
			tested += xb - xa;
			// the planes at the row's first pixel, stepped by dx per pixel
			const float pxa = xa + 0.5f;
			float c[4];
			for ( int k = 0; k < 4; k++ ) {
				c[k] = col[k].At( pxa, py );
			}
			float fl = facLerp.At( pxa, py ), fa = facAlpha.At( pxa, py );
			float uu = u0.At( pxa, py ), vv = v0p.At( pxa, py );
			float uu1 = u1.At( pxa, py ), vv1 = v1p.At( pxa, py );
			uint8_t *row = &pixels_[( static_cast<size_t>( y ) * width_ ) * 4];
			for ( int x = xa; x < xb; x++, fl += facLerp.dx, fa += facAlpha.dx, uu += u0.dx, vv += v0p.dx,
				uu1 += u1.dx, vv1 += v1p.dx, c[0] += col[0].dx, c[1] += col[1].dx, c[2] += col[2].dx, c[3] += col[3].dx ) {
				if ( x < inA || x >= inB ) {
					const float px = x + 0.5f;
					const float e0 = Edge( v1->sx, v1->sy, v2->sx, v2->sy, px, py );
					const float e1 = Edge( v2->sx, v2->sy, v0->sx, v0->sy, px, py );
					const float e2 = Edge( v0->sx, v0->sy, v1->sx, v1->sy, px, py );
					if ( e0 < 0 || e1 < 0 || e2 < 0 ) {
						continue;
					}
					if ( ( e0 == 0 && !tl0 ) || ( e1 == 0 && !tl1 ) || ( e2 == 0 && !tl2 ) ) {
						continue;
					}
				}
				if ( submit ) {
					const float none[4] = { 0, 0, 0, 0 };
					Shade( x, y, none, mode );
					continue;
				}
				float out[4];
				if ( explicitUV ) {
					// DrawBitmaps (glyphs): the quad's colour, alpha from the texture
					float t[4];
					Sample( fill.tex[0], uu, vv, t );
					std::memcpy( out, c, sizeof( out ) );
					out[3] *= t[3];
				} else if ( fill.kind == FILL_COLOR ) {
					std::memcpy( out, fill.color, sizeof( out ) );
				} else if ( fill.kind == FILL_TEXTURE ) {
					Sample( fill.tex[0], uu, vv, out );
				} else if ( !gouraudTex ) {
					std::memcpy( out, c, sizeof( out ) );
				} else {
					float t0[4], t1[4];
					Sample( fill.tex[0], uu, vv, t0 );
					const float *other = c;
					if ( twoTex ) {
						Sample( fill.tex[1], uu1, vv1, t1 );
						other = t1;
					}
					for ( int k = 0; k < 4; k++ ) {
						out[k] = other[k] + ( t0[k] - other[k] ) * fl;
					}
				}
				for ( int k = 0; k < 4; k++ ) {
					out[k] = Clamp01( out[k] * cx.mul[k] + cx.add[k] );
				}
				if ( !explicitUV && fill.kind == FILL_GOURAUD ) {
					out[3] *= Clamp01( fa );	// edge anti-aliasing
				}
				if ( out[3] <= 0.0f ) {
					continue;
				}
				out[0] *= out[3];
				out[1] *= out[3];
				out[2] *= out[3];
				if ( plain ) {
					// Shade's mask test and normal blend, inline
					if ( stencilTest && stencil_[static_cast<size_t>( y ) * width_ + x] != level ) {
						continue;
					}
					uint8_t *p = row + x * 4;
					const float keep = ( 1.0f - out[3] ) * ( 1.0f / 255.0f );
					for ( int k = 0; k < 4; k++ ) {
						p[k] = static_cast<uint8_t>( Clamp01( out[k] + p[k] * keep ) * 255.0f + 0.5f );
					}
				} else {
					Shade( x, y, out, mode );
				}
			}
		}
		testedAll += tested;
	};
	if ( static_cast<int64_t>( x1 - x0 ) * ( y1 - y0 ) >= PARALLEL_PIXELS && y1 - y0 >= 8 ) {
		RowPool::Get().Run( y0, y1, rows );
	} else {
		rows( y0, y1 );
	}
	pixelsTested_ += testedAll;
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
