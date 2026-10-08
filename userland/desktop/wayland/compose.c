/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Window mode: draws the background and every window, bottom to top, into
 * a VK_KHR_display swapchain (WS035 compositing design, D1).
 *
 * A frame is one command buffer and one render pass that clears to the
 * background and draws each window as a textured quad placed by push
 * constants.  Only one frame is in flight: its fence is exported as an fd
 * that the event loop polls, and the buffers the frame sampled and the frame
 * callbacks of the surfaces it showed are held until the fence signals.
 */

#include "desktop.h"
#include "compose.h"
#include "shaders.h"
#include "popup.h"
#include "subsurface.h"
#include "extras.h"
#include "ime.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Room for the common Vulkan extensions and those requested by the OS. */
#define COMPOSE_EXTENSIONS_MAX	16U

static VkResult compose_device(struct kwl_compose *compose);
static VkResult compose_display(struct kwl_server *server);
static void compose_limits(struct kwl_server *server);
static VkResult compose_refresh(struct kwl_server *server, VkDisplayKHR display, uint32_t width, uint32_t height, uint32_t *refresh);
static VkResult compose_objects(struct kwl_compose *compose);
static VkResult compose_pass(struct kwl_compose *compose);
static VkResult compose_pipeline(struct kwl_compose *compose, enum kwl_draw draw, VkShaderModule vertex, VkShaderModule fragment, VkPipelineLayout layout, VkPipeline *pipeline);
static VkResult compose_panel_pipeline(struct kwl_compose *compose);
static VkResult compose_corners(struct kwl_compose *compose);
static VkResult compose_targets(struct kwl_compose *compose);
static void compose_targets_destroy(struct kwl_compose *compose);
static unsigned compose_windows(struct kwl_server *server, struct kwl_object **windows, unsigned capacity);
static unsigned compose_split(struct kwl_server *server, struct kwl_object **windows, unsigned count);
static void compose_lists_log(struct kwl_server *server, struct kwl_object **windows, unsigned count);
static void compose_quad(struct kwl_server *server, VkCommandBuffer command, const struct kwl_import *import, int32_t x, int32_t y);
static void compose_quad_part(struct kwl_server *server, VkCommandBuffer command, const struct kwl_import *import, int32_t x, int32_t y, uint32_t quad_width, uint32_t quad_height, const float *uv);
static const struct kwl_import *surface_image(const struct kwl_object *surface);
static void compose_cursor(struct kwl_server *server, VkCommandBuffer command);
static VkResult compose_record(struct kwl_server *server, uint32_t image, struct kwl_object **windows, unsigned count, const VkRect2D *region);
static int compose_region(struct kwl_server *server, uint32_t image, VkRect2D *region);
static VkResult compose_submit(struct kwl_server *server, uint32_t image);
static void compose_hold(struct kwl_server *server, struct kwl_object **windows, unsigned count);

/*
 * Creates the Vulkan device, the pipelines' fixed objects and the frame's
 * synchronization; the output is opened separately.
 */
int
kwl_compose_open(
	struct kwl_server *server)
{
	struct kwl_compose *compose;
	uint64_t started;
	VkResult result;
	int error;

	/* The state lives as long as the compositor. */
	compose = calloc(1, sizeof(*compose));
	if (compose == NULL)
		return ENOMEM;
	server->compose = compose;

	/* The instance, the device and its graphics queue. */
	started = kwl_milliseconds();
	result = compose_device(compose);
	if (result != VK_SUCCESS) {
		printf("KWL VULKAN_ERROR operation=device result=%d\n", (int)result);
		return EIO;
	}

	/* The display's size (unless --width and --height chose one) and refresh, before anything is drawn at that size. */
	result = compose_display(server);
	if (result != VK_SUCCESS) {
		printf("KWL VULKAN_ERROR operation=display result=%d\n", (int)result);
		return EIO;
	}

	/* What the device can take, against which each client buffer's description is checked. */
	compose_limits(server);

	/* How long the device took (KWL STARTUP, ws035-p129). */
	printf("KWL STARTUP step=vulkan-device ms=%llu\n", (unsigned long long)(kwl_milliseconds() - started));
	started = kwl_milliseconds();

	/* The layouts, sampler, pools and synchronization of a frame. */
	result = compose_objects(compose);
	if (result != VK_SUCCESS) {
		printf("KWL VULKAN_ERROR operation=objects result=%d\n", (int)result);
		return EIO;
	}

	/* The start's images (the arrow, the wallpaper, the glyphs) are moved to their layout together (ws035-p131). */
	kwl_host_image_batch_begin(compose);

	/* the compositor's arrow cursor. */
	error = kwl_arrow_create(server);
	if (error != 0) {
		printf("KWL VULKAN_ERROR operation=arrow\n");
		return EIO;
	}

	/* The glass look's wallpaper and glyphs; without them the plain look is drawn. */
	printf("KWL STARTUP step=vulkan-objects-arrow ms=%llu\n", (unsigned long long)(kwl_milliseconds() - started));
	if (server->glass) {
		error = kwl_glass_open(server);
		if (error != 0) {
			printf("KWL GLASS unavailable errno=%d\n", error);
			server->glass = 0;
		}
	}

	/* The start's images' layout moves, submitted and waited for once. */
	result = kwl_host_image_batch_end(compose);
	if (result != VK_SUCCESS) {
		printf("KWL VULKAN_ERROR operation=setup-layout result=%d\n", (int)result);
		return EIO;
	}

	/* Succeeded: window mode can open its output. */
	return 0;
}

/*
 * Reads a display's native size, its refresh at that size and its name
 * (empty when it has none), for an output moved to it while the compositor
 * runs (ws113-p004a).  Returns VK_SUCCESS, or VK_ERROR_SURFACE_LOST_KHR
 * when the display is not connected now.
 */
VkResult
kwl_compose_display_read(
	struct kwl_server *server,
	VkDisplayKHR display,
	uint32_t *width,
	uint32_t *height,
	uint32_t *refresh,
	char *name,
	size_t size)
{
	struct kwl_compose *compose;
	VkDisplayPropertiesKHR properties[KWL_COMPOSE_DISPLAYS];
	VkResult result;
	uint32_t count;
	uint32_t index;

	/* The displays connected now. */
	compose = server->compose;
	count = KWL_COMPOSE_DISPLAYS;
	result = vkGetPhysicalDeviceDisplayPropertiesKHR(compose->physical, &count, properties);
	if (result != VK_SUCCESS && result != VK_INCOMPLETE)
		return result;

	/* The display among them. */
	for (index = 0U; index < count; index++) {
		/* Handles compare only for equality. */
		if (properties[index].display == display)
			break;
	}

	/* Not connected now. */
	if (index == count)
		return VK_ERROR_SURFACE_LOST_KHR;

	/* Its size and name. */
	*width = properties[index].physicalResolution.width;
	*height = properties[index].physicalResolution.height;
	if (size > 0U) {
		name[0] = '\0';
		if (properties[index].displayName != NULL)
			(void)snprintf(name, size, "%s", properties[index].displayName);
	}

	/* The refresh of its mode at that size. */
	result = compose_refresh(server, display, *width, *height, refresh);
	if (result != VK_SUCCESS)
		return result;

	/* Succeeded: the display's size, refresh and name are known. */
	return VK_SUCCESS;
}

/*
 * Tells whether the lid matters to the output shown: it does unless that
 * output is an external display (an HDMI or DisplayPort connector's key,
 * D-ID A2 "zedbsd-port-v1:...:hdmi:B"), which goes on with the lid closed
 * (ws052-p012, the 2026-10-07 user decision N8; R5: a machine started with
 * its lid closed on an external display).  The built-in panel, a virtual
 * display and a display without a name are the machine's own.
 */
int
kwl_output_lid_matters(
	struct kwl_server *server)
{
	const char *name;
	const char *found;

	/* Without the output nothing is shown on an external display. */
	if (server->compose == NULL)
		return 1;
	name = server->compose->display_name;

	/* An HDMI connector's display goes on with the lid closed. */
	found = strstr(name, ":hdmi:");
	if (found != NULL)
		return 0;

	/* So does a DisplayPort connector's. */
	found = strstr(name, ":dp:");
	if (found != NULL)
		return 0;

	/* Succeeded: the output is the machine's own. */
	return 1;
}

/*
 * Creates window mode's display surface, and the render pass and pipelines
 * for its format the first time, without taking the display (ws035-p130).
 *
 * A surface and its format need no lease (only a swapchain claims the
 * display), so this is done before READY, while another compositor (the
 * greeter, or the session before a Log Out) still shows its picture, and
 * the hand-over waits only for the swapchain.
 */
int
kwl_compose_output_prepare(
	struct kwl_server *server)
{
	struct kwl_compose *compose;
	uint64_t started;
	VkResult result;

	/* A prepared or open output needs nothing. */
	compose = server->compose;
	if (compose == NULL || compose->output_prepared)
		return 0;

	/* The display plane's surface at the compositor's size. */
	started = kwl_milliseconds();
	result = vkdemo_display_open_on(compose->instance, compose->physical, compose->display, server->width, server->height, &compose->output);
	if (result != VK_SUCCESS) {
		printf("KWL VULKAN_ERROR operation=display result=%d\n", (int)result);
		vkdemo_display_close(compose->instance, compose->device, &compose->output);
		return EIO;
	}

	/* The swapchain's format, which the pass and pipelines follow. */
	printf("KWL STARTUP step=output-display ms=%llu\n", (unsigned long long)(kwl_milliseconds() - started));
	started = kwl_milliseconds();
	result = vkdemo_display_choose_format(compose->physical, &compose->output);
	if (result != VK_SUCCESS) {
		printf("KWL VULKAN_ERROR operation=format result=%d\n", (int)result);
		vkdemo_display_close(compose->instance, compose->device, &compose->output);
		return EIO;
	}

	/* The pass and pipelines, made once: the output's format does not change. */
	if (compose->pass == VK_NULL_HANDLE) {
		compose->format = compose->output.format;
		result = compose_pass(compose);
		if (result != VK_SUCCESS) {
			printf("KWL VULKAN_ERROR operation=pipelines result=%d\n", (int)result);
			vkdemo_display_close(compose->instance, compose->device, &compose->output);
			return EIO;
		}
	}

	/* Succeeded: only the swapchain is left, which claims the display. */
	compose->output_prepared = 1;
	printf("KWL STARTUP step=output-pipelines ms=%llu\n", (unsigned long long)(kwl_milliseconds() - started));
	return 0;
}

/*
 * Creates the swapchain of window mode's display surface (preparing the
 * surface first when that was not done), which claims the display.
 */
int
kwl_compose_output_open(
	struct kwl_server *server)
{
	struct kwl_compose *compose;
	uint64_t started;
	VkResult result;
	int readback;
	int error;

	/* An open output needs nothing. */
	compose = server->compose;
	if (compose->output_open)
		return 0;

	/* The surface, and the pass and pipelines for its format. */
	error = kwl_compose_output_prepare(server);
	if (error != 0)
		return error;

	/* Gives Vulkan the display permission before creating its swapchain. */
	result = kwl_os_display_acquire(server, compose->physical, compose->display);
	if (result != VK_SUCCESS) {
		printf("KWL VULKAN_ERROR operation=display-acquire result=%d\n", (int)result);
		vkdemo_display_close(compose->instance, compose->device, &compose->output);
		compose->output_prepared = 0;
		return EIO;
	}

