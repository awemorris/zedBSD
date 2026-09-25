/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Derived from the Linux kernel v6.8.12 (drivers/gpu/drm/i915/display/intel_atomic_plane.c),
 * which carries the following notice.
 *
 * Copyright © 2014 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

/*
 * Derived from the Linux kernel v6.8.12 (drivers/gpu/drm/i915/display/skl_universal_plane.c),
 * which carries the following notice.
 *
 * SPDX-License-Identifier: MIT
 *
 * Copyright © 2020 Intel Corporation
 */

/*
 * The universal plane of the one-screen path (see plane.h).
 *
 * The functions follow the Linux 6.8.12 text of skl_universal_plane.c (the
 * control words and the arming and non-arming register writers of display
 * version 11 and later, the plane's CDCLK need and its readout) and
 * intel_atomic_plane.c (the plane's pixel and data rates).  Every register
 * access goes through the device's backend (modeset-internal.h), so the
 * same text records register words, drives the model in the tests and
 * programs the real display; the callees that are not ported (the scaler,
 * the input CSC, the black CSC) are named steps.  PLANE_CTL is the Linux
 * word (0x94000000 for the linear XRGB8888 primary plane of display 13).
 *
 * Around that text: the plane of the modeset object (one full-screen
 * primary plane on a linear XRGB8888 framebuffer, refused before any Linux
 * text runs when anything else is asked for), its update and its
 * synchronous flip, and the word recorder of the calculation.
 */

#include "modeset-internal.h"
#include "takeover-internal.h"
#include "plane.h"
#include "pipe.h"
#include <kern/kcrt.h>

#include <uapi/errno.h>

static bool i915_icl_is_hdr_plane(struct drm_i915_private *dev_priv, enum plane_id plane_id);
static unsigned int i915_skl_plane_stride_mult(const struct drm_framebuffer *fb, int color_plane, unsigned int rotation);
static u32 i915_skl_plane_stride(const struct intel_plane_state *plane_state, int color_plane);
static u32 i915_skl_plane_ctl_format(u32 pixel_format);
static u32 i915_skl_plane_ctl_alpha(const struct intel_plane_state *plane_state);
static u32 i915_glk_plane_color_ctl_alpha(const struct intel_plane_state *plane_state);
static u32 i915_skl_plane_ctl_tiling(u64 fb_modifier);
static u32 i915_skl_plane_ctl_rotate(unsigned int rotate);
static u32 i915_icl_plane_ctl_flip(unsigned int reflect);
static u32 i915_adlp_plane_ctl_arb_slots(const struct intel_plane_state *plane_state);
static u32 i915_skl_plane_ctl_crtc(const struct intel_crtc_state *crtc_state);
static u32 i915_skl_plane_ctl(const struct intel_crtc_state *crtc_state, const struct intel_plane_state *plane_state);
static u32 i915_glk_plane_color_ctl_crtc(const struct intel_crtc_state *crtc_state);
static u32 i915_glk_plane_color_ctl(const struct intel_crtc_state *crtc_state, const struct intel_plane_state *plane_state);
static u32 i915_skl_surf_address(const struct intel_plane_state *plane_state, int color_plane);
static u32 i915_skl_plane_surf(const struct intel_plane_state *plane_state, int color_plane);
static u32 i915_skl_plane_aux_dist(const struct intel_plane_state *plane_state, int color_plane);
static u32 i915_skl_plane_keyval(const struct intel_plane_state *plane_state);
static u32 i915_skl_plane_keymsk(const struct intel_plane_state *plane_state);
static u32 i915_skl_plane_keymax(const struct intel_plane_state *plane_state);
static int i915_icl_plane_color_plane(const struct intel_plane_state *plane_state);
static void i915_icl_plane_update_sel_fetch_noarm(struct intel_plane *plane, const struct intel_crtc_state *crtc_state, const struct intel_plane_state *plane_state, int color_plane);
static void i915_icl_plane_update_noarm(struct intel_plane *plane, const struct intel_crtc_state *crtc_state, const struct intel_plane_state *plane_state);
static void i915_icl_plane_disable_sel_fetch_arm(struct intel_plane *plane, const struct intel_crtc_state *crtc_state);
static void i915_icl_plane_update_sel_fetch_arm(struct intel_plane *plane, const struct intel_crtc_state *crtc_state, const struct intel_plane_state *plane_state);
static void i915_icl_plane_update_arm(struct intel_plane *plane, const struct intel_crtc_state *crtc_state, const struct intel_plane_state *plane_state);
static void i915_icl_plane_disable_arm(struct intel_plane *plane, const struct intel_crtc_state *crtc_state);
static int i915_icl_plane_min_cdclk(const struct intel_crtc_state *crtc_state, const struct intel_plane_state *plane_state);
static bool i915_skl_plane_get_hw_state(struct intel_plane *plane, enum pipe *pipe);
static bool i915_use_min_ddb(const struct intel_crtc_state *crtc_state, struct intel_plane *plane);
static unsigned int i915_plane_relative_data_rate(const struct intel_crtc_state *crtc_state, const struct intel_plane_state *plane_state, int color_plane);
static const struct drm_format_info *i915_xrgb8888_format(void);

/*
 * Returns the planes that have the HDR pipeline (the Linux
 * icl_hdr_plane_mask()): the primary plane and the first two sprites.
 */
u8
drv_i915_icl_hdr_plane_mask(void)
{
	/* Succeeded: reports the mask. */
	return BIT(PLANE_PRIMARY) | BIT(PLANE_SPRITE0) | BIT(PLANE_SPRITE1);
}

/*
 * Scales a pixel rate by a plane's downscaling (the Linux
 * intel_adjusted_rate()).
 *
 * The source rectangle is in 16.16 fixed point.  Downscaling limits the
 * maximum pixel rate: the destination is taken no larger than the source.
 */
unsigned int
drv_i915_adjusted_rate(
	const struct drm_rect *src,
	const struct drm_rect *dst,
	unsigned int rate)
{
	unsigned int src_w;
	unsigned int src_h;
	unsigned int dst_w;
	unsigned int dst_h;

	/* The source size in whole pixels and the destination size. */
	src_w = i915_drm_rect_width(src) >> 16;
	src_h = i915_drm_rect_height(src) >> 16;
	dst_w = i915_drm_rect_width(dst);
	dst_h = i915_drm_rect_height(dst);

	/* Downscaling limits the maximum pixel rate. */
	dst_w = min(src_w, dst_w);
	dst_h = min(src_h, dst_h);

	/* Succeeded: reports the rate scaled by the area ratio, rounded up. */
	return DIV_ROUND_UP_ULL(mul_u32_u32(rate, src_w * src_h), dst_w * dst_h);
}

/*
 * Returns the pixel rate a plane fetches at (the Linux
 * intel_plane_pixel_rate()).
 *
 * The plane's visibility is not checked: the watermark code uses this for
 * a cursor that is fully off screen too.
 */
unsigned int
drv_i915_plane_pixel_rate(
	const struct intel_crtc_state *crtc_state,
	const struct intel_plane_state *plane_state)
{
	unsigned int rate;

	/* Scales the pipe's pixel rate by the plane's downscaling. */
	rate = drv_i915_adjusted_rate(&plane_state->uapi.src, &plane_state->uapi.dst, crtc_state->pixel_rate);

	/* Succeeded: reports the rate. */
	return rate;
}

/*
 * Returns the data rate of one colour plane of a visible plane (the Linux
 * intel_plane_data_rate()): its pixel rate times its bytes per pixel.
 */
unsigned int
drv_i915_plane_data_rate(
	const struct intel_crtc_state *crtc_state,
	const struct intel_plane_state *plane_state,
	int color_plane)
{
	const struct drm_framebuffer *fb;
	unsigned int pixel_rate;

	/* The framebuffer of the plane. */
	fb = plane_state->hw.fb;

	/* An invisible plane fetches nothing. */
	if (!plane_state->uapi.visible)
		return 0;

	/* The plane's pixel rate. */
	pixel_rate = drv_i915_plane_pixel_rate(crtc_state, plane_state);

	/* Succeeded: reports the bytes per second of the colour plane. */
	return pixel_rate * fb->format->cpp[color_plane];
}

