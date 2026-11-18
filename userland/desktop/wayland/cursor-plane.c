/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The cursor plane (ws141-p014): when the output's display has a small
 * VK_KHR_display plane above the output's, with per-pixel premultiplied
 * alpha, the cursor is presented on it instead of being composed into the
 * frames.  A move then presents the plane's image again at a new place
 * (VkDisplayPresentInfoKHR's dstRect, its hotspot subtracted); a new cursor
 * image is copied into the plane's swapchain image first.  Only standard
 * Vulkan is used: an output without such a plane composes the cursor as
 * before.  A drag, a client's cursor without a host-written image or larger
 * than the plane, and a pointer on another display are composed too.
 *
 * Since ws213-p004 the cursor thread (cursor-thread.c) presents the plane
 * from the cursor's box, where the input thread writes the pointer's
 * place.  The plane's Vulkan objects are made and destroyed apart from
 * the main thread's part (kwl_cursor_plane_make and _stop, which the
 * display thread calls while an output opens or closes, display-thread.c);
 * the main thread here adopts and lets go of them, chooses the image the
 * plane shows at each pass of its loop and writes
 * it to the box: a cursor shape or the arrow as it is (made once, never
 * written again), a client's cursor through one of the plane's two copies
 * (design 4.7), which the main thread writes only while the cursor
 * thread does not read it.
 */

#include "compose.h"
#include "cursor-thread.h"
#include "extras.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* The refresh assumed when the display told none, in millihertz. */
#define PLANE_REFRESH_DEFAULT_MHZ	60000U

/*
 * How much sooner than a whole refresh after the last present a held move
 * may be presented, in microseconds (ws211-p006): the clock reads whole
 * milliseconds on the Pi 4, so a hold of exactly a refresh woke a
 * millisecond or two late, after the pointer's next report (8 ms apart),
 * and presented every 20 to 22 ms (45 a second) instead of every refresh.
 */
#define PLANE_HOLD_SLACK_US		2500U

/* How long the main thread waits for the cursor thread to park, in milliseconds (design 4.8). */
#define PLANE_PARK_WAIT_MS		2000U

static int plane_state(struct kwl_server *server, const struct kwl_import **image, int32_t *hotspot_x, int32_t *hotspot_y);
static VkResult plane_find(struct kwl_compose *compose, const struct vkdemo_display *output, VkDisplayModeKHR *mode, uint32_t *index, uint32_t *stack, VkExtent2D *extent);
static VkResult plane_mode(struct kwl_compose *compose, const struct vkdemo_display *output, VkDisplayModeKHR *mode);
static VkResult plane_objects(struct kwl_compose *compose, struct kwl_cursor_thread *cursor);
static void plane_objects_destroy(struct kwl_compose *compose, struct kwl_cursor_thread *cursor);
static VkResult plane_copies_create(struct kwl_compose *compose, struct kwl_cursor_plane *plane);
static void plane_copies_destroy(struct kwl_compose *compose, struct kwl_cursor_plane *plane);
static int plane_fixed(const struct kwl_server *server, const struct kwl_import *image);
static const struct kwl_import *plane_copy(struct kwl_server *server, struct kwl_cursor_plane *plane, const struct kwl_import *image);
static void plane_publish(struct kwl_server *server, struct kwl_cursor_plane *plane, const struct kwl_import *image, int32_t hotspot_x, int32_t hotspot_y, unsigned show);
static int plane_ready(struct kwl_server *server, const struct kwl_cursor_plane *plane, const struct kwl_import *image);
static uint64_t plane_period_us(const struct kwl_server *server);
static uint64_t plane_hold_us(const struct kwl_server *server);

/*
 * Makes the cursor plane's Vulkan objects for an output's swapchain
 * (output, made already): a small premultiplied plane of the output's
 * display and mode, its surface and swapchain, and the cursor thread's
 * command pool, buffers, fences and semaphores, in a new record of the
 * cursor thread that has nothing of the main thread's yet.  It touches no
 * field of the compositor's, so the display thread may call it (design
 * 4.3).  Returns the record, with the plane's extent and index, or NULL
 * when the output has no such plane or a step failed (logged, nothing
 * kept).
 */
