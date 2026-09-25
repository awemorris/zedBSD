/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Rectangles on the GPU (see blit.h).
 *
 * The vertex shader is disabled: the vertex fetcher writes the VUE itself
 * ([header][position][one attribute]) and one of two fragment kernels runs.
 * Both kernels come from the executor's own compiler, built from IR here
 * rather than from SPIR-V:
 *
 *   fill   colour = the attribute; for a clear the attribute is the clear
 *          value at every vertex
 *   copy   colour = texture(source, attribute); the attribute is the
 *          normalized source coordinate, sampled nearest or linear
 */

#include "blit.h"
#include "batch.h"
#include "draw.h"
#include "gfx.h"
#include "heap.h"
#include "internal.h"
#include "math.h"
#include "state.h"
#include <kern/kcrt.h>

#include "../compiler/compiler.h"
#include "../i915.h"
#include "../memory.h"
#include "../session.h"
#include "../worker.h"

#include <kern/device-io.h>
#include <kern/klog.h>

#include <uapi/errno.h>
#include <stddef.h>
#include <stdint.h>

#include "../intel/genxml.h"

/* How many rectangles are logged even when they succeed. */
#define I915_BLIT_LOGGED_RECTANGLES	4U

/*
 * The fill and copy kernels.
 *
 * They are compiled by the first rectangle and kept for the life of the
 * kernel; every session and every device shares them.  A NULL kernel has
 * not been compiled yet.
 * XXX: the kernels belong to the device and its target, and should be
 * released with it rather than shared by every device.
 */
static struct i915_shader_binary *i915_blit_fill_kernel;
static struct i915_shader_binary *i915_blit_copy_kernel;

/*
 * How many rectangles have been logged.
 *
 * The first few are logged whether they succeed or not, every failure is
 * logged, and the count only grows.
 * XXX: shared by every device and session rather than kept by its owner.
 */
static unsigned i915_blit_logged;

static int i915_blit_compile(int copy, struct i915_shader_binary **result);
static int i915_blit_check_rect(const struct i915_gfx_surface *surface, const struct i915_gfx_rect *rect);
static int i915_blit_window(struct i915_render_session *session, struct i915_gfx_session *work, int copy, struct i915_gfx_op_space *space);
static int i915_blit_record(const struct i915_gfx_op_space *space, const struct i915_gfx_surface *dst, const struct i915_gfx_rect *dst_rect, const struct i915_gfx_surface *src, const struct i915_gfx_rect *src_rect, const uint32_t clear[4], int linear);
static int i915_blit_write_state(uint8_t *page, const struct i915_gfx_surface *dst, const struct i915_gfx_rect *dst_rect, const struct i915_gfx_surface *src, const struct i915_gfx_rect *src_rect, const uint32_t clear[4], int linear, uint32_t mocs);
static void i915_blit_write_vertices(uint8_t *page, const struct i915_gfx_rect *dst_rect, const struct i915_gfx_surface *src, const struct i915_gfx_rect *src_rect, const uint32_t clear[4]);
static void i915_blit_build_batch(struct i915_gfx_batch *batch, const struct i915_gfx_op_space *space, const struct i915_gfx_surface *dst, const struct i915_gfx_kernels *kernels, uint32_t mocs);

/*
 * Compiles the fill and copy kernels once, and makes the session's state
 * and batch objects.
 *
 * Returns the compiler's error, or ENOMEM when the session's objects cannot
 * be made.
 */
int
drv_i915_gfx_rect_prepare(
	struct i915_render_session *session)
{
	struct i915_gfx_session *work;
	int error;

	/* Compiles the fill kernel the first time. */
	if (i915_blit_fill_kernel == NULL) {
		error = i915_blit_compile(0, &i915_blit_fill_kernel);
		if (error != 0) {
			kern_logf("i915: vk: the fill kernel does not compile: %d\n", error);
			return error;
		}
	}

	/* Compiles the copy kernel the first time. */
	if (i915_blit_copy_kernel == NULL) {
		error = i915_blit_compile(1, &i915_blit_copy_kernel);
		if (error != 0) {
			kern_logf("i915: vk: the copy kernel does not compile: %d\n", error);
			return error;
		}

		/* Says what both kernels came to. */
		kern_logf("i915: vk: transfer kernels compiled by the executor: fill %u bytes, copy %u bytes\n",
			  i915_blit_fill_kernel->code_bytes,
			  i915_blit_copy_kernel->code_bytes);
	}

