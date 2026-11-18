/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The display pipe across a reset of the GPU.
 *
 * A mode-1 reset takes the display engine down with the rest: the pipe
 * the firmware lit (and the driver took over) is gone, and the video
 * BIOS's post does not light it again.  Before the reset the words that
 * set the pipe up are read (reset-format.c names them) and the pixel
 * clock of the running timing is measured; after the post the pipe's
 * power is ungated by the video BIOS's routine, the words are written back
 * with the timing generator stopped, the pixel clock is set on the
 * output's PHY PLL by the video BIOS's routine, the timing generator is
 * started, the display interrupts the driver had on are turned on again,
 * and stage A11's re-light (relight.c) turns the encoder's stream and the
 * transmitter on.  The frame counter and the underflow bits say how it
 * went.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <kern/clock.h>
#include <kern/kcrt.h>
#include <kern/lock.h>
#include <uapi/amdrdna.h>
#include <uapi/errno.h>

#include "drivers/gpu/amdrdna/amdrdna-private.h"
#include "drivers/gpu/amdrdna/amdrdna-regs.h"
#include "drivers/gpu/amdrdna/amdrdna-reset.h"
#include "drivers/gpu/amdrdna/amdrdna-sink.h"

/*
 * The video BIOS's routines (their places in the command list: the pixel
 * clock 12, the pipe's power gating 13), the power gating's actions (init
 * 7 on the first pipe, then off, 0, on the pipe the picture uses), the
 * first PHY PLL's number (20; the PLL of PHY n is 20 + n), the pixel
 * clock's flags (0: the crystal as the PLL's reference) and the encoder
 * modes DVI (2) and HDMI (3).
 *
 * Values referenced: Linux v6.19
 * drivers/gpu/drm/amd/include/atomfirmware.h
 * (struct atom_master_list_of_command_functions_v2_1: setpixelclock,
 * enabledisppowergating; ATOM_INIT 7, ATOM_DISABLE 0,
 * ATOM_COMBOPHY_PLL0 20, PIXEL_CLOCK_V7_MISC_REF_DIV_SRC_XTALIN 0,
 * ATOM_ENCODER_MODE_DVI 2, ATOM_ENCODER_MODE_HDMI 3).
 */
#define DISPLAY_CLOCK_ROUTINE		0x0cU
#define DISPLAY_POWER_ROUTINE		0x0dU
#define DISPLAY_POWER_INIT		7U
#define DISPLAY_POWER_UNGATED		0U
#define DISPLAY_FIRST_PHY_PLL		20U
#define DISPLAY_CLOCK_FLAGS		0U
#define DISPLAY_MODE_DVI		2U
#define DISPLAY_MODE_HDMI		3U

/*
 * The video BIOS's sixth scratch word, which a driver that owns the
 * display marks (bit 4).
 *
 * Values referenced: Linux v6.19
 * drivers/gpu/drm/amd/include/atombios.h (ATOM_S6_ACC_MODE).
 */
#define DISPLAY_SCRATCH_OWNS		AMDRDNA_BIOS_SCRATCH_6_DRIVER_OWNS

/* The frames the measure of the refresh spans, and the most it waits for them. */
#define DISPLAY_MEASURE_FRAMES		6U
#define DISPLAY_MEASURE_WAIT_US		300000U
#define DISPLAY_STEP_US			500U

/* How long the timing generator is waited for after its start, and the frames looked at after the re-light. */
#define DISPLAY_START_WAIT_US		100000U
#define DISPLAY_LOOK_FRAMES		30U
#define DISPLAY_LOOK_WAIT_US		1000000U

/*
 * The underflow clear bits of the plane (31) and of the timing
 * generator's input (12).
 *
 * Values referenced: Linux v6.19
 * drivers/gpu/drm/amd/include/asic_reg/dcn/dcn_2_0_0_sh_mask.h
 * (HUBP0_DCHUBP_CNTL__HUBP_UNDERFLOW_CLEAR,
 * ODM0_OPTC_INPUT_GLOBAL_CONTROL__OPTC_UNDERFLOW_CLEAR).
 */
