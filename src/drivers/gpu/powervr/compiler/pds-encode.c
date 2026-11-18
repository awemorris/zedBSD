/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The machine encoding of the PowerVR Rogue data sequencer's
 * instructions and of the data words its output instructions take.
 */

#include "drivers/gpu/powervr/compiler/pds-encode.h"
#include <stddef.h>

#include <uapi/errno.h>

/*
 * The operand spaces an instruction field accepts.
 *
 * Values referenced:
 * Mesa 26.2.5 src/imagination/vulkan/pds/pvr_rogue_pds_defs.h
 * (PVR_ROGUE_PDSINST_REGS32_CONST32_LOWER 0 .. _UPPER 127,
 * _REGS32_TEMP32 128 .. 159, _REGS32_PTEMP32 192 .. 223;
 * _REGS64_CONST64 0 .. 63, _REGS64_TEMP64 64 .. 79,
 * _REGS64_PTEMP64 96 .. 111; _REGS32TP_TEMP32 0 .. 31,
 * _REGS32TP_PTEMP32 32 .. 63; _REGS64TP_TEMP64 0 .. 15,
 * _REGS64TP_PTEMP64 16 .. 31; _REGS32T_TEMP32 0 .. 31;
 * _REGS64C_CONST64 0 .. 63).
 */
enum pds_space {
	PDS_SPACE_ANY32,
	PDS_SPACE_ANY64,
	PDS_SPACE_TEMPORARY32,
	PDS_SPACE_TEMPORARY64,
	PDS_SPACE_SCRATCH32,
	PDS_SPACE_CONSTANT64
};

/*
 * The instruction classes in the top bits of a word.
 *
 * Values referenced:
 * Mesa 26.2.5 src/imagination/vulkan/pds/pvr_rogue_pds_defs.h
 * (PVR_ROGUE_PDSINST_OPCODEC_ADD64 8, _ADD32 9, _CMP 0xb, _BRA 0xc,
 * _SP 0xd, _DDMAD 0xe, _DOUT 0xf, all at bit 28;
 * PVR_ROGUE_PDSINST_OPCODEB_SFTLP32 2 at bit 29).
 */
#define PDS_CLASS_ADD64		0x8U
#define PDS_CLASS_ADD32		0x9U
#define PDS_CLASS_COMPARE	0xbU
#define PDS_CLASS_BRANCH	0xcU
#define PDS_CLASS_SPECIAL	0xdU
#define PDS_CLASS_DMA_ADD	0xeU
#define PDS_CLASS_OUTPUT	0xfU
#define PDS_CLASS_SHIFT_LOGIC	0x2U

/*
 * The special instructions, in bits 26 to 23 of the special class.
 *
 * Values referenced:
 * Mesa 26.2.5 src/imagination/vulkan/pds/pvr_rogue_pds_defs.h
 * (PVR_ROGUE_PDSINST_OPCODESP_LD 0, _ST 1, _WDF 2, _LIMM 3, _HALT 6,
 * _STMC 7, _NOP 0xf; the _OP_SHIFT 23 of each).
 */
#define PDS_SPECIAL_LOAD	0x0U
#define PDS_SPECIAL_STORE	0x1U
#define PDS_SPECIAL_WAIT_DATA	0x2U
#define PDS_SPECIAL_IMMEDIATE	0x3U
#define PDS_SPECIAL_HALT	0x6U
#define PDS_SPECIAL_STREAM_CLEAR 0x7U
#define PDS_SPECIAL_NOTHING	0xfU

static int operand_check(enum pds_space space, unsigned value);
static int flag_check(unsigned flag);
static int special_word(unsigned condition, unsigned special, uint32_t *word);

/*
 * Encodes a 32-bit add or subtract into a 32-bit temporary.
 *
 * Values referenced:
 * Mesa 26.2.5 src/imagination/vulkan/pds/pvr_rogue_pds_defs.h
 * (PVR_ROGUE_PDSINST_ADD32_OPCODE_SHIFT 28, _CC_SHIFT 27,
 * _ALUM_SHIFT 26, _SNA_SHIFT 24, _SRC0_SHIFT 14, _SRC1_SHIFT 6,
 * _DST_SHIFT 0).
 */
