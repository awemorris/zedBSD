/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The eDP stage on real threads, locks and ticks, in the kernel build.
 *
 * Two parts, neither of which reaches the hardware:
 *
 *   1. the delayed work of the work queue (workqueue.c) on its own: a
 *      reservation fires by itself from the tick, a cancel and a re-arm move
 *      or drop the deadline, and a synchronous cancel waits for a running
 *      body;
 *   2. the eDP stage with the kernel backend's real mutexes, timer and worker
 *      (dp-sink.c) driving the DP register model: the delayed VDD-off runs by
 *      itself, a re-acquisition before the deadline keeps VDD on, and a stop
 *      while the timer waits or while the worker body runs neither deadlocks
 *      nor turns VDD off twice.
 *
 * The second part runs in the borrowed DP world (edp-ktest.h).
 */

#include "edp-ktest.h"
#include "display-ktest.h"
#include <kern/kcrt.h>

#include "../execution/ktest.h"
#include "../../display/internal.h"
#include "../../display/dp-sink.h"
#include "dp-fake-hw.h"
#include "dp-fixture-latitude5330.h"

#include <kern/lock.h>
#include <kern/sched.h>

#include <stdint.h>

/* The PP_CONTROL register of PPS 0, and its bit that forces VDD on. */
#define I915_EDP_SYNC_PP_CONTROL 0xc7204u
#define I915_EDP_SYNC_VDD_FORCE 8u

/*
 * The body the delayed-work part queues, and what it observed.
 *
 * One instance (i915_edp_sync_body) lives for the part; the body runs on the
 * worker thread and the part waits on its completions.
 */
struct i915_edp_sync_body {
	/* Signalled when the body starts, released by the part, signalled when the body ends. */
	struct i915_completion started;
	struct i915_completion release;
	struct i915_completion done;

	/* Nonzero while the body must wait for the release before it ends. */
	int block;

	/* How often the body ran, and the scheduler tick of its last start. */
	unsigned runs;
	uint64_t ran_at;
};

/*
 * The delayed-work part's body state.
 *
 * Cleared by the part before its first queue; written by the worker thread
 * and read by the part only after a completion ordered the two.
 */
static struct i915_edp_sync_body i915_edp_sync_body;

/*
 * The work queue, timer queue and delayed work of the delayed-work part.
 *
 * Created and destroyed by the part; too large for the stack.
 */
static struct i915_workqueue i915_edp_sync_wq;
static struct i915_timer_queue i915_edp_sync_tq;
static struct i915_delayed_work i915_edp_sync_dw;

/*
 * The DP register model of the eDP part.
 *
 * The model is single-threaded by design.  Here the part's thread and the
 * worker thread both reach it; the PPS lock serialises them except for the
 * power references, which i915_edp_sync_model_lock protects.
 */
static struct i915_dp_fake_hw i915_edp_sync_hw;

/*
 * The model's own environment hooks (their context is the model).
 *
 * Bound with the model by each fresh start; the eDP's environment wraps
 * them.
 */
static struct i915_dp_env i915_edp_sync_model_env;

/*
 * The eDP's environment: the model's registers, clock and power under the
 * kernel backend's locks and delayed work.
 *
 * Bound afresh with the model; the eDP keeps a pointer to it while live.
 */
static struct i915_dp_env i915_edp_sync_env;

/*
 * The kernel backend: the mutexes, the timer and the worker.
 *
 * It is the eDP device's backend inside the borrowed world's carrier, so the
 * worker's VDD-off body finds that world.  Set for the length of the eDP
 * part; NULL otherwise.
 */
static struct i915_dp_kernel *i915_edp_sync_kernel;

/*
 * The result of the current bring-up.
 *
 * Filled by each begin and read by the checks that follow it.
 */
static struct i915_edp_result i915_edp_sync_res;

/*
 * Signalled when the worker body is inside the VDD-off write, holding the
 * PPS lock.
 *
 * Prepared by the eDP part; signalled at most once per slow VDD-off.
 */
static struct i915_completion i915_edp_sync_in_pp_write;

