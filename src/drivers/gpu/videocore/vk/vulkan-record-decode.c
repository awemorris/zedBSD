/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* Count-selected recording payloads copy exact client words and resolve borrowed typed IDs without acquiring partial owners. */
#include <uapi/errno.h>

#include "drivers/gpu/videocore/vk/vulkan-record.h"
#include "drivers/gpu/videocore/vk/vulkan-occlusion.h"
#include "drivers/gpu/videocore/vk/vulkan-resource.h"

static int decode_clear(struct bcm2711_vulkan_session *session, struct i915_wire_reader *reader, struct bcm2711_vulkan_record *record);
static int decode_depth_clear(struct bcm2711_vulkan_session *session, struct i915_wire_reader *reader, struct bcm2711_vulkan_record *record);
static int decode_sets(struct bcm2711_vulkan_session *session, struct i915_wire_reader *reader, struct bcm2711_vulkan_record *record);
static int decode_vertices(struct bcm2711_vulkan_session *session, struct i915_wire_reader *reader, struct bcm2711_vulkan_record *record);
static int decode_push(struct bcm2711_vulkan_session *session, struct i915_wire_reader *reader, struct bcm2711_vulkan_record *record);
static int decode_area(struct i915_wire_reader *reader, struct bcm2711_vulkan_record *record);
static int decode_dynamic(struct i915_wire_reader *reader, struct bcm2711_vulkan_record *record);
static int decode_attachments(struct i915_wire_reader *reader, struct bcm2711_vulkan_record *record);
static int decode_pass(struct bcm2711_vulkan_session *session, struct i915_wire_reader *reader, struct bcm2711_vulkan_record *record);

/*
 * Decodes one complete admitted graphics or compute record without retaining wire or application pointers.
 */
int
bcm2711_vulkan_record_decode(
	struct bcm2711_vulkan_session *session,
	struct i915_wire_reader *reader,
	struct bcm2711_vulkan_record *record)
{
	uint64_t identity;
	uint32_t point;
	uint32_t index;
	int error;

