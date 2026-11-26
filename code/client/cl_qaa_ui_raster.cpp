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
#include <cmath>
#include <cstring>

namespace qaa_ui {

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
	if ( clamp ) {
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

void Canvas::Shade( int x, int y, const float s[4] ) {
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
	const int mode = blend_.empty() ? BLEND_NORMAL : blend_.back();
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
	triangles_++;
	const bool tl0 = TopLeft( v1->sx, v1->sy, v2->sx, v2->sy );
	const bool tl1 = TopLeft( v2->sx, v2->sy, v0->sx, v0->sy );
	const bool tl2 = TopLeft( v0->sx, v0->sy, v1->sx, v1->sy );
	const float inv = 1.0f / area;
	const bool submit = maskState_ == MASK_SUBMIT_INCR || maskState_ == MASK_SUBMIT_DECR;

	for ( int y = y0; y < y1; y++ ) {
		const float py = y + 0.5f;
		for ( int x = x0; x < x1; x++ ) {
			const float px = x + 0.5f;
			const float w0 = Edge( v1->sx, v1->sy, v2->sx, v2->sy, px, py );
			const float w1 = Edge( v2->sx, v2->sy, v0->sx, v0->sy, px, py );
			const float w2 = Edge( v0->sx, v0->sy, v1->sx, v1->sy, px, py );
			if ( w0 < 0 || w1 < 0 || w2 < 0 ) {
				continue;
			}
			if ( ( w0 == 0 && !tl0 ) || ( w1 == 0 && !tl1 ) || ( w2 == 0 && !tl2 ) ) {
				continue;
			}
			if ( submit ) {
				const float none[4] = { 0, 0, 0, 0 };
				Shade( x, y, none );
				continue;
			}
			const float b0 = w0 * inv, b1 = w1 * inv, b2 = w2 * inv;
			float col[4], fac[4];
			for ( int k = 0; k < 4; k++ ) {
				col[k] = v0->color[k] * b0 + v1->color[k] * b1 + v2->color[k] * b2;
				fac[k] = v0->factor[k] * b0 + v1->factor[k] * b1 + v2->factor[k] * b2;
			}
			const float sxs = v0->x * b0 + v1->x * b1 + v2->x * b2;
			const float sys = v0->y * b0 + v1->y * b1 + v2->y * b2;
			float out[4];
			if ( explicitUV ) {
				// DrawBitmaps (glyphs): the quad's colour, alpha from the texture
				const float u = v0->u * b0 + v1->u * b1 + v2->u * b2;
				const float v = v0->v * b0 + v1->v * b1 + v2->v * b2;
				float t[4];
				SampleTexture( fill.tex[0], u, v, t );
				std::memcpy( out, col, sizeof( out ) );
				out[3] *= t[3];
			} else {
				switch ( fill.kind ) {
				case FILL_COLOR:
					std::memcpy( out, fill.color, sizeof( out ) );
					break;
				case FILL_TEXTURE: {
					float u, v;
					fill.tex[0].uv.Apply( sxs, sys, &u, &v );
					SampleTexture( fill.tex[0], u, v, out );
					break;
				}
				case FILL_GOURAUD: {
					if ( fill.texCount == 0 || fill.gouraud == GFILL_COLOR ) {
						std::memcpy( out, col, sizeof( out ) );
					} else {
						float t0[4], u, v;
						fill.tex[0].uv.Apply( sxs, sys, &u, &v );
						SampleTexture( fill.tex[0], u, v, t0 );
						const float *other = col;
						float t1[4];
						if ( fill.texCount > 1 && ( fill.gouraud == GFILL_2TEXTURE || fill.gouraud == GFILL_2TEXTURE_COLOR
							|| fill.gouraud == GFILL_3TEXTURE ) ) {
							fill.tex[1].uv.Apply( sxs, sys, &u, &v );
							SampleTexture( fill.tex[1], u, v, t1 );
							other = t1;
						}
						for ( int k = 0; k < 4; k++ ) {
							out[k] = other[k] + ( t0[k] - other[k] ) * fac[2];
						}
					}
					break;
				}
				default:
					return;
				}
			}
			for ( int k = 0; k < 4; k++ ) {
				out[k] = Clamp01( out[k] * cx_.mul[k] + cx_.add[k] );
			}
			if ( !explicitUV && fill.kind == FILL_GOURAUD ) {
				out[3] *= Clamp01( fac[3] );	// edge anti-aliasing
			}
			if ( out[3] <= 0.0f ) {
				continue;
			}
			out[0] *= out[3];
			out[1] *= out[3];
			out[2] *= out[3];
			Shade( x, y, out );
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
