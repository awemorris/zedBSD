/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The eDP first stage on the register model, in the kernel build.
 *
 * The production eDP code (dp-sink.c with the Linux text of aux.c, panel.c
 * and edid-read.c) runs against the DP register model, with the target's
 * captured DPCD and EDID as the sink: PPS and VDD ownership, the AUX retries,
 * the I2C-over-AUX EDID read and every fault the model injects.  The first
 * modeset slice (LCD-A) is then computed from the same captured panel data
 * and compared with the values Linux programmed on the target.  The fuller
 * matrix (every fault, under ASan and UBSan) is the host test; this part
 * proves the kernel build of the same code behaves the same.
 *
 * The part also lends the DP world to the part that runs the eDP on real
 * threads (edp-sync-ktest.c); see edp-ktest.h.
 */

#include "edp-ktest.h"
#include "display-ktest.h"
#include <kern/kcrt.h>

#include "../execution/ktest.h"
#include "../../i915.h"
#include "../../display/dp-internal.h"
#include "../../display/dp-sink.h"
#include "../../display/state.h"
#include "dp-fake-hw.h"
#include "dp-fixture-latitude5330.h"

#include <kern/clock.h>
#include <kern/kmem.h>

#include <uapi/errno.h>
#include <stdint.h>

/* The PP_CONTROL bit that forces VDD on. */
#define I915_EDP_KTEST_VDD_FORCE 8u

/* How many bytes of the receiver capabilities, the eDP capabilities and the EDID are compared. */
#define I915_EDP_KTEST_DPCD_BYTES 15u
#define I915_EDP_KTEST_EDP_DPCD_BYTES 3u
#define I915_EDP_KTEST_EDID_BYTES 128u

/* How long a borrowing waits for the resident panel's delayed VDD-off beyond its reservation. */
#define I915_EDP_KTEST_QUIESCE_SLACK_MS 2000u

/* How long a borrowing sleeps between two looks at the resident panel's delayed VDD-off. */
#define I915_EDP_KTEST_QUIESCE_STEP_MS 50u

/*
 * The register model of the part.
 *
 * Each case starts it afresh (i915_edp_ktest_fresh()); only this part's
 * thread touches it.
 */
static struct i915_dp_fake_hw i915_edp_ktest_hw;

/*
 * The DP environment bound to the register model.
 *
 * Bound afresh with the model; the eDP keeps a pointer to it while live.
 */
static struct i915_dp_env i915_edp_ktest_env;

/*
 * The result of the current case's bring-up.
 *
 * Filled by each begin and read by the checks that follow it.
 */
static struct i915_edp_result i915_edp_ktest_res;

/*
 * The first modeset slice computed from the captured panel data.
 *
 * Filled by the LCD-A cases; too large for the stack.
 */
static struct i915_lcd_state i915_edp_ktest_lcd;

/*
 * The transcoder, CPU transcoder, plane and DDI words of the LCD-A cases.
 *
 * Each is filled by its own case and only read by it; too large for the
 * stack.
 */
static struct i915_lcd_words i915_edp_ktest_words;
static struct i915_lcd_words i915_edp_ktest_cpu_words;
static struct i915_lcd_words i915_edp_ktest_plane_words;
static struct i915_lcd_words i915_edp_ktest_ddi_words;

static int i915_edp_ktest_quiesce(struct i915_display *display);
static int i915_edp_ktest_vdd_off_busy(struct i915_edp_device *dev);
static void i915_edp_ktest_target_cfg(struct i915_edp_config *cfg);
static void i915_edp_ktest_fresh(struct i915_dp_world *world);
static int i915_edp_ktest_released(void);
static int i915_edp_ktest_captured(void);
static void i915_edp_ktest_acquire(struct i915_ktest *ktest, struct i915_dp_world *world, const struct i915_edp_config *cfg);
static void i915_edp_ktest_worker(struct i915_ktest *ktest, struct i915_dp_world *world, const struct i915_edp_config *cfg);
static void i915_edp_ktest_end_early(struct i915_ktest *ktest, struct i915_dp_world *world, const struct i915_edp_config *cfg);
static void i915_edp_ktest_no_sink(struct i915_ktest *ktest, struct i915_dp_world *world, const struct i915_edp_config *cfg);
static void i915_edp_ktest_retry(struct i915_ktest *ktest, struct i915_dp_world *world, const struct i915_edp_config *cfg);
static void i915_edp_ktest_i2c(struct i915_ktest *ktest, struct i915_dp_world *world, const struct i915_edp_config *cfg);
static void i915_edp_ktest_edid_bad(struct i915_ktest *ktest, struct i915_dp_world *world, const struct i915_edp_config *cfg);
static void i915_edp_ktest_edid_ext(struct i915_ktest *ktest, struct i915_dp_world *world, const struct i915_edp_config *cfg);
static void i915_edp_ktest_power_fail(struct i915_ktest *ktest, struct i915_dp_world *world, const struct i915_edp_config *cfg);
static void i915_edp_ktest_lcd_a(struct i915_ktest *ktest);
static void i915_edp_ktest_lcd_a_values(struct i915_ktest *ktest, struct i915_lcd_world *lcd_world);
static void i915_edp_ktest_lcd_a_transcoder(struct i915_ktest *ktest, struct i915_lcd_world *lcd_world);
static void i915_edp_ktest_lcd_a_cpu_transcoder(struct i915_ktest *ktest, struct i915_lcd_world *lcd_world);
static void i915_edp_ktest_lcd_a_plane(struct i915_ktest *ktest, struct i915_lcd_world *lcd_world);
static void i915_edp_ktest_lcd_a_ddi(struct i915_ktest *ktest, struct i915_lcd_world *lcd_world);
static void i915_edp_ktest_lcd_a_fit(struct i915_ktest *ktest, struct i915_lcd_world *lcd_world);
static void i915_edp_ktest_config(struct i915_ktest *ktest, struct i915_dp_world *world);

