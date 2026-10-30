/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Stages A6 and A7: taking the display over from the firmware, and the
 * hardware side of the display the GPU device shows.
 *
 * The firmware lit one timing generator, fed by one plane that fetches the
 * console's surface.  The takeover keeps the timing, the link, the clocks
 * and the plane's size, format and pitch as the firmware left them, and
 * changes only the address the plane fetches (a flip, which takes effect
 * at the next frame's update) and the hardware cursor drawn over the
 * plane.  The firmware's surface stays the text console's: giving the
 * display up flips back to it, and the console's picture is there again,
 * except that the GPU device paints it black once its frames took the
 * screen (the boot logo is not shown again between them).
 * The driver starts no output of its own.
 *
 * Stage A6 saves the plane's words, takes the frames' VRAM, measures the
 * refresh from the frame counter and turns on the interrupts of a frame's
 * start, of a finished flip and of the hot-plug line.  Stage A7 takes the
 * cursor's images and sets the cursor up hidden.  Both run when the driver
 * owns the display (the default; amdrdna.disp.own=0 leaves it to the
 * firmware).  Neither draws on the screen: the diagnostic checks that did
 * (display_check_bars, a picture of colour bars flipped to and back, and
 * display_check_cursor, a cursor shown, moved and hidden;
 * amdrdna.disp.test=<seconds> holds them that long for an eye to check)
 * are kept, not called, for bringing up the display of another RDNA GPU.
 *
 * The interrupt thread acknowledges the display's events, counts them,
 * keeps the refresh count, follows the monitor's connection and wakes the
 * waiters.  Every wait also looks again at least once a scheduler tick, so
 * it ends even when no interrupt comes.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <kern/clock.h>
#include <kern/kcrt.h>
#include <kern/kmem.h>
#include <kern/lock.h>
#include <kern/poll.h>
#include <kern/sched.h>
#include <kern/uaccess.h>
#include <kern/waitq.h>
#include <uapi/amdrdna.h>
#include <uapi/errno.h>

#include "drivers/gpu/amdrdna/amdrdna-private.h"
#include "drivers/gpu/amdrdna/amdrdna-regs.h"

/*
 * The plane's pixel format of 32-bit colour (A, R, G, B from the top
 * byte down, B first in memory) and its linear tiling mode: the only
 * surface the takeover keeps showing.
 *
 * Values referenced: Linux v6.19
 * drivers/gpu/drm/amd/display/dc/hubp/dcn20/dcn20_hubp.c
 * (hubp2_program_pixel_format: SURFACE_PIXEL_FORMAT 8 for ARGB8888) and
 * drivers/gpu/drm/amd/display/dc/dc_hw_types.h (DC_SW_LINEAR).
 */
#define DISPLAY_FORMAT_32BIT		8U
#define DISPLAY_TILING_LINEAR		0U

/*
 * The plane's control word: bit 0 blanks the plane, bit 1 says it has no
 * request outstanding, bits 30:28 its underflow; and the combiner's input
 * control, whose bit 10 says the timing generator saw an underflow.
 *
 * Values referenced: Linux v6.19
 * drivers/gpu/drm/amd/include/asic_reg/dcn/dcn_2_0_0_sh_mask.h
 * (HUBP0_DCHUBP_CNTL__HUBP_BLANK_EN_MASK,
 * HUBP0_DCHUBP_CNTL__HUBP_NO_OUTSTANDING_REQ_MASK,
 * HUBP0_DCHUBP_CNTL__HUBP_UNDERFLOW_STATUS_MASK,
 * ODM0_OPTC_INPUT_GLOBAL_CONTROL__OPTC_UNDERFLOW_OCCURRED_STATUS_MASK).
 */
#define DISPLAY_PLANE_BLANK		0x00000001U

/*
 * The hub arbiter's DRAM state control: the self refresh's forced value
 * and its force (bits 0 and 1; forced to "not allowed" stops the memory's
 * self refresh), and the DRAM state change's forced value and its force
 * (bits 4 and 5).
 *
 * Values referenced: Linux v6.19
 * drivers/gpu/drm/amd/include/asic_reg/dcn/dcn_2_0_0_sh_mask.h
 * (DCHUBBUB_ARB_DRAM_STATE_CNTL__DCHUBBUB_ARB_ALLOW_SELF_REFRESH_FORCE_VALUE,
 * _ENABLE, DCHUBBUB_ARB_ALLOW_PSTATE_CHANGE_FORCE_VALUE, _ENABLE).
 */
#define DISPLAY_DRAM_SELF_REFRESH_FORCE	0x00000003U
#define DISPLAY_HUB_DEBUG_PSTATE	0x0000000bU
#define DISPLAY_DRAM_STATE_CHANGE_ALLOW	0x00000030U
#define DISPLAY_PLANE_IDLE		0x00000002U
#define DISPLAY_PLANE_UNDERFLOW_SHIFT	28U
#define DISPLAY_PLANE_UNDERFLOW_FIELD	0x00000007U
#define DISPLAY_TIMING_UNDERFLOW	0x00000400U

/*
 * The clears of the two underflows: bit 31 of the plane's control word and
 * bit 12 of the combiner's input control.
 *
 * Values referenced: Linux v6.19
 * drivers/gpu/drm/amd/include/asic_reg/dcn/dcn_2_0_0_sh_mask.h
 * (HUBP0_DCHUBP_CNTL__HUBP_UNDERFLOW_CLEAR_MASK,
 * ODM0_OPTC_INPUT_GLOBAL_CONTROL__OPTC_UNDERFLOW_CLEAR_MASK).
 */
#define DISPLAY_PLANE_UNDERFLOW_CLEAR	0x80000000U
#define DISPLAY_TIMING_UNDERFLOW_CLEAR	0x00001000U

/*
 * The cursor controls of the plane and of the pixel processor: bit 0
 * shows the cursor.  The pixel processor's bit 16 says a change of the
 * cursor still waits for the next frame's update.  Its scale word: 1.0 as
 * a 16-bit float, no bias.
 *
 * Values referenced: Linux v6.19
 * drivers/gpu/drm/amd/include/asic_reg/dcn/dcn_2_0_0_sh_mask.h
 * (CURSOR0_0_CURSOR_CONTROL__CURSOR_ENABLE_MASK,
 * CNVC_CUR0_CURSOR0_CONTROL__CUR0_ENABLE_MASK,
 * CNVC_CUR0_CURSOR0_CONTROL__CUR0_UPDATE_PENDING_MASK,
 * CNVC_CUR0_CURSOR0_FP_SCALE_BIAS__CUR0_FP_SCALE_MASK,
 * CNVC_CUR0_CURSOR0_FP_SCALE_BIAS__CUR0_FP_BIAS_MASK) and
 * drivers/gpu/drm/amd/display/dc/hwss/dcn10/dcn10_hwseq.c
 * (dcn10_set_cursor_sdr_white_level: the default multiplier 0x3c00).
 */
#define DISPLAY_CURSOR_ON		0x00000001U
#define DISPLAY_PIXEL_CURSOR_PENDING	0x00010000U
#define DISPLAY_PIXEL_CURSOR_SCALE_ONE	0x00003c00U

/*
 * The address of a surface: 48 bits, the high 16 in their own register.
 *
 * Values referenced: Linux v6.19
 * drivers/gpu/drm/amd/include/asic_reg/dcn/dcn_2_0_0_sh_mask.h
 * (HUBPREQ0_DCSURF_PRIMARY_SURFACE_ADDRESS_HIGH__PRIMARY_SURFACE_ADDRESS_HIGH_MASK,
 * CURSOR0_0_CURSOR_SURFACE_ADDRESS_HIGH__CURSOR_SURFACE_ADDRESS_HIGH_MASK).
 */
#define DISPLAY_ADDRESS_HIGH_SHIFT	32U
#define DISPLAY_ADDRESS_HIGH_FIELD	0x0000ffffU

/*
 * A frame's start: the plane reads from a 256-byte boundary and rows whose
 * pitch is a multiple of 256 bytes.
 *
 * Values referenced: Linux v6.19 drivers/gpu/drm/amd/amdgpu/amdgpu_display.c
 * (amdgpu_display_verify_sizes, amdgpu_display_verify_plane: a linear
 * surface's 256-byte block).
 * derived: the frames start on 64 KiB, the driver's own choice, a
 * multiple of 256 bytes.
 */
#define DISPLAY_PITCH_ALIGN		256U
#define DISPLAY_FRAME_ALIGN		0x10000ULL

/* The cursor: 64 by 64 pixels of 32-bit colour, two image slots on 4 KiB boundaries. */
#define DISPLAY_CURSOR_SIDE		64U
#define DISPLAY_CURSOR_BYTES		(DISPLAY_CURSOR_SIDE * DISPLAY_CURSOR_SIDE * 4U)
#define DISPLAY_CURSOR_SLOTS		2U
#define DISPLAY_CURSOR_ALIGN		4096ULL

/* The bytes of one pixel of a frame. */
#define DISPLAY_PIXEL_BYTES		4U

/*
 * The colours of the driver's own pictures, A8R8G8B8: alpha in the top
 * byte, then red, green and blue.
 */
#define DISPLAY_OPAQUE			(255U << 24)
#define DISPLAY_RED			(255U << 16)
#define DISPLAY_GREEN			(255U << 8)
#define DISPLAY_BLUE			255U

/* The colour the frames start with: opaque black. */
#define DISPLAY_BLACK			DISPLAY_OPAQUE

/* The colour bars of stage A6's picture, left to right: white, yellow, cyan, green, magenta, red, blue, black. */
#define DISPLAY_BARS			8U

/* The cursor image of stage A7: a white square with a black border three pixels wide, opaque. */
#define DISPLAY_CURSOR_WHITE		(DISPLAY_OPAQUE | DISPLAY_RED | DISPLAY_GREEN | DISPLAY_BLUE)
#define DISPLAY_CURSOR_BLACK		DISPLAY_OPAQUE
#define DISPLAY_CURSOR_BORDER		3U

/* How long a flip may take to begin (about six frames at 60 Hz), and a plane to go idle. */
#define DISPLAY_FLIP_WAIT_MS		100U
#define DISPLAY_IDLE_WAIT_US		100000U
#define DISPLAY_IDLE_STEP_US		100U

/*
 * The refresh measurement: the frame counter must move within 200 ms, and
 * 30 frames are timed (at most 1.5 s).
 */
#define DISPLAY_FIRST_FRAME_MS		200U
#define DISPLAY_MEASURED_FRAMES		30U
#define DISPLAY_MEASURE_MS		1500U
#define DISPLAY_POLL_MIN_US		500U
#define DISPLAY_POLL_MAX_US		1000U

/* The longest test hold the boot may ask for, in seconds; the shortest hold of stage A6 is one second. */
#define DISPLAY_TEST_MAX_SECONDS	60U
#define DISPLAY_HOLD_MS			1000U

/* How many frames the cursor checks wait for a change to show. */
#define DISPLAY_SETTLE_FRAMES		2U

/* The longest row the screen readout copies, in words: 16 KiB (4096 pixels). */
#define DISPLAY_SCREEN_ROW_WORDS	4096U

/* The pages a mapping of the VRAM BAR is made of for the screen readout. */
#define DISPLAY_SCREEN_PAGE_BYTES	4096U

/* The first lines of the topology: one, before any change of the monitor. */
#define DISPLAY_TOPOLOGY_FIRST		1U

static uint32_t display_index(struct amdrdna_device *device, enum amdrdna_dcn_register which, unsigned instance);
static uint32_t display_read(struct amdrdna_device *device, enum amdrdna_dcn_register which, unsigned instance);
static void display_write(struct amdrdna_device *device, enum amdrdna_dcn_register which, unsigned instance, uint32_t value);
static int takeover_check(struct amdrdna_device *device);
static void takeover_save(struct amdrdna_device *device);
static int takeover_memory(struct amdrdna_device *device);
static void takeover_memory_give(struct amdrdna_device *device);
static int takeover_measure(struct amdrdna_device *device);
static void takeover_bars(struct amdrdna_device *device, unsigned frame);
static int display_check_bars(struct amdrdna_device *device) __attribute__((unused));
static int display_check_cursor(struct amdrdna_device *device) __attribute__((unused));
static void takeover_give_back(struct amdrdna_device *device);
static void display_interrupts(struct amdrdna_device *device, bool on);
static void display_sample_locked(struct amdrdna_device *device);
static void display_hotplug(struct amdrdna_device *device, unsigned line);
static uint64_t display_shown_address(struct amdrdna_device *device);
static void display_cursor_setup(struct amdrdna_device *device);
static void display_cursor_square(struct amdrdna_device *device, unsigned slot);
static int display_wait_frames(struct amdrdna_device *device, unsigned frames);
static unsigned display_test_seconds(void);
static uint32_t display_underflow(struct amdrdna_device *device, uint32_t *timing_underflow);

/* Reports whether the driver owns the display: by default, unless the boot says amdrdna.disp.own=0. */
bool
amdrdna_display_wanted(void)
{
	unsigned own;

	/* The boot word, on by default. */
	own = amdrdna_parameter_number("amdrdna.disp.own", 1U);
	if (own != 0U)
		return true;

	/* The display stays the firmware's. */
	return false;
}

/*
 * Runs stage A6: takes the firmware's plane over, keeping the console's
 * picture on the screen.
 *
 * Returns 0 when the plane is the driver's (or the display stays the
 * firmware's), ENOTSUP for a picture the takeover does not keep, ENOMEM
 * without VRAM for the frames, or EIO when the frame counter does not
 * move.  A failure leaves the firmware's picture and the plane's words as
 * they were.
 */
int
amdrdna_a6_takeover(
	struct amdrdna_device *device)
{
	struct amdrdna_display *display;
	bool wanted;
	int error;

	/* Says the stage starts. */
	amdrdna_mark("A6 begin");
	display = &device->display;

	/* With amdrdna.disp.own=0 the firmware keeps its display. */
	wanted = amdrdna_display_wanted();
	if (!wanted) {
		amdrdna_mark("A6 skip: the display stays the firmware's (amdrdna.disp.own=0)");
		return 0;
	}

	/* The firmware's picture must be one the takeover keeps. */
	error = takeover_check(device);
	if (error != 0)
		return error;

	/* Saves the plane's words; the stop writes them back. */
	takeover_save(device);

	/* The frames and the cursor's images. */
	error = takeover_memory(device);
	if (error != 0) {
		amdrdna_mark("A6 fail: no VRAM for the frames (%d)", error);
		takeover_give_back(device);
		return error;
	}

	/* The refresh, from the frame counter. */
	error = takeover_measure(device);
	if (error != 0) {
		takeover_give_back(device);
		return error;
	}

	/* A new address takes effect at the next frame's update, read without a context. */
	display_write(device, AMDRDNA_DCN_PLANE_SPACE_SELECT, device->gop.plane, 0U);
	display_write(device, AMDRDNA_DCN_PLANE_FLIP_CONTROL, device->gop.plane, amdrdna_flip_control_word(display->saved_flip_control));

	/* The interrupts of a frame's start, of a finished flip and of the hot-plug line. */
	display_interrupts(device, true);

	/*
	 * Diagnostic, not run: the colour bars' check (display_check_bars)
	 * flipped the plane to a test picture for a second at every boot and
	 * back, timing the flip and counting the refreshes and the interrupts
	 * on the screen.  It is kept for bringing up the display of another
	 * RDNA GPU (RDNA2, RDNA3), whose takeover it checks by eye and by the
	 * counts; the call to put back is:
	 *
	 *	error = display_check_bars(device);
	 *	if (error != 0) {
	 *		takeover_give_back(device);
	 *		return error;
	 *	}
	 */

	/* Marks the result. */
	amdrdna_mark("A6 ok refresh=%u.%03uHz %ux%u stride=%u hotplug=%d",
	    (unsigned)(display->refresh_millihz / 1000U),
	    (unsigned)(display->refresh_millihz % 1000U),
	    (unsigned)display->width,
	    (unsigned)display->height,
	    (unsigned)display->stride,
	    display->has_hotplug ? (int)display->hotplug : -1);

	/* Succeeded: the plane is the driver's, showing the console. */
	return 0;
}

/*
 * Diagnostic, not called (amdrdna_a6_takeover keeps the call commented
 * out): flips the taken plane to a picture of colour bars, holds it a
 * second (amdrdna.disp.test=<seconds> longer, for an eye to check), flips
 * back to the console's surface and marks how long the flip took to
 * begin, the refreshes and the frame and flip interrupts over the hold,
 * and the underflows.  For bringing up the display of another RDNA GPU.
 *
 * Returns 0, or ETIMEDOUT when a flip did not begin (the caller gives the
 * display back).
 */
__attribute__((unused)) static int
display_check_bars(
	struct amdrdna_device *device)
{
	struct amdrdna_display *display;
	uint64_t started_us;
	uint64_t latch_us;
	uint64_t signals_before;
	uint64_t signals_after;
	uint64_t flips_before;
	uint64_t flips_after;
	uint64_t count_before;
	uint64_t count_after;
	uint64_t time_ns;
	uint64_t back;
	unsigned seconds;
	unsigned hold_ms;
	uint32_t underflow;
	uint32_t timing_underflow;
	unsigned long irq;
	int error;

	/* The display the takeover holds. */
	display = &device->display;

	/* The colour bars, written through the window and pushed past the host path. */
	takeover_bars(device, 0U);
	amdrdna_host_path_flush(device);

	/* The flip interrupts so far. */
	irq = spin_lock_irqsave(&display->lock);

	flips_before = display->flip_signals;

	spin_unlock_irqrestore(&display->lock, irq);

	/* Flips to the bars and times how long the flip takes to begin. */
	started_us = amdrdna_display_now_us();
	error = amdrdna_display_show(device, amdrdna_display_address(device, display->frames[0]));
	latch_us = amdrdna_display_now_us() - started_us;
	if (error != 0) {
		amdrdna_mark("A6 fail: the flip to the driver's frame did not begin in %ums (%d)", DISPLAY_FLIP_WAIT_MS, error);
		return error;
	}

	/* The hold: at least a second, longer when the boot asks. */
	seconds = display_test_seconds();
	hold_ms = DISPLAY_HOLD_MS;
	if (seconds != 0U)
		hold_ms = seconds * 1000U;

	/* The frame interrupts before the hold. */
	irq = spin_lock_irqsave(&display->lock);

	signals_before = display->frame_signals;

	spin_unlock_irqrestore(&display->lock, irq);

	/* Holds the bars, counting the refreshes over the hold. */
	amdrdna_display_refresh_sample(device, &count_before, &time_ns);
	kern_usleep_range(hold_ms * 1000U, hold_ms * 1000U + 1000U);
	amdrdna_display_refresh_sample(device, &count_after, &time_ns);

	/* The frame interrupts over the hold. */
	irq = spin_lock_irqsave(&display->lock);

	signals_after = display->frame_signals;

	spin_unlock_irqrestore(&display->lock, irq);

	/* Flips back to the firmware's surface, the console's. */
	error = amdrdna_display_show(device, display->console);
	if (error != 0) {
		amdrdna_mark("A6 fail: the flip back to the console did not begin (%d)", error);
		return error;
	}

	/* The address the plane reads now, and the underflows. */
	back = display_shown_address(device);
	underflow = display_underflow(device, &timing_underflow);

	/* The flip interrupts of the two flips, after a frame lets the interrupt thread take them. */
	(void)display_wait_frames(device, 1U);
	irq = spin_lock_irqsave(&display->lock);

	flips_after = display->flip_signals;

	spin_unlock_irqrestore(&display->lock, irq);

	/* Marks the result. */
	amdrdna_mark("A6 bars ok flip pending=0 latch=%lluus back=%u hold=%ums frames=%llu frame_irq=%llu flip_irq=%llu/2 refresh=%u.%03uHz %ux%u stride=%u underflow=%u/%u hotplug=%d",
	    (unsigned long long)latch_us,
	    back == display->console ? 1U : 0U,
	    hold_ms,
	    (unsigned long long)(count_after - count_before),
	    (unsigned long long)(signals_after - signals_before),
	    (unsigned long long)(flips_after - flips_before),
	    (unsigned)(display->refresh_millihz / 1000U),
	    (unsigned)(display->refresh_millihz % 1000U),
	    (unsigned)display->width,
	    (unsigned)display->height,
	    (unsigned)display->stride,
	    (unsigned)underflow,
	    (unsigned)timing_underflow,
	    display->has_hotplug ? (int)display->hotplug : -1);

	/* Succeeded: the plane flipped there and back. */
	return 0;
}

/*
 * Runs stage A7: sets the hardware cursor up, hidden.
 *
 * Returns 0 when the cursor is set up (or the display stays the
 * firmware's), or ENODEV without stage A6's takeover.
 */
int
amdrdna_a7_cursor(
	struct amdrdna_device *device)
{
	struct amdrdna_display *display;
	bool wanted;

	/* Says the stage starts. */
	amdrdna_mark("A7 begin");
	display = &device->display;

	/* With amdrdna.disp.own=0 the firmware keeps its display. */
	wanted = amdrdna_display_wanted();
	if (!wanted) {
		amdrdna_mark("A7 skip: the display stays the firmware's (amdrdna.disp.own=0)");
		return 0;
	}

	/* The cursor is drawn over the plane stage A6 took. */
	if (!display->taken) {
		amdrdna_mark("A7 fail: the display was not taken over (stage A6)");
		return ENODEV;
	}

	/* The cursor set up hidden on the first slot. */
	display_cursor_setup(device);
	amdrdna_display_cursor_use(device, 0U);

	/*
	 * Diagnostic, not run: the cursor's check (display_check_cursor)
	 * painted a test square, showed it in the middle of the screen, moved
	 * it down a diagonal for a second and hid it again at every boot.  It
	 * is kept for bringing up the display of another RDNA GPU (RDNA2,
	 * RDNA3), whose hardware cursor it checks on the screen; the call to
	 * put back is:
	 *
	 *	error = display_check_cursor(device);
	 *	if (error != 0)
	 *		return error;
	 */

	/* The cursor is ready for the GPU device's lease. */
	display->cursor_ready = true;

	/* Marks the result. */
	amdrdna_mark("A7 ok cursor %ux%u at 0x%llx hidden",
	    DISPLAY_CURSOR_SIDE,
	    DISPLAY_CURSOR_SIDE,
	    (unsigned long long)amdrdna_display_address(device, display->cursor_vram));

	/* Succeeded: the cursor is set up, hidden. */
	return 0;
}

/*
 * Diagnostic, not called (amdrdna_a7_cursor keeps the call commented
 * out): paints a test square into the cursor's first slot, shows it in
 * the middle of the screen, moves it down a diagonal one step a frame for
 * a second (amdrdna.disp.test=<seconds> longer) and hides it again,
 * checking at each end that the plane and the pixel processor took the
 * change, and marks the moves.  For bringing up the display of another
 * RDNA GPU.
 *
 * Returns 0, or EIO when a cursor change did not reach the pixel
 * processor.
 */
__attribute__((unused)) static int
display_check_cursor(
	struct amdrdna_device *device)
{
	struct amdrdna_display *display;
	uint32_t control;
	uint32_t pixel;
	unsigned seconds;
	unsigned moves;
	unsigned step;
	unsigned steps;
	int32_t x;
	int32_t y;
	bool shown;
	bool hidden;
	int error;

	/* The display the takeover holds. */
	display = &device->display;

	/* The test image in the first slot, which the set-up cursor reads. */
	display_cursor_square(device, 0U);
	amdrdna_host_path_flush(device);

	/* Shows it in the middle of the screen and lets a frame take it. */
	x = (int32_t)(display->width / 2U) - (int32_t)(DISPLAY_CURSOR_SIDE / 2U);
	y = (int32_t)(display->height / 2U) - (int32_t)(DISPLAY_CURSOR_SIDE / 2U);
	amdrdna_display_cursor_place(device, x, y);
	error = display_wait_frames(device, DISPLAY_SETTLE_FRAMES);
	if (error != 0) {
		amdrdna_mark("A7 fail: no frame after the cursor was shown (%d)", error);
		amdrdna_display_cursor_hide(device);
		return EIO;
	}

	/* The plane and the pixel processor both show it, with nothing left pending. */
	control = display_read(device, AMDRDNA_DCN_CURSOR_CONTROL, device->gop.plane);
	pixel = display_read(device, AMDRDNA_DCN_PIXEL_CURSOR_CONTROL, device->gop.plane);
	shown = false;
	if ((control & DISPLAY_CURSOR_ON) != 0U &&
	    (pixel & DISPLAY_CURSOR_ON) != 0U &&
	    (pixel & DISPLAY_PIXEL_CURSOR_PENDING) == 0U)
		shown = true;
	if (!shown) {
		amdrdna_mark("A7 fail: the cursor is not shown (plane 0x%08x, pixel processor 0x%08x)", (unsigned)control, (unsigned)pixel);
		amdrdna_display_cursor_hide(device);
		return EIO;
	}

	/* Moves it down a diagonal, one step a frame, over the test hold (a second without one). */
	seconds = display_test_seconds();
	steps = display->refresh_millihz / 1000U;
	if (seconds != 0U)
		steps = steps * seconds;
	moves = 0U;
	for (step = 0U; step < steps; step++) {
		/* The next position along the diagonal, inside the screen. */
		x = (int32_t)(((uint64_t)step * (display->width - DISPLAY_CURSOR_SIDE)) / steps);
		y = (int32_t)(((uint64_t)step * (display->height - DISPLAY_CURSOR_SIDE)) / steps);
		amdrdna_display_cursor_place(device, x, y);
		moves++;

		/* One frame shows each position. */
		error = display_wait_frames(device, 1U);
		if (error != 0)
			break;
	}

	/* Hides it and lets a frame take that. */
	amdrdna_display_cursor_hide(device);
	(void)display_wait_frames(device, DISPLAY_SETTLE_FRAMES);
	control = display_read(device, AMDRDNA_DCN_CURSOR_CONTROL, device->gop.plane);
	pixel = display_read(device, AMDRDNA_DCN_PIXEL_CURSOR_CONTROL, device->gop.plane);
	hidden = false;
	if ((control & DISPLAY_CURSOR_ON) == 0U && (pixel & DISPLAY_CURSOR_ON) == 0U)
		hidden = true;
	if (!hidden) {
		amdrdna_mark("A7 fail: the cursor did not hide (plane 0x%08x, pixel processor 0x%08x)", (unsigned)control, (unsigned)pixel);
		return EIO;
	}

	/* Marks the moves. */
	amdrdna_mark("A7 cursor check ok shown=1 pending=0 moves=%u hidden=1", moves);

	/* Succeeded: the cursor showed, moved and hid. */
	return 0;
}


/*
 * Gives the display back to the firmware's state for the reboot: the
 * cursor hidden, the plane unblanked and flipped back to the console's
 * surface, the plane's and the cursor's words as the takeover found them,
 * and the display's interrupts off.  The GPU device's lease has ended
 * before (amdrdna_node_stop).
 */
void
amdrdna_display_stop(
	struct amdrdna_device *device)
{
	/* A display never taken over is the firmware's already. */
	if (!device->display.taken)
		return;

	/* Puts everything back. */
	takeover_give_back(device);
	amdrdna_mark("shutdown: the display is the firmware's again");
}

/*
 * Takes one event of the display engine off the interrupt ring (the
 * interrupt thread): acknowledges it, counts it, and wakes the waiters.
 * A frame's start also brings the refresh count up to date; a plug or an
 * unplug of the monitor moves the topology.
 */
void
amdrdna_display_interrupt(
	struct amdrdna_device *device,
	const struct amdrdna_interrupt *interrupt)
{
	struct amdrdna_display *display;
	unsigned long irq;
	uint32_t word;
	unsigned which;

	/* Before the takeover the display's events are not the driver's. */
	display = &device->display;
	if (!display->interrupts_on)
		return;

	/* A frame's start of the console's timing generator. */
	if (interrupt->source >= AMDRDNA_SOURCE_FRAME_START &&
	    interrupt->source < AMDRDNA_SOURCE_FRAME_START + AMDRDNA_DISPLAY_PIPES) {
		which = interrupt->source - AMDRDNA_SOURCE_FRAME_START;
		if (which != device->gop.timing)
			return;

		/* Acknowledges, counts, brings the refresh count up to date and wakes the waiters. */
		irq = spin_lock_irqsave(&display->lock);

		word = display_read(device, AMDRDNA_DCN_TIMING_SYNC_STATUS, which);
		display_write(device, AMDRDNA_DCN_TIMING_SYNC_STATUS, which, amdrdna_sync_status_word(word, true, true));
		display->frame_signals++;
		display_sample_locked(device);
		waitq_wake_all(&display->waitq);

		spin_unlock_irqrestore(&display->lock, irq);
		return;
	}

	/* A finished flip of the console's plane. */
	if (interrupt->source >= AMDRDNA_SOURCE_FLIP_DONE &&
	    interrupt->source < AMDRDNA_SOURCE_FLIP_DONE + AMDRDNA_DISPLAY_PIPES) {
		which = interrupt->source - AMDRDNA_SOURCE_FLIP_DONE;
		if (which != device->gop.plane)
			return;

		/* Acknowledges, counts and wakes the waiters. */
		irq = spin_lock_irqsave(&display->lock);

		word = display_read(device, AMDRDNA_DCN_PLANE_FLIP_INTERRUPT, which);
		display_write(device, AMDRDNA_DCN_PLANE_FLIP_INTERRUPT, which, amdrdna_flip_interrupt_word(word, true, true));
		display->flip_signals++;
		waitq_wake_all(&display->waitq);

		spin_unlock_irqrestore(&display->lock, irq);
		return;
	}

	/* A hot-plug line, named in the first data word. */
	if (interrupt->source == AMDRDNA_SOURCE_HOTPLUG)
		display_hotplug(device, interrupt->data[0] & 0xffU);
}

/* Reports the GPU address the plane reads a VRAM offset at. */
uint64_t
amdrdna_display_address(
	const struct amdrdna_device *device,
	uint64_t vram)
{
	/* The frame buffer's base in the GPU's address space, plus the offset. */
	return device->frame_buffer_base + vram;
}

/*
 * Flips the plane to a surface and waits until the flip has begun.
 *
 * Returns 0, or ETIMEDOUT when it did not begin in time.
 */
int
amdrdna_display_show(
	struct amdrdna_device *device,
	uint64_t address)
{
	int error;

	/* Asks for the flip. */
	error = amdrdna_display_flip(device, address);
	if (error != 0)
		return error;

	/* Waits for the frame's update that takes it. */
	error = amdrdna_display_flip_wait(device, address);
	if (error != 0)
		return error;

	/* Succeeded: the plane reads the surface. */
	return 0;
}

/*
 * Asks the plane to read a surface from the next frame's update on: the
 * high word of the address first, then the low word, which makes the
 * whole address take effect.
 *
 * Procedure referenced: Linux v6.19
 * drivers/gpu/drm/amd/display/dc/hubp/dcn20/dcn20_hubp.c
 * (hubp2_program_surface_flip_and_addr: the high word before the low).
 *
 * Returns 0.
 */
int
amdrdna_display_flip(
	struct amdrdna_device *device,
	uint64_t address)
{
	struct amdrdna_display *display;
	unsigned long irq;
	uint32_t high;
	uint32_t low;

	/* The two words of the address. */
	display = &device->display;
	high = (uint32_t)(address >> DISPLAY_ADDRESS_HIGH_SHIFT) & DISPLAY_ADDRESS_HIGH_FIELD;
	low = (uint32_t)address;

	/* The high word, then the low word that latches both. */
	display_write(device, AMDRDNA_DCN_PLANE_ADDRESS_HIGH, device->gop.plane, high);
	display_write(device, AMDRDNA_DCN_PLANE_ADDRESS_LOW, device->gop.plane, low);

	/* Records what was asked for. */
	irq = spin_lock_irqsave(&display->lock);

	display->requested = address;
	display->flips++;

	spin_unlock_irqrestore(&display->lock, irq);

	/* Succeeded: the flip waits for the next frame's update. */
	return 0;
}

/*
 * Waits until the plane reads a surface: no flip pending and the address
 * the plane reads is the one asked for.
 *
 * Procedure referenced: Linux v6.19
 * drivers/gpu/drm/amd/display/dc/hubp/dcn20/dcn20_hubp.c
 * (hubp2_is_flip_pending).
 *
 * Returns 0 (at once for a blanked plane), or ETIMEDOUT after about six
 * frames.
 */
int
amdrdna_display_flip_wait(
	struct amdrdna_device *device,
	uint64_t address)
{
	struct amdrdna_display *display;
	uint64_t deadline;
	uint64_t sequence;
	uint64_t shown;
	uint64_t now;
	unsigned long irq;
	uint32_t control;
	bool pending;

	/*
	 * A blanked plane fetches nothing, so it never shows a flip begin: the
	 * address it was given is taken when it is shown again.
	 */
	display = &device->display;
	if (display->blanked)
		return 0;

	/* The time the flip has to begin. */
	deadline = sched_ticks() + kern_ms_to_ticks(DISPLAY_FLIP_WAIT_MS);

	/* Looks at the plane after every flip interrupt, and at least each tick. */
	for (;;) {
		/* Whether a flip waits, and what the plane reads. */
		control = display_read(device, AMDRDNA_DCN_PLANE_FLIP_CONTROL, device->gop.plane);
		pending = amdrdna_flip_pending(control);
		shown = display_shown_address(device);
		if (!pending && shown == address)
			break;

		/* Gives up when the flip did not begin in time. */
		now = sched_ticks();
		if (now >= deadline)
			return ETIMEDOUT;

		/* Sleeps until the next display event or the next tick. */
		irq = spin_lock_irqsave(&display->lock);

		sequence = waitq_sequence(&display->waitq);
		(void)waitq_sleep(&display->waitq, &display->lock, sequence, now + 1U, 0U);

		spin_unlock_irqrestore(&display->lock, irq);
	}

	/* Succeeded: the plane reads the surface. */
	return 0;
}

/*
 * Brings the refresh count up to date from the timing generator's frame
 * counter and reports it with the time it last moved (nanoseconds on the
 * scheduler's clock).  Any context: takes only the display's IRQ-safe lock.
 */
void
amdrdna_display_refresh_sample(
	struct amdrdna_device *device,
	uint64_t *count,
	uint64_t *time_ns)
{
	struct amdrdna_display *display;
	unsigned long irq;

	/* Reads the counter and reports the count. */
	display = &device->display;
	irq = spin_lock_irqsave(&display->lock);

	display_sample_locked(device);
	*count = display->refresh_count;
	*time_ns = display->refresh_time_ns;

	spin_unlock_irqrestore(&display->lock, irq);
}

/*
 * Waits for a refresh after cursor (or reports the count at once for
 * cursor 0), until a deadline in scheduler ticks.
 *
 * Returns 0 with the count and its time, or ETIMEDOUT.
 */
int
amdrdna_display_refresh_wait(
	struct amdrdna_device *device,
	uint64_t cursor,
	uint64_t deadline,
	uint64_t *count,
	uint64_t *time_ns)
{
	struct amdrdna_display *display;
	uint64_t sequence;
	uint64_t now;
	unsigned long irq;

	/* Looks at the counter after every frame interrupt, and at least each tick. */
	display = &device->display;
	for (;;) {
		/* The count now. */
		amdrdna_display_refresh_sample(device, count, time_ns);

		/* Cursor zero, or a refresh after the cursor: reported. */
		if (cursor == 0U || *count > cursor)
			break;

		/* No refresh came in time. */
		now = sched_ticks();
		if (now >= deadline)
			return ETIMEDOUT;

		/* Sleeps until the next display event or the next tick. */
		irq = spin_lock_irqsave(&display->lock);

		sequence = waitq_sequence(&display->waitq);
		(void)waitq_sleep(&display->waitq, &display->lock, sequence, now + 1U, 0U);

		spin_unlock_irqrestore(&display->lock, irq);
	}

	/* Succeeded: the refresh after the cursor. */
	return 0;
}

/*
 * Shows the cursor with its image's top left at a position of the plane,
 * or hides it when the image lies wholly outside; the change takes effect
 * at the next frame's update.  The caller holds the node's display lock or
 * runs the stage.
 *
 * Procedure referenced: Linux v6.19
 * drivers/gpu/drm/amd/display/dc/hubp/dcn20/dcn20_hubp.c
 * (hubp2_cursor_set_position) and
 * drivers/gpu/drm/amd/display/dc/dpp/dcn10/dcn10_dpp.c
 * (dpp1_set_cursor_position): the plane's cursor first, then the pixel
 * processor's.
 */
void
amdrdna_display_cursor_place(
	struct amdrdna_device *device,
	int32_t x,
	int32_t y)
{
	struct amdrdna_display *display;
	struct amdrdna_cursor_place place;
	uint32_t word;

	/* Where the image lies against the plane. */
	display = &device->display;
	amdrdna_cursor_locate(x, y, DISPLAY_CURSOR_SIDE, display->width, display->height, &place);

	/* An image wholly outside the plane hides the cursor. */
	if (!place.shown) {
		amdrdna_display_cursor_hide(device);
		return;
	}

	/* The position and the hot spot; the fetch starts with the line (no timing offset). */
	display_write(device, AMDRDNA_DCN_CURSOR_POSITION, device->gop.plane, place.position);
	display_write(device, AMDRDNA_DCN_CURSOR_HOT_SPOT, device->gop.plane, place.hot_spot);
	display_write(device, AMDRDNA_DCN_CURSOR_DESTINATION, device->gop.plane, 0U);

	/* A hidden cursor is shown by the plane, then by the pixel processor. */
	if (!display->cursor_shown) {
		word = display_read(device, AMDRDNA_DCN_CURSOR_CONTROL, device->gop.plane);
		display_write(device, AMDRDNA_DCN_CURSOR_CONTROL, device->gop.plane, amdrdna_cursor_control_word(word, true));
		word = display_read(device, AMDRDNA_DCN_PIXEL_CURSOR_CONTROL, device->gop.plane);
		display_write(device, AMDRDNA_DCN_PIXEL_CURSOR_CONTROL, device->gop.plane, amdrdna_pixel_cursor_word(word, true));
		display->cursor_shown = true;
	}
}

/* Hides the cursor from the next frame's update on; its image stays. */
void
amdrdna_display_cursor_hide(
	struct amdrdna_device *device)
{
	uint32_t word;

	/* The plane's cursor, then the pixel processor's. */
	word = display_read(device, AMDRDNA_DCN_CURSOR_CONTROL, device->gop.plane);
	display_write(device, AMDRDNA_DCN_CURSOR_CONTROL, device->gop.plane, amdrdna_cursor_control_word(word, false));
	word = display_read(device, AMDRDNA_DCN_PIXEL_CURSOR_CONTROL, device->gop.plane);
	display_write(device, AMDRDNA_DCN_PIXEL_CURSOR_CONTROL, device->gop.plane, amdrdna_pixel_cursor_word(word, false));

	/* The cursor is hidden. */
	device->display.cursor_shown = false;
}

/* Reports where the CPU writes one of the cursor's two image slots (64 by 64, 256 bytes a row). */
volatile uint8_t *
amdrdna_display_cursor_cpu(
	struct amdrdna_device *device,
	unsigned slot)
{
	volatile uint8_t *cpu;

	/* The slot's place in the window. */
	cpu = amdrdna_vram_cpu(device, device->display.cursor_vram + (uint64_t)slot * DISPLAY_CURSOR_BYTES);

	/* Succeeded: the slot's first byte. */
	return cpu;
}

/*
 * Points the cursor at one of its image slots, the high word of the
 * address first; the caller pushed the slot's writes past the host path.
 *
 * Procedure referenced: Linux v6.19
 * drivers/gpu/drm/amd/display/dc/hubp/dcn20/dcn20_hubp.c
 * (hubp2_cursor_set_attributes: the high word before the low).
 */
void
amdrdna_display_cursor_use(
	struct amdrdna_device *device,
	unsigned slot)
{
	uint64_t address;

	/* The slot's GPU address. */
	address = amdrdna_display_address(device, device->display.cursor_vram + (uint64_t)slot * DISPLAY_CURSOR_BYTES);

	/* The high word, then the low. */
	display_write(device, AMDRDNA_DCN_CURSOR_ADDRESS_HIGH, device->gop.plane, (uint32_t)(address >> DISPLAY_ADDRESS_HIGH_SHIFT) & DISPLAY_ADDRESS_HIGH_FIELD);
	display_write(device, AMDRDNA_DCN_CURSOR_ADDRESS_LOW, device->gop.plane, (uint32_t)address);

	/* The slot the cursor reads from now on. */
	device->display.cursor_slot = slot;
}

/* Reports the plane's underflow bits and the timing generator's underflow (for the mode set's checks). */
uint32_t
amdrdna_display_underflow(
	struct amdrdna_device *device,
	uint32_t *timing_underflow)
{
	uint32_t underflow;

	/* The plane's and the timing generator's. */
	underflow = display_underflow(device, timing_underflow);

	/* Succeeded: the plane's underflow bits. */
	return underflow;
}

/*
 * Reports the plane's underflow bits and the timing generator's underflow
 * as amdrdna_display_underflow() does, and clears those that are set, so
 * the next report shows only underflows that came after.
 *
 * Procedure referenced: Linux v6.19
 * drivers/gpu/drm/amd/display/dc/hubp/dcn20/dcn20_hubp.c
 * (hubp2_clear_underflow) and
 * drivers/gpu/drm/amd/display/dc/optc/dcn10/dcn10_optc.c
 * (optc1_clear_optc_underflow).
 */
uint32_t
amdrdna_display_underflow_take(
	struct amdrdna_device *device,
	uint32_t *timing_underflow)
{
	uint32_t underflow;
	uint32_t word;

	/* The bits as they stand. */
	underflow = display_underflow(device, timing_underflow);

	/* The plane's cleared, when set. */
	if (underflow != 0U) {
		word = display_read(device, AMDRDNA_DCN_PLANE_CONTROL, device->gop.plane);
		display_write(device, AMDRDNA_DCN_PLANE_CONTROL, device->gop.plane, word | DISPLAY_PLANE_UNDERFLOW_CLEAR);
	}

	/* The timing generator's cleared, when set. */
	if (*timing_underflow != 0U) {
		word = display_read(device, AMDRDNA_DCN_COMBINER_INPUT, device->gop.timing);
		display_write(device, AMDRDNA_DCN_COMBINER_INPUT, device->gop.timing, word | DISPLAY_TIMING_UNDERFLOW_CLEAR);
	}

	/* Succeeded: the bits before the clear. */
	return underflow;
}

/*
 * Lets the memory clock change once while the plane scans out: the hub
 * arbiter allows the DRAM's state change whatever its watermarks say, and
 * the memory's self refresh is no longer kept off, as Linux leaves the
 * arbiter once its display code has started.  Returns the arbiter's word
 * before, which amdrdna_display_memory_switch_end() puts back.  The
 * system-management unit's firmware waits for the arbiter while it
 * changes the memory clock.
 *
 * Procedure referenced: Linux v6.19
 * drivers/gpu/drm/amd/display/dc/hubbub/dcn10/dcn10_hubbub.c
 * (hubbub1_verify_allow_pstate_change_high: the forced allowing of the
 * state change "to prevent system hang"; hubbub1_allow_self_refresh_control:
 * the self refresh allowed with the force off).
 */
uint32_t
amdrdna_display_memory_switch_begin(
	struct amdrdna_device *device)
{
	uint32_t saved;
	uint32_t word;

	/* The word as the firmware or the last switch left it. */
	saved = display_read(device, AMDRDNA_DCN_HUB_DRAM_STATE, 0U);

	/* The state change allowed by force, the self refresh by the watermarks. */
	word = (saved & ~DISPLAY_DRAM_SELF_REFRESH_FORCE) | DISPLAY_DRAM_STATE_CHANGE_ALLOW;
	display_write(device, AMDRDNA_DCN_HUB_DRAM_STATE, 0U, word);

	/* Succeeded: the word before. */
	return saved;
}

/* Puts the hub arbiter's DRAM state control back as it was before the memory clock's change. */
void
amdrdna_display_memory_switch_end(
	struct amdrdna_device *device,
	uint32_t saved)
{
	/* The word before. */
	display_write(device, AMDRDNA_DCN_HUB_DRAM_STATE, 0U, saved);
}

/*
 * Puts the arbiter's word back as amdrdna_display_memory_switch_end() does,
 * but with the DRAM's state change still allowed by force (the self
 * refresh as it was): a later change of the memory clock the firmware
 * makes by itself does not wait for the display.
 */
void
amdrdna_display_memory_switch_hold(
	struct amdrdna_device *device,
	uint32_t saved)
{
	/* The word before, the state change allowed. */
	display_write(device, AMDRDNA_DCN_HUB_DRAM_STATE, 0U, saved | DISPLAY_DRAM_STATE_CHANGE_ALLOW);
}

/*
 * Puts the arbiter's word back as amdrdna_display_memory_switch_end() does,
 * but with neither force: the memory's self refresh and its state change
 * as the watermarks allow them, the word Linux's display code leaves.
 *
 * Procedure referenced: Linux v6.12
 * drivers/gpu/drm/amd/display/dc/hubbub/dcn10/dcn10_hubbub.c
 * (hubbub1_allow_self_refresh_control with the self refresh allowed) and
 * drivers/gpu/drm/amd/display/dc/hubbub/dcn20/dcn20_hubbub.c (the state
 * change is never forced on DCN 2.0).
 */
void
amdrdna_display_memory_switch_release(
	struct amdrdna_device *device,
	uint32_t saved)
{
	/* The word before, both forces off. */
	display_write(device, AMDRDNA_DCN_HUB_DRAM_STATE, 0U, saved & ~(DISPLAY_DRAM_SELF_REFRESH_FORCE | DISPLAY_DRAM_STATE_CHANGE_ALLOW));
}

/* Reads the display hub's memory state word (DCHUBBUB_ARB_DRAM_STATE_CNTL) as it is. */
uint32_t
amdrdna_display_memory_state(
	struct amdrdna_device *device)
{
	/* The word. */
	return display_read(device, AMDRDNA_DCN_HUB_DRAM_STATE, 0U);
}

/*
 * Reads the hub's memory state handshake as its test debug word shows it
 * (index 0xB): bits 0 to 27 each plane's and cursor's allowing of the
 * memory's state change, bits 28 and 29 the write-back's, bit 30 the
 * arbiter's allowing, and bit 31 the request of the system-management
 * unit's firmware for the change.
 *
 * Values referenced: Linux v6.12
 * drivers/gpu/drm/amd/display/dc/hubbub/dcn10/dcn10_hubbub.c
 * (hubbub1_verify_allow_pstate_change_high: debug_index 0xB for DCN 2.0
 * and the comment on the word's bits) and
 * drivers/gpu/drm/amd/include/asic_reg/dcn/dcn_2_0_0_offset.h
 * (mmDCHUBBUB_TEST_DEBUG_INDEX, mmDCHUBBUB_TEST_DEBUG_DATA).
 */
uint32_t
amdrdna_display_memory_handshake(
	struct amdrdna_device *device)
{
	/* The index of the handshake's word, then the word. */
	display_write(device, AMDRDNA_DCN_HUB_DEBUG_INDEX, 0U, DISPLAY_HUB_DEBUG_PSTATE);
	return display_read(device, AMDRDNA_DCN_HUB_DEBUG_DATA, 0U);
}

/* Reads the hub's memory clock change watermark of set A and its test debug index, as they stand. */
void
amdrdna_display_memory_marks(
	struct amdrdna_device *device,
	uint32_t *switch_mark,
	uint32_t *debug_index)
{
	/* The two words. */
	*switch_mark = display_read(device, AMDRDNA_DCN_HUB_SWITCH_MARK, 0U);
	*debug_index = display_read(device, AMDRDNA_DCN_HUB_DEBUG_INDEX, 0U);
}

/*
 * Blanks the plane (the screen shows the blend unit's background, black)
 * or shows it again; the timing generator keeps running.  A blank waits
 * first, for at most 100 ms, until the plane has no request outstanding.
 *
 * Procedure referenced: Linux v6.19
 * drivers/gpu/drm/amd/display/dc/hubp/dcn20/dcn20_hubp.c
 * (hubp2_set_blank_regs).
 *
 * Returns 0.
 */
int
amdrdna_display_blank(
	struct amdrdna_device *device,
	bool blank)
{
	uint32_t word;
	unsigned waited;

	/* A blank waits for the plane's outstanding requests. */
	if (blank) {
		for (waited = 0U; waited < DISPLAY_IDLE_WAIT_US; waited += DISPLAY_IDLE_STEP_US) {
			/* Stops once nothing is outstanding. */
			word = display_read(device, AMDRDNA_DCN_PLANE_CONTROL, device->gop.plane);
			if ((word & DISPLAY_PLANE_IDLE) != 0U)
				break;

			/* Looks again a step later. */
			kern_usleep_range(DISPLAY_IDLE_STEP_US, DISPLAY_IDLE_STEP_US * 2U);
		}
	}

	/* Sets or clears the blank bit only. */
	word = display_read(device, AMDRDNA_DCN_PLANE_CONTROL, device->gop.plane);
	if (blank) {
		word |= DISPLAY_PLANE_BLANK;
	} else {
		word &= ~DISPLAY_PLANE_BLANK;
	}

	/* Writes the word back. */
	display_write(device, AMDRDNA_DCN_PLANE_CONTROL, device->gop.plane, word);

	/* Records the plane's state for the stop. */
	device->display.blanked = blank;

	/* Succeeded: the plane is blanked or shown. */
	return 0;
}

/*
 * Paints the console's surface (the firmware's, kept out of the VRAM pools
 * and outside the CPU's window) opaque black, through a mapping of its
 * range made for the paint alone.  The caller has the plane on another
 * surface and the text console stopped, so nobody sees the paint and
 * nothing draws there meanwhile; the boot logo is gone from it, and a later
 * flip back to it shows black until the console draws again.
 *
 * Returns 0, ENOENT without the console's range, or the mapping's error.
 */
int
amdrdna_display_console_clear(
	struct amdrdna_device *device)
{
	const struct amdrdna_range *range;
	struct drv_pci_mapping mapping;
	volatile uint64_t *words;
	uint64_t started;
	uint64_t black;
	size_t count;
	size_t at;
	unsigned index;
	int error;

	/* The range the firmware's picture keeps in the VRAM. */
	range = NULL;
	for (index = 0U; index < device->layout.reserved_count; index++) {
		if (device->layout.reserved[index].reason == AMDRDNA_RANGE_GOP) {
			range = &device->layout.reserved[index];
			break;
		}
	}
	if (range == NULL || range->end <= range->start)
		return ENOENT;

	/* Maps the range uncached for the paint. */
	started = amdrdna_display_now_us();
	error = drv_pci_device_map_bar_region(device->pci,
	    AMDRDNA_BAR_VRAM,
	    range->start,
	    (size_t)(range->end - range->start),
	    DRV_PCI_MAP_READ | DRV_PCI_MAP_WRITE | DRV_PCI_MAP_NOCACHE,
	    &mapping);
	if (error != 0)
		return error;

	/* Two black pixels a store. */
	black = ((uint64_t)DISPLAY_BLACK << 32) | DISPLAY_BLACK;
	words = (volatile uint64_t *)mapping.address;
	count = (size_t)(range->end - range->start) / sizeof(uint64_t);
	for (at = 0U; at < count; at++)
		words[at] = black;

	/* Pushes the writes past the host path before the plane may read them. */
	amdrdna_host_path_flush(device);
	drv_pci_device_unmap_bar(device->pci, &mapping);

	/* Marks the paint and its time. */
	amdrdna_mark("display: the console's surface painted black (%llu KiB in %llu us)",
	    (unsigned long long)((range->end - range->start) >> 10),
	    (unsigned long long)(amdrdna_display_now_us() - started));

	/* Succeeded: the console's surface is black. */
	return 0;
}

/*
 * Reports the display's topology sequence and whether a monitor is
 * connected.  Any context: takes only the display's IRQ-safe lock.
 */
uint64_t
amdrdna_display_topology(
	struct amdrdna_device *device,
	bool *connected)
{
	struct amdrdna_display *display;
	unsigned long irq;
	uint64_t topology;

	/* Reads both under the lock the interrupt thread moves them under. */
	display = &device->display;
	irq = spin_lock_irqsave(&display->lock);

	topology = display->topology;
	*connected = display->connected;

	spin_unlock_irqrestore(&display->lock, irq);

	/* Succeeded: the sequence. */
	return topology;
}

/*
 * Moves the display's topology sequence on (the mode changed under the
 * pollers' feet, a monitor's plug having set another) and tells whoever
 * polls the GPU device.
 */
void
amdrdna_display_topology_move(
	struct amdrdna_device *device)
{
	struct amdrdna_display *display;
	unsigned long irq;

	/* One more under the interrupt thread's lock. */
	display = &device->display;
	irq = spin_lock_irqsave(&display->lock);

	display->topology++;

	spin_unlock_irqrestore(&display->lock, irq);

	/* Wakes whoever polls the GPU device for the display's events. */
	poll_notify();
}

/* Reads the console's timing generator's frame counter (24 bits). */
uint32_t
amdrdna_display_frame_count(
	struct amdrdna_device *device)
{
	uint32_t frame;

	/* The counter's word. */
	frame = display_read(device, AMDRDNA_DCN_TIMING_FRAME_COUNT, device->gop.timing);

	/* Succeeded: the count. */
	return frame;
}

/*
 * Tells whether a monitor is on the console's hot-plug line now, read
 * from the line's status (connected when the display has no line).
 */
bool
amdrdna_display_sense(
	struct amdrdna_device *device)
{
	struct amdrdna_display *display;
	uint32_t status;
	bool level;

	/* Without a line, the monitor the firmware lit is taken as there. */
	display = &device->display;
	if (!display->has_hotplug)
		return true;

	/* The line's level. */
	status = display_read(device, AMDRDNA_DCN_HOTPLUG_STATUS, display->hotplug);
	level = amdrdna_hotplug_connected(status);

	/* Succeeded: the level. */
	return level;
}

/* Reports the time in microseconds, from the platform's counter (the tick clock without one). */
uint64_t
amdrdna_display_now_us(void)
{
	uint64_t counter;
	uint64_t frequency;
	uint64_t microseconds;
	bool have;

	/* Reads the counter and its rate. */
	have = kern_rtc_read_counter(&counter, &frequency);
	if (!have || frequency == 0U) {
		/* No counter: the tick clock, in whole milliseconds. */
		microseconds = clock_milliseconds(NULL) * 1000U;
		return microseconds;
	}

	/* Scales the counter without overflowing the product. */
	microseconds = (counter / frequency) * 1000000U + ((counter % frequency) * 1000000U) / frequency;

	/* Succeeded: the time in microseconds. */
	return microseconds;
}

/*
 * Fills the display's part of the control device's report: the picture,
 * the addresses, the registers of the plane, the cursor, the timing
 * generator and the hot-plug line as they read now, and the counts.  The
 * caller fills the GPU device's own counts and flags.
 */
void
amdrdna_display_report(
	struct amdrdna_device *device,
	struct amdrdna_display_state *state)
{
	struct amdrdna_display *display;
	unsigned long irq;
	unsigned plane;
	unsigned timing;
	uint32_t high;
	uint32_t low;

	/* The firmware's chain and the picture the takeover keeps. */
	display = &device->display;
	plane = device->gop.plane;
	timing = device->gop.timing;
	state->timing = timing;
	state->plane = plane;
	state->hotplug = display->hotplug;
	state->width = display->width;
	state->height = display->height;
	state->stride = display->stride;
	state->refresh_millihz = display->refresh_millihz;
	state->console_address = display->console;
	state->cursor_address = 0U;
	if (display->cursor_vram != 0U)
		state->cursor_address = amdrdna_display_address(device, display->cursor_vram + (uint64_t)display->cursor_slot * DISPLAY_CURSOR_BYTES);

	/* What the takeover did. */
	state->flags = 0U;
	if (display->taken)
		state->flags |= AMDRDNA_DISPLAY_TAKEN;
	if (display->cursor_ready)
		state->flags |= AMDRDNA_DISPLAY_CURSOR_READY;
	if (display->interrupts_on)
		state->flags |= AMDRDNA_DISPLAY_INTERRUPTS_ON;
	if (display->blanked)
		state->flags |= AMDRDNA_DISPLAY_BLANKED;
	if (display->cursor_shown)
		state->flags |= AMDRDNA_DISPLAY_CURSOR_SHOWN;
	if (display->has_hotplug)
		state->flags |= AMDRDNA_DISPLAY_HOTPLUG;

	/* The plane's addresses: the one it reads and the one its registers hold. */
	state->shown_address = display_shown_address(device);
	high = display_read(device, AMDRDNA_DCN_PLANE_ADDRESS_HIGH, plane) & DISPLAY_ADDRESS_HIGH_FIELD;
	low = display_read(device, AMDRDNA_DCN_PLANE_ADDRESS_LOW, plane);
	state->plane_address = ((uint64_t)high << DISPLAY_ADDRESS_HIGH_SHIFT) | low;

	/* The registers of the timing generator, the plane and the cursor. */
	state->frame_count = display_read(device, AMDRDNA_DCN_TIMING_FRAME_COUNT, timing);
	state->position = display_read(device, AMDRDNA_DCN_TIMING_POSITION, timing);
	state->sync_status = display_read(device, AMDRDNA_DCN_TIMING_SYNC_STATUS, timing);
	state->combiner_input = display_read(device, AMDRDNA_DCN_COMBINER_INPUT, timing);
	state->flip_control = display_read(device, AMDRDNA_DCN_PLANE_FLIP_CONTROL, plane);
	state->flip_interrupt = display_read(device, AMDRDNA_DCN_PLANE_FLIP_INTERRUPT, plane);
	state->plane_control = display_read(device, AMDRDNA_DCN_PLANE_CONTROL, plane);
	state->cursor_control = display_read(device, AMDRDNA_DCN_CURSOR_CONTROL, plane);
	state->cursor_size = display_read(device, AMDRDNA_DCN_CURSOR_SIZE, plane);
	state->cursor_position = display_read(device, AMDRDNA_DCN_CURSOR_POSITION, plane);
	state->cursor_hot_spot = display_read(device, AMDRDNA_DCN_CURSOR_HOT_SPOT, plane);
	state->pixel_cursor = display_read(device, AMDRDNA_DCN_PIXEL_CURSOR_CONTROL, plane);

	/* The hot-plug line, when the encoder names one. */
	state->hotplug_status = 0U;
	state->hotplug_control = 0U;
	if (display->has_hotplug) {
		state->hotplug_status = display_read(device, AMDRDNA_DCN_HOTPLUG_STATUS, display->hotplug);
		state->hotplug_control = display_read(device, AMDRDNA_DCN_HOTPLUG_CONTROL, display->hotplug);
	}

	/* The counts the interrupt thread and the flips move. */
	irq = spin_lock_irqsave(&display->lock);

	state->requested_address = display->requested;
	state->frame_signals = display->frame_signals;
	state->flip_signals = display->flip_signals;
	state->hotplug_signals = display->hotplug_signals;
	state->flips = display->flips;
	state->refresh_count = display->refresh_count;
	state->topology = display->topology;
	if (display->connected)
		state->flags |= AMDRDNA_DISPLAY_CONNECTED;

	spin_unlock_irqrestore(&display->lock, irq);
}

/*
 * Copies the rows of the surface the plane reads now to a caller's buffer
 * (the control device's screen readout): the picture's size, the address
 * read and the bytes copied are reported.  Each row goes through a kernel
 * bounce buffer from the CPU's window, after the host path's read cache is
 * dropped; a surface outside that window (the console's) is read through
 * a mapping of its part of the VRAM BAR made for the copy, or, past the
 * BAR, through the indirect window.
 *
 * Returns 0, ENOENT before the takeover, ERANGE for a surface outside the
 * CPU's window, ENOMEM, or the copy's error.
 */
int
amdrdna_display_screen(
	struct amdrdna_device *device,
	struct amdrdna_screen *screen)
{
	struct amdrdna_display *display;
	struct drv_pci_mapping mapping;
	volatile const uint32_t *source;
	volatile const uint8_t *base;
	uint32_t *row;
	uint64_t address;
	uint64_t vram;
	uint64_t first;
	uint64_t last;
	uint32_t line;
	uint32_t word;
	uint32_t words;
	uint32_t copied;
	bool outside;
	bool by_bar;
	int error;

	/* The display must be the driver's. */
	display = &device->display;
	if (!display->taken)
		return ENOENT;

	/*
	 * The surface the plane reads, inside the VRAM (outside the part of the
	 * window the CPU maps it is read through the indirect window), with
	 * rows the bounce buffer holds.
	 */
	address = display_shown_address(device);
	if (address < device->frame_buffer_base)
		return ERANGE;
	vram = address - device->frame_buffer_base;
	if (vram + (uint64_t)display->stride * display->height > device->vram_bytes)
		return ERANGE;
	outside = vram < device->window_from ||
		  vram + (uint64_t)display->stride * display->height > device->window_from + device->window.size;
	if (display->stride > DISPLAY_SCREEN_ROW_WORDS * 4U)
		return ERANGE;

	/* The picture's description. */
	screen->width = display->width;
	screen->height = display->height;
	screen->stride = display->stride;
	screen->address = address;

	/* The bounce buffer of one row. */
	row = kern_malloc(display->stride);
	if (row == NULL)
		return ENOMEM;

	/* A surface outside the window: its whole pages of the BAR mapped for the copy, when the BAR reaches them. */
	by_bar = false;
	base = NULL;
	if (outside) {
		first = vram & ~((uint64_t)DISPLAY_SCREEN_PAGE_BYTES - 1U);
		last = (vram + (uint64_t)display->stride * display->height + DISPLAY_SCREEN_PAGE_BYTES - 1U) &
		       ~((uint64_t)DISPLAY_SCREEN_PAGE_BYTES - 1U);
		error = drv_pci_device_map_bar_region(device->pci, AMDRDNA_BAR_VRAM, first, (size_t)(last - first),
		    DRV_PCI_MAP_READ | DRV_PCI_MAP_NOCACHE, &mapping);
		if (error == 0) {
			by_bar = true;
			base = (volatile const uint8_t *)mapping.address + (vram - first);
		}
		error = 0;
	}

	/* Copies whole rows from the top while the buffer has room. */
	amdrdna_host_path_drop(device);
	words = display->stride / 4U;
	copied = 0U;
	error = 0;
	for (line = 0U; line < display->height; line++) {
		/* Stops when the next row does not fit. */
		if (screen->length - copied < display->stride)
			break;

		/* The row through the bounce buffer: from a mapping, or a word at a time through the indirect window. */
		if (by_bar) {
			source = (volatile const uint32_t *)(volatile const void *)(base + (uint64_t)line * display->stride);
			for (word = 0U; word < words; word++)
				row[word] = source[word];
		} else if (outside) {
			amdrdna_vram_read(device, vram + (uint64_t)line * display->stride, row, display->stride);
		} else {
			source = (volatile const uint32_t *)(volatile const void *)amdrdna_vram_cpu(device, vram + (uint64_t)line * display->stride);
			for (word = 0U; word < words; word++)
				row[word] = source[word];
		}

		/* The row to the caller. */
		error = copyout(row, (uintptr_t)(screen->buffer + copied), display->stride);
		if (error != 0)
			break;
		copied += display->stride;
	}

	/* The bounce buffer and the copy's mapping go. */
	kern_free(row);
	if (by_bar)
		drv_pci_device_unmap_bar(device->pci, &mapping);

	/* Reports a copy the caller's buffer refused. */
	if (error != 0)
		return error;

	/* Succeeded: the rows are the caller's. */
	screen->copied = copied;
	return 0;
}

/* Finds one instance of a display register; 0 for one the display block lacks. */
static uint32_t
display_index(
	struct amdrdna_device *device,
	enum amdrdna_dcn_register which,
	unsigned instance)
{
	uint32_t index;

	/* The register's index in the display block's windows. */
	index = amdrdna_dcn_index(&device->blocks.blocks[AMDRDNA_BLOCK_DCN], which, instance);

	/* Succeeded: the index, or 0. */
	return index;
}

/* Reads one instance of a display register; a register the display block lacks reads as zero. */
static uint32_t
display_read(
	struct amdrdna_device *device,
	enum amdrdna_dcn_register which,
	unsigned instance)
{
	uint32_t index;
	uint32_t value;

	/* Finds the register. */
	index = display_index(device, which, instance);
	if (index == 0U)
		return 0U;

	/* Reads it. */
	value = amdrdna_read(device, index);

	/* Succeeded: the register's value. */
	return value;
}

/* Writes one instance of a display register; a register the display block lacks is left alone. */
static void
display_write(
	struct amdrdna_device *device,
	enum amdrdna_dcn_register which,
	unsigned instance,
	uint32_t value)
{
	uint32_t index;

	/* Finds the register. */
	index = display_index(device, which, instance);
	if (index == 0U)
		return;

	/* Writes it. */
	amdrdna_write(device, index, value);
}

/*
 * Checks that the firmware's picture is one the takeover keeps: a lit
 * plane fetching the loader's console from this GPU's VRAM, from the
 * surface's origin, in 32-bit colour, linear and uncompressed, with rows
 * on 256 bytes, and a timing generator whose updates are not held.
 */
static int
takeover_check(
	struct amdrdna_device *device)
{
	struct amdrdna_gop *gop;
	uint32_t stride;

	/* A timing generator must be lit, fetching the loader's console from this GPU. */
	gop = &device->gop;
	if (!gop->lit || !gop->surface_in_vram || !gop->loader_on_gpu) {
		amdrdna_mark("A6 fail: no console picture of this GPU to take over (lit=%u vram=%u loader=%u)",
		    gop->lit ? 1U : 0U,
		    gop->surface_in_vram ? 1U : 0U,
		    gop->loader_on_gpu ? 1U : 0U);
		return ENOTSUP;
	}

	/* The plane's surface: 32-bit colour, linear, uncompressed, from its origin. */
	if (gop->format != DISPLAY_FORMAT_32BIT ||
	    gop->tiling != DISPLAY_TILING_LINEAR ||
	    gop->compressed ||
	    gop->viewport_x != 0U ||
	    gop->viewport_y != 0U) {
		amdrdna_mark("A6 fail: hubp %u's surface is not kept (format %u tiling %u compressed %u start %u,%u)",
		    gop->plane,
		    (unsigned)gop->format,
		    (unsigned)gop->tiling,
		    gop->compressed ? 1U : 0U,
		    (unsigned)gop->viewport_x,
		    (unsigned)gop->viewport_y);
		return ENOTSUP;
	}

	/* The rows: at least the viewport's width, on 256 bytes. */
	stride = gop->loader_stride * DISPLAY_PIXEL_BYTES;
	if (gop->loader_stride < gop->viewport_width ||
	    (stride % DISPLAY_PITCH_ALIGN) != 0U ||
	    gop->viewport_height == 0U) {
		amdrdna_mark("A6 fail: the console's rows (%u bytes for %ux%u) are not kept",
		    (unsigned)stride,
		    (unsigned)gop->viewport_width,
		    (unsigned)gop->viewport_height);
		return ENOTSUP;
	}

	/* A held update keeps a flip from ever taking effect. */
	if (gop->locked) {
		amdrdna_mark("A6 fail: otg %u's updates are held", gop->timing);
		return ENOTSUP;
	}

	/* Succeeded: the picture can be taken over. */
	return 0;
}

/*
 * Saves the plane's and the cursor's words the takeover changes, records
 * the console's surface and the picture's shape, and starts the topology.
 */
static void
takeover_save(
	struct amdrdna_device *device)
{
	struct amdrdna_display *display;
	struct amdrdna_gop *gop;
	uint32_t high;
	uint32_t low;

	/* The console's surface: the address the plane reads now. */
	display = &device->display;
	gop = &device->gop;
	high = display_read(device, AMDRDNA_DCN_PLANE_ADDRESS_HIGH, gop->plane) & DISPLAY_ADDRESS_HIGH_FIELD;
	low = display_read(device, AMDRDNA_DCN_PLANE_ADDRESS_LOW, gop->plane);
	display->console = ((uint64_t)high << DISPLAY_ADDRESS_HIGH_SHIFT) | low;
	display->requested = display->console;

	/* The picture every frame must have: the fetched part of the surface and the loader's rows. */
	display->width = gop->viewport_width;
	display->height = gop->viewport_height;
	display->stride = gop->loader_stride * DISPLAY_PIXEL_BYTES;

	/* The words the takeover changes. */
	display->saved_flip_control = display_read(device, AMDRDNA_DCN_PLANE_FLIP_CONTROL, gop->plane);
	display->saved_space_select = display_read(device, AMDRDNA_DCN_PLANE_SPACE_SELECT, gop->plane);
	display->saved_cursor_control = display_read(device, AMDRDNA_DCN_CURSOR_CONTROL, gop->plane);
	display->saved_cursor_timing = display_read(device, AMDRDNA_DCN_PLANE_CURSOR_TIMING, gop->plane);
	display->saved_pixel_cursor = display_read(device, AMDRDNA_DCN_PIXEL_CURSOR_CONTROL, gop->plane);
	display->saved_pixel_cursor_scale = display_read(device, AMDRDNA_DCN_PIXEL_CURSOR_SCALE, gop->plane);

	/* The hot-plug line of the encoder the firmware drives, when it names one. */
	display->has_hotplug = false;
	if (gop->has_output && gop->output_hotplug < AMDRDNA_DISPLAY_PIPES) {
		display->has_hotplug = true;
		display->hotplug = gop->output_hotplug;
	}

	/* The firmware lit a monitor: connected, the first topology. */
	display->connected = true;
	display->topology = DISPLAY_TOPOLOGY_FIRST;
	display->frame_valid = false;

	/* The words are saved; from here on a stop writes them back. */
	display->taken = true;
}

/*
 * Takes the frames' and the cursor's VRAM from the CPU's pool, clears the
 * frames to black and the cursor's slots to transparent.
 *
 * Returns 0 or ENOMEM (nothing is kept then).
 */
static int
takeover_memory(
	struct amdrdna_device *device)
{
	struct amdrdna_display *display;
	volatile uint32_t *words;
	uint64_t bytes;
	uint64_t at;
	unsigned frame;
	int error;

	/* Each frame: the picture's rows, on 64 KiB. */
	display = &device->display;
	bytes = (uint64_t)display->stride * display->height;
	bytes = (bytes + DISPLAY_FRAME_ALIGN - 1U) & ~(DISPLAY_FRAME_ALIGN - 1U);
	display->frame_bytes = 0U;
	display->cursor_vram = 0U;

	/* Takes the frames one at a time; a failure gives back what was taken. */
	for (frame = 0U; frame < AMDRDNA_DISPLAY_FRAMES; frame++) {
		error = amdrdna_vram_take(device, true, bytes, DISPLAY_FRAME_ALIGN, &display->frames[frame]);
		if (error != 0) {
			/* Gives back the frames taken before. */
			while (frame > 0U) {
				frame--;
				amdrdna_vram_give(device, true, display->frames[frame], bytes);
			}

			/* Reports the VRAM missing. */
			return ENOMEM;
		}
	}

	/* The cursor's two slots. */
	error = amdrdna_vram_take(device, true, (uint64_t)DISPLAY_CURSOR_SLOTS * DISPLAY_CURSOR_BYTES, DISPLAY_CURSOR_ALIGN, &display->cursor_vram);
	if (error != 0) {
		for (frame = 0U; frame < AMDRDNA_DISPLAY_FRAMES; frame++)
			amdrdna_vram_give(device, true, display->frames[frame], bytes);
		return ENOMEM;
	}

	/* The frames' size says the display holds VRAM. */
	display->frame_bytes = bytes;

	/* Clears every frame to black. */
	for (frame = 0U; frame < AMDRDNA_DISPLAY_FRAMES; frame++) {
		words = (volatile uint32_t *)(volatile void *)amdrdna_vram_cpu(device, display->frames[frame]);
		for (at = 0U; at < bytes / DISPLAY_PIXEL_BYTES; at++)
			words[at] = DISPLAY_BLACK;
	}

	/* Clears the cursor's slots: a transparent image. */
	words = (volatile uint32_t *)(volatile void *)amdrdna_vram_cpu(device, display->cursor_vram);
	for (at = 0U; at < (uint64_t)DISPLAY_CURSOR_SLOTS * DISPLAY_CURSOR_BYTES / 4U; at++)
		words[at] = 0U;

	/* Pushes the writes past the host path before anything is shown. */
	amdrdna_host_path_flush(device);

	/* Succeeded: the frames and the cursor have VRAM. */
	return 0;
}

/* Gives the frames' and the cursor's VRAM back to the CPU's pool. */
static void
takeover_memory_give(
	struct amdrdna_device *device)
{
	struct amdrdna_display *display;
	unsigned frame;

	/* Nothing was taken when the frames have no size. */
	display = &device->display;
	if (display->frame_bytes == 0U)
		return;

	/* The frames, then the cursor's slots. */
	for (frame = 0U; frame < AMDRDNA_DISPLAY_FRAMES; frame++)
		amdrdna_vram_give(device, true, display->frames[frame], display->frame_bytes);
	amdrdna_vram_give(device, true, display->cursor_vram, (uint64_t)DISPLAY_CURSOR_SLOTS * DISPLAY_CURSOR_BYTES);

	/* The display holds no VRAM any more. */
	display->frame_bytes = 0U;
	display->cursor_vram = 0U;
}

/*
 * Measures the refresh: waits for the frame counter's next step, then
 * times 30 frames.
 *
 * Returns 0, or EIO when the counter does not move.
 */
static int
takeover_measure(
	struct amdrdna_device *device)
{
	struct amdrdna_display *display;
	uint64_t started_us;
	uint64_t ended_us;
	uint64_t waited_us;
	uint32_t first;
	uint32_t start;
	uint32_t now;
	uint32_t frames;

	/* Waits for the counter to step, so the timing starts on a frame. */
	display = &device->display;
	first = display_read(device, AMDRDNA_DCN_TIMING_FRAME_COUNT, device->gop.timing);
	started_us = amdrdna_display_now_us();
	for (;;) {
		/* Stops at the step. */
		start = display_read(device, AMDRDNA_DCN_TIMING_FRAME_COUNT, device->gop.timing);
		frames = amdrdna_frame_advance(first, start);
		if (frames != 0U)
			break;

		/* A counter that does not move in 200 ms is not counting frames. */
		waited_us = amdrdna_display_now_us() - started_us;
		if (waited_us > (uint64_t)DISPLAY_FIRST_FRAME_MS * 1000U) {
			amdrdna_mark("A6 fail: otg %u's frame counter does not move (0x%06x)", device->gop.timing, (unsigned)first);
			return EIO;
		}

		/* Looks again a moment later. */
		kern_usleep_range(DISPLAY_POLL_MIN_US, DISPLAY_POLL_MAX_US);
	}

	/* Times the next frames, at most 1.5 s. */
	started_us = amdrdna_display_now_us();
	frames = 0U;
	for (;;) {
		/* Stops after the frames or the time. */
		now = display_read(device, AMDRDNA_DCN_TIMING_FRAME_COUNT, device->gop.timing);
		frames = amdrdna_frame_advance(start, now);
		ended_us = amdrdna_display_now_us();
		if (frames >= DISPLAY_MEASURED_FRAMES)
			break;
		if (ended_us - started_us > (uint64_t)DISPLAY_MEASURE_MS * 1000U)
			break;

		/* Looks again a moment later. */
		kern_usleep_range(DISPLAY_POLL_MIN_US, DISPLAY_POLL_MAX_US);
	}

	/* The refresh; the time the last frame was seen is within a poll step of its start. */
	display->refresh_millihz = amdrdna_refresh_millihertz(frames, ended_us - started_us);
	if (display->refresh_millihz == 0U) {
		amdrdna_mark("A6 fail: otg %u counted no frame to time", device->gop.timing);
		return EIO;
	}

	/* Succeeded: the refresh is known. */
	return 0;
}

/* Paints stage A6's colour bars, left to right across the picture, into one frame. */
static void
takeover_bars(
	struct amdrdna_device *device,
	unsigned frame)
{
	static const uint32_t colours[DISPLAY_BARS] = {
		DISPLAY_OPAQUE | DISPLAY_RED | DISPLAY_GREEN | DISPLAY_BLUE,
		DISPLAY_OPAQUE | DISPLAY_RED | DISPLAY_GREEN,
		DISPLAY_OPAQUE | DISPLAY_GREEN | DISPLAY_BLUE,
		DISPLAY_OPAQUE | DISPLAY_GREEN,
		DISPLAY_OPAQUE | DISPLAY_RED | DISPLAY_BLUE,
		DISPLAY_OPAQUE | DISPLAY_RED,
		DISPLAY_OPAQUE | DISPLAY_BLUE,
		DISPLAY_OPAQUE
	};
	struct amdrdna_display *display;
	volatile uint8_t *base;
	volatile uint32_t *row;
	uint32_t line;
	uint32_t column;
	uint32_t bar;

	/* The frame's first byte. */
	display = &device->display;
	base = amdrdna_vram_cpu(device, display->frames[frame]);

	/* Every row: each pixel takes the colour of the bar it lies in. */
	for (line = 0U; line < display->height; line++) {
		row = (volatile uint32_t *)(volatile void *)(base + (uint64_t)line * display->stride);
		for (column = 0U; column < display->width; column++) {
			bar = column * DISPLAY_BARS / display->width;
			row[column] = colours[bar];
		}
	}
}

/*
 * Undoes the takeover: hides the cursor, shows the plane, flips back to
 * the console's surface, writes the saved words back, turns the display's
 * interrupts off and gives the VRAM back.  A flip back that does not
 * begin keeps the VRAM (the plane may still read it).
 */
static void
takeover_give_back(
	struct amdrdna_device *device)
{
	struct amdrdna_display *display;
	int error;

	/* The cursor off and the plane shown. */
	display = &device->display;
	amdrdna_display_cursor_hide(device);
	if (display->blanked)
		(void)amdrdna_display_blank(device, false);

	/* The console's surface again. */
	error = 0;
	if (display->requested != display->console)
		error = amdrdna_display_show(device, display->console);

	/* The interrupts off, the plane's and the cursor's words as the takeover found them. */
	display_interrupts(device, false);
	display_write(device, AMDRDNA_DCN_PLANE_FLIP_CONTROL, device->gop.plane, display->saved_flip_control);
	display_write(device, AMDRDNA_DCN_PLANE_SPACE_SELECT, device->gop.plane, display->saved_space_select);
	display_write(device, AMDRDNA_DCN_CURSOR_CONTROL, device->gop.plane, display->saved_cursor_control);
	display_write(device, AMDRDNA_DCN_PLANE_CURSOR_TIMING, device->gop.plane, display->saved_cursor_timing);
	display_write(device, AMDRDNA_DCN_PIXEL_CURSOR_CONTROL, device->gop.plane, display->saved_pixel_cursor);
	display_write(device, AMDRDNA_DCN_PIXEL_CURSOR_SCALE, device->gop.plane, display->saved_pixel_cursor_scale);

	/* The VRAM goes back, unless the plane may still read a frame. */
	if (error != 0) {
		amdrdna_mark("display: the flip back to the console did not begin (%d); the frames stay taken", error);
	} else {
		takeover_memory_give(device);
	}

	/* The display is the firmware's. */
	display->cursor_ready = false;
	display->taken = false;
}

/*
 * Turns the display's interrupts on or off: a frame's start of the
 * console's timing generator, a finished flip of its plane and, when the
 * encoder names one, the monitor's hot-plug line (waiting for the change
 * away from its level).  Anything pending is acknowledged.
 *
 * Procedure referenced: Linux v6.19
 * drivers/gpu/drm/amd/display/dc/irq/irq_service.c (dal_irq_service_set:
 * acknowledged before it is enabled; hpd0_ack: the level that interrupts
 * follows the line's debounced level).
 */
static void
display_interrupts(
	struct amdrdna_device *device,
	bool on)
{
	struct amdrdna_display *display;
	unsigned long irq;
	uint32_t word;
	uint32_t status;
	bool level;

	/*
	 * The thread takes the events from the moment they may come and ignores
	 * them after; an event it reads while they go off is only acknowledged.
	 */
	display = &device->display;
	irq = spin_lock_irqsave(&display->lock);

	display->interrupts_on = on;
	word = display_read(device, AMDRDNA_DCN_TIMING_SYNC_STATUS, device->gop.timing);
	display_write(device, AMDRDNA_DCN_TIMING_SYNC_STATUS, device->gop.timing, amdrdna_sync_status_word(word, on, true));
	word = display_read(device, AMDRDNA_DCN_PLANE_FLIP_INTERRUPT, device->gop.plane);
	display_write(device, AMDRDNA_DCN_PLANE_FLIP_INTERRUPT, device->gop.plane, amdrdna_flip_interrupt_word(word, on, true));
	if (display->has_hotplug) {
		status = display_read(device, AMDRDNA_DCN_HOTPLUG_STATUS, display->hotplug);
		level = amdrdna_hotplug_connected(status);
		word = display_read(device, AMDRDNA_DCN_HOTPLUG_CONTROL, display->hotplug);
		display_write(device, AMDRDNA_DCN_HOTPLUG_CONTROL, display->hotplug, amdrdna_hotplug_control_word(word, on, level, true, true));
	}

	spin_unlock_irqrestore(&display->lock, irq);
}

/*
 * Brings the refresh count up to date from the frame counter: the 24-bit
 * counter's movement since the last reading is added, so the count never
 * goes back; the first reading only sets the base.  The caller holds the
 * display's lock.
 */
static void
display_sample_locked(
	struct amdrdna_device *device)
{
	struct amdrdna_display *display;
	uint32_t frame;
	uint32_t advance;

	/* Reads the counter. */
	display = &device->display;
	frame = display_read(device, AMDRDNA_DCN_TIMING_FRAME_COUNT, device->gop.timing);

	/* Adds what it moved since the last reading, with the time it moved. */
	if (display->frame_valid) {
		advance = amdrdna_frame_advance(display->frame_last, frame);
		if (advance != 0U) {
			display->refresh_count += advance;
			display->refresh_time_ns = kern_ticks_to_ns(sched_ticks());
		}
	}

	/* The next reading counts from this one. */
	display->frame_last = frame;
	display->frame_valid = true;
}

/*
 * Takes a hot-plug event: acknowledges it and, for the console's line,
 * reads the line's level, waits for the change away from it, and moves
 * the topology when the monitor came or went.  A short pulse is only
 * acknowledged.
 */
static void
display_hotplug(
	struct amdrdna_device *device,
	unsigned line)
{
	struct amdrdna_display *display;
	unsigned long irq;
	uint32_t word;
	uint32_t status;
	bool level;
	bool changed;

	/* Only the console's line is the driver's. */
	display = &device->display;
	if (!display->has_hotplug)
		return;

	/* A short pulse of the console's line is acknowledged and nothing more. */
	if (line >= AMDRDNA_HOTPLUG_SHORT_FIRST) {
		if (line - AMDRDNA_HOTPLUG_SHORT_FIRST != display->hotplug)
			return;
		irq = spin_lock_irqsave(&display->lock);

		word = display_read(device, AMDRDNA_DCN_HOTPLUG_CONTROL, display->hotplug);
		display_write(device, AMDRDNA_DCN_HOTPLUG_CONTROL, display->hotplug, amdrdna_hotplug_control_word(word, true, display->connected, false, true));

		spin_unlock_irqrestore(&display->lock, irq);
		return;
	}

	/* A plug or an unplug of another line is not the driver's. */
	if (line != display->hotplug)
		return;

	/* Acknowledges, waits for the next change, and records the monitor. */
	irq = spin_lock_irqsave(&display->lock);

	status = display_read(device, AMDRDNA_DCN_HOTPLUG_STATUS, display->hotplug);
	level = amdrdna_hotplug_connected(status);
	word = display_read(device, AMDRDNA_DCN_HOTPLUG_CONTROL, display->hotplug);
	display_write(device, AMDRDNA_DCN_HOTPLUG_CONTROL, display->hotplug, amdrdna_hotplug_control_word(word, true, level, true, false));
	display->hotplug_signals++;
	changed = false;
	if (level != display->connected) {
		display->connected = level;
		display->topology++;
		changed = true;
	}

	spin_unlock_irqrestore(&display->lock, irq);

	/* Tells the pollers of the display's events, outside the lock. */
	if (changed) {
		if (level) {
			amdrdna_mark("display: a monitor was connected to hot-plug line %u", display->hotplug);
		} else {
			amdrdna_mark("display: the monitor on hot-plug line %u was disconnected", display->hotplug);
		}

		/* Wakes whoever polls the GPU device for the display's events. */
		poll_notify();

		/* The bring-up thread turns the output off, or reads the monitor and lights it again (stage A13). */
		amdrdna_hotplug_request(device);
	}
}

/* Reads the address the plane reads now (the earliest one in use). */
static uint64_t
display_shown_address(
	struct amdrdna_device *device)
{
	uint32_t high;
	uint32_t low;

	/* The two words. */
	high = display_read(device, AMDRDNA_DCN_PLANE_SHOWN_HIGH, device->gop.plane) & DISPLAY_ADDRESS_HIGH_FIELD;
	low = display_read(device, AMDRDNA_DCN_PLANE_SHOWN_LOW, device->gop.plane);

	/* Succeeded: the address. */
	return ((uint64_t)high << DISPLAY_ADDRESS_HIGH_SHIFT) | low;
}

/*
 * Sets the cursor up, hidden: its size, its colour mode and pitch on the
 * plane and on the pixel processor, its fetch timing and a scale of one.
 *
 * Procedure referenced: Linux v6.19
 * drivers/gpu/drm/amd/display/dc/hubp/dcn20/dcn20_hubp.c
 * (hubp2_cursor_set_attributes) and
 * drivers/gpu/drm/amd/display/dc/dpp/dcn20/dcn20_dpp.c
 * (dpp2_set_cursor_attributes): the plane's words before the pixel
 * processor's.
 */
static void
display_cursor_setup(
	struct amdrdna_device *device)
{
	uint32_t word;

	/* The plane's cursor: its size, its control word (hidden) and its fetch timing. */
	display_write(device, AMDRDNA_DCN_CURSOR_SIZE, device->gop.plane, amdrdna_cursor_size_word(DISPLAY_CURSOR_SIDE));
	word = display_read(device, AMDRDNA_DCN_CURSOR_CONTROL, device->gop.plane);
	display_write(device, AMDRDNA_DCN_CURSOR_CONTROL, device->gop.plane, amdrdna_cursor_control_word(word, false));
	display_write(device, AMDRDNA_DCN_PLANE_CURSOR_TIMING, device->gop.plane, amdrdna_cursor_timing_word());

	/* The pixel processor's cursor: its control word (hidden) and a scale of one. */
	word = display_read(device, AMDRDNA_DCN_PIXEL_CURSOR_CONTROL, device->gop.plane);
	display_write(device, AMDRDNA_DCN_PIXEL_CURSOR_CONTROL, device->gop.plane, amdrdna_pixel_cursor_word(word, false));
	display_write(device, AMDRDNA_DCN_PIXEL_CURSOR_SCALE, device->gop.plane, DISPLAY_PIXEL_CURSOR_SCALE_ONE);

	/* The cursor is hidden. */
	device->display.cursor_shown = false;
}

/* Paints stage A7's cursor image, a white square with a black border, into one slot. */
static void
display_cursor_square(
	struct amdrdna_device *device,
	unsigned slot)
{
	volatile uint32_t *pixels;
	uint32_t line;
	uint32_t column;
	bool border;

	/* The slot's pixels, 64 a row. */
	pixels = (volatile uint32_t *)(volatile void *)amdrdna_display_cursor_cpu(device, slot);

	/* Every pixel: black near an edge, white inside. */
	for (line = 0U; line < DISPLAY_CURSOR_SIDE; line++) {
		for (column = 0U; column < DISPLAY_CURSOR_SIDE; column++) {
			/* Within the border of any edge. */
			border = false;
			if (line < DISPLAY_CURSOR_BORDER || column < DISPLAY_CURSOR_BORDER) {
				border = true;
			} else if (line >= DISPLAY_CURSOR_SIDE - DISPLAY_CURSOR_BORDER || column >= DISPLAY_CURSOR_SIDE - DISPLAY_CURSOR_BORDER) {
				border = true;
			}

			/* The pixel's colour. */
			if (border) {
				pixels[line * DISPLAY_CURSOR_SIDE + column] = DISPLAY_CURSOR_BLACK;
			} else {
				pixels[line * DISPLAY_CURSOR_SIDE + column] = DISPLAY_CURSOR_WHITE;
			}
		}
	}
}

/*
 * Waits for some refreshes, at most ten frames' time each (a quarter of a
 * second at 40 Hz and above).
 *
 * Returns 0 or ETIMEDOUT.
 */
static int
display_wait_frames(
	struct amdrdna_device *device,
	unsigned frames)
{
	uint64_t count;
	uint64_t time_ns;
	uint64_t deadline;
	unsigned frame;
	int error;

	/* One refresh after another. */
	for (frame = 0U; frame < frames; frame++) {
		/* The count now. */
		amdrdna_display_refresh_sample(device, &count, &time_ns);

		/* The next refresh after it. */
		deadline = sched_ticks() + kern_ms_to_ticks(250U);
		error = amdrdna_display_refresh_wait(device, count, deadline, &count, &time_ns);
		if (error != 0)
			return error;
	}

	/* Succeeded: the refreshes came. */
	return 0;
}

/* Reports how long the boot asks the display's test pictures to stay (amdrdna.disp.test=<seconds>, 0 by default). */
static unsigned
display_test_seconds(void)
{
	unsigned seconds;

	/* The boot word, bounded. */
	seconds = amdrdna_parameter_number("amdrdna.disp.test", 0U);
	if (seconds > DISPLAY_TEST_MAX_SECONDS)
		seconds = DISPLAY_TEST_MAX_SECONDS;

	/* Succeeded: the seconds. */
	return seconds;
}

/* Reads the plane's underflow bits and whether the timing generator saw an underflow. */
static uint32_t
display_underflow(
	struct amdrdna_device *device,
	uint32_t *timing_underflow)
{
	uint32_t word;
	uint32_t underflow;

	/* The plane's underflow field. */
	word = display_read(device, AMDRDNA_DCN_PLANE_CONTROL, device->gop.plane);
	underflow = (word >> DISPLAY_PLANE_UNDERFLOW_SHIFT) & DISPLAY_PLANE_UNDERFLOW_FIELD;

	/* The timing generator's underflow bit, on the combiner of the same number. */
	word = display_read(device, AMDRDNA_DCN_COMBINER_INPUT, device->gop.timing);
	*timing_underflow = 0U;
	if ((word & DISPLAY_TIMING_UNDERFLOW) != 0U)
		*timing_underflow = 1U;

	/* Succeeded: the plane's underflow bits. */
	return underflow;
}
