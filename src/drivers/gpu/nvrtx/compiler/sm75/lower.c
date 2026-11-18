/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The lowering of the IR's arithmetic, comparisons, special functions,
 * built-in values and loops into SM75 instructions (see lower.h), and the
 * choice of the lowering for each IR instruction.
 */

#include "drivers/gpu/nvrtx/compiler/sm75/lower.h"

#include <kern/kcrt.h>
#include <uapi/errno.h>

/*
 * All ones, the Boolean true; the sign bit; and 1 / (2 pi) as a float, which
 * the sine and cosine units take.
 *
 * Value referenced: Mesa 25.3.6 src/nouveau/compiler/nak/builder.rs
 * (SSABuilder::fsin and SSABuilder::fcos: the argument scaled by 1 / (2 pi)
 * before MUFU.SIN and MUFU.COS, NVRTX_SM75_INV_TWO_PI).
 */
#define NVRTX_SM75_TRUE			0xffffffffU
#define NVRTX_SM75_SIGN			0x80000000U
#define NVRTX_SM75_INV_TWO_PI		0x3e22f983U

static int nvrtx_sm75_lower_alu(struct nvrtx_sm75_lowering *lowering, const struct drv_gpu_shader_ir_inst *instruction, uint32_t dst);
static int nvrtx_sm75_lower_fma(struct nvrtx_sm75_lowering *lowering, const struct drv_gpu_shader_ir_inst *instruction, uint32_t side, uint32_t dst);
static int nvrtx_sm75_lower_compare(struct nvrtx_sm75_lowering *lowering, const struct drv_gpu_shader_ir_inst *instruction, uint32_t dst);
static int nvrtx_sm75_lower_unit(struct nvrtx_sm75_lowering *lowering, const struct drv_gpu_shader_ir_inst *instruction, uint32_t dst);
static int nvrtx_sm75_lower_system(struct nvrtx_sm75_lowering *lowering, const struct drv_gpu_shader_ir_inst *instruction, uint32_t dst);
static int nvrtx_sm75_lower_loop(struct nvrtx_sm75_lowering *lowering, const struct drv_gpu_shader_ir_inst *instruction);

/*
 * Lowers one IR instruction into dst (RZ when it has no result).
 */
