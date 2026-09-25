/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The panel's diagnostics (see diagnostics.h).
 *
 * The recorder stacked on the modeset hooks forwards every operation and
 * logs it in order with what the backend answered: it is the run log of a
 * modeset (which phase, which write, what a wait returned, the first
 * anomaly), and it never replays anything.  The commit observer owns the
 * pipe's underrun status for a run: it keeps the value found before the
 * run, clears the status where the Linux text clears it, and at every
 * sample records the bits first and clears them after, so no occurrence is
 * lost and the start, stop and steady periods are told apart.  "Frames
 * advance" is decided from the hardware frame counter alone.
 *
 * The named registers are read back by the Linux names and addresses of
 * the extracted register headers, next to the value Linux left in the same
 * register on the same machine with the same panel and mode.  Those dump
 * values are comparison material for the log only; nothing computes with
 * them.
 *
 * The last-resort stop is not a Linux path: at the end of the device, any
 * pipe that still scans out is stopped, because a pipe left running keeps
 * reading memory after the driver let it go.
 *
 * The test picture is a frame, four coloured corners, a grey ramp, colour
 * bars, the letter F and the three digits of the test id, so a photograph
 * shows orientation, colour order and which run drew it.
 */

#include "internal.h"
#include "modeset-internal.h"
#include "watermark-internal.h"
#include <kern/kcrt.h>

/* The backlight PWM registers and the DDI / transcoder registers the table names. */
#include "../intel/mreg.h"
#include "../intel/trans.h"

#include "diagnostics.h"
#include "modeset.h"
#include "power.h"
#include "../mmio.h"

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

/* The pipes and DDI buffers the last-resort stop looks at. */
#define I915_LAST_RESORT_PIPES		4u
#define I915_LAST_RESORT_DDIS		2u

/* The distance between two pipes' registers and between two DDI buffers. */
#define I915_LAST_RESORT_PIPE_STRIDE	0x1000u
#define I915_LAST_RESORT_DDI_STRIDE	0x100u

/* Pipe A's TRANSCONF, PLANE_CTL_1 and PLANE_SURF_1, and DDI A's DDI_BUF_CTL. */
#define I915_LAST_RESORT_TRANSCONF	0x70008u
#define I915_LAST_RESORT_PLANE_CTL	0x70180u
#define I915_LAST_RESORT_PLANE_SURF	0x7019cu
#define I915_LAST_RESORT_DDI_BUF_CTL	0x64000u

/* The enable bit of TRANSCONF, PLANE_CTL and DDI_BUF_CTL, and TRANSCONF's state bit. */
#define I915_LAST_RESORT_ENABLE		0x80000000u
#define I915_LAST_RESORT_STATE		0x40000000u

/* How many polls of the transcoder state the stop makes, about a millisecond apart. */
#define I915_LAST_RESORT_POLLS		100u
#define I915_LAST_RESORT_POLL_MIN_US	1000u
#define I915_LAST_RESORT_POLL_MAX_US	2000u

/* The pipes, DDI ports and PLLs the readout survey looks at. */
#define I915_SURVEY_PIPES		4u
#define I915_SURVEY_DDIS		6u
#define I915_SURVEY_PLLS		7u

/* Pipe A's TRANSCONF, TRANS_DDI_FUNC_CTL and PLANE_CTL_1, and the distance to the next pipe's. */
#define I915_SURVEY_TRANSCONF		0x70008u
#define I915_SURVEY_TRANS_DDI_FUNC_CTL	0x60400u
#define I915_SURVEY_PLANE_CTL		0x70180u
#define I915_SURVEY_PIPE_STRIDE		0x1000u

/* DDI A's DDI_BUF_CTL, and the distance to the next port's. */
#define I915_SURVEY_DDI_BUF_CTL		0x64000u
#define I915_SURVEY_DDI_STRIDE		0x100u

/* The colours of the test picture (XRGB8888). */
#define I915_PATTERN_BACKGROUND		0x00102040u
#define I915_PATTERN_WHITE		0x00ffffffu
#define I915_PATTERN_RED		0x00ff0000u
#define I915_PATTERN_GREEN		0x0000ff00u
#define I915_PATTERN_BLUE		0x000000ffu
#define I915_PATTERN_YELLOW		0x00ffff00u

/* The width of the white frame on the edge of the picture. */
#define I915_PATTERN_FRAME		16u

/* The FNV-1a offset basis and prime the fill's hash uses. */
#define I915_PATTERN_FNV_BASIS		0xcbf29ce484222325ull
#define I915_PATTERN_FNV_PRIME		0x100000001b3ull

/*
 * The seven segments a..g (bits 0..6) lit for each of the digits 0..9.
 *
 * The test picture draws the test id with them; the table never changes.
 */
static const uint8_t i915_pattern_seven_segments[10] = {
	0x3f, 0x06, 0x5b, 0x4f, 0x66, 0x6d, 0x7d, 0x07, 0x7f, 0x6f
};

/*
 * The eight colour bars along the bottom of the test picture, left to right.
 *
 * The table never changes.
 */
static const uint32_t i915_pattern_bars[8] = {
	0x00ffffffu, 0x00ffff00u, 0x0000ffffu, 0x0000ff00u,
	0x00ff00ffu, 0x00ff0000u, 0x000000ffu, 0x00000000u
};

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
 * The DDI ports the readout survey reads, by port number (A, B, TC1..TC4),
 * and their log names in the same order.
 *
 * The tables never change.
 */
static const unsigned i915_survey_ddi_ports[I915_SURVEY_DDIS] = {
	0u, 1u, 3u, 4u, 5u, 6u
};
static const char *const i915_survey_ddi_names[I915_SURVEY_DDIS] = {
	"A", "B", "TC1", "TC2", "TC3", "TC4"
};

/*
 * The enable registers of the PLLs the readout survey reads (DPLL0, DPLL1,
 * the TBT PLL, TC PLL 1..4), and their log names in the same order.
 *
 * The tables never change.
 */
