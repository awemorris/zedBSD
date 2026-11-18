/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The CCITTFaxDecode filter of libpdf's reader (stage 3 of design-pdf.md):
 * the bilevel images of scanned pages and of some bitmap fonts, coded as in
 * ITU-T T.4 (Group 3: one-dimensional, or mixed with two-dimensional rows)
 * and T.6 (Group 4: every row two-dimensional).
 *
 * A row is decoded into its changing elements: the places where the colour
 * changes, starting from white at the left edge.  A one-dimensional row is
 * a sequence of run lengths, white and black in turn, each a Huffman code
 * of T.4.  A two-dimensional row is coded against the row above (the
 * reference row) with the pass, horizontal and vertical modes.  The row
 * then becomes one packed row of the output, 0 for black unless
 * /BlackIs1, and the reference for the next row.
 *
 * Between rows the decoder follows what readers of PDF accept: fill bits
 * and an end-of-line code, byte alignment, the one- or two-dimensional tag
 * of mixed data, and the end-of-block code.  A damaged row ends the decode,
 * keeping the rows before it, unless the data has end-of-line codes and
 * /DamagedRowsBeforeError allows more damage, in which case the decoder
 * finds the next end-of-line and goes on.  Rows /Rows asks for and the data
 * does not hold are white.
 */

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include <pdf.h>

#include "internal.h"

/* The longest run-length code, and so the width of the run tables' index, in bits. */
#define CCITT_RUN_BITS 13
#define CCITT_RUN_TABLE (1UL << CCITT_RUN_BITS)

/* The longest mode code (the vertical modes of three), and so the width of the mode table's index. */
#define CCITT_MODE_BITS 7
#define CCITT_MODE_TABLE (1UL << CCITT_MODE_BITS)

/* The end-of-line code, eleven zeros and a one, and its length. */
#define CCITT_EOL 0x001UL
#define CCITT_EOL_BITS 12

/* Two end-of-line codes in a row, which end a block of byte-aligned rows. */
#define CCITT_EOL_TWICE 0x001001UL
#define CCITT_EOL_TWICE_BITS 24

/* The widest row the decoder takes, the widest image side. */
#define CCITT_COLUMNS_MAX PDF_IMAGE_SIDE_MAX

/* The room the changes of one row take beyond one change a column. */
#define CCITT_CHANGES_SLACK 8

/* The first size of an output whose rows are not known. */
#define CCITT_OUTPUT_INITIAL 65536

/* The colours of a run and of the element a decode stands on. */
#define CCITT_WHITE 0
#define CCITT_BLACK 1

/* The smallest run that is a make-up code: shorter runs are terminating codes, which end a run. */
#define CCITT_MAKEUP_MIN 64

/*
 * The modes of a two-dimensional row's codes.
 */
enum ccitt_mode {
	CCITT_MODE_NONE = 0,
	CCITT_MODE_PASS,
	CCITT_MODE_HORIZONTAL,
	CCITT_MODE_VERTICAL
};

/*
 * One code of the T.4 tables as the recommendation prints it: its bits as
 * the characters 0 and 1, and the run length (or the vertical offset) it
 * stands for.  The tables below are read once per decode into the lookup
 * tables.
 */
struct ccitt_code {
	const char *bits;
	int value;
};

/*
 * One entry of a run lookup table: the run length a code starting with the
 * entry's index stands for, and the code's length in bits (0: no code
 * starts so).
 */
struct ccitt_run_entry {
	unsigned short run;
	unsigned char length;
};

/*
 * One entry of the mode lookup table: the mode a code starting with the
 * entry's index stands for, the vertical offset of a vertical mode, and the
 * code's length in bits (0: no mode code starts so).
 */
struct ccitt_mode_entry {
	unsigned char mode;
	signed char offset;
	unsigned char length;
};

/*
 * The state of one decode: the parameters, the coded bits and how many are
 * read, the lookup tables, the changes of the reference row and of the row
 * being decoded, and the output rows.
 *
 * reference holds the reference row's changes in order, without an empty
 * run, then the row's width three times, so that a search for b1 and b2
 * always ends inside it.  coding holds the changes of the row being
 * decoded; each is at least the one before it.  end_of_line is whether the
 * data has end-of-line codes (the parameter, or the first code seen), and
 * two_dimensional whether the next row of mixed data is coded
 * two-dimensionally.  The output grows by whole rows, the rows decoded
 * counted in rows_done; damaged counts the damaged rows passed over.
 */
struct ccitt_decoder {
	const struct pdf_ccitt_parameters *parameters;
	const unsigned char *data;
	size_t size;
	size_t position;
	size_t total;
	struct ccitt_run_entry white[CCITT_RUN_TABLE];
	struct ccitt_run_entry black[CCITT_RUN_TABLE];
	struct ccitt_mode_entry modes[CCITT_MODE_TABLE];
	long *reference;
	size_t reference_count;
	long *coding;
	size_t coding_count;
	size_t changes_capacity;
	int end_of_line;
	int two_dimensional;
	int finished;
	unsigned char *output;
	size_t output_capacity;
	size_t stride;
	size_t rows_done;
	long damaged;
};

