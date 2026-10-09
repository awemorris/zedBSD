/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The compute part of the code generator (ws101-p002), included by
 * compile.c.
 *
 * Register conventions of a SIMD8 compute thread on Gen12.0 (GPGPU_WALKER):
 *
 *   r0              the thread header: the workgroup's place in the dispatch
 *                   in dwords 1 (x), 6 (y) and 7 (z) (Mesa's
 *                   emit_work_group_id_setup()), the scratch space in
 *                   dwords 3 and 5
 *   r1 ..           the cross-thread push data every thread of a group reads:
 *                   the push constants, then the uniform blocks and the
 *                   storage buffers' addresses (i915_compile_blocks()) --
 *                   push_regs registers
 *   then 4          the thread's own push data: the x, y and z of each
 *                   channel's invocation in its group and its linear index,
 *                   a register each, which the dispatch computes
 *   after them      the values, as for any stage (r16 on, or right after the
 *                   payload)
 *   r127            the thread's end: a copy of r0 sent to the thread
 *                   spawner (Mesa's emit_cs_terminate() before Gfx12.5)
 *
 * A storage buffer word changed atomically goes to the data port as an A64
 * untyped atomic of the channels that run it, with its address built as for
 * a load (i915_compile_storage()); the old value is asked for only when an
 * instruction reads it.
 *
 * Shared memory (ws101-p006) is the data port's untyped surface messages
 * and untyped atomics at binding table entry 254, one register of byte
 * offsets as the address payload.  A fence is a SIMD1 memory fence message
 * of r0 whose reply the thread waits for; a workgroup barrier is the
 * gateway's barrier message with the group's barrier ID from r0.2, then
 * sync.bar (Mesa's emit_barrier() and generate_barrier()).
 *
 * The file is part of compile.c's translation unit: it uses that file's
 * state, constants and helpers, and its functions are declared with the
 * others at the top of compile.c.
 */

/* The registers of r0 holding the workgroup's place along x, y and z, as byte offsets. */
#define COMPILE_CS_GROUP_X_BYTE		4U
#define COMPILE_CS_GROUP_Y_BYTE		24U
#define COMPILE_CS_GROUP_Z_BYTE		28U

/* The byte of a storage buffer's address register that holds the buffer's range. */
#define COMPILE_CS_RANGE_BYTE		8U

/* The register the message that ends a compute thread is sent from: the last one, as a thread end must be in r112 .. r127. */
#define COMPILE_CS_END_GRF		COMPILE_MAX_GRF

/*
 * Checks what a compute shader reads and writes: no interface location, no
 * render target, no discard -- only memory.
 */
static void
i915_compile_compute_interface(
	struct i915_compile_state *state)
{
	/* A compute shader has no inputs, varyings or colour outputs. */
	if (state->input_count != 0U ||
	    state->varying_count != 0U ||
	    state->fs_outputs != 0U)
		state->unsupported = 1;

	/* Nor a point size. */
	if (state->writes_point_size != 0)
		state->unsupported = 1;
}

/*
 * Lowers LOAD_SYSTEM: a compute built-in value of each channel, from the
 * thread's own push data (its invocation's place in the group) or from the
 * header (the group's place in the dispatch, the same for every channel).
 */
static void
i915_compile_load_system(
	struct i915_compile_state *state,
	const struct drv_gpu_shader_ir_inst *inst,
	uint32_t payload_inputs)
{
	struct i915_eu_reg source;
	uint32_t byte;
	uint32_t dst;

	/* The value gets its register. */
	dst = i915_compile_define(state, inst->dst, 1U);

	/* The invocation's place in its group: a register of the thread's own push data, after the cross-thread data. */
	if (inst->component <= DRV_GPU_IR_SYSTEM_LOCAL_INDEX) {
		source = drv_i915_eu_grf_ud(payload_inputs + inst->component);
		drv_i915_eu_mov(&state->code, drv_i915_eu_grf_ud(dst), source);
		return;
	}

	/* The group's place along the axis: a dword of the header. */
	if (inst->component == DRV_GPU_IR_SYSTEM_GROUP_ID_X) {
		byte = COMPILE_CS_GROUP_X_BYTE;
	} else if (inst->component == DRV_GPU_IR_SYSTEM_GROUP_ID_Y) {
		byte = COMPILE_CS_GROUP_Y_BYTE;
	} else if (inst->component == DRV_GPU_IR_SYSTEM_GROUP_ID_Z) {
		byte = COMPILE_CS_GROUP_Z_BYTE;
	} else {
		/* A built-in the IR does not define is inconsistent IR. */
		state->error = 1;
		return;
	}

	/* Every channel reads the same dword. */
	source = drv_i915_eu_grf_scalar(0U, byte);
	source.type = COMPILE_TYPE_UD;
	drv_i915_eu_mov(&state->code, drv_i915_eu_grf_ud(dst), source);
}

/*
 * Finds the push data block that holds a storage buffer's address.
 * Returns 1 with its index, or 0 when the interface laid none out for the
 * uniform, which is inconsistent IR.
 */
static int
i915_compile_storage_block(
	struct i915_compile_state *state,
	uint32_t uniform,
	uint32_t *block)
{
	uint32_t index;

	/* Looks among the blocks for the one that carries the buffer's address. */
	for (index = 0U; index < state->block_count; index++) {
		if (state->block_uniform[index] == uniform && state->block_address[index] != 0U)
			break;
	}

	/* A storage buffer the interface did not lay out is inconsistent IR. */
	if (index == state->block_count) {
		state->error = 1;
		return 0;
	}

	/* Succeeded: the block holds the buffer's address. */
	*block = index;
	return 1;
}

/*
 * Lowers STORAGE_SIZE: the range of a storage buffer, which the dispatch
 * puts in dword 2 of the buffer's address register.
 */
static void
i915_compile_storage_size(
	struct i915_compile_state *state,
	const struct drv_gpu_shader_ir_inst *inst,
	uint32_t payload_inputs)
{
	struct i915_eu_reg source;
	uint32_t block;
	uint32_t push_grf;
	uint32_t dst;
	int found;

	/* Finds the buffer's address register. */
	found = i915_compile_storage_block(state, inst->location, &block);
	if (!found)
		return;

	/* Every channel reads the range beside the address. */
	dst = i915_compile_define(state, inst->dst, 1U);
	push_grf = payload_inputs - state->push_regs + state->block_push_offset[block] / 32U;
	source = drv_i915_eu_grf_scalar(push_grf, COMPILE_CS_RANGE_BYTE);
	source.type = COMPILE_TYPE_UD;
	drv_i915_eu_mov(&state->code, drv_i915_eu_grf_ud(dst), source);
}

/*
 * Lowers ATOMIC: one A64 untyped atomic message of the channels that run it
 * (Mesa's brw_dp_a64_untyped_atomic_desc(), SIMD8), to the buffer's address
 * plus each channel's byte offset, built as a load builds it.
 *
 * The data run is the value, or for a compare and exchange the comparator
 * then the new value (Mesa's MEMORY_LOGICAL_DATA0 and DATA1).  The old value
 * comes back only when an instruction reads it: the reply bit of the message
 * control and the reply length are set together.
 */
static void
i915_compile_atomic(
	struct i915_compile_state *state,
	const struct drv_gpu_shader_ir_inst *inst,
	uint32_t payload_inputs)
{
	struct i915_eu_reg low;
	struct i915_eu_reg high;
	struct i915_eu_reg interleaved;
	struct i915_eu_reg null;
	struct i915_eu_reg reply;
	uint32_t descriptor;
	uint32_t control;
	uint32_t block;
	uint32_t push_grf;
	uint32_t offset_grf;
	uint32_t value_grf;
	uint32_t compare_grf;
	uint32_t predicate_grf;
	uint32_t data_grf;
	uint32_t data_regs;
	uint32_t operands;
	uint32_t payload;
	uint32_t sum;
	uint32_t carry;
	uint32_t dst;
	int returns;
	int found;

	/* A word of shared memory is changed by its own message (ws101-p006). */
	if (inst->location == DRV_GPU_IR_LOCATION_SHARED) {
		i915_compile_atomic_shared(state, inst);
		return;
	}

	/* Finds the buffer's address among the push data's blocks. */
	found = i915_compile_storage_block(state, inst->location, &block);
	if (!found)
		return;

	/* The address's two words in the push data. */
	push_grf = payload_inputs - state->push_regs + state->block_push_offset[block] / 32U;
	low = drv_i915_eu_grf_scalar(push_grf, 0U);
	low.type = COMPILE_TYPE_UD;
	high = drv_i915_eu_grf_scalar(push_grf, 4U);
	high.type = COMPILE_TYPE_UD;

	/* Reads the operands' registers: the offset and the value. */
	operands = i915_compile_operands(inst);
	offset_grf = i915_compile_grf(state, inst->src[0]);
	value_grf = i915_compile_grf(state, inst->src[1]);

	/* A compare and exchange also reads its comparator. */
	compare_grf = 0U;
	if (inst->immediate == DRV_GPU_IR_ATOMIC_CMPXCHG)
		compare_grf = i915_compile_grf(state, inst->src[2]);

	/* A predicated atomic reads its predicate, the last operand. */
	predicate_grf = 0U;
	if (inst->component != 0U)
		predicate_grf = i915_compile_grf(state, inst->src[operands - 1U]);

	/* The old value is asked for only when a later instruction reads it. */
	returns = 0;
	if (state->last_use[inst->dst] > state->index)
		returns = 1;

	/* The result gets its register, which the reply fills when the old value is asked for. */
	dst = i915_compile_define(state, inst->dst, 1U);

	/* Takes the address payload and the sum and carry registers. */
	payload = i915_compile_temporaries(state, 2U);
	sum = i915_compile_temporary(state);
	carry = i915_compile_temporary(state);

	/* Two data values -- a compare and exchange's -- take a run of their own; one is sent from its register. */
	data_grf = value_grf;
	data_regs = 1U;
	if (inst->immediate == DRV_GPU_IR_ATOMIC_CMPXCHG) {
		data_grf = i915_compile_temporaries(state, 2U);
		data_regs = 2U;
	}

	/* Gives up when the registers ran out; the compile reports it. */
	if (state->out_of_registers != 0)
		return;

	/* The low word of each address, and its carry into the high word. */
	drv_i915_eu_alu2(&state->code, I915_EU_ADD, drv_i915_eu_grf_ud(sum), low, drv_i915_eu_grf_ud(offset_grf));
	drv_i915_eu_cmp(&state->code,
			I915_EU_COND_LT,
			I915_EU_FLAG_F0_0,
			0,
			drv_i915_eu_grf_ud(carry),
			drv_i915_eu_grf_ud(sum),
			drv_i915_eu_grf_ud(offset_grf));
	drv_i915_eu_alu2(&state->code, I915_EU_ADD, drv_i915_eu_grf_ud(carry), high, drv_i915_eu_negate(drv_i915_eu_grf_ud(carry)));

	/* Interleaves them: low words at the even dwords of the payload, high words at the odd ones. */
	interleaved = drv_i915_eu_grf_ud(payload);
	interleaved.hstride = EU_HSTRIDE_2;
	drv_i915_eu_mov(&state->code, interleaved, drv_i915_eu_grf_ud(sum));
	interleaved.subnr = 4U;
	drv_i915_eu_mov(&state->code, interleaved, drv_i915_eu_grf_ud(carry));

	/* A compare and exchange sends the comparator, then the new value. */
	if (inst->immediate == DRV_GPU_IR_ATOMIC_CMPXCHG) {
		drv_i915_eu_mov(&state->code, drv_i915_eu_grf_ud(data_grf), drv_i915_eu_grf_ud(compare_grf));
		drv_i915_eu_mov(&state->code, drv_i915_eu_grf_ud(data_grf + 1U), drv_i915_eu_grf_ud(value_grf));
	}

	/* The message: two address registers, the data run, and one reply register when the old value is read. */
	control = inst->immediate;
	descriptor = (2U << EU_DESC_MLEN_SHIFT) |
		     (EU_DP_A64_UNTYPED_ATOMIC << EU_DP_TYPE_SHIFT) |
		     EU_BTI_STATELESS_NON_COHERENT;
	reply = drv_i915_eu_null();
	if (returns != 0) {
		control |= EU_DP_ATOMIC_RETURN;
		descriptor |= 1U << EU_DESC_RLEN_SHIFT;
		reply = drv_i915_eu_grf(dst);
	}

	/* The message control -- the operation and the reply bit -- goes into the descriptor. */
	descriptor |= control << EU_DP_CONTROL_SHIFT;

	/* A predicated atomic changes the word only for the channels whose predicate is not zero. */
	if (inst->component != 0U) {
		null = drv_i915_eu_null();
		null.type = COMPILE_TYPE_D;
		drv_i915_eu_cmp(&state->code,
				I915_EU_COND_NE,
				I915_EU_FLAG_F0_0,
				0,
				null,
				drv_i915_eu_grf_d(predicate_grf),
				drv_i915_eu_imm_d(0U));
		drv_i915_eu_send_masked(&state->code,
					I915_EU_FLAG_F0_0,
					reply,
					drv_i915_eu_grf(payload),
					drv_i915_eu_grf(data_grf),
					EU_SFID_DATA_CACHE_1,
					descriptor,
					COMPILE_EX_MLEN(data_regs),
					0,
					0);
	} else {
		/* An atomic outside every predicate changes the word for every channel. */
		drv_i915_eu_send(&state->code,
				 reply,
				 drv_i915_eu_grf(payload),
				 drv_i915_eu_grf(data_grf),
				 EU_SFID_DATA_CACHE_1,
				 descriptor,
				 COMPILE_EX_MLEN(data_regs),
				 0,
				 0);
	}

	/* The address payload and the sum and carry registers are free again. */
	state->grf_busy[payload] = 0U;
	state->grf_busy[payload + 1U] = 0U;
	state->grf_busy[sum] = 0U;
	state->grf_busy[carry] = 0U;

	/* So is a compare and exchange's run of two data values. */
	if (inst->immediate == DRV_GPU_IR_ATOMIC_CMPXCHG) {
		state->grf_busy[data_grf] = 0U;
		state->grf_busy[data_grf + 1U] = 0U;
	}
}

/*
 * Ends a compute thread: a copy of the header, for all eight channels, sent
 * to the thread spawner with the end-of-thread bit (the form of the kernel
 * of tests/execution/eu-test.c).
 */
static void
i915_compile_terminate_compute(
	struct i915_compile_state *state)
{
	/* Copies the header into the last register, outside the mask. */
	drv_i915_eu_mov_all(&state->code, drv_i915_eu_grf_ud(COMPILE_CS_END_GRF), drv_i915_eu_grf_ud(0U));

	/* Retires the thread. */
	drv_i915_eu_send_all_end(&state->code, drv_i915_eu_grf(COMPILE_CS_END_GRF), EU_SFID_THREAD_SPAWNER, EU_DESC_TERMINATE);
}

/*
 * Records what a dispatch has to program around a compute kernel: the
 * workgroup size, and the push data the threads read -- the cross-thread
 * data from r1, then the per-thread IDs.
 */
static void
i915_compile_describe_compute(
	const struct i915_compile_state *state,
	struct i915_shader_binary *binary)
{
	int threads;

	/* The push data starts right after the header. */
	binary->dispatch_grf_start = COMPILE_CS_PUSH_GRF;

	/* The workgroup's size along each axis. */
	binary->local_size[0] = state->ir->local_size[0];
	binary->local_size[1] = state->ir->local_size[1];
	binary->local_size[2] = state->ir->local_size[2];

	/* Every thread reads the push data, then its own IDs. */
	binary->cross_thread_regs = state->push_regs;
	binary->per_thread_regs = I915_SHADER_PER_THREAD_REGS;

	/* The group's shared memory (ws101-p006). */
	binary->shared_bytes = state->ir->shared_bytes;

	/* How many threads a group takes, which decides whether a barrier sends its message. */
	threads = i915_compile_group_threads(state);

	/* The barrier, when the kernel has one and a group has more than one thread to wait for. */
	binary->uses_barrier = 0U;
	if (state->ir->uses_barrier != 0U && threads > 1)
		binary->uses_barrier = 1U;
}

/*
 * Returns how many threads a group of the kernel takes: its invocations,
 * eight to a SIMD8 thread.
 */
static int
i915_compile_group_threads(
	const struct i915_compile_state *state)
{
	uint32_t invocations;

	/* The invocations of one group. */
	invocations = state->ir->local_size[0] * state->ir->local_size[1] * state->ir->local_size[2];

	/* Reports the invocations rounded up to whole threads of eight. */
	return (int)((invocations + 7U) / 8U);
}

/*
 * Lowers LOAD_SHARED and STORE_SHARED (ws101-p006): an untyped surface read
 * or write of one channel's dword at binding table entry 254 (Mesa's
 * brw_dp_untyped_surface_rw_desc(), SIMD8), the byte offsets as the one
 * address register.  A predicated access runs for the channels whose
 * predicate is not zero; a load's other channels keep what the register
 * held.
 */
static void
i915_compile_shared(
	struct i915_compile_state *state,
	const struct drv_gpu_shader_ir_inst *inst)
{
	struct i915_eu_reg null;
	struct i915_eu_reg dst;
	struct i915_eu_reg data;
	uint32_t offset_grf;
	uint32_t data_grf;
	uint32_t predicate_grf;
	uint32_t predicate_operand;
	uint32_t dst_grf;
	uint32_t descriptor;
	uint32_t ex_descriptor;
	uint32_t control;

	/* Reads the register of each channel's byte offset. */
	offset_grf = i915_compile_grf(state, inst->src[0]);

	/* A store sends its word, the second operand; a load sends none, and its predicate is the second operand. */
	data = drv_i915_eu_null();
	predicate_operand = 1U;
	if (inst->op == DRV_GPU_IR_STORE_SHARED) {
		data_grf = i915_compile_grf(state, inst->src[1]);
		data = drv_i915_eu_grf(data_grf);
		predicate_operand = 2U;
	}

	/* A predicated access reads its predicate after the other operands. */
	predicate_grf = 0U;
	if (inst->component != 0U)
		predicate_grf = i915_compile_grf(state, inst->src[predicate_operand]);

	/* The message: one address register, and for a load one reply register, for a store one data register. */
	control = EU_DP_UNTYPED_ONE_CHANNEL | EU_DP_UNTYPED_SIMD8;
	descriptor = (1U << EU_DESC_MLEN_SHIFT) | (control << EU_DP_CONTROL_SHIFT) | EU_BTI_SLM;
	ex_descriptor = 0U;
	dst = drv_i915_eu_null();
	if (inst->op == DRV_GPU_IR_LOAD_SHARED) {
		descriptor |= (1U << EU_DESC_RLEN_SHIFT) | (EU_DP_UNTYPED_READ << EU_DP_TYPE_SHIFT);
		dst_grf = i915_compile_define(state, inst->dst, 1U);
		dst = drv_i915_eu_grf(dst_grf);
	} else {
		descriptor |= EU_DP_UNTYPED_WRITE << EU_DP_TYPE_SHIFT;
		ex_descriptor = COMPILE_EX_MLEN(1U);
	}

	/* An access outside every predicate runs for every channel. */
	if (inst->component == 0U) {
		drv_i915_eu_send(&state->code,
				 dst,
				 drv_i915_eu_grf(offset_grf),
				 data,
				 COMPILE_SFID_DATA_CACHE_1,
				 descriptor,
				 ex_descriptor,
				 0,
				 0);
		return;
	}

	/* A predicated one runs where the predicate is not zero. */
	null = drv_i915_eu_null();
	null.type = COMPILE_TYPE_D;
	drv_i915_eu_cmp(&state->code,
			I915_EU_COND_NE,
			I915_EU_FLAG_F0_0,
			0,
			null,
			drv_i915_eu_grf_d(predicate_grf),
			drv_i915_eu_imm_d(0U));
	drv_i915_eu_send_masked(&state->code,
				I915_EU_FLAG_F0_0,
				dst,
				drv_i915_eu_grf(offset_grf),
				data,
				COMPILE_SFID_DATA_CACHE_1,
				descriptor,
				ex_descriptor,
				0,
				0);
}

/*
 * Lowers ATOMIC on a word of shared memory (ws101-p006): one untyped atomic
 * message at binding table entry 254 of the channels that run it (Mesa's
 * brw_dp_untyped_atomic_desc(), SIMD8), the byte offsets as the one address
 * register and the value -- or the comparator then the new value -- as the
 * data run.  The old value comes back only when an instruction reads it.
 */
static void
i915_compile_atomic_shared(
	struct i915_compile_state *state,
	const struct drv_gpu_shader_ir_inst *inst)
{
	struct i915_eu_reg null;
	struct i915_eu_reg reply;
	uint32_t descriptor;
	uint32_t control;
	uint32_t offset_grf;
	uint32_t value_grf;
	uint32_t compare_grf;
	uint32_t predicate_grf;
	uint32_t data_grf;
	uint32_t data_regs;
	uint32_t operands;
	uint32_t dst;
	int returns;

	/* Reads the operands' registers: the offset and the value. */
	operands = i915_compile_operands(inst);
	offset_grf = i915_compile_grf(state, inst->src[0]);
	value_grf = i915_compile_grf(state, inst->src[1]);

	/* A compare and exchange also reads its comparator. */
	compare_grf = 0U;
	if (inst->immediate == DRV_GPU_IR_ATOMIC_CMPXCHG)
		compare_grf = i915_compile_grf(state, inst->src[2]);

	/* A predicated atomic reads its predicate, the last operand. */
	predicate_grf = 0U;
	if (inst->component != 0U)
		predicate_grf = i915_compile_grf(state, inst->src[operands - 1U]);

	/* The old value is asked for only when a later instruction reads it. */
	returns = 0;
	if (state->last_use[inst->dst] > state->index)
		returns = 1;

	/* The result gets its register, which the reply fills when the old value is asked for. */
	dst = i915_compile_define(state, inst->dst, 1U);

	/* A compare and exchange sends the comparator, then the new value, in a run of two. */
	data_grf = value_grf;
	data_regs = 1U;
	if (inst->immediate == DRV_GPU_IR_ATOMIC_CMPXCHG) {
		data_grf = i915_compile_temporaries(state, 2U);
		if (state->out_of_registers != 0)
			return;
		data_regs = 2U;

		/* Copies the comparator and the new value into the run. */
		drv_i915_eu_mov(&state->code, drv_i915_eu_grf_ud(data_grf), drv_i915_eu_grf_ud(compare_grf));
		drv_i915_eu_mov(&state->code, drv_i915_eu_grf_ud(data_grf + 1U), drv_i915_eu_grf_ud(value_grf));
	}

	/* The message: one address register, the data run, and one reply register when the old value is read. */
	control = inst->immediate | EU_DP_ATOMIC_SIMD8;
	descriptor = (1U << EU_DESC_MLEN_SHIFT) |
		     (EU_DP_UNTYPED_ATOMIC << EU_DP_TYPE_SHIFT) |
		     EU_BTI_SLM;
	reply = drv_i915_eu_null();
	if (returns != 0) {
		control |= EU_DP_ATOMIC_RETURN;
		descriptor |= 1U << EU_DESC_RLEN_SHIFT;
		reply = drv_i915_eu_grf(dst);
	}

	/* The message control -- the operation, SIMD8 and the reply bit -- goes into the descriptor. */
	descriptor |= control << EU_DP_CONTROL_SHIFT;

	/* A predicated atomic changes the word only for the channels whose predicate is not zero. */
	if (inst->component != 0U) {
		null = drv_i915_eu_null();
		null.type = COMPILE_TYPE_D;
		drv_i915_eu_cmp(&state->code,
				I915_EU_COND_NE,
				I915_EU_FLAG_F0_0,
				0,
				null,
				drv_i915_eu_grf_d(predicate_grf),
				drv_i915_eu_imm_d(0U));
		drv_i915_eu_send_masked(&state->code,
					I915_EU_FLAG_F0_0,
					reply,
					drv_i915_eu_grf(offset_grf),
					drv_i915_eu_grf(data_grf),
					EU_SFID_DATA_CACHE_1,
					descriptor,
					COMPILE_EX_MLEN(data_regs),
					0,
					0);
	} else {
		/* An atomic outside every predicate changes the word for every channel. */
		drv_i915_eu_send(&state->code,
				 reply,
				 drv_i915_eu_grf(offset_grf),
				 drv_i915_eu_grf(data_grf),
				 EU_SFID_DATA_CACHE_1,
				 descriptor,
				 COMPILE_EX_MLEN(data_regs),
				 0,
				 0);
	}

	/* The temporaries are free again. */
	if (inst->immediate == DRV_GPU_IR_ATOMIC_CMPXCHG) {
		state->grf_busy[data_grf] = 0U;
		state->grf_busy[data_grf + 1U] = 0U;
	}
}

/*
 * Lowers the fences `fences` names (DRV_GPU_IR_FENCE_*, ws101-p006): a memory
 * fence message of the data cache (binding table entry 0) for buffers and
 * one of shared memory (254) for shared memory, each SIMD1 of r0 with the
 * commit bit; the encoder waits for each reply before going on, so the
 * thread's earlier accesses are done when the next instruction runs.  An
 * acquire needs no more: nothing after a fence starts before its reply.
 */
static void
i915_compile_fence(
	struct i915_compile_state *state,
	uint32_t fences)
{
	uint32_t descriptor;
	uint32_t reply;

	/* Nothing to order. */
	if ((fences & (DRV_GPU_IR_FENCE_GLOBAL | DRV_GPU_IR_FENCE_SHARED)) == 0U)
		return;

	/* A register for the replies. */
	reply = i915_compile_temporary(state);
	if (state->out_of_registers != 0)
		return;

	/* The message: r0 as the header, one reply register, the fence with commit. */
	descriptor = (1U << EU_DESC_MLEN_SHIFT) |
		     (1U << EU_DESC_RLEN_SHIFT) |
		     EU_DESC_HEADER_PRESENT |
		     (EU_DP_MEMORY_FENCE << EU_DP_TYPE_SHIFT) |
		     (EU_DP_FENCE_COMMIT << EU_DP_CONTROL_SHIFT);

	/* The buffers' fence, at the data cache's entry. */
	if ((fences & DRV_GPU_IR_FENCE_GLOBAL) != 0U)
		drv_i915_eu_send_scalar(&state->code, drv_i915_eu_grf(reply), drv_i915_eu_grf(0U), EU_SFID_DATA_CACHE, descriptor | EU_BTI_DATA_CACHE);

	/* Then shared memory's, at the shared memory's entry. */
	if ((fences & DRV_GPU_IR_FENCE_SHARED) != 0U)
		drv_i915_eu_send_scalar(&state->code, drv_i915_eu_grf(reply), drv_i915_eu_grf(0U), EU_SFID_DATA_CACHE, descriptor | EU_BTI_SLM);

	/* The register is free again. */
	state->grf_busy[reply] = 0U;
}

/*
 * Lowers BARRIER (ws101-p006): its fences, then -- for a group of more than
 * one thread -- the gateway's barrier message, whose payload is zero but
 * for dword 2, the group's barrier ID from r0.2, and sync.bar, which waits
 * until every thread of the group has sent its message.  The message goes
 * for the whole thread, outside the execution mask: a thread whose channels
 * are all disabled still waits once.  A group of one thread runs in order
 * and needs no message (Mesa leaves it out too).
 */
static void
i915_compile_barrier(
	struct i915_compile_state *state,
	const struct drv_gpu_shader_ir_inst *inst)
{
	struct i915_eu_reg dword;
	struct i915_eu_reg identity;
	uint32_t payload;
	int threads;

	/* The fences before the barrier. */
	i915_compile_fence(state, inst->immediate);

	/* A group of one thread waits for nobody. */
	threads = i915_compile_group_threads(state);
	if (threads <= 1)
		return;

	/* The payload: cleared, then the barrier ID in dword 2. */
	payload = i915_compile_temporary(state);
	if (state->out_of_registers != 0)
		return;
	drv_i915_eu_mov_all(&state->code, drv_i915_eu_grf_ud(payload), drv_i915_eu_imm_ud(0U));
	dword = drv_i915_eu_grf_ud(payload);
	dword.subnr = 4U * EU_BARRIER_ID_DWORD;
	identity = drv_i915_eu_grf_scalar(0U, 4U * EU_BARRIER_ID_DWORD);
	identity.type = COMPILE_TYPE_UD;
	drv_i915_eu_alu2_scalar(&state->code, I915_EU_AND, dword, identity, drv_i915_eu_imm_ud(EU_BARRIER_ID_MASK));

	/* The gateway message for the whole thread, then the wait for the group. */
	drv_i915_eu_send_all(&state->code,
			     drv_i915_eu_null(),
			     drv_i915_eu_grf(payload),
			     drv_i915_eu_null(),
			     EU_SFID_GATEWAY,
			     (1U << EU_DESC_MLEN_SHIFT) | EU_GATEWAY_BARRIER,
			     0U);
	drv_i915_eu_sync_function(&state->code, EU_SYNC_BAR);

	/* The register is free again. */
	state->grf_busy[payload] = 0U;
}
