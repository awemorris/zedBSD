/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The colors of linear, radial and conic gradients and their ramps.
 *
 * The arithmetic is done in double precision and rounded to single
 * precision where a value is handed back, so the CPU reference is at least
 * as accurate as the single precision shader it is compared with.
 */

#include "vector/vec-gradient.h"

#include <errno.h>
#include <math.h>

/* The number of channels of a color in the arrays of this file. */
#define CHANNEL_COUNT		4

/* The index of the alpha channel in a channel array (the others follow: red, green, blue). */
#define CHANNEL_ALPHA		0

/*
 * The largest magnitude a parameter or an offset handed back may have.
 * A double beyond the range of a float is not converted (that is
 * undefined in C), so a value is held to this first.
 */
#define VALUE_LIMIT		1e30

/*
 * Two gradient-line ends or two points closer than this, in gradient
 * space, are the same point (the limit that vec-gradient.h states as
 * 1e-6 for the same two points and for the centre of a conic gradient).
 */
#define POINT_SAME		1e-6

/*
 * The largest |a| / (cd.cd + dr^2) at which the quadratic of a radial
 * gradient is solved as a linear equation (the limit vec-gradient.h
 * states as 1e-6).
 */
#define QUADRATIC_FLAT		1e-6

/* The smallest |b| at which the linear equation of a radial gradient has a root (vec-gradient.h states 1e-20). */
#define LINEAR_DIVISOR_MIN	1e-20

/* The relative size under which two circles of a radial gradient are the same circle (vec-gradient.h states 1e-12). */
#define CIRCLE_SAME		1e-12

/* The full turn of a conic gradient, in radians, in double precision. */
#define TURN			6.283185307179586

/* Forward declarations of the static functions. */
static float gradient_to_float(double value);
static int gradient_stops_usable(const struct vec_stop *stops, size_t count);
static double gradient_clamp_unit(double value);
static unsigned int gradient_round_byte(double value);
static void gradient_stop_channels(const struct vec_stop *stop, int premultiplied, double *channels);
static void gradient_stop_premultiplied(const struct vec_stop *stop, int premultiplied, double *channels);
static uint32_t gradient_pack(const double *channels, int premultiplied);
static int gradient_hint_usable(const struct vec_stop *stop);
static double gradient_hint_exponent(const struct vec_stop *stop);
static double gradient_hint_weight(const struct vec_stop *stop, double fraction);
static double gradient_hint_mean(const struct vec_stop *stop);
static double gradient_hint_mean_square(const struct vec_stop *stop);
static int gradient_line_is_point(const struct vec_gradient *gradient);
static int gradient_numbers_finite(const struct vec_gradient *gradient);
static double gradient_parameter_linear(const struct vec_gradient *gradient, double x, double y, int *painted);
static double gradient_parameter_radial(const struct vec_gradient *gradient, double x, double y, int *painted);
static double gradient_parameter_conic(const struct vec_gradient *gradient, double x, double y, int *painted);

/*
 * Fixes up CSS color stops and transition hints.
 *
 * Values referenced: CSS Images 3 section 3.4.3 and CSS Images 4 section
 * 3.5.3 (the three fixup steps, applied in order), and CSS Images 4
 * section 3.5.2 (a hint's place as a fraction of the way between its
 * two stops).
 */
int
vec_gradient_fixup_css(
	struct vec_stop *stops,
	size_t *count)
{
	size_t total;
	size_t i;
	size_t j;
	size_t out_count;
	size_t before;
	size_t after;
	double largest;
	double low;
	double high;
	double position;
	double fraction;
	int finite;

	/* Refuses a missing array or a count the model does not take. */
	if (stops == NULL || count == NULL)
		return EINVAL;
	total = *count;
	if (total < 1 || total > VEC_GRADIENT_STOPS_MAX)
		return EINVAL;

	/* Checks every entry before anything is changed, so a refusal leaves the entries as they were. */
	for (i = 0; i < total; i++) {
		if (stops[i].is_hint) {
			/* A hint stands between two stops: it is neither first nor last nor next to another hint. */
			if (i == 0 || i == total - 1)
				return EINVAL;
			if (stops[i - 1].is_hint)
				return EINVAL;

			/* A hint always has a position. */
			if (!stops[i].has_offset)
				return EINVAL;
		}

		/* A position that is given is a finite number. */
		if (stops[i].has_offset || stops[i].is_hint) {
			finite = vec_finite(stops[i].offset);
			if (!finite)
				return EINVAL;
		}
	}

	/* Fixup step 1: the first stop without a position is at 0, the last at 1. */
	if (!stops[0].has_offset) {
		stops[0].offset = 0.0f;
		stops[0].has_offset = 1;
	}
	if (!stops[total - 1].has_offset) {
		stops[total - 1].offset = 1.0f;
		stops[total - 1].has_offset = 1;
	}

	/*
	 * Fixup step 2: a position before that of any earlier entry is moved
	 * up to the largest earlier one.
	 */
	largest = (double)stops[0].offset;
	for (i = 0; i < total; i++) {
		if (!stops[i].has_offset)
			continue;
		if ((double)stops[i].offset < largest)
			stops[i].offset = (float)largest;
		largest = (double)stops[i].offset;
	}

	/*
	 * Fixup step 3: each run of stops without positions is spread evenly
	 * between the entries with positions before and after it.
	 */
	i = 0;
	while (i < total) {
		/* Skips an entry that has its position. */
		if (stops[i].has_offset) {
			i++;
			continue;
		}

		/* Finds the end of the run; the last entry has a position, so the run ends inside the list. */
		j = i;
		while (j < total && !stops[j].has_offset)
			j++;
		before = i - 1;
		after = j;
		low = (double)stops[before].offset;
		high = (double)stops[after].offset;

		/* Places each stop of the run on the line from the entry before it to the entry after it. */
		while (i < j) {
			fraction = (double)(i - before) / (double)(after - before);
			position = low + (high - low) * fraction;
			stops[i].offset = (float)position;
			stops[i].has_offset = 1;
			i++;
		}
	}

	/* Removes the hints, folding each into the stop before it as a fraction of the way to the next stop. */
	out_count = 0;
	for (i = 0; i < total; i++) {
		if (stops[i].is_hint) {
			low = (double)stops[out_count - 1].offset;
			high = (double)stops[i + 1].offset;
			position = (double)stops[i].offset;

			/* Two stops at one position are a hard transition, which no hint bends. */
			if (high > low) {
				fraction = gradient_clamp_unit((position - low) / (high - low));
				stops[out_count - 1].has_hint = 1;
				stops[out_count - 1].hint = (float)fraction;
			} else {
				stops[out_count - 1].has_hint = 0;
				stops[out_count - 1].hint = 0.0f;
			}
			continue;
		}

		/* Keeps the stop, with no hint until one follows it. */
		stops[out_count] = stops[i];
		stops[out_count].has_offset = 1;
		stops[out_count].is_hint = 0;
		stops[out_count].has_hint = 0;
		stops[out_count].hint = 0.0f;
		out_count++;
	}

	/* Reports the number of stops that remain. */
	*count = out_count;

	/* Succeeded: the stops are in order with every position set and no hint entries left. */
	return 0;
}