	/*
	 * Its FIFO swapchain, in the format the pipelines were made for; its
	 * images are a copy's source when the display allows it, for the
	 * mirror's heads (ws113-p004b) and a test image's capture.
	 */
	started = kwl_milliseconds();
	readback = 1;
	result = vkdemo_display_create_swapchain(compose->physical, compose->device, compose->family, &compose->output, readback);
	if (result == VK_ERROR_FORMAT_NOT_SUPPORTED && readback) {
		/* A display whose images cannot be read back still shows the desktop, without the capture (ws173-p002). */
		printf("KWL SHOT unsupported: the swapchain images cannot be a copy's source\n");
		readback = 0;
		result = vkdemo_display_create_swapchain(compose->physical, compose->device, compose->family, &compose->output, 0);
	}
	compose->readback = (unsigned)readback;
	if (result != VK_SUCCESS ||
	    compose->output.image_count > KWL_SWAPCHAIN_MAX ||
	    compose->output.format != compose->format) {
		printf("KWL VULKAN_ERROR operation=swapchain result=%d images=%u format=%d\n", (int)result, compose->output.image_count, (int)compose->output.format);
		vkdemo_display_close(compose->instance, compose->device, &compose->output);
		kwl_os_display_release(server, compose->physical, compose->display);
		compose->output_prepared = 0;
		return EIO;
	}

	/* A view, framebuffer and semaphore for each swapchain image. */
	printf("KWL STARTUP step=output-swapchain ms=%llu\n", (unsigned long long)(kwl_milliseconds() - started));
	started = kwl_milliseconds();
	result = compose_targets(compose);
	if (result != VK_SUCCESS) {
		printf("KWL VULKAN_ERROR operation=targets result=%d\n", (int)result);
		compose_targets_destroy(compose);
		vkdemo_display_close(compose->instance, compose->device, &compose->output);
		kwl_os_display_release(server, compose->physical, compose->display);
		compose->output_prepared = 0;
		return EIO;
	}

	/* No image has been drawn yet: the first frame of each is drawn whole. */
	printf("KWL STARTUP step=output-targets ms=%llu\n", (unsigned long long)(kwl_milliseconds() - started));
	memset(compose->image_frames, 0, sizeof(compose->image_frames));

	/* The displays' choice (read once), and the heads brought in line at the next look (ws113-p004b). */
	kwl_heads_config_load(server);
	compose->heads_stale = 1U;

	/* Succeeded: window mode owns the display through the swapchain. */
	compose->output_open = 1;
	server->dirty = 1;
	printf("KWL OUTPUT open width=%u height=%u images=%u format=%d\n", server->width, server->height, compose->output.image_count, (int)compose->format);
	return 0;
}

/*
 * Destroys the swapchain and the display surface once the device is idle,
 * which gives the display back (to the greeter's hand-over, or at exit).
 */
void
kwl_compose_output_close(
	struct kwl_server *server)
{
	struct kwl_compose *compose;

	/* A closed output has nothing to destroy. */
	compose = server->compose;
	if (compose == NULL)
		return;

	/* A surface prepared but never given a swapchain goes on its own. */
	if (!compose->output_open) {
		if (compose->output_prepared)
			vkdemo_display_close(compose->instance, compose->device, &compose->output);
		compose->output_prepared = 0;
		return;
	}

	/* No frame may still use the swapchain's images. */
	(void)vkDeviceWaitIdle(compose->device);

	/* The heads go with the output (ws113-p004b). */
	kwl_heads_close_all(server);

	/* The backdrop, the targets, then the swapchain and surface. */
	kwl_backdrop_destroy(compose);
	compose_targets_destroy(compose);
	vkdemo_display_close(compose->instance, compose->device, &compose->output);

	/* Gives back the display only after its acquired swapchain is gone. */
	kwl_os_display_release(server, compose->physical, compose->display);

	/* Marks the output as available for the next handoff. */
	compose->output_prepared = 0;
	compose->output_open = 0;
	printf("KWL OUTPUT closed\n");

	/* Succeeded: the swapchain and OS display ownership are released. */
	return;
}

/*
 * Draws one frame of window mode and presents it.  Returns 0 without
 * drawing while a frame is in flight or the output is closed.
 */
int
kwl_compose_draw(
	struct kwl_server *server)
{
	struct kwl_object *windows[KWL_FRAME_WINDOWS];
	struct kwl_compose *compose;
	const VkRect2D *region_drawn;
	VkRect2D region;
	uint64_t mark;
	uint64_t now;
	uint32_t image;
	unsigned count;
	unsigned all;
	unsigned missed;
	unsigned popups;
	unsigned subsurfaces;
	int partial;
	VkResult result;

	/* One frame at a time, and only with an output. */
	compose = server->compose;
	if (compose->in_flight || !compose->output_open || compose->output_lost)
		return 0;

	/* The frame shows the pointer where it is now. */
	server->pointer_moved = 0U;

	/* The windows to draw, bottom to top. */
	compose->frame_start_cycles = kwl_cycles();
	compose->frame_start_ms = kwl_milliseconds();
	all = compose_windows(server, windows, KWL_FRAME_WINDOWS);

	/* The anchor's windows first, then the heads' (heads.c draws them on theirs, ws113-p007). */
	count = compose_split(server, windows, all);
	compose->frame_heads = windows + count;
	compose->frame_head_count = all - count;

	/* The popups follow the windows in the list the frame holds (popup.c draws them). */
	popups = kwl_popup_collect(server, windows + all, KWL_FRAME_WINDOWS - all);

	/* The sub-surfaces follow them, held and told like them (subsurface.c draws them with their parents). */
	subsurfaces = kwl_subsurface_collect(server, windows + all + popups, KWL_FRAME_WINDOWS - all - popups);

	/* The frame's start, when the per-frame lines were asked for (ws099-p002's parts of a frame). */
	if (server->log_frames)
		printf("KWL LAT draw frame=%llu at_us=%llu\n", (unsigned long long)server->frame + 1U, (unsigned long long)kwl_microseconds());

	/* The next swapchain image (the wait for it is measured apart). */
	mark = kwl_cycles();
	result = vkAcquireNextImageKHR(compose->device, compose->output.swapchain, UINT64_MAX, compose->acquired, VK_NULL_HANDLE, &image);
	server->perf.compose_acquire_cycles += kwl_cycles() - mark;

	/* No image: the heads' windows are not drawn either. */
	if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
		compose->frame_heads = NULL;
		compose->frame_head_count = 0U;
	}

	/* The display went (unplugged, or another generation): no frame until the output moves (ws113-p004a). */
	if (result == VK_ERROR_SURFACE_LOST_KHR || result == VK_ERROR_OUT_OF_DATE_KHR) {
		if (!compose->output_lost) {
			printf("KWL OUTPUT lost operation=acquire result=%d\n", (int)result);
			compose->output_lost = 1U;
		}

		/* No frame, and no failure. */
		return 0;
	}

	/* Any other failure ends the compositor. */
	if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
		printf("KWL VULKAN_ERROR operation=acquire result=%d\n", (int)result);
		return EIO;
	}
	if (server->log_frames)
		printf("KWL LAT acquired frame=%llu at_us=%llu\n", (unsigned long long)server->frame + 1U, (unsigned long long)kwl_microseconds());

	/* The heads' images the frame draws too (ws113-p004b); one that is not ready asks for another frame. */
	missed = kwl_heads_acquire(server);

	/* The part of the image to draw: all of it, or the damage it has missed (its buffer age). */
	partial = compose_region(server, image, &region);

	/* The frame's commands. */
	region_drawn = NULL;
	if (partial)
		region_drawn = &region;
	result = compose_record(server, image, windows, count, region_drawn);
	if (result != VK_SUCCESS) {
		printf("KWL VULKAN_ERROR operation=record result=%d\n", (int)result);
		kwl_heads_frame_skipped(server);
		return EIO;
	}
	if (server->log_frames)
		printf("KWL LAT recorded frame=%llu at_us=%llu\n", (unsigned long long)server->frame + 1U, (unsigned long long)kwl_microseconds());

	/* Submitted and presented; the fence fd tells the event loop when it is done. */
	mark = kwl_cycles();
	result = compose_submit(server, image);
	server->perf.compose_present_cycles += kwl_cycles() - mark;
	if (result != VK_SUCCESS) {
		printf("KWL VULKAN_ERROR operation=submit result=%d\n", (int)result);
		return EIO;
	}

	/* The new images' layouts went with the frame. */
	kwl_import_layouts_done(compose);

	/* The CPU time of recording and submitting the frame. */
	server->perf.compose_draw_cycles += kwl_cycles() - compose->frame_start_cycles;

	/* The frame holds what it sampled until its fence signals, the popups and sub-surfaces too. */
	compose_hold(server, windows, all + popups + subsurfaces);
	compose->frame_heads = NULL;
	compose->frame_head_count = 0U;
	server->dirty = 0;
	if (missed > 0U)
		server->dirty = 1;
	server->damaged = 0;
	server->frame++;
	/* The first frame after App Home or Wiseview was asked to open or close, logged once (C5, also without --log-frames). */
	if (server->transition != NULL) {
		now = kwl_milliseconds();
		printf("KWL FIRST_FRAME what=%s frame=%llu request_ms=%llu ms=%llu\n", server->transition, (unsigned long long)server->frame,
		       (unsigned long long)server->transition_ms, (unsigned long long)(now - server->transition_ms));
		server->transition = NULL;
	}
	if (server->log_frames) {
		printf("KWL COMPOSE frame=%llu image=%u windows=%u at_ms=%llu\n", (unsigned long long)server->frame, image, count, (unsigned long long)kwl_milliseconds());
		printf("KWL LAT submit frame=%llu at_us=%llu\n", (unsigned long long)server->frame, (unsigned long long)kwl_microseconds());
	}

	/* Succeeded: one frame is in flight. */
	return 0;
}

/*
 * Ends the frame in flight after its fence signaled: the buffers it
 * sampled are released (when nothing else holds them) and the frame
 * callbacks of the surfaces it showed are sent.
 */
int
kwl_compose_complete(
	struct kwl_server *server)
{
	struct kwl_compose *compose;
	uint64_t elapsed;
	unsigned index;
	VkResult result;

	/* Only a frame in flight completes. */
	compose = server->compose;
	if (!compose->in_flight)
		return 0;

	/* The fence fd has done its work. */
	if (server->frame_fd >= 0) {
		close(server->frame_fd);
		server->frame_fd = -1;
	}

	/* The fence is signaled (the fd was readable, or the caller waits for it); it is reset for the next frame. */
	result = vkWaitForFences(compose->device, 1U, &compose->fence, VK_TRUE, UINT64_MAX);
	if (result == VK_SUCCESS)
		result = vkResetFences(compose->device, 1U, &compose->fence);
	if (result != VK_SUCCESS) {
		printf("KWL VULKAN_ERROR operation=fence result=%d\n", (int)result);
		return EIO;
	}

	/* The sampled buffers, and the frame callbacks. */
	for (index = 0; index < compose->held_count; index++)
		kwl_buffer_put(compose->held[index]);
	compose->held_count = 0;
	kwl_callbacks_done(&compose->callbacks);
	compose->in_flight = 0;

	/* A test image's capture sends the frame it copied (shot.c, ws173-p002). */
	kwl_shot_complete(server);
	if (server->log_frames)
		printf("KWL LAT shown frame=%llu at_us=%llu\n", (unsigned long long)server->frame, (unsigned long long)kwl_microseconds());

