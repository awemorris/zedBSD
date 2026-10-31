/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The takeover of the firmware's head (the takeover stage, on by default, off
 * with nvrtx.disp=0).
 *
 * After display-client found the head the firmware (GOP) lit, the driver makes
 * the display's core channel and the head's first window channel through the
 * resource manager, writes their context DMA objects and hash-table entries
 * into the display's instance memory, and sends the head the state it is armed
 * with: the same timing, the same output resource (SOR) and control, so the
 * output keeps running.  The window then shows the firmware's console, kept at
 * the VRAM's start (layout and vram-window), so the console goes on where it
 * was; the GPU device (gpu-device) flips away from it and back.  With
 * nvrtx.disp.test=1 the window shows a surface of the driver's own instead, a
 * flip moves it to a second one, and each step waits 5 s so the monitor can be
 * watched.
 *
 * The order: every channel is made first (the core, then for each head its
 * two windows with their window-immediate channels and its cursor), the
 * core gets its start without a release, the lit output is inherited and
 * the sinks detected, and the first commit gives the head its mode from the
 * sink's preferred timing (the armed timing when there is no EDID) in two
 * core releases around the window's first image.  Nothing is shown anywhere
 * but on the head the firmware lit (the firmware's output is the only output
 * the driver scans out).
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <kern/clock.h>
#include <kern/kmem.h>
#include <uapi/errno.h>

#include "drivers/gpu/nvrtx/nvrtx-gpu.h"

/*
 * The control that registers a display channel's push buffer with the
 * resource manager.
 *
 * Value referenced: NVIDIA open-gpu-kernel-modules 570.144
 * src/common/sdk/nvidia/inc/ctrl/ctrl2080/ctrl2080internal.h
 * (NV2080_CTRL_CMD_INTERNAL_DISPLAY_CHANNEL_PUSHBUFFER).
 */
#define SCANOUT_CTL_PUSHBUFFER		0x20800a58U

/*
 * The handle of the display root the channels are made under
 * (display-client's): the root's class in the high half.
 *
 * Value referenced: NVIDIA open-gpu-kernel-modules 570.144
 * src/common/sdk/nvidia/inc/class/clc570.h (NVC570_DISPLAY) and Linux v6.19
 * drivers/gpu/drm/nouveau/nvkm/subdev/gsp/rm/r535/disp.c (r535_disp_init).
 */
#define SCANOUT_HANDLE_ROOT		0xc5700000U

/* A display channel's push buffer: one page of 1024 words, never wrapped. */
#define SCANOUT_PUSH_BYTES		0x1000U
#define SCANOUT_PUSH_WORDS		(SCANOUT_PUSH_BYTES / 4U)

/*
 * Where the channels' put and get are: the core's user area, and each
 * window's page after it.
 *
 * Values referenced: NVIDIA open-gpu-kernel-modules 570.144
 * src/common/inc/swref/published/disp/v03_00/dev_disp.h
 * (NV_UDISP_FE_CHN_ASSY_BASEADR_CORE, NV_UDISP_FE_CHN_ASSY_BASEADR_WIN),
 * Linux v6.19 drivers/gpu/drm/nouveau/nvkm/subdev/gsp/rm/r535/disp.c
 * (r535_chan_user) and NVIDIA open-gpu-doc classes/display/clc57d.h
 * (NVC57D_PUT, NVC57D_GET).
 */
#define SCANOUT_REG_CORE_USER		0x680000U
#define SCANOUT_REG_WINDOW_USER		0x690000U
#define SCANOUT_WINDOW_USER_STRIDE	0x1000U
#define SCANOUT_USER_PUT_AT		0x0U
#define SCANOUT_USER_GET_AT		0x4U

/*
 * The core's armed state: the core methods' values as last released, from
 * this register on.
 *
 * Value referenced: NVIDIA open-gpu-kernel-modules 570.144
 * src/common/inc/swref/published/disp/v03_00/dev_disp.h
 * (NV_UDISP_FE_CHN_ARMED_BASEADR_CORE).
 */
#define SCANOUT_REG_CORE_ARMED		0x688000U

/*
 * The notifiers: 4 KiB of VRAM just after the instance memory; the core's at
 * 0, window w's at (1 + w) * 64 + 32.
 *
 * Values referenced: Linux v6.19 drivers/gpu/drm/nouveau/dispnv50/disp.h
 * (NV50_DISP_SYNC, NV50_DISP_CORE_NTFY, NV50_DISP_WNDW_NTFY) and NVIDIA
 * open-gpu-doc classes/display/clc57d.h (NV_DISP_NOTIFIER_SIZEOF).
 */
#define SCANOUT_SYNC_AFTER_INSTANCE	0x10000U
#define SCANOUT_SYNC_BYTES		0x1000U
#define SCANOUT_CORE_NOTIFIER_AT	0x0U
#define SCANOUT_WINDOW_SLOT_BYTES	0x40U
#define SCANOUT_WINDOW_NOTIFIER_AT	0x20U
#define SCANOUT_NOTIFIER_BYTES		0x10U

/* How long a notifier is waited for, and the polling step. */
#define SCANOUT_NOTIFIER_TIMEOUT_US	2000000U
#define SCANOUT_NOTIFIER_STEP_US	100U

/* How long each test picture stays up, so it can be seen on the monitor. */
#define SCANOUT_SHOW_US			10000000U
#define SCANOUT_SHOW_RESIDENT_US	3000000U
#define SCANOUT_SHOW_STEP_US		100000U

/* The surfaces: rows in 256-byte steps, each surface on its own 1 MiB step. */
#define SCANOUT_PITCH_ALIGN		256U
#define SCANOUT_SURFACE_ALIGN		0x100000ULL
#define SCANOUT_BAND_COUNT		8U

/*
 * The output resource a head can be driven by: the SOR's index from
 * display-client, at most 8.
 */
#define SCANOUT_SOR_MAX			8U

/* The most words one frame's methods take in a window's push buffer (the image, the flip and the update). */
#define SCANOUT_FRAME_WORDS_MAX		64U

/* The most words a change of the head's view takes in the core (the view's 4, the release's 9), with a margin. */
#define SCANOUT_VIEW_WORDS_MAX		32U

/*
 * The SOR capability read once for each SOR output (the caps area's word at
 * 0x13c).
 */
#define SCANOUT_REG_SOR_CAPS		0x64013cU

/*
 * The heads' HDMI infoframe registers: the AVI's control and words, the
 * vendor frame's control, one block of 0x400 for each head.
 *
 * Values referenced: Linux v6.19
 * drivers/gpu/drm/nouveau/nvkm/engine/disp/gv100.c
 * (gv100_sor_hdmi_infoframe_avi, gv100_sor_hdmi_infoframe_vsi).
 */
#define SCANOUT_REG_AVI_CONTROL		0x6f0000U
#define SCANOUT_REG_AVI_HEADER		0x6f0008U
#define SCANOUT_REG_VSI_CONTROL		0x6f0100U
#define SCANOUT_INFOFRAME_STRIDE	0x400U
#define SCANOUT_INFOFRAME_ENABLE	0x00000001U
#define SCANOUT_VSI_DISABLE_MASK	0x00010001U

/* The LUTs: the head's output LUT and the window's input LUT, in the VRAM after the notifiers. */
#define SCANOUT_OLUT_AFTER_INSTANCE	0x11000U
#define SCANOUT_ILUT_AFTER_INSTANCE	0x14000U

/*
 * The protocols of a TMDS output (single link A or B, dual link): the
 * takeover drives only these.
 *
 * Values referenced: NVIDIA open-gpu-doc classes/display/clc57d.h
 * (NVC57D_SOR_SET_CONTROL_PROTOCOL_SINGLE_TMDS_A, _SINGLE_TMDS_B,
 * _DUAL_TMDS).
 */
#define SCANOUT_PROTOCOL_TMDS_A		1U
#define SCANOUT_PROTOCOL_TMDS_B		2U
#define SCANOUT_PROTOCOL_TMDS_DUAL	5U

/*
 * The clock from which a dual-link TMDS output uses both links.
 *
 * Value referenced: Linux v6.19 drivers/gpu/drm/nouveau/dispnv50/disp.c
 * (nv50_sor_atomic_enable).
 */
#define SCANOUT_DUAL_LINK_KHZ		165000U

/* How long each step of the takeover waits after its mark, so the monitor can be watched at it. */
#define SCANOUT_STEP_US			5000000U

/*
 * The display's error state, read when a notifier does not come: the core's
 * exception, the other exceptions, the supervisor and the core's state.
 *
 * Values referenced: Linux v6.19
 * drivers/gpu/drm/nouveau/nvkm/engine/disp/gv100.c (gv100_disp_exception,
 * gv100_disp_intr_exc_other, gv100_disp_intr_ctrl_disp,
 * gv100_disp_core_idle, gv100_disp_dmac_idle, gv100_disp_super).
 */
#define SCANOUT_REG_CORE_EXCEPTION	0x611020U
#define SCANOUT_REG_CORE_EXCEPTION_DATA	0x611024U
#define SCANOUT_REG_CORE_EXCEPTION_CODE	0x611028U
#define SCANOUT_REG_EXCEPTION_OTHER	0x611854U
#define SCANOUT_REG_SUPERVISOR		0x611c30U
#define SCANOUT_REG_CORE_STATE		0x610630U
#define SCANOUT_REG_ERROR_DISP		0x611848U
#define SCANOUT_REG_SUPERVISOR_MAIN	0x6107a8U
#define SCANOUT_REG_WINDOW_EXCEPTION	0x61102cU
#define SCANOUT_EXCEPTION_STRIDE	12U
#define SCANOUT_REG_WINDOW_STATUS	0x610664U
#define SCANOUT_REG_WIMM_STATUS		0x6106e4U

/*
 * The output resource's control: the heads that own it are bits 7:0.
 *
 * Value referenced: NVIDIA open-gpu-doc classes/display/clc57d.h
 * (NVC57D_SOR_SET_CONTROL_OWNER_MASK).
 */
#define SCANOUT_SOR_OWNER_MASK		0xffU

/*
 * The cursor channels' user areas: one page for each head.
 *
 * Values referenced: Linux v6.19
 * drivers/gpu/drm/nouveau/nvkm/subdev/gsp/rm/r535/disp.c (r535_chan_user)
 * and NVIDIA open-gpu-kernel-modules 570.144
 * src/common/inc/swref/published/disp/v03_00/dev_disp.h
 * (NV_UDISP_FE_CHN_ASSY_BASEADR_CURS).
 */
