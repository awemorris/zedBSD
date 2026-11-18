/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The pictures of a display (see xd-picture.h): a screen prepared from
 * its configuration, its light and dark commits as the steps of the
 * screen's output, its plane, its flips and its backlight, and the PLLs
 * and the display buffer the screens share.
 */

#include "xd-picture.h"
#include "xd-dbuf.h"
#include "xd-ddi.h"
#include "xd-dp.h"
#include "xd-output.h"
#include "xd-pipe.h"
#include "xd-screen.h"
#include "xd-tc.h"

#include "../intel/display-dpcd-xd.h"
#include "../intel/display-regs-xd.h"

#include <kern/kcrt.h>
#include <kern/klog.h>

#include <uapi/errno.h>
#include <stddef.h>
#include <stdint.h>

/* How long a waited flip waits for its completion event, in milliseconds. */
#define I915_XD_PICTURE_FLIP_EVENT_MS	100U

/*
 * How long after a commit or a flip the display may enter DC states
 * again, in milliseconds: longer than a frame, so an armed flip latches
 * first and a flip every frame does not switch DC states each time.
 *
 * Value referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_display.c
 * (intel_atomic_commit_tail: the delay of the DC_OFF put).
 */
#define I915_XD_PICTURE_DC_OFF_DELAY_MS	17

/* The negative error the backlight's setup reports when it found no PWM frequency. */
#define I915_XD_PICTURE_NO_BACKLIGHT	(-19)

/*
 * The DPCD byte that says whether a sink is a branch device, and its bit.
 *
 * Values referenced: Linux v6.8.12 include/drm/display/drm_dp.h
 * (DP_DOWNSTREAMPORT_PRESENT, DP_DWN_STRM_PORT_PRESENT).
 */
#define I915_XD_PICTURE_DPCD_PORTS	0x005U
#define I915_XD_PICTURE_DPCD_BRANCH	0x01U

/* The DPCD revision from which a branch device has a protocol converter to set up. */
#define I915_XD_PICTURE_DPCD_REV_1_3	0x13U

/* The link's hop whose status a light checks: the sink itself, behind any repeaters. */
#define I915_XD_PICTURE_HOP_SINK	0U

/* The pipe depth of a panel that dithers: 6 bits per colour. */
#define I915_XD_PICTURE_DITHER_BPP	18U

/* The widest TMDS clock an HDMI output takes and the widest DisplayPort link rate, in kHz. */
#define I915_XD_PICTURE_HDMI_MAX_KHZ	600000U
#define I915_XD_PICTURE_DP_MAX_KHZ	810000U

/*
 * The commit that runs a screen's light or dark: the screens, the screen
 * and its number, the results of the light's pipe and plane steps and of
 * the dark's pipe step, and the screen's error count where the dark's
 * pipe steps began.  It lives on the stack of the commit.
 */
struct i915_xd_picture_commit {
	struct i915_xd_pictures *pictures;
	struct i915_xd_picture *pic;
	unsigned screen;
	int enable_result;
	int plane_result;
	int crtc_result;
	unsigned before;
};

static struct i915_xd_picture *i915_xd_picture_of(struct i915_xd_pictures *pictures, unsigned screen);
static struct i915_xd_picture *i915_xd_picture_running(struct i915_xd_pictures *pictures, unsigned screen);
static void i915_xd_picture_bind(struct i915_xd_picture *pic);
static uint32_t i915_xd_picture_window_read(void *context, uint32_t offset);
static void i915_xd_picture_window_write(void *context, uint32_t offset, uint32_t value);
static void i915_xd_picture_window_wait(void *context, unsigned microseconds);
static uint32_t i915_xd_picture_dkl_read(void *context, unsigned tc, uint32_t phy_offset);
static void i915_xd_picture_dkl_write(void *context, unsigned tc, uint32_t phy_offset, uint32_t value);
static void i915_xd_picture_dkl_rmw(void *context, unsigned tc, uint32_t phy_offset, uint32_t clear, uint32_t set);
static int i915_xd_picture_check_backend(const struct i915_xd_backend *backend);
static int i915_xd_picture_check_config(const struct i915_xd_picture_config *config);
static int i915_xd_picture_reserve(struct i915_xd_pictures *pictures, struct i915_xd_picture *pic);
static int i915_xd_picture_reserve_tc(struct i915_xd_pictures *pictures, struct i915_xd_picture *pic);
static void i915_xd_picture_unreserve(struct i915_xd_pictures *pictures, unsigned pipe);
static int i915_xd_picture_words_equal(const struct i915_xd_dpll_words *one, const struct i915_xd_dpll_words *other);
static void i915_xd_picture_describe(struct i915_xd_pictures *pictures, struct i915_xd_picture *pic);
static void i915_xd_picture_describe_dbuf(struct i915_xd_picture *pic);
static void i915_xd_picture_plan(struct i915_xd_pictures *pictures, struct i915_xd_picture *pic, struct i915_xd_plan *plan);
static void i915_xd_picture_dbuf_on(struct i915_xd_picture *pic, const struct i915_xd_plan *plan);
static void i915_xd_picture_dbuf_off(struct i915_xd_pictures *pictures, struct i915_xd_picture *pic, uint8_t keep_pipes);
static void i915_xd_picture_publish(struct i915_xd_picture *pic, const struct i915_xd_plan *plan);
static void i915_xd_picture_plane_fill(struct i915_xd_picture *pic, uint32_t surface);
static void i915_xd_picture_plane_wm(struct i915_xd_picture *pic);
static int i915_xd_picture_plane_on(struct i915_xd_picture *pic);
static void i915_xd_picture_plane_flip(struct i915_xd_picture *pic);
static int i915_xd_picture_flip_arm(struct i915_xd_picture *pic, unsigned screen, uint32_t surface, struct i915_xd_picture_flip *result, int *dc_off);
static void i915_xd_picture_flip_retire(struct i915_xd_picture *pic);
static uint32_t i915_xd_picture_live(struct i915_xd_picture *pic);
static int i915_xd_picture_dc_domain(struct i915_xd_picture *pic);
static int i915_xd_picture_dc_held(const struct i915_xd_picture *pic);
static int i915_xd_picture_is_retained(struct i915_xd_pictures *pictures, unsigned screen);
static int i915_xd_picture_may_light(struct i915_xd_pictures *pictures, unsigned screen, struct i915_xd_picture *pic);
static void i915_xd_picture_read_link(struct i915_xd_picture *pic, struct i915_xd_picture_status *status);
static void i915_xd_picture_env(struct i915_xd_picture *pic, struct i915_xd_output_env *env);
static void i915_xd_picture_error(struct i915_xd_picture *pic, const char *what);
static void i915_xd_picture_observe(struct i915_xd_picture *pic, int point);
static void i915_xd_picture_step(struct i915_xd_picture *pic, const char *name);
static void i915_xd_picture_light_hooks(struct i915_xd_screen_hooks *hooks, struct i915_xd_picture_commit *commit);
static void i915_xd_picture_dark_hooks(struct i915_xd_screen_hooks *hooks, struct i915_xd_picture_commit *commit);
static int i915_xd_picture_step_power(void *context);
static int i915_xd_picture_step_dbuf(void *context);
static int i915_xd_picture_step_setup(void *context);
static int i915_xd_picture_step_dmc_on(void *context);
static int i915_xd_picture_step_link_power(void *context);
static int i915_xd_picture_step_pll(void *context);
static int i915_xd_picture_step_port(void *context);
static int i915_xd_picture_step_pipe(void *context);
static int i915_xd_picture_step_output(void *context);
static int i915_xd_picture_step_check(void *context);
static int i915_xd_picture_step_plane(void *context);
static int i915_xd_picture_step_settle(void *context);
static int i915_xd_picture_step_dark_power(void *context);
static int i915_xd_picture_step_plane_off(void *context);
static int i915_xd_picture_step_output_off(void *context);
static int i915_xd_picture_step_port_off(void *context);
static int i915_xd_picture_step_pll_off(void *context);
static int i915_xd_picture_step_link_power_off(void *context);
static int i915_xd_picture_step_dmc_off(void *context);
static int i915_xd_picture_step_confirm(void *context);
static int i915_xd_picture_step_dark_settle(void *context);
static int i915_xd_picture_pll_index(const struct i915_xd_picture *pic);
static void i915_xd_picture_pll_load(struct i915_xd_picture *pic);
static void i915_xd_picture_pll_store(struct i915_xd_picture *pic);
static int i915_xd_picture_power_domain(struct i915_xd_picture *pic, unsigned use, unsigned index);
static int i915_xd_picture_power_get(void *context, unsigned use, unsigned index);
static void i915_xd_picture_power_put(void *context, unsigned use, unsigned index, int cookie);
static void i915_xd_picture_power_put_later(void *context, unsigned use, unsigned index, int cookie, int delay_ms);
static int i915_xd_picture_panel(void *context, unsigned step);
static void i915_xd_picture_dbuf_slices(void *context, uint8_t slices);
static void i915_xd_picture_lock(void *context, unsigned lock, int taken);
static void i915_xd_picture_vblank_off(void *context);
static void i915_xd_picture_link_failed(void *context);
static void i915_xd_picture_converter(void *context);
static void i915_xd_picture_output_observe(void *context, unsigned point);
static void i915_xd_picture_output_error(void *context, const char *text);
static void i915_xd_picture_output_debug(void *context, const char *text);
static int i915_xd_picture_sink_read(void *context, uint32_t address, uint8_t *buffer, unsigned size);
static int i915_xd_picture_sink_write(void *context, uint32_t address, const uint8_t *buffer, unsigned size);
static int i915_xd_picture_sink_probe(void *context, uint32_t address);
static int i915_xd_picture_sink_read_caps(void *context, uint8_t *caps);
static unsigned i915_xd_picture_tc_port(const struct i915_xd_picture *pic);
static void i915_xd_picture_tc_get_link(void *context, unsigned lanes);
static void i915_xd_picture_tc_put_link(void *context);
static unsigned i915_xd_picture_tc_mode(void *context);
static void i915_xd_picture_tc_lanes(void *context, unsigned lanes);
static unsigned i915_xd_picture_tc_pin(void *context);
static int i915_xd_picture_tc_tbt_pll(void *context, int on);
static int i915_xd_picture_update_vblank_get(void *context);
static void i915_xd_picture_update_vblank_put(void *context);
static long i915_xd_picture_update_vblank_sleep(void *context, long ticks);
static void i915_xd_picture_update_irq_off(void *context);
static void i915_xd_picture_update_irq_on(void *context);
static void i915_xd_picture_update_arm(void *context);
static void i915_xd_picture_update_arm_event(void *context);
static void i915_xd_picture_update_error(void *context, const char *what);

/*
 * The Type-C hooks of a screen's output on a Type-C port: the backend's
 * link, mode, lanes and pin assignment of the port, and the device's
 * Thunderbolt PLL the screens share.  Constant.
 */
static const struct i915_xd_output_tc i915_xd_picture_tc = {
	i915_xd_picture_tc_get_link,
	i915_xd_picture_tc_put_link,
	i915_xd_picture_tc_mode,
	i915_xd_picture_tc_lanes,
	i915_xd_picture_tc_pin,
	i915_xd_picture_tc_tbt_pll,
};

/*
 * Prepares the pictures of a display: no screen prepared, no display
 * buffer layout known, every PLL free, the device described by platform.
 */
void
drv_i915_xd_pictures_init(
	struct i915_xd_pictures *pictures,
	const struct i915_xd_platform *platform)
{
	unsigned screen;

	/* Nothing is prepared or held. */
	kern_memset(pictures, 0, sizeof(*pictures));
	pictures->platform = platform;

	/* Each screen knows the pictures it belongs to. */
	for (screen = 0U; screen < I915_XD_PICTURE_SCREENS; screen++)
		pictures->screen[screen].pictures = pictures;
}

/*
 * Sets the description of the device the screens are prepared for (the
 * display chooses it at its start; a later choice, with the south
 * display known, replaces it).
 */
void
drv_i915_xd_pictures_platform(
	struct i915_xd_pictures *pictures,
	const struct i915_xd_platform *platform)
{
	/* The next prepare uses it. */
	pictures->platform = platform;
}

/*
 * Forgets the display buffer layout and the PLLs, as on a fresh device:
 * the next prepare takes the layout its configuration read from the
 * hardware.  The screens themselves are not changed.
 */
void
drv_i915_xd_pictures_forget(
	struct i915_xd_pictures *pictures)
{
	/* No layout is known and no PLL is held any more. */
	kern_memset(&pictures->dbuf, 0, sizeof(pictures->dbuf));
	pictures->dbuf_valid = 0;
	kern_memset(pictures->pll, 0, sizeof(pictures->pll));
}

/*
 * Counts an error the backend found outside a screen's own hooks (a power
 * well that would not turn off, a broken time base) against the screen
 * whose operation runs, so a dark is not confirmed over it.
 */
void
drv_i915_xd_pictures_fault(
	struct i915_xd_pictures *pictures,
	const char *what)
{
	struct i915_xd_picture *pic;

	/* The screen whose operation runs. */
	pic = i915_xd_picture_of(pictures, pictures->current);
	if (pic == NULL)
		return;

	/* Counted as one of its errors. */
	i915_xd_picture_error(pic, what);
}

/*
 * Prepares a screen from its configuration: checks it, reserves the PLL
 * its output runs on, describes the output, the plane and the screen's
 * plan (the link, the watermarks, the display buffer, the CD clock and
 * the memory bandwidth); nothing is written.
 *
 * Returns 0; EINVAL for a configuration this path does not light (checked
 * before anything is touched), a CD clock or a bandwidth it cannot serve;
 * EBUSY when the screen is lit or retained, or no PLL is free; or the
 * plane's or the watermarks' refusal.
 */
int
drv_i915_xd_picture_prepare(
	struct i915_xd_pictures *pictures,
	unsigned screen,
	const struct i915_xd_picture_config *config,
	const struct i915_xd_backend *backend)
{
	struct i915_xd_picture *pic;
	struct i915_xd_plan plan;
	int retained;
	int held;
	int error;

	/* Refuses a screen out of range. */
	pic = i915_xd_picture_of(pictures, screen);
	if (pic == NULL)
		return EINVAL;

	/* Refuses a call without its inputs. */
	if (config == NULL || backend == NULL)
		return EINVAL;

	/* The backend must carry every hook the commits use. */
	error = i915_xd_picture_check_backend(backend);
	if (error != 0)
		return error;

	/* The configuration must be one this path lights. */
	error = i915_xd_picture_check_config(config);
	if (error != 0)
		return error;

	/* A retained screen is never prepared again. */
	retained = i915_xd_picture_is_retained(pictures, screen);
	if (retained)
		return EBUSY;

	/* A screen that is lit, shows a buffer or keeps the DC states off is stopped first. */
	held = i915_xd_picture_dc_held(pic);
	if (pic->prepared) {
		if (pic->out.lit ||
		    pic->plane_armed ||
		    held)
			return EBUSY;
	}

	/* Clears the screen; its errors are counted from here, and the operation is its own. */
	kern_memset(pic, 0, sizeof(*pic));
	pic->pictures = pictures;
	pic->backend = backend;
	pic->sink = config->sink;
	pic->config = *config;
	pictures->current = screen;
	i915_xd_picture_bind(pic);

	/*
	 * The display buffer as it is now: the layout the last commit left,
	 * or what the configuration read from the hardware.
	 */
	if (pictures->dbuf_valid) {
		pic->old_dbuf = pictures->dbuf;
	} else {
		pic->old_dbuf.enabled = config->dbuf_slices_now;
		pic->old_dbuf.joined = 0;
		if (config->mbus_joined_now)
			pic->old_dbuf.joined = 1;
	}

	/* The PLL the output runs on: a Type-C port's two, or a combo PLL. */
	if (config->kind == I915_XD_SCREEN_DP) {
		error = i915_xd_picture_reserve_tc(pictures, pic);
	} else {
		error = i915_xd_picture_reserve(pictures, pic);
	}

	/* No PLL is left for the output. */
	if (error != 0)
		return error;

	/* The output, as the light will drive it. */
	i915_xd_picture_describe(pictures, pic);

	/* The framebuffer must be one the plane scans out. */
	error = drv_i915_xd_plane_check(config->fb_fourcc, config->fb_modifier, config->fb_width, config->fb_height, config->fb_pitch, config->fb_surf);
	if (error != 0)
		return error;

	/* The plane shows it first. */
	pic->flip.current = config->fb_surf;
	i915_xd_picture_plane_fill(pic, config->fb_surf);

	/* The plan needs the memory's watermark latencies. */
	if (config->wm_levels == 0U || config->wm_levels > I915_XD_WM_LEVELS)
		return EINVAL;

	/* The plan needs the display buffer's geometry. */
	if (config->dbuf_size == 0U || config->dbuf_slice_mask == 0U)
		return EINVAL;

	/* The screen's plan: the watermarks, the display buffer, the CD clock and the bandwidth. */
	i915_xd_picture_plan(pictures, pic, &plan);
	pic->plan = plan;

	/* The plan's display buffer becomes the layout the commits write. */
	i915_xd_picture_dbuf_on(pic, &plan);

	/* Watermarks that do not fit refuse the plan. */
	if (plan.refused == I915_XD_PLAN_WM) {
		pic->wm_rc = plan.wm_error;
		return pic->wm_rc;
	}

	/* What the CD clock and the bandwidth need, as the plan found it. */
	i915_xd_picture_publish(pic, &plan);

	/* A need above the highest step, or one no step gives. */
	if (plan.refused == I915_XD_PLAN_CDCLK && plan.cdclk_khz == 0U) {
		pic->cdclk_rc = EINVAL;
		return pic->cdclk_rc;
	}

	/* Another CD clock or voltage level would be reprogrammed around the planes, which this path does not do. */
	if (pic->cdclk_change_needed) {
		i915_xd_picture_error(pic, "xd-picture: the mode needs another CD clock than the running one\n");
		return EINVAL;
	}

	/* The memory bandwidth of the allowed point must carry the plane. */
	if (plan.refused == I915_XD_PLAN_QGV) {
		i915_xd_picture_error(pic, "xd-picture: the memory bandwidth allowed is unknown or below what the plane needs\n");
		return EINVAL;
	}

	/* The commits may run. */
	pic->prepared = 1;

	/* Succeeded: the screen is planned. */
	return 0;
}

