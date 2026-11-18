/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * OpenGL ES 3.1's compute dispatches and image units (ws101-p009); libGL
 * builds this file too, but its desktop GL offers no compute, so no
 * context of its reaches them.
 *
 * A dispatch is recorded into the draw surface's frame outside any render
 * pass, like transform feedback's copies: the current compute program's
 * pipeline, a descriptor set with its uniforms, named blocks and the
 * buffer ranges bound to its shader storage blocks' binding points, and
 * vkCmdDispatch (or vkCmdDispatchIndirect from the buffer bound to
 * GL_DISPATCH_INDIRECT_BUFFER).  A buffer bound to a block the shader may
 * write is then newer on the device than on the CPU (gpu_written), and
 * whatever reads its bytes first waits for the frame and reads them back
 * (gles_buffer_fetch): glMapBufferRange, glBufferSubData, copies.
 *
 * Each dispatch is fenced by barriers of its own: before it, from every
 * earlier write (draws, copies, dispatches) to its reads; after it, from
 * its writes to every later use (vertices, indices, uniforms, storage,
 * indirect sizes, copies, the host).  glMemoryBarrier has nothing left to
 * order and only checks its bits: GL leaves a shader's writes unordered
 * until the call, and ordering every dispatch is a stronger form of the
 * same promise.
 *
 * Image load and store: glBindImageTexture binds a level (every layer, or
 * one) of an immutable texture to an image unit with a format.  A
 * dispatch gives each image uniform of the program a storage image view
 * of its unit's level in the uniform's format, the image moved into the
 * general layout for the dispatch and back to the layout it is sampled in
 * after it (the barriers that order the writes); a level the shader may
 * write is newer on the device then (gpu_levels, as a framebuffer
 * object's drawing).  A unit's texture of another internal format of
 * the unit format's size is viewed in the unit's format (OpenGL ES 3.1's
 * compatibility by size; the image is mutable).  A unit whose binding the
 * uniform cannot access (no texture, a format of another size, a unit
 * format other than the uniform's, or a shape or level the image does
 * not have) has a 1x1 texture of the uniform's format in its descriptor
 * and its bit cleared in the shader's hidden uniform gl_ZedImages, so
 * that its accesses read zeros and write nothing, as GL's invalid
 * accesses do; a message says so on stderr once.
 *
 * With ZEDBSD_GLES_COMPUTE_TRACE set in the environment, each dispatch recorded
 * says so on stderr (ws101-p011: the evidence that a program such as Noct's
 * accelerator ran its kernel on the GPU and did not fall back to the CPU).
 */

#include "gles.h"
#include "glsl/glsl.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The bytes of a dispatch's three workgroup counts read from GL_DISPATCH_INDIRECT_BUFFER. */
#define COMPUTE_INDIRECT_SIZE	12U

/* The barrier bits glMemoryBarrierByRegion takes (those of what fragments read). */
#define COMPUTE_REGION_BITS \
	(GL_ATOMIC_COUNTER_BARRIER_BIT | GL_FRAMEBUFFER_BARRIER_BIT | GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | \
	 GL_SHADER_STORAGE_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT | GL_UNIFORM_BARRIER_BIT)

/* Every barrier bit OpenGL ES 3.1 names. */
#define COMPUTE_BARRIER_BITS \
	(GL_VERTEX_ATTRIB_ARRAY_BARRIER_BIT | GL_ELEMENT_ARRAY_BARRIER_BIT | GL_UNIFORM_BARRIER_BIT | \
	 GL_TEXTURE_FETCH_BARRIER_BIT | GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_COMMAND_BARRIER_BIT | \
	 GL_PIXEL_BUFFER_BARRIER_BIT | GL_TEXTURE_UPDATE_BARRIER_BIT | GL_BUFFER_UPDATE_BARRIER_BIT | \
	 GL_FRAMEBUFFER_BARRIER_BIT | GL_TRANSFORM_FEEDBACK_BARRIER_BIT | GL_ATOMIC_COUNTER_BARRIER_BIT | \
	 GL_SHADER_STORAGE_BARRIER_BIT)

static void compute_dispatch(const GLuint *groups, GLintptr indirect, int is_indirect);
static void compute_run(struct zegl_context *context, struct gles_state *state, struct gles_program *program, const GLuint *groups, GLintptr indirect, int is_indirect);
static struct gles_program *compute_program(struct zegl_context *context, struct gles_state *state);
static int compute_indirect_check(struct zegl_context *context, struct gles_state *state, GLintptr indirect, int *skip);
static int compute_storages(struct zegl_context *context, struct gles_state *state, const struct gles_program *program, VkDescriptorBufferInfo *storages);
static void compute_barriers(VkCommandBuffer command, int after);
static void compute_memory_barrier(GLbitfield barriers, GLbitfield allowed);
static void compute_trace(const GLuint *groups, GLintptr indirect, int is_indirect);
static int compute_images(struct gles_state *state, const struct gles_program *program, VkDescriptorImageInfo *infos, struct gles_texture **textures, unsigned *texture_count, uint32_t *valid_units);
static struct gles_texture *compute_image_binding(struct gles_state *state, const struct gles_image *image, uint32_t *level, uint32_t *layer, VkImageViewType *type, uint32_t *faces, int *valid);
static int compute_image_view_fits(struct gles_state *state, const struct gles_texture *texture, VkFormat format);
static void compute_image_layouts(VkCommandBuffer command, struct gles_texture **textures, unsigned count, int after);
static int compute_image_format_index(GLenum format);

/*
 * The formats an image unit takes (OpenGL ES 3.1's table 8.27), in the
 * order of the GLSL compiler's GLSL_IMAGE_FORMAT_* (from 1), with their
 * Vulkan formats and the size of a texel in bytes (table 8.28's Size:
 * two formats of a size are compatible).
 */
static const struct {
	GLenum gl;
	VkFormat vk;
	unsigned bytes;
} compute_formats[GLES_IMAGE_FORMATS] = {
	{ GL_RGBA32F, VK_FORMAT_R32G32B32A32_SFLOAT, 16U },
	{ GL_RGBA16F, VK_FORMAT_R16G16B16A16_SFLOAT, 8U },
	{ GL_R32F, VK_FORMAT_R32_SFLOAT, 4U },
	{ GL_RGBA8, VK_FORMAT_R8G8B8A8_UNORM, 4U },
	{ GL_RGBA8_SNORM, VK_FORMAT_R8G8B8A8_SNORM, 4U },
	{ GL_RGBA32I, VK_FORMAT_R32G32B32A32_SINT, 16U },
	{ GL_RGBA16I, VK_FORMAT_R16G16B16A16_SINT, 8U },
	{ GL_RGBA8I, VK_FORMAT_R8G8B8A8_SINT, 4U },
	{ GL_R32I, VK_FORMAT_R32_SINT, 4U },
	{ GL_RGBA32UI, VK_FORMAT_R32G32B32A32_UINT, 16U },
	{ GL_RGBA16UI, VK_FORMAT_R16G16B16A16_UINT, 8U },
	{ GL_RGBA8UI, VK_FORMAT_R8G8B8A8_UINT, 4U },
	{ GL_R32UI, VK_FORMAT_R32_UINT, 4U }
};

/* Whether a unit read as a 1x1 texture was reported (once a process). */
static int compute_image_reported;

/*
 * Whether dispatches are traced: -1 until the environment is read, then 0
 * or 1 for the life of the process.
 */
static int compute_tracing = -1;

/*
 * Runs the current compute program over a grid of workgroups.
 */
GL_APICALL void GL_APIENTRY
glDispatchCompute(
	GLuint num_groups_x,
	GLuint num_groups_y,
	GLuint num_groups_z)
{
	GLuint groups[3];
	uint64_t started;

	/* The grid's three counts; the recording is timed when ZEDBSD_GLES_COMPUTE_TRACE is 2. */
	groups[0] = num_groups_x;
	groups[1] = num_groups_y;
	groups[2] = num_groups_z;
	started = gles_time_begin();
	compute_dispatch(groups, 0, 0);
	gles_time_end("dispatch-record", started, 0U);
}

/*
 * Runs the current compute program over the grid the buffer bound to
 * GL_DISPATCH_INDIRECT_BUFFER gives at an offset (three unsigned counts).
 */
GL_APICALL void GL_APIENTRY
glDispatchComputeIndirect(
	GLintptr indirect)
{
	GLuint groups[3];

	/* The counts are the buffer's. */
	memset(groups, 0, sizeof(groups));
	compute_dispatch(groups, indirect, 1);
}

/*
 * Orders shader writes before later uses of what they wrote: each
 * dispatch already records the barriers after it, so only the bits are
 * checked.
 */
GL_APICALL void GL_APIENTRY
glMemoryBarrier(
	GLbitfield barriers)
{
	/* Any of OpenGL ES 3.1's bits, or all of them. */
	compute_memory_barrier(barriers, COMPUTE_BARRIER_BITS);
}

/*
 * Orders shader writes before later fragments' reads of the same region:
 * as glMemoryBarrier, of the bits of what fragments read.
 */
GL_APICALL void GL_APIENTRY
glMemoryBarrierByRegion(
	GLbitfield barriers)
{
	/* The bits of what fragments read, or all of them. */
	compute_memory_barrier(barriers, COMPUTE_REGION_BITS);
}

/*
 * Records a dispatch of the current compute program (without one, of the
 * bound program pipeline object's compute stage, current for the
 * dispatch only): the grid's counts, or (is_indirect) the ones at an
 * offset of the buffer bound to GL_DISPATCH_INDIRECT_BUFFER.  Errors are
 * recorded.
 */
static void
compute_dispatch(
	const GLuint *groups,
	GLintptr indirect,
	int is_indirect)
{
	struct zegl_context *context;
	struct gles_state *state;
	struct gles_program *program;

	/* A context with its state, and a linked compute program. */
	context = gles_context();
	state = gles_state(context);
	if (state == NULL)
		return;
	program = compute_program(context, state);
	if (program == NULL)
		return;

	/* The current program's dispatch. */
	if (state->program == program) {
		compute_run(context, state, program, groups, indirect, is_indirect);
		return;
	}

	/* The pipeline's compute stage, current for the dispatch. */
	state->program = program;
	compute_run(context, state, program, groups, indirect, is_indirect);
	state->program = NULL;
}

/* Records a dispatch of a compute program current for it (compute_dispatch). */
static void
compute_run(
	struct zegl_context *context,
	struct gles_state *state,
	struct gles_program *program,
	const GLuint *groups,
	GLintptr indirect,
	int is_indirect)
{
	struct zegl_surface *surface;
	struct gles_buffer *sizes;
	VkDescriptorBufferInfo blocks[GLES_NAMED_BLOCKS];
	VkDescriptorBufferInfo storages[GLES_STORAGE_BINDINGS];
	VkDescriptorImageInfo images[GLES_IMAGE_UNITS];
	struct gles_texture *image_textures[GLES_IMAGE_UNITS];
	unsigned image_texture_count;
	uint32_t valid_units;
	VkDescriptorSet set;
	uint32_t dynamic_offset;
	uint32_t dynamic_count;
	unsigned index;
	EGLint error;
	int status;
	int skip;

	/* Direct: counts no larger than the device's; a grid without workgroups runs nothing. */
	skip = 0;
	if (!is_indirect) {
		for (index = 0U; index < 3U; index++) {
			if (groups[index] > state->limits.maxComputeWorkGroupCount[index]) {
				gles_error(context, GL_INVALID_VALUE);
				return;
			}

			/* An empty dimension empties the grid. */
			if (groups[index] == 0U)
				skip = 1;
		}
	}

	/* Indirect: the sizes' buffer and offset. */
	if (is_indirect) {
		status = compute_indirect_check(context, state, indirect, &skip);
		if (status != 0)
			return;
	}

	/* The frame the dispatch is recorded in: the draw surface's. */
	surface = context->draw;
	if (surface == NULL) {
		gles_error(context, GL_INVALID_OPERATION);
		return;
	}

	/* Nothing to run. */
	if (skip)
		return;

	/* The buffer ranges the storage blocks and the named uniform blocks read. */
	status = compute_storages(context, state, program, storages);
	if (status != 0)
		return;
	status = gles_draw_blocks(context, state, blocks);
	if (status != 0)
		return;

	/* The frame, open, with no pass open in it (a dispatch is recorded outside one). */
	gles_target_close(state);
	error = zegl_frame_begin(surface);
	if (error != EGL_SUCCESS) {
		gles_error(context, GL_OUT_OF_MEMORY);
		return;
	}

	/* An occlusion query's segment and the surface's pass end. */
	gles_queries_suspend(state);
	zegl_frame_leave_pass(surface);

	/* The indirect sizes' device copy, up to date, which this frame reads. */
	sizes = NULL;
	if (is_indirect) {
		sizes = state->dispatch_buffer;
		status = gles_buffer_sync(state, sizes);
		if (status != 0 || sizes->buffer == VK_NULL_HANDLE) {
			gles_error(context, GL_OUT_OF_MEMORY);
			return;
		}

		/* This frame reads it: a later change does not write it in place. */
		sizes->used = state->frame;
	}

	/* The image units' views, their textures in the general layout while the descriptors are written. */
	image_texture_count = 0U;
	status = compute_images(state, program, images, image_textures, &image_texture_count, &valid_units);
	if (status != 0) {
		gles_error(context, GL_OUT_OF_MEMORY);
		return;
	}

	/* The units the shader's image accesses may touch, in its hidden uniform gl_ZedImages. */
	if (program->zed_images >= 0 && program->uniform_data != NULL)
		memcpy(program->uniform_data + program->zed_images, &valid_units, sizeof(valid_units));

	/* The descriptors. */
	set = gles_draw_descriptors(state, NULL, blocks, NULL, storages, images, &dynamic_offset);
	for (index = 0U; index < image_texture_count; index++)
		image_textures[index]->general = 0;
	if (set == VK_NULL_HANDLE) {
		gles_report("the dispatch's descriptors", -1);
		gles_error(context, GL_OUT_OF_MEMORY);
		return;
	}

	/* Earlier writes seen by the dispatch, the images in the general layout. */
	compute_barriers(surface->command, 0);
	compute_image_layouts(surface->command, image_textures, image_texture_count, 0);

	/* The recording: pipeline, descriptors (the uniform block at its dynamic offset), the dispatch. */
	vkCmdBindPipeline(surface->command, VK_PIPELINE_BIND_POINT_COMPUTE, program->compute_pipeline);
	dynamic_count = 0U;
	if (program->uniform_data != NULL)
		dynamic_count = 1U;
	vkCmdBindDescriptorSets(surface->command, VK_PIPELINE_BIND_POINT_COMPUTE, program->layout, 0U, 1U, &set, dynamic_count,
				&dynamic_offset);
	if (is_indirect) {
		vkCmdDispatchIndirect(surface->command, sizes->buffer, (VkDeviceSize)indirect);
	} else {
		vkCmdDispatch(surface->command, groups[0], groups[1], groups[2]);
	}

	/* The dispatch's writes seen by what comes after, the images back in the layout they are sampled in. */
	compute_barriers(surface->command, 1);
	compute_image_layouts(surface->command, image_textures, image_texture_count, 1);
	surface->recorded = 1;

	/* Says so when asked to. */
	compute_trace(groups, indirect, is_indirect);

	/*
	 * A buffer bound to a block the shader may write is newer on the
	 * device now: whatever reads its bytes waits for this frame and reads
	 * them back first (gles_buffer_fetch).
	 */
	for (index = 0U; index < program->storage_count; index++) {
		if (program->storages[index].readonly)
			continue;
		state->storage_ranges[program->storages[index].binding].buffer->gpu_written = 1;
	}
}

/* Returns the current program when it is a linked compute program, else NULL with the error recorded. */
static struct gles_program *
compute_program(
	struct zegl_context *context,
	struct gles_state *state)
{
	struct gles_program *program;

	/* A context that offers compute. */
	if (!state->compute) {
		gles_error(context, GL_INVALID_OPERATION);
		return NULL;
	}

	/* A current program linked with a compute shader, or without one the bound program pipeline object's compute stage's. */
	program = state->program;
	if (program == NULL && state->pipeline != NULL)
		program = state->pipeline->compute;
	if (program == NULL || !program->linked || program->compute_pipeline == VK_NULL_HANDLE) {
		gles_error(context, GL_INVALID_OPERATION);
		return NULL;
	}

	/* Succeeded: the program. */
	return program;
}

/*
 * Checks an indirect dispatch's sizes: a buffer bound to
 * GL_DISPATCH_INDIRECT_BUFFER, unmapped, with the three counts at a
 * word-aligned offset inside it.  *skip is set when the counts (known on
 * the CPU while the device has not written the buffer) run nothing or
 * are larger than the device's, which GL leaves undefined and which is
 * not run.  Returns 0, or -1 with the error recorded.
 */
static int
compute_indirect_check(
	struct zegl_context *context,
	struct gles_state *state,
	GLintptr indirect,
	int *skip)
{
	struct gles_buffer *buffer;
	uint32_t counts[3];
	unsigned index;

	/* An offset that is not negative and is a word's. */
	if (indirect < 0 || (indirect % 4) != 0) {
		gles_error(context, GL_INVALID_VALUE);
		return -1;
	}

	/* A buffer bound, not mapped. */
	buffer = state->dispatch_buffer;
	if (buffer == NULL || buffer->map_active) {
		gles_error(context, GL_INVALID_OPERATION);
		return -1;
	}

	/* The counts inside it. */
	if ((size_t)indirect > buffer->size || buffer->size - (size_t)indirect < COMPUTE_INDIRECT_SIZE) {
		gles_error(context, GL_INVALID_OPERATION);
		return -1;
	}

	/* Counts the device wrote are read by the dispatch only. */
	if (buffer->gpu_written)
		return 0;

	/* Counts on the CPU: none empty, none past the device's. */
	memcpy(counts, buffer->data + indirect, sizeof(counts));
	for (index = 0U; index < 3U; index++) {
		if (counts[index] == 0U || counts[index] > state->limits.maxComputeWorkGroupCount[index])
			*skip = 1;
	}

	/* Succeeded: the sizes can be read. */
	return 0;
}

/*
 * Describes the buffer range bound to each of the program's shader
 * storage blocks' binding points (the bound size, or the rest of the
 * buffer from the offset), each device copy up to date and marked used by
 * the frame.  Returns 0, or -1 with the error recorded when a binding
 * point has no buffer, a mapped one, or a range smaller than the block.
 */
static int
compute_storages(
	struct zegl_context *context,
	struct gles_state *state,
	const struct gles_program *program,
	VkDescriptorBufferInfo *storages)
{
	const struct gles_storage *storage;
	struct gles_buffer_range *range;
	struct gles_buffer *buffer;
	size_t available;
	size_t length;
	unsigned index;
	int status;

	/* Each block's range. */
	memset(storages, 0, GLES_STORAGE_BINDINGS * sizeof(*storages));
	for (index = 0U; index < program->storage_count; index++) {
		storage = &program->storages[index];
		range = &state->storage_ranges[storage->binding];
		buffer = range->buffer;

		/* A buffer bound to the point, not mapped. */
		if (buffer == NULL || buffer->map_active) {
			gles_error(context, GL_INVALID_OPERATION);
			return -1;
		}

		/* The range: the bound size, or the rest of the buffer, within what the buffer has. */
		available = 0U;
		if (range->offset < buffer->size)
			available = buffer->size - range->offset;
		length = available;
		if (range->size != 0U && range->size < available)
			length = range->size;

		/* A range the block's fixed part does not fit in is an error. */
		if (length == 0U || length < storage->size) {
			gles_error(context, GL_INVALID_OPERATION);
			return -1;
		}

		/* The device reads at most its largest storage buffer range. */
		if (length > state->limits.maxStorageBufferRange)
			length = state->limits.maxStorageBufferRange;

		/*
		 * The buffer's device copy, up to date; the frame's use of it
		 * keeps a later change from writing it in place.
		 */
		status = gles_buffer_sync(state, buffer);
		if (status != 0 || buffer->buffer == VK_NULL_HANDLE) {
			gles_error(context, GL_OUT_OF_MEMORY);
			return -1;
		}

		/* This frame uses it: a later change does not write it in place. */
		buffer->used = state->frame;

		/* The descriptor's range. */
		storages[index].buffer = buffer->buffer;
		storages[index].offset = range->offset;
		storages[index].range = length;
	}

	/* Succeeded: every block has its range. */
	return 0;
}

/*
 * Records a dispatch's barrier: before it (after 0), every earlier
 * command's writes seen by the compute shader's reads and writes and by
 * the indirect sizes' read; after it, the compute shader's writes seen by
 * every later use and by the host.
 */
static void
compute_barriers(
	VkCommandBuffer command,
	int after)
{
	VkMemoryBarrier barrier;

	/* Before: from everything written, to the dispatch. */
	memset(&barrier, 0, sizeof(barrier));
	barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
	if (!after) {
		barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_HOST_WRITE_BIT;
		barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_UNIFORM_READ_BIT |
					VK_ACCESS_INDIRECT_COMMAND_READ_BIT;
		vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
				     VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT, 0U, 1U, &barrier,
				     0U, NULL, 0U, NULL);
		return;
	}

	/* After: from the dispatch's writes, to everything that may read or write them next. */
	barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
	barrier.dstAccessMask = VK_ACCESS_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_INDEX_READ_BIT |
				VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_UNIFORM_READ_BIT | VK_ACCESS_SHADER_READ_BIT |
				VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT |
				VK_ACCESS_HOST_READ_BIT;
	vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
			     VK_PIPELINE_STAGE_ALL_COMMANDS_BIT | VK_PIPELINE_STAGE_HOST_BIT, 0U, 1U, &barrier, 0U, NULL, 0U,
			     NULL);
}

