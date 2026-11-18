/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The machine instructions of the amdrdna shader compiler: one record per
 * GFX10 instruction, the words it is written as, and the registers it
 * reads and writes.
 */

#ifndef DRIVERS_GPU_AMDRDNA_COMPILER_GFX10_ENCODE_H
#define DRIVERS_GPU_AMDRDNA_COMPILER_GFX10_ENCODE_H

#include "gfx10-isa.h"

#include <stdint.h>

/* The most words one instruction is written as: two, and a literal or the lane pattern word. */
#define AMDRDNA_GFX10_MAX_WORDS		3U

/* The most register ranges one instruction reads, and writes. */
#define AMDRDNA_GFX10_MAX_READS		8U
#define AMDRDNA_GFX10_MAX_WRITES	3U

/*
 * The flags of a memory, export or control instruction
 * (struct amdrdna_gfx10_inst.flags), zedBSD's own bits that the encoder
 * puts in each form's fields.
 *
 * GLOBALLY_COHERENT makes a buffer access go past the per-unit cache (and
 * an atomic return the old value), DEVICE_COHERENT makes a load go past the
 * per-array cache too; OFFSET_ADDRESS and INDEX_ADDRESS say
 * that a buffer access's address register is a byte offset or a record
 * index; UNNORMALIZED reads an image at texel coordinates; LAST ends the
 * exports of a program and VALID_MASK says the export carries the lanes
 * that are alive; COMPRESSED says an export carries four 16-bit channels,
 * two in each of its first two VGPRs.
 */
#define AMDRDNA_GFX10_FLAG_GLOBALLY_COHERENT	(1U << 0)
#define AMDRDNA_GFX10_FLAG_OFFSET_ADDRESS	(1U << 1)
#define AMDRDNA_GFX10_FLAG_INDEX_ADDRESS	(1U << 2)
#define AMDRDNA_GFX10_FLAG_UNNORMALIZED		(1U << 3)
#define AMDRDNA_GFX10_FLAG_LAST			(1U << 4)
#define AMDRDNA_GFX10_FLAG_VALID_MASK		(1U << 5)
#define AMDRDNA_GFX10_FLAG_DEVICE_COHERENT	(1U << 6)
#define AMDRDNA_GFX10_FLAG_COMPRESSED		(1U << 7)

/*
 * The kind of an instruction: which form it is written in.
 *
 * A one-source, two-source or comparison vector instruction is written in
 * its short form when its operands fit, and in the wide form otherwise
 * (amdrdna_gfx10_is_wide()); WIDE is an operation that only has the wide
 * form, whose opcode is then the whole 10-bit number.  LABEL is no
 * instruction: it marks the place a branch goes to and is written as no
 * word.
 */
enum amdrdna_gfx10_kind {
	AMDRDNA_GFX10_KIND_LABEL = 0,
	AMDRDNA_GFX10_KIND_SCALAR_TWO,
	AMDRDNA_GFX10_KIND_SCALAR_CONSTANT,
	AMDRDNA_GFX10_KIND_SCALAR_ONE,
	AMDRDNA_GFX10_KIND_SCALAR_COMPARE,
	AMDRDNA_GFX10_KIND_CONTROL,
	AMDRDNA_GFX10_KIND_BRANCH,
	AMDRDNA_GFX10_KIND_SCALAR_LOAD,
	AMDRDNA_GFX10_KIND_VECTOR_ONE,
	AMDRDNA_GFX10_KIND_VECTOR_TWO,
	AMDRDNA_GFX10_KIND_VECTOR_COMPARE,
	AMDRDNA_GFX10_KIND_VECTOR_WIDE,
	AMDRDNA_GFX10_KIND_INTERPOLATE,
	AMDRDNA_GFX10_KIND_SHARED,
	AMDRDNA_GFX10_KIND_BUFFER,
	AMDRDNA_GFX10_KIND_IMAGE,
	AMDRDNA_GFX10_KIND_EXPORT
};

/*
 * The wait counter by which an instruction's results arrive (or, for an
 * export, by which the registers it reads are let go): none (the result is
 * ready for the next instruction), the vector memory loads, the vector
 * memory stores, the exports, and the shared memory and scalar loads.
 */
enum amdrdna_gfx10_counter {
	AMDRDNA_GFX10_COUNTER_NONE = 0,
	AMDRDNA_GFX10_COUNTER_VECTOR_LOAD,
	AMDRDNA_GFX10_COUNTER_VECTOR_STORE,
	AMDRDNA_GFX10_COUNTER_EXPORT,
	AMDRDNA_GFX10_COUNTER_SCALAR
};

