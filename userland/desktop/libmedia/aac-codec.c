/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* Original prefix trees and AAC tuple/sign/escape syntax over normative integer codewords. */

#include "aac-codec.h"
#include "aac-huffman.h"

#include <errno.h>
#include <pthread.h>
#include <string.h>

/* A full binary prefix tree with at most 289 leaves uses at most 577 nodes. */
#define AAC_TREE_NODES 577U

/* One branch or leaf, owned by its book for the library lifetime after once publication. */
struct aac_tree_node {
	uint16_t child[2];
	int16_t symbol;
};

/* Original lookup storage; pthread_once publishes all books before any reader traverses them. */
static struct aac_tree_node aac_trees[12][AAC_TREE_NODES];

/* Initialize immutable trees once, shared by decoder threads without per-frame allocation. */
static pthread_once_t aac_tree_once = PTHREAD_ONCE_INIT;

/* A failed normative-table validation remains visible after once initialization finishes. */
static int aac_tree_error;

static void aac_tree_initialize(void);
static int aac_tree_insert(unsigned book, const struct media_aac_codeword *word, unsigned *used);
static int aac_escape(struct media_bits *bits, int *magnitude);

/*
 * Decode one symbol from a complete AAC codebook without reading beyond its leaf.
 */
int
media_aac_huffman_symbol(
	struct media_bits *bits,
	unsigned book,
	uint16_t *symbol)
{
	unsigned node;
	unsigned depth;
	uint32_t branch;
	int error;

	/* Only the twelve normative books have symbol syntax here. */
	if (bits == NULL || symbol == NULL)
		return EINVAL;

	/* Invalid book numbers never index the lookup storage. */
	if (book >= 12U)
		return EINVAL;

	/* Publish the original trees before reading any of their nodes. */
	error = pthread_once(&aac_tree_once, aac_tree_initialize);
	if (error != 0)
		return error;

	/* A malformed generated table must not become an arbitrary decoded coefficient. */
	if (aac_tree_error != 0)
		return aac_tree_error;

	/* Consume only the path to the next leaf, with a bounded maximum code length. */
	node = 0U;
	for (depth = 0U; depth < 32U; depth++) {
		/* A missing input bit makes the caller's reader sticky-failed. */
		branch = media_bits_read1(bits);
		if (bits->error != 0)
			return EINVAL;

		/* Zero is the absent-child sentinel; allocated children always start at one. */
		node = aac_trees[book][node].child[branch];
		if (node == 0U) {
			bits->error = 1;
			return EINVAL;
		}

		/* A leaf ends traversal without reading any bit of the following field. */
		if (aac_trees[book][node].symbol >= 0)
			break;
	}

	/* No leaf in thirty-two bits means invalid syntax, never a partial symbol. */
	if (aac_trees[book][node].symbol < 0) {
		bits->error = 1;
		return EINVAL;
	}

	/* Succeeded: publish exactly the normative index of the reached leaf. */
	*symbol = (uint16_t)aac_trees[book][node].symbol;
	return 0;
}

/*
 * Decode a scalefactor difference in the normative range minus sixty to sixty.
 */
int
media_aac_scalefactor(
	struct media_bits *bits,
	int *difference)
{
	uint16_t symbol;
	int error;

	/* A result destination is required before consuming a codeword. */
	if (difference == NULL)
		return EINVAL;

	/* Book zero numbers its differences around the zero-difference symbol sixty. */
	error = media_aac_huffman_symbol(bits, 0U, &symbol);
	if (error != 0)
		return error;

	/* Succeeded: the caller receives the signed difference. */
	*difference = (int)symbol - 60;
	return 0;
}

/*
 * Decode a spectral pair or quad, including its nonzero signs and escape magnitudes.
 */
int
media_aac_spectral(
	struct media_bits *bits,
	unsigned book,
	int16_t coefficients[4],
	unsigned *count)
{
	int decoded[4];
	uint16_t symbol;
	unsigned dimension;
	unsigned radix;
	unsigned index;
	unsigned remainder;
	int offset;
	int magnitude;
	int sign;
	uint32_t negative;
	int error;

	/* Special zero, noise and intensity books have no spectral Huffman tuples. */
	if (bits == NULL || coefficients == NULL || count == NULL)
		return EINVAL;

	/* Restrict the book before interpreting its radix or tuple dimension. */
	if (book == 0U || book > 11U)
		return EINVAL;

	/* Books one through four encode quads; the remaining books encode pairs. */
	dimension = 2U;
	if (book <= 4U)
		dimension = 4U;

	/* Signed books include the sign in their index; unsigned books append sign bits. */
	offset = 0;
	radix = 17U;
	if (book <= 2U) {
		radix = 3U;
		offset = -1;
	} else if (book <= 4U) {
		radix = 3U;
	} else if (book <= 6U) {
		radix = 9U;
		offset = -4;
	} else if (book <= 8U) {
		radix = 8U;
	} else if (book <= 10U) {
		radix = 13U;
	}

	/* Decode a symbol before translating its lexical tuple index. */
	error = media_aac_huffman_symbol(bits, book, &symbol);
	if (error != 0)
		return error;

	/* Lower-frequency coefficients occupy the most significant radix digits. */
	memset(decoded, 0, sizeof(decoded));
	remainder = symbol;
	for (index = dimension; index != 0U; index--) {
		decoded[index - 1U] = (int)(remainder % radix) + offset;
		remainder /= radix;
	}

	/* Unsigned books transmit all nonzero sign bits before either escape payload. */
	if (offset == 0) {
		/* Zero coefficients have no sign bit and must not consume the next field. */
		for (index = 0U; index < dimension; index++) {
			/* A sign belongs only to a coefficient with nonzero magnitude. */
			if (decoded[index] != 0) {
				negative = media_bits_read1(bits);
				if (bits->error != 0)
					return EINVAL;

				/* One denotes a negative coefficient in AAC's spectral syntax. */
				if (negative != 0U)
					decoded[index] = -decoded[index];
			}
		}
	}

	/* Only book eleven extends magnitudes equal to sixteen. */
	if (book == 11U) {
		/* Escape payloads follow coefficient order after the complete sign field. */
		for (index = 0U; index < dimension; index++) {
			/* Positive and negative sixteen share the same magnitude extension. */
			if (decoded[index] == 16 || decoded[index] == -16) {
				sign = 1;

				/* Retain the previously decoded sign while replacing the magnitude. */
				if (decoded[index] < 0)
					sign = -1;

				/* Decode a normative magnitude bounded by 8191. */
				error = aac_escape(bits, &magnitude);
				if (error != 0)
					return error;
				decoded[index] = sign * magnitude;
			}
		}
	}

	/* Succeeded: publish all coefficients together, clearing unused pair entries. */
	for (index = 0U; index < 4U; index++)
		coefficients[index] = (int16_t)decoded[index];
	*count = dimension;
	return 0;
}

