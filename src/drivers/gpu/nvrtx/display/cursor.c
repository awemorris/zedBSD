/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The cursor plane of the nvrtx GPU device's display (the gpu-device stage).
 *
 * The taken-over head's hardware cursor shows a 64 by 64 A8R8G8B8 image with
 * premultiplied alpha over the window, at a position the cursor's own PIO
 * channel takes (scanout.c: the core's cursor methods and the cursor channel's
 * position and update).  The image is copied into one of two slots of a buffer
 * object in the VRAM, so a shown cursor keeps reading the old slot until the
 * core's update that names the new one has finished; a new image while that
 * update is still out is refused with EBUSY for that moment.  A move of a shown
 * cursor only writes the cursor channel and never waits.
 *
 * Every operation checks the open's lease first; the lease's end hides the
 * cursor and forgets its image.  The state is the node's, under its lock.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <drivers/gpu/gpu.h>
#include <drivers/gpu/gpu-display.h>
#include <kern/kcrt.h>
#include <kern/kmem.h>
#include <uapi/errno.h>
#include <uapi/gpu.h>
#include <uapi/gpu-display.h>

#include "drivers/gpu/nvrtx/nvrtx-node.h"

/* The two image slots, each one cursor image on a 256-byte boundary, in one object on a 64 KiB boundary. */
#define CURSOR_SLOTS			2U
#define CURSOR_SLOT_BYTES		NVRTX_DISP_CURSOR_BYTES
#define CURSOR_SLOT_PIXELS		(NVRTX_DISP_CURSOR_SIDE * NVRTX_DISP_CURSOR_SIDE)
#define CURSOR_OBJECT_ALIGN		0x10000ULL

/*
 * How long a show or a hide waits for the core's previous cursor change to
 * finish before it sends its own: 50 ms, about three frames at 60 Hz.
 */
#define CURSOR_SETTLE_US		50000U

/* The bits of a byte, for putting a pixel's four bytes together. */
#define CURSOR_BYTE_BITS		8U

/* The bytes of a pixel; rows and an image start on this many are read a word at a time. */
#define CURSOR_PIXEL_BYTES		4U

static int cursor_check(struct nvrtx_node *node, void *session, uint64_t lease, uint64_t generation);
static int cursor_read(struct nvrtx_node *node, const struct nvrtx_resource *resource, const struct gpu_display_cursor_image *request, uint32_t *pixels);
static void cursor_write(struct nvrtx_node *node, const uint32_t *pixels, unsigned slot);
static uint64_t cursor_slot_offset(const struct nvrtx_node *node, unsigned slot);
static uint32_t cursor_read_pixel(const volatile uint8_t *source);

/*
 * Makes the cursor plane: the two slots' VRAM, cleared (transparent), the
 * CPU's copy of them, and the head's cursor channel.  Runs before the device is registered; on a
 * failure the display simply has no cursor plane (GPU_DISPLAY_HAS_CURSOR
 * is not set and the cursor operations answer EOPNOTSUPP).
 */
void
nvrtx_cursor_start(
	struct nvrtx_node *node)
{
	struct nvrtx_gpu *gpu;
	volatile uint8_t *cpu;
	uint64_t at;
	int error;

	gpu = node->gpu;

	/* The CPU's copy of the slots, which a set compares a new image with. */
	node->cursor_pixels = kern_calloc((size_t)CURSOR_SLOTS * CURSOR_SLOT_PIXELS, sizeof(*node->cursor_pixels));
	if (node->cursor_pixels == NULL) {
		nvrtx_stage_mark("gpu-device: no cursor plane (memory)");
		return;
	}

	/* Takes the slots' VRAM and maps it for the CPU's copies. */
	error = nvrtx_bo_alloc(gpu, (uint64_t)CURSOR_SLOTS * CURSOR_SLOT_BYTES, CURSOR_OBJECT_ALIGN, &node->cursor_object);
	if (error == 0)
		error = nvrtx_bo_map(gpu, &node->cursor_object);
	if (error != 0) {
		nvrtx_bo_free(gpu, &node->cursor_object);
		kern_free(node->cursor_pixels);
		node->cursor_pixels = NULL;
		nvrtx_stage_mark("gpu-device: no cursor plane (VRAM %d)", error);
		return;
	}

	/* Clears both slots: a transparent image. */
	cpu = nvrtx_bo_cpu(gpu, &node->cursor_object);
	for (at = 0U; at < (uint64_t)CURSOR_SLOTS * CURSOR_SLOT_BYTES; at += 4U)
		*(volatile uint32_t *)(volatile void *)(cpu + at) = 0U;

