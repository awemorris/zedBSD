/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * GT power management: RC6 and RPS.
 *
 * RC6 lets the GT drop into its low-power state when idle, and render and
 * media power gating let the idle units switch off.  RPS picks the GT clock.
 * The GuC does not run: the driver writes the thresholds and the enables
 * itself.  On graphics version 12 RPS follows the load by an evaluation
 * every few milliseconds that compares the engines' busy time with the
 * thresholds (the GT's own up/down interrupts, which versions 6 to 11 use
 * and which are kept here, do not fire on Gen12), a work raises or lowers the requested
 * frequency, a client waiting on work the GT has not started boosts it,
 * and an idle GT at the minimum stops the evaluations until the engines
 * run again.
 *
 * The PCODE mailbox that RPS reads the efficient frequency from belongs to
 * power.c; this file only uses it.
 */

#ifndef DRIVERS_GPU_I915_GT_POWER_H
#define DRIVERS_GPU_I915_GT_POWER_H

#include <kern/lock.h>
#include <stdint.h>

#include "workqueue.h"

struct i915_mmio;
struct i915_gt_info;
struct i915_irq_dev;
struct mutex;

/*
 * The power modes the RPS thresholds are set for: the higher the frequency, the sooner the GT
 * counts as busy enough to go up and the later as idle enough to go down.
 * NONE means no thresholds have been written yet.
 */
#define I915_RPS_POWER_NONE		(-1)
#define I915_RPS_POWER_LOW		0
#define I915_RPS_POWER_BETWEEN		1
#define I915_RPS_POWER_HIGH		2

/* The power management interrupt events of RPS (GEN6_PM_RP_*). */
#define I915_RPS_DOWN_TIMEOUT		(1U << 6)
#define I915_RPS_UP_THRESHOLD		(1U << 5)
#define I915_RPS_DOWN_THRESHOLD		(1U << 4)
#define I915_RPS_UP_EI_EXPIRED		(1U << 2)
#define I915_RPS_DOWN_EI_EXPIRED	(1U << 1)
#define I915_RPS_EVENTS			(I915_RPS_UP_EI_EXPIRED | I915_RPS_UP_THRESHOLD | I915_RPS_DOWN_EI_EXPIRED | \
					 I915_RPS_DOWN_THRESHOLD | I915_RPS_DOWN_TIMEOUT)

/* No frequency has been written yet. */
#define I915_RPS_FREQ_NONE		0xffffffffU

/*
 * The RC6 state of one GT.
 *
 * It is built by the GT table construction and changed by the sanitize and
 * enable steps of the GT resume; the device start is its only user.
 */
struct i915_rc6 {
	/* Nonzero when the platform supports RC6 at all. */
	int supported;

	/* Nonzero once RC6 and power gating have been turned on. */
	int enabled;

	/* The RC_CONTROL value the enable writes. */
	uint32_t ctl_enable;

	/* The PG_ENABLE value the enable writes. */
	uint32_t pg_enable;

	/*
	 * Nonzero when the enable left RC6 and power gating off.  Nothing in
	 * the driver sets it; it stays in the state so the start report keeps
	 * the field it has always printed.
	 */
	int wa_disabled;
};

/*
 * The account of the time one GT spends in RC6.
 *
 * The hardware counts the RC6 time in a 32-bit register that wraps and
 * that reads true only while a forcewake domain holds the GT awake.  The
 * account samples it whenever the running device takes its domains, gives
 * them back, or is asked, and adds each step to a 64-bit total, so the
 * time the GT slept while parked is counted at the next wake.  A wrap is
 * lost only when two samples lie more than one counter period apart.
 *
 * It lives inside the device from attach to detach; lock guards every
 * other field and orders a reader's register access against the running
 * device taking or giving back its domains.  It is taken with interrupts
 * disabled.
 */
struct i915_rc6_residency {
	struct spinlock lock;

	/* Nonzero while the running device holds the domains, so the counter reads true. */
	int readable;

