/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The executor's render passes and framebuffers.
 *
 * A render pass is one subpass writing at most AMDRDNA_VK_COLOURS_MAX
 * colour attachments (one colour target each) and at most one depth
 * attachment (D16_UNORM, D32_SFLOAT, or D32_SFLOAT_S8_UINT with its
 * stencil), all of one sample or all of AMDRDNA_VK_SAMPLES_MAX, the
 * colour attachments of samples each resolved into an attachment of one
 * sample of their format or not; anything else is refused by name.  A
 * framebuffer holds the views of its attachments, each of an image of
 * four-byte pixels in a colour format, of an image of one sample in a
 * storage format (its integer, 16-bit or 32-bit channels), or of a depth
 * format: a plane of a two-plane image, or an image of one or two bytes a
 * texel, is never drawn into.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <kern/kcrt.h>
#include <kern/klog.h>
#include <kern/kmem.h>
#include <uapi/errno.h>
#include <uapi/gpu-op.h>

#include "drivers/gpu/amdrdna/vk/amdrdna-vk-objects.h"

#include "drivers/gpu/i915/vk/vk-codec.inc"

static int target_create_pass(struct amdrdna_vk_session *session, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static int target_pass_supported(const VkRenderPassCreateInfo *info);
static uint32_t target_pass_samples(const VkRenderPassCreateInfo *info);
static int target_create_framebuffer(struct amdrdna_vk_session *session, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static int target_granularity(struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static int target_framebuffer_destroy(struct amdrdna_vk_session *session, void *payload);
static int target_attachment_view(const struct amdrdna_vk_view *view, uint32_t layers);

/*
 * Runs a render pass or framebuffer command; *handled stays clear for an
 * opcode of another handler's.
 */
int
amdrdna_vk_target_route(
	struct amdrdna_vk_session *session,
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
		error = amdrdna_vk_destroy_plain(session, reader, I915_VK_OBJ_RENDER_PASS);
		break;
	case GPU_OP_CREATE_FRAMEBUFFER:
		error = target_create_framebuffer(session, reader, reply);
		break;
	case GPU_OP_DESTROY_FRAMEBUFFER:
		error = amdrdna_vk_destroy_plain(session, reader, I915_VK_OBJ_FRAMEBUFFER);
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
	struct amdrdna_vk_session *session,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	VkRenderPassCreateInfo info;
	const VkSubpassDescription *subpass;
	const VkRenderPassMultiviewCreateInfo *multiview;
	struct amdrdna_vk_render_pass *pass;
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
	identity = amdrdna_vk_create_tail(reader);
	if (reader->error != 0)
		return EINVAL;

	/* Refuses by name a pass of another shape. */
	error = 0;
	supported = target_pass_supported(&info);
	if (supported == 0) {
		kern_logf("amdrdna: vk: vkCreateRenderPass refused: %u subpasses, %u attachments\n",
			  info.subpassCount,
			  info.attachmentCount);
		error = ENOTSUP;
	}

	/* The pass keeps its colour attachments and whether it writes a depth attachment: their numbers, formats and load operations (the stencil's too). */
	pass = NULL;
	if (error == 0) {
		pass = kern_calloc(1U, sizeof(*pass));
		subpass = &info.pSubpasses[0];
		if (pass != NULL)
			pass->samples = target_pass_samples(&info);
		for (index = 0U; pass != NULL && index < subpass->colorAttachmentCount; index++) {
			attachment = subpass->pColorAttachments[index].attachment;
			pass->colour_attachments[index] = attachment;
			pass->resolve_attachments[index] = VK_ATTACHMENT_UNUSED;
			if (subpass->pResolveAttachments != NULL)
				pass->resolve_attachments[index] = subpass->pResolveAttachments[index].attachment;
			if (attachment != VK_ATTACHMENT_UNUSED) {
				pass->formats[index] = info.pAttachments[attachment].format;
				pass->load_ops[index] = info.pAttachments[attachment].loadOp;
			}
		}
		if (pass != NULL)
			pass->colour_count = subpass->colorAttachmentCount;
		multiview = i915_vkc_render_pass_multiview(&info);
		if (pass != NULL && multiview != NULL && multiview->subpassCount != 0U)
			pass->view_mask = multiview->pViewMasks[0];
		if (pass != NULL && subpass->pDepthStencilAttachment != NULL) {
			attachment = subpass->pDepthStencilAttachment->attachment;
			if (attachment != VK_ATTACHMENT_UNUSED) {
				pass->depth = 1U;
				pass->depth_attachment = attachment;
				pass->depth_format = info.pAttachments[attachment].format;
				pass->depth_load_op = info.pAttachments[attachment].loadOp;
				pass->stencil_load_op = info.pAttachments[attachment].stencilLoadOp;
			}
		}
	}

	/* Publishes the pass and answers. */
	amdrdna_vk_create_reply(session, reply, I915_VK_OBJ_RENDER_PASS, identity, pass, amdrdna_vk_free_payload, error);

	/* Succeeded: the reply carries the create's result. */
	return 0;
}

/*
 * Reports the samples of a one-subpass render pass's attachments: those
 * of its first colour or depth attachment, or one without any.
 */
static uint32_t
target_pass_samples(
	const VkRenderPassCreateInfo *info)
{
	const VkSubpassDescription *subpass;
	uint32_t attachment;
	uint32_t index;

	/* The first colour attachment's. */
	subpass = &info->pSubpasses[0];
	for (index = 0U; index < subpass->colorAttachmentCount; index++) {
		attachment = subpass->pColorAttachments[index].attachment;
		if (attachment != VK_ATTACHMENT_UNUSED && attachment < info->attachmentCount)
			return (uint32_t)info->pAttachments[attachment].samples;
	}

	/* Else the depth attachment's. */
	if (subpass->pDepthStencilAttachment != NULL) {
		attachment = subpass->pDepthStencilAttachment->attachment;
		if (attachment != VK_ATTACHMENT_UNUSED && attachment < info->attachmentCount)
			return (uint32_t)info->pAttachments[attachment].samples;
	}

	/* Succeeded: one sample without any. */
	return 1U;
}

/*
 * Reports whether a render pass is one subpass writing at most
 * AMDRDNA_VK_COLOURS_MAX colour attachments, each in a colour format, and
 * at most one depth attachment, in a depth format, all of one sample or
 * all of AMDRDNA_VK_SAMPLES_MAX; each colour attachment of samples
 * resolved (or not) into an attachment of one sample in its format; and
 * nothing else.
 *
 * Values referenced: the Vulkan 1.3 specification (VkSubpassDescription:
 * the colour and depth attachments of a subpass of one sample count; a
 * resolve attachment of one sample and the format of its colour
 * attachment, which has more than one).
 */
static int
target_pass_supported(
	const VkRenderPassCreateInfo *info)
{
	const VkSubpassDescription *subpass;
	const VkRenderPassMultiviewCreateInfo *multiview;
	uint32_t attachment;
	uint32_t resolve;
	uint32_t samples;
	uint32_t index;
	bool colour;
	bool drawn;
	bool depth;

	/* One subpass, at most the colour attachments and a depth attachment. */
	if (info->subpassCount != 1U || info->pSubpasses == NULL)
		return 0;
	if (info->attachmentCount > AMDRDNA_VK_ATTACHMENTS_MAX)
		return 0;
	if (info->attachmentCount != 0U && info->pAttachments == NULL)
		return 0;

	/* Only the views the device reports (maxMultiviewViewCount), named for the one subpass. */
	multiview = i915_vkc_render_pass_multiview(info);
	if (multiview != NULL && multiview->subpassCount != 0U) {
		if (multiview->subpassCount != 1U || multiview->pViewMasks == NULL)
			return 0;
		if ((multiview->pViewMasks[0] >> AMDRDNA_VK_VIEWS_MAX) != 0U)
			return 0;
	}

	/* No inputs, at most the colour targets there are. */
	subpass = &info->pSubpasses[0];
	if (subpass->inputAttachmentCount != 0U)
		return 0;
	if (subpass->colorAttachmentCount > AMDRDNA_VK_COLOURS_MAX)
		return 0;
	if (subpass->colorAttachmentCount != 0U && subpass->pColorAttachments == NULL)
		return 0;

	/* One sample or the most, every attachment alike. */
	samples = target_pass_samples(info);
	if (samples != 1U && samples != AMDRDNA_VK_SAMPLES_MAX)
		return 0;

	/*
	 * Each colour attachment, unless unused, is the pass's attachment in a
	 * format the colour unit draws into, of the samples (a storage
	 * format's of one).
	 */
	for (index = 0U; index < subpass->colorAttachmentCount; index++) {
		attachment = subpass->pColorAttachments[index].attachment;
		if (attachment == VK_ATTACHMENT_UNUSED)
			continue;
		if (attachment >= info->attachmentCount)
			return 0;
		drawn = amdrdna_vk_format_drawn(info->pAttachments[attachment].format);
		colour = amdrdna_vk_format_colour(info->pAttachments[attachment].format);
		if (!drawn || (!colour && samples != 1U))
			return 0;
		if ((uint32_t)info->pAttachments[attachment].samples != samples)
			return 0;
	}

	/* Each resolve, unless unused, of a colour attachment of samples into one of one sample in its format. */
	for (index = 0U; subpass->pResolveAttachments != NULL && index < subpass->colorAttachmentCount; index++) {
		resolve = subpass->pResolveAttachments[index].attachment;
		if (resolve == VK_ATTACHMENT_UNUSED)
			continue;
		attachment = subpass->pColorAttachments[index].attachment;
		if (attachment == VK_ATTACHMENT_UNUSED || resolve >= info->attachmentCount || samples == 1U)
			return 0;
		if (info->pAttachments[resolve].samples != VK_SAMPLE_COUNT_1_BIT)
			return 0;
		if (info->pAttachments[resolve].format != info->pAttachments[attachment].format)
			return 0;
	}

	/* The depth attachment, unless there is none, is the pass's attachment in a depth format of the samples. */
	if (subpass->pDepthStencilAttachment != NULL) {
		attachment = subpass->pDepthStencilAttachment->attachment;
		if (attachment == VK_ATTACHMENT_UNUSED)
			return 1;
		if (attachment >= info->attachmentCount)
			return 0;
		depth = amdrdna_vk_format_depth(info->pAttachments[attachment].format);
		if (!depth)
			return 0;
		if ((uint32_t)info->pAttachments[attachment].samples != samples)
			return 0;
	}

	/* Succeeded: the pass has a supported shape. */
	return 1;
}

/*
 * vkCreateFramebuffer: [device][present][VkFramebufferCreateInfo] and the
 * create's tail -> the create's reply.  The framebuffer holds the views of
 * its attachments.
 */
static int
target_create_framebuffer(
	struct amdrdna_vk_session *session,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	VkFramebufferCreateInfo info;
	struct amdrdna_vk_framebuffer *framebuffer;
	struct amdrdna_vk_object *views[AMDRDNA_VK_ATTACHMENTS_MAX];
	const struct amdrdna_vk_render_pass *pass;
	uint64_t identity;
	uint64_t view_id;
	uint32_t layers;
	uint32_t index;
	uint32_t held;
	int error;

	/* The create info behind the device and its presence word. */
	kern_memset(&info, 0, sizeof(info));
	(void)drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u64(reader);
	i915_vkc_dec_VkFramebufferCreateInfo(reader, &session->arena, &info);
	identity = amdrdna_vk_create_tail(reader);
	if (reader->error != 0)
		return EINVAL;

	/*
	 * The layers each view has: one, or under a multiview pass more than
	 * its highest view.
	 */
	layers = 1U;
	pass = amdrdna_vk_payload(session, I915_VK_OBJ_RENDER_PASS, (uint64_t)(uintptr_t)info.renderPass);
	if (pass != NULL && pass->view_mask != 0U)
		layers = 32U - (uint32_t)__builtin_clz(pass->view_mask);

	/* At most the attachments a pass has, known views, of a size an image may have. */
	error = 0;
	kern_memset(views, 0, sizeof(views));
	if (info.attachmentCount > AMDRDNA_VK_ATTACHMENTS_MAX) {
		error = ENOTSUP;
	} else if (info.width == 0U || info.width > AMDRDNA_VK_IMAGE_SIDE_MAX) {
		error = EINVAL;
	} else if (info.height == 0U || info.height > AMDRDNA_VK_IMAGE_SIDE_MAX) {
		error = EINVAL;
	} else if (info.layers != 1U) {
		error = ENOTSUP;
	}
	for (index = 0U; error == 0 && index < info.attachmentCount; index++) {
		/* The decoded handles are the wire's 64-bit identities. */
		kern_memcpy(&view_id, (const uint8_t *)info.pAttachments + (size_t)index * sizeof(view_id), sizeof(view_id));
		views[index] = amdrdna_vk_object_find(session, I915_VK_OBJ_IMAGE_VIEW, view_id);
		if (views[index] == NULL) {
			error = EINVAL;
			break;
		}

		/* Only a view the colour target or the depth unit writes: never a plane, nor one or two bytes a texel. */
		error = target_attachment_view(views[index]->payload, layers);
	}

	/* The framebuffer holds its views. */
	held = 0U;
	for (index = 0U; error == 0 && index < info.attachmentCount; index++) {
		error = amdrdna_vk_object_retain(views[index]);
		if (error == 0)
			held++;
	}
	framebuffer = NULL;
	if (error == 0)
		framebuffer = kern_calloc(1U, sizeof(*framebuffer));

	/* A framebuffer that could not be made lets the views it took go. */
	if (framebuffer == NULL) {
		for (index = 0U; index < held; index++)
			(void)amdrdna_vk_object_release(views[index]);
	}

	/* Its size and its views. */
	if (framebuffer != NULL) {
		framebuffer->width = info.width;
		framebuffer->height = info.height;
		framebuffer->count = info.attachmentCount;
		kern_memcpy(framebuffer->views, views, sizeof(views));
	}

	/* Publishes the framebuffer and answers. */
	amdrdna_vk_create_reply(session, reply, I915_VK_OBJ_FRAMEBUFFER, identity, framebuffer, target_framebuffer_destroy, error);

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
	struct amdrdna_vk_session *session,
	void *payload)
{
	struct amdrdna_vk_framebuffer *framebuffer;
	uint32_t index;
	int released;
	int error;

	UNUSED_PARAMETER(session);

	framebuffer = payload;

	/* Each view may go with it when nothing else holds the view; the first failure is reported. */
	error = 0;
	for (index = 0U; index < framebuffer->count; index++) {
		released = amdrdna_vk_object_release(framebuffer->views[index]);
		if (released != 0 && error == 0)
			error = released;
	}
	kern_free(framebuffer);

	/* Reports a view whose destruction failed. */
	if (error != 0)
		return error;

	/* Succeeded: the framebuffer is gone. */
	return 0;
}

/*
 * Reports whether a view may be an attachment: one of one layer (of a 2D
 * image: a framebuffer has one layer), or of at least layers layers for a
 * multiview pass, which draws each view into its layer, in a format the colour unit draws
 * into or a depth format, of an image of four-byte pixels or of a storage
 * format (whose copies are moves).  Returns 0, or ENOTSUP for a view the
 * colour target and the depth unit do not write.
 */
static int
target_attachment_view(
	const struct amdrdna_vk_view *view,
	uint32_t layers)
{
	const struct amdrdna_vk_image *image;
	uint32_t texel_bytes;
	bool storage;
	bool drawn;
	bool depth;
	bool moved;

	/* An image the executor draws into, one layer of it (a multiview pass's views' layers). */
	image = view->image->payload;
	moved = amdrdna_vk_image_moved(image);
	storage = amdrdna_vk_format_storage(image->format, &texel_bytes);
	if (moved && !storage)
		return ENOTSUP;
	if (view->layer_count < layers || (layers == 1U && view->layer_count != 1U))
		return ENOTSUP;
	if (view->type != VK_IMAGE_VIEW_TYPE_2D && view->type != VK_IMAGE_VIEW_TYPE_2D_ARRAY)
		return ENOTSUP;

	/* Seen in a format the colour target or the depth unit writes. */
	drawn = amdrdna_vk_format_drawn(view->format);
	depth = amdrdna_vk_format_depth(view->format);
	if (!drawn && !depth)
		return ENOTSUP;

	/* Succeeded: the view may be an attachment. */
	return 0;
}
