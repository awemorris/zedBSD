/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The executor's recorded commands: each vkCmd* libvulkan records arrives as
 * [opcode][0][command buffer] and its arguments, with no reply, and is kept as
 * a record of its command buffer until a submission runs it (vk-execute.c).
 *
 * Every record is read to its end, holding the objects it names: a record
 * of a command buffer that is recording straight into its place at the
 * end of the command buffer's records (and kept there), another one aside
 * and let go.
 * A record the executor cannot run (another bind point, an unknown object,
 * more regions than a record keeps) marks its command buffer failed, and
 * the End reports it.  An opcode the executor does not know cannot be
 * skipped (commands carry no length): it fails the frame.
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

/* The records a command buffer's array starts with; it doubles as it fills. */
#define RECORD_FIRST_CAPACITY		64U

/* The most records one command buffer keeps. */
#define RECORD_MAX			65536U

/* The words of a clear colour's union on the wire: its tag, then four words. */
#define RECORD_COLOUR_TAG		2U
#define RECORD_COLOUR_WORDS		4U

/*
 * The most regions one copy, blit or resolve takes, and the most its
 * regions of several slices become, one a slice (the executor runs them
 * AMDRDNA_VK_REGIONS_MAX at a time).
 */
#define RECORD_COPY_REGIONS_MAX		4096U
#define RECORD_SLICE_REGIONS_MAX	4096U

static int record_decode(struct amdrdna_vk_session *session, uint32_t opcode, struct i915_wire_reader *reader, struct amdrdna_vk_record *record, uint8_t *push);
static int record_hold(struct amdrdna_vk_session *session, struct amdrdna_vk_record *record, enum i915_vk_object_kind kind, uint64_t identity);
static int record_pipeline(struct amdrdna_vk_session *session, struct i915_wire_reader *reader, struct amdrdna_vk_record *record);
static int record_viewport(struct amdrdna_vk_session *session, struct i915_wire_reader *reader, struct amdrdna_vk_record *record);
static int record_scissor(struct amdrdna_vk_session *session, struct i915_wire_reader *reader, struct amdrdna_vk_record *record);
static int record_sets(struct amdrdna_vk_session *session, struct i915_wire_reader *reader, struct amdrdna_vk_record *record);
static int record_vertices(struct amdrdna_vk_session *session, struct i915_wire_reader *reader, struct amdrdna_vk_record *record);
static int record_indices(struct amdrdna_vk_session *session, struct i915_wire_reader *reader, struct amdrdna_vk_record *record);
static int record_draw(struct i915_wire_reader *reader, struct amdrdna_vk_record *record);
static int record_draw_indexed(struct i915_wire_reader *reader, struct amdrdna_vk_record *record);
static int record_dispatch(struct i915_wire_reader *reader, struct amdrdna_vk_record *record);
static int record_copy(struct amdrdna_vk_session *session, uint32_t opcode, struct i915_wire_reader *reader, struct amdrdna_vk_record *record);
static int record_clear(struct amdrdna_vk_session *session, uint32_t opcode, struct i915_wire_reader *reader, struct amdrdna_vk_record *record);
static int record_query_reset(struct amdrdna_vk_session *session, struct i915_wire_reader *reader, struct amdrdna_vk_record *record);
static int record_timestamp(struct amdrdna_vk_session *session, struct i915_wire_reader *reader, struct amdrdna_vk_record *record);
static int record_colour(struct i915_wire_reader *reader, uint32_t *words);
static int record_barrier(struct amdrdna_vk_session *session, struct i915_wire_reader *reader);
static int record_push(struct i915_wire_reader *reader, struct amdrdna_vk_record *record, uint8_t *bytes);
static int record_depth_copy(uint32_t opcode, const struct amdrdna_vk_record *record);
static int record_depth_aspects(const struct amdrdna_vk_image *image, uint32_t aspects, bool one);
static int record_copy_slices(uint32_t opcode, struct amdrdna_vk_record *record, size_t element);
static int record_region_slices(uint32_t opcode, const struct amdrdna_vk_image *source, const struct amdrdna_vk_image *destination, const void *region, uint32_t *slices);
static uint32_t record_layers(const struct amdrdna_vk_image *image, const VkImageSubresourceLayers *layers);
static void record_region_slice(uint32_t opcode, const struct amdrdna_vk_image *image, void *region, uint32_t slice);
static int record_begin_pass(struct amdrdna_vk_session *session, struct i915_wire_reader *reader, struct amdrdna_vk_record *record);
static int record_line_width(struct i915_wire_reader *reader);
static int record_depth_bias(struct i915_wire_reader *reader, struct amdrdna_vk_record *record);
static int record_blend_constants(struct i915_wire_reader *reader, struct amdrdna_vk_record *record);
static int record_stencil(struct i915_wire_reader *reader, struct amdrdna_vk_record *record);
static int record_dynamic_offsets(struct i915_wire_reader *reader, struct amdrdna_vk_record *record);
static int record_clear_attachments(struct amdrdna_vk_session *session, struct i915_wire_reader *reader, struct amdrdna_vk_record *record);
static int record_clear_attachment(struct i915_wire_reader *reader, struct amdrdna_vk_record *record);
static int record_room(struct amdrdna_vk_command_buffer *command, uint32_t opcode);
static void record_drop(struct amdrdna_vk_record *record);

/*
 * Records one vkCmd* command; *handled stays clear for an opcode of
 * another handler's.
 */
int
amdrdna_vk_record_route(
	struct amdrdna_vk_session *session,
	uint32_t opcode,
	uint32_t requested,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply,
	int *handled)
{
	struct amdrdna_vk_command_buffer *command;
	struct amdrdna_vk_record *record;
	struct amdrdna_vk_record aside;
	uint8_t push[AMDRDNA_VK_PUSH_BYTES];
	uint8_t *bytes;
	uint64_t identity;
	int refused;
	int room;

	UNUSED_PARAMETER(requested);
	UNUSED_PARAMETER(reply);

	/* Only the recorded commands. */
	*handled = 0;
	switch (opcode) {
	case GPU_OP_CMD_BIND_PIPELINE:
	case GPU_OP_CMD_SET_VIEWPORT:
	case GPU_OP_CMD_SET_SCISSOR:
	case GPU_OP_CMD_SET_LINE_WIDTH:
	case GPU_OP_CMD_SET_DEPTH_BIAS:
	case GPU_OP_CMD_SET_BLEND_CONSTANTS:
	case GPU_OP_CMD_SET_STENCIL_COMPARE_MASK:
	case GPU_OP_CMD_SET_STENCIL_WRITE_MASK:
	case GPU_OP_CMD_SET_STENCIL_REFERENCE:
	case GPU_OP_CMD_BIND_DESCRIPTOR_SETS:
	case GPU_OP_CMD_BIND_VERTEX_BUFFERS:
	case GPU_OP_CMD_BIND_INDEX_BUFFER:
	case GPU_OP_CMD_DRAW:
	case GPU_OP_CMD_DRAW_INDEXED:
	case GPU_OP_CMD_DISPATCH:
	case GPU_OP_CMD_COPY_BUFFER:
	case GPU_OP_CMD_COPY_IMAGE:
	case GPU_OP_CMD_BLIT_IMAGE:
	case GPU_OP_CMD_COPY_BUFFER_TO_IMAGE:
	case GPU_OP_CMD_COPY_IMAGE_TO_BUFFER:
	case GPU_OP_CMD_RESOLVE_IMAGE:
	case GPU_OP_CMD_CLEAR_COLOR_IMAGE:
	case GPU_OP_CMD_CLEAR_DEPTH_STENCIL_IMAGE:
	case GPU_OP_CMD_CLEAR_ATTACHMENTS:
	case GPU_OP_CMD_PIPELINE_BARRIER:
	case GPU_OP_CMD_PUSH_CONSTANTS:
	case GPU_OP_CMD_BEGIN_RENDER_PASS:
	case GPU_OP_CMD_END_RENDER_PASS:
	case GPU_OP_CMD_RESET_QUERY_POOL:
	case GPU_OP_CMD_WRITE_TIMESTAMP:
		break;
	default:
		/* The video coding and query commands are recorded too (vk-video.inc decodes them). */
		if (!amdrdna_vk_video_recorded(opcode))
			return 0;
		break;
	}

	*handled = 1;

	/* The command buffer the record is for. */
	identity = drv_i915_wire_read_u64(reader);
	if (reader->error != 0)
		return EINVAL;

	/* A command buffer that is recording keeps the record; another one's records are read and let go. */
	command = amdrdna_vk_payload(session, I915_VK_OBJ_COMMAND_BUFFER, identity);
	if (command != NULL && command->state != AMDRDNA_VK_COMMAND_RECORDING)
		command = NULL;

	/*
	 * A kept record is read straight into its place at the end of the
	 * command buffer's array, a push constant record's bytes into theirs:
	 * nothing is copied after it is read.  Without room it is read aside.
	 */
	record = &aside;
	bytes = push;
	room = 0;
	if (command != NULL && opcode != GPU_OP_CMD_SET_LINE_WIDTH && opcode != GPU_OP_CMD_PIPELINE_BARRIER) {
		room = record_room(command, opcode);
		if (room == 0) {
			record = &command->records[command->record_count];
			if (opcode == GPU_OP_CMD_PUSH_CONSTANTS)
				bytes = command->push_bytes + command->push_used;
		}
	}

	/* Reads the record to its end; a record that does not decode leaves the stream unreadable. */
	kern_memset(record, 0, sizeof(*record));
	record->opcode = opcode;
	if (amdrdna_vk_video_recorded(opcode)) {
		refused = amdrdna_vk_video_record(session, command, opcode, reader, record);
	} else {
		refused = record_decode(session, opcode, reader, record, bytes);
	}
	if (refused == EINVAL || reader->error != 0) {
		record_drop(record);
		return EINVAL;
	}

	/* A record of no recording command buffer is let go. */
	if (command == NULL) {
		record_drop(record);
		return 0;
	}

	/* A refused record fails the recording. */
	if (refused != 0) {
		kern_logf("amdrdna: vk: a recorded command %u is not run (%d)\n", (unsigned)opcode, refused);
		record_drop(record);
		if (command->error == 0)
			command->error = refused;
		return 0;
	}

	/* Commands that change nothing a submission runs are not kept. */
	if (opcode == GPU_OP_CMD_SET_LINE_WIDTH || opcode == GPU_OP_CMD_PIPELINE_BARRIER)
		return 0;

	/* A record that had no room fails the recording. */
	if (room != 0) {
		record_drop(record);
		if (command->error == 0)
			command->error = room;
		return 0;
	}

	/* The record, read in its place, is the command buffer's now; a push constant record's bytes are too. */
	if (opcode == GPU_OP_CMD_PUSH_CONSTANTS) {
		record->push_at = command->push_used;
		command->push_used += record->count;
	}
	command->record_count++;

	/* Succeeded: the command is recorded, or its command buffer remembers why not. */
	return 0;
}