static const uint32_t i915_survey_pll_regs[I915_SURVEY_PLLS] = {
	0x46010u, 0x46014u,
	0x46020u,
	0x46030u, 0x46034u, 0x46038u, 0x4603cu
};
static const char *const i915_survey_pll_names[I915_SURVEY_PLLS] = {
	"DPLL0", "DPLL1", "TBT", "TC1", "TC2", "TC3", "TC4"
};

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
static int i915_trace_power_get(void *ctx, int domain);
static void i915_trace_power_put(void *ctx, int domain, int wakeref);
static void i915_trace_power_put_async(void *ctx, int domain, int wakeref, int delay_ms);
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
static void i915_trace_lock(void *ctx, int which, int take);
static void i915_trace_named(struct i915_lcd_trace *trace, int kind, const char *name);
static void i915_trace_step(void *ctx, const char *name);
static void i915_trace_error(void *ctx, const char *what);
static void i915_trace_debug(void *ctx, const char *what);
static int i915_trace_kind_is_named(int kind);
static void i915_observer_sample(struct i915_lcd_observer *observer, int point, int clear_all);
static void i915_reg_table_row(struct i915_lcd_world *world, unsigned *count, const char *name, i915_reg_t reg, int has_linux, uint32_t linux_value, uint32_t compare_mask);
static int i915_pattern_in_rect(uint32_t x, uint32_t y, uint32_t x0, uint32_t y0, uint32_t w, uint32_t h);
static int i915_pattern_in_digit(uint32_t x, uint32_t y, uint32_t x0, uint32_t y0, uint32_t w, uint32_t h, uint32_t t, unsigned digit);
static const char *i915_log_line_end(const char *text);
static const char *i915_trace_kind_name(int kind);

/*
 * ==== The modeset run log ====
 */

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
 * Finds the first entry of a kind at or after an index.
 *
 * A named kind (step, error, phase, decided) matches when its name contains
 * `name`; any other kind matches when its first operand equals `a`.
 * Returns the index, or -1 when no entry matches.
 */
int
drv_i915_lcd_trace_find(
	const struct i915_lcd_trace *trace,
	int kind,
	uint32_t a,
	const char *name,
	unsigned from)
{
	const struct i915_lcd_trace_entry *entry;
	const char *found;
	unsigned index;
	int named;

	/* Walks the record from the first index asked for. */
	for (index = from; index < trace->n; index++) {
		entry = &trace->e[index];

		/* Skips an entry of another kind. */
		if (entry->kind != kind)
			continue;

		/* A named kind is matched by its name, any other by its first operand. */
		named = i915_trace_kind_is_named(kind);
		if (named) {
			/* An entry without a name, or a search without one, never matches. */
			if (name == NULL)
				continue;
			if (entry->name == NULL)
				continue;

			/* The name matches when it contains the one searched for. */
			found = kern_strstr(entry->name, name);
			if (found != NULL)
				return (int)index;
		} else if (entry->a == a) {
			return (int)index;
		}
	}

	/* No entry of the record matches. */
	return -1;
}

/*
 * ==== The commit observer ====
 */

/*
 * Prepares the observer of one pipe over the backend it reads.
 *
 * The underrun mask is icl_pipe_status_underrun_mask() on display version
 * 13: the pipe underrun plus the XELPD soft, hard and port bits.
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
	observer->status_reg = i915_mmio_reg_offset(ICL_PIPESTATUS(pipe));
	observer->frame_reg = i915_mmio_reg_offset(PIPE_FRMCOUNT_G4X(pipe));
	observer->imr_reg = i915_mmio_reg_offset(GEN8_DE_PIPE_IMR(pipe));

	/* Selects the underrun bits of this display version and the vblank bit. */
	observer->underrun_mask = PIPE_STATUS_UNDERRUN |
				  PIPE_STATUS_SOFT_UNDERRUN_XELPD |
				  PIPE_STATUS_HARD_UNDERRUN_XELPD |
				  PIPE_STATUS_PORT_UNDERRUN_XELPD;
	observer->vblank_bit = GEN8_PIPE_VBLANK;
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
	if ((observer->stop_transconf & TRANSCONF_STATE_ENABLE) != 0u)
		return EBUSY;

	/* The frame counter still advanced after the disable. */
	if (observer->stop_frame0 != observer->stop_frame1)
		return EBUSY;

	/* Succeeded: the pipe is stopped. */
	return 0;
}

/*
 * Reads whether the observed pipe's transcoder is enabled.
 *
 * The TRANSCONF value read is reported when asked for.  Returns 1 when
 * its state bit says enabled, 0 otherwise.
 */
int
drv_i915_lcd_observer_pipe_active(
	struct i915_lcd_observer *observer,
	uint32_t *transconf)
{
	uint32_t transconf_reg;
	uint32_t value;

	/* Reads the transcoder's configuration. */
	transconf_reg = i915_mmio_reg_offset(TRANSCONF((enum transcoder)observer->pipe));
	value = observer->hw->read32(observer->hw->ctx, transconf_reg);

	/* Reports the value read when asked for. */
	if (transconf != NULL)
		*transconf = value;

	/* The state bit is clear: the transcoder is off. */
	if ((value & TRANSCONF_STATE_ENABLE) == 0u)
		return 0;

	/* Succeeded: the transcoder is on. */
	return 1;
}

/*
 * ==== The named registers and the last-resort stop ====
 */

/*
 * Fills the world's register table for a pipe, port and DPLL.
 *
 * Each row names a register by its Linux macro and, where Linux's dump of
 * this machine (same panel, same mode) has one, the value Linux left there
 * and the bits compared.  The table is refilled on every call; *n is the
 * number of rows.
 */
