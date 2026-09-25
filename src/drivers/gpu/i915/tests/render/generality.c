/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The generality scenario "vke2": shaders beyond the model viewer's --
 * matrices, integer arithmetic, float remainders and rounding, loops with
 * per-pixel trip counts, sixteen varyings and sixteen vertex attributes --
 * recorded through the wire, run on the GPU and checked against the words
 * generality-shaders/regenerate.py computed.
 *
 * Like the feature scenario, the thread waits for the node, opens a session
 * of its own and drives the executor with the commands libvulkan sends; the
 * objects the commands name are made directly and published in the
 * executor's object table.  Every fragment shader writes the four bytes of
 * one 32-bit word per pixel of a 64x64 RGBA8 target.  The steps:
 *
 *  - MATRIX: a vertex shader placing the quad by transpose(row-major) *
 *    column-major matrices of a uniform block, a fragment shader computing
 *    matrix products of a uniform block (column- and row-major, a mat3) and
 *    of push constants (row- and column-major), a local matrix, transpose,
 *    outer product, matrix times scalar; each pixel the bits of one result;
 *  - INT: sixteen integer operations over values made from the pixel by an
 *    integer hash (negatives, all 32 bits), divisors powers of two and not;
 *  - FLOAT: mod by positive and negative divisors, roundEven / round, trunc,
 *    ceil, sign, step, floor, smoothstep, fract, abs;
 *  - LOOP: loops to x % 17 with continue and break, a nested while with an
 *    if / else and a break, a do-while and a while (true);
 *  - VARY16: sixteen varyings from the vertex shader, all read;
 *  - SUBSET: the same vertex shader, a fragment shader reading five of them;
 *  - VIN16: sixteen vertex attributes;
 *  - SPILL: a fragment shader keeping more values live than the EU has
 *    registers, a loop with per-pixel trip counts among them; the compiler
 *    spills to scratch memory and the draw gives the kernel its scratch;
 *  - VIO16: a vertex shader reading sixteen attributes and writing sixteen
 *    varyings (it gathers its VUE and spills), read by vary16.frag.
 *
 * Each step logs "VKE2-<name> PASS" or "FAIL", then the thread logs the
 * verdict and closes the session.
 */

#include "scenarios.h"
#include <kern/kcrt.h>

#include "../../compiler/compiler.h"
#include "../../i915.h"
#include "../../memory.h"
#include "../../session.h"
#include "../../sync.h"
#include "../../render/gfx.h"
#include "../../render/internal.h"
#include "../../render/object.h"
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

/* The side of the square target, in pixels. */
#define I915_VKE2_SIZE			64U

/* The storage every buffer and the target are bound in, and where each is. */
#define I915_VKE2_STORAGE_BYTES		(256U * 1024U)
#define I915_VKE2_TARGET_OFFSET		0x00000U
#define I915_VKE2_QUADS_OFFSET		0x10000U
#define I915_VKE2_SEEDED_OFFSET		0x11000U
#define I915_VKE2_WIDE_OFFSET		0x12000U
#define I915_VKE2_VERTEX_BYTES		0x01000U
#define I915_VKE2_INDEX_OFFSET		0x13000U
#define I915_VKE2_INDEX_BYTES		0x00100U
#define I915_VKE2_MATRICES_OFFSET	0x20000U
#define I915_VKE2_PLACEMENT_OFFSET	0x21000U
#define I915_VKE2_UNIFORM_BYTES		0x00100U

/*
 * The vertex layouts: a quad vertex is a position and a coordinate (vec4
 * each); a seeded one adds vary16.vert's seed; a wide one has vin16.vert's
 * sixteen vec4 attributes.
 */
#define I915_VKE2_QUAD_STRIDE		32U
#define I915_VKE2_SEEDED_STRIDE		48U
#define I915_VKE2_WIDE_STRIDE		256U
#define I915_VKE2_WIDE_ATTRIBUTES	16U

/* The quad buffer holds the matrix step's quad first, then the plain full-target quad. */
#define I915_VKE2_MATRIX_VERTEX		0U
#define I915_VKE2_PLAIN_VERTEX		4U

/* The bytes of push constants the matrix step pushes (matrix.frag's block). */
#define I915_VKE2_PUSH_BYTES		128U

/* How many units in the last place a float step's word may be off (the reciprocal of smoothstep's span). */
#define I915_VKE2_FLOAT_ULPS		4U

/* The wire identities the scenario publishes its objects under, apart from any client's and the other scenarios'. */
#define I915_VKE2_IDENTITY		0x7e5e200000000000ULL
#define I915_VKE2_ID_QUADS		(I915_VKE2_IDENTITY + 1U)
#define I915_VKE2_ID_SEEDED		(I915_VKE2_IDENTITY + 2U)
#define I915_VKE2_ID_WIDE		(I915_VKE2_IDENTITY + 3U)
#define I915_VKE2_ID_INDICES		(I915_VKE2_IDENTITY + 4U)
#define I915_VKE2_ID_PASS		(I915_VKE2_IDENTITY + 5U)
#define I915_VKE2_ID_FRAMEBUFFER	(I915_VKE2_IDENTITY + 6U)
#define I915_VKE2_ID_POOL		(I915_VKE2_IDENTITY + 7U)
#define I915_VKE2_ID_COMMAND_BUFFER	(I915_VKE2_IDENTITY + 8U)
#define I915_VKE2_ID_MATRICES		(I915_VKE2_IDENTITY + 9U)
#define I915_VKE2_ID_PLACEMENT		(I915_VKE2_IDENTITY + 10U)
#define I915_VKE2_ID_MATRIX_SET		(I915_VKE2_IDENTITY + 11U)
#define I915_VKE2_ID_PIPELINE		(I915_VKE2_IDENTITY + 0x100U)

/* The wire opcodes the scenario sends, as libvulkan numbers them. */
#define I915_VKE2_OP_QUEUE_SUBMIT		18U
#define I915_VKE2_OP_UPDATE_DESCRIPTOR_SETS	79U
#define I915_VKE2_OP_CREATE_COMMAND_POOL	85U
#define I915_VKE2_OP_DESTROY_COMMAND_POOL	86U
#define I915_VKE2_OP_ALLOCATE_COMMAND_BUFFERS	88U
#define I915_VKE2_OP_BEGIN_COMMAND_BUFFER	90U
#define I915_VKE2_OP_END_COMMAND_BUFFER		91U
#define I915_VKE2_OP_BIND_PIPELINE		93U
#define I915_VKE2_OP_BIND_DESCRIPTOR_SETS	103U
#define I915_VKE2_OP_BIND_INDEX_BUFFER		104U
#define I915_VKE2_OP_BIND_VERTEX_BUFFERS	105U
#define I915_VKE2_OP_DRAW_INDEXED		107U
#define I915_VKE2_OP_PUSH_CONSTANTS		132U
#define I915_VKE2_OP_BEGIN_RENDER_PASS		133U
#define I915_VKE2_OP_END_RENDER_PASS		135U

/* The largest stream the scenario builds, and the room for its replies. */
#define I915_VKE2_WIRE_BYTES		4096U
#define I915_VKE2_REPLY_BYTES		1024U

/* How long the thread waits for the node to be published, in seconds. */
#define I915_VKE2_WAIT_S		120U

/* IEEE-754 bits of the floats the scenario itself uses; the kernel computes no float. */
#define I915_VKE2_F_0			0x00000000U
#define I915_VKE2_F_1			0x3f800000U
#define I915_VKE2_F_MINUS_1		0xbf800000U
#define I915_VKE2_F_64			0x42800000U

/* How the words of a step are compared: bit for bit, or as floats within some units in the last place. */
#define I915_VKE2_COMPARE_EXACT		0U
#define I915_VKE2_COMPARE_FLOAT		1U

/* The pipelines, in the order of their identities. */
#define I915_VKE2_PIPE_MATRIX		0U
#define I915_VKE2_PIPE_INT		1U
#define I915_VKE2_PIPE_FLOAT		2U
#define I915_VKE2_PIPE_LOOP		3U
#define I915_VKE2_PIPE_VARY16		4U
#define I915_VKE2_PIPE_SUBSET		5U
#define I915_VKE2_PIPE_VIN16		6U
#define I915_VKE2_PIPE_SPILL		7U
#define I915_VKE2_PIPE_VIO16		8U
#define I915_VKE2_PIPELINES		9U

