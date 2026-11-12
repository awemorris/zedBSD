/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The host test of the execution of recorded commands by the GHCI 1.0
 * driver's Vulkan executor (vk/vk-execute.c, vk/vk-pass.c and the
 * submission of vk/vk-queue.c) against the GPU model and the kernel stubs
 * (design section 14, the scenes of p009): one frame's recording and the
 * packets the model receives, compared with a sequence written here by
 * hand; the binds a draw or dispatch needs; indirect draws on a device
 * without CAP_INDIRECT_DRAW; barriers; transfers with and without the
 * blitter; queries, timestamps and events; clears; a submission larger
 * than the ring; the vertex buffer's range; a triangle fan; the heap
 * runs a destroyed pipeline keeps until the submission that used it has
 * passed; and the submissions held back behind a wait for an event the
 * host or an earlier submission sets (design section 11.4).
 *
 * It is compiled within ghci-pipeline-host-test.c (which includes it at
 * its end) for that file's helpers, and runs when the binary is given
 * "execute".  Every expected packet word is written here from the
 * specification (ghci.md d9252e4 section 7.3 and 8.4) and the driver's
 * design (section 11.3 to 11.5).
 */

/* The identities of the execution scenes' objects. */
#define ID_X_QUEUE			0x2000U
#define ID_X_POOL			0x2100U
#define ID_X_COMMANDS			0x2200U
#define ID_X_IMAGE			0x2300U
#define ID_X_VIEW			0x2400U
#define ID_X_FRAMEBUFFER		0x2500U
#define ID_X_PASS			0x2600U
#define ID_X_QUERIES			0x2700U
#define ID_X_OCCLUSION			0x2701U
#define ID_X_EVENT			0x2800U
#define ID_X_FENCE			0x2900U
#define ID_X_COMPUTE			0x2a00U

/* Where the scenes bind the colour attachment in the memory (64 x 32 R8G8B8A8, 8 KiB). */
#define X_ATTACHMENT_OFFSET		0x40000U

/* The most packets of one submission the scenes look at. */
#define X_OPS_MAX			256U

/* The fills of the buffer's first word the hold scenes count, at most. */
#define X_FILLS_MAX			512U

static int x_open(struct node *node, enum model_profile profile);
static void x_record(const struct node *node, uint64_t commands);
static void x_command(uint32_t opcode, uint64_t commands);
static int x_end(struct node *node, uint64_t commands);
static int x_submit(struct node *node, uint64_t commands, uint64_t fence);
static unsigned x_ops(unsigned mark, unsigned *first, uint32_t *ops);
static int x_same(const uint32_t *ops, unsigned count, const uint32_t *expected, unsigned expected_count);
static const uint8_t *x_gpu(uint32_t session, uint64_t va);
static uint64_t x_load(const uint8_t *bytes, uint32_t size);
static uint64_t x_va(const struct node *node, uint64_t offset);
static void x_begin_pass(uint64_t commands, uint32_t width, uint32_t height);
static void x_bind_triangle(uint64_t commands, uint64_t pipeline, uint64_t offset);
static uint32_t x_triangle(struct node *node, uint32_t topology, uint32_t dynamic, uint64_t identity);
static int x_compute_set(struct node *node);
static void x_query_results(struct node *node, uint64_t pool, uint32_t first, uint32_t count);
static void scene_x_frame(void);
static void scene_x_binds(void);
static void scene_x_indirect(void);
static void scene_x_barriers(void);
static void scene_x_transfers(void);
static void scene_x_plain_blitter(void);
static void scene_x_queries(void);
static void scene_x_clears(void);
static void scene_x_large(void);
static void scene_x_fan(void);
static void scene_x_holds(void);
static int x_image(struct node *node, uint64_t identity, uint64_t view, uint32_t format, uint32_t samples, uint32_t usage, uint64_t offset);
static void scene_x_subpasses(void);
static void scene_x_secondary(void);
static void x_indirect(uint64_t commands, uint32_t opcode, uint64_t offset, uint32_t draws, uint32_t stride);
static void x_begin_with(uint64_t commands, uint64_t pass, uint64_t framebuffer, uint32_t count);
static void x_clear_color0(uint64_t commands, uint32_t first, uint32_t count);
static uint32_t x_framebuffer(struct node *node, uint64_t identity, uint64_t pass, uint64_t view, uint32_t width, uint32_t height, uint32_t layers);
static void scene_x_ranges(void);
static void scene_x_events(void);
static void scene_x_regions(void);
static void scene_x_pass_rules(void);
static void scene_x_layers(void);
static void x_fault_hook(unsigned sleep, void *argument);
static void scene_x_partial(void);
static void x_wait(uint64_t commands, uint64_t event);
static void x_fill(uint64_t commands, uint32_t value);
static int x_host_event(struct node *node, uint32_t opcode, uint64_t event);
static uint32_t x_fence_status(struct node *node, uint64_t fence);
static int x_create(struct node *node, uint32_t opcode, uint32_t type, uint64_t identity);
static unsigned x_fills(unsigned mark, uint32_t *values);
static int x_other_open(struct node *node);
static void scene_x_hold(void);
static void scene_x_hold_fail(void);
static void scene_x_hold_pieces(void);
static unsigned x_piece_fills(unsigned mark, unsigned *fills, unsigned capacity);
static unsigned x_find(unsigned mark, uint32_t opcode, unsigned nth);
static void x_dispatch(uint64_t commands);
static void x_dispatch_bound(uint64_t commands);
static void x_pass_indexed(uint64_t commands, int bound);
static void x_set_event(uint64_t commands, uint32_t opcode, uint64_t event);
static void scene_x_hold_rebind(void);
static void scene_x_hold_stretch(void);
static void scene_x_hold_ends(void);
static void scene_x_hold_lost(void);
static void scene_x_hold_budget(void);

/* Runs the scenes of the execution and reports.  Returns the exit status. */
static int
execute_main(
	const char *directory,
	const char *compiler_directory)
{
	int error;

	/* The modules. */
	error = load_modules(directory, compiler_directory);
	if (error != 0) {
		printf("FAIL ghci-execute-host-test: the SPIR-V modules are not there\n");
		return 1;
	}

	/* Keeps the driver's log out of the report. */
	stub.quiet = 1;

	/* The scenes. */
	scene_x_frame();
	scene_x_binds();
	scene_x_indirect();
	scene_x_barriers();
	scene_x_transfers();
	scene_x_plain_blitter();
	scene_x_queries();
	scene_x_clears();
	scene_x_large();
	scene_x_fan();
	scene_x_holds();
	scene_x_subpasses();
	scene_x_secondary();
	scene_x_ranges();
	scene_x_events();
	scene_x_regions();
	scene_x_pass_rules();
	scene_x_layers();
	scene_x_partial();
	scene_x_hold();
	scene_x_hold_fail();
	scene_x_hold_pieces();
	scene_x_hold_rebind();
	scene_x_hold_stretch();
	scene_x_hold_ends();
	scene_x_hold_lost();
	scene_x_hold_budget();
	model_fini();

	/* The counts. */
	printf("ghci-execute-host-test: %u passed, %u failed\n", checks_passed, checks_failed);
	if (checks_failed != 0U) {
		printf("FAIL ghci-execute-host-test\n");
		return 1;
	}

	/* Succeeded: every check held. */
	printf("PASS ghci-execute-host-test\n");
	return 0;
}

/*
 * Opens a device of the profile (open_device) with a queue, a command
 * pool and three primary command buffers, a 64 x 32 R8G8B8A8 colour
 * attachment image with its view, a render pass of it (cleared and
 * stored), a framebuffer, the graphics pipelines' layout (ID_LAYOUT + 1,
 * an empty set) and a fence.  Returns 0, or 1.
 */
static int
x_open(
	struct node *node,
	enum model_profile profile)
{
	static const uint64_t graphics_set = ID_SET_LAYOUT + 1U;
	struct test_pass pass;
	uint32_t index;
	int error;

	/* The device, then vkGetDeviceQueue2 of queue 0 on timeline 1. */
	error = open_device(node, profile);
	if (error != 0)
		return 1;
	begin(node, GPU_OP_GET_DEVICE_QUEUE2);
	put64(ID_DEVICE);
	put64(1U);
	put32(VK_STRUCTURE_TYPE_DEVICE_QUEUE_INFO_2);
	put64(1U);
	put32(GPU_OP_RECORD_QUEUE_TIMELINE);
	put64(0U);
	put32(1U);
	put32(0U);
	put32(0U);
	put32(0U);
	put64(1U);
	put64(ID_X_QUEUE);
	error = run(node);
	if (error != 0)
		return 1;

	/* The pool and the buffers. */
	begin(node, GPU_OP_CREATE_COMMAND_POOL);
	put64(ID_DEVICE);
	put64(1U);
	put32(VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO);
	put64(0U);
	put32(0U);
	put32(0U);
	tail(ID_X_POOL);
	error = run(node);
	if (error != 0 || reply32(node, 4U) != 0U)
		return 1;
	begin(node, GPU_OP_ALLOCATE_COMMAND_BUFFERS);
	put64(ID_DEVICE);
	put64(1U);
	put32(VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO);
	put64(0U);
	put64(ID_X_POOL);
	put32(VK_COMMAND_BUFFER_LEVEL_PRIMARY);
	put32(3U);
	put64(3U);
	for (index = 0U; index < 3U; index++)
		put64(ID_X_COMMANDS + index);
	error = run(node);
	if (error != 0 || reply32(node, 4U) != 0U)
		return 1;

	/* The attachment image, bound at X_ATTACHMENT_OFFSET, and its view. */
	begin(node, GPU_OP_CREATE_IMAGE);
	put64(ID_DEVICE);
	put64(1U);
	put32(VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO);
	put64(0U);
	put32(0U);
	put32(VK_IMAGE_TYPE_2D);
	put32(VK_FORMAT_R8G8B8A8_UNORM);
	put32(64U);
	put32(32U);
	put32(1U);
	put32(1U);
	put32(1U);
	put32(VK_SAMPLE_COUNT_1_BIT);
	put32(VK_IMAGE_TILING_OPTIMAL);
	put32(VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
	put32(VK_SHARING_MODE_EXCLUSIVE);
	put32(0U);
	put64(0U);
	put32(VK_IMAGE_LAYOUT_UNDEFINED);
	tail(ID_X_IMAGE);
	error = run(node);
	if (error != 0 || reply32(node, 4U) != 0U)
		return 1;
	begin(node, GPU_OP_BIND_IMAGE_MEMORY);
	put64(ID_DEVICE);
	put64(ID_X_IMAGE);
	put64(ID_MEMORY);
	put64(X_ATTACHMENT_OFFSET);
	error = run(node);
	if (error != 0 || reply32(node, 4U) != 0U)
		return 1;
	begin(node, GPU_OP_CREATE_IMAGE_VIEW);
	put64(ID_DEVICE);
	put64(1U);
	put32(VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO);
	put64(0U);
	put32(0U);
	put64(ID_X_IMAGE);
	put32(VK_IMAGE_VIEW_TYPE_2D);
	put32(VK_FORMAT_R8G8B8A8_UNORM);
	put32(VK_COMPONENT_SWIZZLE_IDENTITY);
	put32(VK_COMPONENT_SWIZZLE_IDENTITY);
	put32(VK_COMPONENT_SWIZZLE_IDENTITY);
	put32(VK_COMPONENT_SWIZZLE_IDENTITY);
	put32(VK_IMAGE_ASPECT_COLOR_BIT);
	put32(0U);
	put32(1U);
	put32(0U);
	put32(1U);
	tail(ID_X_VIEW);
	error = run(node);
	if (error != 0 || reply32(node, 4U) != 0U)
		return 1;

	/* The render pass: one colour attachment, cleared and stored. */
	memset(&pass, 0, sizeof(pass));
	pass.attachment_count = 1U;
	pass.format[0] = VK_FORMAT_R8G8B8A8_UNORM;
	pass.samples[0] = VK_SAMPLE_COUNT_1_BIT;
	pass.load[0] = VK_ATTACHMENT_LOAD_OP_CLEAR;
	pass.store[0] = VK_ATTACHMENT_STORE_OP_STORE;
	pass.subpass_count = 1U;
	pass.color_count[0] = 1U;
	pass.color[0][0] = 0U;
	pass.depth[0] = VK_ATTACHMENT_UNUSED;
	if (make_render_pass(node, ID_X_PASS, &pass) != 0U)
		return 1;

	/* The framebuffer of the view. */
	begin(node, GPU_OP_CREATE_FRAMEBUFFER);
	put64(ID_DEVICE);
	put64(1U);
	put32(VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO);
	put64(0U);
	put32(0U);
	put64(ID_X_PASS);
	put32(1U);
	put64(1U);
	put64(ID_X_VIEW);
	put32(64U);
	put32(32U);
	put32(1U);
	tail(ID_X_FRAMEBUFFER);
	error = run(node);
	if (error != 0 || reply32(node, 4U) != 0U)
		return 1;

	/* The graphics pipelines' layout: one empty set, no push constants. */
	if (make_set_layout(node, ID_SET_LAYOUT + 1U, 0U, NULL, NULL, 0U) != 0U)
		return 1;
	if (make_layout(node, ID_LAYOUT + 1U, 1U, &graphics_set, 0U) != 0U)
		return 1;

	/* The fence. */
	begin(node, GPU_OP_CREATE_FENCE);
	put64(ID_DEVICE);
	put64(1U);
	put32(VK_STRUCTURE_TYPE_FENCE_CREATE_INFO);
	put64(0U);
	put32(0U);
	tail(ID_X_FENCE);
	error = run(node);
	if (error != 0 || reply32(node, 4U) != 0U)
		return 1;

	/* Succeeded: the scene's objects exist. */
	return 0;
}

/* Starts a frame that begins a command buffer's recording (the commands follow). */
static void
x_record(
	const struct node *node,
	uint64_t commands)
{
	/* vkBeginCommandBuffer: [buffer][u64 1][sType][u64 0][flags][u64 0]. */
	begin(node, GPU_OP_BEGIN_COMMAND_BUFFER);
	put64(commands);
	put64(1U);
	put32(VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO);
	put64(0U);
	put32(VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT);
	put64(0U);
}

/* Appends a recorded command's head: [number][flags 0][buffer]; the arguments follow. */
static void
x_command(
	uint32_t opcode,
	uint64_t commands)
{
	put32(opcode);
	put32(0U);
	put64(commands);
}

/* Ends the recording and runs the frame.  Returns what the frame returned. */
static int
x_end(
	struct node *node,
	uint64_t commands)
{
	put32(GPU_OP_END_COMMAND_BUFFER);
	put32(0U);
	put64(commands);
	return run(node);
}

/* vkQueueSubmit of one command buffer with a fence (0 for none).  Returns what the frame returned. */
static int
x_submit(
	struct node *node,
	uint64_t commands,
	uint64_t fence)
{
	/* [queue][u32 1][u64 1], VkSubmitInfo with the buffer alone, the fence. */
	begin(node, GPU_OP_QUEUE_SUBMIT);
	put64(ID_X_QUEUE);
	put32(1U);
	put64(1U);
	put32(VK_STRUCTURE_TYPE_SUBMIT_INFO);
	put64(0U);
	put32(0U);
	put64(0U);
	put64(0U);
	put32(1U);
	put64(1U);
	put64(commands);
	put32(0U);
	put64(0U);
	put64(fence);
	return run(node);
}

/*
 * Collects the opcodes of the last submission the model ran after mark:
 * from its BIND_SESSION of a client's session to the BIND_SESSION(0)
 * after its fence.  Returns the count; *first is the record of the first.
 */
static unsigned
x_ops(
	unsigned mark,
	unsigned *first,
	uint32_t *ops)
{
	unsigned index;
	unsigned count;
	unsigned start;

	/* The last BIND_SESSION of a client's session. */
	start = model.command_count;
	for (index = mark; index < model.command_count; index++) {
		if (model.commands[index].opcode == GHCI_OP_BIND_SESSION && model.commands[index].words[1] != 0U)
			start = index;
	}

	/* The records from there to the next BIND_SESSION(0). */
	count = 0U;
	for (index = start; index < model.command_count && count < X_OPS_MAX; index++) {
		ops[count++] = model.commands[index].opcode;
		if (index != start && model.commands[index].opcode == GHCI_OP_BIND_SESSION)
			break;
	}
	*first = start;
	return count;
}

/* Reports whether a run of opcodes is the expected one. */
static int
x_same(
	const uint32_t *ops,
	unsigned count,
	const uint32_t *expected,
	unsigned expected_count)
{
	unsigned index;

	if (count != expected_count) {
		printf("  ops: %u, expected %u:", count, expected_count);
		for (index = 0U; index < count; index++)
			printf(" %02x", ops[index]);
		printf("\n");
		return 0;
	}
	for (index = 0U; index < count; index++) {
		if (ops[index] != expected[index]) {
			printf("  op %u is %02x, expected %02x\n", index, ops[index], expected[index]);
			return 0;
		}
	}
	return 1;
}

/* Reports the bytes the GPU reads at a session's address (NULL when unmapped). */
static const uint8_t *
x_gpu(
	uint32_t session,
	uint64_t va)
{
	const struct model_mapping *mapping;

	mapping = model_lookup(session, va);
	if (mapping == NULL)
		return NULL;
	return model_dma(mapping->phys + (va - mapping->va), 1U);
}

/* Reads a little-endian value of size bytes. */
static uint64_t
x_load(
	const uint8_t *bytes,
	uint32_t size)
{
	uint64_t value;
	uint32_t index;

	value = 0U;
	for (index = 0U; index < size; index++)
		value |= (uint64_t)bytes[index] << (8U * index);
	return value;
}

/* Reports the GPU address of an offset of the scene's memory. */
static uint64_t
x_va(
	const struct node *node,
	uint64_t offset)
{
	uint64_t va;

	va = 0U;
	(void)ghci_vk_bound_va(session_of(node), ID_MEMORY, offset, &va);
	return va;
}

/* Records vkCmdBeginRenderPass of the scene's pass over width x height with a clear colour (1, 0, 0, 1). */
static void
x_begin_pass(
	uint64_t commands,
	uint32_t width,
	uint32_t height)
{
	x_command(GPU_OP_CMD_BEGIN_RENDER_PASS, commands);
	put64(1U);
	put32(VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO);
	put64(0U);
	put64(ID_X_PASS);
	put64(ID_X_FRAMEBUFFER);
	put32(0U);
	put32(0U);
	put32(width);
	put32(height);
	put32(1U);
	put64(1U);
	put32(0U);
	put32(2U);
	put64(4U);
	put32(0x3f800000U);
	put32(0U);
	put32(0U);
	put32(0x3f800000U);
	put32(VK_SUBPASS_CONTENTS_INLINE);
}

/* Records the bind of a graphics pipeline and of the buffer at offset as vertex binding 0. */
static void
x_bind_triangle(
	uint64_t commands,
	uint64_t pipeline,
	uint64_t offset)
{
	x_command(GPU_OP_CMD_BIND_PIPELINE, commands);
	put32(VK_PIPELINE_BIND_POINT_GRAPHICS);
	put64(pipeline);
	x_command(GPU_OP_CMD_BIND_VERTEX_BUFFERS, commands);
	put32(0U);
	put32(1U);
	put64(1U);
	put64(ID_BUFFER);
	put64(1U);
	put64(offset);
}

/* Makes the triangle pipeline of a topology with one dynamic state (~0 for none).  Returns the result. */
static uint32_t
x_triangle(
	struct node *node,
	uint32_t topology,
	uint32_t dynamic,
	uint64_t identity)
{
	struct test_graphics graphics;

	graphics_defaults(&graphics);
	graphics.layout = ID_LAYOUT + 1U;
	graphics.topology = topology;
	if (dynamic != UINT32_MAX) {
		graphics.dynamic_count = 1U;
		graphics.dynamic[0] = dynamic;
	}
	return make_graphics(node, &graphics, identity);
}

/*
 * Makes the compute scenes' layout and set: a dynamic storage buffer in
 * set 0 (the buffer, its whole range), 104 bytes of push constants
 * (ID_LAYOUT, which the graphics pipelines use too), and dyn.comp's
 * pipeline ID_X_COMPUTE.  Returns 0, or 1.
 */
static int
x_compute_set(
	struct node *node)
{
	static const uint32_t types[1] = { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC };
	static const uint32_t stages[1] = { VK_SHADER_STAGE_ALL };
	uint64_t set_layouts[1];
	int error;

	/* The layouts. */
	set_layouts[0] = ID_SET_LAYOUT;
	if (make_set_layout(node, ID_SET_LAYOUT, 1U, types, stages, 0U) != 0U)
		return 1;
	if (make_layout(node, ID_LAYOUT, 1U, set_layouts, 104U) != 0U)
		return 1;

	/* A pool, a set, and the buffer written. */
	begin(node, GPU_OP_CREATE_DESCRIPTOR_POOL);
	put64(ID_DEVICE);
	put64(1U);
	put32(VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO);
	put64(0U);
	put32(0U);
	put32(1U);
	put32(1U);
	put64(1U);
	put32(VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC);
	put32(1U);
	tail(ID_POOL);
	error = run(node);
	if (error != 0 || reply32(node, 4U) != 0U)
		return 1;
	begin(node, GPU_OP_ALLOCATE_DESCRIPTOR_SETS);
	put64(ID_DEVICE);
	put64(1U);
	put32(VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO);
	put64(0U);
	put64(ID_POOL);
	put32(1U);
	put64(1U);
	put64(ID_SET_LAYOUT);
	put64(1U);
	put64(ID_SET);
	error = run(node);
	if (error != 0 || reply32(node, 4U) != 0U)
		return 1;
	begin(node, GPU_OP_UPDATE_DESCRIPTOR_SETS);
	put64(ID_DEVICE);
	put32(1U);
	put64(1U);
	put32(VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET);
	put64(0U);
	put64(ID_SET);
	put32(0U);
	put32(0U);
	put32(1U);
	put32(VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC);
	put64(0U);
	put64(1U);
	put64(ID_BUFFER);
	put64(0U);
	put64(VK_WHOLE_SIZE);
	put64(0U);
	put32(0U);
	put64(0U);
	error = run(node);
	if (error != 0)
		return 1;

