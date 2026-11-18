/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The survey of the resident output after its first picture (see survey.h).
 *
 * The two surveys and the CRC between them run on the worker that serves
 * the display window: the first when the window opens, the second at a
 * flip, and the window's close ends a survey left half done.  Only the first
 * window of the first device is surveyed.
 */

#include "internal.h"
#include "run-log.h"
#include "survey.h"
#include "xd-io.h"

#include "../i915.h"
#include "../mmio-record.h"
#include "../perf.h"
#include "../intel/display-regs-xd.h"
#include "../intel/display-regs-xd-south.h"

#include <kern/klog.h>

#include <uapi/errno.h>
#include <stddef.h>

/*
 * The pipe CRC: its control (enable, and the source the CRC is taken from,
 * plane 1 here) and its first result, one block per transcoder.
 *
 * Values referenced: Linux v6.8.12 drivers/gpu/drm/i915/i915_reg.h
 * (_PIPE_CRC_CTL_A, PIPE_CRC_ENABLE, PIPE_CRC_SOURCE_PLANE_1_SKL,
 * _PIPE_CRC_RES_1_A_IVB, PIPE_CRC_CTL and PIPE_CRC_RES_1_IVB through
 * _MMIO_TRANS2).
 */
#define I915_SURVEY_PIPE_CRC_CTL		0x60050U
#define I915_SURVEY_PIPE_CRC_ENABLE		0x80000000U
#define I915_SURVEY_PIPE_CRC_SOURCE_PLANE_1	0x00000000U
#define I915_SURVEY_PIPE_CRC_RES_1		0x60064U

/* How long the second survey's CRC waits for one more frame: 25 waits of 2 ms. */
#define I915_SURVEY_FRAME_WAITS			25U
#define I915_SURVEY_FRAME_WAIT_US		2000U

/* How long after the first survey the second is taken, and the flips that stand in when there is no clock. */
#define I915_SURVEY_SECOND_AFTER_NS		1000000000ULL
#define I915_SURVEY_SECOND_AFTER_FLIPS		60U

/* How many frames the pipe must have shown between the surveys. */
#define I915_SURVEY_MIN_FRAMES			3U

/* The index of each register the judgement and the pipe choice read. */
enum i915_survey_index {
	I915_SURVEY_PWR_WELL_CTL = 0,
	I915_SURVEY_PP_STATUS = 12,
	I915_SURVEY_PWM_CTL = 14,
	I915_SURVEY_PWM_DUTY = 16,
	I915_SURVEY_TRANS_CONF = 17,
	I915_SURVEY_PLANE_SURF = 21,
	I915_SURVEY_PLANE_SURFLIVE = 22,
	I915_SURVEY_PIPE_FRMCNT = 23
};

/* Where the start's surveys stand. */
enum i915_survey_phase {
	I915_SURVEY_IDLE,
	I915_SURVEY_FIRST_TAKEN,
	I915_SURVEY_DONE
};

/*
 * One register the survey reads: its name in the log, its address for pipe
 * A, how far apart the pipes' copies are (0 for a register of the whole
 * display) and whether only display 13 has it.
 */
struct i915_survey_register {
	const char *name;
	uint32_t offset;
	uint32_t pipe_stride;
	int display_13_only;
};

/*
 * The surveys of the start.
 *
 * Only the worker serving the display window of the device that opened the
 * first window reaches it, so it needs no lock.  It lives for the kernel's
 * lifetime: once DONE, nothing changes it again.
 */
struct i915_survey_state {
	struct i915_display *display;
	enum i915_survey_phase phase;
	struct i915_xd_io io;
	struct i915_survey_reading first;
	uint64_t first_ns;
	unsigned flips_since;
	int crc_running;
	unsigned crc_pipe;
};

/*
 * The registers of a survey, in the order of the log.
 *
 * Values referenced: the offsets are the defines of
 * intel/display-regs-xd.h and intel/display-regs-xd-south.h (their own
 * references), and the pipe CRC above.
 */
