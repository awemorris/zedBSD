/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The machine encoding of the PowerVR Rogue unified shading cluster's
 * instruction groups and of the operations placed in them.
 */

#include "drivers/gpu/powervr/compiler/usc-encode.h"
#include <uapi/errno.h>

/* The largest number of fields one encoded form carries. */
#define USC_FIELD_MAX		13U

/* The field number that always holds the value one, for fixed set bits. */
#define USC_FIELD_ONE		15U

/* The length of a sum-of-products operation, in bytes. */
#define USC_SUM_LENGTH		14U

/* The operand fields of a source or destination word. */
#define OPERAND_BANK_A		0U
#define OPERAND_INDEX_A		1U
#define OPERAND_BANK_B		2U
#define OPERAND_INDEX_B		3U
#define OPERAND_BANK_C		4U
#define OPERAND_INDEX_C		5U
#define OPERAND_MUX		6U
#define OPERAND_FIELD_COUNT	7U

/* An operand limit meaning the operand must be absent from the form. */
#define OPERAND_ABSENT		(-1)

/*
 * One run of bits of one field, placed in one byte of an encoded form.
 *
 * The run holds the field's bits from shift upwards; a field wider than
 * one byte, or split across bytes, has one piece per run.
 */
struct usc_piece {
	uint8_t byte;
	uint8_t low;
	uint8_t width;
	uint8_t field;
	uint8_t shift;
};

/*
 * The layout of one encoded form: its size and where each field's bits
 * go.  Every field has a width that bounds the value the caller passes.
 */
struct usc_layout {
	uint8_t length;
	uint8_t field_count;
	uint8_t piece_count;
	uint8_t field_bits[USC_FIELD_MAX];
	const struct usc_piece *pieces;
};

/*
 * One form of an operand word: the layout and, per operand, the widest
 * bank and index the form can name.
 *
 * A limit of OPERAND_ABSENT means the operand must not be used with
 * this form; the forms are tried in order and the first that fits wins.
 */
struct usc_operand_form {
	struct usc_layout layout;
	int8_t limit[OPERAND_FIELD_COUNT];
};

/*
 * The decisions made about one group before its bytes are written.
 */
struct usc_group_plan {
	unsigned header_length;
	const struct usc_operand_form *lower;
	const struct usc_operand_form *upper;
	const struct usc_operand_form *destination;
	unsigned internal_length;
	unsigned order;
	unsigned decode_assist;
	unsigned length;
	unsigned word_padding;
};

/*
 * The lower source forms, tried in this order.
 *
 * Values referenced:
 * Mesa 26.2.5 src/imagination/pco/pco_isa.py
 * (I_SRC pieces and fields; I_ONE_LO_1B6I, I_ONE_LO_3B11I_2M,
 * I_TWO_LO_1B6I_1B5I, I_TWO_LO_2B7I_2B7I_2M, I_TWO_LO_3B11I_2B8I_3M,
 * I_THREE_LO_2B7I_2B7I_2B6I_2M, I_THREE_LO_3B8I_2B8I_3B8I_3M,
 * I_THREE_LO_3B11I_2B8I_3B11I_3M with their SrcSpec bit counts), and
 * src/imagination/pco/pco_map.h.py (pco_igrp_src_variant, the order in
 * which the forms are tried).
 */
static const struct usc_piece lower_1_pieces[] = {
	{ 0, 6, 1, OPERAND_BANK_A, 0 },
	{ 0, 0, 6, OPERAND_INDEX_A, 0 }
};
static const struct usc_piece lower_2_pieces[] = {
	{ 0, 7, 1, USC_FIELD_ONE, 0 },
	{ 0, 6, 1, OPERAND_BANK_A, 0 },
	{ 1, 2, 2, OPERAND_BANK_A, 1 },
	{ 0, 0, 6, OPERAND_INDEX_A, 0 },
	{ 1, 0, 2, OPERAND_INDEX_A, 6 },
	{ 2, 0, 3, OPERAND_INDEX_A, 8 },
	{ 1, 4, 2, OPERAND_MUX, 0 }
};
static const struct usc_piece lower_3_pieces[] = {
	{ 0, 7, 1, USC_FIELD_ONE, 0 },
	{ 1, 7, 1, USC_FIELD_ONE, 0 },
	{ 0, 6, 1, OPERAND_BANK_A, 0 },
	{ 0, 0, 6, OPERAND_INDEX_A, 0 },
	{ 1, 5, 1, OPERAND_BANK_B, 0 },
	{ 1, 0, 5, OPERAND_INDEX_B, 0 }
};
static const struct usc_piece lower_4_pieces[] = {
	{ 0, 7, 1, USC_FIELD_ONE, 0 },
	{ 1, 7, 1, USC_FIELD_ONE, 0 },
	{ 1, 6, 1, USC_FIELD_ONE, 0 },
	{ 0, 6, 1, OPERAND_BANK_A, 0 },
	{ 2, 4, 1, OPERAND_BANK_A, 1 },
	{ 0, 0, 6, OPERAND_INDEX_A, 0 },
	{ 2, 2, 1, OPERAND_INDEX_A, 6 },
	{ 1, 5, 1, OPERAND_BANK_B, 0 },
	{ 2, 3, 1, OPERAND_BANK_B, 1 },
	{ 1, 0, 5, OPERAND_INDEX_B, 0 },
	{ 2, 0, 2, OPERAND_INDEX_B, 5 },
	{ 2, 5, 2, OPERAND_MUX, 0 }
};
static const struct usc_piece lower_5_pieces[] = {
	{ 0, 7, 1, USC_FIELD_ONE, 0 },
	{ 1, 7, 1, USC_FIELD_ONE, 0 },
	{ 1, 6, 1, USC_FIELD_ONE, 0 },
	{ 2, 7, 1, USC_FIELD_ONE, 0 },
	{ 0, 6, 1, OPERAND_BANK_A, 0 },
	{ 2, 4, 1, OPERAND_BANK_A, 1 },
	{ 3, 3, 1, OPERAND_BANK_A, 2 },
	{ 0, 0, 6, OPERAND_INDEX_A, 0 },
	{ 2, 2, 1, OPERAND_INDEX_A, 6 },
	{ 3, 1, 1, OPERAND_INDEX_A, 7 },
	{ 3, 5, 3, OPERAND_INDEX_A, 8 },
	{ 1, 5, 1, OPERAND_BANK_B, 0 },
	{ 2, 3, 1, OPERAND_BANK_B, 1 },
	{ 1, 0, 5, OPERAND_INDEX_B, 0 },
	{ 2, 0, 2, OPERAND_INDEX_B, 5 },
	{ 3, 0, 1, OPERAND_INDEX_B, 7 },
	{ 2, 5, 2, OPERAND_MUX, 0 },
	{ 3, 4, 1, OPERAND_MUX, 2 }
};
static const struct usc_piece lower_6_pieces[] = {
	{ 0, 7, 1, USC_FIELD_ONE, 0 },
	{ 1, 6, 1, USC_FIELD_ONE, 0 },
	{ 0, 6, 1, OPERAND_BANK_A, 0 },
	{ 2, 4, 1, OPERAND_BANK_A, 1 },
	{ 0, 0, 6, OPERAND_INDEX_A, 0 },
	{ 2, 2, 1, OPERAND_INDEX_A, 6 },
	{ 1, 5, 1, OPERAND_BANK_B, 0 },
	{ 2, 3, 1, OPERAND_BANK_B, 1 },
	{ 1, 0, 5, OPERAND_INDEX_B, 0 },
	{ 2, 0, 2, OPERAND_INDEX_B, 5 },
	{ 3, 6, 2, OPERAND_BANK_C, 0 },
	{ 3, 0, 6, OPERAND_INDEX_C, 0 },
	{ 2, 5, 2, OPERAND_MUX, 0 }
};
static const struct usc_piece lower_7_pieces[] = {
	{ 0, 7, 1, USC_FIELD_ONE, 0 },
	{ 1, 6, 1, USC_FIELD_ONE, 0 },
	{ 2, 7, 1, USC_FIELD_ONE, 0 },
	{ 0, 6, 1, OPERAND_BANK_A, 0 },
	{ 2, 4, 1, OPERAND_BANK_A, 1 },
	{ 4, 3, 1, OPERAND_BANK_A, 2 },
	{ 0, 0, 6, OPERAND_INDEX_A, 0 },
	{ 2, 2, 1, OPERAND_INDEX_A, 6 },
	{ 4, 1, 1, OPERAND_INDEX_A, 7 },
	{ 1, 5, 1, OPERAND_BANK_B, 0 },
	{ 2, 3, 1, OPERAND_BANK_B, 1 },
	{ 1, 0, 5, OPERAND_INDEX_B, 0 },
	{ 2, 0, 2, OPERAND_INDEX_B, 5 },
	{ 4, 0, 1, OPERAND_INDEX_B, 7 },
	{ 3, 6, 2, OPERAND_BANK_C, 0 },
	{ 4, 7, 1, OPERAND_BANK_C, 2 },
	{ 3, 0, 6, OPERAND_INDEX_C, 0 },
	{ 4, 5, 2, OPERAND_INDEX_C, 6 },
	{ 2, 5, 2, OPERAND_MUX, 0 },
	{ 4, 4, 1, OPERAND_MUX, 2 }
};
static const struct usc_piece lower_8_pieces[] = {
	{ 0, 7, 1, USC_FIELD_ONE, 0 },
	{ 1, 6, 1, USC_FIELD_ONE, 0 },
	{ 2, 7, 1, USC_FIELD_ONE, 0 },
	{ 4, 2, 1, USC_FIELD_ONE, 0 },
	{ 0, 6, 1, OPERAND_BANK_A, 0 },
	{ 2, 4, 1, OPERAND_BANK_A, 1 },
	{ 4, 3, 1, OPERAND_BANK_A, 2 },
	{ 0, 0, 6, OPERAND_INDEX_A, 0 },
	{ 2, 2, 1, OPERAND_INDEX_A, 6 },
	{ 4, 1, 1, OPERAND_INDEX_A, 7 },
	{ 5, 0, 3, OPERAND_INDEX_A, 8 },
	{ 1, 5, 1, OPERAND_BANK_B, 0 },
	{ 2, 3, 1, OPERAND_BANK_B, 1 },
	{ 1, 0, 5, OPERAND_INDEX_B, 0 },
	{ 2, 0, 2, OPERAND_INDEX_B, 5 },
	{ 4, 0, 1, OPERAND_INDEX_B, 7 },
	{ 3, 6, 2, OPERAND_BANK_C, 0 },
	{ 4, 7, 1, OPERAND_BANK_C, 2 },
	{ 3, 0, 6, OPERAND_INDEX_C, 0 },
	{ 4, 5, 2, OPERAND_INDEX_C, 6 },
	{ 5, 3, 3, OPERAND_INDEX_C, 8 },
	{ 2, 5, 2, OPERAND_MUX, 0 },
	{ 4, 4, 1, OPERAND_MUX, 2 }
};

/*
 * The upper source forms, tried in this order.  They carry no source
 * selector; its bits are zero.
 *
 * Values referenced:
 * Mesa 26.2.5 src/imagination/pco/pco_isa.py
 * (I_SRC pieces and fields; I_ONE_UP_1B6I, I_ONE_UP_3B11I,
 * I_TWO_UP_1B6I_1B5I, I_TWO_UP_2B7I_2B7I, I_TWO_UP_3B11I_2B8I,
 * I_THREE_UP_2B7I_2B7I_2B6I, I_THREE_UP_3B8I_2B8I_2B8I,
 * I_THREE_UP_3B11I_2B8I_2B8I with their SrcSpec bit counts).
 */
static const struct usc_piece upper_7_pieces[] = {
	{ 0, 7, 1, USC_FIELD_ONE, 0 },
	{ 1, 6, 1, USC_FIELD_ONE, 0 },
	{ 2, 7, 1, USC_FIELD_ONE, 0 },
	{ 0, 6, 1, OPERAND_BANK_A, 0 },
	{ 2, 4, 1, OPERAND_BANK_A, 1 },
	{ 4, 3, 1, OPERAND_BANK_A, 2 },
	{ 0, 0, 6, OPERAND_INDEX_A, 0 },
	{ 2, 2, 1, OPERAND_INDEX_A, 6 },
	{ 4, 1, 1, OPERAND_INDEX_A, 7 },
	{ 1, 5, 1, OPERAND_BANK_B, 0 },
	{ 2, 3, 1, OPERAND_BANK_B, 1 },
	{ 1, 0, 5, OPERAND_INDEX_B, 0 },
	{ 2, 0, 2, OPERAND_INDEX_B, 5 },
	{ 4, 0, 1, OPERAND_INDEX_B, 7 },
	{ 3, 6, 2, OPERAND_BANK_C, 0 },
	{ 3, 0, 6, OPERAND_INDEX_C, 0 },
	{ 4, 5, 2, OPERAND_INDEX_C, 6 }
};
static const struct usc_piece upper_8_pieces[] = {
	{ 0, 7, 1, USC_FIELD_ONE, 0 },
	{ 1, 6, 1, USC_FIELD_ONE, 0 },
	{ 2, 7, 1, USC_FIELD_ONE, 0 },
	{ 4, 2, 1, USC_FIELD_ONE, 0 },
	{ 0, 6, 1, OPERAND_BANK_A, 0 },
	{ 2, 4, 1, OPERAND_BANK_A, 1 },
	{ 4, 3, 1, OPERAND_BANK_A, 2 },
	{ 0, 0, 6, OPERAND_INDEX_A, 0 },
	{ 2, 2, 1, OPERAND_INDEX_A, 6 },
	{ 4, 1, 1, OPERAND_INDEX_A, 7 },
	{ 5, 0, 3, OPERAND_INDEX_A, 8 },
	{ 1, 5, 1, OPERAND_BANK_B, 0 },
	{ 2, 3, 1, OPERAND_BANK_B, 1 },
	{ 1, 0, 5, OPERAND_INDEX_B, 0 },
	{ 2, 0, 2, OPERAND_INDEX_B, 5 },
	{ 4, 0, 1, OPERAND_INDEX_B, 7 },
	{ 3, 6, 2, OPERAND_BANK_C, 0 },
	{ 3, 0, 6, OPERAND_INDEX_C, 0 },
	{ 4, 5, 2, OPERAND_INDEX_C, 6 }
};

