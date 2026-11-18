/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The run log of the display's outputs (run-log.c).
 *
 * The recorder stacked on an output run's hooks, the observer of the
 * pipe's underrun status and frame counter, the run's step, error and
 * debug hooks, the switch of i915.debug=display, and the log of the run
 * and of the observer.
 *
 * Errors: the functions that report an error return a positive errno.
 */

#ifndef DRIVERS_GPU_I915_DISPLAY_RUN_LOG_H
#define DRIVERS_GPU_I915_DISPLAY_RUN_LOG_H

#include "internal.h"

/*
 * ==== The run log ====
 */

/* Stacks the recorder on a backend; hand trace->ops to the run. */
void drv_i915_lcd_trace_init(struct i915_lcd_trace *trace, struct i915_lcd_emit *backend);

/* Records a phase marker written by the caller. */
void drv_i915_lcd_trace_phase(struct i915_lcd_trace *trace, const char *name);

/*
 * ==== The commit observer ====
 */

/* Prepares the observer of one pipe over the backend it reads. */
void drv_i915_lcd_observer_init(struct i915_lcd_observer *observer, struct i915_lcd_emit *hw, int pipe);

/* The observe hook: samples at a named point of the commit (ctx is the observer). */
void drv_i915_lcd_observer_point(void *ctx, int point);

/* Samples once more as start-up, then starts the steady period. */
void drv_i915_lcd_observer_steady_begin(struct i915_lcd_observer *observer);

/* Samples during the steady period. */
void drv_i915_lcd_observer_steady_sample(struct i915_lcd_observer *observer);

/* Ends the steady period with a last sample (nothing when not steady). */
void drv_i915_lcd_observer_steady_end(struct i915_lcd_observer *observer);

/* Waits up to window_ms for min_frames frames; 0, or ETIMEDOUT when the counter did not advance enough. */
int drv_i915_lcd_observer_frames(struct i915_lcd_observer *observer, unsigned window_ms, unsigned min_frames, uint32_t *first, uint32_t *last);

/* Reports the stop evidence; 0, EINVAL when none was taken, or EBUSY when the pipe was not stopped. */
int drv_i915_lcd_observer_stopped(struct i915_lcd_observer *observer, unsigned window_ms, uint32_t *first, uint32_t *last);

/*
 * ==== The output run's hooks and log ====
 */

/* The step hook: counts and prints a decided or an unresolved step (ctx is the struct i915_lcd_kernel). */
void drv_i915_lcd_kernel_step(void *ctx, const char *name);

/* The error hook: counts and prints an error of the run (ctx is the struct i915_lcd_kernel). */
void drv_i915_lcd_kernel_error(void *ctx, const char *what);

/* The debug hook: prints a debug message under i915.debug=display, ignores it otherwise (ctx is the struct i915_lcd_kernel). */
void drv_i915_lcd_kernel_debug(void *ctx, const char *what);

/* Turns the detailed display log of i915.debug=display on (nonzero) or off. */
void drv_i915_lcd_debug_set(int enabled);

/* Reports whether i915.debug=display asked for the detailed display log. */
int drv_i915_lcd_debug_enabled(void);

/* Logs the run log. */
void drv_i915_lcd_log_trace(const struct i915_lcd_trace *trace);

/* Logs the observer's findings and samples. */
void drv_i915_lcd_log_observer(const struct i915_lcd_observer *observer);

#endif /* DRIVERS_GPU_I915_DISPLAY_RUN_LOG_H */
