/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * GT power management: RC6 and RPS (see gt-power.h).
 *
 * Every register access here goes through the held accessors: the device
 * start holds every GT forcewake domain across the GT initialization and
 * resume that call these functions, and for as long as the node is
 * published, which covers the RPS work and interrupts.  The
 * interrupt handler writes the PM mask with the raw accessor, as the rest of
 * the handler does.
 */

#include "i915.h"
#include "gt-power.h"
#include "irq.h"
#include "mmio.h"
#include "perf.h"
#include "power.h"
#include "device-info.h"

#include <kern/clock.h>
#include <kern/klog.h>
#include <kern/sched.h>
#include <uapi/sysctl.h>

#include <stddef.h>
#include <stdint.h>

#include "intel/gt-power.h"
#include "intel/gt-regs.h"

/* The busy share of an evaluation interval above which the GT goes up, and below which it goes down, in percent. */
#define I915_RPS_UP_THRESHOLD_PERCENT		95U
#define I915_RPS_DOWN_THRESHOLD_PERCENT		85U

/*
 * The down evaluation interval of every power mode, in microseconds.
 *
 * Values referenced: Linux v6.19 drivers/gpu/drm/i915/gt/intel_rps.c
 * (rps_set_power: ei_down).
 */
#define I915_RPS_DOWN_EI_US			32000U

/* How long the stop waits for a running RPS work, in milliseconds. */
#define I915_RPS_STOP_WAIT_MS			1000U

/*
 * The longest interval between two busy-time evaluations, in milliseconds.
 *
 * Values referenced: Linux v6.19 drivers/gpu/drm/i915/gt/intel_rps.c
 * (BUSY_MAX_EI).
 */
#define I915_RPS_BUSY_MAX_EI_MS			20U

/* How many frequency changes the log reports after the start. */
#define I915_RPS_LOG_CHANGES			24U

/* The nanoseconds of a second, for the evaluation intervals. */
#define I915_RPS_NS_PER_SECOND			1000000000ULL

/*
 * The up evaluation interval of each power mode, in microseconds: the
 * busier the mode, the shorter the interval, so a GT already fast goes up
 * sooner.  Indexed by I915_RPS_POWER_LOW, _BETWEEN and _HIGH; the table
 * never changes.
 *
 * Values referenced: Linux v6.19 drivers/gpu/drm/i915/gt/intel_rps.c
 * (rps_set_power: ei_up).
 */
static const uint32_t i915_rps_up_ei_us[3] = { 16000U, 13000U, 10000U };

/* One register write of the RC6 setup. */
struct i915_rc6_write {
	uint32_t reg;
	uint32_t value;
};

/*
 * The RC6 wake, evaluation and idle thresholds, written before the
 * engines' idle times.
 *
 * Values referenced: Linux v6.19 drivers/gpu/drm/i915/gt/intel_rc6.c
 * (gen11_rc6_enable).
 */
static const struct i915_rc6_write i915_rc6_before_engines[] = {
	{ I915_GT_GEN6_RC6_WAKE_RATE_LIMIT, (54U << 16) | 85U },
	{ I915_GT_GEN10_MEDIA_WAKE_RATE_LIMIT, 150U },
	{ I915_GT_GEN6_RC_EVALUATION_INTERVAL, 125000U },
	{ I915_GT_GEN6_RC_IDLE_HYSTERSIS, 25U },
};

/*
 * The GuC idle count, the RC sleep, the RC6 threshold and the power gating
 * hysteresis of render and media, written after the engines' idle times.
 *
 * Values referenced: Linux v6.19 drivers/gpu/drm/i915/gt/intel_rc6.c
 * (gen11_rc6_enable).
 */
static const struct i915_rc6_write i915_rc6_after_engines[] = {
	{ I915_GT_GUC_MAX_IDLE_COUNT, 0xaU },
	{ I915_GT_GEN6_RC_SLEEP, 0U },
	{ I915_GT_GEN6_RC6_THRESHOLD, 50000U },
	{ I915_GT_GEN9_MEDIA_PG_IDLE_HYSTERESIS, 60U },
	{ I915_GT_GEN9_RENDER_PG_IDLE_HYSTERESIS, 60U },
};

/* The idle time each engine waits before it may power down. */
#define I915_RC6_ENGINE_MAX_IDLE	10U

/* The number of entries of a table. */
#define I915_RC6_COUNT(table)		(sizeof(table) / sizeof((table)[0]))

static void i915_rc6_write_all(struct i915_mmio *mmio, const struct i915_rc6_write *writes, unsigned count);

static void i915_rps_write_imr(struct i915_rps *rps);
static void i915_rps_write_ier(struct i915_rps *rps);
static void i915_rps_mask_events_locked(struct i915_rps *rps, uint32_t events, uint32_t open);
static void i915_rps_program_mode(struct i915_rps *rps, struct i915_mmio *mmio, int mode);
static int i915_rps_mode_for(const struct i915_rps *rps, uint32_t freq);
static void i915_rps_request(struct i915_rps *rps, struct i915_mmio *mmio, uint32_t freq, int with_mode);
static void i915_rps_move_to(struct i915_rps *rps, uint32_t freq);
static void i915_rps_adjust(void *context);
static void i915_rps_open_events(struct i915_rps *rps, struct i915_irq_dev *irq, struct i915_mmio *mmio);
static void i915_rps_start_ticking(struct i915_rps *rps);
static void i915_rps_arm_tick(struct i915_rps *rps, unsigned delay_ms);
static void i915_rps_tick(void *context);
static void i915_rps_log(struct i915_rps *rps, const char *reason, uint32_t from, uint32_t events);

/*
 * Prepares the RC6 state and makes sure RC6 is off: RC6 must stay disabled
 * until the GT is ready for it.
 */
void
drv_i915_rc6_init(
	struct i915_rc6 *rc6,
	struct i915_mmio *mmio)
{
	/* Every supported GT has RC6. */
	rc6->supported = 1;
	rc6->enabled = 0;
	rc6->ctl_enable = 0U;
	rc6->pg_enable = 0U;
	rc6->wa_disabled = 0;

	/* Sanitizes RC6: it is disabled before the driver is ready for it. */
	drv_i915_write32(mmio, I915_GT_GEN6_RC_CONTROL, 0U);
}

/*
 * Programs the RC6 thresholds and turns on RC6 and power gating.  The GuC
 * does not run, so the driver owns RC_CONTROL.
 */
