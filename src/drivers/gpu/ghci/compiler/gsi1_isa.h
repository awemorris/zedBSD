/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * gsi1_isa.h: the GHCI standard shader ISA GSI1 for C: the operand
 * numbers, the instruction fields, the opcodes, and the registers and
 * export targets of the vertex and fragment stages.  A program is the
 * code alone, without a header or an interface table; how the driver
 * binds its resources is in gsi1-abi.h.  It has no dependency but the C
 * integer constants.
 */

#ifndef GSI1_ISA_H
#define GSI1_ISA_H

/*
 * Operand numbers.
 *
 * Values referenced: amaterrace-gpu d9252e4 docs/ghci.md (section 12.3
 * "Operands"), checked against amaterrace-gpu d9252e4 include/gsi1_isa.h.
 */
#define GSI1_R(n) (n)			/* r0-r62 */
#define GSI1_RQ 63u
#define GSI1_C(n) (64u + (n))		/* c0-c31 */
#define GSI1_C_COUNT 32u
#define GSI1_ZERO 96u
#define GSI1_ONE 97u			/* 1.0 */
#define GSI1_ONEI 98u			/* 1 */
#define GSI1_HALF 99u			/* 0.5 */
#define GSI1_TWO 100u			/* 2.0 */
#define GSI1_MONE 101u			/* -1.0 */
#define GSI1_LANE 102u
#define GSI1_WARP 103u
#define GSI1_EXEC 104u
#define GSI1_KILLED 105u
#define GSI1_WGX 106u
#define GSI1_WGY 107u
#define GSI1_WGZ 108u
#define GSI1_LIDX 109u
#define GSI1_LIDY 110u
#define GSI1_LIDZ 111u
#define GSI1_LIDN 112u
#define GSI1_PRIM 113u
#define GSI1_FRONT 114u
#define GSI1_NWGX 115u
#define GSI1_NWGY 116u
#define GSI1_NWGZ 117u
#define GSI1_ONEH2 118u
#define GSI1_MASK 119u
#define GSI1_VTX_INDEX 120u		/* gl_VertexIndex, a vertex warp's only */
#define GSI1_INST_INDEX 121u		/* gl_InstanceIndex, a vertex warp's only */
#define GSI1_REGISTERS_MAX 63u		/* r0-r62 */

/*
 * A vertex warp's fixed registers: the vertex fetcher also writes the
 * vertex and instance index to r60 and r61 when used_regs is 62 or more
 * (a compiler reads operands 120 and 121 instead), and never writes r62.
 * The attributes arrive packed from r0, at most 60 registers.
 *
 * Values referenced: amaterrace-gpu d9252e4 docs/ghci.md (section
 * 12.10.3, "Vertex Shader Input Conventions"), checked against
 * amaterrace-gpu d9252e4 include/ghci_cmd.h (GHCI_VS_REG_*).
 */
#define GSI1_VS_REG_VERTEX_INDEX 60u
#define GSI1_VS_REG_INSTANCE_INDEX 61u
#define GSI1_VS_REG_SCRATCH 62u
#define GSI1_VS_ATTRIBUTE_REGISTERS 60u

/*
 * The fragment warp's inputs.
 *
 * Values referenced: amaterrace-gpu d9252e4 docs/ghci.md (section
 * 12.10.3, "Fragment Shader Input Interface"; section 12.11, "Fragment
 * program inputs and outputs"), checked against amaterrace-gpu d9252e4
 * include/ghci_cmd.h (GHCI_FS_REG_*).
 */
#define GSI1_IN_B0 0u
#define GSI1_IN_B1 1u
#define GSI1_IN_B2 2u
#define GSI1_IN_Z 3u
#define GSI1_IN_X 4u			/* x + 0.5 */
#define GSI1_IN_Y 5u			/* y + 0.5 */
#define GSI1_IN_SAMPLE_MASK 6u
#define GSI1_IN_CENTROID_B0 7u		/* r7-r9 with fs_centroid (PSO dw15 [16]) */
#define GSI1_IN_COUNT 10u		/* r0-r9 */
#define GSI1_IN_POINT_S 0u		/* a point's PointCoord */
#define GSI1_IN_POINT_T 1u

/*
 * Export targets (S immediate of exp).
 *
 * Values referenced: amaterrace-gpu d9252e4 docs/ghci.md (section
 * 12.10.3, the export tables of the vertex and fragment stages; section
 * 12.12, "Export targets of a fragment warp"), checked against
 * amaterrace-gpu d9252e4 include/ghci_cmd.h (GHCI_EXP_*).
 */