static const struct i915_survey_register i915_survey_registers[I915_SURVEY_REGISTERS] = {
	{ "PWR_WELL_CTL", I915_XD_PWR_WELL_CTL, 0U, 0 },
	{ "DC_STATE_EN", I915_XD_DC_STATE_EN, 0U, 0 },
	{ "DPLL0_ENABLE", I915_XD_DPLL0_ENABLE, 0U, 0 },
	{ "DPLL0_CFGCR0", 0x164284U, 0U, 0 },
	{ "DPLL0_CFGCR1", 0x164288U, 0U, 0 },
	{ "DPCLKA_CFGCR0", I915_XD_DPCLKA_CFGCR0, 0U, 0 },
	{ "DDI_BUF_CTL_A", 0x64000U, 0U, 0 },
	{ "MBUS_CTL", I915_XD_MBUS_CTL, 0U, 0 },
	{ "DBUF_CTL_S1", I915_XD_DBUF_CTL_S1, 0U, 0 },
	{ "DBUF_CTL_S2", I915_XD_DBUF_CTL_S2, 0U, 0 },
	{ "DBUF_CTL_S3", I915_XD_DBUF_CTL_S3, 0U, 1 },
	{ "DBUF_CTL_S4", I915_XD_DBUF_CTL_S4, 0U, 1 },
	{ "PP_STATUS", 0xc7200U, 0U, 0 },
	{ "PP_CONTROL", 0xc7204U, 0U, 0 },
	{ "SBLC_PWM_CTL", 0xc8250U, 0U, 0 },
	{ "SBLC_PWM_FREQ", 0xc8254U, 0U, 0 },
	{ "SBLC_PWM_DUTY", 0xc8258U, 0U, 0 },
	{ "TRANS_CONF", 0x70008U, I915_XD_PIPE_STRIDE, 0 },
	{ "TRANS_DDI_FUNC_CTL", 0x60400U, I915_XD_TRANS_STRIDE, 0 },
	{ "PIPE_SRCSZ", 0x6001cU, I915_XD_TRANS_STRIDE, 0 },
	{ "PLANE_CTL", 0x70180U, I915_XD_PIPE_STRIDE, 0 },
	{ "PLANE_SURF", 0x7019cU, I915_XD_PIPE_STRIDE, 0 },
	{ "PLANE_SURFLIVE", 0x701acU, I915_XD_PIPE_STRIDE, 0 },
	{ "PIPE_FRMCNT", 0x70040U, I915_XD_PIPE_STRIDE, 0 },
	{ "PIPE_CRC_CTL", I915_SURVEY_PIPE_CRC_CTL, I915_XD_TRANS_STRIDE, 0 },
	{ "PIPE_CRC_RES_1", I915_SURVEY_PIPE_CRC_RES_1, I915_XD_TRANS_STRIDE, 0 },
	{ "PLANE_WM_0", 0x70240U, I915_XD_PIPE_STRIDE, 0 },
	{ "PLANE_BUF_CFG", 0x7027cU, I915_XD_PIPE_STRIDE, 0 },
	{ "WM_LINETIME", 0x45270U, 4U, 0 },
	{ "PIPE_MBUS_DBOX_CTL", 0x7003cU, I915_XD_PIPE_STRIDE, 0 },
	{ "PIPE_SCANLINE", 0x70000U, I915_XD_PIPE_STRIDE, 0 }
};

/*
 * The main power well that powers each pipe: power wells 1, 3, 4 and 5 on
 * display 12, the pipe wells A to D on display 13.
 *
 * Values referenced: Linux v6.8.12
 * drivers/gpu/drm/i915/display/intel_display_power_map.c (tgl_power_wells,
 * xelpd_power_wells: the pipe domains of each main well).
 */
static const unsigned i915_survey_pipe_well_12[I915_SURVEY_PIPES] = {
	I915_XD_WELL_PW_1, I915_XD_WELL_PW_3, I915_XD_WELL_PW_4, I915_XD_WELL_PW_5
};
static const unsigned i915_survey_pipe_well_13[I915_SURVEY_PIPES] = {
	I915_XD_WELL_PW_A, I915_XD_WELL_PW_B, I915_XD_WELL_PW_C, I915_XD_WELL_PW_D
};