void
drv_i915_rc6_enable(
	struct i915_rc6 *rc6,
	struct i915_mmio *mmio,
	const struct i915_gt_info *gt)
{
	uint32_t pg_enable;
	unsigned vcs;
	unsigned engine_index;

	/* A GT without RC6 is left alone. */
	if (rc6->supported == 0)
		return;

	/*
	 * Gen9 keeps RC6 and power gating off in its first version: the RC6
	 * context-corruption workaround it needs is not ported.  The sanitize
	 * has already written both controls to 0.
	 */
	if (gt->graphics_ver == 9)
		return;

	/* The thresholds, every engine's idle time, then the rest of the thresholds. */
	i915_rc6_write_all(mmio, i915_rc6_before_engines, I915_RC6_COUNT(i915_rc6_before_engines));
	for (engine_index = 0U; engine_index < gt->num_engines; engine_index++)
		drv_i915_write32(mmio, I915_GT_RING_MAX_IDLE(gt->engines[engine_index].mmio_base), I915_RC6_ENGINE_MAX_IDLE);
	i915_rc6_write_all(mmio, i915_rc6_after_engines, I915_RC6_COUNT(i915_rc6_after_engines));

	/* Without the GuC's RC6 the driver itself enables RC6. */
	rc6->ctl_enable = I915_GT_GEN6_RC_CTL_RC6_ENABLE;

	/* Render, media and media sampler power gating are always enabled. */
	pg_enable = I915_GT_GEN9_RENDER_PG_ENABLE | I915_GT_GEN9_MEDIA_PG_ENABLE | I915_GT_GEN11_MEDIA_SAMPLER_PG_ENABLE;

	/* Graphics version 12 also gates the HCP and MFX units of every video decode engine that exists; Gen11 does not. */
	if (gt->graphics_ver >= 12) {
		for (vcs = 0U; vcs < I915_MAX_VCS; vcs++) {
			/* Looks for the video decode engine of this instance. */
			for (engine_index = 0U; engine_index < gt->num_engines; engine_index++) {
				/* Only a video decode engine has HCP and MFX units. */
				if (gt->engines[engine_index].class != I915_VIDEO_DECODE_CLASS)
					continue;

				/* Another instance is gated by its own bits. */
				if ((unsigned)gt->engines[engine_index].instance != vcs)
					continue;

				/* Gates this instance's HCP and MFX units. */
				pg_enable |= I915_GT_VDN_HCP_POWERGATE_ENABLE(vcs) | I915_GT_VDN_MFX_POWERGATE_ENABLE(vcs);
				break;
			}
		}
	}

	/* Records the gating mask the enable writes, for the start report. */
	rc6->pg_enable = pg_enable;

	/* Turns on power gating, then RC6. */
	drv_i915_write32(mmio, I915_GT_GEN9_PG_ENABLE, pg_enable);
	drv_i915_write32(mmio, I915_GT_GEN6_RC_CONTROL, rc6->ctl_enable);

	/* The GT now enters RC6 on its own whenever it idles. */
	rc6->enabled = 1;
}

/*
 * Turns RC6 and power gating off before the GT is reinitialized, straight
 * at the registers.
 */
void
drv_i915_rc6_disable(
	struct i915_rc6 *rc6,
	struct i915_mmio *mmio)
{
	/* An RC6 still enabled here would be an unbalanced suspend and resume. */
	rc6->enabled = 0;

	/* A GT without RC6 has nothing to turn off. */
	if (rc6->supported == 0)
		return;

	/* Turns off power gating and RC6, and clears the RC state. */
	drv_i915_write32(mmio, I915_GT_GEN9_PG_ENABLE, 0U);
	drv_i915_write32(mmio, I915_GT_GEN6_RC_CONTROL, 0U);
	drv_i915_write32(mmio, I915_GT_GEN6_RC_STATE, 0U);
}

/*
 * Reads the GT frequency limits and the efficient frequency: RP0 and the
 * minimum from the capabilities, RP1 from the frequency information, and
 * the efficient frequency from the PCODE when it answers.
 */
void
drv_i915_rps_init(
	struct i915_rps *rps,
	struct mutex *sb_lock,
	struct i915_mmio *mmio)
{
	uint32_t capabilities;
	uint32_t frequency_info;
	uint32_t ddcc;
	uint32_t ddcc_high;
	uint32_t efficient;
	int pcode_error;

	ddcc = 0U;
	ddcc_high = 0U;

	/* Reads RP0 and the minimum from the capabilities, RP1 from the frequency info. */
	capabilities = drv_i915_read32(mmio, I915_GT_GEN6_RP_STATE_CAP);
	rps->rp0_freq = (capabilities >> 0) & 0xffU;
	frequency_info = drv_i915_read32(mmio, I915_GT_GEN10_FREQ_INFO_REC);
	rps->rp1_freq = (frequency_info & I915_GT_RPE_MASK) >> 8;
	rps->min_freq = (capabilities >> 16) & 0xffU;

	/* The capabilities are in 50 MHz units; the driver counts in 16.67 MHz. */
	rps->rp0_freq *= I915_GT_GEN9_FREQ_SCALER;
	rps->rp1_freq *= I915_GT_GEN9_FREQ_SCALER;
	rps->min_freq *= I915_GT_GEN9_FREQ_SCALER;

	/* Requests may use the whole fused range; RP1 is efficient until PCODE says otherwise. */
	rps->max_freq = rps->rp0_freq;
	rps->efficient_freq = rps->rp1_freq;

	/* Asks the PCODE for the efficient frequency. */
	pcode_error = drv_i915_pcode_read(sb_lock, mmio, I915_GT_HSW_PCODE_DYNAMIC_DUTY_CYCLE_CONTROL, &ddcc, &ddcc_high);
	rps->pcode_ok = 0;
	if (pcode_error == 0)
		rps->pcode_ok = 1;

	/* Takes the PCODE's efficient frequency, clamped into the fused range. */
	if (rps->pcode_ok != 0) {
		efficient = ((ddcc >> 8) & 0xffU) * I915_GT_GEN9_FREQ_SCALER;

		/* The efficient frequency is never below the minimum. */
		if (efficient < rps->min_freq)
			efficient = rps->min_freq;

		/* Nor above the maximum. */
		if (efficient > rps->max_freq)
			efficient = rps->max_freq;

		rps->efficient_freq = efficient;
	}

	/* No frequency has been requested yet. */
	rps->enabled = 0;

