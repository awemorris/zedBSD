/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Stage A11: the re-light of the firmware's TMDS output (DVI or HDMI)
 * in the same mode.
 *
 * The timing generator keeps running and keeps its timing, the plane and
 * the clocks; the picture is blanked at the timing generator, the
 * transmitter is turned off by the video BIOS's routine (the monitor
 * loses its signal), and after a pause the encoder's stream is set up and
 * the transmitter turned on again by the routines, as a mode set does,
 * then the picture is shown again.  The timing generator's CRC of the
 * picture before and after, its frame counter and the underflow bits say
 * how it went.  A DisplayPort output is not re-lit here (its link
 * training is the DisplayPort part, host-tested until a DisplayPort sink
 * is attached).
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <kern/clock.h>
#include <kern/kcrt.h>
#include <uapi/amdrdna.h>
#include <uapi/errno.h>

#include "drivers/gpu/amdrdna/amdrdna-private.h"
#include "drivers/gpu/amdrdna/amdrdna-sink.h"

/*
 * The video BIOS's routines (their places in the command list: the
 * encoder 4, the transmitter 76) and their actions: the encoder's stream
 * setup (15), the transmitter's off (0) and on (1); the modes DVI (2)
 * and HDMI (3) in both; 8 bits a colour (2); four lanes.
 *
 * Values referenced: Linux v6.19
 * drivers/gpu/drm/amd/include/atomfirmware.h
 * (struct atom_master_list_of_command_functions_v2_1: digxencodercontrol,
 * dig1transmittercontrol; ATOM_ENCODER_CMD_STREAM_SETUP 0x0F,
 * ATOM_TRANSMITTER_ACTION_DISABLE 0, ATOM_TRANSMITTER_ACTION_ENABLE 1,
 * ATOM_ENCODER_MODE_DVI 2, ATOM_ENCODER_MODE_HDMI 3,
 * PANEL_8BIT_PER_COLOR 2), atombios.h (ATOM_TRANSMITTER_DIGMODE_V6_DVI 2,
 * ATOM_TRANSMITTER_DIGMODE_V6_HDMI 3) and
 * drivers/gpu/drm/amd/display/dc/dio/dcn10/dcn10_link_encoder.c
 * (dcn10_link_encoder_enable_tmds_output: four lanes for a single link).
 */
#define RELIGHT_ENCODER_ROUTINE		0x04U
#define RELIGHT_TRANSMITTER_ROUTINE	0x4cU
#define RELIGHT_STREAM_SETUP		0x0fU
#define RELIGHT_TRANSMITTER_OFF		0U
#define RELIGHT_TRANSMITTER_ON		1U
#define RELIGHT_MODE_DVI		2U
#define RELIGHT_MODE_HDMI		3U
#define RELIGHT_EIGHT_BITS		2U
#define RELIGHT_LANES			4U

/*
 * The timing generator's blank word (show black: bit 8; the blank the
 * generator is in: bit 0) and its CRC word (on: bit 0, continuous:
 * bit 4).
 *
 * Values referenced: Linux v6.19
 * drivers/gpu/drm/amd/include/asic_reg/dcn/dcn_2_0_0_sh_mask.h
 * (OTG0_OTG_BLANK_CONTROL: OTG_BLANK_DATA_EN, OTG_CURRENT_BLANK_STATE;
 * OTG0_OTG_CRC_CNTL: OTG_CRC_EN, OTG_CRC_CONT_EN) and
 * drivers/gpu/drm/amd/display/dc/optc/dcn10/dcn10_optc.c
 * (optc1_set_blank, optc1_configure_crc, optc1_get_crc).
 */
#define RELIGHT_BLANK_DATA		0x00000100U
#define RELIGHT_BLANK_STATE		0x00000001U
#define RELIGHT_CRC_ON			0x00000001U
#define RELIGHT_CRC_CONTINUOUS		0x00000010U

