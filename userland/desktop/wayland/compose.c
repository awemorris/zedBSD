/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Window mode: draws the background and every window, bottom to top, into
 * a VK_KHR_display swapchain.
 *
 * A frame is one command buffer, recorded from the display list the
 * drawing writes (draw-list.c). Scene changes render the windows and
 * optionally capture cursor-free pixels; cursor-only damage can restore
 * those pixels before its LOAD pass. Only one frame is in flight: its fence is exported as an fd
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

/*
 * The parts of a frame's images it draws: the scene's (the
 * logical damage of the image drawn into, NULL region for all of it) and,
 * for a turned output, the turn pass's on the swapchain's image (the
 * physical rectangles, none for all of it).  The scene's point into the
 * caller's rectangles, or into scene_area and scene_rects for a turn image.
 */
struct kwl_frame_region {
	const VkRect2D *region;
	const VkRect2D *rects;
	unsigned count;
	VkRect2D scene_area;
	VkRect2D scene_rects[KWL_DAMAGE_RECTS];
	VkRect2D present_area;
	VkRect2D present_rects[KWL_DAMAGE_RECTS];
	unsigned present_count;
};

static VkResult compose_device(struct kwl_compose *compose);
static VkResult compose_display(struct kwl_server *server);
static void compose_rotation_choose(struct kwl_server *server);
static void compose_limits(struct kwl_server *server);
static VkResult compose_refresh(struct kwl_server *server, VkDisplayKHR display, uint32_t width, uint32_t height, uint32_t *refresh);
static VkResult compose_objects(struct kwl_compose *compose);
static VkResult compose_pass(struct kwl_compose *compose);
static VkResult compose_pipeline(struct kwl_compose *compose, enum kwl_draw draw, VkShaderModule vertex, VkShaderModule fragment, VkPipelineLayout layout, const VkPipelineVertexInputStateCreateInfo *custom, VkPipeline *pipeline);
static VkResult compose_panel_pipeline(struct kwl_compose *compose);
static void compose_pipelines_destroy(struct kwl_compose *compose);
static VkResult compose_corners(struct kwl_compose *compose);
static VkResult compose_targets(struct kwl_compose *compose);
static void compose_targets_destroy(struct kwl_compose *compose);
static void compose_turn_open(struct kwl_server *server);
static unsigned compose_windows(struct kwl_server *server, struct kwl_object **windows, unsigned capacity);
static unsigned compose_split(struct kwl_server *server, struct kwl_object **windows, unsigned count);
static void compose_lists_log(struct kwl_server *server, struct kwl_object **windows, unsigned count);
static void compose_quad(struct kwl_server *server, struct kwl_list *command, const struct kwl_import *import, int32_t x, int32_t y);
static void compose_quad_part(struct kwl_server *server, struct kwl_list *command, const struct kwl_import *import, int32_t x, int32_t y, uint32_t quad_width, uint32_t quad_height, const float *uv);
static const struct kwl_import *surface_image(const struct kwl_object *surface);
static void compose_cursor(struct kwl_server *server, struct kwl_list *command);
static VkResult compose_record(struct kwl_server *server, uint32_t image, struct kwl_object **windows, unsigned count, const struct kwl_frame_region *drawn);
static void compose_turn_regions(struct kwl_server *server, struct kwl_frame_region *drawn, const VkRect2D *region, const VkRect2D *rects, unsigned count);
static int compose_region(struct kwl_server *server, uint32_t image, VkRect2D *region, VkRect2D *rects, unsigned *rect_count);
static int compose_turn_region(struct kwl_server *server, VkRect2D *region, VkRect2D *rects, unsigned *rect_count);
static int compose_region_since(struct kwl_server *server, uint64_t frame, uint64_t last, uint32_t image, VkRect2D *region, VkRect2D *rects, unsigned *rect_count);
static void compose_walk_timed(struct kwl_server *server, uint64_t walk, uint64_t replay, unsigned quiet);
static void compose_want(struct kwl_server *server, enum kwl_render_kind kind);
static void compose_stalled(struct kwl_server *server);
static void compose_message(struct kwl_server *server, const struct kwl_render_message *message);
static void compose_completed(struct kwl_server *server, const struct kwl_render_message *message);
static void compose_frame_presented(struct kwl_server *server, const struct kwl_render_outcome *outcome, uint64_t number);
static int compose_may_draw(struct kwl_server *server);
static VkResult compose_op_acquire_output(void *context, uint64_t timeout_ns, uint32_t *image, uint64_t *cycles, uint64_t *at_us);
static VkResult compose_op_acquire_head(void *context, unsigned slot, uint32_t *image);
static void compose_op_run(void *context, const struct kwl_render_job *job, struct kwl_render_outcome *outcome);
static VkResult compose_op_wait(void *context, unsigned slot, uint64_t timeout_ns);
static VkResult compose_op_submit_task(void *context, VkCommandBuffer command, VkFence fence);
static VkResult compose_op_wait_task(void *context, VkFence fence, uint64_t timeout_ns);
static void compose_task_done(struct kwl_server *server, const struct kwl_render_message *message);
static unsigned compose_graves(struct kwl_compose *compose, unsigned all);
static VkResult compose_replay_commands(struct kwl_compose *compose, unsigned slot, struct kwl_render_outcome *outcome);
static void compose_rec_timed(struct kwl_server *server, const struct kwl_render_outcome *outcome);
static void compose_hold(struct kwl_server *server, struct kwl_object **windows, unsigned count);
static void compose_log_draws(struct kwl_server *server);
static void compose_count_passes(struct kwl_server *server);
static void compose_log_passes(struct kwl_server *server);
static void compose_log_kinds(struct kwl_server *server);

/* How far into its way App Home's animation shows in its first frame, in milliseconds (review-2 N4). */
#define COMPOSE_HOME_FIRST_MS	8U

/*
 * How long the main thread waits for the render thread (a want with no
 * ready, a drawing with no completion) before it tells a stall, in
 * milliseconds; how many lines it writes a second apart, and then how far
 * apart the later ones are.
 */
#define COMPOSE_STALL_MS	1000U
#define COMPOSE_STALL_LINES	5U
#define COMPOSE_STALL_LATER_MS	60000U

/*
 * The draws one render job of the PowerVR driver holds (its
 * POWERVR_RENDER_DRAWS): a pass of more goes on in another job that
 * loads the pass's area again.  Only KWL PASSES counts with it.
 */
#define COMPOSE_JOB_DRAWS	512U

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
	uint64_t display_started;
	VkResult result;
	int error;

	/* The state lives as long as the compositor. */
	compose = calloc(1, sizeof(*compose));
	if (compose == NULL)
		return ENOMEM;
	server->compose = compose;

	/* The queue's lock, before any thread can use the queue. */
	error = pthread_mutex_init(&compose->queue_lock, NULL);
	if (error != 0) {
		server->compose = NULL;
		free(compose);
		return error;
	}

	/* The instance, the device and its graphics queue. */
	started = kwl_milliseconds();
	result = compose_device(compose);
	if (result != VK_SUCCESS) {
		printf("KWL VULKAN_ERROR operation=device result=%d\n", (int)result);
		return EIO;
	}

	/* The display's size (unless --width and --height chose one) and refresh, before anything is drawn at that size. */
	display_started = kwl_milliseconds();
	result = compose_display(server);
	if (result != VK_SUCCESS) {
		printf("KWL VULKAN_ERROR operation=display result=%d\n", (int)result);
		return EIO;
	}

	/* How long the display's description took, a part of the device's step. */
	printf("KWL STARTUP step=vulkan-display ms=%llu\n", (unsigned long long)(kwl_milliseconds() - display_started));

	/* What the device can take, against which each client buffer's description is checked. */
	compose_limits(server);

	/* How long the device took (KWL STARTUP). */
	printf("KWL STARTUP step=vulkan-device ms=%llu\n", (unsigned long long)(kwl_milliseconds() - started));
	started = kwl_milliseconds();

	/* The layouts, sampler, pools and synchronization of a frame. */
	result = compose_objects(compose);
	if (result != VK_SUCCESS) {
		printf("KWL VULKAN_ERROR operation=objects result=%d\n", (int)result);
		return EIO;
	}

	/* The start's images (the arrow, the wallpaper, the glyphs) are moved to their layout together. */
	kwl_host_image_batch_begin(compose);

	/* the compositor's arrow cursor. */
	error = kwl_arrow_create(server);
	if (error != 0) {
		printf("KWL VULKAN_ERROR operation=arrow\n");
		return EIO;
	}

	/* How long the objects and the arrow took (KWL STARTUP). */
	printf("KWL STARTUP step=vulkan-objects-arrow ms=%llu\n", (unsigned long long)(kwl_milliseconds() - started));

	/*
	 * The cursor shapes' images, in the same batch (review-3
	 * M-6): none is made while the compositor runs.  A failure leaves the
	 * shape to be made when first asked for; their making is timed alone.
	 */
	started = kwl_milliseconds();
	error = kwl_cursor_images_create(server);
	if (error != 0)
		printf("KWL CURSOR images errno=%d\n", error);
	printf("KWL STARTUP step=cursor-shapes ms=%llu\n", (unsigned long long)(kwl_milliseconds() - started));

	/* The glass look's wallpaper and glyphs; without them the plain look is drawn. */
	if (server->glass) {
		error = kwl_glass_open(server);
		if (error != 0) {
			printf("KWL GLASS unavailable errno=%d\n", error);
			server->glass = 0;
		}
	}

	/* The start's images' layout moves, submitted and waited for once. */
	started = kwl_milliseconds();
	result = kwl_host_image_batch_end(compose);
	if (result != VK_SUCCESS) {
		printf("KWL VULKAN_ERROR operation=setup-layout result=%d\n", (int)result);
		return EIO;
	}

	/* How long the GPU took to move them. */
	printf("KWL STARTUP step=image-layouts ms=%llu\n", (unsigned long long)(kwl_milliseconds() - started));

	/* The render thread, which draws the frames from now on; it needs the threads' wake pipe. */
	if (!server->threads_prepared) {
		printf("KWL FAILED site=render-thread errno=%d\n", EINVAL);
		return EIO;
	}

	/* The thread started. */
	started = kwl_milliseconds();
	error = kwl_compose_render_start(server);
	if (error != 0) {
		printf("KWL FAILED site=render-thread errno=%d\n", error);
		return EIO;
	}

	/* How long the render thread took to start. */
	printf("KWL STARTUP step=render-thread ms=%llu\n", (unsigned long long)(kwl_milliseconds() - started));

	/* The display thread, which changes the output's swapchain from now on (without it the changes run here). */
	kwl_display_start(server);

	/* Succeeded: window mode can open its output. */
	return 0;
}

/*
 * Reads a display's native size, its refresh at that size and its name
 * (empty when it has none), for an output moved to it while the compositor
 * runs.  Returns VK_SUCCESS, or VK_ERROR_SURFACE_LOST_KHR
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
 * (the 2026-10-07 user decision N8; R5: a machine started with
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
 * for its format the first time, without taking the display.
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

	/*
	 * The angle displays.conf keeps for the display (or --rotate), which
	 * decides the desktop's logical size and how the surface is made:
	 * every opening chooses it again, for the display it opens on.
	 */
	compose_rotation_choose(server);

	/* The display plane's surface at the display's mode. */
	started = kwl_milliseconds();
	result = vkdemo_display_open_on(compose->instance, compose->physical, compose->display, compose->mode_width, compose->mode_height, &compose->output);
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
			/*
			 * Nothing of a partly made family is kept, so that a
			 * later opening makes the pass and every pipeline again
			 * rather than drawing with the ones that are missing.
			 */
			printf("KWL VULKAN_ERROR operation=pipelines result=%d\n", (int)result);
			compose_pipelines_destroy(compose);
			vkDestroyRenderPass(compose->device, compose->pass, NULL);
			compose->pass = VK_NULL_HANDLE;
			vkdemo_display_close(compose->instance, compose->device, &compose->output);
			return EIO;
		}

		/* The small solid quads drawn together, when the device can (batch.c). */
		kwl_batch_open(server);
	}

	/* Succeeded: only the swapchain is left, which claims the display. */
	compose->output_prepared = 1;
	printf("KWL STARTUP step=output-pipelines ms=%llu\n", (unsigned long long)(kwl_milliseconds() - started));
	return 0;
}

/*
 * Gives the render pass a frame's target is drawn with: the one
 * that clears it, or, with load, the one that keeps its pixels (made by
 * kwl_compose_load_pass the first time it is needed).  The anchor and the
 * heads draw into their swapchain images with the output's passes while
 * nothing turns the picture.
 */
VkRenderPass
kwl_compose_target_pass(
	const struct kwl_compose *compose,
	unsigned slot,
	unsigned load)
{
	/* The anchor turned by the compositor draws into its turn image, left to be sampled. */
	if (slot == KWL_PLANE_ANCHOR && compose->turn.image != VK_NULL_HANDLE) {
		if (load)
			return compose->turn_load_pass;
		return compose->turn_clear_pass;
	}

	/* The pass that keeps the pixels: a frame drawn in its damage, or taken up again. */
	if (load)
		return compose->pass_load;

	/* The pass that clears the image. */
	return compose->pass;
}

/*
 * Gives the image the anchor's frames are drawn into: the turn
 * image when the compositor turns the output, else the swapchain's image
 * of the frame.
 */
VkImage
kwl_compose_target_image(
	const struct kwl_compose *compose,
	uint32_t image)
{
	/* The turn image, one for every frame. */
	if (compose->turn.image != VK_NULL_HANDLE)
		return compose->turn.image;

	/* Succeeded: the swapchain's image. */
	return compose->output.images[image];
}

/*
 * Gives the layout the image the anchor's frames are drawn into is in
 * outside its passes: the turn image is kept to be sampled, the
 * swapchain's image to be presented.
 */
VkImageLayout
kwl_compose_target_layout(
	const struct kwl_compose *compose)
{
	/* The turn image, sampled by the turn pass. */
	if (compose->turn.image != VK_NULL_HANDLE)
		return VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

	/* Succeeded: the swapchain's image, presented. */
	return VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
}

/*
 * Takes in the output's swapchain the display thread made (in
 * compose->output, display-thread.c), the render thread parked: a view,
 * framebuffer and semaphore for each image, the turn image, the scene,
 * the displays' choice, the cursor plane (its objects made by the display
 * thread with the swapchain, cursor NULL for none) and the layers' pictures.
 * Returns 0, or EIO when the targets could not be made (the swapchain is
 * destroyed and the display given back here: a failure's path).
 */
int
kwl_compose_output_take(
	struct kwl_server *server,
	unsigned scene_supported,
	struct kwl_cursor_thread *cursor,
	VkExtent2D plane_extent,
	uint32_t plane_index)
{
	struct kwl_compose *compose;
	uint64_t started;
	VkResult result;

	/* A view, framebuffer and semaphore for each swapchain image. */
	compose = server->compose;
	started = kwl_milliseconds();
	result = compose_targets(compose);
	if (result != VK_SUCCESS) {
		printf("KWL VULKAN_ERROR operation=targets result=%d\n", (int)result);
		compose_targets_destroy(compose);
		if (cursor != NULL) {
			(void)kwl_cursor_plane_stop(compose, cursor);
			free(cursor);
		}
		vkdemo_display_close(compose->instance, compose->device, &compose->output);
		kwl_os_display_release(server, compose->physical, compose->display);
		compose->output_prepared = 0;
		return EIO;
	}

	/*
	 * The turn image, when the angle chosen is turned by the compositor;
	 * one that cannot be made opens the output unrotated, the
	 * choice kept, and the scene and the layers follow the size drawn at.
	 */
	compose_turn_open(server);

	/* Unsupported or exhausted optional cache storage leaves ordinary rendering available. */
	if (scene_supported) {
		result = kwl_compose_load_pass(compose);
		if (result == VK_SUCCESS) {
			result = kwl_scene_open(
				&compose->scene,
				compose->physical,
				compose->device,
				compose->target_width,
				compose->target_height);
		}

		/* An optional cache failure does not prevent ordinary window composition. */
		if (result != VK_SUCCESS)
			printf("KWL SCENE unavailable result=%d\n", (int)result);
	}

	/* No image has been drawn yet: the first frame of each is drawn whole. */
	printf("KWL STARTUP step=output-targets ms=%llu\n", (unsigned long long)(kwl_milliseconds() - started));
	memset(compose->image_frames, 0, sizeof(compose->image_frames));

	/* The displays' choice (read once), and the heads brought in line at the next look. */
	kwl_heads_config_load(server);
	compose->heads_stale = 1U;

	/* The cursor plane, when the output has one (it borrows the output's claim). */
	if (cursor != NULL)
		kwl_cursor_plane_adopt(server, cursor, plane_extent, plane_index);

	/* The layers' pictures, made with the output (layer-cache.c). */
	kwl_layers_open(server);

	/* Succeeded: window mode owns the display through the swapchain. */
	compose->output_open = 1;
	server->dirty = 1;
	printf("KWL OUTPUT open width=%u height=%u images=%u format=%d\n", server->width, server->height, compose->output.image_count, (int)compose->format);
	return 0;
}

/*
 * Lets go of what the output's frames use, the render thread parked and
 * drained (nothing it submitted still runs, display-thread.c): the images
 * it holds are forgotten, the heads let go, the scene, the backdrop, the
 * turn image and the targets destroyed.  The cursor plane, the heads' and the output's swapchains and
 * surfaces are the display thread's to destroy after.  Nothing is wanted without an output.
 */
