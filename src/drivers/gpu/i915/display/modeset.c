/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The one-screen modeset, the one picture on the panel and the resident
 * panel run.
 *
 * The modeset object of a screen is built from the values an atomic check
 * would have computed (the state calculation, state.c) and driven by the
 * reference's own callers and callees in the other modeset files: prepare
 * runs the check phase, the two commits are intel_atomic_commit_tail()
 * reduced to one crtc, and status reads back what the run left behind.  It
 * is not an atomic-commit framework: one crtc, one encoder on a combo-PHY
 * port, one shared DPLL, one primary plane on one linear XRGB8888
 * framebuffer.  "The call returned" is never reported as "it worked".
 *
 * The show body puts one buffer on the panel through the two commits,
 * observes the picture, stops it through the reference's stop path and
 * decides whether the display has let go of the buffer.  Three records are
 * kept apart: the first anomaly, the state the enable left, and what the
 * cleanup returned.
 *
 * The resident run is the panel as the display of the resident node: two
 * full-panel buffers, one modeset, synchronous flips driven by
 * presentations, and the reference's stop path when the lease ends.  Its
 * hooks (registers, waits, panel, power, vblank, locks) work on the objects
 * the normal initialisation built; none is copied.
 *
 * XXX: drv_i915_lcd_display_ver(), drv_i915_lcd_backend_fault() and
 * drv_i915_lcd_debug() take no world, as their callers in the Linux text
 * have none; they reach the one bound world through a file-scope pointer.
 */

#include "modeset-internal.h"
#include "watermark-internal.h"
#include "modeset.h"
#include "aux.h"
#include "clock.h"
#include "color.h"
#include "ddi.h"
#include "diagnostics.h"
#include "dp.h"
#include "dp-sink.h"
#include "edid.h"
#include "hdmi-mode.h"
#include "panel.h"
#include "pipe.h"
#include "plane.h"
#include "power.h"
#include "present.h"
#include "scanout.h"
#include "state.h"
#include "vblank.h"
#include "vbt-parse.h"
#include "watermark.h"
#include <kern/kcrt.h>

#include "../i915.h"
#include "../ggtt.h"
#include "../memory.h"
#include "../mmio.h"
#include "../sync.h"

#include <kern/klog.h>
#include <kern/kmem.h>
#include <kern/lock.h>
#include <kern/irq.h>

#include <uapi/errno.h>
#include <stddef.h>

/* The display version the platform predicates answer before the probe sets one. */
#define I915_LCD_DEFAULT_DISPLAY_VER	13

/* DP_ENHANCED_FRAME_CAP of DPCD byte 2. */
#define I915_LCD_DPCD_ENHANCED_FRAME_CAP	0x80U

/* How long the flip waits for its completion event. */
#define I915_LCD_FLIP_EVENT_MS		100U

/* The delay after which a commit drops DC_OFF again (17 ms, above 60 frames a second). */
#define I915_LCD_DC_OFF_DELAY_MS	17

/* DDI_BUF_PORT_REVERSAL and DDI_A_4_LANES of the DDI_BUF_CTL readout at connector init. */
#define I915_LCD_SAVED_PORT_BITS	((1U << 16) | (1U << 4))

/* How long a panel run waits for the first frames. */
#define I915_LCD_FIRST_FRAMES_MS	1000U

/* The first frames a run must see, and the frames each steady round must see within 1.5 s. */
#define I915_LCD_FIRST_FRAMES		10U
#define I915_LCD_STEADY_FRAMES		30U
#define I915_LCD_STEADY_ROUND_MS	1500U
#define I915_LCD_STEADY_STEP_MS		500U

/* How long the stop confirmation watches the frame counter. */
#define I915_LCD_STOP_WATCH_MS		200U

/* The DBUF geometry of Tiger Lake (two slices) and XE_LPD (four slices). */
#define I915_LCD_DBUF_TGL_SLICES	0x03U
#define I915_LCD_DBUF_TGL_BLOCKS	2048U
#define I915_LCD_DBUF_XELPD_BLOCKS	4096U
#define I915_LCD_DBUF_XELPD_SLICES	0x0fU

/* The pipe A registers the preflight reads raw on the HDMI arm. */
#define I915_LCD_TRANSCONF_B		0x71008U
#define I915_LCD_DDI_BUF_CTL_B		0x64100U
#define I915_LCD_SDEISR			0xc4000U
#define I915_LCD_DPLL0_ENABLE		0x46010U
#define I915_LCD_DPLL1_ENABLE		0x46014U

/* The enable bit of a transcoder, a plane, a PLL and a DDI buffer, and the transcoder's state bit. */
#define I915_LCD_ENABLE_BIT		0x80000000U
#define I915_LCD_TRANSCONF_ON_MASK	0xc0000000U

/* The pipe A mask bits that must stay masked: vblank and FIFO underrun, plus the XE_LPD soft and hard underrun. */
#define I915_LCD_KEEP_MASKED_XELPD	0x80600001U
#define I915_LCD_KEEP_MASKED_GEN12	0x80000001U

/* The scanline register the evasion probe watches. */
#define I915_LCD_PIPEDSL_A		0x70000U
#define I915_LCD_PIPEDSL_MASK		0x1fffU

/*
 * The modeset world the Linux text's world-less hooks report to.
 *
 * Set by the world's creation and cleared by its destruction; one display
 * binds it at a time.  NULL means no display has a modeset world.
 */
static struct i915_lcd_world *i915_modeset_bound_world;

static struct i915_lcd_modeset *i915_modeset_selected_screen(struct i915_lcd_world *world);
static void i915_modeset_bind_current(struct i915_display *display);
static void i915_modeset_on_error(void *ctx, const char *what);
static void i915_modeset_read_link_status(struct i915_lcd_modeset *ms, struct i915_lcd_modeset_status *out);
static void i915_modeset_observe(struct i915_lcd_world *world, int point);
static uint32_t i915_modeset_live_surf(struct i915_lcd_world *world);
static uint32_t i915_modeset_frame_now(struct i915_lcd_world *world);
static int i915_modeset_flip_arm(struct i915_display *display, uint32_t new_surf, struct i915_lcd_flip_result *res, int *dc_off);
static void i915_modeset_flip_retire(struct i915_display *display);
static int i915_modeset_prepare_state(struct i915_display *display, const struct i915_lcd_state *s, const struct i915_lcd_modeset_cfg *cfg, struct i915_lcd_emit *ops);
static void i915_show_anomaly(struct i915_lcd_show_report *r, const char *what, int rc);
static void i915_show_reached(struct i915_lcd_show_env *env, struct i915_lcd_show_report *r, int stage);
static int i915_show_first_error_after(const struct i915_lcd_trace *trace, unsigned from);
static int i915_show_display(struct i915_display *display, struct i915_lcd_show_env *env, struct i915_scanout *so, struct i915_lcd_show_report *r, uint32_t (*verify)(void *ctx, const struct i915_scanout *so), void *verify_ctx);
static int i915_show_passed(struct i915_display *display, const struct i915_lcd_show_report *r, int released);
static int i915_show_begin(struct i915_display *display, struct i915_lcd_show_env *env, struct i915_lcd_show_report *r);
static uint32_t i915_show_pattern_verify(void *ctx, const struct i915_scanout *so);
static void i915_show_release_unused(struct i915_scanout *so, struct i915_lcd_show_report *r);
static struct i915_display *i915_kernel_display(void *ctx);
static uint32_t i915_kernel_read32(void *ctx, uint32_t reg);
static void i915_kernel_write32(void *ctx, uint32_t reg, uint32_t value);
static uint32_t i915_kernel_rmw32(void *ctx, uint32_t reg, uint32_t clear, uint32_t set);
static void i915_kernel_posting_read(void *ctx, uint32_t reg);
static int i915_kernel_wait_reg(void *ctx, uint32_t reg, uint32_t mask, uint32_t value, unsigned timeout_ms);
static void i915_kernel_usleep(void *ctx, unsigned us);
static void i915_kernel_udelay(void *ctx, unsigned us);
static void i915_kernel_dbuf_slices_update(void *ctx, unsigned req_slices);
static void i915_kernel_lock(void *ctx, int which, int take);
static void i915_kernel_irq_off(void *ctx);
static void i915_kernel_irq_on(void *ctx);
static int i915_kernel_preflight_hdmi(struct i915_lcd_kernel *k);
static int i915_resident_window(void *ctx, struct i915_lcd_observer *o);
static uint32_t i915_resident_verify(void *ctx, const struct i915_scanout *so);
static int i915_resident_buffers(struct i915_display *display, const struct i915_lcd_kernel_deps *d);
static int i915_resident_release(struct i915_display *display, const struct i915_lcd_show_report *rep);
static int i915_resident_passed(const struct i915_lcd_show_report *rep);

/*
 * Allocates the modeset world of a display.
 *
 * The world starts zeroed except for the display version, which starts at
 * 13 until the probe sets the device's.  Returns 0, ENOMEM, or EBUSY when
 * another display already has a modeset world.
 */
int
drv_i915_lcd_world_create(
	struct i915_display *display)
{
	struct i915_lcd_world *world;

	/* One display binds the world-less hooks at a time. */
	if (i915_modeset_bound_world != NULL)
		return EBUSY;

	/* Allocates the zeroed world. */
	world = kern_calloc(1U, sizeof(*world));
	if (world == NULL)
		return ENOMEM;

	/* The display version the platform predicates answer until the probe sets one. */
	world->i915_lcd_ver = I915_LCD_DEFAULT_DISPLAY_VER;

	/* The display owns the world, and the world-less hooks report to it. */
	display->lcd_world = world;
	i915_modeset_bound_world = world;

	/* Succeeded: the modeset path has its state. */
	return 0;
}

/*
 * Frees the modeset world of a display.
 */
void
drv_i915_lcd_world_destroy(
	struct i915_display *display)
{
	/* A display without a world has nothing to free. */
	if (display->lcd_world == NULL)
		return;

	/* The world-less hooks no longer report to this world. */
	if (i915_modeset_bound_world == display->lcd_world)
		i915_modeset_bound_world = NULL;

	kern_free(display->lcd_world);
	display->lcd_world = NULL;
}

/*
 * Sets the display version of the device.
 *
 * The tables that differ between the Gen12 platforms (buffer
 * translations, DBUF geometry, CDCLK) are chosen from it; the sequences are
 * the same text.
 */
void
drv_i915_lcd_set_display_ver(
	struct i915_display *display,
	int ver)
{
	/* The world keeps the version the platform predicates answer from. */
	display->lcd_world->i915_lcd_ver = ver;
}

/*
 * Returns the display version of the device (12 = Tiger Lake, 13 = ADL-P class).
 */
int
drv_i915_lcd_display_ver(void)
{
	/* Before a world exists the predicates answer the default version. */
	if (i915_modeset_bound_world == NULL)
		return I915_LCD_DEFAULT_DISPLAY_VER;

	/* Reports the version the probe set. */
	return i915_modeset_bound_world->i915_lcd_ver;
}

/*
 * Records an anomaly the backend found as a Linux-text error of the
 * selected screen.
 *
 * A power well kept on or a broken time base is the run's first anomaly,
 * not a silent condition.
 */
void
drv_i915_lcd_backend_fault(
	const char *what)
{
	/* Without a world there is no run to report to. */
	if (i915_modeset_bound_world == NULL)
		return;

	/* Counts the fault like an error of the Linux text. */
	i915_modeset_on_error(i915_modeset_bound_world, what);
}

/*
 * Hands a debug message of the Linux text to the selected screen's backend.
 */
void
drv_i915_lcd_debug(
	const char *what)
{
	struct i915_lcd_world *world;
	struct i915_lcd_emit *ms_ops;

	/* Without a world there is no backend to hand it to. */
	world = i915_modeset_bound_world;
	if (world == NULL)
		return;

	/* A backend without a debug hook ignores the message. */
	ms_ops = world->ms_ops_pool[world->ms_sel];
	if (ms_ops == NULL || ms_ops->debug == NULL)
		return;

	ms_ops->debug(ms_ops->ctx, what);
}

/*
 * Selects the screen the following calls work on.
 *
 * Every screen keeps its own state meanwhile.  Returns 0, or EINVAL for a
 * screen out of range.
 */
int
drv_i915_lcd_modeset_select(
	struct i915_display *display,
	unsigned screen)
{
	/* Refuses a screen the pools have no room for. */
	if (screen >= I915_LCD_MS_SCREENS)
		return EINVAL;

	/* Selects it and binds its objects to the Linux text. */
	display->lcd_world->ms_sel = screen;
	i915_modeset_bind_current(display);

	/* Succeeded: the calls below work on the screen. */
	return 0;
}

/*
 * Returns the selected screen.
 */
unsigned
drv_i915_lcd_modeset_selected(
	struct i915_display *display)
{
	/* Reports the index into the world's pools. */
	return display->lcd_world->ms_sel;
}

/*
 * Builds the new state of the selected screen and runs the check phase.
 *
 * The state is what intel_dp_compute_config() / intel_crtc_compute_config()
 * would leave for the panel (or an HDMI sink without an EDID), the shared
 * DPLL the reference's rule gives it, the plane state of the framebuffer,
 * and the watermarks, DDB, CDCLK and memory bandwidth it needs; nothing is
 * written.  Returns 0; EINVAL for a configuration this path does not cover
 * (checked before anything is touched) or a CDCLK or bandwidth it cannot
 * serve; EBUSY when the screen is running or retained, or no DPLL is free;
 * or the plane, watermark or CDCLK result of the Linux text.
 */
int
drv_i915_lcd_modeset_prepare(
	struct i915_display *display,
	const struct i915_lcd_state *s,
	const struct i915_lcd_modeset_cfg *cfg,
	struct i915_lcd_emit *ops)
{
	struct i915_lcd_world *world;
	struct i915_lcd_modeset *ms;
	int state_error;

	world = display->lcd_world;
	ms = i915_modeset_selected_screen(world);

	/* Refuses a call without its inputs. */
	if (s == NULL || cfg == NULL || ops == NULL)
		return EINVAL;

	/* The backend must carry every hook the commits use. */
	if (ops->write32 == NULL ||
	    ops->rmw32 == NULL ||
	    ops->read32 == NULL ||
	    ops->wait_reg == NULL ||
	    ops->usleep == NULL ||
	    ops->udelay == NULL ||
	    ops->dpcd_read == NULL ||
	    ops->dpcd_write == NULL ||
	    ops->read_dpcd_caps == NULL ||
	    ops->panel == NULL ||
	    ops->power_get == NULL ||
	    ops->power_put == NULL ||
	    ops->power_put_async == NULL ||
	    ops->dbuf_slices_update == NULL ||
	    ops->lock == NULL ||
	    ops->step == NULL)
		return EINVAL;

	/* Combo PHY ports A and B, pipes and transcoders A to D, the two combo PLLs. */
	if (cfg->port < 0 || cfg->port > 1)
		return EINVAL;
	if (cfg->pipe < 0 || cfg->pipe > 3)
		return EINVAL;
	if (cfg->cpu_transcoder < 0 || cfg->cpu_transcoder > 3)
		return EINVAL;
	if (cfg->dpll_id < 0 || cfg->dpll_id > 1)
		return EINVAL;
	if (s->link.rate_khz <= 0 || s->link.bpp <= 0)
		return EINVAL;

	/* HDMI carries the TMDS clock in link.rate_khz (up to 600 MHz). */
	if (cfg->output_hdmi) {
		if (s->link.rate_khz > 600000)
			return EINVAL;
	} else {
		/* DP carries the link rate (8b/10b rates) and 1, 2 or 4 lanes on the port's own AUX channel. */
		if (cfg->aux_ch != cfg->port)
			return EINVAL;
		if (s->link.rate_khz > 810000)
			return EINVAL;
		if (s->link.lanes != 1 && s->link.lanes != 2 && s->link.lanes != 4)
			return EINVAL;
	}

	/* A retained state is never overwritten: refused before anything is initialised. */
	if (world->ms_retained_pool[world->ms_sel])
		return EBUSY;
	if (ms->stop_unconfirmed)
		return EBUSY;
	if (ms->prepared) {
		if (ms->crtc.active || ms->plane_armed || ms->dc_off_held)
			return EBUSY;
	}

	/* Builds the state and runs the check phase. */
	state_error = i915_modeset_prepare_state(display, s, cfg, ops);
	if (state_error != 0)
		return state_error;

	/* PREPARED: the commits may run. */
	ms->prepared = 1;

	/* Succeeded: the state passed the check phase. */
	return 0;
}

/*
 * Reports what the selected screen holds: how far it got, the link, the
 * backlight, the watermarks, the commit's outer part, the flip and what it
 * owns.
 */
void
drv_i915_lcd_modeset_status(
	struct i915_display *display,
	struct i915_lcd_modeset_status *out)
{
	struct i915_lcd_world *world;
	struct i915_lcd_modeset *ms;
	struct intel_shared_dpll *pll;
	unsigned domain;
	int held;

	/* Without a record there is nothing to fill in. */
	if (out == NULL)
		return;

	world = display->lcd_world;
	ms = i915_modeset_selected_screen(world);
	kern_memset(out, 0, sizeof(*out));

	/* How far the screen got. */
	out->prepared = ms->prepared;
	out->crtc_active = ms->crtc.active;
	out->plane_armed = ms->plane_armed;

