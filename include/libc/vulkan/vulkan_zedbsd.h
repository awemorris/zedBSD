/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * zedBSD's own additions to the Vulkan API of its libvulkan, outside the
 * Khronos declarations.
 *
 * VkImageLayoutZEDBSD names the layout an image shared with another
 * process is placed in: the kernel's tiling of the image (GPU_IMAGE_* of
 * <uapi/gpu.h>) and its parameter, as the image's kernel description
 * gives them.  It is chained after VkExternalMemoryImageCreateInfo in a
 * VkImageCreateInfo of VK_IMAGE_TILING_OPTIMAL, so that an image imported
 * from such a description reads its memory the way the exporter placed it.
 * Only a device that names its own layouts takes it (vkCreateImage refuses
 * it elsewhere with VK_ERROR_FORMAT_NOT_SUPPORTED).
 */

#ifndef VULKAN_ZEDBSD_H_
#define VULKAN_ZEDBSD_H_ 1

#include "vulkan_core.h"

#ifdef __cplusplus
extern "C" {
#endif

#define VK_STRUCTURE_TYPE_IMAGE_LAYOUT_ZEDBSD ((VkStructureType)0x7a650001)

typedef struct VkImageLayoutZEDBSD {
    VkStructureType sType;
    const void *pNext;
    uint32_t tiling;
    uint32_t parameter;
} VkImageLayoutZEDBSD;

#ifdef __cplusplus
}
#endif

#endif