int
nvrtx_sm75_lower(
	struct nvrtx_sm75_lowering *lowering,
	const struct drv_gpu_shader_ir_inst *instruction,
	uint32_t dst)
{
	struct nvrtx_sm75_word word;
	struct nvrtx_sm75_src source;
	uint32_t end;
	int error;

	/* The operations of their own families, then the moves of a value. */
	switch (instruction->op) {
	case DRV_GPU_IR_NOP:
	case DRV_GPU_IR_SKIP_BEGIN:
	case DRV_GPU_IR_SKIP_END:
		return 0;
	case DRV_GPU_IR_FLT:
	case DRV_GPU_IR_FGE:
	case DRV_GPU_IR_FEQ:
	case DRV_GPU_IR_FNEU:
	case DRV_GPU_IR_ILT:
	case DRV_GPU_IR_IGE:
	case DRV_GPU_IR_ULT:
	case DRV_GPU_IR_UGE:
	case DRV_GPU_IR_IEQ:
	case DRV_GPU_IR_INE:
	case DRV_GPU_IR_SELECT:
		error = nvrtx_sm75_lower_compare(lowering, instruction, dst);
		return error;
	case DRV_GPU_IR_RSQ:
	case DRV_GPU_IR_RCP:
	case DRV_GPU_IR_SQRT:
	case DRV_GPU_IR_EXP2:
	case DRV_GPU_IR_LOG2:
	case DRV_GPU_IR_SIN:
	case DRV_GPU_IR_COS:
	case DRV_GPU_IR_FLOOR:
	case DRV_GPU_IR_FRACT:
	case DRV_GPU_IR_FTRUNC:
	case DRV_GPU_IR_FROUND_EVEN:
	case DRV_GPU_IR_I2F:
	case DRV_GPU_IR_U2F:
	case DRV_GPU_IR_F2I:
	case DRV_GPU_IR_F2U:
		error = nvrtx_sm75_lower_unit(lowering, instruction, dst);
		return error;
	case DRV_GPU_IR_LOAD_SYSTEM:
		if (lowering->ir->stage == DRV_GPU_STAGE_GEOMETRY) {
			error = nvrtx_sm75_lower_geometry_system(lowering, instruction, dst);
			return error;
		}
		error = nvrtx_sm75_lower_system(lowering, instruction, dst);
		return error;
	case DRV_GPU_IR_LOAD_VERTEX_INPUT:
		error = nvrtx_sm75_lower_vertex_input(lowering, instruction, dst);
		return error;
	case DRV_GPU_IR_EMIT_VERTEX:
	case DRV_GPU_IR_END_PRIMITIVE:
		error = nvrtx_sm75_lower_emit(lowering, instruction);
		return error;
	case DRV_GPU_IR_LOAD_STORAGE:
	case DRV_GPU_IR_STORE_STORAGE:
	case DRV_GPU_IR_STORAGE_SIZE:
		error = nvrtx_sm75_lower_storage(lowering, instruction, dst);
		return error;
	case DRV_GPU_IR_LOOP_BEGIN:
	case DRV_GPU_IR_LOOP_END:
		error = nvrtx_sm75_lower_loop(lowering, instruction);
		return error;
	case DRV_GPU_IR_UDIV:
	case DRV_GPU_IR_UMOD:
	case DRV_GPU_IR_IDIV:
	case DRV_GPU_IR_IREM:
		error = nvrtx_sm75_lower_divide(lowering, instruction, dst);
		return error;
	case DRV_GPU_IR_ATOMIC:
		error = nvrtx_sm75_lower_atomic(lowering, instruction, dst);
		return error;
	case DRV_GPU_IR_LOAD_SHARED:
	case DRV_GPU_IR_STORE_SHARED:
		error = nvrtx_sm75_lower_shared(lowering, instruction, dst);
		return error;
	case DRV_GPU_IR_BARRIER:
	case DRV_GPU_IR_FENCE:
		error = nvrtx_sm75_lower_sync(lowering, instruction);
		return error;
	case DRV_GPU_IR_LOAD_INPUT:
		error = nvrtx_sm75_lower_input(lowering, instruction, dst);
		return error;
	case DRV_GPU_IR_STORE_OUTPUT:
		error = nvrtx_sm75_lower_output(lowering, instruction);
		return error;
	case DRV_GPU_IR_SAMPLE:
	case DRV_GPU_IR_SAMPLE_LOD:
	case DRV_GPU_IR_SAMPLE_BIAS:
	case DRV_GPU_IR_TEXTURE:
		error = nvrtx_sm75_lower_sample(lowering, instruction);
		return error;
	case DRV_GPU_IR_KILL:
		error = nvrtx_sm75_lower_kill(lowering, instruction);
		return error;
	case DRV_GPU_IR_DDX:
	case DRV_GPU_IR_DDX_FINE:
	case DRV_GPU_IR_DDY:
	case DRV_GPU_IR_DDY_FINE:
		error = nvrtx_sm75_lower_derivative(lowering, instruction, dst);
		return error;
	case DRV_GPU_IR_PACK_HALF:
	case DRV_GPU_IR_UNPACK_HALF:
		error = nvrtx_sm75_lower_half(lowering, instruction, dst);
		return error;
	case DRV_GPU_IR_CONST:
	case DRV_GPU_IR_ICONST:
	case DRV_GPU_IR_BOOL:
		source = nvrtx_sm75_imm(instruction->immediate);
		break;
	case DRV_GPU_IR_LOAD_PUSH:
		/* A push constant word lies in the first 256 bytes of constant buffer 0. */
		end = instruction->immediate + 4U;
		if (end > NVRTX_SM75_CB_PUSH_BYTES ||
		    end < instruction->immediate ||
		    (instruction->immediate & 3U) != 0U) {
			error = nvrtx_sm75_refuse(lowering, ENOTSUP, "a push constant beyond the 256 bytes of the root buffer");
			return error;
		}

		/* The program reads the push constants up to this word. */
		if (end > lowering->binary->push_bytes)
			lowering->binary->push_bytes = end;

		/* The word in constant buffer 0. */
		source = nvrtx_sm75_cb(NVRTX_SM75_CB_ROOT, NVRTX_SM75_CB_PUSH + instruction->immediate);
		break;
	case DRV_GPU_IR_LOAD_UBO:
		/*
		 * A uniform buffer word at an offset the constant buffer field
		 * holds.
		 *
		 * Value referenced: Mesa 25.3.6
		 * src/nouveau/compiler/nak/sm70_encode.rs
		 * (SM70Encoder::set_src_cb: the offset field of 16 bits).
		 */
		if (instruction->location >= lowering->ir->uniform_count || lowering->ir->uniforms[instruction->location].kind != DRV_GPU_IR_UNIFORM_BLOCK) {
			error = nvrtx_sm75_refuse(lowering, EINVAL, "a uniform load names no uniform buffer");
			return error;
		}

		/* The offset is a whole word the field holds. */
		if (instruction->immediate > 0xfffcU || (instruction->immediate & 3U) != 0U) {
			error = nvrtx_sm75_refuse(lowering, ENOTSUP, "a uniform buffer offset beyond 64 KiB");
			return error;
		}

		/* A buffer past the constant buffers is read from memory. */
		if ((lowering->slot[instruction->location] & NVRTX_SM75_SLOT_TABLE) != 0U) {
			if (dst == NVRTX_SM75_RZ)
				return 0;
			error = nvrtx_sm75_lower_uniform_load(lowering, instruction, dst);
			return error;
		}

		/* The word in the uniform buffer's constant buffer. */
		source = nvrtx_sm75_cb(lowering->slot[instruction->location], instruction->immediate);
		break;
	case DRV_GPU_IR_MOVE:
		source = nvrtx_sm75_value(lowering, instruction->src[0]);
		break;
	default:
		error = nvrtx_sm75_lower_alu(lowering, instruction, dst);
		return error;
	}

	/* A value no instruction reads needs no move. */
	if (dst == NVRTX_SM75_RZ)
		return 0;

	/* MOV dst, the source. */
	nvrtx_sm75_begin(&word);
	nvrtx_sm75_mov(&word, dst, &source);
	error = nvrtx_sm75_emit(lowering, &word, NVRTX_SM75_FIXED);
	if (error != 0)
		return error;

	/* Succeeded: the value is in its register. */
	return 0;
}

