/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* Immutable single-colour pass and framebuffer owners bind draw preparation to the exact native target. */
#ifndef KERN_DRIVERS_GPU_BCM2711_VULKAN_TARGET_H
#define KERN_DRIVERS_GPU_BCM2711_VULKAN_TARGET_H

#include "drivers/gpu/bcm2711/vk/vulkan-input.h"

#define BCM2711_VULKAN_PASS_DEPENDENCIES 4U

/*
 * One single-subpass pass retains its device and exact attachment lifecycle until dependent pipelines and targets retire.
 *
 * Attachment zero is the colour target; a pass with depth set also has a
 * depth/stencil attachment one (D24S8 or X8_D24, ws141-p017), which the
 * subpass uses.
 */
struct bcm2711_vulkan_pass {
	struct bcm2711_vulkan_input_owner owner;
	VkAttachmentDescription colour;
	bool depth;
	VkAttachmentDescription depth_stencil;
	uint32_t count;
	VkSubpassDependency dependencies[BCM2711_VULKAN_PASS_DEPENDENCIES];
};

/*
 * One framebuffer retains its pass and full-colour view independently of their public handles and memory identities.
 *
 * depth is the depth/stencil attachment's view, retained like the colour
 * view, when the pass has one; otherwise NULL.
 */
struct bcm2711_vulkan_framebuffer {
	struct bcm2711_vulkan_input_owner owner;
	struct bcm2711_vulkan_object *view;
	struct bcm2711_vulkan_object *depth;
	uint32_t width;
	uint32_t height;
};

int bcm2711_vulkan_target_dispatch(struct bcm2711_vulkan_session *session, uint32_t opcode, uint32_t requested, struct i915_wire_reader *reader, struct i915_wire_writer *reply, int *handled);
int bcm2711_vulkan_pass_compatible(const struct bcm2711_vulkan_pass *first, const struct bcm2711_vulkan_pass *second);
bool bcm2711_vulkan_depth_format(VkFormat format);

#endif