/*
 * The test scenario's panel start, linked only into a kernel built with
 * the display scenarios.  Those scenarios take the pipe CRC themselves, so
 * the survey leaves the CRC alone when this symbol is there; in every
 * other kernel it has no address.
 */
struct i915_lcd_run_params;
extern struct i915_lcd_kernel *drv_i915_test_lcd_start(struct i915_display *display, const struct i915_lcd_run_params *params) __attribute__((weak));

/* The surveys of the start; see struct i915_survey_state. */
static struct i915_survey_state i915_survey;

static int i915_survey_pipe_powered(int display_ver, uint32_t well_ctl, unsigned pipe);
static unsigned i915_survey_buffer_of(const struct i915_survey_scene *scene, uint32_t surface);
static void i915_survey_scene_of(struct i915_display *display, struct i915_survey_scene *scene);
static int i915_survey_display_ver(struct i915_display *display);
static const char *i915_survey_buffer_name(unsigned buffer);

/*
 * Names a register of the survey, by its index in the log.
 */
const char *
drv_i915_survey_register_name(
	unsigned index)
{
	/* An index past the table names nothing. */
	if (index >= I915_SURVEY_REGISTERS)
		return NULL;

	/* Succeeded: the register's name. */
	return i915_survey_registers[index].name;
}

/*
 * Reports a register's address for a pipe (ignored for a register of the
 * whole display).
 */
uint32_t
drv_i915_survey_register_offset(
	unsigned index,
	unsigned pipe)
{
	const struct i915_survey_register *reg;

	/* An index past the table has no address. */
	if (index >= I915_SURVEY_REGISTERS)
		return 0U;

	/* Succeeded: the register's address for the pipe. */
	reg = &i915_survey_registers[index];
	return reg->offset + reg->pipe_stride * pipe;
}

/*
 * Reads every register of a survey.
 *
 * The pipe surveyed is the one whose plane scans out a resident buffer;
 * pipe registers are read only while that pipe's well is on.
 */
void
drv_i915_survey_read(
	struct i915_xd_io *io,
	const struct i915_survey_scene *scene,
	struct i915_survey_reading *reading)
{
	const struct i915_survey_register *reg;
	uint32_t well_ctl;
	uint32_t offset;
	uint32_t surface;
	unsigned buffer;
	unsigned pipe;
	unsigned index;
	int powered;

	/* Starts with pipe A and nothing read. */
	reading->pipe = 0U;
	reading->pipe_found = 0;
	reading->pipe_powered = 0;
	reading->live_buffer = I915_SURVEY_BUFFER_NONE;
	reading->underruns = scene->underruns;
	for (index = 0U; index < I915_SURVEY_REGISTERS; index++) {
		reading->value[index] = 0U;
		reading->read[index] = 0;
	}

	/* The wells tell which pipes can be read. */
	well_ctl = drv_i915_xd_io_read(io, I915_XD_PWR_WELL_CTL);

	/* Finds the powered pipe whose plane shows a resident buffer. */
	for (pipe = 0U; pipe < I915_SURVEY_PIPES; pipe++) {
		powered = i915_survey_pipe_powered(scene->display_ver, well_ctl, pipe);
		if (!powered)
			continue;

		/* Reads which buffer the pipe's plane shows. */
		offset = drv_i915_survey_register_offset(I915_SURVEY_PLANE_SURF, pipe);
		surface = drv_i915_xd_io_read(io, offset);
		buffer = i915_survey_buffer_of(scene, surface);
		if (buffer != I915_SURVEY_BUFFER_NONE) {
			reading->pipe = pipe;
			reading->pipe_found = 1;
			break;
		}
	}

	/* Whether the pipe surveyed can be read. */
	reading->pipe_powered = i915_survey_pipe_powered(scene->display_ver, well_ctl, reading->pipe);

