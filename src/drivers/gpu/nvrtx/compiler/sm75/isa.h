/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The facts of the Turing (SM75) instruction set the nvrtx shader
 * compiler uses: the shader program header, the shader models, where the
 * fields of a 128-bit instruction lie, the opcodes and the codes of their
 * modifiers, the special registers and the attribute addresses.
 */

#ifndef DRIVERS_GPU_NVRTX_COMPILER_SM75_ISA_H
#define DRIVERS_GPU_NVRTX_COMPILER_SM75_ISA_H

/*
 * ==== The shader program header (version 4) ====
 *
 * 32 words before a graphics program's code: the type and version, what
 * the program does, and the maps of the attributes it reads and writes.
 *
 * Values referenced: Mesa 25.3.6
 * src/nouveau/headers/nvidia/classes/cla097sph.h, NVA097_SPHV3T1_* and
 * NVA097_SPHV3T2_* (lines 33-396, 403-635), and
 * src/nouveau/compiler/nak/sph.rs, SPHV4_SHADER_HEADER_SIZE (line 16),
 * ShaderProgramHeader::new (lines 94-111, version 4 from SM73),
 * set_sph_type to set_does_load_or_store (lines 186-236),
 * set_store_req_start and set_store_req_end (lines 334-341), the map
 * views imap_* and omap_* (lines 114-183).
 */
#define NVRTX_SM75_SPH_WORDS		32U
#define NVRTX_SM75_SPH_BYTES		128U
#define NVRTX_SM75_SPH_TYPE_LOW		0U
#define NVRTX_SM75_SPH_TYPE_VTG		1U
#define NVRTX_SM75_SPH_TYPE_PS		2U
#define NVRTX_SM75_SPH_VERSION_LOW	5U
#define NVRTX_SM75_SPH_VERSION		4U
#define NVRTX_SM75_SPH_SHADER_TYPE_LOW	10U
#define NVRTX_SM75_SPH_VERTEX		1U
#define NVRTX_SM75_SPH_PIXEL		5U
#define NVRTX_SM75_SPH_MRT_ENABLE	14U
#define NVRTX_SM75_SPH_KILLS_PIXELS	15U
#define NVRTX_SM75_SPH_GLOBAL_STORE	16U
#define NVRTX_SM75_SPH_SASS_VERSION_LOW	17U
#define NVRTX_SM75_SPH_LOAD_OR_STORE	26U
#define NVRTX_SM75_SPH_STORE_REQ_START_LOW	140U
#define NVRTX_SM75_SPH_STORE_REQ_END_LOW	152U
#define NVRTX_SM75_SPH_IMAP_AB_LOW	160U
#define NVRTX_SM75_SPH_IMAP_GENERIC_LOW	192U
#define NVRTX_SM75_SPH_IMAP_C_VTG_LOW	336U
#define NVRTX_SM75_SPH_IMAP_C_PS_LOW	464U
#define NVRTX_SM75_SPH_OMAP_AB_LOW	400U
#define NVRTX_SM75_SPH_OMAP_GENERIC_LOW	432U
#define NVRTX_SM75_SPH_OMAP_TARGETS_LOW	576U

/*
 * A pixel program's header: whether it writes the sample mask and the depth
 * (in the registers after its colours, the mask first).
 *
 * Values referenced: NVIDIA open-gpu-doc classes/3d/cla097sph.h
 * (NVA097_SPHV3T2_OMAP_SAMPLE_MASK MW(608:608), NVA097_SPHV3T2_OMAP_DEPTH
 * MW(609:609)) and Mesa 25.3.6 src/nouveau/compiler/nak_private.h, enum
 * nak_fs_out (NAK_FS_OUT_SAMPLE_MASK, NAK_FS_OUT_DEPTH after the eight
 * colours).
 */
#define NVRTX_SM75_SPH_OMAP_SAMPLE_MASK	608U
#define NVRTX_SM75_SPH_OMAP_DEPTH	609U

/*
 * A geometry program's header: its shader type, the streams it emits to
 * (stream 0 alone), the threads that run for each input primitive (the
 * invocations), the primitive it emits (points, line strips, triangle
 * strips) and the most vertices one thread emits.
 *
 * Values referenced: Mesa 25.0.7
 * src/nouveau/headers/nvidia/classes/cla097sph.h
 * (NVA097_SPHV3T1_SHADER_TYPE_GEOMETRY, NVA097_SPHV3T1_STREAM_OUT_MASK
 * MW(31:28), NVA097_SPHV3T1_THREADS_PER_INPUT_PRIMITIVE MW(95:88),
 * NVA097_SPHV3T1_OUTPUT_TOPOLOGY MW(123:120) and its POINTLIST, LINESTRIP
 * and TRIANGLESTRIP, NVA097_SPHV3T1_MAX_OUTPUT_VERTEX_COUNT MW(139:128))
 * and src/nouveau/compiler/nak/sph.rs (encode_header: the geometry
 * stage's fields; from_nir.rs, the stream mask of the active streams).
 */
#define NVRTX_SM75_SPH_GEOMETRY		4U
#define NVRTX_SM75_SPH_STREAM_OUT_LOW	28U
#define NVRTX_SM75_SPH_THREADS_LOW	88U
#define NVRTX_SM75_SPH_TOPOLOGY_LOW	120U
#define NVRTX_SM75_SPH_MAX_VERTICES_LOW	128U
#define NVRTX_SM75_SPH_POINTLIST	1U
#define NVRTX_SM75_SPH_LINESTRIP	6U
#define NVRTX_SM75_SPH_TRIANGLESTRIP	7U

/*
 * A fragment input's interpolation in the header's map: constant (flat),
 * perspective, screen-linear.
 *
 * Values referenced: Mesa 25.3.6 src/nouveau/compiler/nak/sph.rs, enum
 * PixelImap and its u8 values (lines 53-68).
 */
#define NVRTX_SM75_IMAP_CONSTANT	1U
#define NVRTX_SM75_IMAP_PERSPECTIVE	2U
#define NVRTX_SM75_IMAP_SCREEN_LINEAR	3U

