/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The compressed texture formats of OpenGL ES 3.0: ETC2 and EAC (the ten
 * formats every OpenGL ES 3.0 implementation has).  A compressed level is
 * decoded on the CPU when it is given and kept as uncompressed texels:
 * the RGB and RGBA formats as RGBA8 (sRGB ones as SRGB8_ALPHA8), the
 * 11-bit red and red-green ones as half floats.  The level keeps the
 * compressed internal format, which the API reports.
 *
 * The block layouts and the decoding follow the Khronos Data Format
 * Specification (section "ETC2 Compressed Texture Image Formats", the
 * OpenGL ES 3.0 specification's annex C): a block is 4x4 texels, its
 * texels numbered down each column (x * 4 + y), its 64-bit words big
 * endian.
 */

#include "gles.h"

#include <string.h>

/* The texels of a block's side, and the bytes of an ETC2 RGB or an EAC block. */
#define ETC2_BLOCK		4
#define ETC2_BLOCK_BYTES	8U

/* The kinds of block a format is made of. */
#define ETC2_KIND_RGB		0U
#define ETC2_KIND_PUNCHTHROUGH	1U
#define ETC2_KIND_RGBA		2U
#define ETC2_KIND_R11		3U
#define ETC2_KIND_RG11		4U

/*
 * The compressed formats with their kept forms (gles_format; the static
 * entries never change) and the kind of block and signedness of each.
 */
struct etc2_format {
	struct gles_format format;
	unsigned kind;
	int is_signed;
};

/* The ten formats. */
static const struct etc2_format etc2_formats[] = {
	{ { GL_COMPRESSED_R11_EAC, GL_RED, VK_FORMAT_R16_SFLOAT, 2U, 1U, GLES_TEXEL_FLOAT, 1, 0, 0, 0 }, ETC2_KIND_R11, 0 },
	{ { GL_COMPRESSED_SIGNED_R11_EAC, GL_RED, VK_FORMAT_R16_SFLOAT, 2U, 1U, GLES_TEXEL_FLOAT, 1, 0, 0, 0 }, ETC2_KIND_R11, 1 },
	{ { GL_COMPRESSED_RG11_EAC, GL_RG, VK_FORMAT_R16G16_SFLOAT, 4U, 2U, GLES_TEXEL_FLOAT, 1, 0, 0, 0 }, ETC2_KIND_RG11, 0 },
	{ { GL_COMPRESSED_SIGNED_RG11_EAC, GL_RG, VK_FORMAT_R16G16_SFLOAT, 4U, 2U, GLES_TEXEL_FLOAT, 1, 0, 0, 0 }, ETC2_KIND_RG11, 1 },
	{ { GL_COMPRESSED_RGB8_ETC2, GL_RGB, VK_FORMAT_R8G8B8A8_UNORM, 4U, 4U, GLES_TEXEL_NORM, 1, 0, 0, 0 }, ETC2_KIND_RGB, 0 },
	{ { GL_COMPRESSED_SRGB8_ETC2, GL_RGB, VK_FORMAT_R8G8B8A8_SRGB, 4U, 4U, GLES_TEXEL_NORM, 1, 0, 0, 0 }, ETC2_KIND_RGB, 0 },
	{ { GL_COMPRESSED_RGB8_PUNCHTHROUGH_ALPHA1_ETC2, GL_RGBA, VK_FORMAT_R8G8B8A8_UNORM, 4U, 4U, GLES_TEXEL_NORM, 1, 0, 0, 0 },
	  ETC2_KIND_PUNCHTHROUGH, 0 },
	{ { GL_COMPRESSED_SRGB8_PUNCHTHROUGH_ALPHA1_ETC2, GL_RGBA, VK_FORMAT_R8G8B8A8_SRGB, 4U, 4U, GLES_TEXEL_NORM, 1, 0, 0, 0 },
	  ETC2_KIND_PUNCHTHROUGH, 0 },
	{ { GL_COMPRESSED_RGBA8_ETC2_EAC, GL_RGBA, VK_FORMAT_R8G8B8A8_UNORM, 4U, 4U, GLES_TEXEL_NORM, 1, 0, 0, 0 }, ETC2_KIND_RGBA, 0 },
	{ { GL_COMPRESSED_SRGB8_ALPHA8_ETC2_EAC, GL_RGBA, VK_FORMAT_R8G8B8A8_SRGB, 4U, 4U, GLES_TEXEL_NORM, 1, 0, 0, 0 },
	  ETC2_KIND_RGBA, 0 }
};

/* ETC1's intensity modifiers (the table codeword picks a row; a texel's index, msb then lsb, picks a column). */
static const int etc2_modifiers[8][4] = {
	{ 2, 8, -2, -8 }, { 5, 17, -5, -17 }, { 9, 29, -9, -29 }, { 13, 42, -13, -42 },
	{ 18, 60, -18, -60 }, { 24, 80, -24, -80 }, { 33, 106, -33, -106 }, { 47, 183, -47, -183 }
};

/* The distances of the T and H modes. */
static const int etc2_distances[8] = { 3, 6, 11, 16, 23, 32, 41, 64 };

/* EAC's modifiers (the table index picks a row, a texel's 3-bit index a column). */
static const int etc2_eac_modifiers[16][8] = {
	{ -3, -6, -9, -15, 2, 5, 8, 14 }, { -3, -7, -10, -13, 2, 6, 9, 12 }, { -2, -5, -8, -13, 1, 4, 7, 12 },
	{ -2, -4, -6, -13, 1, 3, 5, 12 }, { -3, -6, -8, -12, 2, 5, 7, 11 }, { -3, -7, -9, -11, 2, 6, 8, 10 },
	{ -4, -7, -8, -11, 3, 6, 7, 10 }, { -3, -5, -8, -11, 2, 4, 7, 10 }, { -2, -6, -8, -10, 1, 5, 7, 9 },
	{ -2, -5, -8, -10, 1, 4, 7, 9 }, { -2, -4, -8, -10, 1, 3, 7, 9 }, { -2, -5, -7, -10, 1, 4, 6, 9 },
	{ -3, -4, -7, -10, 2, 3, 6, 9 }, { -1, -2, -3, -10, 0, 1, 2, 9 }, { -4, -6, -8, -9, 3, 5, 7, 8 },
	{ -3, -5, -7, -9, 2, 4, 6, 8 }
};

static const struct etc2_format *etc2_find(GLenum internal);
static uint64_t etc2_word(const unsigned char *bytes);
static int etc2_clamp(int value, int low, int high);
static void etc2_rgb_block(uint64_t word, int punchthrough, unsigned char texels[16][4]);
static void etc2_individual(uint64_t word, int differential, int punchthrough, unsigned char texels[16][4]);
static void etc2_paint(uint64_t word, const int colours[4][3], int punchthrough, unsigned char texels[16][4]);
static void etc2_t_mode(uint64_t word, int punchthrough, unsigned char texels[16][4]);
static void etc2_h_mode(uint64_t word, int punchthrough, unsigned char texels[16][4]);
static void etc2_planar(uint64_t word, unsigned char texels[16][4]);
static void etc2_eac_alpha(uint64_t word, unsigned char texels[16][4]);
static void etc2_eac_r11(uint64_t word, int is_signed, float values[16]);
static int etc2_extend4(unsigned value);
static int etc2_extend5(unsigned value);

/*
 * Returns the kept form of a compressed internal format (the format a
 * level of it has), or NULL when the internal format is not one of the
 * ten.
 */
const struct gles_format *
gles_compressed_format(
	GLenum internal)
{
	const struct etc2_format *entry;

	/* The format's entry. */
	entry = etc2_find(internal);
	if (entry == NULL)
		return NULL;

	/* Succeeded: its kept form. */
	return &entry->format;
}

/*
 * Returns how many bytes a compressed image of a format and a size has:
 * whole 4x4 blocks, 8 bytes each (16 for RG11 and RGBA8 with EAC), for
 * each of its depth images; 0 for a format that is not one.
 */
size_t
gles_compressed_size(
	GLenum internal,
	GLsizei width,
	GLsizei height,
	GLsizei depth)
{
	const struct etc2_format *entry;
	size_t blocks;
	size_t bytes;

	/* The format's entry, and the bytes of its block. */
	entry = etc2_find(internal);
	if (entry == NULL || width < 0 || height < 0 || depth < 0)
		return 0U;
	bytes = ETC2_BLOCK_BYTES;
	if (entry->kind == ETC2_KIND_RGBA || entry->kind == ETC2_KIND_RG11)
		bytes = ETC2_BLOCK_BYTES * 2U;

	/* Succeeded: the blocks covering the size, of each image. */
	blocks = ((size_t)width + 3U) / 4U * (((size_t)height + 3U) / 4U);
	return blocks * bytes * (size_t)depth;
}

/*
 * Lists the compressed formats (GL_COMPRESSED_TEXTURE_FORMATS) into
 * values when it is not NULL; returns how many there are.
 */
unsigned
gles_compressed_formats(
	GLint *values)
{
	unsigned index;

	/* Each format's internal format. */
	for (index = 0U; values != NULL && index < sizeof(etc2_formats) / sizeof(etc2_formats[0]); index++)
		values[index] = (GLint)etc2_formats[index].format.internal;

	/* Succeeded: the count. */
	return (unsigned)(sizeof(etc2_formats) / sizeof(etc2_formats[0]));
}

/*
 * Decodes one compressed image of a format and a size into texels of the
 * format's kept form (rows in the order the blocks' rows come, as GL
 * keeps a texture's rows), out holding width * height of them.  A block
 * past the image's edge gives only the texels inside it.
 */
void
gles_compressed_decode(
	const struct gles_format *format,
	const unsigned char *data,
	GLsizei width,
	GLsizei height,
	unsigned char *out)
{
	const struct etc2_format *entry;
	unsigned char texels[16][4];
	float reds[16];
	float greens[16];
	uint16_t halves[2];
	size_t block_bytes;
	size_t offset;
	GLsizei block_x;
	GLsizei block_y;
	int x;
	int y;
	int texel;

	/* The format's entry and its block's bytes. */
	entry = etc2_find(format->internal);
	if (entry == NULL)
		return;
	block_bytes = ETC2_BLOCK_BYTES;
	if (entry->kind == ETC2_KIND_RGBA || entry->kind == ETC2_KIND_RG11)
		block_bytes = ETC2_BLOCK_BYTES * 2U;

	/* Each block in order, row by row of blocks. */
	offset = 0U;
	for (block_y = 0; block_y < height; block_y += ETC2_BLOCK) {
		for (block_x = 0; block_x < width; block_x += ETC2_BLOCK) {
			/* The block decoded: an RGBA8 one, or one or two channels of 11 bits. */
			switch (entry->kind) {
			case ETC2_KIND_RGB:
				etc2_rgb_block(etc2_word(data + offset), 0, texels);
				break;
			case ETC2_KIND_PUNCHTHROUGH:
				etc2_rgb_block(etc2_word(data + offset), 1, texels);
				break;
			case ETC2_KIND_RGBA:
				/* The alpha's EAC block first, then the colour's. */
				etc2_rgb_block(etc2_word(data + offset + ETC2_BLOCK_BYTES), 0, texels);
				etc2_eac_alpha(etc2_word(data + offset), texels);
				break;
			case ETC2_KIND_R11:
				etc2_eac_r11(etc2_word(data + offset), entry->is_signed, reds);
				break;
			default:
				etc2_eac_r11(etc2_word(data + offset), entry->is_signed, reds);
				etc2_eac_r11(etc2_word(data + offset + ETC2_BLOCK_BYTES), entry->is_signed, greens);
				break;
			}
			offset += block_bytes;

			/* Its texels inside the image, into place. */
			for (y = 0; y < ETC2_BLOCK && block_y + y < height; y++) {
				for (x = 0; x < ETC2_BLOCK && block_x + x < width; x++) {
					texel = x * ETC2_BLOCK + y;
					if (entry->kind == ETC2_KIND_R11 || entry->kind == ETC2_KIND_RG11) {
						halves[0] = gles_half_from_float(reds[texel]);
						halves[1] = 0U;
						if (entry->kind == ETC2_KIND_RG11)
							halves[1] = gles_half_from_float(greens[texel]);
						memcpy(out + ((size_t)(block_y + y) * (size_t)width + (size_t)(block_x + x)) * format->bytes, halves,
						       format->bytes);
						continue;
					}

					/* An RGBA8 texel. */
					memcpy(out + ((size_t)(block_y + y) * (size_t)width + (size_t)(block_x + x)) * 4U, texels[texel], 4U);
				}
			}
		}
	}
}

/* Returns the entry of a compressed internal format, or NULL. */
static const struct etc2_format *
etc2_find(
	GLenum internal)
{
	unsigned index;

	/* The ten in order. */
	for (index = 0U; index < sizeof(etc2_formats) / sizeof(etc2_formats[0]); index++) {
		if (etc2_formats[index].format.internal == internal)
			return &etc2_formats[index];
	}

	/* Not one. */
	return NULL;
}

/* Reads a block's big-endian 64-bit word. */
static uint64_t
etc2_word(
	const unsigned char *bytes)
{
	uint64_t word;
	unsigned index;

	/* The most significant byte first. */
	word = 0U;
	for (index = 0U; index < 8U; index++)
		word = (word << 8) | bytes[index];

	/* Succeeded: the word. */
	return word;
}

/* Returns a value kept between two bounds. */
static int
etc2_clamp(
	int value,
	int low,
	int high)
{
	/* Below, above, or inside. */
	if (value < low)
		return low;
	if (value > high)
		return high;

	/* Succeeded: inside already. */
	return value;
}

/* Returns a 4-bit colour component as 8 bits. */
static int
etc2_extend4(
	unsigned value)
{
	/* The four bits twice. */
	return (int)((value << 4) | value);
}

/* Returns a 5-bit colour component as 8 bits. */
static int
etc2_extend5(
	unsigned value)
{
	/* The five bits and their top three. */
	return (int)((value << 3) | (value >> 2));
}

/*
 * Decodes an ETC2 RGB block (with punch-through alpha: its bit 33 says
 * whether the block is opaque, and the individual mode is not there):
 * the individual or differential mode of ETC1, or one of ETC2's T, H and
 * planar modes a differential block's overflowing red, green or blue
 * picks.
 */
static void
etc2_rgb_block(
	uint64_t word,
	int punchthrough,
	unsigned char texels[16][4])
{
	int red;
	int green;
	int blue;
	int delta;

	/* The individual mode (not with punch-through alpha, whose bit 33 is the opaque flag). */
	if (!punchthrough && ((word >> 33) & 1U) == 0U) {
		etc2_individual(word, 0, 0, texels);
		return;
	}

	/* The differential mode's red: an overflow is the T mode. */
	red = (int)((word >> 59) & 0x1fU);
	delta = (int)((word >> 56) & 0x7U);
	if (delta >= 4)
		delta -= 8;
	if (red + delta < 0 || red + delta > 31) {
		etc2_t_mode(word, punchthrough, texels);
		return;
	}

	/* Its green: an overflow is the H mode. */
	green = (int)((word >> 51) & 0x1fU);
	delta = (int)((word >> 48) & 0x7U);
	if (delta >= 4)
		delta -= 8;
	if (green + delta < 0 || green + delta > 31) {
		etc2_h_mode(word, punchthrough, texels);
		return;
	}

	/* Its blue: an overflow is the planar mode (always opaque). */
	blue = (int)((word >> 43) & 0x1fU);
	delta = (int)((word >> 40) & 0x7U);
	if (delta >= 4)
		delta -= 8;
	if (blue + delta < 0 || blue + delta > 31) {
		etc2_planar(word, texels);
		return;
	}

	/* The differential mode itself. */
	etc2_individual(word, 1, punchthrough, texels);
}

/*
 * Decodes ETC1's two sub-block modes: two base colours of 4 bits each
 * (individual), or one of 5 bits and a 3-bit signed difference
 * (differential), each with a table codeword, over two halves of the
 * block the flip bit chooses (left and right, or top and bottom).  A
 * punch-through block that is not opaque has no modifier at indices 0
 * and 2, and index 2 is transparent black.
 */
static void
etc2_individual(
	uint64_t word,
	int differential,
	int punchthrough,
	unsigned char texels[16][4])
{
	int colours[2][3];
	unsigned tables[2];
	unsigned shift;
	unsigned channel;
	unsigned base;
	unsigned index;
	unsigned half;
	unsigned pick;
	int delta;
	int modifier;
	int opaque;
	int x;
	int y;

	/* The base colours, channel by channel (red at bit 63, green 55, blue 47). */
	for (channel = 0U; channel < 3U; channel++) {
		shift = 59U - channel * 8U;
		if (differential) {
			base = (unsigned)((word >> shift) & 0x1fU);
			delta = (int)((word >> (shift - 3U)) & 0x7U);
			if (delta >= 4)
				delta -= 8;
			colours[0][channel] = etc2_extend5(base);
			colours[1][channel] = etc2_extend5((unsigned)((int)base + delta));
			continue;
		}

		/* Two 4-bit colours side by side. */
		colours[0][channel] = etc2_extend4((unsigned)((word >> (shift + 1U)) & 0xfU));
		colours[1][channel] = etc2_extend4((unsigned)((word >> (shift - 3U)) & 0xfU));
	}

	/* The two table codewords, and whether a punch-through block is opaque. */
	tables[0] = (unsigned)((word >> 37) & 0x7U);
	tables[1] = (unsigned)((word >> 34) & 0x7U);
	opaque = 1;
	if (punchthrough && ((word >> 33) & 1U) == 0U)
		opaque = 0;

	/* Each texel: its half, its index, its modifier. */
	for (x = 0; x < ETC2_BLOCK; x++) {
		for (y = 0; y < ETC2_BLOCK; y++) {
			index = (unsigned)(x * ETC2_BLOCK + y);
			half = (unsigned)(x >= 2);
			if (((word >> 32) & 1U) != 0U)
				half = (unsigned)(y >= 2);
			pick = (unsigned)(((word >> (index + 16U)) & 1U) << 1) | (unsigned)((word >> index) & 1U);

			/* A transparent texel of a block that is not opaque. */
			if (!opaque && pick == 2U) {
				memset(texels[index], 0, 4U);
				continue;
			}

			/* The modifier (none at indices 0 and 2 of such a block). */
			modifier = etc2_modifiers[tables[half]][pick];
			if (!opaque && pick == 0U)
				modifier = 0;
			for (channel = 0U; channel < 3U; channel++)
				texels[index][channel] = (unsigned char)etc2_clamp(colours[half][channel] + modifier, 0, 255);
			texels[index][3] = 255U;
		}
	}
}

/*
 * Gives each texel one of four paint colours by its 2-bit index (the T
 * and H modes); a punch-through block that is not opaque has transparent
 * black at index 2.
 */
static void
etc2_paint(
	uint64_t word,
	const int colours[4][3],
	int punchthrough,
	unsigned char texels[16][4])
{
	unsigned index;
	unsigned pick;
	unsigned channel;
	int opaque;

	/* Whether a punch-through block is opaque. */
	opaque = 1;
	if (punchthrough && ((word >> 33) & 1U) == 0U)
		opaque = 0;

	/* Each texel's paint colour. */
	for (index = 0U; index < 16U; index++) {
		pick = (unsigned)(((word >> (index + 16U)) & 1U) << 1) | (unsigned)((word >> index) & 1U);
		if (!opaque && pick == 2U) {
			memset(texels[index], 0, 4U);
			continue;
		}

		/* The colour, opaque. */
		for (channel = 0U; channel < 3U; channel++)
			texels[index][channel] = (unsigned char)etc2_clamp(colours[pick][channel], 0, 255);
		texels[index][3] = 255U;
	}
}

/*
 * Decodes ETC2's T mode: two 4-bit colours, the paint colours the first
 * and the second with a distance added, none, and subtracted.
 */
static void
etc2_t_mode(
	uint64_t word,
	int punchthrough,
	unsigned char texels[16][4])
{
	int first[3];
	int second[3];
	int colours[4][3];
	unsigned channel;
	int distance;

	/* The colours: red 1 at bits 60-59 and 57-56, then 4 bits each from 55 down. */
	first[0] = etc2_extend4((unsigned)((((word >> 59) & 0x3U) << 2) | ((word >> 56) & 0x3U)));
	first[1] = etc2_extend4((unsigned)((word >> 52) & 0xfU));
	first[2] = etc2_extend4((unsigned)((word >> 48) & 0xfU));
	second[0] = etc2_extend4((unsigned)((word >> 44) & 0xfU));
	second[1] = etc2_extend4((unsigned)((word >> 40) & 0xfU));
	second[2] = etc2_extend4((unsigned)((word >> 36) & 0xfU));

	/* The distance: its index at bits 35-34 and 32. */
	distance = etc2_distances[(((word >> 34) & 0x3U) << 1) | ((word >> 32) & 1U)];

	/* The paint colours. */
	for (channel = 0U; channel < 3U; channel++) {
		colours[0][channel] = first[channel];
		colours[1][channel] = second[channel] + distance;
		colours[2][channel] = second[channel];
		colours[3][channel] = second[channel] - distance;
	}

	/* Each texel's. */
	etc2_paint(word, colours, punchthrough, texels);
}

/*
 * Decodes ETC2's H mode: two 4-bit colours, the paint colours each with
 * a distance added and subtracted; the distance index's lowest bit is
 * whether the first colour is not below the second.
 */
static void
etc2_h_mode(
	uint64_t word,
	int punchthrough,
	unsigned char texels[16][4])
{
	unsigned first[3];
	unsigned second[3];
	int colours[4][3];
	unsigned channel;
	unsigned order;
	int distance;

	/* The colours' 4 bits each, spread over the block's first word. */
	first[0] = (unsigned)((word >> 59) & 0xfU);
	first[1] = (unsigned)((((word >> 56) & 0x7U) << 1) | ((word >> 52) & 1U));
	first[2] = (unsigned)((((word >> 51) & 1U) << 3) | (((word >> 48) & 0x3U) << 1) | ((word >> 47) & 1U));
	second[0] = (unsigned)((word >> 43) & 0xfU);
	second[1] = (unsigned)((((word >> 40) & 0x7U) << 1) | ((word >> 39) & 1U));
	second[2] = (unsigned)((word >> 35) & 0xfU);

	/* The distance: bits 34 and 32, and the colours' order. */
	order = 0U;
	if (((first[0] << 8) | (first[1] << 4) | first[2]) >= ((second[0] << 8) | (second[1] << 4) | second[2]))
		order = 1U;
	distance = etc2_distances[(((word >> 34) & 1U) << 2) | (((word >> 32) & 1U) << 1) | order];

	/* The paint colours. */
	for (channel = 0U; channel < 3U; channel++) {
		colours[0][channel] = etc2_extend4(first[channel]) + distance;
		colours[1][channel] = etc2_extend4(first[channel]) - distance;
		colours[2][channel] = etc2_extend4(second[channel]) + distance;
		colours[3][channel] = etc2_extend4(second[channel]) - distance;
	}

	/* Each texel's. */
	etc2_paint(word, colours, punchthrough, texels);
}

/*
 * Decodes ETC2's planar mode: three colours (at the origin, along x and
 * along y) of 6, 7 and 6 bits, interpolated over the block.
 */
static void
etc2_planar(
	uint64_t word,
	unsigned char texels[16][4])
{
	int origin[3];
	int horizontal[3];
	int vertical[3];
	unsigned channel;
	unsigned index;
	int x;
	int y;

	/* The origin's red, green and blue, spread over bits 62 down to 39. */
	origin[0] = (int)((word >> 57) & 0x3fU);
	origin[1] = (int)((((word >> 56) & 1U) << 6) | ((word >> 49) & 0x3fU));
	origin[2] = (int)((((word >> 48) & 1U) << 5) | (((word >> 43) & 0x3U) << 3) | (((word >> 40) & 0x3U) << 1) |
			  ((word >> 39) & 1U));

	/* The horizontal colour (bits 38 down to 19) and the vertical one (18 down to 0). */
	horizontal[0] = (int)((((word >> 34) & 0x1fU) << 1) | ((word >> 32) & 1U));
	horizontal[1] = (int)((word >> 25) & 0x7fU);
	horizontal[2] = (int)((word >> 19) & 0x3fU);
	vertical[0] = (int)((word >> 13) & 0x3fU);
	vertical[1] = (int)((word >> 6) & 0x7fU);
	vertical[2] = (int)(word & 0x3fU);

	/* Each 6-bit component as 8 bits, and green's 7 bits. */
	for (channel = 0U; channel < 3U; channel += 2U) {
		origin[channel] = (origin[channel] << 2) | (origin[channel] >> 4);
		horizontal[channel] = (horizontal[channel] << 2) | (horizontal[channel] >> 4);
		vertical[channel] = (vertical[channel] << 2) | (vertical[channel] >> 4);
	}
	origin[1] = (origin[1] << 1) | (origin[1] >> 6);
	horizontal[1] = (horizontal[1] << 1) | (horizontal[1] >> 6);
	vertical[1] = (vertical[1] << 1) | (vertical[1] >> 6);

	/* Each texel interpolated, rounded, clamped. */
	for (x = 0; x < ETC2_BLOCK; x++) {
		for (y = 0; y < ETC2_BLOCK; y++) {
			index = (unsigned)(x * ETC2_BLOCK + y);
			for (channel = 0U; channel < 3U; channel++) {
				texels[index][channel] = (unsigned char)etc2_clamp(
					(x * (horizontal[channel] - origin[channel]) + y * (vertical[channel] - origin[channel]) +
					 4 * origin[channel] + 2) >> 2,
					0,
					255);
			}
			texels[index][3] = 255U;
		}
	}
}

/* Decodes an EAC block of 8-bit alpha into the texels' alpha. */
static void
etc2_eac_alpha(
	uint64_t word,
	unsigned char texels[16][4])
{
	unsigned index;
	unsigned pick;
	int base;
	int multiplier;
	int table;

	/* The base codeword, the multiplier and the table index. */
	base = (int)((word >> 56) & 0xffU);
	multiplier = (int)((word >> 52) & 0xfU);
	table = (int)((word >> 48) & 0xfU);

	/* Each texel's 3-bit index, from bit 47 down. */
	for (index = 0U; index < 16U; index++) {
		pick = (unsigned)((word >> (45U - index * 3U)) & 0x7U);
		texels[index][3] = (unsigned char)etc2_clamp(base + etc2_eac_modifiers[table][pick] * multiplier, 0, 255);
	}
}

/*
 * Decodes an EAC block of an 11-bit channel into normalized values: an
 * unsigned one in [0, 1] (base * 8 + 4 with each modifier times the
 * multiplier times 8, of 2047), a signed one in [-1, 1] (a signed base,
 * no + 4, of 1023); a multiplier of 0 adds the modifier alone.
 */
static void
etc2_eac_r11(
	uint64_t word,
	int is_signed,
	float values[16])
{
	unsigned index;
	unsigned pick;
	int base;
	int multiplier;
	int table;
	int modifier;
	int value;

	/* The base codeword (signed in a signed format, -128 taken as -127), the multiplier and the table index. */
	base = (int)((word >> 56) & 0xffU);
	if (is_signed && base >= 128)
		base -= 256;
	if (base == -128)
		base = -127;
	multiplier = (int)((word >> 52) & 0xfU);
	table = (int)((word >> 48) & 0xfU);

	/* Each texel's 11-bit value. */
	for (index = 0U; index < 16U; index++) {
		pick = (unsigned)((word >> (45U - index * 3U)) & 0x7U);
		modifier = etc2_eac_modifiers[table][pick];
		if (multiplier != 0)
			modifier *= multiplier * 8;

		/* Unsigned: of 2047, from base * 8 + 4. */
		if (!is_signed) {
			value = etc2_clamp(base * 8 + 4 + modifier, 0, 2047);
			values[index] = (float)value / 2047.0f;
			continue;
		}

		/* Signed: of 1023. */
		value = etc2_clamp(base * 8 + modifier, -1023, 1023);
		values[index] = (float)value / 1023.0f;
	}
}
