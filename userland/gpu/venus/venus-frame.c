/* -*- mode: c; c-file-style: "linux"; tab-width: 8; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Original, finite Venus wire-format-1 Vulkan transfer diagnostic.
 * Protocol constants and field ordering are documented in README.md.
 * This is not a Vulkan loader or a conformant Vulkan implementation.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "client.h"

#define FRAME_WIDTH 256U
#define FRAME_HEIGHT 192U
#define FRAME_BYTES (FRAME_WIDTH * FRAME_HEIGHT * 4U)

/* Stable public Venus command numbers, not native Vulkan function addresses. */
enum venus_command {
	VENUS_QUEUE_SUBMIT = 18,
	VENUS_ALLOCATE_MEMORY = 21,
	VENUS_BIND_BUFFER_MEMORY = 28,
	VENUS_BIND_IMAGE_MEMORY = 29,
	VENUS_GET_BUFFER_REQUIREMENTS = 30,
	VENUS_GET_IMAGE_REQUIREMENTS = 31,
	VENUS_CREATE_FENCE = 35,
	VENUS_GET_FENCE_STATUS = 38,
	VENUS_CREATE_BUFFER = 50,
	VENUS_CREATE_IMAGE = 54,
	VENUS_CREATE_COMMAND_POOL = 85,
	VENUS_ALLOCATE_COMMAND_BUFFERS = 88,
	VENUS_BEGIN_COMMAND_BUFFER = 90,
	VENUS_END_COMMAND_BUFFER = 91,
	VENUS_COPY_IMAGE_TO_BUFFER = 116,
	VENUS_CLEAR_COLOR_IMAGE = 119,
	VENUS_PIPELINE_BARRIER = 126
};

/* Vulkan structure tags carried on the wire, independent of C structure layout. */
enum venus_structure {
	STRUCTURE_SUBMIT_INFO = 4,
	STRUCTURE_MEMORY_ALLOCATE_INFO = 5,
	STRUCTURE_FENCE_CREATE_INFO = 8,
	STRUCTURE_BUFFER_CREATE_INFO = 12,
	STRUCTURE_IMAGE_CREATE_INFO = 14,
	STRUCTURE_COMMAND_POOL_CREATE_INFO = 39,
	STRUCTURE_COMMAND_BUFFER_ALLOCATE_INFO = 40,
	STRUCTURE_COMMAND_BUFFER_BEGIN_INFO = 42,
	STRUCTURE_BUFFER_MEMORY_BARRIER = 44,
	STRUCTURE_IMAGE_MEMORY_BARRIER = 45
};

/* Client-chosen object identities are scoped to this open Venus context. */
enum venus_object {
	OBJECT_IMAGE = 5,
	OBJECT_IMAGE_MEMORY = 6,
	OBJECT_BUFFER = 7,
	OBJECT_BUFFER_MEMORY = 8,
	OBJECT_COMMAND_POOL = 9,
	OBJECT_COMMAND_BUFFER = 10,
	OBJECT_FENCE = 11
};

/* The frame's pixels and render objects survive until its session closes. */
struct venus_frame {
	uint64_t buffer_allocation;
	uint32_t frame;
	uint8_t colors[2][4];
	uint8_t pixels[FRAME_BYTES];
};

/* One process serializes this session; static storage avoids a large stack. */
static struct venus_client client;

/* Rendering and validation share these bytes throughout the capture interval. */
static struct venus_frame frame;

static void wire_range(void);
static int allocate_object_memory(uint32_t object, uint32_t memory, int image);
static int create_render_objects(void);
static int image_barrier(uint32_t old_layout, uint32_t new_layout, uint32_t source_access, uint32_t destination_access);
static int clear_image(uint32_t band);
static int copy_image(uint32_t band);
static int record_commands(void);
static int submit_and_wait(void);
static int render_venus(void);
static int present_frame(void);
static int verify_pixels(void);
static int parse_arguments(int argc, char **argv, const char **device, const char **phase, uint32_t *hold);
static int parse_number(const char *text, uint32_t maximum, uint32_t *number);
static void select_colors(void);

/*
 * Run a bounded 2D or genuine Vulkan transfer frame and retain it for capture.
 */
