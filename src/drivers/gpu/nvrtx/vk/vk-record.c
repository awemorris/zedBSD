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

#include "drivers/gpu/nvrtx/vk/nvrtx-vk-objects.h"

#include "drivers/gpu/i915/vk/vk-codec.inc"

/* The records a command buffer's array starts with; it doubles as it fills. */
#define RECORD_FIRST_CAPACITY		64U

/* The most records one command buffer keeps. */
#define RECORD_MAX			65536U

/* The words of a clear colour's union on the wire: its tag, then four words. */
#define RECORD_COLOUR_TAG		2U
#define RECORD_COLOUR_WORDS		4U

static int record_decode(struct nvrtx_vk_session *session, uint32_t opcode, struct i915_wire_reader *reader, struct nvrtx_vk_record *record, uint8_t *push);
static int record_hold(struct nvrtx_vk_session *session, struct nvrtx_vk_record *record, enum i915_vk_object_kind kind, uint64_t identity);
static int record_pipeline(struct nvrtx_vk_session *session, struct i915_wire_reader *reader, struct nvrtx_vk_record *record);
static int record_viewport(struct nvrtx_vk_session *session, struct i915_wire_reader *reader, struct nvrtx_vk_record *record);
static int record_scissor(struct nvrtx_vk_session *session, struct i915_wire_reader *reader, struct nvrtx_vk_record *record);
static int record_sets(struct nvrtx_vk_session *session, struct i915_wire_reader *reader, struct nvrtx_vk_record *record);
static int record_vertices(struct nvrtx_vk_session *session, struct i915_wire_reader *reader, struct nvrtx_vk_record *record);
static int record_indices(struct nvrtx_vk_session *session, struct i915_wire_reader *reader, struct nvrtx_vk_record *record);
static int record_draw(struct i915_wire_reader *reader, struct nvrtx_vk_record *record);
static int record_draw_indexed(struct i915_wire_reader *reader, struct nvrtx_vk_record *record);
static int record_copy(struct nvrtx_vk_session *session, uint32_t opcode, struct i915_wire_reader *reader, struct nvrtx_vk_record *record);
static int record_clear(struct nvrtx_vk_session *session, struct i915_wire_reader *reader, struct nvrtx_vk_record *record);
static int record_colour(struct i915_wire_reader *reader, uint32_t *words);
static int record_barrier(struct nvrtx_vk_session *session, struct i915_wire_reader *reader);
static int record_push(struct i915_wire_reader *reader, struct nvrtx_vk_record *record, uint8_t *bytes);
static int record_begin_pass(struct nvrtx_vk_session *session, struct i915_wire_reader *reader, struct nvrtx_vk_record *record);
static int record_line_width(struct i915_wire_reader *reader);
static int record_room(struct nvrtx_vk_command_buffer *command, uint32_t opcode);
static void record_drop(struct nvrtx_vk_record *record);

/*
 * Records one vkCmd* command; *handled stays clear for an opcode of
 * another handler's.
 */
int
nvrtx_vk_record_route(
	struct nvrtx_vk_session *session,
	uint32_t opcode,
	uint32_t requested,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply,
	int *handled)
{
	struct nvrtx_vk_command_buffer *command;
	struct nvrtx_vk_record *record;
	struct nvrtx_vk_record aside;
	uint8_t push[NVRTX_VK_PUSH_BYTES];
	uint8_t *bytes;
	uint64_t identity;
	bool video;
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
	case GPU_OP_CMD_BIND_DESCRIPTOR_SETS:
	case GPU_OP_CMD_BIND_VERTEX_BUFFERS:
	case GPU_OP_CMD_BIND_INDEX_BUFFER:
	case GPU_OP_CMD_DRAW:
	case GPU_OP_CMD_DRAW_INDEXED:
	case GPU_OP_CMD_COPY_BUFFER:
	case GPU_OP_CMD_COPY_IMAGE:
	case GPU_OP_CMD_BLIT_IMAGE:
	case GPU_OP_CMD_COPY_BUFFER_TO_IMAGE:
	case GPU_OP_CMD_COPY_IMAGE_TO_BUFFER:
	case GPU_OP_CMD_CLEAR_COLOR_IMAGE:
	case GPU_OP_CMD_PIPELINE_BARRIER:
	case GPU_OP_CMD_PUSH_CONSTANTS:
	case GPU_OP_CMD_BEGIN_RENDER_PASS:
	case GPU_OP_CMD_END_RENDER_PASS:
		break;
	default:
		/* The video and query commands are taken here too; any other opcode is another route's. */
		video = nvrtx_vk_video_recorded(opcode);
		if (!video)
			return 0;
		break;
	}

	*handled = 1;

	/* The command buffer the record is for. */
	identity = drv_i915_wire_read_u64(reader);
	if (reader->error != 0)
		return EINVAL;

	/* A command buffer that is recording keeps the record; another one's records are read and let go. */
	command = nvrtx_vk_payload(session, I915_VK_OBJ_COMMAND_BUFFER, identity);
	if (command != NULL && command->state != NVRTX_VK_COMMAND_RECORDING)
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

	/* A video or query command keeps a structure of its own and takes its scope from the recording. */
	video = nvrtx_vk_video_recorded(opcode);
	if (video)
		refused = nvrtx_vk_video_record(session, command, opcode, reader, record);
	else
		refused = record_decode(session, opcode, reader, record, bytes);

	/* Drops a record that did not decode. */
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
		kern_logf("nvrtx: vk: a recorded command %u is not run (%d)\n", (unsigned)opcode, refused);
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
 * push, NVRTX_VK_PUSH_BYTES long).  Returns EINVAL when they did not
 * decode (the stream is then unreadable), another error when they decoded
 * but cannot be run.
 */