#define GSI1_EXP_POS(c) (0x000u + (c))
#define GSI1_EXP_POINT_SIZE 0x004u
#define GSI1_EXP_VARYING(w) (0x010u + (w))
#define GSI1_EXP_COLOR(k, c) (0x100u + 16u * ((k) & 7u) + (c))
#define GSI1_EXP_DEPTH 0x104u
#define GSI1_EXP_SAMPLE_MASK 0x105u
#define GSI1_EXP_COLOR0_SRC1(c) (0x108u + (c))
#define GSI1_COLOR_ATTACHMENTS 8u

/*
 * lda's immediate: the PT word in [8:2] (words 0-3 the clip-space
 * position x, y, z, w; varying w at 4 + w) and the vertex in [1:0]
 * (vertex 0 is the provoking vertex).  There are 64 varying words (0-63,
 * Vulkan's 64 output and input components).
 *
 * Values referenced: amaterrace-gpu d9252e4 docs/ghci.md (section
 * 12.10.3, "Varying Interpolation" and "Varying Slot Mapping").
 */
#define GSI1_LDA(word, vertex) ((((word) & 0x7Fu) << 2) | ((vertex) & 3u))
#define GSI1_PT_POSITION(c) (c)
#define GSI1_PT_VARYING(w) (4u + (w))
#define GSI1_VARYING_WORDS 64u

/*
 * Instruction fields (section 12.4).  word0 is a RISC-V 32-bit layout;
 * word1 carries the high register bits, the modifiers, the sub-operation
 * and the high immediate.
 *
 * Values referenced: The RISC-V Instruction Set Manual, Volume I:
 * Unprivileged ISA, version 20240411, chapter "RV32I Base Integer
 * Instruction Set" (the R, I, S, B and U formats), and chapter "F"
 * (the R4 format); amaterrace-gpu d9252e4 docs/ghci.md (section 12.4
 * "Instruction format", the word1 fields and the texture slots of section
 * 12.9), checked against amaterrace-gpu d9252e4 include/gsi1_isa.h.
 */
#define GSI1_W0_OPCODE_SHIFT 0u
#define GSI1_W0_RD_SHIFT 7u
#define GSI1_W0_FUNCT3_SHIFT 12u
#define GSI1_W0_RS1_SHIFT 15u
#define GSI1_W0_RS2_SHIFT 20u
#define GSI1_W0_FUNCT7_SHIFT 25u
#define GSI1_W0_RS3_SHIFT 27u
#define GSI1_W0_IMM_SHIFT 20u		/* I: imm[11:0] */
#define GSI1_W0_UIMM_SHIFT 12u		/* U: imm[31:12] */
#define GSI1_W1_RD_SHIFT 0u		/* rd[6:5] */
#define GSI1_W1_RS1_SHIFT 2u
#define GSI1_W1_RS2_SHIFT 4u
#define GSI1_W1_RS3_SHIFT 6u
#define GSI1_W1_N1 (1u << 8)
#define GSI1_W1_A1 (1u << 9)
#define GSI1_W1_N2 (1u << 10)
#define GSI1_W1_A2 (1u << 11)
#define GSI1_W1_N3 (1u << 12)
#define GSI1_W1_A3 (1u << 13)
#define GSI1_W1_SAT (1u << 14)
#define GSI1_W1_HSEL_SHIFT 15u
#define GSI1_W1_SUB_SHIFT 17u
#define GSI1_W1_TEX_SLOT_SHIFT 17u	/* texture forms: sub[3:0], the texture slot (set 0) */
#define GSI1_W1_SAMP_SLOT_SHIFT 6u	/* texture R forms: [9:6], the sampler slot (set 1) */
#define GSI1_TEX_SLOTS 16u
#define GSI1_W1_IMM_HI_SHIFT 22u	/* I/S/B: + sext(imm_hi) << 12 */
#define GSI1_W1_MOVI_SHIFT 2u		/* U: movi's imm[11:0] */
#define GSI1_W1_TEX_DU_SHIFT 22u	/* texture R forms: texel offsets */
#define GSI1_W1_TEX_DV_SHIFT 26u
#define GSI1_W1_TEX_DW_SHIFT 10u
#define GSI1_IMM_MIN (-(1L << 21))	/* the 22-bit signed offsets of I, S, B */
#define GSI1_IMM_MAX ((1L << 21) - 1)