	/* Every selected opcode has an independent fixed or count-bounded client representation. */
	error = 0;
	switch (record->opcode) {
	case GPU_OP_CMD_BIND_PIPELINE:
		point = drv_i915_wire_read_u32(reader);
		identity = drv_i915_wire_read_u64(reader);
		if (point != VK_PIPELINE_BIND_POINT_GRAPHICS && point != VK_PIPELINE_BIND_POINT_COMPUTE)
			return ENOTSUP;

		/* The compute bind point is kept as the compute stage, the graphics one as no stage. */
		record->stages = 0;
		if (point == VK_PIPELINE_BIND_POINT_COMPUTE)
			record->stages = VK_SHADER_STAGE_COMPUTE_BIT;
		record->objects[0] = bcm2711_vulkan_object_find(session, I915_VK_OBJ_PIPELINE, identity);
		break;
	case GPU_OP_CMD_BIND_DESCRIPTOR_SETS:
		error = decode_sets(session, reader, record);
		break;
	case GPU_OP_CMD_BIND_VERTEX_BUFFERS:
		error = decode_vertices(session, reader, record);
		break;
	case GPU_OP_CMD_PUSH_CONSTANTS:
		error = decode_push(session, reader, record);
		break;
	case GPU_OP_CMD_SET_VIEWPORT:
	case GPU_OP_CMD_SET_SCISSOR:
		error = decode_area(reader, record);
		break;
	case GPU_OP_CMD_SET_LINE_WIDTH:
	case GPU_OP_CMD_SET_DEPTH_BIAS:
	case GPU_OP_CMD_SET_BLEND_CONSTANTS:
	case GPU_OP_CMD_SET_STENCIL_COMPARE_MASK:
	case GPU_OP_CMD_SET_STENCIL_WRITE_MASK:
	case GPU_OP_CMD_SET_STENCIL_REFERENCE:
		error = decode_dynamic(reader, record);
		break;
	case GPU_OP_CMD_BEGIN_RENDER_PASS:
		error = decode_pass(session, reader, record);
		break;
	case GPU_OP_CMD_CLEAR_COLOR_IMAGE:
		error = decode_clear(session, reader, record);
		break;
	case GPU_OP_CMD_CLEAR_DEPTH_STENCIL_IMAGE:
		error = decode_depth_clear(session, reader, record);
		break;
	case GPU_OP_CMD_CLEAR_ATTACHMENTS:
		error = decode_attachments(reader, record);
		break;
	case GPU_OP_CMD_BIND_INDEX_BUFFER:
		/* The buffer, its 64-bit offset and the index type, which first keeps. */
		identity = drv_i915_wire_read_u64(reader);
		record->offsets[0] = drv_i915_wire_read_u64(reader);
		record->first = drv_i915_wire_read_u32(reader);
		record->objects[0] = bcm2711_vulkan_object_find(session, I915_VK_OBJ_BUFFER, identity);
		break;
	case GPU_OP_CMD_DRAW:
		/* Draw's four words preserve exact vertex/instance counts and base selections. */
		for (index = 0; index < 4; index++)
			record->words[index] = drv_i915_wire_read_u32(reader);
		break;
	case GPU_OP_CMD_DRAW_INDEXED:
		/* Index count, instance count, first index, signed vertex offset and first instance. */
		for (index = 0; index < 5; index++)
			record->words[index] = drv_i915_wire_read_u32(reader);
		break;
	case GPU_OP_CMD_END_RENDER_PASS:
		break;
	case GPU_OP_CMD_DISPATCH:
		/* The workgroup counts along x, y and z; the first workgroup is zero. */
		for (index = 0; index < 3; index++)
			record->words[index] = drv_i915_wire_read_u32(reader);
		break;
	case GPU_OP_CMD_DISPATCH_BASE:
		/*
		 * vkCmdDispatchBase: the first workgroup along x, y and z, then
		 * the counts.  It is kept as a dispatch whose words 3 to 5 hold
		 * the first workgroup.
		 */
		for (index = 0; index < 3; index++)
			record->words[3U + index] = drv_i915_wire_read_u32(reader);
		for (index = 0; index < 3; index++)
			record->words[index] = drv_i915_wire_read_u32(reader);
		record->opcode = GPU_OP_CMD_DISPATCH;
		break;
	case GPU_OP_CMD_DISPATCH_INDIRECT:
		/* The buffer holding the three counts, and their offset in it. */
		identity = drv_i915_wire_read_u64(reader);
		record->offsets[0] = drv_i915_wire_read_u64(reader);
		record->objects[0] = bcm2711_vulkan_object_find(session, I915_VK_OBJ_BUFFER, identity);
		break;
	case GPU_OP_CMD_BEGIN_QUERY:
	case GPU_OP_CMD_END_QUERY:
	case GPU_OP_CMD_RESET_QUERY_POOL:
		/*
		 * The pool, the query (or the reset's first query and count) and
		 * a begin's VkQueryControlFlags, which words[0] keeps; the counts
		 * are exact either way.
		 */
		identity = drv_i915_wire_read_u64(reader);
		record->first = drv_i915_wire_read_u32(reader);
		record->count = 1U;
		if (record->opcode == GPU_OP_CMD_BEGIN_QUERY)
			record->words[0] = drv_i915_wire_read_u32(reader);
		if (record->opcode == GPU_OP_CMD_RESET_QUERY_POOL)
			record->count = drv_i915_wire_read_u32(reader);
		record->objects[0] = bcm2711_vulkan_object_find(session, I915_VK_OBJ_QUERY_POOL, identity);
		break;
	default:
		return ENOTSUP;
	}

	/* Incomplete framing cannot be skipped safely to a later record, even after an earlier recording failure. */
	if (reader->error != 0)
		return EINVAL;
	if (error != 0)
		return error;

	/* Succeeded: the caller owns copied scalar metadata and controller-serialized borrowed input selections. */
	return 0;
}

/* Decodes exact graphics or compute set selections and their dynamic uniform-buffer offsets. */
static int
decode_sets(
	struct bcm2711_vulkan_session *session,
	struct i915_wire_reader *reader,
	struct bcm2711_vulkan_record *record)
{
	uint64_t identity;
	uint64_t array;
	uint32_t point;
	uint32_t dynamic;
	uint32_t index;

	/* Layout identity precedes a finite set array; all typed selections are borrowed until complete record validation. */
	point = drv_i915_wire_read_u32(reader);
	identity = drv_i915_wire_read_u64(reader);
	record->first = drv_i915_wire_read_u32(reader);
	record->count = drv_i915_wire_read_u32(reader);
	array = drv_i915_wire_read_u64(reader);
	if ((point != VK_PIPELINE_BIND_POINT_GRAPHICS && point != VK_PIPELINE_BIND_POINT_COMPUTE) ||
	    record->first > BCM2711_VULKAN_PIPELINE_SETS ||
	    record->count > BCM2711_VULKAN_PIPELINE_SETS - record->first ||
	    array != record->count)
		return ENOTSUP;
	record->objects[0] = bcm2711_vulkan_object_find(session, I915_VK_OBJ_PIPELINE_LAYOUT, identity);

