/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The records of the display's first commit that touch no hardware (the
 * takeover stage).
 *
 * The takeover sends this when it starts the display and commits the first mode
 * on a head the firmware lit (the Turing core and window classes): the core's
 * start (its notifier object and every window's usage bounds, without a
 * release), then one core release with the output's control, the head's mode
 * and every window's owner, the head's output LUT, the window's image and input
 * LUT released interlocked with the core, and a second core release interlocked
 * with the window.  Also here: the mode's raster words from a timing, the
 * identity LUTs, the EDID's preferred timing and HDMI facts, the AVI
 * infoframe's words and the resource manager's parameters of the controls the
 * takeover sends.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <uapi/errno.h>

#include "drivers/gpu/nvrtx/display/nvrtx-disp.h"
#include "drivers/gpu/nvrtx/nvrtx-gsp.h"

/*
 * The core's start: every window may show packed RGB of 1 to 8 bytes,
 * nothing rotated.
 *
 * Values referenced: Linux v6.19 drivers/gpu/drm/nouveau/dispnv50/corec57d.c
 * (corec57d_init) and NVIDIA open-gpu-doc classes/display/clc57d.h.
 */
#define FIRST_WINDOW_FORMATS		0x0000000fU
#define FIRST_WINDOW_ROTATED		0x00000000U

/*
 * A window's usage bounds: 0x7fff pixels a line, an input LUT, two scaler
 * taps, no upscaling.
 *
 * Values referenced: Linux v6.19 drivers/gpu/drm/nouveau/dispnv50/corec57d.c
 * (corec57d_init) and NVIDIA open-gpu-doc classes/display/clc57d.h.
 */
#define FIRST_WINDOW_USAGE		0x00117fffU

/*
 * The head's usage bounds: a 256 by 256 cursor, an output LUT, two scaler
 * taps, upscaling.
 *
 * Values referenced: Linux v6.19 drivers/gpu/drm/nouveau/dispnv50/corec57d.c
 * (corec57d_init) and NVIDIA open-gpu-doc classes/display/clc57d.h.
 */
#define FIRST_HEAD_USAGE		0x00001114U

/*
 * The head's second vertical blank in a progressive mode, and its control
 * (not interlaced).
 *
 * Values referenced: Linux v6.19 drivers/gpu/drm/nouveau/dispnv50/headc57d.c
 * (headc57d_mode) and NVIDIA open-gpu-doc classes/display/clc57d.h.
 */
#define FIRST_VERT_BLANK2_PROGRESSIVE	0x00000001U
#define FIRST_HEAD_CONTROL		0x00000000U

/*
 * The output resource: no extra packet window, 24-bit 4:4:4, the sync
 * polarities in bits 2 and 3.
 *
 * Values referenced: Linux v6.19 drivers/gpu/drm/nouveau/dispnv50/headc57d.c
 * (headc57d_or) and NVIDIA open-gpu-doc classes/display/clc57d.h.
 */
#define FIRST_OUTPUT_RESOURCE		0xfc000040U
#define FIRST_OUTPUT_HSYNC_NEGATIVE	0x00000004U
#define FIRST_OUTPUT_VSYNC_NEGATIVE	0x00000008U

/*
 * The output LUT: interpolated, direct 10-bit, 1029 entries; its scale; read
 * through the VRAM object.
 *
 * Values referenced: Linux v6.19 drivers/gpu/drm/nouveau/dispnv50/headc57d.c
 * (headc57d_olut_set) and NVIDIA open-gpu-doc classes/display/clc57d.h.
 */
#define FIRST_OLUT_CONTROL		0x00040509U
#define FIRST_OLUT_SCALE		0xffffffffU

/*
 * The input LUT: not interpolated, direct 10-bit, 1029 entries.
 *
 * Values referenced: Linux v6.19 drivers/gpu/drm/nouveau/dispnv50/wndwc57e.c
 * (wndwc57e_ilut_set) and NVIDIA open-gpu-doc classes/display/clc57e.h.
 */
#define FIRST_ILUT_CONTROL		0x00040508U

/*
 * The window's present control: one frame at least, begin without tearing.
 *
 * Values referenced: Linux v6.19 drivers/gpu/drm/nouveau/dispnv50/wndwc57e.c
 * (wndwc57e_image_set) and NVIDIA open-gpu-doc classes/display/clc57e.h.
 */
#define FIRST_PRESENT_CONTROL		0x00000001U

/*
 * The window's blending: depth 255, constant alpha 255, the source over the
 * picture as premultiplied.
 *
 * Values referenced: Linux v6.19 drivers/gpu/drm/nouveau/dispnv50/wndwc37e.c
 * (wndwc37e_blend_set) and NVIDIA open-gpu-doc classes/display/clc57e.h.
 */
#define FIRST_COMPOSITION_DEPTH		0x00000ff0U
#define FIRST_COMPOSITION_ALPHA		0x000000ffU
#define FIRST_COMPOSITION_FACTORS	0x00007722U
#define FIRST_COMPOSITION_KEY		0xffff0000U

