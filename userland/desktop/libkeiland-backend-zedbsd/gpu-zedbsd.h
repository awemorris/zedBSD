/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* The image description used only inside the zedBSD GPU module (libkeiland-backend, WS131 p009). */
#ifndef KWL_GPU_ZEDBSD_H
#define KWL_GPU_ZEDBSD_H

#include "userland/desktop/libkeiland-backend/keiland-backend-gpu.h"
#include "include/libc/vulkan/vulkan_zedbsd.h"

/*
 * What the compositor's Vulkan device can take, against which a description
 * is checked before any of its values reaches Vulkan (from the device the
 * protocol host lends, for one request).
 */
struct kwl_gpu_limits {
	uint32_t max_dimension;
	uint32_t memory_type_count;
};

/*
 * The layout of a client's GPU buffer as its description states it.
 *
 * One per wl_buffer, filled when the buffer is created and not changed
 * afterwards.  The image the compositor makes of the buffer has this size,
 * format and row layout; the import checks that the description is the
 * allocation's real one.
 */
struct kwl_buffer_layout {
	uint32_t width;
	uint32_t height;
	VkFormat format;
	uint32_t stride;
	uint64_t offset;
	uint64_t allocation_bytes;
	uint32_t memory_type;
	/*
	 * The image's layout: linear rows (linear set), or another layout the
	 * device that made it names (tiling and parameter, the kernel's), which
	 * an image of exactly the described uses reads.
	 */
	unsigned linear;
	uint32_t tiling;
	uint32_t parameter;
	VkImageUsageFlags usage;
};

/* Reports the zedBSD description's wire length. */
size_t kwl_gpu_buffer_wire_bytes(void);
/* Decodes and checks every field before it reaches Vulkan. */
int kwl_gpu_buffer_decode(const unsigned char *bytes, size_t size, const struct kwl_gpu_limits *limits, struct kwl_buffer_layout *layout);
/* Reports the memory handle type of a zedBSD image capability. */
VkExternalMemoryHandleTypeFlagBits kwl_gpu_buffer_handle_type(void);

#endif
