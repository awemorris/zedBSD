/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The display list of a frame (ws213-p003, plan/ws213/phase001/phase.md
 * 2 and 4.4): the drawing (compose_record and everything it calls) writes
 * each Vulkan command it would record as an operation of the list, with
 * the arrays it was given copied into the list, and the list is replayed
 * into the frame's command buffer when the drawing ends.  The replay calls
 * the same commands with the same arguments in the same order, so the
 * command buffer is the one the drawing recorded directly before.
 *
 * The walk that writes the list and the replay that encodes it are timed
 * apart (KWL PERF walk), and a later step replays the list on a thread of
 * its own while the event loop goes on.
 */

#include "draw-list.h"

#include <stdlib.h>
#include <string.h>

/* The operations a list first makes room for, and the bytes of its data and of its vertices. */
#define LIST_FIRST_OPS		1024U
#define LIST_FIRST_DATA		(64U * 1024U)
#define LIST_FIRST_VERTICES	(64U * 1024U)
#define LIST_FIRST_COPIES	16U

/* The alignment of every array in a list's data: the largest of the Vulkan structures it holds. */
#define LIST_ALIGN		8U

static struct kwl_list_op *list_op(struct kwl_list *list, enum kwl_list_kind kind);
static int list_data(struct kwl_list *list, const void *source, size_t bytes, size_t *offset);
static int list_room(void **memory, size_t *capacity, size_t needed, size_t first);
static const void *list_at(const struct kwl_list *list, size_t offset, uint32_t count);
static void list_replay_op(const struct kwl_list *list, const struct kwl_list_op *op, VkCommandBuffer command);
static void list_replay_copy(const struct kwl_list *list, const struct kwl_list_op *op, VkCommandBuffer command);
static int list_pass_loads(VkRenderPass pass, const VkRenderPass *loading, unsigned loading_count);
static uint64_t list_copy_pixels(const struct kwl_list *list, const struct kwl_list_op *op);
static void list_pass_end(struct kwl_list_passes *passes, const VkRect2D *area, unsigned draws, int loads, unsigned job_draws);

/*
 * Empties a list for a new drawing, keeping its memory.
 *
 * A list never written is all zero and is reset like any other.
 */
void
kwl_list_reset(
	struct kwl_list *list)
{
	/* No operation, array, vertex or copy, and nothing counted. */
	list->op_count = 0U;
	list->data_used = 0U;
	list->vertex_used = 0U;
	list->copy_count = 0U;
	list->draws = 0U;
	list->joined = 0U;
	list->joined_draws = 0U;
	list->failed = 0U;

	/* A new list has no scissor set yet: whatever was known of the last one's is not of this one (ws211-p009). */
	list->scissor_serial++;
}

/* Frees a list's memory; the list is empty and may be written again. */
void
kwl_list_free(
	struct kwl_list *list)
{
	/* Each of its arrays. */
	free(list->ops);
	free(list->data);
	free(list->vertices);
	free(list->copies);
	memset(list, 0, sizeof(*list));
}

/*
 * Replays a list's operations into a command buffer that is recording.
 *
 * The vertices of the quads drawn together and the host images' copies
 * are not part of it: the caller writes the vertices where the draws read
 * them and records the copies in the command buffer that runs first.
 */
void
kwl_list_replay(
	const struct kwl_list *list,
	VkCommandBuffer command)
{
	size_t index;

	/* Each operation, in the order the drawing wrote it. */
	for (index = 0U; index < list->op_count; index++)
		list_replay_op(list, &list->ops[index], command);
}

/*
 * Reports how many things a list draws: its draws, less those that drew
 * quads together, and every quad so drawn (the count KWL DRAWS's draws
 * gives).
 */
unsigned
kwl_list_things(
	const struct kwl_list *list)
{
	unsigned things;

	/* The draws of one thing each, then the quads drawn together. */
	things = list->draws;
	if (things >= list->joined_draws)
		things -= list->joined_draws;
	things += list->joined;

	/* The number of things drawn. */
	return things;
}

/*
 * Counts what a list asks of the GPU: its passes, the jobs they take when
 * a job holds at most job_draws draws, the pixels of their areas and those
 * loaded, and its copies (KWL PASSES).
 *
 * A pass loads the image's pixels when its render pass is one of the
 * loading ones (count of them); every job after a pass's first loads its
 * area again, whatever the pass.
 */