/*
 * The source forms with their limits, lower forms first.  The upper
 * forms 1 to 6 reuse the lower layouts with the selector absent, since
 * the selector bits are the only difference.
 */
static const struct usc_operand_form lower_forms[] = {
	{ { 1, OPERAND_FIELD_COUNT, 2, { 3, 11, 3, 11, 3, 11, 3 }, lower_1_pieces }, { 1, 6, OPERAND_ABSENT, OPERAND_ABSENT, OPERAND_ABSENT, OPERAND_ABSENT, OPERAND_ABSENT } },
	{ { 3, OPERAND_FIELD_COUNT, 7, { 3, 11, 3, 11, 3, 11, 3 }, lower_2_pieces }, { 3, 11, OPERAND_ABSENT, OPERAND_ABSENT, OPERAND_ABSENT, OPERAND_ABSENT, 2 } },
	{ { 2, OPERAND_FIELD_COUNT, 6, { 3, 11, 3, 11, 3, 11, 3 }, lower_3_pieces }, { 1, 6, 1, 5, OPERAND_ABSENT, OPERAND_ABSENT, OPERAND_ABSENT } },
	{ { 3, OPERAND_FIELD_COUNT, 12, { 3, 11, 3, 11, 3, 11, 3 }, lower_4_pieces }, { 2, 7, 2, 7, OPERAND_ABSENT, OPERAND_ABSENT, 2 } },
	{ { 4, OPERAND_FIELD_COUNT, 18, { 3, 11, 3, 11, 3, 11, 3 }, lower_5_pieces }, { 3, 11, 2, 8, OPERAND_ABSENT, OPERAND_ABSENT, 3 } },
	{ { 4, OPERAND_FIELD_COUNT, 13, { 3, 11, 3, 11, 3, 11, 3 }, lower_6_pieces }, { 2, 7, 2, 7, 2, 6, 2 } },
	{ { 5, OPERAND_FIELD_COUNT, 20, { 3, 11, 3, 11, 3, 11, 3 }, lower_7_pieces }, { 3, 8, 2, 8, 3, 8, 3 } },
	{ { 6, OPERAND_FIELD_COUNT, 23, { 3, 11, 3, 11, 3, 11, 3 }, lower_8_pieces }, { 3, 11, 2, 8, 3, 11, 3 } }
};
static const struct usc_operand_form upper_forms[] = {
	{ { 1, OPERAND_FIELD_COUNT, 2, { 3, 11, 3, 11, 3, 11, 3 }, lower_1_pieces }, { 1, 6, OPERAND_ABSENT, OPERAND_ABSENT, OPERAND_ABSENT, OPERAND_ABSENT, OPERAND_ABSENT } },
	{ { 3, OPERAND_FIELD_COUNT, 6, { 3, 11, 3, 11, 3, 11, 3 }, lower_2_pieces }, { 3, 11, OPERAND_ABSENT, OPERAND_ABSENT, OPERAND_ABSENT, OPERAND_ABSENT, OPERAND_ABSENT } },
	{ { 2, OPERAND_FIELD_COUNT, 6, { 3, 11, 3, 11, 3, 11, 3 }, lower_3_pieces }, { 1, 6, 1, 5, OPERAND_ABSENT, OPERAND_ABSENT, OPERAND_ABSENT } },
	{ { 3, OPERAND_FIELD_COUNT, 11, { 3, 11, 3, 11, 3, 11, 3 }, lower_4_pieces }, { 2, 7, 2, 7, OPERAND_ABSENT, OPERAND_ABSENT, OPERAND_ABSENT } },
	{ { 4, OPERAND_FIELD_COUNT, 16, { 3, 11, 3, 11, 3, 11, 3 }, lower_5_pieces }, { 3, 11, 2, 8, OPERAND_ABSENT, OPERAND_ABSENT, OPERAND_ABSENT } },
	{ { 4, OPERAND_FIELD_COUNT, 12, { 3, 11, 3, 11, 3, 11, 3 }, lower_6_pieces }, { 2, 7, 2, 7, 2, 6, OPERAND_ABSENT } },
	{ { 5, OPERAND_FIELD_COUNT, 17, { 3, 11, 3, 11, 3, 11, 3 }, upper_7_pieces }, { 3, 8, 2, 8, 2, 8, OPERAND_ABSENT } },
	{ { 6, OPERAND_FIELD_COUNT, 19, { 3, 11, 3, 11, 3, 11, 3 }, upper_8_pieces }, { 3, 11, 2, 8, 2, 8, OPERAND_ABSENT } }
};

/*
 * The destination forms.  The first two name one destination in the A
 * fields, the rest name both, the first in A and the second in B.
 *
 * Values referenced:
 * Mesa 26.2.5 src/imagination/pco/pco_isa.py
 * (I_DST pieces and fields; I_ONE_1B6I, I_ONE_3B11I, I_TWO_1B7I_1B6I,
 * I_TWO_3B8I_3B8I, I_TWO_3B11I_3B11I with their DstSpec bit counts), and
 * src/imagination/pco/pco_map.h.py (pco_igrp_dest_variant).
 */
static const struct usc_piece destination_1_pieces[] = {
	{ 0, 6, 1, OPERAND_BANK_A, 0 },
	{ 0, 0, 6, OPERAND_INDEX_A, 0 }
};
static const struct usc_piece destination_2_pieces[] = {
	{ 0, 7, 1, USC_FIELD_ONE, 0 },
	{ 0, 6, 1, OPERAND_BANK_A, 0 },
	{ 1, 2, 2, OPERAND_BANK_A, 1 },
	{ 0, 0, 6, OPERAND_INDEX_A, 0 },
	{ 1, 0, 2, OPERAND_INDEX_A, 6 },
	{ 1, 4, 3, OPERAND_INDEX_A, 8 }
};
static const struct usc_piece destination_3_pieces[] = {
	{ 0, 7, 1, OPERAND_BANK_A, 0 },
	{ 0, 0, 7, OPERAND_INDEX_A, 0 },
	{ 1, 6, 1, OPERAND_BANK_B, 0 },
	{ 1, 0, 6, OPERAND_INDEX_B, 0 }
};
static const struct usc_piece destination_4_pieces[] = {
	{ 1, 7, 1, USC_FIELD_ONE, 0 },
	{ 0, 7, 1, OPERAND_BANK_A, 0 },
	{ 2, 1, 2, OPERAND_BANK_A, 1 },
	{ 0, 0, 7, OPERAND_INDEX_A, 0 },
	{ 2, 0, 1, OPERAND_INDEX_A, 7 },
	{ 1, 6, 1, OPERAND_BANK_B, 0 },
	{ 2, 5, 2, OPERAND_BANK_B, 1 },
	{ 1, 0, 6, OPERAND_INDEX_B, 0 },
	{ 2, 3, 2, OPERAND_INDEX_B, 6 }
};
static const struct usc_piece destination_5_pieces[] = {
	{ 1, 7, 1, USC_FIELD_ONE, 0 },
	{ 2, 7, 1, USC_FIELD_ONE, 0 },
	{ 0, 7, 1, OPERAND_BANK_A, 0 },
	{ 2, 1, 2, OPERAND_BANK_A, 1 },
	{ 0, 0, 7, OPERAND_INDEX_A, 0 },
	{ 2, 0, 1, OPERAND_INDEX_A, 7 },
	{ 3, 0, 3, OPERAND_INDEX_A, 8 },
	{ 1, 6, 1, OPERAND_BANK_B, 0 },
	{ 2, 5, 2, OPERAND_BANK_B, 1 },
	{ 1, 0, 6, OPERAND_INDEX_B, 0 },
	{ 2, 3, 2, OPERAND_INDEX_B, 6 },
	{ 3, 3, 3, OPERAND_INDEX_B, 8 }
};
static const struct usc_operand_form single_destination_forms[] = {
	{ { 1, OPERAND_FIELD_COUNT, 2, { 3, 11, 3, 11, 3, 11, 3 }, destination_1_pieces }, { 1, 6, OPERAND_ABSENT, OPERAND_ABSENT, OPERAND_ABSENT, OPERAND_ABSENT, OPERAND_ABSENT } },
	{ { 2, OPERAND_FIELD_COUNT, 6, { 3, 11, 3, 11, 3, 11, 3 }, destination_2_pieces }, { 3, 11, OPERAND_ABSENT, OPERAND_ABSENT, OPERAND_ABSENT, OPERAND_ABSENT, OPERAND_ABSENT } }
};
static const struct usc_operand_form dual_destination_forms[] = {
	{ { 2, OPERAND_FIELD_COUNT, 4, { 3, 11, 3, 11, 3, 11, 3 }, destination_3_pieces }, { 1, 7, 1, 6, OPERAND_ABSENT, OPERAND_ABSENT, OPERAND_ABSENT } },
	{ { 3, OPERAND_FIELD_COUNT, 9, { 3, 11, 3, 11, 3, 11, 3 }, destination_4_pieces }, { 3, 8, 3, 8, OPERAND_ABSENT, OPERAND_ABSENT, OPERAND_ABSENT } },
	{ { 4, OPERAND_FIELD_COUNT, 12, { 3, 11, 3, 11, 3, 11, 3 }, destination_5_pieces }, { 3, 11, 3, 11, OPERAND_ABSENT, OPERAND_ABSENT, OPERAND_ABSENT } }
};

/*
 * The phase organisations of a main group: which slots a group uses,
 * as a bit per slot (phase 0, 1, 2, back end), mapped to the header's
 * organisation code.  A combination missing here cannot be encoded.
 *
 * Values referenced:
 * Mesa 26.2.5 src/imagination/pco/pco_isa.py
 * (F_OPORG: p0 0b000, p2 0b001, be 0b010, p0_p1 0b011, p0_p2 0b100,
 * p0_p1_p2 0b101, p0_p2_be 0b110, p0_p1_p2_be 0b111).
 */
static const int8_t main_organisation[16] = {
	-1, 0, -1, 3, 1, 4, -1, 5,
	2, -1, -1, -1, -1, 6, -1, 7
};

/*
 * The sum-of-products operations, one per form of enum powervr_usc_sum,
 * derived from observation.  The first byte is the main unit's single
 * operation with its extension; the rest select, for each channel, the
 * sources and halves of its multiply-add and their modifiers.
 */
static const uint8_t sum_operations[POWERVR_USC_SUM_COUNT][USC_SUM_LENGTH] = {
	{ 0x94, 0x99, 0x99, 0x0f, 0x06, 0x4f, 0x32, 0x42, 0x00, 0x00, 0x0b, 0x99, 0x99, 0xf0 },
	{ 0x94, 0x22, 0x22, 0x0f, 0xfe, 0x7f, 0xa8, 0xb9, 0xd0, 0x00, 0x0f, 0x73, 0x24, 0x00 },
	{ 0x94, 0x22, 0x22, 0x0f, 0xfe, 0x7f, 0xa8, 0xb9, 0xdf, 0x00, 0x0f, 0x73, 0x24, 0x00 },
	{ 0x94, 0x99, 0x99, 0x0f, 0xfe, 0x4f, 0xa8, 0xb9, 0x20, 0x00, 0x0f, 0x42, 0x73, 0x00 },
	{ 0x94, 0x42, 0x73, 0x00, 0x06, 0x7f, 0xa8, 0xb9, 0xc0, 0x00, 0x6f, 0x99, 0x99, 0xf0 },
	{ 0x94, 0x99, 0x99, 0x0f, 0x06, 0x4f, 0xa0, 0xb1, 0x00, 0x00, 0x00, 0x99, 0x99, 0xf0 },
	{ 0x94, 0xbb, 0xbb, 0x0f, 0xf6, 0x7f, 0xa8, 0xb9, 0xc0, 0x00, 0x0f, 0xa3, 0xb4, 0x00 }
};

static int layout_place(const struct usc_layout *layout, const uint32_t *values, uint8_t *bytes);
static unsigned bits_needed(uint32_t value);
static int register_bits(const struct powervr_usc_register *reg, int *bank_bits, int *index_bits);
static int operand_choose(const struct usc_operand_form *forms, size_t form_count, const struct powervr_usc_register *operands, unsigned operand_count, int mux, const struct usc_operand_form **chosen);
static void operand_values(const struct powervr_usc_register *operands, unsigned operand_count, int mux, uint32_t *values);
static int group_plan(const struct powervr_usc_group *group, struct usc_group_plan *plan);
static int group_check(const struct powervr_usc_group *group);
static int group_write(const struct powervr_usc_group *group, const struct usc_group_plan *plan, unsigned alignment, uint8_t *output);
static void header_write(const struct powervr_usc_group *group, const struct usc_group_plan *plan, unsigned halfwords, uint8_t *output);
static int operation_build(const struct usc_layout *layout, const uint32_t *values, struct powervr_usc_operation *operation);
static int flag_check(unsigned flag);
static int array_fetch(void *context, size_t index, struct powervr_usc_group *group);

/*
 * Reports how many bytes a group takes before any program alignment.
 */
