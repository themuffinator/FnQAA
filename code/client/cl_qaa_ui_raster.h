/*
===========================================================================
FnQAA: software rasteriser for QAA's Scaleform front end.

Copyright (C) 2026 FnQAA contributors.
This file is part of FnQAA, licensed under the GNU GPL v2 or later.

Replays what GFx 2.x's renderer interface asks for (triangles with colour,
edge-AA factors and texture fills, bitmap quads, hairlines, colour
transforms, blend modes and stencil masks) into a premultiplied RGBA
canvas. The fill rules follow the pixel shaders of QAA's own Xbox
renderer back end (default.xex, the SFUI-Backend at 0x821687F0; their HLSL
source is at 0x820CB900..0x820CE300): output is premultiplied, gouraud
fills take lerp(colour, texture, factor.b), edge AA multiplies alpha by
factor.a, and glyph fills take only the texture's alpha. See docs/UI.md.

The canvas is presented by the client (DrawWebUISurface); this file knows
nothing about the engine and is unit-tested on synthetic data.
===========================================================================
*/
#ifndef CL_QAA_UI_RASTER_H
#define CL_QAA_UI_RASTER_H

#include <cstdint>
#include <vector>

namespace qaa_ui {

/* A texture in straight (not premultiplied) RGBA. Alpha-only textures
 * (glyph caches) keep their alpha in a and 255 in rgb. */
struct Texture {
	int width = 0, height = 0;
	std::vector<uint8_t> rgba;
	bool alphaOnly = false;
};

/* GFx 2.x's 2x3 matrix: x' = m[0]x + m[1]y + m[2], y' = m[3]x + m[4]y + m[5]. */
struct Matrix {
	float m[6] = { 1, 0, 0, 0, 1, 0 };
	static Matrix Mul( const Matrix &a, const Matrix &b );	/* a after b */
	void Apply( float x, float y, float *ox, float *oy ) const {
		*ox = m[0] * x + m[1] * y + m[2];
		*oy = m[3] * x + m[4] * y + m[5];
	}
};

/* Colour transform: out = in * mul + add, add in 0..1 units. */
struct Cxform {
	float mul[4] = { 1, 1, 1, 1 };
	float add[4] = { 0, 0, 0, 0 };
};

enum BlendMode {
	BLEND_NONE = 0, BLEND_NORMAL, BLEND_LAYER, BLEND_MULTIPLY, BLEND_SCREEN, BLEND_LIGHTEN, BLEND_DARKEN,
	BLEND_DIFFERENCE, BLEND_ADD, BLEND_SUBTRACT, BLEND_INVERT, BLEND_ALPHA, BLEND_ERASE, BLEND_OVERLAY,
	BLEND_HARDLIGHT,
};

/* A texture fill: the texture and the matrix from shape space to UV. */
struct TextureFill {
	const Texture *texture = nullptr;
	Matrix uv;
	bool clamp = false;		/* Wrap_Clamp; else Wrap_Repeat */
	bool linear = true;		/* Sample_Linear; else Sample_Point */
};

enum FillKind {
	FILL_NONE,
	FILL_COLOR,			/* a solid colour (FillStyleColor) */
	FILL_TEXTURE,		/* FillStyleBitmap */
	FILL_GOURAUD,		/* FillStyleGouraud: per-vertex colour and factors */
};

/* GFx 2.x GouraudFillType */
enum GouraudType {
	GFILL_COLOR = 0, GFILL_1TEXTURE, GFILL_1TEXTURE_COLOR, GFILL_2TEXTURE, GFILL_2TEXTURE_COLOR, GFILL_3TEXTURE,
};

struct Fill {
	FillKind kind = FILL_NONE;
	float color[4] = { 1, 1, 1, 1 };	/* straight RGBA, FILL_COLOR */
	int gouraud = GFILL_COLOR;
	TextureFill tex[3];
	int texCount = 0;
};

/* A vertex as the replay hands it over: shape-space position, straight
 * RGBA colour and factors in 0..1. */
struct Vertex {
	float x, y;
	float color[4];
	float factor[4];
};

/* One bitmap quad (GRenderer::BitmapDesc). */
struct BitmapQuad {
	float x0, y0, x1, y1;		/* shape space */
	float u0, v0, u1, v1;
	float color[4];				/* straight RGBA */
};

class Canvas {
public:
	void Resize( int width, int height );
	int Width() const { return width_; }
	int Height() const { return height_; }
	/* premultiplied RGBA, row-major, top row first */
	const uint8_t *Pixels() const { return pixels_.data(); }
	/* straight RGBA, for presenting with ordinary alpha blending */
	void ToStraight( std::vector<uint8_t> &out ) const;

