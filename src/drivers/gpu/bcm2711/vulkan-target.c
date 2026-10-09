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

#include "drivers/gpu/bcm2711/vulkan-target.h"
#include "drivers/gpu/bcm2711/vulkan-resource.h"

/* Conservative whole-job synchronization may cover these graphics/transfer/host stages without advertising compute or optional shader stages. */
#define VULKAN_PASS_STAGES (VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT | VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_VERTEX_INPUT_BIT | VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT | VK_PIPELINE_STAGE_HOST_BIT | VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT | VK_PIPELINE_STAGE_ALL_COMMANDS_BIT)
#define VULKAN_PASS_ACCESS (VK_ACCESS_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_INDEX_READ_BIT | VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_UNIFORM_READ_BIT | VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_HOST_READ_BIT | VK_ACCESS_HOST_WRITE_BIT | VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT)

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

	/* Succeeded: distinct clear/load passes can use the same framebuffer and compatible compiled pipeline. */
	return 0;
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
	if (reader->error != 0 || allocator != 0 || present != 1 || identity == 0)
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

/* Decodes the exact one-colour subpass and finite external dependency contract from the actual client record. */
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
	if (reader->error != 0 || structure != VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO || chain != 0 || flags != 0 || count != 1 || array != 1)
		return ENOTSUP;
	colour = &pass->colour;
	colour->flags = drv_i915_wire_read_u32(reader);
	colour->format = drv_i915_wire_read_u32(reader);
	colour->samples = drv_i915_wire_read_u32(reader);
	colour->loadOp = drv_i915_wire_read_u32(reader);
	colour->storeOp = drv_i915_wire_read_u32(reader);
	colour->stencilLoadOp = drv_i915_wire_read_u32(reader);
	colour->stencilStoreOp = drv_i915_wire_read_u32(reader);
	colour->initialLayout = drv_i915_wire_read_u32(reader);
	colour->finalLayout = drv_i915_wire_read_u32(reader);

	/* Clear/load/discard remain distinct lowering obligations; only colour layouts and native RGBA/BGRA are admitted. */
	if (reader->error != 0 || colour->flags != 0 || colour->samples != VK_SAMPLE_COUNT_1_BIT)
		return ENOTSUP;
	if (colour->format != VK_FORMAT_R8G8B8A8_UNORM && colour->format != VK_FORMAT_B8G8R8A8_UNORM)
		return ENOTSUP;
	if (colour->loadOp != VK_ATTACHMENT_LOAD_OP_LOAD && colour->loadOp != VK_ATTACHMENT_LOAD_OP_CLEAR && colour->loadOp != VK_ATTACHMENT_LOAD_OP_DONT_CARE)
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
	if (reader->error != 0 || count != 1 || array != 1)
		return ENOTSUP;
	flags = drv_i915_wire_read_u32(reader);
	attachment = drv_i915_wire_read_u32(reader);
	count = drv_i915_wire_read_u32(reader);
	array = drv_i915_wire_read_u64(reader);
	if (reader->error != 0 || flags != 0 || attachment != VK_PIPELINE_BIND_POINT_GRAPHICS || count != 0 || array != 0)
		return ENOTSUP;

	/* The sole subpass references attachment zero for colour and has no input, resolve, depth or preserve attachment. */
	count = drv_i915_wire_read_u32(reader);
	array = drv_i915_wire_read_u64(reader);
	attachment = drv_i915_wire_read_u32(reader);
	layout = drv_i915_wire_read_u32(reader);
	if (reader->error != 0 || count != 1 || array != 1 || attachment != 0 || layout != VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL)
		return ENOTSUP;
	array = drv_i915_wire_read_u64(reader);
	if (reader->error != 0 || array != 0)
		return ENOTSUP;
	array = drv_i915_wire_read_u64(reader);
	if (reader->error != 0 || array != 0)
		return ENOTSUP;
	count = drv_i915_wire_read_u32(reader);
	array = drv_i915_wire_read_u64(reader);
	if (reader->error != 0 || count != 0 || array != 0)
		return ENOTSUP;

	/* Each external dependency is retained for conservative whole-job ordering and cache visibility during native execution. */
	count = drv_i915_wire_read_u32(reader);
	array = drv_i915_wire_read_u64(reader);
	if (reader->error != 0 || count > BCM2711_VULKAN_PASS_DEPENDENCIES || array != count)
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

	/* Ordinary framebuffer flags and chains cannot silently enable imageless or multiview target semantics. */
	structure = drv_i915_wire_read_u32(reader);
	chain = drv_i915_wire_read_u64(reader);
	flags = drv_i915_wire_read_u32(reader);
	identity = drv_i915_wire_read_u64(reader);
	count = drv_i915_wire_read_u32(reader);
	array = drv_i915_wire_read_u64(reader);
	if (reader->error != 0 || structure != VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO || chain != 0 || flags != 0 || count != 1 || array != 1)
		return ENOTSUP;
	framebuffer->owner.parent = bcm2711_vulkan_object_find(session, I915_VK_OBJ_RENDER_PASS, identity);
	identity = drv_i915_wire_read_u64(reader);
	framebuffer->view = bcm2711_vulkan_object_find(session, I915_VK_OBJ_IMAGE_VIEW, identity);
	framebuffer->width = drv_i915_wire_read_u32(reader);
	framebuffer->height = drv_i915_wire_read_u32(reader);
	layers = drv_i915_wire_read_u32(reader);
	if (reader->error != 0 || framebuffer->owner.parent == NULL || framebuffer->view == NULL)
		return EINVAL;
	if (framebuffer->width == 0 || framebuffer->height == 0 || layers != 1)
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
	if (pass->colour.format != view->format || image->memory == NULL || (image->usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) == 0)
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

	/* Succeeded: the caller now owns both borrowed graph edges and must publish or release them. */
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

/* Recognizes only the native colour image layouts admitted by this finite graphics path. */
static int
colour_layout(
	VkImageLayout layout)
{
	/* Presentable client images are translated to GENERAL before the pass crosses the wire. */
	if (layout != VK_IMAGE_LAYOUT_GENERAL && layout != VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL && layout != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
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
	if (reader->error != 0 || present != 1 || device == NULL || object == NULL)
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