#define SCANOUT_REG_CURSOR_USER		0x6d8000U
#define SCANOUT_CURSOR_USER_STRIDE	0x1000U

/*
 * A cursor channel is written once it has four free entries, waited for at
 * most 100 ms.
 *
 * Values referenced: Linux v6.19 drivers/gpu/drm/nouveau/dispnv50/curs507a.c
 * (curs507a_space).
 */
#define SCANOUT_CURSOR_SPACE_LEAST	4U
#define SCANOUT_CURSOR_SPACE_US		100000U

/*
 * The cursor channel's update: release the gathered state (RELEASE_ELV).
 *
 * Value referenced: Linux v6.19 drivers/gpu/drm/nouveau/dispnv50/cursc37a.c
 * (cursc37a_update) and NVIDIA open-gpu-doc classes/display/clc57a.h
 * (NVC57A_UPDATE_RELEASE_ELV).
 */
#define SCANOUT_CURSOR_RELEASE		0x00000001U

/* How long the core channel is given to fetch what it has before more is written over it, and the polling step. */
#define SCANOUT_DRAIN_US		100000U
#define SCANOUT_DRAIN_STEP_US		50U

/* The most words a cursor's core methods and update take (the cursor's 7, the update's 9), with a margin. */
#define SCANOUT_CURSOR_WORDS_MAX	32U

/*
 * The heads' raster positions, whose top half counts frames (RG_DPCA): one
 * block of 0x800 for each head.
 *
 * Values referenced: NVIDIA open-gpu-kernel-modules 570.144
 * src/common/inc/swref/published/disp/v03_00/dev_disp.h (NV_PDISP_RG_DPCA).
 */
#define SCANOUT_REG_RG_DPCA		0x616330U
#define SCANOUT_RG_STRIDE		0x800U

static int scanout_read_armed(struct nvrtx_gpu *gpu);
static int scanout_surfaces(struct nvrtx_gpu *gpu);
static int scanout_console_surface(struct nvrtx_gpu *gpu, uint32_t width, uint32_t height);
static int scanout_surface_objects(struct nvrtx_gpu *gpu, uint64_t span);
static int scanout_paint(struct nvrtx_gpu *gpu, const struct nvrtx_disp_surface *surface, int reversed);
static int scanout_channel(struct nvrtx_gpu *gpu, uint32_t class_number, uint32_t instance, struct nvrtx_buffer *buffer, struct nvrtx_push *push);
static int scanout_bind(struct nvrtx_gpu *gpu, uint32_t chid, uint32_t handle, uint32_t flags, uint64_t start, uint64_t limit);
static int scanout_channels(struct nvrtx_gpu *gpu);
static int scanout_cursor_channel(struct nvrtx_gpu *gpu, uint32_t head);
static int scanout_mode(struct nvrtx_gpu *gpu);
static int scanout_luts(struct nvrtx_gpu *gpu);
static int scanout_commit(struct nvrtx_gpu *gpu);
static void scanout_infoframes(struct nvrtx_gpu *gpu);
static int scanout_release_core(struct nvrtx_gpu *gpu, uint32_t window_interlock, const char *what);
static int scanout_kick(struct nvrtx_gpu *gpu, struct nvrtx_push *push, uint32_t user);
static int scanout_notifier_reset(struct nvrtx_gpu *gpu, uint32_t offset);
static int scanout_notifier_wait(struct nvrtx_gpu *gpu, uint32_t offset, uint32_t least, uint32_t user, const char *what);
static void scanout_step(const char *what);
static void scanout_diagnose(struct nvrtx_gpu *gpu);
static int scanout_flip(struct nvrtx_gpu *gpu);
static void scanout_hold(uint32_t microseconds);
static uint32_t scanout_window_user(uint32_t window);
static uint32_t scanout_window_notifier(uint32_t window);
static uint32_t scanout_next_notifier(struct nvrtx_gpu *gpu);
static void scanout_diagnose_window(struct nvrtx_gpu *gpu);
static int scanout_drained(struct nvrtx_gpu *gpu, uint32_t user, uint32_t put);
static int scanout_core_room(struct nvrtx_gpu *gpu, uint32_t words);
static int scanout_core_send_cursor(struct nvrtx_gpu *gpu);
static int scanout_cursor_write(struct nvrtx_gpu *gpu, uint32_t method, uint32_t value);

/*
 * Runs the takeover stage: makes the display's channels, inherits the output
 * the firmware lit, detects the sinks and commits the first mode on the lit
 * head, showing the console on it (with the monitor test, a surface of bands
 * and a flip).
 *
 * Without a head the firmware lit, or without a console the head can show,
 * nothing is committed (the scanout rule) and display.taken stays 0.  A
 * failure is returned; the finish's stop frees the channels that were made.
 */
int
nvrtx_scanout_start(
	struct nvrtx_gpu *gpu)
{
	struct nvrtx_display_state *display;
	uint32_t hold_us;
	bool resident;
	bool test;
	bool own;
	int error;

	display = &gpu->display;
	test = nvrtx_stage_disp_test_wanted();

	/* The notifiers live in 4 KiB after the instance memory, cleared. */
	nvrtx_stage_mark("takeover begin");
	display->sync = display->instance + SCANOUT_SYNC_AFTER_INSTANCE;
	error = nvrtx_vram_zero(gpu, display->sync, SCANOUT_SYNC_BYTES);
	if (error != 0)
		return error;

	/* Makes every channel, the core's start first. */
	error = scanout_channels(gpu);
	if (error != 0)
		return error;

	scanout_step("1: channels made");

	/* Inherits the lit output and detects the sinks. */
	error = nvrtx_display_inherit(gpu);
	if (error != 0)
		return error;

	error = nvrtx_display_detect(gpu);
	if (error != 0)
		return error;

	/* Shows nothing anywhere the firmware did not light. */
	if (display->gop_head_found == 0) {
		nvrtx_stage_mark("takeover: no head lit by the firmware; nothing committed");
		return 0;
	}

	/* The head's first window is the one it shows; the hardware must have it. */
	display->window = display->gop_head * 2U;
	if ((display->windows_made & (1U << display->window)) == 0U) {
		nvrtx_stage_mark("takeover fail: window %u of head %u is absent", (unsigned)display->window, (unsigned)display->gop_head);
		return ENODEV;
	}

	nvrtx_push_init(&display->window_push, (uint8_t *)display->window_buffers[display->window].dma.address, SCANOUT_PUSH_WORDS);
	display->window_made = 1;
	if ((display->cursors_made & (1U << display->gop_head)) != 0U)
		display->cursor_made = 1;

	/* Reads the armed state for the log and the checks, then the mode from the EDID or the armed timing. */
	error = scanout_read_armed(gpu);
	if (error != 0)
		return error;

	error = scanout_mode(gpu);
	if (error != 0)
		return error;

	/* Places the surface to show; a console that cannot be shown leaves the head alone. */
	error = scanout_surfaces(gpu);
	if (error == ENOTSUP)
		return 0;
	if (error != 0)
		return error;

	/* The identity LUTs. */
	error = scanout_luts(gpu);
	if (error != 0)
		return error;

	/* The first commit. */
	error = scanout_commit(gpu);
	if (error != 0)
		return error;

	/* The console is on the head again: the takeover is done. */
	own = nvrtx_stage_disp_own_wanted();
	if (!test && !own) {
		display->taken = 1;
		nvrtx_stage_mark("takeover ok: head %u shows the console", (unsigned)display->gop_head);
		return 0;
	}

	/* The driver's own surface is on the head, without the test's flip and waits: the takeover is done. */
	if (!test) {
		display->taken = 1;
		nvrtx_stage_mark("takeover ok: head %u shows the driver's surface", (unsigned)display->gop_head);
		return 0;
	}

	/* Leaves the first picture up, then flips to the second and leaves that up too (shorter when the GPU stays up). */
	hold_us = SCANOUT_SHOW_US;
	resident = nvrtx_stage_resident_wanted();
	if (resident)
		hold_us = SCANOUT_SHOW_RESIDENT_US;
	scanout_hold(hold_us);
	error = scanout_flip(gpu);
	if (error != 0)
		return error;

	scanout_hold(hold_us);

	/* Succeeded: the head shows the driver's surface. */
	display->taken = 1;
	nvrtx_stage_mark("takeover ok: head %u shows the driver's surfaces", (unsigned)display->gop_head);
	return 0;
}

/*
 * Frees every channel takeover made before the display's root: the cursors, the
 * window-immediate channels, the windows, the core.  Failures are only shown.
 */
void
nvrtx_scanout_stop(
	struct nvrtx_gpu *gpu)
{
	struct nvrtx_display_state *display;
	uint32_t index;
	uint32_t handle;
	int failures;
	int error;

	display = &gpu->display;
	failures = 0;

	/* Frees each cursor, window-immediate and window channel that was made. */
	for (index = 0U; index < NVRTX_DISP_WINDOWS; index++) {
		if ((display->cursors_made & (1U << index)) != 0U) {
			handle = (gpu->identity.chip.profile->class_cursor << 16) | index;
			error = nvrtx_rm_free_in(gpu, NVRTX_DISP_CLIENT, handle);
			if (error != 0)
				failures++;
		}
	}

	for (index = 0U; index < NVRTX_DISP_WINDOWS; index++) {
		if ((display->wimms_made & (1U << index)) != 0U) {
			handle = (gpu->identity.chip.profile->class_wimm << 16) | index;
			error = nvrtx_rm_free_in(gpu, NVRTX_DISP_CLIENT, handle);
			if (error != 0)
				failures++;
		}
	}

	for (index = 0U; index < NVRTX_DISP_WINDOWS; index++) {
		if ((display->windows_made & (1U << index)) != 0U) {
			handle = (gpu->identity.chip.profile->class_window << 16) | index;
			error = nvrtx_rm_free_in(gpu, NVRTX_DISP_CLIENT, handle);
			if (error != 0)
				failures++;
		}
	}

	display->cursors_made = 0U;
	display->wimms_made = 0U;
	display->windows_made = 0U;
	display->cursor_made = 0;
	display->window_made = 0;

