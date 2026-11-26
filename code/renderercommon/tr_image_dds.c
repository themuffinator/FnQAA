/*
===========================================================================
FnQAA: DirectDraw Surface (.dds) images.

Copyright (C) 2026 FnQAA contributors.
This file is part of FnQAA, licensed under the GNU GPL v2 or later.

Quake Arena Arcade ships its textures as DDS (docs/FORMATS.md): DXT1, DXT2,
DXT3 and DXT5 compressed, and uncompressed 32-bit ARGB. This loader decodes
the top mip level to 32-bit RGBA, as the other loaders do; the renderer
builds its own mip chain from it. An independent implementation of the
DXT (S3TC) block formats as Microsoft documents them.
===========================================================================
*/
#include "../qcommon/q_shared.h"
#include "../renderercommon/tr_public.h"

#define DDS_MAGIC			0x20534444u	/* "DDS " */
#define DDS_HEADER_SIZE		124
#define DDPF_ALPHAPIXELS	0x1u
#define DDPF_FOURCC			0x4u
#define DDPF_RGB			0x40u
#define DDS_MAX_DIMENSION	16384

static uint32_t DDS_LE32( const byte *p ) {
	return (uint32_t)p[0] | ( (uint32_t)p[1] << 8 ) | ( (uint32_t)p[2] << 16 ) | ( (uint32_t)p[3] << 24 );
}

static uint16_t DDS_LE16( const byte *p ) {
	return (uint16_t)( p[0] | ( p[1] << 8 ) );
}

static void DDS_Color565( uint16_t c, byte *rgb ) {
	const int r = ( c >> 11 ) & 31, g = ( c >> 5 ) & 63, b = c & 31;
	rgb[0] = (byte)( ( r << 3 ) | ( r >> 2 ) );
	rgb[1] = (byte)( ( g << 2 ) | ( g >> 4 ) );
	rgb[2] = (byte)( ( b << 3 ) | ( b >> 2 ) );
}

/* The 4x4 colour part every DXT format shares. */
static void DDS_ColorBlock( const byte *block, byte out[16][4], qboolean dxt1 ) {
	byte palette[4][4];
	const uint16_t c0 = DDS_LE16( block ), c1 = DDS_LE16( block + 2 );
	const uint32_t bits = DDS_LE32( block + 4 );
	int i;

	DDS_Color565( c0, palette[0] );
	DDS_Color565( c1, palette[1] );
	palette[0][3] = palette[1][3] = 255;
	if ( c0 > c1 || !dxt1 ) {
		for ( i = 0; i < 3; i++ ) {
			palette[2][i] = (byte)( ( 2 * palette[0][i] + palette[1][i] + 1 ) / 3 );
			palette[3][i] = (byte)( ( palette[0][i] + 2 * palette[1][i] + 1 ) / 3 );
		}
		palette[2][3] = palette[3][3] = 255;
	} else {
		for ( i = 0; i < 3; i++ ) {
			palette[2][i] = (byte)( ( palette[0][i] + palette[1][i] ) / 2 );
			palette[3][i] = 0;
		}
		palette[2][3] = 255;
		palette[3][3] = 0;	/* DXT1's transparent black */
	}
	for ( i = 0; i < 16; i++ ) {
		const int index = ( bits >> ( 2 * i ) ) & 3;
		out[i][0] = palette[index][0];
		out[i][1] = palette[index][1];
		out[i][2] = palette[index][2];
		out[i][3] = palette[index][3];
	}
}

/* DXT2/DXT3: 4 explicit bits of alpha per texel. */
static void DDS_ExplicitAlpha( const byte *block, byte out[16][4] ) {
	int i;
	for ( i = 0; i < 16; i++ ) {
		const int nibble = ( block[i / 2] >> ( 4 * ( i & 1 ) ) ) & 15;
		out[i][3] = (byte)( nibble * 17 );
	}
}

