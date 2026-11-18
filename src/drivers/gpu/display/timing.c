/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The timings the standards name by size and rate, and the refresh of a
 * timing.  An EDID often names a mode only by its size and rate (an
 * established bit, a standard timing, a CTA-861 video format number); a
 * controller needs the whole timing to show it, and this file supplies it.
 */

#include "drivers/gpu/display/timing.h"

#include <uapi/errno.h>

/*
 * The CVT reduced-blanking (version 1) constants: the horizontal blank,
 * front porch and sync width in pixels, the vertical front porch and the
 * least vertical back porch in lines, the least vertical blank in
 * microseconds, and the pixel clock step in kHz.
 *
 * Values referenced: VESA Coordinated Video Timings Standard, Version 1.2,
 * section 5.4 (the reduced-blanking timing parameters and the pixel clock
 * granularity).
 */
#define REDUCED_H_BLANK			160U
#define REDUCED_H_FRONT			48U
#define REDUCED_H_SYNC			32U
#define REDUCED_V_FRONT			3U
#define REDUCED_V_BACK_MIN		6U
#define REDUCED_V_BLANK_MIN_US		460U
#define REDUCED_CLOCK_STEP_KHZ		250U

/* Picoseconds in a second and in a microsecond. */
#define PICOSECONDS_PER_SECOND		1000000000000ULL
#define PICOSECONDS_PER_MICROSECOND	1000000ULL

/* The widest and tallest timing a 16-bit timing register can hold. */
#define TIMING_TOTAL_MAX		65535U

/*
 * One timing that a standard names by its size and rate: the size, the
 * nominal refresh in hertz, the pixel clock in kHz, the porches and sync
 * widths, and the sync polarities.
 *
 * The tables below are constant; nothing keeps an entry alive but the
 * program image.
 */
struct named_timing {
	uint16_t width;
	uint16_t height;
	uint16_t refresh_hz;
	uint32_t pixel_khz;
	uint16_t h_front;
	uint16_t h_sync;
	uint16_t h_back;
	uint16_t v_front;
	uint16_t v_sync;
	uint16_t v_back;
	uint8_t flags;
};

/*
 * One CTA-861 video format: its number, and its timing.
 *
 * Only the progressive formats a desktop may pick are listed: the 4:3 and
 * 16:9 pairs of one picture share one entry each.
 */
struct format_timing {
	uint8_t format;
	struct named_timing timing;
};

/* The polarity flags of the tables. */
#define BOTH_NEGATIVE			0U
#define H_POSITIVE			GPU_DISPLAY_TIMING_H_POSITIVE
#define V_POSITIVE			GPU_DISPLAY_TIMING_V_POSITIVE
#define BOTH_POSITIVE			(GPU_DISPLAY_TIMING_H_POSITIVE | GPU_DISPLAY_TIMING_V_POSITIVE)

#define MONITOR_TIMING_COUNT		33U

/*
 * The monitor timings an EDID's established and standard timings name.
 *
 * The rows with a reduced blanking (1600x900, 1920x1200, 2048x1152 and
 * 2560x1600 at 60 Hz) are the ones the monitor timing list gives for those
 * sizes; a monitor that names the size in a standard timing takes them.
 *
 * Values referenced: VESA and Industry Standards and Guidelines for
 * Computer Display Monitor Timing (DMT), Version 1.0, Revision 13 (the
 * pixel clocks, porches, sync widths and polarities of each listed mode),
 * and the IBM VGA and Apple Macintosh timings that the EDID's established
 * timings also name (720x400 at 70 and 88 Hz, 640x480 at 67 Hz, 832x624 at
 * 75 Hz, 1152x870 at 75 Hz) as VESA E-EDID Release A, Revision 2, section
 * 3.8 (table 3.18) lists them.
 */
