/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The survey of the resident output after its first picture.
 *
 * Twice in a start the survey reads the display registers that tell whether
 * the panel shows a picture -- the panel power, the backlight PWM, the
 * pipe's frame counter and transcoder, the surface the plane scans out, a
 * CRC of what the plane reads, the watermarks and buffers, the clocks and
 * wells -- and writes them to the kernel log, so two kernels can be compared
 * on a machine nobody looks at.  The first survey is taken when the display
 * window opens (right after the first picture), the second at the first
 * flip a second or more later; between the two the pipe CRC runs.  The
 * first survey also reports the start's record of the display writes
 * (mmio-record.h).
 *
 * The registers are reached through an xd register window, so the host
 * tests run the survey on the register model.
 */

#ifndef DRIVERS_GPU_I915_DISPLAY_SURVEY_H
#define DRIVERS_GPU_I915_DISPLAY_SURVEY_H

#include <stdint.h>

struct i915_display;
struct i915_xd_io;

/* How many registers one survey reads. */
#define I915_SURVEY_REGISTERS		31U

/* How many pipes the survey looks at for the resident buffers. */
#define I915_SURVEY_PIPES		4U

/* The buffer a surface address is: the first or second resident buffer, or neither. */
#define I915_SURVEY_BUFFER_NONE		2U

/*
 * What the survey is told besides the registers: the display version (12
 * or 13), the surface addresses of the two resident buffers and the
 * display interrupt handler's underrun count.
 */
struct i915_survey_scene {
	int display_ver;
	uint32_t buffer_surf[2];
	unsigned underruns;
};

/*
 * One survey: the pipe that scans out a resident buffer (pipe A when none
 * does), each register's value and whether it was read (a pipe register is
 * not read while the pipe's well is off), and the buffer the plane scans
 * out live.
 */
struct i915_survey_reading {
	unsigned pipe;
	int pipe_found;
	int pipe_powered;
	unsigned live_buffer;
	unsigned underruns;
	uint32_t value[I915_SURVEY_REGISTERS];
	int read[I915_SURVEY_REGISTERS];
};

/*
 * The judgement of a pair of surveys: each piece of evidence that the panel
 * shows a picture, and the two CRCs read in the second survey.
 */
struct i915_survey_verdict {
	int panel_on;
	int backlight_on;
	int transcoder_on;
	int frames_advanced;
	int live_is_resident;
	int crc_nonzero;
	int no_underruns;
	uint32_t frames;
	uint32_t crc[2];
	int crc_read;
};

const char *drv_i915_survey_register_name(unsigned index);
uint32_t drv_i915_survey_register_offset(unsigned index, unsigned pipe);
void drv_i915_survey_read(struct i915_xd_io *io, const struct i915_survey_scene *scene, struct i915_survey_reading *reading);
void drv_i915_survey_crc_start(struct i915_xd_io *io, unsigned pipe);
int drv_i915_survey_crc_pair(struct i915_xd_io *io, unsigned pipe, uint32_t crc[2]);
void drv_i915_survey_crc_stop(struct i915_xd_io *io, unsigned pipe);
void drv_i915_survey_judge(const struct i915_survey_reading *first, const struct i915_survey_reading *second, const uint32_t crc[2], int crc_read, struct i915_survey_verdict *verdict);
void drv_i915_survey_log(unsigned round, const struct i915_survey_reading *reading);
void drv_i915_survey_log_verdict(const struct i915_survey_verdict *verdict);

void drv_i915_survey_window_open(struct i915_display *display);
void drv_i915_survey_flip(struct i915_display *display);
void drv_i915_survey_window_close(struct i915_display *display);

#endif
