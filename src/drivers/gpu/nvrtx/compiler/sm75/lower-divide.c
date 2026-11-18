/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The lowering of the integer division and remainder (see lower.h).
 */

#include "drivers/gpu/nvrtx/compiler/sm75/lower.h"

/*
 * 2^32 less 512 as a float (0x4f7ffffe), which scales a reciprocal to a 32-bit fixed-point one.
 *
 * Value referenced: Mesa 25.0.7 src/compiler/nir/nir_lower_idiv.c
 * (emit_udiv).
 */
#define NVRTX_SM75_RECIPROCAL_SCALE	0x4f7ffffeU

static int nvrtx_sm75_divide_unsigned(struct nvrtx_sm75_lowering *lowering, uint32_t numerator, uint32_t denominator, uint32_t quotient, uint32_t remainder);
static int nvrtx_sm75_absolute(struct nvrtx_sm75_lowering *lowering, uint32_t value, uint32_t result);
static int nvrtx_sm75_negate_if_p0(struct nvrtx_sm75_lowering *lowering, uint32_t value);

/*
 * Lowers the integer division and remainder.  The unsigned ones use the
 * float reciprocal of the divisor scaled to 32-bit fixed point, refined
 * once, then a quotient corrected at most twice; the signed ones divide the
 * magnitudes and give the quotient the sign of the operands' product and
 * the remainder the dividend's sign (SPIR-V OpSDiv and OpSRem).
 *
 * Procedure referenced: Mesa 25.0.7 src/compiler/nir/nir_lower_idiv.c
 * (emit_idiv: the magnitudes divided, and the sign of the quotient from the
 * operands' exclusive or).
 */
int
nvrtx_sm75_lower_divide(
	struct nvrtx_sm75_lowering *lowering,
	const struct drv_gpu_shader_ir_inst *instruction,
	uint32_t dst)
{
	struct nvrtx_sm75_word word;
	struct nvrtx_sm75_src a;
	struct nvrtx_sm75_src b;
	struct nvrtx_sm75_src zero;
	uint32_t numerator;
	uint32_t denominator;
	uint32_t magnitude_n;
	uint32_t magnitude_d;
	uint32_t other;
	uint32_t sign;
	int is_signed;
	int error;

	/* The operands' registers, and whether the division is signed. */
	numerator = lowering->reg[instruction->src[0]];
	denominator = lowering->reg[instruction->src[1]];
	is_signed = 0;
	if (instruction->op == DRV_GPU_IR_IDIV || instruction->op == DRV_GPU_IR_IREM)
		is_signed = 1;

	/* The unsigned ones: the quotient or the remainder into dst, the other into a temporary. */
	if (is_signed == 0) {
		/* The temporary for the result not asked for. */
		error = nvrtx_sm75_take(lowering, &other);
		if (error != 0)
			return error;

		/* The division, the result asked for into dst. */
		if (instruction->op == DRV_GPU_IR_UDIV) {
			error = nvrtx_sm75_divide_unsigned(lowering, numerator, denominator, dst, other);
		} else {
			error = nvrtx_sm75_divide_unsigned(lowering, numerator, denominator, other, dst);
		}

		/* The temporary goes back. */
		nvrtx_sm75_give(lowering, other);

		/* Reports an emission past the code's room. */
		if (error != 0)
			return error;

		/* Succeeded: the result is in dst. */
		return 0;
	}

	/* The signed ones: the dividend's magnitude first. */
	error = nvrtx_sm75_take(lowering, &magnitude_n);
	if (error != 0)
		return error;

	/* The divisor's magnitude. */
	error = nvrtx_sm75_take(lowering, &magnitude_d);
	if (error != 0) {
		nvrtx_sm75_give(lowering, magnitude_n);
		return error;
	}

	/* The temporary for the result not asked for, and for the sign. */
	error = nvrtx_sm75_take(lowering, &other);
	if (error != 0) {
		nvrtx_sm75_give(lowering, magnitude_n);
		nvrtx_sm75_give(lowering, magnitude_d);
		return error;
	}

	/* The two magnitudes. */
	error = nvrtx_sm75_absolute(lowering, numerator, magnitude_n);
	if (error == 0)
		error = nvrtx_sm75_absolute(lowering, denominator, magnitude_d);

	/* The unsigned division of the magnitudes. */
	if (error == 0 && instruction->op == DRV_GPU_IR_IDIV)
		error = nvrtx_sm75_divide_unsigned(lowering, magnitude_n, magnitude_d, dst, other);
	if (error == 0 && instruction->op == DRV_GPU_IR_IREM)
		error = nvrtx_sm75_divide_unsigned(lowering, magnitude_n, magnitude_d, other, dst);

	/* P0: the result is negative (the operands' signs differ for the quotient, the dividend is negative for the remainder). */
	zero = nvrtx_sm75_reg(NVRTX_SM75_RZ);
	sign = numerator;
	if (error == 0 && instruction->op == DRV_GPU_IR_IDIV) {
		a = nvrtx_sm75_reg(numerator);
		b = nvrtx_sm75_reg(denominator);
		nvrtx_sm75_begin(&word);
		nvrtx_sm75_lop3(&word, other, &a, &b, &zero, NVRTX_SM75_LUT_XOR);
		error = nvrtx_sm75_emit(lowering, &word, NVRTX_SM75_FIXED);
		sign = other;
	}

	/* P0 where the sign's word is negative. */
	if (error == 0) {
		a = nvrtx_sm75_reg(sign);
		nvrtx_sm75_begin(&word);
		nvrtx_sm75_isetp(&word, NVRTX_SM75_P0, NVRTX_SM75_ICMP_LT, 1, &a, &zero);
		error = nvrtx_sm75_emit(lowering, &word, NVRTX_SM75_FIXED_PREDICATE);
	}

	/* The result negated where P0 holds. */
	if (error == 0)
		error = nvrtx_sm75_negate_if_p0(lowering, dst);

	/* The temporaries go back. */
	nvrtx_sm75_give(lowering, magnitude_n);
	nvrtx_sm75_give(lowering, magnitude_d);
	nvrtx_sm75_give(lowering, other);

	/* Reports an emission past the code's room. */
	if (error != 0)
		return error;

	/* Succeeded: the result is in dst. */
	return 0;
}

