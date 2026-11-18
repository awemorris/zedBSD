/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The entropy decoding of a Huffman scan: the tables made ready (a
 * lookup of the first bits, then the longest codes per length), the bits
 * of the scan with its stuffed bytes, and the restart markers.  In a
 * sequential scan (ITU T.81 F.2) each block's DC difference and AC run
 * lengths are read and the block goes through the inverse DCT into its
 * component's plane at once.  A progressive scan (T.81 G.1.2) adds to the
 * coefficients kept for each block: the DC's first bits or one more of
 * them, or a band of AC coefficients (with end-of-band runs) or one more
 * bit of each; the last scan's coefficients go through the inverse DCT
 * when the image ends (jpeg_compat_finish_progressive).
 *
 * Bad data does not stop the decoding, and goes as libjpeg's does: past a
 * marker the bits read as zeros, and once a code has used them the rest
 * of the segment's MCUs stay zero (uniform gray); a code that is in no
 * table is reported and read as 0.
 */

#include "internal.h"

#include <string.h>

/* The bits the accumulator is filled to (it holds 64). */
#define JPEG_BITS_FILL		56

static void jpeg_compat_fill_bits(j_decompress_ptr cinfo);
static void jpeg_compat_consume(j_decompress_ptr cinfo, int count);
static void jpeg_compat_reset_bits(j_decompress_ptr cinfo);
static int jpeg_compat_decode(j_decompress_ptr cinfo, const struct jpeg_huffman *huffman);
static int jpeg_compat_get_bits(j_decompress_ptr cinfo, int size);
static int jpeg_compat_receive(j_decompress_ptr cinfo, int size);
static void jpeg_compat_decode_block(j_decompress_ptr cinfo, int index, JDIMENSION column, JDIMENSION row, int skip);
static void jpeg_compat_decode_sequential(j_decompress_ptr cinfo, int index, JDIMENSION column, JDIMENSION row);
static JCOEF *jpeg_compat_coefficients(j_decompress_ptr cinfo, int index, JDIMENSION column, JDIMENSION row);
static void jpeg_compat_dc_first(j_decompress_ptr cinfo, int index, JCOEF *block);
static void jpeg_compat_dc_refine(j_decompress_ptr cinfo, JCOEF *block);
static void jpeg_compat_ac_first(j_decompress_ptr cinfo, int index, JCOEF *block);
static void jpeg_compat_ac_refine(j_decompress_ptr cinfo, int index, JCOEF *block);
static void jpeg_compat_refine_bit(j_decompress_ptr cinfo, JCOEF *coefficient, int bit);
static void jpeg_compat_latch_quantization(j_decompress_ptr cinfo);
static void jpeg_compat_store_block(j_decompress_ptr cinfo, int index, JDIMENSION column, JDIMENSION row, const JCOEF *coefficients);
static void jpeg_compat_transform(struct jpeg_plane *plane, const JCOEF *coefficients, const UINT16 *quantization, JDIMENSION column, JDIMENSION row);
static void jpeg_compat_check_tables(j_decompress_ptr cinfo);
static void jpeg_compat_restart(j_decompress_ptr cinfo);
static void jpeg_compat_next_mcu(j_decompress_ptr cinfo, JDIMENSION done);

/*
 * Makes a Huffman table ready for decoding from its code counts and
 * values (the canonical codes of T.81 C.2).  A table whose codes do not
 * fit their lengths is an error.
 */
void
jpeg_compat_build_huffman(
	j_decompress_ptr cinfo,
	const JHUFF_TBL *table,
	struct jpeg_huffman *huffman)
{
	unsigned length;
	unsigned count;
	unsigned first;
	unsigned last;
	int32_t code;
	int32_t value;

	/* Each length's codes, in order: the lookup for the short ones, and the bounds for all. */
	memset(huffman, 0, sizeof(*huffman));
	memcpy(huffman->values, table->huffval, sizeof(huffman->values));
	code = 0;
	value = 0;
	for (length = 1; length <= 16U; length++) {
		huffman->valoffset[length] = value - code;
		for (count = 0; count < table->bits[length]; count++) {
			if (length <= JPEG_FAST_BITS) {
				/* Every lookup index that starts with the code. */
				first = (unsigned)code << (JPEG_FAST_BITS - length);
				last = first + (1U << (JPEG_FAST_BITS - length));
				for (; first < last; first++)
					huffman->fast[first] = (uint16_t)((length << 8) | table->huffval[value]);
			}

			/* The next code and value. */
			code++;
			value++;
		}

		/* The largest code of this length (-1 when it has none), which must fit the length. */
		huffman->maxcode[length] = code - 1;
		if (table->bits[length] == 0)
			huffman->maxcode[length] = -1;
		if (code > ((int32_t)1 << length))
			jpeg_compat_fail((j_common_ptr)cinfo, JERR_BAD_TABLE);
		code <<= 1;
	}

