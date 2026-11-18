/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The reading of an instruction's operands back from its four words, for
 * the reordering and the dependence marking (see schedule.h).
 *
 * The reading knows every instruction the lowering emits: the general
 * registers and predicates it reads and writes, its latency class, whether
 * it ends a block, and the fewest cycles before the next instruction may
 * issue.  Any other instruction is refused.
 */

#include "drivers/gpu/nvrtx/compiler/sm75/encode.h"
#include "drivers/gpu/nvrtx/compiler/sm75/schedule.h"

#include <kern/kcrt.h>

static int nvrtx_sm75_operands_alu(const struct nvrtx_sm75_word *word, uint32_t opcode, struct nvrtx_sm75_operands *operands);
static int nvrtx_sm75_operands_other(const struct nvrtx_sm75_word *word, uint32_t opcode, struct nvrtx_sm75_operands *operands);
static void nvrtx_sm75_texture_counts(const struct nvrtx_sm75_word *word, uint32_t opcode, uint32_t *first, uint32_t *second);
static void nvrtx_sm75_add_read(struct nvrtx_sm75_operands *operands, uint32_t reg, uint32_t read_class);
static void nvrtx_sm75_add_write(struct nvrtx_sm75_operands *operands, uint32_t reg);

/*
 * Reads an instruction's operands from its four words.
 *
 * Returns 0 with the operands, or 1 for an opcode the lowering does not
 * emit.  The guard predicate is read by every guarded instruction.
 */
int
nvrtx_sm75_operands(
	const uint32_t *words,
	struct nvrtx_sm75_operands *operands)
{
	struct nvrtx_sm75_word word;
	uint32_t opcode;
	int unknown;

	/* A copy of the four words the field readers take. */
	word.w[0] = words[0];
	word.w[1] = words[1];
	word.w[2] = words[2];
	word.w[3] = words[3];

	/* Nothing read or written yet, one cycle to the next instruction. */
	kern_memset(operands, 0, sizeof(*operands));
	operands->predicate_read = NVRTX_SM75_NO_PREDICATE;
	operands->predicate_write = NVRTX_SM75_NO_PREDICATE;
	operands->min_delay = 1U;

	/* The guard, unless it is PT. */
	operands->guard = (uint32_t)nvrtx_sm75_get(&word, NVRTX_SM75_GUARD_LOW, NVRTX_SM75_PRED_BITS);

	/* The opcodes of their own encodings, then the ALU ones by their 9 bits. */
	opcode = (uint32_t)nvrtx_sm75_get(&word, NVRTX_SM75_OPCODE_LOW, NVRTX_SM75_OPCODE_BITS);
	unknown = nvrtx_sm75_operands_other(&word, opcode, operands);
	if (unknown == 0)
		return 0;

	/* Any other is an ALU instruction or not known. */
	opcode = (uint32_t)nvrtx_sm75_get(&word, NVRTX_SM75_OPCODE_LOW, NVRTX_SM75_ALU_OPCODE_BITS);
	unknown = nvrtx_sm75_operands_alu(&word, opcode, operands);
	if (unknown != 0)
		return 1;

	/* Succeeded: an ALU instruction. */
	return 0;
}

/*
 * Reads the operands of an ALU instruction by its 9-bit opcode; nonzero for one
 * not known.
 *
 * Values referenced: Mesa 25.3.6
 * src/nouveau/compiler/nak/sm75_instr_latencies.rs, op_category and
 * needs_scoreboards (lines 49-203, 1187-1208), and
 * src/nouveau/compiler/nak/sm80_instr_latencies.rs, the same (lines 85-257,
 * 1433-1453).
 */
static int
nvrtx_sm75_operands_alu(
	const struct nvrtx_sm75_word *word,
	uint32_t opcode,
	struct nvrtx_sm75_operands *operands)
{
	uint32_t form;
	uint32_t dst;
	uint32_t src0;
	uint32_t src1;
	uint32_t src2;
	int has_src0;
	int has_src2;
	int writes_dst;

	/* The form, the destination and the three register slots. */
	form = (uint32_t)nvrtx_sm75_get(word, NVRTX_SM75_ALU_FORM_LOW, NVRTX_SM75_ALU_FORM_BITS);
	dst = (uint32_t)nvrtx_sm75_get(word, NVRTX_SM75_DST_LOW, NVRTX_SM75_REG_BITS);
	src0 = (uint32_t)nvrtx_sm75_get(word, NVRTX_SM75_SRC0_LOW, NVRTX_SM75_REG_BITS);
	src1 = (uint32_t)nvrtx_sm75_get(word, NVRTX_SM75_SRC1_LOW, NVRTX_SM75_REG_BITS);
	src2 = (uint32_t)nvrtx_sm75_get(word, NVRTX_SM75_SRC2_LOW, NVRTX_SM75_REG_BITS);