/*
 * The notifier control: write the notifier at the offset, with or without
 * asking for it.
 *
 * Values referenced: Linux v6.19 drivers/gpu/drm/nouveau/dispnv50/wndwc37e.c
 * (wndwc37e_ntfy_set) and NVIDIA open-gpu-doc classes/display/clc57d.h and
 * clc57e.h.
 */
#define FIRST_NOTIFY_ENABLE		0x00001000U
#define FIRST_NOTIFY_DISABLE		0x00000000U

/*
 * A release: the state gathered so far.
 *
 * Values referenced: Linux v6.19 drivers/gpu/drm/nouveau/dispnv50/wndwc37e.c
 * (wndwc37e_update) and NVIDIA open-gpu-doc classes/display/clc57d.h and
 * clc57e.h.
 */
#define FIRST_RELEASE			0x00000001U

/*
 * The core's methods besides nvrtx-disp.h's: the head's output LUT.
 *
 * Values referenced: NVIDIA open-gpu-doc classes/display/clc57d.h.
 */
#define FIRST_HEAD_OLUT_CONTROL		0x280U

/*
 * The window's methods besides nvrtx-disp.h's: the input LUT's control.
 *
 * Values referenced: NVIDIA open-gpu-doc classes/display/clc57e.h.
 */
#define FIRST_WINDOW_ILUT_CONTROL	0x440U

/*
 * The LUTs: a 32-byte header, then 1024 entries of 8 bytes and the last
 * repeated.
 *
 * Values referenced: Linux v6.19 drivers/gpu/drm/nouveau/dispnv50/headc57d.c
 * (headc57d_olut_load) and wndwc57e.c (wndwc57e_ilut_load).
 */
#define FIRST_LUT_HEADER_BYTES		0x20U
#define FIRST_LUT_ENTRIES		1024U
#define FIRST_LUT_ENTRY_BYTES		8U

/*
 * The EDID: its block, the first detailed timing, an extension's tag and
 * where its data blocks end.
 *
 * Values referenced: VESA E-EDID Standard Release A2 and CTA-861 (the base
 * block and the CTA extension).
 */
#define FIRST_EDID_BLOCK_BYTES		128U
#define FIRST_EDID_TIMING_AT		54U
#define FIRST_EDID_EXTENSIONS_AT	126U
#define FIRST_EDID_CEA_TAG		0x02U
#define FIRST_EDID_CEA_AUDIO		0x40U

/*
 * The CEA data blocks: the vendor-specific tag and the two vendors'
 * identifiers (low byte first).
 *
 * Values referenced: CTA-861 (the vendor-specific data block) and the HDMI
 * specifications (the HDMI and HDMI Forum identifiers).
 */
#define FIRST_CEA_VENDOR_TAG		3U
#define FIRST_CEA_HDMI_OUI		0x000c03U
#define FIRST_CEA_FORUM_OUI		0xc45dd8U

/*
 * The HDMI forum block's capability byte: SCDC present, and scrambling below
 * 340 MHz.
 *
 * Values referenced: the HDMI 2.0 specification (the HDMI Forum
 * vendor-specific data block).
 */
#define FIRST_FORUM_SCDC		0x80U
#define FIRST_FORUM_LOW_RATES		0x08U

/*
 * The HDMI sink capabilities the resource manager is told.
 *
 * Values referenced: NVIDIA open-gpu-kernel-modules 570.144
 * src/common/sdk/nvidia/inc/ctrl/ctrl0073/ctrl0073specific.h (the HDMI sink
 * capabilities).
 */
#define FIRST_SINK_GT_340		0x00000001U
#define FIRST_SINK_LTE_340		0x00000002U
#define FIRST_SINK_SCDC			0x00000004U

/*
 * The AVI infoframe: type, version 2, 13 bytes; RGB with the active aspect,
 * the aspect as the picture's, full range.
 *
 * Values referenced: CTA-861 (the AVI InfoFrame).
 */
#define FIRST_AVI_TYPE			0x82U
#define FIRST_AVI_VERSION		0x02U
#define FIRST_AVI_LENGTH		0x0dU
#define FIRST_AVI_ACTIVE_PRESENT	0x10U
#define FIRST_AVI_ASPECT_PICTURE	0x08U
#define FIRST_AVI_FULL_RANGE		0x08U
#define FIRST_AVI_BYTES			17U

static void first_lut_repeat_last(uint8_t *lut);
static uint16_t first_fp16(uint16_t fixed);
static uint32_t first_oui(const uint8_t *block);
static void first_put16(uint8_t *bytes, uint32_t at, uint16_t value);

/*
 * Writes the core's start: the notifier's object, and every window's
 * format and usage bounds, without a release.
 *
 * Procedure referenced: Linux v6.19 drivers/gpu/drm/nouveau/dispnv50/disp.c
 * and corec57d.c (the core's start).
 */
void
nvrtx_disp_first_core_start(
	struct nvrtx_push *push)
{
	uint32_t formats[2];
	uint32_t window;
	uint32_t base;

	/* The core's notifiers are written through the sync object. */
	nvrtx_push_one(push, NVRTX_CORE_SET_CONTEXT_DMA_NOTIFIER, NVRTX_DISP_HANDLE_SYNC);