	/* Succeeded: the table can decode. */
	huffman->maxcode[17] = INT32_MAX;
	huffman->defined = 1;
}

/*
 * Decodes the scan whose header was just read: its MCUs (one block of
 * its component when it has one, else each component's blocks in turn),
 * with the restart markers between intervals.
 */
void
jpeg_compat_decode_scan(
	j_decompress_ptr cinfo)
{
	struct jpeg_decomp_master *master;
	jpeg_component_info *component;
	JDIMENSION across;
	JDIMENSION down;
	JDIMENSION column;
	JDIMENSION row;
	JDIMENSION done;
	int index;
	int slot;
	int skip;
	int h;
	int v;

	/* The tables it needs, and a clean start: no bits, predictions of 0, no band to skip, the first interval. */
	master = cinfo->master;
	jpeg_compat_check_tables(cinfo);
	jpeg_compat_latch_quantization(cinfo);
	jpeg_compat_reset_bits(cinfo);
	master->insufficient = 0;
	master->eobrun = 0;
	for (slot = 0; slot < master->scan_count; slot++)
		master->planes[master->scan_components[slot]].prediction = 0;
	master->restarts_left = cinfo->restart_interval;
	master->next_restart = 0;

	/* One component: its blocks row by row, each block an MCU. */
	done = 0;
	if (master->scan_count == 1) {
		index = master->scan_components[0];
		component = &master->components[index];
		across = component->width_in_blocks;
		down = component->height_in_blocks;
		for (row = 0; row < down; row++) {
			for (column = 0; column < across; column++) {
				jpeg_compat_next_mcu(cinfo, done);
				skip = master->insufficient;
				jpeg_compat_decode_block(cinfo, index, column, row, skip);
				done++;
			}
		}

		/* Succeeded: the component is decoded. */
		master->planes[index].decoded = 1;
		return;
	}

	/* Several: each MCU holds each component's h by v blocks. */
	for (row = 0; row < master->mcus_down; row++) {
		for (column = 0; column < master->mcus_across; column++) {
			jpeg_compat_next_mcu(cinfo, done);
			skip = master->insufficient;
			for (slot = 0; slot < master->scan_count; slot++) {
				index = master->scan_components[slot];
				component = &master->components[index];
				for (v = 0; v < component->v_samp_factor; v++) {
					for (h = 0; h < component->h_samp_factor; h++)
						jpeg_compat_decode_block(cinfo, index, column * (JDIMENSION)component->h_samp_factor + (JDIMENSION)h, row * (JDIMENSION)component->v_samp_factor + (JDIMENSION)v, skip);
				}
			}

			/* One more MCU of the scan. */
			done++;
		}
	}

	/* Succeeded: the scan's components are decoded. */
	for (slot = 0; slot < master->scan_count; slot++)
		master->planes[master->scan_components[slot]].decoded = 1;
}

/*
 * Ends a progressive image: every block's coefficients, as the last scan
 * left them, go through the inverse DCT into the planes.
 */
void
jpeg_compat_finish_progressive(
	j_decompress_ptr cinfo)
{
	struct jpeg_decomp_master *master;
	struct jpeg_plane *plane;
	JDIMENSION across;
	JDIMENSION down;
	JDIMENSION column;
	JDIMENSION row;
	int index;

	/* Each component's blocks, row by row, with the table its first scan latched. */
	master = cinfo->master;
	for (index = 0; index < cinfo->num_components; index++) {
		plane = &master->planes[index];
		across = (JDIMENSION)plane->blocks_across;
		down = (JDIMENSION)plane->blocks_down;

		/* A component no scan named keeps its plane at the centre sample. */
		if (!plane->quant_latched)
			continue;
		for (row = 0; row < down; row++) {
			for (column = 0; column < across; column++) {
				jpeg_compat_transform(
					plane,
					plane->coefficients + ((size_t)row * across + column) * DCTSIZE2,
					plane->quant.quantval,
					column,
					row);
			}
		}
	}
}

