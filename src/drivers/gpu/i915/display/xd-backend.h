/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The hooks a display screen reaches the device through (xd-picture.c).
 *
 * A screen's light, dark and flips write registers, wait, talk to the
 * sink over AUX, run the panel's power sequencer, take display power,
 * ask for display buffer slices, wait for vertical blanks and take the
 * Type-C ports' links through one table of hooks.  The kernel fills it
 * with the hardware; a host test fills it with a model, so the same
 * screen code runs against both.
 */

#ifndef DRIVERS_GPU_I915_DISPLAY_XD_BACKEND_H
#define DRIVERS_GPU_I915_DISPLAY_XD_BACKEND_H

#include <stddef.h>
#include <stdint.h>

/*
 * The named points of a screen's commit that a backend may sample at:
 * the start (the display kept out of DC states, nothing written yet),
 * the arming of the pipe's underrun report, the pipe enabled, the plane
 * armed, the plane's disable armed, the underrun report disarmed, the
 * pipe disabled, and the end (before the DC states are let back).
 */
#define I915_XD_OBSERVE_COMMIT_BEGIN		0
#define I915_XD_OBSERVE_UNDERRUN_ARM		1
#define I915_XD_OBSERVE_PIPE_ENABLED		2
#define I915_XD_OBSERVE_PLANE_ARMED		3
#define I915_XD_OBSERVE_PLANE_DISABLED		4
#define I915_XD_OBSERVE_UNDERRUN_DISARM		5
#define I915_XD_OBSERVE_PIPE_DISABLED		6
#define I915_XD_OBSERVE_COMMIT_END		7
#define I915_XD_OBSERVE_POINTS			8

/* The negative error a sink hook answers when there is no sink to ask. */
#define I915_XD_BACKEND_EIO			(-5)

/*
 * The modes the tc_mode hook answers a Type-C port is held in: none,
 * Thunderbolt, DisplayPort alternate mode, or a legacy (fixed) port.
 */
#define I915_XD_BACKEND_TC_NONE			0
#define I915_XD_BACKEND_TC_TBT			1
#define I915_XD_BACKEND_TC_DP_ALT		2
#define I915_XD_BACKEND_TC_LEGACY		3

/*
 * The DPCD access of a sink reached over its own AUX channel (an
 * external DisplayPort sink behind a Type-C port), instead of the panel's
 * channel the backend reaches.  The transfers answer the bytes moved or
 * a negative error; the capability read answers 0 or a negative error;
 * the converter hook sets a branch's protocol converter up for an RGB
 * stream and may be NULL.  The owner of the channel keeps the hooks valid
 * for as long as a screen may use them.
 */
struct i915_xd_sink_hooks {
	void *ctx;
	long (*dpcd_read)(void *ctx, unsigned offset, uint8_t *buf, size_t size);
	long (*dpcd_write)(void *ctx, unsigned offset, const uint8_t *buf, size_t size);
	int (*read_dpcd_caps)(void *ctx, uint8_t dpcd[15]);
	int (*configure_converter)(void *ctx);
};