/*
 * ==== The shader models ====
 *
 * The compiler targets Turing (SM75: RTX 20, GTX 16), Ampere (SM86: RTX 30) and
 * Ada (SM89: RTX 40).  The three share this file's encoding but for the memory
 * order of a global access (NVRTX_SM80_MEM_*), and Ampere and Ada share one
 * latency table that differs from Turing's (schedule.c).
 *
 * Values referenced: Mesa 25.3.6 src/nouveau/compiler/nak/ir.rs, is_turing,
 * is_ampere and is_ada (lines 9136-9148: Turing 73..79, Ampere 80..88, Ada
 * 89), and src/nouveau/winsys/nouveau_device.c, sm_for_chipset (lines
 * 51-113: 75, 80, 86 and 89); the three models targeted are zedBSD's
 * choice.
 */
#define NVRTX_SM_TURING			75U
#define NVRTX_SM_AMPERE			86U
#define NVRTX_SM_ADA			89U
#define NVRTX_SM_AMPERE_FIRST		80U

/*
 * An instruction is four 32-bit words, the first holding bit 0.
 *
 * Values referenced: Mesa 25.3.6 src/nouveau/compiler/nak/sm70_encode.rs,
 * encode_sm70_shader (lines 4148-4182): four 32-bit words to an instruction.
 */
#define NVRTX_SM75_INSTRUCTION_WORDS	4U
#define NVRTX_SM75_INSTRUCTION_BYTES	16U

/*
 * The general registers the compiler may assign (R0..R252), the zero
 * register, and the two the hardware keeps.
 *
 * Values referenced: Mesa 25.3.6 src/nouveau/compiler/nak/sm70.rs, num_regs
 * and hw_reserved_gprs (lines 75-106),
 * src/nouveau/compiler/nak/sm70_encode.rs, zero_reg (lines 55-62), and
 * src/nouveau/compiler/nak/api.rs, ShaderBin::new, the register count at
 * least 4 (line 263).
 */
#define NVRTX_SM75_GPR_USABLE		253U
#define NVRTX_SM75_RZ			255U
#define NVRTX_SM75_GPR_RESERVED		2U
#define NVRTX_SM75_GPR_MINIMUM		4U

/*
 * The predicates P0..P6, and PT, which is always true.
 *
 * Values referenced: Mesa 25.3.6 src/nouveau/compiler/nak/sm70.rs, num_regs
 * (RegFile::Pred, lines 75-99), and src/nouveau/compiler/nak/sm70_encode.rs,
 * true_reg (lines 64-66), set_pred_reg, set_pred_src_file and set_pred_src
 * (lines 86-141).
 */
#define NVRTX_SM75_P0			0U
#define NVRTX_SM75_PT			7U

/*
 * ==== The fields of an instruction (bit positions in the 128 bits) ====
 */

/*
 * The opcode (12 bits), the guard predicate and its negation.
 *
 * Values referenced: Mesa 25.3.6 src/nouveau/compiler/nak/sm70_encode.rs,
 * set_opcode (lines 68-70), set_pred (lines 197-208) and encode_alu_base
 * (lines 497-559, the 9-bit opcode and the form at 9..11).
 */
#define NVRTX_SM75_OPCODE_LOW		0U
#define NVRTX_SM75_OPCODE_BITS		12U
#define NVRTX_SM75_ALU_OPCODE_BITS	9U
#define NVRTX_SM75_ALU_FORM_LOW		9U
#define NVRTX_SM75_ALU_FORM_BITS	3U
#define NVRTX_SM75_GUARD_LOW		12U
#define NVRTX_SM75_GUARD_NOT		15U

/*
 * The destination register and the first source register.
 *
 * Values referenced: Mesa 25.3.6 src/nouveau/compiler/nak/sm70_encode.rs,
 * set_reg (lines 72-76), set_dst (lines 210-216) and encode_alu_src0
 * (lines 419-431).
 */
#define NVRTX_SM75_DST_LOW		16U
#define NVRTX_SM75_SRC0_LOW		24U
#define NVRTX_SM75_REG_BITS		8U

/*
 * The second source: a register (32..39, abs 62, neg 63), a 32-bit
 * immediate (32..63), or a constant buffer word.
 *
 * Values referenced: Mesa 25.3.6 src/nouveau/compiler/nak/sm70_encode.rs,
 * encode_alu_reg (lines 455-465), encode_alu_imm (lines 481-483) and
 * encode_alu_base (lines 497-559).
 */
#define NVRTX_SM75_SRC1_LOW		32U
#define NVRTX_SM75_SRC1_ABS		62U
#define NVRTX_SM75_SRC1_NEG		63U
#define NVRTX_SM75_IMM_LOW		32U
#define NVRTX_SM75_IMM_BITS		32U

/*
 * A constant buffer operand: the byte offset (38..53) and the buffer
 * (54..58); bit 91 clear for a bound buffer.
 *
 * Values referenced: Mesa 25.3.6 src/nouveau/compiler/nak/sm70_encode.rs,
 * set_src_cb (lines 179-195) and encode_alu_cb (lines 485-495).
 */
#define NVRTX_SM75_CB_OFFSET_LOW	38U
#define NVRTX_SM75_CB_OFFSET_BITS	16U
#define NVRTX_SM75_CB_INDEX_LOW		54U
#define NVRTX_SM75_CB_INDEX_BITS	5U
#define NVRTX_SM75_CB_BINDLESS		91U

/*
 * The first source's abs and neg, and the third source (64..71, abs 74,
 * neg 75).
 *
 * Values referenced: Mesa 25.3.6 src/nouveau/compiler/nak/sm70_encode.rs,
 * encode_alu_src0 and encode_alu_src2 (lines 419-453).
 */
#define NVRTX_SM75_SRC0_NEG		72U
#define NVRTX_SM75_SRC0_ABS		73U
#define NVRTX_SM75_SRC2_LOW		64U
#define NVRTX_SM75_SRC2_ABS		74U
#define NVRTX_SM75_SRC2_NEG		75U

/*
 * The ALU forms: which of sources 1 and 2 is a register, an immediate or a
 * constant buffer word.
 *
 * Values referenced: Mesa 25.3.6 src/nouveau/compiler/nak/sm70_encode.rs,
 * encode_alu_base, the form match (lines 516-558).
 */
