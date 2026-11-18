/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The Compact Font Format programs of libpdf's reader (stage 3 of
 * design-pdf.md): an embedded /FontFile3 of subtype Type1C or
 * CIDFontType0C (and the CFF table of an OpenType font), read as Adobe
 * Technical Notes #5176 (CFF) and #5177 (Type 2 charstrings) describe.
 *
 * The first font of the program is read: its Top DICT, the CharStrings
 * INDEX, the charset (glyph names, or the CIDs of a CID-keyed font), the
 * program's own encoding, the private DICT with its local subroutines,
 * and for a CID-keyed font the FDArray and FDSelect that give each glyph
 * its private DICT and matrix.  A glyph's Type 2 charstring runs through
 * every drawing operator, the flex operators and the arithmetic ones; the
 * hints are counted only to skip hintmask's bytes.
 *
 * The program is not trusted: every offset and count is checked against
 * the program's size, and the stack, the call depth and the operators
 * run are bounded.
 */

#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include <pdf.h>

#include "charstrings.h"

/* The most operands a DICT operator takes. */
#define CFF_DICT_OPERANDS 48

/* The longest real number a DICT writes in nibbles. */
#define CFF_REAL_MAX 64

/*
 * One INDEX: count objects whose bytes lie between offsets that are one
 * based from the byte before the data.
 */
struct cff_index {
	const unsigned char *offsets;
	const unsigned char *base;
	size_t count;
	int offset_size;
	size_t data_size;
};

/*
 * What the reader takes from a Top DICT, a font DICT or a private DICT
 * (each kind uses its own fields; -1 is an offset the DICT did not give).
 */
struct cff_dict {
	long charset;
	long encoding;
	long charstrings;
	long private_size;
	long private_offset;
	long subrs;
	long fd_array;
	long fd_select;
	long charstring_type;
	double default_width;
	double nominal_width;
	double matrix[6];
	int has_matrix;
	int has_ros;
};

/*
 * A glyph's charstring while it runs: the argument stack, the transient
 * array, the pen, the width, the stems counted for hintmask, and the
 * operators run so far.
 */
struct cff_run {
	struct pdf_charstrings *font;
	const struct charstrings_private *private;
	struct charstrings_path *path;
	double stack[CHARSTRINGS_STACK_MAX];
	int count;
	double transient[CHARSTRINGS_TRANSIENT_MAX];
	double x;
	double y;
	double offset_x;
	double offset_y;
	double width;
	int have_width;
	int stems;
	long operators;
	int ended;
};

static int read_index(const unsigned char *data, size_t size, size_t *position, struct cff_index *index);
static int index_get(const struct cff_index *index, size_t item, struct charstrings_range *range);
static int read_dict(const unsigned char *data, size_t size, struct cff_dict *dict);
static void apply_operator(struct cff_dict *dict, int operator, const double *operands, int count);
static long dict_integer(double value);
static int read_real(const unsigned char *data, size_t size, size_t *position, double *value);
static int read_subrs(const unsigned char *data, size_t size, long offset, struct charstrings_range **subrs, size_t *count);
static int read_private(const unsigned char *data, size_t size, const struct cff_dict *dict, const double *matrix, struct charstrings_private *private);
static int read_charset(struct pdf_charstrings *font, const unsigned char *data, size_t size, long offset, unsigned short *values);
static int read_names(struct pdf_charstrings *font, const unsigned short *sids, const struct cff_index *strings);
static int read_cids(struct pdf_charstrings *font, const unsigned short *cids);
static void read_encoding(struct pdf_charstrings *font, const unsigned char *data, size_t size, long offset, const struct cff_index *strings);
static int glyph_of_sid(const struct pdf_charstrings *font, unsigned sid, const struct cff_index *strings, unsigned *glyph);
static int read_cid_fonts(struct pdf_charstrings *font, const unsigned char *data, size_t size, const struct cff_dict *top);
static int read_fd_select(struct pdf_charstrings *font, const unsigned char *data, size_t size, long offset);
static void multiply(const double first[6], const double second[6], double product[6]);
static unsigned read_u16(const unsigned char *bytes);
static void run_charstring(struct cff_run *run, const struct charstrings_range *range, int depth, int seac_depth);
static int run_operator(struct cff_run *run, int operator, const unsigned char *data, size_t size, size_t *position, int depth, int seac_depth);
static void run_escape(struct cff_run *run, int operator);
static int run_arithmetic(struct cff_run *run, int operator);
static int run_stack_operator(struct cff_run *run, int operator);
static void take_width(struct cff_run *run, int extra);
static void count_stems(struct cff_run *run);
static void call_subr(struct cff_run *run, int global, int depth, int seac_depth);
static void run_seac(struct cff_run *run, int seac_depth);
static void pen_move(struct cff_run *run, double dx, double dy);
static void pen_line(struct cff_run *run, double dx, double dy);
static void pen_curve(struct cff_run *run, double dx1, double dy1, double dx2, double dy2, double dx3, double dy3);
static void run_lines(struct cff_run *run, int horizontal);
static void run_alternating_curves(struct cff_run *run, int horizontal);
static void run_flex(struct cff_run *run, int operator);
static int push(struct cff_run *run, double value);
static long subr_bias(size_t count);

/*
 * Reads a CFF font program: its first font's glyphs, names or CIDs,
 * encoding and private DICTs.  The program points into data, which the
 * caller keeps as long as the program.
 */
int
pdf_cff_open(
	const unsigned char *data,
	size_t size,
	struct pdf_charstrings **font)
{
	struct pdf_charstrings *created;
	struct cff_index names;
	struct cff_index tops;
	struct cff_index strings;
	struct cff_index globals;
	struct cff_index charstrings;
	struct charstrings_range top_range;
	struct cff_dict top;
	unsigned short *charset;
	size_t position;
	size_t index;
	int error;

	/* The header: major version 1, and its own size. */
	if (size < 4)
		return PDF_EFORMAT;
	if (data[0] != 1)
		return ENOTSUP;
	position = data[2];
	if (position < 4 || position > size)
		return PDF_EFORMAT;

	/* The Name, Top DICT, String and Global Subr INDEXes, one after another. */
	error = read_index(data, size, &position, &names);
	if (error != 0)
		return error;
	error = read_index(data, size, &position, &tops);
	if (error != 0)
		return error;
	error = read_index(data, size, &position, &strings);
	if (error != 0)
		return error;
	error = read_index(data, size, &position, &globals);
	if (error != 0)
		return error;

	/* The first font's Top DICT. */
	error = index_get(&tops, 0, &top_range);
	if (error != 0)
		return error;
	error = read_dict(top_range.data, top_range.size, &top);
	if (error != 0)
		return error;

	/* Only Type 2 charstrings are read. */
	if (top.charstring_type != 2)
		return ENOTSUP;
	if (top.charstrings < 0 || (unsigned long)top.charstrings >= size)
		return PDF_EFORMAT;

	/* The CharStrings INDEX. */
	position = (size_t)top.charstrings;
	error = read_index(data, size, &position, &charstrings);
	if (error != 0)
		return error;
	if (charstrings.count == 0 || charstrings.count > CHARSTRINGS_GLYPHS_MAX)
		return PDF_EFORMAT;

