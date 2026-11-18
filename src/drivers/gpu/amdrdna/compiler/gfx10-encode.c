/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The writer of the amdrdna shader compiler's machine instructions
 * (gfx10-encode.h): each record becomes one to three 32-bit words, and
 * each says which registers it reads and writes, so that the wait pass
 * can put the waits and the hazard breaks between them.
 */

#include "gfx10-encode.h"

#include <stddef.h>
#include <stdint.h>

/*
 * The places of the fields in the first word of the scalar forms: the
 * opcode of the two-source form (29:23), of the constant form (27:23), of
 * the one-source form (15:8) and of the comparison and control forms
 * (22:16); the destination SGPR (22:16); the second source (15:8); the
 * first source (7:0).
 *
 * Values referenced: Mesa 25.0.7 src/amd/compiler/aco_assembler.cpp
 * (emit_sop2_instruction, emit_sopk_instruction, emit_sop1_instruction,
 * emit_sopc_instruction, emit_sopp_instruction).
 */
#define GFX10_SCALAR_OPCODE_HIGH	23U
#define GFX10_SCALAR_OPCODE_ONE		8U
#define GFX10_SCALAR_OPCODE_LOW		16U
#define GFX10_SCALAR_DST		16U
#define GFX10_SCALAR_SRC1		8U

/*
 * The places of the fields of a scalar load: the opcode (25:18), the
 * coherent bit (16), the first SGPR loaded (12:6) and the base SGPR pair's
 * number halved (5:0) in the first word; the SGPR offset (31:25, NULL for
 * none) and the byte offset (20:0) in the second.
 *
 * Values referenced: Mesa 25.0.7 src/amd/compiler/aco_assembler.cpp
 * (emit_smem_instruction, the GFX10 case).
 */
#define GFX10_LOAD_OPCODE		18U
#define GFX10_LOAD_COHERENT		16U
#define GFX10_LOAD_DATA			6U
#define GFX10_LOAD_SOFFSET		25U
#define GFX10_LOAD_OFFSET_MASK		0x1fffffU

/*
 * The places of the fields of the short vector forms: the two-source
 * form's opcode (30:25), the one-source form's (16:9), the comparison's
 * (24:17); the destination VGPR (24:17); the second source, a VGPR (16:9);
 * the first source, any operand code (8:0).
 *
 * Values referenced: Mesa 25.0.7 src/amd/compiler/aco_assembler.cpp
 * (emit_vop2_instruction, emit_vop1_instruction, emit_vopc_instruction).
 */
#define GFX10_VECTOR_TWO_OPCODE		25U
#define GFX10_VECTOR_ONE_OPCODE		9U
#define GFX10_VECTOR_COMPARE_OPCODE	17U
#define GFX10_VECTOR_DST		17U
#define GFX10_VECTOR_SRC1		9U

/*
 * The places of the fields of the wide vector form: the opcode (25:16),
 * the absolute value bits (10:8) and the destination (7:0) in the first
 * word; the negate bits (31:29) and the three sources of nine bits each
 * (8:0, 17:9, 26:18) in the second.
 *
 * Values referenced: Mesa 25.0.7 src/amd/compiler/aco_assembler.cpp
 * (emit_vop3_instruction, the GFX10 case).
 */
#define GFX10_WIDE_OPCODE		16U
#define GFX10_WIDE_ABS			8U
#define GFX10_WIDE_NEG			29U
#define GFX10_WIDE_SRC1			9U
#define GFX10_WIDE_SRC2			18U

/*
 * The places of the fields of an interpolation: the destination VGPR
 * (25:18), the opcode (17:16), the attribute (15:10), the channel (9:8)
 * and the barycentric VGPR or the parameter code (7:0).
 *
 * Values referenced: Mesa 25.0.7 src/amd/compiler/aco_assembler.cpp
 * (emit_vintrp_instruction, the GFX10 case).
 */
#define GFX10_INTERP_DST		18U
#define GFX10_INTERP_OPCODE		16U
#define GFX10_INTERP_ATTRIBUTE		10U
#define GFX10_INTERP_CHANNEL		8U

/*
 * The places of the fields of a shared memory access: the opcode (25:18)
 * and the byte offset (15:0) in the first word; the destination (31:24),
 * the second data (23:16), the first data (15:8) and the address (7:0)
 * VGPRs in the second.
 *
 * Values referenced: Mesa 25.0.7 src/amd/compiler/aco_assembler.cpp
 * (emit_ds_instruction, the GFX10 case).
 */
#define GFX10_SHARED_OPCODE		18U
#define GFX10_SHARED_OFFSET_MASK	0xffffU
#define GFX10_SHARED_DST		24U
#define GFX10_SHARED_DATA1		16U
#define GFX10_SHARED_DATA0		8U

/*
 * The places of the fields of a buffer access: the opcode (25:18), the
 * device coherent bit (15), the coherent bit (14), the index (13) and
 * offset (12) address bits and the byte offset (11:0) in the first word; the SGPR offset (31:24), the
 * descriptor's first SGPR divided by four (20:16), the data VGPR (15:8)
 * and the address VGPR (7:0) in the second.
 *
 * Values referenced: Mesa 25.0.7 src/amd/compiler/aco_assembler.cpp
 * (emit_mubuf_instruction, the GFX10 case).
 */
#define GFX10_BUFFER_OPCODE		18U
#define GFX10_BUFFER_DEVICE_COHERENT	15U
#define GFX10_BUFFER_COHERENT		14U
#define GFX10_BUFFER_INDEXED		13U
#define GFX10_BUFFER_OFFSETTED		12U
#define GFX10_BUFFER_OFFSET_MASK	0xfffU
#define GFX10_BUFFER_SOFFSET		24U
#define GFX10_BUFFER_DESCRIPTOR		16U
#define GFX10_BUFFER_DATA		8U

/*
 * The places of the fields of an image access: the opcode (24:18), the
 * coherent (13), unnormalized (12) and device coherent (7) bits, the
 * channel mask (11:8) and the dimension (5:3) in the first word; the sampler's (25:21) and the
 * image descriptor's (20:16) first SGPR divided by four, the data VGPR
 * (15:8) and the first address VGPR (7:0) in the second.
 *
 * Values referenced: Mesa 25.0.7 src/amd/compiler/aco_assembler.cpp
 * (emit_mimg_instruction, the GFX10 case).
 */
#define GFX10_IMAGE_OPCODE		18U
#define GFX10_IMAGE_OPCODE_MASK		0x7fU
#define GFX10_IMAGE_COHERENT		13U
#define GFX10_IMAGE_UNNORMALIZED	12U
#define GFX10_IMAGE_DEVICE_COHERENT	7U
#define GFX10_IMAGE_MASK		8U
#define GFX10_IMAGE_DIMENSION		3U
#define GFX10_IMAGE_DIMENSION_MASK	7U
#define GFX10_IMAGE_SAMPLER		21U
#define GFX10_IMAGE_DESCRIPTOR		16U
#define GFX10_IMAGE_DATA		8U

