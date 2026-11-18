/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The smooth shadings of libpdf's reader (stage 2 of design-pdf.md): the
 * axial (type 2) and radial (type 3) shadings that sh paints and that a
 * shading pattern fills with.
 *
 * A shading is drawn as an image item: the colour of each pixel of a
 * region of the page is found from the shading's parameter there, through
 * a table of the shading's function sampled across its domain.  The
 * display list keeps drawing only fills, images and clips; the region is
 * clipped by whatever clip is in force (and a pattern fill by its path).
 * The functions read are the exponential (type 2), stitching (type 3)
 * and sampled (type 0) ones, on DeviceGray, DeviceRGB, DeviceCMYK and
 * the spaces built on them.  Other shadings and functions report ENOTSUP.
 */

#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include <pdf.h>

#include "internal.h"

/* How many pixels a shading's image has per page point, and the longest side of one. */
#define SHADING_PIXELS_PER_POINT 2.0
#define SHADING_SIDE_MAX 1024

/* How many colours the function is sampled at across its domain. */
#define SHADING_SAMPLES 1024

/* The most output components a function may have, and how deep stitching functions may nest. */
#define SHADING_COMPONENTS_MAX 4
#define SHADING_FUNCTION_DEPTH 8

/* The most samples a sampled function may have. */
#define SHADING_SAMPLED_MAX ((size_t)65536)

/*
 * One smooth shading being drawn: its type, colour space, geometry and
 * the colours sampled along its parameter.
 *
 * coords are x0 y0 x1 y1 (axial) or x0 y0 r0 x1 y1 r1 (radial) in shading
 * space; domain is t0 t1; extend says whether each end is continued past
 * the geometry.  colors holds SHADING_SAMPLES RGB colours from t0 to t1.
 */
struct shading {
	int type;
	int components;
	double coords[6];
	double domain[2];
	int extend[2];
	double colors[SHADING_SAMPLES][3];
};

static int read_shading(struct pdf_document *document, struct pdf_object *object, struct shading *shading);
static int read_space(struct pdf_document *document, struct pdf_object *space, int *components);
static int read_numbers(struct pdf_document *document, struct pdf_object *array, double *numbers, size_t count);
static int sample_colors(struct pdf_document *document, struct pdf_object *function, struct shading *shading);
static int evaluate(struct pdf_document *document, struct pdf_object *function, double input, double *output, int outputs, int depth);
static int evaluate_exponential(struct pdf_document *document, struct pdf_object *function, double input, double *output, int outputs);
static int evaluate_stitching(struct pdf_document *document, struct pdf_object *function, double input, double *output, int outputs, int depth);
static int evaluate_sampled(struct pdf_document *document, struct pdf_object *function, double input, double *output, int outputs);
static void to_rgb(const double *values, int components, double rgb[3]);
static int parameter(const struct shading *shading, double x, double y, double *t);
static int invert(const double matrix[6], double inverse[6]);
static double clamp_unit(double value);

/*
 * Draws a shading into an image over a region of the page.
 *
 * matrix maps the shading's space to the page's shown space; bounds is
 * the region (left, top, right, bottom) in the page's shown space.  The
 * image (RGBA, malloc'd, transparent where the shading does not reach)
 * and its placement, the matrix from the unit square to the page, are
 * returned for a display list's image item.
 */