	/* The work moves in the whole range; a boost goes to RP0. */
	rps->min_softlimit = rps->min_freq;
	rps->max_softlimit = rps->max_freq;
	rps->boost_freq = rps->max_freq;
	rps->cur_freq = rps->min_freq;
	rps->last_freq = I915_RPS_FREQ_NONE;
	rps->last_adj = 0;

	/*
	 * No thresholds are written yet.
	 *
	 * Values referenced: Linux v6.19 drivers/gpu/drm/i915/gt/intel_rps.c
	 * (intel_rps_init: the up and down threshold percentages).
	 */
	rps->power_mode = I915_RPS_POWER_NONE;
	rps->up_threshold = I915_RPS_UP_THRESHOLD_PERCENT;
	rps->down_threshold = I915_RPS_DOWN_THRESHOLD_PERCENT;

	/* Nothing is started: no events, every PM interrupt masked and disabled, no waiter. */
	rps->pm_events = 0U;
	rps->mmio = NULL;
	rps->irq = NULL;
	spin_init(&rps->lock, LOCK_RANK_DEVICE, "i915 rps");
	rps->pm_iir = 0U;
	rps->pm_imr = 0xffffffffU;
	rps->pm_ier = 0U;
	rps->waiters = 0U;
	rps->work_ready = 0;
	rps->logged = 0U;

	/* Graphics version 12 follows the engines' busy time. */
	rps->use_timer = 1;
	rps->fixed = 0;
	rps->busy_total_ns = 0U;
	rps->busy_since_ns = 0U;
	rps->busy_runs = 0U;
	rps->busy_seen_ns = 0U;
	rps->tick_ns = 0U;
	rps->interval_ms = 1U;
	rps->ticking = 0;
	rps->unparking = 0;
	rps->timers_ready = 0;
}

/*
 * Corrects the RPS state of a Gen9 GT after drv_i915_rps_init() and fixes
 * its frequency.
 *
 * Gen9 has RP1 in I915_GT_GEN6_RP_STATE_CAP bits 15:8 (it has no
 * I915_GT_GEN10_FREQ_INFO_REC), and the efficient frequency falls back to it when
 * the PCODE did not answer.  The first Gen9 version keeps the frequency
 * at that request (drv_i915_rps_start()).
 */
void
drv_i915_rps_init_gen9(
	struct i915_rps *rps,
	struct i915_mmio *mmio)
{
	uint32_t capabilities;

	/* Reads RP1 from the capabilities, in 50 MHz units. */
	capabilities = drv_i915_read32(mmio, I915_GT_GEN6_RP_STATE_CAP);
	rps->rp1_freq = ((capabilities >> I915_GT_GEN9_RP1_CAP_SHIFT) & 0xffU) * I915_GT_GEN9_FREQ_SCALER;

	/* Without the PCODE's answer RP1 is the efficient frequency. */
	if (rps->pcode_ok == 0)
		rps->efficient_freq = rps->rp1_freq;

	/* The frequency is requested once and not followed. */
	rps->fixed = 1;
}

/*
 * Programs the RPS defaults and requests the minimum frequency with its
 * thresholds.
 *
 * The interrupts that follow the load are started with the node
 * (drv_i915_rps_start()), which also raises the request to the efficient
 * frequency.  clock_frequency is the GT's command
 * streamer clock in Hz, which the evaluation intervals count in.
 */
void
drv_i915_rps_enable(
	struct i915_rps *rps,
	struct i915_mmio *mmio,
	uint32_t clock_frequency)
{
	/* A GT with no room between its limits has nothing to reclock. */
	if (rps->max_freq <= rps->min_freq)
		return;

	/* The unit of the evaluation intervals. */
	rps->clock_frequency = clock_frequency;

	/* Programs the idle hysteresis; Gen9 also gives RP1 to the video frequency. */
	if (rps->fixed != 0)
		drv_i915_write32(mmio, I915_GT_GEN6_RC_VIDEO_FREQ, I915_GT_GEN9_FREQUENCY(rps->rp1_freq));
	drv_i915_write32(mmio, I915_GT_GEN6_RP_IDLE_HYSTERSIS, 0xaU);

	/* RPS follows the up and down threshold events. */
	rps->pm_events = I915_RPS_UP_THRESHOLD | I915_RPS_DOWN_THRESHOLD;

	/* Forces a fresh request and fresh thresholds. */
	rps->power_mode = I915_RPS_POWER_NONE;
	rps->last_freq = I915_RPS_FREQ_NONE;
	i915_rps_request(rps, mmio, rps->min_freq, 1);
	rps->cur_freq = rps->min_freq;

	/* A frequency is now requested. */
	rps->enabled = 1;
}

/*
 * Masks the RPS interrupts before the GT is reinitialized.
 */
void
drv_i915_rps_mask_all(
	struct i915_rps *rps,
	struct i915_mmio *mmio)
{
	UNUSED_PARAMETER(rps);

	/*
	 * PMINTRMSK masks everything; no bit of it must stay zero on graphics
	 * version 11 and later.  The GPM IER and IMR already hold what the
	 * interrupt setup programmed (IER 0, IMR ~0), so they are not written.
	 */
	drv_i915_write32(mmio, I915_GT_GEN6_PMINTRMSK, 0xffffffffU);
}

/*
 * Starts following the load: the work and its queue, the interrupt
 * handler, the PM interrupts, and the request raised to the efficient
 * frequency.
 *
 * The caller holds every forcewake domain until drv_i915_rps_stop().  Returns
 * 0, or the error of the work queue's creation (RPS then stays at the
 * enable's request).
 */
int
drv_i915_rps_start(
	struct i915_rps *rps,
	struct i915_irq_dev *irq,
	struct i915_mmio *mmio)
{
	uint32_t freq;
	int error;

	/* A GT that RPS did not enable has nothing to follow. */
	if (rps->enabled == 0)
		return 0;

	/*
	 * A GT whose frequency is not followed gets the efficient frequency
	 * once; no work, timer or interrupt is started, so the stop has
	 * nothing to undo.
	 */
	if (rps->fixed != 0) {
		rps->mmio = mmio;
		i915_rps_move_to(rps, rps->efficient_freq);
		kern_logf("i915: rps: fixed at RPe=%u (min=%u RP0=%u)\n",
		    rps->cur_freq,
		    rps->min_freq,
		    rps->rp0_freq);
		return 0;
	}

	/* The queue and the work that change the frequency. */
	error = drv_i915_workqueue_create(&rps->workqueue, "i915-rps");
	if (error != 0) {
		kern_logf("i915: rps: work queue not created (%d); the frequency stays at %u\n", error, rps->cur_freq);
		return error;
	}

