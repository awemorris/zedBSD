/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The run log of the display's outputs (see run-log.h).
 *
 * The recorder stacked on an output run's hooks forwards every operation
 * and logs it in order with what the backend answered: which phase, which
 * write, what a wait returned, the first anomaly.  It never replays
 * anything.  The run's observer owns the pipe's underrun status for the
 * run: it keeps the value found before the run, clears the status when the
 * run arms the underrun report, and at every sample records the bits
 * first and clears them after, so no occurrence is lost and the start,
 * stop and steady periods are told apart.  "Frames advance" is decided
 * from the hardware frame counter alone.
 */

#include "internal.h"

#include <kern/kcrt.h>

/* The backlight PWM registers and the DDI / transcoder registers the table names. */
#include "../intel/display-regs-xd.h"

#include "run-log.h"

#include <kern/clock.h>
#include <kern/klog.h>

#include <uapi/errno.h>
#include <stddef.h>
#include <stdint.h>

/* The step name prefix that marks a callee deliberately not connected. */
#define I915_TRACE_DECIDED_PREFIX	"(decided) "
#define I915_TRACE_DECIDED_PREFIX_LEN	10

/* The largest repeat count one read entry holds. */
#define I915_TRACE_READ_REPEAT_MAX	0xffffu

/* How long the observer lets the stopped pipe run before reading its frame counter again: three frames at 60 Hz. */
#define I915_OBSERVER_STOP_SETTLE_US	50000u

/* The observer's poll step while it waits for frames, in microseconds and in milliseconds. */
#define I915_OBSERVER_POLL_US		10000u
#define I915_OBSERVER_POLL_MS		10u

/*
 * The names of the run log's entry kinds, indexed by enum
 * i915_lcd_trace_kind (0 is not a kind).
 *
 * The table never changes.
 */
static const char *const i915_trace_kind_names[] = {
	"?", "write", "rmw", "read", "wait", "dpcd-w", "dpcd-r", "panel", "pw-get", "pw-put",
	"STEP", "ERROR", "PHASE", "decide", "dbuf", "observe"
};

/*
 * Nonzero when the boot parameter i915.debug=display asks for the display
 * path in detail: the runs' debug messages are printed, and the run log of
 * an enable is printed whether or not it failed.
 *
 * The device start's readiness report sets it once, before any device
 * starts, and nothing changes it afterwards.  Its zero value (quiet) is what
 * every other build, the host tests included, keeps.
 */
static int i915_lcd_debug_enabled;

static struct i915_lcd_trace_entry *i915_trace_add(struct i915_lcd_trace *trace, int kind);
static void i915_trace_write32(void *ctx, uint32_t reg, uint32_t value);
static uint32_t i915_trace_rmw32(void *ctx, uint32_t reg, uint32_t clear, uint32_t set);
static void i915_trace_posting_read(void *ctx, uint32_t reg);
static uint32_t i915_trace_read32(void *ctx, uint32_t reg);
static int i915_trace_wait_reg(void *ctx, uint32_t reg, uint32_t mask, uint32_t value, unsigned timeout_ms);
static void i915_trace_usleep(void *ctx, unsigned us);
static void i915_trace_udelay(void *ctx, unsigned us);
static uint32_t i915_trace_first_bytes(const uint8_t *buf, size_t size);
static long i915_trace_dpcd_read(void *ctx, unsigned offset, uint8_t *buf, size_t size);
static long i915_trace_dpcd_write(void *ctx, unsigned offset, const uint8_t *buf, size_t size);
static int i915_trace_read_dpcd_caps(void *ctx, uint8_t dpcd[15]);
static int i915_trace_panel(void *ctx, int op);
static int i915_trace_power_domain(void *ctx, unsigned use, unsigned index, int thunderbolt);
static int i915_trace_power_get(void *ctx, int domain);
static void i915_trace_power_put(void *ctx, int domain, int cookie);
static void i915_trace_power_put_async(void *ctx, int domain, int cookie, int delay_ms);
static void i915_trace_dbuf_slices_update(void *ctx, unsigned req_slices);
static void i915_trace_observe(void *ctx, int point);
static int i915_trace_vblank_get(void *ctx, int pipe);
static void i915_trace_vblank_put(void *ctx, int pipe);
static long i915_trace_vblank_sleep(void *ctx, int pipe, long ticks);
static void i915_trace_irq_off(void *ctx);
static void i915_trace_irq_on(void *ctx);
static void i915_trace_arm_event(void *ctx, int pipe);
static int i915_trace_wait_event(void *ctx, int pipe, unsigned timeout_ms);
static void i915_trace_cancel_event(void *ctx, int pipe);
static void i915_trace_tc_put_link(void *ctx, int tc_port);
static void i915_trace_tc_get_link(void *ctx, int tc_port, int lanes);
static int i915_trace_tc_mode(void *ctx, int tc_port);
static void i915_trace_tc_set_fia_lane_count(void *ctx, int tc_port, int lanes, int lane_reversal);
static unsigned i915_trace_tc_pin_assignment(void *ctx, int tc_port);
static uint32_t i915_trace_dkl_read(void *ctx, int tc_port, uint32_t phy_address);
static void i915_trace_dkl_write(void *ctx, int tc_port, uint32_t phy_address, uint32_t value);
static void i915_trace_dkl_rmw(void *ctx, int tc_port, uint32_t phy_address, uint32_t clear, uint32_t set);
static void i915_trace_lock(void *ctx, int which, int take);
static void i915_trace_named(struct i915_lcd_trace *trace, int kind, const char *name);
static void i915_trace_step(void *ctx, const char *name);
static void i915_trace_error(void *ctx, const char *what);
static void i915_trace_debug(void *ctx, const char *what);
static void i915_observer_sample(struct i915_lcd_observer *observer, int point, int clear_all);
static const char *i915_log_line_end(const char *text);
static const char *i915_trace_kind_name(int kind);

/*
 * ==== The run log ====
 */

/*
 * Turns the detailed display log of i915.debug=display on or off.
 */
void
drv_i915_lcd_debug_set(
	int enabled)
{
	/* Records the choice; only nonzero and zero are told apart. */
	i915_lcd_debug_enabled = 0;
	if (enabled != 0)
		i915_lcd_debug_enabled = 1;
}