	/* The forms the lowering emits. */
	if (form < NVRTX_SM75_FORM_REG || form > NVRTX_SM75_FORM_CB)
		return 1;

	/* Which sources the operation has, whether it writes a register, its class. */
	has_src0 = 1;
	has_src2 = 1;
	writes_dst = 1;
	operands->class_of = NVRTX_SM75_CLASS_ALU;
	switch (opcode) {
	case NVRTX_SM75_OP_MOV:
	case NVRTX_SM75_OP_MUFU:
	case NVRTX_SM75_OP_I2F:
	case NVRTX_SM75_OP_F2I:
	case NVRTX_SM75_OP_FRND:
	case NVRTX_SM75_OP_F2F:
		/* One source, the second. */
		has_src0 = 0;
		has_src2 = 0;
		if (opcode != NVRTX_SM75_OP_MOV)
			operands->class_of = NVRTX_SM75_CLASS_DECOUPLED;
		break;
	case NVRTX_SM75_OP_SEL:
		/* Two sources and the condition. */
		has_src2 = 0;
		operands->predicate_read = (uint32_t)nvrtx_sm75_get(word, NVRTX_SM75_PSRC_A_LOW, NVRTX_SM75_PRED_BITS);
		break;
	case NVRTX_SM75_OP_FSETP:
	case NVRTX_SM75_OP_ISETP:
		/* Two sources and a predicate result. */
		has_src2 = 0;
		writes_dst = 0;
		operands->predicate_write = (uint32_t)nvrtx_sm75_get(word, NVRTX_SM75_PDST0_LOW, NVRTX_SM75_PRED_BITS);
		break;
	case NVRTX_SM75_OP_FADD:
		/* A third source only as the moved second one of an immediate or constant form. */
		operands->class_of = NVRTX_SM75_CLASS_FMA;
		if (form == NVRTX_SM75_FORM_REG ||
		    form == NVRTX_SM75_FORM_IMM ||
		    form == NVRTX_SM75_FORM_CB)
			has_src2 = 0;
		break;
	case NVRTX_SM75_OP_FMUL:
	case NVRTX_SM75_OP_FFMA:
		operands->class_of = NVRTX_SM75_CLASS_FMA;
		break;
	case NVRTX_SM75_OP_IMAD:
		operands->class_of = NVRTX_SM75_CLASS_IMAD;
		break;
	case NVRTX_SM75_OP_IMAD_WIDE:
		operands->class_of = NVRTX_SM75_CLASS_IMAD_WIDE;
		break;
	case NVRTX_SM75_OP_FMNMX:
	case NVRTX_SM75_OP_IADD3:
	case NVRTX_SM75_OP_LOP3:
	case NVRTX_SM75_OP_SHF:
	case NVRTX_SM75_OP_PRMT:
		break;
	case NVRTX_SM75_OP_OUT:
		/* The handle and the stream, the next handle, of variable latency. */
		has_src2 = 0;
		operands->class_of = NVRTX_SM75_CLASS_DECOUPLED;
		break;
	default:
		return 1;
	}

	/* The first source. */
	if (has_src0 != 0)
		nvrtx_sm75_add_read(operands, src0, operands->class_of);

	/* The second source when it is a register. */
	if (form == NVRTX_SM75_FORM_REG)
		nvrtx_sm75_add_read(operands, src1, operands->class_of);

	/* The register in the third slot; an IMAD.WIDE's addend is a pair it reads late. */
	if (has_src2 != 0 &&
	    opcode == NVRTX_SM75_OP_IMAD_WIDE &&
	    src2 != NVRTX_SM75_RZ) {
		nvrtx_sm75_add_read(operands, src2, NVRTX_SM75_CLASS_DISPATCH);
		nvrtx_sm75_add_read(operands, src2 + 1U, NVRTX_SM75_CLASS_DISPATCH);
	} else if (has_src2 != 0) {
		nvrtx_sm75_add_read(operands, src2, operands->class_of);
	}