int
main(
	int argc,
	char **argv)
{
	const char *device;
	const char *phase;
	uint32_t hold;
	uint32_t x;
	uint32_t y;
	uint32_t band;
	int status;
	int match;
	int saved_errno;
	int close_status;

	/* Default to the real Venus path with a short, finite capture window. */
	device = "/dev/gpu0";
	phase = "venus";
	hold = 10;
	client.fd = -1;
	frame.frame = 1;

	/* Validate all options before acquiring a GPU session. */
	status = parse_arguments(argc, argv, &device, &phase, &hold);
	if (status != 0) {
		fprintf(
			stderr,
			"usage: venus-frame --phase=2d|venus --frame=N --hold=0..120 [--device=/dev/gpu0]\n");
		return 2;
	}

	/* Open the selected GPU and obtain its registered capabilities. */
	status = venus_client_open(&client, device);
	if (status != 0) {
		perror("venus-frame: open");
		return 1;
	}

	/* Report the selected device and test path before rendering. */
	printf(
		"VENUS_FRAME start phase=%s frame=%u driver=%.32s caps=%u\n",
		phase,
		frame.frame,
		client.info.driver_name,
		client.info.capabilities);
	fflush(stdout);
	select_colors();

	/* The 2D control frame is deliberately separate from Vulkan evidence. */
	match = strcmp(phase, "venus");
	if (match == 0) {
		/* Produce this frame through the actual Venus Vulkan command path. */
		status = render_venus();
		if (status != 0) {
			goto out;
		}
	} else {
		/* Fill only the explicitly requested CPU control path. */
		for (y = 0; y < FRAME_HEIGHT; y++) {
			/* Fill the two bands across this CPU control row. */
			for (x = 0; x < FRAME_WIDTH; x++) {
				band = x / (FRAME_WIDTH / 2);
				memcpy(
					&frame.pixels[(y * FRAME_WIDTH + x) * 4],
					frame.colors[band],
					4);
			}
		}
	}

	/* Check every pixel before those same bytes become the displayed image. */
	status = verify_pixels();
	if (status != 0) {
		goto out;
	}

	/* Publish the validated pixels to the selected scanout. */
	status = present_frame();
	if (status != 0) {
		goto out;
	}

	/* Publish capture markers only after validated presentation succeeds. */
	printf(
		"VENUS_FRAME READY phase=%s frame=%u size=256x192 hold=%u\n",
		phase,
		frame.frame,
		hold);
	printf(
		"VENUS-FRAME PRESENT phase=%s frame=%u left=%u,%u,%u right=%u,%u,%u\n",
		phase,
		frame.frame,
		frame.colors[0][0],
		frame.colors[0][1],
		frame.colors[0][2],
		frame.colors[1][0],
		frame.colors[1][1],
		frame.colors[1][2]);
	fflush(stdout);

	/* Keep the session and scanout alive for a bounded host screenshot. */
	while (hold != 0) {
		hold = sleep(hold);
	}

out:
	/* Release the session through one cleanup path without losing its error. */
	saved_errno = errno;
	close_status = venus_client_close(&client);

	/* Preserve a rendering failure even when cleanup also encounters an error. */
	if (status != 0) {
		errno = saved_errno;
		perror("venus-frame");
		fprintf(stderr, "VENUS_FRAME FAILED command=%u\n", client.active_command);
		return 1;
	}

	/* Successful rendering still requires the session to close cleanly. */
	if (close_status != 0) {
		perror("venus-frame: close");
		return 1;
	}

	/* Succeeded. */
	printf("VENUS_FRAME DONE phase=%s frame=%u\n", phase, frame.frame);
	return 0;
}

/* Encode the complete color subresource of the one-level, one-layer image. */
static void
wire_range(void)
{
	/* Select the sole color mip level and array layer. */
	venus_client_wire_u32(&client, 1);
	venus_client_wire_u32(&client, 0);
	venus_client_wire_u32(&client, 1);
	venus_client_wire_u32(&client, 0);
	venus_client_wire_u32(&client, 1);

	/* Succeeded. */
	return;
}