	/* Makes the session's state and batch objects. */
	work = drv_i915_gfx_session_get(session);
	if (work == NULL)
		return ENOMEM;

	/* Succeeded: the kernels and the session's objects exist. */
	return 0;
}

/*
 * Writes the state and the batch of one rectangle; the caller runs the
 * batch.
 *
 * A copy of src_rect of `src` into dst_rect of `dst` when src is not NULL,
 * or a fill of dst_rect with the four float words of `clear`.  Both
 * rectangles must be non-empty and lie inside their surfaces.  The
 * rectangle takes the first slot and the start of the batch object, so the
 * session must not be recording a submission.  Returns EINVAL for a
 * rectangle or surface that cannot be drawn, EBUSY inside a submission,
 * ENOSPC when the batch does not fit, or what the preparation reports.
 */
int
drv_i915_gfx_rect_build(
	struct i915_render_session *session,
	const struct i915_gfx_surface *dst,
	const struct i915_gfx_rect *dst_rect,
	const struct i915_gfx_surface *src,
	const struct i915_gfx_rect *src_rect,
	const uint32_t clear[4],
	int linear,
	uint64_t *batch_va)
{
	struct i915_gfx_session *work;
	struct i915_gfx_op_space space;
	int error;

	/* Makes sure the kernels and the session's objects exist. */
	error = drv_i915_gfx_rect_prepare(session);
	if (error != 0)
		return error;

	/* A submission being recorded owns the batch.  XXX: the caller would have to wait for it. */
	work = session->gfx;
	if (work->open) {
		kern_logf("i915: vk: XXX unimplemented path: a rectangle built while the session records a submission\n");
		return EBUSY;
	}

	/* Finds the kernel's instruction window. */
	error = i915_blit_window(session, work, src != NULL, &space);
	if (error != 0)
		return error;

	/* Takes the first slot and starts the batch at the start of the batch object. */
	space.slot = work->state->address;
	space.slot_va = work->state->va;
	space.batch = &work->cursor;
	work->cursor.count = 0U;
	work->cursor.overflow = 0;

	/* Writes the rectangle's state and commands, and ends the batch. */
	error = i915_blit_record(&space, dst, dst_rect, src, src_rect, clear, linear);
	if (error == 0)
		drv_i915_gfx_emit_batch_end(&work->cursor);

	/* Refuses a rectangle that did not fit the batch; the batch is left empty either way. */
	if (error == 0 && work->cursor.overflow != 0)
		error = ENOSPC;
	if (error != 0) {
		work->cursor.count = 0U;
		work->cursor.overflow = 0;
		return error;
	}

	/* The caller runs the batch; the session's batch starts empty again next time. */
	work->cursor.count = 0U;

	/* Makes the CPU writes visible before the GPU reads them. */
	kern_io_write_barrier();

	/* Hands the batch to the caller. */
	*batch_va = work->batch->va;

	/* Succeeded: the rectangle's batch is ready to run. */
	return 0;
}

/*
 * Records one rectangle into the submission's batch, or runs it to its end
 * on the GPU outside a submission: the recorded clears and copies.
 *
 * The first few rectangles and every failed one are logged; a recorded
 * rectangle is logged with how its recording ended, its GPU run with the
 * batch.
 */
