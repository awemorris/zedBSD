/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The lowering of a compute program's own operations, shared by the V3D
 * versions: the invocation's built-in values, storage buffer loads and
 * stores through the general memory TMU, and the integer multiply,
 * comparisons and divisions its addressing needs.
 *
 * Every operation is written with the semantic instruction interface of
 * qpu.h; the version's lowering file supplies only the payload registers.
 * Every address a program computes is clamped into its buffer
 * (base + umin(offset, limit)), because one GPU page table serves every
 * session: an unclamped offset would reach another session's memory.
 *
 * Included by shader.c, which both versions build.
 */

/*
 * The magic write addresses of the general memory TMU, the discarding
 * address, and the predicate and flag push numbers of the semantic
 * instruction (qpu.h).
 *
 * Values referenced: Mesa 26.2.6 src/broadcom/qpu/qpu_instr.h lines 99-115
 * (V3D_QPU_WADDR_NOP, TMUD, TMUA; the same on 4.2 and 7.1).
 */
#define COMPUTE_DISCARD 6U
#define COMPUTE_TMUD 11U
#define COMPUTE_TMUA 12U
#define COMPUTE_TMUAU 13U
#define COMPUTE_SYNCB 18U
#define COMPUTE_IF_A 1U
#define COMPUTE_IF_NOT_A 3U
#define COMPUTE_PUSH_ZERO 1U
#define COMPUTE_PUSH_CARRY 3U

/*
 * The configuration word of a general TMU access that names an operation:
 * the top bits set, the operation at bit 3, per-pixel at bit 7 and the
 * type in the low bits (a 32-bit word, or the two words of a compare and
 * exchange); and the operations of the atomics.
 *
 * Values referenced: Mesa 26.2.6 src/broadcom/compiler/nir_to_vir.c lines
 * 43-52 and 641-685 (the configuration) and src/broadcom/cle/v3d_packet.xml
 * (TMU Op: add 0, sub 1, xchg 2, cmpxchg 3, umin 4, umax 5, smin 6, smax 7,
 * and 8, or 9, xor 10).
 */
#define COMPUTE_CONFIG_BASE 0xffffff00U
#define COMPUTE_CONFIG_OPERATION_SHIFT 3
#define COMPUTE_CONFIG_PER_PIXEL 0x80U
#define COMPUTE_CONFIG_WORD 7U
#define COMPUTE_CONFIG_PAIR 2U
#define COMPUTE_ATOMIC_ADD 0U
#define COMPUTE_ATOMIC_SUB 1U
#define COMPUTE_ATOMIC_XCHG 2U
#define COMPUTE_ATOMIC_CMPXCHG 3U
#define COMPUTE_ATOMIC_UMIN 4U
#define COMPUTE_ATOMIC_UMAX 5U
#define COMPUTE_ATOMIC_SMIN 6U
#define COMPUTE_ATOMIC_SMAX 7U
#define COMPUTE_ATOMIC_AND 8U
#define COMPUTE_ATOMIC_OR 9U
#define COMPUTE_ATOMIC_XOR 10U

/* The most TMU operations a thread may have outstanding (Mesa nir_to_vir.c MAX_TMU_QUEUE_SIZE), less the load about to be made. */
#define COMPUTE_STORES_BEFORE_WAIT 7U

/*
 * The factor that makes a quotient estimate an underestimate: 1 - 2^-12 as
 * float bits.  It covers a reciprocal error of up to about 2^-12 (the
 * special function's accuracy is not documented); four refinement steps
 * then leave a remainder below twice the divisor for every 32-bit operand.
 */
#define COMPUTE_UNDERESTIMATE 0x3f7ff000U
#define COMPUTE_DIVIDE_STEPS 4U

static int lower_system(struct bcm2711_shader_compiler *compiler, const struct drv_gpu_shader_ir_inst *instruction, uint32_t destination);
static int lower_local_id(struct bcm2711_shader_compiler *compiler, uint32_t axis, uint32_t destination);
static int lower_group_id(struct bcm2711_shader_compiler *compiler, uint32_t axis, uint32_t destination);
static int lower_load(struct bcm2711_shader_compiler *compiler, const struct drv_gpu_shader_ir_inst *instruction, uint32_t destination);
static int lower_store(struct bcm2711_shader_compiler *compiler, const struct drv_gpu_shader_ir_inst *instruction);
static int lower_atomic(struct bcm2711_shader_compiler *compiler, const struct drv_gpu_shader_ir_inst *instruction, uint32_t destination);
static int atomic_operation(uint32_t operation, uint32_t *native);
static int lower_loop_begin(struct bcm2711_shader_compiler *compiler);
static int lower_loop_end(struct bcm2711_shader_compiler *compiler, const struct drv_gpu_shader_ir_inst *instruction);
static int lower_redefinition(struct bcm2711_shader_compiler *compiler, const struct drv_gpu_shader_ir_inst *instruction);
static void forget_constants(struct bcm2711_shader_compiler *compiler);
static int lower_multiply(struct bcm2711_shader_compiler *compiler, const struct drv_gpu_shader_ir_inst *instruction, uint32_t destination);
static int lower_compare(struct bcm2711_shader_compiler *compiler, const struct drv_gpu_shader_ir_inst *instruction, uint32_t destination);
static int lower_divide(struct bcm2711_shader_compiler *compiler, const struct drv_gpu_shader_ir_inst *instruction, uint32_t destination);
static int divide_unsigned(struct bcm2711_shader_compiler *compiler, uint32_t numerator, uint32_t divisor, uint32_t quotient, uint32_t remainder);
static int divide_small(struct bcm2711_shader_compiler *compiler, uint32_t numerator, uint32_t divisor, uint32_t quotient, uint32_t remainder);
static int storage_address(struct bcm2711_shader_compiler *compiler, const struct drv_gpu_shader_ir_inst *instruction, uint32_t address);
static int shared_address(struct bcm2711_shader_compiler *compiler, const struct drv_gpu_shader_ir_inst *instruction, uint32_t address);
static int access_address(struct bcm2711_shader_compiler *compiler, const struct drv_gpu_shader_ir_inst *instruction, uint32_t address);
static int lower_barrier(struct bcm2711_shader_compiler *compiler, const struct drv_gpu_shader_ir_inst *instruction);
static int wait_stores(struct bcm2711_shader_compiler *compiler);
static int storage_uniform(struct bcm2711_shader_compiler *compiler, enum bcm2711_shader_uniform_kind kind, const struct drv_gpu_shader_ir_uniform *buffer, uint32_t bits, uint32_t destination);
static int tmu_room(struct bcm2711_shader_compiler *compiler);
static int temporary(struct bcm2711_shader_compiler *compiler, uint32_t *number);
static int value_register(struct bcm2711_shader_compiler *compiler, uint32_t value, uint32_t *number);
static int bits_source(struct bcm2711_shader_compiler *compiler, uint32_t bits, uint32_t scratch, struct bcm2711_qpu_source *source);
static int emit(struct bcm2711_shader_compiler *compiler, enum bcm2711_qpu_operation operation, uint32_t destination, struct bcm2711_qpu_source left, struct bcm2711_qpu_source right);
static int emit_magic(struct bcm2711_shader_compiler *compiler, enum bcm2711_qpu_operation operation, uint32_t address, struct bcm2711_qpu_source left, struct bcm2711_qpu_source right, uint32_t predicate, uint32_t flags);
static int emit_predicated(struct bcm2711_shader_compiler *compiler, enum bcm2711_qpu_operation operation, uint32_t destination, struct bcm2711_qpu_source left, struct bcm2711_qpu_source right, uint32_t predicate);
static uint32_t shift_bits(uint32_t shift);
static uint32_t power_of_two(uint32_t value, uint32_t *log);

/* The storage image accesses (shader-image.inc). */
static int lower_image_read(struct bcm2711_shader_compiler *compiler, const struct drv_gpu_shader_ir_inst *instruction);
static int lower_image_write(struct bcm2711_shader_compiler *compiler, const struct drv_gpu_shader_ir_inst *instruction);
static int image_address(struct bcm2711_shader_compiler *compiler, const struct drv_gpu_shader_ir_inst *instruction, uint32_t bytes, uint32_t address);
static int image_unpack(struct bcm2711_shader_compiler *compiler, uint32_t format, const uint32_t *words, uint32_t index, uint32_t destination);
static int image_half(struct bcm2711_shader_compiler *compiler, uint32_t format, struct bcm2711_qpu_source word, uint32_t high, uint32_t destination);
static int image_byte(struct bcm2711_shader_compiler *compiler, uint32_t format, struct bcm2711_qpu_source word, uint32_t index, uint32_t destination);
static int image_pack(struct bcm2711_shader_compiler *compiler, uint32_t format, uint32_t first, uint32_t index, uint32_t word);
static int image_integer(struct bcm2711_shader_compiler *compiler, uint32_t format, uint32_t value, uint32_t shift, uint32_t word);
static int image_normalized(struct bcm2711_shader_compiler *compiler, uint32_t format, uint32_t value, uint32_t shift, uint32_t word);
static int image_place(struct bcm2711_shader_compiler *compiler, uint32_t shift, uint32_t word);

/*
 * Lowers one compute operation, or reports that the instruction is not one.
 */
