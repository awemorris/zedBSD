/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The display modes the VisionFive 2's HDMI output is set to without the
 * loader, and the register values each one takes.
 *
 * Five modes are known, each with a pixel clock the HDMI transmitter's
 * PLL table covers.  The boot line's display.mode= picks one; without it
 * the output runs at 1920x1080 at 60 Hz.  From a mode come the display
 * controller's four timing words and the HDMI transmitter's writes: its
 * PLLs, its copy of the timing, and its drive settings.
 */

#include <stddef.h>
#include <stdint.h>

#include "drivers/gpu/powervr/display/dc8200-mode.h"
#include <uapi/errno.h>

/* The boot line's word that names a mode. */
#define MODE_WORD		"display.mode="

/* The largest width, height or refresh rate the boot line's mode may name. */
#define MODE_NUMBER_MAX		16384U

/* The refresh rate a mode written without one has. */
#define MODE_REFRESH_DEFAULT	60U

/* The mode the output runs at when the boot line names none: 1920x1080 at 60 Hz. */
#define MODE_DEFAULT_INDEX	4U

/*
 * The display controller's timing words: the visible count in the low
 * 15 bits and the total from bit 16; the sync start in the low 15 bits,
 * its end from bit 15, the sync enable at bit 30 and a low pulse at
 * bit 31.
 *
 * Values referenced:
 * Linux torvalds/linux master drivers/gpu/drm/verisilicon/vs_crtc_regs.h
 * (VSDC_DISP_HSIZE_TOTAL at bit 16, VSDC_DISP_HSYNC_END at bit 15,
 * VSDC_DISP_HSYNC_EN bit 30, VSDC_DISP_HSYNC_POL bit 31, the same for the
 * vertical words).
 */
#define TIMING_TOTAL_SHIFT	16U
#define TIMING_SYNC_END_SHIFT	15U
#define TIMING_SYNC_ENABLE	(1U << 30)
#define TIMING_SYNC_LOW		(1U << 31)

/*
 * The HDMI transmitter's registers for the PLLs, the timing and the
 * drive, by register number.
 *
 * Values referenced:
 * StarFive Linux JH7110_VisionFive2_6.12.y_devel 4cecf169f38e
 * drivers/gpu/drm/verisilicon/inno_hdmi.c (inno_hdmi_config_pll: the
 * pre-PLL at 0x1a0 to 0x1a6 and its fraction at 0x1d1 to 0x1d3, the
 * post-PLL at 0x1aa to 0x1ad; inno_hdmi_improve_eye_diagram: 0x1bf and
 * 0x1c0) and inno_hdmi.h (HDMI_VIDEO_TIMING_CTL 0x08,
 * HDMI_VIDEO_EXT_HTOTAL_L 0x09 to HDMI_VIDEO_EXT_VDURATION 0x15, the
 * timing control's external-timing bit 0, horizontal polarity bit 2 and
 * vertical polarity bit 3).
 */