/*
 * Decodes one record's arguments (a push constant record's bytes into
 * push, AMDRDNA_VK_PUSH_BYTES long).  Returns EINVAL when they did not
 * decode (the stream is then unreadable), another error when they decoded
 * but cannot be run.
 */
static int
record_decode(
	struct amdrdna_vk_session *session,
	uint32_t opcode,
	struct i915_wire_reader *reader,
	struct amdrdna_vk_record *record,
	uint8_t *push)
{
	int error;

	/* The arguments of each command. */
	switch (opcode) {
	case GPU_OP_CMD_BIND_PIPELINE:
		error = record_pipeline(session, reader, record);
		break;
	case GPU_OP_CMD_SET_VIEWPORT:
		error = record_viewport(session, reader, record);
		break;
	case GPU_OP_CMD_SET_SCISSOR:
		error = record_scissor(session, reader, record);
		break;
	case GPU_OP_CMD_SET_LINE_WIDTH:
		error = record_line_width(reader);
		break;
	case GPU_OP_CMD_SET_DEPTH_BIAS:
		error = record_depth_bias(reader, record);
		break;
	case GPU_OP_CMD_SET_BLEND_CONSTANTS:
		error = record_blend_constants(reader, record);
		break;
	case GPU_OP_CMD_SET_STENCIL_COMPARE_MASK:
	case GPU_OP_CMD_SET_STENCIL_WRITE_MASK:
	case GPU_OP_CMD_SET_STENCIL_REFERENCE:
		error = record_stencil(reader, record);
		break;
	case GPU_OP_CMD_BIND_DESCRIPTOR_SETS:
		error = record_sets(session, reader, record);
		break;
	case GPU_OP_CMD_BIND_VERTEX_BUFFERS:
		error = record_vertices(session, reader, record);
		break;
	case GPU_OP_CMD_BIND_INDEX_BUFFER:
		error = record_indices(session, reader, record);
		break;
	case GPU_OP_CMD_DRAW:
		error = record_draw(reader, record);
		break;
	case GPU_OP_CMD_DRAW_INDEXED:
		error = record_draw_indexed(reader, record);
		break;
	case GPU_OP_CMD_DISPATCH:
		error = record_dispatch(reader, record);
		break;
	case GPU_OP_CMD_COPY_BUFFER:
	case GPU_OP_CMD_COPY_IMAGE:
	case GPU_OP_CMD_BLIT_IMAGE:
	case GPU_OP_CMD_COPY_BUFFER_TO_IMAGE:
	case GPU_OP_CMD_COPY_IMAGE_TO_BUFFER:
	case GPU_OP_CMD_RESOLVE_IMAGE:
		error = record_copy(session, opcode, reader, record);
		break;
	case GPU_OP_CMD_CLEAR_COLOR_IMAGE:
	case GPU_OP_CMD_CLEAR_DEPTH_STENCIL_IMAGE:
		error = record_clear(session, opcode, reader, record);
		break;
	case GPU_OP_CMD_CLEAR_ATTACHMENTS:
		error = record_clear_attachments(session, reader, record);
		break;
	case GPU_OP_CMD_PIPELINE_BARRIER:
		error = record_barrier(session, reader);
		break;
	case GPU_OP_CMD_PUSH_CONSTANTS:
		error = record_push(reader, record, push);
		break;
	case GPU_OP_CMD_BEGIN_RENDER_PASS:
		error = record_begin_pass(session, reader, record);
		break;
	case GPU_OP_CMD_RESET_QUERY_POOL:
		error = record_query_reset(session, reader, record);
		break;
	case GPU_OP_CMD_WRITE_TIMESTAMP:
		error = record_timestamp(session, reader, record);
		break;
	default:
		error = 0;
		break;
	}

	/* Reports a record that did not decode or cannot be run. */
	if (error != 0)
		return error;

	/* Succeeded: the record's arguments are read. */
	return 0;
}

/*
 * Holds an object of a kind for a record; a null identity holds nothing.
 * Returns ENOENT for an identity the session does not have.
 */
static int
record_hold(
	struct amdrdna_vk_session *session,
	struct amdrdna_vk_record *record,
	enum i915_vk_object_kind kind,
	uint64_t identity)
{
	struct amdrdna_vk_object *object;
	int error;

	/* A null handle names nothing. */
	if (identity == 0U)
		return 0;

	/* A known object, while the record has room for it. */
	object = amdrdna_vk_object_find(session, kind, identity);
	if (object == NULL)
		return ENOENT;
	if (record->object_count >= AMDRDNA_VK_SETS_MAX)
		return ENOTSUP;

	/* The record holds it. */
	error = amdrdna_vk_object_retain(object);
	if (error != 0)
		return error;
	record->objects[record->object_count] = object;
	record->object_count++;

	/* Succeeded: the record holds the object. */
	return 0;
}

/*
 * vkCmdBindPipeline: [bind point][pipeline]; a graphics pipeline at the
 * graphics bind point or a compute pipeline at the compute one (the bind
 * point kept in value 0).
 */
static int
record_pipeline(
	struct amdrdna_vk_session *session,
	struct i915_wire_reader *reader,
	struct amdrdna_vk_record *record)
{
	const struct amdrdna_vk_pipeline *bound;
	uint32_t point;
	uint64_t pipeline;
	uint32_t compute;
	int error;

	/* The bind point and the pipeline. */
	point = drv_i915_wire_read_u32(reader);
	pipeline = drv_i915_wire_read_u64(reader);
	if (reader->error != 0)
		return EINVAL;

	/* The graphics and the compute bind points are run. */
	if (point != VK_PIPELINE_BIND_POINT_GRAPHICS && point != VK_PIPELINE_BIND_POINT_COMPUTE)
		return ENOTSUP;
	record->values[0] = point;

	/* The record holds a known pipeline. */
	error = record_hold(session, record, I915_VK_OBJ_PIPELINE, pipeline);
	if (error != 0)
		return error;
	if (record->object_count == 0U)
		return ENOENT;

	/* A pipeline of the bind point's kind. */
	bound = record->objects[0]->payload;
	compute = (point == VK_PIPELINE_BIND_POINT_COMPUTE) ? 1U : 0U;
	if (bound->compute != compute)
		return ENOTSUP;

	/* Succeeded: the pipeline is recorded. */
	return 0;
}

/* vkCmdSetViewport: [first][count][count]{VkViewport}; the first viewport, as float bits. */
static int
record_viewport(
	struct amdrdna_vk_session *session,
	struct i915_wire_reader *reader,
	struct amdrdna_vk_record *record)
{
	VkViewport viewport;
	uint64_t count;
	uint64_t index;
	uint32_t first;

	/* The first index and the viewports. */
	first = drv_i915_wire_read_u32(reader);
	(void)drv_i915_wire_read_u32(reader);
	count = drv_i915_wire_read_u64(reader);
	if (reader->error != 0 || count > AMDRDNA_VK_REGIONS_MAX)
		return EINVAL;
	for (index = 0U; index < count; index++) {
		kern_memset(&viewport, 0, sizeof(viewport));
		i915_vkc_dec_VkViewport(reader, &session->arena, &viewport);
		if (index != 0U)
			continue;

		/* x, y, width, height, min depth and max depth. */
		kern_memcpy(&record->values[0], &viewport.x, 4U);
		kern_memcpy(&record->values[1], &viewport.y, 4U);
		kern_memcpy(&record->values[2], &viewport.width, 4U);
		kern_memcpy(&record->values[3], &viewport.height, 4U);
		kern_memcpy(&record->values[4], &viewport.minDepth, 4U);
		kern_memcpy(&record->values[5], &viewport.maxDepth, 4U);
	}
	if (reader->error != 0)
		return EINVAL;

