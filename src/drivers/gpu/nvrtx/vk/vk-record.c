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
 * A record the executor cannot run (a pipeline of the other bind point, an unknown object,
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

/*
 * The bytes of the commands a draw records as the short way reads them: the
 * header (the opcode and the reply flag), the command buffer, then
 * vkCmdDrawIndexed's five counts, vkCmdDraw's four, vkCmdBindPipeline's bind
 * point and pipeline, vkCmdBindDescriptorSets with one set up to its dynamic
 * offsets (four bytes each), and vkCmdPushConstants up to its bytes.
 */
#define RECORD_QUICK_HEADER		8U
#define RECORD_QUICK_BUFFER		8U
#define RECORD_QUICK_DRAW_INDEXED	20U
#define RECORD_QUICK_DRAW		16U
#define RECORD_QUICK_PIPELINE		12U
#define RECORD_QUICK_SETS		48U
#define RECORD_QUICK_OFFSET		4U
#define RECORD_QUICK_PUSH		28U

/* The rectangles one vkCmdClearAttachments keeps, and its depth and stencil values' places among its values. */
#define RECORD_CLEAR_RECTS_MAX		NVRTX_VK_REGIONS_MAX
#define RECORD_CLEAR_DEPTH		4U
#define RECORD_CLEAR_STENCIL		5U

/* The most bytes one vkCmdUpdateBuffer carries (Vulkan's bound). */
#define RECORD_UPDATE_BYTES_MAX		65536U

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
static int record_draw_indirect(struct nvrtx_vk_session *session, uint32_t opcode, struct i915_wire_reader *reader, struct nvrtx_vk_record *record);
static int record_dispatch(struct i915_wire_reader *reader, struct nvrtx_vk_record *record);
static int record_dispatch_indirect(struct nvrtx_vk_session *session, struct i915_wire_reader *reader, struct nvrtx_vk_record *record);
static int record_dispatch_base(struct i915_wire_reader *reader, struct nvrtx_vk_record *record);
static int record_copy(struct nvrtx_vk_session *session, uint32_t opcode, struct i915_wire_reader *reader, struct nvrtx_vk_record *record);
static int record_update_buffer(struct nvrtx_vk_session *session, struct i915_wire_reader *reader, struct nvrtx_vk_record *record);
static int record_fill_buffer(struct nvrtx_vk_session *session, struct i915_wire_reader *reader, struct nvrtx_vk_record *record);
static int record_clear(struct nvrtx_vk_session *session, uint32_t opcode, struct i915_wire_reader *reader, struct nvrtx_vk_record *record);
static int record_colour(struct i915_wire_reader *reader, uint32_t *words);
static int record_barrier(struct nvrtx_vk_session *session, struct i915_wire_reader *reader);
static int record_push(struct i915_wire_reader *reader, struct nvrtx_vk_record *record, uint8_t *bytes);
static int record_begin_pass(struct nvrtx_vk_session *session, struct i915_wire_reader *reader, struct nvrtx_vk_record *record);
static int record_line_width(struct i915_wire_reader *reader);
static int record_depth_bias(struct i915_wire_reader *reader, struct nvrtx_vk_record *record);
static int record_blend_constants(struct i915_wire_reader *reader, struct nvrtx_vk_record *record);
static int record_stencil(struct i915_wire_reader *reader, struct nvrtx_vk_record *record);
static int record_clear_attachments(struct nvrtx_vk_session *session, struct i915_wire_reader *reader, struct nvrtx_vk_record *record);
static uint32_t record_dynamic_wanted(const struct nvrtx_vk_record *record);
static int record_room(struct nvrtx_vk_command_buffer *command, uint32_t opcode);
static void record_drop(struct nvrtx_vk_record *record);
static bool record_quick(struct nvrtx_vk_session *session, uint32_t opcode, struct i915_wire_reader *reader);
static bool record_quick_taken(uint32_t opcode);
static bool record_quick_one(struct nvrtx_vk_session *session, struct nvrtx_vk_command_buffer *command, uint32_t opcode, const uint8_t *arguments, size_t left, size_t *taken);
static bool record_quick_pipeline(struct nvrtx_vk_session *session, const uint8_t *bytes, struct nvrtx_vk_record *record);
static bool record_quick_sets(struct nvrtx_vk_session *session, const uint8_t *bytes, size_t left, struct nvrtx_vk_record *record, size_t *taken);
static bool record_quick_push(struct nvrtx_vk_command_buffer *command, const uint8_t *bytes, size_t left, struct nvrtx_vk_record *record, size_t *taken);
static uint32_t record_word(const uint8_t *bytes);
static uint64_t record_wide(const uint8_t *bytes);

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
	bool quick;
	bool video;
	int refused;
	int room;

	UNUSED_PARAMETER(requested);
	UNUSED_PARAMETER(reply);

	/*
	 * The commands of each draw, for a recording command buffer with room,
	 * take the short way (most of a frame's commands are these); anything
	 * the short way does not take is read the general way below.
	 */
	quick = record_quick(session, opcode, reader);
	if (quick) {
		*handled = 1;
		return 0;
	}

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
	case GPU_OP_CMD_DRAW_INDIRECT:
	case GPU_OP_CMD_DRAW_INDEXED_INDIRECT:
	case GPU_OP_CMD_DISPATCH:
	case GPU_OP_CMD_DISPATCH_INDIRECT:
	case GPU_OP_CMD_DISPATCH_BASE:
	case GPU_OP_CMD_COPY_BUFFER:
	case GPU_OP_CMD_COPY_IMAGE:
	case GPU_OP_CMD_BLIT_IMAGE:
	case GPU_OP_CMD_RESOLVE_IMAGE:
	case GPU_OP_CMD_COPY_BUFFER_TO_IMAGE:
	case GPU_OP_CMD_COPY_IMAGE_TO_BUFFER:
	case GPU_OP_CMD_UPDATE_BUFFER:
	case GPU_OP_CMD_FILL_BUFFER:
	case GPU_OP_CMD_CLEAR_COLOR_IMAGE:
	case GPU_OP_CMD_CLEAR_DEPTH_STENCIL_IMAGE:
	case GPU_OP_CMD_CLEAR_ATTACHMENTS:
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
	if (command != NULL && opcode != GPU_OP_CMD_SET_LINE_WIDTH) {
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

	/* A command that changes nothing a submission runs is not kept. */
	if (opcode == GPU_OP_CMD_SET_LINE_WIDTH)
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
	case GPU_OP_CMD_DRAW_INDIRECT:
	case GPU_OP_CMD_DRAW_INDEXED_INDIRECT:
		error = record_draw_indirect(session, opcode, reader, record);
		break;
	case GPU_OP_CMD_DISPATCH:
		error = record_dispatch(reader, record);
		break;
	case GPU_OP_CMD_DISPATCH_INDIRECT:
		error = record_dispatch_indirect(session, reader, record);
		break;
	case GPU_OP_CMD_DISPATCH_BASE:
		error = record_dispatch_base(reader, record);
		break;
	case GPU_OP_CMD_COPY_BUFFER:
	case GPU_OP_CMD_COPY_IMAGE:
	case GPU_OP_CMD_BLIT_IMAGE:
	case GPU_OP_CMD_RESOLVE_IMAGE:
	case GPU_OP_CMD_COPY_BUFFER_TO_IMAGE:
	case GPU_OP_CMD_COPY_IMAGE_TO_BUFFER:
		error = record_copy(session, opcode, reader, record);
		break;
	case GPU_OP_CMD_UPDATE_BUFFER:
		error = record_update_buffer(session, reader, record);
		break;
	case GPU_OP_CMD_FILL_BUFFER:
		error = record_fill_buffer(session, reader, record);
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
	if (record->object_count >= NVRTX_VK_RECORD_OBJECTS)
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

/* vkCmdBindPipeline: [bind point][pipeline]; a graphics or a compute pipeline at its own bind point. */
static int
record_pipeline(
	struct nvrtx_vk_session *session,
	struct i915_wire_reader *reader,
	struct nvrtx_vk_record *record)
{
	const struct nvrtx_vk_pipeline *held;
	uint32_t point;
	uint64_t pipeline;
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

	/* The record holds a known pipeline of the bind point's kind. */
	error = record_hold(session, record, I915_VK_OBJ_PIPELINE, pipeline);
	if (error != 0)
		return error;
	if (record->object_count == 0U)
		return ENOENT;
	held = record->objects[0]->payload;
	if (held->compute != (point == VK_PIPELINE_BIND_POINT_COMPUTE))
		return EINVAL;

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
 * vkCmdSetDepthBias: [constant factor][clamp][slope factor], kept as
 * float bits in values 0 to 2.
 */
static int
record_depth_bias(
	struct i915_wire_reader *reader,
	struct nvrtx_vk_record *record)
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

/* vkCmdSetBlendConstants: [count 4]{constant}, kept as float bits in values 0 to 3. */
static int
record_blend_constants(
	struct i915_wire_reader *reader,
	struct nvrtx_vk_record *record)
{
	uint64_t count;
	uint32_t index;

	/* The count of the constants. */
	count = drv_i915_wire_read_u64(reader);
	if (reader->error != 0 || count != 4U)
		return EINVAL;

	/* The four constants. */
	for (index = 0U; index < 4U; index++)
		record->values[index] = drv_i915_wire_read_u32(reader);
	if (reader->error != 0)
		return EINVAL;

	/* Succeeded: the constants are recorded. */
	return 0;
}

/*
 * vkCmdSetStencilCompareMask, vkCmdSetStencilWriteMask,
 * vkCmdSetStencilReference: [face mask][value], kept in values 0 and 1.
 */
static int
record_stencil(
	struct i915_wire_reader *reader,
	struct nvrtx_vk_record *record)
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
 * Reports how many dynamic offsets a bind of descriptor sets takes: one
 * for each descriptor of each dynamic binding of each set it binds.
 */
static uint32_t
record_dynamic_wanted(
	const struct nvrtx_vk_record *record)
{
	const struct nvrtx_vk_descriptor_set *set;
	uint32_t wanted;
	uint32_t index;

	/* Each set the bind names: the offsets its dynamic bindings take. */
	wanted = 0U;
	for (index = 0U; index < record->object_count; index++) {
		if (record->objects[index] == NULL)
			continue;
		set = record->objects[index]->payload;
		wanted += set->dynamic_total;
	}

	/* Succeeded: the offsets the sets take. */
	return wanted;
}

/*
 * vkCmdBindDescriptorSets: [bind point][layout][first set][count][count]
 * {set}[dynamic count][count]{offset}; graphics or compute sets, held, and
 * their dynamic offsets kept: as many as the sets' dynamic bindings take.
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
	uint32_t offset;
	uint32_t point;
	uint32_t wanted;
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
	if (point != VK_PIPELINE_BIND_POINT_GRAPHICS && point != VK_PIPELINE_BIND_POINT_COMPUTE)
		refused = ENOTSUP;
	record->values[0] = point;
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

	/* The dynamic offsets, the ones a record keeps. */
	(void)drv_i915_wire_read_u32(reader);
	offsets = drv_i915_wire_read_u64(reader);
	if (reader->error != 0 || offsets > NVRTX_VK_REGIONS_MAX)
		return EINVAL;
	for (index = 0U; index < offsets; index++) {
		offset = drv_i915_wire_read_u32(reader);
		if (index < NVRTX_VK_DYNAMIC_OFFSETS_MAX)
			record->dynamic[index] = offset;
	}
	if (reader->error != 0)
		return EINVAL;
	if (offsets > NVRTX_VK_DYNAMIC_OFFSETS_MAX && refused == 0)
		refused = ENOTSUP;
	record->dynamic_count = (uint32_t)offsets;

	/* Reports a bind that cannot be run, or whose offsets are not the ones its sets take. */
	record->count = (uint32_t)count;
	if (refused != 0)
		return refused;
	wanted = record_dynamic_wanted(record);
	if (wanted != record->dynamic_count)
		return ERANGE;

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
 * vkCmdDrawIndirect, vkCmdDrawIndexedIndirect: [buffer][offset][draw count]
 * [stride].  The buffer is the record's object 0, the offset its
 * offsets[0], the count its values[0], the stride its values[1].  ERANGE
 * (the recording fails) for an offset not of whole words, a count over
 * NVRTX_VK_DRAW_INDIRECT_MAX, a stride (of more than one draw) not of
 * whole words or shorter than a draw's record, or records past the
 * buffer's end.
 *
 * Values referenced: the Vulkan 1.3 specification, vkCmdDrawIndirect and
 * vkCmdDrawIndexedIndirect (VkDrawIndirectCommand of 16 bytes,
 * VkDrawIndexedIndirectCommand of 20, their valid usage).
 */
static int
record_draw_indirect(
	struct nvrtx_vk_session *session,
	uint32_t opcode,
	struct i915_wire_reader *reader,
	struct nvrtx_vk_record *record)
{
	const struct nvrtx_vk_buffer *buffer;
	uint64_t identity;
	uint64_t last;
	uint32_t bytes;
	int error;

	/* The buffer, where the first record is, how many and how far apart. */
	identity = drv_i915_wire_read_u64(reader);
	record->offsets[0] = drv_i915_wire_read_u64(reader);
	record->values[0] = drv_i915_wire_read_u32(reader);
	record->values[1] = drv_i915_wire_read_u32(reader);
	if (reader->error != 0)
		return EINVAL;

	/* Whole words, a count within the limit, records apart by one record at least. */
	bytes = (uint32_t)sizeof(VkDrawIndirectCommand);
	if (opcode == GPU_OP_CMD_DRAW_INDEXED_INDIRECT)
		bytes = (uint32_t)sizeof(VkDrawIndexedIndirectCommand);
	if ((record->offsets[0] & 3U) != 0U || record->values[0] > NVRTX_VK_DRAW_INDIRECT_MAX)
		return ERANGE;
	if (record->values[0] > 1U && ((record->values[1] & 3U) != 0U || record->values[1] < bytes))
		return ERANGE;

	/* The record holds the buffer, which must be known. */
	error = record_hold(session, record, I915_VK_OBJ_BUFFER, identity);
	if (error == 0 && record->object_count != 1U)
		error = ENOENT;
	if (error != 0)
		return error;

	/* Every record within the buffer. */
	if (record->values[0] != 0U) {
		buffer = record->objects[0]->payload;
		last = record->offsets[0] + (uint64_t)(record->values[0] - 1U) * record->values[1];
		if (record->offsets[0] > buffer->size || last > buffer->size || bytes > buffer->size - last)
			return ERANGE;
	}

	/* Succeeded: the draw is recorded; its records are read when it runs. */
	return 0;
}

/* vkCmdDispatch: [x][y][z], the group counts. */
static int
record_dispatch(
	struct i915_wire_reader *reader,
	struct nvrtx_vk_record *record)
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
 * vkCmdDispatchIndirect: [buffer][offset], where the group counts are
 * (a VkDispatchIndirectCommand, read when it runs).
 *
 * Values referenced: the Vulkan 1.0 specification (section 27.1,
 * vkCmdDispatchIndirect: the offset a multiple of 4, the 12 bytes of
 * VkDispatchIndirectCommand within the buffer).
 */
static int
record_dispatch_indirect(
	struct nvrtx_vk_session *session,
	struct i915_wire_reader *reader,
	struct nvrtx_vk_record *record)
{
	const struct nvrtx_vk_buffer *buffer;
	uint64_t identity;
	int error;

	/* The buffer and where the counts are. */
	identity = drv_i915_wire_read_u64(reader);
	record->offsets[0] = drv_i915_wire_read_u64(reader);
	if (reader->error != 0)
		return EINVAL;
	if ((record->offsets[0] & 3U) != 0U)
		return ERANGE;

	/* The record holds the buffer, which must be known. */
	error = record_hold(session, record, I915_VK_OBJ_BUFFER, identity);
	if (error == 0 && record->object_count != 1U)
		error = ENOENT;
	if (error != 0)
		return error;

	/* The counts within the buffer. */
	buffer = record->objects[0]->payload;
	if (record->offsets[0] > buffer->size || buffer->size - record->offsets[0] < (uint64_t)sizeof(VkDispatchIndirectCommand))
		return ERANGE;

	/* Succeeded: the dispatch is recorded; its counts are read when it runs. */
	return 0;
}

/*
 * vkCmdCopyBuffer: [source][destination][count][count]{VkBufferCopy};
 * vkCmdCopyImage, vkCmdBlitImage, vkCmdResolveImage: [source][layout]
 * [destination][layout][count][count]{VkImageCopy, VkImageBlit or
 * VkImageResolve} (and [filter] after a blit);
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
	uint64_t limit;
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
		if (opcode == GPU_OP_CMD_RESOLVE_IMAGE)
			element = sizeof(VkImageResolve);
	}

	/* How many regions follow (a copy more than a blit or a resolve). */
	(void)drv_i915_wire_read_u32(reader);
	count = drv_i915_wire_read_u64(reader);
	limit = NVRTX_VK_COPY_REGIONS_MAX;
	if (opcode == GPU_OP_CMD_BLIT_IMAGE || opcode == GPU_OP_CMD_RESOLVE_IMAGE)
		limit = NVRTX_VK_REGIONS_MAX;
	if (reader->error != 0 || count > limit)
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
		} else if (opcode == GPU_OP_CMD_RESOLVE_IMAGE) {
			i915_vkc_dec_VkImageResolve(reader, &session->arena, (VkImageResolve *)(void *)(regions + index * element));
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
 * vkCmdUpdateBuffer: [buffer][offset][size][count]{bytes, padded to
 * words}.  The buffer is the record's object 0, the offset its offsets[0],
 * the bytes its own run (regions, region_count of them).  EINVAL for a
 * count other than the size, or a size or offset not of whole words, or
 * more than Vulkan allows (65536 bytes).
 */
static int
record_update_buffer(
	struct nvrtx_vk_session *session,
	struct i915_wire_reader *reader,
	struct nvrtx_vk_record *record)
{
	uint64_t buffer;
	uint64_t offset;
	uint64_t size;
	uint64_t count;
	uint8_t *bytes;
	int error;

	/* The buffer, where in it and how many bytes. */
	buffer = drv_i915_wire_read_u64(reader);
	offset = drv_i915_wire_read_u64(reader);
	size = drv_i915_wire_read_u64(reader);
	count = drv_i915_wire_read_u64(reader);
	if (reader->error != 0 || count != size || size == 0U || size > RECORD_UPDATE_BYTES_MAX)
		return EINVAL;
	if ((size & 3U) != 0U || (offset & 3U) != 0U)
		return EINVAL;

	/* The bytes, into the record's own run. */
	bytes = kern_malloc((size_t)size);
	if (bytes == NULL)
		return ENOMEM;
	record->regions = bytes;
	record->region_count = (uint32_t)size;
	i915_vkc_read_bytes(reader, bytes, (size_t)size);
	if (reader->error != 0)
		return EINVAL;
	record->offsets[0] = offset;

	/* The record holds the buffer, which must be known. */
	error = record_hold(session, record, I915_VK_OBJ_BUFFER, buffer);
	if (error == 0 && record->object_count != 1U)
		error = ENOENT;
	if (error != 0)
		return error;

	/* Succeeded: the update is recorded. */
	return 0;
}

/*
 * vkCmdFillBuffer: [buffer][offset][size][word].  The buffer is the
 * record's object 0, the offset its offsets[0], the size its offsets[1]
 * (VK_WHOLE_SIZE as sent), the word its values[0].  EINVAL for an offset
 * not of whole words.
 */
static int
record_fill_buffer(
	struct nvrtx_vk_session *session,
	struct i915_wire_reader *reader,
	struct nvrtx_vk_record *record)
{
	uint64_t buffer;
	int error;

	/* The buffer, the range and the word. */
	buffer = drv_i915_wire_read_u64(reader);
	record->offsets[0] = drv_i915_wire_read_u64(reader);
	record->offsets[1] = drv_i915_wire_read_u64(reader);
	record->values[0] = drv_i915_wire_read_u32(reader);
	if (reader->error != 0 || (record->offsets[0] & 3U) != 0U)
		return EINVAL;

	/* The record holds the buffer, which must be known. */
	error = record_hold(session, record, I915_VK_OBJ_BUFFER, buffer);
	if (error == 0 && record->object_count != 1U)
		error = ENOENT;
	if (error != 0)
		return error;

	/* Succeeded: the fill is recorded. */
	return 0;
}

/*
 * vkCmdClearColorImage: [image][layout][present]{colour}[count][count]
 * {VkImageSubresourceRange}; vkCmdClearDepthStencilImage the same with
 * {[depth][stencil]} in place of the colour.  The image held, the colour's
 * four words kept (the depth's float bits and the stencil as values 0 and
 * 1), the ranges (the levels and layers cleared, and a depth clear's
 * aspects) into a run of the record's own.
 */
static int
record_clear(
	struct nvrtx_vk_session *session,
	uint32_t opcode,
	struct i915_wire_reader *reader,
	struct nvrtx_vk_record *record)
{
	VkImageSubresourceRange *ranges;
	uint64_t image;
	uint64_t present;
	uint64_t count;
	uint64_t index;
	int error;

	/* The image, its layout and the colour (or the depth and the stencil). */
	image = drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u32(reader);
	present = drv_i915_wire_read_u64(reader);
	if (reader->error != 0)
		return EINVAL;
	if (present != 0U && opcode == GPU_OP_CMD_CLEAR_DEPTH_STENCIL_IMAGE) {
		record->values[0] = drv_i915_wire_read_u32(reader);
		record->values[1] = drv_i915_wire_read_u32(reader);
	} else if (present != 0U) {
		error = record_colour(reader, record->values);
		if (error != 0)
			return error;
	}

	/* The ranges, into the record's own run. */
	(void)drv_i915_wire_read_u32(reader);
	count = drv_i915_wire_read_u64(reader);
	if (reader->error != 0 || count > NVRTX_VK_REGIONS_MAX)
		return EINVAL;
	ranges = NULL;
	if (count != 0U) {
		ranges = kern_calloc((size_t)count, sizeof(*ranges));
		if (ranges == NULL)
			return ENOMEM;
	}
	record->regions = ranges;
	record->region_count = (uint32_t)count;
	for (index = 0U; index < count; index++)
		i915_vkc_dec_VkImageSubresourceRange(reader, &session->arena, &ranges[index]);
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

/*
 * vkCmdClearAttachments: [count][count]{[aspects][colour attachment]
 * [clear]}[count][count]{VkClearRect}, a clear [0]{colour} or
 * [1][depth][stencil].  The aspects cleared are kept in first, the colour
 * attachments cleared (a bit each) in count, the depth (a float's bits)
 * in value 4 and the stencil in value 5; the rectangles go into a run of
 * the record's own, each colour attachment's four words after them
 * (NVRTX_EXEC_COLOURS of them).
 */
static int
record_clear_attachments(
	struct nvrtx_vk_session *session,
	struct i915_wire_reader *reader,
	struct nvrtx_vk_record *record)
{
	VkClearRect *rects;
	uint32_t colour[RECORD_COLOUR_WORDS];
	uint32_t colours[NVRTX_EXEC_COLOURS][RECORD_COLOUR_WORDS];
	uint64_t count;
	uint64_t index;
	uint32_t aspects;
	uint32_t attachment;
	uint32_t kind;
	uint32_t depth;
	uint32_t stencil;
	int refused;
	int error;

	/* The attachments: what each clears and to what. */
	refused = 0;
	depth = 0U;
	stencil = 0U;
	kern_memset(colour, 0, sizeof(colour));
	kern_memset(colours, 0, sizeof(colours));
	(void)drv_i915_wire_read_u32(reader);
	count = drv_i915_wire_read_u64(reader);
	if (reader->error != 0 || count > NVRTX_VK_REGIONS_MAX)
		return EINVAL;
	for (index = 0U; index < count; index++) {
		aspects = drv_i915_wire_read_u32(reader);
		attachment = drv_i915_wire_read_u32(reader);
		kind = drv_i915_wire_read_u32(reader);
		if (reader->error != 0)
			return EINVAL;

		/* A colour, or a depth and a stencil. */
		if (kind == 0U) {
			error = record_colour(reader, colour);
			if (error != 0)
				return error;
		} else if (kind == 1U) {
			depth = drv_i915_wire_read_u32(reader);
			stencil = drv_i915_wire_read_u32(reader);
			if (reader->error != 0)
				return EINVAL;
		} else {
			return EINVAL;
		}

		/* The colour of one of the subpass's colour attachments. */
		if ((aspects & VK_IMAGE_ASPECT_COLOR_BIT) != 0U) {
			if (kind != 0U)
				return EINVAL;
			if (attachment >= NVRTX_EXEC_COLOURS) {
				if (refused == 0)
					refused = ENOTSUP;
				continue;
			}
			kern_memcpy(colours[attachment], colour, sizeof(colour));
			record->count |= 1U << attachment;
			record->first |= VK_IMAGE_ASPECT_COLOR_BIT;
			continue;
		}

		/* The depth and the stencil. */
		if ((aspects & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)) != 0U && kind != 1U)
			return EINVAL;
		if ((aspects & VK_IMAGE_ASPECT_DEPTH_BIT) != 0U) {
			record->values[RECORD_CLEAR_DEPTH] = depth;
			record->first |= VK_IMAGE_ASPECT_DEPTH_BIT;
		}
		if ((aspects & VK_IMAGE_ASPECT_STENCIL_BIT) != 0U) {
			record->values[RECORD_CLEAR_STENCIL] = stencil;
			record->first |= VK_IMAGE_ASPECT_STENCIL_BIT;
		}
	}

	/* How many rectangles follow. */
	(void)drv_i915_wire_read_u32(reader);
	count = drv_i915_wire_read_u64(reader);
	if (reader->error != 0 || count > RECORD_CLEAR_RECTS_MAX)
		return EINVAL;

	/* The rectangles, into the record's own run, the colours after them. */
	rects = kern_calloc(1U, (size_t)count * sizeof(*rects) + sizeof(colours));
	if (rects == NULL)
		return ENOMEM;
	kern_memcpy(rects + count, colours, sizeof(colours));
	record->regions = rects;
	record->region_count = (uint32_t)count;
	for (index = 0U; index < count; index++)
		i915_vkc_dec_VkClearRect(reader, &session->arena, &rects[index]);
	if (reader->error != 0)
		return EINVAL;

	/* Layers of a rectangle: one at least (the executor clears those the pass has). */
	for (index = 0U; index < count && refused == 0; index++) {
		if (rects[index].layerCount == 0U)
			refused = EINVAL;
	}

	/* Reports a clear that cannot be run. */
	if (refused != 0)
		return refused;

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
 * vkCmdPipelineBarrier: the masks and the three barrier arrays, read; the
 * record keeps nothing of them.  A submission runs every record in order
 * and waits for the GPU's idle between the commands that need it, and at
 * a barrier after a dispatch (vk-execute.c).
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
 * object 0, the framebuffer its object 1; the render area is kept, and
 * each attachment's clear value, in the order of the attachments, in a
 * run of the record's own: four words each, a colour's, or a depth (a
 * float's bits) and its stencil.  A clear is [0]{colour} or
 * [1][depth][stencil].
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
	uint32_t (*clears)[RECORD_COLOUR_WORDS];
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

	/* The clear values, each attachment's into the record's own run. */
	clears = NULL;
	if (count != 0U) {
		clears = kern_calloc((size_t)count, sizeof(*clears));
		if (clears == NULL)
			return ENOMEM;
	}
	record->regions = clears;
	record->region_count = (uint32_t)count;
	for (index = 0U; index < count; index++) {
		kind = drv_i915_wire_read_u32(reader);
		if (reader->error != 0)
			return EINVAL;
		if (kind == 0U) {
			error = record_colour(reader, clears[index]);
			if (error != 0)
				return error;
		} else if (kind == 1U) {
			clears[index][0] = drv_i915_wire_read_u32(reader);
			clears[index][1] = drv_i915_wire_read_u32(reader);
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

/*
 * Records one of the commands every draw records (a draw, a pipeline, one
 * descriptor set, push constants) the short way, and the ones that follow
 * it for the same command buffer: each read in place from the stream into
 * the next record of a command buffer that is recording and has room,
 * each argument checked as the general way checks it.  The following ones
 * are taken without going through the stream's and the dispatcher's steps
 * again (they ask for no reply and use no scratch); the session's last
 * opcode names the last one taken.
 *
 * Returns false, with nothing read or held, when the first command is not
 * taken (another command, a stream too short, a command buffer that is not
 * recording or must grow, an argument the general way refuses or reads
 * otherwise): the general way then reads it and reports what it finds.
 * A following command that is not taken is left in the stream.
 */
static bool
record_quick(
	struct nvrtx_vk_session *session,
	uint32_t opcode,
	struct i915_wire_reader *reader)
{
	struct nvrtx_vk_command_buffer *command;
	const uint8_t *bytes;
	uint64_t identity;
	uint32_t requested;
	size_t left;
	size_t taken;
	bool recorded;

	/* Only a command the short way takes, its command buffer inside the stream. */
	recorded = record_quick_taken(opcode);
	if (!recorded || reader->error != 0)
		return false;
	left = reader->size - reader->offset;
	if (left < RECORD_QUICK_BUFFER)
		return false;
	bytes = reader->base + reader->offset;

	/* A command buffer that is recording. */
	identity = record_wide(bytes);
	command = nvrtx_vk_payload(session, I915_VK_OBJ_COMMAND_BUFFER, identity);
	if (command == NULL || command->state != NVRTX_VK_COMMAND_RECORDING)
		return false;

	/* The first command, which the general way reads when the short way does not. */
	recorded = record_quick_one(session, command, opcode, bytes + RECORD_QUICK_BUFFER, left - RECORD_QUICK_BUFFER, &taken);
	if (!recorded)
		return false;
	reader->offset += RECORD_QUICK_BUFFER + taken;

	/* The commands that follow for the same command buffer, without a reply, while the short way takes them. */
	for (;;) {
		left = reader->size - reader->offset;
		if (left < RECORD_QUICK_HEADER + RECORD_QUICK_BUFFER)
			break;
		bytes = reader->base + reader->offset;
		opcode = record_word(bytes);
		requested = record_word(bytes + 4U);
		if (requested != 0U || record_wide(bytes + 8U) != identity)
			break;
		recorded = record_quick_taken(opcode);
		if (!recorded)
			break;
		recorded = record_quick_one(session, command, opcode, bytes + RECORD_QUICK_HEADER + RECORD_QUICK_BUFFER,
					    left - RECORD_QUICK_HEADER - RECORD_QUICK_BUFFER, &taken);
		if (!recorded)
			break;
		reader->offset += RECORD_QUICK_HEADER + RECORD_QUICK_BUFFER + taken;
		session->last_opcode = opcode;
	}

	/* Succeeded: the first command, and those after it that were taken, are recorded. */
	return true;
}

/* Reports whether the short way takes an opcode at all. */
static bool
record_quick_taken(
	uint32_t opcode)
{
	/* The commands every draw records. */
	switch (opcode) {
	case GPU_OP_CMD_DRAW_INDEXED:
	case GPU_OP_CMD_DRAW:
	case GPU_OP_CMD_BIND_PIPELINE:
	case GPU_OP_CMD_BIND_DESCRIPTOR_SETS:
	case GPU_OP_CMD_PUSH_CONSTANTS:
		return true;
	default:
		break;
	}

	/* Another command: the general way's. */
	return false;
}

/*
 * Records one command the short way into the next record of a recording
 * command buffer, from its arguments (left bytes of the stream after its
 * command buffer).  *taken gets the bytes of its arguments.  Returns false,
 * with nothing held and the command buffer as it was, when the short way
 * does not take it.
 */
static bool
record_quick_one(
	struct nvrtx_vk_session *session,
	struct nvrtx_vk_command_buffer *command,
	uint32_t opcode,
	const uint8_t *arguments,
	size_t left,
	size_t *taken)
{
	struct nvrtx_vk_record *record;
	size_t needed;
	bool recorded;

	/* The fixed part of each command. */
	switch (opcode) {
	case GPU_OP_CMD_DRAW_INDEXED:
		needed = RECORD_QUICK_DRAW_INDEXED;
		break;
	case GPU_OP_CMD_DRAW:
		needed = RECORD_QUICK_DRAW;
		break;
	case GPU_OP_CMD_BIND_PIPELINE:
		needed = RECORD_QUICK_PIPELINE;
		break;
	case GPU_OP_CMD_BIND_DESCRIPTOR_SETS:
		needed = RECORD_QUICK_SETS;
		break;
	default:
		needed = RECORD_QUICK_PUSH;
		break;
	}

	/* The fixed part lies inside the stream; the array has room for one more record without growing. */
	if (left < needed)
		return false;
	if (command->record_count == command->record_capacity)
		return false;

	/* The record starts as the general way starts it, in its place at the end of the array. */
	record = &command->records[command->record_count];
	kern_memset(record, 0, sizeof(*record));
	record->opcode = opcode;

	/* The arguments of each command. */
	*taken = needed;
	switch (opcode) {
	case GPU_OP_CMD_DRAW_INDEXED:
		/* The index count, the instance count, the first index, the vertex offset and the first instance. */
		record->values[0] = record_word(arguments);
		record->values[1] = record_word(arguments + 4U);
		record->values[2] = record_word(arguments + 8U);
		record->values[3] = record_word(arguments + 12U);
		record->values[4] = record_word(arguments + 16U);
		recorded = true;
		break;
	case GPU_OP_CMD_DRAW:
		/* The vertex count, the instance count, the first vertex and the first instance. */
		record->values[0] = record_word(arguments);
		record->values[1] = record_word(arguments + 4U);
		record->values[2] = record_word(arguments + 8U);
		record->values[3] = record_word(arguments + 12U);
		recorded = true;
		break;
	case GPU_OP_CMD_BIND_PIPELINE:
		recorded = record_quick_pipeline(session, arguments, record);
		break;
	case GPU_OP_CMD_BIND_DESCRIPTOR_SETS:
		recorded = record_quick_sets(session, arguments, left, record, taken);
		break;
	default:
		recorded = record_quick_push(command, arguments, left, record, taken);
		break;
	}

	/* Anything the short way does not take is left (the record's place is written over later). */
	if (!recorded)
		return false;

	/* Succeeded: the record is the command buffer's. */
	command->record_count++;
	return true;
}

/* vkCmdBindPipeline the short way: [bind point][pipeline], a known graphics pipeline, held. */
static bool
record_quick_pipeline(
	struct nvrtx_vk_session *session,
	const uint8_t *bytes,
	struct nvrtx_vk_record *record)
{
	struct nvrtx_vk_object *object;
	uint32_t point;
	uint64_t pipeline;
	int error;

	/* The graphics bind point. */
	point = record_word(bytes);
	if (point != VK_PIPELINE_BIND_POINT_GRAPHICS)
		return false;

	/* A known graphics pipeline. */
	pipeline = record_wide(bytes + 4U);
	object = nvrtx_vk_object_find(session, I915_VK_OBJ_PIPELINE, pipeline);
	if (object == NULL || ((const struct nvrtx_vk_pipeline *)object->payload)->compute)
		return false;

	/* The record holds it. */
	error = nvrtx_vk_object_retain(object);
	if (error != 0)
		return false;
	record->objects[0] = object;
	record->object_count = 1U;

	/* Succeeded: the pipeline is recorded. */
	return true;
}

/*
 * vkCmdBindDescriptorSets the short way: [bind point][layout][first set]
 * [count][count = 1]{set}[dynamic count][count]{offset}, a known graphics
 * set in a slot a pipeline layout has, held, with as many dynamic offsets
 * (inside the stream, left bytes from the arguments) as it takes.  *taken
 * gets the bytes read after the command buffer.
 */
static bool
record_quick_sets(
	struct nvrtx_vk_session *session,
	const uint8_t *bytes,
	size_t left,
	struct nvrtx_vk_record *record,
	size_t *taken)
{
	struct nvrtx_vk_object *object;
	const struct nvrtx_vk_descriptor_set *set;
	uint32_t point;
	uint32_t first;
	uint32_t index;
	uint32_t wanted;
	uint64_t count;
	uint64_t identity;
	uint64_t offsets;
	int error;

	/* The graphics bind point, a first set inside the layouts' sets, one set and the dynamic offsets a record keeps. */
	point = record_word(bytes);
	first = record_word(bytes + 12U);
	count = record_wide(bytes + 20U);
	offsets = record_wide(bytes + 40U);
	if (point != VK_PIPELINE_BIND_POINT_GRAPHICS)
		return false;
	if (first >= NVRTX_VK_SETS_MAX || count != 1U || offsets > NVRTX_VK_DYNAMIC_OFFSETS_MAX)
		return false;
	if (left < RECORD_QUICK_SETS + (size_t)offsets * RECORD_QUICK_OFFSET)
		return false;

	/* A known set (a null one leaves its slot as it was, the general way's case) that takes as many offsets as the bind gives. */
	identity = record_wide(bytes + 28U);
	object = nvrtx_vk_object_find(session, I915_VK_OBJ_DESCRIPTOR_SET, identity);
	if (object == NULL)
		return false;
	set = object->payload;
	wanted = set->dynamic_total;
	if (wanted != offsets)
		return false;

	/* The record holds it as its object 0, set first. */
	error = nvrtx_vk_object_retain(object);
	if (error != 0)
		return false;
	record->objects[0] = object;
	record->object_count = 1U;
	record->first = first;
	record->count = 1U;

	/* Its dynamic offsets. */
	for (index = 0U; index < (uint32_t)offsets; index++)
		record->dynamic[index] = record_word(bytes + RECORD_QUICK_SETS + index * RECORD_QUICK_OFFSET);
	record->dynamic_count = (uint32_t)offsets;

	/* Succeeded: the set is recorded. */
	*taken = RECORD_QUICK_SETS + (size_t)offsets * RECORD_QUICK_OFFSET;
	return true;
}

/*
 * vkCmdPushConstants the short way: [layout][stages][offset][size][count]
 * {bytes, padded to words}, within the push constant bytes a program
 * reads, the bytes copied to the end of the command buffer's run (which
 * must have room without growing).  *taken gets the bytes read after the
 * command buffer.
 */
static bool
record_quick_push(
	struct nvrtx_vk_command_buffer *command,
	const uint8_t *bytes,
	size_t left,
	struct nvrtx_vk_record *record,
	size_t *taken)
{
	uint32_t offset;
	uint32_t size;
	uint64_t count;
	size_t padded;

	/* The range and the byte count: one count, within the bytes a program reads. */
	offset = record_word(bytes + 12U);
	size = record_word(bytes + 16U);
	count = record_wide(bytes + 20U);
	if (count != size || size > NVRTX_VK_PUSH_BYTES)
		return false;
	if (offset > NVRTX_VK_PUSH_BYTES || size > NVRTX_VK_PUSH_BYTES - offset)
		return false;

	/* The bytes, padded to words, lie inside the stream; the run has room for them. */
	padded = ((size_t)size + 3U) & ~(size_t)3U;
	if (padded > left - RECORD_QUICK_PUSH)
		return false;
	if (NVRTX_VK_PUSH_BYTES > command->push_capacity - command->push_used)
		return false;

	/* The bytes go to the end of the run, the record says where. */
	if (size != 0U)
		kern_memcpy(command->push_bytes + command->push_used, bytes + RECORD_QUICK_PUSH, size);
	record->first = offset;
	record->count = size;
	record->push_at = command->push_used;
	command->push_used += size;

	/* Succeeded: the push constants are recorded. */
	*taken = RECORD_QUICK_PUSH + padded;
	return true;
}

/* Reads one little-endian wire word at bytes. */
static uint32_t
record_word(
	const uint8_t *bytes)
{
	uint32_t word;

	/* Composes the word in wire order, which is little endian whatever the host is. */
	word = (uint32_t)bytes[0];
	word |= (uint32_t)bytes[1] << 8;
	word |= (uint32_t)bytes[2] << 16;
	word |= (uint32_t)bytes[3] << 24;
	return word;
}

/* Reads one little-endian 64-bit wire value at bytes, the low word first. */
static uint64_t
record_wide(
	const uint8_t *bytes)
{
	uint64_t low;
	uint64_t high;

	/* The two words. */
	low = record_word(bytes);
	high = record_word(bytes + 4U);
	return low | (high << 32);
}

/*
 * vkCmdDispatchBase: [base x][base y][base z][groups x][groups y][groups z];
 * the counts in values[0] to [2] as a dispatch's, the base in values[3]
 * to [5].
 */
static int
record_dispatch_base(
	struct i915_wire_reader *reader,
	struct nvrtx_vk_record *record)
{
	/* The base. */
	record->values[3] = drv_i915_wire_read_u32(reader);
	record->values[4] = drv_i915_wire_read_u32(reader);
	record->values[5] = drv_i915_wire_read_u32(reader);

	/* The three counts. */
	record->values[0] = drv_i915_wire_read_u32(reader);
	record->values[1] = drv_i915_wire_read_u32(reader);
	record->values[2] = drv_i915_wire_read_u32(reader);
	if (reader->error != 0)
		return EINVAL;

	/* Succeeded: the dispatch is recorded. */
	return 0;
}