/* Allocate compatible memory and bind the image or readback buffer. */
static int
allocate_object_memory(
	uint32_t object,
	uint32_t memory,
	int image)
{
	uint64_t present;
	uint64_t bytes;
	uint64_t alignment;
	uint32_t bits;
	uint32_t index;
	uint32_t selected;
	uint32_t required;
	uint32_t command;
	int status;

	/* Choose the requirements query for this object type. */
	command = VENUS_GET_BUFFER_REQUIREMENTS;
	if (image) {
		command = VENUS_GET_IMAGE_REQUIREMENTS;
	}

	/* Encode the selected object operation. */
	venus_client_command_begin(&client, command);
	venus_client_wire_u64(&client, VENUS_OBJECT_DEVICE);
	venus_client_wire_u64(&client, object);
	venus_client_wire_u64(&client, 1);

	/* Submit the completed encoding and validate its renderer reply. */
	status = venus_client_command_finish(&client, 0, 0);
	if (status != 0) {
		return -1;
	}

	/* Validate the returned pointer and bounded output payload. */
	present = venus_client_reply_u64(&client);
	bytes = venus_client_reply_u64(&client);
	alignment = venus_client_reply_u64(&client);
	bits = venus_client_reply_u32(&client);
	if (present != 1 ||
		bytes == 0 ||
		bytes > 16U * 1024U * 1024U ||
		alignment == 0) {
		errno = EIO;
		return -1;
	}

	/* Buffer readback requires HOST_VISIBLE and HOST_COHERENT memory. */
	required = 6;
	if (image) {
		required = 0;
	}

	/* Select the first compatible Vulkan memory type. */
	selected = UINT32_MAX;
	for (index = 0; index < client.memory_count; index++) {
		/* Require both the object's type mask and requested memory properties. */
		if ((bits & (1U << index)) != 0 &&
			(client.memory_flags[index] & required) == required) {
			selected = index;
			break;
		}
	}

	/* Fail when the device cannot provide the required memory properties. */
	if (selected == UINT32_MAX) {
		errno = ENOTSUP;
		return -1;
	}

	/* Round exportable allocation storage up to whole host pages. */
	bytes = (bytes + 4095) & ~((uint64_t)4095);

	/* Retain the buffer allocation size for its later one-time export. */
	if (!image) {
		frame.buffer_allocation = bytes;
	}

	/* Report the actual allocation choice for reproducible diagnostics. */
	printf(
		"VENUS_FRAME memory object=%u type=%u bytes=%u flags=%u\n",
		object,
		selected,
		(uint32_t)bytes,
		client.memory_flags[selected]);

	/* Exportable memory is a renderer property of the Venus allocation. */
	venus_client_command_begin(&client, VENUS_ALLOCATE_MEMORY);
	venus_client_wire_u64(&client, VENUS_OBJECT_DEVICE);
	venus_client_wire_u64(&client, 1);
	venus_client_wire_structure(&client, STRUCTURE_MEMORY_ALLOCATE_INFO);
	venus_client_wire_u64(&client, bytes);
	venus_client_wire_u32(&client, selected);
	venus_client_wire_u64(&client, 0);
	venus_client_wire_u64(&client, 1);
	venus_client_wire_u64(&client, memory);

	/* Submit the completed encoding and validate its renderer reply. */
	status = venus_client_command_finish(&client, 1, 0);
	if (status != 0) {
		return -1;
	}

	/* Confirm the renderer retained this client-selected object identity. */
	status = venus_client_reply_handle(&client, memory);
	if (status != 0) {
		return -1;
	}

	/* Both allocations bind at zero, satisfying all reported alignments. */
	command = VENUS_BIND_BUFFER_MEMORY;
	if (image) {
		command = VENUS_BIND_IMAGE_MEMORY;
	}

	/* Encode the selected object operation. */
	venus_client_command_begin(&client, command);
	venus_client_wire_u64(&client, VENUS_OBJECT_DEVICE);
	venus_client_wire_u64(&client, object);
	venus_client_wire_u64(&client, memory);
	venus_client_wire_u64(&client, 0);

	/* Submit the completed encoding and validate its renderer reply. */
	status = venus_client_command_finish(&client, 1, 0);
	if (status != 0) {
		return -1;
	}

	/* Succeeded. */
	return 0;
}