	/* The work is ready from here on. */
	drv_i915_work_init(&rps->work, i915_rps_adjust, rps);
	rps->work_ready = 1;
	rps->mmio = mmio;

	/* The evaluations' timer, on the same queue (without it the frequency stays where the start puts it). */
	if (rps->use_timer != 0) {
		error = drv_i915_timer_queue_create(&rps->timers, &rps->workqueue, "i915-rps-timer");
		if (error == 0) {
			drv_i915_delayed_work_init(&rps->tick_work, i915_rps_tick, rps);
			rps->timers_ready = 1;
		} else {
			kern_logf("i915: rps: evaluation timer not created (%d)\n", error);
		}
	}

	/* Starts from the efficient frequency, or higher, before any interrupt can reach the work. */
	freq = rps->cur_freq;
	if (freq < rps->efficient_freq)
		freq = rps->efficient_freq;
	i915_rps_move_to(rps, freq);
	rps->last_adj = 0;

	/* The busy-time evaluation needs no interrupt: it starts ticking now. */
	rps->irq = irq;
	if (rps->use_timer != 0) {
		i915_rps_start_ticking(rps);
	} else {
		i915_rps_open_events(rps, irq, mmio);
	}

	/* The start's report: the way the load is followed, the frequencies, and the RP control as the GT reads it back. */
	kern_logf("i915: rps: started timer=%d min=%u RPe=%u RP0=%u freq=%u clock=%uHz rp_control=0x%x\n",
	    rps->use_timer,
	    rps->min_freq,
	    rps->efficient_freq,
	    rps->rp0_freq,
	    rps->cur_freq,
	    rps->clock_frequency,
	    drv_i915_read32(mmio, I915_GT_GEN6_RP_CONTROL));

	/* Succeeded: the frequency follows the load. */
	return 0;
}

/*
 * Stops following the load: masks and disables the PM interrupts,
 * unregisters the handler, and waits for the work before its queue goes.
 *
 * The last requested frequency stays.  The caller still holds the forcewake domains.
 */
void
drv_i915_rps_stop(
	struct i915_rps *rps)
{
	struct i915_irq_dev *irq;
	unsigned long flags;

	/* Nothing was started. */
	irq = rps->irq;
	if (irq == NULL)
		return;

	/* Masks every RPS event at the PM mask. */
	drv_i915_write32(rps->mmio, I915_GT_GEN6_PMINTRMSK, 0xffffffffU);

	/* Disables and masks the RPS events. */
	flags = spin_lock_irqsave(&rps->lock);

	rps->pm_ier &= ~I915_RPS_EVENTS;
	i915_rps_mask_events_locked(rps, I915_RPS_EVENTS, 0U);
	i915_rps_write_ier(rps);

	spin_unlock_irqrestore(&rps->lock, flags);

	/* Unregisters the handler and waits for one still running. */
	irq->pm_handler = NULL;
	(void)drv_i915_synchronize_irq(irq);

	/* The evaluations' timer goes first: it feeds the work's queue. */
	if (rps->timers_ready != 0) {
		(void)drv_i915_delayed_cancel_sync(&rps->timers, &rps->tick_work, sched_ticks() + kern_ms_to_ticks(I915_RPS_STOP_WAIT_MS));
		drv_i915_timer_queue_destroy(&rps->timers);
		rps->timers_ready = 0;
	}

	/* Waits for the work, then lets its queue go. */
	if (rps->work_ready != 0) {
		(void)drv_i915_cancel_work_sync(&rps->workqueue, &rps->work, sched_ticks() + kern_ms_to_ticks(I915_RPS_STOP_WAIT_MS));
		drv_i915_workqueue_destroy(&rps->workqueue);
		rps->work_ready = 0;
	}

	/* Clears what is still pending. */
	drv_i915_gt_pm_reset_iir(irq);
	flags = spin_lock_irqsave(&rps->lock);

	rps->pm_iir = 0U;

	spin_unlock_irqrestore(&rps->lock, flags);

	/* Nothing is started any more. */
	irq->pm_context = NULL;
	rps->irq = NULL;
	rps->mmio = NULL;
}

/*
 * Takes the GT power management events of one interrupt: the RPS events
 * among them are masked until the work has taken them.
 *
 * It runs in the interrupt handler, with the RPS state as its argument.
 */
void
drv_i915_rps_irq(
	void *context,
	uint32_t pm_iir)
{
	struct i915_rps *rps;
	unsigned long flags;
	uint32_t events;

	/* Only the events RPS follows. */
	rps = context;
	events = pm_iir & rps->pm_events;
	if (events == 0U)
		return;

	/* Masks them and hands them to the work. */
	flags = spin_lock_irqsave(&rps->lock);

	i915_rps_mask_events_locked(rps, events, 0U);
	rps->pm_iir |= events;

	spin_unlock_irqrestore(&rps->lock, flags);

	/* Counts which way the GT asked to go. */
	if ((events & I915_RPS_UP_THRESHOLD) != 0U)
		rps->up_events++;
	if ((events & I915_RPS_DOWN_THRESHOLD) != 0U)
		rps->down_events++;

	/* The work changes the frequency. */
	(void)drv_i915_queue_work(&rps->workqueue, &rps->work);
}

/*
 * Notes that a client waits on work the GT has not started; the first
 * such waiter boosts the frequency to RP0 until the last one is done.
 *
 * A request wait calls it for a request not yet started.  May be called before RPS starts, when it only counts.
 */
void
drv_i915_rps_boost_begin(
	struct i915_rps *rps)
{
	unsigned long flags;
	unsigned waiters;

	/* One more waiter. */
	flags = spin_lock_irqsave(&rps->lock);

	rps->waiters++;
	waiters = rps->waiters;

	spin_unlock_irqrestore(&rps->lock, flags);

	/* Only the first waiter boosts, and only a started RPS below the boost frequency. */
	if (waiters != 1U)
		return;
	if (rps->work_ready == 0)
		return;
	if (rps->cur_freq >= rps->boost_freq)
		return;

	/* The work raises the frequency. */
	rps->boosts++;
	(void)drv_i915_queue_work(&rps->workqueue, &rps->work);
}

/*
 * Notes that a waiter drv_i915_rps_boost_begin() counted is done; the next events may lower the frequency again.
 */
void
drv_i915_rps_boost_end(
	struct i915_rps *rps)
{
	unsigned long flags;