int
drv_i915_gfx_rect(
	struct i915_render_session *session,
	const struct i915_gfx_surface *dst,
	const struct i915_gfx_rect *dst_rect,
	const struct i915_gfx_surface *src,
	const struct i915_gfx_rect *src_rect,
	const uint32_t clear[4],
	int linear)
{
	struct i915_gfx_session *work;
	struct i915_gfx_op_space space;
	const char *what;
	uint32_t read_width;
	uint32_t read_height;
	int error;

	/* Makes sure the kernels and the session's objects exist. */
	error = drv_i915_gfx_rect_prepare(session);
	if (error != 0)
		return error;

	/* Finds the kernel's instruction window. */
	work = session->gfx;
	error = i915_blit_window(session, work, src != NULL, &space);
	if (error != 0)
		return error;

	/* Takes the rectangle's slot and its room in the batch. */
	error = drv_i915_gfx_op_begin(session, work, &space);
	if (error != 0)
		return error;

	/* Writes the rectangle's state and commands. */
	error = i915_blit_record(&space, dst, dst_rect, src, src_rect, clear, linear);

	/*
	 * A recorded rectangle may write a buffer that a later draw reads on
	 * the CPU as uniform data (see drv_i915_gfx_draw()).
	 */
	if (error == 0)
		work->transfer_pending = 1;

	/* Keeps the rectangle in the batch, or takes it back; outside a submission it runs now. */
	error = drv_i915_gfx_op_end(session, work, error);

	/* Logs a failure, and the first few rectangles whatever their outcome. */
	if (error != 0 || i915_blit_logged < I915_BLIT_LOGGED_RECTANGLES) {
		/* Counts the logged rectangle; once past the first few, only failures are logged. */
		i915_blit_logged++;

		/* A copy reads its source rectangle; a fill covers its destination rectangle. */
		what = "fill";
		read_width = dst_rect->w;
		read_height = dst_rect->h;
		if (src != NULL) {
			what = "copy";
			read_width = src_rect->w;
			read_height = src_rect->h;
		}

		/* Says what the rectangle was and how it ended. */
		kern_logf("i915: vk: GPU %s %ux%u -> %ux%u at (%d,%d) of a %ux%u surface: %d\n",
			  what,
			  read_width,
			  read_height,
			  dst_rect->w,
			  dst_rect->h,
			  dst_rect->x,
			  dst_rect->y,
			  dst->width,
			  dst->height,
			  error);
	}

	/* Reports why the rectangle failed. */
	if (error != 0)
		return error;

	/* Succeeded: the rectangle is recorded, or has run outside a submission. */
	return 0;
}

/*
 * Compiles the fill kernel (copy zero) or the copy kernel from IR.
 *
 * The fill kernel stores its four interpolated inputs as the colour.  The
 * copy kernel loads its two inputs as a coordinate, samples set 0 binding 0
 * at it, and stores the four sampled components.
 */
static int
i915_blit_compile(
	int copy,
	struct i915_shader_binary **result)
{
	struct i915_shader_ir_inst instructions[8];
	struct i915_shader_ir_uniform uniform;
	struct i915_shader_ir ir;
	uint32_t index;
	uint32_t count;
	uint32_t first;
	int error;

	/* Starts from an empty shader. */
	kern_memset(instructions, 0, sizeof(instructions));
	kern_memset(&ir, 0, sizeof(ir));
	kern_memset(&uniform, 0, sizeof(uniform));
	count = 0U;

	/* Produces the colour values: sampled at the coordinate for a copy, the inputs for a fill. */
	if (copy) {
		/* Loads the coordinate into values 0 and 1. */
		for (index = 0U; index < 2U; index++) {
			instructions[count].op = I915_IR_LOAD_INPUT;
			instructions[count].dst = index;
			instructions[count].component = index;
			count++;
		}

		/* Samples set 0 binding 0 at it into values 2 to 5. */
		instructions[count].op = I915_IR_SAMPLE;
		instructions[count].dst = 2U;
		instructions[count].src[0] = 0U;
		instructions[count].src[1] = 1U;
		count++;

		/* Declares the one sampled image. */
		uniform.kind = 1U;
		ir.uniforms = &uniform;
		ir.uniform_count = 1U;
		first = 2U;
	} else {
		/* Loads the four inputs into values 0 to 3. */
		for (index = 0U; index < 4U; index++) {
			instructions[count].op = I915_IR_LOAD_INPUT;
			instructions[count].dst = index;
			instructions[count].component = index;
			count++;
		}

		/* The colour values start at value 0. */
		first = 0U;
	}

	/* Stores the four colour values as the output. */
	for (index = 0U; index < 4U; index++) {
		instructions[count].op = I915_IR_STORE_OUTPUT;
		instructions[count].src[0] = first + index;
		instructions[count].component = index;
		count++;
	}

	/* Completes the fragment shader. */
	ir.stage = I915_STAGE_FRAGMENT;
	ir.instructions = instructions;
	ir.instruction_count = count;
	ir.value_count = first + 4U;

	/* Compiles it. */
	error = drv_i915_shader_compile(&ir, result);
	if (error != 0)
		return error;

	/* Succeeded: the kernel is compiled. */
	return 0;
}

/*
 * Finds the instruction window of the copy kernel (copy nonzero) or of the
 * fill kernel, placing it the first time.
 */
static int
i915_blit_window(
	struct i915_render_session *session,
	struct i915_gfx_session *work,
	int copy,
	struct i915_gfx_op_space *space)
{
	int error;