#define HDMI_PRE_PLL_POWER		0x1a0U
#define HDMI_PRE_PLL_DIVIDER		0x1a1U
#define HDMI_PRE_PLL_FEEDBACK_HIGH	0x1a2U
#define HDMI_PRE_PLL_FEEDBACK_LOW	0x1a3U
#define HDMI_PRE_PLL_TMDS		0x1a4U
#define HDMI_PRE_PLL_PIXEL_AB		0x1a5U
#define HDMI_PRE_PLL_PIXEL_CD		0x1a6U
#define HDMI_POST_PLL_POWER		0x1aaU
#define HDMI_POST_PLL_DIVIDER		0x1abU
#define HDMI_POST_PLL_FEEDBACK		0x1acU
#define HDMI_POST_PLL_OUTPUT		0x1adU
#define HDMI_PRE_PLL_FRACTION_HIGH	0x1d1U
#define HDMI_PRE_PLL_FRACTION_MIDDLE	0x1d2U
#define HDMI_PRE_PLL_FRACTION_LOW	0x1d3U
#define HDMI_DRIVE_PRE_EMPHASIS		0x1bfU
#define HDMI_DRIVE_SWING		0x1c0U
#define HDMI_TIMING_CONTROL		0x08U
#define HDMI_TIMING_HTOTAL_LOW		0x09U
#define HDMI_TIMING_HTOTAL_HIGH		0x0aU
#define HDMI_TIMING_HBLANK_LOW		0x0bU
#define HDMI_TIMING_HBLANK_HIGH		0x0cU
#define HDMI_TIMING_HDELAY_LOW		0x0dU
#define HDMI_TIMING_HDELAY_HIGH		0x0eU
#define HDMI_TIMING_HWIDTH_LOW		0x0fU
#define HDMI_TIMING_HWIDTH_HIGH		0x10U
#define HDMI_TIMING_VTOTAL_LOW		0x11U
#define HDMI_TIMING_VTOTAL_HIGH		0x12U
#define HDMI_TIMING_VBLANK		0x13U
#define HDMI_TIMING_VDELAY		0x14U
#define HDMI_TIMING_VWIDTH		0x15U
#define HDMI_TIMING_EXTERNAL		(1U << 0)
#define HDMI_TIMING_HSYNC_HIGH		(1U << 2)
#define HDMI_TIMING_VSYNC_HIGH		(1U << 3)

/*
 * The pre-PLL's power word while it is set up and after, the post-PLL's
 * power word while it is set up, and after with and without its output
 * divider, and the top bits of the feedback's high word with and without
 * the fraction.
 *
 * Values referenced:
 * StarFive Linux JH7110_VisionFive2_6.12.y_devel 4cecf169f38e
 * drivers/gpu/drm/verisilicon/inno_hdmi.c (inno_hdmi_config_pll: 0x1a0
 * written 1 then 0, 0x1aa written 0x0f then 0x0e or 0x02, 0x1a2's top
 * bits 0xf0, or 0xc0 with a fraction).
 */
#define HDMI_PRE_PLL_DOWN		0x01U
#define HDMI_PRE_PLL_UP			0x00U
#define HDMI_POST_PLL_DOWN		0x0fU
#define HDMI_POST_PLL_UP_DIVIDED	0x0eU
#define HDMI_POST_PLL_UP_DIRECT		0x02U
#define HDMI_FEEDBACK_INTEGER		0xf0U
#define HDMI_FEEDBACK_FRACTIONAL	0xc0U

/*
 * The five modes, in the order of their pixel clocks.
 *
 * Values referenced:
 * CTA-861 and VESA DMT timings as Linux v6.12
 * drivers/gpu/drm/drm_edid.c lists them (edid_cea_modes_1: VIC 1
 * 640x480, VIC 4 1280x720, VIC 16 1920x1080; drm_dmt_modes: 800x600 and
 * 1024x768 at 60 Hz), with the clocks StarFive Linux
 * JH7110_VisionFive2_6.12.y_devel 4cecf169f38e
 * drivers/gpu/drm/verisilicon/inno_hdmi.c's PLL table covers.
 */
static const struct powervr_dc_mode mode_table[] = {
	{ 640U, 480U, 60U, 25175U, 656U, 752U, 800U, 490U, 492U, 525U, 0U, 0U, 1U },
	{ 800U, 600U, 60U, 40000U, 840U, 968U, 1056U, 601U, 605U, 628U, 1U, 1U, 0U },
	{ 1024U, 768U, 60U, 65000U, 1048U, 1184U, 1344U, 771U, 777U, 806U, 0U, 0U, 0U },
	{ 1280U, 720U, 60U, 74250U, 1390U, 1430U, 1650U, 725U, 730U, 750U, 1U, 1U, 4U },
	{ 1920U, 1080U, 60U, 148500U, 2008U, 2052U, 2200U, 1084U, 1089U, 1125U, 1U, 1U, 16U }
};