int
powervr_usc_group_size(
	const struct powervr_usc_group *group,
	size_t *size)
{
	struct usc_group_plan plan;
	int error;

	/* Refuses a missing group or result. */
	if (group == NULL || size == NULL)
		return EINVAL;

	/* Chooses the operand forms and works out the group's length. */
	error = group_plan(group, &plan);
	if (error != 0)
		return error;

	/* Succeeded: the group's length including its word padding. */
	*size = plan.length;
	return 0;
}

/*
 * Encodes a program's instruction groups, padded for the instruction cache.
 *
 * The last group is padded so the program ends on the cache alignment,
 * and the one before it is padded so the last group starts on it, as the
 * hardware fetches the final group whole.  On failure nothing written to
 * the output is meaningful.
 */
int
powervr_usc_program_encode(
	const struct powervr_usc_group *groups,
	size_t count,
	uint8_t *output,
	size_t room,
	size_t *length)
{
	int error;

	/* Refuses a missing group array. */
	if (groups == NULL)
		return EINVAL;

	/* Encodes the array through the fetching form. */
	error = powervr_usc_program_build(count, array_fetch, (void *)(uintptr_t)groups, output, room, length);
	if (error != 0)
		return error;

	/* Succeeded: the program is encoded. */
	return 0;
}

/*
 * Encodes a program whose groups a callback hands over one at a time.
 *
 * The callback is asked for each group twice, once to measure and once
 * to write, and must give the same group both times.  A long program
 * whose groups repeat needs no array this way.  Padding and failure are
 * as for powervr_usc_program_encode().
 */
int
powervr_usc_program_build(
	size_t count,
	int (*fetch)(void *context, size_t index, struct powervr_usc_group *group),
	void *context,
	uint8_t *output,
	size_t room,
	size_t *length)
{
	struct powervr_usc_group group;
	struct usc_group_plan plan;
	size_t offset;
	size_t last_offset;
	unsigned last_padding;
	unsigned previous_padding;
	unsigned remainder;
	unsigned padding;
	size_t i;
	int error;

	/* Refuses a missing or empty program. */
	if (fetch == NULL || count == 0 || output == NULL || length == NULL)
		return EINVAL;

	/* Measures every group to find where the last one starts. */
	offset = 0;
	last_offset = 0;
	last_padding = 0;
	for (i = 0; i < count; i++) {
		error = fetch(context, i, &group);
		if (error != 0)
			return error;

		error = group_plan(&group, &plan);
		if (error != 0)
			return error;

		last_offset = offset;
		offset += plan.length;
	}

	/* Pads the last group so the program ends on the cache alignment. */
	remainder = (unsigned)((offset - last_offset) % POWERVR_USC_PROGRAM_ALIGN);
	if (remainder != 0)
		last_padding = POWERVR_USC_PROGRAM_ALIGN - remainder;

	/*
	 * Pads the group before the last so the last starts on the
	 * alignment.  A single group starts at zero and never needs it.
	 */
	previous_padding = 0;
	remainder = (unsigned)(last_offset % POWERVR_USC_PROGRAM_ALIGN);
	if (remainder != 0)
		previous_padding = POWERVR_USC_PROGRAM_ALIGN - remainder;

	/* Refuses an output that cannot hold the padded program. */
	if (offset + last_padding + previous_padding > room)
		return ENOSPC;

	/* Writes each group with the padding it carries. */
	offset = 0;
	for (i = 0; i < count; i++) {
		error = fetch(context, i, &group);
		if (error != 0)
			return error;

		error = group_plan(&group, &plan);
		if (error != 0)
			return error;

		padding = 0;
		if (i + 1 == count)
			padding = last_padding;
		else if (i + 2 == count)
			padding = previous_padding;

		error = group_write(&group, &plan, padding, output + offset);
		if (error != 0)
			return error;

		offset += plan.length + padding;
	}

	/* Succeeded: the program's padded length. */
	*length = offset;
	return 0;
}

/*
 * Encodes one of the main unit's two-source floating-point operations.
 *
 * The add and multiply forms differ only in their class; the first
 * source may be negated, made absolute and floored, the second made
 * absolute, and the result saturated.
 */
int
powervr_usc_float_pair(
	enum powervr_usc_float_pair kind,
	unsigned saturate,
	unsigned negate_0,
	unsigned absolute_0,
	unsigned absolute_1,
	unsigned floor_0,
	struct powervr_usc_operation *operation)
{
	/*
	 * Values referenced:
	 * Mesa 26.2.5 src/imagination/pco/pco_isa.py
	 * (I_MAIN pieces main_op 0:7..5, sat_fam 0:4, s0neg_fam 0:3,
	 * s0abs_fam 0:2, s1abs_fam 0:1, s0flr_fam 0:0; I_FADD, I_FADD_LP,
	 * I_FMUL, I_FMUL_LP).
	 */
	static const struct usc_piece pieces[] = {
		{ 0, 5, 3, 0, 0 },
		{ 0, 4, 1, 1, 0 },
		{ 0, 3, 1, 2, 0 },
		{ 0, 2, 1, 3, 0 },
		{ 0, 1, 1, 4, 0 },
		{ 0, 0, 1, 5, 0 }
	};
	static const struct usc_layout layout = { 1, 6, 6, { 2, 1, 1, 1, 1, 1 }, pieces };
	uint32_t values[6];
	int error;

	/* Collects the fields in the layout's order. */
	values[0] = (uint32_t)kind;
	values[1] = saturate;
	values[2] = negate_0;
	values[3] = absolute_0;
	values[4] = absolute_1;
	values[5] = floor_0;

	/* Places the fields after checking each fits. */
	error = operation_build(&layout, values, operation);
	if (error != 0)
		return error;

	/* Succeeded: the one-byte operation. */
	return 0;
}

/*
 * Encodes the main unit's fused multiply-add.
 *
 * The one-byte form carries the first source's and the addend's
 * modifiers; the two-byte form adds the second source's modifiers, the
 * addend's floor and absolute value, and low precision.  The short form
 * is used whenever the extra modifiers are all clear.
 */
int
powervr_usc_float_multiply_add(
	unsigned saturate,
	unsigned negate_0,
	unsigned absolute_0,
	unsigned negate_1,
	unsigned absolute_1,
	unsigned negate_2,
	unsigned absolute_2,
	unsigned floor_2,
	unsigned low_precision,
	struct powervr_usc_operation *operation)
{
	/*
	 * Values referenced:
	 * Mesa 26.2.5 src/imagination/pco/pco_isa.py
	 * (I_MAIN pieces main_op 0:7..5 with fmad_movc 0b110, ext0 0:4,
	 * s0neg_fma 0:3, s0abs_fma 0:2, s2neg_fma 0:1, sat_fma 0:0,
	 * rsvd1_fma 1:7..5, lp_fma 1:4, s1abs_fma 1:3, s1neg_fma 1:2,
	 * s2flr_fma 1:1, s2abs_fma 1:0; I_FMAD, I_FMAD_EXT), and
	 * src/imagination/pco/pco_map.py (encode_map O_FMAD: the short form
	 * when the extra modifiers are clear).
	 */
	static const struct usc_piece pieces[] = {
		{ 0, 5, 3, 0, 0 },
		{ 0, 4, 1, 1, 0 },
		{ 0, 3, 1, 2, 0 },
		{ 0, 2, 1, 3, 0 },
		{ 0, 1, 1, 4, 0 },
		{ 0, 0, 1, 5, 0 },
		{ 1, 4, 1, 6, 0 },
		{ 1, 3, 1, 7, 0 },
		{ 1, 2, 1, 8, 0 },
		{ 1, 1, 1, 9, 0 },
		{ 1, 0, 1, 10, 0 }
	};
	static const struct usc_layout short_layout = { 1, 11, 6, { 3, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1 }, pieces };
	static const struct usc_layout long_layout = { 2, 11, 11, { 3, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1 }, pieces };
	const struct usc_layout *layout;
	uint32_t values[11];
	int error;

	/* Collects the fields in the layout's order. */
	values[0] = 6;
	values[1] = 0;
	values[2] = negate_0;
	values[3] = absolute_0;
	values[4] = negate_2;
	values[5] = saturate;
	values[6] = low_precision;
	values[7] = absolute_1;
	values[8] = negate_1;
	values[9] = floor_2;
	values[10] = absolute_2;

	/* Chooses the long form only for the modifiers the short one lacks. */
	layout = &short_layout;
	if (low_precision != 0 ||
	    absolute_1 != 0 ||
	    negate_1 != 0 ||
	    floor_2 != 0 ||
	    absolute_2 != 0) {
		layout = &long_layout;
		values[1] = 1;
	}

	/* Places the fields after checking each fits. */
	error = operation_build(layout, values, operation);
	if (error != 0)
		return error;

	/* Succeeded: the short or long multiply-add. */
	return 0;
}

/*
 * Encodes one of the main unit's single-source operations.
 *
 * The short form has no source modifiers; the long one negates or makes
 * the source absolute.
 */
int
powervr_usc_single(
	enum powervr_usc_single kind,
	unsigned negate,
	unsigned absolute,
	struct powervr_usc_operation *operation)
{
	/*
	 * Values referenced:
	 * Mesa 26.2.5 src/imagination/pco/pco_isa.py
	 * (I_MAIN pieces main_op 0:7..5 with sngl 0b100, ext0 0:4,
	 * sngl_op 0:3..0, rsvd1_sngl over red_part 1:7, iter 1:6..4,
	 * red_type 1:3, rsvd1_sngl_ over pwen_red 1:2, s0neg_sngl 1:1,
	 * s0abs_sngl 1:0; I_SNGL, I_SNGL_EXT).
	 */
	static const struct usc_piece pieces[] = {
		{ 0, 5, 3, 0, 0 },
		{ 0, 4, 1, 1, 0 },
		{ 0, 0, 4, 2, 0 },
		{ 1, 1, 1, 3, 0 },
		{ 1, 0, 1, 4, 0 }
	};
	static const struct usc_layout short_layout = { 1, 5, 3, { 3, 1, 4, 1, 1 }, pieces };
	static const struct usc_layout long_layout = { 2, 5, 5, { 3, 1, 4, 1, 1 }, pieces };
	const struct usc_layout *layout;
	uint32_t values[5];
	int error;

	/* Collects the fields in the layout's order. */
	values[0] = 4;
	values[1] = 0;
	values[2] = (uint32_t)kind;
	values[3] = negate;
	values[4] = absolute;

	/* Chooses the long form only for a modified source. */
	layout = &short_layout;
	if (negate != 0 || absolute != 0) {
		layout = &long_layout;
		values[1] = 1;
	}

	/* Places the fields after checking each fits. */
	error = operation_build(layout, values, operation);
	if (error != 0)
		return error;

	/* Succeeded: the short or long single-source operation. */
	return 0;
}

/*
 * Encodes the main unit's pack of a value into a storage format.
 */
int
powervr_usc_pack(
	unsigned format,
	unsigned programmable,
	unsigned round_to_zero,
	unsigned scale,
	struct powervr_usc_operation *operation)
{
	/*
	 * Values referenced:
	 * Mesa 26.2.5 src/imagination/pco/pco_isa.py
	 * (I_MAIN pieces main_op 0:7..5 with sngl 0b100, ext0 0:4 set,
	 * sngl_op 0:3..0 with pck 0b1100, prog 1:7, rtz 1:6, scale 1:5,
	 * pck_format 1:4..0; I_PCK).
	 */
	static const struct usc_piece pieces[] = {
		{ 0, 5, 3, 0, 0 },
		{ 0, 4, 1, USC_FIELD_ONE, 0 },
		{ 0, 0, 4, 1, 0 },
		{ 1, 7, 1, 2, 0 },
		{ 1, 6, 1, 3, 0 },
		{ 1, 5, 1, 4, 0 },
		{ 1, 0, 5, 5, 0 }
	};
	static const struct usc_layout layout = { 2, 6, 7, { 3, 4, 1, 1, 1, 5 }, pieces };
	uint32_t values[6];
	int error;

	/* Collects the fields in the layout's order. */
	values[0] = 4;
	values[1] = 12;
	values[2] = programmable;
	values[3] = round_to_zero;
	values[4] = scale;
	values[5] = format;

	/* Places the fields after checking each fits. */
	error = operation_build(&layout, values, operation);
	if (error != 0)
		return error;

	/* Succeeded: the two-byte pack. */
	return 0;
}

/*
 * Encodes the main unit's unpack of a storage format into a float.
 */
int
powervr_usc_unpack(
	unsigned format,
	unsigned element,
	unsigned scale_round,
	struct powervr_usc_operation *operation)
{
	/*
	 * Values referenced:
	 * Mesa 26.2.5 src/imagination/pco/pco_isa.py
	 * (I_MAIN pieces main_op 0:7..5 with sngl 0b100, ext0 0:4 set,
	 * sngl_op 0:3..0 with pck 0b1100, upck_elem 1:7..6, scale_rtz 1:5,
	 * pck_format 1:4..0; I_UPCK).
	 */
	static const struct usc_piece pieces[] = {
		{ 0, 5, 3, 0, 0 },
		{ 0, 4, 1, USC_FIELD_ONE, 0 },
		{ 0, 0, 4, 1, 0 },
		{ 1, 6, 2, 2, 0 },
		{ 1, 5, 1, 3, 0 },
		{ 1, 0, 5, 4, 0 }
	};
	static const struct usc_layout layout = { 2, 5, 6, { 3, 4, 2, 1, 5 }, pieces };
	uint32_t values[5];
	int error;

	/* Collects the fields in the layout's order. */
	values[0] = 4;
	values[1] = 12;
	values[2] = element;
	values[3] = scale_round;
	values[4] = format;

	/* Places the fields after checking each fits. */
	error = operation_build(&layout, values, operation);
	if (error != 0)
		return error;

	/* Succeeded: the two-byte unpack. */
	return 0;
}

