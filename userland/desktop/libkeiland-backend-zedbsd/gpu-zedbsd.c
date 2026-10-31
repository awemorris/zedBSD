/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The zedBSD side of the GPU buffers (libkeiland-backend, keiland-backend-gpu.h;
 * WS103, WS131 p009).
 *
 * On zedBSD a client's GPU buffer is a kernel image capability fd with the
 * kernel's description of the image (struct gpu_image_descriptor, 64 bytes,
 * as kl_gpu_buffer_v1.create_buffer's array).  The fd is imported as
 * Vulkan OPAQUE_FD memory; libvulkan checks the description against the
 * kernel's own record of the capability when the import names the image
 * (a dedicated import).  This file is the only one of the desktop that
 * reads the kernel's GPU types.
 */

#include "userland/desktop/libkeiland-backend-zedbsd/gpu-zedbsd.h"

#include <uapi/gpu.h>
#include <errno.h>
#include <string.h>

/*
 * Reports how many bytes a buffer's description takes on the wire.
 */
size_t
kwl_gpu_buffer_wire_bytes(
	void)
{
	/* Succeeded: the kernel's image description. */
	return sizeof(struct gpu_image_descriptor);
}

/*
 * Decodes a buffer's description and checks every value a client chose.
 *
 * Nothing of the description reaches Vulkan before it is known to be an
 * image Vulkan can make: a nonzero size within the device's largest image,
 * one of the two four-channel formats in linear rows, rows of at least the
 * width that start inside and end inside the allocation, and a memory type
 * the device has.  Returns EINVAL for anything else.
 */
int
kwl_gpu_buffer_decode(
	const unsigned char *bytes,
	size_t size,
	const struct kwl_gpu_limits *limits,
	struct kwl_buffer_layout *layout)
{
	struct gpu_image_descriptor image;
	VkFormat format;
	uint64_t row_bytes;
	uint64_t rows_bytes;
	uint64_t end;

	/* The description is exactly the kernel's record. */
	if (size != sizeof(image))
		return EINVAL;

	/* Copies the record out of the (possibly unaligned) wire bytes. */
	memcpy(&image, bytes, sizeof(image));

	/* A record of this interface's revision and length. */
	if (image.version != GPU_ABI_VERSION)
		return EINVAL;

	/* Refuses a record whose declared size differs from this ABI. */
	if (image.size != sizeof(image))
		return EINVAL;

	/* A size Vulkan can make an image of. */
	if (image.width == 0U || image.width > limits->max_dimension)
		return EINVAL;

	/* Refuses a height outside the device image limit. */
	if (image.height == 0U || image.height > limits->max_dimension)
		return EINVAL;


	/* One of the two four-channel formats, in its byte order. */
	if (image.format == GPU_PIXEL_BGRA8888) {
		format = VK_FORMAT_B8G8R8A8_UNORM;
	} else if (image.format == GPU_PIXEL_RGBA8888) {
		format = VK_FORMAT_R8G8B8A8_UNORM;
	} else {
		return EINVAL;
	}

	/* Rows of whole pixels, at least as long as a row of the image. */
	row_bytes = (uint64_t)image.width * 4U;
	if (image.stride < row_bytes)
		return EINVAL;

	/* Refuses a row ending in part of a four-byte pixel. */
	if ((image.stride % 4U) != 0U)
		return EINVAL;

	/* The last row ends inside the allocation (no value here can overflow 64 bits but the offset). */
	rows_bytes = (uint64_t)image.stride * (uint64_t)(image.height - 1U) + row_bytes;
	if (image.offset > UINT64_MAX - rows_bytes)
		return EINVAL;

	/*
	 * A linear image's last pixel ends inside the allocation.  Another
	 * layout's placement is its device's (the kernel checked it against the
	 * allocation when the image went out); its start is inside here.
	 */
	end = image.offset + rows_bytes;
	if (image.tiling == GPU_IMAGE_LINEAR && end > image.allocation_bytes)
		return EINVAL;
	if (image.tiling != GPU_IMAGE_LINEAR && image.offset >= image.allocation_bytes)
		return EINVAL;

	/* An image of another layout is read with exactly its uses, sampling among them. */
	if (image.tiling != GPU_IMAGE_LINEAR && (image.usage & VK_IMAGE_USAGE_SAMPLED_BIT) == 0U)
		return EINVAL;

	/* A memory type the device has. */
	if (image.memory_type >= limits->memory_type_count)
		return EINVAL;

	/* The layout the compositor makes its image of. */
	layout->width = image.width;
	layout->height = image.height;
	layout->format = format;
	layout->stride = image.stride;
	layout->offset = image.offset;
	layout->allocation_bytes = image.allocation_bytes;
	layout->memory_type = image.memory_type;
	layout->linear = 0U;
	if (image.tiling == GPU_IMAGE_LINEAR)
		layout->linear = 1U;
	layout->tiling = image.tiling;
	layout->parameter = image.layout_parameter;
	layout->usage = image.usage;

	/* Succeeded: every value is one Vulkan can take. */
	return 0;
}

/*
 * Reports the Vulkan handle type a buffer's fd is imported as.
 */
VkExternalMemoryHandleTypeFlagBits
kwl_gpu_buffer_handle_type(
	void)
{
	/* Succeeded: a kernel image capability is libvulkan's OPAQUE_FD. */
	return VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
}