	/* Only viewport 0 is drawn to. */
	if (first != 0U || count == 0U)
		return ENOTSUP;

	/* Succeeded: the viewport is recorded. */
	return 0;
}

/* vkCmdSetScissor: [first][count][count]{VkRect2D}; the first rectangle. */
static int
record_scissor(
	struct amdrdna_vk_session *session,
	struct i915_wire_reader *reader,
	struct amdrdna_vk_record *record)
{
	VkRect2D rect;
	uint64_t count;
	uint64_t index;
	uint32_t first;

	/* The first index and the rectangles. */
	first = drv_i915_wire_read_u32(reader);
	(void)drv_i915_wire_read_u32(reader);
	count = drv_i915_wire_read_u64(reader);
	if (reader->error != 0 || count > AMDRDNA_VK_REGIONS_MAX)
		return EINVAL;
	for (index = 0U; index < count; index++) {
		kern_memset(&rect, 0, sizeof(rect));
		i915_vkc_dec_VkRect2D(reader, &session->arena, &rect);
		if (index == 0U)
			record->rect = rect;
	}
	if (reader->error != 0)
		return EINVAL;

	/* Only scissor 0 is drawn with. */
	if (first != 0U || count == 0U)
		return ENOTSUP;

	/* Succeeded: the scissor is recorded. */
	return 0;
}

/* vkCmdSetLineWidth: [width]; nothing is drawn with lines. */
static int
record_line_width(
	struct i915_wire_reader *reader)
{
	/* The width. */
	(void)drv_i915_wire_read_u32(reader);
	if (reader->error != 0)
		return EINVAL;

	/* Succeeded: the width is read. */
	return 0;
}

/* vkCmdSetDepthBias: [constant factor][clamp][slope factor]; the three floats' bits as values 0 to 2. */
static int
record_depth_bias(
	struct i915_wire_reader *reader,
	struct amdrdna_vk_record *record)
{
	/* The three factors. */
	record->values[0] = drv_i915_wire_read_u32(reader);
	record->values[1] = drv_i915_wire_read_u32(reader);
	record->values[2] = drv_i915_wire_read_u32(reader);
	if (reader->error != 0)
		return EINVAL;

	/* Succeeded: the depth bias is recorded. */
	return 0;
}

/* vkCmdSetBlendConstants: [4][4 x float]; the four floats' bits as values 0 to 3. */
static int
record_blend_constants(
	struct i915_wire_reader *reader,
	struct amdrdna_vk_record *record)
{
	uint64_t count;
	uint32_t index;

	/* The count, always four. */
	count = drv_i915_wire_read_u64(reader);
	if (reader->error != 0 || count != 4U)
		return EINVAL;

	/* Red, green, blue and alpha. */
	for (index = 0U; index < 4U; index++)
		record->values[index] = drv_i915_wire_read_u32(reader);
	if (reader->error != 0)
		return EINVAL;

	/* Succeeded: the blend constants are recorded. */
	return 0;
}

/*
 * vkCmdSetStencilCompareMask, vkCmdSetStencilWriteMask and
 * vkCmdSetStencilReference: [face mask][value]; the faces
 * (VkStencilFaceFlags) as value 0, the value as value 1.
 */
static int
record_stencil(
	struct i915_wire_reader *reader,
	struct amdrdna_vk_record *record)
{
	/* The faces and the value. */
	record->values[0] = drv_i915_wire_read_u32(reader);
	record->values[1] = drv_i915_wire_read_u32(reader);
	if (reader->error != 0)
		return EINVAL;

	/* Succeeded: the stencil value is recorded. */
	return 0;
}

/*
 * vkCmdBindDescriptorSets: [bind point][layout][first set][count][count]
 * {set}[dynamic count][count]{offset}; graphics or compute sets (the bind
 * point kept in value 0), held, and the dynamic offsets (value 1 their
 * count): one for each dynamic descriptor of the sets, in the order of the
 * sets and their binding numbers.
 */
static int
record_sets(
	struct amdrdna_vk_session *session,
	struct i915_wire_reader *reader,
	struct amdrdna_vk_record *record)
{
	uint64_t count;
	uint64_t index;
	uint64_t identity;
	uint32_t point;
	int refused;
	int error;

	/* The bind point, the layout, the first set and how many sets follow. */
	point = drv_i915_wire_read_u32(reader);
	(void)drv_i915_wire_read_u64(reader);
	record->first = drv_i915_wire_read_u32(reader);
	(void)drv_i915_wire_read_u32(reader);
	count = drv_i915_wire_read_u64(reader);
	if (reader->error != 0 || count > AMDRDNA_VK_REGIONS_MAX)
		return EINVAL;

	/* Each set, held while the bind's sets fit a pipeline layout. */
	refused = 0;
	if (point != VK_PIPELINE_BIND_POINT_GRAPHICS && point != VK_PIPELINE_BIND_POINT_COMPUTE)
		refused = ENOTSUP;
	record->values[0] = point;
	if (record->first >= AMDRDNA_VK_SETS_MAX || count > AMDRDNA_VK_SETS_MAX - record->first)
		refused = ENOTSUP;
	for (index = 0U; index < count; index++) {
		identity = drv_i915_wire_read_u64(reader);
		if (refused != 0)
			continue;

		/* Object i is set first + i; a null set holds nothing and leaves its slot as it was. */
		record->object_count = (uint32_t)index;
		error = record_hold(session, record, I915_VK_OBJ_DESCRIPTOR_SET, identity);
		if (error != 0)
			refused = error;
		record->object_count = (uint32_t)index + 1U;
	}

	/* The dynamic offsets, as many as the sets' dynamic descriptors. */
	record->count = (uint32_t)count;
	error = record_dynamic_offsets(reader, record);
	if (error == EINVAL)
		return EINVAL;
	if (error != 0 && refused == 0)
		refused = error;

	/* Reports a bind that cannot be run. */
	if (refused != 0)
		return refused;

	/* Succeeded: the sets are recorded. */
	return 0;
}

/*
 * vkCmdBindVertexBuffers: [first][count][count]{buffer}[count]{offset};
 * the buffers held, their offsets kept.
 */
static int
record_vertices(
	struct amdrdna_vk_session *session,
	struct i915_wire_reader *reader,
	struct amdrdna_vk_record *record)
{
	uint64_t count;
	uint64_t offsets;
	uint64_t index;
	uint64_t identity;
	uint64_t offset;
	int refused;
	int error;

	/* The first binding and the buffers. */
	record->first = drv_i915_wire_read_u32(reader);
	(void)drv_i915_wire_read_u32(reader);
	count = drv_i915_wire_read_u64(reader);
	if (reader->error != 0 || count > AMDRDNA_VK_REGIONS_MAX)
		return EINVAL;
	refused = 0;
	if (record->first >= AMDRDNA_VK_VERTEX_BINDINGS_MAX || count > AMDRDNA_VK_VERTEX_BINDINGS_MAX - record->first)
		refused = ENOTSUP;
	for (index = 0U; index < count; index++) {
		identity = drv_i915_wire_read_u64(reader);
		if (refused != 0)
			continue;
		error = record_hold(session, record, I915_VK_OBJ_BUFFER, identity);
		if (error != 0)
			refused = error;
		else if (identity == 0U)
			refused = ENOTSUP;
	}

	/* Their offsets. */
	offsets = drv_i915_wire_read_u64(reader);
	if (reader->error != 0 || offsets > AMDRDNA_VK_REGIONS_MAX)
		return EINVAL;
	for (index = 0U; index < offsets; index++) {
		offset = drv_i915_wire_read_u64(reader);
		if (index < AMDRDNA_VK_VERTEX_BINDINGS_MAX)
			record->offsets[index] = offset;
	}
	if (reader->error != 0)
		return EINVAL;
	if (offsets != count && refused == 0)
		refused = EINVAL;

	/* Reports a bind that cannot be run. */
	record->count = (uint32_t)count;
	if (refused != 0)
		return refused;

	/* Succeeded: the buffers are recorded. */
	return 0;
}

/*
 * vkCmdBindIndexBuffer: [buffer][offset][index type]; the buffer held, its
 * offset kept as offset 0 and the type (16 or 32 bits) as value 0.
 */
static int
record_indices(
	struct amdrdna_vk_session *session,
	struct i915_wire_reader *reader,
	struct amdrdna_vk_record *record)
{
	uint64_t identity;
	int error;

	/* The buffer, the offset and the type. */
	identity = drv_i915_wire_read_u64(reader);
	record->offsets[0] = drv_i915_wire_read_u64(reader);
	record->values[0] = drv_i915_wire_read_u32(reader);
	if (reader->error != 0)
		return EINVAL;

	/* A type of 16 or 32 bits. */
	if (record->values[0] != VK_INDEX_TYPE_UINT16 && record->values[0] != VK_INDEX_TYPE_UINT32)
		return ENOTSUP;