	/* Frees the core channel. */
	if (display->core_made != 0) {
		handle = gpu->identity.chip.profile->class_core << 16;
		error = nvrtx_rm_free_in(gpu, NVRTX_DISP_CLIENT, handle);
		if (error != 0)
			failures++;
		display->core_made = 0;
	}

	/* Says how the stop went, when anything failed. */
	if (failures != 0)
		nvrtx_stage_mark("takeover: %d channel frees failed; the unload removes the rest", failures);
}

/*
 * Gives back the channels' push buffers.
 *
 * Only once the display no longer reads them: after the GSP is stopped.
 */
void
nvrtx_scanout_release(
	struct nvrtx_gpu *gpu)
{
	uint32_t index;

	/* Frees each push buffer that was allocated. */
	nvrtx_buffer_free(gpu, &gpu->display.core_buffer);
	for (index = 0U; index < NVRTX_DISP_WINDOWS; index++) {
		nvrtx_buffer_free(gpu, &gpu->display.window_buffers[index]);
		nvrtx_buffer_free(gpu, &gpu->display.wimm_buffers[index]);
	}
}

/*
 * Shows a surface on the taken-over head's window and waits until the flip
 * has begun (the GPU device's frames, the gpu-device stage): the flip is sent
 * (nvrtx_scanout_frame_send()) and its notifier waited for
 * (nvrtx_scanout_frame_wait()).  Returns as the two do.
 */
int
nvrtx_scanout_frame(
	struct nvrtx_gpu *gpu,
	const struct nvrtx_disp_surface *surface)
{
	uint32_t notifier;
	int error;

	/* Sends the flip. */
	error = nvrtx_scanout_frame_send(gpu, surface, &notifier);
	if (error != 0)
		return error;

	/* Waits until the head has begun to show it. */
	error = nvrtx_scanout_frame_wait(gpu, notifier);
	if (error != 0)
		return error;

	/* Succeeded: the head shows the surface. */
	return 0;
}

/*
 * Sends the flip of the taken-over head's window to a surface; its
 * notifier, which says when the flip has begun, is given in *notifier.
 *
 * The window gets the surface's whole image state, so its size, pitch and
 * format may differ from the last one's.  A surface of another size than
 * the head's view (the console smaller than the mode, and a frame of the
 * mode's size after it, or back) changes the view with the flip: the core
 * takes the new viewport input size, the window's release is interlocked
 * with the core and the core's with the window, as in a commit of a head
 * and its plane together.  When a push buffer has no room left it is wound
 * back to its start first.  The caller serializes the frames and the
 * cursor's core updates, and waits for the notifier before the next flip is
 * sent (the window's two notifiers alternate).  Returns ETIMEDOUT when a
 * change of the view does not finish in 2 s.
 *
 * Procedure referenced: Linux v6.19 drivers/gpu/drm/nouveau/dispnv50/disp.c
 * (nv50_disp_atomic_commit_tail, nv50_dmac_wind).
 */
int
nvrtx_scanout_frame_send(
	struct nvrtx_gpu *gpu,
	const struct nvrtx_disp_surface *surface,
	uint32_t *notifier)
{
	struct nvrtx_display_state *display;
	struct nvrtx_push *push;
	uint32_t user;
	uint32_t view_in;
	int with_core;
	int error;

	display = &gpu->display;
	push = &display->window_push;
	user = scanout_window_user(display->window);

	/* The head's view changes with the flip when the surface is not of the view's size. */
	view_in = (surface->height << 16) | (surface->width & 0xffffU);
	with_core = 0;
	if (view_in != display->view_in)
		with_core = 1;

	/* Writes the head's new view into the core, without a release yet, when it changes. */
	if (with_core != 0) {
		/* The core's notifier is about to be reused: a cursor's update still out finishes first. */
		error = nvrtx_scanout_core_settled(gpu, SCANOUT_NOTIFIER_TIMEOUT_US);
		if (error == EBUSY)
			return ETIMEDOUT;
		if (error != 0)
			return error;

		/* Makes room in the core for the view and the release after it. */
		error = scanout_core_room(gpu, SCANOUT_VIEW_WORDS_MAX);
		if (error != 0)
			return error;

		/* The viewport's input is the surface's size, its output the mode's visible size. */
		nvrtx_disp_first_core_view(&display->core_push, display->gop_head, view_in, display->mode.active);
	}

	/*
	 * Winds the buffer back to its start when the frame might not fit
	 * before its end: a jump to the start where the get stands, and the put
	 * moved to the start, so the channel fetches the jump and stops there
	 * (the put never covers the jump, or the channel would run the old
	 * words again after it).
	 */
	if (push->count + SCANOUT_FRAME_WORDS_MAX + 1U > push->capacity) {
		nvrtx_push_jump(push, 0U);
		if (push->overflow != 0)
			return E2BIG;

		push->count = 0U;
		__atomic_thread_fence(__ATOMIC_SEQ_CST);
		nvrtx_wr32(gpu, user + SCANOUT_USER_PUT_AT, 0U);
	}

	/* Clears the window's next notifier, then sends the flip with it and the whole image, and the window's release. */
	*notifier = scanout_next_notifier(gpu);
	error = scanout_notifier_reset(gpu, *notifier);
	if (error != 0)
		return error;

	nvrtx_disp_first_window_flip(push, display->window, surface, gpu->identity.chip.profile->window_storage_pitch, *notifier, with_core);
	error = scanout_kick(gpu, push, user);
	if (error != 0)
		return error;

	/* Completes a change of the view: the core's release, interlocked with the window, waited for. */
	if (with_core != 0) {
		error = scanout_release_core(gpu, 1U << display->window, NULL);
		if (error != 0)
			return error;

		/* The head's view is the surface's size from this frame on. */
		display->view_in = view_in;
		nvrtx_stage_mark("gpu-device view %ux%u shown at %ux%u",
		    (unsigned)surface->width,
		    (unsigned)surface->height,
		    (unsigned)(display->mode.active & 0xffffU),
		    (unsigned)(display->mode.active >> 16));
	}

	/* Succeeded: the flip is out; its notifier says when it has begun. */
	return 0;
}

/*
 * Waits until the flip sent with a notifier has begun: from then on the
 * head reads the new surface and no longer the one before it.  The wait
 * reads the notifier alone and touches no channel, so it needs only the
 * caller's serialization of the frames, not the cursor's.  Returns
 * ETIMEDOUT when the flip does not begin in 2 s (the window then still
 * reads the surface it had or the new one).
 */
int
nvrtx_scanout_frame_wait(
	struct nvrtx_gpu *gpu,
	uint32_t notifier)
{
	uint32_t user;
	int error;

	/* The window channel's user area, for the report of a notifier that does not come. */
	user = scanout_window_user(gpu->display.window);

	/* Waits until the flip has begun. */
	error = scanout_notifier_wait(gpu, notifier, 1U, user, NULL);
	if (error != 0)
		return error;

	/* Succeeded: the head shows the surface. */
	return 0;
}

/*
 * Reports whether the taken-over head has its cursor channel (the gpu-device
 * stage): takeover makes every head's cursor channel with the others.  Without
 * it the display has no cursor plane.
 *
 * Procedure referenced: Linux v6.19 drivers/gpu/drm/nouveau/dispnv50/head.c
 * (nv50_head_create).
 */
int
nvrtx_scanout_cursor_start(
	struct nvrtx_gpu *gpu)
{
	struct nvrtx_display_state *display;

	display = &gpu->display;

	/* The head's cursor channel was made with the others. */
	if (display->cursor_made == 0) {
		nvrtx_stage_mark("gpu-device: head %u has no cursor channel", (unsigned)display->gop_head);
		return ENODEV;
	}

	/* Succeeded: the head's cursor takes a position and an update. */
	nvrtx_stage_mark("gpu-device channel %04x.%u made", (unsigned)gpu->identity.chip.profile->class_cursor, (unsigned)display->gop_head);
	return 0;
}

/*
 * Shows the head's cursor: the image at a VRAM offset (64 by 64 A8R8G8B8 on
 * a 256-byte boundary) with its top left at a position word.
 *
 * As a commit of a cursor plane: the cursor channel takes the position and
 * its update, the core the cursor's control, object and offset, then the
 * core's update without an interlock.  The core's notifier is asked for but
 * not waited for (core_pending is set); the caller first makes sure no
 * earlier one is out (nvrtx_scanout_core_settled).  Returns 0, ETIMEDOUT
 * when a channel does not take the words, or E2BIG.
 *
 * Procedure referenced: Linux v6.19 drivers/gpu/drm/nouveau/dispnv50/wndw.c
 * (nv50_wndw_flush_set) and disp.c (nv50_disp_atomic_commit_tail).
 */
int
nvrtx_scanout_cursor_show(
	struct nvrtx_gpu *gpu,
	uint64_t offset,
	uint32_t point)
{
	struct nvrtx_display_state *display;
	int error;

	display = &gpu->display;

	/* The cursor channel: the position, then its update. */
	error = scanout_cursor_write(gpu, NVRTX_CURSOR_POINT_OUT, point);
	if (error != 0)
		return error;

	error = scanout_cursor_write(gpu, NVRTX_CURSOR_UPDATE, SCANOUT_CURSOR_RELEASE);
	if (error != 0)
		return error;

	/* The core, once it has room: the cursor on, read through the VRAM object. */
	error = scanout_core_room(gpu, SCANOUT_CURSOR_WORDS_MAX);
	if (error != 0)
		return error;

	nvrtx_disp_core_cursor_set(&display->core_push, display->gop_head, NVRTX_DISP_HANDLE_VRAM, offset);

	/* The core's update with the cursor, not waited for. */
	error = scanout_core_send_cursor(gpu);
	if (error != 0)
		return error;

	/* Succeeded: the cursor shows from a coming frame on. */
	return 0;
}

/*
 * Moves the shown cursor: the cursor channel takes the position and its
 * update alone, no core update (a legacy cursor move).
 *
 * Procedure referenced: Linux v6.19 drivers/gpu/drm/nouveau/dispnv50/disp.c
 * (nv50_disp_atomic_commit_tail, the legacy cursor update).
 */