struct kwl_cursor_thread *
kwl_cursor_plane_make(
	struct kwl_compose *compose,
	const struct vkdemo_display *output,
	VkExtent2D *extent,
	uint32_t *index)
{
	struct kwl_cursor_thread *cursor;
	VkDisplaySurfaceCreateInfoKHR create;
	VkDisplayModeKHR mode;
	uint32_t stack;
	VkResult result;

	/* A small premultiplied plane of the output's display and mode, or none. */
	result = plane_find(compose, output, &mode, index, &stack, extent);
	if (result != VK_SUCCESS)
		return NULL;

	/* The cursor thread's record, which owns the presentation's objects. */
	cursor = calloc(1, sizeof(*cursor));
	if (cursor == NULL)
		return NULL;

	/* Its surface, with per-pixel premultiplied alpha. */
	memset(&create, 0, sizeof(create));
	create.sType = VK_STRUCTURE_TYPE_DISPLAY_SURFACE_CREATE_INFO_KHR;
	create.displayMode = mode;
	create.planeIndex = *index;
	create.planeStackIndex = stack;
	create.transform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
	create.globalAlpha = 1.0f;
	create.alphaMode = VK_DISPLAY_PLANE_ALPHA_PER_PIXEL_PREMULTIPLIED_BIT_KHR;
	create.imageExtent = *extent;
	cursor->output.width = extent->width;
	cursor->output.height = extent->height;
	result = vkCreateDisplayPlaneSurfaceKHR(compose->instance, &create, NULL, &cursor->output.surface);
	if (result != VK_SUCCESS) {
		printf("KWL CURSOR_PLANE unavailable step=surface result=%d\n", (int)result);
		free(cursor);
		return NULL;
	}

	/*
	 * Its swapchain, its images filled by copies only: no colour
	 * attachment, or libvulkan's fast path of a move (ws141-p016 B-u)
	 * never applies (main 2ef0688a8).  Then the thread's own command pool,
	 * buffers, fences and semaphores.
	 */
	result = vkdemo_display_create_swapchain_exact(compose->physical, compose->device, compose->family, &cursor->output, VK_IMAGE_USAGE_TRANSFER_DST_BIT);
	if (result == VK_SUCCESS && cursor->output.image_count > KWL_CURSOR_THREAD_IMAGES)
		result = VK_ERROR_INITIALIZATION_FAILED;
	if (result == VK_SUCCESS)
		result = plane_objects(compose, cursor);
	if (result != VK_SUCCESS) {
		printf("KWL CURSOR_PLANE unavailable step=swapchain result=%d\n", (int)result);
		plane_objects_destroy(compose, cursor);
		vkdemo_display_close(compose->instance, compose->device, &cursor->output);
		free(cursor);
		return NULL;
	}

	/* Succeeded: the record, for the main thread to adopt. */
	return cursor;
}

/*
 * Adopts a cursor thread's record made by kwl_cursor_plane_make for the
 * output open now, on the main thread: the plane's two copies of a
 * client's cursor, the box, and the thread, which presents from now on.
 * Any failure closes the plane, and the cursor is composed.
 */
void
kwl_cursor_plane_adopt(
	struct kwl_server *server,
	struct kwl_cursor_thread *cursor,
	VkExtent2D extent,
	uint32_t index)
{
	struct kwl_compose *compose;
	struct kwl_cursor_plane *plane;
	unsigned copy;
	VkResult result;
	int error;

	/* The main thread's side of the record. */
	compose = server->compose;
	plane = &compose->cursor_plane;
	memset(plane, 0, sizeof(*plane));
	cursor->box = &server->cursor_box;
	cursor->instance = compose->instance;
	cursor->device = compose->device;
	cursor->queue = compose->queue;
	cursor->queue_lock = &compose->queue_lock;
	cursor->wake_main = server->wake[1];
	cursor->log = server->log_frames;
	cursor->hold_us = plane_hold_us(server);
	plane->thread = cursor;
	plane->width = extent.width;
	plane->height = extent.height;

	/* The two copies of a client's cursor. */
	result = plane_copies_create(compose, plane);
	if (result != VK_SUCCESS) {
		printf("KWL CURSOR_PLANE unavailable step=swapchain result=%d\n", (int)result);
		plane->open = 1;
		kwl_cursor_plane_close(server);
		return;
	}

	/* The box shows nothing until the main thread's next pass, and knows the copies. */
	pthread_mutex_lock(&server->cursor_box.lock);

	server->cursor_box.image = NULL;
	server->cursor_box.show = 0U;
	server->cursor_box.failed = 0U;
	server->cursor_box.stop = 0U;
	server->cursor_box.park = 0U;
	server->cursor_box.parked = 0U;
	for (copy = 0; copy < KWL_CURSOR_COPIES; copy++) {
		server->cursor_box.copies[copy] = plane->copies[copy];
		server->cursor_box.pins[copy] = 0U;
		cursor->copies[copy] = plane->copies[copy];
	}

	pthread_mutex_unlock(&server->cursor_box.lock);

	/* The thread presents from now on. */
	plane->open = 1;
	error = kwl_cursor_thread_start(cursor);
	if (error != 0) {
		printf("KWL CURSOR_PLANE unavailable step=thread errno=%d\n", error);
		kwl_cursor_plane_close(server);
		return;
	}

	/* Succeeded: the next pass shows the cursor on it. */
	printf("KWL CURSOR_PLANE open plane=%u width=%u height=%u images=%u format=%d\n", index, extent.width, extent.height, cursor->output.image_count, (int)cursor->output.format);
}

