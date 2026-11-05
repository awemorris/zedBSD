/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* Exact single-colour render targets keep attachment semantics and native storage ownership independent of command arenas. */
#include <kern/kcrt.h>
#include <kern/kmem.h>
#include <uapi/errno.h>

#include "drivers/gpu/bcm2711/vk/vulkan-target.h"
#include "drivers/gpu/bcm2711/vk/vulkan-resource.h"

/* Conservative whole-job synchronization may cover these graphics/transfer/host stages without advertising compute or optional shader stages. */
#define VULKAN_PASS_STAGES (VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT | VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_VERTEX_INPUT_BIT | VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT | VK_PIPELINE_STAGE_HOST_BIT | VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT | VK_PIPELINE_STAGE_ALL_COMMANDS_BIT)
#define VULKAN_PASS_ACCESS (VK_ACCESS_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_INDEX_READ_BIT | VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_UNIFORM_READ_BIT | VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_HOST_READ_BIT | VK_ACCESS_HOST_WRITE_BIT | VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT)

/* One complete decoded creation record contains no application pointer or native owner before validation. */
union target_record {
	struct bcm2711_vulkan_pass pass;
	struct bcm2711_vulkan_framebuffer framebuffer;
};

static int create_target(struct bcm2711_vulkan_session *session, enum i915_vk_object_kind kind, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static int destroy_target(struct bcm2711_vulkan_session *session, enum i915_vk_object_kind kind, struct i915_wire_reader *reader);
static int decode_pass(struct i915_wire_reader *reader, struct bcm2711_vulkan_pass *pass);
static int decode_framebuffer(struct bcm2711_vulkan_session *session, struct i915_wire_reader *reader, struct bcm2711_vulkan_framebuffer *framebuffer);
static int build_framebuffer(struct bcm2711_vulkan_framebuffer *framebuffer, struct bcm2711_vulkan_object *device);
static int release_target(struct bcm2711_vulkan_session *session, void *payload);
static int colour_layout(VkImageLayout layout);
static int decode_attachment(struct i915_wire_reader *reader, VkAttachmentDescription *attachment);
static int depth_attachment(VkAttachmentDescription *attachment);
static int depth_layout(VkImageLayout layout);
static int build_depth(struct bcm2711_vulkan_framebuffer *framebuffer, struct bcm2711_vulkan_object *device);
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
	bool create;
	int error;

	/* Target discovery and lifecycle remain separate from subsequent command-buffer/native draw execution. */
	*handled = 1;
	create = false;
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
		error = create_target(session, kind, reader, reply);
	else
		error = destroy_target(session, kind, reader);
	if (error != 0)
		return error;

	/* Succeeded: the selected target operation has a complete independently owned outcome. */
	return 0;
}

/*
 * Checks compatibility of the implemented one-colour single-sample subpass independently of load/store and layouts.
 *
 * A depth/stencil attachment must be present in both, in the same format.
 */
int
bcm2711_vulkan_pass_compatible(
	const struct bcm2711_vulkan_pass *first,
	const struct bcm2711_vulkan_pass *second)
{
	/* All admitted passes have one colour reference to attachment zero, one sample and no optional attachments. */
	if (first == NULL || second == NULL)
		return EINVAL;
	if (first->colour.format != second->colour.format)
		return EINVAL;

	/* Both have the same depth/stencil attachment, or neither has one. */
	if (first->depth != second->depth)
		return EINVAL;
	if (first->depth && first->depth_stencil.format != second->depth_stencil.format)
		return EINVAL;

	/* Succeeded: distinct clear/load passes can use the same framebuffer and compatible compiled pipeline. */
	return 0;
}

/*
 * Tells whether a format is one of the tile buffer's depth/stencil formats.
 *
 * D24S8 has depth and stencil, X8_D24 depth alone (ws141-p017).
 */
