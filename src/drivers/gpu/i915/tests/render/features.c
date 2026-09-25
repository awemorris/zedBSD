/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The feature scenario "vke1": colour blending, uniform buffers and
 * several sampled images, recorded through the wire, run on the GPU and
 * checked against the pixels feature-shaders/regenerate.py computed.
 *
 * Like the executor scenario, the thread waits for the node, opens a session
 * of its own and drives the executor with the commands libvulkan sends; the
 * objects the commands name are made directly and published in the
 * executor's object table.  The steps:
 *
 *  - BLEND: sixteen pipelines, each with its own blend of attachment 0
 *    (factors, operations, independent alpha, write masks, the pipeline's
 *    blend constants and ones set by vkCmdSetBlendConstants), each draw a
 *    16x16 cell over a target cleared to a known colour;
 *  - UBO: a vertex shader placing its quad by a mat4 and an offset of one
 *    uniform buffer, a fragment shader colouring it from another (a vec4,
 *    an array element, a mat4 column); the descriptors written by
 *    vkUpdateDescriptorSets, the second draw's colour reached through a
 *    dynamic uniform buffer and the dynamic offset of its bind;
 *  - TEX3: a fragment shader sampling three textures, bound at two
 *    bindings of set 0 and one of set 1 with a linear and two nearest
 *    samplers, the two sets bound by one vkCmdBindDescriptorSets.
 *
 * Each step logs "VKE1-<name> PASS" or "FAIL", then the thread logs the
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
#define I915_VKE1_SIZE			64U

/* The storage every buffer, texture and the target are bound in, and where each is. */
#define I915_VKE1_STORAGE_BYTES		(512U * 1024U)
#define I915_VKE1_TARGET_OFFSET		0x00000U
#define I915_VKE1_VERTEX_OFFSET		0x10000U
#define I915_VKE1_VERTEX_BYTES		0x01000U
#define I915_VKE1_INDEX_OFFSET		0x11000U
#define I915_VKE1_INDEX_BYTES		0x00100U
#define I915_VKE1_TRANSFORM_OFFSET	0x20000U
#define I915_VKE1_MATERIAL_OFFSET	0x21000U
#define I915_VKE1_UNIFORM_BYTES		0x00200U
#define I915_VKE1_TEXTURE_OFFSET	0x30000U
#define I915_VKE1_TEXTURE_STRIDE	0x01000U

/* The bytes of one vertex: a vec4 position, then a vec4 colour. */
#define I915_VKE1_VERTEX_STRIDE		32U

/* The blend cases, and the first vertex of the uniform quad and of the textured quad. */
#define I915_VKE1_BLEND_CASES		16U
#define I915_VKE1_UBO_VERTEX		64U
#define I915_VKE1_TEX_VERTEX		68U

/* The floats of one transform block and of one material block (feature-shaders/ubo.vert, ubo.frag). */
#define I915_VKE1_TRANSFORM_FLOATS	20U
#define I915_VKE1_MATERIAL_FLOATS	28U

/* The textures of the texture step: 2x1 RGBA8 each. */
#define I915_VKE1_TEXTURES		3U
#define I915_VKE1_TEXTURE_WIDTH		2U

/* How far a blended or uniform-coloured byte, and a linearly filtered one, may be from its reference. */
#define I915_VKE1_TOLERANCE		1U
#define I915_VKE1_FILTER_TOLERANCE	2U

/* The wire identities the scenario publishes its objects under, apart from any client's and vkx's. */
#define I915_VKE1_IDENTITY		0x7e5e100000000000ULL
#define I915_VKE1_ID_VERTICES		(I915_VKE1_IDENTITY + 1U)
#define I915_VKE1_ID_INDICES		(I915_VKE1_IDENTITY + 2U)
#define I915_VKE1_ID_PASS		(I915_VKE1_IDENTITY + 3U)
#define I915_VKE1_ID_FRAMEBUFFER	(I915_VKE1_IDENTITY + 4U)
#define I915_VKE1_ID_POOL		(I915_VKE1_IDENTITY + 5U)
#define I915_VKE1_ID_COMMAND_BUFFER	(I915_VKE1_IDENTITY + 6U)
#define I915_VKE1_ID_TRANSFORMS		(I915_VKE1_IDENTITY + 7U)
#define I915_VKE1_ID_MATERIALS		(I915_VKE1_IDENTITY + 8U)
#define I915_VKE1_ID_UBO_PIPELINE	(I915_VKE1_IDENTITY + 9U)
#define I915_VKE1_ID_UBO_SET		(I915_VKE1_IDENTITY + 10U)
#define I915_VKE1_ID_DYNAMIC_SET	(I915_VKE1_IDENTITY + 11U)
#define I915_VKE1_ID_TEX_PIPELINE	(I915_VKE1_IDENTITY + 12U)
#define I915_VKE1_ID_TEX_SET0		(I915_VKE1_IDENTITY + 13U)
#define I915_VKE1_ID_TEX_SET1		(I915_VKE1_IDENTITY + 14U)
#define I915_VKE1_ID_BLEND_PIPELINE	(I915_VKE1_IDENTITY + 0x100U)

/* The wire opcodes the scenario sends, as libvulkan numbers them. */
#define I915_VKE1_OP_QUEUE_SUBMIT		18U
#define I915_VKE1_OP_UPDATE_DESCRIPTOR_SETS	79U
#define I915_VKE1_OP_CREATE_COMMAND_POOL	85U
#define I915_VKE1_OP_DESTROY_COMMAND_POOL	86U
#define I915_VKE1_OP_ALLOCATE_COMMAND_BUFFERS	88U
#define I915_VKE1_OP_BEGIN_COMMAND_BUFFER	90U
#define I915_VKE1_OP_END_COMMAND_BUFFER		91U
#define I915_VKE1_OP_BIND_PIPELINE		93U
#define I915_VKE1_OP_SET_BLEND_CONSTANTS	98U
#define I915_VKE1_OP_BIND_DESCRIPTOR_SETS	103U
#define I915_VKE1_OP_BIND_INDEX_BUFFER		104U
#define I915_VKE1_OP_BIND_VERTEX_BUFFERS	105U
#define I915_VKE1_OP_DRAW_INDEXED		107U
#define I915_VKE1_OP_BEGIN_RENDER_PASS		133U
#define I915_VKE1_OP_END_RENDER_PASS		135U

/* The largest stream the scenario builds, and the room for its replies. */
#define I915_VKE1_WIRE_BYTES		8192U
#define I915_VKE1_REPLY_BYTES		1024U

/* How long the thread waits for the node to be published, in seconds. */
#define I915_VKE1_WAIT_S		120U

/* IEEE-754 bits of the floats the scenario itself uses; the kernel computes no float. */
#define I915_VKE1_F_0			0x00000000U
#define I915_VKE1_F_1			0x3f800000U
#define I915_VKE1_F_64			0x42800000U

/* The pixels the scenario writes: RGBA8 in memory, red in the low byte. */
#define I915_VKE1_BLACK			0xff000000U

/*
 * One blend case of the blend step: the blend of attachment 0 as a
 * pipeline states it, the colour the draw writes and the pixel it must
 * leave over the destination.
 *
 * The generated table below names every case; it never changes.
 */
struct i915_vke1_blend_case {
	/* The name the case is logged by. */
	const char *name;

	/* Whether blending is on, and the VkBlendFactor and VkBlendOp of the colour and of the alpha. */
	uint32_t enable;
	uint32_t src_color;
	uint32_t dst_color;
	uint32_t color_op;
	uint32_t src_alpha;
	uint32_t dst_alpha;
	uint32_t alpha_op;

	/* The VkColorComponentFlags attachment 0 writes. */
	uint32_t write_mask;

	/* Nonzero when the blend constants are dynamic and set by vkCmdSetBlendConstants. */
	int dynamic_constants;

	/* The colour the draw writes, as float bits, and the pixel expected. */
	uint32_t color[4];
	uint32_t expected;
};

/*
 * One rectangle of a step's expected image and the pixel it holds.
 *
 * The generated table below names the uniform step's rectangles.
 */
struct i915_vke1_rect {
	/* Columns left .. right - 1 and rows top .. bottom - 1. */
	uint32_t left;
	uint32_t top;
	uint32_t right;
	uint32_t bottom;

	/* The pixel every one of them holds. */
	uint32_t pixel;
};

#include "../fixtures/feature-shaders-gen.inc"

/*
 * Everything the scenario owns while its thread runs.
 *
 * It is filled by the setup, used by the steps and emptied by the teardown,
 * all on the scenario's thread; the scenario runs once per boot.
 */
struct i915_vke1 {
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

	/* The vertex and index buffers, and the uniform buffers of the transforms and the materials. */
	struct i915_gfx_buffer vertices;
	struct i915_gfx_buffer indices;
	struct i915_gfx_buffer transforms;
	struct i915_gfx_buffer materials;

