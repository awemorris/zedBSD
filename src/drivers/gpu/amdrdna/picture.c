/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Matching the firmware's picture with the display chain that shows it.
 *
 * The loader asks the firmware for a mode, and the firmware may meet it
 * without changing the monitor's timing: it fetches a smaller surface and
 * lets the pixel processor's scaler stretch it over the active area, or
 * shows it unscaled in part of it.  The surface the plane fetches (its
 * viewport) is therefore what must match the loader's framebuffer; the
 * timing only says how large the screen is.  The scaler's rectangle and
 * ratios are checked against the viewport and the active area and the
 * result is noted, so the log says how the picture reaches the screen.
 * The scaler may also work the ratios out by itself, or centre the
 * viewport by itself; its programmed ratios then mean nothing.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <uapi/errno.h>

#include "drivers/gpu/amdrdna/amdrdna-hw.h"

/*
 * The scaler's ratio of one: its ratio words hold the source size over the
 * output size with 24 fraction bits (a 3.19 fixed-point number moved up by
 * five bits).
 *
 * Values referenced: Linux v6.19
 * drivers/gpu/drm/amd/display/dc/dpp/dcn10/dcn10_dpp_dscl.c
 * (dpp1_dscl_set_manual_ratio_init).
 */
#define PICTURE_RATIO_SHIFT		24U

/*
 * The scaler's automatic modes: off, working the ratios out from the
 * viewport and the rectangle (with scaling on), and centring the viewport
 * unscaled (with scaling off), with or without repeating pixels.
 *
 * Values referenced: Linux v6.19
 * drivers/gpu/drm/amd/display/dc/dpp/dcn10/dcn10_dpp_dscl.c
 * (enum dscl_autocal_mode).
 */
#define PICTURE_AUTOMATIC_OFF		0U
#define PICTURE_AUTOMATIC_SCALE		1U

/*
 * The driver's own choice: a ratio agrees when it is within 1/1024 of the
 * ratio the viewport and the rectangle give, which leaves room for the
 * rounding of whoever programmed it.
 */
#define PICTURE_RATIO_SLACK_SHIFT	10U

/*
 * The names the log gives the shapes, in the order of
 * enum amdrdna_picture_shape.
 */
static const char *const picture_shape_names[] = {
	"whole",
	"placed",
	"scaled"
};

static bool picture_rectangle_inside(const struct amdrdna_picture_input *input);
static bool picture_ratios_agree(const struct amdrdna_picture_input *input);
static bool picture_ratio_near(uint32_t source, uint32_t output, uint32_t ratio);

/*
 * Checks that the loader's picture is the surface the plane fetches, and
 * finds how that surface reaches the screen.
 *
 * Returns 0 with the shape and the two notes filled, ENOTSUP when the
 * viewport does not start at the surface's origin, or EINVAL when the
 * viewport is empty or the loader's picture is of another size.
 */
int
amdrdna_picture_check(
	const struct amdrdna_picture_input *input,
	struct amdrdna_picture *picture)
{
	bool inside;
	bool agree;

	/* Starts from a surface that fills the screen, with nothing noted. */
	picture->shape = AMDRDNA_PICTURE_WHOLE;
	picture->rectangle_inside = false;
	picture->ratios_agree = false;

	/* A viewport moved into the surface shows another part of it than the loader's picture. */
	if (input->viewport_x != 0U)
		return ENOTSUP;
	if (input->viewport_y != 0U)
		return ENOTSUP;

	/* An empty viewport fetches nothing from the surface. */
	if (input->viewport_width == 0U)
		return EINVAL;
	if (input->viewport_height == 0U)
		return EINVAL;

	/* The loader's picture must be the part of the surface the plane fetches. */
	if (input->loader_present) {
		if (input->loader_width != input->viewport_width)
			return EINVAL;
		if (input->loader_height != input->viewport_height)
			return EINVAL;
	}

	/*
	 * The shape: a viewport of the active area's size fills it pixel for
	 * pixel; one the scaler centres by itself, or one of the scaler's
	 * rectangle's size, sits unscaled inside a larger active area; any
	 * other is stretched by the scaler.
	 */
	if (input->viewport_width == input->active_width &&
	    input->viewport_height == input->active_height) {
		picture->shape = AMDRDNA_PICTURE_WHOLE;
	} else if (input->scaler_automatic > PICTURE_AUTOMATIC_SCALE) {
		picture->shape = AMDRDNA_PICTURE_PLACED;
	} else if (input->viewport_width == input->scaler_width &&
	    input->viewport_height == input->scaler_height) {
		picture->shape = AMDRDNA_PICTURE_PLACED;
	} else {
		picture->shape = AMDRDNA_PICTURE_SCALED;
	}