	/* The record holds the buffer. */
	error = record_hold(session, record, I915_VK_OBJ_BUFFER, identity);
	if (error != 0)
		return error;
	if (record->object_count != 1U)
		return ENOENT;

	/* Succeeded: the index buffer is recorded. */
	return 0;
}

/* vkCmdDraw: [vertex count][instance count][first vertex][first instance]. */
static int
record_draw(
	struct i915_wire_reader *reader,
	struct amdrdna_vk_record *record)
{
	/* The four counts. */
	record->values[0] = drv_i915_wire_read_u32(reader);
	record->values[1] = drv_i915_wire_read_u32(reader);
	record->values[2] = drv_i915_wire_read_u32(reader);
	record->values[3] = drv_i915_wire_read_u32(reader);
	if (reader->error != 0)
		return EINVAL;

	/* Succeeded: the draw is recorded. */
	return 0;
}

/* vkCmdDrawIndexed: [index count][instance count][first index][vertex offset][first instance]. */
static int
record_draw_indexed(
	struct i915_wire_reader *reader,
	struct amdrdna_vk_record *record)
{
	/* The five counts. */
	record->values[0] = drv_i915_wire_read_u32(reader);
	record->values[1] = drv_i915_wire_read_u32(reader);
	record->values[2] = drv_i915_wire_read_u32(reader);
	record->values[3] = drv_i915_wire_read_u32(reader);
	record->values[4] = drv_i915_wire_read_u32(reader);
	if (reader->error != 0)
		return EINVAL;

	/* Succeeded: the draw is recorded. */
	return 0;
}

/* vkCmdDispatch: [x][y][z] workgroups. */
static int
record_dispatch(
	struct i915_wire_reader *reader,
	struct amdrdna_vk_record *record)
{
	/* The three counts. */
	record->values[0] = drv_i915_wire_read_u32(reader);
	record->values[1] = drv_i915_wire_read_u32(reader);
	record->values[2] = drv_i915_wire_read_u32(reader);
	if (reader->error != 0)
		return EINVAL;

	/* Succeeded: the dispatch is recorded. */
	return 0;
}

/*
 * vkCmdCopyImage, vkCmdBlitImage, vkCmdResolveImage: [source][layout]
 * [destination][layout][count][count]{VkImageCopy, VkImageBlit or
 * VkImageResolve} (and [filter] after a blit);
 * vkCmdCopyBufferToImage: [buffer][image][layout][count][count]
 * {VkBufferImageCopy}; vkCmdCopyImageToBuffer: [image][layout][buffer]
 * [count][count]{VkBufferImageCopy}.  The source is the record's object
 * 0, the destination its object 1; the regions are copied into a run of
 * the record's own (up to RECORD_COPY_REGIONS_MAX; more are refused,
 * ENOTSUP).  An image of samples is only resolved (into a colour image of
 * one sample of its format, four bytes a texel), never copied.
 * A depth image is copied and blitted as record_depth_copy says.
 */
static int
record_copy(
	struct amdrdna_vk_session *session,
	uint32_t opcode,
	struct i915_wire_reader *reader,
	struct amdrdna_vk_record *record)
{
	const struct amdrdna_vk_image *image;
	const struct amdrdna_vk_image *source_image;
	enum i915_vk_object_kind source_kind;
	enum i915_vk_object_kind destination_kind;
	uint64_t source;
	uint64_t destination;
	uint64_t count;
	uint64_t index;
	size_t element;
	uint8_t *regions;
	union {
		VkBufferCopy buffer;
		VkImageCopy image;
		VkImageResolve resolve;
		VkImageBlit blit;
		VkBufferImageCopy buffer_image;
	} region;
	bool moved;
	int refused;
	int error;

	/* The two resources and their kinds, as each command lays them out. */
	source_kind = I915_VK_OBJ_IMAGE;
	destination_kind = I915_VK_OBJ_IMAGE;
	element = sizeof(VkImageCopy);
	if (opcode == GPU_OP_CMD_COPY_BUFFER) {
		source_kind = I915_VK_OBJ_BUFFER;
		destination_kind = I915_VK_OBJ_BUFFER;
		source = drv_i915_wire_read_u64(reader);
		destination = drv_i915_wire_read_u64(reader);
		element = sizeof(VkBufferCopy);
	} else if (opcode == GPU_OP_CMD_COPY_BUFFER_TO_IMAGE) {
		source_kind = I915_VK_OBJ_BUFFER;
		source = drv_i915_wire_read_u64(reader);
		destination = drv_i915_wire_read_u64(reader);
		(void)drv_i915_wire_read_u32(reader);
		element = sizeof(VkBufferImageCopy);
	} else if (opcode == GPU_OP_CMD_COPY_IMAGE_TO_BUFFER) {
		destination_kind = I915_VK_OBJ_BUFFER;
		source = drv_i915_wire_read_u64(reader);
		(void)drv_i915_wire_read_u32(reader);
		destination = drv_i915_wire_read_u64(reader);
		element = sizeof(VkBufferImageCopy);
	} else {
		source = drv_i915_wire_read_u64(reader);
		(void)drv_i915_wire_read_u32(reader);
		destination = drv_i915_wire_read_u64(reader);
		(void)drv_i915_wire_read_u32(reader);
		if (opcode == GPU_OP_CMD_BLIT_IMAGE)
			element = sizeof(VkImageBlit);
		if (opcode == GPU_OP_CMD_RESOLVE_IMAGE)
			element = sizeof(VkImageResolve);
	}

	/* How many regions follow: as many as one copy takes (a cube's levels of its six faces, say). */
	(void)drv_i915_wire_read_u32(reader);
	count = drv_i915_wire_read_u64(reader);
	if (reader->error != 0 || count > reader->size)
		return EINVAL;

	/* The regions, into the record's own run; more than a copy takes are read past and refused. */
	regions = NULL;
	if (count != 0U && count <= RECORD_COPY_REGIONS_MAX) {
		regions = kern_calloc((size_t)count, element);
		if (regions == NULL)
			return ENOMEM;
		record->regions = regions;
		record->region_count = (uint32_t)count;
	}
	for (index = 0U; index < count && reader->error == 0; index++) {
		kern_memset(&region, 0, sizeof(region));
		if (opcode == GPU_OP_CMD_COPY_BUFFER) {
			i915_vkc_dec_VkBufferCopy(reader, &session->arena, &region.buffer);
		} else if (opcode == GPU_OP_CMD_COPY_IMAGE) {
			i915_vkc_dec_VkImageCopy(reader, &session->arena, &region.image);
		} else if (opcode == GPU_OP_CMD_RESOLVE_IMAGE) {
			i915_vkc_dec_VkImageResolve(reader, &session->arena, &region.resolve);
		} else if (opcode == GPU_OP_CMD_BLIT_IMAGE) {
			i915_vkc_dec_VkImageBlit(reader, &session->arena, &region.blit);
		} else {
			i915_vkc_dec_VkBufferImageCopy(reader, &session->arena, &region.buffer_image);
		}
		if (regions != NULL)
			kern_memcpy(regions + index * element, &region, element);
	}

	/* A blit's filter. */
	if (opcode == GPU_OP_CMD_BLIT_IMAGE)
		record->values[0] = drv_i915_wire_read_u32(reader);
	if (reader->error != 0)
		return EINVAL;
	if (count > RECORD_COPY_REGIONS_MAX)
		return ENOTSUP;

	/* The record holds both resources, which must be known. */
	refused = 0;
	error = record_hold(session, record, source_kind, source);
	if (error == 0)
		error = record_hold(session, record, destination_kind, destination);
	if (error == 0 && record->object_count != 2U)
		error = ENOENT;
	if (error != 0)
		refused = error;

	/* A depth image's copies and blits are drawn, between its aspects and a buffer or a depth image of its format. */
	if (refused == 0 && opcode != GPU_OP_CMD_COPY_BUFFER)
		refused = record_depth_copy(opcode, record);

	/*
	 * An image of samples is only resolved, from it into a four-byte
	 * colour image of one sample of the same format.
	 */
	if (refused == 0 && source_kind == I915_VK_OBJ_IMAGE) {
		image = record->objects[0]->payload;
		if ((image->samples > 1U) != (opcode == GPU_OP_CMD_RESOLVE_IMAGE))
			refused = ENOTSUP;
	}
	if (refused == 0 && destination_kind == I915_VK_OBJ_IMAGE) {
		image = record->objects[1]->payload;
		source_image = record->objects[0]->payload;
		moved = amdrdna_vk_image_moved(image);
		if (image->samples > 1U) {
			refused = ENOTSUP;
		} else if (opcode == GPU_OP_CMD_RESOLVE_IMAGE && moved) {
			refused = ENOTSUP;
		} else if (opcode == GPU_OP_CMD_RESOLVE_IMAGE && image->format != source_image->format) {
			refused = ENOTSUP;
		}
	}

	/*
	 * A blit draws and filters four-byte pixels: an image copied only by
	 * moves (two planes, or one or two bytes a texel) is not blitted.
	 */
	if (refused == 0 && opcode == GPU_OP_CMD_BLIT_IMAGE) {
		if (amdrdna_vk_image_moved(record->objects[0]->payload)) {
			refused = ENOTSUP;
		} else if (amdrdna_vk_image_moved(record->objects[1]->payload)) {
			refused = ENOTSUP;
		}
	}