/* Create a transfer image, readback buffer, command buffer and fence. */
static int
create_render_objects(void)
{
	int status;

	/* The half-width image is cleared twice and copied into separate bands. */
	venus_client_command_begin(&client, VENUS_CREATE_IMAGE);
	venus_client_wire_u64(&client, VENUS_OBJECT_DEVICE);
	venus_client_wire_u64(&client, 1);
	venus_client_wire_structure(&client, STRUCTURE_IMAGE_CREATE_INFO);
	venus_client_wire_u32(&client, 0);
	venus_client_wire_u32(&client, 1);
	venus_client_wire_u32(&client, 37);
	venus_client_wire_u32(&client, FRAME_WIDTH / 2);
	venus_client_wire_u32(&client, FRAME_HEIGHT);
	venus_client_wire_u32(&client, 1);
	venus_client_wire_u32(&client, 1);
	venus_client_wire_u32(&client, 1);
	venus_client_wire_u32(&client, 1);
	venus_client_wire_u32(&client, 0);
	venus_client_wire_u32(&client, 3);
	venus_client_wire_u32(&client, 0);
	venus_client_wire_u32(&client, 0);
	venus_client_wire_u64(&client, 0);
	venus_client_wire_u32(&client, 0);
	venus_client_wire_u64(&client, 0);
	venus_client_wire_u64(&client, 1);
	venus_client_wire_u64(&client, OBJECT_IMAGE);

	/* Submit the completed encoding and validate its renderer reply. */
	status = venus_client_command_finish(&client, 1, 0);
	if (status != 0) {
		return -1;
	}

	/* Confirm the renderer retained this client-selected object identity. */
	status = venus_client_reply_handle(&client, OBJECT_IMAGE);
	if (status != 0) {
		return -1;
	}

	/* Bind memory whose properties satisfy this Vulkan object. */
	status = allocate_object_memory(OBJECT_IMAGE, OBJECT_IMAGE_MEMORY, 1);
	if (status != 0) {
		return -1;
	}

	/* The linear buffer is only a transfer destination, never CPU-painted. */
	venus_client_command_begin(&client, VENUS_CREATE_BUFFER);
	venus_client_wire_u64(&client, VENUS_OBJECT_DEVICE);
	venus_client_wire_u64(&client, 1);
	venus_client_wire_structure(&client, STRUCTURE_BUFFER_CREATE_INFO);
	venus_client_wire_u32(&client, 0);
	venus_client_wire_u64(&client, FRAME_BYTES);
	venus_client_wire_u32(&client, 2);
	venus_client_wire_u32(&client, 0);
	venus_client_wire_u32(&client, 0);
	venus_client_wire_u64(&client, 0);
	venus_client_wire_u64(&client, 0);
	venus_client_wire_u64(&client, 1);
	venus_client_wire_u64(&client, OBJECT_BUFFER);

	/* Submit the completed encoding and validate its renderer reply. */
	status = venus_client_command_finish(&client, 1, 0);
	if (status != 0) {
		return -1;
	}

	/* Confirm the renderer retained this client-selected object identity. */
	status = venus_client_reply_handle(&client, OBJECT_BUFFER);
	if (status != 0) {
		return -1;
	}

	/* Bind memory whose properties satisfy this Vulkan object. */
	status = allocate_object_memory(OBJECT_BUFFER, OBJECT_BUFFER_MEMORY, 0);
	if (status != 0) {
		return -1;
	}

	/* One primary command buffer records the complete Vulkan frame. */
	venus_client_command_begin(&client, VENUS_CREATE_COMMAND_POOL);
	venus_client_wire_u64(&client, VENUS_OBJECT_DEVICE);
	venus_client_wire_u64(&client, 1);
	venus_client_wire_structure(&client, STRUCTURE_COMMAND_POOL_CREATE_INFO);
	venus_client_wire_u32(&client, 0);
	venus_client_wire_u32(&client, client.queue_family);
	venus_client_wire_u64(&client, 0);
	venus_client_wire_u64(&client, 1);
	venus_client_wire_u64(&client, OBJECT_COMMAND_POOL);

	/* Submit the completed encoding and validate its renderer reply. */
	status = venus_client_command_finish(&client, 1, 0);
	if (status != 0) {
		return -1;
	}

	/* Confirm the renderer retained this client-selected object identity. */
	status = venus_client_reply_handle(&client, OBJECT_COMMAND_POOL);
	if (status != 0) {
		return -1;
	}

	/* Encode allocate command buffers. */
	venus_client_command_begin(&client, VENUS_ALLOCATE_COMMAND_BUFFERS);
	venus_client_wire_u64(&client, VENUS_OBJECT_DEVICE);
	venus_client_wire_u64(&client, 1);
	venus_client_wire_structure(&client, STRUCTURE_COMMAND_BUFFER_ALLOCATE_INFO);
	venus_client_wire_u64(&client, OBJECT_COMMAND_POOL);
	venus_client_wire_u32(&client, 0);
	venus_client_wire_u32(&client, 1);
	venus_client_wire_u64(&client, 1);
	venus_client_wire_u64(&client, OBJECT_COMMAND_BUFFER);

	/* Submit the completed encoding and validate its renderer reply. */
	status = venus_client_command_finish(&client, 1, 0);
	if (status != 0) {
		return -1;
	}

	/* Confirm the renderer retained this client-selected object identity. */
	status = venus_client_reply_handle(&client, OBJECT_COMMAND_BUFFER);
	if (status != 0) {
		return -1;
	}

	/* An initially unsignaled fence proves execution, beyond stream decode. */
	venus_client_command_begin(&client, VENUS_CREATE_FENCE);
	venus_client_wire_u64(&client, VENUS_OBJECT_DEVICE);
	venus_client_wire_u64(&client, 1);
	venus_client_wire_structure(&client, STRUCTURE_FENCE_CREATE_INFO);
	venus_client_wire_u32(&client, 0);
	venus_client_wire_u64(&client, 0);
	venus_client_wire_u64(&client, 1);
	venus_client_wire_u64(&client, OBJECT_FENCE);

	/* Submit the completed encoding and validate its renderer reply. */
	status = venus_client_command_finish(&client, 1, 0);
	if (status != 0) {
		return -1;
	}

	/* Confirm the renderer retained this client-selected object identity. */
	status = venus_client_reply_handle(&client, OBJECT_FENCE);
	if (status != 0) {
		return -1;
	}

	/* Succeeded. */
	return 0;
}