/*
 * Lights a prepared screen (the light commit): the display kept out of
 * DC states and the pipe's power, the display buffer, the panel's
 * backlight setup, the pipe's DMC program, the link's power, the PLL, the
 * port (a DisplayPort link trained), the pipe, the output started, the
 * link checked, the plane armed, then the commit settled.  After a pipe
 * or link that did not come up the plane is not armed and nothing is
 * undone (the caller's dark stops the pipe).
 *
 * Returns I915_XD_PICTURE_OK, I915_XD_PICTURE_NOT_READY, or the light's
 * result (the pipe's first, then the plane's).
 *
 * Procedure referenced: Linux v6.8.12
 * drivers/gpu/drm/i915/display/intel_display.c (intel_atomic_commit_tail).
 */
int
drv_i915_xd_picture_light(
	struct i915_xd_pictures *pictures,
	unsigned screen)
{
	struct i915_xd_picture_commit commit;
	struct i915_xd_screen_hooks hooks;
	struct i915_xd_picture *pic;
	int may;

	/* A screen out of range is never lit. */
	pic = i915_xd_picture_of(pictures, screen);
	if (pic == NULL)
		return I915_XD_PICTURE_NOT_READY;

	/* Only a prepared screen outside a commit and not retained is committed. */
	may = i915_xd_picture_may_light(pictures, screen, pic);
	if (!may)
		return I915_XD_PICTURE_NOT_READY;

	/* A lit screen is darkened first. */
	if (pic->out.lit)
		return I915_XD_PICTURE_NOT_READY;

	/* The screen's operation; one more commit. */
	pictures->current = screen;
	pic->commits++;

	/* The output as the configuration describes it, and the display buffer around the commit. */
	i915_xd_picture_describe(pictures, pic);
	i915_xd_picture_describe_dbuf(pic);

	/* The commit's steps. */
	kern_memset(&commit, 0, sizeof(commit));
	commit.pictures = pictures;
	commit.pic = pic;
	commit.screen = screen;
	commit.enable_result = I915_XD_PICTURE_OK;
	commit.plane_result = I915_XD_PICTURE_OK;
	i915_xd_picture_light_hooks(&hooks, &commit);
	pic->screen.pipe = pic->config.pipe;
	(void)drv_i915_xd_screen_light(&pic->screen, &pic->window, &hooks, 0);

	/* Reports a pipe or link that did not come up first. */
	if (commit.enable_result != I915_XD_PICTURE_OK)
		return commit.enable_result;

	/* Reports a plane that was not armed. */
	if (commit.plane_result != I915_XD_PICTURE_OK)
		return commit.plane_result;

	/* Succeeded: the picture is armed. */
	return I915_XD_PICTURE_OK;
}

/*
 * Darkens a lit screen (the dark commit): the display kept out of DC
 * states, the plane disabled, the output stopped, the port, the PLL, the
 * link's power and the pipe's DMC program, then the confirmation (an
 * error, the PLL or a port reference still held leave the stop
 * unconfirmed: nothing further is given back), then the display buffer
 * laid out for the pipes that stay, the PLL given back and the DC states
 * let back.
 *
 * Returns I915_XD_PICTURE_OK, I915_XD_PICTURE_NOT_READY, or the dark's
 * result.
 *
 * Procedure referenced: Linux v6.8.12
 * drivers/gpu/drm/i915/display/intel_display.c (intel_atomic_commit_tail),
 * skl_watermark.c (intel_dbuf_pre_plane_update, intel_dbuf_post_plane_update).
 */
int
drv_i915_xd_picture_dark(
	struct i915_xd_pictures *pictures,
	unsigned screen)
{
	struct i915_xd_picture_commit commit;
	struct i915_xd_screen_hooks hooks;
	struct i915_xd_picture *pic;
	struct i915_xd_picture *other;
	unsigned index;
	uint8_t keep_pipes;
	int may;
	int error;

	/* A screen out of range is never darkened. */
	pic = i915_xd_picture_of(pictures, screen);
	if (pic == NULL)
		return I915_XD_PICTURE_NOT_READY;

	/* Only a prepared screen outside a commit and not retained is committed. */
	may = i915_xd_picture_may_light(pictures, screen, pic);
	if (!may)
		return I915_XD_PICTURE_NOT_READY;

	/* A dark screen has nothing to darken. */
	if (!pic->out.lit)
		return I915_XD_PICTURE_NOT_READY;

	/* The screen's operation; one more commit. */
	pictures->current = screen;
	pic->commits++;

	/* The pipes another lit screen lights or left room for stay in the layout. */
	keep_pipes = 0U;
	for (index = 0U; index < I915_XD_PICTURE_SCREENS; index++) {
		other = &pictures->screen[index];

		/* This screen leaves the layout. */
		if (index == screen)
			continue;

		/* A screen that does not run keeps nothing. */
		if (!other->prepared || !other->out.lit)
			continue;

		/* The lit screen's pipe and the pipes it left room for. */
		keep_pipes |= (uint8_t)(1U << other->config.pipe);
		keep_pipes |= other->config.also_pipes;
	}

	/* The layout with only the pipes that stay, which the output takes. */
	i915_xd_picture_dbuf_off(pictures, pic, keep_pipes);
	i915_xd_picture_describe_dbuf(pic);

	/* The commit's steps. */
	kern_memset(&commit, 0, sizeof(commit));
	commit.pictures = pictures;
	commit.pic = pic;
	commit.screen = screen;
	i915_xd_picture_dark_hooks(&hooks, &commit);

	/* Runs them; a step that failed reports why the dark is not confirmed. */
	error = drv_i915_xd_screen_dark(&pic->screen, &pic->window, &hooks);
	if (error != 0)
		return error;

	/* Succeeded: the screen is dark and everything is given back. */
	return I915_XD_PICTURE_OK;
}

/*
 * Disables a screen's plane: its scaler's input, its watermarks, then a
 * zero control word and the arming surface.  The plane is not marked off:
 * the write takes effect at the next vertical blank, and only the caller,
 * who watches the hardware, decides that the buffer is free
 * (drv_i915_xd_picture_released).  Returns I915_XD_PICTURE_OK,
 * I915_XD_PICTURE_NOT_READY or I915_XD_PICTURE_ERRORS.
 */
int
drv_i915_xd_picture_plane_off(
	struct i915_xd_pictures *pictures,
	unsigned screen)
{
	struct i915_xd_picture *pic;

	/* A screen out of range has no plane. */
	pic = i915_xd_picture_of(pictures, screen);
	if (pic == NULL)
		return I915_XD_PICTURE_NOT_READY;

	/* Only a prepared screen has a plane to disable. */
	if (!pic->prepared)
		return I915_XD_PICTURE_NOT_READY;

	/* The screen's operation: the plane's disable. */
	pictures->current = screen;
	drv_i915_xd_plane_off_noarm(&pic->window, pic->config.pipe);
	i915_xd_picture_plane_wm(pic);
	drv_i915_xd_plane_off_arm(&pic->window, pic->config.pipe);

	/* An error on the way fails the disable. */
	if (pic->errors != 0U)
		return I915_XD_PICTURE_ERRORS;

	/* Succeeded: the plane's disable is armed. */
	return I915_XD_PICTURE_OK;
}

/*
 * Records that the caller saw on the hardware (the frame counter, the
 * scan line) that a screen's plane is off: the framebuffer and a flip's
 * buffers are the caller's again.
 */
void
drv_i915_xd_picture_released(
	struct i915_xd_pictures *pictures,
	unsigned screen)
{
	struct i915_xd_picture *pic;

	/* A screen out of range holds nothing. */
	pic = i915_xd_picture_of(pictures, screen);
	if (pic == NULL)
		return;

	/* The display reads neither buffer any more. */
	pic->plane_armed = 0;
	pic->flip.armed = 0;
	pic->flip.stuck = 0;
}

/*
 * Records that the caller kept a screen's resources after a dark that
 * was not confirmed.
 *
 * The screen forgets nothing it holds (the power, the DC states kept
 * off, the display buffer, the PLL), and every later prepare of it is
 * refused before anything is touched.  The retained mark names the
 * backend the hardware belongs to.
 */
void
drv_i915_xd_picture_abandoned(
	struct i915_xd_pictures *pictures,
	unsigned screen)
{
	struct i915_xd_picture *pic;

	/* A screen out of range holds nothing. */
	pic = i915_xd_picture_of(pictures, screen);
	if (pic == NULL)
		return;

	/* The mark outlives the screen's clearing; only a model's discard clears it. */
	pic->stop_unconfirmed = 1;
	pictures->retained[screen] = 1;
	pictures->retained_backend[screen] = pic->backend;
}

/*
 * Reports whether a screen is retained: its last dark was not confirmed,
 * or the caller kept its resources.
 */
int
drv_i915_xd_picture_retained(
	struct i915_xd_pictures *pictures,
	unsigned screen)
{
	int retained;

	/* The screen's marks. */
	retained = i915_xd_picture_is_retained(pictures, screen);
	if (retained)
		return 1;

	/* Succeeded: nothing is retained. */
	return 0;
}

/*
 * Discards a retained screen that belongs to a register model.
 *
 * The only way out of a retained screen, permitted only for the model
 * that holds it, whose discard is itself the isolation; on the hardware
 * nothing is released.  Returns 0, or EPERM for the hardware or another
 * backend.
 */
int
drv_i915_xd_picture_discard_model(
	struct i915_xd_pictures *pictures,
	unsigned screen,
	const struct i915_xd_backend *backend)
{
	const struct i915_xd_backend *holder;
	struct i915_xd_picture *pic;
	int retained;

	/* A screen out of range has nothing to discard. */
	pic = i915_xd_picture_of(pictures, screen);
	if (pic == NULL)
		return 0;

	/* Nor has a screen that retains nothing. */
	retained = i915_xd_picture_is_retained(pictures, screen);
	if (!retained)
		return 0;

	/* The backend the retained hardware belongs to. */
	holder = pictures->retained_backend[screen];
	if (holder == NULL)
		holder = pic->backend;

	/* Only the model that holds it may discard it. */
	if (backend == NULL ||
	    backend != holder ||
	    !backend->model)
		return EPERM;

	/* Forgets the screen and its mark. */
	kern_memset(pic, 0, sizeof(*pic));
	pic->pictures = pictures;
	pictures->retained[screen] = 0;
	pictures->retained_backend[screen] = NULL;

	/* Succeeded: the model's state is gone with it. */
	return 0;
}

/*
 * Reports what a screen holds: how far it got, the link, the PLL and the
 * port's power, the backlight, the plan's watermarks and display buffer,
 * the commit's outer part, the flips and the errors.  The sink's link
 * status is not read (drv_i915_xd_picture_link_status reads it).
 */
void
drv_i915_xd_picture_status(
	struct i915_xd_pictures *pictures,
	unsigned screen,
	struct i915_xd_picture_status *status)
{
	const struct i915_xd_picture_pll *pll;
	struct i915_xd_picture *pic;
	const struct i915_xd_output *out;
	unsigned use;
	int index;

	/* Without a record there is nothing to fill in. */
	if (status == NULL)
		return;

	/* A screen out of range holds nothing. */
	kern_memset(status, 0, sizeof(*status));
	pic = i915_xd_picture_of(pictures, screen);
	if (pic == NULL)
		return;

	/* The screen's output. */
	out = &pic->out;

	/* How far the screen got. */
	status->prepared = pic->prepared;
	status->crtc_active = pic->out.lit;
	status->plane_armed = pic->plane_armed;

	/* The link as the port step set it (an HDMI output has none), and as it was trained. */
	status->link_rate = (int)pic->link_rate;
	status->lane_count = (int)pic->link_lanes;
	status->link_trained_flag = out->trained;
	kern_memcpy(status->train_set, out->set, sizeof(status->train_set));
	status->ddi_buf_ctl_value = out->buf_word;

	/* The PLL the pipe runs on, and the pipes that reserved it. */
	status->pll_id = (int)pic->pll;
	index = i915_xd_picture_pll_index(pic);
	if (index >= 0) {
		pll = &pictures->pll[index];
		status->pll_on = pll->on;
		status->pll_active_mask = pll->pipes;
		status->pll_pipe_mask = pll->reserved;
	}

	/* The cookie of the port's IO power, while it is held. */
	if ((out->held & (1U << I915_XD_OUTPUT_POWER_DDI_IO)) != 0U)
		status->ddi_io_wakeref = out->cookie[I915_XD_OUTPUT_POWER_DDI_IO];

	/* The cookie of the AUX channel's power, while it is held. */
	if ((out->held & (1U << I915_XD_OUTPUT_POWER_AUX)) != 0U)
		status->aux_wakeref = out->cookie[I915_XD_OUTPUT_POWER_AUX];

	/* The backlight as its setup found it. */
	status->backlight_present = out->backlight.present;
	status->backlight_enabled = out->backlight.enabled;
	status->backlight_setup_rc = pic->backlight_setup_rc;
	status->backlight_pwm_max = out->backlight.max;
	status->backlight_level = out->backlight.level;

	/* The watermarks and display buffer the plan gave the plane. */
	status->wm_rc = pic->wm_rc;
	status->ddb_start = pic->plan.plane_ddb.start;
	status->ddb_end = pic->plan.plane_ddb.end;
	status->wm0_enable = pic->plan.wm.level[0].enable;
	status->wm0_blocks = pic->plan.wm.level[0].blocks;
	status->wm0_lines = pic->plan.wm.level[0].lines;
	status->dbuf_slices_wanted = pic->new_dbuf.enabled;
	status->mbus_joined = pic->new_dbuf.joined;

	/* The commit's outer part: the CD clock, the bandwidth and the DC states. */
	status->cdclk_rc = pic->cdclk_rc;
	status->cdclk_crtc_min = pic->cdclk_crtc_min;
	status->cdclk_bw_min = pic->cdclk_bw_min;
	status->cdclk_required_khz = pic->cdclk_khz;
	status->cdclk_required_vco = pic->cdclk_vco;
	status->cdclk_required_level = pic->cdclk_voltage;
	status->cdclk_change_needed = pic->cdclk_change_needed;
	status->bw_data_rate = pic->bw_data_rate;
	status->dc_off_held = i915_xd_picture_dc_held(pic);

	/* Counts the power the pipe holds: the display core, the pipe, the transcoder and the port's lanes. */
	for (use = I915_XD_OUTPUT_POWER_CORE; use <= I915_XD_OUTPUT_POWER_LANES; use++) {
		if ((out->held & (1U << use)) != 0U)
			status->crtc_domains_held++;
	}

	/* The display buffer now, and what a dark left. */
	status->dbuf_slices_now = pic->old_dbuf.enabled;
	status->mbus_joined_now = pic->old_dbuf.joined;
	status->dbuf_active_pipes_now = pic->old_dbuf.pipes;
	status->stop_unconfirmed = pic->stop_unconfirmed;
	status->retained = i915_xd_picture_is_retained(pictures, screen);
	status->dither = pic->out.dither;

	/* The flips. */
	status->cur_surf = pic->flip.current;
	status->pend_surf = pic->flip.pending;
	status->flip_pending = pic->flip.armed;
	status->flip_stuck = pic->flip.stuck;
	status->flip_event_ref = pic->flip_event_ref;
	status->events_cancelled = pic->events_cancelled;
	status->flip_gen = pic->flip.gen;

	/* The backlight's range and the user's brightness. */
	status->backlight_min = out->backlight.min;
	status->backlight_max = out->backlight.max;
	status->backlight_user = pic->user_level;
	status->backlight_user_max = pic->user_max;

	/* The commits and their errors. */
	status->commits = pic->commits;
	status->errors = pic->errors;
	status->first_error = pic->first_error;

	/* The sink's link status has not been read into this record. */
	status->link_status_rc = EINVAL;
}

/*
 * Reads the sink's link status of a prepared screen into a status
 * record, and judges its clock recovery and its equalisation.
 */
void
drv_i915_xd_picture_link_status(
	struct i915_xd_pictures *pictures,
	unsigned screen,
	struct i915_xd_picture_status *status)
{
	struct i915_xd_picture *pic;

	/* A screen out of range, or a call without a record, reads nothing. */
	pic = i915_xd_picture_of(pictures, screen);
	if (pic == NULL || status == NULL)
		return;