/*
 * Divides two unsigned 32-bit registers: the quotient and the remainder
 * into their registers (both written, neither a source):
 *
 *   r = f2u(rcp(u2f(d)) * (2^32 - 512))          about 2^32 / d, below it
 *   r = r + hi(r * (-d * r))                      one refinement
 *   q = hi(n * r);  m = n - q * d
 *   twice: if (m >= d) { q = q + 1; m = m - d; }
 *
 * Procedure referenced: Mesa 25.0.7 src/compiler/nir/nir_lower_idiv.c
 * (emit_udiv).
 */
static int
nvrtx_sm75_divide_unsigned(
	struct nvrtx_sm75_lowering *lowering,
	uint32_t numerator,
	uint32_t denominator,
	uint32_t quotient,
	uint32_t remainder)
{
	struct nvrtx_sm75_word word;
	struct nvrtx_sm75_src a;
	struct nvrtx_sm75_src b;
	struct nvrtx_sm75_src c;
	struct nvrtx_sm75_src zero;
	uint32_t reciprocal;
	uint32_t scratch;
	uint32_t pair;
	unsigned step;
	int error;

	/* RZ, the zero register, as an operand. */
	zero = nvrtx_sm75_reg(NVRTX_SM75_RZ);

	/* A register for the reciprocal. */
	error = nvrtx_sm75_take(lowering, &reciprocal);
	if (error != 0)
		return error;

	/* One for the products. */
	error = nvrtx_sm75_take(lowering, &scratch);
	if (error != 0) {
		nvrtx_sm75_give(lowering, reciprocal);
		return error;
	}

	/* A pair for the high halves. */
	error = nvrtx_sm75_take_pair(lowering, &pair);
	if (error != 0) {
		nvrtx_sm75_give(lowering, reciprocal);
		nvrtx_sm75_give(lowering, scratch);
		return error;
	}

	/* The float reciprocal of the divisor, scaled and made an integer: first the divisor as a float. */
	nvrtx_sm75_begin(&word);
	nvrtx_sm75_i2f(&word, scratch, denominator, 0);
	error = nvrtx_sm75_emit(lowering, &word, NVRTX_SM75_VARIABLE_WRITE);

	/* Its reciprocal. */
	if (error == 0) {
		nvrtx_sm75_begin(&word);
		nvrtx_sm75_mufu(&word, scratch, NVRTX_SM75_MUFU_RCP, scratch);
		error = nvrtx_sm75_emit(lowering, &word, NVRTX_SM75_VARIABLE_WRITE);
	}

	/* Scaled by 2^32 less 512. */
	if (error == 0) {
		a = nvrtx_sm75_reg(scratch);
		b = nvrtx_sm75_imm(NVRTX_SM75_RECIPROCAL_SCALE);
		nvrtx_sm75_begin(&word);
		nvrtx_sm75_fmul(&word, scratch, &a, &b);
		error = nvrtx_sm75_emit(lowering, &word, NVRTX_SM75_FIXED);
	}

	/* Made an unsigned integer: r. */
	if (error == 0) {
		nvrtx_sm75_begin(&word);
		nvrtx_sm75_f2i(&word, reciprocal, scratch, 0);
		error = nvrtx_sm75_emit(lowering, &word, NVRTX_SM75_VARIABLE_WRITE);
	}

	/* The refinement: -d * r, its product's high half with r, added to r.  First -d. */
	if (error == 0) {
		a = nvrtx_sm75_reg(denominator);
		a.neg = 1;
		nvrtx_sm75_begin(&word);
		nvrtx_sm75_iadd3(&word, scratch, &a, &zero, &zero);
		error = nvrtx_sm75_emit(lowering, &word, NVRTX_SM75_FIXED);
	}

	/* -d * r. */
	if (error == 0) {
		a = nvrtx_sm75_reg(scratch);
		b = nvrtx_sm75_reg(reciprocal);
		nvrtx_sm75_begin(&word);
		nvrtx_sm75_imad(&word, scratch, &a, &b, &zero, 0);
		error = nvrtx_sm75_emit(lowering, &word, NVRTX_SM75_FIXED);
	}

	/* r times it, into the pair. */
	if (error == 0) {
		a = nvrtx_sm75_reg(reciprocal);
		b = nvrtx_sm75_reg(scratch);
		nvrtx_sm75_begin(&word);
		nvrtx_sm75_imad_wide(&word, pair, &a, &b, &zero);
		error = nvrtx_sm75_emit(lowering, &word, NVRTX_SM75_FIXED);
	}

	/* The high half added to r. */
	if (error == 0) {
		a = nvrtx_sm75_reg(reciprocal);
		b = nvrtx_sm75_reg(pair + 1U);
		nvrtx_sm75_begin(&word);
		nvrtx_sm75_iadd3(&word, reciprocal, &a, &b, &zero);
		error = nvrtx_sm75_emit(lowering, &word, NVRTX_SM75_FIXED);
	}

	/* The quotient: the high half of n * r. */
	if (error == 0) {
		a = nvrtx_sm75_reg(numerator);
		b = nvrtx_sm75_reg(reciprocal);
		nvrtx_sm75_begin(&word);
		nvrtx_sm75_imad_wide(&word, pair, &a, &b, &zero);
		error = nvrtx_sm75_emit(lowering, &word, NVRTX_SM75_FIXED);
	}

	/* Its high half into the quotient's register. */
	if (error == 0) {
		b = nvrtx_sm75_reg(pair + 1U);
		nvrtx_sm75_begin(&word);
		nvrtx_sm75_mov(&word, quotient, &b);
		error = nvrtx_sm75_emit(lowering, &word, NVRTX_SM75_FIXED);
	}

	/* The remainder: n - q * d.  First q * d. */
	if (error == 0) {
		a = nvrtx_sm75_reg(quotient);
		b = nvrtx_sm75_reg(denominator);
		nvrtx_sm75_begin(&word);
		nvrtx_sm75_imad(&word, scratch, &a, &b, &zero, 0);
		error = nvrtx_sm75_emit(lowering, &word, NVRTX_SM75_FIXED);
	}

	/* n less it. */
	if (error == 0) {
		a = nvrtx_sm75_reg(numerator);
		b = nvrtx_sm75_reg(scratch);
		b.neg = 1;
		nvrtx_sm75_begin(&word);
		nvrtx_sm75_iadd3(&word, remainder, &a, &b, &zero);
		error = nvrtx_sm75_emit(lowering, &word, NVRTX_SM75_FIXED);
	}

	/* The two corrections, each where the remainder is still the divisor or more. */
	for (step = 0U; error == 0 && step < 2U; step++) {
		/* P0 where m >= d. */
		a = nvrtx_sm75_reg(remainder);
		b = nvrtx_sm75_reg(denominator);
		nvrtx_sm75_begin(&word);
		nvrtx_sm75_isetp(&word, NVRTX_SM75_P0, NVRTX_SM75_ICMP_GE, 0, &a, &b);
		error = nvrtx_sm75_emit(lowering, &word, NVRTX_SM75_FIXED_PREDICATE);
		if (error != 0)
			break;

		/* @P0 q = q + 1. */
		a = nvrtx_sm75_reg(quotient);
		c = nvrtx_sm75_imm(1U);
		nvrtx_sm75_begin(&word);
		nvrtx_sm75_iadd3(&word, quotient, &a, &c, &zero);
		nvrtx_sm75_guard(&word, NVRTX_SM75_P0, 0);
		error = nvrtx_sm75_emit(lowering, &word, NVRTX_SM75_FIXED);
		if (error != 0)
			break;

		/* @P0 m = m - d. */
		a = nvrtx_sm75_reg(remainder);
		b = nvrtx_sm75_reg(denominator);
		b.neg = 1;
		nvrtx_sm75_begin(&word);
		nvrtx_sm75_iadd3(&word, remainder, &a, &b, &zero);
		nvrtx_sm75_guard(&word, NVRTX_SM75_P0, 0);
		error = nvrtx_sm75_emit(lowering, &word, NVRTX_SM75_FIXED);
	}

	/* The temporaries go back. */
	nvrtx_sm75_give(lowering, reciprocal);
	nvrtx_sm75_give(lowering, scratch);
	nvrtx_sm75_give(lowering, pair);
	nvrtx_sm75_give(lowering, pair + 1U);

	/* Reports an emission past the code's room. */
	if (error != 0)
		return error;

	/* Succeeded: the quotient and the remainder are in their registers. */
	return 0;
}