	/* The compute bind point is kept as the compute stage, the graphics one as no stage. */
	record->stages = 0;
	if (point == VK_PIPELINE_BIND_POINT_COMPUTE)
		record->stages = VK_SHADER_STAGE_COMPUTE_BIT;

	/* Each referenced set has its own eventual retained recording edge. */
	for (index = 0; index < record->count; index++) {
		identity = drv_i915_wire_read_u64(reader);
		record->objects[index + 1U] = bcm2711_vulkan_object_find(session, I915_VK_OBJ_DESCRIPTOR_SET, identity);
	}

	/*
	 * The dynamic offsets, one for each dynamic uniform buffer of the sets
	 * in set and binding order; the sets hold at most four uniform buffers
	 * each, so they fit the words.
	 */
	dynamic = drv_i915_wire_read_u32(reader);
	array = drv_i915_wire_read_u64(reader);
	if (reader->error != 0 ||
	    dynamic > BCM2711_VULKAN_PIPELINE_SETS * 4U ||
	    array != dynamic)
		return EINVAL;
	record->dynamic_count = dynamic;
	for (index = 0; index < dynamic; index++)
		record->words[index] = drv_i915_wire_read_u32(reader);

	/* Succeeded: the set binding and its offsets belong to the record. */
	return 0;
}

/* Decodes a finite vertex-buffer slice and its independent 64-bit logical offsets. */
static int
decode_vertices(
	struct bcm2711_vulkan_session *session,
	struct i915_wire_reader *reader,
	struct bcm2711_vulkan_record *record)
{
	uint64_t identity;
	uint64_t array;
	uint32_t index;

	/* The selected slice fits the existing native sixteen-binding interface. */
	record->first = drv_i915_wire_read_u32(reader);
	record->count = drv_i915_wire_read_u32(reader);
	array = drv_i915_wire_read_u64(reader);
	if (record->first > BCM2711_VULKAN_VERTEX_BINDINGS ||
	    record->count > BCM2711_VULKAN_VERTEX_BINDINGS - record->first ||
	    array != record->count)
		return ENOTSUP;

	/* Buffer identities precede their separate count-selected offset vector in actual client order. */
	for (index = 0; index < record->count; index++) {
		identity = drv_i915_wire_read_u64(reader);
		record->objects[index] = bcm2711_vulkan_object_find(session, I915_VK_OBJ_BUFFER, identity);
	}

	/* Offsets remain logical resource selections; preparation later checks complete vertex-fetch intervals. */
	array = drv_i915_wire_read_u64(reader);
	if (array != record->count)
		return EINVAL;
	for (index = 0; index < record->count; index++)
		record->offsets[index] = drv_i915_wire_read_u64(reader);

	/* Succeeded: all vertex selection fields are independent of caller array storage. */
	return 0;
}

/* Decodes an exact aligned push-byte interval into self-owned raw words. */
static int
decode_push(
	struct bcm2711_vulkan_session *session,
	struct i915_wire_reader *reader,
	struct bcm2711_vulkan_record *record)
{
	uint64_t identity;
	uint64_t array;
	uint32_t index;

	/* The finite scalar compiler interface admits only stage words within 128 bytes. */
	identity = drv_i915_wire_read_u64(reader);
	record->stages = drv_i915_wire_read_u32(reader);
	record->first = drv_i915_wire_read_u32(reader);
	record->count = drv_i915_wire_read_u32(reader);
	array = drv_i915_wire_read_u64(reader);
	if (record->first > BCM2711_VULKAN_PUSH_WORDS * 4U ||
	    record->count == 0 ||
	    record->count > BCM2711_VULKAN_PUSH_WORDS * 4U - record->first ||
	    ((record->first | record->count) & 3U) != 0 ||
	    array != record->count)
		return ENOTSUP;
	record->objects[0] = bcm2711_vulkan_object_find(session, I915_VK_OBJ_PIPELINE_LAYOUT, identity);

	/* Raw words preserve all shader constants without executing floating-point instructions inside the kernel. */
	for (index = 0; index < record->count / 4U; index++)
		record->words[index] = drv_i915_wire_read_u32(reader);