/*
 * The model's own register write, which the eDP's write hook forwards to.
 *
 * Taken from the model's hooks by each fresh start.
 */
static void (*i915_edp_sync_model_write)(void *ctx, uint32_t reg, uint32_t value);

/*
 * Nonzero while the VDD-off write stops for 150 ms inside the PPS lock.
 *
 * Set and cleared by the part around the stop-while-running case.
 */
static int i915_edp_sync_slow_vdd_off;

/*
 * Protects the model's power references against the two threads.
 *
 * Initialised by the eDP part before the first bring-up.
 */
static struct spinlock i915_edp_sync_model_lock;

static void i915_edp_sync_body_run(void *context);
static void i915_edp_sync_delayed_work(struct i915_ktest *ktest);
static void i915_edp_sync_delayed_cancels(struct i915_ktest *ktest);
static uint32_t i915_edp_sync_read32(void *ctx, uint32_t reg);
static void i915_edp_sync_write32(void *ctx, uint32_t reg, uint32_t value);
static int i915_edp_sync_wait_reg(void *ctx, uint32_t reg, uint32_t mask, uint32_t value, unsigned fast_us, unsigned slow_ms, uint32_t *out);
static void i915_edp_sync_sleep_us(void *ctx, unsigned us);
static uint64_t i915_edp_sync_now_ms(void *ctx);
static int i915_edp_sync_power_get(void *ctx, int domain);
static void i915_edp_sync_power_put(void *ctx, int domain);
static void i915_edp_sync_power_put_async(void *ctx, int domain);
static void i915_edp_sync_fresh(struct i915_dp_world *world);
static int i915_edp_sync_released(void);
static unsigned i915_edp_sync_wait_vdd_off(unsigned limit_ms);
static void i915_edp_sync_edp(struct i915_ktest *ktest, struct i915_dp_world *world, struct i915_dp_kernel *k);
static void i915_edp_sync_auto_off(struct i915_ktest *ktest, struct i915_dp_world *world, struct i915_dp_kernel *k, const struct i915_edp_config *cfg);
static void i915_edp_sync_reacquire(struct i915_ktest *ktest, struct i915_dp_world *world, struct i915_dp_kernel *k);
static void i915_edp_sync_stop_running(struct i915_ktest *ktest, struct i915_dp_world *world, struct i915_dp_kernel *k, const struct i915_edp_config *cfg);

/*
 * Checks the delayed work on real threads, then the eDP stage running on
 * the kernel backend's real locks, timer and worker with the register model.
 */
void
drv_i915_display_ktest_edp_sync(
	struct i915_ktest *ktest)
{
	struct i915_edp_ktest_world borrow;
	int error;

	/* The delayed work on its own. */
	i915_edp_sync_delayed_work(ktest);

	/* Borrows the DP world; without it the eDP part cannot run. */
	error = drv_i915_display_ktest_edp_world_enter(ktest, &borrow);
	if (error != 0) {
		drv_i915_ktest_skip(ktest, "edp-sync: the eDP stage on real threads",
				    "the DP world could not be borrowed: the resident panel's VDD-off did not settle, or no memory");
		return;
	}

	/* The eDP on the carrier's kernel backend, then the world goes back. */
	i915_edp_sync_edp(ktest, borrow.world, &borrow.carrier->edp_dev.k);
	drv_i915_display_ktest_edp_world_leave(&borrow);
}

/* Runs the body of the delayed-work part on the worker thread. */
static void
i915_edp_sync_body_run(
	void *context)
{
	struct i915_edp_sync_body *body;
	uint64_t deadline;

	/* Counts the run and tells the part it started. */
	body = context;
	body->runs++;
	body->ran_at = sched_ticks();
	drv_i915_complete(&body->started);

	/* Waits, at most 5 s, for the part to release a blocked body. */
	if (body->block) {
		deadline = drv_i915_ktest_deadline_ms(5000u);
		(void)drv_i915_wait_for_completion(&body->release, deadline);
	}

	/* Tells the part it ended. */
	drv_i915_complete(&body->done);
}

