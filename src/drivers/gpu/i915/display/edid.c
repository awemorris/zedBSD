/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Derived from the Linux kernel v6.8.12 (drivers/gpu/drm/drm_edid.c),
 * which carries the following notice.
 *
 * Copyright (c) 2006 Luc Verhaegen (quirks list)
 * Copyright (c) 2007-2008 Intel Corporation
 *   Jesse Barnes <jesse.barnes@intel.com>
 * Copyright 2010 Red Hat, Inc.
 *
 * DDC probing routines (drm_ddc_read & drm_do_probe_ddc_edid) originally from
 * FB layer.
 *   Copyright (C) 2006 Dennis Munsie <dmunsie@cecropia.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sub license,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

/*
 * Derived from the Linux kernel v6.8.12 (drivers/gpu/drm/drm_modes.c),
 * which carries the following notice.
 *
 * Copyright © 1997-2003 by The XFree86 Project, Inc.
 * Copyright © 2007 Dave Airlie
 * Copyright © 2007-2008 Intel Corporation
 *   Jesse Barnes <jesse.barnes@intel.com>
 * Copyright 2005-2006 Luc Verhaegen
 * Copyright (c) 2001, Andy Ritger  aritger@nvidia.com
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE COPYRIGHT HOLDER(S) OR AUTHOR(S) BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 *
 * Except as contained in this notice, the name of the copyright holder(s)
 * and author(s) shall not be used in advertising or otherwise to promote
 * the sale, use or other dealings in this Software without prior written
 * authorization from the copyright holder(s) and author(s).
 */

/*
 * The display modes of the one-screen modeset path (see edid.h).
 *
 * The panel's preferred mode is the first detailed timing descriptor of the
 * EDID base block whose pixel clock is not zero: EDID 1.3 with the
 * preferred-timing feature bit, and EDID 1.4 always, define the first
 * detailed timing as the preferred one, and a descriptor with a zero pixel
 * clock is a display descriptor, not a timing.  It is converted by the text
 * of the Linux 6.8.12 drm_mode_detailed() (drm_edid.c).  No EDID quirk is
 * applied: the quirk table is keyed by vendor and product and is not
 * carried, so display_info.quirks stays 0.
 *
 * The mode helpers the pipe, vblank and watermark text use follow the
 * Linux 6.8.12 drm_modes.c text: drm_mode_copy(), drm_mode_init(),
 * drm_mode_get_hv_timing() and drm_mode_set_crtcinfo().
 */

#include <uapi/errno.h>
#include <kern/kcrt.h>

#include "internal.h"
#include "modeset-internal.h"
#include "edid.h"

/*
 * The EDID quirk bits of the Linux drm_edid.c.  No quirk table sets them
 * here; drm_mode_detailed() tests the ones that change a detailed timing.
 */
#define EDID_QUIRK_PREFER_LARGE_60		(1 << 0)
#define EDID_QUIRK_135_CLOCK_TOO_HIGH		(1 << 1)
#define EDID_QUIRK_PREFER_LARGE_75		(1 << 2)
#define EDID_QUIRK_DETAILED_IN_CM		(1 << 3)
#define EDID_QUIRK_DETAILED_USE_MAXIMUM_SIZE	(1 << 4)
#define EDID_QUIRK_DETAILED_SYNC_PP		(1 << 6)
#define EDID_QUIRK_FORCE_REDUCED_BLANKING	(1 << 7)
#define EDID_QUIRK_FORCE_8BPC			(1 << 8)
#define EDID_QUIRK_FORCE_12BPC			(1 << 9)
#define EDID_QUIRK_FORCE_6BPC			(1 << 10)
#define EDID_QUIRK_FORCE_10BPC			(1 << 11)
#define EDID_QUIRK_NON_DESKTOP			(1 << 12)
#define EDID_QUIRK_CAP_DSC_15BPP		(1 << 13)

static void i915_drm_mode_do_interlace_quirk(struct drm_display_mode *mode, const struct detailed_pixel_timing *pt);
static struct drm_display_mode *i915_drm_mode_detailed(struct i915_lcd_world *world, struct drm_connector *connector, const struct drm_edid *drm_edid, const struct detailed_timing *timing);
static struct drm_display_mode *i915_edid_mode_create(struct i915_lcd_world *world, struct drm_device *dev);

/*
 * Copies a display mode, keeping the list head of the destination.
 *
 * The Linux drm_mode_copy(): the destination stays linked wherever it was
 * linked before the copy.
 */