/* Record the image layout and access transition needed by the next transfer. */
static int
image_barrier(
	uint32_t old_layout,
	uint32_t new_layout,
	uint32_t source_access,
	uint32_t destination_access)
{
	uint32_t source_stage;
	int status;

	/* Use top-of-pipe only for the initial undefined image layout. */
	source_stage = 0x1000;
	if (old_layout == 0) {
		source_stage = 1;
	}

	/* Encode pipeline barrier. */
	venus_client_command_begin(&client, VENUS_PIPELINE_BARRIER);
	venus_client_wire_u64(&client, OBJECT_COMMAND_BUFFER);
	venus_client_wire_u32(&client, source_stage);
	venus_client_wire_u32(&client, 0x1000);
	venus_client_wire_u32(&client, 0);
	venus_client_wire_u32(&client, 0);
	venus_client_wire_u64(&client, 0);
	venus_client_wire_u32(&client, 0);
	venus_client_wire_u64(&client, 0);
	venus_client_wire_u32(&client, 1);
	venus_client_wire_u64(&client, 1);
	venus_client_wire_structure(&client, STRUCTURE_IMAGE_MEMORY_BARRIER);
	venus_client_wire_u32(&client, source_access);
	venus_client_wire_u32(&client, destination_access);
	venus_client_wire_u32(&client, old_layout);
	venus_client_wire_u32(&client, new_layout);
	venus_client_wire_u32(&client, UINT32_MAX);
	venus_client_wire_u32(&client, UINT32_MAX);
	venus_client_wire_u64(&client, OBJECT_IMAGE);
	wire_range();

	/* Submit the completed encoding and validate its renderer reply. */
	status = venus_client_command_finish(&client, 0, 0);
	if (status != 0) {
		return -1;
	}

	/* Succeeded. */
	return 0;
}

/* Record a real vkCmdClearColorImage with exactly representable UNORM colors. */
static int
clear_image(
	uint32_t band)
{
	uint32_t channel;
	uint32_t bits;
	int status;

	/* Encode clear color image. */
	venus_client_command_begin(&client, VENUS_CLEAR_COLOR_IMAGE);
	venus_client_wire_u64(&client, OBJECT_COMMAND_BUFFER);
	venus_client_wire_u64(&client, OBJECT_IMAGE);
	venus_client_wire_u32(&client, 7);
	venus_client_wire_u64(&client, 1);
	venus_client_wire_u32(&client, 0);
	venus_client_wire_u64(&client, 4);

	/* Union member zero is float32[4], each channel is exactly 0.0 or 1.0. */
	for (channel = 0; channel < 4; channel++) {
		/* Represent this UNORM endpoint as an exact float32 bit pattern. */
		bits = 0;
		if (frame.colors[band][channel] != 0) {
			bits = 0x3f800000U;
		}

		/* Append the chosen channel's exact float representation. */
		venus_client_wire_u32(&client, bits);
	}

	/* Clear the complete color range of the one-level image. */
	venus_client_wire_u32(&client, 1);
	venus_client_wire_u64(&client, 1);
	wire_range();

	/* Submit the completed encoding and validate its renderer reply. */
	status = venus_client_command_finish(&client, 0, 0);
	if (status != 0) {
		return -1;
	}

	/* Succeeded. */
	return 0;
}

/* Copy the Vulkan image into one band of a full-width linear buffer. */
static int
copy_image(
	uint32_t band)
{
	int status;

	/* Encode copy image to buffer. */
	venus_client_command_begin(&client, VENUS_COPY_IMAGE_TO_BUFFER);
	venus_client_wire_u64(&client, OBJECT_COMMAND_BUFFER);
	venus_client_wire_u64(&client, OBJECT_IMAGE);
	venus_client_wire_u32(&client, 6);
	venus_client_wire_u64(&client, OBJECT_BUFFER);
	venus_client_wire_u32(&client, 1);
	venus_client_wire_u64(&client, 1);
	venus_client_wire_u64(&client, band * (FRAME_WIDTH / 2) * 4);
	venus_client_wire_u32(&client, FRAME_WIDTH);
	venus_client_wire_u32(&client, FRAME_HEIGHT);

	/* VkImageSubresourceLayers, zero offset and half-width extent. */
	venus_client_wire_u32(&client, 1);
	venus_client_wire_u32(&client, 0);
	venus_client_wire_u32(&client, 0);
	venus_client_wire_u32(&client, 1);
	venus_client_wire_u32(&client, 0);
	venus_client_wire_u32(&client, 0);
	venus_client_wire_u32(&client, 0);
	venus_client_wire_u32(&client, FRAME_WIDTH / 2);
	venus_client_wire_u32(&client, FRAME_HEIGHT);
	venus_client_wire_u32(&client, 1);

	/* Submit the completed encoding and validate its renderer reply. */
	status = venus_client_command_finish(&client, 0, 0);
	if (status != 0) {
		return -1;
	}

	/* Succeeded. */
	return 0;
}

