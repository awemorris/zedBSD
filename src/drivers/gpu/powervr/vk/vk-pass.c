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
#include "drivers/gpu/powervr/transfer-program.h"
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

static int create_pass(struct powervr_vk_session *session, struct i915_wire_reader *reader, struct i915_wire_writer *reply, bool version2);
static int create_framebuffer(struct powervr_vk_session *session, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static int framebuffer_view(struct powervr_vk_session *session, uint64_t identity, struct powervr_vk_object **held, struct powervr_pass_view *view);
static int framebuffer_destroy(struct powervr_vk_session *session, void *payload);
static void read_clear(struct i915_wire_reader *reader, struct powervr_pass_clear *value);
static int pass_depth_resolve(struct powervr_pass *pass, const struct i915_vkc_depth_resolve *resolve);
static int pass_views(const VkRenderPassCreateInfo *info, struct powervr_pass *pass);
static int pass_granularity(struct i915_wire_reader *reader, struct i915_wire_writer *reply);

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
		error = create_pass(session, reader, reply, false);
		break;
	case GPU_OP_CREATE_RENDER_PASS2:
		error = create_pass(session, reader, reply, true);
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
	case GPU_OP_GET_RENDER_AREA_GRANULARITY:
		error = pass_granularity(reader, reply);
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
 * create's tail -> the create's reply; with version2, vkCreateRenderPass2
 * (GPU_OP_CREATE_RENDER_PASS2), whose record is read in its Vulkan 1.0
 * form with the resolves of its depth and stencil aside.  A shape the
 * executor does not take is VK_ERROR_OUT_OF_HOST_MEMORY with a log line.
 */
static int
create_pass(
	struct powervr_vk_session *session,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply,
	bool version2)
{
	const VkSubpassDescriptionDepthStencilResolve *resolves;
	struct i915_vkc_depth_resolve resolve;
	VkRenderPassCreateInfo info;
	struct powervr_pass *pass;
	uint64_t identity;
	int error;

	/* The create info behind the device and its presence word. */
	kern_memset(&info, 0, sizeof(info));
	resolves = NULL;
	(void)drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u64(reader);
	if (version2)
		i915_vkc_read_render_pass2(reader, &session->arena, &info, &resolves);
	else
		i915_vkc_dec_VkRenderPassCreateInfo(reader, &session->arena, &info);
	identity = powervr_vk_create_tail(reader);
	if (reader->error != 0)
		return EINVAL;

	/* The pass, kept in its supported shape, with its subpass's views. */
	pass = kern_calloc(1, sizeof(*pass));
	error = ENOMEM;
	if (pass != NULL)
		error = powervr_pass_build(&info, pass);
	if (error == 0)
		error = pass_views(&info, pass);
	if (error == 0)
		error = i915_vkc_depth_resolve(&info, resolves, 0U, &resolve);
	if (error == 0 && resolve.target != VK_ATTACHMENT_UNUSED)
		error = pass_depth_resolve(pass, &resolve);
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
 * Keeps the subpass's resolve of sample 0 of its depth attachment (its
 * stencil with it: i915_vkc_depth_resolve() takes both aspects of a format
 * of both or neither): a depth attachment of four samples, a target of one
 * sample of its format, which the transfer resolves (as raw texels,
 * powervr_transfer_resolve_samples()); ENOTSUP for another.
 */
static int
pass_depth_resolve(
	struct powervr_pass *pass,
	const struct i915_vkc_depth_resolve *resolve)
{
	const struct powervr_format *format;

	/* The pass's own depth attachment, of four samples, into one of one sample. */
	if (resolve->source != pass->depth || resolve->target >= pass->attachment_count || pass->samples != 4U ||
	    pass->attachments[resolve->target].samples != 1U)
		return EINVAL;
	format = powervr_format_find(pass->attachments[resolve->target].format);
	if (format == NULL || powervr_transfer_resolve_samples(format) != 1U) {
		kern_logf("powervr: render pass: no resolve of depth or stencil of format %u\n", (unsigned)pass->attachments[resolve->target].format);
		return ENOTSUP;
	}
	pass->depth_resolve = resolve->target;

	/* Succeeded: the resolve is kept. */
	return 0;
}

/*
 * Keeps the views a multiview render pass's one subpass renders, at most
 * the device's (maxMultiviewViewCount); ENOTSUP for another shape.
 */
static int
pass_views(
	const VkRenderPassCreateInfo *info,
	struct powervr_pass *pass)
{
	const VkRenderPassMultiviewCreateInfo *multiview;

	/* None renders one view. */
	pass->view_mask = 0;
	multiview = i915_vkc_render_pass_multiview(info);
	if (multiview == NULL || multiview->subpassCount == 0)
		return 0;

	/* The one subpass's mask. */
	if (multiview->subpassCount != 1 || multiview->pViewMasks == NULL)
		return ENOTSUP;
	if ((multiview->pViewMasks[0] >> POWERVR_PASS_VIEWS) != 0)
		return ENOTSUP;
	pass->view_mask = multiview->pViewMasks[0];

	/* Succeeded: the views are kept. */
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
	view->layers = image_view->layer_count;
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

/*
 * The state the draws read, as the records set it: each set bound with
 * the dynamic offsets its bind gave it, the dynamic state, and the open
 * occlusion query's pool (NULL for none) and query.
 */
struct draw_state {
	const void *pipeline;
	const void *sets[POWERVR_DRAW_SETS];
	uint32_t set_offset_counts[POWERVR_DRAW_SETS];
	uint32_t set_offsets[POWERVR_DRAW_SETS][POWERVR_DRAW_DYNAMIC_OFFSETS];
	uint64_t vertex_addresses[POWERVR_DRAW_VERTEX_BUFFERS];
	uint64_t index_address;
	uint32_t index_bytes;
	VkViewport viewport;
	VkRect2D scissor;
	struct powervr_draw_dynamic dynamic;
	uint8_t push[POWERVR_DRAW_PUSH_BYTES];
	const struct powervr_vk_object *query_pool;
	uint32_t query;
};

static int state_apply(struct draw_state *state, const struct powervr_vk_record *entry);
static void state_sets(struct draw_state *state, const struct powervr_vk_record *entry);
static void state_stencil(uint32_t values[2], const struct powervr_vk_record *entry);
static int draw_add(struct powervr_render_job *job, const struct draw_state *state, const struct powervr_vk_record *entry);
static int job_refused(int error, const char *step, const struct powervr_render_job *job);
static uint32_t view_number(uint32_t views, uint32_t index);
static int view_framebuffer(const struct powervr_pass_framebuffer *framebuffer, uint32_t view, struct powervr_pass_framebuffer *layered);

/*
 * Turns a command buffer's render pass instances into render jobs, in
 * order: a begin starts one, a clear attachments applies to it, each
 * draw is added to it with the state the records set before it, an end
 * closes it.  An instance whose job refuses a partial area with a depth
 * store (EPERM) is dropped with its log line.  An instance with
 * more draws than a job has room for goes on in further jobs, each
 * loading what the one before stored (powervr_render_job_continue).  The
 * jobs are built in place.  A draw while an occlusion query is open
 * (from its begin to its end) counts in the query's visibility register
 * of its job.  Refuses more jobs or draws than room (E2BIG), records
 * outside an instance and draws without a pipeline (EINVAL), and what
 * the jobs do not take yet (ENOTSUP); a job's registers made before a
 * refusal stay with the job for the caller to let go.
 *
 * A multiview instance is built once for each view of its subpass, in
 * order of the views: from its begin to its end again with the state the
 * records had set at the begin, each time in jobs of the view (its
 * attachments at their views' layers plus the view, its draws' programs
 * reading the view as gl_ViewIndex).
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
	const struct powervr_pass *pass;
	struct powervr_render_job *job;
	struct draw_state state;
	struct draw_state begun;
	struct powervr_pass_framebuffer layered;
	uint32_t index;
	uint32_t begin_index;
	uint32_t views;
	uint32_t view;
	uint32_t slot;
	bool dropped;
	int error;

	/* Refuses missing input. */
	if (command == NULL || count == NULL || (room != 0 && jobs == NULL))
		return EINVAL;

	/* Each record in order, a multiview instance's again for each view. */
	*count = 0;
	job = NULL;
	dropped = false;
	begin_index = 0;
	views = 0;
	view = 0;
	slot = 0;
	kern_memset(&state, 0, sizeof(state));
	kern_memset(&begun, 0, sizeof(begun));
	index = 0;
	while (index < command->record_count) {
		entry = &command->records[index];
		if (entry->opcode == GPU_OP_CMD_BEGIN_RENDER_PASS) {
			/* A new instance (or its next view), built in the next job. */
			if (job != NULL)
				return EINVAL;
			if (*count == room)
				return E2BIG;
			job = &jobs[*count];
			begin = entry->data;
			pass = entry->objects[0]->payload;
			framebuffer = entry->objects[1]->payload;

			/* The first view of an instance keeps the state its records start from. */
			if (begin_index != index || views == 0) {
				begin_index = index;
				views = pass->view_mask;
				slot = 0;
				begun = state;
			}
			view = 0;
			if (views != 0)
				view = view_number(views, slot);
			error = view_framebuffer(&framebuffer->state, view, &layered);
			if (error != 0) {
				kern_logf("powervr: render pass: view %u has no layer in an attachment\n", (unsigned)view);
				return error;
			}
			error = powervr_render_job_begin(pass, &layered, &begin->area, begin->clears, begin->clear_count, job);
			job->view = view;
			dropped = false;
			if (error == EPERM) {
				kern_logf("powervr: render pass: partial render area with depth store not supported\n");
				dropped = true;
			} else if (error != 0) {
				return job_refused(error, "begin", job);
			}
		} else if (entry->opcode == GPU_OP_CMD_CLEAR_ATTACHMENTS) {
			/* A clear inside the instance. */
			if (job == NULL)
				return EINVAL;
			clear = entry->data;
			if (!dropped) {
				error = powervr_render_job_clear(job, clear->attachments, clear->count, clear->rects, clear->rect_count);
				if (error != 0)
					return job_refused(error, "clear", job);
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
					return job_refused(error, "continue", job);
				(*count)++;
				job = &jobs[*count];
			}
			if (!dropped) {
				error = draw_add(job, &state, entry);
				if (error != 0)
					return job_refused(error, "draw", job);
			}
		} else if (entry->opcode == GPU_OP_CMD_BEGIN_QUERY && powervr_vk_query_occlusion(entry->objects[0])) {
			/* An occlusion query the draws count for until its end. */
			state.query_pool = entry->objects[0];
			state.query = entry->values[0];
		} else if (entry->opcode == GPU_OP_CMD_END_QUERY && entry->objects[0] == state.query_pool) {
			/* The open occlusion query's end. */
			state.query_pool = NULL;
		} else if (entry->opcode == GPU_OP_CMD_END_RENDER_PASS) {
			/* The instance's end: its job, unless dropped. */
			if (job == NULL)
				return EINVAL;
			job = NULL;
			if (!dropped)
				(*count)++;

			/* A multiview instance's next view, from its begin with the state of then. */
			if (views != 0 && slot + 1U < (uint32_t)__builtin_popcount(views)) {
				slot++;
				state = begun;
				index = begin_index;
				continue;
			}
			views = 0;
		} else {
			/* State the draws read. */
			error = state_apply(&state, entry);
			if (error != 0)
				return error;
		}
		index++;
	}

	/* An instance left open is a client error. */
	if (job != NULL)
		return EINVAL;

	/* Succeeded: the jobs. */
	return 0;
}

/* The view number index (from zero) of a multiview mask names: its bit, counting the set bits from the lowest. */
static uint32_t
view_number(
	uint32_t views,
	uint32_t index)
{
	uint32_t view;

	/* Drops the lower set bits, then takes the lowest one left. */
	for (view = 0; view < index && views != 0; view++)
		views &= views - 1U;
	if (views == 0)
		return 0;

	/* Succeeded: the view's number. */
	return (uint32_t)__builtin_ctz(views);
}

/*
 * A framebuffer's attachments at one view of a multiview render pass:
 * each view's layer plus the view.  EINVAL when a view has no such layer
 * (Vulkan has the views hold more layers than the highest view).
 */
static int
view_framebuffer(
	const struct powervr_pass_framebuffer *framebuffer,
	uint32_t view,
	struct powervr_pass_framebuffer *layered)
{
	uint32_t index;

	/* The framebuffer as it is, and each attachment one layer on for each view. */
	*layered = *framebuffer;
	for (index = 0; index < layered->view_count && view != 0; index++) {
		if (view >= layered->views[index].layers)
			return EINVAL;
		layered->views[index].shape.base_layer += view;
	}

	/* Succeeded: the attachments of the view. */
	return 0;
}

/*
 * Says in the kernel log which step of a render pass instance's job was
 * refused, with the job's shape (its extent, colour formats, depth
 * format and draws so far), and gives the error back.
 */
static int
job_refused(
	int error,
	const char *step,
	const struct powervr_render_job *job)
{
	uint32_t formats[4];
	uint32_t index;

	/* The first four colour attachments' formats (0 for one not used). */
	for (index = 0; index < 4U; index++) {
		formats[index] = 0;
		if (index < job->color_count && job->colors[index].surface.format != NULL)
			formats[index] = job->colors[index].surface.format->vk_format;
	}
	kern_logf("powervr: vk: a render pass's %s was refused (%d): %ux%u, %u colour(s) of formats %u %u %u %u, depth of format %u, %u sample(s), %u draw(s)\n",
		  step, error, (unsigned)job->width, (unsigned)job->height, (unsigned)job->color_count, (unsigned)formats[0], (unsigned)formats[1],
		  (unsigned)formats[2], (unsigned)formats[3], (unsigned)(job->depth.present && job->depth.format != NULL ? job->depth.format->vk_format : 0U),
		  (unsigned)job->samples, (unsigned)job->draw_count);

	/* The refusal. */
	return error;
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
		state_sets(state, entry);
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
	} else if (entry->opcode == GPU_OP_CMD_SET_BLEND_CONSTANTS) {
		for (index = 0; index < 4U; index++)
			state->dynamic.blend_constants[index] = entry->values[index];
	} else if (entry->opcode == GPU_OP_CMD_SET_DEPTH_BIAS) {
		for (index = 0; index < 3U; index++)
			state->dynamic.depth_bias[index] = entry->values[index];
	} else if (entry->opcode == GPU_OP_CMD_SET_STENCIL_COMPARE_MASK) {
		state_stencil(state->dynamic.stencil_compare_mask, entry);
	} else if (entry->opcode == GPU_OP_CMD_SET_STENCIL_WRITE_MASK) {
		state_stencil(state->dynamic.stencil_write_mask, entry);
	} else if (entry->opcode == GPU_OP_CMD_SET_STENCIL_REFERENCE) {
		state_stencil(state->dynamic.stencil_reference, entry);
	}

	/* Succeeded: the state is up to date. */
	return 0;
}