static const struct named_timing monitor_timings[MONITOR_TIMING_COUNT] = {
	{ 640U, 480U, 60U, 25175U, 16U, 96U, 48U, 10U, 2U, 33U, BOTH_NEGATIVE },
	{ 640U, 480U, 67U, 30240U, 64U, 64U, 96U, 3U, 3U, 39U, BOTH_NEGATIVE },
	{ 640U, 480U, 72U, 31500U, 24U, 40U, 128U, 9U, 3U, 28U, BOTH_NEGATIVE },
	{ 640U, 480U, 75U, 31500U, 16U, 64U, 120U, 1U, 3U, 16U, BOTH_NEGATIVE },
	{ 720U, 400U, 70U, 28322U, 18U, 108U, 54U, 12U, 2U, 35U, V_POSITIVE },
	{ 720U, 400U, 88U, 35500U, 18U, 108U, 54U, 12U, 2U, 35U, V_POSITIVE },
	{ 800U, 600U, 56U, 36000U, 24U, 72U, 128U, 1U, 2U, 22U, BOTH_POSITIVE },
	{ 800U, 600U, 60U, 40000U, 40U, 128U, 88U, 1U, 4U, 23U, BOTH_POSITIVE },
	{ 800U, 600U, 72U, 50000U, 56U, 120U, 64U, 37U, 6U, 23U, BOTH_POSITIVE },
	{ 800U, 600U, 75U, 49500U, 16U, 80U, 160U, 1U, 3U, 21U, BOTH_POSITIVE },
	{ 832U, 624U, 75U, 57284U, 32U, 64U, 224U, 1U, 3U, 39U, BOTH_NEGATIVE },
	{ 1024U, 768U, 60U, 65000U, 24U, 136U, 160U, 3U, 6U, 29U, BOTH_NEGATIVE },
	{ 1024U, 768U, 70U, 75000U, 24U, 136U, 144U, 3U, 6U, 29U, BOTH_NEGATIVE },
	{ 1024U, 768U, 75U, 78750U, 16U, 96U, 176U, 1U, 3U, 28U, BOTH_POSITIVE },
	{ 1152U, 864U, 75U, 108000U, 64U, 128U, 256U, 1U, 3U, 32U, BOTH_POSITIVE },
	{ 1152U, 870U, 75U, 100000U, 32U, 128U, 144U, 3U, 3U, 39U, BOTH_NEGATIVE },
	{ 1280U, 720U, 60U, 74250U, 110U, 40U, 220U, 5U, 5U, 20U, BOTH_POSITIVE },
	{ 1280U, 768U, 60U, 79500U, 64U, 128U, 192U, 3U, 7U, 20U, V_POSITIVE },
	{ 1280U, 800U, 60U, 83500U, 72U, 128U, 200U, 3U, 6U, 22U, V_POSITIVE },
	{ 1280U, 960U, 60U, 108000U, 96U, 112U, 312U, 1U, 3U, 36U, BOTH_POSITIVE },
	{ 1280U, 1024U, 60U, 108000U, 48U, 112U, 248U, 1U, 3U, 38U, BOTH_POSITIVE },
	{ 1280U, 1024U, 75U, 135000U, 16U, 144U, 248U, 1U, 3U, 38U, BOTH_POSITIVE },
	{ 1360U, 768U, 60U, 85500U, 64U, 112U, 256U, 3U, 6U, 18U, BOTH_POSITIVE },
	{ 1366U, 768U, 60U, 85500U, 70U, 143U, 213U, 3U, 3U, 24U, BOTH_POSITIVE },
	{ 1400U, 1050U, 60U, 121750U, 88U, 144U, 232U, 3U, 4U, 32U, V_POSITIVE },
	{ 1440U, 900U, 60U, 106500U, 80U, 152U, 232U, 3U, 6U, 25U, V_POSITIVE },
	{ 1600U, 900U, 60U, 108000U, 24U, 80U, 96U, 1U, 3U, 96U, BOTH_POSITIVE },
	{ 1600U, 1200U, 60U, 162000U, 64U, 192U, 304U, 1U, 3U, 46U, BOTH_POSITIVE },
	{ 1680U, 1050U, 60U, 146250U, 104U, 176U, 280U, 3U, 6U, 30U, V_POSITIVE },
	{ 1920U, 1080U, 60U, 148500U, 88U, 44U, 148U, 4U, 5U, 36U, BOTH_POSITIVE },
	{ 1920U, 1200U, 60U, 154000U, 48U, 32U, 80U, 3U, 6U, 26U, H_POSITIVE },
	{ 2048U, 1152U, 60U, 162000U, 26U, 80U, 96U, 1U, 3U, 44U, BOTH_POSITIVE },
	{ 2560U, 1600U, 60U, 268500U, 48U, 32U, 80U, 3U, 6U, 37U, H_POSITIVE }
};

#define FORMAT_TIMING_COUNT		19U

