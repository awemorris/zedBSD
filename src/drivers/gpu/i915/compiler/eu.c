/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Gen9, Gen11 and Gen12 EU instruction encoder (see eu.h).
 *
 * Each instruction is four little-endian 32-bit words on every version.
 * Field bit positions, hardware opcode values, register types and the
 * descriptor layout of SEND are in intel/eu-encoding-gen12.h,
 * intel/eu-encoding-gen9.h and, for the types Ice Lake numbers anew,
 * intel/eu-encoding-gen11.h.  A buffer is encoded in one layout (struct
 * i915_eu_format) chosen when it is prepared; the emitters ask the layout
 * where each field goes.
 *
 * SOFTWARE SCOREBOARD.  Gen12 hardware does not track register dependencies
 * between instructions; the program says them in the SWSB byte.  This
 * encoder says the one thing that is always
 * true of its output: every instruction depends on the one before it.
 *
 *   - an in-order instruction (MOV, ADD, MUL, SEL, CMP, AND, OR, XOR, NOT,
 *     SHL, SHR, ASR, RNDD, RNDZ, RNDE, FRC, WHILE, IF, ENDIF) waits for the previous in-order
 *     instruction (@1); a flag a CMP writes is read only by a later in-order
 *     instruction, so the same wait covers it.  The wait counts back over
 *     the instructions as they ran, so the first instruction of a loop,
 *     reached again from the WHILE, still waits for the one before it;
 *   - an out-of-order instruction (MATH, SEND) waits the same way, names
 *     itself with token 0 and is followed by a sync.nop that waits until it
 *     has written its destination (or, having none, has read its sources), so
 *     nothing overlaps it and token 0 is free again;
 *   - the SEND that ends the thread waits for the previous instruction and
 *     needs nothing after it.
 *
 * XXX: this serialises the kernel.  It is correct for any program this
 * compiler emits and leaves on the table the pipelining a dataflow
 * analysis would allow.
 *
 * Gen9 and Gen11 have no SWSB byte: their hardware scoreboard holds an
 * instruction that reads a register until the instruction writing it is
 * done, so their layouts encode neither the byte nor the sync.nop.  What the hardware does
 * not see -- a message whose reply nobody reads, such as a memory fence --
 * the code generator waits for itself (compile-compute.inc).
 */

#include "eu.h"
#include <kern/kcrt.h>

#include <kern/kmem.h>

#include <uapi/errno.h>

#include "../intel/eu-encoding-gen12.h"
#include "../intel/eu-encoding-gen9.h"
#include "../intel/eu-encoding-gen11.h"

/*
 * Marks a parameter a function deliberately leaves unread.
 *
 * The compiler includes no driver header, so it spells the marker itself
 * unless the including translation unit already has it.
 */
#ifndef UNUSED_PARAMETER
#define UNUSED_PARAMETER(parameter) ((void)(parameter))
#endif

/*
 * The one scoreboard token this encoder uses.
 *
 * Nothing else is in flight when it is set (see the scoreboard note above).
 */
#define I915_EU_TOKEN			0U

/* The scoreboard class of an instruction, as i915_eu_common() takes it. */
#define I915_EU_OUT_OF_ORDER		0
#define I915_EU_IN_ORDER		1
#define I915_EU_END_OF_THREAD		2

/*
 * The token-set SWSB byte of an out-of-order instruction with nothing to
 * wait for.
 *
 * Procedure referenced: Mesa 25.0.7
 * src/intel/compiler/brw_lower_scoreboard.cpp (the software scoreboard's
 * dependency byte).
 */
#define I915_EU_SWSB_TOKEN_SET		0x40U

/* The first buffer capacity, in words; the buffer doubles from there. */
#define I915_EU_FIRST_CAPACITY		64U

/* The largest shared-function identifier the SEND encoding has room for. */
#define I915_EU_SFID_MAX		15U

/* The extended-descriptor bits that are not encodable (they held the SFID once). */
#define I915_EU_EX_DESC_LOW_MASK	0x3fU

/*
 * Which channels an instruction runs on, as i915_eu_scope() takes it: the
 * eight channels of the dispatch that the execution mask leaves enabled,
 * all eight regardless of the mask (NoMask), or the first one regardless of
 * the mask (a SIMD1 NoMask instruction, which writes one dword).
 */
#define I915_EU_SCOPE_MASKED		0
#define I915_EU_SCOPE_ALL		1
#define I915_EU_SCOPE_SCALAR		2

/*
 * Sixteen channels regardless of the mask (a SIMD16 NoMask instruction in a
 * SIMD8 kernel), for an operation on sixteen 16-bit words.
 */
#define I915_EU_SCOPE_SIXTEEN		3

/*
 * Four channels regardless of the mask (a SIMD4 NoMask instruction), for
 * an operation on one quad of pixels.
 */
#define I915_EU_SCOPE_FOUR		4

/* A one-bit field a layout does not have. */
#define I915_EU_NO_BIT			0xffffU

/* An opcode a layout does not have: zero is the illegal instruction on every version. */
#define I915_EU_NO_OPCODE		0U

/* A type a layout cannot encode. */
#define I915_EU_NO_TYPE			0xffU

/* How many operand types the Gen12 encoding (the names eu.h uses) can say: four bits. */
#define I915_EU_TYPE_COUNT		16U

/*
 * The instructions the emitters ask a layout for.
 *
 * Each names one hardware instruction whose opcode value differs between
 * the versions; a layout without the instruction says I915_EU_NO_OPCODE.
 */
enum i915_eu_opcode {
	I915_EU_OPCODE_MOV = 0,
	I915_EU_OPCODE_SEL,
	I915_EU_OPCODE_NOT,
	I915_EU_OPCODE_AND,
	I915_EU_OPCODE_OR,
	I915_EU_OPCODE_XOR,
	I915_EU_OPCODE_SHR,
	I915_EU_OPCODE_SHL,
	I915_EU_OPCODE_ASR,
	I915_EU_OPCODE_CMP,
	I915_EU_OPCODE_IF,
	I915_EU_OPCODE_ENDIF,
	I915_EU_OPCODE_WHILE,
	I915_EU_OPCODE_WAIT,
	I915_EU_OPCODE_SEND,
	I915_EU_OPCODE_SENDC,
	I915_EU_OPCODE_SENDS,
	I915_EU_OPCODE_SENDSC,
	I915_EU_OPCODE_SYNC,
	I915_EU_OPCODE_MATH,
	I915_EU_OPCODE_ADD,
	I915_EU_OPCODE_MUL,
	I915_EU_OPCODE_FRC,
	I915_EU_OPCODE_RNDD,
	I915_EU_OPCODE_RNDE,
	I915_EU_OPCODE_RNDZ,
	I915_EU_OPCODE_NOP,
	I915_EU_OPCODE_MAD,
	I915_EU_OPCODE_COUNT
};

/*
 * One multi-bit field of the 128-bit instruction: its highest and lowest
 * bit, inclusive.
 */
struct i915_eu_field {
	unsigned high;
	unsigned low;
};

/*
 * Where one operand's parts sit in a layout.
 *
 * A destination has no width, vertical stride or source modifier; Gen12
 * spells its register file in one bit and says an immediate with a bit of
 * its own, Gen9 spells all three files in two bits.
 */
struct i915_eu_operand_layout {
	struct i915_eu_field file;
	struct i915_eu_field type;
	struct i915_eu_field subreg;
	struct i915_eu_field reg_nr;
	struct i915_eu_field hstride;
	struct i915_eu_field width;
	struct i915_eu_field vstride;
	unsigned abs_bit;
	unsigned negate_bit;

	/* Gen12: the bit that says the operand is an immediate; I915_EU_NO_BIT on Gen9. */
	unsigned is_imm_bit;
};

/*
 * The instruction layout of one graphics version.
 *
 * One constant instance exists per supported version; a buffer points at
 * the one it encodes for from drv_i915_eu_init_gen() on.  The types the
 * code generator names (the Gen12 encoding) are looked up in the two type
 * tables, which on Gen12 hand every type back unchanged.
 */
struct i915_eu_format {
	unsigned graphics_ver;

	/* Control fields. */
	struct i915_eu_field opcode;
	struct i915_eu_field exec_size;
	struct i915_eu_field pred_control;
	struct i915_eu_field cond_modifier;
	struct i915_eu_field math_function;
	unsigned no_mask_bit;
	unsigned flag_reg_bit;
	unsigned flag_subreg_bit;

	/* Nonzero when the layout carries Gen12's software scoreboard byte. */
	int has_swsb;
	struct i915_eu_field swsb;

	/* The operands, the 32-bit immediate and a branch's two targets. */
	struct i915_eu_operand_layout dst;
	struct i915_eu_operand_layout src0;
	struct i915_eu_operand_layout src1;
	struct i915_eu_field imm32;
	struct i915_eu_field jip;
	struct i915_eu_field uip;

	/* The shared function a SEND addresses and its end-of-thread bit. */
	struct i915_eu_field sfid;
	unsigned eot_bit;

	/* The hardware opcode of each enum i915_eu_opcode. */
	uint32_t opcodes[I915_EU_OPCODE_COUNT];

	/* The hardware type of a register and of an immediate operand, by its Gen12 name. */
	uint8_t register_types[I915_EU_TYPE_COUNT];
	uint8_t immediate_types[I915_EU_TYPE_COUNT];

	/* The value of the file field for an immediate; unused where an is_imm bit says it. */
	uint32_t file_imm;
};

/* The hardware conditional modifier of each enum i915_eu_cond, in its order. */
static const uint32_t i915_eu_cond_bits[I915_EU_COND_COUNT] = {
	EU_COND_Z,
	EU_COND_NZ,
	EU_COND_G,
	EU_COND_GE,
	EU_COND_L,
	EU_COND_LE,
};

/*
 * The Gen12 layout (intel/eu-encoding-gen12.h).
 *
 * It is what every buffer used before Gen9 support, and what
 * drv_i915_eu_init() still chooses: the bytes it produces are those the
 * Tiger Lake and Alder Lake tests were run with.  A type keeps its value.
 */
static const struct i915_eu_format i915_eu_format_gen12 = {
	12U,
	{ EU_OPCODE_HI, EU_OPCODE_LO },
	{ EU_EXEC_SIZE_HI, EU_EXEC_SIZE_LO },
	{ EU_PRED_CONTROL_HI, EU_PRED_CONTROL_LO },
	{ EU_COND_MODIFIER_HI, EU_COND_MODIFIER_LO },
	{ EU_MATH_FUNCTION_HI, EU_MATH_FUNCTION_LO },
	EU_NO_MASK_BIT,
	EU_FLAG_REG_NR_BIT,
	EU_FLAG_SUBREG_NR_BIT,
	1,
	{ EU_SWSB_HI, EU_SWSB_LO },
	{
		{ EU_DST_REG_FILE_BIT, EU_DST_REG_FILE_BIT },
		{ EU_DST_REG_TYPE_HI, EU_DST_REG_TYPE_LO },
		{ EU_DST_SUBREG_HI, EU_DST_SUBREG_LO },
		{ EU_DST_REG_NR_HI, EU_DST_REG_NR_LO },
		{ EU_DST_HSTRIDE_HI, EU_DST_HSTRIDE_LO },
		{ 0U, 0U },
		{ 0U, 0U },
		I915_EU_NO_BIT,
		I915_EU_NO_BIT,
		I915_EU_NO_BIT,
	},
	{
		{ EU_SRC0_REG_FILE_BIT, EU_SRC0_REG_FILE_BIT },
		{ EU_SRC0_REG_TYPE_HI, EU_SRC0_REG_TYPE_LO },
		{ EU_SRC0_SUBREG_HI, EU_SRC0_SUBREG_LO },
		{ EU_SRC0_REG_NR_HI, EU_SRC0_REG_NR_LO },
		{ EU_SRC0_HSTRIDE_HI, EU_SRC0_HSTRIDE_LO },
		{ EU_SRC0_WIDTH_HI, EU_SRC0_WIDTH_LO },
		{ EU_SRC0_VSTRIDE_HI, EU_SRC0_VSTRIDE_LO },
		EU_SRC0_ABS_BIT,
		EU_SRC0_NEGATE_BIT,
		EU_SRC0_IS_IMM_BIT,
	},
	{
		{ EU_SRC1_REG_FILE_BIT, EU_SRC1_REG_FILE_BIT },
		{ EU_SRC1_REG_TYPE_HI, EU_SRC1_REG_TYPE_LO },
		{ EU_SRC1_SUBREG_HI, EU_SRC1_SUBREG_LO },
		{ EU_SRC1_REG_NR_HI, EU_SRC1_REG_NR_LO },
		{ EU_SRC1_HSTRIDE_HI, EU_SRC1_HSTRIDE_LO },
		{ EU_SRC1_WIDTH_HI, EU_SRC1_WIDTH_LO },
		{ EU_SRC1_VSTRIDE_HI, EU_SRC1_VSTRIDE_LO },
		EU_SRC1_ABS_BIT,
		EU_SRC1_NEGATE_BIT,
		EU_SRC1_IS_IMM_BIT,
	},
	{ EU_IMM32_HI, EU_IMM32_LO },
	{ EU_JIP_HI, EU_JIP_LO },
	{ EU_UIP_HI, EU_UIP_LO },
	{ EU_SEND_SFID_HI, EU_SEND_SFID_LO },
	EU_SEND_EOT_BIT,
	{
		EU_OP_MOV,
		EU_OP_SEL,
		EU_OP_NOT,
		EU_OP_AND,
		EU_OP_OR,
		EU_OP_XOR,
		EU_OP_SHR,
		EU_OP_SHL,
		EU_OP_ASR,
		EU_OP_CMP,
		EU_OP_IF,
		EU_OP_ENDIF,
		EU_OP_WHILE,
		I915_EU_NO_OPCODE,
		EU_OP_SEND,
		EU_OP_SENDC,
		I915_EU_NO_OPCODE,
		I915_EU_NO_OPCODE,
		EU_OP_SYNC,
		EU_OP_MATH,
		EU_OP_ADD,
		EU_OP_MUL,
		EU_OP_FRC,
		EU_OP_RNDD,
		EU_OP_RNDE,
		EU_OP_RNDZ,
		EU_OP_NOP,
		EU_OP_MAD,
	},
	{ 0U, 1U, 2U, 3U, 4U, 5U, 6U, 7U, 8U, 9U, 10U, 11U, 12U, 13U, 14U, 15U },
	{ 0U, 1U, 2U, 3U, 4U, 5U, 6U, 7U, 8U, 9U, 10U, 11U, 12U, 13U, 14U, 15U },
	0U,
};

/*
 * The Gen9 layout (intel/eu-encoding-gen9.h): Skylake, Kaby Lake and the
 * Coffee Lake family.
 *
 * Every field sits elsewhere than on Gen12, there is no scoreboard byte,
 * and the types the code generator names are translated: UB 0 -> 4,
 * UW 1 -> 2, UD 2 -> 0, B 4 -> 5 (the immediate V -> 6), W 5 -> 3,
 * D 6 -> 1, HF 9 -> 10 (immediate 11), F 10 -> 7.
 */