/*
 * Checks the eDP first stage on the register model: PPS and VDD ownership,
 * AUX, DPCD and EDID, then the first modeset slice from the same panel data.
 *
 * The DP world of the started device is borrowed for the part and given
 * back unchanged; the register model never reaches the hardware.
 */
void
drv_i915_display_ktest_edp(
	struct i915_ktest *ktest)
{
	struct i915_edp_ktest_world borrow;
	struct i915_edp_config cfg;
	int error;

	/* Borrows the DP world; without it no case can run. */
	error = drv_i915_display_ktest_edp_world_enter(ktest, &borrow);
	if (error != 0) {
		drv_i915_ktest_skip(ktest, "edp: the eDP first stage on the register model",
				    "the DP world could not be borrowed: the resident panel's VDD-off did not settle, or no memory");
		return;
	}

	/* The target's configuration: the delays of its VBT. */
	i915_edp_ktest_target_cfg(&cfg);

	/* Brings the eDP up, lets the delayed VDD-off run and ends it. */
	i915_edp_ktest_acquire(ktest, borrow.world, &cfg);
	i915_edp_ktest_worker(ktest, borrow.world, &cfg);

	/* Ends right after the acquisition. */
	i915_edp_ktest_end_early(ktest, borrow.world, &cfg);

	/* The faults of the sink, the AUX channel, the EDID and the power layer. */
	i915_edp_ktest_no_sink(ktest, borrow.world, &cfg);
	i915_edp_ktest_retry(ktest, borrow.world, &cfg);
	i915_edp_ktest_i2c(ktest, borrow.world, &cfg);
	i915_edp_ktest_edid_bad(ktest, borrow.world, &cfg);
	i915_edp_ktest_edid_ext(ktest, borrow.world, &cfg);
	i915_edp_ktest_power_fail(ktest, borrow.world, &cfg);

	/* The first modeset slice from the same captured panel data. */
	i915_edp_ktest_lcd_a(ktest);

	/* A configuration the eDP stage refuses. */
	i915_edp_ktest_config(ktest, borrow.world);

	/* Gives the DP world back. */
	drv_i915_display_ktest_edp_world_leave(&borrow);
}

/*
 * Borrows the DP world for a ktest part.
 *
 * The device's world is emptied once its resident panel's delayed VDD-off
 * has run, and its contents are kept aside for the leave; without a display
 * world one is created.  Returns 0, EBUSY when the resident panel's VDD-off
 * did not settle, or ENOMEM.
 */
int
drv_i915_display_ktest_edp_world_enter(
	struct i915_ktest *ktest,
	struct i915_edp_ktest_world *borrow)
{
	struct i915_display *display;
	struct i915_display *carrier;
	struct i915_dp_world *saved;
	int error;

	/* Starts with nothing borrowed. */
	kern_memset(borrow, 0, sizeof(*borrow));
	display = NULL;
	if (ktest->device != NULL)
		display = ktest->device->display;

	/* Allocates the test's own display that carries the world. */
	carrier = kern_calloc(1U, sizeof(*carrier));
	if (carrier == NULL)
		return ENOMEM;

	/* Without a display world, creates one on the carrier; else empties the device's world. */
	if (display == NULL || display->dp_world == NULL) {
		error = drv_i915_dp_world_create(carrier);
		if (error != 0) {
			kern_free(carrier);
			return error;
		}

		borrow->world = carrier->dp_world;
		borrow->created = 1;
	} else {
		/* Waits until the resident panel's delayed VDD-off has run. */
		error = i915_edp_ktest_quiesce(display);
		if (error != 0) {
			kern_free(carrier);
			return error;
		}

		/* Allocates the place the world's contents are kept in. */
		saved = kern_malloc(sizeof(*saved));
		if (saved == NULL) {
			kern_free(carrier);
			return ENOMEM;
		}

		/*
		 * Sets the resident panel's world aside and empties it: the Linux
		 * text keeps reaching the same world, which now has no live eDP.
		 */
		kern_memcpy(saved, display->dp_world, sizeof(*saved));
		kern_memset(display->dp_world, 0, sizeof(*display->dp_world));
		carrier->dp_world = display->dp_world;
		borrow->world = display->dp_world;
		borrow->saved = saved;
	}

	/* The carrier lives until the leave. */
	borrow->carrier = carrier;

	/* Succeeded: the part may bring an eDP up in the world. */
	return 0;
}

/*
 * Gives the borrowed DP world back.
 *
 * A created world is destroyed; a device's world gets its contents back.
 * The part must have ended every eDP it brought up.
 */
void
drv_i915_display_ktest_edp_world_leave(
	struct i915_edp_ktest_world *borrow)
{
	/* Nothing was borrowed. */
	if (borrow->carrier == NULL)
		return;

	/* Destroys a world created for the part, or puts the device's world back. */
	if (borrow->created) {
		drv_i915_dp_world_destroy(borrow->carrier);
	} else {
		kern_memcpy(borrow->world, borrow->saved, sizeof(*borrow->world));
		kern_free(borrow->saved);
	}

	/* Frees the carrier. */
	kern_free(borrow->carrier);
	kern_memset(borrow, 0, sizeof(*borrow));
}

/*
 * Waits until the resident panel's delayed VDD-off is neither reserved nor
 * running, at most five power-cycle delays and a slack; reports 0 or EBUSY.
 */
static int
i915_edp_ktest_quiesce(
	struct i915_display *display)
{
	struct i915_edp_device *dev;
	unsigned limit_ms;
	unsigned waited_ms;
	int busy;

	/* A panel without its threads has nothing reserved. */
	dev = &display->edp_dev;
	if (!dev->started)
		return 0;

