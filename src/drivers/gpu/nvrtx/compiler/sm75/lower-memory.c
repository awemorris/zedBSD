/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The lowering of the storage buffer and shared memory accesses, their
 * atomics, and the workgroup barriers and memory fences (see lower.h).
 */

#include "drivers/gpu/nvrtx/compiler/sm75/lower.h"

#include <uapi/errno.h>

static int nvrtx_sm75_storage_address(struct nvrtx_sm75_lowering *lowering, uint32_t slot, uint32_t offset, uint32_t *address);

/*
 * Lowers a storage buffer access: the buffer's address from the root
 * constant buffer plus the byte offset, then a global load or store, under
 * P0 when the access is predicated; or the buffer's size.
 */
int
nvrtx_sm75_lower_storage(
	struct nvrtx_sm75_lowering *lowering,
	const struct drv_gpu_shader_ir_inst *instruction,
	uint32_t dst)
{
	struct nvrtx_sm75_word word;
	struct nvrtx_sm75_src a;
	struct nvrtx_sm75_src zero;
	struct nvrtx_sm75_src size;
	uint32_t slot;
	uint32_t address;
	uint32_t guard;
	int error;

	/* The access names a storage buffer of the list. */
	if (instruction->location >= lowering->ir->uniform_count || lowering->ir->uniforms[instruction->location].kind != DRV_GPU_IR_UNIFORM_STORAGE) {
		error = nvrtx_sm75_refuse(lowering, EINVAL, "a storage access names no storage buffer");
		return error;
	}

	/* The buffer's entry of the storage table. */
	slot = lowering->slot[instruction->location];

	/* The size is a word of the buffer's entry. */
	if (instruction->op == DRV_GPU_IR_STORAGE_SIZE) {
		size = nvrtx_sm75_cb(NVRTX_SM75_CB_ROOT, NVRTX_SM75_CB_STORAGE + slot * NVRTX_SM75_CB_STORAGE_ENTRY + NVRTX_SM75_CB_STORAGE_SIZE);
		nvrtx_sm75_begin(&word);
		nvrtx_sm75_mov(&word, dst, &size);
		error = nvrtx_sm75_emit(lowering, &word, NVRTX_SM75_FIXED);
		if (error != 0)
			return error;

		/* Succeeded: the size is in dst. */
		return 0;
	}

	/* The address of the word. */
	error = nvrtx_sm75_storage_address(lowering, slot, lowering->reg[instruction->src[0]], &address);
	if (error != 0)
		return error;

	/* P0 from the predicate of a predicated access. */
	guard = NVRTX_SM75_PT;
	if (instruction->component != 0U) {
		guard = instruction->src[1];
		if (instruction->op == DRV_GPU_IR_STORE_STORAGE)
			guard = instruction->src[2];
		a = nvrtx_sm75_value(lowering, guard);
		zero = nvrtx_sm75_reg(NVRTX_SM75_RZ);
		nvrtx_sm75_begin(&word);
		nvrtx_sm75_isetp(&word, NVRTX_SM75_P0, NVRTX_SM75_ICMP_NE, 0, &a, &zero);
		error = nvrtx_sm75_emit(lowering, &word, NVRTX_SM75_FIXED_PREDICATE);
		guard = NVRTX_SM75_P0;
	}

	/* The load or the store, guarded. */
	if (error == 0) {
		nvrtx_sm75_begin(&word);
		if (instruction->op == DRV_GPU_IR_LOAD_STORAGE) {
			nvrtx_sm75_ldg(&word, dst, address);
		} else {
			nvrtx_sm75_stg(&word, address, lowering->reg[instruction->src[1]]);
		}

		/* PT, or P0 for a predicated access. */
		nvrtx_sm75_guard(&word, guard, 0);

		/* A load writes a result; a store only reads its sources. */
		if (instruction->op == DRV_GPU_IR_LOAD_STORAGE) {
			error = nvrtx_sm75_emit(lowering, &word, NVRTX_SM75_VARIABLE_WRITE);
		} else {
			error = nvrtx_sm75_emit(lowering, &word, NVRTX_SM75_VARIABLE_STORE);
		}
	}

	/* The address pair goes back. */
	nvrtx_sm75_give(lowering, address);
	nvrtx_sm75_give(lowering, address + 1U);

	/* Reports an emission past the code's room. */
	if (error != 0)
		return error;

	/* Succeeded: the access is made. */
	return 0;
}

/*
 * Lowers a load of a word of a uniform buffer read through the storage
 * table: the buffer's address (LDC.64 from its entry), then LDG at the
 * word's constant offset.
 */