static int
record_decode(
	struct nvrtx_vk_session *session,
	uint32_t opcode,
	struct i915_wire_reader *reader,
	struct nvrtx_vk_record *record,
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
	case GPU_OP_CMD_COPY_BUFFER:
	case GPU_OP_CMD_COPY_IMAGE:
	case GPU_OP_CMD_BLIT_IMAGE:
	case GPU_OP_CMD_COPY_BUFFER_TO_IMAGE:
	case GPU_OP_CMD_COPY_IMAGE_TO_BUFFER:
		error = record_copy(session, opcode, reader, record);
		break;
	case GPU_OP_CMD_CLEAR_COLOR_IMAGE:
		error = record_clear(session, reader, record);
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
	struct nvrtx_vk_session *session,
	struct nvrtx_vk_record *record,
	enum i915_vk_object_kind kind,
	uint64_t identity)
{
	struct nvrtx_vk_object *object;
	int error;

	/* A null handle names nothing. */
	if (identity == 0U)
		return 0;

	/* A known object, while the record has room for it. */
	object = nvrtx_vk_object_find(session, kind, identity);
	if (object == NULL)
		return ENOENT;
	if (record->object_count >= NVRTX_VK_SETS_MAX)
		return ENOTSUP;

	/* The record holds it. */
	error = nvrtx_vk_object_retain(object);
	if (error != 0)
		return error;
	record->objects[record->object_count] = object;
	record->object_count++;

	/* Succeeded: the record holds the object. */
	return 0;
}

/* vkCmdBindPipeline: [bind point][pipeline]; a graphics pipeline. */
static int
record_pipeline(
	struct nvrtx_vk_session *session,
	struct i915_wire_reader *reader,
	struct nvrtx_vk_record *record)
{
	uint32_t point;
	uint64_t pipeline;
	int error;

	/* The bind point and the pipeline. */
	point = drv_i915_wire_read_u32(reader);
	pipeline = drv_i915_wire_read_u64(reader);
	if (reader->error != 0)
		return EINVAL;

	/* Only the graphics bind point is run. */
	if (point != VK_PIPELINE_BIND_POINT_GRAPHICS)
		return ENOTSUP;

	/* The record holds a known pipeline. */
	error = record_hold(session, record, I915_VK_OBJ_PIPELINE, pipeline);
	if (error != 0)
		return error;
	if (record->object_count == 0U)
		return ENOENT;

	/* Succeeded: the pipeline is recorded. */
	return 0;
}

/* vkCmdSetViewport: [first][count][count]{VkViewport}; the first viewport, as float bits. */
static int
record_viewport(
	struct nvrtx_vk_session *session,
	struct i915_wire_reader *reader,
	struct nvrtx_vk_record *record)
{
	VkViewport viewport;
	uint64_t count;
	uint64_t index;
	uint32_t first;

	/* The first index and the viewports. */
	first = drv_i915_wire_read_u32(reader);
	(void)drv_i915_wire_read_u32(reader);
	count = drv_i915_wire_read_u64(reader);
	if (reader->error != 0 || count > NVRTX_VK_REGIONS_MAX)
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
	struct nvrtx_vk_session *session,
	struct i915_wire_reader *reader,
	struct nvrtx_vk_record *record)
{
	VkRect2D rect;
	uint64_t count;
	uint64_t index;
	uint32_t first;