	/* Reads each register this display has, a pipe's only while the pipe is powered. */
	for (index = 0U; index < I915_SURVEY_REGISTERS; index++) {
		reg = &i915_survey_registers[index];
		if (reg->display_13_only && scene->display_ver != 13)
			continue;
		if (reg->pipe_stride != 0U && !reading->pipe_powered)
			continue;

		/* Reads the register. */
		offset = drv_i915_survey_register_offset(index, reading->pipe);
		reading->value[index] = drv_i915_xd_io_read(io, offset);
		reading->read[index] = 1;
	}

	/* The buffer the plane scans out now. */
	if (reading->read[I915_SURVEY_PLANE_SURFLIVE])
		reading->live_buffer = i915_survey_buffer_of(scene, reading->value[I915_SURVEY_PLANE_SURFLIVE]);
}

/*
 * Starts the pipe CRC of a pipe on what plane 1 reads.
 */
void
drv_i915_survey_crc_start(
	struct i915_xd_io *io,
	unsigned pipe)
{
	uint32_t control;

	/* Enables the CRC with plane 1 as its source. */
	control = I915_SURVEY_PIPE_CRC_ENABLE | I915_SURVEY_PIPE_CRC_SOURCE_PLANE_1;
	drv_i915_xd_io_write(io, I915_SURVEY_PIPE_CRC_CTL + I915_XD_TRANS_STRIDE * pipe, control);
}

/*
 * Reads the pipe CRC twice, one frame or more apart.
 *
 * Returns 0, or EAGAIN when the pipe's frame counter did not move within
 * 50 ms (the second value is then 0).
 */
int
drv_i915_survey_crc_pair(
	struct i915_xd_io *io,
	unsigned pipe,
	uint32_t crc[2])
{
	uint32_t frame_offset;
	uint32_t crc_offset;
	uint32_t first_frame;
	uint32_t frame;
	unsigned waits;

	/* The pipe's frame counter and CRC result. */
	frame_offset = drv_i915_survey_register_offset(I915_SURVEY_PIPE_FRMCNT, pipe);
	crc_offset = I915_SURVEY_PIPE_CRC_RES_1 + I915_XD_TRANS_STRIDE * pipe;

	/* Reads the first CRC and the frame it belongs to. */
	first_frame = drv_i915_xd_io_read(io, frame_offset);
	crc[0] = drv_i915_xd_io_read(io, crc_offset);
	crc[1] = 0U;

	/* Waits for the next frame. */
	frame = first_frame;
	for (waits = 0U; waits < I915_SURVEY_FRAME_WAITS; waits++) {
		(void)drv_i915_xd_io_delay(io, I915_SURVEY_FRAME_WAIT_US);
		frame = drv_i915_xd_io_read(io, frame_offset);
		if (frame != first_frame)
			break;
	}

	/* A pipe whose frames do not move has no second CRC. */
	if (frame == first_frame)
		return EAGAIN;

	/* Reads the CRC of the later frame. */
	crc[1] = drv_i915_xd_io_read(io, crc_offset);

	/* Succeeded: two CRCs, a frame or more apart. */
	return 0;
}

/*
 * Stops the pipe CRC of a pipe.
 */
void
drv_i915_survey_crc_stop(
	struct i915_xd_io *io,
	unsigned pipe)
{
	/* Disables the CRC, which also stops its per-frame interrupt. */
	drv_i915_xd_io_write(io, I915_SURVEY_PIPE_CRC_CTL + I915_XD_TRANS_STRIDE * pipe, 0U);
}

/*
 * Judges whether a pair of surveys shows a lit panel scanning out a
 * resident buffer.
 */
void
drv_i915_survey_judge(
	const struct i915_survey_reading *first,
	const struct i915_survey_reading *second,
	const uint32_t crc[2],
	int crc_read,
	struct i915_survey_verdict *verdict)
{
	uint32_t duty;

	/* Starts with no evidence. */
	verdict->panel_on = 0;
	verdict->backlight_on = 0;
	verdict->transcoder_on = 0;
	verdict->frames_advanced = 0;
	verdict->live_is_resident = 0;
	verdict->crc_nonzero = 0;
	verdict->no_underruns = 0;
	verdict->frames = 0U;
	verdict->crc[0] = crc[0];
	verdict->crc[1] = crc[1];
	verdict->crc_read = crc_read;

