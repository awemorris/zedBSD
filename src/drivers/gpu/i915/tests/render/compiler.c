/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The compiler scenario "vkc": shaders compiled by the executor's compiler
 * run on the GPU, their outputs read back and checked against what the CPU
 * expects.
 *
 * Each step compiles a vertex and a fragment shader of
 * compiler-shaders/ into a pipeline, draws into a 64 x 64
 * R32G32B32A32_SFLOAT target cleared by the CPU, and compares every
 * component of every pixel with the bounds regenerate.py computed: exact
 * where the operation is exact, the precision Vulkan requires elsewhere.
 * The cell steps draw 16 x 16 cells whose vertices all carry the same
 * attributes, one input to a cell, so every function runs over 256 inputs;
 * the pixel steps draw one quad whose value is the pixel position, so a
 * branch or a discard goes different ways inside one SIMD8 dispatch and a
 * discarded pixel keeps the clear value.  The draws go straight to the
 * executor's draw (drv_i915_gfx_draw) with a draw state the scenario fills,
 * so no wire is involved.
 *
 * The draws need the node served, so the scenario starts a thread that
 * waits for it, opens a session of its own, runs the steps, logs
 * "VKC-<name> PASS" or "FAIL" for each and the verdict, and closes the
 * session.
 */

#include "scenarios.h"
#include <kern/kcrt.h>

#include "../../compiler/compiler.h"
#include "../../i915.h"
#include "../../memory.h"
#include "../../session.h"
#include "../../sync.h"
#include "../../render/draw.h"
#include "../../render/gfx.h"
#include "../../render/internal.h"
#include "../../render/render.h"

#include <drivers/gpu/gpu.h>
#include <kern/clock.h>
#include <kern/klog.h>
#include <kern/lock.h>
#include <kern/sched.h>
#include <kern/thread.h>

#include <libc/vulkan/vulkan_core.h>

#include <uapi/errno.h>
#include <stddef.h>
#include <stdint.h>

#include "../fixtures/compiler-shaders-gen.inc"

/* The side of the square target in pixels, and of a cell. */
#define I915_VKC_SIZE			64U
#define I915_VKC_CELL			4U

/* The cells across the target, and in all. */
#define I915_VKC_CELLS_ACROSS		(I915_VKC_SIZE / I915_VKC_CELL)
#define I915_VKC_CELLS			(I915_VKC_CELLS_ACROSS * I915_VKC_CELLS_ACROSS)

/* A pixel is four floats. */
#define I915_VKC_PIXEL_BYTES		16U

/* The storage the target and the vertices are bound in, and where each is. */
#define I915_VKC_STORAGE_BYTES		(512U * 1024U)
#define I915_VKC_TARGET_OFFSET		0x00000U
#define I915_VKC_VERTEX_OFFSET		0x10000U
#define I915_VKC_VERTEX_BYTES		0x10000U

/* A vertex is 32 bytes: the position, then the attributes. */
#define I915_VKC_VERTEX_WORDS		8U

/* Two triangles to a quad. */
#define I915_VKC_QUAD_VERTICES		6U

/* The wire identity the scenario's memory carries, apart from any client's. */
#define I915_VKC_IDENTITY		0x7e57c00000000000ULL

/* How long the thread waits for the node to be published, in seconds. */
#define I915_VKC_WAIT_S			120U

/* How many differing components a failed step logs. */
#define I915_VKC_LOG_LIMIT		4U

/* IEEE-754 bits of the floats the scenario uses; the kernel computes no float. */
#define I915_VKC_F_0			0x00000000U
#define I915_VKC_F_HALF			0x3f000000U
#define I915_VKC_F_1			0x3f800000U
#define I915_VKC_F_64			0x42800000U

/*
 * How the vertices of a step carry its inputs.
 *
 * A cell step writes one quad per cell with the cell's attributes; the
 * mview step does the same with mview's three attributes; a pixel step
 * writes one quad over the target whose value is the pixel position.
 */
enum i915_vkc_layout {
	I915_VKC_LAYOUT_CELLS = 0,
	I915_VKC_LAYOUT_MVIEW,
	I915_VKC_LAYOUT_PIXELS
};