void
kwl_compose_output_let_go(
	struct kwl_server *server,
	struct kwl_head_rest *heads,
	unsigned *head_count)
{
	struct kwl_compose *compose;

	/* The images held of the swapchains about to go. */
	compose = server->compose;
	kwl_compose_forget(server, 1U, (1U << KWL_HEADS) - 1U);

	/* The heads go with the output (their swapchains, surfaces and displays in heads, for the display thread). */
	kwl_heads_close_all(server, heads, head_count);

	/*
	 * The scene, the backdrop, the targets.  The layers' pictures and the
	 * glass maps stay: those of another size go when the output opens again
	 * (layer-cache.c) or at their next drawing (glass-map.c), and those of
	 * the same size are not made again (their memory is slow to get on
	 * some devices).
	 */
	kwl_scene_close(&compose->scene, compose->device);
	kwl_backdrop_destroy(compose);
	kwl_turn_close(compose, &compose->turn);
	compose_targets_destroy(compose);

	/* No frame of the output from now on. */
	compose->output_open = 0;
	compose_want(server, KWL_RENDER_NONE);
}

/*
 * Says nothing is to be drawn, for a change of the display (display-thread.c).
 */
void
kwl_compose_want_nothing(
	struct kwl_server *server)
{
	/* Without the thread there is no want. */
	if (server->compose == NULL)
		return;
	compose_want(server, KWL_RENDER_NONE);
}

/*
 * Draws one frame of window mode: the walk that writes its display list,
 * given to the render thread, which records, submits and presents it.
 * The render thread acquires the images first: until it is
 * ready the frame is only wanted, and 0 is returned without drawing; so
 * too while a frame is in flight or the output is closed.
 */
int
kwl_compose_draw(
	struct kwl_server *server)
{
	struct kwl_object *windows[KWL_FRAME_WINDOWS];
	struct kwl_render_job job;
	struct kwl_compose *compose;
	struct kwl_frame_slot *slot;
	const VkRect2D *region_drawn;
	VkRect2D region;
	VkRect2D rects[KWL_DAMAGE_RECTS];
	unsigned rect_count;
	struct kwl_frame_region scene;
	uint64_t now;
	uint32_t image;
	unsigned count;
	unsigned all;
	unsigned missed;
	unsigned popups;
	unsigned subsurfaces;
	unsigned index;
	int partial;
	int moving;
	int allowed;
	unsigned cursor_id;
	VkResult result;

	/* Only with an output not being changed, and while another frame may be drawn now (compose_may_draw). */
	compose = server->compose;
	if (!compose->output_open || compose->output_lost || compose->display_busy)
		return 0;
	allowed = compose_may_draw(server);
	if (!allowed)
		return 0;

	/* The frame is wanted; it is drawn once the render thread holds its images. */
	kwl_compose_want_frame(server);
	if (!compose->ready_valid ||
	    compose->ready.want_kind != KWL_RENDER_FRAME ||
	    compose->ready.number != compose->want_number)
		return 0;

	/* How long the frame pacing waited for the windows since the last frame was shown. */
	if (server->log_frames) {
		printf(
			"KWL LAT pace frame=%llu awaiting=%u waited_ms=%llu wait_ms=%llu\n",
			(unsigned long long)server->frame + 1U,
			server->awaiting,
			(unsigned long long)(kwl_milliseconds() - server->frame_done_ms),
			(unsigned long long)server->frame_wait_ms);
	}

	/* The frame shows the pointer where it is now. */
	server->pointer_moved = 0U;

	/*
	 * App Home's animation starts with its first frame's recording, so that
	 * the time before it (a frame or a picture in flight) is not taken from
	 * its way; the first frame shows it a little on, never at its start,
	 * where Home is not drawn (review-2 N4).
	 */
	if (server->home_start_pending) {
		server->home_start_pending = 0U;
		server->home_start_ms = kwl_milliseconds() - COMPOSE_HOME_FIRST_MS;
	}

	/* The windows to draw, bottom to top. */
	compose->frame_start_cycles = kwl_cycles();
	compose->frame_start_ms = kwl_milliseconds();
	all = compose_windows(server, windows, KWL_FRAME_WINDOWS);

	/* The anchor's windows first, then the heads' (heads.c draws them on theirs). */
	count = compose_split(server, windows, all);
	compose->frame_heads = windows + count;
	compose->frame_head_count = all - count;

	/* The popups follow the windows in the list the frame holds (popup.c draws them). */
	popups = kwl_popup_collect(server, windows + all, KWL_FRAME_WINDOWS - all);

	/* The sub-surfaces follow them, held and told like them (subsurface.c draws them with their parents). */
	subsurfaces = kwl_subsurface_collect(server, windows + all + popups, KWL_FRAME_WINDOWS - all - popups);

	/* The frame's start, when the per-frame lines were asked for (the parts of a frame). */
	if (server->log_frames) {
		/* Identify a client cursor surface and whole-output invalidation in this frame. */
		cursor_id = 0U;
		if (server->cursor_surface != NULL)
			cursor_id = server->cursor_surface->id;

		/* One frame line carries the latest coalesced input instead of flooding the file. */
		printf(
			"KWL LAT draw frame=%llu x=%d y=%d input_us=%llu dirty=%u damage=%u cursor=%u opacity=%.3f at_us=%llu\n",
			(unsigned long long)server->frame + 1U,
			server->pointer_x,
			server->pointer_y,
			(unsigned long long)server->pointer_accepted_us,
			(unsigned)server->dirty,
			(unsigned)server->damaged,
			cursor_id,
			(double)server->window_opacity,
			(unsigned long long)kwl_log_us());
		server->pointer_accepted_us = 0U;
	}

	/* The output's image the render thread holds, and the heads' it holds; one that is not ready asks for another frame. */
	image = compose->ready.held.output_image;
	missed = kwl_heads_take(server, &compose->ready.held);

	/* The part of the image to draw: all of it, or the damage it has missed (its buffer age), as rectangles apart. */
	partial = compose_region(server, image, &region, rects, &rect_count);
	region_drawn = NULL;
	if (partial)
		region_drawn = &region;

	/*
	 * A turned output draws the scene into its turn image, over the damage
	 * that image missed, and the swapchain's image's own damage is
	 * the turn pass's, turned to the physical output.
	 */
	memset(&scene, 0, sizeof(scene));
	scene.region = region_drawn;
	scene.rects = rects;
	scene.count = rect_count;
	if (compose->turn.image != VK_NULL_HANDLE)
		compose_turn_regions(server, &scene, region_drawn, rects, rect_count);

	/* The frame's display list. */
	server->home_progress_frozen = 0U;
	server->home_progress_value = kwl_home_progress(server);
	server->home_progress_frozen = 1U;
	result = compose_record(server, image, windows, count, &scene);
	server->home_progress_frozen = 0U;

	/* A list that could not grow is not given (the compositor's failure); the turn image is drawn whole next. */
	if (result == VK_SUCCESS && compose->list.failed)
		result = VK_ERROR_OUT_OF_HOST_MEMORY;
	if (result != VK_SUCCESS) {
		compose->drawing_open = 0U;
		compose->turn.frame = 0U;
		compose->scene.valid = 0U;
		printf("KWL VULKAN_ERROR operation=record result=%d\n", (int)result);
		kwl_heads_frame_skipped(server);
		return EIO;
	}

	/* What the frame asks of the GPU, for its lines (KWL PASSES). */
	compose_count_passes(server);

	/* What the frame's completion applies, in its slot. */
	slot = &compose->slots[compose->walk_slot];
	slot->bake = 0U;
	slot->layers_generation = compose->frame_layers_generation;
	slot->scene_written = compose->scene.written;
	compose->scene.written = 0U;
	slot->copies = 0U;
	if (compose->list.copy_count != 0U)
		slot->copies = 1U;
	slot->number = server->frame + 1U;
	slot->start_cycles = compose->frame_start_cycles;
	slot->start_ms = compose->frame_start_ms;
	slot->flying = 1U;

	/* The job: the output's image and every head's image held, all presented. */
	memset(&job, 0, sizeof(job));
	job.number = compose->ready.number;
	job.kind = KWL_RENDER_FRAME;
	job.slot = compose->walk_slot;
	job.output_image = image;
	for (index = 0U; index < KWL_HEADS; index++) {
		if (!compose->heads[index].in_frame)
			continue;
		job.heads[index] = 1U;
		job.head_images[index] = compose->heads[index].image;
	}

	/* The list goes to the render thread after the moves recorded before it: the frame is in flight until it completes; the want went down. */
	compose->walk_cycles = kwl_cycles() - compose->walk_start_cycles;
	compose->in_flight++;
	compose->drawings_given++;
	compose->drawing_open = 0U;
	compose->flight_ms = kwl_milliseconds();
	compose->ready_valid = 0U;
	compose->want_kind = KWL_RENDER_NONE;
	kwl_compose_task_flush(compose);
	kwl_render_answer(&compose->render, job.number, KWL_RENDER_ANSWER_LIST, &job);

	/* The new images' layouts went with the frame. */
	kwl_import_layouts_done(compose);

	/*
	 * While the compositor's own animation moves, the next frame is
	 * wanted at once: the render thread acquires its image as soon as this
	 * frame completes, instead of after the main thread has heard of the
	 * completion and asked again (two wakeups of each thread less).
	 */
	moving = kwl_compositor_moving(server);
	if (moving)
		kwl_compose_want_frame(server);

	/* The CPU time of the main thread's part of the frame. */
	server->perf.compose_draw_cycles += kwl_cycles() - compose->frame_start_cycles;

	/* The frame holds what it sampled until it completes, the popups and sub-surfaces too. */
	compose_hold(server, windows, all + popups + subsurfaces);
	compose->frame_heads = NULL;
	compose->frame_head_count = 0U;
	server->dirty = 0;
	if (missed > 0U)
		server->dirty = 1;
	server->damaged = 0;
	compose->scene.changed = 0U;
	server->frame++;
	compose->frame_number = server->frame;

	/* The frame's lines, while its counts are this frame's (--log-frames). */
	if (server->log_frames) {
		compose_log_draws(server);
		compose_log_passes(server);
		compose_log_kinds(server);
	}

	/* The next drawing goes into the next slot. */
	compose->walk_slot = (compose->walk_slot + 1U) % KWL_FRAME_SLOTS;

	/* The first frame after App Home or Wiseview was asked to open or close, logged once (C5, also without --log-frames). */
	if (server->transition != NULL) {
		now = kwl_milliseconds();
		printf("KWL FIRST_FRAME what=%s frame=%llu request_ms=%llu ms=%llu\n", server->transition, (unsigned long long)server->frame,
		       (unsigned long long)server->transition_ms, (unsigned long long)(now - server->transition_ms));
		server->transition = NULL;
	}

	/* The frame given. */
	if (server->log_frames)
		printf("KWL COMPOSE frame=%llu image=%u windows=%u at_ms=%llu\n", (unsigned long long)server->frame, image, count, (unsigned long long)kwl_milliseconds());

	/* Succeeded: one frame is in flight. */
	return 0;
}

/*
 * Waits for the frame (or the layer's picture) in flight to complete and
 * applies its completion: the render thread waits for its
 * fence; the main thread then releases the buffers it sampled and sends
 * the frame callbacks.  Returns 0, or EIO when the frame failed.
 */
int
kwl_compose_complete(
	struct kwl_server *server)
{
	struct kwl_compose *compose;

	/* The render thread's messages until nothing is in flight. */
	compose = server->compose;
	while (compose->in_flight && compose->render_started) {
		(void)kwl_render_wait_message(&compose->render, 1000U);
		kwl_compose_poll(server);
	}

	/* A failed frame ended the compositor. */
	if (server->failed)
		return EIO;

	/* Succeeded: no frame is in flight. */
	return 0;
}

/*
 * Tells whether the event loop must ask a fence's status each pass: never,
 * as the render thread waits for the frames' fences and
 * wakes the loop with their completions.
 */
int
kwl_compose_waiting(
	struct kwl_server *server)
{
	/* The render thread waits. */
	(void)server;
	return 0;
}

/*
 * Applies the render thread's messages in their order: a
 * ready is kept for the next walk (or cancelled when the want changed), a
 * failed acquire is the output's loss or the compositor's failure, and a
 * completion ends the frame in flight.  The event loop calls it each pass,
 * after it emptied its wake pipe.
 */
void
kwl_compose_poll(
	struct kwl_server *server)
{
	struct kwl_render_message *messages;
	struct kwl_render_message *message;
	struct kwl_compose *compose;
	unsigned destroyed;
	unsigned parked;

	/* Nothing runs without the thread. */
	compose = server->compose;
	if (compose == NULL || !compose->render_started)
		return;

	/* A task that could not be given ends the compositor (the images it moves cannot be drawn). */
	if (compose->task_error != VK_SUCCESS && !server->failed) {
		printf("KWL VULKAN_ERROR operation=task result=%d\n", (int)compose->task_error);
		kwl_fatal("KWL FAILED site=compose_task errno=%d operation=give result=%d", EIO, (int)compose->task_error);
		server->failed = 1;
	}

	/* Each message, freed once applied; a park asked without waiting is told after the completions before it. */
	parked = 0U;
	messages = kwl_render_take(&server->compose->render);
	while (messages != NULL) {
		message = messages;
		messages = message->next;
		if (message->kind == KWL_RENDER_PARKED)
			parked = 1U;
		else
			compose_message(server, message);
		free(message);
	}

	/* The change of the display that asked for the park goes on (display-thread.c). */
	if (parked)
		kwl_display_parked(server);

	/* The images let go that nothing given uses any more, told with the frames' lines (--log-frames). */
	destroyed = compose_graves(compose, 0U);
	if (destroyed != 0U && server->log_frames)
		printf("KWL GRAVE destroyed=%u\n", destroyed);

	/* A wait for the thread that lasts is told. */
	compose_stalled(server);
}

/*
 * Gives the render thread the task recorded in this pass, before the
 * event loop waits: the images' moves to their layout run without waiting
 * for the next frame.
 */
void
kwl_compose_give_tasks(
	struct kwl_server *server)
{
	/* Nothing without window mode. */
	if (server->compose == NULL)
		return;

	/* The task being recorded, if any. */
	kwl_compose_task_flush(server->compose);
}

/*
 * Finishes the frame in flight now, waiting for it: before the main thread
 * uses the queue or waits for the device (an output's changes), and at
 * exit.
 */
void
kwl_compose_quiesce(
	struct kwl_server *server)
{
	uint64_t mark;
	int error;

	/* Without window mode, or without a frame in flight, nothing waits. */
	if (server->compose == NULL || !server->compose->in_flight)
		return;

	/* The frame completes; a failure is the compositor's.  The wait counts as the main thread's Vulkan outside a walk. */
	mark = kwl_cycles();
	error = kwl_compose_complete(server);
	kwl_perf_vk_outside(server, kwl_cycles() - mark);
	if (error != 0) {
		kwl_fatal("KWL FAILED site=compose_poll errno=%d", error);
		server->failed = 1;
	}
}

/*
 * Lets the drawings in flight forget a client that goes: their holds on
 * its buffers are given back now and its frame callbacks leave them (the
 * client is fatal, so neither is told), so that its objects may go at
 * once.  The images they sample wait for those drawings when they are let
 * go (kwl_compose_bury), so nothing waits here.
 */
void
kwl_compose_client_gone(
	struct kwl_server *server,
	struct kwl_client *client)
{
	struct kwl_frame_slot *slot;
	struct kwl_object **link;
	struct kwl_object *callback;
	struct kwl_object *buffer;
	unsigned index;
	unsigned held;

	/* Without window mode nothing is held. */
	if (server->compose == NULL)
		return;

