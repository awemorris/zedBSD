/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The display's power (display-power.c).
 *
 * A user of the display names the functions it needs powered (a pipe, a
 * transcoder, the lanes or the IO of a DDI, an AUX channel, the exit from
 * the DC states: I915_XD_POWERS_*), and the counts of the xd power wells
 * bring up the wells that power them for the first user and take them down
 * after the last.  This file switches the wells for those counts, with the
 * display's own steps around a change: the interrupts of a well's pipes,
 * the VGA plane, a Type-C PHY's health, and, for the DC off well, the
 * checks of the CD clock and the display buffer and the combo PHYs that
 * the DMC does not keep.
 *
 * It also starts and stops the display core (the order is xd-core.c's,
 * with power well 1, the CD clock and the display buffer's slices here),
 * holds the start's reference that keeps every pipe's wells and the exit
 * from the DC states until the display is served, and enters and leaves
 * DC9 around a suspend.
 */

#ifndef DRIVERS_GPU_I915_DISPLAY_DISPLAY_POWER_H
#define DRIVERS_GPU_I915_DISPLAY_DISPLAY_POWER_H

#include <stdint.h>

#include <kern/lock.h>

#include "xd-power.h"

struct i915_xd;
struct i915_xd_dkl;
struct i915_vga_client;
struct i915_cdclk_dev;
struct i915_trace;
struct i915_pw_irq_ops;
struct i915_driver_probe;

/*
 * The functions the start's reference and the DMC loader's reference
 * hold: every pipe's wells and the exit from the DC states.
 */
#define I915_DISPLAY_POWER_START	(I915_XD_POWERS_PIPE(0U) | \
					 I915_XD_POWERS_PIPE(1U) | \
					 I915_XD_POWERS_PIPE(2U) | \
					 I915_XD_POWERS_PIPE(3U) | \
					 I915_XD_POWERS_DC_OFF)

/* The delay of a deferred give-back when the caller names none. */
#define I915_DISPLAY_POWER_LATER_MS	100

/*
 * The power of one display: the counts of its wells and what switching
 * them needs.
 *
 * The display owns one for the device's life.  Its lock serializes the
 * counts and every well's change; the record of which wells are on (in
 * the xd counts) is read without it, by the interrupt handler.  The
 * start fills the parts the wells' changes use before the first change,
 * and the interrupt binding fills the interrupt hooks before the
 * interrupts are installed.
 */
struct i915_display_power {
	/* The display's xd state: its register window, its platform and the counts of its wells. */
	struct i915_xd *xd;

	/* Serializes the counts and the wells' changes. */
	struct mutex lock;

	/* Nonzero between the start and the stop. */
	int ready;

	/*
	 * The DC states the display may enter, the one it enters when DC off
	 * loses its last user, and whether the DMC's program is loaded
	 * (without it no DC5 or DC6 is entered).
	 */
	uint32_t dc_allowed;
	uint32_t dc_target;
	int dmc_loaded;

	/* The VGA client a well holding the VGA plane resets, and the Type-C PHYs' windows. */
	struct i915_vga_client *vga;
	struct i915_xd_dkl *dkl;

	/* The CD clock the DC off well compares with what the hardware reads. */
	struct i915_cdclk_dev *cdclk;

	/*
	 * The interrupt layer's hooks around a pipe well's change, and whether
	 * the interrupts are on.  A pipe's interrupts that did not drain set
	 * irq_sync_failed for good: every later well off is refused, because
	 * the handler that did not finish may still read display registers.
	 */
	const struct i915_pw_irq_ops *irq_ops;
	void *irq_ctx;
	int irqs_enabled;
	int irq_sync_failed;

	/* The delayed work behind a deferred give-back; its cancel is called outside the lock. */
	struct i915_xd_power_work work;

	/*
	 * The display buffer's slices: the ones the display has, and the ones
	 * powered now.
	 */
	uint8_t dbuf_mask;
	uint8_t dbuf_slices;

	/*
	 * The start's reference (I915_DISPLAY_POWER_START) is held; the core
	 * went down for a suspend; the core's start stopped at a fault, and
	 * where.
	 */
	int start_held;
	int core_suspended;
	int core_faulted;
	const char *core_fault;

	/* The trace ring of the combo PHYs' start, the display's own. */
	struct i915_trace *trace;

	/* What the wells' changes met, for the logs. */
	unsigned refusals;
	unsigned kept_wells;
	unsigned late_states;
	unsigned late_health;
	unsigned vga_resets;
	unsigned later_puts;
	unsigned dc_off_on;
	unsigned dc_off_off;
};

int drv_i915_display_power_start(struct i915_display_power *power, struct i915_xd *xd, struct i915_trace *trace);
void drv_i915_display_power_stop(struct i915_display_power *power);
int drv_i915_display_power_get(struct i915_display_power *power, uint64_t functions);
void drv_i915_display_power_put(struct i915_display_power *power, uint64_t functions);
void drv_i915_display_power_put_later(struct i915_display_power *power, uint64_t functions, int delay_ms);
int drv_i915_display_power_is_on(struct i915_display_power *power, uint64_t functions);
void drv_i915_display_power_bind_work(struct i915_display_power *power, const struct i915_xd_power_work *work);
void drv_i915_display_power_work(struct i915_display_power *power);
void drv_i915_display_power_flush(struct i915_display_power *power);
uint64_t drv_i915_display_power_aux(unsigned display_ver, int aux_ch, int thunderbolt);
unsigned drv_i915_display_power_sanitize(struct i915_display_power *power);
unsigned drv_i915_display_power_wells_on(struct i915_display_power *power);
unsigned drv_i915_display_power_check(struct i915_display_power *power);
void drv_i915_display_power_log(struct i915_display_power *power);
void drv_i915_display_power_dbuf(struct i915_display_power *power, uint8_t slices);
int drv_i915_display_power_core_start(struct i915_display_power *power, unsigned dram_type, unsigned dram_channels);
void drv_i915_display_power_core_stop(struct i915_display_power *power);
void drv_i915_display_power_hold(struct i915_display_power *power);
void drv_i915_display_power_release(struct i915_display_power *power);
void drv_i915_display_power_serve(struct i915_display_power *power, struct i915_driver_probe *probe);
int drv_i915_display_power_dc9_enter(struct i915_display_power *power, int irqs_enabled);
int drv_i915_display_power_dc9_leave(struct i915_display_power *power);

#endif /* DRIVERS_GPU_I915_DISPLAY_DISPLAY_POWER_H */