/*
 * Records the plane words of one full-screen primary plane.
 *
 * The plane covers the pipe without scaling on a linear XRGB8888 buffer:
 * source the whole framebuffer (16.16), destination the same rectangle,
 * rotation 0, opaque, the DRM default blend mode, no colour key, BT.709
 * limited-range defaults (unused for RGB), no scaler.  PLANE_CTL and
 * PLANE_COLOR_CTL are the Linux skl_plane_check() words; then the Linux
 * icl_plane_update_noarm() and icl_plane_update_arm() write them.  The
 * words go to `emit`.  Anything else is refused before any Linux text
 * runs: 0, or EINVAL.  The device view, the crtc state and the plane
 * state are the world's (former function statics).
 */
int
drv_i915_plane_emit(
	struct i915_lcd_world *world,
	struct i915_lcd_emit *emit,
	int pipe,
	int plane_id,
	u32 fourcc,
	u64 modifier,
	u32 width,
	u32 height,
	u32 pitch,
	u32 surf_ggtt_offset)
{
	struct drm_i915_private *i915;
	struct intel_crtc crtc;
	struct intel_crtc_state *crtc_state;
	struct intel_plane plane;
	struct intel_plane_state *plane_state;
	struct drm_framebuffer fb;

	/* The recorder's device view and states are the world's. */
	i915 = &world->i915_plane_emit_i915;
	crtc_state = &world->i915_plane_emit_crtc_state;
	plane_state = &world->i915_plane_emit_plane_state;

	/* Only a linear XRGB8888 framebuffer is handled. */
	if (fourcc != FORMAT_XRGB8888)
		return EINVAL;
	if (modifier != FORMAT_MOD_LINEAR)
		return EINVAL;

	/* Only the primary plane of pipes A to D. */
	if (pipe < 0 || pipe > 3)
		return EINVAL;
	if (plane_id != PLANE_PRIMARY)
		return EINVAL;

	/* A size of 1 to 8192 pixels each way. */
	if (width == 0u || height == 0u)
		return EINVAL;
	if (width > 8192u || height > 8192u)
		return EINVAL;

	/* A pitch that holds a line and is a multiple of 64 bytes. */
	if (pitch < width * 4u)
		return EINVAL;
	if ((pitch % 64u) != 0u)
		return EINVAL;

	/* A page-aligned surface. */
	if ((surf_ggtt_offset & 0xfffu) != 0u)
		return EINVAL;

	/* Starts every object from zero. */
	kern_memset(i915, 0, sizeof(*i915));
	kern_memset(&crtc, 0, sizeof(crtc));
	kern_memset(crtc_state, 0, sizeof(*crtc_state));
	kern_memset(&plane, 0, sizeof(plane));
	kern_memset(plane_state, 0, sizeof(*plane_state));
	kern_memset(&fb, 0, sizeof(fb));

	/* The device writes through the caller's backend. */
	i915->emit = emit;

	/* The crtc and its state. */
	crtc.base.dev = &i915->drm;
	crtc.pipe = (enum pipe)pipe;
	crtc_state->uapi.crtc = &crtc.base;

	/* The primary plane of the pipe. */
	plane.base.dev = &i915->drm;
	plane.id = (enum plane_id)plane_id;
	plane.pipe = (enum pipe)pipe;

	/* The linear XRGB8888 framebuffer. */
	fb.dev = &i915->drm;
	fb.format = i915_xrgb8888_format();
	fb.modifier = modifier;

	/* The plane state of a full-screen, unscaled, opaque plane. */
	plane_state->uapi.plane = &plane.base;
	plane_state->uapi.src.x2 = (int)(width << 16);
	plane_state->uapi.src.y2 = (int)(height << 16);
	plane_state->uapi.dst.x2 = (int)width;
	plane_state->uapi.dst.y2 = (int)height;
	plane_state->hw.fb = &fb;
	plane_state->hw.rotation = MODE_ROTATE_0;
	plane_state->hw.alpha = 0xffff;
	plane_state->hw.pixel_blend_mode = MODE_BLEND_PREMULTI;
	plane_state->hw.color_encoding = COLOR_YCBCR_BT709;
	plane_state->hw.color_range = COLOR_YCBCR_LIMITED_RANGE;
	plane_state->view.color_plane[0].scanout_stride = pitch;
	plane_state->view.color_plane[0].mapping_stride = pitch;
	plane_state->scaler_id = -1;
	plane_state->ggtt_offset = surf_ggtt_offset;

	/* The control words, as skl_plane_check() computes them. */
	plane_state->ctl = i915_skl_plane_ctl(crtc_state, plane_state);
	plane_state->color_ctl = i915_glk_plane_color_ctl(crtc_state, plane_state);

	/* Writes the plane: the unarmed registers, then PLANE_CTL and the arming PLANE_SURF. */
	i915_icl_plane_update_noarm(&plane, crtc_state, plane_state);
	i915_icl_plane_update_arm(&plane, crtc_state, plane_state);

	/* Succeeded: the words are emitted. */
	return 0;
}

/*
 * Prepares the plane of a modeset object.
 *
 * The framebuffer and plane state are filled exactly as
 * drv_i915_plane_emit() fills them (same checks, same refusals), and
 * PLANE_CTL and PLANE_COLOR_CTL are computed with the Linux functions.
 * 0, or EINVAL for anything but a linear XRGB8888 buffer of 1 to 8192
 * pixels each way with a 64-byte multiple pitch at a page-aligned address.
 */
int
drv_i915_lcd_ms_plane_prepare(
	struct i915_lcd_modeset *ms,
	u32 fourcc,
	u64 modifier,
	u32 width,
	u32 height,
	u32 pitch,
	u32 surf_ggtt_offset)
{
	/* Only a linear XRGB8888 framebuffer is handled. */
	if (fourcc != FORMAT_XRGB8888)
		return EINVAL;
	if (modifier != FORMAT_MOD_LINEAR)
		return EINVAL;

	/* A size of 1 to 8192 pixels each way. */
	if (width == 0u || height == 0u)
		return EINVAL;
	if (width > 8192u || height > 8192u)
		return EINVAL;

	/* A pitch that holds a line and is a multiple of 64 bytes. */
	if (pitch < width * 4u)
		return EINVAL;
	if ((pitch % 64u) != 0u)
		return EINVAL;

	/* A page-aligned surface. */
	if ((surf_ggtt_offset & 0xfffu) != 0u)
		return EINVAL;

	/* Starts the plane, its state and the framebuffer from zero. */
	kern_memset(&ms->plane, 0, sizeof(ms->plane));
	kern_memset(&ms->plane_state, 0, sizeof(ms->plane_state));
	kern_memset(&ms->fb, 0, sizeof(ms->fb));

	/* The primary plane of the object's pipe. */
	ms->plane.base.dev = &ms->i915.drm;
	ms->plane.id = PLANE_PRIMARY;
	ms->plane.pipe = ms->crtc.pipe;

	/* The linear XRGB8888 framebuffer. */
	ms->fb.dev = &ms->i915.drm;
	ms->fb.format = i915_xrgb8888_format();
	ms->fb.modifier = modifier;

	/* The plane state of a full-screen, unscaled, opaque plane. */
	ms->plane_state.uapi.plane = &ms->plane.base;
	ms->plane_state.uapi.src.x2 = (int)(width << 16);
	ms->plane_state.uapi.src.y2 = (int)(height << 16);
	ms->plane_state.uapi.dst.x2 = (int)width;
	ms->plane_state.uapi.dst.y2 = (int)height;
	ms->plane_state.hw.fb = &ms->fb;
	ms->plane_state.hw.rotation = MODE_ROTATE_0;
	ms->plane_state.hw.alpha = 0xffff;
	ms->plane_state.hw.pixel_blend_mode = MODE_BLEND_PREMULTI;
	ms->plane_state.hw.color_encoding = COLOR_YCBCR_BT709;
	ms->plane_state.hw.color_range = COLOR_YCBCR_LIMITED_RANGE;
	ms->plane_state.view.color_plane[0].scanout_stride = pitch;
	ms->plane_state.view.color_plane[0].mapping_stride = pitch;
	ms->plane_state.scaler_id = -1;
	ms->plane_state.ggtt_offset = surf_ggtt_offset;

	/* The control words, as skl_plane_check() computes them. */
	ms->plane_state.ctl = i915_skl_plane_ctl(&ms->crtc_state, &ms->plane_state);
	ms->plane_state.color_ctl = i915_glk_plane_color_ctl(&ms->crtc_state, &ms->plane_state);

	/* Succeeded: the plane is ready for its update. */
	return 0;
}

