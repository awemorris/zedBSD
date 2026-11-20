/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* Immutable single-subpass pass and framebuffer owners bind draw preparation to the exact native targets. */
#ifndef KERN_DRIVERS_GPU_BCM2711_VULKAN_TARGET_H
#define KERN_DRIVERS_GPU_BCM2711_VULKAN_TARGET_H

#include "drivers/gpu/videocore/vk/vulkan-input.h"

#define BCM2711_VULKAN_PASS_DEPENDENCIES 4U

/* The most colour attachments a subpass draws into (the fragment outputs' locations 0 to 3). */
#define BCM2711_VULKAN_PASS_COLOURS 4U

/* The most attachments a pass has: its colour attachments and a depth/stencil one. */
#define BCM2711_VULKAN_PASS_ATTACHMENTS (BCM2711_VULKAN_PASS_COLOURS + 1U)

/* The most views a multiview subpass renders (maxMultiviewViewCount): view masks of bits 0 to 7. */
#define BCM2711_VULKAN_VIEWS 8U

/*
 * One single-subpass pass retains its device and exact attachment lifecycle until dependent pipelines and targets retire.
 *
 * The subpass's colour slots are its colour attachment references, which
 * the fragment outputs' locations name: colour_count of them (none for a
 * depth-only pass), each used or not (VK_ATTACHMENT_UNUSED), with the
 * description of the attachment a used one names and that attachment's
 * index.  A pass with depth set also has a depth/stencil attachment (D24S8,
 * X8_D24, D16 or D32_SFLOAT) at depth_attachment, which the subpass uses.
 * The subpass names every attachment once.  Every attachment has samples
 * samples a pixel (1 or 4) but a resolve's: a pass of four samples with
 * depth_resolve set (from vkCreateRenderPass2) also stores sample 0 of its
 * depth/stencil attachment, both aspects of a D24S8 one, into the
 * attachment of one sample in its format at depth_resolve_attachment.
 *
 * view_mask is the subpass's views under multiview (bit n: view n, drawn
 * into layer n of each attachment's view), zero for a pass of one view.
 */
struct bcm2711_vulkan_pass {
	struct bcm2711_vulkan_input_owner owner;
	uint32_t attachment_count;
	uint32_t samples;
	uint32_t colour_count;
	bool colour_used[BCM2711_VULKAN_PASS_COLOURS];
	VkAttachmentDescription colours[BCM2711_VULKAN_PASS_COLOURS];
	uint32_t colour_attachment[BCM2711_VULKAN_PASS_COLOURS];
	bool depth;
	VkAttachmentDescription depth_stencil;
	uint32_t depth_attachment;
	bool depth_resolve;
	VkAttachmentDescription depth_resolved;
	uint32_t depth_resolve_attachment;
	uint32_t view_mask;
	uint32_t count;
	VkSubpassDependency dependencies[BCM2711_VULKAN_PASS_DEPENDENCIES];
};

/*
 * One framebuffer retains its pass and attachment views independently of their public handles and memory identities.
 *
 * colours holds the view of each of the pass's colour slots (NULL for an
 * unused one), and depth the depth/stencil attachment's view when the
 * pass has one, otherwise NULL, and depth_resolve the view its depth/stencil
 * attachment is resolved into when the pass resolves it, otherwise NULL;
 * each is retained.
 */
struct bcm2711_vulkan_framebuffer {
	struct bcm2711_vulkan_input_owner owner;
	struct bcm2711_vulkan_object *colours[BCM2711_VULKAN_PASS_COLOURS];
	struct bcm2711_vulkan_object *depth;
	struct bcm2711_vulkan_object *depth_resolve;
	uint32_t width;
	uint32_t height;
};

int bcm2711_vulkan_target_dispatch(struct bcm2711_vulkan_session *session, uint32_t opcode, uint32_t requested, struct i915_wire_reader *reader, struct i915_wire_writer *reply, int *handled);
int bcm2711_vulkan_pass_compatible(const struct bcm2711_vulkan_pass *first, const struct bcm2711_vulkan_pass *second);

#endif