	/* The time from the start of the frame to its completion (reported by KWL PERF). */
	elapsed = kwl_cycles() - compose->frame_start_cycles;
	server->perf.compose_frames++;
	server->perf.compose_cycles += elapsed;

	/* The next frame waits a moment for the windows this one told (half its time, 4 to 50 ms). */
	server->frame_done_ms = kwl_milliseconds();
	server->frame_wait_ms = (server->frame_done_ms - compose->frame_start_ms) / 2U;
	if (server->frame_wait_ms < 4U)
		server->frame_wait_ms = 4U;
	if (server->frame_wait_ms > 50U)
		server->frame_wait_ms = 50U;

	/* Succeeded: the next frame may be drawn. */
	return 0;
}

/*
 * Tells whether a frame is in flight whose fence has no fd, so the event
 * loop must ask its status (kwl_compose_poll) rather than poll an fd.
 */
int
kwl_compose_waiting(
	struct kwl_server *server)
{
	/* Only window mode's frame without an exported fence. */
	if (server->compose == NULL || !server->compose->in_flight || server->compose->fence_fd)
		return 0;

	/* Succeeded. */
	return 1;
}

/*
 * Ends the frame in flight when its fence (without an fd) has signaled.
 */
void
kwl_compose_poll(
	struct kwl_server *server)
{
	VkResult result;
	int waiting;

	/* Nothing to ask. */
	waiting = kwl_compose_waiting(server);
	if (!waiting)
		return;

	/* A signaled fence ends the frame; otherwise a later pass asks again. */
	result = vkGetFenceStatus(server->compose->device, server->compose->fence);
	if (result == VK_SUCCESS)
		kwl_frame_done(server);
}

/*
 * Finishes the frame in flight now, waiting for it: before a client whose
 * buffers and callbacks it may hold is destroyed, and at exit.
 */
void
kwl_compose_quiesce(
	struct kwl_server *server)
{
	int error;

	/* Without window mode, or without a frame in flight, nothing waits. */
	if (server->compose == NULL || !server->compose->in_flight)
		return;

	/* The frame completes; a failure is the compositor's. */
	error = kwl_compose_complete(server);
	if (error != 0) {
		printf("KWL FAILED site=compose_poll errno=%d\n", error);
		server->failed = 1;
	}
}

/*
 * Releases every Vulkan object, after the output (at compositor exit).
 */
void
kwl_compose_close(
	struct kwl_server *server)
{
	struct kwl_compose *compose;
	unsigned index;

	/* Nothing was opened. */
	compose = server->compose;
	if (compose == NULL)
		return;

	/* The frame in flight and the output first, then the arrow. */
	if (compose->device != VK_NULL_HANDLE)
		(void)vkDeviceWaitIdle(compose->device);
	(void)kwl_compose_complete(server);
	kwl_compose_output_close(server);
	kwl_arrow_destroy(server);
	kwl_glass_close(server);

	/* The device's objects. */
	if (compose->device != VK_NULL_HANDLE) {
		for (index = 0; index < 2U; index++) {
			if (compose->pipelines[index] != VK_NULL_HANDLE)
				vkDestroyPipeline(compose->device, compose->pipelines[index], NULL);
		}

		/* The glass look's pipeline, layout and sampler. */
		if (compose->panel_pipeline != VK_NULL_HANDLE)
			vkDestroyPipeline(compose->device, compose->panel_pipeline, NULL);
		vkDestroyPipelineLayout(compose->device, compose->panel_layout, NULL);
		vkDestroySampler(compose->device, compose->linear_sampler, NULL);

		/* The corners. */
		vkDestroyBuffer(compose->device, compose->corners, NULL);
		vkFreeMemory(compose->device, compose->corners_memory, NULL);

		/* The pass, layouts, sampler, pools and synchronization. */
		vkDestroyRenderPass(compose->device, compose->pass, NULL);
		vkDestroyPipelineLayout(compose->device, compose->layout, NULL);
		vkDestroyDescriptorSetLayout(compose->device, compose->set_layout, NULL);
		vkDestroySampler(compose->device, compose->sampler, NULL);
		vkDestroyDescriptorPool(compose->device, compose->descriptors, NULL);
		vkDestroySemaphore(compose->device, compose->acquired, NULL);
		vkDestroyFence(compose->device, compose->fence, NULL);
		vkDestroyFence(compose->device, compose->hotplug, NULL);
		vkDestroyCommandPool(compose->device, compose->pool, NULL);
		vkDestroyDevice(compose->device, NULL);
	}

	/* The instance last. */
	if (compose->instance != VK_NULL_HANDLE)
		vkDestroyInstance(compose->instance, NULL);
	free(compose);
	server->compose = NULL;
}

/* Creates the instance, picks the first device with a graphics queue, and creates the device. */
static VkResult
compose_device(
	struct kwl_compose *compose)
{
	static const char *const instance_extensions[] = {
		VK_KHR_SURFACE_EXTENSION_NAME,
		VK_KHR_DISPLAY_EXTENSION_NAME,
		VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME,
		VK_KHR_EXTERNAL_MEMORY_CAPABILITIES_EXTENSION_NAME,
		VK_KHR_EXTERNAL_FENCE_CAPABILITIES_EXTENSION_NAME
	};
	/*
	 * The first five are always enabled: the dedicated allocation (and the
	 * requirement queries it depends on) is how a client's buffer is
	 * imported (import.c, WS103).  The external fence pair follows when the
	 * device has it.
	 */
	static const char *const device_extensions[] = {
		VK_KHR_SWAPCHAIN_EXTENSION_NAME,
		VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME,
		VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
		VK_KHR_GET_MEMORY_REQUIREMENTS_2_EXTENSION_NAME,
		VK_KHR_DEDICATED_ALLOCATION_EXTENSION_NAME,
		VK_KHR_EXTERNAL_FENCE_EXTENSION_NAME,
		VK_KHR_EXTERNAL_FENCE_FD_EXTENSION_NAME
	};
	const char *instance_names[COMPOSE_EXTENSIONS_MAX];
	const char *device_names[COMPOSE_EXTENSIONS_MAX];
	uint32_t instance_count;
	uint32_t device_count;
	uint32_t extra;
	int surface_counter;
	int display_control;
	VkExternalFenceHandleTypeFlagBits fence_type;
	VkExtensionProperties available[32];
	uint32_t found;
	uint32_t wanted;
	VkApplicationInfo application;
	VkInstanceCreateInfo instance;
	VkQueueFamilyProperties families[16];
	VkDeviceQueueCreateInfo queue;
	VkDeviceCreateInfo device;
	float priority;
	uint32_t count;
	uint32_t index;
	int match;
	VkResult result;

	/* Append the OS extensions to the existing instance extension list. */
	memcpy(instance_names, instance_extensions, sizeof(instance_extensions));
	instance_count = 5U;

	/* The surface counters, which the display control needs, when the library offers them (ws113-p004a). */
	surface_counter = 0;
	found = 32U;
	result = vkEnumerateInstanceExtensionProperties(NULL, &found, available);
	if (result != VK_SUCCESS && result != VK_INCOMPLETE)
		found = 0U;
	for (index = 0; index < found; index++) {
		/* The extension's name. */
		match = strcmp(available[index].extensionName, VK_EXT_DISPLAY_SURFACE_COUNTER_EXTENSION_NAME);
		if (match == 0)
			surface_counter = 1;
	}

	/* Enabled when offered. */
	if (surface_counter) {
		instance_names[instance_count] = VK_EXT_DISPLAY_SURFACE_COUNTER_EXTENSION_NAME;
		instance_count++;
	}

	/* The OS's own instance extensions after them. */
	extra = kl_backend_gpu_instance_extensions(instance_names + instance_count, COMPOSE_EXTENSIONS_MAX - instance_count);
	if (extra > COMPOSE_EXTENSIONS_MAX - instance_count)
		return VK_ERROR_EXTENSION_NOT_PRESENT;
	instance_count += extra;

	/* The instance, with the display extensions and those the external fd extensions need (Vulkan 1.0). */
	memset(&application, 0, sizeof(application));
	application.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
	application.pApplicationName = "wayland";
	application.apiVersion = VK_API_VERSION_1_0;

	/* Describes the instance separately from its application identity. */
	memset(&instance, 0, sizeof(instance));
	instance.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
	instance.pApplicationInfo = &application;
	instance.enabledExtensionCount = instance_count;
	instance.ppEnabledExtensionNames = instance_names;
	result = vkCreateInstance(&instance, NULL, &compose->instance);
	if (result != VK_SUCCESS)
		return result;

	/* The first physical device. */
	count = 1U;
	result = vkEnumeratePhysicalDevices(compose->instance, &count, &compose->physical);
	if ((result != VK_SUCCESS && result != VK_INCOMPLETE) || count == 0U)
		return VK_ERROR_INITIALIZATION_FAILED;

	/* Its first graphics queue family. */
	count = 16U;
	vkGetPhysicalDeviceQueueFamilyProperties(compose->physical, &count, families);
	compose->family = UINT32_MAX;
	for (index = 0; index < count; index++) {
		if ((families[index].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0U) {
			compose->family = index;
			break;
		}
	}

	/* A device without a graphics queue cannot draw. */
	if (compose->family == UINT32_MAX)
		return VK_ERROR_INITIALIZATION_FAILED;

	/* The external fence extensions only when the device has both (the native i915 has none). */
	found = 32U;
	result = vkEnumerateDeviceExtensionProperties(compose->physical, NULL, &found, available);
	if (result != VK_SUCCESS && result != VK_INCOMPLETE)
		return result;
	wanted = 0;
	display_control = 0;
	for (index = 0; index < found; index++) {
		match = strcmp(available[index].extensionName, VK_KHR_EXTERNAL_FENCE_EXTENSION_NAME);
		if (match == 0)
			wanted++;
		match = strcmp(available[index].extensionName, VK_KHR_EXTERNAL_FENCE_FD_EXTENSION_NAME);
		if (match == 0)
			wanted++;

		/* The display control (hotplug), with the instance's surface counters. */
		match = strcmp(available[index].extensionName, VK_EXT_DISPLAY_CONTROL_EXTENSION_NAME);
		if (match == 0 && surface_counter)
			display_control = 1;
	}

	/* Export only when the device and the OS both support frame fence fds. */
	fence_type = kl_backend_gpu_frame_fence_type();
	compose->fence_fd = 0;
	if (wanted == 2U && fence_type != 0)
		compose->fence_fd = 1;

	/* Keeps the common extensions and both optional frame-fence extensions together. */
	device_count = 5U;
	if (compose->fence_fd)
		device_count = 7U;

	/* Appends the OS extensions after the selected common device extension list. */
	memcpy(device_names, device_extensions, device_count * sizeof(device_names[0]));
	extra = kl_backend_gpu_device_extensions(compose->physical, device_names + device_count, COMPOSE_EXTENSIONS_MAX - device_count);
	if (extra > COMPOSE_EXTENSIONS_MAX - device_count)
		return VK_ERROR_EXTENSION_NOT_PRESENT;
	device_count += extra;

	/* The display control last, when the library offers it and there is room. */
	if (display_control && device_count < COMPOSE_EXTENSIONS_MAX) {
		device_names[device_count] = VK_EXT_DISPLAY_CONTROL_EXTENSION_NAME;
		device_count++;
	} else {
		display_control = 0;
	}

	/* The device, with one queue, the swapchain and the external memory (and fence) fd extensions. */
	priority = 1.0f;
	memset(&queue, 0, sizeof(queue));
	queue.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
	queue.queueFamilyIndex = compose->family;
	queue.queueCount = 1U;
	queue.pQueuePriorities = &priority;

	/* Describes the device separately from its selected queue. */
	memset(&device, 0, sizeof(device));
	device.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
	device.queueCreateInfoCount = 1U;
	device.pQueueCreateInfos = &queue;
	device.enabledExtensionCount = device_count;
	device.ppEnabledExtensionNames = device_names;
	result = vkCreateDevice(compose->physical, &device, NULL, &compose->device);
	if (result != VK_SUCCESS)
		return result;

	/* Resolve the optional fence export through the device dispatch table. */
	compose->get_fence_fd = NULL;
	if (compose->fence_fd) {
		compose->get_fence_fd = (PFN_vkGetFenceFdKHR)vkGetDeviceProcAddr(compose->device, "vkGetFenceFdKHR");
		if (compose->get_fence_fd == NULL)
			compose->fence_fd = 0;
	}

	/* The hotplug's entry point (ws113-p004a). */
	compose->register_device_event = NULL;
	if (display_control)
		compose->register_device_event = (PFN_vkRegisterDeviceEventEXT)vkGetDeviceProcAddr(compose->device, "vkRegisterDeviceEventEXT");

	/* Succeeded: the queue the frames are submitted to. */
	vkGetDeviceQueue(compose->device, compose->family, 0U, &compose->queue);
	return VK_SUCCESS;
}

/*
 * Takes the output's size and refresh from the display that window mode
 * will use (the first one that shows an unrotated image, as
 * vkdemo_display_open chooses): its native resolution when no size was
 * given, and the refresh of the mode at the output's size.
 */
static VkResult
compose_display(
	struct kwl_server *server)
{
	struct kwl_compose *compose;
	VkDisplayPropertiesKHR *properties;
	VkPhysicalDeviceProperties device;
	VkDisplayKHR display;
	VkExtent2D resolution;
	VkResult result;
	uint32_t count;
	uint32_t index;
	uint32_t refresh;