/*
 * Applies a graphics bind of descriptor sets: each set from the first
 * takes its place, and the bind's dynamic offsets are handed out to the
 * sets in order, each taking as many as its layout has dynamic
 * descriptors (the record's count was checked against them).
 */
static void
state_sets(
	struct draw_state *state,
	const struct powervr_vk_record *entry)
{
	const struct powervr_vk_set_offsets *offsets;
	const struct powervr_vk_descriptor_set *set;
	uint32_t index;
	uint32_t slot;
	uint32_t count;
	uint32_t taken;
	uint32_t word;

	/* The offsets of the bind, none when it kept no data. */
	offsets = entry->data;
	taken = 0;

	/* Each set of the bind, with its share of the offsets. */
	for (index = 0; index < entry->object_count; index++) {
		slot = entry->first + index;
		if (entry->objects[index] == NULL || slot >= POWERVR_DRAW_SETS)
			continue;

		/* The set in its place. */
		set = entry->objects[index]->payload;
		state->sets[slot] = set;

		/* Its offsets, the next ones of the bind. */
		count = powervr_set_dynamic_count(&set->layout);
		if (offsets == NULL || taken + count > offsets->count)
			count = 0;
		for (word = 0; word < count; word++)
			state->set_offsets[slot][word] = offsets->offsets[taken + word];
		state->set_offset_counts[slot] = count;
		taken += count;
	}
}

