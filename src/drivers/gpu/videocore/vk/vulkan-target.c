/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* Exact single-subpass render targets keep attachment semantics and native storage ownership independent of command arenas. */
#include <kern/kcrt.h>
#include <kern/kmem.h>
#include <uapi/errno.h>

#include "drivers/gpu/videocore/vk/vulkan-target.h"
#include "drivers/gpu/videocore/vk/vulkan-resource.h"

/* Conservative whole-job synchronization may cover these graphics/transfer/host stages without advertising compute or optional shader stages. */
#define VULKAN_PASS_STAGES (VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT | VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_VERTEX_INPUT_BIT | VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT | VK_PIPELINE_STAGE_HOST_BIT | VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT | VK_PIPELINE_STAGE_ALL_COMMANDS_BIT)
#define VULKAN_PASS_ACCESS (VK_ACCESS_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_INDEX_READ_BIT | VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_UNIFORM_READ_BIT | VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_HOST_READ_BIT | VK_ACCESS_HOST_WRITE_BIT | VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT)

/* One complete decoded creation record contains no application pointer or native owner before validation. */
union target_record {
	struct bcm2711_vulkan_pass pass;
	struct bcm2711_vulkan_framebuffer framebuffer;
};

static int create_target(struct bcm2711_vulkan_session *session, enum i915_vk_object_kind kind, struct i915_wire_reader *reader, struct i915_wire_writer *reply, bool version2);
static int destroy_target(struct bcm2711_vulkan_session *session, enum i915_vk_object_kind kind, struct i915_wire_reader *reader);
static int decode_pass(struct bcm2711_vulkan_session *session, struct i915_wire_reader *reader, struct bcm2711_vulkan_pass *pass);
static int decode_pass2(struct bcm2711_vulkan_session *session, struct i915_wire_reader *reader, struct bcm2711_vulkan_pass *pass);
static int pass2_subpass(const VkRenderPassCreateInfo *info, const struct i915_vkc_depth_resolve *resolve, struct bcm2711_vulkan_pass *pass);
static int pass_dependency(const VkSubpassDependency *dependency);
static int decode_framebuffer(struct bcm2711_vulkan_session *session, struct i915_wire_reader *reader, struct bcm2711_vulkan_framebuffer *framebuffer);
static int build_framebuffer(struct bcm2711_vulkan_framebuffer *framebuffer, struct bcm2711_vulkan_object *device);
static int release_target(struct bcm2711_vulkan_session *session, void *payload);
static int colour_layout(VkImageLayout layout);
static int colour_attachment(const VkAttachmentDescription *attachment);
static int decode_attachment(struct i915_wire_reader *reader, VkAttachmentDescription *attachment);
static int decode_subpass(struct i915_wire_reader *reader, const VkAttachmentDescription *attachments, struct bcm2711_vulkan_pass *pass);
static int colour_resolve(const VkAttachmentDescription *attachments, uint32_t slot, uint32_t attachment, VkImageLayout layout, uint32_t *named, struct bcm2711_vulkan_pass *pass);
static int pass_samples(const VkAttachmentDescription *attachments, struct bcm2711_vulkan_pass *pass);
static int build_colour(struct bcm2711_vulkan_framebuffer *framebuffer, struct bcm2711_vulkan_object *device, uint32_t slot);
static int release_views(struct bcm2711_vulkan_framebuffer *framebuffer, uint32_t count);
static int depth_attachment(VkAttachmentDescription *attachment);
static int depth_layout(VkImageLayout layout);
static int depth_edge_layout(VkImageLayout layout);
static int build_depth(struct bcm2711_vulkan_framebuffer *framebuffer, struct bcm2711_vulkan_object *device);
static int build_depth_resolve(struct bcm2711_vulkan_framebuffer *framebuffer, struct bcm2711_vulkan_object *device);
static bool view_layers(const struct bcm2711_vulkan_pass *pass, const struct bcm2711_vulkan_image_view *view);
static int granularity(struct bcm2711_vulkan_session *session, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static void target_reply(struct i915_wire_writer *reply, VkResult status, uint64_t identity);

/*
 * Routes single-colour render pass and framebuffer lifecycles while retaining exact dependent native owners.
 */
int
bcm2711_vulkan_target_dispatch(
	struct bcm2711_vulkan_session *session,
	uint32_t opcode,
	uint32_t requested,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply,
	int *handled)
{
	enum i915_vk_object_kind kind;
	bool version2;
	bool create;
	int error;

	/* Target discovery and lifecycle remain separate from subsequent command-buffer/native draw execution. */
	*handled = 1;
	create = false;
	version2 = false;
	switch (opcode) {
	case GPU_OP_GET_RENDER_AREA_GRANULARITY:
		if (requested != 1)
			return EINVAL;
		error = granularity(session, reader, reply);
		if (error != 0)
			return error;
		return 0;
	case GPU_OP_CREATE_RENDER_PASS:
		create = true;
		kind = I915_VK_OBJ_RENDER_PASS;
		break;
	case GPU_OP_CREATE_RENDER_PASS2:
		create = true;
		version2 = true;
		kind = I915_VK_OBJ_RENDER_PASS;
		break;
	case GPU_OP_DESTROY_RENDER_PASS:
		kind = I915_VK_OBJ_RENDER_PASS;
		break;
	case GPU_OP_CREATE_FRAMEBUFFER:
		create = true;
		kind = I915_VK_OBJ_FRAMEBUFFER;
		break;
	case GPU_OP_DESTROY_FRAMEBUFFER:
		kind = I915_VK_OBJ_FRAMEBUFFER;
		break;
	default:
		*handled = 0;
		return 0;
	}

	/* Creation needs an explicit Vulkan result; ordinary void destruction may still request its opcode echo. */
	if (requested > 1)
		return EINVAL;
	if (create && requested != 1)
		return EINVAL;
	if (create)
		error = create_target(session, kind, reader, reply, version2);
	else
		error = destroy_target(session, kind, reader);
	if (error != 0)
		return error;

	/* Succeeded: the selected target operation has a complete independently owned outcome. */
	return 0;
}

/*
 * Checks compatibility of the implemented single-subpass passes independently of load/store and layouts.
 *
 * Both have the same colour slots, each used or not alike and of the same
 * format, the same depth/stencil attachment, or neither has one, and the
 * same views.
 */
int
bcm2711_vulkan_pass_compatible(
	const struct bcm2711_vulkan_pass *first,
	const struct bcm2711_vulkan_pass *second)
{
	uint32_t slot;

	/* All admitted passes have one subpass; both have the same samples. */
	if (first == NULL || second == NULL)
		return EINVAL;
	if (first->colour_count != second->colour_count || first->samples != second->samples || first->view_mask != second->view_mask)
		return EINVAL;

	/* Each colour slot alike. */
	for (slot = 0; slot < first->colour_count; slot++) {
		if (first->colour_used[slot] != second->colour_used[slot])
			return EINVAL;
		if (first->colour_used[slot] && first->colours[slot].format != second->colours[slot].format)
			return EINVAL;
		if (first->colour_resolve[slot] != second->colour_resolve[slot])
			return EINVAL;
		if (first->colour_resolve[slot] && first->colour_resolved[slot].format != second->colour_resolved[slot].format)
			return EINVAL;
	}

	/* Both have the same depth/stencil attachment, or neither has one. */
	if (first->depth != second->depth)
		return EINVAL;
	if (first->depth && first->depth_stencil.format != second->depth_stencil.format)
		return EINVAL;

	/* Succeeded: distinct clear/load passes can use the same framebuffer and compatible compiled pipeline. */
	return 0;
}

/* Publishes a completely decoded target after acquiring every immutable device/pass/view ownership edge. */
static int
create_target(
	struct bcm2711_vulkan_session *session,
	enum i915_vk_object_kind kind,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply,
	bool version2)
{
	union target_record record;
	struct bcm2711_vulkan_input_owner *payload;
	struct bcm2711_vulkan_object *device;
	struct bcm2711_vulkan_object *object;
	struct bcm2711_vulkan_framebuffer *framebuffer;
	uint64_t device_id;
	uint64_t present;
	uint64_t allocator;
	uint64_t identity;
	size_t bytes;
	int error;
	int retired;

	/* Consume the complete record into temporary native fields before allocator failure or publication can interrupt decoding. */
	device_id = drv_i915_wire_read_u64(reader);
	present = drv_i915_wire_read_u64(reader);
	if (reader->error != 0 || present != 1)
		return EINVAL;
	kern_memset(&record, 0, sizeof(record));
	if (kind == I915_VK_OBJ_RENDER_PASS && version2)
		error = decode_pass2(session, reader, &record.pass);
	else if (kind == I915_VK_OBJ_RENDER_PASS)
		error = decode_pass(session, reader, &record.pass);
	else
		error = decode_framebuffer(session, reader, &record.framebuffer);
	if (error != 0)
		return error;
	allocator = drv_i915_wire_read_u64(reader);
	present = drv_i915_wire_read_u64(reader);
	identity = drv_i915_wire_read_u64(reader);
	if (reader->error != 0 ||
	    allocator != 0 ||
	    present != 1 ||
	    identity == 0)
		return EINVAL;
	device = bcm2711_vulkan_object_find(session, I915_VK_OBJ_DEVICE, device_id);
	object = bcm2711_vulkan_object_find(session, kind, identity);
	if (device == NULL)
		return EINVAL;
	if (object != NULL)
		return EEXIST;

	/* Allocate the exact ordinary payload; borrowed decoded pointers acquire ownership only during construction below. */
	bytes = sizeof(record.pass);
	if (kind == I915_VK_OBJ_FRAMEBUFFER)
		bytes = sizeof(record.framebuffer);
	payload = kern_calloc(1, bytes);
	if (payload == NULL) {
		target_reply(reply, VK_ERROR_OUT_OF_HOST_MEMORY, 0);
		return 0;
	}

	/* Construct each payload without copying borrowed owner fields into a destructor-visible ownership graph. */
	if (kind == I915_VK_OBJ_RENDER_PASS) {
		kern_memcpy(payload, &record.pass, bytes);
		error = 0;
	} else {
		framebuffer = (struct bcm2711_vulkan_framebuffer *)payload;
		framebuffer->width = record.framebuffer.width;
		framebuffer->height = record.framebuffer.height;
		error = build_framebuffer(&record.framebuffer, device);
		if (error == 0) {
			framebuffer->owner.parent = record.framebuffer.owner.parent;
			kern_memcpy(framebuffer->colours, record.framebuffer.colours, sizeof(framebuffer->colours));
			kern_memcpy(framebuffer->resolves, record.framebuffer.resolves, sizeof(framebuffer->resolves));
			framebuffer->depth = record.framebuffer.depth;
			framebuffer->depth_resolve = record.framebuffer.depth_resolve;
		}
	}

	/* Device acquisition follows successful target dependency construction; unwind only the acquired edges on failure. */
	if (error == 0)
		error = bcm2711_vulkan_object_retain(device);
	if (error != 0) {
		retired = release_target(session, payload);
		if (retired != 0)
			return retired;
		return error;
	}

	/* Publish only a complete immutable graph and distinguish ordinary registry exhaustion from malformed identity. */
	payload->device = device;
	error = bcm2711_vulkan_object_publish(session, kind, identity, payload, release_target, &object);
	if (error != 0) {
		retired = release_target(session, payload);
		if (retired != 0)
			return retired;
		if (error == ENOMEM || error == ENOSPC) {
			target_reply(reply, VK_ERROR_OUT_OF_HOST_MEMORY, 0);
			return 0;
		}

		/* Exact typed identity errors retain their transport refusal. */
		return error;
	}

	/* Acknowledge only the precise registry identity that now owns the complete target. */
	target_reply(reply, VK_SUCCESS, identity);

	/* Succeeded: temporary command fields are no longer part of the target's lifetime. */
	return 0;
}

/*
 * Decodes the exact single subpass, its colour and depth/stencil attachments and the finite external dependency contract from the actual client record.
 *
 * A pass without attachments (7.1 only) renders the framebuffer's area
 * with the samples of the pipelines it draws with; its samples are zero.
 */
static int
decode_pass(
	struct bcm2711_vulkan_session *session,
	struct i915_wire_reader *reader,
	struct bcm2711_vulkan_pass *pass)
{
	VkAttachmentDescription attachments[BCM2711_VULKAN_PASS_ATTACHMENTS];
	VkSubpassDependency *dependency;
	VkRenderPassCreateInfo chained;
	const VkRenderPassMultiviewCreateInfo *multiview;
	uint64_t array;
	uint32_t structure;
	uint32_t flags;
	uint32_t count;
	uint32_t index;
	int error;

	/*
	 * At most four colour attachments and a depth/stencil one, each read
	 * before the subpass that names them; none needs a 7.1.  The chain is
	 * the multiview record libvulkan forwards, or nothing.
	 */
	kern_memset(&chained, 0, sizeof(chained));
	structure = drv_i915_wire_read_u32(reader);
	i915_vkc_read_render_pass_chain(reader, &session->arena, &chained.pNext);
	flags = drv_i915_wire_read_u32(reader);
	count = drv_i915_wire_read_u32(reader);
	array = drv_i915_wire_read_u64(reader);
	if (reader->error != 0 ||
	    structure != VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO ||
	    flags != 0 ||
	    count > BCM2711_VULKAN_PASS_ATTACHMENTS ||
	    array != count)
		return ENOTSUP;
	if (count == 0 && !bcm2711_vulkan_engine_71(session))
		return ENOTSUP;
	pass->attachment_count = count;
	for (index = 0; index < count; index++) {
		error = decode_attachment(reader, &attachments[index]);
		if (error != 0)
			return error;
	}

	/* The one subpass, which names every attachment. */
	count = drv_i915_wire_read_u32(reader);
	array = drv_i915_wire_read_u64(reader);
	if (reader->error != 0 ||
	    count != 1 ||
	    array != 1)
		return ENOTSUP;
	error = decode_subpass(reader, attachments, pass);
	if (error != 0)
		return error;

	/* The pass's samples; a pass without attachments takes its pipelines'.  Only a 7.1 resolves colour. */
	error = pass_samples(attachments, pass);
	if (error != 0)
		return error;
	for (index = 0; index < pass->colour_count; index++) {
		if (pass->colour_resolve[index] && !bcm2711_vulkan_engine_71(session))
			return ENOTSUP;
	}

	/* The one subpass's views, at most the device's (maxMultiviewViewCount); none renders one view. */
	multiview = i915_vkc_render_pass_multiview(&chained);
	pass->view_mask = 0;
	if (multiview != NULL && multiview->subpassCount != 0) {
		if (multiview->subpassCount != 1 || multiview->pViewMasks == NULL)
			return ENOTSUP;
		if ((multiview->pViewMasks[0] >> BCM2711_VULKAN_VIEWS) != 0)
			return ENOTSUP;
		pass->view_mask = multiview->pViewMasks[0];
	}

	/* Each external dependency is retained for conservative whole-job ordering and cache visibility during native execution. */
	count = drv_i915_wire_read_u32(reader);
	array = drv_i915_wire_read_u64(reader);
	if (reader->error != 0 ||
	    count > BCM2711_VULKAN_PASS_DEPENDENCIES ||
	    array != count)
		return ENOTSUP;
	pass->count = count;
	for (index = 0; index < count; index++) {
		dependency = &pass->dependencies[index];
		dependency->srcSubpass = drv_i915_wire_read_u32(reader);
		dependency->dstSubpass = drv_i915_wire_read_u32(reader);
		dependency->srcStageMask = drv_i915_wire_read_u32(reader);
		dependency->dstStageMask = drv_i915_wire_read_u32(reader);
		dependency->srcAccessMask = drv_i915_wire_read_u32(reader);
		dependency->dstAccessMask = drv_i915_wire_read_u32(reader);
		dependency->dependencyFlags = drv_i915_wire_read_u32(reader);
		if (reader->error != 0)
			return EINVAL;
		error = pass_dependency(dependency);
		if (error != 0)
			return error;
	}

	/* Succeeded: the pass retains every supported attachment and dependency field without a command-arena pointer. */
	return 0;
}

/*
 * Decodes a render pass of vkCreateRenderPass2 (GPU_OP_CREATE_RENDER_PASS2),
 * which libvulkan sends only for a subpass that resolves its depth and
 * stencil: the shape decode_pass() takes, and the resolve of its
 * multisampled depth/stencil attachment's sample 0 into one more
 * attachment of one sample (7.1 only).
 */
static int
decode_pass2(
	struct bcm2711_vulkan_session *session,
	struct i915_wire_reader *reader,
	struct bcm2711_vulkan_pass *pass)
{
	const VkSubpassDescriptionDepthStencilResolve *resolves;
	const VkRenderPassMultiviewCreateInfo *multiview;
	struct i915_vkc_depth_resolve resolve;
	VkRenderPassCreateInfo info;
	uint32_t index;
	int error;

	/* The record in its Vulkan 1.0 form, with the resolve aside. */
	i915_vkc_read_render_pass2(reader, &session->arena, &info, &resolves);
	if (reader->error != 0)
		return EINVAL;

	/* One subpass, at most the attachments a pass has, on a 7.1. */
	if (info.flags != 0 ||
	    info.subpassCount != 1 ||
	    info.attachmentCount > BCM2711_VULKAN_PASS_ATTACHMENTS ||
	    info.attachmentCount == 0 ||
	    !bcm2711_vulkan_engine_71(session))
		return ENOTSUP;
	error = i915_vkc_depth_resolve(&info, resolves, 0, &resolve);
	if (error != 0)
		return error;

	/* The one subpass, which names every attachment. */
	pass->attachment_count = info.attachmentCount;
	error = pass2_subpass(&info, &resolve, pass);
	if (error != 0)
		return error;

	/* Every attachment but the resolves' has four samples. */
	error = pass_samples(info.pAttachments, pass);
	if (error != 0)
		return error;
	if (pass->samples != VK_SAMPLE_COUNT_4_BIT || info.pAttachments[resolve.target].samples != VK_SAMPLE_COUNT_1_BIT)
		return ENOTSUP;

	/* The one subpass's views, at most the device's (maxMultiviewViewCount); none renders one view. */
	multiview = i915_vkc_render_pass_multiview(&info);
	pass->view_mask = 0;
	if (multiview != NULL && multiview->subpassCount != 0) {
		if (multiview->subpassCount != 1 || multiview->pViewMasks == NULL)
			return ENOTSUP;
		if ((multiview->pViewMasks[0] >> BCM2711_VULKAN_VIEWS) != 0)
			return ENOTSUP;
		pass->view_mask = multiview->pViewMasks[0];
	}

	/* The external dependencies, as decode_pass() takes them. */
	if (info.dependencyCount > BCM2711_VULKAN_PASS_DEPENDENCIES)
		return ENOTSUP;
	pass->count = info.dependencyCount;
	for (index = 0; index < info.dependencyCount; index++) {
		pass->dependencies[index] = info.pDependencies[index];
		error = pass_dependency(&pass->dependencies[index]);
		if (error != 0)
			return error;
	}

	/* Succeeded: the pass retains its attachments, its resolve and its dependencies. */
	return 0;
}

/*
 * Takes the one subpass of a render pass of vkCreateRenderPass2 into pass:
 * what decode_subpass() takes (with its colour resolves), the depth/stencil
 * attachment resolved, and
 * resolve's target, an attachment of one sample in its format in a
 * depth/stencil layout during the pass and around it.
 */
static int
pass2_subpass(
	const VkRenderPassCreateInfo *info,
	const struct i915_vkc_depth_resolve *resolve,
	struct bcm2711_vulkan_pass *pass)
{
	const VkSubpassDescription *subpass;
	VkAttachmentDescription resolved;
	uint32_t attachment;
	uint32_t named;
	uint32_t slot;
	int error;

	/* A graphics subpass without input or preserved attachments, which resolves its depth/stencil attachment. */
	subpass = &info->pSubpasses[0];
	if (subpass->flags != 0 ||
	    subpass->pipelineBindPoint != VK_PIPELINE_BIND_POINT_GRAPHICS ||
	    subpass->inputAttachmentCount != 0 ||
	    subpass->preserveAttachmentCount != 0 ||
	    subpass->colorAttachmentCount > BCM2711_VULKAN_PASS_COLOURS ||
	    subpass->pDepthStencilAttachment == NULL ||
	    resolve->target == VK_ATTACHMENT_UNUSED)
		return ENOTSUP;

	/* The colour slots, each naming a colour attachment not named before, or unused. */
	pass->colour_count = subpass->colorAttachmentCount;
	named = 1U << resolve->target;
	for (slot = 0; slot < subpass->colorAttachmentCount; slot++) {
		attachment = subpass->pColorAttachments[slot].attachment;
		if (attachment == VK_ATTACHMENT_UNUSED)
			continue;
		if (attachment >= pass->attachment_count || (named & (1U << attachment)) != 0)
			return ENOTSUP;
		if (subpass->pColorAttachments[slot].layout != VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL)
			return ENOTSUP;
		error = colour_attachment(&info->pAttachments[attachment]);
		if (error != 0)
			return error;
		named |= 1U << attachment;
		pass->colour_used[slot] = true;
		pass->colours[slot] = info->pAttachments[attachment];
		pass->colour_attachment[slot] = attachment;
	}

	/* Each colour slot's resolve attachment, when the subpass has them. */
	for (slot = 0; subpass->pResolveAttachments != NULL && slot < subpass->colorAttachmentCount; slot++) {
		error = colour_resolve(info->pAttachments, slot, subpass->pResolveAttachments[slot].attachment, subpass->pResolveAttachments[slot].layout, &named, pass);
		if (error != 0)
			return error;
	}

	/* The depth/stencil attachment, which is the resolve's source. */
	attachment = subpass->pDepthStencilAttachment->attachment;
	if (attachment != resolve->source || (named & (1U << attachment)) != 0)
		return ENOTSUP;
	error = depth_layout(subpass->pDepthStencilAttachment->layout);
	if (error != 0)
		return error;
	pass->depth = true;
	pass->depth_stencil = info->pAttachments[attachment];
	pass->depth_attachment = attachment;
	error = depth_attachment(&pass->depth_stencil);
	if (error != 0)
		return error;
	named |= 1U << attachment;

	/* The resolve's target: written whole, from a layout before the pass that may be undefined, to one of the pass's or a transfer's. */
	resolved = info->pAttachments[resolve->target];
	if (resolved.flags != 0 || !bcm2711_vulkan_depth_format(resolved.format))
		return ENOTSUP;
	if (resolved.initialLayout != VK_IMAGE_LAYOUT_UNDEFINED) {
		error = depth_edge_layout(resolved.initialLayout);
		if (error != 0)
			return error;
	}
	error = depth_edge_layout(resolved.finalLayout);
	if (error != 0)
		return error;
	pass->depth_resolve = true;
	pass->depth_resolved = resolved;
	pass->depth_resolve_attachment = resolve->target;

	/* Every attachment is one the subpass names. */
	if (named != (1U << pass->attachment_count) - 1U)
		return ENOTSUP;

	/* Succeeded: the subpass's slots, depth/stencil attachment and resolve are in the pass. */
	return 0;
}

/*
 * Checks one external dependency of a pass's single subpass: from outside
 * into it or from it to outside, with the stages and accesses the
 * conservative whole-job ordering covers (BY_REGION as a stronger
 * whole-target dependency).
 */
static int
pass_dependency(
	const VkSubpassDependency *dependency)
{
	/* Into the subpass or out of it. */
	if (dependency->srcSubpass == VK_SUBPASS_EXTERNAL) {
		if (dependency->dstSubpass != 0)
			return ENOTSUP;
	} else {
		if (dependency->srcSubpass != 0 || dependency->dstSubpass != VK_SUBPASS_EXTERNAL)
			return ENOTSUP;
	}

	/* Preserve only supported stage/access semantics, including BY_REGION as a conservatively stronger whole-target dependency. */
	if (dependency->srcStageMask == 0 || dependency->dstStageMask == 0)
		return EINVAL;
	if ((dependency->srcStageMask & ~VULKAN_PASS_STAGES) != 0 || (dependency->dstStageMask & ~VULKAN_PASS_STAGES) != 0)
		return ENOTSUP;
	if ((dependency->srcAccessMask & ~VULKAN_PASS_ACCESS) != 0 || (dependency->dstAccessMask & ~VULKAN_PASS_ACCESS) != 0)
		return ENOTSUP;
	if ((dependency->dependencyFlags & ~VK_DEPENDENCY_BY_REGION_BIT) != 0)
		return ENOTSUP;

	/* Succeeded: the dependency is one the native pass keeps. */
	return 0;
}

/*
 * Decodes the pass's one subpass: its input attachments, which the
 * shader reads as images (they take no slot), its colour slots (up to
 * four, each an attachment or unused), the resolve attachments of the
 * slots when it has them, and its depth/stencil attachment, with no
 * preserved attachment.  Every attachment is named: once as a colour, a
 * resolve or the depth/stencil one (a colour or a resolve one in the
 * colour attachment layout), or as an input one.
 */
static int
decode_subpass(
	struct i915_wire_reader *reader,
	const VkAttachmentDescription *attachments,
	struct bcm2711_vulkan_pass *pass)
{
	uint64_t array;
	uint32_t flags;
	uint32_t count;
	uint32_t attachment;
	uint32_t layout;
	uint32_t slot;
	uint32_t named;
	uint32_t inputs;
	int error;

	/* A graphics subpass. */
	flags = drv_i915_wire_read_u32(reader);
	attachment = drv_i915_wire_read_u32(reader);
	count = drv_i915_wire_read_u32(reader);
	array = drv_i915_wire_read_u64(reader);
	if (reader->error != 0 ||
	    flags != 0 ||
	    attachment != VK_PIPELINE_BIND_POINT_GRAPHICS ||
	    array != count)
		return ENOTSUP;

	/* Its input attachments: attachments of the pass (or unused), which take no slot. */
	inputs = 0;
	for (slot = 0; slot < count; slot++) {
		attachment = drv_i915_wire_read_u32(reader);
		(void)drv_i915_wire_read_u32(reader);
		if (reader->error != 0)
			return EINVAL;
		if (attachment == VK_ATTACHMENT_UNUSED)
			continue;
		if (attachment >= pass->attachment_count)
			return ENOTSUP;
		inputs |= 1U << attachment;
	}

	/* The colour slots, each naming a colour attachment not named before, or unused. */
	count = drv_i915_wire_read_u32(reader);
	array = drv_i915_wire_read_u64(reader);
	if (reader->error != 0 ||
	    count > BCM2711_VULKAN_PASS_COLOURS ||
	    array != count)
		return ENOTSUP;
	pass->colour_count = count;
	named = 0;
	for (slot = 0; slot < count; slot++) {
		attachment = drv_i915_wire_read_u32(reader);
		layout = drv_i915_wire_read_u32(reader);
		if (reader->error != 0)
			return EINVAL;

		/* An unused slot draws nothing. */
		if (attachment == VK_ATTACHMENT_UNUSED)
			continue;

		/* A colour attachment of the pass, in its attachment layout during the subpass. */
		if (attachment >= pass->attachment_count || (named & (1U << attachment)) != 0)
			return ENOTSUP;
		if (layout != VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL)
			return ENOTSUP;
		error = colour_attachment(&attachments[attachment]);
		if (error != 0)
			return error;
		named |= 1U << attachment;
		pass->colour_used[slot] = true;
		pass->colours[slot] = attachments[attachment];
		pass->colour_attachment[slot] = attachment;
	}

	/* The resolve attachments: none, or one for each colour slot (unused or an attachment). */
	array = drv_i915_wire_read_u64(reader);
	if (reader->error != 0)
		return EINVAL;
	if (array != 0 && array != count)
		return ENOTSUP;
	for (slot = 0; slot < array; slot++) {
		attachment = drv_i915_wire_read_u32(reader);
		layout = drv_i915_wire_read_u32(reader);
		if (reader->error != 0)
			return EINVAL;
		error = colour_resolve(attachments, slot, attachment, layout, &named, pass);
		if (error != 0)
			return error;
	}

	/* The depth/stencil attachment, when the subpass has one. */
	array = drv_i915_wire_read_u64(reader);
	if (reader->error != 0)
		return EINVAL;
	if (array != 0 && array != 1)
		return ENOTSUP;
	if (array == 1) {
		attachment = drv_i915_wire_read_u32(reader);
		layout = drv_i915_wire_read_u32(reader);
		if (reader->error != 0)
			return EINVAL;
		if (attachment >= pass->attachment_count || (named & (1U << attachment)) != 0)
			return ENOTSUP;
		error = depth_layout(layout);
		if (error != 0)
			return error;
		pass->depth = true;
		pass->depth_stencil = attachments[attachment];
		pass->depth_attachment = attachment;
		error = depth_attachment(&pass->depth_stencil);
		if (error != 0)
			return error;
		named |= 1U << attachment;
	}

	/* Every attachment is one the subpass names. */
	if ((named | inputs) != (1U << pass->attachment_count) - 1U)
		return ENOTSUP;

	/* No attachment is preserved through the single subpass. */
	count = drv_i915_wire_read_u32(reader);
	array = drv_i915_wire_read_u64(reader);
	if (reader->error != 0 ||
	    count != 0 ||
	    array != 0)
		return ENOTSUP;

	/* Succeeded: the subpass's slots and depth/stencil attachment are in the pass. */
	return 0;
}

/*
 * Takes the resolve attachment of one colour slot (an entry of
 * pResolveAttachments): unused, or an attachment of one sample in the
 * format of the slot's own, which the slot uses, not named before, in the
 * colour attachment layout during the subpass.  pass_samples() checks
 * that the slot's attachment has four samples.
 */
static int
colour_resolve(
	const VkAttachmentDescription *attachments,
	uint32_t slot,
	uint32_t attachment,
	VkImageLayout layout,
	uint32_t *named,
	struct bcm2711_vulkan_pass *pass)
{
	int error;

	/* An unused entry resolves nothing. */
	if (attachment == VK_ATTACHMENT_UNUSED)
		return 0;

	/* The slot draws into an attachment, which this one resolves. */
	if (slot >= pass->colour_count || !pass->colour_used[slot])
		return EINVAL;
	if (attachment >= pass->attachment_count || (*named & (1U << attachment)) != 0)
		return ENOTSUP;
	if (layout != VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL)
		return ENOTSUP;
	error = colour_attachment(&attachments[attachment]);
	if (error != 0)
		return error;
	if (attachments[attachment].samples != VK_SAMPLE_COUNT_1_BIT || attachments[attachment].format != pass->colours[slot].format)
		return EINVAL;

	/* Succeeded: the slot resolves into the attachment. */
	*named |= 1U << attachment;
	pass->colour_resolve[slot] = true;
	pass->colour_resolved[slot] = attachments[attachment];
	pass->colour_resolve_attachment[slot] = attachment;
	return 0;
}

/*
 * Finds a pass's samples a pixel from its attachments: every attachment
 * but a resolve's has the same, one or four, and a pass that resolves has
 * four; a pass without attachments has zero (it takes its pipelines').
 */
static int
pass_samples(
	const VkAttachmentDescription *attachments,
	struct bcm2711_vulkan_pass *pass)
{
	uint32_t resolves;
	uint32_t index;

	/* The attachments that are resolves'. */
	resolves = 0;
	for (index = 0; index < pass->colour_count; index++) {
		if (pass->colour_resolve[index])
			resolves |= 1U << pass->colour_resolve_attachment[index];
	}
	if (pass->depth_resolve)
		resolves |= 1U << pass->depth_resolve_attachment;

	/* The others' samples, all the same. */
	pass->samples = 0;
	for (index = 0; index < pass->attachment_count; index++) {
		if ((resolves & (1U << index)) != 0)
			continue;
		if (pass->samples == 0)
			pass->samples = attachments[index].samples;
		if (attachments[index].samples != pass->samples)
			return ENOTSUP;
	}
	if (pass->samples != 0 && pass->samples != VK_SAMPLE_COUNT_1_BIT && pass->samples != VK_SAMPLE_COUNT_4_BIT)
		return ENOTSUP;

	/* Only a pass of four samples resolves. */
	if (resolves != 0 && pass->samples != VK_SAMPLE_COUNT_4_BIT)
		return EINVAL;

	/* Succeeded: the pass's samples are known. */
	return 0;
}

/*
 * Checks a colour attachment against what the tile buffer does: one
 * sample of a format it holds, loaded, cleared or discarded and stored or
 * discarded, from and to layouts the pass tracks.
 */
static int
colour_attachment(
	const VkAttachmentDescription *attachment)
{
	struct bcm2711_vulkan_colour_target target;
	int error;

	/* One sample or four of a colour attachment format. */
	if (attachment->flags != 0 || (attachment->samples != VK_SAMPLE_COUNT_1_BIT && attachment->samples != VK_SAMPLE_COUNT_4_BIT))
		return ENOTSUP;
	error = bcm2711_vulkan_colour_target(attachment->format, &target);
	if (error != 0)
		return error;

	/* Clear/load/discard remain distinct lowering obligations; a colour attachment has no stencil. */
	if (attachment->loadOp != VK_ATTACHMENT_LOAD_OP_LOAD &&
	    attachment->loadOp != VK_ATTACHMENT_LOAD_OP_CLEAR &&
	    attachment->loadOp != VK_ATTACHMENT_LOAD_OP_DONT_CARE)
		return ENOTSUP;
	if (attachment->storeOp != VK_ATTACHMENT_STORE_OP_STORE && attachment->storeOp != VK_ATTACHMENT_STORE_OP_DONT_CARE)
		return ENOTSUP;
	if (attachment->stencilLoadOp != VK_ATTACHMENT_LOAD_OP_DONT_CARE || attachment->stencilStoreOp != VK_ATTACHMENT_STORE_OP_DONT_CARE)
		return ENOTSUP;

	/* A load reads defined contents; an undefined initial layout has none. */
	if (attachment->initialLayout == VK_IMAGE_LAYOUT_UNDEFINED) {
		if (attachment->loadOp == VK_ATTACHMENT_LOAD_OP_LOAD)
			return EINVAL;
	} else {
		error = colour_layout(attachment->initialLayout);
		if (error != 0)
			return error;
	}

	/* A pass cannot leave its stored attachment in an undefined or unsupported final layout. */
	error = colour_layout(attachment->finalLayout);
	if (error != 0)
		return error;

	/* Succeeded: the attachment's whole lifecycle maps onto tile-buffer loads, clears and stores. */
	return 0;
}

/* Decodes one framebuffer using borrowed registry owners that are retained only after complete validation. */
static int
decode_framebuffer(
	struct bcm2711_vulkan_session *session,
	struct i915_wire_reader *reader,
	struct bcm2711_vulkan_framebuffer *framebuffer)
{
	struct bcm2711_vulkan_object *views[BCM2711_VULKAN_PASS_ATTACHMENTS];
	struct bcm2711_vulkan_pass *pass;
	uint64_t chain;
	uint64_t identity;
	uint64_t array;
	uint32_t structure;
	uint32_t flags;
	uint32_t count;
	uint32_t layers;
	uint32_t index;
	uint32_t slot;

	/* Ordinary framebuffer flags and chains cannot silently enable imageless or multiview target semantics. */
	structure = drv_i915_wire_read_u32(reader);
	chain = drv_i915_wire_read_u64(reader);
	flags = drv_i915_wire_read_u32(reader);
	identity = drv_i915_wire_read_u64(reader);
	count = drv_i915_wire_read_u32(reader);
	array = drv_i915_wire_read_u64(reader);
	if (reader->error != 0 ||
	    structure != VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO ||
	    chain != 0 ||
	    flags != 0 ||
	    count > BCM2711_VULKAN_PASS_ATTACHMENTS ||
	    array != count)
		return ENOTSUP;
	framebuffer->owner.parent = bcm2711_vulkan_object_find(session, I915_VK_OBJ_RENDER_PASS, identity);

	/* One view an attachment, in the pass's attachment order. */
	for (index = 0; index < count; index++) {
		identity = drv_i915_wire_read_u64(reader);
		views[index] = bcm2711_vulkan_object_find(session, I915_VK_OBJ_IMAGE_VIEW, identity);
		if (reader->error != 0 || views[index] == NULL)
			return EINVAL;
	}
	framebuffer->width = drv_i915_wire_read_u32(reader);
	framebuffer->height = drv_i915_wire_read_u32(reader);
	layers = drv_i915_wire_read_u32(reader);
	if (reader->error != 0 || framebuffer->owner.parent == NULL)
		return EINVAL;

	/* The views match the pass's attachments one for one: each colour slot's, then the depth/stencil one's. */
	pass = framebuffer->owner.parent->payload;
	if (count != pass->attachment_count)
		return EINVAL;
	for (slot = 0; slot < pass->colour_count; slot++) {
		if (pass->colour_used[slot])
			framebuffer->colours[slot] = views[pass->colour_attachment[slot]];
		if (pass->colour_resolve[slot])
			framebuffer->resolves[slot] = views[pass->colour_resolve_attachment[slot]];
	}
	if (pass->depth)
		framebuffer->depth = views[pass->depth_attachment];
	if (pass->depth_resolve)
		framebuffer->depth_resolve = views[pass->depth_resolve_attachment];

	/* The extent is the single-layer one native passes render. */
	if (framebuffer->width == 0 ||
	    framebuffer->height == 0 ||
	    layers != 1)
		return ENOTSUP;

	/* Succeeded: complete typed owners and declared dimensions are available for construction. */
	return 0;
}

/* Verifies exact native attachment compatibility and acquires independent pass/view edges before payload publication. */
static int
build_framebuffer(
	struct bcm2711_vulkan_framebuffer *framebuffer,
	struct bcm2711_vulkan_object *device)
{
	struct bcm2711_vulkan_pass *pass;
	uint32_t slot;
	int error;
	int retired;

	/* The pass is the same device's. */
	pass = framebuffer->owner.parent->payload;
	if (pass->owner.device != device)
		return EINVAL;

	/* Retain the pass first; every later failure unwinds what was acquired after it. */
	error = bcm2711_vulkan_object_retain(framebuffer->owner.parent);
	if (error != 0)
		return error;

	/* Each used colour slot's view is checked and retained in slot order. */
	for (slot = 0; slot < pass->colour_count; slot++) {
		if (framebuffer->colours[slot] == NULL)
			continue;
		error = build_colour(framebuffer, device, slot);
		if (error != 0) {
			retired = release_views(framebuffer, slot);
			if (retired != 0)
				return retired;
			retired = bcm2711_vulkan_object_release(framebuffer->owner.parent);
			if (retired != 0)
				return retired;
			return error;
		}
	}

	/* The depth/stencil view, when the pass has one, is checked and retained last; failure unwinds the colour views and the pass. */
	if (framebuffer->depth != NULL) {
		error = build_depth(framebuffer, device);
		if (error != 0) {
			retired = release_views(framebuffer, pass->colour_count);
			if (retired != 0)
				return retired;
			retired = bcm2711_vulkan_object_release(framebuffer->owner.parent);
			if (retired != 0)
				return retired;
			return error;
		}
	}

	/* The resolve's view, when the pass resolves, last; failure unwinds the depth/stencil and colour views and the pass. */
	if (framebuffer->depth_resolve != NULL) {
		error = build_depth_resolve(framebuffer, device);
		if (error != 0) {
			retired = release_views(framebuffer, pass->colour_count);
			if (retired != 0)
				return retired;
			retired = bcm2711_vulkan_object_release(framebuffer->depth);
			if (retired != 0)
				return retired;
			retired = bcm2711_vulkan_object_release(framebuffer->owner.parent);
			if (retired != 0)
				return retired;
			return error;
		}
	}

	/* Succeeded: the caller now owns every borrowed graph edge and must publish or release them. */
	return 0;
}

/* Checks one colour slot's view, and its resolve's, against the pass and the framebuffer's extent, then retains them. */
static int
build_colour(
	struct bcm2711_vulkan_framebuffer *framebuffer,
	struct bcm2711_vulkan_object *device,
	uint32_t slot)
{
	struct bcm2711_vulkan_pass *pass;
	struct bcm2711_vulkan_image_view *view;
	struct bcm2711_vulkan_resource *image;
	int error;
	int retired;

	/* Immutable same-device parents supply the actual bound image format and extent. */
	pass = framebuffer->owner.parent->payload;
	view = framebuffer->colours[slot]->payload;
	image = view->owner.parent->payload;
	if (view->owner.device != device)
		return EINVAL;
	if (pass->colours[slot].format != view->format ||
	    view->format != image->format ||
	    image->memory == NULL ||
	    image->samples != pass->samples ||
	    (image->usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) == 0)
		return EINVAL;
	if (framebuffer->width > image->width || framebuffer->height > image->height)
		return EINVAL;
	if (!view_layers(pass, view))
		return EINVAL;

	/* The framebuffer keeps the view, and through it the image, until it retires. */
	error = bcm2711_vulkan_object_retain(framebuffer->colours[slot]);
	if (error != 0)
		return error;

	/* A resolved slot's view: a colour attachment image of one sample in the slot's format. */
	if (framebuffer->resolves[slot] == NULL)
		return 0;
	view = framebuffer->resolves[slot]->payload;
	image = view->owner.parent->payload;
	error = 0;
	if (view->owner.device != device ||
	    pass->colour_resolved[slot].format != view->format ||
	    view->format != image->format ||
	    image->memory == NULL ||
	    image->samples != VK_SAMPLE_COUNT_1_BIT ||
	    (image->usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) == 0 ||
	    framebuffer->width > image->width ||
	    framebuffer->height > image->height ||
	    !view_layers(pass, view))
		error = EINVAL;
	if (error == 0)
		error = bcm2711_vulkan_object_retain(framebuffer->resolves[slot]);

	/* A refused resolve view gives the slot's own view back. */
	if (error != 0) {
		retired = bcm2711_vulkan_object_release(framebuffer->colours[slot]);
		if (retired != 0)
			return retired;
		return error;
	}

	/* Succeeded: the caller owns the views' edges. */
	return 0;
}

/* Releases the colour views, and their resolves', of the first count slots, keeping the first native error. */
static int
release_views(
	struct bcm2711_vulkan_framebuffer *framebuffer,
	uint32_t count)
{
	uint32_t slot;
	int first;
	int retired;

	/* Every retained view retires even when an earlier one failed. */
	first = 0;
	for (slot = 0; slot < count && slot < BCM2711_VULKAN_PASS_COLOURS; slot++) {
		if (framebuffer->resolves[slot] != NULL) {
			retired = bcm2711_vulkan_object_release(framebuffer->resolves[slot]);
			if (retired != 0 && first == 0)
				first = retired;
		}
		if (framebuffer->colours[slot] == NULL)
			continue;
		retired = bcm2711_vulkan_object_release(framebuffer->colours[slot]);
		if (retired != 0 && first == 0)
			first = retired;
	}

	/* Succeeded unless a release failed. */
	return first;
}

/* Withdraws only a target's public identity while dependent command/pipeline/prepared owners keep its complete graph. */
static int
destroy_target(
	struct bcm2711_vulkan_session *session,
	enum i915_vk_object_kind kind,
	struct i915_wire_reader *reader)
{
	struct bcm2711_vulkan_object *device;
	struct bcm2711_vulkan_object *object;
	struct bcm2711_vulkan_input_owner *owner;
	uint64_t device_id;
	uint64_t identity;
	uint64_t allocator;
	int error;

	/* Ordinary client destruction identifies the exact parent device, target and null allocator. */
	device_id = drv_i915_wire_read_u64(reader);
	identity = drv_i915_wire_read_u64(reader);
	allocator = drv_i915_wire_read_u64(reader);
	if (reader->error != 0 || allocator != 0)
		return EINVAL;
	if (identity == 0)
		return 0;
	device = bcm2711_vulkan_object_find(session, I915_VK_OBJ_DEVICE, device_id);
	object = bcm2711_vulkan_object_find(session, kind, identity);
	if (device == NULL || object == NULL)
		return EINVAL;
	owner = object->payload;
	if (owner->device != device)
		return EINVAL;
	error = bcm2711_vulkan_object_remove(session, kind, identity);
	if (error != 0)
		return error;

	/* Succeeded: dependent owners still retain the exact immutable target. */
	return 0;
}

/* Retires framebuffer view and common pass/device edges, preserving every cleanup obligation and the first native error. */
static int
release_target(
	struct bcm2711_vulkan_session *session,
	void *payload)
{
	struct bcm2711_vulkan_input_owner *owner;
	struct bcm2711_vulkan_framebuffer *framebuffer;
	int error;
	int retired;

	/* A nonnull parent distinguishes framebuffer payloads; passes own only their root device. */
	(void)session;
	owner = payload;
	error = 0;
	if (owner->parent != NULL) {
		framebuffer = payload;
		error = release_views(framebuffer, BCM2711_VULKAN_PASS_COLOURS);
		if (framebuffer->depth != NULL) {
			retired = bcm2711_vulkan_object_release(framebuffer->depth);
			if (retired != 0 && error == 0)
				error = retired;
		}
		if (framebuffer->depth_resolve != NULL) {
			retired = bcm2711_vulkan_object_release(framebuffer->depth_resolve);
			if (retired != 0 && error == 0)
				error = retired;
		}
	}

	/* All dependencies and payload storage retire even if an independent native view enters VA quarantine. */
	retired = bcm2711_vulkan_object_release(owner->parent);
	if (retired != 0 && error == 0)
		error = retired;
	retired = bcm2711_vulkan_object_release(owner->device);
	if (retired != 0 && error == 0)
		error = retired;
	kern_free(payload);
	if (error != 0)
		return error;

	/* Succeeded: no target ownership remains. */
	return 0;
}

/* Reads one attachment description's fields in their wire order. */
static int
decode_attachment(
	struct i915_wire_reader *reader,
	VkAttachmentDescription *attachment)
{
	/* Every field is a 32-bit word. */
	attachment->flags = drv_i915_wire_read_u32(reader);
	attachment->format = drv_i915_wire_read_u32(reader);
	attachment->samples = drv_i915_wire_read_u32(reader);
	attachment->loadOp = drv_i915_wire_read_u32(reader);
	attachment->storeOp = drv_i915_wire_read_u32(reader);
	attachment->stencilLoadOp = drv_i915_wire_read_u32(reader);
	attachment->stencilStoreOp = drv_i915_wire_read_u32(reader);
	attachment->initialLayout = drv_i915_wire_read_u32(reader);
	attachment->finalLayout = drv_i915_wire_read_u32(reader);
	if (reader->error != 0)
		return EINVAL;

	/* Succeeded: the description is complete; its meaning is checked by its role. */
	return 0;
}

/*
 * Checks a depth/stencil attachment against what the tile buffer does.
 *
 * Each aspect may load, clear or discard, and store or discard.  Z and
 * stencil are loaded together, so one aspect loading while the other clears
 * is refused.  A depth-only format's stencil operations are ignored, as
 * Vulkan defines them, and kept as discards.
 */
static int
depth_attachment(
	VkAttachmentDescription *attachment)
{
	int error;

	/* One sample or four of a tile-buffer depth format. */
	if (attachment->flags != 0 || (attachment->samples != VK_SAMPLE_COUNT_1_BIT && attachment->samples != VK_SAMPLE_COUNT_4_BIT))
		return ENOTSUP;
	if (!bcm2711_vulkan_depth_format(attachment->format))
		return ENOTSUP;

	/* A format without stencil has no stencil operations to perform. */
	if (attachment->format != VK_FORMAT_D24_UNORM_S8_UINT) {
		attachment->stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
		attachment->stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
	}

	/* Each aspect's load is one the tile buffer performs. */
	if (attachment->loadOp != VK_ATTACHMENT_LOAD_OP_LOAD &&
	    attachment->loadOp != VK_ATTACHMENT_LOAD_OP_CLEAR &&
	    attachment->loadOp != VK_ATTACHMENT_LOAD_OP_DONT_CARE)
		return ENOTSUP;
	if (attachment->stencilLoadOp != VK_ATTACHMENT_LOAD_OP_LOAD &&
	    attachment->stencilLoadOp != VK_ATTACHMENT_LOAD_OP_CLEAR &&
	    attachment->stencilLoadOp != VK_ATTACHMENT_LOAD_OP_DONT_CARE)
		return ENOTSUP;

	/* Each aspect's store is one the tile buffer performs. */
	if (attachment->storeOp != VK_ATTACHMENT_STORE_OP_STORE && attachment->storeOp != VK_ATTACHMENT_STORE_OP_DONT_CARE)
		return ENOTSUP;
	if (attachment->stencilStoreOp != VK_ATTACHMENT_STORE_OP_STORE && attachment->stencilStoreOp != VK_ATTACHMENT_STORE_OP_DONT_CARE)
		return ENOTSUP;

	/* The one Z+S load cannot leave one aspect to be cleared. */
	if (attachment->loadOp == VK_ATTACHMENT_LOAD_OP_LOAD && attachment->stencilLoadOp == VK_ATTACHMENT_LOAD_OP_CLEAR)
		return ENOTSUP;
	if (attachment->loadOp == VK_ATTACHMENT_LOAD_OP_CLEAR && attachment->stencilLoadOp == VK_ATTACHMENT_LOAD_OP_LOAD)
		return ENOTSUP;

	/* A load reads defined contents; an undefined initial layout has none. */
	if (attachment->initialLayout == VK_IMAGE_LAYOUT_UNDEFINED) {
		if (attachment->loadOp == VK_ATTACHMENT_LOAD_OP_LOAD || attachment->stencilLoadOp == VK_ATTACHMENT_LOAD_OP_LOAD)
			return EINVAL;
	} else {
		error = depth_edge_layout(attachment->initialLayout);
		if (error != 0)
			return error;
	}

	/* The pass leaves the attachment in a defined depth/stencil layout, or ready for a transfer. */
	error = depth_edge_layout(attachment->finalLayout);
	if (error != 0)
		return error;

	/* Succeeded: the attachment's whole lifecycle maps onto tile-buffer loads, clears and stores. */
	return 0;
}

/* Recognizes the layouts a depth/stencil attachment may be in around and during a pass. */
static int
depth_layout(
	VkImageLayout layout)
{
	/* The attachment layout itself, the general one, or one in which its depth is sampled before or after the pass. */
	if (layout != VK_IMAGE_LAYOUT_GENERAL &&
	    layout != VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL &&
	    layout != VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL &&
	    layout != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
		return ENOTSUP;

	/* Succeeded: the layout is one the native pass tracks. */
	return 0;
}

/*
 * Recognizes the layouts a depth/stencil attachment may be in before and
 * after a pass: those of the pass itself, and the transfer ones, as the
 * tile buffer loads and stores the same image whatever the layout (a
 * pass that draws a depth image to be copied from leaves it ready for
 * the copy).
 */
static int
depth_edge_layout(
	VkImageLayout layout)
{
	/* A transfer layout. */
	if (layout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL || layout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL)
		return 0;

	/* Otherwise one of the pass's own. */
	return depth_layout(layout);
}

/* Checks the depth/stencil view against the pass and the framebuffer's extent, then retains it. */
static int
build_depth(
	struct bcm2711_vulkan_framebuffer *framebuffer,
	struct bcm2711_vulkan_object *device)
{
	struct bcm2711_vulkan_pass *pass;
	struct bcm2711_vulkan_image_view *view;
	struct bcm2711_vulkan_resource *image;
	int error;

	/* The same-device view's image has the pass's depth format, attachment usage, memory and size. */
	pass = framebuffer->owner.parent->payload;
	view = framebuffer->depth->payload;
	image = view->owner.parent->payload;
	if (view->owner.device != device)
		return EINVAL;
	if (pass->depth_stencil.format != view->format ||
	    view->stencil ||
	    image->memory == NULL ||
	    image->samples != pass->samples ||
	    (image->usage & VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) == 0)
		return EINVAL;
	if (framebuffer->width > image->width || framebuffer->height > image->height)
		return EINVAL;
	if (!view_layers(pass, view))
		return EINVAL;

	/* The framebuffer keeps the view, and through it the image, until it retires. */
	error = bcm2711_vulkan_object_retain(framebuffer->depth);
	if (error != 0)
		return error;

	/* Succeeded: the caller owns the view's edge. */
	return 0;
}

/* Checks the view a depth/stencil attachment is resolved into against the pass and the framebuffer's extent, then retains it. */
static int
build_depth_resolve(
	struct bcm2711_vulkan_framebuffer *framebuffer,
	struct bcm2711_vulkan_object *device)
{
	struct bcm2711_vulkan_pass *pass;
	struct bcm2711_vulkan_image_view *view;
	struct bcm2711_vulkan_resource *image;
	int error;

	/* The same-device view's image has the pass's depth format, one sample, attachment usage, memory and size. */
	pass = framebuffer->owner.parent->payload;
	view = framebuffer->depth_resolve->payload;
	image = view->owner.parent->payload;
	if (view->owner.device != device)
		return EINVAL;
	if (pass->depth_resolved.format != view->format ||
	    view->stencil ||
	    image->memory == NULL ||
	    image->samples != VK_SAMPLE_COUNT_1_BIT ||
	    (image->usage & VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) == 0)
		return EINVAL;
	if (framebuffer->width > image->width || framebuffer->height > image->height)
		return EINVAL;
	if (!view_layers(pass, view))
		return EINVAL;

	/* The framebuffer keeps the view, and through it the image, until it retires. */
	error = bcm2711_vulkan_object_retain(framebuffer->depth_resolve);
	if (error != 0)
		return error;

	/* Succeeded: the caller owns the view's edge. */
	return 0;
}

/*
 * Reports whether an attachment's view has a layer for each view of the
 * pass (under multiview, more layers than the mask's highest view).
 */
static bool
view_layers(
	const struct bcm2711_vulkan_pass *pass,
	const struct bcm2711_vulkan_image_view *view)
{
	uint32_t highest;

	/* A pass of one view renders the view's first layer. */
	if (pass->view_mask == 0)
		return true;

	/* The highest view the mask names. */
	highest = 31U - (uint32_t)__builtin_clz(pass->view_mask);
	if (view->layers <= highest)
		return false;

	/* Succeeded: every view has its layer. */
	return true;
}

/* Recognizes only the native colour image layouts admitted by this finite graphics path. */
static int
colour_layout(
	VkImageLayout layout)
{
	/*
	 * Presentable client images are translated to GENERAL before the pass
	 * crosses the wire.  Every colour layout is the same raster, so a pass
	 * may also take the image from, or leave it for, a transfer.
	 */
	if (layout != VK_IMAGE_LAYOUT_GENERAL &&
	    layout != VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL &&
	    layout != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL &&
	    layout != VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL &&
	    layout != VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL)
		return ENOTSUP;

	/* Succeeded: this colour lifecycle can be lowered without an unsupported transition. */
	return 0;
}

/* Answers the implemented target's conservative one-pixel render-area granularity. */
static int
granularity(
	struct bcm2711_vulkan_session *session,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	struct bcm2711_vulkan_object *device;
	struct bcm2711_vulkan_object *object;
	struct bcm2711_vulkan_pass *pass;
	uint64_t device_id;
	uint64_t identity;
	uint64_t present;

	/* Discovery applies to an actual same-device pass, rather than a fabricated target identifier. */
	device_id = drv_i915_wire_read_u64(reader);
	identity = drv_i915_wire_read_u64(reader);
	present = drv_i915_wire_read_u64(reader);
	device = bcm2711_vulkan_object_find(session, I915_VK_OBJ_DEVICE, device_id);
	object = bcm2711_vulkan_object_find(session, I915_VK_OBJ_RENDER_PASS, identity);
	if (reader->error != 0 ||
	    present != 1 ||
	    device == NULL ||
	    object == NULL)
		return EINVAL;
	pass = object->payload;
	if (pass->owner.device != device)
		return EINVAL;
	drv_i915_wire_reply_u64(reply, 1);
	drv_i915_wire_reply_u32(reply, 1);
	drv_i915_wire_reply_u32(reply, 1);

	/* Succeeded: arbitrary positive render areas need no hardware tile-alignment expansion at the API boundary. */
	return 0;
}

/* Publishes a complete ordinary creation result and exact nullable output identity. */
static void
target_reply(
	struct i915_wire_writer *reply,
	VkResult status,
	uint64_t identity)
{
	/* Allocation failure still consumes the complete creation record and replies with a null output. */
	drv_i915_wire_reply_u32(reply, (uint32_t)status);
	drv_i915_wire_reply_u64(reply, 1);
	drv_i915_wire_reply_u64(reply, identity);

	/* Succeeded: only a published target identity is acknowledged as live. */
	return;
}