	/* The result, a pair for IMAD.WIDE. */
	if (writes_dst != 0)
		nvrtx_sm75_add_write(operands, dst);
	if (writes_dst != 0 &&
	    opcode == NVRTX_SM75_OP_IMAD_WIDE &&
	    dst != NVRTX_SM75_RZ)
		nvrtx_sm75_add_write(operands, dst + 1U);

	/* Succeeded: the operands are read. */
	return 0;
}

/*
 * Reads the operands of an instruction of its own 12-bit opcode; nonzero for
 * one not known.
 *
 * Values referenced: Mesa 25.3.6
 * src/nouveau/compiler/nak/sm75_instr_latencies.rs, op_category and
 * needs_scoreboards (lines 49-203, 1187-1208), and
 * src/nouveau/compiler/nak/sm80_instr_latencies.rs, the same (lines 85-257,
 * 1433-1453: DecoupledAgu for the address unit's reads).
 */
static int
nvrtx_sm75_operands_other(
	const struct nvrtx_sm75_word *word,
	uint32_t opcode,
	struct nvrtx_sm75_operands *operands)
{
	uint32_t dst;
	uint32_t src0;
	uint32_t src1;
	uint32_t src2;
	uint32_t mode;
	uint32_t extra;
	uint32_t index;

	/* The destination and the three register slots; an instruction of its own encoding has a scoreboard unless it says otherwise. */
	dst = (uint32_t)nvrtx_sm75_get(word, NVRTX_SM75_DST_LOW, NVRTX_SM75_REG_BITS);
	src0 = (uint32_t)nvrtx_sm75_get(word, NVRTX_SM75_SRC0_LOW, NVRTX_SM75_REG_BITS);
	src1 = (uint32_t)nvrtx_sm75_get(word, NVRTX_SM75_SRC1_LOW, NVRTX_SM75_REG_BITS);
	src2 = (uint32_t)nvrtx_sm75_get(word, NVRTX_SM75_SRC2_LOW, NVRTX_SM75_REG_BITS);
	operands->class_of = NVRTX_SM75_CLASS_DECOUPLED;