	/* Allocates the program with its matrix (a thousandth of an em a unit by default). */
	created = calloc(1, sizeof(*created));
	if (created == NULL)
		return ENOMEM;
	created->kind = CHARSTRINGS_CFF;
	created->glyphs = charstrings.count;
	created->matrix[0] = 0.001;
	created->matrix[3] = 0.001;
	if (top.has_matrix)
		memcpy(created->matrix, top.matrix, sizeof(created->matrix));

	/* Each glyph's charstring. */
	created->charstrings = calloc(created->glyphs, sizeof(*created->charstrings));
	if (created->charstrings == NULL) {
		pdf_charstrings_close(created);
		return ENOMEM;
	}

	/* A charstring that is not in the INDEX draws nothing. */
	for (index = 0; index < created->glyphs; index++) {
		error = index_get(&charstrings, index, &created->charstrings[index]);
		if (error != 0) {
			created->charstrings[index].data = NULL;
			created->charstrings[index].size = 0;
		}
	}

	/* The global subroutines. */
	created->global_subrs_count = globals.count;
	if (globals.count > 0) {
		created->global_subrs = calloc(globals.count, sizeof(*created->global_subrs));
		if (created->global_subrs == NULL) {
			pdf_charstrings_close(created);
			return ENOMEM;
		}

		/* Each one's bytes (a subroutine outside the INDEX is empty). */
		for (index = 0; index < globals.count; index++)
			(void)index_get(&globals, index, &created->global_subrs[index]);
	}

	/* The charset: each glyph's SID, or CID for a CID-keyed font. */
	charset = calloc(created->glyphs, sizeof(*charset));
	if (charset == NULL) {
		pdf_charstrings_close(created);
		return ENOMEM;
	}

	/* Reads the charset into it. */
	error = read_charset(created, data, size, top.charset, charset);
	if (error != 0) {
		free(charset);
		pdf_charstrings_close(created);
		return error;
	}

	/* A CID-keyed font: its CIDs, font DICTs and their private DICTs. */
	if (top.has_ros) {
		created->cid_keyed = 1;
		error = read_cids(created, charset);
		if (error == 0)
			error = read_cid_fonts(created, data, size, &top);
		free(charset);
		if (error != 0) {
			pdf_charstrings_close(created);
			return error;
		}

		/* Succeeded: the caller owns the CID-keyed program. */
		*font = created;
		return 0;
	}

	/* Otherwise its names, its private DICT and its encoding. */
	error = read_names(created, charset, &strings);
	if (error == 0)
		error = charstrings_sort_names(created);
	if (error != 0) {
		free(charset);
		pdf_charstrings_close(created);
		return error;
	}

	/* The one private DICT. */
	created->privates = calloc(1, sizeof(*created->privates));
	if (created->privates == NULL) {
		free(charset);
		pdf_charstrings_close(created);
		return ENOMEM;
	}

	/* Reads the one private DICT of a font that is not CID-keyed. */
	created->privates_count = 1;
	error = read_private(data, size, &top, created->matrix, &created->privates[0]);
	if (error != 0) {
		free(charset);
		pdf_charstrings_close(created);
		return error;
	}

	/* The program's own encoding. */
	read_encoding(created, data, size, top.encoding, &strings);
	free(charset);

	/* Succeeded: the caller owns the program. */
	*font = created;
	return 0;
}

/*
 * Runs a glyph's charstring into the path; *width is its advance in glyph
 * space.
 */
void
pdf_cff_run(
	struct pdf_charstrings *font,
	unsigned glyph,
	struct charstrings_path *path,
	double *width)
{
	struct cff_run run;
	size_t private_index;

	/* Starts with an empty stack at the origin, with the glyph's private DICT. */
	memset(&run, 0, sizeof(run));
	run.font = font;
	run.path = path;
	private_index = 0;
	if (font->private_of != NULL)
		private_index = font->private_of[glyph];
	run.private = &font->privates[private_index];

	/* Runs the charstring. */
	run_charstring(&run, &font->charstrings[glyph], 0, 0);

	/* The width the charstring gave, or the default. */
	*width = run.private->default_width;
	if (run.have_width)
		*width = run.width;
}

/* Reads an INDEX at a position and moves past it. */
static int
read_index(
	const unsigned char *data,
	size_t size,
	size_t *position,
	struct cff_index *index)
{
	size_t offsets_size;
	size_t last;
	int byte;

	/* The count; an empty INDEX is its two bytes. */
	memset(index, 0, sizeof(*index));
	if (size - *position < 2 || *position > size)
		return PDF_EFORMAT;
	index->count = read_u16(data + *position);
	*position += 2;
	if (index->count == 0)
		return 0;

	/* The offset size and the offsets. */
	if (*position >= size)
		return PDF_EFORMAT;
	index->offset_size = data[*position];
	*position += 1;
	if (index->offset_size < 1 || index->offset_size > 4)
		return PDF_EFORMAT;
	offsets_size = (index->count + 1) * (size_t)index->offset_size;
	if (offsets_size > size - *position)
		return PDF_EFORMAT;
	index->offsets = data + *position;
	*position += offsets_size;

	/* The last offset gives the data's size. */
	last = 0;
	for (byte = 0; byte < index->offset_size; byte++)
		last = (last << 8) | index->offsets[index->count * (size_t)index->offset_size + (size_t)byte];
	if (last < 1 || last - 1 > size - *position)
		return PDF_EFORMAT;
	index->data_size = last - 1;
	index->base = data + *position;
	*position += index->data_size;

	/* Succeeded (each object's offsets are checked when it is read). */
	return 0;
}

/* Finds one object of an INDEX. */
static int
index_get(
	const struct cff_index *index,
	size_t item,
	struct charstrings_range *range)
{
	size_t start;
	size_t end;
	int byte;

	/* Refuses an object the INDEX does not have. */
	if (item >= index->count)
		return PDF_EFORMAT;

	/* Its offset and the next one. */
	start = 0;
	end = 0;
	for (byte = 0; byte < index->offset_size; byte++) {
		start = (start << 8) | index->offsets[item * (size_t)index->offset_size + (size_t)byte];
		end = (end << 8) | index->offsets[(item + 1) * (size_t)index->offset_size + (size_t)byte];
	}

	/* Refuses offsets out of order or past the data. */
	if (start < 1 || end < start)
		return PDF_EFORMAT;
	if (end - 1 > index->data_size)
		return PDF_EFORMAT;

	/* Succeeded: the object's bytes. */
	range->data = index->base + start - 1;
	range->size = end - start;
	return 0;
}

/* Reads a DICT's operators and the operands before each. */
static int
read_dict(
	const unsigned char *data,
	size_t size,
	struct cff_dict *dict)
{
	double operands[CFF_DICT_OPERANDS];
	size_t position;
	int count;
	int byte;
	int error;

	/* The defaults of the specification. */
	memset(dict, 0, sizeof(*dict));
	dict->charset = 0;
	dict->encoding = 0;
	dict->charstrings = -1;
	dict->private_offset = -1;
	dict->subrs = -1;
	dict->fd_array = -1;
	dict->fd_select = -1;
	dict->charstring_type = 2;