#define DISPLAY_PLANE_UNDERFLOW_CLEAR	0x80000000U
#define DISPLAY_TIMING_UNDERFLOW_CLEAR	0x00001000U

/* The refresh within which a listed mode is taken as the running one: half a hertz. */
#define DISPLAY_REFRESH_SLACK_MILLIHZ	500U

/*
 * The 24 bits of the timing generator's frame counter.
 *
 * Values referenced: Linux v6.19
 * drivers/gpu/drm/amd/include/asic_reg/dcn/dcn_2_0_0_sh_mask.h
 * (OTG0_OTG_STATUS_FRAME_COUNT__OTG_FRAME_COUNT_MASK).
 */
#define DISPLAY_FRAME_MASK		0x00ffffffU

static uint32_t display_instance(const struct amdrdna_device *device, enum amdrdna_reset_place place, unsigned set);
static uint32_t display_named_index(struct amdrdna_device *device, const struct amdrdna_reset_pipe_entry *entry);
static uint32_t display_extra_index(struct amdrdna_device *device, const struct amdrdna_reset_extra_entry *entry);
static uint32_t display_measure(struct amdrdna_device *device);
static uint32_t display_listed_khz(struct amdrdna_device *device, uint32_t refresh_millihz);
static int display_power(struct amdrdna_device *device);
static void display_write_back(struct amdrdna_device *device);
static int display_clock_set(struct amdrdna_device *device);
static int display_start(struct amdrdna_device *device);
static void display_interrupts_again(struct amdrdna_device *device);
static void display_underflow_clear(struct amdrdna_device *device);
static uint32_t display_frames(struct amdrdna_device *device, unsigned frames, uint32_t wait_us);
static uint32_t display_read(struct amdrdna_device *device, enum amdrdna_dcn_register which, unsigned instance);
static void display_write(struct amdrdna_device *device, enum amdrdna_dcn_register which, unsigned instance, uint32_t value);
static uint32_t display_scratch(struct amdrdna_device *device, unsigned word);

/*
 * Reads what the display pipe is set up with before a reset: every word
 * of reset-format.c's tables on the firmware's pipe, whether the timing
 * generator runs, the pixel clock of its timing, and whether a driver
 * owns the display (the video BIOS's scratch bit).  A GPU without a lit
 * output keeps nothing.
 */
void
amdrdna_reset_display_save(
	struct amdrdna_device *device)
{
	struct amdrdna_reset_pipe *pipe;
	const struct amdrdna_reset_pipe_entry *entry;
	const struct amdrdna_reset_extra_entry *extra;
	uint32_t index;
	uint32_t refresh_millihz;
	uint32_t word;
	unsigned count;
	unsigned item;

	/* Nothing to keep without a lit output. */
	pipe = &device->reset.pipe;
	pipe->saved = false;
	pipe->lit = false;
	if (!device->gop.lit || !device->gop.has_output)
		return;

	/* The named words, in the table's order; a word the display block lacks reads 0. */
	count = amdrdna_reset_pipe_count();
	for (item = 0U; item < count; item++) {
		entry = amdrdna_reset_pipe_entry(item);
		index = display_named_index(device, entry);
		pipe->words[item] = 0U;
		if (index != 0U)
			pipe->words[item] = amdrdna_read(device, index);
	}

	/* The other words. */
	count = amdrdna_reset_extra_count();
	for (item = 0U; item < count; item++) {
		extra = amdrdna_reset_extra_entry(item);
		index = display_extra_index(device, extra);
		pipe->extra[item] = 0U;
		if (index != 0U)
			pipe->extra[item] = amdrdna_read(device, index);
	}