int
nvrtx_scanout_cursor_point(
	struct nvrtx_gpu *gpu,
	uint32_t point)
{
	int error;

	/* The position, then the update. */
	error = scanout_cursor_write(gpu, NVRTX_CURSOR_POINT_OUT, point);
	if (error != 0)
		return error;

	error = scanout_cursor_write(gpu, NVRTX_CURSOR_UPDATE, SCANOUT_CURSOR_RELEASE);
	if (error != 0)
		return error;

	/* Succeeded: the cursor moves from a coming frame on. */
	return 0;
}

/*
 * Hides the head's cursor: the core's cursor off and without its object,
 * then the core's update without an interlock (the clear of a cursor
 * plane), its notifier asked for but not waited for.  The caller first
 * makes sure no earlier one is out.
 *
 * Procedure referenced: Linux v6.19 drivers/gpu/drm/nouveau/dispnv50/wndw.c
 * (nv50_wndw_flush_clr).
 */
int
nvrtx_scanout_cursor_clear(
	struct nvrtx_gpu *gpu)
{
	struct nvrtx_display_state *display;
	int error;

	display = &gpu->display;

	/* The core, once it has room: the cursor off. */
	error = scanout_core_room(gpu, SCANOUT_CURSOR_WORDS_MAX);
	if (error != 0)
		return error;

	nvrtx_disp_core_cursor_clear(&display->core_push, display->gop_head);

	/* The core's update with the cursor, not waited for. */
	error = scanout_core_send_cursor(gpu);
	if (error != 0)
		return error;

	/* Succeeded: the cursor is gone from a coming frame on. */
	return 0;
}

/*
 * Reports whether the core update last sent without a wait (a cursor's)
 * has finished, waiting for it at most wait_us.  Returns 0 when none is out
 * (core_pending is then clear), EBUSY while it still is, or the error of
 * reading its notifier.
 */
int
nvrtx_scanout_core_settled(
	struct nvrtx_gpu *gpu,
	uint32_t wait_us)
{
	struct nvrtx_display_state *display;
	uint64_t start;
	uint64_t waited;
	uint32_t word;
	int error;

	display = &gpu->display;

	/* Nothing is out. */
	if (display->core_pending == 0)
		return 0;

	/* Polls the core's notifier until it says finished or the time is spent. */
	start = nvrtx_now_us();
	for (;;) {
		error = nvrtx_vram_read32(gpu, display->sync + SCANOUT_CORE_NOTIFIER_AT, &word);
		if (error != 0)
			return error;

		/* The update is done. */
		if ((word >> NVRTX_DISP_NOTIFIER_STATUS_SHIFT) >= NVRTX_DISP_NOTIFIER_FINISHED)
			break;

		/* Still out after the time the caller gives it. */
		waited = nvrtx_now_us() - start;
		if (waited >= wait_us)
			return EBUSY;

		kern_usleep_range(SCANOUT_NOTIFIER_STEP_US, SCANOUT_NOTIFIER_STEP_US * 2U);
	}

	/* Succeeded: nothing of the update reads the old state any more. */
	display->core_pending = 0;
	return 0;
}

/*
 * Reports the taken-over head's frame counter: 16 bits that move by one
 * each frame the head scans out (the raster generator's RG_DPCA), read
 * without side effects.
 */
uint32_t
nvrtx_scanout_frame_counter(
	struct nvrtx_gpu *gpu)
{
	uint32_t position;

	/* The head's raster position: the line in the low half, the frame in the high half. */
	position = nvrtx_rd32(gpu, SCANOUT_REG_RG_DPCA + gpu->display.gop_head * SCANOUT_RG_STRIDE);

	/* Succeeded: the frame count. */
	return (position >> NVRTX_DISP_FRAME_COUNT_SHIFT) & NVRTX_DISP_FRAME_COUNT_MASK;
}

/* Reads the head's armed methods (in the copied order) and its output resource's control. */
static int
scanout_read_armed(
	struct nvrtx_gpu *gpu)
{
	struct nvrtx_display_state *display;
	uint32_t head_base;
	uint32_t index;

	display = &gpu->display;

	/* An output display-client could not name cannot be kept. */
	if (display->gop_sor >= SCANOUT_SOR_MAX) {
		nvrtx_stage_mark("takeover fail: head %u has no output resource (%x)", (unsigned)display->gop_head, (unsigned)display->gop_sor);
		return ENODEV;
	}

	/* Reads each copied method of the head. */
	head_base = SCANOUT_REG_CORE_ARMED + NVRTX_CORE_HEAD_FIRST + display->gop_head * NVRTX_CORE_HEAD_STRIDE;
	for (index = 0U; index < NVRTX_DISP_HEAD_COPIED; index++)
		display->armed[index] = nvrtx_rd32(gpu, head_base + nvrtx_disp_head_copied[index]);

	/* Reads the output resource's control: its owner heads and protocol. */
	display->sor_control = nvrtx_rd32(gpu, SCANOUT_REG_CORE_ARMED + NVRTX_CORE_SOR_SET_CONTROL + display->gop_sor * NVRTX_CORE_SOR_STRIDE);

	/* Shows the viewport, the raster and the control that will be sent back. */
	nvrtx_stage_mark("takeover head %u armed: view %08x raster %08x clock %08x, SOR %u %08x",
	    (unsigned)display->gop_head,
	    (unsigned)display->armed[NVRTX_DISP_ARMED_VIEWPORT_IN],
	    (unsigned)display->armed[NVRTX_DISP_ARMED_RASTER_SIZE],
	    (unsigned)display->armed[NVRTX_DISP_ARMED_PIXEL_CLOCK],
	    (unsigned)display->gop_sor,
	    (unsigned)display->sor_control);

	/*
	 * Sends nothing when the armed state does not describe the picture on
	 * the screen: a raster of zero or an output not owned by the head would
	 * turn the firmware's picture off.
	 */
	if (display->armed[NVRTX_DISP_ARMED_RASTER_SIZE] == 0U) {
		nvrtx_stage_mark("takeover fail: head %u has no armed raster; the picture is left alone", (unsigned)display->gop_head);
		return ENODEV;
	}

	if ((display->sor_control & SCANOUT_SOR_OWNER_MASK & (1U << display->gop_head)) == 0U) {
		nvrtx_stage_mark("takeover fail: SOR %u is not armed for head %u; the picture is left alone", (unsigned)display->gop_sor, (unsigned)display->gop_head);
		return ENODEV;
	}

	/* A DisplayPort output needs a link training this driver does not do yet. */
	if (display->gop_protocol != SCANOUT_PROTOCOL_TMDS_A &&
	    display->gop_protocol != SCANOUT_PROTOCOL_TMDS_B &&
	    display->gop_protocol != SCANOUT_PROTOCOL_TMDS_DUAL) {
		nvrtx_stage_mark("takeover: protocol %u is not TMDS; the picture is left alone", (unsigned)display->gop_protocol);
		return ENODEV;
	}

	/* The mode the head is set to: the armed raster and clock, the visible size from the blank. */
	nvrtx_disp_mode_from_armed(display->armed, &display->mode);
	nvrtx_stage_mark("takeover head %u mode: %ux%u, view out %08x",
	    (unsigned)display->gop_head,
	    (unsigned)(display->mode.active & 0xffffU),
	    (unsigned)(display->mode.active >> 16),
	    (unsigned)display->armed[NVRTX_DISP_ARMED_VIEWPORT_OUT]);

	/* Succeeded: the head and its output can be sent back as they are. */
	return 0;
}

/*
 * Places the notifiers after the instance memory and the two surfaces below
 * the graphics and compute arenas' reserve, and paints the surfaces.
 */
static int
scanout_surfaces(
	struct nvrtx_gpu *gpu)
{
	struct nvrtx_display_state *display;
	struct nvrtx_disp_surface *surface;
	uint64_t span;
	uint64_t candidate;
	uint32_t region;
	uint32_t width;
	uint32_t height;
	unsigned index;
	bool test;
	bool own;
	int error;

	display = &gpu->display;

	/* The surface has the mode's visible size (the viewport in and out). */
	width = display->mode.active & 0xffffU;
	height = display->mode.active >> 16;
	if (width == 0U || height == 0U || width > 0x4000U || height > 0x4000U) {
		nvrtx_stage_mark("takeover fail: the armed visible size is %ux%u", (unsigned)width, (unsigned)height);
		return EINVAL;
	}

	/* Without the monitor test or the driver's own surface the head shows the console. */
	test = nvrtx_stage_disp_test_wanted();
	own = nvrtx_stage_disp_own_wanted();
	if (!test && !own) {
		error = scanout_console_surface(gpu, width, height);
		if (error != 0)
			return error;

		return 0;
	}

	/*
	 * Takes two surfaces from the VRAM allocator (vram-window), mapped in
	 * the CPU's window when there is one; without the allocator, under the
	 * graphics and compute arenas' reserve in the last usable region, as
	 * before.
	 */
	span = (uint64_t)((width * 4U + SCANOUT_PITCH_ALIGN - 1U) & ~(SCANOUT_PITCH_ALIGN - 1U)) * height;
	span = (span + SCANOUT_SURFACE_ALIGN - 1U) & ~(SCANOUT_SURFACE_ALIGN - 1U);
	if (gpu->memory_state.ready != 0) {
		error = scanout_surface_objects(gpu, span);
	} else {
		error = nvrtx_mmu_vram_block(gpu->static_info, NVRTX_COMPUTE_ARENA_RESERVE + 2U * span, &candidate, &region);
		display->surface_objects[0].vram = candidate;
		display->surface_objects[1].vram = candidate + span;
	}
	if (error != 0) {
		nvrtx_stage_mark("takeover fail: no VRAM for the surfaces (%d)", error);
		return error;
	}

	/* Describes both surfaces: the same size and pitch, read through the VRAM object. */
	for (index = 0U; index < 2U; index++) {
		surface = &display->surfaces[index];
		surface->offset = display->surface_objects[index].vram;
		surface->width = width;
		surface->height = height;
		surface->pitch = (width * 4U + SCANOUT_PITCH_ALIGN - 1U) & ~(SCANOUT_PITCH_ALIGN - 1U);
		surface->handle = NVRTX_DISP_HANDLE_FB;
		surface->format = NVRTX_DISP_FORMAT_A8R8G8B8;
	}

	/* Paints the bands top to bottom on the first, bottom to top on the second. */
	error = scanout_paint(gpu, &display->surfaces[0], 0);
	if (error != 0)
		return error;