	/* Only a prepared screen has a sink to ask. */
	if (!pic->prepared)
		return;

	/* The status, read now. */
	i915_xd_picture_read_link(pic, status);
}

/*
 * Flips a lit screen to another buffer of the same format, size and
 * pitch and waits for it: the plane's words, then the arming writes in
 * one frame, the completion event armed; then the event and the plane's
 * live surface.  The old buffer is free only when the event came and the
 * live surface is the new buffer; otherwise both stay protected and every
 * later flip is refused until the screen is darkened.
 *
 * Returns I915_XD_PICTURE_OK, I915_XD_PICTURE_NOT_READY (nothing written)
 * or I915_XD_PICTURE_ERRORS, with the details in result.
 *
 * Procedure referenced: Linux v6.8.12
 * drivers/gpu/drm/i915/display/intel_crtc.c (intel_pipe_update_start,
 * intel_pipe_update_end), intel_display.c (intel_atomic_commit_tail: the
 * DC_OFF reference around the update).
 */
int
drv_i915_xd_picture_flip(
	struct i915_xd_pictures *pictures,
	unsigned screen,
	uint32_t surface,
	struct i915_xd_picture_flip *result)
{
	const struct i915_xd_backend *backend;
	struct i915_xd_picture_flip flip;
	struct i915_xd_picture *pic;
	unsigned verdict;
	int domain;
	int dc_off;
	int armed;

	/* A screen out of range has nothing to flip. */
	pic = i915_xd_picture_of(pictures, screen);
	if (pic == NULL)
		return I915_XD_PICTURE_NOT_READY;

	/* Arms the flip; a refusal writes nothing. */
	armed = i915_xd_picture_flip_arm(pic, screen, surface, &flip, &dc_off);
	if (armed != I915_XD_PICTURE_OK) {
		if (result != NULL)
			*result = flip;
		return armed;
	}

	/* Waits for the event of the pipe's next vertical blank, then reads what the pipe shows. */
	backend = pic->backend;
	flip.event_rc = backend->wait_event(backend->ctx, (int)pic->config.pipe, I915_XD_PICTURE_FLIP_EVENT_MS);
	flip.live_after = i915_xd_picture_live(pic);
	flip.frame_after = drv_i915_xd_pipe_frame(&pic->window, pic->config.pipe);

	/* The DC states may come back once the flip had its frame. */
	domain = i915_xd_picture_dc_domain(pic);
	backend->power_put_async(backend->ctx, domain, dc_off, I915_XD_PICTURE_DC_OFF_DELAY_MS);

	/* An event that came gives its vertical blank reference back (otherwise it may still come). */
	if (flip.event_rc == 0) {
		backend->vblank_put(backend->ctx, (int)pic->config.pipe);
		pic->flip_event_ref = 0;
	}

	/* The live surface decides whether the old buffer is free. */
	verdict = drv_i915_xd_flip_judge(&pic->flip, flip.event_rc, flip.live_after);
	if (verdict == I915_XD_FLIP_DONE) {
		/* The new buffer latched: the old one is free. */
		flip.result = I915_XD_PICTURE_FLIP_DONE;
	} else if (verdict == I915_XD_FLIP_NOT_LATCHED) {
		/* The event came, but the new buffer is not live. */
		flip.result = I915_XD_PICTURE_FLIP_NOT_LATCHED;
	} else {
		/* No event came. */
		flip.result = I915_XD_PICTURE_FLIP_TIMEOUT;
	}

	/* Reports the details. */
	if (result != NULL)
		*result = flip;

	/* A flip that did not complete keeps both buffers. */
	if (flip.result != I915_XD_PICTURE_FLIP_DONE)
		return I915_XD_PICTURE_ERRORS;

	/* An error during the update fails the flip. */
	if (flip.update_errors != 0)
		return I915_XD_PICTURE_ERRORS;

	/* Succeeded: the new buffer is shown and the old one is free. */
	return I915_XD_PICTURE_OK;
}

/*
 * Flips a lit screen to another buffer without waiting: armed as
 * drv_i915_xd_picture_flip() arms it, latched at the pipe's next vertical
 * blank; until then both buffers stay protected, and a flip still armed
 * refuses the next one (the caller polls first).
 *
 * Returns I915_XD_PICTURE_OK with the result I915_XD_PICTURE_FLIP_ARMED,
 * I915_XD_PICTURE_NOT_READY for a refusal, or I915_XD_PICTURE_ERRORS for
 * an error during the update, with the details in result.
 */
int
drv_i915_xd_picture_flip_nowait(
	struct i915_xd_pictures *pictures,
	unsigned screen,
	uint32_t surface,
	struct i915_xd_picture_flip *result)
{
	const struct i915_xd_backend *backend;
	struct i915_xd_picture_flip flip;
	struct i915_xd_picture *pic;
	int domain;
	int dc_off;
	int armed;

	/* A screen out of range has nothing to flip. */
	pic = i915_xd_picture_of(pictures, screen);
	if (pic == NULL)
		return I915_XD_PICTURE_NOT_READY;

	/* Arms the flip; a refusal writes nothing. */
	armed = i915_xd_picture_flip_arm(pic, screen, surface, &flip, &dc_off);
	if (armed != I915_XD_PICTURE_OK) {
		if (result != NULL)
			*result = flip;
		return armed;
	}

	/*
	 * The DC states may come back later than a frame, so the flip
	 * latches first.
	 */
	backend = pic->backend;
	domain = i915_xd_picture_dc_domain(pic);
	backend->power_put_async(backend->ctx, domain, dc_off, I915_XD_PICTURE_DC_OFF_DELAY_MS);
	flip.result = I915_XD_PICTURE_FLIP_ARMED;

	/* Reports the details. */
	if (result != NULL)
		*result = flip;

	/* An error during the update fails the flip. */
	if (flip.update_errors != 0)
		return I915_XD_PICTURE_ERRORS;

	/* Succeeded: the flip is armed and latches at the next vertical blank. */
	return I915_XD_PICTURE_OK;
}

/*
 * Tells whether a screen has no flip armed, completing an armed flip the
 * pipe has latched (its event is given up, its vertical blank reference
 * goes back and the new buffer is the current one).  Returns 1 when no
 * flip is armed (one that never completes included, which the next flip
 * refuses), 0 while the armed flip waits for its vertical blank.
 */
int
drv_i915_xd_picture_flip_poll(
	struct i915_xd_pictures *pictures,
	unsigned screen)
{
	struct i915_xd_picture *pic;
	uint32_t live;
	int idle;

	/* A screen out of range has nothing armed. */
	pic = i915_xd_picture_of(pictures, screen);
	if (pic == NULL)
		return 1;

	/* Nothing armed, or a flip that will never complete. */
	if (!pic->flip.armed || pic->flip.stuck)
		return 1;

	/* The live surface tells whether the flip latched. */
	live = i915_xd_picture_live(pic);
	idle = drv_i915_xd_flip_idle(&pic->flip, live);
	if (!idle)
		return 0;

	/* The flip latched: its event is given up, and the new buffer is the current one. */
	i915_xd_picture_flip_retire(pic);

	/* Succeeded: no flip is armed any more. */
	return 1;
}

/*
 * Waits until the armed flip of a screen has latched, from a thread that
 * is not the display owner's (the presenting thread, so the owner can
 * render the next frame meanwhile).  Nothing of the screen changes: the
 * owner retires the latched flip when it polls.  A flip that latched
 * already is not waited for.  Returns 0, EINVAL for a screen out of
 * range, or EIO when the flip did not latch within the event's time.
 */
int
drv_i915_xd_picture_flip_wait(
	struct i915_xd_pictures *pictures,
	unsigned screen)
{
	const struct i915_xd_backend *backend;
	struct i915_xd_picture *pic;
	uint32_t live;
	int event_rc;

	/* Refuses a screen out of range. */
	pic = i915_xd_picture_of(pictures, screen);
	if (pic == NULL)
		return EINVAL;

	/* A screen never prepared has nothing to wait for. */
	backend = pic->backend;
	if (backend == NULL)
		return 0;

	/* Nor has one with nothing armed, or with a flip that will never complete. */
	if (!pic->flip.armed || pic->flip.stuck)
		return 0;

	/* A flip that latched already. */
	live = i915_xd_picture_live(pic);
	if (live == pic->flip.pending)
		return 0;

	/* Waits for the vertical blank the flip latches at, then looks again. */
	event_rc = backend->wait_event(backend->ctx, (int)pic->config.pipe, I915_XD_PICTURE_FLIP_EVENT_MS);
	live = i915_xd_picture_live(pic);
	if (live == pic->flip.pending)
		return 0;

	/* The vertical blank came and went without the new buffer, or never came. */
	kern_logf("i915: display screen %u: flip to 0x%08x did not latch: event_rc=%d live 0x%08x\n",
	    screen,
	    pic->flip.pending,
	    event_rc,
	    live);
	return EIO;
}

/*
 * Waits for the armed flip of a screen to complete before its buffers
 * change hands (a dark, or a flip that waits).  Returns 0 when no flip
 * is armed any more, or EIO when the armed flip did not complete or did
 * not latch the new buffer; both buffers then stay protected.
 */
int
drv_i915_xd_picture_flip_settle(
	struct i915_xd_pictures *pictures,
	unsigned screen)
{
	const struct i915_xd_backend *backend;
	struct i915_xd_picture *pic;
	uint32_t live;
	int settled;
	int idle;
	int event_rc;

	/* A flip that latched, or none, needs no wait. */
	idle = drv_i915_xd_picture_flip_poll(pictures, screen);
	if (idle)
		return 0;

	/* Waits for the event of the pipe's next vertical blank, then for the live surface. */
	pic = &pictures->screen[screen];
	backend = pic->backend;
	event_rc = backend->wait_event(backend->ctx, (int)pic->config.pipe, I915_XD_PICTURE_FLIP_EVENT_MS);
	live = i915_xd_picture_live(pic);

	/* An event that did not come, or a surface that did not latch, keeps both buffers. */
	settled = drv_i915_xd_flip_settled(&pic->flip, event_rc, live);
	if (settled != 0) {
		kern_logf("i915: display screen %u: flip to 0x%08x did not complete: event_rc=%d live 0x%08x\n",
		    screen,
		    pic->flip.pending,
		    event_rc,
		    live);
		return EIO;
	}

	/* The event came: its vertical blank reference goes back. */
	backend->vblank_put(backend->ctx, (int)pic->config.pipe);
	pic->flip_event_ref = 0;

	/* Succeeded: no flip is armed. */
	return 0;
}

/*
 * Sets the brightness of a lit screen's panel to level of max, shown at
 * once while the backlight is on.  The backlight device's brightness
 * follows: the level itself when max is the device's range, otherwise
 * the hardware level in the device's range.  Returns I915_XD_PICTURE_OK,
 * I915_XD_PICTURE_NOT_READY (the screen is not lit or retained, or the
 * level is outside a non-empty range) or I915_XD_PICTURE_ERRORS.
 */
int
drv_i915_xd_picture_brightness(
	struct i915_xd_pictures *pictures,
	unsigned screen,
	uint32_t level,
	uint32_t max)
{
	struct i915_xd_output_env env;
	struct i915_xd_picture *pic;
	unsigned before;

	/* Only a lit screen that is not retained takes a brightness. */
	pic = i915_xd_picture_running(pictures, screen);
	if (pic == NULL)
		return I915_XD_PICTURE_NOT_READY;

	/* The level must lie in a range. */
	if (level > max)
		return I915_XD_PICTURE_NOT_READY;

	/* The range must not be empty. */
	if (max == 0U)
		return I915_XD_PICTURE_NOT_READY;

	/* The panel's brightness. */
	before = pic->errors;
	i915_xd_picture_env(pic, &env);
	drv_i915_xd_output_brightness(&pic->out, &env, level, max);

	/* The backlight device's brightness follows. */
	if (max == pic->user_max) {
		pic->user_level = level;
	} else {
		pic->user_level = drv_i915_xd_output_user_level(&pic->out, pic->user_max);
	}

	/* An error on the way. */
	if (pic->errors != before)
		return I915_XD_PICTURE_ERRORS;

	/* Succeeded: the brightness is set. */
	return I915_XD_PICTURE_OK;
}

/*
 * Reports the brightness of a lit screen's panel as a level of max: the
 * hardware level in that range.  Returns I915_XD_PICTURE_OK, or
 * I915_XD_PICTURE_NOT_READY when the screen is not lit, is retained, or
 * the range is empty.
 */
int
drv_i915_xd_picture_brightness_get(
	struct i915_xd_pictures *pictures,
	unsigned screen,
	uint32_t max,
	uint32_t *level)
{
	struct i915_xd_picture *pic;

	/* Only a lit screen that is not retained has a brightness. */
	pic = i915_xd_picture_running(pictures, screen);
	if (pic == NULL)
		return I915_XD_PICTURE_NOT_READY;

	/* The range must not be empty. */
	if (max == 0U)
		return I915_XD_PICTURE_NOT_READY;

	/* The hardware level in the caller's range. */
	*level = drv_i915_xd_output_user_level(&pic->out, max);

	/* Succeeded: the level is reported. */
	return I915_XD_PICTURE_OK;
}

/*
 * Turns the backlight of a lit screen's panel on or off; the pipe keeps
 * scanning out.  An on that raised a level at or below the lowest one
 * moves the backlight device's brightness with it.  Results as
 * drv_i915_xd_picture_brightness().
 */
int
drv_i915_xd_picture_backlight(
	struct i915_xd_pictures *pictures,
	unsigned screen,
	int on)
{
	struct i915_xd_output_env env;
	struct i915_xd_picture *pic;
	uint32_t before_level;
	unsigned before;

	/* Only a lit screen that is not retained is switched. */
	pic = i915_xd_picture_running(pictures, screen);
	if (pic == NULL)
		return I915_XD_PICTURE_NOT_READY;

	/* Switches the backlight, remembering the level it had. */
	before = pic->errors;
	before_level = pic->out.backlight.level;
	i915_xd_picture_env(pic, &env);
	if (on) {
		drv_i915_xd_output_backlight_on(&pic->out, &env);
	} else {
		drv_i915_xd_output_backlight_off(&pic->out, &env);
	}

	/* An on that raised the level moves the device's brightness with it. */
	if (on && pic->out.backlight.level != before_level)
		pic->user_level = drv_i915_xd_output_user_level(&pic->out, pic->user_max);

	/* An error on the way. */
	if (pic->errors != before)
		return I915_XD_PICTURE_ERRORS;

	/* Succeeded: the backlight is switched. */
	return I915_XD_PICTURE_OK;
}

/* Returns a screen of the pictures, or NULL for a number out of range. */
static struct i915_xd_picture *
i915_xd_picture_of(
	struct i915_xd_pictures *pictures,
	unsigned screen)
{
	/* A number out of range names no screen. */
	if (screen >= I915_XD_PICTURE_SCREENS)
		return NULL;

	/* Succeeded: the screen. */
	return &pictures->screen[screen];
}

/*
 * Returns a screen that is lit, outside a commit and not retained (the
 * one a flip's wait, a brightness or a backlight change works on), or
 * NULL.
 */
static struct i915_xd_picture *
i915_xd_picture_running(
	struct i915_xd_pictures *pictures,
	unsigned screen)
{
	struct i915_xd_picture *pic;
	int may;

	/* A number out of range names no screen. */
	pic = i915_xd_picture_of(pictures, screen);
	if (pic == NULL)
		return NULL;

	/* A screen not prepared, inside a commit or retained. */
	may = i915_xd_picture_may_light(pictures, screen, pic);
	if (!may)
		return NULL;

	/* A dark screen. */
	if (!pic->out.lit)
		return NULL;

	/* Succeeded: the screen runs. */
	return pic;
}

/* Points a screen's register window and Dekel route at its backend. */
static void
i915_xd_picture_bind(
	struct i915_xd_picture *pic)
{
	struct i915_xd_io_hooks io_hooks;
	struct i915_xd_dkl_hooks dkl_hooks;

	/* The register window: the backend's registers and its sleeps. */
	drv_i915_xd_io_init(&pic->window, NULL, NULL);
	io_hooks.read = i915_xd_picture_window_read;
	io_hooks.write = i915_xd_picture_window_write;
	io_hooks.wait = i915_xd_picture_window_wait;
	io_hooks.lock = NULL;
	io_hooks.context = pic;
	drv_i915_xd_io_set_hooks(&pic->window, &io_hooks);

	/* The Dekel PHYs: the backend's own route, or the bank index and window through the registers. */
	drv_i915_xd_dkl_init(&pic->dkl, &pic->window);
	dkl_hooks.read = i915_xd_picture_dkl_read;
	dkl_hooks.write = i915_xd_picture_dkl_write;
	dkl_hooks.rmw = i915_xd_picture_dkl_rmw;
	dkl_hooks.context = pic;
	drv_i915_xd_dkl_set_hooks(&pic->dkl, &dkl_hooks);
}

/* Reads a register through the screen's backend; a backend that cannot read answers 0. */
static uint32_t
i915_xd_picture_window_read(
	void *context,
	uint32_t offset)
{
	struct i915_xd_picture *pic;
	uint32_t value;

	/* A backend without a read hook. */
	pic = context;
	if (pic->backend->read32 == NULL)
		return 0U;

	/* The register. */
	value = pic->backend->read32(pic->backend->ctx, offset);

	/* Succeeded: the register's value. */
	return value;
}

