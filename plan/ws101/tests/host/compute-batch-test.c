/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * ws101-p004: a dispatch's slot and commands (render/compute.c), on the
 * host.
 *
 * Compute pipelines are made through the wire from add.spv (push constants
 * and three storage buffers, 64 invocations) and ids.spv (the group counts
 * and one storage buffer, a 4 x 2 x 3 group).  drv_i915_gfx_dispatch_write()
 * then fills a slot from a compute bind point whose sets are made by hand,
 * and drv_i915_gfx_dispatch_build() appends the commands.  The slot is
 * checked here word by word; the batch and the interface descriptor are
 * written to files with the values their fields must hold, which
 * genxml-check.py decodes with Mesa's genxml and compares field by field
 * (plan/ws101/tests/host/run.sh).
 *
 *   compute-batch-test DIR OUT      DIR holds add.spv, ids.spv and reduce.spv; OUT-add, OUT-ids, OUT-indirect and OUT-reduce .bin,
 *                                   .idd and .expect are written
 */

#include "../../../ws031/tests/i915-vk-render-stubs.inc"

#include "../../../../src/drivers/gpu/i915/compiler/compiler.h"
#include "../../../../src/drivers/gpu/i915/render/batch.h"
#include "../../../../src/drivers/gpu/i915/render/heap.h"
#include "../../../../src/drivers/gpu/i915/render/state.h"

/* The stub file stands in for the dispatch; the one under test is compute.c's, under another name. */
#define drv_i915_gfx_dispatch test_dispatch_under_test
#include "../../../../src/drivers/gpu/i915/render/compute.c"

/* The wire opcodes the test sends. */
#define TEST_CREATE_SHADER_MODULE	59U
#define TEST_CREATE_COMPUTE_PIPELINES	66U

/* The identities, and where the test's objects are in the GPU's address space. */
#define TEST_DEVICE		0xd0ULL
#define TEST_MODULE		0xd00ULL
#define TEST_PIPELINE		0xe00ULL
#define TEST_SLOT_VA		0x0000000123450000ULL
#define TEST_WINDOW_VA		0x0000000067800000ULL
#define TEST_SCRATCH_VA		0x0000000089a00000ULL
#define TEST_BUFFER_VA		0x0000000400000000ULL
#define TEST_INDIRECT_VA	0x0000000500000040ULL

/* The stream every command is built in. */
static struct stub_wire test_wire;

/* The directory of the modules, and the output's base name. */
static const char *test_dir;
static const char *test_out;

static struct i915_gfx_pipeline *test_pipeline(const char *name);
static void test_bind(struct i915_gfx_draw_state *state, struct i915_gfx_dset *set, struct i915_gfx_buffer *buffers, struct i915_gfx_memory *memory, struct i915_gem_object *object);
static void test_add(void);
static void test_ids(void);
static void test_ids_indirect(void);
static void test_reduce(void);

/* Stand-ins for the parts of draw.c compute.c calls: the host has no GPU session. */
int
drv_i915_gfx_flush(
	struct i915_render_session *session,
	struct i915_gfx_session *work)
{
	UNUSED_PARAMETER(session);
	UNUSED_PARAMETER(work);
	return 0;
}

int
drv_i915_gfx_scratch(
	struct i915_render_session *session,
	struct i915_gfx_session *work,
	struct i915_gfx_kernels *kernels)
{
	UNUSED_PARAMETER(session);
	UNUSED_PARAMETER(work);
	UNUSED_PARAMETER(kernels);
	return 0;
}