	/* The operands by the opcode. */
	switch (opcode) {
	case NVRTX_SM75_OP_NOP:
		operands->class_of = NVRTX_SM75_CLASS_DISPATCH;
		break;
	case NVRTX_SM75_OP_S2R:
	case NVRTX_SM75_OP_PIXLD:
	case NVRTX_SM75_OP_IPA:
		/* A result; the lowering gives none of them a register source (the offset and interpolation registers are RZ). */
		nvrtx_sm75_add_write(operands, dst);
		break;
	case NVRTX_SM75_OP_ALD:
		/* The offset and the vertex's handle (RZ but in a geometry program), the result. */
		nvrtx_sm75_add_read(operands, src0, NVRTX_SM75_CLASS_DECOUPLED);
		nvrtx_sm75_add_read(operands, src1, NVRTX_SM75_CLASS_DECOUPLED);
		nvrtx_sm75_add_write(operands, dst);
		break;
	case NVRTX_SM75_OP_ISBERD:
		/* The index and the handle. */
		nvrtx_sm75_add_read(operands, src0, NVRTX_SM75_CLASS_DECOUPLED);
		nvrtx_sm75_add_write(operands, dst);
		break;
	case NVRTX_SM75_OP_LDC:
		/* A result, a pair for a 64-bit load. */
		nvrtx_sm75_add_write(operands, dst);
		mode = (uint32_t)nvrtx_sm75_get(word, NVRTX_SM75_MEM_TYPE_LOW, 3U);
		if (mode == NVRTX_SM75_MEM_B64)
			nvrtx_sm75_add_write(operands, dst + 1U);
		nvrtx_sm75_add_read(operands, src0, NVRTX_SM75_CLASS_DECOUPLED);
		break;
	case NVRTX_SM75_OP_LDG:
	case NVRTX_SM75_OP_STG:
	case NVRTX_SM75_OP_ATOMG:
	case NVRTX_SM75_OP_ATOMG_CAS:
		/* The 64-bit address pair, the data, the compare-and-exchange's second data, the result. */
		nvrtx_sm75_add_read(operands, src0, NVRTX_SM75_CLASS_AGU);
		nvrtx_sm75_add_read(operands, src0 + 1U, NVRTX_SM75_CLASS_AGU);
		if (opcode != NVRTX_SM75_OP_LDG)
			nvrtx_sm75_add_read(operands, src1, NVRTX_SM75_CLASS_AGU);
		if (opcode == NVRTX_SM75_OP_ATOMG_CAS)
			nvrtx_sm75_add_read(operands, src2, NVRTX_SM75_CLASS_AGU);
		if (opcode != NVRTX_SM75_OP_STG)
			nvrtx_sm75_add_write(operands, dst);
		break;
	case NVRTX_SM75_OP_LDS:
	case NVRTX_SM75_OP_STS:
	case NVRTX_SM75_OP_ATOMS:
	case NVRTX_SM75_OP_ATOMS_CAS:
		/* The offset, the data, the second data, the result. */
		nvrtx_sm75_add_read(operands, src0, NVRTX_SM75_CLASS_AGU);
		if (opcode != NVRTX_SM75_OP_LDS)
			nvrtx_sm75_add_read(operands, src1, NVRTX_SM75_CLASS_AGU);
		if (opcode == NVRTX_SM75_OP_ATOMS_CAS)
			nvrtx_sm75_add_read(operands, src2, NVRTX_SM75_CLASS_AGU);
		if (opcode != NVRTX_SM75_OP_STS)
			nvrtx_sm75_add_write(operands, dst);
		break;
	case NVRTX_SM75_OP_AST:
		/* The data, the offset (RZ) and the vertex's handle (RZ but in a geometry program). */
		nvrtx_sm75_add_read(operands, src1, NVRTX_SM75_CLASS_AGU);
		nvrtx_sm75_add_read(operands, src0, NVRTX_SM75_CLASS_AGU);
		nvrtx_sm75_add_read(operands, src2, NVRTX_SM75_CLASS_AGU);
		break;
	case NVRTX_SM75_OP_TEX:
	case NVRTX_SM75_OP_TLD:
	case NVRTX_SM75_OP_TLD4:
	case NVRTX_SM75_OP_TMML:
	case NVRTX_SM75_OP_TXD:
	case NVRTX_SM75_OP_TXQ:
		/* The two source vectors, then the channels written into the two result pairs. */
		nvrtx_sm75_texture_counts(word, opcode, &extra, &mode);
		for (index = 0U; index < extra && src0 != NVRTX_SM75_RZ; index++)
			nvrtx_sm75_add_read(operands, src0 + index, NVRTX_SM75_CLASS_DECOUPLED);
		for (index = 0U; index < mode && src1 != NVRTX_SM75_RZ; index++)
			nvrtx_sm75_add_read(operands, src1 + index, NVRTX_SM75_CLASS_DECOUPLED);
		mode = (uint32_t)nvrtx_sm75_get(word, NVRTX_SM75_TEX_CHANNELS_LOW, 4U);
		extra = (mode & 1U) + ((mode >> 1) & 1U) + ((mode >> 2) & 1U) + ((mode >> 3) & 1U);
		for (index = 0U; index < extra && index < 2U && dst != NVRTX_SM75_RZ; index++)
			nvrtx_sm75_add_write(operands, dst + index);
		for (index = 2U; index < extra && src2 != NVRTX_SM75_RZ; index++)
			nvrtx_sm75_add_write(operands, src2 + index - 2U);
		break;
	case NVRTX_SM75_OP_SHFL:
		/* The source and the result. */
		nvrtx_sm75_add_read(operands, src0, NVRTX_SM75_CLASS_DECOUPLED);
		nvrtx_sm75_add_write(operands, dst);
		break;
	case NVRTX_SM75_OP_FSWZADD:
		/* Two sources (the second in the third slot) and the result, of fixed latency. */
		operands->class_of = NVRTX_SM75_CLASS_FMA;
		nvrtx_sm75_add_read(operands, src0, NVRTX_SM75_CLASS_FMA);
		nvrtx_sm75_add_read(operands, src2, NVRTX_SM75_CLASS_FMA);
		nvrtx_sm75_add_write(operands, dst);
		break;
	case NVRTX_SM75_OP_BRA:
		/* The condition; a branch ends its block. */
		operands->predicate_read = (uint32_t)nvrtx_sm75_get(word, NVRTX_SM75_PSRC_A_LOW, NVRTX_SM75_PRED_BITS);
		operands->boundary = 1;
		break;
	case NVRTX_SM75_OP_BSSY:
	case NVRTX_SM75_OP_BSYNC:
	case NVRTX_SM75_OP_KILL:
		/* Convergence and discards end their blocks. */
		operands->boundary = 1;
		break;
	case NVRTX_SM75_OP_BAR:
	case NVRTX_SM75_OP_MEMBAR:
		/* A barrier ends its block and takes 5 cycles to execute. */
		operands->boundary = 1;
		operands->min_delay = NVRTX_SM75_DELAY_SYNC_MIN;
		break;
	case NVRTX_SM75_OP_EXIT:
		/* EXIT reads every output and ends the program. */
		operands->boundary = 1;
		operands->min_delay = NVRTX_SM75_DELAY_EXIT_MIN;
		break;
	default:
		return 1;
	}

