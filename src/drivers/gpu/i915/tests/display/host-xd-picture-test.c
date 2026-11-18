/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The host test of the pictures of a display (display/xd-picture.c) on
 * the register model: what a prepare refuses before anything is touched;
 * the 5330's panel prepared, lit, flipped, its brightness and backlight,
 * and darkened, with what the status reports at each step and the power
 * and locks given back; a second screen beside the lit panel on the
 * other combo PLL, or on the same one when its words are the same, and
 * none left when both run other words; the display buffer the commits
 * leave; a dark that is not confirmed, the screen it retains, and the
 * model's discard that is its only way out; and an external DisplayPort
 * sink on TC1 reserving its port's PLL and the Thunderbolt PLL.
 */

#include "host-test.h"
#include "xd-fake-regs.h"

#include "../../display/xd-io.h"
#include "../../display/xd-picture.h"
#include "../../display/xd-platform.h"
#include "../../display/xd-screen.h"
#include "../../intel/display-dpcd-xd.h"
#include "../../intel/display-regs-xd.h"

#include <uapi/errno.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* The 5330's Alder Lake-P. */
#define I915_XD_PT_ADLP			0x46a6U
#define I915_XD_PT_ADLP_REVID		0x0cU

/* The two buffers the panel flips between, and a third that is not page-aligned. */
#define I915_XD_PT_SURF_A		0x00100000U
#define I915_XD_PT_SURF_B		0x00900000U
#define I915_XD_PT_SURF_ODD		0x00900010U

/* The most domains the model counts references on (the backend's numbering is use * 16 + unit). */
#define I915_XD_PT_DOMAINS		128U

/*
 * The backend the screens are prepared with: the register model, a sink
 * that trains at once, and counts of what the screens take and give
 * back.  power_refs counts the references on each domain; fail_put, when
 * not 0, is a domain whose put is reported as a power well that would
 * not turn off.  irq_off is nonzero inside a flip's short update.
 */
struct i915_xd_pt_backend {
	struct i915_xd_backend hooks;
	struct i915_xd_io_hooks io;
	int power_refs[I915_XD_PT_DOMAINS];
	int fail_put;
	unsigned locks_held;
	unsigned lock_errors;
	int irq_off;
	unsigned irq_errors;
	int vblank_refs;
	unsigned events_armed;
	unsigned errors;
	uint8_t sink_pattern;
	uint8_t sink_lanes;
	unsigned slices_asked;
};

static struct i915_xd_pictures i915_xd_pt_pictures;
static struct i915_xd_pt_backend i915_xd_pt_model;
static struct i915_xd_pt_backend i915_xd_pt_other;

static const struct i915_xd_platform *i915_xd_pt_setup(void);
static void i915_xd_pt_backend_init(struct i915_xd_pt_backend *backend);
static void i915_xd_pt_panel_config(struct i915_xd_picture_config *config);
static void i915_xd_pt_hdmi_config(struct i915_xd_picture_config *config, uint32_t cfgcr0);
static int i915_xd_pt_refs(const struct i915_xd_pt_backend *backend);
static void i915_xd_pt_buffer(uint32_t offset, uint32_t value, void *context);
static void i915_xd_pt_write32(void *ctx, uint32_t reg, uint32_t value);
static uint32_t i915_xd_pt_rmw32(void *ctx, uint32_t reg, uint32_t clear, uint32_t set);
static uint32_t i915_xd_pt_read32(void *ctx, uint32_t reg);
static int i915_xd_pt_wait_reg(void *ctx, uint32_t reg, uint32_t mask, uint32_t value, unsigned timeout_ms);
static void i915_xd_pt_sleep(void *ctx, unsigned us);
static void i915_xd_pt_step(void *ctx, const char *name);
static long i915_xd_pt_dpcd_read(void *ctx, unsigned offset, uint8_t *buf, size_t size);
static long i915_xd_pt_dpcd_write(void *ctx, unsigned offset, const uint8_t *buf, size_t size);
static int i915_xd_pt_read_caps(void *ctx, uint8_t dpcd[15]);
static int i915_xd_pt_panel(void *ctx, int op);
static int i915_xd_pt_power_domain(void *ctx, unsigned use, unsigned index, int thunderbolt);
static int i915_xd_pt_power_get(void *ctx, int domain);
static void i915_xd_pt_power_put(void *ctx, int domain, int wakeref);
static void i915_xd_pt_power_put_async(void *ctx, int domain, int wakeref, int delay_ms);
static void i915_xd_pt_dbuf_slices(void *ctx, unsigned slices);
static int i915_xd_pt_vblank_get(void *ctx, int pipe);
static void i915_xd_pt_vblank_put(void *ctx, int pipe);
static long i915_xd_pt_vblank_sleep(void *ctx, int pipe, long ticks);
static void i915_xd_pt_irq_off(void *ctx);
static void i915_xd_pt_irq_on(void *ctx);
static void i915_xd_pt_arm_event(void *ctx, int pipe);
static int i915_xd_pt_wait_event(void *ctx, int pipe, unsigned timeout_ms);
static void i915_xd_pt_lock(void *ctx, int which, int take);
static void i915_xd_pt_error(void *ctx, const char *what);
static void i915_xd_pt_test_refused(void);
static void i915_xd_pt_test_panel(void);
static void i915_xd_pt_test_second(void);
static void i915_xd_pt_test_retained(void);
static void i915_xd_pt_test_type_c(void);

/*
 * Runs the families in order: the refusals, the panel's life, a second
 * screen beside it, a retained screen, and a Type-C port's
 * reservations.
 */
int
main(void)
{
	int status;

	/* The families. */
	i915_xd_pt_test_refused();
	i915_xd_pt_test_panel();
	i915_xd_pt_test_second();
	i915_xd_pt_test_retained();
	i915_xd_pt_test_type_c();

	/* Reports the tally. */
	status = i915_host_report("xd_picture_host_test");
	if (status != 0)
		return status;

	/* Succeeded: every check passed. */
	return 0;
}