/* Fills the accumulator from the source: stuffed 0xFF 0x00 is a 0xFF byte, and past a marker come zeros. */
static void
jpeg_compat_fill_bits(
	j_decompress_ptr cinfo)
{
	struct jpeg_decomp_master *master;
	int byte;
	int next;

	/* Whole bytes until the accumulator is full enough. */
	master = cinfo->master;
	while (master->bit_count <= JPEG_BITS_FILL) {
		/* A marker was met: the scan's data is over, and zeros follow. */
		if (master->marker != 0) {
			master->bits <<= 8;
			master->bit_count += 8;
			master->pad_bits += 8;
			continue;
		}

		/* A data byte, or 0xFF: stuffed data, fill, or a marker. */
		byte = jpeg_compat_read_byte(cinfo);
		if (byte == 0xFF) {
			do {
				next = jpeg_compat_read_byte(cinfo);
			} while (next == 0xFF);
			if (next != 0) {
				master->marker = next;
				continue;
			}
		}

		/* The byte's bits at the bottom. */
		master->bits = (master->bits << 8) | (uint64_t)byte;
		master->bit_count += 8;
	}
}

/* Uses bits of the accumulator; using the zeros after a marker means the data ran out. */
static void
jpeg_compat_consume(
	j_decompress_ptr cinfo,
	int count)
{
	struct jpeg_decomp_master *master;

	/* The bits go. */
	master = cinfo->master;
	master->bit_count -= count;

	/* Into the zeros: reported once, and the segment's later MCUs are left at zero. */
	if (master->bit_count < master->pad_bits) {
		if (!master->insufficient)
			jpeg_compat_warn((j_common_ptr)cinfo, JWRN_HIT_MARKER);
		master->insufficient = 1;
		master->pad_bits = master->bit_count;
	}
}

/* Empties the accumulator (at the start of a scan and at a restart marker). */
static void
jpeg_compat_reset_bits(
	j_decompress_ptr cinfo)
{
	struct jpeg_decomp_master *master;

	/* No bits, and no zeros. */
	master = cinfo->master;
	master->bits = 0;
	master->bit_count = 0;
	master->pad_bits = 0;
}

/* Decodes one Huffman code; a code in no table is reported and read as 0. */
static int
jpeg_compat_decode(
	j_decompress_ptr cinfo,
	const struct jpeg_huffman *huffman)
{
	struct jpeg_decomp_master *master;
	uint32_t look;
	int32_t code;
	int length;
	unsigned entry;

	/* Enough bits for the longest code. */
	master = cinfo->master;
	if (master->bit_count < 16)
		jpeg_compat_fill_bits(cinfo);

	/* A short code, from the lookup. */
	look = (uint32_t)(master->bits >> (master->bit_count - JPEG_FAST_BITS)) & ((1U << JPEG_FAST_BITS) - 1U);
	entry = huffman->fast[look];
	if (entry != 0) {
		jpeg_compat_consume(cinfo, (int)(entry >> 8));
		return (int)(entry & 0xFFU);
	}

	/* A longer one, length by length. */
	for (length = JPEG_FAST_BITS + 1; length <= 16; length++) {
		code = (int32_t)((master->bits >> (master->bit_count - length)) & ((1U << length) - 1U));
		if (code <= huffman->maxcode[length]) {
			jpeg_compat_consume(cinfo, length);
			return huffman->values[(code + huffman->valoffset[length]) & 0xFF];
		}
	}

	/* No code matches: corrupt data, read as 0 after its sixteen bits (as libjpeg does). */
	jpeg_compat_warn((j_common_ptr)cinfo, JWRN_HUFF_BAD_CODE);
	jpeg_compat_consume(cinfo, 16);
	return 0;
}

/* Reads size raw bits (at most 16) as an unsigned number. */
static int
jpeg_compat_get_bits(
	j_decompress_ptr cinfo,
	int size)
{
	struct jpeg_decomp_master *master;
	int value;

	/* Enough bits in the accumulator, then the top ones. */
	master = cinfo->master;
	if (master->bit_count < size)
		jpeg_compat_fill_bits(cinfo);
	value = (int)((master->bits >> (master->bit_count - size)) & ((1U << size) - 1U));
	jpeg_compat_consume(cinfo, size);

	/* Succeeded: the bits. */
	return value;
}