/*
 * Encodes the main unit's sum of products in one of its forms (enum
 * powervr_usc_sum): the operation of phase 0 alone, in a group without
 * internal selectors.  The encodings are derived from observation.
 */
int
powervr_usc_sum_of_products(
	enum powervr_usc_sum form,
	struct powervr_usc_operation *operation)
{
	unsigned index;

	/* Refuses a form this encoder does not build. */
	if ((unsigned)form >= POWERVR_USC_SUM_COUNT || operation == NULL)
		return EINVAL;

	/* The form's bytes. */
	for (index = 0; index < USC_SUM_LENGTH; index++)
		operation->bytes[index] = sum_operations[form][index];
	operation->length = USC_SUM_LENGTH;

	/* Succeeded: the fourteen-byte operation. */
	return 0;
}

/*
 * Encodes the back end's texture sample in its brief form: no
 * extensions, the coordinates as floats, normalised or not.
 */
int
powervr_usc_sample(
	unsigned normalised,
	unsigned data_return,
	unsigned dimensions,
	unsigned channels,
	unsigned lod_mode,
	struct powervr_usc_operation *operation)
{
	/*
	 * Values referenced:
	 * Mesa 26.2.5 src/imagination/pco/pco_isa.py
	 * (I_BACKEND pieces backend_op 0:7..5 with dma 0b111, fcnorm 0:4,
	 * drc 0:3, dma_op 0:2..0 with smp 0b100, extb 1:7, dmn 1:6..5
	 * (1d 0b01, 2d 0b10, 3d 0b11), exta 1:4, chan 1:3..2 sent less one
	 * (F_UINT2_POS_INC), lodm 1:1..0; I_SMP_BRIEF).
	 */
	static const struct usc_piece pieces[] = {
		{ 0, 5, 3, 0, 0 },
		{ 0, 4, 1, 1, 0 },
		{ 0, 3, 1, 2, 0 },
		{ 0, 0, 3, 3, 0 },
		{ 1, 5, 2, 4, 0 },
		{ 1, 2, 2, 5, 0 },
		{ 1, 0, 2, 6, 0 }
	};
	static const struct usc_layout layout = { 2, 7, 7, { 3, 1, 1, 3, 2, 2, 2 }, pieces };
	uint32_t values[7];
	int error;

	/* Refuses a dimension or a channel count the fields cannot carry. */
	if (dimensions < 1 || dimensions > 3 || channels < 1 || channels > 4)
		return EINVAL;

	/* Collects the fields in the layout's order, the channels less one. */
	values[0] = 7;
	values[1] = normalised;
	values[2] = data_return;
	values[3] = 4;
	values[4] = dimensions;
	values[5] = channels - 1U;
	values[6] = lod_mode;

	/* Places the fields after checking each fits. */
	error = operation_build(&layout, values, operation);
	if (error != 0)
		return error;

	/* Succeeded: the two-byte sample. */
	return 0;
}

/*
 * Encodes the back end's emission of the pixel the shader has written
 * to the pixel output registers.
 */
int
powervr_usc_emit_pixel(
	unsigned free_partition,
	struct powervr_usc_operation *operation)
{
	/*
	 * Values referenced:
	 * Mesa 26.2.5 src/imagination/pco/pco_isa.py
	 * (I_BACKEND pieces backend_op 0:7..5 with emit 0b110, freep 0:1,
	 * the reserved bits 0:4..2 and 0:0; I_EMITPIX).
	 */
	static const struct usc_piece pieces[] = {
		{ 0, 5, 3, 0, 0 },
		{ 0, 1, 1, 1, 0 }
	};
	static const struct usc_layout layout = { 1, 2, 2, { 3, 1 }, pieces };
	uint32_t values[2];
	int error;

	/* Collects the fields in the layout's order. */
	values[0] = 6;
	values[1] = free_partition;

	/* Places the fields after checking each fits. */
	error = operation_build(&layout, values, operation);
	if (error != 0)
		return error;

	/* Succeeded: the one-byte emission. */
	return 0;
}

/*
 * Encodes the back end's visibility test: the alpha test (alpha_test 1)
 * or the depth feedback (0), the predicate it writes (predicate_write),
 * and with feedback 1 the alpha test's own feedback to the depth and
 * visibility unit (Mesa's alphatst; 0 for alphaf, the feedback alone).
 */
int
powervr_usc_visibility_test(
	unsigned predicate_write,
	unsigned alpha_test,
	unsigned feedback,
	struct powervr_usc_operation *operation)
{
	/*
	 * Values referenced:
	 * Mesa 26.2.5 src/imagination/pco/pco_isa.py
	 * (I_BACKEND pieces backend_op 0:7..5 with vistest 0b100,
	 * rsvd0_vistest 0:4..3, pwen_vistest 0:2, vistest_op 0:1 with
	 * depthf 0 and atst 1, ifb 0:0; I_VISTEST_ATST, I_VISTEST_DEPTHF)
	 * and src/imagination/pco/pco_map.py (encode_map of O_ALPHAF: ifb
	 * false, of O_ALPHATST: ifb true, pwen when p0 is written).
	 */
	static const struct usc_piece pieces[] = {
		{ 0, 5, 3, 0, 0 },
		{ 0, 2, 1, 1, 0 },
		{ 0, 1, 1, 2, 0 },
		{ 0, 0, 1, 3, 0 }
	};
	static const struct usc_layout layout = { 1, 4, 4, { 3, 1, 1, 1 }, pieces };
	uint32_t values[4];
	int error;

	/* Collects the fields in the layout's order. */
	values[0] = 4;
	values[1] = predicate_write;
	values[2] = alpha_test;
	values[3] = feedback;

	/* Places the fields after checking each fits. */
	error = operation_build(&layout, values, operation);
	if (error != 0)
		return error;

	/* Succeeded: the one-byte test. */
	return 0;
}

/*
 * Encodes the control unit's direct iteration of fragment varyings
 * from coefficient registers into temporaries.
 *
 * Coefficient and w_coefficient are coefficient register numbers, each
 * a multiple of four; perspective multiplies by the iterated 1/w.
 */
int
powervr_usc_iterate(
	unsigned destination,
	unsigned coefficient,
	unsigned perspective,
	unsigned w_coefficient,
	unsigned mode,
	unsigned count,
	unsigned data_return,
	struct powervr_usc_operation *operation)
{
	/*
	 * Values referenced:
	 * Mesa 26.2.5 src/imagination/pco/pco_isa.py
	 * (I_CTRL pieces dest_7_0 0:7..0, coff_7_2 1:7..2, p_itr 1:1..0,
	 * woff_b2_7_2 2:7..2, ditr_mode 2:1..0, itr_count_b3 3:7..4 sent
	 * modulo 16, coff_idx_ctrl_b3 3:3..2, woff_idx_ctrl_b3 3:1..0,
	 * f16_b4 4:6, sched_ctrl_b4 4:3..2, drc_b4 4:1, sat 4:0;
	 * F_UINT6MUL4 sending a multiple of four quartered; F_PERSP_CTL
	 * none 0b00, iter_mul 0b01; I_DITR) and src/imagination/pco/pco_map.py
	 * (encode_map O_DITRP: p iter_mul).
	 */
	static const struct usc_piece pieces[] = {
		{ 0, 0, 8, 0, 0 },
		{ 1, 2, 6, 1, 0 },
		{ 1, 0, 2, 2, 0 },
		{ 2, 2, 6, 3, 0 },
		{ 2, 0, 2, 4, 0 },
		{ 3, 4, 4, 5, 0 },
		{ 4, 1, 1, 6, 0 }
	};
	static const struct usc_layout layout = { 5, 7, 7, { 8, 6, 2, 6, 2, 4, 1 }, pieces };
	uint32_t values[7];
	int error;

	/* Refuses coefficient numbers off the multiple of four and a count outside one to sixteen. */
	if ((coefficient & 3U) != 0 || (w_coefficient & 3U) != 0 || count < 1 || count > 16)
		return EINVAL;

	/* Collects the fields in the layout's order. */
	values[0] = destination;
	values[1] = coefficient >> 2;
	values[2] = perspective;
	values[3] = w_coefficient >> 2;
	values[4] = mode;
	values[5] = count % 16U;
	values[6] = data_return;

	/* Places the fields after checking each fits. */
	error = operation_build(&layout, values, operation);
	if (error != 0)
		return error;

	/* Succeeded: the five-byte iteration. */
	return 0;
}

/*
 * Encodes the control unit's iterate and sample:
 * the coordinates iterated from coefficient sets the way a direct
 * iteration does, then sampled with the image and sampler state the
 * shared registers hold, into temporaries from destination on.
 *
 * Coefficient, w_coefficient, image and sampler are register numbers,
 * each a multiple of four; perspective multiplies by the iterated 1/w;
 * channels are one to four; schedule is the scheduling control (0 none,
 * 1 swap, 2 wait for the data the operation returns); half returns the
 * channels as half floats, two to a register (the first in the low
 * half).
 */
int
powervr_usc_iterate_sample(
	unsigned destination,
	unsigned coefficient,
	unsigned perspective,
	unsigned w_coefficient,
	unsigned image,
	unsigned sampler,
	unsigned mode,
	unsigned dimensions,
	unsigned channels,
	unsigned normalised,
	unsigned data_return,
	unsigned schedule,
	unsigned half,
	struct powervr_usc_operation *operation)
{
	/*
	 * Values referenced:
	 * Mesa 26.2.5 src/imagination/pco/pco_isa.py (I_CTRL pieces
	 * dest_7_0 0:7..0, coff_7_2 1:7..2, p_itr 1:1..0, texoff_7_2
	 * 2:7..2, drc_b2 2:1, itrsmp_mode_0 2:0, smpoff_7_2 3:7..2, dmn
	 * 3:1..0, woff_b4_7_2 4:7..2, proj 4:1, sat 4:0, ext5 5:7, f16_b5
	 * 5:6, sched_ctrl_b5 5:5..4, chan 5:3..2 sent less one, nncoords
	 * 5:1, fcnorm 5:0; I_ITRSMP without ext5, six bytes; F_CTRLOP
	 * itrsmp 0b1000) and src/imagination/common/device_info/bxe-4-32.h
	 * (has_usc_itrsmp, has_usc_itrsmp_enhanced; F_SCHED_CTRL none 0b00,
	 * swap 0b01, wdf 0b10).  The fields left zero (projection,
	 * saturation, unnormalised coordinates) are not taken.
	 */
	static const struct usc_piece pieces[] = {
		{ 0, 0, 8, 0, 0 },
		{ 1, 2, 6, 1, 0 },
		{ 1, 0, 2, 2, 0 },
		{ 2, 2, 6, 3, 0 },
		{ 2, 1, 1, 4, 0 },
		{ 2, 0, 1, 5, 0 },
		{ 3, 2, 6, 6, 0 },
		{ 3, 0, 2, 7, 0 },
		{ 4, 2, 6, 8, 0 },
		{ 5, 2, 2, 9, 0 },
		{ 5, 0, 1, 10, 0 },
		{ 5, 4, 2, 11, 0 },
		{ 5, 6, 1, 12, 0 }
	};
	static const struct usc_layout layout = { 6, 13, 13, { 8, 6, 2, 6, 1, 1, 6, 2, 6, 2, 1, 2, 1 }, pieces };
	uint32_t values[13];
	int error;

	/* Refuses register numbers off the multiple of four, and a dimension or a channel count the fields cannot carry. */
	if ((coefficient & 3U) != 0 || (w_coefficient & 3U) != 0 || (image & 3U) != 0 || (sampler & 3U) != 0)
		return EINVAL;
	if (dimensions < 1 || dimensions > 3 || channels < 1 || channels > 4)
		return EINVAL;

	/* Collects the fields in the layout's order. */
	values[0] = destination;
	values[1] = coefficient >> 2;
	values[2] = perspective;
	values[3] = image >> 2;
	values[4] = data_return;
	values[5] = mode;
	values[6] = sampler >> 2;
	values[7] = dimensions;
	values[8] = w_coefficient >> 2;
	values[9] = channels - 1U;
	values[10] = normalised;
	values[11] = schedule;
	values[12] = half;

	/* Places the fields after checking each fits. */
	error = operation_build(&layout, values, operation);
	if (error != 0)
		return error;

	/* Succeeded: the six-byte iterate and sample. */
	return 0;
}

/*
 * Encodes the main unit's 32- and 64-bit integer add and multiply-add.
 *
 * The short form carries the signedness, the addend's negation and the
 * kind; the long form adds the carry input and the other modifiers.
 */
int
powervr_usc_integer(
	enum powervr_usc_integer kind,
	unsigned is_signed,
	unsigned carry_in,
	unsigned negate_0,
	unsigned absolute_0,
	unsigned negate_1,
	unsigned absolute_1,
	unsigned negate_2,
	unsigned absolute_2,
	struct powervr_usc_operation *operation)
{
	/*
	 * Values referenced:
	 * Mesa 26.2.5 src/imagination/pco/pco_isa.py
	 * (I_MAIN pieces main_op 0:7..5 with int32_64_tst 0b111, ext0 0:4,
	 * s_i3264 0:3, s2neg_i3264 0:2, int32_64_op 0:1..0,
	 * rsvd1_i3264 1:7 and 1:3, cin_i3264 1:6, s1neg_i3264 1:5,
	 * s0neg_i3264 1:4, s0abs_i3264 1:2, s1abs_i3264 1:1,
	 * s2abs_i3264 1:0; I_INT32_64, I_INT32_64_EXT).
	 */
	static const struct usc_piece pieces[] = {
		{ 0, 5, 3, 0, 0 },
		{ 0, 4, 1, 1, 0 },
		{ 0, 3, 1, 2, 0 },
		{ 0, 2, 1, 3, 0 },
		{ 0, 0, 2, 4, 0 },
		{ 1, 6, 1, 5, 0 },
		{ 1, 5, 1, 6, 0 },
		{ 1, 4, 1, 7, 0 },
		{ 1, 2, 1, 8, 0 },
		{ 1, 1, 1, 9, 0 },
		{ 1, 0, 1, 10, 0 }
	};
	static const struct usc_layout short_layout = { 1, 11, 5, { 3, 1, 1, 1, 2, 1, 1, 1, 1, 1, 1 }, pieces };
	static const struct usc_layout long_layout = { 2, 11, 11, { 3, 1, 1, 1, 2, 1, 1, 1, 1, 1, 1 }, pieces };
	const struct usc_layout *layout;
	uint32_t values[11];
	int error;