	/* Each of the eight windows: the packed RGB formats, none rotated, and the usage. */
	for (window = 0U; window < NVRTX_DISP_WINDOWS; window++) {
		base = window * NVRTX_CORE_WINDOW_STRIDE;
		formats[0] = FIRST_WINDOW_FORMATS;
		formats[1] = FIRST_WINDOW_ROTATED;
		nvrtx_push_method(push, NVRTX_CORE_WINDOW_FORMAT_BOUNDS + base, formats, 2U);
		nvrtx_push_one(push, NVRTX_CORE_WINDOW_USAGE_BOUNDS + base, FIRST_WINDOW_USAGE);
	}
}

/*
 * Writes the first commit's core methods up to its release: the output
 * taken off the head and given back with its protocol, the head's view,
 * raster, clock, usage and output resource, and every window's owner.
 *
 * Procedure referenced: Linux v6.19 drivers/gpu/drm/nouveau/dispnv50/disp.c
 * and headc57d.c (the first commit's core release).
 */
void
nvrtx_disp_first_core_mode(
	struct nvrtx_push *push,
	const struct nvrtx_disp_first *first)
{
	const struct nvrtx_disp_mode *mode;
	uint32_t head_base;
	uint32_t sor_method;
	uint32_t raster[4];
	uint32_t control;
	uint32_t window;

	mode = &first->mode;
	head_base = NVRTX_CORE_HEAD_FIRST + first->head * NVRTX_CORE_HEAD_STRIDE;
	sor_method = NVRTX_CORE_SOR_SET_CONTROL + first->sor * NVRTX_CORE_SOR_STRIDE;

	/* The output off: no display on the head, no owner of the SOR. */
	nvrtx_push_one(push, head_base + NVRTX_HEAD_DISPLAY_ID, 0U);
	nvrtx_push_one(push, sor_method, 0U);

	/* The output on: the display on the head, the SOR owned by the head with its protocol. */
	control = (first->protocol << NVRTX_DISP_SOR_PROTOCOL_SHIFT) | (1U << first->head);
	nvrtx_push_one(push, head_base + NVRTX_HEAD_DISPLAY_ID, first->display_id);
	nvrtx_push_one(push, sor_method, control);

	/* The view: the picture's size in, the mode's visible size out (the head's scaler enlarges a smaller picture). */
	nvrtx_push_one(push, head_base + NVRTX_HEAD_VIEWPORT_SIZE_IN, first->view_in);
	nvrtx_push_one(push, head_base + NVRTX_HEAD_VIEWPORT_SIZE_OUT, mode->active);

	/* The mode: the raster's four words, the second vertical blank, the control, the clock and its maximum, the usage. */
	raster[0] = mode->raster_size;
	raster[1] = mode->sync_end;
	raster[2] = mode->blank_end;
	raster[3] = mode->blank_start;
	nvrtx_push_method(push, head_base + NVRTX_HEAD_RASTER_SIZE, raster, 4U);
	nvrtx_push_one(push, head_base + NVRTX_HEAD_RASTER_VERT_BLANK2, mode->vert_blank2);
	nvrtx_push_one(push, head_base + NVRTX_HEAD_CONTROL, FIRST_HEAD_CONTROL);
	nvrtx_push_one(push, head_base + NVRTX_HEAD_PIXEL_CLOCK, mode->clock_hz);
	nvrtx_push_one(push, head_base + NVRTX_HEAD_PIXEL_CLOCK_MAX, mode->clock_hz);
	nvrtx_push_one(push, head_base + NVRTX_HEAD_USAGE_BOUNDS, FIRST_HEAD_USAGE);

	/* The output resource's control. */
	nvrtx_push_one(push, head_base + NVRTX_HEAD_CONTROL_OUTPUT_RESOURCE, mode->output_resource);

	/* Every window to its head: windows 2h and 2h + 1 belong to head h. */
	for (window = 0U; window < NVRTX_DISP_WINDOWS; window++)
		nvrtx_push_one(push, NVRTX_CORE_WINDOW_SET_CONTROL + window * NVRTX_CORE_WINDOW_STRIDE, window >> 1);
}

/*
 * Writes a core release with its notifier at offset 0 of the sync object:
 * the notifier asked for, the cursors not interlocked, the windows in the
 * mask interlocked, the release, the notifier no longer asked for.
 */
void
nvrtx_disp_first_core_release(
	struct nvrtx_push *push,
	uint32_t window_interlock)
{
	uint32_t interlock[2];

	/* The notifier, written once the release is done. */
	nvrtx_push_one(push, NVRTX_CORE_SET_NOTIFIER_CONTROL, FIRST_NOTIFY_ENABLE);

	/* No cursor, and the given windows, interlocked. */
	interlock[0] = 0U;
	interlock[1] = window_interlock;
	nvrtx_push_method(push, NVRTX_CORE_SET_INTERLOCK_FLAGS, interlock, 2U);

	/* The release, then no notifier for what follows. */
	nvrtx_push_one(push, NVRTX_CORE_UPDATE, FIRST_RELEASE);
	nvrtx_push_one(push, NVRTX_CORE_SET_NOTIFIER_CONTROL, FIRST_NOTIFY_DISABLE);
}