	/* The link as the driver set it; the sink's own status is read apart. */
	out->link_rate = ms->dig_port.dp.link_rate;
	out->lane_count = ms->dig_port.dp.lane_count;
	out->link_trained_flag = ms->dig_port.dp.link_trained;
	kern_memcpy(out->train_set, ms->dig_port.dp.train_set, sizeof(out->train_set));
	out->ddi_buf_ctl_value = ms->dig_port.dp.DP;

	/* The shared DPLL the crtc was given, when it has one. */
	pll = ms->crtc_state.shared_dpll;
	if (pll != NULL) {
		out->pll_on = pll->on;
		out->pll_active_mask = pll->active_mask;
		out->pll_wakeref = pll->wakeref;
		out->pll_pipe_mask = pll->state.pipe_mask;
	}

	out->pll_id = ms->dpll_id;

	/* The encoder's power references. */
	out->ddi_io_wakeref = ms->dig_port.ddi_io_wakeref;
	out->aux_wakeref = ms->dig_port.aux_wakeref;

	/* The backlight PWM as intel_backlight_setup() derived it. */
	out->backlight_present = ms->connector.panel.backlight.present;
	out->backlight_enabled = ms->connector.panel.backlight.enabled;
	out->backlight_setup_rc = ms->backlight_setup_rc;
	out->backlight_pwm_max = ms->connector.panel.backlight.pwm_level_max;
	out->backlight_level = ms->connector.panel.backlight.level;

	/* The watermarks and DDB computed for the plane. */
	out->wm_rc = ms->wm_rc;
	out->ddb_start = ms->crtc_state.wm.skl.plane_ddb[PLANE_PRIMARY].start;
	out->ddb_end = ms->crtc_state.wm.skl.plane_ddb[PLANE_PRIMARY].end;
	out->wm0_enable = ms->crtc_state.wm.skl.optimal.planes[PLANE_PRIMARY].wm[0].enable;
	out->wm0_blocks = ms->crtc_state.wm.skl.optimal.planes[PLANE_PRIMARY].wm[0].blocks;
	out->wm0_lines = ms->crtc_state.wm.skl.optimal.planes[PLANE_PRIMARY].wm[0].lines;
	out->dbuf_slices_wanted = ms->wm.new_dbuf.enabled_slices;
	out->mbus_joined = ms->wm.new_dbuf.joined_mbus;

	/* The commit's outer part: CDCLK, bandwidth and DC_OFF. */
	out->cdclk_rc = ms->cdclk_rc;
	out->cdclk_crtc_min = ms->cdclk.crtc_min;
	out->cdclk_bw_min = ms->cdclk.bw_min;
	out->cdclk_required_khz = ms->cdclk.cdclk;
	out->cdclk_required_vco = ms->cdclk.vco;
	out->cdclk_required_level = ms->cdclk.voltage_level;
	out->cdclk_change_needed = ms->cdclk.change_needed;
	out->bw_data_rate = ms->bw_data_rate;
	out->dc_off_held = ms->dc_off_held;

	/* Counts the power domains the crtc holds. */
	for (domain = 0U; domain < (unsigned)POWER_DOMAIN_NUM; domain++) {
		held = test_bit(domain, ms->crtc.enabled_power_domains.mask.bits);
		if (held)
			out->crtc_domains_held++;
	}

	/* The current global DBUF state and what the stop left. */
	out->dbuf_slices_now = ms->wm.old_dbuf.enabled_slices;
	out->mbus_joined_now = ms->wm.old_dbuf.joined_mbus;
	out->stop_unconfirmed = ms->stop_unconfirmed;
	out->retained = drv_i915_lcd_modeset_retained(display);
	out->dither = ms->crtc_state.dither;

	/* The flip. */
	out->cur_surf = ms->cur_surf;
	out->pend_surf = ms->pend_surf;
	out->flip_pending = ms->flip_pending;
	out->flip_stuck = ms->flip_stuck;
	out->flip_event_ref = ms->flip_event_ref;
	out->events_cancelled = ms->events_cancelled;
	out->flip_gen = ms->flip_gen;

	/* The backlight range and the user brightness. */
	out->backlight_min = ms->connector.panel.backlight.min;
	out->backlight_max = ms->connector.panel.backlight.max;
	out->backlight_user = ms->bl_user;
	out->backlight_user_max = ms->bl_user_max;

	/* The commits and the errors of the Linux text. */
	out->commits = ms->commits;
	out->errors = world->ms_errors_pool[world->ms_sel];
	out->first_error = world->ms_first_error_pool[world->ms_sel];

	/* The sink's link status has not been read into this record. */
	out->link_status_rc = EINVAL;
}

/*
 * Enables the selected screen's crtc: the backlight setup, the active
 * timings and hsw_crtc_enable() with the DDI and DP callees below it.
 *
 * The evidence that the link trained is the sink's own status, not the
 * flag the stop function sets.  Returns I915_LCD_MS_OK,
 * I915_LCD_MS_NOT_PREPARED, I915_LCD_MS_ERRORS or
 * I915_LCD_MS_LINK_NOT_TRAINED.
 */
int
drv_i915_lcd_modeset_enable(
	struct i915_display *display)
{
	struct i915_lcd_world *world;
	struct i915_lcd_modeset *ms;
	struct i915_lcd_modeset_status st;

	world = display->lcd_world;
	ms = i915_modeset_selected_screen(world);

	/* Binds the screen's objects to the Linux text. */
	i915_modeset_bind_current(display);

	/* Only a prepared screen whose crtc is off can be enabled. */
	if (!ms->prepared || ms->crtc.active)
		return I915_LCD_MS_NOT_PREPARED;

	/* The screen's device is the one the Linux text works on. */
	world->i915_lcd_cur_i915 = &ms->i915;

	/* The connector-init work that reads the hardware: the backlight setup (the panel only). */
	if (!ms->output_hdmi) {
		ms->backlight_setup_rc = drv_i915_lcd_ms_backlight_setup(ms);
		if (ms->backlight_setup_rc != 0)
			i915_modeset_on_error(world, "intel_backlight_setup failed (no PWM frequency from the hardware or the VBT)\n");
	}

	/* intel_enable_crtc(): the active timings before the crtc_enable hook. */
	drv_i915_lcd_ms_active_timings(ms);
	drv_i915_lcd_ms_crtc_enable(ms);

	/* intel_backlight_device_register(): max_brightness = backlight.max, brightness = the level scaled to it. */
	ms->bl_user_max = ms->connector.panel.backlight.max;
	ms->bl_user = 0U;
	if (ms->bl_user_max != 0U)
		ms->bl_user = drv_i915_lcd_ms_user_level(ms, ms->bl_user_max);

	/* An error of the Linux text fails the enable. */
	if (world->ms_errors_pool[world->ms_sel] != 0U)
		return I915_LCD_MS_ERRORS;

	/* HDMI has no link training: the crtc being active is the whole of it. */
	drv_i915_lcd_modeset_status(display, &st);
	if (ms->output_hdmi) {
		if (ms->crtc.active)
			return I915_LCD_MS_OK;

		return I915_LCD_MS_LINK_NOT_TRAINED;
	}

	/* Asks the sink whether clock recovery and channel equalisation hold. */
	i915_modeset_read_link_status(ms, &st);
	if (!ms->crtc.active || !st.cr_ok || !st.eq_ok)
		return I915_LCD_MS_LINK_NOT_TRAINED;

	/* Succeeded: the crtc runs and the sink reports a trained link. */
	return I915_LCD_MS_OK;
}

/*
 * Arms the selected screen's plane on its framebuffer.
 *
 * Returns I915_LCD_MS_OK, I915_LCD_MS_NOT_PREPARED or I915_LCD_MS_ERRORS.
 */
int
drv_i915_lcd_modeset_plane_update(
	struct i915_display *display)
{
	struct i915_lcd_world *world;
	struct i915_lcd_modeset *ms;

	world = display->lcd_world;
	ms = i915_modeset_selected_screen(world);

	/* Binds the screen's objects to the Linux text. */
	i915_modeset_bind_current(display);

	/* Only a running crtc takes a plane. */
	if (!ms->prepared || !ms->crtc.active)
		return I915_LCD_MS_NOT_PREPARED;

	/* Arms the plane through the Linux text. */
	world->i915_lcd_cur_i915 = &ms->i915;
	drv_i915_lcd_ms_plane_update(ms);

	/* An error of the Linux text fails the update. */
	if (world->ms_errors_pool[world->ms_sel] != 0U)
		return I915_LCD_MS_ERRORS;

	/* Succeeded: the plane is armed. */
	return I915_LCD_MS_OK;
}

/*
 * Disables the selected screen's plane.
 *
 * plane_armed is not cleared here: the write takes effect at the next
 * vblank, and only the caller, who watches the hardware, may decide that
 * the buffer is no longer scanned out.  Returns I915_LCD_MS_OK,
 * I915_LCD_MS_NOT_PREPARED or I915_LCD_MS_ERRORS.
 */
int
drv_i915_lcd_modeset_plane_disable(
	struct i915_display *display)
{
	struct i915_lcd_world *world;
	struct i915_lcd_modeset *ms;

	world = display->lcd_world;
	ms = i915_modeset_selected_screen(world);

	/* Binds the screen's objects to the Linux text. */
	i915_modeset_bind_current(display);

	/* Only a prepared screen has a plane to disable. */
	if (!ms->prepared)
		return I915_LCD_MS_NOT_PREPARED;

	/* Disables the plane through the Linux text. */
	world->i915_lcd_cur_i915 = &ms->i915;
	drv_i915_lcd_ms_plane_disable(ms);

	/* An error of the Linux text fails the disable. */
	if (world->ms_errors_pool[world->ms_sel] != 0U)
		return I915_LCD_MS_ERRORS;

	/* Succeeded: the plane disable is armed. */
	return I915_LCD_MS_OK;
}

/*
 * Disables the selected screen's crtc (hsw_crtc_disable()).
 *
 * Returns I915_LCD_MS_OK, I915_LCD_MS_NOT_PREPARED, I915_LCD_MS_ERRORS, or
 * I915_LCD_MS_STILL_OWNED when the PLL or a port power reference is still
 * held afterwards.
 */
int
drv_i915_lcd_modeset_disable(
	struct i915_display *display)
{
	struct i915_lcd_world *world;
	struct i915_lcd_modeset *ms;
	unsigned before;

	world = display->lcd_world;
	ms = i915_modeset_selected_screen(world);

	/* Only the errors of this disable decide. */
	before = world->ms_errors_pool[world->ms_sel];

	/* Binds the screen's objects to the Linux text. */
	i915_modeset_bind_current(display);

	/* Only a running crtc can be disabled. */
	if (!ms->prepared || !ms->crtc.active)
		return I915_LCD_MS_NOT_PREPARED;

	/* Disables the crtc through the Linux text. */
	drv_i915_lcd_ms_crtc_disable(ms);

	/* An error of the Linux text fails the disable. */
	if (world->ms_errors_pool[world->ms_sel] != before)
		return I915_LCD_MS_ERRORS;

	/* Everything the enable took must be given back. */
	if (ms->pll.on || ms->pll.active_mask != 0)
		return I915_LCD_MS_STILL_OWNED;
	if (ms->dig_port.ddi_io_wakeref != 0 || ms->dig_port.aux_wakeref != 0)
		return I915_LCD_MS_STILL_OWNED;

	/* Succeeded: the crtc is off and nothing is held. */
	return I915_LCD_MS_OK;
}

/*
 * Records that the caller confirmed on the hardware (frame counter,
 * scanline) that the plane is off: the framebuffer is the caller's again.
 */
void
drv_i915_lcd_modeset_plane_released(
	struct i915_display *display)
{
	struct i915_lcd_modeset *ms;

	ms = i915_modeset_selected_screen(display->lcd_world);

	/* The display reads neither buffer any more. */
	ms->plane_armed = 0;
	ms->flip_pending = 0;
	ms->flip_stuck = 0;
}

/*
 * Reads the sink's link status now into the record (for the run log).
 */
void
drv_i915_lcd_modeset_link_status(
	struct i915_display *display,
	struct i915_lcd_modeset_status *out)
{
	struct i915_lcd_world *world;
	struct i915_lcd_modeset *ms;

	world = display->lcd_world;
	ms = i915_modeset_selected_screen(world);

	/* Only a prepared screen has a sink to ask. */
	if (out == NULL || !ms->prepared)
		return;

	/* Reads the link status on the screen's device. */
	world->i915_lcd_cur_i915 = &ms->i915;
	i915_modeset_read_link_status(ms, out);
}

/*
 * Runs the enable commit of the selected screen.
 *
 * intel_atomic_commit_tail() reduced to this crtc, in the reference's order
 * and with its callees: DC_OFF get, the crtc's power domains, the DBUF
 * pre-plane update and the MBUS DBOX, the crtc enable and the plane
 * update, the DBUF post-plane update, the unused domains put, and DC_OFF
 * put asynchronously after 17 ms.  Adaptation: after an anomaly in the
 * enable the plane is not armed.  Returns I915_LCD_MS_* (the enable's
 * result first, then the plane update's).
 */
int
drv_i915_lcd_modeset_commit_enable(
	struct i915_display *display)
{
	struct i915_lcd_world *world;
	struct i915_lcd_modeset *ms;
	struct intel_power_domain_mask put_domains;
	int enable_result;
	int plane_result;
	int retained;

	world = display->lcd_world;
	ms = i915_modeset_selected_screen(world);
	plane_result = I915_LCD_MS_OK;

	/* Binds the screen's objects to the Linux text. */
	i915_modeset_bind_current(display);

	/* Only a prepared, idle screen that is not retained can be enabled. */
	retained = drv_i915_lcd_modeset_retained(display);
	if (!ms->prepared || ms->crtc.active || ms->dc_off_held || retained)
		return I915_LCD_MS_NOT_PREPARED;

	/* The screen's device and watermark state are the ones the Linux text works on. */
	world->i915_lcd_cur_i915 = &ms->i915;
	display->wm_world->i915_lcd_wm = &ms->wm;
	ms->state.base.dev = &ms->i915.drm;
	ms->commits++;

	/*
	 * "During full modesets we write a lot of registers, wait for PLLs,
	 * etc. Doing that while DC states are enabled is not a good idea."
	 */
	ms->dc_off_wakeref = i915_lcd_intel_display_power_get(&ms->i915, POWER_DOMAIN_DC_OFF);
	ms->dc_off_held = 1;
	i915_modeset_observe(world, I915_LCD_OBS_COMMIT_BEGIN);

	/*
	 * intel_atomic_prepare_plane_clear_colors() applies only to modifiers
	 * with a clear-colour plane; this one is linear.
	 */
	drv_i915_modeset_get_crtc_power_domains(world, &ms->crtc_state, &put_domains);

	/*
	 * intel_commit_modeset_disables(): the old crtc state is inactive.
	 * intel_pmdemand_pre_plane_update() returns below display version 14.
	 * intel_set_cdclk_pre_plane_update(): the required CDCLK state is the
	 * current one (checked in prepare).
	 */
	I915_LCD_DECIDED(&ms->i915, "intel_sagv_pre_plane_update: the QGV restriction stays as the initialisation left it (SAGV off, max-bandwidth point; bandwidth checked in prepare)");
	drv_i915_dbuf_pre_plane_update(&ms->wm, &ms->state);
	drv_i915_mbus_dbox_update(&ms->wm, &ms->state);

	/* skl_commit_modeset_enables(): intel_enable_crtc(), then intel_update_crtc() and the planes. */
	enable_result = drv_i915_lcd_modeset_enable(display);
	i915_modeset_observe(world, I915_LCD_OBS_PIPE_ENABLED);
	if (enable_result == I915_LCD_MS_OK) {
		plane_result = drv_i915_lcd_modeset_plane_update(display);
		i915_modeset_observe(world, I915_LCD_OBS_PLANE_ARMED);
	} else {
		I915_LCD_DECIDED(&ms->i915, "plane update skipped: the crtc enable reported an anomaly, the buffer is not handed to the display");
	}

	/* The post-plane half of the commit. */
	drv_i915_dbuf_post_plane_update(&ms->wm, &ms->state);
	drv_i915_modeset_put_crtc_power_domains(&ms->crtc, &put_domains);
	I915_LCD_DECIDED(&ms->i915, "intel_sagv_post_plane_update: QGV points are not relaxed (SAGV stays off)");

	/*
	 * intel_set_cdclk_post_plane_update() and
	 * intel_pmdemand_post_plane_update() have nothing to do, as above.  The
	 * new global state is the current one from here on.
	 */
	ms->wm.old_dbuf = ms->wm.new_dbuf;
	drv_i915_lcd_dbuf_publish(display->wm_world, &ms->wm.new_dbuf);
	i915_modeset_observe(world, I915_LCD_OBS_COMMIT_END);

	/* "Delay re-enabling DC states by 17 ms to avoid the off->on->off toggling overhead at and above 60 FPS." */
	intel_display_power_put_async_delay(&ms->i915, POWER_DOMAIN_DC_OFF, ms->dc_off_wakeref, I915_LCD_DC_OFF_DELAY_MS);
	ms->dc_off_held = 0;

	/* Reports a failed crtc enable first. */
	if (enable_result != I915_LCD_MS_OK)
		return enable_result;

	/* Reports a failed plane update. */
	if (plane_result != I915_LCD_MS_OK)
		return plane_result;

	/* Succeeded: the picture is armed. */
	return I915_LCD_MS_OK;
}