int
bcm2711_shader_compute_lower(
	struct bcm2711_shader_compiler *compiler,
	const struct drv_gpu_shader_ir_inst *instruction,
	uint32_t *handled)
{
	struct bcm2711_shader_value *scalar;
	uint32_t destination;
	int error;

	/* A store and the loop markers define no value; they are lowered before a destination would be allocated. */
	*handled = 1;
	if (instruction->op == DRV_GPU_IR_BARRIER || instruction->op == DRV_GPU_IR_FENCE) {
		error = lower_barrier(compiler, instruction);
		if (error != 0)
			return error;
		return 0;
	}
	if (instruction->op == DRV_GPU_IR_STORE_STORAGE || instruction->op == DRV_GPU_IR_STORE_SHARED) {
		error = lower_store(compiler, instruction);
		if (error != 0)
			return error;
		return 0;
	}

	/* A storage image's store defines nothing, its load four values. */
	if (instruction->op == DRV_GPU_IR_IMAGE_WRITE) {
		error = lower_image_write(compiler, instruction);
		if (error != 0)
			return error;
		return 0;
	}
	if (instruction->op == DRV_GPU_IR_IMAGE_READ) {
		error = lower_image_read(compiler, instruction);
		if (error != 0)
			return error;
		return 0;
	}
	if (instruction->op == DRV_GPU_IR_LOOP_BEGIN) {
		error = lower_loop_begin(compiler);
		if (error != 0)
			return error;
		return 0;
	}
	if (instruction->op == DRV_GPU_IR_LOOP_END) {
		error = lower_loop_end(compiler, instruction);
		if (error != 0)
			return error;
		return 0;
	}

	/* A loop variable moved into again writes the register it already has. */
	if (instruction->op == DRV_GPU_IR_MOVE && compiler->values[instruction->dst].register_live != 0) {
		error = lower_redefinition(compiler, instruction);
		if (error != 0)
			return error;
		return 0;
	}

	/* The other compute operations each define one value. */
	switch (instruction->op) {
	case DRV_GPU_IR_LOAD_SYSTEM:
	case DRV_GPU_IR_LOAD_STORAGE:
	case DRV_GPU_IR_LOAD_SHARED:
	case DRV_GPU_IR_STORAGE_SIZE:
	case DRV_GPU_IR_ATOMIC:
	case DRV_GPU_IR_IMUL:
	case DRV_GPU_IR_UDIV:
	case DRV_GPU_IR_UMOD:
	case DRV_GPU_IR_IDIV:
	case DRV_GPU_IR_IREM:
	case DRV_GPU_IR_ILT:
	case DRV_GPU_IR_IGE:
	case DRV_GPU_IR_ULT:
	case DRV_GPU_IR_UGE:
	case DRV_GPU_IR_IEQ:
	case DRV_GPU_IR_INE:
		break;
	default:
		*handled = 0;
		return 0;
	}

	/* The destination cannot overwrite a still-live source register. */
	error = bcm2711_shader_register(compiler, &destination);
	if (error != 0)
		return error;
	scalar = &compiler->values[instruction->dst];
	scalar->number = destination;
	scalar->register_live = 1;

	/* Each operation has its own lowering. */
	switch (instruction->op) {
	case DRV_GPU_IR_LOAD_SYSTEM:
		error = lower_system(compiler, instruction, destination);
		break;
	case DRV_GPU_IR_LOAD_STORAGE:
	case DRV_GPU_IR_LOAD_SHARED:
		error = lower_load(compiler, instruction, destination);
		break;
	case DRV_GPU_IR_STORAGE_SIZE:
		error = storage_uniform(compiler, BCM2711_SHADER_STORAGE_SIZE, &compiler->ir->uniforms[instruction->location], 0, destination);
		break;
	case DRV_GPU_IR_ATOMIC:
		error = lower_atomic(compiler, instruction, destination);
		break;
	case DRV_GPU_IR_IMUL:
		error = lower_multiply(compiler, instruction, destination);
		break;
	case DRV_GPU_IR_UDIV:
	case DRV_GPU_IR_UMOD:
	case DRV_GPU_IR_IDIV:
	case DRV_GPU_IR_IREM:
		error = lower_divide(compiler, instruction, destination);
		break;
	default:
		error = lower_compare(compiler, instruction, destination);
		break;
	}

	/* A refused lowering publishes nothing. */
	if (error != 0)
		return error;

	/* Succeeded: the operation is lowered. */
	return 0;
}

/*
 * Ends a compute program: its stores are waited for, and a program with a
 * thread switch marks its last one before the thread end.
 */
int
bcm2711_shader_compute_finish(
	struct bcm2711_shader_compiler *compiler)
{
	struct bcm2711_qpu_source none;
	int error;

	/* The stores complete before the thread ends; TMUWT keeps its two idle words, so it is never the last word. */
	none = bcm2711_shader_bits(0);
	if (compiler->pending_stores != 0) {
		error = emit_magic(compiler, BCM2711_QPU_TEXTURE_WAIT, COMPUTE_DISCARD, none, none, 0, 0);
		if (error != 0)
			return error;
		compiler->pending_stores = 0;
	}

	/* A program with no switch is a single segment whose first switch ends it. */
	if (compiler->switches == 0)
		return 0;

	/* Otherwise two switches in a row mark the last one, which the thread end follows. */
	error = bcm2711_shader_switch(compiler, BCM2711_SHADER_SWITCH_LAST_FIRST);
	if (error != 0)
		return error;
	error = bcm2711_shader_switch(compiler, BCM2711_SHADER_SWITCH_LAST_SECOND);
	if (error != 0)
		return error;

	/* Succeeded: the thread end may follow. */
	return 0;
}

/*
 * Waits for a graphics program's storage buffer stores, which complete
 * before it ends.
 */
int
bcm2711_shader_storage_wait(
	struct bcm2711_shader_compiler *compiler)
{
	int error;

	/* The outstanding stores, if any. */
	error = wait_stores(compiler);
	if (error != 0)
		return error;

	/* Succeeded: the stores reached memory. */
	return 0;
}

/* Lowers one built-in value of the invocation. */
static int
lower_system(
	struct bcm2711_shader_compiler *compiler,
	const struct drv_gpu_shader_ir_inst *instruction,
	uint32_t destination)
{
	struct bcm2711_qpu_source payload;
	struct bcm2711_qpu_source shift;
	uint32_t size;
	uint32_t bits;
	int error;

	/* The workgroup ids and the local ids have lowerings of their own. */
	switch (instruction->component) {
	case DRV_GPU_IR_SYSTEM_GROUP_ID_X:
	case DRV_GPU_IR_SYSTEM_GROUP_ID_Y:
	case DRV_GPU_IR_SYSTEM_GROUP_ID_Z:
		error = lower_group_id(compiler, instruction->component - DRV_GPU_IR_SYSTEM_GROUP_ID_X, destination);
		if (error != 0)
			return error;
		return 0;
	case DRV_GPU_IR_SYSTEM_LOCAL_ID_X:
	case DRV_GPU_IR_SYSTEM_LOCAL_ID_Y:
	case DRV_GPU_IR_SYSTEM_LOCAL_ID_Z:
		error = lower_local_id(compiler, instruction->component, destination);
		if (error != 0)
			return error;
		return 0;
	default:
		break;
	}

	/*
	 * The local invocation index is the top b bits of the second payload
	 * word, b = log2 of the next power of two of max(size, 64) (Mesa 26.2.6
	 * src/broadcom/compiler/nir_to_vir.c lines 3464-3467 and 4907-4914).
	 */
	size = compiler->binary->local_size[0] * compiler->binary->local_size[1] * compiler->binary->local_size[2];
	if (size == 1) {
		/* A one-invocation workgroup's index is zero; its payload's top bits belong to the memory slot. */
		error = emit(compiler, BCM2711_QPU_MOVE, destination, bcm2711_shader_bits(0), bcm2711_shader_bits(0));
		if (error != 0)
			return error;
		return 0;
	}
	bits = 6;
	while ((1U << bits) < size)
		bits++;
	payload = bcm2711_shader_rf(bcm2711_shader_compute_payload(1));
	shift = bcm2711_shader_bits(shift_bits(32U - bits));
	error = emit(compiler, BCM2711_QPU_SHIFT_RIGHT, destination, payload, shift);
	if (error != 0)
		return error;

	/* Succeeded: the index is in the destination. */
	return 0;
}

/*
 * Lowers one local invocation id from the local index: x = index mod sx,
 * y = (index / sx) mod sy, z = index / (sx * sy).
 */