int
drv_i915_gfx_window(
	struct i915_render_session *session,
	struct i915_gfx_session *work,
	const void **owner,
	uint32_t *window,
	uint32_t *generation,
	const uint32_t *vs_code,
	uint32_t vs_bytes,
	const uint32_t *gs_code,
	uint32_t gs_bytes,
	const uint32_t *ps_code,
	uint32_t ps_bytes,
	struct i915_gfx_op_space *space)
{
	UNUSED_PARAMETER(session);
	UNUSED_PARAMETER(work);
	UNUSED_PARAMETER(owner);
	UNUSED_PARAMETER(window);
	UNUSED_PARAMETER(generation);
	UNUSED_PARAMETER(vs_code);
	UNUSED_PARAMETER(vs_bytes);
	UNUSED_PARAMETER(gs_code);
	UNUSED_PARAMETER(gs_bytes);
	UNUSED_PARAMETER(ps_code);
	UNUSED_PARAMETER(ps_bytes);
	UNUSED_PARAMETER(space);
	return ENOMEM;
}

int
main(
	int argc,
	char **argv)
{
	assert(argc == 3);
	test_dir = argv[1];
	test_out = argv[2];

	test_add();
	test_ids();
	test_ids_indirect();
	test_reduce();

	printf("ws101 compute batch host test PASS\n");
	return 0;
}

/* Makes a compute pipeline from DIR/NAME through the wire, in the open session. */
static struct i915_gfx_pipeline *
test_pipeline(
	const char *name)
{
	char path[512];
	uint32_t *code;
	size_t words;
	size_t index;
	size_t read;
	FILE *file;
	long size;

	/* Reads the module. */
	snprintf(path, sizeof(path), "%s/%s", test_dir, name);
	file = fopen(path, "rb");
	assert(file != NULL);
	fseek(file, 0, SEEK_END);
	size = ftell(file);
	fseek(file, 0, SEEK_SET);
	assert(size > 0 && (size % 4) == 0);
	code = malloc((size_t)size);
	assert(code != NULL);
	read = fread(code, 1U, (size_t)size, file);
	assert(read == (size_t)size);
	fclose(file);
	words = (size_t)size / 4U;

	/* vkCreateShaderModule. */
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
	stub_put64(&test_wire, TEST_MODULE);
	(void)stub_execute_ok(&test_wire);
	free(code);

	/* vkCreateComputePipelines of one pipeline, entry "main", no specialization. */
	stub_wire_begin(&test_wire);
	stub_put32(&test_wire, TEST_CREATE_COMPUTE_PIPELINES);
	stub_put32(&test_wire, 1U);
	stub_put64(&test_wire, TEST_DEVICE);
	stub_put64(&test_wire, 0U);
	stub_put32(&test_wire, 1U);
	stub_put64(&test_wire, 1U);
	stub_put32(&test_wire, 29U);
	stub_put64(&test_wire, 0U);
	stub_put32(&test_wire, 0U);
	stub_put32(&test_wire, 18U);
	stub_put64(&test_wire, 0U);
	stub_put32(&test_wire, 0U);
	stub_put32(&test_wire, VK_SHADER_STAGE_COMPUTE_BIT);
	stub_put64(&test_wire, TEST_MODULE);
	stub_put64(&test_wire, 5U);
	stub_put32(&test_wire, 0x6e69616dU);
	stub_put32(&test_wire, 0U);
	stub_put64(&test_wire, 0U);
	stub_put64(&test_wire, 0U);
	stub_put64(&test_wire, 0U);
	stub_put32(&test_wire, 0U);
	stub_put64(&test_wire, 0U);
	stub_put64(&test_wire, 1U);
	stub_put64(&test_wire, TEST_PIPELINE);
	(void)stub_execute_ok(&test_wire);
	assert(stub_get32(stub_reply, 4U) == VK_SUCCESS);

	/* Succeeded: the pipeline. */
	return drv_i915_object_lookup(stub_session, I915_VK_OBJ_PIPELINE, TEST_PIPELINE);
}

/*
 * Binds set 0 at the compute bind point: binding n a storage buffer of 4 KiB
 * at TEST_BUFFER_VA + 64 KiB n, its range 1 KiB from byte 256 n, for the
 * first four bindings.
 */