/*
 * Checks glMemoryBarrier's (or glMemoryBarrierByRegion's) bits: all of
 * them, or only allowed ones; the dispatches' own barriers order the
 * writes.  Errors are recorded.
 */
static void
compute_memory_barrier(
	GLbitfield barriers,
	GLbitfield allowed)
{
	struct zegl_context *context;
	struct gles_state *state;

	/* A context with its state, which offers compute. */
	context = gles_context();
	state = gles_state(context);
	if (state == NULL)
		return;
	if (!state->compute) {
		gles_error(context, GL_INVALID_OPERATION);
		return;
	}

	/* Every bit, or bits of the allowed ones. */
	if (barriers != GL_ALL_BARRIER_BITS && (barriers & ~allowed) != 0U)
		gles_error(context, GL_INVALID_VALUE);
}

/* Writes one line on stderr for a recorded dispatch when ZEDBSD_GLES_COMPUTE_TRACE is set (any value; 2 also times the steps, gles.c). */
static void
compute_trace(
	const GLuint *groups,
	GLintptr indirect,
	int is_indirect)
{
	const char *setting;

	/* The environment, read once. */
	if (compute_tracing < 0) {
		setting = getenv("ZEDBSD_GLES_COMPUTE_TRACE");
		compute_tracing = 0;
		if (setting != NULL)
			compute_tracing = 1;
	}

	/* Nothing unless asked. */
	if (!compute_tracing)
		return;

	/* The grid, or the indirect offset. */
	if (is_indirect) {
		fprintf(stderr, "gles: compute dispatch indirect offset=%ld\n", (long)indirect);
		return;
	}

	/* A direct dispatch's groups. */
	fprintf(stderr, "gles: compute dispatch groups=%u,%u,%u\n", groups[0], groups[1], groups[2]);
}