	error = scanout_paint(gpu, &display->surfaces[1], 1);
	if (error != 0)
		return error;

	/* Succeeded: the surfaces are ready to be shown. */
	nvrtx_stage_mark("takeover surfaces %ux%u pitch %u at %llx and %llx",
	    (unsigned)width,
	    (unsigned)height,
	    (unsigned)display->surfaces[0].pitch,
	    (unsigned long long)display->surfaces[0].offset,
	    (unsigned long long)display->surfaces[1].offset);
	return 0;
}

/*
 * Describes the firmware's console as both surfaces (the second is never
 * flipped to without the test): its VRAM at the start, its size, pitch and
 * format.  The console is shown only when it was kept (layout, vram-window), is
 * no larger than the mode's visible size and has a pitch the window takes (a
 * multiple of 64 bytes); otherwise ENOTSUP leaves the head to the firmware.  A
 * console smaller than the mode is enlarged by the head's scaler, as the
 * firmware itself shows it (a GOP mode of 640x480 on a monitor driven at its
 * own mode).
 */
static int
scanout_console_surface(
	struct nvrtx_gpu *gpu,
	uint32_t width,
	uint32_t height)
{
	const struct nvrtx_console *console;
	struct nvrtx_disp_surface *surface;
	unsigned index;

	console = &gpu->console;

	/* Leaves the head alone without a console the window reaches. */
	if (console->mapped == 0) {
		nvrtx_stage_mark("takeover: no console kept; the head is left to the firmware");
		return ENOTSUP;
	}

	/* Leaves the head alone without a console size the window can show. */
	if (console->width == 0U || console->height == 0U) {
		nvrtx_stage_mark("takeover: the console has no size; the head is left to the firmware");
		return ENOTSUP;
	}

	/* Leaves the head alone when the console is larger than the mode: the head's scaler only enlarges. */
	if (console->width > width || console->height > height) {
		nvrtx_stage_mark("takeover: the console is %ux%u, larger than the mode %ux%u; the head is left to the firmware",
		    (unsigned)console->width,
		    (unsigned)console->height,
		    (unsigned)width,
		    (unsigned)height);
		return ENOTSUP;
	}

	/* Leaves the head alone when the window cannot take the console's rows. */
	if ((console->pitch & (NVRTX_DISP_PITCH_UNIT - 1U)) != 0U) {
		nvrtx_stage_mark("takeover: the console's pitch %u is not a multiple of %u; the head is left to the firmware",
		    (unsigned)console->pitch,
		    (unsigned)NVRTX_DISP_PITCH_UNIT);
		return ENOTSUP;
	}

	/* Describes the console, at its own size, as both surfaces. */
	for (index = 0U; index < 2U; index++) {
		surface = &gpu->display.surfaces[index];
		surface->offset = 0U;
		surface->width = console->width;
		surface->height = console->height;
		surface->pitch = console->pitch;
		surface->handle = NVRTX_DISP_HANDLE_FB;
		surface->format = NVRTX_DISP_FORMAT_A8R8G8B8;
	}

	/* Succeeded: the console is the surface to show, enlarged to the mode when it is smaller. */
	nvrtx_stage_mark("takeover console %ux%u pitch %u at the VRAM's start, shown at %ux%u",
	    (unsigned)console->width,
	    (unsigned)console->height,
	    (unsigned)console->pitch,
	    (unsigned)width,
	    (unsigned)height);
	return 0;
}

/* Takes the two surfaces' VRAM from the allocator and maps them in the CPU's window when it works. */
static int
scanout_surface_objects(
	struct nvrtx_gpu *gpu,
	uint64_t span)
{
	struct nvrtx_bo *object;
	unsigned index;
	int error;

	/* Each surface is its own object on a 1 MiB boundary. */
	for (index = 0U; index < 2U; index++) {
		object = &gpu->display.surface_objects[index];
		error = nvrtx_bo_alloc(gpu, span, SCANOUT_SURFACE_ALIGN, object);
		if (error != 0)
			return error;

		/*
		 * The CPU's view is for later frames; takeover paints through
		 * PRAMIN, so a missing window is no failure.
		 */
		if (gpu->memory_state.bar1_ready != 0)
			(void)nvrtx_bo_map(gpu, object);
	}

	/* Succeeded: both surfaces have VRAM of their own. */
	return 0;
}

/* Paints eight bands of colour across a surface, in order or reversed. */
static int
scanout_paint(
	struct nvrtx_gpu *gpu,
	const struct nvrtx_disp_surface *surface,
	int reversed)
{
	uint32_t band;
	uint32_t colour;
	uint32_t first_row;
	uint32_t end_row;
	int error;

	/* Fills each band's rows (padding included) with its colour. */
	for (band = 0U; band < SCANOUT_BAND_COUNT; band++) {
		/* The reversed picture starts from the last colour. */
		colour = nvrtx_disp_band_color(band);
		if (reversed != 0)
			colour = nvrtx_disp_band_color(SCANOUT_BAND_COUNT - 1U - band);

		first_row = surface->height * band / SCANOUT_BAND_COUNT;
		end_row = surface->height * (band + 1U) / SCANOUT_BAND_COUNT;
		error = nvrtx_vram_fill(gpu,
		    surface->offset + (uint64_t)first_row * surface->pitch,
		    colour,
		    (size_t)(end_row - first_row) * surface->pitch);
		if (error != 0)
			return error;
	}

	/* Succeeded: the surface holds the picture. */
	return 0;
}

/*
 * Makes one display channel of the display's client: registers its push
 * buffer (one page of coherent system memory) with the resource manager,
 * makes it under the display root with its put at the start, and for a
 * core or window channel binds the notifiers' object and the VRAM's.
 * push, when given, is set up over the buffer.
 */
static int
scanout_channel(
	struct nvrtx_gpu *gpu,
	uint32_t class_number,
	uint32_t instance,
	struct nvrtx_buffer *buffer,
	struct nvrtx_push *push)
{
	uint8_t registration[NVRTX_DISP_PUSHBUFFER_BYTES];
	uint8_t params[NVRTX_DISP_CHANNEL_BYTES];
	const uint8_t *reply;
	size_t reply_length;
	uint32_t chid;
	uint32_t handle;
	int error;

	/* One page of coherent system memory for the push buffer. */
	error = nvrtx_buffer_alloc(gpu, SCANOUT_PUSH_BYTES, buffer);
	if (error != 0)
		return error;

	if (push != NULL)
		nvrtx_push_init(push, (uint8_t *)buffer->dma.address, SCANOUT_PUSH_WORDS);

	/* Registers the push buffer with the resource manager, for this class and instance. */
	nvrtx_disp_pushbuffer_params(registration, buffer->dma.device_address, SCANOUT_PUSH_BYTES, class_number, instance);
	error = nvrtx_rm_control(gpu,
	    gpu->internal_client,
	    gpu->internal_subdevice,
	    SCANOUT_CTL_PUSHBUFFER,
	    registration,
	    sizeof(registration),
	    &reply,
	    &reply_length);
	if (error != 0)
		return error;

	/* Makes the channel under the display root, its put at the start. */
	handle = (class_number << 16) | instance;
	nvrtx_disp_channel_params(params, instance);
	error = nvrtx_rm_allocate_in(gpu, NVRTX_DISP_CLIENT, SCANOUT_HANDLE_ROOT, handle, class_number, params, sizeof(params));
	if (error != 0)
		return error;

	/* A window-immediate channel has no objects. */
	if (class_number == gpu->identity.chip.profile->class_wimm)
		return 0;

	/* The core is channel 0 of the hash table; window w is channel 1 + w. */
	chid = NVRTX_DISP_CHID_CORE;
	if (class_number == gpu->identity.chip.profile->class_window)
		chid = NVRTX_DISP_CHID_WINDOW_FIRST + instance;

	/* Gives it the notifiers' object and the whole VRAM's. */
	error = scanout_bind(gpu, chid, NVRTX_DISP_HANDLE_SYNC, NVRTX_DISP_CONTEXT_DMA_FLAGS, gpu->display.sync, gpu->display.sync + SCANOUT_SYNC_BYTES - 1U);
	if (error != 0)
		return error;

	error = scanout_bind(gpu, chid, NVRTX_DISP_HANDLE_VRAM, NVRTX_DISP_CONTEXT_DMA_FLAGS, 0U, gpu->layout.vram_size - 1U);
	if (error != 0)
		return error;

	/* Succeeded: the channel takes methods. */
	return 0;
}

/*
 * Makes every display channel in order: the core (and sends its start,
 * without a release), then for each head its first window with its
 * window-immediate channel, its second window with its own, and its cursor.
 * Each SOR output's capability word is read once, as an encoder is made.
 *
 * Procedure referenced: Linux v6.19 drivers/gpu/drm/nouveau/dispnv50/disp.c
 * (nv50_display_create, nv50_sor_create) and head.c (nv50_head_create).
 */
static int
scanout_channels(
	struct nvrtx_gpu *gpu)
{
	struct nvrtx_display_state *display;
	struct nvrtx_buffer *buffer;
	uint32_t head;
	uint32_t window;
	uint32_t index;
	int error;

	display = &gpu->display;

	/* The core channel. */
	error = scanout_channel(gpu, gpu->identity.chip.profile->class_core, 0U, &display->core_buffer, &display->core_push);
	if (error != 0)
		return error;

	display->core_made = 1;

	/* The core's start: the channel's get is read first, then the start is sent without a release. */
	(void)nvrtx_rd32(gpu, SCANOUT_REG_CORE_USER + SCANOUT_USER_GET_AT);
	nvrtx_disp_first_core_start(&display->core_push);
	error = scanout_kick(gpu, &display->core_push, SCANOUT_REG_CORE_USER);
	if (error != 0)
		return error;

	/* Each SOR output's capability. */
	for (index = 0U; index < 32U; index++) {
		if ((display->output_mask & (1U << index)) == 0U)
			continue;
		if (display->output_type[index] != NVRTX_DISP_OUTPUT_SOR)
			continue;

		(void)nvrtx_rd32(gpu, SCANOUT_REG_SOR_CAPS);
	}