/* The shader modules. */
#define I915_VKE2_SHADER_QUAD_VERT	0U
#define I915_VKE2_SHADER_MATRIX_VERT	1U
#define I915_VKE2_SHADER_MATRIX_FRAG	2U
#define I915_VKE2_SHADER_INT_FRAG	3U
#define I915_VKE2_SHADER_FLOAT_FRAG	4U
#define I915_VKE2_SHADER_LOOP_FRAG	5U
#define I915_VKE2_SHADER_VARY16_VERT	6U
#define I915_VKE2_SHADER_VARY16_FRAG	7U
#define I915_VKE2_SHADER_SUBSET_FRAG	8U
#define I915_VKE2_SHADER_VIN16_VERT	9U
#define I915_VKE2_SHADER_VIN16_FRAG	10U
#define I915_VKE2_SHADER_SPILL_FRAG	11U
#define I915_VKE2_SHADER_VIO16_VERT	12U
#define I915_VKE2_SHADERS		13U

#include "../fixtures/generality-shaders-gen.inc"

/*
 * Everything the scenario owns while its thread runs.
 *
 * It is filled by the setup, used by the steps and emptied by the teardown,
 * all on the scenario's thread; the scenario runs once per boot.
 */
struct i915_vke2 {
	/* The device, and the node and executor sessions the scenario opened. */
	struct i915_device *device;
	struct i915_session *session;
	struct i915_render_session *render;

	/* The storage object and its CPU view; the memory that names it. */
	struct i915_gem_object *storage;
	uint8_t *cpu;
	struct i915_gfx_memory memory;

	/* The target, its view, the render pass and the framebuffer. */
	struct i915_gfx_image target;
	struct i915_gfx_view view;
	struct i915_gfx_pass pass;
	struct i915_gfx_framebuffer framebuffer;

	/* The three vertex buffers, the index buffer and the matrix step's two uniform buffers. */
	struct i915_gfx_buffer quads;
	struct i915_gfx_buffer seeded;
	struct i915_gfx_buffer wide;
	struct i915_gfx_buffer indices;
	struct i915_gfx_buffer matrices;
	struct i915_gfx_buffer placement;

	/* The shader modules and the pipelines. */
	struct i915_gfx_shader shaders[I915_VKE2_SHADERS];
	struct i915_gfx_pipeline pipelines[I915_VKE2_PIPELINES];

	/* The matrix step's layout (binding 0 for the fragment stage, 1 for the vertex stage) and set. */
	struct i915_gfx_dsl matrix_layout;
	struct i915_gfx_dset matrix_set;

	/* Nonzero once the objects are published, and once the command pool exists. */
	int published;
	int pooled;

	/* The stream under construction, and nonzero once it overflowed. */
	uint8_t wire[I915_VKE2_WIRE_BYTES];
	size_t used;
	int overflow;

	/* The replies of the last stream, and how many bytes they took. */
	uint8_t reply[I915_VKE2_REPLY_BYTES];
	size_t reply_bytes;

	/* The words a step expects the target to hold. */
	uint32_t expected[I915_VKE2_SIZE * I915_VKE2_SIZE];

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
static struct i915_vke2 i915_vke2_state;

static void i915_vke2_thread(void *argument);
static int i915_vke2_wait_node(struct i915_device *device);
static int i915_vke2_setup(struct i915_vke2 *x);
static void i915_vke2_teardown(struct i915_vke2 *x);
static int i915_vke2_storage_create(struct i915_vke2 *x);
static void i915_vke2_objects_init(struct i915_vke2 *x);
static void i915_vke2_buffer_init(struct i915_vke2 *x, struct i915_gfx_buffer *buffer, uint64_t size, uint32_t usage, uint64_t offset);
static void i915_vke2_shader_init(struct i915_gfx_shader *shader, const uint32_t *words, uint32_t bytes);
static void i915_vke2_pipeline_init(struct i915_gfx_pipeline *pipeline, struct i915_gfx_shader *vertex, struct i915_gfx_shader *fragment, uint32_t stride, uint32_t attributes);
static int i915_vke2_objects_publish(struct i915_vke2 *x);
static void i915_vke2_objects_withdraw(struct i915_vke2 *x);
static int i915_vke2_pipelines_prepare(struct i915_vke2 *x);
static void i915_vke2_pipelines_release(struct i915_vke2 *x);
static void i915_vke2_data_write(struct i915_vke2 *x);
static void i915_vke2_corner_write(uint32_t *words, unsigned corner, uint32_t x, uint32_t y);
static void i915_vke2_put32(struct i915_vke2 *x, uint32_t value);
static void i915_vke2_put64(struct i915_vke2 *x, uint64_t value);
static void i915_vke2_record(struct i915_vke2 *x, uint32_t opcode);
static int i915_vke2_execute(struct i915_vke2 *x, const char *what);
static uint32_t i915_vke2_reply32(const struct i915_vke2 *x, size_t offset);
static int i915_vke2_pool_create(struct i915_vke2 *x);
static void i915_vke2_begin(struct i915_vke2 *x);
static void i915_vke2_begin_pass(struct i915_vke2 *x);
static void i915_vke2_bind_pipeline(struct i915_vke2 *x, uint32_t pipeline);
static void i915_vke2_bind_buffers(struct i915_vke2 *x, uint64_t vertices);
static void i915_vke2_draw_quad(struct i915_vke2 *x, uint32_t first_vertex);
static void i915_vke2_bind_set(struct i915_vke2 *x, uint64_t set);
static void i915_vke2_push_constants(struct i915_vke2 *x, const uint32_t *block);
static void i915_vke2_buffer_write(struct i915_vke2 *x, uint64_t set, uint32_t binding, uint64_t buffer, uint64_t range);
static int i915_vke2_finish(struct i915_vke2 *x, const char *what);
static uint32_t i915_vke2_half_bits(uint32_t twice);
static void i915_vke2_expect_sums(struct i915_vke2 *x, uint32_t twice);
static int i915_vke2_near(uint32_t word, uint32_t expected, uint32_t mode);
static int i915_vke2_compare(struct i915_vke2 *x, const char *what, uint32_t mode);
static void i915_vke2_verdict(struct i915_vke2 *x, const char *what, int error);
static void i915_vke2_step_draw(struct i915_vke2 *x, const char *what, uint32_t pipeline, uint64_t vertices, uint32_t first_vertex, uint32_t mode);
static void i915_vke2_step_matrix(struct i915_vke2 *x);

/*
 * Starts the generality scenario.
 *
 * Returns at once: the scenario's thread runs the steps after the node is
 * published and logs their verdicts itself.
 */
void
drv_i915_test_render_generality(
	struct i915_device *device)
{
	struct thread *thread;
	int error;

	/* Starts the thread that waits for the node and runs the steps. */
	error = kthread_create(i915_vke2_thread, device, SCHED_PRIORITY_DEFAULT, &thread);
	if (error != 0) {
		kern_logf("i915: vke2: verdict FAIL (the scenario thread cannot be created: %d)\n", error);
		return;
	}

