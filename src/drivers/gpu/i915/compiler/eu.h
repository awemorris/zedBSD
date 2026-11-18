/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Gen9 and Gen12 EU instruction encoder: turns one instruction into its
 * 128-bit word.
 *
 * The values (opcodes, field positions, message descriptors) are in
 * intel/eu-encoding-gen12.h and intel/eu-encoding-gen9.h; instruction
 * selection is the code generator's
 * job.  Operand types and register files are named in the Gen12 encoding
 * (EU_TYPE_*, EU_FILE_*), which the encoder translates for Gen9.  Only the
 * compiler and its host tests use this header.
 */

#ifndef DRIVERS_GPU_I915_COMPILER_EU_H
#define DRIVERS_GPU_I915_COMPILER_EU_H

#include <stddef.h>
#include <stdint.h>

/* The field layout of one graphics version's instructions (compiler/eu.c). */
struct i915_eu_format;

/*
 * A growable buffer of encoded EU instructions.
 *
 * The code generator owns one for the length of one compile.  Once `error`
 * is set, every further emitter leaves the buffer as it is, so a failure
 * anywhere in a sequence is seen once at the end.
 */
struct i915_eu_buf {
	uint32_t *words;
	size_t count;
	size_t capacity;
	int error;

	/* In-order instructions so far: what the scoreboard byte counts back over. */
	unsigned in_order;

	/*
	 * The layout the words are encoded in, chosen when the buffer is
	 * prepared and kept for its life: Gen12's unless drv_i915_eu_init_gen()
	 * named another graphics version.
	 */
	const struct i915_eu_format *format;
};

/*
 * One instruction operand: register file, number, subregister, type and
 * region, or an immediate.
 *
 * Operands are values; the helpers below build them and the emitters encode
 * them.
 */
struct i915_eu_reg {
	uint32_t file;
	uint32_t nr;
	uint32_t subnr;
	uint32_t type;
	uint32_t vstride;
	uint32_t width;
	uint32_t hstride;
	uint32_t immediate;

	/* Source modifier: the operand is read as -value. */
	uint32_t negate;

	/* Source modifier: the operand is read as |value| (before any negation). */
	uint32_t absolute;
};

/*
 * The two-source arithmetic and logic operations a caller can ask for.
 *
 * ADD and MUL (of floats or of integers, as the operand types say), AND,
 * OR, XOR, SHL, SHR and ASR are encoded; SUB is refused by poisoning the
 * buffer (a subtraction is an ADD with its second source negated).  The
 * logic and shift operations take integer operands; SHR shifts zeros in,
 * ASR copies of the sign bit.
 */
enum i915_eu_alu {
	I915_EU_ADD = 0,
	I915_EU_MUL,
	I915_EU_SUB,
	I915_EU_AND,
	I915_EU_OR,
	I915_EU_SHL,
	I915_EU_SHR,
	I915_EU_XOR,
	I915_EU_ASR,
	I915_EU_ALU_COUNT
};

/*
 * The math functions a caller can ask for.
 *
 * Each maps to one hardware math selector.  The float functions read one
 * float source; the integer division's quotient and remainder read two
 * integer sources, signed or unsigned as their type says, without source
 * modifiers.
 */
enum i915_eu_math {
	I915_EU_MATH_INV = 0,
	I915_EU_MATH_RSQ,
	I915_EU_MATH_SQRT,
	I915_EU_MATH_SIN,
	I915_EU_MATH_COS,
	I915_EU_MATH_LOG,
	I915_EU_MATH_EXP,
	I915_EU_MATH_INT_QUOTIENT,
	I915_EU_MATH_INT_REMAINDER,
	I915_EU_MATH_COUNT
};

/*
 * The one-source operations other than a move.
 *
 * NOT takes an integer operand; RNDD (round down), RNDZ (round toward
 * zero), RNDE (round to the nearest, ties to even) and FRC (fraction)
 * take a float.
 */