	/* Succeeded: complete push contents belong to this immutable recording event. */
	return 0;
}

/* Decodes one dynamic viewport or scissor selected by the native single-viewport interface. */
static int
decode_area(
	struct i915_wire_reader *reader,
	struct bcm2711_vulkan_record *record)
{
	uint64_t array;
	uint32_t index;

	/* No static pipeline arrays or extra viewports may leak into an admitted dynamic record. */
	record->first = drv_i915_wire_read_u32(reader);
	record->count = drv_i915_wire_read_u32(reader);
	array = drv_i915_wire_read_u64(reader);
	if (record->first != 0 ||
	    record->count != 1 ||
	    array != 1)
		return ENOTSUP;
	if (record->opcode == GPU_OP_CMD_SET_VIEWPORT) {
		/* All six IEEE words retain the application's exact transform and depth range. */
		for (index = 0; index < 6; index++)
			record->words[index] = drv_i915_wire_read_u32(reader);
	} else {
		/* Standard scissor fields use signed offsets followed by unsigned extents. */
		record->area.offset.x = (int32_t)drv_i915_wire_read_u32(reader);
		record->area.offset.y = (int32_t)drv_i915_wire_read_u32(reader);
		record->area.extent.width = drv_i915_wire_read_u32(reader);
		record->area.extent.height = drv_i915_wire_read_u32(reader);
	}

	/* Succeeded: one complete dynamic clipping selection has copied storage. */
	return 0;
}

/*
 * Decodes one of the other dynamic-state commands into the record's words.
 *
 * The floats stay IEEE bits.  A stencil command keeps its face mask in
 * first and its value in the first word.
 */
static int
decode_dynamic(
	struct i915_wire_reader *reader,
	struct bcm2711_vulkan_record *record)
{
	uint64_t array;
	uint32_t index;

	/* Each command's fixed words, in the order of its Vulkan arguments. */
	switch (record->opcode) {
	case GPU_OP_CMD_SET_LINE_WIDTH:
		record->words[0] = drv_i915_wire_read_u32(reader);
		break;
	case GPU_OP_CMD_SET_DEPTH_BIAS:
		/* The constant factor, the clamp and the slope factor. */
		for (index = 0; index < 3; index++)
			record->words[index] = drv_i915_wire_read_u32(reader);
		break;
	case GPU_OP_CMD_SET_BLEND_CONSTANTS:
		/* The four constants follow their array's count. */
		array = drv_i915_wire_read_u64(reader);
		if (reader->error != 0 || array != 4)
			return EINVAL;
		for (index = 0; index < 4; index++)
			record->words[index] = drv_i915_wire_read_u32(reader);
		break;
	default:
		/* The stencil face mask and the mask or reference. */
		record->first = drv_i915_wire_read_u32(reader);
		record->words[0] = drv_i915_wire_read_u32(reader);
		break;
	}

	/* Succeeded: the values belong to the record. */
	return 0;
}

/*
 * Decodes a clear of the current pass's attachments.
 *
 * A colour entry names a colour slot of the subpass (one beyond the slots
 * the pass can have clears nothing).  A rectangle covers layer 0 alone, the
 * only layer.  The record's words are laid out as vulkan-record.h says.
 */
static int
decode_attachments(
	struct i915_wire_reader *reader,
	struct bcm2711_vulkan_record *record)
{
	uint64_t array;
	uint32_t count;
	uint32_t index;
	uint32_t component;
	uint32_t aspects;
	uint32_t attachment;
	uint32_t branch;
	uint32_t bits;
	uint32_t base;
	uint32_t layers;
	uint32_t *rect;