/*
 * Sets P0 from a Boolean value: it is not zero.
 */
int
nvrtx_sm75_predicate(
	struct nvrtx_sm75_lowering *lowering,
	uint32_t value)
{
	struct nvrtx_sm75_word word;
	struct nvrtx_sm75_src a;
	struct nvrtx_sm75_src zero;
	int error;

	/* The Boolean and RZ, the zero register, as operands. */
	a = nvrtx_sm75_value(lowering, value);
	zero = nvrtx_sm75_reg(NVRTX_SM75_RZ);

	/* ISETP.NE.U32.AND P0, PT, value, RZ, PT. */
	nvrtx_sm75_begin(&word);
	nvrtx_sm75_isetp(&word, NVRTX_SM75_P0, NVRTX_SM75_ICMP_NE, 0, &a, &zero);
	error = nvrtx_sm75_emit(lowering, &word, NVRTX_SM75_FIXED_PREDICATE);
	if (error != 0)
		return error;

	/* Succeeded: P0 holds the Boolean. */
	return 0;
}

/* Lowers the float and integer arithmetic and the bitwise operations. */
static int
nvrtx_sm75_lower_alu(
	struct nvrtx_sm75_lowering *lowering,
	const struct drv_gpu_shader_ir_inst *instruction,
	uint32_t dst)
{
	struct nvrtx_sm75_word word;
	struct nvrtx_sm75_src a;
	struct nvrtx_sm75_src b;
	struct nvrtx_sm75_src zero;
	struct nvrtx_sm75_src mask;
	struct nvrtx_sm75_src swap;
	int error;

	/* An addition or subtraction that reads a fused product is an FFMA. */
	if (instruction->op == DRV_GPU_IR_FADD || instruction->op == DRV_GPU_IR_FSUB) {
		if (lowering->fold[instruction->src[0]] == NVRTX_SM75_FOLD_PRODUCT) {
			error = nvrtx_sm75_lower_fma(lowering, instruction, 0U, dst);
			return error;
		}
		if (lowering->fold[instruction->src[1]] == NVRTX_SM75_FOLD_PRODUCT) {
			error = nvrtx_sm75_lower_fma(lowering, instruction, 1U, dst);
			return error;
		}
	}

	/* The instruction, its first source, and RZ for a second source it may not have. */
	nvrtx_sm75_begin(&word);
	zero = nvrtx_sm75_reg(NVRTX_SM75_RZ);
	a = nvrtx_sm75_operand(lowering, instruction->src[0]);
	b = zero;

	/* The second source of the binary operations. */
	if (instruction->op != DRV_GPU_IR_FNEG &&
	    instruction->op != DRV_GPU_IR_FABS &&
	    instruction->op != DRV_GPU_IR_NOT &&
	    instruction->op != DRV_GPU_IR_INOT &&
	    instruction->op != DRV_GPU_IR_INEG)
		b = nvrtx_sm75_operand(lowering, instruction->src[1]);

	/*
	 * A word read in place (nvrtx_sm75_plan_folds() lets only these
	 * operations have one, never two) goes to the second position: the
	 * operations commute, and a subtraction becomes the negated subtrahend
	 * plus the word.
	 */
	if (a.kind != NVRTX_SM75_SRC_REG) {
		swap = a;
		a = b;
		b = swap;
		if (instruction->op == DRV_GPU_IR_FSUB)
			a.neg = 1;
	}

	/* The SM75 instruction of the operation. */
	switch (instruction->op) {
	case DRV_GPU_IR_FADD:
		nvrtx_sm75_fadd(&word, dst, &a, &b);
		break;
	case DRV_GPU_IR_FSUB:
		/* The subtrahend negated, unless the swap above made the word the second source. */
		if (a.neg == 0)
			b.neg = 1;
		nvrtx_sm75_fadd(&word, dst, &a, &b);
		break;
	case DRV_GPU_IR_FMUL:
		nvrtx_sm75_fmul(&word, dst, &a, &b);
		break;
	case DRV_GPU_IR_FMIN:
		nvrtx_sm75_fmnmx(&word, dst, &a, &b, 1);
		break;
	case DRV_GPU_IR_FMAX:
		nvrtx_sm75_fmnmx(&word, dst, &a, &b, 0);
		break;
	case DRV_GPU_IR_FNEG:
		/* The sign bit flipped, exactly (also for zero and NaN). */
		mask = nvrtx_sm75_imm(NVRTX_SM75_SIGN);
		nvrtx_sm75_lop3(&word, dst, &a, &mask, &zero, NVRTX_SM75_LUT_XOR);
		break;
	case DRV_GPU_IR_FABS:
		/* The sign bit cleared. */
		mask = nvrtx_sm75_imm(~NVRTX_SM75_SIGN);
		nvrtx_sm75_lop3(&word, dst, &a, &mask, &zero, NVRTX_SM75_LUT_AND);
		break;
	case DRV_GPU_IR_IADD:
		nvrtx_sm75_iadd3(&word, dst, &a, &b, &zero);
		break;
	case DRV_GPU_IR_ISUB:
		b.neg = 1;
		nvrtx_sm75_iadd3(&word, dst, &a, &b, &zero);
		break;
	case DRV_GPU_IR_INEG:
		a.neg = 1;
		nvrtx_sm75_iadd3(&word, dst, &a, &zero, &zero);
		break;
	case DRV_GPU_IR_IMUL:
		nvrtx_sm75_imad(&word, dst, &a, &b, &zero, 0);
		break;
	case DRV_GPU_IR_AND:
	case DRV_GPU_IR_IAND:
		nvrtx_sm75_lop3(&word, dst, &a, &b, &zero, NVRTX_SM75_LUT_AND);
		break;
	case DRV_GPU_IR_OR:
	case DRV_GPU_IR_IOR:
		nvrtx_sm75_lop3(&word, dst, &a, &b, &zero, NVRTX_SM75_LUT_OR);
		break;
	case DRV_GPU_IR_IXOR:
		nvrtx_sm75_lop3(&word, dst, &a, &b, &zero, NVRTX_SM75_LUT_XOR);
		break;
	case DRV_GPU_IR_NOT:
	case DRV_GPU_IR_INOT:
		nvrtx_sm75_lop3(&word, dst, &a, &zero, &zero, NVRTX_SM75_LUT_NOT);
		break;
	case DRV_GPU_IR_SHL:
		nvrtx_sm75_shift(&word, dst, a.value, &b, 0, 0);
		break;
	case DRV_GPU_IR_SHR:
		nvrtx_sm75_shift(&word, dst, a.value, &b, 1, 0);
		break;
	case DRV_GPU_IR_ASR:
		nvrtx_sm75_shift(&word, dst, a.value, &b, 1, 1);
		break;
	default:
		error = nvrtx_sm75_refuse(lowering, ENOTSUP, "the operation is not compiled yet");
		return error;
	}

	/* One instruction of fixed latency. */
	error = nvrtx_sm75_emit(lowering, &word, NVRTX_SM75_FIXED);
	if (error != 0)
		return error;

	/* Succeeded: the result is in dst. */
	return 0;
}