void
kwl_list_count_passes(
	const struct kwl_list *list,
	const VkRenderPass *loading,
	unsigned loading_count,
	unsigned job_draws,
	struct kwl_list_passes *passes)
{
	const struct kwl_list_op *op;
	VkRect2D area;
	size_t index;
	unsigned draws;
	int loads;
	int open;

	/* Nothing counted yet, and no pass open. */
	memset(passes, 0, sizeof(*passes));
	memset(&area, 0, sizeof(area));
	draws = 0U;
	loads = 0;
	open = 0;

	/* Each operation in the order the GPU is given them. */
	for (index = 0U; index < list->op_count; index++) {
		op = &list->ops[index];
		if (op->kind == KWL_LIST_BEGIN_PASS) {
			/* A pass begins: its area and whether it keeps the image's pixels. */
			area = op->args.begin.area;
			loads = list_pass_loads(op->args.begin.pass, loading, loading_count);
			draws = 0U;
			open = 1;
		} else if (op->kind == KWL_LIST_DRAW) {
			/* One draw more, in the pass open (or in none, which the GPU refuses). */
			draws++;
			passes->draws++;
		} else if (op->kind == KWL_LIST_END_PASS) {
			/* The pass ends: its jobs and pixels are counted. */
			if (open)
				list_pass_end(passes, &area, draws, loads, job_draws);
			open = 0;
		} else if (op->kind == KWL_LIST_COPY_IMAGE ||
			   op->kind == KWL_LIST_COPY_BUFFER_TO_IMAGE ||
			   op->kind == KWL_LIST_COPY_IMAGE_TO_BUFFER ||
			   op->kind == KWL_LIST_BLIT) {
			/* A copy outside the passes, and the pixels it writes. */
			passes->copies++;
			passes->copy_pixels += list_copy_pixels(list, op);
		}
	}
}

/* Appends vertices of the quads drawn together, at the next byte of the list's vertices. */
void
kwl_list_add_vertices(
	struct kwl_list *list,
	const void *vertices,
	uint32_t bytes)
{
	size_t capacity;
	void *memory;
	int error;

	/* Room for them, grown when the frame draws more than before. */
	memory = list->vertices;
	capacity = list->vertex_capacity;
	error = list_room(&memory, &capacity, (size_t)list->vertex_used + bytes, LIST_FIRST_VERTICES);
	if (error != 0) {
		list->failed = 1U;
		return;
	}

	/* The grown array is the list's. */
	list->vertices = memory;
	list->vertex_capacity = (uint32_t)capacity;

	/* The vertices after those before them. */
	memcpy(list->vertices + list->vertex_used, vertices, bytes);
	list->vertex_used += bytes;
}

/*
 * Appends a host image's copy from its staging into its sampled image.
 *
 * The frame records the copy first.
 */
void
kwl_list_add_copy(
	struct kwl_list *list,
	const struct kwl_list_copy *copy)
{
	size_t capacity;
	void *memory;
	int error;

	/* Room for one more. */
	memory = list->copies;
	capacity = (size_t)list->copy_capacity * sizeof(*list->copies);
	error = list_room(&memory, &capacity, ((size_t)list->copy_count + 1U) * sizeof(*list->copies), LIST_FIRST_COPIES * sizeof(*list->copies));
	if (error != 0) {
		list->failed = 1U;
		return;
	}

	/* The grown array is the list's. */
	list->copies = memory;
	list->copy_capacity = (unsigned)(capacity / sizeof(*list->copies));

	/* The copy after those before it. */
	list->copies[list->copy_count] = *copy;
	list->copy_count++;
}

/* Writes the beginning of a render pass (vkCmdBeginRenderPass). */
void
kwl_list_begin_render_pass(
	struct kwl_list *list,
	const VkRenderPassBeginInfo *begin,
	VkSubpassContents contents)
{
	struct kwl_list_op *op;

	/* A pass begun leaves the scissor to be set again (ws211-p009). */
	list->scissor_serial++;

	/* The operation. */
	op = list_op(list, KWL_LIST_BEGIN_PASS);
	if (op == NULL)
		return;

	/* The pass, its framebuffer and area, and how it is drawn. */
	op->args.begin.pass = begin->renderPass;
	op->args.begin.framebuffer = begin->framebuffer;
	op->args.begin.area = begin->renderArea;
	op->args.begin.contents = contents;
	op->args.begin.clear_count = begin->clearValueCount;

	/* Its clear values. */
	(void)list_data(list, begin->pClearValues, (size_t)begin->clearValueCount * sizeof(VkClearValue), &op->args.begin.clears);
}