/*
 * Starts a fresh model and fresh pictures with the hardware's answers a
 * light waits for (the PLLs' power and lock follow their requests, the
 * DDI buffers leave idle while enabled, the transcoders' state follows
 * their enable, the idle patterns are sent, the plane's live surface is
 * the surface written, the scan line is far from the vertical blank),
 * and two backends on the model; returns the 5330's description.
 */
static const struct i915_xd_platform *
i915_xd_pt_setup(void)
{
	struct i915_xd_choice choice;
	unsigned index;
	int error;

	/* A fresh model. */
	i915_xd_fake_reset();

	/* The hardware's answers. */
	i915_xd_fake_rule_mirror(0x46010U, 0x08000000U, 0x46010U, -1, 0U);
	i915_xd_fake_rule_mirror(0x46010U, 0x80000000U, 0x46010U, -1, 0U);
	i915_xd_fake_rule_mirror(0x46014U, 0x08000000U, 0x46014U, -1, 0U);
	i915_xd_fake_rule_mirror(0x46014U, 0x80000000U, 0x46014U, -1, 0U);
	i915_xd_fake_rule_mirror(I915_XD_TBT_PLL_ENABLE, 0x08000000U, I915_XD_TBT_PLL_ENABLE, -1, 0U);
	i915_xd_fake_rule_mirror(I915_XD_TBT_PLL_ENABLE, 0x80000000U, I915_XD_TBT_PLL_ENABLE, -1, 0U);
	i915_xd_fake_rule_mirror(0x70008U, 0x80000000U, 0x70008U, -1, 0U);
	i915_xd_fake_rule_mirror(0x71008U, 0x80000000U, 0x71008U, -1, 0U);
	for (index = 0U; index < 4U; index++) {
		i915_xd_fake_rule_call(I915_XD_DDI_BUF_CTL(index), i915_xd_pt_buffer, NULL);
		i915_xd_fake_set(I915_XD_DDI_BUF_CTL(index), I915_XD_DDI_BUF_IDLE);
	}

	/* The pipes' frame counters are running. */
	i915_xd_fake_set(0x60544U, 0x02000000U);
	i915_xd_fake_set(0x61544U, 0x02000000U);
	for (index = 0U; index < 2U; index++) {
		i915_xd_fake_rule_mirror(I915_XD_PLANE_SURF(index), 0xffffffffU, I915_XD_PLANE_SURFLIVE(index), 0, 0U);
		i915_xd_fake_set(0x70000U + 0x1000U * index, 100U);
	}

	/* Two backends on the model. */
	i915_xd_pt_backend_init(&i915_xd_pt_model);
	i915_xd_pt_backend_init(&i915_xd_pt_other);

	/* The description, and the pictures it describes. */
	error = drv_i915_xd_platform_choose(I915_XD_PT_ADLP, I915_XD_PT_ADLP_REVID, 0, 0U, &choice);
	if (error != 0)
		return NULL;
	drv_i915_xd_pictures_init(&i915_xd_pt_pictures, choice.platform);

	/* Succeeded: the description. */
	return choice.platform;
}

/* Fills a backend on the model: every hook a commit uses, a register model, no counts. */
static void
i915_xd_pt_backend_init(
	struct i915_xd_pt_backend *backend)
{
	struct i915_xd_backend *hooks;

	/* Nothing counted, and the model's window. */
	memset(backend, 0, sizeof(*backend));
	i915_xd_fake_hooks(&backend->io);

	/* The registers, the time, the sink and the panel. */
	hooks = &backend->hooks;
	hooks->ctx = backend;
	hooks->model = 1;
	hooks->write32 = i915_xd_pt_write32;
	hooks->rmw32 = i915_xd_pt_rmw32;
	hooks->read32 = i915_xd_pt_read32;
	hooks->wait_reg = i915_xd_pt_wait_reg;
	hooks->usleep = i915_xd_pt_sleep;
	hooks->udelay = i915_xd_pt_sleep;
	hooks->step = i915_xd_pt_step;
	hooks->dpcd_read = i915_xd_pt_dpcd_read;
	hooks->dpcd_write = i915_xd_pt_dpcd_write;
	hooks->read_dpcd_caps = i915_xd_pt_read_caps;
	hooks->panel = i915_xd_pt_panel;

	/* The power, the display buffer, the vertical blanks, the locks and the errors. */
	hooks->power_domain = i915_xd_pt_power_domain;
	hooks->power_get = i915_xd_pt_power_get;
	hooks->power_put = i915_xd_pt_power_put;
	hooks->power_put_async = i915_xd_pt_power_put_async;
	hooks->dbuf_slices_update = i915_xd_pt_dbuf_slices;
	hooks->vblank_get = i915_xd_pt_vblank_get;
	hooks->vblank_put = i915_xd_pt_vblank_put;
	hooks->vblank_sleep = i915_xd_pt_vblank_sleep;
	hooks->irq_off = i915_xd_pt_irq_off;
	hooks->irq_on = i915_xd_pt_irq_on;
	hooks->arm_event = i915_xd_pt_arm_event;
	hooks->wait_event = i915_xd_pt_wait_event;
	hooks->lock = i915_xd_pt_lock;
	hooks->error = i915_xd_pt_error;
}

/*
 * Describes the 5330's panel: DDI A on AUX A, pipe and transcoder A,
 * DPLL 0 with its words, 1920x1080 at 18 bpp on HBR x2, the VBT's 200 Hz
 * PWM, the memory's watermarks and the display buffer as the start finds
 * them, and a 1920x1080 XRGB8888 framebuffer at SURF_A.
 */
static void
i915_xd_pt_panel_config(
	struct i915_xd_picture_config *config)
{
	static const uint16_t latency[8] = { 3, 54, 83, 102, 147, 147, 144, 144 };

	/* The output. */
	memset(config, 0, sizeof(*config));
	config->kind = I915_XD_SCREEN_PANEL;
	config->ddi = 0U;
	config->pipe = 0U;
	config->trans = 0U;
	config->aux = 0U;
	config->pll = 0U;
	config->hdmi_level = -1;
	config->caps[0] = 0x11U;
	config->caps[1] = 0x0aU;
	config->caps[2] = 0x82U;