/*
 * Reports whether i915.debug=display asked for the detailed display log.
 */
int
drv_i915_lcd_debug_enabled(void)
{
	/* Reports the recorded choice. */
	return i915_lcd_debug_enabled;
}

/*
 * Stacks the recorder on a backend.
 *
 * The caller hands trace->ops to the modeset; every hook of it records the
 * operation and forwards it to the backend.  The optional hooks exist only
 * when the backend has them.
 */
void
drv_i915_lcd_trace_init(
	struct i915_lcd_trace *trace,
	struct i915_lcd_emit *backend)
{
	/* Starts an empty record with no error seen. */
	kern_memset(trace, 0, sizeof(*trace));
	trace->backend = backend;
	trace->first_error_at = -1;

	/* Binds the recording hooks that every backend has. */
	trace->ops.ctx = trace;
	trace->ops.model = backend->model;
	trace->ops.write32 = i915_trace_write32;
	trace->ops.rmw32 = i915_trace_rmw32;
	trace->ops.posting_read = i915_trace_posting_read;
	trace->ops.step = i915_trace_step;
	trace->ops.read32 = i915_trace_read32;
	trace->ops.wait_reg = i915_trace_wait_reg;
	trace->ops.usleep = i915_trace_usleep;
	trace->ops.udelay = i915_trace_udelay;
	trace->ops.dpcd_read = i915_trace_dpcd_read;
	trace->ops.dpcd_write = i915_trace_dpcd_write;
	trace->ops.read_dpcd_caps = i915_trace_read_dpcd_caps;
	trace->ops.panel = i915_trace_panel;
	trace->ops.power_get = i915_trace_power_get;
	trace->ops.power_put = i915_trace_power_put;

	/* Names the backend's power domains, unrecorded, for a backend that names them. */
	if (backend->power_domain != NULL) {
		trace->ops.power_domain = i915_trace_power_domain;
	} else {
		trace->ops.power_domain = NULL;
	}

	/* Records the asynchronous put only for a backend that has one. */
	if (backend->power_put_async != NULL) {
		trace->ops.power_put_async = i915_trace_power_put_async;
	} else {
		trace->ops.power_put_async = NULL;
	}

	/* Records the DBUF slice update only for a backend that has one. */
	if (backend->dbuf_slices_update != NULL) {
		trace->ops.dbuf_slices_update = i915_trace_dbuf_slices_update;
	} else {
		trace->ops.dbuf_slices_update = NULL;
	}

	/* Every observe point is recorded. */
	trace->ops.observe = i915_trace_observe;

	/* Forwards the synchronous update's hooks only for a backend that has them. */
	if (backend->vblank_get != NULL) {
		trace->ops.vblank_get = i915_trace_vblank_get;
		trace->ops.vblank_put = i915_trace_vblank_put;
		trace->ops.vblank_sleep = i915_trace_vblank_sleep;
		trace->ops.irq_off = i915_trace_irq_off;
		trace->ops.irq_on = i915_trace_irq_on;
		trace->ops.arm_event = i915_trace_arm_event;
		trace->ops.wait_event = i915_trace_wait_event;

		/* The event cancel is optional even then. */
		if (backend->cancel_event != NULL)
			trace->ops.cancel_event = i915_trace_cancel_event;
	}

	/*
	 * Forwards the Type-C ports' links only for a backend that has them:
	 * without them a DisplayPort output on a Type-C port in
	 * DP-alt mode would be lit as if the port were held in no mode -- its
	 * PHY not taken for the lanes, the FIA's lanes not programmed -- and
	 * its link would not train.
	 */
	if (backend->tc_get_link != NULL) {
		trace->ops.tc_put_link = i915_trace_tc_put_link;
		trace->ops.tc_get_link = i915_trace_tc_get_link;
		trace->ops.tc_mode = i915_trace_tc_mode;
		trace->ops.tc_set_fia_lane_count = i915_trace_tc_set_fia_lane_count;
		trace->ops.tc_pin_assignment = i915_trace_tc_pin_assignment;
	}

	/* Forwards the Type-C ports' DKL PHY access, behind the backend's lock, only for a backend that has it. */
	if (backend->dkl_read != NULL) {
		trace->ops.dkl_read = i915_trace_dkl_read;
		trace->ops.dkl_write = i915_trace_dkl_write;
		trace->ops.dkl_rmw = i915_trace_dkl_rmw;
	}

	/* Binds the lock, error and debug hooks. */
	trace->ops.lock = i915_trace_lock;
	trace->ops.error = i915_trace_error;
	trace->ops.debug = i915_trace_debug;
}

/*
 * Records a phase marker written by the caller.
 */
void
drv_i915_lcd_trace_phase(
	struct i915_lcd_trace *trace,
	const char *name)
{
	/* Appends the marker by its name. */
	i915_trace_named(trace, I915_LCD_T_PHASE, name);
}

/*
 * ==== The commit observer ====
 */

/*
 * Prepares the observer of one pipe over the backend it reads.
 *
 * The underrun mask is the pipe's underrun bit with the soft, hard and
 * port underrun bits of display 12 and 13.
 */
void
drv_i915_lcd_observer_init(
	struct i915_lcd_observer *observer,
	struct i915_lcd_emit *hw,
	int pipe)
{
	/* Starts with no sample and no period. */
	kern_memset(observer, 0, sizeof(*observer));
	observer->hw = hw;
	observer->pipe = pipe;

	/* Names the pipe's status, frame counter and interrupt mask registers. */
	observer->status_reg = I915_XD_PIPE_STATUS((unsigned)pipe);
	observer->frame_reg = I915_XD_PIPE_FRMCNT((unsigned)pipe);
	observer->imr_reg = I915_XD_DE_PIPE_ISR((unsigned)pipe) + I915_XD_DE_IMR_OFFSET;

	/* Selects the underrun bits of this display version and the vblank bit. */
	observer->underrun_mask = I915_XD_PIPE_STATUS_UNDERRUN |
				  I915_XD_PIPE_STATUS_UNDERRUN_SOFT |
				  I915_XD_PIPE_STATUS_UNDERRUN_HARD |
				  I915_XD_PIPE_STATUS_UNDERRUN_PORT;
	observer->vblank_bit = I915_XD_PIPE_INT_VBLANK;
}

