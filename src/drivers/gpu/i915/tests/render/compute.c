/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The compute scenario "vkcs": compute pipelines made through
 * the wire (vkCreateComputePipelines), their sets updated and bound at the
 * compute bind point and dispatched (vkCmdDispatch), run on the GPU and
 * checked against words computed here in integers.
 *
 * Like the other render scenarios, the thread waits for the node, opens a
 * session of its own and drives the executor with the commands libvulkan
 * sends; the buffers, the shader modules, the layouts and the sets the
 * commands name are made directly and published in the executor's object
 * table.  The steps, from the bring-up's single channel outwards:
 *
 *  - ONE: one group of one invocation (one thread, one channel), as the
 *    compute test of tests/execution/eu-test.c runs;
 *  - ADD: c[i] = a[i] + b[i] over 16 groups of 64 for n = 1000, the words
 *    past n untouched;
 *  - ID: a 4 x 2 x 3 group over 3 x 2 x 2 groups, every invocation's
 *    built-ins, and a count of the invocations that ran;
 *  - ODD: the same with a 5 x 3 group (15 invocations, two threads, the
 *    last channel off);
 *  - PUSH: push constants, a uniform block and three storage buffers, one
 *    bound with a dynamic offset;
 *  - ATOMIC-SSBO: storage buffer atomics with and without the old value;
 *  - MIXED: a draw, a copy of its target into a buffer, a dispatch reading
 *    that buffer and writing vertices, and a draw of those vertices, in one
 *    command buffer;
 *  - MANYOPS: 200 dispatches of a recurrence in one submission, more than
 *    a batch holds;
 *  - SPILL: a kernel that spills to scratch memory over enough groups to
 *    fill every thread of the GPU;
 *
 * and the steps of shared memory and the workgroup barrier:
 *
 *  - SHARED: a 64 x 64 matrix transposed through 8 x 8 tiles of shared
 *    memory (two dynamic indices), a barrier between the write and the read;
 *  - REDUCE: 64 groups of 128 summed by a tree in shared memory, a barrier
 *    after each of the seven halvings;
 *  - ODD-BARRIER: groups of 5 x 3 (two threads, the last channel off)
 *    mirrored through shared memory across a barrier;
 *  - ATOMIC-SHARED: shared-memory atomics between barriers;
 *  - LOOP: prefix sums by a loop of six steps with two barriers each;
 *  - REFUSE: shared memory past 16 KiB, a barrier after a return and one in
 *    a loop some invocations skip are refused at vkCreateComputePipelines;
 *
 * and the steps of indirect dispatches, run-time arrays and a large grid:
 *
 *  - INDIRECT: ADD's grid of 16 groups read from a buffer at an offset
 *    (vkCmdDispatchIndirect);
 *  - INDIRECT-ID: ID's 3 x 2 x 2 groups read from a buffer, which is also
 *    what gl_NumWorkGroups reads;
 *  - INDIRECT-GPU: a dispatch writes the counts, and the indirect dispatch
 *    after it in the same command buffer reads them;
 *  - INDIRECT-ZERO: three indirect dispatches with a count of zero write
 *    nothing (and the GPU goes on);
 *  - LENGTH: .length() of run-time arrays over the bound ranges;
 *  - MANY: ADD over n = 4,000,000 in 62,500 groups, the size of Noct's
 *    demonstration, with three 16 MB buffers of objects of their own; the
 *    time of the submission is logged.
 *
 * Each step logs "VKCS-<name> PASS" or "FAIL", then the thread logs the
 * verdict and closes the session.
 */

#include "scenarios.h"
#include <kern/kcrt.h>

#include "../../compiler/compiler.h"
#include "../../i915.h"
#include "../../memory.h"
#include "../../session.h"
#include "../../sync.h"
#include "../../vk/vk-gfx.h"
#include "../../vk/vk-internal.h"
#include "../../vk/vk-object.h"
#include "../../vk/vk.h"

#include <drivers/gpu/gpu.h>
#include <kern/clock.h>
#include <kern/klog.h>
#include <kern/kmem.h>
#include <kern/lock.h>
#include <kern/sched.h>
#include <kern/thread.h>

#include <libc/vulkan/vulkan_core.h>

#include <uapi/errno.h>
#include <stddef.h>
#include <stdint.h>

#include "../fixtures/compute-shaders-gen.inc"

/* The target of the mixed step: 64 x 64 RGBA8. */
#define I915_VKCS_SIZE			64U
#define I915_VKCS_PIXELS		(I915_VKCS_SIZE * I915_VKCS_SIZE)

/* The storage: the target, then one 64 KiB region a buffer. */
#define I915_VKCS_STORAGE_BYTES		(1024U * 1024U)
#define I915_VKCS_REGION_BYTES		0x10000U
#define I915_VKCS_REGION_WORDS		(I915_VKCS_REGION_BYTES / 4U)

/* The buffers, as the index of their regions after the target's. */
#define I915_VKCS_BUF_A			0U
#define I915_VKCS_BUF_B			1U
#define I915_VKCS_BUF_C			2U
#define I915_VKCS_BUF_D			3U
#define I915_VKCS_BUF_P			4U
#define I915_VKCS_BUF_Q			5U
#define I915_VKCS_BUF_V			6U
#define I915_VKCS_BUF_W			7U
#define I915_VKCS_BUF_U			8U
#define I915_VKCS_BUF_S			9U
#define I915_VKCS_BUF_G			10U
#define I915_VKCS_BUF_MANY_A		11U
#define I915_VKCS_BUF_MANY_B		12U
#define I915_VKCS_BUF_MANY_C		13U
#define I915_VKCS_BUFFERS		14U


/* The compute kernels, as the index of their modules and pipelines. */
#define I915_VKCS_KERNEL_ONE		0U
#define I915_VKCS_KERNEL_ADD		1U
#define I915_VKCS_KERNEL_IDS		2U
#define I915_VKCS_KERNEL_ODD		3U
#define I915_VKCS_KERNEL_PUSH		4U
#define I915_VKCS_KERNEL_ATOMIC		5U
#define I915_VKCS_KERNEL_MIXED		6U
#define I915_VKCS_KERNEL_INC		7U
#define I915_VKCS_KERNEL_SPILL		8U
#define I915_VKCS_KERNEL_SHARED		9U
#define I915_VKCS_KERNEL_REDUCE		10U
#define I915_VKCS_KERNEL_SCAN		11U
#define I915_VKCS_KERNEL_ODDBAR		12U
#define I915_VKCS_KERNEL_ATOMSH		13U
#define I915_VKCS_KERNEL_REFUSE_BIG	14U
#define I915_VKCS_KERNEL_REFUSE_RETURN	15U
#define I915_VKCS_KERNEL_REFUSE_LOOP	16U
#define I915_VKCS_KERNEL_GRID		17U
#define I915_VKCS_KERNEL_LENGTH		18U
#define I915_VKCS_KERNELS		19U

/* The steps' sets, one a step that dispatches. */
#define I915_VKCS_SET_ONE		0U
#define I915_VKCS_SET_ADD		1U
#define I915_VKCS_SET_IDS		2U
#define I915_VKCS_SET_PUSH		3U
#define I915_VKCS_SET_ATOMIC		4U
#define I915_VKCS_SET_MIXED		5U
#define I915_VKCS_SET_INC		6U
#define I915_VKCS_SET_SPILL		7U
#define I915_VKCS_SET_TWO		8U
#define I915_VKCS_SET_THREE		9U
#define I915_VKCS_SET_GRID		10U
#define I915_VKCS_SET_LENGTH		11U
#define I915_VKCS_SET_MANY		12U
#define I915_VKCS_SETS			13U

/* The identities the wire names the scenario's objects by. */
#define I915_VKCS_IDENTITY		0x7e5ec50000000000ULL
#define I915_VKCS_ID_TARGET		(I915_VKCS_IDENTITY + 1U)
#define I915_VKCS_ID_CLEAR_PASS		(I915_VKCS_IDENTITY + 2U)
#define I915_VKCS_ID_LOAD_PASS		(I915_VKCS_IDENTITY + 3U)
#define I915_VKCS_ID_FRAMEBUFFER	(I915_VKCS_IDENTITY + 4U)
#define I915_VKCS_ID_POOL		(I915_VKCS_IDENTITY + 5U)
#define I915_VKCS_ID_COMMAND_BUFFER	(I915_VKCS_IDENTITY + 6U)
#define I915_VKCS_ID_GRAPHICS		(I915_VKCS_IDENTITY + 7U)
#define I915_VKCS_ID_BUFFER		(I915_VKCS_IDENTITY + 0x100U)
#define I915_VKCS_ID_MODULE		(I915_VKCS_IDENTITY + 0x200U)
#define I915_VKCS_ID_PIPELINE		(I915_VKCS_IDENTITY + 0x300U)
#define I915_VKCS_ID_SET		(I915_VKCS_IDENTITY + 0x400U)

/* The wire opcodes the scenario sends, as libvulkan numbers them. */
#define I915_VKCS_OP_QUEUE_SUBMIT		18U
#define I915_VKCS_OP_CREATE_COMPUTE_PIPELINES	66U
#define I915_VKCS_OP_DESTROY_PIPELINE		67U
#define I915_VKCS_OP_UPDATE_DESCRIPTOR_SETS	79U
#define I915_VKCS_OP_CREATE_COMMAND_POOL	85U
#define I915_VKCS_OP_DESTROY_COMMAND_POOL	86U
#define I915_VKCS_OP_ALLOCATE_COMMAND_BUFFERS	88U
#define I915_VKCS_OP_BEGIN_COMMAND_BUFFER	90U
#define I915_VKCS_OP_END_COMMAND_BUFFER		91U
#define I915_VKCS_OP_BIND_PIPELINE		93U
#define I915_VKCS_OP_BIND_DESCRIPTOR_SETS	103U
#define I915_VKCS_OP_BIND_VERTEX_BUFFERS	105U
#define I915_VKCS_OP_DRAW			106U
#define I915_VKCS_OP_DISPATCH			110U
#define I915_VKCS_OP_DISPATCH_INDIRECT		111U
#define I915_VKCS_OP_COPY_IMAGE_TO_BUFFER	116U
#define I915_VKCS_OP_PUSH_CONSTANTS		132U
#define I915_VKCS_OP_BEGIN_RENDER_PASS		133U
#define I915_VKCS_OP_END_RENDER_PASS		135U

/* The stream (MANYOPS records 200 pushes and dispatches in one) and its replies. */
#define I915_VKCS_WIRE_BYTES		32768U
#define I915_VKCS_REPLY_BYTES		1024U

/* How long the thread waits for the node. */
#define I915_VKCS_WAIT_S		120U

/* The word written where a step must not write. */
#define I915_VKCS_SENTINEL		0xdeadbeefU

/* Floats the steps pass, as their bits. */
#define I915_VKCS_F_0			0x00000000U
#define I915_VKCS_F_1			0x3f800000U
#define I915_VKCS_F_MINUS_1		0xbf800000U
#define I915_VKCS_F_64			0x42800000U
#define I915_VKCS_F_0_2			0x3e4ccccdU
#define I915_VKCS_F_0_4			0x3ecccccdU
#define I915_VKCS_F_0_6			0x3f19999aU

/* The steps' sizes. */
#define I915_VKCS_ADD_N			1000U
#define I915_VKCS_ADD_GROUPS		16U
#define I915_VKCS_PUSH_N		100U
#define I915_VKCS_PUSH_GROUPS		4U
#define I915_VKCS_PUSH_DYNAMIC		256U
#define I915_VKCS_ATOMIC_N		1000U
#define I915_VKCS_ATOMIC_GROUPS		16U
#define I915_VKCS_MANY_WORDS		256U
#define I915_VKCS_MANY_DISPATCHES	200U
#define I915_VKCS_SPILL_GROUPS		256U
#define I915_VKCS_RECORD_WORDS		13U
#define I915_VKCS_REDUCE_GROUPS		64U
#define I915_VKCS_SCAN_GROUPS		32U
#define I915_VKCS_ODDBAR_GROUPS		10U
#define I915_VKCS_ATOMSH_GROUPS		16U
#define I915_VKCS_ATOMSH_WORDS		20U

/*
 * The indirect steps' counts, as word offsets in G: INDIRECT's at 4 (byte
 * 16), INDIRECT-ID's at 0, INDIRECT-GPU's at 8 (written by the GPU), and
 * INDIRECT-ZERO's three grids at 12, 16 and 20.
 */
#define I915_VKCS_G_ADD			4U
#define I915_VKCS_G_IDS			0U
#define I915_VKCS_G_GPU			8U
#define I915_VKCS_G_ZERO		12U

/* The LENGTH step's ranges: 100 words of W; T's 8-byte head, 37 structures of 12 bytes and 5 bytes more. */
#define I915_VKCS_LENGTH_W_RANGE	400U
#define I915_VKCS_LENGTH_T_RANGE	457U

/* The MANY step: n, its groups of 64, and the bytes of each of its buffers. */
#define I915_VKCS_MANY_N		4000000U
#define I915_VKCS_MANY_GROUPS		62500U
#define I915_VKCS_MANY_BYTES		(I915_VKCS_MANY_N * 4U)
#define I915_VKCS_MANY_OBJECTS		3U

/* The words of the atomic step's block (atomic.comp's H). */
#define I915_VKCS_H_BINS		0U
#define I915_VKCS_H_TOTAL		16U
#define I915_VKCS_H_SMIN		17U
#define I915_VKCS_H_SMAX		18U
#define I915_VKCS_H_UMIN		19U
#define I915_VKCS_H_UMAX		20U
#define I915_VKCS_H_AND			21U
#define I915_VKCS_H_OR			22U
#define I915_VKCS_H_XOR			23U
#define I915_VKCS_H_SWAP		24U
#define I915_VKCS_H_CAS			25U
#define I915_VKCS_H_COUNT		26U
#define I915_VKCS_H_WORDS		27U

/* The most bindings a step's set has. */
#define I915_VKCS_MAX_BINDINGS		4U

/*
 * One binding of a step's set: its descriptor type, the buffer and the
 * range it names.
 */
struct i915_vkcs_binding {
	uint32_t type;
	uint32_t buffer;
	uint32_t range;
};

/*
 * Everything the scenario owns while its thread runs.
 *
 * It is allocated by the thread (the test kernel is near its size limit, so
 * it is not static), filled by the setup, used by the steps and emptied by
 * the teardown, all on the scenario's thread.
 */
struct i915_vkcs {
	/* The device, and the node and executor sessions the scenario opened. */
	struct i915_device *device;
	struct i915_session *session;
	struct i915_render_session *render;

	/* The storage object and its CPU view; the memory that names it. */
	struct i915_gem_object *storage;
	uint8_t *cpu;
	struct i915_gfx_memory memory;

	/* The target, its view, the two render passes (clearing and loading) and the framebuffer. */
	struct i915_gfx_image target;
	struct i915_gfx_view view;
	struct i915_gfx_pass clear_pass;
	struct i915_gfx_pass load_pass;
	struct i915_gfx_framebuffer framebuffer;

	/* The buffers, one region of the storage each (the MANY step's in objects of their own). */
	struct i915_gfx_buffer buffers[I915_VKCS_BUFFERS];

	/*
	 * The MANY step's three objects of 16 MB (a buffer is at most one
	 * object, which is contiguous memory), their CPU views and memories;
	 * nonzero many_ready once all three are bound.
	 */
	struct i915_gem_object *many_objects[I915_VKCS_MANY_OBJECTS];
	uint32_t *many_cpu[I915_VKCS_MANY_OBJECTS];
	struct i915_gfx_memory many_memory[I915_VKCS_MANY_OBJECTS];
	int many_ready;

	/* The compute modules (their pipelines are made through the wire), and the mixed step's graphics pipeline. */
	struct i915_gfx_shader modules[I915_VKCS_KERNELS];
	struct i915_gfx_shader mixed_vert;
	struct i915_gfx_shader mixed_frag;
	struct i915_gfx_pipeline graphics;
	int graphics_ready;

	/* Nonzero for each compute pipeline the executor made. */
	int made[I915_VKCS_KERNELS];

	/* The steps' layouts and sets. */
	struct i915_gfx_dsl layouts[I915_VKCS_SETS];
	struct i915_gfx_dset sets[I915_VKCS_SETS];

	/* Nonzero once the objects are published, and once the command pool exists. */
	int published;
	int pooled;