	/* The VBT's backlight and the raw clock. */
	config->vbt_backlight_present = 1;
	config->vbt_backlight_hz = 200U;
	config->vbt_backlight_min = 6U;
	config->rawclk_khz = 19200U;

	/* The memory's watermarks and the display buffer. */
	memcpy(config->wm_latency, latency, sizeof(latency));
	config->wm_levels = 6U;
	config->wm_ipc = 1;
	config->sagv_block_us = 35U;
	config->dbuf_size = 4096U;
	config->dbuf_slice_mask = 0x0fU;
	config->dbuf_slices_now = 0x01U;

	/* The CD clock the start left, its limit, the bandwidth allowed and pipe A's DMC program. */
	config->cdclk.ref_khz = 38400U;
	config->cdclk.vco_khz = 537600U;
	config->cdclk.cdclk_khz = 179200U;
	config->cdclk.bypass_khz = 19200U;
	config->cdclk.voltage = 0U;
	config->cdclk_max_khz = 652800U;
	config->qgv_mbps = 11707U;
	config->dmc_programs = 1U << 1;

	/* The mode and the link. */
	config->mode.clock_khz = 140800;
	config->mode.hdisplay = 1920U;
	config->mode.hsync_start = 1936U;
	config->mode.hsync_end = 1952U;
	config->mode.htotal = 2080U;
	config->mode.vdisplay = 1080U;
	config->mode.vsync_start = 1083U;
	config->mode.vsync_end = 1097U;
	config->mode.vtotal = 1128U;
	config->mode.hsync_positive = 1;
	config->rate_khz = 270000U;
	config->lanes = 2U;
	config->bpp = 18U;
	drv_i915_xd_dp_m_n(18U * 16U, 2U, 140800U, 270000U, 1000000U, &config->m_n);

	/* The PLL's words. */
	config->pll_ref_khz = 38400U;
	config->pll_words.cfgcr0 = 0x00e001a5U;
	config->pll_words.cfgcr1 = 0x00000088U;

	/* The framebuffer. */
	config->fb_fourcc = 0x34325258U;
	config->fb_width = 1920U;
	config->fb_height = 1080U;
	config->fb_pitch = 7680U;
	config->fb_surf = I915_XD_PT_SURF_A;
}

/* Describes an HDMI sink on DDI B, pipe and transcoder B, beside pipe A, with a combo PLL's first word. */
static void
i915_xd_pt_hdmi_config(
	struct i915_xd_picture_config *config,
	uint32_t cfgcr0)
{
	/* The panel's inputs, then the HDMI output. */
	i915_xd_pt_panel_config(config);
	config->kind = I915_XD_SCREEN_HDMI;
	config->ddi = 1U;
	config->pipe = 1U;
	config->trans = 1U;
	config->aux = 1U;
	config->pll = 1U;
	config->also_pipes = 1U;
	config->rate_khz = 148500U;
	config->lanes = 4U;
	config->bpp = 24U;
	config->pll_words.cfgcr0 = cfgcr0;
	config->fb_surf = I915_XD_PT_SURF_B;
}

/* Returns how many power references a backend's screens hold. */
static int
i915_xd_pt_refs(
	const struct i915_xd_pt_backend *backend)
{
	unsigned domain;
	int total;

	/* Every domain's count. */
	total = 0;
	for (domain = 0U; domain < I915_XD_PT_DOMAINS; domain++)
		total += backend->power_refs[domain];

	/* Succeeded: the total. */
	return total;
}

/* The DDI buffer's idle bit follows its enable. */
static void
i915_xd_pt_buffer(
	uint32_t offset,
	uint32_t value,
	void *context)
{
	(void)context;

	/* Idle while disabled. */
	if ((value & I915_XD_DDI_BUF_ENABLE) != 0U) {
		i915_xd_fake_set(offset, value & ~I915_XD_DDI_BUF_IDLE);
	} else {
		i915_xd_fake_set(offset, value | I915_XD_DDI_BUF_IDLE);
	}
}

/* Writes a register of the model. */
static void
i915_xd_pt_write32(
	void *ctx,
	uint32_t reg,
	uint32_t value)
{
	struct i915_xd_pt_backend *backend;

	/* Through the model's window. */
	backend = ctx;
	backend->io.write(backend->io.context, reg, value);
}

/* Changes bits of a register of the model: the old value. */
static uint32_t
i915_xd_pt_rmw32(
	void *ctx,
	uint32_t reg,
	uint32_t clear,
	uint32_t set)
{
	struct i915_xd_pt_backend *backend;
	uint32_t old;

	/* Read, changed, written. */
	backend = ctx;
	old = backend->io.read(backend->io.context, reg);
	backend->io.write(backend->io.context, reg, (old & ~clear) | set);

	/* Succeeded: the old value. */
	return old;
}

/* Reads a register of the model. */
static uint32_t
i915_xd_pt_read32(
	void *ctx,
	uint32_t reg)
{
	struct i915_xd_pt_backend *backend;
	uint32_t value;

	/* Through the model's window. */
	backend = ctx;
	value = backend->io.read(backend->io.context, reg);

	/* Succeeded: the value. */
	return value;
}

/* Waits for a register of the model: met at once, or a timeout. */
static int
i915_xd_pt_wait_reg(
	void *ctx,
	uint32_t reg,
	uint32_t mask,
	uint32_t value,
	unsigned timeout_ms)
{
	uint32_t now;

	UNUSED_PARAMETER(timeout_ms);

	/* The register now. */
	now = i915_xd_pt_read32(ctx, reg);
	if ((now & mask) != value)
		return -110;

	/* Succeeded: the condition holds. */
	return 0;
}

/* Lets the model's time pass. */
static void
i915_xd_pt_sleep(
	void *ctx,
	unsigned us)
{
	struct i915_xd_pt_backend *backend;

	/* Through the model's window. */
	backend = ctx;
	backend->io.wait(backend->io.context, us);
}

