/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The encoder of Turing (SM75) instructions (encode.c).
 *
 * Each function fills one 128-bit instruction from operands that are
 * already physical registers, immediates or constant buffer words.  An
 * instruction starts with nvrtx_sm75_begin() (zero, guarded by PT, no
 * scoreboard), gets its operation from one encode function, and may then
 * get a guard predicate and its scheduling control.  Nothing here allocates
 * or fails: the caller checks its operands first.  The field positions are
 * in isa.h.
 */

#ifndef DRIVERS_GPU_NVRTX_COMPILER_SM75_ENCODE_H
#define DRIVERS_GPU_NVRTX_COMPILER_SM75_ENCODE_H

#include "drivers/gpu/nvrtx/compiler/sm75/isa.h"

#include <stdint.h>

/* What an ALU source operand is. */
#define NVRTX_SM75_SRC_NONE	0
#define NVRTX_SM75_SRC_REG	1
#define NVRTX_SM75_SRC_IMM	2
#define NVRTX_SM75_SRC_CB	3

/*
 * One instruction: four words, the first holding bits 0..31.
 *
 * The caller owns it; it is copied into the program's code when complete.
 */
struct nvrtx_sm75_word {
	uint32_t w[4];
};

/*
 * One source operand of an ALU instruction.
 *
 * value is the register number (RZ for zero), the immediate's bits, or the
 * byte offset in constant buffer cb_index.  neg and abs are the source
 * modifiers the operand's slot has.
 */
struct nvrtx_sm75_src {
	int kind;
	uint32_t value;
	uint32_t cb_index;
	int neg;
	int abs;
};

/*
 * ==== Fields ====
 */

/* Writes a field of up to 64 bits at a bit position. */
void nvrtx_sm75_set(struct nvrtx_sm75_word *word, unsigned low, unsigned bits, uint64_t value);

/* Reads a field of up to 64 bits at a bit position. */
uint64_t nvrtx_sm75_get(const struct nvrtx_sm75_word *word, unsigned low, unsigned bits);

/* Starts an instruction: all bits zero, guarded by PT, no scoreboard. */
void nvrtx_sm75_begin(struct nvrtx_sm75_word *word);

/* Guards an instruction by a predicate, or its negation. */
void nvrtx_sm75_guard(struct nvrtx_sm75_word *word, unsigned predicate, int negate);

/* Sets the scheduling control: the delay, the yield, the scoreboards written and read (7: none), and the ones waited for. */
void nvrtx_sm75_control(struct nvrtx_sm75_word *word, unsigned delay, int yield, unsigned write_bar, unsigned read_bar, unsigned wait_mask);

/* Makes a register, an immediate or a constant buffer source. */
struct nvrtx_sm75_src nvrtx_sm75_reg(uint32_t reg);
struct nvrtx_sm75_src nvrtx_sm75_imm(uint32_t bits);
struct nvrtx_sm75_src nvrtx_sm75_cb(uint32_t index, uint32_t offset);

/*
 * ==== Instructions ====
 */

/* The ALU form shared by most arithmetic: dst, then sources 0, 1 and 2 placed by the form they make. */
void nvrtx_sm75_alu(struct nvrtx_sm75_word *word, uint32_t opcode, uint32_t dst, const struct nvrtx_sm75_src *src0, const struct nvrtx_sm75_src *src1, const struct nvrtx_sm75_src *src2);

/* MOV dst, src. */
void nvrtx_sm75_mov(struct nvrtx_sm75_word *word, uint32_t dst, const struct nvrtx_sm75_src *src);

/* FADD, FMUL and FFMA (round to nearest, denormals kept). */
void nvrtx_sm75_fadd(struct nvrtx_sm75_word *word, uint32_t dst, const struct nvrtx_sm75_src *a, const struct nvrtx_sm75_src *b);
void nvrtx_sm75_fmul(struct nvrtx_sm75_word *word, uint32_t dst, const struct nvrtx_sm75_src *a, const struct nvrtx_sm75_src *b);
void nvrtx_sm75_ffma(struct nvrtx_sm75_word *word, uint32_t dst, const struct nvrtx_sm75_src *a, const struct nvrtx_sm75_src *b, const struct nvrtx_sm75_src *c);

/* FMNMX: the smaller of a and b when min, else the larger. */
void nvrtx_sm75_fmnmx(struct nvrtx_sm75_word *word, uint32_t dst, const struct nvrtx_sm75_src *a, const struct nvrtx_sm75_src *b, int min);

/* FSETP and ISETP: a predicate from a comparison (NVRTX_SM75_FCMP_*, NVRTX_SM75_ICMP_*), ANDed with PT. */
void nvrtx_sm75_fsetp(struct nvrtx_sm75_word *word, uint32_t predicate, uint32_t compare, const struct nvrtx_sm75_src *a, const struct nvrtx_sm75_src *b);
void nvrtx_sm75_isetp(struct nvrtx_sm75_word *word, uint32_t predicate, uint32_t compare, int is_signed, const struct nvrtx_sm75_src *a, const struct nvrtx_sm75_src *b);