/* Writes a register through the screen's backend. */
static void
i915_xd_picture_window_write(
	void *context,
	uint32_t offset,
	uint32_t value)
{
	struct i915_xd_picture *pic;

	/* The register. */
	pic = context;
	pic->backend->write32(pic->backend->ctx, offset, value);
}

/* Lets time pass through the screen's backend, when it keeps time. */
static void
i915_xd_picture_window_wait(
	void *context,
	unsigned microseconds)
{
	struct i915_xd_picture *pic;

	/* A backend without a clock lets no time pass. */
	pic = context;
	if (pic->backend->usleep == NULL)
		return;

	/* Sleeps. */
	pic->backend->usleep(pic->backend->ctx, microseconds);
}

/* Reads a Type-C port's PHY register through the backend's route, or through the bank index and the window. */
static uint32_t
i915_xd_picture_dkl_read(
	void *context,
	unsigned tc,
	uint32_t phy_offset)
{
	const struct i915_xd_backend *backend;
	struct i915_xd_picture *pic;
	uint32_t index_reg;
	uint32_t index_value;
	uint32_t window;
	uint32_t value;

	/* The screen and its backend. */
	pic = context;
	backend = pic->backend;

	/* The backend reads it under the lock the ports' bank index shares. */
	if (backend->dkl_read != NULL) {
		value = backend->dkl_read(backend->ctx, (int)tc, phy_offset);
		return value;
	}

	/* A model is told the bank, then read through the port's window. */
	window = drv_i915_xd_dkl_window(tc, phy_offset, &index_reg, &index_value);
	backend->write32(backend->ctx, index_reg, index_value);
	value = 0U;
	if (backend->read32 != NULL)
		value = backend->read32(backend->ctx, window);

	/* Succeeded: the register's value. */
	return value;
}

/* Writes a Type-C port's PHY register through the backend's route, or through the bank index and the window. */
static void
i915_xd_picture_dkl_write(
	void *context,
	unsigned tc,
	uint32_t phy_offset,
	uint32_t value)
{
	const struct i915_xd_backend *backend;
	struct i915_xd_picture *pic;
	uint32_t index_reg;
	uint32_t index_value;
	uint32_t window;

	/* The screen and its backend. */
	pic = context;
	backend = pic->backend;

	/* The backend writes it under the lock the ports' bank index shares. */
	if (backend->dkl_write != NULL) {
		backend->dkl_write(backend->ctx, (int)tc, phy_offset, value);
		return;
	}

	/* A model is told the bank, then written through the port's window. */
	window = drv_i915_xd_dkl_window(tc, phy_offset, &index_reg, &index_value);
	backend->write32(backend->ctx, index_reg, index_value);
	backend->write32(backend->ctx, window, value);
}

/* Changes bits of a Type-C port's PHY register through the backend's route, or through the bank index and the window. */
static void
i915_xd_picture_dkl_rmw(
	void *context,
	unsigned tc,
	uint32_t phy_offset,
	uint32_t clear,
	uint32_t set)
{
	const struct i915_xd_backend *backend;
	struct i915_xd_picture *pic;
	uint32_t index_reg;
	uint32_t index_value;
	uint32_t window;
	uint32_t value;

	/* The screen and its backend. */
	pic = context;
	backend = pic->backend;

	/* The backend changes it under one hold of the lock the ports' bank index shares. */
	if (backend->dkl_rmw != NULL) {
		backend->dkl_rmw(backend->ctx, (int)tc, phy_offset, clear, set);
		return;
	}

	/* A model is told the bank, then read and written through the port's window. */
	window = drv_i915_xd_dkl_window(tc, phy_offset, &index_reg, &index_value);
	backend->write32(backend->ctx, index_reg, index_value);
	value = 0U;
	if (backend->read32 != NULL)
		value = backend->read32(backend->ctx, window);
	value &= ~clear;
	value |= set;
	backend->write32(backend->ctx, window, value);
}

/* Checks that a backend carries every hook a commit uses: 0, or EINVAL. */
static int
i915_xd_picture_check_backend(
	const struct i915_xd_backend *backend)
{
	/* The registers, the waits and the time. */
	if (backend->write32 == NULL ||
	    backend->rmw32 == NULL ||
	    backend->read32 == NULL ||
	    backend->wait_reg == NULL ||
	    backend->usleep == NULL ||
	    backend->udelay == NULL)
		return EINVAL;

	/* The panel's sink and its power sequencer. */
	if (backend->dpcd_read == NULL ||
	    backend->dpcd_write == NULL ||
	    backend->read_dpcd_caps == NULL ||
	    backend->panel == NULL)
		return EINVAL;

	/* The display power, the display buffer, the locks and the notes. */
	if (backend->power_domain == NULL ||
	    backend->power_get == NULL ||
	    backend->power_put == NULL ||
	    backend->power_put_async == NULL ||
	    backend->dbuf_slices_update == NULL ||
	    backend->lock == NULL ||
	    backend->step == NULL)
		return EINVAL;

	/* Succeeded: the backend is complete. */
	return 0;
}

/* Checks that a configuration is one this path lights: 0, or EINVAL. */
static int
i915_xd_picture_check_config(
	const struct i915_xd_picture_config *config)
{
	/* Pipes and transcoders A to D, and a link with a rate and a depth. */
	if (config->pipe > 3U || config->trans > 3U)
		return EINVAL;
	if (config->rate_khz == 0U || config->bpp == 0U)
		return EINVAL;

	/*
	 * An external DisplayPort sink is on a Type-C port and its own PLL,
	 * over its own AUX channel; the panel and an HDMI sink are on the
	 * combo ports A and B and the two combo PLLs.
	 */
	if (config->kind == I915_XD_SCREEN_DP) {
		if (config->ddi < I915_XD_PICTURE_FIRST_TC_DDI || config->ddi > I915_XD_PICTURE_FIRST_TC_DDI + 3U)
			return EINVAL;
		if (config->pll != I915_XD_PICTURE_PLL_FIRST_TC + config->ddi - I915_XD_PICTURE_FIRST_TC_DDI)
			return EINVAL;
		if (config->sink == NULL)
			return EINVAL;
	} else if (config->kind == I915_XD_SCREEN_PANEL || config->kind == I915_XD_SCREEN_HDMI) {
		if (config->ddi > 1U || config->pll > 1U)
			return EINVAL;
	} else {
		return EINVAL;
	}

	/* An HDMI output carries its TMDS clock as the rate. */
	if (config->kind == I915_XD_SCREEN_HDMI) {
		if (config->rate_khz > I915_XD_PICTURE_HDMI_MAX_KHZ)
			return EINVAL;
		return 0;
	}

	/* The panel is on its port's own AUX channel. */
	if (config->kind == I915_XD_SCREEN_PANEL && config->aux != config->ddi)
		return EINVAL;

	/* A DisplayPort link rate, on 1, 2 or 4 lanes. */
	if (config->rate_khz > I915_XD_PICTURE_DP_MAX_KHZ)
		return EINVAL;
	if (config->lanes != 1U &&
	    config->lanes != 2U &&
	    config->lanes != 4U)
		return EINVAL;

	/* Succeeded: the configuration is lit by this path. */
	return 0;
}

/*
 * Reserves the combo PLL a screen's output runs on: one another pipe
 * runs with the same words, or else the first one no pipe reserved.
 * Returns 0, or EBUSY when both are reserved with other words.
 *
 * Procedure referenced: Linux v6.8.12
 * drivers/gpu/drm/i915/display/intel_dpll_mgr.c (intel_find_shared_dpll,
 * intel_reference_shared_dpll, icl_get_combo_phy_dpll).
 */
static int
i915_xd_picture_reserve(
	struct i915_xd_pictures *pictures,
	struct i915_xd_picture *pic)
{
	struct i915_xd_picture_pll *pll;
	struct i915_xd_dpll_words words;
	uint8_t pipe_bit;
	unsigned index;
	int free_index;
	int shared;

	/* The pipe's bit among the PLL's users. */
	pipe_bit = (uint8_t)(1U << pic->config.pipe);

	/* The words the configuration computed, the AFC startup only with the VBT's override. */
	words = pic->config.pll_words;
	words.div0_set = 0;
	if (pic->config.afc_override)
		words.div0_set = 1;

	/* What the pipe reserved before goes back first. */
	i915_xd_picture_unreserve(pictures, pic->config.pipe);

	/* A combo PLL with the same words, or the first free one. */
	free_index = -1;
	for (index = 0U; index < 2U; index++) {
		pll = &pictures->pll[index];
		if (pll->reserved == 0U) {
			if (free_index < 0)
				free_index = (int)index;
			continue;
		}

		/* A PLL another pipe reserved with the same words is shared. */
		shared = i915_xd_picture_words_equal(&pll->words, &words);
		if (shared) {
			free_index = (int)index;
			break;
		}
	}

	/* Both are reserved with other words. */
	if (free_index < 0) {
		i915_xd_picture_error(pic, "xd-picture: no combo PLL is free for this pipe (both run other words)\n");
		return EBUSY;
	}

	/* The pipe's reservation and the words it shares. */
	pll = &pictures->pll[free_index];
	pll->reserved |= pipe_bit;
	pll->words = words;
	pic->pll = (unsigned)free_index;

	/* Succeeded: the PLL is the screen's. */
	return 0;
}

/*
 * Reserves the two PLLs a Type-C port's output may run on: the
 * Thunderbolt PLL the ports share, at its fixed words, and the port's own
 * PLL at the words of the link rate (the link runs at the rate those
 * words give).  Returns 0, EINVAL when no words give the rate, or EBUSY
 * when another pipe reserved either PLL with other words.
 *
 * Procedure referenced: Linux v6.8.12
 * drivers/gpu/drm/i915/display/intel_dpll_mgr.c (icl_compute_tc_phy_dplls,
 * icl_get_tc_phy_dplls).
 */
static int
i915_xd_picture_reserve_tc(
	struct i915_xd_pictures *pictures,
	struct i915_xd_picture *pic)
{
	const struct i915_xd_platform *platform;
	struct i915_xd_picture_pll *tbt;
	struct i915_xd_picture_pll *own;
	struct i915_xd_tc_pll_words tc_words;
	struct i915_xd_mg_pll_words mg_words;
	uint8_t pipe_bit;
	int afc;
	int same;
	int error;

	/* The platform and the pipe's bit. */
	platform = pic->pictures->platform;
	pipe_bit = (uint8_t)(1U << pic->config.pipe);

	/* The AFC startup the VBT overrides (none without the override). */
	afc = I915_XD_DPLL_AFC_NONE;
	if (pic->config.afc_override)
		afc = 0;

	/* The Thunderbolt PLL's words. */
	kern_memset(&pic->tbt_words, 0, sizeof(pic->tbt_words));
	drv_i915_xd_dpll_tbt_words(platform, pic->config.pll_ref_khz, afc, &pic->tbt_words);

	/* The port's own PLL's words: a Dekel PHY's, or an Ice Lake MG PHY's. */
	kern_memset(&tc_words, 0, sizeof(tc_words));
	kern_memset(&mg_words, 0, sizeof(mg_words));
	if (platform->display_ver < 12U) {
		error = drv_i915_xd_mg_pll_words(pic->config.pll_ref_khz, pic->config.rate_khz, 0, &mg_words);
	} else {
		error = drv_i915_xd_tc_pll_words(pic->config.pll_ref_khz, pic->config.rate_khz, 0, afc, &tc_words);
	}

	/* Refuses a rate the Type-C PLL has no words for. */
	if (error != 0) {
		i915_xd_picture_error(pic, "xd-picture: the Type-C port's PLL has no words for the link rate\n");
		return EINVAL;
	}

	/* The link runs at the rate the port's PLL gives. */
	if (platform->display_ver < 12U) {
		pic->config.rate_khz = drv_i915_xd_mg_pll_port_khz(pic->config.pll_ref_khz, &mg_words);
	} else {
		pic->config.rate_khz = drv_i915_xd_tc_pll_port_khz(pic->config.pll_ref_khz, &tc_words);
	}

	/* What the pipe reserved before goes back first. */
	i915_xd_picture_unreserve(pic->pictures, pic->config.pipe);

	/* The Thunderbolt PLL, unless another pipe reserved it with other words. */
	tbt = &pictures->pll[I915_XD_PICTURE_PLL_TBT];
	if (tbt->reserved != 0U) {
		same = i915_xd_picture_words_equal(&tbt->words, &pic->tbt_words);
		if (!same) {
			i915_xd_picture_error(pic, "xd-picture: the Thunderbolt PLL runs other words for another pipe\n");
			return EBUSY;
		}
	}

	/* The port's own PLL is the port's alone. */
	own = &pictures->pll[pic->config.pll];
	if ((own->reserved & (uint8_t)~pipe_bit) != 0U) {
		i915_xd_picture_error(pic, "xd-picture: the Type-C port's PLL is reserved by another pipe\n");
		return EBUSY;
	}

	/* Both reserved for the pipe. */
	tbt->reserved |= pipe_bit;
	tbt->words = pic->tbt_words;
	own->reserved |= pipe_bit;
	own->tc_words = tc_words;
	own->mg_words = mg_words;
	pic->pll = pic->config.pll;

	/* Succeeded: the PLLs are the screen's. */
	return 0;
}

/* Gives back every PLL reservation of a pipe. */
static void
i915_xd_picture_unreserve(
	struct i915_xd_pictures *pictures,
	unsigned pipe)
{
	unsigned index;

	/* The pipe leaves each PLL's reservation. */
	for (index = 0U; index < I915_XD_PICTURE_PLLS; index++)
		pictures->pll[index].reserved &= (uint8_t)~(1U << pipe);
}

/* Tells whether two combo PLL word sets program the same clock. */
static int
i915_xd_picture_words_equal(
	const struct i915_xd_dpll_words *one,
	const struct i915_xd_dpll_words *other)
{
	/* Each word, then the AFC startup. */
	if (one->cfgcr0 != other->cfgcr0)
		return 0;
	if (one->cfgcr1 != other->cfgcr1)
		return 0;
	if (one->div0 != other->div0)
		return 0;
	if (one->div0_set != other->div0_set)
		return 0;

	/* Succeeded: the same clock. */
	return 1;
}

/*
 * Describes a screen's output from its configuration: the port, the
 * pipe and transcoder, the mode, the link, the PLL and its words, the
 * colours, the DMC programs, the sink's capabilities and the VBT's
 * backlight.  What the output holds (its power, its PLL, its link) is
 * left as it is.
 */
static void
i915_xd_picture_describe(
	struct i915_xd_pictures *pictures,
	struct i915_xd_picture *pic)
{
	const struct i915_xd_picture_config *config;
	const struct i915_xd_picture_pll *own;
	const struct i915_xd_port_desc *port;
	struct i915_xd_output *out;

	/* The screen's configuration and output. */
	config = &pic->config;
	out = &pic->out;

	/* The output, its port and its PHY (the platform's numbering), and its pipe. */
	out->kind = config->kind;
	out->ddi = config->ddi;
	out->phy = config->ddi;
	port = drv_i915_xd_platform_port(pictures->platform, config->ddi);
	if (port != NULL)
		out->phy = port->phy;
	out->aux = config->aux;
	out->pipe = config->pipe;
	out->trans = config->trans;

	/* The mode's timing, progressive. */
	out->timing.hdisplay = config->mode.hdisplay;
	out->timing.hsync_start = config->mode.hsync_start;
	out->timing.hsync_end = config->mode.hsync_end;
	out->timing.htotal = config->mode.htotal;
	out->timing.vdisplay = config->mode.vdisplay;
	out->timing.vsync_start = config->mode.vsync_start;
	out->timing.vsync_end = config->mode.vsync_end;
	out->timing.vtotal = config->mode.vtotal;
	out->timing.interlaced = 0;

	/* The blanking runs from the end of the visible area or the sync, whichever is first, to the end of the total. */
	out->timing.hblank_start = out->timing.hdisplay;
	if (out->timing.hsync_start < out->timing.hblank_start)
		out->timing.hblank_start = out->timing.hsync_start;
	out->timing.hblank_end = out->timing.htotal;
	if (out->timing.hsync_end > out->timing.hblank_end)
		out->timing.hblank_end = out->timing.hsync_end;
	out->timing.vblank_start = out->timing.vdisplay;
	if (out->timing.vsync_start < out->timing.vblank_start)
		out->timing.vblank_start = out->timing.vsync_start;
	out->timing.vblank_end = out->timing.vtotal;
	if (out->timing.vsync_end > out->timing.vblank_end)
		out->timing.vblank_end = out->timing.vsync_end;

	/* The sync polarities and the source the plane fills. */
	out->hsync_high = 0;
	if (config->mode.hsync_positive)
		out->hsync_high = 1;
	out->vsync_high = 0;
	if (config->mode.vsync_positive)
		out->vsync_high = 1;
	out->src_width = config->fb_width;
	out->src_height = config->fb_height;