static const struct i915_eu_format i915_eu_format_gen9 = {
	9U,
	{ GEN9_EU_OPCODE_HI, GEN9_EU_OPCODE_LO },
	{ GEN9_EU_EXEC_SIZE_HI, GEN9_EU_EXEC_SIZE_LO },
	{ GEN9_EU_PRED_CONTROL_HI, GEN9_EU_PRED_CONTROL_LO },
	{ GEN9_EU_COND_MODIFIER_HI, GEN9_EU_COND_MODIFIER_LO },
	{ GEN9_EU_MATH_FUNCTION_HI, GEN9_EU_MATH_FUNCTION_LO },
	GEN9_EU_NO_MASK_BIT,
	GEN9_EU_FLAG_REG_NR_BIT,
	GEN9_EU_FLAG_SUBREG_NR_BIT,
	0,
	{ 0U, 0U },
	{
		{ GEN9_EU_DST_REG_FILE_HI, GEN9_EU_DST_REG_FILE_LO },
		{ GEN9_EU_DST_REG_TYPE_HI, GEN9_EU_DST_REG_TYPE_LO },
		{ GEN9_EU_DST_SUBREG_HI, GEN9_EU_DST_SUBREG_LO },
		{ GEN9_EU_DST_REG_NR_HI, GEN9_EU_DST_REG_NR_LO },
		{ GEN9_EU_DST_HSTRIDE_HI, GEN9_EU_DST_HSTRIDE_LO },
		{ 0U, 0U },
		{ 0U, 0U },
		I915_EU_NO_BIT,
		I915_EU_NO_BIT,
		I915_EU_NO_BIT,
	},
	{
		{ GEN9_EU_SRC0_REG_FILE_HI, GEN9_EU_SRC0_REG_FILE_LO },
		{ GEN9_EU_SRC0_REG_TYPE_HI, GEN9_EU_SRC0_REG_TYPE_LO },
		{ GEN9_EU_SRC0_SUBREG_HI, GEN9_EU_SRC0_SUBREG_LO },
		{ GEN9_EU_SRC0_REG_NR_HI, GEN9_EU_SRC0_REG_NR_LO },
		{ GEN9_EU_SRC0_HSTRIDE_HI, GEN9_EU_SRC0_HSTRIDE_LO },
		{ GEN9_EU_SRC0_WIDTH_HI, GEN9_EU_SRC0_WIDTH_LO },
		{ GEN9_EU_SRC0_VSTRIDE_HI, GEN9_EU_SRC0_VSTRIDE_LO },
		GEN9_EU_SRC0_ABS_BIT,
		GEN9_EU_SRC0_NEGATE_BIT,
		I915_EU_NO_BIT,
	},
	{
		{ GEN9_EU_SRC1_REG_FILE_HI, GEN9_EU_SRC1_REG_FILE_LO },
		{ GEN9_EU_SRC1_REG_TYPE_HI, GEN9_EU_SRC1_REG_TYPE_LO },
		{ GEN9_EU_SRC1_SUBREG_HI, GEN9_EU_SRC1_SUBREG_LO },
		{ GEN9_EU_SRC1_REG_NR_HI, GEN9_EU_SRC1_REG_NR_LO },
		{ GEN9_EU_SRC1_HSTRIDE_HI, GEN9_EU_SRC1_HSTRIDE_LO },
		{ GEN9_EU_SRC1_WIDTH_HI, GEN9_EU_SRC1_WIDTH_LO },
		{ GEN9_EU_SRC1_VSTRIDE_HI, GEN9_EU_SRC1_VSTRIDE_LO },
		GEN9_EU_SRC1_ABS_BIT,
		GEN9_EU_SRC1_NEGATE_BIT,
		I915_EU_NO_BIT,
	},
	{ GEN9_EU_IMM32_HI, GEN9_EU_IMM32_LO },
	{ GEN9_EU_JIP_HI, GEN9_EU_JIP_LO },
	{ GEN9_EU_UIP_HI, GEN9_EU_UIP_LO },
	{ GEN9_EU_SEND_SFID_HI, GEN9_EU_SEND_SFID_LO },
	GEN9_EU_SEND_EOT_BIT,
	{
		GEN9_EU_OP_MOV,
		GEN9_EU_OP_SEL,
		GEN9_EU_OP_NOT,
		GEN9_EU_OP_AND,
		GEN9_EU_OP_OR,
		GEN9_EU_OP_XOR,
		GEN9_EU_OP_SHR,
		GEN9_EU_OP_SHL,
		GEN9_EU_OP_ASR,
		GEN9_EU_OP_CMP,
		GEN9_EU_OP_IF,
		GEN9_EU_OP_ENDIF,
		GEN9_EU_OP_WHILE,
		GEN9_EU_OP_WAIT,
		GEN9_EU_OP_SEND,
		GEN9_EU_OP_SENDC,
		GEN9_EU_OP_SENDS,
		GEN9_EU_OP_SENDSC,
		I915_EU_NO_OPCODE,
		GEN9_EU_OP_MATH,
		GEN9_EU_OP_ADD,
		GEN9_EU_OP_MUL,
		GEN9_EU_OP_FRC,
		GEN9_EU_OP_RNDD,
		GEN9_EU_OP_RNDE,
		GEN9_EU_OP_RNDZ,
		GEN9_EU_OP_NOP,
		I915_EU_NO_OPCODE,
	},
	{
		4U,
		GEN9_EU_TYPE_UW,
		GEN9_EU_TYPE_UD,
		I915_EU_NO_TYPE,
		5U,
		GEN9_EU_TYPE_W,
		GEN9_EU_TYPE_D,
		I915_EU_NO_TYPE,
		I915_EU_NO_TYPE,
		GEN9_EU_TYPE_HF,
		GEN9_EU_TYPE_F,
		I915_EU_NO_TYPE,
		I915_EU_NO_TYPE,
		I915_EU_NO_TYPE,
		I915_EU_NO_TYPE,
		I915_EU_NO_TYPE,
	},
	{
		I915_EU_NO_TYPE,
		GEN9_EU_TYPE_UW,
		GEN9_EU_TYPE_UD,
		I915_EU_NO_TYPE,
		GEN9_EU_IMM_TYPE_V,
		GEN9_EU_TYPE_W,
		GEN9_EU_TYPE_D,
		I915_EU_NO_TYPE,
		I915_EU_NO_TYPE,
		GEN9_EU_IMM_TYPE_HF,
		GEN9_EU_TYPE_F,
		I915_EU_NO_TYPE,
		I915_EU_NO_TYPE,
		I915_EU_NO_TYPE,
		I915_EU_NO_TYPE,
		I915_EU_NO_TYPE,
	},
	GEN9_EU_FILE_IMM,
};

/*
 * The Ice Lake layout: the Gen9 fields, opcodes and SEND forms with the
 * Gen11 type numbers (intel/eu-encoding-gen11.h), translated from the
 * names the code generator uses: UB 0 -> 4, UW 1 -> 2, UD 2 -> 0, B 4 -> 5
 * (the immediate V -> 5), W 5 -> 3, D 6 -> 1, HF 9 -> 8, F 10 -> 9.
 */
static const struct i915_eu_format i915_eu_format_gen11 = {
	11U,
	{ GEN9_EU_OPCODE_HI, GEN9_EU_OPCODE_LO },
	{ GEN9_EU_EXEC_SIZE_HI, GEN9_EU_EXEC_SIZE_LO },
	{ GEN9_EU_PRED_CONTROL_HI, GEN9_EU_PRED_CONTROL_LO },
	{ GEN9_EU_COND_MODIFIER_HI, GEN9_EU_COND_MODIFIER_LO },
	{ GEN9_EU_MATH_FUNCTION_HI, GEN9_EU_MATH_FUNCTION_LO },
	GEN9_EU_NO_MASK_BIT,
	GEN9_EU_FLAG_REG_NR_BIT,
	GEN9_EU_FLAG_SUBREG_NR_BIT,
	0,
	{ 0U, 0U },
	{
		{ GEN9_EU_DST_REG_FILE_HI, GEN9_EU_DST_REG_FILE_LO },
		{ GEN9_EU_DST_REG_TYPE_HI, GEN9_EU_DST_REG_TYPE_LO },
		{ GEN9_EU_DST_SUBREG_HI, GEN9_EU_DST_SUBREG_LO },
		{ GEN9_EU_DST_REG_NR_HI, GEN9_EU_DST_REG_NR_LO },
		{ GEN9_EU_DST_HSTRIDE_HI, GEN9_EU_DST_HSTRIDE_LO },
		{ 0U, 0U },
		{ 0U, 0U },
		I915_EU_NO_BIT,
		I915_EU_NO_BIT,
		I915_EU_NO_BIT,
	},
	{
		{ GEN9_EU_SRC0_REG_FILE_HI, GEN9_EU_SRC0_REG_FILE_LO },
		{ GEN9_EU_SRC0_REG_TYPE_HI, GEN9_EU_SRC0_REG_TYPE_LO },
		{ GEN9_EU_SRC0_SUBREG_HI, GEN9_EU_SRC0_SUBREG_LO },
		{ GEN9_EU_SRC0_REG_NR_HI, GEN9_EU_SRC0_REG_NR_LO },
		{ GEN9_EU_SRC0_HSTRIDE_HI, GEN9_EU_SRC0_HSTRIDE_LO },
		{ GEN9_EU_SRC0_WIDTH_HI, GEN9_EU_SRC0_WIDTH_LO },
		{ GEN9_EU_SRC0_VSTRIDE_HI, GEN9_EU_SRC0_VSTRIDE_LO },
		GEN9_EU_SRC0_ABS_BIT,
		GEN9_EU_SRC0_NEGATE_BIT,
		I915_EU_NO_BIT,
	},
	{
		{ GEN9_EU_SRC1_REG_FILE_HI, GEN9_EU_SRC1_REG_FILE_LO },
		{ GEN9_EU_SRC1_REG_TYPE_HI, GEN9_EU_SRC1_REG_TYPE_LO },
		{ GEN9_EU_SRC1_SUBREG_HI, GEN9_EU_SRC1_SUBREG_LO },
		{ GEN9_EU_SRC1_REG_NR_HI, GEN9_EU_SRC1_REG_NR_LO },
		{ GEN9_EU_SRC1_HSTRIDE_HI, GEN9_EU_SRC1_HSTRIDE_LO },
		{ GEN9_EU_SRC1_WIDTH_HI, GEN9_EU_SRC1_WIDTH_LO },
		{ GEN9_EU_SRC1_VSTRIDE_HI, GEN9_EU_SRC1_VSTRIDE_LO },
		GEN9_EU_SRC1_ABS_BIT,
		GEN9_EU_SRC1_NEGATE_BIT,
		I915_EU_NO_BIT,
	},
	{ GEN9_EU_IMM32_HI, GEN9_EU_IMM32_LO },
	{ GEN9_EU_JIP_HI, GEN9_EU_JIP_LO },
	{ GEN9_EU_UIP_HI, GEN9_EU_UIP_LO },
	{ GEN9_EU_SEND_SFID_HI, GEN9_EU_SEND_SFID_LO },
	GEN9_EU_SEND_EOT_BIT,
	{
		GEN9_EU_OP_MOV,
		GEN9_EU_OP_SEL,
		GEN9_EU_OP_NOT,
		GEN9_EU_OP_AND,
		GEN9_EU_OP_OR,
		GEN9_EU_OP_XOR,
		GEN9_EU_OP_SHR,
		GEN9_EU_OP_SHL,
		GEN9_EU_OP_ASR,
		GEN9_EU_OP_CMP,
		GEN9_EU_OP_IF,
		GEN9_EU_OP_ENDIF,
		GEN9_EU_OP_WHILE,
		GEN9_EU_OP_WAIT,
		GEN9_EU_OP_SEND,
		GEN9_EU_OP_SENDC,
		GEN9_EU_OP_SENDS,
		GEN9_EU_OP_SENDSC,
		I915_EU_NO_OPCODE,
		GEN9_EU_OP_MATH,
		GEN9_EU_OP_ADD,
		GEN9_EU_OP_MUL,
		GEN9_EU_OP_FRC,
		GEN9_EU_OP_RNDD,
		GEN9_EU_OP_RNDE,
		GEN9_EU_OP_RNDZ,
		GEN9_EU_OP_NOP,
		I915_EU_NO_OPCODE,
	},
	{
		GEN11_EU_TYPE_UB,
		GEN11_EU_TYPE_UW,
		GEN11_EU_TYPE_UD,
		I915_EU_NO_TYPE,
		GEN11_EU_TYPE_B,
		GEN11_EU_TYPE_W,
		GEN11_EU_TYPE_D,
		I915_EU_NO_TYPE,
		I915_EU_NO_TYPE,
		GEN11_EU_TYPE_HF,
		GEN11_EU_TYPE_F,
		I915_EU_NO_TYPE,
		I915_EU_NO_TYPE,
		I915_EU_NO_TYPE,
		I915_EU_NO_TYPE,
		I915_EU_NO_TYPE,
	},
	{
		I915_EU_NO_TYPE,
		GEN11_EU_TYPE_UW,
		GEN11_EU_TYPE_UD,
		I915_EU_NO_TYPE,
		GEN11_EU_IMM_TYPE_V,
		GEN11_EU_TYPE_W,
		GEN11_EU_TYPE_D,
		I915_EU_NO_TYPE,
		I915_EU_NO_TYPE,
		GEN11_EU_TYPE_HF,
		GEN11_EU_TYPE_F,
		I915_EU_NO_TYPE,
		I915_EU_NO_TYPE,
		I915_EU_NO_TYPE,
		I915_EU_NO_TYPE,
		I915_EU_NO_TYPE,
	},
	GEN9_EU_FILE_IMM,
};