	/* Looks until the work is idle or the limit has passed. */
	limit_ms = (unsigned)dev->res.delay_power_cycle_ms * 5u + I915_EDP_KTEST_QUIESCE_SLACK_MS;
	waited_ms = 0u;
	busy = i915_edp_ktest_vdd_off_busy(dev);
	while (busy) {
		if (waited_ms >= limit_ms)
			return EBUSY;

		kern_usleep_range(I915_EDP_KTEST_QUIESCE_STEP_MS * 1000u, I915_EDP_KTEST_QUIESCE_STEP_MS * 1000u);
		waited_ms += I915_EDP_KTEST_QUIESCE_STEP_MS;
		busy = i915_edp_ktest_vdd_off_busy(dev);
	}

	/* Succeeded: nothing of the resident panel runs behind the part. */
	return 0;
}

/* Reports whether the resident panel's delayed VDD-off is reserved, queued or running. */
static int
i915_edp_ktest_vdd_off_busy(
	struct i915_edp_device *dev)
{
	int pending;

	/* Reserved on the timer or waiting in the queue. */
	pending = drv_i915_delayed_pending(&dev->k.tq, &dev->k.vdd_off_work);
	if (pending)
		return 1;

	/* Running on the worker. */
	if (dev->k.vdd_off_work.work.state == I915_WORK_RUNNING)
		return 1;

	/* Succeeded: the work is idle. */
	return 0;
}

/* Fills the target's eDP configuration: the delays of its VBT, messages off. */
static void
i915_edp_ktest_target_cfg(
	struct i915_edp_config *cfg)
{
	/* Starts from port A, AUX A and no controller. */
	kern_memset(cfg, 0, sizeof(*cfg));

	/* The raw clock the target reported. */
	cfg->rawclk_khz = 19200u;

	/* The VBT power sequence, in 100 us units. */
	cfg->t1_t3 = 2000u;
	cfg->t8 = 800u;
	cfg->t9 = 2000u;
	cfg->t10 = 1100u;
	cfg->t11_t12 = 5000u;

	/* No message is shown. */
	cfg->log_level = -1;
}

/* Starts the register model afresh with the captured panel and binds the environment to it. */
static void
i915_edp_ktest_fresh(
	struct i915_dp_world *world)
{
	/* The sink answers with the captured DPCD and EDID. */
	drv_i915_dp_fake_init(&i915_edp_ktest_hw, i915_dp_fixture_dpcd_000, i915_dp_fixture_dpcd_100, i915_dp_fixture_dpcd_700,
			      i915_dp_fixture_edid, sizeof(i915_dp_fixture_edid));

	/* Every environment hook goes to the model, which runs the due work in the world. */
	drv_i915_dp_fake_bind_env(&i915_edp_ktest_hw, &i915_edp_ktest_env, world);
}

/*
 * Reports whether the ended eDP released everything: no power reference,
 * VDD off, no wakeref, no pending work, no lock held, no lock or put error.
 *
 * The references put asynchronously belong to the power layer until it is
 * flushed, so the model's own counts are compared after the flush.
 */
static int
i915_edp_ktest_released(void)
{
	struct i915_dp_fake_hw *hw;
	int owned;

	/* Looks for anything the DP layer still owns. */
	hw = &i915_edp_ktest_hw;
	owned = 0;
	if (i915_edp_ktest_env.power_refs[0] != 0) {
		owned = 1;
	} else if (i915_edp_ktest_env.power_refs[1] != 0) {
		owned = 1;
	} else if ((hw->pp_control & I915_EDP_KTEST_VDD_FORCE) != 0u) {
		owned = 1;
	} else if (i915_edp_ktest_res.vdd_wakeref_held != 0) {
		owned = 1;
	} else if (i915_edp_ktest_res.vdd_work_pending != 0) {
		owned = 1;
	} else if (i915_edp_ktest_res.power_put_underflows != 0u) {
		owned = 1;
	} else if (hw->lock_held[0] != 0) {
		owned = 1;
	} else if (hw->lock_held[1] != 0) {
		owned = 1;
	} else if (hw->lock_errors != 0u) {
		owned = 1;
	} else if (i915_edp_ktest_env.lock_errors != 0u) {
		owned = 1;
	}

	/* Releases the parked references as the power layer would. */
	drv_i915_dp_fake_flush_async(hw);

	/* Something was still owned. */
	if (owned)
		return 0;

	/* The hardware side must hold no reference either. */
	if (hw->refs_core != 0)
		return 0;
	if (hw->refs_aux != 0)
		return 0;

	/* Succeeded: everything was released. */
	return 1;
}

/* Reports whether the result holds the captured DPCD caps, eDP caps and the one-block EDID. */
static int
i915_edp_ktest_captured(void)
{
	struct i915_edp_result *res;
	int compared;

	/* The receiver capabilities. */
	res = &i915_edp_ktest_res;
	if (!res->dpcd_ok)
		return 0;

	compared = kern_memcmp(res->dpcd, i915_dp_fixture_dpcd_000, I915_EDP_KTEST_DPCD_BYTES);
	if (compared != 0)
		return 0;

	/* The eDP display control capabilities. */
	if (!res->edp_dpcd_ok)
		return 0;

	compared = kern_memcmp(res->edp_dpcd, i915_dp_fixture_dpcd_700, I915_EDP_KTEST_EDP_DPCD_BYTES);
	if (compared != 0)
		return 0;

	/* The EDID: one whole block. */
	if (!res->edid_ok)
		return 0;
	if (res->edid_blocks != 1u)
		return 0;

	compared = kern_memcmp(res->edid, i915_dp_fixture_edid, I915_EDP_KTEST_EDID_BYTES);
	if (compared != 0)
		return 0;

	/* Succeeded: everything equals the captured panel. */
	return 1;
}