int
powervr_pds_add32(
	unsigned condition,
	unsigned is_signed,
	unsigned subtract,
	unsigned source_0,
	unsigned source_1,
	unsigned destination,
	uint32_t *word)
{
	int error;

	/* Refuses flags that are not single bits. */
	if (condition > 1 || is_signed > 1 || subtract > 1 || word == NULL)
		return EINVAL;

	/* Refuses operands outside the spaces the fields accept. */
	error = operand_check(PDS_SPACE_ANY32, source_0);
	if (error != 0)
		return error;

	error = operand_check(PDS_SPACE_ANY32, source_1);
	if (error != 0)
		return error;

	error = operand_check(PDS_SPACE_SCRATCH32, destination);
	if (error != 0)
		return error;

	/* Assembles the word. */
	*word = PDS_CLASS_ADD32 << 28;
	*word |= (uint32_t)condition << 27;
	*word |= (uint32_t)is_signed << 26;
	*word |= (uint32_t)subtract << 24;
	*word |= (uint32_t)source_0 << 14;
	*word |= (uint32_t)source_1 << 6;
	*word |= (uint32_t)destination;

	/* Succeeded: the add. */
	return 0;
}

/*
 * Encodes a 64-bit add or subtract into a 64-bit temporary.
 *
 * Values referenced:
 * Mesa 26.2.5 src/imagination/vulkan/pds/pvr_rogue_pds_defs.h
 * (PVR_ROGUE_PDSINST_ADD64_OPCODE_SHIFT 28, _CC_SHIFT 27,
 * _ALUM_SHIFT 26, _SNA_SHIFT 24, _SRC0_SHIFT 12, _SRC1_SHIFT 5,
 * _DST_SHIFT 0).
 */
int
powervr_pds_add64(
	unsigned condition,
	unsigned is_signed,
	unsigned subtract,
	unsigned source_0,
	unsigned source_1,
	unsigned destination,
	uint32_t *word)
{
	int error;

	/* Refuses flags that are not single bits. */
	if (condition > 1 || is_signed > 1 || subtract > 1 || word == NULL)
		return EINVAL;

	/* Refuses operands outside the spaces the fields accept. */
	error = operand_check(PDS_SPACE_ANY64, source_0);
	if (error != 0)
		return error;

	error = operand_check(PDS_SPACE_ANY64, source_1);
	if (error != 0)
		return error;

	error = operand_check(PDS_SPACE_TEMPORARY64, destination);
	if (error != 0)
		return error;

	/* Assembles the word. */
	*word = PDS_CLASS_ADD64 << 28;
	*word |= (uint32_t)condition << 27;
	*word |= (uint32_t)is_signed << 26;
	*word |= (uint32_t)subtract << 24;
	*word |= (uint32_t)source_0 << 12;
	*word |= (uint32_t)source_1 << 5;
	*word |= (uint32_t)destination;

	/* Succeeded: the add. */
	return 0;
}

/*
 * Encodes a 32-bit logical operation followed by a shift.
 *
 * With an immediate shift the shift field is the shift itself, six bits
 * of two's complement where negative shifts right; otherwise it names a
 * 32-bit temporary holding the shift.  The first source and the result
 * are temporaries.
 *
 * Values referenced:
 * Mesa 26.2.5 src/imagination/vulkan/pds/pvr_rogue_pds_defs.h
 * (PVR_ROGUE_PDSINST_SFTLP32_OPCODE_SHIFT 29, _IM_SHIFT 28,
 * _CC_SHIFT 27, _LOP_SHIFT 24, _SRC0_SHIFT 19, _SRC1_SHIFT 11,
 * _SRC2_SHIFT 5, _DST_SHIFT 0; PVR_ROGUE_PDSINST_LOP_NONE 0 .. _NOR 7).
 */
int
powervr_pds_shift_logic32(
	unsigned condition,
	unsigned logic,
	unsigned source_0,
	unsigned source_1,
	unsigned shift_immediate,
	unsigned shift,
	unsigned destination,
	uint32_t *word)
{
	int error;

	/* Refuses flags that are not single bits and an undefined logic code. */
	if (condition > 1 || shift_immediate > 1 || logic > 7 || word == NULL)
		return EINVAL;

	/* Refuses operands outside the spaces the fields accept. */
	error = operand_check(PDS_SPACE_SCRATCH32, source_0);
	if (error != 0)
		return error;

	error = operand_check(PDS_SPACE_ANY32, source_1);
	if (error != 0)
		return error;

	error = operand_check(PDS_SPACE_SCRATCH32, destination);
	if (error != 0)
		return error;

	/* An immediate shift is six bits; a register one is a temporary. */
	if (shift_immediate != 0) {
		if (shift > 63)
			return EINVAL;
	} else {
		error = operand_check(PDS_SPACE_TEMPORARY32, shift);
		if (error != 0)
			return error;
	}

	/* Assembles the word. */
	*word = PDS_CLASS_SHIFT_LOGIC << 29;
	*word |= (uint32_t)shift_immediate << 28;
	*word |= (uint32_t)condition << 27;
	*word |= (uint32_t)logic << 24;
	*word |= (uint32_t)source_0 << 19;
	*word |= (uint32_t)source_1 << 11;
	*word |= (uint32_t)shift << 5;
	*word |= (uint32_t)destination;

	/* Succeeded: the shift and logic. */
	return 0;
}

