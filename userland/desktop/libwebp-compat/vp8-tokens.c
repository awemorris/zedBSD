/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The coefficient tokens of a lossy macroblock and their dequantization.
 */

#include "vp8-tokens.h"

#include <string.h>

/* The positions of a 4x4 block, and the block count of the luma and the chroma planes. */
#define TOKENS_POSITIONS	16U
#define TOKENS_LUMA_BLOCKS	16U
#define TOKENS_CHROMA_BLOCKS	8U

/* The tree node a token read starts from: the root, or the node after the EOB branch. */
#define TOKENS_ROOT_NODE	0U
#define TOKENS_NO_EOB_NODE	2U

/* The probability of the sign bit of a non-zero value. */
#define TOKENS_SIGN_PROB	128U

/* The number of context entries of the left side and the above side that belong to the Y, U and V blocks. */
#define TOKENS_PLANE_CONTEXTS	8U

/* The index of the Y2 block's entry in the left and above contexts. */
#define TOKENS_Y2_CONTEXT	8U

static void tokens_context_indexes(unsigned block, unsigned *left_index, unsigned *above_index);
static unsigned tokens_read_extra(struct webpc_bool *tokens, unsigned category);
static int16_t tokens_dequantize(int value, int32_t factor);
static unsigned tokens_read_block(struct webpc_bool *tokens, const uint8_t probs[WEBPC_VP8_COEFF_BANDS][WEBPC_VP8_COEFF_CONTEXTS][WEBPC_VP8_COEFF_NODES], unsigned first, unsigned context, const int32_t factors[2], int16_t coeffs[16]);
static void tokens_code_block(struct webpc_bool *tokens, const struct webpc_vp8_header *header, unsigned type, unsigned block, unsigned first, const int32_t factors[2], uint8_t left[WEBPC_VP8_CONTEXTS], uint8_t above[WEBPC_VP8_CONTEXTS], struct webpc_vp8_residue *residue);

/*
 * Reads a macroblock's tokens from its token partition (RFC 6386 13, 13.2,
 * 13.3, 20.16) and dequantizes them with dequant (14.1).
 *
 * Procedure referenced: RFC 6386 (November 2011) section 20.16
 * (decode_mb_tokens: the order of the blocks, the first position and the
 * type of each, and the context handed on); section 13.3's pseudocode is
 * not followed where it differs.
 */
void
webpc_vp8_read_residue(
	struct webpc_bool *tokens,
	const struct webpc_vp8_header *header,
	const struct webpc_vp8_dequant *dequant,
	int has_y2,
	uint8_t left[WEBPC_VP8_CONTEXTS],
	uint8_t above[WEBPC_VP8_CONTEXTS],
	struct webpc_vp8_residue *residue)
{
	unsigned block;
	unsigned first_luma;
	unsigned luma_type;

	/* Starts with no coefficient and no block coded. */
	memset(residue, 0, sizeof(*residue));

	/*
	 * A macroblock with Y2 codes it first and then the luma blocks
	 * without their DC; one without codes the luma blocks whole.
	 */
	first_luma = 0U;
	luma_type = WEBPC_VP8_TYPE_Y_WITH_DC;
	if (has_y2) {
		tokens_code_block(tokens, header, WEBPC_VP8_TYPE_Y2, WEBPC_VP8_BLOCK_Y2, 0U, dequant->y2, left, above, residue);
		first_luma = 1U;
		luma_type = WEBPC_VP8_TYPE_Y_AFTER_Y2;
	}

	/* Reads the 16 luma blocks in raster order. */
	for (block = WEBPC_VP8_BLOCK_Y; block < WEBPC_VP8_BLOCK_Y + TOKENS_LUMA_BLOCKS; block++) {
		tokens_code_block(tokens, header, luma_type, block, first_luma, dequant->y1, left, above, residue);
	}

	/* Reads the 4 U blocks and then the 4 V blocks, which follow the luma in the numbering. */
	for (block = WEBPC_VP8_BLOCK_U; block < WEBPC_VP8_BLOCK_U + TOKENS_CHROMA_BLOCKS; block++) {
		tokens_code_block(tokens, header, WEBPC_VP8_TYPE_CHROMA, block, 0U, dequant->uv, left, above, residue);
	}
}