/*
 * The HDMI transmitter's PLL settings for the five modes' clocks.
 *
 * Values referenced:
 * StarFive Linux JH7110_VisionFive2_6.12.y_devel 4cecf169f38e
 * drivers/gpu/drm/verisilicon/inno_hdmi.c (pre_pll_cfg_table: the rows
 * for 25175000, 40000000, 65000000, 74250000 and 148500000;
 * post_pll_cfg_table: the first row whose clock is not below the pixel
 * clock, 25200000 for the first and 49000000 to 148500000 for the
 * others) and inno_hdmi.h (struct pre_pll_config and post_pll_config:
 * the order of the fields).
 */
static const struct powervr_dc_pll pll_table[] = {
	{ 25175U, 1U, 100U, 2U, 3U, 3U, 12U, 3U, 3U, 4U, 0xf55555U, 1U, 80U, 13U, 3U },
	{ 40000U, 1U, 80U, 2U, 2U, 2U, 12U, 2U, 2U, 2U, 0U, 1U, 20U, 1U, 3U },
	{ 65000U, 1U, 65U, 2U, 1U, 1U, 12U, 0U, 1U, 1U, 0U, 1U, 20U, 1U, 3U },
	{ 74250U, 1U, 99U, 1U, 2U, 2U, 1U, 2U, 3U, 4U, 0U, 1U, 20U, 1U, 3U },
	{ 148500U, 1U, 99U, 1U, 1U, 1U, 1U, 2U, 2U, 2U, 0U, 1U, 20U, 1U, 3U }
};

static int read_number(const char *text, size_t length, size_t *position, uint32_t *value);
static void add_write(struct powervr_dc_hdmi_write *writes, unsigned *count, uint32_t reg, uint32_t value);

/*
 * Returns the mode the output runs at when the boot line names none.
 */
const struct powervr_dc_mode *
powervr_dc_mode_default(
	void)
{
	/* 1920x1080 at 60 Hz. */
	return &mode_table[MODE_DEFAULT_INDEX];
}

/*
 * Finds a known mode by its size and refresh rate; ENOENT when none
 * matches.
 */
int
powervr_dc_mode_find(
	uint32_t width,
	uint32_t height,
	uint32_t refresh,
	const struct powervr_dc_mode **mode)
{
	const struct powervr_dc_mode *candidate;
	unsigned index;

	/* Starts with none found. */
	*mode = NULL;

	/* Walks the table for the size and the rate. */
	for (index = 0; index < sizeof(mode_table) / sizeof(mode_table[0]); index++) {
		candidate = &mode_table[index];
		if (candidate->width != width)
			continue;
		if (candidate->height != height)
			continue;
		if (candidate->refresh != refresh)
			continue;

		/* Succeeded: the table has the mode. */
		*mode = candidate;
		return 0;
	}

	/* The table lacks it. */
	return ENOENT;
}

/*
 * Reads the mode the boot line names with display.mode=WxH or WxH@R.
 *
 * ENOENT means the line has no such word; EINVAL that the word's value is
 * not a mode.  A value without a rate takes 60 Hz.
 */
int
powervr_dc_mode_from_line(
	const char *line,
	uint32_t *width,
	uint32_t *height,
	uint32_t *refresh)
{
	size_t word_length;
	size_t start;
	size_t end;
	size_t position;
	size_t index;
	int matched;
	int error;

	/* Starts from nothing read. */
	*width = 0;
	*height = 0;
	*refresh = 0;
	if (line == NULL)
		return ENOENT;

	/* Finds the word at the start of the line or after a space. */
	word_length = sizeof(MODE_WORD) - 1U;
	start = 0;
	matched = 0;
	for (position = 0; line[position] != '\0'; position++) {
		/* Only the start of a word can begin it. */
		if (position != 0 && line[position - 1U] != ' ' && line[position - 1U] != '\t')
			continue;

		/* Compares the word letter by letter. */
		for (index = 0; index < word_length; index++) {
			if (line[position + index] != MODE_WORD[index])
				break;
		}

		if (index == word_length) {
			matched = 1;
			start = position + word_length;
			break;
		}
	}

	if (!matched)
		return ENOENT;

	/* The value runs to the next space or the end of the line. */
	end = start;
	while (line[end] != '\0' && line[end] != ' ' && line[end] != '\t')
		end++;

	/* Reads the width, the x, and the height. */
	position = start;
	error = read_number(line, end, &position, width);
	if (error != 0)
		return error;
	if (position >= end || line[position] != 'x')
		return EINVAL;
	position++;

	error = read_number(line, end, &position, height);
	if (error != 0)
		return error;

	/* Takes 60 Hz when the value names no rate. */
	if (position == end) {
		*refresh = MODE_REFRESH_DEFAULT;
		return 0;
	}

	/* Reads the rate after the at sign, which must end the value. */
	if (line[position] != '@')
		return EINVAL;
	position++;

	error = read_number(line, end, &position, refresh);
	if (error != 0)
		return error;
	if (position != end)
		return EINVAL;

	/* Succeeded: the line names a mode. */
	return 0;
}

