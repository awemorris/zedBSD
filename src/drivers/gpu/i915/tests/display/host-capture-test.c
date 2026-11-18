/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The host test of the capture display's layout (display/capture.c, the
 * test build's I915_TEST_CAPTURE): the header offsets the host harness
 * parses, the slot placement and rotation, the ready protocol, and the mode
 * and frame checks.
 *
 *   sh run-xd-host-tests.sh
 *
 * capture.c is compiled only with I915_TEST_CAPTURE, and the host build
 * compiles the display sources without it, so this file includes it with
 * the macro set.  The link drops the node operations, which need the GPU.
 */

#define I915_TEST_CAPTURE 1

#include "host-test.h"

#include "../../display/capture.c"

#include <stdio.h>
#include <stdlib.h>

static void i915_host_capture_layout(void);
static void i915_host_capture_headers(uint8_t *area);
static void i915_host_capture_rotation(uint8_t *area);
static void i915_host_capture_checks(void);

/*
 * Checks the capture area's layout, headers, rotation and checks.
 */
int
main(void)
{
	uint8_t *area;
	int status;

	/* The documented offsets and sizes. */
	i915_host_capture_layout();

	/* A zeroed area as large as the kernel's. */
	area = calloc(1U, (size_t)I915_CAPTURE_AREA_BYTES);
	if (area == NULL) {
		printf("capture_host_test: no memory for the area\n");
		return 2;
	}

	/* The headers of a new area, then six captures through the four slots. */
	i915_host_capture_headers(area);
	i915_host_capture_rotation(area);
	free(area);

	/* The mode and frame checks. */
	i915_host_capture_checks();

	status = i915_host_report("capture_host_test");
	if (status != 0)
		return status;

	/* Succeeded: the layout is the one the host harness parses. */
	return 0;
}

/*
 * Stands in for drv_i915_gfx_rect(): the GPU copy is not modelled on the
 * host, and only the capture display's presentation reaches it.
 */
int
drv_i915_gfx_rect(
	struct i915_render_session *session,
	const struct i915_gfx_surface *dst,
	const struct i915_gfx_rect *dst_rect,
	const struct i915_gfx_surface *src,
	const struct i915_gfx_rect *src_rect,
	const uint32_t clear[4],
	int linear)
{
	UNUSED_PARAMETER(session);
	UNUSED_PARAMETER(dst);
	UNUSED_PARAMETER(dst_rect);
	UNUSED_PARAMETER(src);
	UNUSED_PARAMETER(src_rect);
	UNUSED_PARAMETER(clear);
	UNUSED_PARAMETER(linear);

	/* Ends the test: the tested path reached a service the host does not have. */
	printf("capture_host_test: unreachable: drv_i915_gfx_rect\n");
	exit(2);
}

/* Checks the header offsets and the area's sizes against the layout comment of capture.c. */
static void
i915_host_capture_layout(void)
{
	/* The global header. */
	i915_host_check(offsetof(struct i915_capture_global, magic) == 0x00U &&
			offsetof(struct i915_capture_global, version) == 0x08U &&
			offsetof(struct i915_capture_global, header_bytes) == 0x0cU &&
			offsetof(struct i915_capture_global, slot_count) == 0x10U &&
			offsetof(struct i915_capture_global, pixel_offset) == 0x14U &&
			offsetof(struct i915_capture_global, slot_bytes) == 0x18U &&
			offsetof(struct i915_capture_global, first_slot_offset) == 0x20U &&
			offsetof(struct i915_capture_global, total_bytes) == 0x28U &&
			offsetof(struct i915_capture_global, base) == 0x30U &&
			offsetof(struct i915_capture_global, max_width) == 0x38U &&
			offsetof(struct i915_capture_global, max_height) == 0x3cU &&
			offsetof(struct i915_capture_global, write_count) == 0x40U &&
			offsetof(struct i915_capture_global, last_slot) == 0x48U &&
			offsetof(struct i915_capture_global, reserved) == 0x4cU &&
			sizeof(struct i915_capture_global) == 0x50U,
			"LAYOUT: the global header's offsets (0x00 magic .. 0x48 last_slot, 0x50 bytes)");

	/* A slot header. */
	i915_host_check(offsetof(struct i915_capture_slot, magic) == 0x00U &&
			offsetof(struct i915_capture_slot, sequence) == 0x08U &&
			offsetof(struct i915_capture_slot, width) == 0x10U &&
			offsetof(struct i915_capture_slot, height) == 0x14U &&
			offsetof(struct i915_capture_slot, stride_bytes) == 0x18U &&
			offsetof(struct i915_capture_slot, format) == 0x1cU &&
			offsetof(struct i915_capture_slot, display_id) == 0x20U &&
			offsetof(struct i915_capture_slot, plane_index) == 0x24U &&
			offsetof(struct i915_capture_slot, generation) == 0x28U &&
			offsetof(struct i915_capture_slot, lease) == 0x30U &&
			offsetof(struct i915_capture_slot, lease_sequence) == 0x38U &&
			offsetof(struct i915_capture_slot, present_time_ns) == 0x40U &&
			offsetof(struct i915_capture_slot, hash_kind) == 0x48U &&
			offsetof(struct i915_capture_slot, slot_index) == 0x4cU &&
			offsetof(struct i915_capture_slot, hash) == 0x50U &&
			offsetof(struct i915_capture_slot, ready) == 0x58U &&
			sizeof(struct i915_capture_slot) == 0x60U,
			"LAYOUT: the slot header's offsets (0x00 magic .. 0x58 ready, 0x60 bytes)");

	/* The sizes: 1920 x 1080 x 4 is 2025 whole pages. */
	i915_host_check(I915_CAPTURE_PIXEL_BYTES == 8294400U &&
			I915_CAPTURE_SLOT_BYTES == 8298496U &&
			I915_CAPTURE_AREA_BYTES == 33198080U,
			"LAYOUT: pixels 8294400, slot 8298496, area 33198080 bytes");

	/* Every slot follows the global header and the previous slot; the last ends with the area. */
	i915_host_check(drv_i915_capture_slot_offset(0U) == 4096U &&
			drv_i915_capture_slot_offset(1U) == 4096U + 8298496U &&
			drv_i915_capture_slot_offset(3U) + I915_CAPTURE_SLOT_BYTES == I915_CAPTURE_AREA_BYTES,
			"LAYOUT: slot k at 4096 + k * slot_bytes; slot 3 ends the area");

	/* Every slot and its pixels start on a page. */
	i915_host_check((drv_i915_capture_slot_offset(2U) % 4096U) == 0U &&
			((drv_i915_capture_slot_offset(2U) + I915_CAPTURE_PIXEL_OFFSET) % 4096U) == 0U,
			"LAYOUT: slots and their pixels are page aligned");
}