static void
test_bind(
	struct i915_gfx_draw_state *state,
	struct i915_gfx_dset *set,
	struct i915_gfx_buffer *buffers,
	struct i915_gfx_memory *memory,
	struct i915_gem_object *object)
{
	uint32_t binding;

	memset(set, 0, sizeof(*set));
	memset(object, 0, sizeof(*object));
	memset(memory, 0, sizeof(*memory));
	object->va = TEST_BUFFER_VA;
	memory->object = object;
	memory->size = 0x100000U;
	for (binding = 0U; binding < 4U; binding++) {
		memset(&buffers[binding], 0, sizeof(buffers[binding]));
		buffers[binding].size = 4096U;
		buffers[binding].memory = memory;
		buffers[binding].offset = 0x10000U * binding;
		set->slots[binding].buffer = &buffers[binding];
		set->slots[binding].offset = 256U * binding;
		set->slots[binding].range = 1024U;
	}
	state->compute_dset[0] = set;
}

/* Writes the batch, the interface descriptor and the fields they must hold, for genxml-check.py. */
static void
test_write(
	const char *name,
	const struct i915_gfx_batch *batch,
	const uint8_t *slot,
	const char *expect)
{
	char path[512];
	FILE *file;

	snprintf(path, sizeof(path), "%s-%s.idd", test_out, name);
	file = fopen(path, "wb");
	assert(file != NULL);
	fwrite(slot + I915_GFX_DYNAMIC_HEAP + I915_GFX_DYN_INTERFACE, 4U, GEN12_INTERFACE_DESCRIPTOR_DWORDS, file);
	fclose(file);

	snprintf(path, sizeof(path), "%s-%s.bin", test_out, name);
	file = fopen(path, "wb");
	assert(file != NULL);
	fwrite(batch->cmds, 4U, batch->count, file);
	fclose(file);
	snprintf(path, sizeof(path), "%s-%s.expect", test_out, name);
	file = fopen(path, "w");
	assert(file != NULL);
	fputs(expect, file);
	fclose(file);
}

/*
 * add.comp: the push constant n, three storage buffers at set 0 bindings 0,
 * 1 and 2, a group of 64 invocations in 8 threads.
 */