	/*
	 * The depth, and the dithering only a 6 bpc panel gets (dithering does
	 * not pass the bits through correctly otherwise); full range RGB; one
	 * frame of start delay and no pixel repetition.
	 *
	 * Procedure referenced: Linux v6.8.12
	 * drivers/gpu/drm/i915/display/intel_display.c (intel_modeset_pipe_config).
	 */
	out->bpp = config->bpp;
	out->dither = 0;
	if (config->bpp == I915_XD_PICTURE_DITHER_BPP)
		out->dither = 1;
	out->limited_range = 0;
	out->frame_start_delay = 1U;
	out->pixel_multiplier = 1U;

	/* The link and its M/N; enhanced framing when a DisplayPort sink offers it. */
	out->rate_khz = config->rate_khz;
	out->lanes = config->lanes;
	out->enhanced = 0;
	if (config->kind != I915_XD_SCREEN_HDMI && (config->caps[I915_XD_DPCD_MAX_LANES] & I915_XD_DPCD_LANES_ENHANCED) != 0U)
		out->enhanced = 1;
	out->m_n = config->m_n;
	out->kept = config->kept;

	/* No eDP 1.4 rate select: the link is set by its rate code. */
	out->rate_index = -1;

	/*
	 * An HDMI sink is driven in DVI mode (no infoframes, no scrambling) on
	 * four lanes at 8 bpc, without dithering, the TMDS clock as the rate.
	 */
	if (config->kind == I915_XD_SCREEN_HDMI) {
		out->lanes = 4U;
		out->dither = 0;
	}

	/* A Type-C port. */
	out->tc = 0;
	out->tc_port = 0U;
	if (config->kind == I915_XD_SCREEN_DP) {
		out->tc = 1;
		out->tc_port = i915_xd_picture_tc_port(pic);
	}

	/* The VBT's HDMI level and the panel's swing table settings. */
	out->hdmi_level = config->hdmi_level;
	out->swing_vbt = 0U;
	if (config->vbt_hobl)
		out->swing_vbt |= I915_XD_SWING_HOBL;
	if (config->vbt_low_vswing)
		out->swing_vbt |= I915_XD_SWING_LOW;

	/* The port's PLL and its words; a Type-C port's own PLL's words. */
	out->pll = pic->pll;
	out->pll_words = pictures->pll[pic->pll].words;
	if (out->tc) {
		own = &pictures->pll[pic->pll];
		out->tc_pll = pic->pll;
		out->tc_pll_words = own->tc_words;
		out->tc_pll_words.afc_set = 0;
		if (config->afc_override)
			out->tc_pll_words.afc_set = 1;
		out->mg_pll_words = own->mg_words;
	}

	/* No colour LUT and no colour matrix: 8-bit gamma and no CSC; no line time is programmed. */
	out->gamma_mode = 0U;
	out->csc_mode = 0U;
	out->linetime = 0U;
	out->dmc_programs = config->dmc_programs;

	/* The sink's capabilities as the configuration read them. */
	kern_memcpy(out->caps, config->caps, sizeof(out->caps));

	/* The VBT's backlight, the raw clock and the south display's controllers (not shared with the PPS pins). */
	out->backlight.vbt_present = config->vbt_backlight_present;
	out->backlight.vbt_controller = (unsigned)(uint8_t)config->vbt_backlight_controller;
	out->backlight.vbt_hz = config->vbt_backlight_hz;
	out->backlight.vbt_min = config->vbt_backlight_min;
	out->backlight.rawclk_khz = config->rawclk_khz;
	out->backlight.controllers = 2U;
	out->backlight.muxed = 0;
	out->backlight.inverted = 0;
}

/* Describes the display buffer around a screen's coming commit: the layout now and the one it leaves. */
static void
i915_xd_picture_describe_dbuf(
	struct i915_xd_picture *pic)
{
	struct i915_xd_output_dbuf *dbuf;

	/* Before and after. */
	dbuf = &pic->out.dbuf;
	dbuf->slices_before = pic->old_dbuf.enabled;
	dbuf->slices_after = pic->new_dbuf.enabled;
	dbuf->joined_before = (uint8_t)pic->old_dbuf.joined;
	dbuf->joined_after = (uint8_t)pic->new_dbuf.joined;
	dbuf->pipes_before = pic->old_dbuf.pipes;
	dbuf->pipes_after = pic->new_dbuf.pipes;
}

/*
 * Checks a screen's plan against what the device runs: its link, the
 * watermarks and display buffer beside the other pipes, the CD clock and
 * the memory bandwidth (xd-screen.c).  Nothing is written.
 */
static void
i915_xd_picture_plan(
	struct i915_xd_pictures *pictures,
	struct i915_xd_picture *pic,
	struct i915_xd_plan *plan)
{
	const struct i915_xd_picture_config *config;
	struct i915_xd_plan_input input;
	unsigned index;

	/* The screen's configuration. */
	config = &pic->config;

	/* The output, its mode, and the pipes lit beside it. */
	kern_memset(&input, 0, sizeof(input));
	input.kind = config->kind;
	input.ddi = config->ddi;
	input.pipe = config->pipe;
	input.mode = &config->mode;
	input.platform = pictures->platform;
	input.run_pipes = (uint8_t)(pic->old_dbuf.pipes | (1U << config->pipe) | config->also_pipes);

	/* The CD clock running, its limit, and the memory bandwidth allowed. */
	input.cdclk = config->cdclk;
	input.cdclk_max_khz = config->cdclk_max_khz;
	input.qgv_mbps = config->qgv_mbps;
	input.cpp = 4U;

	/* The memory's watermark levels, the plane's buffer and the pipe's frame start delay. */
	input.wm.levels = config->wm_levels;
	for (index = 0U; index < I915_XD_WM_LEVELS; index++)
		input.wm.latency_us[index] = config->wm_latency[index];
	input.wm.ipc = 0;
	if (config->wm_ipc)
		input.wm.ipc = 1;
	input.wm.sagv_block_us = config->sagv_block_us;
	input.fb_width = config->fb_width;
	input.fb_height = config->fb_height;
	input.frame_start_delay = pic->out.frame_start_delay;

	/* The slices and weights the other pipes have now. */
	for (index = 0U; index < 4U; index++) {
		input.pipe_slices[index] = pic->old_dbuf.slices[index];
		input.pipe_weight[index] = pic->old_dbuf.weight[index];
	}

	/* The link the configuration chose. */
	kern_memset(plan, 0, sizeof(*plan));
	plan->kind = input.kind;
	plan->pipe = input.pipe;
	plan->mode = config->mode;
	plan->rate_khz = config->rate_khz;
	plan->lanes = config->lanes;
	plan->bpp = config->bpp;

	/* The check. */
	(void)drv_i915_xd_screen_plan_check(&input, plan);
}

/*
 * Lays the display buffer out for a screen's light: the layout now with
 * the screen's pipe and the pipes it leaves room for, the plan's joining,
 * slices, weight and range for the pipe.
 */
static void
i915_xd_picture_dbuf_on(
	struct i915_xd_picture *pic,
	const struct i915_xd_plan *plan)
{
	struct i915_xd_picture_dbuf *after;
	unsigned pipe;

	/* The screen's pipe and its new layout. */
	pipe = pic->config.pipe;
	after = &pic->new_dbuf;

	/* The layout now, with the configuration's pipes and the pipe's part. */
	*after = pic->old_dbuf;
	after->pipes = (uint8_t)(pic->old_dbuf.pipes | (1U << pipe) | pic->config.also_pipes);
	after->joined = 0;
	if (plan->joined != 0U)
		after->joined = 1;
	after->slices[pipe] = plan->slices;
	after->enabled = plan->enabled_slices;
	after->weight[pipe] = plan->weight;
	after->ddb[pipe] = plan->pipe_ddb;
}

/*
 * Lays the display buffer out for a screen's dark: the device's layout
 * now (the screen's own when no commit left one) with only the pipes
 * that stay, their joining, the pipe's slices among them and the slices
 * powered; the pipe keeps no weight or range.
 */
static void
i915_xd_picture_dbuf_off(
	struct i915_xd_pictures *pictures,
	struct i915_xd_picture *pic,
	uint8_t keep_pipes)
{
	const struct i915_xd_platform *platform;
	struct i915_xd_picture_dbuf *after;
	unsigned pipe;
	uint8_t joined;

	/* The platform and the screen's pipe. */
	platform = pictures->platform;
	pipe = pic->config.pipe;

	/* The device's layout now, which another screen's commit may have changed. */
	if (pictures->dbuf_valid)
		pic->old_dbuf = pictures->dbuf;

	/* The layout with only the pipes that stay. */
	after = &pic->new_dbuf;
	*after = pic->old_dbuf;
	after->pipes = (uint8_t)(pic->old_dbuf.pipes & keep_pipes);

	/* Their joining and the pipe's slices among them. */
	joined = drv_i915_xd_dbuf_joined(platform, after->pipes);
	after->joined = 0;
	if (joined != 0U)
		after->joined = 1;
	after->slices[pipe] = drv_i915_xd_dbuf_slices(platform, pipe, after->pipes, after->joined);

	/* The slices powered; the pipe keeps no weight or range. */
	after->enabled = drv_i915_xd_dbuf_enabled(after->slices, 4U);
	after->weight[pipe] = 0U;
	kern_memset(&after->ddb[pipe], 0, sizeof(after->ddb[pipe]));
}

/*
 * Keeps what a screen's plan found the CD clock and the bandwidth need:
 * the pipe's and the memory bandwidth's needs, and once a step was found
 * the step, its VCO and voltage level and whether it differs from the
 * running one; the data rate once the CD clock passed.
 */
static void
i915_xd_picture_publish(
	struct i915_xd_picture *pic,
	const struct i915_xd_plan *plan)
{
	/* The needs. */
	pic->cdclk_crtc_min = (int)plan->crtc_min_cdclk_khz;
	pic->cdclk_bw_min = (int)plan->bw_min_cdclk_khz;

	/* A need no step gives keeps nothing more. */
	if (plan->refused == I915_XD_PLAN_CDCLK && plan->cdclk_khz == 0U)
		return;

	/* The step the plan found. */
	pic->cdclk_min = (int)plan->min_cdclk_khz;
	pic->cdclk_khz = (int)plan->cdclk_khz;
	pic->cdclk_vco = (int)plan->vco_khz;
	pic->cdclk_voltage = plan->voltage;

	/* Another step or level than the running one is a change. */
	pic->cdclk_change_needed = 0;
	if (plan->refused == I915_XD_PLAN_CDCLK || plan->refused == I915_XD_PLAN_VOLTAGE) {
		pic->cdclk_change_needed = 1;
		return;
	}

	/* The bandwidth the memory's point is checked against. */
	pic->bw_data_rate = plan->data_mbps;
}

/* Fills a screen's plane: full screen, unscaled and opaque on its pipe, on a buffer at surface. */
static void
i915_xd_picture_plane_fill(
	struct i915_xd_picture *pic,
	uint32_t surface)
{
	struct i915_xd_plane *plane;

	/* The pipe, the buffer's size, pitch and address, at the pipe's origin, opaque. */
	plane = &pic->plane;
	kern_memset(plane, 0, sizeof(*plane));
	plane->pipe = pic->config.pipe;
	plane->width = pic->config.fb_width;
	plane->height = pic->config.fb_height;
	plane->pitch = pic->config.fb_pitch;
	plane->surf = surface;
	plane->alpha = 0xffU;

	/* Its control words. */
	drv_i915_xd_plane_words(pic->pictures->platform, plane);
}

/* Writes a screen's plane's watermarks and its part of the display buffer as the plan found them. */
static void
i915_xd_picture_plane_wm(
	struct i915_xd_picture *pic)
{
	/* The levels, the transition, the SAGV pair (not used: SAGV stays off) and the range. */
	drv_i915_xd_wm_write_plane(&pic->window, pic->pictures->platform, pic->config.pipe, pic->plan.wm_levels, &pic->plan.wm, 0, &pic->plan.plane_ddb);
}

/*
 * Arms a lit screen's plane on its framebuffer: the words that do not
 * arm and the watermarks, then the control word and the arming surface.
 * Returns I915_XD_PICTURE_OK, I915_XD_PICTURE_NOT_READY or
 * I915_XD_PICTURE_ERRORS.
 */
static int
i915_xd_picture_plane_on(
	struct i915_xd_picture *pic)
{
	/* Only a lit pipe takes a plane. */
	if (!pic->prepared || !pic->out.lit)
		return I915_XD_PICTURE_NOT_READY;

	/* The words that do not arm the update, and the watermarks. */
	drv_i915_xd_plane_noarm(&pic->window, &pic->plane);
	i915_xd_picture_plane_wm(pic);

	/*
	 * plane_armed is set before the arming write: from here on the buffer
	 * is not the caller's alone.
	 */
	pic->plane_armed = 1;

	/* The control word and the arming surface. */
	drv_i915_xd_plane_arm(&pic->window, &pic->plane);

	/* An error of the screen fails the update. */
	if (pic->errors != 0U)
		return I915_XD_PICTURE_ERRORS;

	/* Succeeded: the plane is armed. */
	return I915_XD_PICTURE_OK;
}

/*
 * Flips a lit screen's plane to its plane's new buffer: the words that do
 * not arm and the watermarks, then the arming writes as an update that
 * lands in one frame (out of the lines before the vertical blank,
 * interrupts off), the completion event armed for the next blank.
 */
static void
i915_xd_picture_plane_flip(
	struct i915_xd_picture *pic)
{
	struct i915_xd_update_hooks hooks;
	struct i915_xd_update_report report;
	struct i915_xd_update update;
	const struct i915_display_mode *mode;

	/* The words that do not arm the update, and the watermarks. */
	drv_i915_xd_plane_noarm(&pic->window, &pic->plane);
	i915_xd_picture_plane_wm(pic);

	/* The pipe, its frame's lines, the scan line counter's offset and the lines before the blank. */
	mode = &pic->plan.mode;
	update.pipe = pic->config.pipe;
	update.vtotal = (unsigned)mode->vtotal;
	update.scanline_offset = I915_XD_SCANLINE_OFFSET;
	if (pic->config.kind == I915_XD_SCREEN_HDMI)
		update.scanline_offset = I915_XD_SCANLINE_OFFSET_HDMI;
	drv_i915_xd_pipe_evade_window((uint32_t)mode->clock_khz, (uint32_t)mode->htotal, (uint32_t)mode->vdisplay, &update.window);

	/* The backend's side of the update. */
	kern_memset(&hooks, 0, sizeof(hooks));
	hooks.vblank_get = i915_xd_picture_update_vblank_get;
	hooks.vblank_put = i915_xd_picture_update_vblank_put;
	hooks.vblank_sleep = i915_xd_picture_update_vblank_sleep;
	hooks.irq_off = i915_xd_picture_update_irq_off;
	hooks.irq_on = i915_xd_picture_update_irq_on;
	hooks.arm = i915_xd_picture_update_arm;
	hooks.arm_event = i915_xd_picture_update_arm_event;
	hooks.error = i915_xd_picture_update_error;
	hooks.context = pic;

	/* The arming writes in one frame, the event armed. */
	drv_i915_xd_screen_update(&pic->window, &update, &hooks, &report);
}

/*
 * Refuses or arms one flip of a screen, up to the armed event.
 *
 * Refuses a screen that is not lit, is retained, has a flip armed or
 * stuck, the same buffer, an unaligned one, or a backend without the
 * vertical blank hooks (I915_XD_PICTURE_NOT_READY, nothing written).
 * Otherwise the display is kept out of DC states (the cookie in dc_off,
 * for the caller to give back), the plane is updated in one frame, the
 * event holds a vertical blank reference, and I915_XD_PICTURE_OK is
 * returned; result has the details so far either way.
 */
static int
i915_xd_picture_flip_arm(
	struct i915_xd_picture *pic,
	unsigned screen,
	uint32_t surface,
	struct i915_xd_picture_flip *result,
	int *dc_off)
{
	const struct i915_xd_backend *backend;
	unsigned refused;
	unsigned before;
	int domain;
	int retained;
	int lit;
	int error;

	/* The screen's backend. */
	backend = pic->backend;

	/* Nothing written yet: the flip is refused until it is not. */
	kern_memset(result, 0, sizeof(*result));
	result->result = I915_XD_PICTURE_FLIP_REFUSED;
	result->old_surf = pic->flip.current;
	result->new_surf = surface;

	/* A screen never prepared has nothing to flip. */
	if (backend == NULL)
		return I915_XD_PICTURE_NOT_READY;

	/* Refuses what the screen's flip refuses, and a backend without the vertical blank hooks. */
	retained = i915_xd_picture_is_retained(pic->pictures, screen);
	lit = 0;
	if (pic->prepared &&
	    pic->out.lit &&
	    pic->plane_armed)
		lit = 1;
	refused = drv_i915_xd_flip_refused(&pic->flip, lit, retained, surface);
	if (refused != I915_XD_FLIP_OK)
		return I915_XD_PICTURE_NOT_READY;
	if (backend->vblank_get == NULL || backend->wait_event == NULL)
		return I915_XD_PICTURE_NOT_READY;

	/* The screen's operation; the flip's generation and the pipe as it is now. */
	pic->pictures->current = screen;
	pic->flip.gen++;
	result->gen = pic->flip.gen;
	result->live_before = i915_xd_picture_live(pic);
	result->frame_before = drv_i915_xd_pipe_frame(&pic->window, pic->config.pipe);

	/* The same layout on another buffer; a refusal keeps the current one. */
	error = drv_i915_xd_plane_check(pic->config.fb_fourcc, pic->config.fb_modifier, pic->config.fb_width, pic->config.fb_height, pic->config.fb_pitch, surface);
	if (error != 0)
		return I915_XD_PICTURE_NOT_READY;
	i915_xd_picture_plane_fill(pic, surface);

	/* From here both buffers may be read by the display until the completion is known. */
	drv_i915_xd_flip_arm(&pic->flip, surface);
	before = pic->errors;

	/* The display stays out of DC states around the update. */
	domain = i915_xd_picture_dc_domain(pic);
	*dc_off = backend->power_get(backend->ctx, domain);
	i915_xd_picture_plane_flip(pic);

	/* The armed event holds a vertical blank reference. */
	pic->flip_event_ref = 1;
	result->update_errors = (int)(pic->errors - before);

	/* Succeeded: the flip is armed. */
	return I915_XD_PICTURE_OK;
}