/*
 * Binds a level of an immutable texture (every layer, or one) to an image
 * unit, with the access and the format the shaders' image uniforms read
 * it in (texture 0 unbinds).
 */
GL_APICALL void GL_APIENTRY
glBindImageTexture(
	GLuint unit,
	GLuint texture,
	GLint level,
	GLboolean layered,
	GLint layer,
	GLenum access,
	GLenum format)
{
	struct zegl_context *context;
	struct gles_state *state;
	struct gles_texture *bound;
	struct gles_image_unit *binding;
	int index;

	/* A context that offers compute (its image units). */
	context = gles_context();
	state = gles_state(context);
	if (state == NULL)
		return;
	if (!state->compute) {
		gles_error(context, GL_INVALID_OPERATION);
		return;
	}

	/* A unit there is, a level and a layer not negative, a format a unit takes. */
	index = compute_image_format_index(format);
	if (unit >= state->image_units || level < 0 || layer < 0 || index < 0) {
		gles_error(context, GL_INVALID_VALUE);
		return;
	}

	/* An access. */
	if (access != GL_READ_ONLY &&
	    access != GL_WRITE_ONLY &&
	    access != GL_READ_WRITE) {
		gles_error(context, GL_INVALID_ENUM);
		return;
	}

	/* A texture there is (0: none), immutable. */
	bound = NULL;
	if (texture != 0U) {
		bound = gles_names_get(&state->textures, texture);
		if (bound == NULL) {
			gles_error(context, GL_INVALID_VALUE);
			return;
		}
		if (!bound->immutable) {
			gles_error(context, GL_INVALID_OPERATION);
			return;
		}
	}

	/* Succeeded: the unit's binding. */
	binding = &state->image_bindings[unit];
	binding->texture = bound;
	binding->level = level;
	binding->layered = layered != GL_FALSE ? GL_TRUE : GL_FALSE;
	binding->layer = layer;
	binding->access = access;
	binding->format = format;
}

