/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The Ice Lake (graphics version 11) EU instruction encoding where it
 * differs from Gen9 (eu-encoding-gen9.h): every field, opcode and message
 * descriptor of the instructions the encoder emits sits where it does on
 * Gen9, but the hardware types are numbered anew.
 */

#ifndef DRIVERS_GPU_I915_INTEL_EU_ENCODING_GEN11_H
#define DRIVERS_GPU_I915_INTEL_EU_ENCODING_GEN11_H

/*
 * The hardware type of a register or an immediate operand: UD 0, D 1,
 * UW 2, W 3, UB 4, B 5, HF 8, F 9.  The packed immediate vector V shares
 * B's number.
 *
 * Values referenced: Mesa 25.0.7 src/intel/compiler/brw_reg_type.c
 * (brw_type_encode: graphics version 11).
 */
#define GEN11_EU_TYPE_UD		0U
#define GEN11_EU_TYPE_D			1U
#define GEN11_EU_TYPE_UW		2U
#define GEN11_EU_TYPE_W			3U
#define GEN11_EU_TYPE_UB		4U
#define GEN11_EU_TYPE_B			5U
#define GEN11_EU_TYPE_HF		8U
#define GEN11_EU_TYPE_F			9U
#define GEN11_EU_IMM_TYPE_V		5U

#endif /* DRIVERS_GPU_I915_INTEL_EU_ENCODING_GEN11_H */