	/* One fewer waiter; an unbalanced end changes nothing. */
	flags = spin_lock_irqsave(&rps->lock);

	if (rps->waiters != 0U)
		rps->waiters--;

	spin_unlock_irqrestore(&rps->lock, flags);
}

/*
 * Notes that an engine starts running a request: its busy time counts from
 * now.  A parked evaluation (the GT idle at the minimum) starts again, from
 * the efficient frequency.  Called by the thread
 * that runs the requests, not in an interrupt.
 */
void
drv_i915_rps_busy_begin(
	struct i915_rps *rps)
{
	unsigned long flags;
	uint64_t now;
	int unpark;

	/* The clock, before the lock. */
	now = drv_i915_perf_now();

	/* One more run under way; the first one starts the busy time. */
	flags = spin_lock_irqsave(&rps->lock);

	if (rps->busy_runs == 0U)
		rps->busy_since_ns = now;
	rps->busy_runs++;
	unpark = 0;
	if (rps->timers_ready != 0 && rps->ticking == 0) {
		rps->ticking = 1;
		rps->unparking = 1;
		unpark = 1;
	}

	spin_unlock_irqrestore(&rps->lock, flags);

	/* A parked evaluation starts again at once. */
	if (unpark) {
		rps->unparks++;
		i915_rps_arm_tick(rps, 1U);
	}
}

/*
 * Notes that an engine finished running a request: the run's time adds to
 * the busy time.
 */
void
drv_i915_rps_busy_end(
	struct i915_rps *rps)
{
	unsigned long flags;
	uint64_t now;

	/* The clock, before the lock. */
	now = drv_i915_perf_now();

	/* One fewer run; the last one adds the time since the first began (an unbalanced end changes nothing). */
	flags = spin_lock_irqsave(&rps->lock);

	if (rps->busy_runs != 0U) {
		rps->busy_runs--;
		if (rps->busy_runs == 0U && now > rps->busy_since_ns)
			rps->busy_total_ns += now - rps->busy_since_ns;
	}

	spin_unlock_irqrestore(&rps->lock, flags);
}

/*
 * Reads the GT's telemetry for hw.gputelemetry: the busy time
 * the runs added up, with the run under way counted to now, the frequency
 * the driver last wrote, and the range the work moves in.
 *
 * It only reads what the RPS already keeps (no register, no change), so it
 * does not touch the frequency's control.  The context is the device's
 * struct i915_rps, which lives as long as the kernel.  Returns 0.
 */
int
drv_i915_rps_telemetry_read(
	void *context,
	struct gpu_telemetry_entry *entry)
{
	struct i915_rps *rps;
	unsigned long flags;
	uint64_t now;
	uint64_t busy;
	uint32_t written;
	uint32_t lowest;
	uint32_t highest;
	int enabled;

	/* The clock, before the lock, as the busy accounting reads it. */
	rps = context;
	now = drv_i915_perf_now();

	/* The busy time to now, and the frequencies, read together. */
	flags = spin_lock_irqsave(&rps->lock);

	/* The finished runs' time and the run under way's to now; the frequencies as they are. */
	busy = rps->busy_total_ns;
	if (rps->busy_runs != 0U && now > rps->busy_since_ns)
		busy += now - rps->busy_since_ns;
	written = rps->last_freq;
	lowest = rps->min_softlimit;
	highest = rps->max_softlimit;
	enabled = rps->enabled;

	/* The RPS goes on. */
	spin_unlock_irqrestore(&rps->lock, flags);

	/* The busy time at the driver's clock. */
	entry->time_ns = now;
	entry->busy_ns = busy;
	entry->valid |= GPU_TELEMETRY_BUSY;

	/* The frequency last written, in MHz (the driver counts in 50/3 MHz). */
	if (enabled && written != I915_RPS_FREQ_NONE) {
		entry->req_mhz = written * 50U / I915_GT_GEN9_FREQ_SCALER;
		entry->valid |= GPU_TELEMETRY_REQ_MHZ;
	}

	/* The range the work moves in, once it is known. */
	if (highest != 0U) {
		entry->min_mhz = lowest * 50U / I915_GT_GEN9_FREQ_SCALER;
		entry->max_mhz = highest * 50U / I915_GT_GEN9_FREQ_SCALER;
		entry->valid |= GPU_TELEMETRY_RANGE_MHZ;
	}

	/* Succeeded: the entry holds what the RPS keeps. */
	return 0;
}

/*
 * Computes the frequency the work requests next from the events it took
 * and whether a client waits, and the step it took (*adj).
 *
 * A waiting client jumps to the
 * boost frequency; an up event goes up by a step that doubles while the
 * GT keeps asking; a down timeout drops to the efficient frequency, then
 * to the minimum; a down event goes down by a step that doubles likewise;
 * the result stays within the soft limits (up to RP0 for a waiting
 * client).
 *
 * Procedure referenced: Linux v6.19 drivers/gpu/drm/i915/gt/intel_rps.c
 * (rps_work: the frequency decision).
 */
uint32_t
drv_i915_rps_next_freq(
	const struct i915_rps *rps,
	uint32_t pm_iir,
	int client_boost,
	int *adj)
{
	long freq;
	long lowest;
	long highest;
	int step;

	/* From the last step and the current frequency, within the soft limits (RP0 for a client). */
	step = rps->last_adj;
	freq = (long)rps->cur_freq;
	lowest = (long)rps->min_softlimit;
	highest = (long)rps->max_softlimit;
	if (client_boost)
		highest = (long)rps->max_freq;

	/* Picks the step by what happened, the waiting client first. */
	if (client_boost && freq < (long)rps->boost_freq) {
		/* A waiting client below the boost frequency jumps there. */
		freq = (long)rps->boost_freq;
		step = 0;
	} else if ((pm_iir & I915_RPS_UP_THRESHOLD) != 0U) {
		/* Busy: up by one, or by twice the last step when the last went up too. */
		if (step > 0) {
			step *= 2;
		} else {
			step = 1;
		}

		/* Already at the top, no step. */
		if (freq >= (long)rps->max_softlimit)
			step = 0;
	} else if (client_boost) {
		/* A waiting client keeps the frequency where it is. */
		step = 0;
	} else if ((pm_iir & I915_RPS_DOWN_TIMEOUT) != 0U) {
		/* Idle for the timeout: the efficient frequency, then the minimum. */
		if ((long)rps->cur_freq > (long)rps->efficient_freq) {
			freq = (long)rps->efficient_freq;
		} else if ((long)rps->cur_freq > lowest) {
			freq = lowest;
		}

		/* A jump, not a step. */
		step = 0;
	} else if ((pm_iir & I915_RPS_DOWN_THRESHOLD) != 0U) {
		/* Idle: down by one, or by twice the last step when the last went down too. */
		if (step < 0) {
			step *= 2;
		} else {
			step = -1;
		}

		/* Already at the bottom, no step. */
		if (freq <= lowest)
			step = 0;
	} else {
		/* An event RPS does not follow. */
		step = 0;
	}

	/* The step, kept within the limits. */
	freq += step;
	if (freq < lowest)
		freq = lowest;
	if (freq > highest)
		freq = highest;
	*adj = step;

	/* Reports the frequency to request. */
	return (uint32_t)freq;
}