int
pdf_shading_image(
	struct pdf_document *document,
	struct pdf_object *object,
	const double matrix[6],
	const double bounds[4],
	unsigned char **pixels,
	size_t *width,
	size_t *height,
	double placement[6])
{
	struct shading *shading;
	unsigned char *image;
	unsigned char *pixel;
	double inverse[6];
	double page_x;
	double page_y;
	double shading_x;
	double shading_y;
	double t;
	double scale_x;
	double scale_y;
	size_t columns;
	size_t rows;
	size_t column;
	size_t row;
	long sample;
	int inside;
	int error;

	/* Refuses a region of no size, and a matrix that squashes the shading to nothing. */
	if (!(bounds[2] > bounds[0] && bounds[3] > bounds[1]))
		return EINVAL;
	error = invert(matrix, inverse);
	if (error != 0)
		return error;

	/* Reads the shading and samples its colours. */
	shading = malloc(sizeof(*shading));
	if (shading == NULL)
		return ENOMEM;
	error = read_shading(document, object, shading);
	if (error != 0) {
		free(shading);
		return error;
	}

	/* Sizes the image: two pixels a point, up to the longest side. */
	scale_x = (bounds[2] - bounds[0]) * SHADING_PIXELS_PER_POINT;
	scale_y = (bounds[3] - bounds[1]) * SHADING_PIXELS_PER_POINT;
	if (scale_x > SHADING_SIDE_MAX)
		scale_x = SHADING_SIDE_MAX;
	if (scale_y > SHADING_SIDE_MAX)
		scale_y = SHADING_SIDE_MAX;
	columns = (size_t)ceil(scale_x);
	rows = (size_t)ceil(scale_y);
	if (columns == 0)
		columns = 1;
	if (rows == 0)
		rows = 1;
	image = malloc(columns * rows * 4);
	if (image == NULL) {
		free(shading);
		return ENOMEM;
	}

	/* Colours each pixel by the shading's parameter at its centre. */
	for (row = 0; row < rows; row++) {
		for (column = 0; column < columns; column++) {
			/* The pixel's centre on the page, and in the shading's space. */
			page_x = bounds[0] + ((double)column + 0.5) * (bounds[2] - bounds[0]) / (double)columns;
			page_y = bounds[1] + ((double)row + 0.5) * (bounds[3] - bounds[1]) / (double)rows;
			shading_x = inverse[0] * page_x + inverse[2] * page_y + inverse[4];
			shading_y = inverse[1] * page_x + inverse[3] * page_y + inverse[5];

			/* The parameter there; a point the shading does not reach is transparent. */
			pixel = image + (row * columns + column) * 4;
			inside = parameter(shading, shading_x, shading_y, &t);
			if (!inside) {
				memset(pixel, 0, 4);
				continue;
			}

			/* The sampled colour at the parameter. */
			sample = (long)floor((t - shading->domain[0]) / (shading->domain[1] - shading->domain[0]) * (SHADING_SAMPLES - 1) + 0.5);
			if (sample < 0)
				sample = 0;
			if (sample > SHADING_SAMPLES - 1)
				sample = SHADING_SAMPLES - 1;
			pixel[0] = (unsigned char)(shading->colors[sample][0] * 255.0 + 0.5);
			pixel[1] = (unsigned char)(shading->colors[sample][1] * 255.0 + 0.5);
			pixel[2] = (unsigned char)(shading->colors[sample][2] * 255.0 + 0.5);
			pixel[3] = 255;
		}
	}

	/* The shading's description goes; the pixels are the caller's. */
	free(shading);

	/* Places the image on the region: the unit square's corners on the region's. */
	placement[0] = bounds[2] - bounds[0];
	placement[1] = 0.0;
	placement[2] = 0.0;
	placement[3] = bounds[3] - bounds[1];
	placement[4] = bounds[0];
	placement[5] = bounds[1];

	/* Succeeded: the caller owns the image. */
	*pixels = image;
	*width = columns;
	*height = rows;
	return 0;
}

/*
 * Reads an axial or radial shading dictionary (or stream) and samples its
 * function's colours across its domain.
 */
static int
read_shading(
	struct pdf_document *document,
	struct pdf_object *object,
	struct shading *shading)
{
	struct pdf_object *type;
	struct pdf_object *space;
	struct pdf_object *coords;
	struct pdf_object *domain;
	struct pdf_object *extend;
	struct pdf_object *function;
	int error;

	/* Only a dictionary or a stream is a shading. */
	if (object->type != PDF_OBJECT_DICTIONARY && object->type != PDF_OBJECT_STREAM)
		return PDF_EFORMAT;
	memset(shading, 0, sizeof(*shading));

