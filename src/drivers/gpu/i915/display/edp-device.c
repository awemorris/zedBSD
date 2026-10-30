/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The display's built-in panel device and the kernel backend it runs on.
 *
 * The backend gives the display's DisplayPort code its locks (the panel
 * power sequencer's and the AUX channels'), a worker thread and a timer
 * thread for the delayed VDD-off and the power layer's asynchronous put,
 * sleeps and a millisecond clock, and the power references.  The panel
 * device takes the panel's sink through the xd code on that backend:
 * the sequencer, the sink's capabilities and its EDID, then the late
 * step with the panel's final VBT delays; it plans the panel's mode and
 * link, and it gives the resident run the panel's sink and panel power
 * as backend hooks.
 */

#include "internal.h"
#include "edp-device.h"
#include "dp-ext-kern.h"
#include "display-power.h"
#include "vbt-parse.h"
#include "xd-edid.h"
#include <kern/kcrt.h>

#include "../mmio.h"
#include "../sync.h"
#include "../workqueue.h"

#include <kern/clock.h>
#include <kern/klog.h>
#include <kern/lock.h>
#include <kern/sched.h>

#include <uapi/errno.h>
#include <stddef.h>

/* DDI_BUF_CTL of port A, and its port reversal bit. */
#define I915_EDP_DDI_BUF_CTL_A		0x64000u
#define I915_EDP_DDI_BUF_PORT_REVERSAL	0x00010000u

static int i915_dp_kernel_pd_async_queue(void *ctx, int delay_ms);
static int i915_dp_kernel_pd_async_cancel(void *ctx, int sync);

static uint32_t i915_dp_kernel_read32(void *ctx, uint32_t reg);
static void i915_dp_kernel_write32(void *ctx, uint32_t reg, uint32_t value);
static int i915_dp_kernel_wait_reg(void *ctx, uint32_t reg, uint32_t mask, uint32_t value, unsigned fast_us, unsigned slow_ms, uint32_t *out);
static int i915_dp_kernel_now_us(struct i915_dp_kernel *kernel, uint64_t *us);
static void i915_dp_kernel_sleep(void *ctx, unsigned us);
static uint64_t i915_dp_kernel_now_ms(void *ctx);
static void i915_dp_kernel_lock(void *ctx, int which);
static void i915_dp_kernel_unlock(void *ctx, int which);
static int i915_dp_kernel_delayed_queue(void *ctx, int which, unsigned delay_ms);
static int i915_dp_kernel_delayed_cancel(void *ctx, int which, int sync);
static int i915_dp_kernel_delayed_pending(void *ctx, int which);
static void i915_dp_kernel_vdd_off_body(void *arg);
static void i915_dp_kernel_async_put_body(void *arg);
static void i915_edp_log_bytes(const char *tag, const uint8_t *bytes, unsigned count);
static void i915_edp_cfg_from_panel(struct i915_edp_config *cfg, const struct i915_vbt_encoder *enc, const struct i915_vbt_panel *panel, int have_panel);
static unsigned i915_edp_well_refs(const struct i915_display_power *power);
static void i915_edp_device_log_acquire(struct i915_edp_device *dev, int error);
static void i915_edp_device_late(struct i915_edp_device *dev, const struct i915_vbt_encoder *enc, int port);
static int i915_edp_device_take(struct i915_edp_device *dev);
static void i915_edp_cfg_delays(const struct i915_edp_config *cfg, struct i915_xd_pps_delays *delays);
static void i915_edp_device_result(struct i915_edp_device *dev);
static uint64_t i915_edp_xe_now(void *context);
static uint64_t i915_edp_xe_functions(struct i915_edp_device *dev, unsigned use);
static int i915_edp_xe_power_get(void *context, unsigned use);
static void i915_edp_xe_power_put(void *context, unsigned use, int cookie);
static void i915_edp_xe_power_put_later(void *context, unsigned use, int cookie);
static void i915_edp_xe_lock(void *context, unsigned lock, int taken);
static void i915_edp_xe_vdd_later(void *context, unsigned delay_ms);
static void i915_edp_xe_vdd_cancel(void *context, int wait);
static void i915_edp_xe_error(void *context, const char *text);
static void i915_edp_device_link(struct i915_edp_device *dev);
static struct i915_xd_edp *i915_edp_emit_sink(void *ctx);
static uint64_t i915_dp_kernel_sync_deadline(void);
static int i915_edp_emit_error(int error);

/*
 * Sleeps with the kernel's ordinary sleep.
 *
 * Every sleep the display code asks for is an ordinary, sleepable-context
 * sleep and goes to kern_usleep_range(): one absolute deadline computed at
 * entry, the kernel tick and a wait queue, the deadline checked again on
 * every wake, a late wake allowed.  No busy remainder is left: a 500 us
 * retry interval simply waits for the next tick.  The short counter-based
 * delay and the atomic register polls stay in drv_i915_wait_reg()'s fast
 * stage and are not reached from here.  A sleep that returns early is
 * counted as a time-base fault, not a success.
 */
void
drv_i915_dp_kernel_sleep_us(
	struct i915_dp_kernel *k,
	unsigned us)
{
	uint64_t before;
	uint64_t after;
	int have_after;

	/* A zero sleep does not sleep. */
	if (us == 0u)
		return;

	/* Samples the clock, sleeps, and counts the sleep. */
	before = 0;
	after = 0;
	(void)i915_dp_kernel_now_us(k, &before);
	kern_usleep_range(us, us);
	(void)__atomic_add_fetch(&k->tick_sleeps, 1u, __ATOMIC_SEQ_CST);

	/* Measures the sleep when the clock could be read around it. */
	have_after = i915_dp_kernel_now_us(k, &after);
	if (!have_after)
		return;
	if (after < before)
		return;

	/* Counts the time the sleep took. */
	(void)__atomic_add_fetch(&k->tick_slept_us, after - before, __ATOMIC_SEQ_CST);

	/* A sleep shorter than asked is a time-base fault. */
	if (after - before < us)
		k->time_faults++;
}

/*
 * Starts the eDP's locks and threads: the two mutexes, the worker and timer
 * threads, and the delayed VDD-off and asynchronous power put works.
 *
 * Returns 0, or the error of the thread creation; nothing is left started
 * on failure.
 */
int
drv_i915_dp_kernel_sync_start(
	struct i915_dp_kernel *k)
{
	int error;

	/* Creates the PPS and AUX mutexes. */
	(void)mutex_init(&k->locks[I915_DP_LOCK_PPS], LOCK_RANK_DEVICE, "i915-pps");
	(void)mutex_init(&k->locks[I915_DP_LOCK_AUX], LOCK_RANK_DEVICE, "i915-dp-aux");

