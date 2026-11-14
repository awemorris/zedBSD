/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The value syntaxes of SVG attributes and the transform of a viewBox.
 */

#include "svg/svg-values.h"

#include <errno.h>
#include <float.h>
#include <math.h>
#include <string.h>

/*
 * The most significant decimal digits a number keeps in its mantissa.
 * Nineteen digits always fit an unsigned 64-bit integer (at most
 * 9999999999999999999 < 2^64); the digits past them are below the
 * precision of a float and only move the decimal point.
 */
#define VALUES_MANTISSA_DIGITS_MAX	19

/*
 * The exponent magnitude that is kept while an exponent is read.  A larger
 * exponent already decides the result (overflow or zero), so its further
 * digits are dropped instead of overflowing the counter.
 */
#define VALUES_EXPONENT_CLAMP		100000L

/*
 * The decimal exponent beyond which a mantissa of at least 1 is above the
 * largest float and a mantissa under 2^64 is below the smallest one.
 */
#define VALUES_DECIMAL_LIMIT		400L

/*
 * The magnitude from which a double rounds to infinity as a float: halfway
 * between the largest float (2^128 - 2^104) and 2^128, which is
 * 2^128 - 2^103.  A magnitude from it up is an overflow; one between the
 * largest float and it rounds down to the largest float.
 */
#define VALUES_FLOAT_OVERFLOW		3.4028235677973366e38

/* The largest power of ten that a double holds exactly (10^22). */
#define VALUES_POW10_EXACT_MAX		22

/* The longest unit name that is kept to be looked up (the longest unit is "turn" or "grad"). */
#define VALUES_UNIT_NAME_MAX		7

/* The most arguments any transform function takes (matrix). */
#define VALUES_ARGS_MAX			6

/* The degrees of one grad (CSS Values 3 §6.1: 1turn = 400grad = 360deg). */
#define VALUES_DEGREES_PER_GRAD		0.9f

/* The degrees of one radian, 180 / pi. */
#define VALUES_DEGREES_PER_RADIAN	(180.0f / VEC_PI)

/* The degrees of one turn. */
#define VALUES_DEGREES_PER_TURN		360.0f

/* How many alignment keywords preserveAspectRatio has (SVG 2 §8.7). */
#define VALUES_ALIGN_COUNT		10

/*
 * The units an argument of a transform function carries: none (a bare
 * number), px for a length, and the four angle units of CSS Values 3 §6.1.
 * An unknown unit never becomes one of these: the function is invalid.
 */
enum values_arg_unit {
	VALUES_ARG_NUMBER = 0,
	VALUES_ARG_PX,
	VALUES_ARG_DEG,
	VALUES_ARG_GRAD,
	VALUES_ARG_RAD,
	VALUES_ARG_TURN
};

/*
 * Where an alignment puts the viewBox along one axis of the viewport:
 * against the near edge, in the middle, or against the far edge.
 */
enum values_place {
	VALUES_PLACE_MIN = 0,
	VALUES_PLACE_MID,
	VALUES_PLACE_MAX
};

/* One argument of a transform function: its number and unit. */
struct values_arg {
	float value;
	enum values_arg_unit unit;
};

/* One entry of a table that maps a name (a unit or a keyword) to a code. */
struct values_name_entry {
	const char *name;
	int code;
};

/*
 * The powers of ten a double holds exactly, 10^0 to 10^22, with which a
 * decimal exponent is applied to a mantissa with one rounding per step.
 */
static const double values_pow10[VALUES_POW10_EXACT_MAX + 1] = {
	1e0, 1e1, 1e2, 1e3, 1e4, 1e5, 1e6, 1e7, 1e8, 1e9, 1e10, 1e11,
	1e12, 1e13, 1e14, 1e15, 1e16, 1e17, 1e18, 1e19, 1e20, 1e21, 1e22
};

/*
 * The units of a length, by lower-case name (units are matched without
 * regard to ASCII case).  The percent sign is not here: it is not a name.
 *
 * Values referenced: CSS Values 3 §5.1.1 (em, ex, rem), §5.2 (px, in, cm,
 * mm, Q, pt, pc); SVG 2 §4.2 (the length syntax).
 */
static const struct values_name_entry values_length_units[] = {
	{ "px", SVG_UNIT_PX },
	{ "em", SVG_UNIT_EM },
	{ "ex", SVG_UNIT_EX },
	{ "rem", SVG_UNIT_REM },
	{ "in", SVG_UNIT_IN },
	{ "cm", SVG_UNIT_CM },
	{ "mm", SVG_UNIT_MM },
	{ "q", SVG_UNIT_Q },
	{ "pt", SVG_UNIT_PT },
	{ "pc", SVG_UNIT_PC }
};

/*
 * The units an argument of a transform function may carry, by lower-case
 * name: px for lengths, and the angle units.
 *
 * Values referenced: CSS Transforms 1 §7.3 (the numbers of the SVG
 * transform functions are CSS lengths in px and angles in deg, and the
 * attribute is the presentation attribute of the transform property, §7.1);
 * CSS Values 3 §6.1 (deg, grad, rad, turn).
 */
static const struct values_name_entry values_arg_units[] = {
	{ "px", VALUES_ARG_PX },
	{ "deg", VALUES_ARG_DEG },
	{ "grad", VALUES_ARG_GRAD },
	{ "rad", VALUES_ARG_RAD },
	{ "turn", VALUES_ARG_TURN }
};

/*
 * The alignment keywords of preserveAspectRatio and their codes.
 *
 * Values referenced: SVG 2 §8.7 (the align values; SVG 2 has no "defer").
 */
static const struct values_name_entry values_align_names[VALUES_ALIGN_COUNT] = {
	{ "none", SVG_ALIGN_NONE },
	{ "xMinYMin", SVG_ALIGN_XMIN_YMIN },
	{ "xMidYMin", SVG_ALIGN_XMID_YMIN },
	{ "xMaxYMin", SVG_ALIGN_XMAX_YMIN },
	{ "xMinYMid", SVG_ALIGN_XMIN_YMID },
	{ "xMidYMid", SVG_ALIGN_XMID_YMID },
	{ "xMaxYMid", SVG_ALIGN_XMAX_YMID },
	{ "xMinYMax", SVG_ALIGN_XMIN_YMAX },
	{ "xMidYMax", SVG_ALIGN_XMID_YMAX },
	{ "xMaxYMax", SVG_ALIGN_XMAX_YMAX }
};

/* The forward declarations of the static functions. */
static int values_is_space(char c);
static int values_is_letter(char c);
static int values_is_ident_char(char c);
static char values_lower(char c);
static const char *values_skip_space(const char *p);
static int values_word_equal(const char *text, size_t length, const char *keyword, int ignore_case);
static size_t values_word_length(const char *p);
static size_t values_read_unit_name(const char **cursor, char name[VALUES_UNIT_NAME_MAX + 1]);
static int values_find_name(const struct values_name_entry *table, size_t entries, const char *name);
static int values_read_exponent(const char **cursor, long *exponent);
static int values_apply_decimal_exponent(uint64_t mantissa, long exponent, double *magnitude);
static int values_read_length(const char **cursor, struct svg_length *length);
static int values_parse_list(const char *text, int with_units, float *numbers, struct svg_length *lengths, size_t capacity, size_t *count);
static int values_read_separator(const char **cursor, int *comma);
static int values_read_arg(const char **cursor, struct values_arg *arg);
static int values_arg_length(const struct values_arg *arg, float *length);
static int values_arg_angle(const struct values_arg *arg, float *degrees);
static int values_read_function(const char **cursor, struct vec_matrix *matrix);
static int values_function_matrix(const char *name, size_t name_length, const struct values_arg *args, size_t count, struct vec_matrix *matrix);
static int values_make_translate(const struct values_arg *args, size_t count, struct vec_matrix *matrix);
static int values_make_scale(const struct values_arg *args, size_t count, struct vec_matrix *matrix);
static int values_make_rotate(const struct values_arg *args, size_t count, struct vec_matrix *matrix);
static int values_make_skew(const struct values_arg *args, size_t count, int along_y, struct vec_matrix *matrix);
static int values_make_matrix(const struct values_arg *args, size_t count, struct vec_matrix *matrix);
static int values_matrix_finite(const struct vec_matrix *matrix);
static enum values_place values_place_x(enum svg_align align);
static enum values_place values_place_y(enum svg_align align);
static int values_read_plain_paint(const struct svg_tree *tree, const char *text, size_t length, enum svg_paint_kind *kind, uint32_t *color);