/*
 * Finds the HDMI transmitter's PLL settings for a pixel clock; ENOENT
 * when the table lacks the clock.
 */
int
powervr_dc_pll_find(
	uint32_t clock_khz,
	struct powervr_dc_pll *pll)
{
	unsigned index;

	/* Walks the table for the exact clock. */
	for (index = 0; index < sizeof(pll_table) / sizeof(pll_table[0]); index++) {
		if (pll_table[index].clock_khz != clock_khz)
			continue;

		/* Succeeded: the table has the clock. */
		*pll = pll_table[index];
		return 0;
	}

	/* The transmitter cannot make the clock. */
	return ENOENT;
}

/*
 * Computes a panel's four timing words for a mode.
 *
 * Procedure referenced:
 * Linux torvalds/linux master drivers/gpu/drm/verisilicon/vs_crtc.c
 * (vs_crtc_mode_set_nofb: the visible count and the total, the sync's
 * start and end with the enable bit, and the polarity bit for a low
 * pulse).
 */
void
powervr_dc_panel_timing(
	const struct powervr_dc_mode *mode,
	struct powervr_dc_panel_timing *timing)
{
	/* The visible pixels and the pixels of the whole line. */
	timing->horizontal = mode->width | (mode->htotal << TIMING_TOTAL_SHIFT);

	/* Where the line's sync starts and ends, enabled, low unless the mode's is high. */
	timing->horizontal_sync = mode->hsync_start | (mode->hsync_end << TIMING_SYNC_END_SHIFT) | TIMING_SYNC_ENABLE;
	if (!mode->hsync_positive)
		timing->horizontal_sync |= TIMING_SYNC_LOW;

	/* The visible lines and the lines of the whole frame. */
	timing->vertical = mode->height | (mode->vtotal << TIMING_TOTAL_SHIFT);

	/* Where the frame's sync starts and ends, enabled, low unless the mode's is high. */
	timing->vertical_sync = mode->vsync_start | (mode->vsync_end << TIMING_SYNC_END_SHIFT) | TIMING_SYNC_ENABLE;
	if (!mode->vsync_positive)
		timing->vertical_sync |= TIMING_SYNC_LOW;
}

/*
 * Lists the HDMI transmitter's writes that set its two PLLs, in order,
 * and returns how many there are.
 *
 * Procedure referenced:
 * StarFive Linux JH7110_VisionFive2_6.12.y_devel 4cecf169f38e
 * drivers/gpu/drm/verisilicon/inno_hdmi.c (inno_hdmi_config_pll: both
 * PLLs powered down, the dividers written, the post-PLL powered up, the
 * fraction written when there is one, the pre-PLL powered up last).
 */
unsigned
powervr_dc_hdmi_pll_writes(
	const struct powervr_dc_pll *pll,
	struct powervr_dc_hdmi_write *writes)
{
	unsigned count;
	uint32_t feedback_top;
	uint32_t output;
	uint32_t power;

	count = 0;

	/* Powers both PLLs down while they are set. */
	add_write(writes, &count, HDMI_PRE_PLL_POWER, HDMI_PRE_PLL_DOWN);
	add_write(writes, &count, HDMI_POST_PLL_POWER, HDMI_POST_PLL_DOWN);