/*
 * Fixes up SVG gradient stops.
 *
 * Values referenced: SVG 2 section 14.2.4.1 (the offset clamped to
 * 0..1) and section 14.2.4.3 (an offset less than the one before it
 * takes that one's value).
 */
int
vec_gradient_fixup_svg(
	struct vec_stop *stops,
	size_t count)
{
	size_t i;
	double largest;
	double offset;
	int finite;

	/* Refuses a missing array or a count the model does not take. */
	if (stops == NULL)
		return EINVAL;
	if (count < 1 || count > VEC_GRADIENT_STOPS_MAX)
		return EINVAL;

	/* Checks every offset before anything is changed. */
	for (i = 0; i < count; i++) {
		finite = vec_finite(stops[i].offset);
		if (!finite)
			return EINVAL;
	}

	/* Clamps each offset to 0..1 and lifts it to the largest one before it. */
	largest = 0.0;
	for (i = 0; i < count; i++) {
		offset = gradient_clamp_unit((double)stops[i].offset);
		if (offset < largest)
			offset = largest;
		largest = offset;
		stops[i].offset = (float)offset;
		stops[i].has_offset = 1;
		stops[i].is_hint = 0;
		stops[i].has_hint = 0;
		stops[i].hint = 0.0f;
	}

	/* Succeeded: the offsets are in 0..1 and in order. */
	return 0;
}

/*
 * Maps stops that reach outside 0..1 onto it.
 *
 * Values referenced: CSS Images 3 section 3.1 and section 3.2 (the color
 * of the first stop before the line's start and of the last after its
 * end, which the extended line carries).
 */
int
vec_gradient_extend_span(
	struct vec_stop *stops,
	size_t count,
	float *start,
	float *length)
{
	size_t i;
	double low;
	double high;
	double offset;

	/* Refuses missing arguments or a count the model does not take. */
	if (stops == NULL || start == NULL || length == NULL)
		return EINVAL;
	if (count < 1 || count > VEC_GRADIENT_STOPS_MAX)
		return EINVAL;

	/* Finds the range the stops cover together with 0..1. */
	low = (double)stops[0].offset;
	high = (double)stops[count - 1].offset;
	if (low > 0.0)
		low = 0.0;
	if (high < 1.0)
		high = 1.0;

	/* Leaves stops that are inside 0..1 as they are. */
	if (low == 0.0 && high == 1.0) {
		*start = 0.0f;
		*length = 1.0f;
		return 0;
	}

	/* Rescales every offset so that the covered range becomes 0..1. */
	for (i = 0; i < count; i++) {
		offset = ((double)stops[i].offset - low) / (high - low);
		stops[i].offset = (float)gradient_clamp_unit(offset);
	}

	/* Reports where the covered range starts and how long it is in the old units. */
	*start = gradient_to_float(low);
	*length = gradient_to_float(high - low);

	/* Succeeded: the stops now span 0..1 of the extended line. */
	return 0;
}

/*
 * Maps the stops of a repeating gradient onto 0..1.
 *
 * Values referenced: CSS Images 3 section 3.3 (the repetition's length is
 * the distance between the first and last stop; none is a degenerate
 * gradient painted with the average color).
 */