/* Writes the end of a render pass (vkCmdEndRenderPass). */
void
kwl_list_end_render_pass(
	struct kwl_list *list)
{
	/* The operation, which has no arguments (a failure leaves the list failed). */
	(void)list_op(list, KWL_LIST_END_PASS);
}

/* Writes viewports (vkCmdSetViewport). */
void
kwl_list_set_viewport(
	struct kwl_list *list,
	uint32_t first,
	uint32_t count,
	const VkViewport *viewports)
{
	struct kwl_list_op *op;

	/* The operation. */
	op = list_op(list, KWL_LIST_VIEWPORT);
	if (op == NULL)
		return;

	/* The first viewport's place. */
	op->args.range.first = first;
	op->args.range.count = count;

	/* The viewports. */
	(void)list_data(list, viewports, (size_t)count * sizeof(*viewports), &op->args.range.items);
}

/* Writes scissors (vkCmdSetScissor). */
void
kwl_list_set_scissor(
	struct kwl_list *list,
	uint32_t first,
	uint32_t count,
	const VkRect2D *scissors)
{
	struct kwl_list_op *op;

	/* Another scissor set: one known before is not the list's any more (ws211-p009). */
	list->scissor_serial++;

	/* The operation. */
	op = list_op(list, KWL_LIST_SCISSOR);
	if (op == NULL)
		return;

	/* The first scissor's place. */
	op->args.range.first = first;
	op->args.range.count = count;

	/* The scissors. */
	(void)list_data(list, scissors, (size_t)count * sizeof(*scissors), &op->args.range.items);
}

/* Writes a pipeline's bind (vkCmdBindPipeline). */
void
kwl_list_bind_pipeline(
	struct kwl_list *list,
	VkPipelineBindPoint point,
	VkPipeline pipeline)
{
	struct kwl_list_op *op;

	/* The operation. */
	op = list_op(list, KWL_LIST_PIPELINE);
	if (op == NULL)
		return;

	/* The bind point and the pipeline. */
	op->args.pipeline.point = point;
	op->args.pipeline.pipeline = pipeline;
}

/* Writes descriptor sets' bind (vkCmdBindDescriptorSets). */
void
kwl_list_bind_descriptor_sets(
	struct kwl_list *list,
	VkPipelineBindPoint point,
	VkPipelineLayout layout,
	uint32_t first,
	uint32_t count,
	const VkDescriptorSet *sets,
	uint32_t dynamic_count,
	const uint32_t *dynamic_offsets)
{
	struct kwl_list_op *op;
	int error;

	/* The operation. */
	op = list_op(list, KWL_LIST_SETS);
	if (op == NULL)
		return;

	/* The bind point, the layout and the first set's place. */
	op->args.sets.point = point;
	op->args.sets.layout = layout;
	op->args.sets.first = first;
	op->args.sets.count = count;
	op->args.sets.dynamic_count = dynamic_count;

	/* The sets. */
	error = list_data(list, sets, (size_t)count * sizeof(*sets), &op->args.sets.sets);
	if (error != 0)
		return;

	/* Their dynamic offsets. */
	(void)list_data(list, dynamic_offsets, (size_t)dynamic_count * sizeof(*dynamic_offsets), &op->args.sets.dynamic_offsets);
}

/* Writes push constants with their values (vkCmdPushConstants). */
void
kwl_list_push_constants(
	struct kwl_list *list,
	VkPipelineLayout layout,
	VkShaderStageFlags stages,
	uint32_t offset,
	uint32_t size,
	const void *values)
{
	struct kwl_list_op *op;

	/* The operation. */
	op = list_op(list, KWL_LIST_PUSH);
	if (op == NULL)
		return;

	/* The range of the constants. */
	op->args.push.layout = layout;
	op->args.push.stages = stages;
	op->args.push.offset = offset;
	op->args.push.size = size;

	/* The values, kept in the list (the caller's are on its stack). */
	(void)list_data(list, values, size, &op->args.push.values);
}