/*
 * Lowers an addition or subtraction whose source on a side is a fused
 * product into one FFMA: x + a * b, x - a * b as -a * b + x, and a * b - x
 * as a * b + -x.  A factor read in place goes to the second position, an
 * addend read in place to the third (nvrtx_sm75_plan_folds() allows one, and
 * never a negated one).
 */
static int
nvrtx_sm75_lower_fma(
	struct nvrtx_sm75_lowering *lowering,
	const struct drv_gpu_shader_ir_inst *instruction,
	uint32_t side,
	uint32_t dst)
{
	const struct drv_gpu_shader_ir_inst *product;
	struct nvrtx_sm75_word word;
	struct nvrtx_sm75_src a;
	struct nvrtx_sm75_src b;
	struct nvrtx_sm75_src c;
	struct nvrtx_sm75_src swap;
	int error;

	/* The product's factors and the other source, the addend. */
	product = &lowering->ir->instructions[lowering->def[instruction->src[side]]];
	a = nvrtx_sm75_operand(lowering, product->src[0]);
	b = nvrtx_sm75_operand(lowering, product->src[1]);
	c = nvrtx_sm75_operand(lowering, instruction->src[1U - side]);

	/* A factor read in place is the second. */
	if (a.kind != NVRTX_SM75_SRC_REG) {
		swap = a;
		a = b;
		b = swap;
	}

	/* A subtraction negates the product (through its first factor, a register) or the addend. */
	if (instruction->op == DRV_GPU_IR_FSUB && side == 1U)
		a.neg = 1;
	if (instruction->op == DRV_GPU_IR_FSUB && side == 0U)
		c.neg = 1;

	/* FFMA dst = a * b + c, one instruction of fixed latency. */
	nvrtx_sm75_begin(&word);
	nvrtx_sm75_ffma(&word, dst, &a, &b, &c);
	error = nvrtx_sm75_emit(lowering, &word, NVRTX_SM75_FIXED);
	if (error != 0)
		return error;

	/* Succeeded: the result is in dst. */
	return 0;
}