/* Reads a value of a size category (T.81 F.2.2.1): size bits, extended to its sign. */
static int
jpeg_compat_receive(
	j_decompress_ptr cinfo,
	int size)
{
	int value;

	/* Nothing for size 0; a corrupt size larger than a sample's is cut. */
	if (size == 0)
		return 0;
	if (size > 16)
		size = 16;

	/* The bits. */
	value = jpeg_compat_get_bits(cinfo, size);

	/* A value with its top bit clear is negative. */
	if (value < (1 << (size - 1)))
		value -= (1 << size) - 1;
	return value;
}

/*
 * Decodes one block of a component at a block position of its plane: a
 * sequential block into samples, a progressive scan's part into the
 * block's coefficients.  skip leaves the block as it is (the data ran out
 * earlier in the segment), which for a sequential block is zero.
 */
static void
jpeg_compat_decode_block(
	j_decompress_ptr cinfo,
	int index,
	JDIMENSION column,
	JDIMENSION row,
	int skip)
{
	struct jpeg_decomp_master *master;
	JCOEF zero[DCTSIZE2];
	JCOEF *block;

	/* A sequential block: its samples, or uniform gray when the data ran out. */
	master = cinfo->master;
	if (!cinfo->progressive_mode) {
		if (skip) {
			memset(zero, 0, sizeof(zero));
			jpeg_compat_store_block(cinfo, index, column, row, zero);
			return;
		}

		/* The block's samples. */
		jpeg_compat_decode_sequential(cinfo, index, column, row);
		return;
	}

	/* A progressive block outside the plane (a corrupt frame) or past the data is left alone. */
	block = jpeg_compat_coefficients(cinfo, index, column, row);
	if (block == NULL || skip)
		return;

	/* The part of the block this scan carries. */
	if (master->spectral_start == 0) {
		if ((master->approximation >> 4) == 0) {
			/* The DC's first bits. */
			jpeg_compat_dc_first(cinfo, index, block);
		} else {
			/* One more bit of the DC. */
			jpeg_compat_dc_refine(cinfo, block);
		}
	} else if ((master->approximation >> 4) == 0) {
		/* A band of AC coefficients. */
		jpeg_compat_ac_first(cinfo, index, block);
	} else {
		/* One more bit of each AC coefficient of a band. */
		jpeg_compat_ac_refine(cinfo, index, block);
	}
}

/*
 * Decodes one sequential block into its coefficients and puts its samples
 * at a block position of the plane.
 */
static void
jpeg_compat_decode_sequential(
	j_decompress_ptr cinfo,
	int index,
	JDIMENSION column,
	JDIMENSION row)
{
	struct jpeg_decomp_master *master;
	jpeg_component_info *component;
	struct jpeg_plane *plane;
	JCOEF coefficients[DCTSIZE2];
	int position;
	int symbol;
	int run;
	int size;
	int value;

	/* The DC coefficient: the difference from the prediction, its size and then its value. */
	master = cinfo->master;
	component = &master->components[index];
	plane = &master->planes[index];
	memset(coefficients, 0, sizeof(coefficients));
	size = jpeg_compat_decode(cinfo, &master->dc[component->dc_tbl_no]);
	value = jpeg_compat_receive(cinfo, size);
	plane->prediction += value;
	coefficients[0] = (JCOEF)plane->prediction;

	/* The AC coefficients: runs of zeros, each ended by a value, until EOB. */
	for (position = 1; position < DCTSIZE2; position++) {
		symbol = jpeg_compat_decode(cinfo, &master->ac[component->ac_tbl_no]);
		run = symbol >> 4;
		size = symbol & 15;
		if (size != 0) {
			position += run;
			value = jpeg_compat_receive(cinfo, size);
			coefficients[jpeg_compat_natural_order[position]] = (JCOEF)value;
			continue;
		}

		/* ZRL (sixteen zeros) goes on; anything else is EOB. */
		if (run != 15)
			break;
		position += 15;
	}

	/* The samples. */
	jpeg_compat_store_block(cinfo, index, column, row, coefficients);
}