int
nvrtx_sm75_lower_uniform_load(
	struct nvrtx_sm75_lowering *lowering,
	const struct drv_gpu_shader_ir_inst *instruction,
	uint32_t dst)
{
	struct nvrtx_sm75_word word;
	uint32_t slot;
	uint32_t base;
	int error;

	/* The buffer's entry of the storage table. */
	slot = lowering->slot[instruction->location] & ~NVRTX_SM75_SLOT_TABLE;

	/* The pair the buffer's address is loaded into. */
	error = nvrtx_sm75_take_pair(lowering, &base);
	if (error != 0)
		return error;

	/* The address from the entry. */
	nvrtx_sm75_begin(&word);
	nvrtx_sm75_ldc(&word, base, NVRTX_SM75_CB_ROOT, NVRTX_SM75_CB_STORAGE + slot * NVRTX_SM75_CB_STORAGE_ENTRY, 1);
	error = nvrtx_sm75_emit(lowering, &word, NVRTX_SM75_VARIABLE_WRITE);

	/* The word at its offset from it. */
	if (error == 0) {
		nvrtx_sm75_begin(&word);
		nvrtx_sm75_ldg(&word, dst, base);
		nvrtx_sm75_set(&word, NVRTX_SM75_MEM_OFFSET_LOW, NVRTX_SM75_MEM_OFFSET_BITS, instruction->immediate);
		error = nvrtx_sm75_emit(lowering, &word, NVRTX_SM75_VARIABLE_WRITE);
	}

	/* The address pair goes back. */
	nvrtx_sm75_give(lowering, base);
	nvrtx_sm75_give(lowering, base + 1U);
	if (error != 0)
		return error;

	/* Succeeded: the word is in dst. */
	return 0;
}

/*
 * Lowers an atomic on a storage buffer word (its address from the table)
 * or on a shared memory word (its offset), returning the old value; a
 * subtraction adds the negated value, a compare and exchange writes src[1]
 * where the word equals src[2].  A predicated one runs under P0.
 */
int
nvrtx_sm75_lower_atomic(
	struct nvrtx_sm75_lowering *lowering,
	const struct drv_gpu_shader_ir_inst *instruction,
	uint32_t dst)
{
	struct nvrtx_sm75_word word;
	struct nvrtx_sm75_src a;
	struct nvrtx_sm75_src zero;
	uint32_t address;
	uint32_t data;
	uint32_t negated;
	uint32_t operation;
	uint32_t predicate;
	int is_signed;
	int shared;
	int error;

	/* The operation and its type. */
	is_signed = 0;
	switch (instruction->immediate) {
	case DRV_GPU_IR_ATOMIC_ADD:
	case DRV_GPU_IR_ATOMIC_SUB:
		operation = NVRTX_SM75_ATOM_ADD;
		break;
	case DRV_GPU_IR_ATOMIC_AND:
		operation = NVRTX_SM75_ATOM_AND;
		break;
	case DRV_GPU_IR_ATOMIC_OR:
		operation = NVRTX_SM75_ATOM_OR;
		break;
	case DRV_GPU_IR_ATOMIC_XOR:
		operation = NVRTX_SM75_ATOM_XOR;
		break;
	case DRV_GPU_IR_ATOMIC_XCHG:
	case DRV_GPU_IR_ATOMIC_CMPXCHG:
		operation = NVRTX_SM75_ATOM_EXCH;
		break;
	case DRV_GPU_IR_ATOMIC_SMIN:
		operation = NVRTX_SM75_ATOM_MIN;
		is_signed = 1;
		break;
	case DRV_GPU_IR_ATOMIC_SMAX:
		operation = NVRTX_SM75_ATOM_MAX;
		is_signed = 1;
		break;
	case DRV_GPU_IR_ATOMIC_UMIN:
		operation = NVRTX_SM75_ATOM_MIN;
		break;
	case DRV_GPU_IR_ATOMIC_UMAX:
		operation = NVRTX_SM75_ATOM_MAX;
		break;
	default:
		error = nvrtx_sm75_refuse(lowering, EINVAL, "an atomic operation the IR does not define");
		return error;
	}

	/* Where the word is: shared memory by its offset, or a storage buffer's word by its address. */
	shared = 0;
	if (instruction->location == DRV_GPU_IR_LOCATION_SHARED)
		shared = 1;
	address = lowering->reg[instruction->src[0]];
	if (shared == 0) {
		if (instruction->location >= lowering->ir->uniform_count || lowering->ir->uniforms[instruction->location].kind != DRV_GPU_IR_UNIFORM_STORAGE) {
			error = nvrtx_sm75_refuse(lowering, EINVAL, "an atomic names no storage buffer");
			return error;
		}

		/* The word's address. */
		error = nvrtx_sm75_storage_address(lowering, lowering->slot[instruction->location], address, &address);
		if (error != 0)
			return error;
	}

	/* A subtraction adds the negated value. */
	data = lowering->reg[instruction->src[1]];
	negated = NVRTX_SM75_RZ;
	error = 0;
	if (instruction->immediate == DRV_GPU_IR_ATOMIC_SUB) {
		error = nvrtx_sm75_take(lowering, &negated);
		if (error == 0) {
			a = nvrtx_sm75_reg(data);
			a.neg = 1;
			zero = nvrtx_sm75_reg(NVRTX_SM75_RZ);
			nvrtx_sm75_begin(&word);
			nvrtx_sm75_iadd3(&word, negated, &a, &zero, &zero);
			error = nvrtx_sm75_emit(lowering, &word, NVRTX_SM75_FIXED);
			data = negated;
		}
	}

	/* P0 from the predicate, which follows the values. */
	if (error == 0 && instruction->component == DRV_GPU_IR_ATOMIC_PREDICATED) {
		predicate = instruction->src[2];
		if (instruction->immediate == DRV_GPU_IR_ATOMIC_CMPXCHG)
			predicate = instruction->src[3];
		error = nvrtx_sm75_predicate(lowering, predicate);
	}

	/* The atomic. */
	if (error == 0) {
		nvrtx_sm75_begin(&word);
		if (instruction->immediate == DRV_GPU_IR_ATOMIC_CMPXCHG) {
			nvrtx_sm75_atomic_cas(&word, shared, dst, address, lowering->reg[instruction->src[2]], data);
		} else {
			nvrtx_sm75_atomic(&word, shared, dst, address, data, operation, is_signed);
		}

		/* A predicated one runs under P0. */
		if (instruction->component == DRV_GPU_IR_ATOMIC_PREDICATED)
			nvrtx_sm75_guard(&word, NVRTX_SM75_P0, 0);

		/* Appends the atomic. */
		error = nvrtx_sm75_emit(lowering, &word, NVRTX_SM75_VARIABLE_WRITE);
	}

	/* The temporaries go back. */
	if (negated != NVRTX_SM75_RZ)
		nvrtx_sm75_give(lowering, negated);
	if (shared == 0) {
		nvrtx_sm75_give(lowering, address);
		nvrtx_sm75_give(lowering, address + 1U);
	}

	/* Reports an emission past the code's room. */
	if (error != 0)
		return error;

	/* Succeeded: the old value is in dst. */
	return 0;
}