	/* Reads each operand and operator. */
	position = 0;
	count = 0;
	while (position < size) {
		byte = data[position];
		position++;

		/* An operator applies the operands before it. */
		if (byte <= 21) {
			if (byte == 12) {
				if (position >= size)
					return PDF_EFORMAT;
				byte = 1200 + data[position];
				position++;
			}

			/* Applies it; the operands are used up. */
			apply_operator(dict, byte, operands, count);
			count = 0;
			continue;
		}

		/* Refuses more operands than an operator takes. */
		if (count >= CFF_DICT_OPERANDS)
			return PDF_EFORMAT;

		/* An operand of one of the number forms. */
		if (byte == 28) {
			if (size - position < 2)
				return PDF_EFORMAT;
			operands[count] = (double)(short)read_u16(data + position);
			position += 2;
		} else if (byte == 29) {
			if (size - position < 4)
				return PDF_EFORMAT;
			operands[count] = (double)(((unsigned long)read_u16(data + position) << 16) | read_u16(data + position + 2));
			if (operands[count] >= 2147483648.0)
				operands[count] -= 4294967296.0;
			position += 4;
		} else if (byte == 30) {
			error = read_real(data, size, &position, &operands[count]);
			if (error != 0)
				return error;
		} else if (byte >= 32 && byte <= 246) {
			operands[count] = (double)(byte - 139);
		} else if (byte >= 247 && byte <= 250) {
			if (position >= size)
				return PDF_EFORMAT;
			operands[count] = (double)((byte - 247) * 256 + data[position] + 108);
			position++;
		} else if (byte >= 251 && byte <= 254) {
			if (position >= size)
				return PDF_EFORMAT;
			operands[count] = (double)(-(byte - 251) * 256 - data[position] - 108);
			position++;
		} else {
			return PDF_EFORMAT;
		}

		/* The operand waits for its operator. */
		count++;
	}

	/* Succeeded: the DICT is read. */
	return 0;
}

/*
 * Applies one DICT operator (an escaped one is 1200 plus its second byte)
 * to the fields the reader keeps.
 */
static void
apply_operator(
	struct cff_dict *dict,
	int operator,
	const double *operands,
	int count)
{
	int index;

	/* Every operator the reader uses takes its operands from the end. */
	if (count < 1)
		return;

	/* By the operator. */
	switch (operator) {
	case 15:
		dict->charset = dict_integer(operands[count - 1]);
		break;
	case 16:
		dict->encoding = dict_integer(operands[count - 1]);
		break;
	case 17:
		dict->charstrings = dict_integer(operands[count - 1]);
		break;
	case 18:
		/* Private: the size, then the offset. */
		if (count >= 2) {
			dict->private_size = dict_integer(operands[count - 2]);
			dict->private_offset = dict_integer(operands[count - 1]);
		}

		break;
	case 19:
		dict->subrs = dict_integer(operands[count - 1]);
		break;
	case 20:
		dict->default_width = operands[count - 1];
		break;
	case 21:
		dict->nominal_width = operands[count - 1];
		break;
	case 1206:
		dict->charstring_type = dict_integer(operands[count - 1]);
		break;
	case 1207:
		/* FontMatrix: six numbers. */
		if (count >= 6) {
			for (index = 0; index < 6; index++)
				dict->matrix[index] = operands[count - 6 + index];
			dict->has_matrix = 1;
		}

		break;
	case 1230:
		dict->has_ros = 1;
		break;
	case 1236:
		dict->fd_array = dict_integer(operands[count - 1]);
		break;
	case 1237:
		dict->fd_select = dict_integer(operands[count - 1]);
		break;
	default:
		break;
	}
}

/* Converts a DICT's number to an offset, count or type: -1 for one out of range (or not a number). */
static long
dict_integer(
	double value)
{
	/* Refuses what a long of 32 bits cannot hold. */
	if (!(value > -2147483648.0 && value < 2147483648.0))
		return -1;

	/* The whole number. */
	return (long)value;
}

/* Reads a DICT's real number: nibbles of digits, point, exponent and sign, up to 0xf. */
static int
read_real(
	const unsigned char *data,
	size_t size,
	size_t *position,
	double *value)
{
	static const char *const nibbles[16] = {
		"0", "1", "2", "3", "4", "5", "6", "7", "8", "9", ".", "E", "E-", "", "-", ""
	};
	char text[CFF_REAL_MAX + 4];
	size_t length;
	size_t piece;
	int nibble;
	int half;
	int done;

	/* Reads nibbles until the end one, high then low of each byte. */
	length = 0;
	done = 0;
	while (!done) {
		if (*position >= size)
			return PDF_EFORMAT;
		for (half = 0; half < 2 && !done; half++) {
			nibble = data[*position];
			if (half == 0) {
				nibble >>= 4;
			} else {
				nibble &= 0x0f;
			}

			/* 0xf ends the number. */
			if (nibble == 0x0f) {
				done = 1;
				continue;
			}

			/* Any other adds its characters, within the longest number read. */
			if (length + 2 > CFF_REAL_MAX)
				return PDF_EFORMAT;
			piece = strlen(nibbles[nibble]);
			memcpy(text + length, nibbles[nibble], piece);
			length += piece;
		}

		/* The byte's two nibbles are read. */
		(*position)++;
	}

	/* Converts the text. */
	text[length] = '\0';
	*value = strtod(text, NULL);
	if (!(*value > -1e12 && *value < 1e12))
		*value = 0.0;

	/* Succeeded: the number. */
	return 0;
}

/* Reads the subroutines INDEX at an offset. */
static int
read_subrs(
	const unsigned char *data,
	size_t size,
	long offset,
	struct charstrings_range **subrs,
	size_t *count)
{
	struct cff_index index;
	size_t position;
	size_t item;
	int error;

	/* No subroutines without an offset. */
	*subrs = NULL;
	*count = 0;
	if (offset <= 0 || (unsigned long)offset >= size)
		return 0;

	/* The INDEX. */
	position = (size_t)offset;
	error = read_index(data, size, &position, &index);
	if (error != 0)
		return 0;
	if (index.count == 0 || index.count > CHARSTRINGS_SUBRS_MAX)
		return 0;

	/* Each subroutine's bytes. */
	*subrs = calloc(index.count, sizeof(**subrs));
	if (*subrs == NULL)
		return ENOMEM;
	for (item = 0; item < index.count; item++)
		(void)index_get(&index, item, &(*subrs)[item]);

	/* Succeeded: the subroutines. */
	*count = index.count;
	return 0;
}

/*
 * Reads the private DICT a Top DICT or font DICT names, with its local
 * subroutines (their offset is from the private DICT's start).
 */
static int
read_private(
	const unsigned char *data,
	size_t size,
	const struct cff_dict *dict,
	const double *matrix,
	struct charstrings_private *private)
{
	struct cff_dict private_dict;
	int error;

	/* The matrix the glyphs of this private DICT are drawn with. */
	memcpy(private->matrix, matrix, sizeof(private->matrix));

	/* A font without a private DICT has no subroutines and zero widths. */
	if (dict->private_offset < 0 || dict->private_size <= 0)
		return 0;
	if ((unsigned long)dict->private_offset > size)
		return 0;
	if ((unsigned long)dict->private_size > size - (unsigned long)dict->private_offset)
		return 0;

	/* The DICT. */
	error = read_dict(data + dict->private_offset, (size_t)dict->private_size, &private_dict);
	if (error != 0)
		return 0;
	private->default_width = private_dict.default_width;
	private->nominal_width = private_dict.nominal_width;