static void
test_add(void)
{
	static uint8_t slot[I915_GFX_SLOT_BYTES];
	static uint32_t cmds[1024];
	struct i915_gfx_draw_state state;
	struct i915_gfx_dset set;
	struct i915_gfx_buffer buffers[4];
	struct i915_gfx_memory memory;
	struct i915_gem_object object;
	struct i915_gfx_pipeline *pipeline;
	const struct i915_shader_binary *binary;
	struct i915_gfx_op_space space;
	struct i915_gfx_kernels kernels;
	struct i915_gfx_batch batch;
	const struct i915_gfx_grid grid = { {16U, 1U, 1U}, 0U };
	const uint32_t *dynamic;
	const uint32_t *curbe;
	const uint32_t *words;
	uint32_t index;
	uint32_t binding;
	uint32_t push;
	uint32_t curbe_regs;
	char expect[2048];
	int error;

	stub_session_open(NULL);
	pipeline = test_pipeline("add.spv");
	assert(pipeline != NULL && pipeline->threads == 8U);
	binary = pipeline->cs_binary;
	assert(binary->block_count == 3U);

	/* The state: the pipeline, the sets, and n = 1000 in the push constants. */
	memset(&state, 0, sizeof(state));
	state.compute_pipeline = pipeline;
	test_bind(&state, &set, buffers, &memory, &object);
	push = 1000U;
	memcpy(state.push, &push, 4U);

	/* Writes the slot. */
	memset(slot, 0xAB, sizeof(slot));
	error = drv_i915_gfx_dispatch_write(slot, TEST_SLOT_VA, &state, &grid);
	assert(error == 0);
	dynamic = (const uint32_t *)(const void *)(slot + I915_GFX_DYNAMIC_HEAP);

	/* The interface descriptor: kernel at 0, preemption off, 4 per-thread registers, 8 threads, the cross-thread registers. */
	assert(dynamic[0] == 0U && dynamic[1] == 0U);
	assert(dynamic[2] == (1U << 20));
	assert(dynamic[3] == 0U && dynamic[4] == 0U);
	assert(dynamic[5] == (4U << 16));
	assert(dynamic[6] == 8U);
	assert(dynamic[7] == binary->cross_thread_regs);

	/* The group counts. */
	assert(dynamic[I915_GFX_DYN_GROUP_COUNTS / 4U] == 16U);
	assert(dynamic[I915_GFX_DYN_GROUP_COUNTS / 4U + 1U] == 1U);
	assert(dynamic[I915_GFX_DYN_GROUP_COUNTS / 4U + 2U] == 1U);

	/* The CURBE: n first, then each buffer's address and range where its block goes. */
	curbe = (const uint32_t *)(const void *)(slot + I915_GFX_DYNAMIC_HEAP + I915_GFX_DYN_CURBE);
	assert(curbe[0] == 1000U);
	for (index = 0U; index < binary->block_count; index++) {
		binding = binary->blocks[index].binding;
		words = curbe + binary->blocks[index].push_offset / 4U;
		assert(binary->blocks[index].address != 0U && binary->blocks[index].set == 0U);
		assert(words[0] == (uint32_t)(TEST_BUFFER_VA + 0x10000U * binding + 256U * binding));
		assert(words[1] == (uint32_t)((TEST_BUFFER_VA + 0x10000U * binding + 256U * binding) >> 32));
		assert(words[2] == 1024U);
	}

	/* Each thread's IDs after the cross-thread data: thread t, channel c is invocation 8 t + c. */
	for (index = 0U; index < 8U * 8U; index++) {
		words = curbe + binary->cross_thread_regs * 8U + (index / 8U) * 32U;
		assert(words[index % 8U] == index);
		assert(words[8U + index % 8U] == 0U);
		assert(words[24U + index % 8U] == index);
	}
	printf("  add slot: descriptor, group counts, n, three storage addresses with their ranges, 8 threads of IDs\n");

	/* VK_WHOLE_SIZE reaches the buffer's end, and a range past it is cut there (what OpArrayLength reads). */
	set.slots[1].range = VK_WHOLE_SIZE;
	set.slots[2].range = 8192U;
	error = drv_i915_gfx_dispatch_write(slot, TEST_SLOT_VA, &state, &grid);
	assert(error == 0);
	for (index = 0U; index < binary->block_count; index++) {
		binding = binary->blocks[index].binding;
		words = curbe + binary->blocks[index].push_offset / 4U;
		if (binding == 0U)
			assert(words[2] == 1024U);
		else
			assert(words[2] == 4096U - 256U * binding);
	}
	printf("  add slot: a whole-size range and one past the buffer end at the buffer's end\n");

	/* The commands. */
	memset(&space, 0, sizeof(space));
	space.slot_va = TEST_SLOT_VA;
	space.window_va = TEST_WINDOW_VA;
	memset(&kernels, 0, sizeof(kernels));
	batch.cmds = cmds;
	batch.count = 0U;
	batch.capacity = 1024U;
	batch.overflow = 0;
	drv_i915_gfx_dispatch_build(&batch, &space, pipeline, &kernels, &grid, 6U, GEN12_MOCS(I915_MOCS_UNCACHED_INDEX));
	assert(batch.overflow == 0);
	curbe_regs = binary->cross_thread_regs + 32U;
	snprintf(expect, sizeof(expect),
		 "first PIPELINE_SELECT PipelineSelection=0\n"
		 "last PIPELINE_SELECT PipelineSelection=0\n"
		 "STATE_BASE_ADDRESS DynamicStateBaseAddress=0x%llx InstructionBaseAddress=0x%llx GeneralStateBaseAddress=0\n"
		 "exact MEDIA_VFE_STATE MaximumNumberofThreads=671 NumberofURBEntries=2 URBEntryAllocationSize=2 CURBEAllocationSize=%u\n"
		 "exact MEDIA_CURBE_LOAD CURBETotalDataLength=%u CURBEDataStartAddress=%u\n"
		 "exact MEDIA_INTERFACE_DESCRIPTOR_LOAD InterfaceDescriptorTotalLength=32 InterfaceDescriptorDataStartAddress=0\n"
		 "exact GPGPU_WALKER ThreadWidthCounterMaximum=7 ThreadGroupIDXDimension=16 ThreadGroupIDYDimension=1 ThreadGroupIDZDimension=1 RightExecutionMask=0xff BottomExecutionMask=0xffffffff\n"
		 "exact MEDIA_STATE_FLUSH\n"
		 "tail PIPELINE_SELECT PIPE_CONTROL MEDIA_VFE_STATE MEDIA_STATE_FLUSH MEDIA_CURBE_LOAD MEDIA_INTERFACE_DESCRIPTOR_LOAD GPGPU_WALKER MEDIA_STATE_FLUSH PIPE_CONTROL PIPELINE_SELECT\n"
		 "before-gpgpu PIPE_CONTROL CommandStreamerStallEnable=1 RenderTargetCacheFlushEnable=1 DepthCacheFlushEnable=1 HDCPipelineFlushEnable=1\n"
		 "after-gpgpu PIPE_CONTROL CommandStreamerStallEnable=1 StallAtPixelScoreboard=1\n"
		 "before-3d PIPE_CONTROL CommandStreamerStallEnable=1 DCFlushEnable=1 HDCPipelineFlushEnable=1 RenderTargetCacheFlushEnable=0 PipeControlFlushEnable=1\n"
		 "idd ThreadPreemptionDisable=1 ConstantURBEntryReadLength=4 NumberofThreadsinGPGPUThreadGroup=8 CrossThreadConstantDataReadLength=%u\n",
		 (unsigned long long)(TEST_SLOT_VA + I915_GFX_DYNAMIC_HEAP),
		 (unsigned long long)TEST_WINDOW_VA,
		 (curbe_regs + 1U) & ~1U,
		 ((curbe_regs + 1U) & ~1U) * 32U,
		 I915_GFX_DYN_CURBE,
		 binary->cross_thread_regs);
	test_write("add", &batch, slot, expect);
	printf("  add batch: %u dwords written for genxml-check.py\n", batch.count);

	stub_session_close();
}