#define NVRTX_SM75_FORM_REG		1U
#define NVRTX_SM75_FORM_IMM_SRC2	2U
#define NVRTX_SM75_FORM_CB_SRC2		3U
#define NVRTX_SM75_FORM_IMM		4U
#define NVRTX_SM75_FORM_CB		5U

/*
 * The predicate fields many ALU instructions have: two destinations and
 * two sources with their negations.
 *
 * Values referenced: Mesa 25.3.6 src/nouveau/compiler/nak/sm70_encode.rs,
 * set_pred_dst, set_pred_src_file and set_pred_src (lines 111-141), and the
 * predicate fields of OpFSetP (lines 883-912), OpIAdd3 (lines 1350-1397),
 * OpISetP (lines 1596-1651), OpSel (lines 2103-2139) and OpLop3 (lines
 * 1794-1852).
 */
#define NVRTX_SM75_PDST0_LOW		81U
#define NVRTX_SM75_PDST1_LOW		84U
#define NVRTX_SM75_PSRC_A_LOW		87U
#define NVRTX_SM75_PSRC_A_NOT		90U
#define NVRTX_SM75_PSRC_B_LOW		77U
#define NVRTX_SM75_PSRC_B_NOT		80U
#define NVRTX_SM75_PSRC_C_LOW		68U
#define NVRTX_SM75_PSRC_C_NOT		71U
#define NVRTX_SM75_PRED_BITS		3U

/*
 * The scheduling control: the cycles before the next instruction issues,
 * the yield hint, the scoreboard the result releases (7: none), the one the
 * read of the sources releases (7: none), the scoreboards to wait for, and
 * the operand reuse cache.
 *
 * Values referenced: Mesa 25.3.6 src/nouveau/compiler/nak/sm70_encode.rs,
 * set_instr_deps (lines 242-251); src/nouveau/compiler/nak/sm70.rs,
 * max_instr_delay (lines 275-277);
 * src/nouveau/compiler/nak/calc_instr_deps.rs, BarAlloc, six scoreboards
 * (lines 186-197).
 */
#define NVRTX_SM75_DELAY_LOW		105U
#define NVRTX_SM75_DELAY_BITS		4U
#define NVRTX_SM75_YIELD		109U
#define NVRTX_SM75_WRITE_BAR_LOW	110U
#define NVRTX_SM75_READ_BAR_LOW		113U
#define NVRTX_SM75_BAR_BITS		3U
#define NVRTX_SM75_WAIT_LOW		116U
#define NVRTX_SM75_WAIT_BITS		6U
#define NVRTX_SM75_REUSE_LOW		122U
#define NVRTX_SM75_BAR_NONE		7U
#define NVRTX_SM75_WAIT_ALL		0x3fU
#define NVRTX_SM75_DELAY_MAX		15U

/*
 * ==== Opcodes ====
 *
 * An ALU opcode is 9 bits beside its form; the others are 12 bits.
 *
 * Values referenced: Mesa 25.3.6 src/nouveau/compiler/nak/sm70_encode.rs, the
 * encode of each operation: OpMov (lines 2029-2058), OpSel (2103-2139),
 * OpFMnMx (759-778), OpFSetP (883-912), OpISetP (1596-1651), OpIAdd3
 * (1350-1397), OpLop3 (1794-1852), OpShf (1871-1917), OpFMul (780-802),
 * OpFAdd (703-733), OpFFma (735-757), OpIMad (1499-1529), OpIMad64
 * (1531-1561), OpF2I (1969-1988), OpI2F (1990-2008), OpFRnd (2010-2027),
 * OpMuFu (950-973), OpLd (3039-3080), OpSt (3172-3210), OpNop (3753-3761),
 * OpS2R (3786-3801), OpBra (3635-3660), OpExit (3662-3676), OpLdc
 * (3082-3170), OpAtom (3275-3358), OpMemBar (3507-3527), OpBar (3689-3715),
 * OpBSSy (3609-3621), OpBSync (3623-3633), OpALd (3377-3395), OpASt
 * (3397-3414), OpIpa (3416-3455), OpTex (2499-2565), OpTld (2567-2640),
 * OpKill (3742-3751).
 */
#define NVRTX_SM75_OP_MOV		0x002U
#define NVRTX_SM75_OP_SEL		0x007U
#define NVRTX_SM75_OP_FMNMX		0x009U
#define NVRTX_SM75_OP_FSETP		0x00bU
#define NVRTX_SM75_OP_ISETP		0x00cU
#define NVRTX_SM75_OP_IADD3		0x010U
#define NVRTX_SM75_OP_LOP3		0x012U
#define NVRTX_SM75_OP_SHF		0x019U
#define NVRTX_SM75_OP_FMUL		0x020U
#define NVRTX_SM75_OP_FADD		0x021U
#define NVRTX_SM75_OP_FFMA		0x023U
#define NVRTX_SM75_OP_IMAD		0x024U
#define NVRTX_SM75_OP_IMAD_WIDE		0x025U
#define NVRTX_SM75_OP_F2I		0x105U
#define NVRTX_SM75_OP_I2F		0x106U
#define NVRTX_SM75_OP_FRND		0x107U
#define NVRTX_SM75_OP_MUFU		0x108U
#define NVRTX_SM75_OP_LDG		0x381U
#define NVRTX_SM75_OP_STG		0x386U
#define NVRTX_SM75_OP_NOP		0x918U
#define NVRTX_SM75_OP_S2R		0x919U
#define NVRTX_SM75_OP_BRA		0x947U
#define NVRTX_SM75_OP_EXIT		0x94dU
#define NVRTX_SM75_OP_LDC		0xb82U
#define NVRTX_SM75_OP_ATOMG		0x3a8U
#define NVRTX_SM75_OP_ATOMG_CAS		0x3a9U
#define NVRTX_SM75_OP_ATOMS		0x38cU
#define NVRTX_SM75_OP_ATOMS_CAS		0x38dU
#define NVRTX_SM75_OP_LDS		0x984U
#define NVRTX_SM75_OP_STS		0x388U
#define NVRTX_SM75_OP_MEMBAR		0x992U
#define NVRTX_SM75_OP_BAR		0xb1dU
#define NVRTX_SM75_OP_BSSY		0x945U
#define NVRTX_SM75_OP_BSYNC		0x941U
#define NVRTX_SM75_OP_ALD		0x321U
#define NVRTX_SM75_OP_AST		0x322U
#define NVRTX_SM75_OP_IPA		0x326U
#define NVRTX_SM75_OP_TEX		0xb60U
#define NVRTX_SM75_OP_TLD		0xb66U
#define NVRTX_SM75_OP_KILL		0x95bU