	/* The panel power sequencer reports the panel on. */
	if ((second->value[I915_SURVEY_PP_STATUS] & I915_XD_PP_STATUS_ON) != 0U)
		verdict->panel_on = 1;

	/* The backlight PWM runs with a duty cycle above zero. */
	duty = second->value[I915_SURVEY_PWM_DUTY];
	if ((second->value[I915_SURVEY_PWM_CTL] & I915_XD_SBLC_PWM_ENABLE) != 0U && duty != 0U)
		verdict->backlight_on = 1;

	/* The pipe's transcoder is enabled. */
	if ((second->value[I915_SURVEY_TRANS_CONF] & I915_XD_TRANS_CONF_ENABLE) != 0U)
		verdict->transcoder_on = 1;

	/* The same pipe counted frames between the surveys. */
	if (first->pipe == second->pipe &&
	    first->read[I915_SURVEY_PIPE_FRMCNT] &&
	    second->read[I915_SURVEY_PIPE_FRMCNT]) {
		verdict->frames = second->value[I915_SURVEY_PIPE_FRMCNT] - first->value[I915_SURVEY_PIPE_FRMCNT];
		if (verdict->frames >= I915_SURVEY_MIN_FRAMES)
			verdict->frames_advanced = 1;
	}

	/* The plane scans out one of the resident buffers. */
	if (second->live_buffer != I915_SURVEY_BUFFER_NONE)
		verdict->live_is_resident = 1;

	/* Both CRCs were read and neither is zero. */
	if (crc_read &&
	    crc[0] != 0U &&
	    crc[1] != 0U)
		verdict->crc_nonzero = 1;

	/* The interrupt handler counted no underrun. */
	if (second->underruns == 0U)
		verdict->no_underruns = 1;
}

/*
 * Writes one survey to the kernel log.
 */
void
drv_i915_survey_log(
	unsigned round,
	const struct i915_survey_reading *reading)
{
	unsigned index;

	/* Says which pipe was surveyed and what it shows. */
	kern_logf("i915: survey %u pipe %c (%s, %s), live surface %s, underruns %u\n",
	    round,
	    (int)('A' + reading->pipe),
	    reading->pipe_found ? "shows a resident buffer" : "no pipe shows a resident buffer",
	    reading->pipe_powered ? "powered" : "not powered",
	    i915_survey_buffer_name(reading->live_buffer),
	    reading->underruns);

	/* Writes every register, one to a line. */
	for (index = 0U; index < I915_SURVEY_REGISTERS; index++) {
		if (reading->read[index]) {
			kern_logf("i915: survey %u %s=0x%08x\n", round, i915_survey_registers[index].name, reading->value[index]);
		} else {
			kern_logf("i915: survey %u %s=unread\n", round, i915_survey_registers[index].name);
		}
	}
}

/*
 * Writes the judgement of a pair of surveys to the kernel log.
 */
void
drv_i915_survey_log_verdict(
	const struct i915_survey_verdict *verdict)
{
	int lit;

	/* The panel is taken as lit only when every piece of evidence agrees. */
	lit = 1;
	if (!verdict->panel_on)
		lit = 0;
	if (!verdict->backlight_on)
		lit = 0;
	if (!verdict->transcoder_on)
		lit = 0;
	if (!verdict->frames_advanced)
		lit = 0;
	if (!verdict->live_is_resident)
		lit = 0;
	if (!verdict->crc_nonzero)
		lit = 0;
	if (!verdict->no_underruns)
		lit = 0;

	/* Writes the judgement and each piece of evidence. */
	kern_logf("i915: survey: %s: panel %s, backlight %s, transcoder %s, frames +%u, live surface %s, crc %s 0x%08x 0x%08x, underruns %s\n",
	    lit ? "lit" : "NOT lit",
	    verdict->panel_on ? "on" : "off",
	    verdict->backlight_on ? "on" : "off",
	    verdict->transcoder_on ? "on" : "off",
	    verdict->frames,
	    verdict->live_is_resident ? "resident" : "other",
	    verdict->crc_read ? "read" : "unread",
	    verdict->crc[0],
	    verdict->crc[1],
	    verdict->no_underruns ? "none" : "some");
}

