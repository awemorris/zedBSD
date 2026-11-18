/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The GSI1 instruction encoder: one 64-bit instruction from its fields.
 *
 * Every instruction is the RISC-V 32-bit layout of its form in the low
 * word and the extension word above it: the high two bits of each
 * register, the float modifiers, the sub-operation and the high
 * immediate.  The encoder places the fields only; which values an
 * instruction may take is the lowering's business.
 */

#ifndef DRIVERS_GPU_GHCI_COMPILER_GSI1_ENCODE_H
#define DRIVERS_GPU_GHCI_COMPILER_GSI1_ENCODE_H

#include "gsi1_isa.h"

#include <stdint.h>

/*
 * The extension word's own fields, as the encoder's callers pass them:
 * the float modifiers of the three sources, saturation and the half
 * select, the sub-operation, and the texture R forms' sampler slot and
 * texel offsets.
 */
struct gsi1_extra {
	uint32_t modifiers;
	uint32_t sub;
	uint32_t sampler;
	int32_t offset_u;
	int32_t offset_v;
	int32_t offset_w;
};

/* Encodes an R-form instruction. */
uint64_t gsi1_encode_r(uint32_t opcode, uint32_t funct3, uint32_t funct7, uint32_t rd, uint32_t rs1, uint32_t rs2, const struct gsi1_extra *extra);

/* Encodes an R4-form instruction (the three-source form). */
uint64_t gsi1_encode_r4(uint32_t opcode, uint32_t funct3, uint32_t rd, uint32_t rs1, uint32_t rs2, uint32_t rs3, const struct gsi1_extra *extra);

/* Encodes an I-form instruction with a 22-bit signed immediate. */
uint64_t gsi1_encode_i(uint32_t opcode, uint32_t funct3, uint32_t rd, uint32_t rs1, int32_t immediate, uint32_t sub);

/* Encodes an I-form shift by a constant amount. */
uint64_t gsi1_encode_shift(uint32_t funct3, uint32_t funct7, uint32_t rd, uint32_t rs1, uint32_t amount);

/* Encodes an S-form instruction with a 22-bit signed immediate. */
uint64_t gsi1_encode_s(uint32_t opcode, uint32_t funct3, uint32_t rs1, uint32_t rs2, int32_t immediate);

/* Encodes a branch whose target is `offset` instructions from the branch. */
uint64_t gsi1_encode_b(uint32_t funct3, uint32_t rs1, int32_t offset);

/* Encodes movi: a register set to a 32-bit value (lui when the low 12 bits are zero). */
uint64_t gsi1_encode_movi(uint32_t rd, uint32_t value);

/* Reports whether an immediate fits the 22-bit signed offsets of the I, S and B forms. */
int gsi1_immediate_fits(int64_t immediate);

#endif /* DRIVERS_GPU_GHCI_COMPILER_GSI1_ENCODE_H */