static struct i915_eu_reg i915_eu_grf_typed(uint32_t nr, uint32_t type);
static uint32_t *i915_eu_reserve(struct i915_eu_buf *buffer);
static void i915_eu_common(struct i915_eu_buf *buffer, uint32_t *inst, enum i915_eu_opcode opcode, int order);
static void i915_eu_flag(const struct i915_eu_buf *buffer, uint32_t *inst, enum i915_eu_flag flag);
static void i915_eu_alu2_common(struct i915_eu_buf *buffer, int predicated, enum i915_eu_flag flag, int scope, enum i915_eu_alu op, struct i915_eu_reg dst, struct i915_eu_reg src0, struct i915_eu_reg src1);
static void i915_eu_send_common(struct i915_eu_buf *buffer, int predicated, enum i915_eu_flag flag, int scope, struct i915_eu_reg dst, struct i915_eu_reg src0, struct i915_eu_reg src1, uint32_t sfid, uint32_t descriptor, uint32_t ex_descriptor, int conditional, int end_of_thread);
static void i915_eu_send_gen12(uint32_t *inst, struct i915_eu_reg dst, struct i915_eu_reg src0, struct i915_eu_reg src1, uint32_t descriptor, uint32_t ex_descriptor);
static void i915_eu_send_gen9(struct i915_eu_buf *buffer, uint32_t *inst, int scope, struct i915_eu_reg dst, struct i915_eu_reg src0, struct i915_eu_reg src1, uint32_t descriptor, uint32_t ex_descriptor);
static void i915_eu_scope(const struct i915_eu_buf *buffer, uint32_t *inst, int scope);
static void i915_eu_sync(struct i915_eu_buf *buffer, uint32_t swsb);
static int i915_eu_mad_source(struct i915_eu_reg reg, uint32_t *vstride);
static void i915_eu_bits(uint32_t *inst, uint32_t high, uint32_t low, uint32_t value);
static void i915_eu_wait_notification(struct i915_eu_buf *buffer);
static void i915_eu_branch_target(struct i915_eu_buf *buffer, uint32_t *inst, int both);
static void i915_eu_dst(struct i915_eu_buf *buffer, uint32_t *inst, struct i915_eu_reg reg);
static void i915_eu_src0(struct i915_eu_buf *buffer, uint32_t *inst, struct i915_eu_reg reg);
static void i915_eu_src1(struct i915_eu_buf *buffer, uint32_t *inst, struct i915_eu_reg reg);
static void i915_eu_source(struct i915_eu_buf *buffer, uint32_t *inst, const struct i915_eu_operand_layout *layout, struct i915_eu_reg reg);
static uint32_t i915_eu_hw_type(struct i915_eu_buf *buffer, uint32_t type, int immediate);
static uint32_t i915_eu_file_bit(struct i915_eu_reg reg);
static void i915_eu_field(uint32_t *inst, struct i915_eu_field field, uint32_t value);
static void i915_eu_set(uint32_t *inst, unsigned high, unsigned low, uint32_t value);
static void i915_eu_bit(uint32_t *inst, unsigned position, uint32_t value);

/*
 * Prepares an empty instruction buffer.
 */
void
drv_i915_eu_init(
	struct i915_eu_buf *buffer)
{
	/* Starts with no storage, no error and no instruction to wait for. */
	buffer->words = NULL;
	buffer->count = 0U;
	buffer->capacity = 0U;
	buffer->error = 0;
	buffer->in_order = 0U;

	/* Encodes for Gen12 unless drv_i915_eu_init_gen() names another version. */
	buffer->format = &i915_eu_format_gen12;
}

/*
 * Prepares an empty instruction buffer for graphics version `graphics_ver`.
 *
 * 9 (Skylake, Kaby Lake, Coffee Lake), 11 (Ice Lake) and 12 (Tiger Lake,
 * Alder Lake) are encoded; any other version poisons the buffer, so nothing a caller emits
 * into it can be shipped.
 */
void
drv_i915_eu_init_gen(
	struct i915_eu_buf *buffer,
	unsigned graphics_ver)
{
	/* Starts as an empty Gen12 buffer. */
	drv_i915_eu_init(buffer);

	/* Gen9 has its own layout. */
	if (graphics_ver == 9U) {
		buffer->format = &i915_eu_format_gen9;
		return;
	}

	/* So has Ice Lake. */
	if (graphics_ver == 11U) {
		buffer->format = &i915_eu_format_gen11;
		return;
	}

	/* A version without a layout is refused. */
	if (graphics_ver != 12U)
		buffer->error = 1;
}

/*
 * Returns the graphics version a buffer encodes for.
 */
unsigned
drv_i915_eu_graphics_ver(
	const struct i915_eu_buf *buffer)
{
	/* The layout knows its version. */
	return buffer->format->graphics_ver;
}

/*
 * Releases the storage of an instruction buffer.
 *
 * The error and the scoreboard count are left as they are.
 */
void
drv_i915_eu_free(
	struct i915_eu_buf *buffer)
{
	/* Releases the instruction words, if any were ever reserved. */
	if (buffer->words != NULL)
		kern_free(buffer->words);

	/* Leaves the buffer empty. */
	buffer->words = NULL;
	buffer->count = 0U;
	buffer->capacity = 0U;
}

/*
 * Returns the encoded instruction words and their length in bytes.
 */
const uint32_t *
drv_i915_eu_data(
	const struct i915_eu_buf *buffer,
	size_t *bytes)
{
	/* Reports the length alongside the words. */
	*bytes = buffer->count * sizeof(uint32_t);

	/* Succeeded: the words stay owned by the buffer. */
	return buffer->words;
}

/*
 * Names a general register operand of 32-bit float type.
 */
struct i915_eu_reg
drv_i915_eu_grf(
	uint32_t nr)
{
	struct i915_eu_reg reg;

	/* Builds eight float channels of the register. */
	reg = i915_eu_grf_typed(nr, EU_TYPE_F);

	/* Succeeded: the operand is a plain SIMD8 float register. */
	return reg;
}

/*
 * Names a general register as eight unsigned 32-bit words.
 *
 * This is how URB handles, and anything else copied bit for bit, are read.
 */
struct i915_eu_reg
drv_i915_eu_grf_ud(
	uint32_t nr)
{
	struct i915_eu_reg reg;

	/* Builds eight unsigned-word channels of the register. */
	reg = i915_eu_grf_typed(nr, EU_TYPE_UD);

	/* Succeeded: the operand is a plain SIMD8 word register. */
	return reg;
}

/*
 * Names a general register as eight signed 32-bit words.
 *
 * This is how a Boolean (all ones for true, zero for false) is read.
 */
struct i915_eu_reg
drv_i915_eu_grf_d(
	uint32_t nr)
{
	struct i915_eu_reg reg;

	/* Builds eight signed-word channels of the register. */
	reg = i915_eu_grf_typed(nr, EU_TYPE_D);

	/* Succeeded: the operand is a plain SIMD8 signed-word register. */
	return reg;
}

/*
 * Names the low (or, with `high`, the high) 16 bits of each 32-bit channel
 * of a general register, as eight unsigned words.
 *
 * The region is <16;8,2>:uw from byte 0 or 2: the halves of the second
 * source of a 32-bit integer multiply lowered to two 32 x 16-bit ones,
 * which Tiger Lake needs because it has no 32 x 32-bit multiply.
 *
 * Procedure referenced: Mesa 25.0.7
 * src/intel/compiler/brw_lower_integer_multiplication.cpp.
 */
struct i915_eu_reg
drv_i915_eu_grf_uw_half(
	uint32_t nr,
	int high)
{
	struct i915_eu_reg reg;

	/* Names the register as unsigned words. */
	kern_memset(&reg, 0, sizeof(reg));
	reg.file = EU_FILE_GRF;
	reg.nr = nr;
	reg.type = EU_TYPE_UW;

	/* The high half starts two bytes into each channel. */
	reg.subnr = 0U;
	if (high != 0)
		reg.subnr = 2U;

	/* Every second word: eight of them across the register. */
	reg.vstride = EU_VSTRIDE_16;
	reg.width = EU_WIDTH_8;
	reg.hstride = EU_HSTRIDE_2;

	/* Succeeded: the operand reads one half of each channel. */
	return reg;
}

/*
 * Names a general register operand with an explicit region: the element of
 * `type` at byte `subnr`, read with <vstride;width,hstride> (each in its
 * EU_VSTRIDE_ / EU_WIDTH_ / EU_HSTRIDE_ encoding).
 *
 * The derivatives read a value's pixels in quads this way, and the pixel
 * position reads the subspan coordinates of the thread payload.
 */
struct i915_eu_reg
drv_i915_eu_grf_region(
	uint32_t nr,
	uint32_t subnr,
	uint32_t type,
	uint32_t vstride,
	uint32_t width,
	uint32_t hstride)
{
	struct i915_eu_reg reg;

	/* Names the register, the element and the type it is read as. */
	kern_memset(&reg, 0, sizeof(reg));
	reg.file = EU_FILE_GRF;
	reg.nr = nr;
	reg.subnr = subnr;
	reg.type = type;

	/* Reads it with the region given. */
	reg.vstride = vstride;
	reg.width = width;
	reg.hstride = hstride;

	/* Succeeded: the operand reads the region. */
	return reg;
}

/*
 * Names one float of a general register, replicated to every channel.
 *
 * The float sits at byte `subnr` and is read with region <0;1,0>: how a value
 * that is the same for the whole dispatch, such as a push constant, is read.
 */
struct i915_eu_reg
drv_i915_eu_grf_scalar(
	uint32_t nr,
	uint32_t subnr)
{
	struct i915_eu_reg reg;

	/* Names the register and the float inside it. */
	kern_memset(&reg, 0, sizeof(reg));
	reg.file = EU_FILE_GRF;
	reg.nr = nr;
	reg.subnr = subnr;
	reg.type = EU_TYPE_F;

	/* A zero vertical and horizontal stride replicate the one float. */
	reg.vstride = EU_VSTRIDE_0;
	reg.width = EU_WIDTH_1;
	reg.hstride = EU_HSTRIDE_0;

	/* Succeeded: the operand reads one float to every channel. */
	return reg;
}

/*
 * Returns the same operand read negated.
 *
 * It is a source modifier, not valid for a destination or an immediate.
 */
struct i915_eu_reg
drv_i915_eu_negate(
	struct i915_eu_reg reg)
{
	/* Negating twice reads the value unchanged again. */
	reg.negate = reg.negate ^ 1U;

	/* Succeeded: the operand now carries the negate modifier. */
	return reg;
}

/*
 * Returns the same operand read as its absolute value.
 *
 * It is a source modifier of a float operand, applied before any negation.
 */
struct i915_eu_reg
drv_i915_eu_abs(
	struct i915_eu_reg reg)
{
	/* The absolute value of an absolute value is the same. */
	reg.absolute = 1U;

	/* Succeeded: the operand now carries the absolute modifier. */
	return reg;
}

/*
 * Names a 32-bit float immediate operand from its bits.
 */
struct i915_eu_reg
drv_i915_eu_imm_f(
	uint32_t bits)
{
	struct i915_eu_reg reg;

	/* Carries the float bits unchanged; no floating point is involved. */
	kern_memset(&reg, 0, sizeof(reg));
	reg.file = EU_FILE_IMM;
	reg.type = EU_TYPE_F;
	reg.immediate = bits;

	/* Succeeded: the operand is a float immediate. */
	return reg;
}

/*
 * Names a 32-bit signed integer immediate operand.
 */
struct i915_eu_reg
drv_i915_eu_imm_d(
	uint32_t value)
{
	struct i915_eu_reg reg;

	/* Carries the integer bits unchanged. */
	kern_memset(&reg, 0, sizeof(reg));
	reg.file = EU_FILE_IMM;
	reg.type = EU_TYPE_D;
	reg.immediate = value;

	/* Succeeded: the operand is an integer immediate. */
	return reg;
}

/*
 * Names a 32-bit unsigned integer immediate operand.
 */
struct i915_eu_reg
drv_i915_eu_imm_ud(
	uint32_t value)
{
	struct i915_eu_reg reg;

	/* Carries the integer bits unchanged. */
	kern_memset(&reg, 0, sizeof(reg));
	reg.file = EU_FILE_IMM;
	reg.type = EU_TYPE_UD;
	reg.immediate = value;

	/* Succeeded: the operand is an unsigned integer immediate. */
	return reg;
}

/*
 * Names an immediate of eight signed 4-bit integers (V): element n in bits
 * 4 n + 3 .. 4 n.
 *
 * Adding 0x11001010 to the replicated subspan coordinates of a pixel
 * thread makes each pixel's x and y.
 *
 * Procedure referenced: Mesa 25.0.7 src/intel/compiler/brw_compile_fs.cpp (the
 * pixel position).
 */
struct i915_eu_reg
drv_i915_eu_imm_v(
	uint32_t value)
{
	struct i915_eu_reg reg;

	/* Carries the eight nibbles unchanged. */
	kern_memset(&reg, 0, sizeof(reg));
	reg.file = EU_FILE_IMM;
	reg.type = EU_TYPE_V;
	reg.immediate = value;

	/* Succeeded: the operand is a vector immediate. */
	return reg;
}

/*
 * Names the null register.
 *
 * As a float operand it carries the ordinary SIMD8 region.
 */
struct i915_eu_reg
drv_i915_eu_null(
	void)
{
	struct i915_eu_reg reg;

	/* The null register is the architecture file's register zero. */
	reg = i915_eu_grf_typed(0U, EU_TYPE_F);
	reg.file = EU_FILE_ARF;

	/* Succeeded: the operand discards a result or supplies nothing. */
	return reg;
}

/*
 * Encodes a move.
 *
 * A destination of another type than float makes it a move of that type.
 */
void
drv_i915_eu_mov(
	struct i915_eu_buf *buffer,
	struct i915_eu_reg dst,
	struct i915_eu_reg src)
{
	uint32_t *inst;

	/* Reserves the instruction; a poisoned or full buffer takes nothing. */
	inst = i915_eu_reserve(buffer);
	if (inst == NULL)
		return;

	/* Encodes an in-order move and its two operands. */
	i915_eu_common(buffer, inst, I915_EU_OPCODE_MOV, I915_EU_IN_ORDER);
	i915_eu_dst(buffer, inst, dst);
	i915_eu_src0(buffer, inst, src);
}

/*
 * Encodes a two-source arithmetic, logic or shift instruction.
 *
 * Add, multiply, and, or, exclusive or and the three shifts are encoded;
 * any other operation, and an immediate first source, poison the buffer.
 */
void
drv_i915_eu_alu2(
	struct i915_eu_buf *buffer,
	enum i915_eu_alu op,
	struct i915_eu_reg dst,
	struct i915_eu_reg src0,
	struct i915_eu_reg src1)
{
	/* Encodes the instruction for every channel of the mask. */
	i915_eu_alu2_common(buffer, 0, I915_EU_FLAG_F0_0, I915_EU_SCOPE_MASKED, op, dst, src0, src1);
}

/*
 * Encodes a two-source instruction for the channels whose bit of `flag` is
 * set.
 *
 * The operands are those of drv_i915_eu_alu2(); every other channel keeps
 * its destination.  An integer division steps this way: a channel whose
 * remainder reached the divisor subtracts it and sets its quotient bit.
 */
void
drv_i915_eu_alu2_masked(
	struct i915_eu_buf *buffer,
	enum i915_eu_flag flag,
	enum i915_eu_alu op,
	struct i915_eu_reg dst,
	struct i915_eu_reg src0,
	struct i915_eu_reg src1)
{
	/* A flag outside the two flag registers is refused. */
	if (flag >= I915_EU_FLAG_COUNT) {
		buffer->error = 1;
		return;
	}