/*
 * Returns how many image units a context offers: none without compute,
 * else as many storage images as a stage and a set of the device take, at
 * most GLES_IMAGE_UNITS, when the device stores images of a format a unit
 * takes (none otherwise).  Each unit starts as GL's: no texture, level 0,
 * GL_READ_ONLY of GL_R32UI.
 */
unsigned
gles_image_units_offered(
	struct gles_state *state)
{
	uint32_t features;
	unsigned units;
	unsigned index;
	int stores;

	/* The units' first bindings. */
	memset(state->image_bindings, 0, sizeof(state->image_bindings));
	for (index = 0U; index < GLES_IMAGE_UNITS; index++) {
		state->image_bindings[index].access = GL_READ_ONLY;
		state->image_bindings[index].format = GL_R32UI;
	}

	/* Compute's, of a device that stores images of one of the formats. */
	if (!state->compute)
		return 0U;
	stores = 0;
	for (index = 0U; index < GLES_IMAGE_FORMATS; index++) {
		features = gles_image_features(state, compute_formats[index].vk);
		if ((features & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) != 0U)
			stores = 1;
	}
	if (!stores)
		return 0U;

	/* As many as a stage and a set take. */
	units = GLES_IMAGE_UNITS;
	if (state->limits.maxPerStageDescriptorStorageImages < units)
		units = state->limits.maxPerStageDescriptorStorageImages;
	if (state->limits.maxDescriptorSetStorageImages < units)
		units = state->limits.maxDescriptorSetStorageImages;

