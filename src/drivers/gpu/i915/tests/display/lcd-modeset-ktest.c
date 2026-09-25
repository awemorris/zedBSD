/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The GPU-free kernel checks of the one-screen panel modeset.
 *
 * The reference's enable, plane and disable callers and callees run on the
 * register and sink models, with the eDP's own PPS and AUX code underneath
 * (the same production objects as on the hardware; only the backend
 * differs).  The host test has the long version; this part keeps the two
 * ownership stories in the kernel regression:
 *
 *   A  picture up -> frames advance -> plane off -> disable -> everything
 *      given back
 *   C  the pipe does not stop after the plane was armed -> no success, the
 *      buffer stays protected
 *
 * and the DBUF_CTL address table of the power-domain initialisation.
 *
 * The model runs use the started display's modeset, watermark and DP
 * worlds, of which production keeps one each.  The DP world is borrowed
 * from the resident panel (edp-ktest.h); the modeset and watermark worlds
 * are set aside for the run and put back afterwards, so the DPLL and DBUF
 * state the display start read out is what the device keeps.
 */

#include "display-ktest.h"
#include "dp-fake-hw.h"
#include "dp-fixture-latitude5330.h"
#include "edp-ktest.h"
#include "lcd-fake-hw.h"
#include <kern/kcrt.h>

#include "../execution/ktest.h"

#include "../../display/internal.h"
#include "../../display/modeset-internal.h"
#include "../../display/watermark-internal.h"
#include "../../display/clock.h"
#include "../../display/diagnostics.h"
#include "../../display/dp-sink.h"
#include "../../display/modeset.h"
#include "../../display/power.h"
#include "../../display/state.h"
#include "../../display/watermark.h"
#include "../../i915.h"

#include <kern/kmem.h>

#include <uapi/errno.h>
#include <stdint.h>

/* The panel's raw clock (kHz) and the combo PLL's reference (kHz). */
#define I915_LCD_MODESET_KTEST_RAWCLK_KHZ	19200U
#define I915_LCD_MODESET_KTEST_REF_KHZ		38400

/* The VBT colour depth of the panel. */
#define I915_LCD_MODESET_KTEST_VBT_BPP		18

/* The GGTT address the plane is given; nothing is behind it on the model. */
#define I915_LCD_MODESET_KTEST_FB_SURF		0xfdfc0000U

/* The frame counter of pipe A. */
#define I915_LCD_MODESET_KTEST_PIPE_FRMCOUNT	0x70040U

/*
 * The modeset and watermark worlds of the started display, set aside while
 * the model runs in them.
 *
 * It lives on the part's stack from the save to the restore; both copies
 * are allocated by the save and freed by the restore.
 */
struct i915_lcd_modeset_ktest_saved {
	/* The contents of the modeset world. */
	struct i915_lcd_world *lcd;

	/* The contents of the watermark world. */
	struct i915_wm_world *wm;
};

/*
 * The register and sink models of the run.
 *
 * Each bring-up starts them afresh; they live in static storage because
 * the eDP and the modeset keep pointers into them between the calls of a
 * story.
 */
static struct i915_dp_fake_hw i915_lcd_modeset_ktest_dpf;
static struct i915_lcd_fake_hw i915_lcd_modeset_ktest_lcd;

/*
 * The DP environment the eDP runs on, and what its bring-up found.
 *
 * The environment is bound to the DP model by each bring-up; the result
 * carries the DPCD and EDID the modeset state is computed from.
 */
static struct i915_dp_env i915_lcd_modeset_ktest_env;
static struct i915_edp_result i915_lcd_modeset_ktest_edp;

/*
 * The recorder in front of the register model, the computed state, the
 * modeset configuration and the last status read.
 *
 * They are rebuilt by each bring-up and read by the checks after it.
 */
static struct i915_lcd_trace i915_lcd_modeset_ktest_trace;
static struct i915_lcd_state i915_lcd_modeset_ktest_state;
static struct i915_lcd_modeset_cfg i915_lcd_modeset_ktest_cfg;
static struct i915_lcd_modeset_status i915_lcd_modeset_ktest_status;