/*
 * Writes a head's view without a release, for a Volta or later head: the
 * viewport's input size (the picture the windows show) and its output size (the
 * mode's visible size), each width in bits 15:0 and height in 31:16.  An input
 * smaller than the output is enlarged by the head's scaler; the core's release
 * that follows takes it in.
 *
 * Procedure referenced: Linux v6.19
 * drivers/gpu/drm/nouveau/dispnv50/headc37d.c (headc37d_view).
 */
void
nvrtx_disp_first_core_view(
	struct nvrtx_push *push,
	uint32_t head,
	uint32_t view_in,
	uint32_t view_out)
{
	uint32_t head_base;

	head_base = NVRTX_CORE_HEAD_FIRST + head * NVRTX_CORE_HEAD_STRIDE;

	/* The input size, then the output size. */
	nvrtx_push_one(push, head_base + NVRTX_HEAD_VIEWPORT_SIZE_IN, view_in);
	nvrtx_push_one(push, head_base + NVRTX_HEAD_VIEWPORT_SIZE_OUT, view_out);
}

/*
 * Writes the head's output LUT: interpolated, direct, read through the
 * VRAM object at a VRAM offset (on a 256-byte boundary).
 */
void
nvrtx_disp_first_core_olut(
	struct nvrtx_push *push,
	uint32_t head,
	uint64_t offset)
{
	uint32_t words[4];

	/* The control, the scale, the object and the offset in one run. */
	words[0] = FIRST_OLUT_CONTROL;
	words[1] = FIRST_OLUT_SCALE;
	words[2] = NVRTX_DISP_HANDLE_VRAM;
	words[3] = (uint32_t)(offset >> 8);
	nvrtx_push_method(push, NVRTX_CORE_HEAD_FIRST + head * NVRTX_CORE_HEAD_STRIDE + FIRST_HEAD_OLUT_CONTROL, words, 4U);
}

/*
 * Writes the window's first image and its release interlocked with the
 * core: the notifier, the present control, the surface, the input LUT at
 * a VRAM offset, the blending, then the interlock and the release.
 *
 * storage is the window's storage word for a pitch-linear surface: the
 * layout field set on Turing, zero from Ampere on, whose window has no
 * layout field (the context DMA object's kind says pitch there).
 *
 * Procedure referenced: Linux v6.19
 * drivers/gpu/drm/nouveau/dispnv50/wndwc57e.c (the window's image released
 * interlocked with the core).
 */
void
nvrtx_disp_first_window(
	struct nvrtx_push *push,
	uint32_t window,
	const struct nvrtx_disp_surface *surface,
	uint32_t storage,
	uint32_t notifier_offset,
	uint64_t ilut_offset)
{
	uint32_t words[7];
	uint32_t size;
	uint32_t index;

	size = (surface->height << 16) | (surface->width & 0xffffU);

	/* The window's notifier, through the sync object at its offset. */
	words[0] = NVRTX_DISP_HANDLE_SYNC;
	words[1] = notifier_offset;
	nvrtx_push_method(push, NVRTX_WINDOW_SET_CONTEXT_DMA_NOTIFIER, words, 2U);

	/* The present control. */
	nvrtx_push_one(push, NVRTX_WINDOW_SET_PRESENT_CONTROL, FIRST_PRESENT_CONTROL);

	/* The surface: its size, the generation's pitch-linear storage word, format and pitch in 64-byte units. */
	words[0] = size;
	words[1] = storage;
	words[2] = surface->format;
	words[3] = surface->pitch / NVRTX_DISP_PITCH_UNIT;
	nvrtx_push_method(push, NVRTX_WINDOW_SET_SIZE, words, 4U);

	/* Where it is read: the object, the offset, the point in, the size in and out. */
	nvrtx_push_one(push, NVRTX_WINDOW_SET_CONTEXT_DMA_ISO, surface->handle);
	nvrtx_push_one(push, NVRTX_WINDOW_SET_OFFSET, (uint32_t)(surface->offset >> 8));
	nvrtx_push_one(push, NVRTX_WINDOW_SET_POINT_IN, 0U);
	nvrtx_push_one(push, NVRTX_WINDOW_SET_SIZE_IN, size);
	nvrtx_push_one(push, NVRTX_WINDOW_SET_SIZE_OUT, size);

	/* The input LUT: its control, the VRAM object and the offset. */
	words[0] = FIRST_ILUT_CONTROL;
	words[1] = NVRTX_DISP_HANDLE_VRAM;
	words[2] = (uint32_t)(ilut_offset >> 8);
	nvrtx_push_method(push, FIRST_WINDOW_ILUT_CONTROL, words, 3U);

	/* The blending: depth, constant alpha, factors, and the four keys over their full range. */
	words[0] = FIRST_COMPOSITION_DEPTH;
	words[1] = FIRST_COMPOSITION_ALPHA;
	words[2] = FIRST_COMPOSITION_FACTORS;
	for (index = 3U; index < 7U; index++)
		words[index] = FIRST_COMPOSITION_KEY;
	nvrtx_push_method(push, NVRTX_WINDOW_SET_COMPOSITION_CONTROL, words, 7U);

	/* Interlocked with the core (no cursor) and with this window, then released. */
	words[0] = 1U;
	words[1] = 1U << window;
	nvrtx_push_method(push, NVRTX_WINDOW_SET_INTERLOCK_FLAGS, words, 2U);
	nvrtx_push_one(push, NVRTX_WINDOW_UPDATE, FIRST_RELEASE);
}