	/* Succeeded: the units. */
	return units;
}

/*
 * Finds the Vulkan and GL formats of the GLSL compiler's image format
 * (GLSL_IMAGE_FORMAT_*); nonzero for one there is not.
 */
int
gles_image_format(
	unsigned glsl_format,
	VkFormat *vk,
	GLenum *gl)
{
	/* The table from 1. */
	if (glsl_format < 1U || glsl_format > GLES_IMAGE_FORMATS)
		return -1;

	/* Succeeded: the formats. */
	*vk = compute_formats[glsl_format - 1U].vk;
	*gl = compute_formats[glsl_format - 1U].gl;
	return 0;
}

/*
 * Returns the size of a texel in bytes of an internal format an image
 * unit takes (OpenGL ES 3.1's table 8.28), 0 for any other format.
 */
unsigned
gles_image_unit_bytes(
	GLenum internal)
{
	unsigned index;

	/* The table. */
	for (index = 0U; index < GLES_IMAGE_FORMATS; index++) {
		if (compute_formats[index].gl == internal)
			return compute_formats[index].bytes;
	}

	/* Not one. */
	return 0U;
}

/*
 * Reports whether the device stores images of another format an image
 * unit takes of the same size as an internal format (a texture's image of
 * that format is then made mutable, so that a unit of the other format
 * reinterprets its texels).  Returns 1 or 0.
 */