/*
 * Samples the pipe at a named point of the commit.
 *
 * This is the observe hook; ctx is the observer.  The start of a commit
 * ends a steady period first.
 */
void
drv_i915_lcd_observer_point(
	void *ctx,
	int point)
{
	struct i915_lcd_observer *observer;

	/* The observer the hook belongs to. */
	observer = ctx;

	/* A new commit ends the steady picture that stood before it. */
	if (point == I915_LCD_OBS_COMMIT_BEGIN && observer->steady)
		drv_i915_lcd_observer_steady_end(observer);

	/* Samples at the point. */
	i915_observer_sample(observer, point, 0);
}

/*
 * Starts the steady period of the picture.
 */
void
drv_i915_lcd_observer_steady_begin(
	struct i915_lcd_observer *observer)
{
	/* The last sample of the start-up period. */
	i915_observer_sample(observer, I915_LCD_OBS_STEADY, 0);

	/* Samples from here on belong to the steady picture. */
	observer->steady = 1;
}

/*
 * Samples the pipe during the steady period.
 */
void
drv_i915_lcd_observer_steady_sample(
	struct i915_lcd_observer *observer)
{
	/* Samples as a caller's point of the steady picture. */
	i915_observer_sample(observer, I915_LCD_OBS_STEADY, 0);
}

/*
 * Ends the steady period with a last sample.
 */
void
drv_i915_lcd_observer_steady_end(
	struct i915_lcd_observer *observer)
{
	/* Only a steady period has an end to record. */
	if (observer->steady) {
		i915_observer_sample(observer, I915_LCD_OBS_STEADY, 0);
		observer->steady = 0;
	}
}

/*
 * Waits for the frame counter to advance by a number of frames.
 *
 * The counter is polled every 10 ms for at most window_ms.  The first and
 * last values read are reported when asked for.  Returns 0 when the
 * counter advanced by min_frames, or ETIMEDOUT.
 */
int
drv_i915_lcd_observer_frames(
	struct i915_lcd_observer *observer,
	unsigned window_ms,
	unsigned min_frames,
	uint32_t *first,
	uint32_t *last)
{
	uint32_t first_frame;
	uint32_t frame;
	unsigned waited;

	/* Reads the counter the wait starts from. */
	first_frame = observer->hw->read32(observer->hw->ctx, observer->frame_reg);
	frame = first_frame;

	/* Polls until enough frames passed or the window is over. */
	for (waited = 0;
	     waited < window_ms && (uint32_t)(frame - first_frame) < min_frames;
	     waited += I915_OBSERVER_POLL_MS) {
		observer->hw->usleep(observer->hw->ctx, I915_OBSERVER_POLL_US);
		frame = observer->hw->read32(observer->hw->ctx, observer->frame_reg);
	}

	/* Reports the first and the last counter values when asked for. */
	if (first != NULL)
		*first = first_frame;
	if (last != NULL)
		*last = frame;

	/* The counter did not advance by enough frames in the window. */
	if ((uint32_t)(frame - first_frame) < min_frames)
		return ETIMEDOUT;

	/* Succeeded: the pipe produced the frames asked for. */
	return 0;
}

/*
 * Reports the stop evidence taken when the pipe was disabled.
 *
 * The evidence is taken at the disable's pipe-disabled point, so window_ms
 * is not used.  The two frame counter values are reported when asked for.
 * Returns 0 when the transcoder reads disabled and its counter stood
 * still, EINVAL when no evidence was taken, or EBUSY otherwise.
 */
int
drv_i915_lcd_observer_stopped(
	struct i915_lcd_observer *observer,
	unsigned window_ms,
	uint32_t *first,
	uint32_t *last)
{
	UNUSED_PARAMETER(window_ms);

	/* Reports the two counter values of the stop evidence when asked for. */
	if (first != NULL)
		*first = observer->stop_frame0;
	if (last != NULL)
		*last = observer->stop_frame1;

	/* No pipe-disabled point was reached: there is no evidence. */
	if (!observer->stop_checked)
		return EINVAL;

	/* The transcoder still reported itself enabled. */
	if ((observer->stop_transconf & I915_XD_TRANS_CONF_STATE) != 0u)
		return EBUSY;

	/* The frame counter still advanced after the disable. */
	if (observer->stop_frame0 != observer->stop_frame1)
		return EBUSY;

	/* Succeeded: the pipe is stopped. */
	return 0;
}

/*
 * ==== The output run's hooks and log ====
 */

/*
 * Counts and prints a step of an output run that is not connected.
 *
 * This is the step hook of an output run; ctx is the struct
 * i915_lcd_kernel.  A decided step was deliberately left out; any other
 * one is unresolved and must not pass silently on hardware.
 */
void
drv_i915_lcd_kernel_step(
	void *ctx,
	const char *name)
{
	struct i915_lcd_kernel *k;
	int prefix;

	/* The run the hook belongs to. */
	k = ctx;

	/* A decided callee is counted and printed as it is. */
	prefix = kern_strncmp(name, I915_TRACE_DECIDED_PREFIX, I915_TRACE_DECIDED_PREFIX_LEN);
	if (prefix == 0) {
		k->decided++;
		kern_logf("i915: display run: %s\n", name);
		return;
	}

	/* Any other callee is unresolved. */
	k->unresolved_steps++;
	kern_logf("i915: display run: UNRESOLVED step reached: %s\n", name);
}

/*
 * Counts and prints an error of an output run.
 *
 * This is the error hook of an output run; ctx is the struct
 * i915_lcd_kernel.
 * An error of the way down is counted apart from one of the bring-up.
 */
void
drv_i915_lcd_kernel_error(
	void *ctx,
	const char *what)
{
	struct i915_lcd_kernel *k;
	const char *phase;
	const char *line_end;

	/* The run the hook belongs to. */
	k = ctx;

	/* Counts the error in the phase of the run it belongs to. */
	if (k->phase_cleanup) {
		k->cleanup_errors++;
		phase = "cleanup";
	} else {
		k->errors++;
		phase = "bring-up";
	}

	/* Prints the error as one line. */
	line_end = i915_log_line_end(what);
	kern_logf("i915: display run: error (%s): %s%s", phase, what, line_end);
}