/* Writes vertex buffers' bind (vkCmdBindVertexBuffers). */
void
kwl_list_bind_vertex_buffers(
	struct kwl_list *list,
	uint32_t first,
	uint32_t count,
	const VkBuffer *buffers,
	const VkDeviceSize *offsets)
{
	struct kwl_list_op *op;
	int error;

	/* The operation. */
	op = list_op(list, KWL_LIST_VERTEX_BUFFERS);
	if (op == NULL)
		return;

	/* The first binding's place. */
	op->args.vertex_buffers.first = first;
	op->args.vertex_buffers.count = count;

	/* The buffers. */
	error = list_data(list, buffers, (size_t)count * sizeof(*buffers), &op->args.vertex_buffers.buffers);
	if (error != 0)
		return;

	/* Their offsets. */
	(void)list_data(list, offsets, (size_t)count * sizeof(*offsets), &op->args.vertex_buffers.offsets);
}

/* Writes a draw (vkCmdDraw), counted for the walk's timing. */
void
kwl_list_draw(
	struct kwl_list *list,
	uint32_t vertices,
	uint32_t instances,
	uint32_t first_vertex,
	uint32_t first_instance)
{
	struct kwl_list_op *op;

	/* The operation. */
	op = list_op(list, KWL_LIST_DRAW);
	if (op == NULL)
		return;

	/* The counts and the first of each. */
	op->args.draw.vertices = vertices;
	op->args.draw.instances = instances;
	op->args.draw.first_vertex = first_vertex;
	op->args.draw.first_instance = first_instance;

	/* One draw more in the frame (KWL PERF walk sorts the frames by it). */
	list->draws++;
}

/* Writes a pipeline barrier with its barriers (vkCmdPipelineBarrier). */
void
kwl_list_pipeline_barrier(
	struct kwl_list *list,
	VkPipelineStageFlags source,
	VkPipelineStageFlags destination,
	VkDependencyFlags dependency,
	uint32_t memory_count,
	const VkMemoryBarrier *memory,
	uint32_t buffer_count,
	const VkBufferMemoryBarrier *buffers,
	uint32_t image_count,
	const VkImageMemoryBarrier *images)
{
	struct kwl_list_op *op;
	int error;

	/* The operation. */
	op = list_op(list, KWL_LIST_BARRIER);
	if (op == NULL)
		return;

	/* Its stages and its counts. */
	op->args.barrier.source = source;
	op->args.barrier.destination = destination;
	op->args.barrier.dependency = dependency;
	op->args.barrier.memory_count = memory_count;
	op->args.barrier.buffer_count = buffer_count;
	op->args.barrier.image_count = image_count;

	/* The memory barriers. */
	error = list_data(list, memory, (size_t)memory_count * sizeof(*memory), &op->args.barrier.memory);
	if (error != 0)
		return;

	/* The buffer barriers. */
	error = list_data(list, buffers, (size_t)buffer_count * sizeof(*buffers), &op->args.barrier.buffers);
	if (error != 0)
		return;

	/* The image barriers. */
	(void)list_data(list, images, (size_t)image_count * sizeof(*images), &op->args.barrier.images);
}

/* Writes an image's copy into another (vkCmdCopyImage). */
void
kwl_list_copy_image(
	struct kwl_list *list,
	VkImage source,
	VkImageLayout source_layout,
	VkImage destination,
	VkImageLayout destination_layout,
	uint32_t count,
	const VkImageCopy *regions)
{
	struct kwl_list_op *op;

	/* The operation. */
	op = list_op(list, KWL_LIST_COPY_IMAGE);
	if (op == NULL)
		return;

	/* The two images. */
	op->args.images.source = source;
	op->args.images.source_layout = source_layout;
	op->args.images.destination = destination;
	op->args.images.destination_layout = destination_layout;
	op->args.images.count = count;

	/* The regions. */
	(void)list_data(list, regions, (size_t)count * sizeof(*regions), &op->args.images.regions);
}

