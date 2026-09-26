/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Host fixture for command pools and buffers, recording and vkQueueSubmit
 * (render/command.c), and for the commands a draw ends its batch with
 * (render/state.c).
 *
 * A command buffer is recorded as a list of operations and runs at
 * vkQueueSubmit: clears and copies as GPU rectangles, draws as GPU draws.
 * The GPU runs are the stand-ins of i915-vk-render-stubs.inc, which record
 * what they were asked to run, so the fixture checks what the recording
 * handed to the GPU path and in which order: the draws and indexed draws
 * with what was bound and set, and the buffer copies as the rectangles they
 * become.
 */

#include "i915-vk-render-stubs.inc"

#include "../../../src/drivers/gpu/i915/render/batch.h"
#include "../../../src/drivers/gpu/i915/render/heap.h"
#include "../../../src/drivers/gpu/i915/render/state.h"

#include "../../../src/drivers/gpu/i915/intel/commands.h"
#include "../../../src/drivers/gpu/i915/intel/genxml.h"

/* The wire opcodes the fixture sends, as libvulkan numbers them. */
#define FIXTURE_QUEUE_SUBMIT			18U
#define FIXTURE_ALLOCATE_MEMORY			21U
#define FIXTURE_FREE_MEMORY			22U
#define FIXTURE_BIND_BUFFER_MEMORY		28U
#define FIXTURE_BIND_IMAGE_MEMORY		29U
#define FIXTURE_CREATE_FENCE			35U
#define FIXTURE_DESTROY_FENCE			36U
#define FIXTURE_RESET_FENCES			37U
#define FIXTURE_GET_FENCE_STATUS		38U
#define FIXTURE_CREATE_BUFFER			50U
#define FIXTURE_DESTROY_BUFFER			51U
#define FIXTURE_CREATE_IMAGE			54U
#define FIXTURE_DESTROY_IMAGE			55U
#define FIXTURE_CREATE_DSL			72U
#define FIXTURE_DESTROY_DSL			73U
#define FIXTURE_CREATE_DESCRIPTOR_POOL		74U
#define FIXTURE_DESTROY_DESCRIPTOR_POOL		75U
#define FIXTURE_ALLOCATE_DESCRIPTOR_SETS	77U
#define FIXTURE_UPDATE_DESCRIPTOR_SETS		79U
#define FIXTURE_CREATE_COMMAND_POOL		85U
#define FIXTURE_DESTROY_COMMAND_POOL		86U
#define FIXTURE_RESET_COMMAND_POOL		87U
#define FIXTURE_ALLOCATE_COMMAND_BUFFERS	88U
#define FIXTURE_FREE_COMMAND_BUFFERS		89U
#define FIXTURE_BEGIN_COMMAND_BUFFER		90U
#define FIXTURE_END_COMMAND_BUFFER		91U
#define FIXTURE_CMD_BIND_PIPELINE		93U
#define FIXTURE_CMD_SET_VIEWPORT		94U
#define FIXTURE_CMD_SET_SCISSOR			95U
#define FIXTURE_CMD_SET_LINE_WIDTH		96U
#define FIXTURE_CMD_SET_BLEND_CONSTANTS		98U
#define FIXTURE_CMD_BIND_DESCRIPTOR_SETS	103U
#define FIXTURE_CMD_BIND_INDEX_BUFFER		104U
#define FIXTURE_CMD_BIND_VERTEX_BUFFERS		105U
#define FIXTURE_CMD_DRAW			106U
#define FIXTURE_CMD_DRAW_INDEXED		107U
#define FIXTURE_CMD_COPY_BUFFER			112U
#define FIXTURE_CMD_BLIT_IMAGE			114U
#define FIXTURE_CMD_COPY_BUFFER_TO_IMAGE	115U
#define FIXTURE_CMD_COPY_IMAGE_TO_BUFFER	116U
#define FIXTURE_CMD_CLEAR_COLOR_IMAGE		119U
#define FIXTURE_CMD_PUSH_CONSTANTS		132U

/* The wire identities the fixture gives its objects. */
#define FIXTURE_DEVICE		0xd0ULL
#define FIXTURE_QUEUE		0xd1ULL
#define FIXTURE_MEMORY		0x100ULL
#define FIXTURE_BUFFER		0x200ULL
#define FIXTURE_SRC_BUFFER	0x201ULL
#define FIXTURE_DST_BUFFER	0x202ULL
#define FIXTURE_IMAGE		0x300ULL
#define FIXTURE_MIP_IMAGE	0x301ULL
#define FIXTURE_PIPELINE	0xb00ULL
#define FIXTURE_POOL		0x900ULL
#define FIXTURE_CB0		0xa00ULL
#define FIXTURE_CB1		0xa01ULL
#define FIXTURE_FENCE		0xf00ULL
#define FIXTURE_DSL		0x600ULL
#define FIXTURE_DESCRIPTOR_POOL	0x700ULL
#define FIXTURE_SET		0x800ULL

/* The storage blob: its size and the GPU address it is bound at. */
#define FIXTURE_STORAGE_BYTES	262144U
#define FIXTURE_STORAGE_VA	0x200000000ULL

/* Where the copy buffers are bound in the storage, and their size. */
#define FIXTURE_SRC_OFFSET	65536U
#define FIXTURE_DST_OFFSET	131072U
#define FIXTURE_COPY_BYTES	65536U

/*
 * The mipmapped image of the transfer test: 32x32 with all 6 levels, bound
 * at 8192.  Its levels share 128-byte rows: level 1 at row 32, level 2 to
 * its right at column 16, levels 3, 4 and 5 below level 2 at rows 40, 44
 * and 48 (each level at least 4 rows).
 */
#define FIXTURE_MIP_OFFSET	8192U
#define FIXTURE_MIP_PITCH	128U

/* How many operations one command buffer records at most (render/command.c). */
#define FIXTURE_MAX_OPS		65536U

/* How many draws one stream of the fixture carries: each is 32 bytes. */
#define FIXTURE_DRAWS_PER_STREAM	2000U

/*
 * The storage libvulkan exports for the fixture's allocation, and the
 * session object that stands for that blob.
 */
static uint8_t fixture_storage[FIXTURE_STORAGE_BYTES] __attribute__((aligned(4096)));
static struct i915_gem_object fixture_storage_object;

/*
 * The pipeline the recordings bind.
 *
 * The draw is a stand-in, so the pipeline needs no kernels; it is published
 * under its identity for the whole lifecycle test and withdrawn at its end.
 */
static struct i915_gfx_pipeline fixture_pipeline;

/* The stream every command is built in. */
static struct stub_wire fixture_wire;

static void fixture_command_buffer(uint32_t opcode, uint64_t cmdbuf);
static void fixture_destroy(uint32_t opcode, uint64_t identity);
static void fixture_draw(uint64_t cmdbuf, uint32_t vertices, uint32_t instances);
static void fixture_submit(uint64_t cmdbuf, uint64_t fence);
static uint32_t fixture_fence_status(void);
static void fixture_resources(void);
static void fixture_copy_buffers(void);
static void fixture_begin(uint64_t cmdbuf);
static int fixture_find_command(const uint32_t *batch, unsigned used, uint32_t opcode);
static void fixture_subresource(uint32_t level);
static void fixture_level_copy(uint32_t opcode, uint64_t buffer, uint32_t level, uint32_t extent, uint64_t offset);
static void test_emission(void);
static void test_lifecycle(void);
static void test_indexed_dynamic(void);
static void test_copy_buffer(void);
static void test_mip_transfers(void);
static void test_recording_limits(void);
static void fixture_state_image(struct i915_gfx_image *image, struct i915_gfx_memory *memory, uint32_t side, uint64_t offset);
static void test_blend_state(void);
static void fixture_dsl(uint64_t identity, const uint32_t *numbers, const uint32_t *types, uint32_t count);
static void fixture_buffer_write(uint64_t set, uint32_t binding, uint32_t type, uint64_t offset, uint64_t range);
static void fixture_bind_set(uint64_t cmdbuf, uint32_t first, uint64_t set, const uint32_t *offsets, uint32_t offset_count);
static void test_uniform_bindings(void);

/*
 * Runs the command buffer checks.
 */
int
main(void)
{
	/* Checks the end of a draw's batch, then recording and submission. */
	test_emission();
	test_lifecycle();
	test_indexed_dynamic();
	test_copy_buffer();
	test_mip_transfers();
	test_recording_limits();
	test_blend_state();
	test_uniform_bindings();

	/* Succeeded: every check held. */
	printf("i915 vk cmdbuf host test PASS\n");
	return 0;
}

/* Appends a command that names only a command buffer and asks for a reply: vkEndCommandBuffer. */
static void
fixture_command_buffer(
	uint32_t opcode,
	uint64_t cmdbuf)
{
	/* [opcode][reply][command buffer]. */
	stub_put32(&fixture_wire, opcode);
	stub_put32(&fixture_wire, 1U);
	stub_put64(&fixture_wire, cmdbuf);
}

/* Appends a generic destroy or free: [opcode][reply][device][identity][pAllocator]. */
static void
fixture_destroy(
	uint32_t opcode,
	uint64_t identity)
{
	/* The command has no reply body; the reply is its echoed opcode. */
	stub_put32(&fixture_wire, opcode);
	stub_put32(&fixture_wire, 1U);
	stub_put64(&fixture_wire, FIXTURE_DEVICE);
	stub_put64(&fixture_wire, identity);
	stub_put64(&fixture_wire, 0U);
}

/* Appends vkCmdDraw with no first vertex or instance; a recording asks for no reply. */
static void
fixture_draw(
	uint64_t cmdbuf,
	uint32_t vertices,
	uint32_t instances)
{
	/* [106][no reply][command buffer][vertices][instances][first vertex][first instance]. */
	stub_put32(&fixture_wire, FIXTURE_CMD_DRAW);
	stub_put32(&fixture_wire, 0U);
	stub_put64(&fixture_wire, cmdbuf);
	stub_put32(&fixture_wire, vertices);
	stub_put32(&fixture_wire, instances);
	stub_put32(&fixture_wire, 0U);
	stub_put32(&fixture_wire, 0U);
}

/* Appends vkQueueSubmit of one command buffer with no semaphores, signalling `fence`. */
static void
fixture_submit(
	uint64_t cmdbuf,
	uint64_t fence)
{
	/* The header, the queue and one VkSubmitInfo: sType 4, no chain. */
	stub_put32(&fixture_wire, FIXTURE_QUEUE_SUBMIT);
	stub_put32(&fixture_wire, 1U);
	stub_put64(&fixture_wire, FIXTURE_QUEUE);
	stub_put32(&fixture_wire, 1U);
	stub_put64(&fixture_wire, 1U);
	stub_put32(&fixture_wire, 4U);
	stub_put64(&fixture_wire, 0U);

	/* No wait semaphores and no stages. */
	stub_put32(&fixture_wire, 0U);
	stub_put64(&fixture_wire, 0U);
	stub_put64(&fixture_wire, 0U);

	/* One command buffer. */
	stub_put32(&fixture_wire, 1U);
	stub_put64(&fixture_wire, 1U);
	stub_put64(&fixture_wire, cmdbuf);

	/* No signal semaphores, then the fence. */
	stub_put32(&fixture_wire, 0U);
	stub_put64(&fixture_wire, 0U);
	stub_put64(&fixture_wire, fence);
}

/* Asks vkGetFenceStatus of the fixture's fence and reports its VkResult. */
static uint32_t
fixture_fence_status(void)
{
	size_t reply_bytes;
	uint32_t status;

	/* [38][reply][device][fence] -> [38][VkResult]. */
	stub_wire_begin(&fixture_wire);
	stub_put32(&fixture_wire, FIXTURE_GET_FENCE_STATUS);
	stub_put32(&fixture_wire, 1U);
	stub_put64(&fixture_wire, FIXTURE_DEVICE);
	stub_put64(&fixture_wire, FIXTURE_FENCE);
	reply_bytes = stub_execute_ok(&fixture_wire);
	assert(reply_bytes == 8U);

	/* Reports the fence's result. */
	status = stub_get32(stub_reply, 4U);
	return status;
}