static int
lower_local_id(
	struct bcm2711_shader_compiler *compiler,
	uint32_t component,
	uint32_t destination)
{
	struct drv_gpu_shader_ir_inst index_load;
	struct bcm2711_qpu_source zero;
	uint32_t index;
	uint32_t below;
	uint32_t size;
	uint32_t quotient;
	int error;

	/* An axis of size one has id zero. */
	size = compiler->binary->local_size[component - DRV_GPU_IR_SYSTEM_LOCAL_ID_X];
	zero = bcm2711_shader_bits(0);
	if (size == 1) {
		error = emit(compiler, BCM2711_QPU_MOVE, destination, zero, zero);
		if (error != 0)
			return error;
		return 0;
	}

	/* The local index in a temporary register. */
	error = temporary(compiler, &index);
	if (error != 0)
		return error;
	kern_memset(&index_load, 0, sizeof(index_load));
	index_load.op = DRV_GPU_IR_LOAD_SYSTEM;
	index_load.component = DRV_GPU_IR_SYSTEM_LOCAL_INDEX;
	error = lower_system(compiler, &index_load, index);
	if (error != 0)
		return error;

	/* The invocations of the lower axes before this one. */
	below = 1;
	if (component >= DRV_GPU_IR_SYSTEM_LOCAL_ID_Y)
		below *= compiler->binary->local_size[0];
	if (component >= DRV_GPU_IR_SYSTEM_LOCAL_ID_Z)
		below *= compiler->binary->local_size[1];

	/* The index divided by the lower axes' size, then taken modulo this axis' size (z needs no modulo). */
	quotient = index;
	if (below != 1) {
		error = temporary(compiler, &quotient);
		if (error != 0)
			return error;
		error = divide_small(compiler, index, below, quotient, BCM2711_SHADER_ABSENT);
		if (error != 0)
			return error;
	}
	if (component == DRV_GPU_IR_SYSTEM_LOCAL_ID_Z) {
		error = emit(compiler, BCM2711_QPU_MOVE, destination, bcm2711_shader_rf(quotient), zero);
		if (error != 0)
			return error;
		return 0;
	}
	error = divide_small(compiler, quotient, size, BCM2711_SHADER_ABSENT, destination);
	if (error != 0)
		return error;

	/* Succeeded: the id is in the destination. */
	return 0;
}

/*
 * Lowers one workgroup id: the payload's id within the CSD job plus the
 * job's first workgroup, which a dispatch split into several jobs moves
 * (the CFG0 to CFG2 workgroup offsets stay zero).
 */
static int
lower_group_id(
	struct bcm2711_shader_compiler *compiler,
	uint32_t axis,
	uint32_t destination)
{
	struct bcm2711_shader_uniform uniform;
	struct bcm2711_qpu_source payload;
	struct bcm2711_qpu_source operand;
	uint32_t base;
	int error;

	/*
	 * x is the low half of the first payload word and y its high half, z
	 * the low half of the second (Mesa 26.2.6 nir_to_vir.c lines
	 * 3835-3847).
	 */
	payload = bcm2711_shader_rf(bcm2711_shader_compute_payload(0));
	if (axis == 2)
		payload = bcm2711_shader_rf(bcm2711_shader_compute_payload(1));
	if (axis == 1) {
		operand = bcm2711_shader_bits(shift_bits(16));
		error = emit(compiler, BCM2711_QPU_SHIFT_RIGHT, destination, payload, operand);
	} else {
		error = bits_source(compiler, 0xffffU, BCM2711_SHADER_SCRATCH_RIGHT, &operand);
		if (error != 0)
			return error;
		error = emit(compiler, BCM2711_QPU_BITS_AND, destination, payload, operand);
	}
	if (error != 0)
		return error;

	/* The job's first workgroup along the axis. */
	error = temporary(compiler, &base);
	if (error != 0)
		return error;
	kern_memset(&uniform, 0, sizeof(uniform));
	uniform.kind = BCM2711_SHADER_GROUP_BASE;
	uniform.bits = axis;
	error = bcm2711_shader_uniform_load(compiler, &uniform, base);
	if (error != 0)
		return error;
	error = emit(compiler, BCM2711_QPU_INTEGER_ADD, destination, bcm2711_shader_rf(destination), bcm2711_shader_rf(base));
	if (error != 0)
		return error;

	/* Succeeded: the id is in the destination. */
	return 0;
}

/*
 * Lowers a storage buffer load: the clamped address to TMUA, a thread
 * switch while the lookup runs, and the word read with ldtmu.
 *
 * The address write is unconditional even for a predicated load: the
 * clamped address stays in the buffer, and a lane outside the predicate
 * never reads the result.  A load of the dispatch's group counts is a
 * uniform instead.
 */
static int
lower_load(
	struct bcm2711_shader_compiler *compiler,
	const struct drv_gpu_shader_ir_inst *instruction,
	uint32_t destination)
{
	const struct drv_gpu_shader_ir_uniform *buffer;
	struct bcm2711_shader_value *offset;
	struct bcm2711_shader_uniform uniform;
	struct bcm2711_qpu_source none;
	uint32_t address;
	int error;

	/* The group counts: three words at constant offsets of the system buffer. */
	buffer = NULL;
	if (instruction->op == DRV_GPU_IR_LOAD_STORAGE)
		buffer = &compiler->ir->uniforms[instruction->location];
	if (buffer != NULL && buffer->set == DRV_GPU_IR_SYSTEM_SET) {
		offset = &compiler->values[instruction->src[0]];
		if (offset->constant == 0 || offset->number > 8 || (offset->number & 3U) != 0)
			return bcm2711_shader_fail(compiler, ENOTSUP, "group count read at a computed offset");
		kern_memset(&uniform, 0, sizeof(uniform));
		uniform.kind = BCM2711_SHADER_GROUP_COUNT;
		uniform.bits = offset->number / 4;
		error = bcm2711_shader_uniform_load(compiler, &uniform, destination);
		if (error != 0)
			return error;
		return 0;
	}

	/* Room in the TMU queue, then the clamped address. */
	error = tmu_room(compiler);
	if (error != 0)
		return error;
	error = temporary(compiler, &address);
	if (error != 0)
		return error;
	error = access_address(compiler, instruction, address);
	if (error != 0)
		return error;

	/* The address launches the lookup; the switch lets the other threads run until it completes. */
	none = bcm2711_shader_bits(0);
	error = emit_magic(compiler, BCM2711_QPU_MOVE, COMPUTE_TMUA, bcm2711_shader_rf(address), none, 0, 0);
	if (error != 0)
		return error;
	error = bcm2711_shader_switch(compiler, BCM2711_SHADER_SWITCH_SAMPLE);
	if (error != 0)
		return error;
	error = bcm2711_shader_signal(compiler, BCM2711_QPU_SIGNAL_TEXTURE, destination, 0);
	if (error != 0)
		return error;

	/* Succeeded: the word is in the destination. */
	return 0;
}

/*
 * Lowers a storage buffer store: the data to TMUD, then the clamped address
 * to TMUA.
 *
 * A predicated store writes both conditionally, and is jumped over when no
 * dispatched lane is in its predicate: what a conditional TMU write that
 * enables no lane does is not documented.  The branch's three delay words
 * idle, and the stores before it and the skipped store itself are waited
 * for, since no TMU operation may cross a block boundary.
 */
static int
lower_store(
	struct bcm2711_shader_compiler *compiler,
	const struct drv_gpu_shader_ir_inst *instruction)
{
	struct bcm2711_qpu_source none;
	struct bcm2711_qpu_source predicate;
	uint32_t address;
	uint32_t data;
	uint32_t record;
	uint32_t write_predicate;
	int error;

	/* Room in the TMU queue, the clamped address and the data, all before any branch. */
	error = tmu_room(compiler);
	if (error != 0)
		return error;
	error = temporary(compiler, &address);
	if (error != 0)
		return error;
	error = access_address(compiler, instruction, address);
	if (error != 0)
		return error;
	error = value_register(compiler, instruction->src[1], &data);
	if (error != 0)
		return error;

	/* An unpredicated store writes on every lane. */
	none = bcm2711_shader_bits(0);
	write_predicate = 0;
	record = BCM2711_SHADER_ABSENT;
	if (instruction->component == 1) {
		/* Flag A = the predicate is false; no stores are outstanding at the branch. */
		error = bcm2711_shader_operand(compiler, instruction->src[2], BCM2711_SHADER_SCRATCH_WORK, &predicate);
		if (error != 0)
			return error;
		error = emit_magic(compiler, BCM2711_QPU_MOVE, COMPUTE_DISCARD, predicate, none, 0, COMPUTE_PUSH_ZERO);
		if (error != 0)
			return error;
		if (compiler->pending_stores != 0) {
			error = emit_magic(compiler, BCM2711_QPU_TEXTURE_WAIT, COMPUTE_DISCARD, none, none, 0, 0);
			if (error != 0)
				return error;
			compiler->pending_stores = 0;
		}

		/* Jumps over the store when every dispatched lane is outside the predicate; three idle delay words. */
		error = bcm2711_shader_branch(compiler, BCM2711_QPU_BRANCH_ALL_A, &record);
		if (error != 0)
			return error;
		error = bcm2711_shader_nops(compiler, 3);
		if (error != 0)
			return error;
		write_predicate = COMPUTE_IF_NOT_A;
	}

	/* The data, then the address that sends the store. */
	error = emit_magic(compiler, BCM2711_QPU_MOVE, COMPUTE_TMUD, bcm2711_shader_rf(data), none, write_predicate, 0);
	if (error != 0)
		return error;
	error = emit_magic(compiler, BCM2711_QPU_MOVE, COMPUTE_TMUA, bcm2711_shader_rf(address), none, write_predicate, 0);
	if (error != 0)
		return error;
	compiler->pending_stores++;

	/* No TMU operation crosses into the block the branch lands in: the skipped store is waited for inside, then the branch lands. */
	if (record != BCM2711_SHADER_ABSENT) {
		error = emit_magic(compiler, BCM2711_QPU_TEXTURE_WAIT, COMPUTE_DISCARD, none, none, 0, 0);
		if (error != 0)
			return error;
		compiler->pending_stores = 0;
		bcm2711_shader_branch_target(compiler, record);
	}

	/* Succeeded: the store is sent. */
	return 0;
}