/*
 * Stops the cursor thread and destroys the plane's objects (before the
 * output's): the main thread's part before (kwl_cursor_plane_detach), the
 * thread's stop and the plane's Vulkan objects (kwl_cursor_plane_stop),
 * then the main thread's part after (kwl_cursor_plane_detached), all here.
 */
void
kwl_cursor_plane_close(
	struct kwl_server *server)
{
	struct kwl_cursor_parts parts;
	int error;

	/* A closed plane has nothing. */
	kwl_cursor_plane_detach(server, &parts);
	if (parts.thread == NULL)
		return;

	/* The thread, then the objects. */
	error = kwl_cursor_plane_stop(server->compose, parts.thread);
	kwl_cursor_plane_detached(server, &parts, error);
}

/*
 * Takes the cursor plane off the output on the main thread, for its stop
 * (kwl_cursor_plane_stop, which the display thread may call): the plane is
 * closed from now on for the main thread (the frames draw the cursor), and
 * parts keeps the cursor thread's record and the plane's copies, which the
 * thread reads until it has stopped.  parts->thread is NULL for a closed
 * plane.
 */
void
kwl_cursor_plane_detach(
	struct kwl_server *server,
	struct kwl_cursor_parts *parts)
{
	struct kwl_cursor_plane *plane;
	unsigned index;

	/* A closed plane has nothing. */
	memset(parts, 0, sizeof(*parts));
	plane = &server->compose->cursor_plane;
	if (!plane->open)
		return;

	/* The record and the copies leave the plane. */
	parts->thread = plane->thread;
	parts->shown = plane->shown;
	for (index = 0; index < KWL_CURSOR_COPIES; index++)
		parts->copies[index] = plane->copies[index];
	memset(plane, 0, sizeof(*plane));
}

/*
 * Stops a cursor thread taken off its plane and destroys the plane's
 * Vulkan objects once the device is idle (no copy or present of the plane
 * may still run): its commands, fences, semaphores and pool, then its
 * swapchain and surface.  It touches no field of the compositor's, so the
 * display thread may call it.  Returns 0, or ETIMEDOUT when a copy did not
 * finish (the objects are kept: no Vulkan call is safe under the thread).
 */
int
kwl_cursor_plane_stop(
	struct kwl_compose *compose,
	struct kwl_cursor_thread *cursor)
{
	int error;

	/* The thread ends after its copies. */
	error = kwl_cursor_thread_stop(cursor);
	if (error != 0)
		return error;

	/* No copy or present of the plane may still run. */
	kwl_device_wait_idle(compose);

	/* The commands, fences, semaphores and pool, then the swapchain and surface. */
	plane_objects_destroy(compose, cursor);
	vkdemo_display_close(compose->instance, compose->device, &cursor->output);

	/* Succeeded: nothing of the plane's Vulkan is left but the copies. */
	return 0;
}

/*
 * Finishes a plane's close on the main thread, after its thread stopped
 * (error is kwl_cursor_plane_stop's; a copy that did not finish ends the
 * compositor at once): the copies and the record go, and the
 * box shows nothing and has no copies.
 */
void
kwl_cursor_plane_detached(
	struct kwl_server *server,
	struct kwl_cursor_parts *parts,
	int error)
{
	struct kwl_cursor_plane copies;
	unsigned index;

	/* A thread whose copies did not finish leaves nothing safe. */
	if (error != 0) {
		kwl_fatal("KWL FAILED site=park errno=%d thread=cursor step=stop", error);
		_exit(1);
	}

	/* The copies, which nothing reads any more. */
	memset(&copies, 0, sizeof(copies));
	for (index = 0; index < KWL_CURSOR_COPIES; index++)
		copies.copies[index] = parts->copies[index];
	plane_copies_destroy(server->compose, &copies);

	/* The box shows nothing and has no copies. */
	pthread_mutex_lock(&server->cursor_box.lock);

	server->cursor_box.image = NULL;
	server->cursor_box.show = 0U;
	server->cursor_box.failed = 0U;
	server->cursor_box.stop = 0U;
	server->cursor_box.park = 0U;
	server->cursor_box.parked = 0U;
	for (index = 0; index < KWL_CURSOR_COPIES; index++) {
		server->cursor_box.copies[index] = NULL;
		server->cursor_box.pins[index] = 0U;
	}

	pthread_mutex_unlock(&server->cursor_box.lock);

	/* A frame shows the cursor again. */
	if (parts->shown)
		server->dirty = 1;
	free(parts->thread);
	memset(parts, 0, sizeof(*parts));
	printf("KWL CURSOR_PLANE closed\n");
}