/* Ignores a step the model has no hook for. */
static void
i915_xd_pt_step(
	void *ctx,
	const char *name)
{
	(void)ctx;
	(void)name;
}

/* Answers a DPCD read of the panel's sink: the link status reports the clock and the equalisation done on the trained lanes. */
static long
i915_xd_pt_dpcd_read(
	void *ctx,
	unsigned offset,
	uint8_t *buf,
	size_t size)
{
	struct i915_xd_pt_backend *backend;
	unsigned lane;
	uint8_t nibble;

	/* Everything else reads as zeros. */
	backend = ctx;
	memset(buf, 0, size);
	if (offset != I915_XD_DPCD_LANE_STATUS)
		return (long)size;

	/* The status of each lane, aligned. */
	for (lane = 0U; lane < backend->sink_lanes && lane < 4U; lane++) {
		nibble = I915_XD_DPCD_LANE_CR;
		if ((backend->sink_pattern & 0x0fU) != I915_XD_DPCD_PATTERN_1)
			nibble |= I915_XD_DPCD_LANE_EQ | I915_XD_DPCD_LANE_LOCKED;
		buf[lane >> 1] |= (uint8_t)(nibble << (4U * (lane & 1U)));
	}

	/* The lanes are aligned when the read reaches the word. */
	if (size > 2U)
		buf[2] = I915_XD_DPCD_ALIGN_DONE;

	/* Succeeded: the status is read. */
	return (long)size;
}

/* Takes a DPCD write of the panel's sink: its pattern and its lanes. */
static long
i915_xd_pt_dpcd_write(
	void *ctx,
	unsigned offset,
	const uint8_t *buf,
	size_t size)
{
	struct i915_xd_pt_backend *backend;

	/* The pattern, the rate with the lanes, and the lanes alone. */
	backend = ctx;
	if (offset == I915_XD_DPCD_PATTERN_SET)
		backend->sink_pattern = buf[0];
	if (offset == I915_XD_DPCD_BW_SET && size == 2U)
		backend->sink_lanes = buf[1] & 0x1fU;
	if (offset == I915_XD_DPCD_LANES_SET)
		backend->sink_lanes = buf[0] & 0x1fU;

	/* Succeeded: the bytes are taken. */
	return (long)size;
}

/* Answers the panel's sink's capabilities: DPCD 1.1, HBR, two lanes with enhanced framing. */
static int
i915_xd_pt_read_caps(
	void *ctx,
	uint8_t dpcd[15])
{
	(void)ctx;

	/* The capabilities. */
	memset(dpcd, 0, 15U);
	dpcd[0] = 0x11U;
	dpcd[1] = 0x0aU;
	dpcd[2] = 0x82U;

	/* Succeeded: the capabilities are read. */
	return 0;
}

/* Runs a panel power sequencer step: done at once. */
static int
i915_xd_pt_panel(
	void *ctx,
	int op)
{
	(void)ctx;
	(void)op;

	/* Succeeded: the step ran. */
	return 0;
}

/* Names the domain of one power use: the use and the unit (and a Type-C AUX channel's Thunderbolt one apart). */
static int
i915_xd_pt_power_domain(
	void *ctx,
	unsigned use,
	unsigned index,
	int thunderbolt)
{
	(void)ctx;

	/* The Thunderbolt AUX domain is the use's last unit. */
	if (thunderbolt)
		return (int)(use * 16U + 15U);

	/* Succeeded: the use's unit. */
	return (int)(use * 16U + (index & 15U));
}

/* Takes a reference on a domain: the cookie (the domain plus one). */
static int
i915_xd_pt_power_get(
	void *ctx,
	int domain)
{
	struct i915_xd_pt_backend *backend;

	/* One more reference. */
	backend = ctx;
	backend->power_refs[domain]++;

	/* Succeeded: the cookie. */
	return domain + 1;
}

/* Gives a reference on a domain back; the domain the test names is a well that would not turn off. */
static void
i915_xd_pt_power_put(
	void *ctx,
	int domain,
	int wakeref)
{
	struct i915_xd_pt_backend *backend;

	UNUSED_PARAMETER(wakeref);

	/* One reference less. */
	backend = ctx;
	backend->power_refs[domain]--;

	/* The well the test keeps on reports itself. */
	if (backend->fail_put != 0 && domain == backend->fail_put)
		drv_i915_xd_pictures_fault(&i915_xd_pt_pictures, "model: a power well did not turn off\n");
}

/* Gives a reference on a domain back after a delay (at once in the model). */
static void
i915_xd_pt_power_put_async(
	void *ctx,
	int domain,
	int wakeref,
	int delay_ms)
{
	(void)delay_ms;

	/* As a put. */
	i915_xd_pt_power_put(ctx, domain, wakeref);
}

/* Counts the display buffer slice requests. */
static void
i915_xd_pt_dbuf_slices(
	void *ctx,
	unsigned slices)
{
	struct i915_xd_pt_backend *backend;

	UNUSED_PARAMETER(slices);

	/* One more request. */
	backend = ctx;
	backend->slices_asked++;
}

/* Takes a vertical blank reference on a pipe. */
static int
i915_xd_pt_vblank_get(
	void *ctx,
	int pipe)
{
	struct i915_xd_pt_backend *backend;

	UNUSED_PARAMETER(pipe);

	/* One more reference. */
	backend = ctx;
	backend->vblank_refs++;

	/* Succeeded: the reference is taken. */
	return 0;
}

/* Gives a vertical blank reference back. */
static void
i915_xd_pt_vblank_put(
	void *ctx,
	int pipe)
{
	struct i915_xd_pt_backend *backend;

	UNUSED_PARAMETER(pipe);

	/* One reference less. */
	backend = ctx;
	backend->vblank_refs--;
}

/* Sleeps until the next vertical blank: it comes at once. */
static long
i915_xd_pt_vblank_sleep(
	void *ctx,
	int pipe,
	long ticks)
{
	(void)ctx;
	(void)pipe;

	/* Succeeded: ticks were left. */
	return ticks;
}