/*
 * Encodes a 64-bit compare of a temporary with a register, setting the
 * predicate.
 *
 * Values referenced:
 * Mesa 26.2.5 src/imagination/vulkan/pds/pvr_rogue_pds_defs.h
 * (PVR_ROGUE_PDSINST_CMP_OPCODE_SHIFT 28, _CC_SHIFT 27, _COP_SHIFT 25,
 * _SETCP_SHIFT 24, _IM_SHIFT 23, _SRC0_SHIFT 18, _SRC1_SHIFT 2), and
 * src/imagination/vulkan/pds/pvr_rogue_pds_encode.h
 * (pvr_pds_inst_encode_cmp: the predicate is always set, im clear).
 */
int
powervr_pds_compare(
	unsigned condition,
	enum powervr_pds_compare compare,
	unsigned source_0,
	unsigned source_1,
	uint32_t *word)
{
	int error;

	/* Refuses a flag that is not a bit and an undefined comparison. */
	if (condition > 1 || (unsigned)compare > 3 || word == NULL)
		return EINVAL;

	/* Refuses operands outside the spaces the fields accept. */
	error = operand_check(PDS_SPACE_TEMPORARY64, source_0);
	if (error != 0)
		return error;

	error = operand_check(PDS_SPACE_ANY64, source_1);
	if (error != 0)
		return error;

	/* Assembles the word, always writing the predicate. */
	*word = PDS_CLASS_COMPARE << 28;
	*word |= (uint32_t)condition << 27;
	*word |= (uint32_t)compare << 25;
	*word |= 1U << 24;
	*word |= (uint32_t)source_0 << 18;
	*word |= (uint32_t)source_1 << 2;

	/* Succeeded: the compare. */
	return 0;
}

/*
 * Encodes a compare of a 64-bit temporary with a 16-bit immediate.
 *
 * Values referenced:
 * Mesa 26.2.5 src/imagination/vulkan/pds/pvr_rogue_pds_defs.h
 * (PVR_ROGUE_PDSINST_CMPI_OPCODE_SHIFT 28, _CC_SHIFT 27, _COP_SHIFT 25,
 * _SETCP_SHIFT 24, _IM_SHIFT 23, _SRC0_SHIFT 18, _IM16_SHIFT 2).
 */
int
powervr_pds_compare_immediate(
	unsigned condition,
	enum powervr_pds_compare compare,
	unsigned source_0,
	unsigned immediate,
	uint32_t *word)
{
	int error;

	/* Refuses a flag that is not a bit, a comparison and an immediate out of range. */
	if (condition > 1 || (unsigned)compare > 3 || immediate > 0xffff || word == NULL)
		return EINVAL;

	/* Refuses an operand outside the space the field accepts. */
	error = operand_check(PDS_SPACE_TEMPORARY64, source_0);
	if (error != 0)
		return error;

	/* Assembles the word, marking the immediate and writing the predicate. */
	*word = PDS_CLASS_COMPARE << 28;
	*word |= (uint32_t)condition << 27;
	*word |= (uint32_t)compare << 25;
	*word |= 1U << 24;
	*word |= 1U << 23;
	*word |= (uint32_t)source_0 << 18;
	*word |= (uint32_t)immediate << 2;

	/* Succeeded: the compare with an immediate. */
	return 0;
}

/*
 * Encodes a branch on a predicate to an instruction address.
 *
 * The predicates are the hardware's codes: 0 the compare predicate, 1
 * and 2 the condition inputs, 3 to 7 the stream-out overflows, 8 keep
 * and 9 out of bounds.
 *
 * Values referenced:
 * Mesa 26.2.5 src/imagination/vulkan/pds/pvr_rogue_pds_defs.h
 * (PVR_ROGUE_PDSINST_BRA_OPCODE_SHIFT 28, _SRCC_SHIFT 24, _NEG_SHIFT 23,
 * _SETC_SHIFT 19, _ADDR_SHIFT 0, PVR_ROGUE_PDSINST_BRAADDR_MASK
 * 0x7ffff; PVR_ROGUE_PDSINST_PREDICATE_P0 0 .. _OOB 9).
 */
int
powervr_pds_branch(
	unsigned predicate,
	unsigned negate,
	unsigned set_predicate,
	unsigned address,
	uint32_t *word)
{
	/* Refuses an undefined predicate, a flag that is not a bit and a far address. */
	if (predicate > 9 || set_predicate > 9 || negate > 1 || address > 0x7ffff || word == NULL)
		return EINVAL;

	/* Assembles the word. */
	*word = PDS_CLASS_BRANCH << 28;
	*word |= (uint32_t)predicate << 24;
	*word |= (uint32_t)negate << 23;
	*word |= (uint32_t)set_predicate << 19;
	*word |= (uint32_t)address;

	/* Succeeded: the branch. */
	return 0;
}