/*
 * The CTA-861 video formats by number.
 *
 * Values referenced: CTA-861-G, section 4 (tables 1 and 2, the video
 * format timings: the totals, porches, sync widths, polarities and pixel
 * clocks of formats 1 to 4, 16 to 19, 31 to 34, 63, 64 and 93 to 97).
 */
static const struct format_timing format_timings[FORMAT_TIMING_COUNT] = {
	{ 1U, { 640U, 480U, 60U, 25175U, 16U, 96U, 48U, 10U, 2U, 33U, BOTH_NEGATIVE } },
	{ 2U, { 720U, 480U, 60U, 27000U, 16U, 62U, 60U, 9U, 6U, 30U, BOTH_NEGATIVE } },
	{ 3U, { 720U, 480U, 60U, 27000U, 16U, 62U, 60U, 9U, 6U, 30U, BOTH_NEGATIVE } },
	{ 4U, { 1280U, 720U, 60U, 74250U, 110U, 40U, 220U, 5U, 5U, 20U, BOTH_POSITIVE } },
	{ 16U, { 1920U, 1080U, 60U, 148500U, 88U, 44U, 148U, 4U, 5U, 36U, BOTH_POSITIVE } },
	{ 17U, { 720U, 576U, 50U, 27000U, 12U, 64U, 68U, 5U, 5U, 39U, BOTH_NEGATIVE } },
	{ 18U, { 720U, 576U, 50U, 27000U, 12U, 64U, 68U, 5U, 5U, 39U, BOTH_NEGATIVE } },
	{ 19U, { 1280U, 720U, 50U, 74250U, 440U, 40U, 220U, 5U, 5U, 20U, BOTH_POSITIVE } },
	{ 31U, { 1920U, 1080U, 50U, 148500U, 528U, 44U, 148U, 4U, 5U, 36U, BOTH_POSITIVE } },
	{ 32U, { 1920U, 1080U, 24U, 74250U, 638U, 44U, 148U, 4U, 5U, 36U, BOTH_POSITIVE } },
	{ 33U, { 1920U, 1080U, 25U, 74250U, 528U, 44U, 148U, 4U, 5U, 36U, BOTH_POSITIVE } },
	{ 34U, { 1920U, 1080U, 30U, 74250U, 88U, 44U, 148U, 4U, 5U, 36U, BOTH_POSITIVE } },
	{ 63U, { 1920U, 1080U, 120U, 297000U, 88U, 44U, 148U, 4U, 5U, 36U, BOTH_POSITIVE } },
	{ 64U, { 1920U, 1080U, 100U, 297000U, 528U, 44U, 148U, 4U, 5U, 36U, BOTH_POSITIVE } },
	{ 93U, { 3840U, 2160U, 24U, 297000U, 1276U, 88U, 296U, 8U, 10U, 72U, BOTH_POSITIVE } },
	{ 94U, { 3840U, 2160U, 25U, 297000U, 1056U, 88U, 296U, 8U, 10U, 72U, BOTH_POSITIVE } },
	{ 95U, { 3840U, 2160U, 30U, 297000U, 176U, 88U, 296U, 8U, 10U, 72U, BOTH_POSITIVE } },
	{ 96U, { 3840U, 2160U, 50U, 594000U, 1056U, 88U, 296U, 8U, 10U, 72U, BOTH_POSITIVE } },
	{ 97U, { 3840U, 2160U, 60U, 594000U, 176U, 88U, 296U, 8U, 10U, 72U, BOTH_POSITIVE } }
};

static void timing_from_named(const struct named_timing *named, struct gpu_display_timing *timing);
static uint32_t timing_reduced_vsync(uint32_t width, uint32_t height);

/*
 * Computes the refresh of a timing, in millihertz, rounded to the nearest.
 *
 * Returns 0 for a timing with no clock or no totals.
 */
uint32_t
gpu_display_timing_refresh(
	const struct gpu_display_timing *timing)
{
	uint64_t h_total;
	uint64_t v_total;
	uint64_t pixels;
	uint64_t refresh;

	/* The pixels of one whole frame, blanking included. */
	h_total = (uint64_t)timing->h_active + timing->h_front + timing->h_sync + timing->h_back;
	v_total = (uint64_t)timing->v_active + timing->v_front + timing->v_sync + timing->v_back;
	pixels = h_total * v_total;

	/* A timing without a frame or a clock has no refresh. */
	if (pixels == 0U)
		return 0U;
	if (timing->pixel_khz == 0U)
		return 0U;

	/* The frames the clock draws in a second, in millihertz. */
	refresh = ((uint64_t)timing->pixel_khz * 1000000U + pixels / 2U) / pixels;

	/* A refresh beyond 32 bits is no display's; it reads as none. */
	if (refresh > 0xffffffffULL)
		return 0U;

	/* Reports the refresh. */
	return (uint32_t)refresh;
}