/*
 * Reads one number at the cursor.
 *
 * The digits are gathered into an integer mantissa and a decimal exponent,
 * so that no C library conversion (whose answer depends on the locale)
 * is involved; the value is mantissa x 10^exponent, rounded once in double
 * precision and once to float.  The point of a number needs a digit after
 * it (the fractional-constant of SVG 2 §9.3.9 and the <number> of CSS
 * Values 3 §4.3), so "5." is the number 5 followed by a point.
 */
int
svg_number_read(
	const char **cursor,
	float *value)
{
	const char *p;
	uint64_t mantissa;
	long decimal_exponent;
	long written_exponent;
	int significant;
	int integer_digits;
	int fraction_digits;
	int negative;
	int has_exponent;
	int digit;
	int error;
	double magnitude;

	/* Refuses missing arguments, so that a NULL is never read. */
	if (cursor == NULL || *cursor == NULL || value == NULL)
		return EINVAL;

	/* Starts at the cursor with an empty mantissa. */
	p = *cursor;
	mantissa = 0;
	decimal_exponent = 0;
	significant = 0;
	integer_digits = 0;
	fraction_digits = 0;
	negative = 0;

	/* Reads the optional sign. */
	if (*p == '+') {
		p++;
	} else if (*p == '-') {
		negative = 1;
		p++;
	}

	/*
	 * Reads the digits before the point.  A zero in front of the first
	 * non-zero digit adds nothing; a digit past the mantissa's room only
	 * raises the decimal exponent.
	 */
	while (*p >= '0' && *p <= '9') {
		digit = *p - '0';
		integer_digits++;
		if (significant == 0 && digit == 0) {
			/* A leading zero does not change the value. */
		} else if (significant < VALUES_MANTISSA_DIGITS_MAX) {
			mantissa = mantissa * 10 + (uint64_t)digit;
			significant++;
		} else {
			decimal_exponent++;
		}
		p++;
	}

	/*
	 * Reads the fraction: a point and at least one digit.  A point with no
	 * digit after it is not part of the number.  Each digit that is kept
	 * moves the decimal point one place to the left.
	 */
	if (*p == '.' && p[1] >= '0' && p[1] <= '9') {
		p++;
		while (*p >= '0' && *p <= '9') {
			digit = *p - '0';
			fraction_digits++;
			if (significant == 0 && digit == 0) {
				/* A zero before the first significant digit still moves the point. */
				decimal_exponent--;
			} else if (significant < VALUES_MANTISSA_DIGITS_MAX) {
				mantissa = mantissa * 10 + (uint64_t)digit;
				significant++;
				decimal_exponent--;
			} else {
				/* Past the precision of a float: there is nothing to keep. */
			}
			p++;
		}
	}

	/* Refuses text with no digit in either part. */
	if (integer_digits == 0 && fraction_digits == 0)
		return EINVAL;

	/* Reads the optional exponent; an e with no digits is left unread. */
	has_exponent = values_read_exponent(&p, &written_exponent);
	if (has_exponent)
		decimal_exponent += written_exponent;

	/* Scales the mantissa by the decimal exponent, refusing what a float cannot hold. */
	error = values_apply_decimal_exponent(mantissa, decimal_exponent, &magnitude);
	if (error != 0)
		return error;

	/* Publishes the value and moves the cursor past the number. */
	if (negative)
		magnitude = -magnitude;
	*value = (float)magnitude;
	*cursor = p;

	/* Succeeded: one number was read. */
	return 0;
}

/*
 * Parses a whole attribute as one number.
 *
 * White space may surround the number; anything else after it is a
 * malformed value.
 */
int
svg_parse_number(
	const char *text,
	float *value)
{
	const char *p;
	float number;
	int error;

	/* Refuses missing arguments. */
	if (text == NULL || value == NULL)
		return EINVAL;

	/* Reads the number after any leading white space. */
	p = values_skip_space(text);
	error = svg_number_read(&p, &number);
	if (error != 0)
		return error;

	/* Refuses anything but white space after the number. */
	p = values_skip_space(p);
	if (*p != '\0')
		return EINVAL;

	/* Publishes the number. */
	*value = number;

	/* Succeeded: the attribute is one number. */
	return 0;
}

/*
 * Parses a whole attribute as a length.
 *
 * White space may surround the length; the unit follows the number
 * without a gap.
 */
int
svg_parse_length(
	const char *text,
	struct svg_length *length)
{
	const char *p;
	struct svg_length parsed;
	int error;

	/* Refuses missing arguments. */
	if (text == NULL || length == NULL)
		return EINVAL;

	/* Reads the number and its unit after any leading white space. */
	p = values_skip_space(text);
	error = values_read_length(&p, &parsed);
	if (error != 0)
		return error;

	/* Refuses anything but white space after the length. */
	p = values_skip_space(p);
	if (*p != '\0')
		return EINVAL;

	/* Publishes the length. */
	*length = parsed;

	/* Succeeded: the attribute is one length. */
	return 0;
}

/*
 * Resolves a length to user units.
 *
 * Values referenced: CSS Values 3 §5.1.1 and §5.2 (the units), SVG 2 §8.9
 * (a percentage is of the viewport's width, height or normalized
 * diagonal).  The arithmetic is done in double precision and a result
 * that a float cannot hold gives 0.
 */
float
svg_length_resolve(
	const struct svg_length *length,
	enum svg_axis axis,
	const struct svg_context *context)
{
	double number;
	double result;
	double width;
	double height;
	double diagonal;

	/* Gives 0 for missing arguments. */
	if (length == NULL || context == NULL)
		return 0.0f;

	/* Takes the value to resolve. */
	number = (double)length->value;

	/* Multiplies the number by what its unit stands for in user units. */
	switch (length->unit) {
	case SVG_UNIT_NUMBER:
	case SVG_UNIT_PX:
		/* User units and px are the same size. */
		result = number;
		break;
	case SVG_UNIT_PERCENT:
		/* A percentage of the viewport's width, its height or the normalized diagonal. */
		width = (double)context->viewport_width;
		height = (double)context->viewport_height;
		if (axis == SVG_AXIS_X) {
			result = number / 100.0 * width;
		} else if (axis == SVG_AXIS_Y) {
			result = number / 100.0 * height;
		} else {
			diagonal = sqrt((width * width + height * height) / 2.0);
			result = number / 100.0 * diagonal;
		}
		break;
	case SVG_UNIT_EM:
		/* The element's font size. */
		result = number * (double)context->font_size;
		break;
	case SVG_UNIT_EX:
		/* Half the font size, the fallback of CSS Values 3 §5.1.1. */
		result = number * (double)context->font_size / 2.0;
		break;
	case SVG_UNIT_REM:
		/* The root element's font size. */
		result = number * (double)context->root_font_size;
		break;
	case SVG_UNIT_IN:
		result = number * (double)SVG_PX_PER_IN;
		break;
	case SVG_UNIT_CM:
		result = number * (double)SVG_PX_PER_CM;
		break;
	case SVG_UNIT_MM:
		result = number * (double)SVG_PX_PER_MM;
		break;
	case SVG_UNIT_Q:
		result = number * (double)SVG_PX_PER_Q;
		break;
	case SVG_UNIT_PT:
		result = number * (double)SVG_PX_PER_PT;
		break;
	case SVG_UNIT_PC:
		result = number * (double)SVG_PX_PER_PC;
		break;
	default:
		/* A unit code that no parser produces has no size. */
		return 0.0f;
	}

	/* Gives 0 for a result that is not finite or that a float cannot hold. */
	if (result != result)
		return 0.0f;
	if (result > (double)FLT_MAX || result < -(double)FLT_MAX)
		return 0.0f;

	/* Reports the length in user units. */
	return (float)result;
}