	/* Collects the fields in the layout's order. */
	values[0] = 7;
	values[1] = 0;
	values[2] = is_signed;
	values[3] = negate_2;
	values[4] = (uint32_t)kind;
	values[5] = carry_in;
	values[6] = negate_1;
	values[7] = negate_0;
	values[8] = absolute_0;
	values[9] = absolute_1;
	values[10] = absolute_2;

	/* Chooses the long form only for the inputs the short one lacks. */
	layout = &short_layout;
	if (carry_in != 0 ||
	    negate_1 != 0 ||
	    negate_0 != 0 ||
	    absolute_0 != 0 ||
	    absolute_1 != 0 ||
	    absolute_2 != 0) {
		layout = &long_layout;
		values[1] = 1;
	}

	/* Places the fields after checking each fits. */
	error = operation_build(layout, values, operation);
	if (error != 0)
		return error;

	/* Succeeded: the short or long integer operation. */
	return 0;
}

/*
 * Encodes the main unit's test, which compares and writes a predicate.
 *
 * The short form compares 32-bit floats with a three-bit comparison;
 * the long form names the type, the element and the full comparison.
 * The caller picks the form, since the short one means a float test.
 */
int
powervr_usc_test(
	unsigned comparison,
	unsigned type,
	unsigned element,
	unsigned predicate_write,
	unsigned phase_2_end,
	unsigned extended,
	struct powervr_usc_operation *operation)
{
	/*
	 * Values referenced:
	 * Mesa 26.2.5 src/imagination/pco/pco_isa.py
	 * (I_MAIN pieces main_op 0:7..5 with int32_64_tst 0b111, ext0 0:4,
	 * tst_op_2_0 0:3..1, pwen_tst 0:0, tst_type 1:7..5, p2end_tst 1:4,
	 * tst_elem 1:3..2, rsvd1_tst 1:1, tst_op_3 1:0; F_TST_OP values
	 * up to le 0b1001, F_TST_TYPE values up to s32 0b110; I_TST,
	 * I_TST_EXT).
	 */
	static const struct usc_piece pieces[] = {
		{ 0, 5, 3, 0, 0 },
		{ 0, 4, 1, 1, 0 },
		{ 0, 1, 3, 2, 0 },
		{ 0, 0, 1, 3, 0 },
		{ 1, 0, 1, 2, 3 },
		{ 1, 5, 3, 4, 0 },
		{ 1, 4, 1, 5, 0 },
		{ 1, 2, 2, 6, 0 }
	};
	static const struct usc_layout short_layout = { 1, 7, 4, { 3, 1, 3, 1, 3, 1, 2 }, pieces };
	static const struct usc_layout long_layout = { 2, 7, 8, { 3, 1, 4, 1, 3, 1, 2 }, pieces };
	const struct usc_layout *layout;
	uint32_t values[7];
	int error;

	/* Refuses comparisons and types the hardware does not define. */
	if (comparison > 9 || type > 6)
		return EINVAL;

	/* Collects the fields in the layout's order. */
	values[0] = 7;
	values[1] = 0;
	values[2] = comparison;
	values[3] = predicate_write;
	values[4] = type;
	values[5] = phase_2_end;
	values[6] = element;

	/* Uses the long form when asked, the short one only for a float test. */
	error = flag_check(extended);
	if (error != 0)
		return error;

	layout = &short_layout;
	if (extended != 0) {
		layout = &long_layout;
		values[1] = 1;
	} else if (type != 0 || phase_2_end != 0 || element != 0) {
		/* The short form cannot carry them. */
		return EINVAL;
	}

	/* Places the fields after checking each fits. */
	error = operation_build(layout, values, operation);
	if (error != 0)
		return error;

	/* Succeeded: the short or long test. */
	return 0;
}

/*
 * Encodes the main unit's conditional move in the second phase.
 *
 * The selectors pick the feed-through each of the two outputs takes; the
 * long form adds a write mask, the all-write flag and the phase-2 end.
 */
int
powervr_usc_move(
	unsigned select_1,
	unsigned select_0,
	unsigned mask_0,
	unsigned all_write,
	unsigned phase_2_end,
	unsigned extended,
	struct powervr_usc_operation *operation)
{
	/*
	 * Values referenced:
	 * Mesa 26.2.5 src/imagination/pco/pco_isa.py
	 * (I_MAIN pieces main_op 0:7..5 with fmad_movc 0b110, ext0 0:4,
	 * movw1 0:3..2, movw0 0:1..0, rsvd1_movc 1:7..6, maskw0 1:5..2,
	 * aw 1:1, p2end_movc 1:0; I_MOVC, I_MOVC_EXT).
	 */
	static const struct usc_piece pieces[] = {
		{ 0, 5, 3, 0, 0 },
		{ 0, 4, 1, 1, 0 },
		{ 0, 2, 2, 2, 0 },
		{ 0, 0, 2, 3, 0 },
		{ 1, 2, 4, 4, 0 },
		{ 1, 1, 1, 5, 0 },
		{ 1, 0, 1, 6, 0 }
	};
	static const struct usc_layout short_layout = { 1, 7, 4, { 3, 1, 2, 2, 4, 1, 1 }, pieces };
	static const struct usc_layout long_layout = { 2, 7, 7, { 3, 1, 2, 2, 4, 1, 1 }, pieces };
	const struct usc_layout *layout;
	uint32_t values[7];
	int error;

	/* Collects the fields in the layout's order. */
	values[0] = 6;
	values[1] = 0;
	values[2] = select_1;
	values[3] = select_0;
	values[4] = mask_0;
	values[5] = all_write;
	values[6] = phase_2_end;

	/* Uses the long form when asked; the short one cannot carry its fields. */
	error = flag_check(extended);
	if (error != 0)
		return error;

	layout = &short_layout;
	if (extended != 0) {
		layout = &long_layout;
		values[1] = 1;
	} else if (mask_0 != 0 || all_write != 0 || phase_2_end != 0) {
		/* The short form cannot carry them. */
		return EINVAL;
	}

	/* Places the fields after checking each fits. */
	error = operation_build(layout, values, operation);
	if (error != 0)
		return error;

	/* Succeeded: the short or long conditional move. */
	return 0;
}

/*
 * Encodes the back end's load from memory into the unified store.
 *
 * A nonzero burst (1 to 16 words) is carried in the operation; a zero
 * burst takes the length from the source the burst selector names.
 * The selectors name operand sources 0 to 5.
 */
int
powervr_usc_load(
	unsigned address_source,
	unsigned burst,
	unsigned burst_source,
	unsigned cache_mode,
	unsigned data_return,
	struct powervr_usc_operation *operation)
{
	/*
	 * Values referenced:
	 * Mesa 26.2.5 src/imagination/pco/pco_isa.py
	 * (I_BACKEND pieces backend_op 0:7..5 with dma 0b111, immbl 0:4,
	 * drc 0:3, dma_op 0:2..0 with ld 0b001, srcseladd_ldst 1:7..5,
	 * srcselbl_ldst and burstlen_2_0 1:4..2, cachemode_ldst 1:1..0,
	 * rsvd2_ld over 2:7..1, burstlen_3 2:0; F_UINT4_POS_WRAP sending 16
	 * as 0; F_CACHEMODE_LD values up to force_line_fill 0b10; I_LD_IMMBL,
	 * I_LD_REGBL).
	 */
	static const struct usc_piece pieces[] = {
		{ 0, 5, 3, 0, 0 },
		{ 0, 4, 1, 1, 0 },
		{ 0, 3, 1, 2, 0 },
		{ 0, 0, 3, 3, 0 },
		{ 1, 5, 3, 4, 0 },
		{ 1, 2, 3, 5, 0 },
		{ 1, 0, 2, 6, 0 },
		{ 2, 0, 1, 5, 3 }
	};
	static const struct usc_layout register_layout = { 2, 7, 7, { 3, 1, 1, 3, 3, 4, 2 }, pieces };
	static const struct usc_layout immediate_layout = { 3, 7, 8, { 3, 1, 1, 3, 3, 4, 2 }, pieces };
	const struct usc_layout *layout;
	uint32_t values[7];
	int error;

	/* Refuses selectors beyond the six sources, an undefined cache mode or burst. */
	if (address_source > 5 || burst_source > 5 || cache_mode > 2 || burst > 16)
		return EINVAL;

	/* Collects the fields in the layout's order. */
	values[0] = 7;
	values[1] = 0;
	values[2] = data_return;
	values[3] = 1;
	values[4] = address_source;
	values[5] = burst_source;
	values[6] = cache_mode;

	/* Carries an immediate burst in the operation, sixteen as zero. */
	layout = &register_layout;
	if (burst != 0) {
		/* An immediate burst leaves no room for a burst selector. */
		if (burst_source != 0)
			return EINVAL;

		layout = &immediate_layout;
		values[1] = 1;
		values[5] = burst % 16;
	}

	/* Places the fields after checking each fits. */
	error = operation_build(layout, values, operation);
	if (error != 0)
		return error;

	/* Succeeded: the load. */
	return 0;
}

/*
 * Encodes the back end's store from the unified store to memory.
 *
 * The burst is handled as for a load; the data selector names the
 * source holding the first word and the size picks 8, 16 or 32 bits.
 */
int
powervr_usc_store(
	unsigned address_source,
	unsigned burst,
	unsigned burst_source,
	unsigned data_source,
	unsigned data_size,
	unsigned cache_mode,
	unsigned data_return,
	struct powervr_usc_operation *operation)
{
	/*
	 * Values referenced:
	 * Mesa 26.2.5 src/imagination/pco/pco_isa.py
	 * (I_BACKEND pieces backend_op 0:7..5 with dma 0b111, immbl 0:4,
	 * drc 0:3, dma_op 0:2..0 with st 0b010, srcseladd_ldst 1:7..5,
	 * srcselbl_ldst and burstlen_2_0 1:4..2, cachemode_ldst 1:1..0,
	 * tiled 2:7, srcseldata 2:6..4, dsize 2:3..2, rsvd2_ldst 2:1,
	 * burstlen_3 2:0; F_DSIZE values up to 32bit 0b10; F_CACHEMODE_ST
	 * values up to write_back_lazy 0b10; I_ST_IMMBL, I_ST_REGBL).
	 */
	static const struct usc_piece pieces[] = {
		{ 0, 5, 3, 0, 0 },
		{ 0, 4, 1, 1, 0 },
		{ 0, 3, 1, 2, 0 },
		{ 0, 0, 3, 3, 0 },
		{ 1, 5, 3, 4, 0 },
		{ 1, 2, 3, 5, 0 },
		{ 1, 0, 2, 6, 0 },
		{ 2, 4, 3, 7, 0 },
		{ 2, 2, 2, 8, 0 },
		{ 2, 0, 1, 5, 3 }
	};
	static const struct usc_layout layout = { 3, 9, 10, { 3, 1, 1, 3, 3, 4, 2, 3, 2 }, pieces };
	static const struct usc_layout register_layout = { 3, 9, 9, { 3, 1, 1, 3, 3, 3, 2, 3, 2 }, pieces };
	const struct usc_layout *chosen;
	uint32_t values[9];
	int error;

	/* Refuses selectors beyond the six sources and undefined modes. */
	if (address_source > 5 ||
	    burst_source > 5 ||
	    data_source > 5 ||
	    data_size > 2 ||
	    cache_mode > 2 ||
	    burst > 16)
		return EINVAL;

	/* Collects the fields in the layout's order. */
	values[0] = 7;
	values[1] = 0;
	values[2] = data_return;
	values[3] = 2;
	values[4] = address_source;
	values[5] = burst_source;
	values[6] = cache_mode;
	values[7] = data_source;
	values[8] = data_size;

	/* Carries an immediate burst in the operation, sixteen as zero. */
	chosen = &register_layout;
	if (burst != 0) {
		/* An immediate burst leaves no room for a burst selector. */
		if (burst_source != 0)
			return EINVAL;

		chosen = &layout;
		values[1] = 1;
		values[5] = burst % 16;
	}

	/* Places the fields after checking each fits. */
	error = operation_build(chosen, values, operation);
	if (error != 0)
		return error;

	/* Succeeded: the three-byte store. */
	return 0;
}

/*
 * Encodes the back end's data fence on the address a source names.
 */
int
powervr_usc_fence(
	unsigned address_source,
	unsigned data_return,
	struct powervr_usc_operation *operation)
{
	/*
	 * Values referenced:
	 * Mesa 26.2.5 src/imagination/pco/pco_isa.py
	 * (I_BACKEND pieces backend_op 0:7..5 with dma 0b111, rsvd0_dma 0:4,
	 * drc 0:3, dma_op 0:2..0 with idf 0b000, rsvd1 1:7..3,
	 * srcsel 1:2..0; I_IDF).
	 */
	static const struct usc_piece pieces[] = {
		{ 0, 5, 3, 0, 0 },
		{ 0, 3, 1, 1, 0 },
		{ 1, 0, 3, 2, 0 }
	};
	static const struct usc_layout layout = { 2, 3, 3, { 3, 1, 3 }, pieces };
	uint32_t values[3];
	int error;