/*
 * Writes a later flip of the window: the notifier at its offset, the whole
 * image (present control, size, storage, format, pitch, object, offset, point
 * and sizes), then a release interlocked with this window and, when with_core
 * is nonzero, with the core (a flip that comes with a change of the head's
 * view, which the core's release interlocked with this window then completes).
 * storage is as for nvrtx_disp_first_window.
 *
 * Procedure referenced: Linux v6.19
 * drivers/gpu/drm/nouveau/dispnv50/wndwc57e.c and wndwc37e.c (the window's
 * image and update).
 */
void
nvrtx_disp_first_window_flip(
	struct nvrtx_push *push,
	uint32_t window,
	const struct nvrtx_disp_surface *surface,
	uint32_t storage,
	uint32_t notifier_offset,
	int with_core)
{
	uint32_t words[4];
	uint32_t size;

	size = (surface->height << 16) | (surface->width & 0xffffU);

	/*
	 * A block-linear surface's storage word: the layout field block linear
	 * (0) and its block height; the pitch word counts its rows of GOBs in
	 * the same 64-byte units.
	 *
	 * Procedure referenced: Linux v6.19
	 * drivers/gpu/drm/nouveau/dispnv50/wndwc57e.c (wndwc57e_image_set) and
	 * wndw.c (the block-linear pitch in GOBs of 64 bytes).
	 */
	if (surface->block_linear != 0)
		storage = surface->block_height & NVRTX_DISP_STORAGE_BLOCK_HEIGHT_MASK;

	/* The window's notifier, through the sync object at its offset. */
	words[0] = NVRTX_DISP_HANDLE_SYNC;
	words[1] = notifier_offset;
	nvrtx_push_method(push, NVRTX_WINDOW_SET_CONTEXT_DMA_NOTIFIER, words, 2U);

	/* The present control. */
	nvrtx_push_one(push, NVRTX_WINDOW_SET_PRESENT_CONTROL, FIRST_PRESENT_CONTROL);

	/* The surface: its size, the storage word, format and pitch in 64-byte units. */
	words[0] = size;
	words[1] = storage;
	words[2] = surface->format;
	words[3] = surface->pitch / NVRTX_DISP_PITCH_UNIT;
	nvrtx_push_method(push, NVRTX_WINDOW_SET_SIZE, words, 4U);

	/* Where it is read: the object, the offset, the point in, the size in and out. */
	nvrtx_push_one(push, NVRTX_WINDOW_SET_CONTEXT_DMA_ISO, surface->handle);
	nvrtx_push_one(push, NVRTX_WINDOW_SET_OFFSET, (uint32_t)(surface->offset >> 8));
	nvrtx_push_one(push, NVRTX_WINDOW_SET_POINT_IN, 0U);
	nvrtx_push_one(push, NVRTX_WINDOW_SET_SIZE_IN, size);
	nvrtx_push_one(push, NVRTX_WINDOW_SET_SIZE_OUT, size);

	/* Interlocked with this window, and with the core when the head's view changes with it, then released. */
	words[0] = 0U;
	if (with_core != 0)
		words[0] = 1U;
	words[1] = 1U << window;
	nvrtx_push_method(push, NVRTX_WINDOW_SET_INTERLOCK_FLAGS, words, 2U);
	nvrtx_push_one(push, NVRTX_WINDOW_UPDATE, FIRST_RELEASE);
}

/*
 * Computes a head's mode words from a timing: the visible size, the
 * raster's total, sync end, blank end and start counted from the sync's
 * start, a progressive second blank, the clock in hertz, and the output
 * resource's control with the sync polarities.
 */
void
nvrtx_disp_first_mode(
	const struct nvrtx_disp_timing *timing,
	struct nvrtx_disp_mode *mode)
{
	uint32_t hsync_end;
	uint32_t vsync_end;
	uint32_t hblank_end;
	uint32_t vblank_end;
	uint32_t hblank_start;
	uint32_t vblank_start;

	/* The raster counted from the sync's start. */
	hsync_end = timing->hsync_end - timing->hsync_start - 1U;
	vsync_end = timing->vsync_end - timing->vsync_start - 1U;
	hblank_end = timing->htotal - timing->hsync_start - 1U;
	vblank_end = timing->vtotal - timing->vsync_start - 1U;
	hblank_start = hblank_end + timing->hdisplay;
	vblank_start = vblank_end + timing->vdisplay;

	/* The words. */
	mode->active = (timing->vdisplay << 16) | (timing->hdisplay & 0xffffU);
	mode->raster_size = (timing->vtotal << 16) | (timing->htotal & 0xffffU);
	mode->sync_end = (vsync_end << 16) | (hsync_end & 0xffffU);
	mode->blank_end = (vblank_end << 16) | (hblank_end & 0xffffU);
	mode->blank_start = (vblank_start << 16) | (hblank_start & 0xffffU);
	mode->vert_blank2 = FIRST_VERT_BLANK2_PROGRESSIVE;
	mode->clock_hz = timing->clock_khz * 1000U;
	mode->dither = 0U;