/* Turns interrupts off for an update; twice is an error of the screen. */
static void
i915_xd_pt_irq_off(
	void *ctx)
{
	struct i915_xd_pt_backend *backend;

	/* Off once. */
	backend = ctx;
	if (backend->irq_off)
		backend->irq_errors++;
	backend->irq_off = 1;
}

/* Turns interrupts back on after an update; when they were on, an error of the screen. */
static void
i915_xd_pt_irq_on(
	void *ctx)
{
	struct i915_xd_pt_backend *backend;

	/* On once. */
	backend = ctx;
	if (!backend->irq_off)
		backend->irq_errors++;
	backend->irq_off = 0;
}

/* Arms a flip's completion event. */
static void
i915_xd_pt_arm_event(
	void *ctx,
	int pipe)
{
	struct i915_xd_pt_backend *backend;

	UNUSED_PARAMETER(pipe);

	/* One more event. */
	backend = ctx;
	backend->events_armed++;
}

/* Waits for a flip's completion event: it comes at once. */
static int
i915_xd_pt_wait_event(
	void *ctx,
	int pipe,
	unsigned timeout_ms)
{
	(void)ctx;
	(void)pipe;
	(void)timeout_ms;

	/* Succeeded: the event came. */
	return 0;
}

/* Takes or drops a lock; a drop of a lock not held is an error. */
static void
i915_xd_pt_lock(
	void *ctx,
	int which,
	int take)
{
	struct i915_xd_pt_backend *backend;

	UNUSED_PARAMETER(which);

	/* Counts a lock taken. */
	backend = ctx;
	if (take) {
		backend->locks_held++;
		return;
	}

	/* Dropped, when held. */
	if (backend->locks_held == 0U) {
		backend->lock_errors++;
		return;
	}

	/* Drops it. */
	backend->locks_held--;
}

/* Counts an error a screen reports. */
static void
i915_xd_pt_error(
	void *ctx,
	const char *what)
{
	struct i915_xd_pt_backend *backend;

	UNUSED_PARAMETER(what);

	/* One more error. */
	backend = ctx;
	backend->errors++;
}

/* What a prepare refuses before anything is touched. */
static void
i915_xd_pt_test_refused(void)
{
	struct i915_xd_picture_config config;
	struct i915_xd_backend incomplete;
	const struct i915_xd_platform *platform;
	unsigned writes;
	int error;

	/* A fresh model. */
	platform = i915_xd_pt_setup();
	i915_host_check(platform != NULL, "R0: the 5330's description is chosen");
	writes = i915_xd_fake_writes();

	/* A backend without the power domain hook. */
	i915_xd_pt_panel_config(&config);
	incomplete = i915_xd_pt_model.hooks;
	incomplete.power_domain = NULL;
	error = drv_i915_xd_picture_prepare(&i915_xd_pt_pictures, I915_XD_PICTURE_RESIDENT, &config, &incomplete);
	i915_host_check(error == EINVAL, "R1: a backend without the power domain hook is refused");

	/* A pipe that does not exist. */
	i915_xd_pt_panel_config(&config);
	config.pipe = 4U;
	error = drv_i915_xd_picture_prepare(&i915_xd_pt_pictures, I915_XD_PICTURE_RESIDENT, &config, &i915_xd_pt_model.hooks);
	i915_host_check(error == EINVAL, "R2: pipe E is refused");

	/* The panel on another port's AUX channel. */
	i915_xd_pt_panel_config(&config);
	config.aux = 1U;
	error = drv_i915_xd_picture_prepare(&i915_xd_pt_pictures, I915_XD_PICTURE_RESIDENT, &config, &i915_xd_pt_model.hooks);
	i915_host_check(error == EINVAL, "R3: the panel on another port's AUX channel is refused");

	/* Three DisplayPort lanes, and an HDMI clock above 600 MHz. */
	i915_xd_pt_panel_config(&config);
	config.lanes = 3U;
	error = drv_i915_xd_picture_prepare(&i915_xd_pt_pictures, I915_XD_PICTURE_RESIDENT, &config, &i915_xd_pt_model.hooks);
	i915_host_check(error == EINVAL, "R4: a DisplayPort link of three lanes is refused");
	i915_xd_pt_hdmi_config(&config, 0x00e001a5U);
	config.rate_khz = 700000U;
	error = drv_i915_xd_picture_prepare(&i915_xd_pt_pictures, I915_XD_PICTURE_HEAD, &config, &i915_xd_pt_model.hooks);
	i915_host_check(error == EINVAL, "R5: an HDMI clock above 600 MHz is refused");

	/* An external DisplayPort sink without its own AUX channel, and on a combo port. */
	i915_xd_pt_panel_config(&config);
	config.kind = I915_XD_SCREEN_DP;
	config.ddi = 3U;
	config.pll = 3U;
	error = drv_i915_xd_picture_prepare(&i915_xd_pt_pictures, I915_XD_PICTURE_HEAD, &config, &i915_xd_pt_model.hooks);
	i915_host_check(error == EINVAL, "R6: an external DisplayPort sink without its own AUX channel is refused");

	/* A framebuffer that is not page-aligned, and a screen out of range. */
	i915_xd_pt_panel_config(&config);
	config.fb_surf = I915_XD_PT_SURF_ODD;
	error = drv_i915_xd_picture_prepare(&i915_xd_pt_pictures, I915_XD_PICTURE_RESIDENT, &config, &i915_xd_pt_model.hooks);
	i915_host_check(error == EINVAL, "R7: a framebuffer that is not page-aligned is refused");
	i915_xd_pt_panel_config(&config);
	error = drv_i915_xd_picture_prepare(&i915_xd_pt_pictures, 2U, &config, &i915_xd_pt_model.hooks);
	i915_host_check(error == EINVAL, "R8: a screen out of range is refused");

	/* A running CD clock the mode would need changed. */
	i915_xd_pt_panel_config(&config);
	config.cdclk.cdclk_khz = 307200U;
	config.cdclk.vco_khz = 614400U;
	error = drv_i915_xd_picture_prepare(&i915_xd_pt_pictures, I915_XD_PICTURE_RESIDENT, &config, &i915_xd_pt_model.hooks);
	i915_host_check(error == EINVAL && i915_xd_pt_pictures.screen[0].cdclk_change_needed == 1, "R9: a CD clock the mode would change is refused");

	/* Nothing was written. */
	i915_host_check(i915_xd_fake_writes() == writes, "R10: no refusal writes a register");
}