	/* The thread reclaims itself when the steps are done. */
	thread->detached = 1U;
	thread_start(thread);
	kern_logf("i915: vke2: the steps run once the node is published\n");
}

/* Runs the scenario: waits for the node, sets up, runs every step, tears down and logs the verdict. */
static void
i915_vke2_thread(
	void *argument)
{
	struct i915_device *device;
	struct i915_vke2 *x;
	int error;

	/* Waits until the node is published and its worker serves. */
	device = argument;
	error = i915_vke2_wait_node(device);
	if (error != 0) {
		kern_logf("i915: vke2: verdict FAIL (the node was not published within %u s)\n", I915_VKE2_WAIT_S);
		return;
	}

	/* Opens the session and makes every object the steps use. */
	x = &i915_vke2_state;
	kern_memset(x, 0, sizeof(*x));
	x->device = device;
	error = i915_vke2_setup(x);
	if (error != 0) {
		kern_logf("i915: vke2: verdict FAIL (setup: %d)\n", error);
		i915_vke2_teardown(x);
		return;
	}

	/* Runs every step; each logs its own verdict. */
	i915_vke2_step_matrix(x);
	i915_vke2_step_draw(x, "INT", I915_VKE2_PIPE_INT, I915_VKE2_ID_QUADS, I915_VKE2_PLAIN_VERTEX, I915_VKE2_COMPARE_EXACT);
	i915_vke2_step_draw(x, "FLOAT", I915_VKE2_PIPE_FLOAT, I915_VKE2_ID_QUADS, I915_VKE2_PLAIN_VERTEX, I915_VKE2_COMPARE_FLOAT);
	i915_vke2_step_draw(x, "LOOP", I915_VKE2_PIPE_LOOP, I915_VKE2_ID_QUADS, I915_VKE2_PLAIN_VERTEX, I915_VKE2_COMPARE_EXACT);
	i915_vke2_step_draw(x, "VARY16", I915_VKE2_PIPE_VARY16, I915_VKE2_ID_SEEDED, 0U, I915_VKE2_COMPARE_EXACT);
	i915_vke2_step_draw(x, "SUBSET", I915_VKE2_PIPE_SUBSET, I915_VKE2_ID_SEEDED, 0U, I915_VKE2_COMPARE_EXACT);
	i915_vke2_step_draw(x, "VIN16", I915_VKE2_PIPE_VIN16, I915_VKE2_ID_WIDE, 0U, I915_VKE2_COMPARE_EXACT);
	i915_vke2_step_draw(x, "SPILL", I915_VKE2_PIPE_SPILL, I915_VKE2_ID_QUADS, I915_VKE2_PLAIN_VERTEX, I915_VKE2_COMPARE_EXACT);
	i915_vke2_step_draw(x, "VIO16", I915_VKE2_PIPE_VIO16, I915_VKE2_ID_WIDE, 0U, I915_VKE2_COMPARE_EXACT);

	/* Gives everything back and says how the steps went. */
	i915_vke2_teardown(x);
	if (x->failed != 0U) {
		kern_logf("i915: vke2: verdict FAIL (%u of %u steps passed)\n", x->passed, x->passed + x->failed);
		return;
	}

	kern_logf("i915: vke2: verdict PASS (%u of %u steps passed)\n", x->passed, x->passed + x->failed);
}

/* Waits until the node is published, sleeping a twentieth of a second at a time; ETIMEDOUT when it never is. */
static int
i915_vke2_wait_node(
	struct i915_device *device)
{
	struct i915_completion nap;
	unsigned waited;

	/* A completion nobody signals: each wait on it simply lasts until its deadline. */
	drv_i915_completion_init(&nap, "i915 vke2");

	/* Looks for the published node until the budget is spent. */
	for (waited = 0U; waited < I915_VKE2_WAIT_S * 20U; waited++) {
		/* The node is published once the GPU core holds it. */
		if (device->gpu != NULL && device->vk != NULL)
			return 0;

		(void)drv_i915_wait_for_completion(&nap, sched_ticks() + KERN_CLOCK_HZ / 20U);
	}

	/* The node never came. */
	return ETIMEDOUT;
}

/* Opens the session, makes the storage, the objects and the pipelines, and a command pool. */
static int
i915_vke2_setup(
	struct i915_vke2 *x)
{
	void *private_session;
	int error;

	/* Opens a session of the node as a client's open would. */
	error = x->device->gpu_ops.open(x->device, &private_session);
	if (error != 0)
		return error;

	x->session = private_session;
	x->render = x->session->vk;
	if (x->render == NULL)
		return ENODEV;

	/* Makes the storage, bound into the session's address space. */
	error = i915_vke2_storage_create(x);
	if (error != 0)
		return error;

	/* Describes the objects over the storage and publishes them. */
	i915_vke2_objects_init(x);
	error = i915_vke2_objects_publish(x);
	if (error != 0)
		return error;

	/* Compiles every pipeline but the one that must be refused. */
	error = i915_vke2_pipelines_prepare(x);
	if (error != 0)
		return error;

	/* Writes the vertices, the indices and the uniform blocks. */
	i915_vke2_data_write(x);

	/* Creates the command pool and its one command buffer through the wire. */
	error = i915_vke2_pool_create(x);
	if (error != 0)
		return error;

	/* Succeeded: every step can record and submit. */
	return 0;
}

/* Gives back everything the setup made, whatever it got to. */
static void
i915_vke2_teardown(
	struct i915_vke2 *x)
{
	struct i915_device *device;
	int error;

	/* Destroys the command pool and its buffer through the wire. */
	if (x->pooled != 0) {
		x->used = 0U;
		x->overflow = 0;
		i915_vke2_put32(x, I915_VKE2_OP_DESTROY_COMMAND_POOL);
		i915_vke2_put32(x, 1U);
		i915_vke2_put64(x, 0U);
		i915_vke2_put64(x, I915_VKE2_ID_POOL);
		i915_vke2_put64(x, 0U);
		error = i915_vke2_execute(x, "destroy the command pool");
		if (error != 0)
			kern_logf("i915: vke2: the command pool was not destroyed: %d\n", error);
		x->pooled = 0;
	}

	/* Withdraws the published objects and releases the kernels. */
	if (x->published != 0)
		i915_vke2_objects_withdraw(x);
	i915_vke2_pipelines_release(x);

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

/* Makes the storage object, bound into the session's address space. */
static int
i915_vke2_storage_create(
	struct i915_vke2 *x)
{
	struct i915_device *device;
	int error;

	/* Creates the object and binds it, destroying it again when the binding fails. */
	device = x->device;
	mutex_lock(&device->mutex);

	error = drv_i915_gem_create(&device->gem, I915_VKE2_STORAGE_BYTES, &x->storage);
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

	/* Succeeded: the storage is bound. */
	return 0;
}

/* Describes every object the steps use over the storage. */
static void
i915_vke2_objects_init(
	struct i915_vke2 *x)
{
	static const struct {
		const uint32_t *words;
		uint32_t bytes;
	} modules[I915_VKE2_SHADERS] = {
		{ i915_vke2_quad_vert, sizeof(i915_vke2_quad_vert) },
		{ i915_vke2_matrix_vert, sizeof(i915_vke2_matrix_vert) },
		{ i915_vke2_matrix_frag, sizeof(i915_vke2_matrix_frag) },
		{ i915_vke2_int_frag, sizeof(i915_vke2_int_frag) },
		{ i915_vke2_float_frag, sizeof(i915_vke2_float_frag) },
		{ i915_vke2_loop_frag, sizeof(i915_vke2_loop_frag) },
		{ i915_vke2_vary16_vert, sizeof(i915_vke2_vary16_vert) },
		{ i915_vke2_vary16_frag, sizeof(i915_vke2_vary16_frag) },
		{ i915_vke2_subset_frag, sizeof(i915_vke2_subset_frag) },
		{ i915_vke2_vin16_vert, sizeof(i915_vke2_vin16_vert) },
		{ i915_vke2_vin16_frag, sizeof(i915_vke2_vin16_frag) },
		{ i915_vke2_spill_frag, sizeof(i915_vke2_spill_frag) },
		{ i915_vke2_vio16_vert, sizeof(i915_vke2_vio16_vert) },
	};
	struct i915_gfx_shader *shaders;
	uint32_t index;

	/* The memory is the whole storage. */
	x->memory.vk = x->device->vk;
	x->memory.identity = I915_VKE2_IDENTITY;
	x->memory.size = I915_VKE2_STORAGE_BYTES;
	x->memory.object = x->storage;

	/* The target: a linear 64x64 RGBA8 image. */
	x->target.format = VK_FORMAT_R8G8B8A8_UNORM;
	x->target.width = I915_VKE2_SIZE;
	x->target.height = I915_VKE2_SIZE;
	x->target.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
	x->target.pitch = I915_VKE2_SIZE * 4U;
	x->target.bytes = I915_VKE2_SIZE * I915_VKE2_SIZE * 4U;
	x->target.levels = 1U;
	x->target.memory = &x->memory;
	x->target.offset = I915_VKE2_TARGET_OFFSET;
	x->view.image = &x->target;
	x->view.format = VK_FORMAT_R8G8B8A8_UNORM;
	x->view.level_count = 1U;

	/* One subpass writing the one colour attachment, cleared at the begin. */
	x->pass.attachment_count = 1U;
	x->pass.attachments[0].format = VK_FORMAT_R8G8B8A8_UNORM;
	x->pass.attachments[0].load_op = VK_ATTACHMENT_LOAD_OP_CLEAR;
	x->pass.color_attachment = 0U;
	x->pass.depth_attachment = VK_ATTACHMENT_UNUSED;
	x->framebuffer.width = I915_VKE2_SIZE;
	x->framebuffer.height = I915_VKE2_SIZE;
	x->framebuffer.view_count = 1U;
	x->framebuffer.views[0] = &x->view;