/* Checks the acquisition: the captured data, the target's delays and the ownership of VDD and power. */
static void
i915_edp_ktest_acquire(
	struct i915_ktest *ktest,
	struct i915_dp_world *world,
	const struct i915_edp_config *cfg)
{
	struct i915_dp_fake_hw *hw;
	struct i915_edp_result *res;
	int captured;
	int passed;
	int rc;

	/* Brings the eDP up on a fresh model. */
	hw = &i915_edp_ktest_hw;
	res = &i915_edp_ktest_res;
	i915_edp_ktest_fresh(world);
	rc = drv_i915_edp_begin(world, &i915_edp_ktest_env, cfg, res);

	/* PPS 0 was picked and the DPCD caps, eDP caps and EDID equal the captured panel data. */
	passed = 0;
	if (rc == 0 &&
	    res->stage == I915_EDP_STAGE_ACQUIRED &&
	    res->pps_valid == 1 &&
	    res->pps_idx == 0) {
		captured = i915_edp_ktest_captured();
		if (captured)
			passed = 1;
	}

	drv_i915_ktest_check(ktest, passed,
			     "edp: EDP-ACQUIRE PPS 0, DPCD caps, eDP caps and the 128-byte EDID equal the captured panel data");

	/* The delay registers and the software delays are the target's Linux values. */
	passed = 0;
	if (res->after_init.pp_on_delays == 0x07d00001u &&
	    res->after_init.pp_off_delays == 0x044c0001u &&
	    ((res->after_init.pp_control >> 4) & 0x1fu) == 6u &&
	    res->delay_power_up_ms == 200 &&
	    res->delay_power_down_ms == 110 &&
	    res->delay_power_cycle_ms == 600 &&
	    res->delay_bl_on_ms == 80 &&
	    res->delay_bl_off_ms == 200)
		passed = 1;

	drv_i915_ktest_check(ktest, passed,
			     "edp: EDP-DELAYS PP_ON/OFF 0x07d00001/0x044c0001, cycle field 6, software delays 200/110/600/80/200 ms (the target's Linux values)");

	/* VDD went on once with its AUX reference, every transfer was powered, nothing stray was touched. */
	passed = 0;
	if (res->vdd_on_hw == 1 &&
	    res->vdd_wakeref_held == 1 &&
	    res->power_refs_aux == 1 &&
	    res->power_refs_core == 0 &&
	    hw->vdd_on_events == 1u &&
	    hw->aux_without_sink_power == 0u &&
	    hw->aux_without_aux_power == 0u &&
	    hw->aux_without_core_power == 0u &&
	    hw->pp_writes_without_core_power == 0u &&
	    hw->unknown_reg_reads == 0u &&
	    hw->unknown_reg_writes == 0u &&
	    i915_edp_ktest_env.slept_us >= 200000u &&
	    res->log_errors == 0u)
		passed = 1;

	drv_i915_ktest_check(ktest, passed,
			     "edp: EDP-OWNERSHIP VDD on once + its AUX reference, every transfer powered, 200 ms power-up wait, no stray register");
}

/* Checks the late step's delayed VDD-off: reserved, not early, run when due, and the end after it. */
static void
i915_edp_ktest_worker(
	struct i915_ktest *ktest,
	struct i915_dp_world *world,
	const struct i915_edp_config *cfg)
{
	struct i915_dp_fake_hw *hw;
	struct i915_edp_result *res;
	unsigned ran;
	int released;
	int passed;
	int again;
	int end;
	int rc;

	/* The late step reserves the VDD-off; at the model's current time it is not due. */
	hw = &i915_edp_ktest_hw;
	res = &i915_edp_ktest_res;
	rc = drv_i915_edp_init_late(world, cfg, res);
	passed = 0;
	if (rc == 0 && res->vdd_work_pending == 1) {
		ran = drv_i915_dp_fake_run_due(hw);
		if (ran == 0u && (hw->pp_control & I915_EDP_KTEST_VDD_FORCE) != 0u)
			passed = 1;
	}

	drv_i915_ktest_check(ktest, passed,
			     "edp: EDP-WORKER late init schedules the VDD-off worker; it does not run before 5 x the power-cycle delay");

	/* Past five power-cycle delays the worker runs, forces VDD off and returns the reference. */
	hw->now_us += 3001u * 1000u;
	ran = drv_i915_dp_fake_run_due(hw);
	drv_i915_edp_snapshot(world, res);
	passed = 0;
	if (ran == 1u &&
	    res->vdd_on_hw == 0 &&
	    res->vdd_wakeref_held == 0 &&
	    res->power_refs_aux == 0)
		passed = 1;

	drv_i915_ktest_check(ktest, passed, "edp: EDP-WORKER-DUE the due worker forces VDD off and returns the AUX reference");

	/* The end releases everything, and a second end is refused. */
	end = drv_i915_edp_end(world, res);
	passed = 0;
	if (end == 0) {
		released = i915_edp_ktest_released();
		if (released) {
			again = drv_i915_edp_end(world, res);
			if (again != 0)
				passed = 1;
		}
	}

	drv_i915_ktest_check(ktest, passed, "edp: EDP-END everything released; a second end is refused");
}

/* Checks an end right after the acquisition. */
static void
i915_edp_ktest_end_early(
	struct i915_ktest *ktest,
	struct i915_dp_world *world,
	const struct i915_edp_config *cfg)
{
	int released;
	int passed;
	int end;
	int rc;

	/* Brings the eDP up and ends it at once. */
	i915_edp_ktest_fresh(world);
	rc = drv_i915_edp_begin(world, &i915_edp_ktest_env, cfg, &i915_edp_ktest_res);
	end = drv_i915_edp_end(world, &i915_edp_ktest_res);

	/* Nothing was pending to cancel, VDD was forced off once and everything released. */
	passed = 0;
	if (rc == 0 && end == 0) {
		released = i915_edp_ktest_released();
		if (released && i915_edp_ktest_hw.vdd_off_events == 1u)
			passed = 1;
	}

	drv_i915_ktest_check(ktest, passed,
			     "edp: EDP-END-EARLY end right after the acquisition cancels nothing pending and forces VDD off");
}