/*
 * Runs the disable commit of the selected screen.
 *
 * DC_OFF get, the domains to drop, the plane disable and the crtc
 * disable, then the DBUF pre-plane update, the MBUS DBOX and the DBUF
 * post-plane update, the domains put, the PLL reference given back, and
 * DC_OFF put.  Adaptation: after an error in the disable nothing further is
 * given back (DBUF, power domains and DC_OFF stay as they are) and the
 * stop is marked unconfirmed.  Returns I915_LCD_MS_*.
 */
int
drv_i915_lcd_modeset_commit_disable(
	struct i915_display *display)
{
	struct i915_lcd_world *world;
	struct i915_lcd_modeset *ms;
	struct intel_crtc_state *off_state;
	struct intel_power_domain_mask put_domains;
	unsigned before;
	int crtc_result;
	int plane_result;
	int retained;

	world = display->lcd_world;
	ms = i915_modeset_selected_screen(world);
	off_state = &world->i915_lcd_modeset_commit_disable_off_state;

	/* Binds the screen's objects to the Linux text. */
	i915_modeset_bind_current(display);

	/* Only a running screen that is not retained can be disabled. */
	retained = drv_i915_lcd_modeset_retained(display);
	if (!ms->prepared || !ms->crtc.active || ms->dc_off_held || retained)
		return I915_LCD_MS_NOT_PREPARED;

	/* The screen's device is the one the Linux text works on. */
	world->i915_lcd_cur_i915 = &ms->i915;
	ms->state.base.dev = &ms->i915.drm;
	ms->commits++;

	/* The check phase: the global DBUF state without this pipe. */
	drv_i915_lcd_ms_wm_compute_off(display->wm_world, ms);

	/* The new state of this commit: the crtc inactive. */
	*off_state = ms->crtc_state;
	off_state->hw.active = false;
	off_state->hw.enable = false;

	/* DC states off for the commit. */
	ms->dc_off_wakeref = i915_lcd_intel_display_power_get(&ms->i915, POWER_DOMAIN_DC_OFF);
	ms->dc_off_held = 1;
	i915_modeset_observe(world, I915_LCD_OBS_COMMIT_BEGIN);

	/* Nothing new is taken; everything held goes to put_domains. */
	drv_i915_modeset_get_crtc_power_domains(world, off_state, &put_domains);

	/*
	 * intel_commit_modeset_disables() -> intel_old_crtc_state_disables():
	 * the planes, then the crtc.  Only what this commit reports decides (an
	 * enable that failed earlier has left its errors in the count).
	 */
	before = world->ms_errors_pool[world->ms_sel];
	(void)drv_i915_lcd_modeset_plane_disable(display);
	plane_result = I915_LCD_MS_OK;
	if (world->ms_errors_pool[world->ms_sel] != before)
		plane_result = I915_LCD_MS_ERRORS;

	i915_modeset_observe(world, I915_LCD_OBS_PLANE_DISABLED);
	crtc_result = drv_i915_lcd_modeset_disable(display);
	i915_modeset_observe(world, I915_LCD_OBS_PIPE_DISABLED);

	/*
	 * The stop is not confirmed: shrinking the DBUF, dropping the pipe's
	 * power domains or letting DC states back in under a pipe that may still
	 * run is the one thing not to do.  Everything stays; the caller keeps
	 * the buffer.
	 */
	if (crtc_result != I915_LCD_MS_OK || plane_result != I915_LCD_MS_OK) {
		ms->stop_unconfirmed = 1;
		I915_LCD_DECIDED(&ms->i915, "commit tail after a failed disable NOT run: DBUF slices, crtc power domains and DC_OFF stay held");
		if (crtc_result != I915_LCD_MS_OK)
			return crtc_result;

		return plane_result;
	}

	/* The rest of the commit tail, its errors counted apart. */
	I915_LCD_DECIDED(&ms->i915, "intel_sagv_pre_plane_update: the QGV restriction stays as the initialisation left it");
	before = world->ms_errors_pool[world->ms_sel];
	drv_i915_dbuf_pre_plane_update(&ms->wm, &ms->state);
	drv_i915_mbus_dbox_update(&ms->wm, &ms->state);

	/* skl_commit_modeset_enables(): nothing is enabled by this commit. */
	drv_i915_dbuf_post_plane_update(&ms->wm, &ms->state);
	drv_i915_modeset_put_crtc_power_domains(&ms->crtc, &put_domains);
	ms->wm.old_dbuf = ms->wm.new_dbuf;
	drv_i915_lcd_dbuf_publish(display->wm_world, &ms->wm.new_dbuf);

	/* The crtc is off: its reference on the shared DPLL goes back (the object stays for any other pipe). */
	drv_i915_lcd_ms_release_pll(world, ms);
	i915_modeset_observe(world, I915_LCD_OBS_COMMIT_END);

	/*
	 * An error in the tail (e.g. the pipe's power well could not be turned
	 * off because its interrupt drain failed): the stop is not confirmed;
	 * DC_OFF stays held and the caller keeps the buffer.
	 */
	if (world->ms_errors_pool[world->ms_sel] != before) {
		ms->stop_unconfirmed = 1;
		I915_LCD_DECIDED(&ms->i915, "commit tail reported an error (power not released): DC_OFF kept, stop unconfirmed");
		return I915_LCD_MS_ERRORS;
	}

	/* DC states come back 17 ms later. */
	intel_display_power_put_async_delay(&ms->i915, POWER_DOMAIN_DC_OFF, ms->dc_off_wakeref, I915_LCD_DC_OFF_DELAY_MS);
	ms->dc_off_held = 0;

	/* Succeeded: the screen is off and everything is given back. */
	return I915_LCD_MS_OK;
}

/*
 * Records that the caller has taken the selected screen's retained
 * resources over after an unconfirmed stop.
 *
 * The object does not forget what it holds: stop_unconfirmed, the power
 * references, DC_OFF, the DBUF slices and the crtc state stay recorded,
 * and every later prepare is refused before anything is initialised.
 */
void
drv_i915_lcd_modeset_abandoned(
	struct i915_display *display)
{
	struct i915_lcd_world *world;
	struct i915_lcd_modeset *ms;

	world = display->lcd_world;
	ms = i915_modeset_selected_screen(world);

	/*
	 * The retained flag outlives prepare's clearing of the object and names
	 * the backend the retained display belongs to.
	 */
	ms->stop_unconfirmed = 1;
	world->ms_retained_pool[world->ms_sel] = 1;
	world->ms_retained_ops_pool[world->ms_sel] = world->ms_ops_pool[world->ms_sel];
}

/*
 * Reports whether the selected screen's display is retained after an
 * unconfirmed stop.
 */
int
drv_i915_lcd_modeset_retained(
	struct i915_display *display)
{
	struct i915_lcd_world *world;
	struct i915_lcd_modeset *ms;

	world = display->lcd_world;
	ms = i915_modeset_selected_screen(world);

	/* The caller took the resources over. */
	if (world->ms_retained_pool[world->ms_sel])
		return 1;

	/* The disable did not confirm the stop. */
	if (ms->stop_unconfirmed)
		return 1;

	/* Nothing is retained. */
	return 0;
}

/*
 * Discards a retained display that belongs to a model backend.
 *
 * The only way out of a retained state: permitted solely when the
 * retained state belongs to a register model, whose discarding is itself
 * the isolation.  On real hardware nothing is released.  Returns 0, or
 * EPERM for real hardware or a backend that does not hold it.
 */
int
drv_i915_lcd_modeset_discard_model(
	struct i915_display *display,
	const struct i915_lcd_emit *ops)
{
	struct i915_lcd_world *world;
	const struct i915_lcd_emit *holder;
	int retained;

	world = display->lcd_world;

	/* Nothing is retained: nothing to discard. */
	retained = drv_i915_lcd_modeset_retained(display);
	if (!retained)
		return 0;

	/* The backend that holds the retained state. */
	holder = world->ms_retained_ops_pool[world->ms_sel];
	if (holder == NULL)
		holder = world->ms_ops_pool[world->ms_sel];

	/* Only the model that holds it may discard it. */
	if (ops == NULL || ops != holder || !ops->model)
		return EPERM;

	/* Forgets the screen and its retained state. */
	kern_memset(&world->ms_pool[world->ms_sel], 0, sizeof(world->ms_pool[world->ms_sel]));
	world->ms_retained_pool[world->ms_sel] = 0;
	world->ms_retained_ops_pool[world->ms_sel] = NULL;

	/* Succeeded: the model's state is gone with it. */
	return 0;
}

/*
 * Flips the running picture of the selected screen to another buffer of
 * the same format, size and pitch.
 *
 * The reference's update of a running crtc: plane noarm,
 * intel_pipe_update_start() (vblank evasion), plane arm,
 * intel_pipe_update_end() (the event is armed).  COMPLETE only when the
 * event completed and the pipe's live surface is the new buffer; then the
 * old buffer is no longer displayed.  Otherwise both stay protected and
 * further flips are refused (flip_stuck) until the display is stopped.
 * Returns I915_LCD_MS_OK, I915_LCD_MS_NOT_PREPARED or I915_LCD_MS_ERRORS,
 * with the details in out.
 */
int
drv_i915_lcd_modeset_flip(
	struct i915_display *display,
	uint32_t new_surf,
	struct i915_lcd_flip_result *out)
{
	struct i915_lcd_world *world;
	struct i915_lcd_modeset *ms;
	struct i915_lcd_emit *ms_ops;
	struct i915_lcd_flip_result res;
	int dc_off;
	int armed;

	world = display->lcd_world;
	ms = i915_modeset_selected_screen(world);
	ms_ops = world->ms_ops_pool[world->ms_sel];

	/* Arms the flip; a refusal writes nothing. */
	armed = i915_modeset_flip_arm(display, new_surf, &res, &dc_off);
	if (armed != I915_LCD_MS_OK) {
		if (out != NULL)
			*out = res;
		return armed;
	}

	/* Waits for the event the pipe's next vblank completes. */
	res.event_rc = ms_ops->wait_event(ms_ops->ctx, ms->crtc.pipe, I915_LCD_FLIP_EVENT_MS);
	res.live_after = i915_modeset_live_surf(world);
	res.frame_after = i915_modeset_frame_now(world);

	/* drm_atomic_helper_wait_for_flip_done() came first; then the commit drops DC_OFF (async, 17 ms). */
	intel_display_power_put_async_delay(&ms->i915, POWER_DOMAIN_DC_OFF, dc_off, I915_LCD_DC_OFF_DELAY_MS);

	/* Decides what the flip did. */
	if (res.event_rc == 0) {
		/* drm_send_event: the event's vblank reference goes back. */
		ms_ops->vblank_put(ms_ops->ctx, ms->crtc.pipe);
		ms->flip_event_ref = 0;

		/* The live surface decides whether the old buffer is free. */
		if (res.live_after == new_surf) {
			ms->cur_surf = new_surf;
			ms->flip_pending = 0;
			res.result = I915_LCD_FLIP_DONE;
		} else {
			ms->flip_stuck = 1;
			res.result = I915_LCD_FLIP_NOT_LATCHED;
		}
	} else {
		/* The event reference stays: the completion may still come. */
		ms->flip_stuck = 1;
		res.result = I915_LCD_FLIP_TIMEOUT;
	}

	/* Reports the details. */
	if (out != NULL)
		*out = res;

	/* A flip that did not complete keeps both buffers. */
	if (res.result != I915_LCD_FLIP_DONE)
		return I915_LCD_MS_ERRORS;

	/* An error of the Linux text during the update fails the flip. */
	if (res.update_errors != 0)
		return I915_LCD_MS_ERRORS;

	/* Succeeded: the new buffer is displayed and the old one is free. */
	return I915_LCD_MS_OK;
}

/*
 * Flips the running picture of the selected screen to another buffer
 * without waiting for the flip to complete.
 *
 * The flip is armed as drv_i915_lcd_modeset_flip() arms it and latches at
 * the pipe's next vblank; until then both buffers stay protected.
 * drv_i915_lcd_modeset_flip_poll() tells when it has latched and
 * drv_i915_lcd_modeset_flip_settle() waits for it.  A flip still pending is
 * refused like a busy one, so the caller polls first.  Returns
 * I915_LCD_MS_OK with the result I915_LCD_FLIP_ARMED,
 * I915_LCD_MS_NOT_PREPARED for a refusal, or I915_LCD_MS_ERRORS for an
 * error of the Linux text during the update, with the details in out.
 */
int
drv_i915_lcd_modeset_flip_nowait(
	struct i915_display *display,
	uint32_t new_surf,
	struct i915_lcd_flip_result *out)
{
	struct i915_lcd_modeset *ms;
	struct i915_lcd_flip_result res;
	int dc_off;
	int armed;

	ms = i915_modeset_selected_screen(display->lcd_world);

	/* Arms the flip; a refusal writes nothing. */
	armed = i915_modeset_flip_arm(display, new_surf, &res, &dc_off);
	if (armed != I915_LCD_MS_OK) {
		if (out != NULL)
			*out = res;
		return armed;
	}

	/*
	 * Drops DC_OFF as the commit does after its wait; the drop is delayed
	 * by 17 ms, longer than one frame, so the flip latches first.
	 */
	intel_display_power_put_async_delay(&ms->i915, POWER_DOMAIN_DC_OFF, dc_off, I915_LCD_DC_OFF_DELAY_MS);
	res.result = I915_LCD_FLIP_ARMED;

	/* Reports the details. */
	if (out != NULL)
		*out = res;

	/* An error of the Linux text during the update fails the flip. */
	if (res.update_errors != 0)
		return I915_LCD_MS_ERRORS;

	/* Succeeded: the flip is armed and latches at the next vblank. */
	return I915_LCD_MS_OK;
}

/*
 * Tells whether the selected screen has no flip pending, completing an
 * armed flip that the pipe has latched.
 *
 * A latched flip (the live surface is the new buffer) is completed as the
 * event would complete it: the event is given up, its vblank reference goes
 * back and the old buffer is free.  Returns 1 when no flip is pending (a
 * stuck one included, which the next flip refuses), 0 while the armed flip
 * still waits for its vblank.
 */
int
drv_i915_lcd_modeset_flip_poll(
	struct i915_display *display)
{
	struct i915_lcd_world *world;
	struct i915_lcd_modeset *ms;
	uint32_t live;

	world = display->lcd_world;
	ms = i915_modeset_selected_screen(world);

	/* Nothing pending, or a flip that will never complete. */
	if (!ms->flip_pending || ms->flip_stuck)
		return 1;

	/* The pipe still scans the old buffer: the flip waits for its vblank. */
	live = i915_modeset_live_surf(world);
	if (live != ms->pend_surf)
		return 0;

	/* The flip has latched: completes it. */
	i915_modeset_flip_retire(display);

	/* No flip is pending any more. */
	return 1;
}

/*
 * Waits until the armed flip of the selected screen has latched, from a
 * thread that is not the worker: the presenting thread, so the worker is
 * free to run the next frame's rendering meanwhile.
 *
 * Nothing of the screen's state is changed: the worker retires the latched
 * flip when it takes the next presentation (drv_i915_lcd_modeset_flip_poll()).
 * A flip that has latched already is not waited for, so a late caller does
 * not pay for one more vblank.  Returns 0 when the flip has latched, or
 * EIO when it did not within the flip event time.
 */
int
drv_i915_lcd_modeset_flip_wait(
	struct i915_display *display)
{
	struct i915_lcd_world *world;
	struct i915_lcd_modeset *ms;
	struct i915_lcd_emit *ms_ops;
	uint32_t live;
	int event_rc;

	world = display->lcd_world;
	ms = i915_modeset_selected_screen(world);
	ms_ops = world->ms_ops_pool[world->ms_sel];

	/* Nothing armed, or a flip that will never complete: nothing to wait for. */
	if (!ms->flip_pending || ms->flip_stuck)
		return 0;

	/* A flip that has latched already. */
	live = i915_modeset_live_surf(world);
	if (live == ms->pend_surf)
		return 0;

	/* Waits for the vblank the flip latches at, then checks that it did. */
	event_rc = ms_ops->wait_event(ms_ops->ctx, ms->crtc.pipe, I915_LCD_FLIP_EVENT_MS);
	live = i915_modeset_live_surf(world);
	if (live == ms->pend_surf)
		return 0;

	/* The vblank came and went without the new buffer, or never came. */
	kern_logf("i915: resident display: flip to 0x%08x did not latch: event_rc=%d live 0x%08x\n",
	    ms->pend_surf,
	    event_rc,
	    live);
	return EIO;
}

/*
 * Waits for the armed flip of the selected screen to complete, before the
 * buffers change hands (the stop, or a flip that waits).
 *
 * Returns 0 when no flip is pending any more, or EIO when the armed flip
 * did not complete in time or did not latch the new buffer; both buffers
 * then stay protected (flip_stuck).
 */
int
drv_i915_lcd_modeset_flip_settle(
	struct i915_display *display)
{
	struct i915_lcd_world *world;
	struct i915_lcd_modeset *ms;
	struct i915_lcd_emit *ms_ops;
	uint32_t live;
	int idle;
	int event_rc;

	world = display->lcd_world;
	ms = i915_modeset_selected_screen(world);
	ms_ops = world->ms_ops_pool[world->ms_sel];

	/* A flip that has latched, or none, needs no wait. */
	idle = drv_i915_lcd_modeset_flip_poll(display);
	if (idle)
		return 0;