/*
 * Tells whether two refreshes, in millihertz, are the same rate: closer than
 * GPU_DISPLAY_TIMING_SAME_MHZ.
 *
 * Returns 1 when they are, 0 when they are not.
 */
int
gpu_display_timing_same_rate(
	uint32_t first_mhz,
	uint32_t second_mhz)
{
	uint32_t difference;

	/* The distance between the two rates, whichever is higher. */
	difference = second_mhz - first_mhz;
	if (first_mhz > second_mhz)
		difference = first_mhz - second_mhz;

	/* A distance of a twentieth of a hertz or more is another rate. */
	if (difference >= GPU_DISPLAY_TIMING_SAME_MHZ)
		return 0;

	/* Reports the same rate. */
	return 1;
}

/*
 * Looks up the monitor timing (VESA DMT, or an established timing's IBM or
 * Apple mode) of a size and a nominal refresh in hertz.
 *
 * Returns 0 with timing filled (source 0, for the caller to set), or ENOENT
 * when no listed timing has that size and rate.
 */
int
gpu_display_timing_monitor(
	uint32_t width,
	uint32_t height,
	uint32_t refresh_hz,
	struct gpu_display_timing *timing)
{
	uint32_t index;

	/* Finds the row of the size and rate. */
	for (index = 0U; index < MONITOR_TIMING_COUNT; index++) {
		/* Another size or rate is another row. */
		if (monitor_timings[index].width != width)
			continue;
		if (monitor_timings[index].height != height)
			continue;
		if (monitor_timings[index].refresh_hz != refresh_hz)
			continue;

		/* Copies the row out. */
		timing_from_named(&monitor_timings[index], timing);

		/* Succeeded: the timing is the listed one. */
		return 0;
	}

	/* No listed timing has the size and rate. */
	return ENOENT;
}

/*
 * Looks up the timing of a CTA-861 video format number.
 *
 * Returns 0 with timing filled (source 0, for the caller to set), or ENOENT
 * for a format this file does not list (an interlaced one, one wider than
 * a desktop picks, or an unknown number).
 */
int
gpu_display_timing_format(
	uint32_t format,
	struct gpu_display_timing *timing)
{
	uint32_t index;

	/* Finds the row of the format. */
	for (index = 0U; index < FORMAT_TIMING_COUNT; index++) {
		/* Another number is another row. */
		if (format_timings[index].format != format)
			continue;

		/* Copies the row out. */
		timing_from_named(&format_timings[index].timing, timing);

		/* Succeeded: the timing is the format's. */
		return 0;
	}

	/* The format is not listed. */
	return ENOENT;
}

/*
 * Computes the CVT reduced-blanking (version 1) timing of a size and a
 * refresh in hertz.
 *
 * The horizontal blank is fixed; the vertical blank is the least number of
 * whole lines that lasts 460 microseconds, plus one, and at least the front
 * porch, the sync and the least back porch; the pixel clock is rounded down
 * to its 0.25 MHz step.  The sync is positive horizontally and negative
 * vertically, and the vertical sync width names the aspect ratio.
 *
 * Returns 0 with timing filled (source 0, for the caller to set), or EINVAL
 * for a size or rate the formula cannot serve.
 *
 * Procedure referenced: VESA Coordinated Video Timings Standard, Version
 * 1.2, section 5.4 (the reduced-blanking computation) and section 3.4.2
 * (table 3-2, the vertical sync width of each aspect ratio).
 */
int
gpu_display_timing_reduced(
	uint32_t width,
	uint32_t height,
	uint32_t refresh_hz,
	struct gpu_display_timing *timing)
{
	uint64_t frame_ps;
	uint64_t blank_ps;
	uint64_t line_ps;
	uint64_t clock_khz;
	uint32_t v_sync;
	uint32_t v_blank;
	uint32_t v_blank_min;
	uint32_t h_total;
	uint32_t v_total;

	/* Refuses a size or rate the formula has nothing to work with. */
	if (width == 0U || height == 0U)
		return EINVAL;
	if (refresh_hz == 0U)
		return EINVAL;

