/*
 * The Scaleform front end's software rasteriser and image decoders
 * (code/client/cl_qaa_ui_raster.cpp) on synthetic data: fill coverage and
 * the top-left rule, colour transforms, blending, stencil masks, texture
 * sampling, bitmap glyphs, and the DXT, DDS and TGA decoders.
 */
#include "cl_qaa_ui_raster.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

using namespace qaa_ui;

namespace {

int failures;

void Check( bool ok, const char *what ) {
	if ( !ok ) {
		std::printf( "FAIL: %s\n", what );
		failures++;
	}
}

const uint8_t *Px( const Canvas &c, int x, int y ) {
	return c.Pixels() + ( static_cast<size_t>( y ) * c.Width() + x ) * 4;
}

Vertex V( float x, float y ) {
	Vertex v{};
	v.x = x;
	v.y = y;
	for ( int k = 0; k < 4; k++ ) {
		v.color[k] = 1.0f;
		v.factor[k] = 1.0f;
	}
	return v;
}

Fill Solid( float r, float g, float b, float a ) {
	Fill f;
	f.kind = FILL_COLOR;
	f.color[0] = r;
	f.color[1] = g;
	f.color[2] = b;
	f.color[3] = a;
	return f;
}

Canvas Fresh( int w, int h ) {
	Canvas c;
	c.Resize( w, h );
	c.Clear();
	c.SetView( Matrix(), 0, 0, w, h );
	return c;
}

/* A w x h rectangle as two triangles sharing the diagonal. */
void Rect( Canvas &c, float x0, float y0, float x1, float y1, const Fill &f ) {
	const Vertex v[4] = { V( x0, y0 ), V( x1, y0 ), V( x1, y1 ), V( x0, y1 ) };
	const uint32_t idx[6] = { 0, 1, 2, 0, 2, 3 };
	c.DrawTriangles( v, idx, 2, f );
}

void TestCoverageAndTopLeft() {
	Canvas c = Fresh( 8, 8 );
	// half-opaque, so a pixel the shared diagonal covered twice would show
	Rect( c, 2, 2, 6, 6, Solid( 1, 0, 0, 0.5f ) );
	int covered = 0, wrong = 0;
	for ( int y = 0; y < 8; y++ ) {
		for ( int x = 0; x < 8; x++ ) {
			const uint8_t *p = Px( c, x, y );
			const bool inside = x >= 2 && x < 6 && y >= 2 && y < 6;
			if ( inside ) {
				covered++;
				wrong += p[3] != 128 || p[0] != 128 || p[1] != 0;
			} else {
				wrong += p[3] != 0;
			}
		}
	}
	Check( covered == 16 && wrong == 0, "a 4x4 rectangle covers exactly its 16 pixels, each once" );
}

void TestCxformAndStraight() {
	Canvas c = Fresh( 4, 4 );
	Cxform cx;
	cx.mul[0] = 0.0f;			// red off
	cx.add[2] = 1.0f;			// blue on
	c.SetCxform( cx );
	Rect( c, 0, 0, 4, 4, Solid( 1, 1, 0, 1 ) );
	const uint8_t *p = Px( c, 1, 1 );
	Check( p[0] == 0 && p[1] == 255 && p[2] == 255 && p[3] == 255, "the colour transform multiplies and adds" );

	Canvas h = Fresh( 2, 2 );
	Rect( h, 0, 0, 2, 2, Solid( 1, 0.5f, 0, 0.5f ) );
	std::vector<uint8_t> straight;
	h.ToStraight( straight );
	Check( straight.size() == 16 && straight[0] >= 254 && straight[1] >= 126 && straight[1] <= 129 && straight[3] == 128,
		"premultiplied pixels convert back to straight colour" );
}

void TestBlendAdd() {
	Canvas c = Fresh( 2, 2 );
	Rect( c, 0, 0, 2, 2, Solid( 0.25f, 0, 0, 1 ) );
	c.PushBlend( BLEND_ADD );
	Rect( c, 0, 0, 2, 2, Solid( 0.5f, 0, 0, 1 ) );
	c.PopBlend();
	const uint8_t *p = Px( c, 0, 0 );
	Check( p[0] >= 190 && p[0] <= 192, "the add blend sums colour" );
	Check( c.BlendMode() == BLEND_NORMAL, "popping the blend restores normal" );
}

void TestMask() {
	Canvas c = Fresh( 8, 8 );
	c.BeginSubmitMask( 0 );
	Rect( c, 0, 0, 4, 8, Fill() );	// the mask shape: the left half
	c.EndSubmitMask();
	Rect( c, 0, 0, 8, 8, Solid( 0, 1, 0, 1 ) );
	Check( Px( c, 1, 4 )[1] == 255 && Px( c, 6, 4 )[3] == 0, "a stencil mask clips to its shape" );
	c.DisableMask();
	Rect( c, 0, 0, 8, 8, Solid( 0, 0, 1, 1 ) );
	Check( Px( c, 6, 4 )[2] == 255, "disabling the mask draws everywhere" );
}

void TestClip() {
	Canvas c = Fresh( 8, 8 );
	c.SetView( Matrix(), 0, 0, 4, 4 );
	Rect( c, 0, 0, 8, 8, Solid( 1, 1, 1, 1 ) );
	Check( Px( c, 3, 3 )[3] == 255 && Px( c, 4, 4 )[3] == 0, "the view's clip rectangle bounds drawing" );
}

void TestTextureFill() {
	// a 2x2 texture: red, green / blue, white; point-sampled over 4x4 pixels
	Texture t;
	t.width = t.height = 2;
	t.rgba = { 255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 255, 255, 255, 255 };
	Fill f;
	f.kind = FILL_TEXTURE;
	f.tex[0].texture = &t;
	f.tex[0].linear = false;
	f.tex[0].clamp = true;
	f.tex[0].uv.m[0] = 0.25f;	// shape units 0..4 to u 0..1
	f.tex[0].uv.m[4] = 0.25f;
	f.texCount = 1;
	Canvas c = Fresh( 4, 4 );
	Rect( c, 0, 0, 4, 4, f );
	Check( Px( c, 0, 0 )[0] == 255 && Px( c, 3, 0 )[1] == 255 && Px( c, 0, 3 )[2] == 255 && Px( c, 3, 3 )[0] == 255
		&& Px( c, 3, 3 )[2] == 255, "a texture fill maps shape space through its matrix" );
}

void TestGouraudAA() {
	Fill f;
	f.kind = FILL_GOURAUD;
	f.gouraud = GFILL_COLOR;
	Vertex v[3] = { V( 0, 0 ), V( 8, 0 ), V( 0, 8 ) };
	for ( Vertex &x : v ) {
		x.factor[3] = 0.5f;	// edge anti-aliasing factor
	}
	const uint32_t idx[3] = { 0, 1, 2 };
	Canvas c = Fresh( 8, 8 );
	c.DrawTriangles( v, idx, 1, f );
	Check( Px( c, 1, 1 )[3] == 128, "a gouraud fill multiplies alpha by factor.a" );
}

void TestBitmapGlyph() {
	Texture glyph;
	glyph.width = glyph.height = 1;
	glyph.alphaOnly = true;
	glyph.rgba = { 255, 255, 255, 64 };
	BitmapQuad q{};
	q.x1 = q.y1 = 2;
	q.u1 = q.v1 = 1;
	q.color[0] = 1;
	q.color[3] = 1;
	Canvas c = Fresh( 2, 2 );
	c.DrawBitmaps( &q, 1, Matrix(), &glyph );
	const uint8_t *p = Px( c, 0, 0 );
	Check( p[3] == 64 && p[0] == 64 && p[1] == 0, "a glyph takes the quad's colour and the texture's alpha" );
}

void TestMatrix() {
	Matrix a, b;
	a.m[0] = 2;		// scale x by 2
	b.m[2] = 3;		// translate x by 3
	float x, y;
	Matrix::Mul( a, b ).Apply( 1, 0, &x, &y );
	Check( x == 8 && y == 0, "Mul(a, b) applies b, then a" );
}

/* One 4x4 DXT1 block: colour 0 pure red, colour 1 pure blue, every index 0. */
void TestDXT() {
	const uint8_t dxt1[8] = { 0x00, 0xF8, 0x1F, 0x00, 0, 0, 0, 0 };
	std::vector<uint8_t> out;
	Check( DecodeDXT( 10, dxt1, 4, 4, out ) && out.size() == 64 && out[0] == 255 && out[2] == 0 && out[3] == 255,
		"DXT1 decodes its first palette colour" );
	// DXT1 with c0 <= c1 and index 3: transparent black
	const uint8_t dxt1a[8] = { 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
	Check( DecodeDXT( 10, dxt1a, 4, 4, out ) && out[3] == 0, "DXT1's fourth colour is transparent when c0 <= c1" );
	// DXT5: alpha endpoints 255 and 0, every alpha index 1 (the second endpoint)
	uint8_t dxt5[16] = { 255, 0, 0x49, 0x92, 0x24, 0x49, 0x92, 0x24, 0x00, 0xF8, 0x00, 0xF8, 0, 0, 0, 0 };
	Check( DecodeDXT( 12, dxt5, 4, 4, out ) && out[3] == 0 && out[0] == 255, "DXT5 interpolated alpha" );
}

std::vector<uint8_t> DdsHeader( int w, int h, uint32_t pfFlags, const char *fourcc, uint32_t bits ) {
	std::vector<uint8_t> d( 128, 0 );
	auto put = [&]( size_t at, uint32_t v ) {
		for ( int k = 0; k < 4; k++ ) {
			d[at + k] = static_cast<uint8_t>( v >> ( 8 * k ) );
		}
	};
	std::memcpy( d.data(), "DDS ", 4 );
	put( 4, 124 );
	put( 12, static_cast<uint32_t>( h ) );
	put( 16, static_cast<uint32_t>( w ) );
	put( 76, 32 );
	put( 80, pfFlags );
	if ( fourcc ) {
		std::memcpy( &d[84], fourcc, 4 );
	}
	put( 88, bits );
	if ( !fourcc ) {
		put( 92, 0x00FF0000u );
		put( 96, 0x0000FF00u );
		put( 100, 0x000000FFu );
		put( 104, 0xFF000000u );
	}
	return d;
}

void TestDDS() {
	std::vector<uint8_t> dxt3 = DdsHeader( 4, 4, 0x4, "DXT3", 0 );
	const uint8_t block[16] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xE0, 0x07, 0xE0, 0x07, 0, 0, 0, 0 };
	dxt3.insert( dxt3.end(), block, block + 16 );
	Texture t;
	Check( DecodeDDS( dxt3.data(), dxt3.size(), t ) && t.width == 4 && t.height == 4 && t.rgba[1] == 255
		&& t.rgba[0] == 0 && t.rgba[3] == 255, "a DXT3 DDS decodes (green, opaque)" );
	Check( !DecodeDDS( dxt3.data(), dxt3.size() - 1, t ), "a truncated DDS is refused" );

	std::vector<uint8_t> argb = DdsHeader( 1, 1, 0x41, nullptr, 32 );
	const uint8_t px[4] = { 0x10, 0x20, 0x30, 0x80 };	// B, G, R, A
	argb.insert( argb.end(), px, px + 4 );
	Check( DecodeDDS( argb.data(), argb.size(), t ) && t.rgba[0] == 0x30 && t.rgba[1] == 0x20 && t.rgba[2] == 0x10
		&& t.rgba[3] == 0x80, "a 32-bit ARGB DDS decodes to RGBA" );

	std::vector<uint8_t> dxt2 = DdsHeader( 4, 4, 0x4, "DXT2", 0 );
	// alpha 8 of 15 (0x88 nibbles) over premultiplied white 565 (0xFFFF)
	const uint8_t pm[16] = { 0x88, 0x88, 0x88, 0x88, 0x88, 0x88, 0x88, 0x88, 0x10, 0x84, 0x10, 0x84, 0, 0, 0, 0 };
	dxt2.insert( dxt2.end(), pm, pm + 16 );
	Check( DecodeDDS( dxt2.data(), dxt2.size(), t ) && t.rgba[3] == 0x88 && t.rgba[0] >= 0xEE,
		"DXT2's premultiplied colour is made straight" );
}

void TestTGA() {
	// 2x1, 24-bit, bottom-up, uncompressed: BGR pixels
	const uint8_t tga[] = { 0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2, 0, 1, 0, 24, 0, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06 };
	Texture t;
	Check( DecodeTGA( tga, sizeof( tga ), t ) && t.width == 2 && t.rgba[0] == 3 && t.rgba[2] == 1 && t.rgba[3] == 255
		&& t.rgba[4] == 6, "an uncompressed TGA decodes to RGBA" );
	// run-length: one packet of 2 repeated 32-bit pixels, top-down
	const uint8_t rle[] = { 0, 0, 10, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2, 0, 1, 0, 32, 0x20, 0x81, 9, 8, 7, 6 };
	Check( DecodeTGA( rle, sizeof( rle ), t ) && t.rgba[0] == 7 && t.rgba[3] == 6 && t.rgba[4] == 7,
		"a run-length TGA decodes" );
	Check( !DecodeTGA( rle, 10, t ), "a truncated TGA is refused" );
}

/* A canvas big enough to flush in bands of rows on the worker threads: a
 * half-opaque rectangle split along its diagonal covers every pixel exactly
 * once, a mask drawn first still clips what follows, and the bands' straight
 * copy matches the premultiplied pixels. */
void TestBands() {
	const int w = 640, h = 480;
	Canvas c = Fresh( w, h );
	c.BeginSubmitMask( 0 );
	Rect( c, 0, 0, 320, 480, Fill() );	// the mask: the left half
	c.EndSubmitMask();
	Rect( c, 0, 0, 640, 480, Solid( 1, 0, 0, 0.5f ) );
	c.DisableMask();
	const Vertex v[4] = { V( 0, 0 ), V( 640, 0 ), V( 640, 480 ), V( 0, 480 ) };
	const uint32_t idx[6] = { 0, 1, 2, 0, 2, 3 };
	c.DrawTriangles( v, idx, 2, Solid( 0, 0, 1, 0.5f ) );
	std::vector<uint8_t> straight;
	c.ToStraight( straight );
	int wrong = 0;
	for ( int y = 0; y < h; y++ ) {
		for ( int x = 0; x < w; x++ ) {
			const uint8_t *p = Px( c, x, y );
			// left: red at half, then blue at half over it; right: blue only
			const bool left = x < 320;
			const int a = left ? 192 : 128, r = left ? 64 : 0;
			wrong += p[3] != a || p[0] != r || p[2] != 128 || straight[( static_cast<size_t>( y ) * w + x ) * 4 + 3] != a;
		}
	}
	Check( wrong == 0, "banded flush: each pixel once, masks and order kept across bands" );

	// the same frame again: no band is drawn or converted
	auto drawFrame = [&]( bool extra ) {
		c.Clear();
		c.BeginSubmitMask( 0 );
		Rect( c, 0, 0, 320, 480, Fill() );
		c.EndSubmitMask();
		Rect( c, 0, 0, 640, 480, Solid( 1, 0, 0, 0.5f ) );
		c.DisableMask();
		c.DrawTriangles( v, idx, 2, Solid( 0, 0, 1, 0.5f ) );
		if ( extra ) {
			Rect( c, 100, 200, 110, 210, Solid( 0, 1, 0, 1 ) );
		}
	};
	drawFrame( false );
	c.ToStraight( straight );
	Check( c.RowsDrawn() == 0 && c.ChangedRows().empty() && Px( c, 10, 10 )[3] == 192,
		"a frame that repeats draws and converts nothing" );
	// one small change: only the bands it reaches
	drawFrame( true );
	c.ToStraight( straight );
	const std::vector<int> &one = c.ChangedRows();
	Check( c.RowsDrawn() == 2 * Canvas::BAND_ROWS && one.size() == 2 && one[0] == 192 && one[1] == 224
		&& Px( c, 105, 205 )[1] == 255 && Px( c, 105, 220 )[3] == 192 && Px( c, 400, 205 )[2] == 128
		&& straight[( 205 * w + 105 ) * 4 + 1] == 255, "a change redraws only the bands it reaches" );
	drawFrame( false );
	c.ToStraight( straight );
	Check( c.RowsDrawn() == 2 * Canvas::BAND_ROWS && Px( c, 105, 205 )[1] == 0 && Px( c, 105, 205 )[3] == 192
		&& straight[( 205 * w + 105 ) * 4 + 1] == 0, "and taking it away redraws them again" );
	// a frame flushed in two parts is not taken for one that repeats its first
	c.Clear();
	Rect( c, 0, 0, 640, 480, Solid( 1, 1, 1, 1 ) );
	c.Flush();
	Rect( c, 0, 0, 640, 480, Solid( 0, 0, 0, 1 ) );
	c.ToStraight( straight );
	c.Clear();
	Rect( c, 0, 0, 640, 480, Solid( 1, 1, 1, 1 ) );
	c.ToStraight( straight );
	Check( Px( c, 300, 300 )[0] == 255 && straight[( 300 * w + 300 ) * 4] == 255,
		"a frame flushed in parts is drawn again in full" );
	// a texture's new version redraws what samples it
	Texture tex;
	tex.width = tex.height = 1;
	tex.rgba = { 255, 0, 0, 255 };
	tex.version = 1;
	Fill tf;
	tf.kind = FILL_TEXTURE;
	tf.texCount = 1;
	tf.tex[0].texture = &tex;
	tf.tex[0].linear = false;
	tf.tex[0].uv.m[0] = 1.0f / 640;
	tf.tex[0].uv.m[4] = 1.0f / 480;
	c.Clear();
	Rect( c, 0, 0, 640, 480, tf );
	c.ToStraight( straight );
	tex.rgba = { 0, 255, 0, 255 };
	tex.version = 2;
	c.Clear();
	Rect( c, 0, 0, 640, 480, tf );
	c.ToStraight( straight );
	Check( Px( c, 50, 50 )[1] == 255 && Px( c, 50, 50 )[0] == 0 && straight[( 50 * w + 50 ) * 4 + 1] == 255,
		"a texture's new version redraws the bands that sample it" );
	// a straight copy into another buffer gets every row
	std::vector<uint8_t> other;
	c.ToStraight( other );
	Check( other.size() == straight.size() && other[( 50 * w + 50 ) * 4 + 1] == 255
		&& other[( 470 * w + 600 ) * 4 + 1] == 255, "a copy into a new buffer converts every band" );

	// a HUD-like strip: only its rows are rewritten in the same straight buffer
	auto span = [&c]( int *y0, int *y1 ) {
		const std::vector<int> &r = c.ChangedRows();
		*y0 = r.size() == 2 ? r[0] : -1;
		*y1 = r.size() == 2 ? r[1] : -1;
		return static_cast<int>( r.size() / 2 );
	};
	int y0, y1;
	c.Clear();
	Rect( c, 0, 400, 640, 440, Solid( 1, 1, 1, 1 ) );
	c.ToStraight( straight );
	c.Clear();
	Rect( c, 0, 400, 640, 440, Solid( 1, 0, 0, 1 ) );
	c.ToStraight( straight );
	span( &y0, &y1 );
	Check( y0 == 400 && y1 == 440 && straight[( 420 * w ) * 4] == 255 && straight[( 420 * w ) * 4 + 1] == 0
		&& straight[( 100 * w ) * 4 + 3] == 0, "only the rows a frame draws are converted again" );
	c.Clear();
	c.ToStraight( straight );
	span( &y0, &y1 );
	Check( y0 == 400 && y1 == 440 && straight[( 420 * w ) * 4 + 3] == 0 && Px( c, 5, 420 )[3] == 0,
		"rows drawn last frame are cleared when nothing covers them" );
	c.Clear();
	c.ToStraight( straight );
	Check( span( &y0, &y1 ) == 0, "an empty frame after an empty frame changes no rows" );

	// two strips far apart are two ranges, not one covering the gap
	c.Clear();
	Rect( c, 0, 10, 640, 20, Solid( 1, 1, 1, 1 ) );
	Rect( c, 0, 400, 640, 440, Solid( 1, 1, 1, 1 ) );
	c.ToStraight( straight );
	const std::vector<int> &r = c.ChangedRows();
	Check( r.size() == 4 && r[0] == 10 && r[1] == 20 && r[2] == 400 && r[3] == 440, "separate strips upload separately" );
}

/* Changes that cannot show keep their bands: a shape animating outside its
 * mask is not even queued, and one animating under an opaque layer (a
 * linearly sampled opaque texture, whose alpha can fall a hair short of 1)
 * leaves its bands as they are. A change on top redraws only its bands,
 * from the checkpoint before it. Every frame matches a full redraw of the
 * same triangles, masked-out ones included (SetVerify). */
void TestHiddenChanges() {
	const int w = 640, h = 480;
	Texture grad;
	grad.width = 256;
	grad.height = 1;
	grad.rgba.resize( 256 * 4 );
	for ( int i = 0; i < 256; i++ ) {
		grad.rgba[i * 4 + 0] = static_cast<uint8_t>( i );
		grad.rgba[i * 4 + 1] = static_cast<uint8_t>( 40 + i / 2 );
		grad.rgba[i * 4 + 2] = static_cast<uint8_t>( 255 - i );
		grad.rgba[i * 4 + 3] = 255;
	}
	grad.version = 1;
	Fill layer;
	layer.kind = FILL_TEXTURE;
	layer.texCount = 1;
	layer.tex[0].texture = &grad;
	layer.tex[0].clamp = true;
	layer.tex[0].linear = true;
	layer.tex[0].uv.m[0] = 1.0f / 390.0f;
	layer.tex[0].uv.m[2] = -250.0f / 390.0f;
	layer.tex[0].uv.m[4] = 1.0f / h;
	Canvas c;
	c.Resize( w, h );
	std::vector<uint8_t> straight;
	float layerLeft = 250, maskLeft = 300;
	auto frame = [&]( float t ) {
		c.SetVerify( true );
		c.Clear();
		c.SetView( Matrix(), 0, 0, w, h );
		Cxform fade;
		fade.mul[3] = t;
		// underneath: a mask over the right, and two shapes fading under it
		c.BeginSubmitMask( 0 );
		Rect( c, maskLeft, 0, 640, 480, Fill() );
		c.EndSubmitMask();
		c.SetCxform( fade );
		Rect( c, 10, 400, 200, 440, Solid( 1, 0, 0, 1 ) );		// outside the mask
		Rect( c, 320, 100, 600, 140, Solid( 0, 1, 0, 1 ) );	// inside it
		c.DisableMask();
		c.SetCxform( Cxform() );
		// an opaque layer over the right
		Rect( c, layerLeft, 0, 640, 480, layer );
		// on it, a shape fading and a still one over that
		c.SetCxform( fade );
		Rect( c, 260, 200, 500, 236, Solid( 1, 1, 1, 1 ) );
		c.SetCxform( Cxform() );
		Rect( c, 280, 210, 620, 226, Solid( 0, 0, 1, 0.5f ) );
		c.ToStraight( straight );
	};
	frame( 0.25f );
	Check( c.Triangles() == 10, "a shape outside its mask is not queued" );
	frame( 0.5f );
	int drawn = 0, kept = 0;
	for ( int i = 0; i < 6; i++ ) {
		frame( 0.3f + 0.1f * i );
		drawn += c.RowsDrawn();
		kept += c.RowsKept();
	}
	Check( c.VerifiedFlushes() == 8 && c.VerifyMismatches() == 0, "hidden changes: every frame matches a full redraw" );
	Check( drawn == 6 * 3 * Canvas::BAND_ROWS, "only the bands of the change on top are drawn" );
	Check( kept == 6 * 3 * Canvas::BAND_ROWS, "the bands of the change under the opaque layer are kept" );
	// what is under the change on top moves, then the mask: nothing reused
	// may show the old state
	for ( int i = 0; i < 6; i++ ) {
		layerLeft = i == 1 || i == 2 ? 251.5f : 250.0f;
		maskLeft = i == 4 ? 330.0f : 300.0f;
		frame( 0.35f + 0.1f * i );
	}
	Check( c.VerifiedFlushes() == 14 && c.VerifyMismatches() == 0, "changes under a change: every frame matches a full redraw" );
}

}

int main() {
	TestCoverageAndTopLeft();
	TestBands();
	TestHiddenChanges();
	TestCxformAndStraight();
	TestBlendAdd();
	TestMask();
	TestClip();
	TestTextureFill();
	TestGouraudAA();
	TestBitmapGlyph();
	TestMatrix();
	TestDXT();
	TestDDS();
	TestTGA();
	if ( failures ) {
		std::printf( "%d failure(s)\n", failures );
		return 1;
	}
	std::printf( "qaa_ui_raster: all passed\n" );
	return 0;
}