	/* The vertex buffers, the index buffer and the two uniform buffers. */
	i915_vke2_buffer_init(x, &x->quads, I915_VKE2_VERTEX_BYTES, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, I915_VKE2_QUADS_OFFSET);
	i915_vke2_buffer_init(x, &x->seeded, I915_VKE2_VERTEX_BYTES, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, I915_VKE2_SEEDED_OFFSET);
	i915_vke2_buffer_init(x, &x->wide, I915_VKE2_VERTEX_BYTES, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, I915_VKE2_WIDE_OFFSET);
	i915_vke2_buffer_init(x, &x->indices, I915_VKE2_INDEX_BYTES, VK_BUFFER_USAGE_INDEX_BUFFER_BIT, I915_VKE2_INDEX_OFFSET);
	i915_vke2_buffer_init(x, &x->matrices, I915_VKE2_UNIFORM_BYTES, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, I915_VKE2_MATRICES_OFFSET);
	i915_vke2_buffer_init(x, &x->placement, I915_VKE2_UNIFORM_BYTES, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, I915_VKE2_PLACEMENT_OFFSET);

	/* The shader modules borrow the generated words, which the compiler only reads. */
	for (index = 0U; index < I915_VKE2_SHADERS; index++)
		i915_vke2_shader_init(&x->shaders[index], modules[index].words, modules[index].bytes);

	/* The pipelines: a vertex and a fragment shader over their vertex layout. */
	shaders = x->shaders;
	i915_vke2_pipeline_init(&x->pipelines[I915_VKE2_PIPE_MATRIX],
				&shaders[I915_VKE2_SHADER_MATRIX_VERT],
				&shaders[I915_VKE2_SHADER_MATRIX_FRAG],
				I915_VKE2_QUAD_STRIDE,
				2U);
	i915_vke2_pipeline_init(&x->pipelines[I915_VKE2_PIPE_INT],
				&shaders[I915_VKE2_SHADER_QUAD_VERT],
				&shaders[I915_VKE2_SHADER_INT_FRAG],
				I915_VKE2_QUAD_STRIDE,
				2U);
	i915_vke2_pipeline_init(&x->pipelines[I915_VKE2_PIPE_FLOAT],
				&shaders[I915_VKE2_SHADER_QUAD_VERT],
				&shaders[I915_VKE2_SHADER_FLOAT_FRAG],
				I915_VKE2_QUAD_STRIDE,
				2U);
	i915_vke2_pipeline_init(&x->pipelines[I915_VKE2_PIPE_LOOP],
				&shaders[I915_VKE2_SHADER_QUAD_VERT],
				&shaders[I915_VKE2_SHADER_LOOP_FRAG],
				I915_VKE2_QUAD_STRIDE,
				2U);
	i915_vke2_pipeline_init(&x->pipelines[I915_VKE2_PIPE_VARY16],
				&shaders[I915_VKE2_SHADER_VARY16_VERT],
				&shaders[I915_VKE2_SHADER_VARY16_FRAG],
				I915_VKE2_SEEDED_STRIDE,
				3U);
	i915_vke2_pipeline_init(&x->pipelines[I915_VKE2_PIPE_SUBSET],
				&shaders[I915_VKE2_SHADER_VARY16_VERT],
				&shaders[I915_VKE2_SHADER_SUBSET_FRAG],
				I915_VKE2_SEEDED_STRIDE,
				3U);
	i915_vke2_pipeline_init(&x->pipelines[I915_VKE2_PIPE_VIN16],
				&shaders[I915_VKE2_SHADER_VIN16_VERT],
				&shaders[I915_VKE2_SHADER_VIN16_FRAG],
				I915_VKE2_WIDE_STRIDE,
				I915_VKE2_WIDE_ATTRIBUTES);
	i915_vke2_pipeline_init(&x->pipelines[I915_VKE2_PIPE_SPILL],
				&shaders[I915_VKE2_SHADER_QUAD_VERT],
				&shaders[I915_VKE2_SHADER_SPILL_FRAG],
				I915_VKE2_QUAD_STRIDE,
				2U);
	i915_vke2_pipeline_init(&x->pipelines[I915_VKE2_PIPE_VIO16],
				&shaders[I915_VKE2_SHADER_VIO16_VERT],
				&shaders[I915_VKE2_SHADER_VARY16_FRAG],
				I915_VKE2_WIDE_STRIDE,
				I915_VKE2_WIDE_ATTRIBUTES);

	/* The matrix step's layout: binding 0 for the fragment stage, binding 1 for the vertex stage. */
	x->matrix_layout.count = 2U;
	x->matrix_layout.bindings[0].binding = 0U;
	x->matrix_layout.bindings[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
	x->matrix_layout.bindings[0].stages = VK_SHADER_STAGE_FRAGMENT_BIT;
	x->matrix_layout.bindings[1].binding = 1U;
	x->matrix_layout.bindings[1].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
	x->matrix_layout.bindings[1].stages = VK_SHADER_STAGE_VERTEX_BIT;

	/* The set starts empty; vkUpdateDescriptorSets fills it. */
	x->matrix_set.layout = &x->matrix_layout;
}

/* Describes one buffer bound at an offset of the storage. */
static void
i915_vke2_buffer_init(
	struct i915_vke2 *x,
	struct i915_gfx_buffer *buffer,
	uint64_t size,
	uint32_t usage,
	uint64_t offset)
{
	/* The size and the usage it was created with, and where it is bound. */
	buffer->size = size;
	buffer->usage = usage;
	buffer->memory = &x->memory;
	buffer->offset = offset;
}

/* Describes one shader module over generated SPIR-V words. */
static void
i915_vke2_shader_init(
	struct i915_gfx_shader *shader,
	const uint32_t *words,
	uint32_t bytes)
{
	/* The module borrows the words, which the compiler only reads. */
	shader->words = (uint32_t *)(uintptr_t)words;
	shader->word_count = bytes / 4U;
}

/*
 * Describes one pipeline: two shaders, `attributes` vec4 attributes at
 * locations 0, 1, ... one after the other in the vertices of binding 0, a
 * triangle list, no culling and no depth, the whole target as viewport and
 * scissor, every component written without blending.
 */
static void
i915_vke2_pipeline_init(
	struct i915_gfx_pipeline *pipeline,
	struct i915_gfx_shader *vertex,
	struct i915_gfx_shader *fragment,
	uint32_t stride,
	uint32_t attributes)
{
	uint32_t index;

	/* The two stages. */
	kern_memset(pipeline, 0, sizeof(*pipeline));
	pipeline->vertex = vertex;
	pipeline->fragment = fragment;

	/* Binding 0 of `stride`-byte vertices, attribute k a vec4 at 16 k bytes. */
	pipeline->binding_count = 1U;
	pipeline->bindings[0].binding = 0U;
	pipeline->bindings[0].stride = stride;
	pipeline->attribute_count = attributes;
	for (index = 0U; index < attributes; index++) {
		pipeline->attributes[index].location = index;
		pipeline->attributes[index].binding = 0U;
		pipeline->attributes[index].format = VK_FORMAT_R32G32B32A32_SFLOAT;
		pipeline->attributes[index].offset = 16U * index;
	}

	/* A triangle list, drawn from both sides with no depth test. */
	pipeline->topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
	pipeline->cull_mode = VK_CULL_MODE_NONE;

	/* The whole target, depth range [0, 1]. */
	pipeline->viewport[0] = I915_VKE2_F_0;
	pipeline->viewport[1] = I915_VKE2_F_0;
	pipeline->viewport[2] = I915_VKE2_F_64;
	pipeline->viewport[3] = I915_VKE2_F_64;
	pipeline->viewport[4] = I915_VKE2_F_0;
	pipeline->viewport[5] = I915_VKE2_F_1;
	pipeline->scissor.extent.width = I915_VKE2_SIZE;
	pipeline->scissor.extent.height = I915_VKE2_SIZE;
}

/* Publishes the objects the wire names under the scenario's identities. */
static int
i915_vke2_objects_publish(
	struct i915_vke2 *x)
{
	struct i915_render_device *vk;
	uint32_t index;
	int error;