	/* The frame time, which must outlast the least vertical blank. */
	frame_ps = PICOSECONDS_PER_SECOND / refresh_hz;
	blank_ps = (uint64_t)REDUCED_V_BLANK_MIN_US * PICOSECONDS_PER_MICROSECOND;
	if (frame_ps <= blank_ps)
		return EINVAL;

	/* The estimated line time: the frame less the blank, over the active lines. */
	line_ps = (frame_ps - blank_ps) / height;
	if (line_ps == 0U)
		return EINVAL;

	/* The vertical blank: the whole lines of 460 us plus one, at least porch, sync and back porch. */
	v_sync = timing_reduced_vsync(width, height);
	v_blank = (uint32_t)(blank_ps / line_ps) + 1U;
	v_blank_min = REDUCED_V_FRONT + v_sync + REDUCED_V_BACK_MIN;
	if (v_blank < v_blank_min)
		v_blank = v_blank_min;

	/* The totals, and the pixel clock rounded down to its step. */
	h_total = width + REDUCED_H_BLANK;
	v_total = height + v_blank;
	clock_khz = (uint64_t)refresh_hz * h_total * v_total / 1000U;
	clock_khz = clock_khz / REDUCED_CLOCK_STEP_KHZ * REDUCED_CLOCK_STEP_KHZ;

	/* Refuses a timing no timing register or clock field can hold. */
	if (h_total > TIMING_TOTAL_MAX || v_total > TIMING_TOTAL_MAX)
		return EINVAL;
	if (clock_khz == 0U || clock_khz > 0xffffffffULL)
		return EINVAL;

	/* The line: the active pixels, then the fixed porch, sync and back porch. */
	timing->pixel_khz = (uint32_t)clock_khz;
	timing->h_active = width;
	timing->h_front = REDUCED_H_FRONT;
	timing->h_sync = REDUCED_H_SYNC;
	timing->h_back = REDUCED_H_BLANK - REDUCED_H_FRONT - REDUCED_H_SYNC;

	/* The frame: the active lines, then the porch, the aspect's sync and the rest of the blank. */
	timing->v_active = height;
	timing->v_front = REDUCED_V_FRONT;
	timing->v_sync = v_sync;
	timing->v_back = v_blank - REDUCED_V_FRONT - v_sync;

	/* The polarities, the caller's source and the resulting refresh. */
	timing->flags = GPU_DISPLAY_TIMING_H_POSITIVE;
	timing->source = 0U;
	timing->refresh_mhz = gpu_display_timing_refresh(timing);

	/* Succeeded: the timing is computed. */
	return 0;
}

/* Copies a table row into a timing and computes its refresh. */
static void
timing_from_named(
	const struct named_timing *named,
	struct gpu_display_timing *timing)
{
	/* The line. */
	timing->pixel_khz = named->pixel_khz;
	timing->h_active = named->width;
	timing->h_front = named->h_front;
	timing->h_sync = named->h_sync;
	timing->h_back = named->h_back;

	/* The frame. */
	timing->v_active = named->height;
	timing->v_front = named->v_front;
	timing->v_sync = named->v_sync;
	timing->v_back = named->v_back;

	/* The polarities, the caller's source and the resulting refresh. */
	timing->flags = named->flags;
	timing->source = 0U;
	timing->refresh_mhz = gpu_display_timing_refresh(timing);
}

/*
 * Reports the CVT vertical sync width that names a size's aspect ratio.
 *
 * Values referenced: VESA Coordinated Video Timings Standard, Version 1.2,
 * section 3.4.2 (table 3-2: 4 lines for 4:3, 5 for 16:9, 6 for 16:10, 7 for
 * 5:4 and 15:9, 10 for any other ratio).
 */
static uint32_t
timing_reduced_vsync(
	uint32_t width,
	uint32_t height)
{
	/* A 4:3 picture. */
	if (width * 3U == height * 4U)
		return 4U;

	/* A 16:9 picture. */
	if (width * 9U == height * 16U)
		return 5U;

	/* A 16:10 picture. */
	if (width * 10U == height * 16U)
		return 6U;

	/* A 5:4 or a 15:9 picture. */
	if (width * 4U == height * 5U)
		return 7U;
	if (width * 9U == height * 15U)
		return 7U;

	/* Any other picture. */
	return 10U;
}