/*
 * Writes the plane of a modeset object.
 *
 * The unarmed registers first, then PLANE_CTL and PLANE_SURF; the surface
 * write arms the update, and from then on the buffer may be scanned out.
 */
void
drv_i915_lcd_ms_plane_update(
	struct i915_lcd_modeset *ms)
{
	/* Writes the registers that do not arm the update. */
	i915_icl_plane_update_noarm(&ms->plane, &ms->crtc_state, &ms->plane_state);

	/*
	 * plane_armed is set BEFORE the arming write: from here the buffer
	 * is not the caller's alone.
	 */
	ms->plane_armed = 1;

	/* Writes PLANE_CTL and the arming PLANE_SURF. */
	i915_icl_plane_update_arm(&ms->plane, &ms->crtc_state, &ms->plane_state);
}

/*
 * Disables the plane of a modeset object (the Linux icl_plane_disable_arm()).
 */
void
drv_i915_lcd_ms_plane_disable(
	struct i915_lcd_modeset *ms)
{
	/* Writes the disable, armed by the PLANE_SURF write. */
	i915_icl_plane_disable_arm(&ms->plane, &ms->crtc_state);
}

/*
 * Records the CDCLK the plane of a modeset object needs.
 *
 * The Linux intel_plane_calc_min_cdclk(): plane->min_cdclk is
 * icl_plane_min_cdclk on display version 11 and later
 * (skl_universal_plane_create()).
 */
void
drv_i915_lcd_ms_plane_min_cdclk(
	struct i915_lcd_modeset *ms)
{
	/* Stores the plane's need in the crtc state. */
	ms->crtc_state.min_cdclk[ms->plane.id] = i915_icl_plane_min_cdclk(&ms->crtc_state, &ms->plane_state);
}

/*
 * Updates the running crtc of a modeset object to its plane's new state
 * (a synchronous flip).
 *
 * The Linux update of a running crtc (intel_update_crtc()) for its one
 * primary plane: intel_crtc_planes_update_noarm(),
 * intel_pipe_update_start(), intel_crtc_planes_update_arm(),
 * intel_pipe_update_end(); commit_pipe_pre_planes() and
 * commit_pipe_post_planes() have nothing to do for a plane-only update.
 * The old and new crtc state are the object's own (same timings), and the
 * update arms the object's flip event.
 */
void
drv_i915_lcd_ms_plane_update_flip(
	struct i915_lcd_modeset *ms)
{
	struct i915_lcd_world *world;

	/* The world the object belongs to. */
	world = ms->world;

	/* The object's device and pipe are the ones the update works on and sleeps for. */
	world->i915_lcd_cur_i915 = &ms->i915;
	world->i915_lcd_flip_pipe = ms->crtc.pipe;

	/* The update's atomic state: same timings on both sides, not a modeset. */
	ms->state.base.dev = &ms->i915.drm;
	ms->state.crtc_state = &ms->crtc_state;
	ms->state.old_crtc_state = &ms->crtc_state;
	ms->crtc_state.uapi.mode_changed = false;

	/* The completion the update arms. */
	ms->crtc_state.uapi.event = &ms->flip_event;

	/* The unarmed registers, then the arming writes inside the vblank evasion. */
	i915_icl_plane_update_noarm(&ms->plane, &ms->crtc_state, &ms->plane_state);
	drv_i915_pipe_update_start(world, &ms->state, &ms->crtc);
	i915_icl_plane_update_arm(&ms->plane, &ms->crtc_state, &ms->plane_state);
	drv_i915_pipe_update_end(world, &ms->state, &ms->crtc);

	/* The event is armed; the state no longer carries it. */
	ms->crtc_state.uapi.event = 0;
}

/*
 * Returns the readout hook of a plane (the Linux skl_plane_get_hw_state()).
 *
 * The hook is static in this file, as in Linux; the takeover registry
 * binds it.
 */
bool
(*i915_lcd_plane_get_hw_state(void))(struct intel_plane *plane, enum pipe *pipe)
{
	/* Succeeded: reports the hook. */
	return i915_skl_plane_get_hw_state;
}

/*
 * Disables a plane for the takeover (the Linux plane->disable_arm() hook,
 * icl_plane_disable_arm()).
 *
 * The Linux intel_plane_disable_noatomic() calls the plane's disable_arm
 * hook; this path has that text and reaches it here, so the takeover
 * really stops the plane.
 */
void
drv_i915_lcd_plane_disable_arm(
	struct intel_plane *plane,
	const struct intel_crtc_state *crtc_state)
{
	/* Writes the disable. */
	i915_icl_plane_disable_arm(plane, crtc_state);
}

/*
 * Records the data rates of the plane of a modeset object.
 *
 * The two assignments of the Linux intel_plane_atomic_check_with_state()
 * that give a visible plane its data rates; the DDB allocation weighs the
 * planes by rel_data_rate.
 */
void
drv_i915_lcd_ms_plane_data_rates(
	struct i915_lcd_modeset *ms)
{
	struct intel_crtc_state *cs;

	/* The object's crtc state. */
	cs = &ms->crtc_state;

	/* The data rate and the relative data rate of the primary plane's colour plane 0. */
	cs->data_rate[ms->plane.id] = drv_i915_plane_data_rate(cs, &ms->plane_state, 0);
	cs->rel_data_rate[ms->plane.id] = i915_plane_relative_data_rate(cs, &ms->plane_state, 0);
}

/* Tells whether a plane has the HDR pipeline (the Linux icl_is_hdr_plane()). */
static bool
i915_icl_is_hdr_plane(
	struct drm_i915_private *dev_priv,
	enum plane_id plane_id)
{
	u8 hdr_planes;

	UNUSED_PARAMETER(dev_priv);

	/* Only display version 11 and later have HDR planes. */
	if (drv_i915_lcd_display_ver() < 11)
		return false;

	/* The plane is one of them or not. */
	hdr_planes = drv_i915_icl_hdr_plane_mask();
	if ((hdr_planes & BIT(plane_id)) == 0)
		return false;

	/* Succeeded: the plane has the HDR pipeline. */
	return true;
}

/*
 * Returns the unit the stride register counts in (the Linux
 * skl_plane_stride_mult()): 64-byte chunks for a linear buffer, tiles for
 * a tiled one.
 */
static unsigned int
i915_skl_plane_stride_mult(
	const struct drm_framebuffer *fb,
	int color_plane,
	unsigned int rotation)
{
	bool rotated;

	UNUSED_PARAMETER(color_plane);

	/* A linear buffer counts 64-byte chunks. */
	if (is_surface_linear(fb, color_plane))
		return 64;

	/* A tiled buffer counts tiles, across or down depending on the rotation. */
	rotated = rotation_90_or_270(rotation);
	if (rotated)
		return intel_tile_height(fb, color_plane);

	/* Succeeded: the tile width in bytes. */
	return intel_tile_width_bytes(fb, color_plane);
}

/* Returns the stride register value of a colour plane (the Linux skl_plane_stride()). */
static u32
i915_skl_plane_stride(
	const struct intel_plane_state *plane_state,
	int color_plane)
{
	const struct drm_framebuffer *fb;
	unsigned int rotation;
	u32 stride;
	unsigned int mult;

	/* The framebuffer, the rotation and the colour plane's stride in bytes. */
	fb = plane_state->hw.fb;
	rotation = plane_state->hw.rotation;
	stride = plane_state->view.color_plane[color_plane].scanout_stride;

	/* A colour plane the format does not have has no stride. */
	if (color_plane >= fb->format->num_planes)
		return 0;

	/* The unit of the register. */
	mult = i915_skl_plane_stride_mult(fb, color_plane, rotation);

	/* Succeeded: reports the stride in that unit. */
	return stride / mult;
}

