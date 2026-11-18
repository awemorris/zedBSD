/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The Valhall instruction words the Mali compiler writes: where each
 * field of a 64-bit instruction lies, the opcode bits of every
 * instruction the compiler uses, and the values of the fields' enums.
 *
 * Every Valhall instruction is one little-endian 64-bit word.  Bits 7:0,
 * 15:8 and 23:16 hold up to three sources, bits 47:40 the destination
 * (or the staging registers of a message), bits 56:48 the primary
 * opcode, bits 58:57 the FAU page the sources read and bits 62:59 the
 * flow control; the other bits depend on the instruction.
 */

#ifndef KERN_DRIVERS_GPU_MALI_COMPILER_VA_ISA_H
#define KERN_DRIVERS_GPU_MALI_COMPILER_VA_ISA_H

/*
 * The common fields of an instruction word.
 *
 * Values referenced: Mesa 26.2.6
 * src/panfrost/compiler/bifrost/valhall/valhall.py (the Source, Dest,
 * Staging and Instruction field offsets) and va_pack.c (va_pack_instr,
 * va_pack_alu).
 */
#define VA_SHIFT_SOURCE1		8U
#define VA_SHIFT_SOURCE2		16U
#define VA_SHIFT_SOURCE3		24U
#define VA_SHIFT_SLOT			30U
#define VA_SHIFT_STAGING_COUNT		33U
#define VA_SHIFT_STAGING_WRITE_COUNT	36U
#define VA_SHIFT_DEST			40U
#define VA_SHIFT_STAGING_CONTROL	46U
#define VA_SHIFT_OPCODE			48U
#define VA_SHIFT_FAU_PAGE		57U
#define VA_SHIFT_FLOW			59U

/*
 * A destination register's write mask (bits 47:46 of the word): both
 * halves of the 32-bit register.
 */
#define VA_DEST_FULL			0xC0U

/*
 * The staging register control of a message (bits 47:46): the staging
 * registers are read, written, or both.
 *
 * Values referenced: Mesa 26.2.6
 * src/panfrost/compiler/bifrost/valhall/valhall.py (Staging.encoded_flags).
 */
#define VA_STAGING_READ			1U
#define VA_STAGING_WRITE		2U

/*
 * A source byte: a register (bits 5:0, bit 6 its last use), a
 * uniform (0x80, the 64-bit slot within the page in bits 5:1, the word in
 * bit 0), an entry of the constant table (0xC0 and the entry), or a
 * special value (0xE0, its number in bits 4:1, the word in bit 0).
 *
 * Values referenced: Mesa 26.2.6
 * src/panfrost/compiler/bifrost/valhall/va_pack.c (va_pack_src,
 * va_pack_fau_64).
 */
#define VA_SOURCE_UNIFORM		0x80U
#define VA_SOURCE_IMMEDIATE		0xC0U
#define VA_SOURCE_SPECIAL		0xE0U

/*
 * The entries of the constant table the compiler reads: zero, all ones,
 * 1.0, and the integers 0, 1, 2, 3 (one per byte).
 *
 * Values referenced: Mesa 26.2.6
 * src/panfrost/compiler/bifrost/valhall/ISA.xml (lut "Immediates").
 */
#define VA_IMMEDIATE_ZERO		0U
#define VA_IMMEDIATE_ALL_ONES		1U
#define VA_IMMEDIATE_INTEGERS_0123	8U
#define VA_IMMEDIATE_FLOAT_ONE		16U

/*
 * The special values of FAU pages 0, 1 and 3: the alpha test datum, the
 * blend descriptor of render target 0 (the others follow it), the
 * workgroup local storage pointer, and the lane's number in the warp.
 *
 * Values referenced: Mesa 26.2.6
 * src/panfrost/compiler/bifrost/valhall/ISA.xml (enums "FAU special page
 * 0", "FAU special page 1" and "FAU special page 3").
 */
#define VA_SPECIAL_ATEST_DATUM		5U
#define VA_SPECIAL_BLEND_DESCRIPTOR_0	8U
#define VA_SPECIAL_WORKGROUP_LOCAL	3U
#define VA_SPECIAL_LANE_ID		1U