enum i915_eu_unary {
	I915_EU_NOT = 0,
	I915_EU_RNDD,
	I915_EU_FRC,
	I915_EU_RNDZ,
	I915_EU_RNDE,
	I915_EU_UNARY_COUNT
};

/*
 * A conditional modifier: what a comparison tests, or which source a
 * selection keeps.
 *
 * With floats every test but NE is false when either source is NaN; NE is
 * true then.
 */
enum i915_eu_cond {
	I915_EU_COND_EQ = 0,
	I915_EU_COND_NE,
	I915_EU_COND_GT,
	I915_EU_COND_GE,
	I915_EU_COND_LT,
	I915_EU_COND_LE,
	I915_EU_COND_COUNT
};

/*
 * A flag subregister: sixteen channel bits a comparison writes and a
 * predicate reads.
 */
enum i915_eu_flag {
	I915_EU_FLAG_F0_0 = 0,
	I915_EU_FLAG_F0_1,
	I915_EU_FLAG_F1_0,
	I915_EU_FLAG_F1_1,
	I915_EU_FLAG_COUNT
};

void drv_i915_eu_init(struct i915_eu_buf *buffer);
void drv_i915_eu_init_gen(struct i915_eu_buf *buffer, unsigned graphics_ver);
unsigned drv_i915_eu_graphics_ver(const struct i915_eu_buf *buffer);
void drv_i915_eu_free(struct i915_eu_buf *buffer);
const uint32_t *drv_i915_eu_data(const struct i915_eu_buf *buffer, size_t *bytes);

struct i915_eu_reg drv_i915_eu_grf(uint32_t nr);
struct i915_eu_reg drv_i915_eu_grf_ud(uint32_t nr);
struct i915_eu_reg drv_i915_eu_grf_scalar(uint32_t nr, uint32_t subnr);
struct i915_eu_reg drv_i915_eu_grf_d(uint32_t nr);
struct i915_eu_reg drv_i915_eu_grf_uw_half(uint32_t nr, int high);
struct i915_eu_reg drv_i915_eu_grf_region(uint32_t nr, uint32_t subnr, uint32_t type, uint32_t vstride, uint32_t width, uint32_t hstride);
struct i915_eu_reg drv_i915_eu_negate(struct i915_eu_reg reg);
struct i915_eu_reg drv_i915_eu_abs(struct i915_eu_reg reg);
struct i915_eu_reg drv_i915_eu_imm_f(uint32_t bits);
struct i915_eu_reg drv_i915_eu_imm_d(uint32_t value);
struct i915_eu_reg drv_i915_eu_imm_ud(uint32_t value);
struct i915_eu_reg drv_i915_eu_imm_v(uint32_t value);
struct i915_eu_reg drv_i915_eu_null(void);