/* How long the monitor is left without a signal, and the most a few frames are waited for. */
#define RELIGHT_DARK_US			300000U
#define RELIGHT_FRAMES_WAIT_US		200000U
#define RELIGHT_STEP_US			500U

/* The CRC of a picture: its red and green word and its blue word. */
struct relight_crc {
	uint32_t red_green;
	uint32_t blue;
};

static int relight_routine(struct amdrdna_device *device, unsigned routine, uint32_t *words, const char *what, uint32_t *steps);
static void relight_transmitter(struct amdrdna_device *device, const struct amdrdna_sink *sink, uint32_t action, uint32_t mode, uint32_t pixel_10khz, uint32_t *words);
static bool relight_frames(struct amdrdna_device *device, unsigned frames);
static void relight_blank(struct amdrdna_device *device, bool blank);
static void relight_crc(struct amdrdna_device *device, struct relight_crc *crc);
static uint32_t relight_read(struct amdrdna_device *device, enum amdrdna_dcn_register which, unsigned instance);
static void relight_write(struct amdrdna_device *device, enum amdrdna_dcn_register which, unsigned instance, uint32_t value);

/*
 * Stage A11: re-lights the firmware's TMDS output in the same mode.
 *
 * pixel_khz is the running timing's pixel clock; 0 takes the mode shown
 * (modeset.c), else works it out from the totals and the refresh stage A6
 * measured (a caller that runs before A6, the reset, measures the clock
 * itself).  Returns 0; ENOTSUP for an output that is not TMDS; ENOENT before stage
 * A10 read the output's path; or the error of the routine that failed
 * (the transmitter is turned on again and the picture shown either way).
 *
 * Procedure referenced: Linux v6.19
 * drivers/gpu/drm/amd/display/dc/link/link_dpms.c (the stream's off and
 * on: blank, the link's output off, then the stream encoder's setup, the
 * link's output on, unblank),
 * display/dc/dio/dcn10/dcn10_link_encoder.c
 * (dcn10_link_encoder_disable_output, dcn10_link_encoder_enable_tmds_output),
 * display/dc/dio/dcn10/dcn10_stream_encoder.c
 * (enc1_stream_encoder_dvi_set_stream_attribute) and
 * display/dc/bios/command_table2.c (encoder_control_digx_v1_5,
 * transmitter_control_v1_6).
 */