	/* Nonzero once the counter has been sampled at all. */
	int sampled;

	/* The counter at the last sample. */
	uint32_t last_count;

	/* The RC6 time added up since the first sample, in nanoseconds. */
	uint64_t total_ns;

	/* How many forcewake domains the running device holds now. */
	unsigned holds;

	/*
	 * How many times the counter was given up because the GT lost its
	 * power (a suspend to D3hot), after which the next sample only sets
	 * the base again: the time without power is not RC6 time.
	 */
	unsigned forgets;
};

/*
 * The frequency limits and state of one GT, in 16.67 MHz units.
 *
 * It is built by the GT table construction, changed by the enable step of
 * the GT resume, and driven from the start of the published node to its
 * stop: the interrupt handler hands the events to the work, which alone
 * requests frequencies from then on.  lock guards pm_iir, pm_imr, pm_ier and
 * waiters, and is taken with interrupts disabled.
 */
struct i915_rps {
	/* The highest, the sustainable and the lowest frequency the fuses allow. */
	uint32_t rp0_freq;
	uint32_t rp1_freq;
	uint32_t min_freq;

	/* The range the driver requests frequencies from. */
	uint32_t max_freq;

	/* The most power-efficient frequency. */
	uint32_t efficient_freq;

	/* Nonzero once a frequency has been requested. */
	int enabled;

	/* Nonzero when the PCODE reported the efficient frequency. */
	int pcode_ok;

	/*
	 * The range the work moves in (soft limits, which stay the whole range
	 * here), the frequency a boost jumps to (RP0), the
	 * frequency the driver last settled on and the one it last wrote
	 * (I915_RPS_FREQ_NONE before the first), and the last step the work
	 * took (its sign says up or down; consecutive steps double).
	 */
	uint32_t min_softlimit;
	uint32_t max_softlimit;
	uint32_t boost_freq;
	uint32_t cur_freq;
	uint32_t last_freq;
	int last_adj;

	/* The power mode of the thresholds written, and the busy percentages that go up and down. */
	int power_mode;
	uint32_t up_threshold;
	uint32_t down_threshold;

	/* The GT's command streamer clock in Hz, the unit of the evaluation intervals. */
	uint32_t clock_frequency;

	/* The interrupt events RPS follows (I915_RPS_UP_THRESHOLD and I915_RPS_DOWN_THRESHOLD). */
	uint32_t pm_events;

	/*
	 * The started part: the register access and the interrupt device the
	 * handler is registered with (NULL before the start), the events the
	 * handler saw that the work has not taken, the PM interrupt mask and
	 * enable as last written (the low half of GPM_WGBOXPERF's upper half),
	 * and how many clients wait on work the GT has not started.
	 */
	struct i915_mmio *mmio;
	struct i915_irq_dev *irq;
	struct spinlock lock;
	uint32_t pm_iir;
	uint32_t pm_imr;
	uint32_t pm_ier;
	unsigned waiters;

	/* The queue and the work that change the frequency; work_ready says they exist. */
	struct i915_workqueue workqueue;
	struct i915_work work;
	int work_ready;

	/*
	 * How the load is followed: nonzero for the evaluation over the
	 * engines' busy time (graphics version 12), zero for the GT's up/down
	 * interrupts (versions 6 to 11).
	 */
	int use_timer;

	/*
	 * Nonzero for a GT whose frequency is not followed at all (Gen9 in its
	 * first version): the start requests the efficient
	 * frequency once and starts neither the work nor the interrupts.
	 */
	int fixed;

	/*
	 * The busy-time evaluation: the engines' busy time of the runs that
	 * ended, when the runs under way began (busy_runs counts them), the
	 * busy time and the clock at the last evaluation (nanoseconds), the
	 * next evaluation's interval (1 ms after a change, doubling to 20 ms),
	 * whether the evaluations run (zero: parked, the GT idle at the
	 * minimum) and whether the next one starts the GT from the efficient
	 * frequency (unpark).  busy_* and ticking are guarded by lock; the
	 * timer queue fires the evaluations on the work's queue.
	 */
	uint64_t busy_total_ns;
	uint64_t busy_since_ns;
	unsigned busy_runs;
	uint64_t busy_seen_ns;
	uint64_t tick_ns;
	unsigned interval_ms;
	int ticking;
	int unparking;
	struct i915_timer_queue timers;
	struct i915_delayed_work tick_work;
	int timers_ready;