int
gles_image_unit_alias(
	struct gles_state *state,
	GLenum internal)
{
	uint32_t features;
	unsigned bytes;
	unsigned index;

	/* A format an image unit takes. */
	bytes = gles_image_unit_bytes(internal);
	if (bytes == 0U)
		return 0;

	/* Another of its size that the device stores. */
	for (index = 0U; index < GLES_IMAGE_FORMATS; index++) {
		if (compute_formats[index].gl == internal || compute_formats[index].bytes != bytes)
			continue;
		features = gles_image_features(state, compute_formats[index].vk);
		if ((features & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) != 0U)
			return 1;
	}

	/* None. */
	return 0;
}

/* Reports whether a Vulkan format is one an image unit takes. */
int
gles_image_unit_format(
	VkFormat format)
{
	unsigned index;

	/* The table. */
	for (index = 0U; index < GLES_IMAGE_FORMATS; index++) {
		if (compute_formats[index].vk == format)
			return 1;
	}

	/* Not one. */
	return 0;
}

/*
 * Reads an image unit's binding (glGetIntegeri_v of GL_IMAGE_BINDING_*):
 * returns 1 with the value, 0 when the name is not an image unit's, -1 with
 * the error recorded.
 */
int
gles_image_indexed(
	struct zegl_context *context,
	struct gles_state *state,
	GLenum target,
	GLuint index,
	GLint64 *value)
{
	struct gles_image_unit *binding;

	/* The names of a unit's binding. */
	switch (target) {
	case GL_IMAGE_BINDING_NAME:
	case GL_IMAGE_BINDING_LEVEL:
	case GL_IMAGE_BINDING_LAYERED:
	case GL_IMAGE_BINDING_LAYER:
	case GL_IMAGE_BINDING_ACCESS:
	case GL_IMAGE_BINDING_FORMAT:
		break;
	default:
		return 0;
	}

	/* A context that offers compute, and a unit there is. */
	if (!state->compute) {
		gles_error(context, GL_INVALID_ENUM);
		return -1;
	}
	if (index >= state->image_units) {
		gles_error(context, GL_INVALID_VALUE);
		return -1;
	}

	/* The value. */
	binding = &state->image_bindings[index];
	switch (target) {
	case GL_IMAGE_BINDING_NAME:
		*value = 0;
		if (binding->texture != NULL)
			*value = (GLint64)binding->texture->name;
		break;
	case GL_IMAGE_BINDING_LEVEL:
		*value = binding->level;
		break;
	case GL_IMAGE_BINDING_LAYERED:
		*value = binding->layered;
		break;
	case GL_IMAGE_BINDING_LAYER:
		*value = binding->layer;
		break;
	case GL_IMAGE_BINDING_ACCESS:
		*value = (GLint64)binding->access;
		break;
	default:
		*value = (GLint64)binding->format;
		break;
	}

	/* Succeeded: the value. */
	return 1;
}

/* Unbinds a texture being deleted from every image unit it is bound to. */
void
gles_image_forget(
	struct gles_state *state,
	struct gles_texture *texture)
{
	unsigned index;

	/* Each unit that has it. */
	for (index = 0U; index < GLES_IMAGE_UNITS; index++) {
		if (state->image_bindings[index].texture == texture)
			state->image_bindings[index].texture = NULL;
	}
}

/*
 * Describes the storage image view each of the program's image uniforms
 * reads (its unit's binding, or a 1x1 texture), each texture up to date,
 * used by the frame, put in the list of textures once (at most
 * GLES_IMAGE_UNITS) and marked general; a level the uniform may write is
 * newer on the device from now on.  Returns 0, or -1 when a view or a
 * texture cannot be made.
 */