	/* The stream under construction, and nonzero once it overflowed. */
	uint8_t wire[I915_VKCS_WIRE_BYTES];
	size_t used;
	int overflow;

	/* The replies of the last stream, and how many bytes they took. */
	uint8_t reply[I915_VKCS_REPLY_BYTES];
	size_t reply_bytes;

	/* The words a check found wrong in the step being checked. */
	unsigned differ;

	/* How many steps passed and failed. */
	unsigned passed;
	unsigned failed;
};


/*
 * Each step's set: binding k's descriptor type, the buffer it names from
 * the buffer's start, and the range.  The recurrence of MANYOPS writes C as
 * ONE does; the ID and ODD steps count in B's first word.
 */
static const struct i915_vkcs_binding i915_vkcs_bindings[I915_VKCS_SETS][I915_VKCS_MAX_BINDINGS] = {
	[I915_VKCS_SET_ONE] = {
		{ VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, I915_VKCS_BUF_C, I915_VKCS_REGION_BYTES },
	},
	[I915_VKCS_SET_ADD] = {
		{ VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, I915_VKCS_BUF_A, I915_VKCS_REGION_BYTES },
		{ VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, I915_VKCS_BUF_B, I915_VKCS_REGION_BYTES },
		{ VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, I915_VKCS_BUF_C, I915_VKCS_REGION_BYTES },
	},
	[I915_VKCS_SET_IDS] = {
		{ VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, I915_VKCS_BUF_D, I915_VKCS_REGION_BYTES },
		{ VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, I915_VKCS_BUF_B, 4U },
	},
	[I915_VKCS_SET_PUSH] = {
		{ VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, I915_VKCS_BUF_U, 256U },
		{ VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, I915_VKCS_BUF_A, I915_VKCS_REGION_BYTES },
		{ VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, I915_VKCS_BUF_B, I915_VKCS_REGION_BYTES / 2U },
		{ VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, I915_VKCS_BUF_C, I915_VKCS_REGION_BYTES },
	},
	[I915_VKCS_SET_ATOMIC] = {
		{ VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, I915_VKCS_BUF_A, I915_VKCS_REGION_BYTES },
		{ VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, I915_VKCS_BUF_B, I915_VKCS_H_WORDS * 4U },
		{ VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, I915_VKCS_BUF_D, I915_VKCS_REGION_BYTES },
	},
	[I915_VKCS_SET_MIXED] = {
		{ VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, I915_VKCS_BUF_P, I915_VKCS_REGION_BYTES },
		{ VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, I915_VKCS_BUF_Q, I915_VKCS_REGION_BYTES },
		{ VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, I915_VKCS_BUF_V, I915_VKCS_REGION_BYTES },
	},
	[I915_VKCS_SET_INC] = {
		{ VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, I915_VKCS_BUF_C, I915_VKCS_REGION_BYTES },
	},
	[I915_VKCS_SET_SPILL] = {
		{ VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, I915_VKCS_BUF_S, I915_VKCS_REGION_BYTES },
	},
	[I915_VKCS_SET_TWO] = {
		{ VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, I915_VKCS_BUF_A, I915_VKCS_REGION_BYTES },
		{ VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, I915_VKCS_BUF_C, I915_VKCS_REGION_BYTES },
	},
	[I915_VKCS_SET_THREE] = {
		{ VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, I915_VKCS_BUF_A, I915_VKCS_REGION_BYTES },
		{ VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, I915_VKCS_BUF_B, I915_VKCS_REGION_BYTES },
		{ VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, I915_VKCS_BUF_D, I915_VKCS_REGION_BYTES },
	},
	[I915_VKCS_SET_GRID] = {
		{ VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, I915_VKCS_BUF_G, I915_VKCS_REGION_BYTES },
	},
	[I915_VKCS_SET_LENGTH] = {
		{ VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, I915_VKCS_BUF_C, I915_VKCS_REGION_BYTES },
		{ VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, I915_VKCS_BUF_A, I915_VKCS_LENGTH_W_RANGE },
		{ VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, I915_VKCS_BUF_B, I915_VKCS_LENGTH_T_RANGE },
	},
	[I915_VKCS_SET_MANY] = {
		{ VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, I915_VKCS_BUF_MANY_A, I915_VKCS_MANY_BYTES },
		{ VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, I915_VKCS_BUF_MANY_B, I915_VKCS_MANY_BYTES },
		{ VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, I915_VKCS_BUF_MANY_C, I915_VKCS_MANY_BYTES },
	},
};

/* How many bindings each step's set has. */
static const uint32_t i915_vkcs_binding_counts[I915_VKCS_SETS] = {
	[I915_VKCS_SET_ONE] = 1U,
	[I915_VKCS_SET_ADD] = 3U,
	[I915_VKCS_SET_IDS] = 2U,
	[I915_VKCS_SET_PUSH] = 4U,
	[I915_VKCS_SET_ATOMIC] = 3U,
	[I915_VKCS_SET_MIXED] = 3U,
	[I915_VKCS_SET_INC] = 1U,
	[I915_VKCS_SET_SPILL] = 1U,
	[I915_VKCS_SET_TWO] = 2U,
	[I915_VKCS_SET_THREE] = 3U,
	[I915_VKCS_SET_GRID] = 1U,
	[I915_VKCS_SET_LENGTH] = 3U,
	[I915_VKCS_SET_MANY] = 3U,
};

static void i915_vkcs_thread(void *argument);
static int i915_vkcs_wait_node(struct i915_device *device);
static int i915_vkcs_setup(struct i915_vkcs *x);
static void i915_vkcs_teardown(struct i915_vkcs *x);
static int i915_vkcs_storage_create(struct i915_vkcs *x);
static int i915_vkcs_many_create(struct i915_vkcs *x);
static void i915_vkcs_many_destroy(struct i915_vkcs *x);
static void i915_vkcs_objects_init(struct i915_vkcs *x);
static void i915_vkcs_layouts_init(struct i915_vkcs *x);
static void i915_vkcs_layout_init(struct i915_vkcs *x, uint32_t set, const struct i915_vkcs_binding *bindings, uint32_t count);
static int i915_vkcs_objects_publish(struct i915_vkcs *x);
static void i915_vkcs_objects_withdraw(struct i915_vkcs *x);
static void i915_vkcs_pipelines_create(struct i915_vkcs *x);
static void i915_vkcs_pipelines_destroy(struct i915_vkcs *x);
static int i915_vkcs_sets_update(struct i915_vkcs *x);
static uint32_t *i915_vkcs_words(struct i915_vkcs *x, uint32_t buffer);
static void i915_vkcs_flush(struct i915_vkcs *x, uint32_t buffer);
static void i915_vkcs_fill(struct i915_vkcs *x, uint32_t buffer, uint32_t value);
static void i915_vkcs_put32(struct i915_vkcs *x, uint32_t value);
static void i915_vkcs_put64(struct i915_vkcs *x, uint64_t value);
static void i915_vkcs_record(struct i915_vkcs *x, uint32_t opcode);
static int i915_vkcs_execute(struct i915_vkcs *x, const char *what);
static uint32_t i915_vkcs_reply32(const struct i915_vkcs *x, size_t offset);
static int i915_vkcs_pool_create(struct i915_vkcs *x);
static void i915_vkcs_begin(struct i915_vkcs *x);
static void i915_vkcs_bind_compute(struct i915_vkcs *x, uint32_t kernel, uint32_t set, int dynamic, uint32_t offset);
static void i915_vkcs_push(struct i915_vkcs *x, const uint32_t *words, uint32_t count);
static void i915_vkcs_dispatch(struct i915_vkcs *x, uint32_t gx, uint32_t gy, uint32_t gz);
static void i915_vkcs_dispatch_indirect(struct i915_vkcs *x, uint32_t buffer, uint32_t offset);
static void i915_vkcs_begin_pass(struct i915_vkcs *x, uint64_t pass);
static void i915_vkcs_draw(struct i915_vkcs *x, uint32_t buffer);
static void i915_vkcs_copy_target(struct i915_vkcs *x, uint32_t buffer);
static int i915_vkcs_finish(struct i915_vkcs *x, const char *what);
static void i915_vkcs_check(struct i915_vkcs *x, const char *what, uint32_t index, uint32_t word, uint32_t expected);
static int i915_vkcs_bytes_near(uint32_t word, uint32_t expected);
static int i915_vkcs_checked(struct i915_vkcs *x, const char *what);
static void i915_vkcs_verdict(struct i915_vkcs *x, const char *what, int error);
static int i915_vkcs_ready(struct i915_vkcs *x, uint32_t kernel, const char *what);
static uint32_t i915_vkcs_hash(uint32_t value);
static void i915_vkcs_step_one(struct i915_vkcs *x);
static void i915_vkcs_step_add(struct i915_vkcs *x);
static void i915_vkcs_step_ids(struct i915_vkcs *x, const char *what, uint32_t kernel, const uint32_t local[3], int indirect);
static void i915_vkcs_step_push(struct i915_vkcs *x);
static void i915_vkcs_step_atomic(struct i915_vkcs *x);
static void i915_vkcs_step_mixed(struct i915_vkcs *x);
static void i915_vkcs_step_many(struct i915_vkcs *x);
static void i915_vkcs_step_spill(struct i915_vkcs *x);
static uint32_t i915_vkcs_spill_word(uint32_t invocation);
static void i915_vkcs_step_shared(struct i915_vkcs *x);
static void i915_vkcs_step_reduce(struct i915_vkcs *x);
static void i915_vkcs_step_oddbar(struct i915_vkcs *x);
static void i915_vkcs_step_atomsh(struct i915_vkcs *x);
static void i915_vkcs_step_scan(struct i915_vkcs *x);
static void i915_vkcs_step_refuse(struct i915_vkcs *x);
static void i915_vkcs_add_inputs(struct i915_vkcs *x);
static void i915_vkcs_add_check(struct i915_vkcs *x, const char *what, uint32_t n);
static void i915_vkcs_grid(struct i915_vkcs *x, uint32_t at, uint32_t gx, uint32_t gy, uint32_t gz);
static void i915_vkcs_step_indirect(struct i915_vkcs *x);
static void i915_vkcs_step_indirect_gpu(struct i915_vkcs *x);
static void i915_vkcs_step_indirect_zero(struct i915_vkcs *x);
static void i915_vkcs_step_length(struct i915_vkcs *x);
static void i915_vkcs_step_big(struct i915_vkcs *x);
static uint64_t i915_vkcs_now_us(void);
static int i915_vkcs_run(struct i915_vkcs *x, const char *what, uint32_t kernel, uint32_t set, uint32_t groups);

/*
 * Starts the compute scenario.
 *
 * Returns at once: the scenario's thread runs the steps after the node is
 * published and logs their verdicts itself.
 */
void
drv_i915_test_render_compute(
	struct i915_device *device)
{
	struct thread *thread;
	int error;

	/* Starts the thread that waits for the node and runs the steps. */
	error = kthread_create(i915_vkcs_thread, device, SCHED_PRIORITY_DEFAULT, &thread);
	if (error != 0) {
		kern_logf("i915: vkcs: verdict FAIL (the scenario thread cannot be created: %d)\n", error);
		return;
	}

	/* The thread reclaims itself when the steps are done. */
	thread->detached = 1U;
	thread_start(thread);
	kern_logf("i915: vkcs: the steps run once the node is published\n");
}

/* Runs the scenario: waits for the node, sets up, runs every step, tears down and logs the verdict. */
static void
i915_vkcs_thread(
	void *argument)
{
	static const uint32_t ids_local[3] = { 4U, 2U, 3U };
	static const uint32_t odd_local[3] = { 5U, 3U, 1U };
	struct i915_device *device;
	struct i915_vkcs *x;
	unsigned passed;
	unsigned failed;
	int error;

	/* Waits until the node is published and its worker serves. */
	device = argument;
	error = i915_vkcs_wait_node(device);
	if (error != 0) {
		kern_logf("i915: vkcs: verdict FAIL (the node was not published within %u s)\n", I915_VKCS_WAIT_S);
		return;
	}

	/* Takes the scenario's state. */
	x = kern_calloc(1U, sizeof(*x));
	if (x == NULL) {
		kern_logf("i915: vkcs: verdict FAIL (no memory for the scenario's %u bytes)\n", (unsigned)sizeof(*x));
		return;
	}

	/* Opens the session and makes every object the steps use. */
	x->device = device;
	error = i915_vkcs_setup(x);
	if (error != 0) {
		kern_logf("i915: vkcs: verdict FAIL (setup: %d)\n", error);
		i915_vkcs_teardown(x);
		kern_free(x);
		return;
	}

	/* Runs every step, from the single channel outwards; each logs its own verdict. */
	i915_vkcs_step_one(x);
	i915_vkcs_step_add(x);
	i915_vkcs_step_ids(x, "ID", I915_VKCS_KERNEL_IDS, ids_local, 0);
	i915_vkcs_step_ids(x, "ODD", I915_VKCS_KERNEL_ODD, odd_local, 0);
	i915_vkcs_step_push(x);
	i915_vkcs_step_atomic(x);
	i915_vkcs_step_mixed(x);
	i915_vkcs_step_many(x);
	i915_vkcs_step_spill(x);
	i915_vkcs_step_shared(x);
	i915_vkcs_step_reduce(x);
	i915_vkcs_step_oddbar(x);
	i915_vkcs_step_atomsh(x);
	i915_vkcs_step_scan(x);
	i915_vkcs_step_refuse(x);

	/* The indirect dispatches, the run-time arrays and the large grid. */
	i915_vkcs_step_indirect(x);
	i915_vkcs_step_ids(x, "INDIRECT-ID", I915_VKCS_KERNEL_IDS, ids_local, 1);
	i915_vkcs_step_indirect_gpu(x);
	i915_vkcs_step_indirect_zero(x);
	i915_vkcs_step_length(x);
	i915_vkcs_step_big(x);

	/* Gives everything back and says how the steps went. */
	i915_vkcs_teardown(x);
	passed = x->passed;
	failed = x->failed;
	kern_free(x);
	if (failed != 0U) {
		kern_logf("i915: vkcs: verdict FAIL (%u of %u steps passed)\n", passed, passed + failed);
		return;
	}

	kern_logf("i915: vkcs: verdict PASS (%u of %u steps passed)\n", passed, passed + failed);
}

/* Waits until the node is published, sleeping a twentieth of a second at a time; ETIMEDOUT when it never is. */
static int
i915_vkcs_wait_node(
	struct i915_device *device)
{
	struct i915_completion nap;
	unsigned waited;

	/* A completion nobody signals: each wait on it simply lasts until its deadline. */
	drv_i915_completion_init(&nap, "i915 vkcs");

	/* Looks for the published node until the budget is spent. */
	for (waited = 0U; waited < I915_VKCS_WAIT_S * 20U; waited++) {
		/* The node is published once the GPU core holds it. */
		if (device->gpu != NULL && device->vk != NULL)
			return 0;

		(void)drv_i915_wait_for_completion(&nap, sched_ticks() + kern_ms_to_ticks(50U));
	}

	/* The node never came. */
	return ETIMEDOUT;
}

/* Opens the session, makes the storage, the objects, the pipelines, the sets' contents and a command pool. */
static int
i915_vkcs_setup(
	struct i915_vkcs *x)
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
	error = i915_vkcs_storage_create(x);
	if (error != 0)
		return error;

	/* Makes the MANY step's objects; without them only that step fails. */
	error = i915_vkcs_many_create(x);
	if (error != 0)
		kern_logf("i915: vkcs: the MANY step's objects were not made: %d\n", error);

	/* Describes the objects over the storage and publishes them. */
	i915_vkcs_objects_init(x);
	error = i915_vkcs_objects_publish(x);
	if (error != 0)
		return error;

	/* Compiles the mixed step's graphics pipeline directly. */
	error = drv_i915_gfx_pipeline_prepare(x->render, &x->graphics);
	if (error != 0) {
		kern_logf("i915: vkcs: the mixed step's graphics pipeline was not compiled: %d\n", error);
	} else {
		x->graphics_ready = 1;
	}

	/* Creates the compute pipelines through the wire; a step whose pipeline was refused fails alone. */
	i915_vkcs_pipelines_create(x);

	/* Points every set's bindings at their buffers through the wire. */
	error = i915_vkcs_sets_update(x);
	if (error != 0)
		return error;

	/* Creates the command pool and its one command buffer through the wire. */
	error = i915_vkcs_pool_create(x);
	if (error != 0)
		return error;

	/* Succeeded: every step can record and submit. */
	return 0;
}