/*
 * The thread local storage pointer of FAU special page 1 (each thread's
 * own, which the hardware places in the storage the thread storage
 * descriptor names).
 *
 * Values referenced: Mesa 26.2.6
 * src/panfrost/compiler/bifrost/valhall/ISA.xml (enum "FAU special page
 * 1": thread_local_pointer) and va_pack.c (va_pack_fau_special).
 */
#define VA_SPECIAL_THREAD_LOCAL		1U

/*
 * The flow control values: waits on dependency slots (a mask of slots 0
 * to 2), the waits before ATEST and before BLEND, reconvergence, and the
 * end of the program.
 *
 * Values referenced: Mesa 26.2.6
 * src/panfrost/compiler/bifrost/valhall/ISA.xml (enum "Flow").
 */
#define VA_FLOW_NONE			0U
#define VA_FLOW_WAIT_SLOTS		7U
#define VA_FLOW_WAIT0126		8U
#define VA_FLOW_WAIT_ALL		9U
#define VA_FLOW_RECONVERGE		10U
#define VA_FLOW_END			15U

/* The general dependency slots and the slot of a barrier. */
#define VA_GENERAL_SLOTS		3U
#define VA_SLOT_BARRIER			7U

/*
 * The opcode bits of each instruction, already in place in the word
 * (the primary opcode in bits 56:48 and, where the ISA has one, the
 * secondary opcode in its own bits).
 *
 * Values referenced: Mesa 26.2.6
 * src/panfrost/compiler/bifrost/valhall/ISA.xml (the opcode elements of
 * NOP, BRANCHZ, DISCARD.f32, BARRIER, MOV.i32, FADD.f32, FMIN.f32,
 * FMAX.f32, FMA.f32, FRCP.f32, FRSQ.f32, FLOGD.f32, FEXP.f32,
 * FADD_LSCALE.f32, FROUND.f32, FREXPE.f32, IADD.u32,
 * ISUB.u32, IMUL.i32, IADD_IMM.i32, CLPER.i32, SHADDX.u64, MKVEC.v2i16, S32_TO_F32,
 * U32_TO_F32, F32_TO_S32, F32_TO_U32, F16_TO_F32, U16_TO_F32,
 * V2F32_TO_V2F16,
 * LSHIFT_AND.i32, LSHIFT_OR.i32, LSHIFT_XOR.i32, RSHIFT_OR.i32, MUX.i32,
 * FCMP_OR.f32, ICMP_OR.u32, ICMP_OR.s32, LD_VAR_BUF_IMM.f32, LD_VAR_SPECIAL,
 * LD_ATTR_IMM, LEA_BUF_IMM, LOAD.i32, STORE.i16, STORE.i32, STORE.i128,
 * ATOM_RETURN.i32, TEX_SINGLE, TEX_FETCH, ATEST, BLEND).
 */