static int
compute_images(
	struct gles_state *state,
	const struct gles_program *program,
	VkDescriptorImageInfo *infos,
	struct gles_texture **textures,
	unsigned *texture_count,
	uint32_t *valid_units)
{
	const struct gles_image *image;
	struct gles_texture *texture;
	VkImageViewType type;
	uint32_t level;
	uint32_t layer;
	uint32_t faces;
	unsigned listed;
	unsigned index;
	unsigned face;
	int valid;

	/* Each image uniform. */
	memset(infos, 0, GLES_IMAGE_UNITS * sizeof(*infos));
	*valid_units = 0U;
	for (index = 0U; index < program->image_count; index++) {
		image = &program->images[index];

		/* Its unit's binding (its accesses valid), or a 1x1 texture of its format. */
		texture = compute_image_binding(state, image, &level, &layer, &type, &faces, &valid);
		if (texture == NULL)
			return -1;

		/* The shader may touch its unit only when the binding fits. */
		if (valid)
			*valid_units |= (uint32_t)1U << image->unit;

		/* The view, read and written in the general layout. */
		infos[index].imageView = gles_texture_storage_view(state, texture, level, layer, type, image->format);
		infos[index].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
		if (infos[index].imageView == VK_NULL_HANDLE)
			return -1;

		/* The frame uses the texture; a level written is newer on the device (by face: bit level + the base level). */
		texture->used = state->frame;
		for (face = 0U; image->writes && face < GLES_FACES; face++) {
			if ((faces & (1U << face)) != 0U)
				texture->gpu_levels[face] |= 1U << (level + (uint32_t)texture->base_level);
		}

		/* Listed once, general while the descriptors are written. */
		for (listed = 0U; listed < *texture_count; listed++) {
			if (textures[listed] == texture)
				break;
		}
		if (listed == *texture_count) {
			textures[*texture_count] = texture;
			(*texture_count)++;
		}
		texture->general = 1;
	}

	/* Succeeded: every image has its view. */
	return 0;
}

/*
 * Returns the texture an image uniform reads, up to date, with the level
 * of its image (from the base level), the layer (GLES_LAYER_ALL: every
 * one), the view's type, the faces whose level a write changes (bit f:
 * face f), and whether the access is valid: its unit's texture when the
 * binding fits the uniform (the unit's format is the uniform's, the
 * texture's internal format is one a unit takes of the same size,
 * OpenGL ES 3.1 section 8.22, a storage image that a view of the unit's
 * format may read, a level the image has, every layer of a 3D, cube map
 * or 2D array image for those uniforms, one layer of a cube map or a 2D
 * array or a 2D texture for a 2D one), else (invalid: the shader reads
 * zeros and writes nothing) the 1x1 texture of the uniform's shape and
 * format.  NULL when that cannot be made either.
 */
static struct gles_texture *
compute_image_binding(
	struct gles_state *state,
	const struct gles_image *image,
	uint32_t *level,
	uint32_t *layer,
	VkImageViewType *type,
	uint32_t *faces,
	int *valid)
{
	static const GLenum targets[GLES_SHAPES] = { GL_TEXTURE_2D, GL_TEXTURE_CUBE_MAP, GL_TEXTURE_3D, GL_TEXTURE_2D_ARRAY };
	static const VkImageViewType types[GLES_SHAPES] = {
		VK_IMAGE_VIEW_TYPE_2D, VK_IMAGE_VIEW_TYPE_CUBE, VK_IMAGE_VIEW_TYPE_3D, VK_IMAGE_VIEW_TYPE_2D_ARRAY
	};
	const struct gles_image_unit *binding;
	struct gles_texture *texture;
	GLint image_level;
	GLint layers;
	unsigned texture_bytes;
	unsigned unit_bytes;
	int fits;
	int status;

	/* The unit's texture, of the uniform's format, of an internal format of that size a unit takes. */
	*valid = 0;
	binding = &state->image_bindings[image->unit];
	texture = binding->texture;
	fits = 0;
	if (texture != NULL &&
	    binding->format == image->gl_format &&
	    texture->samples <= 1U &&
	    texture->base_level >= 0 &&
	    texture->base_level < (GLint)GLES_LEVELS &&
	    texture->levels[texture->base_level].format != NULL) {
		texture_bytes = gles_image_unit_bytes(texture->levels[texture->base_level].format->internal);
		unit_bytes = gles_image_unit_bytes(image->gl_format);
		if (texture_bytes != 0U && texture_bytes == unit_bytes)
			fits = 1;
	}

	/* A storage image up to date. */
	if (fits) {
		status = gles_texture_sync(state, texture);
		if (status != 0 || !texture->storage)
			fits = 0;
	}

	/* An image a view of the unit's format may read (the image's own format, or one of its size viewing a mutable image). */
	if (fits)
		fits = compute_image_view_fits(state, texture, image->format);

	/* A level its image has. */
	image_level = 0;
	if (fits) {
		image_level = binding->level - texture->base_level;
		if (image_level < 0 || (uint32_t)image_level >= texture->level_count)
			fits = 0;
	}

	/* Its shape: every layer of the uniform's shape, or one layer (a face) of a 2D view for a 2D uniform. */
	*layer = GLES_LAYER_ALL;
	*faces = 1U;
	if (fits) {
		layers = texture->levels[texture->base_level].depth;
		if (texture->target == GL_TEXTURE_CUBE_MAP)
			layers = (GLint)GLES_FACES;

		/* What the uniform's shape takes of the texture's. */
		switch (image->shape) {
		case GLES_SHAPE_2D:
			if (texture->target == GL_TEXTURE_2D) {
				*layer = 0U;
			} else if ((texture->target == GL_TEXTURE_CUBE_MAP || texture->target == GL_TEXTURE_2D_ARRAY) &&
				   !binding->layered && binding->layer < layers) {
				*layer = (uint32_t)binding->layer;
				if (texture->target == GL_TEXTURE_CUBE_MAP)
					*faces = 1U << *layer;
			} else {
				fits = 0;
			}
			break;
		case GLES_SHAPE_CUBE:
			fits = (texture->target == GL_TEXTURE_CUBE_MAP && binding->layered);
			*faces = (1U << GLES_FACES) - 1U;
			break;
		case GLES_SHAPE_3D:
			fits = (texture->target == GL_TEXTURE_3D && binding->layered);
			break;
		default:
			fits = ((texture->target == GL_TEXTURE_2D_ARRAY || texture->target == GL_TEXTURE_CUBE_MAP) && binding->layered);
			if (texture->target == GL_TEXTURE_CUBE_MAP)
				*faces = (1U << GLES_FACES) - 1U;
			break;
		}
	}

	/* A binding that fits: its accesses are valid. */
	if (fits) {
		*level = (uint32_t)image_level;
		*type = types[image->shape];
		*valid = 1;
		return texture;
	}

	/* Otherwise the 1x1 texture, which the shader does not touch (said once: a unit with a texture the uniform cannot read). */
	if (texture != NULL && !compute_image_reported) {
		compute_image_reported = 1;
		fprintf(stderr, "gles: an image unit's binding does not fit its image uniform (format, size, shape or level); "
				"its accesses read zeros and write nothing\n");
	}
	texture = gles_texture_blank(state, targets[image->shape], image->gl_format);
	if (texture == NULL)
		return NULL;
	status = gles_texture_sync(state, texture);
	if (status != 0 || !texture->storage)
		return NULL;

	/* Its one level, every layer of its shape; nothing it holds is the application's. */
	*level = 0U;
	*layer = GLES_LAYER_ALL;
	if (image->shape == GLES_SHAPE_2D)
		*layer = 0U;
	*type = types[image->shape];
	*faces = 0U;
	return texture;
}

