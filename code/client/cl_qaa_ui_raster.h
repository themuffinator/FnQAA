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
	/* Changes whenever the texels do (the owner sets it, from a counter
	 * that never repeats): a triangle records it, so a band of the canvas
	 * that samples a changed texture is drawn again. */
	uint64_t version = 0;
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

/*
 * Drawing is recorded: each call resolves its triangles against the current
 * state (matrices, colour transform, blend mode, mask, clip) and queues
 * them. Flush() rasterises the queue in bands of rows on worker threads;
 * each band runs the queued triangles that reach it in order over its own
 * rows, so the result is the same as drawing one triangle at a time on one
 * thread. Pixels(), ToStraight() and Flush() all flush first. Textures a
 * queued triangle samples must stay unchanged until the next flush.
 *
 * Bands are kept between frames: a frame flushed in one go after Clear()
 * leaves a band's pixels alone when the triangles that reach it are the
 * same as last frame's (positions, fills, state and texture versions), so
 * a frame that repeats, or animates a small part of the screen, redraws
 * only what changed.
 */
class Canvas {
public:
	void Resize( int width, int height );
	int Width() const { return width_; }
	int Height() const { return height_; }
	/* premultiplied RGBA, row-major, top row first */
	const uint8_t *Pixels() const { Flush(); return pixels_.data(); }
	/* straight RGBA, for presenting with ordinary alpha blending (flushes,
	 * converting each band as it finishes) */
	void ToStraight( std::vector<uint8_t> &out ) const;
	/* Rasterise everything queued. */
	void Flush() const { Flush( nullptr ); }
	/* Triangles are queued and not yet rasterised. */
	bool HasQueued() const { return !ops_.empty(); }
	/* The rows the last ToStraight() wrote into the buffer it was given, as
	 * ranges [y0, y1) in pairs, top to bottom; the rest of it is as before. */
	const std::vector<int> &ChangedRows() const { return changed_; }
	/* Rows rasterised since Clear(), as of the last flush (0: the frame
	 * repeated the last one), for profiling. */
	int RowsDrawn() const { return rowsDrawn_; }

	/* Start a frame: the canvas clears to transparent at the next flush. */
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
	/* pixels the rasteriser tested since Clear(), as of the last flush, for profiling */
	int64_t PixelsTested() const { return pixelsTested_; }
	/* for tracing: the current movie-to-pixel transform, blend mode and mask state */
	Matrix FullMatrix() const { return Full(); }
	int BlendMode() const { return blend_.empty() ? BLEND_NORMAL : blend_.back(); }
	int MaskState() const { return static_cast<int>( maskState_ ) * 16 + stencilLevel_; }

	/* ---- internal: the queue Flush() runs ---- */
	/* A value interpolated over a triangle, as a plane in canvas pixels:
	 * value(px, py) = dx * px + dy * py + c. */
	struct Plane {
		float dx = 0, dy = 0, c = 0;
		float At( float px, float py ) const { return dx * px + dy * py + c; }
	};
	struct TexRef {
		const Texture *texture = nullptr;
		bool clamp = false, linear = true;
	};
	/* A queued triangle (or a stencil clear), with everything the
	 * rasteriser needs resolved when it was drawn. */
	struct Op {
		uint8_t type = 0;		/* OP_* */
		uint8_t shade = 0;		/* SHADE_* */
		uint8_t mode = 0;		/* blend mode */
		uint8_t mask = 0;		/* MASK_* */
		uint8_t level = 0;		/* stencil level */
		bool tl[3] = {};		/* top-left edges */
		int x0 = 0, y0 = 0, x1 = 0, y1 = 0;	/* clipped bounds, [x0, x1) x [y0, y1) */
		float sx[3] = {}, sy[3] = {};
		Plane col[4], facLerp, facAlpha, u0, v0, u1, v1;
		float color[4] = {};
		TexRef tex0, tex1;
		uint64_t version0 = 0, version1 = 0;	/* the textures' versions */
		Cxform cx;
	};
	/* Rows per band: what a flush splits the canvas into, and the unit a
	 * frame that repeats in part redraws. */
	static constexpr int BAND_ROWS = 16;

private:
	struct ScreenVertex {
		float sx, sy;		/* canvas pixels */
		float x, y;			/* shape space, for texture coordinates */
		float color[4];
		float factor[4];
		float u, v;			/* explicit UVs (bitmaps) */
	};
	void Raster( const ScreenVertex &a, const ScreenVertex &b, const ScreenVertex &c, const Fill &fill, bool explicitUV );
	Op &NewOp();
	void Flush( std::vector<uint8_t> *straight ) const;
	struct BandResult {
		int64_t tested = 0;
		int rows = 0;				/* rows rasterised */
		std::vector<int> changed;	/* straight rows written, [y0, y1) pairs */
	};
	void RunBand( int band, bool raster, bool clear, std::vector<uint8_t> *straight, bool sameStraight,
		BandResult *out ) const;
	Matrix Full() const;

	int width_ = 0, height_ = 0;
	mutable std::vector<uint8_t> pixels_;
	mutable std::vector<uint8_t> stencil_;
	mutable std::vector<Op> ops_;
	mutable bool clearPending_ = false;
	/* Per row: the row of pixels_ is all transparent (no triangle has
	 * touched it since a clear), and the row of the last straight copy is
	 * all zero. Rows that stay empty are neither cleared nor converted
	 * again: in a game most of the screen is. */
	mutable std::vector<uint8_t> rowBlank_, straightBlank_;
	mutable const uint8_t *lastStraight_ = nullptr;
	mutable size_t lastStraightSize_ = 0;
	mutable std::vector<int> changed_;
	/* Per band: the hash of the triangles that drew it, valid while
	 * bandsValid_ (whole frames, flushed in one go, since the last resize);
	 * and whether its pixels changed since the last straight copy. */
	mutable std::vector<uint64_t> bandHash_;
	mutable std::vector<uint8_t> bandStale_;
	mutable bool bandsValid_ = false;
	mutable std::vector<std::vector<uint32_t>> bandOps_;	/* the queued triangles reaching each band */
	mutable int rowsDrawn_ = 0;
	Matrix view_, matrix_, user_;
	Cxform cx_;
	int clip_[4] = { 0, 0, 0, 0 };
	std::vector<int> blend_;
	int stencilLevel_ = 0;
	enum { MASK_OFF, MASK_SUBMIT_INCR, MASK_SUBMIT_DECR, MASK_TEST } maskState_ = MASK_OFF;
	int triangles_ = 0;
	mutable int64_t pixelsTested_ = 0;
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