/*
 * One step: a pair of shaders, the vertices they read and what the target
 * must hold afterwards.
 *
 * The table below names every step; it never changes.
 */
struct i915_vkc_step {
	/* The name the step logs. */
	const char *name;

	/* The two shaders, and their lengths in words. */
	const uint32_t *vertex;
	uint32_t vertex_words;
	const uint32_t *fragment;
	uint32_t fragment_words;

	/* How the vertices carry the inputs, and the attributes of each cell (cell layouts only). */
	enum i915_vkc_layout layout;
	const uint32_t *attributes;

	/* Cell steps: [lo, hi] ordered keys of the four outputs of each cell. */
	const uint32_t *bounds;

	/* Pixel steps: the colour of each class (class 0 is the clear value) and the class of each pixel. */
	const uint32_t *colors;
	const uint8_t *classes;
};

/*
 * Everything the scenario owns while its thread runs.
 *
 * It is filled by the setup, used by the steps and emptied by the teardown,
 * all on the scenario's thread; the scenario runs once per boot.
 */
struct i915_vkc {
	/* The device, and the node and executor sessions the scenario opened. */
	struct i915_device *device;
	struct i915_session *session;
	struct i915_render_session *render;

	/* The storage object and its CPU view; the memory that names it. */
	struct i915_gem_object *storage;
	uint8_t *cpu;
	struct i915_gfx_memory memory;

	/* The target, its view, the render pass, the framebuffer and the vertex buffer. */
	struct i915_gfx_image target;
	struct i915_gfx_view view;
	struct i915_gfx_pass pass;
	struct i915_gfx_framebuffer framebuffer;
	struct i915_gfx_buffer vertices;

	/* The shader modules and the pipeline of the step being run. */
	struct i915_gfx_shader vertex;
	struct i915_gfx_shader fragment;
	struct i915_gfx_pipeline pipeline;

	/* The draw state and the draw of the step being run. */
	struct i915_gfx_draw_state state;
	struct i915_gfx_draw_args args;

	/* How many steps passed and failed. */
	unsigned passed;
	unsigned failed;
};

/*
 * The scenario's state.
 *
 * Only the scenario's thread touches it, from its start to its end; the
 * scenario runs once per boot.
 */
static struct i915_vkc i915_vkc_state;

/*
 * The steps, in the order they run.
 *
 * The four math steps share the math inputs; vsmath computes in the vertex
 * stage; mview runs mview's own vertex shader with its push constants.
 */