/* Returns the PLANE_CTL format bits of a fourcc (the Linux skl_plane_ctl_format()). */
static u32
i915_skl_plane_ctl_format(
	u32 pixel_format)
{
	/* Maps the fourcc to the plane's format and channel order. */
	switch (pixel_format) {
	case FORMAT_C8:
		return PLANE_CTL_FORMAT_INDEXED;
	case FORMAT_RGB565:
		return PLANE_CTL_FORMAT_RGB_565;
	case FORMAT_XBGR8888:
	case FORMAT_ABGR8888:
		return PLANE_CTL_FORMAT_XRGB_8888 | PLANE_CTL_ORDER_RGBX;
	case FORMAT_XRGB8888:
	case FORMAT_ARGB8888:
		return PLANE_CTL_FORMAT_XRGB_8888;
	case FORMAT_XBGR2101010:
	case FORMAT_ABGR2101010:
		return PLANE_CTL_FORMAT_XRGB_2101010 | PLANE_CTL_ORDER_RGBX;
	case FORMAT_XRGB2101010:
	case FORMAT_ARGB2101010:
		return PLANE_CTL_FORMAT_XRGB_2101010;
	case FORMAT_XBGR16161616F:
	case FORMAT_ABGR16161616F:
		return PLANE_CTL_FORMAT_XRGB_16161616F | PLANE_CTL_ORDER_RGBX;
	case FORMAT_XRGB16161616F:
	case FORMAT_ARGB16161616F:
		return PLANE_CTL_FORMAT_XRGB_16161616F;
	case FORMAT_XYUV8888:
		return PLANE_CTL_FORMAT_XYUV;
	case FORMAT_YUYV:
		return PLANE_CTL_FORMAT_YUV422 | PLANE_CTL_YUV422_ORDER_YUYV;
	case FORMAT_YVYU:
		return PLANE_CTL_FORMAT_YUV422 | PLANE_CTL_YUV422_ORDER_YVYU;
	case FORMAT_UYVY:
		return PLANE_CTL_FORMAT_YUV422 | PLANE_CTL_YUV422_ORDER_UYVY;
	case FORMAT_VYUY:
		return PLANE_CTL_FORMAT_YUV422 | PLANE_CTL_YUV422_ORDER_VYUY;
	case FORMAT_NV12:
		return PLANE_CTL_FORMAT_NV12;
	case FORMAT_P010:
		return PLANE_CTL_FORMAT_P010;
	case FORMAT_P012:
		return PLANE_CTL_FORMAT_P012;
	case FORMAT_P016:
		return PLANE_CTL_FORMAT_P016;
	case FORMAT_Y210:
		return PLANE_CTL_FORMAT_Y210;
	case FORMAT_Y212:
		return PLANE_CTL_FORMAT_Y212;
	case FORMAT_Y216:
		return PLANE_CTL_FORMAT_Y216;
	case FORMAT_XVYU2101010:
		return PLANE_CTL_FORMAT_Y410;
	case FORMAT_XVYU12_16161616:
		return PLANE_CTL_FORMAT_Y412;
	case FORMAT_XVYU16161616:
		return PLANE_CTL_FORMAT_Y416;
	default:
		I915_LCD_MISSING_CASE(pixel_format);
	}

	/* An unknown format has no format bits. */
	return 0;
}

/* Returns the PLANE_CTL alpha bits (the Linux skl_plane_ctl_alpha(), display version < 10). */
static u32
i915_skl_plane_ctl_alpha(
	const struct intel_plane_state *plane_state)
{
	/* A format without alpha has no alpha blending. */
	if (!plane_state->hw.fb->format->has_alpha)
		return PLANE_CTL_ALPHA_DISABLE;

	/* Maps the blend mode. */
	switch (plane_state->hw.pixel_blend_mode) {
	case MODE_BLEND_PIXEL_NONE:
		return PLANE_CTL_ALPHA_DISABLE;
	case MODE_BLEND_PREMULTI:
		return PLANE_CTL_ALPHA_SW_PREMULTIPLY;
	case MODE_BLEND_COVERAGE:
		return PLANE_CTL_ALPHA_HW_PREMULTIPLY;
	default:
		I915_LCD_MISSING_CASE(plane_state->hw.pixel_blend_mode);
		return PLANE_CTL_ALPHA_DISABLE;
	}
}

/* Returns the PLANE_COLOR_CTL alpha bits (the Linux glk_plane_color_ctl_alpha()). */
static u32
i915_glk_plane_color_ctl_alpha(
	const struct intel_plane_state *plane_state)
{
	/* A format without alpha has no alpha blending. */
	if (!plane_state->hw.fb->format->has_alpha)
		return PLANE_COLOR_ALPHA_DISABLE;

	/* Maps the blend mode. */
	switch (plane_state->hw.pixel_blend_mode) {
	case MODE_BLEND_PIXEL_NONE:
		return PLANE_COLOR_ALPHA_DISABLE;
	case MODE_BLEND_PREMULTI:
		return PLANE_COLOR_ALPHA_SW_PREMULTIPLY;
	case MODE_BLEND_COVERAGE:
		return PLANE_COLOR_ALPHA_HW_PREMULTIPLY;
	default:
		I915_LCD_MISSING_CASE(plane_state->hw.pixel_blend_mode);
		return PLANE_COLOR_ALPHA_DISABLE;
	}
}

/* Returns the PLANE_CTL tiling and decompression bits of a modifier (the Linux skl_plane_ctl_tiling()). */
static u32
i915_skl_plane_ctl_tiling(
	u64 fb_modifier)
{
	/* Maps the modifier. */
	switch (fb_modifier) {
	case FORMAT_MOD_LINEAR:
		break;
	case I915_FORMAT_MOD_X_TILED:
		return PLANE_CTL_TILED_X;
	case I915_FORMAT_MOD_Y_TILED:
		return PLANE_CTL_TILED_Y;
	case I915_FORMAT_MOD_4_TILED:
		return PLANE_CTL_TILED_4;
	case I915_FORMAT_MOD_4_TILED_DG2_RC_CCS:
		return PLANE_CTL_TILED_4 | PLANE_CTL_RENDER_DECOMPRESSION_ENABLE | PLANE_CTL_CLEAR_COLOR_DISABLE;
	case I915_FORMAT_MOD_4_TILED_DG2_MC_CCS:
		return PLANE_CTL_TILED_4 | PLANE_CTL_MEDIA_DECOMPRESSION_ENABLE | PLANE_CTL_CLEAR_COLOR_DISABLE;
	case I915_FORMAT_MOD_4_TILED_DG2_RC_CCS_CC:
		return PLANE_CTL_TILED_4 | PLANE_CTL_RENDER_DECOMPRESSION_ENABLE;
	case I915_FORMAT_MOD_4_TILED_MTL_RC_CCS:
		return PLANE_CTL_TILED_4 | PLANE_CTL_RENDER_DECOMPRESSION_ENABLE | PLANE_CTL_CLEAR_COLOR_DISABLE;
	case I915_FORMAT_MOD_4_TILED_MTL_RC_CCS_CC:
		return PLANE_CTL_TILED_4 | PLANE_CTL_RENDER_DECOMPRESSION_ENABLE;
	case I915_FORMAT_MOD_4_TILED_MTL_MC_CCS:
		return PLANE_CTL_TILED_4 | PLANE_CTL_MEDIA_DECOMPRESSION_ENABLE;
	case I915_FORMAT_MOD_Y_TILED_CCS:
	case I915_FORMAT_MOD_Y_TILED_GEN12_RC_CCS_CC:
		return PLANE_CTL_TILED_Y | PLANE_CTL_RENDER_DECOMPRESSION_ENABLE;
	case I915_FORMAT_MOD_Y_TILED_GEN12_RC_CCS:
		return PLANE_CTL_TILED_Y | PLANE_CTL_RENDER_DECOMPRESSION_ENABLE | PLANE_CTL_CLEAR_COLOR_DISABLE;
	case I915_FORMAT_MOD_Y_TILED_GEN12_MC_CCS:
		return PLANE_CTL_TILED_Y | PLANE_CTL_MEDIA_DECOMPRESSION_ENABLE;
	case I915_FORMAT_MOD_Yf_TILED:
		return PLANE_CTL_TILED_YF;
	case I915_FORMAT_MOD_Yf_TILED_CCS:
		return PLANE_CTL_TILED_YF | PLANE_CTL_RENDER_DECOMPRESSION_ENABLE;
	default:
		I915_LCD_MISSING_CASE(fb_modifier);
	}

	/* A linear or unknown modifier has no tiling bits. */
	return 0;
}