/*
 * Tells whether the cursor plane shows the cursor as it is now, so that a pointer move damages no frame.
 */
int
kwl_cursor_plane_takes(
	struct kwl_server *server)
{
	const struct kwl_import *image;
	int32_t hotspot_x;
	int32_t hotspot_y;
	int state;

	/* Only an open plane, for a pointer on the anchor. */
	if (server->compose == NULL || !server->compose->cursor_plane.open)
		return 0;
	if (server->pointer_output != KWL_PLANE_ANCHOR)
		return 0;

	/* That can show the cursor as it is. */
	state = plane_state(server, &image, &hotspot_x, &hotspot_y);
	if (state < 0)
		return 0;

	/* Succeeded: the plane shows it. */
	return 1;
}

/*
 * Brings the cursor plane up to date at a pass of the event loop: the
 * image it shows, written to the box for the cursor thread (the place is
 * the input thread's).
 *
 * A change between the plane and the composed cursor redraws the frame,
 * which draws the cursor or leaves it out.  A cursor thread that failed
 * closes the plane, and the cursor is composed.
 */
void
kwl_cursor_plane_update(
	struct kwl_server *server)
{
	struct kwl_cursor_plane *plane;
	const struct kwl_import *image;
	const struct kwl_import *shown_image;
	int32_t hotspot_x;
	int32_t hotspot_y;
	unsigned failed;
	unsigned shown;
	int fixed;
	int ready;
	int state;

	/* An output without the plane composes the cursor. */
	if (server->compose == NULL || !server->compose->cursor_plane.open || !server->compose->output_open)
		return;
	plane = &server->compose->cursor_plane;

	/* A failed thread has stopped presenting: the plane closes, and frames draw the cursor. */
	pthread_mutex_lock(&server->cursor_box.lock);

	failed = server->cursor_box.failed;

	pthread_mutex_unlock(&server->cursor_box.lock);
	if (failed) {
		kwl_cursor_plane_close(server);
		return;
	}

	/* The cursor as the frames would draw it. */
	state = plane_state(server, &image, &hotspot_x, &hotspot_y);

	/*
	 * A composed cursor leaves the plane empty, published before a parked
	 * thread looks at the box again, so that it never presents the image
	 * of before the park once more (WS217, review-2 L24).
	 */
	if (state < 0)
		plane_publish(server, plane, NULL, 0, 0, 0U);

	/* A park for the game mode or a sleep's request ends with this pass. */
	if (plane->parked) {
		(void)kwl_cursor_thread_park(&server->cursor_box, 0U, 0U);
		plane->parked = 0U;
	}

	/* Moving between the plane and composing redraws the frame with or without the cursor. */
	shown = 0U;
	if (state >= 0 && server->pointer_output == KWL_PLANE_ANCHOR)
		shown = 1U;
	if (shown != plane->shown) {
		plane->shown = shown;
		server->dirty = 1;
	}

	/* A composed cursor leaves the plane empty (published above). */
	if (state < 0)
		return;

	/* No cursor shows: the plane is empty where it was. */
	if (state == 0) {
		plane_publish(server, plane, NULL, 0, 0, 1U);
		return;
	}

	/*
	 * An image whose move to its layout has not completed yet is not given
	 * to the cursor thread, which reads it apart from the render thread's
	 * order (nor are the copies written before theirs has): the box keeps
	 * what it shows until a later pass.
	 */
	ready = plane_ready(server, plane, image);
	if (!ready)
		return;

	/* A shape or the arrow as it is; a client's cursor through a copy, unless both are in use now. */
	shown_image = image;
	fixed = plane_fixed(server, image);
	if (!fixed) {
		shown_image = plane_copy(server, plane, image);
		if (shown_image == NULL) {
			/* The cursor thread wakes the event loop when it lets a copy go. */
			return;
		}
	}