bool
bcm2711_vulkan_depth_format(
	VkFormat format)
{
	/* The two 32-bit formats whose depth is the tile buffer's 24-bit depth. */
	if (format == VK_FORMAT_D24_UNORM_S8_UINT)
		return true;
	if (format == VK_FORMAT_X8_D24_UNORM_PACK32)
		return true;

	/* Any other format is colour or unsupported. */
	return false;
}

/* Publishes a completely decoded target after acquiring every immutable device/pass/view ownership edge. */
static int
create_target(
	struct bcm2711_vulkan_session *session,
	enum i915_vk_object_kind kind,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
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
	if (kind == I915_VK_OBJ_RENDER_PASS)
		error = decode_pass(reader, &record.pass);
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
			framebuffer->view = record.framebuffer.view;
			framebuffer->depth = record.framebuffer.depth;
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

/* Decodes the exact one-colour subpass, its optional depth/stencil attachment and the finite external dependency contract from the actual client record. */
static int
decode_pass(
	struct i915_wire_reader *reader,
	struct bcm2711_vulkan_pass *pass)
{
	VkAttachmentDescription *colour;
	VkSubpassDependency *dependency;
	uint64_t chain;
	uint64_t array;
	uint32_t structure;
	uint32_t flags;
	uint32_t count;
	uint32_t attachment;
	uint32_t layout;
	uint32_t index;
	int error;

	/* Only the implemented one-colour, one-sample render pass shape is accepted before reading its attachment. */
	structure = drv_i915_wire_read_u32(reader);
	chain = drv_i915_wire_read_u64(reader);
	flags = drv_i915_wire_read_u32(reader);
	count = drv_i915_wire_read_u32(reader);
	array = drv_i915_wire_read_u64(reader);
	if (reader->error != 0 ||
	    structure != VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO ||
	    chain != 0 ||
	    flags != 0 ||
	    (count != 1 && count != 2) ||
	    array != count)
		return ENOTSUP;
	colour = &pass->colour;
	error = decode_attachment(reader, colour);
	if (error != 0)
		return error;

	/* A second attachment is the depth/stencil one, decoded and checked on its own. */
	if (count == 2) {
		pass->depth = true;
		error = decode_attachment(reader, &pass->depth_stencil);
		if (error != 0)
			return error;
		error = depth_attachment(&pass->depth_stencil);
		if (error != 0)
			return error;
	}

	/* Clear/load/discard remain distinct lowering obligations; only colour layouts and native RGBA/BGRA are admitted. */
	if (colour->flags != 0 || colour->samples != VK_SAMPLE_COUNT_1_BIT)
		return ENOTSUP;
	if (colour->format != VK_FORMAT_R8G8B8A8_UNORM && colour->format != VK_FORMAT_B8G8R8A8_UNORM)
		return ENOTSUP;
	if (colour->loadOp != VK_ATTACHMENT_LOAD_OP_LOAD &&
	    colour->loadOp != VK_ATTACHMENT_LOAD_OP_CLEAR &&
	    colour->loadOp != VK_ATTACHMENT_LOAD_OP_DONT_CARE)
		return ENOTSUP;
	if (colour->storeOp != VK_ATTACHMENT_STORE_OP_STORE && colour->storeOp != VK_ATTACHMENT_STORE_OP_DONT_CARE)
		return ENOTSUP;
	if (colour->stencilLoadOp != VK_ATTACHMENT_LOAD_OP_DONT_CARE || colour->stencilStoreOp != VK_ATTACHMENT_STORE_OP_DONT_CARE)
		return ENOTSUP;
	if (colour->initialLayout == VK_IMAGE_LAYOUT_UNDEFINED) {
		if (colour->loadOp == VK_ATTACHMENT_LOAD_OP_LOAD)
			return EINVAL;
	} else {
		error = colour_layout(colour->initialLayout);
		if (error != 0)
			return error;
	}

	/* A pass cannot leave its stored attachment in an undefined or unsupported final layout. */
	error = colour_layout(colour->finalLayout);
	if (error != 0)
		return error;
	count = drv_i915_wire_read_u32(reader);
	array = drv_i915_wire_read_u64(reader);
	if (reader->error != 0 ||
	    count != 1 ||
	    array != 1)
		return ENOTSUP;
	flags = drv_i915_wire_read_u32(reader);
	attachment = drv_i915_wire_read_u32(reader);
	count = drv_i915_wire_read_u32(reader);
	array = drv_i915_wire_read_u64(reader);
	if (reader->error != 0 ||
	    flags != 0 ||
	    attachment != VK_PIPELINE_BIND_POINT_GRAPHICS ||
	    count != 0 ||
	    array != 0)
		return ENOTSUP;

	/* The sole subpass references attachment zero for colour and has no input, resolve or preserve attachment. */
	count = drv_i915_wire_read_u32(reader);
	array = drv_i915_wire_read_u64(reader);
	attachment = drv_i915_wire_read_u32(reader);
	layout = drv_i915_wire_read_u32(reader);
	if (reader->error != 0 ||
	    count != 1 ||
	    array != 1 ||
	    attachment != 0 ||
	    layout != VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL)
		return ENOTSUP;
	array = drv_i915_wire_read_u64(reader);
	if (reader->error != 0 || array != 0)
		return ENOTSUP;

	/* A pass's depth/stencil attachment is the subpass's, at index one; a colour-only pass has none. */
	array = drv_i915_wire_read_u64(reader);
	if (reader->error != 0)
		return EINVAL;
	if (array != 0 && array != 1)
		return ENOTSUP;
	if ((array == 1) != pass->depth)
		return ENOTSUP;
	if (array == 1) {
		attachment = drv_i915_wire_read_u32(reader);
		layout = drv_i915_wire_read_u32(reader);
		if (reader->error != 0)
			return EINVAL;
		if (attachment != 1)
			return ENOTSUP;
		error = depth_layout(layout);
		if (error != 0)
			return error;
	}

	/* No attachment is preserved through the single subpass. */
	count = drv_i915_wire_read_u32(reader);
	array = drv_i915_wire_read_u64(reader);
	if (reader->error != 0 ||
	    count != 0 ||
	    array != 0)
		return ENOTSUP;

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
	}

	/* Succeeded: the pass retains every supported attachment and dependency field without a command-arena pointer. */
	return 0;
}