	/* Refuses a selector beyond the six sources. */
	if (address_source > 5)
		return EINVAL;

	/* Collects the fields in the layout's order. */
	values[0] = 7;
	values[1] = data_return;
	values[2] = address_source;

	/* Places the fields after checking each fits. */
	error = operation_build(&layout, values, operation);
	if (error != 0)
		return error;

	/* Succeeded: the two-byte fence. */
	return 0;
}

/*
 * Encodes the back end's atomic memory operation.
 *
 * The kind is the hardware's atomic code; the selectors name the source
 * holding the operands and the one receiving the old value.
 */
int
powervr_usc_atomic(
	unsigned kind,
	unsigned source,
	unsigned destination,
	unsigned data_return,
	struct powervr_usc_operation *operation)
{
	/*
	 * Values referenced:
	 * Mesa 26.2.5 src/imagination/pco/pco_isa.py
	 * (I_BACKEND pieces backend_op 0:7..5 with dma 0b111, rsvd0_dma 0:4,
	 * drc 0:3, dma_op 0:2..0 with atomic 0b101, atomic_op 1:7..4,
	 * rsvd1_atomic 1:3, srcsel 1:2..0, rsvd2_atomic 2:7..3,
	 * dstsel 2:2..0; F_ATOMIC_OP add 0b0000 to xor 0b1010 without 0b0011;
	 * I_ATOMIC).
	 */
	static const struct usc_piece pieces[] = {
		{ 0, 5, 3, 0, 0 },
		{ 0, 3, 1, 1, 0 },
		{ 0, 0, 3, 2, 0 },
		{ 1, 4, 4, 3, 0 },
		{ 1, 0, 3, 4, 0 },
		{ 2, 0, 3, 5, 0 }
	};
	static const struct usc_layout layout = { 3, 6, 6, { 3, 1, 3, 4, 3, 3 }, pieces };
	uint32_t values[6];
	int error;

	/* Refuses an undefined atomic code or a selector beyond the sources. */
	if (kind == 3 || kind > 10 || source > 5 || destination > 5)
		return EINVAL;

	/* Collects the fields in the layout's order. */
	values[0] = 7;
	values[1] = data_return;
	values[2] = 5;
	values[3] = kind;
	values[4] = source;
	values[5] = destination;

	/* Places the fields after checking each fits. */
	error = operation_build(&layout, values, operation);
	if (error != 0)
		return error;

	/* Succeeded: the three-byte atomic. */
	return 0;
}

/*
 * Encodes the back end's write to the vertex output buffer and its
 * emit, cut and end-of-task signals.
 *
 * Kinds 0 (write) and 6 (write, emit and end the task) take a word: from
 * the source the selector names, or at the immediate address.  The other
 * kinds take nothing.
 */
int
powervr_usc_vertex_output(
	unsigned kind,
	unsigned write_select,
	unsigned source,
	unsigned immediate,
	unsigned address,
	struct powervr_usc_operation *operation)
{
	/*
	 * Values referenced:
	 * Mesa 26.2.5 src/imagination/pco/pco_isa.py
	 * (I_BACKEND pieces backend_op 0:7..5 with uvsw 0b000, dsel 0:4,
	 * imm_uvsw 0:3, uvsw_op 0:2..0, rsvd1 1:7..3, srcsel 1:2..0,
	 * imm_addr_uvsw 1:7..0; F_UVSW_OP write 0b000 to
	 * write_emit_endtask 0b110; I_UVSW_WRITE_REG, I_UVSW_WRITE_IMM,
	 * I_UVSW_EMIT, I_UVSW_CUT, I_UVSW_EMIT_CUT, I_UVSW_ENDTASK,
	 * I_UVSW_EMIT_ENDTASK, I_UVSW_WRITE_EMIT_ENDTASK_REG,
	 * I_UVSW_WRITE_EMIT_ENDTASK_IMM).
	 */
	static const struct usc_piece pieces[] = {
		{ 0, 5, 3, 0, 0 },
		{ 0, 4, 1, 1, 0 },
		{ 0, 3, 1, 2, 0 },
		{ 0, 0, 3, 3, 0 },
		{ 1, 0, 3, 4, 0 },
		{ 1, 0, 8, 5, 0 }
	};
	static const struct usc_layout signal_layout = { 1, 6, 4, { 3, 1, 1, 3, 3, 8 }, pieces };
	static const struct usc_layout register_layout = { 2, 6, 5, { 3, 1, 1, 3, 3, 8 }, pieces };
	static const struct usc_piece immediate_pieces[] = {
		{ 0, 5, 3, 0, 0 },
		{ 0, 4, 1, 1, 0 },
		{ 0, 3, 1, 2, 0 },
		{ 0, 0, 3, 3, 0 },
		{ 1, 0, 8, 5, 0 }
	};
	static const struct usc_layout immediate_layout = { 2, 6, 5, { 3, 1, 1, 3, 3, 8 }, immediate_pieces };
	const struct usc_layout *layout;
	uint32_t values[6];
	int error;

	/* Refuses an undefined kind and a selector beyond the sources. */
	if (kind > 6 || source > 5)
		return EINVAL;

	/* Collects the fields in the layout's order. */
	values[0] = 0;
	values[1] = write_select;
	values[2] = immediate;
	values[3] = kind;
	values[4] = source;
	values[5] = address;

	error = flag_check(immediate);
	if (error != 0)
		return error;

	/* Picks the form the kind and the data's origin need. */
	if (kind == 0 || kind == 6) {
		/* A write takes the word from a register or an immediate address, not both. */
		layout = &register_layout;
		if (immediate != 0) {
			if (source != 0)
				return EINVAL;

			layout = &immediate_layout;
		} else if (address != 0) {
			/* The register form has no address field. */
			return EINVAL;
		}
	} else {
		/* A signal carries no data, no selector and no address. */
		if (write_select != 0 || source != 0 || immediate != 0 || address != 0)
			return EINVAL;

		layout = &signal_layout;
	}

	/* Places the fields after checking each fits. */
	error = operation_build(layout, values, operation);
	if (error != 0)
		return error;

	/* Succeeded: the vertex output operation. */
	return 0;
}

/*
 * Encodes the bitwise unit's first phase with its operand from a source.
 *
 * The count kind is 0 (count set bits), 1 (find the top bit) or 2
 * (pass); the shift kind is the first shifter's code; the mask flag
 * makes the bit mask from the source rather than pass it.
 */
int
powervr_usc_bit_count_shift(
	unsigned count_source,
	unsigned count_kind,
	unsigned mask_from_source,
	unsigned shift_kind,
	struct powervr_usc_operation *operation)
{
	/*
	 * Values referenced:
	 * Mesa 26.2.5 src/imagination/pco/pco_isa.py
	 * (I_BITWISE pieces ph0 0:7, csrc 0:6, cnt 0:5, ext0 0:4,
	 * shft1 0:3..2, cnt_byp 0:1, bm 0:0; count_op over cnt_byp and cnt;
	 * F_COUNT_OP cbs 0b00, ftb 0b01, byp 0b10; I_PHASE0_SRC).
	 */
	static const struct usc_piece pieces[] = {
		{ 0, 6, 1, 0, 0 },
		{ 0, 5, 1, 1, 0 },
		{ 0, 1, 1, 1, 1 },
		{ 0, 2, 2, 2, 0 },
		{ 0, 0, 1, 3, 0 }
	};
	static const struct usc_layout layout = { 1, 4, 5, { 1, 2, 2, 1 }, pieces };
	uint32_t values[4];
	int error;

	/* Refuses an undefined count kind. */
	if (count_kind > 2)
		return EINVAL;

	/* Collects the fields in the layout's order. */
	values[0] = count_source;
	values[1] = count_kind;
	values[2] = shift_kind;
	values[3] = mask_from_source;

	/* Places the fields after checking each fits. */
	error = operation_build(&layout, values, operation);
	if (error != 0)
		return error;

	/* Succeeded: the one-byte first phase. */
	return 0;
}

/*
 * Encodes the bitwise unit's first phase carrying an immediate.
 *
 * A wide immediate is 32 bits; otherwise the value must fit 16 bits.
 */
int
powervr_usc_bit_immediate(
	uint32_t value,
	unsigned wide,
	unsigned count_source,
	unsigned count_kind,
	unsigned shift_kind,
	struct powervr_usc_operation *operation)
{
	/*
	 * Values referenced:
	 * Mesa 26.2.5 src/imagination/pco/pco_isa.py
	 * (I_BITWISE pieces csrc 0:6, cnt 0:5, ext0 0:4 set, shft1 0:3..2,
	 * cnt_byp 0:1, bm 0:0 with byp16 0b0 and byp32 0b1,
	 * imm_7_0 1:7..0, imm_15_8 2:7..0, imm_23_16 3:7..0,
	 * imm_31_24 4:7..0; I_PHASE0_IMM16, I_PHASE0_IMM32).
	 */
	static const struct usc_piece pieces[] = {
		{ 0, 6, 1, 0, 0 },
		{ 0, 5, 1, 1, 0 },
		{ 0, 1, 1, 1, 1 },
		{ 0, 4, 1, USC_FIELD_ONE, 0 },
		{ 0, 2, 2, 2, 0 },
		{ 0, 0, 1, 3, 0 },
		{ 1, 0, 8, 4, 0 },
		{ 2, 0, 8, 4, 8 },
		{ 3, 0, 8, 4, 16 },
		{ 4, 0, 8, 4, 24 }
	};
	static const struct usc_layout short_layout = { 3, 5, 8, { 1, 2, 2, 1, 16 }, pieces };
	static const struct usc_layout wide_layout = { 5, 5, 10, { 1, 2, 2, 1, 32 }, pieces };
	const struct usc_layout *layout;
	uint32_t values[5];
	int error;

	/* Refuses an undefined count kind. */
	if (count_kind > 2)
		return EINVAL;

	error = flag_check(wide);
	if (error != 0)
		return error;

	/* Collects the fields in the layout's order. */
	values[0] = count_source;
	values[1] = count_kind;
	values[2] = shift_kind;
	values[3] = wide;
	values[4] = value;

	/* Picks the 16- or 32-bit immediate form. */
	layout = &short_layout;
	if (wide != 0)
		layout = &wide_layout;

	/* Places the fields after checking each fits. */
	error = operation_build(layout, values, operation);
	if (error != 0)
		return error;

	/* Succeeded: the immediate first phase. */
	return 0;
}

/*
 * Encodes the bitwise unit's second phase, a logical operation.
 */
int
powervr_usc_bit_logic(
	unsigned kind,
	unsigned mask_a,
	unsigned mask_b,
	struct powervr_usc_operation *operation)
{
	/*
	 * Values referenced:
	 * Mesa 26.2.5 src/imagination/pco/pco_isa.py
	 * (I_BITWISE pieces ph0 0:7, ph1 0:6 set, mskb 0:5, ext0 0:4,
	 * mska 0:3, logical_op 0:2..0; F_LOGICAL_OP or 0b000 to byp 0b111
	 * without 0b011; I_PHASE1).
	 */
	static const struct usc_piece pieces[] = {
		{ 0, 6, 1, USC_FIELD_ONE, 0 },
		{ 0, 5, 1, 0, 0 },
		{ 0, 3, 1, 1, 0 },
		{ 0, 0, 3, 2, 0 }
	};
	static const struct usc_layout layout = { 1, 3, 4, { 1, 1, 3 }, pieces };
	uint32_t values[3];
	int error;

	/* Refuses the logical code the hardware does not define. */
	if (kind == 3)
		return EINVAL;

	/* Collects the fields in the layout's order. */
	values[0] = mask_b;
	values[1] = mask_a;
	values[2] = kind;

	/* Places the fields after checking each fits. */
	error = operation_build(&layout, values, operation);
	if (error != 0)
		return error;

	/* Succeeded: the one-byte logical phase. */
	return 0;
}

/*
 * Encodes the bitwise unit's third phase, the second shifter and a test.
 */
int
powervr_usc_bit_shift(
	unsigned kind,
	unsigned test_source,
	unsigned test_nonzero,
	unsigned predicate_write,
	struct powervr_usc_operation *operation)
{
	/*
	 * Values referenced:
	 * Mesa 26.2.5 src/imagination/pco/pco_isa.py
	 * (I_BITWISE pieces ph0 0:7, ph1 0:6, pwen 0:5, tsrc 0:4, top 0:3,
	 * shft2 0:2..0; I_PHASE2).
	 */
	static const struct usc_piece pieces[] = {
		{ 0, 5, 1, 0, 0 },
		{ 0, 4, 1, 1, 0 },
		{ 0, 3, 1, 2, 0 },
		{ 0, 0, 3, 3, 0 }
	};
	static const struct usc_layout layout = { 1, 4, 4, { 1, 1, 1, 3 }, pieces };
	uint32_t values[4];
	int error;

	/* Collects the fields in the layout's order. */
	values[0] = predicate_write;
	values[1] = test_source;
	values[2] = test_nonzero;
	values[3] = kind;

	/* Places the fields after checking each fits. */
	error = operation_build(&layout, values, operation);
	if (error != 0)
		return error;

	/* Succeeded: the one-byte shift phase. */
	return 0;
}

/*
 * Encodes the control unit's branch.
 *
 * The offset is in bytes and must be even; a negative offset branches
 * backwards.  Prediction 0 follows the group's condition, 1 branches when
 * every instance's predicate is set and 2 when any is.
 */