/*
 * Lowers a load or a store of a shared memory word, under P0 when predicated.
 */
int
nvrtx_sm75_lower_shared(
	struct nvrtx_sm75_lowering *lowering,
	const struct drv_gpu_shader_ir_inst *instruction,
	uint32_t dst)
{
	struct nvrtx_sm75_word word;
	uint32_t predicate;
	uint32_t offset;
	int error;

	/* The word's offset in shared memory. */
	offset = lowering->reg[instruction->src[0]];

	/* P0 from the predicate of a predicated access. */
	if (instruction->component != 0U) {
		predicate = instruction->src[1];
		if (instruction->op == DRV_GPU_IR_STORE_SHARED)
			predicate = instruction->src[2];
		error = nvrtx_sm75_predicate(lowering, predicate);
		if (error != 0)
			return error;
	}

	/* The load or the store. */
	nvrtx_sm75_begin(&word);
	if (instruction->op == DRV_GPU_IR_LOAD_SHARED) {
		nvrtx_sm75_lds(&word, dst, offset);
	} else {
		nvrtx_sm75_sts(&word, offset, lowering->reg[instruction->src[1]]);
	}

	/* A predicated one runs under P0. */
	if (instruction->component != 0U)
		nvrtx_sm75_guard(&word, NVRTX_SM75_P0, 0);

	/* A load writes a result; a store only reads its sources. */
	if (instruction->op == DRV_GPU_IR_LOAD_SHARED) {
		error = nvrtx_sm75_emit(lowering, &word, NVRTX_SM75_VARIABLE_WRITE);
	} else {
		error = nvrtx_sm75_emit(lowering, &word, NVRTX_SM75_VARIABLE_STORE);
	}

	/* Reports an emission past the code's room. */
	if (error != 0)
		return error;

	/* Succeeded: the access is made. */
	return 0;
}

/*
 * Lowers a workgroup barrier (after the memory barrier of global memory it
 * names; BAR.SYNC itself orders the workgroup's shared memory) and a memory
 * fence (MEMBAR at the GPU's scope for global memory, the workgroup's
 * otherwise).  Each is followed by a NOP of two cycles.
 *
 * Value referenced: the SM75 code the SASS compiler emits (a NOP of two
 * cycles after an instruction with an execution latency), and Mesa 25.3.6
 * src/nouveau/compiler/nak/calc_instr_deps.rs (calc_delays: the NOP with a
 * delay of 2 after an instruction whose execution latency is above 1) and
 * src/nouveau/compiler/nak/sm70.rs (ShaderModel70::exec_latency: BAR and
 * MEMBAR).
 *
 * Procedure referenced: Mesa 25.3.6 src/nouveau/compiler/nak/from_nir.rs
 * (ShaderFromNir::parse_intrinsic, nir_intrinsic_barrier: the MEMBAR's
 * scope, the GPU's for global memory and the workgroup's otherwise).
 */