	/* Encodes the instruction predicated on the flag. */
	i915_eu_alu2_common(buffer, 1, flag, I915_EU_SCOPE_MASKED, op, dst, src0, src1);
}

/*
 * Encodes a two-source instruction on the first channel alone, regardless
 * of the execution mask: a SIMD1 NoMask instruction that writes the one
 * dword the destination's subregister names.
 *
 * The operands are those of drv_i915_eu_alu2(), a source normally read as a
 * scalar region.  The header of a scratch message is built this way: two
 * AND copy the scratch space size and base out of r0.
 *
 * Procedure referenced: Mesa 25.0.7 src/intel/compiler/brw_generator.cpp
 * (generate_scratch_header).
 */
void
drv_i915_eu_alu2_scalar(
	struct i915_eu_buf *buffer,
	enum i915_eu_alu op,
	struct i915_eu_reg dst,
	struct i915_eu_reg src0,
	struct i915_eu_reg src1)
{
	/* Encodes the instruction for the first channel, outside the mask. */
	i915_eu_alu2_common(buffer, 0, I915_EU_FLAG_F0_0, I915_EU_SCOPE_SCALAR, op, dst, src0, src1);
}

/*
 * Encodes a two-source instruction on sixteen channels regardless of the
 * execution mask: a SIMD16 NoMask instruction in a SIMD8 kernel.
 *
 * The operands are those of drv_i915_eu_alu2(), read and written as sixteen
 * 16-bit words.  The pixel positions of a SIMD8 pixel thread are computed
 * this way: one add of sixteen words, the x and y of both subspans.
 *
 * Procedure referenced: Mesa 25.0.7 src/intel/compiler/brw_compile_fs.cpp (the
 * pixel position).
 */
void
drv_i915_eu_alu2_sixteen(
	struct i915_eu_buf *buffer,
	enum i915_eu_alu op,
	struct i915_eu_reg dst,
	struct i915_eu_reg src0,
	struct i915_eu_reg src1)
{
	/* Encodes the instruction for sixteen channels, outside the mask. */
	i915_eu_alu2_common(buffer, 0, I915_EU_FLAG_F0_0, I915_EU_SCOPE_SIXTEEN, op, dst, src0, src1);
}

/*
 * Encodes a two-source instruction on four channels regardless of the
 * execution mask: a SIMD4 NoMask instruction, the destination's first
 * dword naming the quad.
 *
 * The fine y derivative is computed this way on Gen11+, one add per quad.
 *
 * Procedure referenced: Mesa 25.0.7 src/intel/compiler/brw_generator.cpp
 * (generate_ddy).
 */
void
drv_i915_eu_alu2_four(
	struct i915_eu_buf *buffer,
	enum i915_eu_alu op,
	struct i915_eu_reg dst,
	struct i915_eu_reg src0,
	struct i915_eu_reg src1)
{
	/* Encodes the instruction for four channels, outside the mask. */
	i915_eu_alu2_common(buffer, 0, I915_EU_FLAG_F0_0, I915_EU_SCOPE_FOUR, op, dst, src0, src1);
}

/*
 * Encodes a move on all eight channels regardless of the execution mask.
 *
 * A scratch message header is cleared this way before it is filled: a
 * channel the dispatch left disabled must not keep junk in the header.
 *
 * Procedure referenced: Mesa 25.0.7 src/intel/compiler/brw_generator.cpp
 * (generate_scratch_header).
 */
void
drv_i915_eu_mov_all(
	struct i915_eu_buf *buffer,
	struct i915_eu_reg dst,
	struct i915_eu_reg src)
{
	uint32_t *inst;

	/* Reserves the instruction; a poisoned or full buffer takes nothing. */
	inst = i915_eu_reserve(buffer);
	if (inst == NULL)
		return;

	/* Encodes an in-order move and its two operands, outside the mask. */
	i915_eu_common(buffer, inst, I915_EU_OPCODE_MOV, I915_EU_IN_ORDER);
	i915_eu_scope(buffer, inst, I915_EU_SCOPE_ALL);
	i915_eu_dst(buffer, inst, dst);
	i915_eu_src0(buffer, inst, src);
}

/*
 * Encodes a move of one dword on the first channel alone, regardless of the
 * execution mask (a SIMD1 NoMask MOV).
 *
 * The offset of a scratch message goes into dword 2 of its header this way.
 *
 * Procedure referenced: Mesa 25.0.7 src/intel/compiler/brw_reg_allocate.cpp
 * (build_legacy_scratch_header).
 */
void
drv_i915_eu_mov_scalar(
	struct i915_eu_buf *buffer,
	struct i915_eu_reg dst,
	struct i915_eu_reg src)
{
	uint32_t *inst;

	/* Reserves the instruction; a poisoned or full buffer takes nothing. */
	inst = i915_eu_reserve(buffer);
	if (inst == NULL)
		return;

	/* Encodes an in-order move of the one dword, outside the mask. */
	i915_eu_common(buffer, inst, I915_EU_OPCODE_MOV, I915_EU_IN_ORDER);
	i915_eu_scope(buffer, inst, I915_EU_SCOPE_SCALAR);
	i915_eu_dst(buffer, inst, dst);
	i915_eu_src0(buffer, inst, src);
}

/*
 * Encodes a one-source operation other than a move: not, round down, round
 * toward zero, round to even or fraction.
 *
 * An operation outside the enum poisons the buffer.
 *
 * Procedure referenced: Mesa 25.0.7 src/intel/compiler/brw_fs_nir.cpp
 * (fs_nir_emit_alu: ffloor, ftrunc, fround_even, ffract, inot).
 */
void
drv_i915_eu_alu1(
	struct i915_eu_buf *buffer,
	enum i915_eu_unary op,
	struct i915_eu_reg dst,
	struct i915_eu_reg src)
{
	uint32_t *inst;
	uint32_t opcode;

	/* Picks the hardware opcode of the operation. */
	if (op == I915_EU_NOT) {
		opcode = I915_EU_OPCODE_NOT;
	} else if (op == I915_EU_RNDD) {
		opcode = I915_EU_OPCODE_RNDD;
	} else if (op == I915_EU_FRC) {
		opcode = I915_EU_OPCODE_FRC;
	} else if (op == I915_EU_RNDZ) {
		opcode = I915_EU_OPCODE_RNDZ;
	} else if (op == I915_EU_RNDE) {
		opcode = I915_EU_OPCODE_RNDE;
	} else {
		buffer->error = 1;
		return;
	}

	/* Reserves the instruction; a poisoned or full buffer takes nothing. */
	inst = i915_eu_reserve(buffer);
	if (inst == NULL)
		return;

	/* Encodes an in-order instruction and its two operands. */
	i915_eu_common(buffer, inst, opcode, I915_EU_IN_ORDER);
	i915_eu_dst(buffer, inst, dst);
	i915_eu_src0(buffer, inst, src);
}

/*
 * Encodes a comparison.
 *
 * Each channel's bit of `flag` becomes the result of `src0 cond src1`, and
 * a register destination receives all ones for true and zero for false.
 * With `predicated`, only the channels whose bit of `flag` is already set
 * are compared, so a cleared bit stays cleared: how a discard keeps the
 * pixels it has already discarded.
 *
 * Procedure referenced: Mesa 25.0.7 src/intel/compiler/brw_fs_nir.cpp
 * (nir_intrinsic_demote).
 */
void
drv_i915_eu_cmp(
	struct i915_eu_buf *buffer,
	enum i915_eu_cond cond,
	enum i915_eu_flag flag,
	int predicated,
	struct i915_eu_reg dst,
	struct i915_eu_reg src0,
	struct i915_eu_reg src1)
{
	uint32_t *inst;

	/* A test outside the enum, or a flag outside the two flag registers, is refused. */
	if (cond >= I915_EU_COND_COUNT || flag >= I915_EU_FLAG_COUNT) {
		buffer->error = 1;
		return;
	}

	/* The hardware has one immediate slot, and it belongs to the second source. */
	if (src0.file == EU_FILE_IMM) {
		buffer->error = 1;
		return;
	}

	/* Reserves the instruction; a poisoned or full buffer takes nothing. */
	inst = i915_eu_reserve(buffer);
	if (inst == NULL)
		return;

	/* Encodes an in-order comparison, the flag it writes and the test. */
	i915_eu_common(buffer, inst, I915_EU_OPCODE_CMP, I915_EU_IN_ORDER);
	i915_eu_flag(buffer, inst, flag);
	i915_eu_field(inst, buffer->format->cond_modifier, i915_eu_cond_bits[cond]);

	/* A predicated comparison reads the same flag it writes. */
	if (predicated != 0)
		i915_eu_field(inst, buffer->format->pred_control, EU_PREDICATE_NORMAL);

	/* Encodes the operands. */
	i915_eu_dst(buffer, inst, dst);
	i915_eu_src0(buffer, inst, src0);
	i915_eu_src1(buffer, inst, src1);
}

/*
 * Encodes a minimum or a maximum: SEL with a conditional modifier.
 *
 * `cond` LT keeps the smaller source and GE the larger one; no flag is
 * involved.
 *
 * Procedure referenced: Mesa 25.0.7 src/intel/compiler/brw_builder.h
 * (emit_minmax).
 */
void
drv_i915_eu_minmax(
	struct i915_eu_buf *buffer,
	enum i915_eu_cond cond,
	struct i915_eu_reg dst,
	struct i915_eu_reg src0,
	struct i915_eu_reg src1)
{
	uint32_t *inst;

	/* Only the minimum and the maximum form are encoded. */
	if (cond != I915_EU_COND_LT && cond != I915_EU_COND_GE) {
		buffer->error = 1;
		return;
	}

	/* The hardware has one immediate slot, and it belongs to the second source. */
	if (src0.file == EU_FILE_IMM) {
		buffer->error = 1;
		return;
	}

	/* Reserves the instruction; a poisoned or full buffer takes nothing. */
	inst = i915_eu_reserve(buffer);
	if (inst == NULL)
		return;

	/* Encodes an in-order selection with its test and its operands. */
	i915_eu_common(buffer, inst, I915_EU_OPCODE_SEL, I915_EU_IN_ORDER);
	i915_eu_field(inst, buffer->format->cond_modifier, i915_eu_cond_bits[cond]);
	i915_eu_dst(buffer, inst, dst);
	i915_eu_src0(buffer, inst, src0);
	i915_eu_src1(buffer, inst, src1);
}

/*
 * Encodes a per-channel selection: SEL predicated on a flag.
 *
 * A channel whose bit of `flag` is set takes `src0`, any other `src1`.
 * With integer operands the selected bits are copied unchanged.
 */
void
drv_i915_eu_select(
	struct i915_eu_buf *buffer,
	enum i915_eu_flag flag,
	struct i915_eu_reg dst,
	struct i915_eu_reg src0,
	struct i915_eu_reg src1)
{
	uint32_t *inst;

	/* A flag outside the two flag registers is refused. */
	if (flag >= I915_EU_FLAG_COUNT) {
		buffer->error = 1;
		return;
	}

	/* The hardware has one immediate slot, and it belongs to the second source. */
	if (src0.file == EU_FILE_IMM) {
		buffer->error = 1;
		return;
	}

	/* Reserves the instruction; a poisoned or full buffer takes nothing. */
	inst = i915_eu_reserve(buffer);
	if (inst == NULL)
		return;

	/* Encodes an in-order selection predicated on the flag, and its operands. */
	i915_eu_common(buffer, inst, I915_EU_OPCODE_SEL, I915_EU_IN_ORDER);
	i915_eu_flag(buffer, inst, flag);
	i915_eu_field(inst, buffer->format->pred_control, EU_PREDICATE_NORMAL);
	i915_eu_dst(buffer, inst, dst);
	i915_eu_src0(buffer, inst, src0);
	i915_eu_src1(buffer, inst, src1);
}

/*
 * Loads a flag subregister from the 16-bit word at byte `subnr` of general
 * register `nr`.
 *
 * It is a SIMD1 move outside the channel mask, which loads the dispatched
 * pixels of a fragment thread into the discard flag.
 *
 * Procedure referenced: Mesa 25.0.7 src/intel/compiler/brw_compile_fs.cpp (the
 * discard flag).
 */
void
drv_i915_eu_flag_load(
	struct i915_eu_buf *buffer,
	enum i915_eu_flag flag,
	uint32_t nr,
	uint32_t subnr)
{
	struct i915_eu_reg dst;
	struct i915_eu_reg src;
	uint32_t *inst;

	/* A flag outside the two flag registers is refused. */
	if (flag >= I915_EU_FLAG_COUNT) {
		buffer->error = 1;
		return;
	}

	/* The flag register and the byte of its subregister, as a 16-bit destination. */
	kern_memset(&dst, 0, sizeof(dst));
	dst.file = EU_FILE_ARF;
	dst.nr = EU_ARF_FLAG + (uint32_t)flag / 2U;
	dst.subnr = ((uint32_t)flag % 2U) * 2U;
	dst.type = EU_TYPE_UW;

	/* The word of the general register, read once for the one channel. */
	src = drv_i915_eu_grf_scalar(nr, subnr);
	src.type = EU_TYPE_UW;

	/* Reserves the instruction; a poisoned or full buffer takes nothing. */
	inst = i915_eu_reserve(buffer);
	if (inst == NULL)
		return;

	/* Encodes an in-order move, then narrows it to one channel outside the mask. */
	i915_eu_common(buffer, inst, I915_EU_OPCODE_MOV, I915_EU_IN_ORDER);
	i915_eu_field(inst, buffer->format->exec_size, EU_EXEC_SIZE_1);
	i915_eu_bit(inst, buffer->format->no_mask_bit, 1U);
	i915_eu_dst(buffer, inst, dst);
	i915_eu_src0(buffer, inst, src);
}

/*
 * Clears a flag subregister: a SIMD1 move of 0 outside the channel mask.
 *
 * A comparison writes only the bits of the channels it runs on, so the bits
 * of the others stay what they were; the flag is cleared this way before
 * the comparison of an "any" vote.
 *
 * Procedure referenced: Mesa 25.0.7
 * src/intel/compiler/brw_lower_subgroup_ops.cpp (the "any" vote).
 */
void
drv_i915_eu_flag_clear(
	struct i915_eu_buf *buffer,
	enum i915_eu_flag flag)
{
	struct i915_eu_reg dst;
	struct i915_eu_reg src;
	uint32_t *inst;

	/* A flag outside the two flag registers is refused. */
	if (flag >= I915_EU_FLAG_COUNT) {
		buffer->error = 1;
		return;
	}

	/* The flag register and the byte of its subregister, as a 16-bit destination. */
	kern_memset(&dst, 0, sizeof(dst));
	dst.file = EU_FILE_ARF;
	dst.nr = EU_ARF_FLAG + (uint32_t)flag / 2U;
	dst.subnr = ((uint32_t)flag % 2U) * 2U;
	dst.type = EU_TYPE_UW;

	/* A 16-bit zero. */
	src = drv_i915_eu_imm_ud(0U);
	src.type = EU_TYPE_UW;