/* The panel's life: prepare, light, flips, brightness and backlight, dark. */
static void
i915_xd_pt_test_panel(void)
{
	struct i915_xd_picture_config config;
	struct i915_xd_picture_status status;
	struct i915_xd_picture_flip flip;
	struct i915_xd_pictures *pictures;
	uint32_t level;
	int retained;
	int idle;
	int error;

	/* A fresh model, the panel prepared on DPLL 0. */
	(void)i915_xd_pt_setup();
	pictures = &i915_xd_pt_pictures;
	i915_xd_pt_panel_config(&config);
	error = drv_i915_xd_picture_prepare(pictures, I915_XD_PICTURE_RESIDENT, &config, &i915_xd_pt_model.hooks);
	drv_i915_xd_picture_status(pictures, I915_XD_PICTURE_RESIDENT, &status);
	i915_host_check(error == 0 && status.prepared && !status.crtc_active && status.pll_id == 0 && status.pll_pipe_mask == 1,
			"P1: the panel is prepared on DPLL 0, reserved for pipe A, nothing lit");
	i915_host_check(status.ddb_end > status.ddb_start && status.wm0_enable && status.cdclk_change_needed == 0,
			"P2: the plan gives the plane a range and a first level, on the running CD clock");

	/* The light. */
	error = drv_i915_xd_picture_light(pictures, I915_XD_PICTURE_RESIDENT);
	drv_i915_xd_picture_status(pictures, I915_XD_PICTURE_RESIDENT, &status);
	drv_i915_xd_picture_link_status(pictures, I915_XD_PICTURE_RESIDENT, &status);
	i915_host_check(error == I915_XD_PICTURE_OK && status.crtc_active && status.plane_armed && status.errors == 0U,
			"P3: the light arms the plane without an error");
	i915_host_check(status.link_rate == 270000 && status.lane_count == 2 && status.link_trained_flag && status.cr_ok && status.eq_ok,
			"P4: the link is set to HBR x2, trained, and the sink reports the clock and the equalisation");
	i915_host_check(status.pll_on && status.pll_active_mask == 1 && status.crtc_domains_held == 4U && !status.dc_off_held,
			"P5: the pipe runs on DPLL 0 with its four power uses, the DC states let back");
	i915_host_check(status.dbuf_active_pipes_now == 1U && pictures->dbuf_valid && i915_xd_pt_model.slices_asked != 0U,
			"P6: the light left the display buffer laid out for pipe A");
	i915_host_check(status.backlight_present && status.backlight_user_max != 0U,
			"P7: the backlight is set up and the backlight device has a range");
	i915_host_check(i915_xd_pt_model.locks_held == 0U && i915_xd_pt_model.lock_errors == 0U,
			"P8: the light gives every lock back");

	/* A lit screen is not prepared again. */
	error = drv_i915_xd_picture_prepare(pictures, I915_XD_PICTURE_RESIDENT, &config, &i915_xd_pt_model.hooks);
	i915_host_check(error == EBUSY, "P9: a lit screen is not prepared again");

	/* A flip armed without waiting latches at once in the model. */
	error = drv_i915_xd_picture_flip_nowait(pictures, I915_XD_PICTURE_RESIDENT, I915_XD_PT_SURF_B, &flip);
	i915_host_check(error == I915_XD_PICTURE_OK && flip.result == I915_XD_PICTURE_FLIP_ARMED && flip.old_surf == I915_XD_PT_SURF_A,
			"P10: the flip to B is armed from A");
	i915_host_check(i915_xd_pt_model.events_armed == 1U && i915_xd_pt_model.vblank_refs == 1 && !i915_xd_pt_model.irq_off && i915_xd_pt_model.irq_errors == 0U,
			"P11: the flip armed one event that holds a vertical blank reference, interrupts back on");
	idle = drv_i915_xd_picture_flip_poll(pictures, I915_XD_PICTURE_RESIDENT);
	drv_i915_xd_picture_status(pictures, I915_XD_PICTURE_RESIDENT, &status);
	i915_host_check(idle == 1 && status.cur_surf == I915_XD_PT_SURF_B && !status.flip_pending && i915_xd_pt_model.vblank_refs == 0,
			"P12: the latched flip is retired by the poll: B is shown, the reference given back");

	/* The same buffer, and a waited flip back to A. */
	error = drv_i915_xd_picture_flip_nowait(pictures, I915_XD_PICTURE_RESIDENT, I915_XD_PT_SURF_B, &flip);
	i915_host_check(error == I915_XD_PICTURE_NOT_READY && flip.result == I915_XD_PICTURE_FLIP_REFUSED,
			"P13: a flip to the buffer shown is refused");
	error = drv_i915_xd_picture_flip(pictures, I915_XD_PICTURE_RESIDENT, I915_XD_PT_SURF_A, &flip);
	i915_host_check(error == I915_XD_PICTURE_OK && flip.result == I915_XD_PICTURE_FLIP_DONE && flip.live_after == I915_XD_PT_SURF_A,
			"P14: a waited flip back to A is done when A is live");
	error = drv_i915_xd_picture_flip_wait(pictures, I915_XD_PICTURE_RESIDENT);
	i915_host_check(error == 0, "P15: a presenting thread waits for nothing when no flip is armed");

	/* The brightness and the backlight. */
	error = drv_i915_xd_picture_brightness(pictures, I915_XD_PICTURE_RESIDENT, 50U, 100U);
	i915_host_check(error == I915_XD_PICTURE_OK, "P16: the brightness is set to 50 of 100");
	level = 0U;
	error = drv_i915_xd_picture_brightness_get(pictures, I915_XD_PICTURE_RESIDENT, 100U, &level);
	i915_host_check(error == I915_XD_PICTURE_OK && level >= 49U && level <= 51U, "P17: the brightness reads back as 50 of 100");
	error = drv_i915_xd_picture_brightness(pictures, I915_XD_PICTURE_RESIDENT, 101U, 100U);
	i915_host_check(error == I915_XD_PICTURE_NOT_READY, "P18: a level above the range is refused");
	error = drv_i915_xd_picture_backlight(pictures, I915_XD_PICTURE_RESIDENT, 0);
	drv_i915_xd_picture_status(pictures, I915_XD_PICTURE_RESIDENT, &status);
	i915_host_check(error == I915_XD_PICTURE_OK && !status.backlight_enabled && status.crtc_active, "P19: the backlight goes off while the pipe runs");
	error = drv_i915_xd_picture_backlight(pictures, I915_XD_PICTURE_RESIDENT, 1);
	drv_i915_xd_picture_status(pictures, I915_XD_PICTURE_RESIDENT, &status);
	i915_host_check(error == I915_XD_PICTURE_OK && status.backlight_enabled, "P20: the backlight comes back on");

	/* The plane's disable and the dark. */
	error = drv_i915_xd_picture_plane_off(pictures, I915_XD_PICTURE_RESIDENT);
	i915_host_check(error == I915_XD_PICTURE_OK, "P21: the plane's disable is armed");
	error = drv_i915_xd_picture_dark(pictures, I915_XD_PICTURE_RESIDENT);
	drv_i915_xd_picture_status(pictures, I915_XD_PICTURE_RESIDENT, &status);
	retained = drv_i915_xd_picture_retained(pictures, I915_XD_PICTURE_RESIDENT);
	i915_host_check(error == I915_XD_PICTURE_OK && !status.crtc_active && !status.pll_on && status.crtc_domains_held == 0U && !status.dc_off_held && !retained,
			"P22: the dark stops the pipe and the PLL and gives the pipe's power back");
	i915_host_check(i915_xd_pt_refs(&i915_xd_pt_model) == 0 && status.pll_pipe_mask == 0 && status.dbuf_active_pipes_now == 0U,
			"P23: after the dark no power reference, no PLL reservation and no pipe in the display buffer is left");
	drv_i915_xd_picture_released(pictures, I915_XD_PICTURE_RESIDENT);
	drv_i915_xd_picture_status(pictures, I915_XD_PICTURE_RESIDENT, &status);
	i915_host_check(!status.plane_armed && i915_xd_pt_model.locks_held == 0U && i915_xd_pt_model.lock_errors == 0U,
			"P24: the buffer is the caller's again and every lock was given back");

	/* A dark screen is not darkened again, and takes no brightness. */
	error = drv_i915_xd_picture_dark(pictures, I915_XD_PICTURE_RESIDENT);
	i915_host_check(error == I915_XD_PICTURE_NOT_READY, "P25: a dark screen is not darkened again");
	error = drv_i915_xd_picture_brightness(pictures, I915_XD_PICTURE_RESIDENT, 10U, 100U);
	i915_host_check(error == I915_XD_PICTURE_NOT_READY, "P26: a dark screen takes no brightness");
}