/* The white runs' terminating codes (T.4 table 2). */
static const struct ccitt_code ccitt_white_codes[] = {
	{ "00110101", 0 }, { "000111", 1 }, { "0111", 2 }, { "1000", 3 },
	{ "1011", 4 }, { "1100", 5 }, { "1110", 6 }, { "1111", 7 },
	{ "10011", 8 }, { "10100", 9 }, { "00111", 10 }, { "01000", 11 },
	{ "001000", 12 }, { "000011", 13 }, { "110100", 14 }, { "110101", 15 },
	{ "101010", 16 }, { "101011", 17 }, { "0100111", 18 }, { "0001100", 19 },
	{ "0001000", 20 }, { "0010111", 21 }, { "0000011", 22 }, { "0000100", 23 },
	{ "0101000", 24 }, { "0101011", 25 }, { "0010011", 26 }, { "0100100", 27 },
	{ "0011000", 28 }, { "00000010", 29 }, { "00000011", 30 }, { "00011010", 31 },
	{ "00011011", 32 }, { "00010010", 33 }, { "00010011", 34 }, { "00010100", 35 },
	{ "00010101", 36 }, { "00010110", 37 }, { "00010111", 38 }, { "00101000", 39 },
	{ "00101001", 40 }, { "00101010", 41 }, { "00101011", 42 }, { "00101100", 43 },
	{ "00101101", 44 }, { "00000100", 45 }, { "00000101", 46 }, { "00001010", 47 },
	{ "00001011", 48 }, { "01010010", 49 }, { "01010011", 50 }, { "01010100", 51 },
	{ "01010101", 52 }, { "00100100", 53 }, { "00100101", 54 }, { "01011000", 55 },
	{ "01011001", 56 }, { "01011010", 57 }, { "01011011", 58 }, { "01001010", 59 },
	{ "01001011", 60 }, { "00110010", 61 }, { "00110011", 62 }, { "00110100", 63 },
	{ "11011", 64 }, { "10010", 128 }, { "010111", 192 }, { "0110111", 256 },
	{ "00110110", 320 }, { "00110111", 384 }, { "01100100", 448 }, { "01100101", 512 },
	{ "01101000", 576 }, { "01100111", 640 }, { "011001100", 704 }, { "011001101", 768 },
	{ "011010010", 832 }, { "011010011", 896 }, { "011010100", 960 }, { "011010101", 1024 },
	{ "011010110", 1088 }, { "011010111", 1152 }, { "011011000", 1216 }, { "011011001", 1280 },
	{ "011011010", 1344 }, { "011011011", 1408 }, { "010011000", 1472 }, { "010011001", 1536 },
	{ "010011010", 1600 }, { "011000", 1664 }, { "010011011", 1728 }
};

/* The black runs' terminating and make-up codes (T.4 tables 2 and 3). */
static const struct ccitt_code ccitt_black_codes[] = {
	{ "0000110111", 0 }, { "010", 1 }, { "11", 2 }, { "10", 3 },
	{ "011", 4 }, { "0011", 5 }, { "0010", 6 }, { "00011", 7 },
	{ "000101", 8 }, { "000100", 9 }, { "0000100", 10 }, { "0000101", 11 },
	{ "0000111", 12 }, { "00000100", 13 }, { "00000111", 14 }, { "000011000", 15 },
	{ "0000010111", 16 }, { "0000011000", 17 }, { "0000001000", 18 }, { "00001100111", 19 },
	{ "00001101000", 20 }, { "00001101100", 21 }, { "00000110111", 22 }, { "00000101000", 23 },
	{ "00000010111", 24 }, { "00000011000", 25 }, { "000011001010", 26 }, { "000011001011", 27 },
	{ "000011001100", 28 }, { "000011001101", 29 }, { "000001101000", 30 }, { "000001101001", 31 },
	{ "000001101010", 32 }, { "000001101011", 33 }, { "000011010010", 34 }, { "000011010011", 35 },
	{ "000011010100", 36 }, { "000011010101", 37 }, { "000011010110", 38 }, { "000011010111", 39 },
	{ "000001101100", 40 }, { "000001101101", 41 }, { "000011011010", 42 }, { "000011011011", 43 },
	{ "000001010100", 44 }, { "000001010101", 45 }, { "000001010110", 46 }, { "000001010111", 47 },
	{ "000001100100", 48 }, { "000001100101", 49 }, { "000001010010", 50 }, { "000001010011", 51 },
	{ "000000100100", 52 }, { "000000110111", 53 }, { "000000111000", 54 }, { "000000100111", 55 },
	{ "000000101000", 56 }, { "000001011000", 57 }, { "000001011001", 58 }, { "000000101011", 59 },
	{ "000000101100", 60 }, { "000001011010", 61 }, { "000001100110", 62 }, { "000001100111", 63 },
	{ "0000001111", 64 }, { "000011001000", 128 }, { "000011001001", 192 }, { "000001011011", 256 },
	{ "000000110011", 320 }, { "000000110100", 384 }, { "000000110101", 448 }, { "0000001101100", 512 },
	{ "0000001101101", 576 }, { "0000001001010", 640 }, { "0000001001011", 704 }, { "0000001001100", 768 },
	{ "0000001001101", 832 }, { "0000001110010", 896 }, { "0000001110011", 960 }, { "0000001110100", 1024 },
	{ "0000001110101", 1088 }, { "0000001110110", 1152 }, { "0000001110111", 1216 }, { "0000001010010", 1280 },
	{ "0000001010011", 1344 }, { "0000001010100", 1408 }, { "0000001010101", 1472 }, { "0000001011010", 1536 },
	{ "0000001011011", 1600 }, { "0000001100100", 1664 }, { "0000001100101", 1728 }
};