	/* Its subroutines. */
	if (private_dict.subrs <= 0)
		return 0;
	if (private_dict.subrs > (long)(size - (size_t)dict->private_offset))
		return 0;
	error = read_subrs(data, size, dict->private_offset + private_dict.subrs, &private->subrs, &private->subrs_count);
	if (error != 0)
		return error;

	/* Succeeded: the private DICT is read. */
	return 0;
}

/*
 * Reads the charset into each glyph's SID (or CID): one of the predefined
 * charsets by its number, or a charset of format 0, 1 or 2 at an offset.
 */
static int
read_charset(
	struct pdf_charstrings *font,
	const unsigned char *data,
	size_t size,
	long offset,
	unsigned short *values)
{
	size_t glyph;
	size_t position;
	size_t left;
	size_t step;
	size_t need;
	unsigned first;
	int format;

	/* Glyph 0 is .notdef. */
	values[0] = 0;

	/* ISOAdobe: glyph i is SID i, up to SID 228. */
	if (offset == 0) {
		for (glyph = 1; glyph < font->glyphs && glyph <= 228; glyph++)
			values[glyph] = (unsigned short)glyph;
		return 0;
	}

	/* The Expert charsets. */
	if (offset == 1 || offset == 2) {
		for (glyph = 1; glyph < font->glyphs; glyph++) {
			if (offset == 1 && glyph - 1 < pdf_cff_expert_charset_count)
				values[glyph] = pdf_cff_expert_charset[glyph - 1];
			if (offset == 2 && glyph - 1 < pdf_cff_expert_subset_charset_count)
				values[glyph] = pdf_cff_expert_subset_charset[glyph - 1];
		}

		/* The charset is the predefined one. */
		return 0;
	}

	/* A charset of its own: its format. */
	if (offset < 0 || (unsigned long)offset >= size)
		return PDF_EFORMAT;
	position = (size_t)offset;
	format = data[position];
	position++;

	/* Format 0: each glyph's value in turn. */
	glyph = 1;
	if (format == 0) {
		for (; glyph < font->glyphs; glyph++) {
			if (size - position < 2)
				break;
			values[glyph] = (unsigned short)read_u16(data + position);
			position += 2;
		}

		/* The glyphs the charset had room for have their values. */
		return 0;
	}

	/* Formats 1 and 2: runs of consecutive values, their lengths one or two bytes. */
	if (format != 1 && format != 2)
		return PDF_EFORMAT;
	need = 3;
	if (format == 2)
		need = 4;
	while (glyph < font->glyphs) {
		/* The run's first value and how many follow it. */
		if (size - position < need)
			break;
		first = read_u16(data + position);
		if (format == 1) {
			left = data[position + 2];
		} else {
			left = read_u16(data + position + 2);
		}

		/* The run's header is read. */
		position += need;

		/* The run's glyphs. */
		for (step = 0; step <= left && glyph < font->glyphs; step++) {
			values[glyph] = (unsigned short)((first + step) & 0xffffU);
			glyph++;
		}
	}

	/* Succeeded: every glyph the charset covers has its value. */
	return 0;
}

/* Gives each glyph its name from its SID: a standard string, or the String INDEX's. */
static int
read_names(
	struct pdf_charstrings *font,
	const unsigned short *sids,
	const struct cff_index *strings)
{
	size_t glyph;
	int error;

	/* Allocates the names. */
	font->names = calloc(font->glyphs, sizeof(*font->names));
	if (font->names == NULL)
		return ENOMEM;

	/* Each glyph's name. */
	for (glyph = 0; glyph < font->glyphs; glyph++) {
		if (sids[glyph] < PDF_CFF_STANDARD_STRINGS) {
			font->names[glyph].data = (const unsigned char *)pdf_cff_standard_strings[sids[glyph]];
			font->names[glyph].size = strlen(pdf_cff_standard_strings[sids[glyph]]);
			continue;
		}

		/* A string of the font's own; one outside the INDEX leaves the glyph without a name. */
		error = index_get(strings, (size_t)sids[glyph] - PDF_CFF_STANDARD_STRINGS, &font->names[glyph]);
		if (error != 0) {
			font->names[glyph].data = NULL;
			font->names[glyph].size = 0;
		}
	}

	/* Succeeded: the glyphs have names. */
	return 0;
}

/* Lists a CID-keyed font's CIDs with their glyphs, sorted by CID. */
static int
read_cids(
	struct pdf_charstrings *font,
	const unsigned short *cids)
{
	struct charstrings_cid *list;
	struct charstrings_cid moved;
	size_t glyph;
	size_t count;
	size_t index;
	unsigned previous;
	int sorted;

	/* Allocates the list. */
	list = calloc(font->glyphs, sizeof(*list));
	if (list == NULL)
		return ENOMEM;

	/* Each glyph's CID, in glyph order (charsets list CIDs rising, which is checked). */
	count = 0;
	previous = 0;
	sorted = 1;
	for (glyph = 0; glyph < font->glyphs; glyph++) {
		if (glyph > 0 && cids[glyph] <= previous)
			sorted = 0;
		previous = cids[glyph];
		list[count].cid = cids[glyph];
		list[count].glyph = (unsigned)glyph;
		count++;
	}

	/* A charset out of order is sorted by insertion (they are nearly sorted when they are not). */
	if (!sorted) {
		for (glyph = 1; glyph < count; glyph++) {
			moved = list[glyph];
			index = glyph;
			while (index > 0 && list[index - 1].cid > moved.cid) {
				list[index] = list[index - 1];
				index--;
			}

			/* The CID's place among those before it. */
			list[index] = moved;
		}
	}

	/* Succeeded: the CIDs can be searched. */
	font->cids = list;
	font->cids_count = count;
	return 0;
}

/*
 * Reads the program's own encoding: StandardEncoding (0) through the
 * glyphs' names, or a format 0 or 1 encoding at an offset with its
 * supplements.  The Expert encoding (1) is not read.
 */
static void
read_encoding(
	struct pdf_charstrings *font,
	const unsigned char *data,
	size_t size,
	long offset,
	const struct cff_index *strings)
{
	size_t position;
	size_t count;
	size_t index;
	size_t glyph;
	unsigned code;
	unsigned left;
	unsigned sid;
	int format;
	int error;

	/* StandardEncoding by the names of its codes. */
	if (offset == 0) {
		for (code = 0; code < 256; code++) {
			error = charstrings_standard_glyph(font, code, &font->builtin[code]);
			if (error != 0)
				font->builtin[code] = 0;
		}

		/* The program's encoding is StandardEncoding. */
		font->has_builtin = 1;
		return;
	}

	/* The Expert encoding, or an offset past the program, is not read. */
	if (offset < 2 || (unsigned long)offset >= size)
		return;
	position = (size_t)offset;
	format = data[position];
	position++;

	/* Format 0: a code for each glyph from 1; format 1: runs of codes. */
	glyph = 1;
	if ((format & 0x7f) == 0) {
		if (position >= size)
			return;
		count = data[position];
		position++;
		for (index = 0; index < count && glyph < font->glyphs; index++) {
			if (position >= size)
				return;
			font->builtin[data[position]] = (unsigned)glyph;
			position++;
			glyph++;
		}
	} else if ((format & 0x7f) == 1) {
		if (position >= size)
			return;
		count = data[position];
		position++;
		for (index = 0; index < count; index++) {
			if (size - position < 2)
				return;
			code = data[position];
			left = data[position + 1];
			position += 2;
			for (; glyph < font->glyphs && code < 256; code++) {
				font->builtin[code] = (unsigned)glyph;
				glyph++;
				if (left == 0)
					break;
				left--;
			}
		}
	} else {
		return;
	}

	/* The program has an encoding of its own. */
	font->has_builtin = 1;

	/* The supplements: more codes for glyphs named by SID. */
	if ((format & 0x80) == 0)
		return;
	if (position >= size)
		return;
	count = data[position];
	position++;
	for (index = 0; index < count; index++) {
		if (size - position < 3)
			return;
		code = data[position];
		sid = read_u16(data + position + 1);
		position += 3;
		error = glyph_of_sid(font, sid, strings, &font->builtin[code]);
		if (error != 0)
			font->builtin[code] = 0;
	}
}