/*
 * ids.comp: the group counts (the system storage buffer) and one storage
 * buffer, a 4 x 2 x 3 group in 3 threads; its commands are built as for a
 * kernel of 4 KiB of scratch a thread, on 4 dual-subslices.
 */
static void
test_ids(void)
{
	static uint8_t slot[I915_GFX_SLOT_BYTES];
	static uint32_t cmds[1024];
	struct i915_gfx_draw_state state;
	struct i915_gfx_dset set;
	struct i915_gfx_buffer buffers[4];
	struct i915_gfx_memory memory;
	struct i915_gem_object object;
	struct i915_gfx_pipeline *pipeline;
	const struct i915_shader_binary *binary;
	struct i915_gfx_op_space space;
	struct i915_gfx_kernels kernels;
	struct i915_gfx_batch batch;
	const struct i915_gfx_grid grid = { {3U, 2U, 5U}, 0U };
	const uint32_t *curbe;
	const uint32_t *words;
	uint32_t index;
	uint32_t systems;
	uint32_t curbe_regs;
	char expect[2048];
	int error;

	stub_session_open(NULL);
	pipeline = test_pipeline("ids.spv");
	assert(pipeline != NULL && pipeline->threads == 3U && pipeline->right_mask == 0xffU);
	binary = pipeline->cs_binary;

	memset(&state, 0, sizeof(state));
	state.compute_pipeline = pipeline;
	test_bind(&state, &set, buffers, &memory, &object);

	/* Writes the slot. */
	error = drv_i915_gfx_dispatch_write(slot, TEST_SLOT_VA, &state, &grid);
	assert(error == 0);

	/* The system buffer's register holds the address of the group counts in the slot, 12 bytes. */
	curbe = (const uint32_t *)(const void *)(slot + I915_GFX_DYNAMIC_HEAP + I915_GFX_DYN_CURBE);
	systems = 0U;
	for (index = 0U; index < binary->block_count; index++) {
		if (binary->blocks[index].set != DRV_GPU_IR_SYSTEM_SET)
			continue;
		words = curbe + binary->blocks[index].push_offset / 4U;
		assert(words[0] == (uint32_t)(TEST_SLOT_VA + I915_GFX_DYNAMIC_HEAP + I915_GFX_DYN_GROUP_COUNTS));
		assert(words[1] == (uint32_t)((TEST_SLOT_VA + I915_GFX_DYNAMIC_HEAP + I915_GFX_DYN_GROUP_COUNTS) >> 32));
		assert(words[2] == 12U);
		systems++;
	}
	assert(systems == 1U);
	printf("  ids slot: the system storage buffer points at the group counts in the slot\n");

	/* The commands, with a 4 KiB scratch space at 0x20000 of the scratch buffer. */
	memset(&space, 0, sizeof(space));
	space.slot_va = TEST_SLOT_VA;
	space.window_va = TEST_WINDOW_VA;
	memset(&kernels, 0, sizeof(kernels));
	kernels.scratch_base = TEST_SCRATCH_VA;
	kernels.cs_scratch_bytes = 4096U;
	kernels.cs_scratch_offset = 0x20000U;
	batch.cmds = cmds;
	batch.count = 0U;
	batch.capacity = 1024U;
	batch.overflow = 0;
	drv_i915_gfx_dispatch_build(&batch, &space, pipeline, &kernels, &grid, 4U, GEN12_MOCS(I915_MOCS_UNCACHED_INDEX));
	assert(batch.overflow == 0);
	curbe_regs = binary->cross_thread_regs + 12U;
	snprintf(expect, sizeof(expect),
		 "STATE_BASE_ADDRESS GeneralStateBaseAddress=0x%llx\n"
		 "exact MEDIA_VFE_STATE MaximumNumberofThreads=447 NumberofURBEntries=2 URBEntryAllocationSize=2 CURBEAllocationSize=%u PerThreadScratchSpace=2 ScratchSpaceBasePointer=0x20000\n"
		 "exact GPGPU_WALKER ThreadWidthCounterMaximum=2 ThreadGroupIDXDimension=3 ThreadGroupIDYDimension=2 ThreadGroupIDZDimension=5 RightExecutionMask=0xff BottomExecutionMask=0xffffffff\n"
		 "idd ThreadPreemptionDisable=1 ConstantURBEntryReadLength=4 NumberofThreadsinGPGPUThreadGroup=3 CrossThreadConstantDataReadLength=%u\n",
		 (unsigned long long)TEST_SCRATCH_VA,
		 (curbe_regs + 1U) & ~1U,
		 binary->cross_thread_regs);
	test_write("ids", &batch, slot, expect);
	printf("  ids batch: %u dwords written for genxml-check.py\n", batch.count);

	stub_session_close();
}