static int i915_lcd_modeset_ktest_worlds_save(struct i915_display *display, struct i915_lcd_modeset_ktest_saved *saved);
static void i915_lcd_modeset_ktest_worlds_restore(struct i915_display *display, struct i915_lcd_modeset_ktest_saved *saved);
static void i915_lcd_modeset_ktest_skip_model(struct i915_ktest *ktest, const char *reason);
static int i915_lcd_modeset_ktest_bring_up(struct i915_display *display, struct i915_dp_world *world);
static void i915_lcd_modeset_ktest_fill_cfg(void);
static int i915_lcd_modeset_ktest_released(struct i915_dp_world *world);
static void i915_lcd_modeset_ktest_story_a(struct i915_ktest *ktest, struct i915_display *display, struct i915_dp_world *world);
static void i915_lcd_modeset_ktest_story_c(struct i915_ktest *ktest, struct i915_display *display, struct i915_dp_world *world);
static void i915_lcd_modeset_ktest_dbuf_addresses(struct i915_ktest *ktest);

/*
 * The checks of the two model stories, reported as skipped when the model
 * cannot run on the started display.
 */
static const char *const i915_lcd_modeset_ktest_model_checks[] = {
	"lcd-ms: A-PREPARE the state is built without touching anything",
	"lcd-ms: A-ENABLE the sink reports CR + EQ + lock; DDI_BUF_CTL / TRANS_DDI_FUNC_CTL / TRANS_CLK_SEL / PWM = Linux's dump",
	"lcd-ms: A-ORDER no violation; PLL + DDI IO + AUX + the crtc's four domains held, DC_OFF dropped, DBUF 0xf joined, CDCLK unchanged",
	"lcd-ms: A-PLANE armed once on the running pipe with this buffer's address; frames advance",
	"lcd-ms: A-PLANE-OFF the modeset does not declare the buffer free on its own",
	"lcd-ms: A-DISABLE the reference's disable: pipe / DDI / PLL / panel / backlight off, references returned, no violation",
	"lcd-ms: A-RELEASED nothing is held after the eDP ends",
	"lcd-ms: C-STUCK a pipe that does not stop: no success, the buffer stays marked in use, a new modeset is refused",
	"lcd-ms: C-RETAINED abandon is not forgotten: the entry is refused again, the object still holds its state",
	"lcd-ms: C-DISCARD released only by discarding the model that holds it"
};

/*
 * Checks the one-screen modeset on the register and sink models.
 *
 * The two stories run in the started display's worlds, borrowed for the
 * run; the DBUF_CTL address table is checked in any case.
 */
void
drv_i915_display_ktest_lcd_modeset(
	struct i915_ktest *ktest)
{
	struct i915_lcd_modeset_ktest_saved saved;
	struct i915_edp_ktest_world borrow;
	struct i915_display *display;
	int error;

	/* The stories need a display with its modeset and watermark worlds. */
	display = NULL;
	if (ktest->device != NULL)
		display = ktest->device->display;
	if (display == NULL ||
	    display->lcd_world == NULL ||
	    display->wm_world == NULL) {
		i915_lcd_modeset_ktest_skip_model(ktest, "the device has no display with modeset worlds");
		i915_lcd_modeset_ktest_dbuf_addresses(ktest);
		return;
	}

	/* Borrows the DP world from the resident panel. */
	error = drv_i915_display_ktest_edp_world_enter(ktest, &borrow);
	if (error != 0) {
		i915_lcd_modeset_ktest_skip_model(ktest, "the DP world could not be borrowed");
		i915_lcd_modeset_ktest_dbuf_addresses(ktest);
		return;
	}

	/* Sets the modeset and watermark worlds aside. */
	error = i915_lcd_modeset_ktest_worlds_save(display, &saved);
	if (error != 0) {
		drv_i915_display_ktest_edp_world_leave(&borrow);
		i915_lcd_modeset_ktest_skip_model(ktest, "no memory to set the modeset worlds aside");
		i915_lcd_modeset_ktest_dbuf_addresses(ktest);
		return;
	}

	/* The two ownership stories. */
	i915_lcd_modeset_ktest_story_a(ktest, display, borrow.world);
	i915_lcd_modeset_ktest_story_c(ktest, display, borrow.world);

	/* Puts the worlds back as the display start left them. */
	i915_lcd_modeset_ktest_worlds_restore(display, &saved);
	drv_i915_display_ktest_edp_world_leave(&borrow);

	/* The power-domain initialisation's DBUF_CTL table. */
	i915_lcd_modeset_ktest_dbuf_addresses(ktest);
}