/* Checks queueing, firing, cancelling and re-arming a delayed work on real threads. */
static void
i915_edp_sync_delayed_work(
	struct i915_ktest *ktest)
{
	struct i915_edp_sync_body *body;
	uint64_t t0;
	int passed;
	int error;
	int r1;
	int r2;
	int r3;

	/* Prepares the body. */
	body = &i915_edp_sync_body;
	kern_memset(body, 0, sizeof(*body));
	drv_i915_completion_init(&body->started, "dw-started");
	drv_i915_completion_init(&body->release, "dw-release");
	drv_i915_completion_init(&body->done, "dw-done");

	/* Starts the worker thread and the timer thread feeding it. */
	error = drv_i915_workqueue_create(&i915_edp_sync_wq, "ktest-dw-wq");
	if (error != 0) {
		drv_i915_ktest_check(ktest, 0, "dwork: DW-SETUP worker and timer threads start");
		return;
	}

	error = drv_i915_timer_queue_create(&i915_edp_sync_tq, &i915_edp_sync_wq, "ktest-dw-timer");
	if (error != 0) {
		drv_i915_workqueue_destroy(&i915_edp_sync_wq);
		drv_i915_ktest_check(ktest, 0, "dwork: DW-SETUP worker and timer threads start");
		return;
	}

	drv_i915_delayed_work_init(&i915_edp_sync_dw, i915_edp_sync_body_run, body);

	/* Queues the work for 50 ms, then again while it is armed. */
	t0 = sched_ticks();
	r1 = drv_i915_delayed_queue(&i915_edp_sync_tq, &i915_edp_sync_dw, 50u);
	r2 = drv_i915_delayed_queue(&i915_edp_sync_tq, &i915_edp_sync_dw, 50u);
	r3 = drv_i915_delayed_pending(&i915_edp_sync_tq, &i915_edp_sync_dw);
	passed = 0;
	if (r1 == 1 &&
	    r2 == 0 &&
	    r3 == 1 &&
	    body->runs == 0u)
		passed = 1;

	drv_i915_ktest_check(ktest, passed,
			     "dwork: DW-QUEUE newly queued once; a second queue while armed is refused; nothing ran yet");

	/* The body runs by itself, not before the 50 ms (5 ticks) asked for. */
	r1 = drv_i915_wait_for_completion(&body->done, drv_i915_ktest_deadline_ms(1000u));
	passed = 0;
	if (r1 == 1 &&
	    body->runs == 1u &&
	    body->ran_at - t0 >= 5u &&
	    i915_edp_sync_dw.fired_count == 1u)
		passed = 1;

	drv_i915_ktest_check(ktest, passed,
			     "dwork: DW-FIRE the body runs by itself on the worker, not before the 50 ms (5 ticks) asked for");

	/* The cancels, the synchronous cancel and the flush, then the threads end. */
	i915_edp_sync_delayed_cancels(ktest);
	drv_i915_timer_queue_destroy(&i915_edp_sync_tq);
	drv_i915_workqueue_destroy(&i915_edp_sync_wq);
	passed = 0;
	if (i915_edp_sync_tq.alive == 0 && i915_edp_sync_wq.worker_alive == 0)
		passed = 1;

	drv_i915_ktest_check(ktest, passed, "dwork: DW-STOP both threads end");
}

/* Checks a cancel, a re-arm and a synchronous cancel of the delayed work. */
static void
i915_edp_sync_delayed_cancels(
	struct i915_ktest *ktest)
{
	struct i915_edp_sync_body *body;
	uint64_t t0;
	int passed;
	int r1;
	int r2;
	int r3;

	/* A cancelled reservation never runs. */
	body = &i915_edp_sync_body;
	(void)drv_i915_delayed_queue(&i915_edp_sync_tq, &i915_edp_sync_dw, 100u);
	r1 = drv_i915_delayed_cancel(&i915_edp_sync_tq, &i915_edp_sync_dw);
	r2 = drv_i915_wait_for_completion(&body->done, drv_i915_ktest_deadline_ms(250u));
	r3 = drv_i915_delayed_pending(&i915_edp_sync_tq, &i915_edp_sync_dw);
	passed = 0;
	if (r1 == 1 &&
	    r2 == 0 &&
	    body->runs == 1u &&
	    r3 == 0)
		passed = 1;