	/* Each head's two windows, each with its window-immediate channel, then its cursor. */
	for (head = 0U; head < NVRTX_DISP_HEAD_MAX; head++) {
		if ((display->head_mask & (1U << head)) == 0U)
			continue;

		for (window = head * 2U; window < head * 2U + 2U; window++) {
			if ((display->window_mask & (1U << window)) == 0U)
				continue;

			buffer = &display->window_buffers[window];
			error = scanout_channel(gpu, gpu->identity.chip.profile->class_window, window, buffer, NULL);
			if (error != 0)
				return error;

			display->windows_made |= 1U << window;

			buffer = &display->wimm_buffers[window];
			error = scanout_channel(gpu, gpu->identity.chip.profile->class_wimm, window, buffer, NULL);
			if (error != 0)
				return error;

			display->wimms_made |= 1U << window;
		}

		error = scanout_cursor_channel(gpu, head);
		if (error != 0)
			return error;

		display->cursors_made |= 1U << head;
	}

	/* Succeeded: every channel exists. */
	nvrtx_stage_mark("takeover channels: core, windows %x, immediate %x, cursors %x",
	    (unsigned)display->windows_made,
	    (unsigned)display->wimms_made,
	    (unsigned)display->cursors_made);
	return 0;
}

/*
 * Makes a head's cursor channel: the resource manager is told the PIO
 * channel has no push buffer, then makes it under the display root.
 */
static int
scanout_cursor_channel(
	struct nvrtx_gpu *gpu,
	uint32_t head)
{
	uint8_t registration[NVRTX_DISP_PUSHBUFFER_BYTES];
	uint8_t params[NVRTX_DISP_PIO_CHANNEL_BYTES];
	const uint8_t *reply;
	size_t reply_length;
	uint32_t handle;
	int error;

	/* Registers the cursor channel without a push buffer. */
	nvrtx_disp_pio_pushbuffer_params(registration, gpu->identity.chip.profile->class_cursor, head);
	error = nvrtx_rm_control(gpu,
	    gpu->internal_client,
	    gpu->internal_subdevice,
	    SCANOUT_CTL_PUSHBUFFER,
	    registration,
	    sizeof(registration),
	    &reply,
	    &reply_length);
	if (error != 0)
		return error;

	/* Makes the channel under the display root, the head as its instance. */
	handle = (gpu->identity.chip.profile->class_cursor << 16) | head;
	nvrtx_disp_pio_channel_params(params, head);
	error = nvrtx_rm_allocate_in(gpu, NVRTX_DISP_CLIENT, SCANOUT_HANDLE_ROOT, handle, gpu->identity.chip.profile->class_cursor, params, sizeof(params));
	if (error != 0)
		return error;

	/* Succeeded: the head's cursor channel exists. */
	return 0;
}

/*
 * Sets the first commit's mode: the EDID's preferred timing, or the armed
 * timing when there is none; the view and surfaces take its visible size.
 */
static int
scanout_mode(
	struct nvrtx_gpu *gpu)
{
	struct nvrtx_display_state *display;
	const char *source;

	display = &gpu->display;

	/* The timing the commit uses. */
	source = "EDID";
	if (display->timing_from_edid == 0) {
		nvrtx_disp_first_timing_from_armed(display->armed, &display->timing);
		source = "armed";
	}

	nvrtx_disp_first_mode(&display->timing, &display->mode);
	nvrtx_stage_mark("takeover head %u mode (%s): %ux%u, total %ux%u, clock %u kHz, output %08x",
	    (unsigned)display->gop_head,
	    source,
	    (unsigned)display->timing.hdisplay,
	    (unsigned)display->timing.vdisplay,
	    (unsigned)display->timing.htotal,
	    (unsigned)display->timing.vtotal,
	    (unsigned)display->timing.clock_khz,
	    (unsigned)display->mode.output_resource);

	/* A mode with no size is refused. */
	if (display->timing.hdisplay == 0U || display->timing.vdisplay == 0U)
		return EINVAL;

	/* Succeeded: the mode is set. */
	return 0;
}

/* Writes the identity output and input LUTs into the VRAM after the notifiers. */
static int
scanout_luts(
	struct nvrtx_gpu *gpu)
{
	struct nvrtx_display_state *display;
	uint8_t *lut;
	int error;

	display = &gpu->display;
	display->olut = display->instance + SCANOUT_OLUT_AFTER_INSTANCE;
	display->ilut = display->instance + SCANOUT_ILUT_AFTER_INSTANCE;

	/* One LUT's bytes in memory at a time. */
	lut = kern_malloc(NVRTX_DISP_LUT_BYTES);
	if (lut == NULL)
		return ENOMEM;

	nvrtx_disp_first_olut_fill(lut);
	error = nvrtx_vram_write(gpu, display->olut, lut, NVRTX_DISP_LUT_BYTES);
	if (error == 0) {
		nvrtx_disp_first_ilut_fill(lut);
		error = nvrtx_vram_write(gpu, display->ilut, lut, NVRTX_DISP_LUT_BYTES);
	}

	kern_free(lut);

	/* Reports a LUT that could not be written. */
	if (error != 0)
		return error;

	/* Succeeded: both LUTs are in the VRAM. */
	return 0;
}

/*
 * Commits the first mode on the lit head as a first commit does:
 * the window's surface object; the core's methods (the output taken off
 * and given back with its protocol, the HDMI controls and infoframes in
 * between for an HDMI sink, the head's mode, every window's owner) and
 * its release, waited for; the head's output LUT; the window's first
 * image, released interlocked with the core; the core's second release,
 * interlocked with the window, waited for; and the window's notifier.
 * A notifier that does not come is shown and the commit goes on; any such
 * is reported at the end as EIO.
 *
 * Procedure referenced: Linux v6.19 drivers/gpu/drm/nouveau/dispnv50/disp.c
 * (nv50_disp_atomic_commit_tail, nv50_disp_atomic_commit_core).
 */
static int
scanout_commit(
	struct nvrtx_gpu *gpu)
{
	struct nvrtx_display_state *display;
	struct nvrtx_disp_first first;
	uint32_t notifier;
	uint32_t window_user;
	int failed;
	int error;

	display = &gpu->display;
	display->window_ntfy = scanout_window_notifier(display->window);
	notifier = scanout_next_notifier(gpu);
	window_user = scanout_window_user(display->window);
	failed = 0;

	/* The window's object for the surface it shows (all the VRAM, large pages). */
	error = scanout_bind(gpu, NVRTX_DISP_CHID_WINDOW_FIRST + display->window, NVRTX_DISP_HANDLE_FB, NVRTX_DISP_CONTEXT_DMA_FB_FLAGS, 0U, gpu->layout.vram_size - 1U);
	if (error != 0)
		return error;

	/* Its object for a block-linear surface (the same VRAM, the block-linear kind). */
	error = scanout_bind(gpu, NVRTX_DISP_CHID_WINDOW_FIRST + display->window, NVRTX_DISP_HANDLE_FB_BLOCK_LINEAR, NVRTX_DISP_CONTEXT_DMA_BL_FLAGS, 0U, gpu->layout.vram_size - 1U);
	if (error != 0)
		return error;

	/* Its notifier, cleared before the commit. */
	error = scanout_notifier_reset(gpu, notifier);
	if (error != 0)
		return error;

	/* The output off; for an HDMI sink the HDMI controls and the infoframes; the output on with the head's mode and the owners. */
	first.head = display->gop_head;
	first.sor = display->gop_sor;
	first.display_id = display->gop_display_id;
	first.protocol = display->gop_protocol;
	if (first.protocol == SCANOUT_PROTOCOL_TMDS_DUAL) {
		/* A dual link drives only a clock of 165 MHz or more to a DVI sink; below that, link A alone. */
		if (display->timing.clock_khz < SCANOUT_DUAL_LINK_KHZ || display->sink.hdmi != 0)
			first.protocol = SCANOUT_PROTOCOL_TMDS_A;
	}
	first.mode = display->mode;
	first.view_in = (display->surfaces[0].height << 16) | (display->surfaces[0].width & 0xffffU);
	if (display->sink.hdmi != 0) {
		error = nvrtx_display_hdmi(gpu);
		if (error != 0)
			return error;

		scanout_infoframes(gpu);
	}

	nvrtx_disp_first_core_mode(&display->core_push, &first);
	error = scanout_release_core(gpu, 0U, "core mode");
	if (error == ETIMEDOUT)
		failed = 1;
	else if (error != 0)
		return error;

	scanout_step("2: core released with the mode");

	/* The head's output LUT. */
	nvrtx_disp_first_core_olut(&display->core_push, display->gop_head, display->olut);

	/* The window's first image, released with the core; its get is read first. */
	(void)nvrtx_rd32(gpu, window_user + SCANOUT_USER_GET_AT);
	nvrtx_disp_first_window(&display->window_push,
	    display->window,
	    &display->surfaces[0],
	    gpu->identity.chip.profile->window_storage_pitch,
	    notifier,
	    display->ilut);
	error = scanout_kick(gpu, &display->window_push, window_user);
	if (error != 0)
		return error;

	/* The core's second release, interlocked with the window. */
	error = scanout_release_core(gpu, 1U << display->window, "core with window");
	if (error == ETIMEDOUT)
		failed = 1;
	else if (error != 0)
		return error;

	/* The window's flip has begun. */
	error = scanout_notifier_wait(gpu, notifier, 1U, window_user, "window first");
	if (error == ETIMEDOUT) {
		scanout_diagnose(gpu);
		scanout_diagnose_window(gpu);
		failed = 1;
	} else if (error != 0) {
		return error;
	}

	/* Reports a commit a notifier did not confirm. */
	if (failed != 0) {
		nvrtx_stage_mark("takeover fail: the first commit was not confirmed");
		return EIO;
	}

	/* Succeeded: the head shows the first surface, its view the surface's size. */
	display->view_in = first.view_in;
	nvrtx_stage_mark("takeover ok: mode set (%ux%u on SOR %u), surface A %ux%u shown",
	    (unsigned)(display->mode.active & 0xffffU),
	    (unsigned)(display->mode.active >> 16),
	    (unsigned)display->gop_sor,
	    (unsigned)display->surfaces[0].width,
	    (unsigned)display->surfaces[0].height);
	return 0;
}