/* Gives back everything the setup made, whatever it got to. */
static void
i915_vkcs_teardown(
	struct i915_vkcs *x)
{
	struct i915_device *device;
	int error;

	/* Destroys the command pool and its buffer through the wire. */
	if (x->pooled != 0) {
		x->used = 0U;
		x->overflow = 0;
		i915_vkcs_put32(x, I915_VKCS_OP_DESTROY_COMMAND_POOL);
		i915_vkcs_put32(x, 1U);
		i915_vkcs_put64(x, 0U);
		i915_vkcs_put64(x, I915_VKCS_ID_POOL);
		i915_vkcs_put64(x, 0U);
		error = i915_vkcs_execute(x, "destroy the command pool");
		if (error != 0)
			kern_logf("i915: vkcs: the command pool was not destroyed: %d\n", error);
		x->pooled = 0;
	}

	/* Destroys the compute pipelines through the wire. */
	if (x->render != NULL)
		i915_vkcs_pipelines_destroy(x);

	/* Withdraws the published objects and releases the graphics pipeline's kernels. */
	if (x->published != 0)
		i915_vkcs_objects_withdraw(x);
	if (x->graphics_ready != 0) {
		drv_i915_gfx_pipeline_release(&x->graphics);
		x->graphics_ready = 0;
	}

	/* Unbinds and destroys the MANY step's objects, then the storage. */
	device = x->device;
	i915_vkcs_many_destroy(x);
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
i915_vkcs_storage_create(
	struct i915_vkcs *x)
{
	struct i915_device *device;
	int error;

	/* Creates the object and binds it, destroying it again when the binding fails. */
	device = x->device;
	mutex_lock(&device->mutex);

	error = drv_i915_gem_create(&device->gem, I915_VKCS_STORAGE_BYTES, &x->storage);
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

/*
 * Makes the MANY step's three objects of 16 MB, bound into the session's
 * address space (each is contiguous memory, as a Vulkan allocation's
 * object is); on a failure the ones made so far are destroyed.
 */
static int
i915_vkcs_many_create(
	struct i915_vkcs *x)
{
	struct i915_device *device;
	uint32_t index;
	int error;

	/* Each object, created and bound under the device's lock. */
	device = x->device;
	error = 0;
	mutex_lock(&device->mutex);
	for (index = 0U; index < I915_VKCS_MANY_OBJECTS; index++) {
		error = drv_i915_gem_create(&device->gem, I915_VKCS_MANY_BYTES, &x->many_objects[index]);
		if (error != 0) {
			x->many_objects[index] = NULL;
			break;
		}

		/* Bound, or destroyed again. */
		error = drv_i915_gem_bind_vm(x->session->vm, x->many_objects[index]);
		if (error != 0) {
			drv_i915_gem_destroy(&device->gem, x->many_objects[index]);
			x->many_objects[index] = NULL;
			break;
		}

		/* The step writes and reads it through its CPU view. */
		x->many_cpu[index] = x->many_objects[index]->address;
	}

	/* The objects are made, or the first failure stopped the loop. */
	mutex_unlock(&device->mutex);

	/* A failure gives back what was made. */
	if (error != 0) {
		i915_vkcs_many_destroy(x);
		return error;
	}

	/* Succeeded: the step can run. */
	x->many_ready = 1;
	return 0;
}

/* Unbinds and destroys the MANY step's objects that were made. */
static void
i915_vkcs_many_destroy(
	struct i915_vkcs *x)
{
	struct i915_device *device;
	uint32_t index;

	/* Each object that exists, under the device's lock. */
	device = x->device;
	mutex_lock(&device->mutex);
	for (index = 0U; index < I915_VKCS_MANY_OBJECTS; index++) {
		if (x->many_objects[index] == NULL)
			continue;
		drv_i915_gem_unbind_vm(x->many_objects[index]);
		drv_i915_gem_destroy(&device->gem, x->many_objects[index]);
		x->many_objects[index] = NULL;
		x->many_cpu[index] = NULL;
	}

	/* The lock goes back. */
	mutex_unlock(&device->mutex);

	/* Nothing is left for the step. */
	x->many_ready = 0;
}

/*
 * Describes every object the steps use over the storage: the target, the
 * passes and the framebuffer, the buffers, the modules, the graphics
 * pipeline and the layouts.
 */
static void
i915_vkcs_objects_init(
	struct i915_vkcs *x)
{
	static const struct {
		const uint32_t *words;
		uint32_t bytes;
	} modules[I915_VKCS_KERNELS] = {
		{ i915_vkcs_one_comp, sizeof(i915_vkcs_one_comp) },
		{ i915_vkcs_add_comp, sizeof(i915_vkcs_add_comp) },
		{ i915_vkcs_ids_comp, sizeof(i915_vkcs_ids_comp) },
		{ i915_vkcs_odd_comp, sizeof(i915_vkcs_odd_comp) },
		{ i915_vkcs_push_comp, sizeof(i915_vkcs_push_comp) },
		{ i915_vkcs_atomic_comp, sizeof(i915_vkcs_atomic_comp) },
		{ i915_vkcs_mixed_comp, sizeof(i915_vkcs_mixed_comp) },
		{ i915_vkcs_inc_comp, sizeof(i915_vkcs_inc_comp) },
		{ i915_vkcs_spill_comp, sizeof(i915_vkcs_spill_comp) },
		{ i915_vkcs_shared_comp, sizeof(i915_vkcs_shared_comp) },
		{ i915_vkcs_reduce_comp, sizeof(i915_vkcs_reduce_comp) },
		{ i915_vkcs_scan_comp, sizeof(i915_vkcs_scan_comp) },
		{ i915_vkcs_oddbar_comp, sizeof(i915_vkcs_oddbar_comp) },
		{ i915_vkcs_atomsh_comp, sizeof(i915_vkcs_atomsh_comp) },
		{ i915_vkcs_refuse_big_comp, sizeof(i915_vkcs_refuse_big_comp) },
		{ i915_vkcs_refuse_retbar_comp, sizeof(i915_vkcs_refuse_retbar_comp) },
		{ i915_vkcs_refuse_loopbar_comp, sizeof(i915_vkcs_refuse_loopbar_comp) },
		{ i915_vkcs_grid_comp, sizeof(i915_vkcs_grid_comp) },
		{ i915_vkcs_length_comp, sizeof(i915_vkcs_length_comp) },
	};
	struct i915_gfx_pipeline *pipeline;
	struct i915_gfx_buffer *buffer;
	uint32_t index;

	/* The memory is the whole storage. */
	x->memory.vk = x->device->vk;
	x->memory.identity = I915_VKCS_IDENTITY;
	x->memory.size = I915_VKCS_STORAGE_BYTES;
	x->memory.object = x->storage;

	/* The target: a linear 64x64 RGBA8 image at the storage's start, drawn into and copied from. */
	x->target.format = VK_FORMAT_R8G8B8A8_UNORM;
	x->target.width = I915_VKCS_SIZE;
	x->target.height = I915_VKCS_SIZE;
	x->target.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
	x->target.pitch = I915_VKCS_SIZE * 4U;
	x->target.bytes = I915_VKCS_PIXELS * 4U;
	x->target.levels = 1U;
	x->target.memory = &x->memory;
	x->target.offset = 0U;
	x->view.image = &x->target;
	x->view.format = VK_FORMAT_R8G8B8A8_UNORM;
	x->view.level_count = 1U;

	/* Two passes of one subpass writing the one colour attachment: one clears it, the other keeps it. */
	x->clear_pass.attachment_count = 1U;
	x->clear_pass.attachments[0].format = VK_FORMAT_R8G8B8A8_UNORM;
	x->clear_pass.attachments[0].load_op = VK_ATTACHMENT_LOAD_OP_CLEAR;
	x->clear_pass.color_attachment = 0U;
	x->clear_pass.depth_attachment = VK_ATTACHMENT_UNUSED;
	x->load_pass = x->clear_pass;
	x->load_pass.attachments[0].load_op = VK_ATTACHMENT_LOAD_OP_LOAD;
	x->framebuffer.width = I915_VKCS_SIZE;
	x->framebuffer.height = I915_VKCS_SIZE;
	x->framebuffer.view_count = 1U;
	x->framebuffer.views[0] = &x->view;

	/* The buffers: one 64 KiB region each after the target's, usable every way the steps use them. */
	for (index = 0U; index < I915_VKCS_BUFFERS; index++) {
		buffer = &x->buffers[index];
		buffer->size = I915_VKCS_REGION_BYTES;
		buffer->usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
				VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT |
				VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
				VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT |
				VK_BUFFER_USAGE_TRANSFER_DST_BIT |
				VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
		buffer->memory = &x->memory;
		buffer->offset = (uint64_t)(index + 1U) * I915_VKCS_REGION_BYTES;
	}

	/* The MANY step's buffers: each the whole of its own object (unbound when the objects could not be made). */
	for (index = 0U; index < I915_VKCS_MANY_OBJECTS; index++) {
		x->many_memory[index].vk = x->device->vk;
		x->many_memory[index].identity = I915_VKCS_IDENTITY + 0x500U + index;
		x->many_memory[index].size = I915_VKCS_MANY_BYTES;
		x->many_memory[index].object = x->many_objects[index];
		buffer = &x->buffers[I915_VKCS_BUF_MANY_A + index];
		buffer->size = I915_VKCS_MANY_BYTES;
		buffer->memory = NULL;
		buffer->offset = 0U;
		if (x->many_ready != 0)
			buffer->memory = &x->many_memory[index];
	}

	/* The modules borrow the generated words, which the compiler only reads. */
	for (index = 0U; index < I915_VKCS_KERNELS; index++) {
		x->modules[index].words = (uint32_t *)(uintptr_t)modules[index].words;
		x->modules[index].word_count = modules[index].bytes / 4U;
	}
	x->mixed_vert.words = (uint32_t *)(uintptr_t)i915_vkcs_mixed_vert;
	x->mixed_vert.word_count = sizeof(i915_vkcs_mixed_vert) / 4U;
	x->mixed_frag.words = (uint32_t *)(uintptr_t)i915_vkcs_mixed_frag;
	x->mixed_frag.word_count = sizeof(i915_vkcs_mixed_frag) / 4U;

	/*
	 * The mixed step's graphics pipeline: a position and a colour, two
	 * vec4 attributes of 32-byte vertices; a triangle list, no culling, no
	 * depth; the whole target.
	 */
	pipeline = &x->graphics;
	pipeline->vertex = &x->mixed_vert;
	pipeline->fragment = &x->mixed_frag;
	pipeline->binding_count = 1U;
	pipeline->bindings[0].binding = 0U;
	pipeline->bindings[0].stride = 32U;
	pipeline->attribute_count = 2U;
	for (index = 0U; index < 2U; index++) {
		pipeline->attributes[index].location = index;
		pipeline->attributes[index].binding = 0U;
		pipeline->attributes[index].format = VK_FORMAT_R32G32B32A32_SFLOAT;
		pipeline->attributes[index].offset = 16U * index;
	}
	pipeline->topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
	pipeline->cull_mode = VK_CULL_MODE_NONE;
	pipeline->viewport[0] = I915_VKCS_F_0;
	pipeline->viewport[1] = I915_VKCS_F_0;
	pipeline->viewport[2] = I915_VKCS_F_64;
	pipeline->viewport[3] = I915_VKCS_F_64;
	pipeline->viewport[4] = I915_VKCS_F_0;
	pipeline->viewport[5] = I915_VKCS_F_1;
	pipeline->scissor.extent.width = I915_VKCS_SIZE;
	pipeline->scissor.extent.height = I915_VKCS_SIZE;

	/* The steps' layouts. */
	i915_vkcs_layouts_init(x);
}

/* Describes each step's layout from the table of its bindings. */
static void
i915_vkcs_layouts_init(
	struct i915_vkcs *x)
{
	uint32_t set;

	/* One layout a step. */
	for (set = 0U; set < I915_VKCS_SETS; set++)
		i915_vkcs_layout_init(x, set, i915_vkcs_bindings[set], i915_vkcs_binding_counts[set]);
}

/* Describes one step's layout: binding k of the given type for the compute stage. */
static void
i915_vkcs_layout_init(
	struct i915_vkcs *x,
	uint32_t set,
	const struct i915_vkcs_binding *bindings,
	uint32_t count)
{
	struct i915_gfx_dsl *layout;
	uint32_t index;

	/* The bindings in order; the set starts empty and vkUpdateDescriptorSets fills it. */
	layout = &x->layouts[set];
	layout->count = count;
	for (index = 0U; index < count; index++) {
		layout->bindings[index].binding = index;
		layout->bindings[index].type = bindings[index].type;
		layout->bindings[index].stages = VK_SHADER_STAGE_COMPUTE_BIT;
	}
	x->sets[set].layout = layout;
}

/* Publishes the objects the wire names under the scenario's identities. */
static int
i915_vkcs_objects_publish(
	struct i915_vkcs *x)
{
	struct i915_render_session *session;
	uint32_t index;
	int error;

	/* Publishes the target, the passes and the framebuffer, stopping at the first refusal. */
	session = x->render;
	error = drv_i915_object_insert(session, I915_VK_OBJ_IMAGE, I915_VKCS_ID_TARGET, &x->target);
	if (error == 0)
		error = drv_i915_object_insert(session, I915_VK_OBJ_RENDER_PASS, I915_VKCS_ID_CLEAR_PASS, &x->clear_pass);
	if (error == 0)
		error = drv_i915_object_insert(session, I915_VK_OBJ_RENDER_PASS, I915_VKCS_ID_LOAD_PASS, &x->load_pass);
	if (error == 0)
		error = drv_i915_object_insert(session, I915_VK_OBJ_FRAMEBUFFER, I915_VKCS_ID_FRAMEBUFFER, &x->framebuffer);
	if (error == 0)
		error = drv_i915_object_insert(session, I915_VK_OBJ_PIPELINE, I915_VKCS_ID_GRAPHICS, &x->graphics);

	/* Publishes the buffers, the modules and the sets, one identity each. */
	for (index = 0U; error == 0 && index < I915_VKCS_BUFFERS; index++)
		error = drv_i915_object_insert(session, I915_VK_OBJ_BUFFER, I915_VKCS_ID_BUFFER + index, &x->buffers[index]);
	for (index = 0U; error == 0 && index < I915_VKCS_KERNELS; index++)
		error = drv_i915_object_insert(session, I915_VK_OBJ_SHADER_MODULE, I915_VKCS_ID_MODULE + index, &x->modules[index]);
	for (index = 0U; error == 0 && index < I915_VKCS_SETS; index++)
		error = drv_i915_object_insert(session, I915_VK_OBJ_DESCRIPTOR_SET, I915_VKCS_ID_SET + index, &x->sets[index]);

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
i915_vkcs_objects_withdraw(
	struct i915_vkcs *x)
{
	struct i915_render_session *session;
	uint32_t index;

	/* Removes each identity from the table. */
	session = x->render;
	drv_i915_object_remove(session, I915_VK_OBJ_IMAGE, I915_VKCS_ID_TARGET);
	drv_i915_object_remove(session, I915_VK_OBJ_RENDER_PASS, I915_VKCS_ID_CLEAR_PASS);
	drv_i915_object_remove(session, I915_VK_OBJ_RENDER_PASS, I915_VKCS_ID_LOAD_PASS);
	drv_i915_object_remove(session, I915_VK_OBJ_FRAMEBUFFER, I915_VKCS_ID_FRAMEBUFFER);
	drv_i915_object_remove(session, I915_VK_OBJ_PIPELINE, I915_VKCS_ID_GRAPHICS);
	for (index = 0U; index < I915_VKCS_BUFFERS; index++)
		drv_i915_object_remove(session, I915_VK_OBJ_BUFFER, I915_VKCS_ID_BUFFER + index);
	for (index = 0U; index < I915_VKCS_KERNELS; index++)
		drv_i915_object_remove(session, I915_VK_OBJ_SHADER_MODULE, I915_VKCS_ID_MODULE + index);
	for (index = 0U; index < I915_VKCS_SETS; index++)
		drv_i915_object_remove(session, I915_VK_OBJ_DESCRIPTOR_SET, I915_VKCS_ID_SET + index);
	x->published = 0;
}

/*
 * Creates each compute pipeline with vkCreateComputePipelines, one a
 * stream: [66][reply][device][cache][1][1]{[sType 29][no chain][flags]
 * [sType 18][no chain][flags][compute][module][5]["main"][no
 * specialization][layout][base][index]}[no allocator][1][identity].
 */
static void
i915_vkcs_pipelines_create(
	struct i915_vkcs *x)
{
	const struct i915_gfx_pipeline *pipeline;
	uint32_t result;
	uint32_t index;
	int error;