	/* Makes the head's cursor channel through the resource manager. */
	error = nvrtx_scanout_cursor_start(gpu);
	if (error != 0) {
		nvrtx_bo_free(gpu, &node->cursor_object);
		kern_free(node->cursor_pixels);
		node->cursor_pixels = NULL;
		nvrtx_stage_mark("gpu-device: no cursor plane (channel %d)", error);
		return;
	}

	/* Succeeded: the display has a cursor plane, hidden and without an image. */
	node->cursor_slot = 0U;
	node->cursor_ready = 1;
	nvrtx_stage_mark("gpu-device ok: cursor plane %ux%u at %llx",
	    (unsigned)NVRTX_DISP_CURSOR_SIDE,
	    (unsigned)NVRTX_DISP_CURSOR_SIDE,
	    (unsigned long long)node->cursor_object.vram);
}

/*
 * Hides the cursor at the end of a lease and forgets its image; the caller
 * holds the node's lock.  A hide that fails is only shown: the cursor may
 * then stay on the screen until the next lease sets an image and hides it.
 */
void
nvrtx_cursor_lease_end(
	struct nvrtx_node *node)
{
	int error;

	/* Nothing to hide without a cursor plane. */
	if (node->cursor_ready == 0)
		return;

	/* Hides a shown cursor once the core's previous cursor change has finished. */
	if (node->cursor_visible != 0) {
		error = nvrtx_scanout_core_settled(node->gpu, CURSOR_SETTLE_US);
		if (error == 0)
			error = nvrtx_scanout_cursor_clear(node->gpu);
		if (error == 0) {
			node->cursor_visible = 0;
		} else {
			nvrtx_stage_mark("gpu-device: the cursor was not hidden at the lease's end (%d)", error);
		}
	}

	/* The next lease starts without an image. */
	node->cursor_has_image = 0;
}

/*
 * Sets the cursor's image from the lease holder's storage (the display
 * cursor set operation): the rows are read at once, converted to A8R8G8B8,
 * the rest of the 64 by 64 slot transparent.  An image equal to the one
 * the cursor's slot holds changes nothing (an application presents the
 * same image again and again while the cursor only moves); another is
 * written into the slot the shown cursor does not read, and a shown cursor
 * switches to it from a coming frame on; a hidden one stays hidden.
 *
 * Returns 0, EOPNOTSUPP without a cursor plane, EBUSY for another open's
 * lease or, for a new image, while the core's previous cursor change is
 * still out, ESTALE for another generation, or EIO when the display is
 * broken or a channel did not take the change.
 */
int
nvrtx_cursor_set(
	void *private_data,
	void *session,
	void *object,
	const struct gpu_display_cursor_image *request)
{
	struct nvrtx_node *node;
	const struct nvrtx_resource *resource;
	uint32_t *staged;
	const uint32_t *shown;
	uint64_t started;
	unsigned slot;
	int compared;
	int same;
	int error;

	node = private_data;
	resource = object;

	/* The plane exists from the start or never; this does not change. */
	if (node->cursor_ready == 0)
		return EOPNOTSUPP;

	/* Checks the lease and changes the cursor under the node's lock, counting the time it is held. */
	mutex_lock(&node->lock);

	started = nvrtx_now_us();
	nvrtx_node_count(node, &node->counts.cursor_sets, 1U);
	error = cursor_check(node, session, request->lease, request->generation);
	if (error != 0) {
		mutex_unlock(&node->lock);
		return error;
	}

	/* The image goes into the CPU's copy of the slot the shown cursor does not read. */
	slot = node->cursor_slot ^ 1U;
	staged = node->cursor_pixels + (size_t)slot * CURSOR_SLOT_PIXELS;
	shown = node->cursor_pixels + (size_t)node->cursor_slot * CURSOR_SLOT_PIXELS;
	error = cursor_read(node, resource, request, staged);
	if (error != 0) {
		mutex_unlock(&node->lock);
		return error;
	}

	/* The image the cursor's slot already holds needs nothing sent, even while the core's last change is out. */
	compared = kern_memcmp(staged, shown, CURSOR_SLOT_BYTES);
	same = 0;
	if (node->cursor_has_image != 0 && compared == 0)
		same = 1;
	if (same != 0) {
		nvrtx_node_count(node, &node->counts.cursor_same, 1U);
		nvrtx_node_count(node, &node->counts.cursor_set_us, nvrtx_now_us() - started);
		mutex_unlock(&node->lock);
		return 0;
	}

	/* The other slot is free once no core update that may still name it is out. */
	error = nvrtx_scanout_core_settled(node->gpu, 0U);
	if (error != 0) {
		mutex_unlock(&node->lock);
		return error;
	}

	/* Writes the image into that slot. */
	cursor_write(node, staged, slot);

	/* A shown cursor is moved to the new slot by the core. */
	if (node->cursor_visible != 0) {
		error = nvrtx_scanout_cursor_show(node->gpu, cursor_slot_offset(node, slot), node->cursor_point);
		if (error != 0) {
			mutex_unlock(&node->lock);
			return EIO;
		}
	}

	/* The new slot is the image from now on. */
	node->cursor_slot = slot;
	node->cursor_has_image = 1;
	nvrtx_node_count(node, &node->counts.cursor_set_us, nvrtx_now_us() - started);

	mutex_unlock(&node->lock);

	/* Succeeded: the image is taken; the storage is no longer read. */
	return 0;
}