/*
 * Parses a list of numbers.
 *
 * The list reader is shared with the list of lengths.
 */
int
svg_parse_numbers(
	const char *text,
	float *values,
	size_t capacity,
	size_t *count)
{
	int error;

	/* Reads the list as numbers. */
	error = values_parse_list(text, 0, values, NULL, capacity, count);
	if (error != 0)
		return error;

	/* Succeeded: *count numbers are in values. */
	return 0;
}

/*
 * Parses a list of lengths, or the keyword none.
 *
 * A malformed list keeps nothing; a list that is merely too long keeps
 * the lengths that fit.
 */
int
svg_parse_lengths(
	const char *text,
	struct svg_length *values,
	size_t capacity,
	size_t *count)
{
	const char *p;
	size_t word_length;
	int is_none;
	int error;

	/* Refuses missing arguments. */
	if (text == NULL || count == NULL)
		return EINVAL;

	/* Accepts the keyword none (ASCII case-insensitive) as an empty list. */
	p = values_skip_space(text);
	word_length = values_word_length(p);
	is_none = values_word_equal(p, word_length, "none", 1);
	if (is_none) {
		*count = 0;
		p = values_skip_space(p + word_length);
		if (*p != '\0')
			return EINVAL;
		return 0;
	}

	/* Reads the list as lengths. */
	error = values_parse_list(text, 1, NULL, values, capacity, count);

	/* A malformed list keeps nothing. */
	if (error == EINVAL)
		*count = 0;

	/* Reports why the list was not kept. */
	if (error != 0)
		return error;

	/* Succeeded: *count lengths are in values. */
	return 0;
}

/*
 * Parses the transform attribute.
 *
 * The functions are read from left to right; each is composed to the
 * right of those before it (M1 x M2 x ...), so the leftmost applies last
 * to a point.  The result replaces *matrix only when the whole attribute
 * is valid.  A function that follows another needs white space or a comma
 * between them (the comma-wsp of CSS Transforms 1 §7.2).
 */
int
svg_parse_transform(
	const char *text,
	struct vec_matrix *matrix)
{
	const char *p;
	struct vec_matrix total;
	struct vec_matrix function_matrix;
	size_t count;
	int comma;
	int separated;
	int finite;
	int error;

	/* Refuses missing arguments. */
	if (text == NULL || matrix == NULL)
		return EINVAL;

	/* Starts from the identity, which an empty attribute leaves as it is. */
	vec_matrix_identity(&total);
	count = 0;
	p = values_skip_space(text);

	/* Reads the transform functions one after the other. */
	while (*p != '\0') {
		/* Reads one function into its matrix. */
		error = values_read_function(&p, &function_matrix);
		if (error != 0)
			return error;

		/* Refuses a list of more than SVG_TRANSFORMS_MAX functions. */
		if (count >= SVG_TRANSFORMS_MAX)
			return ENOBUFS;
		count++;

		/* Composes the function to the right of the ones before it: total = total x function. */
		vec_matrix_multiply(&total, &function_matrix, &total);

		/* Refuses a product that overflowed. */
		finite = values_matrix_finite(&total);
		if (!finite)
			return EINVAL;

		/* Reads the white space and the one comma that separate two functions. */
		separated = values_read_separator(&p, &comma);

		/* A list does not end with a comma. */
		if (*p == '\0' && comma)
			return EINVAL;

		/* Two functions need a separator between them. */
		if (*p != '\0' && !separated)
			return EINVAL;
	}

	/* Publishes the composed transform. */
	*matrix = total;

	/* Succeeded: *matrix is the transform of the attribute. */
	return 0;
}

/*
 * Parses the viewBox attribute.
 *
 * Reads at most five numbers, so that a fifth is seen and refused without
 * a large buffer.
 */
int
svg_parse_view_box(
	const char *text,
	struct vec_rect *view_box)
{
	float numbers[5];
	struct vec_rect parsed;
	size_t count;
	int finite;
	int error;

	/* Refuses missing arguments. */
	if (text == NULL || view_box == NULL)
		return EINVAL;

	/* Reads the numbers; a malformed list or a fifth number is not a viewBox. */
	count = 0;
	error = svg_parse_numbers(text, numbers, 5, &count);
	if (error != 0)
		return EINVAL;

	/* Refuses anything but four numbers. */
	if (count != 4)
		return EINVAL;

	/* Refuses a negative width or height (SVG 2 §8.6); zero is allowed. */
	if (numbers[2] < 0.0f || numbers[3] < 0.0f)
		return EINVAL;

	/* Turns min-x, min-y, width and height into the edges of the rectangle. */
	parsed.left = numbers[0];
	parsed.top = numbers[1];
	parsed.right = numbers[0] + numbers[2];
	parsed.bottom = numbers[1] + numbers[3];

	/* Refuses an edge that overflowed. */
	finite = vec_finite(parsed.right);
	if (!finite)
		return EINVAL;
	finite = vec_finite(parsed.bottom);
	if (!finite)
		return EINVAL;

	/* Publishes the rectangle. */
	*view_box = parsed;

	/* Succeeded: *view_box is the viewBox. */
	return 0;
}

/*
 * Parses the preserveAspectRatio attribute.
 *
 * The keywords are case-sensitive and separated by white space.
 */
int
svg_parse_aspect(
	const char *text,
	struct svg_aspect *aspect)
{
	const char *p;
	struct svg_aspect parsed;
	size_t word_length;
	size_t index;
	int found;
	int matched;

	/* Refuses missing arguments. */
	if (text == NULL || aspect == NULL)
		return EINVAL;

	/* Reads the alignment keyword after any leading white space. */
	p = values_skip_space(text);
	word_length = values_word_length(p);
	found = 0;
	parsed.align = SVG_ALIGN_NONE;
	parsed.slice = 0;

	/* Looks the keyword up among the ten alignments, matching the case exactly. */
	for (index = 0; index < VALUES_ALIGN_COUNT; index++) {
		matched = values_word_equal(p, word_length, values_align_names[index].name, 0);
		if (matched) {
			parsed.align = (enum svg_align)values_align_names[index].code;
			found = 1;
			break;
		}
	}

	/* Refuses a word that is not an alignment. */
	if (!found)
		return EINVAL;
	p += word_length;

	/*
	 * Reads the optional meet or slice.  The alignment keyword took every
	 * letter, so what follows it is set apart by white space or is not a
	 * keyword at all.
	 */
	p = values_skip_space(p);
	if (*p != '\0') {
		/* Reads the keyword that says whether to fit inside (meet) or to cover (slice). */
		word_length = values_word_length(p);
		matched = values_word_equal(p, word_length, "meet", 0);
		if (matched) {
			parsed.slice = 0;
		} else {
			matched = values_word_equal(p, word_length, "slice", 0);
			if (!matched)
				return EINVAL;
			parsed.slice = 1;
		}
		p = values_skip_space(p + word_length);
	}

	/* Refuses anything left after the keywords. */
	if (*p != '\0')
		return EINVAL;

	/* Publishes the aspect. */
	*aspect = parsed;

	/* Succeeded: *aspect is the preserveAspectRatio. */
	return 0;
}