	/*
	 * Opens the register window the AUX channels are driven through,
	 * on the same MMIO; the AUX mutex serializes its users.
	 */
	drv_i915_xd_io_init(&k->aux_io, k->mmio, NULL);

	/* Starts the worker thread. */
	error = drv_i915_workqueue_create(&k->wq, "i915-display-wq");
	if (error != 0)
		return error;

	/* Starts the timer thread that feeds the worker. */
	error = drv_i915_timer_queue_create(&k->tq, &k->wq, "i915-display-timer");
	if (error != 0) {
		drv_i915_workqueue_destroy(&k->wq);
		return error;
	}

	/* Prepares the delayed VDD-off and the asynchronous power put. */
	drv_i915_delayed_work_init(&k->vdd_off_work, i915_dp_kernel_vdd_off_body, k);
	drv_i915_delayed_work_init(&k->async_put_work, i915_dp_kernel_async_put_body, k);

	/* The threads exist until drv_i915_dp_kernel_sync_stop(). */
	k->sync_started = 1;

	/* Succeeded: the locks and threads are ready. */
	return 0;
}

/*
 * Stops the eDP's threads: both delayed works are cancelled and waited for,
 * then the timer and worker threads end.
 */
void
drv_i915_dp_kernel_sync_stop(
	struct i915_dp_kernel *k)
{
	/* Nothing to stop when nothing was started. */
	if (!k->sync_started)
		return;

	/* Cancels both works and waits for a running body. */
	(void)drv_i915_delayed_cancel_sync(&k->tq, &k->vdd_off_work, i915_dp_kernel_sync_deadline());
	(void)drv_i915_delayed_cancel_sync(&k->tq, &k->async_put_work, i915_dp_kernel_sync_deadline());

	/* Ends the timer thread, then the worker it feeds. */
	drv_i915_timer_queue_destroy(&k->tq);
	drv_i915_workqueue_destroy(&k->wq);

	/* The threads are gone. */
	k->sync_started = 0;
}

/*
 * Binds an environment's locks and delayed work to the eDP's kernel
 * backend.
 *
 * The register, time and power hooks are left as they are, so the
 * GPU-free tests can combine the real locks and threads with a register
 * model.
 */
void
drv_i915_dp_kernel_bind_sync(
	struct i915_dp_kernel *k,
	struct i915_dp_env *env)
{
	/* The backend is the hooks' context. */
	env->ctx = k;

	/* The real mutexes. */
	env->lock = i915_dp_kernel_lock;
	env->unlock = i915_dp_kernel_unlock;

	/* The timer and worker threads. */
	env->delayed_queue = i915_dp_kernel_delayed_queue;
	env->delayed_cancel = i915_dp_kernel_delayed_cancel;
	env->delayed_pending = i915_dp_kernel_delayed_pending;
}

/*
 * Binds every hook of an environment to the eDP's kernel backend: MMIO,
 * waits, sleeps, the clock, the locks and the delayed work.
 */
void
drv_i915_dp_kernel_bind(
	struct i915_dp_kernel *k,
	struct i915_dp_env *env)
{
	/* Starts from an environment with no hooks and no bookkeeping. */
	kern_memset(env, 0, sizeof(*env));

	/* The locks and the delayed work. */
	drv_i915_dp_kernel_bind_sync(k, env);

	/* The registers and their waits, and the AUX channels' window on the same MMIO. */
	env->read32 = i915_dp_kernel_read32;
	env->write32 = i915_dp_kernel_write32;
	env->wait_reg = i915_dp_kernel_wait_reg;
	env->aux_io = &k->aux_io;

	/* The sleeps and the clock. */
	env->sleep_us = i915_dp_kernel_sleep;
	env->now_ms = i915_dp_kernel_now_ms;
}

/*
 * Prepares the resident eDP device of a display: the MMIO, the display's
 * power, and the VBT the output setup reads.
 */
void
drv_i915_edp_device_prepare(
	struct i915_edp_device *dev,
	struct i915_mmio *mmio,
	struct i915_display_power *power,
	struct i915_vbt_state *vbt)
{
	/* Starts from a device that has not begun. */
	kern_memset(dev, 0, sizeof(*dev));

	/* Records what the eDP runs on. */
	dev->k.mmio = mmio;
	dev->k.power = power;
	dev->vbt = vbt;
}

/*
 * Starts the resident eDP device's locks and threads and binds the
 * environment and the asynchronous power put to them.
 *
 * The eDP connector starts them, and so do the external DP ports when no
 * eDP did (their AUX transfers use the same locks, threads and power).
 * A started device is left as it is.  Returns 0, or the error of the
 * thread creation.
 */
int
drv_i915_edp_device_start(
	struct i915_edp_device *dev)
{
	struct i915_xd_power_work work;
	int error;

	/* A started device keeps its threads. */
	if (dev->started)
		return 0;

	/* Starts the mutexes and threads. */
	error = drv_i915_dp_kernel_sync_start(&dev->k);
	if (error != 0)
		return error;

	/* Binds the display power's delayed give-back and every environment hook to them. */
	work.queue = i915_dp_kernel_pd_async_queue;
	work.cancel = i915_dp_kernel_pd_async_cancel;
	work.context = &dev->k;
	drv_i915_display_power_bind_work(dev->k.power, &work);
	drv_i915_dp_kernel_bind(&dev->k, &dev->env);

	/* The locks and threads exist until the device stops. */
	dev->started = 1;

	/* Succeeded: the device is started. */
	return 0;
}

/*
 * Builds the built-in panel's connector on a DisplayPort port, called by
 * the output setup for each such port.
 *
 * ctx is the display's panel device.  It returns 1 when the port is not
 * the VBT's eDP port (nothing done), 0 when the panel is taken and its
 * state kept in the device, and a negative errno (-I915_EDP_E*) when the
 * panel could not be taken (the output setup then drops the port).
 */