/*
 * The places of the fields of an export: the valid mask bit (12), the
 * last-export bit (11), the compressed bit (10), the target (9:4) and the
 * channel enable (3:0) in the first word; the four VGPRs, eight bits each
 * from bit 0, in the second.
 *
 * Values referenced: Mesa 25.0.7 src/amd/compiler/aco_assembler.cpp
 * (emit_exp_instruction, the GFX10 case).
 */
#define GFX10_EXPORT_VALID		12U
#define GFX10_EXPORT_LAST		11U
#define GFX10_EXPORT_COMPRESSED		10U
#define GFX10_EXPORT_TARGET		4U

/*
 * The bits of an 8-bit register field, the low bits of an operand code;
 * and of a 9-bit source field, the whole code.
 *
 * Values referenced: Mesa 25.0.7 src/amd/compiler/aco_assembler.cpp
 * (reg(): a register in 8 bits where a field holds VGPRs only; the 9-bit
 * sources of emit_vop3_instruction).
 */
#define GFX10_FIELD8_MASK		0xffU
#define GFX10_SOURCE9_MASK		0x1ffU

static unsigned encode_scalar(const struct amdrdna_gfx10_inst *inst, uint32_t *words);
static unsigned encode_scalar_load(const struct amdrdna_gfx10_inst *inst, uint32_t *words);
static unsigned encode_vector(const struct amdrdna_gfx10_inst *inst, uint32_t *words);
static unsigned encode_wide(const struct amdrdna_gfx10_inst *inst, uint32_t *words);
static unsigned encode_interpolate(const struct amdrdna_gfx10_inst *inst, uint32_t *words);
static unsigned encode_shared(const struct amdrdna_gfx10_inst *inst, uint32_t *words);
static unsigned encode_buffer(const struct amdrdna_gfx10_inst *inst, uint32_t *words);
static unsigned encode_image(const struct amdrdna_gfx10_inst *inst, uint32_t *words);
static unsigned encode_export(const struct amdrdna_gfx10_inst *inst, uint32_t *words);
static unsigned append_literal(const struct amdrdna_gfx10_inst *inst, unsigned source_count, uint32_t *words, unsigned count);
static unsigned vector_source_count(const struct amdrdna_gfx10_inst *inst);
static unsigned wide_opcode(const struct amdrdna_gfx10_inst *inst);
static int is_scalar_operand(unsigned code);
static void shared_shape(unsigned opcode, int *returns, unsigned *data_count);
static void buffer_shape(const struct amdrdna_gfx10_inst *inst, unsigned *result_count, unsigned *data_count);
static unsigned mask_channels(unsigned mask);
static void add_read(struct amdrdna_gfx10_access *access, unsigned code, unsigned count);
static void add_write(struct amdrdna_gfx10_access *access, unsigned code, unsigned count);

/*
 * Writes one instruction as words.
 *
 * Returns the words written to words[] (one to AMDRDNA_GFX10_MAX_WORDS),
 * or 0 for a label, which is no instruction, and for an instruction whose
 * operands no form can hold.
 */
unsigned
amdrdna_gfx10_encode(
	const struct amdrdna_gfx10_inst *inst,
	uint32_t *words)
{
	unsigned count;

	/* Picks the writer of the instruction's form. */
	switch (inst->kind) {
	case AMDRDNA_GFX10_KIND_SCALAR_TWO:
	case AMDRDNA_GFX10_KIND_SCALAR_CONSTANT:
	case AMDRDNA_GFX10_KIND_SCALAR_ONE:
	case AMDRDNA_GFX10_KIND_SCALAR_COMPARE:
	case AMDRDNA_GFX10_KIND_CONTROL:
	case AMDRDNA_GFX10_KIND_BRANCH:
		count = encode_scalar(inst, words);
		break;
	case AMDRDNA_GFX10_KIND_SCALAR_LOAD:
		count = encode_scalar_load(inst, words);
		break;
	case AMDRDNA_GFX10_KIND_VECTOR_ONE:
	case AMDRDNA_GFX10_KIND_VECTOR_TWO:
	case AMDRDNA_GFX10_KIND_VECTOR_COMPARE:
	case AMDRDNA_GFX10_KIND_VECTOR_WIDE:
		count = encode_vector(inst, words);
		break;
	case AMDRDNA_GFX10_KIND_INTERPOLATE:
		count = encode_interpolate(inst, words);
		break;
	case AMDRDNA_GFX10_KIND_SHARED:
		count = encode_shared(inst, words);
		break;
	case AMDRDNA_GFX10_KIND_BUFFER:
		count = encode_buffer(inst, words);
		break;
	case AMDRDNA_GFX10_KIND_IMAGE:
		count = encode_image(inst, words);
		break;
	case AMDRDNA_GFX10_KIND_EXPORT:
		count = encode_export(inst, words);
		break;
	default:
		count = 0U;
		break;
	}

	/* Reports the words written, none for a label or a refusal. */
	return count;
}

/*
 * Reports whether a vector instruction is written in the wide form.
 *
 * An operation with only the wide form always is; a short-form operation
 * is when it is asked to be, carries a modifier, reads a second source
 * that is not a VGPR, selects by a mask other than VCC_LO, or compares
 * into a mask other than VCC_LO.
 */
int
amdrdna_gfx10_is_wide(
	const struct amdrdna_gfx10_inst *inst)
{
	int second_is_vgpr;

	/* An operation that has only the wide form. */
	if (inst->kind == AMDRDNA_GFX10_KIND_VECTOR_WIDE)
		return 1;

	/* A scalar, memory or export instruction has no wide form. */
	if (inst->kind != AMDRDNA_GFX10_KIND_VECTOR_ONE &&
	    inst->kind != AMDRDNA_GFX10_KIND_VECTOR_TWO &&
	    inst->kind != AMDRDNA_GFX10_KIND_VECTOR_COMPARE)
		return 0;

	/* A read of another lane of the group of four has only the short forms. */
	if (inst->quad_read != 0U)
		return 0;

	/* The lowering asked for the wide form, or a modifier needs it. */
	if (inst->wide != 0U)
		return 1;
	if (inst->modifiers != 0U)
		return 1;

	/* A one-source operation fits the short form. */
	if (inst->kind == AMDRDNA_GFX10_KIND_VECTOR_ONE)
		return 0;

	/* The short two-source and comparison forms read their second source from a VGPR only. */
	second_is_vgpr = amdrdna_gfx10_is_vgpr(inst->src[1]);
	if (second_is_vgpr == 0)
		return 1;

	/* The short selection reads its condition from VCC_LO only. */
	if (inst->kind == AMDRDNA_GFX10_KIND_VECTOR_TWO &&
	    inst->opcode == AMDRDNA_GFX10_V_CNDMASK_B32 &&
	    inst->src[2] != AMDRDNA_GFX10_VCC_LO)
		return 1;

	/* The short comparison writes its mask to VCC_LO only. */
	if (inst->kind == AMDRDNA_GFX10_KIND_VECTOR_COMPARE &&
	    inst->dst != AMDRDNA_GFX10_VCC_LO)
		return 1;

	/* Everything fits the short form. */
	return 0;
}