/*
 * A geometry program's own instructions: ISBERD (the attribute handle of an
 * input vertex from its index), and OUT (an ALU encoding; its register
 * form's 12-bit opcode is the reordering's), which emits the output vertex
 * a handle names, cuts the strip, or ends the thread's output (the type in
 * 78..79: 1 emit, 2 cut, 0 the final one), giving the next handle.
 *
 * Values referenced: Mesa 25.0.7 src/nouveau/compiler/nak/sm70.rs
 * (OpIsberd::encode: 0x923, the index in 24..31; OpOut::encode and
 * OpOutFinal::encode: encode_alu of 0x124, the handle the first source,
 * the stream the second, the type in 78..79).
 */
#define NVRTX_SM75_OP_ISBERD		0x923U
#define NVRTX_SM75_OP_OUT		0x124U
#define NVRTX_SM75_OP_OUT_REG		0x324U
#define NVRTX_SM75_OUT_TYPE_LOW		78U
#define NVRTX_SM75_OUT_FINAL		0U
#define NVRTX_SM75_OUT_EMIT		1U
#define NVRTX_SM75_OUT_CUT		2U

/*
 * ==== The modifiers of the instructions ====
 */

/*
 * MOV: the lanes of a quad written (all four).
 *
 * Values referenced: Mesa 25.3.6 src/nouveau/compiler/nak/sm70_encode.rs,
 * OpMov (lines 2029-2058), and src/nouveau/compiler/nak/lower_copy_swap.rs,
 * lower_copy (quad_lanes 0xf, line 36).
 */
#define NVRTX_SM75_MOV_LANES_LOW	72U
#define NVRTX_SM75_MOV_LANES_ALL	0xfU

/*
 * The float rounding (78..79): to nearest even, toward -inf, toward +inf,
 * toward zero; flush to zero (80).
 *
 * Values referenced: Mesa 25.3.6 src/nouveau/compiler/nak/sm70_encode.rs,
 * set_rnd_mode (lines 633-644) and the ftz bit of OpFAdd, OpFMul and OpFFma
 * (lines 703-802).
 */
#define NVRTX_SM75_ROUND_LOW		78U
#define NVRTX_SM75_ROUND_NEAREST	0U
#define NVRTX_SM75_ROUND_DOWN		1U
#define NVRTX_SM75_ROUND_UP		2U
#define NVRTX_SM75_ROUND_ZERO		3U
#define NVRTX_SM75_FTZ			80U

/*
 * FMUL: the post-divide field holds 4 (none).
 *
 * Values referenced: Mesa 25.3.6 src/nouveau/compiler/nak/sm70_encode.rs,
 * OpFMul (lines 780-802).
 */
#define NVRTX_SM75_FMUL_PDIV_LOW	84U
#define NVRTX_SM75_FMUL_PDIV_NONE	4U

/*
 * The comparisons of FSETP (76..79) and ISETP (76..78), and how the result
 * combines with the accumulator (74..75).
 *
 * Values referenced: Mesa 25.3.6 src/nouveau/compiler/nak/sm70_encode.rs,
 * set_float_cmp_op (lines 805-826), set_pred_set_op (lines 828-838) and
 * set_int_cmp_op (lines 840-855).
 */
#define NVRTX_SM75_CMP_LOW		76U
#define NVRTX_SM75_FCMP_LT		0x1U
#define NVRTX_SM75_FCMP_EQ		0x2U
#define NVRTX_SM75_FCMP_GE		0x6U
#define NVRTX_SM75_FCMP_UNORD_NE	0xdU
#define NVRTX_SM75_ICMP_LT		1U
#define NVRTX_SM75_ICMP_EQ		2U
#define NVRTX_SM75_ICMP_NE		5U
#define NVRTX_SM75_ICMP_GE		6U
#define NVRTX_SM75_SETOP_LOW		74U
#define NVRTX_SM75_SETOP_AND		0U

/*
 * ISETP and IMAD: bit 73 set for a signed comparison or product.
 *
 * Values referenced: Mesa 25.3.6 src/nouveau/compiler/nak/sm70_encode.rs,
 * OpISetP (lines 1596-1651) and OpIMad (lines 1499-1529).
 */
#define NVRTX_SM75_SIGNED		73U

/*
 * LOP3: the truth table (72..79) over sources a (0xf0), b (0xcc) and c
 * (0xaa).
 *
 * Values referenced: Mesa 25.3.6 src/nouveau/compiler/nak/sm70_encode.rs,
 * OpLop3 (lines 1794-1852), and src/nouveau/compiler/nak/ir.rs,
 * LogicOp3::SRC_MASKS (line 1769).
 */
#define NVRTX_SM75_LUT_LOW		72U
#define NVRTX_SM75_LUT_A		0xf0U
#define NVRTX_SM75_LUT_B		0xccU
#define NVRTX_SM75_LUT_C		0xaaU

/*
 * SHF: the data type (73..74), the wrap of the shift count (75), right
 * (76), the high half (80).
 *
 * Values referenced: Mesa 25.3.6 src/nouveau/compiler/nak/sm70_encode.rs,
 * OpShf (lines 1871-1917).
 */
#define NVRTX_SM75_SHF_TYPE_LOW		73U
#define NVRTX_SM75_SHF_TYPE_I32		2U
#define NVRTX_SM75_SHF_TYPE_U32		3U
#define NVRTX_SM75_SHF_WRAP		75U
#define NVRTX_SM75_SHF_RIGHT		76U
#define NVRTX_SM75_SHF_HIGH		80U