int
drv_i915_edp_device_init_connector(
	void *ctx,
	int port)
{
	struct i915_edp_device *dev;
	const struct i915_vbt_encoder *enc;
	struct i915_vbt_panel panel;
	int rc;
	int error;
	int have_panel;
	int panel_found;
	int panel_type;

	/* Without a real VBT the defaults carry no eDP child. */
	dev = ctx;
	if (dev == NULL)
		return 1;
	if (dev->vbt == NULL)
		return 1;
	if (!dev->vbt->parsed_live)
		return 1;
	if (!dev->vbt->vbt_found)
		return 1;

	/* Only the VBT's eDP port has a panel. */
	enc = drv_i915_vbt_encoder_for_port(&dev->vbt->parsed, port);
	if (enc == NULL)
		return 1;
	if (!enc->supports_edp)
		return 1;

	/* One eDP connector at a time. */
	if (dev->connector_live)
		return -I915_EDP_EBUSY;

	/* The locks and threads exist only on a machine that has the panel. */
	if (!dev->started) {
		error = drv_i915_edp_device_start(dev);
		if (error != 0) {
			kern_logf("i915: edp: worker/timer threads could not start rc=%d\n", error);
			return -I915_EDP_EIO;
		}
	}

	/* Takes the early panel data the VBT has before the EDID is known. */
	panel_found = drv_i915_vbt_init_panel(&dev->vbt->parsed, port, 0, &panel);
	have_panel = 0;
	if (panel_found == 0)
		have_panel = 1;

	/* The configuration from the VBT's panel. */
	i915_edp_cfg_from_panel(&dev->cfg, enc, &panel, have_panel);

	/* Reports the configuration the eDP begins with. */
	panel_type = -1;
	if (have_panel)
		panel_type = panel.panel_type;

	/* Logs the configuration. */
	kern_logf("i915: edp init_connector (in setup_outputs): vbt_source=%d port=%c aux_ch=%d "
		  "panel_early=%d type=%d pps(100us) t1_t3=%u t8=%u t9=%u t10=%u t11_t12=%u controller=%d well_refs=%u\n",
		  dev->vbt->source, 'A' + dev->cfg.port, dev->cfg.aux_ch, have_panel, panel_type,
		  dev->cfg.t1_t3, dev->cfg.t8, dev->cfg.t9, dev->cfg.t10, dev->cfg.t11_t12, dev->cfg.bl_controller,
		  i915_edp_well_refs(dev->k.power));

	/* Takes the panel's sink up to its EDID and reports what it found. */
	error = i915_edp_device_take(dev);
	rc = 0;
	if (error == ENXIO) {
		rc = -I915_EDP_ENXIO;
	} else if (error != 0) {
		rc = -I915_EDP_EIO;
	}

	/* Records and logs what the taking found. */
	dev->init_rc = rc;
	i915_edp_device_log_acquire(dev, error);

	/* A failed taking already let VDD go; the threads are kept for the stop. */
	if (rc != 0) {
		kern_logf("i915: edp init_connector FAILED (%d): eDP disabled, VDD off\n", error);
		return rc;
	}

	/* The late step: the EDID-based panel lookup, the final delays, VDD let go later. */
	i915_edp_device_late(dev, enc, port);

	/* The connector is kept for the later display stages. */
	dev->connector_live = 1;

	/* Plans the panel's mode and link from what its sink read (nothing is written). */
	i915_edp_device_link(dev);

	/* Succeeded: the eDP connector is initialized and kept. */
	return 0;
}

/*
 * Stops the resident eDP device: the delayed VDD-off is cancelled
 * synchronously and VDD forced off, the parked power references are
 * flushed, and the threads end.
 */
void
drv_i915_edp_device_fini(
	struct i915_edp_device *dev)
{
	struct i915_display_power *power;
	const struct i915_xd_power_stats *stats;
	int vdd_held;

	/* Nothing to stop when the device never started. */
	if (dev == NULL)
		return;
	if (!dev->started)
		return;

	/* The external DP ports stop first. */
	drv_i915_dp_ext_stop(container_of(dev, struct i915_display, edp_dev));

	/* Lets the panel's sink go; the delayed VDD-off is called off before the sequencer is held. */
	power = dev->k.power;
	if (dev->connector_live) {
		drv_i915_xd_edp_stop(&dev->xe);
		dev->connector_live = 0;
	}

	/* Gives the deferred references back and unbinds the delayed work. */
	drv_i915_display_power_flush(power);
	drv_i915_display_power_bind_work(power, NULL);
	stats = &power->xd->power.stats;

	/* Whether the sequencer still holds VDD after the stop. */
	vdd_held = 0;
	if (dev->xe.vdd_cookie != 0)
		vdd_held = 1;

	/* Reports what the stop left and what the works and the power layer did. */
	kern_logf("i915: edp fini: vdd held=%d | vdd-off work armed=%u fired=%u ran=%d cancelled(timer/queue)=%u/%u | "
		  "later puts=%u deferred=%u taken over=%u work runs=%u released=%u flushes=%u errors=%u | well_refs=%u\n",
		  vdd_held,
		  dev->k.vdd_off_work.armed_count, dev->k.vdd_off_work.fired_count, dev->k.vdd_off_work.work.ran_count,
		  dev->k.vdd_off_work.cancelled_armed, dev->k.vdd_off_work.cancelled_pending,
		  power->later_puts, stats->deferred, stats->taken_over, stats->work_runs, stats->released,
		  stats->flushes, stats->errors, i915_edp_well_refs(power));

	/* Ends the threads. */
	drv_i915_dp_kernel_sync_stop(&dev->k);
	dev->started = 0;
}

/*
 * Reads DPCD bytes of the panel's sink for the modeset.
 *
 * It is the dpcd_read hook of struct i915_lcd_emit; ctx is the panel run
 * (struct i915_lcd_kernel).  It returns the bytes transferred, or a negative
 * errno (-I915_EDP_EINVAL without a taken sink).
 */
long
drv_i915_edp_emit_dpcd_read(
	void *ctx,
	unsigned offset,
	uint8_t *buf,
	size_t size)
{
	struct i915_xd_edp *xe;
	int error;

	/* The panel's sink of the run's display. */
	xe = i915_edp_emit_sink(ctx);

	/* Reads the bytes. */
	error = drv_i915_xd_edp_dpcd_read(xe, offset, buf, (unsigned)size);
	if (error != 0)
		return (long)i915_edp_emit_error(error);

	/* Succeeded: reports the bytes read. */
	return (long)size;
}

/*
 * Writes DPCD bytes of the panel's sink for the modeset.
 *
 * It is the dpcd_write hook of struct i915_lcd_emit, as
 * drv_i915_edp_emit_dpcd_read().
 */
long
drv_i915_edp_emit_dpcd_write(
	void *ctx,
	unsigned offset,
	const uint8_t *buf,
	size_t size)
{
	struct i915_xd_edp *xe;
	int error;

	/* The panel's sink of the run's display. */
	xe = i915_edp_emit_sink(ctx);

	/* Writes the bytes. */
	error = drv_i915_xd_edp_dpcd_write(xe, offset, buf, (unsigned)size);
	if (error != 0)
		return (long)i915_edp_emit_error(error);

	/* Succeeded: reports the bytes written. */
	return (long)size;
}

/*
 * Reads the receiver capabilities of the panel's sink for the modeset.
 *
 * It is the read_dpcd_caps hook of struct i915_lcd_emit; it returns 0, or a
 * negative errno (-I915_EDP_E*).
 */