static const struct i915_vkc_step i915_vkc_steps[] = {
	{
		"unary", i915_vkc_cells_vert, sizeof(i915_vkc_cells_vert) / 4U,
		i915_vkc_unary_frag, sizeof(i915_vkc_unary_frag) / 4U,
		I915_VKC_LAYOUT_CELLS, i915_vkc_math_attributes, i915_vkc_unary_bounds, NULL, NULL
	},
	{
		"exponent", i915_vkc_cells_vert, sizeof(i915_vkc_cells_vert) / 4U,
		i915_vkc_exponent_frag, sizeof(i915_vkc_exponent_frag) / 4U,
		I915_VKC_LAYOUT_CELLS, i915_vkc_math_attributes, i915_vkc_exponent_bounds, NULL, NULL
	},
	{
		"minmax", i915_vkc_cells_vert, sizeof(i915_vkc_cells_vert) / 4U,
		i915_vkc_minmax_frag, sizeof(i915_vkc_minmax_frag) / 4U,
		I915_VKC_LAYOUT_CELLS, i915_vkc_math_attributes, i915_vkc_minmax_bounds, NULL, NULL
	},
	{
		"divide", i915_vkc_cells_vert, sizeof(i915_vkc_cells_vert) / 4U,
		i915_vkc_divide_frag, sizeof(i915_vkc_divide_frag) / 4U,
		I915_VKC_LAYOUT_CELLS, i915_vkc_math_attributes, i915_vkc_divide_bounds, NULL, NULL
	},
	{
		"compare", i915_vkc_cells_vert, sizeof(i915_vkc_cells_vert) / 4U,
		i915_vkc_compare_frag, sizeof(i915_vkc_compare_frag) / 4U,
		I915_VKC_LAYOUT_CELLS, i915_vkc_compare_attributes, i915_vkc_compare_bounds, NULL, NULL
	},
	{
		"vsmath", i915_vkc_vsmath_vert, sizeof(i915_vkc_vsmath_vert) / 4U,
		i915_vkc_passthrough_frag, sizeof(i915_vkc_passthrough_frag) / 4U,
		I915_VKC_LAYOUT_CELLS, i915_vkc_vsmath_attributes, i915_vkc_vsmath_bounds, NULL, NULL
	},
	{
		"mview", i915_vkc_mview_vert, sizeof(i915_vkc_mview_vert) / 4U,
		i915_vkc_shade_frag, sizeof(i915_vkc_shade_frag) / 4U,
		I915_VKC_LAYOUT_MVIEW, i915_vkc_mview_attributes, i915_vkc_mview_bounds, NULL, NULL
	},
	{
		"branch", i915_vkc_cells_vert, sizeof(i915_vkc_cells_vert) / 4U,
		i915_vkc_branch_frag, sizeof(i915_vkc_branch_frag) / 4U,
		I915_VKC_LAYOUT_PIXELS, NULL, NULL, i915_vkc_branch_colors, i915_vkc_branch_classes
	},
	{
		"discard", i915_vkc_cells_vert, sizeof(i915_vkc_cells_vert) / 4U,
		i915_vkc_discard_frag, sizeof(i915_vkc_discard_frag) / 4U,
		I915_VKC_LAYOUT_PIXELS, NULL, NULL, i915_vkc_discard_colors, i915_vkc_discard_classes
	},
};

static void i915_vkc_thread(void *argument);
static int i915_vkc_wait_node(struct i915_device *device);
static int i915_vkc_setup(struct i915_vkc *x);
static void i915_vkc_teardown(struct i915_vkc *x);
static void i915_vkc_objects_init(struct i915_vkc *x);
static void i915_vkc_pipeline_init(struct i915_vkc *x, const struct i915_vkc_step *step);
static uint32_t i915_vkc_vertices_write(struct i915_vkc *x, const struct i915_vkc_step *step);
static void i915_vkc_quad_write(uint32_t *words, uint32_t left, uint32_t top, uint32_t right, uint32_t bottom, const uint32_t *attributes, uint32_t attribute_count, int corner_values);
static void i915_vkc_clear(struct i915_vkc *x);
static void i915_vkc_step_run(struct i915_vkc *x, const struct i915_vkc_step *step);
static unsigned i915_vkc_check(struct i915_vkc *x, const struct i915_vkc_step *step);
static uint32_t i915_vkc_key(uint32_t bits);

/*
 * Starts the compiler scenario.
 *
 * Returns at once: the scenario's thread runs the steps after the node is
 * published and logs their verdicts itself.
 */
void
drv_i915_test_render_compiler(
	struct i915_device *device)
{
	struct thread *thread;
	int error;

	/* Starts the thread that waits for the node and runs the steps. */
	error = kthread_create(i915_vkc_thread, device, SCHED_PRIORITY_DEFAULT, &thread);
	if (error != 0) {
		kern_logf("i915: vkc: verdict FAIL (the scenario thread cannot be created: %d)\n", error);
		return;
	}

	/* The thread reclaims itself when the steps are done. */
	thread->detached = 1U;
	thread_start(thread);
	kern_logf("i915: vkc: the steps run once the node is published\n");
}