	/* Counts the displays before their properties are read. */
	compose = server->compose;
	count = 0;
	result = vkGetPhysicalDeviceDisplayPropertiesKHR(compose->physical, &count, NULL);
	if (result != VK_SUCCESS)
		return result;

	/* Without a display there is nothing to show windows on. */
	if (count == 0U)
		return VK_ERROR_INITIALIZATION_FAILED;

	/* The properties of every display. */
	properties = calloc(count, sizeof(*properties));
	if (properties == NULL)
		return VK_ERROR_OUT_OF_HOST_MEMORY;

	/* Reads the displays, which a hot-plug may have changed since they were counted. */
	result = vkGetPhysicalDeviceDisplayPropertiesKHR(compose->physical, &count, properties);
	if (result != VK_SUCCESS && result != VK_INCOMPLETE) {
		free(properties);
		return result;
	}

	/* The first display that shows an unrotated image is the one window mode opens. */
	display = VK_NULL_HANDLE;
	resolution.width = 0U;
	resolution.height = 0U;
	for (index = 0U; index < count; index++) {
		/* A display that cannot show the image unrotated is passed over, as vkdemo_display_open does. */
		if ((properties[index].supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR) == 0U)
			continue;

		/* This display, its native resolution and its name. */
		display = properties[index].display;
		resolution = properties[index].physicalResolution;
		if (properties[index].displayName != NULL) {
			(void)snprintf(compose->display_name, sizeof(compose->display_name), "%s", properties[index].displayName);
		}

		/* The first such display is the one. */
		break;
	}

	/* Keeps the chosen display for the OS acquire and release hooks. */
	compose->display = display;
	compose->boot_display = display;

	/* The display handle stays valid after its properties are freed. */
	free(properties);

	/* Without such a display there is nothing to show windows on. */
	if (display == VK_NULL_HANDLE)
		return VK_ERROR_INITIALIZATION_FAILED;

	/*
	 * Without --width and --height the output takes the display's native
	 * resolution (a login's session and screen); a display that does not know
	 * its resolution keeps the default size.
	 */
	if (!server->size_given &&
	    resolution.width != 0U &&
	    resolution.height != 0U) {
		server->width = resolution.width;
		server->height = resolution.height;
	}

	/* The refresh of the mode at the output's size, which wl_output tells the clients. */
	result = compose_refresh(server, display, server->width, server->height, &refresh);
	if (result != VK_SUCCESS)
		return result;

	/* wl_output tells the clients this refresh. */
	server->refresh = refresh;

	/* The machine log names the device and the output the compositor will open. */
	vkGetPhysicalDeviceProperties(compose->physical, &device);
	printf("KWL DISPLAY device=%s width=%u height=%u refresh_mhz=%u\n", device.deviceName, server->width, server->height, server->refresh);

	/* Succeeded: the output's size and refresh are known. */
	return VK_SUCCESS;
}

/* Records the device's largest 2D image and its number of memory types, which bound a client buffer's description. */
static void
compose_limits(
	struct kwl_server *server)
{
	VkPhysicalDeviceProperties device;
	VkPhysicalDeviceMemoryProperties memory;

	/* The device the GPU buffers are imported into. */
	server->gpu_device.instance = server->compose->instance;
	server->gpu_device.physical = server->compose->physical;
	server->gpu_device.device = server->compose->device;

	/* The largest width and height of a 2D image. */
	vkGetPhysicalDeviceProperties(server->compose->physical, &device);
	server->gpu_device.max_dimension = device.limits.maxImageDimension2D;

	/* The memory types a buffer's memory may be one of. */
	vkGetPhysicalDeviceMemoryProperties(server->compose->physical, &memory);
	server->gpu_device.memory_type_count = memory.memoryTypeCount;

	/* Succeeded: descriptions are checked against these. */
	return;
}

/*
 * Finds the refresh of a display's mode at the output's size, or, when the
 * display has no such mode (vkdemo_display_open then asks for one), the
 * refresh of its first mode, which that request uses.
 */
static VkResult
compose_refresh(
	struct kwl_server *server,
	VkDisplayKHR display,
	uint32_t width,
	uint32_t height,
	uint32_t *refresh)
{
	struct kwl_compose *compose;
	VkDisplayModePropertiesKHR *modes;
	VkResult result;
	uint32_t count;
	uint32_t index;
	uint32_t mode_width;
	uint32_t mode_height;

	/* Counts the display's modes. */
	compose = server->compose;
	count = 0U;
	result = vkGetDisplayModePropertiesKHR(compose->physical, display, &count, NULL);
	if (result != VK_SUCCESS)
		return result;

	/* A display without modes has no refresh to take. */
	if (count == 0U)
		return VK_ERROR_INITIALIZATION_FAILED;

	/* The modes' parameters. */
	modes = calloc(count, sizeof(*modes));
	if (modes == NULL)
		return VK_ERROR_OUT_OF_HOST_MEMORY;

	/* Reads the modes, which may have changed since they were counted. */
	result = vkGetDisplayModePropertiesKHR(compose->physical, display, &count, modes);
	if (result != VK_SUCCESS && result != VK_INCOMPLETE) {
		free(modes);
		return result;
	}

	/* The first mode's refresh, unless a mode has the output's size. */
	*refresh = modes[0].parameters.refreshRate;
	for (index = 0U; index < count; index++) {
		/* The mode's visible size. */
		mode_width = modes[index].parameters.visibleRegion.width;
		mode_height = modes[index].parameters.visibleRegion.height;

		/* A mode at the output's size gives its own refresh. */
		if (mode_width == width && mode_height == height) {
			*refresh = modes[index].parameters.refreshRate;
			break;
		}
	}

	/* Frees the mode list; the refresh was copied out of it. */
	free(modes);

	/* A zero refresh is no refresh to tell the clients. */
	if (*refresh == 0U)
		return VK_ERROR_INITIALIZATION_FAILED;

	/* Succeeded: the refresh is known. */
	return VK_SUCCESS;
}

/* Creates the descriptor and pipeline layouts, the sampler, the pools and the frame's synchronization. */
static VkResult
compose_objects(
	struct kwl_compose *compose)
{
	VkDescriptorSetLayoutBinding binding;
	VkDescriptorSetLayoutCreateInfo set_layout;
	VkPushConstantRange range;
	VkPipelineLayoutCreateInfo layout;
	VkSamplerCreateInfo sampler;
	VkDescriptorPoolSize pool_size;
	VkDescriptorPoolCreateInfo pool;
	VkCommandPoolCreateInfo command_pool;
	VkCommandBufferAllocateInfo command;
	VkExportFenceCreateInfo export;
	VkFenceCreateInfo fence;
	VkSemaphoreCreateInfo semaphore;
	VkResult result;

	/* One sampled image for the fragment shader. */
	memset(&binding, 0, sizeof(binding));
	binding.binding = 0U;
	binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	binding.descriptorCount = 1U;
	binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
	memset(&set_layout, 0, sizeof(set_layout));
	set_layout.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
	set_layout.bindingCount = 1U;
	set_layout.pBindings = &binding;
	result = vkCreateDescriptorSetLayout(compose->device, &set_layout, NULL, &compose->set_layout);
	if (result != VK_SUCCESS)
		return result;

	/* The quad's place and texture coordinates as push constants. */
	memset(&range, 0, sizeof(range));
	range.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
	range.size = 8U * sizeof(float);
	memset(&layout, 0, sizeof(layout));
	layout.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
	layout.setLayoutCount = 1U;
	layout.pSetLayouts = &compose->set_layout;
	layout.pushConstantRangeCount = 1U;
	layout.pPushConstantRanges = &range;
	result = vkCreatePipelineLayout(compose->device, &layout, NULL, &compose->layout);
	if (result != VK_SUCCESS)
		return result;

	/* Nearest sampling at the edge: a window drawn at its own size is copied pixel for pixel. */
	memset(&sampler, 0, sizeof(sampler));
	sampler.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
	sampler.magFilter = VK_FILTER_NEAREST;
	sampler.minFilter = VK_FILTER_NEAREST;
	sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
	sampler.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	sampler.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	sampler.maxLod = 0.0f;
	result = vkCreateSampler(compose->device, &sampler, NULL, &compose->sampler);
	if (result != VK_SUCCESS)
		return result;

	/* Linear sampling for the glass look's blurred wallpaper, which is smaller than the output. */
	sampler.magFilter = VK_FILTER_LINEAR;
	sampler.minFilter = VK_FILTER_LINEAR;
	result = vkCreateSampler(compose->device, &sampler, NULL, &compose->linear_sampler);
	if (result != VK_SUCCESS)
		return result;

	/* The glass look's constants (place, image part, box, color, shape, output) reach both stages. */
	range.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
	range.size = KWL_PANEL_CONSTANTS * sizeof(float);
	result = vkCreatePipelineLayout(compose->device, &layout, NULL, &compose->panel_layout);
	if (result != VK_SUCCESS)
		return result;

	/* A descriptor set for each imported buffer, freed with it. */
	memset(&pool_size, 0, sizeof(pool_size));
	pool_size.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	pool_size.descriptorCount = KWL_DESCRIPTOR_MAX;
	memset(&pool, 0, sizeof(pool));
	pool.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
	pool.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
	pool.maxSets = KWL_DESCRIPTOR_MAX;
	pool.poolSizeCount = 1U;
	pool.pPoolSizes = &pool_size;
	result = vkCreateDescriptorPool(compose->device, &pool, NULL, &compose->descriptors);
	if (result != VK_SUCCESS)
		return result;

