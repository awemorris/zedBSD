/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * How a GSI1 program meets the driver: the one place where the compiler
 * and the Vulkan executor agree on the constant bank, the four hardware
 * descriptor sets and the stage inputs.
 *
 * A program is its code alone.  The hardware binds four descriptor tables
 * (sets 0 to 3) to a draw or a dispatch, shared by its vertex and fragment
 * warps, and mirrors their addresses into the constant bank.  A texture
 * instruction names a texture descriptor of set 0 and a sampler descriptor
 * of set 1 by slot.  Everything else the program reaches it reads from
 * memory: set 2 is the pipeline's resource table, 16-byte units holding a
 * buffer descriptor for each uniform, storage and texel buffer and a copy
 * of the texture descriptor of each storage image and input attachment,
 * and set 3 is the driver's table (the scratch memory).  The push
 * constants and the dynamic offsets are constant-bank words.
 *
 * Shader memory is addressed with 32 bits (lw, sw and atom), so a program
 * uses the low word of every address it reads from a descriptor or the
 * bank; the executor keeps the memory programs reach below 4 GiB.
 */

#ifndef DRIVERS_GPU_GHCI_COMPILER_GSI1_ABI_H
#define DRIVERS_GPU_GHCI_COMPILER_GSI1_ABI_H

#include "gsi1_isa.h"

/*
 * The constant bank: 512 words.  c0-c23 hold push-constant bytes 0-95
 * (the c operands), c24-c31 the hardware's mirror of the four tables'
 * addresses (set s's low word at 24 + 2 s, its high word above), c32-c39
 * push-constant bytes 96-127 (read with lwc), and from c40 the dynamic
 * offsets of the pipeline layout, one word each in the layout's order.
 *
 * Values referenced: amaterrace-gpu d9252e4 docs/ghci.md (section 8.4
 * "Descriptor Tables, Push Constants & Shader Resource Binding", the
 * mirrors c[24..31]; section 12.8, the bank of 512 words read with lwc).
 */
#define GSI1_ABI_BANK_WORDS		512U
#define GSI1_ABI_BANK_PUSH_LOW		0U
#define GSI1_ABI_PUSH_LOW_WORDS		24U
#define GSI1_ABI_BANK_SET(set)		(24U + 2U * (set))
#define GSI1_ABI_BANK_PUSH_HIGH		32U
#define GSI1_ABI_PUSH_HIGH_WORDS	8U
#define GSI1_ABI_BANK_DYNAMIC		40U
#define GSI1_ABI_PUSH_BYTES		128U

/*
 * The most shared memory a workgroup has (PSO dw15 lds_bytes).
 *
 * Values referenced: amaterrace-gpu d9252e4 docs/ghci.md (section 8.2,
 * compute_lds_and_flags [15:0] lds_bytes 0..16384).
 */
#define GSI1_ABI_LDS_BYTES		16384U

/* The four hardware descriptor sets as the driver uses them. */
#define GSI1_ABI_SET_TEXTURES		0U
#define GSI1_ABI_SET_SAMPLERS		1U
#define GSI1_ABI_SET_RESOURCES		2U
#define GSI1_ABI_SET_DRIVER		3U

/*
 * The descriptors and the fields a program reads.  A buffer descriptor:
 * the address, the range in bytes, the element stride of a texel buffer
 * (the low 16 bits of the word at 0x0C) and its format.  A texture
 * descriptor: the address, the width (low 16 bits) and height (high 16
 * bits) in one word, the depth or layers (low 16 bits of the word at
 * 0x0C), the row and slice pitches, and the sample count (low 8 bits of
 * the word at 0x18).
 *
 * Values referenced: amaterrace-gpu d9252e4 docs/ghci.md (section 8.3,
 * GhciTextureDescriptor 32 bytes, GhciSamplerDescriptor 16 bytes,
 * GhciBufferDescriptor 16 bytes).
 */
#define GSI1_ABI_TEXTURE_BYTES		32U
#define GSI1_ABI_SAMPLER_BYTES		16U
#define GSI1_ABI_BUFFER_BYTES		16U
#define GSI1_ABI_BUFFER_BASE		0x00U
#define GSI1_ABI_BUFFER_SIZE		0x08U
#define GSI1_ABI_BUFFER_STRIDE		0x0cU
#define GSI1_ABI_TEXTURE_BASE		0x00U
#define GSI1_ABI_TEXTURE_EXTENT		0x08U
#define GSI1_ABI_TEXTURE_DEPTH		0x0cU
#define GSI1_ABI_TEXTURE_ROW_PITCH	0x10U
#define GSI1_ABI_TEXTURE_SLICE_PITCH	0x14U
#define GSI1_ABI_TEXTURE_SAMPLES	0x18U
#define GSI1_ABI_LOW_HALF		0xffffU
#define GSI1_ABI_LOW_BYTE		0xffU

/*
 * Set 2, the resource table: entries at multiples of 16 bytes from the
 * table's address, a buffer descriptor taking one unit and a texture
 * descriptor two; the most bytes a pipeline's table takes.
 */
#define GSI1_ABI_RESOURCE_UNIT		16U
#define GSI1_ABI_RESOURCE_TABLE_BYTES	4096U

/*
 * Set 3, the driver's table: the address of the scratch memory of the
 * vertex, fragment and compute warps (8 bytes each; a warp's part is the
 * address plus the `warp` operand times the program's bytes per warp).
 */
#define GSI1_ABI_DRIVER_SCRATCH_VERTEX	0x00U
#define GSI1_ABI_DRIVER_SCRATCH_FRAGMENT 0x08U
#define GSI1_ABI_DRIVER_SCRATCH_COMPUTE	0x10U
#define GSI1_ABI_DRIVER_TABLE_BYTES	0x20U

/*
 * A uniform texel buffer takes a texture slot that the executor fills
 * with a linear 2D image of the view's format this many texels wide
 * (element i is texel (i mod width, i / width)), and a buffer descriptor
 * in set 2 of the view (stride = the texel's bytes) for its element count.
 */
#define GSI1_ABI_TEXEL_BUFFER_WIDTH	256U
#define GSI1_ABI_TEXEL_BUFFER_SHIFT	8U

/*
 * The vertex attributes: the driver puts the bound attribute locations
 * in PSO attribute slots 0, 1, ... in increasing location order, and the
 * fetcher writes slot i's C_i components (its format's) to the registers
 * from base_i, the sum of C over the slots before; at most 60 registers.
 *
 * Values referenced: amaterrace-gpu d9252e4 docs/ghci.md (section
 * 12.10.3, "Consecutive Register Allocation ABI").
 */
#define GSI1_ABI_ATTRIBUTE_REGISTERS	GSI1_VS_ATTRIBUTE_REGISTERS

#endif /* DRIVERS_GPU_GHCI_COMPILER_GSI1_ABI_H */