/*
 * ids.comp dispatched indirectly (ws101-p007): the system storage buffer
 * points at the indirect buffer's three counts, and the commands load the
 * walker's registers from them before an indirect walker whose own counts
 * are zero.
 */
static void
test_ids_indirect(void)
{
	static uint8_t slot[I915_GFX_SLOT_BYTES];
	static uint32_t cmds[1024];
	struct i915_gfx_draw_state state;
	struct i915_gfx_dset set;
	struct i915_gfx_buffer buffers[4];
	struct i915_gfx_memory memory;
	struct i915_gem_object object;
	struct i915_gfx_pipeline *pipeline;
	const struct i915_shader_binary *binary;
	struct i915_gfx_op_space space;
	struct i915_gfx_kernels kernels;
	struct i915_gfx_batch batch;
	struct i915_gfx_grid grid;
	const uint32_t *curbe;
	const uint32_t *words;
	uint32_t index;
	uint32_t systems;
	char expect[2048];
	int error;

	stub_session_open(NULL);
	pipeline = test_pipeline("ids.spv");
	assert(pipeline != NULL);
	binary = pipeline->cs_binary;

	/* The counts at an address of the indirect buffer. */
	memset(&state, 0, sizeof(state));
	state.compute_pipeline = pipeline;
	test_bind(&state, &set, buffers, &memory, &object);
	memset(&grid, 0, sizeof(grid));
	grid.indirect_va = TEST_INDIRECT_VA;

	/* Writes the slot: the slot's own counts stay as they were cleared. */
	memset(slot, 0xAB, sizeof(slot));
	error = drv_i915_gfx_dispatch_write(slot, TEST_SLOT_VA, &state, &grid);
	assert(error == 0);
	words = (const uint32_t *)(const void *)(slot + I915_GFX_DYNAMIC_HEAP + I915_GFX_DYN_GROUP_COUNTS);
	assert(words[0] == 0U && words[1] == 0U && words[2] == 0U);

	/* The system buffer's register holds the indirect counts' address, 12 bytes. */
	curbe = (const uint32_t *)(const void *)(slot + I915_GFX_DYNAMIC_HEAP + I915_GFX_DYN_CURBE);
	systems = 0U;
	for (index = 0U; index < binary->block_count; index++) {
		if (binary->blocks[index].set != DRV_GPU_IR_SYSTEM_SET)
			continue;
		words = curbe + binary->blocks[index].push_offset / 4U;
		assert(words[0] == (uint32_t)TEST_INDIRECT_VA);
		assert(words[1] == (uint32_t)(TEST_INDIRECT_VA >> 32));
		assert(words[2] == 12U);
		systems++;
	}
	assert(systems == 1U);
	printf("  ids indirect slot: the system storage buffer points at the indirect counts\n");

	/* The commands. */
	memset(&space, 0, sizeof(space));
	space.slot_va = TEST_SLOT_VA;
	space.window_va = TEST_WINDOW_VA;
	memset(&kernels, 0, sizeof(kernels));
	batch.cmds = cmds;
	batch.count = 0U;
	batch.capacity = 1024U;
	batch.overflow = 0;
	drv_i915_gfx_dispatch_build(&batch, &space, pipeline, &kernels, &grid, 6U, GEN12_MOCS(I915_MOCS_UNCACHED_INDEX));
	assert(batch.overflow == 0);
	snprintf(expect, sizeof(expect),
		 "first MI_LOAD_REGISTER_MEM RegisterAddress=0x2500 MemoryAddress=0x%llx UseGlobalGTT=0\n"
		 "last MI_LOAD_REGISTER_MEM RegisterAddress=0x2508 MemoryAddress=0x%llx UseGlobalGTT=0\n"
		 "exact GPGPU_WALKER IndirectParameterEnable=1 ThreadWidthCounterMaximum=2 ThreadGroupIDXDimension=0 ThreadGroupIDYDimension=0 ThreadGroupIDZDimension=0 RightExecutionMask=0xff BottomExecutionMask=0xffffffff\n"
		 "tail PIPELINE_SELECT PIPE_CONTROL MEDIA_VFE_STATE MEDIA_STATE_FLUSH MEDIA_CURBE_LOAD MEDIA_INTERFACE_DESCRIPTOR_LOAD MI_LOAD_REGISTER_MEM MI_LOAD_REGISTER_MEM MI_LOAD_REGISTER_MEM GPGPU_WALKER MEDIA_STATE_FLUSH PIPE_CONTROL PIPELINE_SELECT\n",
		 (unsigned long long)TEST_INDIRECT_VA,
		 (unsigned long long)(TEST_INDIRECT_VA + 8U));
	test_write("indirect", &batch, slot, expect);
	printf("  ids indirect batch: %u dwords written for genxml-check.py\n", batch.count);

	stub_session_close();
}