/*
 * Sets the contexts of a macroblock whose mb_skip_coeff is 1 (no tokens
 * read; RFC 6386 20.16 reset_mb_context): the Y, U and V entries of left and
 * above become 0, and the Y2 entries too unless has_y2 is 0 (a B_PRED
 * macroblock leaves the Y2 contexts as they were).
 */
void
webpc_vp8_skip_residue(
	int has_y2,
	uint8_t left[WEBPC_VP8_CONTEXTS],
	uint8_t above[WEBPC_VP8_CONTEXTS])
{
	unsigned index;

	/* Clears the entries of the Y, U and V blocks on both sides. */
	for (index = 0U; index < TOKENS_PLANE_CONTEXTS; index++) {
		left[index] = 0U;
		above[index] = 0U;
	}

	/* Clears the Y2 entries only when this macroblock would have coded a Y2 block. */
	if (has_y2) {
		left[TOKENS_Y2_CONTEXT] = 0U;
		above[TOKENS_Y2_CONTEXT] = 0U;
	}
}

/*
 * Gives the indexes of the left and above context entries a block uses.
 *
 * Values referenced: RFC 6386 (November 2011) section 20.16
 * (left_context_index and above_context_index, for the 25 blocks).
 */
static void
tokens_context_indexes(
	unsigned block,
	unsigned *left_index,
	unsigned *above_index)
{
	/* The Y2 block has one entry on each side. */
	if (block == WEBPC_VP8_BLOCK_Y2) {
		*left_index = TOKENS_Y2_CONTEXT;
		*above_index = TOKENS_Y2_CONTEXT;
		return;
	}

	/* The luma blocks lie 4 across: a row shares a left entry, a column an above entry. */
	if (block < WEBPC_VP8_BLOCK_U) {
		*left_index = block / 4U;
		*above_index = block % 4U;
		return;
	}

	/* The U blocks lie 2 across, after the 4 luma entries. */
	if (block < WEBPC_VP8_BLOCK_V) {
		*left_index = 4U + (block - WEBPC_VP8_BLOCK_U) / 2U;
		*above_index = 4U + (block - WEBPC_VP8_BLOCK_U) % 2U;
		return;
	}

	/* The V blocks lie 2 across, after the U entries. */
	*left_index = 6U + (block - WEBPC_VP8_BLOCK_V) / 2U;
	*above_index = 6U + (block - WEBPC_VP8_BLOCK_V) % 2U;
}

/*
 * Reads the extra bits of a category token and gives its magnitude.
 *
 * Values referenced: RFC 6386 (November 2011) section 13.2 (Pcat1 to
 * Pcat6, the bases of the ranges); procedure referenced: the same section
 * (DCTextra: the bits come most significant first).
 */
static unsigned
tokens_read_extra(
	struct webpc_bool *tokens,
	unsigned category)
{
	const uint8_t *probs;
	unsigned offset;
	unsigned bit;

	/* Takes one bit for each probability of the category's row, which ends at a 0. */
	probs = webpc_vp8_cat_probs[category];
	offset = 0U;
	while (*probs != 0U) {
		bit = (unsigned)webpc_bool_read(tokens, *probs);
		offset = offset * 2U + bit;
		probs++;
	}

	/* The magnitude is the offset above the category's smallest value. */
	return (unsigned)webpc_vp8_cat_base[category] + offset;
}

/*
 * Multiplies a coefficient by its factor and wraps it to the 16 bits the
 * reference decoder stores (RFC 6386 14.1).
 */
static int16_t
tokens_dequantize(
	int value,
	int32_t factor)
{
	int64_t product;
	int32_t reduced;
	int32_t wrapped;

	/*
	 * The product is made in 64 bits (the factor is the caller's), cut to
	 * a multiple of 65536 away from the 16-bit range, and wrapped into it.
	 */
	product = (int64_t)value * (int64_t)factor;
	reduced = (int32_t)(product % 65536);
	wrapped = webpc_wrap16(reduced);

	/* The wrapped value is in the range of a 16-bit integer. */
	return (int16_t)wrapped;
}