/*
 * Returns the PLANE_CTL rotation bits (the Linux skl_plane_ctl_rotate()).
 *
 * DRM_MODE_ROTATE_ is counter clockwise to stay compatible with Xrandr,
 * while the hardware rotates clockwise: 90 and 270 swap.
 */
static u32
i915_skl_plane_ctl_rotate(
	unsigned int rotate)
{
	/* Maps the rotation. */
	switch (rotate) {
	case MODE_ROTATE_0:
		break;
	case MODE_ROTATE_90:
		return PLANE_CTL_ROTATE_270;
	case MODE_ROTATE_180:
		return PLANE_CTL_ROTATE_180;
	case MODE_ROTATE_270:
		return PLANE_CTL_ROTATE_90;
	default:
		I915_LCD_MISSING_CASE(rotate);
	}

	/* No rotation, or an unknown one, has no rotation bits. */
	return 0;
}

/* Returns the PLANE_CTL reflection bits (the Linux icl_plane_ctl_flip()). */
static u32
i915_icl_plane_ctl_flip(
	unsigned int reflect)
{
	/* Maps the reflection; only the horizontal one exists. */
	switch (reflect) {
	case 0:
		break;
	case MODE_REFLECT_X:
		return PLANE_CTL_FLIP_HORIZONTAL;
	case MODE_REFLECT_Y:
	default:
		I915_LCD_MISSING_CASE(reflect);
	}

	/* No reflection, or an unsupported one, has no bits. */
	return 0;
}

/* Returns the ADL-P arbiter slots of a plane (the Linux adlp_plane_ctl_arb_slots(), Wa_22012358565). */
static u32
i915_adlp_plane_ctl_arb_slots(
	const struct intel_plane_state *plane_state)
{
	const struct drm_framebuffer *fb;

	/* The framebuffer. */
	fb = plane_state->hw.fb;

	/* A semiplanar YUV buffer: one slot for 2-byte luma. */
	if (intel_format_info_is_yuv_semiplanar(fb->format, fb->modifier)) {
		/* Picks the slots by the luma's bytes per pixel. */
		switch (fb->format->cpp[0]) {
		case 2:
			return PLANE_CTL_ARB_SLOTS(1);
		default:
			return PLANE_CTL_ARB_SLOTS(0);
		}
	}

	/* Picks the slots by the bytes per pixel: 3 for 8, 1 for 4. */
	switch (fb->format->cpp[0]) {
	case 8:
		return PLANE_CTL_ARB_SLOTS(3);
	case 4:
		return PLANE_CTL_ARB_SLOTS(1);
	default:
		return PLANE_CTL_ARB_SLOTS(0);
	}
}

/* Returns the pipe gamma and CSC bits of PLANE_CTL (the Linux skl_plane_ctl_crtc(), display version < 10). */
static u32
i915_skl_plane_ctl_crtc(
	const struct intel_crtc_state *crtc_state)
{
	u32 plane_ctl;

	/* Starts from no bit. */
	plane_ctl = 0;

	/* Display version 10 and later have these bits in PLANE_COLOR_CTL. */
	if (drv_i915_lcd_display_ver() >= 10)
		return plane_ctl;

	/* The pipe gamma and CSC. */
	if (crtc_state->gamma_enable)
		plane_ctl |= PLANE_CTL_PIPE_GAMMA_ENABLE;
	if (crtc_state->csc_enable)
		plane_ctl |= PLANE_CTL_PIPE_CSC_ENABLE;

	/* Succeeded: reports the bits. */
	return plane_ctl;
}

/*
 * Computes the PLANE_CTL word of a plane state (the Linux skl_plane_ctl()).
 *
 * Enable, format, tiling, rotation, reflection, colour key and, on display
 * version 13, the arbiter slots.
 */
static u32
i915_skl_plane_ctl(
	const struct intel_crtc_state *crtc_state,
	const struct intel_plane_state *plane_state)
{
	const struct drm_framebuffer *fb;
	unsigned int rotation;
	const struct intel_sprite_colorkey *key;
	u32 plane_ctl;
	int display_ver;

	UNUSED_PARAMETER(crtc_state);

	/* The display version of the plane's device, the framebuffer, the rotation and the key. */
	display_ver = drv_i915_lcd_display_ver();
	fb = plane_state->hw.fb;
	rotation = plane_state->hw.rotation;
	key = &plane_state->ckey;

	/* The plane is enabled. */
	plane_ctl = PLANE_CTL_ENABLE;

	/* Before display version 10 alpha, gamma and the YUV conversion are in PLANE_CTL. */
	if (display_ver < 10) {
		plane_ctl |= i915_skl_plane_ctl_alpha(plane_state);
		plane_ctl |= PLANE_CTL_PLANE_GAMMA_DISABLE;

		/* The BT.709 YUV conversion. */
		if (plane_state->hw.color_encoding == COLOR_YCBCR_BT709)
			plane_ctl |= PLANE_CTL_YUV_TO_RGB_CSC_FORMAT_BT709;

		/* Full-range YUV needs no range correction. */
		if (plane_state->hw.color_range == COLOR_YCBCR_FULL_RANGE)
			plane_ctl |= PLANE_CTL_YUV_RANGE_CORRECTION_DISABLE;
	}

	/* The format, the tiling and the rotation. */
	plane_ctl |= i915_skl_plane_ctl_format(fb->format->format);
	plane_ctl |= i915_skl_plane_ctl_tiling(fb->modifier);
	plane_ctl |= i915_skl_plane_ctl_rotate(rotation & MODE_ROTATE_MASK);

	/* The reflection (display version 11 and later). */
	if (display_ver >= 11)
		plane_ctl |= i915_icl_plane_ctl_flip(rotation & MODE_REFLECT_MASK);

	/* The colour key. */
	if (key->flags & I915_SET_COLORKEY_DESTINATION) {
		plane_ctl |= PLANE_CTL_KEY_ENABLE_DESTINATION;
	} else if (key->flags & I915_SET_COLORKEY_SOURCE) {
		plane_ctl |= PLANE_CTL_KEY_ENABLE_SOURCE;
	}

	/* Wa_22012358565:adl-p */
	if (display_ver == 13)
		plane_ctl |= i915_adlp_plane_ctl_arb_slots(plane_state);

	/* Succeeded: reports the word. */
	return plane_ctl;
}

/* Returns the pipe gamma and CSC bits of PLANE_COLOR_CTL (the Linux glk_plane_color_ctl_crtc(), display version < 11). */
static u32
i915_glk_plane_color_ctl_crtc(
	const struct intel_crtc_state *crtc_state)
{
	u32 plane_color_ctl;

	/* Starts from no bit. */
	plane_color_ctl = 0;

	/* Display version 11 and later take these from the pipe. */
	if (drv_i915_lcd_display_ver() >= 11)
		return plane_color_ctl;

	/* The pipe gamma and CSC. */
	if (crtc_state->gamma_enable)
		plane_color_ctl |= PLANE_COLOR_PIPE_GAMMA_ENABLE;
	if (crtc_state->csc_enable)
		plane_color_ctl |= PLANE_COLOR_PIPE_CSC_ENABLE;

	/* Succeeded: reports the bits. */
	return plane_color_ctl;
}

/*
 * Computes the PLANE_COLOR_CTL word of a plane state (the Linux
 * glk_plane_color_ctl()).
 *
 * Plane gamma off, the alpha mode, the YUV conversion of a YUV format
 * (through the plane CSC on a non-HDR plane, the input CSC on an HDR
 * plane) and the forced-black CSC.
 */