#define VA_OP_NOP			0x0000000000000000ULL
#define VA_OP_BRANCHZ			0x001F000000000000ULL
#define VA_OP_DISCARD_F32		0x0020000000000000ULL
#define VA_OP_BARRIER			0x0045000000000000ULL
#define VA_OP_MOV_I32			0x0091000000000000ULL
#define VA_OP_FADD_F32			0x00A4000000000000ULL
#define VA_OP_FMIN_F32			0x00A4000000020000ULL
#define VA_OP_FMAX_F32			0x00A4000000030000ULL
#define VA_OP_FEXP_F32			0x00A4000000080000ULL
#define VA_OP_FADD_LSCALE_F32		0x00A4000000090000ULL
#define VA_OP_FMA_F32			0x00B2000000000000ULL
#define VA_OP_FRCP_F32			0x009C000000000000ULL
#define VA_OP_FRSQ_F32			0x009C000000020000ULL
#define VA_OP_FLOGD_F32			0x009C000000080000ULL
#define VA_OP_FROUND_F32		0x00900000000D0000ULL
#define VA_OP_FREXPE_F32		0x0099000000020000ULL
#define VA_OP_IADD_U32			0x00A0000000000000ULL
#define VA_OP_ISUB_U32			0x00A0000000010000ULL
#define VA_OP_IMUL_I32			0x00A00000000A0000ULL
#define VA_OP_IADD_IMM_I32		0x0110000000000000ULL
#define VA_OP_CLPER_I32			0x00A00000000F0000ULL
#define VA_OP_SHADDX_U64		0x01A3000000070000ULL
#define VA_OP_MKVEC_V2I16		0x00A1000000050000ULL
#define VA_OP_S32_TO_F32		0x0090000000090000ULL
#define VA_OP_U32_TO_F32		0x0090000000190000ULL
#define VA_OP_F32_TO_S32		0x00900000000C0000ULL
#define VA_OP_F32_TO_U32		0x00900000001C0000ULL
#define VA_OP_F16_TO_F32		0x00900000000B0000ULL
#define VA_OP_U16_TO_F32		0x0090000000150000ULL
#define VA_OP_V2F32_TO_V2F16		0x00A5000000040000ULL
#define VA_OP_LSHIFT_AND_I32		0x00B4000100000000ULL
#define VA_OP_LSHIFT_OR_I32		0x00B4000101000000ULL
#define VA_OP_LSHIFT_XOR_I32		0x00B4000102000000ULL
#define VA_OP_RSHIFT_OR_I32		0x00B4000001000000ULL
#define VA_OP_MUX_I32			0x00B8000000000000ULL
#define VA_OP_FCMP_OR_F32		0x00F4000000000000ULL
#define VA_OP_ICMP_OR_U32		0x00F0000000000000ULL
#define VA_OP_ICMP_OR_S32		0x00F8000000000000ULL
#define VA_OP_LD_VAR_BUF_IMM_F32	0x005C000000000000ULL
#define VA_OP_LD_ATTR_IMM		0x0066000000000000ULL
#define VA_OP_LD_VAR_SPECIAL		0x0056000000000000ULL
#define VA_OP_LEA_BUF_IMM		0x005E000000000000ULL
#define VA_OP_LOAD_I32			0x0060000018000000ULL
#define VA_OP_STORE_I16			0x0061000008000000ULL
#define VA_OP_STORE_I32			0x0061000018000000ULL
#define VA_OP_STORE_I128		0x0061000038000000ULL
#define VA_OP_ATOM_RETURN_I32		0x0120000018000000ULL
#define VA_OP_TEX_SINGLE		0x0128000000000000ULL
#define VA_OP_TEX_FETCH			0x0125000000000000ULL
#define VA_OP_ATEST			0x007D000000000000ULL
#define VA_OP_BLEND			0x007F000000000000ULL

/*
 * The opcode bits of the cube map transform, the texture gather, the
 * image texel loads and addresses, and the converting store.
 *
 * Values referenced: Mesa 26.2.6
 * src/panfrost/compiler/bifrost/valhall/ISA.xml (the opcode elements of
 * CUBEFACE1, CUBEFACE2_V9, CUBE_SSEL, CUBE_TSEL, TEX_GATHER, LD_TEX_IMM,
 * LEA_TEX_IMM and ST_CVT).
 */
#define VA_OP_CUBEFACE1			0x00C0000000000000ULL
#define VA_OP_CUBEFACE2_V9		0x00C1000000000000ULL
#define VA_OP_CUBE_SSEL			0x00BC000000000000ULL
#define VA_OP_CUBE_TSEL			0x00BC000001000000ULL
#define VA_OP_TEX_GATHER		0x0129000000000000ULL
#define VA_OP_LD_TEX_IMM		0x0066002000000000ULL
#define VA_OP_LEA_TEX_IMM		0x0067002000000000ULL
#define VA_OP_ST_CVT			0x0071000000000000ULL