/* The make-up codes of long runs of either colour (T.4 table 3, the extended codes). */
static const struct ccitt_code ccitt_long_codes[] = {
	{ "00000001000", 1792 }, { "00000001100", 1856 }, { "00000001101", 1920 }, { "000000010010", 1984 },
	{ "000000010011", 2048 }, { "000000010100", 2112 }, { "000000010101", 2176 }, { "000000010110", 2240 },
	{ "000000010111", 2304 }, { "000000011100", 2368 }, { "000000011101", 2432 }, { "000000011110", 2496 },
	{ "000000011111", 2560 }
};

/* The vertical modes' codes and their offsets (T.4 table 4). */
static const struct ccitt_code ccitt_vertical_codes[] = {
	{ "1", 0 }, { "011", 1 }, { "000011", 2 }, { "0000011", 3 },
	{ "010", -1 }, { "000010", -2 }, { "0000010", -3 }
};

/* The pass and horizontal modes' codes (T.4 table 4). */
#define CCITT_PASS_CODE "0001"
#define CCITT_HORIZONTAL_CODE "001"

static void build_run_table(struct ccitt_run_entry *table, const struct ccitt_code *codes, size_t count);
static void build_mode_table(struct ccitt_mode_entry *table);
static void put_mode(struct ccitt_mode_entry *table, const char *bits, enum ccitt_mode mode, int offset);
static unsigned long code_value(const char *bits, size_t *length);
static unsigned long peek_bits(const struct ccitt_decoder *decoder, unsigned count);
static void eat_bits(struct ccitt_decoder *decoder, size_t count);
static int decode_rows(struct ccitt_decoder *decoder);
static void start_data(struct ccitt_decoder *decoder);
static int decode_one_dimensional(struct ccitt_decoder *decoder);
static int decode_two_dimensional(struct ccitt_decoder *decoder);
static int read_run(struct ccitt_decoder *decoder, int color, long *run);
static void find_b1_b2(const struct ccitt_decoder *decoder, long a0, int color, size_t *search, long *b1, long *b2);
static int add_change(struct ccitt_decoder *decoder, long position);
static int put_row(struct ccitt_decoder *decoder);
static void paint_span(unsigned char *row, long from, long to, int black_is_1);
static void make_reference(struct ccitt_decoder *decoder);
static void between_rows(struct ccitt_decoder *decoder);
static int skip_to_end_of_line(struct ccitt_decoder *decoder);
static int reserve_rows(struct ccitt_decoder *decoder, size_t rows);

/*
 * Decodes CCITT fax data (Group 3 or Group 4) into packed rows of one bit
 * a pixel, each row starting on a byte.
 *
 * Returns 0 with the rows in a new buffer the caller frees, PDF_EFORMAT
 * for parameters no image can be made of or data that holds no row,
 * ENOMEM when memory or the decode limit runs out.
 */
