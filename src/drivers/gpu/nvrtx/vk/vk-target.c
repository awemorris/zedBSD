/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The executor's render passes and framebuffers.
 *
 * A render pass is one subpass writing at most four colour attachments
 * (the first used when the others are) and at most one depth attachment;
 * anything else is refused by name.  A framebuffer holds the views of its
 * attachments.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <kern/kcrt.h>
#include <kern/klog.h>
#include <kern/kmem.h>
#include <uapi/errno.h>
#include <uapi/gpu-op.h>

#include "drivers/gpu/nvrtx/vk/nvrtx-vk-objects.h"

#include "drivers/gpu/i915/vk/vk-codec.inc"

static int target_create_pass(struct nvrtx_vk_session *session, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static uint32_t target_pass_samples(const VkRenderPassCreateInfo *info);
static int target_pass_supported(const VkRenderPassCreateInfo *info);
static int target_create_framebuffer(struct nvrtx_vk_session *session, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static int target_granularity(struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static int target_framebuffer_destroy(struct nvrtx_vk_session *session, void *payload);

/*
 * Runs a render pass or framebuffer command; *handled stays clear for an
 * opcode of another handler's.
 */
int
nvrtx_vk_target_route(
	struct nvrtx_vk_session *session,
	uint32_t opcode,
	uint32_t requested,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply,
	int *handled)
{
	int error;

	UNUSED_PARAMETER(requested);

	/* Picks the command, or leaves it to another handler. */
	*handled = 1;
	switch (opcode) {
	case GPU_OP_CREATE_RENDER_PASS:
		error = target_create_pass(session, reader, reply);
		break;
	case GPU_OP_DESTROY_RENDER_PASS:
		error = nvrtx_vk_destroy_plain(session, reader, I915_VK_OBJ_RENDER_PASS);
		break;
	case GPU_OP_CREATE_FRAMEBUFFER:
		error = target_create_framebuffer(session, reader, reply);
		break;
	case GPU_OP_DESTROY_FRAMEBUFFER:
		error = nvrtx_vk_destroy_plain(session, reader, I915_VK_OBJ_FRAMEBUFFER);
		break;
	case GPU_OP_GET_RENDER_AREA_GRANULARITY:
		error = target_granularity(reader, reply);
		break;
	default:
		*handled = 0;
		return 0;
	}

	/* Reports a command that could not be decoded. */
	if (error != 0)
		return error;

	/* Succeeded: the command ran and wrote its reply. */
	return 0;
}

/*
 * vkCreateRenderPass: [device][present][VkRenderPassCreateInfo] and the
 * create's tail -> the create's reply.
 */
static int
target_create_pass(
	struct nvrtx_vk_session *session,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	VkRenderPassCreateInfo info;
	const VkSubpassDescription *subpass;
	struct nvrtx_vk_render_pass *pass;
	uint64_t identity;
	uint32_t attachment;
	uint32_t index;
	int supported;
	int error;

	/* The create info behind the device and its presence word. */
	kern_memset(&info, 0, sizeof(info));
	(void)drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u64(reader);
	i915_vkc_dec_VkRenderPassCreateInfo(reader, &session->arena, &info);
	identity = nvrtx_vk_create_tail(reader);
	if (reader->error != 0)
		return EINVAL;

	/* Refuses by name a pass of another shape. */
	error = 0;
	supported = target_pass_supported(&info);
	if (supported == 0) {
		kern_logf("nvrtx: vk: vkCreateRenderPass refused: %u subpasses, %u attachments\n",
			  info.subpassCount,
			  info.attachmentCount);
		error = ENOTSUP;
	}

	/*
	 * The pass keeps how many colour attachments it writes (none when its
	 * one is unused) and a depth attachment, and of each its number,
	 * format and load operation; the samples a pixel of its attachments,
	 * and the attachment each colour attachment is resolved into (unused
	 * for none).
	 */
	pass = NULL;
	if (error == 0) {
		pass = kern_calloc(1U, sizeof(*pass));
		subpass = &info.pSubpasses[0];
		for (index = 0U; pass != NULL && index < NVRTX_EXEC_COLOURS; index++)
			pass->resolves[index] = VK_ATTACHMENT_UNUSED;
		if (pass != NULL)
			pass->samples = target_pass_samples(&info);
		for (index = 0U; pass != NULL && index < subpass->colorAttachmentCount; index++) {
			attachment = subpass->pColorAttachments[index].attachment;
			pass->colour_attachments[index] = attachment;
			if (subpass->pResolveAttachments != NULL)
				pass->resolves[index] = subpass->pResolveAttachments[index].attachment;
			if (attachment == VK_ATTACHMENT_UNUSED)
				continue;
			pass->formats[index] = info.pAttachments[attachment].format;
			pass->load_ops[index] = info.pAttachments[attachment].loadOp;
		}
		if (pass != NULL && subpass->colorAttachmentCount != 0U && pass->colour_attachments[0] != VK_ATTACHMENT_UNUSED) {
			pass->colour = subpass->colorAttachmentCount;
			pass->colour_attachment = pass->colour_attachments[0];
			pass->format = pass->formats[0];
			pass->load_op = pass->load_ops[0];
		}
		if (pass != NULL && subpass->pDepthStencilAttachment != NULL) {
			attachment = subpass->pDepthStencilAttachment->attachment;
			if (attachment != VK_ATTACHMENT_UNUSED) {
				pass->depth = 1U;
				pass->depth_attachment = attachment;
				pass->depth_load_op = info.pAttachments[attachment].loadOp;
				pass->stencil_load_op = info.pAttachments[attachment].stencilLoadOp;
			}
		}
	}

	/* Publishes the pass and answers. */
	nvrtx_vk_create_reply(session, reply, I915_VK_OBJ_RENDER_PASS, identity, pass, nvrtx_vk_free_payload, error);

	/* Succeeded: the reply carries the create's result. */
	return 0;
}

/*
 * Reports the samples a pixel of a pass's attachments (the first used
 * one's; 1 for a pass of none).
 */
static uint32_t
target_pass_samples(
	const VkRenderPassCreateInfo *info)
{
	const VkSubpassDescription *subpass;
	uint32_t attachment;
	uint32_t index;

	/* The colour attachments, then the depth attachment. */
	subpass = &info->pSubpasses[0];
	for (index = 0U; index < subpass->colorAttachmentCount; index++) {
		attachment = subpass->pColorAttachments[index].attachment;
		if (attachment != VK_ATTACHMENT_UNUSED)
			return info->pAttachments[attachment].samples;
	}
	if (subpass->pDepthStencilAttachment != NULL && subpass->pDepthStencilAttachment->attachment != VK_ATTACHMENT_UNUSED)
		return info->pAttachments[subpass->pDepthStencilAttachment->attachment].samples;

	/* Succeeded: one sample. */
	return VK_SAMPLE_COUNT_1_BIT;
}

/*
 * Reports whether a render pass is one subpass writing at most four
 * colour attachments, in colour formats, the first used when another is,
 * and at most one depth attachment, in a depth format, every attachment
 * of one or four samples alike, a multisampled colour attachment resolved
 * (or not) into an attachment of one sample in its format, and nothing
 * else.
 */
static int
target_pass_supported(
	const VkRenderPassCreateInfo *info)
{
	const VkSubpassDescription *subpass;
	const VkAttachmentDescription *resolved;
	uint32_t attachment;
	uint32_t resolve;
	uint32_t samples;
	uint32_t index;
	bool colour;

	/* One subpass, at most five attachments. */
	if (info->subpassCount != 1U || info->pSubpasses == NULL)
		return 0;
	if (info->attachmentCount > NVRTX_VK_ATTACHMENTS_MAX)
		return 0;

	/* Every attachment of one or four samples, all alike. */
	samples = 0U;
	for (index = 0U; index < info->attachmentCount && info->pAttachments != NULL; index++) {
		if (info->pAttachments[index].samples != VK_SAMPLE_COUNT_1_BIT && info->pAttachments[index].samples != VK_SAMPLE_COUNT_4_BIT)
			return 0;
	}

	/* The depth attachment, when there is one, is the pass's attachment in a depth format. */
	subpass = &info->pSubpasses[0];
	if (subpass->pDepthStencilAttachment != NULL) {
		attachment = subpass->pDepthStencilAttachment->attachment;
		if (attachment != VK_ATTACHMENT_UNUSED) {
			if (attachment >= info->attachmentCount || info->pAttachments == NULL)
				return 0;
			if (!nvrtx_vk_format_depth(info->pAttachments[attachment].format))
				return 0;
			samples = info->pAttachments[attachment].samples;
		}
	}

	/* No inputs, at most four colour attachments. */
	if (subpass->inputAttachmentCount != 0U)
		return 0;
	if (subpass->colorAttachmentCount > NVRTX_EXEC_COLOURS)
		return 0;
	if (subpass->colorAttachmentCount != 0U && subpass->pColorAttachments == NULL)
		return 0;

	/* Each colour attachment used is the pass's attachment in a colour format; the first is used when another is. */
	for (index = 0U; index < subpass->colorAttachmentCount; index++) {
		attachment = subpass->pColorAttachments[index].attachment;
		if (attachment == VK_ATTACHMENT_UNUSED) {
			if (index == 0U && subpass->colorAttachmentCount > 1U)
				return 0;
			continue;
		}
		if (attachment >= info->attachmentCount || info->pAttachments == NULL)
			return 0;
		colour = nvrtx_vk_format_colour(info->pAttachments[attachment].format);
		if (!colour)
			return 0;
		if (samples != 0U && info->pAttachments[attachment].samples != samples)
			return 0;
		samples = info->pAttachments[attachment].samples;

		/* Its resolve: into an attachment of one sample in its format, of a multisampled one only. */
		if (subpass->pResolveAttachments == NULL)
			continue;
		resolve = subpass->pResolveAttachments[index].attachment;
		if (resolve == VK_ATTACHMENT_UNUSED)
			continue;
		if (resolve >= info->attachmentCount || samples == VK_SAMPLE_COUNT_1_BIT)
			return 0;
		resolved = &info->pAttachments[resolve];
		if (resolved->samples != VK_SAMPLE_COUNT_1_BIT || resolved->format != info->pAttachments[attachment].format)
			return 0;
	}

	/* Succeeded: the pass has the one supported shape. */
	return 1;
}

/*
 * vkCreateFramebuffer: [device][present][VkFramebufferCreateInfo] and the
 * create's tail -> the create's reply.  The framebuffer holds the views of
 * its attachments.
 */
static int
target_create_framebuffer(
	struct nvrtx_vk_session *session,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	VkFramebufferCreateInfo info;
	struct nvrtx_vk_framebuffer *framebuffer;
	struct nvrtx_vk_object *views[NVRTX_VK_ATTACHMENTS_MAX];
	const struct nvrtx_vk_view *view;
	uint64_t identity;
	uint64_t view_id;
	uint32_t index;
	uint32_t held;
	int error;

	/* The create info behind the device and its presence word. */
	kern_memset(&info, 0, sizeof(info));
	(void)drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u64(reader);
	i915_vkc_dec_VkFramebufferCreateInfo(reader, &session->arena, &info);
	identity = nvrtx_vk_create_tail(reader);
	if (reader->error != 0)
		return EINVAL;

	/* At most five attachments, known views, of a size an image may have, of layers an image may have. */
	error = 0;
	kern_memset(views, 0, sizeof(views));
	if (info.attachmentCount > NVRTX_VK_ATTACHMENTS_MAX) {
		error = ENOTSUP;
	} else if (info.width == 0U || info.width > NVRTX_VK_IMAGE_SIDE_MAX) {
		error = EINVAL;
	} else if (info.height == 0U || info.height > NVRTX_VK_IMAGE_SIDE_MAX) {
		error = EINVAL;
	} else if (info.layers == 0U || info.layers > NVRTX_VK_IMAGE_LAYERS_MAX) {
		error = EINVAL;
	}
	for (index = 0U; error == 0 && index < info.attachmentCount; index++) {
		/* The decoded handles are the wire's 64-bit identities. */
		kern_memcpy(&view_id, (const uint8_t *)info.pAttachments + index * sizeof(view_id), sizeof(view_id));
		views[index] = nvrtx_vk_object_find(session, I915_VK_OBJ_IMAGE_VIEW, view_id);
		if (views[index] == NULL) {
			/* Not a view of the session's. */
			error = EINVAL;
		} else {
			/* A view of the framebuffer's layers at least, not a 3D image's slices. */
			view = views[index]->payload;
			if (view->type == VK_IMAGE_VIEW_TYPE_3D)
				error = ENOTSUP;
			else if (view->layer_count < info.layers)
				error = EINVAL;
		}
	}

	/* The framebuffer holds its views. */
	held = 0U;
	while (error == 0 && held < info.attachmentCount) {
		error = nvrtx_vk_object_retain(views[held]);
		if (error == 0)
			held++;
	}
	framebuffer = NULL;
	if (error == 0) {
		framebuffer = kern_calloc(1U, sizeof(*framebuffer));
		if (framebuffer == NULL)
			error = ENOMEM;
	}
	if (error != 0) {
		for (index = 0U; index < held; index++)
			(void)nvrtx_vk_object_release(views[index]);
	}

	/* Its size and its views. */
	if (framebuffer != NULL) {
		framebuffer->width = info.width;
		framebuffer->height = info.height;
		framebuffer->layers = info.layers;
		framebuffer->view_count = info.attachmentCount;
		kern_memcpy(framebuffer->views, views, sizeof(views));
	}

	/* A refusal says why (the attachments, the size, the layers asked for). */
	if (error != 0) {
		kern_logf("nvrtx: vk: vkCreateFramebuffer refused (%d): %u attachments %ux%u layers %u\n", error,
			  (unsigned)info.attachmentCount, (unsigned)info.width, (unsigned)info.height, (unsigned)info.layers);
	}

	/* Publishes the framebuffer and answers. */
	nvrtx_vk_create_reply(session, reply, I915_VK_OBJ_FRAMEBUFFER, identity, framebuffer, target_framebuffer_destroy, error);

	/* Succeeded: the reply carries the create's result. */
	return 0;
}

/*
 * vkGetRenderAreaGranularity: [device][render pass][present] ->
 * [present][VkExtent2D].  Any pixel can start a render area.
 */
static int
target_granularity(
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	VkExtent2D extent;

	/* The device, the pass and the output's presence word. */
	(void)drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u64(reader);
	if (reader->error != 0)
		return EINVAL;

	/* One pixel by one. */
	extent.width = 1U;
	extent.height = 1U;
	drv_i915_wire_reply_u64(reply, 1U);
	i915_vkc_enc_VkExtent2D(reply, &extent);

	/* Succeeded: the reply carries the granularity. */
	return 0;
}

/* Destroys a framebuffer nothing holds: it lets its views go. */
static int
target_framebuffer_destroy(
	struct nvrtx_vk_session *session,
	void *payload)
{
	struct nvrtx_vk_framebuffer *framebuffer;
	uint32_t index;
	int released;
	int error;

	UNUSED_PARAMETER(session);

	framebuffer = payload;

	/* The views may go with it when nothing else holds them; the first failure is reported. */
	error = 0;
	for (index = 0U; index < framebuffer->view_count; index++) {
		released = nvrtx_vk_object_release(framebuffer->views[index]);
		if (error == 0)
			error = released;
	}
	kern_free(framebuffer);

	/* Reports a view whose destruction failed. */
	if (error != 0)
		return error;

	/* Succeeded: the framebuffer is gone. */
	return 0;
}