/* Checks a sink that never answers. */
static void
i915_edp_ktest_no_sink(
	struct i915_ktest *ktest,
	struct i915_dp_world *world,
	const struct i915_edp_config *cfg)
{
	int released;
	int passed;
	int end;
	int rc;

	/* The sink needs an hour of VDD before it answers. */
	i915_edp_ktest_fresh(world);
	i915_edp_ktest_hw.sink_power_up_us = 3600u * 1000000u;
	rc = drv_i915_edp_begin(world, &i915_edp_ktest_env, cfg, &i915_edp_ktest_res);
	end = drv_i915_edp_end(world, &i915_edp_ktest_res);

	/* The DPCD stage times out after 32 x 5 finite tries and everything is released. */
	passed = 0;
	if (rc == -I915_EDP_ETIMEDOUT &&
	    i915_edp_ktest_res.failed_stage == I915_EDP_STAGE_DPCD &&
	    end == 0) {
		released = i915_edp_ktest_released();
		if (released && i915_edp_ktest_hw.aux_transactions == 160u)
			passed = 1;
	}

	drv_i915_ktest_check(ktest, passed,
			     "edp: EDP-NOSINK no answer -> -ETIMEDOUT after 32 x 5 finite tries, VDD and references released");
}

/* Checks that every native AUX fault is retried and never taken as data. */
static void
i915_edp_ktest_retry(
	struct i915_ktest *ktest,
	struct i915_dp_world *world,
	const struct i915_edp_config *cfg)
{
	uint8_t faults[I915_DP_FAKE_SCRIPT_MAX];
	uint8_t params[I915_DP_FAKE_SCRIPT_MAX];
	int dpcd_compared;
	int edid_compared;
	int released;
	int passed;
	int end;
	int rc;

	/*
	 * Scripts the first transactions: DEFER, NACK, a receive error, the
	 * forbidden sizes 21 and 0, a reserved reply and a 5-byte short read.
	 */
	i915_edp_ktest_fresh(world);
	kern_memset(params, 0, sizeof(params));
	kern_memset(faults, I915_DP_FAKE_OK, sizeof(faults));
	faults[0] = I915_DP_FAKE_NATIVE_DEFER;
	faults[1] = I915_DP_FAKE_NATIVE_NACK;
	faults[2] = I915_DP_FAKE_RECEIVE_ERROR;
	faults[3] = I915_DP_FAKE_BAD_SIZE_BIG;
	faults[4] = I915_DP_FAKE_INVALID_REPLY;
	faults[5] = I915_DP_FAKE_BAD_SIZE_ZERO;
	faults[7] = I915_DP_FAKE_SHORT_REPLY;
	params[7] = 5u;
	drv_i915_dp_fake_script(&i915_edp_ktest_hw, 8u, faults, params);

	/* Brings the eDP up through the faults and ends it. */
	rc = drv_i915_edp_begin(world, &i915_edp_ktest_env, cfg, &i915_edp_ktest_res);
	end = drv_i915_edp_end(world, &i915_edp_ktest_res);

	/* The data read equals the captured panel and everything is released. */
	dpcd_compared = kern_memcmp(i915_edp_ktest_res.dpcd, i915_dp_fixture_dpcd_000, I915_EDP_KTEST_DPCD_BYTES);
	edid_compared = kern_memcmp(i915_edp_ktest_res.edid, i915_dp_fixture_edid, I915_EDP_KTEST_EDID_BYTES);
	passed = 0;
	if (rc == 0 &&
	    dpcd_compared == 0 &&
	    edid_compared == 0 &&
	    end == 0) {
		released = i915_edp_ktest_released();
		if (released)
			passed = 1;
	}

	drv_i915_ktest_check(ktest, passed,
			     "edp: EDP-RETRY DEFER, NACK, receive error, forbidden sizes 21/0, reserved reply and a short read are retried, never taken as data");
}

/* Checks the I2C DEFERs and a partial I2C read of the EDID. */
static void
i915_edp_ktest_i2c(
	struct i915_ktest *ktest,
	struct i915_dp_world *world,
	const struct i915_edp_config *cfg)
{
	uint8_t faults[I915_DP_FAKE_SCRIPT_MAX];
	uint8_t params[I915_DP_FAKE_SCRIPT_MAX];
	int edid_compared;
	int released;
	int passed;
	int end;
	int rc;

	/*
	 * Scripts the first I2C transaction to DEFER twice, and the first
	 * 16-byte EDID read to answer with 4 bytes.
	 */
	i915_edp_ktest_fresh(world);
	kern_memset(faults, I915_DP_FAKE_OK, sizeof(faults));
	kern_memset(params, 0, sizeof(params));
	faults[6] = I915_DP_FAKE_I2C_DEFER;
	faults[7] = I915_DP_FAKE_I2C_DEFER;
	faults[11] = I915_DP_FAKE_SHORT_REPLY;
	params[11] = 4u;
	drv_i915_dp_fake_script(&i915_edp_ktest_hw, 12u, faults, params);

	/* Brings the eDP up through the faults and ends it. */
	rc = drv_i915_edp_begin(world, &i915_edp_ktest_env, cfg, &i915_edp_ktest_res);
	end = drv_i915_edp_end(world, &i915_edp_ktest_res);

	/* Both DEFERs were retried, the EDID has no gap or repeat and everything is released. */
	edid_compared = kern_memcmp(i915_edp_ktest_res.edid, i915_dp_fixture_edid, I915_EDP_KTEST_EDID_BYTES);
	passed = 0;
	if (rc == 0 &&
	    i915_edp_ktest_res.i2c_defers == 2u &&
	    edid_compared == 0 &&
	    end == 0) {
		released = i915_edp_ktest_released();
		if (released)
			passed = 1;
	}

	drv_i915_ktest_check(ktest, passed,
			     "edp: EDP-I2C I2C DEFERs are retried and a partial I2C read continues without gap or repeat");
}