/*
 * Computes the PM interrupt mask for a frequency: the down events are
 * wanted above the minimum and the up events below the maximum, among the
 * events RPS follows.
 */
uint32_t
drv_i915_rps_pm_mask(
	const struct i915_rps *rps,
	uint32_t freq)
{
	uint32_t wanted;

	/* Down events are wanted above the minimum. */
	wanted = 0U;
	if (freq > rps->min_softlimit)
		wanted |= I915_RPS_UP_EI_EXPIRED | I915_RPS_DOWN_THRESHOLD | I915_RPS_DOWN_TIMEOUT;

	/* Up events below the maximum. */
	if (freq < rps->max_softlimit)
		wanted |= I915_RPS_UP_EI_EXPIRED | I915_RPS_UP_THRESHOLD;

	/* Reports the mask: every event not wanted, or not followed, stays masked. */
	wanted &= rps->pm_events;
	return ~wanted;
}

/*
 * Computes the interrupt limits for a frequency: the maximum always, the
 * minimum only once the frequency reached it, which avoids a missed down
 * interrupt when the GT leaves RC6 at the minimal clock.
 */
uint32_t
drv_i915_rps_limits(
	const struct i915_rps *rps,
	uint32_t freq)
{
	uint32_t limits;

	/* The maximum in [31:23]. */
	limits = rps->max_softlimit << 23;

	/* The minimum in [22:14], at the minimum only. */
	if (freq <= rps->min_softlimit)
		limits |= rps->min_softlimit << 14;

	/* Reports the limits. */
	return limits;
}

/*
 * Converts nanoseconds to the PM evaluation units: sixteen command
 * streamer clocks, rounded up.
 */
uint32_t
drv_i915_rps_pm_interval(
	uint32_t clock_frequency,
	uint64_t ns)
{
	uint64_t clocks;
	uint64_t units;

	/* The clocks in the time, rounded up. */
	clocks = ((uint64_t)clock_frequency * ns + I915_RPS_NS_PER_SECOND - 1U) / I915_RPS_NS_PER_SECOND;

	/* Reports them in sixteens, rounded up. */
	units = (clocks + 15U) / 16U;
	return (uint32_t)units;
}

/* Writes the PM interrupt mask as last computed; the PM half is the upper one of GPM_WGBOXPERF. */
static void
i915_rps_write_imr(
	struct i915_rps *rps)
{
	/* The raw accessor, as the interrupt handler that also calls this uses. */
	drv_i915_raw_write32(rps->mmio, I915_GT_GEN11_GPM_WGBOXPERF_INTR_MASK, rps->pm_imr << 16);
}

/* Writes the PM interrupt enable as last computed, in the upper half of GPM_WGBOXPERF. */
static void
i915_rps_write_ier(
	struct i915_rps *rps)
{
	/* The raw accessor, as for the mask. */
	drv_i915_raw_write32(rps->mmio, I915_GT_GEN11_GPM_WGBOXPERF_INTR_ENABLE, rps->pm_ier << 16);
}

/*
 * Masks the events that open leaves out and unmasks the rest of them,
 * writing the mask only when it changed.  The caller holds the RPS lock.
 */
static void
i915_rps_mask_events_locked(
	struct i915_rps *rps,
	uint32_t events,
	uint32_t open)
{
	uint32_t mask;

	/* The new mask. */
	mask = (rps->pm_imr & ~events) | (~open & events);

	/* An unchanged mask is not written again. */
	if (mask == rps->pm_imr)
		return;

	/* The new mask, written. */
	rps->pm_imr = mask;
	i915_rps_write_imr(rps);
}

/*
 * Writes the evaluation intervals and thresholds of a power mode and turns
 * RP control on.
 */
static void
i915_rps_program_mode(
	struct i915_rps *rps,
	struct i915_mmio *mmio,
	int mode)
{
	uint64_t ei_up;
	uint64_t ei_down;

	/* The same mode needs nothing written. */
	if (mode == rps->power_mode)
		return;

	/* The mode's intervals, in microseconds. */
	ei_up = i915_rps_up_ei_us[mode];
	ei_down = I915_RPS_DOWN_EI_US;

	/* The up interval and the busy time in it that goes up. */
	drv_i915_write32(mmio, I915_GT_GEN6_RP_UP_EI, drv_i915_rps_pm_interval(rps->clock_frequency, ei_up * 1000U));
	drv_i915_write32(mmio, I915_GT_GEN6_RP_UP_THRESHOLD, drv_i915_rps_pm_interval(rps->clock_frequency, ei_up * rps->up_threshold * 10U));

	/* The down interval and the busy time in it below which it goes down. */
	drv_i915_write32(mmio, I915_GT_GEN6_RP_DOWN_EI, drv_i915_rps_pm_interval(rps->clock_frequency, ei_down * 1000U));
	drv_i915_write32(mmio, I915_GT_GEN6_RP_DOWN_THRESHOLD, drv_i915_rps_pm_interval(rps->clock_frequency, ei_down * rps->down_threshold * 10U));

	/* RP control on, averaging busy time up and idle time down; no media turbo past graphics version 9. */
	drv_i915_write32(mmio, I915_GT_GEN6_RP_CONTROL,
	    I915_GT_GEN6_RP_MEDIA_HW_NORMAL_MODE |
	    I915_GT_GEN6_RP_MEDIA_IS_GFX |
	    I915_GT_GEN6_RP_ENABLE |
	    I915_GT_GEN6_RP_UP_BUSY_AVG |
	    I915_GT_GEN6_RP_DOWN_IDLE_AVG);

	/* The mode now in force. */
	rps->power_mode = mode;
}