/*
 * The opcode bits of the depth and stencil write and of the 16-bit to
 * 32-bit integer widening, and ZS_EMIT's depth write (bit 25).
 *
 * Values referenced: Mesa 26.2.6
 * src/panfrost/compiler/bifrost/valhall/ISA.xml (the opcode elements of
 * ZS_EMIT and U16_TO_U32, and ZS_EMIT's va_mod z).
 */
#define VA_OP_ZS_EMIT			0x007E000000000000ULL
#define VA_OP_U16_TO_U32		0x0090000000140000ULL
#define VA_SHIFT_ZS_EMIT_DEPTH		25U

/*
 * ZS_EMIT's stencil write (bit 24).
 *
 * Value referenced: Mesa 26.2.6
 * src/panfrost/compiler/bifrost/valhall/ISA.xml (ZS_EMIT's va_mod
 * stencil).
 */
#define VA_SHIFT_ZS_EMIT_STENCIL	24U

/*
 * The opcode bits of the right shift with an AND (RSHIFT_AND.i32).
 *
 * Value referenced: Mesa 26.2.6
 * src/panfrost/compiler/bifrost/valhall/ISA.xml (the opcode elements of
 * RSHIFT_AND.i32).
 */
#define VA_OP_RSHIFT_AND_I32		0x00B4000000000000ULL

/*
 * The texture gradient (TEX_GRADIENT): its opcode bits, its flags (the
 * derivatives given in registers, no level of detail bias, no level of
 * detail clamp), and the level of detail mode of a sample whose level
 * comes from the gradient descriptor it makes.
 *
 * Values referenced: Mesa 26.2.6
 * src/panfrost/compiler/bifrost/valhall/ISA.xml (the opcode element of
 * TEX_GRADIENT, enum "LOD mode") and va_pack.c (va_pack_instr, the
 * TEX_GRADIENT flags in bits 13, 14 and 15).
 */
#define VA_OP_TEX_GRADIENT		0x012A000000000000ULL
#define VA_SHIFT_TEX_LOD_BIAS_DISABLE	13U
#define VA_SHIFT_TEX_LOD_CLAMP_DISABLE	14U
#define VA_SHIFT_TEX_DERIVATIVE_ENABLE	15U
#define VA_LOD_MODE_GRDESC		6U
#define VA_REGISTER_TYPE_U		2U
#define VA_WRITE_MASK_RG		3U

/*
 * The opcode bits of the loads of two, three and four words, and the
 * identity load lane they take (bits 38:36).
 *
 * Values referenced: Mesa 26.2.6
 * src/panfrost/compiler/bifrost/valhall/ISA.xml (the opcode elements of
 * LOAD.i64, LOAD.i96 and LOAD.i128, and the enums "Load lane (64-bit)",
 * "Load lane (96-bit)" and "Load lane (128-bit)") and
 * test/assembler-cases.txt (the LOAD.i64, LOAD.i96 and LOAD.i128 words).
 */
#define VA_OP_LOAD_I64			0x0060000028000000ULL
#define VA_OP_LOAD_I96			0x0060000030000000ULL
#define VA_OP_LOAD_I128			0x0060000038000000ULL
#define VA_SHIFT_LOAD_LANE		36U
#define VA_LOAD_LANE_64_IDENTITY	7U
#define VA_LOAD_LANE_96_IDENTITY	6U
#define VA_LOAD_LANE_128_IDENTITY	7U

/*
 * The modifier fields the compiler sets, by their first bit: a
 * comparison's condition and its result type, a conversion's or a
 * rounding's round mode, a negate and an absolute value of each of the
 * first three sources, the not of an operand, an arithmetic right shift,
 * the shift's byte lanes, a MUX's condition, a 16-bit source lane, a
 * branch's sense, a 64-bit source's widening, the shift of SHADDX and the
 * immediates of IADD_IMM and of a memory access.
 *
 * Values referenced: Mesa 26.2.6
 * src/panfrost/compiler/bifrost/valhall/valhall.py (MODIFIERS, Source)
 * and ISA.xml (the va_mod and imm elements of the instructions above).
 */