	/* The type: axial and radial are read, the others are left for later stages. */
	error = pdf_reader_resolve_key(document, object, "ShadingType", &type);
	if (error != 0)
		return error;
	if (type->type != PDF_OBJECT_INTEGER)
		return PDF_EFORMAT;
	if (type->integer != 2 && type->integer != 3)
		return ENOTSUP;
	shading->type = (int)type->integer;

	/* The colour space. */
	error = pdf_reader_resolve_key(document, object, "ColorSpace", &space);
	if (error != 0)
		return error;
	error = read_space(document, space, &shading->components);
	if (error != 0)
		return error;

	/* The geometry: four numbers for an axial shading, six for a radial one. */
	error = pdf_reader_resolve_key(document, object, "Coords", &coords);
	if (error != 0)
		return error;
	if (shading->type == 2) {
		error = read_numbers(document, coords, shading->coords, 4);
	} else {
		error = read_numbers(document, coords, shading->coords, 6);
	}

	/* Refuses coordinates that are not numbers. */
	if (error != 0)
		return error;

	/* The domain, 0 to 1 by default. */
	shading->domain[0] = 0.0;
	shading->domain[1] = 1.0;
	error = pdf_reader_resolve_key(document, object, "Domain", &domain);
	if (error != 0)
		return error;
	if (domain->type == PDF_OBJECT_ARRAY) {
		error = read_numbers(document, domain, shading->domain, 2);
		if (error != 0)
			return error;
	}

	/* An empty domain (or one that is not a number) has no colours. */
	if (!(shading->domain[1] != shading->domain[0]))
		return PDF_EFORMAT;

	/* Whether each end is extended, neither by default. */
	error = pdf_reader_resolve_key(document, object, "Extend", &extend);
	if (error != 0)
		return error;
	if (extend->type == PDF_OBJECT_ARRAY && extend->count == 2) {
		error = pdf_reader_resolve(document, extend->values[0], &type);
		if (error == 0 && type->type == PDF_OBJECT_BOOLEAN)
			shading->extend[0] = type->boolean;
		error = pdf_reader_resolve(document, extend->values[1], &type);
		if (error == 0 && type->type == PDF_OBJECT_BOOLEAN)
			shading->extend[1] = type->boolean;
	}

	/* Samples the function's colours across the domain. */
	error = pdf_reader_resolve_key(document, object, "Function", &function);
	if (error != 0)
		return error;
	error = sample_colors(document, function, shading);
	if (error != 0)
		return error;

	/* Succeeded: the shading can be drawn. */
	return 0;
}

/* Tells how many components a shading's colour space has (a device space or one built on it). */
static int
read_space(
	struct pdf_document *document,
	struct pdf_object *space,
	int *components)
{
	struct pdf_object *family;
	struct pdf_object *profile;
	struct pdf_object *count;
	int is_name;
	int error;

	/* A name, or an array whose first element names the family. */
	family = space;
	if (space->type == PDF_OBJECT_ARRAY) {
		if (space->count == 0)
			return PDF_EFORMAT;
		error = pdf_reader_resolve(document, space->values[0], &family);
		if (error != 0)
			return error;
	}

	/* The gray spaces. */
	is_name = pdf_object_is_name(family, "DeviceGray");
	if (!is_name)
		is_name = pdf_object_is_name(family, "CalGray");
	if (!is_name)
		is_name = pdf_object_is_name(family, "G");
	if (is_name) {
		*components = 1;
		return 0;
	}

	/* The RGB spaces. */
	is_name = pdf_object_is_name(family, "DeviceRGB");
	if (!is_name)
		is_name = pdf_object_is_name(family, "CalRGB");
	if (!is_name)
		is_name = pdf_object_is_name(family, "RGB");
	if (is_name) {
		*components = 3;
		return 0;
	}

	/* CMYK. */
	is_name = pdf_object_is_name(family, "DeviceCMYK");
	if (!is_name)
		is_name = pdf_object_is_name(family, "CMYK");
	if (is_name) {
		*components = 4;
		return 0;
	}

	/* An ICC-based space by its profile's count; anything else is left for later stages. */
	is_name = pdf_object_is_name(family, "ICCBased");
	if (!is_name)
		return ENOTSUP;
	if (space->type != PDF_OBJECT_ARRAY || space->count < 2)
		return ENOTSUP;
	error = pdf_reader_resolve(document, space->values[1], &profile);
	if (error != 0)
		return error;
	error = pdf_reader_resolve_key(document, profile, "N", &count);
	if (error != 0)
		return error;
	if (count->type != PDF_OBJECT_INTEGER)
		return PDF_EFORMAT;
	if (count->integer != 1 &&
	    count->integer != 3 &&
	    count->integer != 4)
		return ENOTSUP;

	/* Succeeded: the profile's component count. */
	*components = (int)count->integer;
	return 0;
}