const struct i915_lcd_named_reg *
drv_i915_lcd_reg_table(
	struct i915_lcd_world *world,
	int pipe,
	int port,
	int dpll_id,
	unsigned *n)
{
	enum transcoder transcoder;
	unsigned count;

	/* The transcoder of the pipe has the pipe's number. */
	transcoder = (enum transcoder)pipe;
	count = 0;

	/* The DPLL enable: only its enable, lock and power bits are compared. */
	i915_reg_table_row(world, &count, "ICL_DPLL_ENABLE", ICL_DPLL_ENABLE(dpll_id), 1, 0xcc000000u, 0xcc000000u);

	/* The DPLL's configuration words. */
	i915_reg_table_row(world, &count, "TGL_DPLL_CFGCR0", TGL_DPLL_CFGCR0(dpll_id), 1, 0x00e001a5u, 0xffffffffu);
	i915_reg_table_row(world, &count, "TGL_DPLL_CFGCR1", TGL_DPLL_CFGCR1(dpll_id), 1, 0x00000088u, 0xffffffffu);

	/* The DDI clock selection, which the dump does not carry. */
	i915_reg_table_row(world, &count, "ICL_DPCLKA_CFGCR0", ICL_DPCLKA_CFGCR0, 0, 0u, 0u);

	/* The DDI buffer control: the idle bit (bit 7) is not compared. */
	i915_reg_table_row(world, &count, "DDI_BUF_CTL", DDI_BUF_CTL(port), 1, 0x80000002u, 0xffffff7fu);

	/* The transcoder's clock selection and DDI function. */
	i915_reg_table_row(world, &count, "TRANS_CLK_SEL", TRANS_CLK_SEL(transcoder), 1, 0x10000000u, 0xffffffffu);
	i915_reg_table_row(world, &count, "TRANS_DDI_FUNC_CTL", TRANS_DDI_FUNC_CTL(transcoder), 1, 0x8a210002u, 0xffffffffu);

	/* The transcoder's enable and state bits. */
	i915_reg_table_row(world, &count, "TRANSCONF", TRANSCONF(transcoder), 1, 0xc0000000u, 0xc0000000u);

	/* The pipe's misc word: not in the register dump; Linux's display_info says dither=yes, bpp=18. */
	i915_reg_table_row(world, &count, "PIPE_MISC", PIPE_MISC(pipe), 0, 0u, 0u);

	/* The primary plane's control, colour, stride and size. */
	i915_reg_table_row(world, &count, "PLANE_CTL_1", PLANE_CTL(pipe, PLANE_PRIMARY), 1, 0x94000000u, 0xffffffffu);
	i915_reg_table_row(world, &count, "PLANE_COLOR_CTL_1", PLANE_COLOR_CTL(pipe, PLANE_PRIMARY), 1, 0x00002000u, 0xffffffffu);
	i915_reg_table_row(world, &count, "PLANE_STRIDE_1", PLANE_STRIDE(pipe, PLANE_PRIMARY), 1, 0x00000078u, 0xffffffffu);
	i915_reg_table_row(world, &count, "PLANE_SIZE_1", PLANE_SIZE(pipe, PLANE_PRIMARY), 1, 0x0437077fu, 0xffffffffu);

	/* The primary plane's surface address, which differs from Linux's by nature. */
	i915_reg_table_row(world, &count, "PLANE_SURF_1", PLANE_SURF(pipe, PLANE_PRIMARY), 0, 0u, 0u);

	/* The primary plane's level-0 watermark and its DDB allocation. */
	i915_reg_table_row(world, &count, "PLANE_WM_1_0", PLANE_WM(pipe, PLANE_PRIMARY, 0), 1, 0x80004010u, 0xffffffffu);
	i915_reg_table_row(world, &count, "PLANE_BUF_CFG_1", PLANE_BUF_CFG(pipe, PLANE_PRIMARY), 1, 0x0fdb0000u, 0xffffffffu);

	/* The MBUS control and the pipe's MBUS DBOX, which the dump does not carry. */
	i915_reg_table_row(world, &count, "MBUS_CTL", MBUS_CTL, 1, 0xdc000700u, 0xffffffffu);
	i915_reg_table_row(world, &count, "PIPE_MBUS_DBOX_CTL", PIPE_MBUS_DBOX_CTL(pipe), 0, 0u, 0u);

	/* The four DBUF slices: Linux powered the first two. */
	i915_reg_table_row(world, &count, "DBUF_CTL_S1", DBUF_CTL_S(DBUF_S1), 1, 0xc043c000u, 0xffffffffu);
	i915_reg_table_row(world, &count, "DBUF_CTL_S2", DBUF_CTL_S(DBUF_S2), 1, 0xc043c000u, 0xffffffffu);
	i915_reg_table_row(world, &count, "DBUF_CTL_S3", DBUF_CTL_S(DBUF_S3), 0, 0u, 0u);
	i915_reg_table_row(world, &count, "DBUF_CTL_S4", DBUF_CTL_S(DBUF_S4), 0, 0u, 0u);

	/* The backlight PWM control. */
	i915_reg_table_row(world, &count, "BXT_BLC_PWM_CTL", BXT_BLC_PWM_CTL(0), 0, 0u, 0u);

	/*
	 * The backlight PWM frequency at 0xc8254: the dump tool labels it
	 * BLC_PWM_PCH_CTL2; the CNP backlight code of the reference names it
	 * BXT_BLC_PWM_FREQ(0).
	 */
	i915_reg_table_row(world, &count, "BXT_BLC_PWM_FREQ", BXT_BLC_PWM_FREQ(0), 1, 0x00017700u, 0xffffffffu);

	/* The backlight PWM duty cycle. */
	i915_reg_table_row(world, &count, "BXT_BLC_PWM_DUTY", BXT_BLC_PWM_DUTY(0), 0, 0u, 0u);

	/* The pipe's status and interrupt registers and its frame counter. */
	i915_reg_table_row(world, &count, "ICL_PIPESTATUS", ICL_PIPESTATUS(pipe), 0, 0u, 0u);
	i915_reg_table_row(world, &count, "GEN8_DE_PIPE_IMR", GEN8_DE_PIPE_IMR(pipe), 0, 0u, 0u);
	i915_reg_table_row(world, &count, "GEN8_DE_PIPE_IER", GEN8_DE_PIPE_IER(pipe), 0, 0u, 0u);
	i915_reg_table_row(world, &count, "GEN8_DE_PIPE_IIR", GEN8_DE_PIPE_IIR(pipe), 0, 0u, 0u);
	i915_reg_table_row(world, &count, "PIPE_FRMCOUNT_G4X", PIPE_FRMCOUNT_G4X(pipe), 0, 0u, 0u);

	/* Reports the number of rows filled. */
	*n = count;

	/* Succeeded: the table lives in the world. */
	return world->table;
}

/*
 * Returns the offset of a named register of pipe A, port A and DPLL 0.
 *
 * The name is one of the table's row names; an unknown name gives 0.
 */
uint32_t
drv_i915_lcd_reg_by_name(
	struct i915_lcd_world *world,
	const char *name)
{
	const struct i915_lcd_named_reg *table;
	unsigned count;
	unsigned index;
	int order;

	/* Fills the table for pipe A, port A and DPLL 0. */
	count = 0;
	table = drv_i915_lcd_reg_table(world, 0, 0, 0, &count);

	/* Looks the name up among the rows. */
	for (index = 0; index < count; index++) {
		order = kern_strcmp(table[index].name, name);
		if (order == 0)
			return table[index].reg;
	}

	/* No row has that name. */
	return 0u;
}

