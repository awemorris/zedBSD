/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The executor's render passes and framebuffers (see render-pass.h).
 *
 * Every command is decoded exactly as libvulkan encodes it: the records
 * through the generated codec, the framing around them as a generic create.
 */

#include "render-pass.h"
#include "codec.h"
#include "forget.h"
#include "gfx.h"
#include "internal.h"
#include "object.h"
#include "reply.h"
#include <kern/kcrt.h>

#include <kern/klog.h>
#include <kern/kmem.h>

#include <libc/vulkan/vulkan_core.h>

#include <uapi/errno.h>
#include <stddef.h>
#include <stdint.h>

#include "vulkan-codec.inc"

static int i915_gfx_render_pass_supported(const VkRenderPassCreateInfo *info);

/*
 * Creates a VkRenderPass: vkCreateRenderPass, a generic create.
 *
 * XXX: one subpass, at most four colour attachments and one depth
 * attachment.  Anything else is refused by name.  The pass keeps each
 * attachment's format and load operation, and which attachments the
 * subpass writes.
 */
int
drv_i915_gfx_create_render_pass(
	struct i915_render_session *session,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	VkRenderPassCreateInfo info;
	const VkSubpassDescription *subpass;
	struct i915_gfx_pass *pass;
	uint64_t identity;
	uint32_t index;
	uint32_t color_count;
	int supported;
	int error;

	/* Decodes the create info behind the device and its presence marker. */
	kern_memset(&info, 0, sizeof(info));
	(void)drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u64(reader);
	i915_vkc_dec_VkRenderPassCreateInfo(reader, &session->arena, &info);
	identity = drv_i915_gfx_create_tail(reader);
	if (reader->error != 0)
		return EINVAL;

	/* Decides whether the pass has the one shape the executor supports. */
	supported = i915_gfx_render_pass_supported(&info);

	/* Refuses any other pass by name, and allocates a supported one. */
	pass = NULL;
	error = 0;
	if (supported == 0) {
		/* The colour attachment count is reported only when a subpass exists. */
		color_count = 0U;
		if (info.subpassCount != 0U)
			color_count = info.pSubpasses[0].colorAttachmentCount;

		kern_logf("i915: vk: XXX vkCreateRenderPass refused: %u subpasses, %u attachments, %u colour attachments\n",
			  info.subpassCount,
			  info.attachmentCount,
			  color_count);
		error = ENOTSUP;
	} else {
		pass = kern_calloc(1U, sizeof(*pass));
	}

	/* Keeps each attachment's format and load operation. */
	if (pass != NULL) {
		pass->attachment_count = info.attachmentCount;
		for (index = 0U; index < info.attachmentCount; index++) {
			pass->attachments[index].format = info.pAttachments[index].format;
			pass->attachments[index].load_op = info.pAttachments[index].loadOp;
			pass->attachments[index].stencil_load_op = info.pAttachments[index].stencilLoadOp;
		}

		/* Records the colour attachments the subpass writes, if any. */
		subpass = &info.pSubpasses[0];
		pass->color_count = subpass->colorAttachmentCount;
		for (index = 0U; index < subpass->colorAttachmentCount; index++)
			pass->color_attachments[index] = subpass->pColorAttachments[index].attachment;
		pass->color_attachment = VK_ATTACHMENT_UNUSED;
		if (subpass->colorAttachmentCount != 0U)
			pass->color_attachment = subpass->pColorAttachments[0].attachment;

		/* Records the depth attachment the subpass writes, if any. */
		if (subpass->pDepthStencilAttachment != NULL) {
			pass->depth_attachment = subpass->pDepthStencilAttachment->attachment;
		} else {
			pass->depth_attachment = VK_ATTACHMENT_UNUSED;
		}
	}

	/* Publishes the pass and answers; a refused or failed pass is reported there. */
	drv_i915_gfx_create_reply(session, reply, I915_VK_OBJ_RENDER_PASS, identity, pass, error);

	/* Succeeded: the reply carries the result of the create. */
	return 0;
}

/*
 * Creates a VkFramebuffer: vkCreateFramebuffer, a generic create.
 *
 * The framebuffer keeps its extent and the view of each attachment; an
 * unknown view is kept as NULL.  More attachments than a framebuffer holds
 * are refused.
 */
int
drv_i915_gfx_create_framebuffer(
	struct i915_render_session *session,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	VkFramebufferCreateInfo info;
	struct i915_gfx_framebuffer *framebuffer;
	uint64_t identity;
	uint64_t view_id;
	uint32_t index;
	int error;

	/* Decodes the create info behind the device and its presence marker. */
	kern_memset(&info, 0, sizeof(info));
	(void)drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u64(reader);
	i915_vkc_dec_VkFramebufferCreateInfo(reader, &session->arena, &info);
	identity = drv_i915_gfx_create_tail(reader);
	if (reader->error != 0)
		return EINVAL;

	/* Refuses more attachments than a framebuffer holds, and allocates one that fits. */
	framebuffer = NULL;
	error = 0;
	if (info.attachmentCount > I915_GFX_MAX_ATTACHMENTS) {
		error = ENOTSUP;
	} else {
		framebuffer = kern_calloc(1U, sizeof(*framebuffer));
	}

	/* Keeps the extent and resolves the view of each attachment. */
	if (framebuffer != NULL) {
		framebuffer->width = info.width;
		framebuffer->height = info.height;
		framebuffer->layers = info.layers;
		if (framebuffer->layers == 0U)
			framebuffer->layers = 1U;
		framebuffer->view_count = info.attachmentCount;
		for (index = 0U; index < info.attachmentCount; index++) {
			/* The decoded handles are the wire's 64-bit identities, eight bytes apart. */
			kern_memcpy(&view_id, (const char *)info.pAttachments + index * 8U, sizeof(view_id));
			framebuffer->views[index] = drv_i915_gfx_view_lookup(session, view_id);
		}
	}

	/* Publishes the framebuffer and answers; a refused or failed one is reported there. */
	drv_i915_gfx_create_reply(session, reply, I915_VK_OBJ_FRAMEBUFFER, identity, framebuffer, error);

	/* Succeeded: the reply carries the result of the create. */
	return 0;
}

/* Decides whether a render pass has the one shape the executor supports. */
static int
i915_gfx_render_pass_supported(
	const VkRenderPassCreateInfo *info)
{
	/* Only one subpass. */
	if (info->subpassCount != 1U)
		return 0;

	/* Only as many attachments as a pass holds. */
	if (info->attachmentCount > I915_GFX_MAX_ATTACHMENTS)
		return 0;

	/* Only as many colour attachments in the one subpass as there are render targets. */
	if (info->pSubpasses[0].colorAttachmentCount > I915_GFX_MAX_COLOR_ATTACHMENTS)
		return 0;

	/* Succeeded: the pass has the supported shape. */
	return 1;
}

/*
 * Reports the attachment colour slot `slot` of a pass draws into, or
 * VK_ATTACHMENT_UNUSED.  Slot 0 is color_attachment, so a pass filled by
 * hand with only that field (a test's) has its one colour attachment.
 */
uint32_t
drv_i915_gfx_pass_color(
	const struct i915_gfx_pass *pass,
	uint32_t slot)
{
	/* The first slot is the pass's first colour attachment. */
	if (slot == 0U)
		return pass->color_attachment;

	/* A later slot the subpass does not have draws nothing. */
	if (slot >= pass->color_count || slot >= I915_GFX_MAX_COLOR_ATTACHMENTS)
		return VK_ATTACHMENT_UNUSED;

	/* Succeeded: the subpass's attachment of the slot. */
	return pass->color_attachments[slot];
}