/*
 * reduce.comp (ws101-p006): 512 bytes of shared memory and a barrier over a
 * group of 128 in 16 threads; the interface descriptor asks for 1 KiB of
 * shared local memory (the encoding 1) and the barrier.
 */
static void
test_reduce(void)
{
	static uint8_t slot[I915_GFX_SLOT_BYTES];
	static uint32_t cmds[1024];
	struct i915_gfx_draw_state state;
	struct i915_gfx_dset set;
	struct i915_gfx_buffer buffers[4];
	struct i915_gfx_memory memory;
	struct i915_gem_object object;
	struct i915_gfx_pipeline *pipeline;
	const struct i915_shader_binary *binary;
	struct i915_gfx_op_space space;
	struct i915_gfx_kernels kernels;
	struct i915_gfx_batch batch;
	const struct i915_gfx_grid grid = { {4U, 1U, 1U}, 0U };
	const uint32_t *dynamic;
	char expect[512];
	int error;

	stub_session_open(NULL);
	pipeline = test_pipeline("reduce.spv");
	assert(pipeline != NULL && pipeline->threads == 16U);
	binary = pipeline->cs_binary;
	assert(binary->shared_bytes == 512U && binary->uses_barrier == 1U);

	/* The slot: the descriptor's word 6 has the threads, 1 KiB of shared local memory and the barrier. */
	memset(&state, 0, sizeof(state));
	state.compute_pipeline = pipeline;
	test_bind(&state, &set, buffers, &memory, &object);
	error = drv_i915_gfx_dispatch_write(slot, TEST_SLOT_VA, &state, &grid);
	assert(error == 0);
	dynamic = (const uint32_t *)(const void *)(slot + I915_GFX_DYNAMIC_HEAP);
	assert(dynamic[6] == (16U | (1U << 16) | (1U << 21)));
	printf("  reduce slot: 16 threads, 1 KiB of shared local memory, the barrier\n");

	/* The commands. */
	memset(&space, 0, sizeof(space));
	space.slot_va = TEST_SLOT_VA;
	space.window_va = TEST_WINDOW_VA;
	memset(&kernels, 0, sizeof(kernels));
	batch.cmds = cmds;
	batch.count = 0U;
	batch.capacity = 1024U;
	batch.overflow = 0;
	drv_i915_gfx_dispatch_build(&batch, &space, pipeline, &kernels, &grid, 6U, GEN12_MOCS(I915_MOCS_UNCACHED_INDEX));
	assert(batch.overflow == 0);
	snprintf(expect, sizeof(expect),
		 "exact GPGPU_WALKER ThreadWidthCounterMaximum=15 ThreadGroupIDXDimension=4 ThreadGroupIDYDimension=1 ThreadGroupIDZDimension=1 RightExecutionMask=0xff BottomExecutionMask=0xffffffff\n"
		 "idd ThreadPreemptionDisable=1 ConstantURBEntryReadLength=4 NumberofThreadsinGPGPUThreadGroup=16 SharedLocalMemorySize=1 BarrierEnable=1 CrossThreadConstantDataReadLength=%u\n",
		 binary->cross_thread_regs);
	test_write("reduce", &batch, slot, expect);
	printf("  reduce batch: %u dwords written for genxml-check.py\n", batch.count);

	stub_session_close();
}