int
vec_gradient_repeat_span(
	struct vec_stop *stops,
	size_t count,
	float *start,
	float *length)
{
	size_t i;
	double first;
	double last;
	double span;
	double offset;

	/* Refuses missing arguments or a count the model does not take. */
	if (stops == NULL || start == NULL || length == NULL)
		return EINVAL;
	if (count < 1 || count > VEC_GRADIENT_STOPS_MAX)
		return EINVAL;

	/* Leaves a gradient whose first and last stop do not differ to the caller's average color. */
	first = (double)stops[0].offset;
	last = (double)stops[count - 1].offset;
	span = last - first;
	if (!(span > 0.0))
		return ENOTSUP;

	/* Rescales every offset so that one repetition becomes 0..1. */
	for (i = 0; i < count; i++) {
		offset = ((double)stops[i].offset - first) / span;
		stops[i].offset = (float)gradient_clamp_unit(offset);
	}

	/* Reports where one repetition starts and how long it is in the old units. */
	*start = gradient_to_float(first);
	*length = gradient_to_float(span);

	/* Succeeded: the stops now span one repetition. */
	return 0;
}

/*
 * Reports the color at a position on the gradient line.
 *
 * Values referenced: CSS Images 3 section 3.4.2 (the premultiplied
 * interpolation and the hint weight P^(log_H 0.5)), CSS Images 4 section
 * 3.5.2, and SVG 2 section 14.2.1 (the straight interpolation).
 */
uint32_t
vec_gradient_color_at(
	const struct vec_stop *stops,
	size_t count,
	int premultiplied,
	float position)
{
	size_t i;
	size_t found;
	size_t next;
	double where;
	double span;
	double fraction;
	double weight;
	double first[CHANNEL_COUNT];
	double second[CHANNEL_COUNT];
	double blended[CHANNEL_COUNT];
	uint32_t color;
	int usable;

	/* Takes no color from stops the model does not take. */
	usable = gradient_stops_usable(stops, count);
	if (!usable)
		return 0;

	/* Clamps the position to the line (a NaN is taken as the start). */
	where = (double)position;
	where = gradient_clamp_unit(where);

	/* Finds the last stop at or before the position; a hard stop's later color wins at its position. */
	found = count;
	for (i = count; i > 0; i--) {
		if (stops[i - 1].offset <= where) {
			found = i - 1;
			break;
		}
	}

	/* Before the first stop and after the last the color is the nearest stop's. */
	if (found == count || found == count - 1) {
		if (found == count)
			found = 0;
		gradient_stop_channels(&stops[found], premultiplied, first);
		color = gradient_pack(first, premultiplied);
		return color;
	}

	/* Finds how far the position is from the stop to the next one. */
	next = found + 1;
	span = (double)stops[next].offset - (double)stops[found].offset;
	fraction = 0.0;
	if (span > 0.0)
		fraction = (where - (double)stops[found].offset) / span;
	fraction = gradient_clamp_unit(fraction);

	/* Weights the second color, by the hint when there is one. */
	weight = gradient_hint_weight(&stops[found], fraction);

	/* Blends the two stops' channels linearly with that weight. */
	gradient_stop_channels(&stops[found], premultiplied, first);
	gradient_stop_channels(&stops[next], premultiplied, second);
	for (i = 0; i < CHANNEL_COUNT; i++)
		blended[i] = first[i] + (second[i] - first[i]) * weight;

	/* Rounds to the premultiplied 8 bit color. */
	color = gradient_pack(blended, premultiplied);

	/* Reports the blended color. */
	return color;
}

/*
 * Reports the average color of the stops over 0..1.
 *
 * Values referenced: CSS Images 3 section 3.3 (the average color: each
 * pair of stops weighted by the line it colors, and the degenerate
 * repeating gradient respaced evenly).  The weight of a pair with a
 * hint at H is P^k with k = log_H 0.5, whose mean over the pair is
 * 1 / (k + 1) and mean square 1 / (2k + 1).
 */
uint32_t
vec_gradient_average(
	const struct vec_stop *stops,
	size_t count,
	int premultiplied)
{
	size_t i;
	int channel;
	double offsets[VEC_GRADIENT_STOPS_MAX];
	double sums[CHANNEL_COUNT];
	double first[CHANNEL_COUNT];
	double second[CHANNEL_COUNT];
	double length;
	double mean;
	double mean_square;
	double pair_mean;
	uint32_t color;
	int usable;

	/* Takes no color from stops the model does not take. */
	usable = gradient_stops_usable(stops, count);
	if (!usable)
		return 0;

	/* A single stop is its own average. */
	if (count == 1) {
		gradient_stop_channels(&stops[0], premultiplied, first);
		color = gradient_pack(first, premultiplied);
		return color;
	}

	/* Reads the offsets, held to 0..1 (the average is over that range). */
	for (i = 0; i < count; i++) {
		offsets[i] = gradient_clamp_unit((double)stops[i].offset);
	}

	/* Respaces stops that all lie at one position evenly over 0..1, keeping their colors and order. */
	if (!(offsets[count - 1] > offsets[0])) {
		for (i = 0; i < count; i++) {
			offsets[i] = (double)i / (double)(count - 1);
		}
	}

	/* Starts the sums of the premultiplied channels. */
	for (channel = 0; channel < CHANNEL_COUNT; channel++)
		sums[channel] = 0.0;

	/* Adds each pair of stops, weighted by the length of line it colors. */
	for (i = 0; i + 1 < count; i++) {
		length = offsets[i + 1] - offsets[i];
		if (!(length > 0.0))
			continue;

		/* Takes the pair's channels and the mean weights of its blend. */
		gradient_stop_channels(&stops[i], premultiplied, first);
		gradient_stop_channels(&stops[i + 1], premultiplied, second);
		mean = gradient_hint_mean(&stops[i]);
		mean_square = gradient_hint_mean_square(&stops[i]);

		/* The alpha channel is linear in the weight in both interpolations. */
		pair_mean = first[CHANNEL_ALPHA] + (second[CHANNEL_ALPHA] - first[CHANNEL_ALPHA]) * mean;
		sums[CHANNEL_ALPHA] += length * pair_mean;

		/* A premultiplied color channel is linear in the weight; a straight one is premultiplied afterwards, so it is quadratic. */
		for (channel = 1; channel < CHANNEL_COUNT; channel++) {
			if (premultiplied) {
				pair_mean = first[channel] + (second[channel] - first[channel]) * mean;
			} else {
				pair_mean = first[channel] * first[CHANNEL_ALPHA];
				pair_mean += (first[channel] * (second[CHANNEL_ALPHA] - first[CHANNEL_ALPHA]) +
				    (second[channel] - first[channel]) * first[CHANNEL_ALPHA]) * mean;
				pair_mean += (second[channel] - first[channel]) * (second[CHANNEL_ALPHA] - first[CHANNEL_ALPHA]) * mean_square;
				pair_mean = pair_mean / 255.0;
			}
			sums[channel] += length * pair_mean;
		}
	}

	/* The line before the first stop has the first stop's color. */
	if (offsets[0] > 0.0) {
		gradient_stop_premultiplied(&stops[0], premultiplied, first);
		for (channel = 0; channel < CHANNEL_COUNT; channel++)
			sums[channel] += offsets[0] * first[channel];
	}

	/* The line after the last stop has the last stop's color. */
	if (offsets[count - 1] < 1.0) {
		gradient_stop_premultiplied(&stops[count - 1], premultiplied, first);
		for (channel = 0; channel < CHANNEL_COUNT; channel++)
			sums[channel] += (1.0 - offsets[count - 1]) * first[channel];
	}

	/* Rounds the premultiplied sums to an 8 bit color. */
	color = gradient_pack(sums, 1);

	/* Reports the average color. */
	return color;
}