/* Runs the scenario: waits for the node, sets up, runs every step, tears down and logs the verdict. */
static void
i915_vkc_thread(
	void *argument)
{
	struct i915_device *device;
	struct i915_vkc *x;
	unsigned index;
	int error;

	/* Waits until the node is published and its worker serves. */
	device = argument;
	error = i915_vkc_wait_node(device);
	if (error != 0) {
		kern_logf("i915: vkc: verdict FAIL (the node was not published within %u s)\n", I915_VKC_WAIT_S);
		return;
	}

	/* Opens the session and makes the storage and the objects the steps use. */
	x = &i915_vkc_state;
	kern_memset(x, 0, sizeof(*x));
	x->device = device;
	error = i915_vkc_setup(x);
	if (error != 0) {
		kern_logf("i915: vkc: verdict FAIL (setup: %d)\n", error);
		i915_vkc_teardown(x);
		return;
	}

	/* Runs every step; each logs its own verdict. */
	for (index = 0U; index < sizeof(i915_vkc_steps) / sizeof(i915_vkc_steps[0]); index++)
		i915_vkc_step_run(x, &i915_vkc_steps[index]);

	/* Gives everything back and says how the steps went. */
	i915_vkc_teardown(x);
	if (x->failed != 0U) {
		kern_logf("i915: vkc: verdict FAIL (%u of %u steps passed)\n", x->passed, x->passed + x->failed);
		return;
	}

	kern_logf("i915: vkc: verdict PASS (%u of %u steps passed)\n", x->passed, x->passed + x->failed);
}

/* Waits until the node is published, sleeping a twentieth of a second at a time; ETIMEDOUT when it never is. */
static int
i915_vkc_wait_node(
	struct i915_device *device)
{
	struct i915_completion nap;
	unsigned waited;

	/* A completion nobody signals: each wait on it simply lasts until its deadline. */
	drv_i915_completion_init(&nap, "i915 vkc");

	/* Looks for the published node until the budget is spent. */
	for (waited = 0U; waited < I915_VKC_WAIT_S * 20U; waited++) {
		/* The node is published once the GPU core holds it. */
		if (device->gpu != NULL && device->vk != NULL)
			return 0;

		(void)drv_i915_wait_for_completion(&nap, sched_ticks() + KERN_CLOCK_HZ / 20U);
	}

	/* The node never came. */
	return ETIMEDOUT;
}

/* Opens the session, makes the storage bound into its address space, and describes the objects over it. */
static int
i915_vkc_setup(
	struct i915_vkc *x)
{
	struct i915_device *device;
	void *private_session;
	int error;

	/* Opens a session of the node as a client's open would. */
	device = x->device;
	error = device->gpu_ops.open(device, &private_session);
	if (error != 0)
		return error;

	x->session = private_session;
	x->render = x->session->vk;
	if (x->render == NULL)
		return ENODEV;

	/* Creates the storage and binds it, destroying it again when the binding fails. */
	mutex_lock(&device->mutex);

	error = drv_i915_gem_create(&device->gem, I915_VKC_STORAGE_BYTES, &x->storage);
	if (error == 0) {
		error = drv_i915_gem_bind_vm(x->session->vm, x->storage);
		if (error != 0) {
			drv_i915_gem_destroy(&device->gem, x->storage);
			x->storage = NULL;
		}
	} else {
		x->storage = NULL;
	}

	mutex_unlock(&device->mutex);

	/* Reports why the storage could not be made. */
	if (error != 0)
		return error;

	/* The steps write and read the storage through its CPU view. */
	x->cpu = x->storage->address;
	i915_vkc_objects_init(x);

	/* Succeeded: every step can draw. */
	return 0;
}

/* Gives back everything the setup made, whatever it got to. */
static void
i915_vkc_teardown(
	struct i915_vkc *x)
{
	struct i915_device *device;

	/* Unbinds and destroys the storage. */
	device = x->device;
	if (x->storage != NULL) {
		mutex_lock(&device->mutex);

		drv_i915_gem_unbind_vm(x->storage);
		drv_i915_gem_destroy(&device->gem, x->storage);

		mutex_unlock(&device->mutex);

		x->storage = NULL;
	}

	/* Closes the session, which releases the executor's draw state with it. */
	if (x->session != NULL) {
		device->gpu_ops.close(device, x->session);
		x->session = NULL;
		x->render = NULL;
	}
}

/* Describes the memory, the target, the pass, the framebuffer and the vertex buffer over the storage. */
static void
i915_vkc_objects_init(
	struct i915_vkc *x)
{
	/* The memory is the whole storage. */
	x->memory.vk = x->device->vk;
	x->memory.identity = I915_VKC_IDENTITY;
	x->memory.size = I915_VKC_STORAGE_BYTES;
	x->memory.object = x->storage;