	/* The shader modules. */
	struct i915_gfx_shader pass_vert;
	struct i915_gfx_shader color_frag;
	struct i915_gfx_shader ubo_vert;
	struct i915_gfx_shader ubo_frag;
	struct i915_gfx_shader tex3_frag;

	/* The pipelines: one per blend case, the uniform one and the texturing one. */
	struct i915_gfx_pipeline blend_pipelines[I915_VKE1_BLEND_CASES];
	struct i915_gfx_pipeline ubo_pipeline;
	struct i915_gfx_pipeline tex_pipeline;

	/*
	 * The uniform step's layouts and sets: both have a uniform buffer at
	 * binding 0; binding 1 is a plain uniform buffer in the first and a
	 * dynamic one in the second.
	 */
	struct i915_gfx_dsl ubo_layout;
	struct i915_gfx_dsl dynamic_layout;
	struct i915_gfx_dset ubo_set;
	struct i915_gfx_dset dynamic_set;

	/* The texture step's textures, views, samplers and its two sets. */
	struct i915_gfx_image textures[I915_VKE1_TEXTURES];
	struct i915_gfx_view texture_views[I915_VKE1_TEXTURES];
	struct i915_gfx_sampler samplers[I915_VKE1_TEXTURES];
	struct i915_gfx_dset tex_set0;
	struct i915_gfx_dset tex_set1;

	/* Nonzero once the objects are published, and once the command pool exists. */
	int published;
	int pooled;

	/* The stream under construction, and nonzero once it overflowed. */
	uint8_t wire[I915_VKE1_WIRE_BYTES];
	size_t used;
	int overflow;

	/* The replies of the last stream, and how many bytes they took. */
	uint8_t reply[I915_VKE1_REPLY_BYTES];
	size_t reply_bytes;

	/* The image a step expects the target to hold, and how far each byte may be from it. */
	uint32_t expected[I915_VKE1_SIZE * I915_VKE1_SIZE];
	uint8_t tolerance[I915_VKE1_SIZE * I915_VKE1_SIZE];

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
static struct i915_vke1 i915_vke1_state;

static void i915_vke1_thread(void *argument);
static int i915_vke1_wait_node(struct i915_device *device);
static int i915_vke1_setup(struct i915_vke1 *x);
static void i915_vke1_teardown(struct i915_vke1 *x);
static int i915_vke1_storage_create(struct i915_vke1 *x);
static void i915_vke1_objects_init(struct i915_vke1 *x);
static void i915_vke1_buffer_init(struct i915_vke1 *x, struct i915_gfx_buffer *buffer, uint64_t size, uint32_t usage, uint64_t offset);
static void i915_vke1_shader_init(struct i915_gfx_shader *shader, const uint32_t *words, uint32_t bytes);
static void i915_vke1_pipeline_init(struct i915_gfx_pipeline *pipeline, struct i915_gfx_shader *vertex, struct i915_gfx_shader *fragment);
static void i915_vke1_blend_init(struct i915_gfx_pipeline *pipeline, const struct i915_vke1_blend_case *blend);
static void i915_vke1_texture_init(struct i915_vke1 *x, uint32_t index);
static int i915_vke1_objects_publish(struct i915_vke1 *x);
static void i915_vke1_objects_withdraw(struct i915_vke1 *x);
static int i915_vke1_pipelines_prepare(struct i915_vke1 *x);
static void i915_vke1_pipelines_release(struct i915_vke1 *x);
static void i915_vke1_data_write(struct i915_vke1 *x);
static void i915_vke1_quad_write(uint32_t *words, unsigned left, unsigned top, unsigned right, unsigned bottom, const uint32_t color[4]);
static void i915_vke1_texquad_write(uint32_t *words, unsigned left, unsigned top, unsigned right, unsigned bottom);
static void i915_vke1_put32(struct i915_vke1 *x, uint32_t value);
static void i915_vke1_put64(struct i915_vke1 *x, uint64_t value);
static void i915_vke1_record(struct i915_vke1 *x, uint32_t opcode);
static int i915_vke1_execute(struct i915_vke1 *x, const char *what);
static uint32_t i915_vke1_reply32(const struct i915_vke1 *x, size_t offset);
static int i915_vke1_pool_create(struct i915_vke1 *x);
static void i915_vke1_begin(struct i915_vke1 *x);
static void i915_vke1_begin_pass(struct i915_vke1 *x, const uint32_t clear[4]);
static void i915_vke1_bind_pipeline(struct i915_vke1 *x, uint64_t pipeline);
static void i915_vke1_bind_buffers(struct i915_vke1 *x);
static void i915_vke1_draw_quad(struct i915_vke1 *x, uint32_t first_vertex);
static void i915_vke1_bind_sets(struct i915_vke1 *x, uint32_t first, const uint64_t *sets, uint32_t set_count, const uint32_t *offsets, uint32_t offset_count);
static void i915_vke1_set_blend_constants(struct i915_vke1 *x, const uint32_t constants[4]);
static void i915_vke1_buffer_write(struct i915_vke1 *x, uint64_t set, uint32_t binding, uint32_t type, uint64_t buffer, uint64_t offset, uint64_t range);
static int i915_vke1_finish(struct i915_vke1 *x, const char *what);
static void i915_vke1_expect_fill(struct i915_vke1 *x, uint32_t pixel, uint8_t tolerance);
static void i915_vke1_expect_rect(struct i915_vke1 *x, uint32_t left, uint32_t top, uint32_t right, uint32_t bottom, uint32_t pixel, uint8_t tolerance);
static int i915_vke1_near(uint32_t pixel, uint32_t expected, uint32_t tolerance);
static int i915_vke1_compare(struct i915_vke1 *x, const char *what);
static void i915_vke1_verdict(struct i915_vke1 *x, const char *what, int error);
static void i915_vke1_step_blend(struct i915_vke1 *x);
static void i915_vke1_step_ubo(struct i915_vke1 *x);
static void i915_vke1_step_tex3(struct i915_vke1 *x);

/*
 * Starts the feature scenario.
 *
 * Returns at once: the scenario's thread runs the steps after the node is
 * published and logs their verdicts itself.
 */
void
drv_i915_test_render_features(
	struct i915_device *device)
{
	struct thread *thread;
	int error;

	/* Starts the thread that waits for the node and runs the steps. */
	error = kthread_create(i915_vke1_thread, device, SCHED_PRIORITY_DEFAULT, &thread);
	if (error != 0) {
		kern_logf("i915: vke1: verdict FAIL (the scenario thread cannot be created: %d)\n", error);
		return;
	}

	/* The thread reclaims itself when the steps are done. */
	thread->detached = 1U;
	thread_start(thread);
	kern_logf("i915: vke1: the steps run once the node is published\n");
}

/* Runs the scenario: waits for the node, sets up, runs every step, tears down and logs the verdict. */
static void
i915_vke1_thread(
	void *argument)
{
	struct i915_device *device;
	struct i915_vke1 *x;
	int error;

	/* Waits until the node is published and its worker serves. */
	device = argument;
	error = i915_vke1_wait_node(device);
	if (error != 0) {
		kern_logf("i915: vke1: verdict FAIL (the node was not published within %u s)\n", I915_VKE1_WAIT_S);
		return;
	}

	/* Opens the session and makes every object the steps use. */
	x = &i915_vke1_state;
	kern_memset(x, 0, sizeof(*x));
	x->device = device;
	error = i915_vke1_setup(x);
	if (error != 0) {
		kern_logf("i915: vke1: verdict FAIL (setup: %d)\n", error);
		i915_vke1_teardown(x);
		return;
	}

	/* Runs every step; each logs its own verdict. */
	i915_vke1_step_blend(x);
	i915_vke1_step_ubo(x);
	i915_vke1_step_tex3(x);

	/* Gives everything back and says how the steps went. */
	i915_vke1_teardown(x);
	if (x->failed != 0U) {
		kern_logf("i915: vke1: verdict FAIL (%u of %u steps passed)\n", x->passed, x->passed + x->failed);
		return;
	}

	kern_logf("i915: vke1: verdict PASS (%u of %u steps passed)\n", x->passed, x->passed + x->failed);
}

/* Waits until the node is published, sleeping a twentieth of a second at a time; ETIMEDOUT when it never is. */
static int
i915_vke1_wait_node(
	struct i915_device *device)
{
	struct i915_completion nap;
	unsigned waited;

	/* A completion nobody signals: each wait on it simply lasts until its deadline. */
	drv_i915_completion_init(&nap, "i915 vke1");