/*
 * Computes the transform of a viewBox into a viewport.
 *
 * Values referenced: SVG 2 §8.2 (computing the equivalent transform of an
 * SVG viewport).  The algorithm is followed step by step in double
 * precision; the answer is translate(tx, ty) scale(sx, sy), that is
 * a = sx, d = sy, e = tx, f = ty.
 */
int
svg_view_box_transform(
	const struct vec_rect *view_box,
	const struct svg_aspect *aspect,
	float x,
	float y,
	float width,
	float height,
	struct vec_matrix *matrix)
{
	struct vec_matrix result;
	double vb_x;
	double vb_y;
	double vb_width;
	double vb_height;
	double scale_x;
	double scale_y;
	double translate_x;
	double translate_y;
	enum values_place place;
	enum svg_align align;
	int slice;
	int finite;

	/* Refuses missing arguments. */
	if (view_box == NULL || aspect == NULL || matrix == NULL)
		return EINVAL;

	/* Refuses a viewBox, position or size that is not finite. */
	finite = vec_finite(view_box->left);
	if (!finite)
		return EINVAL;
	finite = vec_finite(view_box->top);
	if (!finite)
		return EINVAL;
	finite = vec_finite(view_box->right);
	if (!finite)
		return EINVAL;
	finite = vec_finite(view_box->bottom);
	if (!finite)
		return EINVAL;
	finite = vec_finite(x);
	if (!finite)
		return EINVAL;
	finite = vec_finite(y);
	if (!finite)
		return EINVAL;
	finite = vec_finite(width);
	if (!finite)
		return EINVAL;
	finite = vec_finite(height);
	if (!finite)
		return EINVAL;

	/* Takes vb-x, vb-y, vb-width and vb-height from the rectangle, in double precision. */
	vb_x = (double)view_box->left;
	vb_y = (double)view_box->top;
	vb_width = (double)view_box->right - (double)view_box->left;
	vb_height = (double)view_box->bottom - (double)view_box->top;

	/* Refuses a viewBox without a positive size, and a viewport with a negative one. */
	if (vb_width <= 0.0 || vb_height <= 0.0)
		return EINVAL;
	if (width < 0.0f || height < 0.0f)
		return EINVAL;

	/* Takes the alignment and whether to slice. */
	align = aspect->align;
	slice = aspect->slice;

	/* Initializes scale-x and scale-y to the ratios of the viewport to the viewBox. */
	scale_x = (double)width / vb_width;
	scale_y = (double)height / vb_height;

	/* Makes the scale uniform unless the alignment is none: the smaller one to fit (meet), the larger to cover (slice). */
	if (align != SVG_ALIGN_NONE && !slice) {
		if (scale_x > scale_y)
			scale_x = scale_y;
		else
			scale_y = scale_x;
	} else if (align != SVG_ALIGN_NONE && slice) {
		if (scale_x < scale_y)
			scale_x = scale_y;
		else
			scale_y = scale_x;
	}

	/* Initializes the translation to put the viewBox's origin at the viewport's. */
	translate_x = (double)x - vb_x * scale_x;
	translate_y = (double)y - vb_y * scale_y;

	/* Moves the viewBox to the middle or the far edge of the viewport along x. */
	place = values_place_x(align);
	if (place == VALUES_PLACE_MID)
		translate_x += ((double)width - vb_width * scale_x) / 2.0;
	else if (place == VALUES_PLACE_MAX)
		translate_x += (double)width - vb_width * scale_x;

	/* Moves the viewBox to the middle or the far edge of the viewport along y. */
	place = values_place_y(align);
	if (place == VALUES_PLACE_MID)
		translate_y += ((double)height - vb_height * scale_y) / 2.0;
	else if (place == VALUES_PLACE_MAX)
		translate_y += (double)height - vb_height * scale_y;

	/* Refuses a result that a float cannot hold. */
	if (scale_x != scale_x || scale_y != scale_y || translate_x != translate_x || translate_y != translate_y)
		return EINVAL;
	if (scale_x > (double)FLT_MAX || scale_y > (double)FLT_MAX)
		return EINVAL;
	if (translate_x > (double)FLT_MAX || translate_x < -(double)FLT_MAX)
		return EINVAL;
	if (translate_y > (double)FLT_MAX || translate_y < -(double)FLT_MAX)
		return EINVAL;

	/* Writes translate(tx, ty) scale(sx, sy) as one matrix. */
	result.a = (float)scale_x;
	result.b = 0.0f;
	result.c = 0.0f;
	result.d = (float)scale_y;
	result.e = (float)translate_x;
	result.f = (float)translate_y;

	/* Publishes the matrix. */
	*matrix = result;

	/* Succeeded: *matrix maps viewBox units to the viewport. */
	return 0;
}

/*
 * Reads an url reference to a local element.
 *
 * White space before the reference is skipped.  The id ends at the closing
 * quote, at white space or at the closing parenthesis, as the form says.
 * A reference to anything but a fragment of the same document (the first
 * character after the opening is not a number sign) is refused with
 * ENOTSUP before the rest is looked at.
 */
int
svg_url_read(
	const char **cursor,
	int bare,
	char id[SVG_ID_MAX + 1])
{
	const char *p;
	const char *id_start;
	size_t id_length;
	char quote;
	int is_url;
	int ended;
	int space;

	/* Refuses missing arguments. */
	if (cursor == NULL || *cursor == NULL || id == NULL)
		return EINVAL;

	/* Skips the white space before the reference. */
	p = values_skip_space(*cursor);
	quote = '\0';

	/* Reads the url( and the opening quote, if the reference is not a bare #id. */
	if (!bare) {
		is_url = values_word_equal(p, 3, "url", 1);
		if (!is_url)
			return EINVAL;
		if (p[3] != '(')
			return EINVAL;
		p = values_skip_space(p + 4);
		if (*p == '"' || *p == '\'') {
			quote = *p;
			p++;
		}
	}

	/* Refuses an empty reference. */
	if (*p == '\0' || *p == ')')
		return EINVAL;
	if (quote != '\0' && *p == quote)
		return EINVAL;

	/* Refuses a reference to another document, which this renderer does not load. */
	if (*p != '#')
		return ENOTSUP;
	p++;

	/* Finds the end of the id: the closing quote, or white space, or the closing parenthesis. */
	id_start = p;
	ended = 0;
	while (*p != '\0' && !ended) {
		if (quote != '\0') {
			if (*p == quote)
				ended = 1;
		} else {
			space = values_is_space(*p);
			if (space)
				ended = 1;
			else if (!bare && *p == ')')
				ended = 1;
		}
		if (!ended)
			p++;
	}
	id_length = (size_t)(p - id_start);

	/* Refuses an empty id. */
	if (id_length == 0)
		return EINVAL;

	/* Reads the closing quote, which must be there. */
	if (quote != '\0') {
		if (*p != quote)
			return EINVAL;
		p++;
	}

	/* Reads the closing parenthesis after any white space. */
	if (!bare) {
		p = values_skip_space(p);
		if (*p != ')')
			return EINVAL;
		p++;
	}

	/* Refuses an id longer than the buffer. */
	if (id_length > SVG_ID_MAX)
		return ENOBUFS;

	/* Copies the id and ends it, then moves the cursor past the reference. */
	memcpy(id, id_start, id_length);
	id[id_length] = '\0';
	*cursor = p;

	/* Succeeded: id names the element. */
	return 0;
}

/*
 * Parses a paint.
 *
 * Values referenced: SVG 2 §13.2 (the fill and stroke value syntax:
 * none, <color>, <url> [none | <color>]?, context-fill, context-stroke;
 * currentColor is a CSS color).
 */
int
svg_parse_paint(
	const struct svg_tree *tree,
	const char *text,
	struct svg_paint *paint)
{
	struct svg_paint parsed;
	const char *start;
	const char *end;
	const char *p;
	size_t length;
	size_t rest_length;
	enum svg_paint_kind kind;
	uint32_t color;
	int is_url;
	int space;
	int error;