/*
 * Takes the first survey when the display window opens.
 *
 * Runs on the worker that serves the window, after the first picture; it
 * reports the start's record of the display writes first, so the survey's
 * own writes are not in it.
 */
void
drv_i915_survey_window_open(
	struct i915_display *display)
{
	struct i915_survey_scene scene;
	int display_ver;
	int whole;

	/* Only the first window of the first device is surveyed. */
	if (i915_survey.phase != I915_SURVEY_IDLE)
		return;

	/* Records the display; a later window is not surveyed. */
	i915_survey.display = display;
	i915_survey.phase = I915_SURVEY_DONE;

	/* Only display 12 and 13 have the registers the survey reads. */
	display_ver = i915_survey_display_ver(display);
	if (display_ver != 12 && display_ver != 13)
		return;

	/* Reports the record of the writes: the digest, and every write when the display log is detailed. */
	whole = drv_i915_lcd_debug_enabled();
	drv_i915_mmio_record_log(whole);

	/* Reads the registers through a window of their own. */
	drv_i915_xd_io_init(&i915_survey.io, &display->device->gt.mmio, NULL);
	i915_survey_scene_of(display, &scene);
	drv_i915_survey_read(&i915_survey.io, &scene, &i915_survey.first);
	drv_i915_survey_log(1U, &i915_survey.first);

	/* Starts the CRC on the surveyed pipe, unless a display test scenario uses it. */
	i915_survey.crc_running = 0;
	if (i915_survey.first.pipe_powered && drv_i915_test_lcd_start == NULL) {
		drv_i915_survey_crc_start(&i915_survey.io, i915_survey.first.pipe);
		i915_survey.crc_running = 1;
		i915_survey.crc_pipe = i915_survey.first.pipe;
	}

	/* The second survey waits for a flip a second from now. */
	i915_survey.first_ns = drv_i915_perf_now();
	i915_survey.flips_since = 0U;
	i915_survey.phase = I915_SURVEY_FIRST_TAKEN;
}

/*
 * Takes the second survey at the first flip a second or more after the first.
 *
 * Runs on the worker before the flip is armed.  The CRC pair may wait up to
 * 50 ms for a frame; that happens once in a start.
 */
void
drv_i915_survey_flip(
	struct i915_display *display)
{
	struct i915_survey_scene scene;
	struct i915_survey_reading second;
	struct i915_survey_verdict verdict;
	uint32_t crc[2];
	uint64_t now;
	int crc_read;
	int error;

	/* Only a first survey of this display waits for its second. */
	if (i915_survey.phase != I915_SURVEY_FIRST_TAKEN)
		return;
	if (i915_survey.display != display)
		return;

	/* Waits for a second to pass, or for enough flips when there is no clock. */
	i915_survey.flips_since++;
	now = drv_i915_perf_now();
	if (now != 0U && now - i915_survey.first_ns < I915_SURVEY_SECOND_AFTER_NS)
		return;
	if (now == 0U && i915_survey.flips_since < I915_SURVEY_SECOND_AFTER_FLIPS)
		return;

	/* Reads the registers again. */
	i915_survey_scene_of(display, &scene);
	drv_i915_survey_read(&i915_survey.io, &scene, &second);

	/* Reads the CRC twice and stops it. */
	crc[0] = 0U;
	crc[1] = 0U;
	crc_read = 0;
	if (i915_survey.crc_running) {
		error = drv_i915_survey_crc_pair(&i915_survey.io, i915_survey.crc_pipe, crc);
		if (error == 0)
			crc_read = 1;

		/* Stops the CRC. */
		drv_i915_survey_crc_stop(&i915_survey.io, i915_survey.crc_pipe);
		i915_survey.crc_running = 0;
	}