static u32
i915_glk_plane_color_ctl(
	const struct intel_crtc_state *crtc_state,
	const struct intel_plane_state *plane_state)
{
	struct drm_i915_private *dev_priv;
	const struct drm_framebuffer *fb;
	struct intel_plane *plane;
	u32 plane_color_ctl;
	bool hdr;

	UNUSED_PARAMETER(crtc_state);

	/* Finds the device, the framebuffer and the plane. */
	dev_priv = i915_lcd_to_i915(plane_state->uapi.plane->dev);
	fb = plane_state->hw.fb;
	plane = to_intel_plane(plane_state->uapi.plane);
	plane_color_ctl = 0;

	/* Plane gamma off and the alpha mode. */
	plane_color_ctl |= PLANE_COLOR_PLANE_GAMMA_DISABLE;
	plane_color_ctl |= i915_glk_plane_color_ctl_alpha(plane_state);

	/* A YUV format is converted by the plane CSC, or by the input CSC of an HDR plane. */
	if (fb->format->is_yuv) {
		hdr = i915_icl_is_hdr_plane(dev_priv, plane->id);
		if (!hdr) {
			/* Picks the conversion of the colour encoding. */
			switch (plane_state->hw.color_encoding) {
			case COLOR_YCBCR_BT709:
				plane_color_ctl |= PLANE_COLOR_CSC_MODE_YUV709_TO_RGB709;
				break;
			case COLOR_YCBCR_BT2020:
				plane_color_ctl |= PLANE_COLOR_CSC_MODE_YUV2020_TO_RGB2020;
				break;
			default:
				plane_color_ctl |= PLANE_COLOR_CSC_MODE_YUV601_TO_RGB601;
			}

			/* Full-range YUV needs no range correction. */
			if (plane_state->hw.color_range == COLOR_YCBCR_FULL_RANGE)
				plane_color_ctl |= PLANE_COLOR_YUV_RANGE_CORRECTION_DISABLE;
		} else {
			plane_color_ctl |= PLANE_COLOR_INPUT_CSC_ENABLE;
			if (plane_state->hw.color_range == COLOR_YCBCR_FULL_RANGE)
				plane_color_ctl |= PLANE_COLOR_YUV_RANGE_CORRECTION_DISABLE;
		}
	}

	/* A forced-black plane goes through the plane CSC. */
	if (plane_state->force_black)
		plane_color_ctl |= PLANE_COLOR_PLANE_CSC_ENABLE;

	/* Succeeded: reports the word. */
	return plane_color_ctl;
}

/*
 * Returns the surface offset of a colour plane (the Linux skl_surf_address()).
 *
 * A DPT framebuffer's offset is in 512-byte units; otherwise the offset
 * must be page aligned.  Linear framebuffers here never use a DPT.
 */
static u32
i915_skl_surf_address(
	const struct intel_plane_state *plane_state,
	int color_plane)
{
	u32 offset;

	/* The colour plane's offset (the warnings name the plane's DRM device). */
	offset = plane_state->view.color_plane[color_plane].offset;

	/* The DPT object contains only one vma, so its offset within the DPT is always 0. */
	if (intel_fb_uses_dpt(plane_state->hw.fb)) {
		I915_LCD_DRM_WARN_ON(plane_state->uapi.plane->dev, plane_state->dpt_vma && plane_state->dpt_vma->node.start);
		I915_LCD_DRM_WARN_ON(plane_state->uapi.plane->dev, offset & 0x1fffff);
		return offset >> 9;
	}

	/* The offset must be page aligned. */
	I915_LCD_DRM_WARN_ON(plane_state->uapi.plane->dev, offset & 0xfff);

	/* Succeeded: reports the offset. */
	return offset;
}

/* Returns the PLANE_SURF word of a colour plane (the Linux skl_plane_surf()). */
static u32
i915_skl_plane_surf(
	const struct intel_plane_state *plane_state,
	int color_plane)
{
	u32 plane_surf;
	u32 address;

	/* The buffer's GGTT address plus the colour plane's offset. */
	address = i915_skl_surf_address(plane_state, color_plane);
	plane_surf = intel_plane_ggtt_offset(plane_state) + address;

	/* A protected buffer is decrypted by the plane. */
	if (plane_state->decrypt)
		plane_surf |= PLANE_SURF_DECRYPT;

	/* Succeeded: reports the word. */
	return plane_surf;
}

/*
 * Returns the PLANE_AUX_DIST word of a colour plane (the Linux
 * skl_plane_aux_dist()).
 *
 * The distance from the main surface to its CCS aux surface; 0 without an
 * aux plane (always here: linear framebuffers have none).
 */
static u32
i915_skl_plane_aux_dist(
	const struct intel_plane_state *plane_state,
	int color_plane)
{
	int aux_plane;
	u32 aux_dist;
	u32 aux_address;
	u32 main_address;
	u32 aux_stride;

	/* The aux plane of the colour plane. */
	aux_plane = skl_main_to_aux_plane(plane_state->hw.fb, color_plane);
	if (!aux_plane)
		return 0;

	/* The distance between the two surfaces. */
	aux_address = i915_skl_surf_address(plane_state, aux_plane);
	main_address = i915_skl_surf_address(plane_state, color_plane);
	aux_dist = aux_address - main_address;

	/* Before display version 12 the aux stride is in the same word. */
	if (drv_i915_lcd_display_ver() < 12) {
		aux_stride = i915_skl_plane_stride(plane_state, aux_plane);
		aux_dist |= PLANE_AUX_STRIDE(aux_stride);
	}

	/* Succeeded: reports the word. */
	return aux_dist;
}

/* Returns the PLANE_KEYVAL word (the Linux skl_plane_keyval()): the key's minimum. */
static u32
i915_skl_plane_keyval(
	const struct intel_plane_state *plane_state)
{
	/* Succeeded: reports the colour key's minimum value. */
	return plane_state->ckey.min_value;
}

/* Returns the PLANE_KEYMSK word (the Linux skl_plane_keymsk()): the channel mask and the alpha enable. */
static u32
i915_skl_plane_keymsk(
	const struct intel_plane_state *plane_state)
{
	const struct intel_sprite_colorkey *key;
	u8 alpha;
	u32 keymsk;

	/* The colour key and the plane alpha. */
	key = &plane_state->ckey;
	alpha = plane_state->hw.alpha >> 8;

	/* The channel mask of the key. */
	keymsk = key->channel_mask & 0x7ffffff;

	/* A translucent plane enables the constant alpha. */
	if (alpha < 0xff)
		keymsk |= PLANE_KEYMSK_ALPHA_ENABLE;

	/* Succeeded: reports the word. */
	return keymsk;
}

/* Returns the PLANE_KEYMAX word (the Linux skl_plane_keymax()): the key's maximum and the plane alpha. */
static u32
i915_skl_plane_keymax(
	const struct intel_plane_state *plane_state)
{
	const struct intel_sprite_colorkey *key;
	u8 alpha;

	/* The colour key and the plane alpha. */
	key = &plane_state->ckey;
	alpha = plane_state->hw.alpha >> 8;

	/* Succeeded: reports the word. */
	return (key->max_value & 0xffffff) | PLANE_KEYMAX_ALPHA(alpha);
}

/* Returns the colour plane a plane programs (the Linux icl_plane_color_plane()): UV on a planar master. */
static int
i915_icl_plane_color_plane(
	const struct intel_plane_state *plane_state)
{
	/* A planar master programs the UV plane. */
	if (plane_state->planar_linked_plane && !plane_state->planar_slave)
		return 1;

	/* Succeeded: every other plane programs colour plane 0. */
	return 0;
}

/*
 * Writes the PSR2 selective fetch area of a plane (the Linux
 * icl_plane_update_sel_fetch_noarm()); nothing without selective fetch.
 */
static void
i915_icl_plane_update_sel_fetch_noarm(
	struct intel_plane *plane,
	const struct intel_crtc_state *crtc_state,
	const struct intel_plane_state *plane_state,
	int color_plane)
{
	struct drm_i915_private *i915;
	enum pipe pipe;
	const struct drm_rect *clip;
	u32 val;
	int x;
	int y;

	/* Finds the device and the pipe. */
	i915 = i915_lcd_to_i915(plane->base.dev);
	pipe = plane->pipe;

	/* Without selective fetch there is nothing to write. */
	if (!crtc_state->enable_psr2_sel_fetch)
		return;

	/* The area to fetch. */
	clip = &plane_state->psr2_sel_fetch_area;

	/* The position of the fetched area. */
	val = (clip->y1 + plane_state->uapi.dst.y1) << 16;
	val |= plane_state->uapi.dst.x1;
	i915_lcd_intel_de_write_fw(i915, PLANE_SEL_FETCH_POS(pipe, plane->id), val);

	/* The offset: the UV surface starts at half the Y plane's start line (Bspec). */
	x = plane_state->view.color_plane[color_plane].x;
	if (!color_plane) {
		y = plane_state->view.color_plane[color_plane].y + clip->y1;
	} else {
		y = plane_state->view.color_plane[color_plane].y + clip->y1 / 2;
	}

	/* Writes the offset. */
	val = y << 16 | x;
	i915_lcd_intel_de_write_fw(i915, PLANE_SEL_FETCH_OFFSET(pipe, plane->id), val);

	/* The size (0 based). */
	val = (i915_drm_rect_height(clip) - 1) << 16;
	val |= (i915_drm_rect_width(&plane_state->uapi.src) >> 16) - 1;
	i915_lcd_intel_de_write_fw(i915, PLANE_SEL_FETCH_SIZE(pipe, plane->id), val);
}