/* Writes a buffer's copy into an image (vkCmdCopyBufferToImage). */
void
kwl_list_copy_buffer_to_image(
	struct kwl_list *list,
	VkBuffer buffer,
	VkImage image,
	VkImageLayout layout,
	uint32_t count,
	const VkBufferImageCopy *regions)
{
	struct kwl_list_op *op;

	/* The operation. */
	op = list_op(list, KWL_LIST_COPY_BUFFER_TO_IMAGE);
	if (op == NULL)
		return;

	/* The buffer and the image. */
	op->args.buffer_image.buffer = buffer;
	op->args.buffer_image.image = image;
	op->args.buffer_image.layout = layout;
	op->args.buffer_image.count = count;

	/* The regions. */
	(void)list_data(list, regions, (size_t)count * sizeof(*regions), &op->args.buffer_image.regions);
}

/* Writes an image's copy into a buffer (vkCmdCopyImageToBuffer). */
void
kwl_list_copy_image_to_buffer(
	struct kwl_list *list,
	VkImage image,
	VkImageLayout layout,
	VkBuffer buffer,
	uint32_t count,
	const VkBufferImageCopy *regions)
{
	struct kwl_list_op *op;

	/* The operation. */
	op = list_op(list, KWL_LIST_COPY_IMAGE_TO_BUFFER);
	if (op == NULL)
		return;

	/* The image and the buffer. */
	op->args.buffer_image.buffer = buffer;
	op->args.buffer_image.image = image;
	op->args.buffer_image.layout = layout;
	op->args.buffer_image.count = count;

	/* The regions. */
	(void)list_data(list, regions, (size_t)count * sizeof(*regions), &op->args.buffer_image.regions);
}

/* Writes an image's scaled copy into another (vkCmdBlitImage). */
void
kwl_list_blit_image(
	struct kwl_list *list,
	VkImage source,
	VkImageLayout source_layout,
	VkImage destination,
	VkImageLayout destination_layout,
	uint32_t count,
	const VkImageBlit *regions,
	VkFilter filter)
{
	struct kwl_list_op *op;

	/* The operation. */
	op = list_op(list, KWL_LIST_BLIT);
	if (op == NULL)
		return;

	/* The two images and the filter. */
	op->args.images.source = source;
	op->args.images.source_layout = source_layout;
	op->args.images.destination = destination;
	op->args.images.destination_layout = destination_layout;
	op->args.images.count = count;
	op->args.images.filter = filter;

	/* The regions. */
	(void)list_data(list, regions, (size_t)count * sizeof(*regions), &op->args.images.regions);
}

/* Writes an image's clear outside a pass (vkCmdClearColorImage). */
void
kwl_list_clear_color_image(
	struct kwl_list *list,
	VkImage image,
	VkImageLayout layout,
	const VkClearColorValue *color,
	uint32_t count,
	const VkImageSubresourceRange *ranges)
{
	struct kwl_list_op *op;

	/* The operation. */
	op = list_op(list, KWL_LIST_CLEAR);
	if (op == NULL)
		return;

	/* The image and the colour. */
	op->args.clear.image = image;
	op->args.clear.layout = layout;
	op->args.clear.color = *color;
	op->args.clear.count = count;

	/* The ranges. */
	(void)list_data(list, ranges, (size_t)count * sizeof(*ranges), &op->args.clear.ranges);
}

/* Writes a clear of rectangles of the pass's attachments, inside the pass (vkCmdClearAttachments). */
void
kwl_list_clear_attachments(
	struct kwl_list *list,
	uint32_t attachment_count,
	const VkClearAttachment *attachments,
	uint32_t rect_count,
	const VkClearRect *rects)
{
	struct kwl_list_op *op;

	/* The operation. */
	op = list_op(list, KWL_LIST_CLEAR_ATTACHMENTS);
	if (op == NULL)
		return;

	/* The attachments and their values. */
	op->args.attachments.attachment_count = attachment_count;
	(void)list_data(list, attachments, (size_t)attachment_count * sizeof(*attachments), &op->args.attachments.attachments);

	/* The rectangles. */
	op->args.attachments.rect_count = rect_count;
	(void)list_data(list, rects, (size_t)rect_count * sizeof(*rects), &op->args.attachments.rects);
}

/*
 * Appends an operation of a kind, all its arguments zero; returns NULL,
 * with the list failed, when there is no room and none can be made.
 */
static struct kwl_list_op *
list_op(
	struct kwl_list *list,
	enum kwl_list_kind kind)
{
	struct kwl_list_op *op;
	size_t capacity;
	void *memory;
	int error;