	/* Whether the timing generator runs; a stopped one is left stopped and needs no clock. */
	word = display_read(device, AMDRDNA_DCN_TIMING_CONTROL, device->gop.timing);
	pipe->lit = amdrdna_reset_timing_running(word);
	pipe->saved = true;
	if (!pipe->lit) {
		amdrdna_mark("reset: display kept, otg%u stopped", device->gop.timing);
		return;
	}

	/* The pixel clock: a mode the monitor lists with the same totals and refresh, else the measured one. */
	refresh_millihz = display_measure(device);
	pipe->pixel_khz = display_listed_khz(device, refresh_millihz);
	if (pipe->pixel_khz == 0U)
		pipe->pixel_khz = amdrdna_reset_pixel_khz(device->gop.h_total, device->gop.v_total, refresh_millihz);

	/* Whether a driver owns the display, for the video BIOS's scratch word after the post. */
	word = display_scratch(device, AMDRDNA_BIOS_SCRATCH_ACCELERATED);
	pipe->driver_owns = false;
	if ((word & DISPLAY_SCRATCH_OWNS) != 0U)
		pipe->driver_owns = true;

	/* Marks what was kept. */
	amdrdna_mark("reset: display kept, %u words, otg%u running, pclk=%ukHz refresh=%u.%03uHz",
	    amdrdna_reset_pipe_count() + amdrdna_reset_extra_count(),
	    device->gop.timing,
	    (unsigned)pipe->pixel_khz,
	    (unsigned)(refresh_millihz / 1000U),
	    (unsigned)(refresh_millihz % 1000U));
}

/*
 * Lights the display pipe again after the post from the words kept
 * before the reset: the pipe's power, the words with the timing generator
 * stopped, the pixel clock, the timing generator started, the display
 * interrupts the driver had on, then stage A11's re-light of the output
 * (stage A10 reads the output's path first when it has not yet).
 *
 * Returns 0 when frames go on without underflow; 0 at once when nothing
 * was lit; or the error of the step that failed (EIO for frames that do
 * not move or an underflow).
 */
int
amdrdna_reset_display_restore(
	struct amdrdna_device *device)
{
	struct amdrdna_reset_pipe *pipe;
	const struct amdrdna_sink *sink;
	uint32_t word;
	uint32_t frames;
	uint32_t underflow;
	uint32_t timing_underflow;
	int error;

	/* Nothing to light when nothing was. */
	pipe = &device->reset.pipe;
	if (!pipe->saved || !pipe->lit)
		return 0;

	/* The pipe's power, through the video BIOS's routine. */
	error = display_power(device);
	if (error != 0)
		return error;

	/* The kept words, the timing generator stopped. */
	display_write_back(device);

	/* The pixel clock on the output's PHY PLL. */
	error = display_clock_set(device);
	if (error != 0)
		return error;

	/* The timing generator started. */
	error = display_start(device);
	if (error != 0)
		return error;

	/* The driver's mark in the video BIOS's scratch word, as before the reset. */
	if (pipe->driver_owns) {
		word = display_scratch(device, AMDRDNA_BIOS_SCRATCH_ACCELERATED);
		amdrdna_block_write(device,
		    AMDRDNA_BLOCK_NBIO,
		    AMDRDNA_NBIO_WINDOW_SCRATCH,
		    AMDRDNA_NBIO_REG_BIOS_SCRATCH_FIRST + AMDRDNA_BIOS_SCRATCH_ACCELERATED,
		    word | DISPLAY_SCRATCH_OWNS);
	}

	/* The display interrupts the driver had on. */
	display_interrupts_again(device);

	/* The output's path, read now when stage A10 has not run. */
	sink = amdrdna_sink_ready(device);
	if (sink == NULL) {
		error = amdrdna_a10_sink(device);
		if (error != 0) {
			amdrdna_mark("reset: display fail: the output's path not read (%d)", error);
			return error;
		}
	}