/* Finds the glyph whose name a SID names. */
static int
glyph_of_sid(
	const struct pdf_charstrings *font,
	unsigned sid,
	const struct cff_index *strings,
	unsigned *glyph)
{
	struct charstrings_range name;
	int error;

	/* The SID's string. */
	if (sid < PDF_CFF_STANDARD_STRINGS) {
		name.data = (const unsigned char *)pdf_cff_standard_strings[sid];
		name.size = strlen(pdf_cff_standard_strings[sid]);
	} else {
		error = index_get(strings, (size_t)sid - PDF_CFF_STANDARD_STRINGS, &name);
		if (error != 0)
			return error;
	}

	/* The glyph of that name. */
	error = pdf_charstrings_find(font, name.data, name.size, glyph);
	if (error != 0)
		return error;

	/* Succeeded: the SID's glyph. */
	return 0;
}

/*
 * Reads a CID-keyed font's FDArray (its font DICTs, each with a private
 * DICT and perhaps a matrix) and its FDSelect (each glyph's font DICT).
 */
static int
read_cid_fonts(
	struct pdf_charstrings *font,
	const unsigned char *data,
	size_t size,
	const struct cff_dict *top)
{
	struct cff_index fds;
	struct charstrings_range range;
	struct cff_dict fd;
	double matrix[6];
	double top_matrix[6];
	size_t position;
	size_t index;
	int error;

	/* The FDArray INDEX. */
	if (top->fd_array <= 0 || (unsigned long)top->fd_array >= size)
		return PDF_EFORMAT;
	position = (size_t)top->fd_array;
	error = read_index(data, size, &position, &fds);
	if (error != 0)
		return error;
	if (fds.count == 0 || fds.count > CHARSTRINGS_PRIVATES_MAX)
		return PDF_EFORMAT;

	/* The Top DICT's own matrix is applied after a font DICT's; without one, a font DICT's matrix stands alone. */
	memset(top_matrix, 0, sizeof(top_matrix));
	top_matrix[0] = 1.0;
	top_matrix[3] = 1.0;
	if (top->has_matrix)
		memcpy(top_matrix, top->matrix, sizeof(top_matrix));

	/* Each font DICT's private DICT and matrix. */
	font->privates = calloc(fds.count, sizeof(*font->privates));
	if (font->privates == NULL)
		return ENOMEM;
	font->privates_count = fds.count;
	for (index = 0; index < fds.count; index++) {
		error = index_get(&fds, index, &range);
		if (error != 0)
			return error;
		error = read_dict(range.data, range.size, &fd);
		if (error != 0)
			return error;
		if (fd.has_matrix) {
			multiply(fd.matrix, top_matrix, matrix);
		} else {
			memcpy(matrix, font->matrix, sizeof(matrix));
		}

		/* The font DICT's private DICT. */
		error = read_private(data, size, &fd, matrix, &font->privates[index]);
		if (error != 0)
			return error;
	}

	/* Each glyph's font DICT. */
	error = read_fd_select(font, data, size, top->fd_select);
	if (error != 0)
		return error;

	/* Succeeded: every glyph has its private DICT. */
	return 0;
}

/* Reads the FDSelect (format 0 or 3); a glyph it does not cover uses the first font DICT. */
static int
read_fd_select(
	struct pdf_charstrings *font,
	const unsigned char *data,
	size_t size,
	long offset)
{
	size_t position;
	size_t glyph;
	size_t ranges;
	size_t range;
	size_t first;
	size_t next;
	unsigned fd;
	int format;

	/* Every glyph starts with the first font DICT. */
	font->private_of = calloc(font->glyphs, 1);
	if (font->private_of == NULL)
		return ENOMEM;
	if (offset <= 0 || (unsigned long)offset >= size)
		return 0;
	position = (size_t)offset;
	format = data[position];
	position++;

	/* Format 0: a font DICT for each glyph. */
	if (format == 0) {
		for (glyph = 0; glyph < font->glyphs && position < size; glyph++) {
			fd = data[position];
			position++;
			if (fd < font->privates_count)
				font->private_of[glyph] = (unsigned char)fd;
		}

		/* Every glyph the FDSelect covers has its font DICT. */
		return 0;
	}

	/* Format 3: ranges of glyphs, ended by a sentinel. */
	if (format != 3)
		return 0;
	if (size - position < 2)
		return 0;
	ranges = read_u16(data + position);
	position += 2;
	for (range = 0; range < ranges; range++) {
		if (size - position < 5)
			return 0;
		first = read_u16(data + position);
		fd = data[position + 2];
		next = read_u16(data + position + 3);
		position += 3;
		if (fd >= font->privates_count)
			continue;
		for (glyph = first; glyph < next && glyph < font->glyphs; glyph++)
			font->private_of[glyph] = (unsigned char)fd;
	}

	/* Succeeded: each glyph has its font DICT. */
	return 0;
}

/* Multiplies two affine matrices: first applied, then second. */
static void
multiply(
	const double first[6],
	const double second[6],
	double product[6])
{
	/* The linear part, then the translation. */
	product[0] = first[0] * second[0] + first[1] * second[2];
	product[1] = first[0] * second[1] + first[1] * second[3];
	product[2] = first[2] * second[0] + first[3] * second[2];
	product[3] = first[2] * second[1] + first[3] * second[3];
	product[4] = first[4] * second[0] + first[5] * second[2] + second[4];
	product[5] = first[4] * second[1] + first[5] * second[3] + second[5];
}

/* Reads a big-endian 16-bit number. */
static unsigned
read_u16(
	const unsigned char *bytes)
{
	/* The high byte first. */
	return ((unsigned)bytes[0] << 8) | bytes[1];
}

/*
 * Runs a charstring or a subroutine: numbers go on the stack, operators
 * take them.  Damage ends the glyph where it stands.
 */