	/* The target: a linear 64x64 image of four floats to a pixel. */
	x->target.format = VK_FORMAT_R32G32B32A32_SFLOAT;
	x->target.width = I915_VKC_SIZE;
	x->target.height = I915_VKC_SIZE;
	x->target.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
	x->target.pitch = I915_VKC_SIZE * I915_VKC_PIXEL_BYTES;
	x->target.bytes = I915_VKC_SIZE * I915_VKC_SIZE * I915_VKC_PIXEL_BYTES;
	x->target.levels = 1U;
	x->target.memory = &x->memory;
	x->target.offset = I915_VKC_TARGET_OFFSET;
	x->view.image = &x->target;
	x->view.format = VK_FORMAT_R32G32B32A32_SFLOAT;
	x->view.base_level = 0U;
	x->view.level_count = 1U;

	/* One subpass writing the one colour attachment, which the CPU clears before each draw. */
	x->pass.attachment_count = 1U;
	x->pass.attachments[0].format = VK_FORMAT_R32G32B32A32_SFLOAT;
	x->pass.attachments[0].load_op = VK_ATTACHMENT_LOAD_OP_LOAD;
	x->pass.color_attachment = 0U;
	x->pass.depth_attachment = VK_ATTACHMENT_UNUSED;
	x->framebuffer.width = I915_VKC_SIZE;
	x->framebuffer.height = I915_VKC_SIZE;
	x->framebuffer.view_count = 1U;
	x->framebuffer.views[0] = &x->view;

	/* The vertex buffer. */
	x->vertices.size = I915_VKC_VERTEX_BYTES;
	x->vertices.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
	x->vertices.memory = &x->memory;
	x->vertices.offset = I915_VKC_VERTEX_OFFSET;
}

/*
 * Describes the pipeline of a step: its two shaders, 32-byte vertices of
 * one binding, a triangle list, no culling and no depth, the whole target
 * as viewport and scissor.
 */
static void
i915_vkc_pipeline_init(
	struct i915_vkc *x,
	const struct i915_vkc_step *step)
{
	/* The shader modules borrow the generated words, which the compiler only reads. */
	x->vertex.words = (uint32_t *)(uintptr_t)step->vertex;
	x->vertex.word_count = step->vertex_words;
	x->fragment.words = (uint32_t *)(uintptr_t)step->fragment;
	x->fragment.word_count = step->fragment_words;

	/* The two stages. */
	kern_memset(&x->pipeline, 0, sizeof(x->pipeline));
	x->pipeline.vertex = &x->vertex;
	x->pipeline.fragment = &x->fragment;

	/* Binding 0 of 32-byte vertices. */
	x->pipeline.binding_count = 1U;
	x->pipeline.bindings[0].binding = 0U;
	x->pipeline.bindings[0].stride = I915_VKC_VERTEX_WORDS * 4U;

	/* mview's vertex: position, normal and texture position; any other: position and value. */
	if (step->layout == I915_VKC_LAYOUT_MVIEW) {
		x->pipeline.attribute_count = 3U;
		x->pipeline.attributes[0].location = 0U;
		x->pipeline.attributes[0].format = VK_FORMAT_R32G32B32_SFLOAT;
		x->pipeline.attributes[0].offset = 0U;
		x->pipeline.attributes[1].location = 1U;
		x->pipeline.attributes[1].format = VK_FORMAT_R32G32B32_SFLOAT;
		x->pipeline.attributes[1].offset = 12U;
		x->pipeline.attributes[2].location = 2U;
		x->pipeline.attributes[2].format = VK_FORMAT_R32G32_SFLOAT;
		x->pipeline.attributes[2].offset = 24U;
	} else {
		x->pipeline.attribute_count = 2U;
		x->pipeline.attributes[0].location = 0U;
		x->pipeline.attributes[0].format = VK_FORMAT_R32G32B32A32_SFLOAT;
		x->pipeline.attributes[0].offset = 0U;
		x->pipeline.attributes[1].location = 1U;
		x->pipeline.attributes[1].format = VK_FORMAT_R32G32B32A32_SFLOAT;
		x->pipeline.attributes[1].offset = 16U;
	}

	/* A triangle list, drawn from both sides with no depth test. */
	x->pipeline.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
	x->pipeline.cull_mode = VK_CULL_MODE_NONE;

	/* The whole target, depth range [0, 1]. */
	x->pipeline.viewport[0] = I915_VKC_F_0;
	x->pipeline.viewport[1] = I915_VKC_F_0;
	x->pipeline.viewport[2] = I915_VKC_F_64;
	x->pipeline.viewport[3] = I915_VKC_F_64;
	x->pipeline.viewport[4] = I915_VKC_F_0;
	x->pipeline.viewport[5] = I915_VKC_F_1;
	x->pipeline.scissor.extent.width = I915_VKC_SIZE;
	x->pipeline.scissor.extent.height = I915_VKC_SIZE;
}