/*
 * Completes an armed flip the pipe latched without waiting for its
 * event: the event is given up and its vertical blank reference goes
 * back (the screen's flip already made the new buffer the current one).
 */
static void
i915_xd_picture_flip_retire(
	struct i915_xd_picture *pic)
{
	const struct i915_xd_backend *backend;

	/* The event will not be waited for. */
	backend = pic->backend;
	if (backend->cancel_event != NULL)
		backend->cancel_event(backend->ctx, (int)pic->config.pipe);

	/* Its reference goes back. */
	backend->vblank_put(backend->ctx, (int)pic->config.pipe);
	pic->flip_event_ref = 0;
}

/*
 * Reads the surface a screen's plane scans out now, through the backend
 * itself: a presenting thread reads it too, and the screen's window is
 * the owner's.
 */
static uint32_t
i915_xd_picture_live(
	struct i915_xd_picture *pic)
{
	const struct i915_xd_backend *backend;
	uint32_t live;

	/* A backend that cannot read sees nothing. */
	backend = pic->backend;
	if (backend->read32 == NULL)
		return 0U;

	/* The plane's live surface. */
	live = backend->read32(backend->ctx, I915_XD_PLANE_SURFLIVE(pic->config.pipe));

	/* Succeeded: the surface. */
	return live;
}

/* Returns the backend's domain that keeps the display out of DC states. */
static int
i915_xd_picture_dc_domain(
	struct i915_xd_picture *pic)
{
	int domain;

	/* The backend names it. */
	domain = pic->backend->power_domain(pic->backend->ctx, I915_XD_OUTPUT_POWER_DC_OFF, 0U, 0);

	/* Succeeded: the domain. */
	return domain;
}

/* Tells whether a screen keeps the display out of DC states (inside a commit, or after a dark not confirmed). */
static int
i915_xd_picture_dc_held(
	const struct i915_xd_picture *pic)
{
	/* The output holds the use. */
	if ((pic->out.held & (1U << I915_XD_OUTPUT_POWER_DC_OFF)) != 0U)
		return 1;

	/* Succeeded: the DC states may be entered. */
	return 0;
}

/* Tells whether a screen is retained: its dark was not confirmed, or the caller kept its resources. */
static int
i915_xd_picture_is_retained(
	struct i915_xd_pictures *pictures,
	unsigned screen)
{
	/* A screen out of range retains nothing. */
	if (screen >= I915_XD_PICTURE_SCREENS)
		return 0;

	/* The caller kept its resources. */
	if (pictures->retained[screen])
		return 1;

	/* Its last dark was not confirmed. */
	if (pictures->screen[screen].stop_unconfirmed)
		return 1;

	/* Succeeded: nothing is retained. */
	return 0;
}

/*
 * Tells whether a commit or a backlight change may run on a screen: it
 * is prepared, it does not keep the display out of DC states, and it is
 * not retained.
 */
static int
i915_xd_picture_may_light(
	struct i915_xd_pictures *pictures,
	unsigned screen,
	struct i915_xd_picture *pic)
{
	int retained;
	int held;

	/* A screen never prepared. */
	if (!pic->prepared)
		return 0;

	/* A commit in progress, or a dark that kept the DC states off. */
	held = i915_xd_picture_dc_held(pic);
	if (held)
		return 0;

	/* A retained screen. */
	retained = i915_xd_picture_is_retained(pictures, screen);
	if (retained)
		return 0;

	/* Succeeded: the screen may be committed. */
	return 1;
}

/* Reads the sink's link status of a screen and judges clock recovery and equalisation on its lanes. */
static void
i915_xd_picture_read_link(
	struct i915_xd_picture *pic,
	struct i915_xd_picture_status *status)
{
	struct i915_xd_output_env env;
	uint8_t link_status[I915_XD_DPCD_STATUS_SIZE];
	unsigned lanes;
	int recovered;
	int equalized;

	/* The sink's status bytes. */
	kern_memset(link_status, 0, sizeof(link_status));
	i915_xd_picture_env(pic, &env);
	status->link_status_rc = drv_i915_xd_dp_read_status(&env.sink, I915_XD_PICTURE_HOP_SINK, link_status);
	kern_memcpy(status->link_status, link_status, sizeof(status->link_status));

	/* Judges the status only when it was read. */
	status->cr_ok = 0;
	status->eq_ok = 0;
	if (status->link_status_rc != 0)
		return;

	/* The lanes the link runs on. */
	lanes = pic->config.lanes;
	if (pic->config.kind == I915_XD_SCREEN_HDMI)
		lanes = 4U;

	/* Clock recovery on every lane. */
	recovered = drv_i915_xd_dp_clock_recovered(link_status, lanes);
	if (recovered)
		status->cr_ok = 1;

	/* Equalisation, symbol lock and inter-lane alignment. */
	equalized = drv_i915_xd_dp_equalized(link_status, lanes);
	if (equalized)
		status->eq_ok = 1;
}

/*
 * Fills the environment of a screen's output: its register window and
 * the platform, the backend's power, panel, sink, display buffer and lock
 * hooks, the screen's notes, and for an external DisplayPort sink its
 * converter, its Type-C port and the Dekel PHYs.
 */
static void
i915_xd_picture_env(
	struct i915_xd_picture *pic,
	struct i915_xd_output_env *env)
{
	/* The registers and the platform. */
	kern_memset(env, 0, sizeof(*env));
	env->io = &pic->window;
	env->platform = pic->pictures->platform;

	/* The backend's hooks, on the screen. */
	env->power_get = i915_xd_picture_power_get;
	env->power_put = i915_xd_picture_power_put;
	env->power_put_later = i915_xd_picture_power_put_later;
	env->panel = i915_xd_picture_panel;
	env->sink.read = i915_xd_picture_sink_read;
	env->sink.write = i915_xd_picture_sink_write;
	env->sink.probe = i915_xd_picture_sink_probe;
	env->sink.read_caps = i915_xd_picture_sink_read_caps;
	env->sink.context = pic;
	env->dbuf_slices = i915_xd_picture_dbuf_slices;
	env->lock = i915_xd_picture_lock;
	env->vblank_off = i915_xd_picture_vblank_off;
	env->link_failed = i915_xd_picture_link_failed;
	env->observe = i915_xd_picture_output_observe;

	/* An external DisplayPort sink's converter, its Type-C port and the Dekel PHYs. */
	if (pic->config.kind == I915_XD_SCREEN_DP) {
		env->converter = i915_xd_picture_converter;
		env->tc = &i915_xd_picture_tc;
		env->dkl = &pic->dkl;
	}

	/* The notes, and the context every hook gets. */
	env->error = i915_xd_picture_output_error;
	env->debug = i915_xd_picture_output_debug;
	env->context = pic;
}

/* Counts an error of a screen; the first one is what a failed run is read from, and the backend hears each. */
static void
i915_xd_picture_error(
	struct i915_xd_picture *pic,
	const char *what)
{
	const struct i915_xd_backend *backend;

	/* The first error. */
	if (pic->errors == 0U)
		pic->first_error = what;

	/* One more error of the screen's run. */
	pic->errors++;

	/* The backend hears it. */
	backend = pic->backend;
	if (backend != NULL && backend->error != NULL)
		backend->error(backend->ctx, what);
}

/* Tells a screen's backend that a named point of the commit was reached. */
static void
i915_xd_picture_observe(
	struct i915_xd_picture *pic,
	int point)
{
	const struct i915_xd_backend *backend;

	/* A backend without the hook observes nothing. */
	backend = pic->backend;
	if (backend->observe == NULL)
		return;

	/* The point. */
	backend->observe(backend->ctx, point);
}

/* Records on a screen's backend a step a model has no hook for. */
static void
i915_xd_picture_step(
	struct i915_xd_picture *pic,
	const char *name)
{
	const struct i915_xd_backend *backend;

	/* A backend without the hook records nothing. */
	backend = pic->backend;
	if (backend->step == NULL)
		return;

	/* The step's name. */
	backend->step(backend->ctx, name);
}

/* Fills the light's steps of a commit. */
static void
i915_xd_picture_light_hooks(
	struct i915_xd_screen_hooks *hooks,
	struct i915_xd_picture_commit *commit)
{
	/* The light's steps; the dark's are not run by a light that does not undo. */
	kern_memset(hooks, 0, sizeof(*hooks));
	hooks->power = i915_xd_picture_step_power;
	hooks->dbuf = i915_xd_picture_step_dbuf;
	hooks->setup = i915_xd_picture_step_setup;
	hooks->dmc_on = i915_xd_picture_step_dmc_on;
	hooks->link_power = i915_xd_picture_step_link_power;
	hooks->pll = i915_xd_picture_step_pll;
	hooks->port = i915_xd_picture_step_port;
	hooks->pipe = i915_xd_picture_step_pipe;
	hooks->output = i915_xd_picture_step_output;
	hooks->check = i915_xd_picture_step_check;
	hooks->plane = i915_xd_picture_step_plane;
	hooks->settle = i915_xd_picture_step_settle;
	hooks->context = commit;
}

/* Fills the dark's steps of a commit. */
static void
i915_xd_picture_dark_hooks(
	struct i915_xd_screen_hooks *hooks,
	struct i915_xd_picture_commit *commit)
{
	/* The dark's steps. */
	kern_memset(hooks, 0, sizeof(*hooks));
	hooks->dark_power = i915_xd_picture_step_dark_power;
	hooks->plane_off = i915_xd_picture_step_plane_off;
	hooks->output_off = i915_xd_picture_step_output_off;
	hooks->port_off = i915_xd_picture_step_port_off;
	hooks->pll_off = i915_xd_picture_step_pll_off;
	hooks->link_power_off = i915_xd_picture_step_link_power_off;
	hooks->dmc_off = i915_xd_picture_step_dmc_off;
	hooks->confirm = i915_xd_picture_step_confirm;
	hooks->dark_settle = i915_xd_picture_step_dark_settle;
	hooks->context = commit;
}

/*
 * The light's power: the display kept out of DC states (registers are
 * written and PLLs waited for, which a DC state would undo), the
 * commit's start, then the pipe's power.
 */
static int
i915_xd_picture_step_power(
	void *context)
{
	struct i915_xd_output_env env;
	struct i915_xd_picture_commit *commit;
	struct i915_xd_picture *pic;

	/* DC states held off, the commit's start, then the pipe's power. */
	commit = context;
	pic = commit->pic;
	i915_xd_picture_env(pic, &env);
	(void)drv_i915_xd_output_dc_hold(&pic->out, &env);
	i915_xd_picture_observe(pic, I915_XD_OBSERVE_COMMIT_BEGIN);
	(void)drv_i915_xd_output_power(&pic->out, &env);

	/* Succeeded: the power is held. */
	return 0;
}

/*
 * The light's display buffer before the plane: the memory's point stays
 * as the start left it (the bandwidth was checked in the plan), the bus
 * and both layouts' slices, then the pipe's credits.
 */
static int
i915_xd_picture_step_dbuf(
	void *context)
{
	struct i915_xd_output_env env;
	struct i915_xd_picture_commit *commit;
	struct i915_xd_picture *pic;

	/* The buffer, through the screen's output. */
	commit = context;
	pic = commit->pic;
	i915_xd_picture_env(pic, &env);
	(void)drv_i915_xd_output_dbuf(&pic->out, &env);

	/* Succeeded: the buffer is ready for the pipe. */
	return 0;
}

/* The light's setup: the panel's backlight from the PWM the firmware left and the VBT. */
static int
i915_xd_picture_step_setup(
	void *context)
{
	struct i915_xd_output_env env;
	struct i915_xd_picture_commit *commit;
	struct i915_xd_picture *pic;
	int error;

	/* The commit and its screen. */
	commit = context;
	pic = commit->pic;

	/* Only the panel has a backlight to set up. */
	if (pic->config.kind != I915_XD_SCREEN_PANEL)
		return 0;

	/* The panel's backlight; one without a PWM frequency is an error of the light. */
	i915_xd_picture_env(pic, &env);
	error = drv_i915_xd_output_backlight_setup(&pic->out, &env);
	pic->backlight_setup_rc = 0;
	if (error != 0) {
		pic->backlight_setup_rc = I915_XD_PICTURE_NO_BACKLIGHT;
		i915_xd_picture_error(pic, "xd-picture: the backlight has no PWM frequency from the hardware or the VBT\n");
	}

	/* Succeeded: the setup ran. */
	return 0;
}

/* The light's pipe DMC program. */
static int
i915_xd_picture_step_dmc_on(
	void *context)
{
	struct i915_xd_output_env env;
	struct i915_xd_picture_commit *commit;
	struct i915_xd_picture *pic;

	/* The pipe's DMC program switched on. */
	commit = context;
	pic = commit->pic;
	i915_xd_picture_env(pic, &env);
	(void)drv_i915_xd_output_dmc(&pic->out, &env, 1);

	/* Succeeded: the step ran. */
	return 0;
}

/* The light's link power: a Type-C port's link and the AUX channel's power before the PLL. */
static int
i915_xd_picture_step_link_power(
	void *context)
{
	struct i915_xd_output_env env;
	struct i915_xd_picture_commit *commit;
	struct i915_xd_picture *pic;

	/* The link's power. */
	commit = context;
	pic = commit->pic;
	i915_xd_picture_env(pic, &env);
	(void)drv_i915_xd_output_link_power(&pic->out, &env);

	/* Succeeded: the step ran. */
	return 0;
}

/* The light's PLL: the pipe joins the PLL its port runs on, turning it on when it is the first. */
static int
i915_xd_picture_step_pll(
	void *context)
{
	struct i915_xd_output_env env;
	struct i915_xd_picture_commit *commit;
	struct i915_xd_picture *pic;

	/* The PLL as the device runs it, the pipe on it, and the device's record of it. */
	commit = context;
	pic = commit->pic;
	i915_xd_picture_env(pic, &env);
	i915_xd_picture_pll_load(pic);
	(void)drv_i915_xd_output_pll(&pic->out, &env);
	i915_xd_picture_pll_store(pic);

	/* Succeeded: the step ran. */
	return 0;
}

/* The light's port: the port up, a DisplayPort link trained. */
static int
i915_xd_picture_step_port(
	void *context)
{
	struct i915_xd_output_env env;
	struct i915_xd_picture_commit *commit;
	struct i915_xd_picture *pic;

	/* The link the port is set to, when it is a DisplayPort link. */
	commit = context;
	pic = commit->pic;
	if (pic->config.kind != I915_XD_SCREEN_HDMI) {
		pic->link_rate = pic->out.rate_khz;
		pic->link_lanes = pic->out.lanes;
	}

	/* The port. */
	i915_xd_picture_env(pic, &env);
	(void)drv_i915_xd_output_port(&pic->out, &env);

	/* Succeeded: the step ran. */
	return 0;
}

/* The light's pipe: its source, word, transcoder, colours, line time and chicken bits. */
static int
i915_xd_picture_step_pipe(
	void *context)
{
	struct i915_xd_output_env env;
	struct i915_xd_picture_commit *commit;
	struct i915_xd_picture *pic;

	/* The pipe. */
	commit = context;
	pic = commit->pic;
	i915_xd_picture_env(pic, &env);
	(void)drv_i915_xd_output_pipe(&pic->out, &env);

	/* Succeeded: the step ran. */
	return 0;
}

/* The light's output: the transcoder runs and the output starts, then the backlight device's levels. */
static int
i915_xd_picture_step_output(
	void *context)
{
	struct i915_xd_output_env env;
	struct i915_xd_picture_commit *commit;
	struct i915_xd_picture *pic;

	/* The output. */
	commit = context;
	pic = commit->pic;
	i915_xd_picture_env(pic, &env);
	(void)drv_i915_xd_output_start(&pic->out, &env);

	/* The backlight device's range is the backlight's, its brightness the level in that range. */
	pic->user_max = pic->out.backlight.max;
	pic->user_level = 0U;
	if (pic->user_max != 0U)
		pic->user_level = drv_i915_xd_output_user_level(&pic->out, pic->user_max);

	/* Succeeded: the step ran. */
	return 0;
}

/*
 * The light's check: an error of the light fails it; an HDMI pipe must
 * run; a DisplayPort sink must report clock recovery and equalisation.
 * A failed check keeps the plane back.
 */