	drv_i915_ktest_check(ktest, passed, "dwork: DW-CANCEL a cancelled reservation never runs");

	/* Re-arms before the deadline: the deadline counts from the last queue. */
	(void)drv_i915_delayed_queue(&i915_edp_sync_tq, &i915_edp_sync_dw, 100u);
	(void)drv_i915_wait_for_completion(&body->done, drv_i915_ktest_deadline_ms(50u));
	(void)drv_i915_delayed_cancel(&i915_edp_sync_tq, &i915_edp_sync_dw);
	t0 = sched_ticks();
	(void)drv_i915_delayed_queue(&i915_edp_sync_tq, &i915_edp_sync_dw, 100u);
	r1 = drv_i915_wait_for_completion(&body->done, drv_i915_ktest_deadline_ms(1000u));
	passed = 0;
	if (r1 == 1 &&
	    body->runs == 2u &&
	    body->ran_at - t0 >= 10u)
		passed = 1;

	drv_i915_ktest_check(ktest, passed, "dwork: DW-REARM cancel + queue moves the deadline: the old one does not fire");

	/*
	 * Cancels while the body is running: a plain cancel does not wait, a
	 * synchronous one returns only after the body finished.  The
	 * completions count, so they start from none.
	 */
	drv_i915_reinit_completion(&body->started);
	drv_i915_reinit_completion(&body->done);
	body->block = 1;
	(void)drv_i915_delayed_queue(&i915_edp_sync_tq, &i915_edp_sync_dw, 0u);
	r1 = drv_i915_wait_for_completion(&body->started, drv_i915_ktest_deadline_ms(1000u));
	r2 = drv_i915_delayed_cancel(&i915_edp_sync_tq, &i915_edp_sync_dw);
	drv_i915_complete(&body->release);
	(void)drv_i915_delayed_cancel_sync(&i915_edp_sync_tq, &i915_edp_sync_dw, drv_i915_ktest_deadline_ms(3000u));

	/* The body must already be complete. */
	r3 = drv_i915_wait_for_completion(&body->done, drv_i915_ktest_deadline_ms(10u));
	passed = 0;
	if (r1 == 1 &&
	    r2 == 0 &&
	    r3 == 1 &&
	    body->runs == 3u)
		passed = 1;

	drv_i915_ktest_check(ktest, passed,
			     "dwork: DW-CANCEL-SYNC a plain cancel does not wait for a running body; cancel_sync returns only after it finished");
	body->block = 0;

	/* The work queue offers no flush of a delayed work, so the flush is not checked. */
	drv_i915_ktest_skip(ktest,
			    "dwork: DW-FLUSH an armed work is run now and waited for, without waiting for its deadline",
			    "the work queue has no flush of a delayed work; production never flushes one");
}

/* Reads a register of the model. */
static uint32_t
i915_edp_sync_read32(
	void *ctx,
	uint32_t reg)
{
	uint32_t value;

	UNUSED_PARAMETER(ctx);

	/* Forwards to the model's own hook. */
	value = i915_edp_sync_model_env.read32(&i915_edp_sync_hw, reg);

	/* Succeeded: reports the register's value. */
	return value;
}

/*
 * Writes a register of the model; with the slow VDD-off set, the write
 * that turns VDD off stops for 150 ms first, inside the PPS lock.
 */
static void
i915_edp_sync_write32(
	void *ctx,
	uint32_t reg,
	uint32_t value)
{
	UNUSED_PARAMETER(ctx);

	/* The worker body is now inside the VDD-off write, holding the PPS lock. */
	if (i915_edp_sync_slow_vdd_off &&
	    reg == I915_EDP_SYNC_PP_CONTROL &&
	    (value & I915_EDP_SYNC_VDD_FORCE) == 0u &&
	    (i915_edp_sync_hw.pp_control & I915_EDP_SYNC_VDD_FORCE) != 0u) {
		drv_i915_complete(&i915_edp_sync_in_pp_write);
		drv_i915_dp_kernel_sleep_us(i915_edp_sync_kernel, 150000u);
	}

	/* Forwards to the model's own hook. */
	i915_edp_sync_model_write(&i915_edp_sync_hw, reg, value);
}

