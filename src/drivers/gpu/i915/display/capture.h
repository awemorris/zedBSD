/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The capture display (capture.c, I915_TEST_CAPTURE).
 *
 * The capture display leaves the display hardware alone and shows every
 * presentation in an area of RAM a reader outside the kernel reads.  The
 * layout of that area is the contract with that reader; capture.c
 * describes it in full.  Only the capture build and the host test include
 * this header.
 */

#ifndef DRIVERS_GPU_I915_DISPLAY_CAPTURE_H
#define DRIVERS_GPU_I915_DISPLAY_CAPTURE_H

#include <stdint.h>

struct drv_gpu_ops;
struct i915_device;

/* The fixed mode of the virtual display: 1920x1080 at 60 Hz, a 13.3-inch 16:9 panel. */
#define I915_CAPTURE_WIDTH		1920U
#define I915_CAPTURE_HEIGHT		1080U
#define I915_CAPTURE_REFRESH_MILLIHZ	60000U
#define I915_CAPTURE_WIDTH_MM		294U
#define I915_CAPTURE_HEIGHT_MM		165U

/* The layout version the global header states. */
#define I915_CAPTURE_VERSION		1U

/* How many slots the area rotates through. */
#define I915_CAPTURE_SLOTS		4U

/* The global header, a slot header, and where a slot's pixels start. */
#define I915_CAPTURE_HEADER_BYTES	4096U
#define I915_CAPTURE_PIXEL_OFFSET	4096U

/* The bytes of the largest frame, rounded up to whole pages (1920 x 1080 x 4 is 2025 pages already). */
#define I915_CAPTURE_PIXEL_BYTES	((((uint64_t)I915_CAPTURE_WIDTH * I915_CAPTURE_HEIGHT * 4U) + 4095U) & ~(uint64_t)4095U)

/* One slot: its header and the largest frame. */
#define I915_CAPTURE_SLOT_BYTES		((uint64_t)I915_CAPTURE_PIXEL_OFFSET + I915_CAPTURE_PIXEL_BYTES)

/* The whole area: the global header and every slot. */
#define I915_CAPTURE_AREA_BYTES		((uint64_t)I915_CAPTURE_HEADER_BYTES + (uint64_t)I915_CAPTURE_SLOTS * I915_CAPTURE_SLOT_BYTES)

/* A frame's rows start on 64-byte boundaries, as a linear render target's do. */
#define I915_CAPTURE_STRIDE_ALIGN	64U

/* last_slot before the first capture. */
#define I915_CAPTURE_NO_SLOT		0xffffffffU

/* hash_kind of a slot whose pixels were not hashed (the host hashes them itself). */
#define I915_CAPTURE_HASH_NONE		0U

/*
 * The global header at the start of the area, as the host reads it.
 *
 * Written whole once when the area is made; afterwards only last_slot and
 * write_count change, in that order, after a slot is complete.  Every field
 * sits at its natural alignment, so the offsets are fixed (capture.c).
 */
struct i915_capture_global {
	/* "I915CAP1", the layout version and the header's own size. */
	char magic[8];
	uint32_t version;
	uint32_t header_bytes;

	/* How many slots, and where a slot's pixels start inside it. */
	uint32_t slot_count;
	uint32_t pixel_offset;

	/* The bytes of one slot, where slot 0 starts, and the bytes of the whole area. */
	uint64_t slot_bytes;
	uint64_t first_slot_offset;
	uint64_t total_bytes;

	/* The guest physical address of the area (the one the log names). */
	uint64_t base;

	/* The largest frame a slot holds. */
	uint32_t max_width;
	uint32_t max_height;

	/* Completed captures so far (0 before the first), and the slot of the newest. */
	uint64_t write_count;
	uint32_t last_slot;
	uint32_t reserved;
};

/*
 * The header at the start of one slot, as the host reads it.
 *
 * ready is 0 while the slot is being written and equals sequence once the
 * frame and every other field are complete; it is written last.
 */
struct i915_capture_slot {
	/* "I915SLOT", written once when the area is made. */
	char magic[8];

	/* The capture's number: write_count once it completed (1-based). */
	uint64_t sequence;

	/* The frame: its extent, the bytes to a row, and its format (GPU_PIXEL_BGRA8888 or GPU_PIXEL_RGBA8888). */
	uint32_t width;
	uint32_t height;
	uint32_t stride_bytes;
	uint32_t format;

	/* The display and plane it was presented to, and the display's generation. */
	uint32_t display_id;
	uint32_t plane_index;
	uint64_t generation;

	/* The lease that presented it and the presentation's sequence in that lease. */
	uint64_t lease;
	uint64_t lease_sequence;

	/* When the presentation completed. */
	uint64_t present_time_ns;

	/* How the pixels were hashed (I915_CAPTURE_HASH_NONE: not at all), and this slot's index. */
	uint32_t hash_kind;
	uint32_t slot_index;
	uint64_t hash;

	/* 0 while being written; equal to sequence once complete. */
	uint64_t ready;
};

/*
 * One completed frame, as the present path hands it to the slot header.
 */
struct i915_capture_frame {
	/* The capture's number, 1-based. */
	uint64_t sequence;

	/* The frame's extent, row bytes and format. */
	uint32_t width;
	uint32_t height;
	uint32_t stride_bytes;
	uint32_t format;

	/* The lease, the presentation's sequence in it, and the completion time. */
	uint64_t lease;
	uint64_t lease_sequence;
	uint64_t present_time_ns;
};

/*
 * ==== The GPU node (display.c calls these in the capture build) ====
 */

/* Makes the area and binds the capture display in place of the panel; without the area the node has no display. */
void drv_i915_capture_bind_ops(struct i915_device *device, struct drv_gpu_ops *ops);

/* Ends the lease a closing session still holds. */
void drv_i915_capture_session_close(struct i915_device *device, void *session);

/* Frees the area once nothing maps it. */
void drv_i915_capture_fini(struct i915_device *device);

/*
 * ==== The layout (host-tested) ====
 */

/* The offset of slot `slot` from the start of the area. */
uint64_t drv_i915_capture_slot_offset(unsigned slot);

/* The slot the next capture goes to, after write_count completed ones. */
unsigned drv_i915_capture_next_slot(uint64_t write_count);

/* Writes the global header and every slot's magic into a zeroed area at guest physical `base`. */
void drv_i915_capture_init_header(uint8_t *area, uint64_t base);

/* Marks a slot as being written (ready 0) before the GPU copies into it. */
void drv_i915_capture_slot_open(uint8_t *area, unsigned slot);

/* Describes a completed frame in its slot, ready last, then publishes it in the global header. */
void drv_i915_capture_slot_close(uint8_t *area, unsigned slot, const struct i915_capture_frame *frame);

/* Checks a mode against the virtual display; a zero refresh is given the display's. */
int drv_i915_capture_check_mode(uint32_t width, uint32_t height, uint32_t *refresh_millihz);

/* Checks a frame against a slot and gives its row bytes in the slot. */
int drv_i915_capture_check_frame(uint32_t width, uint32_t height, uint32_t *stride_bytes);

#endif /* DRIVERS_GPU_I915_DISPLAY_CAPTURE_H */