/*
 * Writes the lit head's HDMI infoframes as an HDMI enable does: the AVI
 * frame of the RGB picture, and the vendor frame turned off (a mode without
 * an HDMI code or 3D has none).
 *
 * Procedure referenced: Linux v6.19 drivers/gpu/drm/nouveau/dispnv50/disp.c
 * (nv50_hdmi_enable) and drivers/gpu/drm/nouveau/nvkm/engine/disp/gv100.c
 * (gv100_sor_hdmi_infoframe_avi, gv100_sor_hdmi_infoframe_vsi).
 */
static void
scanout_infoframes(
	struct nvrtx_gpu *gpu)
{
	uint32_t words[5];
	uint32_t base;
	uint32_t index;
	uint32_t value;

	base = gpu->display.gop_head * SCANOUT_INFOFRAME_STRIDE;
	nvrtx_disp_first_avi(words);

	/* The AVI frame off, its five words, then on. */
	value = nvrtx_rd32(gpu, SCANOUT_REG_AVI_CONTROL + base);
	nvrtx_wr32(gpu, SCANOUT_REG_AVI_CONTROL + base, value & ~SCANOUT_INFOFRAME_ENABLE);
	for (index = 0U; index < 5U; index++)
		nvrtx_wr32(gpu, SCANOUT_REG_AVI_HEADER + base + index * 4U, words[index]);
	value = nvrtx_rd32(gpu, SCANOUT_REG_AVI_CONTROL + base);
	nvrtx_wr32(gpu, SCANOUT_REG_AVI_CONTROL + base, value | SCANOUT_INFOFRAME_ENABLE);

	/* The vendor frame off. */
	value = nvrtx_rd32(gpu, SCANOUT_REG_VSI_CONTROL + base);
	nvrtx_wr32(gpu, SCANOUT_REG_VSI_CONTROL + base, value & ~SCANOUT_VSI_DISABLE_MASK);
}

/*
 * Sends a core release with the windows in the mask interlocked, after
 * clearing the core's notifier, and waits for it.  A notifier that does
 * not come shows the display's error state and returns ETIMEDOUT.
 */
static int
scanout_release_core(
	struct nvrtx_gpu *gpu,
	uint32_t window_interlock,
	const char *what)
{
	struct nvrtx_display_state *display;
	int error;

	display = &gpu->display;

	/* Clears the notifier, then sends the release. */
	error = scanout_notifier_reset(gpu, SCANOUT_CORE_NOTIFIER_AT);
	if (error != 0)
		return error;

	nvrtx_disp_first_core_release(&display->core_push, window_interlock);
	error = scanout_kick(gpu, &display->core_push, SCANOUT_REG_CORE_USER);
	if (error != 0)
		return error;

	/* Waits for the core to finish it. */
	error = scanout_notifier_wait(gpu, SCANOUT_CORE_NOTIFIER_AT, NVRTX_DISP_NOTIFIER_FINISHED, SCANOUT_REG_CORE_USER, what);
	if (error == ETIMEDOUT) {
		scanout_diagnose(gpu);
		return ETIMEDOUT;
	}

	if (error != 0)
		return error;

	/* Succeeded: the core has released the state. */
	return 0;
}

/*
 * Writes one context DMA object into the instance memory and enters it in
 * the hash table for one channel, probing past the slots already taken.
 */
static int
scanout_bind(
	struct nvrtx_gpu *gpu,
	uint32_t chid,
	uint32_t handle,
	uint32_t flags,
	uint64_t start,
	uint64_t limit)
{
	struct nvrtx_display_state *display;
	uint8_t object[NVRTX_DISP_CONTEXT_DMA_BYTES];
	uint8_t entry[NVRTX_DISP_RAMHT_ENTRY_BYTES];
	uint32_t slot;
	uint32_t used;
	uint32_t client;
	int taken;
	int error;

	display = &gpu->display;

	/* The objects follow the hash table. */
	if (display->object_next == 0U)
		display->object_next = NVRTX_DISP_RAMHT_BYTES;

	/* The slots taken are recorded; more than the record holds is refused. */
	if (display->ramht_count >= NVRTX_DISP_RAMHT_USED_MAX)
		return ENOSPC;

	/* Writes the object over the range. */
	nvrtx_disp_context_dma_flags(object, flags, start, limit);
	error = nvrtx_vram_write(gpu, display->instance + display->object_next, object, sizeof(object));
	if (error != 0)
		return error;

	/* Finds the first free slot from the handle's hash on. */
	slot = nvrtx_disp_ramht_hash(chid, handle);
	for (;;) {
		/* Looks the slot up among the taken ones. */
		taken = 0;
		for (used = 0U; used < display->ramht_count; used++) {
			if (display->ramht_slots[used] == slot)
				taken = 1;
		}

		/* A free slot ends the search; a taken one moves to the next. */
		if (taken == 0)
			break;

		slot = (slot + 1U) % NVRTX_DISP_RAMHT_ENTRIES;
	}

	/* Enters the handle with the channel, the low bits of the owning client, and the object's offset. */
	client = NVRTX_DISP_CLIENT;
	nvrtx_disp_ramht_entry(entry, handle, chid, client, display->object_next);
	error = nvrtx_vram_write(gpu, display->instance + (uint64_t)slot * NVRTX_DISP_RAMHT_ENTRY_BYTES, entry, sizeof(entry));
	if (error != 0)
		return error;

	/* Succeeded: the object and its slot are taken. */
	display->ramht_slots[display->ramht_count] = slot;
	display->ramht_count++;
	display->object_next += NVRTX_DISP_CONTEXT_DMA_STEP;
	return 0;
}

/* Hands the methods written so far to a channel: the put moves to the end of the buffer's words. */
static int
scanout_kick(
	struct nvrtx_gpu *gpu,
	struct nvrtx_push *push,
	uint32_t user)
{
	/* A buffer that overflowed would send a cut sequence. */
	if (push->overflow != 0) {
		nvrtx_stage_mark("takeover fail: a push buffer overflowed (%u words)", (unsigned)push->count);
		return E2BIG;
	}

	/* The words are in memory before the put moves; the put is a byte offset. */
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	nvrtx_wr32(gpu, user + SCANOUT_USER_PUT_AT, push->count * 4U);

	/* Succeeded: the channel fetches up to the put. */
	return 0;
}

/* Clears one notifier (status not begun) before the update that writes it. */
static int
scanout_notifier_reset(
	struct nvrtx_gpu *gpu,
	uint32_t offset)
{
	int error;

	/* Clears its four words. */
	error = nvrtx_vram_zero(gpu, gpu->display.sync + offset, SCANOUT_NOTIFIER_BYTES);
	if (error != 0)
		return error;

	/* Succeeded: the notifier reads not begun. */
	return 0;
}

/*
 * Waits until a notifier's status reaches least; on a timeout shows the
 * notifier and the channel's get and put.  what names the wait in the marks;
 * NULL (a frame of the GPU device) marks only a timeout.
 */
static int
scanout_notifier_wait(
	struct nvrtx_gpu *gpu,
	uint32_t offset,
	uint32_t least,
	uint32_t user,
	const char *what)
{
	uint64_t start;
	uint64_t waited;
	uint32_t word;
	uint32_t get;
	uint32_t put;
	int error;

	/* Polls until the status is reached or the time, measured, is spent. */
	start = nvrtx_now_us();
	word = 0U;
	for (;;) {
		error = nvrtx_vram_read32(gpu, gpu->display.sync + offset, &word);
		if (error != 0)
			return error;

		/* The status is the word's top two bits. */
		waited = nvrtx_now_us() - start;
		if ((word >> NVRTX_DISP_NOTIFIER_STATUS_SHIFT) >= least) {
			if (what != NULL)
				nvrtx_stage_mark("takeover %s notified after %u us", what, (unsigned)waited);
			return 0;
		}

		if (waited > SCANOUT_NOTIFIER_TIMEOUT_US)
			break;

		kern_usleep_range(SCANOUT_NOTIFIER_STEP_US, SCANOUT_NOTIFIER_STEP_US * 2U);
	}

	/* The notifier never came: shows where the channel stopped. */
	get = nvrtx_rd32(gpu, user + SCANOUT_USER_GET_AT);
	put = nvrtx_rd32(gpu, user + SCANOUT_USER_PUT_AT);
	if (what == NULL)
		what = "frame";
	nvrtx_stage_mark("takeover fail: %s notifier %08x after 2 s, get %x put %x",
	    what,
	    (unsigned)word,
	    (unsigned)get,
	    (unsigned)put);
	return ETIMEDOUT;
}

/* Marks one step of the takeover; with the monitor test it waits, so the monitor can be watched at it. */
static void
scanout_step(
	const char *what)
{
	bool test;

	/* Only the mark without the test. */
	test = nvrtx_stage_disp_test_wanted();
	if (!test) {
		nvrtx_stage_mark("takeover step %s", what);
		return;
	}

	/* The mark, then the wait. */
	nvrtx_stage_mark("takeover step %s (5 s)", what);
	scanout_hold(SCANOUT_STEP_US);
}

/* Shows the display's error state after a notifier did not come. */
static void
scanout_diagnose(
	struct nvrtx_gpu *gpu)
{
	/* The core channel's exception: its status (method in bits 11:0, reason in 14:12), data and code. */
	nvrtx_stage_mark("takeover core exception %08x data %08x code %08x",
	    (unsigned)nvrtx_rd32(gpu, SCANOUT_REG_CORE_EXCEPTION),
	    (unsigned)nvrtx_rd32(gpu, SCANOUT_REG_CORE_EXCEPTION_DATA),
	    (unsigned)nvrtx_rd32(gpu, SCANOUT_REG_CORE_EXCEPTION_CODE));

	/* The other exceptions, the supervisor's pending steps and the core's state. */
	nvrtx_stage_mark("takeover other %08x supervisor %08x core state %08x",
	    (unsigned)nvrtx_rd32(gpu, SCANOUT_REG_EXCEPTION_OTHER),
	    (unsigned)nvrtx_rd32(gpu, SCANOUT_REG_SUPERVISOR),
	    (unsigned)nvrtx_rd32(gpu, SCANOUT_REG_CORE_STATE));

	/* The display's error detail, the supervisor's mode switch, and window 0's exception. */
	nvrtx_stage_mark("takeover error %08x supervisor main %08x window exception %08x",
	    (unsigned)nvrtx_rd32(gpu, SCANOUT_REG_ERROR_DISP),
	    (unsigned)nvrtx_rd32(gpu, SCANOUT_REG_SUPERVISOR_MAIN),
	    (unsigned)nvrtx_rd32(gpu, SCANOUT_REG_WINDOW_EXCEPTION));
}