static void
run_charstring(
	struct cff_run *run,
	const struct charstrings_range *range,
	int depth,
	int seac_depth)
{
	const unsigned char *data;
	unsigned long bits;
	size_t size;
	size_t position;
	double value;
	int byte;
	int pushed;
	int returned;

	/* Refuses calls nested past the limit, and a charstring the program lacks. */
	if (depth > CHARSTRINGS_CALL_DEPTH || range->data == NULL) {
		run->ended = 1;
		return;
	}

	/* The charstring's bytes. */
	data = range->data;
	size = range->size;

	/* Reads each number or operator. */
	position = 0;
	while (position < size && !run->ended) {
		/* Bounds the work one glyph may take. */
		run->operators++;
		if (run->operators > CHARSTRINGS_OPERATORS_MAX) {
			run->ended = 1;
			return;
		}

		/* The next byte: a number's first, or an operator. */
		byte = data[position];
		position++;

		/* A number: one, two or three bytes, or 16.16 fixed. */
		if (byte == 28 || byte >= 32) {
			if (byte == 28) {
				if (size - position < 2)
					break;
				value = (double)(short)read_u16(data + position);
				position += 2;
			} else if (byte <= 246) {
				value = (double)(byte - 139);
			} else if (byte <= 250) {
				if (position >= size)
					break;
				value = (double)((byte - 247) * 256 + data[position] + 108);
				position++;
			} else if (byte <= 254) {
				if (position >= size)
					break;
				value = (double)(-(byte - 251) * 256 - data[position] - 108);
				position++;
			} else {
				if (size - position < 4)
					break;
				bits = ((unsigned long)read_u16(data + position) << 16) | read_u16(data + position + 2);
				value = (double)bits;
				if (bits >= 0x80000000UL)
					value -= 4294967296.0;
				value /= 65536.0;
				position += 4;
			}

			/* The number goes on the stack. */
			pushed = push(run, value);
			if (!pushed)
				return;
			continue;
		}

		/* An operator; return ends this call. */
		returned = run_operator(run, byte, data, size, &position, depth, seac_depth);
		if (returned)
			return;
	}
}

/*
 * Runs one operator.  Reports 1 when the call ends (return, endchar or
 * damage).
 */
static int
run_operator(
	struct cff_run *run,
	int operator,
	const unsigned char *data,
	size_t size,
	size_t *position,
	int depth,
	int seac_depth)
{
	size_t mask_bytes;
	int index;

	/* By the operator. */
	switch (operator) {
	case 1:
	case 3:
	case 18:
	case 23:
		/* The stem hints: counted for hintmask. */
		count_stems(run);
		break;
	case 19:
	case 20:
		/* hintmask and cntrmask: stems on the stack before them are vstems; the mask's bytes are skipped. */
		count_stems(run);
		mask_bytes = ((size_t)run->stems + 7) / 8;
		if (mask_bytes > size - *position) {
			run->ended = 1;
			return 1;
		}

		/* The mask is skipped: the hints are not used. */
		*position += mask_bytes;
		break;
	case 21:
		/* rmoveto. */
		take_width(run, 2);
		if (run->count >= 2)
			pen_move(run, run->stack[0], run->stack[1]);
		break;
	case 22:
		/* hmoveto. */
		take_width(run, 1);
		if (run->count >= 1)
			pen_move(run, run->stack[0], 0.0);
		break;
	case 4:
		/* vmoveto. */
		take_width(run, 1);
		if (run->count >= 1)
			pen_move(run, 0.0, run->stack[0]);
		break;
	case 5:
		/* rlineto: pairs. */
		for (index = 0; index + 1 < run->count; index += 2)
			pen_line(run, run->stack[index], run->stack[index + 1]);
		break;
	case 6:
	case 7:
		/* hlineto and vlineto: alternating, starting across or up. */
		run_lines(run, operator == 6);
		break;
	case 8:
		/* rrcurveto: groups of six. */
		for (index = 0; index + 5 < run->count; index += 6) {
			pen_curve(run, run->stack[index], run->stack[index + 1], run->stack[index + 2], run->stack[index + 3],
			    run->stack[index + 4], run->stack[index + 5]);
		}

		break;
	case 24:
		/* rcurveline: curves, then a line. */
		for (index = 0; index + 7 < run->count; index += 6) {
			pen_curve(run, run->stack[index], run->stack[index + 1], run->stack[index + 2], run->stack[index + 3],
			    run->stack[index + 4], run->stack[index + 5]);
		}

		/* The line. */
		if (index + 1 < run->count)
			pen_line(run, run->stack[index], run->stack[index + 1]);
		break;
	case 25:
		/* rlinecurve: lines, then a curve. */
		for (index = 0; index + 7 < run->count; index += 2)
			pen_line(run, run->stack[index], run->stack[index + 1]);
		if (index + 5 < run->count) {
			pen_curve(run, run->stack[index], run->stack[index + 1], run->stack[index + 2], run->stack[index + 3],
			    run->stack[index + 4], run->stack[index + 5]);
		}

		break;
	case 26:
		/* vvcurveto: an optional first dx, then groups of four up and up. */
		index = 0;
		if (run->count % 2 == 1) {
			if (run->count >= 5)
				pen_curve(run, run->stack[0], run->stack[1], run->stack[2], run->stack[3], 0.0, run->stack[4]);
			index = 5;
		}

		/* The groups of four. */
		for (; index + 3 < run->count; index += 4)
			pen_curve(run, 0.0, run->stack[index], run->stack[index + 1], run->stack[index + 2], 0.0, run->stack[index + 3]);
		break;
	case 27:
		/* hhcurveto: an optional first dy, then groups of four across and across. */
		index = 0;
		if (run->count % 2 == 1) {
			if (run->count >= 5)
				pen_curve(run, run->stack[1], run->stack[0], run->stack[2], run->stack[3], run->stack[4], 0.0);
			index = 5;
		}

		/* The groups of four. */
		for (; index + 3 < run->count; index += 4)
			pen_curve(run, run->stack[index], 0.0, run->stack[index + 1], run->stack[index + 2], run->stack[index + 3], 0.0);
		break;
	case 30:
	case 31:
		/* vhcurveto and hvcurveto: alternating curves. */
		run_alternating_curves(run, operator == 31);
		break;
	case 10:
	case 29:
		/* callsubr and callgsubr: the index is the top of the stack; the rest stays for the subroutine. */
		call_subr(run, operator == 29, depth, seac_depth);
		return run->ended;
	case 11:
		/* return. */
		return 1;
	case 14:
		/* endchar, which with four arguments draws an accented character. */
		take_width(run, 4);
		if (run->count >= 4)
			run_seac(run, seac_depth);

		/* The glyph ends. */
		charstrings_close(run->path);
		run->ended = 1;
		return 1;
	case 12:
		/* An escaped operator. */
		if (*position >= size) {
			run->ended = 1;
			return 1;
		}

		/* Runs it; the arithmetic leaves its result on the stack. */
		operator = data[*position];
		*position += 1;
		run_escape(run, operator);
		return run->ended;
	default:
		/* An operator Type 2 does not have ends the glyph. */
		run->ended = 1;
		return 1;
	}

	/* The operator took the stack. */
	run->count = 0;
	return 0;
}

/* Runs an escaped operator: the flexes, and the arithmetic that leaves its result on the stack. */
static void
run_escape(
	struct cff_run *run,
	int operator)
{
	int done;

	/* The flexes draw and clear the stack. */
	if (operator >= 34 && operator <= 37) {
		run_flex(run, operator);
		run->count = 0;
		return;
	}

	/* The arithmetic on numbers, then the operators that move the stack's elements. */
	done = run_arithmetic(run, operator);
	if (done)
		return;
	done = run_stack_operator(run, operator);
	if (done)
		return;

	/* An escaped operator Type 2 does not have, or one without its operands, ends the glyph. */
	run->ended = 1;
}

/*
 * Runs an arithmetic operator on the top of the stack.  Reports 1 when it
 * ran, 0 for another operator or missing operands.
 */