/*
 * Chooses a ramp's width for a gradient line of a length in device pixels.
 */
int
vec_gradient_ramp_width(
	float length)
{
	int width;
	int finite;

	/* A length that is not a number gets the widest ramp. */
	finite = vec_finite(length);
	if (!finite)
		return VEC_RAMP_WIDTH_MAX;

	/* Clamps the ends before the conversion to an integer. */
	if (length >= (float)VEC_RAMP_WIDTH_MAX)
		return VEC_RAMP_WIDTH_MAX;
	if (length <= (float)VEC_RAMP_WIDTH_MIN)
		return VEC_RAMP_WIDTH_MIN;

	/* Rounds the length up to a whole number of texels. */
	width = (int)length;
	if ((float)width < length)
		width++;

	/* Keeps the width inside the limits. */
	if (width < VEC_RAMP_WIDTH_MIN)
		width = VEC_RAMP_WIDTH_MIN;
	if (width > VEC_RAMP_WIDTH_MAX)
		width = VEC_RAMP_WIDTH_MAX;

	/* Reports the width. */
	return width;
}

/*
 * Fills a ramp with the color at the middle of each texel.
 */
int
vec_gradient_ramp(
	const struct vec_gradient *gradient,
	int width,
	uint32_t *ramp)
{
	int i;
	int usable;
	float position;

	/* Refuses missing arguments and a width the ramp texture does not take. */
	if (gradient == NULL || ramp == NULL)
		return EINVAL;
	if (width < VEC_RAMP_WIDTH_MIN || width > VEC_RAMP_WIDTH_MAX)
		return EINVAL;

	/* Refuses stops the model does not take. */
	usable = gradient_stops_usable(gradient->stops, gradient->count);
	if (!usable)
		return EINVAL;

	/* Fills texel i with the color at (i + 0.5) / width. */
	for (i = 0; i < width; i++) {
		position = ((float)i + 0.5f) / (float)width;
		ramp[i] = vec_gradient_color_at(gradient->stops, gradient->count, gradient->premultiplied, position);
	}

	/* Succeeded: every texel of the ramp is written. */
	return 0;
}

/*
 * Computes a point's parameter t before the spread.
 *
 * Values referenced: SVG 2 section 14.2.2 (the linear projection) and
 * 14.2.2.2 (a line of no length), HTML Standard 4.12.5.1.10
 * (createRadialGradient: the largest w with a positive radius) and SVG 2
 * section 14.2.3.2, CSS Images 4 section 3.3 (the conic angle, clockwise
 * from up).
 */
float
vec_gradient_parameter(
	const struct vec_gradient *gradient,
	float x,
	float y,
	int *painted)
{
	struct vec_point point;
	struct vec_point mapped;
	double parameter;
	int unused_flag;
	int valid;
	int finite_x;
	int finite_y;

	/* Lets the caller leave out the painted flag. */
	if (painted == NULL)
		painted = &unused_flag;
	*painted = 0;

	/* Paints nothing for a missing gradient. */
	if (gradient == NULL)
		return 0.0f;

	/* Paints nothing at a point that is not a number. */
	finite_x = vec_finite(x);
	finite_y = vec_finite(y);
	if (!finite_x || !finite_y)
		return 0.0f;

	/* Paints nothing for a matrix that cannot be used or numbers that are not finite. */
	valid = vec_matrix_valid(&gradient->to_gradient);
	if (!valid)
		return 0.0f;
	valid = gradient_numbers_finite(gradient);
	if (!valid)
		return 0.0f;

	/* Maps the point into gradient space; a point that overflowed lies far outside any gradient this model takes. */
	point.x = x;
	point.y = y;
	mapped = vec_matrix_apply(&gradient->to_gradient, point);
	finite_x = vec_finite(mapped.x);
	finite_y = vec_finite(mapped.y);
	if (!finite_x || !finite_y)
		return 0.0f;

	/* Computes t by the gradient's kind. */
	parameter = 0.0;
	if (gradient->kind == VEC_GRADIENT_LINEAR) {
		parameter = gradient_parameter_linear(gradient, (double)mapped.x, (double)mapped.y, painted);
	} else if (gradient->kind == VEC_GRADIENT_RADIAL) {
		parameter = gradient_parameter_radial(gradient, (double)mapped.x, (double)mapped.y, painted);
	} else if (gradient->kind == VEC_GRADIENT_CONIC) {
		parameter = gradient_parameter_conic(gradient, (double)mapped.x, (double)mapped.y, painted);
	}

	/* Reports t as a single precision number. */
	return gradient_to_float(parameter);
}