/*
 * Lowers a comparison into P0 and then a Boolean, and a selection by a
 * Boolean.
 */
static int
nvrtx_sm75_lower_compare(
	struct nvrtx_sm75_lowering *lowering,
	const struct drv_gpu_shader_ir_inst *instruction,
	uint32_t dst)
{
	struct nvrtx_sm75_word word;
	struct nvrtx_sm75_src a;
	struct nvrtx_sm75_src b;
	struct nvrtx_sm75_src zero;
	struct nvrtx_sm75_src truth;
	int error;

	/* RZ, the Boolean true, and the two sources as operands. */
	zero = nvrtx_sm75_reg(NVRTX_SM75_RZ);
	truth = nvrtx_sm75_imm(NVRTX_SM75_TRUE);
	a = nvrtx_sm75_value(lowering, instruction->src[0]);
	b = nvrtx_sm75_value(lowering, instruction->src[1]);

	/* P0 from the comparison, or from the selection's condition. */
	nvrtx_sm75_begin(&word);
	switch (instruction->op) {
	case DRV_GPU_IR_FLT:
		nvrtx_sm75_fsetp(&word, NVRTX_SM75_P0, NVRTX_SM75_FCMP_LT, &a, &b);
		break;
	case DRV_GPU_IR_FGE:
		nvrtx_sm75_fsetp(&word, NVRTX_SM75_P0, NVRTX_SM75_FCMP_GE, &a, &b);
		break;
	case DRV_GPU_IR_FEQ:
		nvrtx_sm75_fsetp(&word, NVRTX_SM75_P0, NVRTX_SM75_FCMP_EQ, &a, &b);
		break;
	case DRV_GPU_IR_FNEU:
		nvrtx_sm75_fsetp(&word, NVRTX_SM75_P0, NVRTX_SM75_FCMP_UNORD_NE, &a, &b);
		break;
	case DRV_GPU_IR_ILT:
		nvrtx_sm75_isetp(&word, NVRTX_SM75_P0, NVRTX_SM75_ICMP_LT, 1, &a, &b);
		break;
	case DRV_GPU_IR_IGE:
		nvrtx_sm75_isetp(&word, NVRTX_SM75_P0, NVRTX_SM75_ICMP_GE, 1, &a, &b);
		break;
	case DRV_GPU_IR_ULT:
		nvrtx_sm75_isetp(&word, NVRTX_SM75_P0, NVRTX_SM75_ICMP_LT, 0, &a, &b);
		break;
	case DRV_GPU_IR_UGE:
		nvrtx_sm75_isetp(&word, NVRTX_SM75_P0, NVRTX_SM75_ICMP_GE, 0, &a, &b);
		break;
	case DRV_GPU_IR_IEQ:
		nvrtx_sm75_isetp(&word, NVRTX_SM75_P0, NVRTX_SM75_ICMP_EQ, 0, &a, &b);
		break;
	case DRV_GPU_IR_INE:
		nvrtx_sm75_isetp(&word, NVRTX_SM75_P0, NVRTX_SM75_ICMP_NE, 0, &a, &b);
		break;
	default:
		/* SELECT: P0 is the condition (src[0]) being nonzero. */
		nvrtx_sm75_isetp(&word, NVRTX_SM75_P0, NVRTX_SM75_ICMP_NE, 0, &a, &zero);
		break;
	}

	/* Appends the comparison. */
	error = nvrtx_sm75_emit(lowering, &word, NVRTX_SM75_FIXED_PREDICATE);
	if (error != 0)
		return error;

	/* A result no instruction reads is not made. */
	if (dst == NVRTX_SM75_RZ)
		return 0;

	/* The result: the selected source, or the Boolean of P0. */
	nvrtx_sm75_begin(&word);
	if (instruction->op == DRV_GPU_IR_SELECT) {
		a = nvrtx_sm75_value(lowering, instruction->src[1]);
		b = nvrtx_sm75_value(lowering, instruction->src[2]);
		nvrtx_sm75_sel(&word, dst, &a, &b, NVRTX_SM75_P0, 0);
	} else {
		nvrtx_sm75_sel(&word, dst, &zero, &truth, NVRTX_SM75_P0, 1);
	}

	/* Appends the selection. */
	error = nvrtx_sm75_emit(lowering, &word, NVRTX_SM75_FIXED);
	if (error != 0)
		return error;

	/* Succeeded: the result is in dst. */
	return 0;
}