	/* What happened, for the diagnostics: events, boosts, work runs and frequency changes. */
	volatile unsigned up_events;
	volatile unsigned down_events;
	volatile unsigned boosts;
	volatile unsigned work_runs;
	volatile unsigned changes;
	volatile unsigned ticks;
	volatile unsigned parks;
	volatile unsigned unparks;

	/* How many frequency changes the log has reported (the first few only). */
	unsigned logged;

	/*
	 * Keeps the GT's registers awake around a frequency change, for a
	 * device whose forcewake domains are not held all the time: get
	 * returns 0 once they are held, put lets them go.  NULL when the
	 * caller holds the domains itself.  wake_failures counts the changes
	 * dropped because the GT did not wake.
	 */
	int (*awake_get)(void *context);
	void (*awake_put)(void *context);
	void *awake_context;
	volatile unsigned wake_failures;
};

void drv_i915_rc6_init(struct i915_rc6 *rc6, struct i915_mmio *mmio);
void drv_i915_rc6_enable(struct i915_rc6 *rc6, struct i915_mmio *mmio, const struct i915_gt_info *gt);
void drv_i915_rc6_disable(struct i915_rc6 *rc6, struct i915_mmio *mmio);
void drv_i915_rc6_residency_init(struct i915_rc6_residency *residency);
void drv_i915_rc6_residency_resume(struct i915_rc6_residency *residency, struct i915_mmio *mmio, unsigned holds);
void drv_i915_rc6_residency_pause(struct i915_rc6_residency *residency, struct i915_mmio *mmio);
void drv_i915_rc6_residency_forget(struct i915_rc6_residency *residency);
unsigned drv_i915_rc6_residency_forgets(struct i915_rc6_residency *residency);
int drv_i915_rc6_residency_read(struct i915_rc6_residency *residency, struct i915_mmio *mmio, uint64_t *sleep_ns, unsigned *holds);
uint64_t drv_i915_rc6_count_ns(uint32_t count);
void drv_i915_rps_init(struct i915_rps *rps, struct mutex *sb_lock, struct i915_mmio *mmio);
void drv_i915_rps_init_gen9(struct i915_rps *rps, struct i915_mmio *mmio);
void drv_i915_rps_enable(struct i915_rps *rps, struct i915_mmio *mmio, uint32_t clock_frequency);
void drv_i915_rps_mask_all(struct i915_rps *rps, struct i915_mmio *mmio);
int drv_i915_rps_start(struct i915_rps *rps, struct i915_irq_dev *irq, struct i915_mmio *mmio);
void drv_i915_rps_stop(struct i915_rps *rps);
void drv_i915_rps_irq(void *context, uint32_t pm_iir);
void drv_i915_rps_boost_begin(struct i915_rps *rps);
void drv_i915_rps_boost_end(struct i915_rps *rps);
void drv_i915_rps_busy_begin(struct i915_rps *rps);
void drv_i915_rps_busy_end(struct i915_rps *rps);
struct gpu_telemetry_entry;
int drv_i915_rps_telemetry_read(void *context, struct gpu_telemetry_entry *entry);
uint32_t drv_i915_rps_next_freq(const struct i915_rps *rps, uint32_t pm_iir, int client_boost, int *adj);
uint32_t drv_i915_rps_pm_mask(const struct i915_rps *rps, uint32_t freq);
uint32_t drv_i915_rps_limits(const struct i915_rps *rps, uint32_t freq);
uint32_t drv_i915_rps_pm_interval(uint32_t clock_frequency, uint64_t ns);

#endif