	/* The copy kernel and the fill kernel each have a window of their own. */
	if (copy) {
		error = drv_i915_gfx_window(session,
					    work,
					    NULL,
					    &work->copy_window,
					    &work->copy_generation,
					    NULL,
					    0U,
					    i915_blit_copy_kernel->code,
					    i915_blit_copy_kernel->code_bytes,
					    space);
	} else {
		error = drv_i915_gfx_window(session,
					    work,
					    NULL,
					    &work->fill_window,
					    &work->fill_generation,
					    NULL,
					    0U,
					    i915_blit_fill_kernel->code,
					    i915_blit_fill_kernel->code_bytes,
					    space);
	}

	/* Reports a window that could not be had. */
	if (error != 0)
		return error;

	/* Succeeded: the kernel is in its window. */
	return 0;
}

/*
 * Writes one rectangle into its slot and appends its commands to the batch.
 *
 * Returns EINVAL for a rectangle or a surface that cannot be drawn.
 */
static int
i915_blit_record(
	const struct i915_gfx_op_space *space,
	const struct i915_gfx_surface *dst,
	const struct i915_gfx_rect *dst_rect,
	const struct i915_gfx_surface *src,
	const struct i915_gfx_rect *src_rect,
	const uint32_t clear[4],
	int linear)
{
	const struct i915_shader_binary *kernel;
	struct i915_gfx_kernels kernels;
	uint32_t mocs;
	int error;

	/* A copy runs the copy kernel and a fill the fill kernel; every surface is uncached. */
	mocs = GEN12_MOCS(I915_MOCS_UNCACHED_INDEX);
	kernel = i915_blit_fill_kernel;
	if (src != NULL)
		kernel = i915_blit_copy_kernel;

	/* Refuses a destination rectangle outside its surface. */
	error = i915_blit_check_rect(dst, dst_rect);
	if (error != 0)
		return error;

	/* Refuses a source rectangle outside its surface. */
	if (src != NULL) {
		error = i915_blit_check_rect(src, src_rect);
		if (error != 0)
			return error;
	}

	/* Writes the surfaces, the sampler and the vertices into the slot. */
	error = i915_blit_write_state(space->slot, dst, dst_rect, src, src_rect, clear, linear, mocs);
	if (error != 0)
		return error;

	/*
	 * Describes the pixel kernel for its packets: one attribute, the
	 * kernel's payload start and its sampled images.
	 */
	kern_memset(&kernels, 0, sizeof(kernels));
	kernels.varyings = 1U;
	kernels.ps_grf_start = kernel->dispatch_grf_start;
	kernels.ps_samplers = kernel->sampler_count;

	/* Appends the rectangle's commands. */
	i915_blit_build_batch(space->batch, space, dst, &kernels, mocs);

	/* Succeeded: the rectangle is written. */
	return 0;
}

/* Refuses (EINVAL) a rectangle that is empty or does not lie inside its surface. */
static int
i915_blit_check_rect(
	const struct i915_gfx_surface *surface,
	const struct i915_gfx_rect *rect)
{
	/* Refuses a rectangle that starts left of or above the surface. */
	if (rect->x < 0 || rect->y < 0)
		return EINVAL;

	/* Refuses an empty rectangle. */
	if (rect->w == 0U || rect->h == 0U)
		return EINVAL;

	/* Refuses a rectangle that runs past the right edge. */
	if ((uint32_t)rect->x + rect->w > surface->width)
		return EINVAL;

	/* Refuses a rectangle that runs past the bottom edge. */
	if ((uint32_t)rect->y + rect->h > surface->height)
		return EINVAL;

	/* Succeeded: the rectangle lies inside the surface. */
	return 0;
}

/*
 * Writes everything a rectangle's batch points at into its slot.
 *
 * Binding table entry 0 is the destination and entry 1 the source; the
 * sampler clamps to the edge with the requested filter; blending clamps to
 * the target's format and the depth range is [0, 1].  The pixel kernel
 * waits in its instruction window.
 */