/* IADD3 dst = a + b + c (no carries). */
void nvrtx_sm75_iadd3(struct nvrtx_sm75_word *word, uint32_t dst, const struct nvrtx_sm75_src *a, const struct nvrtx_sm75_src *b, const struct nvrtx_sm75_src *c);

/* IMAD dst = a * b + c, the low 32 bits; IMAD.WIDE.U32 the 64-bit dst pair = a * b + the c pair. */
void nvrtx_sm75_imad(struct nvrtx_sm75_word *word, uint32_t dst, const struct nvrtx_sm75_src *a, const struct nvrtx_sm75_src *b, const struct nvrtx_sm75_src *c, int is_signed);
void nvrtx_sm75_imad_wide(struct nvrtx_sm75_word *word, uint32_t dst, const struct nvrtx_sm75_src *a, const struct nvrtx_sm75_src *b, const struct nvrtx_sm75_src *c);

/* LOP3 dst = lut(a, b, c). */
void nvrtx_sm75_lop3(struct nvrtx_sm75_word *word, uint32_t dst, const struct nvrtx_sm75_src *a, const struct nvrtx_sm75_src *b, const struct nvrtx_sm75_src *c, uint32_t lut);

/* SHF: a 32-bit shift of value by count, left or right (logical or arithmetic), the count taken modulo 32. */
void nvrtx_sm75_shift(struct nvrtx_sm75_word *word, uint32_t dst, uint32_t value, const struct nvrtx_sm75_src *count, int right, int arithmetic);

/* SEL dst = predicate (or its negation) ? a : b. */
void nvrtx_sm75_sel(struct nvrtx_sm75_word *word, uint32_t dst, const struct nvrtx_sm75_src *a, const struct nvrtx_sm75_src *b, uint32_t predicate, int negate);

/* MUFU dst = function(src) (NVRTX_SM75_MUFU_*). */
void nvrtx_sm75_mufu(struct nvrtx_sm75_word *word, uint32_t dst, uint32_t function, uint32_t src);

/* I2F, F2I and FRND between 32-bit values, with a rounding (NVRTX_SM75_ROUND_*). */
void nvrtx_sm75_i2f(struct nvrtx_sm75_word *word, uint32_t dst, uint32_t src, int is_signed);
void nvrtx_sm75_f2i(struct nvrtx_sm75_word *word, uint32_t dst, uint32_t src, int is_signed);
void nvrtx_sm75_frnd(struct nvrtx_sm75_word *word, uint32_t dst, uint32_t src, uint32_t rounding);

/* S2R dst = a special register (NVRTX_SM75_SR_*). */
void nvrtx_sm75_s2r(struct nvrtx_sm75_word *word, uint32_t dst, uint32_t special);
void nvrtx_sm75_pixld(struct nvrtx_sm75_word *word, uint32_t dst, uint32_t value);

/* LDC dst = the 32- or 64-bit word at offset in constant buffer index (a 64-bit result fills an even register pair). */
void nvrtx_sm75_ldc(struct nvrtx_sm75_word *word, uint32_t dst, uint32_t index, uint32_t offset, int wide);

/* LDG.E and STG.E of a 32-bit word at the 64-bit address in a register pair. */
void nvrtx_sm75_ldg(struct nvrtx_sm75_word *word, uint32_t dst, uint32_t address);
void nvrtx_sm75_stg(struct nvrtx_sm75_word *word, uint32_t address, uint32_t data);

/*
 * Sets a global access's memory order (strong at the GPU's scope) in the
 * form of a shader model: SM75's scope and order fields, or from SM80 on
 * their one field.  The global encoders above write SM75's form.
 */
void nvrtx_sm75_memory_order(struct nvrtx_sm75_word *word, uint32_t sm);

/* LDS and STS of a 32-bit word at a byte offset of the workgroup's shared memory in a register. */
void nvrtx_sm75_lds(struct nvrtx_sm75_word *word, uint32_t dst, uint32_t offset);
void nvrtx_sm75_sts(struct nvrtx_sm75_word *word, uint32_t offset, uint32_t data);

/* An atomic on a 32-bit word: global (the address a register pair) or shared (the offset a register), returning the old value. */
void nvrtx_sm75_atomic(struct nvrtx_sm75_word *word, int shared, uint32_t dst, uint32_t address, uint32_t data, uint32_t operation, int is_signed);

/* A compare and exchange: where the word equals compare, data is written; the old value is returned. */
void nvrtx_sm75_atomic_cas(struct nvrtx_sm75_word *word, int shared, uint32_t dst, uint32_t address, uint32_t compare, uint32_t data);

/* MEMBAR of a scope (NVRTX_SM75_MEMBAR_*), and BAR.SYNC of barrier 0 for the whole workgroup. */
void nvrtx_sm75_membar(struct nvrtx_sm75_word *word, uint32_t scope);
void nvrtx_sm75_bar(struct nvrtx_sm75_word *word);

/* BSSY: the convergence barrier collects the threads at the BSYNC so many instructions after the next one; BSYNC waits for them. */
void nvrtx_sm75_bssy(struct nvrtx_sm75_word *word, uint32_t barrier, int32_t instructions);
void nvrtx_sm75_bsync(struct nvrtx_sm75_word *word, uint32_t barrier);