int
amdrdna_a11_relight(
	struct amdrdna_device *device,
	uint32_t pixel_khz)
{
	struct amdrdna_encoder_command encoder;
	struct relight_crc before;
	struct relight_crc after;
	const struct amdrdna_sink *sink;
	uint32_t words[AMDRDNA_COMMAND_WORDS];
	struct amdrdna_timing now;
	uint32_t pixel_10khz;
	uint32_t refresh_millihz;
	int listed;
	uint64_t frame_pixels;
	uint32_t mode;
	uint32_t underflow;
	uint32_t timing_underflow;
	uint32_t frame_before;
	uint32_t frame_after;
	uint32_t steps;
	uint32_t total_steps;
	uint64_t dark_from;
	uint64_t dark_us;
	bool same;
	int error;
	int final;

	/* Stage A10's path of a lit TMDS output. */
	amdrdna_mark("A11 begin");
	sink = amdrdna_sink_ready(device);
	if (sink == NULL || !device->gop.has_output) {
		amdrdna_mark("A11 fail: no output path (stage A10 first)");
		return ENOENT;
	}
	if (device->gop.output_kind == AMDRDNA_OUTPUT_DVI) {
		mode = RELIGHT_MODE_DVI;
	} else if (device->gop.output_kind == AMDRDNA_OUTPUT_HDMI) {
		mode = RELIGHT_MODE_HDMI;
	} else {
		amdrdna_mark("A11 note: output kind %u is not TMDS; the DisplayPort re-light waits for a DisplayPort sink",
		    device->gop.output_kind);
		return ENOTSUP;
	}

	/*
	 * The pixel clock of the running timing, in 10 kHz: the caller's, else
	 * the mode shown (the monitor's listed clock), else the totals at the
	 * measured refresh.
	 */
	frame_pixels = (uint64_t)device->gop.h_total * device->gop.v_total;
	if (pixel_khz == 0U) {
		mutex_lock(&device->display.mode_lock);

		listed = amdrdna_mode_current(device, &now);

		mutex_unlock(&device->display.mode_lock);

		if (listed == 0)
			pixel_khz = now.pixel_khz;
	}
	if (pixel_khz != 0U) {
		pixel_10khz = (pixel_khz + 5U) / 10U;
	} else {
		pixel_10khz = (uint32_t)((frame_pixels * device->display.refresh_millihz + 5000000U) / 10000000U);
	}
	if (pixel_10khz == 0U || frame_pixels == 0U) {
		amdrdna_mark("A11 fail: the running timing's pixel clock is not known");
		return ENOENT;
	}

	/* The refresh the clock gives, for the report. */
	refresh_millihz = (uint32_t)(((uint64_t)pixel_10khz * 10000000U + frame_pixels / 2U) / frame_pixels);

	/* The picture's CRC, the frame counter and the underflow before. */
	relight_crc(device, &before);
	frame_before = relight_read(device, AMDRDNA_DCN_TIMING_FRAME_COUNT, device->gop.timing);
	total_steps = 0U;

	/* The picture blanked at the timing generator, then the transmitter off. */
	relight_blank(device, true);
	relight_transmitter(device, sink, RELIGHT_TRANSMITTER_OFF, mode, pixel_10khz, words);
	error = relight_routine(device, RELIGHT_TRANSMITTER_ROUTINE, words, "transmitter off", &steps);
	total_steps += steps;
	dark_from = amdrdna_display_now_us();

	/* The monitor without a signal for a while. */
	kern_usleep_range(RELIGHT_DARK_US, RELIGHT_DARK_US + RELIGHT_STEP_US);

	/* The encoder's stream set up as a mode set does. */
	if (error == 0) {
		encoder = (struct amdrdna_encoder_command){ 0 };
		encoder.dig = (uint8_t)device->gop.output_front;
		encoder.action = RELIGHT_STREAM_SETUP;
		encoder.mode = (uint8_t)mode;
		encoder.lanes = RELIGHT_LANES;
		encoder.pixel_10khz = pixel_10khz;
		if (mode == RELIGHT_MODE_HDMI)
			encoder.bits_per_colour = RELIGHT_EIGHT_BITS;
		amdrdna_command_encoder(words, &encoder);
		error = relight_routine(device, RELIGHT_ENCODER_ROUTINE, words, "encoder setup", &steps);
		total_steps += steps;
	}

	/* The transmitter on again, whatever happened before (the output must come back). */
	relight_transmitter(device, sink, RELIGHT_TRANSMITTER_ON, mode, pixel_10khz, words);
	final = relight_routine(device, RELIGHT_TRANSMITTER_ROUTINE, words, "transmitter on", &steps);
	total_steps += steps;
	dark_us = amdrdna_display_now_us() - dark_from;
	if (error == 0)
		error = final;

	/* Two frames for the monitor to lock, then the picture shown and two more frames. */
	(void)relight_frames(device, 2U);
	relight_blank(device, false);
	(void)relight_frames(device, 2U);

	/* The CRC, the frame counter and the underflow after. */
	relight_crc(device, &after);
	frame_after = relight_read(device, AMDRDNA_DCN_TIMING_FRAME_COUNT, device->gop.timing);
	underflow = amdrdna_display_underflow(device, &timing_underflow);
	same = false;
	if (before.red_green == after.red_green && before.blue == after.blue)
		same = true;

	/* Reports a routine that failed. */
	if (error != 0) {
		amdrdna_mark("A11 fail: a routine failed (%d); the transmitter was turned on again, frames %u", error,
		    (unsigned)(frame_after - frame_before));
		return error;
	}

	/* Says how it went (the CRC before only when the picture changed). */
	if (!same)
		amdrdna_mark("A11 note: the picture's crc before was %08x/%08x", (unsigned)before.red_green, (unsigned)before.blue);
	amdrdna_mark("A11 ok relit %ux%u@%u.%03u %s phy=%u dig=%u pclk=%ukHz dark=%llums steps=%u frames=%u crc=%s %08x/%08x underflow=%u/%u",
	    device->display.width,
	    device->display.height,
	    (unsigned)(refresh_millihz / 1000U),
	    (unsigned)(refresh_millihz % 1000U),
	    mode == RELIGHT_MODE_DVI ? "DVI" : "HDMI",
	    sink->connector.transmitter,
	    device->gop.output_front,
	    (unsigned)(pixel_10khz * 10U),
	    (unsigned long long)(dark_us / 1000U),
	    (unsigned)total_steps,
	    (unsigned)(frame_after - frame_before),
	    same ? "same" : "differs",
	    (unsigned)after.red_green,
	    (unsigned)after.blue,
	    (unsigned)underflow,
	    (unsigned)timing_underflow);

	/* Succeeded: the output is lit again. */
	return 0;
}