/*
 * Moves the cursor's top left to a position of the output (the display
 * cursor move operation), showing a hidden cursor.  A shown cursor's move
 * writes only the cursor channel; showing one waits for the core's
 * previous cursor change (at most 50 ms) before the core's own.
 *
 * Returns 0, EOPNOTSUPP without a cursor plane, EINVAL when the lease set
 * no image, EBUSY for another open's lease or when the previous change did
 * not finish, ESTALE for another generation, or EIO.
 */
int
nvrtx_cursor_move(
	void *private_data,
	void *session,
	const struct gpu_display_cursor_move *request)
{
	struct nvrtx_node *node;
	uint64_t started;
	uint32_t point;
	int error;

	node = private_data;

	/* The plane exists from the start or never; this does not change. */
	if (node->cursor_ready == 0)
		return EOPNOTSUPP;

	/* Checks the lease and moves the cursor under the node's lock, counting the time it is held. */
	mutex_lock(&node->lock);

	started = nvrtx_now_us();
	nvrtx_node_count(node, &node->counts.cursor_moves, 1U);
	error = cursor_check(node, session, request->lease, request->generation);
	if (error != 0) {
		mutex_unlock(&node->lock);
		return error;
	}

	/* Only an image the lease set can be shown. */
	if (node->cursor_has_image == 0) {
		mutex_unlock(&node->lock);
		return EINVAL;
	}

	/* The position word the cursor channel takes. */
	point = nvrtx_disp_cursor_point(request->x, request->y);

	/* A shown cursor only moves; a hidden one is shown at the position. */
	if (node->cursor_visible != 0) {
		error = nvrtx_scanout_cursor_point(node->gpu, point);
	} else {
		error = nvrtx_scanout_core_settled(node->gpu, CURSOR_SETTLE_US);
		if (error == 0)
			error = nvrtx_scanout_cursor_show(node->gpu, cursor_slot_offset(node, node->cursor_slot), point);
		if (error == 0)
			node->cursor_visible = 1;
	}

	/* A previous change still out is EBUSY for a moment; anything else broke the cursor. */
	if (error == EBUSY) {
		mutex_unlock(&node->lock);
		return EBUSY;
	}

	if (error != 0) {
		mutex_unlock(&node->lock);
		return EIO;
	}

	/* The position is the cursor's from now on. */
	node->cursor_point = point;
	nvrtx_node_count(node, &node->counts.cursor_move_us, nvrtx_now_us() - started);

	mutex_unlock(&node->lock);

	/* Succeeded: the cursor is at the position from a coming frame on. */
	return 0;
}

/*
 * Hides the cursor and keeps its image (the display cursor hide operation).
 *
 * Returns 0 (also for a cursor already hidden), EOPNOTSUPP without a
 * cursor plane, EBUSY for another open's lease or when the previous change
 * did not finish, ESTALE for another generation, or EIO.
 */
int
nvrtx_cursor_hide(
	void *private_data,
	void *session,
	const struct gpu_display_cursor_hide *request)
{
	struct nvrtx_node *node;
	int error;

	node = private_data;

	/* The plane exists from the start or never; this does not change. */
	if (node->cursor_ready == 0)
		return EOPNOTSUPP;

	/* Checks the lease and hides the cursor under the node's lock. */
	mutex_lock(&node->lock);

	error = cursor_check(node, session, request->lease, request->generation);
	if (error != 0) {
		mutex_unlock(&node->lock);
		return error;
	}

	/* A shown cursor is taken off by the core, once its previous cursor change has finished. */
	error = 0;
	if (node->cursor_visible != 0) {
		error = nvrtx_scanout_core_settled(node->gpu, CURSOR_SETTLE_US);
		if (error == 0)
			error = nvrtx_scanout_cursor_clear(node->gpu);
		if (error == 0)
			node->cursor_visible = 0;
	}

	mutex_unlock(&node->lock);

	/* A previous change still out is EBUSY for a moment; anything else broke the cursor. */
	if (error == EBUSY)
		return EBUSY;
	if (error != 0)
		return EIO;

	/* Succeeded: the cursor is gone from a coming frame on. */
	return 0;
}

/*
 * Checks that the session holds the lease, of this generation, on a display
 * that works; the caller holds the node's lock.
 */
