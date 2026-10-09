/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* Count-selected recording payloads copy exact client words and resolve borrowed typed IDs without acquiring partial owners. */
#include <uapi/errno.h>

#include "drivers/gpu/bcm2711/vulkan-record.h"

static int decode_clear(struct bcm2711_vulkan_session *session, struct i915_wire_reader *reader, struct bcm2711_vulkan_record *record);
static int decode_sets(struct bcm2711_vulkan_session *session, struct i915_wire_reader *reader, struct bcm2711_vulkan_record *record);
static int decode_vertices(struct bcm2711_vulkan_session *session, struct i915_wire_reader *reader, struct bcm2711_vulkan_record *record);
static int decode_push(struct bcm2711_vulkan_session *session, struct i915_wire_reader *reader, struct bcm2711_vulkan_record *record);
static int decode_area(struct i915_wire_reader *reader, struct bcm2711_vulkan_record *record);
static int decode_pass(struct bcm2711_vulkan_session *session, struct i915_wire_reader *reader, struct bcm2711_vulkan_record *record);

/*
 * Decodes one complete admitted graphics record without retaining wire or application pointers.
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
		if (point != VK_PIPELINE_BIND_POINT_GRAPHICS)
			return ENOTSUP;
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
	case GPU_OP_CMD_BEGIN_RENDER_PASS:
		error = decode_pass(session, reader, record);
		break;
	case GPU_OP_CMD_CLEAR_COLOR_IMAGE:
		error = decode_clear(session, reader, record);
		break;
	case GPU_OP_CMD_DRAW:
		/* Draw's four words preserve exact vertex/instance counts and base selections. */
		for (index = 0; index < 4; index++)
			record->words[index] = drv_i915_wire_read_u32(reader);
		break;
	case GPU_OP_CMD_END_RENDER_PASS:
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

/* Decodes exact graphics set selections and refuses unsupported dynamic-buffer offset framing. */
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
	if (point != VK_PIPELINE_BIND_POINT_GRAPHICS ||
	    record->first > BCM2711_VULKAN_PIPELINE_SETS ||
	    record->count > BCM2711_VULKAN_PIPELINE_SETS - record->first ||
	    array != record->count)
		return ENOTSUP;
	record->objects[0] = bcm2711_vulkan_object_find(session, I915_VK_OBJ_PIPELINE_LAYOUT, identity);

	/* Each referenced set has its own eventual retained recording edge. */
	for (index = 0; index < record->count; index++) {
		identity = drv_i915_wire_read_u64(reader);
		record->objects[index + 1U] = bcm2711_vulkan_object_find(session, I915_VK_OBJ_DESCRIPTOR_SET, identity);
	}

	/* The admitted layouts contain no dynamic descriptors, so their standard dynamic array must be empty. */
	dynamic = drv_i915_wire_read_u32(reader);
	array = drv_i915_wire_read_u64(reader);
	if (dynamic != 0 || array != 0)
		return ENOTSUP;

	/* Succeeded: complete set binding framing contains no unsupported offset semantics. */
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

	/* The finite scalar compiler interface admits only vertex/fragment stage words within 128 bytes. */
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

/* Decodes the actual client's selected clear union, including ignored entries beyond the single colour attachment. */
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

	/* Colour branch zero carries representation two and four words; extra clear entries remain ignored but fully consumed. */
	for (index = 0; index < clears; index++) {
		branch = drv_i915_wire_read_u32(reader);
		representation = drv_i915_wire_read_u32(reader);
		array = drv_i915_wire_read_u64(reader);
		if (branch != 0 ||
		    representation != 2 ||
		    array != 4)
			return ENOTSUP;
		for (component = 0; component < 4; component++) {
			bits = drv_i915_wire_read_u32(reader);
			if (index == 0)
				record->words[component] = bits;
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

	/* All implemented images have exactly one mip and layer, so repeated complete ranges coalesce into one native tile clear. */
	record->count = drv_i915_wire_read_u32(reader);
	array = drv_i915_wire_read_u64(reader);
	if (record->count > 64U || array != record->count)
		return EINVAL;

	/* An empty clear is a complete semantic refusal, while a framing mismatch cannot be skipped. */
	if (record->count == 0)
		record->semantic_error = EINVAL;
	for (index = 0; index < record->count; index++) {
		/* Decode the complete five-word range even when an earlier range or colour selection was refused. */
		aspect = drv_i915_wire_read_u32(reader);
		mip = drv_i915_wire_read_u32(reader);
		levels = drv_i915_wire_read_u32(reader);
		layer = drv_i915_wire_read_u32(reader);
		layers = drv_i915_wire_read_u32(reader);
		if (aspect != VK_IMAGE_ASPECT_COLOR_BIT ||
		    mip != 0 ||
		    layer != 0) {
			if (record->semantic_error == 0)
				record->semantic_error = EINVAL;
		}

		/* Standard remaining-count sentinels select the same sole implemented subresource. */
		if ((levels != 1 &&
		     levels != VK_REMAINING_MIP_LEVELS) ||
		    (layers != 1 &&
		     layers != VK_REMAINING_ARRAY_LAYERS)) {
			if (record->semantic_error == 0)
				record->semantic_error = EINVAL;
		}
	}

	/* Succeeded: the outer decoder checks framing before End can observe the complete semantic outcome. */
	return 0;
}