	/* The attachments: each its aspects, its colour index and a tagged value. */
	count = drv_i915_wire_read_u32(reader);
	array = drv_i915_wire_read_u64(reader);
	if (reader->error != 0 ||
	    count == 0 ||
	    count > 8 ||
	    array != count)
		return EINVAL;
	for (index = 0; index < count; index++) {
		aspects = drv_i915_wire_read_u32(reader);
		attachment = drv_i915_wire_read_u32(reader);
		branch = drv_i915_wire_read_u32(reader);

		/* A depth/stencil value: the depth float's bits and the stencil value. */
		if (branch == 1) {
			record->words[BCM2711_VULKAN_CLEAR_DEPTH] = drv_i915_wire_read_u32(reader);
			record->words[BCM2711_VULKAN_CLEAR_DEPTH + 1U] = drv_i915_wire_read_u32(reader);
			if (aspects == 0 || (aspects & ~(uint32_t)(VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)) != 0)
				return EINVAL;
			record->words[BCM2711_VULKAN_CLEAR_ASPECTS] |= aspects;
			continue;
		}

		/* A colour value: representation two and four words, the raw bits of the union. */
		bits = drv_i915_wire_read_u32(reader);
		array = drv_i915_wire_read_u64(reader);
		if (reader->error != 0 ||
		    branch != 0 ||
		    bits != 2 ||
		    array != 4 ||
		    aspects != VK_IMAGE_ASPECT_COLOR_BIT)
			return EINVAL;
		for (component = 0; component < 4; component++) {
			bits = drv_i915_wire_read_u32(reader);
			if (attachment < BCM2711_VULKAN_PASS_COLOURS)
				record->words[attachment * 4U + component] = bits;
		}
		if (attachment < BCM2711_VULKAN_PASS_COLOURS)
			record->words[BCM2711_VULKAN_CLEAR_ASPECTS] |= 1U << (BCM2711_VULKAN_CLEAR_COLOUR_SHIFT + attachment);
	}

	/* The rectangles, each on layer 0 alone. */
	count = drv_i915_wire_read_u32(reader);
	array = drv_i915_wire_read_u64(reader);
	if (reader->error != 0 || count == 0 || array != count)
		return EINVAL;
	if (count > BCM2711_VULKAN_CLEAR_RECTS)
		return ENOTSUP;
	record->count = count;
	for (index = 0; index < count; index++) {
		rect = &record->words[BCM2711_VULKAN_CLEAR_RECT_WORD + index * 4U];
		for (component = 0; component < 4; component++)
			rect[component] = drv_i915_wire_read_u32(reader);
		base = drv_i915_wire_read_u32(reader);
		layers = drv_i915_wire_read_u32(reader);
		if (reader->error != 0 ||
		    base != 0 ||
		    layers != 1)
			return EINVAL;
	}

	/* Succeeded: the record holds the values, the aspects and the rectangles. */
	return 0;
}

/* Decodes the actual client's selected clear union, including ignored entries beyond the pass's attachments. */
static int
decode_pass(
	struct bcm2711_vulkan_session *session,
	struct i915_wire_reader *reader,
	struct bcm2711_vulkan_record *record)
{
	uint64_t present;
	uint64_t chain;
	uint64_t identity;
	uint64_t array;
	uint32_t type;
	uint32_t clears;
	uint32_t branch;
	uint32_t representation;
	uint32_t contents;
	uint32_t index;
	uint32_t component;
	uint32_t bits;
	uint32_t stencil;

	/* Complete pass begin selects the exact render pass, framebuffer and signed render area. */
	present = drv_i915_wire_read_u64(reader);
	type = drv_i915_wire_read_u32(reader);
	chain = drv_i915_wire_read_u64(reader);
	if (present != 1 ||
	    type != VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO ||
	    chain != 0)
		return ENOTSUP;
	identity = drv_i915_wire_read_u64(reader);
	record->objects[0] = bcm2711_vulkan_object_find(session, I915_VK_OBJ_RENDER_PASS, identity);
	identity = drv_i915_wire_read_u64(reader);
	record->objects[1] = bcm2711_vulkan_object_find(session, I915_VK_OBJ_FRAMEBUFFER, identity);
	record->area.offset.x = (int32_t)drv_i915_wire_read_u32(reader);
	record->area.offset.y = (int32_t)drv_i915_wire_read_u32(reader);
	record->area.extent.width = drv_i915_wire_read_u32(reader);
	record->area.extent.height = drv_i915_wire_read_u32(reader);
	clears = drv_i915_wire_read_u32(reader);
	array = drv_i915_wire_read_u64(reader);
	if (clears > 16 || array != clears)
		return ENOTSUP;
	record->count = clears;