	/* The box shows it from the cursor thread's next look. */
	plane_publish(server, plane, shown_image, hotspot_x, hotspot_y, 1U);
}

/*
 * Parks the cursor thread while the scheduler shows nothing (the game mode,
 * a sleep's request, a mode switch waiting): the plane is not presented
 * then.  The next kwl_cursor_plane_update ends the park.  A thread that
 * does not park in time ends the compositor (design 4.8).
 */
void
kwl_cursor_plane_hold(
	struct kwl_server *server)
{
	struct kwl_cursor_plane *plane;
	int error;

	/* Only an open plane not parked yet. */
	if (server->compose == NULL || !server->compose->cursor_plane.open)
		return;
	plane = &server->compose->cursor_plane;
	if (plane->parked)
		return;

	/* The thread's answer, or the end. */
	error = kwl_cursor_thread_park(&server->cursor_box, 1U, PLANE_PARK_WAIT_MS);
	if (error != 0) {
		kwl_fatal("KWL FAILED site=park errno=%d thread=cursor step=hold", error);
		_exit(1);
	}

	/* Parked until the next update. */
	plane->parked = 1U;
}

/*
 * Finds what the plane shows: 1 with an image and its hotspot, 0 for
 * nothing, -1 when the cursor must be composed (a drag, an image the
 * plane cannot show, or a rotated output).  The output the pointer is on
 * is the cursor thread's to judge, from the box.
 */
static int
plane_state(
	struct kwl_server *server,
	const struct kwl_import **image,
	int32_t *hotspot_x,
	int32_t *hotspot_y)
{
	struct kwl_cursor_plane *plane;
	int state;

	/*
	 * A rotated output composes the cursor (WS217): the plane is not
	 * turned with the picture, by the hardware's 180 or the turn pass.
	 */
	plane = &server->compose->cursor_plane;
	if (server->compose->rotation != KWL_ROTATE_0)
		return -1;

	/* The cursor as the frames would draw it. */
	state = kwl_compose_cursor_source(server, image, hotspot_x, hotspot_y);
	if (state <= 0)
		return state;

	/* Only a host-written image (the arrow, the shapes, a wl_shm cursor) that fits the plane. */
	if ((*image)->staging == VK_NULL_HANDLE ||
	    (*image)->map == NULL ||
	    (*image)->width > plane->width ||
	    (*image)->height > plane->height)
		return -1;

	/* Succeeded: the plane shows this image. */
	return 1;
}
/* Finds a plane of the output's display and mode (output's size) with per-pixel premultiplied alpha and a small image, above the output's. */
static VkResult
plane_find(
	struct kwl_compose *compose,
	const struct vkdemo_display *output,
	VkDisplayModeKHR *mode,
	uint32_t *index,
	uint32_t *stack,
	VkExtent2D *extent)
{
	VkDisplayPlanePropertiesKHR properties[8];
	VkDisplayPlaneCapabilitiesKHR capabilities;
	VkDisplayKHR displays[8];
	uint32_t count;
	uint32_t display_count;
	uint32_t plane;
	uint32_t at;
	VkResult result;
	int matches;

	/* The mode the output shows. */
	result = plane_mode(compose, output, mode);
	if (result != VK_SUCCESS)
		return result;

	/* Every plane; a display with more than this many is looked at only so far. */
	count = 8U;
	result = vkGetPhysicalDeviceDisplayPlanePropertiesKHR(compose->physical, &count, properties);
	if (result != VK_SUCCESS && result != VK_INCOMPLETE)
		return result;

	/* The first plane that fits. */
	for (plane = 0; plane < count; plane++) {
		/* One that drives the output's display. */
		display_count = 8U;
		result = vkGetDisplayPlaneSupportedDisplaysKHR(compose->physical, plane, &display_count, displays);
		if (result != VK_SUCCESS && result != VK_INCOMPLETE)
			continue;
		matches = 0;
		for (at = 0; at < display_count; at++) {
			if (displays[at] == compose->display)
				matches = 1;
		}
		if (!matches)
			continue;

		/* With per-pixel premultiplied alpha, a small image and places partly off the output. */
		result = vkGetDisplayPlaneCapabilitiesKHR(compose->physical, *mode, plane, &capabilities);
		if (result != VK_SUCCESS)
			continue;
		if ((capabilities.supportedAlpha & VK_DISPLAY_PLANE_ALPHA_PER_PIXEL_PREMULTIPLIED_BIT_KHR) == 0U)
			continue;
		if (capabilities.maxSrcExtent.width < 32U || capabilities.maxSrcExtent.width > 256U ||
		    capabilities.maxSrcExtent.height < 32U || capabilities.maxSrcExtent.height > 256U)
			continue;
		if (capabilities.minDstPosition.x >= 0 || capabilities.minDstPosition.y >= 0)
			continue;

		/* At most 64 pixels square, which every cursor image fits. */
		*index = plane;
		*stack = properties[plane].currentStackIndex;
		extent->width = capabilities.maxSrcExtent.width < 64U ? capabilities.maxSrcExtent.width : 64U;
		extent->height = capabilities.maxSrcExtent.height < 64U ? capabilities.maxSrcExtent.height : 64U;
		return VK_SUCCESS;
	}

	/* No plane fits: the cursor is composed. */
	return VK_ERROR_INITIALIZATION_FAILED;
}