/* Runs one of the display routines; a failure is said with what it was. */
static int
relight_routine(
	struct amdrdna_device *device,
	unsigned routine,
	uint32_t *words,
	const char *what,
	uint32_t *steps)
{
	int error;

	/* The routine on the board's registers. */
	error = amdrdna_bios_run(device, routine, words, AMDRDNA_COMMAND_WORDS, steps);
	if (error != 0) {
		amdrdna_mark("A11 note: the %s routine failed (%d)", what, error);
		return error;
	}

	/* Succeeded: the routine ran. */
	return 0;
}

/*
 * Packs the transmitter routine's parameters for the output: its PHY, the
 * action, the mode, its hot-plug line and no front end (the front end is
 * chosen in the back end's word); turning it off names the connector,
 * turning it on gives four lanes and the symbol clock (the pixel clock at
 * 8 bits a colour) instead.
 *
 * Procedure referenced: Linux v6.19
 * drivers/gpu/drm/amd/display/dc/dio/dcn10/dcn10_link_encoder.c
 * (dcn10_link_encoder_disable_output, dcn10_link_encoder_enable_tmds_output)
 * and display/dc/bios/dce112/command_table_helper2_dce112.c (no front end
 * in the routine's selection).
 */
static void
relight_transmitter(
	struct amdrdna_device *device,
	const struct amdrdna_sink *sink,
	uint32_t action,
	uint32_t mode,
	uint32_t pixel_10khz,
	uint32_t *words)
{
	struct amdrdna_transmitter_command command;

	/* The fields. */
	command = (struct amdrdna_transmitter_command){ 0 };
	command.phy = (uint8_t)sink->connector.transmitter;
	command.action = (uint8_t)action;
	command.mode = (uint8_t)mode;
	command.hotplug = (uint8_t)(device->gop.output_hotplug + 1U);
	command.front_ends = 0U;
	if (action == RELIGHT_TRANSMITTER_ON) {
		command.lanes = RELIGHT_LANES;
		command.symbol_10khz = pixel_10khz;
	} else {
		command.connector_id = (uint8_t)(sink->connector.connector_object & 255U);
	}
	amdrdna_command_transmitter(words, &command);
}

/* Waits for a number of frames of the firmware's timing generator; false when they did not pass in time. */
static bool
relight_frames(
	struct amdrdna_device *device,
	unsigned frames)
{
	uint32_t first;
	uint32_t now;
	unsigned waited;

	/* The frame counter until it moved on that far. */
	first = relight_read(device, AMDRDNA_DCN_TIMING_FRAME_COUNT, device->gop.timing);
	for (waited = 0U; waited < RELIGHT_FRAMES_WAIT_US; waited += RELIGHT_STEP_US) {
		now = relight_read(device, AMDRDNA_DCN_TIMING_FRAME_COUNT, device->gop.timing);
		if (((now - first) & 0x00ffffffU) >= frames)
			return true;
		kern_usleep_range(RELIGHT_STEP_US, RELIGHT_STEP_US * 2U);
	}

	/* Succeeded: no; the frames did not pass. */
	return false;
}