	/* A failed list takes nothing more: it is not replayed. */
	if (list->failed)
		return NULL;

	/* Room for one more operation. */
	memory = list->ops;
	capacity = list->op_capacity * sizeof(*list->ops);
	error = list_room(&memory, &capacity, (list->op_count + 1U) * sizeof(*list->ops), LIST_FIRST_OPS * sizeof(*list->ops));
	if (error != 0) {
		list->failed = 1U;
		return NULL;
	}

	/* The grown array is the list's. */
	list->ops = memory;
	list->op_capacity = capacity / sizeof(*list->ops);

	/* The new operation, after those before it. */
	op = &list->ops[list->op_count];
	memset(op, 0, sizeof(*op));
	op->kind = kind;
	list->op_count++;

	/* Succeeded: the caller fills its arguments. */
	return op;
}

/*
 * Copies an array of a command into the list's data and gives its offset
 * there; an empty array takes no room.  Returns -1, with the list failed,
 * when there is no room and none can be made.
 */
static int
list_data(
	struct kwl_list *list,
	const void *source,
	size_t bytes,
	size_t *offset)
{
	size_t capacity;
	size_t start;
	void *memory;
	int error;

	/* An empty array is at no place. */
	*offset = 0U;
	if (bytes == 0U)
		return 0;

	/* Room after the arrays before it, at the alignment of any structure. */
	start = (list->data_used + LIST_ALIGN - 1U) & ~(size_t)(LIST_ALIGN - 1U);
	memory = list->data;
	capacity = list->data_capacity;
	error = list_room(&memory, &capacity, start + bytes, LIST_FIRST_DATA);
	if (error != 0) {
		list->failed = 1U;
		return -1;
	}

	/* The grown array is the list's. */
	list->data = memory;
	list->data_capacity = capacity;

	/* The array, copied. */
	memcpy(list->data + start, source, bytes);
	list->data_used = start + bytes;
	*offset = start;

	/* Succeeded: the array is kept. */
	return 0;
}

/*
 * Makes an array at least as large as needed, doubling it from a first
 * size; returns -1 when it cannot be allocated (the array is as it was).
 */
static int
list_room(
	void **memory,
	size_t *capacity,
	size_t needed,
	size_t first)
{
	size_t size;
	void *grown;

	/* Large enough already. */
	if (needed <= *capacity)
		return 0;

	/* The new size: the first one, doubled until the need fits. */
	size = *capacity;
	if (size == 0U)
		size = first;
	while (size < needed)
		size *= 2U;

	/* The array grown, its contents kept. */
	grown = realloc(*memory, size);
	if (grown == NULL)
		return -1;

	/* Succeeded: the larger array. */
	*memory = grown;
	*capacity = size;
	return 0;
}

/* Gives an array of the list's data by its offset, or NULL for an empty one. */
static const void *
list_at(
	const struct kwl_list *list,
	size_t offset,
	uint32_t count)
{
	/* An empty array is passed as none, as the drawing may have passed it. */
	if (count == 0U)
		return NULL;

	/* The array in the data. */
	return list->data + offset;
}