	/* Refuses missing arguments. */
	if (text == NULL || paint == NULL)
		return EINVAL;

	/* Finds the text of the value between its leading and trailing white space. */
	start = values_skip_space(text);
	end = start + strlen(start);
	while (end > start) {
		space = values_is_space(end[-1]);
		if (!space)
			break;
		end--;
	}
	length = (size_t)(end - start);

	/* Refuses an empty value. */
	if (length == 0)
		return EINVAL;

	/* Starts from an empty paint, whose fallback is the none of a paint with no reference. */
	memset(&parsed, 0, sizeof(parsed));
	parsed.kind = SVG_PAINT_NONE;
	parsed.fallback_kind = SVG_PAINT_NONE;

	/* Tells a reference to a paint server (url followed by a parenthesis) from the other forms. */
	is_url = 0;
	if (length >= 4) {
		is_url = values_word_equal(start, 3, "url", 1);
		if (is_url && start[3] != '(')
			is_url = 0;
	}
	if (is_url) {
		/* Reads the reference; another document is not supported, any other fault is invalid. */
		p = start;
		error = svg_url_read(&p, 0, parsed.id);
		if (error == ENOTSUP)
			return ENOTSUP;
		if (error != 0)
			return EINVAL;
		parsed.kind = SVG_PAINT_URL;
		parsed.fallback_kind = SVG_PAINT_URL;

		/* Reads the fallback, which is what the paint is when the reference fails. */
		p = values_skip_space(p);
		if (p < end) {
			rest_length = (size_t)(end - p);
			error = values_read_plain_paint(tree, p, rest_length, &kind, &color);
			if (error != 0)
				return EINVAL;
			parsed.fallback_kind = kind;
			parsed.fallback_color = color;
		}
	} else {
		/* Reads none, currentColor or a color. */
		error = values_read_plain_paint(tree, start, length, &kind, &color);
		if (error != 0)
			return error;
		parsed.kind = kind;
		parsed.color = color;
	}

	/* Publishes the paint. */
	*paint = parsed;

	/* Succeeded: *paint is the value. */
	return 0;
}

/* Tells whether a character is one of the white space characters SVG allows. */
static int
values_is_space(
	char c)
{
	/* Space, tab, line feed, form feed and carriage return are white space. */
	if (c == ' ' || c == '\t' || c == '\n' || c == '\f' || c == '\r')
		return 1;

	/* Succeeded: anything else is not white space. */
	return 0;
}

/* Tells whether a character is an ASCII letter. */
static int
values_is_letter(
	char c)
{
	/* Lower case letters. */
	if (c >= 'a' && c <= 'z')
		return 1;

	/* Upper case letters. */
	if (c >= 'A' && c <= 'Z')
		return 1;

	/* Succeeded: anything else is not a letter. */
	return 0;
}

/*
 * Tells whether a character can continue a CSS identifier: a letter, a
 * digit, a hyphen, an underscore or a byte of a non-ASCII character.
 */
static int
values_is_ident_char(
	char c)
{
	int letter;

	/* Letters continue an identifier. */
	letter = values_is_letter(c);
	if (letter)
		return 1;

	/* So do digits, hyphens and underscores. */
	if (c >= '0' && c <= '9')
		return 1;
	if (c == '-' || c == '_')
		return 1;

	/* So do the bytes of a non-ASCII character. */
	if ((unsigned char)c >= 0x80)
		return 1;

	/* Succeeded: anything else ends the identifier. */
	return 0;
}

/* Gives the ASCII lower case of a character, which other characters keep. */
static char
values_lower(
	char c)
{
	/* Moves an upper case letter to its lower case. */
	if (c >= 'A' && c <= 'Z')
		return (char)(c - 'A' + 'a');

	/* Succeeded: the character is not an upper case letter. */
	return c;
}

/* Skips the white space at a position, stopping at the first other character or the NUL. */
static const char *
values_skip_space(
	const char *p)
{
	int space;

	/* Moves past every white space character. */
	space = values_is_space(*p);
	while (space) {
		p++;
		space = values_is_space(*p);
	}

	/* Reports the position of the first character that is not white space. */
	return p;
}

/*
 * Tells whether the length bytes at text are the keyword.
 *
 * The text is never read beyond length bytes, nor beyond a NUL (a NUL
 * differs from every letter of a keyword).  With ignore_case the letters
 * match without regard to ASCII case (the keyword is given in lower case
 * then).
 */
static int
values_word_equal(
	const char *text,
	size_t length,
	const char *keyword,
	int ignore_case)
{
	size_t index;
	char actual;
	char expected;

	/* Refuses a word of another length. */
	if (strlen(keyword) != length)
		return 0;

	/* Compares the letters one by one, stopping at the first difference. */
	for (index = 0; index < length; index++) {
		actual = text[index];
		expected = keyword[index];
		if (ignore_case)
			actual = values_lower(actual);
		if (actual != expected)
			return 0;
	}

	/* Succeeded: the word is the keyword. */
	return 1;
}

/* Counts the ASCII letters at a position (the length of a keyword that starts there). */
static size_t
values_word_length(
	const char *p)
{
	size_t length;
	int letter;

	/* Counts the letters up to the first character that is not one. */
	length = 0;
	letter = values_is_letter(p[0]);
	while (letter) {
		length++;
		letter = values_is_letter(p[length]);
	}

	/* Reports the count. */
	return length;
}

/*
 * Reads the name of a unit that follows a number: a letter and then the
 * characters that continue a CSS identifier (so that "10px5" has the unit
 * "px5", which is unknown, rather than the unit px and a number).
 *
 * The cursor moves past the whole name.  The name is copied in lower
 * case up to VALUES_UNIT_NAME_MAX bytes and ended with a NUL; the length
 * of the whole name is reported (0 for no name), so a caller refuses a
 * name longer than any unit.
 */
static size_t
values_read_unit_name(
	const char **cursor,
	char name[VALUES_UNIT_NAME_MAX + 1])
{
	const char *p;
	size_t length;
	int letter;
	int more;

	/* Starts with an empty name. */
	p = *cursor;
	length = 0;
	name[0] = '\0';

	/* A name starts with a letter; anything else means the number has no unit. */
	letter = values_is_letter(*p);
	if (!letter)
		return 0;

	/* Reads the characters that belong to the identifier, keeping the first ones. */
	more = 1;
	while (more) {
		if (length < VALUES_UNIT_NAME_MAX) {
			name[length] = values_lower(*p);
			name[length + 1] = '\0';
		}
		length++;
		p++;
		more = values_is_ident_char(*p);
	}

	/* Moves the cursor past the name. */
	*cursor = p;

	/* Reports the length of the whole name. */
	return length;
}

/*
 * Looks a name up in a table.
 *
 * Reports the code of the entry, or -1 when the name is not in the table.
 */
static int
values_find_name(
	const struct values_name_entry *table,
	size_t entries,
	const char *name)
{
	size_t index;
	int different;

	/* Compares the name with each entry. */
	for (index = 0; index < entries; index++) {
		different = strcmp(table[index].name, name);
		if (different == 0)
			return table[index].code;
	}

	/* The name is in no entry. */
	return -1;
}

/*
 * Reads an exponent: e or E, an optional sign and at least one digit.
 *
 * When the text there is not that, nothing is read and 0 is reported (the
 * e then belongs to what follows, such as the unit em).  The magnitude is
 * clamped to VALUES_EXPONENT_CLAMP.
 */