/*
 * MUFU: the function (74..79).
 *
 * Values referenced: Mesa 25.3.6 src/nouveau/compiler/nak/sm70_encode.rs,
 * OpMuFu (lines 950-973).
 */
#define NVRTX_SM75_MUFU_LOW		74U
#define NVRTX_SM75_MUFU_COS		0U
#define NVRTX_SM75_MUFU_SIN		1U
#define NVRTX_SM75_MUFU_EX2		2U
#define NVRTX_SM75_MUFU_LG2		3U
#define NVRTX_SM75_MUFU_RCP		4U
#define NVRTX_SM75_MUFU_RSQ		5U
#define NVRTX_SM75_MUFU_SQRT		8U

/*
 * The conversions: F2I signed (72), I2F signed (74), the result's size
 * (75..76) and the source's (84..85) as log2 of bytes.
 *
 * Values referenced: Mesa 25.3.6 src/nouveau/compiler/nak/sm70_encode.rs,
 * OpF2I and OpI2F (lines 1969-2008).
 */
#define NVRTX_SM75_F2I_SIGNED		72U
#define NVRTX_SM75_I2F_SIGNED		74U
#define NVRTX_SM75_CVT_DST_SIZE_LOW	75U
#define NVRTX_SM75_CVT_SRC_SIZE_LOW	84U
#define NVRTX_SM75_CVT_SIZE_32		2U

/*
 * The special registers S2R reads (72..79): the thread's place in its group
 * and the group's place in the grid.
 *
 * Values referenced: Mesa 25.3.6 src/nouveau/compiler/nak/sm70_encode.rs,
 * OpS2R (lines 3786-3801), and src/nouveau/compiler/nak_private.h, enum
 * nak_sv, NAK_SV_TID_* and NAK_SV_CTAID_* (lines 115-122).
 */
#define NVRTX_SM75_S2R_INDEX_LOW	72U
#define NVRTX_SM75_SR_TID_X		0x21U
#define NVRTX_SM75_SR_TID_Y		0x22U
#define NVRTX_SM75_SR_TID_Z		0x23U
#define NVRTX_SM75_SR_CTAID_X		0x25U
#define NVRTX_SM75_SR_CTAID_Y		0x26U
#define NVRTX_SM75_SR_CTAID_Z		0x27U

/*
 * A geometry thread's special registers: which of its input primitive's
 * invocations it is, and its place among the primitives the warp runs
 * (bits 7:0 the primitive, 23:16 the vertices of one), which with an input
 * vertex's number makes the index ISBERD takes.
 *
 * Values referenced: Mesa 25.0.7 src/nouveau/compiler/nak_private.h, enum
 * nak_sv (NAK_SV_INVOCATION_ID, NAK_SV_INVOCATION_INFO) and
 * src/nouveau/compiler/nak_nir_lower_vtg_io.c (lower_vtg_io_intrin: the
 * input vertex's index the low byte times byte 2, plus the vertex).
 */
#define NVRTX_SM75_SR_INVOCATION_ID	0x11U
#define NVRTX_SM75_SR_INVOCATION_INFO	0x1dU

/*
 * The memory accesses: a 64-bit address (72), the size (73..75: 4 for 32
 * bits, 5 for 64), the scope (77..78: 2 the GPU) and the order (79..80: 2
 * strong) before SM80, the eviction priority (84..86: 1 normal), and the
 * immediate offset of a global access (40..63).
 *
 * Values referenced: Mesa 25.3.6 src/nouveau/compiler/nak/sm70_encode.rs,
 * set_mem_order (lines 2854-2891, the SM75 branch 2855-2878),
 * set_eviction_priority (lines 2893-2905), set_mem_type (lines 2907-2921),
 * set_mem_access (lines 2923-2934), OpLd and OpSt (lines 3039-3080,
 * 3172-3210).
 */
#define NVRTX_SM75_MEM_A64		72U
#define NVRTX_SM75_MEM_TYPE_LOW		73U
#define NVRTX_SM75_MEM_B32		4U
#define NVRTX_SM75_MEM_B64		5U
#define NVRTX_SM75_MEM_SCOPE_LOW	77U
#define NVRTX_SM75_MEM_SCOPE_GPU	2U
#define NVRTX_SM75_MEM_ORDER_LOW	79U
#define NVRTX_SM75_MEM_ORDER_STRONG	2U
#define NVRTX_SM75_MEM_EVICT_LOW	84U
#define NVRTX_SM75_MEM_EVICT_NORMAL	1U
#define NVRTX_SM75_MEM_OFFSET_LOW	40U
#define NVRTX_SM75_MEM_OFFSET_BITS	24U

/*
 * From SM80 on, the scope and the order are one field (77..80): 7 is strong
 * at the GPU's scope.
 *
 * Values referenced: Mesa 25.3.6 src/nouveau/compiler/nak/sm70_encode.rs,
 * set_mem_order, the SM80 branch (lines 2879-2890).
 */
#define NVRTX_SM80_MEM_ORDER_LOW	77U
#define NVRTX_SM80_MEM_ORDER_BITS	4U
#define NVRTX_SM80_MEM_STRONG_GPU	0x7U

/*
 * LDC: the mode (78..79, 0 indexed) and the data register a store reads
 * (32..39).
 *
 * Values referenced: Mesa 25.3.6 src/nouveau/compiler/nak/sm70_encode.rs,
 * OpLdc (lines 3082-3170) and OpSt (lines 3172-3210).
 */
#define NVRTX_SM75_LDC_MODE_LOW		78U
#define NVRTX_SM75_STORE_DATA_LOW	32U

/*
 * The atomics: the operation (87..90), the type (73..75: 0 unsigned, 1
 * signed 32 bits), and the compare and exchange's second data register
 * (64..71, the compared value at 32..39).
 *
 * Values referenced: Mesa 25.3.6 src/nouveau/compiler/nak/sm70_encode.rs,
 * set_atom_op (lines 3213-3229), set_atom_type (lines 3231-3272) and OpAtom
 * (lines 3275-3358).
 */