/* Finds the mode of the output's display with the size of the output's swapchain (output). */
static VkResult
plane_mode(
	struct kwl_compose *compose,
	const struct vkdemo_display *output,
	VkDisplayModeKHR *mode)
{
	VkDisplayModePropertiesKHR modes[16];
	uint32_t count;
	uint32_t index;
	VkResult result;

	/* The display's modes, as many as fit. */
	count = 16U;
	result = vkGetDisplayModePropertiesKHR(compose->physical, compose->display, &count, modes);
	if (result != VK_SUCCESS && result != VK_INCOMPLETE)
		return result;

	/* The first of the output's size. */
	for (index = 0; index < count; index++) {
		if (modes[index].parameters.visibleRegion.width == output->width &&
		    modes[index].parameters.visibleRegion.height == output->height) {
			*mode = modes[index].displayMode;
			return VK_SUCCESS;
		}
	}

	/* None: no plane is looked for. */
	return VK_ERROR_INITIALIZATION_FAILED;
}


/*
 * Makes the cursor thread's own command pool, and a command buffer, fence
 * and semaphore for each swapchain image, and the acquisitions'
 * semaphores.  The pool is the thread's alone, not the frames' (review-4
 * H-C): Vulkan requires a pool to be used by one thread at a time.
 */
static VkResult
plane_objects(
	struct kwl_compose *compose,
	struct kwl_cursor_thread *cursor)
{
	VkCommandPoolCreateInfo pool;
	VkCommandBufferAllocateInfo allocate;
	VkFenceCreateInfo fence;
	VkSemaphoreCreateInfo semaphore;
	unsigned index;
	VkResult result;

	/* The pool, its buffers reset one at a time. */
	memset(&pool, 0, sizeof(pool));
	pool.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
	pool.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
	pool.queueFamilyIndex = compose->family;
	result = vkCreateCommandPool(compose->device, &pool, NULL, &cursor->pool);
	if (result != VK_SUCCESS)
		return result;

	/* One of each per image; the fences start signaled, as no copy is in flight. */
	memset(&allocate, 0, sizeof(allocate));
	allocate.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
	allocate.commandPool = cursor->pool;
	allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	allocate.commandBufferCount = 1U;
	memset(&fence, 0, sizeof(fence));
	fence.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
	fence.flags = VK_FENCE_CREATE_SIGNALED_BIT;
	memset(&semaphore, 0, sizeof(semaphore));
	semaphore.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
	for (index = 0; index < cursor->output.image_count; index++) {
		result = vkAllocateCommandBuffers(compose->device, &allocate, &cursor->commands[index]);
		if (result == VK_SUCCESS)
			result = vkCreateFence(compose->device, &fence, NULL, &cursor->fences[index]);
		if (result == VK_SUCCESS)
			result = vkCreateSemaphore(compose->device, &semaphore, NULL, &cursor->ready[index]);
		if (result != VK_SUCCESS)
			return result;
	}

	/* One more acquisition semaphore than images, so a free one is always there. */
	for (index = 0; index < cursor->output.image_count + 1U; index++) {
		result = vkCreateSemaphore(compose->device, &semaphore, NULL, &cursor->acquired[index]);
		if (result != VK_SUCCESS)
			return result;
	}

	/* Succeeded: the plane can be presented. */
	return VK_SUCCESS;
}

/* Destroys the cursor thread's command buffers, fences, semaphores and pool, once the thread has stopped. */
static void
plane_objects_destroy(
	struct kwl_compose *compose,
	struct kwl_cursor_thread *cursor)
{
	unsigned index;