/* Reads count finite numbers from an array. */
static int
read_numbers(
	struct pdf_document *document,
	struct pdf_object *array,
	double *numbers,
	size_t count)
{
	struct pdf_object *element;
	size_t index;
	int error;

	/* The array must hold at least count elements. */
	if (array->type != PDF_OBJECT_ARRAY || array->count < count)
		return PDF_EFORMAT;

	/* Each element, which must be a finite number. */
	for (index = 0; index < count; index++) {
		error = pdf_reader_resolve(document, array->values[index], &element);
		if (error != 0)
			return error;
		error = pdf_object_number(element, &numbers[index]);
		if (error != 0)
			return error;
		if (!(numbers[index] > -1e9 && numbers[index] < 1e9))
			return PDF_EFORMAT;
	}

	/* Succeeded: numbers holds the elements. */
	return 0;
}

/*
 * Samples the shading's function (one function of all components, or an
 * array of one function per component) across the domain into colours.
 */
static int
sample_colors(
	struct pdf_document *document,
	struct pdf_object *function,
	struct shading *shading)
{
	struct pdf_object *component;
	double values[SHADING_COMPONENTS_MAX];
	double t;
	size_t sample;
	int index;
	int error;

	/* An array must have one function per component. */
	if (function->type == PDF_OBJECT_ARRAY && function->count != (size_t)shading->components)
		return PDF_EFORMAT;

	/* Evaluates the function at each sample of the domain. */
	for (sample = 0; sample < SHADING_SAMPLES; sample++) {
		t = shading->domain[0] + (shading->domain[1] - shading->domain[0]) * (double)sample / (SHADING_SAMPLES - 1);
		if (function->type == PDF_OBJECT_ARRAY) {
			/* One output from each component's function. */
			for (index = 0; index < shading->components; index++) {
				error = pdf_reader_resolve(document, function->values[index], &component);
				if (error != 0)
					return error;
				error = evaluate(document, component, t, &values[index], 1, 0);
				if (error != 0)
					return error;
			}
		} else {
			/* Every output from the one function. */
			error = evaluate(document, function, t, values, shading->components, 0);
			if (error != 0)
				return error;
		}

		/* The sample's colour in RGB. */
		to_rgb(values, shading->components, shading->colors[sample]);
	}

	/* Succeeded: the colours are sampled. */
	return 0;
}

/* Evaluates a function of one input at a value, into outputs outputs. */
static int
evaluate(
	struct pdf_document *document,
	struct pdf_object *function,
	double input,
	double *output,
	int outputs,
	int depth)
{
	struct pdf_object *type;
	struct pdf_object *domain;
	double bounds[2];
	int error;

	/* Refuses functions nested past the limit. */
	if (depth > SHADING_FUNCTION_DEPTH)
		return PDF_EFORMAT;

	/* The input is clipped to the function's domain. */
	error = pdf_reader_resolve_key(document, function, "Domain", &domain);
	if (error != 0)
		return error;
	error = read_numbers(document, domain, bounds, 2);
	if (error == 0) {
		if (input < bounds[0])
			input = bounds[0];
		if (input > bounds[1])
			input = bounds[1];
	}

	/* Evaluates the function by its type; a PostScript calculator function is left for later stages. */
	error = pdf_reader_resolve_key(document, function, "FunctionType", &type);
	if (error != 0)
		return error;
	if (type->type != PDF_OBJECT_INTEGER)
		return PDF_EFORMAT;
	switch (type->integer) {
	case 0:
		error = evaluate_sampled(document, function, input, output, outputs);
		break;
	case 2:
		error = evaluate_exponential(document, function, input, output, outputs);
		break;
	case 3:
		error = evaluate_stitching(document, function, input, output, outputs, depth);
		break;
	default:
		error = ENOTSUP;
		break;
	}

	/* Reports a function that could not be evaluated. */
	if (error != 0)
		return error;

	/* Succeeded: output holds the values. */
	return 0;
}