/* Build bounded, immutable prefix trees from the normative literal codewords. */
static void
aac_tree_initialize(
	void)
{
	unsigned book;
	unsigned index;
	unsigned used;
	int error;

	/* A negative symbol marks a branch; zero child links mean absent paths. */
	for (book = 0U; book < 12U; book++) {
		/* Initialize every possible branch before inserting this book's leaves. */
		for (index = 0U; index < AAC_TREE_NODES; index++)
			aac_trees[book][index].symbol = -1;

		/* Allocate node zero as the root, then insert each distinct normative leaf. */
		used = 1U;
		for (index = 0U; index < media_aac_huffman_books[book].count; index++) {
			/* Symbols must fit this book before they become signed leaf indices. */
			if (media_aac_huffman_books[book].words[index].symbol >= media_aac_huffman_books[book].count) {
				aac_tree_error = EINVAL;
				return;
			}

			/* Insert the numeric word without accepting any duplicate or overlapping prefix. */
			error = aac_tree_insert(book, &media_aac_huffman_books[book].words[index], &used);
			if (error != 0) {
				aac_tree_error = error;
				return;
			}
		}
	}

	/* Succeeded: pthread_once publishes every initialized book to subsequent readers. */
	return;
}

/* Insert one codeword, rejecting collisions or any inconsistent generated prefix. */
static int
aac_tree_insert(
	unsigned book,
	const struct media_aac_codeword *word,
	unsigned *used)
{
	unsigned node;
	unsigned remaining;
	unsigned branch;
	unsigned next;

	/* A codeword must fit its declared nonzero width. */
	if (word->length == 0U || word->length > 32U)
		return EINVAL;

	/* A thirty-two-bit word needs no shift by its full integer width. */
	if (word->length < 32U && word->word >= ((uint32_t)1U << word->length))
		return EINVAL;

	/* Walk most significant bits first, allocating branches within the static bound. */
	node = 0U;
	for (remaining = word->length; remaining != 0U; remaining--) {
		/* Another leaf cannot serve as a prefix of this word. */
		if (aac_trees[book][node].symbol >= 0)
			return EINVAL;

		/* Select the next branch from the remaining most significant codeword bit. */
		branch = (word->word >> (remaining - 1U)) & 1U;
		next = aac_trees[book][node].child[branch];

		/* Unallocated paths receive a new branch, preserving the zero sentinel. */
		if (next == 0U) {
			/* The maximum full tree must fit even the largest codebook. */
			if (*used == AAC_TREE_NODES)
				return EINVAL;
			next = *used;
			(*used)++;
			aac_trees[book][node].child[branch] = (uint16_t)next;
		}

		/* Continue this word's traversal through the now-present branch. */
		node = next;
	}

	/* Duplicate words and prefixes of earlier words are both inconsistent. */
	if (aac_trees[book][node].symbol >= 0)
		return EINVAL;

	/* A leaf cannot replace an existing branch with either child. */
	if (aac_trees[book][node].child[0] != 0U || aac_trees[book][node].child[1] != 0U)
		return EINVAL;

	/* Succeeded: the new leaf names this word's normative symbol. */
	aac_trees[book][node].symbol = (int16_t)word->symbol;
	return 0;
}

/* Read a book-eleven escape magnitude without accepting coefficients above 8191. */
static int
aac_escape(
	struct media_bits *bits,
	int *magnitude)
{
	unsigned width;
	uint32_t bit;
	uint32_t suffix;

	/* Each prefix one extends the suffix width, starting at four bits. */
	width = 4U;
	for (;;) {
		bit = media_bits_read1(bits);
		if (bits->error != 0)
			return EINVAL;

		/* Zero separates the unary prefix from its unsigned suffix. */
		if (bit == 0U)
			break;
		width++;

		/* Thirteen-bit magnitudes exceed AAC's normative quantized spectral domain. */
		if (width > 12U) {
			bits->error = 1;
			return EINVAL;
		}
	}

	/* The implicit leading one plus the suffix yields a magnitude from sixteen to 8191. */
	suffix = media_bits_read(bits, width);
	if (bits->error != 0)
		return EINVAL;

	/* Succeeded: the original sign can be applied without narrowing overflow. */
	*magnitude = (int)(((uint32_t)1U << width) + suffix);
	return 0;
}