int
powervr_usc_branch(
	int32_t offset,
	unsigned absolute,
	unsigned link,
	unsigned prediction,
	struct powervr_usc_operation *operation)
{
	/*
	 * Values referenced:
	 * Mesa 26.2.5 src/imagination/pco/pco_isa.py
	 * (I_CTRL pieces link 0:4, bpred 0:3..2, abs 0:1,
	 * branch_offset_7_1 1:7..1, branch_offset_15_8 2:7..0,
	 * branch_offset_23_16 3:7..0, branch_offset_31_24 4:7..0, the
	 * reserved bits 0:7..5, 0:0 and 1:0; F_OFFSET31 sending the offset
	 * halved; F_BPRED cc 0b00, allp 0b01, anyp 0b10; I_BRANCH).
	 */
	static const struct usc_piece pieces[] = {
		{ 0, 4, 1, 0, 0 },
		{ 0, 2, 2, 1, 0 },
		{ 0, 1, 1, 2, 0 },
		{ 1, 1, 7, 3, 0 },
		{ 2, 0, 8, 3, 7 },
		{ 3, 0, 8, 3, 15 },
		{ 4, 0, 8, 3, 23 }
	};
	static const struct usc_layout layout = { 5, 4, 7, { 1, 2, 1, 31 }, pieces };
	uint32_t values[4];
	uint32_t raw;
	int error;

	/* Refuses an odd offset and an undefined prediction. */
	raw = (uint32_t)offset;
	if ((raw & 1U) != 0 || prediction > 2)
		return EINVAL;

	/* Collects the fields in the layout's order, the offset halved. */
	values[0] = link;
	values[1] = prediction;
	values[2] = absolute;
	values[3] = raw >> 1;

	/* Places the fields after checking each fits. */
	error = operation_build(&layout, values, operation);
	if (error != 0)
		return error;

	/* Succeeded: the five-byte branch. */
	return 0;
}

/*
 * Encodes the control unit's conditional-execution step.
 *
 * The kind is the hardware's conditional code (start, else, ..., end),
 * the predicate picks which instances it applies to and the adjust is
 * the nesting adjustment.
 */
int
powervr_usc_conditional(
	unsigned kind,
	unsigned predicate,
	unsigned adjust,
	struct powervr_usc_operation *operation)
{
	/*
	 * Values referenced:
	 * Mesa 26.2.5 src/imagination/pco/pco_isa.py
	 * (I_CTRL pieces rsvd0_cnd 0:7, adjust 0:6..5, pcnd 0:4..3,
	 * cndinst 0:2..0; I_CND).
	 */
	static const struct usc_piece pieces[] = {
		{ 0, 5, 2, 0, 0 },
		{ 0, 3, 2, 1, 0 },
		{ 0, 0, 3, 2, 0 }
	};
	static const struct usc_layout layout = { 1, 3, 3, { 2, 2, 3 }, pieces };
	uint32_t values[3];
	int error;

	/* Collects the fields in the layout's order. */
	values[0] = adjust;
	values[1] = predicate;
	values[2] = kind;

	/* Places the fields after checking each fits. */
	error = operation_build(&layout, values, operation);
	if (error != 0)
		return error;

	/* Succeeded: the one-byte conditional step. */
	return 0;
}

/*
 * Encodes the control unit's mutex lock or release.
 */
int
powervr_usc_mutex(
	unsigned identifier,
	unsigned action,
	struct powervr_usc_operation *operation)
{
	/*
	 * Values referenced:
	 * Mesa 26.2.5 src/imagination/pco/pco_isa.py
	 * (I_CTRL pieces lr 0:7..6, rsvd0_mutex 0:5..4, id 0:3..0; F_LR
	 * release 0b00, release_sleep 0b01, release_wakeup 0b10,
	 * lock 0b11; I_MUTEX).
	 */
	static const struct usc_piece pieces[] = {
		{ 0, 6, 2, 0, 0 },
		{ 0, 0, 4, 1, 0 }
	};
	static const struct usc_layout layout = { 1, 2, 2, { 2, 4 }, pieces };
	uint32_t values[2];
	int error;

	/* Collects the fields in the layout's order. */
	values[0] = action;
	values[1] = identifier;

	/* Places the fields after checking each fits. */
	error = operation_build(&layout, values, operation);
	if (error != 0)
		return error;

	/* Succeeded: the one-byte mutex operation. */
	return 0;
}

/*
 * Encodes the control unit's no-operation, a single reserved byte.
 *
 * Values referenced:
 * Mesa 26.2.5 src/imagination/pco/pco_isa.py
 * (I_CTRL piece rsvd0_nop 0:7..0; I_NOP).
 */
int
powervr_usc_nothing(
	struct powervr_usc_operation *operation)
{
	/* Refuses a missing result. */
	if (operation == NULL)
		return EINVAL;

	/* Leaves one zero byte. */
	operation->length = 1;
	operation->bytes[0] = 0;

	/* Succeeded: the one-byte no-operation. */
	return 0;
}

/*
 * Encodes a control operation that has no fields: the waits for output
 * and for data, and the link-register moves.  The header alone names it.
 *
 * Values referenced:
 * Mesa 26.2.5 src/imagination/pco/pco_isa.py
 * (I_WOP, I_WDF, I_LAPC, I_SAVL with no field mappings).
 */
int
powervr_usc_empty(
	struct powervr_usc_operation *operation)
{
	/* Refuses a missing result. */
	if (operation == NULL)
		return EINVAL;

	/* Leaves no bytes. */
	operation->length = 0;

	/* Succeeded: the operation the header names alone. */
	return 0;
}

/*
 * Encodes the control unit's setting of the coefficient or shared base.
 */
int
powervr_usc_shared_base(
	unsigned offset,
	unsigned shared_target,
	struct powervr_usc_operation *operation)
{
	/*
	 * Values referenced:
	 * Mesa 26.2.5 src/imagination/pco/pco_isa.py
	 * (I_CTRL pieces sbo_offset_6_0 0:7..1, tgt 0:0, rsvd1_sbo 1:7..1,
	 * sbo_offset_7 1:0; F_TGT coeff 0b0, shared 0b1; I_SBO).
	 */
	static const struct usc_piece pieces[] = {
		{ 0, 1, 7, 0, 0 },
		{ 1, 0, 1, 0, 7 },
		{ 0, 0, 1, 1, 0 }
	};
	static const struct usc_layout layout = { 2, 2, 3, { 8, 1 }, pieces };
	uint32_t values[2];
	int error;

	/* Collects the fields in the layout's order. */
	values[0] = offset;
	values[1] = shared_target;

	/* Places the fields after checking each fits. */
	error = operation_build(&layout, values, operation);
	if (error != 0)
		return error;

	/* Succeeded: the two-byte base setting. */
	return 0;
}

/* Places a layout's fields into zeroed bytes after checking each value fits. */
static int
layout_place(
	const struct usc_layout *layout,
	const uint32_t *values,
	uint8_t *bytes)
{
	const struct usc_piece *piece;
	uint32_t value;
	unsigned i;

	/* Refuses a value wider than its field. */
	for (i = 0; i < layout->field_count; i++) {
		if (layout->field_bits[i] < 32 &&
		    values[i] >> layout->field_bits[i] != 0)
			return EINVAL;
	}

	/* Clears the form so unplaced bits read as the reserved zero. */
	for (i = 0; i < layout->length; i++)
		bytes[i] = 0;

	/* Places each run of bits in its byte. */
	for (i = 0; i < layout->piece_count; i++) {
		piece = &layout->pieces[i];

		/* The fixed-one field sets the bit that marks the form. */
		value = 1;
		if (piece->field != USC_FIELD_ONE)
			value = values[piece->field] >> piece->shift;

		value &= (1U << piece->width) - 1U;
		bytes[piece->byte] |= (uint8_t)(value << piece->low);
	}

	/* Succeeded: every field is in place. */
	return 0;
}

/* Reports how many bits a value needs, zero for zero. */
static unsigned
bits_needed(
	uint32_t value)
{
	unsigned bits;

	/* Counts up to the highest set bit. */
	bits = 0;
	while (value != 0) {
		bits++;
		value >>= 1;
	}

	/* The width of the value. */
	return bits;
}

/* Reports the bank and index widths a register needs, or -1 for an unused one. */
static int
register_bits(
	const struct powervr_usc_register *reg,
	int *bank_bits,
	int *index_bits)
{
	/* An unused operand needs nothing. */
	if (reg->used == 0) {
		*bank_bits = OPERAND_ABSENT;
		*index_bits = OPERAND_ABSENT;
		return 0;
	}

	/* Refuses a bank or index beyond what any form can name. */
	if (reg->bank > POWERVR_USC_BANK_INDEXED_1 || reg->index >= 2048)
		return EINVAL;

	/*
	 * An indexed bank's index field packs the index bank and an offset,
	 * so it always needs the widest form.
	 *
	 * Procedure referenced:
	 * Mesa 26.2.5 src/imagination/pco/pco_map.h.py
	 * (pco_map_reg_bank_bits, pco_map_reg_index_bits).
	 */
	*bank_bits = (int)bits_needed(reg->bank);
	*index_bits = (int)bits_needed(reg->index);
	if (reg->bank >= POWERVR_USC_BANK_INDEXED_0)
		*index_bits = 11;

	/* Succeeded: the widths the operand needs. */
	return 0;
}

/*
 * Chooses the first form that can name every operand.  An operand the
 * form must not carry fits only when it is unused.
 */
static int
operand_choose(
	const struct usc_operand_form *forms,
	size_t form_count,
	const struct powervr_usc_register *operands,
	unsigned operand_count,
	int mux,
	const struct usc_operand_form **chosen)
{
	int needed[OPERAND_FIELD_COUNT];
	size_t i;
	unsigned field;
	int fits;
	int error;

	/* Measures each operand the word carries; the rest are absent. */
	for (field = 0; field < 3; field++) {
		needed[2 * field] = OPERAND_ABSENT;
		needed[2 * field + 1] = OPERAND_ABSENT;
		if (field < operand_count) {
			error = register_bits(&operands[field], &needed[2 * field], &needed[2 * field + 1]);
			if (error != 0)
				return error;
		}
	}

	/* Measures the selector, which is absent when negative. */
	needed[OPERAND_MUX] = OPERAND_ABSENT;
	if (mux >= 0)
		needed[OPERAND_MUX] = (int)bits_needed((uint32_t)mux);

	/* Takes the first form whose every limit covers what is needed. */
	for (i = 0; i < form_count; i++) {
		fits = 1;
		for (field = 0; field < OPERAND_FIELD_COUNT; field++) {
			if (forms[i].limit[field] == OPERAND_ABSENT) {
				/* The form has no room: only an absent operand fits. */
				if (needed[field] != OPERAND_ABSENT)
					fits = 0;
			} else if (needed[field] > forms[i].limit[field]) {
				/* The operand is wider than the form's field. */
				fits = 0;
			}
		}

		if (fits) {
			*chosen = &forms[i];
			return 0;
		}
	}

	/* No form names these operands. */
	return EINVAL;
}

/* Collects the operand fields of up to three registers and a selector. */
static void
operand_values(
	const struct powervr_usc_register *operands,
	unsigned operand_count,
	int mux,
	uint32_t *values)
{
	unsigned i;

	/* Takes each used register's bank and index; unused ones are zero. */
	for (i = 0; i < 3; i++) {
		values[2 * i] = 0;
		values[2 * i + 1] = 0;
		if (i < operand_count && operands[i].used != 0) {
			values[2 * i] = operands[i].bank;
			values[2 * i + 1] = operands[i].index;
		}
	}

	/* Takes the selector when one is used. */
	values[OPERAND_MUX] = 0;
	if (mux >= 0)
		values[OPERAND_MUX] = (uint32_t)mux;
}

/*
 * Checks the fields of a group that no form choice can repair.
 *
 * Values referenced:
 * Mesa 26.2.5 src/imagination/pco/pco_isa.py
 * (F_IS0_SEL s0 0b000 to s2 0b101; F_IS1_SEL and F_IS2_SEL one bit;
 * F_IS3_SEL ft0 0b00, ft1 0b01, fte 0b11; F_IS4_SEL and F_IS5_SEL two
 * bits; F_UINT2_POS_INC for the repeat count 1 to 4; F_CTRLOP codes 0 to
 * 8, 11 and 12).
 */
static int
group_check(
	const struct powervr_usc_group *group)
{
	unsigned phase;

	/* Refuses an undefined unit or condition. */
	if (group->unit != POWERVR_USC_UNIT_MAIN &&
	    group->unit != POWERVR_USC_UNIT_BITWISE &&
	    group->unit != POWERVR_USC_UNIT_CONTROL)
		return EINVAL;
	if (group->condition > 3)
		return EINVAL;

	/* Refuses header flags that are not single bits. */
	if (group->end > 1 ||
	    group->atomic > 1 ||
	    group->overlap_check > 1 ||
	    group->write_0 > 1 ||
	    group->write_1 > 1 ||
	    group->control_misc > 1)
		return EINVAL;

	/* Refuses a repeat count outside one to four. */
	if (group->repeat < 1 || group->repeat > 4)
		return EINVAL;

	/* Refuses operations longer than any the hardware defines. */
	for (phase = 0; phase < POWERVR_USC_PHASE_COUNT; phase++) {
		if (group->operation[phase].length > POWERVR_USC_OPERATION_MAX)
			return EINVAL;
	}

	/* Refuses a source selector beyond the six sources. */
	if (group->mux_used > 1 || (group->mux_used != 0 && group->mux > 5))
		return EINVAL;

	/* A group without internal selectors names none and no source selector. */
	if (group->internal_absent > 1)
		return EINVAL;
	if (group->internal_absent != 0) {
		if (group->unit != POWERVR_USC_UNIT_MAIN || group->mux_used != 0)
			return EINVAL;
		for (phase = 0; phase < 6; phase++) {
			if (group->internal[phase] != 0)
				return EINVAL;
		}
	}

	/* Refuses internal selectors outside their codes. */
	if (group->internal[0] != 0 ||
	    group->internal[1] > 1 ||
	    group->internal[2] > 1 ||
	    group->internal[3] == 2 ||
	    group->internal[3] > 3 ||
	    group->internal[4] > 3 ||
	    group->internal[5] > 3)
		return EINVAL;

	/* A control group has one defined operation, no repeat and no internal routing. */
	if (group->unit == POWERVR_USC_UNIT_CONTROL) {
		if (group->control == 9 ||
		    group->control == 10 ||
		    group->control > 12 ||
		    group->end != 0 ||
		    group->atomic != 0 ||
		    group->repeat != 1)
			return EINVAL;

		for (phase = 1; phase < POWERVR_USC_PHASE_COUNT; phase++) {
			if (group->operation[phase].length != 0)
				return EINVAL;
		}
	} else if (group->control != 0 || group->control_misc != 0) {
		/* Only a control group names a control operation. */
		return EINVAL;
	}

	/* Only a main group routes through the internal selectors. */
	if (group->unit != POWERVR_USC_UNIT_MAIN) {
		for (phase = 0; phase < 6; phase++) {
			if (group->internal[phase] != 0)
				return EINVAL;
		}
	}

	/* Succeeded: the group's fixed fields are well formed. */
	return 0;
}