static int
i915_xd_picture_step_check(
	void *context)
{
	struct i915_xd_picture_commit *commit;
	struct i915_xd_picture_status status;
	struct i915_xd_picture *pic;
	int result;

	/* The commit and its screen. */
	commit = context;
	pic = commit->pic;

	/* The result, by the rules of the kind. */
	result = I915_XD_PICTURE_OK;
	if (pic->errors != 0U) {
		result = I915_XD_PICTURE_ERRORS;
	} else if (pic->config.kind == I915_XD_SCREEN_HDMI) {
		if (!pic->out.lit)
			result = I915_XD_PICTURE_LINK_NOT_TRAINED;
	} else {
		kern_memset(&status, 0, sizeof(status));
		i915_xd_picture_read_link(pic, &status);
		if (!pic->out.lit ||
		    !status.cr_ok ||
		    !status.eq_ok)
			result = I915_XD_PICTURE_LINK_NOT_TRAINED;
	}

	/* The point after the pipe's enable. */
	commit->enable_result = result;
	i915_xd_picture_observe(pic, I915_XD_OBSERVE_PIPE_ENABLED);

	/* After an anomaly the buffer is not handed to the display. */
	if (result != I915_XD_PICTURE_OK)
		return result;

	/* Succeeded: the pipe runs and its link holds. */
	return 0;
}

/* The light's plane: armed on the framebuffer. */
static int
i915_xd_picture_step_plane(
	void *context)
{
	struct i915_xd_picture_commit *commit;

	/* The plane and its point. */
	commit = context;
	commit->plane_result = i915_xd_picture_plane_on(commit->pic);
	i915_xd_picture_observe(commit->pic, I915_XD_OBSERVE_PLANE_ARMED);

	/* Succeeded: the update ran (its result is the commit's to report). */
	return 0;
}

/*
 * The light's settle: the new layout's slices alone and the unneeded
 * power given back, the new layout the device's, the DC states let back
 * after a delay (not switched on and off with every frame).
 */
static int
i915_xd_picture_step_settle(
	void *context)
{
	struct i915_xd_output_env env;
	struct i915_xd_picture_commit *commit;
	struct i915_xd_picture *pic;

	/* The rest of the commit. */
	commit = context;
	pic = commit->pic;
	i915_xd_picture_env(pic, &env);
	(void)drv_i915_xd_output_settle(&pic->out, &env);

	/* The new layout is the device's from here on. */
	pic->old_dbuf = pic->new_dbuf;
	commit->pictures->dbuf = pic->new_dbuf;
	commit->pictures->dbuf_valid = 1;
	i915_xd_picture_observe(pic, I915_XD_OBSERVE_COMMIT_END);

	/* The DC states come back later. */
	drv_i915_xd_output_dc_allow(&pic->out, &env);

	/* Succeeded: the commit is settled. */
	return 0;
}

/* The dark's power: the display kept out of DC states, the commit's start, the pipe's power marked to go back. */
static int
i915_xd_picture_step_dark_power(
	void *context)
{
	struct i915_xd_output_env env;
	struct i915_xd_picture_commit *commit;
	struct i915_xd_picture *pic;

	/* DC states held off, the commit's start, the power marked. */
	commit = context;
	pic = commit->pic;
	i915_xd_picture_env(pic, &env);
	(void)drv_i915_xd_output_dc_hold(&pic->out, &env);
	i915_xd_picture_observe(pic, I915_XD_OBSERVE_COMMIT_BEGIN);
	(void)drv_i915_xd_output_dark_power(&pic->out, &env);

	/* Succeeded: the power is held. */
	return 0;
}

/* The dark's plane: its disable armed, its errors counted apart from the pipe's. */
static int
i915_xd_picture_step_plane_off(
	void *context)
{
	struct i915_xd_picture_commit *commit;
	struct i915_xd_picture *pic;
	unsigned before;

	/* The plane's disable, judged by the errors it added. */
	commit = context;
	pic = commit->pic;
	before = pic->errors;
	(void)drv_i915_xd_picture_plane_off(commit->pictures, commit->screen);
	commit->plane_result = I915_XD_PICTURE_OK;
	if (pic->errors != before)
		commit->plane_result = I915_XD_PICTURE_ERRORS;
	i915_xd_picture_observe(pic, I915_XD_OBSERVE_PLANE_DISABLED);

	/* The errors of the pipe's dark are counted from here. */
	commit->before = pic->errors;

	/* Succeeded: the step ran (the confirmation judges it). */
	return 0;
}

/* The dark's output: the picture stopped (the panel's backlight off). */
static int
i915_xd_picture_step_output_off(
	void *context)
{
	struct i915_xd_output_env env;
	struct i915_xd_picture_commit *commit;
	struct i915_xd_picture *pic;

	/* The output's picture. */
	commit = context;
	pic = commit->pic;
	i915_xd_picture_env(pic, &env);
	(void)drv_i915_xd_output_stop(&pic->out, &env);

	/* Succeeded: the step ran. */
	return 0;
}

/* The dark's port: the transcoder stops, then the port (and the panel's power). */
static int
i915_xd_picture_step_port_off(
	void *context)
{
	struct i915_xd_output_env env;
	struct i915_xd_picture_commit *commit;
	struct i915_xd_picture *pic;

	/* The transcoder and the port. */
	commit = context;
	pic = commit->pic;
	i915_xd_picture_env(pic, &env);
	(void)drv_i915_xd_output_port_off(&pic->out, &env);

	/* Succeeded: the step ran (the confirmation judges it). */
	return 0;
}

/* The dark's PLL: the pipe leaves its port's PLL, turning it off when it is the last. */
static int
i915_xd_picture_step_pll_off(
	void *context)
{
	struct i915_xd_output_env env;
	struct i915_xd_picture_commit *commit;
	struct i915_xd_picture *pic;

	/* The PLL as the device runs it, the pipe off it, and the device's record of it. */
	commit = context;
	pic = commit->pic;
	i915_xd_picture_env(pic, &env);
	i915_xd_picture_pll_load(pic);
	(void)drv_i915_xd_output_pll_off(&pic->out, &env);
	i915_xd_picture_pll_store(pic);

	/* Succeeded: the step ran (the confirmation judges it). */
	return 0;
}

/* The dark's link power: the AUX channel's power and a Type-C port's link after the PLL. */
static int
i915_xd_picture_step_link_power_off(
	void *context)
{
	struct i915_xd_output_env env;
	struct i915_xd_picture_commit *commit;
	struct i915_xd_picture *pic;

	/* The link's power. */
	commit = context;
	pic = commit->pic;
	i915_xd_picture_env(pic, &env);
	(void)drv_i915_xd_output_link_power_off(&pic->out, &env);

	/* Succeeded: the step ran. */
	return 0;
}

/* The dark's pipe DMC program, after which the pipe no longer runs. */
static int
i915_xd_picture_step_dmc_off(
	void *context)
{
	struct i915_xd_output_env env;
	struct i915_xd_picture_commit *commit;
	struct i915_xd_picture *pic;

	/* The pipe's DMC program switched off. */
	commit = context;
	pic = commit->pic;
	i915_xd_picture_env(pic, &env);
	(void)drv_i915_xd_output_dmc(&pic->out, &env, 0);

	/* Succeeded: the step ran. */
	return 0;
}

/*
 * The dark's confirmation: an error of the pipe's dark, the pipe still
 * on its PLL, a PLL left on with no pipe, a port reference still held,
 * or an error of the plane's leave the stop unconfirmed.  Shrinking the
 * display buffer, giving the pipe's power back or letting the DC states
 * in under a pipe that may still run is the one thing not to do, so
 * everything stays and the caller keeps the buffer.
 */
static int
i915_xd_picture_step_confirm(
	void *context)
{
	struct i915_xd_picture_commit *commit;
	struct i915_xd_picture *pic;
	const struct i915_xd_output *out;
	uint8_t pipe_bit;

	/* The commit, its screen and the output. */
	commit = context;
	pic = commit->pic;
	out = &pic->out;
	pipe_bit = (uint8_t)(1U << pic->config.pipe);

	/* The pipe's dark's result. */
	commit->crtc_result = I915_XD_PICTURE_OK;
	if (pic->errors != commit->before) {
		commit->crtc_result = I915_XD_PICTURE_ERRORS;
	} else if ((out->pll_pipes & pipe_bit) != 0U) {
		commit->crtc_result = I915_XD_PICTURE_STILL_OWNED;
	} else if (out->pll_on && out->pll_pipes == 0U) {
		commit->crtc_result = I915_XD_PICTURE_STILL_OWNED;
	} else if ((out->held & (1U << I915_XD_OUTPUT_POWER_DDI_IO)) != 0U) {
		commit->crtc_result = I915_XD_PICTURE_STILL_OWNED;
	} else if ((out->held & (1U << I915_XD_OUTPUT_POWER_AUX)) != 0U) {
		commit->crtc_result = I915_XD_PICTURE_STILL_OWNED;
	}

	/* Records the pipe as disabled. */
	i915_xd_picture_observe(pic, I915_XD_OBSERVE_PIPE_DISABLED);

	/* A stop not confirmed: the pipe's result first, then the plane's. */
	if (commit->crtc_result != I915_XD_PICTURE_OK || commit->plane_result != I915_XD_PICTURE_OK) {
		pic->stop_unconfirmed = 1;
		if (commit->crtc_result != I915_XD_PICTURE_OK)
			return commit->crtc_result;

		/* Reports the plane's result when the pipe's was good. */
		return commit->plane_result;
	}

	/* Succeeded: the pipe stopped and nothing of the port is held. */
	return 0;
}

/*
 * The dark's settle, its errors counted apart: the display buffer laid
 * out without the pipe and the pipe's power given back, the new layout
 * the device's, the pipe's PLL reservations given back; the DC states
 * back after a delay, unless the settle itself reported an error (a well
 * that would not turn off): then the stop is not confirmed.
 */
static int
i915_xd_picture_step_dark_settle(
	void *context)
{
	struct i915_xd_output_env env;
	struct i915_xd_picture_commit *commit;
	struct i915_xd_picture *pic;
	unsigned before;

	/* The rest of the commit, its errors counted apart. */
	commit = context;
	pic = commit->pic;
	before = pic->errors;
	i915_xd_picture_env(pic, &env);
	(void)drv_i915_xd_output_dark_settle(&pic->out, &env);
	pic->old_dbuf = pic->new_dbuf;
	commit->pictures->dbuf = pic->new_dbuf;
	commit->pictures->dbuf_valid = 1;

	/* The pipe is off: its PLL reservations go back (the PLLs stay for any other pipe). */
	i915_xd_picture_unreserve(commit->pictures, pic->config.pipe);
	i915_xd_picture_observe(pic, I915_XD_OBSERVE_COMMIT_END);

	/* An error in the settle: the DC states stay off and the caller keeps the buffer. */
	if (pic->errors != before) {
		pic->stop_unconfirmed = 1;
		return I915_XD_PICTURE_ERRORS;
	}

	/* The DC states come back later. */
	drv_i915_xd_output_dc_allow(&pic->out, &env);

	/* Succeeded: the screen is dark and everything is given back. */
	return 0;
}

/*
 * Returns the device's PLL a screen's pipe runs on: a combo port's PLL;
 * a Type-C port's own PLL until the port's link is taken and while the
 * port is in DisplayPort alternate or legacy mode; otherwise the
 * Thunderbolt PLL.
 */
static int
i915_xd_picture_pll_index(
	const struct i915_xd_picture *pic)
{
	/* A combo port's PLL. */
	if (pic->config.kind != I915_XD_SCREEN_DP)
		return (int)pic->pll;

	/* Before the port's link is taken, the port's own PLL is the one planned. */
	if (!pic->out.tc_linked)
		return (int)pic->pll;

	/* A Type-C port in DisplayPort alternate or legacy mode runs on its own PLL. */
	if (pic->out.tc_mode == I915_XD_OUTPUT_TC_DP_ALT || pic->out.tc_mode == I915_XD_OUTPUT_TC_LEGACY)
		return (int)pic->pll;

	/* Succeeded: in any other mode, on the Thunderbolt PLL. */
	return (int)I915_XD_PICTURE_PLL_TBT;
}

/*
 * Gives a screen's output the device's record of its own PLL (the pipes
 * on it, whether it is on) before the output joins or leaves it; the
 * Thunderbolt PLL keeps its record in the device alone.
 */
static void
i915_xd_picture_pll_load(
	struct i915_xd_picture *pic)
{
	const struct i915_xd_picture_pll *pll;
	int index;

	/* Only a PLL the output switches itself. */
	index = i915_xd_picture_pll_index(pic);
	if (index < 0 || index == (int)I915_XD_PICTURE_PLL_TBT)
		return;

	/* The device's record. */
	pll = &pic->pictures->pll[index];
	pic->out.pll_pipes = pll->pipes;
	pic->out.pll_on = pll->on;
}

/* Keeps in the device's record of a screen's own PLL what the output left. */
static void
i915_xd_picture_pll_store(
	struct i915_xd_picture *pic)
{
	struct i915_xd_picture_pll *pll;
	int index;

	/* Only a PLL the output switches itself. */
	index = i915_xd_picture_pll_index(pic);
	if (index < 0 || index == (int)I915_XD_PICTURE_PLL_TBT)
		return;

	/* The device's record. */
	pll = &pic->pictures->pll[index];
	pll->pipes = pic->out.pll_pipes;
	pll->on = pic->out.pll_on;
}

/*
 * Returns the backend's domain of one power use of a screen's output: an
 * AUX channel of a Type-C port in Thunderbolt mode has a domain of its
 * own.
 */
static int
i915_xd_picture_power_domain(
	struct i915_xd_picture *pic,
	unsigned use,
	unsigned index)
{
	const struct i915_xd_backend *backend;
	unsigned mode;
	int thunderbolt;
	int domain;

	/* The screen's backend. */
	backend = pic->backend;

	/* A Type-C port's AUX channel in Thunderbolt mode. */
	thunderbolt = 0;
	if (use == I915_XD_OUTPUT_POWER_AUX && pic->config.kind == I915_XD_SCREEN_DP) {
		mode = i915_xd_picture_tc_mode(pic);
		if (mode == I915_XD_OUTPUT_TC_TBT)
			thunderbolt = 1;
	}

	/* The backend names the domain. */
	domain = backend->power_domain(backend->ctx, use, index, thunderbolt);

	/* Succeeded: the domain. */
	return domain;
}

/* Takes a power reference of a screen's output through the backend: the cookie, or 0. */
static int
i915_xd_picture_power_get(
	void *context,
	unsigned use,
	unsigned index)
{
	struct i915_xd_picture *pic;
	int domain;
	int cookie;

	/* The domain of the use. */
	pic = context;
	domain = i915_xd_picture_power_domain(pic, use, index);

	/* The backend's reference. */
	cookie = pic->backend->power_get(pic->backend->ctx, domain);

	/* Succeeded: the cookie (0 when the get failed). */
	return cookie;
}

/* Gives a power reference of a screen's output back through the backend. */
static void
i915_xd_picture_power_put(
	void *context,
	unsigned use,
	unsigned index,
	int cookie)
{
	struct i915_xd_picture *pic;
	int domain;

	/* The domain of the use, given back. */
	pic = context;
	domain = i915_xd_picture_power_domain(pic, use, index);
	pic->backend->power_put(pic->backend->ctx, domain, cookie);
}

/* Gives a power reference of a screen's output back after a delay through the backend. */
static void
i915_xd_picture_power_put_later(
	void *context,
	unsigned use,
	unsigned index,
	int cookie,
	int delay_ms)
{
	struct i915_xd_picture *pic;
	int domain;

	/* The domain of the use, given back later. */
	pic = context;
	domain = i915_xd_picture_power_domain(pic, use, index);
	pic->backend->power_put_async(pic->backend->ctx, domain, cookie, delay_ms);
}

/* Runs one step of the panel's power sequencer through the backend: 0 when done. */
static int
i915_xd_picture_panel(
	void *context,
	unsigned step)
{
	struct i915_xd_picture *pic;
	int done;

	/* The backend's step of the same number. */
	pic = context;
	done = pic->backend->panel(pic->backend->ctx, (int)step);
	if (done != 0)
		return done;

	/* Succeeded: the step ran. */
	return 0;
}

/* Asks the backend for exactly these display buffer slices. */
static void
i915_xd_picture_dbuf_slices(
	void *context,
	uint8_t slices)
{
	struct i915_xd_picture *pic;

	/* The backend's request. */
	pic = context;
	pic->backend->dbuf_slices_update(pic->backend->ctx, (unsigned)slices);
}

/* Takes or drops the backend's PLL or backlight lock (the numbers are the same). */
static void
i915_xd_picture_lock(
	void *context,
	unsigned lock,
	int taken)
{
	struct i915_xd_picture *pic;

	/* The backend's lock. */
	pic = context;
	pic->backend->lock(pic->backend->ctx, (int)lock, taken);
}

/*
 * Settles a screen's armed flip event before its transcoder stops: the
 * backend forgets the event (a later wait is refused, a late vertical
 * blank completes nothing), then its reference goes back, once.
 */
