/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * ws101-p003: the executor's compute pipelines and the recording of
 * dispatches, on the host.
 *
 * vkCreateShaderModule, vkCreateComputePipelines, vkCreateGraphicsPipelines
 * and the recording commands are driven through the wire as libvulkan
 * encodes them (the stand-ins of plan/ws031/tests/i915-vk-render-stubs.inc
 * run the executor's session).  command.c is part of this file, so the
 * recorded operations and the bind points are looked at directly.
 *
 *   executor-test DIR     DIR holds add.spv and ids.spv (compute) and cuboid.vert.spv, cuboid.frag.spv (vkdemo)
 */

#include "../../../ws031/tests/i915-vk-render-stubs.inc"

#include "../../../../src/drivers/gpu/i915/compiler/compiler.h"
#include "../../../../src/drivers/gpu/i915/render/command.c"

/* The wire opcodes the test sends, as libvulkan numbers them. */
#define TEST_CREATE_SHADER_MODULE	59U
#define TEST_CREATE_GRAPHICS_PIPELINES	65U
#define TEST_CREATE_COMPUTE_PIPELINES	66U
#define TEST_DESTROY_PIPELINE		67U
#define TEST_CREATE_COMMAND_POOL	85U
#define TEST_ALLOCATE_COMMAND_BUFFERS	88U
#define TEST_BEGIN_COMMAND_BUFFER	90U
#define TEST_END_COMMAND_BUFFER		91U
#define TEST_CMD_BIND_PIPELINE		93U
#define TEST_CMD_BIND_DESCRIPTOR_SETS	103U
#define TEST_CMD_DISPATCH		110U

/* The wire identities the test gives its objects. */
#define TEST_DEVICE		0xd0ULL
#define TEST_ADD		0xd00ULL
#define TEST_IDS		0xd01ULL
#define TEST_VS			0xd02ULL
#define TEST_FS			0xd03ULL
#define TEST_ADD_PIPELINE	0xe00ULL
#define TEST_IDS_PIPELINE	0xe01ULL
#define TEST_BAD_PIPELINE	0xe02ULL
#define TEST_POOL		0xf00ULL
#define TEST_CB			0xf01ULL
#define TEST_SET		0xf02ULL

/* The stream every command is built in. */
static struct stub_wire test_wire;

/* The directory of the SPIR-V modules. */
static const char *test_dir;

static uint32_t *test_load(const char *name, size_t *words);
static void test_module(const char *name, uint64_t identity);
static void test_stage(uint32_t stage, uint64_t module);
static void test_compute_pipeline(uint64_t module, uint64_t identity, int specialized);
static void test_graphics_pipeline(uint64_t vertex, uint64_t fragment, uint64_t identity);
static uint32_t test_create_result(void);
static void test_pipelines(void);
static void test_thread_ids(void);
static void test_stage_mismatch(void);
static void test_recording(void);

int
main(
	int argc,
	char **argv)
{
	/* The directory of the modules. */
	assert(argc == 2);
	test_dir = argv[1];

	/* Runs every check; an assertion stops the test. */
	test_pipelines();
	test_thread_ids();
	test_stage_mismatch();
	test_recording();

	/* Succeeded: every check held. */
	printf("ws101 executor host test PASS\n");
	return 0;
}

/* Loads DIR/NAME into words the caller frees. */
static uint32_t *
test_load(
	const char *name,
	size_t *words)
{
	char path[512];
	uint32_t *code;
	FILE *file;
	long size;

	snprintf(path, sizeof(path), "%s/%s", test_dir, name);
	file = fopen(path, "rb");
	assert(file != NULL);
	fseek(file, 0, SEEK_END);
	size = ftell(file);
	assert(size > 0 && (size % 4) == 0);
	fseek(file, 0, SEEK_SET);
	code = malloc((size_t)size);
	assert(code != NULL);
	assert(fread(code, 1U, (size_t)size, file) == (size_t)size);
	fclose(file);
	*words = (size_t)size / 4U;
	return code;
}

/* Creates a shader module from DIR/NAME through the wire. */
static void
test_module(
	const char *name,
	uint64_t identity)
{
	uint32_t *code;
	size_t words;
	size_t index;

	code = test_load(name, &words);
	stub_wire_begin(&test_wire);
	stub_put32(&test_wire, TEST_CREATE_SHADER_MODULE);
	stub_put32(&test_wire, 1U);
	stub_put64(&test_wire, TEST_DEVICE);
	stub_put64(&test_wire, 1U);
	stub_put32(&test_wire, 16U);
	stub_put64(&test_wire, 0U);
	stub_put32(&test_wire, 0U);
	stub_put64(&test_wire, (uint64_t)words * 4U);
	stub_put64(&test_wire, (uint64_t)words);
	for (index = 0U; index < words; index++)
		stub_put32(&test_wire, code[index]);
	stub_put64(&test_wire, 0U);
	stub_put64(&test_wire, 1U);
	stub_put64(&test_wire, identity);
	(void)stub_execute_ok(&test_wire);
	free(code);
	assert(drv_i915_object_lookup(stub_session, I915_VK_OBJ_SHADER_MODULE, identity) != NULL);
}

/* Appends one VkPipelineShaderStageCreateInfo of `stage` naming `module`, entry "main", no specialization. */
static void
test_stage(
	uint32_t stage,
	uint64_t module)
{
	stub_put32(&test_wire, 18U);
	stub_put64(&test_wire, 0U);
	stub_put32(&test_wire, 0U);
	stub_put32(&test_wire, stage);
	stub_put64(&test_wire, module);
	stub_put64(&test_wire, 5U);
	stub_put32(&test_wire, 0x6e69616dU);
	stub_put32(&test_wire, 0U);
}

/* Sends vkCreateComputePipelines of one pipeline; `specialized` gives it one specialization constant. */
static void
test_compute_pipeline(
	uint64_t module,
	uint64_t identity,
	int specialized)
{
	stub_wire_begin(&test_wire);
	stub_put32(&test_wire, TEST_CREATE_COMPUTE_PIPELINES);
	stub_put32(&test_wire, 1U);
	stub_put64(&test_wire, TEST_DEVICE);
	stub_put64(&test_wire, 0U);
	stub_put32(&test_wire, 1U);
	stub_put64(&test_wire, 1U);

	/* VkComputePipelineCreateInfo: sType 29, no chain, flags, the stage. */
	stub_put32(&test_wire, 29U);
	stub_put64(&test_wire, 0U);
	stub_put32(&test_wire, 0U);
	test_stage(VK_SHADER_STAGE_COMPUTE_BIT, module);

	/* The stage's specialization: none, or one 4-byte constant 0 at offset 0 with its data. */
	if (specialized != 0) {
		stub_put64(&test_wire, 1U);
		stub_put32(&test_wire, 1U);
		stub_put64(&test_wire, 1U);
		stub_put32(&test_wire, 0U);
		stub_put32(&test_wire, 0U);
		stub_put64(&test_wire, 4U);
		stub_put64(&test_wire, 4U);
		stub_put64(&test_wire, 4U);
		stub_put32(&test_wire, 64U);
	} else {
		stub_put64(&test_wire, 0U);
	}

	/* No layout, no base pipeline; no allocator, then one identity. */
	stub_put64(&test_wire, 0U);
	stub_put64(&test_wire, 0U);
	stub_put32(&test_wire, 0U);
	stub_put64(&test_wire, 0U);
	stub_put64(&test_wire, 1U);
	stub_put64(&test_wire, identity);
}

/* Sends vkCreateGraphicsPipelines of one triangle-list pipeline of two stages (the pipe test's form). */
static void
test_graphics_pipeline(
	uint64_t vertex,
	uint64_t fragment,
	uint64_t identity)
{
	unsigned index;

	stub_wire_begin(&test_wire);
	stub_put32(&test_wire, TEST_CREATE_GRAPHICS_PIPELINES);
	stub_put32(&test_wire, 1U);
	stub_put64(&test_wire, TEST_DEVICE);
	stub_put64(&test_wire, 0U);
	stub_put32(&test_wire, 1U);
	stub_put64(&test_wire, 1U);
	stub_put32(&test_wire, 28U);
	stub_put64(&test_wire, 0U);
	stub_put32(&test_wire, 0U);
	stub_put32(&test_wire, 2U);
	stub_put64(&test_wire, 2U);
	test_stage(VK_SHADER_STAGE_VERTEX_BIT, vertex);
	stub_put64(&test_wire, 0U);
	test_stage(VK_SHADER_STAGE_FRAGMENT_BIT, fragment);
	stub_put64(&test_wire, 0U);
	stub_put64(&test_wire, 0U);
	stub_put64(&test_wire, 1U);
	stub_put32(&test_wire, 20U);
	stub_put64(&test_wire, 0U);
	stub_put32(&test_wire, 0U);
	stub_put32(&test_wire, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
	stub_put32(&test_wire, 0U);
	stub_put64(&test_wire, 0U);
	stub_put64(&test_wire, 0U);
	stub_put64(&test_wire, 1U);
	for (index = 0U; index < 14U; index++)
		stub_put32(&test_wire, 0U);
	stub_put64(&test_wire, 0U);
	stub_put64(&test_wire, 0U);
	stub_put64(&test_wire, 0U);
	stub_put64(&test_wire, 0U);
	stub_put64(&test_wire, 0U);
	stub_put64(&test_wire, 0U);
	stub_put32(&test_wire, 0U);
	stub_put64(&test_wire, 0U);
	stub_put32(&test_wire, 0U);
	stub_put64(&test_wire, 0U);
	stub_put64(&test_wire, 1U);
	stub_put64(&test_wire, identity);
}

/* Runs the stream of a create and returns its VkResult. */
static uint32_t
test_create_result(void)
{
	(void)stub_execute_ok(&test_wire);
	return stub_get32(stub_reply, 4U);
}

/*
 * A compute pipeline from a compute module is compiled and published at the
 * compute bind point; one from a vertex module, one with specialization
 * constants and one naming an unknown module fail their create and publish
 * nothing, and nothing stays allocated once the session closes.
 */
static void
test_pipelines(void)
{
	struct i915_gfx_pipeline *pipeline;

	stub_session_open(NULL);
	test_module("add.spv", TEST_ADD);
	test_module("cuboid.vert.spv", TEST_VS);

	/* The add shader: 64 invocations in 8 threads, all eight channels of the last. */
	test_compute_pipeline(TEST_ADD, TEST_ADD_PIPELINE, 0);
	assert(test_create_result() == VK_SUCCESS);
	pipeline = drv_i915_object_lookup(stub_session, I915_VK_OBJ_PIPELINE, TEST_ADD_PIPELINE);
	assert(pipeline != NULL);
	assert(pipeline->bind_point == VK_PIPELINE_BIND_POINT_COMPUTE);
	assert(pipeline->kernels_ready != 0);
	assert(pipeline->cs_binary != NULL && pipeline->cs_binary->stage == DRV_GPU_STAGE_COMPUTE);
	assert(pipeline->vs_binary == NULL && pipeline->fs_binary == NULL);
	assert(pipeline->threads == 8U && pipeline->right_mask == 0xffU);
	assert(pipeline->cs_binary->local_size[0] == 64U);
	printf("  compute pipeline: add.comp compiled at the compute bind point, 8 threads of 8 channels\n");

	/* A vertex module as the compute stage fails the create. */
	test_compute_pipeline(TEST_VS, TEST_BAD_PIPELINE, 0);
	assert(test_create_result() != VK_SUCCESS);
	assert(drv_i915_object_lookup(stub_session, I915_VK_OBJ_PIPELINE, TEST_BAD_PIPELINE) == NULL);
	assert(stub_get64(stub_reply, 16U) == 0U);

	/* Specialization constants fail the create. */
	test_compute_pipeline(TEST_ADD, TEST_BAD_PIPELINE, 1);
	assert(test_create_result() != VK_SUCCESS);
	assert(drv_i915_object_lookup(stub_session, I915_VK_OBJ_PIPELINE, TEST_BAD_PIPELINE) == NULL);

	/* An unknown module fails the create. */
	test_compute_pipeline(0x12345ULL, TEST_BAD_PIPELINE, 0);
	assert(test_create_result() != VK_SUCCESS);
	assert(drv_i915_object_lookup(stub_session, I915_VK_OBJ_PIPELINE, TEST_BAD_PIPELINE) == NULL);
	printf("  compute pipeline: a vertex module, specialization constants and an unknown module each fail the create and publish nothing\n");

	/* Closes the session: the pipeline, its kernel and its table go with it. */
	stub_session_close();
	assert(stub_live == 0U);
	printf("  compute pipeline: nothing stays allocated after the session closes\n");
}

/*
 * The table of a (4, 2, 3) group: 24 invocations in 3 threads, each
 * channel's local IDs and linear index as the x-major order gives them.
 */
static void
test_thread_ids(void)
{
	struct i915_gfx_pipeline *pipeline;
	const uint32_t *table;
	uint32_t thread;
	uint32_t channel;
	uint32_t linear;

	stub_session_open(NULL);
	test_module("ids.spv", TEST_IDS);
	test_compute_pipeline(TEST_IDS, TEST_IDS_PIPELINE, 0);
	assert(test_create_result() == VK_SUCCESS);
	pipeline = drv_i915_object_lookup(stub_session, I915_VK_OBJ_PIPELINE, TEST_IDS_PIPELINE);
	assert(pipeline != NULL);
	assert(pipeline->threads == 3U && pipeline->right_mask == 0xffU);
	table = pipeline->thread_ids;
	for (thread = 0U; thread < 3U; thread++) {
		for (channel = 0U; channel < 8U; channel++) {
			linear = thread * 8U + channel;
			assert(table[thread * 32U + channel] == linear % 4U);
			assert(table[thread * 32U + 8U + channel] == (linear / 4U) % 2U);
			assert(table[thread * 32U + 16U + channel] == linear / 8U);
			assert(table[thread * 32U + 24U + channel] == linear);
		}
	}
	stub_session_close();
	assert(stub_live == 0U);
	printf("  thread IDs: a 4 x 2 x 3 group in 3 threads, every channel's x, y, z and index\n");
}

/*
 * A compute module used as the stages of a graphics pipeline is refused
 * (the parser takes the stage from the module's entry point).
 */
static void
test_stage_mismatch(void)
{
	stub_session_open(NULL);
	test_module("add.spv", TEST_ADD);
	test_module("cuboid.frag.spv", TEST_FS);
	test_module("cuboid.vert.spv", TEST_VS);

	/*
	 * The compute module as both stages fails the create.  Without the
	 * stage check it would compile twice as a compute kernel whose
	 * interfaces agree (no varying on either side) and be published.
	 */
	test_graphics_pipeline(TEST_ADD, TEST_ADD, TEST_BAD_PIPELINE);
	assert(test_create_result() != VK_SUCCESS);
	assert(drv_i915_object_lookup(stub_session, I915_VK_OBJ_PIPELINE, TEST_BAD_PIPELINE) == NULL);

	/* The vkdemo pipeline still builds, a graphics pipeline at the graphics bind point. */
	test_graphics_pipeline(TEST_VS, TEST_FS, TEST_ADD_PIPELINE);
	assert(test_create_result() == VK_SUCCESS);
	assert(((struct i915_gfx_pipeline *)drv_i915_object_lookup(stub_session, I915_VK_OBJ_PIPELINE, TEST_ADD_PIPELINE))->bind_point == VK_PIPELINE_BIND_POINT_GRAPHICS);
	stub_session_close();

	/*
	 * A failed vkCreateGraphicsPipelines neither releases nor frees its
	 * pipeline (pipeline.c, an XXX of the graphics path, not of this
	 * Phase); the blocks it lost are reclaimed so the next test starts clean.
	 */
	(void)stub_release_since(0UL);
	printf("  stage check: a compute module as the vertex and fragment stages is refused; the vkdemo pipeline still builds\n");
}

/*
 * Recording: vkCmdBindPipeline of a compute pipeline, vkCmdBindDescriptorSets
 * at the compute bind point and vkCmdDispatch are recorded as they came; a
 * compute pipeline and set bind at the compute bind point only, and a
 * dispatch needs a compute pipeline, runs nothing for zero groups and
 * otherwise goes to drv_i915_gfx_dispatch() with its groups (ws101-p004; the
 * stand-in records the call).
 */
static void
test_recording(void)
{
	struct i915_gfx_draw_state state;
	struct i915_gfx_pipeline *compute;
	struct i915_gfx_pipeline graphics;
	struct i915_gfx_cmdbuf *cmdbuf;
	struct i915_gfx_op op;
	struct i915_gfx_dset set;
	struct i915_gem_object object;
	struct i915_gfx_memory memory;
	struct i915_gfx_buffer buffer;
	unsigned long mark;
	int error;

	mark = stub_allocation_mark();
	stub_session_open(NULL);
	test_module("add.spv", TEST_ADD);
	test_compute_pipeline(TEST_ADD, TEST_ADD_PIPELINE, 0);
	assert(test_create_result() == VK_SUCCESS);
	compute = drv_i915_object_lookup(stub_session, I915_VK_OBJ_PIPELINE, TEST_ADD_PIPELINE);

	/* A pool, a command buffer, and its begin. */
	stub_wire_begin(&test_wire);
	stub_put32(&test_wire, TEST_CREATE_COMMAND_POOL);
	stub_put32(&test_wire, 1U);
	stub_put64(&test_wire, TEST_DEVICE);
	stub_put64(&test_wire, 1U);
	stub_put32(&test_wire, 39U);
	stub_put64(&test_wire, 0U);
	stub_put32(&test_wire, 0U);
	stub_put32(&test_wire, 0U);
	stub_put64(&test_wire, 0U);
	stub_put64(&test_wire, 1U);
	stub_put64(&test_wire, TEST_POOL);
	stub_put32(&test_wire, TEST_ALLOCATE_COMMAND_BUFFERS);
	stub_put32(&test_wire, 1U);
	stub_put64(&test_wire, TEST_DEVICE);
	stub_put64(&test_wire, 1U);
	stub_put32(&test_wire, 40U);
	stub_put64(&test_wire, 0U);
	stub_put64(&test_wire, TEST_POOL);
	stub_put32(&test_wire, VK_COMMAND_BUFFER_LEVEL_PRIMARY);
	stub_put32(&test_wire, 1U);
	stub_put64(&test_wire, 1U);
	stub_put64(&test_wire, TEST_CB);
	stub_put32(&test_wire, TEST_BEGIN_COMMAND_BUFFER);
	stub_put32(&test_wire, 1U);
	stub_put64(&test_wire, TEST_CB);
	stub_put64(&test_wire, 1U);
	stub_put32(&test_wire, 42U);
	stub_put64(&test_wire, 0U);
	stub_put32(&test_wire, 0U);
	stub_put64(&test_wire, 0U);
	(void)stub_execute_ok(&test_wire);

	/* Records: the compute pipeline, one set at the compute bind point, a 4 x 2 x 1 dispatch; then ends. */
	stub_wire_begin(&test_wire);
	stub_put32(&test_wire, TEST_CMD_BIND_PIPELINE);
	stub_put32(&test_wire, 0U);
	stub_put64(&test_wire, TEST_CB);
	stub_put32(&test_wire, VK_PIPELINE_BIND_POINT_COMPUTE);
	stub_put64(&test_wire, TEST_ADD_PIPELINE);
	stub_put32(&test_wire, TEST_CMD_BIND_DESCRIPTOR_SETS);
	stub_put32(&test_wire, 0U);
	stub_put64(&test_wire, TEST_CB);
	stub_put32(&test_wire, VK_PIPELINE_BIND_POINT_COMPUTE);
	stub_put64(&test_wire, 0U);
	stub_put32(&test_wire, 1U);
	stub_put32(&test_wire, 1U);
	stub_put64(&test_wire, 1U);
	stub_put64(&test_wire, TEST_SET);
	stub_put32(&test_wire, 0U);
	stub_put64(&test_wire, 0U);
	stub_put32(&test_wire, TEST_CMD_DISPATCH);
	stub_put32(&test_wire, 0U);
	stub_put64(&test_wire, TEST_CB);
	stub_put32(&test_wire, 4U);
	stub_put32(&test_wire, 2U);
	stub_put32(&test_wire, 1U);
	stub_put32(&test_wire, TEST_END_COMMAND_BUFFER);
	stub_put32(&test_wire, 1U);
	stub_put64(&test_wire, TEST_CB);
	(void)stub_execute_ok(&test_wire);

	/* The operations as they came. */
	cmdbuf = drv_i915_object_lookup(stub_session, I915_VK_OBJ_COMMAND_BUFFER, TEST_CB);
	assert(cmdbuf != NULL);
	assert(cmdbuf->op_count == 3U);
	assert(cmdbuf->ops[0].kind == I915_GFX_OP_BIND_PIPELINE && cmdbuf->ops[0].u.pipeline == compute);
	assert(cmdbuf->ops[1].kind == I915_GFX_OP_BIND_DESCRIPTOR_SET);
	assert(cmdbuf->ops[1].u.descriptor.bind_point == VK_PIPELINE_BIND_POINT_COMPUTE && cmdbuf->ops[1].u.descriptor.set == 1U);
	assert(cmdbuf->ops[2].kind == I915_GFX_OP_DISPATCH);
	assert(cmdbuf->ops[2].u.dispatch.groups[0] == 4U && cmdbuf->ops[2].u.dispatch.groups[1] == 2U && cmdbuf->ops[2].u.dispatch.groups[2] == 1U);
	printf("  recording: the compute pipeline, a set at the compute bind point and a 4 x 2 x 1 dispatch are recorded\n");

	/* Binding keeps the bind points apart. */
	memset(&state, 0, sizeof(state));
	memset(&graphics, 0, sizeof(graphics));
	memset(&set, 0, sizeof(set));
	state.pipeline = &graphics;
	i915_execute_bind_pipeline(&state, &cmdbuf->ops[0]);
	assert(state.pipeline == &graphics && state.compute_pipeline == compute);
	memset(&op, 0, sizeof(op));
	op.kind = I915_GFX_OP_BIND_DESCRIPTOR_SET;
	op.u.descriptor.bind_point = VK_PIPELINE_BIND_POINT_COMPUTE;
	op.u.descriptor.set = 2U;
	op.u.descriptor.dset = &set;
	i915_execute_bind_set(&state, &op);
	assert(state.compute_dset[2] == &set && state.dset[2] == NULL);
	op.u.descriptor.bind_point = VK_PIPELINE_BIND_POINT_GRAPHICS;
	op.u.descriptor.set = 3U;
	i915_execute_bind_set(&state, &op);
	assert(state.dset[3] == &set && state.compute_dset[3] == NULL);
	printf("  bind points: a compute pipeline and set leave the graphics ones bound, and the reverse\n");

	/* A dispatch: refused without a compute pipeline, nothing for zero groups, recorded otherwise. */
	memset(&op, 0, sizeof(op));
	op.kind = I915_GFX_OP_DISPATCH;
	op.u.dispatch.groups[0] = 1U;
	op.u.dispatch.groups[1] = 1U;
	op.u.dispatch.groups[2] = 1U;
	state.compute_pipeline = NULL;
	error = i915_execute_dispatch(stub_session, &state, &op);
	assert(error == EINVAL);
	state.compute_pipeline = compute;
	op.u.dispatch.groups[1] = I915_GFX_MAX_GROUP_COUNT + 1U;
	error = i915_execute_dispatch(stub_session, &state, &op);
	assert(error == EINVAL);
	stub_dispatch_calls = 0U;
	op.u.dispatch.groups[1] = 0U;
	error = i915_execute_dispatch(stub_session, &state, &op);
	assert(error == 0 && stub_dispatch_calls == 0U);
	op.u.dispatch.groups[1] = 1U;
	error = i915_execute_dispatch(stub_session, &state, &op);
	assert(error == 0 && stub_dispatch_calls == 1U);

	/* The whole command buffer runs its dispatch of 4 x 2 x 1 groups. */
	error = i915_command_buffer_execute(stub_session, cmdbuf);
	assert(error == 0 && stub_dispatch_calls == 2U);
	assert(stub_last_dispatch[0] == 4U && stub_last_dispatch[1] == 2U && stub_last_dispatch[2] == 1U);
	assert(stub_last_indirect_va == 0U);

	/*
	 * An indirect dispatch (ws101-p007): refused without a compute
	 * pipeline, for an unbound buffer, an offset that is not a word's or
	 * counts past the buffer's end; otherwise dispatched at the counts'
	 * address, whatever they hold.
	 */
	memset(&object, 0, sizeof(object));
	memset(&memory, 0, sizeof(memory));
	memset(&buffer, 0, sizeof(buffer));
	object.va = 0x0000000500000000ULL;
	memory.object = &object;
	memory.size = 0x10000U;
	buffer.size = 64U;
	buffer.offset = 0x100U;
	memset(&op, 0, sizeof(op));
	op.kind = I915_GFX_OP_DISPATCH_INDIRECT;
	op.u.dispatch_indirect.buffer = &buffer;
	op.u.dispatch_indirect.offset = 16U;
	state.compute_pipeline = NULL;
	error = i915_execute_dispatch_indirect(stub_session, &state, &op);
	assert(error == EINVAL);
	state.compute_pipeline = compute;
	error = i915_execute_dispatch_indirect(stub_session, &state, &op);
	assert(error == EINVAL);
	buffer.memory = &memory;
	op.u.dispatch_indirect.offset = 18U;
	error = i915_execute_dispatch_indirect(stub_session, &state, &op);
	assert(error == EINVAL);
	op.u.dispatch_indirect.offset = 56U;
	error = i915_execute_dispatch_indirect(stub_session, &state, &op);
	assert(error == EINVAL);
	op.u.dispatch_indirect.offset = 0xfffffffffffffff0ULL;
	error = i915_execute_dispatch_indirect(stub_session, &state, &op);
	assert(error == EINVAL);
	assert(stub_dispatch_calls == 2U);
	op.u.dispatch_indirect.offset = 52U;
	error = i915_execute_dispatch_indirect(stub_session, &state, &op);
	assert(error == 0 && stub_dispatch_calls == 3U);
	assert(stub_last_indirect_va == 0x0000000500000000ULL + 0x100U + 52U);
	printf("  indirect dispatch: refused without a pipeline, a bound buffer, a word's offset or room for three counts; dispatched at the counts' address\n");
	stub_session_close();
	assert(stub_live_since(mark) == 0U);
	printf("  dispatch: refused without a compute pipeline or past 65535 groups, nothing for zero groups, otherwise dispatched with its groups\n");
}