	/* Looks for the published node until the budget is spent. */
	for (waited = 0U; waited < I915_VKE1_WAIT_S * 20U; waited++) {
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
i915_vke1_setup(
	struct i915_vke1 *x)
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
	error = i915_vke1_storage_create(x);
	if (error != 0)
		return error;

	/* Describes the objects over the storage and publishes them. */
	i915_vke1_objects_init(x);
	error = i915_vke1_objects_publish(x);
	if (error != 0)
		return error;

	/* Compiles every pipeline. */
	error = i915_vke1_pipelines_prepare(x);
	if (error != 0)
		return error;

	/* Writes the vertices, the indices, the uniform blocks and the texels. */
	i915_vke1_data_write(x);

	/* Creates the command pool and its one command buffer through the wire. */
	error = i915_vke1_pool_create(x);
	if (error != 0)
		return error;

	/* Succeeded: every step can record and submit. */
	return 0;
}

/* Gives back everything the setup made, whatever it got to. */
static void
i915_vke1_teardown(
	struct i915_vke1 *x)
{
	struct i915_device *device;
	int error;

	/* Destroys the command pool and its buffer through the wire. */
	if (x->pooled != 0) {
		x->used = 0U;
		x->overflow = 0;
		i915_vke1_put32(x, I915_VKE1_OP_DESTROY_COMMAND_POOL);
		i915_vke1_put32(x, 1U);
		i915_vke1_put64(x, 0U);
		i915_vke1_put64(x, I915_VKE1_ID_POOL);
		i915_vke1_put64(x, 0U);
		error = i915_vke1_execute(x, "destroy the command pool");
		if (error != 0)
			kern_logf("i915: vke1: the command pool was not destroyed: %d\n", error);
		x->pooled = 0;
	}

	/* Withdraws the published objects and releases the kernels. */
	if (x->published != 0)
		i915_vke1_objects_withdraw(x);
	i915_vke1_pipelines_release(x);

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
i915_vke1_storage_create(
	struct i915_vke1 *x)
{
	struct i915_device *device;
	int error;

	/* Creates the object and binds it, destroying it again when the binding fails. */
	device = x->device;
	mutex_lock(&device->mutex);

	error = drv_i915_gem_create(&device->gem, I915_VKE1_STORAGE_BYTES, &x->storage);
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
i915_vke1_objects_init(
	struct i915_vke1 *x)
{
	uint32_t index;

	/* The memory is the whole storage. */
	x->memory.vk = x->device->vk;
	x->memory.identity = I915_VKE1_IDENTITY;
	x->memory.size = I915_VKE1_STORAGE_BYTES;
	x->memory.object = x->storage;

	/* The target: a linear 64x64 RGBA8 image. */
	x->target.format = VK_FORMAT_R8G8B8A8_UNORM;
	x->target.width = I915_VKE1_SIZE;
	x->target.height = I915_VKE1_SIZE;
	x->target.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
	x->target.pitch = I915_VKE1_SIZE * 4U;
	x->target.bytes = I915_VKE1_SIZE * I915_VKE1_SIZE * 4U;
	x->target.levels = 1U;
	x->target.memory = &x->memory;
	x->target.offset = I915_VKE1_TARGET_OFFSET;
	x->view.image = &x->target;
	x->view.format = VK_FORMAT_R8G8B8A8_UNORM;
	x->view.level_count = 1U;

	/* One subpass writing the one colour attachment, cleared at the begin. */
	x->pass.attachment_count = 1U;
	x->pass.attachments[0].format = VK_FORMAT_R8G8B8A8_UNORM;
	x->pass.attachments[0].load_op = VK_ATTACHMENT_LOAD_OP_CLEAR;
	x->pass.color_attachment = 0U;
	x->pass.depth_attachment = VK_ATTACHMENT_UNUSED;
	x->framebuffer.width = I915_VKE1_SIZE;
	x->framebuffer.height = I915_VKE1_SIZE;
	x->framebuffer.view_count = 1U;
	x->framebuffer.views[0] = &x->view;

	/* The vertex and index buffers, and the two uniform buffers. */
	i915_vke1_buffer_init(x, &x->vertices, I915_VKE1_VERTEX_BYTES, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, I915_VKE1_VERTEX_OFFSET);
	i915_vke1_buffer_init(x, &x->indices, I915_VKE1_INDEX_BYTES, VK_BUFFER_USAGE_INDEX_BUFFER_BIT, I915_VKE1_INDEX_OFFSET);
	i915_vke1_buffer_init(x, &x->transforms, I915_VKE1_UNIFORM_BYTES, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, I915_VKE1_TRANSFORM_OFFSET);
	i915_vke1_buffer_init(x, &x->materials, I915_VKE1_UNIFORM_BYTES, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, I915_VKE1_MATERIAL_OFFSET);

	/* The shader modules borrow the generated words, which the compiler only reads. */
	i915_vke1_shader_init(&x->pass_vert, i915_vke1_pass_vert, sizeof(i915_vke1_pass_vert));
	i915_vke1_shader_init(&x->color_frag, i915_vke1_color_frag, sizeof(i915_vke1_color_frag));
	i915_vke1_shader_init(&x->ubo_vert, i915_vke1_ubo_vert, sizeof(i915_vke1_ubo_vert));
	i915_vke1_shader_init(&x->ubo_frag, i915_vke1_ubo_frag, sizeof(i915_vke1_ubo_frag));
	i915_vke1_shader_init(&x->tex3_frag, i915_vke1_tex3_frag, sizeof(i915_vke1_tex3_frag));

	/* One pipeline per blend case, with the case's blend. */
	for (index = 0U; index < I915_VKE1_BLEND_CASES; index++) {
		i915_vke1_pipeline_init(&x->blend_pipelines[index], &x->pass_vert, &x->color_frag);
		i915_vke1_blend_init(&x->blend_pipelines[index], &i915_vke1_blend_cases[index]);
	}

	/* The uniform pipeline and the texturing pipeline write without blending. */
	i915_vke1_pipeline_init(&x->ubo_pipeline, &x->ubo_vert, &x->ubo_frag);
	i915_vke1_pipeline_init(&x->tex_pipeline, &x->pass_vert, &x->tex3_frag);