/*
 * Lowers an atomic on a word of a storage buffer or of shared memory: the
 * data (for a compare and exchange the comparator, then the new word) to
 * TMUD, then the clamped address to TMUAU with the configuration that
 * names the operation; the old word comes back like a load's.
 *
 * A predicated atomic writes conditionally and is jumped over, its read
 * included, when no dispatched lane is in its predicate, as a predicated
 * store is; the jump moves the uniform stream past the configuration word
 * it skips.  Values referenced: Mesa 26.2.6
 * src/broadcom/compiler/nir_to_vir.c lines 405-425 and 439-500 (the data
 * words and the configured address write) and
 * src/compiler/spirv/spirv_to_nir.c lines 4311-4312 (the comparator first).
 */
static int
lower_atomic(
	struct bcm2711_shader_compiler *compiler,
	const struct drv_gpu_shader_ir_inst *instruction,
	uint32_t destination)
{
	struct bcm2711_shader_uniform uniform;
	struct bcm2711_qpu_source none;
	struct bcm2711_qpu_source predicate;
	uint32_t operation;
	uint32_t address;
	uint32_t data;
	uint32_t comparator;
	uint32_t record;
	uint32_t write_predicate;
	uint32_t predicate_source;
	int error;

	/* The operation the TMU performs. */
	error = atomic_operation(instruction->immediate, &operation);
	if (error != 0)
		return bcm2711_shader_fail(compiler, error, "atomic operation without a TMU operation");

	/* Room in the TMU queue, the clamped address and the data, all before any branch. */
	error = tmu_room(compiler);
	if (error != 0)
		return error;
	error = temporary(compiler, &address);
	if (error != 0)
		return error;
	error = access_address(compiler, instruction, address);
	if (error != 0)
		return error;
	error = value_register(compiler, instruction->src[1], &data);
	if (error != 0)
		return error;
	comparator = BCM2711_SHADER_ABSENT;
	predicate_source = 2;
	if (operation == COMPUTE_ATOMIC_CMPXCHG) {
		error = value_register(compiler, instruction->src[2], &comparator);
		if (error != 0)
			return error;
		predicate_source = 3;
	}

	/* An unpredicated atomic runs on every lane. */
	none = bcm2711_shader_bits(0);
	write_predicate = 0;
	record = BCM2711_SHADER_ABSENT;
	if (instruction->component == DRV_GPU_IR_ATOMIC_PREDICATED) {
		/* Flag A = the predicate is false; no stores are outstanding at the branch. */
		error = bcm2711_shader_operand(compiler, instruction->src[predicate_source], BCM2711_SHADER_SCRATCH_WORK, &predicate);
		if (error != 0)
			return error;
		error = emit_magic(compiler, BCM2711_QPU_MOVE, COMPUTE_DISCARD, predicate, none, 0, COMPUTE_PUSH_ZERO);
		if (error != 0)
			return error;
		error = wait_stores(compiler);
		if (error != 0)
			return error;

		/*
		 * Jumps over the atomic when every dispatched lane is outside the
		 * predicate; three idle delay words.  The jump also moves the
		 * uniform stream past the configuration word the skipped address
		 * write would have read (the branch's own uniform: one word on),
		 * or every later uniform would be read one word early.
		 */
		kern_memset(&uniform, 0, sizeof(uniform));
		uniform.kind = BCM2711_SHADER_CONSTANT;
		uniform.bits = 4U;
		error = bcm2711_shader_uniform_append(compiler, &uniform);
		if (error != 0)
			return error;
		error = bcm2711_shader_branch(compiler, BCM2711_QPU_BRANCH_ALL_A, &record);
		if (error != 0)
			return error;
		error = bcm2711_shader_branch_rewind(compiler, record);
		if (error != 0)
			return error;
		error = bcm2711_shader_nops(compiler, 3);
		if (error != 0)
			return error;
		write_predicate = COMPUTE_IF_NOT_A;
	}

	/* The data words, then the configured address that sends the operation. */
	if (comparator != BCM2711_SHADER_ABSENT) {
		error = emit_magic(compiler, BCM2711_QPU_MOVE, COMPUTE_TMUD, bcm2711_shader_rf(comparator), none, write_predicate, 0);
		if (error != 0)
			return error;
	}
	error = emit_magic(compiler, BCM2711_QPU_MOVE, COMPUTE_TMUD, bcm2711_shader_rf(data), none, write_predicate, 0);
	if (error != 0)
		return error;
	kern_memset(&uniform, 0, sizeof(uniform));
	uniform.kind = BCM2711_SHADER_CONSTANT;
	uniform.bits = COMPUTE_CONFIG_BASE | operation << COMPUTE_CONFIG_OPERATION_SHIFT | COMPUTE_CONFIG_PER_PIXEL | COMPUTE_CONFIG_WORD;
	if (operation == COMPUTE_ATOMIC_CMPXCHG)
		uniform.bits = COMPUTE_CONFIG_BASE | operation << COMPUTE_CONFIG_OPERATION_SHIFT | COMPUTE_CONFIG_PER_PIXEL | COMPUTE_CONFIG_PAIR;
	error = bcm2711_shader_uniform_append(compiler, &uniform);
	if (error != 0)
		return error;
	error = emit_magic(compiler, BCM2711_QPU_MOVE, COMPUTE_TMUAU, bcm2711_shader_rf(address), none, write_predicate, 0);
	if (error != 0)
		return error;

	/* The old word, read as a load's result after the switch. */
	error = bcm2711_shader_switch(compiler, BCM2711_SHADER_SWITCH_SAMPLE);
	if (error != 0)
		return error;
	error = bcm2711_shader_signal(compiler, BCM2711_QPU_SIGNAL_TEXTURE, destination, 0);
	if (error != 0)
		return error;

	/* The skipped atomic lands here; nothing of the TMU is outstanding. */
	if (record != BCM2711_SHADER_ABSENT)
		bcm2711_shader_branch_target(compiler, record);

	/* Succeeded: the operation is sent and its old word read. */
	return 0;
}

/* Translates an IR atomic operation into the TMU's. */
static int
atomic_operation(
	uint32_t operation,
	uint32_t *native)
{
	/* The IR tags and the TMU numbers are both fixed. */
	switch (operation) {
	case DRV_GPU_IR_ATOMIC_ADD:
		*native = COMPUTE_ATOMIC_ADD;
		return 0;
	case DRV_GPU_IR_ATOMIC_SUB:
		*native = COMPUTE_ATOMIC_SUB;
		return 0;
	case DRV_GPU_IR_ATOMIC_XCHG:
		*native = COMPUTE_ATOMIC_XCHG;
		return 0;
	case DRV_GPU_IR_ATOMIC_CMPXCHG:
		*native = COMPUTE_ATOMIC_CMPXCHG;
		return 0;
	case DRV_GPU_IR_ATOMIC_UMIN:
		*native = COMPUTE_ATOMIC_UMIN;
		return 0;
	case DRV_GPU_IR_ATOMIC_UMAX:
		*native = COMPUTE_ATOMIC_UMAX;
		return 0;
	case DRV_GPU_IR_ATOMIC_SMIN:
		*native = COMPUTE_ATOMIC_SMIN;
		return 0;
	case DRV_GPU_IR_ATOMIC_SMAX:
		*native = COMPUTE_ATOMIC_SMAX;
		return 0;
	case DRV_GPU_IR_ATOMIC_AND:
		*native = COMPUTE_ATOMIC_AND;
		return 0;
	case DRV_GPU_IR_ATOMIC_OR:
		*native = COMPUTE_ATOMIC_OR;
		return 0;
	case DRV_GPU_IR_ATOMIC_XOR:
		*native = COMPUTE_ATOMIC_XOR;
		return 0;
	default:
		return ENOTSUP;
	}
}

/*
 * Lowers the start of a loop: the next record is the target of the loop's
 * backward branch.
 *
 * Nothing of the TMU crosses into the loop's first block, so the stores
 * before it are waited for; the constants kept in registers are forgotten,
 * because a later iteration may arrive after the body gave their registers
 * to other values.
 */
static int
lower_loop_begin(
	struct bcm2711_shader_compiler *compiler)
{
	struct bcm2711_qpu_source none;
	int error;

	/* The nest is bounded by the analysis. */
	if (compiler->loop_depth == BCM2711_SHADER_LOOP_DEPTH)
		return ENOTSUP;
	none = bcm2711_shader_bits(0);
	if (compiler->pending_stores != 0) {
		error = emit_magic(compiler, BCM2711_QPU_TEXTURE_WAIT, COMPUTE_DISCARD, none, none, 0, 0);
		if (error != 0)
			return error;
		compiler->pending_stores = 0;
	}
	forget_constants(compiler);

	/* The loop's first record comes next. */
	compiler->loop_targets[compiler->loop_depth] = compiler->op_count;
	compiler->loop_depth++;

	/* Succeeded: the loop is open. */
	return 0;
}

