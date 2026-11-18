/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The reading of the test pattern in a captured frame: where its parts lie, the average colour of a part,
 * the YUV matrix and range a frame was encoded with, the patches' error
 * against what the pattern drew, and the frame counter.
 *
 * The parts are given in fractions of the frame, the same fractions the
 * pattern draws with, so the capture device's scaling does not matter.
 * Nothing here touches a device; the host test runs it on made frames.
 */

#ifndef USERLAND_TESTS_CAMCHECK_ANALYZE_H
#define USERLAND_TESTS_CAMCHECK_ANALYZE_H

#include <stddef.h>
#include <stdint.h>

/* The bands of the pattern, as fractions of the frame's height. */
#define CAMCHECK_PATCH_TOP		0.04
#define CAMCHECK_PATCH_BOTTOM		0.30
#define CAMCHECK_COUNTER_TOP		0.58
#define CAMCHECK_COUNTER_BOTTOM		0.78

/* The patches, left to right, and the counter's squares (the most significant at the left). */
#define CAMCHECK_PATCHES		9U
#define CAMCHECK_COUNTER_BITS		16U

/* The patches by place. */
#define CAMCHECK_PATCH_RED		0U
#define CAMCHECK_PATCH_GREEN		1U
#define CAMCHECK_PATCH_BLUE		2U
#define CAMCHECK_PATCH_WHITE		3U
#define CAMCHECK_PATCH_BLACK		4U
#define CAMCHECK_PATCH_GREY		5U
#define CAMCHECK_PATCH_YELLOW		6U
#define CAMCHECK_PATCH_CYAN		7U
#define CAMCHECK_PATCH_MAGENTA		8U

/* The YUV matrices and ranges a frame may use (camcommon's numbers). */
#define CAMCHECK_MATRIX_BT601		1U
#define CAMCHECK_MATRIX_BT709		2U
#define CAMCHECK_RANGE_LIMITED		1U
#define CAMCHECK_RANGE_FULL		2U

/* The counter's increments told apart: 0, 1, 2, and 3 or more. */
#define CAMCHECK_INCREMENT_KINDS	4U

/*
 * A part of the frame, in fractions of its width and height (left, top,
 * right, bottom).
 */
struct camcheck_region {
	double left;
	double top;
	double right;
	double bottom;
};

/*
 * The average of three samples over a region: Y, Cb and Cr of a YUV
 * frame, or R, G and B of an RGB one, each 0 to 255.
 */
struct camcheck_sample {
	double value[3];
};

/*
 * What the frame counter did over a run: the last value read, whether one
 * was read, the times it went down (a failure), and how many times it went
 * up by 0, 1, 2 and 3 or more.
 */
struct camcheck_counter_run {
	uint32_t last;
	int started;
	unsigned decreases;
	unsigned increments[CAMCHECK_INCREMENT_KINDS];
};

/*
 * Gives the inner half of a patch (index 0 to 8), away from its edges.
 */
void camcheck_patch_region(unsigned index, struct camcheck_region *region);

/*
 * Gives the inner part of a counter square (bit 0 to 15, 0 the most significant).
 */
void camcheck_counter_region(unsigned bit, struct camcheck_region *region);

/*
 * Averages Y, Cb and Cr of a YUYV frame over a region.
 */
int camcheck_sample_yuyv(const uint8_t *frame, uint32_t width, uint32_t height, uint32_t stride, const struct camcheck_region *region, struct camcheck_sample *sample);

/*
 * Averages R, G and B of a packed RGB frame over a region.
 */
int camcheck_sample_rgb(const uint8_t *rgb, uint32_t width, uint32_t height, const struct camcheck_region *region, struct camcheck_sample *sample);

/*
 * Gives the Y, Cb and Cr an RGB colour has in one matrix and range.
 */
void camcheck_rgb_to_yuv(const double *rgb, uint32_t matrix, uint32_t range, double *yuv);

/*
 * Gives the RGB colour of Y, Cb and Cr in one matrix and range.
 */
void camcheck_yuv_to_rgb(const double *yuv, uint32_t matrix, uint32_t range, double *rgb);

/*
 * Guesses the matrix and range of a YUV frame from its patches.
 */
void camcheck_estimate(const struct camcheck_sample *patches, uint32_t *matrix, uint32_t *range);

/*
 * Gives the RGB colour the pattern draws a patch with.
 */
void camcheck_patch_expected(unsigned index, double *rgb);

/*
 * Reports the largest difference of any patch's colour from what the pattern drew.
 */
double camcheck_patch_error(const struct camcheck_sample *rgb_patches, unsigned *worst);

/*
 * Reads the counter from its squares' brightness between black and white.
 */
uint32_t camcheck_counter_decode(const double *brightness, double black, double white);

/*
 * Adds one counter value to a run.
 */
void camcheck_counter_add(struct camcheck_counter_run *run, uint32_t value);

#endif
