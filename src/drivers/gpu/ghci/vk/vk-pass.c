/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Render passes and framebuffers of the Vulkan executor.
 *
 * A render pass keeps, for each subpass, what a pipeline made for it
 * needs (the formats and samples of its attachments) and which
 * attachments it names, and for each attachment its format, samples,
 * load and store operations and the first and last subpass that uses
 * it, and the views of a multiview pass.  A framebuffer keeps its views
 * by identity and its size.  The submission turns each subpass into a
 * render pass description of the hardware (vk-execute.c), drawing a
 * multiview pass once per view.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <kern/kcrt.h>
#include <kern/kmem.h>
#include <uapi/errno.h>
#include <uapi/gpu-op.h>

#include "vk.h"

#include "drivers/gpu/i915/vk/vk-codec.inc"

/* The most attachments and subpasses one render pass keeps. */
#define PASS_ATTACHMENTS_MAX		32U
#define PASS_SUBPASSES_MAX		16U

/* The render area granularity: any area is drawn as it is. */
#define PASS_GRANULARITY		1U

static int create_render_pass(struct ghci_vk_session *session, uint32_t requested, struct i915_wire_reader *reader, struct i915_wire_writer *reply, bool version2);
static void *pass2_counted(struct ghci_vk_session *session, struct i915_wire_reader *reader, const void *array, uint32_t count, size_t element);
static void pass2_count(struct ghci_vk_session *session, struct i915_wire_reader *reader, VkRenderPassCreateInfo *info);
static uint32_t pass_build(struct ghci_vk_session *session, const VkRenderPassCreateInfo *info, struct ghci_vk_render_pass *pass);
static int pass_subpass(struct ghci_vk_session *session, const VkRenderPassCreateInfo *info, uint32_t index, struct ghci_vk_render_pass *pass);
static int pass_reference(const VkRenderPassCreateInfo *info, const VkAttachmentReference *reference, uint32_t index, struct ghci_vk_render_pass *pass, uint32_t *attachment);
static int pass_views(const VkRenderPassCreateInfo *info, struct ghci_vk_render_pass *pass);
static int create_framebuffer(struct ghci_vk_session *session, uint32_t requested, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static int destroy_pass_object(struct ghci_vk_session *session, enum ghci_vk_kind kind, struct i915_wire_reader *reader);
static int granularity(struct ghci_vk_session *session, struct i915_wire_reader *reader, struct i915_wire_writer *reply, uint32_t requested);
static void pass_free(struct ghci_vk_render_pass *pass);

/*
 * Runs a render pass or framebuffer command; *handled stays clear for
 * another group's number.
 */
int
ghci_vk_pass_route(
	struct ghci_vk_session *session,
	uint32_t opcode,
	uint32_t requested,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply,
	int *handled)
{
	int error;

	/* Picks the command, or leaves it to another group. */
	*handled = 1;
	switch (opcode) {
	case GPU_OP_CREATE_RENDER_PASS:
		error = create_render_pass(session, requested, reader, reply, false);
		break;
	case GPU_OP_CREATE_RENDER_PASS2:
		error = create_render_pass(session, requested, reader, reply, true);
		break;
	case GPU_OP_DESTROY_RENDER_PASS:
		error = destroy_pass_object(session, GHCI_VK_RENDER_PASS, reader);
		break;
	case GPU_OP_CREATE_FRAMEBUFFER:
		error = create_framebuffer(session, requested, reader, reply);
		break;
	case GPU_OP_DESTROY_FRAMEBUFFER:
		error = destroy_pass_object(session, GHCI_VK_FRAMEBUFFER, reader);
		break;
	case GPU_OP_GET_RENDER_AREA_GRANULARITY:
		error = granularity(session, reader, reply, requested);
		break;
	default:
		*handled = 0;
		return 0;
	}

	/* A command that could not be decoded or run. */
	if (error != 0)
		return error;

	/* Succeeded: the command ran. */
	return 0;
}

/* Lets a render pass or a framebuffer go: its records. */
void
ghci_vk_pass_release(
	struct ghci_vk_object *object)
{
	/* What each kind holds. */
	if (object->kind == GHCI_VK_RENDER_PASS) {
		pass_free(object->as.render_pass);
		object->as.render_pass = NULL;
	} else if (object->kind == GHCI_VK_FRAMEBUFFER) {
		kern_free(object->as.framebuffer.views);
		object->as.framebuffer.views = NULL;
	}
}

/*
 * vkCreateRenderPass: the create form with VkRenderPassCreateInfo; with
 * version2, vkCreateRenderPass2 (GPU_OP_CREATE_RENDER_PASS2), whose record
 * is read in its Vulkan 1.0 form with the resolves of its depth and
 * stencil aside.  A pass the device cannot draw (more colour attachments than it has, a
 * sample count it does not take) is answered
 * VK_ERROR_FEATURE_NOT_PRESENT; one past the executor's bounds
 * VK_ERROR_OUT_OF_HOST_MEMORY; a reference to an attachment the pass
 * does not have ends the frame (EINVAL).
 */
static int
create_render_pass(
	struct ghci_vk_session *session,
	uint32_t requested,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply,
	bool version2)
{
	const VkSubpassDescriptionDepthStencilResolve *resolves;
	struct i915_vkc_depth_resolve resolve;
	VkRenderPassCreateInfo info;
	struct ghci_vk_render_pass *pass;
	struct ghci_vk_object *device;
	struct ghci_vk_object *object;
	uint64_t handle;
	uint64_t present;
	uint64_t identity;
	uint64_t charged;
	uint32_t result;
	uint32_t index;
	int error;

	/* The device, the create info and the tail. */
	kern_memset(&info, 0, sizeof(info));
	resolves = NULL;
	handle = drv_i915_wire_read_u64(reader);
	present = drv_i915_wire_read_u64(reader);
	if (present == 1U && version2) {
		i915_vkc_read_render_pass2(reader, &session->arena, &info, &resolves);
		pass2_count(session, reader, &info);
	}
	else if (present == 1U)
		i915_vkc_dec_VkRenderPassCreateInfo(reader, &session->arena, &info);
	identity = ghci_vk_create_tail(reader);
	if (reader->error != 0 || present != 1U)
		return EINVAL;
	device = ghci_vk_object_find(session, GHCI_VK_DEVICE, handle);
	if (device == NULL)
		return EINVAL;
	if (info.pAttachments != NULL && ghci_vk_decoded_count(session, info.pAttachments) != info.attachmentCount)
		return EINVAL;
	if (info.pSubpasses != NULL && ghci_vk_decoded_count(session, info.pSubpasses) != info.subpassCount)
		return EINVAL;
	if ((info.attachmentCount != 0U && info.pAttachments == NULL) || info.subpassCount == 0U || info.pSubpasses == NULL)
		return EINVAL;

	/* Each subpass's resolve of its depth and stencil is one the executor does. */
	for (index = 0U; index < info.subpassCount; index++) {
		error = i915_vkc_depth_resolve(&info, resolves, index, &resolve);
		if (error == EINVAL)
			return EINVAL;
		if (error != 0) {
			ghci_vk_reply_created(reply, requested, (uint32_t)VK_ERROR_FEATURE_NOT_PRESENT, identity);
			return 0;
		}
	}

	/* The record, within the bounds and the session's budget. */
	if (info.attachmentCount > PASS_ATTACHMENTS_MAX || info.subpassCount > PASS_SUBPASSES_MAX) {
		ghci_vk_reply_created(reply, requested, (uint32_t)VK_ERROR_OUT_OF_HOST_MEMORY, identity);
		return 0;
	}
	charged = sizeof(*pass) + (uint64_t)info.subpassCount * (sizeof(struct ghci_vk_subpass) + sizeof(struct ghci_vk_pass_references))
		+ (uint64_t)info.attachmentCount * sizeof(struct ghci_vk_pass_attachment);
	if (ghci_vk_budget_take(session, charged) != 0) {
		ghci_vk_reply_created(reply, requested, (uint32_t)VK_ERROR_OUT_OF_HOST_MEMORY, identity);
		return 0;
	}
	pass = kern_calloc(1U, sizeof(*pass));
	if (pass != NULL) {
		pass->subpasses = kern_calloc(info.subpassCount, sizeof(*pass->subpasses));
		pass->references = kern_calloc(info.subpassCount, sizeof(*pass->references));
		if (info.attachmentCount != 0U)
			pass->attachments = kern_calloc(info.attachmentCount, sizeof(*pass->attachments));
	}
	if (pass == NULL || pass->subpasses == NULL || pass->references == NULL || (info.attachmentCount != 0U && pass->attachments == NULL)) {
		pass_free(pass);
		ghci_vk_budget_give(session, charged);
		ghci_vk_reply_created(reply, requested, (uint32_t)VK_ERROR_OUT_OF_HOST_MEMORY, identity);
		return 0;
	}

	/* The attachments and subpasses; a malformed one ends the frame. */
	result = pass_build(session, &info, pass);
	if (result == UINT32_MAX) {
		pass_free(pass);
		ghci_vk_budget_give(session, charged);
		return EINVAL;
	}
	if (result != (uint32_t)VK_SUCCESS) {
		pass_free(pass);
		ghci_vk_budget_give(session, charged);
		ghci_vk_reply_created(reply, requested, result, identity);
		return 0;
	}

	/* The attachment each subpass resolves its depth and stencil into (VK_ATTACHMENT_UNUSED for none). */
	for (index = 0U; index < info.subpassCount; index++) {
		(void)i915_vkc_depth_resolve(&info, resolves, index, &resolve);
		pass->references[index].depth_resolve = resolve.target;
	}

	/* The object. */
	error = ghci_vk_object_add(session, GHCI_VK_RENDER_PASS, identity, device->device, &object);
	if (error != 0) {
		pass_free(pass);
		ghci_vk_budget_give(session, charged);
		return error;
	}
	object->as.render_pass = pass;
	object->charged = charged;
	ghci_vk_reply_created(reply, requested, (uint32_t)VK_SUCCESS, identity);

	/* Succeeded: the render pass exists. */
	return 0;
}

/*
 * Gives the arrays of a render pass the shared codec read in its Vulkan
 * 1.0 form (i915_vkc_read_render_pass2, whose arrays are as long as their
 * counts) the decoded lengths the executor's checks read: each is copied
 * into an array of ghci_vk_decode_array().
 */
static void
pass2_count(
	struct ghci_vk_session *session,
	struct i915_wire_reader *reader,
	VkRenderPassCreateInfo *info)
{
	VkSubpassDescription *subpasses;
	uint32_t index;

	/* The attachments and the subpasses. */
	info->pAttachments = pass2_counted(session, reader, info->pAttachments, info->attachmentCount, sizeof(*info->pAttachments));
	subpasses = pass2_counted(session, reader, info->pSubpasses, info->subpassCount, sizeof(*info->pSubpasses));
	info->pSubpasses = subpasses;

	/* Each subpass's input, colour and resolve attachments. */
	for (index = 0U; subpasses != NULL && index < info->subpassCount; index++) {
		subpasses[index].pInputAttachments = pass2_counted(session, reader, subpasses[index].pInputAttachments,
			subpasses[index].inputAttachmentCount, sizeof(VkAttachmentReference));
		subpasses[index].pColorAttachments = pass2_counted(session, reader, subpasses[index].pColorAttachments,
			subpasses[index].colorAttachmentCount, sizeof(VkAttachmentReference));
		subpasses[index].pResolveAttachments = pass2_counted(session, reader, subpasses[index].pResolveAttachments,
			subpasses[index].colorAttachmentCount, sizeof(VkAttachmentReference));
	}
}

/* Copies count elements of array into an array of ghci_vk_decode_array(); NULL for an absent one. */
static void *
pass2_counted(
	struct ghci_vk_session *session,
	struct i915_wire_reader *reader,
	const void *array,
	uint32_t count,
	size_t element)
{
	void *counted;

	/* An absent array stays absent. */
	if (array == NULL || count == 0U)
		return NULL;

	/* The copy, with its length kept before it. */
	counted = ghci_vk_decode_array(reader, &session->arena, count, element);
	if (counted == NULL)
		return NULL;
	kern_memcpy(counted, array, (size_t)count * element);

	/* Succeeded: the array's length is the executor's to check. */
	return counted;
}

/*
 * Fills a render pass from its create info: the attachments, then each
 * subpass.  Returns VK_SUCCESS, VK_ERROR_FEATURE_NOT_PRESENT for what
 * the device cannot draw, or UINT32_MAX for a malformed pass.
 */
static uint32_t
pass_build(
	struct ghci_vk_session *session,
	const VkRenderPassCreateInfo *info,
	struct ghci_vk_render_pass *pass)
{
	const VkAttachmentDescription *description;
	uint32_t supported;
	uint32_t index;
	int error;

	/* The attachments: a sample count the device takes, no subpass yet. */
	supported = ghci_vk_caps_sample_counts(&session->device->caps);
	pass->attachment_count = info->attachmentCount;
	for (index = 0U; index < info->attachmentCount; index++) {
		description = &info->pAttachments[index];
		if ((description->samples & supported) == 0U || (description->samples & (description->samples - 1U)) != 0U)
			return (uint32_t)VK_ERROR_FEATURE_NOT_PRESENT;
		if (description->loadOp > VK_ATTACHMENT_LOAD_OP_DONT_CARE || description->stencilLoadOp > VK_ATTACHMENT_LOAD_OP_DONT_CARE)
			return UINT32_MAX;
		if (description->storeOp > VK_ATTACHMENT_STORE_OP_DONT_CARE || description->stencilStoreOp > VK_ATTACHMENT_STORE_OP_DONT_CARE)
			return UINT32_MAX;
		pass->attachments[index].format = description->format;
		pass->attachments[index].samples = description->samples;
		pass->attachments[index].load = description->loadOp;
		pass->attachments[index].store = description->storeOp;
		pass->attachments[index].stencil_load = description->stencilLoadOp;
		pass->attachments[index].stencil_store = description->stencilStoreOp;
		pass->attachments[index].first = UINT32_MAX;
		pass->attachments[index].last = 0U;
	}

	/* Each subpass. */
	pass->subpass_count = info->subpassCount;
	for (index = 0U; index < info->subpassCount; index++) {
		error = pass_subpass(session, info, index, pass);
		if (error == ENOTSUP)
			return (uint32_t)VK_ERROR_FEATURE_NOT_PRESENT;
		if (error != 0)
			return UINT32_MAX;
	}

	/* The views of a multiview pass. */
	error = pass_views(info, pass);
	if (error == ENOTSUP)
		return (uint32_t)VK_ERROR_FEATURE_NOT_PRESENT;
	if (error != 0)
		return UINT32_MAX;

	/* Succeeded: the pass is whole. */
	return (uint32_t)VK_SUCCESS;
}

/*
 * Fills one subpass: the attachments it names and what a pipeline made
 * for it knows.  Returns 0, ENOTSUP for more colour attachments than the
 * device has or attachments of different sample counts, or EINVAL for a
 * malformed subpass.
 */
static int
pass_subpass(
	struct ghci_vk_session *session,
	const VkRenderPassCreateInfo *info,
	uint32_t index,
	struct ghci_vk_render_pass *pass)
{
	const VkSubpassDescription *description;
	struct ghci_vk_pass_references *references;
	struct ghci_vk_subpass *subpass;
	uint32_t attachment;
	uint32_t samples;
	uint32_t at;
	int error;

	/* A graphics subpass, with arrays as long as their counts. */
	description = &info->pSubpasses[index];
	references = &pass->references[index];
	subpass = &pass->subpasses[index];
	if (description->pipelineBindPoint != VK_PIPELINE_BIND_POINT_GRAPHICS)
		return EINVAL;
	if (description->colorAttachmentCount != 0U && (description->pColorAttachments == NULL
		|| ghci_vk_decoded_count(session, description->pColorAttachments) != description->colorAttachmentCount))
		return EINVAL;
	if (description->pResolveAttachments != NULL
		&& ghci_vk_decoded_count(session, description->pResolveAttachments) != description->colorAttachmentCount)
		return EINVAL;
	if (description->inputAttachmentCount != 0U && (description->pInputAttachments == NULL
		|| ghci_vk_decoded_count(session, description->pInputAttachments) != description->inputAttachmentCount))
		return EINVAL;
	if (description->colorAttachmentCount > GHCI_VK_COLOR_ATTACHMENTS || description->colorAttachmentCount > session->device->caps.max_mrt)
		return ENOTSUP;
	if (description->inputAttachmentCount > GHCI_GSI1_INPUT_ATTACHMENTS)
		return ENOTSUP;

	/* The colour attachments and their resolves. */
	samples = 0U;
	references->color_count = description->colorAttachmentCount;
	subpass->color_count = description->colorAttachmentCount;
	for (at = 0U; at < description->colorAttachmentCount; at++) {
		error = pass_reference(info, &description->pColorAttachments[at], index, pass, &references->color[at]);
		if (error != 0)
			return error;
		references->resolve[at] = VK_ATTACHMENT_UNUSED;
		if (description->pResolveAttachments != NULL) {
			error = pass_reference(info, &description->pResolveAttachments[at], index, pass, &references->resolve[at]);
			if (error != 0)
				return error;
		}
		attachment = references->color[at];
		if (attachment == VK_ATTACHMENT_UNUSED)
			continue;
		subpass->color_format[at] = pass->attachments[attachment].format;
		if (samples != 0U && samples != pass->attachments[attachment].samples)
			return ENOTSUP;
		samples = pass->attachments[attachment].samples;
	}

	/* The depth and stencil attachment. */
	references->depth = VK_ATTACHMENT_UNUSED;
	if (description->pDepthStencilAttachment != NULL) {
		error = pass_reference(info, description->pDepthStencilAttachment, index, pass, &references->depth);
		if (error != 0)
			return error;
	}
	if (references->depth != VK_ATTACHMENT_UNUSED) {
		subpass->depth_format = pass->attachments[references->depth].format;
		if (samples != 0U && samples != pass->attachments[references->depth].samples)
			return ENOTSUP;
		samples = pass->attachments[references->depth].samples;
	}
	subpass->samples = samples != 0U ? samples : 1U;

	/* The input attachments. */
	references->input_count = description->inputAttachmentCount;
	for (at = 0U; at < description->inputAttachmentCount; at++) {
		error = pass_reference(info, &description->pInputAttachments[at], index, pass, &references->input[at]);
		if (error != 0)
			return error;
		attachment = references->input[at];
		if (attachment == VK_ATTACHMENT_UNUSED)
			continue;
		subpass->input_format[at] = pass->attachments[attachment].format;
		subpass->input_samples[at] = pass->attachments[attachment].samples;
	}

	/* Succeeded: the subpass is whole. */
	return 0;
}

/*
 * Reads the views of a multiview pass (VkRenderPassMultiviewCreateInfo)
 * into the pass and each subpass.  The submission draws the whole pass
 * once per view, so every subpass draws the same views, at most
 * GHCI_VK_VIEWS_MAX of them, and reads no input attachment (which would
 * have to be read at the view's layer).  Returns 0, ENOTSUP for a pass
 * the submission cannot draw so, or EINVAL for a malformed record.
 */
static int
pass_views(
	const VkRenderPassCreateInfo *info,
	struct ghci_vk_render_pass *pass)
{
	const VkRenderPassMultiviewCreateInfo *multiview;
	uint32_t mask;
	uint32_t index;

	/* A pass of one view. */
	multiview = i915_vkc_render_pass_multiview(info);
	if (multiview == NULL || multiview->subpassCount == 0U)
		return 0;

	/* A mask for each subpass. */
	if (multiview->subpassCount != info->subpassCount || multiview->pViewMasks == NULL)
		return EINVAL;

	/* The same views in each, which the device draws, and no input attachment. */
	mask = multiview->pViewMasks[0];
	for (index = 0U; index < info->subpassCount; index++) {
		if (multiview->pViewMasks[index] != mask)
			return ENOTSUP;
		if (mask != 0U && pass->references[index].input_count != 0U)
			return ENOTSUP;
	}
	if ((mask >> GHCI_VK_VIEWS_MAX) != 0U)
		return ENOTSUP;

	/* Succeeded: the pass and its subpasses draw the views. */
	pass->view_mask = mask;
	for (index = 0U; index < info->subpassCount; index++)
		pass->subpasses[index].view_mask = mask;
	return 0;
}

/*
 * Reads one attachment reference of a subpass: an attachment of the
 * pass, or VK_ATTACHMENT_UNUSED; the attachment's first and last
 * subpass take this one.  Returns 0, or EINVAL for an attachment the
 * pass does not have.
 */
static int
pass_reference(
	const VkRenderPassCreateInfo *info,
	const VkAttachmentReference *reference,
	uint32_t index,
	struct ghci_vk_render_pass *pass,
	uint32_t *attachment)
{
	/* Unused, or an attachment of the pass. */
	*attachment = reference->attachment;
	if (reference->attachment == VK_ATTACHMENT_UNUSED)
		return 0;
	if (reference->attachment >= info->attachmentCount)
		return EINVAL;

	/* The subpasses that use it. */
	if (pass->attachments[reference->attachment].first == UINT32_MAX)
		pass->attachments[reference->attachment].first = index;
	pass->attachments[reference->attachment].last = index;

	/* Succeeded: the reference is read. */
	return 0;
}

/*
 * vkCreateFramebuffer: the create form with VkFramebufferCreateInfo.  The
 * views are kept by identity and found again by the submission; a size
 * past the hardware's (16384, or a layer count past 2048) is answered
 * VK_ERROR_OUT_OF_HOST_MEMORY.
 */
static int
create_framebuffer(
	struct ghci_vk_session *session,
	uint32_t requested,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	VkFramebufferCreateInfo info;
	struct ghci_vk_object *device;
	struct ghci_vk_object *object;
	struct ghci_vk_object *view;
	uint64_t *views;
	uint64_t handle;
	uint64_t present;
	uint64_t identity;
	uint64_t charged;
	uint32_t index;
	int error;

	/* The device, the create info and the tail. */
	kern_memset(&info, 0, sizeof(info));
	handle = drv_i915_wire_read_u64(reader);
	present = drv_i915_wire_read_u64(reader);
	if (present == 1U)
		i915_vkc_dec_VkFramebufferCreateInfo(reader, &session->arena, &info);
	identity = ghci_vk_create_tail(reader);
	if (reader->error != 0 || present != 1U)
		return EINVAL;
	device = ghci_vk_object_find(session, GHCI_VK_DEVICE, handle);
	if (device == NULL)
		return EINVAL;
	if (info.attachmentCount != 0U && (info.pAttachments == NULL || ghci_vk_decoded_count(session, info.pAttachments) != info.attachmentCount))
		return EINVAL;

	/* A size the hardware draws. */
	if (info.attachmentCount > PASS_ATTACHMENTS_MAX || info.width == 0U || info.height == 0U || info.layers == 0U
		|| info.width > 16384U || info.height > 16384U || info.layers > 2048U) {
		ghci_vk_reply_created(reply, requested, (uint32_t)VK_ERROR_OUT_OF_HOST_MEMORY, identity);
		return 0;
	}

	/* The views, each an image view of the session at least as large as the framebuffer. */
	for (index = 0U; index < info.attachmentCount; index++) {
		view = ghci_vk_object_find(session, GHCI_VK_IMAGE_VIEW, (uint64_t)(uintptr_t)info.pAttachments[index]);
		if (view == NULL)
			return EINVAL;
		if (view->as.image_view.width < info.width || view->as.image_view.height < info.height || view->as.image_view.layers < info.layers)
			return EINVAL;
	}
	charged = (uint64_t)info.attachmentCount * sizeof(uint64_t);
	if (ghci_vk_budget_take(session, charged) != 0) {
		ghci_vk_reply_created(reply, requested, (uint32_t)VK_ERROR_OUT_OF_HOST_MEMORY, identity);
		return 0;
	}
	views = NULL;
	if (info.attachmentCount != 0U) {
		views = kern_calloc(info.attachmentCount, sizeof(*views));
		if (views == NULL) {
			ghci_vk_budget_give(session, charged);
			ghci_vk_reply_created(reply, requested, (uint32_t)VK_ERROR_OUT_OF_HOST_MEMORY, identity);
			return 0;
		}
		for (index = 0U; index < info.attachmentCount; index++)
			views[index] = (uint64_t)(uintptr_t)info.pAttachments[index];
	}

	/* The object. */
	error = ghci_vk_object_add(session, GHCI_VK_FRAMEBUFFER, identity, device->device, &object);
	if (error != 0) {
		kern_free(views);
		ghci_vk_budget_give(session, charged);
		return error;
	}
	object->as.framebuffer.attachment_count = info.attachmentCount;
	object->as.framebuffer.views = views;
	object->as.framebuffer.width = info.width;
	object->as.framebuffer.height = info.height;
	object->as.framebuffer.layers = info.layers;
	object->charged = charged;
	ghci_vk_reply_created(reply, requested, (uint32_t)VK_SUCCESS, identity);

	/* Succeeded: the framebuffer exists. */
	return 0;
}

/* vkDestroyRenderPass and vkDestroyFramebuffer: the destroy form, no reply body. */
static int
destroy_pass_object(
	struct ghci_vk_session *session,
	enum ghci_vk_kind kind,
	struct i915_wire_reader *reader)
{
	struct ghci_vk_object *object;
	uint64_t identity;
	uint64_t device;

	/* The destroy form. */
	identity = ghci_vk_destroy_form(reader, &device);
	if (reader->error != 0)
		return EINVAL;

	/* The object, when the session has it. */
	object = ghci_vk_object_find(session, kind, identity);
	if (object != NULL)
		ghci_vk_object_destroy(session, object);

	/* Succeeded: the object is gone. */
	return 0;
}

/* vkGetRenderAreaGranularity: [device][render pass][u64 1] -> [u64 1][VkExtent2D]. */
static int
granularity(
	struct ghci_vk_session *session,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply,
	uint32_t requested)
{
	uint64_t identity;
	uint64_t present;

	/* The device, the render pass and the extent's presence. */
	(void)drv_i915_wire_read_u64(reader);
	identity = drv_i915_wire_read_u64(reader);
	present = drv_i915_wire_read_u64(reader);
	if (reader->error != 0 || present != 1U)
		return EINVAL;
	if (ghci_vk_object_find(session, GHCI_VK_RENDER_PASS, identity) == NULL)
		return EINVAL;

	/* Any area is drawn as it is. */
	if (requested != 0U) {
		drv_i915_wire_reply_u64(reply, 1U);
		drv_i915_wire_reply_u32(reply, PASS_GRANULARITY);
		drv_i915_wire_reply_u32(reply, PASS_GRANULARITY);
	}

	/* Succeeded: the granularity is reported. */
	return 0;
}

/* Frees a render pass's record and arrays (NULL is nothing). */
static void
pass_free(
	struct ghci_vk_render_pass *pass)
{
	/* The arrays, then the record. */
	if (pass == NULL)
		return;
	kern_free(pass->subpasses);
	kern_free(pass->references);
	kern_free(pass->attachments);
	kern_free(pass);
}