int
pdf_ccitt_decode(
	const struct pdf_ccitt_parameters *parameters,
	const unsigned char *input,
	size_t input_size,
	unsigned char **output,
	size_t *output_size)
{
	struct ccitt_decoder *decoder;
	size_t rows;
	size_t used;
	int error;

	/* Nothing is decoded yet. */
	*output = NULL;
	*output_size = 0;

	/* Refuses a width no row can have, or a negative height. */
	if (parameters->columns < 1 || parameters->columns > CCITT_COLUMNS_MAX)
		return PDF_EFORMAT;
	if (parameters->rows < 0)
		return PDF_EFORMAT;

	/* Refuses data whose bits cannot be counted in a size. */
	if (input_size > ((size_t)-1) / 8)
		return ENOMEM;

	/* Allocates the decoder, whose lookup tables are too large for the stack. */
	decoder = calloc(1, sizeof(*decoder));
	if (decoder == NULL)
		return ENOMEM;

	/* The parameters, the coded bits and the size of a row. */
	decoder->parameters = parameters;
	decoder->data = input;
	decoder->size = input_size;
	decoder->position = 0;
	decoder->total = input_size * 8;
	decoder->stride = ((size_t)parameters->columns + 7) / 8;
	decoder->end_of_line = parameters->end_of_line;

	/* The changes of the reference row and of the row decoded, one a column and some to spare. */
	decoder->changes_capacity = (size_t)parameters->columns + CCITT_CHANGES_SLACK;
	decoder->reference = malloc(decoder->changes_capacity * sizeof(decoder->reference[0]));
	if (decoder->reference == NULL) {
		free(decoder);
		return ENOMEM;
	}

	/* The changes of the row being decoded. */
	decoder->coding = malloc(decoder->changes_capacity * sizeof(decoder->coding[0]));
	if (decoder->coding == NULL) {
		free(decoder->reference);
		free(decoder);
		return ENOMEM;
	}

	/* The lookup tables of the runs of each colour (the long make-up codes serve both) and of the modes. */
	build_run_table(decoder->white, ccitt_white_codes, sizeof(ccitt_white_codes) / sizeof(ccitt_white_codes[0]));
	build_run_table(decoder->white, ccitt_long_codes, sizeof(ccitt_long_codes) / sizeof(ccitt_long_codes[0]));
	build_run_table(decoder->black, ccitt_black_codes, sizeof(ccitt_black_codes) / sizeof(ccitt_black_codes[0]));
	build_run_table(decoder->black, ccitt_long_codes, sizeof(ccitt_long_codes) / sizeof(ccitt_long_codes[0]));
	build_mode_table(decoder->modes);

	/* The first reference row is all white: only the row's end. */
	decoder->reference[0] = parameters->columns;
	decoder->reference[1] = parameters->columns;
	decoder->reference[2] = parameters->columns;
	decoder->reference_count = 3;

	/* Decodes the rows. */
	error = decode_rows(decoder);

	/* A height asked for is filled with white rows past the ones decoded. */
	rows = decoder->rows_done;
	if (error == 0 && (size_t)parameters->rows > rows) {
		rows = (size_t)parameters->rows;
		error = reserve_rows(decoder, rows);
	}

	/* The white of the rows past the decoded ones (all ones unless black is one). */
	if (error == 0 && rows > decoder->rows_done) {
		used = decoder->rows_done * decoder->stride;
		if (parameters->black_is_1) {
			memset(decoder->output + used, 0x00, (rows - decoder->rows_done) * decoder->stride);
		} else {
			memset(decoder->output + used, 0xff, (rows - decoder->rows_done) * decoder->stride);
		}
	}

	/* Data that gives no row at all is not an image. */
	if (error == 0 && rows == 0)
		error = PDF_EFORMAT;

	/* The changes are not needed any more. */
	free(decoder->reference);
	free(decoder->coding);

	/* Reports a decode that failed, freeing its rows. */
	if (error != 0) {
		free(decoder->output);
		free(decoder);
		return error;
	}

	/* Succeeded: the caller owns the rows. */
	*output = decoder->output;
	*output_size = rows * decoder->stride;
	free(decoder);
	return 0;
}

/*
 * Enters the codes of a table into a lookup table indexed by the next
 * CCITT_RUN_BITS bits: every index that starts with a code's bits gives
 * that code.
 */
static void
build_run_table(
	struct ccitt_run_entry *table,
	const struct ccitt_code *codes,
	size_t count)
{
	unsigned long value;
	unsigned long first;
	unsigned long last;
	unsigned long index;
	size_t length;
	size_t code;

	/* Each code fills the indices its bits start. */
	for (code = 0; code < count; code++) {
		/* The code's value and length, and the range of indices it starts. */
		value = code_value(codes[code].bits, &length);
		first = value << (CCITT_RUN_BITS - length);
		last = first + (1UL << (CCITT_RUN_BITS - length));

		/* Every index of the range gives the code. */
		for (index = first; index < last; index++) {
			table[index].run = (unsigned short)codes[code].value;
			table[index].length = (unsigned char)length;
		}
	}
}

/* Enters the pass, horizontal and vertical modes into the mode lookup table. */
static void
build_mode_table(
	struct ccitt_mode_entry *table)
{
	size_t index;

	/* The pass and the horizontal mode. */
	put_mode(table, CCITT_PASS_CODE, CCITT_MODE_PASS, 0);
	put_mode(table, CCITT_HORIZONTAL_CODE, CCITT_MODE_HORIZONTAL, 0);

	/* The seven vertical modes with their offsets. */
	for (index = 0; index < sizeof(ccitt_vertical_codes) / sizeof(ccitt_vertical_codes[0]); index++)
		put_mode(table, ccitt_vertical_codes[index].bits, CCITT_MODE_VERTICAL, ccitt_vertical_codes[index].value);
}

/* Enters one mode code into the mode lookup table: every index its bits start. */
static void
put_mode(
	struct ccitt_mode_entry *table,
	const char *bits,
	enum ccitt_mode mode,
	int offset)
{
	unsigned long value;
	unsigned long first;
	unsigned long last;
	unsigned long index;
	size_t length;

	/* The code's value and length, and the range of indices it starts. */
	value = code_value(bits, &length);
	first = value << (CCITT_MODE_BITS - length);
	last = first + (1UL << (CCITT_MODE_BITS - length));

	/* Every index of the range gives the mode. */
	for (index = first; index < last; index++) {
		table[index].mode = (unsigned char)mode;
		table[index].offset = (signed char)offset;
		table[index].length = (unsigned char)length;
	}
}

/* Reads a code's bits, written as the characters 0 and 1, into a value and a length. */
static unsigned long
code_value(
	const char *bits,
	size_t *length)
{
	unsigned long value;
	size_t index;

	/* Each character is the next bit, the first the most significant. */
	value = 0;
	for (index = 0; bits[index] != '\0'; index++) {
		value <<= 1;
		if (bits[index] == '1')
			value |= 1;
	}

	/* Reports the value; the length is the number of characters. */
	*length = index;
	return value;
}