	/* Writes the second survey and the judgement of the pair. */
	drv_i915_survey_log(2U, &second);
	drv_i915_survey_judge(&i915_survey.first, &second, crc, crc_read, &verdict);
	drv_i915_survey_log_verdict(&verdict);

	/* The start's surveys are done. */
	i915_survey.phase = I915_SURVEY_DONE;
}

/*
 * Ends a survey left half done when the display window closes.
 *
 * Runs on the worker while the pipe still runs: the CRC is stopped and the
 * second survey is reported as skipped.
 */
void
drv_i915_survey_window_close(
	struct i915_display *display)
{
	/* Only a first survey of this display is left half done. */
	if (i915_survey.phase != I915_SURVEY_FIRST_TAKEN)
		return;
	if (i915_survey.display != display)
		return;

	/* Stops the CRC the first survey started. */
	if (i915_survey.crc_running) {
		drv_i915_survey_crc_stop(&i915_survey.io, i915_survey.crc_pipe);
		i915_survey.crc_running = 0;
	}

	/* Says why there is no second survey. */
	kern_logf("i915: survey 2 skipped: the display window closed before a flip a second after the first picture\n");
	i915_survey.phase = I915_SURVEY_DONE;
}

/* Reports whether a pipe's main power well is on, by the display version's well of the pipe. */
static int
i915_survey_pipe_powered(
	int display_ver,
	uint32_t well_ctl,
	unsigned pipe)
{
	unsigned well;

	/* A pipe past the table has no well. */
	if (pipe >= I915_SURVEY_PIPES)
		return 0;

	/* The well that powers the pipe on this display. */
	if (display_ver == 13) {
		well = i915_survey_pipe_well_13[pipe];
	} else {
		well = i915_survey_pipe_well_12[pipe];
	}

	/* The well's state bit says whether it is on. */
	if ((well_ctl & I915_XD_PWR_WELL_STATE(well)) == 0U)
		return 0;

	/* Succeeded: the pipe is powered. */
	return 1;
}

/* Tells which resident buffer a surface address is, or that it is neither. */
static unsigned
i915_survey_buffer_of(
	const struct i915_survey_scene *scene,
	uint32_t surface)
{
	uint32_t address;

	/* Compares the 4 KiB-aligned addresses, the low bits being control bits. */
	address = surface & I915_XD_PLANE_SURF_ADDRESS;
	if (address == 0U)
		return I915_SURVEY_BUFFER_NONE;
	if (address == (scene->buffer_surf[0] & I915_XD_PLANE_SURF_ADDRESS))
		return 0U;
	if (address == (scene->buffer_surf[1] & I915_XD_PLANE_SURF_ADDRESS))
		return 1U;

	/* The surface is not a resident buffer. */
	return I915_SURVEY_BUFFER_NONE;
}

/* Fills what the survey is told about a display: its version, the resident buffers and the underruns. */
static void
i915_survey_scene_of(
	struct i915_display *display,
	struct i915_survey_scene *scene)
{
	/* The display version and the two resident buffers' surfaces. */
	scene->display_ver = i915_survey_display_ver(display);
	scene->buffer_surf[0] = (uint32_t)display->resident_buf[0].surf;
	scene->buffer_surf[1] = (uint32_t)display->resident_buf[1].surf;

	/* The underruns the interrupt handler counted. */
	scene->underruns = display->irq.de_underrun_count;
}

/* Reports the display version of a Gen12 device, or 0 for any other device. */
static int
i915_survey_display_ver(
	struct i915_display *display)
{
	/* Only a Gen12 device's display has the surveyed layout. */
	if (display->device->gt.graphics_ver != 12U)
		return 0;

	/* Succeeded: the display version (12 or 13). */
	return (int)display->device->gt.display_ver;
}

/* Names a resident buffer for the log. */
static const char *
i915_survey_buffer_name(
	unsigned buffer)
{
	/* The first and the second resident buffer. */
	if (buffer == 0U)
		return "buffer A";
	if (buffer == 1U)
		return "buffer B";

	/* Any other surface. */
	return "not a resident buffer";
}