/*
 * Applies a spread method to t.
 *
 * Values referenced: SVG 2 section 14.2.2.1 (spreadMethod pad, reflect
 * and repeat).
 */
float
vec_gradient_spread(
	enum vec_spread spread,
	float t)
{
	double value;
	double reduced;
	int finite;

	/* A t that is not finite gives 0. */
	finite = vec_finite(t);
	if (!finite)
		return 0.0f;
	value = (double)t;

	/* Reflects: the pattern runs forward over 0..1 and backward over 1..2. */
	if (spread == VEC_SPREAD_REFLECT) {
		reduced = value - 2.0 * floor(value / 2.0);
		if (reduced > 1.0)
			reduced = 2.0 - reduced;
	} else if (spread == VEC_SPREAD_REPEAT) {
		/* Repeats: the fractional part of t. */
		reduced = value - floor(value);
	} else {
		/* Pads: t held to 0..1 below. */
		reduced = value;
	}

	/* Holds the result to 0..1 (a fractional part can round up to 1). */
	reduced = gradient_clamp_unit(reduced);

	/* Reports the spread t. */
	return (float)reduced;
}

/*
 * Reports the color a renderer paints at a point from a ramp.
 */
uint32_t
vec_gradient_sample(
	const struct vec_gradient *gradient,
	const uint32_t *ramp,
	int width,
	float x,
	float y)
{
	int painted;
	int point_line;
	int index;
	float t;
	float spread_t;
	float scaled;

	/* Paints nothing without a gradient or a ramp the model takes. */
	if (gradient == NULL || ramp == NULL)
		return 0;
	if (width < VEC_RAMP_WIDTH_MIN || width > VEC_RAMP_WIDTH_MAX)
		return 0;

	/* Computes t, and paints nothing where the gradient does not reach. */
	painted = 0;
	t = vec_gradient_parameter(gradient, x, y, &painted);
	if (!painted)
		return 0;

	/* Applies the spread, except for a line of no length, whose color is always the last stop's. */
	point_line = gradient_line_is_point(gradient);
	if (point_line) {
		spread_t = 1.0f;
	} else {
		spread_t = vec_gradient_spread(gradient->spread, t);
	}

	/* Finds the ramp texel; t = 1 takes the last one. */
	scaled = spread_t * (float)width;
	index = (int)scaled;
	if (index < 0)
		index = 0;
	if (index > width - 1)
		index = width - 1;

	/* Reports the texel's premultiplied color. */
	return ramp[index];
}

/*
 * Reports the length over which t runs from 0 to 1, in device pixels.
 */
float
vec_gradient_line_length(
	const struct vec_gradient *gradient,
	const struct vec_rect *area)
{
	struct vec_matrix inverse;
	struct vec_point corner;
	struct vec_point mapped;
	double stretch;
	double length;
	double distance;
	double largest;
	double dx;
	double dy;
	double radius;
	int error;
	int valid;
	int finite_x;
	int finite_y;
	int i;

	/* A missing gradient has no length. */
	if (gradient == NULL)
		return 0.0f;

	/* A gradient with numbers that are not finite has no length. */
	valid = gradient_numbers_finite(gradient);
	if (!valid)
		return 0.0f;

	/* The stretch from gradient units to device pixels is that of the inverse matrix. */
	error = vec_matrix_invert(&inverse, &gradient->to_gradient);
	if (error != 0)
		return 0.0f;
	stretch = (double)vec_matrix_scale_max(&inverse);

	/* Measures the gradient line in gradient units by the kind. */
	length = 0.0;
	if (gradient->kind == VEC_GRADIENT_LINEAR) {
		dx = (double)gradient->end.x - (double)gradient->start.x;
		dy = (double)gradient->end.y - (double)gradient->start.y;
		length = sqrt(dx * dx + dy * dy);
	} else if (gradient->kind == VEC_GRADIENT_RADIAL) {
		dx = (double)gradient->center.x - (double)gradient->focal.x;
		dy = (double)gradient->center.y - (double)gradient->focal.y;
		radius = (double)gradient->radius;
		if (radius < 0.0)
			radius = 0.0;
		length = sqrt(dx * dx + dy * dy) + radius;
	} else if (gradient->kind == VEC_GRADIENT_CONIC) {
		/* A full turn at the farthest corner of the painted area, which needs the area. */
		if (area == NULL)
			return 0.0f;
		largest = 0.0;
		for (i = 0; i < 4; i++) {
			corner.x = area->left;
			corner.y = area->top;
			if (i == 1 || i == 3)
				corner.x = area->right;
			if (i == 2 || i == 3)
				corner.y = area->bottom;
			finite_x = vec_finite(corner.x);
			finite_y = vec_finite(corner.y);
			if (!finite_x || !finite_y)
				return 0.0f;
			mapped = vec_matrix_apply(&gradient->to_gradient, corner);
			dx = (double)mapped.x - (double)gradient->center.x;
			dy = (double)mapped.y - (double)gradient->center.y;
			distance = sqrt(dx * dx + dy * dy);
			if (distance > largest)
				largest = distance;
		}
		length = TURN * largest;
	}

	/* Converts to device pixels, giving 0 for anything that is not a usable length. */
	length = length * stretch;
	if (length != length || length < 0.0)
		return 0.0f;

	/* Reports the length. */
	return gradient_to_float(length);
}