	/* Each image's. */
	for (index = 0; index < KWL_CURSOR_THREAD_IMAGES; index++) {
		if (cursor->commands[index] != VK_NULL_HANDLE)
			vkFreeCommandBuffers(compose->device, cursor->pool, 1U, &cursor->commands[index]);
		if (cursor->fences[index] != VK_NULL_HANDLE)
			vkDestroyFence(compose->device, cursor->fences[index], NULL);
		if (cursor->ready[index] != VK_NULL_HANDLE)
			vkDestroySemaphore(compose->device, cursor->ready[index], NULL);
	}

	/* The acquisitions'. */
	for (index = 0; index < KWL_CURSOR_THREAD_IMAGES + 1U; index++) {
		if (cursor->acquired[index] != VK_NULL_HANDLE)
			vkDestroySemaphore(compose->device, cursor->acquired[index], NULL);
	}

	/* The pool. */
	if (cursor->pool != VK_NULL_HANDLE)
		vkDestroyCommandPool(compose->device, cursor->pool, NULL);
}

/*
 * Makes the plane's two copies of a client's cursor: host-written images
 * of the plane's size, transparent (design 4.7).  They are never marked
 * written, so no frame copies them: only the cursor thread reads their
 * staging.
 */
static VkResult
plane_copies_create(
	struct kwl_compose *compose,
	struct kwl_cursor_plane *plane)
{
	struct kwl_import *copy;
	unsigned index;
	uint32_t row;
	VkResult result;

	/* Each copy, its rows cleared. */
	for (index = 0; index < KWL_CURSOR_COPIES; index++) {
		copy = calloc(1, sizeof(*copy));
		if (copy == NULL)
			return VK_ERROR_OUT_OF_HOST_MEMORY;
		plane->copies[index] = copy;
		result = kwl_host_image_create(compose, plane->width, plane->height, compose->sampler, copy);
		if (result != VK_SUCCESS)
			return result;
		for (row = 0; row < plane->height; row++)
			memset((unsigned char *)copy->map + (size_t)row * copy->row_pitch, 0, (size_t)plane->width * 4U);
		copy->draw = KWL_DRAW_ALPHA;
	}

	/* Succeeded: both copies can be written. */
	return VK_SUCCESS;
}

/* Releases the plane's two copies, once the cursor thread has stopped. */
static void
plane_copies_destroy(
	struct kwl_compose *compose,
	struct kwl_cursor_plane *plane)
{
	unsigned index;

	/* Each copy made. */
	for (index = 0; index < KWL_CURSOR_COPIES; index++) {
		if (plane->copies[index] == NULL)
			continue;
		kwl_host_image_release(compose, plane->copies[index]);
		free(plane->copies[index]);
		plane->copies[index] = NULL;
	}
}

/* Tells whether an image is the compositor's own cursor (the arrow, a shape), made once and never written again. */
static int
plane_fixed(
	const struct kwl_server *server,
	const struct kwl_import *image)
{
	unsigned index;

	/* The arrow. */
	if (image == server->arrow)
		return 1;

	/* A shape. */
	for (index = 0; index < KWL_CURSOR_IMAGES; index++) {
		if (image == server->cursor_images[index])
			return 1;
	}

	/* A client's. */
	return 0;
}

/*
 * Gives the plane's copy that holds a client's cursor image as it is now:
 * the copy already written with it, or the other copy written now.  Only a
 * copy the box does not show and the cursor thread does not read is
 * written (design 4.7 3); when neither is, NULL, and a later pass tries
 * again.  The copy is written whole, transparent beyond the image.
 */