	/* Reserves the instruction; a poisoned or full buffer takes nothing. */
	inst = i915_eu_reserve(buffer);
	if (inst == NULL)
		return;

	/* Encodes an in-order move, then narrows it to one channel outside the mask. */
	i915_eu_common(buffer, inst, I915_EU_OPCODE_MOV, I915_EU_IN_ORDER);
	i915_eu_field(inst, buffer->format->exec_size, EU_EXEC_SIZE_1);
	i915_eu_bit(inst, buffer->format->no_mask_bit, 1U);
	i915_eu_dst(buffer, inst, dst);
	i915_eu_src0(buffer, inst, src);
}

/*
 * Refuses a multiply-add: the three-source operand layout is not encoded.
 */
void
drv_i915_eu_mad(
	struct i915_eu_buf *buffer,
	struct i915_eu_reg dst,
	struct i915_eu_reg src0,
	struct i915_eu_reg src1,
	struct i915_eu_reg src2)
{
	uint32_t *inst;
	uint32_t vstride0;
	uint32_t vstride1;

	/*
	 * Only the Gen12 three-source layout is encoded, for float registers: a
	 * whole destination register, sources that are whole registers read
	 * <8;8,1> or one float replicated <0;1,0>.  Anything else poisons the
	 * buffer (the code generator then keeps MUL and ADD).
	 */
	if (buffer->format->has_swsb == 0 ||
	    dst.file != EU_FILE_GRF || dst.type != EU_TYPE_F || dst.subnr != 0U ||
	    i915_eu_mad_source(src0, &vstride0) != 0 ||
	    i915_eu_mad_source(src1, &vstride1) != 0 ||
	    i915_eu_mad_source(src2, NULL) != 0) {
		buffer->error = 1;
		return;
	}

	/* Reserves the instruction; a poisoned or full buffer takes nothing. */
	inst = i915_eu_reserve(buffer);
	if (inst == NULL)
		return;

	/* The opcode, SIMD8 and the in-order dependency, as for any ALU instruction. */
	i915_eu_common(buffer, inst, I915_EU_OPCODE_MAD, I915_EU_IN_ORDER);

	/* The float execution and the destination: a whole register, stride 1. */
	i915_eu_bit(inst, EU_3SRC_EXEC_FLOAT_BIT, 1U);
	i915_eu_bits(inst, EU_3SRC_DST_REG_NR_HI, EU_3SRC_DST_REG_NR_LO, dst.nr);
	i915_eu_bit(inst, EU_3SRC_DST_FILE_BIT, 1U);
	i915_eu_bits(inst, EU_3SRC_DST_TYPE_HI, EU_3SRC_DST_TYPE_LO, EU_3SRC_TYPE_F);

	/* Source 0, the addend. */
	i915_eu_bits(inst, EU_3SRC_SRC0_REG_NR_HI, EU_3SRC_SRC0_REG_NR_LO, src0.nr);
	i915_eu_bits(inst, EU_3SRC_SRC0_SUBREG_HI, EU_3SRC_SRC0_SUBREG_LO, src0.subnr);
	i915_eu_bit(inst, EU_3SRC_SRC0_FILE_BIT, 1U);
	i915_eu_bits(inst, EU_3SRC_SRC0_HSTRIDE_HI, EU_3SRC_SRC0_HSTRIDE_LO, src0.hstride == EU_HSTRIDE_0 ? 0U : 1U);
	i915_eu_bit(inst, EU_3SRC_SRC0_VSTRIDE_HIGH_BIT, vstride0 >> 1);
	i915_eu_bit(inst, EU_3SRC_SRC0_VSTRIDE_LOW_BIT, vstride0 & 1U);
	i915_eu_bits(inst, EU_3SRC_SRC0_TYPE_HI, EU_3SRC_SRC0_TYPE_LO, EU_3SRC_TYPE_F);
	i915_eu_bit(inst, EU_3SRC_SRC0_NEGATE_BIT, src0.negate & 1U);
	i915_eu_bit(inst, EU_3SRC_SRC0_ABS_BIT, src0.absolute & 1U);

	/* Source 1, the first factor. */
	i915_eu_bits(inst, EU_3SRC_SRC1_REG_NR_HI, EU_3SRC_SRC1_REG_NR_LO, src1.nr);
	i915_eu_bits(inst, EU_3SRC_SRC1_SUBREG_HI, EU_3SRC_SRC1_SUBREG_LO, src1.subnr);
	i915_eu_bit(inst, EU_3SRC_SRC1_FILE_BIT, 1U);
	i915_eu_bits(inst, EU_3SRC_SRC1_HSTRIDE_HI, EU_3SRC_SRC1_HSTRIDE_LO, src1.hstride == EU_HSTRIDE_0 ? 0U : 1U);
	i915_eu_bit(inst, EU_3SRC_SRC1_VSTRIDE_HIGH_BIT, vstride1 >> 1);
	i915_eu_bit(inst, EU_3SRC_SRC1_VSTRIDE_LOW_BIT, vstride1 & 1U);
	i915_eu_bits(inst, EU_3SRC_SRC1_TYPE_HI, EU_3SRC_SRC1_TYPE_LO, EU_3SRC_TYPE_F);
	i915_eu_bit(inst, EU_3SRC_SRC1_NEGATE_BIT, src1.negate & 1U);
	i915_eu_bit(inst, EU_3SRC_SRC1_ABS_BIT, src1.absolute & 1U);

	/* Source 2, the second factor: its vertical stride is implied. */
	i915_eu_bits(inst, EU_3SRC_SRC2_REG_NR_HI, EU_3SRC_SRC2_REG_NR_LO, src2.nr);
	i915_eu_bits(inst, EU_3SRC_SRC2_SUBREG_HI, EU_3SRC_SRC2_SUBREG_LO, src2.subnr);
	i915_eu_bit(inst, EU_3SRC_SRC2_FILE_BIT, 1U);
	i915_eu_bits(inst, EU_3SRC_SRC2_HSTRIDE_HI, EU_3SRC_SRC2_HSTRIDE_LO, src2.hstride == EU_HSTRIDE_0 ? 0U : 1U);
	i915_eu_bits(inst, EU_3SRC_SRC2_TYPE_HI, EU_3SRC_SRC2_TYPE_LO, EU_3SRC_TYPE_F);
	i915_eu_bit(inst, EU_3SRC_SRC2_NEGATE_BIT, src2.negate & 1U);
	i915_eu_bit(inst, EU_3SRC_SRC2_ABS_BIT, src2.absolute & 1U);
}

/*
 * Checks that a MAD source is a float register read whole (<8;8,1>) or as
 * one replicated float (<0;1,0>), and gives its three-source vertical
 * stride (NULL for source two, which has none).  Returns 0, or -1 for a
 * source the layout cannot say.
 */
static int
i915_eu_mad_source(
	struct i915_eu_reg reg,
	uint32_t *vstride)
{
	/* A float register only. */
	if (reg.file != EU_FILE_GRF || reg.type != EU_TYPE_F)
		return -1;

	/* One float replicated. */
	if (reg.vstride == EU_VSTRIDE_0 && reg.hstride == EU_HSTRIDE_0) {
		if (vstride != NULL)
			*vstride = EU_3SRC_VSTRIDE_0;
		return 0;
	}

	/* A whole register, one float to a channel. */
	if (reg.vstride == EU_VSTRIDE_8 && reg.hstride == EU_HSTRIDE_1 && reg.subnr == 0U) {
		if (vstride != NULL)
			*vstride = EU_3SRC_VSTRIDE_8;
		return 0;
	}

	/* Succeeded: no other region. */
	return -1;
}

/* Sets the bits `high` down to `low` of an instruction to `value`. */
static void
i915_eu_bits(
	uint32_t *inst,
	uint32_t high,
	uint32_t low,
	uint32_t value)
{
	struct i915_eu_field field;

	/* As a field of the layout. */
	field.high = high;
	field.low = low;
	i915_eu_field(inst, field, value);
}

/*
 * Encodes a math function: a one-operand float function, whose second
 * source is the null register, or the quotient or the remainder of an
 * integer division of src0 by src1.
 *
 * Math is out of order, so a sync.nop on its destination follows it (see the
 * scoreboard note above).  The integer division reads its operands as their
 * type says (signed D or unsigned UD) and takes no source modifier; a
 * negated or absolute integer source poisons the buffer.
 *
 * Procedure referenced: Mesa 25.0.7 src/intel/compiler/brw_eu_emit.c
 * (gfx6_math).
 */
void
drv_i915_eu_math(
	struct i915_eu_buf *buffer,
	enum i915_eu_math func,
	struct i915_eu_reg dst,
	struct i915_eu_reg src0,
	struct i915_eu_reg src1)
{
	uint32_t *inst;
	uint32_t selector;

	/* The function selector maps to the Gen math sub-opcode. */
	if (func == I915_EU_MATH_INV) {
		selector = EU_MATH_INV;
	} else if (func == I915_EU_MATH_RSQ) {
		selector = EU_MATH_RSQ;
	} else if (func == I915_EU_MATH_SQRT) {
		selector = EU_MATH_SQRT;
	} else if (func == I915_EU_MATH_SIN) {
		selector = EU_MATH_SIN;
	} else if (func == I915_EU_MATH_COS) {
		selector = EU_MATH_COS;
	} else if (func == I915_EU_MATH_LOG) {
		selector = EU_MATH_LOG;
	} else if (func == I915_EU_MATH_EXP) {
		selector = EU_MATH_EXP;
	} else if (func == I915_EU_MATH_INT_QUOTIENT) {
		selector = EU_MATH_INT_DIV_QUOTIENT;
	} else if (func == I915_EU_MATH_INT_REMAINDER) {
		selector = EU_MATH_INT_DIV_REMAINDER;
	} else {
		buffer->error = 1;
		return;
	}

	/* The integer division takes no source modifier. */
	if (func == I915_EU_MATH_INT_QUOTIENT || func == I915_EU_MATH_INT_REMAINDER) {
		if (src0.negate != 0U ||
		    src0.absolute != 0U ||
		    src1.negate != 0U ||
		    src1.absolute != 0U) {
			buffer->error = 1;
			return;
		}
	}

	/* Reserves the instruction; a poisoned or full buffer takes nothing. */
	inst = i915_eu_reserve(buffer);
	if (inst == NULL)
		return;

	/* Encodes an out-of-order math instruction, its function and its operands. */
	i915_eu_common(buffer, inst, I915_EU_OPCODE_MATH, I915_EU_OUT_OF_ORDER);
	i915_eu_field(inst, buffer->format->math_function, selector);
	i915_eu_dst(buffer, inst, dst);
	i915_eu_src0(buffer, inst, src0);
	i915_eu_src1(buffer, inst, src1);

	/* Waits until the math has written its destination. */
	i915_eu_sync(buffer, EU_SWSB_SYNC_DST(I915_EU_TOKEN));
}

/*
 * Encodes a message to a shared function.
 *
 * `src0` and, for a split payload, `src1` are the first registers of the two
 * payload runs whose lengths the descriptors carry (desc: mlen, rlen, the
 * function's control; ex_desc: the length of the second run).
 * `conditional` selects SENDC, the form a render-target write takes.
 * `end_of_thread`: the message retires the thread (its payload must then sit
 * in r112..r127, which is the caller's business).
 */
void
drv_i915_eu_send(
	struct i915_eu_buf *buffer,
	struct i915_eu_reg dst,
	struct i915_eu_reg src0,
	struct i915_eu_reg src1,
	uint32_t sfid,
	uint32_t descriptor,
	uint32_t ex_descriptor,
	int conditional,
	int end_of_thread)
{
	/* Encodes the message for every channel of the mask. */
	i915_eu_send_common(buffer,
			    0,
			    I915_EU_FLAG_F0_0,
			    I915_EU_SCOPE_MASKED,
			    dst,
			    src0,
			    src1,
			    sfid,
			    descriptor,
			    ex_descriptor,
			    conditional,
			    end_of_thread);
}

/*
 * Encodes a message to a shared function for the channels whose bit of
 * `flag` is set.
 *
 * The operands are those of drv_i915_eu_send().  A render-target write that
 * ends the thread is sent this way when the shader discards: the pixels the
 * flag no longer holds are not written, and the thread still ends when no
 * pixel is left.
 *
 * Procedure referenced: Mesa 25.0.7 src/intel/compiler/brw_compile_fs.cpp (the
 * render-target write predicated on the discard flag).
 */
void
drv_i915_eu_send_masked(
	struct i915_eu_buf *buffer,
	enum i915_eu_flag flag,
	struct i915_eu_reg dst,
	struct i915_eu_reg src0,
	struct i915_eu_reg src1,
	uint32_t sfid,
	uint32_t descriptor,
	uint32_t ex_descriptor,
	int conditional,
	int end_of_thread)
{
	/* A flag outside the two flag registers is refused. */
	if (flag >= I915_EU_FLAG_COUNT) {
		buffer->error = 1;
		return;
	}

	/* Encodes the message predicated on the flag. */
	i915_eu_send_common(buffer,
			    1,
			    flag,
			    I915_EU_SCOPE_MASKED,
			    dst,
			    src0,
			    src1,
			    sfid,
			    descriptor,
			    ex_descriptor,
			    conditional,
			    end_of_thread);
}

/*
 * Encodes a message to a shared function on all eight channels regardless
 * of the execution mask.
 *
 * The operands are those of drv_i915_eu_send().  A spilled register is
 * read back from scratch memory this way: the fill is a temporary of the
 * instruction that needs it, so every channel of it is read, whichever are
 * enabled.
 *
 * Procedure referenced: Mesa 25.0.7 src/intel/compiler/brw_reg_allocate.cpp
 * (emit_unspill).
 */
void
drv_i915_eu_send_all(
	struct i915_eu_buf *buffer,
	struct i915_eu_reg dst,
	struct i915_eu_reg src0,
	struct i915_eu_reg src1,
	uint32_t sfid,
	uint32_t descriptor,
	uint32_t ex_descriptor)
{
	/* Encodes the message for all eight channels, outside the mask. */
	i915_eu_send_common(buffer,
			    0,
			    I915_EU_FLAG_F0_0,
			    I915_EU_SCOPE_ALL,
			    dst,
			    src0,
			    src1,
			    sfid,
			    descriptor,
			    ex_descriptor,
			    0,
			    0);
}

/*
 * Encodes the message that ends the thread for all eight channels, outside
 * the mask.
 *
 * A compute thread ends this way: a one-register message with no
 * destination to the thread spawner.
 */
void
drv_i915_eu_send_all_end(
	struct i915_eu_buf *buffer,
	struct i915_eu_reg src0,
	uint32_t sfid,
	uint32_t descriptor)
{
	/* Encodes the message for all eight channels, outside the mask, with the end-of-thread bit. */
	i915_eu_send_common(buffer,
			    0,
			    I915_EU_FLAG_F0_0,
			    I915_EU_SCOPE_ALL,
			    drv_i915_eu_null(),
			    src0,
			    drv_i915_eu_null(),
			    sfid,
			    descriptor,
			    0U,
			    0,
			    1);
}