	/* Waits for the event the pipe's next vblank completes, then for the live surface. */
	event_rc = ms_ops->wait_event(ms_ops->ctx, ms->crtc.pipe, I915_LCD_FLIP_EVENT_MS);
	live = i915_modeset_live_surf(world);

	/* An event that did not come, or a surface that did not latch, keeps both buffers. */
	if (event_rc != 0 || live != ms->pend_surf) {
		ms->flip_stuck = 1;
		kern_logf("i915: resident display: flip to 0x%08x did not complete: event_rc=%d live 0x%08x\n",
		    ms->pend_surf,
		    event_rc,
		    live);
		return EIO;
	}

	/* drm_send_event: the event's vblank reference goes back, and the old buffer is free. */
	ms_ops->vblank_put(ms_ops->ctx, ms->crtc.pipe);
	ms->flip_event_ref = 0;
	ms->cur_surf = ms->pend_surf;
	ms->flip_pending = 0;

	/* Succeeded: no flip is pending. */
	return 0;
}

/*
 * Shows one buffer somebody else prepared and stops it safely.
 *
 * The pixels are not touched and the buffer is not created, unpinned or
 * destroyed here.  It must be pinned and unused.  On return it is either
 * pinned again (display_released: the stop was confirmed; the owner
 * releases it when its other users are done) or abandoned (the stop was
 * not confirmed: kept for ever, the device latch is set).  verify
 * (optional) re-checks the pixels after the stop.  Returns 0 when the
 * picture was up for the whole window and the stop was confirmed; EINVAL
 * or EBUSY when nothing was touched; EIO when the run did not pass.
 */
int
drv_i915_lcd_show_prepared(
	struct i915_display *display,
	struct i915_lcd_show_env *env,
	struct i915_scanout *so,
	uint32_t (*verify)(void *ctx, const struct i915_scanout *so),
	void *verify_ctx,
	struct i915_lcd_show_report *r)
{
	int begin_error;

	/* Refuses a call without its inputs. */
	if (env == NULL || r == NULL || env->hw == NULL || env->lcd == NULL || so == NULL)
		return EINVAL;

	/* An earlier run left resources the display or the GPU may still use: nothing is touched. */
	begin_error = i915_show_begin(display, env, r);
	if (begin_error != 0)
		return begin_error;

	/* The buffer must be pinned and unused. */
	if (so->state != I915_SCANOUT_PINNED) {
		i915_show_anomaly(r, "the prepared buffer is not a pinned, unused scanout buffer", EINVAL);
		return EINVAL;
	}

	i915_show_reached(env, r, I915_LCD_SHOW_BUFFER_READY);

	/* Shows it and stops it. */
	(void)i915_show_display(display, env, so, r, verify, verify_ctx);

	/* For the prepared path: the display let go; the owner releases the buffer. */
	r->released = r->display_released;
	r->pass = i915_show_passed(display, r, r->display_released);
	if (!r->pass)
		return EIO;

	/* Succeeded: the picture was up and the display let go of it. */
	return 0;
}

/*
 * Shows one known picture in a buffer of its own and stops it safely.
 *
 * Creates, fills and publishes the buffer, shows it, stops it and
 * releases it.  Returns 0 when the picture was up for the whole window,
 * the stop was confirmed and everything was given back; EINVAL or EBUSY
 * when nothing was touched; EIO when the run did not pass.
 */
int
drv_i915_lcd_show_run(
	struct i915_display *display,
	struct i915_lcd_show_env *env,
	struct i915_lcd_show_report *r)
{
	struct i915_scanout *so;
	int begin_error;
	int shown;

	/* Refuses a call without its inputs. */
	if (env == NULL || r == NULL || env->hw == NULL || env->gm == NULL || env->so == NULL || env->lcd == NULL)
		return EINVAL;

	/* An earlier run left resources the display or the GPU may still use: nothing is touched. */
	begin_error = i915_show_begin(display, env, r);
	if (begin_error != 0)
		return begin_error;

	/* The storage must be free (an earlier buffer may have been abandoned). */
	so = env->so;
	if (so->state != I915_SCANOUT_NONE) {
		i915_show_anomaly(r, "the scanout storage is still in use (an earlier buffer was abandoned?)", EBUSY);
		return EBUSY;
	}

	/* The display window of the GGTT; EBUSY means it was claimed earlier, which is fine. */
	r->window_rc = drv_i915_gt_display_window_init(env->gm, I915_GT_DISPLAY_PAGES);
	if (r->window_rc != 0 && r->window_rc != EBUSY) {
		i915_show_anomaly(r, "display GGTT window", r->window_rc);
		return EIO;
	}

	/* Creates the buffer at the panel's size. */
	r->create_rc = drv_i915_scanout_create(env->gm, (uint32_t)env->lcd->mode.hdisplay, (uint32_t)env->lcd->mode.vdisplay, I915_FOURCC_XRGB8888, I915_MOD_LINEAR, so);
	if (r->create_rc != 0) {
		i915_show_anomaly(r, "scanout create", r->create_rc);
		return EIO;
	}

	/* Pins it for the display. */
	r->pin_rc = drv_i915_scanout_pin(so, "lcd-b");
	if (r->pin_rc != 0) {
		i915_show_anomaly(r, "scanout pin", r->pin_rc);
		r->destroy_rc = drv_i915_scanout_destroy(so);
		return EIO;
	}

	/* Draws the picture, publishes it and reads it back. */
	r->surf = so->surf;
	r->pattern_hash = drv_i915_lcd_pattern_fill(so->cpu, so->pitch, so->width, so->height, env->pattern_id);
	drv_i915_scanout_publish(so);
	r->readback_bad_before = drv_i915_lcd_pattern_verify(so->cpu, so->pitch, so->width, so->height, env->pattern_id, NULL, NULL);
	if (r->readback_bad_before != 0U || (env->pattern_fnv != 0U && r->pattern_hash != env->pattern_fnv)) {
		i915_show_anomaly(r, "the picture in the buffer is not the expected one", EIO);
		i915_show_release_unused(so, r);
		return EIO;
	}

	i915_show_reached(env, r, I915_LCD_SHOW_BUFFER_READY);

	/* Shows it and stops it; a buffer the display never saw is released the ordinary way. */
	shown = i915_show_display(display, env, so, r, i915_show_pattern_verify, env);
	if (!shown) {
		i915_show_release_unused(so, r);
		return EIO;
	}

	/* The display let go: the buffer is released. */
	if (r->display_released) {
		r->unpin_rc = drv_i915_scanout_unpin(so);
		r->destroy_rc = EBUSY;
		if (r->unpin_rc == 0)
			r->destroy_rc = drv_i915_scanout_destroy(so);

		r->released = 0;
		if (r->unpin_rc == 0 && r->destroy_rc == 0)
			r->released = 1;

		/* A buffer that cannot be released after a confirmed stop is an anomaly. */
		if (!r->released) {
			if (r->unpin_rc != 0) {
				i915_show_anomaly(r, "the buffer could not be released after a confirmed stop", r->unpin_rc);
			} else {
				i915_show_anomaly(r, "the buffer could not be released after a confirmed stop", r->destroy_rc);
			}
		} else {
			i915_show_reached(env, r, I915_LCD_SHOW_RELEASED);
		}
	}

	/* Judges the run. */
	r->pass = i915_show_passed(display, r, r->released);
	if (!r->pass)
		return EIO;

	/* Succeeded: the picture was up, stopped and given back. */
	return 0;
}

/*
 * Reports whether a run left resources the display or the GPU may still
 * use.
 *
 * The device-side latch is set when a run ends abandoned and never cleared
 * by a later run; every run refuses before it initialises anything while
 * it is set, and the outer teardown asks it before releasing the DMA
 * device, the scratch page, the BARs or bus mastering.
 */
int
drv_i915_lcd_show_retained(
	struct i915_display *display)
{
	/* The display was not shown to have stopped. */
	if (display->show_retained)
		return 1;

	/* The GPU was not shown to be done. */
	if (display->show_gpu_retained)
		return 1;

	/* Nothing is retained. */
	return 0;
}

/*
 * Records that a buffer's other user, the GPU, is not shown to have
 * stopped using it.
 *
 * The same device-side retained state as an unconfirmed display stop; gm
 * names the memory manager that owns the kept objects.
 */
void
drv_i915_lcd_show_retain_gpu(
	struct i915_display *display,
	const void *gm,
	const char *why)
{
	const char *reason;

	/* The latch and the manager it belongs to. */
	display->show_gpu_retained = 1;
	display->show_gpu_retained_gm = gm;
	display->show_gpu_retained_why = why;

	/* Names the reason in the log. */
	reason = "-";
	if (why != NULL)
		reason = why;

	kern_logf("i915: retained (GPU not shown to be done): %s -- the buffer and every object the request may use are kept\n", reason);
}

/*
 * Reports whether the GPU was not shown to be done with a retained buffer.
 */
int
drv_i915_lcd_show_gpu_retained(
	struct i915_display *display)
{
	/* Reports the latch. */
	return display->show_gpu_retained;
}

/*
 * Drops the GPU latch for a memory manager that has since been finalised.
 *
 * Its objects are gone with it (GPU-free tests).  Returns 0, or EPERM for
 * another manager or one that still exists.
 */
int
drv_i915_lcd_show_discard_gpu_model(
	struct i915_display *display,
	const void *gm,
	int gm_finalised)
{
	/* Nothing is latched. */
	if (!display->show_gpu_retained)
		return 0;

	/* Only the finalised manager that holds it may drop it. */
	if (gm == NULL || gm != display->show_gpu_retained_gm || !gm_finalised)
		return EPERM;

	/* Forgets the latch. */
	display->show_gpu_retained = 0;
	display->show_gpu_retained_gm = NULL;
	display->show_gpu_retained_why = NULL;

	/* Succeeded: the latch is dropped. */
	return 0;
}

/*
 * Drops the display latch of a run on a register model.
 *
 * Returns 0, or EPERM for real hardware or a backend that does not hold it.
 */
int
drv_i915_lcd_show_discard_model(
	struct i915_display *display,
	struct i915_lcd_show_env *env)
{
	int discarded;

	/* Nothing is latched. */
	if (!display->show_retained)
		return 0;

	/* Only the model the retained run used may drop it. */
	if (env == NULL || env->hw == NULL)
		return EPERM;
	if (env->hw != display->show_retained_hw || !env->hw->model)
		return EPERM;

	/* The modeset object of the model goes first. */
	discarded = drv_i915_lcd_modeset_discard_model(display, &display->show_trace.ops);
	if (discarded != 0)
		return EPERM;

	/* Forgets the latch. */
	display->show_retained = 0;
	display->show_retained_hw = NULL;

	/* Succeeded: the latch is dropped. */
	return 0;
}

/*
 * Lights the panel as the display of the resident node.
 *
 * Two full-panel buffers, one modeset, synchronous flips: buffer A is
 * shown through the show body, and serve runs inside its observation
 * window (after the first frames were seen) until the display is to be
 * given back; the reference's stop path follows and both buffers are
 * released.  Returns 0 when the panel came up and was stopped and released
 * cleanly; EINVAL (a dependency is missing), EBUSY (resources of an earlier
 * run are retained) or EIO otherwise.
 */
int
drv_i915_lcd_kernel_resident_run(
	struct i915_display *display,
	const struct i915_lcd_kernel_deps *d,
	int (*serve)(void *ctx),
	void *ctx)
{
	struct i915_lcd_kernel *k;
	struct i915_lcd_show_env *env;
	struct i915_lcd_show_report *rep;
	const char *verdict;
	const char *stop;
	const char *first_anomaly;
	int show_result;
	int released;
	int passed;
	int held;
	int retained;
	int buffers_error;
	int preflight_error;
	int fill_error;
	unsigned domain;

	k = &display->lk;
	env = &display->resident_run_env;
	rep = &display->resident_run_rep;

	/* The run needs the panel, the registers, the GT memory, the interrupts and a serve loop. */
	if (d == NULL || d->edp == NULL || d->mmio == NULL || d->gm == NULL || d->irq == NULL || serve == NULL) {
		kern_logf("i915: resident display: not started (a dependency is missing)\n");
		return EINVAL;
	}

	/* An earlier run left resources the display may still read: refused. */
	retained = drv_i915_lcd_show_retained(display);
	if (!retained)
		retained = drv_i915_lcd_modeset_retained(display);
	if (retained ||
	    display->resident_buf[0].state != I915_SCANOUT_NONE ||
	    display->resident_buf[1].state != I915_SCANOUT_NONE) {
		kern_logf("i915: resident display: refused (resources of an earlier run are retained)\n");
		return EBUSY;
	}

	/* The two modeset locks live as long as the device. */
	drv_i915_lcd_kernel_locks_init(display);

	/* Starts the run with its hooks bound over the normal initialisation's objects. */
	kern_memset(k, 0, sizeof(*k));
	k->locks = display->lcdb_locks;
	k->d = d;
	drv_i915_lcd_kernel_bind_ops(k);

	/* The hardware must be as the initialisation left it, and the inputs must be complete. */
	kern_memset(env, 0, sizeof(*env));
	preflight_error = drv_i915_lcd_kernel_preflight(k);
	fill_error = 0;
	if (preflight_error == 0)
		fill_error = drv_i915_lcd_kernel_fill_cfg(k, &env->cfg);
	if (preflight_error != 0 || fill_error != 0) {
		kern_logf("i915: resident display: not started (preflight: nothing was written)\n");
		return EIO;
	}

	/* Creates, pins, clears and publishes both buffers. */
	buffers_error = i915_resident_buffers(display, d);
	if (buffers_error != 0)
		return EIO;

	/* Buffer A is shown first; the serve loop runs in the window. */
	display->resident_front = 0U;
	display->resident_serve = serve;
	display->resident_serve_ctx = ctx;
	k->flip_a = &display->resident_buf[0];
	k->flip_b = &display->resident_buf[1];

	/* The show environment: the window is the time serve runs. */
	env->hw = &k->ops;
	env->gm = d->gm;
	env->lcd = &d->edp->lcd;
	env->pipe = 0;
	env->first_frames_ms = I915_LCD_FIRST_FRAMES_MS;
	env->window_ms = 0U;
	env->in_window = i915_resident_window;
	env->in_window_ctx = k;
	env->at_stage = drv_i915_lcd_kernel_at_stage;
	env->at_stage_ctx = k;
	k->window_ms = 0U;

	/* Shows buffer A, serves in the window, and stops through the reference's path. */
	show_result = drv_i915_lcd_show_prepared(display, env, &display->resident_buf[0], i915_resident_verify, NULL, rep);

	/* Counts the power references the run still holds. */
	held = 0;
	for (domain = 0U; domain < I915_PW_DOMAIN_NUM; domain++)
		held += k->power_refs[domain];

	/* Releases both buffers, or abandons them when the display may still read them. */
	released = i915_resident_release(display, rep);

	/*
	 * Judges the run on the counters: the run log holds a bounded number of
	 * entries and a display that flips for as long as an application
	 * presents always outgrows it.
	 */
	passed = i915_resident_passed(rep);
	if (!passed || !released || held != 0) {
		drv_i915_lcd_log_trace(rep->trace);
		drv_i915_lcd_log_observer(&rep->obs);
	} else {
		kern_logf("i915: resident display: run log %u writes, %u rmw, %u waits (0 timed out), 0 errors, 0 unresolved steps (%u entries kept, %u not kept)\n",
		    rep->trace->writes,
		    rep->trace->rmws,
		    rep->trace->waits,
		    rep->trace->n,
		    rep->trace->dropped);
	}

	/* Reports how the run ended. */
	verdict = "FAIL";
	if (passed && released && held == 0)
		verdict = "PASS";

	stop = "NOT confirmed";
	if (rep->display_released)
		stop = "confirmed";

	first_anomaly = "none";
	if (rep->first_anomaly != NULL)
		first_anomaly = rep->first_anomaly;

	kern_logf("i915: resident display: ended %s (show rc=%d; flips %u, stop %s, buffers released=%d, power refs held %d, first anomaly: %s)\n",
	    verdict,
	    show_result,
	    k->flips_done,
	    stop,
	    released,
	    held,
	    first_anomaly);

	/* The serve loop is no longer called. */
	display->resident_serve = NULL;

	/* A run that did not pass, did not give everything back or still holds power failed. */
	if (!passed || !released || held != 0)
		return EIO;

	/* Succeeded: the panel came up, stopped and was released. */
	return 0;
}

/*
 * Creates the two modeset mutexes once.
 *
 * They live as long as the device.
 */
void
drv_i915_lcd_kernel_locks_init(
	struct i915_display *display)
{
	/* The mutexes exist already. */
	if (display->lcdb_locks_live)
		return;

	/* The dpll lock and the backlight lock. */
	(void)mutex_init(&display->lcdb_locks[I915_LCD_LOCK_DPLL], LOCK_RANK_DEVICE, "i915-lcd-dpll");
	(void)mutex_init(&display->lcdb_locks[I915_LCD_LOCK_BACKLIGHT], LOCK_RANK_DEVICE, "i915-lcd-backlight");
	display->lcdb_locks_live = 1;
}

/*
 * Binds the hooks of a panel run: registers, waits and time here; DPCD and
 * panel power through the resident eDP; power domains, vblank and events,
 * and the note, step, error and debug hooks through their owners.  The
 * observe hook is left to the run log's tap; the observer reads through
 * these hooks directly.
 */