void
drv_i915_drm_mode_copy(
	struct drm_display_mode *dst,
	const struct drm_display_mode *src)
{
	struct list_head head;

	/* Saves the list link of the destination. */
	head = dst->head;

	/* Copies the mode and puts the saved link back. */
	*dst = *src;
	dst->head = head;
}

/*
 * Initializes a display mode from another one.
 *
 * The Linux drm_mode_init(): the destination is cleared first, so its list
 * head is zero instead of whatever an on-stack mode held.
 */
void
drv_i915_drm_mode_init(
	struct drm_display_mode *dst,
	const struct drm_display_mode *src)
{
	/* Clears the destination, including its list head. */
	kern_memset(dst, 0, sizeof(*dst));

	/* Copies the mode over the cleared destination. */
	drv_i915_drm_mode_copy(dst, src);
}

/*
 * Reports the visible width and height of a display mode.
 *
 * The Linux drm_mode_get_hv_timing(): the height is doubled for a stereo
 * mode whose layout carries both eyes in one frame.
 */
void
drv_i915_drm_mode_get_hv_timing(
	const struct drm_display_mode *mode,
	int *hdisplay,
	int *vdisplay)
{
	struct drm_display_mode adjusted;

	/* Works on a copy, so the caller's mode keeps its crtc timings. */
	drv_i915_drm_mode_init(&adjusted, mode);

	/* Computes the crtc timings with only the stereo adjustment applied. */
	drv_i915_drm_mode_set_crtcinfo(&adjusted, CRTC_STEREO_DOUBLE_ONLY);

	/* Reports the visible size the stereo adjustment gave. */
	*hdisplay = adjusted.crtc_hdisplay;
	*vdisplay = adjusted.crtc_vdisplay;
}

/*
 * Sets the crtc timings of a display mode, adjusting them as asked.
 *
 * The Linux drm_mode_set_crtcinfo().  CRTC_INTERLACE_HALVE_V halves the
 * vertical timings of an interlaced mode.  CRTC_STEREO_DOUBLE computes the
 * timings of a buffer that carries both eyes (only for the frame-packing
 * layout).  CRTC_NO_DBLSCAN and CRTC_NO_VSCAN ask that a doublescan mode
 * and a mode with vscan > 1 are not adjusted.
 */
void
drv_i915_drm_mode_set_crtcinfo(
	struct drm_display_mode *p,
	int adjust_flags)
{
	unsigned int layout;

	/* A missing mode has no timings to set. */
	if (!p)
		return;

	/* Starts from the mode's own timings. */
	p->crtc_clock = p->clock;
	p->crtc_hdisplay = p->hdisplay;
	p->crtc_hsync_start = p->hsync_start;
	p->crtc_hsync_end = p->hsync_end;
	p->crtc_htotal = p->htotal;
	p->crtc_hskew = p->hskew;
	p->crtc_vdisplay = p->vdisplay;
	p->crtc_vsync_start = p->vsync_start;
	p->crtc_vsync_end = p->vsync_end;
	p->crtc_vtotal = p->vtotal;

	/* An interlaced mode is programmed per field when the caller asks for it. */
	if (p->flags & DRM_MODE_FLAG_INTERLACE) {
		if (adjust_flags & CRTC_INTERLACE_HALVE_V) {
			p->crtc_vdisplay /= 2;
			p->crtc_vsync_start /= 2;
			p->crtc_vsync_end /= 2;
			p->crtc_vtotal /= 2;
		}
	}

	/* A doublescan mode sends every line twice, unless the caller refuses the adjustment. */
	if (!(adjust_flags & CRTC_NO_DBLSCAN)) {
		if (p->flags & DRM_MODE_FLAG_DBLSCAN) {
			p->crtc_vdisplay *= 2;
			p->crtc_vsync_start *= 2;
			p->crtc_vsync_end *= 2;
			p->crtc_vtotal *= 2;
		}
	}

	/* A mode with vscan > 1 sends every line vscan times, unless the caller refuses the adjustment. */
	if (!(adjust_flags & CRTC_NO_VSCAN)) {
		if (p->vscan > 1) {
			p->crtc_vdisplay *= p->vscan;
			p->crtc_vsync_start *= p->vscan;
			p->crtc_vsync_end *= p->vscan;
			p->crtc_vtotal *= p->vscan;
		}
	}

	/* A stereo buffer that carries both eyes doubles the frame of the frame-packing layout. */
	if (adjust_flags & CRTC_STEREO_DOUBLE) {
		layout = p->flags & DRM_MODE_FLAG_3D_MASK;

		/* Only the frame-packing layout stacks the two eyes in one frame. */
		switch (layout) {
		case DRM_MODE_FLAG_3D_FRAME_PACKING:
			p->crtc_clock *= 2;
			p->crtc_vdisplay += p->crtc_vtotal;
			p->crtc_vsync_start += p->crtc_vtotal;
			p->crtc_vsync_end += p->crtc_vtotal;
			p->crtc_vtotal += p->crtc_vtotal;
			break;
		}
	}

	/* The blanking spans from the end of the visible area or the sync to the end of the total. */
	p->crtc_vblank_start = min(p->crtc_vsync_start, p->crtc_vdisplay);
	p->crtc_vblank_end = max(p->crtc_vsync_end, p->crtc_vtotal);
	p->crtc_hblank_start = min(p->crtc_hsync_start, p->crtc_hdisplay);
	p->crtc_hblank_end = max(p->crtc_hsync_end, p->crtc_htotal);
}