	/* The pre-PLL's input divider and feedback divider, nine bits split over two registers. */
	feedback_top = HDMI_FEEDBACK_INTEGER;
	if (pll->fraction != 0)
		feedback_top = HDMI_FEEDBACK_FRACTIONAL;
	add_write(writes, &count, HDMI_PRE_PLL_DIVIDER, pll->pre_divider);
	add_write(writes, &count, HDMI_PRE_PLL_FEEDBACK_HIGH, feedback_top | (pll->pre_feedback >> 8));
	add_write(writes, &count, HDMI_PRE_PLL_FEEDBACK_LOW, pll->pre_feedback & 0xffU);

	/* The TMDS clock's three dividers. */
	add_write(writes, &count, HDMI_PRE_PLL_TMDS, (pll->tmds_a << 4) | (pll->tmds_b << 2) | pll->tmds_c);

	/* The pixel clock's four dividers, two to a register. */
	add_write(writes, &count, HDMI_PRE_PLL_PIXEL_AB, (pll->pixel_b << 5) | pll->pixel_a);
	add_write(writes, &count, HDMI_PRE_PLL_PIXEL_CD, (pll->pixel_c << 5) | pll->pixel_d);

	/* The post-PLL's input divider, feedback divider and output divider. */
	output = 0;
	power = HDMI_POST_PLL_UP_DIRECT;
	if (pll->post_divider_enable != 0) {
		output = pll->post_divider_out;
		power = HDMI_POST_PLL_UP_DIVIDED;
	}
	add_write(writes, &count, HDMI_POST_PLL_DIVIDER, pll->post_divider_in);
	add_write(writes, &count, HDMI_POST_PLL_FEEDBACK, pll->post_feedback & 0xffU);
	add_write(writes, &count, HDMI_POST_PLL_OUTPUT, output);

	/* Powers the post-PLL up. */
	add_write(writes, &count, HDMI_POST_PLL_POWER, power);

	/* The pre-PLL's fraction, low byte first, when the clock needs one. */
	if (pll->fraction != 0) {
		add_write(writes, &count, HDMI_PRE_PLL_FRACTION_LOW, pll->fraction & 0xffU);
		add_write(writes, &count, HDMI_PRE_PLL_FRACTION_MIDDLE, (pll->fraction >> 8) & 0xffU);
		add_write(writes, &count, HDMI_PRE_PLL_FRACTION_HIGH, (pll->fraction >> 16) & 0xffU);
	}

	/* Powers the pre-PLL up last. */
	add_write(writes, &count, HDMI_PRE_PLL_POWER, HDMI_PRE_PLL_UP);

	return count;
}

/*
 * Lists the HDMI transmitter's writes that give it the mode's timing,
 * and returns how many there are.
 *
 * Procedure referenced:
 * StarFive Linux JH7110_VisionFive2_6.12.y_devel 4cecf169f38e
 * drivers/gpu/drm/verisilicon/inno_hdmi.c (inno_hdmi_config_video_timing:
 * the line's total, blank, delay from the sync to the line's end and
 * sync width; the frame's the same; then the external timing with the
 * syncs' polarities).
 */
unsigned
powervr_dc_hdmi_timing_writes(
	const struct powervr_dc_mode *mode,
	struct powervr_dc_hdmi_write *writes)
{
	unsigned count;
	uint32_t value;
	uint32_t control;

	count = 0;

	/* The whole line. */
	value = mode->htotal;
	add_write(writes, &count, HDMI_TIMING_HTOTAL_LOW, value & 0xffU);
	add_write(writes, &count, HDMI_TIMING_HTOTAL_HIGH, (value >> 8) & 0xffU);

	/* The line's blank. */
	value = mode->htotal - mode->width;
	add_write(writes, &count, HDMI_TIMING_HBLANK_LOW, value & 0xffU);
	add_write(writes, &count, HDMI_TIMING_HBLANK_HIGH, (value >> 8) & 0xffU);

	/* From the line's sync to its end. */
	value = mode->htotal - mode->hsync_start;
	add_write(writes, &count, HDMI_TIMING_HDELAY_LOW, value & 0xffU);
	add_write(writes, &count, HDMI_TIMING_HDELAY_HIGH, (value >> 8) & 0xffU);