	/* The uniform layouts: binding 0 a uniform buffer; binding 1 a plain one, or a dynamic one. */
	x->ubo_layout.count = 2U;
	x->ubo_layout.bindings[0].binding = 0U;
	x->ubo_layout.bindings[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
	x->ubo_layout.bindings[0].stages = VK_SHADER_STAGE_VERTEX_BIT;
	x->ubo_layout.bindings[1].binding = 1U;
	x->ubo_layout.bindings[1].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
	x->ubo_layout.bindings[1].stages = VK_SHADER_STAGE_FRAGMENT_BIT;
	x->dynamic_layout = x->ubo_layout;
	x->dynamic_layout.bindings[1].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;

	/* The uniform sets start empty; vkUpdateDescriptorSets fills them. */
	x->ubo_set.layout = &x->ubo_layout;
	x->dynamic_set.layout = &x->dynamic_layout;

	/* The three textures, their views and samplers. */
	for (index = 0U; index < I915_VKE1_TEXTURES; index++)
		i915_vke1_texture_init(x, index);

	/* Set 0 holds textures 0 and 1 at bindings 0 and 2, set 1 texture 2 at binding 1. */
	x->tex_set0.slots[0].view = &x->texture_views[0];
	x->tex_set0.slots[0].sampler = &x->samplers[0];
	x->tex_set0.slots[2].view = &x->texture_views[1];
	x->tex_set0.slots[2].sampler = &x->samplers[1];
	x->tex_set1.slots[1].view = &x->texture_views[2];
	x->tex_set1.slots[1].sampler = &x->samplers[2];
}

/* Describes one buffer bound at an offset of the storage. */
static void
i915_vke1_buffer_init(
	struct i915_vke1 *x,
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
i915_vke1_shader_init(
	struct i915_gfx_shader *shader,
	const uint32_t *words,
	uint32_t bytes)
{
	/* The module borrows the words, which the compiler only reads. */
	shader->words = (uint32_t *)(uintptr_t)words;
	shader->word_count = bytes / 4U;
}

/*
 * Describes one pipeline: two shaders, two vec4 attributes of one binding,
 * a triangle list, no culling and no depth, the whole target as viewport
 * and scissor, every component written without blending.
 */
static void
i915_vke1_pipeline_init(
	struct i915_gfx_pipeline *pipeline,
	struct i915_gfx_shader *vertex,
	struct i915_gfx_shader *fragment)
{
	/* The two stages. */
	kern_memset(pipeline, 0, sizeof(*pipeline));
	pipeline->vertex = vertex;
	pipeline->fragment = fragment;

	/* Binding 0 of 32-byte vertices: the position at 0, the colour at 16. */
	pipeline->binding_count = 1U;
	pipeline->bindings[0].binding = 0U;
	pipeline->bindings[0].stride = I915_VKE1_VERTEX_STRIDE;
	pipeline->attribute_count = 2U;
	pipeline->attributes[0].location = 0U;
	pipeline->attributes[0].binding = 0U;
	pipeline->attributes[0].format = VK_FORMAT_R32G32B32A32_SFLOAT;
	pipeline->attributes[0].offset = 0U;
	pipeline->attributes[1].location = 1U;
	pipeline->attributes[1].binding = 0U;
	pipeline->attributes[1].format = VK_FORMAT_R32G32B32A32_SFLOAT;
	pipeline->attributes[1].offset = 16U;

	/* A triangle list, drawn from both sides with no depth test. */
	pipeline->topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
	pipeline->cull_mode = VK_CULL_MODE_NONE;

	/* The whole target, depth range [0, 1]. */
	pipeline->viewport[0] = I915_VKE1_F_0;
	pipeline->viewport[1] = I915_VKE1_F_0;
	pipeline->viewport[2] = I915_VKE1_F_64;
	pipeline->viewport[3] = I915_VKE1_F_64;
	pipeline->viewport[4] = I915_VKE1_F_0;
	pipeline->viewport[5] = I915_VKE1_F_1;
	pipeline->scissor.extent.width = I915_VKE1_SIZE;
	pipeline->scissor.extent.height = I915_VKE1_SIZE;
}

/*
 * Gives a pipeline the blend of one case, as pipeline.c keeps a decoded
 * VkPipelineColorBlendStateCreateInfo: the factors and operations, the
 * components not written, the pipeline's blend constants and whether they
 * are dynamic.
 */
static void
i915_vke1_blend_init(
	struct i915_gfx_pipeline *pipeline,
	const struct i915_vke1_blend_case *blend)
{
	/* The factors and operations of the colour and of the alpha. */
	pipeline->blend_enable = blend->enable;
	pipeline->blend_src_color = blend->src_color;
	pipeline->blend_dst_color = blend->dst_color;
	pipeline->blend_color_op = blend->color_op;
	pipeline->blend_src_alpha = blend->src_alpha;
	pipeline->blend_dst_alpha = blend->dst_alpha;
	pipeline->blend_alpha_op = blend->alpha_op;

	/* The components the case does not write. */
	pipeline->color_write_disable = ~blend->write_mask & 0xfU;

	/* Every pipeline has the same constants; the dynamic case must not use them. */
	kern_memcpy(pipeline->blend_constants, i915_vke1_pipeline_constants, sizeof(pipeline->blend_constants));
	pipeline->dynamic_blend_constants = blend->dynamic_constants;
}

/*
 * Describes one 2x1 RGBA8 texture of the texture step at its place in the
 * storage, laid out as the executor lays out a created image, its view
 * and its sampler: clamped to the edge, linear or nearest as the generated
 * table says.
 */
static void
i915_vke1_texture_init(
	struct i915_vke1 *x,
	uint32_t index)
{
	struct i915_gfx_image *image;
	uint32_t filter;

	/* The texture, laid out by the executor's own layout. */
	image = &x->textures[index];
	image->format = VK_FORMAT_R8G8B8A8_UNORM;
	image->width = I915_VKE1_TEXTURE_WIDTH;
	image->height = 1U;
	image->usage = VK_IMAGE_USAGE_SAMPLED_BIT;
	image->levels = 1U;
	(void)drv_i915_gfx_image_layout(image);
	image->memory = &x->memory;
	image->offset = I915_VKE1_TEXTURE_OFFSET + index * I915_VKE1_TEXTURE_STRIDE;

	/* A view of its one level. */
	x->texture_views[index].image = image;
	x->texture_views[index].format = VK_FORMAT_R8G8B8A8_UNORM;
	x->texture_views[index].base_level = 0U;
	x->texture_views[index].level_count = 1U;

	/* Its sampler: the table's filter both ways, clamped to the edge, level 0 only. */
	filter = VK_FILTER_NEAREST;
	if (i915_vke1_texture_linear[index] != 0U)
		filter = VK_FILTER_LINEAR;
	x->samplers[index].mag_filter = filter;
	x->samplers[index].min_filter = filter;
	x->samplers[index].address_u = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	x->samplers[index].address_v = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	x->samplers[index].mipmap_mode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
}

/* Publishes the objects the wire names under the scenario's identities. */
static int
i915_vke1_objects_publish(
	struct i915_vke1 *x)
{
	struct i915_render_device *vk;
	uint32_t index;
	int error;

	/* Publishes the buffers, the pass, the framebuffer, the pipelines and the sets, stopping at the first refusal. */
	vk = x->device->vk;
	error = drv_i915_object_insert(vk, I915_VK_OBJ_BUFFER, I915_VKE1_ID_VERTICES, &x->vertices);
	if (error == 0)
		error = drv_i915_object_insert(vk, I915_VK_OBJ_BUFFER, I915_VKE1_ID_INDICES, &x->indices);
	if (error == 0)
		error = drv_i915_object_insert(vk, I915_VK_OBJ_BUFFER, I915_VKE1_ID_TRANSFORMS, &x->transforms);
	if (error == 0)
		error = drv_i915_object_insert(vk, I915_VK_OBJ_BUFFER, I915_VKE1_ID_MATERIALS, &x->materials);
	if (error == 0)
		error = drv_i915_object_insert(vk, I915_VK_OBJ_RENDER_PASS, I915_VKE1_ID_PASS, &x->pass);
	if (error == 0)
		error = drv_i915_object_insert(vk, I915_VK_OBJ_FRAMEBUFFER, I915_VKE1_ID_FRAMEBUFFER, &x->framebuffer);
	if (error == 0)
		error = drv_i915_object_insert(vk, I915_VK_OBJ_PIPELINE, I915_VKE1_ID_UBO_PIPELINE, &x->ubo_pipeline);
	if (error == 0)
		error = drv_i915_object_insert(vk, I915_VK_OBJ_PIPELINE, I915_VKE1_ID_TEX_PIPELINE, &x->tex_pipeline);
	if (error == 0)
		error = drv_i915_object_insert(vk, I915_VK_OBJ_DESCRIPTOR_SET, I915_VKE1_ID_UBO_SET, &x->ubo_set);
	if (error == 0)
		error = drv_i915_object_insert(vk, I915_VK_OBJ_DESCRIPTOR_SET, I915_VKE1_ID_DYNAMIC_SET, &x->dynamic_set);
	if (error == 0)
		error = drv_i915_object_insert(vk, I915_VK_OBJ_DESCRIPTOR_SET, I915_VKE1_ID_TEX_SET0, &x->tex_set0);
	if (error == 0)
		error = drv_i915_object_insert(vk, I915_VK_OBJ_DESCRIPTOR_SET, I915_VKE1_ID_TEX_SET1, &x->tex_set1);

	/* Publishes the blend pipelines, one identity each. */
	for (index = 0U; error == 0 && index < I915_VKE1_BLEND_CASES; index++)
		error = drv_i915_object_insert(vk, I915_VK_OBJ_PIPELINE, I915_VKE1_ID_BLEND_PIPELINE + index, &x->blend_pipelines[index]);

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
i915_vke1_objects_withdraw(
	struct i915_vke1 *x)
{
	struct i915_render_device *vk;
	uint32_t index;

	/* Removes each identity from the table. */
	vk = x->device->vk;
	drv_i915_object_remove(vk, I915_VK_OBJ_BUFFER, I915_VKE1_ID_VERTICES);
	drv_i915_object_remove(vk, I915_VK_OBJ_BUFFER, I915_VKE1_ID_INDICES);
	drv_i915_object_remove(vk, I915_VK_OBJ_BUFFER, I915_VKE1_ID_TRANSFORMS);
	drv_i915_object_remove(vk, I915_VK_OBJ_BUFFER, I915_VKE1_ID_MATERIALS);
	drv_i915_object_remove(vk, I915_VK_OBJ_RENDER_PASS, I915_VKE1_ID_PASS);
	drv_i915_object_remove(vk, I915_VK_OBJ_FRAMEBUFFER, I915_VKE1_ID_FRAMEBUFFER);
	drv_i915_object_remove(vk, I915_VK_OBJ_PIPELINE, I915_VKE1_ID_UBO_PIPELINE);
	drv_i915_object_remove(vk, I915_VK_OBJ_PIPELINE, I915_VKE1_ID_TEX_PIPELINE);
	drv_i915_object_remove(vk, I915_VK_OBJ_DESCRIPTOR_SET, I915_VKE1_ID_UBO_SET);
	drv_i915_object_remove(vk, I915_VK_OBJ_DESCRIPTOR_SET, I915_VKE1_ID_DYNAMIC_SET);
	drv_i915_object_remove(vk, I915_VK_OBJ_DESCRIPTOR_SET, I915_VKE1_ID_TEX_SET0);
	drv_i915_object_remove(vk, I915_VK_OBJ_DESCRIPTOR_SET, I915_VKE1_ID_TEX_SET1);
	for (index = 0U; index < I915_VKE1_BLEND_CASES; index++)
		drv_i915_object_remove(vk, I915_VK_OBJ_PIPELINE, I915_VKE1_ID_BLEND_PIPELINE + index);
	x->published = 0;
}

/* Compiles every pipeline's kernels with the executor's compiler. */
static int
i915_vke1_pipelines_prepare(
	struct i915_vke1 *x)
{
	uint32_t index;
	int error;

	/* The blend pipelines. */
	for (index = 0U; index < I915_VKE1_BLEND_CASES; index++) {
		error = drv_i915_gfx_pipeline_prepare(x->render, &x->blend_pipelines[index]);
		if (error != 0)
			return error;
	}

	/* The uniform pipeline. */
	error = drv_i915_gfx_pipeline_prepare(x->render, &x->ubo_pipeline);
	if (error != 0)
		return error;

	/* The texturing pipeline. */
	error = drv_i915_gfx_pipeline_prepare(x->render, &x->tex_pipeline);
	if (error != 0)
		return error;

	/* Says how the uniform pipeline receives its blocks and how many images the texturing one samples. */
	kern_logf("i915: vke1: uniform push registers: vertex %u (blocks %u), fragment %u (blocks %u); texture step samplers %u\n",
		  x->ubo_pipeline.vs_binary->push_regs,
		  x->ubo_pipeline.vs_binary->block_count,
		  x->ubo_pipeline.fs_binary->push_regs,
		  x->ubo_pipeline.fs_binary->block_count,
		  x->tex_pipeline.fs_binary->sampler_count);

	/* Succeeded: every pipeline can draw. */
	return 0;
}

/* Releases the kernels of every pipeline; a pipeline never prepared has none. */
static void
i915_vke1_pipelines_release(
	struct i915_vke1 *x)
{
	uint32_t index;

	/* Releases each pipeline's kernels. */
	for (index = 0U; index < I915_VKE1_BLEND_CASES; index++)
		drv_i915_gfx_pipeline_release(&x->blend_pipelines[index]);
	drv_i915_gfx_pipeline_release(&x->ubo_pipeline);
	drv_i915_gfx_pipeline_release(&x->tex_pipeline);
}

/*
 * Writes what the steps read and flushes it to memory: the vertex buffer
 * (sixteen blend cells in their cases' colours, the uniform quad over the
 * whole target, the textured quad over pixels 16 .. 48), the six indices
 * of a quad, both transform and both material blocks, and the texels.
 */
static void
i915_vke1_data_write(
	struct i915_vke1 *x)
{
	static const uint32_t white[4] = { I915_VKE1_F_1, I915_VKE1_F_1, I915_VKE1_F_1, I915_VKE1_F_1 };
	static const uint32_t quad_indices[6] = { 0U, 1U, 2U, 0U, 2U, 3U };
	uint32_t *words;
	uint32_t *texels;
	uint32_t index;
	unsigned column;
	unsigned row;

	/* The blend cells: case k at column k % 4 and row k / 4, 16 pixels (2 NDC steps) square. */
	words = (uint32_t *)(void *)(x->cpu + I915_VKE1_VERTEX_OFFSET);
	for (index = 0U; index < I915_VKE1_BLEND_CASES; index++) {
		column = index % 4U;
		row = index / 4U;
		i915_vke1_quad_write(words + index * 4U * 8U,
				     column * 2U,
				     row * 2U,
				     column * 2U + 2U,
				     row * 2U + 2U,
				     i915_vke1_blend_cases[index].color);
	}

	/* The uniform quad over the whole target, and the textured quad over pixels 16 .. 48. */
	i915_vke1_quad_write(words + I915_VKE1_UBO_VERTEX * 8U, 0U, 0U, 8U, 8U, white);
	i915_vke1_texquad_write(words + I915_VKE1_TEX_VERTEX * 8U, 2U, 2U, 6U, 6U);
	drv_i915_gt_clflush(words, I915_VKE1_VERTEX_BYTES);

	/* The indices of one quad. */
	kern_memcpy(x->cpu + I915_VKE1_INDEX_OFFSET, quad_indices, sizeof(quad_indices));
	drv_i915_gt_clflush(x->cpu + I915_VKE1_INDEX_OFFSET, I915_VKE1_INDEX_BYTES);

	/* The transforms and the materials, the second of each at the second block's offset. */
	kern_memset(x->cpu + I915_VKE1_TRANSFORM_OFFSET, 0, I915_VKE1_UNIFORM_BYTES);
	kern_memset(x->cpu + I915_VKE1_MATERIAL_OFFSET, 0, I915_VKE1_UNIFORM_BYTES);
	kern_memcpy(x->cpu + I915_VKE1_TRANSFORM_OFFSET, i915_vke1_transforms, I915_VKE1_TRANSFORM_FLOATS * 4U);
	kern_memcpy(x->cpu + I915_VKE1_TRANSFORM_OFFSET + I915_VKE1_SECOND_BLOCK,
	       &i915_vke1_transforms[I915_VKE1_TRANSFORM_FLOATS],
	       I915_VKE1_TRANSFORM_FLOATS * 4U);
	kern_memcpy(x->cpu + I915_VKE1_MATERIAL_OFFSET, i915_vke1_materials, I915_VKE1_MATERIAL_FLOATS * 4U);
	kern_memcpy(x->cpu + I915_VKE1_MATERIAL_OFFSET + I915_VKE1_SECOND_BLOCK,
	       &i915_vke1_materials[I915_VKE1_MATERIAL_FLOATS],
	       I915_VKE1_MATERIAL_FLOATS * 4U);
	drv_i915_gt_clflush(x->cpu + I915_VKE1_TRANSFORM_OFFSET, I915_VKE1_UNIFORM_BYTES);
	drv_i915_gt_clflush(x->cpu + I915_VKE1_MATERIAL_OFFSET, I915_VKE1_UNIFORM_BYTES);

	/* The two texels of each texture, at the start of its first row. */
	for (index = 0U; index < I915_VKE1_TEXTURES; index++) {
		texels = (uint32_t *)(void *)(x->cpu + x->textures[index].offset);
		kern_memset(texels, 0, (size_t)x->textures[index].bytes);
		texels[0] = i915_vke1_texels[index * 2U];
		texels[1] = i915_vke1_texels[index * 2U + 1U];
		drv_i915_gt_clflush(texels, (size_t)x->textures[index].bytes);
	}
}

/* Writes the four vertices of an axis-aligned quad between NDC steps (left, top) and (right, bottom) in one colour. */
static void
i915_vke1_quad_write(
	uint32_t *words,
	unsigned left,
	unsigned top,
	unsigned right,
	unsigned bottom,
	const uint32_t color[4])
{
	unsigned corner;
	unsigned x;
	unsigned y;

	/* Writes the corners in turn: top left, top right, bottom right, bottom left. */
	for (corner = 0U; corner < 4U; corner++) {
		/* The right corners are the second and third, the bottom ones the third and fourth. */
		x = left;
		if (corner == 1U || corner == 2U)
			x = right;
		y = top;
		if (corner >= 2U)
			y = bottom;

		/* The position (x, y, 0, 1), then the colour. */
		words[corner * 8U + 0U] = i915_vke1_ndc[x];
		words[corner * 8U + 1U] = i915_vke1_ndc[y];
		words[corner * 8U + 2U] = I915_VKE1_F_0;
		words[corner * 8U + 3U] = I915_VKE1_F_1;
		kern_memcpy(&words[corner * 8U + 4U], color, 4U * sizeof(color[0]));
	}
}

/*
 * Writes the four vertices of an axis-aligned quad between NDC steps
 * (left, top) and (right, bottom) whose colour is the texture coordinate:
 * (0, 0) at the top left, (1, 1) at the bottom right.
 */
static void
i915_vke1_texquad_write(
	uint32_t *words,
	unsigned left,
	unsigned top,
	unsigned right,
	unsigned bottom)
{
	unsigned corner;
	unsigned x;
	unsigned y;
	uint32_t u;
	uint32_t v;

	/* Writes the corners in turn: top left, top right, bottom right, bottom left. */
	for (corner = 0U; corner < 4U; corner++) {
		/* The right corners are the second and third, the bottom ones the third and fourth. */
		x = left;
		u = I915_VKE1_F_0;
		if (corner == 1U || corner == 2U) {
			x = right;
			u = I915_VKE1_F_1;
		}
		y = top;
		v = I915_VKE1_F_0;
		if (corner >= 2U) {
			y = bottom;
			v = I915_VKE1_F_1;
		}

		/* The position (x, y, 0, 1), then the coordinate (u, v, 0, 1). */
		words[corner * 8U + 0U] = i915_vke1_ndc[x];
		words[corner * 8U + 1U] = i915_vke1_ndc[y];
		words[corner * 8U + 2U] = I915_VKE1_F_0;
		words[corner * 8U + 3U] = I915_VKE1_F_1;
		words[corner * 8U + 4U] = u;
		words[corner * 8U + 5U] = v;
		words[corner * 8U + 6U] = I915_VKE1_F_0;
		words[corner * 8U + 7U] = I915_VKE1_F_1;
	}
}

/* Appends one little-endian word to the stream; a stream that would overflow is marked. */
static void
i915_vke1_put32(
	struct i915_vke1 *x,
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
i915_vke1_put64(
	struct i915_vke1 *x,
	uint64_t value)
{
	/* The low word first. */
	i915_vke1_put32(x, (uint32_t)value);
	i915_vke1_put32(x, (uint32_t)(value >> 32));
}

/* Appends the head of a recording: [opcode][no reply][command buffer]. */
static void
i915_vke1_record(
	struct i915_vke1 *x,
	uint32_t opcode)
{
	/* A recording asks for no reply. */
	i915_vke1_put32(x, opcode);
	i915_vke1_put32(x, 0U);
	i915_vke1_put64(x, I915_VKE1_ID_COMMAND_BUFFER);
}

/* Executes the stream built so far and empties it; the replies stay in the reply area. */
static int
i915_vke1_execute(
	struct i915_vke1 *x,
	const char *what)
{
	int error;

	/* Refuses a stream that did not fit. */
	if (x->overflow != 0) {
		kern_logf("i915: vke1: %s: the stream does not fit %u bytes\n", what, I915_VKE1_WIRE_BYTES);
		return ENOSPC;
	}

	/* Executes it into the reply area. */
	x->reply_bytes = sizeof(x->reply);
	error = drv_i915_render_execute(x->render, x->wire, x->used, x->reply, &x->reply_bytes);
	x->used = 0U;
	if (error != 0) {
		kern_logf("i915: vke1: %s: the executor refused the stream: %d\n", what, error);
		return error;
	}

	/* Succeeded: the replies are in the reply area. */
	return 0;
}

/* Reads one reply word; zero past the replies. */
static uint32_t
i915_vke1_reply32(
	const struct i915_vke1 *x,
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
i915_vke1_pool_create(
	struct i915_vke1 *x)
{
	uint32_t result;
	int error;

	/* vkCreateCommandPool: [85][reply][device][present][sType 39][no chain][flags][family][no allocator][present][identity]. */
	x->used = 0U;
	i915_vke1_put32(x, I915_VKE1_OP_CREATE_COMMAND_POOL);
	i915_vke1_put32(x, 1U);
	i915_vke1_put64(x, 0U);
	i915_vke1_put64(x, 1U);
	i915_vke1_put32(x, 39U);
	i915_vke1_put64(x, 0U);
	i915_vke1_put32(x, 0U);
	i915_vke1_put32(x, 0U);
	i915_vke1_put64(x, 0U);
	i915_vke1_put64(x, 1U);
	i915_vke1_put64(x, I915_VKE1_ID_POOL);

	/* vkAllocateCommandBuffers: [88][reply][device][present][sType 40][no chain][pool][primary][1][1][identity]. */
	i915_vke1_put32(x, I915_VKE1_OP_ALLOCATE_COMMAND_BUFFERS);
	i915_vke1_put32(x, 1U);
	i915_vke1_put64(x, 0U);
	i915_vke1_put64(x, 1U);
	i915_vke1_put32(x, 40U);
	i915_vke1_put64(x, 0U);
	i915_vke1_put64(x, I915_VKE1_ID_POOL);
	i915_vke1_put32(x, VK_COMMAND_BUFFER_LEVEL_PRIMARY);
	i915_vke1_put32(x, 1U);
	i915_vke1_put64(x, 1U);
	i915_vke1_put64(x, I915_VKE1_ID_COMMAND_BUFFER);
	error = i915_vke1_execute(x, "create the command pool");
	if (error != 0)
		return error;

	/* The pool exists from here on, whatever the allocation answered. */
	x->pooled = 1;

	/* Refuses a pool the executor did not make: [85][result][present][identity]. */
	result = i915_vke1_reply32(x, 4U);
	if (result != VK_SUCCESS)
		return EIO;

	/* Refuses a buffer it did not allocate: [88][result][count][identity] behind the pool's reply. */
	result = i915_vke1_reply32(x, 24U + 4U);
	if (result != VK_SUCCESS)
		return EIO;

	/* Succeeded: the command buffer can record. */
	return 0;
}

/* Starts a stream with vkBeginCommandBuffer, which empties the recording: [90][reply][buffer][present][sType 42][no chain][flags][no inheritance]. */
static void
i915_vke1_begin(
	struct i915_vke1 *x)
{
	/* The begin asks for its reply. */
	x->used = 0U;
	x->overflow = 0;
	i915_vke1_put32(x, I915_VKE1_OP_BEGIN_COMMAND_BUFFER);
	i915_vke1_put32(x, 1U);
	i915_vke1_put64(x, I915_VKE1_ID_COMMAND_BUFFER);
	i915_vke1_put64(x, 1U);
	i915_vke1_put32(x, 42U);
	i915_vke1_put64(x, 0U);
	i915_vke1_put32(x, 0U);
	i915_vke1_put64(x, 0U);
}

/*
 * Appends vkCmdBeginRenderPass of the whole target, cleared to a colour
 * given as float bits: [present][sType 43][no chain][pass][framebuffer]
 * [area][present][1]{[colour][tag][4][r g b a]}[inline].
 */
static void
i915_vke1_begin_pass(
	struct i915_vke1 *x,
	const uint32_t clear[4])
{
	uint32_t index;

	/* The begin info. */
	i915_vke1_record(x, I915_VKE1_OP_BEGIN_RENDER_PASS);
	i915_vke1_put64(x, 1U);
	i915_vke1_put32(x, 43U);
	i915_vke1_put64(x, 0U);
	i915_vke1_put64(x, I915_VKE1_ID_PASS);
	i915_vke1_put64(x, I915_VKE1_ID_FRAMEBUFFER);

	/* The render area: the whole target. */
	i915_vke1_put32(x, 0U);
	i915_vke1_put32(x, 0U);
	i915_vke1_put32(x, I915_VKE1_SIZE);
	i915_vke1_put32(x, I915_VKE1_SIZE);

	/* One colour clear value. */
	i915_vke1_put32(x, 1U);
	i915_vke1_put64(x, 1U);
	i915_vke1_put32(x, 0U);
	i915_vke1_put32(x, 0U);
	i915_vke1_put64(x, 4U);
	for (index = 0U; index < 4U; index++)
		i915_vke1_put32(x, clear[index]);

	/* The subpass contents are inline. */
	i915_vke1_put32(x, VK_SUBPASS_CONTENTS_INLINE);
}

/* Appends vkCmdBindPipeline of a graphics pipeline. */
static void
i915_vke1_bind_pipeline(
	struct i915_vke1 *x,
	uint64_t pipeline)
{
	/* [bind point][pipeline]. */
	i915_vke1_record(x, I915_VKE1_OP_BIND_PIPELINE);
	i915_vke1_put32(x, VK_PIPELINE_BIND_POINT_GRAPHICS);
	i915_vke1_put64(x, pipeline);
}

/* Appends the binds of the vertex buffer at binding 0 and of the 32-bit quad indices. */
static void
i915_vke1_bind_buffers(
	struct i915_vke1 *x)
{
	/* vkCmdBindVertexBuffers: [first][present][1]{buffer}[1]{offset}. */
	i915_vke1_record(x, I915_VKE1_OP_BIND_VERTEX_BUFFERS);
	i915_vke1_put32(x, 0U);
	i915_vke1_put32(x, 1U);
	i915_vke1_put64(x, 1U);
	i915_vke1_put64(x, I915_VKE1_ID_VERTICES);
	i915_vke1_put64(x, 1U);
	i915_vke1_put64(x, 0U);

	/* vkCmdBindIndexBuffer: [buffer][offset][index type]. */
	i915_vke1_record(x, I915_VKE1_OP_BIND_INDEX_BUFFER);
	i915_vke1_put64(x, I915_VKE1_ID_INDICES);
	i915_vke1_put64(x, 0U);
	i915_vke1_put32(x, VK_INDEX_TYPE_UINT32);
}

/* Appends vkCmdDrawIndexed of the quad whose first vertex is given: [6][1][0][first vertex][0]. */
static void
i915_vke1_draw_quad(
	struct i915_vke1 *x,
	uint32_t first_vertex)
{
	/* The six indices of one instance, moved to the quad by the vertex offset. */
	i915_vke1_record(x, I915_VKE1_OP_DRAW_INDEXED);
	i915_vke1_put32(x, 6U);
	i915_vke1_put32(x, 1U);
	i915_vke1_put32(x, 0U);
	i915_vke1_put32(x, first_vertex);
	i915_vke1_put32(x, 0U);
}

/* Appends vkCmdBindDescriptorSets of sets from `first` with their dynamic offsets: [bind point][layout][first][n][n]{set}[m][m]{offset}. */
static void
i915_vke1_bind_sets(
	struct i915_vke1 *x,
	uint32_t first,
	const uint64_t *sets,
	uint32_t set_count,
	const uint32_t *offsets,
	uint32_t offset_count)
{
	uint32_t index;

	/* The graphics bind point, no layout, the first set and the sets. */
	i915_vke1_record(x, I915_VKE1_OP_BIND_DESCRIPTOR_SETS);
	i915_vke1_put32(x, VK_PIPELINE_BIND_POINT_GRAPHICS);
	i915_vke1_put64(x, 0U);
	i915_vke1_put32(x, first);
	i915_vke1_put32(x, set_count);
	i915_vke1_put64(x, set_count);
	for (index = 0U; index < set_count; index++)
		i915_vke1_put64(x, sets[index]);

	/* The dynamic offsets. */
	i915_vke1_put32(x, offset_count);
	i915_vke1_put64(x, offset_count);
	for (index = 0U; index < offset_count; index++)
		i915_vke1_put32(x, offsets[index]);
}

/* Appends vkCmdSetBlendConstants: [4]{float}. */
static void
i915_vke1_set_blend_constants(
	struct i915_vke1 *x,
	const uint32_t constants[4])
{
	uint32_t index;

	/* The four constants as float bits. */
	i915_vke1_record(x, I915_VKE1_OP_SET_BLEND_CONSTANTS);
	i915_vke1_put64(x, 4U);
	for (index = 0U; index < 4U; index++)
		i915_vke1_put32(x, constants[index]);
}

/*
 * Appends vkUpdateDescriptorSets of one buffer descriptor:
 * [79][reply][device][1][1]{[sType 35][no chain][set][binding][0][1][type]
 * [0][1]{buffer offset range}[0]}[0][0].
 */
static void
i915_vke1_buffer_write(
	struct i915_vke1 *x,
	uint64_t set,
	uint32_t binding,
	uint32_t type,
	uint64_t buffer,
	uint64_t offset,
	uint64_t range)
{
	/* The command and its one write. */
	i915_vke1_put32(x, I915_VKE1_OP_UPDATE_DESCRIPTOR_SETS);
	i915_vke1_put32(x, 1U);
	i915_vke1_put64(x, 0U);
	i915_vke1_put32(x, 1U);
	i915_vke1_put64(x, 1U);

	/* The write's head: the set, the binding, element 0, one descriptor of the type. */
	i915_vke1_put32(x, 35U);
	i915_vke1_put64(x, 0U);
	i915_vke1_put64(x, set);
	i915_vke1_put32(x, binding);
	i915_vke1_put32(x, 0U);
	i915_vke1_put32(x, 1U);
	i915_vke1_put32(x, type);

	/* No images, the one buffer, no texel views. */
	i915_vke1_put64(x, 0U);
	i915_vke1_put64(x, 1U);
	i915_vke1_put64(x, buffer);
	i915_vke1_put64(x, offset);
	i915_vke1_put64(x, range);
	i915_vke1_put64(x, 0U);

	/* No copies. */
	i915_vke1_put32(x, 0U);
	i915_vke1_put64(x, 0U);
}

/*
 * Appends the end and a submission of the command buffer, executes the
 * stream and checks the three replies: [90][result][91][result][18][result].
 */
static int
i915_vke1_finish(
	struct i915_vke1 *x,
	const char *what)
{
	uint32_t begun;
	uint32_t ended;
	uint32_t submitted;
	int error;

	/* vkEndCommandBuffer: [91][reply][buffer]. */
	i915_vke1_put32(x, I915_VKE1_OP_END_COMMAND_BUFFER);
	i915_vke1_put32(x, 1U);
	i915_vke1_put64(x, I915_VKE1_ID_COMMAND_BUFFER);

	/* vkQueueSubmit of the one buffer, with no semaphores and no fence. */
	i915_vke1_put32(x, I915_VKE1_OP_QUEUE_SUBMIT);
	i915_vke1_put32(x, 1U);
	i915_vke1_put64(x, 0U);
	i915_vke1_put32(x, 1U);
	i915_vke1_put64(x, 1U);
	i915_vke1_put32(x, 4U);
	i915_vke1_put64(x, 0U);
	i915_vke1_put32(x, 0U);
	i915_vke1_put64(x, 0U);
	i915_vke1_put64(x, 0U);
	i915_vke1_put32(x, 1U);
	i915_vke1_put64(x, 1U);
	i915_vke1_put64(x, I915_VKE1_ID_COMMAND_BUFFER);
	i915_vke1_put32(x, 0U);
	i915_vke1_put64(x, 0U);
	i915_vke1_put64(x, 0U);

	/* Runs the stream: the recording, then the submission to its end. */
	error = i915_vke1_execute(x, what);
	if (error != 0)
		return error;

	/* Takes the results of the begin, the end and the submission. */
	begun = i915_vke1_reply32(x, 4U);
	ended = i915_vke1_reply32(x, 12U);
	submitted = i915_vke1_reply32(x, 20U);

	/* Refuses a begin, an end or a submission that did not succeed. */
	if (begun != VK_SUCCESS ||
	    ended != VK_SUCCESS ||
	    submitted != VK_SUCCESS) {
		kern_logf("i915: vke1: %s: begin %d, end %d, submit %d\n",
			  what,
			  (int)begun,
			  (int)ended,
			  (int)submitted);
		return EIO;
	}

	/* Succeeded: the command buffer ran to its end. */
	return 0;
}

/* Expects every pixel of the target to hold a pixel value, each byte within a tolerance. */
static void
i915_vke1_expect_fill(
	struct i915_vke1 *x,
	uint32_t pixel,
	uint8_t tolerance)
{
	unsigned index;

	/* Every pixel holds the value. */
	for (index = 0U; index < I915_VKE1_SIZE * I915_VKE1_SIZE; index++) {
		x->expected[index] = pixel;
		x->tolerance[index] = tolerance;
	}
}

/* Expects the pixels of columns left .. right - 1 and rows top .. bottom - 1 to hold a pixel value, each byte within a tolerance. */
static void
i915_vke1_expect_rect(
	struct i915_vke1 *x,
	uint32_t left,
	uint32_t top,
	uint32_t right,
	uint32_t bottom,
	uint32_t pixel,
	uint8_t tolerance)
{
	uint32_t column;
	uint32_t row;

	/* Paints the rectangle into the expected image. */
	for (row = top; row < bottom; row++) {
		for (column = left; column < right; column++) {
			x->expected[row * I915_VKE1_SIZE + column] = pixel;
			x->tolerance[row * I915_VKE1_SIZE + column] = tolerance;
		}
	}
}

/* Decides whether every byte of a pixel is within a tolerance of the expected pixel's byte. */
static int
i915_vke1_near(
	uint32_t pixel,
	uint32_t expected,
	uint32_t tolerance)
{
	uint32_t shift;
	uint32_t have;
	uint32_t want;

	/* Compares each of the four bytes. */
	for (shift = 0U; shift < 32U; shift += 8U) {
		have = (pixel >> shift) & 0xffU;
		want = (expected >> shift) & 0xffU;
		if (have > want + tolerance)
			return 0;
		if (want > have + tolerance)
			return 0;
	}

	/* Succeeded: every byte is near. */
	return 1;
}

/* Compares the target with the expected image, each byte within its pixel's tolerance; EIO with the first difference logged. */
static int
i915_vke1_compare(
	struct i915_vke1 *x,
	const char *what)
{
	const uint32_t *pixels;
	unsigned differ;
	unsigned first;
	unsigned index;
	int near;

	/* Reads the target through the CPU view, past any line the CPU still caches. */
	pixels = (const uint32_t *)(const void *)(x->cpu + I915_VKE1_TARGET_OFFSET);
	drv_i915_gt_clflush(pixels, I915_VKE1_SIZE * I915_VKE1_SIZE * 4U);

	/* Counts the pixels that are not near and notes the first. */
	differ = 0U;
	first = 0U;
	for (index = 0U; index < I915_VKE1_SIZE * I915_VKE1_SIZE; index++) {
		near = i915_vke1_near(pixels[index], x->expected[index], x->tolerance[index]);
		if (near == 0) {
			if (differ == 0U)
				first = index;
			differ++;
		}
	}

	/* Says where the image differs. */
	if (differ != 0U) {
		kern_logf("i915: vke1: %s: %u of %u pixels differ; first at (%u,%u): 0x%08x, expected 0x%08x\n",
			  what,
			  differ,
			  I915_VKE1_SIZE * I915_VKE1_SIZE,
			  first % I915_VKE1_SIZE,
			  first / I915_VKE1_SIZE,
			  pixels[first],
			  x->expected[first]);
		return EIO;
	}

	/* Succeeded: every pixel is near its expected value. */
	return 0;
}

/* Logs a step's verdict and counts it. */
static void
i915_vke1_verdict(
	struct i915_vke1 *x,
	const char *what,
	int error)
{
	/* A step fails with the error that stopped it. */
	if (error != 0) {
		x->failed++;
		kern_logf("i915: vke1: VKE1-%s FAIL (%d)\n", what, error);
		return;
	}

	x->passed++;
	kern_logf("i915: vke1: VKE1-%s PASS\n", what);
}

/*
 * BLEND: the target is cleared to the destination colour, then each blend
 * case's pipeline draws its 16x16 cell in its colour; the dynamic case's
 * constants are set by vkCmdSetBlendConstants right before its draw, and
 * differ from every pipeline's own.  Each cell must hold the pixel the
 * Vulkan blend equation gives, each byte within one step; a cell whose
 * pixels differ is named.
 */
static void
i915_vke1_step_blend(
	struct i915_vke1 *x)
{
	const uint32_t *pixels;
	const struct i915_vke1_blend_case *blend;
	uint32_t index;
	uint32_t column;
	uint32_t row;
	int near;
	int error;

	/* Records the pass over the destination colour, then one draw per case. */
	i915_vke1_begin(x);
	i915_vke1_begin_pass(x, i915_vke1_destination);
	i915_vke1_bind_buffers(x);
	for (index = 0U; index < I915_VKE1_BLEND_CASES; index++) {
		/* The dynamic case takes its constants from the command buffer. */
		i915_vke1_bind_pipeline(x, I915_VKE1_ID_BLEND_PIPELINE + index);
		if (i915_vke1_blend_cases[index].dynamic_constants != 0)
			i915_vke1_set_blend_constants(x, i915_vke1_dynamic_constants);
		i915_vke1_draw_quad(x, index * 4U);
	}

	i915_vke1_record(x, I915_VKE1_OP_END_RENDER_PASS);

	/* Runs it. */
	error = i915_vke1_finish(x, "BLEND");
	if (error != 0) {
		i915_vke1_verdict(x, "BLEND", error);
		return;
	}

	/* Names each case whose cell's first pixel is not what it expects. */
	pixels = (const uint32_t *)(const void *)(x->cpu + I915_VKE1_TARGET_OFFSET);
	drv_i915_gt_clflush(pixels, I915_VKE1_SIZE * I915_VKE1_SIZE * 4U);
	for (index = 0U; index < I915_VKE1_BLEND_CASES; index++) {
		blend = &i915_vke1_blend_cases[index];
		column = (index % 4U) * 16U;
		row = (index / 4U) * 16U;
		near = i915_vke1_near(pixels[row * I915_VKE1_SIZE + column], blend->expected, I915_VKE1_TOLERANCE);
		if (near == 0) {
			kern_logf("i915: vke1: BLEND case %u (%s): 0x%08x, expected 0x%08x\n",
				  index,
				  blend->name,
				  pixels[row * I915_VKE1_SIZE + column],
				  blend->expected);
		}
	}

	/* Compares the whole target: each cell in its case's pixel. */
	i915_vke1_expect_fill(x, I915_VKE1_DESTINATION_PIXEL, 0U);
	for (index = 0U; index < I915_VKE1_BLEND_CASES; index++) {
		column = (index % 4U) * 16U;
		row = (index / 4U) * 16U;
		i915_vke1_expect_rect(x, column, row, column + 16U, row + 16U, i915_vke1_blend_cases[index].expected, I915_VKE1_TOLERANCE);
	}

	error = i915_vke1_compare(x, "BLEND");
	i915_vke1_verdict(x, "BLEND", error);
}

/*
 * UBO: vkUpdateDescriptorSets gives the plain set the first transform
 * (binding 0) and the first material (binding 1), and the dynamic set the
 * second transform and the material buffer as a dynamic uniform buffer
 * from its start.  The first draw, with the plain set, places the quad by
 * the first transform and colours it from the first material; the second,
 * with the dynamic set and dynamic offset 256, by the second transform and
 * from the second material.  Each rectangle must hold its colour, each byte
 * within one step, on black.
 */
static void
i915_vke1_step_ubo(
	struct i915_vke1 *x)
{
	static const uint32_t black[4] = { I915_VKE1_F_0, I915_VKE1_F_0, I915_VKE1_F_0, I915_VKE1_F_1 };
	uint64_t set;
	uint32_t dynamic_offset;
	uint32_t index;
	int error;

	/* Writes the four descriptors through the wire. */
	x->used = 0U;
	x->overflow = 0;
	i915_vke1_buffer_write(x, I915_VKE1_ID_UBO_SET, 0U, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, I915_VKE1_ID_TRANSFORMS, 0U, 256U);
	i915_vke1_buffer_write(x, I915_VKE1_ID_UBO_SET, 1U, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, I915_VKE1_ID_MATERIALS, 0U, 256U);
	i915_vke1_buffer_write(x, I915_VKE1_ID_DYNAMIC_SET, 0U, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, I915_VKE1_ID_TRANSFORMS,
			       I915_VKE1_SECOND_BLOCK, VK_WHOLE_SIZE);
	i915_vke1_buffer_write(x, I915_VKE1_ID_DYNAMIC_SET, 1U, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, I915_VKE1_ID_MATERIALS, 0U, 256U);
	error = i915_vke1_execute(x, "UBO descriptors");
	if (error != 0) {
		i915_vke1_verdict(x, "UBO", error);
		return;
	}

	/* Refuses descriptors that did not land where they were written. */
	if (x->ubo_set.slots[1].buffer != &x->materials ||
	    x->dynamic_set.slots[0].offset != I915_VKE1_SECOND_BLOCK ||
	    x->dynamic_set.slots[1].dynamic == 0) {
		kern_logf("i915: vke1: UBO: vkUpdateDescriptorSets did not bind the uniform buffers\n");
		i915_vke1_verdict(x, "UBO", EIO);
		return;
	}

	/* Records the pass on black, the uniform pipeline and the two draws. */
	i915_vke1_begin(x);
	i915_vke1_begin_pass(x, black);
	i915_vke1_bind_pipeline(x, I915_VKE1_ID_UBO_PIPELINE);
	i915_vke1_bind_buffers(x);
	set = I915_VKE1_ID_UBO_SET;
	i915_vke1_bind_sets(x, 0U, &set, 1U, NULL, 0U);
	i915_vke1_draw_quad(x, I915_VKE1_UBO_VERTEX);
	set = I915_VKE1_ID_DYNAMIC_SET;
	dynamic_offset = I915_VKE1_SECOND_BLOCK;
	i915_vke1_bind_sets(x, 0U, &set, 1U, &dynamic_offset, 1U);
	i915_vke1_draw_quad(x, I915_VKE1_UBO_VERTEX);
	i915_vke1_record(x, I915_VKE1_OP_END_RENDER_PASS);

	/* Runs it and compares the target with the two rectangles on black. */
	error = i915_vke1_finish(x, "UBO");
	if (error == 0) {
		i915_vke1_expect_fill(x, I915_VKE1_BLACK, 0U);
		for (index = 0U; index < 2U; index++) {
			i915_vke1_expect_rect(x,
					      i915_vke1_ubo_expected[index].left,
					      i915_vke1_ubo_expected[index].top,
					      i915_vke1_ubo_expected[index].right,
					      i915_vke1_ubo_expected[index].bottom,
					      i915_vke1_ubo_expected[index].pixel,
					      I915_VKE1_TOLERANCE);
		}

		error = i915_vke1_compare(x, "UBO");
	}

	i915_vke1_verdict(x, "UBO", error);
}

/*
 * TEX3: one vkCmdBindDescriptorSets binds both texture sets as sets 0 and
 * 1; the textured quad over pixels 16 .. 48 writes the red of texture 0
 * (set 0 binding 0, linear), the green of texture 1 (set 0 binding 2,
 * nearest) and the blue of texture 2 (set 1 binding 1, nearest).  Every
 * pixel must be the generated one, each byte within two steps (the linear
 * filter's sub-texel weights).
 */
static void
i915_vke1_step_tex3(
	struct i915_vke1 *x)
{
	static const uint32_t black[4] = { I915_VKE1_F_0, I915_VKE1_F_0, I915_VKE1_F_0, I915_VKE1_F_1 };
	uint64_t sets[2];
	uint32_t index;
	int error;

	/* Records the pass on black, the texturing pipeline, both sets and the quad. */
	sets[0] = I915_VKE1_ID_TEX_SET0;
	sets[1] = I915_VKE1_ID_TEX_SET1;
	i915_vke1_begin(x);
	i915_vke1_begin_pass(x, black);
	i915_vke1_bind_pipeline(x, I915_VKE1_ID_TEX_PIPELINE);
	i915_vke1_bind_buffers(x);
	i915_vke1_bind_sets(x, 0U, sets, 2U, NULL, 0U);
	i915_vke1_draw_quad(x, I915_VKE1_TEX_VERTEX);
	i915_vke1_record(x, I915_VKE1_OP_END_RENDER_PASS);

	/* Runs it and compares every pixel with the generated image. */
	error = i915_vke1_finish(x, "TEX3");
	if (error == 0) {
		for (index = 0U; index < I915_VKE1_SIZE * I915_VKE1_SIZE; index++) {
			x->expected[index] = i915_vke1_tex3_expected[index];
			x->tolerance[index] = I915_VKE1_FILTER_TOLERANCE;
		}

		error = i915_vke1_compare(x, "TEX3");
	}

	i915_vke1_verdict(x, "TEX3", error);
}