/* Waits for a register of the model. */
static int
i915_edp_sync_wait_reg(
	void *ctx,
	uint32_t reg,
	uint32_t mask,
	uint32_t value,
	unsigned fast_us,
	unsigned slow_ms,
	uint32_t *out)
{
	int rc;

	UNUSED_PARAMETER(ctx);

	/* Forwards to the model's own hook. */
	rc = i915_edp_sync_model_env.wait_reg(&i915_edp_sync_hw, reg, mask, value, fast_us, slow_ms, out);
	if (rc != 0)
		return rc;

	/* Succeeded: the register reached the value. */
	return 0;
}

/* Sleeps on the model's clock, so the panel delays cost no real time. */
static void
i915_edp_sync_sleep_us(
	void *ctx,
	unsigned us)
{
	UNUSED_PARAMETER(ctx);

	/* Forwards to the model's own hook. */
	i915_edp_sync_model_env.sleep_us(&i915_edp_sync_hw, us);
}

/* Reads the model's clock. */
static uint64_t
i915_edp_sync_now_ms(
	void *ctx)
{
	uint64_t now;

	UNUSED_PARAMETER(ctx);

	/* Forwards to the model's own hook. */
	now = i915_edp_sync_model_env.now_ms(&i915_edp_sync_hw);

	/* Succeeded: reports the model's milliseconds. */
	return now;
}

/* Takes a power reference in the model, under the model lock. */
static int
i915_edp_sync_power_get(
	void *ctx,
	int domain)
{
	int rc;

	UNUSED_PARAMETER(ctx);

	/* Forwards to the model's own hook. */
	spin_lock(&i915_edp_sync_model_lock);

	rc = i915_edp_sync_model_env.power_get(&i915_edp_sync_hw, domain);

	spin_unlock(&i915_edp_sync_model_lock);

	/* Reports a refused reference. */
	if (rc != 0)
		return rc;

	/* Succeeded: the reference is held. */
	return 0;
}

/* Returns a power reference to the model, under the model lock. */
static void
i915_edp_sync_power_put(
	void *ctx,
	int domain)
{
	UNUSED_PARAMETER(ctx);

	/* Forwards to the model's own hook. */
	spin_lock(&i915_edp_sync_model_lock);

	i915_edp_sync_model_env.power_put(&i915_edp_sync_hw, domain);

	spin_unlock(&i915_edp_sync_model_lock);
}

/* Parks a power reference in the model, under the model lock. */
static void
i915_edp_sync_power_put_async(
	void *ctx,
	int domain)
{
	UNUSED_PARAMETER(ctx);

	/* Forwards to the model's own hook. */
	spin_lock(&i915_edp_sync_model_lock);

	i915_edp_sync_model_env.power_put_async(&i915_edp_sync_hw, domain);

	spin_unlock(&i915_edp_sync_model_lock);
}

/*
 * Starts the model afresh and binds the eDP's environment: the kernel
 * backend's real locks and delayed work, the model's registers, clock and
 * power.
 */
static void
i915_edp_sync_fresh(
	struct i915_dp_world *world)
{
	struct i915_dp_env *env;

	/* The model with the captured panel, and its own hooks. */
	drv_i915_dp_fake_init(&i915_edp_sync_hw, i915_dp_fixture_dpcd_000, i915_dp_fixture_dpcd_100, i915_dp_fixture_dpcd_700,
			      i915_dp_fixture_edid, sizeof(i915_dp_fixture_edid));
	drv_i915_dp_fake_bind_env(&i915_edp_sync_hw, &i915_edp_sync_model_env, world);
	i915_edp_sync_model_write = i915_edp_sync_model_env.write32;

	/* The real locks and delayed work; the backend is the hooks' context. */
	env = &i915_edp_sync_env;
	kern_memset(env, 0, sizeof(*env));
	drv_i915_dp_kernel_bind_sync(i915_edp_sync_kernel, env);