	/* The first index and the rectangles. */
	first = drv_i915_wire_read_u32(reader);
	(void)drv_i915_wire_read_u32(reader);
	count = drv_i915_wire_read_u64(reader);
	if (reader->error != 0 || count > NVRTX_VK_REGIONS_MAX)
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

/*
 * vkCmdBindDescriptorSets: [bind point][layout][first set][count][count]
 * {set}[dynamic count][count]{offset}; graphics sets, held, without
 * dynamic offsets.
 */
static int
record_sets(
	struct nvrtx_vk_session *session,
	struct i915_wire_reader *reader,
	struct nvrtx_vk_record *record)
{
	uint64_t count;
	uint64_t index;
	uint64_t identity;
	uint64_t offsets;
	uint32_t point;
	int refused;
	int error;

	/* The bind point, the layout, the first set and how many sets follow. */
	point = drv_i915_wire_read_u32(reader);
	(void)drv_i915_wire_read_u64(reader);
	record->first = drv_i915_wire_read_u32(reader);
	(void)drv_i915_wire_read_u32(reader);
	count = drv_i915_wire_read_u64(reader);
	if (reader->error != 0 || count > NVRTX_VK_REGIONS_MAX)
		return EINVAL;

	/* Each set, held while the bind's sets fit a pipeline layout. */
	refused = 0;
	if (point != VK_PIPELINE_BIND_POINT_GRAPHICS)
		refused = ENOTSUP;
	if (record->first >= NVRTX_VK_SETS_MAX || count > NVRTX_VK_SETS_MAX - record->first)
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

	/* The dynamic offsets: none is taken. */
	(void)drv_i915_wire_read_u32(reader);
	offsets = drv_i915_wire_read_u64(reader);
	if (reader->error != 0 || offsets > NVRTX_VK_REGIONS_MAX)
		return EINVAL;
	for (index = 0U; index < offsets; index++)
		(void)drv_i915_wire_read_u32(reader);
	if (reader->error != 0)
		return EINVAL;
	if (offsets != 0U && refused == 0)
		refused = ENOTSUP;

	/* Reports a bind that cannot be run. */
	record->count = (uint32_t)count;
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
	struct nvrtx_vk_session *session,
	struct i915_wire_reader *reader,
	struct nvrtx_vk_record *record)
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
	if (reader->error != 0 || count > NVRTX_VK_REGIONS_MAX)
		return EINVAL;
	refused = 0;
	if (record->first >= NVRTX_VK_VERTEX_BINDINGS_MAX || count > NVRTX_VK_VERTEX_BINDINGS_MAX - record->first)
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
	if (reader->error != 0 || offsets > NVRTX_VK_REGIONS_MAX)
		return EINVAL;
	for (index = 0U; index < offsets; index++) {
		offset = drv_i915_wire_read_u64(reader);
		if (index < NVRTX_VK_VERTEX_BINDINGS_MAX)
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
	struct nvrtx_vk_session *session,
	struct i915_wire_reader *reader,
	struct nvrtx_vk_record *record)
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

/* vkCmdDrawIndexed: [index count][instance count][first index][vertex offset][first instance]. */
static int
record_draw_indexed(
	struct i915_wire_reader *reader,
	struct nvrtx_vk_record *record)
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

/* vkCmdDraw: [vertex count][instance count][first vertex][first instance]. */
static int
record_draw(
	struct i915_wire_reader *reader,
	struct nvrtx_vk_record *record)
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

/*
 * vkCmdCopyBuffer: [source][destination][count][count]{VkBufferCopy};
 * vkCmdCopyImage, vkCmdBlitImage: [source][layout][destination][layout]
 * [count][count]{VkImageCopy or VkImageBlit} (and [filter] after a blit);
 * vkCmdCopyBufferToImage: [buffer][image][layout][count][count]
 * {VkBufferImageCopy}; vkCmdCopyImageToBuffer: [image][layout][buffer]
 * [count][count]{VkBufferImageCopy}.  The source is the record's object
 * 0, the destination its object 1; the regions are copied into a run of
 * the record's own.
 */
static int
record_copy(
	struct nvrtx_vk_session *session,
	uint32_t opcode,
	struct i915_wire_reader *reader,
	struct nvrtx_vk_record *record)
{
	enum i915_vk_object_kind source_kind;
	enum i915_vk_object_kind destination_kind;
	uint64_t source;
	uint64_t destination;
	uint64_t count;
	uint64_t index;
	size_t element;
	uint8_t *regions;
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
	}