/*
 * Reports the next bits of the data (at most 24) without reading them,
 * the first the most significant; bits past the end read as zero.
 */
static unsigned long
peek_bits(
	const struct ccitt_decoder *decoder,
	unsigned count)
{
	unsigned long window;
	size_t byte;
	unsigned shift;
	unsigned index;

	/* The four bytes from the one the next bit is in, zero past the end. */
	byte = decoder->position / 8;
	shift = (unsigned)(decoder->position % 8);
	window = 0;
	for (index = 0; index < 4; index++) {
		window <<= 8;
		if (decoder->position < decoder->total && byte + index < decoder->size)
			window |= decoder->data[byte + index];
	}

	/* The count bits after the ones already read. */
	window = (window << shift) & 0xffffffffUL;
	return window >> (32 - count);
}

/* Reads (passes over) some bits of the data. */
static void
eat_bits(
	struct ccitt_decoder *decoder,
	size_t count)
{
	/* The position moves on, at most to the end. */
	decoder->position += count;
	if (decoder->position > decoder->total)
		decoder->position = decoder->total;
}

/*
 * Decodes rows until the data, the end-of-block code or the rows asked for
 * end, or until damage the parameters do not allow.
 *
 * Returns 0 (the rows decoded are in the output), or ENOMEM.
 */
static int
decode_rows(
	struct ccitt_decoder *decoder)
{
	const struct pdf_ccitt_parameters *parameters;
	int damaged_row;
	int found;
	int error;

	/* Passes over the fill and the end-of-line the data may start with. */
	parameters = decoder->parameters;
	start_data(decoder);

	/* One row a turn. */
	while (!decoder->finished) {
		/* The rows asked for, or the bits, have run out. */
		if (parameters->rows > 0 && decoder->rows_done >= (size_t)parameters->rows)
			break;
		if (decoder->position >= decoder->total)
			break;

		/* The row, by the coding of the data and, for mixed data, the row's tag. */
		if (parameters->k < 0) {
			error = decode_two_dimensional(decoder);
		} else if (parameters->k == 0) {
			error = decode_one_dimensional(decoder);
		} else if (decoder->two_dimensional) {
			error = decode_two_dimensional(decoder);
		} else {
			error = decode_one_dimensional(decoder);
		}

		/* A damaged row ends the decode unless end-of-line codes let the decoder find the next row. */
		damaged_row = 0;
		if (error != 0) {
			decoder->damaged++;
			if (!decoder->end_of_line)
				break;
			if (decoder->damaged > parameters->damaged_rows)
				break;
			damaged_row = 1;
		}

		/* The row (a damaged one as far as it was decoded) goes out and is the next row's reference. */
		error = put_row(decoder);
		if (error != 0)
			return error;
		make_reference(decoder);

		/* The next row follows the next end-of-line after a damaged one; the data may end first. */
		if (damaged_row) {
			found = skip_to_end_of_line(decoder);
			if (!found)
				break;
			continue;
		}

		/* What comes between this row and the next. */
		between_rows(decoder);
	}

	/* Succeeded: the rows are decoded. */
	return 0;
}

/*
 * Passes over the zero fill and the end-of-line code the data may start
 * with (which says that the data has them), and reads the first row's tag
 * of mixed data.
 */
static void
start_data(
	struct ccitt_decoder *decoder)
{
	unsigned long code;

	/* Passes over zero bits while twelve of them are ahead. */
	code = peek_bits(decoder, CCITT_EOL_BITS);
	while (code == 0 && decoder->position < decoder->total) {
		eat_bits(decoder, 1);
		code = peek_bits(decoder, CCITT_EOL_BITS);
	}

	/* An end-of-line code at the start means that every row has one. */
	if (code == CCITT_EOL) {
		eat_bits(decoder, CCITT_EOL_BITS);
		decoder->end_of_line = 1;
	}

	/* Mixed data tags each row: 1 for one-dimensional, 0 for two-dimensional. */
	if (decoder->parameters->k > 0) {
		code = peek_bits(decoder, 1);
		decoder->two_dimensional = 0;
		if (code == 0)
			decoder->two_dimensional = 1;
		eat_bits(decoder, 1);
	}
}

/*
 * Decodes a one-dimensional row: runs of white and black in turn, from a
 * white one, until the row's width.
 *
 * Returns 0, or PDF_EFORMAT for a damaged row (its changes so far kept).
 */
static int
decode_one_dimensional(
	struct ccitt_decoder *decoder)
{
	long columns;
	long position;
	long run;
	int color;
	int error;

	/* The row starts white at its left edge, with no change yet. */
	columns = decoder->parameters->columns;
	decoder->coding_count = 0;
	position = 0;
	color = CCITT_WHITE;

	/* Each run ends where the colour changes. */
	while (position < columns) {
		/* The run's length in the colour. */
		error = read_run(decoder, color, &run);
		if (error != 0)
			return error;

		/* Its end, within the row, is a change. */
		position += run;
		if (position > columns)
			position = columns;
		error = add_change(decoder, position);
		if (error != 0)
			return error;

		/* The next run is the other colour. */
		color = 1 - color;
	}

	/* Succeeded: the row's changes are complete. */
	return 0;
}