#define NVRTX_SM75_ATOM_OP_LOW		87U
#define NVRTX_SM75_ATOM_ADD		0U
#define NVRTX_SM75_ATOM_MIN		1U
#define NVRTX_SM75_ATOM_MAX		2U
#define NVRTX_SM75_ATOM_AND		5U
#define NVRTX_SM75_ATOM_OR		6U
#define NVRTX_SM75_ATOM_XOR		7U
#define NVRTX_SM75_ATOM_EXCH		8U
#define NVRTX_SM75_ATOM_TYPE_LOW	73U
#define NVRTX_SM75_ATOM_U32		0U
#define NVRTX_SM75_ATOM_I32		1U
#define NVRTX_SM75_CAS_DATA_LOW		64U

/*
 * MEMBAR: the scope (76..78): 0 the workgroup, 2 the GPU.
 *
 * Values referenced: Mesa 25.3.6 src/nouveau/compiler/nak/sm70_encode.rs,
 * OpMemBar (lines 3507-3527).
 */
#define NVRTX_SM75_MEMBAR_SCOPE_LOW	76U
#define NVRTX_SM75_MEMBAR_CTA		0U
#define NVRTX_SM75_MEMBAR_GPU		2U

/*
 * BSSY and BSYNC: the convergence barrier (16..19), BSSY's offset to its
 * BSYNC in words from the next instruction (34..63).
 *
 * Values referenced: Mesa 25.3.6 src/nouveau/compiler/nak/sm70_encode.rs,
 * OpBSSy and OpBSync (lines 3609-3633), get_rel_offset and set_rel_offset
 * (lines 3530-3544), and src/nouveau/compiler/nak/sm70.rs, num_regs
 * (RegFile::Bar, line 96).
 */
#define NVRTX_SM75_CONVERGE_BAR_LOW	16U
#define NVRTX_SM75_CONVERGE_BARS	16U
#define NVRTX_SM75_BSSY_OFFSET_LOW	34U
#define NVRTX_SM75_BSSY_OFFSET_BITS	30U

/*
 * The attribute loads and stores of the vertex stage: ALD's vertex (32..39)
 * and AST's (64..71), the offset register (24..31, RZ), the attribute
 * address (40..49) and the components less one (74..75); AST's data
 * (32..39).
 *
 * Values referenced: Mesa 25.3.6 src/nouveau/compiler/nak/sm70_encode.rs,
 * OpALd and OpASt (lines 3377-3414).
 */
#define NVRTX_SM75_ATTR_ADDRESS_LOW	40U
#define NVRTX_SM75_ATTR_ADDRESS_BITS	10U
#define NVRTX_SM75_ATTR_COMPONENTS_LOW	74U
#define NVRTX_SM75_AST_VERTEX_LOW	64U

/*
 * IPA: the attribute's address in words (64..71), the location (76..77, 0
 * the pixel's centre) and the frequency (78..79: 0 pass, 1 constant).
 *
 * Values referenced: Mesa 25.3.6 src/nouveau/compiler/nak/sm70_encode.rs,
 * OpIpa (lines 3416-3455).
 */
#define NVRTX_SM75_IPA_ADDRESS_LOW	64U
#define NVRTX_SM75_IPA_FREQUENCY_LOW	78U
#define NVRTX_SM75_IPA_PASS		0U
#define NVRTX_SM75_IPA_CONSTANT		1U

/*
 * TEX with its handle in a constant buffer: the handle's word offset
 * (40..53) and buffer (54..58), the second destination pair (64..71, RZ for
 * none), the coordinates' first register (24..31) and the second source
 * (32..39, the level of detail), the dimension (61..63: 1 2D), the
 * channels (72..75), the level of detail mode (87..89: 0 implicit, 1 zero,
 * 2 bias, 3 explicit), the fault predicate (81..83) and the eviction
 * priority (84..86).
 *
 * Values referenced: Mesa 25.3.6 src/nouveau/compiler/nak/sm70_encode.rs,
 * OpTex (lines 2499-2565), set_tex_cb_ref (lines 2351-2357), set_tex_dim
 * (lines 2359-2373), set_tex_lod_mode (lines 2375-2390), set_tex_channel_mask
 * (lines 2460-2466) and set_eviction_priority (lines 2893-2905).
 */
#define NVRTX_SM75_TEX_HANDLE_LOW	40U
#define NVRTX_SM75_TEX_HANDLE_BITS	14U
#define NVRTX_SM75_TEX_BUFFER_LOW	54U
#define NVRTX_SM75_TEX_DST2_LOW		64U
#define NVRTX_SM75_TEX_DIM_LOW		61U
#define NVRTX_SM75_TEX_DIM_2D		1U
#define NVRTX_SM75_TEX_CHANNELS_LOW	72U
#define NVRTX_SM75_TEX_LOD_LOW		87U
#define NVRTX_SM75_TEX_LOD_AUTO		0U
#define NVRTX_SM75_TEX_LOD_ZERO		1U
#define NVRTX_SM75_TEX_LOD_BIAS		2U
#define NVRTX_SM75_TEX_LOD_EXPLICIT	3U

/*
 * A constant texel offset (bit 76, AOFFI): the register after the level of
 * detail (or the second source alone) holds u in bits 3..0, v in 7..4 and r in
 * 11..8, each a signed 4-bit number.  TLD (texel fetch) has the TEX fields with
 * an explicit level of detail mode.
 *
 * Values referenced: Mesa 25.3.6 src/nouveau/compiler/nak/sm70_encode.rs,
 * OpTex and OpTld (lines 2499-2640, AOFFI at bit 76), and
 * src/nouveau/compiler/nak_nir_lower_tex.c, lower_tex, the offsets packed 4
 * bits a component (lines 230-243).
 */
#define NVRTX_SM75_TEX_AOFFI		76U
#define NVRTX_SM75_TEX_OFFSET_BITS	4U

/*
 * The attribute addresses: the position, the point size, the first generic
 * one (16 bytes each), the instance and the vertex numbers.
 *
 * Values referenced: Mesa 25.3.6 src/nouveau/compiler/nak_private.h, enum
 * nak_attr, NAK_ATTR_POINT_SIZE to NAK_ATTR_VERTEX_ID (lines 48-77), and
 * nak_attribute_attr_addr, 16 bytes a generic attribute (lines 93-99);
 * src/nouveau/compiler/nak/ir.rs, VtgIoInfo::mark_attrs, the generic
 * attributes below 0x280 (line 8980).
 */