/* Creates the memory with its storage, a vertex buffer and a 16x16 image bound in it. */
static void
fixture_resources(void)
{
	size_t reply_bytes;
	int error;

	/* vkAllocateMemory of the whole storage, memory type 0. */
	stub_wire_begin(&fixture_wire);
	stub_put32(&fixture_wire, FIXTURE_ALLOCATE_MEMORY);
	stub_put32(&fixture_wire, 1U);
	stub_put64(&fixture_wire, FIXTURE_DEVICE);
	stub_put64(&fixture_wire, 1U);
	stub_put32(&fixture_wire, 5U);
	stub_put64(&fixture_wire, 0U);
	stub_put64(&fixture_wire, FIXTURE_STORAGE_BYTES);
	stub_put32(&fixture_wire, 0U);
	stub_put64(&fixture_wire, 0U);
	stub_put64(&fixture_wire, 1U);
	stub_put64(&fixture_wire, FIXTURE_MEMORY);

	/* vkCreateBuffer of 256 vertex-buffer bytes. */
	stub_put32(&fixture_wire, FIXTURE_CREATE_BUFFER);
	stub_put32(&fixture_wire, 1U);
	stub_put64(&fixture_wire, FIXTURE_DEVICE);
	stub_put64(&fixture_wire, 1U);
	stub_put32(&fixture_wire, 12U);
	stub_put64(&fixture_wire, 0U);
	stub_put32(&fixture_wire, 0U);
	stub_put64(&fixture_wire, 256U);
	stub_put32(&fixture_wire, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
	stub_put32(&fixture_wire, VK_SHARING_MODE_EXCLUSIVE);
	stub_put32(&fixture_wire, 0U);
	stub_put64(&fixture_wire, 0U);
	stub_put64(&fixture_wire, 0U);
	stub_put64(&fixture_wire, 1U);
	stub_put64(&fixture_wire, FIXTURE_BUFFER);

	/* vkCreateImage of a 16x16 R8G8B8A8_UNORM image. */
	stub_put32(&fixture_wire, FIXTURE_CREATE_IMAGE);
	stub_put32(&fixture_wire, 1U);
	stub_put64(&fixture_wire, FIXTURE_DEVICE);
	stub_put64(&fixture_wire, 1U);
	stub_put32(&fixture_wire, 14U);
	stub_put64(&fixture_wire, 0U);
	stub_put32(&fixture_wire, 0U);
	stub_put32(&fixture_wire, VK_IMAGE_TYPE_2D);
	stub_put32(&fixture_wire, VK_FORMAT_R8G8B8A8_UNORM);
	stub_put32(&fixture_wire, 16U);
	stub_put32(&fixture_wire, 16U);
	stub_put32(&fixture_wire, 1U);
	stub_put32(&fixture_wire, 1U);
	stub_put32(&fixture_wire, 1U);
	stub_put32(&fixture_wire, VK_SAMPLE_COUNT_1_BIT);
	stub_put32(&fixture_wire, VK_IMAGE_TILING_OPTIMAL);
	stub_put32(&fixture_wire, VK_IMAGE_USAGE_TRANSFER_DST_BIT);
	stub_put32(&fixture_wire, VK_SHARING_MODE_EXCLUSIVE);
	stub_put32(&fixture_wire, 0U);
	stub_put64(&fixture_wire, 0U);
	stub_put32(&fixture_wire, VK_IMAGE_LAYOUT_UNDEFINED);
	stub_put64(&fixture_wire, 0U);
	stub_put64(&fixture_wire, 1U);
	stub_put64(&fixture_wire, FIXTURE_IMAGE);

	/* Binds the buffer at 0 and the image at 4096. */
	stub_put32(&fixture_wire, FIXTURE_BIND_BUFFER_MEMORY);
	stub_put32(&fixture_wire, 1U);
	stub_put64(&fixture_wire, FIXTURE_DEVICE);
	stub_put64(&fixture_wire, FIXTURE_BUFFER);
	stub_put64(&fixture_wire, FIXTURE_MEMORY);
	stub_put64(&fixture_wire, 0U);
	stub_put32(&fixture_wire, FIXTURE_BIND_IMAGE_MEMORY);
	stub_put32(&fixture_wire, 1U);
	stub_put64(&fixture_wire, FIXTURE_DEVICE);
	stub_put64(&fixture_wire, FIXTURE_IMAGE);
	stub_put64(&fixture_wire, FIXTURE_MEMORY);
	stub_put64(&fixture_wire, 4096U);
	reply_bytes = stub_execute_ok(&fixture_wire);
	assert(reply_bytes == 3U * 24U + 2U * 8U);
	assert(stub_get32(stub_reply, 3U * 24U + 4U) == VK_SUCCESS);
	assert(stub_get32(stub_reply, 3U * 24U + 12U) == VK_SUCCESS);

	/* The blob libvulkan exports for the allocation becomes its storage. */
	error = drv_i915_render_blob_attach(stub_vk, &stub_gpu, FIXTURE_MEMORY, &fixture_storage_object);
	assert(error == 0);
}

/* Creates two 64 KiB transfer buffers bound at FIXTURE_SRC_OFFSET and FIXTURE_DST_OFFSET. */
static void
fixture_copy_buffers(void)
{
	size_t reply_bytes;
	uint64_t identities[2];
	uint64_t offsets[2];
	unsigned index;

	/* The two buffers and where they are bound. */
	identities[0] = FIXTURE_SRC_BUFFER;
	identities[1] = FIXTURE_DST_BUFFER;
	offsets[0] = FIXTURE_SRC_OFFSET;
	offsets[1] = FIXTURE_DST_OFFSET;

	/* vkCreateBuffer and vkBindBufferMemory of each. */
	stub_wire_begin(&fixture_wire);
	for (index = 0U; index < 2U; index++) {
		stub_put32(&fixture_wire, FIXTURE_CREATE_BUFFER);
		stub_put32(&fixture_wire, 1U);
		stub_put64(&fixture_wire, FIXTURE_DEVICE);
		stub_put64(&fixture_wire, 1U);
		stub_put32(&fixture_wire, 12U);
		stub_put64(&fixture_wire, 0U);
		stub_put32(&fixture_wire, 0U);
		stub_put64(&fixture_wire, FIXTURE_COPY_BYTES);
		stub_put32(&fixture_wire, VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT);
		stub_put32(&fixture_wire, VK_SHARING_MODE_EXCLUSIVE);
		stub_put32(&fixture_wire, 0U);
		stub_put64(&fixture_wire, 0U);
		stub_put64(&fixture_wire, 0U);
		stub_put64(&fixture_wire, 1U);
		stub_put64(&fixture_wire, identities[index]);
		stub_put32(&fixture_wire, FIXTURE_BIND_BUFFER_MEMORY);
		stub_put32(&fixture_wire, 1U);
		stub_put64(&fixture_wire, FIXTURE_DEVICE);
		stub_put64(&fixture_wire, identities[index]);
		stub_put64(&fixture_wire, FIXTURE_MEMORY);
		stub_put64(&fixture_wire, offsets[index]);
	}

	reply_bytes = stub_execute_ok(&fixture_wire);
	assert(reply_bytes == 2U * (24U + 8U));
	assert(stub_get32(stub_reply, 4U) == VK_SUCCESS);
	assert(stub_get32(stub_reply, 24U + 4U) == VK_SUCCESS);
}

/* Appends vkBeginCommandBuffer with no flags; the reply is [90][VkResult]. */
static void
fixture_begin(
	uint64_t cmdbuf)
{
	/* [90][reply][command buffer][present][sType 42][no chain][flags][no inheritance]. */
	stub_put32(&fixture_wire, FIXTURE_BEGIN_COMMAND_BUFFER);
	stub_put32(&fixture_wire, 1U);
	stub_put64(&fixture_wire, cmdbuf);
	stub_put64(&fixture_wire, 1U);
	stub_put32(&fixture_wire, 42U);
	stub_put64(&fixture_wire, 0U);
	stub_put32(&fixture_wire, 0U);
	stub_put64(&fixture_wire, 0U);
}

/* Finds the first dword of a batch whose command opcode is `opcode`; -1 when none is. */
static int
fixture_find_command(
	const uint32_t *batch,
	unsigned used,
	uint32_t opcode)
{
	unsigned index;

	/* Looks at every dword's high half. */
	for (index = 0U; index < used; index++) {
		if ((batch[index] >> 16) == opcode)
			return (int)index;
	}

	/* No dword carries the opcode. */
	return -1;
}

/*
 * A draw ends its batch with the primitive: 3DPRIMITIVE with the vertex
 * and instance counts, a flush, and MI_BATCH_BUFFER_END.
 */
static void
test_emission(void)
{
	struct i915_gfx_draw_state state;
	struct i915_gfx_primitive shape;
	struct i915_gfx_memory memory;
	struct i915_gfx_buffer buffer;
	struct i915_gem_object object;
	struct i915_gfx_batch batch;
	uint32_t commands[128];
	int primitive;
	int error;

	/* Emits the primitive of a three-vertex, one-instance triangle list on a 64x32 target. */
	memset(commands, 0, sizeof(commands));
	batch.cmds = commands;
	batch.count = 0U;
	batch.capacity = 128U;
	batch.overflow = 0;
	memset(&shape, 0, sizeof(shape));
	shape.topology = GEN12_3DPRIM_TRILIST;
	shape.vertex_count = 3U;
	shape.instance_count = 1U;
	drv_i915_gfx_emit_primitive(&batch, 64U, 32U, &shape);
	assert(batch.overflow == 0);

	/* The primitive carries the topology, sequential access, the vertex count and the instance count. */
	primitive = fixture_find_command(commands, batch.count, GEN12_CMD_3DPRIMITIVE);
	assert(primitive >= 0);
	assert(commands[primitive + 1] == 4U);
	assert(commands[primitive + 2] == 3U);
	assert(commands[primitive + 4] == 1U);
	assert(commands[primitive + 6] == 0U);

	/* The drawing rectangle covers the target. */
	primitive = fixture_find_command(commands, batch.count, GEN12_CMD_3DSTATE_DRAWING_RECTANGLE);
	assert(primitive >= 0);
	assert(commands[primitive + 2] == ((64U - 1U) | ((32U - 1U) << 16)));

	/* The batch ends: MI_BATCH_BUFFER_END and one MI_NOOP behind it. */
	assert(commands[batch.count - 2U] == MI_BATCH_BUFFER_END);
	assert(commands[batch.count - 1U] == MI_NOOP);

	/*
	 * A batch too small for the primitive overflows instead of writing past
	 * its end, and still counts the dwords it would have needed.
	 */
	memset(commands, 0xee, sizeof(commands));
	batch.count = 0U;
	batch.capacity = 8U;
	batch.overflow = 0;
	drv_i915_gfx_emit_primitive(&batch, 64U, 32U, &shape);
	assert(batch.overflow != 0);
	assert(batch.count > 8U);
	assert(commands[8] == 0xeeeeeeeeU);

	/*
	 * An indexed primitive reads at random from the first index, each index
	 * moved by the base vertex: six indices from index 3, base vertex -2,
	 * two instances from instance 1.
	 */
	memset(commands, 0, sizeof(commands));
	batch.count = 0U;
	batch.capacity = 128U;
	batch.overflow = 0;
	shape.random_access = 1;
	shape.vertex_count = 6U;
	shape.start_vertex = 3U;
	shape.instance_count = 2U;
	shape.start_instance = 1U;
	shape.base_vertex = -2;
	drv_i915_gfx_emit_primitive(&batch, 64U, 32U, &shape);
	assert(batch.overflow == 0);
	primitive = fixture_find_command(commands, batch.count, GEN12_CMD_3DPRIMITIVE);
	assert(primitive >= 0);
	assert(commands[primitive + 1] == (4U | GEN12_3DPRIMITIVE_VERTEX_RANDOM));
	assert(commands[primitive + 2] == 6U);
	assert(commands[primitive + 3] == 3U);
	assert(commands[primitive + 4] == 2U);
	assert(commands[primitive + 5] == 1U);
	assert(commands[primitive + 6] == 0xfffffffeU);

	/* A buffer of 256 bytes bound at 0x1000 of an object at 0x7000_0000. */
	memset(&object, 0, sizeof(object));
	object.bytes = 65536U;
	object.va = 0x70000000ULL;
	memset(&memory, 0, sizeof(memory));
	memory.object = &object;
	memory.size = 65536U;
	memset(&buffer, 0, sizeof(buffer));
	buffer.size = 256U;
	buffer.memory = &memory;
	buffer.offset = 0x1000U;

	/* A 16-bit index buffer at offset 8: the address past the offset and the bytes to the end. */
	memset(&state, 0, sizeof(state));
	state.index.buffer = &buffer;
	state.index.offset = 8U;
	state.index.type = VK_INDEX_TYPE_UINT16;
	memset(commands, 0, sizeof(commands));
	batch.count = 0U;
	error = drv_i915_gfx_emit_index_buffer(&batch, &state, 0x6U);
	assert(error == 0);
	assert(batch.count == GEN12_3DSTATE_INDEX_BUFFER_DWORDS);
	assert(commands[0] == GEN12_CMD_HEADER(GEN12_CMD_3DSTATE_INDEX_BUFFER, GEN12_3DSTATE_INDEX_BUFFER_DWORDS));
	assert(commands[1] == (0x6U | (GEN12_INDEX_WORD << 8) | (1U << 11)));
	assert(commands[2] == 0x70001008U);
	assert(commands[3] == 0U);
	assert(commands[4] == 248U);

	/* A 32-bit index buffer names the dword format. */
	state.index.type = VK_INDEX_TYPE_UINT32;
	batch.count = 0U;
	error = drv_i915_gfx_emit_index_buffer(&batch, &state, 0x6U);
	assert(error == 0);
	assert(commands[1] == (0x6U | (GEN12_INDEX_DWORD << 8) | (1U << 11)));

	/* An offset that does not start an index, one past the end, and no buffer are refused. */
	state.index.offset = 6U;
	error = drv_i915_gfx_emit_index_buffer(&batch, &state, 0x6U);
	assert(error == EINVAL);
	state.index.offset = 256U;
	error = drv_i915_gfx_emit_index_buffer(&batch, &state, 0x6U);
	assert(error == EINVAL);
	state.index.buffer = NULL;
	error = drv_i915_gfx_emit_index_buffer(&batch, &state, 0x6U);
	assert(error == EINVAL);

	/* An 8-bit index type is not implemented and says so. */
	state.index.buffer = &buffer;
	state.index.offset = 0U;
	state.index.type = VK_INDEX_TYPE_UINT8_EXT;
	error = drv_i915_gfx_emit_index_buffer(&batch, &state, 0x6U);
	assert(error == ENOTSUP);
	assert(strstr(stub_log, "index type") != NULL);
}

/*
 * A command buffer is allocated, recorded, submitted with a fence, and
 * freed through the wire; at submission its operations reach the GPU path
 * in recording order with what was bound.
 */
static void
test_lifecycle(void)
{
	struct i915_gfx_buffer *buffer;
	size_t reply_bytes;
	unsigned draws;
	unsigned rects;
	int error;

	/* Opens a session with the storage blob and makes the resources. */
	memset(&fixture_storage_object, 0, sizeof(fixture_storage_object));
	fixture_storage_object.slot = 7U;
	fixture_storage_object.bytes = sizeof(fixture_storage);
	fixture_storage_object.run.paddr = (hal_physaddr_t)(uintptr_t)fixture_storage;
	fixture_storage_object.va = FIXTURE_STORAGE_VA;
	stub_session_open(&fixture_storage_object);
	fixture_resources();
	buffer = drv_i915_object_lookup(stub_session, I915_VK_OBJ_BUFFER, FIXTURE_BUFFER);
	assert(buffer != NULL);

	/* Publishes the stand-in pipeline the recording binds. */
	memset(&fixture_pipeline, 0, sizeof(fixture_pipeline));
	fixture_pipeline.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
	error = drv_i915_object_insert(stub_session, I915_VK_OBJ_PIPELINE, FIXTURE_PIPELINE, &fixture_pipeline);
	assert(error == 0);

	/* vkCreateCommandPool: [85][VK_SUCCESS][present][identity]. */
	stub_wire_begin(&fixture_wire);
	stub_put32(&fixture_wire, FIXTURE_CREATE_COMMAND_POOL);
	stub_put32(&fixture_wire, 1U);
	stub_put64(&fixture_wire, FIXTURE_DEVICE);
	stub_put64(&fixture_wire, 1U);
	stub_put32(&fixture_wire, 39U);
	stub_put64(&fixture_wire, 0U);
	stub_put32(&fixture_wire, 0U);
	stub_put32(&fixture_wire, 0U);
	stub_put64(&fixture_wire, 0U);
	stub_put64(&fixture_wire, 1U);
	stub_put64(&fixture_wire, FIXTURE_POOL);
	reply_bytes = stub_execute_ok(&fixture_wire);
	assert(reply_bytes == 24U);
	assert(stub_get32(stub_reply, 4U) == VK_SUCCESS);
	assert(stub_get64(stub_reply, 16U) == FIXTURE_POOL);
	assert(drv_i915_object_lookup(stub_session, I915_VK_OBJ_COMMAND_POOL, FIXTURE_POOL) != NULL);

	/* vkAllocateCommandBuffers of two primary buffers: [88][VK_SUCCESS][count][identities]. */
	stub_wire_begin(&fixture_wire);
	stub_put32(&fixture_wire, FIXTURE_ALLOCATE_COMMAND_BUFFERS);
	stub_put32(&fixture_wire, 1U);
	stub_put64(&fixture_wire, FIXTURE_DEVICE);
	stub_put64(&fixture_wire, 1U);
	stub_put32(&fixture_wire, 40U);
	stub_put64(&fixture_wire, 0U);
	stub_put64(&fixture_wire, FIXTURE_POOL);
	stub_put32(&fixture_wire, VK_COMMAND_BUFFER_LEVEL_PRIMARY);
	stub_put32(&fixture_wire, 2U);
	stub_put64(&fixture_wire, 2U);
	stub_put64(&fixture_wire, FIXTURE_CB0);
	stub_put64(&fixture_wire, FIXTURE_CB1);
	reply_bytes = stub_execute_ok(&fixture_wire);
	assert(reply_bytes == 32U);
	assert(stub_get32(stub_reply, 4U) == VK_SUCCESS);
	assert(stub_get64(stub_reply, 8U) == 2U);
	assert(stub_get64(stub_reply, 16U) == FIXTURE_CB0);
	assert(stub_get64(stub_reply, 24U) == FIXTURE_CB1);
	assert(drv_i915_object_lookup(stub_session, I915_VK_OBJ_COMMAND_BUFFER, FIXTURE_CB0) != NULL);
	assert(drv_i915_object_lookup(stub_session, I915_VK_OBJ_COMMAND_BUFFER, FIXTURE_CB1) != NULL);

	/* vkBeginCommandBuffer of cb0: [90][VK_SUCCESS]. */
	stub_wire_begin(&fixture_wire);
	stub_put32(&fixture_wire, FIXTURE_BEGIN_COMMAND_BUFFER);
	stub_put32(&fixture_wire, 1U);
	stub_put64(&fixture_wire, FIXTURE_CB0);
	stub_put64(&fixture_wire, 1U);
	stub_put32(&fixture_wire, 42U);
	stub_put64(&fixture_wire, 0U);
	stub_put32(&fixture_wire, 0U);
	stub_put64(&fixture_wire, 0U);
	reply_bytes = stub_execute_ok(&fixture_wire);
	assert(reply_bytes == 8U);
	assert(stub_get32(stub_reply, 4U) == VK_SUCCESS);

	/*
	 * Records, with no replies: a clear of the image to four words, the
	 * pipeline, the vertex buffer at offset 64, eight bytes of push
	 * constants, then a three-vertex draw.
	 */
	stub_wire_begin(&fixture_wire);
	stub_put32(&fixture_wire, FIXTURE_CMD_CLEAR_COLOR_IMAGE);
	stub_put32(&fixture_wire, 0U);
	stub_put64(&fixture_wire, FIXTURE_CB0);
	stub_put64(&fixture_wire, FIXTURE_IMAGE);
	stub_put32(&fixture_wire, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
	stub_put64(&fixture_wire, 1U);
	stub_put32(&fixture_wire, 0U);
	stub_put64(&fixture_wire, 4U);
	stub_put32(&fixture_wire, 0x11111111U);
	stub_put32(&fixture_wire, 0x22222222U);
	stub_put32(&fixture_wire, 0x33333333U);
	stub_put32(&fixture_wire, 0x44444444U);
	stub_put32(&fixture_wire, 1U);
	stub_put64(&fixture_wire, 1U);
	stub_put32(&fixture_wire, VK_IMAGE_ASPECT_COLOR_BIT);
	stub_put32(&fixture_wire, 0U);
	stub_put32(&fixture_wire, 1U);
	stub_put32(&fixture_wire, 0U);
	stub_put32(&fixture_wire, 1U);
	stub_put32(&fixture_wire, FIXTURE_CMD_BIND_PIPELINE);
	stub_put32(&fixture_wire, 0U);
	stub_put64(&fixture_wire, FIXTURE_CB0);
	stub_put32(&fixture_wire, VK_PIPELINE_BIND_POINT_GRAPHICS);
	stub_put64(&fixture_wire, FIXTURE_PIPELINE);
	stub_put32(&fixture_wire, FIXTURE_CMD_BIND_VERTEX_BUFFERS);
	stub_put32(&fixture_wire, 0U);
	stub_put64(&fixture_wire, FIXTURE_CB0);
	stub_put32(&fixture_wire, 0U);
	stub_put32(&fixture_wire, 1U);
	stub_put64(&fixture_wire, 1U);
	stub_put64(&fixture_wire, FIXTURE_BUFFER);
	stub_put64(&fixture_wire, 1U);
	stub_put64(&fixture_wire, 64U);
	stub_put32(&fixture_wire, FIXTURE_CMD_PUSH_CONSTANTS);
	stub_put32(&fixture_wire, 0U);
	stub_put64(&fixture_wire, FIXTURE_CB0);
	stub_put64(&fixture_wire, 0U);
	stub_put32(&fixture_wire, VK_SHADER_STAGE_VERTEX_BIT);
	stub_put32(&fixture_wire, 16U);
	stub_put32(&fixture_wire, 8U);
	stub_put64(&fixture_wire, 8U);
	stub_put32(&fixture_wire, 0x04030201U);
	stub_put32(&fixture_wire, 0x08070605U);
	fixture_draw(FIXTURE_CB0, 3U, 1U);
	reply_bytes = stub_execute_ok(&fixture_wire);
	assert(reply_bytes == 0U);

	/* Recording runs nothing on the GPU. */
	assert(stub_draw_calls == 0U);
	assert(stub_rect_calls == 0U);

	/* vkEndCommandBuffer of cb0: [91][VK_SUCCESS]. */
	stub_wire_begin(&fixture_wire);
	fixture_command_buffer(FIXTURE_END_COMMAND_BUFFER, FIXTURE_CB0);
	reply_bytes = stub_execute_ok(&fixture_wire);
	assert(reply_bytes == 8U);
	assert(stub_get32(stub_reply, 0U) == FIXTURE_END_COMMAND_BUFFER);
	assert(stub_get32(stub_reply, 4U) == VK_SUCCESS);

	/* vkCreateFence, unsignaled. */
	stub_wire_begin(&fixture_wire);
	stub_put32(&fixture_wire, FIXTURE_CREATE_FENCE);
	stub_put32(&fixture_wire, 1U);
	stub_put64(&fixture_wire, FIXTURE_DEVICE);
	stub_put64(&fixture_wire, 1U);
	stub_put32(&fixture_wire, 8U);
	stub_put64(&fixture_wire, 0U);
	stub_put32(&fixture_wire, 0U);
	stub_put64(&fixture_wire, 0U);
	stub_put64(&fixture_wire, 1U);
	stub_put64(&fixture_wire, FIXTURE_FENCE);
	reply_bytes = stub_execute_ok(&fixture_wire);
	assert(reply_bytes == 24U);
	assert(fixture_fence_status() == VK_NOT_READY);

	/* vkQueueSubmit runs cb0 to its end before it replies: [18][VK_SUCCESS]. */
	stub_wire_begin(&fixture_wire);
	fixture_submit(FIXTURE_CB0, FIXTURE_FENCE);
	reply_bytes = stub_execute_ok(&fixture_wire);
	assert(reply_bytes == 8U);
	assert(stub_get32(stub_reply, 0U) == FIXTURE_QUEUE_SUBMIT);
	assert(stub_get32(stub_reply, 4U) == VK_SUCCESS);

	/* The clear ran as one GPU fill of the whole image, at the image's address in the storage. */
	assert(stub_rect_calls == 1U);
	assert(stub_last_rect.copy == 0);
	assert(stub_last_rect.dst.va == FIXTURE_STORAGE_VA + 4096U);
	assert(stub_last_rect.dst.width == 16U);
	assert(stub_last_rect.dst.pitch == 64U);
	assert(stub_last_rect.dst_rect.w == 16U);
	assert(stub_last_rect.dst_rect.h == 16U);
	assert(stub_last_rect.clear[0] == 0x11111111U);
	assert(stub_last_rect.clear[3] == 0x44444444U);

	/* The draw ran with the pipeline, the vertex buffer and the push constants that were bound. */
	assert(stub_draw_calls == 1U);
	assert(stub_last_draw.state.pipeline == &fixture_pipeline);
	assert(stub_last_draw.state.vertex[0].buffer == buffer);
	assert(stub_last_draw.state.vertex[0].offset == 64U);
	assert(stub_last_draw.state.push[16] == 0x01U);
	assert(stub_last_draw.state.push[23] == 0x08U);
	assert(stub_last_draw.args.indexed == 0);
	assert(stub_last_draw.args.count == 3U);
	assert(stub_last_draw.args.instance_count == 1U);
	assert(stub_last_draw.args.first == 0U);

	/* Everything the submission asked for is done, so its fence is signalled. */
	assert(fixture_fence_status() == VK_SUCCESS);

	/* vkResetFences returns the fence to unsignaled: [37][VK_SUCCESS]. */
	stub_wire_begin(&fixture_wire);
	stub_put32(&fixture_wire, FIXTURE_RESET_FENCES);
	stub_put32(&fixture_wire, 1U);
	stub_put64(&fixture_wire, FIXTURE_DEVICE);
	stub_put32(&fixture_wire, 1U);
	stub_put64(&fixture_wire, 1U);
	stub_put64(&fixture_wire, FIXTURE_FENCE);
	reply_bytes = stub_execute_ok(&fixture_wire);
	assert(reply_bytes == 8U);
	assert(fixture_fence_status() == VK_NOT_READY);

	/*
	 * A draw that fails on the GPU stops the command buffer: the submission
	 * reports a lost device and the fence stays unsignaled.
	 */
	stub_draw_result = EIO;
	stub_wire_begin(&fixture_wire);
	fixture_submit(FIXTURE_CB0, FIXTURE_FENCE);
	reply_bytes = stub_execute_ok(&fixture_wire);
	stub_draw_result = 0;
	assert(reply_bytes == 8U);
	assert(stub_get32(stub_reply, 4U) == (uint32_t)VK_ERROR_DEVICE_LOST);
	assert(strstr(stub_log, "command buffer stopped at operation 5 of 5") != NULL);
	assert(stub_draw_calls == 2U);
	assert(stub_rect_calls == 2U);
	assert(fixture_fence_status() == VK_NOT_READY);

	/* vkResetCommandPool empties every recording: a submission then runs nothing. */
	stub_wire_begin(&fixture_wire);
	stub_put32(&fixture_wire, FIXTURE_RESET_COMMAND_POOL);
	stub_put32(&fixture_wire, 1U);
	stub_put64(&fixture_wire, FIXTURE_DEVICE);
	stub_put64(&fixture_wire, FIXTURE_POOL);
	stub_put32(&fixture_wire, 0U);
	fixture_submit(FIXTURE_CB0, FIXTURE_FENCE);
	reply_bytes = stub_execute_ok(&fixture_wire);
	assert(reply_bytes == 16U);
	assert(stub_get32(stub_reply, 4U) == VK_SUCCESS);
	assert(stub_get32(stub_reply, 12U) == VK_SUCCESS);
	draws = stub_draw_calls;
	rects = stub_rect_calls;
	assert(draws == 2U);
	assert(rects == 2U);
	assert(fixture_fence_status() == VK_SUCCESS);

	/* vkFreeCommandBuffers releases both buffers. */
	stub_wire_begin(&fixture_wire);
	stub_put32(&fixture_wire, FIXTURE_FREE_COMMAND_BUFFERS);
	stub_put32(&fixture_wire, 1U);
	stub_put64(&fixture_wire, FIXTURE_DEVICE);
	stub_put64(&fixture_wire, FIXTURE_POOL);
	stub_put32(&fixture_wire, 2U);
	stub_put64(&fixture_wire, 2U);
	stub_put64(&fixture_wire, FIXTURE_CB0);
	stub_put64(&fixture_wire, FIXTURE_CB1);
	reply_bytes = stub_execute_ok(&fixture_wire);
	assert(reply_bytes == 4U);
	assert(drv_i915_object_lookup(stub_session, I915_VK_OBJ_COMMAND_BUFFER, FIXTURE_CB0) == NULL);
	assert(drv_i915_object_lookup(stub_session, I915_VK_OBJ_COMMAND_BUFFER, FIXTURE_CB1) == NULL);

	/* Destroys the pool, the fence and the resources, and withdraws the pipeline. */
	stub_wire_begin(&fixture_wire);
	fixture_destroy(FIXTURE_DESTROY_COMMAND_POOL, FIXTURE_POOL);
	fixture_destroy(FIXTURE_DESTROY_FENCE, FIXTURE_FENCE);
	fixture_destroy(FIXTURE_DESTROY_IMAGE, FIXTURE_IMAGE);
	fixture_destroy(FIXTURE_DESTROY_BUFFER, FIXTURE_BUFFER);
	fixture_destroy(FIXTURE_FREE_MEMORY, FIXTURE_MEMORY);
	reply_bytes = stub_execute_ok(&fixture_wire);
	assert(reply_bytes == 5U * 4U);
	assert(drv_i915_object_lookup(stub_session, I915_VK_OBJ_COMMAND_POOL, FIXTURE_POOL) == NULL);
	drv_i915_object_remove(stub_session, I915_VK_OBJ_PIPELINE, FIXTURE_PIPELINE);

	/* Closes the session: the draw state is released once and nothing stays allocated. */
	stub_session_close();
	assert(stub_session_closes == 1U);
	assert(stub_live == 0U);
}

/*
 * A recording far longer than the first operation list grows its list and
 * runs whole; one past the bound fails its end; an unimplemented vkCmd*
 * fails the stream; destroying a pool frees the buffers still in it.
 */
static void
test_recording_limits(void)
{
	size_t reply_bytes;
	unsigned recorded;
	unsigned index;
	int error;

	/* Opens a session with a pool and one command buffer. */
	stub_session_open(NULL);
	stub_wire_begin(&fixture_wire);
	stub_put32(&fixture_wire, FIXTURE_CREATE_COMMAND_POOL);
	stub_put32(&fixture_wire, 1U);
	stub_put64(&fixture_wire, FIXTURE_DEVICE);
	stub_put64(&fixture_wire, 1U);
	stub_put32(&fixture_wire, 39U);
	stub_put64(&fixture_wire, 0U);
	stub_put32(&fixture_wire, 0U);
	stub_put32(&fixture_wire, 0U);
	stub_put64(&fixture_wire, 0U);
	stub_put64(&fixture_wire, 1U);
	stub_put64(&fixture_wire, FIXTURE_POOL);
	stub_put32(&fixture_wire, FIXTURE_ALLOCATE_COMMAND_BUFFERS);
	stub_put32(&fixture_wire, 1U);
	stub_put64(&fixture_wire, FIXTURE_DEVICE);
	stub_put64(&fixture_wire, 1U);
	stub_put32(&fixture_wire, 40U);
	stub_put64(&fixture_wire, 0U);
	stub_put64(&fixture_wire, FIXTURE_POOL);
	stub_put32(&fixture_wire, VK_COMMAND_BUFFER_LEVEL_PRIMARY);
	stub_put32(&fixture_wire, 1U);
	stub_put64(&fixture_wire, 1U);
	stub_put64(&fixture_wire, FIXTURE_CB0);
	reply_bytes = stub_execute_ok(&fixture_wire);
	assert(reply_bytes == 24U + 24U);

	/* A secondary command buffer is refused by name as a missing feature. */
	stub_wire_begin(&fixture_wire);
	stub_put32(&fixture_wire, FIXTURE_ALLOCATE_COMMAND_BUFFERS);
	stub_put32(&fixture_wire, 1U);
	stub_put64(&fixture_wire, FIXTURE_DEVICE);
	stub_put64(&fixture_wire, 1U);
	stub_put32(&fixture_wire, 40U);
	stub_put64(&fixture_wire, 0U);
	stub_put64(&fixture_wire, FIXTURE_POOL);
	stub_put32(&fixture_wire, VK_COMMAND_BUFFER_LEVEL_SECONDARY);
	stub_put32(&fixture_wire, 1U);
	stub_put64(&fixture_wire, 1U);
	stub_put64(&fixture_wire, FIXTURE_CB1);
	reply_bytes = stub_execute_ok(&fixture_wire);
	assert(reply_bytes == 8U);
	assert(stub_get32(stub_reply, 4U) == (uint32_t)VK_ERROR_INITIALIZATION_FAILED);
	assert(strstr(stub_log, "secondary command buffers are not implemented") != NULL);
	assert(drv_i915_object_lookup(stub_session, I915_VK_OBJ_COMMAND_BUFFER, FIXTURE_CB1) == NULL);

	/*
	 * Two hundred draws, more than three times the first list: the list
	 * grows, the end succeeds and a submission runs every draw in order.
	 * With no fence the submission signals nothing.
	 */
	stub_draw_calls = 0U;
	stub_wire_begin(&fixture_wire);
	for (index = 0U; index < 200U; index++)
		fixture_draw(FIXTURE_CB0, index + 1U, 1U);
	fixture_command_buffer(FIXTURE_END_COMMAND_BUFFER, FIXTURE_CB0);
	fixture_submit(FIXTURE_CB0, 0U);
	reply_bytes = stub_execute_ok(&fixture_wire);
	assert(reply_bytes == 16U);
	assert(stub_get32(stub_reply, 4U) == VK_SUCCESS);
	assert(stub_get32(stub_reply, 12U) == VK_SUCCESS);
	assert(stub_draw_calls == 200U);
	assert(stub_last_draw.args.count == 200U);

	/*
	 * One operation more than a buffer records at most: the end reports
	 * that the recording did not fit.  The draws travel in streams of their
	 * own, as a long recording does.
	 */
	stub_wire_begin(&fixture_wire);
	fixture_begin(FIXTURE_CB0);
	reply_bytes = stub_execute_ok(&fixture_wire);
	assert(stub_get32(stub_reply, 4U) == VK_SUCCESS);
	recorded = 0U;
	while (recorded < FIXTURE_MAX_OPS + 1U) {
		stub_wire_begin(&fixture_wire);
		for (index = 0U; index < FIXTURE_DRAWS_PER_STREAM && recorded < FIXTURE_MAX_OPS + 1U; index++) {
			fixture_draw(FIXTURE_CB0, 3U, 1U);
			recorded++;
		}
		reply_bytes = stub_execute_ok(&fixture_wire);
		assert(reply_bytes == 0U);
	}

	stub_wire_begin(&fixture_wire);
	fixture_command_buffer(FIXTURE_END_COMMAND_BUFFER, FIXTURE_CB0);
	reply_bytes = stub_execute_ok(&fixture_wire);
	assert(reply_bytes == 8U);
	assert(stub_get32(stub_reply, 4U) == (uint32_t)VK_ERROR_OUT_OF_HOST_MEMORY);
	assert(strstr(stub_log, "needs more than 65536 operations") != NULL);

	/* An unimplemented recording fails the stream: nothing after it runs and no reply is published. */
	stub_wire_begin(&fixture_wire);
	stub_put32(&fixture_wire, FIXTURE_CMD_SET_LINE_WIDTH);
	stub_put32(&fixture_wire, 0U);
	stub_put64(&fixture_wire, FIXTURE_CB0);
	stub_put32(&fixture_wire, 0x3f800000U);
	fixture_command_buffer(FIXTURE_END_COMMAND_BUFFER, FIXTURE_CB0);
	error = stub_execute(&fixture_wire, &reply_bytes);
	assert(error == ENOTSUP);
	assert(reply_bytes == STUB_REPLY_BYTES);
	assert(strcmp(stub_log, "i915: vk: XXX unimplemented opcode 96 (recording)\n") == 0);

	/* vkDestroyCommandPool frees the buffer still allocated from it. */
	stub_wire_begin(&fixture_wire);
	fixture_destroy(FIXTURE_DESTROY_COMMAND_POOL, FIXTURE_POOL);
	reply_bytes = stub_execute_ok(&fixture_wire);
	assert(reply_bytes == 4U);
	assert(drv_i915_object_lookup(stub_session, I915_VK_OBJ_COMMAND_BUFFER, FIXTURE_CB0) == NULL);

	/* Closes the session; nothing stays allocated. */
	stub_session_close();
	assert(stub_live == 0U);
}

/*
 * An index buffer bind, a dynamic viewport and scissor and an indexed draw
 * reach the GPU path with what was recorded: the draw carries its counts,
 * its first index, its vertex offset and its first instance; the state
 * carries the index buffer and the viewport and scissor set last.  A
 * viewport or scissor set at index 1 has no place and changes nothing.
 */
static void
test_indexed_dynamic(void)
{
	struct i915_gfx_buffer *buffer;
	size_t reply_bytes;
	int error;

	/* Opens a session with the storage blob, the resources, the pipeline, a pool and one buffer. */
	memset(&fixture_storage_object, 0, sizeof(fixture_storage_object));
	fixture_storage_object.slot = 7U;
	fixture_storage_object.bytes = sizeof(fixture_storage);
	fixture_storage_object.run.paddr = (hal_physaddr_t)(uintptr_t)fixture_storage;
	fixture_storage_object.va = FIXTURE_STORAGE_VA;
	stub_session_open(&fixture_storage_object);
	fixture_resources();
	buffer = drv_i915_object_lookup(stub_session, I915_VK_OBJ_BUFFER, FIXTURE_BUFFER);
	assert(buffer != NULL);
	memset(&fixture_pipeline, 0, sizeof(fixture_pipeline));
	fixture_pipeline.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
	error = drv_i915_object_insert(stub_session, I915_VK_OBJ_PIPELINE, FIXTURE_PIPELINE, &fixture_pipeline);
	assert(error == 0);
	stub_wire_begin(&fixture_wire);
	stub_put32(&fixture_wire, FIXTURE_CREATE_COMMAND_POOL);
	stub_put32(&fixture_wire, 1U);
	stub_put64(&fixture_wire, FIXTURE_DEVICE);
	stub_put64(&fixture_wire, 1U);
	stub_put32(&fixture_wire, 39U);
	stub_put64(&fixture_wire, 0U);
	stub_put32(&fixture_wire, 0U);
	stub_put32(&fixture_wire, 0U);
	stub_put64(&fixture_wire, 0U);
	stub_put64(&fixture_wire, 1U);
	stub_put64(&fixture_wire, FIXTURE_POOL);
	stub_put32(&fixture_wire, FIXTURE_ALLOCATE_COMMAND_BUFFERS);
	stub_put32(&fixture_wire, 1U);
	stub_put64(&fixture_wire, FIXTURE_DEVICE);
	stub_put64(&fixture_wire, 1U);
	stub_put32(&fixture_wire, 40U);
	stub_put64(&fixture_wire, 0U);
	stub_put64(&fixture_wire, FIXTURE_POOL);
	stub_put32(&fixture_wire, VK_COMMAND_BUFFER_LEVEL_PRIMARY);
	stub_put32(&fixture_wire, 1U);
	stub_put64(&fixture_wire, 1U);
	stub_put64(&fixture_wire, FIXTURE_CB0);
	fixture_begin(FIXTURE_CB0);
	reply_bytes = stub_execute_ok(&fixture_wire);
	assert(reply_bytes == 24U + 24U + 8U);

	/* Counts the GPU runs of this test only. */
	stub_draw_calls = 0U;
	stub_rect_calls = 0U;

	/*
	 * Records the pipeline; a 16-bit index buffer at offset 8; viewport 0 of
	 * (16, 8, 32, 16) in depth [0, 1]; viewport 1, which has no place;
	 * scissor 0 of (4, 2) 40x30; then an indexed draw of six indices from
	 * index 3 with vertex offset -2, two instances from instance 1.
	 */
	stub_wire_begin(&fixture_wire);
	stub_put32(&fixture_wire, FIXTURE_CMD_BIND_PIPELINE);
	stub_put32(&fixture_wire, 0U);
	stub_put64(&fixture_wire, FIXTURE_CB0);
	stub_put32(&fixture_wire, VK_PIPELINE_BIND_POINT_GRAPHICS);
	stub_put64(&fixture_wire, FIXTURE_PIPELINE);
	stub_put32(&fixture_wire, FIXTURE_CMD_BIND_INDEX_BUFFER);
	stub_put32(&fixture_wire, 0U);
	stub_put64(&fixture_wire, FIXTURE_CB0);
	stub_put64(&fixture_wire, FIXTURE_BUFFER);
	stub_put64(&fixture_wire, 8U);
	stub_put32(&fixture_wire, VK_INDEX_TYPE_UINT16);
	stub_put32(&fixture_wire, FIXTURE_CMD_SET_VIEWPORT);
	stub_put32(&fixture_wire, 0U);
	stub_put64(&fixture_wire, FIXTURE_CB0);
	stub_put32(&fixture_wire, 0U);
	stub_put32(&fixture_wire, 1U);
	stub_put64(&fixture_wire, 1U);
	stub_put32(&fixture_wire, 0x41800000U);
	stub_put32(&fixture_wire, 0x41000000U);
	stub_put32(&fixture_wire, 0x42000000U);
	stub_put32(&fixture_wire, 0x41800000U);
	stub_put32(&fixture_wire, 0x00000000U);
	stub_put32(&fixture_wire, 0x3f800000U);
	stub_put32(&fixture_wire, FIXTURE_CMD_SET_VIEWPORT);
	stub_put32(&fixture_wire, 0U);
	stub_put64(&fixture_wire, FIXTURE_CB0);
	stub_put32(&fixture_wire, 1U);
	stub_put32(&fixture_wire, 1U);
	stub_put64(&fixture_wire, 1U);
	stub_put32(&fixture_wire, 0x3f800000U);
	stub_put32(&fixture_wire, 0x3f800000U);
	stub_put32(&fixture_wire, 0x3f800000U);
	stub_put32(&fixture_wire, 0x3f800000U);
	stub_put32(&fixture_wire, 0x3f800000U);
	stub_put32(&fixture_wire, 0x3f800000U);
	stub_put32(&fixture_wire, FIXTURE_CMD_SET_SCISSOR);
	stub_put32(&fixture_wire, 0U);
	stub_put64(&fixture_wire, FIXTURE_CB0);
	stub_put32(&fixture_wire, 0U);
	stub_put32(&fixture_wire, 1U);
	stub_put64(&fixture_wire, 1U);
	stub_put32(&fixture_wire, 4U);
	stub_put32(&fixture_wire, 2U);
	stub_put32(&fixture_wire, 40U);
	stub_put32(&fixture_wire, 30U);
	stub_put32(&fixture_wire, FIXTURE_CMD_DRAW_INDEXED);
	stub_put32(&fixture_wire, 0U);
	stub_put64(&fixture_wire, FIXTURE_CB0);
	stub_put32(&fixture_wire, 6U);
	stub_put32(&fixture_wire, 2U);
	stub_put32(&fixture_wire, 3U);
	stub_put32(&fixture_wire, 0xfffffffeU);
	stub_put32(&fixture_wire, 1U);
	fixture_command_buffer(FIXTURE_END_COMMAND_BUFFER, FIXTURE_CB0);
	fixture_submit(FIXTURE_CB0, 0U);
	reply_bytes = stub_execute_ok(&fixture_wire);
	assert(reply_bytes == 16U);
	assert(stub_get32(stub_reply, 4U) == VK_SUCCESS);
	assert(stub_get32(stub_reply, 12U) == VK_SUCCESS);

	/* The indexed draw ran with its counts and firsts. */
	assert(stub_draw_calls == 1U);
	assert(stub_last_draw.args.indexed != 0);
	assert(stub_last_draw.args.count == 6U);
	assert(stub_last_draw.args.instance_count == 2U);
	assert(stub_last_draw.args.first == 3U);
	assert(stub_last_draw.args.vertex_offset == -2);
	assert(stub_last_draw.args.first_instance == 1U);

	/* It ran with the index buffer, and with viewport 0 and scissor 0 as set. */
	assert(stub_last_draw.state.index.buffer == buffer);
	assert(stub_last_draw.state.index.offset == 8U);
	assert(stub_last_draw.state.index.type == VK_INDEX_TYPE_UINT16);
	assert(stub_last_draw.state.viewport_set != 0);
	assert(stub_last_draw.state.viewport[0] == 0x41800000U);
	assert(stub_last_draw.state.viewport[1] == 0x41000000U);
	assert(stub_last_draw.state.viewport[2] == 0x42000000U);
	assert(stub_last_draw.state.viewport[3] == 0x41800000U);
	assert(stub_last_draw.state.viewport[4] == 0x00000000U);
	assert(stub_last_draw.state.viewport[5] == 0x3f800000U);
	assert(stub_last_draw.state.scissor_set != 0);
	assert(stub_last_draw.state.scissor.offset.x == 4);
	assert(stub_last_draw.state.scissor.offset.y == 2);
	assert(stub_last_draw.state.scissor.extent.width == 40U);
	assert(stub_last_draw.state.scissor.extent.height == 30U);

	/* Destroys the pool and the resources, and withdraws the pipeline. */
	stub_wire_begin(&fixture_wire);
	fixture_destroy(FIXTURE_DESTROY_COMMAND_POOL, FIXTURE_POOL);
	fixture_destroy(FIXTURE_DESTROY_IMAGE, FIXTURE_IMAGE);
	fixture_destroy(FIXTURE_DESTROY_BUFFER, FIXTURE_BUFFER);
	fixture_destroy(FIXTURE_FREE_MEMORY, FIXTURE_MEMORY);
	reply_bytes = stub_execute_ok(&fixture_wire);
	assert(reply_bytes == 4U * 4U);
	drv_i915_object_remove(stub_session, I915_VK_OBJ_PIPELINE, FIXTURE_PIPELINE);

	/* Closes the session; nothing stays allocated. */
	stub_session_close();
	assert(stub_live == 0U);
}

/*
 * A buffer copy runs as copies between linear surfaces of four-byte texels
 * over the two buffers: a short region as one short row; a long one as full
 * rows of 4096 texels and then its rest.  A region that is not whole
 * four-byte texels, or that runs past a buffer, fails the submission.
 */
static void
test_copy_buffer(void)
{
	size_t reply_bytes;

	/* Opens a session with the storage blob, the resources, the copy buffers, a pool and one buffer. */
	memset(&fixture_storage_object, 0, sizeof(fixture_storage_object));
	fixture_storage_object.slot = 7U;
	fixture_storage_object.bytes = sizeof(fixture_storage);
	fixture_storage_object.run.paddr = (hal_physaddr_t)(uintptr_t)fixture_storage;
	fixture_storage_object.va = FIXTURE_STORAGE_VA;
	stub_session_open(&fixture_storage_object);
	fixture_resources();
	fixture_copy_buffers();
	stub_wire_begin(&fixture_wire);
	stub_put32(&fixture_wire, FIXTURE_CREATE_COMMAND_POOL);
	stub_put32(&fixture_wire, 1U);
	stub_put64(&fixture_wire, FIXTURE_DEVICE);
	stub_put64(&fixture_wire, 1U);
	stub_put32(&fixture_wire, 39U);
	stub_put64(&fixture_wire, 0U);
	stub_put32(&fixture_wire, 0U);
	stub_put32(&fixture_wire, 0U);
	stub_put64(&fixture_wire, 0U);
	stub_put64(&fixture_wire, 1U);
	stub_put64(&fixture_wire, FIXTURE_POOL);
	stub_put32(&fixture_wire, FIXTURE_ALLOCATE_COMMAND_BUFFERS);
	stub_put32(&fixture_wire, 1U);
	stub_put64(&fixture_wire, FIXTURE_DEVICE);
	stub_put64(&fixture_wire, 1U);
	stub_put32(&fixture_wire, 40U);
	stub_put64(&fixture_wire, 0U);
	stub_put64(&fixture_wire, FIXTURE_POOL);
	stub_put32(&fixture_wire, VK_COMMAND_BUFFER_LEVEL_PRIMARY);
	stub_put32(&fixture_wire, 1U);
	stub_put64(&fixture_wire, 1U);
	stub_put64(&fixture_wire, FIXTURE_CB0);
	fixture_begin(FIXTURE_CB0);
	reply_bytes = stub_execute_ok(&fixture_wire);
	assert(reply_bytes == 24U + 24U + 8U);

	/* Counts the GPU runs of this test only. */
	stub_draw_calls = 0U;
	stub_rect_calls = 0U;

	/*
	 * Records one copy of two regions: 12 bytes from source 16 to
	 * destination 4, and 2 * 16384 + 20 bytes from source 4096 to
	 * destination 8192.
	 */
	stub_wire_begin(&fixture_wire);
	stub_put32(&fixture_wire, FIXTURE_CMD_COPY_BUFFER);
	stub_put32(&fixture_wire, 0U);
	stub_put64(&fixture_wire, FIXTURE_CB0);
	stub_put64(&fixture_wire, FIXTURE_SRC_BUFFER);
	stub_put64(&fixture_wire, FIXTURE_DST_BUFFER);
	stub_put32(&fixture_wire, 2U);
	stub_put64(&fixture_wire, 2U);
	stub_put64(&fixture_wire, 16U);
	stub_put64(&fixture_wire, 4U);
	stub_put64(&fixture_wire, 12U);
	stub_put64(&fixture_wire, 4096U);
	stub_put64(&fixture_wire, 8192U);
	stub_put64(&fixture_wire, 2U * 16384U + 20U);
	fixture_command_buffer(FIXTURE_END_COMMAND_BUFFER, FIXTURE_CB0);
	fixture_submit(FIXTURE_CB0, 0U);
	reply_bytes = stub_execute_ok(&fixture_wire);
	assert(reply_bytes == 16U);
	assert(stub_get32(stub_reply, 4U) == VK_SUCCESS);
	assert(stub_get32(stub_reply, 12U) == VK_SUCCESS);

	/* Three rectangles ran: the short region, then the long one's two full rows and its rest. */
	assert(stub_rect_calls == 3U);

	/* The short region is one row of three texels, source to destination. */
	assert(stub_rects[0].copy != 0);
	assert(stub_rects[0].src.va == FIXTURE_STORAGE_VA + FIXTURE_SRC_OFFSET + 16U);
	assert(stub_rects[0].dst.va == FIXTURE_STORAGE_VA + FIXTURE_DST_OFFSET + 4U);
	assert(stub_rects[0].src.width == 3U);
	assert(stub_rects[0].src.height == 1U);
	assert(stub_rects[0].src.pitch == 12U);
	assert(stub_rects[0].src.format == VK_FORMAT_R8G8B8A8_UNORM);
	assert(stub_rects[0].dst.width == 3U);
	assert(stub_rects[0].dst_rect.w == 3U);
	assert(stub_rects[0].src_rect.w == 3U);

	/* The long region's full rows: two rows of 4096 texels. */
	assert(stub_rects[1].src.va == FIXTURE_STORAGE_VA + FIXTURE_SRC_OFFSET + 4096U);
	assert(stub_rects[1].dst.va == FIXTURE_STORAGE_VA + FIXTURE_DST_OFFSET + 8192U);
	assert(stub_rects[1].src.width == 4096U);
	assert(stub_rects[1].src.height == 2U);
	assert(stub_rects[1].src.pitch == 16384U);
	assert(stub_rects[1].dst_rect.h == 2U);

	/* Its rest: one row of five texels after them. */
	assert(stub_rects[2].src.va == FIXTURE_STORAGE_VA + FIXTURE_SRC_OFFSET + 4096U + 32768U);
	assert(stub_rects[2].dst.va == FIXTURE_STORAGE_VA + FIXTURE_DST_OFFSET + 8192U + 32768U);
	assert(stub_rects[2].src.width == 5U);
	assert(stub_rects[2].src.height == 1U);

	/* A region of six bytes is not whole texels: the submission fails and says why. */
	stub_rect_calls = 0U;
	stub_wire_begin(&fixture_wire);
	fixture_begin(FIXTURE_CB0);
	stub_put32(&fixture_wire, FIXTURE_CMD_COPY_BUFFER);
	stub_put32(&fixture_wire, 0U);
	stub_put64(&fixture_wire, FIXTURE_CB0);
	stub_put64(&fixture_wire, FIXTURE_SRC_BUFFER);
	stub_put64(&fixture_wire, FIXTURE_DST_BUFFER);
	stub_put32(&fixture_wire, 1U);
	stub_put64(&fixture_wire, 1U);
	stub_put64(&fixture_wire, 0U);
	stub_put64(&fixture_wire, 0U);
	stub_put64(&fixture_wire, 6U);
	fixture_command_buffer(FIXTURE_END_COMMAND_BUFFER, FIXTURE_CB0);
	fixture_submit(FIXTURE_CB0, 0U);
	reply_bytes = stub_execute_ok(&fixture_wire);
	assert(reply_bytes == 24U);
	assert(stub_get32(stub_reply, 20U) == (uint32_t)VK_ERROR_INITIALIZATION_FAILED);
	assert(stub_rect_calls == 0U);
	assert(strstr(stub_log, "command buffer stopped at operation 1 of 1") != NULL);

	/* A region that runs past the destination fails the submission before any copy. */
	stub_wire_begin(&fixture_wire);
	fixture_begin(FIXTURE_CB0);
	stub_put32(&fixture_wire, FIXTURE_CMD_COPY_BUFFER);
	stub_put32(&fixture_wire, 0U);
	stub_put64(&fixture_wire, FIXTURE_CB0);
	stub_put64(&fixture_wire, FIXTURE_SRC_BUFFER);
	stub_put64(&fixture_wire, FIXTURE_DST_BUFFER);
	stub_put32(&fixture_wire, 1U);
	stub_put64(&fixture_wire, 1U);
	stub_put64(&fixture_wire, 0U);
	stub_put64(&fixture_wire, FIXTURE_COPY_BYTES - 4U);
	stub_put64(&fixture_wire, 8U);
	fixture_command_buffer(FIXTURE_END_COMMAND_BUFFER, FIXTURE_CB0);
	fixture_submit(FIXTURE_CB0, 0U);
	reply_bytes = stub_execute_ok(&fixture_wire);
	assert(reply_bytes == 24U);
	assert(stub_get32(stub_reply, 20U) == (uint32_t)VK_ERROR_INITIALIZATION_FAILED);
	assert(stub_rect_calls == 0U);

	/* Destroys the pool and the resources. */
	stub_wire_begin(&fixture_wire);
	fixture_destroy(FIXTURE_DESTROY_COMMAND_POOL, FIXTURE_POOL);
	fixture_destroy(FIXTURE_DESTROY_IMAGE, FIXTURE_IMAGE);
	fixture_destroy(FIXTURE_DESTROY_BUFFER, FIXTURE_BUFFER);
	fixture_destroy(FIXTURE_DESTROY_BUFFER, FIXTURE_SRC_BUFFER);
	fixture_destroy(FIXTURE_DESTROY_BUFFER, FIXTURE_DST_BUFFER);
	fixture_destroy(FIXTURE_FREE_MEMORY, FIXTURE_MEMORY);
	reply_bytes = stub_execute_ok(&fixture_wire);
	assert(reply_bytes == 6U * 4U);

	/* Closes the session; nothing stays allocated. */
	stub_session_close();
	assert(stub_live == 0U);
}

/* Appends a VkImageSubresourceLayers of one colour level: [aspect][level][first layer][layers]. */
static void
fixture_subresource(
	uint32_t level)
{
	/* The colour aspect of the level's one layer. */
	stub_put32(&fixture_wire, VK_IMAGE_ASPECT_COLOR_BIT);
	stub_put32(&fixture_wire, level);
	stub_put32(&fixture_wire, 0U);
	stub_put32(&fixture_wire, 1U);
}

/*
 * Appends vkCmdCopyBufferToImage or vkCmdCopyImageToBuffer of one tightly
 * packed region: a whole level of `extent` texels square, at buffer offset
 * `offset`.
 */
static void
fixture_level_copy(
	uint32_t opcode,
	uint64_t buffer,
	uint32_t level,
	uint32_t extent,
	uint64_t offset)
{
	/* The header and the buffer and image in the direction's order. */
	stub_put32(&fixture_wire, opcode);
	stub_put32(&fixture_wire, 0U);
	stub_put64(&fixture_wire, FIXTURE_CB0);
	if (opcode == FIXTURE_CMD_COPY_BUFFER_TO_IMAGE) {
		stub_put64(&fixture_wire, buffer);
		stub_put64(&fixture_wire, FIXTURE_MIP_IMAGE);
		stub_put32(&fixture_wire, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
	} else {
		stub_put64(&fixture_wire, FIXTURE_MIP_IMAGE);
		stub_put32(&fixture_wire, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
		stub_put64(&fixture_wire, buffer);
	}

	/* One region: the offset, packed rows, the level, no image offset, the extent. */
	stub_put32(&fixture_wire, 1U);
	stub_put64(&fixture_wire, 1U);
	stub_put64(&fixture_wire, offset);
	stub_put32(&fixture_wire, 0U);
	stub_put32(&fixture_wire, 0U);
	fixture_subresource(level);
	stub_put32(&fixture_wire, 0U);
	stub_put32(&fixture_wire, 0U);
	stub_put32(&fixture_wire, 0U);
	stub_put32(&fixture_wire, extent);
	stub_put32(&fixture_wire, extent);
	stub_put32(&fixture_wire, 1U);
}

/*
 * Per-level transfers of a mipmapped image: a buffer copy into level 2, a
 * linear blit from level 0 into level 1 of the same image, a clear of the
 * remaining levels from level 3 and a copy of level 5 out to a buffer each
 * address the level's own place in the mip layout, with the level's extent
 * and the image's pitch; a copy into a level the image lacks fails the
 * submission.
 */
static void
test_mip_transfers(void)
{
	size_t reply_bytes;
	uint64_t base;
	unsigned index;

	/* Opens a session with the storage blob, the resources, the copy buffers, a pool and one buffer. */
	memset(&fixture_storage_object, 0, sizeof(fixture_storage_object));
	fixture_storage_object.slot = 7U;
	fixture_storage_object.bytes = sizeof(fixture_storage);
	fixture_storage_object.run.paddr = (hal_physaddr_t)(uintptr_t)fixture_storage;
	fixture_storage_object.va = FIXTURE_STORAGE_VA;
	stub_session_open(&fixture_storage_object);
	fixture_resources();
	fixture_copy_buffers();

	/* vkCreateImage of a 32x32 R8G8B8A8_UNORM image of 6 levels, bound at FIXTURE_MIP_OFFSET. */
	stub_wire_begin(&fixture_wire);
	stub_put32(&fixture_wire, FIXTURE_CREATE_IMAGE);
	stub_put32(&fixture_wire, 1U);
	stub_put64(&fixture_wire, FIXTURE_DEVICE);
	stub_put64(&fixture_wire, 1U);
	stub_put32(&fixture_wire, 14U);
	stub_put64(&fixture_wire, 0U);
	stub_put32(&fixture_wire, 0U);
	stub_put32(&fixture_wire, VK_IMAGE_TYPE_2D);
	stub_put32(&fixture_wire, VK_FORMAT_R8G8B8A8_UNORM);
	stub_put32(&fixture_wire, 32U);
	stub_put32(&fixture_wire, 32U);
	stub_put32(&fixture_wire, 1U);
	stub_put32(&fixture_wire, 6U);
	stub_put32(&fixture_wire, 1U);
	stub_put32(&fixture_wire, VK_SAMPLE_COUNT_1_BIT);
	stub_put32(&fixture_wire, VK_IMAGE_TILING_OPTIMAL);
	stub_put32(&fixture_wire, VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
	stub_put32(&fixture_wire, VK_SHARING_MODE_EXCLUSIVE);
	stub_put32(&fixture_wire, 0U);
	stub_put64(&fixture_wire, 0U);
	stub_put32(&fixture_wire, VK_IMAGE_LAYOUT_UNDEFINED);
	stub_put64(&fixture_wire, 0U);
	stub_put64(&fixture_wire, 1U);
	stub_put64(&fixture_wire, FIXTURE_MIP_IMAGE);
	stub_put32(&fixture_wire, FIXTURE_BIND_IMAGE_MEMORY);
	stub_put32(&fixture_wire, 1U);
	stub_put64(&fixture_wire, FIXTURE_DEVICE);
	stub_put64(&fixture_wire, FIXTURE_MIP_IMAGE);
	stub_put64(&fixture_wire, FIXTURE_MEMORY);
	stub_put64(&fixture_wire, FIXTURE_MIP_OFFSET);

	/* A pool with one command buffer, begun. */
	stub_put32(&fixture_wire, FIXTURE_CREATE_COMMAND_POOL);
	stub_put32(&fixture_wire, 1U);
	stub_put64(&fixture_wire, FIXTURE_DEVICE);
	stub_put64(&fixture_wire, 1U);
	stub_put32(&fixture_wire, 39U);
	stub_put64(&fixture_wire, 0U);
	stub_put32(&fixture_wire, 0U);
	stub_put32(&fixture_wire, 0U);
	stub_put64(&fixture_wire, 0U);
	stub_put64(&fixture_wire, 1U);
	stub_put64(&fixture_wire, FIXTURE_POOL);
	stub_put32(&fixture_wire, FIXTURE_ALLOCATE_COMMAND_BUFFERS);
	stub_put32(&fixture_wire, 1U);
	stub_put64(&fixture_wire, FIXTURE_DEVICE);
	stub_put64(&fixture_wire, 1U);
	stub_put32(&fixture_wire, 40U);
	stub_put64(&fixture_wire, 0U);
	stub_put64(&fixture_wire, FIXTURE_POOL);
	stub_put32(&fixture_wire, VK_COMMAND_BUFFER_LEVEL_PRIMARY);
	stub_put32(&fixture_wire, 1U);
	stub_put64(&fixture_wire, 1U);
	stub_put64(&fixture_wire, FIXTURE_CB0);
	fixture_begin(FIXTURE_CB0);
	reply_bytes = stub_execute_ok(&fixture_wire);
	assert(reply_bytes == 24U + 8U + 24U + 24U + 8U);
	assert(stub_get32(stub_reply, 24U + 4U) == VK_SUCCESS);

	/* Counts the GPU runs of this test only. */
	stub_rect_calls = 0U;

	/* Records the copy into level 2, the blit of level 0 into level 1, the clear from level 3 and the copy out of level 5. */
	stub_wire_begin(&fixture_wire);
	fixture_level_copy(FIXTURE_CMD_COPY_BUFFER_TO_IMAGE, FIXTURE_SRC_BUFFER, 2U, 8U, 256U);

	/* vkCmdBlitImage: [src][layout][dst][layout][1][1]{src level, 2 corners, dst level, 2 corners}[filter]. */
	stub_put32(&fixture_wire, FIXTURE_CMD_BLIT_IMAGE);
	stub_put32(&fixture_wire, 0U);
	stub_put64(&fixture_wire, FIXTURE_CB0);
	stub_put64(&fixture_wire, FIXTURE_MIP_IMAGE);
	stub_put32(&fixture_wire, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
	stub_put64(&fixture_wire, FIXTURE_MIP_IMAGE);
	stub_put32(&fixture_wire, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
	stub_put32(&fixture_wire, 1U);
	stub_put64(&fixture_wire, 1U);
	fixture_subresource(0U);
	stub_put64(&fixture_wire, 2U);
	for (index = 0U; index < 3U; index++)
		stub_put32(&fixture_wire, 0U);
	stub_put32(&fixture_wire, 32U);
	stub_put32(&fixture_wire, 32U);
	stub_put32(&fixture_wire, 1U);
	fixture_subresource(1U);
	stub_put64(&fixture_wire, 2U);
	for (index = 0U; index < 3U; index++)
		stub_put32(&fixture_wire, 0U);
	stub_put32(&fixture_wire, 16U);
	stub_put32(&fixture_wire, 16U);
	stub_put32(&fixture_wire, 1U);
	stub_put32(&fixture_wire, VK_FILTER_LINEAR);

	/* vkCmdClearColorImage: [image][layout][present][tag][4]{opaque red}[1][1]{colour, level 3, the remaining levels, layer 0, 1}. */
	stub_put32(&fixture_wire, FIXTURE_CMD_CLEAR_COLOR_IMAGE);
	stub_put32(&fixture_wire, 0U);
	stub_put64(&fixture_wire, FIXTURE_CB0);
	stub_put64(&fixture_wire, FIXTURE_MIP_IMAGE);
	stub_put32(&fixture_wire, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
	stub_put64(&fixture_wire, 1U);
	stub_put32(&fixture_wire, 0U);
	stub_put64(&fixture_wire, 4U);
	stub_put32(&fixture_wire, 0x3f800000U);
	stub_put32(&fixture_wire, 0U);
	stub_put32(&fixture_wire, 0U);
	stub_put32(&fixture_wire, 0x3f800000U);
	stub_put32(&fixture_wire, 1U);
	stub_put64(&fixture_wire, 1U);
	stub_put32(&fixture_wire, VK_IMAGE_ASPECT_COLOR_BIT);
	stub_put32(&fixture_wire, 3U);
	stub_put32(&fixture_wire, VK_REMAINING_MIP_LEVELS);
	stub_put32(&fixture_wire, 0U);
	stub_put32(&fixture_wire, 1U);

	/* The copy of level 5 out to the destination buffer, then the end and the submission. */
	fixture_level_copy(FIXTURE_CMD_COPY_IMAGE_TO_BUFFER, FIXTURE_DST_BUFFER, 5U, 1U, 64U);
	fixture_command_buffer(FIXTURE_END_COMMAND_BUFFER, FIXTURE_CB0);
	fixture_submit(FIXTURE_CB0, 0U);
	reply_bytes = stub_execute_ok(&fixture_wire);
	assert(reply_bytes == 16U);
	assert(stub_get32(stub_reply, 4U) == VK_SUCCESS);
	assert(stub_get32(stub_reply, 12U) == VK_SUCCESS);

	/* Six rectangles ran: the copy in, the blit, three level fills and the copy out. */
	base = FIXTURE_STORAGE_VA + FIXTURE_MIP_OFFSET;
	assert(stub_rect_calls == 6U);

	/* The copy in writes level 2: 8x8 at row 32, column 16, with the image's pitch. */
	assert(stub_rects[0].copy != 0);
	assert(stub_rects[0].src.va == FIXTURE_STORAGE_VA + FIXTURE_SRC_OFFSET + 256U);
	assert(stub_rects[0].dst.va == base + 32U * FIXTURE_MIP_PITCH + 16U * 4U);
	assert(stub_rects[0].dst.width == 8U);
	assert(stub_rects[0].dst.height == 8U);
	assert(stub_rects[0].dst.pitch == FIXTURE_MIP_PITCH);
	assert(stub_rects[0].dst_rect.w == 8U);

	/* The blit reads all of level 0 and writes all of level 1, at row 32 of the same image. */
	assert(stub_rects[1].copy != 0);
	assert(stub_rects[1].src.va == base);
	assert(stub_rects[1].src.width == 32U);
	assert(stub_rects[1].src_rect.w == 32U);
	assert(stub_rects[1].dst.va == base + 32U * FIXTURE_MIP_PITCH);
	assert(stub_rects[1].dst.width == 16U);
	assert(stub_rects[1].dst.pitch == FIXTURE_MIP_PITCH);
	assert(stub_rects[1].dst_rect.h == 16U);

	/* The clear fills levels 3, 4 and 5 whole: rows 40, 44 and 48 at column 16. */
	assert(stub_rects[2].copy == 0);
	assert(stub_rects[2].dst.va == base + 40U * FIXTURE_MIP_PITCH + 16U * 4U);
	assert(stub_rects[2].dst_rect.w == 4U);
	assert(stub_rects[2].clear[0] == 0x3f800000U);
	assert(stub_rects[3].dst.va == base + 44U * FIXTURE_MIP_PITCH + 16U * 4U);
	assert(stub_rects[3].dst_rect.w == 2U);
	assert(stub_rects[4].dst.va == base + 48U * FIXTURE_MIP_PITCH + 16U * 4U);
	assert(stub_rects[4].dst_rect.h == 1U);

	/* The copy out reads level 5's one texel into the destination buffer. */
	assert(stub_rects[5].copy != 0);
	assert(stub_rects[5].src.va == base + 48U * FIXTURE_MIP_PITCH + 16U * 4U);
	assert(stub_rects[5].dst.va == FIXTURE_STORAGE_VA + FIXTURE_DST_OFFSET + 64U);
	assert(stub_rects[5].src_rect.w == 1U);

	/* A copy into level 6, which the image lacks, fails the submission before any rectangle. */
	stub_rect_calls = 0U;
	stub_wire_begin(&fixture_wire);
	fixture_begin(FIXTURE_CB0);
	fixture_level_copy(FIXTURE_CMD_COPY_BUFFER_TO_IMAGE, FIXTURE_SRC_BUFFER, 6U, 1U, 0U);
	fixture_command_buffer(FIXTURE_END_COMMAND_BUFFER, FIXTURE_CB0);
	fixture_submit(FIXTURE_CB0, 0U);
	reply_bytes = stub_execute_ok(&fixture_wire);
	assert(reply_bytes == 24U);
	assert(stub_get32(stub_reply, 20U) == (uint32_t)VK_ERROR_INITIALIZATION_FAILED);
	assert(stub_rect_calls == 0U);

	/* Destroys the pool and the resources. */
	stub_wire_begin(&fixture_wire);
	fixture_destroy(FIXTURE_DESTROY_COMMAND_POOL, FIXTURE_POOL);
	fixture_destroy(FIXTURE_DESTROY_IMAGE, FIXTURE_MIP_IMAGE);
	fixture_destroy(FIXTURE_DESTROY_IMAGE, FIXTURE_IMAGE);
	fixture_destroy(FIXTURE_DESTROY_BUFFER, FIXTURE_BUFFER);
	fixture_destroy(FIXTURE_DESTROY_BUFFER, FIXTURE_SRC_BUFFER);
	fixture_destroy(FIXTURE_DESTROY_BUFFER, FIXTURE_DST_BUFFER);
	fixture_destroy(FIXTURE_FREE_MEMORY, FIXTURE_MEMORY);
	reply_bytes = stub_execute_ok(&fixture_wire);
	assert(reply_bytes == 7U * 4U);

	/* Closes the session; nothing stays allocated. */
	stub_session_close();
	assert(stub_live == 0U);
}

/* Describes a linear RGBA8 image of one level in the fixture's storage at `offset`. */
static void
fixture_state_image(
	struct i915_gfx_image *image,
	struct i915_gfx_memory *memory,
	uint32_t side,
	uint64_t offset)
{
	/* A square RGBA8 image of one level. */
	memset(image, 0, sizeof(*image));
	image->format = VK_FORMAT_R8G8B8A8_UNORM;
	image->width = side;
	image->height = side;
	image->pitch = side * 4U;
	image->bytes = (uint64_t)side * side * 4U;
	image->levels = 1U;
	image->memory = memory;
	image->offset = offset;
}

/*
 * The state a draw writes for blending, sampled images and uniform blocks
 * (render/state.c): BLEND_STATE and the blend constants of COLOR_CALC_STATE
 * as the pipeline or the dynamic state says, 3DSTATE_PS_BLEND agreeing with
 * BLEND_STATE; one binding table entry, surface state and sampler per
 * sampled image of the kernel, each from its own set and binding; and the
 * push data of a stage, its push constants then each uniform block's range,
 * moved by the dynamic offset of a dynamic uniform buffer.
 */
static void
test_blend_state(void)
{
	static uint8_t page[I915_GFX_SLOT_BYTES];
	static const uint32_t sampler_sets[3] = { 0U, 0U, 1U };
	static const uint32_t sampler_bindings[3] = { 0U, 2U, 1U };
	struct i915_shader_block blocks[2];
	struct i915_gfx_pipeline pipeline;
	struct i915_gfx_draw_state state;
	struct i915_gfx_kernels kernels;
	struct i915_gem_object object;
	struct i915_gfx_memory memory;
	struct i915_gfx_image target;
	struct i915_gfx_image images[3];
	struct i915_gfx_view views[3];
	struct i915_gfx_sampler samplers[3];
	struct i915_gfx_dset sets[2];
	struct i915_gfx_buffer uniforms;
	struct i915_gfx_batch batch;
	const uint32_t *dynamic;
	const uint32_t *surface;
	const uint32_t *push;
	uint32_t commands[8];
	uint32_t *words;
	uint32_t index;
	int error;

	/* The fixture's storage at 0x7000_0000: the target at 0, three 4x4 textures after it, uniforms at 0x8000. */
	memset(&object, 0, sizeof(object));
	object.bytes = sizeof(fixture_storage);
	object.va = 0x70000000ULL;
	object.run.paddr = (hal_physaddr_t)(uintptr_t)fixture_storage;
	memset(&memory, 0, sizeof(memory));
	memory.object = &object;
	memory.size = sizeof(fixture_storage);
	fixture_state_image(&target, &memory, 16U, 0U);
	for (index = 0U; index < 3U; index++) {
		fixture_state_image(&images[index], &memory, 4U, 0x1000U + index * 0x100U);
		memset(&views[index], 0, sizeof(views[index]));
		views[index].image = &images[index];
		views[index].format = VK_FORMAT_R8G8B8A8_UNORM;
		views[index].level_count = 1U;
		memset(&samplers[index], 0, sizeof(samplers[index]));
	}

	/* Texture 0 and 2 are sampled linear, texture 1 nearest, so each sampler shows whose it is. */
	samplers[0].mag_filter = VK_FILTER_LINEAR;
	samplers[0].min_filter = VK_FILTER_LINEAR;
	samplers[2].mag_filter = VK_FILTER_LINEAR;

	/* Set 0 holds textures 0 and 1 at bindings 0 and 2, set 1 texture 2 at binding 1 and the uniforms. */
	memset(sets, 0, sizeof(sets));
	sets[0].slots[0].view = &views[0];
	sets[0].slots[0].sampler = &samplers[0];
	sets[0].slots[2].view = &views[1];
	sets[0].slots[2].sampler = &samplers[1];
	sets[1].slots[1].view = &views[2];
	sets[1].slots[1].sampler = &samplers[2];
	memset(&uniforms, 0, sizeof(uniforms));
	uniforms.size = 1024U;
	uniforms.memory = &memory;
	uniforms.offset = 0x8000U;
	sets[1].slots[3].buffer = &uniforms;
	sets[1].slots[3].offset = 64U;
	sets[1].slots[3].range = VK_WHOLE_SIZE;
	sets[1].slots[4].buffer = &uniforms;
	sets[1].slots[4].offset = 0U;
	sets[1].slots[4].range = 256U;
	sets[1].slots[4].dynamic = 1;

	/* The uniform buffer's words are their own byte offsets. */
	words = (uint32_t *)(void *)(fixture_storage + 0x8000U);
	for (index = 0U; index < 256U; index++)
		words[index] = index * 4U;

	/* A pipeline blending source alpha over the destination, alpha by ONE and ZERO, writing R, G and A. */
	memset(&pipeline, 0, sizeof(pipeline));
	pipeline.blend_enable = 1U;
	pipeline.blend_src_color = VK_BLEND_FACTOR_SRC_ALPHA;
	pipeline.blend_dst_color = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
	pipeline.blend_color_op = VK_BLEND_OP_ADD;
	pipeline.blend_src_alpha = VK_BLEND_FACTOR_ONE;
	pipeline.blend_dst_alpha = VK_BLEND_FACTOR_ZERO;
	pipeline.blend_alpha_op = VK_BLEND_OP_REVERSE_SUBTRACT;
	pipeline.blend_constants[0] = 0x3e800000U;
	pipeline.blend_constants[3] = 0x3f800000U;
	pipeline.color_write_disable = VK_COLOR_COMPONENT_B_BIT;
	pipeline.viewport[2] = 0x41800000U;
	pipeline.viewport[3] = 0x41800000U;
	pipeline.viewport[5] = 0x3f800000U;
	pipeline.scissor.extent.width = 16U;
	pipeline.scissor.extent.height = 16U;

	/* The draw state: the pipeline, both sets, push constants 0..127, dynamic offset 128 for set 1 binding 4. */
	memset(&state, 0, sizeof(state));
	state.pipeline = &pipeline;
	state.dset[0] = &sets[0];
	state.dset[1] = &sets[1];
	for (index = 0U; index < I915_GFX_PUSH_BYTES; index++)
		state.push[index] = (uint8_t)index;
	state.dynamic_offsets[1][4] = 128U;
	state.blend_constants[1] = 0x3f000000U;

	/*
	 * The kernels: a pixel kernel that samples the three textures and reads
	 * 32 bytes of push constants, then 32 bytes from byte 32 of set 1
	 * binding 3 and 32 bytes from byte 0 of set 1 binding 4.
	 */
	memset(&kernels, 0, sizeof(kernels));
	kernels.ps_samplers = 3U;
	kernels.ps_sampler_sets = sampler_sets;
	kernels.ps_sampler_bindings = sampler_bindings;
	memset(blocks, 0, sizeof(blocks));
	blocks[0].set = 1U;
	blocks[0].binding = 3U;
	blocks[0].offset = 32U;
	blocks[0].bytes = 32U;
	blocks[0].push_offset = 32U;
	blocks[1].set = 1U;
	blocks[1].binding = 4U;
	blocks[1].offset = 0U;
	blocks[1].bytes = 32U;
	blocks[1].push_offset = 64U;
	kernels.ps_push_regs = 3U;
	kernels.ps_push.regs = 3U;
	kernels.ps_push.constant_bytes = 32U;
	kernels.ps_push.block_count = 2U;
	kernels.ps_push.blocks = blocks;

	/* Writes the state. */
	error = drv_i915_gfx_write_state(page, &state, &kernels, &target, 0x6U);
	assert(error == 0);
	dynamic = (const uint32_t *)(const void *)(page + I915_GFX_DYNAMIC_HEAP);
	surface = (const uint32_t *)(const void *)(page + I915_GFX_SURFACE_HEAP);

	/*
	 * BLEND_STATE: the alpha blends by its own factors and function; the
	 * entry blends SRC_ALPHA / INV_SRC_ALPHA / ADD for the colour and ONE /
	 * ZERO / REVERSE_SUBTRACT for the alpha, blue unwritten; the clamps.
	 */
	assert(dynamic[I915_GFX_DYN_BLEND / 4U] == GEN12_BLEND_INDEPENDENT_ALPHA);
	assert(dynamic[I915_GFX_DYN_BLEND / 4U + 1U] ==
	       (GEN12_BLEND_ENABLE |
		(GEN12_BLENDFACTOR_SRC_ALPHA << GEN12_BLEND_SRC_FACTOR_SHIFT) |
		(GEN12_BLENDFACTOR_INV_SRC_ALPHA << GEN12_BLEND_DST_FACTOR_SHIFT) |
		(GEN12_BLENDFUNCTION_ADD << GEN12_BLEND_COLOR_FUNCTION_SHIFT) |
		(GEN12_BLENDFACTOR_ONE << GEN12_BLEND_SRC_ALPHA_FACTOR_SHIFT) |
		(GEN12_BLENDFACTOR_ZERO << GEN12_BLEND_DST_ALPHA_FACTOR_SHIFT) |
		(GEN12_BLENDFUNCTION_REVERSE_SUBTRACT << GEN12_BLEND_ALPHA_FUNCTION_SHIFT) |
		GEN12_BLEND_WRITE_DISABLE_BLUE));
	assert(dynamic[I915_GFX_DYN_BLEND / 4U + 2U] == (1U | (1U << 1) | (GEN12_COLORCLAMP_RTFORMAT << 2)));

	/* COLOR_CALC_STATE carries the pipeline's blend constants. */
	assert(dynamic[I915_GFX_DYN_COLOR_CALC / 4U + 2U] == 0x3e800000U);
	assert(dynamic[I915_GFX_DYN_COLOR_CALC / 4U + 3U] == 0U);
	assert(dynamic[I915_GFX_DYN_COLOR_CALC / 4U + 5U] == 0x3f800000U);

	/* 3DSTATE_PS_BLEND repeats the factors and the independent alpha, with a writeable target. */
	memset(commands, 0, sizeof(commands));
	batch.cmds = commands;
	batch.count = 0U;
	batch.capacity = 8U;
	batch.overflow = 0;
	drv_i915_gfx_emit_ps_blend(&batch, &pipeline);
	assert(batch.count == 2U);
	assert(commands[0] == GEN12_CMD_HEADER(GEN12_CMD_3DSTATE_PS_BLEND, GEN12_3DSTATE_PS_BLEND_DWORDS));
	assert(commands[1] ==
	       (GEN12_PS_BLEND_HAS_WRITEABLE_RT |
		GEN12_PS_BLEND_ENABLE |
		GEN12_PS_BLEND_INDEPENDENT_ALPHA |
		(GEN12_BLENDFACTOR_SRC_ALPHA << GEN12_PS_BLEND_SRC_FACTOR_SHIFT) |
		(GEN12_BLENDFACTOR_INV_SRC_ALPHA << GEN12_PS_BLEND_DST_FACTOR_SHIFT) |
		(GEN12_BLENDFACTOR_ONE << GEN12_PS_BLEND_SRC_ALPHA_FACTOR_SHIFT) |
		(GEN12_BLENDFACTOR_ZERO << GEN12_PS_BLEND_DST_ALPHA_FACTOR_SHIFT)));

	/*
	 * The binding table: the target at entry 0, then texture n at entry
	 * 1 + n with its surface state at RSS_TEXTURE + n * RSS_BYTES, each the
	 * image its set and binding hold.
	 */
	assert(surface[0] == I915_GFX_RSS_TARGET);
	assert(surface[I915_GFX_RSS_TARGET / 4U + 8U] == 0x70000000U);
	for (index = 0U; index < 3U; index++) {
		assert(surface[1U + index] == I915_GFX_RSS_TEXTURE + index * I915_GFX_RSS_BYTES);
		assert(surface[(I915_GFX_RSS_TEXTURE + index * I915_GFX_RSS_BYTES) / 4U + 8U] == 0x70001000U + index * 0x100U);
	}

	/* Sampler n is the sampler of texture n: linear, nearest, linear magnification only. */
	assert(((dynamic[I915_GFX_DYN_SAMPLER / 4U] >> GEN12_SAMPLER_MAG_FILTER_SHIFT) & 1U) == 1U);
	assert(((dynamic[I915_GFX_DYN_SAMPLER / 4U] >> GEN12_SAMPLER_MIN_FILTER_SHIFT) & 1U) == 1U);
	assert(((dynamic[(I915_GFX_DYN_SAMPLER + I915_GFX_SAMPLER_BYTES) / 4U] >> GEN12_SAMPLER_MAG_FILTER_SHIFT) & 1U) == 0U);
	assert(((dynamic[(I915_GFX_DYN_SAMPLER + 2U * I915_GFX_SAMPLER_BYTES) / 4U] >> GEN12_SAMPLER_MAG_FILTER_SHIFT) & 1U) == 1U);
	assert(((dynamic[(I915_GFX_DYN_SAMPLER + 2U * I915_GFX_SAMPLER_BYTES) / 4U] >> GEN12_SAMPLER_MIN_FILTER_SHIFT) & 1U) == 0U);

	/*
	 * The pixel push data: push constants 0..31; binding 3's bytes 32..63
	 * of its range at 64 (buffer bytes 96..127); binding 4's bytes 0..31 at
	 * its dynamic offset 128 (buffer bytes 128..159).
	 */
	push = (const uint32_t *)(const void *)(page + I915_GFX_PS_PUSH_BUFFER);
	assert(page[I915_GFX_PS_PUSH_BUFFER] == 0U);
	assert(page[I915_GFX_PS_PUSH_BUFFER + 31U] == 31U);
	for (index = 0U; index < 8U; index++) {
		assert(push[8U + index] == 96U + index * 4U);
		assert(push[16U + index] == 128U + index * 4U);
	}

	/* A pipeline whose blend constants are dynamic takes the ones the command buffer set. */
	pipeline.dynamic_blend_constants = 1;
	state.blend_constants_set = 1;
	error = drv_i915_gfx_write_state(page, &state, &kernels, &target, 0x6U);
	assert(error == 0);
	assert(dynamic[I915_GFX_DYN_COLOR_CALC / 4U + 2U] == 0U);
	assert(dynamic[I915_GFX_DYN_COLOR_CALC / 4U + 3U] == 0x3f000000U);

	/* MIN and MAX use no factors: the hardware is given ONE for both, and the same colour and alpha blend together. */
	pipeline.blend_src_color = VK_BLEND_FACTOR_SRC_ALPHA;
	pipeline.blend_dst_color = VK_BLEND_FACTOR_DST_COLOR;
	pipeline.blend_color_op = VK_BLEND_OP_MAX;
	pipeline.blend_src_alpha = VK_BLEND_FACTOR_SRC_ALPHA;
	pipeline.blend_dst_alpha = VK_BLEND_FACTOR_DST_COLOR;
	pipeline.blend_alpha_op = VK_BLEND_OP_MAX;
	pipeline.color_write_disable = 0U;
	error = drv_i915_gfx_write_state(page, &state, &kernels, &target, 0x6U);
	assert(error == 0);
	assert(dynamic[I915_GFX_DYN_BLEND / 4U] == 0U);
	assert(dynamic[I915_GFX_DYN_BLEND / 4U + 1U] ==
	       (GEN12_BLEND_ENABLE |
		(GEN12_BLENDFACTOR_ONE << GEN12_BLEND_SRC_FACTOR_SHIFT) |
		(GEN12_BLENDFACTOR_ONE << GEN12_BLEND_DST_FACTOR_SHIFT) |
		(GEN12_BLENDFUNCTION_MAX << GEN12_BLEND_COLOR_FUNCTION_SHIFT) |
		(GEN12_BLENDFACTOR_ONE << GEN12_BLEND_SRC_ALPHA_FACTOR_SHIFT) |
		(GEN12_BLENDFACTOR_ONE << GEN12_BLEND_DST_ALPHA_FACTOR_SHIFT) |
		(GEN12_BLENDFUNCTION_MAX << GEN12_BLEND_ALPHA_FUNCTION_SHIFT)));

	/* A constant factor and SUBTRACT: CONST_COLOR and INV_CONST_ALPHA. */
	pipeline.blend_src_color = VK_BLEND_FACTOR_CONSTANT_COLOR;
	pipeline.blend_dst_color = VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
	pipeline.blend_color_op = VK_BLEND_OP_SUBTRACT;
	pipeline.blend_src_alpha = VK_BLEND_FACTOR_CONSTANT_COLOR;
	pipeline.blend_dst_alpha = VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
	pipeline.blend_alpha_op = VK_BLEND_OP_SUBTRACT;
	error = drv_i915_gfx_write_state(page, &state, &kernels, &target, 0x6U);
	assert(error == 0);
	assert(dynamic[I915_GFX_DYN_BLEND / 4U + 1U] ==
	       (GEN12_BLEND_ENABLE |
		(GEN12_BLENDFACTOR_CONST_COLOR << GEN12_BLEND_SRC_FACTOR_SHIFT) |
		(GEN12_BLENDFACTOR_INV_CONST_ALPHA << GEN12_BLEND_DST_FACTOR_SHIFT) |
		(GEN12_BLENDFUNCTION_SUBTRACT << GEN12_BLEND_COLOR_FUNCTION_SHIFT) |
		(GEN12_BLENDFACTOR_CONST_COLOR << GEN12_BLEND_SRC_ALPHA_FACTOR_SHIFT) |
		(GEN12_BLENDFACTOR_INV_CONST_ALPHA << GEN12_BLEND_DST_ALPHA_FACTOR_SHIFT) |
		(GEN12_BLENDFUNCTION_SUBTRACT << GEN12_BLEND_ALPHA_FUNCTION_SHIFT)));

	/* An equation that reads a second source is not blended; blending off leaves only the write mask. */
	pipeline.blend_dst_alpha = VK_BLEND_FACTOR_SRC1_ALPHA;
	pipeline.color_write_disable = VK_COLOR_COMPONENT_A_BIT;
	error = drv_i915_gfx_write_state(page, &state, &kernels, &target, 0x6U);
	assert(error == 0);
	assert(dynamic[I915_GFX_DYN_BLEND / 4U] == 0U);
	assert(dynamic[I915_GFX_DYN_BLEND / 4U + 1U] == GEN12_BLEND_WRITE_DISABLE_ALPHA);
	batch.count = 0U;
	drv_i915_gfx_emit_ps_blend(&batch, &pipeline);
	assert(commands[1] == GEN12_PS_BLEND_HAS_WRITEABLE_RT);

	/* A sampled image whose set lacks it, and a uniform block with no buffer, refuse the draw. */
	sets[0].slots[2].view = NULL;
	error = drv_i915_gfx_write_state(page, &state, &kernels, &target, 0x6U);
	assert(error == EINVAL);
	assert(strstr(stub_log, "set 0 binding 2 has no image view and sampler") != NULL);
	sets[0].slots[2].view = &views[1];
	sets[1].slots[3].buffer = NULL;
	error = drv_i915_gfx_write_state(page, &state, &kernels, &target, 0x6U);
	assert(error == EINVAL);
	assert(strstr(stub_log, "set 1 binding 3 has no uniform buffer") != NULL);

	/* A dynamic offset past the range reads zeros. */
	sets[1].slots[3].buffer = &uniforms;
	state.dynamic_offsets[1][4] = 1024U;
	error = drv_i915_gfx_write_state(page, &state, &kernels, &target, 0x6U);
	assert(error == 0);
	for (index = 0U; index < 8U; index++)
		assert(push[16U + index] == 0U);
}

/* Appends vkCreateDescriptorSetLayout of bindings (number, type) for the fragment stage. */
static void
fixture_dsl(
	uint64_t identity,
	const uint32_t *numbers,
	const uint32_t *types,
	uint32_t count)
{
	uint32_t index;

	/* [72][reply][device][present][sType 32][no chain][flags][count][count]{binding}[no allocator][present][identity]. */
	stub_put32(&fixture_wire, FIXTURE_CREATE_DSL);
	stub_put32(&fixture_wire, 1U);
	stub_put64(&fixture_wire, FIXTURE_DEVICE);
	stub_put64(&fixture_wire, 1U);
	stub_put32(&fixture_wire, 32U);
	stub_put64(&fixture_wire, 0U);
	stub_put32(&fixture_wire, 0U);
	stub_put32(&fixture_wire, count);
	stub_put64(&fixture_wire, count);
	for (index = 0U; index < count; index++) {
		stub_put32(&fixture_wire, numbers[index]);
		stub_put32(&fixture_wire, types[index]);
		stub_put32(&fixture_wire, 1U);
		stub_put32(&fixture_wire, VK_SHADER_STAGE_FRAGMENT_BIT);
		stub_put64(&fixture_wire, 0U);
	}
	stub_put64(&fixture_wire, 0U);
	stub_put64(&fixture_wire, 1U);
	stub_put64(&fixture_wire, identity);
}

/* Appends one VkWriteDescriptorSet of one buffer descriptor: [sType 35][no chain][set][binding][0][1][type][0][1]{buffer offset range}[0]. */
static void
fixture_buffer_write(
	uint64_t set,
	uint32_t binding,
	uint32_t type,
	uint64_t offset,
	uint64_t range)
{
	/* The write's head. */
	stub_put32(&fixture_wire, 35U);
	stub_put64(&fixture_wire, 0U);
	stub_put64(&fixture_wire, set);
	stub_put32(&fixture_wire, binding);
	stub_put32(&fixture_wire, 0U);
	stub_put32(&fixture_wire, 1U);
	stub_put32(&fixture_wire, type);

	/* No images, one buffer, no texel views. */
	stub_put64(&fixture_wire, 0U);
	stub_put64(&fixture_wire, 1U);
	stub_put64(&fixture_wire, FIXTURE_BUFFER);
	stub_put64(&fixture_wire, offset);
	stub_put64(&fixture_wire, range);
	stub_put64(&fixture_wire, 0U);
}

/* Appends vkCmdBindDescriptorSets of one set at `first` with its dynamic offsets. */
static void
fixture_bind_set(
	uint64_t cmdbuf,
	uint32_t first,
	uint64_t set,
	const uint32_t *offsets,
	uint32_t offset_count)
{
	uint32_t index;

	/* [103][no reply][command buffer][bind point][layout][first][1][1]{set}[count][count]{offset}. */
	stub_put32(&fixture_wire, FIXTURE_CMD_BIND_DESCRIPTOR_SETS);
	stub_put32(&fixture_wire, 0U);
	stub_put64(&fixture_wire, cmdbuf);
	stub_put32(&fixture_wire, VK_PIPELINE_BIND_POINT_GRAPHICS);
	stub_put64(&fixture_wire, 0U);
	stub_put32(&fixture_wire, first);
	stub_put32(&fixture_wire, 1U);
	stub_put64(&fixture_wire, 1U);
	stub_put64(&fixture_wire, set);
	stub_put32(&fixture_wire, offset_count);
	stub_put64(&fixture_wire, offset_count);
	for (index = 0U; index < offset_count; index++)
		stub_put32(&fixture_wire, offsets[index]);
}

/*
 * Uniform buffers, plain and dynamic, reach a draw through the wire:
 * vkUpdateDescriptorSets binds a uniform buffer and a dynamic one (the
 * dynamic flag kept with the slot); vkCmdBindDescriptorSets gives the
 * dynamic buffers their offsets in binding order; vkCmdSetBlendConstants
 * reaches the draw state.  Too few or too many dynamic offsets are refused.
 */
static void
test_uniform_bindings(void)
{
	static const uint32_t numbers[3] = { 3U, 0U, 1U };
	static const uint32_t types[3] = {
		VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC,
		VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC,
		VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
	};
	static const uint32_t offsets[3] = { 64U, 192U, 7U };
	struct i915_gfx_buffer *buffer;
	struct i915_gfx_dset *dset;
	size_t reply_bytes;
	int error;

	/* Opens a session with the storage blob, the resources, the pipeline, a pool and one buffer. */
	memset(&fixture_storage_object, 0, sizeof(fixture_storage_object));
	fixture_storage_object.slot = 7U;
	fixture_storage_object.bytes = sizeof(fixture_storage);
	fixture_storage_object.run.paddr = (hal_physaddr_t)(uintptr_t)fixture_storage;
	fixture_storage_object.va = FIXTURE_STORAGE_VA;
	stub_session_open(&fixture_storage_object);
	fixture_resources();
	buffer = drv_i915_object_lookup(stub_session, I915_VK_OBJ_BUFFER, FIXTURE_BUFFER);
	assert(buffer != NULL);
	memset(&fixture_pipeline, 0, sizeof(fixture_pipeline));
	fixture_pipeline.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
	fixture_pipeline.dynamic_blend_constants = 1;
	error = drv_i915_object_insert(stub_session, I915_VK_OBJ_PIPELINE, FIXTURE_PIPELINE, &fixture_pipeline);
	assert(error == 0);

	/* A layout of dynamic buffers at bindings 3 and 0 and a plain one at 1; a pool; one set of it. */
	stub_wire_begin(&fixture_wire);
	fixture_dsl(FIXTURE_DSL, numbers, types, 3U);
	stub_put32(&fixture_wire, FIXTURE_CREATE_DESCRIPTOR_POOL);
	stub_put32(&fixture_wire, 1U);
	stub_put64(&fixture_wire, FIXTURE_DEVICE);
	stub_put64(&fixture_wire, 1U);
	stub_put32(&fixture_wire, 33U);
	stub_put64(&fixture_wire, 0U);
	stub_put32(&fixture_wire, 0U);
	stub_put32(&fixture_wire, 1U);
	stub_put32(&fixture_wire, 1U);
	stub_put64(&fixture_wire, 1U);
	stub_put32(&fixture_wire, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);
	stub_put32(&fixture_wire, 3U);
	stub_put64(&fixture_wire, 0U);
	stub_put64(&fixture_wire, 1U);
	stub_put64(&fixture_wire, FIXTURE_DESCRIPTOR_POOL);
	stub_put32(&fixture_wire, FIXTURE_ALLOCATE_DESCRIPTOR_SETS);
	stub_put32(&fixture_wire, 1U);
	stub_put64(&fixture_wire, FIXTURE_DEVICE);
	stub_put64(&fixture_wire, 1U);
	stub_put32(&fixture_wire, 34U);
	stub_put64(&fixture_wire, 0U);
	stub_put64(&fixture_wire, FIXTURE_DESCRIPTOR_POOL);
	stub_put32(&fixture_wire, 1U);
	stub_put64(&fixture_wire, 1U);
	stub_put64(&fixture_wire, FIXTURE_DSL);
	stub_put64(&fixture_wire, 1U);
	stub_put64(&fixture_wire, FIXTURE_SET);
	reply_bytes = stub_execute_ok(&fixture_wire);
	assert(reply_bytes == 24U + 24U + 24U);
	assert(stub_get32(stub_reply, 72U - 20U) == VK_SUCCESS);
	dset = drv_i915_object_lookup(stub_session, I915_VK_OBJ_DESCRIPTOR_SET, FIXTURE_SET);
	assert(dset != NULL);

	/* vkUpdateDescriptorSets: the three buffers, each a range of the vertex buffer. */
	stub_wire_begin(&fixture_wire);
	stub_put32(&fixture_wire, FIXTURE_UPDATE_DESCRIPTOR_SETS);
	stub_put32(&fixture_wire, 1U);
	stub_put64(&fixture_wire, FIXTURE_DEVICE);
	stub_put32(&fixture_wire, 3U);
	stub_put64(&fixture_wire, 3U);
	fixture_buffer_write(FIXTURE_SET, 3U, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 16U, 64U);
	fixture_buffer_write(FIXTURE_SET, 0U, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 0U, VK_WHOLE_SIZE);
	fixture_buffer_write(FIXTURE_SET, 1U, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 32U, 32U);
	stub_put32(&fixture_wire, 0U);
	stub_put64(&fixture_wire, 0U);
	reply_bytes = stub_execute_ok(&fixture_wire);
	assert(reply_bytes == 4U);

	/* Each slot holds its buffer and range; only the dynamic ones say so. */
	assert(dset->slots[3].buffer == buffer);
	assert(dset->slots[3].offset == 16U);
	assert(dset->slots[3].range == 64U);
	assert(dset->slots[3].dynamic != 0);
	assert(dset->slots[0].buffer == buffer);
	assert(dset->slots[0].range == VK_WHOLE_SIZE);
	assert(dset->slots[0].dynamic != 0);
	assert(dset->slots[1].buffer == buffer);
	assert(dset->slots[1].offset == 32U);
	assert(dset->slots[1].dynamic == 0);

	/* A pool and a command buffer. */
	stub_wire_begin(&fixture_wire);
	stub_put32(&fixture_wire, FIXTURE_CREATE_COMMAND_POOL);
	stub_put32(&fixture_wire, 1U);
	stub_put64(&fixture_wire, FIXTURE_DEVICE);
	stub_put64(&fixture_wire, 1U);
	stub_put32(&fixture_wire, 39U);
	stub_put64(&fixture_wire, 0U);
	stub_put32(&fixture_wire, 0U);
	stub_put32(&fixture_wire, 0U);
	stub_put64(&fixture_wire, 0U);
	stub_put64(&fixture_wire, 1U);
	stub_put64(&fixture_wire, FIXTURE_POOL);
	stub_put32(&fixture_wire, FIXTURE_ALLOCATE_COMMAND_BUFFERS);
	stub_put32(&fixture_wire, 1U);
	stub_put64(&fixture_wire, FIXTURE_DEVICE);
	stub_put64(&fixture_wire, 1U);
	stub_put32(&fixture_wire, 40U);
	stub_put64(&fixture_wire, 0U);
	stub_put64(&fixture_wire, FIXTURE_POOL);
	stub_put32(&fixture_wire, VK_COMMAND_BUFFER_LEVEL_PRIMARY);
	stub_put32(&fixture_wire, 1U);
	stub_put64(&fixture_wire, 1U);
	stub_put64(&fixture_wire, FIXTURE_CB0);
	fixture_begin(FIXTURE_CB0);
	reply_bytes = stub_execute_ok(&fixture_wire);
	assert(reply_bytes == 24U + 24U + 8U);

	/*
	 * Records the pipeline, the set as set 1 with dynamic offsets 64 and
	 * 192 (binding 0 takes the first, binding 3 the second), the blend
	 * constants (0.25, 0.5, 0.75, 1) and a draw.
	 */
	stub_draw_calls = 0U;
	stub_wire_begin(&fixture_wire);
	stub_put32(&fixture_wire, FIXTURE_CMD_BIND_PIPELINE);
	stub_put32(&fixture_wire, 0U);
	stub_put64(&fixture_wire, FIXTURE_CB0);
	stub_put32(&fixture_wire, VK_PIPELINE_BIND_POINT_GRAPHICS);
	stub_put64(&fixture_wire, FIXTURE_PIPELINE);
	fixture_bind_set(FIXTURE_CB0, 1U, FIXTURE_SET, offsets, 2U);
	stub_put32(&fixture_wire, FIXTURE_CMD_SET_BLEND_CONSTANTS);
	stub_put32(&fixture_wire, 0U);
	stub_put64(&fixture_wire, FIXTURE_CB0);
	stub_put64(&fixture_wire, 4U);
	stub_put32(&fixture_wire, 0x3e800000U);
	stub_put32(&fixture_wire, 0x3f000000U);
	stub_put32(&fixture_wire, 0x3f400000U);
	stub_put32(&fixture_wire, 0x3f800000U);
	fixture_draw(FIXTURE_CB0, 3U, 1U);
	fixture_command_buffer(FIXTURE_END_COMMAND_BUFFER, FIXTURE_CB0);
	fixture_submit(FIXTURE_CB0, 0U);
	reply_bytes = stub_execute_ok(&fixture_wire);
	assert(reply_bytes == 16U);
	assert(stub_get32(stub_reply, 4U) == VK_SUCCESS);
	assert(stub_get32(stub_reply, 12U) == VK_SUCCESS);

	/* The draw ran with the set as set 1, its dynamic offsets by binding and the blend constants. */
	assert(stub_draw_calls == 1U);
	assert(stub_last_draw.state.dset[1] == dset);
	assert(stub_last_draw.state.dynamic_offsets[1][0] == 64U);
	assert(stub_last_draw.state.dynamic_offsets[1][3] == 192U);
	assert(stub_last_draw.state.dynamic_offsets[1][1] == 0U);
	assert(stub_last_draw.state.blend_constants_set != 0);
	assert(stub_last_draw.state.blend_constants[0] == 0x3e800000U);
	assert(stub_last_draw.state.blend_constants[3] == 0x3f800000U);

	/* One offset for two dynamic buffers, and three, are refused while recording. */
	stub_wire_begin(&fixture_wire);
	fixture_begin(FIXTURE_CB0);
	fixture_bind_set(FIXTURE_CB0, 0U, FIXTURE_SET, offsets, 1U);
	error = stub_execute(&fixture_wire, &reply_bytes);
	assert(error == EINVAL);
	assert(strstr(stub_log, "1 dynamic offsets for more dynamic uniform buffers") != NULL);
	stub_wire_begin(&fixture_wire);
	fixture_begin(FIXTURE_CB0);
	fixture_bind_set(FIXTURE_CB0, 0U, FIXTURE_SET, offsets, 3U);
	error = stub_execute(&fixture_wire, &reply_bytes);
	assert(error == EINVAL);
	assert(strstr(stub_log, "3 dynamic offsets for 2 dynamic uniform buffers") != NULL);

	/*
	 * Destroys the pool, the resources and the layout, and withdraws the
	 * pipeline.  XXX: no command frees a descriptor set (see the res
	 * fixture); the fixture unpublishes and frees it itself.
	 */
	drv_i915_object_remove(stub_session, I915_VK_OBJ_DESCRIPTOR_SET, FIXTURE_SET);
	kern_free(dset);
	stub_wire_begin(&fixture_wire);
	fixture_destroy(FIXTURE_DESTROY_COMMAND_POOL, FIXTURE_POOL);
	fixture_destroy(FIXTURE_DESTROY_DESCRIPTOR_POOL, FIXTURE_DESCRIPTOR_POOL);
	fixture_destroy(FIXTURE_DESTROY_DSL, FIXTURE_DSL);
	fixture_destroy(FIXTURE_DESTROY_IMAGE, FIXTURE_IMAGE);
	fixture_destroy(FIXTURE_DESTROY_BUFFER, FIXTURE_BUFFER);
	fixture_destroy(FIXTURE_FREE_MEMORY, FIXTURE_MEMORY);
	reply_bytes = stub_execute_ok(&fixture_wire);
	assert(reply_bytes == 6U * 4U);
	drv_i915_object_remove(stub_session, I915_VK_OBJ_PIPELINE, FIXTURE_PIPELINE);

	/* Closes the session; nothing stays allocated. */
	stub_session_close();
	assert(stub_live == 0U);
}