/*
 * Encodes a load from memory described by a 64-bit register.
 *
 * Values referenced:
 * Mesa 26.2.5 src/imagination/vulkan/pds/pvr_rogue_pds_defs.h
 * (PVR_ROGUE_PDSINST_LD_SRC0_SHIFT 0).
 */
int
powervr_pds_load(
	unsigned condition,
	unsigned source,
	uint32_t *word)
{
	int error;

	/* Refuses an operand outside the 64-bit space. */
	error = operand_check(PDS_SPACE_ANY64, source);
	if (error != 0)
		return error;

	/* Builds the special word and adds the source. */
	error = special_word(condition, PDS_SPECIAL_LOAD, word);
	if (error != 0)
		return error;

	*word |= (uint32_t)source;

	/* Succeeded: the load. */
	return 0;
}

/*
 * Encodes a store to memory described by a 64-bit register.
 *
 * Values referenced:
 * Mesa 26.2.5 src/imagination/vulkan/pds/pvr_rogue_pds_defs.h
 * (PVR_ROGUE_PDSINST_ST_SRC0_SHIFT 0).
 */
int
powervr_pds_store(
	unsigned condition,
	unsigned source,
	uint32_t *word)
{
	int error;

	/* Refuses an operand outside the 64-bit space. */
	error = operand_check(PDS_SPACE_ANY64, source);
	if (error != 0)
		return error;

	/* Builds the special word and adds the source. */
	error = special_word(condition, PDS_SPECIAL_STORE, word);
	if (error != 0)
		return error;

	*word |= (uint32_t)source;

	/* Succeeded: the store. */
	return 0;
}

/*
 * Encodes a wait for outstanding data loads.
 */
int
powervr_pds_wait_data(
	unsigned condition,
	uint32_t *word)
{
	int error;

	/* Builds the special word with no operands. */
	error = special_word(condition, PDS_SPECIAL_WAIT_DATA, word);
	if (error != 0)
		return error;

	/* Succeeded: the wait. */
	return 0;
}

/*
 * Encodes a load of a 16-bit immediate into a 32-bit temporary.
 *
 * With global set the immediate names a global register to read instead.
 *
 * Values referenced:
 * Mesa 26.2.5 src/imagination/vulkan/pds/pvr_rogue_pds_defs.h
 * (PVR_ROGUE_PDSINST_LIMM_SRC1_SHIFT 18, _SRC0_SHIFT 2, _GR_SHIFT 1).
 */
int
powervr_pds_load_immediate(
	unsigned condition,
	unsigned destination,
	unsigned immediate,
	unsigned global,
	uint32_t *word)
{
	int error;

	/* Refuses an immediate wider than sixteen bits and a flag that is not a bit. */
	if (immediate > 0xffff || global > 1)
		return EINVAL;

	/* Refuses a destination that is not a temporary. */
	error = operand_check(PDS_SPACE_SCRATCH32, destination);
	if (error != 0)
		return error;

	/* Builds the special word and adds the fields. */
	error = special_word(condition, PDS_SPECIAL_IMMEDIATE, word);
	if (error != 0)
		return error;

	*word |= (uint32_t)destination << 18;
	*word |= (uint32_t)immediate << 2;
	*word |= (uint32_t)global << 1;

	/* Succeeded: the immediate load. */
	return 0;
}

/*
 * Encodes the end of the program.
 */
int
powervr_pds_halt(
	unsigned condition,
	uint32_t *word)
{
	int error;

	/* Builds the special word with no operands. */
	error = special_word(condition, PDS_SPECIAL_HALT, word);
	if (error != 0)
		return error;

	/* Succeeded: the halt. */
	return 0;
}

/*
 * Encodes an instruction that does nothing.
 */
int
powervr_pds_nothing(
	unsigned condition,
	uint32_t *word)
{
	int error;

	/* Builds the special word with no operands. */
	error = special_word(condition, PDS_SPECIAL_NOTHING, word);
	if (error != 0)
		return error;

	/* Succeeded: the no-operation. */
	return 0;
}

/*
 * Encodes a clear of the stream out predicates a mask names.
 *
 * Values referenced:
 * Mesa 26.2.5 src/imagination/vulkan/pds/pvr_rogue_pds_defs.h
 * (PVR_ROGUE_PDSINST_STMC_SOMASK_SHIFT 0, PVR_ROGUE_PDSINST_SOMASK_MASK
 * 0x1f).
 */