void
drv_i915_lcd_kernel_bind_ops(
	struct i915_lcd_kernel *k)
{
	/* Starts with every hook empty; the run is their context. */
	kern_memset(&k->ops, 0, sizeof(k->ops));
	k->ops.ctx = k;

	/* Registers, waits and time. */
	k->ops.write32 = i915_kernel_write32;
	k->ops.rmw32 = i915_kernel_rmw32;
	k->ops.posting_read = i915_kernel_posting_read;
	k->ops.read32 = i915_kernel_read32;
	k->ops.wait_reg = i915_kernel_wait_reg;
	k->ops.usleep = i915_kernel_usleep;
	k->ops.udelay = i915_kernel_udelay;

	/* The sink's DPCD and the panel power sequencer of the resident eDP. */
	k->ops.dpcd_read = drv_i915_edp_emit_dpcd_read;
	k->ops.dpcd_write = drv_i915_edp_emit_dpcd_write;
	k->ops.read_dpcd_caps = drv_i915_edp_emit_read_dpcd_caps;
	k->ops.panel = drv_i915_edp_emit_panel;

	/* Display power and the DBUF slices. */
	k->ops.power_get = drv_i915_lcd_power_get;
	k->ops.power_get_if_enabled = drv_i915_lcd_power_get_if_enabled;
	k->ops.power_put = drv_i915_lcd_power_put;
	k->ops.power_put_async = drv_i915_lcd_power_put_async;
	k->ops.dbuf_slices_update = i915_kernel_dbuf_slices_update;

	/* The locks, the steps, the errors and the debug messages. */
	k->ops.lock = i915_kernel_lock;
	k->ops.step = drv_i915_lcd_kernel_step;
	k->ops.error = drv_i915_lcd_kernel_error;
	k->ops.debug = drv_i915_lcd_kernel_debug;

	/* The synchronous update: vblank references, the vblank sleep, the section's interrupts and the event. */
	k->ops.vblank_get = drv_i915_lcd_kernel_vblank_get;
	k->ops.vblank_put = drv_i915_lcd_kernel_vblank_put;
	k->ops.vblank_sleep = drv_i915_lcd_kernel_vblank_sleep;
	k->ops.irq_off = i915_kernel_irq_off;
	k->ops.irq_on = i915_kernel_irq_on;
	k->ops.arm_event = drv_i915_lcd_kernel_arm_event;
	k->ops.wait_event = drv_i915_lcd_kernel_wait_event;
	k->ops.cancel_event = drv_i915_lcd_kernel_cancel_event;
}

/*
 * Checks that the hardware is in the state the normal initialisation
 * leaves; anything else is not this run's to fix.  Returns 0, or EBUSY
 * when nothing may be written.
 */
int
drv_i915_lcd_kernel_preflight(
	struct i915_lcd_kernel *k)
{
	struct i915_display *display;
	const struct i915_lcd_kernel_deps *d;
	uint32_t transconf;
	uint32_t plane_ctl;
	uint32_t pll;
	uint32_t ddi;
	uint32_t imr;
	uint32_t want;
	int idle;
	int masked;
	int ok;
	int hdmi_error;

	display = i915_kernel_display(k);
	d = k->d;
	ok = 1;

	/* An HDMI run checks its own pipe and port. */
	if (k->p != NULL && k->p->output_hdmi) {
		hdmi_error = i915_kernel_preflight_hdmi(k);
		if (hdmi_error != 0)
			return hdmi_error;

		return 0;
	}

	/* The resident eDP must be live and its state computed. */
	if (!d->edp->connector_live || d->edp->lcd_rc != 0) {
		kern_logf("i915: LCD-B preflight: the resident eDP is not live (connector_live=%d lcd_rc=%d)\n",
		    d->edp->connector_live,
		    d->edp->lcd_rc);
		return EBUSY;
	}

	/* The scanout buffer needs the GT memory's GGTT. */
	if (!d->gm->inited) {
		kern_logf("i915: LCD-B preflight: no GT memory (GGTT) for the scanout buffer\n");
		return EBUSY;
	}

	/* Logs the registers and reads the pipe, plane, PLL, DDI and interrupt mask. */
	drv_i915_lcd_log_regs(display->lcd_world, k, "preflight", 0);
	transconf = drv_i915_read32(d->mmio, drv_i915_lcd_reg_by_name(display->lcd_world, "TRANSCONF"));
	plane_ctl = drv_i915_read32(d->mmio, drv_i915_lcd_reg_by_name(display->lcd_world, "PLANE_CTL_1"));
	pll = drv_i915_read32(d->mmio, drv_i915_lcd_reg_by_name(display->lcd_world, "ICL_DPLL_ENABLE"));
	ddi = drv_i915_read32(d->mmio, drv_i915_lcd_reg_by_name(display->lcd_world, "DDI_BUF_CTL"));
	imr = drv_i915_read32(d->mmio, drv_i915_lcd_reg_by_name(display->lcd_world, "GEN8_DE_PIPE_IMR"));

	/* A running display means the start conditions are not the prepared ones: nothing may be written. */
	if ((transconf & I915_LCD_TRANSCONF_ON_MASK) != 0U ||
	    (plane_ctl & I915_LCD_ENABLE_BIT) != 0U ||
	    (pll & I915_LCD_ENABLE_BIT) != 0U ||
	    (ddi & I915_LCD_ENABLE_BIT) != 0U) {
		kern_logf("i915: LCD-B preflight: the display is not idle (TRANSCONF 0x%08x PLANE_CTL 0x%08x DPLL 0x%08x DDI_BUF_CTL 0x%08x)\n",
		    transconf,
		    plane_ctl,
		    pll,
		    ddi);
		ok = 0;
	}

	/*
	 * Pipe A's registers sit in power well A, which is off until the
	 * commit takes the pipe's power domain: they read 0 here and say
	 * nothing.  Checked instead: the interrupt state's mask, which the
	 * power-well enable programs, keeps vblank (bit 0) and underrun (bit
	 * 31, and bits 22 / 21 from display version 13) masked.
	 */
	want = I915_LCD_KEEP_MASKED_GEN12;
	if (drv_i915_lcd_display_ver() >= 13)
		want = I915_LCD_KEEP_MASKED_XELPD;

	if ((d->irq->de_irq_mask[0] & want) != want) {
		kern_logf("i915: LCD-B preflight: the IRQ state's pipe A mask 0x%08x would leave vblank / underrun unmasked (this display needs 0x%08x)\n",
		    d->irq->de_irq_mask[0],
		    want);
		ok = 0;
	}

	/* Logs what was found. */
	idle = 0;
	if ((pll & I915_LCD_ENABLE_BIT) == 0U && (ddi & I915_LCD_ENABLE_BIT) == 0U)
		idle = 1;

	masked = 0;
	if ((d->irq->de_irq_mask[0] & I915_LCD_KEEP_MASKED_XELPD) == I915_LCD_KEEP_MASKED_XELPD)
		masked = 1;

	kern_logf("i915: LCD-B preflight: DDI / PLL idle=%d (pipe A registers read TRANSCONF 0x%08x PLANE_CTL 0x%08x IMR 0x%08x with power well A off) | IRQ state pipe A mask 0x%08x: vblank + underrun masked=%d | no code of this path waits for a software vblank count; no worker / callback / waiter is created by it\n",
	    idle,
	    transconf,
	    plane_ctl,
	    imr,
	    d->irq->de_irq_mask[0],
	    masked);

	/* The display is not in the prepared state. */
	if (!ok)
		return EBUSY;

	/* Succeeded: the run may write the display. */
	return 0;
}

/*
 * Fills the modeset configuration of a panel run from the objects the
 * normal initialisation built: the VBT panel data, the resident eDP's
 * capabilities and readouts, the watermark latencies, the DBUF, CDCLK and
 * QGV state, and the loaded DMC firmware.  Returns 0, or EINVAL when the
 * VBT has no panel data for port A.
 */
int
drv_i915_lcd_kernel_fill_cfg(
	struct i915_lcd_kernel *k,
	struct i915_lcd_modeset_cfg *c)
{
	struct i915_display *display;
	const struct i915_lcd_kernel_deps *d;
	struct i915_edp_device *edp;
	struct i915_vbt_panel *pn;
	unsigned allowed;
	unsigned point;
	unsigned i;
	uint32_t mbus;
	int panel_error;

	display = i915_kernel_display(k);
	d = k->d;
	edp = d->edp;
	pn = &display->fill_cfg_pn;

	/* Reads the panel's VBT data for port A. */
	kern_memset(c, 0, sizeof(*c));
	panel_error = drv_i915_vbt_init_panel(&edp->vbt->parsed, 0, edp->res.edid, pn);
	if (panel_error != 0) {
		kern_logf("i915: LCD-B input: the VBT has no panel data for port A (rc=%d)\n", panel_error);
		return EINVAL;
	}

	/* Port A, pipe and transcoder A, DPLL 0 expected, AUX A. */
	c->port = 0;
	c->pipe = 0;
	c->cpu_transcoder = 0;
	c->dpll_id = 0;
	c->aux_ch = 0;

	/* intel_ddi_init(): the port reversal and four-lane bits of the readout at connector init. */
	c->saved_port_bits = edp->ddi_buf_ctl_readout & I915_LCD_SAVED_PORT_BITS;

	/* The sink's capabilities from the resident eDP. */
	kern_memcpy(c->dpcd, edp->res.dpcd, sizeof(c->dpcd));
	kern_memcpy(c->edp_dpcd, edp->res.edp_dpcd, sizeof(c->edp_dpcd));

	/* The panel's VBT eDP and backlight blocks. */
	c->vbt_low_vswing = pn->edp_low_vswing;
	c->vbt_hobl = pn->edp_hobl;
	c->vbt_override_afc_startup = pn->override_afc_startup;
	c->vbt_backlight_present = pn->bl_present;
	c->vbt_backlight_active_low = pn->bl_active_low_pwm;
	c->vbt_backlight_controller = pn->bl_controller;
	c->vbt_backlight_pwm_freq_hz = pn->bl_pwm_freq_hz;
	c->vbt_backlight_min_brightness = pn->bl_min_brightness;
	k->vbt_min = pn->bl_min_brightness;
	c->rawclk_khz = edp->cfg.rawclk_khz;

	/* The watermark latencies and the SAGV block time the PCODE readout left. */
	for (i = 0U; i < 8U && i < I915_NOGEM_MAX_WM_LVL; i++)
		c->wm_latency[i] = d->nogem->wm_skl_latency[i];

	c->wm_num_levels = (uint8_t)d->nogem->wm_num_levels;
	c->wm_ipc_enabled = d->ipc_enabled;
	c->sagv_block_time_us = (uint8_t)d->nogem->sagv_block_time_us;

	/*
	 * The DBUF geometry of this display (intel_display_device.c): XE_LPD
	 * has 4096 blocks over four slices, Tiger Lake 2048 over two.
	 */
	c->dbuf_size = I915_LCD_DBUF_XELPD_BLOCKS;
	if (d->dcore->dbuf_slice_mask == I915_LCD_DBUF_TGL_SLICES)
		c->dbuf_size = I915_LCD_DBUF_TGL_BLOCKS;

	c->dbuf_slice_mask = I915_LCD_DBUF_XELPD_SLICES;
	if (d->dcore->dbuf_slice_mask != 0U)
		c->dbuf_slice_mask = d->dcore->dbuf_slice_mask;

	/* The DBUF slices enabled now, and whether MBUS is joined. */
	c->dbuf_enabled_slices = d->dcore->dbuf_enabled_slices;
	mbus = drv_i915_read32(d->mmio, drv_i915_lcd_reg_by_name(display->lcd_world, "MBUS_CTL"));
	c->mbus_joined = (mbus >> 31) & 1U;

	/* The CDCLK state the init left, and intel_update_max_cdclk() for display version 11+. */
	c->cdclk_khz = d->cdclk->hw.cdclk;
	c->cdclk_vco_khz = d->cdclk->hw.vco;
	c->cdclk_ref_khz = d->cdclk->hw.ref;
	c->cdclk_bypass_khz = d->cdclk->hw.bypass;
	c->cdclk_voltage_level = d->cdclk->hw.voltage_level;
	c->cdclk_max_khz = drv_i915_max_cdclk_freq(d->cdclk);

	/* SAGV: the forced disable left exactly one QGV point unmasked; its derated bandwidth for one active plane. */
	allowed = (unsigned)~d->dstate->bw_obj_state.qgv_points_mask & ((1U << d->bw->max[0].num_qgv_points) - 1U);
	if (allowed != 0U && (allowed & (allowed - 1U)) == 0U) {
		/* Finds the one allowed point. */
		point = 0U;
		while ((allowed & (1U << point)) == 0U)
			point++;

		c->qgv_allowed_bw = drv_i915_icl_qgv_bw(d->bw, 13, 1, (int)point);
	}

	/* The DMC firmware ids whose payload is loaded. */
	for (i = 0U; i < I915_DMC_FW_MAX; i++) {
		if (d->dmc->dmc.dmc_info[i].payload != NULL && d->dmc->dmc.dmc_info[i].present)
			c->dmc_fw_mask |= 1U << i;
	}

	/* Logs every input and where it came from. */
	kern_logf("i915: LCD-B input: VBT low_vswing=%d hobl=%d afc_override=%d backlight present=%d active_low=%d controller=%d pwm=%u Hz min=%u | rawclk=%u kHz | DDI_BUF_CTL at connector init 0x%08x -> saved bits 0x%x\n",
	    c->vbt_low_vswing,
	    c->vbt_hobl,
	    c->vbt_override_afc_startup,
	    c->vbt_backlight_present,
	    c->vbt_backlight_active_low,
	    c->vbt_backlight_controller,
	    c->vbt_backlight_pwm_freq_hz,
	    c->vbt_backlight_min_brightness,
	    c->rawclk_khz,
	    edp->ddi_buf_ctl_readout,
	    c->saved_port_bits);
	kern_logf("i915: LCD-B input: wm levels=%u latency %u/%u/%u/%u/%u/%u/%u/%u us ipc=%d sagv_block=%u us | DBUF slices now 0x%x MBUS_CTL=0x%08x joined=%d | CDCLK now %u kHz vco %u ref %u bypass %u level %u max %u | QGV mask 0x%x -> allowed 0x%x bw %u MB/s | DMC fw mask 0x%x\n",
	    c->wm_num_levels,
	    c->wm_latency[0],
	    c->wm_latency[1],
	    c->wm_latency[2],
	    c->wm_latency[3],
	    c->wm_latency[4],
	    c->wm_latency[5],
	    c->wm_latency[6],
	    c->wm_latency[7],
	    c->wm_ipc_enabled,
	    c->sagv_block_time_us,
	    c->dbuf_enabled_slices,
	    mbus,
	    c->mbus_joined,
	    c->cdclk_khz,
	    c->cdclk_vco_khz,
	    c->cdclk_ref_khz,
	    c->cdclk_bypass_khz,
	    c->cdclk_voltage_level,
	    c->cdclk_max_khz,
	    (unsigned)d->dstate->bw_obj_state.qgv_points_mask,
	    allowed,
	    c->qgv_allowed_bw,
	    c->dmc_fw_mask);

	/* Succeeded: the configuration is complete. */
	return 0;
}

/*
 * Reacts to a stage of a panel run: the cleanup phase, the registers at
 * picture-up and after the disable.
 */
void
drv_i915_lcd_kernel_at_stage(
	void *ctx,
	int stage)
{
	struct i915_lcd_kernel *k;
	struct i915_display *display;

	k = ctx;
	display = i915_kernel_display(ctx);

	/* Reacts to the stages the run log names. */
	switch (stage) {
	case I915_LCD_SHOW_ENABLE_RETURNED:
		/* Whatever the reference reports from here on belongs to the way down: counted apart. */
		k->phase_cleanup = 1;
		break;
	case I915_LCD_SHOW_PICTURE_UP:
		/* The registers next to Linux's dump, and the start of the window. */
		drv_i915_lcd_log_regs(display->lcd_world, k, "picture-up", 1);
		kern_logf("i915: LCD-B PICTURE UP: pattern id=%u on the panel; observation window %u ms starts now (take the photograph)\n",
		    k->pattern_id,
		    k->window_ms);
		break;
	case I915_LCD_SHOW_WINDOW_DONE:
		kern_logf("i915: LCD-B observation window over; stopping through the reference's disable path\n");
		break;
	case I915_LCD_SHOW_DISABLE_RETURNED:
		drv_i915_lcd_log_regs(display->lcd_world, k, "after-disable", 0);
		break;
	default:
		break;
	}
}

/* Returns the modeset object of the world's selected screen. */
static struct i915_lcd_modeset *
i915_modeset_selected_screen(
	struct i915_lcd_world *world)
{
	/* The selected screen is the index into the pool. */
	return &world->ms_pool[world->ms_sel];
}

/*
 * Binds the selected screen's objects to the Linux text: its device, its
 * watermark state and its encoder (the text reaches them through the
 * world).
 */
static void
i915_modeset_bind_current(
	struct i915_display *display)
{
	struct i915_lcd_world *world;
	struct i915_lcd_modeset *ms;

	world = display->lcd_world;
	ms = i915_modeset_selected_screen(world);

	/* A screen that is not prepared has nothing to bind. */
	if (!ms->prepared)
		return;