/*
 * Lowers the operations of the units of variable latency: the special
 * functions (sine and cosine take the angle in turns), the conversions and
 * the roundings (the fraction is x - floor(x)).
 */
static int
nvrtx_sm75_lower_unit(
	struct nvrtx_sm75_lowering *lowering,
	const struct drv_gpu_shader_ir_inst *instruction,
	uint32_t dst)
{
	struct nvrtx_sm75_word word;
	struct nvrtx_sm75_src a;
	struct nvrtx_sm75_src b;
	struct nvrtx_sm75_src turns;
	uint32_t source;
	uint32_t temporary;
	uint32_t function;
	int error;

	/* The source's register, and no temporary yet. */
	source = lowering->reg[instruction->src[0]];
	temporary = NVRTX_SM75_RZ;

	/*
	 * Sine and cosine first scale the angle into turns.
	 *
	 * Procedure referenced: Mesa 25.3.6
	 * src/nouveau/compiler/nak/builder.rs (SSABuilder::fsin and
	 * SSABuilder::fcos: the angle times 1 / (2 pi) from SM70 on).
	 */
	if (instruction->op == DRV_GPU_IR_SIN || instruction->op == DRV_GPU_IR_COS) {
		error = nvrtx_sm75_take(lowering, &temporary);
		if (error != 0)
			return error;

		/* The angle times 1 / (2 pi) into the temporary. */
		a = nvrtx_sm75_reg(source);
		turns = nvrtx_sm75_imm(NVRTX_SM75_INV_TWO_PI);
		nvrtx_sm75_begin(&word);
		nvrtx_sm75_fmul(&word, temporary, &a, &turns);
		error = nvrtx_sm75_emit(lowering, &word, NVRTX_SM75_FIXED);
		if (error != 0) {
			nvrtx_sm75_give(lowering, temporary);
			return error;
		}

		/* The unit reads the turns. */
		source = temporary;
	}

	/* The fraction rounds into a temporary first. */
	if (instruction->op == DRV_GPU_IR_FRACT) {
		error = nvrtx_sm75_take(lowering, &temporary);
		if (error != 0)
			return error;
	}

	/* The unit's instruction. */
	nvrtx_sm75_begin(&word);
	function = NVRTX_SM75_MUFU_RCP;
	switch (instruction->op) {
	case DRV_GPU_IR_RSQ:
		function = NVRTX_SM75_MUFU_RSQ;
		break;
	case DRV_GPU_IR_SQRT:
		function = NVRTX_SM75_MUFU_SQRT;
		break;
	case DRV_GPU_IR_EXP2:
		function = NVRTX_SM75_MUFU_EX2;
		break;
	case DRV_GPU_IR_LOG2:
		function = NVRTX_SM75_MUFU_LG2;
		break;
	case DRV_GPU_IR_SIN:
		function = NVRTX_SM75_MUFU_SIN;
		break;
	case DRV_GPU_IR_COS:
		function = NVRTX_SM75_MUFU_COS;
		break;
	default:
		break;
	}

	/* The conversions and the roundings; the special functions otherwise. */
	switch (instruction->op) {
	case DRV_GPU_IR_I2F:
		nvrtx_sm75_i2f(&word, dst, source, 1);
		break;
	case DRV_GPU_IR_U2F:
		nvrtx_sm75_i2f(&word, dst, source, 0);
		break;
	case DRV_GPU_IR_F2I:
		nvrtx_sm75_f2i(&word, dst, source, 1);
		break;
	case DRV_GPU_IR_F2U:
		nvrtx_sm75_f2i(&word, dst, source, 0);
		break;
	case DRV_GPU_IR_FLOOR:
		nvrtx_sm75_frnd(&word, dst, source, NVRTX_SM75_ROUND_DOWN);
		break;
	case DRV_GPU_IR_FTRUNC:
		nvrtx_sm75_frnd(&word, dst, source, NVRTX_SM75_ROUND_ZERO);
		break;
	case DRV_GPU_IR_FROUND_EVEN:
		nvrtx_sm75_frnd(&word, dst, source, NVRTX_SM75_ROUND_NEAREST);
		break;
	case DRV_GPU_IR_FRACT:
		nvrtx_sm75_frnd(&word, temporary, source, NVRTX_SM75_ROUND_DOWN);
		break;
	default:
		nvrtx_sm75_mufu(&word, dst, function, source);
		break;
	}

	/* Appends the unit's instruction. */
	error = nvrtx_sm75_emit(lowering, &word, NVRTX_SM75_VARIABLE_WRITE);

	/* The fraction subtracts the rounded value. */
	if (error == 0 && instruction->op == DRV_GPU_IR_FRACT) {
		a = nvrtx_sm75_reg(source);
		b = nvrtx_sm75_reg(temporary);
		b.neg = 1;
		nvrtx_sm75_begin(&word);
		nvrtx_sm75_fadd(&word, dst, &a, &b);
		error = nvrtx_sm75_emit(lowering, &word, NVRTX_SM75_FIXED);
	}

	/* The temporary goes back. */
	if (temporary != NVRTX_SM75_RZ)
		nvrtx_sm75_give(lowering, temporary);

	/* Reports an emission past the code's room. */
	if (error != 0)
		return error;

	/* Succeeded: the result is in dst. */
	return 0;
}