/* Checks the headers of a new area. */
static void
i915_host_capture_headers(
	uint8_t *area)
{
	const struct i915_capture_global *global;
	const struct i915_capture_slot *header;
	unsigned slot;
	int named;
	int magic;

	global = (const struct i915_capture_global *)(const void *)area;

	/* Writes the headers of an area at a sample guest address. */
	drv_i915_capture_init_header(area, 0x123456000ULL);

	/* The global header names the layout and holds no capture. */
	magic = memcmp(area, "I915CAP1", 8U);
	i915_host_check(magic == 0 &&
			global->version == 1U &&
			global->header_bytes == 4096U &&
			global->slot_count == 4U &&
			global->pixel_offset == 4096U &&
			global->slot_bytes == 8298496U &&
			global->first_slot_offset == 4096U &&
			global->total_bytes == 33198080U &&
			global->base == 0x123456000ULL &&
			global->max_width == 1920U &&
			global->max_height == 1080U &&
			global->write_count == 0U &&
			global->last_slot == 0xffffffffU,
			"HEADER: I915CAP1 v1, 4 slots of 8298496, base, 1920x1080, no capture (last_slot ~0)");

	/* Every slot is named, knows its index and is not ready. */
	named = 1;
	for (slot = 0U; slot < I915_CAPTURE_SLOTS; slot++) {
		header = (const struct i915_capture_slot *)(const void *)(area + drv_i915_capture_slot_offset(slot));
		magic = memcmp(header->magic, "I915SLOT", 8U);
		if (magic != 0 || header->slot_index != slot || header->ready != 0U)
			named = 0;
	}

	i915_host_check(named, "HEADER: every slot is I915SLOT with its index and ready 0");
}