#define VA_SHIFT_CONDITION		32U
#define VA_SHIFT_RESULT_TYPE		30U
#define VA_SHIFT_ROUND_MODE		30U
#define VA_SHIFT_NEG_SOURCE0		38U
#define VA_SHIFT_ABS_SOURCE0		39U
#define VA_SHIFT_NEG_SOURCE1		36U
#define VA_SHIFT_ABS_SOURCE1		37U
#define VA_SHIFT_NEG_SOURCE2		34U
#define VA_SHIFT_ABS_SOURCE2		35U
#define VA_SHIFT_NOT			35U
#define VA_SHIFT_ARITHMETIC		34U
#define VA_SHIFT_LANES			26U
#define VA_SHIFT_MUX			32U
#define VA_SHIFT_LANE_SOURCE0		28U
#define VA_SHIFT_BRANCH_EQ		36U
#define VA_SHIFT_WIDEN_SOURCE1		26U
#define VA_SHIFT_SHADDX_SHIFT		20U
#define VA_SHIFT_FREXP_SQRT		24U

/* The condition codes and the all-ones result of a comparison. */
#define VA_CONDITION_EQ			0U
#define VA_CONDITION_GT			1U
#define VA_CONDITION_GE			2U
#define VA_CONDITION_NE			3U
#define VA_CONDITION_LT			4U
#define VA_RESULT_M1			2U

/* The round modes. */
#define VA_ROUND_RTE			0U
#define VA_ROUND_RTN			2U
#define VA_ROUND_RTZ			3U

/* A MUX that keeps its first source where the mask is zero. */
#define VA_MUX_INT_ZERO			1U

/*
 * A MUX that selects bit by bit (the first source where the mask's bit
 * is set), and the clamps of a float result to [-1, 1] and to [0, 1]
 * (bits 33:32).
 *
 * Values referenced: Mesa 26.2.6
 * src/panfrost/compiler/bifrost/valhall/ISA.xml (enums "MUX" and
 * "Clamp") and valhall.py (MODIFIERS: clamp).
 */
#define VA_MUX_BIT			3U
#define VA_SHIFT_CLAMP			32U
#define VA_CLAMP_M1_1			2U
#define VA_CLAMP_0_1			3U

/*
 * The byte lanes b00 of a shift (b11, b22 and b33 follow it), and the
 * widening .w0 of a 64-bit source.
 */
#define VA_LANES_B00			4U
#define VA_WIDEN64_W0			8U

/*
 * The message fields: the register format (bits 26:24), the vector size
 * (bits 29:28), the memory access hint (bits 25:24), the varying
 * source format (bits 27:24), the interpolation sample (bits 39:38) and
 * update mode (bits 37:36), the load lane and the unsigned load (bits
 * 39:36), the tables and indices of the immediate forms, and the atomic
 * operation.
 *
 * Values referenced: Mesa 26.2.6
 * src/panfrost/compiler/bifrost/valhall/ISA.xml (enums "Register
 * format", "Vector size", "Memory access", "Source format", "Sample
 * mode", "Update mode", "Atomic operation", and the imm elements of
 * LD_ATTR_IMM, LEA_BUF_IMM, LD_VAR_BUF_IMM and LD_VAR_SPECIAL, and the
 * second staging destination of the instructions whose staging write
 * has no flags) and va_pack.c (the v10
 * position of the LD_VAR_BUF_IMM index, va_pack_load).
 */
#define VA_SHIFT_REGISTER_FORMAT	24U
#define VA_SHIFT_VECTOR_SIZE		28U
#define VA_SHIFT_MEMORY_ACCESS		24U
#define VA_SHIFT_SOURCE_FORMAT		24U
#define VA_SHIFT_UPDATE			36U
#define VA_SHIFT_SAMPLE			38U
#define VA_SHIFT_LOAD_UNSIGNED		39U
#define VA_SHIFT_MEMORY_OFFSET		8U
#define VA_SHIFT_ATTR_TABLE		16U
#define VA_SHIFT_ATTR_INDEX		20U
#define VA_SHIFT_BUF_TABLE		8U
#define VA_SHIFT_BUF_INDEX		12U
#define VA_SHIFT_VARYING_INDEX		16U
#define VA_SHIFT_ATOMIC_OPERATION	22U
#define VA_SHIFT_ATOMIC_OFFSET		8U
#define VA_SHIFT_ATOMIC_COMPARE		26U
#define VA_SHIFT_SPECIAL_INDEX		12U
#define VA_SHIFT_STAGING_DEST		16U