/*
 * Names a display mode.
 *
 * The name is a label for logs, and the callers print the numbers
 * themselves, so no name is written.
 */
void
drv_i915_lcd_drm_mode_set_name(
	struct drm_display_mode *mode)
{
	UNUSED_PARAMETER(mode);
}

/*
 * Reads the panel's preferred mode from its EDID base block.
 *
 * The preferred mode is the first detailed timing descriptor whose pixel
 * clock is not zero, converted by the Linux drm_mode_detailed() and marked
 * preferred.  When index is not NULL it receives the descriptor's index.
 * Returns 0, or EINVAL when the block has no timing descriptor or the
 * first one cannot be converted.
 */
int
drv_i915_edid_preferred_mode(
	struct i915_lcd_world *world,
	const u8 *edid128,
	struct drm_display_mode *mode,
	unsigned *index)
{
	struct drm_connector connector;
	struct drm_edid drm_edid;
	const struct edid *edid;
	struct drm_display_mode *detailed;
	unsigned i;

	/* The block is read as the Linux EDID layout. */
	edid = (const struct edid *)edid128;

	/*
	 * Describes the connector the conversion logs about: the panel, on the
	 * parser's own DRM device.  Only the base block is handed over.
	 */
	kern_memset(&connector, 0, sizeof(connector));
	connector.name = "eDP-1";
	connector.dev = &world->i915_edid_preferred_mode_dev;
	drm_edid.edid = edid;

	/* Finds the first descriptor that is a timing. */
	for (i = 0; i < ARRAY_SIZE(edid->detailed_timings); i++) {
		/* A zero pixel clock marks a display descriptor, not a timing. */
		if (le16_to_cpu(edid->detailed_timings[i].pixel_clock) != 0)
			break;
	}

	/* A block without a timing descriptor has no preferred mode. */
	if (i == ARRAY_SIZE(edid->detailed_timings))
		return EINVAL;

	/* Converts the timing into the one mode object the conversion hands out. */
	world->edid_mode_taken = 0;
	detailed = i915_drm_mode_detailed(
		world,
		&connector,
		&drm_edid,
		&edid->detailed_timings[i]);
	if (detailed == NULL)
		return EINVAL;

	/* Hands the mode to the caller, marked as the panel's preferred one. */
	*mode = *detailed;
	mode->type |= DRM_MODE_TYPE_PREFERRED;

	/* Tells the caller which descriptor the mode came from. */
	if (index != NULL)
		*index = i;

	/* Succeeded: the caller holds the preferred mode. */
	return 0;
}

/*
 * Doubles the vertical timings of an interlaced CEA mode the EDID gave per field.
 *
 * EDID is delightfully ambiguous about how interlaced modes are to be
 * encoded.  Our internal representation is of frame height, but some
 * HDTV detailed timings are encoded as field height.
 *
 * The format list here is from CEA, in frame size.  Technically we
 * should be checking refresh rate too.  Whatever.
 */