/* Evaluates an exponential interpolation function: C0 + x^N (C1 - C0). */
static int
evaluate_exponential(
	struct pdf_document *document,
	struct pdf_object *function,
	double input,
	double *output,
	int outputs)
{
	struct pdf_object *first_object;
	struct pdf_object *last_object;
	struct pdf_object *exponent_object;
	double first[SHADING_COMPONENTS_MAX];
	double last[SHADING_COMPONENTS_MAX];
	double exponent;
	double whole;
	double factor;
	int index;
	int error;

	/* C0 and C1, 0 and 1 by default. */
	for (index = 0; index < outputs; index++) {
		first[index] = 0.0;
		last[index] = 1.0;
	}

	/* The function's own C0. */
	error = pdf_reader_resolve_key(document, function, "C0", &first_object);
	if (error != 0)
		return error;
	if (first_object->type == PDF_OBJECT_ARRAY) {
		error = read_numbers(document, first_object, first, (size_t)outputs);
		if (error != 0)
			return error;
	}

	/* The function's own C1. */
	error = pdf_reader_resolve_key(document, function, "C1", &last_object);
	if (error != 0)
		return error;
	if (last_object->type == PDF_OBJECT_ARRAY) {
		error = read_numbers(document, last_object, last, (size_t)outputs);
		if (error != 0)
			return error;
	}

	/* The exponent. */
	error = pdf_reader_resolve_key(document, function, "N", &exponent_object);
	if (error != 0)
		return error;
	error = pdf_object_number(exponent_object, &exponent);
	if (error != 0)
		return error;

	/* The interpolation; a negative input to a fractional power is 0. */
	factor = 0.0;
	whole = floor(exponent);
	if (input > 0.0 || exponent == whole)
		factor = pow(input, exponent);
	if (!(factor > -1e9 && factor < 1e9))
		factor = 0.0;
	for (index = 0; index < outputs; index++)
		output[index] = first[index] + factor * (last[index] - first[index]);

	/* Succeeded: output holds the values. */
	return 0;
}

/*
 * Evaluates a stitching function: the subfunction whose part of the domain
 * holds the input, with the input mapped through its /Encode pair.
 */