int
drv_i915_edp_emit_read_dpcd_caps(
	void *ctx,
	uint8_t dpcd[15])
{
	struct i915_xd_edp *xe;
	int error;

	/* The panel's sink of the run's display. */
	xe = i915_edp_emit_sink(ctx);

	/* Reads the capabilities. */
	error = drv_i915_xd_edp_read_caps(xe, dpcd);
	if (error != 0)
		return i915_edp_emit_error(error);

	/* Succeeded: the capabilities are in dpcd. */
	return 0;
}

/*
 * Runs one panel power operation of the modeset hooks on the panel's sink.
 *
 * It is the panel hook of the modeset's register and panel hooks (struct
 * i915_lcd_emit): ctx is the panel run (struct i915_lcd_kernel), and op an
 * enum i915_lcd_panel_op.  It returns 0, or -I915_EDP_EINVAL without a live
 * eDP or for an unknown operation.
 */
int
drv_i915_edp_emit_panel(
	void *ctx,
	int op)
{
	struct i915_lcd_kernel *kernel;
	unsigned step;
	int error;

	/* The step of the panel's sink the operation names. */
	kernel = ctx;
	switch (op) {
	case I915_LCD_PANEL_ON:
		step = I915_XD_EDP_PANEL_ON;
		break;
	case I915_LCD_PANEL_OFF:
		step = I915_XD_EDP_PANEL_OFF;
		break;
	case I915_LCD_PANEL_VDD_ON:
		step = I915_XD_EDP_PANEL_VDD_ON;
		break;
	case I915_LCD_PANEL_VDD_OFF_SYNC:
		step = I915_XD_EDP_PANEL_VDD_OFF;
		break;
	case I915_LCD_PANEL_BACKLIGHT_ON:
		step = I915_XD_EDP_BACKLIGHT_ON;
		break;
	case I915_LCD_PANEL_BACKLIGHT_OFF:
		step = I915_XD_EDP_BACKLIGHT_OFF;
		break;
	default:
		return -I915_EDP_EINVAL;
	}

	/* Runs it on the display's panel. */
	error = drv_i915_xd_edp_panel(&kernel->d->edp->xe, step);
	if (error != 0)
		return -I915_EDP_EINVAL;

	/* Succeeded: the step ran. */
	return 0;
}

/* Finds the panel's sink of a panel run's display, for the modeset hooks. */
static struct i915_xd_edp *
i915_edp_emit_sink(
	void *ctx)
{
	struct i915_lcd_kernel *kernel;

	/* The hook context is the panel run; its panel is the display's eDP device. */
	kernel = ctx;

	/* Succeeded: the device's sink. */
	return &kernel->d->edp->xe;
}

/* Turns an error of the panel's sink into the negative errno (-I915_EDP_E*) of the run's hooks. */
static int
i915_edp_emit_error(
	int error)
{
	/* The errors the hooks' callers tell apart. */
	if (error == EINVAL)
		return -I915_EDP_EINVAL;
	if (error == ETIMEDOUT)
		return -I915_EDP_ETIMEDOUT;
	if (error == EBUSY)
		return -I915_EDP_EBUSY;

	/* Any other failure of the channel. */
	return -I915_EDP_EIO;
}

/* Reads a display register for the environment. */
static uint32_t
i915_dp_kernel_read32(
	void *ctx,
	uint32_t reg)
{
	struct i915_dp_kernel *kernel;
	uint32_t value;

	/* Reads through the held MMIO. */
	kernel = ctx;
	value = drv_i915_read32(kernel->mmio, reg);

	/* Succeeded: reports the register value. */
	return value;
}

/* Writes a display register for the environment. */
static void
i915_dp_kernel_write32(
	void *ctx,
	uint32_t reg,
	uint32_t value)
{
	struct i915_dp_kernel *kernel;

	/* Writes through the held MMIO. */
	kernel = ctx;
	drv_i915_write32(kernel->mmio, reg, value);
}

/*
 * Waits for a register for the environment.
 *
 * It returns 0, -I915_EDP_ETIMEDOUT for a register that did not reach the
 * value, or -I915_EDP_EIO for a fault of the time base, which is not a
 * timeout; both are counted.
 */
static int
i915_dp_kernel_wait_reg(
	void *ctx,
	uint32_t reg,
	uint32_t mask,
	uint32_t value,
	unsigned fast_us,
	unsigned slow_ms,
	uint32_t *out)
{
	struct i915_dp_kernel *kernel;
	int error;

	/* Waits: a fast busy stage, then a sleeping one. */
	kernel = ctx;
	error = drv_i915_wait_reg(kernel->mmio, reg, mask, value, fast_us, slow_ms, out);
	if (error == 0)
		return 0;

	/* Anything but a timeout is a fault of the time base. */
	if (error != ETIMEDOUT) {
		kernel->time_faults++;
		return -I915_EDP_EIO;
	}

	/* Reports and counts the timeout. */
	kernel->wait_timeouts++;
	return -I915_EDP_ETIMEDOUT;
}

/*
 * Reads the monotonic counter in microseconds.
 *
 * It returns 1, or 0 with the fault counted when the counter cannot be
 * read.
 */
static int
i915_dp_kernel_now_us(
	struct i915_dp_kernel *kernel,
	uint64_t *us)
{
	uint64_t counter;
	uint64_t freq;
	bool available;

	/* Reads the counter and its frequency. */
	counter = 0;
	freq = 0;
	available = kern_rtc_read_counter(&counter, &freq);
	if (!available || freq == 0u) {
		kernel->time_faults++;
		return 0;
	}

	/* Converts the counter to microseconds without overflowing. */
	*us = (counter / freq) * 1000000u + ((counter % freq) * 1000000u) / freq;

	/* Succeeded: *us holds the time. */
	return 1;
}

/* Sleeps for the environment. */
static void
i915_dp_kernel_sleep(
	void *ctx,
	unsigned us)
{
	/* Sleeps with the kernel's ordinary sleep. */
	drv_i915_dp_kernel_sleep_us(ctx, us);
}

/*
 * Reports the time in milliseconds for the environment.
 *
 * When the counter cannot be read, the last time read is reported again.
 */
static uint64_t
i915_dp_kernel_now_ms(
	void *ctx)
{
	struct i915_dp_kernel *kernel;
	uint64_t us;
	int available;

	/* Reads the counter and keeps the time. */
	kernel = ctx;
	us = 0;
	available = i915_dp_kernel_now_us(kernel, &us);
	if (available)
		kernel->last_ms = us / 1000u;

	/* Succeeded: reports the latest time read. */
	return kernel->last_ms;
}

/* Takes one of the eDP's mutexes for the environment (I915_DP_LOCK_*). */
static void
i915_dp_kernel_lock(
	void *ctx,
	int which)
{
	struct i915_dp_kernel *kernel;

	/* Blocks on the real mutex. */
	kernel = ctx;
	mutex_lock(&kernel->locks[which]);
}