/*
 * Writes the plane registers that do not arm the update (the Linux
 * icl_plane_update_noarm()).
 *
 * Stride, position, size, colour key, offset, aux distance, CUS control,
 * colour control, the input CSC, the watermarks, the black CSC and the
 * selective fetch area, in this order.
 */
static void
i915_icl_plane_update_noarm(
	struct intel_plane *plane,
	const struct intel_crtc_state *crtc_state,
	const struct intel_plane_state *plane_state)
{
	struct drm_i915_private *dev_priv;
	enum plane_id plane_id;
	enum pipe pipe;
	int color_plane;
	u32 stride;
	const struct drm_framebuffer *fb;
	int crtc_x;
	int crtc_y;
	int x;
	int y;
	int src_w;
	int src_h;
	u32 plane_color_ctl;
	u32 key_word;
	u32 aux_dist;
	bool hdr;

	/* Finds the device, the plane, the colour plane, the stride and the geometry. */
	dev_priv = i915_lcd_to_i915(plane->base.dev);
	plane_id = plane->id;
	pipe = plane->pipe;
	color_plane = i915_icl_plane_color_plane(plane_state);
	stride = i915_skl_plane_stride(plane_state, color_plane);
	fb = plane_state->hw.fb;
	crtc_x = plane_state->uapi.dst.x1;
	crtc_y = plane_state->uapi.dst.y1;
	x = plane_state->view.color_plane[color_plane].x;
	y = plane_state->view.color_plane[color_plane].y;
	src_w = i915_drm_rect_width(&plane_state->uapi.src) >> 16;
	src_h = i915_drm_rect_height(&plane_state->uapi.src) >> 16;

	/* The colour control with the pipe's bits. */
	plane_color_ctl = plane_state->color_ctl | i915_glk_plane_color_ctl_crtc(crtc_state);

	/* The scaler handles the output position. */
	if (plane_state->scaler_id >= 0) {
		crtc_x = 0;
		crtc_y = 0;
	}

	/* The stride, the position and the size. */
	i915_lcd_intel_de_write_fw(dev_priv, PLANE_STRIDE(pipe, plane_id), PLANE_STRIDE_(stride));
	i915_lcd_intel_de_write_fw(dev_priv, PLANE_POS(pipe, plane_id), PLANE_POS_Y(crtc_y) | PLANE_POS_X(crtc_x));
	i915_lcd_intel_de_write_fw(dev_priv, PLANE_SIZE(pipe, plane_id), PLANE_HEIGHT(src_h - 1) | PLANE_WIDTH(src_w - 1));

	/* The colour key: its value, its mask and its maximum. */
	key_word = i915_skl_plane_keyval(plane_state);
	i915_lcd_intel_de_write_fw(dev_priv, PLANE_KEYVAL(pipe, plane_id), key_word);
	key_word = i915_skl_plane_keymsk(plane_state);
	i915_lcd_intel_de_write_fw(dev_priv, PLANE_KEYMSK(pipe, plane_id), key_word);
	key_word = i915_skl_plane_keymax(plane_state);
	i915_lcd_intel_de_write_fw(dev_priv, PLANE_KEYMAX(pipe, plane_id), key_word);

	/* The offset inside the buffer. */
	i915_lcd_intel_de_write_fw(dev_priv, PLANE_OFFSET(pipe, plane_id), PLANE_OFFSET_Y(y) | PLANE_OFFSET_X(x));

	/* The clear colour of a CCS-CC buffer. */
	if (intel_fb_is_rc_ccs_cc_modifier(fb->modifier)) {
		i915_lcd_intel_de_write_fw(dev_priv, PLANE_CC_VAL(pipe, plane_id, 0), lower_32_bits(plane_state->ccval));
		i915_lcd_intel_de_write_fw(dev_priv, PLANE_CC_VAL(pipe, plane_id, 1), upper_32_bits(plane_state->ccval));
	}

	/* FLAT CCS doesn't need to program AUX_DIST. */
	if (!HAS_FLAT_CCS(dev_priv) && drv_i915_lcd_display_ver() < 20) {
		aux_dist = i915_skl_plane_aux_dist(plane_state, color_plane);
		i915_lcd_intel_de_write_fw(dev_priv, PLANE_AUX_DIST(pipe, plane_id), aux_dist);
	}

	/* The chroma upsampler of an HDR plane. */
	hdr = i915_icl_is_hdr_plane(dev_priv, plane_id);
	if (hdr)
		i915_lcd_intel_de_write_fw(dev_priv, PLANE_CUS_CTL(pipe, plane_id), plane_state->cus_ctl);

	/* The colour control. */
	i915_lcd_intel_de_write_fw(dev_priv, PLANE_COLOR_CTL(pipe, plane_id), plane_color_ctl);

	/* The input CSC of a YUV buffer on an HDR plane. */
	if (fb->format->is_yuv && hdr)
		icl_program_input_csc(plane, crtc_state, plane_state);

	/* The watermarks and DDB of the plane. */
	skl_write_plane_wm(plane, crtc_state);

	/*
	 * FIXME: a PXP session invalidation can hit at any time, even during
	 * or after the commit, and the display content is garbage then.
	 */
	if (plane_state->force_black)
		icl_plane_csc_load_black(plane);

	/* The selective fetch area. */
	i915_icl_plane_update_sel_fetch_noarm(plane, crtc_state, plane_state, color_plane);
}

/* Disables the PSR2 selective fetch of a plane (the Linux icl_plane_disable_sel_fetch_arm()). */
static void
i915_icl_plane_disable_sel_fetch_arm(
	struct intel_plane *plane,
	const struct intel_crtc_state *crtc_state)
{
	struct drm_i915_private *i915;
	enum pipe pipe;

	/* Finds the device and the pipe. */
	i915 = i915_lcd_to_i915(plane->base.dev);
	pipe = plane->pipe;

	/* Without selective fetch there is nothing to write. */
	if (!crtc_state->enable_psr2_sel_fetch)
		return;

	/* Turns the selective fetch off. */
	i915_lcd_intel_de_write_fw(i915, PLANE_SEL_FETCH_CTL(pipe, plane->id), 0);
}

/* Enables or disables the PSR2 selective fetch of a plane (the Linux icl_plane_update_sel_fetch_arm()). */
static void
i915_icl_plane_update_sel_fetch_arm(
	struct intel_plane *plane,
	const struct intel_crtc_state *crtc_state,
	const struct intel_plane_state *plane_state)
{
	struct drm_i915_private *i915;
	enum pipe pipe;

	/* Finds the device and the pipe. */
	i915 = i915_lcd_to_i915(plane->base.dev);
	pipe = plane->pipe;

	/* Without selective fetch there is nothing to write. */
	if (!crtc_state->enable_psr2_sel_fetch)
		return;

	/* An area to fetch turns the fetch on; an empty one turns it off. */
	if (i915_drm_rect_height(&plane_state->psr2_sel_fetch_area) > 0) {
		i915_lcd_intel_de_write_fw(i915, PLANE_SEL_FETCH_CTL(pipe, plane->id), PLANE_SEL_FETCH_CTL_ENABLE);
	} else {
		i915_icl_plane_disable_sel_fetch_arm(plane, crtc_state);
	}
}