/*
 * Encodes a message to a shared function on the first channel alone,
 * regardless of the execution mask (a SIMD1 NoMask SEND).
 *
 * The operands are those of drv_i915_eu_send().  A memory fence is sent
 * this way: one message for the thread, whose one-register reply says the
 * fence is done.
 *
 * Procedure referenced: Mesa 25.0.7 src/intel/compiler/brw_eu_emit.c
 * (brw_memory_fence).
 */
void
drv_i915_eu_send_scalar(
	struct i915_eu_buf *buffer,
	struct i915_eu_reg dst,
	struct i915_eu_reg src0,
	uint32_t sfid,
	uint32_t descriptor)
{
	/* Encodes the message for the first channel, outside the mask. */
	i915_eu_send_common(buffer,
			    0,
			    I915_EU_FLAG_F0_0,
			    I915_EU_SCOPE_SCALAR,
			    dst,
			    src0,
			    drv_i915_eu_null(),
			    sfid,
			    descriptor,
			    0U,
			    0,
			    0);
}

/*
 * Encodes a sync of function `function` (EU_SYNC_*) with no dependency:
 * sync.bar waits until the thread's workgroup barrier message is answered,
 * sync.allwr until every write the thread sent is done.
 *
 * Gen9 has no SYNC.  Its barrier wait is a WAIT on the notification
 * register, which the gateway signals when the group's barrier is complete;
 * any other function poisons the buffer.
 *
 * Procedure referenced: Mesa 25.0.7 src/intel/compiler/brw_generator.cpp
 * (generate_barrier).
 */
void
drv_i915_eu_sync_function(
	struct i915_eu_buf *buffer,
	uint32_t function)
{
	uint32_t *inst;

	/* Gen9 waits for the barrier with a WAIT and has nothing for the other functions. */
	if (buffer->format->has_swsb == 0) {
		/* The barrier's wait is a WAIT on n0; nothing else has a Gen9 form. */
		if (function == EU_SYNC_BAR) {
			i915_eu_wait_notification(buffer);
		} else {
			buffer->error = 1;
		}

		return;
	}

	/* Reserves the instruction; a poisoned or full buffer takes nothing. */
	inst = i915_eu_reserve(buffer);
	if (inst == NULL)
		return;

	/* Encodes a SIMD1 sync of the function, outside the channel mask, with no scoreboard dependency. */
	i915_eu_field(inst, buffer->format->opcode, buffer->format->opcodes[I915_EU_OPCODE_SYNC]);
	i915_eu_field(inst, buffer->format->cond_modifier, function);
	i915_eu_field(inst, buffer->format->exec_size, EU_EXEC_SIZE_1);
	i915_eu_bit(inst, buffer->format->no_mask_bit, 1U);
}

/*
 * Encodes a no-op.
 */
void
drv_i915_eu_nop(
	struct i915_eu_buf *buffer)
{
	uint32_t *inst;

	/* Reserves the instruction; a poisoned or full buffer takes nothing. */
	inst = i915_eu_reserve(buffer);
	if (inst == NULL)
		return;

	/* Only the opcode is set; the no-op carries no scoreboard byte. */
	i915_eu_field(inst, buffer->format->opcode, buffer->format->opcodes[I915_EU_OPCODE_NOP]);
}

/*
 * Returns the index of the instruction encoded next.
 *
 * A loop remembers it at its start, for the WHILE at its end to jump back
 * to.
 */
uint32_t
drv_i915_eu_position(
	const struct i915_eu_buf *buffer)
{
	/* Four words to an instruction. */
	return (uint32_t)(buffer->count / GEN12_EU_DWORDS);
}

/*
 * Encodes the WHILE that ends a loop: the channels whose bit of `flag` is
 * set jump back to instruction `target`, the others wait after the WHILE
 * until the loop ends.
 *
 * The form is a null signed-integer destination, SIMD8, the jump in bytes
 * from the WHILE to the target in the JIP (on Gen9 after an immediate
 * source 0, see i915_eu_branch_target()).  A target at or after the WHILE,
 * or a poisoned buffer, encodes nothing.
 *
 * Procedure referenced: Mesa 25.0.7 src/intel/compiler/brw_eu_emit.c
 * (brw_WHILE).
 */
void
drv_i915_eu_while(
	struct i915_eu_buf *buffer,
	enum i915_eu_flag flag,
	uint32_t target)
{
	struct i915_eu_reg null;
	uint32_t *inst;
	uint32_t here;
	int32_t jump;

	/* A flag outside the two flag registers is refused. */
	if (flag >= I915_EU_FLAG_COUNT) {
		buffer->error = 1;
		return;
	}

	/* A loop jumps back to an instruction before its end. */
	here = drv_i915_eu_position(buffer);
	if (target >= here) {
		buffer->error = 1;
		return;
	}

	/* Reserves the instruction; a poisoned or full buffer takes nothing. */
	inst = i915_eu_reserve(buffer);
	if (inst == NULL)
		return;

	/* The jump, in bytes, back from the WHILE to the first instruction of the loop. */
	jump = -(int32_t)((here - target) * GEN12_EU_DWORDS * 4U);

	/* Encodes an in-order WHILE predicated on the flag, its null destination and its jump. */
	i915_eu_common(buffer, inst, I915_EU_OPCODE_WHILE, I915_EU_IN_ORDER);
	i915_eu_flag(buffer, inst, flag);
	i915_eu_field(inst, buffer->format->pred_control, EU_PREDICATE_NORMAL);
	null = drv_i915_eu_null();
	null.type = EU_TYPE_D;
	i915_eu_dst(buffer, inst, null);
	i915_eu_branch_target(buffer, inst, 0);
	i915_eu_field(inst, buffer->format->jip, (uint32_t)jump);
}

/*
 * Moves a flag subregister's sixteen channel bits, zero-extended, into the
 * dword at byte `subnr` of general register `nr`: one channel, outside the
 * mask.  The counterpart of drv_i915_eu_flag_load().
 */
void
drv_i915_eu_flag_store(
	struct i915_eu_buf *buffer,
	enum i915_eu_flag flag,
	uint32_t nr,
	uint32_t subnr)
{
	struct i915_eu_reg dst;
	struct i915_eu_reg src;
	uint32_t *inst;

	/* A flag outside the two flag registers is refused. */
	if (flag >= I915_EU_FLAG_COUNT) {
		buffer->error = 1;
		return;
	}

	/* The dword of the general register, written for the one channel. */
	dst = drv_i915_eu_grf_scalar(nr, subnr);
	dst.type = EU_TYPE_UD;
	dst.hstride = EU_HSTRIDE_1;

	/* The flag register and the byte of its subregister, as a 16-bit scalar source. */
	kern_memset(&src, 0, sizeof(src));
	src.file = EU_FILE_ARF;
	src.nr = EU_ARF_FLAG + (uint32_t)flag / 2U;
	src.subnr = ((uint32_t)flag % 2U) * 2U;
	src.type = EU_TYPE_UW;
	src.vstride = EU_VSTRIDE_0;
	src.width = EU_WIDTH_1;
	src.hstride = EU_HSTRIDE_0;

	/* Reserves the instruction; a poisoned or full buffer takes nothing. */
	inst = i915_eu_reserve(buffer);
	if (inst == NULL)
		return;

	/* Encodes an in-order move, then narrows it to one channel outside the mask. */
	i915_eu_common(buffer, inst, I915_EU_OPCODE_MOV, I915_EU_IN_ORDER);
	i915_eu_field(inst, buffer->format->exec_size, EU_EXEC_SIZE_1);
	i915_eu_bit(inst, buffer->format->no_mask_bit, 1U);
	i915_eu_dst(buffer, inst, dst);
	i915_eu_src0(buffer, inst, src);
}

/*
 * Moves a flag subregister's sixteen channel bits into the 16-bit word at
 * byte `subnr` of general register `nr`: one channel, outside the mask.
 *
 * Gen9's render-target write header takes the pixels a discarding shader
 * still holds this way, in its word 15.
 *
 * Values referenced: Mesa 25.0.7
 * src/intel/compiler/brw_lower_logical_sends.cpp (lower_fb_write_logical_send,
 * before Gen11).
 */
void
drv_i915_eu_flag_store_word(
	struct i915_eu_buf *buffer,
	enum i915_eu_flag flag,
	uint32_t nr,
	uint32_t subnr)
{
	struct i915_eu_reg dst;
	struct i915_eu_reg src;

	/* A flag outside the two flag registers is refused. */
	if (flag >= I915_EU_FLAG_COUNT) {
		buffer->error = 1;
		return;
	}

	/* The word of the general register, written for the one channel. */
	dst = drv_i915_eu_grf_region(nr, subnr, EU_TYPE_UW, EU_VSTRIDE_0, EU_WIDTH_1, EU_HSTRIDE_1);

	/* The flag register and the byte of its subregister, as a 16-bit scalar source. */
	kern_memset(&src, 0, sizeof(src));
	src.file = EU_FILE_ARF;
	src.nr = EU_ARF_FLAG + (uint32_t)flag / 2U;
	src.subnr = ((uint32_t)flag % 2U) * 2U;
	src.type = EU_TYPE_UW;
	src.vstride = EU_VSTRIDE_0;
	src.width = EU_WIDTH_1;
	src.hstride = EU_HSTRIDE_0;

	/* Encodes a move of the one word, outside the mask. */
	drv_i915_eu_mov_scalar(buffer, dst, src);
}

/*
 * Encodes an IF on `flag`: the channels whose bit of the flag is clear stop
 * running until the matching ENDIF, and when no channel is left the thread
 * jumps to the ENDIF.  Returns the IF's position; its targets are written by
 * drv_i915_eu_patch_if() once the ENDIF is placed.
 *
 * The form is a null signed-integer destination, SIMD8 under the execution
 * mask, the JIP and the UIP immediate (on Gen9 after an immediate source 0,
 * see i915_eu_branch_target()).
 *
 * Procedure referenced: Mesa 25.0.7 src/intel/compiler/brw_eu_emit.c (brw_IF).
 */
uint32_t
drv_i915_eu_if(
	struct i915_eu_buf *buffer,
	enum i915_eu_flag flag)
{
	struct i915_eu_reg null;
	uint32_t *inst;
	uint32_t here;

	/* The IF's position, which the patch names it by. */
	here = drv_i915_eu_position(buffer);

	/* A flag outside the two flag registers is refused. */
	if (flag >= I915_EU_FLAG_COUNT) {
		buffer->error = 1;
		return here;
	}

	/* Reserves the instruction; a poisoned or full buffer takes nothing. */
	inst = i915_eu_reserve(buffer);
	if (inst == NULL)
		return here;

	/* Encodes an in-order IF predicated on the flag, with a null destination. */
	i915_eu_common(buffer, inst, I915_EU_OPCODE_IF, I915_EU_IN_ORDER);
	i915_eu_flag(buffer, inst, flag);
	i915_eu_field(inst, buffer->format->pred_control, EU_PREDICATE_NORMAL);
	null = drv_i915_eu_null();
	null.type = EU_TYPE_D;
	i915_eu_dst(buffer, inst, null);

	/* Both targets are immediates, zero until the ENDIF is placed. */
	i915_eu_branch_target(buffer, inst, 1);

	/* Succeeded: the IF waits for its targets. */
	return here;
}

/*
 * Encodes the ENDIF of an IF: the channels the IF stopped run again.
 * Returns its position.  Its JIP -- where the thread goes when no channel is
 * left after it -- is the next instruction until drv_i915_eu_patch_endif()
 * points it at the end of an enclosing loop.
 *
 * Procedure referenced: Mesa 25.0.7 src/intel/compiler/brw_eu_emit.c
 * (brw_set_uip_jip).
 */
uint32_t
drv_i915_eu_endif(
	struct i915_eu_buf *buffer)
{
	uint32_t *inst;
	uint32_t here;

	/* The ENDIF's position, which the patches name it by. */
	here = drv_i915_eu_position(buffer);

	/* Reserves the instruction; a poisoned or full buffer takes nothing. */
	inst = i915_eu_reserve(buffer);
	if (inst == NULL)
		return here;

	/*
	 * Encodes an in-order ENDIF whose JIP is the next instruction, in bytes;
	 * the JIP is a signed-integer immediate source 0 (the immediate's
	 * value is the JIP's word, set after it).
	 */
	i915_eu_common(buffer, inst, I915_EU_OPCODE_ENDIF, I915_EU_IN_ORDER);
	i915_eu_src0(buffer, inst, drv_i915_eu_imm_d(0U));
	i915_eu_field(inst, buffer->format->jip, GEN12_EU_DWORDS * 4U);

	/* Succeeded: the ENDIF is placed. */
	return here;
}

/*
 * Points an IF's JIP and UIP at its ENDIF, for an IF without an ELSE.
 * Positions outside what the buffer holds poison it.
 *
 * Procedure referenced: Mesa 25.0.7 src/intel/compiler/brw_eu_emit.c
 * (patch_IF_ELSE).
 */
void
drv_i915_eu_patch_if(
	struct i915_eu_buf *buffer,
	uint32_t if_position,
	uint32_t endif_position)
{
	uint32_t *inst;
	uint32_t jump;
	uint32_t end;

	/* A poisoned buffer encoded neither instruction. */
	if (buffer->error != 0)
		return;

	/* The ENDIF comes after its IF, and both are in the buffer. */
	end = drv_i915_eu_position(buffer);
	if (endif_position <= if_position || endif_position >= end) {
		buffer->error = 1;
		return;
	}

	/* Both targets are the ENDIF, in bytes from the IF. */
	inst = buffer->words + (size_t)if_position * GEN12_EU_DWORDS;
	jump = (endif_position - if_position) * GEN12_EU_DWORDS * 4U;
	i915_eu_field(inst, buffer->format->jip, jump);
	i915_eu_field(inst, buffer->format->uip, jump);
}

/*
 * Points an ENDIF's JIP at `target`, the WHILE of the loop it is inside.
 * Positions outside what the buffer holds poison it.
 */
void
drv_i915_eu_patch_endif(
	struct i915_eu_buf *buffer,
	uint32_t endif_position,
	uint32_t target)
{
	uint32_t *inst;
	uint32_t end;

	/* A poisoned buffer encoded neither instruction. */
	if (buffer->error != 0)
		return;

	/* The target comes after the ENDIF, and both are in the buffer. */
	end = drv_i915_eu_position(buffer);
	if (target <= endif_position || target >= end) {
		buffer->error = 1;
		return;
	}

	/* The JIP, in bytes from the ENDIF. */
	inst = buffer->words + (size_t)endif_position * GEN12_EU_DWORDS;
	i915_eu_field(inst, buffer->format->jip, (target - endif_position) * GEN12_EU_DWORDS * 4U);
}