/*
 * Prints a debug message of an output run when i915.debug=display asks
 * for it, and ignores it otherwise.
 *
 * This is the debug hook of an output run; ctx is the struct
 * i915_lcd_kernel.  The message is printed as it is given.
 */
void
drv_i915_lcd_kernel_debug(
	void *ctx,
	const char *what)
{
	const char *line_end;

	UNUSED_PARAMETER(ctx);

	/* Without the detailed display log the message is dropped. */
	if (i915_lcd_debug_enabled == 0)
		return;

	/* Prints the message as one line. */
	line_end = i915_log_line_end(what);
	kern_logf("i915: display run: debug: %s%s", what, line_end);
}

/*
 * Logs the run log.
 *
 * The totals come first; then every entry except the reads, a named entry
 * by its name and any other by its operands.
 */
void
drv_i915_lcd_log_trace(
	const struct i915_lcd_trace *trace)
{
	const struct i915_lcd_trace_entry *entry;
	const char *kind;
	const char *line_end;
	unsigned index;

	/* Prints the totals of the run. */
	kern_logf("i915: display run: log: %u entries (%u writes, %u rmw, %u waits [%u timed out], %u unresolved steps, "
		"%u decided, %u errors, dropped %u, slept %llu us)\n",
		trace->n,
		trace->writes,
		trace->rmws,
		trace->waits,
		trace->wait_timeouts,
		trace->steps,
		trace->decided,
		trace->errors,
		trace->dropped,
		(unsigned long long)trace->slept_us);

	/* Prints every entry that is not a read. */
	for (index = 0u; index < trace->n; index++) {
		entry = &trace->e[index];

		/* The reads are too many to print. */
		if (entry->kind == I915_LCD_T_READ)
			continue;

		/* Prints a named entry by its name, any other by its operands. */
		kind = i915_trace_kind_name(entry->kind);
		if (entry->name != NULL) {
			line_end = i915_log_line_end(entry->name);
			kern_logf("i915: display run: t[%3u] %s %s%s", index, kind, entry->name, line_end);
		} else {
			kern_logf("i915: display run: t[%3u] %s 0x%05x b=0x%08x c=0x%08x d=0x%08x rc=%d n=%u\n",
				index,
				kind,
				entry->a,
				entry->b,
				entry->c,
				entry->d,
				entry->rc,
				entry->n);
		}
	}
}

/*
 * Logs the observer's registers, findings and samples.
 */
void
drv_i915_lcd_log_observer(
	const struct i915_lcd_observer *observer)
{
	const char *period;
	unsigned index;

	/* Prints what the observer reads and why the test owns the status. */
	kern_logf("i915: display run: observer: PIPE_STATUS 0x%05x underrun mask 0x%08x (bit31 pipe underrun, bit28 soft, bit27 hard, "
		"bit26 port) | PIPE_FRMCNT 0x%05x | DE_PIPE_IMR 0x%05x vblank bit 0x%x | the run owns the status; "
		"the pipe's underrun and vblank interrupts stay masked, so the IRQ handler never reads or clears it\n",
		observer->status_reg,
		observer->underrun_mask,
		observer->frame_reg,
		observer->imr_reg,
		observer->vblank_bit);

	/* Prints what the observer found in each period. */
	kern_logf("i915: display run: observer: status found before the run 0x%08x (kept, not judged) | start/stop periods 0x%08x | "
		"steady picture 0x%08x | vblank interrupt ever unmasked=%d | samples=%u dropped=%u\n",
		observer->saved_before,
		observer->seen_transition,
		observer->seen_steady,
		observer->vblank_unmasked_seen,
		observer->n,
		observer->dropped);

	/* Prints every sample. */
	for (index = 0u; index < observer->n; index++) {
		period = observer->s[index].steady ? "steady" : "trans ";
		kern_logf("i915: display run: observer sample %2u point=%3d %s status=0x%08x frame=%u imr=0x%08x\n",
			index,
			observer->s[index].point,
			period,
			observer->s[index].status,
			observer->s[index].frame,
			observer->s[index].imr);
	}
}

/* Appends an empty entry of a kind, or counts it as dropped when the record is full. */
static struct i915_lcd_trace_entry *
i915_trace_add(
	struct i915_lcd_trace *trace,
	int kind)
{
	struct i915_lcd_trace_entry *entry;

	/* A full record drops the entry. */
	if (trace->n >= I915_LCD_TRACE_MAX) {
		trace->dropped++;
		return NULL;
	}

	/* Takes the next entry and starts it as one operation of the kind. */
	entry = &trace->e[trace->n++];
	kern_memset(entry, 0, sizeof(*entry));
	entry->kind = (uint8_t)kind;
	entry->n = 1u;

	/* Succeeded: the caller fills the operands. */
	return entry;
}

/* Records a register write, then forwards it. */
static void
i915_trace_write32(
	void *ctx,
	uint32_t reg,
	uint32_t value)
{
	struct i915_lcd_trace *trace;
	struct i915_lcd_trace_entry *entry;

	/* The trace the hook records in. */
	trace = ctx;

	/* Records the write. */
	entry = i915_trace_add(trace, I915_LCD_T_WRITE);
	trace->writes++;
	if (entry != NULL) {
		entry->a = reg;
		entry->b = value;
	}

	/* Forwards the write. */
	trace->backend->write32(trace->backend->ctx, reg, value);
}

/* Records a read-modify-write with the old value the backend reports. */
static uint32_t
i915_trace_rmw32(
	void *ctx,
	uint32_t reg,
	uint32_t clear,
	uint32_t set)
{
	struct i915_lcd_trace *trace;
	struct i915_lcd_trace_entry *entry;
	uint32_t old;

	/* The trace the hook records in. */
	trace = ctx;

	/* Takes the entry, then forwards the operation. */
	entry = i915_trace_add(trace, I915_LCD_T_RMW);
	old = trace->backend->rmw32(trace->backend->ctx, reg, clear, set);

	/* Records the operation and the old value. */
	trace->rmws++;
	if (entry != NULL) {
		entry->a = reg;
		entry->b = set;
		entry->c = clear;
		entry->d = old;
	}

	/* Succeeded: the old value, as the backend read it. */
	return old;
}