/*
 * Writes the plane registers that arm the update (the Linux
 * icl_plane_update_arm()).
 *
 * The scaler is enabled before the plane so that no catastrophic underrun
 * happens even if the two end up in different frames.  PLANE_CTL
 * self-arms when the plane was disabled, so it is written just before
 * PLANE_SURF to make the enable atomic.
 */
static void
i915_icl_plane_update_arm(
	struct intel_plane *plane,
	const struct intel_crtc_state *crtc_state,
	const struct intel_plane_state *plane_state)
{
	struct drm_i915_private *dev_priv;
	enum plane_id plane_id;
	enum pipe pipe;
	int color_plane;
	u32 plane_ctl;
	u32 plane_surf;

	/* Finds the device, the plane and the colour plane. */
	dev_priv = i915_lcd_to_i915(plane->base.dev);
	plane_id = plane->id;
	pipe = plane->pipe;
	color_plane = i915_icl_plane_color_plane(plane_state);

	/* The control word with the pipe's bits. */
	plane_ctl = plane_state->ctl | i915_skl_plane_ctl_crtc(crtc_state);

	/* The scaler first (TODO in Linux: split into noarm+arm). */
	if (plane_state->scaler_id >= 0)
		skl_program_plane_scaler(plane, crtc_state, plane_state);

	/* The selective fetch. */
	i915_icl_plane_update_sel_fetch_arm(plane, crtc_state, plane_state);

	/* PLANE_CTL, then the arming PLANE_SURF. */
	i915_lcd_intel_de_write_fw(dev_priv, PLANE_CTL(pipe, plane_id), plane_ctl);
	plane_surf = i915_skl_plane_surf(plane_state, color_plane);
	i915_lcd_intel_de_write_fw(dev_priv, PLANE_SURF(pipe, plane_id), plane_surf);
}

/*
 * Disables a plane (the Linux icl_plane_disable_arm()).
 *
 * The CUS control of an HDR plane, the watermarks, the selective fetch,
 * then PLANE_CTL = 0 and the arming PLANE_SURF = 0.
 */
static void
i915_icl_plane_disable_arm(
	struct intel_plane *plane,
	const struct intel_crtc_state *crtc_state)
{
	struct drm_i915_private *dev_priv;
	enum plane_id plane_id;
	enum pipe pipe;
	bool hdr;

	/* Finds the device and the plane. */
	dev_priv = i915_lcd_to_i915(plane->base.dev);
	plane_id = plane->id;
	pipe = plane->pipe;

	/* The chroma upsampler of an HDR plane goes off. */
	hdr = i915_icl_is_hdr_plane(dev_priv, plane_id);
	if (hdr)
		i915_lcd_intel_de_write_fw(dev_priv, PLANE_CUS_CTL(pipe, plane_id), 0);

	/* The watermarks of the disabled plane. */
	skl_write_plane_wm(plane, crtc_state);

	/* The selective fetch, then the control and the arming surface. */
	i915_icl_plane_disable_sel_fetch_arm(plane, crtc_state);
	i915_lcd_intel_de_write_fw(dev_priv, PLANE_CTL(pipe, plane_id), 0);
	i915_lcd_intel_de_write_fw(dev_priv, PLANE_SURF(pipe, plane_id), 0);
}

/* Returns the CDCLK a plane needs (the Linux icl_plane_min_cdclk()): two pixels per clock. */
static int
i915_icl_plane_min_cdclk(
	const struct intel_crtc_state *crtc_state,
	const struct intel_plane_state *plane_state)
{
	unsigned int pixel_rate;

	/* The plane's pixel rate. */
	pixel_rate = drv_i915_plane_pixel_rate(crtc_state, plane_state);

	/* Succeeded: two pixels per clock, rounded up. */
	return DIV_ROUND_UP(pixel_rate, 2);
}

/*
 * Reads out whether a plane is enabled (the Linux skl_plane_get_hw_state(),
 * the plane's readout hook).
 *
 * A reference on the pipe's power domain is taken only where its well is
 * already on (on the plane's device, the readout's device while the
 * takeover runs); a pipe whose well is off has no enabled plane.
 */
static bool
i915_skl_plane_get_hw_state(
	struct intel_plane *plane,
	enum pipe *pipe)
{
	struct drm_i915_private *dev_priv;
	enum intel_display_power_domain power_domain;
	enum plane_id plane_id;
	intel_wakeref_t wakeref;
	u32 plane_ctl;
	bool ret;

	/* Finds the device, the pipe's power domain and the plane. */
	dev_priv = i915_lcd_to_i915(plane->base.dev);
	power_domain = POWER_DOMAIN_PIPE(plane->pipe);
	plane_id = plane->id;

	/* Holds the pipe's well only if it is on already. */
	wakeref = I915_TAKEOVER_INTEL_DISPLAY_POWER_GET_IF_ENABLED(dev_priv, power_domain);
	if (!wakeref)
		return false;

	/* Reads the enable bit and names the plane's pipe. */
	plane_ctl = i915_lcd_intel_de_read(dev_priv, PLANE_CTL(plane->pipe, plane_id));
	ret = false;
	if (plane_ctl & PLANE_CTL_ENABLE)
		ret = true;

	*pipe = plane->pipe;

	/* Gives the reference back. */
	i915_lcd_intel_display_power_put(dev_priv, power_domain, wakeref);

	/* Reports a disabled plane. */
	if (!ret)
		return false;

	/* Succeeded: the plane is enabled. */
	return true;
}

/*
 * Tells whether an async flip gives the plane its minimum DDB only (the
 * Linux use_min_ddb(), display version 13 and later).
 */
static bool
i915_use_min_ddb(
	const struct intel_crtc_state *crtc_state,
	struct intel_plane *plane)
{
	/* Only display version 13 and later. */
	if (drv_i915_lcd_display_ver() < 13)
		return false;

	/* Only an async flip on a plane that does them. */
	if (!crtc_state->uapi.async_flip)
		return false;
	if (!plane->async_flip)
		return false;

	/* Succeeded: the plane gets its minimum DDB. */
	return true;
}

/*
 * Returns the relative data rate a plane weighs in the DDB allocation
 * with (the Linux intel_plane_relative_data_rate()).
 *
 * The source is already rotated for 90/270 degrees (to match the GTT
 * mapping); the UV plane sub-samples by 2.  A cursor counts its bytes, any
 * other plane its bytes scaled by its downscaling.
 */
static unsigned int
i915_plane_relative_data_rate(
	const struct intel_crtc_state *crtc_state,
	const struct intel_plane_state *plane_state,
	int color_plane)
{
	struct intel_plane *plane;
	const struct drm_framebuffer *fb;
	int width;
	int height;
	unsigned int rel_data_rate;
	bool min_ddb;
	unsigned int adjusted;

	/* The plane and its framebuffer. */
	plane = to_intel_plane(plane_state->uapi.plane);
	fb = plane_state->hw.fb;

	/* An invisible plane weighs nothing. */
	if (!plane_state->uapi.visible)
		return 0;

	/* A plane that gets its minimum DDB is not weighed for the extra DDB. */
	min_ddb = i915_use_min_ddb(crtc_state, plane);
	if (min_ddb)
		return 0;

	/* The source size, halved for the UV plane. */
	width = i915_drm_rect_width(&plane_state->uapi.src) >> 16;
	height = i915_drm_rect_height(&plane_state->uapi.src) >> 16;
	if (color_plane == 1) {
		width /= 2;
		height /= 2;
	}

	/* The bytes of the source. */
	rel_data_rate = width * height * fb->format->cpp[color_plane];

	/* A cursor weighs its bytes. */
	if (plane->id == PLANE_CURSOR)
		return rel_data_rate;

	/* Scales the bytes by the downscaling. */
	adjusted = drv_i915_adjusted_rate(&plane_state->uapi.src, &plane_state->uapi.dst, rel_data_rate);

	/* Succeeded: reports the weight. */
	return adjusted;
}

/* Returns the description of the XRGB8888 format this path scans out. */
static const struct drm_format_info *
i915_xrgb8888_format(void)
{
	/* The one format of this path: 4 bytes per pixel, one plane, no alpha. */
	static const struct drm_format_info xrgb8888 = {
		FORMAT_XRGB8888,
		1,
		{ 4, 0, 0, 0 },
		false,
		false,
	};

	/* Succeeded: the description is constant and shared. */
	return &xrgb8888;
}