/* Finds the coefficients kept for a block position of a component's plane (NULL outside it). */
static JCOEF *
jpeg_compat_coefficients(
	j_decompress_ptr cinfo,
	int index,
	JDIMENSION column,
	JDIMENSION row)
{
	struct jpeg_plane *plane;

	/* A block outside the plane (a corrupt frame) has none. */
	plane = &cinfo->master->planes[index];
	if ((size_t)row >= plane->blocks_down || (size_t)column >= plane->blocks_across)
		return NULL;

	/* Succeeded: the block's 64 coefficients, blocks row by row. */
	return plane->coefficients + ((size_t)row * plane->blocks_across + column) * DCTSIZE2;
}

/* Reads the first bits of a block's DC (T.81 G.1.2.1): the difference from the prediction, shifted up by Al. */
static void
jpeg_compat_dc_first(
	j_decompress_ptr cinfo,
	int index,
	JCOEF *block)
{
	struct jpeg_decomp_master *master;
	jpeg_component_info *component;
	struct jpeg_plane *plane;
	int size;
	int value;
	int low;

	/* The difference's size, then its value, added to the prediction. */
	master = cinfo->master;
	component = &master->components[index];
	plane = &master->planes[index];
	low = master->approximation & 15;
	size = jpeg_compat_decode(cinfo, &master->dc[component->dc_tbl_no]);
	value = jpeg_compat_receive(cinfo, size);
	plane->prediction += value;

	/* The DC holds the bits above the ones later scans send. */
	block[0] = (JCOEF)(plane->prediction * (1 << low));
}

/* Reads one more bit of a block's DC (T.81 G.1.2.1). */
static void
jpeg_compat_dc_refine(
	j_decompress_ptr cinfo,
	JCOEF *block)
{
	int low;
	int bit;

	/* The bit, set at its place when it is 1. */
	low = cinfo->master->approximation & 15;
	bit = jpeg_compat_get_bits(cinfo, 1);
	if (bit != 0)
		block[0] = (JCOEF)(block[0] | (1 << low));
}

/*
 * Reads the first bits of a band of a block's AC coefficients (T.81
 * G.1.2.2): runs of zeros ended by values, shifted up by Al, or an
 * end-of-band run that also covers the blocks after this one.
 */
static void
jpeg_compat_ac_first(
	j_decompress_ptr cinfo,
	int index,
	JCOEF *block)
{
	struct jpeg_decomp_master *master;
	jpeg_component_info *component;
	int position;
	int symbol;
	int run;
	int size;
	int value;
	int low;

	/* A block inside an end-of-band run has nothing in this band. */
	master = cinfo->master;
	if (master->eobrun > 0) {
		master->eobrun--;
		return;
	}

	/* Each coefficient of the band, until its end or an end-of-band run. */
	component = &master->components[index];
	low = master->approximation & 15;
	for (position = master->spectral_start; position <= master->spectral_end; position++) {
		symbol = jpeg_compat_decode(cinfo, &master->ac[component->ac_tbl_no]);
		run = symbol >> 4;
		size = symbol & 15;
		if (size != 0) {
			/* A value after run zeros. */
			position += run;
			value = jpeg_compat_receive(cinfo, size);
			block[jpeg_compat_natural_order[position]] = (JCOEF)(value * (1 << low));
			continue;
		}

		/* ZRL: sixteen zeros. */
		if (run == 15) {
			position += 15;
			continue;
		}

		/* An end-of-band run of 2^run blocks and run more bits, this block being the first. */
		master->eobrun = 1U << run;
		if (run != 0)
			master->eobrun += (unsigned int)jpeg_compat_get_bits(cinfo, run);
		master->eobrun--;
		break;
	}
}

/*
 * Reads one more bit of a band of a block's AC coefficients (T.81
 * G.1.2.3): each coefficient already nonzero gets a correction bit, and a
 * new coefficient of +1 or -1 at that bit comes after a run of the zero
 * ones; inside an end-of-band run only the corrections come.
 */