	/* The registers and waits of the model. */
	env->read32 = i915_edp_sync_read32;
	env->write32 = i915_edp_sync_write32;
	env->wait_reg = i915_edp_sync_wait_reg;

	/* The model's clock: the panel delays cost no real time. */
	env->sleep_us = i915_edp_sync_sleep_us;
	env->now_ms = i915_edp_sync_now_ms;

	/* The model's power references. */
	env->power_get = i915_edp_sync_power_get;
	env->power_put = i915_edp_sync_power_put;
	env->power_put_async = i915_edp_sync_power_put_async;

	/* The VDD-off write runs at full speed. */
	i915_edp_sync_slow_vdd_off = 0;
}

/*
 * Reports whether the ended eDP released everything: no power reference,
 * VDD off, no wakeref, no pending work, no lock or put error, and after the
 * power layer's flush no reference in the model.
 */
static int
i915_edp_sync_released(void)
{
	struct i915_edp_result *res;
	int owned;

	/* Looks for anything the DP layer still owns. */
	res = &i915_edp_sync_res;
	owned = 0;
	if (i915_edp_sync_env.power_refs[0] != 0) {
		owned = 1;
	} else if (i915_edp_sync_env.power_refs[1] != 0) {
		owned = 1;
	} else if ((i915_edp_sync_hw.pp_control & I915_EDP_SYNC_VDD_FORCE) != 0u) {
		owned = 1;
	} else if (res->vdd_wakeref_held != 0) {
		owned = 1;
	} else if (res->vdd_work_pending != 0) {
		owned = 1;
	} else if (res->power_put_underflows != 0u) {
		owned = 1;
	} else if (i915_edp_sync_env.lock_errors != 0u) {
		owned = 1;
	}

	/* Releases the parked references as the power layer would. */
	drv_i915_dp_fake_flush_async(&i915_edp_sync_hw);

	/* Something was still owned. */
	if (owned)
		return 0;

	/* The model must hold no reference either. */
	if (i915_edp_sync_hw.refs_core != 0)
		return 0;
	if (i915_edp_sync_hw.refs_aux != 0)
		return 0;

	/* Succeeded: everything was released. */
	return 1;
}

/* Waits, sleeping 20 ms at a time, until VDD is off or the limit passed; reports the milliseconds waited. */
static unsigned
i915_edp_sync_wait_vdd_off(
	unsigned limit_ms)
{
	unsigned waited;

	/* Looks at the model's VDD bit between sleeps. */
	waited = 0u;
	while ((i915_edp_sync_hw.pp_control & I915_EDP_SYNC_VDD_FORCE) != 0u && waited < limit_ms) {
		drv_i915_dp_kernel_sleep_us(i915_edp_sync_kernel, 20000u);
		waited += 20u;
	}

	/* Succeeded: reports how long it waited. */
	return waited;
}

/* Checks the eDP stage on the kernel backend's real locks, timer and worker with the register model. */
static void
i915_edp_sync_edp(
	struct i915_ktest *ktest,
	struct i915_dp_world *world,
	struct i915_dp_kernel *k)
{
	struct i915_edp_config cfg;
	int passed;
	int error;

	/* Prepares the model lock and the completion of the slow VDD-off. */
	i915_edp_sync_kernel = k;
	spin_init(&i915_edp_sync_model_lock, LOCK_RANK_DEVICE, "ktest-dp-model");
	drv_i915_completion_init(&i915_edp_sync_in_pp_write, "ktest-pp-write");

	/* Starts the locks, the worker and the timer. */
	error = drv_i915_dp_kernel_sync_start(k);
	if (error != 0) {
		drv_i915_ktest_check(ktest, 0, "edp-sync: SYNC-SETUP locks, worker and timer start");
		i915_edp_sync_kernel = NULL;
		return;
	}