/* Drops one of the eDP's mutexes for the environment. */
static void
i915_dp_kernel_unlock(
	void *ctx,
	int which)
{
	struct i915_dp_kernel *kernel;

	/* Releases the real mutex. */
	kernel = ctx;
	mutex_unlock(&kernel->locks[which]);
}

/*
 * Reports the deadline for waiting on a running work body.
 *
 * Ten seconds bound it: the longest wait a body makes is the 5 s
 * panel-status timeout.
 */
static uint64_t
i915_dp_kernel_sync_deadline(void)
{
	uint64_t deadline;
	uint64_t now;

	/* Ten seconds from the current tick. */
	deadline = 0;
	now = sched_ticks();
	(void)kern_deadline_after(now, kern_ms_to_ticks(10000U), &deadline);

	/* Succeeded: reports the deadline. */
	return deadline;
}

/* Arms the delayed VDD-off for the environment; reports 1 when newly armed. */
static int
i915_dp_kernel_delayed_queue(
	void *ctx,
	int which,
	unsigned delay_ms)
{
	struct i915_dp_kernel *kernel;
	int queued;

	UNUSED_PARAMETER(which);

	/* Arms the one delayed work of the environment. */
	kernel = ctx;
	queued = drv_i915_delayed_queue(&kernel->tq, &kernel->vdd_off_work, delay_ms);

	/* Succeeded: reports whether the work was newly armed. */
	return queued;
}

/*
 * Cancels the delayed VDD-off for the environment; with sync it also waits
 * for a running body.  Reports 1 when it was pending.
 */
static int
i915_dp_kernel_delayed_cancel(
	void *ctx,
	int which,
	int sync)
{
	struct i915_dp_kernel *kernel;
	int cancelled;

	UNUSED_PARAMETER(which);

	/* Cancels, waiting for a running body when asked. */
	kernel = ctx;
	if (sync) {
		cancelled = drv_i915_delayed_cancel_sync(&kernel->tq, &kernel->vdd_off_work, i915_dp_kernel_sync_deadline());
	} else {
		cancelled = drv_i915_delayed_cancel(&kernel->tq, &kernel->vdd_off_work);
	}

	/* Succeeded: reports whether the work was pending. */
	return cancelled;
}

/* Tells the environment whether the delayed VDD-off is armed or pending. */
static int
i915_dp_kernel_delayed_pending(
	void *ctx,
	int which)
{
	struct i915_dp_kernel *kernel;
	int pending;

	UNUSED_PARAMETER(which);

	/* Asks the timer queue. */
	kernel = ctx;
	pending = drv_i915_delayed_pending(&kernel->tq, &kernel->vdd_off_work);

	/* Succeeded: reports the state. */
	return pending;
}

/* Runs the delayed VDD-off on the worker thread. */
static void
i915_dp_kernel_vdd_off_body(
	void *arg)
{
	struct i915_dp_kernel *kernel;
	struct i915_edp_device *dev;

	/* The backend is the kernel part of the display's eDP device. */
	kernel = arg;
	dev = container_of(kernel, struct i915_edp_device, k);

	/* Runs the body on the panel's sink, when it is taken. */
	if (dev->connector_live)
		drv_i915_xd_edp_vdd_work(&dev->xe);
}

/* Runs the asynchronous power put on the worker thread. */
static void
i915_dp_kernel_async_put_body(
	void *arg)
{
	struct i915_dp_kernel *kernel;

	/* Gives back the references owed to this run. */
	kernel = arg;
	drv_i915_display_power_work(kernel->power);
}

/* Arms the display power's delayed give-back; reports 1 when newly armed. */
static int
i915_dp_kernel_pd_async_queue(
	void *ctx,
	int delay_ms)
{
	struct i915_dp_kernel *kernel;
	int queued;

	/* Arms the work on the eDP's timer queue. */
	kernel = ctx;
	queued = drv_i915_delayed_queue(&kernel->tq, &kernel->async_put_work, (unsigned)delay_ms);

	/* Succeeded: reports whether the work was newly armed. */
	return queued;
}

/* Cancels the display power's delayed give-back; with sync it also waits for a running body. */
static int
i915_dp_kernel_pd_async_cancel(
	void *ctx,
	int sync)
{
	struct i915_dp_kernel *kernel;
	int cancelled;

	/* Cancels, waiting for a running body when asked. */
	kernel = ctx;
	if (sync) {
		cancelled = drv_i915_delayed_cancel_sync(&kernel->tq, &kernel->async_put_work, i915_dp_kernel_sync_deadline());
	} else {
		cancelled = drv_i915_delayed_cancel(&kernel->tq, &kernel->async_put_work);
	}

	/* Succeeded: reports whether the work was pending. */
	return cancelled;
}

/* Logs bytes 16 to a line, as hex. */
static void
i915_edp_log_bytes(
	const char *tag,
	const uint8_t *bytes,
	unsigned count)
{
	unsigned i;

	/* Writes each whole line of 16 bytes. */
	for (i = 0; i + 16u <= count; i += 16u) {
		kern_logf("i915: edp %s +%03x: %02x %02x %02x %02x %02x %02x %02x %02x "
			  "%02x %02x %02x %02x %02x %02x %02x %02x\n", tag, i,
			  bytes[i], bytes[i + 1], bytes[i + 2], bytes[i + 3], bytes[i + 4], bytes[i + 5], bytes[i + 6], bytes[i + 7],
			  bytes[i + 8], bytes[i + 9], bytes[i + 10], bytes[i + 11], bytes[i + 12], bytes[i + 13], bytes[i + 14], bytes[i + 15]);
	}
}

/*
 * Fills the eDP configuration from the VBT's encoder and, when known, its
 * panel.
 */
static void
i915_edp_cfg_from_panel(
	struct i915_edp_config *cfg,
	const struct i915_vbt_encoder *enc,
	const struct i915_vbt_panel *panel,
	int have_panel)
{
	/* Starts from an empty configuration. */
	kern_memset(cfg, 0, sizeof(*cfg));

	/* The port and AUX channel the VBT names. */
	cfg->port = enc->port;
	cfg->aux_ch = enc->aux_ch;

	/*
	 * The raw clock: only a sequencer with a PP_DIVISOR register reads it,
	 * which the south displays this driver drives do not have.  19200 kHz
	 * is the raw clock of a 19.2 MHz reference.
	 */
	cfg->rawclk_khz = 19200u;

	/* No controller unless the panel names one. */
	cfg->bl_controller = -1;

	/* The panel's power sequence and controller. */
	if (have_panel) {
		cfg->t1_t3 = panel->t1_t3;
		cfg->t8 = panel->t8;
		cfg->t9 = panel->t9;
		cfg->t10 = panel->t10;
		cfg->t11_t12 = panel->t11_t12;
		cfg->bl_controller = panel->bl_controller;
	}

	/* Errors and information are shown. */
	cfg->log_level = 1;
}