/* Decodes one full-colour framebuffer using borrowed registry owners that are retained only after complete validation. */
static int
decode_framebuffer(
	struct bcm2711_vulkan_session *session,
	struct i915_wire_reader *reader,
	struct bcm2711_vulkan_framebuffer *framebuffer)
{
	uint64_t chain;
	uint64_t identity;
	uint64_t array;
	uint32_t structure;
	uint32_t flags;
	uint32_t count;
	uint32_t layers;
	struct bcm2711_vulkan_pass *pass;

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
	    (count != 1 && count != 2) ||
	    array != count)
		return ENOTSUP;
	framebuffer->owner.parent = bcm2711_vulkan_object_find(session, I915_VK_OBJ_RENDER_PASS, identity);
	identity = drv_i915_wire_read_u64(reader);
	framebuffer->view = bcm2711_vulkan_object_find(session, I915_VK_OBJ_IMAGE_VIEW, identity);

	/* A second view is the depth/stencil attachment's, which the pass must have. */
	if (count == 2) {
		identity = drv_i915_wire_read_u64(reader);
		framebuffer->depth = bcm2711_vulkan_object_find(session, I915_VK_OBJ_IMAGE_VIEW, identity);
		if (reader->error != 0 || framebuffer->depth == NULL)
			return EINVAL;
	}
	framebuffer->width = drv_i915_wire_read_u32(reader);
	framebuffer->height = drv_i915_wire_read_u32(reader);
	layers = drv_i915_wire_read_u32(reader);
	if (reader->error != 0 ||
	    framebuffer->owner.parent == NULL ||
	    framebuffer->view == NULL)
		return EINVAL;

	/* The views match the pass's attachments one for one. */
	pass = framebuffer->owner.parent->payload;
	if (pass->depth != (framebuffer->depth != NULL))
		return EINVAL;

	/* The extent is the single-layer one native passes render. */
	if (framebuffer->width == 0 ||
	    framebuffer->height == 0 ||
	    layers != 1)
		return ENOTSUP;

	/* Succeeded: complete typed owners and declared dimensions are available for construction. */
	return 0;
}