	/* Publishes the buffers, the pass, the framebuffer and the set, stopping at the first refusal. */
	vk = x->device->vk;
	error = drv_i915_object_insert(vk, I915_VK_OBJ_BUFFER, I915_VKE2_ID_QUADS, &x->quads);
	if (error == 0)
		error = drv_i915_object_insert(vk, I915_VK_OBJ_BUFFER, I915_VKE2_ID_SEEDED, &x->seeded);
	if (error == 0)
		error = drv_i915_object_insert(vk, I915_VK_OBJ_BUFFER, I915_VKE2_ID_WIDE, &x->wide);
	if (error == 0)
		error = drv_i915_object_insert(vk, I915_VK_OBJ_BUFFER, I915_VKE2_ID_INDICES, &x->indices);
	if (error == 0)
		error = drv_i915_object_insert(vk, I915_VK_OBJ_BUFFER, I915_VKE2_ID_MATRICES, &x->matrices);
	if (error == 0)
		error = drv_i915_object_insert(vk, I915_VK_OBJ_BUFFER, I915_VKE2_ID_PLACEMENT, &x->placement);
	if (error == 0)
		error = drv_i915_object_insert(vk, I915_VK_OBJ_RENDER_PASS, I915_VKE2_ID_PASS, &x->pass);
	if (error == 0)
		error = drv_i915_object_insert(vk, I915_VK_OBJ_FRAMEBUFFER, I915_VKE2_ID_FRAMEBUFFER, &x->framebuffer);
	if (error == 0)
		error = drv_i915_object_insert(vk, I915_VK_OBJ_DESCRIPTOR_SET, I915_VKE2_ID_MATRIX_SET, &x->matrix_set);

	/* Publishes the pipelines, one identity each. */
	for (index = 0U; error == 0 && index < I915_VKE2_PIPELINES; index++)
		error = drv_i915_object_insert(vk, I915_VK_OBJ_PIPELINE, I915_VKE2_ID_PIPELINE + index, &x->pipelines[index]);

	/* The teardown withdraws whatever was published, all of it or part of it. */
	x->published = 1;

	/* Reports why an object could not be published. */
	if (error != 0)
		return error;

	/* Succeeded: the wire can name every object. */
	return 0;
}

/* Withdraws every object the scenario published; an identity never published is ignored. */
static void
i915_vke2_objects_withdraw(
	struct i915_vke2 *x)
{
	struct i915_render_device *vk;
	uint32_t index;

	/* Removes each identity from the table. */
	vk = x->device->vk;
	drv_i915_object_remove(vk, I915_VK_OBJ_BUFFER, I915_VKE2_ID_QUADS);
	drv_i915_object_remove(vk, I915_VK_OBJ_BUFFER, I915_VKE2_ID_SEEDED);
	drv_i915_object_remove(vk, I915_VK_OBJ_BUFFER, I915_VKE2_ID_WIDE);
	drv_i915_object_remove(vk, I915_VK_OBJ_BUFFER, I915_VKE2_ID_INDICES);
	drv_i915_object_remove(vk, I915_VK_OBJ_BUFFER, I915_VKE2_ID_MATRICES);
	drv_i915_object_remove(vk, I915_VK_OBJ_BUFFER, I915_VKE2_ID_PLACEMENT);
	drv_i915_object_remove(vk, I915_VK_OBJ_RENDER_PASS, I915_VKE2_ID_PASS);
	drv_i915_object_remove(vk, I915_VK_OBJ_FRAMEBUFFER, I915_VKE2_ID_FRAMEBUFFER);
	drv_i915_object_remove(vk, I915_VK_OBJ_DESCRIPTOR_SET, I915_VKE2_ID_MATRIX_SET);
	for (index = 0U; index < I915_VKE2_PIPELINES; index++)
		drv_i915_object_remove(vk, I915_VK_OBJ_PIPELINE, I915_VKE2_ID_PIPELINE + index);
	x->published = 0;
}

/* Compiles every pipeline's kernels with the executor's compiler. */
static int
i915_vke2_pipelines_prepare(
	struct i915_vke2 *x)
{
	struct i915_gfx_pipeline *pipeline;
	uint32_t index;
	int error;

	/* Each pipeline in turn. */
	for (index = 0U; index < I915_VKE2_PIPELINES; index++) {
		error = drv_i915_gfx_pipeline_prepare(x->render, &x->pipelines[index]);
		if (error != 0) {
			kern_logf("i915: vke2: pipeline %u was not compiled: %d\n", index, error);
			return error;
		}

		/* Says how big each kernel came out, how many registers it uses and how much it spills a thread. */
		pipeline = &x->pipelines[index];
		kern_logf("i915: vke2: pipeline %u: vs %u bytes (r%u, %u varyings, scratch %u), fs %u bytes (r%u, %u inputs, scratch %u)\n",
			  index,
			  pipeline->vs_binary->code_bytes,
			  pipeline->vs_binary->grf_used,
			  pipeline->vs_binary->varying_count,
			  pipeline->vs_binary->scratch_bytes,
			  pipeline->fs_binary->code_bytes,
			  pipeline->fs_binary->grf_used,
			  pipeline->fs_binary->input_count,
			  pipeline->fs_binary->scratch_bytes);
	}

	/* Succeeded: every drawing pipeline can draw. */
	return 0;
}

/* Releases the kernels of every pipeline; a pipeline never prepared has none. */
static void
i915_vke2_pipelines_release(
	struct i915_vke2 *x)
{
	uint32_t index;

	/* Releases each pipeline's kernels. */
	for (index = 0U; index < I915_VKE2_PIPELINES; index++)
		drv_i915_gfx_pipeline_release(&x->pipelines[index]);
}

/*
 * Writes what the steps read and flushes it to memory: the quads (the
 * matrix step's, placed before its chain, and the plain full-target one),
 * the seeded quad of the varying steps, the wide quad of the attribute
 * step, the six indices of a quad and the two uniform blocks.
 */
static void
i915_vke2_data_write(
	struct i915_vke2 *x)
{
	static const uint32_t quad_indices[6] = { 0U, 1U, 2U, 0U, 2U, 3U };
	static const uint32_t corner_x[4] = { I915_VKE2_F_MINUS_1, I915_VKE2_F_1, I915_VKE2_F_1, I915_VKE2_F_MINUS_1 };
	static const uint32_t corner_y[4] = { I915_VKE2_F_MINUS_1, I915_VKE2_F_MINUS_1, I915_VKE2_F_1, I915_VKE2_F_1 };
	uint32_t *words;
	unsigned corner;
	unsigned k;

	/* The quads: the matrix step's corners before its chain, then the plain corners. */
	words = (uint32_t *)(void *)(x->cpu + I915_VKE2_QUADS_OFFSET);
	kern_memset(words, 0, I915_VKE2_VERTEX_BYTES);
	for (corner = 0U; corner < 4U; corner++) {
		i915_vke2_corner_write(words + (I915_VKE2_MATRIX_VERTEX + corner) * 8U,
				       corner,
				       i915_vke2_matrix_corners[2U * corner],
				       i915_vke2_matrix_corners[2U * corner + 1U]);
		i915_vke2_corner_write(words + (I915_VKE2_PLAIN_VERTEX + corner) * 8U, corner, corner_x[corner], corner_y[corner]);
	}
	drv_i915_gt_clflush(words, I915_VKE2_VERTEX_BYTES);

	/* The seeded quad: the plain corners, then the seed. */
	words = (uint32_t *)(void *)(x->cpu + I915_VKE2_SEEDED_OFFSET);
	kern_memset(words, 0, I915_VKE2_VERTEX_BYTES);
	for (corner = 0U; corner < 4U; corner++) {
		i915_vke2_corner_write(words + corner * 12U, corner, corner_x[corner], corner_y[corner]);
		kern_memcpy(&words[corner * 12U + 8U], i915_vke2_seed, sizeof(i915_vke2_seed));
	}
	drv_i915_gt_clflush(words, I915_VKE2_VERTEX_BYTES);

	/* The wide quad: the plain corners, then the fourteen data attributes. */
	words = (uint32_t *)(void *)(x->cpu + I915_VKE2_WIDE_OFFSET);
	kern_memset(words, 0, I915_VKE2_VERTEX_BYTES);
	for (corner = 0U; corner < 4U; corner++) {
		i915_vke2_corner_write(words + corner * 64U, corner, corner_x[corner], corner_y[corner]);
		for (k = 0U; k < 14U * 4U; k++)
			words[corner * 64U + 8U + k] = i915_vke2_vin_data[k];
	}
	drv_i915_gt_clflush(words, I915_VKE2_VERTEX_BYTES);

	/* The indices of one quad. */
	kern_memcpy(x->cpu + I915_VKE2_INDEX_OFFSET, quad_indices, sizeof(quad_indices));
	drv_i915_gt_clflush(x->cpu + I915_VKE2_INDEX_OFFSET, I915_VKE2_INDEX_BYTES);