/*
 * Returns the reference's DBUF_CTL_S(slice) offset.
 *
 * It is the saved reference's own definition (the extracted
 * skl_watermark_regs.h), for an independent check of the table the power
 * domain initialisation uses; that table was once found shifted by one
 * slice.  Slice 0 is S1; a slice beyond S4 gives 0.
 */
uint32_t
drv_i915_lcd_ref_dbuf_ctl(
	unsigned slice)
{
	/* Only the four slices of the reference exist. */
	if (slice >= 4u)
		return 0u;

	/* Succeeded: the offset of that slice's control. */
	return i915_mmio_reg_offset(DBUF_CTL_S((enum dbuf_slice)slice));
}

/*
 * Stops every pipe and DDI buffer that is still on.
 *
 * This is the last resort at the end of the device, not a Linux path: a
 * pipe left running would keep reading memory after the driver let it go,
 * which on a VFIO test host hangs the IOMMU unmap when the guest ends.
 * Each wait is a fixed number of polls, and every step is logged.  Returns
 * how many pipes and buffers were stopped.
 */
unsigned
drv_i915_lcd_last_resort_stop(
	struct i915_mmio *mmio)
{
	unsigned stopped;
	unsigned pipe;
	unsigned ddi;
	unsigned poll;
	uint32_t base;
	uint32_t transconf;
	uint32_t plane_ctl;
	uint32_t state;
	uint32_t reg;
	uint32_t buf;

	stopped = 0u;

	/* Stops every pipe whose transcoder or primary plane is still on. */
	for (pipe = 0u; pipe < I915_LAST_RESORT_PIPES; pipe++) {
		/* Reads the pipe's transcoder and primary plane. */
		base = I915_LAST_RESORT_PIPE_STRIDE * pipe;
		transconf = drv_i915_read32(mmio, I915_LAST_RESORT_TRANSCONF + base);
		plane_ctl = drv_i915_read32(mmio, I915_LAST_RESORT_PLANE_CTL + base);

		/* A pipe with both off needs nothing. */
		if ((transconf & I915_LAST_RESORT_ENABLE) == 0u && (plane_ctl & I915_LAST_RESORT_ENABLE) == 0u)
			continue;

		/* Reports the pipe found on. */
		kern_logf("i915: LAST-RESORT pipe %u is still on (TRANSCONF=0x%08x PLANE_CTL=0x%08x): stopping it so that nothing is scanned out when the driver is gone\n", pipe, transconf, plane_ctl);

		/* Turns the plane off, arms that through PLANE_SURF, and turns the transcoder off. */
		drv_i915_write32(mmio, I915_LAST_RESORT_PLANE_CTL + base, plane_ctl & ~I915_LAST_RESORT_ENABLE);
		drv_i915_write32(mmio, I915_LAST_RESORT_PLANE_SURF + base, 0u);
		drv_i915_write32(mmio, I915_LAST_RESORT_TRANSCONF + base, transconf & ~I915_LAST_RESORT_ENABLE);

		/* Waits a bounded time for the transcoder to report itself off. */
		for (poll = 0u; poll < I915_LAST_RESORT_POLLS; poll++) {
			state = drv_i915_read32(mmio, I915_LAST_RESORT_TRANSCONF + base);
			if ((state & I915_LAST_RESORT_STATE) == 0u)
				break;
			kern_usleep_range(I915_LAST_RESORT_POLL_MIN_US, I915_LAST_RESORT_POLL_MAX_US);
		}

		/* Reads the pipe back after the wait, transcoder first. */
		transconf = drv_i915_read32(mmio, I915_LAST_RESORT_TRANSCONF + base);
		plane_ctl = drv_i915_read32(mmio, I915_LAST_RESORT_PLANE_CTL + base);

		/* Reports what the pipe reads after the wait. */
		kern_logf("i915: LAST-RESORT pipe %u after %u ms: TRANSCONF=0x%08x PLANE_CTL=0x%08x\n", pipe, poll, transconf, plane_ctl);
		stopped++;
	}

	/* Disables the DDI A and B buffers that are still enabled. */
	for (ddi = 0u; ddi < I915_LAST_RESORT_DDIS; ddi++) {
		/* Reads the buffer's control. */
		reg = I915_LAST_RESORT_DDI_BUF_CTL + I915_LAST_RESORT_DDI_STRIDE * ddi;
		buf = drv_i915_read32(mmio, reg);

		/* A disabled buffer needs nothing. */
		if ((buf & I915_LAST_RESORT_ENABLE) == 0u)
			continue;

		/* Disables the buffer and reports it. */
		drv_i915_write32(mmio, reg, buf & ~I915_LAST_RESORT_ENABLE);
		kern_logf("i915: LAST-RESORT DDI %c buffer was enabled (0x%08x): disabled\n", (char)(65 + ddi), buf);
		stopped++;
	}

	/* Succeeded: reports how many pipes and buffers were stopped. */
	return stopped;
}

/*
 * ==== The readout survey ====
 */

/*
 * Logs what the firmware left enabled on the display.
 *
 * This is a diagnostic only and changes no state.  Before the readout and
 * sanitize, it shows whether any pipe, DDI buffer, PLL or power well was
 * left on, which decides whether the sanitize must drive a full crtc
 * disable.  Pipe, transcoder and plane registers live behind the pipe
 * power wells, so a pipe is read only when its pipe or transcoder domain
 * is enabled, as the Linux readout gates it.  The caller records the stage
 * as completed.
 */
void
drv_i915_display_survey(
	struct i915_display *display)
{
	struct i915_power_well *well;
	struct i915_mmio *mmio;
	unsigned index;
	unsigned wells_on;
	unsigned wells_on_unused;
	uint32_t base;
	uint32_t transconf;
	uint32_t trans_ddi_func_ctl;
	uint32_t plane_ctl;
	uint32_t value;
	int powered;
	int tpowered;

	mmio = display->pwc.mmio;
	wells_on = 0u;
	wells_on_unused = 0u;