	/* The underflow the start may have left, cleared; then the encoder's stream and the transmitter, by stage A11. */
	display_underflow_clear(device);
	error = amdrdna_a11_relight(device, pipe->pixel_khz);
	if (error != 0) {
		amdrdna_mark("reset: display fail: the re-light failed (%d)", error);
		return error;
	}

	/* The frames from now on, and the underflow bits. */
	frames = display_frames(device, DISPLAY_LOOK_FRAMES, DISPLAY_LOOK_WAIT_US);
	underflow = amdrdna_display_underflow(device, &timing_underflow);
	if (frames < DISPLAY_LOOK_FRAMES ||
	    underflow != 0U ||
	    timing_underflow != 0U) {
		amdrdna_mark("reset: display fail: frames=%u underflow=%u/%u", (unsigned)frames, (unsigned)underflow,
		    (unsigned)timing_underflow);
		return EIO;
	}

	/* Says how it went. */
	amdrdna_mark("reset: display lit again otg%u hubp%u pclk=%ukHz frames=%u underflow=0",
	    device->gop.timing,
	    device->gop.plane,
	    (unsigned)pipe->pixel_khz,
	    (unsigned)frames);

	/* Succeeded: the picture goes on. */
	return 0;
}

/*
 * Acknowledges the display's events again once the interrupt ring
 * delivers (after stage A3 turned it on): an event that came while the
 * ring was off left its status set, and the display raises no other
 * until it is acknowledged.
 */
void
amdrdna_reset_display_rearm(
	struct amdrdna_device *device)
{
	/* The same switch as after the post: acknowledged and on. */
	display_interrupts_again(device);
}

/* Gives the instance a kept word belongs to. */
static uint32_t
display_instance(
	const struct amdrdna_device *device,
	enum amdrdna_reset_place place,
	unsigned set)
{
	/* The firmware's chain names each instance. */
	switch (place) {
	case AMDRDNA_RESET_ON_PLANE:
		return device->gop.plane;
	case AMDRDNA_RESET_ON_BLEND:
		return device->gop.blend;
	case AMDRDNA_RESET_ON_TREE:
		return device->gop.output_processor;
	case AMDRDNA_RESET_ON_PROCESSOR:
		return device->gop.output_processor;
	case AMDRDNA_RESET_ON_TIMING:
		return device->gop.timing;
	case AMDRDNA_RESET_ON_FRONT:
		return device->gop.output_front;
	case AMDRDNA_RESET_ON_BACK:
		return device->gop.output;
	case AMDRDNA_RESET_ON_SET:
		return set;
	default:
		break;
	}

	/* The hub's own words have one instance. */
	return 0U;
}

/* Gives the absolute index of a named kept word, 0 when the display block lacks it. */
static uint32_t
display_named_index(
	struct amdrdna_device *device,
	const struct amdrdna_reset_pipe_entry *entry)
{
	uint32_t instance;
	uint32_t index;

	/* The instance, then the register's place. */
	instance = display_instance(device, entry->place, entry->set);
	index = amdrdna_dcn_index(&device->blocks.blocks[AMDRDNA_BLOCK_DCN], entry->which, instance);

	/* Succeeded: the index, 0 for none. */
	return index;
}

/* Gives the absolute index of another kept word, 0 when the display block lacks its window. */
static uint32_t
display_extra_index(
	struct amdrdna_device *device,
	const struct amdrdna_reset_extra_entry *entry)
{
	uint32_t instance;
	uint32_t index;
	int error;

	/* The instance's offset in the window, then its index. */
	instance = display_instance(device, entry->place, 0U);
	error = amdrdna_block_index(device, AMDRDNA_BLOCK_DCN, entry->window, entry->offset + entry->stride * instance, &index);
	if (error != 0)
		return 0U;

	/* Succeeded: the index. */
	return index;
}

/*
 * Measures the refresh of the running timing in thousandths of a hertz:
 * the time of six frames from the start of one.  0 when the frames do not
 * move.
 */