/* Record two clears, their copies, and host readback visibility. */
static int
record_commands(void)
{
	uint32_t band;
	int status;

	/* Encode begin command buffer. */
	venus_client_command_begin(&client, VENUS_BEGIN_COMMAND_BUFFER);
	venus_client_wire_u64(&client, OBJECT_COMMAND_BUFFER);
	venus_client_wire_u64(&client, 1);
	venus_client_wire_structure(&client, STRUCTURE_COMMAND_BUFFER_BEGIN_INFO);
	venus_client_wire_u32(&client, 1);
	venus_client_wire_u64(&client, 0);

	/* Submit the completed encoding and validate its renderer reply. */
	status = venus_client_command_finish(&client, 1, 0);
	if (status != 0) {
		return -1;
	}

	/* Order image accesses and transition to the next transfer layout. */
	status = image_barrier(0, 7, 0, 0x1000);
	if (status != 0) {
		return -1;
	}

	/* Each band originates from a separate Vulkan clear operation. */
	for (band = 0; band < 2; band++) {
		/* Clear the image to this band's Vulkan color. */
		status = clear_image(band);
		if (status != 0) {
			return -1;
		}

		/* Order image accesses and transition to the next transfer layout. */
		status = image_barrier(7, 6, 0x1000, 0x800);
		if (status != 0) {
			return -1;
		}

		/* Copy the completed image into this band of the readback buffer. */
		status = copy_image(band);
		if (status != 0) {
			return -1;
		}

		/* Prepare the shared image for the second clear after its first copy. */
		if (band == 0) {
			/* Order image accesses and transition to the next transfer layout. */
			status = image_barrier(6, 7, 0x800, 0x1000);
			if (status != 0) {
				return -1;
			}
		}
	}

	/* Make all transfer writes visible to coherent host reads after the fence. */
	venus_client_command_begin(&client, VENUS_PIPELINE_BARRIER);
	venus_client_wire_u64(&client, OBJECT_COMMAND_BUFFER);
	venus_client_wire_u32(&client, 0x1000);
	venus_client_wire_u32(&client, 0x4000);
	venus_client_wire_u32(&client, 0);
	venus_client_wire_u32(&client, 0);
	venus_client_wire_u64(&client, 0);
	venus_client_wire_u32(&client, 1);
	venus_client_wire_u64(&client, 1);
	venus_client_wire_structure(&client, STRUCTURE_BUFFER_MEMORY_BARRIER);
	venus_client_wire_u32(&client, 0x1000);
	venus_client_wire_u32(&client, 0x2000);
	venus_client_wire_u32(&client, UINT32_MAX);
	venus_client_wire_u32(&client, UINT32_MAX);
	venus_client_wire_u64(&client, OBJECT_BUFFER);
	venus_client_wire_u64(&client, 0);
	venus_client_wire_u64(&client, FRAME_BYTES);
	venus_client_wire_u32(&client, 0);
	venus_client_wire_u64(&client, 0);

	/* Submit the completed encoding and validate its renderer reply. */
	status = venus_client_command_finish(&client, 0, 0);
	if (status != 0) {
		return -1;
	}

	/* Encode end command buffer. */
	venus_client_command_begin(&client, VENUS_END_COMMAND_BUFFER);
	venus_client_wire_u64(&client, OBJECT_COMMAND_BUFFER);

	/* Submit the completed encoding and validate its renderer reply. */
	status = venus_client_command_finish(&client, 1, 0);
	if (status != 0) {
		return -1;
	}

	/* Succeeded. */
	return 0;
}

/* Submit once and poll Vulkan's fence independently of protocol completion. */
static int
submit_and_wait(void)
{
	uint32_t poll;
	int status;

	/* Encode queue submit. */
	venus_client_command_begin(&client, VENUS_QUEUE_SUBMIT);
	venus_client_wire_u64(&client, VENUS_OBJECT_QUEUE);
	venus_client_wire_u32(&client, 1);
	venus_client_wire_u64(&client, 1);
	venus_client_wire_structure(&client, STRUCTURE_SUBMIT_INFO);
	venus_client_wire_u32(&client, 0);
	venus_client_wire_u64(&client, 0);
	venus_client_wire_u64(&client, 0);
	venus_client_wire_u32(&client, 1);
	venus_client_wire_u64(&client, 1);
	venus_client_wire_u64(&client, OBJECT_COMMAND_BUFFER);
	venus_client_wire_u32(&client, 0);
	venus_client_wire_u64(&client, 0);
	venus_client_wire_u64(&client, OBJECT_FENCE);

	/* Submit the completed encoding and validate its renderer reply. */
	status = venus_client_command_finish(&client, 1, 0);
	if (status != 0) {
		return -1;
	}

	/* VK_NOT_READY is expected until the GPU completes the recorded transfers. */
	for (poll = 0; poll < VENUS_CLIENT_POLL_LIMIT; poll++) {
		/* Encode get fence status. */
		venus_client_command_begin(&client, VENUS_GET_FENCE_STATUS);
		venus_client_wire_u64(&client, VENUS_OBJECT_DEVICE);
		venus_client_wire_u64(&client, OBJECT_FENCE);

		/* Submit the completed encoding and validate its renderer reply. */
		status = venus_client_command_finish(&client, 1, 1);
		if (status == 0) {
			break;
		}

		/* Reject fence errors instead of accepting them as pending work. */
		if (status != 1) {
			return -1;
		}

		/* Yield before the next bounded completion check. */
		status = venus_client_poll_pause(&client);
		if (status != 0) {
			return -1;
		}
	}

	/* A bounded fence failure cannot be reported as a rendered frame. */
	if (poll == VENUS_CLIENT_POLL_LIMIT) {
		errno = ETIMEDOUT;
		return -1;
	}

	/* Succeeded: Vulkan has completed the submitted image transfers. */
	printf("VENUS_FRAME Vulkan fence=signaled polls=%u\n", poll + 1);
	return 0;
}