/* Checks an EDID that never checksums. */
static void
i915_edp_ktest_edid_bad(
	struct i915_ktest *ktest,
	struct i915_dp_world *world,
	const struct i915_edp_config *cfg)
{
	int released;
	int passed;
	int end;
	int rc;

	/* Corrupts every I2C read. */
	i915_edp_ktest_fresh(world);
	i915_edp_ktest_hw.fault_every_i2c_read = I915_DP_FAKE_CORRUPT_DATA;
	rc = drv_i915_edp_begin(world, &i915_edp_ktest_env, cfg, &i915_edp_ktest_res);
	end = drv_i915_edp_end(world, &i915_edp_ktest_res);

	/* The EDID stage fails with -EPROTO and everything is released. */
	passed = 0;
	if (rc == -I915_EDP_EPROTO &&
	    i915_edp_ktest_res.failed_stage == I915_EDP_STAGE_EDID &&
	    end == 0) {
		released = i915_edp_ktest_released();
		if (released)
			passed = 1;
	}

	drv_i915_ktest_check(ktest, passed,
			     "edp: EDP-EDID-BAD an EDID that never checksums is rejected (-EPROTO), VDD and references released");
}

/* Checks an EDID that announces more extensions than the buffer holds. */
static void
i915_edp_ktest_edid_ext(
	struct i915_ktest *ktest,
	struct i915_dp_world *world,
	const struct i915_edp_config *cfg)
{
	int released;
	int passed;
	int end;

	/* Announces 200 extensions, keeping the base block's checksum. */
	i915_edp_ktest_fresh(world);
	i915_edp_ktest_hw.edid[126] = 200u;
	i915_edp_ktest_hw.edid[127] = (uint8_t)(i915_edp_ktest_hw.edid[127] - 200u);
	(void)drv_i915_edp_begin(world, &i915_edp_ktest_env, cfg, &i915_edp_ktest_res);
	end = drv_i915_edp_end(world, &i915_edp_ktest_res);

	/* No more blocks than fit were written and everything is released. */
	passed = 0;
	if (i915_edp_ktest_res.edid_blocks <= I915_EDP_MAX_EDID_BLOCKS &&
	    i915_edp_ktest_res.edid_extensions == 200u &&
	    end == 0) {
		released = i915_edp_ktest_released();
		if (released)
			passed = 1;
	}

	drv_i915_ktest_check(ktest, passed, "edp: EDP-EDID-EXT an extension count beyond the buffer never writes past it");
}

/* Checks a failed power-domain get. */
static void
i915_edp_ktest_power_fail(
	struct i915_ktest *ktest,
	struct i915_dp_world *world,
	const struct i915_edp_config *cfg)
{
	int passed;
	int end;
	int rc;

	/* Every power-domain get fails. */
	i915_edp_ktest_fresh(world);
	i915_edp_ktest_hw.fail_power_get = 1;
	rc = drv_i915_edp_begin(world, &i915_edp_ktest_env, cfg, &i915_edp_ktest_res);
	end = drv_i915_edp_end(world, &i915_edp_ktest_res);

	/* The failure is reported and its put skipped, so nothing underflows. */
	passed = 0;
	if (rc < 0 &&
	    i915_edp_ktest_res.power_get_failures != 0u &&
	    i915_edp_ktest_res.power_put_underflows == 0u &&
	    end == 0)
		passed = 1;

	drv_i915_ktest_check(ktest, passed,
			     "edp: EDP-POWER-FAIL a failed power-domain get is reported and its put skipped (no underflow)");
}

/*
 * Checks the first modeset slice (LCD-A) from the captured panel data
 * against the values Linux programmed on the target.
 *
 * The calculation runs in the modeset environment's world of the started
 * display, as the resident eDP's own calculation does.
 */
static void
i915_edp_ktest_lcd_a(
	struct i915_ktest *ktest)
{
	struct i915_lcd_world *lcd_world;

	/* The calculation needs the display's modeset world. */
	lcd_world = NULL;
	if (ktest->device != NULL && ktest->device->display != NULL)
		lcd_world = ktest->device->display->lcd_world;

	if (lcd_world == NULL) {
		drv_i915_ktest_skip(ktest, "lcd: LCD-A (7 checks)", "the device has no display, so no modeset world to compute in");
		return;
	}

	/* The mode, link, M/N and PLL values, then the words of each writer. */
	i915_edp_ktest_lcd_a_values(ktest, lcd_world);
	i915_edp_ktest_lcd_a_transcoder(ktest, lcd_world);
	i915_edp_ktest_lcd_a_cpu_transcoder(ktest, lcd_world);
	i915_edp_ktest_lcd_a_plane(ktest, lcd_world);
	i915_edp_ktest_lcd_a_ddi(ktest, lcd_world);

	/* A link that cannot carry the mode. */
	i915_edp_ktest_lcd_a_fit(ktest, lcd_world);
}

/* Checks the computed mode, link, M/N and PLL words. */
static void
i915_edp_ktest_lcd_a_values(
	struct i915_ktest *ktest,
	struct i915_lcd_world *lcd_world)
{
	struct i915_lcd_state *lcd;
	int passed;
	int rc;