/*
 * Describes the registers an instruction reads and writes.
 *
 * The lane mask every vector, memory and export instruction uses and the
 * condition a short selection, a short comparison or a conditional branch
 * uses are included.  The counter says by which wait the written registers
 * (and an export's read ones) become usable.
 */
void
amdrdna_gfx10_describe(
	const struct amdrdna_gfx10_inst *inst,
	struct amdrdna_gfx10_access *access)
{
	unsigned source_count;
	unsigned result_count;
	unsigned data_count;
	unsigned index;
	unsigned channels;
	int returns;

	/* Starts with no register used. */
	access->reads_used = 0U;
	access->writes_used = 0U;
	access->counter = AMDRDNA_GFX10_COUNTER_NONE;

	/* Reads the registers of the instruction's form. */
	switch (inst->kind) {
	case AMDRDNA_GFX10_KIND_SCALAR_TWO:
	case AMDRDNA_GFX10_KIND_SCALAR_COMPARE:
		add_read(access, inst->src[0], 1U);
		add_read(access, inst->src[1], 1U);
		if (inst->kind == AMDRDNA_GFX10_KIND_SCALAR_TWO)
			add_write(access, inst->dst, 1U);
		break;
	case AMDRDNA_GFX10_KIND_SCALAR_CONSTANT:
		/* The store counter wait names NULL and writes nothing. */
		if (inst->opcode == AMDRDNA_GFX10_S_MOVK_I32)
			add_write(access, inst->dst, 1U);
		break;
	case AMDRDNA_GFX10_KIND_SCALAR_ONE:
		add_read(access, inst->src[0], 1U);
		add_write(access, inst->dst, 1U);

		/* Narrowing the lanes reads and writes the lane mask too. */
		if (inst->opcode == AMDRDNA_GFX10_S_AND_SAVEEXEC_B32) {
			add_read(access, AMDRDNA_GFX10_EXEC_LO, 1U);
			add_write(access, AMDRDNA_GFX10_EXEC_LO, 1U);
		}

		break;
	case AMDRDNA_GFX10_KIND_CONTROL:
		/* A message carries its data in M0. */
		if (inst->opcode == AMDRDNA_GFX10_S_SENDMSG)
			add_read(access, AMDRDNA_GFX10_M0, 1U);
		break;
	case AMDRDNA_GFX10_KIND_LABEL:
		break;
	case AMDRDNA_GFX10_KIND_BRANCH:
		/* A conditional branch reads the condition or the lane mask it tests. */
		if (inst->opcode == AMDRDNA_GFX10_S_CBRANCH_VCCZ ||
		    inst->opcode == AMDRDNA_GFX10_S_CBRANCH_VCCNZ) {
			add_read(access, AMDRDNA_GFX10_VCC_LO, 1U);
		} else if (inst->opcode == AMDRDNA_GFX10_S_CBRANCH_EXECZ ||
			   inst->opcode == AMDRDNA_GFX10_S_CBRANCH_EXECNZ) {
			add_read(access, AMDRDNA_GFX10_EXEC_LO, 1U);
		}

		break;
	case AMDRDNA_GFX10_KIND_SCALAR_LOAD:
		/* A buffer load reads the four SGPRs of its descriptor, a load the pair of its address. */
		if (inst->opcode == AMDRDNA_GFX10_S_BUFFER_LOAD_DWORD ||
		    inst->opcode == AMDRDNA_GFX10_S_BUFFER_LOAD_DWORDX4) {
			add_read(access, inst->src[0], 4U);
		} else {
			add_read(access, inst->src[0], 2U);
		}

		/* The loaded dwords are written to the destination registers. */
		add_write(access, inst->dst, inst->count);
		access->counter = AMDRDNA_GFX10_COUNTER_SCALAR;
		break;
	case AMDRDNA_GFX10_KIND_VECTOR_ONE:
	case AMDRDNA_GFX10_KIND_VECTOR_TWO:
	case AMDRDNA_GFX10_KIND_VECTOR_COMPARE:
	case AMDRDNA_GFX10_KIND_VECTOR_WIDE:
		add_read(access, AMDRDNA_GFX10_EXEC_LO, 1U);

		/* Reads every source the operation takes; a selection's third is its condition. */
		source_count = vector_source_count(inst);
		for (index = 0U; index < source_count; index++)
			add_read(access, inst->src[index], 1U);

		/* The result goes to the destination register. */
		add_write(access, inst->dst, 1U);
		break;
	case AMDRDNA_GFX10_KIND_INTERPOLATE:
		add_read(access, AMDRDNA_GFX10_EXEC_LO, 1U);
		add_read(access, AMDRDNA_GFX10_M0, 1U);

		/* The flat move names a parameter, not a register. */
		if (inst->opcode != AMDRDNA_GFX10_V_INTERP_MOV_F32)
			add_read(access, inst->src[0], 1U);

		/* The second step adds to its destination. */
		if (inst->opcode == AMDRDNA_GFX10_V_INTERP_P2_F32)
			add_read(access, inst->dst, 1U);

		/* The result goes to the destination register. */
		add_write(access, inst->dst, 1U);
		break;
	case AMDRDNA_GFX10_KIND_SHARED:
		add_read(access, AMDRDNA_GFX10_EXEC_LO, 1U);
		add_read(access, inst->src[0], 1U);

		/* Reads the data the operation takes, and writes what it returns. */
		shared_shape(inst->opcode, &returns, &data_count);
		for (index = 0U; index < data_count; index++)
			add_read(access, inst->src[1U + index], 1U);
		if (returns != 0)
			add_write(access, inst->dst, 1U);

		/* A shared-memory access is counted on the scalar counter. */
		access->counter = AMDRDNA_GFX10_COUNTER_SCALAR;
		break;
	case AMDRDNA_GFX10_KIND_BUFFER:
		/* A cache invalidation names no register. */
		if (inst->opcode == AMDRDNA_GFX10_BUFFER_GL0_INV ||
		    inst->opcode == AMDRDNA_GFX10_BUFFER_GL1_INV)
			break;

		/* Reads the lane mask, the address registers and the descriptor. */
		add_read(access, AMDRDNA_GFX10_EXEC_LO, 1U);
		add_read(access, inst->src[0], 1U);
		add_read(access, inst->src[1], 4U);
		add_read(access, inst->src[2], 1U);

		/* Reads the data stored, and writes what is loaded or returned. */
		buffer_shape(inst, &result_count, &data_count);
		if (data_count != 0U)
			add_read(access, inst->src[3], data_count);
		if (result_count != 0U) {
			add_write(access, inst->dst, result_count);
			access->counter = AMDRDNA_GFX10_COUNTER_VECTOR_LOAD;
		} else {
			access->counter = AMDRDNA_GFX10_COUNTER_VECTOR_STORE;
		}

		break;
	case AMDRDNA_GFX10_KIND_IMAGE:
		add_read(access, AMDRDNA_GFX10_EXEC_LO, 1U);
		add_read(access, inst->src[0], inst->count);
		add_read(access, inst->src[1], 8U);

		/* A load has no sampler. */
		if (inst->src[2] != AMDRDNA_GFX10_NULL)
			add_read(access, inst->src[2], 4U);

		/*
		 * A store reads the channels it selects; an atomic reads them and
		 * returns the old value in the first when coherent.
		 */
		if (inst->opcode == AMDRDNA_GFX10_IMAGE_STORE) {
			add_read(access, inst->dst, mask_channels(inst->mask));
			access->counter = AMDRDNA_GFX10_COUNTER_VECTOR_STORE;
			break;
		}
		if (inst->opcode >= AMDRDNA_GFX10_IMAGE_ATOMIC_SWAP && inst->opcode <= AMDRDNA_GFX10_IMAGE_ATOMIC_XOR) {
			add_read(access, inst->dst, mask_channels(inst->mask));
			access->counter = AMDRDNA_GFX10_COUNTER_VECTOR_STORE;
			if ((inst->flags & AMDRDNA_GFX10_FLAG_GLOBALLY_COHERENT) != 0U) {
				add_write(access, inst->dst, mask_channels(inst->mask));
				access->counter = AMDRDNA_GFX10_COUNTER_VECTOR_LOAD;
			}
			break;
		}

		/* A gather writes four channels whatever its mask; any other access the channels it selects. */
		if (inst->opcode == AMDRDNA_GFX10_IMAGE_GATHER4_LZ ||
		    inst->opcode == AMDRDNA_GFX10_IMAGE_GATHER4_C_LZ ||
		    inst->opcode == AMDRDNA_GFX10_IMAGE_GATHER4_LZ_O ||
		    inst->opcode == AMDRDNA_GFX10_IMAGE_GATHER4_C_LZ_O) {
			add_write(access, inst->dst, 4U);
		} else {
			channels = mask_channels(inst->mask);
			add_write(access, inst->dst, channels);
		}

		/* An image access is counted with the vector loads. */
		access->counter = AMDRDNA_GFX10_COUNTER_VECTOR_LOAD;
		break;
	case AMDRDNA_GFX10_KIND_EXPORT:
		add_read(access, AMDRDNA_GFX10_EXEC_LO, 1U);

		/*
		 * Reads the VGPR of every enabled channel (a compressed export's
		 * first two), which stays read until the export counter lets it go.
		 */
		for (index = 0U; index < 4U; index++) {
			if ((inst->flags & AMDRDNA_GFX10_FLAG_COMPRESSED) != 0U && index >= 2U)
				break;
			if ((inst->mask & (1U << index)) != 0U)
				add_read(access, inst->src[index], 1U);
		}

		/* An export is counted on the export counter. */
		access->counter = AMDRDNA_GFX10_COUNTER_EXPORT;
		break;
	default:
		break;
	}
}