	/* A region of several layers or depth slices becomes one region a slice (a resolve's as a copy's, its regions alike). */
	if (refused == 0 && opcode == GPU_OP_CMD_RESOLVE_IMAGE)
		refused = record_copy_slices(GPU_OP_CMD_COPY_IMAGE, record, element);
	else if (refused == 0 && opcode != GPU_OP_CMD_COPY_BUFFER)
		refused = record_copy_slices(opcode, record, element);

	/* Reports a copy that cannot be run. */
	if (refused != 0)
		return refused;

	/* Succeeded: the copy is recorded. */
	return 0;
}

/*
 * Reports whether a copy, a blit or a copy between a buffer and an image
 * that names a depth image is one the executor draws: between a buffer
 * and a depth image of one sample, each region of one aspect the image has
 * (its depth, or its stencil) at a buffer offset on four bytes; between
 * two depth images of one sample and one format, each region of the same
 * aspects on both sides, that they have; a blit of those, nearest.  A
 * depth image is not copied to or from a colour image, and not resolved.
 * Returns 0 (also for a transfer without a depth image), or ENOTSUP.
 *
 * Values referenced: the Vulkan 1.3 specification (vkCmdCopyBufferToImage
 * and vkCmdCopyImageToBuffer: a depth/stencil image's region of one aspect,
 * its bufferOffset a multiple of 4; vkCmdCopyImage: a depth/stencil
 * format's regions the same aspects on both sides, the formats the same;
 * vkCmdBlitImage: a depth/stencil image blitted only with the same format,
 * the filter VK_FILTER_NEAREST).
 */
static int
record_depth_copy(
	uint32_t opcode,
	const struct amdrdna_vk_record *record)
{
	const struct amdrdna_vk_image *source;
	const struct amdrdna_vk_image *destination;
	const struct amdrdna_vk_image *image;
	const VkBufferImageCopy *buffer_regions;
	const VkImageCopy *copies;
	const VkImageBlit *blits;
	uint32_t source_aspects;
	uint32_t destination_aspects;
	uint32_t index;
	int error;

	/*
	 * Between a buffer and an image: a depth image's regions each of one
	 * aspect, on four bytes of the buffer.
	 */
	if (opcode == GPU_OP_CMD_COPY_BUFFER_TO_IMAGE || opcode == GPU_OP_CMD_COPY_IMAGE_TO_BUFFER) {
		image = record->objects[0]->payload;
		if (opcode == GPU_OP_CMD_COPY_BUFFER_TO_IMAGE)
			image = record->objects[1]->payload;
		if (!image->depth)
			return 0;
		if (image->samples > 1U)
			return ENOTSUP;
		buffer_regions = record->regions;
		for (index = 0U; index < record->region_count; index++) {
			error = record_depth_aspects(image, buffer_regions[index].imageSubresource.aspectMask, true);
			if (error != 0)
				return error;
			if ((buffer_regions[index].bufferOffset & 3U) != 0U)
				return ENOTSUP;
		}

		/* Succeeded: the copies of the depth image are drawn. */
		return 0;
	}

	/* Between two images: neither a depth image, or both of one format and one sample (never resolved). */
	source = record->objects[0]->payload;
	destination = record->objects[1]->payload;
	if (!source->depth && !destination->depth)
		return 0;
	if (opcode == GPU_OP_CMD_RESOLVE_IMAGE)
		return ENOTSUP;
	if (!source->depth ||
	    !destination->depth ||
	    source->format != destination->format)
		return ENOTSUP;
	if (source->samples > 1U || destination->samples > 1U)
		return ENOTSUP;

	/* A blit of depth or stencil takes the nearest texel. */
	if (opcode == GPU_OP_CMD_BLIT_IMAGE && record->values[0] != VK_FILTER_NEAREST)
		return ENOTSUP;

	/* Each region the same aspects on both sides, that the format has. */
	copies = record->regions;
	blits = record->regions;
	for (index = 0U; index < record->region_count; index++) {
		if (opcode == GPU_OP_CMD_BLIT_IMAGE) {
			source_aspects = blits[index].srcSubresource.aspectMask;
			destination_aspects = blits[index].dstSubresource.aspectMask;
		} else {
			source_aspects = copies[index].srcSubresource.aspectMask;
			destination_aspects = copies[index].dstSubresource.aspectMask;
		}
		if (source_aspects != destination_aspects)
			return ENOTSUP;
		error = record_depth_aspects(source, source_aspects, false);
		if (error != 0)
			return error;
	}

	/* Succeeded: the copies between the depth images are drawn. */
	return 0;
}

/*
 * Reports whether aspects are ones a depth image has: its depth, its
 * stencil (a format with stencil), or both unless one says one alone.
 * Returns 0, or ENOTSUP.
 */
static int
record_depth_aspects(
	const struct amdrdna_vk_image *image,
	uint32_t aspects,
	bool one)
{
	/* Some aspect, only depth and stencil. */
	if (aspects == 0U || (aspects & ~(uint32_t)(VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)) != 0U)
		return ENOTSUP;

	/* The stencil of a format with stencil. */
	if ((aspects & VK_IMAGE_ASPECT_STENCIL_BIT) != 0U && !image->stencil)
		return ENOTSUP;

	/* One aspect where one is asked. */
	if (one &&
	    aspects != VK_IMAGE_ASPECT_DEPTH_BIT &&
	    aspects != VK_IMAGE_ASPECT_STENCIL_BIT)
		return ENOTSUP;

	/* Succeeded: the image has the aspects. */
	return 0;
}

/*
 * Replaces the regions of a copy, a blit or a copy between a buffer and an
 * image that name more than one slice (layers of a 2D image, or depth
 * slices of a 3D one) with one region a slice, in order: each its layer
 * (or its z and a depth of 1), and between a buffer and an image the
 * buffer's offset that slice's (a slice the buffer's rows times its image
 * height, the region's height when zero, of the image's texels).  A blit
 * whose slices are not as many in both images is refused (ENOTSUP), as
 * are more than RECORD_SLICE_REGIONS_MAX regions; ENOMEM when the regions
 * cannot be kept.
 */
static int
record_copy_slices(
	uint32_t opcode,
	struct amdrdna_vk_record *record,
	size_t element)
{
	const struct amdrdna_vk_image *source;
	const struct amdrdna_vk_image *destination;
	uint8_t *expanded;
	uint8_t *from;
	uint8_t *to;
	uint32_t slices;
	uint32_t total;
	uint32_t index;
	uint32_t slice;
	int error;

	/* The images: a buffer's copy names one, and it is both here. */
	source = record->objects[0]->payload;
	destination = record->objects[1]->payload;
	if (opcode == GPU_OP_CMD_COPY_BUFFER_TO_IMAGE)
		source = destination;
	if (opcode == GPU_OP_CMD_COPY_IMAGE_TO_BUFFER)
		destination = source;

	/* Each region's slices; nothing to do when every region names one. */
	total = 0U;
	for (index = 0U; index < record->region_count; index++) {
		error = record_region_slices(opcode, source, destination, (uint8_t *)record->regions + index * element, &slices);
		if (error != 0)
			return error;
		if (slices > RECORD_SLICE_REGIONS_MAX - total)
			return ENOTSUP;
		total += slices;
	}
	if (total == record->region_count)
		return 0;

	/* One region a slice, into a run of their own. */
	expanded = kern_calloc(total, element);
	if (expanded == NULL)
		return ENOMEM;
	to = expanded;
	for (index = 0U; index < record->region_count; index++) {
		from = (uint8_t *)record->regions + index * element;
		(void)record_region_slices(opcode, source, destination, from, &slices);
		for (slice = 0U; slice < slices; slice++) {
			kern_memcpy(to, from, element);
			record_region_slice(opcode, source, to, slice);
			to += element;
		}
	}

	/* Succeeded: the record's regions are the slices'. */
	kern_free(record->regions);
	record->regions = expanded;
	record->region_count = total;
	return 0;
}

/*
 * Reports how many slices a region of a copy names: the depth of a 3D
 * image's region, else its layers (the rest of the image's for
 * VK_REMAINING_ARRAY_LAYERS).  ENOTSUP for a blit whose two images' slices
 * differ in number or whose depth runs backwards, or for none.
 */