	/* Computes the slice from the captured EDID and DPCD, 18 bpp from the VBT, a 38.4 MHz reference. */
	lcd = &i915_edp_ktest_lcd;
	rc = drv_i915_lcd_compute(lcd_world, i915_dp_fixture_edid, i915_dp_fixture_dpcd_000, i915_dp_fixture_dpcd_700, 18, 38400, lcd);

	/* The mode: Linux's transcoder A timings, 6 bpc, HBR x2. */
	passed = 0;
	if (rc == 0 &&
	    lcd->mode.clock_khz == 140800 &&
	    lcd->mode.hdisplay == 1920u &&
	    lcd->mode.htotal == 2080u &&
	    lcd->mode.hsync_start == 1936u &&
	    lcd->mode.hsync_end == 1952u &&
	    lcd->mode.vdisplay == 1080u &&
	    lcd->mode.vtotal == 1128u &&
	    lcd->mode.vsync_start == 1083u &&
	    lcd->mode.vsync_end == 1097u &&
	    lcd->mode.edid_bpc == 6 &&
	    lcd->link.bpp == 18 &&
	    lcd->link.rate_khz == 270000 &&
	    lcd->link.lanes == 2)
		passed = 1;

	drv_i915_ktest_check(ktest, passed,
			     "lcd: LCD-A-MODE 1920x1080 140.8 MHz timings, 6 bpc / 18 bpp, HBR x2 (Linux transcoder A + DDI A values)");

	/* The link: bandwidth, M/N and the DPLL words Linux wrote. */
	passed = 0;
	if (rc == 0 &&
	    lcd->link.required_kbps == 316800 &&
	    lcd->link.available_kbps == 540000 &&
	    lcd->link.tu == 64u &&
	    lcd->link.data_m == 0x4b17e4u &&
	    lcd->link.data_n == 0x800000u &&
	    lcd->link.link_m == 273406u &&
	    lcd->link.link_n == 524288u &&
	    lcd->pll.cfgcr0 == 0x00e001a5u &&
	    lcd->pll.cfgcr1 == 0x88u &&
	    lcd->notes == 0u)
		passed = 1;

	drv_i915_ktest_check(ktest, passed,
			     "lcd: LCD-A-LINK data/link M/N and DPLL CFGCR0/1 equal the PIPE_DATA/LINK_M1/N1 and DPLL0 words Linux wrote");
}

/* Checks the transcoder, M/N and pipe-source words against Linux's register dump. */
static void
i915_edp_ktest_lcd_a_transcoder(
	struct i915_ktest *ktest,
	struct i915_lcd_world *lcd_world)
{
	struct i915_lcd_words *words;
	int passed;
	int rc;

	/* Emits the words of pipe A, transcoder A, full-screen source. */
	words = &i915_edp_ktest_words;
	rc = drv_i915_lcd_emit_transcoder(lcd_world, &i915_edp_ktest_lcd, 0, 0, 1920u, 1080u, words);

	/* The words of Linux's register dump, LINK_N written last of the M/N and PIPESRC last of all. */
	passed = 0;
	if (rc == 0 &&
	    words->n == 13u &&
	    words->w[0].reg == 0x60030u &&
	    words->w[0].value == 0x7e4b17e4u &&
	    words->w[3].reg == 0x60044u &&
	    words->w[3].value == 0x00080000u &&
	    words->w[6].reg == 0x60000u &&
	    words->w[6].value == 0x081f077fu &&
	    words->w[8].value == 0x079f078fu &&
	    words->w[9].value == 0x04670437u &&
	    words->w[10].value == 0x04670000u &&
	    words->w[11].value == 0x0448043au &&
	    words->w[12].reg == 0x6001cu &&
	    words->w[12].value == 0x077f0437u)
		passed = 1;

	drv_i915_ktest_check(ktest, passed,
			     "lcd: LCD-A-WORDS the reference's transcoder / M-N / PIPESRC writers emit the words of Linux's register dump, LINK_N last");
}

/* Checks the order of hsw_configure_cpu_transcoder()'s operations. */
static void
i915_edp_ktest_lcd_a_cpu_transcoder(
	struct i915_ktest *ktest,
	struct i915_lcd_world *lcd_world)
{
	struct i915_lcd_words *words;
	unsigned pipesrc_writes;
	int passed;
	int rc;

	/* Emits the operations of pipe A, transcoder A. */
	words = &i915_edp_ktest_cpu_words;
	rc = drv_i915_lcd_emit_cpu_transcoder(lcd_world, &i915_edp_ktest_lcd, 0, 0, words);

	/*
	 * M/N, timings, VRR, MULT, frame start delay and TRANSCONF without the
	 * enable bit, in that order, and no PIPESRC.
	 */
	passed = 0;
	if (rc == 0 &&
	    words->n == 17u &&
	    words->w[3].reg == 0x60044u &&
	    words->w[11].reg == 0x60014u &&
	    words->w[12].rmw == 1u &&
	    words->w[12].reg == 0x420c0u &&
	    words->w[12].value == 0x80000000u &&
	    words->w[13].reg == 0x60420u &&
	    words->w[14].reg == 0x6002cu &&
	    words->w[15].rmw == 1u &&
	    words->w[15].clear == 0x18000000u &&
	    words->w[16].reg == 0x70008u &&
	    words->w[16].value == 0u) {
		pipesrc_writes = drv_i915_lcd_words_find(words, 0x6001cu, NULL);
		if (pipesrc_writes == 0u)
			passed = 1;
	}

	drv_i915_ktest_check(ktest, passed,
			     "lcd: LCD-A-CPU-TRANSCODER the reference's hsw_configure_cpu_transcoder orders M/N, timings, VRR, MULT, frame start delay, TRANSCONF (no enable bit)");
}