static void
i915_drm_mode_do_interlace_quirk(
	struct drm_display_mode *mode,
	const struct detailed_pixel_timing *pt)
{
	/* The interlaced CEA formats, in frame size. */
	static const struct {
		int w, h;
	} cea_interlaced[] = {
		{ 1920, 1080 },
		{  720,  480 },
		{ 1440,  480 },
		{ 2880,  480 },
		{  720,  576 },
		{ 1440,  576 },
		{ 2880,  576 },
	};
	unsigned int i;

	/* A progressive timing needs nothing. */
	if (!(pt->misc & EDID_PT_INTERLACED))
		return;

	/* Turns a field-height timing of a CEA format into its frame height. */
	for (i = 0; i < ARRAY_SIZE(cea_interlaced); i++) {
		/* The timing matches the format at half its frame height. */
		if ((mode->hdisplay == cea_interlaced[i].w) &&
		    (mode->vdisplay == cea_interlaced[i].h / 2)) {
			mode->vdisplay *= 2;
			mode->vsync_start *= 2;
			mode->vsync_end *= 2;
			mode->vtotal *= 2;
			mode->vtotal |= 1;
		}
	}

	/* The mode is interlaced whatever height the EDID gave. */
	mode->flags |= DRM_MODE_FLAG_INTERLACE;
}