	/* Each slot's holds and callbacks (those of a completed slot are none). */
	for (index = 0U; index < KWL_FRAME_SLOTS; index++) {
		slot = &server->compose->slots[index];

		/* Its buffers, given back; the completion skips the empty places. */
		for (held = 0U; held < slot->held_count; held++) {
			buffer = slot->held[held];
			if (buffer == NULL || buffer->client != client)
				continue;
			slot->held[held] = NULL;
			kwl_buffer_put(buffer);
		}

		/* Its frame callbacks, out of the list and retired untold. */
		link = &slot->callbacks;
		while (*link != NULL) {
			callback = *link;
			if (callback->client != client) {
				link = &callback->callback_next;
				continue;
			}

			/* Takes the callback out of the list and retires it. */
			*link = callback->callback_next;
			callback->callback_next = NULL;
			kwl_object_destroy(callback);
		}
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

	/* A change of the display running ends, and the display thread stops (the output's close runs here). */
	kwl_display_stop(server);

	/* The frame in flight completes, the tasks recorded are given, the render thread stops after them, then the output goes, then the arrow. */
	(void)kwl_compose_complete(server);
	kwl_compose_task_flush(compose);
	if (compose->render_started) {
		kwl_render_stop(&compose->render);
		kwl_compose_poll(server);
		kwl_render_destroy(&compose->render);
		compose->render_started = 0U;
	}

	/* The device idle, then the images let go, then the output's objects. */
	if (compose->device != VK_NULL_HANDLE)
		kwl_device_wait_idle(compose);
	(void)compose_graves(compose, 1U);
	kwl_compose_output_close(server);
	kwl_layers_close(compose);
	kwl_glass_maps_destroy(compose);
	kwl_arrow_destroy(server);
	kwl_glass_close(server);

	/* The device's objects. */
	if (compose->device != VK_NULL_HANDLE) {
		/* The pass's pipelines, including a partially constructed family. */
		compose_pipelines_destroy(compose);

		/* The family shares its constants, descriptors and sampler. */
		vkDestroyPipelineLayout(compose->device, compose->panel_layout, NULL);
		vkDestroySampler(compose->device, compose->linear_sampler, NULL);

		/* The turn pass's pipeline and passes, when an output ever turned (turn.c). */
		kwl_turn_destroy_shared(compose);

		/* The joined quads' pipeline and buffer, then the corners. */
		kwl_batch_close(compose);
		vkDestroyBuffer(compose->device, compose->corners, NULL);
		vkFreeMemory(compose->device, compose->corners_memory, NULL);

		/*
		 * The passes (the one that keeps the image's pixels is made once
		 * and kept from one output to the next, as the format is),
		 * layouts, sampler, pools and synchronization.
		 */
		vkDestroyRenderPass(compose->device, compose->pass, NULL);
		if (compose->pass_load != VK_NULL_HANDLE)
			vkDestroyRenderPass(compose->device, compose->pass_load, NULL);
		compose->pass_load = VK_NULL_HANDLE;
		vkDestroyPipelineLayout(compose->device, compose->layout, NULL);
		vkDestroyDescriptorSetLayout(compose->device, compose->set_layout, NULL);
		vkDestroySampler(compose->device, compose->sampler, NULL);
		vkDestroyDescriptorPool(compose->device, compose->descriptors, NULL);
		vkDestroySemaphore(compose->device, compose->acquired, NULL);
		for (index = 0U; index < KWL_FRAME_SLOTS; index++)
			vkDestroyFence(compose->device, compose->slots[index].fence, NULL);
		vkDestroyFence(compose->device, compose->hotplug, NULL);
		vkDestroyCommandPool(compose->device, compose->pool, NULL);
		vkDestroyCommandPool(compose->device, compose->task_pool, NULL);
		vkDestroyDevice(compose->device, NULL);
	}

	/* The instance last. */
	if (compose->instance != VK_NULL_HANDLE)
		vkDestroyInstance(compose->instance, NULL);

	/* The display list's memory, then the state. */
	kwl_list_free(&compose->list);
	(void)pthread_mutex_destroy(&compose->queue_lock);
	free(compose);
	server->compose = NULL;
}

/*
 * Starts the render thread, once the device and its objects
 * are made: it waits for a want.
 */
int
kwl_compose_render_start(
	struct kwl_server *server)
{
	struct kwl_render_ops ops;
	struct kwl_compose *compose;
	int error;

	/* Its Vulkan work, done by this file's operations on the compositor's objects. */
	compose = server->compose;
	memset(&ops, 0, sizeof(ops));
	ops.acquire_output = compose_op_acquire_output;
	ops.acquire_head = compose_op_acquire_head;
	ops.run = compose_op_run;
	ops.wait = compose_op_wait;
	ops.now_us = kwl_log_us;
	ops.submit_task = compose_op_submit_task;
	ops.wait_task = compose_op_wait_task;
	error = kwl_render_init(&compose->render, &ops, compose, server->wake[1]);
	if (error != 0)
		return error;

	/* The thread. */
	error = kwl_render_start(&compose->render);
	if (error != 0) {
		kwl_render_destroy(&compose->render);
		return error;
	}

	/* Succeeded: frames are drawn through it. */
	compose->render_started = 1U;
	return 0;
}

/*
 * Says a frame may be drawn (design 4.1): the render thread acquires its
 * images (the output's, and the heads' the frame needs) ahead of the walk.
 * Nothing is wanted while a frame is in flight is fine: the thread looks
 * after the completion.
 */
void
kwl_compose_want_frame(
	struct kwl_server *server)
{
	struct kwl_compose *compose;

	/* Only with an output to draw, and not while the display changes. */
	compose = server->compose;
	if (compose == NULL || !compose->render_started || !compose->output_open || compose->output_lost || compose->display_busy)
		return;

	/* The frame. */
	compose_want(server, KWL_RENDER_FRAME);
}

/*
 * Says a layer's picture may be drawn (layer-cache.c), and tells whether
 * the render thread is ready for it, so that its walk may begin now.
 */
int
kwl_compose_want_layer(
	struct kwl_server *server)
{
	struct kwl_compose *compose;

	/* Only with the thread, and not while the display changes. */
	compose = server->compose;
	if (compose == NULL || !compose->render_started || compose->display_busy)
		return 0;

	/* The picture wanted. */
	compose_want(server, KWL_RENDER_LAYER);

	/* Ready for it, or not yet. */
	if (compose->ready_valid &&
	    compose->ready.want_kind == KWL_RENDER_LAYER &&
	    compose->ready.number == compose->want_number)
		return 1;
	return 0;
}

/*
 * Parks the render thread while the main thread changes what it uses
 * (the output's or a head's swapchain, design 4.8): it parks after the
 * frame in flight, whose completion is applied here.  Parks nest; the
 * thread goes on when the outermost ends.  A thread that does not park in
 * time ends the compositor, as no Vulkan call is safe under it.  A change
 * of the display running ends first (display-thread.c), unless this park
 * is one of its steps'.
 */
void
kwl_compose_park(
	struct kwl_server *server)
{
	struct kwl_compose *compose;
	int error;

	/* Only the outermost park waits. */
	compose = server->compose;
	if (compose == NULL || !compose->render_started)
		return;
	if (compose->display_parking || (compose->display_busy && compose->display_in_step == 0U))
		kwl_display_settle(server);
	compose->park_depth++;
	if (compose->park_depth > 1U)
		return;

	/* The thread's answer, or the end. */
	error = kwl_render_park(&compose->render, 2000U);
	if (error != 0) {
		kwl_fatal("KWL FAILED site=park errno=%d thread=render depth=%u", error, compose->park_depth);
		_exit(1);
	}

	/* The frame completed before the park is applied now. */
	kwl_compose_poll(server);
}

/*
 * Ends a park: the render thread goes on when the outermost ends.
 */
void
kwl_compose_unpark(
	struct kwl_server *server)
{
	struct kwl_compose *compose;

	/* Only the outermost park ends the thread's. */
	compose = server->compose;
	if (compose == NULL || !compose->render_started || compose->park_depth == 0U)
		return;
	compose->park_depth--;
	if (compose->park_depth != 0U)
		return;