	/* The matrix step's two uniform blocks. */
	kern_memset(x->cpu + I915_VKE2_MATRICES_OFFSET, 0, I915_VKE2_UNIFORM_BYTES);
	kern_memset(x->cpu + I915_VKE2_PLACEMENT_OFFSET, 0, I915_VKE2_UNIFORM_BYTES);
	kern_memcpy(x->cpu + I915_VKE2_MATRICES_OFFSET, i915_vke2_matrices, sizeof(i915_vke2_matrices));
	kern_memcpy(x->cpu + I915_VKE2_PLACEMENT_OFFSET, i915_vke2_placement, sizeof(i915_vke2_placement));
	drv_i915_gt_clflush(x->cpu + I915_VKE2_MATRICES_OFFSET, I915_VKE2_UNIFORM_BYTES);
	drv_i915_gt_clflush(x->cpu + I915_VKE2_PLACEMENT_OFFSET, I915_VKE2_UNIFORM_BYTES);
}

/*
 * Writes the position (x, y, 0, 1) and the pixel coordinate of one corner
 * of the target: top left (0, 0), top right (64, 0), bottom right (64, 64),
 * bottom left (0, 64).
 */
static void
i915_vke2_corner_write(
	uint32_t *words,
	unsigned corner,
	uint32_t x,
	uint32_t y)
{
	/* The position. */
	words[0] = x;
	words[1] = y;
	words[2] = I915_VKE2_F_0;
	words[3] = I915_VKE2_F_1;

	/* The right corners are the second and third, the bottom ones the third and fourth. */
	words[4] = I915_VKE2_F_0;
	if (corner == 1U || corner == 2U)
		words[4] = I915_VKE2_F_64;
	words[5] = I915_VKE2_F_0;
	if (corner >= 2U)
		words[5] = I915_VKE2_F_64;
	words[6] = I915_VKE2_F_0;
	words[7] = I915_VKE2_F_0;
}

/* Appends one little-endian word to the stream; a stream that would overflow is marked. */
static void
i915_vke2_put32(
	struct i915_vke2 *x,
	uint32_t value)
{
	/* Refuses a word past the end of the stream. */
	if (x->used + 4U > sizeof(x->wire)) {
		x->overflow = 1;
		return;
	}

	kern_memcpy(x->wire + x->used, &value, 4U);
	x->used += 4U;
}

/* Appends one little-endian double word to the stream. */
static void
i915_vke2_put64(
	struct i915_vke2 *x,
	uint64_t value)
{
	/* The low word first. */
	i915_vke2_put32(x, (uint32_t)value);
	i915_vke2_put32(x, (uint32_t)(value >> 32));
}

/* Appends the head of a recording: [opcode][no reply][command buffer]. */
static void
i915_vke2_record(
	struct i915_vke2 *x,
	uint32_t opcode)
{
	/* A recording asks for no reply. */
	i915_vke2_put32(x, opcode);
	i915_vke2_put32(x, 0U);
	i915_vke2_put64(x, I915_VKE2_ID_COMMAND_BUFFER);
}

/* Executes the stream built so far and empties it; the replies stay in the reply area. */
static int
i915_vke2_execute(
	struct i915_vke2 *x,
	const char *what)
{
	int error;

	/* Refuses a stream that did not fit. */
	if (x->overflow != 0) {
		kern_logf("i915: vke2: %s: the stream does not fit %u bytes\n", what, I915_VKE2_WIRE_BYTES);
		return ENOSPC;
	}

	/* Executes it into the reply area. */
	x->reply_bytes = sizeof(x->reply);
	error = drv_i915_render_execute(x->render, x->wire, x->used, x->reply, &x->reply_bytes);
	x->used = 0U;
	if (error != 0) {
		kern_logf("i915: vke2: %s: the executor refused the stream: %d\n", what, error);
		return error;
	}

	/* Succeeded: the replies are in the reply area. */
	return 0;
}

/* Reads one reply word; zero past the replies. */
static uint32_t
i915_vke2_reply32(
	const struct i915_vke2 *x,
	size_t offset)
{
	uint32_t value;

	/* A word past the replies reads as zero. */
	if (offset + 4U > x->reply_bytes)
		return 0U;

	kern_memcpy(&value, x->reply + offset, 4U);

	/* Succeeded: the word at the offset. */
	return value;
}

/* Creates the command pool and allocates its one primary command buffer. */
static int
i915_vke2_pool_create(
	struct i915_vke2 *x)
{
	uint32_t result;
	int error;

	/* vkCreateCommandPool: [85][reply][device][present][sType 39][no chain][flags][family][no allocator][present][identity]. */
	x->used = 0U;
	i915_vke2_put32(x, I915_VKE2_OP_CREATE_COMMAND_POOL);
	i915_vke2_put32(x, 1U);
	i915_vke2_put64(x, 0U);
	i915_vke2_put64(x, 1U);
	i915_vke2_put32(x, 39U);
	i915_vke2_put64(x, 0U);
	i915_vke2_put32(x, 0U);
	i915_vke2_put32(x, 0U);
	i915_vke2_put64(x, 0U);
	i915_vke2_put64(x, 1U);
	i915_vke2_put64(x, I915_VKE2_ID_POOL);

	/* vkAllocateCommandBuffers: [88][reply][device][present][sType 40][no chain][pool][primary][1][1][identity]. */
	i915_vke2_put32(x, I915_VKE2_OP_ALLOCATE_COMMAND_BUFFERS);
	i915_vke2_put32(x, 1U);
	i915_vke2_put64(x, 0U);
	i915_vke2_put64(x, 1U);
	i915_vke2_put32(x, 40U);
	i915_vke2_put64(x, 0U);
	i915_vke2_put64(x, I915_VKE2_ID_POOL);
	i915_vke2_put32(x, VK_COMMAND_BUFFER_LEVEL_PRIMARY);
	i915_vke2_put32(x, 1U);
	i915_vke2_put64(x, 1U);
	i915_vke2_put64(x, I915_VKE2_ID_COMMAND_BUFFER);
	error = i915_vke2_execute(x, "create the command pool");
	if (error != 0)
		return error;

	/* The pool exists from here on, whatever the allocation answered. */
	x->pooled = 1;

	/* Refuses a pool the executor did not make: [85][result][present][identity]. */
	result = i915_vke2_reply32(x, 4U);
	if (result != VK_SUCCESS)
		return EIO;

	/* Refuses a buffer it did not allocate: [88][result][count][identity] behind the pool's reply. */
	result = i915_vke2_reply32(x, 24U + 4U);
	if (result != VK_SUCCESS)
		return EIO;

	/* Succeeded: the command buffer can record. */
	return 0;
}

/* Starts a stream with vkBeginCommandBuffer, which empties the recording: [90][reply][buffer][present][sType 42][no chain][flags][no inheritance]. */
static void
i915_vke2_begin(
	struct i915_vke2 *x)
{
	/* The begin asks for its reply. */
	x->used = 0U;
	x->overflow = 0;
	i915_vke2_put32(x, I915_VKE2_OP_BEGIN_COMMAND_BUFFER);
	i915_vke2_put32(x, 1U);
	i915_vke2_put64(x, I915_VKE2_ID_COMMAND_BUFFER);
	i915_vke2_put64(x, 1U);
	i915_vke2_put32(x, 42U);
	i915_vke2_put64(x, 0U);
	i915_vke2_put32(x, 0U);
	i915_vke2_put64(x, 0U);
}

/*
 * Appends vkCmdBeginRenderPass of the whole target, cleared to black:
 * [present][sType 43][no chain][pass][framebuffer][area][present][1]
 * {[colour][tag][4][r g b a]}[inline].
 */
static void
i915_vke2_begin_pass(
	struct i915_vke2 *x)
{
	uint32_t index;

	/* The begin info. */
	i915_vke2_record(x, I915_VKE2_OP_BEGIN_RENDER_PASS);
	i915_vke2_put64(x, 1U);
	i915_vke2_put32(x, 43U);
	i915_vke2_put64(x, 0U);
	i915_vke2_put64(x, I915_VKE2_ID_PASS);
	i915_vke2_put64(x, I915_VKE2_ID_FRAMEBUFFER);

	/* The render area: the whole target. */
	i915_vke2_put32(x, 0U);
	i915_vke2_put32(x, 0U);
	i915_vke2_put32(x, I915_VKE2_SIZE);
	i915_vke2_put32(x, I915_VKE2_SIZE);

	/* One colour clear value: zero in every channel, which no step's result is everywhere. */
	i915_vke2_put32(x, 1U);
	i915_vke2_put64(x, 1U);
	i915_vke2_put32(x, 0U);
	i915_vke2_put32(x, 0U);
	i915_vke2_put64(x, 4U);
	for (index = 0U; index < 4U; index++)
		i915_vke2_put32(x, I915_VKE2_F_0);