/* Execute the finite Vulkan path and retrieve only its GPU-produced pixels. */
static int
render_venus(void)
{
	struct gpu_blob_create blob;
	int status;

	/* Establish the shared reply stream, Vulkan device and graphics queue. */
	status = venus_client_init_vulkan(&client);
	if (status != 0) {
		return -1;
	}

	/* Report the protocol and resources selected by the shared bootstrap. */
	printf(
		"VENUS_FRAME capset=4 wire=%u xml=%u bytes=%u\n",
		client.wire_version,
		client.xml_version,
		client.capset_bytes);
	printf(
		"VENUS_FRAME reply-blob=created resource=%u bytes=%u blob-id=0\n",
		client.reply_resource,
		VENUS_CLIENT_REPLY_BYTES);
	printf("VENUS_FRAME Vulkan device=created family=%u\n", client.queue_family);
	fflush(stdout);

	/* Allocate the Vulkan resources used by this finite frame. */
	status = create_render_objects();
	if (status != 0) {
		return -1;
	}

	/* Record the clears, image copies and visibility barriers. */
	status = record_commands();
	if (status != 0) {
		return -1;
	}

	/* Wait for Vulkan execution before exporting readback memory. */
	status = submit_and_wait();
	if (status != 0) {
		return -1;
	}

	/* Nonzero blob ID exports that completed VkDeviceMemory exactly once. */
	memset(&blob, 0, sizeof(blob));
	blob.version = GPU_ABI_VERSION;
	blob.size = sizeof(blob);
	blob.bytes = frame.buffer_allocation;
	blob.blob_id = OBJECT_BUFFER_MEMORY;
	blob.flags = GPU_BLOB_MAPPABLE;
	status = ioctl(client.fd, GPU_BLOB_CREATE, &blob);
	if (status != 0) {
		return -1;
	}

	/* Copy the requested bytes through the owned resource interface. */
	status = venus_client_resource_copy(&client, blob.handle, 0, frame.pixels, FRAME_BYTES, 0);
	if (status != 0) {
		return -1;
	}

	/* Succeeded. */
	return 0;
}

/* Publish the validated packed pixels through the initial copy-based WSI. */
static int
present_frame(void)
{
	struct gpu_resource_create resource;
	struct gpu_present present;
	int status;

	/* Allocate ordinary storage for the initial copy-based scanout path. */
	memset(&resource, 0, sizeof(resource));
	resource.version = GPU_ABI_VERSION;
	resource.size = sizeof(resource);
	resource.bytes = FRAME_BYTES;
	resource.usage = GPU_RESOURCE_USAGE_STORAGE;
	status = ioctl(client.fd, GPU_RESOURCE_CREATE, &resource);
	if (status != 0) {
		return -1;
	}

	/* Copy the requested bytes through the owned resource interface. */
	status = venus_client_resource_copy(&client, resource.handle, 0, frame.pixels, FRAME_BYTES, 1);
	if (status != 0) {
		return -1;
	}

	/* Present does not manufacture pixels or assert Vulkan completion. */
	memset(&present, 0, sizeof(present));
	present.version = GPU_ABI_VERSION;
	present.size = sizeof(present);
	present.handle = resource.handle;
	present.width = FRAME_WIDTH;
	present.height = FRAME_HEIGHT;
	present.stride = FRAME_WIDTH * 4;
	present.format = GPU_PIXEL_RGBA8888;
	present.frame = frame.frame;
	status = ioctl(client.fd, GPU_PRESENT, &present);
	if (status != 0) {
		return -1;
	}

	/* Succeeded. */
	return 0;
}