static int
values_read_exponent(
	const char **cursor,
	long *exponent)
{
	const char *p;
	long magnitude;
	int negative;

	/* Needs an e or E. */
	p = *cursor;
	if (*p != 'e' && *p != 'E')
		return 0;
	p++;

	/* Reads the optional sign. */
	negative = 0;
	if (*p == '+') {
		p++;
	} else if (*p == '-') {
		negative = 1;
		p++;
	}

	/* Needs at least one digit; without one the e is not an exponent. */
	if (*p < '0' || *p > '9')
		return 0;

	/* Reads the digits, keeping the magnitude within the clamp. */
	magnitude = 0;
	while (*p >= '0' && *p <= '9') {
		if (magnitude < VALUES_EXPONENT_CLAMP)
			magnitude = magnitude * 10 + (long)(*p - '0');
		p++;
	}
	if (magnitude > VALUES_EXPONENT_CLAMP)
		magnitude = VALUES_EXPONENT_CLAMP;

	/* Publishes the exponent and moves the cursor past it. */
	if (negative)
		magnitude = -magnitude;
	*exponent = magnitude;
	*cursor = p;

	/* Succeeded: an exponent was read. */
	return 1;
}

/*
 * Computes mantissa x 10^exponent in double precision.
 *
 * Powers of ten are applied up to 10^22 at a time, which a double holds
 * exactly.  Errors: EINVAL when the magnitude is beyond the largest float
 * (the number overflows).  A magnitude under the smallest float is 0.
 */
static int
values_apply_decimal_exponent(
	uint64_t mantissa,
	long exponent,
	double *magnitude)
{
	double result;
	long remaining;
	long step;

	/* Zero stays zero whatever the exponent. */
	if (mantissa == 0) {
		*magnitude = 0.0;
		return 0;
	}

	/* An exponent beyond the limit decides the outcome without scaling. */
	if (exponent > VALUES_DECIMAL_LIMIT)
		return EINVAL;
	if (exponent < -VALUES_DECIMAL_LIMIT) {
		*magnitude = 0.0;
		return 0;
	}

	/* Scales up, refusing as soon as the value is beyond the largest float. */
	result = (double)mantissa;
	remaining = exponent;
	while (remaining > 0) {
		step = remaining;
		if (step > VALUES_POW10_EXACT_MAX)
			step = VALUES_POW10_EXACT_MAX;
		result *= values_pow10[step];
		remaining -= step;
		if (result >= VALUES_FLOAT_OVERFLOW)
			return EINVAL;
	}

	/* Scales down. */
	remaining = -exponent;
	while (remaining > 0) {
		step = remaining;
		if (step > VALUES_POW10_EXACT_MAX)
			step = VALUES_POW10_EXACT_MAX;
		result /= values_pow10[step];
		remaining -= step;
	}

	/*
	 * Holds a magnitude just above the largest float at it, so the
	 * conversion to float stays defined.  (A magnitude that rounds to
	 * infinity was refused while scaling up; scaling down and a mantissa
	 * under 2^64 stay far below it.)
	 */
	if (result > (double)FLT_MAX)
		result = (double)FLT_MAX;

	/* Publishes the magnitude. */
	*magnitude = result;

	/* Succeeded: the magnitude fits a float. */
	return 0;
}

/*
 * Reads a length at the cursor: a number, then a unit (or a percent sign)
 * that follows it without a gap.
 *
 * Errors: EINVAL for no number or an unknown unit; *cursor and *length are
 * unchanged then.
 */
static int
values_read_length(
	const char **cursor,
	struct svg_length *length)
{
	const char *p;
	char name[VALUES_UNIT_NAME_MAX + 1];
	size_t name_length;
	size_t entries;
	float number;
	int unit;
	int error;

	/* Reads the number. */
	p = *cursor;
	error = svg_number_read(&p, &number);
	if (error != 0)
		return EINVAL;

	/* Reads the unit that follows the number, if any. */
	unit = SVG_UNIT_NUMBER;
	if (*p == '%') {
		/* A percentage. */
		unit = SVG_UNIT_PERCENT;
		p++;
	} else {
		/* A unit name, which must be one of the table; a name longer than any unit is unknown. */
		name_length = values_read_unit_name(&p, name);
		if (name_length > VALUES_UNIT_NAME_MAX)
			return EINVAL;
		if (name_length != 0) {
			entries = sizeof(values_length_units) / sizeof(values_length_units[0]);
			unit = values_find_name(values_length_units, entries, name);
			if (unit < 0)
				return EINVAL;
		}
	}

	/* Publishes the length and moves the cursor past it. */
	length->value = number;
	length->unit = (enum svg_unit)unit;
	*cursor = p;

	/* Succeeded: one length was read. */
	return 0;
}

/*
 * Parses a list of numbers or of lengths.
 *
 * The list is white space and commas between items: after an item, white
 * space and then at most one comma and more white space, and an item
 * follows a comma; two items need a separator between them.  An empty list
 * is valid.  On EINVAL and ENOBUFS *count is the number of items stored.
 */
static int
values_parse_list(
	const char *text,
	int with_units,
	float *numbers,
	struct svg_length *lengths,
	size_t capacity,
	size_t *count)
{
	const char *p;
	size_t stored;
	size_t limit;
	float number;
	struct svg_length length;
	int comma;
	int separated;
	int error;

	/* Refuses missing arguments. */
	if (text == NULL || count == NULL)
		return EINVAL;

	/* Keeps the capacity within the limit of a list. */
	limit = capacity;
	if (limit > SVG_NUMBERS_MAX)
		limit = SVG_NUMBERS_MAX;
	stored = 0;
	*count = 0;
	number = 0.0f;
	length.value = 0.0f;
	length.unit = SVG_UNIT_NUMBER;

	/* An empty or all-white list has no items. */
	p = values_skip_space(text);
	if (*p == '\0')
		return 0;

	/* Reads the items one after the other. */
	for (;;) {
		/* Reads one item; a stray comma or a non-number ends here as malformed. */
		if (with_units)
			error = values_read_length(&p, &length);
		else
			error = svg_number_read(&p, &number);
		if (error != 0) {
			*count = stored;
			return EINVAL;
		}

		/* Refuses an item that the capacity has no room for. */
		if (stored >= limit) {
			*count = stored;
			return ENOBUFS;
		}

		/* Stores the item. */
		if (with_units)
			lengths[stored] = length;
		else
			numbers[stored] = number;
		stored++;
		*count = stored;

		/* Reads the separator after the item. */
		separated = values_read_separator(&p, &comma);

		/* Ends the list where the text ends; a trailing comma is malformed. */
		if (*p == '\0') {
			if (comma)
				return EINVAL;
			return 0;
		}

		/* Two items with nothing between them are one malformed item. */
		if (!separated)
			return EINVAL;
	}
}

/*
 * Reads the separator between two items: white space, then at most one
 * comma, then white space (SVG 2 §4.2's comma-wsp).
 *
 * *comma is set to whether a comma was read.  Reports whether any
 * character was read.
 */
static int
values_read_separator(
	const char **cursor,
	int *comma)
{
	const char *p;
	const char *start;

	/* Reads the white space before the comma. */
	start = *cursor;
	p = values_skip_space(start);

	/* Reads the one comma and the white space after it. */
	*comma = 0;
	if (*p == ',') {
		*comma = 1;
		p++;
		p = values_skip_space(p);
	}

	/* Moves the cursor past what was read. */
	*cursor = p;

	/* Reports that something separates the items. */
	if (p != start)
		return 1;

	/* Succeeded: nothing separates the items. */
	return 0;
}

/*
 * Reads one argument of a transform function: a number, and a unit that
 * follows it without a gap.
 *
 * Errors: EINVAL for no number or a unit that is not px, deg, grad, rad or
 * turn.
 */
static int
values_read_arg(
	const char **cursor,
	struct values_arg *arg)
{
	const char *p;
	char name[VALUES_UNIT_NAME_MAX + 1];
	size_t name_length;
	size_t entries;
	float number;
	int unit;
	int error;

	/* Reads the number. */
	p = *cursor;
	error = svg_number_read(&p, &number);
	if (error != 0)
		return EINVAL;