/*
 * Writes the vertices of a step and returns how many there are: a quad
 * per cell carrying the cell's attributes, or one quad over the target
 * whose value runs from (0, 0) to (64, 64).
 */
static uint32_t
i915_vkc_vertices_write(
	struct i915_vkc *x,
	const struct i915_vkc_step *step)
{
	uint32_t *words;
	uint32_t attribute_count;
	uint32_t cell;
	uint32_t column;
	uint32_t row;

	/* The vertex buffer, as words. */
	words = (uint32_t *)(void *)(x->cpu + I915_VKC_VERTEX_OFFSET);

	/* One quad over the whole target; its corners carry their own pixel positions. */
	if (step->layout == I915_VKC_LAYOUT_PIXELS) {
		i915_vkc_quad_write(words, 0U, 0U, 16U, 16U, NULL, 0U, 1);
		drv_i915_gt_clflush(words, I915_VKC_QUAD_VERTICES * I915_VKC_VERTEX_WORDS * 4U);
		return I915_VKC_QUAD_VERTICES;
	}

	/* mview's cells carry five floats (the normal and the texture position), the others four. */
	attribute_count = 4U;
	if (step->layout == I915_VKC_LAYOUT_MVIEW)
		attribute_count = 5U;

	/* One quad per cell, cell k in row k / 16 and column k % 16. */
	for (cell = 0U; cell < I915_VKC_CELLS; cell++) {
		column = cell % I915_VKC_CELLS_ACROSS;
		row = cell / I915_VKC_CELLS_ACROSS;
		i915_vkc_quad_write(words + cell * I915_VKC_QUAD_VERTICES * I915_VKC_VERTEX_WORDS,
				    column,
				    row,
				    column + 1U,
				    row + 1U,
				    step->attributes + cell * attribute_count,
				    attribute_count,
				    0);
	}

	/* Writes the vertices back to memory before the GPU reads them. */
	drv_i915_gt_clflush(words, I915_VKC_CELLS * I915_VKC_QUAD_VERTICES * I915_VKC_VERTEX_WORDS * 4U);

	/* Succeeded: six vertices to a cell. */
	return I915_VKC_CELLS * I915_VKC_QUAD_VERTICES;
}

/*
 * Writes the two triangles of the quad between cell edges (left, top) and
 * (right, bottom): the position (x, y, 0.5, 1) or, for mview, (x, y, 0.5),
 * then the attributes -- the same at every corner, or with `corner_values`
 * the corner's pixel position (x, y, 0, 0).
 */