static uint32_t
display_measure(
	struct amdrdna_device *device)
{
	uint64_t first_us;
	uint64_t last_us;
	uint32_t start;
	uint32_t frame;
	unsigned waited;

	/* Waits for a frame to begin, so the measure starts on its edge. */
	start = display_read(device, AMDRDNA_DCN_TIMING_FRAME_COUNT, device->gop.timing);
	frame = start;
	for (waited = 0U; waited < DISPLAY_MEASURE_WAIT_US && frame == start; waited += DISPLAY_STEP_US) {
		kern_usleep_range(DISPLAY_STEP_US, DISPLAY_STEP_US + DISPLAY_STEP_US / 4U);
		frame = display_read(device, AMDRDNA_DCN_TIMING_FRAME_COUNT, device->gop.timing);
	}
	if (frame == start)
		return 0U;
	first_us = amdrdna_display_now_us();
	start = frame;

	/* Waits for the frames to pass. */
	for (waited = 0U; waited < DISPLAY_MEASURE_WAIT_US; waited += DISPLAY_STEP_US) {
		frame = display_read(device, AMDRDNA_DCN_TIMING_FRAME_COUNT, device->gop.timing);
		if (((frame - start) & DISPLAY_FRAME_MASK) >= DISPLAY_MEASURE_FRAMES)
			break;
		kern_usleep_range(DISPLAY_STEP_US, DISPLAY_STEP_US + DISPLAY_STEP_US / 4U);
	}
	last_us = amdrdna_display_now_us();
	if (((frame - start) & DISPLAY_FRAME_MASK) < DISPLAY_MEASURE_FRAMES || last_us <= first_us)
		return 0U;

	/* Succeeded: the frames a second, in thousandths. */
	return (uint32_t)((uint64_t)((frame - start) & DISPLAY_FRAME_MASK) * 1000000000ULL / (last_us - first_us));
}

/*
 * Finds the running timing among the modes the monitor lists (the same
 * totals and a refresh within half a hertz) and gives its pixel clock in
 * kHz; 0 when stage A10 has not read the monitor or none matches.
 */
static uint32_t
display_listed_khz(
	struct amdrdna_device *device,
	uint32_t refresh_millihz)
{
	const struct amdrdna_sink *sink;
	const struct amdrdna_timing *mode;
	uint32_t h_total;
	uint32_t v_total;
	uint32_t difference;
	unsigned index;

	/* Without the monitor's list there is nothing to match. */
	sink = amdrdna_sink_ready(device);
	if (sink == NULL)
		return 0U;

	/* The first listed mode with the same totals and about the same refresh. */
	for (index = 0U; index < sink->edid.mode_count; index++) {
		mode = &sink->edid.modes[index];
		h_total = (uint32_t)mode->width + mode->h_blanking;
		v_total = (uint32_t)mode->height + mode->v_blanking;
		if (mode->pixel_khz == 0U ||
		    h_total != device->gop.h_total ||
		    v_total != device->gop.v_total)
			continue;

		/* The refresh's distance from the measured one. */
		difference = mode->refresh_millihz - refresh_millihz;
		if (refresh_millihz > mode->refresh_millihz)
			difference = refresh_millihz - mode->refresh_millihz;
		if (difference <= DISPLAY_REFRESH_SLACK_MILLIHZ)
			return mode->pixel_khz;
	}

	/* Succeeded: none matches. */
	return 0U;
}

/*
 * Ungates the power of the pipe the picture uses as the display's own
 * init does: the video BIOS's routine with init on the first pipe, then
 * with the gating off on the plane's pipe.
 *
 * Procedure referenced: Linux v6.19
 * drivers/gpu/drm/amd/display/dc/hwss/dcn10/dcn10_hwseq.c
 * (dcn10_bios_golden_init: ASIC_PIPE_INIT on the first controller, then
 * ASIC_PIPE_DISABLE on each pipe) and display/dc/bios/command_table2.c
 * (enable_disp_power_gating_v2_1).
 */