/*
 * Reports whether an operand code names a VGPR.
 */
int
amdrdna_gfx10_is_vgpr(
	unsigned code)
{
	/* VGPRs are the codes from 256 on. */
	if (code < AMDRDNA_GFX10_VGPR_FIRST)
		return 0;
	if (code >= AMDRDNA_GFX10_VGPR_FIRST + AMDRDNA_GFX10_VGPR_COUNT)
		return 0;

	/* A VGPR. */
	return 1;
}

/*
 * Reports whether an operand code names a register (an SGPR, a special scalar register or a VGPR).
 */
int
amdrdna_gfx10_is_register(
	unsigned code)
{
	int vgpr;

	/* The SGPRs and the special registers are the codes below the constants. */
	if (code < AMDRDNA_GFX10_INT_FIRST)
		return 1;

	/* The rest that are registers are the VGPRs. */
	vgpr = amdrdna_gfx10_is_vgpr(code);
	if (vgpr != 0)
		return 1;

	/* A constant or the literal. */
	return 0;
}

/* Writes a scalar ALU, constant, control or branch instruction and its literal. */
static unsigned
encode_scalar(
	const struct amdrdna_gfx10_inst *inst,
	uint32_t *words)
{
	uint32_t word;
	unsigned source_count;
	unsigned count;

	/* Builds the one word of the form. */
	word = 0U;
	source_count = 0U;
	switch (inst->kind) {
	case AMDRDNA_GFX10_KIND_SCALAR_TWO:
		word = AMDRDNA_GFX10_FORM_SCALAR_TWO;
		word |= (uint32_t)inst->opcode << GFX10_SCALAR_OPCODE_HIGH;
		word |= (uint32_t)inst->dst << GFX10_SCALAR_DST;
		word |= (uint32_t)inst->src[1] << GFX10_SCALAR_SRC1;
		word |= inst->src[0];
		source_count = 2U;
		break;
	case AMDRDNA_GFX10_KIND_SCALAR_CONSTANT:
		word = AMDRDNA_GFX10_FORM_SCALAR_CONSTANT;
		word |= (uint32_t)inst->opcode << GFX10_SCALAR_OPCODE_HIGH;
		word |= (uint32_t)inst->dst << GFX10_SCALAR_DST;
		word |= inst->immediate & 0xffffU;
		break;
	case AMDRDNA_GFX10_KIND_SCALAR_ONE:
		word = AMDRDNA_GFX10_FORM_SCALAR_ONE;
		word |= (uint32_t)inst->dst << GFX10_SCALAR_DST;
		word |= (uint32_t)inst->opcode << GFX10_SCALAR_OPCODE_ONE;
		word |= inst->src[0];
		source_count = 1U;
		break;
	case AMDRDNA_GFX10_KIND_SCALAR_COMPARE:
		word = AMDRDNA_GFX10_FORM_SCALAR_COMPARE;
		word |= (uint32_t)inst->opcode << GFX10_SCALAR_OPCODE_LOW;
		word |= (uint32_t)inst->src[1] << GFX10_SCALAR_SRC1;
		word |= inst->src[0];
		source_count = 2U;
		break;
	default:
		word = AMDRDNA_GFX10_FORM_CONTROL;
		word |= (uint32_t)inst->opcode << GFX10_SCALAR_OPCODE_LOW;
		word |= inst->immediate & 0xffffU;
		break;
	}

	/* The scalar fields hold codes below 256 only: a VGPR cannot be a scalar operand. */
	if (inst->dst >= AMDRDNA_GFX10_VGPR_FIRST)
		return 0U;
	if (inst->src[0] >= AMDRDNA_GFX10_VGPR_FIRST)
		return 0U;
	if (inst->src[1] >= AMDRDNA_GFX10_VGPR_FIRST)
		return 0U;

	/* Appends the literal a source names. */
	words[0] = word;
	count = append_literal(inst, source_count, words, 1U);

	/* Reports the word and the literal, if any. */
	return count;
}