/* Checks the plane words of a full-HD XRGB8888 linear buffer against Linux's dump. */
static void
i915_edp_ktest_lcd_a_plane(
	struct i915_ktest *ktest,
	struct i915_lcd_world *lcd_world)
{
	struct i915_lcd_words *words;
	uint32_t ctl;
	uint32_t stride;
	uint32_t size;
	uint32_t color;
	uint32_t surf;
	unsigned ctl_writes;
	unsigned stride_writes;
	unsigned size_writes;
	unsigned color_writes;
	unsigned surf_writes;
	int passed;
	int rc;

	/* Emits the words of pipe A's primary plane for a buffer at GGTT offset 0x180000. */
	words = &i915_edp_ktest_plane_words;
	rc = drv_i915_lcd_emit_plane(lcd_world, 0, 0, I915_FOURCC_XRGB8888, I915_MOD_LINEAR, 1920u, 1080u, 7680u, 0x00180000u, words);

	/* Looks the five plane registers up. */
	ctl = 0u;
	stride = 0u;
	size = 0u;
	color = 0u;
	surf = 0u;
	ctl_writes = drv_i915_lcd_words_find(words, 0x70180u, &ctl);
	stride_writes = drv_i915_lcd_words_find(words, 0x70188u, &stride);
	size_writes = drv_i915_lcd_words_find(words, 0x70190u, &size);
	color_writes = drv_i915_lcd_words_find(words, 0x701ccu, &color);
	surf_writes = drv_i915_lcd_words_find(words, 0x7019cu, &surf);

	/* Each is written once with Linux's value, PLANE_CTL just before PLANE_SURF, which is last. */
	passed = 0;
	if (rc == 0 &&
	    ctl_writes == 1u &&
	    ctl == 0x94000000u &&
	    stride_writes == 1u &&
	    stride == 0x78u &&
	    size_writes == 1u &&
	    size == 0x0437077fu &&
	    color_writes == 1u &&
	    color == 0x2000u &&
	    surf_writes == 1u &&
	    surf == 0x00180000u &&
	    words->n >= 2u) {
		if (words->w[words->n - 1u].reg == 0x7019cu &&
		    words->w[words->n - 2u].reg == 0x70180u)
			passed = 1;
	}

	drv_i915_ktest_check(ktest, passed,
			     "lcd: LCD-A-PLANE-WORDS PLANE_CTL / STRIDE / SIZE / COLOR_CTL / SURF equal Linux's dump, PLANE_SURF last");
}

/* Checks the DDI words and the DDI_BUF_CTL value against Linux's dump. */
static void
i915_edp_ktest_lcd_a_ddi(
	struct i915_ktest *ktest,
	struct i915_lcd_world *lcd_world)
{
	struct i915_lcd_words *words;
	uint32_t buf_ctl;
	uint32_t func;
	unsigned func_writes;
	int passed;
	int rc;

	/* Emits the words of port A, pipe A, transcoder A, no port reversal. */
	words = &i915_edp_ktest_ddi_words;
	buf_ctl = 0u;
	rc = drv_i915_lcd_emit_ddi(lcd_world, &i915_edp_ktest_lcd, 0, 0, 0, 0u, words, &buf_ctl);

	/* Looks TRANS_DDI_FUNC_CTL up. */
	func = 0u;
	func_writes = drv_i915_lcd_words_find(words, 0x60400u, &func);

	/* MSA 6 bpc, FUNC_CTL2, FUNC_CTL 0x8a210002, and DDI_BUF_CTL 0x00000002 before its enable bit. */
	passed = 0;
	if (rc == 0 &&
	    words->n == 3u &&
	    words->w[0].reg == 0x60410u &&
	    words->w[0].value == 1u &&
	    words->w[1].reg == 0x60404u &&
	    func_writes == 1u &&
	    func == 0x8a210002u &&
	    buf_ctl == 0x00000002u)
		passed = 1;

	drv_i915_ktest_check(ktest, passed,
			     "lcd: LCD-A-DDI-WORDS TRANS_DDI_FUNC_CTL equals Linux's dump (0x8a210002), DDI_BUF_CTL value + enable = 0x80000002, MSA 6 bpc");
}

/* Checks that a link which cannot carry the mode is refused. */
static void
i915_edp_ktest_lcd_a_fit(
	struct i915_ktest *ktest,
	struct i915_lcd_world *lcd_world)
{
	uint8_t one_lane[16];
	int rc;

	/* A sink of RBR x1 only. */
	kern_memcpy(one_lane, i915_dp_fixture_dpcd_000, sizeof(one_lane));
	one_lane[1] = 0x06u;
	one_lane[2] = 0x01u;

	/* The calculation refuses the mode with ENOSPC. */
	rc = drv_i915_lcd_compute(lcd_world, i915_dp_fixture_edid, one_lane, i915_dp_fixture_dpcd_700, 18, 38400, &i915_edp_ktest_lcd);
	drv_i915_ktest_check(ktest, rc == ENOSPC, "lcd: LCD-A-FIT a link that cannot carry the mode is refused (RBR x1)");
}

/* Checks that a Type-C AUX channel is refused. */
static void
i915_edp_ktest_config(
	struct i915_ktest *ktest,
	struct i915_dp_world *world)
{
	struct i915_edp_config cfg;
	int rc;

	/* The target's configuration on AUX D, a Type-C channel. */
	i915_edp_ktest_fresh(world);
	i915_edp_ktest_target_cfg(&cfg);
	cfg.aux_ch = 3;

	/* The bring-up refuses it before anything goes live. */
	rc = drv_i915_edp_begin(world, &i915_edp_ktest_env, &cfg, &i915_edp_ktest_res);
	drv_i915_ktest_check(ktest, rc == -I915_EDP_EINVAL, "edp: EDP-CONFIG a Type-C AUX channel is refused");
}