	/* The output resource with the polarities. */
	mode->output_resource = FIRST_OUTPUT_RESOURCE;
	if (timing->nhsync != 0)
		mode->output_resource |= FIRST_OUTPUT_HSYNC_NEGATIVE;
	if (timing->nvsync != 0)
		mode->output_resource |= FIRST_OUTPUT_VSYNC_NEGATIVE;
}

/*
 * Recovers a timing from a head's armed raster words, the clock and the
 * output resource's polarities (the inverse of nvrtx_disp_first_mode()).
 */
void
nvrtx_disp_first_timing_from_armed(
	const uint32_t *armed,
	struct nvrtx_disp_timing *timing)
{
	uint32_t total;
	uint32_t sync_end;
	uint32_t blank_end;
	uint32_t blank_start;
	uint32_t output;

	total = armed[NVRTX_DISP_ARMED_RASTER_SIZE];
	sync_end = armed[NVRTX_DISP_ARMED_SYNC_END];
	blank_end = armed[NVRTX_DISP_ARMED_BLANK_END];
	blank_start = armed[NVRTX_DISP_ARMED_BLANK_START];
	output = armed[NVRTX_DISP_ARMED_OUTPUT_RESOURCE];

	/* The horizontal timing. */
	timing->htotal = total & 0xffffU;
	timing->hsync_start = timing->htotal - (blank_end & 0xffffU) - 1U;
	timing->hsync_end = timing->hsync_start + (sync_end & 0xffffU) + 1U;
	timing->hdisplay = (blank_start & 0xffffU) - (blank_end & 0xffffU);

	/* The vertical timing. */
	timing->vtotal = total >> 16;
	timing->vsync_start = timing->vtotal - (blank_end >> 16) - 1U;
	timing->vsync_end = timing->vsync_start + (sync_end >> 16) + 1U;
	timing->vdisplay = (blank_start >> 16) - (blank_end >> 16);

	/* The clock in kHz and the polarities. */
	timing->clock_khz = armed[NVRTX_DISP_ARMED_PIXEL_CLOCK] / 1000U;
	timing->nhsync = 0;
	timing->nvsync = 0;
	if ((output & FIRST_OUTPUT_HSYNC_NEGATIVE) != 0U)
		timing->nhsync = 1;
	if ((output & FIRST_OUTPUT_VSYNC_NEGATIVE) != 0U)
		timing->nvsync = 1;
}

/*
 * Reads the EDID's preferred timing (its first detailed timing).
 *
 * Returns 0, or EINVAL for an EDID too short, a first descriptor that is
 * not a timing, or an interlaced one.
 */
int
nvrtx_disp_first_edid_timing(
	const uint8_t *edid,
	size_t length,
	struct nvrtx_disp_timing *timing)
{
	const uint8_t *d;
	uint32_t clock;
	uint32_t hblank;
	uint32_t vblank;
	uint32_t hfront;
	uint32_t hwidth;
	uint32_t vfront;
	uint32_t vwidth;

	/* The base block holds the descriptor. */
	if (length < FIRST_EDID_BLOCK_BYTES)
		return EINVAL;

	/* A descriptor with no clock is not a timing. */
	d = edid + FIRST_EDID_TIMING_AT;
	clock = (uint32_t)d[0] | ((uint32_t)d[1] << 8);
	if (clock == 0U)
		return EINVAL;

	/* An interlaced mode is not driven. */
	if ((d[17] & 0x80U) != 0U)
		return EINVAL;

	/* The fields: active, blank, front porch and sync width, each with high bits elsewhere. */
	timing->hdisplay = (uint32_t)d[2] | (((uint32_t)d[4] & 0xf0U) << 4);
	hblank = (uint32_t)d[3] | (((uint32_t)d[4] & 0x0fU) << 8);
	timing->vdisplay = (uint32_t)d[5] | (((uint32_t)d[7] & 0xf0U) << 4);
	vblank = (uint32_t)d[6] | (((uint32_t)d[7] & 0x0fU) << 8);
	hfront = (uint32_t)d[8] | (((uint32_t)d[11] & 0xc0U) << 2);
	hwidth = (uint32_t)d[9] | (((uint32_t)d[11] & 0x30U) << 4);
	vfront = ((uint32_t)d[10] >> 4) | (((uint32_t)d[11] & 0x0cU) << 2);
	vwidth = ((uint32_t)d[10] & 0x0fU) | (((uint32_t)d[11] & 0x03U) << 4);

	/* The timing as a mode has it. */
	timing->hsync_start = timing->hdisplay + hfront;
	timing->hsync_end = timing->hsync_start + hwidth;
	timing->htotal = timing->hdisplay + hblank;
	timing->vsync_start = timing->vdisplay + vfront;
	timing->vsync_end = timing->vsync_start + vwidth;
	timing->vtotal = timing->vdisplay + vblank;
	timing->clock_khz = clock * 10U;

	/* The polarities of digital separate sync (bits 4:3 both set): bit 1 horizontal, bit 2 vertical positive. */
	timing->nhsync = 0;
	timing->nvsync = 0;
	if ((d[17] & 0x18U) == 0x18U) {
		if ((d[17] & 0x02U) == 0U)
			timing->nhsync = 1;
		if ((d[17] & 0x04U) == 0U)
			timing->nvsync = 1;
	}