	/* Reads the transcoder, DDI function and primary plane of every powered pipe. */
	for (index = 0u; index < I915_SURVEY_PIPES; index++) {
		/* Asks whether the pipe's and the transcoder's power domains are enabled. */
		powered = drv_i915_display_power_is_enabled(&display->power_domains,
							    (enum i915_power_domain)(I915_PW_DOMAIN_PIPE_A + index),
							    &display->pwc);
		tpowered = drv_i915_display_power_is_enabled(&display->power_domains,
							     (enum i915_power_domain)(I915_PW_DOMAIN_TRANSCODER_A + index),
							     &display->pwc);

		/* A pipe with both domains off would read garbage: it is only reported. */
		if (!powered && !tpowered) {
			kern_logf("i915: P5-0 survey pipe %c: power OFF "
				"(pipe=%d trans=%d) -- not read\n",
				(char)('A' + index),
				powered,
				tpowered);
			continue;
		}

		/* Reads the pipe's registers. */
		base = index * I915_SURVEY_PIPE_STRIDE;
		transconf = drv_i915_read32(mmio, I915_SURVEY_TRANSCONF + base);
		trans_ddi_func_ctl = drv_i915_read32(mmio, I915_SURVEY_TRANS_DDI_FUNC_CTL + base);
		plane_ctl = drv_i915_read32(mmio, I915_SURVEY_PLANE_CTL + base);

		/* Reports them. */
		kern_logf("i915: P5-0 survey pipe %c: power(pipe=%d trans=%d) "
			"TRANSCONF=0x%08x TRANS_DDI_FUNC_CTL=0x%08x PLANE_CTL(1)=0x%08x\n",
			(char)('A' + index),
			powered,
			tpowered,
			transconf,
			trans_ddi_func_ctl,
			plane_ctl);
	}

	/* Reads and reports the buffer control of every DDI port. */
	for (index = 0u; index < I915_SURVEY_DDIS; index++) {
		value = drv_i915_read32(mmio, I915_SURVEY_DDI_BUF_CTL + i915_survey_ddi_ports[index] * I915_SURVEY_DDI_STRIDE);
		kern_logf("i915: P5-0 survey DDI %s: DDI_BUF_CTL=0x%08x\n",
			i915_survey_ddi_names[index],
			value);
	}

	/* Reads and reports the enable register of every PLL. */
	for (index = 0u; index < I915_SURVEY_PLLS; index++) {
		value = drv_i915_read32(mmio, i915_survey_pll_regs[index]);
		kern_logf("i915: P5-0 survey PLL %s: ENABLE=0x%08x\n",
			i915_survey_pll_names[index],
			value);
	}

	/* Counts the wells that are on, and reports each one on that nothing holds. */
	for (index = 0u; index < display->power_domains.num_power_wells; index++) {
		well = &display->power_domains.power_wells[index];

		/* A well not known to be on is not counted. */
		if (well->hw_enabled != 1)
			continue;
		wells_on++;

		/* An on well that is not always on and has no reference is a sanitize candidate. */
		if (!well->always_on && well->refcount == 0u) {
			wells_on_unused++;
			kern_logf("i915: P5-0 survey well '%s': ON but refcount=0 "
				"(a sanitize candidate)\n",
				well->name);
		}
	}

	/* Reports the well totals. */
	kern_logf("i915: P5-0 survey wells: total=%u on=%u on_unused=%u\n",
		display->power_domains.num_power_wells,
		wells_on,
		wells_on_unused);
}

/*
 * ==== The test picture ====
 */

/*
 * Returns the pixel of the test picture at (x, y).
 *
 * A point outside the picture is black.
 */
uint32_t
drv_i915_lcd_pattern_pixel(
	uint32_t x,
	uint32_t y,
	uint32_t width,
	uint32_t height,
	unsigned test_id)
{
	uint32_t frame;
	uint32_t corner_w;
	uint32_t corner_h;
	uint32_t band_h;
	uint32_t span;
	uint32_t grey;
	uint32_t fx;
	uint32_t fy;
	uint32_t fw;
	uint32_t fh;
	uint32_t ft;
	uint32_t dw;
	uint32_t dh;
	uint32_t dt;
	uint32_t dx;
	uint32_t dy;
	uint32_t i;
	unsigned digit;
	int inside;

	/* Sizes the frame, the corner blocks and the two bands from the picture. */
	frame = I915_PATTERN_FRAME;
	corner_w = width / 8u;
	corner_h = height / 6u;
	band_h = height / 12u;

	/* A point outside the picture is black. */
	if (x >= width || y >= height)
		return 0u;

	/* The white frame on the very edge of the active area. */
	if (x < frame ||
	    y < frame ||
	    x >= width - frame ||
	    y >= height - frame)
		return I915_PATTERN_WHITE;

	/* The red block in the top left corner. */
	inside = i915_pattern_in_rect(x, y, frame, frame, corner_w, corner_h);
	if (inside)
		return I915_PATTERN_RED;

	/* The green block in the top right corner. */
	inside = i915_pattern_in_rect(x, y, width - frame - corner_w, frame, corner_w, corner_h);
	if (inside)
		return I915_PATTERN_GREEN;

	/* The blue block in the bottom left corner. */
	inside = i915_pattern_in_rect(x, y, frame, height - frame - corner_h, corner_w, corner_h);
	if (inside)
		return I915_PATTERN_BLUE;

	/* The yellow block in the bottom right corner. */
	inside = i915_pattern_in_rect(x, y, width - frame - corner_w, height - frame - corner_h, corner_w, corner_h);
	if (inside)
		return I915_PATTERN_YELLOW;

	/* The grey ramp along the top band, between the corner blocks. */
	if (y >= frame &&
	    y < frame + band_h &&
	    x >= frame + corner_w &&
	    x < width - frame - corner_w) {
		span = width - 2u * (frame + corner_w);
		grey = ((x - frame - corner_w) * 255u) / (span - 1u);
		return (grey << 16) | (grey << 8) | grey;
	}

	/* The eight colour bars along the bottom band, between the corner blocks. */
	if (y >= height - frame - band_h &&
	    y < height - frame &&
	    x >= frame + corner_w &&
	    x < width - frame - corner_w) {
		span = width - 2u * (frame + corner_w);
		return i915_pattern_bars[((x - frame - corner_w) * 8u) / span];
	}

	/* Sizes and places the letter F, left of the centre. */
	fh = height / 2u;
	fw = fh / 2u;
	ft = fh / 8u;
	fx = width / 4u - fw / 2u;
	fy = height / 4u;

	/* The stem of the F. */
	inside = i915_pattern_in_rect(x, y, fx, fy, ft, fh);
	if (inside)
		return I915_PATTERN_WHITE;

	/* The top bar of the F. */
	inside = i915_pattern_in_rect(x, y, fx, fy, fw, ft);
	if (inside)
		return I915_PATTERN_WHITE;

	/* The middle bar of the F. */
	inside = i915_pattern_in_rect(x, y, fx, fy + fh / 2u - ft, (fw * 3u) / 4u, ft);
	if (inside)
		return I915_PATTERN_WHITE;

	/* Sizes and places the three seven-segment digits of the test id, right of the centre. */
	dh = height / 3u;
	dw = dh / 2u;
	dt = dh / 10u;
	dx = width / 2u + width / 16u;
	dy = height / 3u;

	/* Draws the hundreds, the tens and the units of the test id. */
	for (i = 0u; i < 3u; i++) {
		/* Picks the digit this position shows. */
		if (i == 0u) {
			digit = (test_id / 100u) % 10u;
		} else if (i == 1u) {
			digit = (test_id / 10u) % 10u;
		} else {
			digit = test_id % 10u;
		}

		/* A lit segment of the digit is yellow. */
		inside = i915_pattern_in_digit(x, y, dx + i * (dw + dw / 2u), dy, dw, dh, dt, digit);
		if (inside)
			return I915_PATTERN_YELLOW;
	}

	/* Succeeded: everything else is background. */
	return I915_PATTERN_BACKGROUND;
}