static void
jpeg_compat_ac_refine(
	j_decompress_ptr cinfo,
	int index,
	JCOEF *block)
{
	struct jpeg_decomp_master *master;
	jpeg_component_info *component;
	JCOEF *coefficient;
	int position;
	int symbol;
	int run;
	int size;
	int bit;
	int plus;
	int minus;
	int value;

	/* The bit this scan adds, as a positive and a negative value. */
	master = cinfo->master;
	component = &master->components[index];
	plus = 1 << (master->approximation & 15);
	minus = -plus;
	position = master->spectral_start;

	/* Outside an end-of-band run: new coefficients after runs of zeros, with corrections on the way. */
	if (master->eobrun == 0) {
		for (; position <= master->spectral_end; position++) {
			symbol = jpeg_compat_decode(cinfo, &master->ac[component->ac_tbl_no]);
			run = symbol >> 4;
			size = symbol & 15;
			value = 0;
			if (size != 0) {
				/* A new coefficient: its sign bit says +1 or -1 at this bit. */
				bit = jpeg_compat_get_bits(cinfo, 1);
				value = minus;
				if (bit != 0)
					value = plus;
			} else if (run != 15) {
				/* An end-of-band run: the rest of this block is corrected below. */
				master->eobrun = 1U << run;
				if (run != 0)
					master->eobrun += (unsigned int)jpeg_compat_get_bits(cinfo, run);
				break;
			}

			/* Past the nonzero coefficients (each corrected) and run zero ones, to the new one's place. */
			do {
				coefficient = &block[jpeg_compat_natural_order[position]];
				if (*coefficient != 0) {
					bit = jpeg_compat_get_bits(cinfo, 1);
					jpeg_compat_refine_bit(cinfo, coefficient, bit);
				} else {
					run--;
					if (run < 0)
						break;
				}

				/* The next coefficient of the band. */
				position++;
			} while (position <= master->spectral_end);

			/* The new coefficient (none for ZRL). */
			if (value != 0)
				block[jpeg_compat_natural_order[position]] = (JCOEF)value;
		}
	}

	/* Inside an end-of-band run: only the corrections of the nonzero coefficients left. */
	if (master->eobrun > 0) {
		for (; position <= master->spectral_end; position++) {
			coefficient = &block[jpeg_compat_natural_order[position]];
			if (*coefficient != 0) {
				bit = jpeg_compat_get_bits(cinfo, 1);
				jpeg_compat_refine_bit(cinfo, coefficient, bit);
			}
		}

		/* This block is done with the run. */
		master->eobrun--;
	}
}

/*
 * Applies a correction bit to a nonzero AC coefficient: a 1 adds the
 * scan's bit to its magnitude, unless the coefficient has it already.
 */
static void
jpeg_compat_refine_bit(
	j_decompress_ptr cinfo,
	JCOEF *coefficient,
	int bit)
{
	int place;

	/* A 0 changes nothing. */
	if (bit == 0)
		return;

	/* The magnitude grows by the bit, away from zero. */
	place = 1 << (cinfo->master->approximation & 15);
	if ((*coefficient & place) != 0)
		return;
	if (*coefficient >= 0) {
		*coefficient = (JCOEF)(*coefficient + place);
	} else {
		*coefficient = (JCOEF)(*coefficient - place);
	}
}

/* Puts a block's samples (its inverse DCT) at a block position of its component's plane. */
static void
jpeg_compat_store_block(
	j_decompress_ptr cinfo,
	int index,
	JDIMENSION column,
	JDIMENSION row,
	const JCOEF *coefficients)
{
	struct jpeg_decomp_master *master;
	jpeg_component_info *component;
	struct jpeg_plane *plane;

	/* A block outside the plane (a corrupt frame) is dropped. */
	master = cinfo->master;
	component = &master->components[index];
	plane = &master->planes[index];
	if ((size_t)row >= plane->blocks_down || (size_t)column >= plane->blocks_across)
		return;

	/* The inverse DCT into the plane. */
	jpeg_compat_transform(plane, coefficients, master->quant[component->quant_tbl_no].quantval, column, row);
}

/* Puts a block's inverse DCT, full or reduced to the plane's block size, at its block position in the plane. */
static void
jpeg_compat_transform(
	struct jpeg_plane *plane,
	const JCOEF *coefficients,
	const UINT16 *quantization,
	JDIMENSION column,
	JDIMENSION row)
{
	JSAMPLE *out;
	size_t size;

	/* The block's first sample in the plane. */
	size = (size_t)plane->block_size;
	out = plane->samples + (size_t)row * size * plane->stride + (size_t)column * size;

	/* A full block takes the 8-point transform, a smaller one the reduced transform. */
	if (plane->block_size == DCTSIZE) {
		jpeg_compat_idct(coefficients, quantization, out, plane->stride);
	} else {
		jpeg_compat_idct_reduced(coefficients, quantization, plane->block_size, out, plane->stride);
	}
}

/*
 * Refuses a scan whose component names a table the file has not defined:
 * a sequential scan uses its DC and AC tables, a progressive one the DC
 * table for the DC's first bits and the AC table for a band of AC.
 */
