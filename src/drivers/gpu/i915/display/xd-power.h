/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The power wells of the display 12 and 13 code (xd-power.c).
 *
 * The display hardware is split into power wells, each switched through
 * a request bit and reporting through a state bit of one of three
 * controls (the main wells, the AUX wells, the DDI IO wells).  The
 * platform's description lists the wells with what each one needs: the
 * power gate whose fuses it waits for, whether it holds the VGA plane,
 * the pipes whose interrupt registers it holds, and whether it is the
 * Thunderbolt AUX well of a Type-C port.
 *
 * Around a well whose loss clears pipe interrupt registers the owner's
 * hooks restore the interrupts after it comes up and drain them before
 * it goes down; a drain that fails keeps the well on.  A well that holds
 * the VGA plane has the VGA registers rewritten after it comes up.  The
 * AUX well of a Type-C port in DisplayPort or legacy mode waits for its
 * PHY's microcontroller through the owner's hook.  The caller serializes
 * the calls (the power domains' lock).
 *
 * Above the switching, the counts keep who uses each well: a user names
 * the functions it needs (a pipe, a transcoder, the lanes or the IO of a
 * DDI, an AUX channel, the VGA plane, the exit from DC states), and the
 * wells whose sets hold them are brought up for the first user and taken
 * down after the last, the owner doing the switching.
 *
 * The DC states let the DMC firmware power the display down while it is
 * idle: the DC off well keeps the display out of them, and when it loses
 * its last user the state the options and the loaded firmware allow is
 * written.  DC9, the deepest, is entered around suspend with the PCH's
 * reference clock let go.
 */

#ifndef DRIVERS_GPU_I915_DISPLAY_XD_POWER_H
#define DRIVERS_GPU_I915_DISPLAY_XD_POWER_H

#include <stdint.h>

#include "xd-io.h"
#include "xd-platform.h"

/*
 * The owner's answers around a well's change.
 *
 * pipes_up restores the interrupts of the pipes in pipe_mask after their
 * well came up; pipes_down disables and drains them before it goes down
 * and reports nonzero when the drain failed (the well then stays on).
 * vga_reset rewrites the VGA registers after the well that holds the VGA
 * plane came up.  tc_health waits for the microcontroller of a Type-C
 * port's Dekel PHY (0 for the first port) after its AUX well came up,
 * and reports 0, ETIMEDOUT (only noted) or EIO (the time base failed).
 * tc_legacy tells whether a Type-C port (0 for the first) has a legacy
 * connector, whose AUX well on Ice Lake also asks the firmware to leave
 * TC cold.  port_edp tells whether a DDI drives an eDP panel, whose
 * combo AUX well on Ice Lake keeps its analog defaults.  Any hook may be
 * NULL (a NULL tc_legacy or port_edp answers no).
 */
struct i915_xd_power_hooks {
	void (*pipes_up)(void *context, unsigned pipe_mask);
	int (*pipes_down)(void *context, unsigned pipe_mask);
	void (*vga_reset)(void *context);
	int (*tc_health)(void *context, unsigned tc_port);
	int (*tc_legacy)(void *context, unsigned tc_port);
	int (*port_edp)(void *context, unsigned ddi);
	void *context;
};

/*
 * What a well's change met on the way that did not stop it: the state
 * bit that did not follow the request in time (on enable: the well may
 * still come up; a Thunderbolt AUX well with its tunnel down never
 * reports), and the power gates (a bit each) whose fuses were not
 * distributed in time.
 */
struct i915_xd_well_report {
	int state_late;
	uint32_t fuses_late;
};

/* The most wells a platform's table may have: one bit each in a 64-bit set. */
#define I915_XD_POWER_MAX_WELLS		64U

/*
 * How the owner switches one well of the table for the counts below:
 * on brings the well up and reports 0 or the error that stops the
 * caller; off takes it down and reports 0, or EBUSY when the well had to
 * stay on (it then keeps one user for good).  The calls come under the
 * owner's lock, the one the counts are kept under.
 */
struct i915_xd_power_switch {
	int (*on)(void *context, unsigned well);
	int (*off)(void *context, unsigned well);
	void *context;
};

/*
 * The owner's delayed work behind a deferred give-back: queue arms it to
 * run drv_i915_xd_power_work() after a delay in milliseconds and reports
 * 1 when it was not armed already; cancel disarms it (waiting for a run
 * in progress when sync is nonzero).
 */
struct i915_xd_power_work {
	int (*queue)(void *context, int delay_ms);
	int (*cancel)(void *context, int sync);
	void *context;
};

/*
 * What the counts have seen, for the owner's diagnostics: the deferred
 * give-backs, the gets that took a deferred user over, the runs of the
 * work (and those that found nothing owed), the wells the work gave
 * back, the times it armed itself again, the flushes, and the times the
 * counts were found inconsistent.
 */
struct i915_xd_power_stats {
	unsigned deferred;
	unsigned taken_over;
	unsigned work_runs;
	unsigned work_empty;
	unsigned released;
	unsigned requeues;
	unsigned flushes;
	unsigned errors;
};

/*
 * Who uses each power well of a display, and which wells are on.
 *
 * A user asks for a set of functions (I915_XD_POWERS_*); every well
 * whose set holds one of them counts the user, the first user brings the
 * well up in the table's order (a parent well comes before the wells
 * inside it), and the last one takes it down in the reverse order.  A
 * user may give its wells back later instead of now: they then stay up,
 * owed to the delayed work, until the work runs or a new user takes them
 * over.  The display owns one for the device's life.  The counts are kept
 * under the owner's lock; the record of which wells are on is read
 * without it, because an interrupt handler asks whether a pipe is powered.
 */
struct i915_xd_power {
	/* The platform's description, whose wells the counts follow. */
	const struct i915_xd_platform *platform;

	/* How many users each well has, the ones owed to the work included. */
	uint16_t refs[I915_XD_POWER_MAX_WELLS];

	/*
	 * The users each well owes to the delayed work: [0] the ones the
	 * armed work gives back when it runs, [1] the ones deferred while it
	 * was armed, which it gives back on the run after.
	 */
	uint16_t owed[2][I915_XD_POWER_MAX_WELLS];

	/* Nonzero while the work is armed and owed users; the delay of its next arming. */
	int work_armed;
	int next_delay;

	/* The owner's delayed work; without one a deferred give-back is immediate. */
	struct i915_xd_power_work work;

	/*
	 * The wells known to be on, a bit a table entry, as the owner
	 * recorded them after switching or reading them.  It is written with
	 * atomic stores and read without the lock.
	 */
	uint64_t on;

	/* What the counts have seen. */
	struct i915_xd_power_stats stats;

	/*
	 * The DC states: the ones the options allow, the one the display
	 * enters when DC off loses its last user, and whether the DMC
	 * firmware is loaded (without it no DC5 or DC6 is entered); the
	 * owner sets them.  Then the DC state bits last written, and the
	 * writes and the rewrites a state needed before it read back.
	 */
	uint32_t dc_allowed;
	uint32_t dc_target;
	int dc_firmware;
	uint32_t dc_state;
	unsigned dc_writes;
	unsigned dc_rewrites;
};

/*
 * What a DC state change met, for the owner's log: the DC state bits
 * found before the write when they were not the ones last written (the
 * DMC ignoring the requests), a state that never read back as written,
 * DC_STATE_EN as read by a DC9 check, and, before DC6, the utility pin in
 * PWM mode or DC6 already requested.
 */
struct i915_xd_dc_report {
	int mismatch;
	uint32_t expected;
	uint32_t found;
	int not_taken;
	uint32_t wanted;
	uint32_t now;
	uint32_t checked;
	int pw2_on;
	int pwm_pin;
	int dc6_already;
};

int drv_i915_xd9_power_pw2_on(struct i915_xd_io *io, const struct i915_xd_platform *platform);
int drv_i915_xd9_power_ddi_io(struct i915_xd_io *io, const struct i915_xd_platform *platform, unsigned ddi, int on);
int drv_i915_xd_power_find(const struct i915_xd_platform *platform, unsigned control, unsigned index);
void drv_i915_xd_power_init(struct i915_xd_power *power, const struct i915_xd_platform *platform);
void drv_i915_xd_power_bind_work(struct i915_xd_power *power, const struct i915_xd_power_work *work);
uint64_t drv_i915_xd_power_wells_of(const struct i915_xd_platform *platform, uint64_t functions);
int drv_i915_xd_power_get(struct i915_xd_power *power, uint64_t functions, const struct i915_xd_power_switch *sw);
void drv_i915_xd_power_put(struct i915_xd_power *power, uint64_t functions, const struct i915_xd_power_switch *sw);
void drv_i915_xd_power_put_later(struct i915_xd_power *power, uint64_t functions, int delay_ms, const struct i915_xd_power_switch *sw);
void drv_i915_xd_power_work(struct i915_xd_power *power, const struct i915_xd_power_switch *sw);
void drv_i915_xd_power_flush(struct i915_xd_power *power, const struct i915_xd_power_switch *sw);
int drv_i915_xd_power_owes(const struct i915_xd_power *power);
int drv_i915_xd_power_has(const struct i915_xd_power *power, uint64_t functions);
void drv_i915_xd_power_record(struct i915_xd_power *power, unsigned well, int on);
void drv_i915_xd_power_dc_policy(struct i915_xd_power *power, uint32_t allowed, uint32_t target, int firmware);
int drv_i915_xd_power_dc_write(struct i915_xd_power *power, struct i915_xd_io *io, uint32_t state, struct i915_xd_dc_report *report);
void drv_i915_xd_power_dc_leave(struct i915_xd_power *power, struct i915_xd_io *io, struct i915_xd_dc_report *report);
int drv_i915_xd_power_dc_enter(struct i915_xd_power *power, struct i915_xd_io *io, struct i915_xd_dc_report *report);
int drv_i915_xd_power_dc_off_held(const struct i915_xd_power *power, struct i915_xd_io *io);
void drv_i915_xd_power_dc_clear(struct i915_xd_power *power, struct i915_xd_io *io);
int drv_i915_xd_power_dc9_enter(struct i915_xd_power *power, struct i915_xd_io *io, int interrupts_enabled, struct i915_xd_dc_report *report);
int drv_i915_xd_power_dc9_leave(struct i915_xd_power *power, struct i915_xd_io *io, struct i915_xd_dc_report *report);
int drv_i915_xd_power_well_enable(struct i915_xd_io *io, const struct i915_xd_platform *platform, unsigned well, const struct i915_xd_power_hooks *hooks, struct i915_xd_well_report *report);
int drv_i915_xd_power_well_disable(struct i915_xd_io *io, const struct i915_xd_platform *platform, unsigned well, const struct i915_xd_power_hooks *hooks, struct i915_xd_well_report *report);
int drv_i915_xd_power_well_powered(struct i915_xd_io *io, const struct i915_xd_platform *platform, unsigned well);
int drv_i915_xd_power_well_owned(struct i915_xd_io *io, const struct i915_xd_platform *platform, unsigned well);
void drv_i915_xd_power_well_adopt(struct i915_xd_io *io, const struct i915_xd_platform *platform, unsigned well);

#endif /* DRIVERS_GPU_I915_DISPLAY_XD_POWER_H */