static const struct kwl_import *
plane_copy(
	struct kwl_server *server,
	struct kwl_cursor_plane *plane,
	const struct kwl_import *image)
{
	struct kwl_import *copy;
	const unsigned char *source;
	unsigned char *target;
	unsigned candidate;
	unsigned chosen;
	uint32_t row;

	/* The copy that already holds this version. */
	if (plane->copy_valid &&
	    plane->copy_source == image &&
	    plane->copy_generation == image->generation &&
	    plane->copy_version == image->version)
		return plane->copies[plane->copy_index];

	/* A copy neither shown nor read now. */
	chosen = KWL_CURSOR_COPIES;
	pthread_mutex_lock(&server->cursor_box.lock);

	for (candidate = 0; candidate < KWL_CURSOR_COPIES; candidate++) {
		if (server->cursor_box.image == plane->copies[candidate])
			continue;
		if (server->cursor_box.pins[candidate] != 0U)
			continue;
		chosen = candidate;
		break;
	}

	/*
	 * Both in use: the cursor thread wakes the event loop when it lets go
	 * of a copy, and that pass writes it.
	 */
	server->cursor_box.copy_wanted = 0U;
	if (chosen == KWL_CURSOR_COPIES)
		server->cursor_box.copy_wanted = 1U;

	pthread_mutex_unlock(&server->cursor_box.lock);

	/* Both are in use: the pass after the cursor thread's wake. */
	if (chosen == KWL_CURSOR_COPIES)
		return NULL;

	/* The image's rows, and nothing beyond them (a smaller image after a larger one). */
	copy = plane->copies[chosen];
	source = image->map;
	target = copy->map;
	for (row = 0; row < plane->height; row++) {
		memset(target + (size_t)row * copy->row_pitch, 0, (size_t)plane->width * 4U);
		if (row < image->height)
			memcpy(target + (size_t)row * copy->row_pitch, source + (size_t)row * image->row_pitch, (size_t)image->width * 4U);
	}

	/* A new version of the copy, which the cursor thread copies again. */
	copy->version++;
	plane->copy_valid = 1U;
	plane->copy_source = image;
	plane->copy_generation = image->generation;
	plane->copy_version = image->version;
	plane->copy_index = chosen;

	/* Succeeded: the copy holds the image now. */
	return copy;
}

/* Tells whether an image and the plane's copies have completed their moves to their layout, so the cursor thread may read them; 1 when they have. */
static int
plane_ready(
	struct kwl_server *server,
	const struct kwl_cursor_plane *plane,
	const struct kwl_import *image)
{
	unsigned index;
	int ready;

	/* The image itself. */
	ready = kwl_compose_task_ready(server->compose, image);
	if (!ready)
		return 0;

	/* Each copy a client's cursor goes through. */
	for (index = 0; index < KWL_CURSOR_COPIES; index++) {
		ready = kwl_compose_task_ready(server->compose, plane->copies[index]);
		if (!ready)
			return 0;
	}

	/* Succeeded: every one is in its layout. */
	return 1;
}

/* Writes what the plane shows to the box when it changed, and wakes the cursor thread. */
static void
plane_publish(
	struct kwl_server *server,
	struct kwl_cursor_plane *plane,
	const struct kwl_import *image,
	int32_t hotspot_x,
	int32_t hotspot_y,
	unsigned show)
{
	uint64_t generation;
	uint32_t version;

	/* The image's identity now. */
	generation = 0U;
	version = 0U;
	if (image != NULL) {
		generation = image->generation;
		version = image->version;
	}

	/* The box already says so. */
	if (plane->published &&
	    plane->published_image == image &&
	    plane->published_generation == generation &&
	    plane->published_version == version &&
	    plane->published_hotspot_x == hotspot_x &&
	    plane->published_hotspot_y == hotspot_y &&
	    plane->published_show == show)
		return;

	/* The box, and the cursor thread woken. */
	pthread_mutex_lock(&server->cursor_box.lock);

	server->cursor_box.image = image;
	server->cursor_box.image_generation = generation;
	server->cursor_box.image_version = version;
	server->cursor_box.hotspot_x = hotspot_x;
	server->cursor_box.hotspot_y = hotspot_y;
	server->cursor_box.show = show;
	server->cursor_box.sequence++;
	pthread_cond_broadcast(&server->cursor_box.changed);

	pthread_mutex_unlock(&server->cursor_box.lock);

	/* What the box was given. */
	plane->published = 1U;
	plane->published_image = image;
	plane->published_generation = generation;
	plane->published_version = version;
	plane->published_hotspot_x = hotspot_x;
	plane->published_hotspot_y = hotspot_y;
	plane->published_show = show;
}

/* Gives the output's refresh period in microseconds: the display's mode, or 60 Hz when it told none. */
static uint64_t
plane_period_us(
	const struct kwl_server *server)
{
	uint64_t refresh;

	/* The mode's refresh in millihertz, which wl_output tells the clients too. */
	refresh = server->refresh;
	if (refresh == 0U)
		refresh = PLANE_REFRESH_DEFAULT_MHZ;

	/* Succeeded: one refresh, in microseconds. */
	return 1000000000ULL / refresh;
}

/* Gives how long a move of the same image is held after a present: a refresh, less the slack of the clock. */
static uint64_t
plane_hold_us(
	const struct kwl_server *server)
{
	uint64_t period;

	/* A refresh too short for the slack is held for half of it. */
	period = plane_period_us(server);
	if (period <= 2U * PLANE_HOLD_SLACK_US)
		return period / 2U;

	/* Succeeded: the refresh less the slack. */
	return period - PLANE_HOLD_SLACK_US;
}