/* Creates a display mode from an EDID detailed timing descriptor (the Linux drm_mode_detailed()). */
static struct drm_display_mode *
i915_drm_mode_detailed(
	struct i915_lcd_world *world,
	struct drm_connector *connector,
	const struct drm_edid *drm_edid,
	const struct detailed_timing *timing)
{
	const struct drm_display_info *info;
	struct drm_device *dev;
	struct drm_display_mode *mode;
	const struct detailed_pixel_timing *pt;
	unsigned hactive;
	unsigned vactive;
	unsigned hblank;
	unsigned vblank;
	unsigned hsync_offset;
	unsigned hsync_pulse_width;
	unsigned vsync_offset;
	unsigned vsync_pulse_width;

	/* Finds what the EDID told about the sink, the DRM device and the timing words. */
	info = &connector->display_info;
	dev = connector->dev;
	pt = &timing->data.pixel_data;

	/* Decodes the timing words, whose high bits are packed into shared bytes. */
	hactive = (pt->hactive_hblank_hi & 0xf0) << 4 | pt->hactive_lo;
	vactive = (pt->vactive_vblank_hi & 0xf0) << 4 | pt->vactive_lo;
	hblank = (pt->hactive_hblank_hi & 0xf) << 8 | pt->hblank_lo;
	vblank = (pt->vactive_vblank_hi & 0xf) << 8 | pt->vblank_lo;
	hsync_offset = (pt->hsync_vsync_offset_pulse_width_hi & 0xc0) << 2 | pt->hsync_offset_lo;
	hsync_pulse_width = (pt->hsync_vsync_offset_pulse_width_hi & 0x30) << 4 | pt->hsync_pulse_width_lo;
	vsync_offset = (pt->hsync_vsync_offset_pulse_width_hi & 0xc) << 2 | pt->vsync_offset_pulse_width_lo >> 4;
	vsync_pulse_width = (pt->hsync_vsync_offset_pulse_width_hi & 0x3) << 4 | (pt->vsync_offset_pulse_width_lo & 0xf);

	/* ignore tiny modes */
	if (hactive < 64 || vactive < 64)
		return NULL;

	/* A stereo timing is refused. */
	if (pt->misc & EDID_PT_STEREO) {
		I915_LCD_DRM_DBG_KMS(dev,
				     "[CONNECTOR:%d:%s] Stereo mode not supported\n",
				     connector->base.id,
				     connector->name);
		return NULL;
	}

	/* A composite-sync timing is only noted: it is converted like a separate-sync one. */
	if (!(pt->misc & EDID_PT_SEPARATE_SYNC)) {
		I915_LCD_DRM_DBG_KMS(dev,
				     "[CONNECTOR:%d:%s] Composite sync not supported\n",
				     connector->base.id,
				     connector->name);
	}

	/* it is incorrect if hsync/vsync width is zero */
	if (!hsync_pulse_width || !vsync_pulse_width) {
		I915_LCD_DRM_DBG_KMS(dev,
				     "[CONNECTOR:%d:%s] Incorrect Detailed timing. Wrong Hsync/Vsync pulse width\n",
				     connector->base.id,
				     connector->name);
		return NULL;
	}

	/*
	 * A sink with the reduced-blanking quirk gets a CVT mode of the same
	 * size instead of the descriptor's timings (the Linux text jumps to
	 * the size assignment); any other sink gets the descriptor's timings.
	 */
	if (info->quirks & EDID_QUIRK_FORCE_REDUCED_BLANKING) {
		mode = drm_cvt_mode(dev,
				    hactive,
				    vactive,
				    60,
				    true,
				    false,
				    false);
		if (!mode)
			return NULL;
	} else {
		mode = i915_edid_mode_create(world, dev);
		if (!mode)
			return NULL;

		/* The pixel clock is in units of 10 kHz, except on a sink whose clock is known too high. */
		if (info->quirks & EDID_QUIRK_135_CLOCK_TOO_HIGH)
			mode->clock = 1088 * 10;
		else
			mode->clock = le16_to_cpu(timing->pixel_clock) * 10;

		/* Lays out the horizontal timings from the active width. */
		mode->hdisplay = hactive;
		mode->hsync_start = mode->hdisplay + hsync_offset;
		mode->hsync_end = mode->hsync_start + hsync_pulse_width;
		mode->htotal = mode->hdisplay + hblank;

		/* Lays out the vertical timings from the active height. */
		mode->vdisplay = vactive;
		mode->vsync_start = mode->vdisplay + vsync_offset;
		mode->vsync_end = mode->vsync_start + vsync_pulse_width;
		mode->vtotal = mode->vdisplay + vblank;

		/* Some EDIDs have bogus h/vsync_end values */
		if (mode->hsync_end > mode->htotal) {
			I915_LCD_DRM_DBG_KMS(dev,
					     "[CONNECTOR:%d:%s] reducing hsync_end %d->%d\n",
					     connector->base.id,
					     connector->name,
					     mode->hsync_end,
					     mode->htotal);
			mode->hsync_end = mode->htotal;
		}

		/* The same for the vertical sync end. */
		if (mode->vsync_end > mode->vtotal) {
			I915_LCD_DRM_DBG_KMS(dev,
					     "[CONNECTOR:%d:%s] reducing vsync_end %d->%d\n",
					     connector->base.id,
					     connector->name,
					     mode->vsync_end,
					     mode->vtotal);
			mode->vsync_end = mode->vtotal;
		}

		/* Turns an interlaced CEA timing given per field into its frame height. */
		i915_drm_mode_do_interlace_quirk(mode, pt);

		/* The sync polarities: forced positive by a quirk, otherwise as the descriptor says. */
		if (info->quirks & EDID_QUIRK_DETAILED_SYNC_PP) {
			mode->flags |= DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC;
		} else {
			mode->flags |= (pt->misc & EDID_PT_HSYNC_POSITIVE) ? DRM_MODE_FLAG_PHSYNC : DRM_MODE_FLAG_NHSYNC;
			mode->flags |= (pt->misc & EDID_PT_VSYNC_POSITIVE) ? DRM_MODE_FLAG_PVSYNC : DRM_MODE_FLAG_NVSYNC;
		}
	}

	/* The physical size of the image, in millimetres. */
	mode->width_mm = pt->width_mm_lo | (pt->width_height_mm_hi & 0xf0) << 4;
	mode->height_mm = pt->height_mm_lo | (pt->width_height_mm_hi & 0xf) << 8;

	/* A sink with this quirk gives the size in centimetres. */
	if (info->quirks & EDID_QUIRK_DETAILED_IN_CM) {
		mode->width_mm *= 10;
		mode->height_mm *= 10;
	}

	/* A sink with this quirk is sized by the base block's maximum image size. */
	if (info->quirks & EDID_QUIRK_DETAILED_USE_MAXIMUM_SIZE) {
		mode->width_mm = drm_edid->edid->width_cm * 10;
		mode->height_mm = drm_edid->edid->height_cm * 10;
	}

	/* Marks the mode as one the driver made, and names it. */
	mode->type = DRM_MODE_TYPE_DRIVER;
	drv_i915_lcd_drm_mode_set_name(mode);

	/* Succeeded: the caller holds the converted mode. */
	return mode;
}

/* Hands out the world's one mode object, cleared, unless it is taken (the Linux drm_mode_create()). */
static struct drm_display_mode *
i915_edid_mode_create(
	struct i915_lcd_world *world,
	struct drm_device *dev)
{
	UNUSED_PARAMETER(dev);

	/* The one object is already handed out. */
	if (world->edid_mode_taken)
		return NULL;

	/* The object is taken until the preferred-mode parser clears the mark again. */
	world->edid_mode_taken = 1;

	/* Clears the object for the new mode. */
	kern_memset(&world->edid_mode_object, 0, sizeof(world->edid_mode_object));

	/* Succeeded: the caller owns the mode object. */
	return &world->edid_mode_object;
}