	/* Each kernel in turn; a refused one leaves its step to fail. */
	for (index = 0U; index < I915_VKCS_KERNELS; index++) {
		x->used = 0U;
		x->overflow = 0;
		i915_vkcs_put32(x, I915_VKCS_OP_CREATE_COMPUTE_PIPELINES);
		i915_vkcs_put32(x, 1U);
		i915_vkcs_put64(x, 0U);
		i915_vkcs_put64(x, 0U);
		i915_vkcs_put32(x, 1U);
		i915_vkcs_put64(x, 1U);

		/* VkComputePipelineCreateInfo and its stage: the compute stage of the module, entry "main". */
		i915_vkcs_put32(x, 29U);
		i915_vkcs_put64(x, 0U);
		i915_vkcs_put32(x, 0U);
		i915_vkcs_put32(x, 18U);
		i915_vkcs_put64(x, 0U);
		i915_vkcs_put32(x, 0U);
		i915_vkcs_put32(x, VK_SHADER_STAGE_COMPUTE_BIT);
		i915_vkcs_put64(x, I915_VKCS_ID_MODULE + index);
		i915_vkcs_put64(x, 5U);
		i915_vkcs_put32(x, 0x6e69616dU);
		i915_vkcs_put32(x, 0U);

		/* No specialization, no layout, no base pipeline; no allocator, then one identity. */
		i915_vkcs_put64(x, 0U);
		i915_vkcs_put64(x, 0U);
		i915_vkcs_put64(x, 0U);
		i915_vkcs_put32(x, 0U);
		i915_vkcs_put64(x, 0U);
		i915_vkcs_put64(x, 1U);
		i915_vkcs_put64(x, I915_VKCS_ID_PIPELINE + index);
		error = i915_vkcs_execute(x, "vkCreateComputePipelines");
		result = i915_vkcs_reply32(x, 4U);
		if (error != 0 || result != VK_SUCCESS) {
			kern_logf("i915: vkcs: compute pipeline %u was not made: error %d, result %d\n", index, error, (int)result);
			continue;
		}

		/* Says how the kernel came out. */
		x->made[index] = 1;
		pipeline = drv_i915_object_lookup(x->render, I915_VK_OBJ_PIPELINE, I915_VKCS_ID_PIPELINE + index);
		if (pipeline != NULL && pipeline->cs_binary != NULL) {
			kern_logf("i915: vkcs: compute pipeline %u: %u bytes, r%u, %u threads, right mask 0x%x, cross-thread %u, scratch %u\n",
				  index,
				  pipeline->cs_binary->code_bytes,
				  pipeline->cs_binary->grf_used,
				  pipeline->threads,
				  pipeline->right_mask,
				  pipeline->cs_binary->cross_thread_regs,
				  pipeline->cs_binary->scratch_bytes);
		}
	}
}

/* Destroys every compute pipeline the executor made: [67][reply][device][pipeline][no allocator]. */
static void
i915_vkcs_pipelines_destroy(
	struct i915_vkcs *x)
{
	uint32_t index;
	int error;

	/* Each pipeline that was made. */
	for (index = 0U; index < I915_VKCS_KERNELS; index++) {
		if (x->made[index] == 0)
			continue;

		x->used = 0U;
		x->overflow = 0;
		i915_vkcs_put32(x, I915_VKCS_OP_DESTROY_PIPELINE);
		i915_vkcs_put32(x, 1U);
		i915_vkcs_put64(x, 0U);
		i915_vkcs_put64(x, I915_VKCS_ID_PIPELINE + index);
		i915_vkcs_put64(x, 0U);
		error = i915_vkcs_execute(x, "vkDestroyPipeline");
		if (error != 0)
			kern_logf("i915: vkcs: compute pipeline %u was not destroyed: %d\n", index, error);
		x->made[index] = 0;
	}
}

/*
 * Points each binding of every set at its buffer from the buffer's start
 * (i915_vkcs_bindings), one vkUpdateDescriptorSets a binding: [79][reply][device][1][1]
 * {[sType 35][no chain][set][binding][0][1][type][0][1]{buffer offset
 * range}[0]}[0][0].
 */
static int
i915_vkcs_sets_update(
	struct i915_vkcs *x)
{
	const struct i915_gfx_dsl *layout;
	uint32_t set;
	uint32_t binding;
	int error;

	/* Every binding of every set (the MANY step's only when its objects were made). */
	for (set = 0U; set < I915_VKCS_SETS; set++) {
		if (set == I915_VKCS_SET_MANY && x->many_ready == 0)
			continue;
		layout = &x->layouts[set];
		for (binding = 0U; binding < layout->count; binding++) {
			/* The write's head: the set, the binding, element 0, one descriptor of the layout's type. */
			x->used = 0U;
			x->overflow = 0;
			i915_vkcs_put32(x, I915_VKCS_OP_UPDATE_DESCRIPTOR_SETS);
			i915_vkcs_put32(x, 1U);
			i915_vkcs_put64(x, 0U);
			i915_vkcs_put32(x, 1U);
			i915_vkcs_put64(x, 1U);
			i915_vkcs_put32(x, 35U);
			i915_vkcs_put64(x, 0U);
			i915_vkcs_put64(x, I915_VKCS_ID_SET + set);
			i915_vkcs_put32(x, binding);
			i915_vkcs_put32(x, 0U);
			i915_vkcs_put32(x, 1U);
			i915_vkcs_put32(x, layout->bindings[binding].type);

			/* No images, the one buffer from its start, no texel views; no copies. */
			i915_vkcs_put64(x, 0U);
			i915_vkcs_put64(x, 1U);
			i915_vkcs_put64(x, I915_VKCS_ID_BUFFER + i915_vkcs_bindings[set][binding].buffer);
			i915_vkcs_put64(x, 0U);
			i915_vkcs_put64(x, i915_vkcs_bindings[set][binding].range);
			i915_vkcs_put64(x, 0U);
			i915_vkcs_put32(x, 0U);
			i915_vkcs_put64(x, 0U);
			error = i915_vkcs_execute(x, "vkUpdateDescriptorSets");
			if (error != 0)
				return error;
		}
	}