/*
 * Reads the tokens of one 4x4 block from position first, storing the
 * dequantized values in raster order, and tells whether any token other
 * than an immediate EOB was read.
 *
 * Procedure referenced: RFC 6386 (November 2011) sections 13.2, 13.3 and
 * 20.16 (decode_mb_tokens: the band of the position, the context of the
 * previous value, no EOB branch after a zero, no EOB after position 15).
 */
static unsigned
tokens_read_block(
	struct webpc_bool *tokens,
	const uint8_t probs[WEBPC_VP8_COEFF_BANDS][WEBPC_VP8_COEFF_CONTEXTS][WEBPC_VP8_COEFF_NODES],
	unsigned first,
	unsigned context,
	const int32_t factors[2],
	int16_t coeffs[16])
{
	unsigned position;
	unsigned band;
	unsigned start_node;
	unsigned advanced;
	unsigned magnitude;
	unsigned factor_index;
	int token;
	int value;

	/* Walks the positions until an EOB or the last position. */
	position = first;
	start_node = TOKENS_ROOT_NODE;
	advanced = 0U;
	while (position < TOKENS_POSITIONS) {
		/* Reads the token with the probabilities of this position's band and the context. */
		band = webpc_vp8_coeff_bands[position];
		token = webpc_bool_tree(tokens, webpc_vp8_coeff_tree, probs[band][context], start_node);

		/* An EOB ends the block; it is not a token that advanced the block. */
		if ((unsigned)token == WEBPC_VP8_DCT_EOB)
			break;

		/* Any other token, a zero too, moves the block past its first position. */
		advanced = 1U;

		/* A zero stores nothing, makes the next context 0 and takes the EOB branch away. */
		if ((unsigned)token == WEBPC_VP8_DCT_0) {
			context = 0U;
			start_node = TOKENS_NO_EOB_NODE;
			position++;
			continue;
		}

		/* A category token adds its extra bits to the base of its range, the others are their own value. */
		if ((unsigned)token >= WEBPC_VP8_DCT_CAT1)
			magnitude = tokens_read_extra(tokens, (unsigned)token - WEBPC_VP8_DCT_CAT1);
		else
			magnitude = (unsigned)token;

		/* The sign comes after the magnitude, a 1 meaning negative. */
		value = (int)magnitude;
		if (webpc_bool_read(tokens, TOKENS_SIGN_PROB) != 0)
			value = -value;

		/* The first position takes the DC factor and every later one the AC factor. */
		factor_index = 1U;
		if (position == 0U)
			factor_index = 0U;

		/* Stores the dequantized value at the position's place in raster order. */
		coeffs[webpc_vp8_zigzag[position]] = tokens_dequantize(value, factors[factor_index]);

		/* The next position's context is the size of this value: 1 for a one, 2 for more. */
		if (magnitude == 1U)
			context = 1U;
		else
			context = 2U;
		start_node = TOKENS_ROOT_NODE;
		position++;
	}

	/* Reports whether the block held more than an immediate EOB. */
	return advanced;
}

/*
 * Reads one block of the macroblock with the context its neighbours give,
 * stores its coefficients and updates both context entries and the coded
 * bits.
 */
static void
tokens_code_block(
	struct webpc_bool *tokens,
	const struct webpc_vp8_header *header,
	unsigned type,
	unsigned block,
	unsigned first,
	const int32_t factors[2],
	uint8_t left[WEBPC_VP8_CONTEXTS],
	uint8_t above[WEBPC_VP8_CONTEXTS],
	struct webpc_vp8_residue *residue)
{
	unsigned left_index;
	unsigned above_index;
	unsigned context;
	unsigned advanced;

	/* Finds the neighbours' entries; any non-zero entry counts as 1, so a bad caller's value stays in the table. */
	tokens_context_indexes(block, &left_index, &above_index);
	context = 0U;
	if (left[left_index] != 0U)
		context++;
	if (above[above_index] != 0U)
		context++;

	/* Reads the block's tokens. */
	advanced = tokens_read_block(tokens, header->coeff_probs[type], first, context, factors, residue->coeffs[block]);

	/* Hands the block's value to the macroblocks and blocks that follow, and records it as coded. */
	left[left_index] = (uint8_t)advanced;
	above[above_index] = (uint8_t)advanced;
	if (advanced != 0U) {
		residue->coded |= (uint32_t)1U << block;
		residue->coded |= (uint32_t)1U << 31;
	}
}