/*
 * Lowers the end of a loop: flag A is set on the lanes whose condition is
 * false, and the backward branch is taken while any dispatched lane has it
 * clear.  The branch moves the uniform stream back over every uniform the
 * loop read, its own included, and its three delay words idle.
 */
static int
lower_loop_end(
	struct bcm2711_shader_compiler *compiler,
	const struct drv_gpu_shader_ir_inst *instruction)
{
	struct bcm2711_shader_uniform uniform;
	struct bcm2711_qpu_source none;
	struct bcm2711_qpu_source condition;
	uint32_t target;
	uint32_t reads;
	uint32_t index;
	uint32_t record;
	int error;

	/* The innermost open loop. */
	if (compiler->loop_depth == 0)
		return EINVAL;
	target = compiler->loop_targets[compiler->loop_depth - 1U];

	/* No store is outstanding at the branch; then the condition's flag. */
	none = bcm2711_shader_bits(0);
	if (compiler->pending_stores != 0) {
		error = emit_magic(compiler, BCM2711_QPU_TEXTURE_WAIT, COMPUTE_DISCARD, none, none, 0, 0);
		if (error != 0)
			return error;
		compiler->pending_stores = 0;
	}
	error = bcm2711_shader_operand(compiler, instruction->src[0], BCM2711_SHADER_SCRATCH_WORK, &condition);
	if (error != 0)
		return error;
	error = emit_magic(compiler, BCM2711_QPU_MOVE, COMPUTE_DISCARD, condition, none, 0, COMPUTE_PUSH_ZERO);
	if (error != 0)
		return error;

	/* The uniforms one iteration reads: every consuming record from the target on, and the branch's own. */
	reads = 1;
	for (index = target; index < compiler->op_count; index++) {
		if (compiler->ops[index].has_uniform != 0)
			reads++;
	}
	kern_memset(&uniform, 0, sizeof(uniform));
	uniform.kind = BCM2711_SHADER_CONSTANT;
	uniform.bits = 0U - reads * 4U;
	error = bcm2711_shader_uniform_append(compiler, &uniform);
	if (error != 0)
		return error;

	/* The branch back while any dispatched lane continues, then its delay words. */
	error = bcm2711_shader_branch(compiler, BCM2711_QPU_BRANCH_ANY_NOT_A, &record);
	if (error != 0)
		return error;
	error = bcm2711_shader_branch_rewind(compiler, record);
	if (error != 0)
		return error;
	compiler->ops[record].target = target;
	error = bcm2711_shader_nops(compiler, 3);
	if (error != 0)
		return error;
	compiler->loop_depth--;

	/* The constants the body kept may be used after the loop: its last iteration ran the whole body. */
	return 0;
}

/* Writes a loop variable's new value into the register it already holds. */
static int
lower_redefinition(
	struct bcm2711_shader_compiler *compiler,
	const struct drv_gpu_shader_ir_inst *instruction)
{
	struct bcm2711_qpu_source source;
	int error;

	/* A bit-preserving move into the variable's register. */
	error = bcm2711_shader_operand(compiler, instruction->src[0], BCM2711_SHADER_SCRATCH_LEFT, &source);
	if (error != 0)
		return error;
	error = emit(compiler, BCM2711_QPU_MOVE, compiler->values[instruction->dst].number, source, bcm2711_shader_bits(0));
	if (error != 0)
		return error;

	/* Succeeded: the variable holds its new value. */
	return 0;
}

/* Gives up every register that keeps a constant, so code after this point loads its constants itself. */
static void
forget_constants(
	struct bcm2711_shader_compiler *compiler)
{
	uint32_t index;

	/* Each kept constant's register returns to the allocation order. */
	for (index = 0; index < 64; index++) {
		if (compiler->constant_held[index] == 0)
			continue;
		compiler->constant_held[index] = 0;
		bcm2711_shader_release(compiler, index);
	}
}

/* Lowers the low 32 bits of an integer product: MULTOP latches the high part, UMUL24 adds the 24-bit product. */
static int
lower_multiply(
	struct bcm2711_shader_compiler *compiler,
	const struct drv_gpu_shader_ir_inst *instruction,
	uint32_t destination)
{
	struct bcm2711_qpu_source left;
	struct bcm2711_qpu_source right;
	uint32_t number;
	int error;

	/*
	 * Both halves read the same two sources; a word reads one small
	 * immediate, so the left source is a register (Mesa 26.2.6
	 * src/broadcom/compiler/v3d_compiler.h, vir_UMUL: MULTOP then UMUL24).
	 */
	error = value_register(compiler, instruction->src[0], &number);
	if (error != 0)
		return error;
	left = bcm2711_shader_rf(number);
	error = bcm2711_shader_operand(compiler, instruction->src[1], BCM2711_SHADER_SCRATCH_RIGHT, &right);
	if (error != 0)
		return error;
	error = emit_magic(compiler, BCM2711_QPU_MULTIPLY_TOP, COMPUTE_DISCARD, left, right, 0, 0);
	if (error != 0)
		return error;
	error = emit(compiler, BCM2711_QPU_MULTIPLY_24, destination, left, right);
	if (error != 0)
		return error;

	/* Succeeded: the product is in the destination. */
	return 0;
}

/*
 * Lowers an integer comparison into a Boolean: a flag push, a default false
 * and a predicated true.
 *
 * Equality is the zero flag of an exclusive or; unsigned less-than is the
 * carry (borrow) of a subtraction; signed less-than is the carry of the
 * minimum of the operands exchanged (Mesa 26.2.6 nir_to_vir.c,
 * ntq_emit_comparison).
 */
static int
lower_compare(
	struct bcm2711_shader_compiler *compiler,
	const struct drv_gpu_shader_ir_inst *instruction,
	uint32_t destination)
{
	enum bcm2711_qpu_operation operation;
	struct bcm2711_qpu_source left;
	struct bcm2711_qpu_source right;
	struct bcm2711_qpu_source exchanged;
	uint32_t number;
	uint32_t push;
	uint32_t holds;
	int error;

	/* The left source is a register so the word reads at most one small immediate. */
	error = value_register(compiler, instruction->src[0], &number);
	if (error != 0)
		return error;
	left = bcm2711_shader_rf(number);
	error = bcm2711_shader_operand(compiler, instruction->src[1], BCM2711_SHADER_SCRATCH_RIGHT, &right);
	if (error != 0)
		return error;

	/* The flag operation, its push and the flag value that means true. */
	operation = BCM2711_QPU_BITS_XOR;
	push = COMPUTE_PUSH_ZERO;
	holds = COMPUTE_IF_A;
	switch (instruction->op) {
	case DRV_GPU_IR_IEQ:
		break;
	case DRV_GPU_IR_INE:
		holds = COMPUTE_IF_NOT_A;
		break;
	case DRV_GPU_IR_ULT:
	case DRV_GPU_IR_UGE:
		operation = BCM2711_QPU_INTEGER_SUBTRACT;
		push = COMPUTE_PUSH_CARRY;
		if (instruction->op == DRV_GPU_IR_UGE)
			holds = COMPUTE_IF_NOT_A;
		break;
	case DRV_GPU_IR_ILT:
	case DRV_GPU_IR_IGE:
		/* The minimum's carry is set when its left input is above its right: min(b, a) for a < b. */
		operation = BCM2711_QPU_INTEGER_MINIMUM;
		push = COMPUTE_PUSH_CARRY;
		exchanged = left;
		left = right;
		right = exchanged;
		if (instruction->op == DRV_GPU_IR_IGE)
			holds = COMPUTE_IF_NOT_A;
		break;
	default:
		return ENOTSUP;
	}

	/* The push, false everywhere, then true where the flag says so. */
	error = emit_magic(compiler, operation, COMPUTE_DISCARD, left, right, 0, push);
	if (error != 0)
		return error;
	error = emit(compiler, BCM2711_QPU_MOVE, destination, bcm2711_shader_bits(0), bcm2711_shader_bits(0));
	if (error != 0)
		return error;
	error = emit_predicated(compiler, BCM2711_QPU_MOVE, destination, bcm2711_shader_bits(0xffffffffU), bcm2711_shader_bits(0), holds);
	if (error != 0)
		return error;

	/* Succeeded: the Boolean is in the destination. */
	return 0;
}

/*
 * Lowers an integer division or remainder.
 *
 * A signed one divides the magnitudes and restores the signs: the quotient
 * takes the sign of the operands' product, the remainder the numerator's.
 */