/*
 * Major opcodes (word0[6:0]) and their funct values.
 *
 * Values referenced: The RISC-V Instruction Set Manual, Volume I:
 * Unprivileged ISA, version 20240411, chapter "RV32I Base Integer
 * Instruction Set" (the opcode map: OP, OP-IMM, LUI, LOAD, STORE, BRANCH,
 * custom-0 to custom-3), "M" (funct7 0000001), "F" (OP-FP, the fused
 * multiply-add opcodes and their funct7 and rs2 values), "Zfa" (fround),
 * "Zbb" (min, max, minu, maxu, andn, orn, xnor).
 */
#define GSI1_OP_OP 0x33u
#define GSI1_OP_OPIMM 0x13u
#define GSI1_OP_LUI 0x37u
#define GSI1_OP_LOAD 0x03u
#define GSI1_OP_STORE 0x23u
#define GSI1_OP_BRANCH 0x63u
#define GSI1_OP_OPFP 0x53u
#define GSI1_OP_MADD 0x43u
#define GSI1_OP_MSUB 0x47u
#define GSI1_OP_NMSUB 0x4Bu
#define GSI1_OP_NMADD 0x4Fu
#define GSI1_OP_CUSTOM0 0x0Bu		/* memory and synchronization */
#define GSI1_OP_CUSTOM1 0x2Bu		/* texture, input and output, lanes */
#define GSI1_OP_CUSTOM2 0x5Bu		/* packed 16 bit */
#define GSI1_OP_CUSTOM3 0x7Bu		/* control, transcendental, select, vote */

/* OP: funct3 and funct7. */
#define GSI1_F3_ADD 0u			/* sub: funct7 0x20 */
#define GSI1_F3_SLL 1u
#define GSI1_F3_SLT 2u
#define GSI1_F3_SLTU 3u
#define GSI1_F3_XOR 4u
#define GSI1_F3_SRL 5u			/* sra: funct7 0x20 */
#define GSI1_F3_OR 6u
#define GSI1_F3_AND 7u
#define GSI1_F7_BASE 0x00u
#define GSI1_F7_ALT 0x20u		/* sub, sra, andn, orn, xnor */
#define GSI1_F7_MULDIV 0x01u
#define GSI1_F7_MINMAX 0x05u
#define GSI1_F3_MUL 0u
#define GSI1_F3_MULH 1u
#define GSI1_F3_MULHSU 2u
#define GSI1_F3_MULHU 3u
#define GSI1_F3_DIV 4u
#define GSI1_F3_DIVU 5u
#define GSI1_F3_REM 6u
#define GSI1_F3_REMU 7u
#define GSI1_F3_MIN 4u
#define GSI1_F3_MINU 5u
#define GSI1_F3_MAX 6u
#define GSI1_F3_MAXU 7u

/* LOAD and STORE: funct3. */
#define GSI1_F3_LW 2u
#define GSI1_F3_LWC 6u
#define GSI1_F3_SW 2u

/* STORE: the sub field (word1 [21:17]); sw.x is made by every EXEC lane (the warp's scratch memory). */
#define GSI1_SW_PLAIN 0u
#define GSI1_SW_X 1u

/* BRANCH: funct3 (warp branches; the offset counts instructions). */
#define GSI1_F3_BANY 0u
#define GSI1_F3_BNONE 1u
#define GSI1_F3_BALL 2u
#define GSI1_F3_J 3u
#define GSI1_F3_BEXNZ 4u
#define GSI1_F3_BEXZ 5u

/* OP-FP: funct7 (fmt 00, single), and the rs2 of the one-source ones. */
#define GSI1_F7_FADD 0x00u
#define GSI1_F7_FSUB 0x04u
#define GSI1_F7_FMUL 0x08u
#define GSI1_F7_FSGNJ 0x10u		/* funct3 0 */
#define GSI1_F7_FMINMAX 0x14u		/* funct3 0 fmin, 1 fmax */
#define GSI1_F7_FROUND 0x20u		/* rs2 4 */
#define GSI1_F7_FSQRT 0x2Cu		/* rs2 0 */
#define GSI1_F7_FCMP 0x50u		/* funct3 2 feq, 1 flt, 0 fle, 3 fne */
#define GSI1_F7_FCVT_W_S 0x60u		/* rs2 0 fcvt.w.s, 1 fcvt.wu.s */
#define GSI1_F7_FCVT_S_W 0x68u		/* rs2 0 fcvt.s.w, 1 fcvt.s.wu */
#define GSI1_RS2_FROUND 4u
#define GSI1_F3_FEQ 2u
#define GSI1_F3_FLT 1u
#define GSI1_F3_FLE 0u
#define GSI1_F3_FNE 3u