static int
evaluate_stitching(
	struct pdf_document *document,
	struct pdf_object *function,
	double input,
	double *output,
	int outputs,
	int depth)
{
	struct pdf_object *functions;
	struct pdf_object *bounds_object;
	struct pdf_object *encode_object;
	struct pdf_object *domain_object;
	struct pdf_object *chosen;
	double domain[2];
	double low;
	double high;
	double bound;
	double encode[2];
	double mapped;
	size_t count;
	size_t index;
	int error;

	/* The subfunctions, the bounds between them, and the domain. */
	error = pdf_reader_resolve_key(document, function, "Functions", &functions);
	if (error != 0)
		return error;
	if (functions->type != PDF_OBJECT_ARRAY || functions->count == 0)
		return PDF_EFORMAT;
	count = functions->count;
	error = pdf_reader_resolve_key(document, function, "Bounds", &bounds_object);
	if (error != 0)
		return error;
	if (bounds_object->type != PDF_OBJECT_ARRAY || bounds_object->count != count - 1)
		return PDF_EFORMAT;
	error = pdf_reader_resolve_key(document, function, "Domain", &domain_object);
	if (error != 0)
		return error;
	error = read_numbers(document, domain_object, domain, 2);
	if (error != 0)
		return error;

	/* Finds the part that holds the input: [low, high). */
	low = domain[0];
	high = domain[1];
	for (index = 0; index < count; index++) {
		high = domain[1];
		if (index < count - 1) {
			error = pdf_reader_resolve(document, bounds_object->values[index], &chosen);
			if (error != 0)
				return error;
			error = pdf_object_number(chosen, &bound);
			if (error != 0)
				return error;
			high = bound;
		}

		/* The input falls in this function's part, or this is the last part. */
		if (input < high || index == count - 1)
			break;
		low = high;
	}

	/* Maps the input through the part's /Encode pair (the part itself by default). */
	encode[0] = low;
	encode[1] = high;
	error = pdf_reader_resolve_key(document, function, "Encode", &encode_object);
	if (error != 0)
		return error;
	if (encode_object->type == PDF_OBJECT_ARRAY && encode_object->count >= 2 * count) {
		error = pdf_reader_resolve(document, encode_object->values[2 * index], &chosen);
		if (error == 0)
			error = pdf_object_number(chosen, &encode[0]);
		if (error != 0)
			return error;
		error = pdf_reader_resolve(document, encode_object->values[2 * index + 1], &chosen);
		if (error == 0)
			error = pdf_object_number(chosen, &encode[1]);
		if (error != 0)
			return error;
	}

	/* The input mapped from the part onto the function's encoded domain. */
	mapped = encode[0];
	if (high != low)
		mapped = encode[0] + (input - low) * (encode[1] - encode[0]) / (high - low);

	/* Evaluates the part's function. */
	error = pdf_reader_resolve(document, functions->values[index], &chosen);
	if (error != 0)
		return error;
	error = evaluate(document, chosen, mapped, output, outputs, depth + 1);
	if (error != 0)
		return error;

	/* Succeeded: output holds the values. */
	return 0;
}

/*
 * Evaluates a sampled function of one input: the samples on each side of
 * the input's position, linearly interpolated, decoded into the range.
 */