static int
record_region_slices(
	uint32_t opcode,
	const struct amdrdna_vk_image *source,
	const struct amdrdna_vk_image *destination,
	const void *region,
	uint32_t *slices)
{
	const VkBufferImageCopy *buffer_copy;
	const VkImageCopy *copy;
	const VkImageBlit *blit;
	int32_t depth;
	uint32_t count;
	uint32_t other;

	/* Between a buffer and an image: the image's slices. */
	if (opcode == GPU_OP_CMD_COPY_BUFFER_TO_IMAGE || opcode == GPU_OP_CMD_COPY_IMAGE_TO_BUFFER) {
		buffer_copy = region;
		count = record_layers(source, &buffer_copy->imageSubresource);
		if (source->type == VK_IMAGE_TYPE_3D)
			count = buffer_copy->imageExtent.depth;
	} else if (opcode == GPU_OP_CMD_COPY_IMAGE) {
		/* Between two images: the source's, which the destination's equal. */
		copy = region;
		count = record_layers(source, &copy->srcSubresource);
		if (source->type == VK_IMAGE_TYPE_3D || destination->type == VK_IMAGE_TYPE_3D)
			count = copy->extent.depth;
	} else {
		/* A blit: as many in both, unscaled across them. */
		blit = region;
		count = record_layers(source, &blit->srcSubresource);
		if (source->type == VK_IMAGE_TYPE_3D) {
			depth = blit->srcOffsets[1].z - blit->srcOffsets[0].z;
			if (depth <= 0)
				return ENOTSUP;
			count = (uint32_t)depth;
		}
		other = record_layers(destination, &blit->dstSubresource);
		if (destination->type == VK_IMAGE_TYPE_3D) {
			depth = blit->dstOffsets[1].z - blit->dstOffsets[0].z;
			if (depth <= 0)
				return ENOTSUP;
			other = (uint32_t)depth;
		}
		if (other != count)
			return ENOTSUP;
	}

	/* A region names one slice at least. */
	if (count == 0U)
		return ENOTSUP;

	/* Succeeded: the region's slices. */
	*slices = count;
	return 0;
}

/* Reports the layers a subresource names of an image: its count, or the image's from its first for VK_REMAINING_ARRAY_LAYERS. */
static uint32_t
record_layers(
	const struct amdrdna_vk_image *image,
	const VkImageSubresourceLayers *layers)
{
	/* The rest of the image's. */
	if (layers->layerCount == VK_REMAINING_ARRAY_LAYERS) {
		if (layers->baseArrayLayer >= image->slices)
			return 0U;
		return image->slices - layers->baseArrayLayer;
	}

	/* Succeeded: the count. */
	return layers->layerCount;
}

/*
 * Makes a copy of a region the region of one of its slices: its layer
 * (its z, a depth of 1, for a 3D image) on each side, and between a
 * buffer and an image the buffer's offset of the slice.
 */
static void
record_region_slice(
	uint32_t opcode,
	const struct amdrdna_vk_image *image,
	void *region,
	uint32_t slice)
{
	VkBufferImageCopy *buffer_copy;
	VkImageCopy *copy;
	VkImageBlit *blit;
	uint64_t rows;
	uint64_t row;

	/* Between a buffer and an image: the layer or z, and the buffer's offset of the slice. */
	if (opcode == GPU_OP_CMD_COPY_BUFFER_TO_IMAGE || opcode == GPU_OP_CMD_COPY_IMAGE_TO_BUFFER) {
		buffer_copy = region;
		row = buffer_copy->bufferRowLength;
		if (row == 0U)
			row = buffer_copy->imageExtent.width;
		rows = buffer_copy->bufferImageHeight;
		if (rows == 0U)
			rows = buffer_copy->imageExtent.height;
		buffer_copy->bufferOffset += (uint64_t)slice * row * rows * image->texel_bytes;
		if (image->type == VK_IMAGE_TYPE_3D) {
			buffer_copy->imageOffset.z += (int32_t)slice;
			buffer_copy->imageExtent.depth = 1U;
		} else {
			buffer_copy->imageSubresource.baseArrayLayer += slice;
			buffer_copy->imageSubresource.layerCount = 1U;
		}
		return;
	}

	/* Between two images: each side's layer or z, a depth of 1. */
	if (opcode == GPU_OP_CMD_COPY_IMAGE) {
		copy = region;
		copy->srcSubresource.baseArrayLayer += slice;
		copy->srcSubresource.layerCount = 1U;
		copy->dstSubresource.baseArrayLayer += slice;
		copy->dstSubresource.layerCount = 1U;
		copy->srcOffset.z += (int32_t)slice;
		copy->dstOffset.z += (int32_t)slice;
		copy->extent.depth = 1U;
		return;
	}

	/* A blit: each side's layer, or its z one slice deep. */
	blit = region;
	blit->srcSubresource.baseArrayLayer += slice;
	blit->srcSubresource.layerCount = 1U;
	blit->dstSubresource.baseArrayLayer += slice;
	blit->dstSubresource.layerCount = 1U;
	blit->srcOffsets[0].z += (int32_t)slice;
	blit->srcOffsets[1].z = blit->srcOffsets[0].z + 1;
	blit->dstOffsets[0].z += (int32_t)slice;
	blit->dstOffsets[1].z = blit->dstOffsets[0].z + 1;
}

/*
 * vkCmdClearColorImage: [image][layout][present]{colour}[count][count]
 * {VkImageSubresourceRange}; the image held, the colour's four words
 * kept, the ranges (levels and layers) in a run of the record's own.
 * vkCmdClearDepthStencilImage: [image][layout][present]{[depth][stencil]}
 * [count][count]{VkImageSubresourceRange}; the depth's float bits kept as
 * value 0 and the stencil as value 1, the ranges' aspects ones the depth
 * image has.  ENOMEM when the ranges cannot be kept; ENOTSUP for a clear
 * of an image the executor does not clear so.
 */
static int
record_clear(
	struct amdrdna_vk_session *session,
	uint32_t opcode,
	struct i915_wire_reader *reader,
	struct amdrdna_vk_record *record)
{
	const struct amdrdna_vk_image *image;
	VkImageSubresourceRange *ranges;
	VkImageSubresourceRange range;
	uint64_t identity;
	uint64_t present;
	uint64_t count;
	uint64_t index;
	int error;

	/* The image, its layout and the colour, or the depth's and the stencil's values. */
	identity = drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u32(reader);
	present = drv_i915_wire_read_u64(reader);
	if (reader->error != 0)
		return EINVAL;
	if (present != 0U && opcode == GPU_OP_CMD_CLEAR_DEPTH_STENCIL_IMAGE) {
		record->values[0] = drv_i915_wire_read_u32(reader);
		record->values[1] = drv_i915_wire_read_u32(reader);
		if (reader->error != 0)
			return EINVAL;
	} else if (present != 0U) {
		error = record_colour(reader, record->values);
		if (error != 0)
			return error;
	}

	/* The ranges, into the record's own run (read to their end even without one). */
	(void)drv_i915_wire_read_u32(reader);
	count = drv_i915_wire_read_u64(reader);
	if (reader->error != 0 || count > AMDRDNA_VK_REGIONS_MAX)
		return EINVAL;
	ranges = NULL;
	if (count != 0U)
		ranges = kern_calloc((size_t)count, sizeof(*ranges));
	record->regions = ranges;
	if (ranges != NULL)
		record->region_count = (uint32_t)count;
	for (index = 0U; index < count; index++) {
		kern_memset(&range, 0, sizeof(range));
		i915_vkc_dec_VkImageSubresourceRange(reader, &session->arena, &range);
		if (ranges != NULL)
			ranges[index] = range;
	}
	if (reader->error != 0)
		return EINVAL;
	if (count != 0U && ranges == NULL)
		return ENOMEM;

	/* The record holds the known image. */
	error = record_hold(session, record, I915_VK_OBJ_IMAGE, identity);
	if (error == 0 && record->object_count != 1U)
		error = ENOENT;
	if (error != 0)
		return error;

	/* A clear of depth and stencil draws into a depth image (every sample of one of samples), of the aspects it has. */
	image = record->objects[0]->payload;
	if (opcode == GPU_OP_CMD_CLEAR_DEPTH_STENCIL_IMAGE) {
		if (!image->depth)
			return ENOTSUP;
		for (index = 0U; index < record->region_count; index++) {
			error = record_depth_aspects(image, ranges[index].aspectMask, false);
			if (error != 0)
				return error;
		}

		/* Succeeded: the clear of depth and stencil is recorded. */
		return 0;
	}

	/* A clear draws four-byte colour pixels: an image copied only by moves, or a depth image, is not cleared so. */
	if (image->depth)
		return ENOTSUP;
	if (amdrdna_vk_image_moved(image))
		return ENOTSUP;

	/* Succeeded: the clear is recorded. */
	return 0;
}

/*
 * Reads a bind's dynamic offsets ([count][count]{offset}) into the
 * record's (value 1 their count), after its sets are held.  Returns
 * EINVAL when they did not decode, ENOTSUP for more than a record keeps,
 * and ERANGE for a count other than the sets' dynamic descriptors.
 */
static int
record_dynamic_offsets(
	struct i915_wire_reader *reader,
	struct amdrdna_vk_record *record)
{
	const struct amdrdna_vk_descriptor_set *set;
	uint64_t offsets;
	uint32_t offset;
	uint32_t wanted;
	uint32_t index;
	uint32_t binding;

	/* The offsets, each kept while the record has room. */
	(void)drv_i915_wire_read_u32(reader);
	offsets = drv_i915_wire_read_u64(reader);
	if (reader->error != 0 || offsets > AMDRDNA_VK_REGIONS_MAX)
		return EINVAL;
	for (index = 0U; index < (uint32_t)offsets; index++) {
		offset = drv_i915_wire_read_u32(reader);
		if (index < AMDRDNA_VK_DYNAMIC_MAX)
			record->dynamic[index] = offset;
	}
	if (reader->error != 0)
		return EINVAL;
	record->values[1] = (uint32_t)offsets;
	if (offsets > AMDRDNA_VK_DYNAMIC_MAX)
		return ENOTSUP;