#define NVRTX_SM75_ATTR_POINT_SIZE	0x06cU
#define NVRTX_SM75_ATTR_POSITION	0x070U
#define NVRTX_SM75_ATTR_POSITION_W	0x07cU
#define NVRTX_SM75_ATTR_GENERIC		0x080U
#define NVRTX_SM75_ATTR_GENERIC_END	0x280U
#define NVRTX_SM75_ATTR_INSTANCE_ID	0x2f8U
#define NVRTX_SM75_ATTR_VERTEX_ID	0x2fcU

/*
 * A pixel program's system inputs: the primitive's number (in the A and B
 * part of the input map), the point sprite's coordinate (in the C part,
 * from 0x2c0), and the face (in no part), each read as an input.
 *
 * Values referenced: Mesa 25.3.6 src/nouveau/compiler/nak_private.h, enum
 * nak_attr (NAK_ATTR_PRIMITIVE_ID, NAK_ATTR_POINT_SPRITE_S,
 * NAK_ATTR_FRONT_FACE); src/nouveau/compiler/nak_nir_lower_fs_inputs.c
 * (lower_fs_input_intrin: the point coordinate interpolated screen-linear,
 * the face loaded constant and tested against zero); src/nouveau/compiler/
 * nak/ir.rs (mark_attrs: the parts of the map by address).
 */
#define NVRTX_SM75_ATTR_PRIMITIVE_ID	0x060U
#define NVRTX_SM75_ATTR_POINT_SPRITE	0x2e0U
#define NVRTX_SM75_ATTR_SYSTEM_C	0x2c0U
#define NVRTX_SM75_ATTR_FRONT_FACE	0x3fcU

/*
 * A geometry program's output of the layer it draws its primitive into
 * (gl_Layer).
 *
 * Value referenced: Mesa 25.0.7 src/nouveau/compiler/nak_private.h, enum
 * nak_attr (NAK_ATTR_RT_ARRAY_INDEX).
 */
#define NVRTX_SM75_ATTR_LAYER		0x064U

/*
 * PIXLD: a value of the pixel; the covered samples' mask is value 1, and
 * its predicate destination is PT.
 *
 * Values referenced: Mesa 25.3.6 src/nouveau/compiler/nak/sm70_encode.rs,
 * OpPixLd (opcode 0x925, the value in 78..81, the predicate destination in
 * 81..84) and src/nouveau/compiler/nak/from_nir.rs
 * (load_sample_mask_in: PIXLD.COVMASK).
 */
#define NVRTX_SM75_OP_PIXLD		0x925U
#define NVRTX_SM75_PIXLD_VALUE_LOW	78U
#define NVRTX_SM75_PIXLD_COVERAGE	1U
#define NVRTX_SM75_PIXLD_PREDICATE_LOW	81U

/*
 * BRA: the offset to the target in words from the next instruction
 * (34..81).
 *
 * Values referenced: Mesa 25.3.6 src/nouveau/compiler/nak/sm70_encode.rs,
 * OpBra (lines 3635-3660), get_rel_offset and set_rel_offset (lines
 * 3530-3544).
 */
#define NVRTX_SM75_BRA_OFFSET_LOW	34U
#define NVRTX_SM75_BRA_OFFSET_BITS	48U

/*
 * The other texture instructions with their handle in a constant buffer, of
 * the same fields as TEX: TLD4 (a gather), TMML (the level of detail a
 * sample would use), TXD (a sample with the derivatives given) and TXQ (the
 * image's size and levels).
 *
 * Values referenced: Mesa 25.0.7 src/nouveau/codegen/nv50_ir_emit_gv100.cpp,
 * CodeEmitterGV100::emitTLD4, emitTMML, emitTXD and emitTXQ (lines
 * 1294-1411: the opcodes of the constant buffer form, 0xb63, 0xb69, 0xb6c
 * and 0xb6f).
 */
#define NVRTX_SM75_OP_TLD4		0xb63U
#define NVRTX_SM75_OP_TMML		0xb69U
#define NVRTX_SM75_OP_TXD		0xb6cU
#define NVRTX_SM75_OP_TXQ		0xb6fU

/*
 * The dimension of a texture instruction (61..63): 1D, 2D, 3D or cube in
 * the low two bits, an array in bit 63.
 *
 * Values referenced: Mesa 25.0.7 src/nouveau/compiler/nak/sm70.rs,
 * set_tex_dim (lines 2266-2280), and
 * src/nouveau/codegen/nv50_ir_emit_gv100.cpp, CodeEmitterGV100::emitTEX
 * (lines 1260-1262).
 */
#define NVRTX_SM75_TEX_DIM_1D		0U
#define NVRTX_SM75_TEX_DIM_3D		2U
#define NVRTX_SM75_TEX_DIM_CUBE		3U
#define NVRTX_SM75_TEX_DIM_ARRAY	4U

/*
 * The modifiers of the texture instructions: the depth compare (78, DC) of
 * TEX and TLD4, a TLD4's offset mode (76..77: 1 a constant offset), its
 * not-evict-first bit (84) and its channel (87..88), and what TXQ asks
 * (62..63: 0 the dimensions and the levels).  TEX and TLD keep the eviction
 * priority of the memory accesses (NVRTX_SM75_MEM_EVICT_*); TMML, TXD and
 * TXQ leave it, and TMML and TXQ the fault predicate, zero.
 *
 * Values referenced: Mesa 25.0.7 src/nouveau/compiler/nak/sm70.rs, OpTex,
 * OpTld4, OpTmml, OpTxd and OpTxq (lines 2313-2492), and
 * src/nouveau/codegen/nv50_ir_emit_gv100.cpp, CodeEmitterGV100::emitTEX to
 * emitTXQ (lines 1223-1411).
 */