/*
 * Lowers a built-in value of the compute invocation: its place in its
 * group, the group's place in the grid, and its linear index in the group
 * (x + width * (y + height * z)).
 */
static int
nvrtx_sm75_lower_system(
	struct nvrtx_sm75_lowering *lowering,
	const struct drv_gpu_shader_ir_inst *instruction,
	uint32_t dst)
{
	struct nvrtx_sm75_word word;
	struct nvrtx_sm75_src a;
	struct nvrtx_sm75_src b;
	struct nvrtx_sm75_src c;
	uint32_t ids[3];
	uint32_t axis;
	uint32_t special;
	int error;

	/* The register of a single built-in. */
	special = 0U;
	switch (instruction->component) {
	case DRV_GPU_IR_SYSTEM_LOCAL_ID_X:
		special = NVRTX_SM75_SR_TID_X;
		break;
	case DRV_GPU_IR_SYSTEM_LOCAL_ID_Y:
		special = NVRTX_SM75_SR_TID_Y;
		break;
	case DRV_GPU_IR_SYSTEM_LOCAL_ID_Z:
		special = NVRTX_SM75_SR_TID_Z;
		break;
	case DRV_GPU_IR_SYSTEM_GROUP_ID_X:
		special = NVRTX_SM75_SR_CTAID_X;
		break;
	case DRV_GPU_IR_SYSTEM_GROUP_ID_Y:
		special = NVRTX_SM75_SR_CTAID_Y;
		break;
	case DRV_GPU_IR_SYSTEM_GROUP_ID_Z:
		special = NVRTX_SM75_SR_CTAID_Z;
		break;
	case DRV_GPU_IR_SYSTEM_LOCAL_INDEX:
		break;
	default:
		error = nvrtx_sm75_refuse(lowering, ENOTSUP, "a built-in value a compute shader does not have");
		return error;
	}

	/* A single built-in is one read of its special register. */
	if (special != 0U) {
		nvrtx_sm75_begin(&word);
		nvrtx_sm75_s2r(&word, dst, special);
		error = nvrtx_sm75_emit(lowering, &word, NVRTX_SM75_VARIABLE_SPECIAL);
		if (error != 0)
			return error;

		/* Succeeded: the built-in is in dst. */
		return 0;
	}

	/* The linear index: the three local ids into temporaries. */
	for (axis = 0U; axis < 3U; axis++) {
		error = nvrtx_sm75_take(lowering, &ids[axis]);
		if (error != 0) {
			while (axis > 0U) {
				axis--;
				nvrtx_sm75_give(lowering, ids[axis]);
			}

			/* Reports the shortage of registers, the ones taken given back. */
			return error;
		}

		/* S2R of the axis's local id. */
		nvrtx_sm75_begin(&word);
		nvrtx_sm75_s2r(&word, ids[axis], NVRTX_SM75_SR_TID_X + axis);
		error = nvrtx_sm75_emit(lowering, &word, NVRTX_SM75_VARIABLE_SPECIAL);
		if (error != 0)
			break;
	}

	/* z * height + y into the z temporary, then that * width + x into dst. */
	if (error == 0) {
		a = nvrtx_sm75_reg(ids[2]);
		b = nvrtx_sm75_imm(lowering->ir->local_size[1]);
		c = nvrtx_sm75_reg(ids[1]);
		nvrtx_sm75_begin(&word);
		nvrtx_sm75_imad(&word, ids[2], &a, &b, &c, 0);
		error = nvrtx_sm75_emit(lowering, &word, NVRTX_SM75_FIXED);
	}

	/* That times the width plus x into dst. */
	if (error == 0) {
		a = nvrtx_sm75_reg(ids[2]);
		b = nvrtx_sm75_imm(lowering->ir->local_size[0]);
		c = nvrtx_sm75_reg(ids[0]);
		nvrtx_sm75_begin(&word);
		nvrtx_sm75_imad(&word, dst, &a, &b, &c, 0);
		error = nvrtx_sm75_emit(lowering, &word, NVRTX_SM75_FIXED);
	}

	/* The temporaries go back. */
	for (axis = 0U; axis < 3U; axis++)
		nvrtx_sm75_give(lowering, ids[axis]);

	/* Reports an emission past the code's room. */
	if (error != 0)
		return error;

	/* Succeeded: the index is in dst. */
	return 0;
}