/*
 * Reports whether a view of a format may read a texture's storage image:
 * the image's own format, or another one when the image is mutable and
 * the format has the features of every use the image was made for (a
 * Vulkan 1.0 view takes the image's usage; the device stores the unit's
 * format, which the link checked).  Returns 1 or 0.
 */
static int
compute_image_view_fits(
	struct gles_state *state,
	const struct gles_texture *texture,
	VkFormat format)
{
	uint32_t features;

	/* The image's own format. */
	if (texture->image_format != NULL && texture->image_format->vk == format)
		return 1;

	/* Another one: only of a mutable image. */
	if (!texture->mutable_format)
		return 0;

	/* With the features of each use: sampled, drawn into (copies need none in Vulkan 1.0). */
	features = gles_image_features(state, format);
	if ((texture->image_usage & VK_IMAGE_USAGE_SAMPLED_BIT) != 0U && (features & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) == 0U)
		return 0;
	if ((texture->image_usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) != 0U && (features & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT) == 0U)
		return 0;
	if ((features & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) == 0U)
		return 0;

	/* Succeeded: the view may read it. */
	return 1;
}

/*
 * Records the image layouts of a dispatch's storage images: before it
 * (after 0), every level and layer from the layout they are sampled in to
 * the general one, earlier writes seen by the shader; after it, back,
 * the shader's writes seen by every later use.
 */
static void
compute_image_layouts(
	VkCommandBuffer command,
	struct gles_texture **textures,
	unsigned count,
	int after)
{
	VkImageMemoryBarrier barriers[GLES_IMAGE_UNITS];
	unsigned index;

	/* Nothing without images. */
	if (count == 0U)
		return;

	/* Each texture's image as a whole. */
	memset(barriers, 0, sizeof(barriers));
	for (index = 0U; index < count; index++) {
		barriers[index].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
		barriers[index].srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT |
						VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
		barriers[index].dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
		barriers[index].oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		barriers[index].newLayout = VK_IMAGE_LAYOUT_GENERAL;
		if (after) {
			barriers[index].srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
			barriers[index].dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT |
							VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT |
							VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
			barriers[index].oldLayout = VK_IMAGE_LAYOUT_GENERAL;
			barriers[index].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		}
		barriers[index].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barriers[index].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barriers[index].image = textures[index]->image;
		barriers[index].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		barriers[index].subresourceRange.levelCount = VK_REMAINING_MIP_LEVELS;
		barriers[index].subresourceRange.layerCount = VK_REMAINING_ARRAY_LAYERS;
	}

	/* Before: from everything, to the compute shader. */
	if (!after) {
		vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0U, 0U, NULL, 0U,
				     NULL, count, barriers);
		return;
	}

	/* After: from the compute shader, to everything. */
	vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0U, 0U, NULL, 0U, NULL,
			     count, barriers);
}

/* Returns the index of a format an image unit takes in compute_formats, -1 for another. */
static int
compute_image_format_index(
	GLenum format)
{
	unsigned index;

	/* The table. */
	for (index = 0U; index < GLES_IMAGE_FORMATS; index++) {
		if (compute_formats[index].gl == format)
			return (int)index;
	}

	/* Not one. */
	return -1;
}