/*
 * Converts a double to a float, held to a range a float takes.
 */
static float
gradient_to_float(
	double value)
{
	/* A NaN becomes 0. */
	if (value != value)
		return 0.0f;

	/* A value beyond the limit is held to it, so the conversion is defined. */
	if (value > VALUE_LIMIT)
		return (float)VALUE_LIMIT;
	if (value < -VALUE_LIMIT)
		return (float)-VALUE_LIMIT;

	/* A value inside the limit converts as it is. */
	return (float)value;
}

/*
 * Tells whether stops are an array the model takes.
 */
static int
gradient_stops_usable(
	const struct vec_stop *stops,
	size_t count)
{
	/* The model takes 1 to the maximum number of stops. */
	if (stops == NULL)
		return 0;
	if (count < 1 || count > VEC_GRADIENT_STOPS_MAX)
		return 0;

	/* Usable. */
	return 1;
}

/*
 * Holds a number to 0..1 (a NaN becomes 0).
 */
static double
gradient_clamp_unit(
	double value)
{
	/* A NaN becomes 0. */
	if (value != value)
		return 0.0;

	/* Holds the ends. */
	if (value < 0.0)
		return 0.0;
	if (value > 1.0)
		return 1.0;

	/* A number inside 0..1 stays. */
	return value;
}

/*
 * Rounds a channel value to the nearest of 0 to 255.
 */
static unsigned int
gradient_round_byte(
	double value)
{
	/* Holds the ends, which also keeps the conversion defined. */
	if (!(value > 0.0))
		return 0;
	if (value >= 255.0)
		return 255;

	/* Rounds to the nearest whole number, halves up. */
	return (unsigned int)(value + 0.5);
}

/*
 * Splits a stop's color into the channels of the interpolation space:
 * alpha, red, green and blue, each 0 to 255; premultiplied for CSS,
 * straight for SVG.
 */
static void
gradient_stop_channels(
	const struct vec_stop *stop,
	int premultiplied,
	double *channels)
{
	uint32_t color;
	double alpha;
	int channel;

	/* Splits the 0xAARRGGBB color into its four bytes. */
	color = stop->color;
	alpha = (double)((color >> 24) & 0xffU);
	channels[CHANNEL_ALPHA] = alpha;
	channels[1] = (double)((color >> 16) & 0xffU);
	channels[2] = (double)((color >> 8) & 0xffU);
	channels[3] = (double)(color & 0xffU);

	/* Premultiplies the color channels when the interpolation takes place premultiplied. */
	if (premultiplied) {
		for (channel = 1; channel < CHANNEL_COUNT; channel++)
			channels[channel] = channels[channel] * alpha / 255.0;
	}
}

/*
 * Splits a stop's color into premultiplied channels whatever the
 * interpolation space is.
 */
static void
gradient_stop_premultiplied(
	const struct vec_stop *stop,
	int premultiplied,
	double *channels)
{
	double alpha;
	int channel;

	/* Takes the channels of the interpolation space. */
	gradient_stop_channels(stop, premultiplied, channels);

	/* Premultiplies them when they are straight. */
	if (!premultiplied) {
		alpha = channels[CHANNEL_ALPHA];
		for (channel = 1; channel < CHANNEL_COUNT; channel++)
			channels[channel] = channels[channel] * alpha / 255.0;
	}
}

/*
 * Rounds channels of the interpolation space to a premultiplied 8 bit
 * 0xAARRGGBB color, no color channel above alpha.
 */
static uint32_t
gradient_pack(
	const double *channels,
	int premultiplied)
{
	unsigned int alpha_byte;
	unsigned int color_byte;
	double alpha;
	double color_value;
	uint32_t color;
	int channel;

	/* Rounds alpha, held to 0..255. */
	alpha = channels[CHANNEL_ALPHA];
	if (!(alpha > 0.0))
		alpha = 0.0;
	if (alpha > 255.0)
		alpha = 255.0;
	alpha_byte = gradient_round_byte(alpha);
	color = (uint32_t)alpha_byte << 24;

	/* Premultiplies (for straight channels) and rounds each color channel, none above alpha. */
	for (channel = 1; channel < CHANNEL_COUNT; channel++) {
		color_value = channels[channel];
		if (!premultiplied)
			color_value = color_value * alpha / 255.0;
		color_byte = gradient_round_byte(color_value);
		if (color_byte > alpha_byte)
			color_byte = alpha_byte;
		color |= (uint32_t)color_byte << (8 * (3 - channel));
	}

	/* Reports the packed color. */
	return color;
}

/*
 * Tells whether the stop carries a transition hint that is a number
 * strictly inside 0..1 (a hint at either end is a hard transition, which
 * the callers treat as its own case).
 */