	/* How many regions follow. */
	(void)drv_i915_wire_read_u32(reader);
	count = drv_i915_wire_read_u64(reader);
	if (reader->error != 0 || count > NVRTX_VK_REGIONS_MAX)
		return EINVAL;

	/* The regions, into the record's own run. */
	regions = NULL;
	if (count != 0U) {
		regions = kern_calloc((size_t)count, element);
		if (regions == NULL)
			return ENOMEM;
	}
	record->regions = regions;
	record->region_count = (uint32_t)count;
	for (index = 0U; index < count; index++) {
		if (opcode == GPU_OP_CMD_COPY_BUFFER) {
			i915_vkc_dec_VkBufferCopy(reader, &session->arena, (VkBufferCopy *)(void *)(regions + index * element));
		} else if (opcode == GPU_OP_CMD_COPY_IMAGE) {
			i915_vkc_dec_VkImageCopy(reader, &session->arena, (VkImageCopy *)(void *)(regions + index * element));
		} else if (opcode == GPU_OP_CMD_BLIT_IMAGE) {
			i915_vkc_dec_VkImageBlit(reader, &session->arena, (VkImageBlit *)(void *)(regions + index * element));
		} else {
			i915_vkc_dec_VkBufferImageCopy(reader, &session->arena, (VkBufferImageCopy *)(void *)(regions + index * element));
		}
	}

	/* A blit's filter. */
	if (opcode == GPU_OP_CMD_BLIT_IMAGE)
		record->values[0] = drv_i915_wire_read_u32(reader);
	if (reader->error != 0)
		return EINVAL;

	/* The record holds both resources, which must be known. */
	refused = 0;
	error = record_hold(session, record, source_kind, source);
	if (error == 0)
		error = record_hold(session, record, destination_kind, destination);
	if (error == 0 && record->object_count != 2U)
		error = ENOENT;
	if (error != 0)
		refused = error;

	/* Reports a copy that cannot be run. */
	if (refused != 0)
		return refused;

	/* Succeeded: the copy is recorded. */
	return 0;
}

/*
 * vkCmdClearColorImage: [image][layout][present]{colour}[count][count]
 * {VkImageSubresourceRange}; the image held, the colour's four words kept
 * (the image has one level and layer, so every range is all of it).
 */
static int
record_clear(
	struct nvrtx_vk_session *session,
	struct i915_wire_reader *reader,
	struct nvrtx_vk_record *record)
{
	VkImageSubresourceRange range;
	uint64_t image;
	uint64_t present;
	uint64_t count;
	uint64_t index;
	int error;

	/* The image, its layout and the colour. */
	image = drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u32(reader);
	present = drv_i915_wire_read_u64(reader);
	if (reader->error != 0)
		return EINVAL;
	if (present != 0U) {
		error = record_colour(reader, record->values);
		if (error != 0)
			return error;
	}

	/* The ranges, read. */
	(void)drv_i915_wire_read_u32(reader);
	count = drv_i915_wire_read_u64(reader);
	if (reader->error != 0 || count > NVRTX_VK_REGIONS_MAX)
		return EINVAL;
	for (index = 0U; index < count; index++)
		i915_vkc_dec_VkImageSubresourceRange(reader, &session->arena, &range);
	if (reader->error != 0)
		return EINVAL;

	/* The record holds the known image. */
	error = record_hold(session, record, I915_VK_OBJ_IMAGE, image);
	if (error == 0 && record->object_count != 1U)
		error = ENOENT;
	if (error != 0)
		return error;

	/* Succeeded: the clear is recorded. */
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
	struct nvrtx_vk_session *session,
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
	if (reader->error != 0 || count > NVRTX_VK_REGIONS_MAX)
		return EINVAL;
	for (index = 0U; index < count; index++)
		i915_vkc_dec_VkMemoryBarrier(reader, &session->arena, &memory);

	/* The buffer barriers. */
	(void)drv_i915_wire_read_u32(reader);
	count = drv_i915_wire_read_u64(reader);
	if (reader->error != 0 || count > NVRTX_VK_REGIONS_MAX)
		return EINVAL;
	for (index = 0U; index < count; index++)
		i915_vkc_dec_VkBufferMemoryBarrier(reader, &session->arena, &buffer);