/* DXT4/DXT5: two endpoints and 3-bit interpolation indices. */
static void DDS_InterpolatedAlpha( const byte *block, byte out[16][4] ) {
	byte alpha[8];
	uint64_t bits = 0;
	int i;

	alpha[0] = block[0];
	alpha[1] = block[1];
	if ( alpha[0] > alpha[1] ) {
		for ( i = 1; i < 7; i++ ) {
			alpha[i + 1] = (byte)( ( ( 7 - i ) * alpha[0] + i * alpha[1] + 3 ) / 7 );
		}
	} else {
		for ( i = 1; i < 5; i++ ) {
			alpha[i + 1] = (byte)( ( ( 5 - i ) * alpha[0] + i * alpha[1] + 2 ) / 5 );
		}
		alpha[6] = 0;
		alpha[7] = 255;
	}
	for ( i = 0; i < 6; i++ ) {
		bits |= (uint64_t)block[2 + i] << ( 8 * i );
	}
	for ( i = 0; i < 16; i++ ) {
		out[i][3] = alpha[( bits >> ( 3 * i ) ) & 7];
	}
}

static int DDS_MaskShift( uint32_t mask ) {
	int shift = 0;
	if ( !mask ) {
		return 0;
	}
	while ( !( mask & 1 ) ) {
		mask >>= 1;
		shift++;
	}
	return shift;
}

static byte DDS_MaskChannel( uint32_t pixel, uint32_t mask ) {
	const int shift = DDS_MaskShift( mask );
	uint32_t max, v;
	if ( !mask ) {
		return 255;
	}
	max = mask >> shift;
	v = ( pixel & mask ) >> shift;
	return (byte)( max == 255 ? v : ( v * 255 + max / 2 ) / max );
}