/*
 * Returns the power mode for a new frequency.
 *
 * The lowest and the highest frequencies have their own modes.  Otherwise
 * a rising frequency moves one mode up once it passes the mode's upper
 * edge (above the efficient frequency for LOW, at RP0 for BETWEEN), and a
 * falling one moves one mode down once it passes the lower edge (at the
 * efficient frequency for BETWEEN, below the middle of RP1 and RP0 for
 * HIGH); the mode stays put in between, and before any mode is set.
 *
 * Procedure referenced: Linux v6.19 drivers/gpu/drm/i915/gt/intel_rps.c
 * (gen6_rps_set_thresholds: the mode edges).
 */
static int
i915_rps_mode_for(
	const struct i915_rps *rps,
	uint32_t freq)
{
	int rising;
	int falling;

	/* The ends of the range. */
	if (freq >= rps->max_softlimit)
		return I915_RPS_POWER_HIGH;
	if (freq <= rps->min_softlimit)
		return I915_RPS_POWER_LOW;

	/* One mode up past the upper edge, one down past the lower edge. */
	rising = freq > rps->cur_freq;
	falling = freq < rps->cur_freq;
	if (rps->power_mode == I915_RPS_POWER_LOW && rising && freq > rps->efficient_freq + 1U)
		return I915_RPS_POWER_BETWEEN;
	if (rps->power_mode == I915_RPS_POWER_BETWEEN && falling && freq <= rps->efficient_freq)
		return I915_RPS_POWER_LOW;
	if (rps->power_mode == I915_RPS_POWER_BETWEEN && rising && freq >= rps->rp0_freq)
		return I915_RPS_POWER_HIGH;
	if (rps->power_mode == I915_RPS_POWER_HIGH && falling && freq < (rps->rp1_freq + rps->rp0_freq) >> 1)
		return I915_RPS_POWER_BETWEEN;

	/* The mode stays. */
	return rps->power_mode;
}

/*
 * Requests a frequency, and with with_mode the thresholds of its power
 * mode.  The same frequency is not written again.
 */
static void
i915_rps_request(
	struct i915_rps *rps,
	struct i915_mmio *mmio,
	uint32_t freq,
	int with_mode)
{
	/* The frequency already requested. */
	if (freq == rps->last_freq)
		return;

	/* The request. */
	drv_i915_write32(mmio, I915_GT_GEN6_RPNSWREQ, I915_GT_GEN9_FREQUENCY(freq));

	/* The thresholds for it. */
	if (with_mode)
		i915_rps_program_mode(rps, mmio, i915_rps_mode_for(rps, freq));

	/* The request now in force. */
	rps->last_freq = freq;
	rps->changes++;
}

/*
 * Requests a frequency within the soft limits and keeps the interrupts
 * coming until it reaches the minimum or the maximum.  Runs in the work, or
 * in the start before the work.
 */
static void
i915_rps_move_to(
	struct i915_rps *rps,
	uint32_t freq)
{
	struct i915_mmio *mmio;

	/* Within the soft limits. */
	mmio = rps->mmio;
	if (freq < rps->min_softlimit)
		freq = rps->min_softlimit;
	if (freq > rps->max_softlimit)
		freq = rps->max_softlimit;

	/* The request and its thresholds. */
	i915_rps_request(rps, mmio, freq, 1);

	/* With the interrupts, their limits and mask follow the new frequency. */
	if (rps->use_timer == 0) {
		drv_i915_write32(mmio, I915_GT_GEN6_RP_INTERRUPT_LIMITS, drv_i915_rps_limits(rps, freq));
		drv_i915_write32(mmio, I915_GT_GEN6_PMINTRMSK, drv_i915_rps_pm_mask(rps, freq));
	}

	/* The frequency the next decision starts from. */
	rps->cur_freq = freq;
}

/*
 * Changes the frequency for the events the handler took and the waiting
 * clients, then unmasks the events again.
 */
static void
i915_rps_adjust(
	void *context)
{
	struct i915_rps *rps;
	unsigned long flags;
	uint32_t pm_iir;
	uint32_t freq;
	uint32_t before;
	int client_boost;
	int step;

	/* Takes the events and whether a client waits. */
	rps = context;
	flags = spin_lock_irqsave(&rps->lock);

	pm_iir = rps->pm_iir & rps->pm_events;
	rps->pm_iir = 0U;
	client_boost = 0;
	if (rps->waiters != 0U)
		client_boost = 1;

	spin_unlock_irqrestore(&rps->lock, flags);

	/* One more run. */
	rps->work_runs++;

	/* Moves the frequency when there is anything to move it for. */
	if (rps->mmio != NULL &&
	    (pm_iir != 0U || client_boost)) {
		before = rps->cur_freq;
		freq = drv_i915_rps_next_freq(rps, pm_iir, client_boost, &step);
		i915_rps_move_to(rps, freq);
		rps->last_adj = step;
		i915_rps_log(rps, "work", before, pm_iir | (uint32_t)client_boost << 31);
	}

	/* Lets the events come again. */
	flags = spin_lock_irqsave(&rps->lock);

	if (rps->mmio != NULL)
		i915_rps_mask_events_locked(rps, rps->pm_events, rps->pm_events);

	spin_unlock_irqrestore(&rps->lock, flags);
}

/*
 * Enables and unmasks the up and down events and registers the handler
 * that takes them (the interrupt way, graphics versions 6 to 11).
 */
static void
i915_rps_open_events(
	struct i915_rps *rps,
	struct i915_irq_dev *irq,
	struct i915_mmio *mmio)
{
	unsigned long flags;

	/* Clears what the GTPM source may have pending before any of it is enabled. */
	drv_i915_gt_pm_reset_iir(irq);
	flags = spin_lock_irqsave(&rps->lock);

	rps->pm_iir = 0U;

	spin_unlock_irqrestore(&rps->lock, flags);

	/* Registers the handler: its argument first, so the handler never runs without one. */
	irq->pm_context = rps;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	irq->pm_handler = drv_i915_rps_irq;

	/* Enables and unmasks the RPS events. */
	flags = spin_lock_irqsave(&rps->lock);

	rps->pm_ier |= rps->pm_events;
	i915_rps_write_ier(rps);
	i915_rps_mask_events_locked(rps, rps->pm_events, rps->pm_events);

	spin_unlock_irqrestore(&rps->lock, flags);

	/* Lets the events through the mask that follows the frequency. */
	drv_i915_write32(mmio, I915_GT_GEN6_PMINTRMSK, drv_i915_rps_pm_mask(rps, rps->last_freq));
}