static int
cursor_check(
	struct nvrtx_node *node,
	void *session,
	uint64_t lease,
	uint64_t generation)
{
	/* Only the lease's holder changes the cursor. */
	if (node->owner != session || node->lease != lease)
		return EBUSY;

	/* Only the one generation exists. */
	if (generation != NVRTX_NODE_GENERATION)
		return ESTALE;

	/* A broken display takes no change. */
	if (node->broken != 0)
		return EIO;

	/* The session may change the cursor. */
	return 0;
}

/*
 * Reads a cursor image's rows into the CPU's copy of a slot (64 by 64
 * words), as A8R8G8B8, with every pixel outside the image transparent.  An
 * image whose start and rows lie on whole pixels is read a word at a time
 * (a blob's are reads through the window, each one a bus round trip; the
 * word is the pixel's four bytes in order on the little-endian CPUs the
 * driver runs on).  The
 * core checked the rows lie inside the storage.  Returns the error of a blob
 * that cannot take its place in the window.
 */
static int
cursor_read(
	struct nvrtx_node *node,
	const struct nvrtx_resource *resource,
	const struct gpu_display_cursor_image *request,
	uint32_t *pixels)
{
	const volatile uint8_t *source;
	const volatile uint8_t *at;
	volatile uint8_t *view;
	uint32_t pixel;
	uint32_t row;
	uint32_t column;
	int words;
	int rgba;
	int error;

	/* The image's first byte: in RAM for storage, in the window for a blob (mapped now if it is not). */
	if (resource->kind == NVRTX_RESOURCE_STORAGE) {
		source = resource->cpu + request->offset;
	} else {
		error = nvrtx_node_backing_cpu(node, resource->backing, &view);
		if (error != 0)
			return error;
		source = view + request->offset;
	}

	/* Whether every pixel can be read as one word, and whether red and blue change places. */
	words = 0;
	if ((request->offset % CURSOR_PIXEL_BYTES) == 0U && (request->stride % CURSOR_PIXEL_BYTES) == 0U)
		words = 1;
	rgba = 0;
	if (request->format == GPU_PIXEL_RGBA8888)
		rgba = 1;

	/* Every pixel of the slot: the image's where it has one, transparent (premultiplied zero) elsewhere. */
	for (row = 0U; row < NVRTX_DISP_CURSOR_SIDE; row++) {
		for (column = 0U; column < NVRTX_DISP_CURSOR_SIDE; column++) {
			pixel = 0U;
			if (row < request->height && column < request->width) {
				at = source + (uint64_t)row * request->stride + (uint64_t)column * CURSOR_PIXEL_BYTES;
				if (words != 0) {
					pixel = *(const volatile uint32_t *)(const volatile void *)at;
				} else {
					pixel = cursor_read_pixel(at);
				}
			}

			pixels[row * NVRTX_DISP_CURSOR_SIDE + column] = nvrtx_disp_cursor_pixel(pixel, rgba);
		}
	}

	/* Succeeded: the CPU's copy holds the image. */
	return 0;
}

/*
 * Writes the CPU's copy of a slot (64 by 64 words) into that slot through
 * the CPU's window.
 */
static void
cursor_write(
	struct nvrtx_node *node,
	const uint32_t *pixels,
	unsigned slot)
{
	volatile uint8_t *slot_base;
	volatile uint32_t *target;
	uint32_t index;

	/* The slot's first word. */
	slot_base = nvrtx_bo_cpu(node->gpu, &node->cursor_object) + (uint64_t)slot * CURSOR_SLOT_BYTES;
	target = (volatile uint32_t *)(volatile void *)slot_base;

	/* Every word of the slot. */
	for (index = 0U; index < CURSOR_SLOT_PIXELS; index++)
		target[index] = pixels[index];

	/* Reads the slot back once, so the writes have reached the VRAM before the core names it. */
	(void)target[0];
}

/* Reports where a slot lies in the VRAM (a multiple of 256). */
static uint64_t
cursor_slot_offset(
	const struct nvrtx_node *node,
	unsigned slot)
{
	/* The object's start and the slot's place in it. */
	return node->cursor_object.vram + (uint64_t)slot * CURSOR_SLOT_BYTES;
}

/* Reads one pixel's four bytes as a little-endian word (the image need not be word-aligned). */
static uint32_t
cursor_read_pixel(
	const volatile uint8_t *source)
{
	uint32_t pixel;

	/* The first byte is the lowest. */
	pixel = source[0];
	pixel |= (uint32_t)source[1] << CURSOR_BYTE_BITS;
	pixel |= (uint32_t)source[2] << (2U * CURSOR_BYTE_BITS);
	pixel |= (uint32_t)source[3] << (3U * CURSOR_BYTE_BITS);

	/* Succeeded: the pixel. */
	return pixel;
}