/* Forwards a posting read to a backend that has one; it is not recorded. */
static void
i915_trace_posting_read(
	void *ctx,
	uint32_t reg)
{
	struct i915_lcd_trace *trace;

	/* The trace the hook records in. */
	trace = ctx;

	/* The posting read is optional in the backend. */
	if (trace->backend->posting_read != NULL)
		trace->backend->posting_read(trace->backend->ctx, reg);
}

/* Forwards a register read and records it; a run of identical reads is one entry. */
static uint32_t
i915_trace_read32(
	void *ctx,
	uint32_t reg)
{
	struct i915_lcd_trace *trace;
	struct i915_lcd_trace_entry *entry;
	struct i915_lcd_trace_entry *last;
	uint32_t value;

	/* The trace the hook records in. */
	trace = ctx;

	/* Reads the register. */
	value = trace->backend->read32(trace->backend->ctx, reg);

	/* A polling loop reads the same value again: the last entry counts it. */
	if (trace->n != 0u) {
		last = &trace->e[trace->n - 1u];
		if (last->kind == I915_LCD_T_READ &&
		    last->a == reg &&
		    last->b == value &&
		    last->n < I915_TRACE_READ_REPEAT_MAX) {
			last->n++;
			return value;
		}
	}

	/* Records a new read. */
	entry = i915_trace_add(trace, I915_LCD_T_READ);
	if (entry != NULL) {
		entry->a = reg;
		entry->b = value;
	}

	/* Succeeded: the value read. */
	return value;
}

/* Forwards a register wait and records its condition and result. */
static int
i915_trace_wait_reg(
	void *ctx,
	uint32_t reg,
	uint32_t mask,
	uint32_t value,
	unsigned timeout_ms)
{
	struct i915_lcd_trace *trace;
	struct i915_lcd_trace_entry *entry;
	int result;

	/* The trace the hook records in. */
	trace = ctx;

	/* Waits first, then takes the entry. */
	result = trace->backend->wait_reg(trace->backend->ctx, reg, mask, value, timeout_ms);
	entry = i915_trace_add(trace, I915_LCD_T_WAIT);

	/* Counts the wait and a wait that did not reach its condition. */
	trace->waits++;
	if (result != 0)
		trace->wait_timeouts++;

	/* Records the condition and the result. */
	if (entry != NULL) {
		entry->a = reg;
		entry->b = value;
		entry->c = mask;
		entry->d = timeout_ms;
		entry->rc = result;
	}

	/* Succeeded: the wait's own result, in the backend's numbering. */
	return result;
}

/* Counts a sleep and its length, then forwards it. */
static void
i915_trace_usleep(
	void *ctx,
	unsigned us)
{
	struct i915_lcd_trace *trace;

	/* The trace the hook records in. */
	trace = ctx;

	/* Counts the sleep and the time slept. */
	trace->sleeps++;
	trace->slept_us += us;

	/* Forwards the sleep. */
	trace->backend->usleep(trace->backend->ctx, us);
}

/* Counts the length of a busy delay, then forwards it. */
static void
i915_trace_udelay(
	void *ctx,
	unsigned us)
{
	struct i915_lcd_trace *trace;

	/* The trace the hook records in. */
	trace = ctx;

	/* Counts the time delayed. */
	trace->slept_us += us;

	/* Forwards the delay. */
	trace->backend->udelay(trace->backend->ctx, us);
}

/* Packs up to the first four bytes of a buffer, the first byte lowest. */
static uint32_t
i915_trace_first_bytes(
	const uint8_t *buf,
	size_t size)
{
	uint32_t packed;
	size_t i;

	/* Nothing packed yet. */
	packed = 0u;

	/* Packs at most four bytes. */
	for (i = 0; i < size && i < 4u; i++)
		packed |= (uint32_t)buf[i] << (8u * i);

	/* Succeeded: the packed bytes. */
	return packed;
}

/* Forwards a DPCD read and records its offset, first bytes, size and result. */
static long
i915_trace_dpcd_read(
	void *ctx,
	unsigned offset,
	uint8_t *buf,
	size_t size)
{
	struct i915_lcd_trace *trace;
	struct i915_lcd_trace_entry *entry;
	long result;

	/* The trace the hook records in. */
	trace = ctx;

	/* Reads first, then takes the entry. */
	result = trace->backend->dpcd_read(trace->backend->ctx, offset, buf, size);
	entry = i915_trace_add(trace, I915_LCD_T_DPCD_READ);

	/* Records the read; the first bytes only when some were transferred. */
	if (entry != NULL) {
		entry->a = offset;
		if (result > 0) {
			entry->b = i915_trace_first_bytes(buf, (size_t)result);
		} else {
			entry->b = 0u;
		}

		/* Records the size asked for and what the read answered. */
		entry->c = (uint32_t)size;
		entry->rc = (int32_t)result;
	}

	/* Succeeded: the read's own result. */
	return result;
}

/* Records a DPCD write, then forwards it and records its result. */
static long
i915_trace_dpcd_write(
	void *ctx,
	unsigned offset,
	const uint8_t *buf,
	size_t size)
{
	struct i915_lcd_trace *trace;
	struct i915_lcd_trace_entry *entry;
	long result;

	/* The trace the hook records in. */
	trace = ctx;

	/* Takes the entry, then writes. */
	entry = i915_trace_add(trace, I915_LCD_T_DPCD_WRITE);
	result = trace->backend->dpcd_write(trace->backend->ctx, offset, buf, size);

	/* Records the write and its result. */
	if (entry != NULL) {
		entry->a = offset;
		entry->b = i915_trace_first_bytes(buf, size);
		entry->c = (uint32_t)size;
		entry->rc = (int32_t)result;
	}

	/* Succeeded: the write's own result. */
	return result;
}

/* Forwards the read of the receiver capabilities and records it as a DPCD read. */
static int
i915_trace_read_dpcd_caps(
	void *ctx,
	uint8_t dpcd[15])
{
	struct i915_lcd_trace *trace;
	struct i915_lcd_trace_entry *entry;
	int result;

	/* The trace the hook records in. */
	trace = ctx;

	/* Reads first, then takes the entry. */
	result = trace->backend->read_dpcd_caps(trace->backend->ctx, dpcd);
	entry = i915_trace_add(trace, I915_LCD_T_DPCD_READ);

	/* Records the read; the first bytes only when it succeeded. */
	if (entry != NULL) {
		entry->a = 0u;
		if (result == 0) {
			entry->b = i915_trace_first_bytes(dpcd, 4u);
		} else {
			entry->b = 0u;
		}

		/* Records the capability bytes asked for and what the read answered. */
		entry->c = 15u;
		entry->rc = result;
		entry->name = "dpcd caps read";
	}

	/* Succeeded: the read's own result. */
	return result;
}