/* Writes a scalar load. */
static unsigned
encode_scalar_load(
	const struct amdrdna_gfx10_inst *inst,
	uint32_t *words)
{
	uint32_t word;

	/* The base is an SGPR pair (or a descriptor's four), named by its even first SGPR. */
	if (inst->src[0] >= AMDRDNA_GFX10_SGPR_COUNT)
		return 0U;
	if ((inst->src[0] & 1U) != 0U)
		return 0U;
	if (inst->dst >= AMDRDNA_GFX10_SGPR_COUNT)
		return 0U;

	/* The first word: the form, the opcode, the SGPRs loaded and the base. */
	word = AMDRDNA_GFX10_FORM_SCALAR_LOAD;
	word |= (uint32_t)inst->opcode << GFX10_LOAD_OPCODE;
	if ((inst->flags & AMDRDNA_GFX10_FLAG_GLOBALLY_COHERENT) != 0U)
		word |= 1U << GFX10_LOAD_COHERENT;
	word |= (uint32_t)inst->dst << GFX10_LOAD_DATA;
	word |= (uint32_t)inst->src[0] >> 1;
	words[0] = word;

	/* The second word: no SGPR offset, and the byte offset. */
	word = (uint32_t)AMDRDNA_GFX10_NULL << GFX10_LOAD_SOFFSET;
	word |= inst->immediate & GFX10_LOAD_OFFSET_MASK;
	words[1] = word;

	/* Two words. */
	return 2U;
}

/* Writes a vector ALU instruction in its short form, or in the wide form when it needs it. */
static unsigned
encode_vector(
	const struct amdrdna_gfx10_inst *inst,
	uint32_t *words)
{
	int vgpr;
	uint32_t word;
	unsigned source_count;
	unsigned count;
	int wide;

	/* The wide form has a writer of its own. */
	wide = amdrdna_gfx10_is_wide(inst);
	if (wide != 0) {
		count = encode_wide(inst, words);
		return count;
	}

	/*
	 * The short forms write a VGPR, except the comparison, which writes
	 * VCC_LO, and the read of the first lane, which writes an SGPR.
	 */
	vgpr = amdrdna_gfx10_is_vgpr(inst->dst);
	if (inst->kind == AMDRDNA_GFX10_KIND_VECTOR_COMPARE) {
		/* The comparison's VCC_LO is implied. */
	} else if (inst->opcode == AMDRDNA_GFX10_V_READFIRSTLANE_B32 &&
		   inst->kind == AMDRDNA_GFX10_KIND_VECTOR_ONE) {
		/* The first lane's value goes to a scalar register. */
		if (inst->dst >= AMDRDNA_GFX10_INT_FIRST)
			return 0U;
	} else if (vgpr == 0) {
		/* Any other short form writes a VGPR. */
		return 0U;
	}

	/* Builds the one word of the form. */
	source_count = vector_source_count(inst);
	switch (inst->kind) {
	case AMDRDNA_GFX10_KIND_VECTOR_ONE:
		word = AMDRDNA_GFX10_FORM_VECTOR_ONE;
		word |= ((uint32_t)inst->dst & GFX10_FIELD8_MASK) << GFX10_VECTOR_DST;
		word |= (uint32_t)inst->opcode << GFX10_VECTOR_ONE_OPCODE;
		word |= inst->src[0];
		break;
	case AMDRDNA_GFX10_KIND_VECTOR_TWO:
		word = AMDRDNA_GFX10_FORM_VECTOR_TWO;
		word |= (uint32_t)inst->opcode << GFX10_VECTOR_TWO_OPCODE;
		word |= ((uint32_t)inst->dst & GFX10_FIELD8_MASK) << GFX10_VECTOR_DST;
		word |= ((uint32_t)inst->src[1] & GFX10_FIELD8_MASK) << GFX10_VECTOR_SRC1;
		word |= inst->src[0];

		/* The selection's third source is the VCC_LO the short form implies. */
		source_count = 2U;
		break;
	default:
		word = AMDRDNA_GFX10_FORM_VECTOR_COMPARE;
		word |= (uint32_t)inst->opcode << GFX10_VECTOR_COMPARE_OPCODE;
		word |= ((uint32_t)inst->src[1] & GFX10_FIELD8_MASK) << GFX10_VECTOR_SRC1;
		word |= inst->src[0];
		break;
	}

	/*
	 * A read of another lane of the group of four names the pattern word
	 * as its first source; that word follows with the VGPR, the pattern,
	 * zero from a lane that does not run, and every row and bank on.
	 */
	if (inst->quad_read != 0U) {
		if (inst->kind == AMDRDNA_GFX10_KIND_VECTOR_COMPARE)
			return 0U;
		vgpr = amdrdna_gfx10_is_vgpr(inst->src[0]);
		if (vgpr == 0)
			return 0U;
		vgpr = amdrdna_gfx10_is_vgpr(inst->src[1]);
		if (inst->kind == AMDRDNA_GFX10_KIND_VECTOR_TWO && vgpr == 0)
			return 0U;
		word &= ~(uint32_t)GFX10_SOURCE9_MASK;
		word |= AMDRDNA_GFX10_QUAD_SOURCE;
		words[0] = word;
		words[1] = (uint32_t)inst->src[0] & GFX10_FIELD8_MASK;
		words[1] |= (uint32_t)inst->quad_pattern << AMDRDNA_GFX10_QUAD_PATTERN_SHIFT;
		words[1] |= AMDRDNA_GFX10_QUAD_ZERO_IF_IDLE;
		words[1] |= AMDRDNA_GFX10_QUAD_ENABLES;
		return 2U;
	}

	/* Appends the literal the first source may name. */
	words[0] = word;
	count = append_literal(inst, source_count, words, 1U);

	/* Reports the word and the literal, if any. */
	return count;
}