	/* The compute pipeline. */
	if (make_compute(node, ID_MODULE + MODULE_DYN_COMP, ID_LAYOUT, NULL, 0U, ID_X_COMPUTE) != 0U)
		return 1;
	return 0;
}

/* Asks vkGetQueryPoolResults of 64-bit results with availability (the reply holds them). */
static void
x_query_results(
	struct node *node,
	uint64_t pool,
	uint32_t first,
	uint32_t count)
{
	begin(node, GPU_OP_GET_QUERY_POOL_RESULTS);
	put64(ID_DEVICE);
	put64(pool);
	put32(first);
	put32(count);
	put64((uint64_t)count * 16U);
	put64((uint64_t)count * 16U);
	put64(16U);
	put32(VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
	(void)run(node);
}

/*
 * One frame: a render pass that clears its attachment, the triangle
 * pipeline with its vertex buffer and a draw, then a dispatch of
 * dyn.comp with its set, dynamic offset and push constants; the packets
 * the model receives against the sequence by hand, and their words.
 */
static void
scene_x_frame(void)
{
	static const uint32_t expected[] = {
		GHCI_OP_BIND_SESSION, GHCI_OP_BARRIER, GHCI_OP_BEGIN_RENDER_PASS, GHCI_OP_BIND_PIPELINE,
		GHCI_OP_SET_VIEWPORT, GHCI_OP_SET_SCISSOR, GHCI_OP_SET_DYNAMIC_STATE, GHCI_OP_SET_DYNAMIC_STATE,
		GHCI_OP_SET_DYNAMIC_STATE, GHCI_OP_SET_DYNAMIC_STATE, GHCI_OP_BIND_VERTEX_BUFFERS, GHCI_OP_DRAW,
		GHCI_OP_END_RENDER_PASS, GHCI_OP_BIND_PIPELINE, GHCI_OP_BIND_DESCRIPTOR_TABLE, GHCI_OP_LOAD_CONST,
		GHCI_OP_LOAD_CONST, GHCI_OP_DISPATCH, GHCI_OP_BARRIER, GHCI_OP_FENCE, GHCI_OP_BIND_SESSION
	};
	const struct ghci_vk_pipeline *pipeline;
	const struct model_command *command;
	const struct model_mapping *mapping;
	const uint8_t *desc;
	const uint8_t *table;
	uint32_t ops[X_OPS_MAX];
	uint64_t desc_va;
	uint32_t session;
	unsigned first;
	unsigned count;
	unsigned mark;
	struct node node;
	int error;

	/* The device, the compute set and pipeline, the triangle. */
	error = x_open(&node, MODEL_FULL);
	error |= x_compute_set(&node);
	error |= (int)x_triangle(&node, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, UINT32_MAX, ID_PIPELINE);
	expect(error == 0, "frame: objects");
	pipeline = pipeline_of(&node, ID_PIPELINE);
	if (error != 0 || pipeline == NULL) {
		node_close(&node);
		stop("frame");
		return;
	}

	/* The recording. */
	x_record(&node, ID_X_COMMANDS);
	x_begin_pass(ID_X_COMMANDS, 64U, 32U);
	x_bind_triangle(ID_X_COMMANDS, ID_PIPELINE, 0U);
	x_command(GPU_OP_CMD_DRAW, ID_X_COMMANDS);
	put32(3U);
	put32(1U);
	put32(0U);
	put32(0U);
	x_command(GPU_OP_CMD_END_RENDER_PASS, ID_X_COMMANDS);
	x_command(GPU_OP_CMD_BIND_PIPELINE, ID_X_COMMANDS);
	put32(VK_PIPELINE_BIND_POINT_COMPUTE);
	put64(ID_X_COMPUTE);
	x_command(GPU_OP_CMD_BIND_DESCRIPTOR_SETS, ID_X_COMMANDS);
	put32(VK_PIPELINE_BIND_POINT_COMPUTE);
	put64(ID_LAYOUT);
	put32(0U);
	put32(1U);
	put64(1U);
	put64(ID_SET);
	put32(1U);
	put64(1U);
	put32(256U);
	x_command(GPU_OP_CMD_PUSH_CONSTANTS, ID_X_COMMANDS);
	put64(ID_LAYOUT);
	put32(VK_SHADER_STAGE_COMPUTE_BIT);
	put32(0U);
	put32(8U);
	put64(8U);
	put32(0x1234U);
	put32(0x5678U);
	x_command(GPU_OP_CMD_DISPATCH, ID_X_COMMANDS);
	put32(2U);
	put32(1U);
	put32(1U);
	error = x_end(&node, ID_X_COMMANDS);
	expect(error == 0 && reply32(&node, 4U) == 0U, "frame: recorded");

	/* The submission: the packets by hand. */
	mark = model.command_count;
	error = x_submit(&node, ID_X_COMMANDS, ID_X_FENCE);
	count = x_ops(mark, &first, ops);
	expect(error == 0 && reply32(&node, 4U) == 0U && x_same(ops, count, expected, sizeof(expected) / sizeof(expected[0])),
	       "frame: the packets of the frame");
	if (count != sizeof(expected) / sizeof(expected[0])) {
		node_close(&node);
		stop("frame");
		return;
	}
	session = model.commands[first].words[1];

	/* The head and tail barriers. */
	expect(model.commands[first + 1U].words[3] == (GHCI_BARRIER_INVAL_ICACHE | GHCI_BARRIER_INVAL_TEX | GHCI_BARRIER_INVAL_L1_DATA |
	       GHCI_BARRIER_INVAL_DESCRIPTOR) && model.commands[first + 18U].words[3] == (GHCI_BARRIER_WAIT_IDLE |
	       GHCI_BARRIER_FLUSH_L1_DATA | GHCI_BARRIER_FLUSH_L2), "frame: head barrier 0x36, tail barrier 0x49");

	/* The render pass: the area, colour 0 cleared, and its description. */
	command = &model.commands[first + 2U];
	desc_va = command->words[1] | ((uint64_t)command->words[2] << 32);
	expect(command->words[3] == 0U && command->words[4] == (64U | (32U << 16)) && command->words[8] == 1U && (desc_va & 63U) == 0U,
	       "frame: BEGIN_RENDER_PASS area 64 x 32, CLEAR_COLOR[0]");
	desc = x_gpu(session, desc_va);
	mapping = model_lookup(session, desc_va);
	expect(desc != NULL && mapping != NULL && (mapping->flags & GHCI_VM_FLAG_EXEC) == 0U,
	       "frame: the description is in the data heap (not executable)");
	if (desc != NULL) {
		expect(x_load(desc + 0x00U, 8U) == x_va(&node, X_ATTACHMENT_OFFSET) && x_load(desc + 0x08U, 4U) == 256U &&
		       desc[0x0cU] == 37U && desc[0x0dU] == 1U && desc[0x0eU] == 0U && desc[0x0fU] == 0U,
		       "frame: colour 0 at the view, pitch 256, R8G8B8A8_UNORM, CLEAR | STORE, mip 0, layer 0");
		expect(x_load(desc + 0x10U, 8U) == 0U && x_load(desc + 0x80U, 8U) == 0U && x_load(desc + 0x90U, 8U) == 0U,
		       "frame: no other attachment, no depth, no resolve");
		expect(x_load(desc + 0xa0U, 2U) == 64U && x_load(desc + 0xa2U, 2U) == 32U && x_load(desc + 0xa4U, 2U) == 1U &&
		       desc[0xa6U] == 1U, "frame: 64 x 32, one layer, one sample");
		table = x_gpu(session, x_load(desc + 0xa8U, 8U));
		expect(table != NULL && x_load(table, 4U) == 0x3f800000U && x_load(table + 4U, 4U) == 0U &&
		       x_load(table + 12U, 4U) == 0x3f800000U, "frame: the clear table holds (1, 0, 0, 1)");
	}

	/* The pipeline and its state. */
	command = &model.commands[first + 3U];
	expect(command->words[1] == (uint32_t)pipeline->pso_va && command->words[2] == (uint32_t)(pipeline->pso_va >> 32),
	       "frame: BIND_PIPELINE of the triangle's record");
	expect(model.commands[first + 6U].words[1] == GHCI_DYNAMIC_DEPTH_BIAS && model.commands[first + 7U].words[1] ==
	       GHCI_DYNAMIC_BLEND_CONSTANTS && model.commands[first + 8U].words[1] == GHCI_DYNAMIC_STENCIL_PARAMS &&
	       model.commands[first + 9U].words[1] == GHCI_DYNAMIC_BLEND_ATTACHMENT_0, "frame: depth bias, blend constants, stencil, blend 0");
	expect(model.commands[first + 8U].words[2] == pipeline->state.stencil[0] && model.commands[first + 9U].words[2] ==
	       pso_word(pipeline, 11U), "frame: the stencil word and the blend word are the pipeline's");

	/* The vertex buffer and the draw. */
	command = &model.commands[first + 10U];
	expect(command->words[1] == (0U | (1U << 16)) && (command->words[2] | ((uint64_t)command->words[3] << 32)) ==
	       x_va(&node, 0x80000U) && command->words[4] == 8192U && command->words[5] == 20U,
	       "frame: binding 0 at the buffer, 8192 bytes, stride 20");
	command = &model.commands[first + 11U];
	expect(command->words[1] == 3U && command->words[2] == 1U && command->words[3] == 0U && command->words[4] == 0U,
	       "frame: DRAW 3 vertices, 1 instance");

	/* The dispatch: set 2's table of one entry, c0-c23, c32-c40 with the dynamic offset in c40. */
	command = &model.commands[first + 14U];
	expect(command->words[3] == (2U | (1U << 16)), "frame: BIND_DESCRIPTOR_TABLE set 2, one entry");
	command = &model.commands[first + 15U];
	expect(command->words[1] == 0U && command->words[2] == 24U && command->words[3] == 0x1234U && command->words[4] == 0x5678U,
	       "frame: LOAD_CONST c0-c23 with the push constants");
	command = &model.commands[first + 16U];
	expect(command->words[1] == 32U && command->words[2] == 9U && command->words[3 + 8U] == 256U,
	       "frame: LOAD_CONST c32-c40, c40 the dynamic offset (c24-c31 not loaded)");
	command = &model.commands[first + 17U];
	expect(command->words[1] == 2U && command->words[2] == 1U && command->words[3] == 1U, "frame: DISPATCH 2 x 1 x 1");

	/* The fence passed. */
	begin(&node, GPU_OP_GET_FENCE_STATUS);
	put64(ID_DEVICE);
	put64(ID_X_FENCE);
	error = run(&node);
	expect(error == 0 && reply32(&node, 4U) == (uint32_t)VK_SUCCESS, "frame: the fence passed");

	/* A dispatch after a draw in one buffer places the tables of each again (they share the slots). */
	x_record(&node, ID_X_COMMANDS + 1U);
	x_command(GPU_OP_CMD_BIND_PIPELINE, ID_X_COMMANDS + 1U);
	put32(VK_PIPELINE_BIND_POINT_COMPUTE);
	put64(ID_X_COMPUTE);
	x_command(GPU_OP_CMD_BIND_DESCRIPTOR_SETS, ID_X_COMMANDS + 1U);
	put32(VK_PIPELINE_BIND_POINT_COMPUTE);
	put64(ID_LAYOUT);
	put32(0U);
	put32(1U);
	put64(1U);
	put64(ID_SET);
	put32(1U);
	put64(1U);
	put32(0U);
	x_command(GPU_OP_CMD_DISPATCH, ID_X_COMMANDS + 1U);
	put32(1U);
	put32(1U);
	put32(1U);
	x_begin_pass(ID_X_COMMANDS + 1U, 64U, 32U);
	x_bind_triangle(ID_X_COMMANDS + 1U, ID_PIPELINE, 0U);
	x_command(GPU_OP_CMD_DRAW, ID_X_COMMANDS + 1U);
	put32(3U);
	put32(1U);
	put32(0U);
	put32(0U);
	x_command(GPU_OP_CMD_END_RENDER_PASS, ID_X_COMMANDS + 1U);
	x_command(GPU_OP_CMD_DISPATCH, ID_X_COMMANDS + 1U);
	put32(1U);
	put32(1U);
	put32(1U);
	error = x_end(&node, ID_X_COMMANDS + 1U);
	mark = model.command_count;
	error |= x_submit(&node, ID_X_COMMANDS + 1U, 0U);
	expect(error == 0 && model_count_commands(GHCI_OP_DISPATCH) == 3U &&
	       model_count_commands(GHCI_OP_BIND_DESCRIPTOR_TABLE) == 3U, "frame: the compute table again after the draw");
	(void)mark;
	node_close(&node);
	stop("frame");
}

/*
 * The binds a draw or dispatch needs (design section 11.3 item 2): a draw
 * without a pipeline, without its dynamic viewport, without its vertex
 * buffer, or outside a render pass, and a dispatch without its set,
 * each refuse the submission before anything is placed; the device goes
 * on.
 */
static void
scene_x_binds(void)
{
	unsigned fences;
	struct node node;
	int error;
	int index;

	/* The device and the pipelines (one with a dynamic viewport). */
	error = x_open(&node, MODEL_FULL);
	error |= x_compute_set(&node);
	error |= (int)x_triangle(&node, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, UINT32_MAX, ID_PIPELINE);
	error |= (int)x_triangle(&node, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, VK_DYNAMIC_STATE_VIEWPORT, ID_PIPELINE + 1U);
	expect(error == 0, "binds: objects");

	/* Each recording that misses a bind. */
	for (index = 0; index < 5; index++) {
		x_record(&node, ID_X_COMMANDS);
		if (index != 4)
			x_begin_pass(ID_X_COMMANDS, 64U, 32U);
		if (index == 1) {
			x_bind_triangle(ID_X_COMMANDS, ID_PIPELINE + 1U, 0U);
		} else if (index == 2) {
			x_command(GPU_OP_CMD_BIND_PIPELINE, ID_X_COMMANDS);
			put32(VK_PIPELINE_BIND_POINT_GRAPHICS);
			put64(ID_PIPELINE);
		} else if (index == 4) {
			x_command(GPU_OP_CMD_BIND_PIPELINE, ID_X_COMMANDS);
			put32(VK_PIPELINE_BIND_POINT_COMPUTE);
			put64(ID_X_COMPUTE);
			x_command(GPU_OP_CMD_DISPATCH, ID_X_COMMANDS);
			put32(1U);
			put32(1U);
			put32(1U);
		} else if (index == 3) {
			x_bind_triangle(ID_X_COMMANDS, ID_PIPELINE, 0U);
			x_command(GPU_OP_CMD_END_RENDER_PASS, ID_X_COMMANDS);
		}
		if (index != 4) {
			x_command(GPU_OP_CMD_DRAW, ID_X_COMMANDS);
			put32(3U);
			put32(1U);
			put32(0U);
			put32(0U);
		}
		if (index != 3 && index != 4)
			x_command(GPU_OP_CMD_END_RENDER_PASS, ID_X_COMMANDS);
		error = x_end(&node, ID_X_COMMANDS);
		fences = model_count_commands(GHCI_OP_FENCE);
		error |= x_submit(&node, ID_X_COMMANDS, 0U) != EINVAL;
		expect(error == 0 && model_count_commands(GHCI_OP_FENCE) == fences && model_count_commands(GHCI_OP_DRAW) == 0U &&
		       model_count_commands(GHCI_OP_DISPATCH) == 0U,
		       index == 0 ? "binds: a draw without a pipeline is refused" : index == 1 ? "binds: a draw without its dynamic viewport is refused" :
		       index == 2 ? "binds: a draw without its vertex buffer is refused" : index == 3 ? "binds: a draw outside a render pass is refused" :
		       "binds: a dispatch without its set is refused");
	}

	/* The viewport recorded: the draw places it. */
	x_record(&node, ID_X_COMMANDS);
	x_begin_pass(ID_X_COMMANDS, 64U, 32U);
	x_bind_triangle(ID_X_COMMANDS, ID_PIPELINE + 1U, 0U);
	x_command(GPU_OP_CMD_SET_VIEWPORT, ID_X_COMMANDS);
	put32(0U);
	put32(1U);
	put64(1U);
	put32(0x40000000U);
	put32(0x40400000U);
	put32(0x42800000U);
	put32(0x42000000U);
	put32(0U);
	put32(0x3f800000U);
	x_command(GPU_OP_CMD_DRAW, ID_X_COMMANDS);
	put32(3U);
	put32(1U);
	put32(0U);
	put32(0U);
	x_command(GPU_OP_CMD_END_RENDER_PASS, ID_X_COMMANDS);
	error = x_end(&node, ID_X_COMMANDS);
	error |= x_submit(&node, ID_X_COMMANDS, 0U);
	expect(error == 0 && model_count_commands(GHCI_OP_DRAW) == 1U && model_last_command(GHCI_OP_SET_VIEWPORT) != NULL &&
	       model_last_command(GHCI_OP_SET_VIEWPORT)->words[1] == 0x40000000U &&
	       model_last_command(GHCI_OP_SET_VIEWPORT)->words[3] == 0x42800000U, "binds: the recorded viewport is placed; the device goes on");
	node_close(&node);
	stop("binds");
}

/*
 * Indirect draws and dispatches: placed with CAP_INDIRECT_DRAW (the
 * buffer, the offset, the count and stride; INDEXED in the header's
 * flags), skipped without it (the rest of the buffer still runs).
 */
static void
scene_x_indirect(void)
{
	const struct model_command *command;
	unsigned draws;
	struct node node;
	int pass;
	int error;

	/* With the flag, then without it. */
	for (pass = 0; pass < 2; pass++) {
		test_caps_off = pass == 0 ? 0U : GHCI_CAP_INDIRECT_DRAW;
		error = x_open(&node, MODEL_FULL);
		test_caps_off = 0U;
		error |= x_compute_set(&node);
		error |= (int)x_triangle(&node, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, UINT32_MAX, ID_PIPELINE);
		expect(error == 0, pass == 0 ? "indirect: objects" : "indirect: objects (no indirect)");

		/* An indirect draw, an indexed one, a plain one, and an indirect dispatch. */
		x_record(&node, ID_X_COMMANDS);
		x_begin_pass(ID_X_COMMANDS, 64U, 32U);
		x_bind_triangle(ID_X_COMMANDS, ID_PIPELINE, 0U);
		x_command(GPU_OP_CMD_BIND_INDEX_BUFFER, ID_X_COMMANDS);
		put64(ID_BUFFER);
		put64(512U);
		put32(VK_INDEX_TYPE_UINT16);
		x_command(GPU_OP_CMD_DRAW_INDIRECT, ID_X_COMMANDS);
		put64(ID_BUFFER);
		put64(64U);
		put32(2U);
		put32(16U);
		x_command(GPU_OP_CMD_DRAW_INDEXED_INDIRECT, ID_X_COMMANDS);
		put64(ID_BUFFER);
		put64(128U);
		put32(1U);
		put32(20U);
		x_command(GPU_OP_CMD_DRAW, ID_X_COMMANDS);
		put32(3U);
		put32(1U);
		put32(0U);
		put32(0U);
		x_command(GPU_OP_CMD_END_RENDER_PASS, ID_X_COMMANDS);
		x_command(GPU_OP_CMD_BIND_PIPELINE, ID_X_COMMANDS);
		put32(VK_PIPELINE_BIND_POINT_COMPUTE);
		put64(ID_X_COMPUTE);
		x_command(GPU_OP_CMD_BIND_DESCRIPTOR_SETS, ID_X_COMMANDS);
		put32(VK_PIPELINE_BIND_POINT_COMPUTE);
		put64(ID_LAYOUT);
		put32(0U);
		put32(1U);
		put64(1U);
		put64(ID_SET);
		put32(1U);
		put64(1U);
		put32(0U);
		x_command(GPU_OP_CMD_DISPATCH_INDIRECT, ID_X_COMMANDS);
		put64(ID_BUFFER);
		put64(256U);
		error = x_end(&node, ID_X_COMMANDS);
		draws = model_count_commands(GHCI_OP_DRAW);
		error |= x_submit(&node, ID_X_COMMANDS, 0U);
		if (pass == 0) {
			command = model_last_command(GHCI_OP_DRAW_INDIRECT);
			expect(error == 0 && model_count_commands(GHCI_OP_DRAW_INDIRECT) == 2U && command != NULL && command->flags == 1U &&
			       (command->words[1] | ((uint64_t)command->words[2] << 32)) == x_va(&node, 0x80000U) && command->words[3] == 128U &&
			       command->words[4] == 1U && command->words[5] == 20U && command->words[6] == 0U,
			       "indirect: DRAW_INDIRECT with INDEXED, the buffer, offset 128, 1 draw, stride 20");
			command = &model.commands[0];
			for (draws = 0U; draws < model.command_count; draws++) {
				if (model.commands[draws].opcode == GHCI_OP_DRAW_INDIRECT) {
					command = &model.commands[draws];
					break;
				}
			}
			expect(command->opcode == GHCI_OP_DRAW_INDIRECT && command->flags == 0U && command->words[3] == 64U &&
			       command->words[4] == 2U && command->words[5] == 16U, "indirect: DRAW_INDIRECT not indexed, offset 64, 2 draws, stride 16");
			command = model_last_command(GHCI_OP_DISPATCH_INDIRECT);
			expect(command != NULL && command->words[3] == 256U, "indirect: DISPATCH_INDIRECT at offset 256");
		} else {
			expect(error == 0 && model_count_commands(GHCI_OP_DRAW_INDIRECT) == 0U && model_count_commands(GHCI_OP_DISPATCH_INDIRECT) == 0U &&
			       model_count_commands(GHCI_OP_DRAW) == draws + 1U && model.faults == 0U,
			       "indirect: without CAP_INDIRECT_DRAW skipped, the plain draw runs");
		}
		node_close(&node);
		stop("indirect");
	}
}

/*
 * Transfers on a device whose blitter neither scales, blends nor tiles,
 * without the bilinear filter: a copy of buffers, a fill, an image copy
 * and a linear blit are the driver's programs and draws, and no packet
 * goes to the blitter.
 */
static void
scene_x_plain_blitter(void)
{
	static const uint32_t blitter_ops[] = {
		GHCI_OP_COPY_BUFFER, GHCI_OP_COPY_IMAGE, GHCI_OP_BLIT, GHCI_OP_FILL_BUFFER, GHCI_OP_RESOLVE_IMAGE,
		GHCI_OP_COPY_BUFFER_TO_IMAGE, GHCI_OP_COPY_IMAGE_TO_BUFFER
	};
	unsigned before[7];
	unsigned draws;
	unsigned dispatches;
	unsigned index;
	struct node node;
	int same;
	int error;

	/* The recording: a copy of 256 bytes, a fill, an image copy and a blit to half the size. */
	error = x_open(&node, MODEL_BLT_MIN);
	x_record(&node, ID_X_COMMANDS);
	x_command(GPU_OP_CMD_COPY_BUFFER, ID_X_COMMANDS);
	put64(ID_BUFFER);
	put64(ID_BUFFER);
	put32(1U);
	put64(1U);
	put64(0U);
	put64(4096U);
	put64(256U);
	x_command(GPU_OP_CMD_FILL_BUFFER, ID_X_COMMANDS);
	put64(ID_BUFFER);
	put64(64U);
	put64(256U);
	put32(0xdeadbeefU);
	x_command(GPU_OP_CMD_COPY_IMAGE, ID_X_COMMANDS);
	put64(ID_IMAGE);
	put32(VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
	put64(ID_X_IMAGE);
	put32(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
	put32(1U);
	put64(1U);
	put32(VK_IMAGE_ASPECT_COLOR_BIT);
	put32(0U);
	put32(0U);
	put32(1U);
	put32(0U);
	put32(0U);
	put32(0U);
	put32(VK_IMAGE_ASPECT_COLOR_BIT);
	put32(0U);
	put32(0U);
	put32(1U);
	put32(4U);
	put32(4U);
	put32(0U);
	put32(16U);
	put32(16U);
	put32(1U);
	x_command(GPU_OP_CMD_BLIT_IMAGE, ID_X_COMMANDS);
	put64(ID_IMAGE);
	put32(VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
	put64(ID_X_IMAGE);
	put32(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
	put32(1U);
	put64(1U);
	put32(VK_IMAGE_ASPECT_COLOR_BIT);
	put32(0U);
	put32(0U);
	put32(1U);
	put64(2U);
	put32(0U);
	put32(0U);
	put32(0U);
	put32(64U);
	put32(32U);
	put32(1U);
	put32(VK_IMAGE_ASPECT_COLOR_BIT);
	put32(0U);
	put32(0U);
	put32(1U);
	put64(2U);
	put32(0U);
	put32(0U);
	put32(0U);
	put32(32U);
	put32(16U);
	put32(1U);
	put32(VK_FILTER_LINEAR);
	error |= x_end(&node, ID_X_COMMANDS);
	expect(error == 0 && reply32(&node, 4U) == 0U, "plain blitter: recorded");

	/* The submission: no blitter packet; the copies and the fill dispatches, the blit a draw. */
	for (index = 0U; index < 7U; index++)
		before[index] = model_count_commands(blitter_ops[index]);
	draws = model_count_commands(GHCI_OP_DRAW);
	dispatches = model_count_commands(GHCI_OP_DISPATCH);
	error = x_submit(&node, ID_X_COMMANDS, 0U);
	same = 1;
	for (index = 0U; index < 7U; index++) {
		if (model_count_commands(blitter_ops[index]) != before[index])
			same = 0;
	}
	expect(error == 0 && same && model.faults == 0U, "plain blitter: nothing for the blitter");
	expect(model_count_commands(GHCI_OP_DISPATCH) >= dispatches + 3U && model_count_commands(GHCI_OP_DRAW) >= draws + 1U,
	       "plain blitter: the copies and the fill dispatched, the blit drawn");
	node_close(&node);
	stop("plain blitter");
}

/*
 * A pipeline barrier's stages and caches (colour writes before fragment
 * shader reads, a transfer's writes before the host), and a barrier
 * between two transfers on the full profile and without the blitter: the
 * transfer stage is the blitter and the units of the driver's programs
 * and draws (CS, FS, ROP; without the blitter those alone), and a
 * transfer's reads invalidate L1 and the texture cache.
 */
static void
scene_x_barriers(void)
{
	static const enum model_profile profiles[2] = { MODEL_FULL, MODEL_NO_BLT };
	const struct model_command *command;
	uint32_t ops[X_OPS_MAX];
	uint32_t units;
	unsigned barriers;
	unsigned first;
	unsigned count;
	unsigned mark;
	unsigned index;
	unsigned at;
	struct node node;
	int error;

	/* The barrier: a memory barrier of colour attachment writes before shader reads, and a host read. */
	error = x_open(&node, MODEL_FULL);
	x_record(&node, ID_X_COMMANDS);
	x_command(GPU_OP_CMD_PIPELINE_BARRIER, ID_X_COMMANDS);
	put32(VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
	put32(VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
	put32(0U);
	put32(1U);
	put64(1U);
	put32(VK_STRUCTURE_TYPE_MEMORY_BARRIER);
	put64(0U);
	put32(VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
	put32(VK_ACCESS_SHADER_READ_BIT);
	put32(0U);
	put64(0U);
	put32(0U);
	put64(0U);
	x_command(GPU_OP_CMD_PIPELINE_BARRIER, ID_X_COMMANDS);
	put32(VK_PIPELINE_STAGE_TRANSFER_BIT);
	put32(VK_PIPELINE_STAGE_HOST_BIT);
	put32(0U);
	put32(0U);
	put64(0U);
	put32(1U);
	put64(1U);
	put32(VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER);
	put64(0U);
	put32(VK_ACCESS_TRANSFER_WRITE_BIT);
	put32(VK_ACCESS_HOST_READ_BIT);
	put32(VK_QUEUE_FAMILY_IGNORED);
	put32(VK_QUEUE_FAMILY_IGNORED);
	put64(ID_BUFFER);
	put64(0U);
	put64(VK_WHOLE_SIZE);
	put32(0U);
	put64(0U);
	error |= x_end(&node, ID_X_COMMANDS);
	barriers = model_count_commands(GHCI_OP_BARRIER);
	error |= x_submit(&node, ID_X_COMMANDS, 0U);
	expect(error == 0 && model_count_commands(GHCI_OP_BARRIER) == barriers + 4U, "barriers: head, two, tail");
	command = &model.commands[model.command_count - 5U];
	expect(command->opcode == GHCI_OP_BARRIER && command->words[1] == GHCI_STAGE_ROP && command->words[2] == GHCI_STAGE_FS &&
	       command->words[3] == (GHCI_BARRIER_FLUSH_L1_DATA | GHCI_BARRIER_INVAL_L1_DATA | GHCI_BARRIER_INVAL_TEX),
	       "barriers: ROP before FS, flush L1, invalidate L1 and the texture cache");
	command = &model.commands[model.command_count - 4U];
	expect(command->opcode == GHCI_OP_BARRIER && command->words[1] == (GHCI_STAGE_BLT | GHCI_STAGE_CS | GHCI_STAGE_FS | GHCI_STAGE_ROP) &&
	       command->words[2] == 0U && command->words[3] == (GHCI_BARRIER_WAIT_IDLE | GHCI_BARRIER_FLUSH_L1_DATA | GHCI_BARRIER_FLUSH_L2),
	       "barriers: the transfer's units (BLT, CS, FS, ROP) before the host, idle and flush L1 and L2");
	node_close(&node);
	stop("barriers");

	/* A copy of 256 bytes of whole words, a barrier of its writes before a transfer's reads, a fill: on each profile. */
	for (index = 0U; index < 2U; index++) {
		error = x_open(&node, profiles[index]);
		x_record(&node, ID_X_COMMANDS);
		x_command(GPU_OP_CMD_COPY_BUFFER, ID_X_COMMANDS);
		put64(ID_BUFFER);
		put64(ID_BUFFER);
		put32(1U);
		put64(1U);
		put64(0U);
		put64(4096U);
		put64(256U);
		x_command(GPU_OP_CMD_PIPELINE_BARRIER, ID_X_COMMANDS);
		put32(VK_PIPELINE_STAGE_TRANSFER_BIT);
		put32(VK_PIPELINE_STAGE_TRANSFER_BIT);
		put32(0U);
		put32(1U);
		put64(1U);
		put32(VK_STRUCTURE_TYPE_MEMORY_BARRIER);
		put64(0U);
		put32(VK_ACCESS_TRANSFER_WRITE_BIT);
		put32(VK_ACCESS_TRANSFER_READ_BIT);
		put32(0U);
		put64(0U);
		put32(0U);
		put64(0U);
		x_command(GPU_OP_CMD_FILL_BUFFER, ID_X_COMMANDS);
		put64(ID_BUFFER);
		put64(6144U);
		put64(256U);
		put32(0U);
		error |= x_end(&node, ID_X_COMMANDS);
		mark = model.command_count;
		error |= x_submit(&node, ID_X_COMMANDS, 0U);
		count = x_ops(mark, &first, ops);

		/* The barrier after the copy's packet (COPY_BUFFER, or the program's DISPATCH). */
		for (at = 2U; at < count; at++) {
			if (ops[at] == GHCI_OP_BARRIER)
				break;
		}
		units = GHCI_STAGE_CS | GHCI_STAGE_FS | GHCI_STAGE_ROP;
		if (index == 0U)
			units |= GHCI_STAGE_BLT;
		command = &model.commands[first + at];
		expect(error == 0 && at < count && (ops[at - 1U] == GHCI_OP_COPY_BUFFER || ops[at - 1U] == GHCI_OP_DISPATCH) &&
		       command->words[1] == units && command->words[2] == units &&
		       command->words[3] == (GHCI_BARRIER_FLUSH_L1_DATA | GHCI_BARRIER_INVAL_L1_DATA | GHCI_BARRIER_INVAL_TEX),
		       index == 0U ? "barriers full: copy, barrier BLT|CS|FS|ROP to BLT|CS|FS|ROP, flush L1, invalidate L1 and the texture cache"
		                   : "barriers no-blt: copy, barrier CS|FS|ROP to CS|FS|ROP, flush L1, invalidate L1 and the texture cache");
		node_close(&node);
		stop("barriers");
	}
}

/*
 * Transfers on a device with the blitter: copies of buffers (a region of
 * whole words, and one whose ends are not: its middle, and the ends by
 * the driver's program), a fill, an update through temporary memory, a
 * buffer to an image level and layer and back, an image copy and a blit
 * (the transfers without the blitter are ghci-meta-host-test.c's).
 */
static void
scene_x_transfers(void)
{
	const struct model_command *command;
	const uint8_t *bytes;
	uint64_t buffer_va;
	uint64_t image_va;
	uint64_t attachment_va;
	uint32_t ops[X_OPS_MAX];
	unsigned first;
	unsigned count;
	unsigned mark;
	struct node node;
	int error;

	/* The recording of each. */
	error = x_open(&node, MODEL_FULL);
	buffer_va = x_va(&node, 0x80000U);
	image_va = x_va(&node, 0U);
	attachment_va = x_va(&node, X_ATTACHMENT_OFFSET);
	x_record(&node, ID_X_COMMANDS);
	x_command(GPU_OP_CMD_COPY_BUFFER, ID_X_COMMANDS);
	put64(ID_BUFFER);
	put64(ID_BUFFER);
	put32(2U);
	put64(2U);
	put64(0U);
	put64(4096U);
	put64(256U);
	put64(1U);
	put64(4097U);
	put64(10U);
	x_command(GPU_OP_CMD_FILL_BUFFER, ID_X_COMMANDS);
	put64(ID_BUFFER);
	put64(64U);
	put64(VK_WHOLE_SIZE);
	put32(0xdeadbeefU);
	x_command(GPU_OP_CMD_UPDATE_BUFFER, ID_X_COMMANDS);
	put64(ID_BUFFER);
	put64(128U);
	put64(8U);
	put64(8U);
	put32(0x11223344U);
	put32(0x55667788U);
	x_command(GPU_OP_CMD_COPY_BUFFER_TO_IMAGE, ID_X_COMMANDS);
	put64(ID_BUFFER);
	put64(ID_IMAGE);
	put32(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
	put32(1U);
	put64(1U);
	put64(0U);
	put32(0U);
	put32(0U);
	put32(VK_IMAGE_ASPECT_COLOR_BIT);
	put32(1U);
	put32(1U);
	put32(1U);
	put32(0U);
	put32(0U);
	put32(0U);
	put32(32U);
	put32(16U);
	put32(1U);
	x_command(GPU_OP_CMD_COPY_IMAGE_TO_BUFFER, ID_X_COMMANDS);
	put64(ID_IMAGE);
	put32(VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
	put64(ID_BUFFER);
	put32(1U);
	put64(1U);
	put64(1024U);
	put32(0U);
	put32(0U);
	put32(VK_IMAGE_ASPECT_COLOR_BIT);
	put32(0U);
	put32(0U);
	put32(1U);
	put32(8U);
	put32(4U);
	put32(0U);
	put32(16U);
	put32(8U);
	put32(1U);
	x_command(GPU_OP_CMD_COPY_IMAGE, ID_X_COMMANDS);
	put64(ID_IMAGE);
	put32(VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
	put64(ID_X_IMAGE);
	put32(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
	put32(1U);
	put64(1U);
	put32(VK_IMAGE_ASPECT_COLOR_BIT);
	put32(0U);
	put32(0U);
	put32(1U);
	put32(0U);
	put32(0U);
	put32(0U);
	put32(VK_IMAGE_ASPECT_COLOR_BIT);
	put32(0U);
	put32(0U);
	put32(1U);
	put32(4U);
	put32(4U);
	put32(0U);
	put32(16U);
	put32(16U);
	put32(1U);
	x_command(GPU_OP_CMD_BLIT_IMAGE, ID_X_COMMANDS);
	put64(ID_IMAGE);
	put32(VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
	put64(ID_X_IMAGE);
	put32(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
	put32(1U);
	put64(1U);
	put32(VK_IMAGE_ASPECT_COLOR_BIT);
	put32(0U);
	put32(0U);
	put32(1U);
	put64(2U);
	put32(0U);
	put32(0U);
	put32(0U);
	put32(64U);
	put32(32U);
	put32(1U);
	put32(VK_IMAGE_ASPECT_COLOR_BIT);
	put32(0U);
	put32(0U);
	put32(1U);
	put64(2U);
	put32(0U);
	put32(0U);
	put32(0U);
	put32(32U);
	put32(16U);
	put32(1U);
	put32(VK_FILTER_LINEAR);
	error |= x_end(&node, ID_X_COMMANDS);
	expect(error == 0 && reply32(&node, 4U) == 0U, "transfers: recorded");

	/*
	 * The packets: head, two copies (the second's 3 bytes before and after
	 * its word the driver's program's), a fill, an update, two buffer-image
	 * copies, an image copy, a blit, tail.
	 */
	mark = model.command_count;
	error = x_submit(&node, ID_X_COMMANDS, 0U);
	count = x_ops(mark, &first, ops);
	expect(error == 0 && count == 21U && ops[2] == GHCI_OP_COPY_BUFFER && ops[3] == GHCI_OP_BIND_PIPELINE &&
	       ops[4] == GHCI_OP_BIND_DESCRIPTOR_TABLE && ops[5] == GHCI_OP_LOAD_CONST && ops[6] == GHCI_OP_DISPATCH &&
	       ops[7] == GHCI_OP_COPY_BUFFER && ops[8] == GHCI_OP_BIND_PIPELINE && ops[11] == GHCI_OP_DISPATCH &&
	       ops[12] == GHCI_OP_FILL_BUFFER && ops[13] == GHCI_OP_COPY_BUFFER && ops[14] == GHCI_OP_COPY_BUFFER_TO_IMAGE &&
	       ops[15] == GHCI_OP_COPY_IMAGE_TO_BUFFER && ops[16] == GHCI_OP_COPY_IMAGE && ops[17] == GHCI_OP_BLIT,
	       "transfers: the packets of each");
	if (count != 21U) {
		node_close(&node);
		stop("transfers");
		return;
	}
	command = &model.commands[first + 2U];
	expect((command->words[1] | ((uint64_t)command->words[2] << 32)) == buffer_va &&
	       (command->words[3] | ((uint64_t)command->words[4] << 32)) == buffer_va + 4096U && command->words[5] == 256U,
	       "transfers: COPY_BUFFER of 256 bytes to offset 4096");
	command = &model.commands[first + 7U];
	expect((command->words[1] | ((uint64_t)command->words[2] << 32)) == buffer_va + 4U &&
	       (command->words[3] | ((uint64_t)command->words[4] << 32)) == buffer_va + 4100U && command->words[5] == 4U,
	       "transfers: 10 bytes from 1 to 4097 copy their whole word 4 to 4100 (R11; the ends are the driver's program's)");
	command = &model.commands[first + 12U];
	expect((command->words[1] | ((uint64_t)command->words[2] << 32)) == buffer_va + 64U && command->words[3] == 8128U &&
	       command->words[4] == 0xdeadbeefU, "transfers: FILL_BUFFER from 64 to the end with the pattern");
	command = &model.commands[first + 13U];
	bytes = x_gpu(command->session, command->words[1] | ((uint64_t)command->words[2] << 32));
	expect((command->words[3] | ((uint64_t)command->words[4] << 32)) == buffer_va + 128U && command->words[5] == 8U &&
	       bytes != NULL && x_load(bytes, 4U) == 0x11223344U && x_load(bytes + 4U, 4U) == 0x55667788U,
	       "transfers: the update's 8 bytes copied from temporary memory");
	command = &model.commands[first + 14U];
	expect((command->words[1] | ((uint64_t)command->words[2] << 32)) == buffer_va &&
	       (command->words[3] | ((uint64_t)command->words[4] << 32)) == image_va + 10752U + 8192U && command->words[5] == 128U &&
	       command->words[6] == 128U && command->words[7] == 0U && command->words[8] == (32U | (16U << 16)) && command->words[9] == 37U,
	       "transfers: buffer to level 1 of layer 1, pitches 128, 32 x 16");
	command = &model.commands[first + 15U];
	expect((command->words[1] | ((uint64_t)command->words[2] << 32)) == image_va &&
	       (command->words[3] | ((uint64_t)command->words[4] << 32)) == buffer_va + 1024U && command->words[5] == 256U &&
	       command->words[6] == 64U && command->words[7] == (8U | (4U << 16)) && command->words[8] == (16U | (8U << 16)),
	       "transfers: level 0 at (8, 4) to the buffer at 1024, image pitch 256, buffer pitch 64");
	command = &model.commands[first + 16U];
	expect((command->words[1] | ((uint64_t)command->words[2] << 32)) == image_va &&
	       (command->words[3] | ((uint64_t)command->words[4] << 32)) == attachment_va && command->words[5] == 256U &&
	       command->words[6] == 256U && command->words[8] == (4U | (4U << 16)) && command->words[9] == (16U | (16U << 16)) &&
	       command->words[10] == 37U, "transfers: COPY_IMAGE 16 x 16 to (4, 4)");
	command = &model.commands[first + 17U];
	expect(command->words[7] == 0U && command->words[8] == (64U | (32U << 16)) && command->words[9] == 0U &&
	       command->words[10] == (32U | (16U << 16)) && command->words[11] == (37U | (37U << 8) | (1U << 16)),
	       "transfers: BLIT 64 x 32 to 32 x 16, linear");
	node_close(&node);
	stop("transfers");
}

/*
 * Queries, timestamps and events: a reset of a timestamp pool (a fill),
 * a timestamp and the fence that makes it available (R12), an occlusion
 * query, the results on the host, an event set and waited for in one
 * buffer, reset, a wait for an event nothing sets (refused), one the host
 * set, and a copy of results (skipped).
 */
static void
scene_x_queries(void)
{
	const struct ghci_vk_query_pool *pool;
	const struct ghci_vk_object *object;
	const struct model_command *command;
	uint32_t ops[X_OPS_MAX];
	uint64_t event_bus;
	uint64_t private_bus;
	unsigned first;
	unsigned count;
	unsigned mark;
	unsigned fences;
	struct node node;
	int error;
	int index;

	/* The device, two pools and an event. */
	error = x_open(&node, MODEL_FULL);
	error |= (int)x_triangle(&node, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, UINT32_MAX, ID_PIPELINE);
	for (index = 0; index < 2; index++) {
		begin(&node, GPU_OP_CREATE_QUERY_POOL);
		put64(ID_DEVICE);
		put64(1U);
		put32(VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO);
		put64(0U);
		put32(0U);
		put32(index == 0 ? VK_QUERY_TYPE_TIMESTAMP : VK_QUERY_TYPE_OCCLUSION);
		put32(index == 0 ? 4U : 2U);
		put32(0U);
		tail(index == 0 ? ID_X_QUERIES : ID_X_OCCLUSION);
		error |= run(&node);
		error |= (int)reply32(&node, 4U);
	}
	begin(&node, GPU_OP_CREATE_QUERY_POOL);
	put64(ID_DEVICE);
	put64(1U);
	put32(VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO);
	put64(0U);
	put32(0U);
	put32(VK_QUERY_TYPE_PIPELINE_STATISTICS);
	put32(1U);
	put32(1U);
	tail(ID_X_QUERIES + 0x10U);
	error |= run(&node);
	expect(reply32(&node, 4U) == (uint32_t)VK_ERROR_FEATURE_NOT_PRESENT, "queries: a statistics pool is not a feature");
	begin(&node, GPU_OP_CREATE_EVENT);
	put64(ID_DEVICE);
	put64(1U);
	put32(VK_STRUCTURE_TYPE_EVENT_CREATE_INFO);
	put64(0U);
	put32(0U);
	tail(ID_X_EVENT);
	error |= run(&node);
	object = ghci_vk_object_find(session_of(&node), GHCI_VK_QUERY_POOL, ID_X_QUERIES);
	expect(error == 0 && object != NULL, "queries: objects");
	if (object == NULL) {
		node_close(&node);
		stop("queries");
		return;
	}
	pool = &object->as.query_pool;
	object = ghci_vk_object_find(session_of(&node), GHCI_VK_EVENT, ID_X_EVENT);
	event_bus = object->as.event.block->allocation->bus + object->as.event.offset;

	/* Reset, a timestamp of query 1, an occlusion query in a pass, the event set and waited for, a copy of results. */
	x_record(&node, ID_X_COMMANDS);
	x_command(GPU_OP_CMD_RESET_QUERY_POOL, ID_X_COMMANDS);
	put64(ID_X_QUERIES);
	put32(0U);
	put32(4U);
	x_command(GPU_OP_CMD_WRITE_TIMESTAMP, ID_X_COMMANDS);
	put32(VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
	put64(ID_X_QUERIES);
	put32(1U);
	x_begin_pass(ID_X_COMMANDS, 64U, 32U);
	x_command(GPU_OP_CMD_BEGIN_QUERY, ID_X_COMMANDS);
	put64(ID_X_OCCLUSION);
	put32(1U);
	put32(VK_QUERY_CONTROL_PRECISE_BIT);
	x_command(GPU_OP_CMD_END_QUERY, ID_X_COMMANDS);
	put64(ID_X_OCCLUSION);
	put32(1U);
	x_command(GPU_OP_CMD_END_RENDER_PASS, ID_X_COMMANDS);
	x_command(GPU_OP_CMD_SET_EVENT, ID_X_COMMANDS);
	put64(ID_X_EVENT);
	put32(VK_PIPELINE_STAGE_TRANSFER_BIT);
	x_command(GPU_OP_CMD_WAIT_EVENTS, ID_X_COMMANDS);
	put32(1U);
	put64(1U);
	put64(ID_X_EVENT);
	put32(VK_PIPELINE_STAGE_TRANSFER_BIT);
	put32(VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
	put32(0U);
	put64(0U);
	put32(0U);
	put64(0U);
	put32(0U);
	put64(0U);
	x_command(GPU_OP_CMD_COPY_QUERY_POOL_RESULTS, ID_X_COMMANDS);
	put64(ID_X_QUERIES);
	put32(0U);
	put32(4U);
	put64(ID_BUFFER);
	put64(0U);
	put64(16U);
	put32(VK_QUERY_RESULT_64_BIT);
	error = x_end(&node, ID_X_COMMANDS);
	mark = model.command_count;
	error |= x_submit(&node, ID_X_COMMANDS, 0U);
	count = x_ops(mark, &first, ops);
	expect(error == 0 && count == 23U && ops[2] == GHCI_OP_FILL_BUFFER && ops[3] == GHCI_OP_BARRIER && ops[4] == GHCI_OP_WRITE_TIMESTAMP &&
	       ops[5] == GHCI_OP_FENCE && ops[6] == GHCI_OP_BEGIN_RENDER_PASS && ops[7] == GHCI_OP_BEGIN_QUERY && ops[8] == GHCI_OP_END_QUERY &&
	       ops[9] == GHCI_OP_END_RENDER_PASS && ops[10] == GHCI_OP_FENCE && ops[11] == GHCI_OP_FENCE && ops[12] == GHCI_OP_WAIT_FENCE &&
	       ops[13] == GHCI_OP_BARRIER && ops[14] == GHCI_OP_BARRIER && ops[15] == GHCI_OP_BIND_PIPELINE &&
	       ops[16] == GHCI_OP_BIND_DESCRIPTOR_TABLE && ops[17] == GHCI_OP_LOAD_CONST && ops[18] == GHCI_OP_DISPATCH &&
	       ops[19] == GHCI_OP_BARRIER && ops[20] == GHCI_OP_BARRIER,
	       "queries: fill and its barrier, timestamp and its fence, the query in the pass, the event's two fences, its wait and barrier, "
	       "the copy's program between two barriers");
	if (count == 23U) {
		command = &model.commands[first + 2U];
		expect((command->words[1] | ((uint64_t)command->words[2] << 32)) == pool->va && command->words[3] == 64U && command->words[4] == 0U,
		       "queries: the reset fills the pool's 64 bytes with 0");
		command = &model.commands[first + 3U];
		expect(command->words[1] == 0U && command->words[2] == 0U &&
		       command->words[3] == (GHCI_BARRIER_WAIT_IDLE | GHCI_BARRIER_FLUSH_L1_DATA | GHCI_BARRIER_INVAL_L1_DATA),
		       "queries: after the reset's fill, a barrier: idle, flush and invalidate L1");
		command = &model.commands[first + 4U];
		expect(command->flags == 0U && (command->words[1] | ((uint64_t)command->words[2] << 32)) == pool->va + 16U && command->words[3] == 2U,
		       "queries: WRITE_TIMESTAMP to the session's address of query 1, bottom of the pipe");
		command = &model.commands[first + 5U];
		expect((command->words[1] | ((uint64_t)command->words[2] << 32)) == pool->bus + 24U && command->words[3] == 1U &&
		       command->words[4] == 0U && command->words[5] == GHCI_FENCE_FLAG_MEM_WRITE,
		       "queries: a fence of 1 on query 1's availability word, no interrupt");
		command = &model.commands[first + 7U];
		expect(command->words[3] == (1U | GHCI_QUERY_PRECISE | GHCI_QUERY_AVAILABILITY), "queries: BEGIN_QUERY 1, precise, with availability");
		command = &model.commands[first + 10U];
		expect((command->words[1] | ((uint64_t)command->words[2] << 32)) == event_bus && command->words[3] == 1U &&
		       command->words[5] == GHCI_FENCE_FLAG_MEM_WRITE, "queries: SetEvent is a fence of 1 on the event's word");
		command = &model.commands[first + 11U];
		private_bus = command->words[1] | ((uint64_t)command->words[2] << 32);
		expect(private_bus != event_bus && private_bus != 0U && command->words[3] == 1U && command->words[5] == GHCI_FENCE_FLAG_MEM_WRITE,
		       "queries: SetEvent's second fence sets a word of the submission's own");
		command = &model.commands[first + 12U];
		expect((command->words[1] | ((uint64_t)command->words[2] << 32)) == private_bus && command->words[3] == 1U && command->words[4] == 0U,
		       "queries: WaitEvents after the set waits with WAIT_FENCE for 1 on the submission's word, not the client's");
		command = &model.commands[first + 13U];
		expect(command->words[1] == (GHCI_STAGE_BLT | GHCI_STAGE_CS | GHCI_STAGE_FS | GHCI_STAGE_ROP) && command->words[2] == GHCI_STAGE_FS,
		       "queries: the wait's barrier: the transfer's units (BLT, CS, FS, ROP) before FS");
		command = &model.commands[first + 14U];
		expect(command->words[1] == 0U && command->words[2] == 0U &&
		       command->words[3] == (GHCI_BARRIER_WAIT_IDLE | GHCI_BARRIER_FLUSH_L1_DATA | GHCI_BARRIER_INVAL_L1_DATA),
		       "queries: before the copy's program, a barrier: idle, flush and invalidate L1");
		command = &model.commands[first + 19U];
		expect(command->words[1] == 0U && command->words[2] == 0U &&
		       command->words[3] == (GHCI_BARRIER_WAIT_IDLE | GHCI_BARRIER_FLUSH_L1_DATA | GHCI_BARRIER_INVAL_L1_DATA),
		       "queries: after the copy's program, the same barrier");
	}

	/* The host: query 1 available, the others not; the event set. */
	x_query_results(&node, ID_X_QUERIES, 0U, 4U);
	expect(reply32(&node, 4U) == (uint32_t)VK_NOT_READY && reply64(&node, 8U) == 64U && reply64(&node, 16U + 8U) == 0U &&
	       reply64(&node, 16U + 24U) == 1U && reply64(&node, 16U + 40U) == 0U, "queries: NOT_READY, query 1 alone available");
	x_query_results(&node, ID_X_QUERIES, 1U, 1U);
	expect(reply32(&node, 4U) == (uint32_t)VK_SUCCESS && reply64(&node, 16U + 8U) == 1U, "queries: query 1 is ready");
	begin(&node, GPU_OP_GET_EVENT_STATUS);
	put64(ID_DEVICE);
	put64(ID_X_EVENT);
	error = run(&node);
	expect(error == 0 && reply32(&node, 4U) == (uint32_t)VK_EVENT_SET, "queries: the GPU set the event");

	/* ResetEvent on the GPU, then a wait for the event that nothing sets: held until the host sets it. */
	x_record(&node, ID_X_COMMANDS);
	x_command(GPU_OP_CMD_RESET_EVENT, ID_X_COMMANDS);
	put64(ID_X_EVENT);
	put32(VK_PIPELINE_STAGE_TRANSFER_BIT);
	error = x_end(&node, ID_X_COMMANDS);
	error |= x_submit(&node, ID_X_COMMANDS, 0U);
	begin(&node, GPU_OP_GET_EVENT_STATUS);
	put64(ID_DEVICE);
	put64(ID_X_EVENT);
	error |= run(&node);
	expect(error == 0 && reply32(&node, 4U) == (uint32_t)VK_EVENT_RESET, "queries: the GPU reset the event");
	for (index = 0; index < 2; index++) {
		if (index == 1) {
			fences = model_count_commands(GHCI_OP_FENCE);
			begin(&node, GPU_OP_SET_EVENT);
			put64(ID_DEVICE);
			put64(ID_X_EVENT);
			error = run(&node);
			expect(error == 0 && model_count_commands(GHCI_OP_FENCE) == fences + 1U,
			       "queries: the host's set places the held submission");
		}
		x_record(&node, ID_X_COMMANDS);
		x_command(GPU_OP_CMD_WAIT_EVENTS, ID_X_COMMANDS);
		put32(1U);
		put64(1U);
		put64(ID_X_EVENT);
		put32(VK_PIPELINE_STAGE_HOST_BIT);
		put32(VK_PIPELINE_STAGE_TRANSFER_BIT);
		put32(0U);
		put64(0U);
		put32(0U);
		put64(0U);
		put32(0U);
		put64(0U);
		error = x_end(&node, ID_X_COMMANDS);
		fences = model_count_commands(GHCI_OP_FENCE);
		error |= x_submit(&node, ID_X_COMMANDS, 0U);
		if (index == 0)
			expect(error == 0 && model_count_commands(GHCI_OP_FENCE) == fences + 1U,
			       "queries: a wait for an event nothing sets is held after the part before it");
		else
			expect(error == 0 && model_count_commands(GHCI_OP_FENCE) == fences + 1U, "queries: a wait for an event the host set runs");
	}
	node_close(&node);
	stop("queries");
}

/*
 * Clears: an image's colour by a render pass that only clears, and
 * vkCmdClearAttachments of the whole area (the subpass again, clearing)
 * and of part of it (skipped).
 */
static void
scene_x_clears(void)
{
	const struct model_command *command;
	const uint8_t *desc;
	const uint8_t *table;
	uint32_t ops[X_OPS_MAX];
	unsigned first;
	unsigned count;
	unsigned mark;
	struct node node;
	int error;

	/* ClearColorImage of the attachment image. */
	error = x_open(&node, MODEL_FULL);
	x_record(&node, ID_X_COMMANDS);
	x_command(GPU_OP_CMD_CLEAR_COLOR_IMAGE, ID_X_COMMANDS);
	put64(ID_X_IMAGE);
	put32(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
	put64(1U);
	put32(2U);
	put64(4U);
	put32(0x3e800000U);
	put32(0x3f000000U);
	put32(0x3f400000U);
	put32(0x3f800000U);
	put32(1U);
	put64(1U);
	put32(VK_IMAGE_ASPECT_COLOR_BIT);
	put32(0U);
	put32(1U);
	put32(0U);
	put32(1U);
	error |= x_end(&node, ID_X_COMMANDS);
	mark = model.command_count;
	error |= x_submit(&node, ID_X_COMMANDS, 0U);
	count = x_ops(mark, &first, ops);
	expect(error == 0 && count == 7U && ops[2] == GHCI_OP_BEGIN_RENDER_PASS && ops[3] == GHCI_OP_END_RENDER_PASS,
	       "clears: a clear of an image is a render pass of its own");
	if (count == 7U) {
		command = &model.commands[first + 2U];
		desc = x_gpu(command->session, command->words[1] | ((uint64_t)command->words[2] << 32));
		table = desc != NULL ? x_gpu(command->session, x_load(desc + 0xa8U, 8U)) : NULL;
		expect(command->words[4] == (64U | (32U << 16)) && command->words[8] == 1U && desc != NULL && table != NULL &&
		       x_load(desc, 8U) == x_va(&node, X_ATTACHMENT_OFFSET) && desc[0x0dU] == 1U && x_load(table, 4U) == 0x3e800000U &&
		       x_load(table + 12U, 4U) == 0x3f800000U, "clears: the whole image as colour 0, CLEAR, the colour in the table");
	}

	/* ClearAttachments in a pass: the whole area, then a part. */
	x_record(&node, ID_X_COMMANDS);
	x_begin_pass(ID_X_COMMANDS, 64U, 32U);
	x_command(GPU_OP_CMD_CLEAR_ATTACHMENTS, ID_X_COMMANDS);
	put32(1U);
	put64(1U);
	put32(VK_IMAGE_ASPECT_COLOR_BIT);
	put32(0U);
	put32(0U);
	put32(2U);
	put64(4U);
	put32(0U);
	put32(0x3f800000U);
	put32(0U);
	put32(0x3f800000U);
	put32(1U);
	put64(1U);
	put32(0U);
	put32(0U);
	put32(64U);
	put32(32U);
	put32(0U);
	put32(1U);
	x_command(GPU_OP_CMD_CLEAR_ATTACHMENTS, ID_X_COMMANDS);
	put32(1U);
	put64(1U);
	put32(VK_IMAGE_ASPECT_COLOR_BIT);
	put32(0U);
	put32(0U);
	put32(2U);
	put64(4U);
	put32(0U);
	put32(0U);
	put32(0U);
	put32(0U);
	put32(1U);
	put64(1U);
	put32(8U);
	put32(8U);
	put32(16U);
	put32(16U);
	put32(0U);
	put32(1U);
	x_command(GPU_OP_CMD_END_RENDER_PASS, ID_X_COMMANDS);
	error = x_end(&node, ID_X_COMMANDS);
	mark = model.command_count;
	error |= x_submit(&node, ID_X_COMMANDS, 0U);
	count = x_ops(mark, &first, ops);
	expect(error == 0 && count == 9U && ops[2] == GHCI_OP_BEGIN_RENDER_PASS && ops[3] == GHCI_OP_END_RENDER_PASS &&
	       ops[4] == GHCI_OP_BEGIN_RENDER_PASS && ops[5] == GHCI_OP_END_RENDER_PASS,
	       "clears: the whole area begins the subpass again; the partial clear is skipped");
	if (count == 9U) {
		command = &model.commands[first + 4U];
		desc = x_gpu(command->session, command->words[1] | ((uint64_t)command->words[2] << 32));
		table = desc != NULL ? x_gpu(command->session, x_load(desc + 0xa8U, 8U)) : NULL;
		expect(command->words[8] == 1U && desc != NULL && desc[0x0dU] == 1U && table != NULL && x_load(table + 4U, 4U) == 0x3f800000U,
		       "clears: colour 0 cleared to the attachment clear's value");
	}
	node_close(&node);
	stop("clears");
}

/*
 * A submission larger than the ring (a 4 KiB ring and 400 draws), and a
 * vertex buffer bound near the buffer's end: the bytes to the end are the
 * binding's size (the vertex fetch reads 0 past it).
 */
static void
scene_x_large(void)
{
	const struct model_command *command;
	unsigned draws;
	struct node node;
	int index;
	int error;

	/* A 4 KiB ring. */
	test_ring_bytes = 4096U;
	error = x_open(&node, MODEL_FULL);
	test_ring_bytes = 0U;
	error |= (int)x_triangle(&node, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, UINT32_MAX, ID_PIPELINE);
	expect(error == 0, "large: objects");

	/* 400 draws (2000 dwords), the vertex buffer at 8000 of 8192. */
	x_record(&node, ID_X_COMMANDS);
	x_begin_pass(ID_X_COMMANDS, 64U, 32U);
	x_bind_triangle(ID_X_COMMANDS, ID_PIPELINE, 8000U);
	for (index = 0; index < 400; index++) {
		x_command(GPU_OP_CMD_DRAW, ID_X_COMMANDS);
		put32(3U);
		put32(1U);
		put32((uint32_t)index);
		put32(0U);
	}
	x_command(GPU_OP_CMD_END_RENDER_PASS, ID_X_COMMANDS);
	error = x_end(&node, ID_X_COMMANDS);
	draws = model_count_commands(GHCI_OP_DRAW);
	error |= x_submit(&node, ID_X_COMMANDS, ID_X_FENCE);
	begin(&node, GPU_OP_GET_FENCE_STATUS);
	put64(ID_DEVICE);
	put64(ID_X_FENCE);
	error |= run(&node);
	expect(error == 0 && model_count_commands(GHCI_OP_DRAW) == draws + 400U && reply32(&node, 4U) == (uint32_t)VK_SUCCESS &&
	       model.faults == 0U, "large: 400 draws through a 4 KiB ring, the fence passed");
	command = model_last_command(GHCI_OP_BIND_VERTEX_BUFFERS);
	expect(command != NULL && (command->words[2] | ((uint64_t)command->words[3] << 32)) == x_va(&node, 0x80000U + 8000U) &&
	       command->words[4] == 192U, "large: the vertex binding's size is the 192 bytes to the buffer's end");
	command = model_last_command(GHCI_OP_DRAW);
	expect(command != NULL && command->words[3] == 399U, "large: the last draw is the 400th");
	node_close(&node);
	stop("large");
}

/*
 * A triangle fan (R30): five vertices drawn as three triangles from an
 * index list (i + 1, i + 2, 0) with the first vertex as the offset; an
 * indexed fan is skipped.
 */
static void
scene_x_fan(void)
{
	const struct model_command *command;
	const uint8_t *indices;
	static const uint32_t expected[9] = { 1U, 2U, 0U, 2U, 3U, 0U, 3U, 4U, 0U };
	uint32_t index;
	unsigned drawn;
	struct node node;
	int same;
	int error;

	/* The fan and its draw. */
	error = x_open(&node, MODEL_FULL);
	error |= (int)x_triangle(&node, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN, UINT32_MAX, ID_PIPELINE);
	x_record(&node, ID_X_COMMANDS);
	x_begin_pass(ID_X_COMMANDS, 64U, 32U);
	x_bind_triangle(ID_X_COMMANDS, ID_PIPELINE, 0U);
	x_command(GPU_OP_CMD_DRAW, ID_X_COMMANDS);
	put32(5U);
	put32(2U);
	put32(7U);
	put32(1U);
	x_command(GPU_OP_CMD_BIND_INDEX_BUFFER, ID_X_COMMANDS);
	put64(ID_BUFFER);
	put64(0U);
	put32(VK_INDEX_TYPE_UINT16);
	x_command(GPU_OP_CMD_DRAW_INDEXED, ID_X_COMMANDS);
	put32(6U);
	put32(1U);
	put32(0U);
	put32(0U);
	put32(0U);
	x_command(GPU_OP_CMD_END_RENDER_PASS, ID_X_COMMANDS);
	error |= x_end(&node, ID_X_COMMANDS);
	drawn = model_count_commands(GHCI_OP_DRAW_INDEXED);
	error |= x_submit(&node, ID_X_COMMANDS, 0U);
	command = model_last_command(GHCI_OP_DRAW_INDEXED);
	expect(error == 0 && model_count_commands(GHCI_OP_DRAW_INDEXED) == drawn + 1U && model_count_commands(GHCI_OP_DRAW) == 0U &&
	       command != NULL && command->words[1] == 9U && command->words[2] == 2U && command->words[3] == 0U && command->words[4] == 7U &&
	       command->words[5] == 1U, "fan: DRAW_INDEXED of 9 indices, 2 instances, vertex offset 7; the indexed fan skipped");
	command = model_last_command(GHCI_OP_BIND_INDEX_BUFFER);
	indices = command != NULL ? x_gpu(command->session, command->words[1] | ((uint64_t)command->words[2] << 32)) : NULL;
	same = indices != NULL && command->words[3] == 1U;
	for (index = 0U; same && index < 9U; index++)
		same = x_load(indices + index * 4U, 4U) == expected[index];
	expect(same, "fan: the 32-bit index list (1, 2, 0), (2, 3, 0), (3, 4, 0)");

	/* A fan of 2^32 - 1 vertices is skipped (no index list that long); one of 20000 is drawn from a list past one chunk. */
	x_record(&node, ID_X_COMMANDS);
	x_begin_pass(ID_X_COMMANDS, 64U, 32U);
	x_bind_triangle(ID_X_COMMANDS, ID_PIPELINE, 0U);
	x_command(GPU_OP_CMD_DRAW, ID_X_COMMANDS);
	put32(0xffffffffU);
	put32(1U);
	put32(0U);
	put32(0U);
	x_command(GPU_OP_CMD_DRAW, ID_X_COMMANDS);
	put32(20000U);
	put32(1U);
	put32(0U);
	put32(0U);
	x_command(GPU_OP_CMD_END_RENDER_PASS, ID_X_COMMANDS);
	error = x_end(&node, ID_X_COMMANDS);
	drawn = model_count_commands(GHCI_OP_DRAW_INDEXED);
	error |= x_submit(&node, ID_X_COMMANDS, 0U);
	command = model_last_command(GHCI_OP_DRAW_INDEXED);
	expect(error == 0 && model_count_commands(GHCI_OP_DRAW_INDEXED) == drawn + 1U && command != NULL && command->words[1] == 3U * 19998U,
	       "fan: 2^32 - 1 vertices skipped, 20000 drawn as 19998 triangles");
	command = model_last_command(GHCI_OP_BIND_INDEX_BUFFER);
	indices = command != NULL ? x_gpu(command->session, command->words[1] | ((uint64_t)command->words[2] << 32)) : NULL;
	expect(indices != NULL && x_load(indices + 19997U * 12U, 4U) == 19998U && x_load(indices + 19997U * 12U + 4U, 4U) == 19999U &&
	       x_load(indices + 19997U * 12U + 8U, 4U) == 0U, "fan: the last triangle is (19998, 19999, 0)");
	node_close(&node);
	stop("fan");
}

/*
 * The heap runs a submission reads (design 4.12): a pipeline destroyed
 * while the submission that draws with it has not passed keeps its run
 * (a pipeline made then takes another), and the run goes back once the
 * fence passed.
 */
static void
scene_x_holds(void)
{
	const struct ghci_vk_pipeline *pipeline;
	struct ghci_vk_device *record;
	uint64_t held_va;
	struct node node;
	int error;

	/* A pipeline and a draw with it, held in the model. */
	error = x_open(&node, MODEL_FULL);
	error |= (int)x_triangle(&node, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, UINT32_MAX, ID_PIPELINE);
	pipeline = pipeline_of(&node, ID_PIPELINE);
	record = session_of(&node)->devices;
	expect(error == 0 && pipeline != NULL && record != NULL, "holds: objects");
	if (pipeline == NULL || record == NULL) {
		node_close(&node);
		stop("holds");
		return;
	}
	held_va = pipeline->pso_va;
	x_record(&node, ID_X_COMMANDS);
	x_begin_pass(ID_X_COMMANDS, 64U, 32U);
	x_bind_triangle(ID_X_COMMANDS, ID_PIPELINE, 0U);
	x_command(GPU_OP_CMD_DRAW, ID_X_COMMANDS);
	put32(3U);
	put32(1U);
	put32(0U);
	put32(0U);
	x_command(GPU_OP_CMD_END_RENDER_PASS, ID_X_COMMANDS);
	error = x_end(&node, ID_X_COMMANDS);
	model_hold(1);
	error |= x_submit(&node, ID_X_COMMANDS, ID_X_FENCE);

	/* Destroyed while the submission waits: its run waits too, and a new pipeline takes another. */
	begin(&node, GPU_OP_DESTROY_PIPELINE);
	put64(ID_DEVICE);
	put64(ID_PIPELINE);
	put64(0U);
	error |= run(&node);
	expect(error == 0 && record->retired != NULL, "holds: the destroyed pipeline's run waits for the fence");
	error = (int)x_triangle(&node, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, UINT32_MAX, ID_PIPELINE + 1U);
	pipeline = pipeline_of(&node, ID_PIPELINE + 1U);
	expect(error == 0 && pipeline != NULL && pipeline->pso_va != held_va, "holds: a pipeline made meanwhile takes another run");

	/* The fence passes: the run goes back, and the next pipeline may take it. */
	model_hold(0);
	ghci_service_step(&device, GHCI_WORK_RECLAIM);
	begin(&node, GPU_OP_GET_FENCE_STATUS);
	put64(ID_DEVICE);
	put64(ID_X_FENCE);
	error = run(&node);
	expect(error == 0 && reply32(&node, 4U) == (uint32_t)VK_SUCCESS, "holds: the fence passed");
	error = (int)x_triangle(&node, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, UINT32_MAX, ID_PIPELINE + 2U);
	pipeline = pipeline_of(&node, ID_PIPELINE + 2U);
	expect(error == 0 && record->retired == NULL && pipeline != NULL && pipeline->pso_va == held_va,
	       "holds: after the fence the run is free again and reused");
	node_close(&node);
	stop("holds");
}

/*
 * Makes a 64 x 32 image of one level and layer, bound at offset of the
 * scene's memory, and a view of it.  Returns 0, or 1.
 */
static int
x_image(
	struct node *node,
	uint64_t identity,
	uint64_t view,
	uint32_t format,
	uint32_t samples,
	uint32_t usage,
	uint64_t offset)
{
	int error;

	/* The image, bound. */
	begin(node, GPU_OP_CREATE_IMAGE);
	put64(ID_DEVICE);
	put64(1U);
	put32(VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO);
	put64(0U);
	put32(0U);
	put32(VK_IMAGE_TYPE_2D);
	put32(format);
	put32(64U);
	put32(32U);
	put32(1U);
	put32(1U);
	put32(1U);
	put32(samples);
	put32(VK_IMAGE_TILING_OPTIMAL);
	put32(usage);
	put32(VK_SHARING_MODE_EXCLUSIVE);
	put32(0U);
	put64(0U);
	put32(VK_IMAGE_LAYOUT_UNDEFINED);
	tail(identity);
	error = run(node);
	if (error != 0 || reply32(node, 4U) != 0U)
		return 1;
	begin(node, GPU_OP_BIND_IMAGE_MEMORY);
	put64(ID_DEVICE);
	put64(identity);
	put64(ID_MEMORY);
	put64(offset);
	error = run(node);
	if (error != 0 || reply32(node, 4U) != 0U)
		return 1;

	/* Its view. */
	begin(node, GPU_OP_CREATE_IMAGE_VIEW);
	put64(ID_DEVICE);
	put64(1U);
	put32(VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO);
	put64(0U);
	put32(0U);
	put64(identity);
	put32(VK_IMAGE_VIEW_TYPE_2D);
	put32(format);
	put32(VK_COMPONENT_SWIZZLE_IDENTITY);
	put32(VK_COMPONENT_SWIZZLE_IDENTITY);
	put32(VK_COMPONENT_SWIZZLE_IDENTITY);
	put32(VK_COMPONENT_SWIZZLE_IDENTITY);
	put32(format == VK_FORMAT_D32_SFLOAT ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT);
	put32(0U);
	put32(1U);
	put32(0U);
	put32(1U);
	tail(view);
	error = run(node);
	if (error != 0 || reply32(node, 4U) != 0U)
		return 1;
	return 0;
}

/*
 * Two subpasses of 4-sample attachments: subpass 0 clears colours 0 and 1
 * (0 not stored, its resolve in the description; 1 stored and resolved
 * with RESOLVE_IMAGE after the subpass) and a depth attachment (cleared,
 * no stencil to clear, not stored); subpass 1 loads colour 1 and stores
 * it.
 */
static void
scene_x_subpasses(void)
{
	static const uint32_t expected[] = {
		GHCI_OP_BIND_SESSION, GHCI_OP_BARRIER, GHCI_OP_BEGIN_RENDER_PASS, GHCI_OP_END_RENDER_PASS, GHCI_OP_BARRIER,
		GHCI_OP_RESOLVE_IMAGE, GHCI_OP_BEGIN_RENDER_PASS, GHCI_OP_END_RENDER_PASS, GHCI_OP_BARRIER, GHCI_OP_FENCE,
		GHCI_OP_BIND_SESSION
	};
	const struct model_command *command;
	const uint8_t *desc;
	struct test_pass pass;
	uint32_t ops[X_OPS_MAX];
	uint64_t va[5];
	unsigned first;
	unsigned count;
	unsigned mark;
	struct node node;
	uint32_t index;
	int error;

	/* Two 4-sample colours, two resolves, a 4-sample depth. */
	error = x_open(&node, MODEL_FULL);
	error |= x_image(&node, ID_X_IMAGE + 1U, ID_X_VIEW + 1U, VK_FORMAT_R8G8B8A8_UNORM, 4U, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, 0x50000U);
	error |= x_image(&node, ID_X_IMAGE + 2U, ID_X_VIEW + 2U, VK_FORMAT_R8G8B8A8_UNORM, 4U, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, 0x58000U);
	error |= x_image(&node, ID_X_IMAGE + 3U, ID_X_VIEW + 3U, VK_FORMAT_R8G8B8A8_UNORM, 1U,
		VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, 0x60000U);
	error |= x_image(&node, ID_X_IMAGE + 4U, ID_X_VIEW + 4U, VK_FORMAT_D32_SFLOAT, 4U, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, 0x68000U);
	memset(&pass, 0, sizeof(pass));
	pass.attachment_count = 5U;
	for (index = 0U; index < 5U; index++) {
		pass.format[index] = index == 4U ? VK_FORMAT_D32_SFLOAT : VK_FORMAT_R8G8B8A8_UNORM;
		pass.samples[index] = index == 2U || index == 3U ? 1U : 4U;
		pass.load[index] = index == 2U || index == 3U ? VK_ATTACHMENT_LOAD_OP_DONT_CARE : VK_ATTACHMENT_LOAD_OP_CLEAR;
		pass.store[index] = index == 0U || index == 4U ? VK_ATTACHMENT_STORE_OP_DONT_CARE : VK_ATTACHMENT_STORE_OP_STORE;
	}
	pass.subpass_count = 2U;
	pass.color_count[0] = 2U;
	pass.color[0][0] = 0U;
	pass.color[0][1] = 1U;
	pass.resolves[0] = 1U;
	pass.resolve[0][0] = 2U;
	pass.resolve[0][1] = 3U;
	pass.depth[0] = 4U;
	pass.color_count[1] = 1U;
	pass.color[1][0] = 1U;
	pass.depth[1] = VK_ATTACHMENT_UNUSED;
	error |= (int)make_render_pass(&node, ID_X_PASS + 1U, &pass);
	begin(&node, GPU_OP_CREATE_FRAMEBUFFER);
	put64(ID_DEVICE);
	put64(1U);
	put32(VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO);
	put64(0U);
	put32(0U);
	put64(ID_X_PASS + 1U);
	put32(5U);
	put64(5U);
	put64(ID_X_VIEW + 1U);
	put64(ID_X_VIEW + 2U);
	put64(ID_X_VIEW);
	put64(ID_X_VIEW + 3U);
	put64(ID_X_VIEW + 4U);
	put32(64U);
	put32(32U);
	put32(1U);
	tail(ID_X_FRAMEBUFFER + 1U);
	error |= run(&node);
	error |= (int)reply32(&node, 4U);
	expect(error == 0, "subpasses: objects");
	va[0] = x_va(&node, 0x50000U);
	va[1] = x_va(&node, 0x58000U);
	va[2] = x_va(&node, X_ATTACHMENT_OFFSET);
	va[3] = x_va(&node, 0x60000U);
	va[4] = x_va(&node, 0x68000U);

	/* The pass begun with five clear values, the next subpass, the end. */
	x_record(&node, ID_X_COMMANDS);
	x_command(GPU_OP_CMD_BEGIN_RENDER_PASS, ID_X_COMMANDS);
	put64(1U);
	put32(VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO);
	put64(0U);
	put64(ID_X_PASS + 1U);
	put64(ID_X_FRAMEBUFFER + 1U);
	put32(0U);
	put32(0U);
	put32(64U);
	put32(32U);
	put32(5U);
	put64(5U);
	for (index = 0U; index < 4U; index++) {
		put32(0U);
		put32(2U);
		put64(4U);
		put32(index);
		put32(0U);
		put32(0U);
		put32(0x3f800000U);
	}
	put32(1U);
	put32(0x3f800000U);
	put32(0U);
	put32(VK_SUBPASS_CONTENTS_INLINE);
	x_command(GPU_OP_CMD_NEXT_SUBPASS, ID_X_COMMANDS);
	put32(VK_SUBPASS_CONTENTS_INLINE);
	x_command(GPU_OP_CMD_END_RENDER_PASS, ID_X_COMMANDS);
	error = x_end(&node, ID_X_COMMANDS);
	mark = model.command_count;
	error |= x_submit(&node, ID_X_COMMANDS, 0U);
	count = x_ops(mark, &first, ops);
	expect(error == 0 && x_same(ops, count, expected, sizeof(expected) / sizeof(expected[0])), "subpasses: subpass 0, colour 1's resolve, subpass 1");
	if (count != sizeof(expected) / sizeof(expected[0])) {
		node_close(&node);
		stop("subpasses");
		return;
	}

	/* Subpass 0: colour 0 cleared and not stored, colour 1 cleared and stored, depth cleared, colour 0's resolve. */
	command = &model.commands[first + 2U];
	desc = x_gpu(command->session, command->words[1] | ((uint64_t)command->words[2] << 32));
	expect(command->words[8] == (3U | GHCI_CLEAR_DEPTH) && command->words[6] == 0x3f800000U && desc != NULL,
	       "subpass 0: CLEAR_COLOR 0 and 1 and CLEAR_DEPTH (no stencil), depth 1.0");
	if (desc != NULL) {
		expect(x_load(desc, 8U) == va[0] && desc[0x0dU] == (1U | (1U << 2)) && x_load(desc + 0x10U, 8U) == va[1] &&
		       desc[0x1dU] == 1U, "subpass 0: colour 0 CLEAR | DONT_CARE, colour 1 CLEAR | STORE");
		expect(x_load(desc + 0x80U, 8U) == va[4] && desc[0x8cU] == 126U && desc[0x8dU] == (1U | (1U << 2) | (1U << 4) | (1U << 6)),
		       "subpass 0: depth D32_SFLOAT, CLEAR | DONT_CARE");
		expect(x_load(desc + 0x90U, 8U) == va[2] && desc[0x9cU] == 37U && desc[0x9dU] == 0U && desc[0xa6U] == 4U,
		       "subpass 0: colour 0 resolved to attachment 2 by the description (average), 4 samples");
	}

	/* Colour 1's resolve after the subpass, after a barrier that waits for the pass's writes. */
	command = &model.commands[first + 4U];
	expect(command->words[1] == GHCI_STAGE_ROP && command->words[2] == GHCI_STAGE_BLT &&
	       command->words[3] == (GHCI_BARRIER_WAIT_IDLE | GHCI_BARRIER_FLUSH_L1_DATA), "subpasses: BARRIER ROP before BLT, idle and flush L1");
	command = &model.commands[first + 5U];
	expect((command->words[1] | ((uint64_t)command->words[2] << 32)) == va[1] &&
	       (command->words[3] | ((uint64_t)command->words[4] << 32)) == va[3] && command->words[9] == (64U | (32U << 16)) &&
	       command->words[10] == (37U | (0U << 8) | (2U << 10)), "subpasses: RESOLVE_IMAGE of colour 1 to attachment 3, 4 samples, average");

	/* Subpass 1: colour 1 loaded and stored, nothing cleared. */
	command = &model.commands[first + 6U];
	desc = x_gpu(command->session, command->words[1] | ((uint64_t)command->words[2] << 32));
	expect(command->words[8] == 0U && desc != NULL && x_load(desc, 8U) == va[1] && desc[0x0dU] == 0U && x_load(desc + 0x10U, 8U) == 0U &&
	       x_load(desc + 0x80U, 8U) == 0U && x_load(desc + 0x90U, 8U) == 0U, "subpass 1: colour 1 LOAD | STORE alone");
	node_close(&node);
	stop("subpasses");
}

/*
 * A secondary command buffer executed within a render pass: its draw
 * runs from a state where nothing is bound; submitted alone it is
 * refused.
 */
static void
scene_x_secondary(void)
{
	unsigned draws;
	unsigned fences;
	struct node node;
	int error;

	/* A secondary buffer that binds and draws. */
	error = x_open(&node, MODEL_FULL);
	error |= (int)x_triangle(&node, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, UINT32_MAX, ID_PIPELINE);
	begin(&node, GPU_OP_ALLOCATE_COMMAND_BUFFERS);
	put64(ID_DEVICE);
	put64(1U);
	put32(VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO);
	put64(0U);
	put64(ID_X_POOL);
	put32(VK_COMMAND_BUFFER_LEVEL_SECONDARY);
	put32(1U);
	put64(1U);
	put64(ID_X_COMMANDS + 8U);
	error |= run(&node);
	x_record(&node, ID_X_COMMANDS + 8U);
	x_bind_triangle(ID_X_COMMANDS + 8U, ID_PIPELINE, 0U);
	x_command(GPU_OP_CMD_DRAW, ID_X_COMMANDS + 8U);
	put32(6U);
	put32(1U);
	put32(0U);
	put32(0U);
	error |= x_end(&node, ID_X_COMMANDS + 8U);

	/* The primary executes it in its pass. */
	x_record(&node, ID_X_COMMANDS);
	x_begin_pass(ID_X_COMMANDS, 64U, 32U);
	x_command(GPU_OP_CMD_EXECUTE_COMMANDS, ID_X_COMMANDS);
	put32(1U);
	put64(1U);
	put64(ID_X_COMMANDS + 8U);
	x_command(GPU_OP_CMD_END_RENDER_PASS, ID_X_COMMANDS);
	error |= x_end(&node, ID_X_COMMANDS);
	draws = model_count_commands(GHCI_OP_DRAW);
	error |= x_submit(&node, ID_X_COMMANDS, 0U);
	expect(error == 0 && model_count_commands(GHCI_OP_DRAW) == draws + 1U && model_last_command(GHCI_OP_DRAW)->words[1] == 6U &&
	       model_count_commands(GHCI_OP_EXECUTE_COMMANDS) == 0U, "secondary: its draw runs inline in the primary's pass");

	/* Submitted alone, a secondary buffer is refused. */
	fences = model_count_commands(GHCI_OP_FENCE);
	error = x_submit(&node, ID_X_COMMANDS + 8U, 0U);
	expect(error == EINVAL && model_count_commands(GHCI_OP_FENCE) == fences, "secondary: not submitted on its own");
	node_close(&node);
	stop("secondary");
}

/* Records an indirect draw or dispatch from the scene's buffer: [buffer][u64 offset], and a draw's [count][stride]. */
static void
x_indirect(
	uint64_t commands,
	uint32_t opcode,
	uint64_t offset,
	uint32_t draws,
	uint32_t stride)
{
	x_command(opcode, commands);
	put64(ID_BUFFER);
	put64(offset);
	if (opcode == GPU_OP_CMD_DISPATCH_INDIRECT)
		return;
	put32(draws);
	put32(stride);
}

/* Records vkCmdBeginRenderPass of a pass and framebuffer over 64 x 32 with count clear colours (index, 0, 0, 1). */
static void
x_begin_with(
	uint64_t commands,
	uint64_t pass,
	uint64_t framebuffer,
	uint32_t count)
{
	uint32_t index;

	x_command(GPU_OP_CMD_BEGIN_RENDER_PASS, commands);
	put64(1U);
	put32(VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO);
	put64(0U);
	put64(pass);
	put64(framebuffer);
	put32(0U);
	put32(0U);
	put32(64U);
	put32(32U);
	put32(count);
	put64(count);
	for (index = 0U; index < count; index++) {
		put32(0U);
		put32(2U);
		put64(4U);
		put32(index);
		put32(0U);
		put32(0U);
		put32(0x3f800000U);
	}
	put32(VK_SUBPASS_CONTENTS_INLINE);
}

/* Records vkCmdClearAttachments of colour 0 to (0, 1, 0, 1) over the whole 64 x 32 area of layers [first, first + count). */
static void
x_clear_color0(
	uint64_t commands,
	uint32_t first,
	uint32_t count)
{
	x_command(GPU_OP_CMD_CLEAR_ATTACHMENTS, commands);
	put32(1U);
	put64(1U);
	put32(VK_IMAGE_ASPECT_COLOR_BIT);
	put32(0U);
	put32(0U);
	put32(2U);
	put64(4U);
	put32(0U);
	put32(0x3f800000U);
	put32(0U);
	put32(0x3f800000U);
	put32(1U);
	put64(1U);
	put32(0U);
	put32(0U);
	put32(64U);
	put32(32U);
	put32(first);
	put32(count);
}

/* Makes a framebuffer of a pass of one attachment with one view, width x height x layers.  Returns the frame's error, or the result. */
static uint32_t
x_framebuffer(
	struct node *node,
	uint64_t identity,
	uint64_t pass,
	uint64_t view,
	uint32_t width,
	uint32_t height,
	uint32_t layers)
{
	int error;

	begin(node, GPU_OP_CREATE_FRAMEBUFFER);
	put64(ID_DEVICE);
	put64(1U);
	put32(VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO);
	put64(0U);
	put32(0U);
	put64(pass);
	put32(1U);
	put64(1U);
	put64(view);
	put32(width);
	put32(height);
	put32(layers);
	tail(identity);
	error = run(node);
	if (error != 0)
		return (uint32_t)error;
	return reply32(node, 4U);
}

/*
 * The ranges of indirect and indexed draws: indirect records at an
 * offset that is not a multiple of 4, with a stride below a record or
 * not a multiple of 4, past the buffer (also by 2^30 records, which 32
 * bits would wrap) are skipped, and one at the buffer's very end runs;
 * the same for an indirect dispatch.  Indexed draws past the index
 * buffer's end, and from one bound at an odd offset, are skipped.
 */
static void
scene_x_ranges(void)
{
	const struct model_command *command;
	unsigned indirect;
	unsigned dispatches;
	unsigned indexed;
	struct node node;
	int error;

	/* With CAP_INDIRECT_DRAW: the triangle, the compute pipeline and its set. */
	error = x_open(&node, MODEL_FULL);
	error |= x_compute_set(&node);
	error |= (int)x_triangle(&node, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, UINT32_MAX, ID_PIPELINE);
	expect(error == 0, "ranges: objects");

	/* The indirect draws the rules skip, then one at the end of the 8192 bytes, and the dispatches. */
	x_record(&node, ID_X_COMMANDS);
	x_begin_pass(ID_X_COMMANDS, 64U, 32U);
	x_bind_triangle(ID_X_COMMANDS, ID_PIPELINE, 0U);
	x_indirect(ID_X_COMMANDS, GPU_OP_CMD_DRAW_INDIRECT, 66U, 1U, 16U);
	x_indirect(ID_X_COMMANDS, GPU_OP_CMD_DRAW_INDIRECT, 64U, 2U, 8U);
	x_indirect(ID_X_COMMANDS, GPU_OP_CMD_DRAW_INDIRECT, 64U, 2U, 18U);
	x_indirect(ID_X_COMMANDS, GPU_OP_CMD_DRAW_INDIRECT, 8192U - 16U, 2U, 16U);
	x_indirect(ID_X_COMMANDS, GPU_OP_CMD_DRAW_INDEXED_INDIRECT, 8192U - 16U, 1U, 20U);
	x_indirect(ID_X_COMMANDS, GPU_OP_CMD_DRAW_INDIRECT, 0U, 0x40000000U, 16U);
	x_indirect(ID_X_COMMANDS, GPU_OP_CMD_DRAW_INDIRECT, 0U, 0U, 16U);
	x_indirect(ID_X_COMMANDS, GPU_OP_CMD_DRAW_INDIRECT, 8192U - 16U, 1U, 0U);
	x_command(GPU_OP_CMD_END_RENDER_PASS, ID_X_COMMANDS);
	x_command(GPU_OP_CMD_BIND_PIPELINE, ID_X_COMMANDS);
	put32(VK_PIPELINE_BIND_POINT_COMPUTE);
	put64(ID_X_COMPUTE);
	x_command(GPU_OP_CMD_BIND_DESCRIPTOR_SETS, ID_X_COMMANDS);
	put32(VK_PIPELINE_BIND_POINT_COMPUTE);
	put64(ID_LAYOUT);
	put32(0U);
	put32(1U);
	put64(1U);
	put64(ID_SET);
	put32(1U);
	put64(1U);
	put32(0U);
	x_indirect(ID_X_COMMANDS, GPU_OP_CMD_DISPATCH_INDIRECT, 8192U - 8U, 0U, 0U);
	x_indirect(ID_X_COMMANDS, GPU_OP_CMD_DISPATCH_INDIRECT, 6U, 0U, 0U);
	x_indirect(ID_X_COMMANDS, GPU_OP_CMD_DISPATCH_INDIRECT, 8192U - 12U, 0U, 0U);
	error = x_end(&node, ID_X_COMMANDS);
	indirect = model_count_commands(GHCI_OP_DRAW_INDIRECT);
	dispatches = model_count_commands(GHCI_OP_DISPATCH_INDIRECT);
	error |= x_submit(&node, ID_X_COMMANDS, 0U);
	command = model_last_command(GHCI_OP_DRAW_INDIRECT);
	expect(error == 0 && model_count_commands(GHCI_OP_DRAW_INDIRECT) == indirect + 1U && command != NULL && command->words[3] == 8192U - 16U &&
	       command->words[4] == 1U && command->words[5] == 16U, "ranges: of the indirect draws only the one at the buffer's end runs (stride 16)");
	command = model_last_command(GHCI_OP_DISPATCH_INDIRECT);
	expect(model_count_commands(GHCI_OP_DISPATCH_INDIRECT) == dispatches + 1U && command != NULL && command->words[3] == 8192U - 12U,
	       "ranges: of the indirect dispatches only the one whose 12 bytes fit runs");

	/* Indexed draws: 6 indices of 16 bits at the buffer's end, 7, from index 5, and from an odd offset. */
	x_record(&node, ID_X_COMMANDS + 1U);
	x_begin_pass(ID_X_COMMANDS + 1U, 64U, 32U);
	x_bind_triangle(ID_X_COMMANDS + 1U, ID_PIPELINE, 0U);
	x_command(GPU_OP_CMD_BIND_INDEX_BUFFER, ID_X_COMMANDS + 1U);
	put64(ID_BUFFER);
	put64(8192U - 12U);
	put32(VK_INDEX_TYPE_UINT16);
	x_command(GPU_OP_CMD_DRAW_INDEXED, ID_X_COMMANDS + 1U);
	put32(6U);
	put32(1U);
	put32(0U);
	put32(0U);
	put32(0U);
	x_command(GPU_OP_CMD_DRAW_INDEXED, ID_X_COMMANDS + 1U);
	put32(7U);
	put32(1U);
	put32(0U);
	put32(0U);
	put32(0U);
	x_command(GPU_OP_CMD_DRAW_INDEXED, ID_X_COMMANDS + 1U);
	put32(2U);
	put32(1U);
	put32(5U);
	put32(0U);
	put32(0U);
	x_command(GPU_OP_CMD_BIND_INDEX_BUFFER, ID_X_COMMANDS + 1U);
	put64(ID_BUFFER);
	put64(1U);
	put32(VK_INDEX_TYPE_UINT16);
	x_command(GPU_OP_CMD_DRAW_INDEXED, ID_X_COMMANDS + 1U);
	put32(1U);
	put32(1U);
	put32(0U);
	put32(0U);
	put32(0U);
	x_command(GPU_OP_CMD_END_RENDER_PASS, ID_X_COMMANDS + 1U);
	error = x_end(&node, ID_X_COMMANDS + 1U);
	indexed = model_count_commands(GHCI_OP_DRAW_INDEXED);
	error |= x_submit(&node, ID_X_COMMANDS + 1U, 0U);
	command = model_last_command(GHCI_OP_DRAW_INDEXED);
	expect(error == 0 && model_count_commands(GHCI_OP_DRAW_INDEXED) == indexed + 1U && command != NULL && command->words[1] == 6U &&
	       model.faults == 0U, "ranges: the 6 indices in the buffer are drawn; past its end and from an odd offset skipped");
	node_close(&node);
	stop("ranges");
}

/*
 * The word a wait of the GPU watches: the client's vkResetEvent on the
 * host after the submission that sets and waits for the event does not
 * stop the ring (the wait watches the submission's own word); a wait
 * after the submission reset the event it set is held: the part before
 * the wait is placed, and the rest once the host sets the event.
 */
static void
scene_x_events(void)
{
	unsigned waits;
	unsigned fences;
	struct node node;
	int error;

	/* The device and an event. */
	error = x_open(&node, MODEL_FULL);
	begin(&node, GPU_OP_CREATE_EVENT);
	put64(ID_DEVICE);
	put64(1U);
	put32(VK_STRUCTURE_TYPE_EVENT_CREATE_INFO);
	put64(0U);
	put32(0U);
	tail(ID_X_EVENT);
	error |= run(&node);
	expect(error == 0, "events: objects");

	/* SetEvent and WaitEvents, held in the model, and the host's reset meanwhile. */
	x_record(&node, ID_X_COMMANDS);
	x_command(GPU_OP_CMD_SET_EVENT, ID_X_COMMANDS);
	put64(ID_X_EVENT);
	put32(VK_PIPELINE_STAGE_TRANSFER_BIT);
	x_command(GPU_OP_CMD_WAIT_EVENTS, ID_X_COMMANDS);
	put32(1U);
	put64(1U);
	put64(ID_X_EVENT);
	put32(VK_PIPELINE_STAGE_TRANSFER_BIT);
	put32(VK_PIPELINE_STAGE_TRANSFER_BIT);
	put32(0U);
	put64(0U);
	put32(0U);
	put64(0U);
	put32(0U);
	put64(0U);
	error = x_end(&node, ID_X_COMMANDS);
	waits = model_count_commands(GHCI_OP_WAIT_FENCE);
	model_hold(1);
	error |= x_submit(&node, ID_X_COMMANDS, ID_X_FENCE);
	begin(&node, GPU_OP_RESET_EVENT);
	put64(ID_DEVICE);
	put64(ID_X_EVENT);
	error |= run(&node);
	model_hold(0);
	model_tick();
	ghci_service_step(&device, GHCI_WORK_RECLAIM);
	begin(&node, GPU_OP_GET_FENCE_STATUS);
	put64(ID_DEVICE);
	put64(ID_X_FENCE);
	error |= run(&node);
	expect(error == 0 && reply32(&node, 4U) == (uint32_t)VK_SUCCESS && model_count_commands(GHCI_OP_WAIT_FENCE) == waits + 1U &&
	       model.waiting_fence == 0 && model.faults == 0U, "events: the host's reset after the submission does not stop the wait");

	/* Set, reset, then a wait in one submission: the part before the wait is placed, the rest held. */
	x_record(&node, ID_X_COMMANDS);
	x_command(GPU_OP_CMD_SET_EVENT, ID_X_COMMANDS);
	put64(ID_X_EVENT);
	put32(VK_PIPELINE_STAGE_TRANSFER_BIT);
	x_command(GPU_OP_CMD_RESET_EVENT, ID_X_COMMANDS);
	put64(ID_X_EVENT);
	put32(VK_PIPELINE_STAGE_TRANSFER_BIT);
	x_command(GPU_OP_CMD_WAIT_EVENTS, ID_X_COMMANDS);
	put32(1U);
	put64(1U);
	put64(ID_X_EVENT);
	put32(VK_PIPELINE_STAGE_TRANSFER_BIT);
	put32(VK_PIPELINE_STAGE_TRANSFER_BIT);
	put32(0U);
	put64(0U);
	put32(0U);
	put64(0U);
	put32(0U);
	put64(0U);
	error = x_end(&node, ID_X_COMMANDS);
	fences = model_count_commands(GHCI_OP_FENCE);
	waits = model_count_commands(GHCI_OP_WAIT_FENCE);
	error |= x_submit(&node, ID_X_COMMANDS, ID_X_FENCE);
	ghci_service_step(&device, GHCI_WORK_RECLAIM);
	begin(&node, GPU_OP_GET_FENCE_STATUS);
	put64(ID_DEVICE);
	put64(ID_X_FENCE);
	error |= run(&node);
	expect(error == 0 && reply32(&node, 4U) == (uint32_t)VK_NOT_READY && model_count_commands(GHCI_OP_FENCE) == fences + 4U &&
	       model_count_commands(GHCI_OP_WAIT_FENCE) == waits && model.waiting_fence == 0,
	       "events: a wait after the submission reset the event: the set, the reset and their fence placed, the rest held");
	begin(&node, GPU_OP_SET_EVENT);
	put64(ID_DEVICE);
	put64(ID_X_EVENT);
	error |= run(&node);
	ghci_service_step(&device, GHCI_WORK_RECLAIM);
	begin(&node, GPU_OP_GET_FENCE_STATUS);
	put64(ID_DEVICE);
	put64(ID_X_FENCE);
	error |= run(&node);
	expect(error == 0 && reply32(&node, 4U) == (uint32_t)VK_SUCCESS && model_count_commands(GHCI_OP_FENCE) == fences + 5U,
	       "events: the host's set places the rest, and the fence signals after it");
	node_close(&node);
	stop("events");
}

/*
 * The regions of image copies: a negative offset, an extent past the
 * image, an offset past the 16 bits of the packet, layers past the
 * image, a blit of 2^32 - 1 layers, rows past the buffer and rows
 * shorter than the extent each refuse the submission before anything is
 * placed; a framebuffer larger than its view is not made, one whose view
 * is not of its pass's format is refused at its begin; the code heap is
 * mapped executable and not writable, the data heap writable and not
 * executable.
 */
static void
scene_x_regions(void)
{
	static const int32_t offsets[4][3] = { { -1, 0, 0 }, { 0, 0, 0 }, { 70000, 0, 0 }, { 0, 0, 0 } };
	static const uint32_t extents[4][3] = { { 4U, 4U, 1U }, { 65U, 1U, 1U }, { 1U, 1U, 1U }, { 4U, 4U, 1U } };
	static const char *const names[] = {
		"regions: a negative offset is refused", "regions: an extent past the image is refused",
		"regions: an offset past 16 bits is refused", "regions: layers past the image are refused",
		"regions: a blit of 2^32 - 1 layers is refused", "regions: rows past the buffer are refused",
		"regions: rows shorter than the extent are refused"
	};
	const struct ghci_vk_pipeline *pipeline;
	const struct model_mapping *mapping;
	const struct ghci_vk_object *object;
	struct test_pass pass;
	unsigned fences;
	uint32_t session;
	struct node node;
	int index;
	int error;

	/* The device and the triangle. */
	error = x_open(&node, MODEL_FULL);
	error |= (int)x_triangle(&node, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, UINT32_MAX, ID_PIPELINE);
	expect(error == 0, "regions: objects");

	/* Each copy the checks refuse. */
	for (index = 0; index < 7; index++) {
		x_record(&node, ID_X_COMMANDS);
		if (index < 4) {
			x_command(GPU_OP_CMD_COPY_IMAGE, ID_X_COMMANDS);
			put64(ID_IMAGE);
			put32(VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
			put64(ID_X_IMAGE);
			put32(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
			put32(1U);
			put64(1U);
			put32(VK_IMAGE_ASPECT_COLOR_BIT);
			put32(0U);
			put32(index == 3 ? 1U : 0U);
			put32(index == 3 ? 2U : 1U);
			put32((uint32_t)offsets[index][0]);
			put32((uint32_t)offsets[index][1]);
			put32((uint32_t)offsets[index][2]);
			put32(VK_IMAGE_ASPECT_COLOR_BIT);
			put32(0U);
			put32(0U);
			put32(index == 3 ? 2U : 1U);
			put32(0U);
			put32(0U);
			put32(0U);
			put32(extents[index][0]);
			put32(extents[index][1]);
			put32(extents[index][2]);
		} else if (index == 4) {
			x_command(GPU_OP_CMD_BLIT_IMAGE, ID_X_COMMANDS);
			put64(ID_IMAGE);
			put32(VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
			put64(ID_X_IMAGE);
			put32(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
			put32(1U);
			put64(1U);
			put32(VK_IMAGE_ASPECT_COLOR_BIT);
			put32(0U);
			put32(0U);
			put32(0xffffffffU);
			put64(2U);
			put32(0U);
			put32(0U);
			put32(0U);
			put32(64U);
			put32(32U);
			put32(1U);
			put32(VK_IMAGE_ASPECT_COLOR_BIT);
			put32(0U);
			put32(0U);
			put32(0xffffffffU);
			put64(2U);
			put32(0U);
			put32(0U);
			put32(0U);
			put32(32U);
			put32(16U);
			put32(1U);
			put32(VK_FILTER_NEAREST);
		} else {
			x_command(GPU_OP_CMD_COPY_BUFFER_TO_IMAGE, ID_X_COMMANDS);
			put64(ID_BUFFER);
			put64(ID_IMAGE);
			put32(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
			put32(1U);
			put64(1U);
			put64(index == 5 ? 8192U - 100U : 0U);
			put32(index == 5 ? 0U : 8U);
			put32(0U);
			put32(VK_IMAGE_ASPECT_COLOR_BIT);
			put32(0U);
			put32(0U);
			put32(1U);
			put32(0U);
			put32(0U);
			put32(0U);
			put32(32U);
			put32(16U);
			put32(1U);
		}
		error = x_end(&node, ID_X_COMMANDS);
		fences = model_count_commands(GHCI_OP_FENCE);
		error |= x_submit(&node, ID_X_COMMANDS, 0U) != EINVAL;
		expect(error == 0 && model_count_commands(GHCI_OP_FENCE) == fences && model_count_commands(GHCI_OP_COPY_IMAGE) == 0U &&
		       model_count_commands(GHCI_OP_BLIT) == 0U && model_count_commands(GHCI_OP_COPY_BUFFER_TO_IMAGE) == 0U, names[index]);
	}

	/* A framebuffer wider than its view, and one of more layers, are not made. */
	expect(x_framebuffer(&node, ID_X_FRAMEBUFFER + 3U, ID_X_PASS, ID_X_VIEW, 128U, 32U, 1U) == EINVAL &&
	       x_framebuffer(&node, ID_X_FRAMEBUFFER + 3U, ID_X_PASS, ID_X_VIEW, 64U, 32U, 2U) == EINVAL,
	       "regions: a framebuffer larger than its view is not made");

	/* A pass of B8G8R8A8 begun with a framebuffer of the R8G8B8A8 view: refused. */
	memset(&pass, 0, sizeof(pass));
	pass.attachment_count = 1U;
	pass.format[0] = VK_FORMAT_B8G8R8A8_UNORM;
	pass.samples[0] = VK_SAMPLE_COUNT_1_BIT;
	pass.load[0] = VK_ATTACHMENT_LOAD_OP_CLEAR;
	pass.store[0] = VK_ATTACHMENT_STORE_OP_STORE;
	pass.subpass_count = 1U;
	pass.color_count[0] = 1U;
	pass.color[0][0] = 0U;
	pass.depth[0] = VK_ATTACHMENT_UNUSED;
	error = (int)make_render_pass(&node, ID_X_PASS + 3U, &pass);
	error |= (int)x_framebuffer(&node, ID_X_FRAMEBUFFER + 3U, ID_X_PASS + 3U, ID_X_VIEW, 64U, 32U, 1U);
	x_record(&node, ID_X_COMMANDS);
	x_begin_with(ID_X_COMMANDS, ID_X_PASS + 3U, ID_X_FRAMEBUFFER + 3U, 1U);
	x_command(GPU_OP_CMD_END_RENDER_PASS, ID_X_COMMANDS);
	error |= x_end(&node, ID_X_COMMANDS);
	fences = model_count_commands(GHCI_OP_FENCE);
	error |= x_submit(&node, ID_X_COMMANDS, 0U) != EINVAL;
	expect(error == 0 && model_count_commands(GHCI_OP_FENCE) == fences && model_count_commands(GHCI_OP_BEGIN_RENDER_PASS) == 0U,
	       "regions: a framebuffer whose view is not of its pass's format is refused at the begin");

	/* The heaps' mappings. */
	pipeline = pipeline_of(&node, ID_PIPELINE);
	session = session_of(&node)->devices->vm->session;
	mapping = pipeline != NULL ? model_lookup(session, pipeline->pso_va) : NULL;
	expect(mapping != NULL && (mapping->flags & GHCI_VM_FLAG_EXEC) != 0U && (mapping->flags & GHCI_VM_FLAG_WRITE) == 0U &&
	       (mapping->flags & GHCI_VM_FLAG_READ) != 0U, "regions: the code heap is executable and read-only to the GPU");
	begin(&node, GPU_OP_CREATE_EVENT);
	put64(ID_DEVICE);
	put64(1U);
	put32(VK_STRUCTURE_TYPE_EVENT_CREATE_INFO);
	put64(0U);
	put32(0U);
	tail(ID_X_EVENT);
	(void)run(&node);
	object = ghci_vk_object_find(session_of(&node), GHCI_VK_EVENT, ID_X_EVENT);
	mapping = object != NULL ? model_lookup(session, object->as.event.block->mapping->va) : NULL;
	expect(mapping != NULL && (mapping->flags & GHCI_VM_FLAG_EXEC) == 0U && (mapping->flags & GHCI_VM_FLAG_WRITE) != 0U,
	       "regions: the data heap is writable and not executable");
	node_close(&node);
	stop("regions");
}

/*
 * What Vulkan allows in and out of a render pass: a primary buffer that
 * ends inside one, a fill, a dispatch and a SetEvent inside one, and a
 * secondary buffer that begins or ends one each refuse the submission
 * before anything is placed.
 */
static void
scene_x_pass_rules(void)
{
	static const char *const names[] = {
		"pass rules: a primary buffer that ends inside a pass is refused", "pass rules: a fill inside a pass is refused",
		"pass rules: a dispatch inside a pass is refused", "pass rules: a SetEvent inside a pass is refused",
		"pass rules: a secondary buffer that begins a pass is refused", "pass rules: a secondary buffer that ends the pass is refused"
	};
	unsigned fences;
	struct node node;
	int index;
	int error;

	/* The device, the compute pipeline and its set, an event and a secondary buffer. */
	error = x_open(&node, MODEL_FULL);
	error |= x_compute_set(&node);
	begin(&node, GPU_OP_CREATE_EVENT);
	put64(ID_DEVICE);
	put64(1U);
	put32(VK_STRUCTURE_TYPE_EVENT_CREATE_INFO);
	put64(0U);
	put32(0U);
	tail(ID_X_EVENT);
	error |= run(&node);
	begin(&node, GPU_OP_ALLOCATE_COMMAND_BUFFERS);
	put64(ID_DEVICE);
	put64(1U);
	put32(VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO);
	put64(0U);
	put64(ID_X_POOL);
	put32(VK_COMMAND_BUFFER_LEVEL_SECONDARY);
	put32(1U);
	put64(1U);
	put64(ID_X_COMMANDS + 8U);
	error |= run(&node);
	expect(error == 0, "pass rules: objects");

	/* Each recording. */
	for (index = 0; index < 6; index++) {
		/* The secondary buffer: a pass begun and ended, or an end alone. */
		if (index >= 4) {
			x_record(&node, ID_X_COMMANDS + 8U);
			if (index == 4)
				x_begin_pass(ID_X_COMMANDS + 8U, 64U, 32U);
			x_command(GPU_OP_CMD_END_RENDER_PASS, ID_X_COMMANDS + 8U);
			error = x_end(&node, ID_X_COMMANDS + 8U);
		}

		/* The primary. */
		x_record(&node, ID_X_COMMANDS);
		if (index == 2) {
			x_command(GPU_OP_CMD_BIND_PIPELINE, ID_X_COMMANDS);
			put32(VK_PIPELINE_BIND_POINT_COMPUTE);
			put64(ID_X_COMPUTE);
			x_command(GPU_OP_CMD_BIND_DESCRIPTOR_SETS, ID_X_COMMANDS);
			put32(VK_PIPELINE_BIND_POINT_COMPUTE);
			put64(ID_LAYOUT);
			put32(0U);
			put32(1U);
			put64(1U);
			put64(ID_SET);
			put32(1U);
			put64(1U);
			put32(0U);
		}
		if (index != 4)
			x_begin_pass(ID_X_COMMANDS, 64U, 32U);
		if (index == 1) {
			x_command(GPU_OP_CMD_FILL_BUFFER, ID_X_COMMANDS);
			put64(ID_BUFFER);
			put64(0U);
			put64(64U);
			put32(0U);
		} else if (index == 2) {
			x_command(GPU_OP_CMD_DISPATCH, ID_X_COMMANDS);
			put32(1U);
			put32(1U);
			put32(1U);
		} else if (index == 3) {
			x_command(GPU_OP_CMD_SET_EVENT, ID_X_COMMANDS);
			put64(ID_X_EVENT);
			put32(VK_PIPELINE_STAGE_TRANSFER_BIT);
		} else if (index >= 4) {
			x_command(GPU_OP_CMD_EXECUTE_COMMANDS, ID_X_COMMANDS);
			put32(1U);
			put64(1U);
			put64(ID_X_COMMANDS + 8U);
		}
		if (index != 0 && index != 4)
			x_command(GPU_OP_CMD_END_RENDER_PASS, ID_X_COMMANDS);
		error |= x_end(&node, ID_X_COMMANDS);
		fences = model_count_commands(GHCI_OP_FENCE);
		error |= x_submit(&node, ID_X_COMMANDS, 0U) != EINVAL;
		expect(error == 0 && model_count_commands(GHCI_OP_FENCE) == fences && model_count_commands(GHCI_OP_FILL_BUFFER) == 0U &&
		       model_count_commands(GHCI_OP_DISPATCH) == 0U, names[index]);
	}
	node_close(&node);
	stop("pass rules");
}

/*
 * A framebuffer of two layers: the pass's clear of layer 1 is a pass of
 * its own before the subpass (layer 0 is the subpass's); a
 * vkCmdClearAttachments of layer 1 ends the subpass, clears layer 1, and
 * begins the subpass again loading layer 0.  A clear of attachments in
 * a subpass whose attachment is not stored makes the first part store
 * it (the subpass goes on) and the last part keep DONT_CARE.
 */
static void
scene_x_layers(void)
{
	static const uint32_t expected[] = {
		GHCI_OP_BIND_SESSION, GHCI_OP_BARRIER, GHCI_OP_BEGIN_RENDER_PASS, GHCI_OP_END_RENDER_PASS, GHCI_OP_BEGIN_RENDER_PASS,
		GHCI_OP_END_RENDER_PASS, GHCI_OP_BEGIN_RENDER_PASS, GHCI_OP_END_RENDER_PASS, GHCI_OP_BEGIN_RENDER_PASS,
		GHCI_OP_END_RENDER_PASS, GHCI_OP_BARRIER, GHCI_OP_FENCE, GHCI_OP_BIND_SESSION
	};
	const struct model_command *command;
	const uint8_t *desc;
	const uint8_t *table;
	struct test_pass pass;
	uint32_t ops[X_OPS_MAX];
	uint64_t layer0;
	unsigned first;
	unsigned count;
	unsigned mark;
	struct node node;
	int error;

	/* A 64 x 32 image of two layers at 0x70000 and its 2D array view. */
	error = x_open(&node, MODEL_FULL);
	begin(&node, GPU_OP_CREATE_IMAGE);
	put64(ID_DEVICE);
	put64(1U);
	put32(VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO);
	put64(0U);
	put32(0U);
	put32(VK_IMAGE_TYPE_2D);
	put32(VK_FORMAT_R8G8B8A8_UNORM);
	put32(64U);
	put32(32U);
	put32(1U);
	put32(1U);
	put32(2U);
	put32(VK_SAMPLE_COUNT_1_BIT);
	put32(VK_IMAGE_TILING_OPTIMAL);
	put32(VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
	put32(VK_SHARING_MODE_EXCLUSIVE);
	put32(0U);
	put64(0U);
	put32(VK_IMAGE_LAYOUT_UNDEFINED);
	tail(ID_X_IMAGE + 5U);
	error |= run(&node);
	error |= (int)reply32(&node, 4U);
	begin(&node, GPU_OP_BIND_IMAGE_MEMORY);
	put64(ID_DEVICE);
	put64(ID_X_IMAGE + 5U);
	put64(ID_MEMORY);
	put64(0x70000U);
	error |= run(&node);
	error |= (int)reply32(&node, 4U);
	begin(&node, GPU_OP_CREATE_IMAGE_VIEW);
	put64(ID_DEVICE);
	put64(1U);
	put32(VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO);
	put64(0U);
	put32(0U);
	put64(ID_X_IMAGE + 5U);
	put32(VK_IMAGE_VIEW_TYPE_2D_ARRAY);
	put32(VK_FORMAT_R8G8B8A8_UNORM);
	put32(VK_COMPONENT_SWIZZLE_IDENTITY);
	put32(VK_COMPONENT_SWIZZLE_IDENTITY);
	put32(VK_COMPONENT_SWIZZLE_IDENTITY);
	put32(VK_COMPONENT_SWIZZLE_IDENTITY);
	put32(VK_IMAGE_ASPECT_COLOR_BIT);
	put32(0U);
	put32(1U);
	put32(0U);
	put32(2U);
	tail(ID_X_VIEW + 5U);
	error |= run(&node);
	error |= (int)reply32(&node, 4U);
	error |= (int)x_framebuffer(&node, ID_X_FRAMEBUFFER + 5U, ID_X_PASS, ID_X_VIEW + 5U, 64U, 32U, 2U);
	expect(error == 0, "layers: objects");
	layer0 = x_va(&node, 0x70000U);

	/* The pass begun, layer 1 cleared by vkCmdClearAttachments, the end. */
	x_record(&node, ID_X_COMMANDS);
	x_begin_with(ID_X_COMMANDS, ID_X_PASS, ID_X_FRAMEBUFFER + 5U, 1U);
	x_clear_color0(ID_X_COMMANDS, 1U, 1U);
	x_command(GPU_OP_CMD_END_RENDER_PASS, ID_X_COMMANDS);
	error = x_end(&node, ID_X_COMMANDS);
	mark = model.command_count;
	error |= x_submit(&node, ID_X_COMMANDS, 0U);
	count = x_ops(mark, &first, ops);
	expect(error == 0 && x_same(ops, count, expected, sizeof(expected) / sizeof(expected[0])),
	       "layers: layer 1's clear, the subpass, the clear of layer 1 between its two parts");
	if (count == sizeof(expected) / sizeof(expected[0])) {
		/* Layer 1 cleared to the pass's value before the subpass, over the render area. */
		command = &model.commands[first + 2U];
		desc = x_gpu(command->session, command->words[1] | ((uint64_t)command->words[2] << 32));
		table = desc != NULL ? x_gpu(command->session, x_load(desc + 0xa8U, 8U)) : NULL;
		expect(desc != NULL && table != NULL && x_load(desc, 8U) == layer0 + 8192U && desc[0x0dU] == 1U && command->words[8] == 1U &&
		       command->words[4] == (64U | (32U << 16)) && x_load(table, 4U) == 0U && x_load(table + 12U, 4U) == 0x3f800000U,
		       "layers: layer 1 (8192 bytes on) cleared to (0, 0, 0, 1) by a pass of its own");

		/* The subpass on layer 0, stored at its end in the middle. */
		command = &model.commands[first + 4U];
		desc = x_gpu(command->session, command->words[1] | ((uint64_t)command->words[2] << 32));
		expect(desc != NULL && x_load(desc, 8U) == layer0 && desc[0x0dU] == 1U, "layers: the subpass draws layer 0, CLEAR | STORE");

		/* The clear of layer 1 with the attachments' value. */
		command = &model.commands[first + 6U];
		desc = x_gpu(command->session, command->words[1] | ((uint64_t)command->words[2] << 32));
		table = desc != NULL ? x_gpu(command->session, x_load(desc + 0xa8U, 8U)) : NULL;
		expect(desc != NULL && table != NULL && x_load(desc, 8U) == layer0 + 8192U && x_load(table + 4U, 4U) == 0x3f800000U,
		       "layers: vkCmdClearAttachments clears layer 1 to (0, 1, 0, 1)");

		/* The subpass again, loading layer 0. */
		command = &model.commands[first + 8U];
		desc = x_gpu(command->session, command->words[1] | ((uint64_t)command->words[2] << 32));
		expect(desc != NULL && command->words[8] == 0U && x_load(desc, 8U) == layer0 && desc[0x0dU] == 0U,
		       "layers: the subpass begun again loads layer 0 (LOAD | STORE)");
	}

	/* A pass that does not store its colour: the clear's first part stores it, the last keeps DONT_CARE. */
	memset(&pass, 0, sizeof(pass));
	pass.attachment_count = 1U;
	pass.format[0] = VK_FORMAT_R8G8B8A8_UNORM;
	pass.samples[0] = VK_SAMPLE_COUNT_1_BIT;
	pass.load[0] = VK_ATTACHMENT_LOAD_OP_CLEAR;
	pass.store[0] = VK_ATTACHMENT_STORE_OP_DONT_CARE;
	pass.subpass_count = 1U;
	pass.color_count[0] = 1U;
	pass.color[0][0] = 0U;
	pass.depth[0] = VK_ATTACHMENT_UNUSED;
	error = (int)make_render_pass(&node, ID_X_PASS + 4U, &pass);
	error |= (int)x_framebuffer(&node, ID_X_FRAMEBUFFER + 4U, ID_X_PASS + 4U, ID_X_VIEW, 64U, 32U, 1U);
	x_record(&node, ID_X_COMMANDS);
	x_begin_with(ID_X_COMMANDS, ID_X_PASS + 4U, ID_X_FRAMEBUFFER + 4U, 1U);
	x_clear_color0(ID_X_COMMANDS, 0U, 1U);
	x_command(GPU_OP_CMD_END_RENDER_PASS, ID_X_COMMANDS);
	error |= x_end(&node, ID_X_COMMANDS);
	mark = model.command_count;
	error |= x_submit(&node, ID_X_COMMANDS, 0U);
	count = x_ops(mark, &first, ops);
	expect(error == 0 && count == 9U && ops[2] == GHCI_OP_BEGIN_RENDER_PASS && ops[4] == GHCI_OP_BEGIN_RENDER_PASS,
	       "layers: the clear of a DONT_CARE attachment ends the subpass in its middle");
	if (count == 9U) {
		command = &model.commands[first + 2U];
		desc = x_gpu(command->session, command->words[1] | ((uint64_t)command->words[2] << 32));
		expect(desc != NULL && desc[0x0dU] == 1U, "layers: the first part stores (CLEAR | STORE, not DONT_CARE)");
		command = &model.commands[first + 4U];
		desc = x_gpu(command->session, command->words[1] | ((uint64_t)command->words[2] << 32));
		expect(desc != NULL && desc[0x0dU] == (1U | (1U << 2)) && command->words[8] == 1U,
		       "layers: the last part clears and keeps DONT_CARE");
	}
	node_close(&node);
	stop("layers");
}

/* The sleep hook of the partial scene: a fault of the processor at the first wait for the ring's room. */
static void
x_fault_hook(
	unsigned sleep,
	void *argument)
{
	int *injected;

	(void)sleep;
	injected = argument;
	if (*injected)
		return;
	*injected = 1;
	model_inject(GHCI_FAULT_KIND_AXI_ERROR, GHCI_UNIT_CP, 0U, 0x2000U, 0);
}

/*
 * A submission a recovery cuts after part of it is in the ring (a 4 KiB
 * ring the held processor does not read, and a fault while the driver
 * waits for room): the reply is VK_ERROR_DEVICE_LOST, the device's last
 * ticket is the part's, pending, its temporary memory waits on it, and
 * the reset ends it and gives the memory back.
 */
static void
scene_x_partial(void)
{
	struct ghci_vk_device *record;
	struct ghci_ticket ticket;
	enum ghci_ticket_state state;
	struct node node;
	int injected;
	int index;
	int error;

	/* A 4 KiB ring, the triangle, 400 draws. */
	test_ring_bytes = 4096U;
	error = x_open(&node, MODEL_FULL);
	test_ring_bytes = 0U;
	error |= (int)x_triangle(&node, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, UINT32_MAX, ID_PIPELINE);
	record = session_of(&node)->devices;
	expect(error == 0 && record != NULL, "partial: objects");
	if (record == NULL) {
		node_close(&node);
		stop("partial");
		return;
	}
	x_record(&node, ID_X_COMMANDS);
	x_begin_pass(ID_X_COMMANDS, 64U, 32U);
	x_bind_triangle(ID_X_COMMANDS, ID_PIPELINE, 0U);
	for (index = 0; index < 400; index++) {
		x_command(GPU_OP_CMD_DRAW, ID_X_COMMANDS);
		put32(3U);
		put32(1U);
		put32((uint32_t)index);
		put32(0U);
	}
	x_command(GPU_OP_CMD_END_RENDER_PASS, ID_X_COMMANDS);
	error = x_end(&node, ID_X_COMMANDS);

	/* The processor held; the fault comes while the driver waits for room. */
	model_hold(1);
	injected = 0;
	stub_set_sleep_hook(x_fault_hook, &injected);
	error |= x_submit(&node, ID_X_COMMANDS, 0U);
	stub_set_sleep_hook(NULL, NULL);
	ticket = record->last;
	state = ghci_cp_ticket(&device, &ticket);
	expect(error == 0 && injected && reply32(&node, 4U) == (uint32_t)VK_ERROR_DEVICE_LOST && record->submitted &&
	       ticket.generation == device.generation && state == GHCI_TICKET_PENDING && record->retired != NULL,
	       "partial: DEVICE_LOST; the part's ticket is the device's last, pending, and the memory waits on it");

	/* The recovery ends the ticket; the memory goes back. */
	model_hold(0);
	ghci_service_step(&device, GHCI_WORK_RECOVER);
	state = ghci_cp_ticket(&device, &ticket);
	ghci_vk_heap_reclaim(session_of(&node), record);
	expect(state == GHCI_TICKET_FAILED && record->retired == NULL, "partial: the reset ends the ticket and the memory goes back");
	node_close(&node);
	stop("partial");
}

/* Appends vkCmdWaitEvents of one event, from the host's stage to the transfer's, with no barrier. */
static void
x_wait(
	uint64_t commands,
	uint64_t event)
{
	x_command(GPU_OP_CMD_WAIT_EVENTS, commands);
	put32(1U);
	put64(1U);
	put64(event);
	put32(VK_PIPELINE_STAGE_HOST_BIT);
	put32(VK_PIPELINE_STAGE_TRANSFER_BIT);
	put32(0U);
	put64(0U);
	put32(0U);
	put64(0U);
	put32(0U);
	put64(0U);
}

/* Appends vkCmdFillBuffer of the buffer's first word with a value. */
static void
x_fill(
	uint64_t commands,
	uint32_t value)
{
	x_command(GPU_OP_CMD_FILL_BUFFER, commands);
	put64(ID_BUFFER);
	put64(0U);
	put64(4U);
	put32(value);
}

/* Runs vkSetEvent, vkResetEvent or vkGetEventStatus on the host.  Returns what the frame returned. */
static int
x_host_event(
	struct node *node,
	uint32_t opcode,
	uint64_t event)
{
	begin(node, opcode);
	put64(ID_DEVICE);
	put64(event);
	return run(node);
}

/* Reports vkGetFenceStatus of a fence (UINT32_MAX when the frame failed). */
static uint32_t
x_fence_status(
	struct node *node,
	uint64_t fence)
{
	begin(node, GPU_OP_GET_FENCE_STATUS);
	put64(ID_DEVICE);
	put64(fence);
	if (run(node) != 0)
		return UINT32_MAX;
	return reply32(node, 4U);
}

/* Creates a fence or an event (flags 0).  Returns 0, or 1. */
static int
x_create(
	struct node *node,
	uint32_t opcode,
	uint32_t type,
	uint64_t identity)
{
	begin(node, opcode);
	put64(ID_DEVICE);
	put64(1U);
	put32(type);
	put64(0U);
	put32(0U);
	tail(identity);
	if (run(node) != 0 || reply32(node, 4U) != 0U)
		return 1;
	return 0;
}

/* Collects the values of the fills of the buffer's first word the model ran after mark.  Returns the count. */
static unsigned
x_fills(
	unsigned mark,
	uint32_t *values)
{
	unsigned index;
	unsigned count;

	count = 0U;
	for (index = mark; index < model.command_count && count < X_FILLS_MAX; index++) {
		if (model.commands[index].opcode != GHCI_OP_FILL_BUFFER || model.commands[index].words[3] != 4U)
			continue;
		values[count] = model.commands[index].words[4];
		count++;
	}
	return count;
}

/*
 * Opens a second node session on the started device with a VkDevice, its
 * queue on timeline 1, a command pool with one primary command buffer,
 * a fence and an event (the same identities as the first session's).
 * Returns 0, or 1.
 */
static int
x_other_open(
	struct node *node)
{
	int error;

	/* The session and vkCreateDevice: one queue of family 0, no feature. */
	error = node_open(node);
	if (error != 0)
		return 1;
	begin(node, GPU_OP_CREATE_DEVICE);
	put64(ID_PHYSICAL);
	put64(1U);
	put32(VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO);
	put64(0U);
	put32(0U);
	put32(1U);
	put64(1U);
	put32(VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO);
	put64(0U);
	put32(0U);
	put32(0U);
	put32(1U);
	put64(1U);
	put32(0x3f800000U);
	put32(0U);
	put64(0U);
	put32(0U);
	put64(0U);
	put64(0U);
	tail(ID_DEVICE);
	if (run(node) != 0 || reply32(node, 4U) != 0U)
		return 1;

	/* The queue on timeline 1, the pool and its buffer. */
	begin(node, GPU_OP_GET_DEVICE_QUEUE2);
	put64(ID_DEVICE);
	put64(1U);
	put32(VK_STRUCTURE_TYPE_DEVICE_QUEUE_INFO_2);
	put64(1U);
	put32(GPU_OP_RECORD_QUEUE_TIMELINE);
	put64(0U);
	put32(1U);
	put32(0U);
	put32(0U);
	put32(0U);
	put64(1U);
	put64(ID_X_QUEUE);
	if (run(node) != 0)
		return 1;
	begin(node, GPU_OP_CREATE_COMMAND_POOL);
	put64(ID_DEVICE);
	put64(1U);
	put32(VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO);
	put64(0U);
	put32(0U);
	put32(0U);
	tail(ID_X_POOL);
	if (run(node) != 0 || reply32(node, 4U) != 0U)
		return 1;
	begin(node, GPU_OP_ALLOCATE_COMMAND_BUFFERS);
	put64(ID_DEVICE);
	put64(1U);
	put32(VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO);
	put64(0U);
	put64(ID_X_POOL);
	put32(VK_COMMAND_BUFFER_LEVEL_PRIMARY);
	put32(1U);
	put64(1U);
	put64(ID_X_COMMANDS);
	if (run(node) != 0 || reply32(node, 4U) != 0U)
		return 1;

	/* The fence and the event. */
	if (x_create(node, GPU_OP_CREATE_FENCE, VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, ID_X_FENCE) != 0)
		return 1;
	return x_create(node, GPU_OP_CREATE_EVENT, VK_STRUCTURE_TYPE_EVENT_CREATE_INFO, ID_X_EVENT);
}

/*
 * The hold: a submission that waits for an event the host has not set
 * places nothing after the wait and holds the device's later submission
 * too, with no WAIT_FENCE in the ring; their fences, a job marker and a
 * context fence frame behind them wait; the host's reset keeps it
 * waiting, and the host's set places both, in order, after which the
 * fences, the marker and the frame complete.  An earlier submission's
 * SetEvent that the GPU has not run yet holds the wait until the service
 * thread sees it run.  Another session's work runs meanwhile, and the
 * close of the session ends what it holds with an error.
 */
static void
scene_x_hold(void)
{
	struct drv_gpu_completion *completion[3];
	static char completions[3];
	uint32_t values[X_FILLS_MAX];
	struct node other;
	struct node node;
	unsigned completed;
	unsigned waits;
	unsigned mark;
	unsigned count;
	void *token;
	int error;

	/* The device, an event, two more fences. */
	for (count = 0U; count < 3U; count++)
		completion[count] = (struct drv_gpu_completion *)(void *)&completions[count];
	error = x_open(&node, MODEL_FULL);
	error |= x_create(&node, GPU_OP_CREATE_EVENT, VK_STRUCTURE_TYPE_EVENT_CREATE_INFO, ID_X_EVENT);
	error |= x_create(&node, GPU_OP_CREATE_FENCE, VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, ID_X_FENCE + 1U);
	expect(error == 0, "hold: objects");

	/* A wait and a fill, then a fill alone, each with a fence; a marker and a frame behind them. */
	x_record(&node, ID_X_COMMANDS);
	x_wait(ID_X_COMMANDS, ID_X_EVENT);
	x_fill(ID_X_COMMANDS, 0x101U);
	error = x_end(&node, ID_X_COMMANDS);
	x_record(&node, ID_X_COMMANDS + 1U);
	x_fill(ID_X_COMMANDS + 1U, 0x102U);
	error |= x_end(&node, ID_X_COMMANDS + 1U);
	mark = model.command_count;
	waits = model_count_commands(GHCI_OP_WAIT_FENCE);
	completed = stub.completions;
	error |= x_submit(&node, ID_X_COMMANDS, ID_X_FENCE);
	expect(error == 0 && reply32(&node, 4U) == (uint32_t)VK_SUCCESS, "hold: the submission that waits is taken");
	error = device.ops.jobs->reserve(&device, node.session, 1U, completion[0], &token);
	error |= device.ops.jobs->commit(&device, node.session, token, completion[0]);
	error |= x_submit(&node, ID_X_COMMANDS + 1U, ID_X_FENCE + 1U);
	error |= device.ops.commands->submit(&device, node.session, NULL, 0U, GPU_COMMAND_CONTEXT_FENCE, 1U, completion[1]);
	ghci_service_step(&device, GHCI_WORK_RECLAIM);
	ghci_service_step(&device, GHCI_WORK_RECLAIM);
	expect(error == 0 && x_fills(mark, values) == 0U && x_fence_status(&node, ID_X_FENCE) == (uint32_t)VK_NOT_READY &&
	       x_fence_status(&node, ID_X_FENCE + 1U) == (uint32_t)VK_NOT_READY && stub.completions == completed &&
	       model_count_commands(GHCI_OP_WAIT_FENCE) == waits && model.waiting_fence == 0,
	       "hold: no fill, no WAIT_FENCE; the fences, the marker and the frame wait");
	expect(device.ops.recovery->stop_poll(&device, node.session) == EAGAIN, "hold: the stop poll waits for held work");

	/* The host's reset before any set: still held. */
	error = x_host_event(&node, GPU_OP_RESET_EVENT, ID_X_EVENT);
	ghci_service_step(&device, GHCI_WORK_RECLAIM);
	expect(error == 0 && x_fills(mark, values) == 0U && x_fence_status(&node, ID_X_FENCE) == (uint32_t)VK_NOT_READY,
	       "hold: the host's reset before the set keeps it waiting");

	/* The host's set: both placed in order; then the fences, the marker and the frame. */
	error = x_host_event(&node, GPU_OP_SET_EVENT, ID_X_EVENT);
	count = x_fills(mark, values);
	expect(error == 0 && count == 2U && values[0] == 0x101U && values[1] == 0x102U && model.waiting_fence == 0,
	       "hold: the host's set places the held fill, then the later submission's");
	ghci_service_step(&device, GHCI_WORK_RECLAIM);
	expect(x_fence_status(&node, ID_X_FENCE) == (uint32_t)VK_SUCCESS && x_fence_status(&node, ID_X_FENCE + 1U) == (uint32_t)VK_SUCCESS &&
	       stub.completions == completed + 2U && stub.last_completion_error == 0,
	       "hold: the fences, the marker and the frame complete after the work ran");

	/* An earlier submission's set the GPU has not run: the wait is held until the service sees it run. */
	error = x_host_event(&node, GPU_OP_RESET_EVENT, ID_X_EVENT);
	x_record(&node, ID_X_COMMANDS + 2U);
	x_command(GPU_OP_CMD_SET_EVENT, ID_X_COMMANDS + 2U);
	put64(ID_X_EVENT);
	put32(VK_PIPELINE_STAGE_TRANSFER_BIT);
	error |= x_end(&node, ID_X_COMMANDS + 2U);
	x_record(&node, ID_X_COMMANDS);
	x_wait(ID_X_COMMANDS, ID_X_EVENT);
	x_fill(ID_X_COMMANDS, 0x201U);
	error |= x_end(&node, ID_X_COMMANDS);
	mark = model.command_count;
	model_hold(1);
	error |= x_submit(&node, ID_X_COMMANDS + 2U, 0U);
	error |= x_submit(&node, ID_X_COMMANDS, ID_X_FENCE);
	ghci_service_step(&device, GHCI_WORK_RECLAIM);
	expect(error == 0 && x_fills(mark, values) == 0U && x_fence_status(&node, ID_X_FENCE) == (uint32_t)VK_NOT_READY,
	       "hold: a wait for an earlier submission's set the GPU has not run is held");
	model_hold(0);
	model_tick();
	ghci_service_step(&device, GHCI_WORK_RECLAIM);
	count = x_fills(mark, values);
	ghci_service_step(&device, GHCI_WORK_RECLAIM);
	expect(count == 1U && values[0] == 0x201U && x_fence_status(&node, ID_X_FENCE) == (uint32_t)VK_SUCCESS &&
	       model.waiting_fence == 0, "hold: the service places the rest once the earlier set ran; its fence signals");

	/* Another session's work runs while this one holds. */
	error = x_host_event(&node, GPU_OP_RESET_EVENT, ID_X_EVENT);
	x_record(&node, ID_X_COMMANDS);
	x_wait(ID_X_COMMANDS, ID_X_EVENT);
	x_fill(ID_X_COMMANDS, 0x301U);
	error |= x_end(&node, ID_X_COMMANDS);
	mark = model.command_count;
	error |= x_submit(&node, ID_X_COMMANDS, ID_X_FENCE);
	error |= x_other_open(&other);
	x_record(&other, ID_X_COMMANDS);
	x_command(GPU_OP_CMD_SET_EVENT, ID_X_COMMANDS);
	put64(ID_X_EVENT);
	put32(VK_PIPELINE_STAGE_TRANSFER_BIT);
	error |= x_end(&other, ID_X_COMMANDS);
	error |= x_submit(&other, ID_X_COMMANDS, ID_X_FENCE);
	ghci_service_step(&device, GHCI_WORK_RECLAIM);
	expect(error == 0 && x_fence_status(&other, ID_X_FENCE) == (uint32_t)VK_SUCCESS && x_fills(mark, values) == 0U &&
	       x_fence_status(&node, ID_X_FENCE) == (uint32_t)VK_NOT_READY && model.waiting_fence == 0,
	       "hold: another session's submission runs while one is held; the ring does not wait");

	/* The close ends the held work and the marker behind it with an error. */
	completed = stub.completions;
	error = device.ops.jobs->reserve(&device, node.session, 1U, completion[2], &token);
	error |= device.ops.jobs->commit(&device, node.session, token, completion[2]);
	ghci_service_step(&device, GHCI_WORK_RECLAIM);
	expect(error == 0 && stub.completions == completed, "hold: a marker behind the held work waits");
	node_close(&node);
	expect(stub.completions == completed + 1U && stub.last_completion_error == EIO && x_fills(mark, values) == 0U,
	       "hold: the close ends the held work and its marker with an error");
	node_close(&other);
	stop("hold");
}

/*
 * The failures of held work: a reset that ends the device's placed work
 * (its GHCI session dies) fails the held submission behind it: its fence
 * is lost and its marker ends with an error, and nothing of it runs; the
 * isolation of the session fails what it holds.
 */
static void
scene_x_hold_fail(void)
{
	struct drv_gpu_completion *completion[2];
	static char completions[2];
	uint32_t values[X_FILLS_MAX];
	struct node node;
	unsigned completed;
	unsigned mark;
	void *token;
	int error;

	/* The device and an event. */
	completion[0] = (struct drv_gpu_completion *)(void *)&completions[0];
	completion[1] = (struct drv_gpu_completion *)(void *)&completions[1];
	error = x_open(&node, MODEL_FULL);
	error |= x_create(&node, GPU_OP_CREATE_EVENT, VK_STRUCTURE_TYPE_EVENT_CREATE_INFO, ID_X_EVENT);
	expect(error == 0, "hold fail: objects");

	/* Placed work the processor holds, then held work with its fence and a marker. */
	x_record(&node, ID_X_COMMANDS);
	x_fill(ID_X_COMMANDS, 0x401U);
	error = x_end(&node, ID_X_COMMANDS);
	x_record(&node, ID_X_COMMANDS + 1U);
	x_wait(ID_X_COMMANDS + 1U, ID_X_EVENT);
	x_fill(ID_X_COMMANDS + 1U, 0x402U);
	error |= x_end(&node, ID_X_COMMANDS + 1U);
	mark = model.command_count;
	completed = stub.completions;
	model_hold(1);
	error |= x_submit(&node, ID_X_COMMANDS, 0U);
	error |= x_submit(&node, ID_X_COMMANDS + 1U, ID_X_FENCE);
	error |= device.ops.jobs->reserve(&device, node.session, 1U, completion[0], &token);
	error |= device.ops.jobs->commit(&device, node.session, token, completion[0]);
	expect(error == 0 && stub.completions == completed, "hold fail: held behind placed work");

	/* A fault in the placed work; the reset ends it, and the held work fails. */
	model_inject(GHCI_FAULT_KIND_AXI_ERROR, GHCI_UNIT_CP, 0U, 0x2000U, 0);
	model_hold(0);
	ghci_service_step(&device, GHCI_WORK_RECOVER);
	ghci_service_step(&device, GHCI_WORK_RECLAIM);
	expect(stub.completions == completed + 1U && stub.last_completion_error == EIO &&
	       x_fence_status(&node, ID_X_FENCE) == (uint32_t)VK_ERROR_DEVICE_LOST && x_fills(mark, values) == 0U,
	       "hold fail: a reset that ends the placed work fails the held work: its fence is lost, its marker EIO");
	node_close(&node);
	stop("hold fail");

	/* The isolation of the session fails what it holds. */
	error = x_open(&node, MODEL_FULL);
	error |= x_create(&node, GPU_OP_CREATE_EVENT, VK_STRUCTURE_TYPE_EVENT_CREATE_INFO, ID_X_EVENT);
	x_record(&node, ID_X_COMMANDS);
	x_wait(ID_X_COMMANDS, ID_X_EVENT);
	x_fill(ID_X_COMMANDS, 0x403U);
	error |= x_end(&node, ID_X_COMMANDS);
	error |= x_submit(&node, ID_X_COMMANDS, ID_X_FENCE);
	completed = stub.completions;
	error |= device.ops.jobs->reserve(&device, node.session, 1U, completion[1], &token);
	error |= device.ops.jobs->commit(&device, node.session, token, completion[1]);
	error |= device.ops.recovery->isolate(&device, node.session);
	ghci_service_step(&device, GHCI_WORK_RECLAIM);
	expect(error == 0 && stub.completions == completed + 1U && stub.last_completion_error == EIO &&
	       session_of(&node)->hold_count == 0U, "hold fail: the isolation fails the held work and its marker");
	node_close(&node);
	stop("hold fail isolated");
}

/*
 * A held submission larger than half of a 4 KiB ring, which an earlier
 * submission's set on the GPU lets go: the service thread places it in
 * pieces the ring takes at once (each with its fence), in order, and its
 * fence signals after the last.  The pieces by hand: the ring is 1024
 * dwords, so a plain cut comes every 128 dwords or more (an eighth) and
 * the service places at most 512 - 18 = 494 dwords of packets at once.
 * The held submission is the head barrier (dwords 0-3), the wait's cut at
 * 4 and its barrier (4-7), 400 fills of 5 dwords from 8 (fill i at
 * 8 + 5i) and the tail barrier (2008-2011).  Its plain cuts are before
 * fill 25 (dword 133, 129 past the wait's cut) and every 26 fills after
 * (130 dwords): 133, 263, ..., 1953 (15 cuts).  The client places the
 * head (0-3) at the submission, since the set has not run; the service
 * then places 4-392 (fills 0-76: the cut at 523 is 519 past 4), 393-782
 * (fills 77-154), 783-1172 (155-232), 1173-1562 (233-310) and 1563-2011
 * (311-399, with the tail: 449 dwords).  With the set's own submission,
 * 7 fences with the interrupt.
 */
static void
scene_x_hold_pieces(void)
{
	uint32_t values[X_FILLS_MAX];
	unsigned fills[8];
	struct node node;
	unsigned rounds;
	unsigned count;
	unsigned mark;
	unsigned index;
	unsigned heads;
	unsigned tails;
	unsigned pieces;
	int held;
	int error;

	/* A 4 KiB ring and an event. */
	test_ring_bytes = 4096U;
	error = x_open(&node, MODEL_FULL);
	test_ring_bytes = 0U;
	error |= x_create(&node, GPU_OP_CREATE_EVENT, VK_STRUCTURE_TYPE_EVENT_CREATE_INFO, ID_X_EVENT);
	expect(error == 0, "hold pieces: objects");

	/* The set, then the wait and 400 fills (2000 dwords). */
	x_record(&node, ID_X_COMMANDS);
	x_command(GPU_OP_CMD_SET_EVENT, ID_X_COMMANDS);
	put64(ID_X_EVENT);
	put32(VK_PIPELINE_STAGE_TRANSFER_BIT);
	error = x_end(&node, ID_X_COMMANDS);
	x_record(&node, ID_X_COMMANDS + 1U);
	x_wait(ID_X_COMMANDS + 1U, ID_X_EVENT);
	for (index = 0U; index < 400U; index++)
		x_fill(ID_X_COMMANDS + 1U, 0x1000U + index);
	error |= x_end(&node, ID_X_COMMANDS + 1U);
	mark = model.command_count;
	model_hold(1);
	error |= x_submit(&node, ID_X_COMMANDS, 0U);
	error |= x_submit(&node, ID_X_COMMANDS + 1U, ID_X_FENCE);
	expect(error == 0 && x_fills(mark, values) == 0U, "hold pieces: held behind the set");

	/* The set runs; the service places the rest in pieces. */
	model_hold(0);
	model_tick();
	for (rounds = 0U; rounds < 16U && x_fills(mark, values) < 400U; rounds++) {
		model_tick();
		ghci_service_step(&device, GHCI_WORK_RECLAIM);
	}
	count = x_fills(mark, values);
	held = count == 400U;
	for (index = 0U; index < count && held; index++) {
		if (values[index] != 0x1000U + index)
			held = 0;
	}
	ghci_service_step(&device, GHCI_WORK_RECLAIM);
	expect(held && x_fence_status(&node, ID_X_FENCE) == (uint32_t)VK_SUCCESS && model.waiting_fence == 0,
	       "hold pieces: 400 fills in order; the fence signals after the last");
	pieces = x_piece_fills(mark, fills, 8U);
	expect(pieces == 7U && fills[0] == 0U && fills[1] == 0U && fills[2] == 77U && fills[3] == 78U && fills[4] == 78U &&
	       fills[5] == 78U && fills[6] == 89U,
	       "hold pieces: 7 fences: the set, the head, then 77, 78, 78, 78 and 89 fills");

	/* Each piece shows the GPU the host's writes at its head and puts the GPU's in memory at its tail. */
	heads = 0U;
	tails = 0U;
	for (index = mark; index < model.command_count; index++) {
		if (model.commands[index].opcode != GHCI_OP_BARRIER || model.commands[index].words[1] != 0U)
			continue;
		if (model.commands[index].words[3] == GHCI_VK_HEAD_FLAGS)
			heads++;
		if (model.commands[index].words[3] == GHCI_VK_TAIL_FLAGS)
			tails++;
	}
	expect(heads == 7U && tails == 7U, "hold pieces: 7 head and 7 tail barriers: the set's, and one of each around each piece");

	/* A wait inside a render pass holds the submission from where the pass begins, never inside it. */
	error = x_host_event(&node, GPU_OP_RESET_EVENT, ID_X_EVENT);
	x_record(&node, ID_X_COMMANDS);
	x_fill(ID_X_COMMANDS, 0x501U);
	x_begin_pass(ID_X_COMMANDS, 64U, 32U);
	x_wait(ID_X_COMMANDS, ID_X_EVENT);
	x_command(GPU_OP_CMD_END_RENDER_PASS, ID_X_COMMANDS);
	error |= x_end(&node, ID_X_COMMANDS);
	mark = model.command_count;
	error |= x_submit(&node, ID_X_COMMANDS, ID_X_FENCE);
	ghci_service_step(&device, GHCI_WORK_RECLAIM);
	count = x_fills(mark, values);
	expect(error == 0 && count == 1U && values[0] == 0x501U && model_count_commands(GHCI_OP_BEGIN_RENDER_PASS) == 0U &&
	       x_fence_status(&node, ID_X_FENCE) == (uint32_t)VK_NOT_READY,
	       "hold pieces: a wait inside a render pass: the fill before the pass placed, the pass held");
	error = x_host_event(&node, GPU_OP_SET_EVENT, ID_X_EVENT);
	ghci_service_step(&device, GHCI_WORK_RECLAIM);
	expect(error == 0 && model_count_commands(GHCI_OP_BEGIN_RENDER_PASS) == 1U && x_fence_status(&node, ID_X_FENCE) == (uint32_t)VK_SUCCESS,
	       "hold pieces: the host's set places the pass whole");
	node_close(&node);
	stop("hold pieces");
}

/*
 * Counts the fills of the buffer's first word in each piece of a client's
 * session the model ran after mark, a piece ending at each fence with the
 * interrupt in a client's session (the driver's own, of session 0, are
 * not counted): fills (at most capacity of them) gets each piece's count.
 * Returns the number of pieces.
 */
static unsigned
x_piece_fills(
	unsigned mark,
	unsigned *fills,
	unsigned capacity)
{
	const struct model_command *command;
	unsigned index;
	unsigned count;
	unsigned in_piece;

	count = 0U;
	in_piece = 0U;
	for (index = mark; index < model.command_count; index++) {
		command = &model.commands[index];
		if (command->opcode == GHCI_OP_FILL_BUFFER && command->words[3] == 4U)
			in_piece++;
		if (command->opcode != GHCI_OP_FENCE || (command->words[5] & GHCI_FENCE_FLAG_IRQ) == 0U || command->session == 0U)
			continue;
		if (count < capacity)
			fills[count] = in_piece;
		count++;
		in_piece = 0U;
	}
	return count;
}

/* Reports the record of the nth (from 0) packet of an opcode the model ran after mark, or UINT32_MAX. */
static unsigned
x_find(
	unsigned mark,
	uint32_t opcode,
	unsigned nth)
{
	unsigned index;

	for (index = mark; index < model.command_count; index++) {
		if (model.commands[index].opcode != opcode)
			continue;
		if (nth == 0U)
			return index;
		nth--;
	}
	return UINT32_MAX;
}

/* Appends vkCmdDispatch of 2 x 1 x 1. */
static void
x_dispatch(
	uint64_t commands)
{
	x_command(GPU_OP_CMD_DISPATCH, commands);
	put32(2U);
	put32(1U);
	put32(1U);
}

/* Appends the binds of dyn.comp, its set (dynamic offset 256) and push constants (0x1234, 0x5678), then a dispatch. */
static void
x_dispatch_bound(
	uint64_t commands)
{
	x_command(GPU_OP_CMD_BIND_PIPELINE, commands);
	put32(VK_PIPELINE_BIND_POINT_COMPUTE);
	put64(ID_X_COMPUTE);
	x_command(GPU_OP_CMD_BIND_DESCRIPTOR_SETS, commands);
	put32(VK_PIPELINE_BIND_POINT_COMPUTE);
	put64(ID_LAYOUT);
	put32(0U);
	put32(1U);
	put64(1U);
	put64(ID_SET);
	put32(1U);
	put64(1U);
	put32(256U);
	x_command(GPU_OP_CMD_PUSH_CONSTANTS, commands);
	put64(ID_LAYOUT);
	put32(VK_SHADER_STAGE_COMPUTE_BIT);
	put32(0U);
	put32(8U);
	put64(8U);
	put32(0x1234U);
	put32(0x5678U);
	x_dispatch(commands);
}

/*
 * Appends a render pass with an indexed draw of 3 indices; with bound,
 * the binds first: the triangle with a dynamic viewport (ID_PIPELINE + 1),
 * its vertex buffer, a 16-bit index buffer at 0x100 of the buffer and the
 * viewport (2, 3, 64, 32, 0, 1).
 */
static void
x_pass_indexed(
	uint64_t commands,
	int bound)
{
	x_begin_pass(commands, 64U, 32U);
	if (bound) {
		x_bind_triangle(commands, ID_PIPELINE + 1U, 0U);
		x_command(GPU_OP_CMD_BIND_INDEX_BUFFER, commands);
		put64(ID_BUFFER);
		put64(0x100U);
		put32(VK_INDEX_TYPE_UINT16);
		x_command(GPU_OP_CMD_SET_VIEWPORT, commands);
		put32(0U);
		put32(1U);
		put64(1U);
		put32(0x40000000U);
		put32(0x40400000U);
		put32(0x42800000U);
		put32(0x42000000U);
		put32(0U);
		put32(0x3f800000U);
	}
	x_command(GPU_OP_CMD_DRAW_INDEXED, commands);
	put32(3U);
	put32(1U);
	put32(0U);
	put32(0U);
	put32(0U);
	x_command(GPU_OP_CMD_END_RENDER_PASS, commands);
}

/* Appends vkCmdSetEvent or vkCmdResetEvent of an event after the transfers. */
static void
x_set_event(
	uint64_t commands,
	uint32_t opcode,
	uint64_t event)
{
	x_command(opcode, commands);
	put64(event);
	put32(VK_PIPELINE_STAGE_TRANSFER_BIT);
}

/*
 * The binds a held submission places again: after a plain cut (the
 * service places the submission in pieces, and the GPU may lose the
 * session's binds between two), a dispatch places the compute pipeline,
 * its table and constants again (the push constants and the dynamic
 * offset), and the render pass after it the graphics pipeline, the
 * viewport, scissor and static state, the vertex and the index buffer;
 * a submission that is not held has no plain cut and places nothing
 * again.  After a reset (another session's fault) between two pieces,
 * the piece after the cut binds everything again and runs.
 */
static void
scene_x_hold_rebind(void)
{
	static const uint32_t compute[] = {
		GHCI_OP_BIND_PIPELINE, GHCI_OP_BIND_DESCRIPTOR_TABLE, GHCI_OP_LOAD_CONST, GHCI_OP_LOAD_CONST, GHCI_OP_DISPATCH
	};
	static const uint32_t graphics[] = {
		GHCI_OP_BEGIN_RENDER_PASS, GHCI_OP_BIND_PIPELINE, GHCI_OP_SET_VIEWPORT, GHCI_OP_SET_SCISSOR,
		GHCI_OP_SET_DYNAMIC_STATE, GHCI_OP_SET_DYNAMIC_STATE, GHCI_OP_SET_DYNAMIC_STATE, GHCI_OP_SET_DYNAMIC_STATE,
		GHCI_OP_BIND_VERTEX_BUFFERS, GHCI_OP_BIND_INDEX_BUFFER, GHCI_OP_DRAW_INDEXED, GHCI_OP_END_RENDER_PASS
	};
	const struct model_command *command;
	uint32_t ops[X_OPS_MAX];
	unsigned fills[8];
	struct node other;
	struct node node;
	unsigned rounds;
	unsigned found;
	unsigned mark;
	unsigned index;
	uint32_t culprit;
	int error;

	/* A 4 KiB ring, the compute set and pipeline, the triangle with a dynamic viewport, an event. */
	test_ring_bytes = 4096U;
	error = x_open(&node, MODEL_FULL);
	test_ring_bytes = 0U;
	error |= x_compute_set(&node);
	error |= (int)x_triangle(&node, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, VK_DYNAMIC_STATE_VIEWPORT, ID_PIPELINE + 1U);
	error |= x_create(&node, GPU_OP_CREATE_EVENT, VK_STRUCTURE_TYPE_EVENT_CREATE_INFO, ID_X_EVENT);
	expect(error == 0, "hold rebind: objects");

	/* The set; then the wait, a bound dispatch, 40 fills, a dispatch, a bound pass, 40 fills, a pass. */
	x_record(&node, ID_X_COMMANDS);
	x_set_event(ID_X_COMMANDS, GPU_OP_CMD_SET_EVENT, ID_X_EVENT);
	error = x_end(&node, ID_X_COMMANDS);
	x_record(&node, ID_X_COMMANDS + 1U);
	x_wait(ID_X_COMMANDS + 1U, ID_X_EVENT);
	x_dispatch_bound(ID_X_COMMANDS + 1U);
	for (index = 0U; index < 40U; index++)
		x_fill(ID_X_COMMANDS + 1U, 0x700U + index);
	x_dispatch(ID_X_COMMANDS + 1U);
	x_pass_indexed(ID_X_COMMANDS + 1U, 1);
	for (index = 0U; index < 40U; index++)
		x_fill(ID_X_COMMANDS + 1U, 0x740U + index);
	x_pass_indexed(ID_X_COMMANDS + 1U, 0);
	error |= x_end(&node, ID_X_COMMANDS + 1U);
	mark = model.command_count;
	model_hold(1);
	error |= x_submit(&node, ID_X_COMMANDS, 0U);
	error |= x_submit(&node, ID_X_COMMANDS + 1U, ID_X_FENCE);
	model_hold(0);
	model_tick();
	for (rounds = 0U; rounds < 16U && model_count_commands(GHCI_OP_DRAW_INDEXED) < 2U; rounds++) {
		model_tick();
		ghci_service_step(&device, GHCI_WORK_RECLAIM);
	}
	ghci_service_step(&device, GHCI_WORK_RECLAIM);
	expect(error == 0 && x_piece_fills(mark, fills, 8U) > 3U && x_fence_status(&node, ID_X_FENCE) == (uint32_t)VK_SUCCESS,
	       "hold rebind: the service places the held submission in pieces; its fence signals");

	/* The dispatch after the fills: the pipeline, the table and the constants again. */
	found = x_find(mark, GHCI_OP_DISPATCH, 1U);
	for (index = 0U; index < 5U && found != UINT32_MAX && found >= 4U; index++)
		ops[index] = model.commands[found - 4U + index].opcode;
	expect(found != UINT32_MAX && found >= 4U && x_same(ops, 5U, compute, 5U) && model.commands[found - 5U].opcode == GHCI_OP_FILL_BUFFER,
	       "hold rebind: after a plain cut the dispatch places BIND_PIPELINE, its table and two LOAD_CONST again");
	if (found != UINT32_MAX && found >= 4U) {
		command = &model.commands[found - 2U];
		expect(command->words[1] == 0U && command->words[3] == 0x1234U && command->words[4] == 0x5678U,
		       "hold rebind: the push constants again");
		command = &model.commands[found - 1U];
		expect(command->words[1] == 32U && command->words[3 + 8U] == 256U, "hold rebind: the dynamic offset again");
	}

	/* The pass after the fills: everything the draw reads again, with the recorded viewport. */
	found = x_find(mark, GHCI_OP_BEGIN_RENDER_PASS, 1U);
	for (index = 0U; index < 12U && found != UINT32_MAX && found + index < model.command_count; index++)
		ops[index] = model.commands[found + index].opcode;
	expect(found != UINT32_MAX && index == 12U && x_same(ops, 12U, graphics, 12U) && model.commands[found - 1U].opcode == GHCI_OP_FILL_BUFFER,
	       "hold rebind: the pass after the fills places the pipeline, its state, the vertex and index buffers again");
	if (found != UINT32_MAX && index == 12U) {
		expect(model.commands[found + 2U].words[1] == 0x40000000U && model.commands[found + 2U].words[3] == 0x42800000U,
		       "hold rebind: the recorded viewport again");
		command = &model.commands[found + 9U];
		expect((command->words[1] | ((uint64_t)command->words[2] << 32)) == x_va(&node, 0x80000U + 0x100U) && command->words[3] == 0U,
		       "hold rebind: the 16-bit index buffer again");
	}

	/* Not held: no plain cut, and the dispatch after the fills places nothing again. */
	x_record(&node, ID_X_COMMANDS + 2U);
	x_dispatch_bound(ID_X_COMMANDS + 2U);
	for (index = 0U; index < 40U; index++)
		x_fill(ID_X_COMMANDS + 2U, 0x780U + index);
	x_dispatch(ID_X_COMMANDS + 2U);
	error = x_end(&node, ID_X_COMMANDS + 2U);
	mark = model.command_count;
	error |= x_submit(&node, ID_X_COMMANDS + 2U, 0U);
	found = x_find(mark, GHCI_OP_DISPATCH, 1U);
	expect(error == 0 && found != UINT32_MAX && model.commands[found - 1U].opcode == GHCI_OP_FILL_BUFFER &&
	       x_find(mark, GHCI_OP_BIND_PIPELINE, 1U) == UINT32_MAX && x_piece_fills(mark, fills, 8U) == 1U,
	       "hold rebind: a submission that is not held: one piece, and the dispatch follows the fills");

	/* A reset between two pieces: a bound dispatch placed, another session's fault, then the rest. */
	error = x_host_event(&node, GPU_OP_RESET_EVENT, ID_X_EVENT);
	x_record(&node, ID_X_COMMANDS + 2U);
	x_dispatch_bound(ID_X_COMMANDS + 2U);
	x_wait(ID_X_COMMANDS + 2U, ID_X_EVENT);
	x_dispatch(ID_X_COMMANDS + 2U);
	error |= x_end(&node, ID_X_COMMANDS + 2U);
	mark = model.command_count;
	error |= x_submit(&node, ID_X_COMMANDS + 2U, ID_X_FENCE);
	model_tick();
	ghci_service_step(&device, GHCI_WORK_RECLAIM);
	expect(error == 0 && model_count_commands(GHCI_OP_DISPATCH) != 0U && x_find(mark, GHCI_OP_DISPATCH, 0U) != UINT32_MAX &&
	       x_find(mark, GHCI_OP_DISPATCH, 1U) == UINT32_MAX, "hold rebind: the piece before the wait ran");
	error = x_other_open(&other);
	culprit = 0U;
	if (session_of(&other)->devices != NULL)
		culprit = session_of(&other)->devices->vm->session;
	model_inject(GHCI_FAULT_KIND_AXI_ERROR, GHCI_UNIT_CP, culprit, 0x2000U, 0);
	ghci_service_step(&device, GHCI_WORK_RECOVER);
	ghci_service_step(&device, GHCI_WORK_RECLAIM);
	expect(error == 0 && culprit != 0U && session_of(&node)->hold_count == 1U, "hold rebind: another session's reset leaves the held work");
	mark = model.command_count;
	error = x_host_event(&node, GPU_OP_SET_EVENT, ID_X_EVENT);
	model_tick();
	ghci_service_step(&device, GHCI_WORK_RECLAIM);
	found = x_find(mark, GHCI_OP_DISPATCH, 0U);
	for (index = 0U; index < 5U && found != UINT32_MAX && found >= 4U; index++)
		ops[index] = model.commands[found - 4U + index].opcode;
	expect(error == 0 && found != UINT32_MAX && found >= 4U && x_same(ops, 5U, compute, 5U) &&
	       x_fence_status(&node, ID_X_FENCE) == (uint32_t)VK_SUCCESS,
	       "hold rebind: after the reset the rest binds everything again and runs; its fence signals");
	node_close(&other);
	node_close(&node);
	stop("hold rebind");
}

/*
 * A held submission whose stretch between two cuts is longer than a
 * 4 KiB ring, behind an earlier submission's set on the GPU: an
 * occlusion query begun outside a render pass around 220 fills keeps
 * plain cuts out (the cuts by hand: the wait's at dword 4, after the
 * head barrier; the query from 8 to 11, the fills from 12 to 1111, the
 * query's end from 1112 to 1115; then a plain cut before the first of 30
 * fills, at 1116, and one 26 fills later, at 1246).  The service thread
 * never places the 1112 dwords from 4 to 1115 (more than the ring); the
 * client's poll of the fence places the rest whole, and the fence
 * signals.  A wait for the host inside such a query cuts the submission
 * before the query's begin, so that the query runs in one piece of the
 * session (the occlusion counter is one for the device and a session's
 * bind clears it: ghci.md 644072a section 7.3), and the dispatch inside
 * it binds everything again.
 */
static void
scene_x_hold_stretch(void)
{
	static const uint32_t query[] = {
		GHCI_OP_BEGIN_QUERY, GHCI_OP_FILL_BUFFER, GHCI_OP_BIND_PIPELINE, GHCI_OP_BIND_DESCRIPTOR_TABLE, GHCI_OP_LOAD_CONST,
		GHCI_OP_LOAD_CONST, GHCI_OP_DISPATCH, GHCI_OP_BARRIER, GHCI_OP_FILL_BUFFER, GHCI_OP_END_QUERY
	};
	uint32_t values[X_FILLS_MAX];
	uint32_t ops[X_OPS_MAX];
	const struct ghci_vk_submission *submission;
	unsigned fills[4];
	struct node node;
	unsigned found;
	unsigned rounds;
	unsigned pieces;
	unsigned count;
	unsigned mark;
	unsigned index;
	int ordered;
	int error;

	/* A 4 KiB ring, an event and an occlusion pool of two queries. */
	test_ring_bytes = 4096U;
	error = x_open(&node, MODEL_FULL);
	test_ring_bytes = 0U;
	error |= x_create(&node, GPU_OP_CREATE_EVENT, VK_STRUCTURE_TYPE_EVENT_CREATE_INFO, ID_X_EVENT);
	begin(&node, GPU_OP_CREATE_QUERY_POOL);
	put64(ID_DEVICE);
	put64(1U);
	put32(VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO);
	put64(0U);
	put32(0U);
	put32(VK_QUERY_TYPE_OCCLUSION);
	put32(2U);
	put32(0U);
	tail(ID_X_OCCLUSION);
	error |= run(&node);
	error |= (int)reply32(&node, 4U);
	error |= x_compute_set(&node);
	expect(error == 0, "hold stretch: objects");

	/* The set; then the wait, the query around 220 fills, and 30 fills. */
	x_record(&node, ID_X_COMMANDS);
	x_set_event(ID_X_COMMANDS, GPU_OP_CMD_SET_EVENT, ID_X_EVENT);
	error = x_end(&node, ID_X_COMMANDS);
	x_record(&node, ID_X_COMMANDS + 1U);
	x_wait(ID_X_COMMANDS + 1U, ID_X_EVENT);
	x_command(GPU_OP_CMD_BEGIN_QUERY, ID_X_COMMANDS + 1U);
	put64(ID_X_OCCLUSION);
	put32(0U);
	put32(0U);
	for (index = 0U; index < 220U; index++)
		x_fill(ID_X_COMMANDS + 1U, 0x2000U + index);
	x_command(GPU_OP_CMD_END_QUERY, ID_X_COMMANDS + 1U);
	put64(ID_X_OCCLUSION);
	put32(0U);
	for (index = 220U; index < 250U; index++)
		x_fill(ID_X_COMMANDS + 1U, 0x2000U + index);
	error |= x_end(&node, ID_X_COMMANDS + 1U);
	mark = model.command_count;
	model_hold(1);
	error |= x_submit(&node, ID_X_COMMANDS, 0U);
	error |= x_submit(&node, ID_X_COMMANDS + 1U, ID_X_FENCE);

	/* No plain cut while the query is active. */
	submission = NULL;
	if (session_of(&node)->devices != NULL && session_of(&node)->devices->holds != NULL)
		submission = &session_of(&node)->devices->holds->submission;
	expect(error == 0 && submission != NULL && submission->used == 1270U && submission->cut_count == 3U &&
	       submission->cuts[0].at == 4U && submission->cuts[0].count == 1U && submission->cuts[1].at == 1116U &&
	       submission->cuts[1].count == 0U && submission->cuts[2].at == 1246U,
	       "hold stretch: cuts at 4 (the wait), 1116 and 1246: none inside the query");

	/* The set runs; the service cannot place the stretch. */
	model_hold(0);
	model_tick();
	for (rounds = 0U; rounds < 8U; rounds++) {
		model_tick();
		ghci_service_step(&device, GHCI_WORK_RECLAIM);
	}
	expect(x_fills(mark, values) == 0U && session_of(&node)->hold_count == 1U && x_piece_fills(mark, fills, 4U) == 2U,
	       "hold stretch: the service does not place a stretch longer than the ring");

	/* The client polls the fence: the rest is placed whole, and the fence signals. */
	(void)x_fence_status(&node, ID_X_FENCE);
	for (rounds = 0U; rounds < 8U && x_fills(mark, values) < 250U; rounds++) {
		model_tick();
		ghci_service_step(&device, GHCI_WORK_RECLAIM);
	}
	ghci_service_step(&device, GHCI_WORK_RECLAIM);
	count = x_fills(mark, values);
	ordered = count == 250U;
	for (index = 0U; index < count && ordered; index++) {
		if (values[index] != 0x2000U + index)
			ordered = 0;
	}
	pieces = x_piece_fills(mark, fills, 4U);
	expect(ordered && pieces == 3U && fills[2] == 250U && session_of(&node)->hold_count == 0U &&
	       x_fence_status(&node, ID_X_FENCE) == (uint32_t)VK_SUCCESS && model_count_commands(GHCI_OP_BEGIN_QUERY) == 1U &&
	       model_count_commands(GHCI_OP_END_QUERY) == 1U,
	       "hold stretch: the poll places the rest in one piece: 250 fills in order; the fence signals");

	/* A bound dispatch, a fill, then a query around a fill, a dispatch, a wait for the host and a fill; a fill after. */
	error = x_host_event(&node, GPU_OP_RESET_EVENT, ID_X_EVENT);
	x_record(&node, ID_X_COMMANDS + 2U);
	x_dispatch_bound(ID_X_COMMANDS + 2U);
	x_fill(ID_X_COMMANDS + 2U, 0x801U);
	x_command(GPU_OP_CMD_BEGIN_QUERY, ID_X_COMMANDS + 2U);
	put64(ID_X_OCCLUSION);
	put32(1U);
	put32(0U);
	x_fill(ID_X_COMMANDS + 2U, 0x802U);
	x_dispatch(ID_X_COMMANDS + 2U);
	x_wait(ID_X_COMMANDS + 2U, ID_X_EVENT);
	x_fill(ID_X_COMMANDS + 2U, 0x803U);
	x_command(GPU_OP_CMD_END_QUERY, ID_X_COMMANDS + 2U);
	put64(ID_X_OCCLUSION);
	put32(1U);
	x_fill(ID_X_COMMANDS + 2U, 0x804U);
	error |= x_end(&node, ID_X_COMMANDS + 2U);
	mark = model.command_count;
	error |= x_submit(&node, ID_X_COMMANDS + 2U, ID_X_FENCE);
	model_tick();
	ghci_service_step(&device, GHCI_WORK_RECLAIM);
	count = x_fills(mark, values);
	expect(error == 0 && count == 1U && values[0] == 0x801U && x_find(mark, GHCI_OP_BEGIN_QUERY, 0U) == UINT32_MAX &&
	       session_of(&node)->hold_count == 1U, "hold stretch: a wait inside a query holds from the query's begin");
	error = x_host_event(&node, GPU_OP_SET_EVENT, ID_X_EVENT);
	model_tick();
	ghci_service_step(&device, GHCI_WORK_RECLAIM);
	found = x_find(mark, GHCI_OP_BEGIN_QUERY, 0U);
	for (index = 0U; index < 10U && found != UINT32_MAX && found + index < model.command_count; index++)
		ops[index] = model.commands[found + index].opcode;
	count = x_fills(mark, values);
	expect(error == 0 && found != UINT32_MAX && found >= 2U && index == 10U && x_same(ops, 10U, query, 10U) &&
	       model.commands[found - 2U].opcode == GHCI_OP_BIND_SESSION && model.commands[found - 2U].words[1] != 0U &&
	       model.commands[found - 1U].opcode == GHCI_OP_BARRIER && count == 4U && values[3] == 0x804U &&
	       x_fence_status(&node, ID_X_FENCE) == (uint32_t)VK_SUCCESS,
	       "hold stretch: the host's set places the query whole in one piece, the dispatch in it bound again");
	node_close(&node);
	stop("hold stretch");
}

/*
 * The word of an event placed work sets and then resets is not taken for
 * set while that work runs: a wait behind it stays held when the word
 * shows 1 between the set and the reset, and after the work ran (the
 * word 0); the host's set lets it go.  The stop of the session ends what
 * it holds with an error: the stop wakes the service thread, the fence
 * is lost and the marker behind it ends with EIO, and the stop poll is
 * done.
 */
static void
scene_x_hold_ends(void)
{
	struct drv_gpu_completion *completion;
	static char completions[1];
	uint32_t values[X_FILLS_MAX];
	struct ghci_vk_object *object;
	struct node node;
	unsigned completed;
	unsigned mark;
	void *token;
	int error;

	/* The device and an event. */
	completion = (struct drv_gpu_completion *)(void *)&completions[0];
	error = x_open(&node, MODEL_FULL);
	error |= x_create(&node, GPU_OP_CREATE_EVENT, VK_STRUCTURE_TYPE_EVENT_CREATE_INFO, ID_X_EVENT);
	object = ghci_vk_object_find(session_of(&node), GHCI_VK_EVENT, ID_X_EVENT);
	expect(error == 0 && object != NULL, "hold ends: objects");
	if (object == NULL) {
		node_close(&node);
		stop("hold ends");
		return;
	}

	/* Placed work that sets and resets the event, held by the processor; a wait behind it. */
	x_record(&node, ID_X_COMMANDS);
	x_set_event(ID_X_COMMANDS, GPU_OP_CMD_SET_EVENT, ID_X_EVENT);
	x_set_event(ID_X_COMMANDS, GPU_OP_CMD_RESET_EVENT, ID_X_EVENT);
	error = x_end(&node, ID_X_COMMANDS);
	x_record(&node, ID_X_COMMANDS + 1U);
	x_wait(ID_X_COMMANDS + 1U, ID_X_EVENT);
	x_fill(ID_X_COMMANDS + 1U, 0x601U);
	error |= x_end(&node, ID_X_COMMANDS + 1U);
	mark = model.command_count;
	model_hold(1);
	error |= x_submit(&node, ID_X_COMMANDS, 0U);
	error |= x_submit(&node, ID_X_COMMANDS + 1U, ID_X_FENCE);

	/* The word at 1 between the set and the reset: still held. */
	*object->as.event.word = 1U;
	ghci_service_step(&device, GHCI_WORK_RECLAIM);
	expect(error == 0 && x_fills(mark, values) == 0U && session_of(&node)->hold_count == 1U,
	       "hold ends: a word at 1 while the work that resets it runs does not let the wait go");

	/* The work ran: the word is 0, still held; the host's set lets it go. */
	model_hold(0);
	model_tick();
	ghci_service_step(&device, GHCI_WORK_RECLAIM);
	expect(*object->as.event.word == 0U && x_fills(mark, values) == 0U && x_fence_status(&node, ID_X_FENCE) == (uint32_t)VK_NOT_READY,
	       "hold ends: after the set and the reset ran, the wait is still held");
	error = x_host_event(&node, GPU_OP_SET_EVENT, ID_X_EVENT);
	model_tick();
	ghci_service_step(&device, GHCI_WORK_RECLAIM);
	expect(error == 0 && x_fills(mark, values) == 1U && values[0] == 0x601U && x_fence_status(&node, ID_X_FENCE) == (uint32_t)VK_SUCCESS,
	       "hold ends: the host's set places it; the fence signals");

	/* Held work with its fence and a marker; the stop wakes the service, which ends both. */
	error = x_host_event(&node, GPU_OP_RESET_EVENT, ID_X_EVENT);
	x_record(&node, ID_X_COMMANDS + 1U);
	x_wait(ID_X_COMMANDS + 1U, ID_X_EVENT);
	x_fill(ID_X_COMMANDS + 1U, 0x602U);
	error |= x_end(&node, ID_X_COMMANDS + 1U);
	mark = model.command_count;
	error |= x_submit(&node, ID_X_COMMANDS + 1U, ID_X_FENCE);
	completed = stub.completions;
	error |= device.ops.jobs->reserve(&device, node.session, 1U, completion, &token);
	error |= device.ops.jobs->commit(&device, node.session, token, completion);
	model_tick();
	ghci_service_step(&device, GHCI_WORK_RECLAIM);
	expect(error == 0 && session_of(&node)->hold_count == 1U && stub.completions == completed, "hold ends: held, the marker waits");
	device.service_work = 0U;
	error = device.ops.recovery->stop_begin(&device, node.session, 0);
	expect(error == 0 && (device.service_work & GHCI_WORK_RECLAIM) != 0U, "hold ends: the stop wakes the service thread");
	ghci_service_step(&device, GHCI_WORK_RECLAIM);
	object = ghci_vk_object_find(session_of(&node), GHCI_VK_FENCE, ID_X_FENCE);
	expect(session_of(&node)->hold_count == 0U && object != NULL && object->as.fence.lost && stub.completions == completed + 1U &&
	       stub.last_completion_error == EIO && x_fills(mark, values) == 0U,
	       "hold ends: the stop ends the held work: its fence is lost, its marker EIO, nothing of it runs");
	expect(device.ops.recovery->stop_poll(&device, node.session) == 0, "hold ends: the stop poll is done");
	node_close(&node);
	stop("hold ends");
}

/*
 * The loss of the GPU ends what a session holds with an error: its fence
 * is lost and the marker behind it ends with EIO, nothing of it runs;
 * the next context fence of the timeline ends with EIO (the held work
 * it follows failed), and the one after it completes.
 */
static void
scene_x_hold_lost(void)
{
	struct drv_gpu_completion *completion[3];
	static char completions[3];
	uint32_t values[X_FILLS_MAX];
	struct node node;
	unsigned completed;
	unsigned count;
	unsigned mark;
	void *token;
	int error;

	/* The device, an event, held work with its fence and a marker. */
	for (count = 0U; count < 3U; count++)
		completion[count] = (struct drv_gpu_completion *)(void *)&completions[count];
	error = x_open(&node, MODEL_FULL);
	error |= x_create(&node, GPU_OP_CREATE_EVENT, VK_STRUCTURE_TYPE_EVENT_CREATE_INFO, ID_X_EVENT);
	x_record(&node, ID_X_COMMANDS);
	x_wait(ID_X_COMMANDS, ID_X_EVENT);
	x_fill(ID_X_COMMANDS, 0x603U);
	error |= x_end(&node, ID_X_COMMANDS);
	mark = model.command_count;
	error |= x_submit(&node, ID_X_COMMANDS, ID_X_FENCE);
	completed = stub.completions;
	error |= device.ops.jobs->reserve(&device, node.session, 1U, completion[0], &token);
	error |= device.ops.jobs->commit(&device, node.session, token, completion[0]);
	model_tick();
	ghci_service_step(&device, GHCI_WORK_RECLAIM);
	expect(error == 0 && session_of(&node)->hold_count == 1U && stub.completions == completed, "hold lost: held, the marker waits");

	/* The GPU is lost: the held work fails. */
	device.ops.recovery->fault(&device, EIO);
	ghci_service_step(&device, GHCI_WORK_RECLAIM);
	expect(session_of(&node)->hold_count == 0U && stub.completions == completed + 1U && stub.last_completion_error == EIO &&
	       x_fills(mark, values) == 0U && x_fence_status(&node, ID_X_FENCE) == (uint32_t)VK_ERROR_DEVICE_LOST,
	       "hold lost: the loss ends the held work: its fence is lost, its marker EIO, nothing of it runs");

	/* The next context fence reports the failure; the one after it does not. */
	error = device.ops.commands->submit(&device, node.session, NULL, 0U, GPU_COMMAND_CONTEXT_FENCE, 1U, completion[1]);
	ghci_service_step(&device, GHCI_WORK_RECLAIM);
	expect(error == 0 && stub.completions == completed + 2U && stub.last_completion_error == EIO,
	       "hold lost: the next context fence of the timeline ends with EIO");
	error = device.ops.commands->submit(&device, node.session, NULL, 0U, GPU_COMMAND_CONTEXT_FENCE, 1U, completion[2]);
	ghci_service_step(&device, GHCI_WORK_RECLAIM);
	expect(error == 0 && stub.completions == completed + 3U && stub.last_completion_error == 0,
	       "hold lost: the context fence after it completes");
	node_close(&node);
	stop("hold lost");
}

/*
 * The budget of held work: a held submission takes its host memory and
 * its temporary memory (a 64 KiB chunk for its SetEvent's word) of the
 * session's budget.  With 32 KiB left it is refused with
 * VK_ERROR_OUT_OF_DEVICE_MEMORY, with 64 bytes left with
 * VK_ERROR_OUT_OF_HOST_MEMORY, and nothing is held or taken; within the
 * budget it is held, and it gives what it took back once placed.
 */
static void
scene_x_hold_budget(void)
{
	struct ghci_vk_session *session;
	const struct ghci_vk_hold *hold;
	uint32_t values[X_FILLS_MAX];
	struct node node;
	uint64_t saved;
	uint64_t taken;
	unsigned mark;
	int error;

	/* The device and two events; the wait, a set of the other event and a fill. */
	error = x_open(&node, MODEL_FULL);
	error |= x_create(&node, GPU_OP_CREATE_EVENT, VK_STRUCTURE_TYPE_EVENT_CREATE_INFO, ID_X_EVENT);
	error |= x_create(&node, GPU_OP_CREATE_EVENT, VK_STRUCTURE_TYPE_EVENT_CREATE_INFO, ID_X_EVENT + 1U);
	x_record(&node, ID_X_COMMANDS);
	x_wait(ID_X_COMMANDS, ID_X_EVENT);
	x_set_event(ID_X_COMMANDS, GPU_OP_CMD_SET_EVENT, ID_X_EVENT + 1U);
	x_fill(ID_X_COMMANDS, 0x604U);
	error |= x_end(&node, ID_X_COMMANDS);
	expect(error == 0, "hold budget: objects");
	session = session_of(&node);
	saved = session->budget_used;
	mark = model.command_count;

	/* 32 KiB left: the temporary memory does not fit. */
	session->budget_used = GHCI_VK_BUDGET_BYTES - 32768U;
	error = x_submit(&node, ID_X_COMMANDS, ID_X_FENCE);
	expect(error == 0 && reply32(&node, 4U) == (uint32_t)VK_ERROR_OUT_OF_DEVICE_MEMORY && session->hold_count == 0U &&
	       session->budget_used == GHCI_VK_BUDGET_BYTES - 32768U, "hold budget: 32 KiB left: OUT_OF_DEVICE_MEMORY, nothing held");

	/* 64 bytes left: the host memory does not fit. */
	session->budget_used = GHCI_VK_BUDGET_BYTES - 64U;
	error = x_submit(&node, ID_X_COMMANDS, ID_X_FENCE);
	expect(error == 0 && reply32(&node, 4U) == (uint32_t)VK_ERROR_OUT_OF_HOST_MEMORY && session->hold_count == 0U &&
	       session->budget_used == GHCI_VK_BUDGET_BYTES - 64U, "hold budget: 64 bytes left: OUT_OF_HOST_MEMORY, nothing held");

	/* Within the budget: held, with its 64 KiB chunk and its host memory taken. */
	session->budget_used = saved;
	error = x_submit(&node, ID_X_COMMANDS, ID_X_FENCE);
	hold = NULL;
	if (session->devices != NULL)
		hold = session->devices->holds;
	taken = session->budget_used - saved;
	expect(error == 0 && reply32(&node, 4U) == (uint32_t)VK_SUCCESS && hold != NULL && hold->submission.temp_bytes == 65536U &&
	       hold->charged == taken && taken > 65536U && x_fills(mark, values) == 0U,
	       "hold budget: within the budget: held; its 64 KiB chunk and its host memory are taken");

	/* The host's set: placed, and the budget goes back. */
	error = x_host_event(&node, GPU_OP_SET_EVENT, ID_X_EVENT);
	model_tick();
	ghci_service_step(&device, GHCI_WORK_RECLAIM);
	expect(error == 0 && session->budget_used == saved && session->hold_count == 0U && x_fills(mark, values) == 1U &&
	       values[0] == 0x604U && x_fence_status(&node, ID_X_FENCE) == (uint32_t)VK_SUCCESS,
	       "hold budget: placed, the budget goes back; the fence signals");
	node_close(&node);
	stop("hold budget");
}