#define VA_REGISTER_FORMAT_AUTO		0U
#define VA_REGISTER_FORMAT_F32		2U
#define VA_REGISTER_FORMAT_S32		4U
#define VA_REGISTER_FORMAT_U32		6U
#define VA_MEMORY_ISTREAM		1U
#define VA_MEMORY_ESTREAM		2U
#define VA_MEMORY_FORCE			3U
#define VA_SOURCE_FORMAT_FLAT32		0U
#define VA_SOURCE_FORMAT_F32		2U
#define VA_UPDATE_STORE			0U
#define VA_UPDATE_CLOBBER		3U
#define VA_SAMPLE_CENTER		0U

/*
 * A CLPER's lane operation (bits 33:32), its inactive lanes' result (bits
 * 25:22) and its subgroup (bits 39:36): the plain and the exclusive-or
 * lane, zero, and the quad.
 *
 * Values referenced: Mesa 26.2.6
 * src/panfrost/compiler/bifrost/valhall/ISA.xml (enums "Lane
 * operation", "Inactive result", "Subgroup size") and va_pack.c
 * (va_pack_alu, CLPER.i32).
 */
#define VA_SHIFT_LANE_OPERATION		32U
#define VA_SHIFT_INACTIVE_RESULT	22U
#define VA_SHIFT_SUBGROUP		36U
#define VA_LANE_OPERATION_NONE		0U
#define VA_LANE_OPERATION_XOR		1U
#define VA_SUBGROUP_QUAD		1U

#define VA_ATOMIC_ADD			2U
#define VA_ATOMIC_SMIN			8U
#define VA_ATOMIC_SMAX			9U
#define VA_ATOMIC_UMIN			10U
#define VA_ATOMIC_UMAX			11U
#define VA_ATOMIC_AND			12U
#define VA_ATOMIC_OR			13U
#define VA_ATOMIC_XOR			14U
#define VA_ATOMIC_XCHG			15U

/*
 * The texture fields: the image handle's table and index and the
 * sampler's in one word, the level of detail mode (bits 15:13), the
 * destination register (bits 21:16), the write mask (bits 25:22), the
 * register type (bits 27:26), the dimension (bits 29:28), the result's
 * 32-bit register width (bit 46), and the texel offset and array enable.
 *
 * Values referenced: Mesa 26.2.6
 * src/panfrost/compiler/pan_nir_lower_tex.c (va_tex_handle, the narrow
 * handle and offsets), valhall.py (MODIFIERS) and ISA.xml (enums "LOD
 * mode", "Register type", "Dimension", "Write mask").
 */
#define VA_TEX_TEXTURE_TABLE_SHIFT	27U
#define VA_TEX_TEXTURE_INDEX_SHIFT	16U
#define VA_TEX_SAMPLER_TABLE_SHIFT	11U
#define VA_SHIFT_TEX_ARRAY		10U
#define VA_SHIFT_TEX_OFFSET		11U
#define VA_SHIFT_TEX_LOD_MODE		13U
#define VA_SHIFT_TEX_DEST		16U
#define VA_SHIFT_TEX_WRITE_MASK		22U
#define VA_SHIFT_TEX_REGISTER_TYPE	26U
#define VA_SHIFT_TEX_DIMENSION		28U
#define VA_SHIFT_TEX_REGISTER_WIDTH	46U
#define VA_LOD_MODE_ZERO		0U
#define VA_LOD_MODE_COMPUTED		1U
#define VA_LOD_MODE_EXPLICIT		4U
#define VA_LOD_MODE_COMPUTED_BIAS	5U
#define VA_REGISTER_TYPE_F		1U