/*
 * Draws the test picture into a buffer.
 *
 * pitch is the distance between two rows in bytes.  Returns the FNV-1a
 * hash of the pixels, each hashed as four bytes, low byte first.
 */
uint64_t
drv_i915_lcd_pattern_fill(
	uint32_t *pixels,
	uint32_t pitch,
	uint32_t width,
	uint32_t height,
	unsigned test_id)
{
	uint64_t hash;
	uint32_t *row;
	uint32_t pixel;
	uint32_t x;
	uint32_t y;
	unsigned byte;

	hash = I915_PATTERN_FNV_BASIS;

	/* Draws every row of the picture. */
	for (y = 0u; y < height; y++) {
		row = (uint32_t *)((uint8_t *)pixels + (uint64_t)y * pitch);

		/* Draws every pixel of the row and hashes it. */
		for (x = 0u; x < width; x++) {
			pixel = drv_i915_lcd_pattern_pixel(x, y, width, height, test_id);
			row[x] = pixel;

			/* Hashes the pixel's four bytes, low byte first. */
			for (byte = 0u; byte < 4u; byte++) {
				hash ^= (pixel >> (8u * byte)) & 0xffu;
				hash *= I915_PATTERN_FNV_PRIME;
			}
		}
	}

	/* Succeeded: the picture is drawn; its hash identifies it. */
	return hash;
}

/*
 * Counts the pixels of a buffer that differ from the test picture.
 *
 * pitch is the distance between two rows in bytes.  The position of the
 * first differing pixel is reported when asked for and when there is one.
 */
uint32_t
drv_i915_lcd_pattern_verify(
	const uint32_t *pixels,
	uint32_t pitch,
	uint32_t width,
	uint32_t height,
	unsigned test_id,
	uint32_t *first_x,
	uint32_t *first_y)
{
	const uint32_t *row;
	uint32_t expected;
	uint32_t x;
	uint32_t y;
	uint32_t bad;

	bad = 0u;

	/* Compares every row of the buffer. */
	for (y = 0u; y < height; y++) {
		row = (const uint32_t *)((const uint8_t *)pixels + (uint64_t)y * pitch);

		/* Compares every pixel of the row with the picture. */
		for (x = 0u; x < width; x++) {
			expected = drv_i915_lcd_pattern_pixel(x, y, width, height, test_id);
			if (row[x] == expected)
				continue;

			/* The first difference is where the report points. */
			if (bad == 0u) {
				if (first_x != NULL)
					*first_x = x;
				if (first_y != NULL)
					*first_y = y;
			}

			bad++;
		}
	}

	/* Succeeded: reports the number of differing pixels. */
	return bad;
}

/*
 * ==== The panel run's hooks and log ====
 */

/*
 * Prints one note of the Linux text.
 *
 * This is the note sink bound while the notes are traced.
 */
void
drv_i915_lcd_kernel_note_sink(
	const char *fmt)
{
	/* The note is printed as it is, after the prefix. */
	kern_logf("i915: N1 note: %s", fmt);
}

/*
 * Counts and prints a callee of the Linux text that is not ported.
 *
 * This is the step hook of a panel run; ctx is the struct i915_lcd_kernel.
 * A decided callee was deliberately not connected; any other one is
 * unresolved and must not pass silently on hardware.
 */
void
drv_i915_lcd_kernel_step(
	void *ctx,
	const char *name)
{
	struct i915_lcd_kernel *k;
	int prefix;

	k = ctx;

	/* A decided callee is counted and printed as it is. */
	prefix = kern_strncmp(name, I915_TRACE_DECIDED_PREFIX, I915_TRACE_DECIDED_PREFIX_LEN);
	if (prefix == 0) {
		k->decided++;
		kern_logf("i915: LCD-B %s\n", name);
		return;
	}

	/* Any other callee is unresolved. */
	k->unresolved_steps++;
	kern_logf("i915: LCD-B UNRESOLVED step reached: %s\n", name);
}

/*
 * Counts and prints an error of the Linux text.
 *
 * This is the error hook of a panel run; ctx is the struct i915_lcd_kernel.
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
	kern_logf("i915: LCD-B reference error (%s): %s%s", phase, what, line_end);
}

/*
 * Ignores a debug message of the Linux text.
 *
 * This is the debug hook of a panel run; ctx is the struct i915_lcd_kernel.
 */
void
drv_i915_lcd_kernel_debug(
	void *ctx,
	const char *what)
{
	UNUSED_PARAMETER(ctx);
	UNUSED_PARAMETER(what);
}

/*
 * Logs the named registers of pipe A, port A and DPLL 0.
 *
 * With compare set, each register Linux's dump has a value for is printed
 * next to it with the verdict, and the number of matches is printed last;
 * no value is copied from the dump.
 */