/*
 * One GFX10 instruction before it is written as words.
 *
 * The lowering fills one per instruction it chooses; the wait pass adds
 * waits and resolves branches; amdrdna_gfx10_encode() writes it.  Operand
 * fields hold operand codes (gfx10-isa.h): an SGPR, a special register, an
 * inline constant, AMDRDNA_GFX10_LITERAL for `literal`, or a VGPR.  What
 * each field means depends on the kind:
 *
 *   scalar two, one, compare  dst = src[0] op src[1]
 *   scalar constant           dst (or NULL), `immediate` the 16 bits
 *   control                   `immediate` the 16 bits
 *   branch                    `label` the target; `immediate` the distance
 *                             in words once the wait pass resolves it
 *   scalar load               dst the first SGPR loaded, `count` of them;
 *                             src[0] the base SGPR pair (or a buffer
 *                             descriptor's four); `immediate` byte offset
 *   vector one, two, wide     dst = op(src[0], src[1], src[2]); a two-source
 *                             selection reads its condition from src[2];
 *                             a comparison writes its lane mask to dst
 *                             (VCC_LO or an SGPR); a short one or two-source
 *                             operation with `quad_read` reads src[0] (a
 *                             VGPR) from the lane of its group of four
 *                             that `quad_pattern` names
 *   interpolate               dst = the attribute `target`, channel
 *                             `channel`, at the barycentric src[0]; the
 *                             flat move has the parameter code in src[0]
 *   shared                    dst = op(address src[0], data0 src[1], data1
 *                             src[2]); `immediate` byte offset
 *   buffer                    address src[0], descriptor src[1] (four SGPRs),
 *                             byte offset src[2] (an SGPR or a constant),
 *                             data src[3]; a load writes `count` VGPRs from
 *                             dst; an atomic that returns writes dst, which
 *                             is src[3]
 *   image                     dst the first of the VGPRs `mask` selects (a
 *                             gather's four whatever its mask), which a
 *                             store reads and an atomic reads (writing the
 *                             old value back from the first when coherent);
 *                             the `count` address VGPRs from src[0]; image
 *                             descriptor src[1] (eight SGPRs), sampler
 *                             src[2] (four SGPRs, NULL for a load, a store
 *                             or an atomic); `dimension` the image's
 *                             dimension field
 *   export                    target `target`, the VGPRs src[0..3] of the
 *                             channels `mask` enables (a compressed one:
 *                             src[0] and src[1], two channels each)
 */
struct amdrdna_gfx10_inst {
	enum amdrdna_gfx10_kind kind;
	uint16_t opcode;
	uint16_t dst;
	uint16_t src[4];
	uint32_t literal;
	uint32_t immediate;
	uint32_t label;

	/* Nonzero to write a short-form vector instruction in the wide form. */
	uint8_t wide;

	/* The wide form's negate and absolute value bits, one per source (AMDRDNA_GFX10_WIDE_NEG/ABS). */
	uint8_t modifiers;

	/* AMDRDNA_GFX10_FLAG_* */
	uint8_t flags;

	/* An image access's channel mask, an export's channel enable. */
	uint8_t mask;

	/* An export's target, an interpolation's attribute. */
	uint8_t target;

	/* An interpolation's channel. */
	uint8_t channel;

	/* The registers a scalar load or a buffer load writes, the address registers an image access reads. */
	uint8_t count;

	/* An image access's dimension field (AMDRDNA_GFX10_IMAGE_DIM_*). */
	uint8_t dimension;

	/*
	 * Nonzero when a short vector operation reads its first source from
	 * another lane of its group of four, and the lane each lane reads
	 * (two bits per lane, AMDRDNA_GFX10_QUAD_*).
	 */
	uint8_t quad_read;
	uint8_t quad_pattern;
};

/*
 * A run of consecutive registers by operand code (SGPRs, the special
 * scalar registers and VGPRs; never a constant).
 */
struct amdrdna_gfx10_range {
	uint16_t first;
	uint16_t count;
};

/*
 * What one instruction does to the registers: the ranges it reads and
 * writes, the lane mask and condition it uses implicitly included, and
 * the counter its results (or an export's reads) wait on.
 */
struct amdrdna_gfx10_access {
	struct amdrdna_gfx10_range reads[AMDRDNA_GFX10_MAX_READS];
	uint32_t reads_used;
	struct amdrdna_gfx10_range writes[AMDRDNA_GFX10_MAX_WRITES];
	uint32_t writes_used;
	enum amdrdna_gfx10_counter counter;
};

/* Writes one instruction: the words (1 to 3) put in words[], 0 for an instruction the encoder cannot write. */
unsigned amdrdna_gfx10_encode(const struct amdrdna_gfx10_inst *inst, uint32_t *words);

/* Reports whether a vector instruction is written in the wide form. */
int amdrdna_gfx10_is_wide(const struct amdrdna_gfx10_inst *inst);

/* Describes the registers an instruction reads and writes. */
void amdrdna_gfx10_describe(const struct amdrdna_gfx10_inst *inst, struct amdrdna_gfx10_access *access);

/* Reports whether an operand code names a VGPR. */
int amdrdna_gfx10_is_vgpr(unsigned code);

/* Reports whether an operand code names a register (an SGPR, a special scalar register or a VGPR). */
int amdrdna_gfx10_is_register(unsigned code);

#endif /* DRIVERS_GPU_AMDRDNA_COMPILER_GFX10_ENCODE_H */