	/* One command buffer, re-recorded every frame. */
	memset(&command_pool, 0, sizeof(command_pool));
	command_pool.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
	command_pool.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
	command_pool.queueFamilyIndex = compose->family;
	result = vkCreateCommandPool(compose->device, &command_pool, NULL, &compose->pool);
	if (result != VK_SUCCESS)
		return result;
	memset(&command, 0, sizeof(command));
	command.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
	command.commandPool = compose->pool;
	command.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	command.commandBufferCount = 1U;
	result = vkAllocateCommandBuffers(compose->device, &command, &compose->command);
	if (result != VK_SUCCESS)
		return result;

	/* The frame's fence, exportable as an fd the event loop polls (design D3). */
	memset(&export, 0, sizeof(export));
	export.sType = VK_STRUCTURE_TYPE_EXPORT_FENCE_CREATE_INFO;
	export.handleTypes = kl_backend_gpu_frame_fence_type();

	/* Describes the frame fence with the OS export type when export is available. */
	memset(&fence, 0, sizeof(fence));
	fence.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
	if (compose->fence_fd)
		fence.pNext = &export;
	result = vkCreateFence(compose->device, &fence, NULL, &compose->fence);
	if (result != VK_SUCCESS)
		return result;

	/* The acquire semaphore. */
	memset(&semaphore, 0, sizeof(semaphore));
	semaphore.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
	result = vkCreateSemaphore(compose->device, &semaphore, NULL, &compose->acquired);
	if (result != VK_SUCCESS)
		return result;

	/* The quad's corners. */
	result = compose_corners(compose);
	return result;
}

/* Creates the vertex buffer of a quad's two triangles, written once by the CPU. */
static VkResult
compose_corners(
	struct kwl_compose *compose)
{
	static const float corners[KWL_QUAD_VERTICES * 2U] = {
		0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f,
		0.0f, 1.0f, 1.0f, 0.0f, 1.0f, 1.0f
	};
	VkPhysicalDeviceMemoryProperties memory;
	VkMemoryRequirements requirements;
	VkMemoryAllocateInfo allocate;
	VkBufferCreateInfo buffer;
	VkMemoryPropertyFlags wanted;
	uint32_t index;
	VkResult result;
	void *map;

	/* The buffer. */
	memset(&buffer, 0, sizeof(buffer));
	buffer.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
	buffer.size = sizeof(corners);
	buffer.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
	buffer.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	result = vkCreateBuffer(compose->device, &buffer, NULL, &compose->corners);
	if (result != VK_SUCCESS)
		return result;

	/* Host-visible, coherent memory for it. */
	vkGetBufferMemoryRequirements(compose->device, compose->corners, &requirements);
	vkGetPhysicalDeviceMemoryProperties(compose->physical, &memory);
	wanted = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
	for (index = 0; index < memory.memoryTypeCount; index++) {
		if ((requirements.memoryTypeBits & (1U << index)) != 0U &&
		    (memory.memoryTypes[index].propertyFlags & wanted) == wanted)
			break;
	}

	/* Without such memory the corners cannot be written. */
	if (index == memory.memoryTypeCount)
		return VK_ERROR_FORMAT_NOT_SUPPORTED;

	/* The memory, bound. */
	memset(&allocate, 0, sizeof(allocate));
	allocate.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	allocate.allocationSize = requirements.size;
	allocate.memoryTypeIndex = index;
	result = vkAllocateMemory(compose->device, &allocate, NULL, &compose->corners_memory);
	if (result != VK_SUCCESS)
		return result;
	result = vkBindBufferMemory(compose->device, compose->corners, compose->corners_memory, 0U);
	if (result != VK_SUCCESS)
		return result;

	/* The corners, written once. */
	result = vkMapMemory(compose->device, compose->corners_memory, 0U, sizeof(corners), 0U, &map);
	if (result != VK_SUCCESS)
		return result;
	memcpy(map, corners, sizeof(corners));
	vkUnmapMemory(compose->device, compose->corners_memory);

	/* Succeeded. */
	return VK_SUCCESS;
}

/* Creates the render pass for the output's format and the two pipelines. */
static VkResult
compose_pass(
	struct kwl_compose *compose)
{
	VkAttachmentDescription attachment;
	VkAttachmentReference reference;
	VkSubpassDescription subpass;
	VkSubpassDependency dependency;
	VkRenderPassCreateInfo pass;
	VkShaderModuleCreateInfo module;
	VkShaderModule vertex;
	VkShaderModule fragment;
	VkResult result;

	/* One color attachment, cleared to the background and presented. */
	memset(&attachment, 0, sizeof(attachment));
	attachment.format = compose->format;
	attachment.samples = VK_SAMPLE_COUNT_1_BIT;
	attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
	attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
	attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
	attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
	attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	attachment.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
	memset(&reference, 0, sizeof(reference));
	reference.attachment = 0U;
	reference.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	memset(&subpass, 0, sizeof(subpass));
	subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
	subpass.colorAttachmentCount = 1U;
	subpass.pColorAttachments = &reference;

	/* The acquired image is written only after the acquire semaphore. */
	memset(&dependency, 0, sizeof(dependency));
	dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
	dependency.dstSubpass = 0U;
	dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
	dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
	dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
	memset(&pass, 0, sizeof(pass));
	pass.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
	pass.attachmentCount = 1U;
	pass.pAttachments = &attachment;
	pass.subpassCount = 1U;
	pass.pSubpasses = &subpass;
	pass.dependencyCount = 1U;
	pass.pDependencies = &dependency;
	result = vkCreateRenderPass(compose->device, &pass, NULL, &compose->pass);
	if (result != VK_SUCCESS)
		return result;

	/* The shaders, needed only while the pipelines are created. */
	memset(&module, 0, sizeof(module));
	module.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
	module.codeSize = sizeof(kwl_quad_vert);
	module.pCode = kwl_quad_vert;
	result = vkCreateShaderModule(compose->device, &module, NULL, &vertex);
	if (result != VK_SUCCESS)
		return result;
	module.codeSize = sizeof(kwl_quad_frag);
	module.pCode = kwl_quad_frag;
	result = vkCreateShaderModule(compose->device, &module, NULL, &fragment);
	if (result != VK_SUCCESS) {
		vkDestroyShaderModule(compose->device, vertex, NULL);
		return result;
	}

	/* The opaque and the alpha pipelines. */
	result = compose_pipeline(compose, KWL_DRAW_OPAQUE, vertex, fragment, compose->layout, &compose->pipelines[KWL_DRAW_OPAQUE]);
	if (result == VK_SUCCESS)
		result = compose_pipeline(compose, KWL_DRAW_ALPHA, vertex, fragment, compose->layout, &compose->pipelines[KWL_DRAW_ALPHA]);
	vkDestroyShaderModule(compose->device, vertex, NULL);
	vkDestroyShaderModule(compose->device, fragment, NULL);
	if (result != VK_SUCCESS)
		return result;

	/* The glass look's pipeline. */
	result = compose_panel_pipeline(compose);
	return result;
}

/* Creates the glass look's pipeline: its shaders, blended with premultiplied alpha. */
static VkResult
compose_panel_pipeline(
	struct kwl_compose *compose)
{
	VkShaderModuleCreateInfo module;
	VkShaderModule vertex;
	VkShaderModule fragment;
	VkResult result;

	/* The shaders, needed only while the pipeline is created. */
	memset(&module, 0, sizeof(module));
	module.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
	module.codeSize = sizeof(kwl_panel_vert);
	module.pCode = kwl_panel_vert;
	result = vkCreateShaderModule(compose->device, &module, NULL, &vertex);
	if (result != VK_SUCCESS)
		return result;
	module.codeSize = sizeof(kwl_panel_frag);
	module.pCode = kwl_panel_frag;
	result = vkCreateShaderModule(compose->device, &module, NULL, &fragment);
	if (result != VK_SUCCESS) {
		vkDestroyShaderModule(compose->device, vertex, NULL);
		return result;
	}

	/* Every shape is blended: the shader's output is premultiplied. */
	result = compose_pipeline(compose, KWL_DRAW_ALPHA, vertex, fragment, compose->panel_layout, &compose->panel_pipeline);
	vkDestroyShaderModule(compose->device, vertex, NULL);
	vkDestroyShaderModule(compose->device, fragment, NULL);
	return result;
}

/* Creates a pipeline that draws a quad the given way, with the given layout. */
static VkResult
compose_pipeline(
	struct kwl_compose *compose,
	enum kwl_draw draw,
	VkShaderModule vertex,
	VkShaderModule fragment,
	VkPipelineLayout layout,
	VkPipeline *result)
{
	static const VkDynamicState dynamic[] = {
		VK_DYNAMIC_STATE_VIEWPORT,
		VK_DYNAMIC_STATE_SCISSOR
	};
	VkPipelineShaderStageCreateInfo stages[2];
	VkVertexInputBindingDescription binding;
	VkVertexInputAttributeDescription attribute;
	VkPipelineVertexInputStateCreateInfo input;
	VkPipelineInputAssemblyStateCreateInfo assembly;
	VkPipelineViewportStateCreateInfo viewport;
	VkPipelineRasterizationStateCreateInfo raster;
	VkPipelineMultisampleStateCreateInfo multisample;
	VkPipelineColorBlendAttachmentState blend_attachment;
	VkPipelineColorBlendStateCreateInfo blend;
	VkPipelineDynamicStateCreateInfo dynamic_state;
	VkGraphicsPipelineCreateInfo pipeline;

	/* The two shader stages. */
	memset(stages, 0, sizeof(stages));
	stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
	stages[0].module = vertex;
	stages[0].pName = "main";
	stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
	stages[1].module = fragment;
	stages[1].pName = "main";

	/* One vertex buffer: the corners of the quad's two triangles, a vec2 each. */
	memset(&binding, 0, sizeof(binding));
	binding.binding = 0U;
	binding.stride = 2U * sizeof(float);
	binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
	memset(&attribute, 0, sizeof(attribute));
	attribute.location = 0U;
	attribute.binding = 0U;
	attribute.format = VK_FORMAT_R32G32_SFLOAT;
	attribute.offset = 0U;
	memset(&input, 0, sizeof(input));
	input.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
	input.vertexBindingDescriptionCount = 1U;
	input.pVertexBindingDescriptions = &binding;
	input.vertexAttributeDescriptionCount = 1U;
	input.pVertexAttributeDescriptions = &attribute;
	memset(&assembly, 0, sizeof(assembly));
	assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
	assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

	/* The viewport and scissor are set per frame. */
	memset(&viewport, 0, sizeof(viewport));
	viewport.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
	viewport.viewportCount = 1U;
	viewport.scissorCount = 1U;
	memset(&raster, 0, sizeof(raster));
	raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
	raster.polygonMode = VK_POLYGON_MODE_FILL;
	raster.cullMode = VK_CULL_MODE_NONE;
	raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
	raster.lineWidth = 1.0f;
	memset(&multisample, 0, sizeof(multisample));
	multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
	multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

	/*
	 * Opaque windows replace what is under them; the alpha pipeline blends
	 * premultiplied colors (Wayland's convention for images with alpha).
	 */
	memset(&blend_attachment, 0, sizeof(blend_attachment));
	blend_attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
	    VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
	if (draw == KWL_DRAW_ALPHA) {
		blend_attachment.blendEnable = VK_TRUE;
		blend_attachment.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
		blend_attachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
		blend_attachment.colorBlendOp = VK_BLEND_OP_ADD;
		blend_attachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
		blend_attachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
		blend_attachment.alphaBlendOp = VK_BLEND_OP_ADD;
	}

	/* One attachment's blending, and the viewport and scissor left to the frame. */
	memset(&blend, 0, sizeof(blend));
	blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
	blend.attachmentCount = 1U;
	blend.pAttachments = &blend_attachment;
	memset(&dynamic_state, 0, sizeof(dynamic_state));
	dynamic_state.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
	dynamic_state.dynamicStateCount = 2U;
	dynamic_state.pDynamicStates = dynamic;

	/* The pipeline. */
	memset(&pipeline, 0, sizeof(pipeline));
	pipeline.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
	pipeline.stageCount = 2U;
	pipeline.pStages = stages;
	pipeline.pVertexInputState = &input;
	pipeline.pInputAssemblyState = &assembly;
	pipeline.pViewportState = &viewport;
	pipeline.pRasterizationState = &raster;
	pipeline.pMultisampleState = &multisample;
	pipeline.pColorBlendState = &blend;
	pipeline.pDynamicState = &dynamic_state;
	pipeline.layout = layout;
	pipeline.renderPass = compose->pass;
	return vkCreateGraphicsPipelines(compose->device, VK_NULL_HANDLE, 1U, &pipeline, NULL, result);
}

/* Creates a view, a framebuffer and a present semaphore for each swapchain image. */
static VkResult
compose_targets(
	struct kwl_compose *compose)
{
	VkImageViewCreateInfo view;
	VkFramebufferCreateInfo framebuffer;
	VkSemaphoreCreateInfo semaphore;
	uint32_t index;
	VkResult result;