	/* The subpass contents are inline. */
	i915_vke2_put32(x, VK_SUBPASS_CONTENTS_INLINE);
}

/* Appends vkCmdBindPipeline of one of the scenario's graphics pipelines. */
static void
i915_vke2_bind_pipeline(
	struct i915_vke2 *x,
	uint32_t pipeline)
{
	/* [bind point][pipeline]. */
	i915_vke2_record(x, I915_VKE2_OP_BIND_PIPELINE);
	i915_vke2_put32(x, VK_PIPELINE_BIND_POINT_GRAPHICS);
	i915_vke2_put64(x, I915_VKE2_ID_PIPELINE + pipeline);
}

/* Appends the binds of a vertex buffer at binding 0 and of the 32-bit quad indices. */
static void
i915_vke2_bind_buffers(
	struct i915_vke2 *x,
	uint64_t vertices)
{
	/* vkCmdBindVertexBuffers: [first][present][1]{buffer}[1]{offset}. */
	i915_vke2_record(x, I915_VKE2_OP_BIND_VERTEX_BUFFERS);
	i915_vke2_put32(x, 0U);
	i915_vke2_put32(x, 1U);
	i915_vke2_put64(x, 1U);
	i915_vke2_put64(x, vertices);
	i915_vke2_put64(x, 1U);
	i915_vke2_put64(x, 0U);

	/* vkCmdBindIndexBuffer: [buffer][offset][index type]. */
	i915_vke2_record(x, I915_VKE2_OP_BIND_INDEX_BUFFER);
	i915_vke2_put64(x, I915_VKE2_ID_INDICES);
	i915_vke2_put64(x, 0U);
	i915_vke2_put32(x, VK_INDEX_TYPE_UINT32);
}

/* Appends vkCmdDrawIndexed of the quad whose first vertex is given: [6][1][0][first vertex][0]. */
static void
i915_vke2_draw_quad(
	struct i915_vke2 *x,
	uint32_t first_vertex)
{
	/* The six indices of one instance, moved to the quad by the vertex offset. */
	i915_vke2_record(x, I915_VKE2_OP_DRAW_INDEXED);
	i915_vke2_put32(x, 6U);
	i915_vke2_put32(x, 1U);
	i915_vke2_put32(x, 0U);
	i915_vke2_put32(x, first_vertex);
	i915_vke2_put32(x, 0U);
}

/* Appends vkCmdBindDescriptorSets of one set as set 0: [bind point][layout][0][1][1]{set}[0][0]. */
static void
i915_vke2_bind_set(
	struct i915_vke2 *x,
	uint64_t set)
{
	/* The graphics bind point, no layout, set 0, no dynamic offsets. */
	i915_vke2_record(x, I915_VKE2_OP_BIND_DESCRIPTOR_SETS);
	i915_vke2_put32(x, VK_PIPELINE_BIND_POINT_GRAPHICS);
	i915_vke2_put64(x, 0U);
	i915_vke2_put32(x, 0U);
	i915_vke2_put32(x, 1U);
	i915_vke2_put64(x, 1U);
	i915_vke2_put64(x, set);
	i915_vke2_put32(x, 0U);
	i915_vke2_put64(x, 0U);
}

/* Appends vkCmdPushConstants of the whole block to both stages: [layout][stages][offset][size][size]{bytes}. */
static void
i915_vke2_push_constants(
	struct i915_vke2 *x,
	const uint32_t *block)
{
	unsigned index;

	/* No layout, both stages, from byte 0. */
	i915_vke2_record(x, I915_VKE2_OP_PUSH_CONSTANTS);
	i915_vke2_put64(x, 0U);
	i915_vke2_put32(x, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT);
	i915_vke2_put32(x, 0U);
	i915_vke2_put32(x, I915_VKE2_PUSH_BYTES);
	i915_vke2_put64(x, I915_VKE2_PUSH_BYTES);
	for (index = 0U; index < I915_VKE2_PUSH_BYTES / 4U; index++)
		i915_vke2_put32(x, block[index]);
}

/*
 * Appends vkUpdateDescriptorSets of one uniform buffer descriptor from the
 * buffer's start: [79][reply][device][1][1]{[sType 35][no chain][set]
 * [binding][0][1][type][0][1]{buffer offset range}[0]}[0][0].
 */
static void
i915_vke2_buffer_write(
	struct i915_vke2 *x,
	uint64_t set,
	uint32_t binding,
	uint64_t buffer,
	uint64_t range)
{
	/* The command and its one write. */
	i915_vke2_put32(x, I915_VKE2_OP_UPDATE_DESCRIPTOR_SETS);
	i915_vke2_put32(x, 1U);
	i915_vke2_put64(x, 0U);
	i915_vke2_put32(x, 1U);
	i915_vke2_put64(x, 1U);

	/* The write's head: the set, the binding, element 0, one uniform buffer. */
	i915_vke2_put32(x, 35U);
	i915_vke2_put64(x, 0U);
	i915_vke2_put64(x, set);
	i915_vke2_put32(x, binding);
	i915_vke2_put32(x, 0U);
	i915_vke2_put32(x, 1U);
	i915_vke2_put32(x, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);

	/* No images, the one buffer, no texel views. */
	i915_vke2_put64(x, 0U);
	i915_vke2_put64(x, 1U);
	i915_vke2_put64(x, buffer);
	i915_vke2_put64(x, 0U);
	i915_vke2_put64(x, range);
	i915_vke2_put64(x, 0U);

	/* No copies. */
	i915_vke2_put32(x, 0U);
	i915_vke2_put64(x, 0U);
}

/*
 * Appends the end and a submission of the command buffer, executes the
 * stream and checks the three replies: [90][result][91][result][18][result].
 */
static int
i915_vke2_finish(
	struct i915_vke2 *x,
	const char *what)
{
	uint32_t begun;
	uint32_t ended;
	uint32_t submitted;
	int error;

	/* vkEndCommandBuffer: [91][reply][buffer]. */
	i915_vke2_put32(x, I915_VKE2_OP_END_COMMAND_BUFFER);
	i915_vke2_put32(x, 1U);
	i915_vke2_put64(x, I915_VKE2_ID_COMMAND_BUFFER);

	/* vkQueueSubmit of the one buffer, with no semaphores and no fence. */
	i915_vke2_put32(x, I915_VKE2_OP_QUEUE_SUBMIT);
	i915_vke2_put32(x, 1U);
	i915_vke2_put64(x, 0U);
	i915_vke2_put32(x, 1U);
	i915_vke2_put64(x, 1U);
	i915_vke2_put32(x, 4U);
	i915_vke2_put64(x, 0U);
	i915_vke2_put32(x, 0U);
	i915_vke2_put64(x, 0U);
	i915_vke2_put64(x, 0U);
	i915_vke2_put32(x, 1U);
	i915_vke2_put64(x, 1U);
	i915_vke2_put64(x, I915_VKE2_ID_COMMAND_BUFFER);
	i915_vke2_put32(x, 0U);
	i915_vke2_put64(x, 0U);
	i915_vke2_put64(x, 0U);

	/* Runs the stream: the recording, then the submission to its end. */
	error = i915_vke2_execute(x, what);
	if (error != 0)
		return error;

	/* Takes the results of the begin, the end and the submission. */
	begun = i915_vke2_reply32(x, 4U);
	ended = i915_vke2_reply32(x, 12U);
	submitted = i915_vke2_reply32(x, 20U);

	/* Refuses a begin, an end or a submission that did not succeed. */
	if (begun != VK_SUCCESS ||
	    ended != VK_SUCCESS ||
	    submitted != VK_SUCCESS) {
		kern_logf("i915: vke2: %s: begin %d, end %d, submit %d\n",
			  what,
			  (int)begun,
			  (int)ended,
			  (int)submitted);
		return EIO;
	}

	/* Succeeded: the command buffer ran to its end. */
	return 0;
}

/*
 * Returns the bits of the float twice / 2 for a whole number twice below
 * 2^24, without the FPU: the float of twice (exact), its exponent one less.
 */
static uint32_t
i915_vke2_half_bits(
	uint32_t twice)
{
	uint32_t top;

	/* Zero is zero. */
	if (twice == 0U)
		return 0U;

	/* Finds the highest set bit, which the float keeps implicitly. */
	top = 23U;
	while ((twice >> top) == 0U)
		top--;

	/* Succeeded: exponent 127 + top - 1, the bits below the top as the mantissa. */
	return ((126U + top) << 23) | ((twice << (23U - top)) & 0x007fffffU);
}

/*
 * Expects the varying and attribute steps' words: the bits of
 * floor(x) * 1000 + floor(y) * 100000 plus the step's constant, given as
 * twice its value.
 */
static void
i915_vke2_expect_sums(
	struct i915_vke2 *x,
	uint32_t twice)
{
	uint32_t column;
	uint32_t row;

	/* Every pixel has its own sum. */
	for (row = 0U; row < I915_VKE2_SIZE; row++) {
		for (column = 0U; column < I915_VKE2_SIZE; column++)
			x->expected[row * I915_VKE2_SIZE + column] = i915_vke2_half_bits(2U * (column * 1000U + row * 100000U) + twice);
	}
}

/*
 * Decides whether a word matches the expected one: bit for bit, or, as
 * floats, of one sign (a zero of either) and within I915_VKE2_FLOAT_ULPS
 * units in the last place.
 */
static int
i915_vke2_near(
	uint32_t word,
	uint32_t expected,
	uint32_t mode)
{
	uint32_t magnitude;
	uint32_t wanted;

	/* An exact step, or an equal word. */
	if (word == expected)
		return 1;
	if (mode == I915_VKE2_COMPARE_EXACT)
		return 0;

	/* Two zeros of different signs are the same float. */
	magnitude = word & 0x7fffffffU;
	wanted = expected & 0x7fffffffU;
	if (magnitude == 0U && wanted == 0U)
		return 1;

	/* Floats of different signs are not near. */
	if ((word ^ expected) >> 31 != 0U)
		return 0;

	/* Neighbouring floats of one sign are neighbouring words. */
	if (magnitude > wanted + I915_VKE2_FLOAT_ULPS)
		return 0;
	if (wanted > magnitude + I915_VKE2_FLOAT_ULPS)
		return 0;

	/* Succeeded: within the units allowed. */
	return 1;
}

/* Compares the target with the expected words; EIO with the first differences logged. */
static int
i915_vke2_compare(
	struct i915_vke2 *x,
	const char *what,
	uint32_t mode)
{
	const uint32_t *pixels;
	unsigned differ;
	unsigned index;
	int near;

	/* Reads the target through the CPU view, past any line the CPU still caches. */
	pixels = (const uint32_t *)(const void *)(x->cpu + I915_VKE2_TARGET_OFFSET);
	drv_i915_gt_clflush(pixels, I915_VKE2_SIZE * I915_VKE2_SIZE * 4U);

	/* Counts the words that are not near and names the first few. */
	differ = 0U;
	for (index = 0U; index < I915_VKE2_SIZE * I915_VKE2_SIZE; index++) {
		near = i915_vke2_near(pixels[index], x->expected[index], mode);
		if (near != 0)
			continue;

		/* Names the first eight, with their row and column (the row picks the operation). */
		if (differ < 8U) {
			kern_logf("i915: vke2: %s: (%u,%u): 0x%08x, expected 0x%08x\n",
				  what,
				  index % I915_VKE2_SIZE,
				  index / I915_VKE2_SIZE,
				  pixels[index],
				  x->expected[index]);
		}
		differ++;
	}

	/* Says how much of the image differs. */
	if (differ != 0U) {
		kern_logf("i915: vke2: %s: %u of %u words differ\n", what, differ, I915_VKE2_SIZE * I915_VKE2_SIZE);
		return EIO;
	}

	/* Succeeded: every word is near its expected value. */
	return 0;
}

/* Logs a step's verdict and counts it. */
static void
i915_vke2_verdict(
	struct i915_vke2 *x,
	const char *what,
	int error)
{
	/* A step fails with the error that stopped it. */
	if (error != 0) {
		x->failed++;
		kern_logf("i915: vke2: VKE2-%s FAIL (%d)\n", what, error);
		return;
	}

	x->passed++;
	kern_logf("i915: vke2: VKE2-%s PASS\n", what);
}

/*
 * Runs one drawing step without descriptors: the pass on black, the
 * pipeline, the vertex buffer and one quad; then compares the target with
 * the step's words (generated, or the sums of the varying steps).
 */
static void
i915_vke2_step_draw(
	struct i915_vke2 *x,
	const char *what,
	uint32_t pipeline,
	uint64_t vertices,
	uint32_t first_vertex,
	uint32_t mode)
{
	const uint32_t *generated;
	uint32_t twice;
	int error;

	/* Records the pass, the pipeline, the buffers and the quad. */
	i915_vke2_begin(x);
	i915_vke2_begin_pass(x);
	i915_vke2_bind_pipeline(x, pipeline);
	i915_vke2_bind_buffers(x, vertices);
	i915_vke2_draw_quad(x, first_vertex);
	i915_vke2_record(x, I915_VKE2_OP_END_RENDER_PASS);

	/* Runs it. */
	error = i915_vke2_finish(x, what);
	if (error != 0) {
		i915_vke2_verdict(x, what, error);
		return;
	}

	/* Picks the step's words: generated for the arithmetic steps, sums for the interface steps. */
	generated = NULL;
	twice = 0U;
	switch (pipeline) {
	case I915_VKE2_PIPE_INT:
		generated = i915_vke2_int_expected;
		break;
	case I915_VKE2_PIPE_FLOAT:
		generated = i915_vke2_float_expected;
		break;
	case I915_VKE2_PIPE_LOOP:
		generated = i915_vke2_loop_expected;
		break;
	case I915_VKE2_PIPE_SPILL:
		generated = i915_vke2_spill_expected;
		break;
	case I915_VKE2_PIPE_VIO16:
		twice = I915_VKE2_VIO16_TWICE;
		break;
	case I915_VKE2_PIPE_VARY16:
		twice = I915_VKE2_VARY16_TWICE;
		break;
	case I915_VKE2_PIPE_SUBSET:
		twice = I915_VKE2_SUBSET_TWICE;
		break;
	default:
		twice = I915_VKE2_VIN16_TWICE;
		break;
	}

	/* Fills the expected words and compares. */
	if (generated != NULL) {
		kern_memcpy(x->expected, generated, sizeof(x->expected));
	} else {
		i915_vke2_expect_sums(x, twice);
	}

	error = i915_vke2_compare(x, what, mode);
	i915_vke2_verdict(x, what, error);
}

/*
 * MATRIX: vkUpdateDescriptorSets gives the set the fragment stage's block
 * (binding 0) and the vertex stage's placement (binding 1); the push
 * constants carry a row-major and a column-major mat4.  The quad is placed
 * by the vertex shader's chain, so a wrong matrix layout moves the pixel
 * coordinates; every pixel holds the bits of one float result, a zero of
 * either sign equal.
 */
static void
i915_vke2_step_matrix(
	struct i915_vke2 *x)
{
	int error;