	/* The image barriers. */
	(void)drv_i915_wire_read_u32(reader);
	count = drv_i915_wire_read_u64(reader);
	if (reader->error != 0 || count > NVRTX_VK_REGIONS_MAX)
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
 * to words}; the bytes read into bytes (NVRTX_VK_PUSH_BYTES long), which
 * the record's command buffer keeps when it keeps the record.
 */
static int
record_push(
	struct i915_wire_reader *reader,
	struct nvrtx_vk_record *record,
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
	if (reader->error != 0 || count != size || size > NVRTX_VK_PUSH_BYTES)
		return EINVAL;

	/* The bytes. */
	i915_vkc_read_bytes(reader, bytes, size);
	if (reader->error != 0)
		return EINVAL;
	record->first = offset;
	record->count = size;

	/* Within the push constant bytes a program reads. */
	if (offset > NVRTX_VK_PUSH_BYTES || size > NVRTX_VK_PUSH_BYTES - offset)
		return ENOTSUP;

	/* Succeeded: the push constants are recorded. */
	return 0;
}

/*
 * vkCmdBeginRenderPass: [present]{[sType][pNext][pass][framebuffer]
 * [VkRect2D][count][count]{clear}}[contents].  The pass is the record's
 * object 0, the framebuffer its object 1; the render area, the first
 * clear colour and the first clear depth with its stencil are kept.  A
 * clear is [0]{colour} or [1][depth][stencil].
 */
static int
record_begin_pass(
	struct nvrtx_vk_session *session,
	struct i915_wire_reader *reader,
	struct nvrtx_vk_record *record)
{
	uint64_t present;
	uint64_t pass;
	uint64_t framebuffer;
	uint64_t count;
	uint64_t index;
	uint32_t kind;
	uint32_t colour[RECORD_COLOUR_WORDS];
	uint32_t colours;
	uint32_t depths;
	uint32_t depth;
	uint32_t stencil;
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
	if (reader->error != 0 || count > NVRTX_VK_REGIONS_MAX)
		return EINVAL;

	/* The clear values: the first colour is kept in values 0 to 3, the first depth (a float's bits) in value 4, its stencil in value 5. */
	colours = 0U;
	depths = 0U;
	for (index = 0U; index < count; index++) {
		kind = drv_i915_wire_read_u32(reader);
		if (reader->error != 0)
			return EINVAL;
		if (kind == 0U) {
			error = record_colour(reader, colour);
			if (error != 0)
				return error;
			if (colours == 0U)
				kern_memcpy(record->values, colour, sizeof(colour));
			colours++;
		} else if (kind == 1U) {
			depth = drv_i915_wire_read_u32(reader);
			stencil = drv_i915_wire_read_u32(reader);
			if (depths == 0U) {
				record->values[RECORD_COLOUR_WORDS] = depth;
				record->values[RECORD_COLOUR_WORDS + 1U] = stencil;
			}
			depths++;
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

	/* Succeeded: the pass's beginning is recorded. */
	return 0;
}

/*
 * Makes room at the end of a command buffer's records for one more, and
 * for a push constant record NVRTX_VK_PUSH_BYTES more push bytes too,
 * growing each run (twice as large, what it held moved over) when it
 * lacks it.  Returns ENOMEM when a run cannot grow.
 */
static int
record_room(
	struct nvrtx_vk_command_buffer *command,
	uint32_t opcode)
{
	struct nvrtx_vk_record *grown;
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
	if (NVRTX_VK_PUSH_BYTES > command->push_capacity - command->push_used) {
		if (command->push_capacity >= RECORD_MAX * NVRTX_VK_PUSH_BYTES)
			return ENOMEM;
		capacity = RECORD_FIRST_CAPACITY * NVRTX_VK_PUSH_BYTES;
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
	struct nvrtx_vk_record *record)
{
	uint32_t index;
	bool video;

	/* Its objects. */
	for (index = 0U; index < record->object_count; index++)
		(void)nvrtx_vk_object_release(record->objects[index]);
	record->object_count = 0U;

	/* A video or query command's structure, with what it holds. */
	video = nvrtx_vk_video_recorded(record->opcode);
	if (video)
		nvrtx_vk_video_drop(record);

	/* Its run of regions. */
	if (record->regions != NULL)
		kern_free(record->regions);
	record->regions = NULL;
}