static void
jpeg_compat_check_tables(
	j_decompress_ptr cinfo)
{
	struct jpeg_decomp_master *master;
	jpeg_component_info *component;
	int needs_dc;
	int needs_ac;
	int slot;

	/* The tables this kind of scan reads. */
	master = cinfo->master;
	needs_dc = 1;
	needs_ac = 1;
	if (cinfo->progressive_mode) {
		needs_dc = 0;
		needs_ac = 0;
		if (master->spectral_start == 0 && (master->approximation >> 4) == 0)
			needs_dc = 1;
		if (master->spectral_start != 0)
			needs_ac = 1;
	}

	/* Each component of the scan: its Huffman and quantization tables. */
	for (slot = 0; slot < master->scan_count; slot++) {
		component = &master->components[master->scan_components[slot]];
		if (needs_dc && !master->dc[component->dc_tbl_no].defined)
			jpeg_compat_fail_number((j_common_ptr)cinfo, JERR_NO_TABLE, component->dc_tbl_no, 0);
		if (needs_ac && !master->ac[component->ac_tbl_no].defined)
			jpeg_compat_fail_number((j_common_ptr)cinfo, JERR_NO_TABLE, 0x10 + component->ac_tbl_no, 0);
		if (!master->quant[component->quant_tbl_no].sent_table)
			jpeg_compat_fail_number((j_common_ptr)cinfo, JERR_NO_TABLE, component->quant_tbl_no, 0);
	}
}

/*
 * Latches the quantization table of each component of a progressive scan
 * the first time the component is scanned: its coefficients go through
 * the inverse DCT only at the end, with the table in force then.
 */
static void
jpeg_compat_latch_quantization(
	j_decompress_ptr cinfo)
{
	struct jpeg_decomp_master *master;
	jpeg_component_info *component;
	struct jpeg_plane *plane;
	int index;
	int slot;

	/* Only a progressive image keeps coefficients. */
	master = cinfo->master;
	if (!cinfo->progressive_mode)
		return;

	/* Each component of the scan not latched yet. */
	for (slot = 0; slot < master->scan_count; slot++) {
		index = master->scan_components[slot];
		component = &master->components[index];
		plane = &master->planes[index];
		if (plane->quant_latched)
			continue;
		plane->quant = master->quant[component->quant_tbl_no];
		plane->quant_latched = 1;
	}
}

/* Starts an MCU: at the end of a restart interval, the restart marker comes first. */
static void
jpeg_compat_next_mcu(
	j_decompress_ptr cinfo,
	JDIMENSION done)
{
	struct jpeg_decomp_master *master;

	/* No intervals, or the first MCU. */
	master = cinfo->master;
	if (cinfo->restart_interval == 0)
		return;
	if (done != 0 && master->restarts_left == 0)
		jpeg_compat_restart(cinfo);

	/* One MCU of the interval is used. */
	master->restarts_left--;
}

/*
 * Reads a restart marker: the bits left are padding, the marker must be
 * the next RSTn (else the source's resync decides), and the predictions
 * start again.
 */
static void
jpeg_compat_restart(
	j_decompress_ptr cinfo)
{
	struct jpeg_decomp_master *master;
	int marker;
	int slot;

	/* The marker the bits ran into, or the next one in the source. */
	master = cinfo->master;
	jpeg_compat_reset_bits(cinfo);
	marker = master->marker;
	if (marker == 0) {
		marker = jpeg_compat_next_marker(cinfo);
		master->marker = marker;
	}

	/* The expected RSTn is consumed; another RSTn is reported and consumed; any other marker stays. */
	if (marker != JPEG_RST0 + master->next_restart) {
		cinfo->src->resync_to_restart(cinfo, master->next_restart);
	}

	/* A restart marker is used up; another marker waits for the marker reader. */
	if (marker >= JPEG_RST0 && marker <= JPEG_RST0 + 7)
		master->marker = 0;

	/* Data again after a restart marker; still none when another marker is in the way. */
	if (master->marker == 0)
		master->insufficient = 0;

	/* A new interval with new predictions and no band to skip. */
	for (slot = 0; slot < master->scan_count; slot++)
		master->planes[master->scan_components[slot]].prediction = 0;
	master->eobrun = 0;
	master->next_restart = (master->next_restart + 1) & 7;
	master->restarts_left = cinfo->restart_interval;
}