static int
display_power(
	struct amdrdna_device *device)
{
	uint32_t words[AMDRDNA_COMMAND_WORDS];
	uint32_t steps;
	int error;

	/* The init on the first pipe. */
	amdrdna_command_pipe(words, 0U, DISPLAY_POWER_INIT);
	error = amdrdna_bios_run(device, DISPLAY_POWER_ROUTINE, words, AMDRDNA_COMMAND_WORDS, &steps);
	if (error != 0) {
		amdrdna_mark("reset: display fail: the power routine's init failed (%d)", error);
		return error;
	}

	/* The gating off on the plane's pipe. */
	amdrdna_command_pipe(words, device->gop.plane, DISPLAY_POWER_UNGATED);
	error = amdrdna_bios_run(device, DISPLAY_POWER_ROUTINE, words, AMDRDNA_COMMAND_WORDS, &steps);
	if (error != 0) {
		amdrdna_mark("reset: display fail: the power routine failed on pipe %u (%d)", device->gop.plane, error);
		return error;
	}

	/* Succeeded: the pipe has its power. */
	return 0;
}

/*
 * Writes the kept words back in the tables' order, the other words first;
 * the timing generator's control and its vertical generator are written
 * with their enables off.
 */
static void
display_write_back(
	struct amdrdna_device *device)
{
	struct amdrdna_reset_pipe *pipe;
	const struct amdrdna_reset_pipe_entry *entry;
	const struct amdrdna_reset_extra_entry *extra;
	uint32_t index;
	uint32_t word;
	unsigned count;
	unsigned item;

	/* The other words: the hub's apertures and the clocks. */
	pipe = &device->reset.pipe;
	count = amdrdna_reset_extra_count();
	for (item = 0U; item < count; item++) {
		extra = amdrdna_reset_extra_entry(item);
		index = display_extra_index(device, extra);
		if (index != 0U)
			amdrdna_write(device, index, pipe->extra[item]);
	}

	/* The named words, the timing generator's enables held off. */
	count = amdrdna_reset_pipe_count();
	for (item = 0U; item < count; item++) {
		entry = amdrdna_reset_pipe_entry(item);
		index = display_named_index(device, entry);
		if (index == 0U)
			continue;

		/* The word, stopped when it is the timing generator's switch. */
		word = pipe->words[item];
		if (entry->which == AMDRDNA_DCN_TIMING_CONTROL)
			word = amdrdna_reset_timing_stopped(word);
		else if (entry->which == AMDRDNA_DCN_VERTICAL_GENERATOR)
			word = amdrdna_reset_vertical_word(word, false);
		amdrdna_write(device, index, word);
	}
}

/*
 * Sets the pixel clock of the kept timing on the output's PHY PLL for the
 * firmware's timing generator, by the video BIOS's routine.
 *
 * Procedure referenced: Linux v6.19
 * drivers/gpu/drm/amd/display/dc/dce/dce_clock_source.c
 * (dcn20_program_pix_clk, dce112_program_pix_clk) and
 * display/dc/bios/command_table2.c (set_pixel_clock_v7).
 */
static int
display_clock_set(
	struct amdrdna_device *device)
{
	struct amdrdna_pixel_clock_command clock;
	struct amdrdna_vbios_connector connector;
	uint32_t words[AMDRDNA_COMMAND_WORDS];
	uint32_t steps;
	uint32_t mode;
	int error;

	/* The output's path in the video BIOS: its PHY and its encoder. */
	error = amdrdna_vbios_connector(device->vbios, device->vbios_bytes, &device->vbios_info, device->gop.output, &connector);
	if (error != 0) {
		amdrdna_mark("reset: display fail: no path for transmitter %u (%d)", device->gop.output, error);
		return error;
	}