/* Sets the display's modeset and watermark worlds aside: 0, or ENOMEM. */
static int
i915_lcd_modeset_ktest_worlds_save(
	struct i915_display *display,
	struct i915_lcd_modeset_ktest_saved *saved)
{
	/* Allocates the copy of the modeset world. */
	saved->lcd = kern_malloc(sizeof(*saved->lcd));
	if (saved->lcd == NULL)
		return ENOMEM;

	/* Allocates the copy of the watermark world. */
	saved->wm = kern_malloc(sizeof(*saved->wm));
	if (saved->wm == NULL) {
		kern_free(saved->lcd);
		saved->lcd = NULL;
		return ENOMEM;
	}

	/* Copies both worlds as the display start left them. */
	kern_memcpy(saved->lcd, display->lcd_world, sizeof(*saved->lcd));
	kern_memcpy(saved->wm, display->wm_world, sizeof(*saved->wm));

	/* Succeeded: the model may change the worlds. */
	return 0;
}

/* Puts the display's modeset and watermark worlds back and frees the copies. */
static void
i915_lcd_modeset_ktest_worlds_restore(
	struct i915_display *display,
	struct i915_lcd_modeset_ktest_saved *saved)
{
	/*
	 * The worlds get their bytes back at the same addresses, so the
	 * pointers they hold into themselves are valid again.
	 */
	kern_memcpy(display->lcd_world, saved->lcd, sizeof(*saved->lcd));
	kern_memcpy(display->wm_world, saved->wm, sizeof(*saved->wm));

	/* Frees the copies. */
	kern_free(saved->lcd);
	kern_free(saved->wm);
	saved->lcd = NULL;
	saved->wm = NULL;
}

/* Names each check of the two stories as not run, with the reason. */
static void
i915_lcd_modeset_ktest_skip_model(
	struct i915_ktest *ktest,
	const char *reason)
{
	unsigned count;
	unsigned index;

	/* One skip line per check. */
	count = sizeof(i915_lcd_modeset_ktest_model_checks) / sizeof(i915_lcd_modeset_ktest_model_checks[0]);
	for (index = 0U; index < count; index++)
		drv_i915_ktest_skip(ktest, i915_lcd_modeset_ktest_model_checks[index], reason);
}

/* Brings the model eDP up and computes the panel state: 0, or the first failure. */
static int
i915_lcd_modeset_ktest_bring_up(
	struct i915_display *display,
	struct i915_dp_world *world)
{
	struct i915_edp_config config;
	int error;

	/* A fresh device for this story: the shared DPLLs and the DBUF state start over. */
	drv_i915_lcd_dplls_reset(display->lcd_world);
	drv_i915_lcd_dbuf_forget(display->wm_world);

	/* The panel's VBT power sequence and raw clock; the eDP logs errors only. */
	kern_memset(&config, 0, sizeof(config));
	config.rawclk_khz = I915_LCD_MODESET_KTEST_RAWCLK_KHZ;
	config.t1_t3 = 2000;
	config.t8 = 800;
	config.t9 = 2000;
	config.t10 = 1100;
	config.t11_t12 = 5000;
	config.log_level = -1;

	/* The sink model holds the laptop panel's DPCD and EDID. */
	drv_i915_dp_fake_init(&i915_lcd_modeset_ktest_dpf, i915_dp_fixture_dpcd_000, i915_dp_fixture_dpcd_100, i915_dp_fixture_dpcd_700, i915_dp_fixture_edid, 128U);
	drv_i915_dp_fake_bind_env(&i915_lcd_modeset_ktest_dpf, &i915_lcd_modeset_ktest_env, world);

	/* Brings the eDP up to its DPCD and EDID. */
	error = drv_i915_edp_begin(world, &i915_lcd_modeset_ktest_env, &config, &i915_lcd_modeset_ktest_edp);
	if (error != 0)
		return error;

	/* Finishes the eDP's late initialisation. */
	error = drv_i915_edp_init_late(world, &config, &i915_lcd_modeset_ktest_edp);
	if (error != 0)
		return error;