/* Writes a vector ALU instruction in the wide form. */
static unsigned
encode_wide(
	const struct amdrdna_gfx10_inst *inst,
	uint32_t *words)
{
	int vgpr;
	uint32_t word;
	unsigned source_count;
	unsigned count;
	unsigned index;

	/* The destination field holds eight bits: a VGPR's low bits, or an SGPR for a comparison. */
	if (inst->kind == AMDRDNA_GFX10_KIND_VECTOR_COMPARE) {
		if (inst->dst >= AMDRDNA_GFX10_INT_FIRST)
			return 0U;
	} else {
		vgpr = amdrdna_gfx10_is_vgpr(inst->dst);
		if (vgpr == 0 && inst->opcode != AMDRDNA_GFX10_V_READFIRSTLANE_B32)
			return 0U;
	}

	/* The first word: the form, the opcode, the absolute value bits and the destination. */
	word = AMDRDNA_GFX10_FORM_VECTOR_WIDE;
	word |= (uint32_t)wide_opcode(inst) << GFX10_WIDE_OPCODE;
	word |= ((uint32_t)(inst->modifiers >> 3) & 7U) << GFX10_WIDE_ABS;
	word |= (uint32_t)inst->dst & GFX10_FIELD8_MASK;
	words[0] = word;

	/* The second word: the negate bits and the sources the operation takes; the others are 0. */
	source_count = vector_source_count(inst);
	word = ((uint32_t)inst->modifiers & 7U) << GFX10_WIDE_NEG;
	for (index = 0U; index < source_count; index++)
		word |= (uint32_t)inst->src[index] << (index * GFX10_WIDE_SRC1);
	words[1] = word;

	/* Appends the literal a source may name. */
	count = append_literal(inst, source_count, words, 2U);

	/* Reports the two words and the literal, if any. */
	return count;
}

/* Writes an interpolation. */
static unsigned
encode_interpolate(
	const struct amdrdna_gfx10_inst *inst,
	uint32_t *words)
{
	int vgpr;
	uint32_t word;

	/* The destination is a VGPR, and so is the barycentric source of the two steps. */
	vgpr = amdrdna_gfx10_is_vgpr(inst->dst);
	if (vgpr == 0)
		return 0U;
	if (inst->opcode != AMDRDNA_GFX10_V_INTERP_MOV_F32) {
		vgpr = amdrdna_gfx10_is_vgpr(inst->src[0]);
		if (vgpr == 0)
			return 0U;
	}

	/* The one word: the form, the destination, the opcode, the attribute, the channel and the source. */
	word = AMDRDNA_GFX10_FORM_INTERPOLATE;
	word |= ((uint32_t)inst->dst & GFX10_FIELD8_MASK) << GFX10_INTERP_DST;
	word |= (uint32_t)inst->opcode << GFX10_INTERP_OPCODE;
	word |= (uint32_t)inst->target << GFX10_INTERP_ATTRIBUTE;
	word |= (uint32_t)inst->channel << GFX10_INTERP_CHANNEL;
	word |= (uint32_t)inst->src[0] & GFX10_FIELD8_MASK;
	words[0] = word;

	/* One word. */
	return 1U;
}

/* Writes a shared memory access. */
static unsigned
encode_shared(
	const struct amdrdna_gfx10_inst *inst,
	uint32_t *words)
{
	int vgpr;
	uint32_t word;
	unsigned data_count;
	unsigned index;
	int returns;

	/* The address is a VGPR. */
	vgpr = amdrdna_gfx10_is_vgpr(inst->src[0]);
	if (vgpr == 0)
		return 0U;

	/* The first word: the form, the opcode and the byte offset. */
	word = AMDRDNA_GFX10_FORM_SHARED;
	word |= (uint32_t)inst->opcode << GFX10_SHARED_OPCODE;
	word |= inst->immediate & GFX10_SHARED_OFFSET_MASK;
	words[0] = word;

	/* The second word: the result, the data and the address VGPRs. */
	shared_shape(inst->opcode, &returns, &data_count);
	word = (uint32_t)inst->src[0] & GFX10_FIELD8_MASK;
	for (index = 0U; index < data_count; index++) {
		/* Every data source is a VGPR. */
		vgpr = amdrdna_gfx10_is_vgpr(inst->src[1U + index]);
		if (vgpr == 0)
			return 0U;

		/* Places the data source in its field of the second word. */
		word |= ((uint32_t)inst->src[1U + index] & GFX10_FIELD8_MASK) << (GFX10_SHARED_DATA0 + 8U * index);
	}

	/* An operation that returns a value writes a VGPR. */
	if (returns != 0) {
		vgpr = amdrdna_gfx10_is_vgpr(inst->dst);
		if (vgpr == 0)
			return 0U;

		/* Places the destination in its field of the second word. */
		word |= ((uint32_t)inst->dst & GFX10_FIELD8_MASK) << GFX10_SHARED_DST;
	}

	/* Stores the second word. */
	words[1] = word;

	/* Two words. */
	return 2U;
}

