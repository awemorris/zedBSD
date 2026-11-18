/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The encoding of the QPU branch word, included by qpu.c and qpu71.c.
 *
 * The branch word has one form on V3D 4.2 and 7.1, so each version's ALU
 * word encoder takes this part unchanged.
 */

/*
 * The fields of the branch word, the same on V3D 4.2 and 7.1, and the
 * largest distance in words whose byte offset fits the signed 32-bit offset.
 *
 * Values referenced: Mesa 26.2.6 src/broadcom/qpu/qpu_pack.c lines 50-102
 * (the signal, branch address, condition, ignore, uniform and destination
 * fields) and lines 2571-2620 (the branch's signal value sixteen and the
 * relative destination one).
 */
#define QPU_BRANCH_SIGNAL 16
#define QPU_BRANCH_SIGNAL_SHIFT 53
#define QPU_BRANCH_LOW_SHIFT 35
#define QPU_BRANCH_CONDITION_SHIFT 32
#define QPU_BRANCH_HIGH_SHIFT 24
#define QPU_BRANCH_IGNORE_SHIFT 21
#define QPU_BRANCH_UNIFORM_TARGET_SHIFT 15
#define QPU_BRANCH_UNIFORM_SHIFT 14
#define QPU_BRANCH_TARGET_SHIFT 12
#define QPU_BRANCH_RELATIVE 1
#define QPU_BRANCH_WORDS_MAXIMUM 0x0fffffff

/*
 * Encodes one relative branch word.
 *
 * Values referenced: Mesa 26.2.6 src/broadcom/qpu/qpu_pack.c lines 63-102
 * (the branch fields) and lines 2571-2620 (v3d_qpu_instr_pack_branch: a
 * branch is the word whose multiply opcode is zero and whose signal field is
 * sixteen; the condition field holds two plus the condition after "always";
 * the target and uniform destinations select one for a relative address;
 * the byte offset's bits 3-23 and 24-31 occupy two fields).  The branch word
 * has the same form on V3D 4.2 and 7.1.
 */
int
bcm2711_qpu_encode_branch(
	const struct bcm2711_qpu_branch *branch,
	uint64_t *word)
{
	uint32_t condition;
	uint32_t offset;
	uint64_t packed;

	/* Refusal preserves the caller's word, so a partial machine instruction can never be published. */
	if (branch == NULL)
		return EINVAL;
	if (word == NULL)
		return EINVAL;

	/* The conditions are always, lane zero's flag and its inverse, and the four all-or-any lane tests. */
	if (branch->condition > BCM2711_QPU_BRANCH_ALL_NOT_A)
		return EINVAL;
	if (branch->ignore > BCM2711_QPU_BRANCH_IGNORE_Q)
		return EINVAL;
	if (branch->uniform_relative > 1)
		return EINVAL;

	/* The byte offset is a signed 32-bit value of whole eight-byte words. */
	if (branch->words > QPU_BRANCH_WORDS_MAXIMUM)
		return EINVAL;
	if (branch->words < -QPU_BRANCH_WORDS_MAXIMUM)
		return EINVAL;
	offset = (uint32_t)branch->words * 8U;

	/* "Always" has the field value zero; every other condition follows it from two. */
	condition = 0;
	if (branch->condition != BCM2711_QPU_BRANCH_ALWAYS)
		condition = 2 + (uint32_t)branch->condition - (uint32_t)BCM2711_QPU_BRANCH_LANE0_A;

	/* The fixed branch format is assembled locally before the caller receives any machine bits. */
	packed = (uint64_t)QPU_BRANCH_SIGNAL << QPU_BRANCH_SIGNAL_SHIFT;
	packed |= (uint64_t)((offset & 0x00ffffffU) >> 3) << QPU_BRANCH_LOW_SHIFT;
	packed |= (uint64_t)condition << QPU_BRANCH_CONDITION_SHIFT;
	packed |= (uint64_t)(offset >> 24) << QPU_BRANCH_HIGH_SHIFT;
	packed |= (uint64_t)branch->ignore << QPU_BRANCH_IGNORE_SHIFT;
	packed |= (uint64_t)QPU_BRANCH_RELATIVE << QPU_BRANCH_TARGET_SHIFT;

	/* A branch that moves the uniform stream also takes its relative byte offset from the stream. */
	if (branch->uniform_relative != 0) {
		packed |= (uint64_t)1 << QPU_BRANCH_UNIFORM_SHIFT;
		packed |= (uint64_t)QPU_BRANCH_RELATIVE << QPU_BRANCH_UNIFORM_TARGET_SHIFT;
	}

	/* The complete word reaches the caller. */
	*word = packed;

	/* Succeeded: the caller owns one complete native branch word. */
	return 0;
}