void drv_i915_eu_mov(struct i915_eu_buf *buffer, struct i915_eu_reg dst, struct i915_eu_reg src);
void drv_i915_eu_alu2(struct i915_eu_buf *buffer, enum i915_eu_alu op, struct i915_eu_reg dst, struct i915_eu_reg src0, struct i915_eu_reg src1);
void drv_i915_eu_alu2_masked(struct i915_eu_buf *buffer, enum i915_eu_flag flag, enum i915_eu_alu op, struct i915_eu_reg dst, struct i915_eu_reg src0, struct i915_eu_reg src1);
void drv_i915_eu_alu2_scalar(struct i915_eu_buf *buffer, enum i915_eu_alu op, struct i915_eu_reg dst, struct i915_eu_reg src0, struct i915_eu_reg src1);
void drv_i915_eu_alu2_sixteen(struct i915_eu_buf *buffer, enum i915_eu_alu op, struct i915_eu_reg dst, struct i915_eu_reg src0, struct i915_eu_reg src1);
void drv_i915_eu_alu2_four(struct i915_eu_buf *buffer, enum i915_eu_alu op, struct i915_eu_reg dst, struct i915_eu_reg src0, struct i915_eu_reg src1);
void drv_i915_eu_mov_all(struct i915_eu_buf *buffer, struct i915_eu_reg dst, struct i915_eu_reg src);
void drv_i915_eu_mov_scalar(struct i915_eu_buf *buffer, struct i915_eu_reg dst, struct i915_eu_reg src);
void drv_i915_eu_alu1(struct i915_eu_buf *buffer, enum i915_eu_unary op, struct i915_eu_reg dst, struct i915_eu_reg src);
void drv_i915_eu_cmp(struct i915_eu_buf *buffer, enum i915_eu_cond cond, enum i915_eu_flag flag, int predicated, struct i915_eu_reg dst, struct i915_eu_reg src0, struct i915_eu_reg src1);
void drv_i915_eu_minmax(struct i915_eu_buf *buffer, enum i915_eu_cond cond, struct i915_eu_reg dst, struct i915_eu_reg src0, struct i915_eu_reg src1);
void drv_i915_eu_select(struct i915_eu_buf *buffer, enum i915_eu_flag flag, struct i915_eu_reg dst, struct i915_eu_reg src0, struct i915_eu_reg src1);
void drv_i915_eu_flag_load(struct i915_eu_buf *buffer, enum i915_eu_flag flag, uint32_t nr, uint32_t subnr);
void drv_i915_eu_flag_clear(struct i915_eu_buf *buffer, enum i915_eu_flag flag);
void drv_i915_eu_flag_store(struct i915_eu_buf *buffer, enum i915_eu_flag flag, uint32_t nr, uint32_t subnr);
void drv_i915_eu_flag_store_word(struct i915_eu_buf *buffer, enum i915_eu_flag flag, uint32_t nr, uint32_t subnr);
void drv_i915_eu_mad(struct i915_eu_buf *buffer, struct i915_eu_reg dst, struct i915_eu_reg src0, struct i915_eu_reg src1, struct i915_eu_reg src2);
void drv_i915_eu_math(struct i915_eu_buf *buffer, enum i915_eu_math func, struct i915_eu_reg dst, struct i915_eu_reg src0, struct i915_eu_reg src1);
void drv_i915_eu_send(struct i915_eu_buf *buffer, struct i915_eu_reg dst, struct i915_eu_reg src0, struct i915_eu_reg src1, uint32_t sfid, uint32_t descriptor, uint32_t ex_descriptor, int conditional, int end_of_thread);
void drv_i915_eu_send_masked(struct i915_eu_buf *buffer, enum i915_eu_flag flag, struct i915_eu_reg dst, struct i915_eu_reg src0, struct i915_eu_reg src1, uint32_t sfid, uint32_t descriptor, uint32_t ex_descriptor, int conditional, int end_of_thread);
void drv_i915_eu_send_all(struct i915_eu_buf *buffer, struct i915_eu_reg dst, struct i915_eu_reg src0, struct i915_eu_reg src1, uint32_t sfid, uint32_t descriptor, uint32_t ex_descriptor);
void drv_i915_eu_send_all_end(struct i915_eu_buf *buffer, struct i915_eu_reg src0, uint32_t sfid, uint32_t descriptor);
void drv_i915_eu_send_scalar(struct i915_eu_buf *buffer, struct i915_eu_reg dst, struct i915_eu_reg src0, uint32_t sfid, uint32_t descriptor);
void drv_i915_eu_sync_function(struct i915_eu_buf *buffer, uint32_t function);
void drv_i915_eu_nop(struct i915_eu_buf *buffer);
uint32_t drv_i915_eu_position(const struct i915_eu_buf *buffer);
void drv_i915_eu_while(struct i915_eu_buf *buffer, enum i915_eu_flag flag, uint32_t target);
uint32_t drv_i915_eu_if(struct i915_eu_buf *buffer, enum i915_eu_flag flag);
uint32_t drv_i915_eu_endif(struct i915_eu_buf *buffer);
void drv_i915_eu_patch_if(struct i915_eu_buf *buffer, uint32_t if_position, uint32_t endif_position);
void drv_i915_eu_patch_endif(struct i915_eu_buf *buffer, uint32_t endif_position, uint32_t target);

#endif /* DRIVERS_GPU_I915_COMPILER_EU_H */