/* Writes the magnitude of a signed register into another: -value where it is negative. */
static int
nvrtx_sm75_absolute(
	struct nvrtx_sm75_lowering *lowering,
	uint32_t value,
	uint32_t result)
{
	struct nvrtx_sm75_word word;
	struct nvrtx_sm75_src a;
	struct nvrtx_sm75_src zero;
	int error;

	/* RZ, the zero register, as an operand. */
	zero = nvrtx_sm75_reg(NVRTX_SM75_RZ);

	/* P0: the value is negative. */
	a = nvrtx_sm75_reg(value);
	nvrtx_sm75_begin(&word);
	nvrtx_sm75_isetp(&word, NVRTX_SM75_P0, NVRTX_SM75_ICMP_LT, 1, &a, &zero);
	error = nvrtx_sm75_emit(lowering, &word, NVRTX_SM75_FIXED_PREDICATE);
	if (error != 0)
		return error;

	/* The value, then its negation where P0 holds. */
	nvrtx_sm75_begin(&word);
	nvrtx_sm75_mov(&word, result, &a);
	error = nvrtx_sm75_emit(lowering, &word, NVRTX_SM75_FIXED);
	if (error != 0)
		return error;

	/* -value where P0 holds. */
	error = nvrtx_sm75_negate_if_p0(lowering, result);
	if (error != 0)
		return error;

	/* Succeeded: the magnitude is in result. */
	return 0;
}

/* Negates a register in place where P0 holds. */
static int
nvrtx_sm75_negate_if_p0(
	struct nvrtx_sm75_lowering *lowering,
	uint32_t value)
{
	struct nvrtx_sm75_word word;
	struct nvrtx_sm75_src a;
	struct nvrtx_sm75_src zero;
	int error;

	/* RZ, the zero register, as an operand. */
	zero = nvrtx_sm75_reg(NVRTX_SM75_RZ);

	/* @P0 IADD3 value, -value, RZ, RZ. */
	a = nvrtx_sm75_reg(value);
	a.neg = 1;
	nvrtx_sm75_begin(&word);
	nvrtx_sm75_iadd3(&word, value, &a, &zero, &zero);
	nvrtx_sm75_guard(&word, NVRTX_SM75_P0, 0);
	error = nvrtx_sm75_emit(lowering, &word, NVRTX_SM75_FIXED);
	if (error != 0)
		return error;

	/* Succeeded: the register is negated where P0 holds. */
	return 0;
}