	/* The dynamic descriptors of the sets held (a null set has none). */
	wanted = 0U;
	for (index = 0U; index < record->object_count; index++) {
		if (record->objects[index] == NULL)
			continue;
		set = record->objects[index]->payload;
		for (binding = 0U; binding < AMDRDNA_VK_BINDINGS_MAX; binding++)
			wanted += set->dynamic[binding];
	}

	/* One offset for each. */
	if (wanted != (uint32_t)offsets)
		return ERANGE;

	/* Succeeded: the offsets are kept. */
	return 0;
}

/*
 * vkCmdClearAttachments: [count][count]{VkClearAttachment}[count][count]
 * {VkClearRect}.  The aspects cleared (VkImageAspectFlags) are kept as
 * first, each colour by its colour attachment in the record's colours,
 * the depth as value 4 (its float's bits) and the stencil as value 5; the
 * rectangles go into a run of the record's own.  ENOTSUP for a rectangle
 * of a layer past the first (the framebuffers have one), ENOMEM when the
 * colours or the rectangles cannot be kept.
 */
static int
record_clear_attachments(
	struct amdrdna_vk_session *session,
	struct i915_wire_reader *reader,
	struct amdrdna_vk_record *record)
{
	VkClearRect *rects;
	VkClearRect rect;
	uint64_t count;
	uint64_t index;
	int refused;
	int error;

	/* The attachments: their aspects and values, the colours into the record's own run. */
	refused = 0;
	(void)drv_i915_wire_read_u32(reader);
	count = drv_i915_wire_read_u64(reader);
	if (reader->error != 0 || count > AMDRDNA_VK_REGIONS_MAX)
		return EINVAL;
	if (count != 0U) {
		record->colours = kern_calloc(1U, sizeof(*record->colours));
		if (record->colours == NULL)
			refused = ENOMEM;
	}
	for (index = 0U; index < count; index++) {
		error = record_clear_attachment(reader, record);
		if (error == EINVAL)
			return EINVAL;
		if (error != 0 && refused == 0)
			refused = error;
	}

	/* How many rectangles follow. */
	(void)drv_i915_wire_read_u32(reader);
	count = drv_i915_wire_read_u64(reader);
	if (reader->error != 0 || count > AMDRDNA_VK_REGIONS_MAX)
		return EINVAL;

	/* The rectangles, into the record's own run (read to their end even without one). */
	rects = NULL;
	if (count != 0U) {
		rects = kern_calloc((size_t)count, sizeof(*rects));
		if (rects == NULL && refused == 0)
			refused = ENOMEM;
	}
	record->regions = rects;
	record->region_count = 0U;
	if (rects != NULL)
		record->region_count = (uint32_t)count;
	for (index = 0U; index < count; index++) {
		kern_memset(&rect, 0, sizeof(rect));
		i915_vkc_dec_VkClearRect(reader, &session->arena, &rect);
		if (rect.baseArrayLayer != 0U && refused == 0)
			refused = ENOTSUP;
		if (rects != NULL)
			rects[index] = rect;
	}
	if (reader->error != 0)
		return EINVAL;

	/* Reports a clear that cannot be run. */
	if (refused != 0)
		return refused;

	/* Succeeded: the clear is recorded. */
	return 0;
}

/*
 * Reads one VkClearAttachment: [aspects][colour attachment] and its value,
 * [0]{colour} or [1][depth][stencil], kept in the record as
 * record_clear_attachments says (a colour only when the record has its
 * colours' run).  ENOTSUP for a colour attachment past the colour targets
 * or a value of the wrong kind.
 */
static int
record_clear_attachment(
	struct i915_wire_reader *reader,
	struct amdrdna_vk_record *record)
{
	uint32_t colour[RECORD_COLOUR_WORDS];
	uint32_t aspects;
	uint32_t attachment;
	uint32_t kind;
	int error;

	/* The aspects, the colour attachment and the value's kind. */
	aspects = drv_i915_wire_read_u32(reader);
	attachment = drv_i915_wire_read_u32(reader);
	kind = drv_i915_wire_read_u32(reader);
	if (reader->error != 0)
		return EINVAL;

	/* A colour's four words, or a depth and a stencil. */
	if (kind == 0U) {
		error = record_colour(reader, colour);
		if (error != 0)
			return error;
	} else if (kind == 1U) {
		record->values[4] = drv_i915_wire_read_u32(reader);
		record->values[5] = drv_i915_wire_read_u32(reader);
		if (reader->error != 0)
			return EINVAL;
	} else {
		return EINVAL;
	}

	/* A colour clears one colour attachment alone; the depth and the stencil only themselves. */
	if (kind == 0U && aspects != VK_IMAGE_ASPECT_COLOR_BIT)
		return ENOTSUP;
	if (kind == 0U && attachment >= AMDRDNA_VK_COLOURS_MAX)
		return ENOTSUP;
	if (kind == 1U && (aspects & ~(uint32_t)(VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)) != 0U)
		return ENOTSUP;
	record->first |= aspects;
	if (kind == 0U && record->colours != NULL) {
		kern_memcpy(record->colours->words[attachment], colour, sizeof(colour));
		record->colours->mask |= 1U << attachment;
	}

	/* Succeeded: the attachment's clear is kept. */
	return 0;
}

/*
 * vkCmdResetQueryPool: [pool][first][count]; the pool held as object 0,
 * the first query and the count kept.  ENOENT for an unknown pool and
 * ERANGE for queries outside it, which fail the recording.
 */
static int
record_query_reset(
	struct amdrdna_vk_session *session,
	struct i915_wire_reader *reader,
	struct amdrdna_vk_record *record)
{
	const struct amdrdna_vk_query_pool *pool;
	uint64_t identity;
	uint32_t first;
	uint32_t count;
	int error;

	/* The pool and the range. */
	identity = drv_i915_wire_read_u64(reader);
	first = drv_i915_wire_read_u32(reader);
	count = drv_i915_wire_read_u32(reader);
	if (reader->error != 0)
		return EINVAL;

	/* The record holds the known pool. */
	error = record_hold(session, record, I915_VK_OBJ_QUERY_POOL, identity);
	if (error == 0 && record->object_count != 1U)
		error = ENOENT;
	if (error != 0)
		return error;

	/* Queries the pool has. */
	pool = record->objects[0]->payload;
	if (first > pool->count || count > pool->count - first)
		return ERANGE;

	/* The range the reset makes unavailable when it runs. */
	record->first = first;
	record->count = count;

	/* Succeeded: the reset is recorded. */
	return 0;
}

/*
 * vkCmdWriteTimestamp: [stage][pool][query]; the record holds the known
 * pool, which must be a timestamp pool with the query, and keeps the
 * query as first.  Every stage writes at the bottom of the pipe.
 */
static int
record_timestamp(
	struct amdrdna_vk_session *session,
	struct i915_wire_reader *reader,
	struct amdrdna_vk_record *record)
{
	const struct amdrdna_vk_query_pool *pool;
	uint64_t identity;
	uint32_t query;
	int error;

	/* The stage, the pool and the query. */
	(void)drv_i915_wire_read_u32(reader);
	identity = drv_i915_wire_read_u64(reader);
	query = drv_i915_wire_read_u32(reader);
	if (reader->error != 0)
		return EINVAL;

	/* The record holds the known pool. */
	error = record_hold(session, record, I915_VK_OBJ_QUERY_POOL, identity);
	if (error == 0 && record->object_count != 1U)
		error = ENOENT;
	if (error != 0)
		return error;

	/* A timestamp pool's query. */
	pool = record->objects[0]->payload;
	if (pool->type != VK_QUERY_TYPE_TIMESTAMP || query >= pool->count)
		return ERANGE;

	/* Succeeded: the timestamp is recorded. */
	record->first = query;
	return 0;
}

/* Reads a clear colour's union: [tag 2][count 4]{word}; its four words, as their bits. */
static int
record_colour(
	struct i915_wire_reader *reader,
	uint32_t *words)
{
	uint32_t tag;
	uint64_t count;
	uint32_t index;

	/* The tag and the count of the colour's words. */
	tag = drv_i915_wire_read_u32(reader);
	count = drv_i915_wire_read_u64(reader);
	if (reader->error != 0 || tag != RECORD_COLOUR_TAG || count != RECORD_COLOUR_WORDS)
		return EINVAL;

	/* The four words. */
	for (index = 0U; index < RECORD_COLOUR_WORDS; index++)
		words[index] = drv_i915_wire_read_u32(reader);
	if (reader->error != 0)
		return EINVAL;

	/* Succeeded: the colour is read. */
	return 0;
}

/*
 * vkCmdPipelineBarrier: the masks and the three barrier arrays, read.
 * Nothing is kept: a submission runs every record in order and waits for
 * the GPU's idle between the commands that need it (vk-execute.c).
 */
static int
record_barrier(
	struct amdrdna_vk_session *session,
	struct i915_wire_reader *reader)
{
	VkMemoryBarrier memory;
	VkBufferMemoryBarrier buffer;
	VkImageMemoryBarrier image;
	uint64_t count;
	uint64_t index;