static int
i915_blit_write_state(
	uint8_t *page,
	const struct i915_gfx_surface *dst,
	const struct i915_gfx_rect *dst_rect,
	const struct i915_gfx_surface *src,
	const struct i915_gfx_rect *src_rect,
	const uint32_t clear[4],
	int linear,
	uint32_t mocs)
{
	struct i915_gfx_sampler sampler;
	uint32_t *surface;
	uint32_t *dynamic;
	uint32_t filter;
	int error;

	/* Clears the whole slot and locates the two heaps. */
	kern_memset(page, 0, I915_GFX_SLOT_BYTES);
	surface = (uint32_t *)(void *)(page + I915_GFX_SURFACE_HEAP);
	dynamic = (uint32_t *)(void *)(page + I915_GFX_DYNAMIC_HEAP);

	/* Points the binding table at the destination and the source. */
	surface[I915_GFX_BINDING_TABLE / 4U] = I915_GFX_RSS_TARGET;
	surface[I915_GFX_BINDING_TABLE / 4U + 1U] = I915_GFX_RSS_TEXTURE;

	/* Describes the destination. */
	error = drv_i915_gfx_surface_write(&surface[I915_GFX_RSS_TARGET / 4U], dst, mocs);
	if (error != 0)
		return error;

	/* Describes the source of a copy. */
	if (src != NULL) {
		error = drv_i915_gfx_surface_write(&surface[I915_GFX_RSS_TEXTURE / 4U], src, mocs);
		if (error != 0)
			return error;
	}

	/*
	 * Samples with the requested filter, clamped to the edge, at LOD 0 of
	 * the source's one level: a surface here is always one level, a mip
	 * level of an image being described as a surface of its own.
	 */
	filter = VK_FILTER_NEAREST;
	if (linear)
		filter = VK_FILTER_LINEAR;
	kern_memset(&sampler, 0, sizeof(sampler));
	sampler.mag_filter = filter;
	sampler.min_filter = filter;
	sampler.address_u = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	sampler.address_v = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	sampler.mipmap_mode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
	sampler.lod_bias = 0U;
	sampler.min_lod = 0U;
	sampler.max_lod = 0U;
	drv_i915_gfx_sampler_write(&dynamic[I915_GFX_DYN_SAMPLER / 4U], &sampler);

	/* Clamps before and after blending to the target's format; the depth range ends at 1.0. */
	dynamic[I915_GFX_DYN_BLEND / 4U + 2U] = 1U | (1U << 1) | (GEN12_COLORCLAMP_RTFORMAT << 2);
	dynamic[I915_GFX_DYN_CC_VIEWPORT / 4U + 1U] = I915_FLOAT_ONE;

	/* Writes the three corners of the rectangle and their attributes. */
	i915_blit_write_vertices(page, dst_rect, src, src_rect, clear);

	/* Succeeded: the state object holds everything the batch points at. */
	return 0;
}

/*
 * Writes the three vertices of a RECTLIST: the corners (x1, y1), (x0, y1)
 * and (x0, y0) of the destination rectangle.
 *
 * Each vertex is a VUE of twelve floats: a zero header, the position
 * (x, y, 0, 1) and one attribute.  The attribute of a copy is the matching
 * source corner, normalized by the source extent (the sampling form proven
 * on the hardware); that of a fill is the clear value.
 */
static void
i915_blit_write_vertices(
	uint8_t *page,
	const struct i915_gfx_rect *dst_rect,
	const struct i915_gfx_surface *src,
	const struct i915_gfx_rect *src_rect,
	const uint32_t clear[4])
{
	uint32_t attributes[3][4];
	uint32_t *vertices;
	uint32_t *vertex;
	uint32_t index;
	uint32_t x0;
	uint32_t y0;
	uint32_t x1;
	uint32_t y1;
	uint32_t source_x;
	uint32_t source_y;

	/* Takes the corners of the destination rectangle. */
	x0 = (uint32_t)dst_rect->x;
	y0 = (uint32_t)dst_rect->y;
	x1 = x0 + dst_rect->w;
	y1 = y0 + dst_rect->h;

	/* Gives every corner its attribute. */
	for (index = 0U; index < 3U; index++) {
		/* A fill carries the clear value at every corner. */
		if (src == NULL) {
			kern_memcpy(attributes[index], clear, sizeof(attributes[index]));
			continue;
		}

		/* The first corner is at the source's right edge, the first two at its bottom edge. */
		source_x = (uint32_t)src_rect->x;
		if (index == 0U)
			source_x += src_rect->w;
		source_y = (uint32_t)src_rect->y;
		if (index < 2U)
			source_y += src_rect->h;

		/* A copy carries the normalized source coordinate. */
		attributes[index][0] = drv_i915_float_ratio(source_x, src->width);
		attributes[index][1] = drv_i915_float_ratio(source_y, src->height);
		attributes[index][2] = 0U;
		attributes[index][3] = 0U;
	}

	/* Writes the three vertices. */
	vertices = (uint32_t *)(void *)(page + I915_GFX_RECT_VERTICES);
	for (index = 0U; index < 3U; index++) {
		vertex = vertices + index * (I915_GFX_RECT_VERTEX_BYTES / 4U);

		/* The first corner is at the right edge, the other two at the left. */
		if (index == 0U) {
			vertex[4] = drv_i915_float_from_u32(x1);
		} else {
			vertex[4] = drv_i915_float_from_u32(x0);
		}

		/* The first two corners are at the bottom edge, the last at the top. */
		if (index < 2U) {
			vertex[5] = drv_i915_float_from_u32(y1);
		} else {
			vertex[5] = drv_i915_float_from_u32(y0);
		}

		/* Completes the position and copies the attribute after it; the header stays zero. */
		vertex[6] = 0U;
		vertex[7] = I915_FLOAT_ONE;
		kern_memcpy(&vertex[8], attributes[index], sizeof(attributes[index]));
	}
}