	/* Reads the unit that follows the number, if any. */
	unit = VALUES_ARG_NUMBER;
	name_length = values_read_unit_name(&p, name);
	if (name_length > VALUES_UNIT_NAME_MAX)
		return EINVAL;
	if (name_length != 0) {
		entries = sizeof(values_arg_units) / sizeof(values_arg_units[0]);
		unit = values_find_name(values_arg_units, entries, name);
		if (unit < 0)
			return EINVAL;
	}

	/* Publishes the argument and moves the cursor past it. */
	arg->value = number;
	arg->unit = (enum values_arg_unit)unit;
	*cursor = p;

	/* Succeeded: one argument was read. */
	return 0;
}

/*
 * Takes an argument as a length in user units: a bare number or px.
 *
 * Errors: EINVAL for an angle unit.
 */
static int
values_arg_length(
	const struct values_arg *arg,
	float *length)
{
	/* Refuses an angle where a length is needed. */
	if (arg->unit != VALUES_ARG_NUMBER && arg->unit != VALUES_ARG_PX)
		return EINVAL;

	/* Publishes the length. */
	*length = arg->value;

	/* Succeeded: the argument is a length. */
	return 0;
}

/*
 * Takes an argument as an angle in degrees: a bare number (degrees) or
 * deg, grad, rad or turn.
 *
 * Errors: EINVAL for px, or for an angle that overflows.
 */
static int
values_arg_angle(
	const struct values_arg *arg,
	float *degrees)
{
	float result;
	int finite;

	/* Converts the number by its unit (CSS Values 3 §6.1). */
	switch (arg->unit) {
	case VALUES_ARG_NUMBER:
	case VALUES_ARG_DEG:
		result = arg->value;
		break;
	case VALUES_ARG_GRAD:
		result = arg->value * VALUES_DEGREES_PER_GRAD;
		break;
	case VALUES_ARG_RAD:
		result = arg->value * VALUES_DEGREES_PER_RADIAN;
		break;
	case VALUES_ARG_TURN:
		result = arg->value * VALUES_DEGREES_PER_TURN;
		break;
	default:
		/* A length is not an angle. */
		return EINVAL;
	}

	/* Refuses an angle that overflowed. */
	finite = vec_finite(result);
	if (!finite)
		return EINVAL;

	/* Publishes the angle. */
	*degrees = result;

	/* Succeeded: the argument is an angle. */
	return 0;
}

/*
 * Reads one transform function: its name, an opening parenthesis, its
 * arguments and the closing parenthesis, and makes its matrix.
 *
 * Errors: EINVAL for an unknown name, a wrong number of arguments, a
 * malformed argument, a missing parenthesis.  *cursor is moved only on
 * success.
 */
static int
values_read_function(
	const char **cursor,
	struct vec_matrix *matrix)
{
	const char *p;
	struct values_arg args[VALUES_ARGS_MAX];
	size_t name_length;
	size_t count;
	int done;
	int error;

	/* Reads the name of the function. */
	p = *cursor;
	name_length = values_word_length(p);
	if (name_length == 0)
		return EINVAL;
	p += name_length;

	/* Reads the opening parenthesis, which may be set apart from the name. */
	p = values_skip_space(p);
	if (*p != '(')
		return EINVAL;
	p++;
	p = values_skip_space(p);

	/*
	 * Reads the arguments.  A comma may follow an argument and then an
	 * argument must follow; with no comma the next argument starts at
	 * once (the comma-wsp of the grammar is optional here), and the list
	 * ends at the closing parenthesis.
	 */
	count = 0;
	done = 0;
	while (!done) {
		/* Refuses more arguments than any function takes. */
		if (count >= VALUES_ARGS_MAX)
			return EINVAL;

		/* Reads the argument. */
		error = values_read_arg(&p, &args[count]);
		if (error != 0)
			return error;
		count++;

		/* Reads the separator after it, or the end of the list. */
		p = values_skip_space(p);
		if (*p == ')') {
			done = 1;
		} else if (*p == ',') {
			p++;
			p = values_skip_space(p);
		}
	}

	/* Moves past the closing parenthesis. */
	p++;

	/* Makes the matrix from the name and the arguments. */
	error = values_function_matrix(*cursor, name_length, args, count, matrix);
	if (error != 0)
		return error;

	/* Moves the cursor past the function. */
	*cursor = p;

	/* Succeeded: one function was read. */
	return 0;
}

/*
 * Makes the matrix of a transform function from its name and arguments.
 *
 * The names are case-sensitive (CSS Transforms 1 §7.2); a name that is not
 * one of the six functions is invalid.
 */
static int
values_function_matrix(
	const char *name,
	size_t name_length,
	const struct values_arg *args,
	size_t count,
	struct vec_matrix *matrix)
{
	int is_name;
	int error;

	/* Makes translate. */
	is_name = values_word_equal(name, name_length, "translate", 0);
	if (is_name) {
		error = values_make_translate(args, count, matrix);
		if (error != 0)
			return error;
		return 0;
	}

	/* Makes scale. */
	is_name = values_word_equal(name, name_length, "scale", 0);
	if (is_name) {
		error = values_make_scale(args, count, matrix);
		if (error != 0)
			return error;
		return 0;
	}

	/* Makes rotate. */
	is_name = values_word_equal(name, name_length, "rotate", 0);
	if (is_name) {
		error = values_make_rotate(args, count, matrix);
		if (error != 0)
			return error;
		return 0;
	}

	/* Makes skewX. */
	is_name = values_word_equal(name, name_length, "skewX", 0);
	if (is_name) {
		error = values_make_skew(args, count, 0, matrix);
		if (error != 0)
			return error;
		return 0;
	}

	/* Makes skewY. */
	is_name = values_word_equal(name, name_length, "skewY", 0);
	if (is_name) {
		error = values_make_skew(args, count, 1, matrix);
		if (error != 0)
			return error;
		return 0;
	}

	/* Makes matrix. */
	is_name = values_word_equal(name, name_length, "matrix", 0);
	if (is_name) {
		error = values_make_matrix(args, count, matrix);
		if (error != 0)
			return error;
		return 0;
	}

	/* Refuses a name that is no transform function. */
	return EINVAL;
}

/*
 * Makes translate(tx [ty]): one or two lengths, ty being 0 when absent
 * (CSS Transforms 1 §7.2 and §7.3).
 */
static int
values_make_translate(
	const struct values_arg *args,
	size_t count,
	struct vec_matrix *matrix)
{
	struct vec_matrix result;
	float tx;
	float ty;
	int error;

	/* Refuses a count of arguments the function does not take. */
	if (count != 1 && count != 2)
		return EINVAL;

	/* Takes the horizontal offset. */
	error = values_arg_length(&args[0], &tx);
	if (error != 0)
		return error;

	/* Takes the vertical offset, which is 0 when absent. */
	ty = 0.0f;
	if (count == 2) {
		error = values_arg_length(&args[1], &ty);
		if (error != 0)
			return error;
	}

	/* Makes the matrix. */
	error = vec_matrix_translate(&result, tx, ty);
	if (error != 0)
		return error;
	*matrix = result;

	/* Succeeded: *matrix is the translation. */
	return 0;
}

/*
 * Makes scale(sx [sy]): one or two bare numbers, sy being sx when absent
 * (CSS Transforms 1 §7.2 and §7.3).
 */
static int
values_make_scale(
	const struct values_arg *args,
	size_t count,
	struct vec_matrix *matrix)
{
	struct vec_matrix result;
	float sx;
	float sy;
	int error;

	/* Refuses a count of arguments the function does not take. */
	if (count != 1 && count != 2)
		return EINVAL;

	/* Takes the horizontal factor, which has no unit. */
	if (args[0].unit != VALUES_ARG_NUMBER)
		return EINVAL;
	sx = args[0].value;

	/* Takes the vertical factor, which is the horizontal one when absent. */
	sy = sx;
	if (count == 2) {
		if (args[1].unit != VALUES_ARG_NUMBER)
			return EINVAL;
		sy = args[1].value;
	}