/* ALD dst = one word of the vertex's input attribute at an address; AST: one word of the output attribute. */
void nvrtx_sm75_ald(struct nvrtx_sm75_word *word, uint32_t dst, uint32_t address);
void nvrtx_sm75_ast(struct nvrtx_sm75_word *word, uint32_t address, uint32_t data);

/* ALD of an input vertex's attribute word (the vertex's handle a register) and AST of the output vertex's (its handle a register). */
void nvrtx_sm75_ald_vertex(struct nvrtx_sm75_word *word, uint32_t dst, uint32_t address, uint32_t vertex);
void nvrtx_sm75_ast_vertex(struct nvrtx_sm75_word *word, uint32_t address, uint32_t data, uint32_t vertex);

/* ISBERD dst = the attribute handle of an input vertex index; OUT dst = the handle after emitting, cutting or ending at a handle. */
void nvrtx_sm75_isberd(struct nvrtx_sm75_word *word, uint32_t dst, uint32_t index);
void nvrtx_sm75_out(struct nvrtx_sm75_word *word, uint32_t dst, uint32_t handle, uint32_t type);

/* IPA dst = the fragment's interpolation of the attribute at an address: passed (pass) or the provoking vertex's (constant). */
void nvrtx_sm75_ipa(struct nvrtx_sm75_word *word, uint32_t dst, uint32_t address, uint32_t frequency);

/*
 * TEX.2D of the texture whose handle is the word at offset of constant
 * buffer index, at the coordinates in the pair from coordinates: the four
 * channels into the pairs first and second, with a level of detail mode and
 * its register (RZ for none).
 */
void nvrtx_sm75_tex(struct nvrtx_sm75_word *word, uint32_t first, uint32_t second, uint32_t coordinates, uint32_t lod, uint32_t lod_mode, uint32_t index, uint32_t offset);

/* Makes a TEX a TLD (a texel fetch at integer coordinates, an explicit level of detail mode). */
void nvrtx_sm75_tex_fetch(struct nvrtx_sm75_word *word);

/* Makes a TEX or a TLD take a constant texel offset from the register after its level of detail (AOFFI). */
void nvrtx_sm75_tex_offset(struct nvrtx_sm75_word *word);

/*
 * A texture instruction with its handle in a constant buffer (TEX, TLD,
 * TLD4, TMML, TXD or TXQ): channels into first and second (RZ for none),
 * from the source vectors src0 and src1 (RZ for none), of a dimension
 * (NVRTX_SM75_TEX_DIM_*), the handle the word at offset of constant buffer
 * index.
 */
void nvrtx_sm75_texture(struct nvrtx_sm75_word *word, uint32_t opcode, uint32_t first, uint32_t second, uint32_t src0, uint32_t src1, uint32_t dim, uint32_t channels, uint32_t index, uint32_t offset);

/* Sets a TEX's or a TLD's level of detail mode, makes a TEX or a TLD4 compare (DC), sets a TLD4's channel. */
void nvrtx_sm75_tex_lod(struct nvrtx_sm75_word *word, uint32_t mode);
void nvrtx_sm75_tex_compare(struct nvrtx_sm75_word *word);
void nvrtx_sm75_tld_multisample(struct nvrtx_sm75_word *word);
void nvrtx_sm75_tld4_channel(struct nvrtx_sm75_word *word, uint32_t channel);

/* SHFL dst = src of the quad's lane by index or butterfly (NVRTX_SM75_SHFL_*). */
void nvrtx_sm75_shfl(struct nvrtx_sm75_word *word, uint32_t dst, uint32_t src, uint32_t lane, uint32_t mode);

/* FSWZADD dst = each quad lane's operation of a and b (ops, NVRTX_SM75_FSWZADD_*). */
void nvrtx_sm75_fswzadd(struct nvrtx_sm75_word *word, uint32_t dst, uint32_t a, uint32_t b, uint32_t ops);

/* F2F dst = src to a 16-bit float (to_half) or from the low or high half (high) of src. */
void nvrtx_sm75_f2f(struct nvrtx_sm75_word *word, uint32_t dst, uint32_t src, int to_half, int high);

/* PRMT dst = the bytes of a and b a selector names. */
void nvrtx_sm75_prmt(struct nvrtx_sm75_word *word, uint32_t dst, uint32_t a, uint32_t selector, uint32_t b);

/* KILL of the fragment (under the instruction's guard). */
void nvrtx_sm75_kill(struct nvrtx_sm75_word *word);

/* Makes a BRA conditional: it branches where the predicate (or its negation) holds. */
void nvrtx_sm75_bra_if(struct nvrtx_sm75_word *word, uint32_t predicate, int negate);

/* EXIT, NOP, and BRA by a signed number of instructions from the next one. */
void nvrtx_sm75_exit(struct nvrtx_sm75_word *word);
void nvrtx_sm75_nop(struct nvrtx_sm75_word *word);
void nvrtx_sm75_bra(struct nvrtx_sm75_word *word, int32_t instructions);

#endif