static int
run_arithmetic(
	struct cff_run *run,
	int operator)
{
	double a;
	double b;
	double result;
	int unary;

	/* The one-operand operators: not, abs, neg, sqrt. */
	unary = 0;
	if (operator == 5 || operator == 9)
		unary = 1;
	if (operator == 14 || operator == 26)
		unary = 1;
	if (unary) {
		if (run->count < 1)
			return 0;
		a = run->stack[run->count - 1];
		result = 0.0;
		switch (operator) {
		case 5:
			if (a == 0.0)
				result = 1.0;
			break;
		case 9:
			result = fabs(a);
			break;
		case 14:
			result = -a;
			break;
		default:
			if (a > 0.0)
				result = sqrt(a);
			break;
		}

		/* The operand becomes the result. */
		run->stack[run->count - 1] = result;
		return 1;
	}

	/* The two-operand operators: and, or, add, sub, div, eq, mul. */
	switch (operator) {
	case 3:
	case 4:
	case 10:
	case 11:
	case 12:
	case 15:
	case 24:
		break;
	default:
		return 0;
	}

	/* Their two operands. */
	if (run->count < 2)
		return 0;
	a = run->stack[run->count - 2];
	b = run->stack[run->count - 1];

	/* The result, by the operator. */
	result = 0.0;
	switch (operator) {
	case 3:
		if (a != 0.0 && b != 0.0)
			result = 1.0;
		break;
	case 4:
		if (a != 0.0 || b != 0.0)
			result = 1.0;
		break;
	case 10:
		result = a + b;
		break;
	case 11:
		result = a - b;
		break;
	case 12:
		if (b != 0.0)
			result = a / b;
		break;
	case 15:
		if (a == b)
			result = 1.0;
		break;
	default:
		result = a * b;
		break;
	}

	/* The two operands become the result. */
	run->count--;
	run->stack[run->count - 1] = result;
	return 1;
}

/*
 * Runs an operator that moves the stack's elements: drop, put, get,
 * ifelse, random, dup, exch, index, roll.  Reports 1 when it ran.
 */
static int
run_stack_operator(
	struct cff_run *run,
	int operator)
{
	double a;
	double b;
	double moved;
	long count;
	long shift;
	long index;

	/* By the operator. */
	switch (operator) {
	case 18:
		/* drop. */
		if (run->count > 0)
			run->count--;
		return 1;
	case 20:
		/* put: a value into the transient array. */
		if (run->count < 2)
			return 0;
		b = run->stack[run->count - 1];
		if (b >= 0.0 && b < (double)CHARSTRINGS_TRANSIENT_MAX)
			run->transient[(int)b] = run->stack[run->count - 2];
		run->count -= 2;
		return 1;
	case 21:
		/* get: a value from the transient array. */
		if (run->count < 1)
			return 0;
		b = run->stack[run->count - 1];
		run->stack[run->count - 1] = 0.0;
		if (b >= 0.0 && b < (double)CHARSTRINGS_TRANSIENT_MAX)
			run->stack[run->count - 1] = run->transient[(int)b];
		return 1;
	case 22:
		/* ifelse: s1 s2 v1 v2 leaves s1 when v1 <= v2, else s2. */
		if (run->count < 4)
			return 0;
		if (run->stack[run->count - 2] > run->stack[run->count - 1])
			run->stack[run->count - 4] = run->stack[run->count - 3];
		run->count -= 3;
		return 1;
	case 23:
		/* random: a fixed value in (0, 1], so that a glyph draws the same each time. */
		(void)push(run, 0.5);
		return 1;
	case 27:
		/* dup. */
		if (run->count < 1)
			return 0;
		(void)push(run, run->stack[run->count - 1]);
		return 1;
	case 28:
		/* exch. */
		if (run->count < 2)
			return 0;
		a = run->stack[run->count - 1];
		run->stack[run->count - 1] = run->stack[run->count - 2];
		run->stack[run->count - 2] = a;
		return 1;
	case 29:
		/* index: the element i below the top (the one under it for a negative i). */
		if (run->count < 2)
			return 0;
		b = run->stack[run->count - 1];
		index = 0;
		if (b > 0.0 && b < (double)run->count)
			index = (long)b;
		if ((long)run->count - 2 - index < 0)
			index = 0;
		run->stack[run->count - 1] = run->stack[run->count - 2 - index];
		return 1;
	case 30:
		/* roll: the top n elements turned by j. */
		if (run->count < 2)
			return 0;
		a = run->stack[run->count - 2];
		b = run->stack[run->count - 1];
		run->count -= 2;
		if (!(a > 0.0 && a <= (double)run->count))
			return 1;
		if (!(b > -1e6 && b < 1e6))
			return 1;
		count = (long)a;
		shift = ((long)b % count + count) % count;
		for (; shift > 0; shift--) {
			moved = run->stack[run->count - 1];
			for (index = run->count - 1; index > (long)run->count - count; index--)
				run->stack[index] = run->stack[index - 1];
			run->stack[run->count - count] = moved;
		}

		/* The elements are turned. */
		return 1;
	default:
		break;
	}

	/* Another operator. */
	return 0;
}

/*
 * Takes the width from the bottom of the stack when the first
 * stack-clearing operator has one more argument than it takes (extra is
 * how many it takes; hstem-like operators take pairs).
 */
static void
take_width(
	struct cff_run *run,
	int extra)
{
	int index;

	/* Only the first such operator carries the width. */
	if (run->have_width)
		return;
	run->have_width = 1;
	run->width = run->private->default_width;

	/* An extra argument at the bottom is the width, from the nominal one. */
	if (extra == 4) {
		if (run->count != 1 && run->count != 5)
			return;
	} else if (extra > 0) {
		if (run->count <= extra)
			return;
	} else {
		if (run->count % 2 == 0)
			return;
	}

	/* The width is taken off the bottom of the stack. */
	run->width = run->private->nominal_width + run->stack[0];
	for (index = 1; index < run->count; index++)
		run->stack[index - 1] = run->stack[index];
	run->count--;
}

/* Counts the stems a stem hint (or hintmask's implied vstems) gives. */
static void
count_stems(
	struct cff_run *run)
{
	/* A width may lead the first hint's pairs. */
	take_width(run, 0);
	run->stems += run->count / 2;
}

/* Calls a local or global subroutine by the index on the top of the stack, plus the bias. */
static void
call_subr(
	struct cff_run *run,
	int global,
	int depth,
	int seac_depth)
{
	const struct charstrings_range *subrs;
	double value;
	size_t count;
	long index;
	long bias;

	/* The subroutines and their count. */
	subrs = run->private->subrs;
	count = run->private->subrs_count;
	if (global) {
		subrs = run->font->global_subrs;
		count = run->font->global_subrs_count;
	}

	/* The index, biased; one the program does not have ends the glyph. */
	if (run->count < 1) {
		run->ended = 1;
		return;
	}

	/* The index comes off the stack and must be a number a subroutine index can be. */
	value = run->stack[run->count - 1];
	run->count--;
	if (!(value > -40000.0 && value < 40000.0)) {
		run->ended = 1;
		return;
	}

	/* The biased index must name a subroutine the program has. */
	bias = subr_bias(count);
	index = (long)value + bias;
	if (index < 0 || (size_t)index >= count) {
		run->ended = 1;
		return;
	}

	/* Runs the subroutine, one level deeper. */
	run_charstring(run, &subrs[index], depth + 1, seac_depth);
}