	/* Computes the mode, the link and the PLL words from what the sink reported. */
	error = drv_i915_lcd_compute(display->lcd_world,
				     i915_lcd_modeset_ktest_edp.edid,
				     i915_lcd_modeset_ktest_edp.dpcd,
				     i915_lcd_modeset_ktest_edp.edp_dpcd,
				     I915_LCD_MODESET_KTEST_VBT_BPP,
				     I915_LCD_MODESET_KTEST_REF_KHZ,
				     &i915_lcd_modeset_ktest_state);
	if (error != 0)
		return error;

	/* The register model of pipe A, port A and DPLL 0, with the recorder in front of it. */
	drv_i915_lcd_fake_init(&i915_lcd_modeset_ktest_lcd, &i915_lcd_modeset_ktest_dpf, 0, 0, 0, i915_lcd_modeset_ktest_state.mode.vtotal);
	i915_lcd_modeset_ktest_lcd.dbuf_size = 4096U;
	drv_i915_lcd_trace_init(&i915_lcd_modeset_ktest_trace, &i915_lcd_modeset_ktest_lcd.ops);

	/* The configuration the normal initialisation would have read. */
	i915_lcd_modeset_ktest_fill_cfg();

	/* Succeeded: the story can prepare its modeset. */
	return 0;
}

/* Fills the modeset configuration as the normal initialisation of this machine reads it. */
static void
i915_lcd_modeset_ktest_fill_cfg(void)
{
	static const uint16_t latency[8] = {
		3,
		54,
		83,
		102,
		147,
		147,
		144,
		144
	};
	struct i915_lcd_modeset_cfg *cfg;

	cfg = &i915_lcd_modeset_ktest_cfg;
	kern_memset(cfg, 0, sizeof(*cfg));

	/* The sink's capabilities from the eDP. */
	kern_memcpy(cfg->dpcd, i915_lcd_modeset_ktest_edp.dpcd, sizeof(cfg->dpcd));
	kern_memcpy(cfg->edp_dpcd, i915_lcd_modeset_ktest_edp.edp_dpcd, sizeof(cfg->edp_dpcd));

	/* A full-HD linear XRGB8888 framebuffer at a fixed GGTT address. */
	cfg->fb_fourcc = 0x34325258U;
	cfg->fb_width = 1920U;
	cfg->fb_height = 1080U;
	cfg->fb_pitch = 7680U;
	cfg->fb_surf = I915_LCD_MODESET_KTEST_FB_SURF;

	/* The watermark latencies, IPC and the SAGV block time. */
	kern_memcpy(cfg->wm_latency, latency, sizeof(latency));
	cfg->wm_num_levels = 6;
	cfg->wm_ipc_enabled = 1;
	cfg->sagv_block_time_us = 35;

	/* Four DBUF slices, of which slice 1 is on. */
	cfg->dbuf_size = 4096U;
	cfg->dbuf_slice_mask = 0x0f;
	cfg->dbuf_enabled_slices = 0x01;

	/* The DMC firmware of pipe A is loaded. */
	cfg->dmc_fw_mask = 1U << 1;

	/* The VBT backlight: PWM at 200 Hz, minimum 6. */
	cfg->vbt_backlight_present = 1;
	cfg->vbt_backlight_pwm_freq_hz = 200;
	cfg->vbt_backlight_min_brightness = 6;
	cfg->rawclk_khz = I915_LCD_MODESET_KTEST_RAWCLK_KHZ;

	/* The CDCLK the initialisation left, MBUS not joined, and the allowed QGV bandwidth. */
	cfg->cdclk_khz = 179200U;
	cfg->cdclk_vco_khz = 537600U;
	cfg->cdclk_ref_khz = 38400U;
	cfg->cdclk_bypass_khz = 19200U;
	cfg->cdclk_max_khz = 652800U;
	cfg->cdclk_voltage_level = 0U;
	cfg->mbus_joined = 0;
	cfg->qgv_allowed_bw = 11707U;
}

/* Ends the model eDP and reports whether nothing is held any more. */
static int
i915_lcd_modeset_ktest_released(
	struct i915_dp_world *world)
{
	struct i915_lcd_fake_hw *lcd;
	struct i915_dp_fake_hw *dpf;
	int power_refs;
	int ended;

	lcd = &i915_lcd_modeset_ktest_lcd;
	dpf = &i915_lcd_modeset_ktest_dpf;

	/* Ends the eDP and runs its pending asynchronous power releases. */
	ended = drv_i915_edp_end(world, &i915_lcd_modeset_ktest_edp);
	drv_i915_dp_fake_flush_async(dpf);
	power_refs = drv_i915_lcd_fake_power_refs_total(lcd);