	/* Each image in turn. */
	for (index = 0; index < compose->output.image_count; index++) {
		memset(&view, 0, sizeof(view));
		view.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
		view.image = compose->output.images[index];
		view.viewType = VK_IMAGE_VIEW_TYPE_2D;
		view.format = compose->format;
		view.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		view.subresourceRange.levelCount = 1U;
		view.subresourceRange.layerCount = 1U;
		result = vkCreateImageView(compose->device, &view, NULL, &compose->views[index]);
		if (result != VK_SUCCESS)
			return result;

		/* Its framebuffer of the output's size. */
		memset(&framebuffer, 0, sizeof(framebuffer));
		framebuffer.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
		framebuffer.renderPass = compose->pass;
		framebuffer.attachmentCount = 1U;
		framebuffer.pAttachments = &compose->views[index];
		framebuffer.width = compose->output.width;
		framebuffer.height = compose->output.height;
		framebuffer.layers = 1U;
		result = vkCreateFramebuffer(compose->device, &framebuffer, NULL, &compose->framebuffers[index]);
		if (result != VK_SUCCESS)
			return result;

		/* The semaphore its present waits for. */
		memset(&semaphore, 0, sizeof(semaphore));
		semaphore.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
		result = vkCreateSemaphore(compose->device, &semaphore, NULL, &compose->rendered[index]);
		if (result != VK_SUCCESS)
			return result;
	}

	/* Succeeded. */
	return VK_SUCCESS;
}

/* Destroys the per-image views, framebuffers and semaphores. */
static void
compose_targets_destroy(
	struct kwl_compose *compose)
{
	uint32_t index;

	/* Each image's objects, those that were made. */
	for (index = 0; index < KWL_SWAPCHAIN_MAX; index++) {
		if (compose->framebuffers[index] != VK_NULL_HANDLE)
			vkDestroyFramebuffer(compose->device, compose->framebuffers[index], NULL);
		if (compose->views[index] != VK_NULL_HANDLE)
			vkDestroyImageView(compose->device, compose->views[index], NULL);
		if (compose->rendered[index] != VK_NULL_HANDLE)
			vkDestroySemaphore(compose->device, compose->rendered[index], NULL);
		compose->framebuffers[index] = VK_NULL_HANDLE;
		compose->views[index] = VK_NULL_HANDLE;
		compose->rendered[index] = VK_NULL_HANDLE;
	}
}

/*
 * Collects the windows to draw, bottom to top: mapped surfaces of live
 * clients with a current image that window mode can sample.
 */
static unsigned
compose_windows(
	struct kwl_server *server,
	struct kwl_object **windows,
	unsigned capacity)
{
	const struct kwl_import *image;
	struct kwl_client *client;
	struct kwl_object *surface;
	unsigned count;
	unsigned index;
	unsigned at;

	/* Every drawable window. */
	count = 0;
	for (client = server->clients; client != NULL; client = client->next) {
		if (client->fatal)
			continue;
		for (surface = client->objects; surface != NULL; surface = surface->next) {
			if (surface->kind != KWL_SURFACE ||
			    surface->dead ||
			    !surface->mapped ||
			    surface->current == NULL)
				continue;

			/* A window whose image is not made yet waits. */
			image = surface_image(surface);
			if (image == NULL)
				continue;

			/* Insertion by map order keeps the list bottom to top. */
			if (count == capacity)
				break;
			at = count;
			while (at > 0 && windows[at - 1]->map_order > surface->map_order)
				at--;
			for (index = count; index > at; index--)
				windows[index] = windows[index - 1];
			windows[at] = surface;
			count++;
		}
	}

	/* Succeeded: the windows to draw. */
	return count;
}

/*
 * Puts the anchor's windows first in a frame's list (bottom to top), then
 * the heads' in the same order (ws113-p007); a window of an output not
 * shown (the mirror, a head gone) is the anchor's.  Returns how many are
 * the anchor's.
 */
static unsigned
compose_split(
	struct kwl_server *server,
	struct kwl_object **windows,
	unsigned count)
{
	struct kwl_object *heads[KWL_FRAME_WINDOWS];
	struct kwl_plane_rect outputs[KWL_PLANE_SLOTS];
	struct kwl_object *parent;
	unsigned anchor;
	unsigned others;
	unsigned slots;
	unsigned slot;
	unsigned index;

	/* Each window in turn, to its output's part of the list. */
	slots = kwl_outputs(server, outputs);
	anchor = 0U;
	others = 0U;
	for (index = 0U; index < count; index++) {
		/* A sheet is its parent's output's (ws090-p014's sheets are drawn under the parent's title bar). */
		parent = kwl_sheet_parent(windows[index]);
		if (parent != NULL)
			windows[index]->output = parent->output;

		/* A window of an output not shown is the anchor's. */
		slot = windows[index]->output;
		if (slot >= slots || outputs[slot].width == 0U) {
			windows[index]->output = KWL_PLANE_ANCHOR;
			slot = KWL_PLANE_ANCHOR;
		}

		/* The anchor's in place, the heads' aside. */
		if (slot == KWL_PLANE_ANCHOR) {
			windows[anchor] = windows[index];
			anchor++;
		} else {
			heads[others] = windows[index];
			others++;
		}
	}

	/* The heads' after the anchor's, and each output's list logged when it changes. */
	memcpy(windows + anchor, heads, others * sizeof(heads[0]));
	compose_lists_log(server, windows, count);
	return anchor;
}

/*
 * Logs each output's windows (its render list's surfaces, bottom to top)
 * when they differ from the last frame's: "KWL RENDER output=N
 * surfaces=A,B" (ws113-p007, D-ATOMIC: a window is in its output's list
 * alone).
 */
static void
compose_lists_log(
	struct kwl_server *server,
	struct kwl_object **windows,
	unsigned count)
{
	struct kwl_compose *compose;
	char list[KWL_RENDER_LOG];
	const char *separator;
	size_t length;
	unsigned slot;
	unsigned index;
	int written;
	int same;

	/* Each output's list. */
	compose = server->compose;
	for (slot = 0U; slot < KWL_PLANE_SLOTS; slot++) {
		list[0] = '\0';
		length = 0U;
		for (index = 0U; index < count; index++) {
			if (windows[index]->output != slot)
				continue;
			separator = ",";
			if (length == 0U)
				separator = "";
			written = snprintf(list + length, sizeof(list) - length, "%s%u", separator, windows[index]->id);
			if (written < 0 || (size_t)written >= sizeof(list) - length)
				break;
			length += (size_t)written;
		}

		/* Logged when it changed. */
		same = strcmp(list, compose->render_lists[slot]);
		if (same == 0)
			continue;
		memcpy(compose->render_lists[slot], list, sizeof(list));
		printf("KWL RENDER output=%u surfaces=%s\n", slot, list);
	}
}

/* Draws the cursor in the pass being recorded (the anchor's, or a head's, ws113-p007). */
void
kwl_compose_cursor(
	struct kwl_server *server,
	VkCommandBuffer command)
{
	/* The same cursor; the pass's part of the plane places it. */
	compose_cursor(server, command);
}

/*
 * Returns the image window mode samples for a surface: its GPU buffer's
 * import, or its copy of a wl_shm image; NULL when there is none yet.
 */
static const struct kwl_import *
surface_image(
	const struct kwl_object *surface)
{
	/* A wl_shm buffer is drawn from the surface's copy. */
	if (surface->current == NULL)
		return NULL;
	if (surface->current->shm != NULL)
		return surface->shm_image;

	/* A GPU buffer from its own import. */
	return surface->current->import;
}

/*
 * Gives out a descriptor set of the image layout: one an image gave back, or
 * a new one from the pool.
 */
VkResult
kwl_compose_set_get(
	struct kwl_compose *compose,
	VkDescriptorSet *result)
{
	VkDescriptorSetAllocateInfo set;
	VkResult status;

	/* A spare set is written again by its new owner. */
	if (compose->spare_count != 0U) {
		compose->spare_count--;
		*result = compose->spare_sets[compose->spare_count];
		return VK_SUCCESS;
	}

	/* Otherwise a new one. */
	memset(&set, 0, sizeof(set));
	set.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
	set.descriptorPool = compose->descriptors;
	set.descriptorSetCount = 1U;
	set.pSetLayouts = &compose->set_layout;
	status = vkAllocateDescriptorSets(compose->device, &set, result);
	return status;
}

/*
 * Takes back a descriptor set an image no longer uses, for the next image:
 * sets are not freed one by one (the native i915 executor has no
 * vkFreeDescriptorSets); the pool frees them all at the end.
 */
void
kwl_compose_set_put(
	struct kwl_compose *compose,
	VkDescriptorSet set)
{
	/* Nothing to keep, or no room (the pool frees it at the end). */
	if (set == VK_NULL_HANDLE || compose->spare_count == KWL_DESCRIPTOR_MAX)
		return;

	/* Kept for the next image. */
	compose->spare_sets[compose->spare_count] = set;
	compose->spare_count++;
}

/*
 * Gives an image a second descriptor set with the linear sampler, for
 * drawing it smaller than its size (Wiseview).  The set is freed with the
 * image's own.
 */
VkResult
kwl_compose_linear_set(
	struct kwl_compose *compose,
	struct kwl_import *import)
{
	VkDescriptorImageInfo image_info;
	VkWriteDescriptorSet write;
	VkResult result;

	/* The set (a spare one when there is one). */
	result = kwl_compose_set_get(compose, &import->linear_set);
	if (result != VK_SUCCESS)
		return result;