	/* The encoder's mode: DVI or HDMI; another output is not lit here. */
	if (device->gop.output_kind == AMDRDNA_OUTPUT_DVI) {
		mode = DISPLAY_MODE_DVI;
	} else if (device->gop.output_kind == AMDRDNA_OUTPUT_HDMI) {
		mode = DISPLAY_MODE_HDMI;
	} else {
		amdrdna_mark("reset: display fail: output kind %u is not TMDS", device->gop.output_kind);
		return ENOTSUP;
	}

	/* The clock on the PHY's PLL, for the timing generator, the encoder and the mode. */
	clock = (struct amdrdna_pixel_clock_command){ 0 };
	clock.pixel_100hz = device->reset.pipe.pixel_khz * 10U;
	clock.pll = (uint8_t)(DISPLAY_FIRST_PHY_PLL + connector.transmitter);
	clock.encoder_object = (uint8_t)(connector.encoder_object & 255U);
	clock.mode = (uint8_t)mode;
	clock.flags = DISPLAY_CLOCK_FLAGS;
	clock.generator = (uint8_t)device->gop.timing;
	amdrdna_command_pixel_clock(words, &clock);
	error = amdrdna_bios_run(device, DISPLAY_CLOCK_ROUTINE, words, AMDRDNA_COMMAND_WORDS, &steps);
	if (error != 0) {
		amdrdna_mark("reset: display fail: the pixel clock routine failed (%d)", error);
		return error;
	}

	/* Succeeded: the pixel clock runs. */
	return 0;
}

/*
 * Starts the timing generator: its vertical generator first, then its
 * kept control word with the master enable, and waits for it to run.
 *
 * Procedure referenced: Linux v6.19
 * drivers/gpu/drm/amd/display/dc/optc/dcn20/dcn20_optc.c
 * (optc2_enable_crtc: the vertical generator, then the master enable).
 */
static int
display_start(
	struct amdrdna_device *device)
{
	const struct amdrdna_reset_pipe_entry *entry;
	uint32_t control;
	uint32_t word;
	unsigned count;
	unsigned item;
	unsigned waited;
	bool running;

	/* The kept control word. */
	control = 0U;
	count = amdrdna_reset_pipe_count();
	for (item = 0U; item < count; item++) {
		entry = amdrdna_reset_pipe_entry(item);
		if (entry->which == AMDRDNA_DCN_TIMING_CONTROL)
			control = device->reset.pipe.words[item];
	}

	/* The vertical generator, then the master enable. */
	word = display_read(device, AMDRDNA_DCN_VERTICAL_GENERATOR, device->gop.timing);
	display_write(device, AMDRDNA_DCN_VERTICAL_GENERATOR, device->gop.timing, amdrdna_reset_vertical_word(word, true));
	display_write(device, AMDRDNA_DCN_TIMING_CONTROL, device->gop.timing, control);

	/* Waits for it to run. */
	for (waited = 0U; waited < DISPLAY_START_WAIT_US; waited += DISPLAY_STEP_US) {
		/* Stops once its state says it runs. */
		word = display_read(device, AMDRDNA_DCN_TIMING_CONTROL, device->gop.timing);
		running = amdrdna_reset_timing_running(word);
		if (running)
			return 0;

		/* Sleeps one step. */
		kern_usleep_range(DISPLAY_STEP_US, DISPLAY_STEP_US + DISPLAY_STEP_US / 4U);
	}

	/* The timing generator did not start. */
	amdrdna_mark("reset: display fail: otg%u did not start (0x%08x)", device->gop.timing, (unsigned)word);
	return EIO;
}

/*
 * Turns on again the display interrupts the driver had on before the
 * reset (a frame's start, a finished flip, the monitor's hot-plug line),
 * acknowledging anything pending, as stage A6 does.
 *
 * Procedure referenced: display.c (display_interrupts) and Linux v6.19
 * drivers/gpu/drm/amd/display/dc/irq/irq_service.c (dal_irq_service_set:
 * acknowledged before it is enabled).
 */
static void
display_interrupts_again(
	struct amdrdna_device *device)
{
	struct amdrdna_display *display;
	unsigned long irq;
	uint32_t word;
	uint32_t status;
	bool level;