	/* Succeeded: the operands are read. */
	return 0;
}

/*
 * Counts the registers a texture instruction reads in its first and its
 * second source vector, from its fields (isa.h, NVRTX_SM75_TEX_VECTOR).
 */
static void
nvrtx_sm75_texture_counts(
	const struct nvrtx_sm75_word *word,
	uint32_t opcode,
	uint32_t *first,
	uint32_t *second)
{
	uint32_t dim;
	uint32_t coordinates;
	uint32_t array;
	uint32_t mode;
	uint32_t offset;
	uint32_t compare;

	/* TXQ reads its level alone. */
	if (opcode == NVRTX_SM75_OP_TXQ) {
		*first = 1U;
		*second = 0U;
		return;
	}

	/* The coordinates the dimension has, and the layer of an array. */
	dim = (uint32_t)nvrtx_sm75_get(word, NVRTX_SM75_TEX_DIM_LOW, 3U);
	coordinates = (dim & 3U) + 1U;
	if ((dim & 3U) == NVRTX_SM75_TEX_DIM_CUBE)
		coordinates = 3U;
	array = ((dim & NVRTX_SM75_TEX_DIM_ARRAY) != 0U) ? 1U : 0U;
	offset = (uint32_t)nvrtx_sm75_get(word, NVRTX_SM75_TEX_AOFFI, 1U);
	compare = (uint32_t)nvrtx_sm75_get(word, NVRTX_SM75_TEX_COMPARE, 1U);

	/* TXD: the coordinates, then the offset with the layer; each coordinate's two derivatives. */
	if (opcode == NVRTX_SM75_OP_TXD) {
		*first = coordinates + (((array | offset) != 0U) ? 1U : 0U);
		*second = coordinates * 2U;
		return;
	}

	/* The others: the layer, then the coordinates. */
	*first = array + coordinates;
	*second = 0U;
	if (opcode == NVRTX_SM75_OP_TMML)
		return;

	/* A TLD4's offset and reference. */
	if (opcode == NVRTX_SM75_OP_TLD4) {
		*second = (uint32_t)nvrtx_sm75_get(word, NVRTX_SM75_TLD4_OFFSET_LOW, 2U) + compare;
		return;
	}

	/* A TLD.MS's sample (its compare bit), a TEX's or a TLD's level of detail or bias, offset and (TEX) reference. */
	if (opcode == NVRTX_SM75_OP_TLD)
		*second += compare;
	mode = (uint32_t)nvrtx_sm75_get(word, NVRTX_SM75_TEX_LOD_LOW, 3U);
	if (mode == NVRTX_SM75_TEX_LOD_BIAS || mode == NVRTX_SM75_TEX_LOD_EXPLICIT)
		(*second)++;
	*second += offset;
	if (opcode == NVRTX_SM75_OP_TEX)
		*second += compare;
}

/* Adds a register read of a class, unless it is RZ or the list is full. */
static void
nvrtx_sm75_add_read(
	struct nvrtx_sm75_operands *operands,
	uint32_t reg,
	uint32_t read_class)
{
	/* RZ is never waited for. */
	if (reg >= NVRTX_SM75_RZ)
		return;
	if (operands->read_count == NVRTX_SM75_OPERANDS_READS)
		return;

	/* The register and how it is read. */
	operands->reads[operands->read_count] = reg;
	operands->read_class[operands->read_count] = read_class;
	operands->read_count++;
}

/* Adds a register write, unless it is RZ or the list is full. */
static void
nvrtx_sm75_add_write(
	struct nvrtx_sm75_operands *operands,
	uint32_t reg)
{
	/* A write to RZ is dropped. */
	if (reg >= NVRTX_SM75_RZ)
		return;
	if (operands->write_count == NVRTX_SM75_OPERANDS_WRITES)
		return;

	/* The register. */
	operands->writes[operands->write_count] = reg;
	operands->write_count++;
}
