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
 *
 * VK_ZEDBSD_display_edid (an instance extension that requires
 * VK_KHR_get_display_properties2) gives a display's EDID as its sink sent
 * it: VkDisplayEdidPropertiesZEDBSD chained to a VkDisplayProperties2KHR
 * of vkGetPhysicalDeviceDisplayProperties2KHR receives up to 512 bytes
 * (whole blocks of 128; edidSize 0 when the display has none or its
 * driver keeps none).  The bytes are the sink's: the caller checks them.
 * It is how the desktop tells one monitor from another.
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

#define VK_ZEDBSD_display_edid 1
#define VK_ZEDBSD_DISPLAY_EDID_SPEC_VERSION 1
#define VK_ZEDBSD_DISPLAY_EDID_EXTENSION_NAME "VK_ZEDBSD_display_edid"
#define VK_STRUCTURE_TYPE_DISPLAY_EDID_PROPERTIES_ZEDBSD ((VkStructureType)0x7a650002)
#define VK_ZEDBSD_DISPLAY_EDID_MAX 512U

typedef struct VkDisplayEdidPropertiesZEDBSD {
    VkStructureType sType;
    void *pNext;
    uint32_t edidSize;
    uint8_t edid[VK_ZEDBSD_DISPLAY_EDID_MAX];
} VkDisplayEdidPropertiesZEDBSD;

#ifdef __cplusplus
}
#endif

#endif