static int
lower_divide(
	struct bcm2711_shader_compiler *compiler,
	const struct drv_gpu_shader_ir_inst *instruction,
	uint32_t destination)
{
	struct bcm2711_qpu_source sign_shift;
	uint32_t numerator;
	uint32_t divisor;
	uint32_t quotient;
	uint32_t remainder;
	uint32_t numerator_sign;
	uint32_t divisor_sign;
	uint32_t log;
	uint32_t is_power;
	struct bcm2711_shader_value *constant;
	int error;

	/* An unsigned division by a power-of-two constant is a shift or a mask. */
	constant = &compiler->values[instruction->src[1]];
	is_power = 0;
	if (constant->constant != 0)
		is_power = power_of_two(constant->number, &log);
	if (is_power != 0 && (instruction->op == DRV_GPU_IR_UDIV || instruction->op == DRV_GPU_IR_UMOD)) {
		error = value_register(compiler, instruction->src[0], &numerator);
		if (error != 0)
			return error;
		if (instruction->op == DRV_GPU_IR_UDIV) {
			error = emit(compiler, BCM2711_QPU_SHIFT_RIGHT, destination, bcm2711_shader_rf(numerator), bcm2711_shader_bits(shift_bits(log)));
			if (error != 0)
				return error;
			return 0;
		}
		error = bits_source(compiler, constant->number - 1U, BCM2711_SHADER_SCRATCH_RIGHT, &sign_shift);
		if (error != 0)
			return error;
		error = emit(compiler, BCM2711_QPU_BITS_AND, destination, bcm2711_shader_rf(numerator), sign_shift);
		if (error != 0)
			return error;
		return 0;
	}

	/* The operands in temporaries the division may overwrite. */
	error = temporary(compiler, &numerator);
	if (error != 0)
		return error;
	error = temporary(compiler, &divisor);
	if (error != 0)
		return error;
	error = value_register(compiler, instruction->src[0], &quotient);
	if (error != 0)
		return error;
	error = emit(compiler, BCM2711_QPU_MOVE, numerator, bcm2711_shader_rf(quotient), bcm2711_shader_bits(0));
	if (error != 0)
		return error;
	error = value_register(compiler, instruction->src[1], &quotient);
	if (error != 0)
		return error;
	error = emit(compiler, BCM2711_QPU_MOVE, divisor, bcm2711_shader_rf(quotient), bcm2711_shader_bits(0));
	if (error != 0)
		return error;

	/* A signed division works on the magnitudes: |x| = (x ^ s) - s with s = x >> 31 (arithmetic). */
	numerator_sign = BCM2711_SHADER_ABSENT;
	divisor_sign = BCM2711_SHADER_ABSENT;
	sign_shift = bcm2711_shader_bits(shift_bits(31));
	if (instruction->op == DRV_GPU_IR_IDIV || instruction->op == DRV_GPU_IR_IREM) {
		error = temporary(compiler, &numerator_sign);
		if (error != 0)
			return error;
		error = temporary(compiler, &divisor_sign);
		if (error != 0)
			return error;
		error = emit(compiler, BCM2711_QPU_SHIFT_SIGNED, numerator_sign, bcm2711_shader_rf(numerator), sign_shift);
		if (error != 0)
			return error;
		error = emit(compiler, BCM2711_QPU_SHIFT_SIGNED, divisor_sign, bcm2711_shader_rf(divisor), sign_shift);
		if (error != 0)
			return error;
		error = emit(compiler, BCM2711_QPU_BITS_XOR, numerator, bcm2711_shader_rf(numerator), bcm2711_shader_rf(numerator_sign));
		if (error != 0)
			return error;
		error = emit(compiler, BCM2711_QPU_INTEGER_SUBTRACT, numerator, bcm2711_shader_rf(numerator), bcm2711_shader_rf(numerator_sign));
		if (error != 0)
			return error;
		error = emit(compiler, BCM2711_QPU_BITS_XOR, divisor, bcm2711_shader_rf(divisor), bcm2711_shader_rf(divisor_sign));
		if (error != 0)
			return error;
		error = emit(compiler, BCM2711_QPU_INTEGER_SUBTRACT, divisor, bcm2711_shader_rf(divisor), bcm2711_shader_rf(divisor_sign));
		if (error != 0)
			return error;
	}

	/* The unsigned quotient and remainder. */
	error = temporary(compiler, &quotient);
	if (error != 0)
		return error;
	error = temporary(compiler, &remainder);
	if (error != 0)
		return error;
	error = divide_unsigned(compiler, numerator, divisor, quotient, remainder);
	if (error != 0)
		return error;

	/* An unsigned result is the destination as it is. */
	if (instruction->op == DRV_GPU_IR_UDIV) {
		error = emit(compiler, BCM2711_QPU_MOVE, destination, bcm2711_shader_rf(quotient), bcm2711_shader_bits(0));
		if (error != 0)
			return error;
		return 0;
	}
	if (instruction->op == DRV_GPU_IR_UMOD) {
		error = emit(compiler, BCM2711_QPU_MOVE, destination, bcm2711_shader_rf(remainder), bcm2711_shader_bits(0));
		if (error != 0)
			return error;
		return 0;
	}

	/* A signed remainder takes the numerator's sign: (r ^ s) - s. */
	if (instruction->op == DRV_GPU_IR_IREM) {
		error = emit(compiler, BCM2711_QPU_BITS_XOR, remainder, bcm2711_shader_rf(remainder), bcm2711_shader_rf(numerator_sign));
		if (error != 0)
			return error;
		error = emit(compiler, BCM2711_QPU_INTEGER_SUBTRACT, destination, bcm2711_shader_rf(remainder), bcm2711_shader_rf(numerator_sign));
		if (error != 0)
			return error;
		return 0;
	}

	/* A signed quotient takes the sign of the product: s = sn ^ sd, then (q ^ s) - s. */
	error = emit(compiler, BCM2711_QPU_BITS_XOR, numerator_sign, bcm2711_shader_rf(numerator_sign), bcm2711_shader_rf(divisor_sign));
	if (error != 0)
		return error;
	error = emit(compiler, BCM2711_QPU_BITS_XOR, quotient, bcm2711_shader_rf(quotient), bcm2711_shader_rf(numerator_sign));
	if (error != 0)
		return error;
	error = emit(compiler, BCM2711_QPU_INTEGER_SUBTRACT, destination, bcm2711_shader_rf(quotient), bcm2711_shader_rf(numerator_sign));
	if (error != 0)
		return error;

	/* Succeeded: the signed quotient is in the destination. */
	return 0;
}

/*
 * Divides two unsigned registers without a multiply-high: the quotient is
 * built from underestimates t = trunc(float(r) * rcp'), where rcp' is the
 * divisor's reciprocal scaled just below itself, so t × d never exceeds r
 * and r - t × d never wraps.  Each step shrinks r by about 2^11; after four
 * r < 2d, and one corrected step finishes.  A zero divisor gives some value
 * without trapping (the result is undefined).
 */
static int
divide_unsigned(
	struct bcm2711_shader_compiler *compiler,
	uint32_t numerator,
	uint32_t divisor,
	uint32_t quotient,
	uint32_t remainder)
{
	struct bcm2711_qpu_source none;
	struct bcm2711_qpu_source factor;
	uint32_t reciprocal;
	uint32_t estimate;
	uint32_t product;
	uint32_t step;
	int error;

	/* rcp' = (1 / float(d)) × (1 - 2^-12). */
	none = bcm2711_shader_bits(0);
	error = temporary(compiler, &reciprocal);
	if (error != 0)
		return error;
	error = temporary(compiler, &estimate);
	if (error != 0)
		return error;
	error = temporary(compiler, &product);
	if (error != 0)
		return error;
	error = emit(compiler, BCM2711_QPU_UNSIGNED_TO_FLOAT, estimate, bcm2711_shader_rf(divisor), none);
	if (error != 0)
		return error;
	error = emit(compiler, BCM2711_QPU_FLOAT_RECIPROCAL, reciprocal, bcm2711_shader_rf(estimate), none);
	if (error != 0)
		return error;
	error = bits_source(compiler, COMPUTE_UNDERESTIMATE, BCM2711_SHADER_SCRATCH_RIGHT, &factor);
	if (error != 0)
		return error;
	error = emit(compiler, BCM2711_QPU_FLOAT_MULTIPLY, reciprocal, bcm2711_shader_rf(reciprocal), factor);
	if (error != 0)
		return error;

	/* q = 0, r = n. */
	error = emit(compiler, BCM2711_QPU_MOVE, quotient, none, none);
	if (error != 0)
		return error;
	error = emit(compiler, BCM2711_QPU_MOVE, remainder, bcm2711_shader_rf(numerator), none);
	if (error != 0)
		return error;

	/* Each step: t = trunc(float(r) × rcp'), r -= t × d, q += t. */
	for (step = 0; step < COMPUTE_DIVIDE_STEPS; step++) {
		error = emit(compiler, BCM2711_QPU_UNSIGNED_TO_FLOAT, estimate, bcm2711_shader_rf(remainder), none);
		if (error != 0)
			return error;
		error = emit(compiler, BCM2711_QPU_FLOAT_MULTIPLY, estimate, bcm2711_shader_rf(estimate), bcm2711_shader_rf(reciprocal));
		if (error != 0)
			return error;
		error = emit(compiler, BCM2711_QPU_FLOAT_TO_UNSIGNED, estimate, bcm2711_shader_rf(estimate), none);
		if (error != 0)
			return error;
		error = emit_magic(compiler, BCM2711_QPU_MULTIPLY_TOP, COMPUTE_DISCARD, bcm2711_shader_rf(estimate), bcm2711_shader_rf(divisor), 0, 0);
		if (error != 0)
			return error;
		error = emit(compiler, BCM2711_QPU_MULTIPLY_24, product, bcm2711_shader_rf(estimate), bcm2711_shader_rf(divisor));
		if (error != 0)
			return error;
		error = emit(compiler, BCM2711_QPU_INTEGER_SUBTRACT, remainder, bcm2711_shader_rf(remainder), bcm2711_shader_rf(product));
		if (error != 0)
			return error;
		error = emit(compiler, BCM2711_QPU_INTEGER_ADD, quotient, bcm2711_shader_rf(quotient), bcm2711_shader_rf(estimate));
		if (error != 0)
			return error;
	}

	/* r < 2d now: where r >= d (no borrow from r - d), q += 1 and r -= d. */
	error = emit_magic(compiler, BCM2711_QPU_INTEGER_SUBTRACT, COMPUTE_DISCARD, bcm2711_shader_rf(remainder), bcm2711_shader_rf(divisor), 0, COMPUTE_PUSH_CARRY);
	if (error != 0)
		return error;
	error = emit_predicated(compiler, BCM2711_QPU_INTEGER_ADD, quotient, bcm2711_shader_rf(quotient), bcm2711_shader_bits(1), COMPUTE_IF_NOT_A);
	if (error != 0)
		return error;
	error = emit_predicated(compiler, BCM2711_QPU_INTEGER_SUBTRACT, remainder, bcm2711_shader_rf(remainder), bcm2711_shader_rf(divisor), COMPUTE_IF_NOT_A);
	if (error != 0)
		return error;

	/* Succeeded: q and r are the quotient and remainder. */
	return 0;
}