/*
 * The compare (shadow) bit of TEX_SINGLE and TEX_GATHER, and the channel a
 * TEX_GATHER reads (bits 15:14).
 *
 * Values referenced: Mesa 26.2.6
 * src/panfrost/compiler/bifrost/valhall/valhall.py (MODIFIERS: shadow,
 * fetch_component) and va_pack.c (va_pack_instr, the texture cases).
 */
#define VA_SHIFT_TEX_SHADOW		12U
#define VA_SHIFT_TEX_FETCH_COMPONENT	14U

/*
 * The memory access hint of ST_CVT (bits 39:37) and the table and index
 * of LD_TEX_IMM and LEA_TEX_IMM (bits 19:16 and 23:20).
 *
 * Values referenced: Mesa 26.2.6
 * src/panfrost/compiler/bifrost/valhall/ISA.xml (ST_CVT's va_mod
 * memory_access, the imm elements of LD_TEX_IMM and LEA_TEX_IMM).
 */
#define VA_SHIFT_CVT_MEMORY_ACCESS	37U
#define VA_SHIFT_TEX_IMM_TABLE		16U
#define VA_SHIFT_TEX_IMM_INDEX		20U
#define VA_DIMENSION_1D			0U
#define VA_DIMENSION_2D			1U
#define VA_DIMENSION_3D			2U
#define VA_DIMENSION_CUBE		3U
#define VA_WRITE_MASK_RGBA		15U

/*
 * The resource table 61 as the immediate forms encode it (tables 60 to
 * 63 are folded to 12 to 15).
 *
 * Value referenced: Mesa 26.2.6
 * src/panfrost/compiler/bifrost/valhall/valhall.h (va_res_fold_table_idx).
 */
#define VA_TABLE_61_FOLDED		13U

/*
 * The preloaded registers: the compute local id (r55, r56), the workgroup
 * id (r57 to r59), the vertex's internal output index (r59), its vertex
 * and instance index (r60, r61), a fragment's primitive flags (r58, bit 0
 * set for a back face), its pixel position (r59), its
 * coverage (r60), and the interpolation's sample source (r61, whose bits
 * 15:0 are the samples the rasterizer found covered).
 *
 * Values referenced: Mesa 26.2.6
 * src/panfrost/compiler/bifrost/compiler.h (bi_preload_reg, arch 9 and
 * later) and bifrost_compile.c (bi_varying_src0_for_barycentric, the
 * load_sample_mask_in intrinsic).
 */
#define VA_PRELOAD_FIRST		48U
#define VA_PRELOAD_LOCAL_ID_XY		55U
#define VA_PRELOAD_LOCAL_ID_Z		56U
#define VA_PRELOAD_WORKGROUP_X		57U
#define VA_PRELOAD_OUTPUT_INDEX		59U
#define VA_PRELOAD_VERTEX_ID		60U
#define VA_PRELOAD_INSTANCE_ID		61U
#define VA_PRELOAD_PRIMITIVE_FLAGS	58U
#define VA_PRELOAD_POSITION_XY		59U
#define VA_PRELOAD_COVERAGE		60U
#define VA_PRELOAD_VARYING_SOURCE	61U
#define VA_PRELOAD_RASTER_COVERAGE	61U

/*
 * The register whose bits 23:16 hold the number of the sample a fragment
 * program runs for, and the shift and mask that read it (the top bits of
 * the field may read garbage, so only five are kept).
 *
 * Values referenced: Mesa 26.2.6
 * src/panfrost/compiler/bifrost/compiler.h (BI_PRELOAD_SAMPLE_ID) and
 * bifrost_compile.c (bi_load_sample_id_to).
 */
#define VA_PRELOAD_SAMPLE_ID		61U
#define VA_SAMPLE_ID_SHIFT		16U
#define VA_SAMPLE_ID_MASK		0x1FU

/* The registers of a program. */
#define VA_REGISTERS			64U

#endif /* KERN_DRIVERS_GPU_MALI_COMPILER_VA_ISA_H */