/*
 * Encodes a two-source instruction on the channels `scope` names,
 * predicated on `flag` when `predicated` is nonzero (see drv_i915_eu_alu2()).
 */
static void
i915_eu_alu2_common(
	struct i915_eu_buf *buffer,
	int predicated,
	enum i915_eu_flag flag,
	int scope,
	enum i915_eu_alu op,
	struct i915_eu_reg dst,
	struct i915_eu_reg src0,
	struct i915_eu_reg src1)
{
	uint32_t *inst;
	uint32_t opcode;

	/* Picks the hardware opcode; a subtraction is refused. */
	if (op == I915_EU_ADD) {
		opcode = I915_EU_OPCODE_ADD;
	} else if (op == I915_EU_MUL) {
		opcode = I915_EU_OPCODE_MUL;
	} else if (op == I915_EU_AND) {
		opcode = I915_EU_OPCODE_AND;
	} else if (op == I915_EU_OR) {
		opcode = I915_EU_OPCODE_OR;
	} else if (op == I915_EU_XOR) {
		opcode = I915_EU_OPCODE_XOR;
	} else if (op == I915_EU_SHL) {
		opcode = I915_EU_OPCODE_SHL;
	} else if (op == I915_EU_SHR) {
		opcode = I915_EU_OPCODE_SHR;
	} else if (op == I915_EU_ASR) {
		opcode = I915_EU_OPCODE_ASR;
	} else {
		buffer->error = 1;
		return;
	}

	/* The hardware has one immediate slot, and it belongs to the second source. */
	if (src0.file == EU_FILE_IMM) {
		buffer->error = 1;
		return;
	}

	/* Reserves the instruction; a poisoned or full buffer takes nothing. */
	inst = i915_eu_reserve(buffer);
	if (inst == NULL)
		return;

	/* Encodes an in-order instruction on its channels and its three operands. */
	i915_eu_common(buffer, inst, opcode, I915_EU_IN_ORDER);
	i915_eu_scope(buffer, inst, scope);
	i915_eu_dst(buffer, inst, dst);
	i915_eu_src0(buffer, inst, src0);
	i915_eu_src1(buffer, inst, src1);

	/* A masked instruction runs only on the channels whose bit of the flag is set. */
	if (predicated != 0) {
		i915_eu_flag(buffer, inst, flag);
		i915_eu_field(inst, buffer->format->pred_control, EU_PREDICATE_NORMAL);
	}
}

/*
 * Encodes a SEND or SENDC on the channels `scope` names, predicated on
 * `flag` when `predicated` is nonzero (see drv_i915_eu_send()).
 */
static void
i915_eu_send_common(
	struct i915_eu_buf *buffer,
	int predicated,
	enum i915_eu_flag flag,
	int scope,
	struct i915_eu_reg dst,
	struct i915_eu_reg src0,
	struct i915_eu_reg src1,
	uint32_t sfid,
	uint32_t descriptor,
	uint32_t ex_descriptor,
	int conditional,
	int end_of_thread)
{
	const struct i915_eu_format *format;
	enum i915_eu_opcode opcode;
	uint32_t *inst;
	uint32_t eot;
	uint32_t swsb;
	int split;
	int order;

	/* The layout the buffer encodes in. */
	format = buffer->format;

	/* The low six bits of the extended descriptor are not encodable (they held the SFID once). */
	if ((ex_descriptor & I915_EU_EX_DESC_LOW_MASK) != 0U) {
		buffer->error = 1;
		return;
	}

	/* The shared-function field has four bits. */
	if (sfid > I915_EU_SFID_MAX) {
		buffer->error = 1;
		return;
	}

	/*
	 * Gen9 says a second payload with SENDS, whose extended descriptor has
	 * room for its bits 31:16 and the length in 9:6 only, and keeps the
	 * descriptor in 31 bits beside the end-of-thread bit.
	 */
	split = 0;
	if (format->has_swsb == 0) {
		/* Bit 31 of the descriptor would land on the end-of-thread bit. */
		if ((descriptor >> 31) != 0U) {
			buffer->error = 1;
			return;
		}

		/* The extended descriptor's middle bits (a render target index, say) have no place. */
		if ((ex_descriptor & GEN9_EU_SENDS_EX_DESC_ZERO_MASK) != 0U) {
			buffer->error = 1;
			return;
		}

		/* Anything left in the extended descriptor is a second payload. */
		if (ex_descriptor != 0U)
			split = 1;
	}

	/* Reserves the instruction; a poisoned or full buffer takes nothing. */
	inst = i915_eu_reserve(buffer);
	if (inst == NULL)
		return;

	/* A render-target write is the conditional form of the message, a second payload the split form. */
	if (conditional != 0 && split != 0) {
		opcode = I915_EU_OPCODE_SENDSC;
	} else if (conditional != 0) {
		opcode = I915_EU_OPCODE_SENDC;
	} else if (split != 0) {
		opcode = I915_EU_OPCODE_SENDS;
	} else {
		opcode = I915_EU_OPCODE_SEND;
	}

	/* The message that ends the thread is its own scoreboard class. */
	if (end_of_thread != 0) {
		order = I915_EU_END_OF_THREAD;
		eot = 1U;
	} else {
		order = I915_EU_OUT_OF_ORDER;
		eot = 0U;
	}

	/* Encodes the control fields, the channels and the shared function. */
	i915_eu_common(buffer, inst, opcode, order);
	i915_eu_scope(buffer, inst, scope);
	i915_eu_field(inst, format->sfid, sfid);

	/* A masked message goes only to the channels whose bit of the flag is set. */
	if (predicated != 0) {
		i915_eu_flag(buffer, inst, flag);
		i915_eu_field(inst, format->pred_control, EU_PREDICATE_NORMAL);
	}

	/* Encodes the operands and the descriptors in the version's form. */
	if (format->has_swsb != 0) {
		i915_eu_send_gen12(inst, dst, src0, src1, descriptor, ex_descriptor);
	} else {
		i915_eu_send_gen9(buffer, inst, scope, dst, src0, src1, descriptor, ex_descriptor);
	}

	/* The end-of-thread bit goes last: on Gen9 it is the top bit of the descriptor's word. */
	i915_eu_bit(inst, format->eot_bit, eot);

	/* The message that ends the thread needs nothing after it. */
	if (end_of_thread != 0)
		return;

	/*
	 * A message with a destination is waited for until it has written it;
	 * one without, until it has read its sources.
	 */
	if (dst.file == EU_FILE_GRF) {
		swsb = EU_SWSB_SYNC_DST(I915_EU_TOKEN);
	} else {
		swsb = EU_SWSB_SYNC_SRC(I915_EU_TOKEN);
	}

	/* Waits until the message is done with its registers (nothing on Gen9). */
	i915_eu_sync(buffer, swsb);
}

/*
 * Encodes a Gen12 SEND's operands -- a file bit and a register number,
 * nothing else -- and scatters its two descriptors as
 * intel/eu-encoding-gen12.h says.
 */
static void
i915_eu_send_gen12(
	uint32_t *inst,
	struct i915_eu_reg dst,
	struct i915_eu_reg src0,
	struct i915_eu_reg src1,
	uint32_t descriptor,
	uint32_t ex_descriptor)
{
	/* Encodes the operands: a file bit and a register number, nothing else. */
	i915_eu_bit(inst, EU_DST_REG_FILE_BIT, i915_eu_file_bit(dst));
	i915_eu_set(inst, EU_DST_REG_NR_HI, EU_DST_REG_NR_LO, dst.nr);
	i915_eu_bit(inst, EU_SRC0_REG_FILE_BIT, i915_eu_file_bit(src0));
	i915_eu_set(inst, EU_SRC0_REG_NR_HI, EU_SRC0_REG_NR_LO, src0.nr);
	i915_eu_bit(inst, EU_SRC1_REG_FILE_BIT, i915_eu_file_bit(src1));
	i915_eu_set(inst, EU_SRC1_REG_NR_HI, EU_SRC1_REG_NR_LO, src1.nr);

	/* Scatters the message descriptor as eu-encoding-gen12.h says. */
	i915_eu_set(inst, 123U, 122U, descriptor >> 30);
	i915_eu_set(inst, 71U, 67U, descriptor >> 25);
	i915_eu_set(inst, 55U, 51U, descriptor >> 20);
	i915_eu_set(inst, 121U, 113U, descriptor >> 11);
	i915_eu_set(inst, 91U, 81U, descriptor);

	/* Scatters the extended descriptor the same way. */
	i915_eu_set(inst, 127U, 124U, ex_descriptor >> 28);
	i915_eu_set(inst, 97U, 96U, ex_descriptor >> 26);
	i915_eu_set(inst, 65U, 64U, ex_descriptor >> 24);
	i915_eu_set(inst, 47U, 35U, ex_descriptor >> 11);
	i915_eu_set(inst, 103U, 99U, ex_descriptor >> 6);
}

/*
 * Encodes a Gen9 SEND's operands and descriptors: SEND with the descriptor
 * as source 1's immediate when the extended descriptor is zero, SENDS with
 * the second payload in source 1 otherwise.  `scope` is the instruction's,
 * as i915_eu_scope() set it.
 *
 * Procedure referenced: Mesa 25.0.7 src/intel/compiler/brw_eu_emit.c
 * (brw_send_indirect_message, brw_send_indirect_split_message).
 */
static void
i915_eu_send_gen9(
	struct i915_eu_buf *buffer,
	uint32_t *inst,
	int scope,
	struct i915_eu_reg dst,
	struct i915_eu_reg src0,
	struct i915_eu_reg src1,
	uint32_t descriptor,
	uint32_t ex_descriptor)
{
	/*
	 * The single-payload form is an ordinary two-source instruction: the
	 * reply as UW words, the payload as UD words (the region does not
	 * matter to the message), the descriptor as a UD immediate.
	 */
	if (ex_descriptor == 0U) {
		dst.type = EU_TYPE_UW;
		src0.type = EU_TYPE_UD;

		/* A SIMD1 message reads its payload as one dword: no region may be wider than the execution. */
		if (scope == I915_EU_SCOPE_SCALAR) {
			src0.vstride = EU_VSTRIDE_0;
			src0.width = EU_WIDTH_1;
			src0.hstride = EU_HSTRIDE_0;
		}

		/* Encodes the destination, the payload and the message descriptor. */
		i915_eu_dst(buffer, inst, dst);
		i915_eu_src0(buffer, inst, src0);
		i915_eu_src1(buffer, inst, drv_i915_eu_imm_ud(descriptor));

		/*
		 * The extended descriptor, zero, overlays source 0's region and
		 * source 1's type: written last, so the message sees no stray
		 * extended bits.
		 *
		 * Procedure referenced: Mesa 25.0.7
		 * src/intel/compiler/brw_eu_emit.c (brw_set_desc_ex).
		 */
		i915_eu_set(inst, GEN9_EU_SEND_EX_DESC_31_28_HI, GEN9_EU_SEND_EX_DESC_31_28_LO, 0U);
		i915_eu_set(inst, GEN9_EU_SEND_EX_DESC_27_24_HI, GEN9_EU_SEND_EX_DESC_27_24_LO, 0U);
		i915_eu_set(inst, GEN9_EU_SEND_EX_DESC_23_20_HI, GEN9_EU_SEND_EX_DESC_23_20_LO, 0U);
		i915_eu_set(inst, GEN9_EU_SEND_EX_DESC_19_16_HI, GEN9_EU_SEND_EX_DESC_19_16_LO, 0U);
		return;
	}

	/* The split form names three registers by number, with a one-bit file for the reply and the second payload. */
	i915_eu_bit(inst, GEN9_EU_SENDS_DST_FILE_BIT, i915_eu_file_bit(dst));
	i915_eu_field(inst, buffer->format->dst.reg_nr, dst.nr);
	i915_eu_field(inst, buffer->format->src0.reg_nr, src0.nr);
	i915_eu_bit(inst, GEN9_EU_SENDS_SRC1_FILE_BIT, i915_eu_file_bit(src1));
	i915_eu_set(inst, GEN9_EU_SENDS_SRC1_REG_NR_HI, GEN9_EU_SENDS_SRC1_REG_NR_LO, src1.nr);

	/* The descriptor, and the extended descriptor's high half and second payload length. */
	i915_eu_set(inst, GEN9_EU_SEND_DESC_HI, GEN9_EU_SEND_DESC_LO, descriptor);
	i915_eu_set(inst,
		    GEN9_EU_SENDS_EX_DESC_HIGH_HI,
		    GEN9_EU_SENDS_EX_DESC_HIGH_LO,
		    ex_descriptor >> GEN9_EU_SENDS_EX_DESC_HIGH_SHIFT);
	i915_eu_set(inst,
		    GEN9_EU_SENDS_EX_MLEN_HI,
		    GEN9_EU_SENDS_EX_MLEN_LO,
		    (ex_descriptor >> GEN9_EU_SENDS_EX_MLEN_SHIFT) & GEN9_EU_SENDS_EX_MLEN_MASK);
}

/* Names a general register operand: eight channels of `type`. */
static struct i915_eu_reg
i915_eu_grf_typed(
	uint32_t nr,
	uint32_t type)
{
	struct i915_eu_reg reg;

	/* Names the register and the type its channels are read as. */
	kern_memset(&reg, 0, sizeof(reg));
	reg.file = EU_FILE_GRF;
	reg.nr = nr;
	reg.type = type;

	/* The ordinary SIMD8 region <8;8,1>: eight consecutive channels. */
	reg.vstride = EU_VSTRIDE_8;
	reg.width = EU_WIDTH_8;
	reg.hstride = EU_HSTRIDE_1;

	/* Succeeded: the operand reads the whole register. */
	return reg;
}

/* Reserves and zeroes one instruction, returning its four words. */
static uint32_t *
i915_eu_reserve(
	struct i915_eu_buf *buffer)
{
	uint32_t *grown;
	uint32_t *inst;
	size_t capacity;

	/* A prior error stops further encoding. */
	if (buffer->error != 0)
		return NULL;

	/* The buffer doubles geometrically to amortize the growth. */
	if (buffer->count + GEN12_EU_DWORDS > buffer->capacity) {
		/* The first growth takes the first capacity; each later one doubles. */
		if (buffer->capacity == 0U) {
			capacity = I915_EU_FIRST_CAPACITY;
		} else {
			capacity = buffer->capacity * 2U;
		}

		/* Allocates the larger storage; a failure poisons the buffer. */
		grown = kern_calloc(capacity, sizeof(uint32_t));
		if (grown == NULL) {
			buffer->error = 1;
			return NULL;
		}

		/* Moves the instructions encoded so far into the larger storage. */
		if (buffer->words != NULL) {
			kern_memcpy(grown, buffer->words, buffer->count * sizeof(uint32_t));
			kern_free(buffer->words);
		}

		/* Publishes the larger storage. */
		buffer->words = grown;
		buffer->capacity = capacity;
	}