/*
 * Divides a register holding an integer below 256 by a constant from 1 to
 * 256: a power of two is a shift and a mask; any other divisor c uses
 * q = (n × ceil(2^16 / c)) >> 16, exact for n < 256 and c < 256, whose
 * product fits the 24-bit multiply.  Either result may be absent.
 */
static int
divide_small(
	struct bcm2711_shader_compiler *compiler,
	uint32_t numerator,
	uint32_t divisor,
	uint32_t quotient,
	uint32_t remainder)
{
	struct bcm2711_qpu_source operand;
	uint32_t log;
	uint32_t is_power;
	uint32_t scaled;
	int error;

	/* A power of two. */
	is_power = power_of_two(divisor, &log);
	if (is_power != 0) {
		if (quotient != BCM2711_SHADER_ABSENT) {
			error = emit(compiler, BCM2711_QPU_SHIFT_RIGHT, quotient, bcm2711_shader_rf(numerator), bcm2711_shader_bits(shift_bits(log)));
			if (error != 0)
				return error;
		}
		if (remainder != BCM2711_SHADER_ABSENT) {
			error = bits_source(compiler, divisor - 1U, BCM2711_SHADER_SCRATCH_RIGHT, &operand);
			if (error != 0)
				return error;
			error = emit(compiler, BCM2711_QPU_BITS_AND, remainder, bcm2711_shader_rf(numerator), operand);
			if (error != 0)
				return error;
		}
		return 0;
	}

	/* q = (n × ceil(65536 / c)) >> 16; the remainder needs q in a register of its own. */
	scaled = quotient;
	if (scaled == BCM2711_SHADER_ABSENT) {
		error = temporary(compiler, &scaled);
		if (error != 0)
			return error;
	}
	error = bits_source(compiler, (65536U + divisor - 1U) / divisor, BCM2711_SHADER_SCRATCH_RIGHT, &operand);
	if (error != 0)
		return error;
	error = emit(compiler, BCM2711_QPU_MULTIPLY_24, scaled, bcm2711_shader_rf(numerator), operand);
	if (error != 0)
		return error;
	error = emit(compiler, BCM2711_QPU_SHIFT_RIGHT, scaled, bcm2711_shader_rf(scaled), bcm2711_shader_bits(shift_bits(16)));
	if (error != 0)
		return error;
	if (remainder == BCM2711_SHADER_ABSENT)
		return 0;

	/* r = n - q × c. */
	error = bits_source(compiler, divisor, BCM2711_SHADER_SCRATCH_RIGHT, &operand);
	if (error != 0)
		return error;
	error = emit(compiler, BCM2711_QPU_MULTIPLY_24, remainder, bcm2711_shader_rf(scaled), operand);
	if (error != 0)
		return error;
	error = emit(compiler, BCM2711_QPU_INTEGER_SUBTRACT, remainder, bcm2711_shader_rf(numerator), bcm2711_shader_rf(remainder));
	if (error != 0)
		return error;

	/* Succeeded: the requested results are in their registers. */
	return 0;
}

/*
 * Computes the clamped address of a storage access of one word into a
 * register: base + umin(offset, range - 4), both from the uniform stream.
 */
static int
storage_address(
	struct bcm2711_shader_compiler *compiler,
	const struct drv_gpu_shader_ir_inst *instruction,
	uint32_t address)
{
	const struct drv_gpu_shader_ir_uniform *buffer;
	struct bcm2711_qpu_source offset;
	uint32_t limit;
	int error;

	/* The buffer's limit for a four-byte access, and the offset clamped to it. */
	buffer = &compiler->ir->uniforms[instruction->location];
	error = temporary(compiler, &limit);
	if (error != 0)
		return error;
	error = storage_uniform(compiler, BCM2711_SHADER_STORAGE_LIMIT, buffer, 4, limit);
	if (error != 0)
		return error;
	error = bcm2711_shader_operand(compiler, instruction->src[0], BCM2711_SHADER_SCRATCH_LEFT, &offset);
	if (error != 0)
		return error;
	error = emit(compiler, BCM2711_QPU_UNSIGNED_MINIMUM, address, bcm2711_shader_rf(limit), offset);
	if (error != 0)
		return error;

	/* The base plus the clamped offset. */
	error = storage_uniform(compiler, BCM2711_SHADER_STORAGE_BASE, buffer, 0, limit);
	if (error != 0)
		return error;
	error = emit(compiler, BCM2711_QPU_INTEGER_ADD, address, bcm2711_shader_rf(limit), bcm2711_shader_rf(address));
	if (error != 0)
		return error;

	/* Succeeded: the address is inside the buffer. */
	return 0;
}

/* Computes the clamped address of a storage buffer, shared memory or storage image texel access. */
static int
access_address(
	struct bcm2711_shader_compiler *compiler,
	const struct drv_gpu_shader_ir_inst *instruction,
	uint32_t address)
{
	int error;

	/* Shared memory has its own arena, a storage image's texel its view. */
	if (instruction->op == DRV_GPU_IR_LOAD_SHARED ||
	    instruction->op == DRV_GPU_IR_STORE_SHARED ||
	    (instruction->op == DRV_GPU_IR_ATOMIC && instruction->location == DRV_GPU_IR_LOCATION_SHARED))
		error = shared_address(compiler, instruction, address);
	else if (instruction->op == DRV_GPU_IR_ATOMIC && (instruction->image_shape & DRV_GPU_IR_IMAGE_TEXEL) != 0)
		error = image_address(compiler, instruction, 4, address);
	else
		error = storage_address(compiler, instruction, address);
	if (error != 0)
		return error;

	/* Succeeded: the address is inside its memory. */
	return 0;
}

/*
 * Computes the clamped address of a shared memory access of one word:
 * base + umin(slot × shared_bytes + offset, arena - 4), where slot is the
 * workgroup's memory slot in the second payload word, (rf2 >> 16) masked
 * below the local index's b bits, unmasked for a workgroup of one
 * invocation (Mesa 26.2.6 src/broadcom/compiler/nir_to_vir.c lines
 * 4917-4928).  The clamp covers the
 * whole arena: a stray offset reaches another workgroup of the same
 * dispatch, never another session.
 */
static int
shared_address(
	struct bcm2711_shader_compiler *compiler,
	const struct drv_gpu_shader_ir_inst *instruction,
	uint32_t address)
{
	struct bcm2711_shader_uniform uniform;
	struct bcm2711_qpu_source operand;
	struct bcm2711_qpu_source offset;
	uint32_t size;
	uint32_t bits;
	uint32_t limit;
	int error;

	/* The slot. */
	size = compiler->binary->local_size[0] * compiler->binary->local_size[1] * compiler->binary->local_size[2];
	bits = 6;
	while ((1U << bits) < size)
		bits++;
	error = emit(compiler, BCM2711_QPU_SHIFT_RIGHT, address, bcm2711_shader_rf(bcm2711_shader_compute_payload(1)), bcm2711_shader_bits(shift_bits(16)));
	if (error != 0)
		return error;
	if (size != 1) {
		error = bits_source(compiler, (1U << (16U - bits)) - 1U, BCM2711_SHADER_SCRATCH_RIGHT, &operand);
		if (error != 0)
			return error;
		error = emit(compiler, BCM2711_QPU_BITS_AND, address, bcm2711_shader_rf(address), operand);
		if (error != 0)
			return error;
	}

	/* slot × shared_bytes; both halves of the multiply, since a one-invocation workgroup's slot has 16 bits. */
	error = bits_source(compiler, compiler->binary->shared_bytes, BCM2711_SHADER_SCRATCH_RIGHT, &operand);
	if (error != 0)
		return error;
	error = emit_magic(compiler, BCM2711_QPU_MULTIPLY_TOP, COMPUTE_DISCARD, bcm2711_shader_rf(address), operand, 0, 0);
	if (error != 0)
		return error;
	error = emit(compiler, BCM2711_QPU_MULTIPLY_24, address, bcm2711_shader_rf(address), operand);
	if (error != 0)
		return error;
	error = bcm2711_shader_operand(compiler, instruction->src[0], BCM2711_SHADER_SCRATCH_LEFT, &offset);
	if (error != 0)
		return error;
	error = emit(compiler, BCM2711_QPU_INTEGER_ADD, address, bcm2711_shader_rf(address), offset);
	if (error != 0)
		return error;

	/* Clamped to the arena, then added to its base. */
	error = temporary(compiler, &limit);
	if (error != 0)
		return error;
	kern_memset(&uniform, 0, sizeof(uniform));
	uniform.kind = BCM2711_SHADER_SHARED_LIMIT;
	uniform.bits = 4;
	error = bcm2711_shader_uniform_load(compiler, &uniform, limit);
	if (error != 0)
		return error;
	error = emit(compiler, BCM2711_QPU_UNSIGNED_MINIMUM, address, bcm2711_shader_rf(limit), bcm2711_shader_rf(address));
	if (error != 0)
		return error;
	uniform.kind = BCM2711_SHADER_SHARED_BASE;
	uniform.bits = 0;
	error = bcm2711_shader_uniform_load(compiler, &uniform, limit);
	if (error != 0)
		return error;
	error = emit(compiler, BCM2711_QPU_INTEGER_ADD, address, bcm2711_shader_rf(limit), bcm2711_shader_rf(address));
	if (error != 0)
		return error;

	/* Succeeded: the address is inside the arena. */
	return 0;
}