	/* The device, the watermark state and the encoder of the selected screen. */
	world->i915_lcd_cur_i915 = &ms->i915;
	display->wm_world->i915_lcd_wm = &ms->wm;
	drv_i915_lcd_ms_bind_encoder(ms);
}

/* Counts an error of the Linux text for the selected screen and hands it to the backend (ctx: the world). */
static void
i915_modeset_on_error(
	void *ctx,
	const char *what)
{
	struct i915_lcd_world *world;
	struct i915_lcd_emit *ms_ops;

	world = ctx;

	/* The first error is what a failed run is read from. */
	if (world->ms_errors_pool[world->ms_sel] == 0U)
		world->ms_first_error_pool[world->ms_sel] = what;

	world->ms_errors_pool[world->ms_sel]++;

	/* Hands it to the backend's error hook. */
	ms_ops = world->ms_ops_pool[world->ms_sel];
	if (ms_ops != NULL && ms_ops->error != NULL)
		ms_ops->error(ms_ops->ctx, what);
}

/* Reads the sink's link status and judges clock recovery and channel equalisation. */
static void
i915_modeset_read_link_status(
	struct i915_lcd_modeset *ms,
	struct i915_lcd_modeset_status *out)
{
	u8 ls[DP_LINK_STATUS_SIZE];
	bool ok;

	/* Reads DPCD 0x202 onwards of the DPRX. */
	kern_memset(ls, 0, sizeof(ls));
	out->link_status_rc = drv_i915_drm_dp_dpcd_read_phy_link_status(&ms->dig_port.dp.aux, DP_PHY_DPRX, ls);
	kern_memcpy(out->link_status, ls, sizeof(out->link_status));

	/* Judges the status only when it was read. */
	out->cr_ok = 0;
	out->eq_ok = 0;
	if (out->link_status_rc != 0)
		return;

	/* Clock recovery on every lane. */
	ok = drv_i915_drm_dp_clock_recovery_ok(ls, ms->crtc_state.lane_count);
	if (ok)
		out->cr_ok = 1;

	/* Channel equalisation, symbol lock and inter-lane alignment. */
	ok = drv_i915_drm_dp_channel_eq_ok(ls, ms->crtc_state.lane_count);
	if (ok)
		out->eq_ok = 1;
}

/* Tells the selected screen's backend that a named point of the commit was reached. */
static void
i915_modeset_observe(
	struct i915_lcd_world *world,
	int point)
{
	struct i915_lcd_emit *ms_ops;

	/* A backend without the hook observes nothing. */
	ms_ops = world->ms_ops_pool[world->ms_sel];
	if (ms_ops == NULL || ms_ops->observe == NULL)
		return;

	ms_ops->observe(ms_ops->ctx, point);
}

/* Reads the surface the selected screen's primary plane is scanning out now. */
static uint32_t
i915_modeset_live_surf(
	struct i915_lcd_world *world)
{
	struct i915_lcd_modeset *ms;
	struct i915_lcd_emit *ms_ops;
	uint32_t live;

	ms = i915_modeset_selected_screen(world);
	ms_ops = world->ms_ops_pool[world->ms_sel];

	/* PLANE_SURFLIVE of the primary plane. */
	live = ms_ops->read32(ms_ops->ctx, i915_mmio_reg_offset(PLANE_SURFLIVE(ms->crtc.pipe, PLANE_PRIMARY)));

	/* Reports the live surface. */
	return live;
}

/*
 * Refuses or arms one flip of the selected screen: the first half of
 * drv_i915_lcd_modeset_flip(), up to the armed event.
 *
 * Refuses a screen that is not running, retained, busy with a flip or
 * stuck, the same buffer, an unaligned address, or a backend without the
 * vblank hooks (I915_LCD_MS_NOT_PREPARED, nothing written).  Otherwise the
 * plane is updated inside the vblank evasion, the event is armed and holds a
 * vblank reference, DC_OFF is held (its reference in `dc_off`, for the
 * caller to drop) and I915_LCD_MS_OK is returned; `res` has the details so
 * far either way.
 */
static int
i915_modeset_flip_arm(
	struct i915_display *display,
	uint32_t new_surf,
	struct i915_lcd_flip_result *res,
	int *dc_off)
{
	struct i915_lcd_world *world;
	struct i915_lcd_modeset *ms;
	struct i915_lcd_emit *ms_ops;
	unsigned before;
	int prepare_error;
	int retained;
	int refused;

	world = display->lcd_world;
	ms = i915_modeset_selected_screen(world);
	ms_ops = world->ms_ops_pool[world->ms_sel];

	/* Nothing written yet: the flip is refused until it is not. */
	kern_memset(res, 0, sizeof(*res));
	res->result = I915_LCD_FLIP_REFUSED;
	res->old_surf = ms->cur_surf;
	res->new_surf = new_surf;

	/*
	 * Refuses a screen that is not running, retained, busy with a flip or
	 * stuck, the same buffer, an unaligned address, or a backend without
	 * the vblank hooks.
	 */
	retained = drv_i915_lcd_modeset_retained(display);
	refused = 0;
	if (!ms->prepared || !ms->crtc.active || !ms->plane_armed) {
		refused = 1;
	} else if (retained || ms->flip_pending || ms->flip_stuck) {
		refused = 1;
	} else if (new_surf == ms->cur_surf || (new_surf & 0xfffU) != 0U) {
		refused = 1;
	} else if (ms_ops->vblank_get == NULL || ms_ops->wait_event == NULL) {
		refused = 1;
	}

	if (refused)
		return I915_LCD_MS_NOT_PREPARED;

	/* The flip's generation and the pipe as it is now. */
	world->i915_lcd_cur_i915 = &ms->i915;
	ms->flip_gen++;
	res->gen = ms->flip_gen;
	res->live_before = i915_modeset_live_surf(world);
	res->frame_before = i915_modeset_frame_now(world);

	/* The new plane state: the same layout, another surface; a refusal restores the current one. */
	prepare_error = drv_i915_lcd_ms_plane_prepare(ms, ms->fb_fourcc, ms->fb_modifier, ms->fb_width, ms->fb_height, ms->fb_pitch, new_surf);
	if (prepare_error != 0) {
		(void)drv_i915_lcd_ms_plane_prepare(ms, ms->fb_fourcc, ms->fb_modifier, ms->fb_width, ms->fb_height, ms->fb_pitch, ms->cur_surf);
		return I915_LCD_MS_NOT_PREPARED;
	}

	/* From here both buffers may be read by the display until the completion is known. */
	ms->old_surf = ms->cur_surf;
	ms->pend_surf = new_surf;
	ms->flip_pending = 1;
	before = world->ms_errors_pool[world->ms_sel];

	/* intel_atomic_commit_tail(): DC states off around every commit, fastsets and other updates too. */
	*dc_off = i915_lcd_intel_display_power_get(&ms->i915, POWER_DOMAIN_DC_OFF);
	drv_i915_lcd_ms_plane_update_flip(ms);

	/* intel_pipe_update_end(): the armed event holds a vblank reference (drm_crtc_vblank_get()). */
	ms->flip_event_ref = 1;
	res->update_errors = (int)(world->ms_errors_pool[world->ms_sel] - before);

	/* Succeeded: the flip is armed. */
	return I915_LCD_MS_OK;
}

/*
 * Completes an armed flip the pipe has latched without waiting for its
 * event: the event is given up (drm_crtc_vblank_off() on it), its vblank
 * reference goes back, and the new buffer is the current one.
 */
static void
i915_modeset_flip_retire(
	struct i915_display *display)
{
	struct i915_lcd_world *world;
	struct i915_lcd_modeset *ms;
	struct i915_lcd_emit *ms_ops;

	world = display->lcd_world;
	ms = i915_modeset_selected_screen(world);
	ms_ops = world->ms_ops_pool[world->ms_sel];

	/* The event will not be waited for; its reference goes back. */
	if (ms_ops->cancel_event != NULL)
		ms_ops->cancel_event(ms_ops->ctx, ms->crtc.pipe);
	ms_ops->vblank_put(ms_ops->ctx, ms->crtc.pipe);
	ms->flip_event_ref = 0;

	/* The new buffer is displayed; the old one is free. */
	ms->cur_surf = ms->pend_surf;
	ms->flip_pending = 0;
}

/* Reads the selected screen's hardware frame counter. */
static uint32_t
i915_modeset_frame_now(
	struct i915_lcd_world *world)
{
	struct i915_lcd_modeset *ms;
	struct i915_lcd_emit *ms_ops;
	uint32_t frame;

	ms = i915_modeset_selected_screen(world);
	ms_ops = world->ms_ops_pool[world->ms_sel];

	/* PIPE_FRMCOUNT_G4X of the pipe. */
	frame = ms_ops->read32(ms_ops->ctx, i915_mmio_reg_offset(PIPE_FRMCOUNT_G4X(ms->crtc.pipe)));

	/* Reports the frame count. */
	return frame;
}

/*
 * Clears the selected screen's object and builds the state an atomic
 * check would have computed, then runs the check phase (watermarks, DDB,
 * CDCLK, bandwidth).  Nothing is written.
 */
static int
i915_modeset_prepare_state(
	struct i915_display *display,
	const struct i915_lcd_state *s,
	const struct i915_lcd_modeset_cfg *cfg,
	struct i915_lcd_emit *ops)
{
	struct i915_lcd_world *world;
	struct i915_lcd_modeset *ms;
	struct drm_display_mode *mode;
	struct intel_dpll_hw_state want;
	int dbuf_known;
	int plane_error;

	world = display->lcd_world;
	ms = i915_modeset_selected_screen(world);

	/* Clears the object; it belongs to this world (the flip's interrupt nesting and the device bookkeeping live there). */
	kern_memset(ms, 0, sizeof(*ms));
	ms->world = world;
	ms->output_hdmi = cfg->output_hdmi;
	ms->hdmi_level_shift = cfg->vbt_hdmi_level_shift;
	ms->also_active_pipes = cfg->also_active_pipes;

	/* The backend of the screen's commits, and a clean error count bound to this world. */
	world->ms_ops_pool[world->ms_sel] = ops;
	world->ms_errors_pool[world->ms_sel] = 0U;
	world->ms_first_error_pool[world->ms_sel] = NULL;
	drv_i915_lcd_error_bind(world, i915_modeset_on_error, world);

	/* The device: its hooks, its locks and what the normal initialisation found. */
	ms->i915.emit = ops;
	ms->i915.display.dpll.lock.which = I915_LCD_LOCK_DPLL;
	ms->i915.display.backlight.lock.which = I915_LCD_LOCK_BACKLIGHT;
	ms->i915.display.vbt.override_afc_startup = cfg->vbt_override_afc_startup != 0;
	ms->i915.display.dmc.fw_mask = cfg->dmc_fw_mask;
	kern_memcpy(ms->i915.display.wm.skl_latency, cfg->wm_latency, sizeof(ms->i915.display.wm.skl_latency));
	ms->i915.display.wm.num_levels = cfg->wm_num_levels;
	ms->i915.display.wm.ipc_enabled = cfg->wm_ipc_enabled != 0;
	ms->i915.display.sagv.block_time_us = cfg->sagv_block_time_us;
	ms->i915.display.device_info.dbuf.size = cfg->dbuf_size;
	ms->i915.display.device_info.dbuf.slice_mask = cfg->dbuf_slice_mask;
	ms->i915.display.runtime.pipe_mask = 0x0f;

	/* The global DBUF state as it is now: the device's own, or what the caller read from the hardware. */
	dbuf_known = drv_i915_lcd_dbuf_current(display->wm_world, &ms->wm.old_dbuf);
	if (dbuf_known != 0) {
		ms->wm.old_dbuf.enabled_slices = cfg->dbuf_enabled_slices;
		ms->wm.old_dbuf.joined_mbus = cfg->mbus_joined != 0;
	}

	/* The CDCLK state the initialisation left, and the platform limit. */
	ms->i915.display.device_info.has_ddi = true;
	ms->i915.display.cdclk.hw.cdclk = cfg->cdclk_khz;
	ms->i915.display.cdclk.hw.vco = cfg->cdclk_vco_khz;
	ms->i915.display.cdclk.hw.ref = cfg->cdclk_ref_khz;
	ms->i915.display.cdclk.hw.bypass = cfg->cdclk_bypass_khz;
	ms->i915.display.cdclk.hw.voltage_level = cfg->cdclk_voltage_level;
	ms->i915.display.cdclk.max_cdclk_freq = cfg->cdclk_max_khz;

	/* The crtc. */
	ms->crtc.base.dev = &ms->i915.drm;
	ms->crtc.base.name = "pipe";
	ms->crtc.pipe = (enum pipe)cfg->pipe;

	/* The crtc state intel_dp_compute_config() / intel_crtc_compute_config() leave for an SST panel. */
	ms->crtc_state.uapi.crtc = &ms->crtc.base;
	ms->crtc_state.uapi.mode_changed = true;
	ms->crtc_state.cpu_transcoder = cfg->cpu_transcoder;
	ms->crtc_state.master_transcoder = INVALID_TRANSCODER;
	ms->crtc_state.mst_master_transcoder = INVALID_TRANSCODER;
	ms->crtc_state.hsw_workaround_pipe = INVALID_PIPE;

	/* The adjusted mode: the panel's timing. */
	mode = &ms->crtc_state.hw.adjusted_mode;
	mode->clock = s->mode.clock_khz;
	mode->hdisplay = s->mode.hdisplay;
	mode->hsync_start = s->mode.hsync_start;
	mode->hsync_end = s->mode.hsync_end;
	mode->htotal = s->mode.htotal;
	mode->vdisplay = s->mode.vdisplay;
	mode->vsync_start = s->mode.vsync_start;
	mode->vsync_end = s->mode.vsync_end;
	mode->vtotal = s->mode.vtotal;

	/* The sync polarities. */
	mode->flags = 0;
	if (s->mode.hsync_positive) {
		mode->flags |= DRM_MODE_FLAG_PHSYNC;
	} else {
		mode->flags |= DRM_MODE_FLAG_NHSYNC;
	}

	if (s->mode.vsync_positive) {
		mode->flags |= DRM_MODE_FLAG_PVSYNC;
	} else {
		mode->flags |= DRM_MODE_FLAG_NVSYNC;
	}

	/* The crtc timing derived from the mode. */
	drv_i915_drm_mode_set_crtcinfo(mode, 0);
	ms->crtc_state.pixel_rate = mode->crtc_clock;
	ms->crtc_state.pipe_src.x2 = (int)cfg->fb_width;
	ms->crtc_state.pipe_src.y2 = (int)cfg->fb_height;

	/* The output: the eDP panel or an HDMI sink, RGB. */
	if (cfg->output_hdmi) {
		ms->crtc_state.output_types = BIT(INTEL_OUTPUT_HDMI);
	} else {
		ms->crtc_state.output_types = BIT(INTEL_OUTPUT_EDP);
	}

	ms->crtc_state.output_format = INTEL_OUTPUT_FORMAT_RGB;
	ms->crtc_state.port_clock = s->link.rate_khz;
	ms->crtc_state.lane_count = s->link.lanes;
	ms->crtc_state.pipe_bpp = s->link.bpp;

	/*
	 * intel_modeset_pipe_config(): "Dithering seems to not pass-through bits
	 * correctly when it should, so only enable it on 6bpc panels";
	 * dither_force_disable is set by DP compliance tests only.
	 */
	ms->crtc_state.dither = ms->crtc_state.pipe_bpp == 6 * 3;
	ms->crtc_state.pixel_multiplier = 1;
	ms->crtc_state.framestart_delay = 1;

	/* Enhanced framing when the DP sink offers it. */
	ms->crtc_state.enhanced_framing = false;
	if (!cfg->output_hdmi && (cfg->dpcd[2] & I915_LCD_DPCD_ENHANCED_FRAME_CAP) != 0U)
		ms->crtc_state.enhanced_framing = true;

	/*
	 * What intel_hdmi_compute_config() leaves for a sink whose EDID could
	 * not be read: DVI mode (no HDMI sink, no infoframes, no scrambling),
	 * RGB 8 bpc, four lanes, the TMDS clock as the port clock.  ADAPTATION:
	 * the reference derives has_hdmi_sink, bpc and colorimetry from the
	 * EDID; without one it would not light the sink at all.
	 */
	if (cfg->output_hdmi) {
		ms->crtc_state.has_hdmi_sink = false;
		ms->crtc_state.has_infoframe = false;
		ms->crtc_state.hdmi_scrambling = false;
		ms->crtc_state.hdmi_high_tmds_clock_ratio = false;
		ms->crtc_state.limited_color_range = false;
		ms->crtc_state.lane_count = 4;
		ms->crtc_state.dither = false;
	}

	/* The link M/N, the primary plane and the one encoder (drm_encoder_mask() of it). */
	ms->crtc_state.dp_m_n.tu = s->link.tu;
	ms->crtc_state.dp_m_n.data_m = s->link.data_m;
	ms->crtc_state.dp_m_n.data_n = s->link.data_n;
	ms->crtc_state.dp_m_n.link_m = s->link.link_m;
	ms->crtc_state.dp_m_n.link_n = s->link.link_n;
	ms->crtc_state.active_planes = (u8)BIT(PLANE_PRIMARY);
	ms->crtc_state.uapi.encoder_mask = 1U << 0;