	/* The image with the linear sampler. */
	memset(&image_info, 0, sizeof(image_info));
	image_info.sampler = compose->linear_sampler;
	image_info.imageView = import->view;
	image_info.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
	memset(&write, 0, sizeof(write));
	write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	write.dstSet = import->linear_set;
	write.dstBinding = 0U;
	write.descriptorCount = 1U;
	write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	write.pImageInfo = &image_info;
	vkUpdateDescriptorSets(compose->device, 1U, &write, 0U, NULL);

	/* Succeeded. */
	return VK_SUCCESS;
}

/*
 * Returns the image window mode samples for a surface, for the glass look.
 */
const struct kwl_import *
kwl_compose_surface_image(
	const struct kwl_object *surface)
{
	const struct kwl_import *image;

	/* The same image as the plain look's. */
	image = surface_image(surface);
	return image;
}

/*
 * Draws a surface's image as a quad at a place on the output, at the
 * surface's size and showing its viewport's source (viewport.c): a window
 * or a sub-surface in the plain look, a popup (popup.c, subsurface.c).
 */
void
kwl_compose_surface_quad(
	struct kwl_server *server,
	VkCommandBuffer command,
	const struct kwl_object *surface,
	const struct kwl_import *import,
	int32_t x,
	int32_t y)
{
	uint32_t width;
	uint32_t height;
	float uv[4];

	/* The surface's size (the image's own without one). */
	kwl_surface_size(surface, &width, &height);
	if (width == 0U || height == 0U) {
		width = import->width;
		height = import->height;
	}

	/* The part of its buffer shown. */
	kwl_viewport_source(surface, uv);

	/* The quad. */
	compose_quad_part(server, command, import, x, y, width, height, uv);
}

/*
 * Draws a part of an image (uv: left, top, right, bottom as fractions) as
 * a quad at a place and size of the desktop, in the frame being recorded.
 */
void
kwl_compose_image_quad(
	struct kwl_server *server,
	VkCommandBuffer command,
	const struct kwl_import *import,
	int32_t x,
	int32_t y,
	uint32_t width,
	uint32_t height,
	const float *uv)
{
	/* The quad. */
	compose_quad_part(server, command, import, x, y, width, height, uv);
}

/* Draws an image as a quad at a place on the output, its own size, all of it. */
static void
compose_quad(
	struct kwl_server *server,
	VkCommandBuffer command,
	const struct kwl_import *import,
	int32_t x,
	int32_t y)
{
	static const float whole[4] = { 0.0f, 0.0f, 1.0f, 1.0f };

	/* The image's size, all of it. */
	compose_quad_part(server, command, import, x, y, import->width, import->height, whole);
}

/* Draws a part of an image (uv: left, top, right, bottom as fractions) as a quad at a place and size on the output. */
static void
compose_quad_part(
	struct kwl_server *server,
	VkCommandBuffer command,
	const struct kwl_import *import,
	int32_t x,
	int32_t y,
	uint32_t quad_width,
	uint32_t quad_height,
	const float *uv)
{
	float constants[8];
	float width;
	float height;

	/* The output drawn: the anchor, or a head's part of the plane (ws113-p007). */
	width = (float)server->width;
	height = (float)server->height;
	if (server->view_width != 0U) {
		x -= server->view_x;
		y -= server->view_y;
		width = (float)server->view_width;
		height = (float)server->view_height;
	}

	/* The rectangle in normalized device coordinates, and the part of the image. */
	constants[0] = 2.0f * (float)x / width - 1.0f;
	constants[1] = 2.0f * (float)y / height - 1.0f;
	constants[2] = 2.0f * (float)(x + (int32_t)quad_width) / width - 1.0f;
	constants[3] = 2.0f * (float)(y + (int32_t)quad_height) / height - 1.0f;
	memcpy(&constants[4], uv, 4U * sizeof(float));

	/* The pipeline for the window's way of drawing, its image, and the strip. */
	vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, server->compose->pipelines[import->draw]);
	vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS, server->compose->layout, 0U, 1U, &import->set, 0U, NULL);
	vkCmdPushConstants(command, server->compose->layout, VK_SHADER_STAGE_VERTEX_BIT, 0U, sizeof(constants), constants);
	vkCmdDraw(command, KWL_QUAD_VERTICES, 1U, 0U, 0U);
}

/*
 * Works out the part of a swapchain image a frame draws: the damage of
 * this frame and of every frame since the image was last drawn (its buffer
 * age).  Returns 1 with that part, or 0 when the whole image is drawn: a
 * change of unknown extent, an image not drawn lately, or damage over most
 * of the output.
 */
static int
compose_region(
	struct kwl_server *server,
	uint32_t image,
	VkRect2D *region)
{
	struct kwl_compose *compose;
	int32_t box[4];
	uint64_t frame;
	uint64_t last;
	uint64_t index;
	unsigned slot;
	VkResult result;

	/* This frame's number and its damage, kept for the images that miss it. */
	compose = server->compose;
	frame = server->frame + 1U;
	slot = (unsigned)(frame % KWL_DAMAGE_HISTORY);
	compose->history_whole[slot] = 1;
	if (!server->dirty && server->damaged) {
		compose->history_whole[slot] = 0;
		memcpy(compose->history[slot], server->damage, sizeof(compose->history[slot]));
	}

	/* The image is drawn in this frame. */
	last = 0;
	if (image < KWL_SWAPCHAIN_MAX) {
		last = compose->image_frames[image];
		compose->image_frames[image] = frame;
	}

	/* An image never drawn, or not for longer than the history, is drawn whole. */
	if (last == 0U || frame - last >= KWL_DAMAGE_HISTORY)
		return 0;

	/* The damage of each frame it missed and of this one; a whole one makes it whole. */
	memcpy(box, compose->history[slot], sizeof(box));
	for (index = last + 1U; index <= frame; index++) {
		slot = (unsigned)(index % KWL_DAMAGE_HISTORY);
		if (compose->history_whole[slot])
			return 0;
		if (compose->history[slot][0] < box[0])
			box[0] = compose->history[slot][0];
		if (compose->history[slot][1] < box[1])
			box[1] = compose->history[slot][1];
		if (compose->history[slot][2] > box[2])
			box[2] = compose->history[slot][2];
		if (compose->history[slot][3] > box[3])
			box[3] = compose->history[slot][3];
	}

	/* Inside the output. */
	if (box[0] < 0)
		box[0] = 0;
	if (box[1] < 0)
		box[1] = 0;
	if (box[2] > (int32_t)compose->output.width)
		box[2] = (int32_t)compose->output.width;
	if (box[3] > (int32_t)compose->output.height)
		box[3] = (int32_t)compose->output.height;
	if (box[2] <= box[0] || box[3] <= box[1])
		return 0;

	/* Most of the output is drawn whole (the loading pass costs more than it saves). */
	if ((int64_t)(box[2] - box[0]) * (box[3] - box[1]) * 10 > (int64_t)compose->output.width * compose->output.height * 7)
		return 0;

	/* The pass that keeps the image's pixels, made the first time. */
	result = kwl_compose_load_pass(compose);
	if (result != VK_SUCCESS)
		return 0;

	/* Succeeded: the part, logged when frames are. */
	region->offset.x = box[0];
	region->offset.y = box[1];
	region->extent.width = (uint32_t)(box[2] - box[0]);
	region->extent.height = (uint32_t)(box[3] - box[1]);
	if (server->log_frames)
		printf("KWL DAMAGE frame=%llu image=%u x=%d y=%d width=%u height=%u\n", (unsigned long long)frame, image, box[0], box[1], region->extent.width, region->extent.height);
	return 1;
}

/* Records a frame: the background, then each window from the bottom. */
static VkResult
compose_record(
	struct kwl_server *server,
	uint32_t image,
	struct kwl_object **windows,
	unsigned count,
	const VkRect2D *region)
{
	struct kwl_compose *compose;
	VkCommandBufferBeginInfo begin;
	VkRenderPassBeginInfo pass;
	VkClearValue clear;
	VkViewport viewport;
	VkRect2D scissor;
	VkDeviceSize offset;
	unsigned index;
	VkResult result;

	/*
	 * A fresh recording: beginning the command buffer resets it (its pool
	 * has VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT), without a call
	 * of its own (a round trip less under Venus, ws099-p002).
	 */
	compose = server->compose;
	memset(&begin, 0, sizeof(begin));
	begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	result = vkBeginCommandBuffer(compose->command, &begin);
	if (result != VK_SUCCESS)
		return result;

	/* The clients' images imported since the last frame go to the layout they are sampled in (ws099-p016). */
	kwl_import_layouts_record(compose, compose->command);

	/* The anchor's pass: its own part of the plane (ws113-p007). */
	server->view_output = KWL_PLANE_ANCHOR;
	server->view_x = 0;
	server->view_y = 0;
	server->view_width = 0U;
	server->view_height = 0U;

	/* The pass clears the image to the background. */
	memset(&clear, 0, sizeof(clear));
	clear.color.float32[0] = KWL_BACKGROUND_RED;
	clear.color.float32[1] = KWL_BACKGROUND_GREEN;
	clear.color.float32[2] = KWL_BACKGROUND_BLUE;
	clear.color.float32[3] = 1.0f;
	memset(&pass, 0, sizeof(pass));
	pass.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
	pass.renderPass = compose->pass;
	if (region != NULL)
		pass.renderPass = compose->pass_load;
	pass.framebuffer = compose->framebuffers[image];
	compose->framebuffer_now = compose->framebuffers[image];
	compose->backdrop_set = VK_NULL_HANDLE;
	pass.renderArea.extent.width = compose->output.width;
	pass.renderArea.extent.height = compose->output.height;
	pass.clearValueCount = 1U;
	pass.pClearValues = &clear;
	vkCmdBeginRenderPass(compose->command, &pass, VK_SUBPASS_CONTENTS_INLINE);

	/* The whole output is placed; the drawing is kept to the damage when there is one (design D4). */
	memset(&viewport, 0, sizeof(viewport));
	viewport.width = (float)compose->output.width;
	viewport.height = (float)compose->output.height;
	viewport.maxDepth = 1.0f;
	vkCmdSetViewport(compose->command, 0U, 1U, &viewport);
	memset(&scissor, 0, sizeof(scissor));
	scissor.extent.width = compose->output.width;
	scissor.extent.height = compose->output.height;
	if (region != NULL)
		scissor = *region;
	compose->scissor_now = scissor;
	vkCmdSetScissor(compose->command, 0U, 1U, &scissor);

	/* Every quad's corners. */
	offset = 0U;
	vkCmdBindVertexBuffers(compose->command, 0U, 1U, &compose->corners, &offset);

	/*
	 * The windows, bottom to top (painter's order): plainly, or in the glass
	 * look with the wallpaper, their title bars and the system bar.  Then the
	 * cursor over them.
	 */
	if (server->glass) {
		kwl_glass_draw(server, compose->command, windows, count);
	} else {
		/* The desktop's icons under the windows (desktop.c). */
		kwl_desktop_draw(server, compose->command);

		/* Each window. */
		for (index = 0; index < count; index++) {
			/* A window between its sub-surfaces below and above it (subsurface.c). */
			kwl_subsurface_draw(server, compose->command, windows[index], (float)windows[index]->x, (float)windows[index]->y, 1.0f, 1.0f, 0U);
			kwl_compose_surface_quad(server, compose->command, windows[index], surface_image(windows[index]), windows[index]->x, windows[index]->y);
			kwl_subsurface_draw(server, compose->command, windows[index], (float)windows[index]->x, (float)windows[index]->y, 1.0f, 1.0f, 1U);
		}

		/* The popups over the windows. */
		kwl_popup_draw(server, compose->command);
	}

	/* The input method's candidate window over the windows and their popups, in both looks (input-method.c). */
	kwl_ime_popup_draw(server, compose->command);

	/* The cursor over everything. */
	compose_cursor(server, compose->command);
	vkCmdEndRenderPass(compose->command);

	/* A test image's capture copies the finished image (shot.c; nothing elsewhere, ws173-p002). */
	kwl_shot_record(server, compose->command, compose->output.images[image]);

	/* The heads' pictures: the finished image copied (mirror), or the wallpaper (extended, ws113-p004b). */
	kwl_heads_record(server, compose->command, compose->output.images[image]);

	/* The recording is complete. */
	return vkEndCommandBuffer(compose->command);
}