	/* Nothing to do when the driver had none on. */
	display = &device->display;
	if (!display->interrupts_on)
		return;

	/*
	 * The three, under the display's lock as the display's own switch
	 * takes them; the frame counter started again from 0, so the refresh
	 * count takes its next reading as a new base (it never goes back).
	 */
	irq = spin_lock_irqsave(&display->lock);

	display->frame_valid = false;

	word = display_read(device, AMDRDNA_DCN_TIMING_SYNC_STATUS, device->gop.timing);
	display_write(device, AMDRDNA_DCN_TIMING_SYNC_STATUS, device->gop.timing, amdrdna_sync_status_word(word, true, true));
	word = display_read(device, AMDRDNA_DCN_PLANE_FLIP_INTERRUPT, device->gop.plane);
	display_write(device, AMDRDNA_DCN_PLANE_FLIP_INTERRUPT, device->gop.plane, amdrdna_flip_interrupt_word(word, true, true));
	if (display->has_hotplug) {
		status = display_read(device, AMDRDNA_DCN_HOTPLUG_STATUS, display->hotplug);
		level = amdrdna_hotplug_connected(status);
		word = display_read(device, AMDRDNA_DCN_HOTPLUG_CONTROL, display->hotplug);
		display_write(device, AMDRDNA_DCN_HOTPLUG_CONTROL, display->hotplug, amdrdna_hotplug_control_word(word, true, level, true, true));
	}

	spin_unlock_irqrestore(&display->lock, irq);
}

/* Clears the plane's and the timing generator's underflow bits. */
static void
display_underflow_clear(
	struct amdrdna_device *device)
{
	uint32_t word;

	/* The plane's, then the timing generator's. */
	word = display_read(device, AMDRDNA_DCN_PLANE_CONTROL, device->gop.plane);
	display_write(device, AMDRDNA_DCN_PLANE_CONTROL, device->gop.plane, word | DISPLAY_PLANE_UNDERFLOW_CLEAR);
	word = display_read(device, AMDRDNA_DCN_COMBINER_INPUT, device->gop.timing);
	display_write(device, AMDRDNA_DCN_COMBINER_INPUT, device->gop.timing, word | DISPLAY_TIMING_UNDERFLOW_CLEAR);
}

/* Waits for a number of frames of the firmware's timing generator; gives the frames that passed. */
static uint32_t
display_frames(
	struct amdrdna_device *device,
	unsigned frames,
	uint32_t wait_us)
{
	uint32_t first;
	uint32_t now;
	uint32_t passed;
	unsigned waited;

	/* The frame counter until it moved on that far, or the time ran out. */
	first = display_read(device, AMDRDNA_DCN_TIMING_FRAME_COUNT, device->gop.timing);
	passed = 0U;
	for (waited = 0U; waited < wait_us; waited += DISPLAY_STEP_US) {
		now = display_read(device, AMDRDNA_DCN_TIMING_FRAME_COUNT, device->gop.timing);
		passed = (now - first) & DISPLAY_FRAME_MASK;
		if (passed >= frames)
			break;
		kern_usleep_range(DISPLAY_STEP_US, DISPLAY_STEP_US * 2U);
	}

	/* Succeeded: the frames that passed. */
	return passed;
}

/* Reads a display register (0 when the display block lacks it). */
static uint32_t
display_read(
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
display_write(
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

/* Reads one of the video BIOS's scratch words. */
static uint32_t
display_scratch(
	struct amdrdna_device *device,
	unsigned word)
{
	uint32_t value;

	/* The scratch words follow the first one. */
	value = amdrdna_block_read(device, AMDRDNA_BLOCK_NBIO, AMDRDNA_NBIO_WINDOW_SCRATCH, AMDRDNA_NBIO_REG_BIOS_SCRATCH_FIRST + word);

	/* Succeeded: the word. */
	return value;
}