	/* Succeeded: the preferred timing. */
	return 0;
}

/*
 * Reads what the EDID's CEA extensions say of an HDMI sink: whether it is
 * one (the HDMI vendor block), whether it has audio, and the capabilities
 * the HDMI forum block gives (SCDC, scrambling, scrambling below 340 MHz).
 */
void
nvrtx_disp_first_edid_sink(
	const uint8_t *edid,
	size_t length,
	struct nvrtx_disp_sink *sink)
{
	const uint8_t *block;
	uint32_t extensions;
	uint32_t index;
	uint32_t at;
	uint32_t end;
	uint32_t tag;
	uint32_t size;
	uint32_t oui;

	sink->hdmi = 0;
	sink->audio = 0;
	sink->caps = 0U;

	/* The extensions the base block announces, as far as the EDID holds them. */
	if (length < FIRST_EDID_BLOCK_BYTES)
		return;

	extensions = edid[FIRST_EDID_EXTENSIONS_AT];

	/* Each CEA extension's flags and data blocks. */
	for (index = 1U; index <= extensions; index++) {
		if ((size_t)(index + 1U) * FIRST_EDID_BLOCK_BYTES > length)
			break;

		block = edid + (size_t)index * FIRST_EDID_BLOCK_BYTES;
		if (block[0] != FIRST_EDID_CEA_TAG)
			continue;

		/* Basic audio is a flag of the extension. */
		if ((block[3] & FIRST_EDID_CEA_AUDIO) != 0U)
			sink->audio = 1;

		/* The data blocks lie from byte 4 up to the detailed timings' offset. */
		end = block[2];
		if (end > FIRST_EDID_BLOCK_BYTES)
			end = FIRST_EDID_BLOCK_BYTES;

		for (at = 4U; at < end; at += size + 1U) {
			tag = (uint32_t)block[at] >> 5;
			size = (uint32_t)block[at] & 0x1fU;
			if (at + 1U + size > end)
				break;

			/* Only the vendor blocks with their identifier say anything here. */
			if (tag != FIRST_CEA_VENDOR_TAG || size < 3U)
				continue;

			oui = first_oui(block + at + 1U);
			if (oui == FIRST_CEA_HDMI_OUI)
				sink->hdmi = 1;

			/* The forum block's capability byte (byte 6 of the block). */
			if (oui == FIRST_CEA_FORUM_OUI && size >= 6U) {
				if ((block[at + 6U] & FIRST_FORUM_SCDC) != 0U) {
					sink->caps |= FIRST_SINK_SCDC | FIRST_SINK_GT_340;
					if ((block[at + 6U] & FIRST_FORUM_LOW_RATES) != 0U)
						sink->caps |= FIRST_SINK_LTE_340;
				}
			}
		}
	}
}

/*
 * Packs the AVI infoframe of an RGB, full-range picture whose mode has no
 * CEA code into the five words the head's infoframe registers take: the
 * header, then two subpackets of seven bytes, each low word four bytes and
 * high word three.
 */
void
nvrtx_disp_first_avi(
	uint32_t *words)
{
	uint8_t frame[FIRST_AVI_BYTES];
	uint32_t sum;
	uint32_t index;

	/* The header and the payload; the checksum makes the bytes sum to zero. */
	for (index = 0U; index < FIRST_AVI_BYTES; index++)
		frame[index] = 0U;
	frame[0] = FIRST_AVI_TYPE;
	frame[1] = FIRST_AVI_VERSION;
	frame[2] = FIRST_AVI_LENGTH;
	frame[4] = FIRST_AVI_ACTIVE_PRESENT;
	frame[5] = FIRST_AVI_ASPECT_PICTURE;
	frame[6] = FIRST_AVI_FULL_RANGE;
	sum = 0U;
	for (index = 0U; index < FIRST_AVI_BYTES; index++)
		sum += frame[index];
	frame[3] = (uint8_t)(0x100U - (sum & 0xffU));

	/* The words. */
	words[0] = (uint32_t)frame[0] | ((uint32_t)frame[1] << 8) | ((uint32_t)frame[2] << 16);
	words[1] = (uint32_t)frame[3] | ((uint32_t)frame[4] << 8) | ((uint32_t)frame[5] << 16) | ((uint32_t)frame[6] << 24);
	words[2] = (uint32_t)frame[7] | ((uint32_t)frame[8] << 8) | ((uint32_t)frame[9] << 16);
	words[3] = (uint32_t)frame[10] | ((uint32_t)frame[11] << 8) | ((uint32_t)frame[12] << 16) | ((uint32_t)frame[13] << 24);
	words[4] = (uint32_t)frame[14] | ((uint32_t)frame[15] << 8) | ((uint32_t)frame[16] << 16);
}

/*
 * Fills an output LUT with the identity: the header clear, entry i of the
 * 1024 at 16 bits i * 64 in red, green and blue, and the last repeated.
 */