/* A second screen beside the lit panel: the other combo PLL, the same one with the same words, none left. */
static void
i915_xd_pt_test_second(void)
{
	struct i915_xd_picture_config config;
	struct i915_xd_picture_status status;
	struct i915_xd_pictures *pictures;
	int error;

	/* The panel lit on DPLL 0. */
	(void)i915_xd_pt_setup();
	pictures = &i915_xd_pt_pictures;
	i915_xd_pt_panel_config(&config);
	config.also_pipes = 2U;
	error = drv_i915_xd_picture_prepare(pictures, I915_XD_PICTURE_RESIDENT, &config, &i915_xd_pt_model.hooks);
	if (error == 0)
		error = drv_i915_xd_picture_light(pictures, I915_XD_PICTURE_RESIDENT);
	i915_host_check(error == 0, "S1: the panel is lit on pipe A, room left for pipe B");

	/* HDMI with other words: DPLL 1. */
	i915_xd_pt_hdmi_config(&config, 0x00e00123U);
	error = drv_i915_xd_picture_prepare(pictures, I915_XD_PICTURE_HEAD, &config, &i915_xd_pt_other.hooks);
	drv_i915_xd_picture_status(pictures, I915_XD_PICTURE_HEAD, &status);
	i915_host_check(error == 0 && status.pll_id == 1 && status.pll_pipe_mask == 2, "S2: an HDMI sink with other words gets DPLL 1");

	/* Lit beside the panel, then darkened: the panel's pipe stays in the layout. */
	error = drv_i915_xd_picture_light(pictures, I915_XD_PICTURE_HEAD);
	drv_i915_xd_picture_status(pictures, I915_XD_PICTURE_HEAD, &status);
	i915_host_check(error == I915_XD_PICTURE_OK && status.crtc_active && status.dbuf_active_pipes_now == 3U, "S3: the HDMI sink lights pipe B beside pipe A");
	(void)drv_i915_xd_picture_plane_off(pictures, I915_XD_PICTURE_HEAD);
	error = drv_i915_xd_picture_dark(pictures, I915_XD_PICTURE_HEAD);
	drv_i915_xd_picture_status(pictures, I915_XD_PICTURE_HEAD, &status);
	i915_host_check(error == I915_XD_PICTURE_OK && (status.dbuf_active_pipes_now & 1U) != 0U && i915_xd_pt_refs(&i915_xd_pt_other) == 0,
			"S4: the HDMI sink's dark keeps pipe A in the layout and gives its power back");
	drv_i915_xd_picture_status(pictures, I915_XD_PICTURE_RESIDENT, &status);
	i915_host_check(status.crtc_active && status.pll_on, "S5: the panel still runs on DPLL 0");

	/* HDMI with the panel's words: DPLL 0 shared. */
	drv_i915_xd_picture_released(pictures, I915_XD_PICTURE_HEAD);
	i915_xd_pt_hdmi_config(&config, 0x00e001a5U);
	error = drv_i915_xd_picture_prepare(pictures, I915_XD_PICTURE_HEAD, &config, &i915_xd_pt_other.hooks);
	drv_i915_xd_picture_status(pictures, I915_XD_PICTURE_HEAD, &status);
	i915_host_check(error == 0 && status.pll_id == 0 && status.pll_pipe_mask == 3, "S6: an HDMI sink with the panel's words shares DPLL 0");

	/* Both combo PLLs reserved with other words: none is left for a third set of words. */
	i915_xd_pt_hdmi_config(&config, 0x00e00123U);
	error = drv_i915_xd_picture_prepare(pictures, I915_XD_PICTURE_HEAD, &config, &i915_xd_pt_other.hooks);
	pictures->pll[1].reserved = 4U;
	i915_xd_pt_hdmi_config(&config, 0x00e00456U);
	error = drv_i915_xd_picture_prepare(pictures, I915_XD_PICTURE_HEAD, &config, &i915_xd_pt_other.hooks);
	i915_host_check(error == EBUSY, "S7: no combo PLL is left when both run other words");
}