/*
=================
R_LoadDDS
=================
*/
void R_LoadDDS( const char *name, byte **pic, int *width, int *height ) {
	union {
		byte *b;
		void *v;
	} buffer;
	const byte *header, *data;
	uint32_t w, h, pfFlags, fourcc, bitCount, masks[4];
	int length, blockBytes = 0, format = 0;
	size_t need;
	byte *out;
	uint32_t x, y;

	*pic = NULL;
	*width = 0;
	*height = 0;

	length = ri.FS_ReadFile( name, &buffer.v );
	if ( !buffer.b || length < 0 ) {
		return;
	}
	if ( length < 4 + DDS_HEADER_SIZE || DDS_LE32( buffer.b ) != DDS_MAGIC || DDS_LE32( buffer.b + 4 ) != DDS_HEADER_SIZE ) {
		ri.Printf( PRINT_WARNING, "LoadDDS: %s is not a DDS file\n", name );
		ri.FS_FreeFile( buffer.v );
		return;
	}
	header = buffer.b + 4;
	h = DDS_LE32( header + 8 );
	w = DDS_LE32( header + 12 );
	pfFlags = DDS_LE32( header + 76 );
	fourcc = DDS_LE32( header + 80 );
	bitCount = DDS_LE32( header + 84 );
	masks[0] = DDS_LE32( header + 88 );
	masks[1] = DDS_LE32( header + 92 );
	masks[2] = DDS_LE32( header + 96 );
	masks[3] = ( pfFlags & DDPF_ALPHAPIXELS ) ? DDS_LE32( header + 100 ) : 0;
	data = header + DDS_HEADER_SIZE;

	if ( w == 0 || h == 0 || w > DDS_MAX_DIMENSION || h > DDS_MAX_DIMENSION ) {
		ri.Printf( PRINT_WARNING, "LoadDDS: %s has bad dimensions %ux%u\n", name, w, h );
		ri.FS_FreeFile( buffer.v );
		return;
	}

	if ( pfFlags & DDPF_FOURCC ) {
		switch ( fourcc ) {
		case 0x31545844u: format = 1; blockBytes = 8; break;	/* DXT1 */
		case 0x32545844u: format = 2; blockBytes = 16; break;	/* DXT2 */
		case 0x33545844u: format = 3; blockBytes = 16; break;	/* DXT3 */
		case 0x34545844u: format = 4; blockBytes = 16; break;	/* DXT4 */
		case 0x35545844u: format = 5; blockBytes = 16; break;	/* DXT5 */
		default:
			ri.Printf( PRINT_WARNING, "LoadDDS: %s uses an unsupported format %08X\n", name, fourcc );
			ri.FS_FreeFile( buffer.v );
			return;
		}
		need = (size_t)( ( w + 3 ) / 4 ) * ( ( h + 3 ) / 4 ) * blockBytes;
	} else if ( ( pfFlags & DDPF_RGB ) && ( bitCount == 32 || bitCount == 24 ) ) {
		need = (size_t)w * h * ( bitCount / 8 );
	} else {
		ri.Printf( PRINT_WARNING, "LoadDDS: %s uses an unsupported pixel format\n", name );
		ri.FS_FreeFile( buffer.v );
		return;
	}
	if ( need > (size_t)length - 4 - DDS_HEADER_SIZE ) {
		ri.Printf( PRINT_WARNING, "LoadDDS: %s is truncated\n", name );
		ri.FS_FreeFile( buffer.v );
		return;
	}

	out = ri.Malloc( (int)( w * h * 4 ) );

	if ( format ) {
		const uint32_t bw = ( w + 3 ) / 4, bh = ( h + 3 ) / 4;
		for ( y = 0; y < bh; y++ ) {
			for ( x = 0; x < bw; x++ ) {
				const byte *block = data + ( (size_t)y * bw + x ) * blockBytes;
				byte texels[16][4];
				int i;
				if ( format == 1 ) {
					DDS_ColorBlock( block, texels, qtrue );
				} else {
					DDS_ColorBlock( block + 8, texels, qfalse );
					if ( format <= 3 ) {
						DDS_ExplicitAlpha( block, texels );
					} else {
						DDS_InterpolatedAlpha( block, texels );
					}
					if ( format == 2 || format == 4 ) {
						/* premultiplied: undo it, as the renderer blends straight alpha */
						for ( i = 0; i < 16; i++ ) {
							const int a = texels[i][3];
							int c;
							for ( c = 0; c < 3 && a; c++ ) {
								const int v = texels[i][c] * 255 / a;
								texels[i][c] = (byte)( v > 255 ? 255 : v );
							}
						}
					}
				}
				for ( i = 0; i < 16; i++ ) {
					const uint32_t px = x * 4 + ( i & 3 ), py = y * 4 + ( i >> 2 );
					if ( px < w && py < h ) {
						byte *dst = out + ( (size_t)py * w + px ) * 4;
						dst[0] = texels[i][0];
						dst[1] = texels[i][1];
						dst[2] = texels[i][2];
						dst[3] = texels[i][3];
					}
				}
			}
		}
	} else {
		const uint32_t step = bitCount / 8;
		for ( y = 0; y < h; y++ ) {
			for ( x = 0; x < w; x++ ) {
				const byte *src = data + ( (size_t)y * w + x ) * step;
				const uint32_t pixel = step == 4 ? DDS_LE32( src ) : ( (uint32_t)src[0] | ( (uint32_t)src[1] << 8 ) | ( (uint32_t)src[2] << 16 ) );
				byte *dst = out + ( (size_t)y * w + x ) * 4;
				dst[0] = DDS_MaskChannel( pixel, masks[0] );
				dst[1] = DDS_MaskChannel( pixel, masks[1] );
				dst[2] = DDS_MaskChannel( pixel, masks[2] );
				dst[3] = DDS_MaskChannel( pixel, masks[3] );
			}
		}
	}

	ri.FS_FreeFile( buffer.v );
	*pic = out;
	*width = (int)w;
	*height = (int)h;
}