static int
evaluate_sampled(
	struct pdf_document *document,
	struct pdf_object *function,
	double input,
	double *output,
	int outputs)
{
	struct pdf_object *size_object;
	struct pdf_object *bits_object;
	struct pdf_object *domain_object;
	struct pdf_object *range_object;
	struct pdf_object *encode_object;
	struct pdf_object *decode_object;
	const unsigned char *data;
	unsigned char *owned;
	double domain[2];
	double range[2 * SHADING_COMPONENTS_MAX];
	double encode[2];
	double decode[2 * SHADING_COMPONENTS_MAX];
	double position;
	double fraction;
	double low_value;
	double high_value;
	double maximum;
	double size_value;
	size_t data_size;
	size_t count;
	size_t low;
	size_t high;
	size_t bit;
	size_t step;
	unsigned long sample;
	long bits;
	int which;
	int index;
	int dct;
	int error;

	/* The size (one input), the bits per sample, the domain and the range. */
	error = pdf_reader_resolve_key(document, function, "Size", &size_object);
	if (error != 0)
		return error;
	error = read_numbers(document, size_object, &size_value, 1);
	if (error != 0)
		return error;
	if (!(size_value >= 1.0 && size_value <= (double)SHADING_SAMPLED_MAX))
		return PDF_EFORMAT;
	count = (size_t)size_value;
	error = pdf_reader_resolve_key(document, function, "BitsPerSample", &bits_object);
	if (error != 0)
		return error;
	if (bits_object->type != PDF_OBJECT_INTEGER)
		return PDF_EFORMAT;
	bits = bits_object->integer;
	if (bits != 1 &&
	    bits != 2 &&
	    bits != 4 &&
	    bits != 8 &&
	    bits != 12 &&
	    bits != 16 &&
	    bits != 24 &&
	    bits != 32)
		return PDF_EFORMAT;
	error = pdf_reader_resolve_key(document, function, "Domain", &domain_object);
	if (error != 0)
		return error;
	error = read_numbers(document, domain_object, domain, 2);
	if (error != 0)
		return error;
	error = pdf_reader_resolve_key(document, function, "Range", &range_object);
	if (error != 0)
		return error;
	error = read_numbers(document, range_object, range, (size_t)outputs * 2);
	if (error != 0)
		return error;

	/* The encoding of the input, 0 to size - 1 by default, and the decoding of the outputs, the range by default. */
	encode[0] = 0.0;
	encode[1] = (double)(count - 1);
	error = pdf_reader_resolve_key(document, function, "Encode", &encode_object);
	if (error == 0 && encode_object->type == PDF_OBJECT_ARRAY)
		(void)read_numbers(document, encode_object, encode, 2);
	memcpy(decode, range, sizeof(decode));
	error = pdf_reader_resolve_key(document, function, "Decode", &decode_object);
	if (error == 0 && decode_object->type == PDF_OBJECT_ARRAY)
		(void)read_numbers(document, decode_object, decode, (size_t)outputs * 2);

	/* The samples; the stream must hold them all. */
	if (function->type != PDF_OBJECT_STREAM)
		return PDF_EFORMAT;
	error = pdf_filter_decode(document, function, 0, &data, &data_size, &owned, &dct);
	if (error != 0)
		return error;
	if (count * (size_t)outputs * (size_t)bits > data_size * 8) {
		free(owned);
		return PDF_EFORMAT;
	}

	/* The input's position among the samples. */
	position = encode[0];
	if (domain[1] != domain[0])
		position = encode[0] + (input - domain[0]) * (encode[1] - encode[0]) / (domain[1] - domain[0]);
	if (!(position >= 0.0))
		position = 0.0;
	if (position > (double)(count - 1))
		position = (double)(count - 1);
	low = (size_t)floor(position);
	high = low + 1;
	if (high > count - 1)
		high = count - 1;
	fraction = position - (double)low;
	maximum = pow(2.0, (double)bits) - 1.0;

	/* Interpolates each output between its two samples and decodes it. */
	for (index = 0; index < outputs; index++) {
		low_value = 0.0;
		high_value = 0.0;
		for (which = 0; which < 2; which++) {
			/* The sample's bits, the high one first. */
			bit = low * (size_t)outputs + (size_t)index;
			if (which == 1)
				bit = high * (size_t)outputs + (size_t)index;
			bit *= (size_t)bits;
			sample = 0;
			for (step = 0; step < (size_t)bits; step++)
				sample = (sample << 1) | ((data[(bit + step) / 8] >> (7 - (bit + step) % 8)) & 1U);
			if (which == 0) {
				low_value = (double)sample;
			} else {
				high_value = (double)sample;
			}
		}

		/* Interpolates between the two samples and maps the result by /Decode. */
		output[index] = low_value + fraction * (high_value - low_value);
		output[index] = decode[2 * index] + output[index] * (decode[2 * index + 1] - decode[2 * index]) / maximum;
	}

	/* The samples' decoded copy goes. */
	free(owned);

	/* Succeeded: output holds the values. */
	return 0;
}

/* Converts a colour of 1 (gray), 3 (RGB) or 4 (CMYK) components to RGB. */
static void
to_rgb(
	const double *values,
	int components,
	double rgb[3])
{
	/* By the number of components, as the content's colours are. */
	if (components == 1) {
		rgb[0] = clamp_unit(values[0]);
		rgb[1] = rgb[0];
		rgb[2] = rgb[0];
	} else if (components == 3) {
		rgb[0] = clamp_unit(values[0]);
		rgb[1] = clamp_unit(values[1]);
		rgb[2] = clamp_unit(values[2]);
	} else {
		rgb[0] = (1.0 - clamp_unit(values[0])) * (1.0 - clamp_unit(values[3]));
		rgb[1] = (1.0 - clamp_unit(values[1])) * (1.0 - clamp_unit(values[3]));
		rgb[2] = (1.0 - clamp_unit(values[2])) * (1.0 - clamp_unit(values[3]));
	}
}

/*
 * Finds the shading's parameter at a point of its space.  Reports whether
 * the shading paints the point: a point before the start or past the end
 * is painted only when that end is extended.
 */