/*
 * Decodes a two-dimensional row against the reference row: each mode code
 * moves a0, the element the decode stands on, along the row.
 *
 * a0 starts on an imaginary white element before the row.  The pass mode
 * moves a0 under b2 in the same colour.  The horizontal mode codes the two
 * runs from a0 as one-dimensional runs.  A vertical mode puts the next
 * change a1 at b1 moved by up to three elements, and a0 on it in the other
 * colour.
 *
 * Returns 0, or PDF_EFORMAT for a damaged row (its changes so far kept).
 */
static int
decode_two_dimensional(
	struct ccitt_decoder *decoder)
{
	const struct ccitt_mode_entry *entry;
	unsigned long code;
	size_t search;
	long columns;
	long a0;
	long a1;
	long a2;
	long b1;
	long b2;
	long start;
	long first_run;
	long second_run;
	int color;
	int error;

	/* The row starts on the imaginary white element, with no change yet. */
	columns = decoder->parameters->columns;
	decoder->coding_count = 0;
	search = 0;
	a0 = -1;
	color = CCITT_WHITE;

	/* Each mode code, until a0 reaches the row's end. */
	while (a0 < columns) {
		/* The mode; a code of none (an end-of-line, an extension) is damage, and so is the data's end. */
		code = peek_bits(decoder, CCITT_MODE_BITS);
		entry = &decoder->modes[code];
		if (entry->length == 0)
			return PDF_EFORMAT;
		if (decoder->position + entry->length > decoder->total)
			return PDF_EFORMAT;
		eat_bits(decoder, entry->length);

		/* The reference row's b1 and b2 for the element a0 stands on. */
		find_b1_b2(decoder, a0, color, &search, &b1, &b2);

		/* Moves a0 by the mode. */
		switch (entry->mode) {
		case CCITT_MODE_PASS:
			/* The colour goes on under b2. */
			a0 = b2;
			break;
		case CCITT_MODE_HORIZONTAL:
			/* The two runs from a0 (from the row's start for the imaginary element). */
			start = a0;
			if (start < 0)
				start = 0;
			error = read_run(decoder, color, &first_run);
			if (error != 0)
				return error;
			error = read_run(decoder, 1 - color, &second_run);
			if (error != 0)
				return error;

			/* Their ends, within the row, are two changes, and a0 stands on the second. */
			a1 = start + first_run;
			if (a1 > columns)
				a1 = columns;
			a2 = a1 + second_run;
			if (a2 > columns)
				a2 = columns;
			error = add_change(decoder, a1);
			if (error != 0)
				return error;
			error = add_change(decoder, a2);
			if (error != 0)
				return error;
			a0 = a2;
			break;
		case CCITT_MODE_VERTICAL:
			/* a1 near b1; it cannot leave the row or go back past a0. */
			a1 = b1 + entry->offset;
			if (a1 < 0 || a1 > columns)
				return PDF_EFORMAT;
			if (a1 < a0)
				return PDF_EFORMAT;

			/* The change, and a0 on it in the other colour. */
			error = add_change(decoder, a1);
			if (error != 0)
				return error;
			a0 = a1;
			color = 1 - color;
			break;
		default:
			return PDF_EFORMAT;
		}
	}

	/* Succeeded: the row's changes are complete. */
	return 0;
}

/*
 * Reads one run of a colour: make-up codes (runs of 64 and more), which
 * add up, then the terminating code (a run under 64), which ends it.
 *
 * Returns 0, or PDF_EFORMAT for bits that are no code of the colour, a
 * code past the data's end, or a run far longer than any row.
 */
static int
read_run(
	struct ccitt_decoder *decoder,
	int color,
	long *run)
{
	const struct ccitt_run_entry *table;
	const struct ccitt_run_entry *entry;
	unsigned long code;
	long total;

	/* The colour's table. */
	table = decoder->white;
	if (color == CCITT_BLACK)
		table = decoder->black;

	/* Codes until a terminating one. */
	total = 0;
	for (;;) {
		/* The next code; bits that start none, or run past the data, are damage. */
		code = peek_bits(decoder, CCITT_RUN_BITS);
		entry = &table[code];
		if (entry->length == 0)
			return PDF_EFORMAT;
		if (decoder->position + entry->length > decoder->total)
			return PDF_EFORMAT;
		eat_bits(decoder, entry->length);

		/* Adds its run; a run longer than twice the widest row is damage. */
		total += entry->run;
		if (total > 2L * CCITT_COLUMNS_MAX)
			return PDF_EFORMAT;

		/* A terminating code ends the run. */
		if (entry->run < CCITT_MAKEUP_MIN)
			break;
	}

	/* Succeeded: the run's length. */
	*run = total;
	return 0;
}

/*
 * Finds b1, the first change of the reference row right of a0 to the
 * colour opposite a0's, and b2, the change after it.
 *
 * search is where the last search stood: a0 only moves right within a
 * row, so a change at or left of a0 is passed for good.  The reference
 * row's changes alternate from a change to black, so the colour a change
 * turns to is its index's parity; the row's width three times at the end
 * stops every search.
 */