	/* Notes whether the scaler's rectangle lies inside the active area. */
	inside = picture_rectangle_inside(input);
	picture->rectangle_inside = inside;

	/* Notes whether the scaler's mode and ratios fit the viewport and the rectangle. */
	agree = picture_ratios_agree(input);
	picture->ratios_agree = agree;

	/* Succeeded: the loader's picture is the fetched surface, and its shape is known. */
	return 0;
}

/*
 * Names a picture shape for the log.
 */
const char *
amdrdna_picture_shape_name(
	enum amdrdna_picture_shape shape)
{
	/* A value outside the enumeration has no name. */
	if ((unsigned)shape >= sizeof(picture_shape_names) / sizeof(picture_shape_names[0]))
		return "?";

	/* Succeeded: the shape's name. */
	return picture_shape_names[shape];
}

/* Reports whether the scaler's rectangle is not empty and lies inside the active area. */
static bool
picture_rectangle_inside(
	const struct amdrdna_picture_input *input)
{
	uint64_t right;
	uint64_t bottom;

	/* An empty rectangle shows nothing. */
	if (input->scaler_width == 0U)
		return false;
	if (input->scaler_height == 0U)
		return false;

	/* The rectangle's right and bottom edges must not pass the active area's. */
	right = (uint64_t)input->scaler_x + input->scaler_width;
	bottom = (uint64_t)input->scaler_y + input->scaler_height;
	if (right > input->active_width)
		return false;
	if (bottom > input->active_height)
		return false;

	/* Succeeded: the rectangle is part of the screen. */
	return true;
}

/*
 * Reports whether the scaler's mode and ratios fit the viewport and the
 * rectangle: a scaler that works the ratios out by itself needs scaling
 * on, one that centres the viewport by itself needs it off; otherwise a
 * scaler that passes the pixels through needs a rectangle of the
 * viewport's size, and a scaling one needs ratios of the viewport's size
 * over the rectangle's.
 */
static bool
picture_ratios_agree(
	const struct amdrdna_picture_input *input)
{
	bool horizontal;
	bool vertical;

	/* An empty rectangle gives no ratio to compare. */
	if (input->scaler_width == 0U)
		return false;
	if (input->scaler_height == 0U)
		return false;

	/* A scaler that works the ratios out by itself only has to scale. */
	if (input->scaler_automatic == PICTURE_AUTOMATIC_SCALE) {
		if (input->scaler_mode == 0U)
			return false;

		/* Succeeded: the scaler stretches the viewport over the rectangle by its own ratios. */
		return true;
	}

	/* A scaler that centres the viewport by itself must not scale. */
	if (input->scaler_automatic != PICTURE_AUTOMATIC_OFF) {
		if (input->scaler_mode != 0U)
			return false;

		/* Succeeded: the scaler centres the viewport unscaled. */
		return true;
	}

	/* A scaler that passes the pixels through draws the viewport at its own size. */
	if (input->scaler_mode == 0U) {
		if (input->viewport_width != input->scaler_width)
			return false;
		if (input->viewport_height != input->scaler_height)
			return false;

		/* Succeeded: the pixels pass through at the viewport's size. */
		return true;
	}

	/* The horizontal ratio must be the viewport's width over the rectangle's. */
	horizontal = picture_ratio_near(input->viewport_width, input->scaler_width, input->horizontal_ratio);
	if (!horizontal)
		return false;

	/* The vertical ratio must be the viewport's height over the rectangle's. */
	vertical = picture_ratio_near(input->viewport_height, input->scaler_height, input->vertical_ratio);
	if (!vertical)
		return false;

	/* Succeeded: the scaler stretches the viewport over the rectangle. */
	return true;
}

/* Reports whether a ratio word is close to the source size over the output size. */
static bool
picture_ratio_near(
	uint32_t source,
	uint32_t output,
	uint32_t ratio)
{
	uint64_t expected;
	uint64_t slack;
	uint64_t difference;

	/* The ratio the sizes give, and how far a programmed one may stray from it. */
	expected = ((uint64_t)source << PICTURE_RATIO_SHIFT) / output;
	slack = expected >> PICTURE_RATIO_SLACK_SHIFT;

	/* The distance between the programmed ratio and the expected one. */
	if (ratio > expected) {
		difference = ratio - expected;
	} else {
		difference = expected - ratio;
	}

	/* A ratio farther than the slack scales by another amount. */
	if (difference > slack)
		return false;

	/* Succeeded: the ratio is the sizes' ratio. */
	return true;
}