/*
 * Lowers a workgroup barrier or a memory fence.
 *
 * Loads are read as soon as they are made, so the TMU idles once the
 * stores are waited for; a barrier then writes its identity to the
 * barrier unit and switches threads, the switch waiting for the
 * supergroup (Mesa 26.2.6 nir_to_vir.c lines 3697-3712).  A fence needs
 * only the wait.
 */
static int
lower_barrier(
	struct bcm2711_shader_compiler *compiler,
	const struct drv_gpu_shader_ir_inst *instruction)
{
	struct bcm2711_qpu_source none;
	int error;

	/* The stores complete first. */
	error = wait_stores(compiler);
	if (error != 0)
		return error;
	if (instruction->op == DRV_GPU_IR_FENCE)
		return 0;

	/* The barrier unit, then the switch that waits. */
	none = bcm2711_shader_bits(0);
	error = emit_magic(compiler, BCM2711_QPU_BARRIER_ID, COMPUTE_SYNCB, none, none, 0, 0);
	if (error != 0)
		return error;
	error = bcm2711_shader_switch(compiler, BCM2711_SHADER_SWITCH_SAMPLE);
	if (error != 0)
		return error;

	/* Succeeded: every invocation of the workgroup passed the barrier. */
	return 0;
}

/* Waits for the outstanding stores. */
static int
wait_stores(
	struct bcm2711_shader_compiler *compiler)
{
	struct bcm2711_qpu_source none;
	int error;

	/* Nothing to wait for. */
	if (compiler->pending_stores == 0)
		return 0;
	none = bcm2711_shader_bits(0);
	error = emit_magic(compiler, BCM2711_QPU_TEXTURE_WAIT, COMPUTE_DISCARD, none, none, 0, 0);
	if (error != 0)
		return error;
	compiler->pending_stores = 0;

	/* Succeeded: every store reached memory. */
	return 0;
}

/* Loads one uniform describing a storage buffer into a register. */
static int
storage_uniform(
	struct bcm2711_shader_compiler *compiler,
	enum bcm2711_shader_uniform_kind kind,
	const struct drv_gpu_shader_ir_uniform *buffer,
	uint32_t bits,
	uint32_t destination)
{
	struct bcm2711_shader_uniform uniform;
	int error;

	/* The runtime fills the word from the bound descriptor of (set, binding). */
	kern_memset(&uniform, 0, sizeof(uniform));
	uniform.kind = kind;
	uniform.set = buffer->set;
	uniform.binding = buffer->binding;
	uniform.element = buffer->element;
	uniform.bits = bits;
	error = bcm2711_shader_uniform_load(compiler, &uniform, destination);
	if (error != 0)
		return error;

	/* Succeeded: the word is in the destination. */
	return 0;
}

/* Waits for the outstanding stores when one more TMU operation would exceed the queue. */
static int
tmu_room(
	struct bcm2711_shader_compiler *compiler)
{
	struct bcm2711_qpu_source none;
	int error;

	/* Loads are read at once, so only the stores accumulate. */
	if (compiler->pending_stores < COMPUTE_STORES_BEFORE_WAIT)
		return 0;
	none = bcm2711_shader_bits(0);
	error = emit_magic(compiler, BCM2711_QPU_TEXTURE_WAIT, COMPUTE_DISCARD, none, none, 0, 0);
	if (error != 0)
		return error;
	compiler->pending_stores = 0;

	/* Succeeded: the queue has room. */
	return 0;
}

/* Allocates a register for a value that lives until the current source instruction is lowered. */
static int
temporary(
	struct bcm2711_shader_compiler *compiler,
	uint32_t *number)
{
	int error;

	/* A borrowed register returns when the instruction's scratch registers are released. */
	error = bcm2711_shader_register(compiler, number);
	if (error != 0)
		return error;
	compiler->borrowed[*number] = 1;

	/* Succeeded: the register is the caller's for this instruction. */
	return 0;
}

/* Places a value in a register: its own, or a temporary holding a constant. */
static int
value_register(
	struct bcm2711_shader_compiler *compiler,
	uint32_t value,
	uint32_t *number)
{
	struct bcm2711_qpu_source source;
	int error;

	/* A live value already has a register. */
	error = bcm2711_shader_operand(compiler, value, BCM2711_SHADER_SCRATCH_LEFT, &source);
	if (error != 0)
		return error;
	if (source.kind == BCM2711_QPU_REGISTER) {
		*number = source.number;
		return 0;
	}

	/* A small constant is moved into a temporary. */
	error = temporary(compiler, number);
	if (error != 0)
		return error;
	error = emit(compiler, BCM2711_QPU_MOVE, *number, source, bcm2711_shader_bits(0));
	if (error != 0)
		return error;

	/* Succeeded: the value is in the register. */
	return 0;
}

/* Gives exact constant bits as a small immediate or a register loaded from the uniform stream. */
static int
bits_source(
	struct bcm2711_shader_compiler *compiler,
	uint32_t bits,
	uint32_t scratch,
	struct bcm2711_qpu_source *source)
{
	uint32_t encoded;
	uint32_t number;
	int error;

	/* A small immediate needs no uniform. */
	error = bcm2711_qpu_small_constant(bits, &encoded);
	if (error == 0) {
		*source = bcm2711_shader_bits(bits);
		return 0;
	}
	if (error != ENOTSUP)
		return error;

	/* A full-width constant comes from the uniform stream. */
	error = bcm2711_shader_constant(compiler, bits, scratch, &number);
	if (error != 0)
		return error;
	*source = bcm2711_shader_rf(number);

	/* Succeeded: the source holds the exact bits. */
	return 0;
}

/* Emits one unpredicated operation writing a register. */
static int
emit(
	struct bcm2711_shader_compiler *compiler,
	enum bcm2711_qpu_operation operation,
	uint32_t destination,
	struct bcm2711_qpu_source left,
	struct bcm2711_qpu_source right)
{
	int error;

	/* No predicate, no flag push. */
	error = bcm2711_shader_operation(compiler, operation, destination, 0, left, right, 0, 0);
	if (error != 0)
		return error;

	/* Succeeded: the result is on its way to the register. */
	return 0;
}

/* Emits one operation writing a magic address, with a predicate and a flag push. */
static int
emit_magic(
	struct bcm2711_shader_compiler *compiler,
	enum bcm2711_qpu_operation operation,
	uint32_t address,
	struct bcm2711_qpu_source left,
	struct bcm2711_qpu_source right,
	uint32_t predicate,
	uint32_t flags)
{
	int error;

	/* The magic bit names a peripheral rather than a register. */
	error = bcm2711_shader_operation(compiler, operation, address, 1, left, right, predicate, flags);
	if (error != 0)
		return error;

	/* Succeeded: the peripheral write is recorded. */
	return 0;
}

/* Emits one predicated operation writing a register. */
static int
emit_predicated(
	struct bcm2711_shader_compiler *compiler,
	enum bcm2711_qpu_operation operation,
	uint32_t destination,
	struct bcm2711_qpu_source left,
	struct bcm2711_qpu_source right,
	uint32_t predicate)
{
	int error;

	/* Only the lanes whose flag matches write. */
	error = bcm2711_shader_operation(compiler, operation, destination, 0, left, right, predicate, 0);
	if (error != 0)
		return error;

	/* Succeeded: the predicated result is on its way. */
	return 0;
}

/* Gives a shift amount as a small immediate: the shifts read its low five bits, so 16 to 31 are -16 to -1. */
static uint32_t
shift_bits(
	uint32_t shift)
{
	/* 0 to 15 are themselves. */
	if (shift < 16)
		return shift;

	/* 16 to 31 are the negative immediates with the same low five bits. */
	return shift - 32U;
}

/* Tells whether a value is a power of two, and its logarithm. */
static uint32_t
power_of_two(
	uint32_t value,
	uint32_t *log)
{
	uint32_t bits;

	/* Zero and values with more than one bit are not. */
	if (value == 0 || (value & (value - 1U)) != 0)
		return 0;

	/* The position of the one bit. */
	bits = 0;
	while ((1U << bits) != value)
		bits++;
	*log = bits;
	return 1;
}