#define NVRTX_SM75_TEX_COMPARE		78U
#define NVRTX_SM75_TLD4_OFFSET_LOW	76U
#define NVRTX_SM75_TLD4_OFFSET_CONSTANT	1U
#define NVRTX_SM75_TLD4_KEEP		84U
#define NVRTX_SM75_TLD4_CHANNEL_LOW	87U
#define NVRTX_SM75_TXQ_QUERY_LOW	62U
#define NVRTX_SM75_TXQ_DIMENSIONS	0U

/*
 * The registers a texture instruction reads (zedBSD's reading of them): the
 * first source is up to four consecutive registers from the one in 24..31,
 * the second up to four from the one in 32..39.  TEX, TLD, TLD4 and TMML
 * read the layer of an array first, then the coordinates; their second
 * source holds the level of detail or bias, the constant offset and the
 * depth reference, each when the instruction has it.  TXD reads the
 * coordinates, then the offset (bits 31..16) with the layer (15..0) when it
 * has either; its second source holds each coordinate's two derivatives,
 * x then y.  TXQ reads the level.  A vector of three or four registers starts
 * at a multiple of four, one of two at an even register.
 *
 * Procedure referenced: Mesa 25.0.7
 * src/nouveau/compiler/nak_nir_lower_tex.c (lower_tex and lower_txq: the
 * sources of the hardware after the handle, which the constant buffer form
 * does not pass) and src/nouveau/compiler/nak/assign_regs.rs (the vectors
 * aligned to their size rounded up to a power of two).
 */
#define NVRTX_SM75_TEX_VECTOR		4U

/*
 * The texel offset of a TLD4: eight bits for each of u and v (u lowest),
 * where the other texture instructions take four.
 *
 * Value referenced: Mesa 25.0.7 src/nouveau/compiler/nak_nir_lower_tex.c
 * (lower_tex: bits8 for nir_texop_tg4, line 120).
 */
#define NVRTX_SM75_TLD4_OFFSET_BITS	8U

/*
 * A TMML's reply: the level of detail the derivatives give, relative to the
 * view's base, in its first word as a signed number of 1/256 levels (bits
 * 15..0); the level the sample would access, after the clamps, in its
 * second as an unsigned one.
 *
 * Value referenced: Mesa 25.0.7 src/nouveau/compiler/nak_nir_lower_tex.c
 * (lower_tex: the outputs of nir_texop_lod, lines 231-254).
 */
#define NVRTX_SM75_TMML_FRACTION	256U

/*
 * SHFL with an immediate lane and an immediate clamp (opcode 0xf89): the
 * clamp and segment mask (40..52), the lane (53..57), the mode (58..59: 0 an
 * index, 3 a butterfly), the in-bounds predicate (81..83).  A quad is the
 * segment of 0x1c03 (four lanes, clamped at the last).
 *
 * Values referenced: Mesa 25.0.7 src/nouveau/compiler/nak/sm70.rs, OpShfl
 * (lines 2123-2177), and src/nouveau/compiler/nak/from_nir.rs
 * (nir_intrinsic_ddx: the clamp 0x3 | 0x1c << 8, lines 2124-2131).
 */
#define NVRTX_SM75_OP_SHFL		0xf89U
#define NVRTX_SM75_SHFL_CLAMP_LOW	40U
#define NVRTX_SM75_SHFL_LANE_LOW	53U
#define NVRTX_SM75_SHFL_MODE_LOW	58U
#define NVRTX_SM75_SHFL_INDEX		0U
#define NVRTX_SM75_SHFL_BUTTERFLY	3U
#define NVRTX_SM75_SHFL_QUAD		0x1c03U

/*
 * FSWZADD: one of four operations for each lane of a quad on its first
 * source (24..31) and its second (64..71), the operations in 32..39 with
 * the quad's first lane in the high two bits: 0 the sum, 1 the first source
 * minus the second, 2 the second minus the first.  The x derivative of a
 * quad (a butterfly of 1 in the first source) is 1, 2, 1, 2; the y one (a
 * butterfly of 2) 1, 1, 2, 2.
 *
 * Values referenced: Mesa 25.0.7 src/nouveau/compiler/nak/sm70.rs, OpFSwzAdd
 * (lines 998-1031), src/nouveau/compiler/nak/from_nir.rs (nir_intrinsic_ddx
 * and nir_intrinsic_ddy, lines 2115-2181), and
 * src/nouveau/codegen/nv50_ir_lowering_gm107.cpp,
 * GM107LoweringPass::handleDFDX (lines 223-250).
 */
#define NVRTX_SM75_OP_FSWZADD		0x822U
#define NVRTX_SM75_FSWZADD_OPS_LOW	32U
#define NVRTX_SM75_FSWZADD_SECOND_LOW	64U
#define NVRTX_SM75_FSWZADD_DDX		0x66U
#define NVRTX_SM75_FSWZADD_DDY		0x5aU

/*
 * F2F (an ALU opcode): the source's half (60..61: 1 the high one), the
 * result's size and the source's as for the other conversions (16 bits is
 * 1).  PRMT (an ALU opcode): the bytes of its first source (0..3) and its
 * third (4..7) chosen for each byte of the result by a nibble of the second,
 * the mode (72..74: 0 by index).
 *
 * Values referenced: Mesa 25.0.7 src/nouveau/compiler/nak/sm70.rs, OpF2F
 * (lines 1900-1922) and OpPrmt (lines 2043-2077), and
 * src/nouveau/compiler/nak/from_nir.rs (nir_op_pack_half_2x16_split, the
 * PRMT of bytes 0, 1, 4 and 5, lines 1494-1541).
 */
#define NVRTX_SM75_OP_F2F		0x104U
#define NVRTX_SM75_OP_PRMT		0x016U
#define NVRTX_SM75_F2F_HIGH_LOW		60U
#define NVRTX_SM75_CVT_SIZE_16		1U
#define NVRTX_SM75_PRMT_MODE_LOW	72U
#define NVRTX_SM75_PRMT_LOW_HALVES	0x5410U

/*
 * PRMT selectors of one byte of the first source, the rest of the result
 * the third's byte 0 (RZ's, zero): byte 0 and byte 2.  zedBSD's own, from
 * the selector's nibbles above.
 */
#define NVRTX_SM75_PRMT_BYTE_0		0x4440U
#define NVRTX_SM75_PRMT_BYTE_2		0x4442U

#endif