	/* Makes the matrix. */
	error = vec_matrix_scale(&result, sx, sy);
	if (error != 0)
		return error;
	*matrix = result;

	/* Succeeded: *matrix is the scaling. */
	return 0;
}

/*
 * Makes rotate(a) or rotate(a cx cy): the angle, and the centre of the
 * rotation as two lengths, which makes translate(cx cy) rotate(a)
 * translate(-cx -cy) (CSS Transforms 1 §7.3).
 */
static int
values_make_rotate(
	const struct values_arg *args,
	size_t count,
	struct vec_matrix *matrix)
{
	struct vec_matrix total;
	struct vec_matrix step;
	float degrees;
	float cx;
	float cy;
	int finite;
	int error;

	/* Refuses a count of arguments the function does not take. */
	if (count != 1 && count != 3)
		return EINVAL;

	/* Takes the angle. */
	error = values_arg_angle(&args[0], &degrees);
	if (error != 0)
		return error;

	/* Takes the centre, which is the origin when absent. */
	cx = 0.0f;
	cy = 0.0f;
	if (count == 3) {
		error = values_arg_length(&args[1], &cx);
		if (error != 0)
			return error;
		error = values_arg_length(&args[2], &cy);
		if (error != 0)
			return error;
	}

	/* Starts with the translation to the centre. */
	error = vec_matrix_translate(&total, cx, cy);
	if (error != 0)
		return error;

	/* Composes the rotation to the right of it. */
	error = vec_matrix_rotate(&step, degrees);
	if (error != 0)
		return error;
	vec_matrix_multiply(&total, &step, &total);

	/* Composes the translation back from the centre to the right of it. */
	error = vec_matrix_translate(&step, -cx, -cy);
	if (error != 0)
		return error;
	vec_matrix_multiply(&total, &step, &total);

	/* Refuses a product that overflowed. */
	finite = values_matrix_finite(&total);
	if (!finite)
		return EINVAL;

	/* Publishes the matrix. */
	*matrix = total;

	/* Succeeded: *matrix is the rotation. */
	return 0;
}

/* Makes skewX(a) (along_y 0) or skewY(a) (along_y 1): one angle (CSS Transforms 1 §7.3). */
static int
values_make_skew(
	const struct values_arg *args,
	size_t count,
	int along_y,
	struct vec_matrix *matrix)
{
	struct vec_matrix result;
	float degrees;
	int error;

	/* Refuses a count of arguments the function does not take. */
	if (count != 1)
		return EINVAL;

	/* Takes the angle. */
	error = values_arg_angle(&args[0], &degrees);
	if (error != 0)
		return error;

	/* Makes the matrix along the axis the function names. */
	if (along_y)
		error = vec_matrix_skew_y(&result, degrees);
	else
		error = vec_matrix_skew_x(&result, degrees);
	if (error != 0)
		return error;
	*matrix = result;

	/* Succeeded: *matrix is the skew. */
	return 0;
}

/* Makes matrix(a b c d e f): six bare numbers, the entries of the matrix as they are. */
static int
values_make_matrix(
	const struct values_arg *args,
	size_t count,
	struct vec_matrix *matrix)
{
	size_t index;

	/* Refuses a count of arguments the function does not take. */
	if (count != 6)
		return EINVAL;

	/* Refuses an argument that has a unit. */
	for (index = 0; index < 6; index++) {
		if (args[index].unit != VALUES_ARG_NUMBER)
			return EINVAL;
	}

	/* Copies the six entries in the order the function gives them. */
	matrix->a = args[0].value;
	matrix->b = args[1].value;
	matrix->c = args[2].value;
	matrix->d = args[3].value;
	matrix->e = args[4].value;
	matrix->f = args[5].value;

	/* Succeeded: *matrix is the given matrix. */
	return 0;
}

/* Tells whether all six entries of a matrix are finite. */
static int
values_matrix_finite(
	const struct vec_matrix *matrix)
{
	int finite;

	/* Checks the entries one by one. */
	finite = vec_finite(matrix->a);
	if (!finite)
		return 0;
	finite = vec_finite(matrix->b);
	if (!finite)
		return 0;
	finite = vec_finite(matrix->c);
	if (!finite)
		return 0;
	finite = vec_finite(matrix->d);
	if (!finite)
		return 0;
	finite = vec_finite(matrix->e);
	if (!finite)
		return 0;
	finite = vec_finite(matrix->f);
	if (!finite)
		return 0;

	/* Succeeded: every entry is finite. */
	return 1;
}

/*
 * Tells where an alignment puts the viewBox along x.
 *
 * Values referenced: SVG 2 §8.2 (xMid and xMax in the alignment).
 */
static enum values_place
values_place_x(
	enum svg_align align)
{
	/* Groups the nine alignments by their x part. */
	switch (align) {
	case SVG_ALIGN_XMID_YMIN:
	case SVG_ALIGN_XMID_YMID:
	case SVG_ALIGN_XMID_YMAX:
		return VALUES_PLACE_MID;
	case SVG_ALIGN_XMAX_YMIN:
	case SVG_ALIGN_XMAX_YMID:
	case SVG_ALIGN_XMAX_YMAX:
		return VALUES_PLACE_MAX;
	default:
		/* The near edge: xMin, and none, which does not align. */
		return VALUES_PLACE_MIN;
	}
}

/*
 * Tells where an alignment puts the viewBox along y.
 *
 * Values referenced: SVG 2 §8.2 (YMid and YMax in the alignment).
 */
static enum values_place
values_place_y(
	enum svg_align align)
{
	/* Groups the nine alignments by their y part. */
	switch (align) {
	case SVG_ALIGN_XMIN_YMID:
	case SVG_ALIGN_XMID_YMID:
	case SVG_ALIGN_XMAX_YMID:
		return VALUES_PLACE_MID;
	case SVG_ALIGN_XMIN_YMAX:
	case SVG_ALIGN_XMID_YMAX:
	case SVG_ALIGN_XMAX_YMAX:
		return VALUES_PLACE_MAX;
	default:
		/* The near edge: YMin, and none, which does not align. */
		return VALUES_PLACE_MIN;
	}
}

/*
 * Reads a paint that is not a reference: none, currentColor (ASCII
 * case-insensitive) or a color the tree parses.  context-fill and
 * context-stroke give ENOTSUP.
 *
 * Errors: EINVAL; ENOTSUP.
 */
static int
values_read_plain_paint(
	const struct svg_tree *tree,
	const char *text,
	size_t length,
	enum svg_paint_kind *kind,
	uint32_t *color)
{
	uint32_t parsed;
	int matched;
	int error;

	/* Reads the keyword none. */
	matched = values_word_equal(text, length, "none", 1);
	if (matched) {
		*kind = SVG_PAINT_NONE;
		*color = 0;
		return 0;
	}

	/* Reads the keyword currentColor. */
	matched = values_word_equal(text, length, "currentcolor", 1);
	if (matched) {
		*kind = SVG_PAINT_CURRENT_COLOR;
		*color = 0;
		return 0;
	}

	/* Refuses the context paints, which this renderer does not do. */
	matched = values_word_equal(text, length, "context-fill", 1);
	if (matched)
		return ENOTSUP;
	matched = values_word_equal(text, length, "context-stroke", 1);
	if (matched)
		return ENOTSUP;

	/* Refuses a color when there is no color parser to ask. */
	if (tree == NULL || tree->parse_color == NULL)
		return EINVAL;

	/* Asks the tree to parse the text as a color. */
	error = tree->parse_color(tree->context, text, length, &parsed);
	if (error != 0)
		return EINVAL;

	/* Publishes the color. */
	*kind = SVG_PAINT_COLOR;
	*color = parsed;

	/* Succeeded: the text is a color. */
	return 0;
}