static void
find_b1_b2(
	const struct ccitt_decoder *decoder,
	long a0,
	int color,
	size_t *search,
	long *b1,
	long *b2)
{
	size_t index;

	/* Passes the changes at or left of a0 (a0 is left of the row's end, so the end stops this). */
	index = *search;
	while (decoder->reference[index] <= a0)
		index++;
	*search = index;

	/* An even change turns to black, an odd one to white: b1 turns to the colour a0 is not. */
	if (color == CCITT_WHITE && (index % 2) != 0)
		index++;
	if (color == CCITT_BLACK && (index % 2) == 0)
		index++;

	/* b1 and the change after it. */
	*b1 = decoder->reference[index];
	*b2 = decoder->reference[index + 1];
}

/*
 * Adds a change to the row being decoded.
 *
 * Returns 0, or PDF_EFORMAT for a change left of the one before it or one
 * more than a row can have.
 */
static int
add_change(
	struct ccitt_decoder *decoder,
	long position)
{
	/* A row has at most one change a column (and some to spare for empty runs). */
	if (decoder->coding_count >= decoder->changes_capacity)
		return PDF_EFORMAT;

	/* A change never goes back. */
	if (decoder->coding_count > 0) {
		if (position < decoder->coding[decoder->coding_count - 1])
			return PDF_EFORMAT;
	}

	/* Succeeded: the change is the row's. */
	decoder->coding[decoder->coding_count] = position;
	decoder->coding_count++;
	return 0;
}

/*
 * Writes the row being decoded into the output: white, then the black
 * runs between its changes.
 *
 * Returns 0, or ENOMEM when the output cannot grow.
 */
static int
put_row(
	struct ccitt_decoder *decoder)
{
	const struct pdf_ccitt_parameters *parameters;
	unsigned char *row;
	size_t index;
	long start;
	long end;
	int color;
	int error;

	/* Room for the row. */
	parameters = decoder->parameters;
	error = reserve_rows(decoder, decoder->rows_done + 1);
	if (error != 0)
		return error;

	/* The row starts all white (ones, unless black is one). */
	row = decoder->output + decoder->rows_done * decoder->stride;
	if (parameters->black_is_1) {
		memset(row, 0x00, decoder->stride);
	} else {
		memset(row, 0xff, decoder->stride);
	}

	/* Paints each black run: from a change to black to the next change, or to the row's end. */
	start = 0;
	color = CCITT_WHITE;
	for (index = 0; index < decoder->coding_count; index++) {
		end = decoder->coding[index];
		if (color == CCITT_BLACK)
			paint_span(row, start, end, parameters->black_is_1);
		start = end;
		color = 1 - color;
	}

	/* A row that ends black is black to its end. */
	if (color == CCITT_BLACK)
		paint_span(row, start, parameters->columns, parameters->black_is_1);

	/* Succeeded: one more row is out. */
	decoder->rows_done++;
	return 0;
}

/* Paints the pixels from one place to another of a packed row black. */
static void
paint_span(
	unsigned char *row,
	long from,
	long to,
	int black_is_1)
{
	unsigned char mask;
	long pixel;

	/* The pixels up to a byte's edge one at a time. */
	pixel = from;
	while (pixel < to && (pixel % 8) != 0) {
		mask = (unsigned char)(0x80U >> (pixel % 8));
		if (black_is_1) {
			row[pixel / 8] |= mask;
		} else {
			row[pixel / 8] &= (unsigned char)~mask;
		}

		/* The next pixel. */
		pixel++;
	}

	/* Whole bytes at once. */
	while (to - pixel >= 8) {
		if (black_is_1) {
			row[pixel / 8] = 0xff;
		} else {
			row[pixel / 8] = 0x00;
		}

		/* The next byte's pixels. */
		pixel += 8;
	}

	/* The pixels past the last whole byte. */
	while (pixel < to) {
		mask = (unsigned char)(0x80U >> (pixel % 8));
		if (black_is_1) {
			row[pixel / 8] |= mask;
		} else {
			row[pixel / 8] &= (unsigned char)~mask;
		}

		/* The next pixel. */
		pixel++;
	}
}

/*
 * Makes the row just decoded the next row's reference: its changes without
 * an empty run (two equal changes) or a change at the row's end, then the
 * row's width three times.
 */
static void
make_reference(
	struct ccitt_decoder *decoder)
{
	long columns;
	long change;
	size_t count;
	size_t index;

	/* Each change joins the reference; one equal to the last cancels it (an empty run is no change). */
	columns = decoder->parameters->columns;
	count = 0;
	for (index = 0; index < decoder->coding_count; index++) {
		change = decoder->coding[index];
		if (change >= columns)
			break;
		if (count > 0 && decoder->reference[count - 1] == change) {
			count--;
			continue;
		}

		/* Any other change is the reference's next. */
		decoder->reference[count] = change;
		count++;
	}

	/* The row's end three times ends every search. */
	decoder->reference[count] = columns;
	decoder->reference[count + 1] = columns;
	decoder->reference[count + 2] = columns;
	decoder->reference_count = count + 3;
}

/*
 * Reads what comes between two rows: the zero fill and an end-of-line
 * code, the byte alignment, mixed data's tag of the next row, and the
 * end-of-block code, which finishes the decode.
 *
 * With byte alignment and no end-of-line codes, zero fill is not passed
 * over, since the last bits of a row and the first of the next may be
 * zeros that look like an end-of-line.
 */