int
powervr_pds_stream_clear(
	unsigned condition,
	unsigned mask,
	uint32_t *word)
{
	int error;

	/* Refuses a mask wider than the five predicates. */
	if (mask > 0x1fU)
		return EINVAL;

	/* Builds the special word and adds the mask. */
	error = special_word(condition, PDS_SPECIAL_STREAM_CLEAR, word);
	if (error != 0)
		return error;

	*word |= (uint32_t)mask;

	/* Succeeded: the clear. */
	return 0;
}

/*
 * Encodes a DMA whose address is a multiply-add: source 2 plus
 * source 0 times source 1, with the transfer described by the 64-bit
 * constant source 3 (see powervr_pds_dma_add_control_word).
 *
 * Values referenced:
 * Mesa 26.2.5 src/imagination/vulkan/pds/pvr_rogue_pds_defs.h
 * (PVR_ROGUE_PDSINST_DDMAD_OPCODE_SHIFT 28, _CC_SHIFT 27, _END_SHIFT 26,
 * _SRC0_SHIFT 18, _SRC1_SHIFT 13, _SRC2_SHIFT 6, _SRC3_SHIFT 0).
 */
int
powervr_pds_dma_add(
	unsigned condition,
	unsigned end,
	unsigned source_0,
	unsigned source_1,
	unsigned source_2,
	unsigned source_3,
	uint32_t *word)
{
	int error;

	/* Refuses flags that are not single bits. */
	if (condition > 1 || end > 1 || word == NULL)
		return EINVAL;

	/* Refuses operands outside the spaces the fields accept. */
	error = operand_check(PDS_SPACE_ANY32, source_0);
	if (error != 0)
		return error;

	error = operand_check(PDS_SPACE_SCRATCH32, source_1);
	if (error != 0)
		return error;

	error = operand_check(PDS_SPACE_ANY64, source_2);
	if (error != 0)
		return error;

	error = operand_check(PDS_SPACE_CONSTANT64, source_3);
	if (error != 0)
		return error;

	/* Assembles the word. */
	*word = PDS_CLASS_DMA_ADD << 28;
	*word |= (uint32_t)condition << 27;
	*word |= (uint32_t)end << 26;
	*word |= (uint32_t)source_0 << 18;
	*word |= (uint32_t)source_1 << 13;
	*word |= (uint32_t)source_2 << 6;
	*word |= (uint32_t)source_3;

	/* Succeeded: the DMA multiply-add. */
	return 0;
}

/*
 * Encodes a data output: a DMA, a write, a shading task issue or a fence,
 * from a 64-bit data word and a 32-bit one.
 *
 * Values referenced:
 * Mesa 26.2.5 src/imagination/vulkan/pds/pvr_rogue_pds_defs.h
 * (PVR_ROGUE_PDSINST_DOUT_OPCODE_SHIFT 28, _CC_SHIFT 27, _END_SHIFT 26,
 * _SRC1_SHIFT 16, _SRC0_SHIFT 8, _DST_SHIFT 0).
 */
int
powervr_pds_output(
	unsigned condition,
	unsigned end,
	enum powervr_pds_output output,
	unsigned source_0,
	unsigned source_1,
	uint32_t *word)
{
	int error;

	/* Refuses flags that are not single bits and an undefined output. */
	if (condition > 1 || end > 1 || (unsigned)output > 5 || word == NULL)
		return EINVAL;

	/* Refuses operands outside the spaces the fields accept. */
	error = operand_check(PDS_SPACE_ANY64, source_0);
	if (error != 0)
		return error;

	error = operand_check(PDS_SPACE_ANY32, source_1);
	if (error != 0)
		return error;

	/* Assembles the word. */
	*word = PDS_CLASS_OUTPUT << 28;
	*word |= (uint32_t)condition << 27;
	*word |= (uint32_t)end << 26;
	*word |= (uint32_t)source_1 << 16;
	*word |= (uint32_t)source_0 << 8;
	*word |= (uint32_t)output;

	/* Succeeded: the output. */
	return 0;
}

/*
 * Builds the 64-bit data word of a shading task issue.
 *
 * The program address is a 4-byte aligned address below 4 GiB in the
 * shader heap; the temporaries are allocated in blocks of four.
 *
 * Values referenced:
 * Mesa 26.2.5 src/imagination/vulkan/pds/pvr_rogue_pds_defs.h
 * (PVR_ROGUE_PDSINST_DOUT_FIELDS_DOUTU_SRC0_DUAL_PHASE_SHIFT 41,
 * _TEMPS_SHIFT 35, _TEMPS_ALIGNSIZE 4, _TEMPS_MASK 0x3f,
 * _SAMPLE_RATE_SHIFT 33, _SAMPLE_RATE_FULL 2, _EXE_OFF_SHIFT 2 with
 * _EXE_OFF_ALIGNSHIFT 2 and _EXE_OFF_MASK 0x3fffffff).
 * Procedure referenced:
 * Mesa 26.2.5 src/imagination/vulkan/pds/pvr_pds.c (pvr_pds_setup_doutu:
 * the temporaries rounded up to blocks of four).
 */