int
nvrtx_sm75_lower_sync(
	struct nvrtx_sm75_lowering *lowering,
	const struct drv_gpu_shader_ir_inst *instruction)
{
	struct nvrtx_sm75_word word;
	uint32_t scope;
	int membar;
	int error;

	/* Whether a MEMBAR comes, and its scope. */
	scope = NVRTX_SM75_MEMBAR_CTA;
	if ((instruction->immediate & DRV_GPU_IR_FENCE_GLOBAL) != 0U)
		scope = NVRTX_SM75_MEMBAR_GPU;
	membar = 1;
	if (instruction->op == DRV_GPU_IR_BARRIER && scope != NVRTX_SM75_MEMBAR_GPU)
		membar = 0;

	/* The MEMBAR and its NOP. */
	if (membar != 0) {
		nvrtx_sm75_begin(&word);
		nvrtx_sm75_membar(&word, scope);
		error = nvrtx_sm75_emit(lowering, &word, NVRTX_SM75_SYNC);
		if (error != 0)
			return error;

		/* The NOP of two cycles after it. */
		nvrtx_sm75_begin(&word);
		nvrtx_sm75_nop(&word);
		error = nvrtx_sm75_emit(lowering, &word, NVRTX_SM75_FIXED);
		if (error != 0)
			return error;
	}

	/* A fence is only the MEMBAR. */
	if (instruction->op == DRV_GPU_IR_FENCE)
		return 0;

	/* The barrier and its NOP. */
	nvrtx_sm75_begin(&word);
	nvrtx_sm75_bar(&word);
	error = nvrtx_sm75_emit(lowering, &word, NVRTX_SM75_SYNC);
	if (error != 0)
		return error;

	/* The NOP of two cycles after it. */
	nvrtx_sm75_begin(&word);
	nvrtx_sm75_nop(&word);
	error = nvrtx_sm75_emit(lowering, &word, NVRTX_SM75_FIXED);
	if (error != 0)
		return error;

	/* Succeeded: the workgroup has met. */
	return 0;
}

/*
 * Computes a storage word's address into a new register pair: the
 * buffer's 64-bit address (LDC.64 from the table) plus the 32-bit byte
 * offset (IMAD.WIDE.U32 offset * 1 + address).
 */
static int
nvrtx_sm75_storage_address(
	struct nvrtx_sm75_lowering *lowering,
	uint32_t slot,
	uint32_t offset,
	uint32_t *address)
{
	struct nvrtx_sm75_word word;
	struct nvrtx_sm75_src a;
	struct nvrtx_sm75_src one;
	struct nvrtx_sm75_src base_pair;
	uint32_t base;
	int error;

	/* The pair the buffer's address is loaded into. */
	error = nvrtx_sm75_take_pair(lowering, &base);
	if (error != 0)
		return error;

	/* The pair the word's address goes into. */
	error = nvrtx_sm75_take_pair(lowering, address);
	if (error != 0) {
		nvrtx_sm75_give(lowering, base);
		nvrtx_sm75_give(lowering, base + 1U);
		return error;
	}

	/* The buffer's address from its entry. */
	nvrtx_sm75_begin(&word);
	nvrtx_sm75_ldc(&word, base, NVRTX_SM75_CB_ROOT, NVRTX_SM75_CB_STORAGE + slot * NVRTX_SM75_CB_STORAGE_ENTRY, 1);
	error = nvrtx_sm75_emit(lowering, &word, NVRTX_SM75_VARIABLE_WRITE);

	/* The address plus the offset. */
	if (error == 0) {
		a = nvrtx_sm75_reg(offset);
		one = nvrtx_sm75_imm(1U);
		base_pair = nvrtx_sm75_reg(base);
		nvrtx_sm75_begin(&word);
		nvrtx_sm75_imad_wide(&word, *address, &a, &one, &base_pair);
		error = nvrtx_sm75_emit(lowering, &word, NVRTX_SM75_FIXED);
	}

	/* The buffer's address is no longer needed. */
	nvrtx_sm75_give(lowering, base);
	nvrtx_sm75_give(lowering, base + 1U);

	/* An emission past the code's room gives the word's pair back too. */
	if (error != 0) {
		nvrtx_sm75_give(lowering, *address);
		nvrtx_sm75_give(lowering, *address + 1U);
		return error;
	}

	/* Succeeded: the pair holds the word's address. */
	return 0;
}
