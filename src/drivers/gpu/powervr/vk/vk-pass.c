/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The Venus executor's render passes and framebuffers, the records of a
 * render pass instance (begin, clear attachments) and the render jobs a
 * command buffer's records come to.
 * The jobs are what the render kick takes; nothing here kicks.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <kern/kcrt.h>
#include <kern/klog.h>
#include <kern/kmem.h>
#include <uapi/errno.h>
#include <uapi/gpu-op.h>

#include "drivers/gpu/powervr/render-pass.h"
#include "drivers/gpu/powervr/vk/vk-draw.h"
#include "drivers/gpu/powervr/vk/vk.h"

#include "drivers/gpu/i915/vk/vk-codec.inc"

/* The clear values one begin keeps (one an attachment of a pass). */
#define BEGIN_CLEARS_MAX	64U

/* A VkFramebuffer: the views it holds and what the jobs read of them. */
struct powervr_vk_framebuffer {
	struct powervr_vk_object *views[POWERVR_PASS_ATTACHMENTS];
	struct powervr_pass_framebuffer state;
};

/* The data of a begin record: the render area and the clear values. */
struct begin_data {
	struct powervr_render_area area;
	uint32_t clear_count;
	struct powervr_pass_clear clears[POWERVR_PASS_ATTACHMENTS];
};

/* The data of a clear attachments record. */
struct clear_data {
	uint32_t count;
	struct powervr_pass_clear_attachment attachments[POWERVR_PASS_ATTACHMENTS];
	uint32_t rect_count;
	VkClearRect rects[POWERVR_PASS_CLEAR_RECTS];
};