static void
i915_vkc_quad_write(
	uint32_t *words,
	uint32_t left,
	uint32_t top,
	uint32_t right,
	uint32_t bottom,
	const uint32_t *attributes,
	uint32_t attribute_count,
	int corner_values)
{
	static const uint8_t corners[I915_VKC_QUAD_VERTICES] = { 0U, 1U, 2U, 0U, 2U, 3U };
	static const uint32_t pixel_edges[2] = { I915_VKC_F_0, I915_VKC_F_64 };
	uint32_t *vertex;
	uint32_t corner;
	uint32_t x;
	uint32_t y;
	uint32_t index;
	uint32_t word;

	/* Writes the six vertices: corners 0, 1, 2 and 0, 2, 3 of top left, top right, bottom right, bottom left. */
	for (index = 0U; index < I915_VKC_QUAD_VERTICES; index++) {
		/* The right corners are the second and third, the bottom ones the third and fourth. */
		corner = corners[index];
		x = 0U;
		if (corner == 1U || corner == 2U)
			x = 1U;
		y = 0U;
		if (corner >= 2U)
			y = 1U;

		/* Starts the vertex empty. */
		vertex = words + index * I915_VKC_VERTEX_WORDS;
		kern_memset(vertex, 0, I915_VKC_VERTEX_WORDS * 4U);

		/* The position: the cell edge in NDC, depth one half. */
		vertex[0] = i915_vkc_ndc[left + x * (right - left)];
		vertex[1] = i915_vkc_ndc[top + y * (bottom - top)];
		vertex[2] = I915_VKC_F_HALF;

		/* mview's position has three components and its attributes follow at once. */
		if (attribute_count == 5U) {
			for (word = 0U; word < attribute_count; word++)
				vertex[3U + word] = attributes[word];
			continue;
		}

		/* Any other position has w = 1, and the value follows it. */
		vertex[3] = I915_VKC_F_1;
		if (corner_values != 0) {
			vertex[4] = pixel_edges[x];
			vertex[5] = pixel_edges[y];
		} else {
			for (word = 0U; word < attribute_count; word++)
				vertex[4U + word] = attributes[word];
		}
	}
}

/* Fills every component of every pixel with the clear value and writes it back to memory. */
static void
i915_vkc_clear(
	struct i915_vkc *x)
{
	uint32_t *pixels;
	uint32_t index;

	/* Every component holds the clear value. */
	pixels = (uint32_t *)(void *)(x->cpu + I915_VKC_TARGET_OFFSET);
	for (index = 0U; index < I915_VKC_SIZE * I915_VKC_SIZE * 4U; index++)
		pixels[index] = I915_VKC_CLEAR_BITS;

	/* The GPU reads nothing of it, but its writes must not meet a stale line later. */
	drv_i915_gt_clflush(pixels, I915_VKC_SIZE * I915_VKC_SIZE * I915_VKC_PIXEL_BYTES);
}

/* Runs one step: compiles its pipeline, draws, checks the target and logs the verdict. */
static void
i915_vkc_step_run(
	struct i915_vkc *x,
	const struct i915_vkc_step *step)
{
	unsigned wrong;
	int error;

	/* Compiles the two shaders into the step's pipeline. */
	i915_vkc_pipeline_init(x, step);
	error = drv_i915_gfx_pipeline_prepare(x->render, &x->pipeline);
	if (error != 0) {
		kern_logf("i915: vkc: VKC-%s FAIL (the pipeline was refused: %d)\n", step->name, error);
		drv_i915_gfx_pipeline_release(&x->pipeline);
		x->failed++;
		return;
	}

	/* Writes the vertices and clears the target. */
	kern_memset(&x->args, 0, sizeof(x->args));
	x->args.count = i915_vkc_vertices_write(x, step);
	x->args.instance_count = 1U;
	i915_vkc_clear(x);

	/* The draw state: the pass, the framebuffer, the pipeline, the vertices, mview's push constants. */
	kern_memset(&x->state, 0, sizeof(x->state));
	x->state.pass = &x->pass;
	x->state.framebuffer = &x->framebuffer;
	x->state.pipeline = &x->pipeline;
	x->state.vertex[0].buffer = &x->vertices;
	x->state.vertex[0].offset = 0U;
	kern_memcpy(x->state.push, i915_vkc_mview_push, sizeof(i915_vkc_mview_push));

	/* Draws; the draw runs to its end on the GPU before it returns. */
	error = drv_i915_gfx_draw(x->render, &x->state, &x->args);
	if (error != 0) {
		kern_logf("i915: vkc: VKC-%s FAIL (the draw failed: %d)\n", step->name, error);
		drv_i915_gfx_pipeline_release(&x->pipeline);
		x->failed++;
		return;
	}

	/* Compares the target with what the step expects. */
	wrong = i915_vkc_check(x, step);
	if (wrong != 0U) {
		kern_logf("i915: vkc: VKC-%s FAIL (%u of %u components out of bounds; kernels %u + %u bytes)\n",
			  step->name,
			  wrong,
			  I915_VKC_SIZE * I915_VKC_SIZE * 4U,
			  x->pipeline.vs_binary->code_bytes,
			  x->pipeline.fs_binary->code_bytes);
		drv_i915_gfx_pipeline_release(&x->pipeline);
		x->failed++;
		return;
	}

	/* Says what passed: every component of every pixel. */
	kern_logf("i915: vkc: VKC-%s PASS (%u components; kernels %u + %u bytes, fragment discards: %u)\n",
		  step->name,
		  I915_VKC_SIZE * I915_VKC_SIZE * 4U,
		  x->pipeline.vs_binary->code_bytes,
		  x->pipeline.fs_binary->code_bytes,
		  x->pipeline.fs_binary->uses_kill);
	drv_i915_gfx_pipeline_release(&x->pipeline);
	x->passed++;
}

