/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* Immutable single-colour pass and framebuffer owners bind draw preparation to the exact native target. */
#ifndef KERN_DRIVERS_GPU_BCM2711_VULKAN_TARGET_H
#define KERN_DRIVERS_GPU_BCM2711_VULKAN_TARGET_H

#include "drivers/gpu/bcm2711/vulkan-input.h"

#define BCM2711_VULKAN_PASS_DEPENDENCIES 4U

/* One single-subpass pass retains its device and exact attachment lifecycle until dependent pipelines and targets retire. */
struct bcm2711_vulkan_pass {
	struct bcm2711_vulkan_input_owner owner;
	VkAttachmentDescription colour;
	uint32_t count;
	VkSubpassDependency dependencies[BCM2711_VULKAN_PASS_DEPENDENCIES];
};

/* One framebuffer retains its pass and full-colour view independently of their public handles and memory identities. */
struct bcm2711_vulkan_framebuffer {
	struct bcm2711_vulkan_input_owner owner;
	struct bcm2711_vulkan_object *view;
	uint32_t width;
	uint32_t height;
};

int bcm2711_vulkan_target_dispatch(struct bcm2711_vulkan_session *session, uint32_t opcode, uint32_t requested, struct i915_wire_reader *reader, struct i915_wire_writer *reply, int *handled);
int bcm2711_vulkan_pass_compatible(const struct bcm2711_vulkan_pass *first, const struct bcm2711_vulkan_pass *second);

#endif