/* Runs six captures through the four slots and checks each slot header and the global header. */
static void
i915_host_capture_rotation(
	uint8_t *area)
{
	const struct i915_capture_global *global;
	const struct i915_capture_slot *header;
	struct i915_capture_frame frame;
	uint64_t write_count;
	uint64_t newest;
	unsigned newest_slot;
	unsigned slot;
	int rotated;
	int opened;

	global = (const struct i915_capture_global *)(const void *)area;

	/* Six captures: slots 0, 1, 2, 3, 0, 1, each opened before and closed after its copy. */
	rotated = 1;
	opened = 1;
	for (write_count = 0U; write_count < 6U; write_count++) {
		slot = drv_i915_capture_next_slot(write_count);
		if (slot != (unsigned)(write_count % 4U))
			rotated = 0;

		/* The slot a capture reuses is not ready while it is written. */
		drv_i915_capture_slot_open(area, slot);
		header = (const struct i915_capture_slot *)(const void *)(area + drv_i915_capture_slot_offset(slot));
		if (header->ready != 0U)
			opened = 0;

		memset(&frame, 0, sizeof(frame));
		frame.sequence = write_count + 1U;
		frame.width = 640U + (uint32_t)write_count;
		frame.height = 480U;
		frame.stride_bytes = 2624U;
		frame.format = 2U;
		frame.lease = 7U;
		frame.lease_sequence = write_count + 1U;
		frame.present_time_ns = 1000000U * (write_count + 1U);
		drv_i915_capture_slot_close(area, slot, &frame);
	}

	i915_host_check(rotated, "ROTATION: capture n goes to slot (n - 1) mod 4");
	i915_host_check(opened, "ROTATION: an opened slot reads ready 0 until it is closed");

	/* The newest capture is the sixth, in slot 1. */
	i915_host_check(global->write_count == 6U && global->last_slot == 1U, "ROTATION: write_count 6, last_slot 1");

	/* Slot 1 holds capture 6 with every field; ready equals the sequence. */
	header = (const struct i915_capture_slot *)(const void *)(area + drv_i915_capture_slot_offset(1U));
	i915_host_check(header->sequence == 6U &&
			header->ready == 6U &&
			header->width == 645U &&
			header->height == 480U &&
			header->stride_bytes == 2624U &&
			header->format == 2U &&
			header->display_id == 1U &&
			header->plane_index == 0U &&
			header->generation == 1U &&
			header->lease == 7U &&
			header->lease_sequence == 6U &&
			header->present_time_ns == 6000000U &&
			header->hash_kind == 0U &&
			header->slot_index == 1U,
			"ROTATION: slot 1 holds capture 6 (645x480, stride 2624, RGBA, display 1 plane 0 gen 1, lease 7), ready 6");

	/* Slot 3 still holds capture 4, and the newest by ready is slot 1. */
	newest = 0U;
	newest_slot = I915_CAPTURE_NO_SLOT;
	for (slot = 0U; slot < I915_CAPTURE_SLOTS; slot++) {
		header = (const struct i915_capture_slot *)(const void *)(area + drv_i915_capture_slot_offset(slot));
		if (header->ready == header->sequence && header->ready > newest) {
			newest = header->ready;
			newest_slot = slot;
		}
	}

	header = (const struct i915_capture_slot *)(const void *)(area + drv_i915_capture_slot_offset(3U));
	i915_host_check(header->ready == 4U && newest == 6U && newest_slot == 1U,
			"ROTATION: slot 3 keeps capture 4; the slot headers alone name slot 1 as the newest");
}

/* Checks the mode validation and the frame check. */
static void
i915_host_capture_checks(void)
{
	uint32_t refresh;
	uint32_t stride;
	int error;

	/* A zero refresh is given 60 Hz. */
	refresh = 0U;
	error = drv_i915_capture_check_mode(1920U, 1080U, &refresh);
	i915_host_check(error == 0 && refresh == 60000U, "MODE: 1920x1080 at 0 is accepted at 60000 mHz");

	/* A smaller extent at 60 Hz is accepted. */
	refresh = 60000U;
	error = drv_i915_capture_check_mode(800U, 600U, &refresh);
	i915_host_check(error == 0 && refresh == 60000U, "MODE: 800x600 at 60000 mHz is accepted");

	/* Another refresh is refused. */
	refresh = 59940U;
	error = drv_i915_capture_check_mode(800U, 600U, &refresh);
	i915_host_check(error == EINVAL, "MODE: 59940 mHz is refused");

	/* A wider or taller extent is refused. */
	refresh = 0U;
	error = drv_i915_capture_check_mode(1921U, 1080U, &refresh);
	i915_host_check(error == EINVAL, "MODE: 1921 wide is refused");
	refresh = 0U;
	error = drv_i915_capture_check_mode(1920U, 1081U, &refresh);
	i915_host_check(error == EINVAL, "MODE: 1081 high is refused");

	/* A full frame's rows are 7680 bytes; a 100-pixel row is rounded up to 448. */
	stride = 0U;
	error = drv_i915_capture_check_frame(1920U, 1080U, &stride);
	i915_host_check(error == 0 && stride == 7680U, "FRAME: 1920x1080 rows of 7680 bytes");
	stride = 0U;
	error = drv_i915_capture_check_frame(100U, 1U, &stride);
	i915_host_check(error == 0 && stride == 448U, "FRAME: a 100-pixel row takes 448 bytes (64-byte rows)");

	/* The largest frame fits the slot's pixels. */
	i915_host_check((uint64_t)7680U * 1080U <= I915_CAPTURE_PIXEL_BYTES, "FRAME: the largest frame fits a slot");

	/* Empty and oversized frames are refused. */
	error = drv_i915_capture_check_frame(0U, 10U, &stride);
	i915_host_check(error == EINVAL, "FRAME: an empty frame is refused");
	error = drv_i915_capture_check_frame(1921U, 10U, &stride);
	i915_host_check(error == EINVAL, "FRAME: 1921 wide is refused");
	error = drv_i915_capture_check_frame(10U, 1081U, &stride);
	i915_host_check(error == EINVAL, "FRAME: 1081 high is refused");
}