/* Records a panel power operation, then forwards it and records its result. */
static int
i915_trace_panel(
	void *ctx,
	int op)
{
	struct i915_lcd_trace *trace;
	struct i915_lcd_trace_entry *entry;
	int result;

	/* The trace the hook records in. */
	trace = ctx;

	/* Takes the entry, then runs the operation. */
	entry = i915_trace_add(trace, I915_LCD_T_PANEL);
	result = trace->backend->panel(trace->backend->ctx, op);

	/* Records the operation and its result. */
	if (entry != NULL) {
		entry->a = (uint32_t)op;
		entry->rc = result;
	}

	/* Succeeded: the operation's own result. */
	return result;
}

/* Asks the backend for the domain of one power use of an output (not recorded: the get and the put are). */
static int
i915_trace_power_domain(
	void *ctx,
	unsigned use,
	unsigned index,
	int thunderbolt)
{
	struct i915_lcd_trace *trace;
	int domain;

	/* The backend names it. */
	trace = ctx;
	domain = trace->backend->power_domain(trace->backend->ctx, use, index, thunderbolt);

	/* Succeeded: the domain. */
	return domain;
}

/* Forwards a power domain get and records the cookie it returned. */
static int
i915_trace_power_get(
	void *ctx,
	int domain)
{
	struct i915_lcd_trace *trace;
	struct i915_lcd_trace_entry *entry;
	int cookie;

	/* The trace the hook records in. */
	trace = ctx;

	/* Takes the reference first, then the entry. */
	cookie = trace->backend->power_get(trace->backend->ctx, domain);
	entry = i915_trace_add(trace, I915_LCD_T_POWER_GET);

	/* Records the domain and the cookie. */
	if (entry != NULL) {
		entry->a = (uint32_t)domain;
		entry->rc = cookie;
	}

	/* Succeeded: the cookie, 0 when the get failed. */
	return cookie;
}

/* Records a power domain put, then forwards it. */
static void
i915_trace_power_put(
	void *ctx,
	int domain,
	int cookie)
{
	struct i915_lcd_trace *trace;
	struct i915_lcd_trace_entry *entry;

	/* The trace the hook records in. */
	trace = ctx;

	/* Records the domain and the cookie returned. */
	entry = i915_trace_add(trace, I915_LCD_T_POWER_PUT);
	if (entry != NULL) {
		entry->a = (uint32_t)domain;
		entry->b = (uint32_t)cookie;
	}

	/* Forwards the put. */
	trace->backend->power_put(trace->backend->ctx, domain, cookie);
}

/* Records an asynchronous power domain put with its delay, then forwards it. */
static void
i915_trace_power_put_async(
	void *ctx,
	int domain,
	int cookie,
	int delay_ms)
{
	struct i915_lcd_trace *trace;
	struct i915_lcd_trace_entry *entry;

	/* The trace the hook records in. */
	trace = ctx;

	/* Records the put; d = 1 marks it asynchronous. */
	entry = i915_trace_add(trace, I915_LCD_T_POWER_PUT);
	if (entry != NULL) {
		entry->a = (uint32_t)domain;
		entry->b = (uint32_t)cookie;
		entry->c = (uint32_t)delay_ms;
		entry->d = 1u;
	}

	/* Forwards the put. */
	trace->backend->power_put_async(trace->backend->ctx, domain, cookie, delay_ms);
}

/* Records a DBUF slice request, then forwards it. */
static void
i915_trace_dbuf_slices_update(
	void *ctx,
	unsigned req_slices)
{
	struct i915_lcd_trace *trace;
	struct i915_lcd_trace_entry *entry;

	/* The trace the hook records in. */
	trace = ctx;

	/* Records the slices requested. */
	entry = i915_trace_add(trace, I915_LCD_T_DBUF);
	if (entry != NULL)
		entry->a = req_slices;

	/* Forwards the request. */
	trace->backend->dbuf_slices_update(trace->backend->ctx, req_slices);
}

/* Records an observe point, then hands it to the tap and to the backend. */
static void
i915_trace_observe(
	void *ctx,
	int point)
{
	struct i915_lcd_trace *trace;
	struct i915_lcd_trace_entry *entry;

	/* The trace the hook records in. */
	trace = ctx;

	/* Records the point. */
	entry = i915_trace_add(trace, I915_LCD_T_OBSERVE);
	if (entry != NULL)
		entry->a = (uint32_t)point;

	/* The caller's tap sees the point before the backend. */
	if (trace->tap != NULL)
		trace->tap(trace->tap_ctx, point);

	/* The backend's observe hook is optional. */
	if (trace->backend->observe != NULL)
		trace->backend->observe(trace->backend->ctx, point);
}

/* Forwards a vblank reference get; it is not recorded. */
static int
i915_trace_vblank_get(
	void *ctx,
	int pipe)
{
	struct i915_lcd_trace *trace;
	int result;

	/* The trace the hook records in. */
	trace = ctx;

	/* Takes the vblank reference. */
	result = trace->backend->vblank_get(trace->backend->ctx, pipe);

	/* Succeeded: the backend's own result. */
	return result;
}

/* Forwards a vblank reference put; it is not recorded. */
static void
i915_trace_vblank_put(
	void *ctx,
	int pipe)
{
	struct i915_lcd_trace *trace;

	/* The trace the hook records in. */
	trace = ctx;

	/* Returns the vblank reference. */
	trace->backend->vblank_put(trace->backend->ctx, pipe);
}

/* Forwards a sleep until the next vblank; it is not recorded. */
static long
i915_trace_vblank_sleep(
	void *ctx,
	int pipe,
	long ticks)
{
	struct i915_lcd_trace *trace;
	long left;

	/* The trace the hook records in. */
	trace = ctx;

	/* Sleeps until the pipe's next vblank or the ticks run out. */
	left = trace->backend->vblank_sleep(trace->backend->ctx, pipe, ticks);

	/* Succeeded: the ticks left. */
	return left;
}