	/* No power reference, lock, DP reference or panel power is left. */
	if (ended != 0)
		return 0;
	if (power_refs != 0)
		return 0;
	if (lcd->lock_held[0] != 0 || lcd->lock_held[1] != 0)
		return 0;
	if (dpf->refs_core != 0 || dpf->refs_aux != 0)
		return 0;
	if ((dpf->pp_control & 9U) != 0U)
		return 0;

	/* Succeeded: everything was given back. */
	return 1;
}

/* Runs story A: up, frames, plane off, disable, everything given back. */
static void
i915_lcd_modeset_ktest_story_a(
	struct i915_ktest *ktest,
	struct i915_display *display,
	struct i915_dp_world *world)
{
	struct i915_lcd_modeset_status *s;
	struct i915_lcd_fake_hw *lcd;
	uint32_t frame_before;
	uint32_t frame_after;
	uint32_t trans_ddi_func_ctl;
	uint32_t trans_clk_sel;
	uint32_t pwm_freq;
	uint32_t plane_buf_cfg;
	uint32_t plane_wm0;
	uint32_t transconf;
	uint32_t ddi_buf_ctl;
	unsigned violations;
	int power_refs;
	int released;
	int passed;
	int error;

	s = &i915_lcd_modeset_ktest_status;
	lcd = &i915_lcd_modeset_ktest_lcd;

	/* Brings the models up and prepares the modeset. */
	error = i915_lcd_modeset_ktest_bring_up(display, world);
	if (error == 0)
		error = drv_i915_lcd_modeset_prepare(display, &i915_lcd_modeset_ktest_state, &i915_lcd_modeset_ktest_cfg, &i915_lcd_modeset_ktest_trace.ops);

	/* The state is built without a single register operation. */
	passed = 0;
	if (error == 0 && i915_lcd_modeset_ktest_trace.n == 0U)
		passed = 1;

	drv_i915_ktest_check(ktest, passed, "lcd-ms: A-PREPARE the state is built without touching anything");

	/* Runs the enable commit and reads the status and the sink's link status. */
	error = drv_i915_lcd_modeset_commit_enable(display);
	drv_i915_lcd_modeset_status(display, s);
	drv_i915_lcd_modeset_link_status(display, s);

	/* Reads TRANS_DDI_FUNC_CTL A, TRANS_CLK_SEL A and the PCH backlight PWM frequency from the model. */
	trans_ddi_func_ctl = drv_i915_lcd_fake_reg(lcd, 0x60400U);
	trans_clk_sel = drv_i915_lcd_fake_reg(lcd, 0x46140U);
	pwm_freq = drv_i915_lcd_fake_reg(lcd, 0xc8254U);

	/* The link trained and the words match Linux's register dump of this machine. */
	passed = 0;
	if (error == I915_LCD_MS_OK &&
	    s->errors == 0U &&
	    s->cr_ok &&
	    s->eq_ok &&
	    (s->link_status[0] & 0x77U) == 0x77U &&
	    s->ddi_buf_ctl_value == 0x80000002U &&
	    trans_ddi_func_ctl == 0x8a210002U &&
	    trans_clk_sel == 0x10000000U &&
	    pwm_freq == 0x17700U)
		passed = 1;

	drv_i915_ktest_check(ktest, passed, "lcd-ms: A-ENABLE the sink reports CR + EQ + lock; DDI_BUF_CTL / TRANS_DDI_FUNC_CTL / TRANS_CLK_SEL / PWM = Linux's dump");

	/* Reads the model's order violations and power references. */
	violations = drv_i915_lcd_fake_violations(lcd);
	power_refs = drv_i915_lcd_fake_power_refs_total(lcd);

	/* The enable took exactly what it needs and dropped DC_OFF asynchronously. */
	passed = 0;
	if (violations == 0U &&
	    s->pll_on &&
	    s->ddi_io_wakeref != 0 &&
	    s->aux_wakeref != 0 &&
	    power_refs == 6 &&
	    s->crtc_domains_held == 4U &&
	    s->dc_off_held == 0 &&
	    lcd->async_puts == 1U &&
	    lcd->dbuf_enabled == 0x0fU &&
	    s->mbus_joined_now == 1 &&
	    s->cdclk_required_khz == 179200 &&
	    s->cdclk_change_needed == 0)
		passed = 1;