static int
gradient_hint_usable(
	const struct vec_stop *stop)
{
	int finite;

	/* A stop without a hint has none. */
	if (!stop->has_hint)
		return 0;

	/* A hint that is not a number is ignored. */
	finite = vec_finite(stop->hint);
	if (!finite)
		return 0;

	/* Usable. */
	return 1;
}

/*
 * Reports k = log_H 0.5 = ln 0.5 / ln H of a hint strictly inside 0..1
 * (CSS Images 4 section 3.5.2), so that the weight is P^k.
 */
static double
gradient_hint_exponent(
	const struct vec_stop *stop)
{
	double hint;
	double exponent;

	/* Divides the two logarithms. */
	hint = (double)stop->hint;
	exponent = log(0.5) / log(hint);

	/* Reports k. */
	return exponent;
}

/*
 * Reports the weight C of the next stop's color at the fraction P of
 * the way from this stop to the next, by CSS Images 4 section 3.5.2:
 * C = P^(log_H 0.5), and P without a hint.
 */
static double
gradient_hint_weight(
	const struct vec_stop *stop,
	double fraction)
{
	double hint;
	double exponent;
	double weight;
	int usable;

	/* A fraction at the ends has the end's weight whatever the hint. */
	if (fraction <= 0.0)
		return 0.0;
	if (fraction >= 1.0)
		return 1.0;

	/* Without a hint, and for a hint halfway, the blend is linear. */
	usable = gradient_hint_usable(stop);
	if (!usable)
		return fraction;
	hint = (double)stop->hint;
	if (hint == 0.5)
		return fraction;

	/* A hint on a stop makes a hard transition: the limits of the formula. */
	if (hint <= 0.0)
		return 1.0;
	if (hint >= 1.0)
		return 0.0;

	/* Applies C = P^k. */
	exponent = gradient_hint_exponent(stop);
	weight = pow(fraction, exponent);

	/* Reports the weight. */
	return weight;
}

/*
 * Reports the mean of the weight C over a pair of stops (the mean of
 * P^k over P in 0..1 is 1 / (k + 1)).
 */
static double
gradient_hint_mean(
	const struct vec_stop *stop)
{
	double hint;
	double exponent;
	int usable;

	/* Without a hint the weight is P, whose mean is a half. */
	usable = gradient_hint_usable(stop);
	if (!usable)
		return 0.5;
	hint = (double)stop->hint;

	/* A hint on a stop is a hard transition: all the next color, or all the first. */
	if (hint <= 0.0)
		return 1.0;
	if (hint >= 1.0)
		return 0.0;

	/* Integrates P^k. */
	exponent = gradient_hint_exponent(stop);

	/* Reports the mean weight. */
	return 1.0 / (exponent + 1.0);
}

/*
 * Reports the mean of the square of the weight C over a pair of stops
 * (the mean of P^(2k) is 1 / (2k + 1)).
 */
static double
gradient_hint_mean_square(
	const struct vec_stop *stop)
{
	double hint;
	double exponent;
	int usable;

	/* Without a hint the weight is P, whose mean square is a third. */
	usable = gradient_hint_usable(stop);
	if (!usable)
		return 1.0 / 3.0;
	hint = (double)stop->hint;

	/* A hint on a stop is a hard transition. */
	if (hint <= 0.0)
		return 1.0;
	if (hint >= 1.0)
		return 0.0;

	/* Integrates P^(2k). */
	exponent = gradient_hint_exponent(stop);

	/* Reports the mean square weight. */
	return 1.0 / (2.0 * exponent + 1.0);
}

/*
 * Tells whether a linear gradient has its start and end at the same
 * point (closer than POINT_SAME), where every point has t = 1.
 */
static int
gradient_line_is_point(
	const struct vec_gradient *gradient)
{
	double dx;
	double dy;
	double distance;

	/* Only a linear gradient has a line. */
	if (gradient->kind != VEC_GRADIENT_LINEAR)
		return 0;

	/* Measures the distance between the two ends. */
	dx = (double)gradient->end.x - (double)gradient->start.x;
	dy = (double)gradient->end.y - (double)gradient->start.y;
	distance = sqrt(dx * dx + dy * dy);
	if (distance < POINT_SAME)
		return 1;

	/* The ends differ. */
	return 0;
}

/*
 * Tells whether the numbers a gradient's kind uses are finite.
 */
static int
gradient_numbers_finite(
	const struct vec_gradient *gradient)
{
	int a;
	int b;
	int c;
	int d;
	int e;
	int f;

	/* Checks the numbers of the kind. */
	if (gradient->kind == VEC_GRADIENT_LINEAR) {
		a = vec_finite(gradient->start.x);
		b = vec_finite(gradient->start.y);
		c = vec_finite(gradient->end.x);
		d = vec_finite(gradient->end.y);
		if (!a || !b || !c || !d)
			return 0;
	} else if (gradient->kind == VEC_GRADIENT_RADIAL) {
		a = vec_finite(gradient->focal.x);
		b = vec_finite(gradient->focal.y);
		c = vec_finite(gradient->center.x);
		d = vec_finite(gradient->center.y);
		e = vec_finite(gradient->focal_radius);
		f = vec_finite(gradient->radius);
		if (!a || !b || !c || !d || !e || !f)
			return 0;
	} else if (gradient->kind == VEC_GRADIENT_CONIC) {
		a = vec_finite(gradient->center.x);
		b = vec_finite(gradient->center.y);
		c = vec_finite(gradient->start_angle);
		if (!a || !b || !c)
			return 0;
	} else {
		/* A kind this model does not have paints nothing. */
		return 0;
	}

	/* Finite. */
	return 1;
}