	/* The thread steps again. */
	kwl_render_unpark(&compose->render);
}

/*
 * Forgets the images the parked render thread holds of swapchains about
 * to be destroyed (the output's, and the heads' set in heads_mask): a
 * ready held for them is cancelled, and the output's acquisition
 * semaphore, which may be signalled for an image never used, is made
 * again (design 4.2, review-3 M-5).
 */
void
kwl_compose_forget(
	struct kwl_server *server,
	unsigned output,
	unsigned heads_mask)
{
	struct kwl_compose *compose;
	VkSemaphoreCreateInfo semaphore;
	unsigned held_output;
	VkResult result;

	/* Only with the thread parked. */
	compose = server->compose;
	if (compose == NULL || !compose->render_started || compose->park_depth == 0U)
		return;

	/* Whether it held the output's image. */
	pthread_mutex_lock(&compose->render.lock);

	held_output = compose->render.held.output;

	pthread_mutex_unlock(&compose->render.lock);

	/* Forgotten. */
	kwl_render_forget(&compose->render, output, heads_mask);

	/* A ready for the images is answered with a cancel: the thread acquires again after the park. */
	if (compose->ready_valid) {
		kwl_render_answer(&compose->render, compose->ready.number, KWL_RENDER_ANSWER_CANCEL, NULL);
		compose->ready_valid = 0U;
	}

	/* The output's acquisition semaphore made again when an image was acquired with it. */
	if (!output || !held_output)
		return;
	vkDestroySemaphore(compose->device, compose->acquired, NULL);
	compose->acquired = VK_NULL_HANDLE;
	memset(&semaphore, 0, sizeof(semaphore));
	semaphore.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
	result = vkCreateSemaphore(compose->device, &semaphore, NULL, &compose->acquired);
	if (result != VK_SUCCESS) {
		printf("KWL VULKAN_ERROR operation=acquire-semaphore result=%d\n", (int)result);
		kwl_fatal("KWL FAILED site=acquire-semaphore errno=%d result=%d", EIO, (int)result);
		server->failed = 1;
	}
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
	 * imported (import.c).  The external fence pair follows when the
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

	/* The surface counters, which the display control needs, when the library offers them. */
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

	/* The hotplug's entry point. */
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
	uint32_t mode_width;
	uint32_t mode_height;

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
	 * its resolution keeps the size asked for (320x240 by default).
	 */
	mode_width = server->size_width;
	mode_height = server->size_height;
	if (!server->size_given &&
	    resolution.width != 0U &&
	    resolution.height != 0U) {
		mode_width = resolution.width;
		mode_height = resolution.height;
	}

	/* The output's mode until it opens. */
	compose->mode_width = mode_width;
	compose->mode_height = mode_height;

	/*
	 * The displays' choice (read once, nothing else done), whose angle for
	 * this display makes the desktop's logical size before the look, the
	 * scene and the layers are made at it.
	 */
	kwl_heads_config_load(server);
	compose_rotation_choose(server);

	/* What the output shows, which its first opening is compared with: the look and the scene are made at it. */
	kwl_output_state(server, &compose->opened);
	compose->opened_display = display;

	/* The refresh of the mode at the output's size, which wl_output tells the clients. */
	result = compose_refresh(server, display, compose->mode_width, compose->mode_height, &refresh);
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

/*
 * Chooses the angle the output shows on the display it opens on:
 * --rotate when a test gave it, else the angle displays.conf keeps for the
 * display's key (the login screen is never rotated, D9).  An angle the
 * display cannot take opens it unrotated, the file's line kept and the
 * reason logged.  Sets the angles the display may take, how the angle is
 * shown, and the desktop's logical size (the mode turned by the angle).
 */
static void
compose_rotation_choose(
	struct kwl_server *server)
{
	VkPhysicalDeviceProperties properties;
	struct kwl_compose *compose;
	unsigned wanted;
	unsigned scanout;
	unsigned turn;
	unsigned why;
	int error;

	/* The angle asked for: a test's option over the file's line for the display. */
	compose = server->compose;
	wanted = kwl_displays_rotation(&compose->config, compose->display_name);
	if (server->rotate_given)
		wanted = server->rotate_option;

	/* The login screen is shown unrotated, whatever its user's file says. */
	if (server->greeter)
		wanted = KWL_ROTATE_0;

	/* Whether the turn pass is offered on this GPU without a test's policy (not the Pi 4's yet). */
	vkGetPhysicalDeviceProperties(compose->physical, &properties);
	compose->turn_default = 1U;
	if (properties.vendorID == KWL_TURN_VENDOR_OFF)
		compose->turn_default = 0U;

	/* The angles the display may take: none but 0 by default until its hardware's 180 is read; every one under a test's policy. */
	compose->rotations = kwl_rotate_mask(compose->hw180, compose->turn_default, KWL_TURN_BUILD, server->rotate_policy, &why);
	compose->rotate_why = why;

	/* How the angle is shown; one the display cannot take opens it unrotated, the choice still in the file. */
	error = kwl_rotate_plan(wanted, compose->rotations, compose->hw180, &scanout, &turn);
	if (error != 0) {
		printf("KWL OUTPUT rotate unsupported key=%s rotation=%u reason=mask why=%u\n", compose->display_name, wanted, why);
		wanted = KWL_ROTATE_0;
		scanout = KWL_ROTATE_0;
		turn = KWL_ROTATE_0;
	}

	/* The choice, how it is shown, and the desktop's size at it. */
	compose->rotation_selected = wanted;
	compose->scanout_rotation = scanout;
	compose->turn_rotation = turn;
	kwl_output_size_set(server, compose->mode_width, compose->mode_height, wanted);
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
	unsigned index;
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

	/* One command buffer, re-recorded from the display list every frame. */
	memset(&command_pool, 0, sizeof(command_pool));
	command_pool.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
	command_pool.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
	command_pool.queueFamilyIndex = compose->family;
	result = vkCreateCommandPool(compose->device, &command_pool, NULL, &compose->pool);
	if (result != VK_SUCCESS)
		return result;

	/* The main thread's own pool, for the tasks the render thread submits for it (one-time command buffers). */
	command_pool.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
	result = vkCreateCommandPool(compose->device, &command_pool, NULL, &compose->task_pool);
	if (result != VK_SUCCESS)
		return result;
	memset(&command, 0, sizeof(command));
	command.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
	command.commandPool = compose->pool;
	command.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	command.commandBufferCount = 1U;

	/* The frame's fence, exportable as an fd the event loop polls (design D3). */
	memset(&export, 0, sizeof(export));
	export.sType = VK_STRUCTURE_TYPE_EXPORT_FENCE_CREATE_INFO;
	export.handleTypes = kl_backend_gpu_frame_fence_type();

	/* Describes the frame fence with the OS export type when export is available. */
	memset(&fence, 0, sizeof(fence));
	fence.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
	if (compose->fence_fd)
		fence.pNext = &export;

	/* Each slot's drawing and host images' copies (recorded when the list is replayed) and fence. */
	for (index = 0U; index < KWL_FRAME_SLOTS; index++) {
		result = vkAllocateCommandBuffers(compose->device, &command, &compose->slots[index].command);
		if (result != VK_SUCCESS)
			return result;
		result = vkAllocateCommandBuffers(compose->device, &command, &compose->slots[index].copy_command);
		if (result != VK_SUCCESS)
			return result;
		result = vkCreateFence(compose->device, &fence, NULL, &compose->slots[index].fence);
		if (result != VK_SUCCESS)
			return result;
	}

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
	result = compose_pipeline(compose, KWL_DRAW_OPAQUE, vertex, fragment, compose->layout, NULL, &compose->pipelines[KWL_DRAW_OPAQUE]);
	if (result == VK_SUCCESS)
		result = compose_pipeline(compose, KWL_DRAW_ALPHA, vertex, fragment, compose->layout, NULL, &compose->pipelines[KWL_DRAW_ALPHA]);
	vkDestroyShaderModule(compose->device, vertex, NULL);
	vkDestroyShaderModule(compose->device, fragment, NULL);
	if (result != VK_SUCCESS)
		return result;

	/* The glass look's pipeline. */
	result = compose_panel_pipeline(compose);
	return result;
}

/* Creates premultiplied panel pipelines without computing unused shape modes. */
static VkResult
compose_panel_pipeline(
	struct kwl_compose *compose)
{
	VkShaderModuleCreateInfo module;
	VkShaderModule vertex;
	VkShaderModule fragment;
	VkPipeline *pipeline;
	VkResult error;
	uint32_t mode;
	uint32_t inside;

	/* All modes share the same quad and push-constant representation. */
	memset(&module, 0, sizeof(module));
	module.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
	module.codeSize = sizeof(kwl_panel_vert);
	module.pCode = kwl_panel_vert;
	error = vkCreateShaderModule(compose->device, &module, NULL, &vertex);
	if (error != VK_SUCCESS)
		return error;

	/* Builds each exact mode and the original shader for unfamiliar mode values. */
	for (mode = 0; mode <= KWL_PANEL_MODES; mode++) {
		/* Only the fallback retains every branch of the panel shader. */
		if (mode == KWL_PANEL_MODES) {
			module.codeSize = sizeof(kwl_panel_frag);
			module.pCode = kwl_panel_frag;
			pipeline = &compose->panel_pipeline;
		} else {
			module.codeSize = kwl_panel_variants[mode].bytes;
			module.pCode = kwl_panel_variants[mode].words;
			pipeline = &compose->panel_pipelines[mode];
		}

		/* Modules exist only while their pipelines are compiled. */
		error = vkCreateShaderModule(compose->device, &module, NULL, &fragment);
		if (error != VK_SUCCESS) {
			vkDestroyShaderModule(compose->device, vertex, NULL);
			return error;
		}

		/* Every mode preserves the original premultiplied source-over blend. */
		error = compose_pipeline(
			compose,
			KWL_DRAW_ALPHA,
			vertex,
			fragment,
			compose->panel_layout,
			NULL,
			pipeline);

		vkDestroyShaderModule(compose->device, fragment, NULL);
		if (error != VK_SUCCESS) {
			vkDestroyShaderModule(compose->device, vertex, NULL);
			return error;
		}
	}

	/* The insides of shapes (glass.c) from their own variants, blended and, for opaque ones, written as they are. */
	for (inside = 0; inside < KWL_PANEL_INSIDES; inside++) {
		/* The variant's module, only while its pipelines are compiled. */
		module.codeSize = kwl_panel_inside_variants[inside].bytes;
		module.pCode = kwl_panel_inside_variants[inside].words;
		error = vkCreateShaderModule(compose->device, &module, NULL, &fragment);
		if (error != VK_SUCCESS) {
			vkDestroyShaderModule(compose->device, vertex, NULL);
			return error;
		}

		/* The premultiplied source-over blend, as the whole shape's. */
		error = compose_pipeline(
			compose,
			KWL_DRAW_ALPHA,
			vertex,
			fragment,
			compose->panel_layout,
			NULL,
			&compose->panel_inside_pipelines[inside]);

		vkDestroyShaderModule(compose->device, fragment, NULL);
		if (error != VK_SUCCESS) {
			vkDestroyShaderModule(compose->device, vertex, NULL);
			return error;
		}

		/* An opaque inside at full opacity, from its own variant, written as it is. */
		module.codeSize = kwl_panel_opaque_variants[inside].bytes;
		module.pCode = kwl_panel_opaque_variants[inside].words;
		error = vkCreateShaderModule(compose->device, &module, NULL, &fragment);
		if (error != VK_SUCCESS) {
			vkDestroyShaderModule(compose->device, vertex, NULL);
			return error;
		}
		error = compose_pipeline(
			compose,
			KWL_DRAW_OPAQUE,
			vertex,
			fragment,
			compose->panel_layout,
			NULL,
			&compose->panel_opaque_pipelines[inside]);
		vkDestroyShaderModule(compose->device, fragment, NULL);
		if (error != VK_SUCCESS) {
			vkDestroyShaderModule(compose->device, vertex, NULL);
			return error;
		}
	}

	/* The compositor owns pipelines until its normal or failed-open cleanup. */
	vkDestroyShaderModule(compose->device, vertex, NULL);
	return VK_SUCCESS;
}

/* Destroys the pass's opaque, alpha and glass pipelines that exist, leaving none. */
static void
compose_pipelines_destroy(
	struct kwl_compose *compose)
{
	unsigned index;

	/* The opaque and the alpha pipelines. */
	for (index = 0; index < 2U; index++) {
		if (compose->pipelines[index] != VK_NULL_HANDLE)
			vkDestroyPipeline(compose->device, compose->pipelines[index], NULL);
		compose->pipelines[index] = VK_NULL_HANDLE;
	}

	/* The glass look's fallback pipeline. */
	if (compose->panel_pipeline != VK_NULL_HANDLE)
		vkDestroyPipeline(compose->device, compose->panel_pipeline, NULL);
	compose->panel_pipeline = VK_NULL_HANDLE;

	/* Every mode's pipeline, of a whole or a partially constructed family. */
	for (index = 0; index < KWL_PANEL_MODES; index++) {
		if (compose->panel_pipelines[index] != VK_NULL_HANDLE)
			vkDestroyPipeline(compose->device, compose->panel_pipelines[index], NULL);
		compose->panel_pipelines[index] = VK_NULL_HANDLE;
	}

	/* Every inside's blended and opaque pipelines. */
	for (index = 0; index < KWL_PANEL_INSIDES; index++) {
		if (compose->panel_inside_pipelines[index] != VK_NULL_HANDLE)
			vkDestroyPipeline(compose->device, compose->panel_inside_pipelines[index], NULL);
		compose->panel_inside_pipelines[index] = VK_NULL_HANDLE;
		if (compose->panel_opaque_pipelines[index] != VK_NULL_HANDLE)
			vkDestroyPipeline(compose->device, compose->panel_opaque_pipelines[index], NULL);
		compose->panel_opaque_pipelines[index] = VK_NULL_HANDLE;
	}
}

/*
 * Creates a pipeline of the glass look's blending (premultiplied source
 * over) with its own vertex input (the solid quads drawn together,
 * batch.c), in the output's pass and the given layout.
 */
VkResult
kwl_compose_pipeline_input(
	struct kwl_compose *compose,
	VkShaderModule vertex,
	VkShaderModule fragment,
	VkPipelineLayout layout,
	const VkPipelineVertexInputStateCreateInfo *input,
	VkPipeline *result)
{
	/* The quads' pipeline with the vertices it is given. */
	return compose_pipeline(compose, KWL_DRAW_ALPHA, vertex, fragment, layout, input, result);
}

/*
 * Creates a pipeline that copies a quad without blending (the turn pass,
 * turn.c), in the output's pass and the quads' layout.
 */
VkResult
kwl_compose_pipeline_opaque(
	struct kwl_compose *compose,
	VkShaderModule vertex,
	VkShaderModule fragment,
	VkPipeline *pipeline)
{
	VkResult result;

	/* The quads' corners, replacing what is under them. */
	result = compose_pipeline(compose, KWL_DRAW_OPAQUE, vertex, fragment, compose->layout, NULL, pipeline);
	if (result != VK_SUCCESS)
		return result;

	/* Succeeded: the pipeline. */
	return VK_SUCCESS;
}

/*
 * Creates a pipeline that draws a quad the given way, with the given
 * layout; with custom, the vertices it describes instead of the corners.
 */
static VkResult
compose_pipeline(
	struct kwl_compose *compose,
	enum kwl_draw draw,
	VkShaderModule vertex,
	VkShaderModule fragment,
	VkPipelineLayout layout,
	const VkPipelineVertexInputStateCreateInfo *custom,
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
	if (custom != NULL)
		pipeline.pVertexInputState = custom;
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

	/*
	 * The frames are drawn into the swapchain's images at their size while
	 * nothing turns the picture (the turn pass draws into an image of
	 * the logical size instead).
	 */
	compose->target_width = compose->output.width;
	compose->target_height = compose->output.height;

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

/*
 * Makes the turn image of an output opened at an angle the compositor
 * turns, at the logical size; when it cannot be made, the output opens
 * unrotated instead (the choice and displays.conf's line kept, the reason
 * told), and the opening's comparison fits the desktop to that size.
 */
static void
compose_turn_open(
	struct kwl_server *server)
{
	struct kwl_compose *compose;
	VkResult result;

	/* Nothing to make for an angle shown as it is, or by the hardware. */
	compose = server->compose;
	if (compose->turn_rotation == KWL_ROTATE_0)
		return;

	/* The image of the logical size, drawn into from now on. */
	result = kwl_turn_open(compose, server->width, server->height, compose->turn_rotation, &compose->turn);
	if (result == VK_SUCCESS) {
		compose->target_width = compose->turn.width;
		compose->target_height = compose->turn.height;
		printf("KWL OUTPUT turn width=%u height=%u rotation=%u\n", compose->turn.width, compose->turn.height, compose->turn.rotation);
		return;
	}

	/* None: the output shows its picture unrotated, and says why. */
	printf("KWL OUTPUT rotate unsupported key=%s rotation=%u reason=resource result=%d\n", compose->display_name, compose->rotation_selected, (int)result);
	compose->rotate_why |= KWL_ROTATE_WHY_RESOURCE;
	compose->turn_rotation = KWL_ROTATE_0;
	compose->scanout_rotation = KWL_ROTATE_0;
	kwl_output_size_set(server, compose->mode_width, compose->mode_height, KWL_ROTATE_0);
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
 * the heads' in the same order; a window of an output not
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
		/* A sheet is its parent's output's (sheets are drawn under the parent's title bar). */
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
 * surfaces=A,B" (D-ATOMIC: a window is in its output's list
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

/* Draws the cursor in the pass being recorded (the anchor's, or a head's). */
void
kwl_compose_cursor(
	struct kwl_server *server,
	struct kwl_list *command)
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
 * Begins a drawing into the list (a frame's, or a layer's picture that
 * presents nothing): the list is emptied, nothing is known as bound, the
 * joined quads start from their buffer's beginning, the walk's time
 * starts, and the glyphs the glyph worker rendered since the
 * last walk go into their cells.
 */
void
kwl_compose_list_begin(
	struct kwl_compose *compose)
{
	/* The walk is timed from here to the replay (KWL PERF walk). */
	compose->walk_start_cycles = kwl_cycles();

	/* An empty list, with nothing bound and no quad waiting; an image let go from now on may be in it. */
	kwl_list_reset(&compose->list);
	compose->drawing_open = 1U;
	kwl_compose_bind_forget(compose);
	kwl_batch_begin(compose);

	/* The glyphs rendered meanwhile, written into the atlas inside the walk (its copy goes with this drawing). */
	kwl_glass_glyphs_take(compose);
}

/*
 * Takes the queue's lock before a submit, a present or a wait idle on the
 * compose's queue (render, cursor and main threads share it).
 */
void
kwl_queue_lock(
	struct kwl_compose *compose)
{
	/* Held only around the queue's calls. */
	(void)pthread_mutex_lock(&compose->queue_lock);
}

/*
 * Gives the queue's lock back.
 */
void
kwl_queue_unlock(
	struct kwl_compose *compose)
{
	/* The next thread's call may go. */
	(void)pthread_mutex_unlock(&compose->queue_lock);
}

/*
 * Waits for the device to be idle under the queue's lock (Vulkan wants
 * every queue of the device untouched meanwhile).  Only where the main
 * thread may still wait: the output's changes and the end.
 */
void
kwl_device_wait_idle(
	struct kwl_compose *compose)
{
	/* Every queue's work done, no other thread submitting meanwhile. */
	kwl_queue_lock(compose);

	(void)vkDeviceWaitIdle(compose->device);

	kwl_queue_unlock(compose);
}

/*
 * Gives the next image made its own number: numbers only
 * increase from 1, so none is given twice and 0 stays "no image".
 */
uint64_t
kwl_compose_generation(
	struct kwl_compose *compose)
{
	/* One past the last number given. */
	compose->next_generation++;

	/* The new image's number. */
	return compose->next_generation;
}

/*
 * Gives the command buffer of the task being recorded, beginning a new
 * task when none is: the main thread records an image's move to its
 * layout into it (after the host has written the image, or before it
 * does in the same pass), and the task goes to the render thread at the
 * next kwl_compose_task_flush.  Only while the render thread runs.
 */
VkResult
kwl_compose_task_record(
	struct kwl_compose *compose,
	VkCommandBuffer *command)
{
	VkCommandBufferAllocateInfo allocate;
	VkCommandBufferBeginInfo begin;
	VkCommandBuffer task_command;
	VkResult result;

	/* The task being recorded. */
	if (compose->task_command != VK_NULL_HANDLE) {
		*command = compose->task_command;
		return VK_SUCCESS;
	}

	/* A command buffer of the main thread's own pool. */
	memset(&allocate, 0, sizeof(allocate));
	allocate.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
	allocate.commandPool = compose->task_pool;
	allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	allocate.commandBufferCount = 1U;
	result = vkAllocateCommandBuffers(compose->device, &allocate, &task_command);
	if (result != VK_SUCCESS)
		return result;

	/* Begun for one submission; a failure lets it go. */
	memset(&begin, 0, sizeof(begin));
	begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	result = vkBeginCommandBuffer(task_command, &begin);
	if (result != VK_SUCCESS) {
		vkFreeCommandBuffers(compose->device, compose->task_pool, 1U, &task_command);
		return result;
	}

	/* The task numbered after the one given last. */
	compose->task_command = task_command;
	compose->task_open = compose->task_given + 1U;
	*command = task_command;
	return VK_SUCCESS;
}

/*
 * Gives the number of the task being recorded, which an image whose move
 * was just recorded into it waits for (struct kwl_import's task); 0 when
 * no task is being recorded.
 */
uint64_t
kwl_compose_task_current(
	struct kwl_compose *compose)
{
	/* 0 when nothing is recorded. */
	return compose->task_open;
}

/*
 * Gives the task being recorded to the render thread, ended and with a
 * fence of its own: before a list goes to the render thread and before
 * the event loop waits, so that every move recorded in a pass is
 * submitted before the drawings that sample its image.  A failure gives
 * nothing and is kept for the next poll, which ends the compositor.
 */
void
kwl_compose_task_flush(
	struct kwl_compose *compose)
{
	struct kwl_render_message *task;
	VkFenceCreateInfo create;
	VkCommandBuffer command;
	VkFence fence;
	VkResult result;

	/* Nothing recorded. */
	if (compose == NULL || compose->task_command == VK_NULL_HANDLE)
		return;
	command = compose->task_command;
	compose->task_command = VK_NULL_HANDLE;

	/* The commands ended, and the fence the render thread watches. */
	fence = VK_NULL_HANDLE;
	result = vkEndCommandBuffer(command);
	if (result == VK_SUCCESS) {
		memset(&create, 0, sizeof(create));
		create.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
		result = vkCreateFence(compose->device, &create, NULL, &fence);
	}

	/* Its record, which comes back as its completion. */
	task = NULL;
	if (result == VK_SUCCESS) {
		task = calloc(1, sizeof(*task));
		if (task == NULL)
			result = VK_ERROR_OUT_OF_HOST_MEMORY;
	}

	/* A failure: nothing is given, so the images it names never become ready, and the compositor ends. */
	if (result != VK_SUCCESS) {
		if (fence != VK_NULL_HANDLE)
			vkDestroyFence(compose->device, fence, NULL);
		vkFreeCommandBuffers(compose->device, compose->task_pool, 1U, &command);
		compose->task_open = 0U;
		if (compose->task_error == VK_SUCCESS)
			compose->task_error = result;
		return;
	}

	/* Given: submitted before any list given from now on. */
	task->number = compose->task_open;
	task->task_command = command;
	task->task_fence = fence;
	compose->task_given = compose->task_open;
	compose->task_open = 0U;
	kwl_render_task(&compose->render, task);
}

/*
 * Tells whether an image's move to its layout has completed (or it had
 * none to wait for), so that it may be given to the cursor thread, which
 * reads it outside the render thread's order.  Returns 1 when it has.
 */
int
kwl_compose_task_ready(
	struct kwl_compose *compose,
	const struct kwl_import *import)
{
	/* No image, or one moved without a task. */
	if (import == NULL || import->task == 0U)
		return 1;

	/* Every task up to the last applied completion has completed. */
	if (import->task <= compose->task_done)
		return 1;

	/* Still to complete (or not given yet). */
	return 0;
}

/*
 * Tells whether a task given to the render thread has not completed yet:
 * the host then writes no image's staging, which a move to its layout may
 * still be reading.  Returns 1 while one has not.
 */
int
kwl_compose_tasks_busy(
	struct kwl_compose *compose)
{
	/* The last given is past the last completed. */
	if (compose->task_given > compose->task_done)
		return 1;

	/* Every task given has completed. */
	return 0;
}

/*
 * Lets go of an image record: its objects are released now when no
 * drawing being written, given and not completed and no task that moves
 * it may still use them (always before the render thread starts and after
 * it stops), otherwise once they have completed (at a later poll), without
 * waiting.  destroy releases the objects; the record is cleared either way
 * and may be used again or freed at once.  Without memory for the wait the
 * compositor ends, as nothing in use may be destroyed.
 */
void
kwl_compose_bury(
	struct kwl_compose *compose,
	struct kwl_import *import,
	kwl_grave_destroy_fn *destroy)
{
	struct kwl_grave *grave;
	struct kwl_grave **tail;
	int ready;

	/* Nothing in flight uses it: now. */
	ready = kwl_compose_task_ready(compose, import);
	if (!compose->render_started || (ready && !compose->drawing_open && compose->drawings_done >= compose->drawings_given)) {
		destroy(compose, import);
		memset(import, 0, sizeof(*import));
		return;
	}

	/* Its grave; without memory nothing can wait, and nothing in use may be destroyed. */
	grave = calloc(1, sizeof(*grave));
	if (grave == NULL) {
		kwl_fatal("KWL FAILED site=compose_bury errno=%d", ENOMEM);
		_exit(1);
	}

	/* Records what the grave waits for, and how to destroy it. */
	grave->drawings = compose->drawings_given + compose->drawing_open;
	grave->record = *import;
	grave->destroy = destroy;
	memset(import, 0, sizeof(*import));

	/* The end of the list, where it waits after the graves let go before it. */
	for (tail = &compose->graves; *tail != NULL; tail = &(*tail)->next)
		continue;
	*tail = grave;
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
	/* A set given back may come back with another image: it is no longer known as bound. */
	if (set != VK_NULL_HANDLE && set == compose->bound_set)
		kwl_compose_bind_forget(compose);

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
	struct kwl_list *command,
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
	struct kwl_list *command,
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
	struct kwl_list *command,
	const struct kwl_import *import,
	int32_t x,
	int32_t y)
{
	static const float whole[4] = { 0.0f, 0.0f, 1.0f, 1.0f };

	/* The image's size, all of it. */
	compose_quad_part(server, command, import, x, y, import->width, import->height, whole);
}

/*
 * Counts one draw of the frame being recorded (KWL DRAWS): a
 * glass shape (shape nonzero) or a window's quad, in the section drawn now,
 * and whether its pipeline and descriptor set differ from the draw before.
 * Only with --log-frames.
 */
void
kwl_compose_count_draw(
	struct kwl_server *server,
	VkPipeline pipeline,
	VkDescriptorSet set,
	unsigned shape)
{
	struct kwl_draw_counts *counts;

	/* Counted only for the log. */
	if (!server->log_frames)
		return;

	/* The draw, and its section. */
	counts = &server->draw_counts;
	if (shape)
		counts->shapes++;
	else
		counts->quads++;
	if (server->draw_section < KWL_SECTIONS)
		counts->sections[server->draw_section]++;

	/* A pipeline or a set other than the draw before's. */
	if ((uint64_t)(uintptr_t)pipeline != counts->last_pipeline)
		counts->pipelines++;
	if ((uint64_t)(uintptr_t)set != counts->last_set)
		counts->sets++;
	counts->last_pipeline = (uint64_t)(uintptr_t)pipeline;
	counts->last_set = (uint64_t)(uintptr_t)set;
}

/*
 * Counts the pixels a draw covers inside the pass's area, blended or
 * not, for the frame's log (KWL DRAWS); returns them (0 without the log).
 */
uint64_t
kwl_compose_count_pixels(
	struct kwl_server *server,
	float x,
	float y,
	float width,
	float height,
	int blended)
{
	const VkRect2D *area;
	float left;
	float top;
	float right;
	float bottom;
	uint64_t pixels;

	/* Counted only for the log. */
	if (!server->log_frames)
		return 0U;

	/* The draw's rectangle cut to the pass's area. */
	area = &server->compose->pass_area;
	left = x;
	top = y;
	right = x + width;
	bottom = y + height;
	if (left < (float)area->offset.x)
		left = (float)area->offset.x;
	if (top < (float)area->offset.y)
		top = (float)area->offset.y;
	if (right > (float)area->offset.x + (float)area->extent.width)
		right = (float)area->offset.x + (float)area->extent.width;
	if (bottom > (float)area->offset.y + (float)area->extent.height)
		bottom = (float)area->offset.y + (float)area->extent.height;

	/* Nothing inside. */
	if (right <= left || bottom <= top)
		return 0U;

	/* The pixels, by kind. */
	pixels = (uint64_t)((right - left) * (bottom - top));
	if (blended) {
		server->draw_counts.blended_pixels += pixels;
	} else {
		server->draw_counts.opaque_pixels += pixels;
	}

	/* The pixels counted. */
	return pixels;
}

/* Counts a draw of a kind and the pixels it covers for the frame's log (KWL MODES). */
void
kwl_compose_count_kind(
	struct kwl_server *server,
	unsigned kind,
	uint64_t pixels)
{
	/* Counted only for the log, and only of the kinds known. */
	if (!server->log_frames || kind >= KWL_COUNT_KINDS)
		return;

	/* One draw more of the kind, and its pixels. */
	server->draw_counts.kind_draws[kind]++;
	server->draw_counts.kind_pixels[kind] += pixels;
}

/* Draws a part of an image (uv: left, top, right, bottom as fractions) as a quad at a place and size on the output. */
static void
compose_quad_part(
	struct kwl_server *server,
	struct kwl_list *command,
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
	uint64_t pixels;
	int visible;

	/* The output drawn: the anchor, or a head's part of the plane. */
	width = (float)server->width;
	height = (float)server->height;
	if (server->view_width != 0U) {
		x -= server->view_x;
		y -= server->view_y;
		width = (float)server->view_width;
		height = (float)server->view_height;
	}

	/* Omit images wholly outside the pass scissor, or between the damage's rectangles. */
	visible = kwl_compose_clip_visible(server->compose, (float)x, (float)y, (float)quad_width, (float)quad_height);
	if (!visible)
		return;

	/* The rectangle in normalized device coordinates, and the part of the image. */
	constants[0] = 2.0f * (float)x / width - 1.0f;
	constants[1] = 2.0f * (float)y / height - 1.0f;
	constants[2] = 2.0f * (float)(x + (int32_t)quad_width) / width - 1.0f;
	constants[3] = 2.0f * (float)(y + (int32_t)quad_height) / height - 1.0f;
	memcpy(&constants[4], uv, 4U * sizeof(float));

	/* The pipeline for the window's way of drawing, its image, and the strip. */
	kwl_compose_count_draw(server, server->compose->pipelines[import->draw], import->set, 0U);
	pixels = kwl_compose_count_pixels(server, (float)x, (float)y, (float)quad_width, (float)quad_height, import->draw == KWL_DRAW_ALPHA);
	kwl_compose_count_kind(server, KWL_COUNT_QUAD, pixels);
	kwl_batch_flush(server, command);
	kwl_compose_bind(server, command, server->compose->pipelines[import->draw], server->compose->layout, import->set);
	kwl_list_push_constants(command, server->compose->layout, VK_SHADER_STAGE_VERTEX_BIT, 0U, sizeof(constants), constants);
	kwl_compose_clip_draw(server, command, (float)x, (float)y, (float)quad_width, (float)quad_height, KWL_QUAD_VERTICES);
	kwl_compose_count_commands(server, 1U, 1U);
}

/*
 * Works out the part of a swapchain image a frame draws: the damage of
 * this frame and of every frame since the image was last drawn (its buffer
 * age), as rectangles that do not overlap.  Returns 1 with
 * the box around them and the rectangles, or 0 when the whole image is
 * drawn: a change of unknown extent, an image not drawn lately, or damage
 * over most of the output.
 */
static int
compose_region(
	struct kwl_server *server,
	uint32_t image,
	VkRect2D *region,
	VkRect2D *rects,
	unsigned *rect_count)
{
	struct kwl_compose *compose;
	uint64_t frame;
	uint64_t last;
	unsigned slot;
	int partial;

	/* This frame's number and its damage, kept for the images that miss it. */
	compose = server->compose;
	*rect_count = 0U;
	frame = server->frame + 1U;
	slot = (unsigned)(frame % KWL_DAMAGE_HISTORY);
	compose->history_whole[slot] = 1;
	compose->history_count[slot] = 0U;
	if (!server->dirty && server->damaged && server->damage_count != 0U) {
		compose->history_whole[slot] = 0;
		compose->history_count[slot] = server->damage_count;
		memcpy(compose->history[slot], server->damage, (size_t)server->damage_count * sizeof(compose->history[slot][0]));
	}

	/* The image is drawn in this frame. */
	last = 0;
	if (image < KWL_SWAPCHAIN_MAX) {
		last = compose->image_frames[image];
		compose->image_frames[image] = frame;
	}

	/* The damage since it was last drawn. */
	partial = compose_region_since(server, frame, last, image, region, rects, rect_count);
	if (!partial)
		return 0;

	/* Succeeded: a partial frame of the image. */
	return 1;
}

/*
 * Works out a turned output's parts of a frame: the swapchain's
 * image's damage (region, rects, count: its buffer age, logical) turned to
 * the physical rectangles of the turn pass, and the turn image's own.
 */
static void
compose_turn_regions(
	struct kwl_server *server,
	struct kwl_frame_region *drawn,
	const VkRect2D *region,
	const VkRect2D *rects,
	unsigned count)
{
	struct kwl_compose *compose;
	int32_t box[4];
	int32_t turned[4];
	unsigned index;
	int partial;

	/* The swapchain's image, whole when its damage is unknown. */
	compose = server->compose;
	drawn->present_count = 0U;
	if (region != NULL) {
		/* The box around the damage, turned. */
		box[0] = region->offset.x;
		box[1] = region->offset.y;
		box[2] = region->offset.x + (int32_t)region->extent.width;
		box[3] = region->offset.y + (int32_t)region->extent.height;
		kwl_rotate_rect(compose->turn.rotation, compose->output.width, compose->output.height, box, turned);
		drawn->present_area.offset.x = turned[0];
		drawn->present_area.offset.y = turned[1];
		drawn->present_area.extent.width = (uint32_t)(turned[2] - turned[0]);
		drawn->present_area.extent.height = (uint32_t)(turned[3] - turned[1]);

		/* One rectangle is the box itself. */
		if (count == 0U) {
			drawn->present_rects[0] = drawn->present_area;
			drawn->present_count = 1U;
		}

		/* Each rectangle turned. */
		for (index = 0U; index < count; index++) {
			box[0] = rects[index].offset.x;
			box[1] = rects[index].offset.y;
			box[2] = rects[index].offset.x + (int32_t)rects[index].extent.width;
			box[3] = rects[index].offset.y + (int32_t)rects[index].extent.height;
			kwl_rotate_rect(compose->turn.rotation, compose->output.width, compose->output.height, box, turned);
			drawn->present_rects[index].offset.x = turned[0];
			drawn->present_rects[index].offset.y = turned[1];
			drawn->present_rects[index].extent.width = (uint32_t)(turned[2] - turned[0]);
			drawn->present_rects[index].extent.height = (uint32_t)(turned[3] - turned[1]);
			drawn->present_count = index + 1U;
		}
	}

	/* The turn pass's part, for the tests (physical). */
	if (server->log_frames) {
		printf("KWL DAMAGE turn frame=%llu rotation=%u rects=%u", (unsigned long long)server->frame + 1U, compose->turn.rotation, drawn->present_count);
		for (index = 0U; index < drawn->present_count; index++) {
			printf(" %d,%d,%u,%u", drawn->present_rects[index].offset.x, drawn->present_rects[index].offset.y, drawn->present_rects[index].extent.width,
			    drawn->present_rects[index].extent.height);
		}

		/* The line ends. */
		printf("\n");
	}

	/* The turn image's own damage, since it was last drawn. */
	partial = compose_turn_region(server, &drawn->scene_area, drawn->scene_rects, &drawn->count);
	drawn->region = NULL;
	drawn->rects = drawn->scene_rects;
	if (partial)
		drawn->region = &drawn->scene_area;
}

/*
 * Works out the part of the turn image a frame draws: the damage
 * since it was last drawn, as compose_region does for a swapchain image
 * (the turn image is drawn in every frame, so usually this frame's alone).
 * Called after compose_region, which keeps this frame's damage.
 */
static int
compose_turn_region(
	struct kwl_server *server,
	VkRect2D *region,
	VkRect2D *rects,
	unsigned *rect_count)
{
	struct kwl_compose *compose;
	uint64_t frame;
	uint64_t last;
	int partial;

	/* The turn image is drawn in this frame. */
	compose = server->compose;
	*rect_count = 0U;
	frame = server->frame + 1U;
	last = compose->turn.frame;
	compose->turn.frame = frame;

	/* The damage since it was last drawn. */
	partial = compose_region_since(server, frame, last, KWL_SWAPCHAIN_MAX, region, rects, rect_count);
	if (!partial)
		return 0;

	/* Succeeded: a partial frame of the turn image. */
	return 1;
}

/*
 * Works out the damage of the frames after last up to frame, as
 * rectangles that do not overlap, cut to the image drawn into (the
 * logical size).  Returns 1 with their box and the rectangles, or 0 for
 * the whole image: a change of unknown extent, an image not drawn lately,
 * or damage over most of it.  image names a swapchain image in the log
 * (KWL_SWAPCHAIN_MAX: the turn image).
 */
static int
compose_region_since(
	struct kwl_server *server,
	uint64_t frame,
	uint64_t last,
	uint32_t image,
	VkRect2D *region,
	VkRect2D *rects,
	unsigned *rect_count)
{
	struct kwl_compose *compose;
	int32_t list[KWL_DAMAGE_RECTS][4];
	int32_t box[4];
	int64_t area;
	uint64_t index;
	unsigned count;
	unsigned slot;
	unsigned rect;
	unsigned kept;
	VkResult result;

	/* An image never drawn, or not for longer than the history, is drawn whole. */
	compose = server->compose;
	*rect_count = 0U;
	if (last == 0U || frame - last >= KWL_DAMAGE_HISTORY)
		return 0;

	/* The damage of each frame it missed and of this one, joined as few rectangles; a whole one makes it whole. */
	count = 0U;
	for (index = last + 1U; index <= frame; index++) {
		slot = (unsigned)(index % KWL_DAMAGE_HISTORY);
		if (compose->history_whole[slot])
			return 0;
		for (rect = 0U; rect < compose->history_count[slot]; rect++)
			kwl_damage_rects_add(list, &count, compose->history[slot][rect]);
	}

	/* Each rectangle inside the output, those left with nothing dropped; their box and their area. */
	kept = 0U;
	area = 0;
	box[0] = INT32_MAX;
	box[1] = INT32_MAX;
	box[2] = INT32_MIN;
	box[3] = INT32_MIN;
	for (rect = 0U; rect < count; rect++) {
		/* The rectangle cut to the output. */
		if (list[rect][0] < 0)
			list[rect][0] = 0;
		if (list[rect][1] < 0)
			list[rect][1] = 0;
		if (list[rect][2] > (int32_t)compose->target_width)
			list[rect][2] = (int32_t)compose->target_width;
		if (list[rect][3] > (int32_t)compose->target_height)
			list[rect][3] = (int32_t)compose->target_height;
		if (list[rect][2] <= list[rect][0] || list[rect][3] <= list[rect][1])
			continue;

		/* Kept, in the box and the area. */
		rects[kept].offset.x = list[rect][0];
		rects[kept].offset.y = list[rect][1];
		rects[kept].extent.width = (uint32_t)(list[rect][2] - list[rect][0]);
		rects[kept].extent.height = (uint32_t)(list[rect][3] - list[rect][1]);
		kept++;
		area += (int64_t)(list[rect][2] - list[rect][0]) * (list[rect][3] - list[rect][1]);
		if (list[rect][0] < box[0])
			box[0] = list[rect][0];
		if (list[rect][1] < box[1])
			box[1] = list[rect][1];
		if (list[rect][2] > box[2])
			box[2] = list[rect][2];
		if (list[rect][3] > box[3])
			box[3] = list[rect][3];
	}

	/* Nothing inside the output: the whole image (as before, for a damage left of nothing). */
	if (kept == 0U)
		return 0;

	/*
	 * Most of the output drawn is drawn whole (the loading pass costs more
	 * than it saves): the rectangles' own area, not their box's, so that
	 * two small changes far apart stay a partial frame.
	 */
	if (area * 10 > (int64_t)compose->target_width * compose->target_height * 7)
		return 0;

	/* The pass that keeps the image's pixels, made the first time. */
	result = kwl_compose_load_pass(compose);
	if (result != VK_SUCCESS)
		return 0;

	/* Succeeded: the box and the rectangles, logged when frames are. */
	region->offset.x = box[0];
	region->offset.y = box[1];
	region->extent.width = (uint32_t)(box[2] - box[0]);
	region->extent.height = (uint32_t)(box[3] - box[1]);
	*rect_count = kept;
	if (server->log_frames) {
		if (image >= KWL_SWAPCHAIN_MAX)
			printf("KWL DAMAGE turn-scene frame=%llu x=%d y=%d width=%u height=%u rects=%u", (unsigned long long)frame, box[0], box[1], region->extent.width, region->extent.height, kept);
		else
			printf("KWL DAMAGE frame=%llu image=%u x=%d y=%d width=%u height=%u rects=%u", (unsigned long long)frame, image, box[0], box[1], region->extent.width, region->extent.height, kept);
		for (rect = 0U; rect < kept; rect++)
			printf(" %d,%d,%u,%u", rects[rect].offset.x, rects[rect].offset.y, rects[rect].extent.width, rects[rect].extent.height);
		printf("\n");
	}
	return 1;
}

/*
 * Records a frame: the background, then each window from the bottom.  A
 * partial frame (region) draws inside its damage's rectangles (rects,
 * rect_count of them, inside region); two or more are drawn apart in one
 * pass over the box around them.
 */
static VkResult
compose_record(
	struct kwl_server *server,
	uint32_t image,
	struct kwl_object **windows,
	unsigned count,
	const struct kwl_frame_region *drawn)
{
	struct kwl_compose *compose;
	struct kwl_list *command;
	VkRenderPassBeginInfo pass;
	VkClearValue clear;
	VkViewport viewport;
	VkRect2D scissor;
	VkDeviceSize offset;
	const VkRect2D *region;
	const VkRect2D *rects;
	VkImage target;
	VkFramebuffer framebuffer;
	unsigned rect_count;
	unsigned index;
	unsigned reuse;
	unsigned capture;

	/* The part of the image the scene is drawn in, and the image and framebuffer it is drawn into (the turn image's). */
	region = drawn->region;
	rects = drawn->rects;
	rect_count = drawn->count;
	compose = server->compose;
	target = kwl_compose_target_image(compose, image);
	framebuffer = compose->framebuffers[image];
	if (compose->turn.image != VK_NULL_HANDLE)
		framebuffer = compose->turn.framebuffer;

	/* A fresh list for the frame's drawing, which compose_replay records; no rectangle is drawn apart yet. */
	command = &compose->list;
	kwl_compose_list_begin(compose);
	compose->clip_count = 0U;
	compose->clip_scissor_valid = 0U;

	/*
	 * The scene is kept only while the cursor is drawn into the frames: with
	 * the cursor on its plane no frame differs from the one before by the
	 * cursor alone, so none could reuse it (all 266 frames of the Pi's
	 * 2026-10-12 boot were reuse=0), and capturing it cost a whole-image
	 * copy and a second pass each frame.  One value
	 * decides the whole frame's use of it (review-2 N1); a frame without
	 * the capture leaves the scene unknown, so the first after the plane
	 * closes is drawn whole.
	 */
	capture = 0U;
	if (compose->scene.buffer != VK_NULL_HANDLE && !compose->cursor_plane.shown)
		capture = 1U;
	if (!capture)
		compose->scene.valid = 0U;

	/* Unknown cache contents require one whole scene before any reuse. */
	if (capture && !compose->scene.valid) {
		region = NULL;
		rects = NULL;
		rect_count = 0U;
	}

	/* A partial frame of one rectangle draws it as its region (the box around one is itself). */
	if (region != NULL && rect_count == 0U) {
		rects = region;
		rect_count = 1U;
	}

	/*
	 * Only cursor damage over an unchanged initialized scene may skip client
	 * sampling.  An image that is whole only because of its buffer age is
	 * restored whole from the same scene.
	 */
	reuse = 0U;
	compose->scene.written = 0U;
	if (capture && compose->scene.valid && !compose->scene.changed && !server->dirty) {
		reuse = 1U;

		/* Other outputs retain their independent scene construction and callback order. */
		for (index = 0U; index < KWL_HEADS; index++) {
			/* A live secondary output needs the ordinary whole-scene recording. */
			if (compose->heads[index].open)
				reuse = 0U;
		}
	}

	/* Log the actual reuse decision only through the supported frame diagnostic. */
	if (server->log_frames) {
		printf(
			"KWL SCENE frame=%llu reuse=%u changed=%u valid=%u capture=%u\n",
			(unsigned long long)server->frame + 1U,
			reuse,
			compose->scene.changed,
			compose->scene.valid,
			capture);
	}

	/* The frame's draws are counted from nothing (KWL DRAWS). */
	memset(&server->draw_counts, 0, sizeof(server->draw_counts));
	server->draw_section = KWL_SECTION_OTHER;

	/* A redrawn scene records the hover tests its drawing makes; a whole one can vouch for later cursor-only moves. */
	if (!reuse)
		kwl_hover_begin(server, region == NULL);

	/* The clients' images imported since the last frame go to the layout they are sampled in. */
	kwl_import_layouts_record(compose, command);

	/* The colours of flat glass the last frames asked for, before anything samples them (glass-map.c). */
	if (server->glass)
		kwl_glass_maps_inband(server, command);

	/* The pictures App Home's animation lacks, drawn before the frame's pass (layer-cache.c). */
	compose->frame_layers_generation = compose->layers.generation;
	kwl_layers_inband(server, command, windows, count);

	/* The pictures a window's dock or undock animation shows, drawn in its first frame (layer-cache.c). */
	kwl_layers_anim_inband(server, command, windows, count);

	/* The anchor's pass: its own part of the plane. */
	server->view_output = KWL_PLANE_ANCHOR;
	server->view_x = 0;
	server->view_y = 0;
	server->view_width = 0U;
	server->view_height = 0U;

	/* Removes every older cursor covered by buffer-age damage before drawing its new place: each rectangle, not their box. */
	if (reuse)
		kwl_scene_copy(&compose->scene, command, target, kwl_compose_target_layout(compose), rects, rect_count, 1U);

	/* The pass clears the image to the background. */
	memset(&clear, 0, sizeof(clear));
	clear.color.float32[0] = KWL_BACKGROUND_RED;
	clear.color.float32[1] = KWL_BACKGROUND_GREEN;
	clear.color.float32[2] = KWL_BACKGROUND_BLUE;
	clear.color.float32[3] = 1.0f;
	memset(&pass, 0, sizeof(pass));
	pass.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
	pass.renderPass = kwl_compose_target_pass(compose, KWL_PLANE_ANCHOR, 0U);
	if (region != NULL || reuse)
		pass.renderPass = kwl_compose_target_pass(compose, KWL_PLANE_ANCHOR, 1U);
	pass.framebuffer = framebuffer;
	compose->framebuffer_now = framebuffer;
	compose->backdrop_set = VK_NULL_HANDLE;
	compose->backdrop_recording = 0U;
	pass.renderArea.extent.width = compose->target_width;
	pass.renderArea.extent.height = compose->target_height;

	/* Restrict tile loading and storing to the same preserved damage as rasterization; the backdrop takes the pass up again so. */
	if (region != NULL)
		pass.renderArea = *region;
	compose->pass_area = pass.renderArea;

	/* The clear value belongs to a whole frame; the partial pass and a restored scene retain old pixels. */
	pass.clearValueCount = 1U;
	pass.pClearValues = &clear;
	kwl_list_begin_render_pass(command, &pass, VK_SUBPASS_CONTENTS_INLINE);
	kwl_compose_bind_forget(compose);

	/* The whole output is placed; the drawing is kept to the damage when there is one (design D4). */
	memset(&viewport, 0, sizeof(viewport));
	viewport.width = (float)compose->target_width;
	viewport.height = (float)compose->target_height;
	viewport.maxDepth = 1.0f;
	kwl_list_set_viewport(command, 0U, 1U, &viewport);
	memset(&scissor, 0, sizeof(scissor));
	scissor.extent.width = compose->target_width;
	scissor.extent.height = compose->target_height;
	if (region != NULL)
		scissor = *region;
	compose->scissor_now = scissor;
	kwl_list_set_scissor(command, 0U, 1U, &scissor);

	/*
	 * Two or more rectangles are drawn apart inside the box (compose-bind.c):
	 * each quad once for each one it meets, the scissor cut to it.  The
	 * anchor's pass alone; the heads', the layers' and the backdrop's are
	 * drawn as before.
	 */
	if (rect_count >= 2U) {
		memcpy(compose->clips, rects, (size_t)rect_count * sizeof(compose->clips[0]));
		compose->clip_count = rect_count;
		compose->clip_framebuffer = framebuffer;
	}

	/* Every quad's corners. */
	offset = 0U;
	kwl_list_bind_vertex_buffers(command, 0U, 1U, &compose->corners, &offset);

	/*
	 * The windows, bottom to top (painter's order): plainly, or in the glass
	 * look with the wallpaper, their title bars and the system bar.  Then the
	 * cursor over them.
	 */
	if (!reuse) {
		if (server->glass) {
			kwl_glass_draw(server, command, windows, count);
		} else {
			/* The desktop's icons under the windows (desktop.c). */
			kwl_desktop_draw(server, command);

			/* Each window. */
			for (index = 0; index < count; index++) {
				/* A window between its sub-surfaces below and above it (subsurface.c). */
				kwl_subsurface_draw(server, command, windows[index], (float)windows[index]->x, (float)windows[index]->y, 1.0f, 1.0f, 0U);
				kwl_compose_surface_quad(server, command, windows[index], surface_image(windows[index]), windows[index]->x, windows[index]->y);
				kwl_subsurface_draw(server, command, windows[index], (float)windows[index]->x, (float)windows[index]->y, 1.0f, 1.0f, 1U);
			}

			/* The popups over the windows. */
			kwl_popup_draw(server, command);
		}

		/* The input method's candidate window over the windows and their popups, in both looks (input-method.c). */
		kwl_ime_popup_draw(server, command);
	}

	/* Captures scene pixels before the cursor can contaminate the reusable background. */
	if (!reuse && capture) {
		kwl_batch_flush(server, command);
		kwl_list_end_render_pass(command);
		kwl_scene_copy(&compose->scene, command, target, kwl_compose_target_layout(compose), rects, rect_count, 0U);
		compose->scene.written = 1U;

		/* The compatible LOAD pass retains scene pixels and adds only the cursor. */
		pass.renderPass = kwl_compose_target_pass(compose, KWL_PLANE_ANCHOR, 1U);
		kwl_list_begin_render_pass(command, &pass, VK_SUBPASS_CONTENTS_INLINE);
		kwl_compose_bind_forget(compose);
	}

	/* The cursor over everything, in the rectangles too. */
	compose_cursor(server, command);
	kwl_batch_flush(server, command);
	kwl_list_end_render_pass(command);

	/* The anchor's pass is done: nothing after it is drawn apart. */
	compose->clip_count = 0U;
	compose->clip_scissor_valid = 0U;

	/* A turned output's picture copied turned onto the swapchain's image, in that image's own damage. */
	if (compose->turn.image != VK_NULL_HANDLE)
		kwl_turn_record(server, command, image, &drawn->present_area, drawn->present_rects, drawn->present_count);

	/* A test image's capture copies the finished image (shot.c; nothing elsewhere). */
	kwl_shot_record(server, command, compose->output.images[image]);

	/* The heads' pictures: the desktop copied (mirror, from the turn image when it turns), or the wallpaper (extended). */
	kwl_heads_record(server, command, target, kwl_compose_target_layout(compose));

	/* The scene's hover tests, including the heads' bars, are all recorded. */
	if (!reuse)
		kwl_hover_end(server);

	/* The host images written up to now, the drawing's own included (a glyph first drawn), are copied first. */
	kwl_host_images_take(compose, command);

	/* A list that could not grow is not replayed. */
	if (command->failed)
		return VK_ERROR_OUT_OF_HOST_MEMORY;

	/* Succeeded: the list holds the frame. */
	return VK_SUCCESS;
}

/*
 * Draws the cursor at the pointer: the client's cursor surface (its hotspot
 * at the pointer), or the compositor's arrow (its tip), with alpha; nothing when
 * hidden (design D8) or over a game that fills the output.
 */
static void
compose_cursor(
	struct kwl_server *server,
	struct kwl_list *command)
{
	const struct kwl_import *image;
	struct kwl_object *surface;
	struct kwl_import alpha;
	int32_t hotspot_x;
	int32_t hotspot_y;
	int hidden;
	int shown;

	/*
	 * A drag and drop's icon under the cursor (its surface's corner at the
	 * pointer moved by its attach offsets), or the compositor's
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

	/* A cursor that has not moved since the start is not drawn. */
	if (server->pointer_unmoved)
		return;

	/* A game that fills the output shows no cursor. */
	hidden = kwl_scanout_cursor_hidden(server);
	if (hidden)
		return;

	/* The cursor plane shows the cursor without the frame (cursor-plane.c). */
	if (server->compose->cursor_plane.shown)
		return;

	/* A client's cursor only over its own window: elsewhere the frame's arrow or the compositor's. */
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

	/* A shape the client asked for, its hotspot at the pointer (cursor.c). */
	image = kwl_cursor_image(server, &hotspot_x, &hotspot_y);
	if (image != NULL) {
		compose_quad(server, command, image, server->pointer_x - hotspot_x, server->pointer_y - hotspot_y);
		return;
	}

	/* Otherwise the arrow, its tip at the pointer. */
	if (server->arrow != NULL)
		compose_quad(server, command, server->arrow, server->pointer_x, server->pointer_y);
}

/*
 * Tells which image shows the cursor and where its hotspot is, as compose_cursor draws it.
 *
 * Returns 1 with the image, 0 when no cursor shows (unmoved, hidden, over
 * a game that fills the output, a cursor surface without an image) and -1
 * for a drag, whose icon and marks only composing draws.
 */
int
kwl_compose_cursor_source(
	struct kwl_server *server,
	const struct kwl_import **image,
	int32_t *hotspot_x,
	int32_t *hotspot_y)
{
	const struct kwl_import *found;
	struct kwl_object *surface;
	int hidden;
	int shown;

	/* Nothing found yet. */
	*image = NULL;
	*hotspot_x = 0;
	*hotspot_y = 0;

	/* A drag's icon and marks are composed with the cursor. */
	if (server->dnd_active)
		return -1;

	/* A cursor that has not moved since the start shows nothing. */
	if (server->pointer_unmoved)
		return 0;

	/* A game that fills the output shows no cursor. */
	hidden = kwl_scanout_cursor_hidden(server);
	if (hidden)
		return 0;

	/* Away from the client's window: the frame's arrow or the compositor's. */
	shown = kwl_cursor_client_shown(server);
	if (!shown) {
		if (server->frame_edges != 0U) {
			found = kwl_cursor_image(server, hotspot_x, hotspot_y);
			if (found != NULL) {
				*image = found;
				return 1;
			}
		}
		*hotspot_x = 0;
		*hotspot_y = 0;
		*image = server->arrow;
		return server->arrow != NULL ? 1 : 0;
	}

	/* A hidden cursor shows nothing. */
	if (server->cursor_hidden)
		return 0;

	/* A window frame's resize arrow, over the client's own cursor. */
	if (server->frame_edges != 0U) {
		found = kwl_cursor_image(server, hotspot_x, hotspot_y);
		if (found != NULL) {
			*image = found;
			return 1;
		}
	}

	/* The client's surface, when it has an image. */
	surface = server->cursor_surface;
	if (surface != NULL && !surface->dead) {
		*image = surface_image(surface);
		*hotspot_x = server->cursor_hotspot_x;
		*hotspot_y = server->cursor_hotspot_y;
		return *image != NULL ? 1 : 0;
	}

	/* A shape the client asked for, else the arrow. */
	found = kwl_cursor_image(server, hotspot_x, hotspot_y);
	if (found != NULL) {
		*image = found;
		return 1;
	}
	*hotspot_x = 0;
	*hotspot_y = 0;
	*image = server->arrow;
	return server->arrow != NULL ? 1 : 0;
}

/*
 * Keeps one drawing's walk and replay times and how many things it drew,
 * for the five seconds' KWL PERF walk; a drawing past the
 * window's room is only counted.
 */
static void
compose_walk_timed(
	struct kwl_server *server,
	uint64_t walk,
	uint64_t replay,
	unsigned quiet)
{
	struct kwl_perf *perf;
	uint32_t slot;

	/* The pass's walks, for its KWL SLOW_PASS line. */
	perf = &server->perf;
	perf->pass_walk_cycles += walk;

	/* No room left in the window. */
	if (perf->walk_count == KWL_PERF_WALKS) {
		perf->walk_dropped++;
		return;
	}

	/* The drawing's sample. */
	slot = perf->walk_count;
	perf->walk_cycles[slot] = walk;
	perf->replay_cycles[slot] = replay;
	perf->walk_things[slot] = kwl_list_things(&server->compose->list);
	perf->walk_quiet[slot] = (uint8_t)quiet;
	perf->walk_count++;
}

/*
 * Logs the submitted frame's draws (KWL DRAWS, --log-frames):
 * the shapes and the windows' quads, those of App Home, the desktop layer,
 * the system bar and the rest, the pipeline and descriptor set changes,
 * whether the frame captured the scene, and the host images copied first.
 */
static void
compose_log_draws(
	struct kwl_server *server)
{
	const struct kwl_draw_counts *counts;

	/* The counts compose_record and the drawing made. */
	counts = &server->draw_counts;
	printf(
		"KWL DRAWS frame=%llu draws=%u shapes=%u quads=%u home=%u layer=%u bar=%u other=%u pipelines=%u sets=%u capture=%u copies=%u cmds draw=%u pipeline=%u set=%u push=%u pixels opaque=%llu blended=%llu area=%ux%u\n",
		(unsigned long long)server->frame,
		counts->shapes + counts->quads,
		counts->shapes,
		counts->quads,
		counts->sections[KWL_SECTION_HOME],
		counts->sections[KWL_SECTION_LAYER],
		counts->sections[KWL_SECTION_BAR],
		counts->sections[KWL_SECTION_OTHER],
		counts->pipelines,
		counts->sets,
		server->compose->scene.written,
		server->compose->list.copy_count,
		counts->cmd_draws,
		counts->cmd_pipelines,
		counts->cmd_sets,
		counts->cmd_pushes,
		(unsigned long long)counts->opaque_pixels,
		(unsigned long long)counts->blended_pixels,
		server->compose->pass_area.extent.width,
		server->compose->pass_area.extent.height);
}

/* Counts what the frame's list asks of the GPU (KWL PASSES), only with --log-frames. */
static void
compose_count_passes(
	struct kwl_server *server)
{
	struct kwl_compose *compose;
	VkRenderPass loading[2];

	/* Only for the frame's lines. */
	if (!server->log_frames)
		return;

	/* The passes that keep the image's pixels: the output's, and the turn image's. */
	compose = server->compose;
	loading[0] = compose->pass_load;
	loading[1] = compose->turn_load_pass;
	kwl_list_count_passes(&compose->list, loading, 2U, COMPOSE_JOB_DRAWS, &compose->frame_passes);
}

/*
 * Logs what the submitted frame asked of the GPU (KWL PASSES,
 * --log-frames): its passes, the jobs they took, the pixels of their
 * areas and those loaded again, its copies, and the first passes one by
 * one (area, draws, l when it loaded).
 */
static void
compose_log_passes(
	struct kwl_server *server)
{
	const struct kwl_list_passes *passes;
	unsigned index;

	/* The frame's totals. */
	passes = &server->compose->frame_passes;
	printf(
		"KWL PASSES frame=%llu passes=%u jobs=%u draws=%u loading=%u area_px=%llu load_px=%llu copies=%u copy_px=%llu",
		(unsigned long long)server->frame,
		passes->passes,
		passes->jobs,
		passes->draws,
		passes->loading_passes,
		(unsigned long long)passes->area_pixels,
		(unsigned long long)passes->load_pixels,
		passes->copies,
		(unsigned long long)passes->copy_pixels);

	/* The first passes one by one: where, how large, how many draws, and whether they loaded. */
	for (index = 0U; index < passes->detail_count; index++) {
		printf(
			" %d,%d,%ux%u:%u%s",
			passes->detail_areas[index].offset.x,
			passes->detail_areas[index].offset.y,
			passes->detail_areas[index].extent.width,
			passes->detail_areas[index].extent.height,
			passes->detail_draws[index],
			passes->detail_loads[index] ? "l" : "");
	}

	/* The line ends. */
	printf("\n");
}

/*
 * Logs the submitted frame's draws by kind (KWL MODES, --log-frames): for
 * each of the glass look's modes and the windows' quads, the draws and
 * the pixels they cover inside the pass's area.
 */
static void
compose_log_kinds(
	struct kwl_server *server)
{
	static const char *const names[KWL_COUNT_KINDS] = {
		"glass", "shadow", "image", "solid", "ring", "text", "blur", "dark", "quad"
	};
	const struct kwl_draw_counts *counts;
	unsigned kind;

	/* The frame, then each kind drawn as draws/pixels. */
	counts = &server->draw_counts;
	printf("KWL MODES frame=%llu", (unsigned long long)server->frame);
	for (kind = 0U; kind < KWL_COUNT_KINDS; kind++) {
		if (counts->kind_draws[kind] != 0U)
			printf(" %s=%u/%llu", names[kind], counts->kind_draws[kind], (unsigned long long)counts->kind_pixels[kind]);
	}

	/* The line ends. */
	printf("\n");
}

/*
 * Gives the drawing written into the list since kwl_compose_list_begin to
 * the render thread as a layer's picture that presents nothing
 * (layer-cache.c): the host images written up to now are
 * copied first, as for a frame.  The render thread must be ready for the
 * layer (kwl_compose_want_layer).  Its slot says it draws a layer's
 * picture (bake).
 */
VkResult
kwl_compose_submit_quiet(
	struct kwl_server *server)
{
	struct kwl_render_job job;
	struct kwl_compose *compose;
	struct kwl_frame_slot *slot;

	/* The host images the drawing wrote, given to the list. */
	compose = server->compose;
	kwl_host_images_take(compose, &compose->list);

	/* A list that could not grow is not given (nor written any more). */
	if (compose->list.failed) {
		compose->drawing_open = 0U;
		return VK_ERROR_OUT_OF_HOST_MEMORY;
	}

	/* The pictures it draws belong to this output's generation (layer-cache.c), as its slot says. */
	compose->frame_layers_generation = compose->layers.generation;
	slot = &compose->slots[compose->walk_slot];
	slot->bake = 1U;
	slot->layers_generation = compose->frame_layers_generation;
	slot->scene_written = 0U;
	slot->held_count = 0U;
	slot->callbacks = NULL;
	slot->copies = 0U;
	if (compose->list.copy_count != 0U)
		slot->copies = 1U;
	slot->number = server->frame;
	slot->start_cycles = kwl_cycles();
	slot->start_ms = kwl_milliseconds();
	slot->flying = 1U;

	/* The job: no image is acquired and none is presented. */
	memset(&job, 0, sizeof(job));
	job.number = compose->ready.number;
	job.kind = KWL_RENDER_LAYER;
	job.slot = compose->walk_slot;

	/* In flight until it completes; the want went down; the next drawing goes into the next slot; the moves recorded before it go first. */
	compose->walk_slot = (compose->walk_slot + 1U) % KWL_FRAME_SLOTS;
	compose->walk_cycles = kwl_cycles() - compose->walk_start_cycles;
	compose->in_flight++;
	compose->drawings_given++;
	compose->drawing_open = 0U;
	compose->flight_ms = kwl_milliseconds();
	compose->ready_valid = 0U;
	compose->want_kind = KWL_RENDER_NONE;
	kwl_compose_task_flush(compose);
	kwl_render_answer(&compose->render, job.number, KWL_RENDER_ANSWER_LIST, &job);

	/* Succeeded: the drawing is in flight. */
	return VK_SUCCESS;
}

/*
 * Tells whether the main thread may write host images' staging, make
 * images and use the queue and the frames' command pool outside a walk
 * now (the wl_shm copies, shm.c): nothing in flight, or only frames that
 * copy no host image while the render thread runs no job.  Returns 1 when
 * it may.
 */
int
kwl_compose_host_writable(
	struct kwl_server *server)
{
	struct kwl_compose *compose;
	unsigned index;
	int running;
	int busy;

	/* No task in flight, which may still be reading a staging's layout. */
	compose = server->compose;
	busy = kwl_compose_tasks_busy(compose);
	if (busy)
		return 0;

	/* Nothing in flight. */
	if (compose->in_flight == 0U)
		return 1;

	/* No frame in flight copies a host image's staging. */
	for (index = 0U; index < KWL_FRAME_SLOTS; index++) {
		if (compose->slots[index].flying && (compose->slots[index].copies || compose->slots[index].bake))
			return 0;
	}

	/* The render thread records, submits and presents nothing meanwhile. */
	running = kwl_render_running(&compose->render);
	if (running)
		return 0;

	/* Succeeded: the frames in flight read none of it. */
	return 1;
}

/*
 * Tells whether a frame may be drawn now: nothing in flight, or one frame
 * that the next may follow before it completes.  A frame drawn then
 * records and submits while the one before runs on the GPU (the slots'
 * command buffers, fences, vertices and holds are apart; what both use in
 * the GPU's memory is ordered by the queue).  The CPU writes host images'
 * staging while it draws, so the frame in flight must copy none; and a
 * layer's picture, the heads, a turned output, the cursor-free scene's
 * capture and a test's screen capture keep one at a time.
 */
static int
compose_may_draw(
	struct kwl_server *server)
{
	struct kwl_compose *compose;
	struct kwl_frame_slot *slot;
	unsigned index;
	int waiting;
	int busy;

	/* A task in flight first: the drawing writes host images' staging, which a move to its layout may still be reading. */
	compose = server->compose;
	busy = kwl_compose_tasks_busy(compose);
	if (busy)
		return 0;

	/* Nothing in flight, or every slot taken. */
	if (compose->in_flight == 0U)
		return 1;
	if (compose->in_flight >= KWL_FRAME_SLOTS)
		return 0;

	/* The frame in flight copies no host image and draws no layer's picture. */
	for (index = 0U; index < KWL_FRAME_SLOTS; index++) {
		slot = &compose->slots[index];
		if (!slot->flying)
			continue;
		if (slot->copies || slot->bake)
			return 0;
	}

	/* No head, no turned output, no capture of the cursor-free scene. */
	for (index = 0U; index < KWL_HEADS; index++) {
		if (compose->heads[index].open)
			return 0;
	}
	if (compose->turn.image != VK_NULL_HANDLE)
		return 0;
	if (compose->scene.buffer != VK_NULL_HANDLE && !compose->cursor_plane.shown)
		return 0;

	/* No test's screen capture waiting for a frame. */
	waiting = kwl_shot_waiting();
	if (waiting)
		return 0;

	/* Succeeded: the next frame may follow the one in flight. */
	return 1;
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
	struct kwl_frame_slot *slot;
	struct kwl_object *surface;
	struct kwl_object **tail;
	unsigned index;

	/* Each window's current buffer is held by the frame, in the slot the frame was recorded for. */
	slot = &server->compose->slots[server->compose->walk_slot];
	slot->held_count = 0U;
	slot->callbacks = NULL;
	for (index = 0; index < count; index++) {
		kwl_buffer_get(windows[index]->current);
		slot->held[slot->held_count++] = windows[index]->current;
		windows[index]->fresh = 0;

		/*
		 * A window awaited after an earlier frame that has not committed
		 * since is not awaited any more: one that asked for a frame
		 * callback and then had nothing to draw would otherwise hold every
		 * later frame for the pacing's whole wait.
		 */
		if (windows[index]->awaited && windows[index]->committed_callbacks == NULL) {
			windows[index]->awaited = 0;
			server->awaiting--;
		}

		/* A window with frame callbacks is awaited after the frame (frame pacing). */
		if (windows[index]->committed_callbacks != NULL && !windows[index]->awaited) {
			windows[index]->awaited = 1;
			server->awaiting++;
		}

		/* Its frame callbacks wait for the frame, in request order. */
		tail = &slot->callbacks;
		while (*tail != NULL)
			tail = &(*tail)->callback_next;
		*tail = windows[index]->committed_callbacks;
		windows[index]->committed_callbacks = NULL;
	}

	/* A drag's icon surface is held and told too. */
	surface = server->dnd_icon;
	if (server->dnd_active && surface != NULL && !surface->dead && surface->current != NULL) {
		kwl_buffer_get(surface->current);
		slot->held[slot->held_count++] = surface->current;

		/* Its frame callbacks after the windows'. */
		tail = &slot->callbacks;
		while (*tail != NULL)
			tail = &(*tail)->callback_next;
		*tail = surface->committed_callbacks;
		surface->committed_callbacks = NULL;
	}

	/* The desktop surface is held and told too (desktop.c). */
	surface = kwl_desktop_surface(server);
	if (surface != NULL && surface->current != NULL) {
		kwl_buffer_get(surface->current);
		slot->held[slot->held_count++] = surface->current;

		/* Its frame callbacks after the windows'. */
		tail = &slot->callbacks;
		while (*tail != NULL)
			tail = &(*tail)->callback_next;
		*tail = surface->committed_callbacks;
		surface->committed_callbacks = NULL;
	}

	/* A client's cursor surface is held and told too. */
	surface = server->cursor_surface;
	if (surface != NULL && !surface->dead && surface->current != NULL) {
		kwl_buffer_get(surface->current);
		slot->held[slot->held_count++] = surface->current;

		/* Its frame callbacks after the windows'. */
		tail = &slot->callbacks;
		while (*tail != NULL)
			tail = &(*tail)->callback_next;
		*tail = surface->committed_callbacks;
		surface->committed_callbacks = NULL;
	}
}

/*
 * Gives the render thread the want (design 4.1) when it changed: its kind,
 * and for a frame the heads it needs now.  A ready held for an older want
 * is answered with a cancel, so that the thread looks at the new one; the
 * images it holds stay held.
 */
static void
compose_want(
	struct kwl_server *server,
	enum kwl_render_kind kind)
{
	struct kwl_compose *compose;
	unsigned heads[KWL_RENDER_HEADS];
	unsigned count;
	unsigned index;
	unsigned same;
	uint64_t number;

	/* Nothing to tell without the thread. */
	compose = server->compose;
	if (!compose->render_started)
		return;

	/* The heads a frame needs now. */
	memset(heads, 0, sizeof(heads));
	count = 0U;
	if (kind == KWL_RENDER_FRAME)
		count = kwl_heads_wanted(server, heads, KWL_RENDER_HEADS);

	/* Whether it is the want given last. */
	same = 0U;
	if (kind == compose->want_kind && count == compose->want_head_count) {
		same = 1U;
		for (index = 0U; index < count; index++) {
			if (heads[index] != compose->want_heads[index])
				same = 0U;
		}
	}

	/* The same want as given: nothing to tell. */
	if (same)
		return;

	/* The want, its number, and since when a new number waits. */
	kwl_render_set_want(&compose->render, kind, heads, count, &number);
	if (number != compose->want_number)
		compose->want_ms = kwl_milliseconds();
	compose->want_kind = kind;
	compose->want_number = number;
	compose->want_head_count = count;
	for (index = 0U; index < count; index++)
		compose->want_heads[index] = heads[index];

	/* A ready for another want is cancelled; one for this want (a new list of heads) is kept. */
	if (compose->ready_valid && compose->ready.number != number) {
		kwl_render_answer(&compose->render, compose->ready.number, KWL_RENDER_ANSWER_CANCEL, NULL);
		compose->ready_valid = 0U;
	}
}

/*
 * Tells a wait for the render thread that lasts a second (KWL RENDER
 * stalled): a drawing in flight with no completion, or a want with no
 * ready since it was given (or since the drawing before it completed).
 * The line gives the main thread's view and the thread's, which tells a
 * want lost between them from images or a fence that do not come.  A
 * line a second at first, then one a minute.
 */
static void
compose_stalled(
	struct kwl_server *server)
{
	struct kwl_render_status status;
	struct kwl_compose *compose;
	uint64_t since;
	uint64_t gap;
	uint64_t now;

	/* Since when the main thread waits: the drawing in flight, or the want unanswered. */
	compose = server->compose;
	now = kwl_milliseconds();
	if (compose->in_flight) {
		since = compose->flight_ms;
	} else if (compose->want_kind != KWL_RENDER_NONE && !compose->ready_valid) {
		since = compose->want_ms;
		if (compose->done_ms > since)
			since = compose->done_ms;
	} else {
		compose->stall_lines = 0U;
		return;
	}

	/* A wait shorter than a second is no stall; the next is told from its start. */
	if (now - since < COMPOSE_STALL_MS) {
		compose->stall_lines = 0U;
		return;
	}

	/* A line a second at first, then a line a minute. */
	gap = COMPOSE_STALL_MS;
	if (compose->stall_lines >= COMPOSE_STALL_LINES)
		gap = COMPOSE_STALL_LATER_MS;
	if (compose->stall_lines > 0U && now - compose->stall_ms < gap)
		return;
	compose->stall_ms = now;
	compose->stall_lines++;

	/* Both sides of the pull. */
	kwl_render_status(&compose->render, &status);
	printf(
		"KWL RENDER stalled waited_ms=%llu want=%u number=%llu ready=%u ready_number=%llu in_flight=%u park_depth=%u"
		" | thread want=%u number=%llu answer=%u answer_number=%llu park=%u parked=%u busy=%u held_output=%u"
		" messages=%u last_ready=%llu last_completed=%llu\n",
		(unsigned long long)(now - since),
		(unsigned)compose->want_kind,
		(unsigned long long)compose->want_number,
		compose->ready_valid,
		(unsigned long long)compose->ready.number,
		(unsigned)compose->in_flight,
		compose->park_depth,
		(unsigned)status.want_kind,
		(unsigned long long)status.want_number,
		(unsigned)status.answer,
		(unsigned long long)status.answer_number,
		status.park,
		status.parked,
		status.busy,
		status.held_output,
		status.messages,
		(unsigned long long)status.last_ready,
		(unsigned long long)status.last_completed);
}

/* Applies one message of the render thread. */
static void
compose_message(
	struct kwl_server *server,
	const struct kwl_render_message *message)
{
	struct kwl_compose *compose;
	VkResult result;

	/* By what it says. */
	compose = server->compose;
	switch (message->kind) {
	case KWL_RENDER_READY:
		/* A ready for a want no longer given is cancelled at once. */
		if (message->number != compose->want_number || message->want_kind != compose->want_kind) {
			kwl_render_answer(&compose->render, message->number, KWL_RENDER_ANSWER_CANCEL, NULL);
			break;
		}

		/* Kept for the walk; the time of the acquire (KWL PERF compose, KWL LAT). */
		compose->ready = *message;
		compose->ready.next = NULL;
		compose->ready_valid = 1U;
		server->perf.compose_acquire_cycles += message->acquire_cycles;
		if (server->log_frames && message->want_kind == KWL_RENDER_FRAME)
			printf("KWL LAT acquired frame=%llu at_us=%llu\n", (unsigned long long)server->frame + 1U, (unsigned long long)message->acquired_us);

		/* The loop draws at its next look. */
		break;
	case KWL_RENDER_ACQUIRE_FAILED:
		/* The want went down with it. */
		if (message->number == compose->want_number)
			compose->want_kind = KWL_RENDER_NONE;

		/* The display went (unplugged, or another generation): no frame until the output moves. */
		result = message->result;
		if (result == VK_ERROR_SURFACE_LOST_KHR || result == VK_ERROR_OUT_OF_DATE_KHR) {
			if (!compose->output_lost) {
				printf("KWL OUTPUT lost operation=acquire result=%d\n", (int)result);
				compose->output_lost = 1U;
			}

			/* No failure. */
			break;
		}

		/* Any other failure ends the compositor. */
		printf("KWL VULKAN_ERROR operation=acquire result=%d\n", (int)result);
		kwl_fatal("KWL FAILED site=compose_draw errno=%d operation=acquire result=%d", EIO, (int)result);
		server->failed = 1;
		break;
	case KWL_RENDER_COMPLETED:
		compose->done_ms = kwl_milliseconds();
		compose_completed(server, message);
		break;
	case KWL_RENDER_TASK_DONE:
		compose_task_done(server, message);
		break;
	default:
		break;
	}
}

/*
 * Destroys the images let go whose drawings and task have completed, or
 * every one (all: the device idle at the end); returns how many.
 */
static unsigned
compose_graves(
	struct kwl_compose *compose,
	unsigned all)
{
	struct kwl_grave **link;
	struct kwl_grave *grave;
	unsigned destroyed;
	int ready;

	/* Each grave, in the order let go. */
	destroyed = 0U;
	link = &compose->graves;
	while (*link != NULL) {
		/* Still in use by a drawing or its task given before. */
		grave = *link;
		ready = kwl_compose_task_ready(compose, &grave->record);
		if (!all && (!ready || compose->drawings_done < grave->drawings)) {
			link = &grave->next;
			continue;
		}

		/* Destroyed, and out of the list. */
		*link = grave->next;
		grave->destroy(compose, &grave->record);
		free(grave);
		destroyed++;
	}

	/* Succeeded: the number destroyed. */
	return destroyed;
}

/*
 * Applies a task's completion: its command buffer and fence go, and every
 * task up to it has completed (the images it moved are ready).  A task
 * that could not be submitted or waited for ends the compositor.
 */
static void
compose_task_done(
	struct kwl_server *server,
	const struct kwl_render_message *message)
{
	struct kwl_compose *compose;

	/* The main thread's objects of the task. */
	compose = server->compose;
	vkFreeCommandBuffers(compose->device, compose->task_pool, 1U, &message->task_command);
	vkDestroyFence(compose->device, message->task_fence, NULL);

	/* Tasks complete in their order. */
	if (message->number > compose->task_done)
		compose->task_done = message->number;

	/* Told with the frames' lines (--log-frames). */
	if (server->log_frames)
		printf("KWL TASK done number=%llu result=%d\n", (unsigned long long)message->number, (int)message->result);

	/* A failure is the compositor's. */
	if (message->result != VK_SUCCESS) {
		printf("KWL VULKAN_ERROR operation=task result=%d\n", (int)message->result);
		kwl_fatal("KWL FAILED site=compose_task errno=%d operation=submit result=%d", EIO, (int)message->result);
		server->failed = 1;
	}
}

/*
 * Ends the frame (or the layer's picture) in flight after the render
 * thread completed it: the outcome of its presents, then the buffers it
 * sampled are released (when nothing else holds them) and the frame
 * callbacks of the surfaces it showed are sent.
 */
static void
compose_completed(
	struct kwl_server *server,
	const struct kwl_render_message *message)
{
	const struct kwl_render_outcome *outcome;
	struct kwl_compose *compose;
	struct kwl_frame_slot *slot;
	uint64_t elapsed;
	unsigned index;
	unsigned quiet;

	/* The walk and the replay of the drawing (KWL PERF walk); its slot; one more drawing is done with what it used. */
	compose = server->compose;
	compose->drawings_done++;
	outcome = &message->outcome;
	slot = &compose->slots[message->job.slot % KWL_FRAME_SLOTS];
	slot->flying = 0U;
	quiet = 0U;
	if (message->want_kind == KWL_RENDER_LAYER)
		quiet = 1U;
	compose_walk_timed(server, compose->walk_cycles, outcome->replay_cycles, quiet);

	/* A drawing that could not be recorded, submitted or waited for ends the compositor. */
	if (outcome->result != VK_SUCCESS) {
		compose->scene.valid = 0U;
		if (compose->in_flight > 0U)
			compose->in_flight--;
		slot->bake = 0U;
		kwl_heads_frame_skipped(server);
		printf("KWL VULKAN_ERROR operation=submit result=%d\n", (int)outcome->result);
		kwl_fatal("KWL FAILED site=compose_draw errno=%d operation=submit result=%d", EIO, (int)outcome->result);
		server->failed = 1;
		return;
	}

	/* A frame's presents, its lines, and its part of the render thread's time. */
	if (message->want_kind == KWL_RENDER_FRAME) {
		compose_frame_presented(server, outcome, slot->number);
		server->perf.compose_present_cycles += outcome->run_cycles;
		compose_rec_timed(server, outcome);
	}

	/*
	 * A layer's picture drawn (layer-cache.c): it presented nothing, held
	 * nothing and told no client, so the frame's accounting stays as it was.
	 */
	if (slot->bake) {
		slot->bake = 0U;
		if (compose->in_flight > 0U)
			compose->in_flight--;
		kwl_layers_complete(server, slot->layers_generation);
		return;
	}

	/* The pictures the frame drew in its own commands are taken (layer-cache.c). */
	kwl_layers_complete(server, slot->layers_generation);

	/* Only confirmed command completion publishes the cursor-free captured scene. */
	if (slot->scene_written)
		compose->scene.valid = 1U;
	slot->scene_written = 0U;

	/* The sampled buffers, and the frame callbacks. */
	for (index = 0; index < slot->held_count; index++)
		kwl_buffer_put(slot->held[index]);
	slot->held_count = 0;
	kwl_callbacks_done(&slot->callbacks);
	if (compose->in_flight > 0U)
		compose->in_flight--;

	/* A test image's capture sends the frame it copied (shot.c). */
	kwl_shot_complete(server);
	if (server->log_frames)
		printf("KWL LAT shown frame=%llu at_us=%llu\n", (unsigned long long)slot->number, (unsigned long long)kwl_log_us());

	/* The time from the start of the frame to its completion (reported by KWL PERF). */
	elapsed = kwl_cycles() - slot->start_cycles;
	server->perf.compose_frames++;
	server->perf.compose_cycles += elapsed;

	/* The next frame waits a moment for the windows this one told (half its time, 4 to 50 ms). */
	server->frame_done_ms = kwl_milliseconds();
	server->frame_wait_ms = (server->frame_done_ms - slot->start_ms) / 2U;
	if (server->frame_wait_ms < 4U)
		server->frame_wait_ms = 4U;
	if (server->frame_wait_ms > 50U)
		server->frame_wait_ms = 50U;
}

/*
 * Applies a completed frame's presents: a move's first frame proves the
 * move or sends the output back, a display gone is lost, the
 * heads' presents are applied, and the frame's lines go out.
 */
static void
compose_frame_presented(
	struct kwl_server *server,
	const struct kwl_render_outcome *outcome,
	uint64_t number)
{
	struct kwl_compose *compose;
	VkResult result;

	/* The render thread's times of the frame (KWL LAT). */
	compose = server->compose;
	if (server->log_frames) {
		printf("KWL LAT recorded frame=%llu at_us=%llu\n", (unsigned long long)number, (unsigned long long)outcome->recorded_us);
		printf("KWL LAT queued frame=%llu at_us=%llu\n", (unsigned long long)number, (unsigned long long)outcome->queued_us);
		printf("KWL LAT submit frame=%llu at_us=%llu\n", (unsigned long long)number, (unsigned long long)outcome->presented_us);
	}

	/*
	 * A move's first frame that the display refused (the display
	 * could not be lit): the frame still completed, and the output goes
	 * back to the display it left (output-switch.c) instead of the
	 * compositor ending.
	 */
	result = outcome->present_result;
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

	/* The display went: the output moves. */
	if (result == VK_ERROR_SURFACE_LOST_KHR || result == VK_ERROR_OUT_OF_DATE_KHR) {
		printf("KWL OUTPUT lost operation=present result=%d\n", (int)result);
		if (!compose->output_lost)
			compose->output_lost = 1U;
		result = VK_SUCCESS;
	}

	/* The heads' presents. */
	kwl_heads_presented(server, outcome);

	/* Any other failure of the present is the compositor's. */
	if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
		printf("KWL VULKAN_ERROR operation=present result=%d\n", (int)result);
		kwl_fatal("KWL FAILED site=compose_draw errno=%d operation=present result=%d", EIO, (int)result);
		server->failed = 1;
	}
}

/*
 * Acquires the output's next image for the render thread, waiting at most
 * timeout_ns, with the acquire's time.  The output's swapchain and its
 * acquisition semaphore change only while the thread is parked.
 */
static VkResult
compose_op_acquire_output(
	void *context,
	uint64_t timeout_ns,
	uint32_t *image,
	uint64_t *cycles,
	uint64_t *at_us)
{
	struct kwl_compose *compose;
	uint64_t mark;
	VkResult result;

	/* The next image, and how long that took. */
	compose = context;
	mark = kwl_cycles();
	result = vkAcquireNextImageKHR(compose->device, compose->output.swapchain, timeout_ns, compose->acquired, VK_NULL_HANDLE, image);
	*cycles = kwl_cycles() - mark;
	*at_us = kwl_log_us();

	/* Reports the acquire's result. */
	return result;
}

/*
 * Acquires a head's next image for the render thread, without waiting: the
 * output's frame does not wait for another display.  A head closed since
 * it was wanted is out of date.
 */
static VkResult
compose_op_acquire_head(
	void *context,
	unsigned slot,
	uint32_t *image)
{
	struct kwl_compose *compose;
	struct kwl_head *head;
	VkResult result;

	/* The head, if it is still open (it changes only while the thread is parked). */
	compose = context;
	if (slot >= KWL_HEADS)
		return VK_ERROR_OUT_OF_DATE_KHR;
	head = &compose->heads[slot];
	if (!head->open)
		return VK_ERROR_OUT_OF_DATE_KHR;

	/* Its next image now, or not ready. */
	result = vkAcquireNextImageKHR(compose->device, head->output.swapchain, 0U, head->acquired, VK_NULL_HANDLE, image);

	/* Reports the acquire's result. */
	return result;
}

/*
 * Records the list into the command buffers, submits it and presents the
 * job's images, for the render thread: the host images'
 * copies first in the same submission; a frame waits for the acquired
 * images (the output's, then the heads') and signals their present
 * semaphores.  The outcome says what each step came to and when.
 */
static void
compose_op_run(
	void *context,
	const struct kwl_render_job *job,
	struct kwl_render_outcome *outcome)
{
	struct kwl_compose *compose;
	struct kwl_frame_slot *slot;
	struct kwl_head *head;
	VkSemaphore waits[1U + KWL_HEADS];
	VkPipelineStageFlags stages[1U + KWL_HEADS];
	VkSemaphore signals[1U + KWL_HEADS];
	VkSubmitInfo submit;
	VkPresentInfoKHR present;
	VkCommandBuffer buffers[2];
	unsigned wait_count;
	unsigned signal_count;
	unsigned index;
	uint64_t start;
	uint64_t mark;
	VkResult result;

	/* The list into the command buffers of the job's slot. */
	compose = context;
	slot = &compose->slots[job->slot % KWL_FRAME_SLOTS];
	memset(outcome, 0, sizeof(*outcome));
	start = kwl_cycles();
	result = compose_replay_commands(compose, job->slot % KWL_FRAME_SLOTS, outcome);
	outcome->replay_cycles = kwl_cycles() - start;
	outcome->recorded_us = kwl_log_us();
	outcome->present_result = VK_SUCCESS;
	if (result != VK_SUCCESS) {
		outcome->result = result;
		return;
	}

	/* A frame waits for its acquired images and signals their present semaphores; a layer's picture for nothing. */
	memset(&submit, 0, sizeof(submit));
	submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	wait_count = 0U;
	signal_count = 0U;
	if (job->kind == KWL_RENDER_FRAME) {
		waits[0] = compose->acquired;
		stages[0] = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT;
		signals[0] = compose->rendered[job->output_image];
		wait_count = 1U;
		signal_count = 1U;
		for (index = 0U; index < KWL_HEADS; index++) {
			if (!job->heads[index])
				continue;
			head = &compose->heads[index];
			waits[wait_count] = head->acquired;
			stages[wait_count] = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT;
			wait_count++;
			signals[signal_count] = head->rendered[job->head_images[index]];
			signal_count++;
		}
	}

	/* The waits and the signals. */
	submit.waitSemaphoreCount = wait_count;
	submit.pWaitSemaphores = waits;
	submit.pWaitDstStageMask = stages;
	submit.signalSemaphoreCount = signal_count;
	submit.pSignalSemaphores = signals;

	/*
	 * Host images written up to now, the drawing's own included (a glyph
	 * first drawn, a cursor shape first shown), reach their sampled images
	 * in a command buffer that runs first in the same submission.
	 */
	submit.commandBufferCount = 1U;
	submit.pCommandBuffers = &slot->command;
	if (compose->frame_copies != 0U) {
		buffers[0] = slot->copy_command;
		buffers[1] = slot->command;
		submit.commandBufferCount = 2U;
		submit.pCommandBuffers = buffers;
	}

	/* The submission, timed for KWL PERF rec, and the presents after it, under the queue's lock. */
	kwl_queue_lock(compose);
	mark = kwl_cycles();
	result = vkQueueSubmit(compose->queue, 1U, &submit, slot->fence);
	outcome->submit_cycles = kwl_cycles() - mark;
	outcome->queued_us = kwl_log_us();
	if (result != VK_SUCCESS) {
		kwl_queue_unlock(compose);
		outcome->result = result;
		return;
	}

	/* A layer's picture presents nothing. */
	if (job->kind != KWL_RENDER_FRAME) {
		kwl_queue_unlock(compose);
		outcome->run_cycles = kwl_cycles() - start;
		return;
	}

	/* The output's image goes to the display after the commands; the presents are timed together. */
	mark = kwl_cycles();
	memset(&present, 0, sizeof(present));
	present.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
	present.waitSemaphoreCount = 1U;
	present.pWaitSemaphores = &compose->rendered[job->output_image];
	present.swapchainCount = 1U;
	present.pSwapchains = &compose->output.swapchain;
	present.pImageIndices = &job->output_image;
	outcome->present_result = vkQueuePresentKHR(compose->queue, &present);

	/* Each head's image to its display after the commands. */
	for (index = 0U; index < KWL_HEADS; index++) {
		if (!job->heads[index])
			continue;
		head = &compose->heads[index];
		memset(&present, 0, sizeof(present));
		present.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
		present.waitSemaphoreCount = 1U;
		present.pWaitSemaphores = &head->rendered[job->head_images[index]];
		present.swapchainCount = 1U;
		present.pSwapchains = &head->output.swapchain;
		present.pImageIndices = &job->head_images[index];
		outcome->heads[index] = 1U;
		outcome->head_results[index] = vkQueuePresentKHR(compose->queue, &present);
	}

	/* Lets the queue go once all the presents are made. */
	kwl_queue_unlock(compose);

	/* The present's end, and the render thread's time of the job. */
	outcome->present_cycles = kwl_cycles() - mark;
	outcome->presented_us = kwl_log_us();
	outcome->run_cycles = kwl_cycles() - start;
}

/*
 * Waits at most timeout_ns for the fence of the job in a slot, for the
 * render thread; a signalled fence is reset for the slot's next job.
 */
static VkResult
compose_op_wait(
	void *context,
	unsigned slot,
	uint64_t timeout_ns)
{
	struct kwl_compose *compose;
	VkFence fence;
	VkResult result;

	/* The slot's fence. */
	compose = context;
	fence = compose->slots[slot % KWL_FRAME_SLOTS].fence;
	result = vkWaitForFences(compose->device, 1U, &fence, VK_TRUE, timeout_ns);
	if (result != VK_SUCCESS)
		return result;

	/* Reset for the next job. */
	result = vkResetFences(compose->device, 1U, &fence);
	if (result != VK_SUCCESS)
		return result;

	/* Succeeded: the job completed. */
	return VK_SUCCESS;
}

/* Submits a task's command buffer with its fence, for the render thread, under the queue's lock. */
static VkResult
compose_op_submit_task(
	void *context,
	VkCommandBuffer command,
	VkFence fence)
{
	struct kwl_compose *compose;
	VkSubmitInfo submit;
	VkResult result;

	/* The one command buffer. */
	compose = context;
	memset(&submit, 0, sizeof(submit));
	submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	submit.commandBufferCount = 1U;
	submit.pCommandBuffers = &command;

	/* After every submission before it on the queue, the cursor thread's included. */
	kwl_queue_lock(compose);

	result = vkQueueSubmit(compose->queue, 1U, &submit, fence);

	kwl_queue_unlock(compose);
	if (result != VK_SUCCESS)
		return result;

	/* Succeeded: the fence is signalled when the commands have run. */
	return VK_SUCCESS;
}

/* Waits at most timeout_ns for a task's fence, for the render thread (VK_TIMEOUT while it runs). */
static VkResult
compose_op_wait_task(
	void *context,
	VkFence fence,
	uint64_t timeout_ns)
{
	struct kwl_compose *compose;
	VkResult result;

	/* The fence, which only this task uses. */
	compose = context;
	result = vkWaitForFences(compose->device, 1U, &fence, VK_TRUE, timeout_ns);
	if (result != VK_SUCCESS)
		return result;

	/* Succeeded: the task completed. */
	return VK_SUCCESS;
}

/*
 * Records the list into the frame's command buffers, for the render thread
 * (p004): the copies of the host images it took into
 * copy_command, its vertices of the joined quads into the batch's vertex
 * buffer (no drawing is in flight), and its operations into command.
 */
static VkResult
compose_replay_commands(
	struct kwl_compose *compose,
	unsigned slot,
	struct kwl_render_outcome *outcome)
{
	VkCommandBufferBeginInfo begin;
	uint64_t mark;
	VkResult result;

	/*
	 * Beginning a command buffer resets it (its pool has
	 * VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT), without a call of
	 * its own (a round trip less under Venus).
	 */
	memset(&begin, 0, sizeof(begin));
	begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

	/*
	 * The host images' copies, which run before the drawing in the same
	 * submission.  Most frames copy none, and then copy_command is neither
	 * begun nor ended: each is a round trip to the kernel, and
	 * compose_op_run submits copy_command only when frame_copies says so.
	 */
	compose->frame_copies = 0U;
	if (compose->list.copy_count != 0U) {
		mark = kwl_cycles();
		result = vkBeginCommandBuffer(compose->slots[slot].copy_command, &begin);
		outcome->begin_cycles += kwl_cycles() - mark;
		if (result != VK_SUCCESS)
			return result;

		/* The copies the list took, ended into their own command buffer. */
		compose->frame_copies = kwl_host_images_record(&compose->list, compose->slots[slot].copy_command);
		result = vkEndCommandBuffer(compose->slots[slot].copy_command);
		if (result != VK_SUCCESS)
			return result;

		/* The frame's KWL PERF rec line counts it among those that copied. */
		outcome->copied = 1U;
	}

	/* The joined quads' vertices where the batch's draws read them. */
	if (compose->list.vertex_used != 0U)
		memcpy(compose->batch.map + (size_t)slot * compose->batch.capacity, compose->list.vertices, compose->list.vertex_used);

	/* The drawing's command buffer begun afresh. */
	mark = kwl_cycles();
	result = vkBeginCommandBuffer(compose->slots[slot].command, &begin);
	outcome->begin_cycles += kwl_cycles() - mark;
	if (result != VK_SUCCESS)
		return result;

	/* The drawing's operations, in the order it wrote them. */
	kwl_list_replay(&compose->list, compose->slots[slot].command);

	/* The End sends the encoded operations, which the kernel decodes before it answers. */
	mark = kwl_cycles();
	result = vkEndCommandBuffer(compose->slots[slot].command);
	outcome->end_cycles = kwl_cycles() - mark;
	if (result != VK_SUCCESS)
		return result;

	/* Succeeded: the command buffers are ready to submit. */
	return VK_SUCCESS;
}

/* Adds a frame's render-thread steps to the window's KWL PERF rec line. */
static void
compose_rec_timed(
	struct kwl_server *server,
	const struct kwl_render_outcome *outcome)
{
	struct kwl_perf *perf;
	uint64_t encode;

	/* The encoding is the replay less its Begins and its End (and a copy's End, which is short). */
	perf = &server->perf;
	encode = 0U;
	if (outcome->replay_cycles > outcome->begin_cycles + outcome->end_cycles)
		encode = outcome->replay_cycles - outcome->begin_cycles - outcome->end_cycles;

	/* The window's sums. */
	perf->rec_frames++;
	perf->rec_copied += outcome->copied;
	perf->rec_begin_cycles += outcome->begin_cycles;
	perf->rec_encode_cycles += encode;
	perf->rec_end_cycles += outcome->end_cycles;
	perf->rec_submit_cycles += outcome->submit_cycles;
	perf->rec_present_cycles += outcome->present_cycles;

	/* The window's longest End, submit and presents. */
	if (outcome->end_cycles > perf->rec_end_max_cycles)
		perf->rec_end_max_cycles = outcome->end_cycles;
	if (outcome->submit_cycles > perf->rec_submit_max_cycles)
		perf->rec_submit_max_cycles = outcome->submit_cycles;
	if (outcome->present_cycles > perf->rec_present_max_cycles)
		perf->rec_present_max_cycles = outcome->present_cycles;
}