	drv_i915_ktest_check(ktest, passed, "lcd-ms: A-ORDER no violation; PLL + DDI IO + AUX + the crtc's four domains held, DC_OFF dropped, DBUF 0xf joined, CDCLK unchanged");

	/* Reads the frame counter half a second apart. */
	frame_before = lcd->ops.read32(lcd->ops.ctx, I915_LCD_MODESET_KTEST_PIPE_FRMCOUNT);
	lcd->ops.usleep(lcd->ops.ctx, 500000U);
	frame_after = lcd->ops.read32(lcd->ops.ctx, I915_LCD_MODESET_KTEST_PIPE_FRMCOUNT);

	/* Reads PLANE_BUF_CFG and PLANE_WM level 0 of plane 1 A from the model. */
	plane_buf_cfg = drv_i915_lcd_fake_reg(lcd, 0x7027cU);
	plane_wm0 = drv_i915_lcd_fake_reg(lcd, 0x70240U);

	/* The plane was armed once with this buffer and its DDB and watermarks; the frames advance. */
	passed = 0;
	if (error == I915_LCD_MS_OK &&
	    lcd->plane_arms == 1U &&
	    lcd->plane_ctl_at_arm == 0x94000000U &&
	    lcd->plane_surf_at_arm == I915_LCD_MODESET_KTEST_FB_SURF &&
	    frame_after >= frame_before + 29U &&
	    lcd->plane_armed_without_ddb == 0U &&
	    plane_buf_cfg == 0x0fdb0000U &&
	    plane_wm0 == 0x80004010U)
		passed = 1;

	drv_i915_ktest_check(ktest, passed, "lcd-ms: A-PLANE armed once on the running pipe with this buffer's address; frames advance");

	/* Runs the disable commit. */
	error = drv_i915_lcd_modeset_commit_disable(display);
	drv_i915_lcd_modeset_status(display, s);

	/* The plane is still marked armed: only the caller can say the buffer is free. */
	passed = 0;
	if (error == I915_LCD_MS_OK && s->plane_armed == 1)
		passed = 1;

	drv_i915_ktest_check(ktest, passed, "lcd-ms: A-PLANE-OFF the modeset does not declare the buffer free on its own");

	/* The caller confirmed that the plane is off; the model's state is read. */
	drv_i915_lcd_modeset_plane_released(display);
	violations = drv_i915_lcd_fake_violations(lcd);
	transconf = drv_i915_lcd_fake_reg(lcd, 0x70008U);
	ddi_buf_ctl = drv_i915_lcd_fake_reg(lcd, 0x46010U);

	/* Everything the enable switched on is off, and every reference is back. */
	passed = 0;
	if (error == I915_LCD_MS_OK &&
	    s->errors == 0U &&
	    !s->crtc_active &&
	    !s->pll_on &&
	    s->ddi_io_wakeref == 0 &&
	    s->aux_wakeref == 0 &&
	    (transconf & 0xc0000000U) == 0U &&
	    (ddi_buf_ctl & 0xcc000000U) == 0U &&
	    (i915_lcd_modeset_ktest_dpf.pp_control & 5U) == 0U &&
	    violations == 0U &&
	    s->crtc_domains_held == 0U &&
	    lcd->dbuf_enabled == 0x01U &&
	    s->mbus_joined_now == 0)
		passed = 1;

	drv_i915_ktest_check(ktest, passed, "lcd-ms: A-DISABLE the reference's disable: pipe / DDI / PLL / panel / backlight off, references returned, no violation");

	/* Ends the eDP and looks for anything still held. */
	released = i915_lcd_modeset_ktest_released(world);
	drv_i915_ktest_check(ktest, released, "lcd-ms: A-RELEASED nothing is held after the eDP ends");
}

/* Runs story C: a pipe that does not stop keeps the buffer protected until the model is discarded. */
static void
i915_lcd_modeset_ktest_story_c(
	struct i915_ktest *ktest,
	struct i915_display *display,
	struct i915_dp_world *world)
{
	struct i915_lcd_modeset_status *s;
	struct i915_lcd_fake_hw *lcd;
	int prepare_again;
	int retained;
	int discarded;
	int passed;
	int error;

	s = &i915_lcd_modeset_ktest_status;
	lcd = &i915_lcd_modeset_ktest_lcd;