/*
 * Draws the accented character endchar's four arguments name (adx ady
 * bchar achar): the base, then the accent moved by (adx, ady).
 */
static void
run_seac(
	struct cff_run *run,
	int seac_depth)
{
	const struct charstrings_private *saved_private;
	double accent_x;
	double accent_y;
	double width;
	int have_width;
	unsigned base;
	unsigned accent;
	int error;

	/* Refuses a seac inside a seac and codes that are not codes. */
	if (seac_depth > 0)
		return;
	if (!(run->stack[2] >= 0.0 && run->stack[2] < 256.0))
		return;
	if (!(run->stack[3] >= 0.0 && run->stack[3] < 256.0))
		return;
	accent_x = run->stack[0];
	accent_y = run->stack[1];

	/* The two glyphs by their StandardEncoding codes. */
	error = charstrings_standard_glyph(run->font, (unsigned)run->stack[2], &base);
	if (error != 0)
		return;
	error = charstrings_standard_glyph(run->font, (unsigned)run->stack[3], &accent);
	if (error != 0)
		return;

	/* The base, then the accent, keeping this glyph's width. */
	width = run->width;
	have_width = run->have_width;
	saved_private = run->private;
	charstrings_close(run->path);
	run->count = 0;
	run->x = 0.0;
	run->y = 0.0;
	run->ended = 0;
	run_charstring(run, &run->font->charstrings[base], 0, seac_depth + 1);
	charstrings_close(run->path);
	run->count = 0;
	run->x = 0.0;
	run->y = 0.0;
	run->ended = 0;
	run->offset_x = accent_x;
	run->offset_y = accent_y;
	run->stems = 0;
	run_charstring(run, &run->font->charstrings[accent], 0, seac_depth + 1);
	charstrings_close(run->path);

	/* This glyph's width, and nothing more to draw. */
	run->offset_x = 0.0;
	run->offset_y = 0.0;
	run->private = saved_private;
	run->width = width;
	run->have_width = have_width;
}

/* Starts a subpath at a displacement from the pen. */
static void
pen_move(
	struct cff_run *run,
	double dx,
	double dy)
{
	/* The new point. */
	run->x += dx;
	run->y += dy;
	charstrings_move(run->path, run->x + run->offset_x, run->y + run->offset_y);
}

/* Draws a line by a displacement. */
static void
pen_line(
	struct cff_run *run,
	double dx,
	double dy)
{
	/* The line to the new point. */
	run->x += dx;
	run->y += dy;
	charstrings_line(run->path, run->x + run->offset_x, run->y + run->offset_y);
}

/* Draws a curve by three displacements, each from the point before. */
static void
pen_curve(
	struct cff_run *run,
	double dx1,
	double dy1,
	double dx2,
	double dy2,
	double dx3,
	double dy3)
{
	double x1;
	double y1;
	double x2;
	double y2;

	/* The control points and the end, in turn. */
	x1 = run->x + dx1;
	y1 = run->y + dy1;
	x2 = x1 + dx2;
	y2 = y1 + dy2;
	run->x = x2 + dx3;
	run->y = y2 + dy3;
	charstrings_curve(run->path, x1 + run->offset_x, y1 + run->offset_y, x2 + run->offset_x, y2 + run->offset_y,
	    run->x + run->offset_x, run->y + run->offset_y);
}

/* Draws hlineto's or vlineto's lines, alternating direction. */
static void
run_lines(
	struct cff_run *run,
	int horizontal)
{
	int index;

	/* Each argument is one line, across then up (or up then across). */
	for (index = 0; index < run->count; index++) {
		if (horizontal) {
			pen_line(run, run->stack[index], 0.0);
		} else {
			pen_line(run, 0.0, run->stack[index]);
		}

		/* The next line turns. */
		horizontal = !horizontal;
	}
}

/*
 * Draws hvcurveto's or vhcurveto's curves: each starts across (or up)
 * and ends up (or across), the next the other way; the last may have a
 * fifth argument for its end's other coordinate.
 */
static void
run_alternating_curves(
	struct cff_run *run,
	int horizontal)
{
	double last;
	int index;

	/* Each group of four, with the fifth argument on the last. */
	for (index = 0; index + 3 < run->count; index += 4) {
		last = 0.0;
		if (index + 5 == run->count)
			last = run->stack[index + 4];
		if (horizontal) {
			pen_curve(run, run->stack[index], 0.0, run->stack[index + 1], run->stack[index + 2], last, run->stack[index + 3]);
		} else {
			pen_curve(run, 0.0, run->stack[index], run->stack[index + 1], run->stack[index + 2], run->stack[index + 3], last);
		}

		/* The next curve starts the other way. */
		horizontal = !horizontal;
	}
}

/* Draws the two curves of flex, hflex, hflex1 or flex1 (the depth argument is ignored: always curves). */
static void
run_flex(
	struct cff_run *run,
	int operator)
{
	double *s;
	double dx;
	double dy;
	double across;
	double up;
	int index;

	/* The arguments. */
	s = run->stack;
	if (operator == 35) {
		/* flex: six displacements for each curve, and the depth. */
		if (run->count < 12)
			return;
		pen_curve(run, s[0], s[1], s[2], s[3], s[4], s[5]);
		pen_curve(run, s[6], s[7], s[8], s[9], s[10], s[11]);
	} else if (operator == 34) {
		/* hflex: level ends, the middle raised by dy2. */
		if (run->count < 7)
			return;
		pen_curve(run, s[0], 0.0, s[1], s[2], s[3], 0.0);
		pen_curve(run, s[4], 0.0, s[5], -s[2], s[6], 0.0);
	} else if (operator == 36) {
		/* hflex1: back to the start's height. */
		if (run->count < 9)
			return;
		pen_curve(run, s[0], s[1], s[2], s[3], s[4], 0.0);
		pen_curve(run, s[5], 0.0, s[6], s[7], s[8], -(s[1] + s[3] + s[7]));
	} else {
		/* flex1: the last point's one coordinate, the other back to the start. */
		if (run->count < 11)
			return;
		dx = 0.0;
		dy = 0.0;
		for (index = 0; index < 10; index += 2) {
			dx += s[index];
			dy += s[index + 1];
		}

		/* The first curve, then the second along the longer way. */
		pen_curve(run, s[0], s[1], s[2], s[3], s[4], s[5]);
		across = fabs(dx);
		up = fabs(dy);
		if (across > up) {
			pen_curve(run, s[6], s[7], s[8], s[9], s[10], -dy);
		} else {
			pen_curve(run, s[6], s[7], s[8], s[9], -dx, s[10]);
		}
	}
}

/* Pushes a number; a full stack ends the glyph (reported as 0). */
static int
push(
	struct cff_run *run,
	double value)
{
	/* Refuses a stack past its limit. */
	if (run->count >= CHARSTRINGS_STACK_MAX) {
		run->ended = 1;
		return 0;
	}

	/* Succeeded: the number is on top. */
	run->stack[run->count] = value;
	run->count++;
	return 1;
}

/* Tells the bias a subroutine index is given, by the count of subroutines. */
static long
subr_bias(
	size_t count)
{
	/* The three biases of the specification. */
	if (count < 1240)
		return 107;
	if (count < 33900)
		return 1131;

	/* The largest. */
	return 32768;
}