void
nvrtx_disp_first_olut_fill(
	uint8_t *lut)
{
	uint32_t index;
	uint32_t at;
	uint16_t value;

	/* Clears the whole LUT, the header with it. */
	for (at = 0U; at < NVRTX_DISP_LUT_BYTES; at++)
		lut[at] = 0U;

	/* Each entry's three colours. */
	for (index = 0U; index < FIRST_LUT_ENTRIES; index++) {
		value = (uint16_t)(index << 6);
		at = FIRST_LUT_HEADER_BYTES + index * FIRST_LUT_ENTRY_BYTES;
		first_put16(lut, at + 0U, value);
		first_put16(lut, at + 2U, value);
		first_put16(lut, at + 4U, value);
	}

	/* The entry after the last, for the interpolation. */
	first_lut_repeat_last(lut);
}

/*
 * Fills an input LUT with the identity in half floats: entry i of the 1024
 * is i * 64 as a 16-bit fraction, and the last is repeated.
 */
void
nvrtx_disp_first_ilut_fill(
	uint8_t *lut)
{
	uint32_t index;
	uint32_t at;
	uint16_t value;

	/* Clears the whole LUT, the header with it. */
	for (at = 0U; at < NVRTX_DISP_LUT_BYTES; at++)
		lut[at] = 0U;

	/* Each entry's three colours. */
	for (index = 0U; index < FIRST_LUT_ENTRIES; index++) {
		value = first_fp16((uint16_t)(index << 6));
		at = FIRST_LUT_HEADER_BYTES + index * FIRST_LUT_ENTRY_BYTES;
		first_put16(lut, at + 0U, value);
		first_put16(lut, at + 2U, value);
		first_put16(lut, at + 4U, value);
	}

	/* The entry after the last, for the interpolation. */
	first_lut_repeat_last(lut);
}

/*
 * Writes the parameters that make an event object for one of the
 * resource manager's notifications of the display's subdevice.
 */
void
nvrtx_disp_first_event_params(
	uint8_t *params,
	uint32_t client,
	uint32_t notify)
{
	uint32_t at;

	/* Clears the parameters. */
	for (at = 0U; at < NVRTX_DISP_EVENT_BYTES; at++)
		params[at] = 0U;

	/* The client the callback belongs to, the event class, and which notification. */
	nvrtx_put32(params, 0U, client);
	nvrtx_put32(params, 8U, NVRTX_DISP_CLASS_EVENT);
	nvrtx_put32(params, 12U, NVRTX_DISP_EVENT_CLIENT | notify);
}

/*
 * Writes the parameters that ask for a notification each time it happens.
 */
void
nvrtx_disp_first_notification_params(
	uint8_t *params,
	uint32_t notify)
{
	uint32_t at;

	/* Clears the parameters. */
	for (at = 0U; at < NVRTX_DISP_NOTIFICATION_BYTES; at++)
		params[at] = 0U;

	/* The notification, repeated. */
	nvrtx_put32(params, 0U, notify);
	nvrtx_put32(params, 4U, NVRTX_DISP_NOTIFY_REPEAT);
}

/*
 * Writes the parameters of a display control that names one display: the
 * subdevice instance 0 and the display id at byte 4, the rest clear.
 */
void
nvrtx_disp_first_display_params(
	uint8_t *params,
	size_t length,
	uint32_t display_id)
{
	size_t at;

	/* Clears the parameters. */
	for (at = 0U; at < length; at++)
		params[at] = 0U;

	/* The display. */
	nvrtx_put32(params, 4U, display_id);
}

/* Repeats a LUT's last entry after it. */
static void
first_lut_repeat_last(
	uint8_t *lut)
{
	uint32_t last;
	uint32_t next;
	uint32_t index;

	last = FIRST_LUT_HEADER_BYTES + (FIRST_LUT_ENTRIES - 1U) * FIRST_LUT_ENTRY_BYTES;
	next = last + FIRST_LUT_ENTRY_BYTES;

	/* Copies the three colours. */
	for (index = 0U; index < 6U; index++)
		lut[next + index] = lut[last + index];
}

/* Converts a 16-bit fraction to a half float (no sign, the exponent from the leading one, ten bits of mantissa). */
static uint16_t
first_fp16(
	uint16_t fixed)
{
	int32_t exponent;
	uint32_t value;
	uint32_t mantissa;

	/* Zero stays zero. */
	if (fixed == 0U)
		return 0U;

	/* Shifts the leading one to bit 15, counting the shifts below 0. */
	value = fixed;
	exponent = -1;
	while ((value & 0x8000U) == 0U) {
		value <<= 1;
		exponent--;
	}

	/* The mantissa after the leading one, and the biased exponent. */
	mantissa = ((value << 1) & 0xffc0U) >> 6;
	exponent += 15;

	/* Succeeded: the half float. */
	return (uint16_t)(((uint32_t)exponent << 10) | mantissa);
}

/* Reads a CEA vendor block's three-byte identifier (lowest byte first). */
static uint32_t
first_oui(
	const uint8_t *bytes)
{
	/* The identifier. */
	return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) | ((uint32_t)bytes[2] << 16);
}

/* Writes a 16-bit value, low byte first. */
static void
first_put16(
	uint8_t *bytes,
	uint32_t at,
	uint16_t value)
{
	/* The two bytes. */
	bytes[at] = (uint8_t)(value & 0xffU);
	bytes[at + 1U] = (uint8_t)(value >> 8);
}