int
powervr_pds_task_word(
	uint64_t address,
	unsigned temporaries,
	unsigned sample_rate,
	unsigned dual_phase,
	uint64_t *word)
{
	uint64_t blocks;

	/* Refuses a misaligned or high program address. */
	if ((address & 3U) != 0 || address > 0xffffffffULL || word == NULL)
		return EINVAL;

	/* Refuses an undefined sample rate and a flag that is not a bit. */
	if (sample_rate > 2 || dual_phase > 1)
		return EINVAL;

	/* Counts the temporaries in blocks of four, refusing more than the field holds. */
	blocks = ((uint64_t)temporaries + 3U) / 4U;
	if (blocks > 0x3f)
		return EINVAL;

	/* Assembles the word; the address keeps its own bit positions. */
	*word = address;
	*word |= (uint64_t)sample_rate << 33;
	*word |= blocks << 35;
	*word |= (uint64_t)dual_phase << 41;

	/* Succeeded: the task issue word. */
	return 0;
}

/*
 * Builds the 64-bit address word of a DMA into the shading cluster's
 * stores: a 40-bit source address and a 13-bit destination offset.
 *
 * Values referenced:
 * Mesa 26.2.5 src/imagination/vulkan/pds/pvr_rogue_pds_defs.h
 * (PVR_ROGUE_PDSINST_DOUT_FIELDS_DOUTD_SRC0_DOFFSET_SHIFT 40 with
 * _DOFFSET_MASK 0x1fff, _SRC0_SBASE_SHIFT 0 with _SBASE_MASK
 * 0xffffffffff).  The system level cache mode field at bit 62 is
 * left clear, as the BXE-4-32 lacks the cache controls it needs
 * (src/imagination/common/device_info/bxe-4-32.h has no
 * has_slc_mcu_cache_controls).
 */
int
powervr_pds_dma_address_word(
	uint64_t address,
	unsigned offset,
	uint64_t *word)
{
	/* Refuses an address beyond 40 bits and an offset beyond 13. */
	if (address > 0xffffffffffULL || offset > 0x1fff || word == NULL)
		return EINVAL;

	/* Assembles the word. */
	*word = address;
	*word |= (uint64_t)offset << 40;

	/* Succeeded: the DMA address word. */
	return 0;
}

/*
 * Builds the 32-bit control word of a DMA: whether it is the last, the
 * word size (1 to 4 words per element), the store it fills, the cache
 * mode, the store address, repeat and the size.
 *
 * Values referenced:
 * Mesa 26.2.5 src/imagination/vulkan/pds/pvr_rogue_pds_defs.h
 * (PVR_ROGUE_PDSINST_DOUT_FIELDS_DOUTD_SRC1_LAST_SHIFT 31,
 * _WORDSIZE_SHIFT 29 with ONE 0 .. FOUR 3, _DEST_SHIFT 28 with
 * COMMON_STORE 1, _CMODE_SHIFT 26 with CACHED 0, BYPASS 1,
 * FORCE_LINE_FILL 2, _AO_SHIFT 13 with _AO_MASK 0x1fff,
 * _REPEAT_SHIFT 12, _BSIZE_SHIFT 0 with _BSIZE_MASK 0xfff).
 */
int
powervr_pds_dma_control_word(
	unsigned last,
	unsigned words,
	unsigned common_store,
	unsigned cache_mode,
	unsigned store_address,
	unsigned repeat,
	unsigned size,
	uint32_t *word)
{
	int error;

	/* Refuses flags that are not single bits. */
	error = flag_check(last);
	if (error != 0)
		return error;

	error = flag_check(common_store);
	if (error != 0)
		return error;

	error = flag_check(repeat);
	if (error != 0)
		return error;

	/* Refuses an element size, cache mode, address or size out of range. */
	if (words < 1 || words > 4 || cache_mode > 2 || store_address > 0x1fff || size > 0xfff || word == NULL)
		return EINVAL;

	/* Assembles the word. */
	*word = (uint32_t)last << 31;
	*word |= (uint32_t)(words - 1U) << 29;
	*word |= (uint32_t)common_store << 28;
	*word |= (uint32_t)cache_mode << 26;
	*word |= (uint32_t)store_address << 13;
	*word |= (uint32_t)repeat << 12;
	*word |= (uint32_t)size;

	/* Succeeded: the DMA control word. */
	return 0;
}