/* Writes a buffer access. */
static unsigned
encode_buffer(
	const struct amdrdna_gfx10_inst *inst,
	uint32_t *words)
{
	int vgpr;
	uint32_t word;
	unsigned result_count;
	unsigned data_count;
	unsigned data;
	int invalidates;

	/* A cache invalidation is the form and the opcode with every other field zero. */
	invalidates = 0;
	if (inst->opcode == AMDRDNA_GFX10_BUFFER_GL0_INV)
		invalidates = 1;
	if (inst->opcode == AMDRDNA_GFX10_BUFFER_GL1_INV)
		invalidates = 1;
	if (invalidates != 0) {
		words[0] = AMDRDNA_GFX10_FORM_BUFFER | ((uint32_t)inst->opcode << GFX10_BUFFER_OPCODE);
		words[1] = 0U;
		return 2U;
	}

	/* The descriptor is four SGPRs from a multiple of four, the address a VGPR. */
	if (inst->src[1] >= AMDRDNA_GFX10_SGPR_COUNT)
		return 0U;
	if ((inst->src[1] & 3U) != 0U)
		return 0U;
	vgpr = amdrdna_gfx10_is_vgpr(inst->src[0]);
	if (vgpr == 0)
		return 0U;

	/* The data VGPR is what a load or a returning atomic writes, or what a store reads. */
	buffer_shape(inst, &result_count, &data_count);
	data = inst->src[3];
	if (data_count == 0U)
		data = inst->dst;
	vgpr = amdrdna_gfx10_is_vgpr(data);
	if (vgpr == 0)
		return 0U;

	/* An atomic that returns its old value writes it over its data. */
	if (result_count != 0U && data_count != 0U && inst->dst != inst->src[3])
		return 0U;

	/* The first word: the form, the opcode, the bits and the byte offset. */
	word = AMDRDNA_GFX10_FORM_BUFFER;
	word |= (uint32_t)inst->opcode << GFX10_BUFFER_OPCODE;
	if ((inst->flags & AMDRDNA_GFX10_FLAG_GLOBALLY_COHERENT) != 0U)
		word |= 1U << GFX10_BUFFER_COHERENT;
	if ((inst->flags & AMDRDNA_GFX10_FLAG_DEVICE_COHERENT) != 0U)
		word |= 1U << GFX10_BUFFER_DEVICE_COHERENT;
	if ((inst->flags & AMDRDNA_GFX10_FLAG_INDEX_ADDRESS) != 0U)
		word |= 1U << GFX10_BUFFER_INDEXED;
	if ((inst->flags & AMDRDNA_GFX10_FLAG_OFFSET_ADDRESS) != 0U)
		word |= 1U << GFX10_BUFFER_OFFSETTED;
	word |= inst->immediate & GFX10_BUFFER_OFFSET_MASK;
	words[0] = word;

	/* The second word: the SGPR offset, the descriptor, the data and the address. */
	word = (uint32_t)inst->src[2] << GFX10_BUFFER_SOFFSET;
	word |= ((uint32_t)inst->src[1] >> 2) << GFX10_BUFFER_DESCRIPTOR;
	word |= (data & GFX10_FIELD8_MASK) << GFX10_BUFFER_DATA;
	word |= (uint32_t)inst->src[0] & GFX10_FIELD8_MASK;
	words[1] = word;

	/* Two words. */
	return 2U;
}

/* Writes an image access. */
static unsigned
encode_image(
	const struct amdrdna_gfx10_inst *inst,
	uint32_t *words)
{
	int vgpr;
	uint32_t word;

	/* The data and the address are VGPRs; the descriptor and the sampler start at a multiple of four. */
	vgpr = amdrdna_gfx10_is_vgpr(inst->dst);
	if (vgpr == 0)
		return 0U;
	vgpr = amdrdna_gfx10_is_vgpr(inst->src[0]);
	if (vgpr == 0)
		return 0U;
	if (inst->src[1] >= AMDRDNA_GFX10_SGPR_COUNT)
		return 0U;
	if ((inst->src[1] & 3U) != 0U)
		return 0U;
	if (inst->src[2] != AMDRDNA_GFX10_NULL) {
		if (inst->src[2] >= AMDRDNA_GFX10_SGPR_COUNT)
			return 0U;
		if ((inst->src[2] & 3U) != 0U)
			return 0U;
	}

	/* The first word: the form, the opcode, the bits, the channel mask and the dimension. */
	word = AMDRDNA_GFX10_FORM_IMAGE;
	word |= ((uint32_t)inst->opcode & GFX10_IMAGE_OPCODE_MASK) << GFX10_IMAGE_OPCODE;
	if ((inst->flags & AMDRDNA_GFX10_FLAG_GLOBALLY_COHERENT) != 0U)
		word |= 1U << GFX10_IMAGE_COHERENT;
	if ((inst->flags & AMDRDNA_GFX10_FLAG_UNNORMALIZED) != 0U)
		word |= 1U << GFX10_IMAGE_UNNORMALIZED;
	if ((inst->flags & AMDRDNA_GFX10_FLAG_DEVICE_COHERENT) != 0U)
		word |= 1U << GFX10_IMAGE_DEVICE_COHERENT;
	word |= ((uint32_t)inst->mask & 0xfU) << GFX10_IMAGE_MASK;
	word |= ((uint32_t)inst->dimension & GFX10_IMAGE_DIMENSION_MASK) << GFX10_IMAGE_DIMENSION;
	words[0] = word;

	/* The second word: the sampler (none for a load), the descriptor, the data and the address. */
	word = 0U;
	if (inst->src[2] != AMDRDNA_GFX10_NULL)
		word |= ((uint32_t)inst->src[2] >> 2) << GFX10_IMAGE_SAMPLER;
	word |= ((uint32_t)inst->src[1] >> 2) << GFX10_IMAGE_DESCRIPTOR;
	word |= ((uint32_t)inst->dst & GFX10_FIELD8_MASK) << GFX10_IMAGE_DATA;
	word |= (uint32_t)inst->src[0] & GFX10_FIELD8_MASK;
	words[1] = word;

	/* Two words. */
	return 2U;
}

/* Writes an export. */
static unsigned
encode_export(
	const struct amdrdna_gfx10_inst *inst,
	uint32_t *words)
{
	int vgpr;
	uint32_t word;
	unsigned index;

	/* The first word: the form, the bits, the target and the channel enable. */
	word = AMDRDNA_GFX10_FORM_EXPORT;
	if ((inst->flags & AMDRDNA_GFX10_FLAG_VALID_MASK) != 0U)
		word |= 1U << GFX10_EXPORT_VALID;
	if ((inst->flags & AMDRDNA_GFX10_FLAG_LAST) != 0U)
		word |= 1U << GFX10_EXPORT_LAST;
	if ((inst->flags & AMDRDNA_GFX10_FLAG_COMPRESSED) != 0U)
		word |= 1U << GFX10_EXPORT_COMPRESSED;
	word |= (uint32_t)inst->target << GFX10_EXPORT_TARGET;
	word |= (uint32_t)inst->mask & 0xfU;
	words[0] = word;

	/* The second word: the VGPR of every enabled channel (of a compressed export, the first two), 0 for the others. */
	word = 0U;
	for (index = 0U; index < 4U; index++) {
		/* A disabled channel names no register, nor do a compressed export's last two. */
		if ((inst->mask & (1U << index)) == 0U)
			continue;
		if ((inst->flags & AMDRDNA_GFX10_FLAG_COMPRESSED) != 0U && index >= 2U)
			continue;

		/* An enabled channel's source is a VGPR. */
		vgpr = amdrdna_gfx10_is_vgpr(inst->src[index]);
		if (vgpr == 0)
			return 0U;

		/* Places the channel's source in its field of the second word. */
		word |= ((uint32_t)inst->src[index] & GFX10_FIELD8_MASK) << (8U * index);
	}

	/* Stores the second word. */
	words[1] = word;

	/* Two words. */
	return 2U;
}