void
drv_i915_lcd_log_regs(
	struct i915_lcd_world *world,
	struct i915_lcd_kernel *k,
	const char *when,
	int compare)
{
	const struct i915_lcd_named_reg *table;
	const char *verdict;
	unsigned count;
	unsigned index;
	unsigned match;
	unsigned compared;
	uint32_t value;

	/* Fills the table for pipe A, port A and DPLL 0. */
	count = 0u;
	match = 0u;
	compared = 0u;
	table = drv_i915_lcd_reg_table(world, 0, 0, 0, &count);

	/* Reads and prints every row. */
	for (index = 0u; index < count; index++) {
		value = drv_i915_read32(k->d->mmio, table[index].reg);

		/* A row with a Linux value is compared under its mask when asked for. */
		if (compare && table[index].has_linux) {
			compared++;
			if ((value & table[index].compare_mask) == (table[index].linux_value & table[index].compare_mask)) {
				match++;
				verdict = "same";
			} else {
				verdict = "DIFFERENT";
			}
			kern_logf("i915: LCD-B reg[%s] %s 0x%05x = 0x%08x | Linux 0x%08x mask 0x%08x %s\n",
				when,
				table[index].name,
				table[index].reg,
				value,
				table[index].linux_value,
				table[index].compare_mask,
				verdict);
		} else {
			kern_logf("i915: LCD-B reg[%s] %s 0x%05x = 0x%08x\n",
				when,
				table[index].name,
				table[index].reg,
				value);
		}
	}

	/* Prints the number of matches of a comparison. */
	if (compare) {
		kern_logf("i915: LCD-B reg[%s] same as Linux's dump: %u/%u (comparison only; no value above was copied from it)\n",
			when,
			match,
			compared);
	}
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
	kern_logf("i915: LCD-B run log: %u entries (%u writes, %u rmw, %u waits [%u timed out], %u unresolved steps, "
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
			kern_logf("i915: LCD-B t[%3u] %s %s%s", index, kind, entry->name, line_end);
		} else {
			kern_logf("i915: LCD-B t[%3u] %s 0x%05x b=0x%08x c=0x%08x d=0x%08x rc=%d n=%u\n",
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
	kern_logf("i915: LCD-B observer: ICL_PIPESTATUS 0x%05x underrun mask 0x%08x (bit31 pipe underrun, bit28 soft, bit27 hard, "
		"bit26 port) | PIPE_FRMCOUNT_G4X 0x%05x | GEN8_DE_PIPE_IMR 0x%05x vblank bit 0x%x | the test owns the status for the run; "
		"the pipe's underrun and vblank interrupts stay masked, so the IRQ handler never reads or clears it\n",
		observer->status_reg,
		observer->underrun_mask,
		observer->frame_reg,
		observer->imr_reg,
		observer->vblank_bit);

	/* Prints what the observer found in each period. */
	kern_logf("i915: LCD-B observer: status found before the run 0x%08x (kept, not judged) | start/stop periods 0x%08x | "
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
		kern_logf("i915: LCD-B observer sample %2u point=%3d %s status=0x%08x frame=%u imr=0x%08x\n",
			index,
			observer->s[index].point,
			period,
			observer->s[index].status,
			observer->s[index].frame,
			observer->s[index].imr);
	}
}

/*
 * Logs a modeset status: the software state, then what the sink reported.
 */
void
drv_i915_lcd_log_status(
	const char *when,
	const struct i915_lcd_modeset_status *status)
{
	/* Prints what the software holds. */
	kern_logf("i915: LCD-B state[%s] software: prepared=%d crtc_active=%d plane_armed=%d pll_on=%d pll_mask=0x%x "
		"wakerefs io=%d aux=%d crtc_domains=%u dc_off_held=%d dbuf_slices=0x%x mbus_joined=%d stop_unconfirmed=%d backlight=%d "
		"level=%u/%u errors=%u\n",
		when,
		status->prepared,
		status->crtc_active,
		status->plane_armed,
		status->pll_on,
		status->pll_active_mask,
		status->ddi_io_wakeref,
		status->aux_wakeref,
		status->crtc_domains_held,
		status->dc_off_held,
		status->dbuf_slices_now,
		status->mbus_joined_now,
		status->stop_unconfirmed,
		status->backlight_enabled,
		status->backlight_level,
		status->backlight_pwm_max,
		status->errors);

	/* Prints what the sink reported about the link. */
	kern_logf("i915: LCD-B state[%s] sink: link %d x%d rc=%d status %02x %02x %02x %02x %02x %02x cr_ok=%d eq_ok=%d "
		"train_set %02x %02x (driver flag link_trained=%d is not evidence)\n",
		when,
		status->link_rate,
		status->lane_count,
		status->link_status_rc,
		status->link_status[0],
		status->link_status[1],
		status->link_status[2],
		status->link_status[3],
		status->link_status[4],
		status->link_status[5],
		status->cr_ok,
		status->eq_ok,
		status->train_set[0],
		status->train_set[1],
		status->link_trained_flag);
}

/*
 * Reports whether a panel run kept display resources.
 *
 * A run keeps them when its stop was not confirmed, either by the show
 * path or by the modeset object.  Returns 1 when it did, 0 otherwise.
 */
int
drv_i915_lcd_kernel_abandoned(
	struct i915_display *display)
{
	int retained;

	/* The show path kept its buffer and hooks. */
	retained = drv_i915_lcd_show_retained(display);
	if (retained)
		return 1;

	/* The modeset object kept what it holds. */
	retained = drv_i915_lcd_modeset_retained(display);
	if (retained)
		return 1;

	/* Succeeded: nothing was kept. */
	return 0;
}

/*
 * Reports whether a panel run kept a GPU buffer.
 *
 * The buffer is kept when the GPU was not shown to be done with it.
 */
int
drv_i915_lcd_kernel_gpu_retained(
	struct i915_display *display)
{
	int retained;

	/* Asks the show path whether it kept the GPU buffer. */
	retained = drv_i915_lcd_show_gpu_retained(display);

	/* Succeeded: reports whether the buffer is kept. */
	return retained;
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
		entry->c = 15u;
		entry->rc = result;
		entry->name = "drm_dp_read_dpcd_caps";
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

/* Forwards a power domain get and records the wakeref it returned. */
static int
i915_trace_power_get(
	void *ctx,
	int domain)
{
	struct i915_lcd_trace *trace;
	struct i915_lcd_trace_entry *entry;
	int wakeref;

	trace = ctx;

	/* Takes the reference first, then the entry. */
	wakeref = trace->backend->power_get(trace->backend->ctx, domain);
	entry = i915_trace_add(trace, I915_LCD_T_POWER_GET);

	/* Records the domain and the wakeref. */
	if (entry != NULL) {
		entry->a = (uint32_t)domain;
		entry->rc = wakeref;
	}

	/* Succeeded: the wakeref, 0 when the get failed. */
	return wakeref;
}

/* Records a power domain put, then forwards it. */
static void
i915_trace_power_put(
	void *ctx,
	int domain,
	int wakeref)
{
	struct i915_lcd_trace *trace;
	struct i915_lcd_trace_entry *entry;

	trace = ctx;

	/* Records the domain and the wakeref returned. */
	entry = i915_trace_add(trace, I915_LCD_T_POWER_PUT);
	if (entry != NULL) {
		entry->a = (uint32_t)domain;
		entry->b = (uint32_t)wakeref;
	}

	/* Forwards the put. */
	trace->backend->power_put(trace->backend->ctx, domain, wakeref);
}

/* Records an asynchronous power domain put with its delay, then forwards it. */
static void
i915_trace_power_put_async(
	void *ctx,
	int domain,
	int wakeref,
	int delay_ms)
{
	struct i915_lcd_trace *trace;
	struct i915_lcd_trace_entry *entry;

	trace = ctx;

	/* Records the put; d = 1 marks it asynchronous. */
	entry = i915_trace_add(trace, I915_LCD_T_POWER_PUT);
	if (entry != NULL) {
		entry->a = (uint32_t)domain;
		entry->b = (uint32_t)wakeref;
		entry->c = (uint32_t)delay_ms;
		entry->d = 1u;
	}

	/* Forwards the put. */
	trace->backend->power_put_async(trace->backend->ctx, domain, wakeref, delay_ms);
}

/* Records a DBUF slice request, then forwards it. */
static void
i915_trace_dbuf_slices_update(
	void *ctx,
	unsigned req_slices)
{
	struct i915_lcd_trace *trace;
	struct i915_lcd_trace_entry *entry;

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

	trace = ctx;

	/* Cancels the pending event. */
	trace->backend->cancel_event(trace->backend->ctx, pipe);
}

/* Forwards a modeset lock operation; it is not recorded. */
static void
i915_trace_lock(
	void *ctx,
	int which,
	int take)
{
	struct i915_lcd_trace *trace;

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

/* Records an error of the Linux text, remembering where the first one is, then forwards it. */
static void
i915_trace_error(
	void *ctx,
	const char *what)
{
	struct i915_lcd_trace *trace;

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

	trace = ctx;

	/* The backend's debug hook is optional. */
	if (trace->backend->debug != NULL)
		trace->backend->debug(trace->backend->ctx, what);
}

/* Reports whether entries of a kind are matched by name rather than by operand. */
static int
i915_trace_kind_is_named(
	int kind)
{
	/* Steps, errors, phases and decided callees carry a name. */
	switch (kind) {
	case I915_LCD_T_STEP:
	case I915_LCD_T_ERROR:
	case I915_LCD_T_PHASE:
	case I915_LCD_T_DECIDED:
		return 1;
	default:
		break;
	}

	/* Succeeded: any other kind carries operands. */
	return 0;
}

/*
 * Samples the pipe's status, interrupt mask and frame counter at a point.
 *
 * The first sample keeps what was left before the run; the underrun-arm
 * point clears the status as the Linux text does; the pipe-disabled point
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
		transconf_reg = i915_mmio_reg_offset(TRANSCONF((enum transcoder)observer->pipe));
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
	 * then, at the reference's position, bdw_set_fifo_underrun_reporting()
	 * writes the whole underrun mask.
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

/* Appends one row to the world's register table while it has room. */
static void
i915_reg_table_row(
	struct i915_lcd_world *world,
	unsigned *count,
	const char *name,
	i915_reg_t reg,
	int has_linux,
	uint32_t linux_value,
	uint32_t compare_mask)
{
	struct i915_lcd_named_reg *row;

	/* A full table takes no more rows. */
	if (*count >= sizeof(world->table) / sizeof(world->table[0]))
		return;

	/* Fills the next row. */
	row = &world->table[*count];
	row->name = name;
	row->reg = i915_mmio_reg_offset(reg);
	row->has_linux = has_linux;
	row->linux_value = linux_value;
	row->compare_mask = compare_mask;
	(*count)++;
}

/* Reports whether (x, y) lies in the w x h rectangle at (x0, y0). */
static int
i915_pattern_in_rect(
	uint32_t x,
	uint32_t y,
	uint32_t x0,
	uint32_t y0,
	uint32_t w,
	uint32_t h)
{
	/* Left of or right of the rectangle. */
	if (x < x0)
		return 0;
	if (x >= x0 + w)
		return 0;

	/* Above or below the rectangle. */
	if (y < y0)
		return 0;
	if (y >= y0 + h)
		return 0;

	/* Succeeded: the point is inside. */
	return 1;
}

/* Reports whether (x, y) lies on a lit segment of a digit in the w x h box at (x0, y0) with segments t thick. */
static int
i915_pattern_in_digit(
	uint32_t x,
	uint32_t y,
	uint32_t x0,
	uint32_t y0,
	uint32_t w,
	uint32_t h,
	uint32_t t,
	unsigned digit)
{
	uint32_t half;
	uint8_t segments;
	int inside;

	/* Looks the digit's segments up; the box splits at half its height. */
	segments = i915_pattern_seven_segments[digit % 10u];
	half = h / 2u;

	/* Segment a: the top. */
	if ((segments & 0x01u) != 0u) {
		inside = i915_pattern_in_rect(x, y, x0, y0, w, t);
		if (inside)
			return 1;
	}

	/* Segment b: the upper right. */
	if ((segments & 0x02u) != 0u) {
		inside = i915_pattern_in_rect(x, y, x0 + w - t, y0, t, half);
		if (inside)
			return 1;
	}

	/* Segment c: the lower right. */
	if ((segments & 0x04u) != 0u) {
		inside = i915_pattern_in_rect(x, y, x0 + w - t, y0 + half, t, h - half);
		if (inside)
			return 1;
	}

	/* Segment d: the bottom. */
	if ((segments & 0x08u) != 0u) {
		inside = i915_pattern_in_rect(x, y, x0, y0 + h - t, w, t);
		if (inside)
			return 1;
	}

	/* Segment e: the lower left. */
	if ((segments & 0x10u) != 0u) {
		inside = i915_pattern_in_rect(x, y, x0, y0 + half, t, h - half);
		if (inside)
			return 1;
	}

	/* Segment f: the upper left. */
	if ((segments & 0x20u) != 0u) {
		inside = i915_pattern_in_rect(x, y, x0, y0, t, half);
		if (inside)
			return 1;
	}

	/* Segment g: the middle. */
	if ((segments & 0x40u) != 0u) {
		inside = i915_pattern_in_rect(x, y, x0, y0 + half - t / 2u, w, t);
		if (inside)
			return 1;
	}

	/* Succeeded: the point is on no lit segment. */
	return 0;
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