	/*
	 * The shortest delays Linux's rules give: T11_T12 of 1 plus 100 ms,
	 * rounded up, is 200 ms, and five of them are 1 s.
	 */
	kern_memset(&cfg, 0, sizeof(cfg));
	cfg.rawclk_khz = 19200u;
	cfg.t1_t3 = 100u;
	cfg.t8 = 10u;
	cfg.t9 = 10u;
	cfg.t10 = 100u;
	cfg.t11_t12 = 1u;
	cfg.log_level = -1;

	/* The delayed off by itself, a re-acquisition, and the two stops. */
	i915_edp_sync_auto_off(ktest, world, k, &cfg);
	i915_edp_sync_reacquire(ktest, world, k);
	i915_edp_sync_stop_running(ktest, world, k, &cfg);

	/* The threads end. */
	drv_i915_dp_kernel_sync_stop(k);
	passed = 0;
	if (k->tq.alive == 0 && k->wq.worker_alive == 0)
		passed = 1;

	drv_i915_ktest_check(ktest, passed, "edp-sync: SYNC-STOP threads end");
	i915_edp_sync_kernel = NULL;
}

/* Checks that the delayed VDD-off runs by itself, from the tick, on the worker thread. */
static void
i915_edp_sync_auto_off(
	struct i915_ktest *ktest,
	struct i915_dp_world *world,
	struct i915_dp_kernel *k,
	const struct i915_edp_config *cfg)
{
	struct i915_edp_result *res;
	unsigned waited;
	int passed;
	int rc;

	/* Brings the eDP up and runs the late step. */
	res = &i915_edp_sync_res;
	i915_edp_sync_fresh(world);
	rc = drv_i915_edp_begin(world, &i915_edp_sync_env, cfg, res);
	if (rc == 0)
		rc = drv_i915_edp_init_late(world, cfg, res);

	/* The panel is kept: VDD on, its AUX reference held, the off reserved in 1 s. */
	passed = 0;
	if (rc == 0 &&
	    res->delay_power_cycle_ms == 200 &&
	    res->vdd_on_hw == 1 &&
	    res->vdd_work_pending == 1 &&
	    res->vdd_wakeref_held == 1 &&
	    res->power_refs_aux == 1)
		passed = 1;

	drv_i915_ktest_check(ktest, passed,
			     "edp-sync: SYNC-RESERVE after late init the panel is KEPT: VDD on, its AUX reference held, off reserved in 1 s");

	/* Nobody drives the worker: about 1 s later it takes the PPS lock and forces VDD off. */
	waited = i915_edp_sync_wait_vdd_off(3000u);
	drv_i915_edp_snapshot(world, res);
	passed = 0;
	if (waited >= 800u &&
	    waited < 3000u &&
	    res->vdd_on_hw == 0 &&
	    res->vdd_wakeref_held == 0 &&
	    res->power_refs_aux == 0 &&
	    i915_edp_sync_hw.refs_aux == 0 &&
	    k->vdd_off_work.work.ran_count == 1 &&
	    k->vdd_off_work.fired_count == 1u &&
	    i915_edp_sync_env.lock_errors == 0u)
		passed = 1;

	drv_i915_ktest_check(ktest, passed,
			     "edp-sync: SYNC-AUTO-OFF nobody drives it: ~1 s later the worker took the PPS lock, forced VDD off and returned the reference");
}

/* Checks a re-acquisition before the deadline and a stop while the timer waits. */
static void
i915_edp_sync_reacquire(
	struct i915_ktest *ktest,
	struct i915_dp_world *world,
	struct i915_dp_kernel *k)
{
	struct i915_edp_result *res;
	uint8_t bytes[2];
	unsigned waited;
	unsigned ran;
	long read;
	int released;
	int pending;
	int reserved;
	int passed;
	int end;

	/* A read turns VDD on and reserves its off; a second read 0.6 s later cancels and reserves anew. */
	res = &i915_edp_sync_res;
	read = drv_i915_edp_dpcd_read(world, 0x000u, bytes, sizeof(bytes));
	ran = (unsigned)k->vdd_off_work.work.ran_count;
	drv_i915_dp_kernel_sleep_us(k, 600000u);
	if (read == 2)
		read = drv_i915_edp_dpcd_read(world, 0x000u, bytes, sizeof(bytes));