/* Verifies exact native colour compatibility and acquires independent pass/view edges before payload publication. */
static int
build_framebuffer(
	struct bcm2711_vulkan_framebuffer *framebuffer,
	struct bcm2711_vulkan_object *device)
{
	struct bcm2711_vulkan_pass *pass;
	struct bcm2711_vulkan_image_view *view;
	struct bcm2711_vulkan_resource *image;
	int error;
	int retired;

	/* Immutable same-device parents supply the actual bound image format and extent. */
	pass = framebuffer->owner.parent->payload;
	view = framebuffer->view->payload;
	image = view->owner.parent->payload;
	if (pass->owner.device != device || view->owner.device != device)
		return EINVAL;
	if (pass->colour.format != view->format ||
	    image->memory == NULL ||
	    (image->usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) == 0)
		return EINVAL;
	if (framebuffer->width > image->width || framebuffer->height > image->height)
		return EINVAL;

	/* Retain the complete target graph in dependency order, unwinding only the successfully acquired pass if view acquisition fails. */
	error = bcm2711_vulkan_object_retain(framebuffer->owner.parent);
	if (error != 0)
		return error;
	error = bcm2711_vulkan_object_retain(framebuffer->view);
	if (error != 0) {
		retired = bcm2711_vulkan_object_release(framebuffer->owner.parent);
		if (retired != 0)
			return retired;
		return error;
	}

	/* The depth/stencil view, when the pass has one, is checked and retained last; failure unwinds the colour view and the pass. */
	if (framebuffer->depth != NULL) {
		error = build_depth(framebuffer, device);
		if (error != 0) {
			retired = bcm2711_vulkan_object_release(framebuffer->view);
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

/* Retires framebuffer colour and common pass/device edges, preserving every cleanup obligation and the first native error. */
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
		error = bcm2711_vulkan_object_release(framebuffer->view);
		if (framebuffer->depth != NULL) {
			retired = bcm2711_vulkan_object_release(framebuffer->depth);
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

	/* One sample of a tile-buffer depth format. */
	if (attachment->flags != 0 || attachment->samples != VK_SAMPLE_COUNT_1_BIT)
		return ENOTSUP;
	if (attachment->format != VK_FORMAT_D24_UNORM_S8_UINT && attachment->format != VK_FORMAT_X8_D24_UNORM_PACK32)
		return ENOTSUP;

	/* A format without stencil has no stencil operations to perform. */
	if (attachment->format == VK_FORMAT_X8_D24_UNORM_PACK32) {
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
		error = depth_layout(attachment->initialLayout);
		if (error != 0)
			return error;
	}

	/* The pass leaves the attachment in a defined depth/stencil layout. */
	error = depth_layout(attachment->finalLayout);
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
	/* The attachment layout itself, or the general one. */
	if (layout != VK_IMAGE_LAYOUT_GENERAL && layout != VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL)
		return ENOTSUP;

	/* Succeeded: the layout is one the native pass tracks. */
	return 0;
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
	    image->memory == NULL ||
	    (image->usage & VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) == 0)
		return EINVAL;
	if (framebuffer->width > image->width || framebuffer->height > image->height)
		return EINVAL;

	/* The framebuffer keeps the view, and through it the image, until it retires. */
	error = bcm2711_vulkan_object_retain(framebuffer->depth);
	if (error != 0)
		return error;

	/* Succeeded: the caller owns the view's edge. */
	return 0;
}

/* Recognizes only the native colour image layouts admitted by this finite graphics path. */
static int
colour_layout(
	VkImageLayout layout)
{
	/* Presentable client images are translated to GENERAL before the pass crosses the wire. */
	if (layout != VK_IMAGE_LAYOUT_GENERAL &&
	    layout != VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL &&
	    layout != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
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