/*
 * Flips the window to the second surface (a commit of a plane alone) and
 * waits for its notifier.
 *
 * Procedure referenced: Linux v6.19 drivers/gpu/drm/nouveau/dispnv50/disp.c
 * (nv50_disp_atomic_commit_tail).
 */
static int
scanout_flip(
	struct nvrtx_gpu *gpu)
{
	struct nvrtx_display_state *display;
	uint32_t notifier;
	uint32_t user;
	int error;

	display = &gpu->display;
	user = scanout_window_user(display->window);

	/* Clears the window's next notifier, then flips with it, on the window alone. */
	notifier = scanout_next_notifier(gpu);
	error = scanout_notifier_reset(gpu, notifier);
	if (error != 0)
		return error;

	nvrtx_disp_first_window_flip(&display->window_push,
	    display->window,
	    &display->surfaces[1],
	    gpu->identity.chip.profile->window_storage_pitch,
	    notifier,
	    0);
	error = scanout_kick(gpu, &display->window_push, user);
	if (error != 0)
		return error;

	/* Waits until the flip has begun (the window's notifier leaves "not begun"). */
	error = scanout_notifier_wait(gpu, notifier, 1U, user, "window flip");
	if (error == ETIMEDOUT)
		scanout_diagnose_window(gpu);
	if (error != 0)
		return error;

	/* Succeeded: the second surface is on the head. */
	nvrtx_stage_mark("takeover ok: flip, surface B shown");
	return 0;
}

/* Keeps the current picture up for a while, so it can be seen. */
static void
scanout_hold(
	uint32_t microseconds)
{
	uint64_t start;
	uint64_t now;

	/* Waits, measured, in short steps. */
	start = nvrtx_now_us();
	for (;;) {
		now = nvrtx_now_us();
		if (now - start >= microseconds)
			break;

		kern_usleep_range(SCANOUT_SHOW_STEP_US, SCANOUT_SHOW_STEP_US);
	}
}

/*
 * Waits until a channel's get reaches a byte offset of its push buffer (its
 * put): the channel has fetched every word it was given.
 */
static int
scanout_drained(
	struct nvrtx_gpu *gpu,
	uint32_t user,
	uint32_t put)
{
	uint64_t start;
	uint64_t waited;
	uint32_t get;

	/* Polls the get until it stands at the put or the time is spent. */
	start = nvrtx_now_us();
	for (;;) {
		get = nvrtx_rd32(gpu, user + SCANOUT_USER_GET_AT);
		if (get == put)
			break;

		/* The channel did not fetch its words in time: shows where it stopped. */
		waited = nvrtx_now_us() - start;
		if (waited > SCANOUT_DRAIN_US) {
			nvrtx_stage_mark("gpu-device fail: channel at %x stopped, get %x put %x", (unsigned)user, (unsigned)get, (unsigned)put);
			return ETIMEDOUT;
		}

		kern_usleep_range(SCANOUT_DRAIN_STEP_US, SCANOUT_DRAIN_STEP_US * 2U);
	}

	/* Succeeded: the channel has every word. */
	return 0;
}

/*
 * Makes room for words more of the core's methods: once the core has
 * fetched everything it was given, the buffer is wound back to its start
 * when the words would not fit before its end (a jump to the start where
 * the get stands, the put moved to the start, and the channel left to take
 * the jump), so nothing unfetched is written over.
 *
 * Procedure referenced: Linux v6.19 drivers/gpu/drm/nouveau/dispnv50/disp.c
 * (nv50_dmac_wind).
 */
static int
scanout_core_room(
	struct nvrtx_gpu *gpu,
	uint32_t words)
{
	struct nvrtx_push *push;
	int error;

	push = &gpu->display.core_push;

	/* The core has fetched up to its put. */
	error = scanout_drained(gpu, SCANOUT_REG_CORE_USER, push->count * 4U);
	if (error != 0)
		return error;

	/* The words fit before the end, with a word left for a later jump. */
	if (push->count + words + 1U <= push->capacity)
		return 0;

	/* Jumps back to the start and moves the put there. */
	nvrtx_push_jump(push, 0U);
	if (push->overflow != 0)
		return E2BIG;

	push->count = 0U;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	nvrtx_wr32(gpu, SCANOUT_REG_CORE_USER + SCANOUT_USER_PUT_AT, 0U);

	/* Waits until the core has taken the jump. */
	error = scanout_drained(gpu, SCANOUT_REG_CORE_USER, 0U);
	if (error != 0)
		return error;

	/* Succeeded: the buffer is empty from its start. */
	return 0;
}

/*
 * Sends the core's update for a cursor change (no interlock), with the
 * core's notifier asked for and not waited for: core_pending is set until
 * nvrtx_scanout_core_settled() sees it finished.
 */
static int
scanout_core_send_cursor(
	struct nvrtx_gpu *gpu)
{
	struct nvrtx_display_state *display;
	int error;

	display = &gpu->display;

	/* Clears the notifier, then sends the update. */
	error = scanout_notifier_reset(gpu, SCANOUT_CORE_NOTIFIER_AT);
	if (error != 0)
		return error;

	nvrtx_disp_core_update_cursor(&display->core_push, SCANOUT_CORE_NOTIFIER_AT);
	error = scanout_kick(gpu, &display->core_push, SCANOUT_REG_CORE_USER);
	if (error != 0)
		return error;

	/* Succeeded: the update is out; its notifier will say when it is done. */
	display->core_pending = 1;
	return 0;
}

/*
 * Writes one register of the head's cursor channel once the channel has
 * room for it (four free entries within 100 ms).
 *
 * Procedure referenced: Linux v6.19 drivers/gpu/drm/nouveau/dispnv50/curs507a.c
 * (curs507a_space).
 */
static int
scanout_cursor_write(
	struct nvrtx_gpu *gpu,
	uint32_t method,
	uint32_t value)
{
	uint64_t start;
	uint64_t waited;
	uint32_t user;
	uint32_t free_entries;

	/* The head's own page of the cursor channels. */
	user = SCANOUT_REG_CURSOR_USER + gpu->display.gop_head * SCANOUT_CURSOR_USER_STRIDE;

	/* Polls the free entries until there are enough or the time is spent. */
	start = nvrtx_now_us();
	for (;;) {
		free_entries = nvrtx_rd32(gpu, user + NVRTX_CURSOR_FREE) & NVRTX_CURSOR_FREE_COUNT_MASK;
		if (free_entries >= SCANOUT_CURSOR_SPACE_LEAST)
			break;

		/* The channel did not make room in time. */
		waited = nvrtx_now_us() - start;
		if (waited > SCANOUT_CURSOR_SPACE_US) {
			nvrtx_stage_mark("gpu-device fail: cursor channel has %u free entries", (unsigned)free_entries);
			return ETIMEDOUT;
		}

		kern_usleep_range(SCANOUT_DRAIN_STEP_US, SCANOUT_DRAIN_STEP_US * 2U);
	}

	/* Writes the register. */
	nvrtx_wr32(gpu, user + method, value);

	/* Succeeded: the channel took the word. */
	return 0;
}

/* Reports where one window channel's put and get are. */
static uint32_t
scanout_window_user(
	uint32_t window)
{
	/* Each window has its own page after the first. */
	return SCANOUT_REG_WINDOW_USER + window * SCANOUT_WINDOW_USER_STRIDE;
}

/* Reports where one window's notifier is in the notifiers' VRAM. */
static uint32_t
scanout_window_notifier(
	uint32_t window)
{
	/* After the core's slot, one slot for each window, the notifier in its second half. */
	return (1U + window) * SCANOUT_WINDOW_SLOT_BYTES + SCANOUT_WINDOW_NOTIFIER_AT;
}

/*
 * Takes the window's notifier for the next flip and moves to the other one
 * (the two 16-byte halves of the window's slot are used in turn).
 *
 * Procedure referenced: Linux v6.19 drivers/gpu/drm/nouveau/dispnv50/wndw.c
 * (nv50_wndw_ntfy_enable).
 */
static uint32_t
scanout_next_notifier(
	struct nvrtx_gpu *gpu)
{
	uint32_t notifier;

	/* This flip's notifier, then the other for the next. */
	notifier = gpu->display.window_ntfy;
	gpu->display.window_ntfy ^= SCANOUT_NOTIFIER_BYTES;

	/* Succeeded: the notifier's offset in the sync object. */
	return notifier;
}

/* Shows the window channel's state after its notifier did not come: its exception, its status and its window-immediate channel's. */
static void
scanout_diagnose_window(
	struct nvrtx_gpu *gpu)
{
	uint32_t window;

	window = gpu->display.window;

	/* The window's exception (channel 1 + window), its status, its immediate channel's status, and its get and put. */
	nvrtx_stage_mark("takeover window %u exception %08x data %08x code %08x status %08x immediate %08x",
	    (unsigned)window,
	    (unsigned)nvrtx_rd32(gpu, SCANOUT_REG_CORE_EXCEPTION + (1U + window) * SCANOUT_EXCEPTION_STRIDE),
	    (unsigned)nvrtx_rd32(gpu, SCANOUT_REG_CORE_EXCEPTION_DATA + (1U + window) * SCANOUT_EXCEPTION_STRIDE),
	    (unsigned)nvrtx_rd32(gpu, SCANOUT_REG_CORE_EXCEPTION_CODE + (1U + window) * SCANOUT_EXCEPTION_STRIDE),
	    (unsigned)nvrtx_rd32(gpu, SCANOUT_REG_WINDOW_STATUS + window * 4U),
	    (unsigned)nvrtx_rd32(gpu, SCANOUT_REG_WIMM_STATUS + window * 4U));
	nvrtx_stage_mark("takeover other %08x supervisor %08x core state %08x error %08x",
	    (unsigned)nvrtx_rd32(gpu, SCANOUT_REG_EXCEPTION_OTHER),
	    (unsigned)nvrtx_rd32(gpu, SCANOUT_REG_SUPERVISOR),
	    (unsigned)nvrtx_rd32(gpu, SCANOUT_REG_CORE_STATE),
	    (unsigned)nvrtx_rd32(gpu, SCANOUT_REG_ERROR_DISP));
}