	/* Brings the picture up. */
	error = i915_lcd_modeset_ktest_bring_up(display, world);
	if (error == 0)
		error = drv_i915_lcd_modeset_prepare(display, &i915_lcd_modeset_ktest_state, &i915_lcd_modeset_ktest_cfg, &i915_lcd_modeset_ktest_trace.ops);
	if (error == 0)
		error = drv_i915_lcd_modeset_commit_enable(display);

	/* The pipe's state bit never clears; the disable runs into it. */
	lcd->fault_pipe_stuck_on = 1;
	if (error == 0) {
		error = drv_i915_lcd_modeset_commit_disable(display);
	} else {
		error = -99;
	}
	drv_i915_lcd_modeset_status(display, s);

	/* A new modeset is tried on the unconfirmed stop. */
	prepare_again = drv_i915_lcd_modeset_prepare(display, &i915_lcd_modeset_ktest_state, &i915_lcd_modeset_ktest_cfg, &i915_lcd_modeset_ktest_trace.ops);

	/* No success; the plane, DC_OFF, the crtc's domains and the DBUF stay; the new modeset is refused. */
	passed = 0;
	if (error == I915_LCD_MS_ERRORS &&
	    s->first_error != NULL &&
	    s->plane_armed == 1 &&
	    s->stop_unconfirmed == 1 &&
	    s->dc_off_held == 1 &&
	    s->crtc_domains_held == 4U &&
	    lcd->dbuf_enabled == 0x0fU &&
	    prepare_again == EBUSY)
		passed = 1;

	drv_i915_ktest_check(ktest, passed, "lcd-ms: C-STUCK a pipe that does not stop: no success, the buffer stays marked in use, a new modeset is refused");

	/* The eDP ends; the caller decided to keep the buffer for ever. */
	(void)drv_i915_edp_end(world, &i915_lcd_modeset_ktest_edp);
	drv_i915_lcd_modeset_abandoned(display);
	prepare_again = drv_i915_lcd_modeset_prepare(display, &i915_lcd_modeset_ktest_state, &i915_lcd_modeset_ktest_cfg, &i915_lcd_modeset_ktest_trace.ops);
	retained = drv_i915_lcd_modeset_retained(display);

	/* The abandoned state is not forgotten. */
	passed = 0;
	if (prepare_again == EBUSY && retained == 1)
		passed = 1;

	drv_i915_ktest_check(ktest, passed, "lcd-ms: C-RETAINED abandon is not forgotten: the entry is refused again, the object still holds its state");

	/* Discards the model that holds the retained state. */
	discarded = drv_i915_lcd_modeset_discard_model(display, &i915_lcd_modeset_ktest_trace.ops);
	retained = drv_i915_lcd_modeset_retained(display);

	/* Only the discard releases it. */
	passed = 0;
	if (discarded == 0 && retained == 0)
		passed = 1;

	drv_i915_ktest_check(ktest, passed, "lcd-ms: C-DISCARD released only by discarding the model that holds it");
}

/* Checks the power-domain initialisation's DBUF_CTL table against the reference's macro. */
static void
i915_lcd_modeset_ktest_dbuf_addresses(
	struct i915_ktest *ktest)
{
	uint32_t initialisation;
	uint32_t reference;
	unsigned slice;
	unsigned bad;
	int passed;

	/* Compares DBUF_CTL_S(S1..S4) slice by slice. */
	bad = 0U;
	for (slice = 0U; slice < 4U; slice++) {
		initialisation = drv_i915_dbuf_ctl_reg(slice);
		reference = drv_i915_lcd_ref_dbuf_ctl(slice);

		/* A differing or missing address counts. */
		if (initialisation != reference || initialisation == 0U)
			bad++;
	}

	/* Reads slice 1 once more for the equality of the first entry. */
	initialisation = drv_i915_dbuf_ctl_reg(0U);
	reference = drv_i915_lcd_ref_dbuf_ctl(0U);

	/* Every slice's address is the reference's. */
	passed = 0;
	if (bad == 0U && initialisation == reference)
		passed = 1;

	drv_i915_ktest_check(ktest, passed, "lcd-ms: DBUF-ADDR the power-domain init's DBUF_CTL table = the reference's DBUF_CTL_S(S1..S4) macro, slice by slice");
}