/*
 * Appends the commands of one rectangle to the batch.
 *
 * The order is the fixture draw's RECTLIST path with a fragment input: the
 * context setup at the rectangle's slot and window; one vertex buffer of three vertices whose three elements
 * are VUE slots 0 (header), 1 (position) and 2 (attribute); no geometry
 * shading, so the vertex fetcher feeds the clipper; a screen-space
 * rectangle with no clipping, perspective divide or viewport transform; the
 * pixel kernel; no depth; the primitive with its closing flush.
 */
static void
i915_blit_build_batch(
	struct i915_gfx_batch *batch,
	const struct i915_gfx_op_space *space,
	const struct i915_gfx_surface *dst,
	const struct i915_gfx_kernels *kernels,
	uint32_t mocs)
{
	struct i915_gfx_primitive primitive;
	uint64_t state_va;
	uint64_t vertices_va;
	uint32_t index;
	uint32_t vs_dword;

	/* Switches to 3D, programs the state bases and the once-per-context state. */
	state_va = space->slot_va;
	drv_i915_gfx_emit_context_setup(batch, state_va, space->window_va, 0U, mocs);

	/* Describes the one vertex buffer: MOCS, address modify enable and the stride; the address; the size. */
	vertices_va = state_va + I915_GFX_RECT_VERTICES;
	drv_i915_batch_emit(batch, GEN12_CMD_HEADER(GEN12_CMD_3DSTATE_VERTEX_BUFFERS, 1U + GEN12_VERTEX_BUFFER_STATE_DWORDS));
	drv_i915_batch_emit(batch,
			    GEN12_VERTEX_BUFFER_L3_BYPASS_DISABLE |
			    (mocs << 16) |
			    (1U << 14) |
			    I915_GFX_RECT_VERTEX_BYTES);
	drv_i915_batch_emit(batch, (uint32_t)vertices_va);
	drv_i915_batch_emit(batch, (uint32_t)(vertices_va >> 32));
	drv_i915_batch_emit(batch, 3U * I915_GFX_RECT_VERTEX_BYTES);

	/* Describes three vec4 elements, one for each VUE slot, stored as they are. */
	drv_i915_batch_emit(batch, GEN12_CMD_HEADER(GEN12_CMD_3DSTATE_VERTEX_ELEMENTS, 1U + 3U * GEN12_VERTEX_ELEMENT_STATE_DWORDS));
	for (index = 0U; index < 3U; index++) {
		drv_i915_batch_emit(batch, (1U << 25) | (GEN12_FORMAT_R32G32B32A32_FLOAT << 16) | (index * 16U));
		drv_i915_batch_emit(batch,
				    (GEN12_VFCOMP_STORE_SRC << 28) |
				    (GEN12_VFCOMP_STORE_SRC << 24) |
				    (GEN12_VFCOMP_STORE_SRC << 20) |
				    (GEN12_VFCOMP_STORE_SRC << 16));
	}

	/* Enables the vertex fetch statistics and clears the fetcher's other state. */
	drv_i915_batch_emit(batch, (GEN12_CMD_3DSTATE_VF_STATISTICS << 16) | 1U);
	drv_i915_batch_zero(batch, GEN12_CMD_3DSTATE_VF, GEN12_3DSTATE_VF_DWORDS);
	drv_i915_batch_zero(batch, GEN12_CMD_3DSTATE_VF_SGVS, GEN12_3DSTATE_VF_SGVS_DWORDS);
	drv_i915_batch_zero(batch, GEN12_CMD_3DSTATE_VF_SGVS_2, GEN12_3DSTATE_VF_SGVS_2_DWORDS);

	/* Turns instancing off for every vertex element. */
	for (index = 0U; index < 3U; index++) {
		drv_i915_batch_emit(batch, GEN12_CMD_HEADER(GEN12_CMD_3DSTATE_VF_INSTANCING, GEN12_3DSTATE_VF_INSTANCING_DWORDS));
		drv_i915_batch_emit(batch, index);
		drv_i915_batch_emit(batch, 0U);
	}

	/* Draws rectangle lists. */
	drv_i915_batch_emit(batch, GEN12_CMD_HEADER(GEN12_CMD_3DSTATE_VF_TOPOLOGY, GEN12_3DSTATE_VF_TOPOLOGY_DWORDS));
	drv_i915_batch_emit(batch, GEN12_3DPRIM_RECTLIST);

	/* Gives the vertex stage one-slot URB entries and reads no push constants. */
	drv_i915_gfx_emit_urb(batch, 1U);
	drv_i915_gfx_emit_constants(batch, state_va + I915_GFX_PUSH_BUFFER, 0U, state_va + I915_GFX_PS_PUSH_BUFFER, 0U, mocs);

	/* Points the pipeline at the colour calc, blend, viewport and coarse pixel state. */
	drv_i915_batch_pointer(batch, GEN12_CMD_3DSTATE_CC_STATE_POINTERS, I915_GFX_DYN_COLOR_CALC | 1U);
	drv_i915_batch_pointer(batch, GEN12_CMD_3DSTATE_BLEND_STATE_POINTERS, I915_GFX_DYN_BLEND | 1U);
	drv_i915_batch_pointer(batch, GEN12_CMD_3DSTATE_VIEWPORT_STATE_POINTERS_CC, I915_GFX_DYN_CC_VIEWPORT);
	drv_i915_batch_pointer(batch, GEN12_CMD_3DSTATE_CPS_POINTERS, I915_GFX_DYN_CPS);

	/* Disables the binding table pool, as anv does against state leaking from other contexts. */
	drv_i915_batch_emit(batch, GEN12_CMD_HEADER(GEN12_CMD_3DSTATE_BINDING_TABLE_POOL_ALLOC, GEN12_3DSTATE_BINDING_TABLE_POOL_ALLOC_DWORDS));
	drv_i915_batch_emit(batch, mocs);
	drv_i915_batch_emit(batch, 0U);
	drv_i915_batch_emit(batch, 0U);

	/* Renders one sample per pixel. */
	drv_i915_batch_zero(batch, GEN12_CMD_3DSTATE_MULTISAMPLE, GEN12_3DSTATE_MULTISAMPLE_DWORDS);
	drv_i915_batch_emit(batch, GEN12_CMD_HEADER(GEN12_CMD_3DSTATE_SAMPLE_MASK, GEN12_3DSTATE_SAMPLE_MASK_DWORDS));
	drv_i915_batch_emit(batch, 1U);

	/* Leaves the vertex shader off with statistics only (dword 7); the vertex fetcher feeds the clipper. */
	drv_i915_batch_emit(batch, GEN12_CMD_HEADER(GEN12_CMD_3DSTATE_VS, GEN12_3DSTATE_VS_DWORDS));
	for (index = 1U; index < GEN12_3DSTATE_VS_DWORDS; index++) {
		vs_dword = 0U;
		if (index == 7U)
			vs_dword = 1U << 10;
		drv_i915_batch_emit(batch, vs_dword);
	}

	/* Enables no other geometry stage. */
	drv_i915_batch_zero(batch, GEN12_CMD_3DSTATE_HS, GEN12_3DSTATE_HS_DWORDS);
	drv_i915_batch_zero(batch, GEN12_CMD_3DSTATE_TE, GEN12_3DSTATE_TE_DWORDS);
	drv_i915_batch_zero(batch, GEN12_CMD_3DSTATE_DS, GEN12_3DSTATE_DS_DWORDS);
	drv_i915_batch_zero(batch, GEN12_CMD_3DSTATE_STREAMOUT, GEN12_3DSTATE_STREAMOUT_DWORDS);
	drv_i915_batch_zero(batch, GEN12_CMD_3DSTATE_GS, GEN12_3DSTATE_GS_DWORDS);
	drv_i915_batch_zero(batch, GEN12_CMD_3DSTATE_PRIMITIVE_REPLICATION, GEN12_3DSTATE_PRIMITIVE_REPLICATION_DWORDS);

	/* Clips with statistics only, and passes screen-space positions through. */
	drv_i915_batch_emit(batch, GEN12_CMD_HEADER(GEN12_CMD_3DSTATE_CLIP, GEN12_3DSTATE_CLIP_DWORDS));
	drv_i915_batch_emit(batch, 1U << 10);
	drv_i915_batch_emit(batch, 1U << 9);
	drv_i915_batch_emit(batch, 0U);

	/* Sets up with statistics and no viewport transform. */
	drv_i915_batch_emit(batch, GEN12_CMD_HEADER(GEN12_CMD_3DSTATE_SF, GEN12_3DSTATE_SF_DWORDS));
	drv_i915_batch_emit(batch, 1U << 10);
	drv_i915_batch_emit(batch, GEN12_URB_DEREF_BLOCK_SIZE_32 << 29);
	drv_i915_batch_emit(batch, 0U);

	/* Rasterizes without culling. */
	drv_i915_batch_emit(batch, GEN12_CMD_HEADER(GEN12_CMD_3DSTATE_RASTER, GEN12_3DSTATE_RASTER_DWORDS));
	drv_i915_batch_emit(batch, GEN12_CULLMODE_NONE << 16);
	for (index = 2U; index < GEN12_3DSTATE_RASTER_DWORDS; index++)
		drv_i915_batch_emit(batch, 0U);

	/* Programs the pixel stage for the kernel, with a writeable render target. */
	drv_i915_gfx_emit_pixel_shader(batch, kernels);
	drv_i915_batch_emit(batch, GEN12_CMD_HEADER(GEN12_CMD_3DSTATE_PS_BLEND, GEN12_3DSTATE_PS_BLEND_DWORDS));
	drv_i915_batch_emit(batch, 1U << 30);

	/* Turns the depth test off and describes null depth and stencil buffers. */
	drv_i915_batch_zero(batch, GEN12_CMD_3DSTATE_WM_DEPTH_STENCIL, GEN12_3DSTATE_WM_DEPTH_STENCIL_DWORDS);
	drv_i915_batch_emit(batch, GEN12_CMD_HEADER(GEN12_CMD_3DSTATE_DEPTH_BUFFER, GEN12_3DSTATE_DEPTH_BUFFER_DWORDS));
	drv_i915_batch_emit(batch, (GEN12_SURFTYPE_NULL << 29) | (GEN12_DEPTH_FORMAT_D32_FLOAT << 24));
	for (index = 2U; index < GEN12_3DSTATE_DEPTH_BUFFER_DWORDS; index++)
		drv_i915_batch_emit(batch, 0U);
	drv_i915_batch_emit(batch, GEN12_CMD_HEADER(GEN12_CMD_3DSTATE_STENCIL_BUFFER, GEN12_3DSTATE_STENCIL_BUFFER_DWORDS));
	drv_i915_batch_emit(batch, GEN12_SURFTYPE_NULL << 29);
	for (index = 2U; index < GEN12_3DSTATE_STENCIL_BUFFER_DWORDS; index++)
		drv_i915_batch_emit(batch, 0U);
	drv_i915_batch_zero(batch, GEN12_CMD_3DSTATE_HIER_DEPTH_BUFFER, GEN12_3DSTATE_HIER_DEPTH_BUFFER_DWORDS);
	drv_i915_batch_zero(batch, GEN12_CMD_3DSTATE_CLEAR_PARAMS, GEN12_3DSTATE_CLEAR_PARAMS_DWORDS);

	/* Describes the one rectangle: three vertices in order, one instance. */
	kern_memset(&primitive, 0, sizeof(primitive));
	primitive.topology = GEN12_3DPRIM_RECTLIST;
	primitive.vertex_count = 3U;
	primitive.instance_count = 1U;

	/* Draws the one rectangle over the destination and flushes what it wrote. */
	drv_i915_gfx_emit_draw(batch, dst->width, dst->height, &primitive);
}