	/* Writes the two descriptors through the wire. */
	x->used = 0U;
	x->overflow = 0;
	i915_vke2_buffer_write(x, I915_VKE2_ID_MATRIX_SET, 0U, I915_VKE2_ID_MATRICES, I915_VKE2_UNIFORM_BYTES);
	i915_vke2_buffer_write(x, I915_VKE2_ID_MATRIX_SET, 1U, I915_VKE2_ID_PLACEMENT, I915_VKE2_UNIFORM_BYTES);
	error = i915_vke2_execute(x, "MATRIX descriptors");
	if (error != 0) {
		i915_vke2_verdict(x, "MATRIX", error);
		return;
	}

	/* Records the pass, the pipeline, the set, the push constants and the placed quad. */
	i915_vke2_begin(x);
	i915_vke2_begin_pass(x);
	i915_vke2_bind_pipeline(x, I915_VKE2_PIPE_MATRIX);
	i915_vke2_bind_buffers(x, I915_VKE2_ID_QUADS);
	i915_vke2_bind_set(x, I915_VKE2_ID_MATRIX_SET);
	i915_vke2_push_constants(x, i915_vke2_push);
	i915_vke2_draw_quad(x, I915_VKE2_MATRIX_VERTEX);
	i915_vke2_record(x, I915_VKE2_OP_END_RENDER_PASS);

	/* Runs it and compares every word with the generated ones. */
	error = i915_vke2_finish(x, "MATRIX");
	if (error == 0) {
		kern_memcpy(x->expected, i915_vke2_matrix_expected, sizeof(x->expected));
		error = i915_vke2_compare(x, "MATRIX", I915_VKE2_COMPARE_FLOAT);
	}

	i915_vke2_verdict(x, "MATRIX", error);
}