/* Reports the users all power wells have. */
static unsigned
i915_edp_well_refs(
	const struct i915_display_power *power)
{
	unsigned well;
	unsigned users;

	/* Adds up every well's users. */
	users = 0U;
	for (well = 0U; well < I915_XD_POWER_MAX_WELLS; well++)
		users += power->xd->power.refs[well];

	/* Succeeded: the sum. */
	return users;
}

/* Logs what the taking of the panel's sink up to the EDID found. */
static void
i915_edp_device_log_acquire(
	struct i915_edp_device *dev,
	int error)
{
	struct i915_edp_result *res;
	uint8_t digest[32];

	/* The sequencer and its delays. */
	res = &dev->res;
	kern_logf("i915: edp pps: idx=%d valid=%d delays(ms) up=%d down=%d cycle=%d bl_on=%d bl_off=%d\n",
		  res->pps_idx, res->pps_valid, res->delay_power_up_ms, res->delay_power_down_ms,
		  res->delay_power_cycle_ms, res->delay_bl_on_ms, res->delay_bl_off_ms);

	/* The stage reached, the time and the sleeps. */
	kern_logf("i915: edp acquire: error=%d stage=%u dpcd_ok=%d edp_dpcd_ok=%d edid_ok=%d edid_blocks=%u ext=%u "
		  "time_faults=%u | sleeps: tick=%u (%llu us)\n",
		  error, dev->xe.stage, res->dpcd_ok, res->edp_dpcd_ok, res->edid_ok, res->edid_blocks, res->edid_extensions,
		  dev->k.time_faults, dev->k.tick_sleeps, (unsigned long long)dev->k.tick_slept_us);

	/* The receiver and eDP capabilities, when read. */
	if (res->dpcd_ok) {
		kern_logf("i915: edp DPCD 000: %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x "
			  "%02x %02x %02x %02x %02x | eDP 700: %02x %02x %02x (ok=%d)\n",
			  res->dpcd[0], res->dpcd[1], res->dpcd[2], res->dpcd[3], res->dpcd[4], res->dpcd[5],
			  res->dpcd[6], res->dpcd[7], res->dpcd[8], res->dpcd[9], res->dpcd[10], res->dpcd[11],
			  res->dpcd[12], res->dpcd[13], res->dpcd[14], res->edp_dpcd[0], res->edp_dpcd[1],
			  res->edp_dpcd[2], res->edp_dpcd_ok);
	}

	/* The EDID bytes, their digest, the manufacturer and the product, when read. */
	if (res->edid_blocks != 0u) {
		i915_edp_log_bytes("EDID", res->edid, 128u * res->edid_blocks);
		drv_i915_sha256(res->edid, 128u * res->edid_blocks, digest);
		kern_logf("i915: edp EDID sha256=%02x%02x%02x%02x%02x%02x%02x%02x.. mfg=%02x%02x "
			  "product=%02x%02x\n", digest[0], digest[1], digest[2], digest[3], digest[4], digest[5],
			  digest[6], digest[7], res->edid[8], res->edid[9], res->edid[11], res->edid[10]);
	}
}

/*
 * Runs the late step: the panel looked up again with its EDID, the
 * sequencer finished with the panel's final VBT delays, VDD let go a
 * while later.
 */
static void
i915_edp_device_late(
	struct i915_edp_device *dev,
	const struct i915_vbt_encoder *enc,
	int port)
{
	struct i915_vbt_panel late_panel_data;
	struct i915_xd_pps_delays delays;
	int late_panel;
	int panel_found;
	int panel_type;
	int vdd_held;

	/* Looks the panel up again, now with its EDID. */
	panel_found = drv_i915_vbt_init_panel(&dev->vbt->parsed, port, dev->res.edid, &late_panel_data);
	late_panel = 0;
	if (panel_found == 0)
		late_panel = 1;

	/* Takes the final configuration and the VBT colour depth. */
	i915_edp_cfg_from_panel(&dev->cfg, enc, &late_panel_data, late_panel);
	dev->vbt_bpp = 0;
	if (late_panel)
		dev->vbt_bpp = late_panel_data.bpp;

	/* Finishes the sequencer with the final delays. */
	i915_edp_cfg_delays(&dev->cfg, &delays);
	drv_i915_xd_edp_late(&dev->xe, dev->cfg.bl_controller, &delays);
	dev->late_rc = 0;
	i915_edp_device_result(dev);

	/* Reports the final delays and what is kept on. */
	panel_type = -1;
	if (late_panel)
		panel_type = late_panel_data.panel_type;

	/* Whether VDD is still held, to be let go after five power cycles. */
	vdd_held = 0;
	if (dev->xe.vdd_cookie != 0)
		vdd_held = 1;

	/* Logs what the late step found. */
	kern_logf("i915: edp late: panel_late=%d type=%d pps=%u delays(ms) up=%d down=%d cycle=%d | VDD held=%d, off in %d ms\n",
		  late_panel, panel_type, dev->xe.pps, dev->res.delay_power_up_ms, dev->res.delay_power_down_ms,
		  dev->res.delay_power_cycle_ms, vdd_held, dev->res.delay_power_cycle_ms * 5);
}

/*
 * Takes the panel's sink with the xd code: its environment on the
 * device's locks, threads and power, the sequencer's choice from the
 * VBT's early panel data and the south display, and what it read copied
 * into the device's result for the later display stages.  Returns 0 or
 * the xd sink's error.
 */
static int
i915_edp_device_take(
	struct i915_edp_device *dev)
{
	struct i915_xd_edp_config config;
	struct i915_xd_edp_env env;
	struct i915_display *display;
	int pch_kind;
	int error;

	/* The sink's environment, on this device. */
	display = container_of(dev, struct i915_display, edp_dev);
	kern_memset(&env, 0, sizeof(env));
	env.now_ms = i915_edp_xe_now;
	env.power_get = i915_edp_xe_power_get;
	env.power_put = i915_edp_xe_power_put;
	env.power_put_later = i915_edp_xe_power_put_later;
	env.lock = i915_edp_xe_lock;
	env.vdd_later = i915_edp_xe_vdd_later;
	env.vdd_cancel = i915_edp_xe_vdd_cancel;
	env.error = i915_edp_xe_error;
	env.context = dev;
	drv_i915_xd_edp_init(&dev->xe, &dev->k.aux_io, display->xd.choice.platform, &env);