/*
 * Computes t of a linear gradient: the projection on the line from start
 * to end (SVG 2 section 14.2.2), and 1 for a line of no length (SVG 2
 * section 14.2.2.2).
 */
static double
gradient_parameter_linear(
	const struct vec_gradient *gradient,
	double x,
	double y,
	int *painted)
{
	double dx;
	double dy;
	double length_squared;
	double along;
	int point_line;

	/* Every point of a line of no length has t = 1. */
	*painted = 1;
	point_line = gradient_line_is_point(gradient);
	if (point_line)
		return 1.0;

	/* Projects the point on the line, as a fraction of its length. */
	dx = (double)gradient->end.x - (double)gradient->start.x;
	dy = (double)gradient->end.y - (double)gradient->start.y;
	length_squared = dx * dx + dy * dy;
	along = (x - (double)gradient->start.x) * dx + (y - (double)gradient->start.y) * dy;

	/* Reports t. */
	return along / length_squared;
}

/*
 * Computes t of a radial gradient: the largest w for which the point is
 * on the circle at focal + w (center - focal) of radius
 * focal_radius + w (radius - focal_radius), where that radius is
 * positive (HTML Standard 4.12.5.1.10, SVG 2 section 14.2.3.2).
 */
static double
gradient_parameter_radial(
	const struct vec_gradient *gradient,
	double x,
	double y,
	int *painted)
{
	double center_x;
	double center_y;
	double point_x;
	double point_y;
	double focal_radius;
	double radius_change;
	double center_squared;
	double scale;
	double a;
	double b;
	double c;
	double discriminant;
	double q;
	double roots[2];
	double best;
	double radius_at;
	int root_count;
	int found;
	int i;

	/* A negative radius paints nothing. */
	*painted = 0;
	focal_radius = (double)gradient->focal_radius;
	if (focal_radius < 0.0 || gradient->radius < 0.0f)
		return 0.0;

	/* Sets up the vectors of the quadratic. */
	center_x = (double)gradient->center.x - (double)gradient->focal.x;
	center_y = (double)gradient->center.y - (double)gradient->focal.y;
	point_x = x - (double)gradient->focal.x;
	point_y = y - (double)gradient->focal.y;
	radius_change = (double)gradient->radius - focal_radius;
	center_squared = center_x * center_x + center_y * center_y;
	scale = center_squared + radius_change * radius_change;

	/* Two circles that are the same paint nothing. */
	if (scale < CIRCLE_SAME * ((double)gradient->radius * (double)gradient->radius + 1.0))
		return 0.0;

	/* Forms a w^2 - 2 b w + c = 0. */
	a = center_squared - radius_change * radius_change;
	b = point_x * center_x + point_y * center_y + focal_radius * radius_change;
	c = point_x * point_x + point_y * point_y - focal_radius * focal_radius;

	/* Solves it: as a line when a is small against the circles' size, else by the stable pair of roots. */
	root_count = 0;
	roots[0] = 0.0;
	roots[1] = 0.0;
	if (fabs(a) <= QUADRATIC_FLAT * scale) {
		if (fabs(b) >= LINEAR_DIVISOR_MIN) {
			roots[0] = c / (2.0 * b);
			root_count = 1;
		}
	} else {
		discriminant = b * b - a * c;
		if (discriminant >= 0.0) {
			q = sqrt(discriminant);
			if (b < 0.0)
				q = -q;
			q = b + q;
			roots[0] = q / a;
			root_count = 1;
			if (q != 0.0) {
				roots[1] = c / q;
				root_count = 2;
			}
		}
	}

	/* Takes the largest root whose circle has a positive radius. */
	found = 0;
	best = 0.0;
	for (i = 0; i < root_count; i++) {
		radius_at = focal_radius + roots[i] * radius_change;
		if (!(radius_at > 0.0))
			continue;
		if (!found || roots[i] > best) {
			best = roots[i];
			found = 1;
		}
	}

	/* A point on no circle of the cone is not painted. */
	if (!found)
		return 0.0;

	/* Reports t of the circle. */
	*painted = 1;
	return best;
}

/*
 * Computes t of a conic gradient: the angle about the center, clockwise
 * from up, less the start angle, over a full turn, in 0..1 (CSS Images 4
 * section 3.3).
 */
static double
gradient_parameter_conic(
	const struct vec_gradient *gradient,
	double x,
	double y,
	int *painted)
{
	double dx;
	double dy;
	double angle;
	double turns;
	float angle_x;
	float angle_y;

	/* Every point of a conic gradient is painted. */
	*painted = 1;

	/* The center itself has t = 0. */
	dx = x - (double)gradient->center.x;
	dy = y - (double)gradient->center.y;
	if (fabs(dx) < POINT_SAME && fabs(dy) < POINT_SAME)
		return 0.0;

	/* Finds the angle from up, growing clockwise on the screen, by the shared arctangent. */
	angle_y = gradient_to_float(dx);
	angle_x = gradient_to_float(-dy);
	angle = (double)vec_atan2(angle_y, angle_x);

	/* Takes the start angle off and reduces to one turn, 0 up to but not including 1. */
	turns = (angle - (double)gradient->start_angle) / TURN;
	turns = turns - floor(turns);
	if (turns >= 1.0)
		turns = 0.0;

	/* Reports t. */
	return turns;
}