	/* The line's sync width. */
	value = mode->hsync_end - mode->hsync_start;
	add_write(writes, &count, HDMI_TIMING_HWIDTH_LOW, value & 0xffU);
	add_write(writes, &count, HDMI_TIMING_HWIDTH_HIGH, (value >> 8) & 0xffU);

	/* The whole frame. */
	value = mode->vtotal;
	add_write(writes, &count, HDMI_TIMING_VTOTAL_LOW, value & 0xffU);
	add_write(writes, &count, HDMI_TIMING_VTOTAL_HIGH, (value >> 8) & 0xffU);

	/* The frame's blank, from its sync to its end, and its sync width, a byte each. */
	add_write(writes, &count, HDMI_TIMING_VBLANK, (mode->vtotal - mode->height) & 0xffU);
	add_write(writes, &count, HDMI_TIMING_VDELAY, (mode->vtotal - mode->vsync_start) & 0xffU);
	add_write(writes, &count, HDMI_TIMING_VWIDTH, (mode->vsync_end - mode->vsync_start) & 0xffU);

	/* Takes the timing from these registers, with the syncs' polarities. */
	control = HDMI_TIMING_EXTERNAL;
	if (mode->hsync_positive)
		control |= HDMI_TIMING_HSYNC_HIGH;
	if (mode->vsync_positive)
		control |= HDMI_TIMING_VSYNC_HIGH;
	add_write(writes, &count, HDMI_TIMING_CONTROL, control);

	return count;
}

/*
 * Lists the HDMI transmitter's drive settings a mode's video code asks
 * for, and returns how many there are (none for the other modes).
 *
 * Values referenced:
 * StarFive Linux JH7110_VisionFive2_6.12.y_devel 4cecf169f38e
 * drivers/gpu/drm/verisilicon/inno_hdmi.c (inno_hdmi_improve_eye_diagram:
 * VIC 16 and 31 write 0x02 and 0x22; VIC 1, 3 and 4 write 0 and 0).
 */
unsigned
powervr_dc_hdmi_drive_writes(
	const struct powervr_dc_mode *mode,
	struct powervr_dc_hdmi_write *writes)
{
	unsigned count;

	count = 0;

	/* The 1080p modes. */
	if (mode->vic == 16U || mode->vic == 31U) {
		add_write(writes, &count, HDMI_DRIVE_PRE_EMPHASIS, 0x02U);
		add_write(writes, &count, HDMI_DRIVE_SWING, 0x22U);
	}

	/* The 480p and 720p modes. */
	if (mode->vic == 1U || mode->vic == 3U || mode->vic == 4U) {
		add_write(writes, &count, HDMI_DRIVE_PRE_EMPHASIS, 0x00U);
		add_write(writes, &count, HDMI_DRIVE_SWING, 0x00U);
	}

	return count;
}

/*
 * Reads a decimal number of the boot line's mode, moving the position past
 * it.  EINVAL for no digit or a number over the limit.
 */
static int
read_number(
	const char *text,
	size_t length,
	size_t *position,
	uint32_t *value)
{
	size_t start;
	uint32_t number;

	/* Reads digits while they last. */
	start = *position;
	number = 0;
	while (*position < length && text[*position] >= '0' && text[*position] <= '9') {
		number = number * 10U + (uint32_t)(text[*position] - '0');
		if (number > MODE_NUMBER_MAX)
			return EINVAL;
		(*position)++;
	}

	/* Refuses an empty number. */
	if (*position == start)
		return EINVAL;

	/* Succeeded: the number is read. */
	*value = number;
	return 0;
}

/* Appends one write to a list that has room for it. */
static void
add_write(
	struct powervr_dc_hdmi_write *writes,
	unsigned *count,
	uint32_t reg,
	uint32_t value)
{
	/* A full list drops the write; the callers' lists are sized for their steps. */
	if (*count >= POWERVR_DC_HDMI_WRITES_MAX)
		return;

	writes[*count].reg = reg;
	writes[*count].value = value;
	(*count)++;
}