	/* Succeeded: every binding names its buffer. */
	return 0;
}

/* Returns the CPU view of one buffer's words. */
static uint32_t *
i915_vkcs_words(
	struct i915_vkcs *x,
	uint32_t buffer)
{
	/* The buffer's region of the storage. */
	return (uint32_t *)(void *)(x->cpu + x->buffers[buffer].offset);
}

/* Writes one buffer's lines back and drops them, before the GPU reads it or after it wrote it. */
static void
i915_vkcs_flush(
	struct i915_vkcs *x,
	uint32_t buffer)
{
	/* The whole region. */
	drv_i915_gt_clflush(i915_vkcs_words(x, buffer), I915_VKCS_REGION_BYTES);
}

/* Fills one buffer with a word and makes it visible to the GPU. */
static void
i915_vkcs_fill(
	struct i915_vkcs *x,
	uint32_t buffer,
	uint32_t value)
{
	uint32_t *words;
	uint32_t index;

	/* Every word of the region. */
	words = i915_vkcs_words(x, buffer);
	for (index = 0U; index < I915_VKCS_REGION_WORDS; index++)
		words[index] = value;
	i915_vkcs_flush(x, buffer);
}

/* Appends one little-endian word to the stream; a stream that would overflow is marked. */
static void
i915_vkcs_put32(
	struct i915_vkcs *x,
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
i915_vkcs_put64(
	struct i915_vkcs *x,
	uint64_t value)
{
	/* The low word first. */
	i915_vkcs_put32(x, (uint32_t)value);
	i915_vkcs_put32(x, (uint32_t)(value >> 32));
}

/* Appends the head of a recording: [opcode][no reply][command buffer]. */
static void
i915_vkcs_record(
	struct i915_vkcs *x,
	uint32_t opcode)
{
	/* A recording asks for no reply. */
	i915_vkcs_put32(x, opcode);
	i915_vkcs_put32(x, 0U);
	i915_vkcs_put64(x, I915_VKCS_ID_COMMAND_BUFFER);
}

/* Executes the stream built so far and empties it; the replies stay in the reply area. */
static int
i915_vkcs_execute(
	struct i915_vkcs *x,
	const char *what)
{
	int error;

	/* Refuses a stream that did not fit. */
	if (x->overflow != 0) {
		kern_logf("i915: vkcs: %s: the stream does not fit %u bytes\n", what, I915_VKCS_WIRE_BYTES);
		x->used = 0U;
		return ENOSPC;
	}

	/* Executes it into the reply area. */
	x->reply_bytes = sizeof(x->reply);
	error = drv_i915_render_execute(x->render, x->wire, x->used, x->reply, &x->reply_bytes);
	x->used = 0U;
	if (error != 0) {
		kern_logf("i915: vkcs: %s: the executor refused the stream: %d\n", what, error);
		return error;
	}

	/* Succeeded: the replies are in the reply area. */
	return 0;
}

/* Reads one reply word; zero past the replies. */
static uint32_t
i915_vkcs_reply32(
	const struct i915_vkcs *x,
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
i915_vkcs_pool_create(
	struct i915_vkcs *x)
{
	uint32_t result;
	int error;

	/* vkCreateCommandPool: [85][reply][device][present][sType 39][no chain][flags][family][no allocator][present][identity]. */
	x->used = 0U;
	x->overflow = 0;
	i915_vkcs_put32(x, I915_VKCS_OP_CREATE_COMMAND_POOL);
	i915_vkcs_put32(x, 1U);
	i915_vkcs_put64(x, 0U);
	i915_vkcs_put64(x, 1U);
	i915_vkcs_put32(x, 39U);
	i915_vkcs_put64(x, 0U);
	i915_vkcs_put32(x, 0U);
	i915_vkcs_put32(x, 0U);
	i915_vkcs_put64(x, 0U);
	i915_vkcs_put64(x, 1U);
	i915_vkcs_put64(x, I915_VKCS_ID_POOL);

	/* vkAllocateCommandBuffers: [88][reply][device][present][sType 40][no chain][pool][primary][1][1][identity]. */
	i915_vkcs_put32(x, I915_VKCS_OP_ALLOCATE_COMMAND_BUFFERS);
	i915_vkcs_put32(x, 1U);
	i915_vkcs_put64(x, 0U);
	i915_vkcs_put64(x, 1U);
	i915_vkcs_put32(x, 40U);
	i915_vkcs_put64(x, 0U);
	i915_vkcs_put64(x, I915_VKCS_ID_POOL);
	i915_vkcs_put32(x, VK_COMMAND_BUFFER_LEVEL_PRIMARY);
	i915_vkcs_put32(x, 1U);
	i915_vkcs_put64(x, 1U);
	i915_vkcs_put64(x, I915_VKCS_ID_COMMAND_BUFFER);
	error = i915_vkcs_execute(x, "create the command pool");
	if (error != 0)
		return error;

	/* The pool exists from here on, whatever the allocation answered. */
	x->pooled = 1;

	/* Refuses a pool the executor did not make: [85][result][present][identity]. */
	result = i915_vkcs_reply32(x, 4U);
	if (result != VK_SUCCESS)
		return EIO;

	/* Refuses a buffer it did not allocate: [88][result][count][identity] behind the pool's reply. */
	result = i915_vkcs_reply32(x, 24U + 4U);
	if (result != VK_SUCCESS)
		return EIO;

	/* Succeeded: the command buffer can record. */
	return 0;
}

/* Starts a stream with vkBeginCommandBuffer, which empties the recording: [90][reply][buffer][present][sType 42][no chain][flags][no inheritance]. */
static void
i915_vkcs_begin(
	struct i915_vkcs *x)
{
	/* The begin asks for its reply. */
	x->used = 0U;
	x->overflow = 0;
	i915_vkcs_put32(x, I915_VKCS_OP_BEGIN_COMMAND_BUFFER);
	i915_vkcs_put32(x, 1U);
	i915_vkcs_put64(x, I915_VKCS_ID_COMMAND_BUFFER);
	i915_vkcs_put64(x, 1U);
	i915_vkcs_put32(x, 42U);
	i915_vkcs_put64(x, 0U);
	i915_vkcs_put32(x, 0U);
	i915_vkcs_put64(x, 0U);
}

/*
 * Appends the binds of a compute pipeline and of a step's set as set 0 at
 * the compute bind point, with one dynamic offset when `dynamic` is set:
 * [bind point][pipeline], then [bind point][layout][0][1][1]{set}[count]
 * [count]{offsets}.
 */
static void
i915_vkcs_bind_compute(
	struct i915_vkcs *x,
	uint32_t kernel,
	uint32_t set,
	int dynamic,
	uint32_t offset)
{
	/* vkCmdBindPipeline at the compute bind point. */
	i915_vkcs_record(x, I915_VKCS_OP_BIND_PIPELINE);
	i915_vkcs_put32(x, VK_PIPELINE_BIND_POINT_COMPUTE);
	i915_vkcs_put64(x, I915_VKCS_ID_PIPELINE + kernel);

	/* vkCmdBindDescriptorSets of the one set at the compute bind point. */
	i915_vkcs_record(x, I915_VKCS_OP_BIND_DESCRIPTOR_SETS);
	i915_vkcs_put32(x, VK_PIPELINE_BIND_POINT_COMPUTE);
	i915_vkcs_put64(x, 0U);
	i915_vkcs_put32(x, 0U);
	i915_vkcs_put32(x, 1U);
	i915_vkcs_put64(x, 1U);
	i915_vkcs_put64(x, I915_VKCS_ID_SET + set);

	/* The dynamic offsets: none, or the one given. */
	if (dynamic != 0) {
		i915_vkcs_put32(x, 1U);
		i915_vkcs_put64(x, 1U);
		i915_vkcs_put32(x, offset);
	} else {
		i915_vkcs_put32(x, 0U);
		i915_vkcs_put64(x, 0U);
	}
}

/* Appends vkCmdPushConstants of `count` words to the compute stage from byte 0: [layout][stages][offset][size][size]{bytes}. */
static void
i915_vkcs_push(
	struct i915_vkcs *x,
	const uint32_t *words,
	uint32_t count)
{
	uint32_t index;

	/* No layout, the compute stage, from byte 0. */
	i915_vkcs_record(x, I915_VKCS_OP_PUSH_CONSTANTS);
	i915_vkcs_put64(x, 0U);
	i915_vkcs_put32(x, VK_SHADER_STAGE_COMPUTE_BIT);
	i915_vkcs_put32(x, 0U);
	i915_vkcs_put32(x, count * 4U);
	i915_vkcs_put64(x, count * 4U);
	for (index = 0U; index < count; index++)
		i915_vkcs_put32(x, words[index]);
}

/* Appends vkCmdDispatch: [x][y][z]. */
static void
i915_vkcs_dispatch(
	struct i915_vkcs *x,
	uint32_t gx,
	uint32_t gy,
	uint32_t gz)
{
	/* The groups along each axis. */
	i915_vkcs_record(x, I915_VKCS_OP_DISPATCH);
	i915_vkcs_put32(x, gx);
	i915_vkcs_put32(x, gy);
	i915_vkcs_put32(x, gz);
}

/* Appends vkCmdDispatchIndirect of the counts in one buffer at a byte offset: [buffer][offset]. */
static void
i915_vkcs_dispatch_indirect(
	struct i915_vkcs *x,
	uint32_t buffer,
	uint32_t offset)
{
	/* The buffer and the offset of its three counts. */
	i915_vkcs_record(x, I915_VKCS_OP_DISPATCH_INDIRECT);
	i915_vkcs_put64(x, I915_VKCS_ID_BUFFER + buffer);
	i915_vkcs_put64(x, offset);
}

/*
 * Appends vkCmdBeginRenderPass of the whole target with one of the passes,
 * the clear value zero: [present][sType 43][no chain][pass][framebuffer]
 * [area][1][1]{[colour][tag][4][r g b a]}[inline].
 */
static void
i915_vkcs_begin_pass(
	struct i915_vkcs *x,
	uint64_t pass)
{
	uint32_t index;

	/* The begin info. */
	i915_vkcs_record(x, I915_VKCS_OP_BEGIN_RENDER_PASS);
	i915_vkcs_put64(x, 1U);
	i915_vkcs_put32(x, 43U);
	i915_vkcs_put64(x, 0U);
	i915_vkcs_put64(x, pass);
	i915_vkcs_put64(x, I915_VKCS_ID_FRAMEBUFFER);

	/* The render area: the whole target. */
	i915_vkcs_put32(x, 0U);
	i915_vkcs_put32(x, 0U);
	i915_vkcs_put32(x, I915_VKCS_SIZE);
	i915_vkcs_put32(x, I915_VKCS_SIZE);

	/* One colour clear value, zero; the loading pass ignores it. */
	i915_vkcs_put32(x, 1U);
	i915_vkcs_put64(x, 1U);
	i915_vkcs_put32(x, 0U);
	i915_vkcs_put32(x, 0U);
	i915_vkcs_put64(x, 4U);
	for (index = 0U; index < 4U; index++)
		i915_vkcs_put32(x, I915_VKCS_F_0);

	/* The subpass contents are inline. */
	i915_vkcs_put32(x, VK_SUBPASS_CONTENTS_INLINE);
}

/* Appends the bind of the graphics pipeline and of a vertex buffer, and vkCmdDraw of its six vertices. */
static void
i915_vkcs_draw(
	struct i915_vkcs *x,
	uint32_t buffer)
{
	/* vkCmdBindPipeline at the graphics bind point. */
	i915_vkcs_record(x, I915_VKCS_OP_BIND_PIPELINE);
	i915_vkcs_put32(x, VK_PIPELINE_BIND_POINT_GRAPHICS);
	i915_vkcs_put64(x, I915_VKCS_ID_GRAPHICS);

	/* vkCmdBindVertexBuffers: [first][present][1]{buffer}[1]{offset}. */
	i915_vkcs_record(x, I915_VKCS_OP_BIND_VERTEX_BUFFERS);
	i915_vkcs_put32(x, 0U);
	i915_vkcs_put32(x, 1U);
	i915_vkcs_put64(x, 1U);
	i915_vkcs_put64(x, I915_VKCS_ID_BUFFER + buffer);
	i915_vkcs_put64(x, 1U);
	i915_vkcs_put64(x, 0U);

	/* vkCmdDraw: [6][1][0][0]. */
	i915_vkcs_record(x, I915_VKCS_OP_DRAW);
	i915_vkcs_put32(x, 6U);
	i915_vkcs_put32(x, 1U);
	i915_vkcs_put32(x, 0U);
	i915_vkcs_put32(x, 0U);
}

/*
 * Appends vkCmdCopyImageToBuffer of the whole target into a buffer from
 * its start, rows packed: [image][layout][buffer][1][1]{offset, row length,
 * image height, aspect, level, layer, layers, x y z, w h d}.
 */
static void
i915_vkcs_copy_target(
	struct i915_vkcs *x,
	uint32_t buffer)
{
	/* The image and the buffer. */
	i915_vkcs_record(x, I915_VKCS_OP_COPY_IMAGE_TO_BUFFER);
	i915_vkcs_put64(x, I915_VKCS_ID_TARGET);
	i915_vkcs_put32(x, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
	i915_vkcs_put64(x, I915_VKCS_ID_BUFFER + buffer);

	/* One region: the whole colour level 0. */
	i915_vkcs_put32(x, 1U);
	i915_vkcs_put64(x, 1U);
	i915_vkcs_put64(x, 0U);
	i915_vkcs_put32(x, 0U);
	i915_vkcs_put32(x, 0U);
	i915_vkcs_put32(x, VK_IMAGE_ASPECT_COLOR_BIT);
	i915_vkcs_put32(x, 0U);
	i915_vkcs_put32(x, 0U);
	i915_vkcs_put32(x, 1U);
	i915_vkcs_put32(x, 0U);
	i915_vkcs_put32(x, 0U);
	i915_vkcs_put32(x, 0U);
	i915_vkcs_put32(x, I915_VKCS_SIZE);
	i915_vkcs_put32(x, I915_VKCS_SIZE);
	i915_vkcs_put32(x, 1U);
}

/*
 * Appends the end and a submission of the command buffer, executes the
 * stream and checks the three replies: [90][result][91][result][18][result].
 */
static int
i915_vkcs_finish(
	struct i915_vkcs *x,
	const char *what)
{
	uint32_t begun;
	uint32_t ended;
	uint32_t submitted;
	int error;

	/* vkEndCommandBuffer: [91][reply][buffer]. */
	i915_vkcs_put32(x, I915_VKCS_OP_END_COMMAND_BUFFER);
	i915_vkcs_put32(x, 1U);
	i915_vkcs_put64(x, I915_VKCS_ID_COMMAND_BUFFER);

	/* vkQueueSubmit of the one buffer, with no semaphores and no fence. */
	i915_vkcs_put32(x, I915_VKCS_OP_QUEUE_SUBMIT);
	i915_vkcs_put32(x, 1U);
	i915_vkcs_put64(x, 0U);
	i915_vkcs_put32(x, 1U);
	i915_vkcs_put64(x, 1U);
	i915_vkcs_put32(x, 4U);
	i915_vkcs_put64(x, 0U);
	i915_vkcs_put32(x, 0U);
	i915_vkcs_put64(x, 0U);
	i915_vkcs_put64(x, 0U);
	i915_vkcs_put32(x, 1U);
	i915_vkcs_put64(x, 1U);
	i915_vkcs_put64(x, I915_VKCS_ID_COMMAND_BUFFER);
	i915_vkcs_put32(x, 0U);
	i915_vkcs_put64(x, 0U);
	i915_vkcs_put64(x, 0U);

	/* Runs the stream: the recording, then the submission to its end. */
	error = i915_vkcs_execute(x, what);
	if (error != 0)
		return error;

	/* Takes the results of the begin, the end and the submission. */
	begun = i915_vkcs_reply32(x, 4U);
	ended = i915_vkcs_reply32(x, 12U);
	submitted = i915_vkcs_reply32(x, 20U);

	/* Refuses a begin, an end or a submission that did not succeed. */
	if (begun != VK_SUCCESS ||
	    ended != VK_SUCCESS ||
	    submitted != VK_SUCCESS) {
		kern_logf("i915: vkcs: %s: begin %d, end %d, submit %d\n",
			  what,
			  (int)begun,
			  (int)ended,
			  (int)submitted);
		return EIO;
	}

	/* Succeeded: the command buffer ran to its end. */
	return 0;
}

/* Compares one word with the one expected; names the first eight that differ and counts them all. */
static void
i915_vkcs_check(
	struct i915_vkcs *x,
	const char *what,
	uint32_t index,
	uint32_t word,
	uint32_t expected)
{
	/* An equal word is right. */
	if (word == expected)
		return;

	/* Names the first few. */
	if (x->differ < 8U)
		kern_logf("i915: vkcs: %s: word %u: 0x%08x, expected 0x%08x\n", what, index, word, expected);
	x->differ++;
}

/* Decides whether two RGBA8 words agree within one in every byte. */
static int
i915_vkcs_bytes_near(
	uint32_t word,
	uint32_t expected)
{
	uint32_t shift;
	uint32_t a;
	uint32_t b;

	/* Each byte on its own. */
	for (shift = 0U; shift < 32U; shift += 8U) {
		a = (word >> shift) & 0xffU;
		b = (expected >> shift) & 0xffU;
		if (a > b + 1U || b > a + 1U)
			return 0;
	}

	/* Succeeded: every byte is within one. */
	return 1;
}

/* Ends a step's checks: EIO when a word differed, with the count logged. */
static int
i915_vkcs_checked(
	struct i915_vkcs *x,
	const char *what)
{
	unsigned differ;

	/* Starts the next step's count afresh. */
	differ = x->differ;
	x->differ = 0U;
	if (differ != 0U) {
		kern_logf("i915: vkcs: %s: %u words differ\n", what, differ);
		return EIO;
	}

	/* Succeeded: every word was right. */
	return 0;
}

/* Logs a step's verdict and counts it. */
static void
i915_vkcs_verdict(
	struct i915_vkcs *x,
	const char *what,
	int error)
{
	/* A step fails with the error that stopped it. */
	if (error != 0) {
		x->failed++;
		kern_logf("i915: vkcs: VKCS-%s FAIL (%d)\n", what, error);
		return;
	}

	x->passed++;
	kern_logf("i915: vkcs: VKCS-%s PASS\n", what);
}

/* Reports whether a step's compute pipeline was made; fails the step when not. */
static int
i915_vkcs_ready(
	struct i915_vkcs *x,
	uint32_t kernel,
	const char *what)
{
	/* A step without its pipeline fails without running. */
	if (x->made[kernel] == 0) {
		kern_logf("i915: vkcs: %s: its compute pipeline was not made\n", what);
		i915_vkcs_verdict(x, what, ENOENT);
		return 0;
	}

	/* Succeeded: the pipeline can be bound. */
	return 1;
}

/* A 32-bit integer hash (the inputs of the steps). */
static uint32_t
i915_vkcs_hash(
	uint32_t value)
{
	/* Multiplies and folds. */
	value *= 2654435761U;
	value ^= value >> 15;
	value *= 0x2c1b3c6dU;
	value ^= value >> 12;

	/* Succeeded: the hashed value. */
	return value;
}

/*
 * ONE: one group of one invocation writes 0x01234567 at word 0 of C; the
 * next words keep their sentinel.
 */
static void
i915_vkcs_step_one(
	struct i915_vkcs *x)
{
	const uint32_t *words;
	uint32_t index;
	int error;

	/* Needs its pipeline. */
	if (i915_vkcs_ready(x, I915_VKCS_KERNEL_ONE, "ONE") == 0)
		return;

	/* Records the pipeline, the set and one group; runs it. */
	i915_vkcs_fill(x, I915_VKCS_BUF_C, I915_VKCS_SENTINEL);
	i915_vkcs_begin(x);
	i915_vkcs_bind_compute(x, I915_VKCS_KERNEL_ONE, I915_VKCS_SET_ONE, 0, 0U);
	i915_vkcs_dispatch(x, 1U, 1U, 1U);
	error = i915_vkcs_finish(x, "ONE");
	if (error != 0) {
		i915_vkcs_verdict(x, "ONE", error);
		return;
	}

	/* The one word, and the sentinel after it. */
	i915_vkcs_flush(x, I915_VKCS_BUF_C);
	words = i915_vkcs_words(x, I915_VKCS_BUF_C);
	i915_vkcs_check(x, "ONE", 0U, words[0], 0x01234567U);
	for (index = 1U; index < 64U; index++)
		i915_vkcs_check(x, "ONE", index, words[index], I915_VKCS_SENTINEL);
	i915_vkcs_verdict(x, "ONE", i915_vkcs_checked(x, "ONE"));
}

/*
 * ADD: c[i] = a[i] + b[i] for i below 1000 over 16 groups of 64; the 24
 * invocations past n and the words past them write nothing.
 */
static void
i915_vkcs_step_add(
	struct i915_vkcs *x)
{
	uint32_t *a;
	uint32_t *b;
	const uint32_t *c;
	uint32_t n;
	uint32_t index;
	int error;

	/* Needs its pipeline. */
	if (i915_vkcs_ready(x, I915_VKCS_KERNEL_ADD, "ADD") == 0)
		return;

	/* The inputs, and C all sentinel. */
	a = i915_vkcs_words(x, I915_VKCS_BUF_A);
	b = i915_vkcs_words(x, I915_VKCS_BUF_B);
	for (index = 0U; index < I915_VKCS_REGION_WORDS; index++) {
		a[index] = index * 0x9e3779b9U;
		b[index] = index ^ 0x5a5a5a5aU;
	}
	i915_vkcs_flush(x, I915_VKCS_BUF_A);
	i915_vkcs_flush(x, I915_VKCS_BUF_B);
	i915_vkcs_fill(x, I915_VKCS_BUF_C, I915_VKCS_SENTINEL);

	/* Records n, the pipeline, the set and the groups; runs it. */
	n = I915_VKCS_ADD_N;
	i915_vkcs_begin(x);
	i915_vkcs_bind_compute(x, I915_VKCS_KERNEL_ADD, I915_VKCS_SET_ADD, 0, 0U);
	i915_vkcs_push(x, &n, 1U);
	i915_vkcs_dispatch(x, I915_VKCS_ADD_GROUPS, 1U, 1U);
	error = i915_vkcs_finish(x, "ADD");
	if (error != 0) {
		i915_vkcs_verdict(x, "ADD", error);
		return;
	}

	/* The sums below n, the sentinel from n on. */
	i915_vkcs_flush(x, I915_VKCS_BUF_C);
	c = i915_vkcs_words(x, I915_VKCS_BUF_C);
	for (index = 0U; index < I915_VKCS_ADD_GROUPS * 64U + 64U; index++) {
		if (index < n) {
			i915_vkcs_check(x, "ADD", index, c[index], a[index] + b[index]);
		} else {
			i915_vkcs_check(x, "ADD", index, c[index], I915_VKCS_SENTINEL);
		}
	}
	i915_vkcs_verdict(x, "ADD", i915_vkcs_checked(x, "ADD"));
}

/*
 * ID and ODD: a group of the kernel's local size over 3 x 2 x 2 groups;
 * each invocation writes its local ID, local index, group ID, group
 * counts and global ID at 13 * its global linear index, and counts itself.
 * The words past the last record keep their sentinel, so a channel past
 * the group's invocations that ran would show.  INDIRECT-ID (indirect set)
 * reads the groups from G, and gl_NumWorkGroups with them.
 */
static void
i915_vkcs_step_ids(
	struct i915_vkcs *x,
	const char *what,
	uint32_t kernel,
	const uint32_t local[3],
	int indirect)
{
	static const uint32_t groups[3] = { 3U, 2U, 2U };
	const uint32_t *words;
	uint32_t *count;
	uint32_t expected[I915_VKCS_RECORD_WORDS];
	uint32_t size[3];
	uint32_t g[3];
	uint32_t invocations;
	uint32_t linear;
	uint32_t word;
	int error;

	/* Needs its pipeline. */
	if (i915_vkcs_ready(x, kernel, what) == 0)
		return;

	/* The records all sentinel, the count zero. */
	i915_vkcs_fill(x, I915_VKCS_BUF_D, I915_VKCS_SENTINEL);
	count = i915_vkcs_words(x, I915_VKCS_BUF_B);
	count[0] = 0U;
	i915_vkcs_flush(x, I915_VKCS_BUF_B);

	/* Records the pipeline, the set and the groups (or G's counts); runs it. */
	i915_vkcs_begin(x);
	i915_vkcs_bind_compute(x, kernel, I915_VKCS_SET_IDS, 0, 0U);
	if (indirect != 0) {
		i915_vkcs_fill(x, I915_VKCS_BUF_G, 0U);
		i915_vkcs_grid(x, I915_VKCS_G_IDS, groups[0], groups[1], groups[2]);
		i915_vkcs_dispatch_indirect(x, I915_VKCS_BUF_G, I915_VKCS_G_IDS * 4U);
	} else {
		i915_vkcs_dispatch(x, groups[0], groups[1], groups[2]);
	}

	/* Runs it. */
	error = i915_vkcs_finish(x, what);
	if (error != 0) {
		i915_vkcs_verdict(x, what, error);
		return;
	}

	/* The grid's extent in invocations. */
	size[0] = local[0] * groups[0];
	size[1] = local[1] * groups[1];
	size[2] = local[2] * groups[2];
	invocations = size[0] * size[1] * size[2];

	/* Every invocation's record. */
	i915_vkcs_flush(x, I915_VKCS_BUF_D);
	i915_vkcs_flush(x, I915_VKCS_BUF_B);
	words = i915_vkcs_words(x, I915_VKCS_BUF_D);
	for (g[2] = 0U; g[2] < size[2]; g[2]++) {
		for (g[1] = 0U; g[1] < size[1]; g[1]++) {
			for (g[0] = 0U; g[0] < size[0]; g[0]++) {
				expected[0] = g[0] % local[0];
				expected[1] = g[1] % local[1];
				expected[2] = g[2] % local[2];
				expected[3] = expected[0] + local[0] * (expected[1] + local[1] * expected[2]);
				expected[4] = g[0] / local[0];
				expected[5] = g[1] / local[1];
				expected[6] = g[2] / local[2];
				expected[7] = groups[0];
				expected[8] = groups[1];
				expected[9] = groups[2];
				expected[10] = g[0];
				expected[11] = g[1];
				expected[12] = g[2];
				linear = g[0] + size[0] * (g[1] + size[1] * g[2]);
				for (word = 0U; word < I915_VKCS_RECORD_WORDS; word++) {
					i915_vkcs_check(x,
							what,
							linear * I915_VKCS_RECORD_WORDS + word,
							words[linear * I915_VKCS_RECORD_WORDS + word],
							expected[word]);
				}
			}
		}
	}

	/* Nothing past the last record, and every invocation counted once. */
	for (word = invocations * I915_VKCS_RECORD_WORDS; word < invocations * I915_VKCS_RECORD_WORDS + 256U; word++)
		i915_vkcs_check(x, what, word, words[word], I915_VKCS_SENTINEL);
	i915_vkcs_check(x, what, 0xffffffffU, count[0], invocations);
	i915_vkcs_verdict(x, what, i915_vkcs_checked(x, what));
}

/*
 * PUSH: c[i] = a[i] * k0 + b[i + 64] + u0.x + (u1.y ^ k1) + u2.z * i +
 * u2.w for i below 100, with k0, k1 and n pushed, u a uniform block and b
 * bound with a dynamic offset of 256 bytes; the rest untouched.
 */
static void
i915_vkcs_step_push(
	struct i915_vkcs *x)
{
	static const uint32_t uniforms[12] = {
		0x11111111U, 0U, 0U, 0U,
		0U, 0x0000ff00U, 0U, 0U,
		0U, 0U, 7U, 0x40000000U,
	};
	uint32_t constants[3];
	uint32_t *a;
	uint32_t *b;
	uint32_t *u;
	const uint32_t *c;
	uint32_t expected;
	uint32_t index;
	int error;

	/* Needs its pipeline. */
	if (i915_vkcs_ready(x, I915_VKCS_KERNEL_PUSH, "PUSH") == 0)
		return;

	/* The inputs: A and B hashed, the uniform block, C all sentinel. */
	a = i915_vkcs_words(x, I915_VKCS_BUF_A);
	b = i915_vkcs_words(x, I915_VKCS_BUF_B);
	u = i915_vkcs_words(x, I915_VKCS_BUF_U);
	for (index = 0U; index < I915_VKCS_REGION_WORDS; index++) {
		a[index] = i915_vkcs_hash(index);
		b[index] = i915_vkcs_hash(index + 0x10000U);
	}
	kern_memset(u, 0, 256U);
	kern_memcpy(u, uniforms, sizeof(uniforms));
	i915_vkcs_flush(x, I915_VKCS_BUF_A);
	i915_vkcs_flush(x, I915_VKCS_BUF_B);
	i915_vkcs_flush(x, I915_VKCS_BUF_U);
	i915_vkcs_fill(x, I915_VKCS_BUF_C, I915_VKCS_SENTINEL);

	/* Records k0, k1 and n, the pipeline, the set with its dynamic offset and the groups; runs it. */
	constants[0] = 0x01000193U;
	constants[1] = 0x00ff00ffU;
	constants[2] = I915_VKCS_PUSH_N;
	i915_vkcs_begin(x);
	i915_vkcs_bind_compute(x, I915_VKCS_KERNEL_PUSH, I915_VKCS_SET_PUSH, 1, I915_VKCS_PUSH_DYNAMIC);
	i915_vkcs_push(x, constants, 3U);
	i915_vkcs_dispatch(x, I915_VKCS_PUSH_GROUPS, 1U, 1U);
	error = i915_vkcs_finish(x, "PUSH");
	if (error != 0) {
		i915_vkcs_verdict(x, "PUSH", error);
		return;
	}

	/* The results below n, the sentinel from n on. */
	i915_vkcs_flush(x, I915_VKCS_BUF_C);
	c = i915_vkcs_words(x, I915_VKCS_BUF_C);
	for (index = 0U; index < I915_VKCS_PUSH_GROUPS * 32U + 32U; index++) {
		expected = I915_VKCS_SENTINEL;
		if (index < I915_VKCS_PUSH_N) {
			expected = a[index] * constants[0] +
				   b[index + I915_VKCS_PUSH_DYNAMIC / 4U] +
				   uniforms[0] +
				   (uniforms[5] ^ constants[1]) +
				   uniforms[10] * index +
				   uniforms[11];
		}
		i915_vkcs_check(x, "PUSH", index, c[index], expected);
	}
	i915_vkcs_verdict(x, "PUSH", i915_vkcs_checked(x, "PUSH"));
}

/*
 * ATOMIC-SSBO: 1000 invocations of atomic.comp over values below 5000.
 * Every result that does not depend on the order is compared: the
 * histogram, the sum, the extremes, and, or, xor, the exchange (7), the
 * compare-exchange (1), the counter (n), and the counter's old values, each
 * of 0 to n - 1 exactly once.
 */
static void
i915_vkcs_step_atomic(
	struct i915_vkcs *x)
{
	uint8_t seen[I915_VKCS_ATOMIC_N / 8U + 1U];
	uint32_t expected[I915_VKCS_H_WORDS];
	uint32_t *v;
	uint32_t *h;
	const uint32_t *old;
	uint32_t n;
	uint32_t value;
	uint32_t index;
	int32_t low;
	int32_t high;
	int32_t signed_value;
	int error;

	/* Needs its pipeline. */
	if (i915_vkcs_ready(x, I915_VKCS_KERNEL_ATOMIC, "ATOMIC-SSBO") == 0)
		return;

	/* The values, the block at the atomics' identities, the old values all sentinel. */
	v = i915_vkcs_words(x, I915_VKCS_BUF_A);
	for (index = 0U; index < I915_VKCS_REGION_WORDS; index++)
		v[index] = i915_vkcs_hash(index) % 5000U;
	i915_vkcs_flush(x, I915_VKCS_BUF_A);
	h = i915_vkcs_words(x, I915_VKCS_BUF_B);
	kern_memset(h, 0, I915_VKCS_H_WORDS * 4U);
	h[I915_VKCS_H_SMIN] = 0x7fffffffU;
	h[I915_VKCS_H_SMAX] = 0x80000000U;
	h[I915_VKCS_H_UMIN] = 0xffffffffU;
	h[I915_VKCS_H_AND] = 0xffffffffU;
	i915_vkcs_flush(x, I915_VKCS_BUF_B);
	i915_vkcs_fill(x, I915_VKCS_BUF_D, I915_VKCS_SENTINEL);

	/* Records n, the pipeline, the set and the groups; runs it. */
	n = I915_VKCS_ATOMIC_N;
	i915_vkcs_begin(x);
	i915_vkcs_bind_compute(x, I915_VKCS_KERNEL_ATOMIC, I915_VKCS_SET_ATOMIC, 0, 0U);
	i915_vkcs_push(x, &n, 1U);
	i915_vkcs_dispatch(x, I915_VKCS_ATOMIC_GROUPS, 1U, 1U);
	error = i915_vkcs_finish(x, "ATOMIC-SSBO");
	if (error != 0) {
		i915_vkcs_verdict(x, "ATOMIC-SSBO", error);
		return;
	}

	/* The block the atomics must leave. */
	kern_memset(expected, 0, sizeof(expected));
	low = 0x7fffffff;
	high = (int32_t)0x80000000U;
	expected[I915_VKCS_H_UMIN] = 0xffffffffU;
	expected[I915_VKCS_H_AND] = 0xffffffffU;
	for (index = 0U; index < n; index++) {
		value = v[index];
		signed_value = (int32_t)value - 1000;
		expected[I915_VKCS_H_BINS + value % 16U]++;
		expected[I915_VKCS_H_TOTAL] += value;
		if (signed_value < low)
			low = signed_value;
		if (signed_value > high)
			high = signed_value;
		if (value < expected[I915_VKCS_H_UMIN])
			expected[I915_VKCS_H_UMIN] = value;
		if (value > expected[I915_VKCS_H_UMAX])
			expected[I915_VKCS_H_UMAX] = value;
		expected[I915_VKCS_H_AND] &= value | 0xffff0000U;
		expected[I915_VKCS_H_OR] |= value;
		expected[I915_VKCS_H_XOR] ^= value;
	}
	expected[I915_VKCS_H_SMIN] = (uint32_t)low;
	expected[I915_VKCS_H_SMAX] = (uint32_t)high;
	expected[I915_VKCS_H_SWAP] = 7U;
	expected[I915_VKCS_H_CAS] = 1U;
	expected[I915_VKCS_H_COUNT] = n;

	/* Compares the block. */
	i915_vkcs_flush(x, I915_VKCS_BUF_B);
	i915_vkcs_flush(x, I915_VKCS_BUF_D);
	for (index = 0U; index < I915_VKCS_H_WORDS; index++)
		i915_vkcs_check(x, "ATOMIC-SSBO", index, h[index], expected[index]);

	/* Each old value of the counter once, the sentinel past n. */
	old = i915_vkcs_words(x, I915_VKCS_BUF_D);
	kern_memset(seen, 0, sizeof(seen));
	for (index = 0U; index < n; index++) {
		value = old[index];
		if (value >= n || (seen[value / 8U] & (1U << (value % 8U))) != 0U) {
			i915_vkcs_check(x, "ATOMIC-SSBO", 0x1000U + index, value, 0xffffffffU);
			continue;
		}
		seen[value / 8U] |= (uint8_t)(1U << (value % 8U));
	}
	for (index = n; index < I915_VKCS_ATOMIC_GROUPS * 64U + 64U; index++)
		i915_vkcs_check(x, "ATOMIC-SSBO", 0x1000U + index, old[index], I915_VKCS_SENTINEL);
	i915_vkcs_verdict(x, "ATOMIC-SSBO", i915_vkcs_checked(x, "ATOMIC-SSBO"));
}

/*
 * MIXED: in one command buffer, a draw fills the target with (51, 102,
 * 153, 255); a copy moves the target into P; a dispatch writes P + 1 into
 * Q and, from the first pixel, the left half's vertices in the inverse
 * colour into V; a draw of V over the kept target.  The target's left half
 * must hold the inverse, its right half the first colour.
 */
static void
i915_vkcs_step_mixed(
	struct i915_vkcs *x)
{
	static const uint32_t corner_x[6] = {
		I915_VKCS_F_MINUS_1, I915_VKCS_F_1, I915_VKCS_F_1, I915_VKCS_F_MINUS_1, I915_VKCS_F_1, I915_VKCS_F_MINUS_1,
	};
	static const uint32_t corner_y[6] = {
		I915_VKCS_F_MINUS_1, I915_VKCS_F_MINUS_1, I915_VKCS_F_1, I915_VKCS_F_MINUS_1, I915_VKCS_F_1, I915_VKCS_F_1,
	};
	const uint32_t colour = 51U | (102U << 8) | (153U << 16) | (255U << 24);
	const uint32_t *pixels;
	const uint32_t *copied;
	const uint32_t *plus;
	uint32_t *w;
	uint32_t inverse;
	uint32_t first;
	uint32_t index;
	int error;

	/* Needs its compute and graphics pipelines. */
	if (i915_vkcs_ready(x, I915_VKCS_KERNEL_MIXED, "MIXED") == 0)
		return;
	if (x->graphics_ready == 0) {
		i915_vkcs_verdict(x, "MIXED", ENOENT);
		return;
	}

	/* The first draw's vertices: the whole target in the first colour. */
	w = i915_vkcs_words(x, I915_VKCS_BUF_W);
	for (index = 0U; index < 6U; index++) {
		w[index * 8U] = corner_x[index];
		w[index * 8U + 1U] = corner_y[index];
		w[index * 8U + 2U] = I915_VKCS_F_0;
		w[index * 8U + 3U] = I915_VKCS_F_1;
		w[index * 8U + 4U] = I915_VKCS_F_0_2;
		w[index * 8U + 5U] = I915_VKCS_F_0_4;
		w[index * 8U + 6U] = I915_VKCS_F_0_6;
		w[index * 8U + 7U] = I915_VKCS_F_1;
	}
	i915_vkcs_flush(x, I915_VKCS_BUF_W);
	i915_vkcs_fill(x, I915_VKCS_BUF_P, I915_VKCS_SENTINEL);
	i915_vkcs_fill(x, I915_VKCS_BUF_Q, I915_VKCS_SENTINEL);
	i915_vkcs_fill(x, I915_VKCS_BUF_V, 0U);

	/* Records the draw, the copy, the dispatch and the second draw; runs them. */
	i915_vkcs_begin(x);
	i915_vkcs_begin_pass(x, I915_VKCS_ID_CLEAR_PASS);
	i915_vkcs_draw(x, I915_VKCS_BUF_W);
	i915_vkcs_record(x, I915_VKCS_OP_END_RENDER_PASS);
	i915_vkcs_copy_target(x, I915_VKCS_BUF_P);
	i915_vkcs_bind_compute(x, I915_VKCS_KERNEL_MIXED, I915_VKCS_SET_MIXED, 0, 0U);
	i915_vkcs_dispatch(x, I915_VKCS_PIXELS / 64U, 1U, 1U);
	i915_vkcs_begin_pass(x, I915_VKCS_ID_LOAD_PASS);
	i915_vkcs_draw(x, I915_VKCS_BUF_V);
	i915_vkcs_record(x, I915_VKCS_OP_END_RENDER_PASS);
	error = i915_vkcs_finish(x, "MIXED");
	if (error != 0) {
		i915_vkcs_verdict(x, "MIXED", error);
		return;
	}

	/* The copy: every pixel the first colour, within one. */
	i915_vkcs_flush(x, I915_VKCS_BUF_P);
	i915_vkcs_flush(x, I915_VKCS_BUF_Q);
	drv_i915_gt_clflush(x->cpu, I915_VKCS_PIXELS * 4U);
	copied = i915_vkcs_words(x, I915_VKCS_BUF_P);
	first = copied[0];
	for (index = 0U; index < I915_VKCS_PIXELS; index++) {
		if (i915_vkcs_bytes_near(copied[index], colour) == 0)
			i915_vkcs_check(x, "MIXED copy", index, copied[index], colour);
	}

	/* The dispatch: every copied word plus one. */
	plus = i915_vkcs_words(x, I915_VKCS_BUF_Q);
	for (index = 0U; index < I915_VKCS_PIXELS; index++)
		i915_vkcs_check(x, "MIXED plus", index, plus[index], copied[index] + 1U);

	/* The second draw: the left half the inverse of the first pixel, the right half as the first draw left it. */
	inverse = (~first & 0x00ffffffU) | 0xff000000U;
	pixels = (const uint32_t *)(const void *)x->cpu;
	for (index = 0U; index < I915_VKCS_PIXELS; index++) {
		if (index % I915_VKCS_SIZE < I915_VKCS_SIZE / 2U) {
			if (i915_vkcs_bytes_near(pixels[index], inverse) == 0)
				i915_vkcs_check(x, "MIXED target", index, pixels[index], inverse);
		} else {
			i915_vkcs_check(x, "MIXED target", index, pixels[index], first);
		}
	}
	i915_vkcs_verdict(x, "MIXED", i915_vkcs_checked(x, "MIXED"));
}

/*
 * MANYOPS: 200 dispatches in one submission, each o[i] = o[i] * 3 + k + i
 * over 256 words with its own k pushed before it; the slots of one batch
 * run out on the way, so the executor runs the batch in the middle.  The
 * words must be the recurrence applied 200 times in order.
 */
static void
i915_vkcs_step_many(
	struct i915_vkcs *x)
{
	uint32_t expected[I915_VKCS_MANY_WORDS];
	const uint32_t *o;
	uint32_t *words;
	uint32_t index;
	uint32_t step;
	uint32_t k;
	int error;

	/* Needs its pipeline. */
	if (i915_vkcs_ready(x, I915_VKCS_KERNEL_INC, "MANYOPS") == 0)
		return;

	/* The words start at their index. */
	i915_vkcs_fill(x, I915_VKCS_BUF_C, I915_VKCS_SENTINEL);
	words = i915_vkcs_words(x, I915_VKCS_BUF_C);
	for (index = 0U; index < I915_VKCS_MANY_WORDS; index++) {
		words[index] = index;
		expected[index] = index;
	}
	i915_vkcs_flush(x, I915_VKCS_BUF_C);

	/* Records the pipeline and the set, then a push and a dispatch for each step; runs them. */
	i915_vkcs_begin(x);
	i915_vkcs_bind_compute(x, I915_VKCS_KERNEL_INC, I915_VKCS_SET_INC, 0, 0U);
	for (step = 0U; step < I915_VKCS_MANY_DISPATCHES; step++) {
		k = step * 7U + 1U;
		i915_vkcs_push(x, &k, 1U);
		i915_vkcs_dispatch(x, I915_VKCS_MANY_WORDS / 64U, 1U, 1U);
		for (index = 0U; index < I915_VKCS_MANY_WORDS; index++)
			expected[index] = expected[index] * 3U + k + index;
	}
	error = i915_vkcs_finish(x, "MANYOPS");
	if (error != 0) {
		i915_vkcs_verdict(x, "MANYOPS", error);
		return;
	}

	/* The recurrence's words, and the sentinel after them. */
	i915_vkcs_flush(x, I915_VKCS_BUF_C);
	o = i915_vkcs_words(x, I915_VKCS_BUF_C);
	for (index = 0U; index < I915_VKCS_MANY_WORDS; index++)
		i915_vkcs_check(x, "MANYOPS", index, o[index], expected[index]);
	for (index = I915_VKCS_MANY_WORDS; index < I915_VKCS_MANY_WORDS + 64U; index++)
		i915_vkcs_check(x, "MANYOPS", index, o[index], I915_VKCS_SENTINEL);
	i915_vkcs_verdict(x, "MANYOPS", i915_vkcs_checked(x, "MANYOPS"));
}

/*
 * SPILL: spill.comp over 256 groups of 64 (2048 threads, more than the GPU
 * runs at once); the kernel must spill, and every word must be the sum
 * i915_vkcs_spill_word() computes.
 */
static void
i915_vkcs_step_spill(
	struct i915_vkcs *x)
{
	const struct i915_gfx_pipeline *pipeline;
	const uint32_t *o;
	uint32_t index;
	int error;

	/* Needs its pipeline, which must spill. */
	if (i915_vkcs_ready(x, I915_VKCS_KERNEL_SPILL, "SPILL") == 0)
		return;
	pipeline = drv_i915_object_lookup(x->render, I915_VK_OBJ_PIPELINE, I915_VKCS_ID_PIPELINE + I915_VKCS_KERNEL_SPILL);
	if (pipeline == NULL || pipeline->cs_binary == NULL || pipeline->cs_binary->scratch_bytes == 0U) {
		kern_logf("i915: vkcs: SPILL: the kernel does not spill\n");
		i915_vkcs_verdict(x, "SPILL", EINVAL);
		return;
	}

	/* Records the pipeline, the set and the groups; runs it. */
	i915_vkcs_fill(x, I915_VKCS_BUF_S, I915_VKCS_SENTINEL);
	i915_vkcs_begin(x);
	i915_vkcs_bind_compute(x, I915_VKCS_KERNEL_SPILL, I915_VKCS_SET_SPILL, 0, 0U);
	i915_vkcs_dispatch(x, I915_VKCS_SPILL_GROUPS, 1U, 1U);
	error = i915_vkcs_finish(x, "SPILL");
	if (error != 0) {
		i915_vkcs_verdict(x, "SPILL", error);
		return;
	}

	/* Every invocation's sum. */
	i915_vkcs_flush(x, I915_VKCS_BUF_S);
	o = i915_vkcs_words(x, I915_VKCS_BUF_S);
	for (index = 0U; index < I915_VKCS_SPILL_GROUPS * 64U; index++)
		i915_vkcs_check(x, "SPILL", index, o[index], i915_vkcs_spill_word(index));
	i915_vkcs_verdict(x, "SPILL", i915_vkcs_checked(x, "SPILL"));
}

/*
 * Computes what spill.comp writes for one invocation: the values x * (k +
 * 1) + y, the loop over (x + y) % 5 trips, the sum of the paired products,
 * all modulo 2^32 (compute-shaders/regenerate.py writes the shader).
 */
static uint32_t
i915_vkcs_spill_word(
	uint32_t invocation)
{
	uint32_t t[I915_VKCS_SPILL_VALUES];
	uint32_t y;
	uint32_t a;
	uint32_t b;
	uint32_t n;
	uint32_t i;
	uint32_t k;
	uint32_t s;

	/* The values. */
	y = (invocation * 2654435761U) >> 7;
	for (k = 0U; k < I915_VKCS_SPILL_VALUES; k++)
		t[k] = invocation * (k + 1U) + y;

	/* The loop. */
	a = t[7];
	b = t[50];
	n = (invocation + y) % 5U;
	for (i = 0U; i < n; i++) {
		a = a * 3U + t[3];
		b = b + t[90] * 5U - a;
	}

	/* The sum of the pairs. */
	s = 0U;
	for (k = 0U; k < I915_VKCS_SPILL_VALUES; k++)
		s += t[k] * t[(k * I915_VKCS_SPILL_STRIDE + I915_VKCS_SPILL_SHIFT) % I915_VKCS_SPILL_VALUES];

	/* Succeeded: the word the invocation writes. */
	return s + a + b;
}

/*
 * Records the binds of a step's pipeline and set and a dispatch of `groups`
 * along x, and runs it.  Returns 0, or the error with the
 * step's verdict already logged; a step without its pipeline fails here.
 */
static int
i915_vkcs_run(
	struct i915_vkcs *x,
	const char *what,
	uint32_t kernel,
	uint32_t set,
	uint32_t groups)
{
	int error;

	/* Needs its pipeline. */
	if (i915_vkcs_ready(x, kernel, what) == 0)
		return ENOENT;

	/* Records and runs. */
	i915_vkcs_begin(x);
	i915_vkcs_bind_compute(x, kernel, set, 0, 0U);
	i915_vkcs_dispatch(x, groups, 1U, 1U);
	error = i915_vkcs_finish(x, what);
	if (error != 0) {
		i915_vkcs_verdict(x, what, error);
		return error;
	}

	/* Succeeded: the dispatch ran. */
	return 0;
}

/*
 * SHARED: shared.comp over 8 x 8 groups of 8 x 8; C must be A transposed,
 * every word.
 */
static void
i915_vkcs_step_shared(
	struct i915_vkcs *x)
{
	const uint32_t *t;
	uint32_t *m;
	uint32_t row;
	uint32_t column;
	int error;

	/* Needs its pipeline. */
	if (i915_vkcs_ready(x, I915_VKCS_KERNEL_SHARED, "SHARED") == 0)
		return;

	/* The matrix, and C all sentinel. */
	m = i915_vkcs_words(x, I915_VKCS_BUF_A);
	for (row = 0U; row < 4096U; row++)
		m[row] = i915_vkcs_hash(row + 0x20000U);
	i915_vkcs_flush(x, I915_VKCS_BUF_A);
	i915_vkcs_fill(x, I915_VKCS_BUF_C, I915_VKCS_SENTINEL);

	/* Records the pipeline, the set and 8 x 8 groups; runs it. */
	i915_vkcs_begin(x);
	i915_vkcs_bind_compute(x, I915_VKCS_KERNEL_SHARED, I915_VKCS_SET_TWO, 0, 0U);
	i915_vkcs_dispatch(x, 8U, 8U, 1U);
	error = i915_vkcs_finish(x, "SHARED");
	if (error != 0) {
		i915_vkcs_verdict(x, "SHARED", error);
		return;
	}

	/* The transpose, and the sentinel after it. */
	i915_vkcs_flush(x, I915_VKCS_BUF_C);
	t = i915_vkcs_words(x, I915_VKCS_BUF_C);
	for (row = 0U; row < 64U; row++) {
		for (column = 0U; column < 64U; column++)
			i915_vkcs_check(x, "SHARED", row * 64U + column, t[row * 64U + column], m[column * 64U + row]);
	}
	for (row = 4096U; row < 4096U + 64U; row++)
		i915_vkcs_check(x, "SHARED", row, t[row], I915_VKCS_SENTINEL);
	i915_vkcs_verdict(x, "SHARED", i915_vkcs_checked(x, "SHARED"));
}

/* REDUCE: reduce.comp over 64 groups of 128; C's word g must be the sum of group g's words. */
static void
i915_vkcs_step_reduce(
	struct i915_vkcs *x)
{
	const uint32_t *sums;
	uint32_t *v;
	uint32_t group;
	uint32_t index;
	uint32_t sum;

	/* The words, and C all sentinel. */
	v = i915_vkcs_words(x, I915_VKCS_BUF_A);
	for (index = 0U; index < I915_VKCS_REDUCE_GROUPS * 128U; index++)
		v[index] = i915_vkcs_hash(index + 0x30000U) >> 8;
	i915_vkcs_flush(x, I915_VKCS_BUF_A);
	i915_vkcs_fill(x, I915_VKCS_BUF_C, I915_VKCS_SENTINEL);

	/* Runs the groups. */
	if (i915_vkcs_run(x, "REDUCE", I915_VKCS_KERNEL_REDUCE, I915_VKCS_SET_TWO, I915_VKCS_REDUCE_GROUPS) != 0)
		return;

	/* Each group's sum, and the sentinel after them. */
	i915_vkcs_flush(x, I915_VKCS_BUF_C);
	sums = i915_vkcs_words(x, I915_VKCS_BUF_C);
	for (group = 0U; group < I915_VKCS_REDUCE_GROUPS; group++) {
		sum = 0U;
		for (index = 0U; index < 128U; index++)
			sum += v[group * 128U + index];
		i915_vkcs_check(x, "REDUCE", group, sums[group], sum);
	}
	for (group = I915_VKCS_REDUCE_GROUPS; group < I915_VKCS_REDUCE_GROUPS + 64U; group++)
		i915_vkcs_check(x, "REDUCE", group, sums[group], I915_VKCS_SENTINEL);
	i915_vkcs_verdict(x, "REDUCE", i915_vkcs_checked(x, "REDUCE"));
}

/* ODD-BARRIER: oddbar.comp over 10 groups of 5 x 3; o[g * 15 + l] = 3 v[g * 15 + 14 - l] + l. */
static void
i915_vkcs_step_oddbar(
	struct i915_vkcs *x)
{
	const uint32_t *o;
	uint32_t *v;
	uint32_t group;
	uint32_t index;

	/* The words, and C all sentinel. */
	v = i915_vkcs_words(x, I915_VKCS_BUF_A);
	for (index = 0U; index < I915_VKCS_ODDBAR_GROUPS * 15U; index++)
		v[index] = i915_vkcs_hash(index + 0x40000U);
	i915_vkcs_flush(x, I915_VKCS_BUF_A);
	i915_vkcs_fill(x, I915_VKCS_BUF_C, I915_VKCS_SENTINEL);

	/* Runs the groups. */
	if (i915_vkcs_run(x, "ODD-BARRIER", I915_VKCS_KERNEL_ODDBAR, I915_VKCS_SET_TWO, I915_VKCS_ODDBAR_GROUPS) != 0)
		return;

	/* Each group's mirrored words, and the sentinel after them. */
	i915_vkcs_flush(x, I915_VKCS_BUF_C);
	o = i915_vkcs_words(x, I915_VKCS_BUF_C);
	for (group = 0U; group < I915_VKCS_ODDBAR_GROUPS; group++) {
		for (index = 0U; index < 15U; index++)
			i915_vkcs_check(x, "ODD-BARRIER", group * 15U + index, o[group * 15U + index], v[group * 15U + 14U - index] * 3U + index);
	}
	for (index = I915_VKCS_ODDBAR_GROUPS * 15U; index < I915_VKCS_ODDBAR_GROUPS * 15U + 64U; index++)
		i915_vkcs_check(x, "ODD-BARRIER", index, o[index], I915_VKCS_SENTINEL);
	i915_vkcs_verdict(x, "ODD-BARRIER", i915_vkcs_checked(x, "ODD-BARRIER"));
}

/*
 * ATOMIC-SHARED: atomsh.comp over 16 groups of 64.  Each group's 20 words:
 * its histogram of v % 16, its sum, its maximum, the compare-exchange's
 * winner (some invocation's index plus one, whichever came first), the
 * counter (64); and each group's old counter values once each.
 */
static void
i915_vkcs_step_atomsh(
	struct i915_vkcs *x)
{
	uint32_t expected[I915_VKCS_ATOMSH_WORDS];
	uint8_t seen[64];
	const uint32_t *results;
	const uint32_t *old;
	uint32_t *v;
	uint32_t group;
	uint32_t index;
	uint32_t value;

	/* The words, the results and the old values all sentinel. */
	v = i915_vkcs_words(x, I915_VKCS_BUF_A);
	for (index = 0U; index < I915_VKCS_ATOMSH_GROUPS * 64U; index++)
		v[index] = i915_vkcs_hash(index + 0x50000U) >> 20;
	i915_vkcs_flush(x, I915_VKCS_BUF_A);
	i915_vkcs_fill(x, I915_VKCS_BUF_B, I915_VKCS_SENTINEL);
	i915_vkcs_fill(x, I915_VKCS_BUF_D, I915_VKCS_SENTINEL);

	/* Runs the groups. */
	if (i915_vkcs_run(x, "ATOMIC-SHARED", I915_VKCS_KERNEL_ATOMSH, I915_VKCS_SET_THREE, I915_VKCS_ATOMSH_GROUPS) != 0)
		return;

	/* Each group's results and its old values. */
	i915_vkcs_flush(x, I915_VKCS_BUF_B);
	i915_vkcs_flush(x, I915_VKCS_BUF_D);
	results = i915_vkcs_words(x, I915_VKCS_BUF_B);
	old = i915_vkcs_words(x, I915_VKCS_BUF_D);
	for (group = 0U; group < I915_VKCS_ATOMSH_GROUPS; group++) {
		kern_memset(expected, 0, sizeof(expected));
		for (index = 0U; index < 64U; index++) {
			value = v[group * 64U + index];
			expected[value % 16U]++;
			expected[16] += value;
			if (value > expected[17])
				expected[17] = value;
		}
		expected[19] = 64U;
		for (index = 0U; index < I915_VKCS_ATOMSH_WORDS; index++) {
			if (index == 18U)
				continue;
			i915_vkcs_check(x, "ATOMIC-SHARED", group * I915_VKCS_ATOMSH_WORDS + index, results[group * I915_VKCS_ATOMSH_WORDS + index], expected[index]);
		}
		value = results[group * I915_VKCS_ATOMSH_WORDS + 18U];
		if (value == 0U || value > 64U)
			i915_vkcs_check(x, "ATOMIC-SHARED", group * I915_VKCS_ATOMSH_WORDS + 18U, value, 1U);
		kern_memset(seen, 0, sizeof(seen));
		for (index = 0U; index < 64U; index++) {
			value = old[group * 64U + index];
			if (value >= 64U || seen[value] != 0U) {
				i915_vkcs_check(x, "ATOMIC-SHARED", 0x1000U + group * 64U + index, value, 0xffffffffU);
				continue;
			}
			seen[value] = 1U;
		}
	}
	i915_vkcs_verdict(x, "ATOMIC-SHARED", i915_vkcs_checked(x, "ATOMIC-SHARED"));
}

/* LOOP: scan.comp over 32 groups of 64; each group's inclusive prefix sums. */
static void
i915_vkcs_step_scan(
	struct i915_vkcs *x)
{
	const uint32_t *prefix;
	uint32_t *v;
	uint32_t index;
	uint32_t sum;

	/* The words, and C all sentinel. */
	v = i915_vkcs_words(x, I915_VKCS_BUF_A);
	for (index = 0U; index < I915_VKCS_SCAN_GROUPS * 64U; index++)
		v[index] = i915_vkcs_hash(index + 0x60000U) >> 12;
	i915_vkcs_flush(x, I915_VKCS_BUF_A);
	i915_vkcs_fill(x, I915_VKCS_BUF_C, I915_VKCS_SENTINEL);

	/* Runs the groups. */
	if (i915_vkcs_run(x, "LOOP", I915_VKCS_KERNEL_SCAN, I915_VKCS_SET_TWO, I915_VKCS_SCAN_GROUPS) != 0)
		return;

	/* The prefix sums, starting afresh at each group, and the sentinel after them. */
	i915_vkcs_flush(x, I915_VKCS_BUF_C);
	prefix = i915_vkcs_words(x, I915_VKCS_BUF_C);
	sum = 0U;
	for (index = 0U; index < I915_VKCS_SCAN_GROUPS * 64U; index++) {
		if (index % 64U == 0U)
			sum = 0U;
		sum += v[index];
		i915_vkcs_check(x, "LOOP", index, prefix[index], sum);
	}
	for (index = I915_VKCS_SCAN_GROUPS * 64U; index < I915_VKCS_SCAN_GROUPS * 64U + 64U; index++)
		i915_vkcs_check(x, "LOOP", index, prefix[index], I915_VKCS_SENTINEL);
	i915_vkcs_verdict(x, "LOOP", i915_vkcs_checked(x, "LOOP"));
}

/*
 * REFUSE: the three modules the compiler must refuse -- shared memory past
 * 16 KiB, a barrier after a return, a barrier in a loop some invocations
 * skip -- made no pipeline at vkCreateComputePipelines (the setup tried;
 * the executor logged each reason).
 */
static void
i915_vkcs_step_refuse(
	struct i915_vkcs *x)
{
	/* None of the three was made. */
	i915_vkcs_check(x, "REFUSE", I915_VKCS_KERNEL_REFUSE_BIG, (uint32_t)x->made[I915_VKCS_KERNEL_REFUSE_BIG], 0U);
	i915_vkcs_check(x, "REFUSE", I915_VKCS_KERNEL_REFUSE_RETURN, (uint32_t)x->made[I915_VKCS_KERNEL_REFUSE_RETURN], 0U);
	i915_vkcs_check(x, "REFUSE", I915_VKCS_KERNEL_REFUSE_LOOP, (uint32_t)x->made[I915_VKCS_KERNEL_REFUSE_LOOP], 0U);
	i915_vkcs_verdict(x, "REFUSE", i915_vkcs_checked(x, "REFUSE"));
}

/* Writes ADD's inputs into A and B and fills C with the sentinel (the formulas of the ADD step). */
static void
i915_vkcs_add_inputs(
	struct i915_vkcs *x)
{
	uint32_t *a;
	uint32_t *b;
	uint32_t index;

	/* A and B from their index, C all sentinel. */
	a = i915_vkcs_words(x, I915_VKCS_BUF_A);
	b = i915_vkcs_words(x, I915_VKCS_BUF_B);
	for (index = 0U; index < I915_VKCS_REGION_WORDS; index++) {
		a[index] = index * 0x9e3779b9U;
		b[index] = index ^ 0x5a5a5a5aU;
	}

	/* Written back for the GPU, C filled. */
	i915_vkcs_flush(x, I915_VKCS_BUF_A);
	i915_vkcs_flush(x, I915_VKCS_BUF_B);
	i915_vkcs_fill(x, I915_VKCS_BUF_C, I915_VKCS_SENTINEL);
}

/* Checks C after ADD over 16 groups: a + b below n (0: nothing written), the sentinel from n on; logs the verdict. */
static void
i915_vkcs_add_check(
	struct i915_vkcs *x,
	const char *what,
	uint32_t n)
{
	const uint32_t *a;
	const uint32_t *b;
	const uint32_t *c;
	uint32_t index;

	/* The sums below n, the sentinel from n on, a group past the last. */
	i915_vkcs_flush(x, I915_VKCS_BUF_C);
	a = i915_vkcs_words(x, I915_VKCS_BUF_A);
	b = i915_vkcs_words(x, I915_VKCS_BUF_B);
	c = i915_vkcs_words(x, I915_VKCS_BUF_C);
	for (index = 0U; index < I915_VKCS_ADD_GROUPS * 64U + 64U; index++) {
		if (index < n) {
			i915_vkcs_check(x, what, index, c[index], a[index] + b[index]);
		} else {
			i915_vkcs_check(x, what, index, c[index], I915_VKCS_SENTINEL);
		}
	}

	/* The step's verdict. */
	i915_vkcs_verdict(x, what, i915_vkcs_checked(x, what));
}

/* Writes three group counts into G at a word, on the CPU, and makes them visible to the GPU. */
static void
i915_vkcs_grid(
	struct i915_vkcs *x,
	uint32_t at,
	uint32_t gx,
	uint32_t gy,
	uint32_t gz)
{
	uint32_t *g;

	/* The counts along x, y and z. */
	g = i915_vkcs_words(x, I915_VKCS_BUF_G);
	g[at] = gx;
	g[at + 1U] = gy;
	g[at + 2U] = gz;
	i915_vkcs_flush(x, I915_VKCS_BUF_G);
}

/*
 * INDIRECT: ADD over n = 1000 with its 16 x 1 x 1 groups read
 * from G at byte 16 by vkCmdDispatchIndirect; the words before and after
 * the counts are other grids' (zeros), so a read at the wrong place runs
 * nothing.
 */
static void
i915_vkcs_step_indirect(
	struct i915_vkcs *x)
{
	uint32_t n;
	int ready;
	int error;

	/* Needs ADD's pipeline. */
	ready = i915_vkcs_ready(x, I915_VKCS_KERNEL_ADD, "INDIRECT");
	if (ready == 0)
		return;

	/* The inputs, and the counts in G among zeros. */
	i915_vkcs_add_inputs(x);
	i915_vkcs_fill(x, I915_VKCS_BUF_G, 0U);
	i915_vkcs_grid(x, I915_VKCS_G_ADD, I915_VKCS_ADD_GROUPS, 1U, 1U);

	/* Records n, the pipeline, the set and the indirect dispatch; runs it. */
	n = I915_VKCS_ADD_N;
	i915_vkcs_begin(x);
	i915_vkcs_bind_compute(x, I915_VKCS_KERNEL_ADD, I915_VKCS_SET_ADD, 0, 0U);
	i915_vkcs_push(x, &n, 1U);
	i915_vkcs_dispatch_indirect(x, I915_VKCS_BUF_G, I915_VKCS_G_ADD * 4U);
	error = i915_vkcs_finish(x, "INDIRECT");
	if (error != 0) {
		i915_vkcs_verdict(x, "INDIRECT", error);
		return;
	}

	/* As ADD. */
	i915_vkcs_add_check(x, "INDIRECT", n);
}

/*
 * INDIRECT-GPU: in one command buffer, GRID writes 16, 1, 1
 * into G at word 8 on the GPU, and ADD's indirect dispatch reads its counts
 * there; G starts at zero, so counts read before the GPU wrote them run
 * nothing and the step fails.
 */
static void
i915_vkcs_step_indirect_gpu(
	struct i915_vkcs *x)
{
	uint32_t grid[4];
	const uint32_t *g;
	uint32_t n;
	int ready;
	int error;

	/* Needs both pipelines. */
	ready = i915_vkcs_ready(x, I915_VKCS_KERNEL_GRID, "INDIRECT-GPU");
	if (ready == 0)
		return;
	ready = i915_vkcs_ready(x, I915_VKCS_KERNEL_ADD, "INDIRECT-GPU");
	if (ready == 0)
		return;

	/* The inputs, and G all zero. */
	i915_vkcs_add_inputs(x);
	i915_vkcs_fill(x, I915_VKCS_BUF_G, 0U);

	/* Records GRID writing the counts, then ADD reading them; runs both. */
	grid[0] = I915_VKCS_G_GPU;
	grid[1] = I915_VKCS_ADD_GROUPS;
	grid[2] = 1U;
	grid[3] = 1U;
	n = I915_VKCS_ADD_N;
	i915_vkcs_begin(x);
	i915_vkcs_bind_compute(x, I915_VKCS_KERNEL_GRID, I915_VKCS_SET_GRID, 0, 0U);
	i915_vkcs_push(x, grid, 4U);
	i915_vkcs_dispatch(x, 1U, 1U, 1U);
	i915_vkcs_bind_compute(x, I915_VKCS_KERNEL_ADD, I915_VKCS_SET_ADD, 0, 0U);
	i915_vkcs_push(x, &n, 1U);
	i915_vkcs_dispatch_indirect(x, I915_VKCS_BUF_G, I915_VKCS_G_GPU * 4U);
	error = i915_vkcs_finish(x, "INDIRECT-GPU");
	if (error != 0) {
		i915_vkcs_verdict(x, "INDIRECT-GPU", error);
		return;
	}

	/* The counts GRID wrote, then ADD's words. */
	i915_vkcs_flush(x, I915_VKCS_BUF_G);
	g = i915_vkcs_words(x, I915_VKCS_BUF_G);
	i915_vkcs_check(x, "INDIRECT-GPU", 0x1000U + I915_VKCS_G_GPU, g[I915_VKCS_G_GPU], I915_VKCS_ADD_GROUPS);
	i915_vkcs_check(x, "INDIRECT-GPU", 0x1001U + I915_VKCS_G_GPU, g[I915_VKCS_G_GPU + 1U], 1U);
	i915_vkcs_check(x, "INDIRECT-GPU", 0x1002U + I915_VKCS_G_GPU, g[I915_VKCS_G_GPU + 2U], 1U);
	i915_vkcs_add_check(x, "INDIRECT-GPU", n);
}

/*
 * INDIRECT-ZERO: three indirect dispatches of ADD whose grids
 * have a zero along x, y and z; C keeps its sentinel, and the command
 * buffer (and the steps after it) run on.
 */
static void
i915_vkcs_step_indirect_zero(
	struct i915_vkcs *x)
{
	uint32_t n;
	uint32_t index;
	int ready;
	int error;

	/* Needs ADD's pipeline. */
	ready = i915_vkcs_ready(x, I915_VKCS_KERNEL_ADD, "INDIRECT-ZERO");
	if (ready == 0)
		return;

	/* The inputs, and the three grids with a zero each. */
	i915_vkcs_add_inputs(x);
	i915_vkcs_fill(x, I915_VKCS_BUF_G, 0U);
	i915_vkcs_grid(x, I915_VKCS_G_ZERO, 0U, 1U, 1U);
	i915_vkcs_grid(x, I915_VKCS_G_ZERO + 4U, I915_VKCS_ADD_GROUPS, 0U, 1U);
	i915_vkcs_grid(x, I915_VKCS_G_ZERO + 8U, I915_VKCS_ADD_GROUPS, 1U, 0U);

	/* Records the three dispatches; runs them. */
	n = I915_VKCS_ADD_N;
	i915_vkcs_begin(x);
	i915_vkcs_bind_compute(x, I915_VKCS_KERNEL_ADD, I915_VKCS_SET_ADD, 0, 0U);
	i915_vkcs_push(x, &n, 1U);
	for (index = 0U; index < 3U; index++)
		i915_vkcs_dispatch_indirect(x, I915_VKCS_BUF_G, (I915_VKCS_G_ZERO + index * 4U) * 4U);
	error = i915_vkcs_finish(x, "INDIRECT-ZERO");
	if (error != 0) {
		i915_vkcs_verdict(x, "INDIRECT-ZERO", error);
		return;
	}

	/* Nothing written: every word the sentinel. */
	i915_vkcs_add_check(x, "INDIRECT-ZERO", 0U);
}

/*
 * LENGTH: length.comp writes the lengths of W's words over a
 * range of 400 bytes (100) and of T's 12-byte structures after its 8-byte
 * head over 457 bytes (37: the last 5 bytes are no structure), and a sum of
 * words read from T (head.x + items[1].b).
 */
static void
i915_vkcs_step_length(
	struct i915_vkcs *x)
{
	uint32_t *t;
	const uint32_t *o;
	int ready;
	int error;

	/* Needs its pipeline. */
	ready = i915_vkcs_ready(x, I915_VKCS_KERNEL_LENGTH, "LENGTH");
	if (ready == 0)
		return;

	/* T's head and structures (T is B), W (A) anything, the output all sentinel. */
	i915_vkcs_fill(x, I915_VKCS_BUF_A, 0x11111111U);
	t = i915_vkcs_words(x, I915_VKCS_BUF_B);
	kern_memset(t, 0, I915_VKCS_REGION_BYTES);
	t[0] = 1000U;
	t[2U + 3U + 1U] = 234U;
	i915_vkcs_flush(x, I915_VKCS_BUF_B);
	i915_vkcs_fill(x, I915_VKCS_BUF_C, I915_VKCS_SENTINEL);

	/* Records the pipeline, the set and one group; runs it. */
	i915_vkcs_begin(x);
	i915_vkcs_bind_compute(x, I915_VKCS_KERNEL_LENGTH, I915_VKCS_SET_LENGTH, 0, 0U);
	i915_vkcs_dispatch(x, 1U, 1U, 1U);
	error = i915_vkcs_finish(x, "LENGTH");
	if (error != 0) {
		i915_vkcs_verdict(x, "LENGTH", error);
		return;
	}

	/* The two lengths, the sum, and nothing after them. */
	i915_vkcs_flush(x, I915_VKCS_BUF_C);
	o = i915_vkcs_words(x, I915_VKCS_BUF_C);
	i915_vkcs_check(x, "LENGTH", 0U, o[0], I915_VKCS_LENGTH_W_RANGE / 4U);
	i915_vkcs_check(x, "LENGTH", 1U, o[1], (I915_VKCS_LENGTH_T_RANGE - 8U) / 12U);
	i915_vkcs_check(x, "LENGTH", 2U, o[2], 1234U);
	i915_vkcs_check(x, "LENGTH", 3U, o[3], I915_VKCS_SENTINEL);
	i915_vkcs_verdict(x, "LENGTH", i915_vkcs_checked(x, "LENGTH"));
}

/*
 * MANY: ADD over n = 4,000,000 in 62,500 groups of 64 (the
 * size of Noct's demonstration, design section 4.2), over three buffers
 * of 16 MB in objects of their own; every word is checked, and the time of
 * the submission (the recording, the run on the GPU and the wait) is
 * logged for reference.
 */
static void
i915_vkcs_step_big(
	struct i915_vkcs *x)
{
	uint32_t *a;
	uint32_t *b;
	uint32_t *c;
	uint64_t started;
	uint64_t elapsed;
	uint32_t index;
	uint32_t n;
	int ready;
	int error;

	/* Needs ADD's pipeline and the three objects. */
	ready = i915_vkcs_ready(x, I915_VKCS_KERNEL_ADD, "MANY");
	if (ready == 0)
		return;
	if (x->many_ready == 0) {
		i915_vkcs_verdict(x, "MANY", ENOMEM);
		return;
	}

	/* A and B from their index as ADD's, C all sentinel. */
	a = x->many_cpu[0];
	b = x->many_cpu[1];
	c = x->many_cpu[2];
	for (index = 0U; index < I915_VKCS_MANY_N; index++) {
		a[index] = index * 0x9e3779b9U;
		b[index] = index ^ 0x5a5a5a5aU;
		c[index] = I915_VKCS_SENTINEL;
	}

	/* Written back for the GPU. */
	drv_i915_gt_clflush(a, I915_VKCS_MANY_BYTES);
	drv_i915_gt_clflush(b, I915_VKCS_MANY_BYTES);
	drv_i915_gt_clflush(c, I915_VKCS_MANY_BYTES);

	/* Records n, the pipeline, the set and the groups; runs it, timed. */
	n = I915_VKCS_MANY_N;
	i915_vkcs_begin(x);
	i915_vkcs_bind_compute(x, I915_VKCS_KERNEL_ADD, I915_VKCS_SET_MANY, 0, 0U);
	i915_vkcs_push(x, &n, 1U);
	i915_vkcs_dispatch(x, I915_VKCS_MANY_GROUPS, 1U, 1U);
	started = i915_vkcs_now_us();
	error = i915_vkcs_finish(x, "MANY");
	elapsed = i915_vkcs_now_us() - started;
	if (error != 0) {
		i915_vkcs_verdict(x, "MANY", error);
		return;
	}

	/* The time, for reference (the verdict is the words'). */
	kern_logf("i915: vkcs: MANY: n %u in %u groups, the submission took %u us\n",
		  n,
		  I915_VKCS_MANY_GROUPS,
		  (unsigned)elapsed);

	/* Every sum. */
	drv_i915_gt_clflush(c, I915_VKCS_MANY_BYTES);
	for (index = 0U; index < I915_VKCS_MANY_N; index++)
		i915_vkcs_check(x, "MANY", index, c[index], a[index] + b[index]);
	i915_vkcs_verdict(x, "MANY", i915_vkcs_checked(x, "MANY"));
}

/* Reads the time of day in microseconds (the MANY step's timing). */
static uint64_t
i915_vkcs_now_us(void)
{
	time_t seconds;
	long nanoseconds;

	/* The real-time clock, to the microsecond. */
	clock_realtime(&seconds, &nanoseconds);

	/* Succeeded: the microseconds. */
	return (uint64_t)seconds * 1000000U + (uint64_t)nanoseconds / 1000U;
}