	/*
	 * Colour branch zero carries representation two and four words; the
	 * depth/stencil branch one a depth float and a stencil word.  Entry a
	 * of the pass's attachments is kept in words 4a and up; extra clear
	 * entries remain ignored but fully consumed.
	 */
	for (index = 0; index < clears; index++) {
		branch = drv_i915_wire_read_u32(reader);
		if (branch == 1) {
			bits = drv_i915_wire_read_u32(reader);
			stencil = drv_i915_wire_read_u32(reader);
			if (index < BCM2711_VULKAN_PASS_ATTACHMENTS) {
				record->words[index * 4U] = bits;
				record->words[index * 4U + 1U] = stencil;
			}
			continue;
		}

		/* Any other branch is the colour's. */
		representation = drv_i915_wire_read_u32(reader);
		array = drv_i915_wire_read_u64(reader);
		if (branch != 0 ||
		    representation != 2 ||
		    array != 4)
			return ENOTSUP;
		for (component = 0; component < 4; component++) {
			bits = drv_i915_wire_read_u32(reader);
			if (index < BCM2711_VULKAN_PASS_ATTACHMENTS)
				record->words[index * 4U + component] = bits;
		}
	}

	/* Secondary execution is unimplemented and must not become an empty successful inline pass. */
	contents = drv_i915_wire_read_u32(reader);
	if (contents != VK_SUBPASS_CONTENTS_INLINE)
		return ENOTSUP;

	/* Succeeded: the complete single-colour begin event copied its exact target, area and selected clear bits. */
	return 0;
}

/* Consumes every bounded clear range before retaining a semantic refusal for the primary's reply-bearing End. */
static int
decode_clear(
	struct bcm2711_vulkan_session *session,
	struct i915_wire_reader *reader,
	struct bcm2711_vulkan_record *record)
{
	uint64_t identity;
	uint64_t present;
	uint64_t array;
	uint32_t representation;
	uint32_t index;
	uint32_t aspect;
	uint32_t mip;
	uint32_t levels;
	uint32_t layer;
	uint32_t layers;
	struct bcm2711_vulkan_object *image;
	VkFormat format;
	bool engine_71;

	/* The actual public wrapper selects one typed image and an explicit layout without retaining a client pointer. */
	identity = drv_i915_wire_read_u64(reader);
	record->objects[0] = bcm2711_vulkan_object_find(session, I915_VK_OBJ_IMAGE, identity);
	record->layout = drv_i915_wire_read_u32(reader);
	present = drv_i915_wire_read_u64(reader);
	if (present > 1)
		return EINVAL;

	/* A present colour union carries the actual client's raw unsigned representation of four IEEE component words. */
	if (present != 0) {
		representation = drv_i915_wire_read_u32(reader);
		array = drv_i915_wire_read_u64(reader);
		if (array > 4)
			return EINVAL;

		/* Keep malformed colour semantics distinct from the complete following range framing. */
		if (representation != 2 || array != 4)
			record->semantic_error = EINVAL;
		for (index = 0; index < array; index++)
			record->words[index] = drv_i915_wire_read_u32(reader);
	} else {
		record->semantic_error = EINVAL;
	}

	/* The ranges, each a word of its own (BCM2711_VULKAN_CLEAR_RANGE_WORD). */
	record->count = drv_i915_wire_read_u32(reader);
	array = drv_i915_wire_read_u64(reader);
	if (record->count > 64U || array != record->count)
		return EINVAL;

	/* An empty clear is a complete semantic refusal, and more ranges than a record keeps are unsupported. */
	if (record->count == 0) {
		record->semantic_error = EINVAL;
	} else if (record->count > BCM2711_VULKAN_CLEAR_RANGES && record->semantic_error == 0) {
		record->semantic_error = ENOTSUP;
	}

	/* Decode every complete five-word range even when an earlier range or colour selection was refused. */
	for (index = 0; index < record->count; index++) {
		aspect = drv_i915_wire_read_u32(reader);
		mip = drv_i915_wire_read_u32(reader);
		levels = drv_i915_wire_read_u32(reader);
		layer = drv_i915_wire_read_u32(reader);
		layers = drv_i915_wire_read_u32(reader);

		/* A colour range starts at a level and a layer an image may have and names at least one of each. */
		if (aspect != VK_IMAGE_ASPECT_COLOR_BIT ||
		    mip >= BCM2711_VULKAN_IMAGE_LEVELS ||
		    layer >= BCM2711_VULKAN_IMAGE_LAYERS ||
		    levels == 0 ||
		    layers == 0 ||
		    (levels > BCM2711_VULKAN_IMAGE_LEVELS && levels != VK_REMAINING_MIP_LEVELS) ||
		    (layers > BCM2711_VULKAN_IMAGE_LAYERS && layers != VK_REMAINING_ARRAY_LAYERS)) {
			if (record->semantic_error == 0)
				record->semantic_error = EINVAL;
			continue;
		}

		/* The remaining-count sentinels are kept as zero counts, resolved against the image when the record is validated. */
		if (levels == VK_REMAINING_MIP_LEVELS)
			levels = 0;
		if (layers == VK_REMAINING_ARRAY_LAYERS)
			layers = 0;

		/* A range past the ones a record keeps was refused above. */
		if (index >= BCM2711_VULKAN_CLEAR_RANGES)
			continue;

		/* The range's word keeps its first level, level count, first layer and layer count. */
		record->words[BCM2711_VULKAN_CLEAR_RANGE_WORD + index] =
			(mip << BCM2711_VULKAN_CLEAR_LEVEL_SHIFT) |
			(levels << BCM2711_VULKAN_CLEAR_LEVELS_SHIFT) |
			(layer << BCM2711_VULKAN_CLEAR_LAYER_SHIFT) |
			(layers << BCM2711_VULKAN_CLEAR_LAYERS_SHIFT);
	}