/*
 * Builds the 64-bit control constant of a DMA multiply-add: the DMA
 * control word in the low half and, above it, the bounds test the
 * BXE-4-32's data sequencer offers, with its limit.
 *
 * Values referenced:
 * Mesa 26.2.5 src/imagination/csbgen/rogue/pds.xml
 * (DDMAD_FIELDS_SRC3: msize 33..63, test 32, last 31, wordsize 29..30,
 * dest 28, cmode 26..27, a0 13..25, repeat 12, bsize 0..11, the first
 * two only with PDS_DDMADT), and
 * src/imagination/common/device_info/bxe-4-32.h (has_pds_ddmadt).
 */
int
powervr_pds_dma_add_control_word(
	uint32_t control,
	unsigned test,
	uint32_t limit,
	uint64_t *word)
{
	int error;

	/* Refuses a flag that is not a bit and a limit beyond 31 bits. */
	error = flag_check(test);
	if (error != 0)
		return error;

	if (limit > 0x7fffffffU || word == NULL)
		return EINVAL;

	/* Assembles the word. */
	*word = control;
	*word |= (uint64_t)test << 32;
	*word |= (uint64_t)limit << 33;

	/* Succeeded: the DMA multiply-add control constant. */
	return 0;
}

/*
 * Builds the 32-bit control word of a data write: whether it is the
 * last write, the store it fills (the common store or the unified
 * one), the cache mode, the store address it writes at, and which part
 * of the 64-bit source it writes (the lower word, the upper word, both,
 * or none).
 *
 * Values referenced:
 * Mesa 26.2.5 src/imagination/vulkan/pds/pvr_rogue_pds_defs.h
 * (PVR_ROGUE_PDSINST_DOUT_FIELDS_DOUTW_SRC1_LAST_SHIFT 31, _DEST_SHIFT
 * 28 with UNIFIED_STORE 0 and COMMON_STORE 1, _CMODE_SHIFT 26 with
 * CACHED 0, BYPASS 1, FORCE_LINE_FILL 2, _AO_SHIFT 13 with _AO_CLRMSK
 * 0xfc001fff, _BSIZE_SHIFT 0 with LOWER 0, UPPER 1, ALL64 2, NONE 3).
 */
int
powervr_pds_write_control_word(
	unsigned last,
	unsigned common_store,
	unsigned cache_mode,
	unsigned store_address,
	unsigned size,
	uint32_t *word)
{
	int error;

	/* Refuses flags that are not single bits. */
	error = flag_check(last);
	if (error != 0)
		return error;

	error = flag_check(common_store);
	if (error != 0)
		return error;

	/* Refuses a cache mode, address or size out of range. */
	if (cache_mode > 2 || store_address > 0x1fff || size > 3 || word == NULL)
		return EINVAL;

	/* Assembles the word. */
	*word = (uint32_t)last << 31;
	*word |= (uint32_t)common_store << 28;
	*word |= (uint32_t)cache_mode << 26;
	*word |= (uint32_t)store_address << 13;
	*word |= (uint32_t)size;

	/* Succeeded: the data write control word. */
	return 0;
}

/*
 * Builds the 64-bit source word of a load from memory into temporaries:
 * the 4-byte aligned address, the count of 64-bit words (1 to 7), the
 * cache mode, and the first 64-bit temporary (counted from zero, the
 * persistent ones from 16).
 *
 * Values referenced:
 * Mesa 26.2.5 src/imagination/vulkan/pds/pvr_rogue_pds_defs.h
 * (PVR_ROGUE_PDSINST_LD_LD_SRC0_DEST_SHIFT 47 with
 * PVR_ROGUE_PDSINST_REGS64TP_MASK 0x1f, _CMODE_SHIFT 44 with CACHED 0,
 * BYPASS 1, FORCE_LINE_FILL 2, _COUNT8_SHIFT 41 with
 * PVR_ROGUE_PDSINST_LD_COUNT8_MASK 7, _SRCADD_SHIFT 2 with
 * PVR_ROGUE_PDSINST_LD_SRCADD_MASK 0x3fffffffff, the address in words).
 * The system level cache mode at bit 62 is left clear, as for the DMA.
 */
int
powervr_pds_load_word(
	uint64_t address,
	unsigned count,
	unsigned cache_mode,
	unsigned destination,
	uint64_t *word)
{
	int error;

	/* Refuses a misaligned or wide address and a count out of range. */
	if ((address & 3U) != 0 || (address >> 2) > 0x3fffffffffULL || count < 1 || count > 7 || cache_mode > 2)
		return EINVAL;

	/* Refuses a destination outside the temporaries. */
	error = operand_check(PDS_SPACE_TEMPORARY64, destination);
	if (error != 0)
		return error;

	if (word == NULL)
		return EINVAL;

	/* Assembles the word. */
	*word = (address >> 2) << 2;
	*word |= (uint64_t)count << 41;
	*word |= (uint64_t)cache_mode << 44;
	*word |= (uint64_t)destination << 47;

	/* Succeeded: the load source word. */
	return 0;
}