static int create_pass(struct powervr_vk_session *session, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static int create_framebuffer(struct powervr_vk_session *session, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static int framebuffer_view(struct powervr_vk_session *session, uint64_t identity, struct powervr_vk_object **held, struct powervr_pass_view *view);
static int framebuffer_destroy(struct powervr_vk_session *session, void *payload);
static void read_clear(struct i915_wire_reader *reader, struct powervr_pass_clear *value);

/*
 * Runs a render pass or framebuffer command; *handled stays clear for
 * another handler's opcode.
 */
int
powervr_vk_pass_route(
	struct powervr_vk_session *session,
	uint32_t opcode,
	uint32_t requested,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply,
	int *handled)
{
	int error;

	/* Picks the command, or leaves it to another handler. */
	(void)requested;
	*handled = 1;
	switch (opcode) {
	case GPU_OP_CREATE_RENDER_PASS:
		error = create_pass(session, reader, reply);
		break;
	case GPU_OP_DESTROY_RENDER_PASS:
		error = powervr_vk_destroy_plain(session, reader, I915_VK_OBJ_RENDER_PASS);
		break;
	case GPU_OP_CREATE_FRAMEBUFFER:
		error = create_framebuffer(session, reader, reply);
		break;
	case GPU_OP_DESTROY_FRAMEBUFFER:
		error = powervr_vk_destroy_plain(session, reader, I915_VK_OBJ_FRAMEBUFFER);
		break;
	default:
		*handled = 0;
		return 0;
	}

	/* A command that could not be decoded. */
	if (error != 0)
		return error;

	/* Succeeded: the command ran. */
	return 0;
}

/*
 * vkCreateRenderPass: [device][present][VkRenderPassCreateInfo] and the
 * create's tail -> the create's reply.  A shape the executor does not
 * take is VK_ERROR_OUT_OF_HOST_MEMORY with a log line.
 */
static int
create_pass(
	struct powervr_vk_session *session,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	VkRenderPassCreateInfo info;
	struct powervr_pass *pass;
	uint64_t identity;
	int error;

	/* The create info behind the device and its presence word. */
	kern_memset(&info, 0, sizeof(info));
	(void)drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u64(reader);
	i915_vkc_dec_VkRenderPassCreateInfo(reader, &session->arena, &info);
	identity = powervr_vk_create_tail(reader);
	if (reader->error != 0)
		return EINVAL;

	/* The pass, kept in its supported shape. */
	pass = kern_calloc(1, sizeof(*pass));
	error = ENOMEM;
	if (pass != NULL)
		error = powervr_pass_build(&info, pass);
	if (error == ENOBUFS && info.subpassCount != 1) {
		kern_logf("powervr: render pass: %u subpasses not supported\n", (unsigned)info.subpassCount);
	} else if (error == ENOBUFS) {
		kern_logf("powervr: render pass: %u attachments of this shape not supported\n", (unsigned)info.attachmentCount);
	}
	if (error != 0) {
		kern_free(pass);
		pass = NULL;
	}

	/* Publishes the pass and answers. */
	powervr_vk_create_reply(session, reply, I915_VK_OBJ_RENDER_PASS, identity, pass, powervr_vk_free_payload, error);

	/* Succeeded: the reply carries the create's result. */
	return 0;
}

/*
 * vkCreateFramebuffer: [device][present][VkFramebufferCreateInfo] and
 * the create's tail -> the create's reply.  The framebuffer holds its
 * views and keeps each one's shape for the jobs.
 */
static int
create_framebuffer(
	struct powervr_vk_session *session,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	VkFramebufferCreateInfo info;
	struct powervr_vk_framebuffer *framebuffer;
	uint64_t identity;
	uint64_t view;
	uint32_t index;
	int error;

	/* The create info behind the device and its presence word. */
	kern_memset(&info, 0, sizeof(info));
	(void)drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u64(reader);
	i915_vkc_dec_VkFramebufferCreateInfo(reader, &session->arena, &info);
	identity = powervr_vk_create_tail(reader);
	if (reader->error != 0)
		return EINVAL;

	/* No more attachments than a pass has, an extent the core renders. */
	error = 0;
	if (info.attachmentCount > POWERVR_PASS_ATTACHMENTS || info.width == 0 || info.height == 0 ||
	    info.width > POWERVR_VK_IMAGE_EDGE || info.height > POWERVR_VK_IMAGE_EDGE)
		error = ENOTSUP;

	/* The framebuffer and its views. */
	framebuffer = NULL;
	if (error == 0) {
		framebuffer = kern_calloc(1, sizeof(*framebuffer));
		if (framebuffer == NULL)
			error = ENOMEM;
	}
	if (error == 0) {
		framebuffer->state.width = info.width;
		framebuffer->state.height = info.height;
		framebuffer->state.layers = info.layers == 0 ? 1U : info.layers;
		for (index = 0; index < info.attachmentCount && error == 0; index++) {
			/* The decoded handles are the wire's 64-bit identities, eight bytes apart. */
			kern_memcpy(&view, (const char *)info.pAttachments + index * 8U, sizeof(view));
			error = framebuffer_view(session, view, &framebuffer->views[index], &framebuffer->state.views[index]);
			if (error == 0)
				framebuffer->state.view_count = index + 1U;
		}
		if (error != 0) {
			(void)framebuffer_destroy(session, framebuffer);
			framebuffer = NULL;
		}
	}

	/* Publishes the framebuffer and answers. */
	powervr_vk_create_reply(session, reply, I915_VK_OBJ_FRAMEBUFFER, identity, framebuffer, framebuffer_destroy, error);

	/* Succeeded: the reply carries the create's result. */
	return 0;
}

/* Holds one view of a framebuffer and fills its shape: the image's format, layout, address, level and layer. */
static int
framebuffer_view(
	struct powervr_vk_session *session,
	uint64_t identity,
	struct powervr_vk_object **held,
	struct powervr_pass_view *view)
{
	const struct powervr_vk_image_view *image_view;
	const struct powervr_vk_image *image;
	struct powervr_vk_object *object;
	int error;

	/* A view of the session over a bound image. */
	object = powervr_vk_object_find(session, I915_VK_OBJ_IMAGE_VIEW, identity);
	if (object == NULL)
		return EINVAL;
	image_view = object->payload;
	image = image_view->image->payload;

	/* A multi-planar image is never drawn into: its views show one plane each, which a pass does not take. */
	if (image->plane_count != 0)
		return ENOTSUP;

	/* The image's place in memory. */
	kern_memset(view, 0, sizeof(*view));
	error = powervr_vk_image_address(image, &view->shape.address);
	if (error != 0)
		return EINVAL;

	/* Its shape. */
	view->shape.format = image->format;
	view->shape.layout = &image->layout;
	view->shape.width = image->shape.width;
	view->shape.height = image->shape.height;
	view->shape.image_levels = image->shape.levels;
	view->shape.base_level = image_view->base_level;
	view->shape.level_count = 1;
	view->shape.base_layer = image_view->base_layer;
	view->shape.layer_count = 1;
	view->shape.linear = image->shape.linear;

	/* The framebuffer holds the view (and through it the image). */
	error = powervr_vk_object_retain(object);
	if (error != 0)
		return error;
	*held = object;

	/* Succeeded: the view's shape. */
	return 0;
}

/* Destroys a framebuffer nothing holds: it lets its views go. */
static int
framebuffer_destroy(
	struct powervr_vk_session *session,
	void *payload)
{
	struct powervr_vk_framebuffer *framebuffer;
	uint32_t index;

	/* Each held view, then the record. */
	(void)session;
	framebuffer = payload;
	for (index = 0; index < POWERVR_PASS_ATTACHMENTS; index++) {
		if (framebuffer->views[index] != NULL)
			(void)powervr_vk_object_release(framebuffer->views[index]);
	}
	kern_free(framebuffer);

	/* Succeeded: the framebuffer is gone. */
	return 0;
}

/*
 * vkCmdBeginRenderPass: [u64 1][u32 sType][u64 0][render pass]
 * [framebuffer][VkRect2D][u32][u64 clears]{clear value}[u32 contents].
 * The record holds the pass and the framebuffer and keeps the area and
 * the first clear values.
 *
 * Procedure referenced: i915/vk/vk-command.c (i915_record_begin_pass,
 * the same wire form).
 */
int
powervr_vk_record_begin_pass(
	struct powervr_vk_session *session,
	struct i915_wire_reader *reader,
	struct powervr_vk_record *entry)
{
	struct powervr_pass_clear value;
	struct powervr_vk_object *object;
	struct begin_data *data;
	VkRect2D area;
	uint64_t present;
	uint64_t pass;
	uint64_t framebuffer;
	uint64_t count;
	uint64_t index;
	int retained;

	/* The begin info: its pass, framebuffer and area. */
	present = drv_i915_wire_read_u64(reader);
	if (present == 0) {
		(void)drv_i915_wire_read_u32(reader);
		return EINVAL;
	}
	(void)drv_i915_wire_read_u32(reader);
	(void)drv_i915_wire_read_u64(reader);
	pass = drv_i915_wire_read_u64(reader);
	framebuffer = drv_i915_wire_read_u64(reader);
	kern_memset(&area, 0, sizeof(area));
	i915_vkc_dec_VkRect2D(reader, &session->arena, &area);

	/* The record's data. */
	data = kern_calloc(1, sizeof(*data));
	if (data == NULL)
		return ENOMEM;
	entry->data = data;

	/* The clear values, the first ones kept. */
	(void)drv_i915_wire_read_u32(reader);
	count = drv_i915_wire_read_u64(reader);
	if (reader->error != 0 || count > BEGIN_CLEARS_MAX)
		return EINVAL;
	for (index = 0; index < count; index++) {
		read_clear(reader, &value);
		if (index < POWERVR_PASS_ATTACHMENTS) {
			data->clears[index] = value;
			data->clear_count = (uint32_t)index + 1U;
		}
	}

	/* The contents. */
	(void)drv_i915_wire_read_u32(reader);
	if (reader->error != 0)
		return EINVAL;

	/* The area, refused when negative. */
	if (area.offset.x < 0 || area.offset.y < 0)
		return EINVAL;
	data->area.x = (uint32_t)area.offset.x;
	data->area.y = (uint32_t)area.offset.y;
	data->area.width = area.extent.width;
	data->area.height = area.extent.height;

	/* The record holds the pass and the framebuffer. */
	object = powervr_vk_object_find(session, I915_VK_OBJ_RENDER_PASS, pass);
	if (object == NULL)
		return EINVAL;
	retained = powervr_vk_object_retain(object);
	if (retained != 0)
		return EINVAL;
	entry->objects[entry->object_count++] = object;
	object = powervr_vk_object_find(session, I915_VK_OBJ_FRAMEBUFFER, framebuffer);
	if (object == NULL)
		return EINVAL;
	retained = powervr_vk_object_retain(object);
	if (retained != 0)
		return EINVAL;
	entry->objects[entry->object_count++] = object;

	/* Succeeded: the begin is recorded. */
	return 0;
}

/*
 * vkCmdClearAttachments: [u32][u64 count]{aspects, colour index, clear
 * value}[u32][u64 rects]{VkClearRect}.
 *
 * Procedure referenced: i915/vk/vk-command.c
 * (i915_record_clear_attachments, the same wire form).
 */
int
powervr_vk_record_clear_attachments(
	struct powervr_vk_session *session,
	struct i915_wire_reader *reader,
	struct powervr_vk_record *entry)
{
	struct clear_data *data;
	uint64_t count;
	uint64_t index;

	/* The record's data. */
	data = kern_calloc(1, sizeof(*data));
	if (data == NULL)
		return ENOMEM;
	entry->data = data;

	/* The attachments. */
	(void)drv_i915_wire_read_u32(reader);
	count = drv_i915_wire_read_u64(reader);
	if (reader->error != 0 || count > POWERVR_PASS_ATTACHMENTS)
		return EINVAL;
	for (index = 0; index < count; index++) {
		data->attachments[index].aspects = drv_i915_wire_read_u32(reader);
		data->attachments[index].color_index = drv_i915_wire_read_u32(reader);
		read_clear(reader, &data->attachments[index].value);
	}
	data->count = (uint32_t)count;

	/* The rectangles. */
	(void)drv_i915_wire_read_u32(reader);
	count = drv_i915_wire_read_u64(reader);
	if (reader->error != 0 || count > POWERVR_PASS_CLEAR_RECTS)
		return EINVAL;
	for (index = 0; index < count; index++)
		i915_vkc_dec_VkClearRect(reader, &session->arena, &data->rects[index]);
	data->rect_count = (uint32_t)count;
	if (reader->error != 0)
		return EINVAL;

	/* Succeeded: the clear is recorded. */
	return 0;
}

/* The state the draws read, as the records set it. */
struct draw_state {
	const void *pipeline;
	const void *sets[POWERVR_DRAW_SETS];
	uint64_t vertex_addresses[POWERVR_DRAW_VERTEX_BUFFERS];
	uint64_t index_address;
	uint32_t index_bytes;
	VkViewport viewport;
	VkRect2D scissor;
	uint8_t push[POWERVR_DRAW_PUSH_BYTES];
};

static int state_apply(struct draw_state *state, const struct powervr_vk_record *entry);
static int draw_add(struct powervr_render_job *job, const struct draw_state *state, const struct powervr_vk_record *entry);

/*
 * Turns a command buffer's render pass instances into render jobs, in
 * order: a begin starts one, a clear attachments applies to it, each
 * draw is added to it with the state the records set before it, an end
 * closes it.  An instance whose job refuses a partial area with a depth
 * store (EPERM) is dropped with its log line.  An instance with
 * more draws than a job has room for goes on in further jobs, each
 * loading what the one before stored (powervr_render_job_continue).  The
 * jobs are built in place.  Refuses more jobs or draws than room (E2BIG), records
 * outside an instance and draws without a pipeline (EINVAL), and what
 * the jobs do not take yet (ENOTSUP).
 */
int
powervr_vk_render_jobs(
	const struct powervr_vk_command_buffer *command,
	struct powervr_render_job *jobs,
	uint32_t room,
	uint32_t *count)
{
	const struct powervr_vk_record *entry;
	const struct powervr_vk_framebuffer *framebuffer;
	const struct begin_data *begin;
	const struct clear_data *clear;
	struct powervr_render_job *job;
	struct draw_state state;
	uint32_t index;
	bool dropped;
	int error;

	/* Refuses missing input. */
	if (command == NULL || count == NULL || (room != 0 && jobs == NULL))
		return EINVAL;

	/* Each record in order. */
	*count = 0;
	job = NULL;
	dropped = false;
	kern_memset(&state, 0, sizeof(state));
	for (index = 0; index < command->record_count; index++) {
		entry = &command->records[index];
		if (entry->opcode == GPU_OP_CMD_BEGIN_RENDER_PASS) {
			/* A new instance, built in the next job. */
			if (job != NULL)
				return EINVAL;
			if (*count == room)
				return E2BIG;
			job = &jobs[*count];
			begin = entry->data;
			framebuffer = entry->objects[1]->payload;
			error = powervr_render_job_begin(entry->objects[0]->payload, &framebuffer->state, &begin->area, begin->clears, begin->clear_count, job);
			dropped = false;
			if (error == EPERM) {
				kern_logf("powervr: render pass: partial render area with depth store not supported\n");
				dropped = true;
			} else if (error != 0) {
				return error;
			}
		} else if (entry->opcode == GPU_OP_CMD_CLEAR_ATTACHMENTS) {
			/* A clear inside the instance. */
			if (job == NULL)
				return EINVAL;
			clear = entry->data;
			if (!dropped) {
				error = powervr_render_job_clear(job, clear->attachments, clear->count, clear->rects, clear->rect_count);
				if (error != 0)
					return error;
			}
		} else if (entry->opcode == GPU_OP_CMD_DRAW || entry->opcode == GPU_OP_CMD_DRAW_INDEXED) {
			/* A draw inside the instance. */
			if (job == NULL)
				return EINVAL;
			if (!dropped && job->draw_count == POWERVR_RENDER_DRAWS) {
				/* A full job goes on in the next one, which loads what it stored. */
				if (*count + 1U >= room)
					return E2BIG;
				error = powervr_render_job_continue(job, &jobs[*count + 1U]);
				if (error != 0)
					return error;
				(*count)++;
				job = &jobs[*count];
			}
			if (!dropped) {
				error = draw_add(job, &state, entry);
				if (error != 0)
					return error;
			}
		} else if (entry->opcode == GPU_OP_CMD_END_RENDER_PASS) {
			/* The instance's end: its job, unless dropped. */
			if (job == NULL)
				return EINVAL;
			job = NULL;
			if (!dropped)
				(*count)++;
		} else {
			/* State the draws read. */
			error = state_apply(&state, entry);
			if (error != 0)
				return error;
		}
	}

	/* An instance left open is a client error. */
	if (job != NULL)
		return EINVAL;

	/* Succeeded: the jobs. */
	return 0;
}

/* Applies a state record to the draw state; other records leave it as it is. */
static int
state_apply(
	struct draw_state *state,
	const struct powervr_vk_record *entry)
{
	const struct powervr_vk_vertex_bind *vertex;
	const struct powervr_vk_state_record *data;
	const struct powervr_vk_buffer *buffer;
	uint64_t address;
	uint32_t index;
	int error;

	/* Each kind of state. */
	if (entry->opcode == POWERVR_VK_RECORD_BIND_GRAPHICS) {
		state->pipeline = ((const struct powervr_vk_pipeline *)entry->objects[0]->payload)->graphics;
	} else if (entry->opcode == POWERVR_VK_RECORD_SETS_GRAPHICS) {
		for (index = 0; index < entry->object_count; index++) {
			if (entry->objects[index] != NULL && entry->first + index < POWERVR_DRAW_SETS)
				state->sets[entry->first + index] = entry->objects[index]->payload;
		}
	} else if (entry->opcode == GPU_OP_CMD_BIND_VERTEX_BUFFERS) {
		vertex = entry->data;
		for (index = 0; index < vertex->count; index++) {
			buffer = entry->objects[index]->payload;
			error = powervr_vk_buffer_address(buffer, &address);
			if (error != 0)
				return EINVAL;
			state->vertex_addresses[vertex->first + index] = address + vertex->offsets[index];
		}
	} else if (entry->opcode == GPU_OP_CMD_BIND_INDEX_BUFFER) {
		buffer = entry->objects[0]->payload;
		error = powervr_vk_buffer_address(buffer, &address);
		if (error != 0)
			return EINVAL;
		state->index_address = address + ((uint64_t)entry->values[1] << 32 | entry->values[0]);
		state->index_bytes = entry->values[2];
	} else if (entry->opcode == GPU_OP_CMD_SET_VIEWPORT) {
		data = entry->data;
		__builtin_memcpy(&state->viewport, &data->viewport, sizeof(state->viewport));
	} else if (entry->opcode == GPU_OP_CMD_SET_SCISSOR) {
		data = entry->data;
		state->scissor = data->scissor;
	} else if (entry->opcode == GPU_OP_CMD_PUSH_CONSTANTS) {
		data = entry->data;
		kern_memcpy(&state->push[data->push_offset], data->push, data->push_bytes);
	}

	/* Succeeded: the state is up to date. */
	return 0;
}

/* Adds a draw to a job with the state in force. */
static int
draw_add(
	struct powervr_render_job *job,
	const struct draw_state *state,
	const struct powervr_vk_record *entry)
{
	struct powervr_render_draw *draw;

	/* Refuses a draw without a pipeline and one past the job's room. */
	if (state->pipeline == NULL)
		return EINVAL;
	if (job->draw_count == POWERVR_RENDER_DRAWS)
		return E2BIG;

	/* The draw and the state it reads. */
	draw = &job->draws[job->draw_count];
	kern_memset(draw, 0, sizeof(*draw));
	draw->pipeline = state->pipeline;
	kern_memcpy(draw->sets, state->sets, sizeof(draw->sets));
	kern_memcpy(draw->vertex_addresses, state->vertex_addresses, sizeof(draw->vertex_addresses));
	__builtin_memcpy(&draw->viewport, &state->viewport, sizeof(draw->viewport));
	draw->scissor = state->scissor;
	draw->push_bytes = POWERVR_DRAW_PUSH_BYTES;
	kern_memcpy(draw->push, state->push, sizeof(draw->push));

	/* The counts: a draw's first vertex, an indexed draw's index buffer at its first index and its vertex offset. */
	draw->count = entry->values[0];
	draw->instances = entry->values[1];
	if (entry->opcode == GPU_OP_CMD_DRAW) {
		draw->vertex_offset = entry->values[2];
	} else {
		if (state->index_bytes == 0)
			return EINVAL;
		draw->indexed = true;
		draw->index_bytes = state->index_bytes;
		draw->index_address = state->index_address + (uint64_t)entry->values[2] * state->index_bytes;
		draw->vertex_offset = entry->first;
	}

	/* An empty draw is no draw. */
	if (draw->count == 0 || draw->instances == 0)
		return 0;

	/* Succeeded: the draw is the job's. */
	job->draw_count++;
	return 0;
}

/* Reads one tagged clear value: a depth and stencil pair, or four colour words. */
static void
read_clear(
	struct i915_wire_reader *reader,
	struct powervr_pass_clear *value)
{
	uint64_t length;
	uint32_t word;

	/* The tag, then its words. */
	kern_memset(value, 0, sizeof(*value));
	value->is_depth = drv_i915_wire_read_u32(reader) != 0;
	if (value->is_depth) {
		value->words[0] = drv_i915_wire_read_u32(reader);
		value->words[1] = drv_i915_wire_read_u32(reader);
		return;
	}
	(void)drv_i915_wire_read_u32(reader);
	length = drv_i915_wire_read_u64(reader);
	if (length != 4)
		reader->error = 1;
	for (word = 0; word < 4; word++)
		value->words[word] = drv_i915_wire_read_u32(reader);
}