/*
 * Compares every component of every pixel with the step's expectation and
 * returns how many are wrong, logging the first few: inside [lo, hi] of its
 * cell for a cell step, equal to its class colour for a pixel step.
 */
static unsigned
i915_vkc_check(
	struct i915_vkc *x,
	const struct i915_vkc_step *step)
{
	const uint32_t *pixels;
	uint32_t pixel;
	uint32_t component;
	uint32_t cell;
	uint32_t got;
	uint32_t lo;
	uint32_t hi;
	uint32_t want;
	uint32_t got_key;
	unsigned wrong;
	int outside;

	/* Reads the target through the CPU view, past any line the CPU still caches. */
	pixels = (const uint32_t *)(const void *)(x->cpu + I915_VKC_TARGET_OFFSET);
	drv_i915_gt_clflush(pixels, I915_VKC_SIZE * I915_VKC_SIZE * I915_VKC_PIXEL_BYTES);

	/* Checks each component of each pixel. */
	wrong = 0U;
	for (pixel = 0U; pixel < I915_VKC_SIZE * I915_VKC_SIZE; pixel++) {
		for (component = 0U; component < 4U; component++) {
			got = pixels[pixel * 4U + component];

			/* A pixel step expects its class colour exactly; a cell step a key inside the bounds. */
			outside = 0;
			if (step->classes != NULL) {
				want = step->colors[step->classes[pixel] * 4U + component];
				lo = want;
				hi = want;
				if (got != want)
					outside = 1;
			} else {
				cell = (pixel / I915_VKC_SIZE / I915_VKC_CELL) * I915_VKC_CELLS_ACROSS +
				    (pixel % I915_VKC_SIZE) / I915_VKC_CELL;
				lo = step->bounds[(cell * 4U + component) * 2U];
				hi = step->bounds[(cell * 4U + component) * 2U + 1U];
				got_key = i915_vkc_key(got);
				if (got_key < lo || got_key > hi)
					outside = 1;
			}

			/* Counts a wrong component and logs the first ones. */
			if (outside == 0)
				continue;
			if (wrong < I915_VKC_LOG_LIMIT) {
				kern_logf("i915: vkc: %s: pixel (%u,%u) component %u: 0x%08x, expected [0x%08x, 0x%08x]%s\n",
					  step->name,
					  pixel % I915_VKC_SIZE,
					  pixel / I915_VKC_SIZE,
					  component,
					  got,
					  lo,
					  hi,
					  step->classes != NULL ? " (float bits)" : " (ordered keys)");
			}
			wrong++;
		}
	}

	/* Reports the wrong components; zero when the target is as expected. */
	return wrong;
}

/* Returns the ordered key of float bits: integer order is float order. */
static uint32_t
i915_vkc_key(
	uint32_t bits)
{
	/* A negative float orders by its inverted bits, a positive one above every negative one. */
	if ((bits & 0x80000000U) != 0U)
		return ~bits;

	/* Succeeded: the key of a positive float. */
	return bits | 0x80000000U;
}