	void Clear();
	/* Transform from movie space to canvas pixels, and the clip rectangle. */
	void SetView( const Matrix &toPixels, int clipX0, int clipY0, int clipX1, int clipY1 );
	void SetMatrix( const Matrix &m ) { matrix_ = m; }
	void SetUserMatrix( const Matrix &m ) { user_ = m; }
	void SetCxform( const Cxform &c ) { cx_ = c; }
	const Cxform &CurrentCxform() const { return cx_; }
	void PushBlend( int mode );
	void PopBlend();

	void BeginSubmitMask( int mode );	/* 0 clear, 1 increment, 2 decrement */
	void EndSubmitMask();
	void DisableMask();

	void DrawTriangles( const Vertex *v, const uint32_t *indices, int triangles, const Fill &fill );
	void DrawLineStrip( const Vertex *v, int count, const float color[4] );
	void DrawBitmaps( const BitmapQuad *quads, int count, const Matrix &m, const Texture *tex );

	int Triangles() const { return triangles_; }
	/* for tracing: the current movie-to-pixel transform, blend mode and mask state */
	Matrix FullMatrix() const { return Full(); }
	int BlendMode() const { return blend_.empty() ? BLEND_NORMAL : blend_.back(); }
	int MaskState() const { return static_cast<int>( maskState_ ) * 16 + stencilLevel_; }

private:
	struct ScreenVertex {
		float sx, sy;		/* canvas pixels */
		float x, y;			/* shape space, for texture coordinates */
		float color[4];
		float factor[4];
		float u, v;			/* explicit UVs (bitmaps) */
	};
	void Raster( const ScreenVertex &a, const ScreenVertex &b, const ScreenVertex &c, const Fill &fill, bool explicitUV );
	void Shade( int x, int y, const float rgba[4] );	/* premultiplied in, blended out */
	Matrix Full() const;

	int width_ = 0, height_ = 0;
	std::vector<uint8_t> pixels_;
	std::vector<uint8_t> stencil_;
	Matrix view_, matrix_, user_;
	Cxform cx_;
	int clip_[4] = { 0, 0, 0, 0 };
	std::vector<int> blend_;
	int stencilLevel_ = 0;
	enum { MASK_OFF, MASK_SUBMIT_INCR, MASK_SUBMIT_DECR, MASK_TEST } maskState_ = MASK_OFF;
	int triangles_ = 0;
};

/* Sample a texture (straight RGBA out, 0..1). */
void SampleTexture( const TextureFill &t, float u, float v, float out[4] );

/* Decode DXT1/3/5 (GFx image formats 10, 11, 12) to straight RGBA. */
bool DecodeDXT( int format, const uint8_t *data, int width, int height, std::vector<uint8_t> &rgba );

/* Image files to straight RGBA: TGA (uncompressed and run-length, 24 and
 * 32 bits) and DDS (DXT1/2/3/5 and 32-bit ARGB, the top mip level). */
bool DecodeTGA( const uint8_t *data, size_t size, Texture &t );
bool DecodeDDS( const uint8_t *data, size_t size, Texture &t );

}

#endif