/*
 * Draws the cursor at the pointer: the client's cursor surface (its hotspot
 * at the pointer), or the compositor's arrow (its tip), with alpha; nothing when
 * hidden (design D8).
 */
static void
compose_cursor(
	struct kwl_server *server,
	VkCommandBuffer command)
{
	const struct kwl_import *image;
	struct kwl_object *surface;
	struct kwl_import alpha;
	int32_t hotspot_x;
	int32_t hotspot_y;
	int shown;

	/*
	 * A drag and drop's icon under the cursor (its surface's corner at the
	 * pointer moved by its attach offsets, ws189-p002), or the compositor's
	 * badge in the glass look (data.c); in the glass look the mark of what
	 * the drop would do over it (shell.c).
	 */
	if (server->dnd_active) {
		surface = server->dnd_icon;
		image = NULL;
		if (surface != NULL && !surface->dead)
			image = surface_image(surface);
		if (image != NULL) {
			alpha = *image;
			alpha.draw = KWL_DRAW_ALPHA;
			compose_quad(server, command, &alpha, server->pointer_x + surface->offset_x, server->pointer_y + surface->offset_y);
		} else if (server->glass) {
			kwl_glass_draw_drag_badge(server, command);
		}

		/* The mark over it. */
		if (server->glass)
			kwl_glass_draw_drag_mark(server, command, server->dnd_state);
	}

	/* A cursor that has not moved since the start is not drawn (ws035-p116). */
	if (server->pointer_unmoved)
		return;

	/* A client's cursor only over its own window (BUG-118): elsewhere the frame's arrow or the compositor's. */
	shown = kwl_cursor_client_shown(server);
	if (!shown) {
		if (server->frame_edges != 0U) {
			image = kwl_cursor_image(server, &hotspot_x, &hotspot_y);
			if (image != NULL) {
				compose_quad(server, command, image, server->pointer_x - hotspot_x, server->pointer_y - hotspot_y);
				return;
			}
		}

		/* The arrow, its tip at the pointer. */
		if (server->arrow != NULL)
			compose_quad(server, command, server->arrow, server->pointer_x, server->pointer_y);
		return;
	}

	/* A hidden cursor is not drawn. */
	if (server->cursor_hidden)
		return;

	/* A window frame's resize arrow, over the client's own cursor (shell.c, cursor.c). */
	if (server->frame_edges != 0U) {
		image = kwl_cursor_image(server, &hotspot_x, &hotspot_y);
		if (image != NULL) {
			compose_quad(server, command, image, server->pointer_x - hotspot_x, server->pointer_y - hotspot_y);
			return;
		}
	}

	/* The client's surface, when it has an image. */
	surface = server->cursor_surface;
	if (surface != NULL && !surface->dead) {
		image = surface_image(surface);
		if (image == NULL)
			return;
		alpha = *image;
		alpha.draw = KWL_DRAW_ALPHA;
		compose_quad(server, command, &alpha, server->pointer_x - server->cursor_hotspot_x, server->pointer_y - server->cursor_hotspot_y);
		return;
	}

	/* A shape the client asked for, its hotspot at the pointer (cursor.c, ws035-p080). */
	image = kwl_cursor_image(server, &hotspot_x, &hotspot_y);
	if (image != NULL) {
		compose_quad(server, command, image, server->pointer_x - hotspot_x, server->pointer_y - hotspot_y);
		return;
	}

	/* Otherwise the arrow, its tip at the pointer. */
	if (server->arrow != NULL)
		compose_quad(server, command, server->arrow, server->pointer_x, server->pointer_y);
}

/* Submits the frame, presents it, and exports its fence as the fd the event loop polls. */
static VkResult
compose_submit(
	struct kwl_server *server,
	uint32_t image)
{
	struct kwl_compose *compose;
	VkSemaphore waits[1U + KWL_HEADS];
	VkPipelineStageFlags stages[1U + KWL_HEADS];
	VkSemaphore signals[1U + KWL_HEADS];
	VkSubmitInfo submit;
	VkPresentInfoKHR present;
	VkFenceGetFdInfoKHR fd_info;
	unsigned wait_count;
	unsigned signal_count;
	VkResult result;
	int fd;

	/*
	 * The commands wait for the acquired images (the output's, then the
	 * heads', ws113-p004b) and signal their present semaphores.
	 */
	compose = server->compose;
	waits[0] = compose->acquired;
	stages[0] = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
	wait_count = 1U + kwl_heads_waits(server, waits + 1, stages + 1, KWL_HEADS);
	signals[0] = compose->rendered[image];
	signal_count = 1U + kwl_heads_signals(server, signals + 1, KWL_HEADS);
	memset(&submit, 0, sizeof(submit));
	submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	submit.waitSemaphoreCount = wait_count;
	submit.pWaitSemaphores = waits;
	submit.pWaitDstStageMask = stages;
	submit.commandBufferCount = 1U;
	submit.pCommandBuffers = &compose->command;
	submit.signalSemaphoreCount = signal_count;
	submit.pSignalSemaphores = signals;
	result = vkQueueSubmit(compose->queue, 1U, &submit, compose->fence);
	if (result != VK_SUCCESS) {
		kwl_heads_frame_skipped(server);
		return result;
	}

	/* The image goes to the display after the commands. */
	memset(&present, 0, sizeof(present));
	present.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
	present.waitSemaphoreCount = 1U;
	present.pWaitSemaphores = &compose->rendered[image];
	present.swapchainCount = 1U;
	present.pSwapchains = &compose->output.swapchain;
	present.pImageIndices = &image;
	result = vkQueuePresentKHR(compose->queue, &present);

	/*
	 * A move's first frame that the display refused (BUG-266: the display
	 * could not be lit): the frame still completes, and the output goes
	 * back to the display it left (output-switch.c) instead of the
	 * compositor ending.
	 */
	if (compose->switch_proving &&
	    (result == VK_ERROR_SURFACE_LOST_KHR ||
	     result == VK_ERROR_OUT_OF_DATE_KHR ||
	     result == VK_ERROR_DEVICE_LOST)) {
		printf("KWL OUTPUT switch unshown name=%s result=%d\n", compose->display_name, (int)result);
		compose->switch_proving = 0U;
		compose->switch_failed = 1U;
		if (!compose->output_lost)
			compose->output_lost = 1U;
		result = VK_SUCCESS;
	}

	/* A move's first frame presented: the move holds, and a result held back for it is answered. */
	if (compose->switch_proving && (result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR)) {
		compose->switch_proving = 0U;
		kwl_displays_move_settled(server, 0);
	}

	/* The display went: the frame still completes, and the output moves (ws113-p004a). */
	if (result == VK_ERROR_SURFACE_LOST_KHR || result == VK_ERROR_OUT_OF_DATE_KHR) {
		printf("KWL OUTPUT lost operation=present result=%d\n", (int)result);
		if (!compose->output_lost)
			compose->output_lost = 1U;
		result = VK_SUCCESS;
	}

	/* The heads' images to their displays (ws113-p004b). */
	kwl_heads_present(server);

	/* Any other failure is the frame's. */
	if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR)
		return result;

	/* Without an exported fence the event loop asks the fence's status each pass. */
	if (!compose->fence_fd) {
		compose->in_flight = 1;
		return VK_SUCCESS;
	}

	/* The fence as an fd: the event loop learns of completion without waiting (design D3). */
	memset(&fd_info, 0, sizeof(fd_info));
	fd_info.sType = VK_STRUCTURE_TYPE_FENCE_GET_FD_INFO_KHR;
	fd_info.fence = compose->fence;
	fd_info.handleType = kl_backend_gpu_frame_fence_type();
	fd = -1;
	result = compose->get_fence_fd(compose->device, &fd_info, &fd);
	if (result != VK_SUCCESS)
		return result;

	/* Succeeded: the frame is in flight. */
	server->frame_fd = fd;
	compose->in_flight = 1;
	return VK_SUCCESS;
}

/*
 * Holds the buffers a frame sampled, and takes the frame callbacks of the
 * surfaces it showed, until the frame completes.
 */
static void
compose_hold(
	struct kwl_server *server,
	struct kwl_object **windows,
	unsigned count)
{
	struct kwl_compose *compose;
	struct kwl_object *surface;
	struct kwl_object **tail;
	unsigned index;

	/* Each window's current buffer is held by the frame. */
	compose = server->compose;
	for (index = 0; index < count; index++) {
		kwl_buffer_get(windows[index]->current);
		compose->held[compose->held_count++] = windows[index]->current;
		windows[index]->fresh = 0;

		/* A window with frame callbacks is awaited after the frame (frame pacing). */
		if (windows[index]->committed_callbacks != NULL && !windows[index]->awaited) {
			windows[index]->awaited = 1;
			server->awaiting++;
		}

		/* Its frame callbacks wait for the frame, in request order. */
		tail = &compose->callbacks;
		while (*tail != NULL)
			tail = &(*tail)->callback_next;
		*tail = windows[index]->committed_callbacks;
		windows[index]->committed_callbacks = NULL;
	}

	/* A drag's icon surface is held and told too. */
	surface = server->dnd_icon;
	if (server->dnd_active && surface != NULL && !surface->dead && surface->current != NULL) {
		kwl_buffer_get(surface->current);
		compose->held[compose->held_count++] = surface->current;

		/* Its frame callbacks after the windows'. */
		tail = &compose->callbacks;
		while (*tail != NULL)
			tail = &(*tail)->callback_next;
		*tail = surface->committed_callbacks;
		surface->committed_callbacks = NULL;
	}

	/* The desktop surface is held and told too (desktop.c). */
	surface = kwl_desktop_surface(server);
	if (surface != NULL && surface->current != NULL) {
		kwl_buffer_get(surface->current);
		compose->held[compose->held_count++] = surface->current;

		/* Its frame callbacks after the windows'. */
		tail = &compose->callbacks;
		while (*tail != NULL)
			tail = &(*tail)->callback_next;
		*tail = surface->committed_callbacks;
		surface->committed_callbacks = NULL;
	}

	/* A client's cursor surface is held and told too. */
	surface = server->cursor_surface;
	if (surface != NULL && !surface->dead && surface->current != NULL) {
		kwl_buffer_get(surface->current);
		compose->held[compose->held_count++] = surface->current;

		/* Its frame callbacks after the windows'. */
		tail = &compose->callbacks;
		while (*tail != NULL)
			tail = &(*tail)->callback_next;
		*tail = surface->committed_callbacks;
		surface->committed_callbacks = NULL;
	}
}