static int
parameter(
	const struct shading *shading,
	double x,
	double y,
	double *t)
{
	const double *c;
	double dx;
	double dy;
	double dr;
	double px;
	double py;
	double a;
	double b;
	double k;
	double discriminant;
	double root;
	double s;
	double s1;
	double s2;
	double length;
	double magnitude;
	int beyond;

	/* The parameter s by the kind of shading. */
	c = shading->coords;
	if (shading->type == 2) {
		/* Axial: the projection of the point on the axis, in units of its length. */
		dx = c[2] - c[0];
		dy = c[3] - c[1];
		length = dx * dx + dy * dy;
		s = 0.0;
		if (length > 0.0)
			s = ((x - c[0]) * dx + (y - c[1]) * dy) / length;
	} else {
		/*
		 * Radial: the largest s for which the point is on the circle of
		 * centre c0 + s (c1 - c0) and radius r0 + s (r1 - r0), that radius
		 * not negative.
		 */
		dx = c[3] - c[0];
		dy = c[4] - c[1];
		dr = c[5] - c[2];
		px = x - c[0];
		py = y - c[1];
		a = dx * dx + dy * dy - dr * dr;
		b = px * dx + py * dy + c[2] * dr;
		k = px * px + py * py - c[2] * c[2];
		magnitude = fabs(a);
		if (magnitude < 1e-12) {
			/* One root: the circles grow as fast as their centres move. */
			magnitude = fabs(b);
			if (magnitude < 1e-12)
				return 0;
			s = k / (2.0 * b);
			if (c[2] + s * dr < 0.0)
				return 0;
		} else {
			/* Two roots; the larger one whose radius is not negative, within the extension. */
			discriminant = b * b - a * k;
			if (discriminant < 0.0)
				return 0;
			root = sqrt(discriminant);
			s1 = (b + root) / a;
			s2 = (b - root) / a;
			if (s2 > s1) {
				s = s1;
				s1 = s2;
				s2 = s;
			}

			/* The larger root, unless its radius is negative or it lies past an end that is not extended. */
			s = s1;
			beyond = 0;
			if (c[2] + s * dr < 0.0) {
				beyond = 1;
			} else if (s > 1.0 && !shading->extend[1]) {
				beyond = 1;
			} else if (s < 0.0 && !shading->extend[0]) {
				beyond = 1;
			}

			/* Then the smaller one. */
			if (beyond)
				s = s2;

			/* Neither root has a radius that is not negative. */
			if (c[2] + s * dr < 0.0)
				return 0;
		}
	}

	/* Past either end, the end's colour when extended, else nothing. */
	if (s < 0.0) {
		if (!shading->extend[0])
			return 0;
		s = 0.0;
	}

	/* Past the far end likewise. */
	if (s > 1.0) {
		if (!shading->extend[1])
			return 0;
		s = 1.0;
	}

	/* The parameter within the domain. */
	*t = shading->domain[0] + s * (shading->domain[1] - shading->domain[0]);
	return 1;
}

/* Inverts an affine matrix; one that squashes the plane is refused. */
static int
invert(
	const double matrix[6],
	double inverse[6])
{
	double determinant;
	double magnitude;

	/* The linear part's determinant. */
	determinant = matrix[0] * matrix[3] - matrix[1] * matrix[2];
	magnitude = fabs(determinant);
	if (!(magnitude > 1e-12))
		return EINVAL;

	/* The inverse. */
	inverse[0] = matrix[3] / determinant;
	inverse[1] = -matrix[1] / determinant;
	inverse[2] = -matrix[2] / determinant;
	inverse[3] = matrix[0] / determinant;
	inverse[4] = -(inverse[0] * matrix[4] + inverse[2] * matrix[5]);
	inverse[5] = -(inverse[1] * matrix[4] + inverse[3] * matrix[5]);

	/* Succeeded: inverse undoes matrix. */
	return 0;
}

/* Clamps a colour value to 0..1, NaN to 0. */
static double
clamp_unit(
	double value)
{
	/* Below the range, NaN included. */
	if (!(value > 0.0))
		return 0.0;

	/* Above the range. */
	if (value > 1.0)
		return 1.0;

	/* Within the range. */
	return value;
}