static void
i915_xd_picture_vblank_off(
	void *context)
{
	const struct i915_xd_backend *backend;
	struct i915_xd_picture *pic;

	/* The screen and its backend. */
	pic = context;
	backend = pic->backend;

	/* Nothing to settle without an event that holds a reference. */
	if (!pic->flip_event_ref)
		return;

	/* The backend forgets the event, and its reference goes back. */
	if (backend->cancel_event != NULL)
		backend->cancel_event(backend->ctx, (int)pic->config.pipe);
	backend->vblank_put(backend->ctx, (int)pic->config.pipe);

	/* The event no longer holds a reference; one more event was cancelled. */
	pic->flip_event_ref = 0;
	pic->events_cancelled++;
}

/*
 * Hears that a screen's DisplayPort link did not train.  With the
 * high-efficiency table in use the output gives it up for the next
 * training (it marked that itself); otherwise this path tries no lower
 * link, which is an error of the light.
 *
 * Procedure referenced: Linux v6.8.12
 * drivers/gpu/drm/i915/display/intel_dp_link_training.c
 * (intel_dp_schedule_fallback_link_training).
 */
static void
i915_xd_picture_link_failed(
	void *context)
{
	struct i915_xd_picture *pic;

	/* The high-efficiency table is given up first. */
	pic = context;
	if (pic->out.hobl_active)
		return;

	/* No lower link is tried here. */
	i915_xd_picture_error(pic, "xd-picture: the link did not train and no lower link is tried\n");
}

/*
 * Sets an external sink's protocol converter up for the RGB stream
 * through its own AUX channel, when its owner can (a failure is only
 * noted there); a branch device whose converter cannot be set up is an
 * error of the light.
 *
 * Procedure referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_dp.c
 * (intel_dp_configure_protocol_converter).
 */
static void
i915_xd_picture_converter(
	void *context)
{
	const struct i915_xd_sink_hooks *sink;
	struct i915_xd_picture *pic;

	/* The screen and its sink's hooks. */
	pic = context;
	sink = pic->sink;

	/* The sink's own channel sets its converter up. */
	if (sink != NULL && sink->configure_converter != NULL) {
		(void)sink->configure_converter(sink->ctx);
		return;
	}

	/* A sink before DPCD 1.3, or one that is not a branch device, has no converter. */
	if (pic->out.caps[I915_XD_DPCD_REV] < I915_XD_PICTURE_DPCD_REV_1_3)
		return;
	if ((pic->out.caps[I915_XD_PICTURE_DPCD_PORTS] & I915_XD_PICTURE_DPCD_BRANCH) == 0U)
		return;

	/* A branch device whose converter nobody sets up. */
	i915_xd_picture_error(pic, "xd-picture: the sink is a branch device whose protocol converter cannot be set up\n");
}

/* Hands a point of a screen's output to the backend as the commit's named point. */
static void
i915_xd_picture_output_observe(
	void *context,
	unsigned point)
{
	struct i915_xd_picture *pic;

	/* The screen the hook belongs to. */
	pic = context;

	/* The pipe's underrun report armed. */
	if (point == I915_XD_OUTPUT_SEE_UNDERRUN) {
		i915_xd_picture_observe(pic, I915_XD_OBSERVE_UNDERRUN_ARM);
		return;
	}

	/* The start and the end of the commit. */
	if (point == I915_XD_OUTPUT_SEE_BEGIN) {
		i915_xd_picture_observe(pic, I915_XD_OBSERVE_COMMIT_BEGIN);
	} else {
		i915_xd_picture_observe(pic, I915_XD_OBSERVE_COMMIT_END);
	}
}

/* Counts an error of a screen's output against the commit. */
static void
i915_xd_picture_output_error(
	void *context,
	const char *text)
{
	/* The screen's error. */
	i915_xd_picture_error(context, text);
}

/* Hands a note of a screen's output to the backend. */
static void
i915_xd_picture_output_debug(
	void *context,
	const char *text)
{
	const struct i915_xd_backend *backend;
	struct i915_xd_picture *pic;

	/* A backend without the hook ignores the note. */
	pic = context;
	backend = pic->backend;
	if (backend->debug == NULL)
		return;

	/* The note. */
	backend->debug(backend->ctx, text);
}

/* Reads DPCD bytes of a screen's sink: its own channel's, or the backend's: the bytes read, or a negative error. */
static int
i915_xd_picture_sink_read(
	void *context,
	uint32_t address,
	uint8_t *buffer,
	unsigned size)
{
	struct i915_xd_picture *pic;
	long moved;

	/* A sink with its own channel, or the panel's sink over the backend's channel. */
	pic = context;
	if (pic->sink != NULL) {
		moved = pic->sink->dpcd_read(pic->sink->ctx, address, buffer, size);
	} else {
		moved = pic->backend->dpcd_read(pic->backend->ctx, address, buffer, size);
	}

	/* Reports a failed transfer. */
	if (moved < 0)
		return (int)moved;

	/* Succeeded: the bytes read. */
	return (int)moved;
}

/* Writes DPCD bytes of a screen's sink, as i915_xd_picture_sink_read(). */
static int
i915_xd_picture_sink_write(
	void *context,
	uint32_t address,
	const uint8_t *buffer,
	unsigned size)
{
	struct i915_xd_picture *pic;
	long moved;

	/* A sink with its own channel, or the panel's sink over the backend's channel. */
	pic = context;
	if (pic->sink != NULL) {
		moved = pic->sink->dpcd_write(pic->sink->ctx, address, buffer, size);
	} else {
		moved = pic->backend->dpcd_write(pic->backend->ctx, address, buffer, size);
	}

	/* Reports a failed transfer. */
	if (moved < 0)
		return (int)moved;

	/* Succeeded: the bytes written. */
	return (int)moved;
}

/* Wakes a screen's sink with a one-byte read at an address: 0, the read's negative error, or a short read's. */
static int
i915_xd_picture_sink_probe(
	void *context,
	uint32_t address)
{
	uint8_t byte;
	int moved;

	/* One byte. */
	moved = i915_xd_picture_sink_read(context, address, &byte, 1U);
	if (moved < 0)
		return moved;

	/* A read that did not bring the byte. */
	if (moved != 1)
		return I915_XD_BACKEND_EIO;

	/* Succeeded: the sink answered. */
	return 0;
}

/* Reads a screen's sink's receiver capabilities, the extended ones where it has them: 0, or a negative error. */
static int
i915_xd_picture_sink_read_caps(
	void *context,
	uint8_t *caps)
{
	struct i915_xd_picture *pic;
	int error;

	/* A sink with its own channel, or the panel's sink over the backend's channel. */
	pic = context;
	if (pic->sink != NULL) {
		error = pic->sink->read_dpcd_caps(pic->sink->ctx, caps);
	} else {
		error = pic->backend->read_dpcd_caps(pic->backend->ctx, caps);
	}

	/* Reports a failed read. */
	if (error != 0)
		return error;

	/* Succeeded: the capabilities are in place. */
	return 0;
}

/* Returns the Type-C port of a screen's output (0 for the first): the platform's, or its place after the combo ports. */
static unsigned
i915_xd_picture_tc_port(
	const struct i915_xd_picture *pic)
{
	const struct i915_xd_port_desc *port;

	/* The platform's Type-C port of the DDI. */
	port = drv_i915_xd_platform_port(pic->pictures->platform, pic->config.ddi);
	if (port != NULL && port->tc != 0U)
		return (unsigned)port->tc - 1U;

	/* Succeeded: the place after the combo ports. */
	return pic->config.ddi - I915_XD_PICTURE_FIRST_TC_DDI;
}

/* Takes the Type-C port's link for a screen's lanes, and with it the PHY. */
static void
i915_xd_picture_tc_get_link(
	void *context,
	unsigned lanes)
{
	const struct i915_xd_backend *backend;
	struct i915_xd_picture *pic;

	/* The screen and its backend. */
	pic = context;
	backend = pic->backend;

	/* A model records the step. */
	if (backend->tc_get_link == NULL) {
		i915_xd_picture_step(pic, "the Type-C port's link taken");
		return;
	}

	/* The port's link. */
	backend->tc_get_link(backend->ctx, (int)i915_xd_picture_tc_port(pic), (int)lanes);
}

/* Gives a screen's Type-C port's link back; the last one gives the PHY back. */
static void
i915_xd_picture_tc_put_link(
	void *context)
{
	const struct i915_xd_backend *backend;
	struct i915_xd_picture *pic;

	/* The screen and its backend. */
	pic = context;
	backend = pic->backend;

	/* A model records the step. */
	if (backend->tc_put_link == NULL) {
		i915_xd_picture_step(pic, "the Type-C port's link given back");
		return;
	}

	/* The port's link. */
	backend->tc_put_link(backend->ctx, (int)i915_xd_picture_tc_port(pic));
}

/* Reports the mode a screen's Type-C port is held in, in the output's numbering. */
static unsigned
i915_xd_picture_tc_mode(
	void *context)
{
	const struct i915_xd_backend *backend;
	struct i915_xd_picture *pic;
	int mode;

	/* The screen and its backend. */
	pic = context;
	backend = pic->backend;

	/* A model records the step and holds the port in no mode. */
	if (backend->tc_mode == NULL) {
		i915_xd_picture_step(pic, "the Type-C port's mode asked");
		return I915_XD_OUTPUT_TC_NONE;
	}

	/* The backend's answer, in the output's numbering. */
	mode = backend->tc_mode(backend->ctx, (int)i915_xd_picture_tc_port(pic));
	if (mode == I915_XD_BACKEND_TC_TBT)
		return I915_XD_OUTPUT_TC_TBT;
	if (mode == I915_XD_BACKEND_TC_DP_ALT)
		return I915_XD_OUTPUT_TC_DP_ALT;
	if (mode == I915_XD_BACKEND_TC_LEGACY)
		return I915_XD_OUTPUT_TC_LEGACY;

	/* Succeeded: the port is held in no mode. */
	return I915_XD_OUTPUT_TC_NONE;
}

/*
 * Gives the display a screen's Type-C PHY lanes for a link of so many
 * lanes, mirrored when the DDI buffer is lane-reversed (which only a
 * legacy port may be).
 */
static void
i915_xd_picture_tc_lanes(
	void *context,
	unsigned lanes)
{
	const struct i915_xd_backend *backend;
	struct i915_xd_picture *pic;
	unsigned mode;
	int reversed;

	/* The screen and its backend. */
	pic = context;
	backend = pic->backend;

	/* Whether the DDI buffer is lane-reversed. */
	reversed = 0;
	if ((pic->config.kept & I915_XD_DDI_BUF_REVERSAL) != 0U)
		reversed = 1;

	/* A model records the step. */
	if (backend->tc_set_fia_lane_count == NULL) {
		i915_xd_picture_step(pic, "the Type-C port's lanes given to the display");
		return;
	}

	/* Only a legacy port's lanes are reversed. */
	if (reversed) {
		mode = i915_xd_picture_tc_mode(pic);
		if (mode != I915_XD_OUTPUT_TC_LEGACY)
			i915_xd_picture_error(pic, "xd-picture: the lanes of a Type-C port that is not legacy are reversed\n");
	}

	/* The FIA's lanes. */
	backend->tc_set_fia_lane_count(backend->ctx, (int)i915_xd_picture_tc_port(pic), (int)lanes, reversed);
}

/* Reports the DisplayPort pin assignment the FIA records for a screen's Type-C port (0 in a model). */
static unsigned
i915_xd_picture_tc_pin(
	void *context)
{
	const struct i915_xd_backend *backend;
	struct i915_xd_picture *pic;
	unsigned pin;

	/* The screen and its backend. */
	pic = context;
	backend = pic->backend;

	/* A model records the step and answers no assignment. */
	if (backend->tc_pin_assignment == NULL) {
		i915_xd_picture_step(pic, "the Type-C port's pin assignment asked");
		return 0U;
	}

	/* The FIA's pin assignment. */
	pin = backend->tc_pin_assignment(backend->ctx, (int)i915_xd_picture_tc_port(pic));

	/* Succeeded: the assignment. */
	return pin;
}

/*
 * Turns the Thunderbolt PLL the Type-C ports share on for a screen's
 * pipe (the first pipe turns it on with its words) or off (the last one
 * turns it off), under the PLLs' lock.  Waits that ran out are errors of
 * the screen; the answer is 0.
 */
static int
i915_xd_picture_tc_tbt_pll(
	void *context,
	int on)
{
	const struct i915_xd_pll_desc *desc;
	struct i915_xd_picture_pll *pll;
	struct i915_xd_picture *pic;
	uint8_t pipe_bit;
	unsigned late;

	/* The screen, the Thunderbolt PLL and the pipe's bit. */
	pic = context;
	pll = &pic->pictures->pll[I915_XD_PICTURE_PLL_TBT];
	pipe_bit = (uint8_t)(1U << pic->config.pipe);
	desc = drv_i915_xd_dpll_desc(pic->pictures->platform, I915_XD_PICTURE_PLL_TBT);

	/* The PLL's record and its switch, under the PLLs' lock. */
	i915_xd_picture_lock(pic, I915_XD_OUTPUT_LOCK_PLL, 1);

	late = 0U;
	if (on) {
		if (pll->pipes == 0U && desc != NULL) {
			late = drv_i915_xd_dpll_enable(&pic->window, pic->pictures->platform, desc, &pic->tbt_words);
			pll->on = 1;
		}

		/* The pipe uses the PLL. */
		pll->pipes |= pipe_bit;
	} else {
		pll->pipes &= (uint8_t)~pipe_bit;
		if (pll->pipes == 0U &&
		    pll->on &&
		    desc != NULL) {
			late = drv_i915_xd_dpll_disable(&pic->window, desc);
			pll->on = 0;
		}
	}

	i915_xd_picture_lock(pic, I915_XD_OUTPUT_LOCK_PLL, 0);

	/* The waits that ran out. */
	if ((late & I915_XD_DPLL_LATE_POWER_ON) != 0U)
		i915_xd_picture_error(pic, "xd-picture: the Thunderbolt PLL's power did not come on\n");
	if ((late & I915_XD_DPLL_LATE_LOCK) != 0U)
		i915_xd_picture_error(pic, "xd-picture: the Thunderbolt PLL did not lock\n");
	if ((late & I915_XD_DPLL_LATE_UNLOCK) != 0U)
		i915_xd_picture_error(pic, "xd-picture: the Thunderbolt PLL did not unlock\n");
	if ((late & I915_XD_DPLL_LATE_POWER_OFF) != 0U)
		i915_xd_picture_error(pic, "xd-picture: the Thunderbolt PLL's power did not go off\n");

	/* Succeeded: the errors are the screen's. */
	return 0;
}

/* Takes a reference to the pipe's vertical blank interrupt through the backend: 0, or its error. */
static int
i915_xd_picture_update_vblank_get(
	void *context)
{
	struct i915_xd_picture *pic;
	int error;

	/* The backend's reference. */
	pic = context;
	error = pic->backend->vblank_get(pic->backend->ctx, (int)pic->config.pipe);
	if (error != 0)
		return error;

	/* Succeeded: the interrupt is on and referenced. */
	return 0;
}

/* Gives a reference to the pipe's vertical blank interrupt back. */
static void
i915_xd_picture_update_vblank_put(
	void *context)
{
	struct i915_xd_picture *pic;

	/* The backend's reference goes back. */
	pic = context;
	pic->backend->vblank_put(pic->backend->ctx, (int)pic->config.pipe);
}

/* Sleeps until the pipe's next vertical blank or the ticks run out; the ticks left. */
static long
i915_xd_picture_update_vblank_sleep(
	void *context,
	long ticks)
{
	struct i915_xd_picture *pic;
	long left;

	/* The backend's sleep. */
	pic = context;
	left = pic->backend->vblank_sleep(pic->backend->ctx, (int)pic->config.pipe, ticks);

	/* Succeeded: the ticks left (0 when the blank did not come). */
	return left;
}

/* Turns interrupts off for the update, unless they are off already. */
static void
i915_xd_picture_update_irq_off(
	void *context)
{
	struct i915_xd_picture *pic;

	/* Off through the backend, once. */
	pic = context;
	if (!pic->pictures->irqs_off)
		pic->backend->irq_off(pic->backend->ctx);

	/* From here the update has interrupts off. */
	pic->pictures->irqs_off = 1;
}

/* Turns interrupts back on after the update, when it had them off. */
static void
i915_xd_picture_update_irq_on(
	void *context)
{
	struct i915_xd_picture *pic;

	/* On through the backend. */
	pic = context;
	if (pic->pictures->irqs_off)
		pic->backend->irq_on(pic->backend->ctx);

	/* From here the update has interrupts on. */
	pic->pictures->irqs_off = 0;
}

/* Writes the plane's control word and the arming surface. */
static void
i915_xd_picture_update_arm(
	void *context)
{
	struct i915_xd_picture *pic;

	/* The arming writes. */
	pic = context;
	drv_i915_xd_plane_arm(&pic->window, &pic->plane);
}

/* Arms the flip's completion event for the pipe's next vertical blank. */
static void
i915_xd_picture_update_arm_event(
	void *context)
{
	struct i915_xd_picture *pic;

	/* The backend's event. */
	pic = context;
	pic->backend->arm_event(pic->backend->ctx, (int)pic->config.pipe);
}

/* Counts an error of the update as one of the screen's. */
static void
i915_xd_picture_update_error(
	void *context,
	const char *what)
{
	/* The screen's error. */
	i915_xd_picture_error(context, what);
}