/* Starts the busy-time evaluations from the busy time and the clock of now. */
static void
i915_rps_start_ticking(
	struct i915_rps *rps)
{
	unsigned long flags;
	uint64_t now;

	/* Without the timer there is nothing to start. */
	if (rps->timers_ready == 0)
		return;

	/* The baseline of the first evaluation. */
	now = drv_i915_perf_now();
	flags = spin_lock_irqsave(&rps->lock);

	rps->busy_seen_ns = rps->busy_total_ns;
	if (rps->busy_runs != 0U)
		rps->busy_seen_ns += now - rps->busy_since_ns;
	rps->tick_ns = now;
	rps->interval_ms = 1U;
	rps->ticking = 1;

	spin_unlock_irqrestore(&rps->lock, flags);

	/* The first evaluation. */
	i915_rps_arm_tick(rps, 1U);
}

/* Arms the next busy-time evaluation after a delay (an armed one keeps its deadline). */
static void
i915_rps_arm_tick(
	struct i915_rps *rps,
	unsigned delay_ms)
{
	/* The evaluation, on the timer queue. */
	(void)drv_i915_delayed_queue(&rps->timers, &rps->tick_work, delay_ms);
}

/*
 * Evaluates the engines' busy time since the last evaluation: busier than
 * the up threshold goes up, idler than the down threshold goes down, as
 * the up and down events would.  After
 * a change the next evaluation comes in 1 ms, otherwise the interval
 * doubles up to 20 ms.  The first evaluation after an unpark starts from
 * the efficient frequency; an idle GT at the minimum with no waiting
 * client parks the evaluations until the engines run again.  Runs on the
 * work's queue, so it never races the work.
 */
static void
i915_rps_tick(
	void *context)
{
	struct i915_rps *rps;
	unsigned long flags;
	uint64_t now;
	uint64_t busy;
	uint64_t busy_delta;
	uint64_t elapsed;
	uint32_t events;
	uint32_t before;
	uint32_t freq;
	unsigned waiters;
	int unparking;
	int parked;
	int step;

	/* The busy time and the clock now, against the last evaluation. */
	rps = context;
	now = drv_i915_perf_now();
	flags = spin_lock_irqsave(&rps->lock);

	busy = rps->busy_total_ns;
	if (rps->busy_runs != 0U && now > rps->busy_since_ns)
		busy += now - rps->busy_since_ns;
	waiters = rps->waiters;
	unparking = rps->unparking;
	rps->unparking = 0;

	spin_unlock_irqrestore(&rps->lock, flags);

	/* What changed since the last evaluation. */
	busy_delta = 0U;
	if (busy > rps->busy_seen_ns)
		busy_delta = busy - rps->busy_seen_ns;
	elapsed = 0U;
	if (now > rps->tick_ns)
		elapsed = now - rps->tick_ns;
	rps->busy_seen_ns = busy;
	rps->tick_ns = now;
	rps->ticks++;

	/* Nothing to change once stopped. */
	if (rps->mmio == NULL)
		return;

	/* An unpark starts the GT from the efficient frequency, or higher. */
	if (unparking) {
		before = rps->cur_freq;
		freq = rps->cur_freq;
		if (freq < rps->efficient_freq)
			freq = rps->efficient_freq;
		i915_rps_move_to(rps, freq);
		rps->last_adj = 0;
		rps->interval_ms = 1U;
		i915_rps_log(rps, "unpark", before, 0U);
		i915_rps_arm_tick(rps, rps->interval_ms);
		return;
	}

	/* Busier than the up threshold, idler than the down one, or neither. */
	events = 0U;
	if (elapsed != 0U &&
	    100U * busy_delta > (uint64_t)rps->up_threshold * elapsed &&
	    rps->cur_freq < rps->max_softlimit) {
		events = I915_RPS_UP_THRESHOLD;
		rps->up_events++;
	} else if (elapsed != 0U &&
	    100U * busy_delta < (uint64_t)rps->down_threshold * elapsed &&
	    rps->cur_freq > rps->min_softlimit) {
		events = I915_RPS_DOWN_THRESHOLD;
		rps->down_events++;
	}

	/* An event moves the frequency and looks again soon; none resets the step. */
	if (events != 0U) {
		before = rps->cur_freq;
		freq = drv_i915_rps_next_freq(rps, events, waiters != 0U, &step);
		i915_rps_move_to(rps, freq);
		rps->last_adj = step;
		rps->interval_ms = 1U;
		i915_rps_log(rps, "busy", before, events);
	} else {
		rps->last_adj = 0;
	}

	/* An idle GT at the minimum with no waiter parks until the engines run again. */
	parked = 0;
	if (busy_delta == 0U &&
	    waiters == 0U &&
	    rps->cur_freq <= rps->min_softlimit) {
		flags = spin_lock_irqsave(&rps->lock);

		if (rps->busy_runs == 0U) {
			rps->ticking = 0;
			parked = 1;
		}

		spin_unlock_irqrestore(&rps->lock, flags);
	}

	/* Parked: no more evaluations until a run starts. */
	if (parked) {
		rps->parks++;
		return;
	}

	/* The next evaluation, and the interval after it doubles up to the longest. */
	i915_rps_arm_tick(rps, rps->interval_ms);
	rps->interval_ms *= 2U;
	if (rps->interval_ms > I915_RPS_BUSY_MAX_EI_MS)
		rps->interval_ms = I915_RPS_BUSY_MAX_EI_MS;
}

/* Writes a table of RC6 register writes in order. */
static void
i915_rc6_write_all(
	struct i915_mmio *mmio,
	const struct i915_rc6_write *writes,
	unsigned count)
{
	unsigned index;

	/* Each write, in the table's order. */
	for (index = 0U; index < count; index++)
		drv_i915_write32(mmio, writes[index].reg, writes[index].value);
}

/* Reports one frequency change in the log, for the first few after the start. */
static void
i915_rps_log(
	struct i915_rps *rps,
	const char *reason,
	uint32_t from,
	uint32_t events)
{
	/* Only a change, and only the first few. */
	if (from == rps->cur_freq)
		return;
	if (rps->logged >= I915_RPS_LOG_CHANGES)
		return;

	/* The line. */
	rps->logged++;
	kern_logf("i915: rps: %s events=0x%x freq %u -> %u (up=%u down=%u boosts=%u)\n",
	    reason,
	    events,
	    from,
	    rps->cur_freq,
	    rps->up_events,
	    rps->down_events,
	    rps->boosts);
}