/*
 * The device as a screen sees it.
 *
 * model is 1 for a register model, whose retained state may be thrown
 * away with the model, and 0 for the hardware.  The register hooks write,
 * read-modify-write (answering the old value), read and wait; usleep may
 * yield, udelay does not.  The DPCD hooks reach the panel's sink.  panel
 * runs one step of the panel's power sequencer (0 when done).
 *
 * Display power is asked for by domain: power_domain names the domain of
 * one use of an output (the use and unit numbers of xd-output.h, and
 * whether a Type-C port runs in Thunderbolt mode), and the get, put and
 * delayed put take that number; get answers a nonzero cookie or 0.  The
 * numbering belongs to the backend.  power_get_if_enabled takes the
 * reference only when the domain is on already (NULL: the backend keeps
 * no state of its own).
 *
 * dbuf_slices_update asks for exactly the display buffer slices of the
 * mask.  The vertical blank hooks take and give back a pipe's interrupt
 * reference, sleep until the next blank or until the ticks of 10 ms run
 * out (answering the ticks left), turn interrupts off and on around a
 * short update, arm the completion event of the pipe's next blank, wait
 * for it (0, -110 when it did not come in time, -5 on a broken time
 * base) and give it up for good.
 *
 * observe is told the named points of a commit (may be NULL).  lock
 * takes (take 1) or drops (take 0) the PLL lock (0) or the backlight
 * lock (1).  error is told every error of a commit, the first one being
 * what a failed run is read from; debug every note.  step records a
 * callee the older display code did not carry over.
 *
 * The Type-C hooks take and give back an output's link to a port (0 is
 * the first Type-C port), answer the mode the port is held in, give the
 * display the lanes and answer the pin assignment; the Dekel hooks reach
 * a port's PHY registers under the lock the ports share.  All of them
 * are NULL in a model, which records a step instead.
 */
struct i915_xd_backend {
	void *ctx;
	int model;
	void (*write32)(void *ctx, uint32_t reg, uint32_t value);
	uint32_t (*rmw32)(void *ctx, uint32_t reg, uint32_t clear, uint32_t set);
	void (*posting_read)(void *ctx, uint32_t reg);
	void (*step)(void *ctx, const char *name);
	uint32_t (*read32)(void *ctx, uint32_t reg);
	int (*wait_reg)(void *ctx, uint32_t reg, uint32_t mask, uint32_t value, unsigned timeout_ms);
	void (*usleep)(void *ctx, unsigned us);
	void (*udelay)(void *ctx, unsigned us);
	long (*dpcd_read)(void *ctx, unsigned offset, uint8_t *buf, size_t size);
	long (*dpcd_write)(void *ctx, unsigned offset, const uint8_t *buf, size_t size);
	int (*read_dpcd_caps)(void *ctx, uint8_t dpcd[15]);
	int (*panel)(void *ctx, int op);
	int (*power_domain)(void *ctx, unsigned use, unsigned index, int thunderbolt);
	int (*power_get)(void *ctx, int domain);
	int (*power_get_if_enabled)(void *ctx, int domain);
	void (*power_put)(void *ctx, int domain, int cookie);
	void (*power_put_async)(void *ctx, int domain, int cookie, int delay_ms);
	void (*dbuf_slices_update)(void *ctx, unsigned req_slices);
	int (*vblank_get)(void *ctx, int pipe);
	void (*vblank_put)(void *ctx, int pipe);
	long (*vblank_sleep)(void *ctx, int pipe, long ticks);
	void (*irq_off)(void *ctx);
	void (*irq_on)(void *ctx);
	void (*arm_event)(void *ctx, int pipe);
	int (*wait_event)(void *ctx, int pipe, unsigned timeout_ms);
	void (*cancel_event)(void *ctx, int pipe);
	void (*observe)(void *ctx, int point);
	void (*lock)(void *ctx, int which, int take);
	void (*error)(void *ctx, const char *what);
	void (*debug)(void *ctx, const char *what);
	void (*tc_put_link)(void *ctx, int tc_port);
	void (*tc_get_link)(void *ctx, int tc_port, int lanes);
	int (*tc_mode)(void *ctx, int tc_port);
	void (*tc_set_fia_lane_count)(void *ctx, int tc_port, int lanes, int lane_reversal);
	unsigned (*tc_pin_assignment)(void *ctx, int tc_port);
	uint32_t (*dkl_read)(void *ctx, int tc_port, uint32_t phy_address);
	void (*dkl_write)(void *ctx, int tc_port, uint32_t phy_address, uint32_t value);
	void (*dkl_rmw)(void *ctx, int tc_port, uint32_t phy_address, uint32_t clear, uint32_t set);
};

#endif /* DRIVERS_GPU_I915_DISPLAY_XD_BACKEND_H */