	/* The panel's channel, the south display's sequencers and the VBT's early delays. */
	kern_memset(&config, 0, sizeof(config));
	config.aux_channel = (unsigned)dev->cfg.aux_ch;
	config.sequencers = 1U;
	config.muxed = 0;
	pch_kind = display->xd.choice.pch_kind;
	if (pch_kind >= I915_PCH_ICP && pch_kind <= I915_PCH_ADP) {
		config.sequencers = 2U;
		config.muxed = 1;
	}

	/* The VBT's backlight controller and power sequence delays. */
	config.controller = dev->cfg.bl_controller;
	i915_edp_cfg_delays(&dev->cfg, &config.vbt);
	config.long_power_cycle = 0;

	/* The taking, and what it read. */
	error = drv_i915_xd_edp_start(&dev->xe, &config);
	i915_edp_device_result(dev);
	if (error != 0)
		return error;

	/* Succeeded: the sink is taken with VDD on. */
	return 0;
}

/* Copies the VBT's panel delays of a configuration into the xd sequencer's form. */
static void
i915_edp_cfg_delays(
	const struct i915_edp_config *cfg,
	struct i915_xd_pps_delays *delays)
{
	/* The same delays in the same units. */
	delays->power_up = cfg->t1_t3;
	delays->backlight_on = cfg->t8;
	delays->backlight_off = cfg->t9;
	delays->power_down = cfg->t10;
	delays->power_cycle = cfg->t11_t12;
}

/* Copies what the xd sink read and chose into the device's result, which the later stages read. */
static void
i915_edp_device_result(
	struct i915_edp_device *dev)
{
	struct i915_edp_result *res;
	const struct i915_xd_edp *xe;
	unsigned bytes;

	/* The record of the result, cleared, and the sink it is read from. */
	res = &dev->res;
	xe = &dev->xe;
	kern_memset(res, 0, sizeof(*res));

	/* The sequencer and its waits. */
	res->stage = (int)xe->stage;
	res->pps_idx = (int)xe->pps;
	res->pps_valid = 0;
	if (xe->stage >= I915_XD_EDP_STAGE_PPS)
		res->pps_valid = 1;
	res->delay_power_up_ms = (int)xe->waits.power_up_ms;
	res->delay_power_down_ms = (int)xe->waits.power_down_ms;
	res->delay_power_cycle_ms = (int)xe->waits.power_cycle_ms;
	res->delay_bl_on_ms = (int)xe->waits.backlight_on_ms;
	res->delay_bl_off_ms = (int)xe->waits.backlight_off_ms;

	/* The capabilities. */
	if (xe->stage >= I915_XD_EDP_STAGE_CAPS) {
		kern_memcpy(res->dpcd, xe->caps, sizeof(res->dpcd));
		res->dpcd_ok = 1;
	}

	/* The eDP capabilities, when they were read. */
	if (xe->edp_caps_read) {
		kern_memcpy(res->edp_dpcd, xe->edp_caps, sizeof(res->edp_dpcd));
		res->edp_dpcd_ok = 1;
	}

	/* The EDID, as many blocks as both hold. */
	res->edid_blocks = xe->edid_blocks;
	if (res->edid_blocks > I915_EDP_MAX_EDID_BLOCKS)
		res->edid_blocks = I915_EDP_MAX_EDID_BLOCKS;
	bytes = 128u * res->edid_blocks;
	kern_memcpy(res->edid, xe->edid, bytes);
	res->edid_extensions = xe->edid_extensions;
	res->edid_ok = 0;
	if (res->edid_blocks != 0u &&
	    (res->edid_blocks == 1u + res->edid_extensions ||
	     res->edid_blocks == I915_EDP_MAX_EDID_BLOCKS))
		res->edid_ok = 1;
}

/* Reads the clock of the panel's sink, in milliseconds. */
static uint64_t
i915_edp_xe_now(
	void *context)
{
	struct i915_edp_device *dev;
	uint64_t now;

	/* The device's clock. */
	dev = context;
	now = i915_dp_kernel_now_ms(&dev->k);

	/* Succeeded: the time. */
	return now;
}

/*
 * Returns the power functions a use of the panel's sink needs: none for
 * the display core (no switched well powers it), the AUX channel's for
 * the channel.
 */
static uint64_t
i915_edp_xe_functions(
	struct i915_edp_device *dev,
	unsigned use)
{
	struct i915_display *display;
	uint64_t functions;

	/* The display core needs no well. */
	if (use == I915_XD_EDP_POWER_CORE)
		return 0ULL;

	/* The AUX channel's well on the display's version. */
	display = container_of(dev, struct i915_display, edp_dev);
	functions = drv_i915_display_power_aux(display->xd.choice.platform->display_ver, (int)dev->cfg.aux_ch, 0);

	/* Succeeded: the channel's functions. */
	return functions;
}

/* Takes a power reference for a use of the panel's sink: 1, or 0 when it could not. */
static int
i915_edp_xe_power_get(
	void *context,
	unsigned use)
{
	struct i915_edp_device *dev;
	uint64_t functions;
	int error;

	/* The use's wells, through the display's power. */
	dev = context;
	functions = i915_edp_xe_functions(dev, use);
	error = drv_i915_display_power_get(dev->k.power, functions);
	if (error != 0)
		return 0;

	/* Succeeded: the reference is held. */
	return 1;
}

/* Gives a power reference of the panel's sink back. */
static void
i915_edp_xe_power_put(
	void *context,
	unsigned use,
	int cookie)
{
	struct i915_edp_device *dev;
	uint64_t functions;

	UNUSED_PARAMETER(cookie);

	/* The use's wells, through the display's power. */
	dev = context;
	functions = i915_edp_xe_functions(dev, use);
	drv_i915_display_power_put(dev->k.power, functions);
}

/* Lets a power reference of the panel's sink go after the power layer's delay. */
static void
i915_edp_xe_power_put_later(
	void *context,
	unsigned use,
	int cookie)
{
	struct i915_edp_device *dev;
	uint64_t functions;

	UNUSED_PARAMETER(cookie);

	/* The use's wells, given back after the default delay. */
	dev = context;
	functions = i915_edp_xe_functions(dev, use);
	drv_i915_display_power_put_later(dev->k.power, functions, -1);
}

/* Takes or drops one of the panel's sink's locks: the sequencer's or the AUX channel's mutex. */
static void
i915_edp_xe_lock(
	void *context,
	unsigned lock,
	int taken)
{
	struct i915_edp_device *dev;
	int which;

	/* The device's mutex of the lock. */
	dev = context;
	which = I915_DP_LOCK_PPS;
	if (lock == I915_XD_EDP_LOCK_AUX)
		which = I915_DP_LOCK_AUX;

	/* Takes or drops it. */
	if (taken) {
		mutex_lock(&dev->k.locks[which]);
	} else {
		mutex_unlock(&dev->k.locks[which]);
	}
}