/*
 * Chooses a group's operand forms and works out its lengths.
 *
 * Procedure referenced:
 * Mesa 26.2.5 src/imagination/pco/pco_group_instrs.c (calc_lengths,
 * calc_da) and src/imagination/pco/pco_map.h.py (pco_igrp_hdr_variant
 * with src/imagination/pco/pco_ops.py OM_RPT nzdefault 1,
 * pco_igrp_src_variant, pco_igrp_iss_variant, pco_igrp_dest_variant).
 */
static int
group_plan(
	const struct powervr_usc_group *group,
	struct usc_group_plan *plan)
{
	const struct powervr_usc_register *lower;
	const struct powervr_usc_register *upper;
	unsigned used;
	unsigned phase;
	unsigned operations;
	unsigned lower_used;
	unsigned upper_used;
	int mux;
	int error;

	/* Refuses fields no form can carry. */
	error = group_check(group);
	if (error != 0)
		return error;

	/* Notes which halves of the operand set are in use. */
	lower = &group->source[0];
	upper = &group->source[3];
	mux = -1;
	if (group->mux_used != 0)
		mux = group->mux;

	lower_used = 0;
	if (lower[0].used || lower[1].used || lower[2].used || mux >= 0)
		lower_used = 1;

	upper_used = 0;
	if (upper[0].used || upper[1].used || upper[2].used)
		upper_used = 1;

	/*
	 * Every group takes the three-byte header.  The ISA also has a
	 * two-byte main header without the end, atomic and repeat fields,
	 * but Mesa never emits it (its repeat count is never zero), so its
	 * meaning on the hardware is unproven and the encoder leaves it out.
	 */
	plan->header_length = 3;

	/*
	 * Chooses the operand forms.  A main or bitwise group always carries
	 * both halves, even unused; a control group leaves an unused half out.
	 */
	plan->lower = NULL;
	plan->upper = NULL;
	if (group->unit != POWERVR_USC_UNIT_CONTROL || lower_used) {
		error = operand_choose(lower_forms, sizeof(lower_forms) / sizeof(lower_forms[0]), lower, 3, mux, &plan->lower);
		if (error != 0)
			return error;
	}

	if (group->unit != POWERVR_USC_UNIT_CONTROL || upper_used) {
		error = operand_choose(upper_forms, sizeof(upper_forms) / sizeof(upper_forms[0]), upper, 3, -1, &plan->upper);
		if (error != 0)
			return error;
	}

	/* Chooses the destination form for one result or two. */
	plan->destination = NULL;
	if (group->destination[0].used && group->destination[1].used) {
		error = operand_choose(dual_destination_forms, sizeof(dual_destination_forms) / sizeof(dual_destination_forms[0]), group->destination, 2, -1, &plan->destination);
		if (error != 0)
			return error;
	} else if (group->destination[0].used) {
		error = operand_choose(single_destination_forms, sizeof(single_destination_forms) / sizeof(single_destination_forms[0]), &group->destination[0], 1, -1, &plan->destination);
		if (error != 0)
			return error;
	} else if (group->destination[1].used) {
		error = operand_choose(single_destination_forms, sizeof(single_destination_forms) / sizeof(single_destination_forms[0]), &group->destination[1], 1, -1, &plan->destination);
		if (error != 0)
			return error;
	}

	/* Only a main group carries the internal selector byte, and not the sum of products. */
	plan->internal_length = 0;
	if (group->unit == POWERVR_USC_UNIT_MAIN && group->internal_absent == 0)
		plan->internal_length = 1;

	/* Works out the phase organisation from the slots in use. */
	used = 0;
	operations = 0;
	for (phase = 0; phase < POWERVR_USC_PHASE_COUNT; phase++) {
		operations += group->operation[phase].length;
		if (group->operation[phase].length != 0)
			used |= 1U << phase;
	}

	/* Main groups name an organisation code, bitwise ones a phase set. */
	if (group->unit == POWERVR_USC_UNIT_MAIN) {
		if (main_organisation[used] < 0)
			return EINVAL;

		plan->order = (unsigned)main_organisation[used];
	} else if (group->unit == POWERVR_USC_UNIT_BITWISE) {
		/* A bitwise group has no back end and needs a phase. */
		if (used == 0 || (used & (1U << POWERVR_USC_PHASE_BACKEND)) != 0)
			return EINVAL;

		plan->order = used;
	} else {
		/* A control group's header names the operation itself. */
		plan->order = 0;
	}

	/* Adds up the group and pads it to a whole half-word. */
	plan->length = plan->header_length + operations + plan->internal_length;
	if (plan->lower != NULL)
		plan->length += plan->lower->layout.length;
	if (plan->upper != NULL)
		plan->length += plan->upper->layout.length;
	if (plan->destination != NULL)
		plan->length += plan->destination->layout.length;

	plan->word_padding = plan->length % 2;
	plan->length += plan->word_padding;

	/*
	 * The decode assist tells the fetch where the operand words start:
	 * past the header and the operations the hardware decodes late.
	 */
	plan->decode_assist = plan->header_length;
	if (group->unit == POWERVR_USC_UNIT_MAIN) {
		plan->decode_assist += group->operation[POWERVR_USC_PHASE_2].length;
		plan->decode_assist += group->operation[POWERVR_USC_PHASE_BACKEND].length;
	} else if (group->unit == POWERVR_USC_UNIT_BITWISE) {
		plan->decode_assist += operations;
	} else if (plan->lower == NULL && plan->upper == NULL && plan->destination == NULL) {
		/* A control group with no operands needs no assist. */
		plan->decode_assist = 0;
	} else {
		plan->decode_assist += operations;
	}

	/* Refuses a group the four-bit fields cannot describe. */
	if (plan->decode_assist > 15 || plan->length > POWERVR_USC_GROUP_MAX)
		return EINVAL;

	/* Succeeded: the group's forms and lengths. */
	return 0;
}

/* Writes the header of a group whose padded size is halfwords half-words. */
static void
header_write(
	const struct powervr_usc_group *group,
	const struct usc_group_plan *plan,
	unsigned halfwords,
	uint8_t *output)
{
	/*
	 * Values referenced:
	 * Mesa 26.2.5 src/imagination/pco/pco_isa.py
	 * (I_IGRP_HDR pieces da 0:7..4, length 0:3..0, ext 1:7,
	 * oporg and opcnt 1:6..4, olchk 1:3, w1p 1:2, w0p 1:1, cc 1:0,
	 * end and miscctl 2:7, alutype 2:6..5, rsvd 2:4, ctrlop 2:4..1,
	 * atom 2:3, rpt 2:2..1, ccext 2:0; F_UINT4_POS_WRAP sending a length
	 * of 16 as 0; I_IGRP_HDR_MAIN,
	 * I_IGRP_HDR_BITWISE, I_IGRP_HDR_CONTROL).
	 */

	/* The decode assist and the length in half-words, sixteen as zero. */
	output[0] = (uint8_t)((plan->decode_assist << 4) | (halfwords % 16));

	/* The organisation, the overlap check, the write enables and the low condition bit. */
	output[1] = (uint8_t)((plan->order << 4) |
	    ((unsigned)group->overlap_check << 3) |
	    ((unsigned)group->write_1 << 2) |
	    ((unsigned)group->write_0 << 1) |
	    (group->condition & 1U));

	/* The three-byte headers set the extension bit. */
	output[1] |= 0x80;
	if (group->unit == POWERVR_USC_UNIT_CONTROL) {
		output[2] = (uint8_t)(((unsigned)group->control_misc << 7) |
		    ((unsigned)group->unit << 5) |
		    ((unsigned)group->control << 1) |
		    ((unsigned)group->condition >> 1));
	} else {
		output[2] = (uint8_t)(((unsigned)group->end << 7) |
		    ((unsigned)group->unit << 5) |
		    ((unsigned)group->atomic << 3) |
		    (((unsigned)group->repeat - 1U) << 1) |
		    ((unsigned)group->condition >> 1));
	}
}

/*
 * Writes one planned group and the alignment padding it carries.
 *
 * Procedure referenced:
 * Mesa 26.2.5 src/imagination/pco/pco_binary.c (pco_encode_igrp,
 * pco_encode_align: the order header, operations from the last phase
 * to the first, lower and upper sources, internal selectors,
 * destinations, then the word padding byte 0xff and the alignment
 * padding 0xf0 with its half-word count followed by 0xff bytes).
 */
static int
group_write(
	const struct powervr_usc_group *group,
	const struct usc_group_plan *plan,
	unsigned alignment,
	uint8_t *output)
{
	uint32_t values[OPERAND_FIELD_COUNT];
	size_t offset;
	unsigned phase;
	unsigned i;
	int mux;
	int error;

	/* Refuses padding that would overflow the header's length. */
	if (plan->length + alignment > POWERVR_USC_GROUP_MAX)
		return EINVAL;

	/* Writes the header with the padded length. */
	header_write(group, plan, (plan->length + alignment) / 2, output);
	offset = plan->header_length;

	/* Writes the operations, the latest phase first. */
	for (phase = POWERVR_USC_PHASE_COUNT; phase > 0; phase--) {
		for (i = 0; i < group->operation[phase - 1].length; i++)
			output[offset + i] = group->operation[phase - 1].bytes[i];

		offset += group->operation[phase - 1].length;
	}

	/* Writes the lower sources with the selector. */
	mux = -1;
	if (group->mux_used != 0)
		mux = group->mux;

	if (plan->lower != NULL) {
		operand_values(&group->source[0], 3, mux, values);
		error = layout_place(&plan->lower->layout, values, output + offset);
		if (error != 0)
			return error;

		offset += plan->lower->layout.length;
	}

	/* Writes the upper sources. */
	if (plan->upper != NULL) {
		operand_values(&group->source[3], 3, -1, values);
		error = layout_place(&plan->upper->layout, values, output + offset);
		if (error != 0)
			return error;

		offset += plan->upper->layout.length;
	}

	/*
	 * Writes the internal selectors of a main group.
	 *
	 * Values referenced:
	 * Mesa 26.2.5 src/imagination/pco/pco_isa.py
	 * (I__ISS pieces is5 0:7..6, is4 0:5..4, is3 0:3..2, is2 0:1,
	 * is1 0:0).
	 */
	if (plan->internal_length != 0) {
		output[offset] = (uint8_t)(((unsigned)group->internal[5] << 6) |
		    ((unsigned)group->internal[4] << 4) |
		    ((unsigned)group->internal[3] << 2) |
		    ((unsigned)group->internal[2] << 1) |
		    (unsigned)group->internal[1]);
		offset++;
	}

	/* Writes the destinations; a lone one takes the first fields. */
	if (plan->destination != NULL) {
		if (group->destination[0].used && group->destination[1].used)
			operand_values(group->destination, 2, -1, values);
		else if (group->destination[0].used)
			operand_values(&group->destination[0], 1, -1, values);
		else
			operand_values(&group->destination[1], 1, -1, values);

		error = layout_place(&plan->destination->layout, values, output + offset);
		if (error != 0)
			return error;

		offset += plan->destination->layout.length;
	}

	/* Pads to a whole half-word. */
	if (plan->word_padding != 0) {
		output[offset] = 0xff;
		offset++;
	}

	/* Pads for the cache: a marker with the half-word count, then filler. */
	if (alignment != 0) {
		output[offset] = (uint8_t)(0xf0 | (alignment / 2));
		offset++;
		for (i = 1; i < alignment; i++) {
			output[offset] = 0xff;
			offset++;
		}
	}

	/* Succeeded: the group and its padding are written. */
	return 0;
}

/* Builds an operation from its layout and field values. */
static int
operation_build(
	const struct usc_layout *layout,
	const uint32_t *values,
	struct powervr_usc_operation *operation)
{
	int error;

	/* Refuses a missing result. */
	if (operation == NULL)
		return EINVAL;

	/* Places the fields, refusing any that does not fit. */
	error = layout_place(layout, values, operation->bytes);
	if (error != 0)
		return error;

	/* Succeeded: the operation's length is the layout's. */
	operation->length = layout->length;
	return 0;
}

/* Refuses a flag that is not zero or one. */
static int
flag_check(
	unsigned flag)
{
	/* A flag is one bit. */
	if (flag > 1)
		return EINVAL;

	/* Succeeded: the flag is a bit. */
	return 0;
}

/* Hands over one group of an array for powervr_usc_program_encode(). */
static int
array_fetch(
	void *context,
	size_t index,
	struct powervr_usc_group *group)
{
	const struct powervr_usc_group *groups;

	/* Copies the indexed group. */
	groups = context;
	*group = groups[index];

	/* Succeeded: the group is handed over. */
	return 0;
}