/* Applies a stencil set to the faces its mask names: element 0 the front, 1 the back. */
static void
state_stencil(
	uint32_t values[2],
	const struct powervr_vk_record *entry)
{
	/* The front face. */
	if ((entry->values[0] & VK_STENCIL_FACE_FRONT_BIT) != 0)
		values[0] = entry->values[1];

	/* The back face. */
	if ((entry->values[0] & VK_STENCIL_FACE_BACK_BIT) != 0)
		values[1] = entry->values[1];
}

/* Adds a draw to a job with the state in force. */
static int
draw_add(
	struct powervr_render_job *job,
	const struct draw_state *state,
	const struct powervr_vk_record *entry)
{
	const struct powervr_vk_graphics *graphics;
	struct powervr_render_draw *draw;
	uint32_t slot;
	uint32_t word;
	int error;

	/* Refuses a draw without a pipeline and one past the job's room. */
	if (state->pipeline == NULL)
		return EINVAL;
	if (job->draw_count == POWERVR_RENDER_DRAWS)
		return E2BIG;

	/*
	 * A job without attachments rasterizes at its pipelines' samples (a
	 * subpass without attachments takes the rasterizationSamples of the
	 * pipelines it draws with): four when a rasterizing pipeline has them.
	 */
	graphics = state->pipeline;
	if (job->color_count == 0 && !job->depth.present && job->samples <= 1U &&
	    !graphics->fixed.rasterizer_discard && graphics->fixed.samples == 4U)
		job->samples = 4U;

	/* The draw and the state it reads. */
	draw = &job->draws[job->draw_count];
	kern_memset(draw, 0, sizeof(*draw));
	draw->pipeline = state->pipeline;
	kern_memcpy(draw->sets, state->sets, sizeof(draw->sets));
	kern_memcpy(draw->vertex_addresses, state->vertex_addresses, sizeof(draw->vertex_addresses));
	__builtin_memcpy(&draw->viewport, &state->viewport, sizeof(draw->viewport));
	draw->scissor = state->scissor;
	draw->dynamic = state->dynamic;
	draw->push_bytes = POWERVR_DRAW_PUSH_BYTES;
	kern_memcpy(draw->push, state->push, sizeof(draw->push));

	/* The bound sets' dynamic offsets, in the order of the sets. */
	for (slot = 0; slot < POWERVR_DRAW_SETS; slot++) {
		if (state->sets[slot] == NULL)
			continue;

		for (word = 0; word < state->set_offset_counts[slot]; word++) {
			if (draw->dynamic_offset_count == POWERVR_DRAW_DYNAMIC_OFFSETS)
				return ENOTSUP;
			draw->dynamic_offsets[draw->dynamic_offset_count] = state->set_offsets[slot][word];
			draw->dynamic_offset_count++;
		}
	}

	/* The counts: a draw's first vertex, an indexed draw's index buffer at its first index and its vertex offset. */
	draw->count = entry->values[0];
	draw->instances = entry->values[1];
	draw->first_instance = entry->values[3];
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

	/* The visibility register of the open occlusion query. */
	if (state->query_pool != NULL) {
		error = powervr_vk_query_visibility(job, state->query_pool, state->query, &draw->visibility);
		if (error != 0)
			return error;
	}

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

/*
 * vkGetRenderAreaGranularity: [device][render pass][present] ->
 * [present][VkExtent2D].  A render area may start at any pixel (the begin
 * takes any offset), so the answer is one pixel by one; the granularity
 * is a hint of efficiency only.
 *
 * Values referenced: the Vulkan 1.0 specification (section 7.4,
 * vkGetRenderAreaGranularity: the granularity is the size of the render
 * area's alignment that gives the best efficiency).
 */
static int
pass_granularity(
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	VkExtent2D extent;

	/* The device, the pass and the output's presence word. */
	(void)drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u64(reader);
	if (reader->error != 0)
		return EINVAL;

	/* One pixel by one. */
	extent.width = 1U;
	extent.height = 1U;
	drv_i915_wire_reply_u64(reply, 1U);
	i915_vkc_enc_VkExtent2D(reply, &extent);

	/* Succeeded: the reply carries the granularity. */
	return 0;
}