	/* The new instruction starts zeroed so only set fields carry bits. */
	inst = &buffer->words[buffer->count];
	kern_memset(inst, 0, GEN12_EU_DWORDS * sizeof(uint32_t));
	buffer->count += GEN12_EU_DWORDS;

	/* Succeeded: the caller fills the four words. */
	return inst;
}

/*
 * Sets the control fields every instruction carries, the scoreboard byte among
 * them (on a layout that has one); `order` is I915_EU_IN_ORDER,
 * I915_EU_OUT_OF_ORDER (a sync.nop follows) or I915_EU_END_OF_THREAD.  An
 * opcode the layout does not have poisons the buffer.
 */
static void
i915_eu_common(
	struct i915_eu_buf *buffer,
	uint32_t *inst,
	enum i915_eu_opcode opcode,
	int order)
{
	uint32_t hardware_opcode;
	uint32_t regdist;
	uint32_t swsb;

	/* The layout's value of the opcode; an instruction it has not got is refused. */
	hardware_opcode = buffer->format->opcodes[opcode];
	if (hardware_opcode == I915_EU_NO_OPCODE)
		buffer->error = 1;

	/* The first in-order instruction has nothing before it to wait for. */
	if (buffer->in_order != 0U) {
		regdist = 1U;
	} else {
		regdist = 0U;
	}

	/* Encodes the opcode and the SIMD8 execution size. */
	i915_eu_field(inst, buffer->format->opcode, hardware_opcode);
	i915_eu_field(inst, buffer->format->exec_size, EU_EXEC_SIZE_8);

	/*
	 * An out-of-order instruction also names itself with the token; with
	 * nothing to wait for, the byte says only that.
	 */
	if (order == I915_EU_OUT_OF_ORDER) {
		if (regdist != 0U) {
			swsb = EU_SWSB_REGDIST_SET(regdist, I915_EU_TOKEN);
		} else {
			swsb = I915_EU_SWSB_TOKEN_SET | I915_EU_TOKEN;
		}
	} else {
		swsb = EU_SWSB_REGDIST(regdist);
	}

	/* Encodes the scoreboard byte where the layout has one; Gen9's hardware keeps its own. */
	if (buffer->format->has_swsb != 0)
		i915_eu_field(inst, buffer->format->swsb, swsb);

	/*
	 * The in-order count is what the next instruction's register distance
	 * counts back over; only an in-order instruction moves it.
	 */
	if (order == I915_EU_IN_ORDER)
		buffer->in_order++;
}

/*
 * Narrows an instruction to the channels `scope` names: the SIMD8 dispatch
 * under the execution mask (what i915_eu_common() encoded), all eight
 * channels outside it, or the first channel outside it.
 */
static void
i915_eu_scope(
	const struct i915_eu_buf *buffer,
	uint32_t *inst,
	int scope)
{
	/* The masked SIMD8 form is what the common fields already say. */
	if (scope == I915_EU_SCOPE_MASKED)
		return;

	/* A scalar instruction runs one channel, a sixteen-word one sixteen. */
	if (scope == I915_EU_SCOPE_SCALAR)
		i915_eu_field(inst, buffer->format->exec_size, EU_EXEC_SIZE_1);
	if (scope == I915_EU_SCOPE_SIXTEEN)
		i915_eu_field(inst, buffer->format->exec_size, EU_EXEC_SIZE_16);
	if (scope == I915_EU_SCOPE_FOUR)
		i915_eu_field(inst, buffer->format->exec_size, EU_EXEC_SIZE_4);

	/* Both other forms ignore the execution mask. */
	i915_eu_bit(inst, buffer->format->no_mask_bit, 1U);
}

/* Names the flag subregister an instruction's conditional modifier writes or its predicate reads. */
static void
i915_eu_flag(
	const struct i915_eu_buf *buffer,
	uint32_t *inst,
	enum i915_eu_flag flag)
{
	/* f0.0, f0.1, f1.0, f1.1: the register is the high bit, the subregister the low one. */
	i915_eu_bit(inst, buffer->format->flag_reg_bit, (uint32_t)flag >> 1);
	i915_eu_bit(inst, buffer->format->flag_subreg_bit, (uint32_t)flag & 1U);
}

/*
 * Encodes a sync.nop: the front end waits here until the named dependency is
 * resolved.  Gen9's hardware scoreboard waits by itself, so nothing is
 * encoded there.
 */
static void
i915_eu_sync(
	struct i915_eu_buf *buffer,
	uint32_t swsb)
{
	uint32_t *inst;

	/* A layout without the scoreboard byte needs no sync.nop. */
	if (buffer->format->has_swsb == 0)
		return;

	/* Reserves the instruction; a poisoned or full buffer takes nothing. */
	inst = i915_eu_reserve(buffer);
	if (inst == NULL)
		return;

	/* Encodes a SIMD1 sync with the dependency it waits on, outside the channel mask. */
	i915_eu_field(inst, buffer->format->opcode, buffer->format->opcodes[I915_EU_OPCODE_SYNC]);
	i915_eu_field(inst, buffer->format->swsb, swsb);
	i915_eu_field(inst, buffer->format->exec_size, EU_EXEC_SIZE_1);
	i915_eu_bit(inst, buffer->format->no_mask_bit, 1U);
}

/*
 * Encodes Gen9's barrier wait: a SIMD1 WAIT outside the channel mask on the
 * notification register n0, which the gateway signals once every thread of
 * the group has sent its barrier message.
 *
 * Procedure referenced: Mesa 25.0.7 src/intel/compiler/brw_eu_emit.c
 * (brw_WAIT).
 */
static void
i915_eu_wait_notification(
	struct i915_eu_buf *buffer)
{
	struct i915_eu_reg notification;
	uint32_t *inst;

	/* n0 as one unsigned dword, read and written once. */
	kern_memset(&notification, 0, sizeof(notification));
	notification.file = EU_FILE_ARF;
	notification.nr = GEN9_EU_ARF_NOTIFICATION;
	notification.type = EU_TYPE_UD;
	notification.vstride = EU_VSTRIDE_0;
	notification.width = EU_WIDTH_1;
	notification.hstride = EU_HSTRIDE_0;

	/* Reserves the instruction; a poisoned or full buffer takes nothing. */
	inst = i915_eu_reserve(buffer);
	if (inst == NULL)
		return;

	/* Encodes the WAIT on one channel, outside the mask, with n0 as its destination and first source. */
	i915_eu_common(buffer, inst, I915_EU_OPCODE_WAIT, I915_EU_IN_ORDER);
	i915_eu_scope(buffer, inst, I915_EU_SCOPE_SCALAR);
	i915_eu_dst(buffer, inst, notification);
	i915_eu_src0(buffer, inst, notification);
	i915_eu_src1(buffer, inst, drv_i915_eu_null());
}

/*
 * Marks a branch's targets as immediates: the JIP alone (`both` zero, a
 * WHILE) or the JIP and the UIP (an IF).  The caller writes the targets
 * afterwards.
 *
 * Gen12 has a bit per source for it.  Gen9 has an immediate signed-integer
 * zero as source 0, which also sets source 1's file and type; an IF's UIP,
 * cleared here, then overwrites those.
 *
 * Procedure referenced: Mesa 25.0.7 src/intel/compiler/brw_eu_emit.c (brw_IF,
 * brw_WHILE, before Gen12).
 */
static void
i915_eu_branch_target(
	struct i915_eu_buf *buffer,
	uint32_t *inst,
	int both)
{
	/* Gen9: the immediate source 0, and for an IF the cleared UIP. */
	if (buffer->format->has_swsb == 0) {
		i915_eu_src0(buffer, inst, drv_i915_eu_imm_d(0U));

		/* An IF's UIP overwrites what the immediate said of source 1. */
		if (both != 0)
			i915_eu_field(inst, buffer->format->uip, 0U);
		return;
	}

	/* Gen12: the JIP's source is an immediate. */
	i915_eu_bit(inst, EU_SRC0_IS_IMM_BIT, 1U);

	/* So is an IF's UIP source. */
	if (both != 0)
		i915_eu_bit(inst, EU_SRC1_IS_IMM_BIT, 1U);
}

/* Encodes a destination register. */
static void
i915_eu_dst(
	struct i915_eu_buf *buffer,
	uint32_t *inst,
	struct i915_eu_reg reg)
{
	const struct i915_eu_operand_layout *layout;
	uint32_t stride;
	uint32_t type;

	/* Where the destination's parts go, and the type it is written as. */
	layout = &buffer->format->dst;
	type = i915_eu_hw_type(buffer, reg.type, 0);

	/*
	 * A destination is written with horizontal stride one, or two when it
	 * asks for it: the 16-bit halves of 32-bit channels, as a packed half
	 * float is written.
	 */
	stride = EU_HSTRIDE_1;
	if (reg.hstride == EU_HSTRIDE_2)
		stride = EU_HSTRIDE_2;

	/* Encodes the file, the type, the stride and the register. */
	i915_eu_field(inst, layout->file, i915_eu_file_bit(reg));
	i915_eu_field(inst, layout->type, type);
	i915_eu_field(inst, layout->hstride, stride);
	i915_eu_field(inst, layout->subreg, reg.subnr);
	i915_eu_field(inst, layout->reg_nr, reg.nr);
}

/*
 * Encodes source zero, whether a register or an immediate.
 *
 * On Gen9 an immediate source 0 also says source 1 is the architecture file
 * read as the immediate's type: the two type fields describe the one 32-bit
 * value.
 *
 * Values referenced: Mesa 25.0.7 src/intel/compiler/brw_eu_emit.c
 * (brw_set_src0, before Gen12).
 */
static void
i915_eu_src0(
	struct i915_eu_buf *buffer,
	uint32_t *inst,
	struct i915_eu_reg reg)
{
	/* Encodes the source in source 0's fields. */
	i915_eu_source(buffer, inst, &buffer->format->src0, reg);

	/* Gen12 says an immediate with its own bit and needs nothing more. */
	if (buffer->format->has_swsb != 0)
		return;

	/* Gen9: an immediate's source 1 is the architecture file of the same type. */
	if (reg.file == EU_FILE_IMM) {
		i915_eu_field(inst, buffer->format->src1.file, GEN9_EU_FILE_ARF);
		i915_eu_field(inst, buffer->format->src1.type, i915_eu_hw_type(buffer, reg.type, 1));
	}
}

/* Encodes source one, whether a register or an immediate. */
static void
i915_eu_src1(
	struct i915_eu_buf *buffer,
	uint32_t *inst,
	struct i915_eu_reg reg)
{
	/* Encodes the source in source 1's fields. */
	i915_eu_source(buffer, inst, &buffer->format->src1, reg);
}

/*
 * Encodes one source, a register or an immediate, in the fields `layout`
 * names.
 */
static void
i915_eu_source(
	struct i915_eu_buf *buffer,
	uint32_t *inst,
	const struct i915_eu_operand_layout *layout,
	struct i915_eu_reg reg)
{
	int immediate;

	/* An immediate's type has its own encoding on Gen9. */
	immediate = 0;
	if (reg.file == EU_FILE_IMM)
		immediate = 1;

	/* Encodes the type the source is read as. */
	i915_eu_field(inst, layout->type, i915_eu_hw_type(buffer, reg.type, immediate));

	/*
	 * An immediate says so -- with its own bit on Gen12, with the file
	 * field on Gen9 -- and carries its value in the last word.
	 */
	if (immediate != 0) {
		if (layout->is_imm_bit != I915_EU_NO_BIT) {
			i915_eu_bit(inst, layout->is_imm_bit, 1U);
		} else {
			i915_eu_field(inst, layout->file, buffer->format->file_imm);
		}

		/* The value rides in the last word. */
		i915_eu_field(inst, buffer->format->imm32, reg.immediate);

		return;
	}

	/* Encodes the register, its region and its negate modifier. */
	i915_eu_field(inst, layout->file, i915_eu_file_bit(reg));
	i915_eu_field(inst, layout->reg_nr, reg.nr);
	i915_eu_field(inst, layout->subreg, reg.subnr);
	i915_eu_field(inst, layout->hstride, reg.hstride);
	i915_eu_field(inst, layout->width, reg.width);
	i915_eu_field(inst, layout->vstride, reg.vstride);
	i915_eu_bit(inst, layout->negate_bit, reg.negate & 1U);
	i915_eu_bit(inst, layout->abs_bit, reg.absolute & 1U);
}

/*
 * Returns the hardware encoding of an operand type named in the Gen12
 * encoding, for a register or (`immediate` nonzero) an immediate operand.
 * A type the layout cannot say poisons the buffer.
 */
static uint32_t
i915_eu_hw_type(
	struct i915_eu_buf *buffer,
	uint32_t type,
	int immediate)
{
	uint32_t encoded;

	/* A name outside the four-bit encoding is no type at all. */
	if (type >= I915_EU_TYPE_COUNT) {
		buffer->error = 1;
		return 0U;
	}

	/* Looks the type up in the table of its kind of operand. */
	if (immediate != 0) {
		encoded = buffer->format->immediate_types[type];
	} else {
		encoded = buffer->format->register_types[type];
	}

	/* A type the version cannot encode is refused. */
	if (encoded == I915_EU_NO_TYPE) {
		buffer->error = 1;
		return 0U;
	}

	/* Succeeded: the type as the version's hardware spells it. */
	return encoded;
}

/* Returns the hardware file bit of an operand: one for a general register, zero otherwise. */
static uint32_t
i915_eu_file_bit(
	struct i915_eu_reg reg)
{
	/* The hardware has one bit: general register or not. */
	if (reg.file == EU_FILE_GRF)
		return 1U;

	/* Anything else (the null register) is the architecture file. */
	return 0U;
}

/* Writes value into one field of the instruction. */
static void
i915_eu_field(
	uint32_t *inst,
	struct i915_eu_field field,
	uint32_t value)
{
	/* The field's bits are its range in the instruction. */
	i915_eu_set(inst, field.high, field.low, value);
}

/* Writes value into the inclusive bit range [high:low] of the instruction. */
static void
i915_eu_set(
	uint32_t *inst,
	unsigned high,
	unsigned low,
	uint32_t value)
{
	unsigned position;

	/* Each bit of the value takes its place from the low end upward. */
	for (position = low; position <= high; position++)
		i915_eu_bit(inst, position, (value >> (position - low)) & 1U);
}

/* Sets or clears one bit of the 128-bit instruction. */
static void
i915_eu_bit(
	uint32_t *inst,
	unsigned position,
	uint32_t value)
{
	uint32_t mask;

	/* The instruction is four little-endian words; the bit selects one. */
	mask = 1U << (position % 32U);

	/* Sets or clears the bit as the value's low bit says. */
	if ((value & 1U) != 0U) {
		inst[position / 32U] |= mask;
	} else {
		inst[position / 32U] &= ~mask;
	}
}