/* Blanks the timing generator's picture (black) or shows it, and waits for the generator to take it. */
static void
relight_blank(
	struct amdrdna_device *device,
	bool blank)
{
	uint32_t word;
	uint32_t wanted;
	unsigned waited;

	/* The blank bit. */
	word = relight_read(device, AMDRDNA_DCN_TIMING_BLANK, device->gop.timing);
	wanted = 0U;
	if (blank) {
		word |= RELIGHT_BLANK_DATA;
		wanted = RELIGHT_BLANK_STATE;
	} else {
		word &= ~RELIGHT_BLANK_DATA;
	}
	relight_write(device, AMDRDNA_DCN_TIMING_BLANK, device->gop.timing, word);

	/* The generator's state at the next frame. */
	for (waited = 0U; waited < RELIGHT_FRAMES_WAIT_US; waited += RELIGHT_STEP_US) {
		word = relight_read(device, AMDRDNA_DCN_TIMING_BLANK, device->gop.timing);
		if ((word & RELIGHT_BLANK_STATE) == wanted)
			return;
		kern_usleep_range(RELIGHT_STEP_US, RELIGHT_STEP_US * 2U);
	}
}

/*
 * Reads the CRC of the whole picture: the CRC set to run continuously
 * over the active area (the union of two windows that both cover it),
 * two frames, then its words.
 *
 * Procedure referenced: Linux v6.19
 * drivers/gpu/drm/amd/display/dc/core/dc.c (dc_stream_configure_crc: the
 * full frame, UNION_WINDOW_A_B) and display/dc/optc/dcn10/dcn10_optc.c
 * (optc1_configure_crc).
 */
static void
relight_crc(
	struct amdrdna_device *device,
	struct relight_crc *crc)
{
	/* Off, the whole active area as both windows, then continuous and on. */
	relight_write(device, AMDRDNA_DCN_TIMING_CRC_CONTROL, device->gop.timing, 0U);
	amdrdna_timing_crc_window(device, device->gop.timing, device->display.width, device->display.height);
	relight_write(device, AMDRDNA_DCN_TIMING_CRC_CONTROL, device->gop.timing, RELIGHT_CRC_CONTINUOUS | RELIGHT_CRC_ON);

	/* Two whole frames, then the words. */
	(void)relight_frames(device, 2U);
	crc->red_green = relight_read(device, AMDRDNA_DCN_TIMING_CRC_RED_GREEN, device->gop.timing);
	crc->blue = relight_read(device, AMDRDNA_DCN_TIMING_CRC_BLUE, device->gop.timing);
}

/* Reads a display register (0 when the display block lacks it). */
static uint32_t
relight_read(
	struct amdrdna_device *device,
	enum amdrdna_dcn_register which,
	unsigned instance)
{
	uint32_t index;
	uint32_t value;

	/* Its index, then the read. */
	index = amdrdna_dcn_index(&device->blocks.blocks[AMDRDNA_BLOCK_DCN], which, instance);
	if (index == 0U)
		return 0U;
	value = amdrdna_read(device, index);

	/* Succeeded: the register's value. */
	return value;
}

/* Writes a display register (nothing when the display block lacks it). */
static void
relight_write(
	struct amdrdna_device *device,
	enum amdrdna_dcn_register which,
	unsigned instance,
	uint32_t value)
{
	uint32_t index;

	/* Its index, then the write. */
	index = amdrdna_dcn_index(&device->blocks.blocks[AMDRDNA_BLOCK_DCN], which, instance);
	if (index == 0U)
		return;
	amdrdna_write(device, index, value);
}