/* Validate the complete frame and print an independently reproducible hash. */
static int
verify_pixels(void)
{
	uint32_t x;
	uint32_t y;
	uint32_t channel;
	uint32_t index;
	uint32_t band;
	uint32_t hash;
	uint8_t expected;

	/* Start the reproducible FNV-1a checksum before inspecting actual bytes. */
	hash = 2166136261U;

	/* Inspect actual bytes; never repair or replace a mismatching GPU result. */
	for (y = 0; y < FRAME_HEIGHT; y++) {
		/* Inspect each pixel across this completed row. */
		for (x = 0; x < FRAME_WIDTH; x++) {
			/* Select the expected band and compare each RGBA channel. */
			band = x / (FRAME_WIDTH / 2);
			for (channel = 0; channel < 4; channel++) {
				/* Locate this channel and compare it with its exact expected endpoint. */
				index = (y * FRAME_WIDTH + x) * 4 + channel;
				expected = frame.colors[band][channel];
				if (frame.pixels[index] != expected) {
					fprintf(
						stderr,
						"pixel mismatch x=%u y=%u channel=%u got=%u expected=%u\n",
						x,
						y,
						channel,
						frame.pixels[index],
						expected);
					errno = EIO;
					return -1;
				}

				/* Accumulate the unmodified, validated readback byte. */
				hash ^= frame.pixels[index];
				hash *= 16777619U;
			}
		}
	}

	/* Report the complete frame checksum for capture correlation. */
	printf(
		"VENUS_FRAME pixels=verified bytes=%u fnv1a=%08x frame=%u\n",
		FRAME_BYTES,
		hash,
		frame.frame);

	/* Succeeded. */
	return 0;
}

/* Parse the diagnostic options before acquiring any external resources. */
static int
parse_arguments(
	int argc,
	char **argv,
	const char **device,
	const char **phase,
	uint32_t *hold)
{
	int argument;
	int match;
	int status;

	/* Reject unknown options instead of silently changing the test. */
	for (argument = 1; argument < argc; argument++) {
		/* Select the requested rendering path. */
		match = strncmp(argv[argument], "--phase=", 8);
		if (match == 0) {
			*phase = argv[argument] + 8;
			continue;
		}

		/* Select the ordinary GPU device node. */
		match = strncmp(argv[argument], "--device=", 9);
		if (match == 0) {
			*device = argv[argument] + 9;
			continue;
		}

		/* Select the frame pattern identifier within its finite range. */
		match = strncmp(argv[argument], "--frame=", 8);
		if (match == 0) {
			/* Require a complete decimal option value. */
			status = parse_number(argv[argument] + 8, 1000000, &frame.frame);
			if (status != 0) {
				return -1;
			}

			/* Advance after consuming this recognized option. */
			continue;
		}

		/* Select the finite screenshot capture window. */
		match = strncmp(argv[argument], "--hold=", 7);
		if (match == 0) {
			/* Reject an absent value or a capture longer than the limit. */
			status = parse_number(argv[argument] + 7, 120, hold);
			if (status != 0) {
				return -1;
			}

			/* Advance after consuming this recognized option. */
			continue;
		}

		/* Reject an argument which matched none of the documented options. */
		return -1;
	}

	/* Only the explicit CPU control and Vulkan paths are supported. */
	match = strcmp(*phase, "venus");
	status = strcmp(*phase, "2d");
	if (match != 0 && status != 0) {
		return -1;
	}

	/* Succeeded. */
	return 0;
}

/* Parse a finite decimal option without accepting signs or trailing text. */
static int
parse_number(
	const char *text,
	uint32_t maximum,
	uint32_t *number)
{
	uint32_t parsed_number;
	uint32_t digit;

	/* Reject an absent number before processing its digits. */
	parsed_number = 0;
	if (*text == '\0') {
		return -1;
	}

	/* Check the limit before multiplying each successive decimal digit. */
	while (*text != '\0') {
		/* Reject signs and every other nondecimal character. */
		if (*text < '0' || *text > '9') {
			return -1;
		}

		/* Check the limit before accumulating this decimal digit. */
		digit = (uint32_t)(*text - '0');
		if (parsed_number > maximum / 10 ||
			(parsed_number == maximum / 10 && digit > maximum % 10)) {
			return -1;
		}

		/* Accumulate the checked digit and advance the input. */
		parsed_number = parsed_number * 10 + digit;
		text++;
	}

	/* Publish the complete validated option parsed_number. */
	*number = parsed_number;

	/* Succeeded. */
	return 0;
}

/* Change both image bands with frame parity to expose stale-frame captures. */
static void
select_colors(void)
{
	/* Initialize opaque RGBA endpoints before selecting the frame palette. */
	memset(frame.colors, 0, sizeof(frame.colors));
	frame.colors[0][3] = 255;
	frame.colors[1][3] = 255;

	/* Odd frames are red/green; even frames are blue/yellow in RGBA order. */
	if ((frame.frame & 1) != 0) {
		frame.colors[0][0] = 255;
		frame.colors[1][1] = 255;
	} else {
		frame.colors[0][2] = 255;
		frame.colors[1][0] = 255;
		frame.colors[1][1] = 255;
	}

	/* Succeeded. */
	return;
}