/* Records one operation into the command buffer, with the arguments it was written with. */
static void
list_replay_op(
	const struct kwl_list *list,
	const struct kwl_list_op *op,
	VkCommandBuffer command)
{
	VkRenderPassBeginInfo begin;

	/* The Vulkan command of the operation's kind. */
	switch (op->kind) {
	case KWL_LIST_BEGIN_PASS:
		/* The pass's beginning, its clear values from the data. */
		memset(&begin, 0, sizeof(begin));
		begin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
		begin.renderPass = op->args.begin.pass;
		begin.framebuffer = op->args.begin.framebuffer;
		begin.renderArea = op->args.begin.area;
		begin.clearValueCount = op->args.begin.clear_count;
		begin.pClearValues = list_at(list, op->args.begin.clears, op->args.begin.clear_count);
		vkCmdBeginRenderPass(command, &begin, op->args.begin.contents);
		break;
	case KWL_LIST_END_PASS:
		vkCmdEndRenderPass(command);
		break;
	case KWL_LIST_VIEWPORT:
		vkCmdSetViewport(command, op->args.range.first, op->args.range.count, list_at(list, op->args.range.items, op->args.range.count));
		break;
	case KWL_LIST_SCISSOR:
		vkCmdSetScissor(command, op->args.range.first, op->args.range.count, list_at(list, op->args.range.items, op->args.range.count));
		break;
	case KWL_LIST_PIPELINE:
		vkCmdBindPipeline(command, op->args.pipeline.point, op->args.pipeline.pipeline);
		break;
	case KWL_LIST_SETS:
		vkCmdBindDescriptorSets(
			command,
			op->args.sets.point,
			op->args.sets.layout,
			op->args.sets.first,
			op->args.sets.count,
			list_at(list, op->args.sets.sets, op->args.sets.count),
			op->args.sets.dynamic_count,
			list_at(list, op->args.sets.dynamic_offsets, op->args.sets.dynamic_count));
		break;
	case KWL_LIST_PUSH:
		vkCmdPushConstants(
			command,
			op->args.push.layout,
			op->args.push.stages,
			op->args.push.offset,
			op->args.push.size,
			list_at(list, op->args.push.values, op->args.push.size));
		break;
	case KWL_LIST_VERTEX_BUFFERS:
		vkCmdBindVertexBuffers(
			command,
			op->args.vertex_buffers.first,
			op->args.vertex_buffers.count,
			list_at(list, op->args.vertex_buffers.buffers, op->args.vertex_buffers.count),
			list_at(list, op->args.vertex_buffers.offsets, op->args.vertex_buffers.count));
		break;
	case KWL_LIST_DRAW:
		vkCmdDraw(command, op->args.draw.vertices, op->args.draw.instances, op->args.draw.first_vertex, op->args.draw.first_instance);
		break;
	case KWL_LIST_BARRIER:
		vkCmdPipelineBarrier(
			command,
			op->args.barrier.source,
			op->args.barrier.destination,
			op->args.barrier.dependency,
			op->args.barrier.memory_count,
			list_at(list, op->args.barrier.memory, op->args.barrier.memory_count),
			op->args.barrier.buffer_count,
			list_at(list, op->args.barrier.buffers, op->args.barrier.buffer_count),
			op->args.barrier.image_count,
			list_at(list, op->args.barrier.images, op->args.barrier.image_count));
		break;
	case KWL_LIST_CLEAR_ATTACHMENTS:
		vkCmdClearAttachments(
			command,
			op->args.attachments.attachment_count,
			list_at(list, op->args.attachments.attachments, op->args.attachments.attachment_count),
			op->args.attachments.rect_count,
			list_at(list, op->args.attachments.rects, op->args.attachments.rect_count));
		break;
	default:
		/* The copies, the blit and the clear. */
		list_replay_copy(list, op, command);
		break;
	}
}

/* Records one copy, blit or clear into the command buffer, with the arguments it was written with. */
static void
list_replay_copy(
	const struct kwl_list *list,
	const struct kwl_list_op *op,
	VkCommandBuffer command)
{
	/* The Vulkan command of the operation's kind. */
	switch (op->kind) {
	case KWL_LIST_COPY_IMAGE:
		vkCmdCopyImage(
			command,
			op->args.images.source,
			op->args.images.source_layout,
			op->args.images.destination,
			op->args.images.destination_layout,
			op->args.images.count,
			list_at(list, op->args.images.regions, op->args.images.count));
		break;
	case KWL_LIST_COPY_BUFFER_TO_IMAGE:
		vkCmdCopyBufferToImage(
			command,
			op->args.buffer_image.buffer,
			op->args.buffer_image.image,
			op->args.buffer_image.layout,
			op->args.buffer_image.count,
			list_at(list, op->args.buffer_image.regions, op->args.buffer_image.count));
		break;
	case KWL_LIST_COPY_IMAGE_TO_BUFFER:
		vkCmdCopyImageToBuffer(
			command,
			op->args.buffer_image.image,
			op->args.buffer_image.layout,
			op->args.buffer_image.buffer,
			op->args.buffer_image.count,
			list_at(list, op->args.buffer_image.regions, op->args.buffer_image.count));
		break;
	case KWL_LIST_BLIT:
		vkCmdBlitImage(
			command,
			op->args.images.source,
			op->args.images.source_layout,
			op->args.images.destination,
			op->args.images.destination_layout,
			op->args.images.count,
			list_at(list, op->args.images.regions, op->args.images.count),
			op->args.images.filter);
		break;
	case KWL_LIST_CLEAR:
		vkCmdClearColorImage(
			command,
			op->args.clear.image,
			op->args.clear.layout,
			&op->args.clear.color,
			op->args.clear.count,
			list_at(list, op->args.clear.ranges, op->args.clear.count));
		break;
	default:
		/* The kinds list_replay_op records itself never come here. */
		break;
	}
}