/*
 * Lowers a loop's ends.  The threads of a warp may leave a loop at
 * different passes, so the loop is a convergence region: LOOP_BEGIN is a
 * BSSY whose barrier collects the threads at the BSYNC after the loop, and
 * LOOP_END branches back to the loop's first instruction for the threads
 * whose Boolean holds; the others go on to the BSYNC and wait there.
 *
 * Procedure referenced: Mesa 25.3.6 src/nouveau/compiler/nak_nir_lower_cf.c
 * (lower_cf_list: the loop's break scope, push_scope and pop_scope) and
 * src/nouveau/compiler/nak/from_nir.rs (ShaderFromNir::parse_intrinsic: the
 * OpBSSy of bar_set_nv and the OpBSync of bar_sync_nv).
 */
static int
nvrtx_sm75_lower_loop(
	struct nvrtx_sm75_lowering *lowering,
	const struct drv_gpu_shader_ir_inst *instruction)
{
	struct nvrtx_sm75_word word;
	uint32_t *slot;
	uint32_t depth;
	uint32_t bssy;
	int32_t back;
	int error;

	/* LOOP_BEGIN: the BSSY (its target is set at the loop's end), and the loop starts after it. */
	if (instruction->op == DRV_GPU_IR_LOOP_BEGIN) {
		depth = lowering->loop_depth;
		if (depth == NVRTX_SM75_LOOP_DEPTH) {
			error = nvrtx_sm75_refuse(lowering, ENOTSUP, "loops nested deeper than the convergence barriers");
			return error;
		}

		/* The BSSY, remembered so that the loop's end can set its target. */
		nvrtx_sm75_begin(&word);
		nvrtx_sm75_bssy(&word, depth, 0);
		lowering->loop_bssy[depth] = lowering->count;
		error = nvrtx_sm75_emit(lowering, &word, NVRTX_SM75_CONTROL);
		if (error != 0)
			return error;

		/* Succeeded: the loop starts after the BSSY, one level deeper. */
		lowering->loop_start[depth] = lowering->count;
		lowering->loop_depth++;
		return 0;
	}

	/* LOOP_END closes the innermost loop. */
	if (lowering->loop_depth == 0U) {
		error = nvrtx_sm75_refuse(lowering, EINVAL, "a loop's end without its beginning");
		return error;
	}

	/* The loop being closed is one level out. */
	lowering->loop_depth--;
	depth = lowering->loop_depth;

	/* P0: the thread runs the body again. */
	error = nvrtx_sm75_predicate(lowering, instruction->src[0]);
	if (error != 0)
		return error;

	/* Back to the loop's first instruction where P0 holds. */
	back = (int32_t)lowering->loop_start[depth] - (int32_t)(lowering->count + 1U);
	nvrtx_sm75_begin(&word);
	nvrtx_sm75_bra(&word, back);
	nvrtx_sm75_bra_if(&word, NVRTX_SM75_P0, 0);
	error = nvrtx_sm75_emit(lowering, &word, NVRTX_SM75_CONTROL);
	if (error != 0)
		return error;

	/* The BSYNC the loop's threads meet at, and the BSSY's target made its address. */
	bssy = lowering->loop_bssy[depth];
	nvrtx_sm75_begin(&word);
	nvrtx_sm75_bsync(&word, depth);
	slot = lowering->binary->code + (size_t)bssy * NVRTX_SM75_INSTRUCTION_WORDS;
	error = nvrtx_sm75_emit(lowering, &word, NVRTX_SM75_CONTROL);
	if (error != 0)
		return error;

	/*
	 * The BSSY's offset: from the instruction after the BSSY to the
	 * BSYNC, in 32-bit words.
	 *
	 * Value referenced: Mesa 25.3.6 src/nouveau/compiler/nak/sm70_encode.rs
	 * (SM70Encoder::get_rel_offset: the target's place minus the next
	 * instruction's, in 32-bit words).
	 */
	kern_memcpy(word.w, slot, sizeof(word.w));
	nvrtx_sm75_set(&word, NVRTX_SM75_BSSY_OFFSET_LOW, NVRTX_SM75_BSSY_OFFSET_BITS, (uint64_t)((int64_t)(lowering->count - 1U - (bssy + 1U)) * NVRTX_SM75_INSTRUCTION_WORDS));
	kern_memcpy(slot, word.w, sizeof(word.w));

	/* Succeeded: the loop is closed. */
	return 0;
}