/* Appends the literal when one of the first source_count sources names it; returns the words then written. */
static unsigned
append_literal(
	const struct amdrdna_gfx10_inst *inst,
	unsigned source_count,
	uint32_t *words,
	unsigned count)
{
	unsigned index;

	/* One literal serves every source that names it. */
	for (index = 0U; index < source_count; index++) {
		if (inst->src[index] == AMDRDNA_GFX10_LITERAL) {
			words[count] = inst->literal;
			return count + 1U;
		}
	}

	/* No source names the literal. */
	return count;
}

/* Reports the sources a vector ALU operation reads. */
static unsigned
vector_source_count(
	const struct amdrdna_gfx10_inst *inst)
{
	/* Picks the count by the kind, and by the operation for the selection and the wide-only ones. */
	switch (inst->kind) {
	case AMDRDNA_GFX10_KIND_VECTOR_ONE:
		return 1U;
	case AMDRDNA_GFX10_KIND_VECTOR_TWO:
		/* The selection's condition is its third source. */
		if (inst->opcode == AMDRDNA_GFX10_V_CNDMASK_B32)
			return 3U;
		return 2U;
	case AMDRDNA_GFX10_KIND_VECTOR_COMPARE:
		return 2U;
	default:
		/* The multiply-adds and the cube operations take three sources, the others two. */
		if (inst->opcode == AMDRDNA_GFX10_V_FMA_F32)
			return 3U;
		if (inst->opcode == AMDRDNA_GFX10_V_MAD_U32_U24)
			return 3U;
		if (inst->opcode >= AMDRDNA_GFX10_V_CUBEID_F32 && inst->opcode <= AMDRDNA_GFX10_V_CUBEMA_F32)
			return 3U;
		return 2U;
	}
}

/* Reports the opcode a vector operation has in the wide form. */
static unsigned
wide_opcode(
	const struct amdrdna_gfx10_inst *inst)
{
	/* Moves a short-form opcode to its place among the wide ones. */
	switch (inst->kind) {
	case AMDRDNA_GFX10_KIND_VECTOR_ONE:
		return inst->opcode + AMDRDNA_GFX10_WIDE_FROM_ONE;
	case AMDRDNA_GFX10_KIND_VECTOR_TWO:
		return inst->opcode + AMDRDNA_GFX10_WIDE_FROM_TWO;
	case AMDRDNA_GFX10_KIND_VECTOR_COMPARE:
		return inst->opcode + AMDRDNA_GFX10_WIDE_FROM_COMPARE;
	default:
		return inst->opcode;
	}
}

/* Reports whether an operand code is a scalar register (an SGPR or a special one). */
static int
is_scalar_operand(
	unsigned code)
{
	/* The scalar registers are the codes below the constants. */
	if (code < AMDRDNA_GFX10_INT_FIRST)
		return 1;

	/* A constant, the literal or a VGPR. */
	return 0;
}

/* Says whether a shared memory operation returns a value and how many data VGPRs it reads. */
static void
shared_shape(
	unsigned opcode,
	int *returns,
	unsigned *data_count)
{
	/* Picks the shape by the operation. */
	switch (opcode) {
	case AMDRDNA_GFX10_DS_WRITE_B32:
		*returns = 0;
		*data_count = 1U;
		break;
	case AMDRDNA_GFX10_DS_READ_B32:
		*returns = 1;
		*data_count = 0U;
		break;
	case AMDRDNA_GFX10_DS_CMPST_RTN_B32:
		*returns = 1;
		*data_count = 2U;
		break;
	default:
		/* Every other operation used is an atomic that returns the old value. */
		*returns = 1;
		*data_count = 1U;
		break;
	}
}

/* Says how many VGPRs a buffer access writes and how many data VGPRs it reads. */
static void
buffer_shape(
	const struct amdrdna_gfx10_inst *inst,
	unsigned *result_count,
	unsigned *data_count)
{
	/* Picks the shape by the operation. */
	switch (inst->opcode) {
	case AMDRDNA_GFX10_BUFFER_LOAD_DWORD:
	case AMDRDNA_GFX10_BUFFER_LOAD_FORMAT_XYZW:
		*result_count = inst->count;
		*data_count = 0U;
		break;
	case AMDRDNA_GFX10_BUFFER_STORE_DWORD:
		*result_count = 0U;
		*data_count = 1U;
		break;
	case AMDRDNA_GFX10_BUFFER_STORE_FORMAT_XYZW:
		*result_count = 0U;
		*data_count = 4U;
		break;
	default:
		/* An atomic: the compare-and-swap reads a pair; it returns one value when coherent. */
		*data_count = 1U;
		if (inst->opcode == AMDRDNA_GFX10_BUFFER_ATOMIC_CMPSWAP)
			*data_count = 2U;

		/* Counts the values the atomic returns. */
		*result_count = 0U;
		if ((inst->flags & AMDRDNA_GFX10_FLAG_GLOBALLY_COHERENT) != 0U)
			*result_count = 1U;
		break;
	}
}

/* Reports the channels an image channel mask selects. */
static unsigned
mask_channels(
	unsigned mask)
{
	unsigned count;
	unsigned bit;

	/* Counts the set bits of the four. */
	count = 0U;
	for (bit = 0U; bit < 4U; bit++) {
		if ((mask & (1U << bit)) != 0U)
			count++;
	}

	/* Reports the count. */
	return count;
}

/* Records a read of count registers from code, when the code names a register. */
static void
add_read(
	struct amdrdna_gfx10_access *access,
	unsigned code,
	unsigned count)
{
	int is_register;

	/* A constant, the literal or NULL is no register. */
	is_register = amdrdna_gfx10_is_register(code);
	if (is_register == 0)
		return;
	if (code == AMDRDNA_GFX10_NULL)
		return;

	/* The list is long enough for every instruction's reads. */
	if (access->reads_used == AMDRDNA_GFX10_MAX_READS)
		return;

	/* Appends the range. */
	access->reads[access->reads_used].first = (uint16_t)code;
	access->reads[access->reads_used].count = (uint16_t)count;
	access->reads_used++;
}

/* Records a write of count registers from code, when the code names a register. */
static void
add_write(
	struct amdrdna_gfx10_access *access,
	unsigned code,
	unsigned count)
{
	int scalar;
	int vgpr;

	/* NULL and the constants are no register. */
	scalar = is_scalar_operand(code);
	vgpr = amdrdna_gfx10_is_vgpr(code);
	if (scalar == 0 && vgpr == 0)
		return;
	if (code == AMDRDNA_GFX10_NULL)
		return;

	/* The list is long enough for every instruction's writes. */
	if (access->writes_used == AMDRDNA_GFX10_MAX_WRITES)
		return;

	/* Appends the range. */
	access->writes[access->writes_used].first = (uint16_t)code;
	access->writes[access->writes_used].count = (uint16_t)count;
	access->writes_used++;
}