/* Forwards the interrupt disable around the update section; it is not recorded. */
static void
i915_trace_irq_off(
	void *ctx)
{
	struct i915_lcd_trace *trace;

	/* The trace the hook records in. */
	trace = ctx;

	/* Disables the local interrupts. */
	trace->backend->irq_off(trace->backend->ctx);
}

/* Forwards the interrupt enable after the update section; it is not recorded. */
static void
i915_trace_irq_on(
	void *ctx)
{
	struct i915_lcd_trace *trace;

	/* The trace the hook records in. */
	trace = ctx;

	/* Enables the local interrupts. */
	trace->backend->irq_on(trace->backend->ctx);
}

/* Forwards the arming of the vblank event; it is not recorded. */
static void
i915_trace_arm_event(
	void *ctx,
	int pipe)
{
	struct i915_lcd_trace *trace;

	/* The trace the hook records in. */
	trace = ctx;

	/* Arms the event for the pipe's next vblank. */
	trace->backend->arm_event(trace->backend->ctx, pipe);
}

/* Forwards the wait for the vblank event; it is not recorded. */
static int
i915_trace_wait_event(
	void *ctx,
	int pipe,
	unsigned timeout_ms)
{
	struct i915_lcd_trace *trace;
	int result;

	/* The trace the hook records in. */
	trace = ctx;

	/* Waits for the event to complete. */
	result = trace->backend->wait_event(trace->backend->ctx, pipe, timeout_ms);

	/* Succeeded: the wait's own result, in the backend's numbering. */
	return result;
}

/* Forwards the cancel of a pending vblank event; it is not recorded. */
static void
i915_trace_cancel_event(
	void *ctx,
	int pipe)
{
	struct i915_lcd_trace *trace;

	/* The trace the hook records in. */
	trace = ctx;

	/* Cancels the pending event. */
	trace->backend->cancel_event(trace->backend->ctx, pipe);
}

/* Forwards the giving back of a Type-C port's link; it is not recorded. */
static void
i915_trace_tc_put_link(
	void *ctx,
	int tc_port)
{
	struct i915_lcd_trace *trace;

	/* The run log the hook was bound with. */
	trace = ctx;

	/* Gives the port's link back; the last one gives its PHY back. */
	trace->backend->tc_put_link(trace->backend->ctx, tc_port);
}

/* Forwards the taking of a Type-C port's link; it is not recorded. */
static void
i915_trace_tc_get_link(
	void *ctx,
	int tc_port,
	int lanes)
{
	struct i915_lcd_trace *trace;

	/* The run log the hook was bound with. */
	trace = ctx;

	/* Takes the port's link, and with it the PHY for the lanes. */
	trace->backend->tc_get_link(trace->backend->ctx, tc_port, lanes);
}

/* Forwards the question of the mode a Type-C port is held in; it is not recorded. */
static int
i915_trace_tc_mode(
	void *ctx,
	int tc_port)
{
	struct i915_lcd_trace *trace;
	int mode;

	/* The run log the hook was bound with. */
	trace = ctx;

	/* Asks the Type-C core. */
	mode = trace->backend->tc_mode(trace->backend->ctx, tc_port);

	/* Succeeded: the mode the port is held in. */
	return mode;
}

/* Forwards the programming of a Type-C port's FIA lanes; it is not recorded. */
static void
i915_trace_tc_set_fia_lane_count(
	void *ctx,
	int tc_port,
	int lanes,
	int lane_reversal)
{
	struct i915_lcd_trace *trace;

	/* The run log the hook was bound with. */
	trace = ctx;

	/* Programs the FIA's DisplayPort lanes. */
	trace->backend->tc_set_fia_lane_count(trace->backend->ctx, tc_port, lanes, lane_reversal);
}

/* Forwards the reading of a Type-C port's pin assignment; it is not recorded. */
static unsigned
i915_trace_tc_pin_assignment(
	void *ctx,
	int tc_port)
{
	struct i915_lcd_trace *trace;
	unsigned pin_assignment;

	/* The run log the hook was bound with. */
	trace = ctx;

	/* Asks the Type-C core for the FIA's pin assignment. */
	pin_assignment = trace->backend->tc_pin_assignment(trace->backend->ctx, tc_port);

	/* Succeeded: the pin assignment. */
	return pin_assignment;
}

/* Forwards a read of a Type-C port's DKL PHY register; it is not recorded. */
static uint32_t
i915_trace_dkl_read(
	void *ctx,
	int tc_port,
	uint32_t phy_address)
{
	struct i915_lcd_trace *trace;
	uint32_t value;

	/* The run log the hook was bound with. */
	trace = ctx;

	/* Reads the register behind the ports' bank index lock. */
	value = trace->backend->dkl_read(trace->backend->ctx, tc_port, phy_address);

	/* Succeeded: the register's value. */
	return value;
}

/* Forwards a write of a Type-C port's DKL PHY register; it is not recorded. */
static void
i915_trace_dkl_write(
	void *ctx,
	int tc_port,
	uint32_t phy_address,
	uint32_t value)
{
	struct i915_lcd_trace *trace;

	/* The run log the hook was bound with. */
	trace = ctx;

	/* Writes the register behind the ports' bank index lock. */
	trace->backend->dkl_write(trace->backend->ctx, tc_port, phy_address, value);
}

/* Forwards a read-modify-write of a Type-C port's DKL PHY register; it is not recorded. */
static void
i915_trace_dkl_rmw(
	void *ctx,
	int tc_port,
	uint32_t phy_address,
	uint32_t clear,
	uint32_t set)
{
	struct i915_lcd_trace *trace;

	/* The run log the hook was bound with. */
	trace = ctx;

	/* Changes the register's bits behind the ports' bank index lock. */
	trace->backend->dkl_rmw(trace->backend->ctx, tc_port, phy_address, clear, set);
}

/* Forwards a modeset lock operation; it is not recorded. */
static void
i915_trace_lock(
	void *ctx,
	int which,
	int take)
{
	struct i915_lcd_trace *trace;

	/* The trace the hook records in. */
	trace = ctx;

	/* Takes or gives back the lock. */
	trace->backend->lock(trace->backend->ctx, which, take);
}