	/* The stage masks and the dependency flags. */
	(void)drv_i915_wire_read_u32(reader);
	(void)drv_i915_wire_read_u32(reader);
	(void)drv_i915_wire_read_u32(reader);

	/* The memory barriers. */
	(void)drv_i915_wire_read_u32(reader);
	count = drv_i915_wire_read_u64(reader);
	if (reader->error != 0 || count > AMDRDNA_VK_REGIONS_MAX)
		return EINVAL;
	for (index = 0U; index < count; index++)
		i915_vkc_dec_VkMemoryBarrier(reader, &session->arena, &memory);

	/* The buffer barriers. */
	(void)drv_i915_wire_read_u32(reader);
	count = drv_i915_wire_read_u64(reader);
	if (reader->error != 0 || count > AMDRDNA_VK_REGIONS_MAX)
		return EINVAL;
	for (index = 0U; index < count; index++)
		i915_vkc_dec_VkBufferMemoryBarrier(reader, &session->arena, &buffer);

	/* The image barriers. */
	(void)drv_i915_wire_read_u32(reader);
	count = drv_i915_wire_read_u64(reader);
	if (reader->error != 0 || count > AMDRDNA_VK_REGIONS_MAX)
		return EINVAL;
	for (index = 0U; index < count; index++)
		i915_vkc_dec_VkImageMemoryBarrier(reader, &session->arena, &image);
	if (reader->error != 0)
		return EINVAL;

	/* Succeeded: the barrier is read. */
	return 0;
}

/*
 * vkCmdPushConstants: [layout][stages][offset][size][count]{bytes, padded
 * to words}; the bytes read into bytes (AMDRDNA_VK_PUSH_BYTES long), which
 * the record's command buffer keeps when it keeps the record.
 */
static int
record_push(
	struct i915_wire_reader *reader,
	struct amdrdna_vk_record *record,
	uint8_t *bytes)
{
	uint32_t offset;
	uint32_t size;
	uint64_t count;

	/* The layout, the stages, the range and the byte count. */
	(void)drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u32(reader);
	offset = drv_i915_wire_read_u32(reader);
	size = drv_i915_wire_read_u32(reader);
	count = drv_i915_wire_read_u64(reader);
	if (reader->error != 0 || count != size || size > AMDRDNA_VK_PUSH_BYTES)
		return EINVAL;

	/* The bytes. */
	i915_vkc_read_bytes(reader, bytes, size);
	if (reader->error != 0)
		return EINVAL;
	record->first = offset;
	record->count = size;

	/* Within the push constant bytes a program reads. */
	if (offset > AMDRDNA_VK_PUSH_BYTES || size > AMDRDNA_VK_PUSH_BYTES - offset)
		return ENOTSUP;

	/* Succeeded: the push constants are recorded. */
	return 0;
}

/*
 * vkCmdBeginRenderPass: [present]{[sType][pNext][pass][framebuffer]
 * [VkRect2D][count][count]{clear}}[contents].  The pass is the record's
 * object 0, the framebuffer its object 1; the render area, each colour
 * clear by its attachment number (in the record's colours) and the first
 * depth and stencil clear (value 4 the depth's float's bits, count the
 * stencil; value 5 set) are kept.  A clear is [0]{colour} or
 * [1][depth][stencil].  ENOMEM when the colours cannot be kept.
 */
static int
record_begin_pass(
	struct amdrdna_vk_session *session,
	struct i915_wire_reader *reader,
	struct amdrdna_vk_record *record)
{
	struct amdrdna_vk_clear_colours *colours;
	uint64_t present;
	uint64_t pass;
	uint64_t framebuffer;
	uint64_t count;
	uint64_t index;
	uint32_t kind;
	uint32_t colour[RECORD_COLOUR_WORDS];
	uint32_t depth;
	uint32_t stencil;
	int refused;
	int error;

	/* The begin info. */
	present = drv_i915_wire_read_u64(reader);
	if (reader->error != 0 || present != 1U)
		return EINVAL;
	(void)drv_i915_wire_read_u32(reader);
	(void)drv_i915_wire_read_u64(reader);
	pass = drv_i915_wire_read_u64(reader);
	framebuffer = drv_i915_wire_read_u64(reader);
	i915_vkc_dec_VkRect2D(reader, &session->arena, &record->rect);
	(void)drv_i915_wire_read_u32(reader);
	count = drv_i915_wire_read_u64(reader);
	if (reader->error != 0 || count > AMDRDNA_VK_REGIONS_MAX)
		return EINVAL;

	/* The colours' own run (the clear values are read to their end even without one). */
	refused = 0;
	colours = NULL;
	if (count != 0U) {
		colours = kern_calloc(1U, sizeof(*colours));
		if (colours == NULL)
			refused = ENOMEM;
	}
	record->colours = colours;

	/* The clear values: each colour by its attachment, the first depth and stencil. */
	for (index = 0U; index < count; index++) {
		kind = drv_i915_wire_read_u32(reader);
		if (reader->error != 0)
			return EINVAL;
		if (kind == 0U) {
			error = record_colour(reader, colour);
			if (error != 0)
				return error;
			if (colours != NULL && index < AMDRDNA_VK_ATTACHMENTS_MAX) {
				kern_memcpy(colours->words[index], colour, sizeof(colour));
				colours->mask |= 1U << index;
			}
		} else if (kind == 1U) {
			depth = drv_i915_wire_read_u32(reader);
			stencil = drv_i915_wire_read_u32(reader);
			if (record->values[5] == 0U) {
				record->values[4] = depth;
				record->values[5] = 1U;
				record->count = stencil;
			}
		} else {
			return EINVAL;
		}
	}

	/* The contents (inline only: there are no secondary command buffers). */
	(void)drv_i915_wire_read_u32(reader);
	if (reader->error != 0)
		return EINVAL;

	/* The record holds the known pass and framebuffer. */
	error = record_hold(session, record, I915_VK_OBJ_RENDER_PASS, pass);
	if (error == 0)
		error = record_hold(session, record, I915_VK_OBJ_FRAMEBUFFER, framebuffer);
	if (error == 0 && record->object_count != 2U)
		error = ENOENT;
	if (error != 0)
		return error;

	/* Reports colours that could not be kept. */
	if (refused != 0)
		return refused;

	/* Succeeded: the pass's beginning is recorded. */
	return 0;
}

/*
 * Makes room at the end of a command buffer's records for one more, and
 * for a push constant record AMDRDNA_VK_PUSH_BYTES more push bytes too,
 * growing each run (twice as large, what it held moved over) when it
 * lacks it.  Returns ENOMEM when a run cannot grow.
 */
static int
record_room(
	struct amdrdna_vk_command_buffer *command,
	uint32_t opcode)
{
	struct amdrdna_vk_record *grown;
	uint8_t *grown_bytes;
	uint32_t capacity;

	/* Grows a full array. */
	if (command->record_count == command->record_capacity) {
		if (command->record_capacity >= RECORD_MAX)
			return ENOMEM;
		capacity = RECORD_FIRST_CAPACITY;
		if (command->record_capacity != 0U)
			capacity = command->record_capacity * 2U;
		grown = kern_malloc((size_t)capacity * sizeof(*grown));
		if (grown == NULL)
			return ENOMEM;
		if (command->record_count != 0U)
			kern_memcpy(grown, command->records, (size_t)command->record_count * sizeof(*grown));
		if (command->records != NULL)
			kern_free(command->records);
		command->records = grown;
		command->record_capacity = capacity;
	}

	/* Only a push constant record takes push bytes. */
	if (opcode != GPU_OP_CMD_PUSH_CONSTANTS)
		return 0;

	/* Grows a run without room for the most bytes one record has. */
	if (AMDRDNA_VK_PUSH_BYTES > command->push_capacity - command->push_used) {
		if (command->push_capacity >= RECORD_MAX * AMDRDNA_VK_PUSH_BYTES)
			return ENOMEM;
		capacity = RECORD_FIRST_CAPACITY * AMDRDNA_VK_PUSH_BYTES;
		if (command->push_capacity != 0U)
			capacity = command->push_capacity * 2U;
		grown_bytes = kern_malloc(capacity);
		if (grown_bytes == NULL)
			return ENOMEM;
		if (command->push_used != 0U)
			kern_memcpy(grown_bytes, command->push_bytes, command->push_used);
		if (command->push_bytes != NULL)
			kern_free(command->push_bytes);
		command->push_bytes = grown_bytes;
		command->push_capacity = capacity;
	}

	/* Succeeded: the next record, and its push bytes, have their place. */
	return 0;
}

/* Lets a record that is not kept go: its objects and its run of regions. */
static void
record_drop(
	struct amdrdna_vk_record *record)
{
	uint32_t index;

	/* Its objects. */
	for (index = 0U; index < record->object_count; index++)
		(void)amdrdna_vk_object_release(record->objects[index]);
	record->object_count = 0U;

	/* Its run of regions and its clear colours. */
	if (record->regions != NULL)
		kern_free(record->regions);
	record->regions = NULL;
	if (record->colours != NULL)
		kern_free(record->colours);
	record->colours = NULL;
}