/* A dark that is not confirmed, the screen it retains, and the model's discard. */
static void
i915_xd_pt_test_retained(void)
{
	struct i915_xd_picture_config config;
	struct i915_xd_picture_status status;
	struct i915_xd_pictures *pictures;
	int retained;
	int error;

	/* The panel lit; the pipe's power will not turn off. */
	(void)i915_xd_pt_setup();
	pictures = &i915_xd_pt_pictures;
	i915_xd_pt_panel_config(&config);
	error = drv_i915_xd_picture_prepare(pictures, I915_XD_PICTURE_RESIDENT, &config, &i915_xd_pt_model.hooks);
	if (error == 0)
		error = drv_i915_xd_picture_light(pictures, I915_XD_PICTURE_RESIDENT);
	i915_host_check(error == 0, "T1: the panel is lit");
	i915_xd_pt_model.fail_put = (int)(I915_XD_OUTPUT_POWER_PIPE * 16U);

	/* The dark is not confirmed: the DC states stay off and the screen is retained. */
	(void)drv_i915_xd_picture_plane_off(pictures, I915_XD_PICTURE_RESIDENT);
	error = drv_i915_xd_picture_dark(pictures, I915_XD_PICTURE_RESIDENT);
	drv_i915_xd_picture_status(pictures, I915_XD_PICTURE_RESIDENT, &status);
	retained = drv_i915_xd_picture_retained(pictures, I915_XD_PICTURE_RESIDENT);
	i915_host_check(error == I915_XD_PICTURE_ERRORS && status.stop_unconfirmed && status.dc_off_held && retained,
			"T2: a power well that would not turn off leaves the dark unconfirmed, the DC states off, the screen retained");

	/* Nothing more is done on it. */
	error = drv_i915_xd_picture_prepare(pictures, I915_XD_PICTURE_RESIDENT, &config, &i915_xd_pt_model.hooks);
	i915_host_check(error == EBUSY, "T3: a retained screen is not prepared again");
	drv_i915_xd_picture_abandoned(pictures, I915_XD_PICTURE_RESIDENT);
	error = drv_i915_xd_picture_discard_model(pictures, I915_XD_PICTURE_RESIDENT, &i915_xd_pt_other.hooks);
	i915_host_check(error == EPERM, "T4: another backend cannot discard the retained screen");

	/* The model that holds it discards it, and the screen is prepared again. */
	error = drv_i915_xd_picture_discard_model(pictures, I915_XD_PICTURE_RESIDENT, &i915_xd_pt_model.hooks);
	retained = drv_i915_xd_picture_retained(pictures, I915_XD_PICTURE_RESIDENT);
	i915_host_check(error == 0 && !retained, "T5: the model that holds the retained screen discards it");
	drv_i915_xd_pictures_forget(pictures);
	i915_xd_pt_model.fail_put = 0;
	error = drv_i915_xd_picture_prepare(pictures, I915_XD_PICTURE_RESIDENT, &config, &i915_xd_pt_model.hooks);
	i915_host_check(error == 0, "T6: the discarded screen is prepared again");
}

/* An external DisplayPort sink on TC1: its port's PLL and the Thunderbolt PLL reserved, the rate its PLL gives. */
static void
i915_xd_pt_test_type_c(void)
{
	static const struct i915_xd_sink_hooks sink = { NULL, NULL, NULL, NULL, NULL };
	struct i915_xd_picture_config config;
	struct i915_xd_picture_status status;
	struct i915_xd_pictures *pictures;
	int error;

	/* TC1's sink on pipe B. */
	(void)i915_xd_pt_setup();
	pictures = &i915_xd_pt_pictures;
	i915_xd_pt_panel_config(&config);
	config.kind = I915_XD_SCREEN_DP;
	config.ddi = 3U;
	config.pll = 3U;
	config.aux = 3U;
	config.pipe = 1U;
	config.trans = 1U;
	config.sink = &sink;
	error = drv_i915_xd_picture_prepare(pictures, I915_XD_PICTURE_HEAD, &config, &i915_xd_pt_model.hooks);
	drv_i915_xd_picture_status(pictures, I915_XD_PICTURE_HEAD, &status);
	i915_host_check(error == 0 && status.pll_id == 3 && status.pll_pipe_mask == 2, "C1: TC1's own PLL is reserved for pipe B");
	i915_host_check(pictures->pll[I915_XD_PICTURE_PLL_TBT].reserved == 2U, "C2: the Thunderbolt PLL is reserved beside it");
	i915_host_check(pictures->screen[1].config.rate_khz >= 269000U && pictures->screen[1].config.rate_khz <= 271000U,
			"C3: the link runs at the rate TC1's PLL gives for HBR");
	i915_host_check(pictures->screen[1].out.tc && pictures->screen[1].out.tc_port == 0U && pictures->screen[1].out.phy == 5U,
			"C4: the output is on Type-C port 1, the PHY of the platform's numbering");
}