/* Arms the delayed VDD-off of the panel's sink on the device's timer. */
static void
i915_edp_xe_vdd_later(
	void *context,
	unsigned delay_ms)
{
	struct i915_edp_device *dev;

	/* The device's delayed work. */
	dev = context;
	(void)drv_i915_delayed_queue(&dev->k.tq, &dev->k.vdd_off_work, delay_ms);
}

/* Disarms the delayed VDD-off of the panel's sink, waiting for a running one when asked. */
static void
i915_edp_xe_vdd_cancel(
	void *context,
	int wait)
{
	struct i915_edp_device *dev;

	/* The device's delayed work. */
	dev = context;
	if (wait) {
		(void)drv_i915_delayed_cancel_sync(&dev->k.tq, &dev->k.vdd_off_work, i915_dp_kernel_sync_deadline());
	} else {
		(void)drv_i915_delayed_cancel(&dev->k.tq, &dev->k.vdd_off_work);
	}
}

/* Logs a note of the panel's sink. */
static void
i915_edp_xe_error(
	void *context,
	const char *text)
{
	UNUSED_PARAMETER(context);

	/* One line. */
	kern_logf("i915: %s", text);
}

/*
 * Plans the panel's mode and link from what its sink read: the first
 * detailed timing of the EDID, the colour depth an EDID 1.4 digital input
 * states, and the link and the combo PLL's words the screen's plan chooses
 * from the sink's rate and lanes, its eDP revision and the VBT's colour
 * depth.  The PLLs' reference is the CD clock's.  Only DDI_BUF_CTL of
 * port A is read; nothing is written.
 */
static void
i915_edp_device_link(
	struct i915_edp_device *dev)
{
	struct i915_xd_plan_input input;
	struct i915_xd_edid decoded;
	struct i915_xd_plan plan;
	struct i915_display *display;
	struct i915_edp_result *res;
	unsigned refused;
	unsigned index;
	uint32_t ref;
	int error;

	/* The display, the result, and the mode and link cleared. */
	display = container_of(dev, struct i915_display, edp_dev);
	res = &dev->res;
	kern_memset(&dev->mode, 0, sizeof(dev->mode));
	kern_memset(&dev->link, 0, sizeof(dev->link));

	/* The PLLs' reference clock from the CD clock's readout, when known. */
	ref = 0U;
	if (dev->k.power != NULL && dev->k.power->cdclk != NULL)
		ref = dev->k.power->cdclk->hw.ref;

	/* The mode: the first detailed timing of the EDID's base block. */
	index = 0U;
	error = drv_i915_xd_edid_first_timing(res->edid, &dev->mode, &index);
	if (error != 0) {
		dev->link_rc = error;
		kern_logf("i915: edp link: the EDID has no timing the display drives (%d)\n", error);
		return;
	}

	/* The colour depth an EDID 1.4 digital input states (0: not said). */
	kern_memset(&decoded, 0, sizeof(decoded));
	error = drv_i915_xd_edid_decode(res->edid, 128U, &decoded);
	if (error == 0)
		dev->mode.edid_bpc = decoded.bpc;

	/* The link, its M/N and the combo PLL through the screen's plan. */
	kern_memset(&input, 0, sizeof(input));
	input.kind = I915_XD_SCREEN_PANEL;
	input.ddi = 0U;
	input.mode = &dev->mode;
	input.sink.rate_code = res->dpcd[1];
	input.sink.max_lanes = res->dpcd[2] & 0x1fU;
	input.sink.edp_revision = res->edp_dpcd[0];
	if (dev->vbt_bpp > 0)
		input.sink.vbt_bpp = (unsigned)dev->vbt_bpp;
	input.platform = display->xd.choice.platform;
	input.ref_khz = ref;
	refused = drv_i915_xd_screen_plan_link(&input, &plan);

	/* The link as the plan found it. */
	dev->link.rate_khz = plan.rate_khz;
	dev->link.lanes = plan.lanes;
	dev->link.bpp = plan.bpp;
	dev->link.tu = plan.m_n.tu;
	dev->link.data_m = plan.m_n.data_m;
	dev->link.data_n = plan.m_n.data_n;
	dev->link.link_m = plan.m_n.link_m;
	dev->link.link_n = plan.m_n.link_n;
	dev->link.ref_khz = ref;
	if (plan.pll_set) {
		dev->link.cfgcr0 = plan.pll.cfgcr0;
		dev->link.cfgcr1 = plan.pll.cfgcr1;
		dev->link.div0 = plan.pll.div0;
	}

	/* A sink the plan does not drive, a link that does not carry the mode, a rate without PLL words. */
	dev->link_rc = 0;
	if (refused == I915_XD_PLAN_SINK) {
		dev->link_rc = EINVAL;
	} else if (refused == I915_XD_PLAN_LINK) {
		dev->link_rc = ENOSPC;
	} else if (refused != I915_XD_PLAN_OK) {
		dev->link_rc = EINVAL;
	}

	/* The port reversal the firmware left in port A's DDI_BUF_CTL, which the panel keeps. */
	dev->ddi_buf_ctl_readout = dev->env.read32(dev->env.ctx, I915_EDP_DDI_BUF_CTL_A);

	/* Reports the plan. */
	kern_logf("i915: edp link: rc=%d mode %ux%u clock=%d kHz h %u/%u/%u/%u v %u/%u/%u/%u bpc=%d | link %u kHz x%u bpp=%u need=%u carry=%u kBps | TU=%u data M/N 0x%x/0x%x link M/N %u/%u | DPLL ref=%u kHz CFGCR0=0x%08x CFGCR1=0x%08x DIV0=0x%x | DDI_BUF_CTL_A 0x%08x\n",
	    dev->link_rc,
	    dev->mode.hdisplay,
	    dev->mode.vdisplay,
	    dev->mode.clock_khz,
	    dev->mode.hdisplay,
	    dev->mode.hsync_start,
	    dev->mode.hsync_end,
	    dev->mode.htotal,
	    dev->mode.vdisplay,
	    dev->mode.vsync_start,
	    dev->mode.vsync_end,
	    dev->mode.vtotal,
	    dev->mode.edid_bpc,
	    dev->link.rate_khz,
	    dev->link.lanes,
	    dev->link.bpp,
	    plan.need_kbps,
	    plan.carry_kbps,
	    dev->link.tu,
	    dev->link.data_m,
	    dev->link.data_n,
	    dev->link.link_m,
	    dev->link.link_n,
	    dev->link.ref_khz,
	    dev->link.cfgcr0,
	    dev->link.cfgcr1,
	    dev->link.div0,
	    dev->ddi_buf_ctl_readout);
}