/* Appends an entry that carries only a name. */
static void
i915_trace_named(
	struct i915_lcd_trace *trace,
	int kind,
	const char *name)
{
	struct i915_lcd_trace_entry *entry;

	/* Records the name when the record has room. */
	entry = i915_trace_add(trace, kind);
	if (entry != NULL)
		entry->name = name;
}

/* Records a decided or an unported callee, then forwards it to the backend's step hook. */
static void
i915_trace_step(
	void *ctx,
	const char *name)
{
	struct i915_lcd_trace *trace;
	int prefix;

	/* The trace the hook records in. */
	trace = ctx;

	/* A decided callee is counted apart from an unported one. */
	prefix = kern_strncmp(name, I915_TRACE_DECIDED_PREFIX, I915_TRACE_DECIDED_PREFIX_LEN);
	if (prefix == 0) {
		trace->decided++;
		i915_trace_named(trace, I915_LCD_T_DECIDED, name);
	} else {
		trace->steps++;
		i915_trace_named(trace, I915_LCD_T_STEP, name);
	}

	/* The backend's step hook is optional. */
	if (trace->backend->step != NULL)
		trace->backend->step(trace->backend->ctx, name);
}

/* Records an error of the run, remembering where the first one is, then forwards it. */
static void
i915_trace_error(
	void *ctx,
	const char *what)
{
	struct i915_lcd_trace *trace;

	/* The trace the hook records in. */
	trace = ctx;

	/* The first error marks the entry a failed run is read from. */
	if (trace->errors == 0u)
		trace->first_error_at = (int)trace->n;
	trace->errors++;

	/* Records the error. */
	i915_trace_named(trace, I915_LCD_T_ERROR, what);

	/* The backend's error hook is optional. */
	if (trace->backend->error != NULL)
		trace->backend->error(trace->backend->ctx, what);
}

/* Forwards a debug message to the backend's debug hook; it is not recorded. */
static void
i915_trace_debug(
	void *ctx,
	const char *what)
{
	struct i915_lcd_trace *trace;

	/* The trace the hook records in. */
	trace = ctx;

	/* The backend's debug hook is optional. */
	if (trace->backend->debug != NULL)
		trace->backend->debug(trace->backend->ctx, what);
}

/*
 * Samples the pipe's status, interrupt mask and frame counter at a point.
 *
 * The first sample keeps what was left before the run; the underrun-arm
 * point clears the whole status; the pipe-disabled point
 * takes the stop evidence; any other point records the underrun bits seen,
 * then clears them.
 */
static void
i915_observer_sample(
	struct i915_lcd_observer *observer,
	int point,
	int clear_all)
{
	struct i915_lcd_obs_sample *sample;
	uint32_t transconf_reg;
	uint32_t status;
	uint32_t imr;
	uint32_t bits;

	/* Reads the underrun status and the interrupt mask. */
	status = observer->hw->read32(observer->hw->ctx, observer->status_reg);
	imr = observer->hw->read32(observer->hw->ctx, observer->imr_reg);
	bits = status & observer->underrun_mask;

	/* Records the sample with the frame counter, or counts it as dropped. */
	if (observer->n < I915_LCD_OBS_MAX_SAMPLES) {
		sample = &observer->s[observer->n++];
		sample->point = point;
		sample->status = status;
		sample->frame = observer->hw->read32(observer->hw->ctx, observer->frame_reg);
		sample->imr = imr;
		sample->steady = observer->steady;
	} else {
		observer->dropped++;
	}

	/* From the underrun-arm point on, the crtc holds the pipe's power well. */
	if (point == I915_LCD_OBS_UNDERRUN_ARM)
		observer->powered = 1;

	/* An unmasked vblank interrupt is judged only while the well is on. */
	if (observer->powered && (imr & observer->vblank_bit) == 0u)
		observer->vblank_unmasked_seen = 1;

	/*
	 * The pipe-disabled point is the last moment the pipe's well is on:
	 * takes the stop evidence (TRANSCONF and two frame counter reads three
	 * frame times apart).
	 */
	if (point == I915_LCD_OBS_PIPE_DISABLED) {
		transconf_reg = I915_XD_TRANS_CONF((unsigned)observer->pipe);
		observer->stop_transconf = observer->hw->read32(observer->hw->ctx, transconf_reg);
		observer->stop_frame0 = observer->hw->read32(observer->hw->ctx, observer->frame_reg);
		observer->hw->usleep(observer->hw->ctx, I915_OBSERVER_STOP_SETTLE_US);
		observer->stop_frame1 = observer->hw->read32(observer->hw->ctx, observer->frame_reg);
		observer->stop_checked = 1;
		observer->powered = 0;
	}

	/* The first sample keeps what whoever ran before left there: kept, not judged. */
	if (!observer->begun) {
		observer->saved_before = status;
		observer->begun = 1;
		return;
	}

	/*
	 * The first read with the pipe's well on is what was really left there;
	 * then the whole underrun mask is written, which clears the status as
	 * the run arms its underrun report.
	 */
	if (point == I915_LCD_OBS_UNDERRUN_ARM) {
		observer->saved_before = status;
		observer->hw->write32(observer->hw->ctx, observer->status_reg, observer->underrun_mask);
		return;
	}

	/* Records the bits first, in the period they belong to, so nothing is lost. */
	if (observer->steady) {
		observer->seen_steady |= bits;
	} else {
		observer->seen_transition |= bits;
	}

	/* Clears them after, so the next period starts clean. */
	if (bits != 0u || clear_all)
		observer->hw->write32(observer->hw->ctx, observer->status_reg, bits);
}

/* Returns the newline a log line still needs after a text that may already end with one. */
static const char *
i915_log_line_end(
	const char *text)
{
	size_t length;

	/* An empty text still needs the newline. */
	if (text[0] == '\0')
		return "\n";

	/* A text that ends with a newline needs none. */
	length = kern_strlen(text);
	if (text[length - 1u] == '\n')
		return "";

	/* Succeeded: any other text needs the newline. */
	return "\n";
}

/* Returns the log name of a run log entry kind, "?" for an unknown one. */
static const char *
i915_trace_kind_name(
	int kind)
{
	/* A kind outside the table has no name. */
	if (kind < 0)
		return "?";
	if (kind > I915_LCD_T_OBSERVE)
		return "?";

	/* Succeeded: the kind's name. */
	return i915_trace_kind_names[kind];
}