	/* 1.2 s after the first reservation, its deadline has passed and VDD must still be on. */
	drv_i915_dp_kernel_sleep_us(k, 600000u);
	passed = 0;
	if (read == 2 &&
	    (i915_edp_sync_hw.pp_control & I915_EDP_SYNC_VDD_FORCE) != 0u &&
	    (unsigned)k->vdd_off_work.work.ran_count == ran &&
	    k->vdd_off_work.cancelled_armed >= 1u)
		passed = 1;

	drv_i915_ktest_check(ktest, passed,
			     "edp-sync: SYNC-REACQUIRE a read before the deadline cancels the reservation; the old deadline passes and VDD stays on");

	/* The new reservation then fires by itself. */
	waited = i915_edp_sync_wait_vdd_off(3000u);
	passed = 0;
	if (waited < 3000u && (unsigned)k->vdd_off_work.work.ran_count == ran + 1u)
		passed = 1;

	drv_i915_ktest_check(ktest, passed, "edp-sync: SYNC-REACQUIRE-OFF the new reservation then fires by itself");

	/* Stops with the off still reserved. */
	read = drv_i915_edp_dpcd_read(world, 0x000u, bytes, sizeof(bytes));
	drv_i915_edp_snapshot(world, res);
	reserved = res->vdd_work_pending;
	end = drv_i915_edp_end(world, res);

	/* The reservation was cancelled synchronously, VDD forced off and the references returned. */
	passed = 0;
	if (read == 2 &&
	    reserved == 1 &&
	    end == 0) {
		released = i915_edp_sync_released();
		pending = drv_i915_delayed_pending(&k->tq, &k->vdd_off_work);
		if (released && pending == 0)
			passed = 1;
	}

	drv_i915_ktest_check(ktest, passed,
			     "edp-sync: SYNC-STOP-WAITING stop with the off still reserved: cancelled synchronously, VDD forced off, references back");
}

/* Checks a stop while the worker body runs, and a body that runs after the end. */
static void
i915_edp_sync_stop_running(
	struct i915_ktest *ktest,
	struct i915_dp_world *world,
	struct i915_dp_kernel *k,
	const struct i915_edp_config *cfg)
{
	struct i915_edp_result *res;
	int released;
	int inside;
	int passed;
	int end;
	int rc;

	/* Brings the eDP up again with the VDD-off write slowed down inside the PPS lock. */
	res = &i915_edp_sync_res;
	i915_edp_sync_fresh(world);
	rc = drv_i915_edp_begin(world, &i915_edp_sync_env, cfg, res);
	if (rc == 0)
		rc = drv_i915_edp_init_late(world, cfg, res);

	i915_edp_sync_slow_vdd_off = 1;

	/*
	 * Waits until the worker body is inside the VDD-off write, then stops:
	 * the synchronous cancel must wait for the body before it takes the lock.
	 */
	inside = drv_i915_wait_for_completion(&i915_edp_sync_in_pp_write, drv_i915_ktest_deadline_ms(3000u));
	end = drv_i915_edp_end(world, res);

	/* No deadlock, one VDD-off only, and the work idle after the end. */
	passed = 0;
	if (rc == 0 &&
	    inside == 1 &&
	    end == 0) {
		released = i915_edp_sync_released();
		if (released &&
		    i915_edp_sync_hw.vdd_off_events == 1u &&
		    k->vdd_off_work.work.state == I915_WORK_IDLE)
			passed = 1;
	}

	drv_i915_ktest_check(ktest, passed,
			     "edp-sync: SYNC-STOP-RUNNING stop while the body is mid-write: no deadlock, no double off, no access after the end");
	i915_edp_sync_slow_vdd_off = 0;

	/* A body dequeued after the end finds nothing live. */
	drv_i915_edp_work_run(world, I915_DP_WORK_VDD_OFF);
	drv_i915_ktest_check(ktest, i915_edp_sync_hw.vdd_off_events == 1u,
			     "edp-sync: SYNC-LATE-BODY a body that runs after the end touches nothing");
}