/*
 * Builds the 64-bit source word of a store from temporaries to memory:
 * the 4-byte aligned address, the count of 32-bit words (1 to 15), the
 * cache mode (write through or write back), and the first 32-bit
 * temporary (counted from zero, the persistent ones from 32).
 *
 * Values referenced:
 * Mesa 26.2.5 src/imagination/vulkan/pds/pvr_rogue_pds_defs.h
 * (PVR_ROGUE_PDSINST_ST_ST_SRC0_SRC_SHIFT 46 with
 * PVR_ROGUE_PDSINST_REGS32TP_MASK 0x3f, _CMODE_SHIFT 44 with
 * WRITE_THROUGH 0, WRITE_BACK 1, LAZY_WRITE_BACK 2, _COUNT4_SHIFT 40
 * with PVR_ROGUE_PDSINST_ST_COUNT4_MASK 0xf, _DSTADD_SHIFT 2 with
 * PVR_ROGUE_PDSINST_ST_SRCADD_MASK 0x3fffffffff, the address in words).
 * The system level cache mode at bit 62 is left clear.
 */
int
powervr_pds_store_word(
	uint64_t address,
	unsigned count,
	unsigned cache_mode,
	unsigned source,
	uint64_t *word)
{
	int error;

	/* Refuses a misaligned or wide address and a count out of range. */
	if ((address & 3U) != 0 || (address >> 2) > 0x3fffffffffULL || count < 1 || count > 15 || cache_mode > 2)
		return EINVAL;

	/* Refuses a source outside the temporaries. */
	error = operand_check(PDS_SPACE_TEMPORARY32, source);
	if (error != 0)
		return error;

	if (word == NULL)
		return EINVAL;

	/* Assembles the word. */
	*word = (address >> 2) << 2;
	*word |= (uint64_t)count << 40;
	*word |= (uint64_t)cache_mode << 44;
	*word |= (uint64_t)source << 46;

	/* Succeeded: the store source word. */
	return 0;
}

/* Refuses an operand outside the register space a field accepts. */
static int
operand_check(
	enum pds_space space,
	unsigned value)
{
	/* Each space has its own ranges; see the table above the enum. */
	switch (space) {
	case PDS_SPACE_ANY32:
		/* Constants and temporaries, then the persistent temporaries. */
		if (value <= 159)
			return 0;
		if (value >= 192 && value <= 223)
			return 0;
		break;
	case PDS_SPACE_ANY64:
		/* Constants and temporaries, then the persistent temporaries. */
		if (value <= 79)
			return 0;
		if (value >= 96 && value <= 111)
			return 0;
		break;
	case PDS_SPACE_TEMPORARY32:
		/* Temporaries and persistent temporaries counted from zero. */
		if (value <= 63)
			return 0;
		break;
	case PDS_SPACE_TEMPORARY64:
		/* Temporaries and persistent temporaries counted from zero. */
		if (value <= 31)
			return 0;
		break;
	case PDS_SPACE_SCRATCH32:
		/* Temporaries only, counted from zero. */
		if (value <= 31)
			return 0;
		break;
	case PDS_SPACE_CONSTANT64:
		/* Constants only. */
		if (value <= 63)
			return 0;
		break;
	}

	/* The operand is outside the space. */
	return EINVAL;
}

/* Refuses a flag that is not zero or one. */
static int
flag_check(
	unsigned flag)
{
	/* A flag is one bit. */
	if (flag > 1)
		return EINVAL;

	/* Succeeded: the flag is a bit. */
	return 0;
}

/*
 * Builds the common part of a special-class instruction.
 *
 * Values referenced:
 * Mesa 26.2.5 src/imagination/vulkan/pds/pvr_rogue_pds_defs.h
 * (PVR_ROGUE_PDSINST_LD_OPCODE_SHIFT 28, _CC_SHIFT 27, _OP_SHIFT 23,
 * the same for ST, WDF, LIMM, HALT and NOP).
 */
static int
special_word(
	unsigned condition,
	unsigned special,
	uint32_t *word)
{
	/* Refuses a flag that is not a bit and a missing result. */
	if (condition > 1 || word == NULL)
		return EINVAL;

	/* Assembles the class, the condition and the special code. */
	*word = PDS_CLASS_SPECIAL << 28;
	*word |= (uint32_t)condition << 27;
	*word |= (uint32_t)special << 23;

	/* Succeeded: the special word. */
	return 0;
}