static void
between_rows(
	struct ccitt_decoder *decoder)
{
	const struct pdf_ccitt_parameters *parameters;
	unsigned long code;
	int got_end_of_line;
	int last_row;

	/* The last row of data without an end-of-block code needs nothing after it. */
	parameters = decoder->parameters;
	last_row = 0;
	if (parameters->rows > 0 && decoder->rows_done >= (size_t)parameters->rows)
		last_row = 1;
	if (last_row && !parameters->end_of_block) {
		decoder->finished = 1;
		return;
	}

	/* The fill and an end-of-line code (with end-of-line codes, anything up to the next one). */
	got_end_of_line = 0;
	if (decoder->end_of_line || !parameters->byte_align) {
		code = peek_bits(decoder, CCITT_EOL_BITS);
		if (decoder->end_of_line) {
			while (code != CCITT_EOL && decoder->position < decoder->total) {
				eat_bits(decoder, 1);
				code = peek_bits(decoder, CCITT_EOL_BITS);
			}
		} else {
			while (code == 0 && decoder->position < decoder->total) {
				eat_bits(decoder, 1);
				code = peek_bits(decoder, CCITT_EOL_BITS);
			}
		}

		/* The end-of-line code itself. */
		if (code == CCITT_EOL) {
			eat_bits(decoder, CCITT_EOL_BITS);
			got_end_of_line = 1;
		}
	}

	/* Byte-aligned rows without an end-of-line code start on the next byte. */
	if (parameters->byte_align && !got_end_of_line) {
		if (decoder->position % 8 != 0)
			eat_bits(decoder, 8 - decoder->position % 8);
	}

	/* The data may end here. */
	if (decoder->position >= decoder->total) {
		decoder->finished = 1;
		return;
	}

	/* Mixed data tags the next row. */
	if (parameters->k > 0) {
		code = peek_bits(decoder, 1);
		decoder->two_dimensional = 0;
		if (code == 0)
			decoder->two_dimensional = 1;
		eat_bits(decoder, 1);
	}

	/* Byte-aligned rows without end-of-line codes end their block with two of them. */
	if (parameters->end_of_block &&
	    !decoder->end_of_line &&
	    parameters->byte_align) {
		code = peek_bits(decoder, CCITT_EOL_TWICE_BITS);
		if (code == CCITT_EOL_TWICE) {
			eat_bits(decoder, CCITT_EOL_BITS);
			got_end_of_line = 1;
		}
	}

	/* A second end-of-line code right after one ends the block (Group 3's six, Group 4's two). */
	if (parameters->end_of_block && got_end_of_line) {
		code = peek_bits(decoder, CCITT_EOL_BITS);
		if (code == CCITT_EOL)
			decoder->finished = 1;
	}
}

/*
 * Passes over the bits after a damaged row up to and through the next
 * end-of-line code, and reads mixed data's tag of the row after it.
 *
 * Returns 1 when one was found, 0 when the data ended first.
 */
static int
skip_to_end_of_line(
	struct ccitt_decoder *decoder)
{
	unsigned long code;

	/* One bit at a time until an end-of-line code is next. */
	code = peek_bits(decoder, CCITT_EOL_BITS);
	while (code != CCITT_EOL) {
		if (decoder->position >= decoder->total)
			return 0;
		eat_bits(decoder, 1);
		code = peek_bits(decoder, CCITT_EOL_BITS);
	}

	/* The end-of-line code itself. */
	eat_bits(decoder, CCITT_EOL_BITS);

	/* Mixed data tags the row after it. */
	if (decoder->parameters->k > 0) {
		code = peek_bits(decoder, 1);
		decoder->two_dimensional = 0;
		if (code == 0)
			decoder->two_dimensional = 1;
		eat_bits(decoder, 1);
	}

	/* Found: the next row starts here. */
	return 1;
}

/*
 * Makes room in the output for a number of rows, growing it by doubling
 * within the decode limit.
 *
 * Returns 0, or ENOMEM.
 */
static int
reserve_rows(
	struct ccitt_decoder *decoder,
	size_t rows)
{
	unsigned char *grown;
	size_t needed;
	size_t capacity;

	/* Refuses more rows than the decode limit holds. */
	if (rows > PDF_FILTER_OUTPUT_MAX / decoder->stride)
		return ENOMEM;
	needed = rows * decoder->stride;

	/* The room is already there. */
	if (needed <= decoder->output_capacity)
		return 0;

	/* Doubles the capacity until it holds the rows, bounded by the limit. */
	capacity = decoder->output_capacity;
	if (capacity < CCITT_OUTPUT_INITIAL)
		capacity = CCITT_OUTPUT_INITIAL;
	while (capacity < needed)
		capacity *= 2;
	if (capacity > PDF_FILTER_OUTPUT_MAX)
		capacity = PDF_FILTER_OUTPUT_MAX;

	/* Grows the buffer. */
	grown = realloc(decoder->output, capacity);
	if (grown == NULL)
		return ENOMEM;

	/* Succeeded: the grown buffer is the output's. */
	decoder->output = grown;
	decoder->output_capacity = capacity;
	return 0;
}