	/* The encoder, digital port and DP object as intel_ddi_init() leaves them for a combo-PHY port. */
	ms->dig_port.base.base.dev = &ms->i915.drm;
	ms->dig_port.base.base.name = "DDI";
	ms->dig_port.base.port = (enum port)cfg->port;
	if (cfg->output_hdmi) {
		ms->dig_port.base.type = INTEL_OUTPUT_DDI;
	} else {
		ms->dig_port.base.type = INTEL_OUTPUT_EDP;
	}

	/* An HDMI port: its connector, no DP++ adaptor (a step), and the infoframe hook. */
	if (cfg->output_hdmi) {
		ms->dig_port.hdmi.attached_connector = &ms->connector;
		ms->dig_port.hdmi.dp_dual_mode.type = DRM_DP_DUAL_MODE_NONE;
		ms->dig_port.set_infoframes = drv_i915_lcd_hdmi_set_infoframes();
	}

	/* The port bits, the AUX channel, four lanes and the port's I/O domain (d13_port_domains[]: ports A to C). */
	ms->dig_port.saved_port_bits = cfg->saved_port_bits;
	ms->dig_port.aux_ch = cfg->aux_ch;
	ms->dig_port.max_lanes = 4;
	ms->dig_port.ddi_io_power_domain = POWER_DOMAIN_PORT_DDI_IO_A + cfg->port;

	/* The DP object: its connector, the sink's capabilities and the AUX name. */
	ms->dig_port.dp.attached_connector = &ms->connector;
	kern_memcpy(ms->dig_port.dp.dpcd, cfg->dpcd, sizeof(ms->dig_port.dp.dpcd));
	kern_memcpy(ms->dig_port.dp.edp_dpcd, cfg->edp_dpcd, sizeof(ms->dig_port.dp.edp_dpcd));
	ms->dig_port.dp.aux.name = "AUX";

	/* The connector and the panel's VBT data. */
	if (cfg->output_hdmi) {
		ms->connector.base.name = "HDMI";
	} else {
		ms->connector.base.name = "eDP";
	}

	ms->connector.panel.vbt.edp.low_vswing = cfg->vbt_low_vswing != 0;
	ms->connector.panel.vbt.edp.hobl = cfg->vbt_hobl != 0;
	ms->connector.base.dev = &ms->i915.drm;
	ms->connector.panel.vbt.backlight.present = cfg->vbt_backlight_present != 0;
	ms->connector.panel.vbt.backlight.active_low_pwm = cfg->vbt_backlight_active_low != 0;
	ms->connector.panel.vbt.backlight.controller = (s8)cfg->vbt_backlight_controller;
	ms->connector.panel.vbt.backlight.pwm_freq_hz = cfg->vbt_backlight_pwm_freq_hz;
	ms->connector.panel.vbt.backlight.min_brightness = cfg->vbt_backlight_min_brightness;
	ms->i915.display.runtime.rawclk_freq = cfg->rawclk_khz;

	/* The connector state: the connector drives this crtc through this encoder. */
	ms->conn_state.colorspace = MODE_COLORIMETRY_DEFAULT;
	ms->conn_state.connector = &ms->connector;
	ms->conn_state.best_encoder = &ms->dig_port.base;
	ms->conn_state.crtc = &ms->crtc.base;

	/*
	 * The PLL state icl_calc_dpll_state() computed, and the object that
	 * carries it: the reference's rule over the device's pool, not a fixed
	 * id (cfg->dpll_id is only the caller's expectation).
	 */
	kern_memset(&want, 0, sizeof(want));
	want.cfgcr0 = s->pll.cfgcr0;
	want.cfgcr1 = s->pll.cfgcr1;
	want.div0 = s->pll.div0;
	ms->dpll_id = drv_i915_lcd_ms_alloc_pll(world, ms, &want);
	if (ms->dpll_id < 0) {
		i915_modeset_on_error(world, "no shared DPLL is free for this pipe (both are used by other pipes with other states)\n");
		return EBUSY;
	}

	/* Binds the encoder's hooks and checks the colour state. */
	drv_i915_lcd_ms_bind_encoder(ms);
	drv_i915_lcd_ms_color_check(ms);

	/* The framebuffer and the plane state that shows it. */
	ms->fb_fourcc = cfg->fb_fourcc;
	ms->fb_modifier = cfg->fb_modifier;
	ms->fb_width = cfg->fb_width;
	ms->fb_height = cfg->fb_height;
	ms->fb_pitch = cfg->fb_pitch;
	ms->cur_surf = cfg->fb_surf;
	plane_error = drv_i915_lcd_ms_plane_prepare(ms, cfg->fb_fourcc, cfg->fb_modifier, cfg->fb_width, cfg->fb_height, cfg->fb_pitch, cfg->fb_surf);
	if (plane_error != 0)
		return plane_error;

	/* The check phase needs the watermark latencies and the DBUF geometry. */
	if (cfg->wm_num_levels == 0U || cfg->wm_num_levels > 8U)
		return EINVAL;
	if (cfg->dbuf_size == 0U || cfg->dbuf_slice_mask == 0U)
		return EINVAL;

	/* The new state is active, and the plane visible; the cursor is never shown but has DDB. */
	ms->crtc_state.hw.active = true;
	ms->crtc_state.hw.enable = true;
	ms->crtc_state.hw.pipe_mode = ms->crtc_state.hw.adjusted_mode;
	ms->plane_state.uapi.visible = true;
	ms->cursor.base.dev = &ms->i915.drm;
	ms->cursor.id = PLANE_CURSOR;
	ms->cursor.pipe = ms->crtc.pipe;
	ms->crtc.base.cursor = &ms->cursor.base;

	/* The check phase: watermarks and DDB for the new state (nothing is written). */
	world->i915_lcd_cur_i915 = &ms->i915;
	ms->wm_rc = drv_i915_lcd_ms_wm_compute(display->wm_world, ms);
	if (ms->wm_rc != 0)
		return ms->wm_rc;

	/* The check phase, continued: the CDCLK this state requires. */
	drv_i915_ddi_compute_min_voltage_level(&ms->crtc_state);
	drv_i915_lcd_ms_plane_min_cdclk(ms);
	ms->cdclk_rc = drv_i915_lcd_ms_cdclk_check(display->takeover_world, ms);
	if (ms->cdclk_rc != 0)
		return ms->cdclk_rc;

	/* The reference would reprogram CDCLK around the planes (intel_set_cdclk_pre/post_plane_update): not connected. */
	if (ms->cdclk.change_needed) {
		i915_modeset_on_error(world, "the mode requires a CDCLK state other than the current one: CDCLK programming is not connected\n");
		return EINVAL;
	}

	/* The memory bandwidth of the allowed QGV point must cover the plane. */
	ms->bw_data_rate = drv_i915_lcd_ms_bw_data_rate(ms);
	if (cfg->qgv_allowed_bw == 0U || ms->bw_data_rate > cfg->qgv_allowed_bw) {
		i915_modeset_on_error(world, "the memory bandwidth of the allowed QGV point is unknown or below what the plane needs\n");
		return EINVAL;
	}

	/* Succeeded: the state is built and checked. */
	return 0;
}

/* Records the first anomaly of a run; later ones are its consequences. */
static void
i915_show_anomaly(
	struct i915_lcd_show_report *r,
	const char *what,
	int rc)
{
	/* The first one is the finding. */
	if (r->first_anomaly != NULL)
		return;

	r->first_anomaly = what;
	r->first_anomaly_stage = r->stage;
	r->first_anomaly_rc = rc;
}

/* Records a stage the run reached and tells the caller's stage hook. */
static void
i915_show_reached(
	struct i915_lcd_show_env *env,
	struct i915_lcd_show_report *r,
	int stage)
{
	/* The furthest stage so far. */
	r->stage = stage;

	/* The caller logs registers or injects faults here. */
	if (env->at_stage != NULL)
		env->at_stage(env->at_stage_ctx, stage);
}

/* Finds the first error entry of the run log at or after an index; -1 when none. */
static int
i915_show_first_error_after(
	const struct i915_lcd_trace *trace,
	unsigned from)
{
	unsigned index;

	/* Scans the entries from the index on. */
	for (index = from; index < trace->n; index++) {
		if (trace->e[index].kind == I915_LCD_T_ERROR)
			return (int)index;
	}

	/* No error was recorded there. */
	return -1;
}

/*
 * The display part of a run: the check phase, the enable commit, the
 * observation window, the disable commit, and the decision whether the
 * display has let go of the buffer.  Never creates, fills, unpins or
 * destroys the buffer.  Returns 1 when the buffer was handed to the display
 * (the enable was attempted), 0 when it never was.
 */
static int
i915_show_display(
	struct i915_display *display,
	struct i915_lcd_show_env *env,
	struct i915_scanout *so,
	struct i915_lcd_show_report *r,
	uint32_t (*verify)(void *ctx, const struct i915_scanout *so),
	void *verify_ctx)
{
	struct i915_lcd_trace *trace;
	unsigned disable_from;
	unsigned waited;
	uint32_t frame0;

	trace = &display->show_trace;
	r->surf = so->surf;

	/* The check phase: the framebuffer is the one the buffer describes. */
	env->cfg.fb_fourcc = so->format;
	env->cfg.fb_modifier = so->modifier;
	env->cfg.fb_width = so->width;
	env->cfg.fb_height = so->height;
	env->cfg.fb_pitch = so->pitch;
	env->cfg.fb_surf = (uint32_t)so->surf;

	/* The run log stacks on the backend, and the observer samples at every commit point. */
	drv_i915_lcd_trace_init(trace, env->hw);
	drv_i915_lcd_observer_init(&r->obs, env->hw, env->pipe);
	trace->tap = drv_i915_lcd_observer_point;
	trace->tap_ctx = &r->obs;

	/* Builds and checks the state. */
	r->prepare_rc = drv_i915_lcd_modeset_prepare(display, env->lcd, &env->cfg, &trace->ops);
	drv_i915_lcd_modeset_status(display, &r->at_enable);
	if (r->prepare_rc != 0) {
		if (r->at_enable.first_error != NULL) {
			i915_show_anomaly(r, r->at_enable.first_error, r->prepare_rc);
		} else {
			i915_show_anomaly(r, "modeset prepare refused the state", r->prepare_rc);
		}

		return 0;
	}

	i915_show_reached(env, r, I915_LCD_SHOW_PREPARED);

	/* Commit 1: from here on the display may read the buffer. */
	r->begin_rc = drv_i915_scanout_begin(so);
	if (r->begin_rc != 0) {
		i915_show_anomaly(r, "scanout begin", r->begin_rc);
		return 0;
	}

	r->display_acquired = 1;
	drv_i915_lcd_trace_phase(trace, "commit: enable");
	r->enable_rc = drv_i915_lcd_modeset_commit_enable(display);
	drv_i915_lcd_modeset_status(display, &r->at_enable);
	drv_i915_lcd_modeset_link_status(display, &r->at_enable);
	r->enable_errors = r->at_enable.errors;
	r->first_error_trace_at = trace->first_error_at;
	if (r->enable_rc != I915_LCD_MS_OK) {
		if (r->at_enable.first_error != NULL) {
			i915_show_anomaly(r, r->at_enable.first_error, r->enable_rc);
		} else {
			i915_show_anomaly(r, "the enable commit did not succeed", r->enable_rc);
		}
	}

	i915_show_reached(env, r, I915_LCD_SHOW_ENABLE_RETURNED);

	/* "The arm was written" is a software fact; whether the timing runs is asked of the frame counter. */
	if (r->enable_rc == I915_LCD_MS_OK) {
		r->first_frames_rc = drv_i915_lcd_observer_frames(&r->obs, env->first_frames_ms, I915_LCD_FIRST_FRAMES, &r->frame_first, &r->frame_last);
		if (r->first_frames_rc != 0)
			i915_show_anomaly(r, "the pipe's frame counter does not advance after the enable", r->first_frames_rc);
	}

	/* The picture is up: the in-window test, then the finite window. */
	if (r->enable_rc == I915_LCD_MS_OK && r->first_frames_rc == 0) {
		drv_i915_lcd_observer_steady_begin(&r->obs);
		i915_show_reached(env, r, I915_LCD_SHOW_PICTURE_UP);

		/* A test that runs while the picture is up. */
		if (env->in_window != NULL) {
			r->window_hook_rc = env->in_window(env->in_window_ctx, &r->obs);
			if (r->window_hook_rc != 0)
				i915_show_anomaly(r, "the in-window test failed while the picture was up", r->window_hook_rc);
		}

		/* The finite window: every round wants 30 more frames (about half a second at 60 Hz) within 1.5 s. */
		r->steady_frame_first = r->frame_last;
		for (waited = 0U; r->window_hook_rc == 0 && waited < env->window_ms; waited += I915_LCD_STEADY_STEP_MS) {
			frame0 = 0U;
			r->steady_rc = drv_i915_lcd_observer_frames(&r->obs, I915_LCD_STEADY_ROUND_MS, I915_LCD_STEADY_FRAMES, &frame0, &r->steady_frame_last);
			drv_i915_lcd_observer_steady_sample(&r->obs);
			r->steady_rounds++;
			if (r->steady_rc != 0) {
				i915_show_anomaly(r, "the frame counter stopped advancing while the picture was up", r->steady_rc);
				break;
			}
		}

		/* The state at the end of the window. */
		drv_i915_lcd_observer_steady_end(&r->obs);
		drv_i915_lcd_modeset_status(display, &r->at_window_end);
		drv_i915_lcd_modeset_link_status(display, &r->at_window_end);

		/* A DP sink that lost the link while the picture was up. */
		if (!r->output_hdmi && r->steady_rc == 0) {
			if (!r->at_window_end.cr_ok || !r->at_window_end.eq_ok)
				i915_show_anomaly(r, "the sink lost the link while the picture was up", EIO);
		}

		/* An underrun while the picture stood. */
		if (r->obs.seen_steady != 0U)
			i915_show_anomaly(r, "underrun status during the steady picture", EIO);

		i915_show_reached(env, r, I915_LCD_SHOW_WINDOW_DONE);
	}

	/* Commit 2: the reference's stop path, whatever happened above. */
	disable_from = trace->n;
	drv_i915_lcd_trace_phase(trace, "commit: disable");
	r->disable_rc = drv_i915_lcd_modeset_commit_disable(display);
	drv_i915_lcd_modeset_status(display, &r->at_disable);
	r->cleanup_errors = r->at_disable.errors - r->enable_errors;
	r->cleanup_first_error_trace_at = i915_show_first_error_after(trace, disable_from);
	if (r->disable_rc != I915_LCD_MS_OK)
		i915_show_anomaly(r, "the disable commit did not succeed", r->disable_rc);

	i915_show_reached(env, r, I915_LCD_SHOW_DISABLE_RETURNED);

	/* Has the display let go of the buffer?  Only when the hardware says the pipe stands. */
	r->stopped_rc = EBUSY;
	if (r->disable_rc == I915_LCD_MS_OK) {
		r->stopped_rc = drv_i915_lcd_observer_stopped(&r->obs, I915_LCD_STOP_WATCH_MS, &r->stop_frame_first, &r->stop_frame_last);

		/* Read at the pipe-disabled point, the well still on. */
		r->transconf_after_stop = r->obs.stop_transconf;
		if (r->stopped_rc != 0)
			i915_show_anomaly(r, "after the disable the pipe still reports activity", r->stopped_rc);
	}

	/* The stop is confirmed: the buffer is the caller's again. */
	if (r->disable_rc == I915_LCD_MS_OK && r->stopped_rc == 0) {
		i915_show_reached(env, r, I915_LCD_SHOW_STOP_CONFIRMED);
		drv_i915_lcd_modeset_plane_released(display);
		drv_i915_scanout_end(so);
		r->display_released = 1;
		if (verify != NULL)
			r->readback_bad_after = verify(verify_ctx, so);
	} else {
		/*
		 * Not confirmed: the backing pages, their DMA mapping, the GGTT
		 * entries, the pin and its owner stay for ever; the modeset object
		 * keeps what it holds (DC_OFF, power domains, DBUF).
		 */
		drv_i915_scanout_abandon(so);
		drv_i915_lcd_modeset_abandoned(display);
		display->show_retained = 1;
		display->show_retained_hw = env->hw;
		r->abandoned = 1;
		i915_show_reached(env, r, I915_LCD_SHOW_ABANDONED);
	}

	/* The buffer was handed to the display. */
	return 1;
}

/* Judges a run: no anomaly, the picture up for the window, the link trained, no underrun, nothing unresolved. */
static int
i915_show_passed(
	struct i915_display *display,
	const struct i915_lcd_show_report *r,
	int released)
{
	/* The first anomaly, the in-window test and the release decide first. */
	if (r->first_anomaly != NULL)
		return 0;
	if (r->window_hook_rc != 0)
		return 0;
	if (!released)
		return 0;

	/* The enable and the window. */
	if (r->enable_rc != I915_LCD_MS_OK)
		return 0;
	if (r->steady_rc != 0)
		return 0;

	/* A DP sink must have reported a trained link after the enable. */
	if (!r->output_hdmi) {
		if (!r->at_enable.cr_ok || !r->at_enable.eq_ok)
			return 0;
	}

	/* No underrun in the steady picture, and the vblank interrupt never unmasked. */
	if (r->obs.seen_steady != 0U)
		return 0;
	if (r->obs.vblank_unmasked_seen != 0)
		return 0;

	/* The pixels after the stop, and a complete run log without unresolved steps. */
	if (r->readback_bad_after != 0U)
		return 0;
	if (display->show_trace.steps != 0U)
		return 0;
	if (display->show_trace.dropped != 0U)
		return 0;

	/* Passed. */
	return 1;
}