/* Rounding modes (the funct3 of the float instructions). */
#define GSI1_RM_RNE 0u
#define GSI1_RM_RTZ 1u
#define GSI1_RM_RDN 2u
#define GSI1_RM_RUP 3u
#define GSI1_RM_RMM 4u

/* custom-0: funct3. */
#define GSI1_F3_LDS 0u
#define GSI1_F3_STS 1u
#define GSI1_F3_ATOM 2u			/* R4, sub = GSI1_ATOM_* [| GSI1_ATOM_SHARED] */
#define GSI1_F3_FENCE 4u		/* I, imm GSI1_FENCE_* */
#define GSI1_F3_BAR 5u
#define GSI1_F3_WAIT 6u			/* I, imm cm | ct << 4 | cx << 8 */
#define GSI1_F3_END 7u
#define GSI1_FENCE_GLOBAL 1u
#define GSI1_FENCE_SHARED 2u
#define GSI1_FENCE_ACQUIRE 4u
#define GSI1_ATOM_AND 0u
#define GSI1_ATOM_OR 1u
#define GSI1_ATOM_XOR 2u
#define GSI1_ATOM_XCHG 3u
#define GSI1_ATOM_ADD 4u
#define GSI1_ATOM_SUB 5u
#define GSI1_ATOM_SMAX 6u
#define GSI1_ATOM_SMIN 7u
#define GSI1_ATOM_UMAX 8u
#define GSI1_ATOM_UMIN 9u
#define GSI1_ATOM_CMPXCHG 10u		/* rs3 is the compare value */
#define GSI1_ATOM_SHARED (1u << 4)	/* atoms: shared memory (rs1 a byte address) */

/*
 * custom-1: funct3, the funct7 modifiers of the texture R forms (section
 * 12.9) and the texsz immediates.
 */
#define GSI1_F3_TEXL 0u
#define GSI1_F3_TEXB 1u
#define GSI1_F3_TEXF 2u
#define GSI1_F3_TEXSZ 3u
#define GSI1_F3_EXP 4u			/* S */
#define GSI1_F3_LDA 5u			/* I */
#define GSI1_F3_QD 6u
#define GSI1_F3_SHFL 7u
#define GSI1_TEX_F7_COMPARE (1u << 0)	/* .c: Dref after the coordinates */
#define GSI1_TEX_F7_GATHER (1u << 1)	/* texl: texg */
#define GSI1_TEX_F7_COMPONENT_SHIFT 2u	/* texg's component */
#define GSI1_TEX_F7_QUERY_LOD 0x10u	/* texb: texqlod */
#define GSI1_TEX_F7_THREE (1u << 5)	/* .3: a third coordinate in rs1+2 */
#define GSI1_TEXSZ_SIZE 0u		/* {height, width} of level rs1 */
#define GSI1_TEXSZ_LEVELS 1u		/* texsz.l */
#define GSI1_TEXSZ_DEPTH 2u		/* texsz.d: depth or layers of level rs1 */
#define GSI1_TEXSZ_SAMPLES 3u		/* texsz.s (DEVICE_CAPS[31]) */
#define GSI1_QD_DX 0u			/* fine, lane ^ 1 */
#define GSI1_QD_DY 1u			/* fine, lane ^ 2 */
#define GSI1_QD_SWAP1 2u
#define GSI1_QD_SWAP2 3u
#define GSI1_QD_SWAP3 4u
#define GSI1_QD_BCAST 5u		/* the quad's lane 0 */

/* custom-2: funct3 and funct7 (packh and unpackh are always there). */
#define GSI1_F3_PACK 7u
#define GSI1_F7_PACKH 0u
#define GSI1_F7_UNPACKH 1u		/* sub 0 low, 1 high */
#define GSI1_F7_SWAPH 2u

/* custom-3: funct3 and sub. */
#define GSI1_F3_SEL 0u			/* R4 */
#define GSI1_F3_EXEC 1u
#define GSI1_F3_KILL 2u
#define GSI1_F3_XU 3u
#define GSI1_F3_VOTE 4u
#define GSI1_F3_BALLOT 5u
#define GSI1_F3_SACTL 6u
#define GSI1_F3_NOP 7u
#define GSI1_EXEC_SET 0u
#define GSI1_EXEC_AND 1u
#define GSI1_EXEC_OR 2u
#define GSI1_EXEC_SAVE 3u
#define GSI1_XU_RCP 0u
#define GSI1_XU_RSQ 1u
#define GSI1_XU_EXP2 2u
#define GSI1_XU_LOG2 3u
#define GSI1_XU_SIN 4u
#define GSI1_XU_COS 5u
#define GSI1_XU_FRACT 6u

#endif /* GSI1_ISA_H */