	/* The 4.2's tile buffer clears only the two byte UNORM formats; the 7.1 every colour attachment format, which validation checks. */
	image = record->objects[0];
	if (image != NULL &&
	    image->kind == I915_VK_OBJ_IMAGE &&
	    record->semantic_error == 0) {
		engine_71 = bcm2711_vulkan_engine_71(session);
		format = ((const struct bcm2711_vulkan_resource *)image->payload)->format;
		if (!engine_71 &&
		    format != VK_FORMAT_R8G8B8A8_UNORM &&
		    format != VK_FORMAT_B8G8R8A8_UNORM)
			record->semantic_error = ENOTSUP;
	}

	/* Succeeded: the outer decoder checks framing before End can observe the complete semantic outcome. */
	return 0;
}

/*
 * Copies a depth/stencil image clear: the image, its layout, the depth (IEEE
 * bits) and stencil values to words 0 and 1, and the union of the ranges'
 * aspects to word 2.  A depth/stencil image has one level and one layer, so
 * every range names the whole image.
 */
static int
decode_depth_clear(
	struct bcm2711_vulkan_session *session,
	struct i915_wire_reader *reader,
	struct bcm2711_vulkan_record *record)
{
	uint64_t identity;
	uint64_t present;
	uint64_t array;
	uint32_t index;
	uint32_t aspect;
	uint32_t mip;
	uint32_t levels;
	uint32_t layer;
	uint32_t layers;

	/* The image, its layout and the values, never a client pointer. */
	identity = drv_i915_wire_read_u64(reader);
	record->objects[0] = bcm2711_vulkan_object_find(session, I915_VK_OBJ_IMAGE, identity);
	record->layout = drv_i915_wire_read_u32(reader);
	present = drv_i915_wire_read_u64(reader);
	if (present != 1)
		return EINVAL;
	record->words[0] = drv_i915_wire_read_u32(reader);
	record->words[1] = drv_i915_wire_read_u32(reader);

	/* The ranges, each read whole even when an earlier one was refused. */
	record->count = drv_i915_wire_read_u32(reader);
	array = drv_i915_wire_read_u64(reader);
	if (record->count > 64U || array != record->count)
		return EINVAL;
	if (record->count == 0)
		record->semantic_error = EINVAL;
	record->words[2] = 0;
	record->first = 0U;
	for (index = 0; index < record->count; index++) {
		aspect = drv_i915_wire_read_u32(reader);
		mip = drv_i915_wire_read_u32(reader);
		levels = drv_i915_wire_read_u32(reader);
		layer = drv_i915_wire_read_u32(reader);
		layers = drv_i915_wire_read_u32(reader);

		/* The ranges start at the first layer; first keeps the most layers one names (VK_REMAINING_ARRAY_LAYERS: all). */
		if (layers > record->first)
			record->first = layers;

		/* Depth and stencil aspects of the only level, from the first layer. */
		if (aspect == 0 ||
		    (aspect & ~(uint32_t)(VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)) != 0 ||
		    mip != 0 ||
		    layer != 0 ||
		    (levels != 1 && levels != VK_REMAINING_MIP_LEVELS) ||
		    layers == 0) {
			if (record->semantic_error == 0)
				record->semantic_error = EINVAL;
		}
		record->words[2] |= aspect;
	}

	/* Succeeded: the outer decoder checks framing before End can observe the semantic outcome. */
	return 0;
}