/* Starts a run's report, refusing when an earlier run's resources are retained. */
static int
i915_show_begin(
	struct i915_display *display,
	struct i915_lcd_show_env *env,
	struct i915_lcd_show_report *r)
{
	int retained;

	/* An earlier run left resources the display or the GPU may still use: nothing is touched. */
	retained = drv_i915_lcd_show_retained(display);
	if (retained)
		return EBUSY;

	retained = drv_i915_lcd_modeset_retained(display);
	if (retained)
		return EBUSY;

	/* A fresh report of this run over the display's run log. */
	kern_memset(r, 0, sizeof(*r));
	r->output_hdmi = env->cfg.output_hdmi;
	r->first_error_trace_at = -1;
	r->cleanup_first_error_trace_at = -1;
	r->trace = &display->show_trace;

	/* Succeeded: the run may start. */
	return 0;
}

/* Counts the pixels of the buffer that differ from the environment's test picture. */
static uint32_t
i915_show_pattern_verify(
	void *ctx,
	const struct i915_scanout *so)
{
	const struct i915_lcd_show_env *env;
	uint32_t bad;

	env = ctx;

	/* Compares the whole buffer with the picture. */
	bad = drv_i915_lcd_pattern_verify(so->cpu, so->pitch, so->width, so->height, env->pattern_id, NULL, NULL);

	/* Reports the wrong pixels. */
	return bad;
}

/* Releases a buffer the display never saw, in the ordinary order. */
static void
i915_show_release_unused(
	struct i915_scanout *so,
	struct i915_lcd_show_report *r)
{
	/* Unpins, then destroys only what was unpinned. */
	r->unpin_rc = drv_i915_scanout_unpin(so);
	r->destroy_rc = EBUSY;
	if (r->unpin_rc == 0)
		r->destroy_rc = drv_i915_scanout_destroy(so);

	/* Released when both succeeded. */
	r->released = 0;
	if (r->unpin_rc == 0 && r->destroy_rc == 0)
		r->released = 1;
}

/* Returns the display a panel run belongs to (ctx: its struct i915_lcd_kernel). */
static struct i915_display *
i915_kernel_display(
	void *ctx)
{
	struct i915_lcd_kernel *k;

	/* The run is the display's lk member. */
	k = ctx;

	/* Reports the owner. */
	return container_of(k, struct i915_display, lk);
}

/*
 * The register read hook of a panel run.  While the register trace is on
 * every access is printed; the evasion probe records the first scanline
 * the update body reads after its trigger.
 */
static uint32_t
i915_kernel_read32(
	void *ctx,
	uint32_t reg)
{
	struct i915_lcd_kernel *k;
	struct i915_display *display;
	uint32_t value;

	k = ctx;
	display = i915_kernel_display(ctx);

	/* Reads the register. */
	value = drv_i915_read32(k->d->mmio, reg);

	/* The register trace prints every access (a diagnostic switch). */
	if (display->i915_lcd_reg_trace)
		kern_logf("i915: N1 read 0x%05x = 0x%08x\n", reg, value);

	/* The evasion probe (observation only): the first scanline read after the trigger. */
	if (k->probe_watch && reg == I915_LCD_PIPEDSL_A) {
		k->probe_first_dsl = value & I915_LCD_PIPEDSL_MASK;
		k->probe_watch = 0;
	}

	/* Reports the value. */
	return value;
}

/* The register write hook of a panel run. */
static void
i915_kernel_write32(
	void *ctx,
	uint32_t reg,
	uint32_t value)
{
	struct i915_lcd_kernel *k;
	struct i915_display *display;

	k = ctx;
	display = i915_kernel_display(ctx);

	/* The register trace prints every access (a diagnostic switch). */
	if (display->i915_lcd_reg_trace)
		kern_logf("i915: N1 write 0x%05x = 0x%08x\n", reg, value);

	/* Writes the register. */
	drv_i915_write32(k->d->mmio, reg, value);
}

/* The read-modify-write hook of a panel run: returns the old value. */
static uint32_t
i915_kernel_rmw32(
	void *ctx,
	uint32_t reg,
	uint32_t clear,
	uint32_t set)
{
	struct i915_lcd_kernel *k;
	uint32_t old;

	k = ctx;

	/* Reads, clears, sets and writes back. */
	old = drv_i915_read32(k->d->mmio, reg);
	drv_i915_write32(k->d->mmio, reg, (old & ~clear) | set);

	/* Reports the value before the write. */
	return old;
}

/* The posting-read hook of a panel run. */
static void
i915_kernel_posting_read(
	void *ctx,
	uint32_t reg)
{
	struct i915_lcd_kernel *k;

	k = ctx;

	/* Reads the register and drops the value. */
	(void)drv_i915_read32(k->d->mmio, reg);
}

/*
 * The register wait hook of a panel run (intel_de_wait_for_register()):
 * 2 us of polling without sleeping, then the sleeping stage up to
 * timeout_ms.  Returns 0, the Linux ETIMEDOUT, or the Linux EIO for a time
 * base or wait primitive fault, which is also the run's anomaly.
 */
static int
i915_kernel_wait_reg(
	void *ctx,
	uint32_t reg,
	uint32_t mask,
	uint32_t value,
	unsigned timeout_ms)
{
	struct i915_lcd_kernel *k;
	uint32_t last;
	int error;

	k = ctx;
	last = 0U;

	/* Polls, then sleeps, until the masked register holds the value. */
	error = drv_i915_wait_reg(k->d->mmio, reg, mask, value, 2U, timeout_ms, &last);
	if (error == 0)
		return 0;

	/* The device did not reach the condition: the reference's timeout. */
	if (error == ETIMEDOUT) {
		k->wait_timeouts++;
		return I915_LCD_ETIMEDOUT;
	}

	/* Anything else is the time source failing: not a timeout, and the run's first anomaly. */
	k->time_faults++;
	drv_i915_lcd_backend_fault("time base / wait primitive fault in a register wait (not a timeout)\n");
	return I915_LCD_EIO;
}

/* The sleep hook of a panel run: the resident eDP's tick sleep. */
static void
i915_kernel_usleep(
	void *ctx,
	unsigned us)
{
	struct i915_lcd_kernel *k;
	unsigned before;

	k = ctx;

	/* Sleeps on the eDP's kernel backend and watches its time-fault count. */
	before = k->d->edp->k.time_faults;
	drv_i915_dp_kernel_sleep_us(&k->d->edp->k, us);

	/* A sleep that returned early or found the clock failed is the run's anomaly. */
	if (k->d->edp->k.time_faults != before) {
		k->time_faults++;
		drv_i915_lcd_backend_fault("time base fault during a sleep (returned early or the clock failed)\n");
	}
}

/* The short-delay hook of a panel run. */
static void
i915_kernel_udelay(
	void *ctx,
	unsigned us)
{
	struct i915_lcd_kernel *k;
	int error;

	k = ctx;

	/* Spins for the delay; a failed time base is the run's anomaly. */
	error = drv_i915_udelay(us);
	if (error != 0) {
		k->time_faults++;
		drv_i915_lcd_backend_fault("time base fault during a short delay\n");
	}
}

/* The DBUF hook of a panel run: requests exactly these slices through the display core. */
static void
i915_kernel_dbuf_slices_update(
	void *ctx,
	unsigned req_slices)
{
	struct i915_lcd_kernel *k;

	k = ctx;

	/* The same body the power-domain init uses. */
	drv_i915_gen9_dbuf_slices_update(k->d->dcore, (uint8_t)req_slices);
}

/* The lock hook of a panel run: takes or gives back the dpll or backlight mutex. */
static void
i915_kernel_lock(
	void *ctx,
	int which,
	int take)
{
	struct i915_lcd_kernel *k;
	struct i915_display *display;

	k = ctx;
	display = i915_kernel_display(ctx);

	/* Only the two modeset locks exist. */
	if (which < 0 || which > 1)
		return;

	/* The register trace names every lock step (a diagnostic switch). */
	if (display->i915_lcd_reg_trace) {
		if (take) {
			kern_logf("i915: N1 lock %d take\n", which);
		} else {
			kern_logf("i915: N1 lock %d give back\n", which);
		}
	}

	/* Takes or releases the mutex. */
	if (take) {
		mutex_lock(&k->locks[which]);
	} else {
		mutex_unlock(&k->locks[which]);
	}
}

/* The local_irq_disable() hook of the update section: remembers whether interrupts were on. */
static void
i915_kernel_irq_off(
	void *ctx)
{
	struct i915_lcd_kernel *k;

	k = ctx;

	/* Turns the CPU's interrupts off and keeps the state they were in. */
	k->irq_was_on = kern_irq_disable();
}

/* The local_irq_enable() hook of the update section: turns interrupts back on if they were. */
static void
i915_kernel_irq_on(
	void *ctx)
{
	struct i915_lcd_kernel *k;

	k = ctx;

	/* Interrupts that were off before the section stay off. */
	if (k->irq_was_on)
		kern_irq_enable();
}

/* Checks that pipe B and DDI B are idle for an HDMI run. */
static int
i915_kernel_preflight_hdmi(
	struct i915_lcd_kernel *k)
{
	const struct i915_lcd_kernel_deps *d;
	uint32_t transconf;
	uint32_t buf_ctl;

	d = k->d;

	/* Reads TRANSCONF(B) and DDI_BUF_CTL(B). */
	transconf = drv_i915_read32(d->mmio, I915_LCD_TRANSCONF_B);
	buf_ctl = drv_i915_read32(d->mmio, I915_LCD_DDI_BUF_CTL_B);

	/* The scanout buffer needs the GT memory's GGTT. */
	if (!d->gm->inited) {
		kern_logf("i915: HDMI-B preflight: no GT memory (GGTT) for the scanout buffer\n");
		return EBUSY;
	}

	/* Logs the pipe, the port, the south display status and both PLLs. */
	kern_logf("i915: HDMI-B preflight: TRANSCONF(B)=0x%08x DDI_BUF_CTL(B)=0x%08x SDEISR=0x%08x DPLL0=0x%08x DPLL1=0x%08x\n",
	    transconf,
	    buf_ctl,
	    drv_i915_read32(d->mmio, I915_LCD_SDEISR),
	    drv_i915_read32(d->mmio, I915_LCD_DPLL0_ENABLE),
	    drv_i915_read32(d->mmio, I915_LCD_DPLL1_ENABLE));

	/* Pipe B and DDI B must be off. */
	if ((transconf & I915_LCD_ENABLE_BIT) != 0U || (buf_ctl & I915_LCD_ENABLE_BIT) != 0U) {
		kern_logf("i915: HDMI-B preflight: pipe B / DDI B are not idle\n");
		return EBUSY;
	}

	/* Succeeded: the HDMI output may be written. */
	return 0;
}

/*
 * The observation window of the resident run: the serve loop runs until
 * the display is to be given back, then the panel goes back to buffer A,
 * the one the show body began with and will end.
 */
static int
i915_resident_window(
	void *ctx,
	struct i915_lcd_observer *o)
{
	struct i915_lcd_kernel *k;
	struct i915_display *display;
	int flip_error;

	UNUSED_PARAMETER(o);

	k = ctx;
	display = i915_kernel_display(ctx);

	/* The buffers are the GPU's to draw into from here on. */
	display->resident_up = 1;
	kern_logf("i915: resident display: picture up (buffer A surf 0x%08x); serving presentation\n", (uint32_t)display->resident_buf[0].surf);

	/* Serves until the display is to be given back. */
	(void)display->resident_serve(display->resident_serve_ctx);

	/* A flip armed without waiting completes before the buffers change hands (it logs a failure). */
	(void)drv_i915_lcd_modeset_flip_settle(display);

	/* Ends on buffer A; the flip back is only armed, so it is settled too. */
	if (display->resident_front != 0U) {
		flip_error = drv_i915_lcd_resident_flip(display);
		if (flip_error != 0)
			kern_logf("i915: resident display: XXX could not flip back to buffer A before the stop\n");
		(void)drv_i915_lcd_modeset_flip_settle(display);
	}

	/* The buffers are not the GPU's any more. */
	display->resident_up = 0;
	kern_logf("i915: resident display: released after %u flip(s); the reference stop path follows\n", k->flips_done);

	/* The window passed. */
	return 0;
}

/* The pictures are the application's: nothing to compare them with after the stop. */
static uint32_t
i915_resident_verify(
	void *ctx,
	const struct i915_scanout *so)
{
	UNUSED_PARAMETER(ctx);
	UNUSED_PARAMETER(so);

	/* No wrong pixels can be told. */
	return 0U;
}

/*
 * Creates and pins both resident buffers at the panel's size, black
 * outside the application's image, and publishes them.  A failure unpins
 * and destroys whatever was made.
 */
static int
i915_resident_buffers(
	struct i915_display *display,
	const struct i915_lcd_kernel_deps *d)
{
	struct i915_scanout *a;
	struct i915_scanout *b;
	int window_error;
	int error;
	int i;

	a = &display->resident_buf[0];
	b = &display->resident_buf[1];

	/* The display window of the GGTT; EBUSY means it was claimed earlier, which is fine. */
	window_error = drv_i915_gt_display_window_init(d->gm, I915_GT_DISPLAY_PAGES);
	if (window_error != 0 && window_error != EBUSY)
		return window_error;

	/* Creates and pins A at the panel's size, then B at A's size. */
	error = drv_i915_scanout_create(d->gm, d->edp->lcd.mode.hdisplay, d->edp->lcd.mode.vdisplay, I915_FOURCC_XRGB8888, I915_MOD_LINEAR, a);
	if (error == 0)
		error = drv_i915_scanout_pin(a, "resident A");
	if (error == 0)
		error = drv_i915_scanout_create(d->gm, a->width, a->height, I915_FOURCC_XRGB8888, I915_MOD_LINEAR, b);
	if (error == 0)
		error = drv_i915_scanout_pin(b, "resident B");

	/* Gives back whatever was made, B first. */
	if (error != 0) {
		kern_logf("i915: resident display: not started (buffers rc=%d)\n", error);
		for (i = 1; i >= 0; i--) {
			(void)drv_i915_scanout_unpin(&display->resident_buf[i]);
			(void)drv_i915_scanout_destroy(&display->resident_buf[i]);
		}

		return error;
	}

	/* Black outside the application's image, visible to the display. */
	for (i = 0; i < 2; i++) {
		kern_memset(display->resident_buf[i].cpu, 0, display->resident_buf[i].size);
		drv_i915_scanout_publish(&display->resident_buf[i]);
	}

	/* Succeeded: both buffers are pinned and black. */
	return 0;
}

/*
 * Releases both resident buffers when the display provably reads neither,
 * or abandons them when it may.  Reports whether both were released.
 */
static int
i915_resident_release(
	struct i915_display *display,
	const struct i915_lcd_show_report *rep)
{
	struct i915_scanout *so;
	int error;
	int i;

	/* The display may still read the buffers: both are kept for ever. */
	if (!rep->display_released && rep->display_acquired) {
		for (i = 0; i < 2; i++) {
			so = &display->resident_buf[i];
			if (so->state >= I915_SCANOUT_PINNED && so->state != I915_SCANOUT_ABANDONED)
				drv_i915_scanout_abandon(so);
		}

		return 0;
	}

	/* The show body ended A; B's last use ends here. */
	drv_i915_scanout_end(&display->resident_buf[1]);

	/* Unpins and destroys A then B, stopping at the first refusal. */
	error = drv_i915_scanout_unpin(&display->resident_buf[0]);
	if (error == 0)
		error = drv_i915_scanout_destroy(&display->resident_buf[0]);
	if (error == 0)
		error = drv_i915_scanout_unpin(&display->resident_buf[1]);
	if (error == 0)
		error = drv_i915_scanout_destroy(&display->resident_buf[1]);
	if (error != 0)
		return 0;

	/* Succeeded: both buffers are gone. */
	return 1;
}

/*
 * Judges the resident run: the show body's judgement without the complete
 * run log (a display that flips for as long as an application presents
 * always outgrows it); the counters keep counting past the last kept entry.
 */
static int
i915_resident_passed(
	const struct i915_lcd_show_report *rep)
{
	/* No anomaly, the window and the release. */
	if (rep->first_anomaly != NULL)
		return 0;
	if (rep->window_hook_rc != 0)
		return 0;
	if (!rep->display_released)
		return 0;

	/* The enable, the first frames and the trained link. */
	if (rep->enable_rc != I915_LCD_MS_OK)
		return 0;
	if (rep->steady_rc != 0)
		return 0;
	if (!rep->at_enable.cr_ok || !rep->at_enable.eq_ok)
		return 0;

	/* No underrun, the vblank interrupt never unmasked, and no wrong pixels. */
	if (rep->obs.seen_steady != 0U)
		return 0;
	if (rep->obs.vblank_unmasked_seen != 0)
		return 0;
	if (rep->readback_bad_after != 0U)
		return 0;

	/* The run log's counters: no unresolved step, no error, no timed-out wait. */
	if (rep->trace->steps != 0U)
		return 0;
	if (rep->trace->errors != 0U)
		return 0;
	if (rep->trace->wait_timeouts != 0U)
		return 0;

	/* Passed. */
	return 1;
}