/* Tells whether a render pass is one of those that keep the image's pixels (its colour attachment loaded). */
static int
list_pass_loads(
	VkRenderPass pass,
	const VkRenderPass *loading,
	unsigned loading_count)
{
	unsigned index;

	/* One of the loading passes the caller named. */
	for (index = 0U; index < loading_count; index++) {
		if (loading[index] != VK_NULL_HANDLE && loading[index] == pass)
			return 1;
	}

	/* A pass that clears, or whose old pixels do not matter. */
	return 0;
}

/* Counts the pixels a copy or a blit writes: each region's extent at the destination. */
static uint64_t
list_copy_pixels(
	const struct kwl_list *list,
	const struct kwl_list_op *op)
{
	const VkImageCopy *copies;
	const VkBufferImageCopy *buffer_copies;
	const VkImageBlit *blits;
	uint64_t pixels;
	int64_t width;
	int64_t height;
	uint32_t index;

	/* The regions of the copy's kind, their pixels added up. */
	pixels = 0U;
	if (op->kind == KWL_LIST_COPY_IMAGE) {
		/* An image's copy: the regions' extents. */
		copies = list_at(list, op->args.images.regions, op->args.images.count);
		for (index = 0U; index < op->args.images.count; index++)
			pixels += (uint64_t)copies[index].extent.width * copies[index].extent.height;
	} else if (op->kind == KWL_LIST_BLIT) {
		/* A blit: the destination's corners of each region. */
		blits = list_at(list, op->args.images.regions, op->args.images.count);
		for (index = 0U; index < op->args.images.count; index++) {
			width = (int64_t)blits[index].dstOffsets[1].x - blits[index].dstOffsets[0].x;
			height = (int64_t)blits[index].dstOffsets[1].y - blits[index].dstOffsets[0].y;
			if (width < 0)
				width = -width;
			if (height < 0)
				height = -height;
			pixels += (uint64_t)(width * height);
		}
	} else {
		/* A copy between a buffer and an image: the image's extents. */
		buffer_copies = list_at(list, op->args.buffer_image.regions, op->args.buffer_image.count);
		for (index = 0U; index < op->args.buffer_image.count; index++)
			pixels += (uint64_t)buffer_copies[index].imageExtent.width * buffer_copies[index].imageExtent.height;
	}

	/* The pixels written. */
	return pixels;
}

/*
 * Counts a pass that ended: one job, and one more for each further run of
 * job_draws draws, each of which loads the pass's area again; the pass's
 * own load when it keeps the image's pixels; and its line of the summary
 * while there is room for one.
 */
static void
list_pass_end(
	struct kwl_list_passes *passes,
	const VkRect2D *area,
	unsigned draws,
	int loads,
	unsigned job_draws)
{
	uint64_t pixels;
	unsigned jobs;
	unsigned loaded;

	/* The jobs the pass takes: one without draws, else one for each job's worth of them. */
	jobs = 1U;
	if (job_draws != 0U && draws > job_draws)
		jobs = (draws + job_draws - 1U) / job_draws;

	/* The loads: the pass's own when it keeps the pixels, and one for each job that goes on from another. */
	loaded = jobs - 1U;
	if (loads)
		loaded++;

	/* The pass's pixels, and those its jobs load. */
	pixels = (uint64_t)area->extent.width * area->extent.height;
	passes->passes++;
	passes->jobs += jobs;
	passes->area_pixels += pixels;
	passes->load_pixels += pixels * loaded;
	if (loads)
		passes->loading_passes++;

	/* The pass one by one, while there is room. */
	if (passes->detail_count < KWL_LIST_PASS_DETAIL) {
		passes->detail_areas[passes->detail_count] = *area;
		passes->detail_draws[passes->detail_count] = draws;
		passes->detail_loads[passes->detail_count] = (unsigned)loads;
		passes->detail_count++;
	}
}
