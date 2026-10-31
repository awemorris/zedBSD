/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The lowering of the nvrtx shader compiler (see sm75.h).
 *
 * One pass over the scalar IR chooses the SM75 instructions of each
 * IR instruction and assigns registers as it goes: a value gets a
 * register when it is defined and gives it back after the last
 * instruction that reads it; a value read inside a loop it was defined
 * before lives to the loop's end.  Addresses take an even pair.  A Boolean of the IR is a register of
 * all ones or zero; a comparison sets P0 and turns it into a Boolean
 * at once, and a selection or a guarded access turns the Boolean back
 * into P0.
 *
 * The dependences are marked when the program is complete (the
 * dependence marking, schedule.h): each read waits only as long as the shader model's
 * latency table says for its writer, or for the scoreboard of a
 * variable-latency writer.  The lowering says of each instruction it
 * emits whether it is of fixed or variable latency or ends a block,
 * and the marking's reading of the instruction must agree.
 *
 * A global access gets the shader model's memory order field when it
 * is emitted; nothing else the lowering chooses depends on the shader
 * model.
 */

#ifndef DRIVERS_GPU_NVRTX_COMPILER_SM75_LOWER_H
#define DRIVERS_GPU_NVRTX_COMPILER_SM75_LOWER_H

#include "drivers/gpu/nvrtx/compiler/sm75/encode.h"
#include "drivers/gpu/nvrtx/compiler/sm75/sm75.h"

#include <stdint.h>

/* A value without a register. */
#define NVRTX_SM75_NO_REG		0xffffU

/* The most values one IR instruction reads (a texture message's parameters). */
#define NVRTX_SM75_MAX_SOURCES		DRV_GPU_IR_TEXTURE_MAX_PARAMS

/* What kind of instruction the lowering emits (checked against the dependence marking's reading of it). */
#define NVRTX_SM75_FIXED		0
#define NVRTX_SM75_FIXED_PREDICATE	1
#define NVRTX_SM75_VARIABLE_WRITE	2
#define NVRTX_SM75_VARIABLE_STORE	3
#define NVRTX_SM75_VARIABLE_SPECIAL	4
#define NVRTX_SM75_CONTROL		5
#define NVRTX_SM75_SYNC			6

/* The deepest loop nest (one convergence barrier each), and a value not yet defined. */
#define NVRTX_SM75_LOOP_DEPTH		16U
#define NVRTX_SM75_NOT_DEFINED		0xffffffffU

/*
 * The truth tables of LOP3 used: a and b, a or b, a xor b, not a.
 *
 * Values referenced: Mesa 25.3.6 src/nouveau/compiler/nak/ir.rs
 * (LogicOp3::SRC_MASKS and LogicOp3::new_lut: the sources as 0xf0, 0xcc
 * and 0xaa).
 */
#define NVRTX_SM75_LUT_AND		0xc0U
#define NVRTX_SM75_LUT_OR		0xfcU
#define NVRTX_SM75_LUT_XOR		0x3cU
#define NVRTX_SM75_LUT_NOT		0x0fU

/*
 * The colour targets a fragment program writes.
 *
 * Value referenced: Mesa 25.3.6
 * src/nouveau/headers/nvidia/classes/cla097sph.h, NVA097_SPHV3T2_OMAP_RED(i)
 * to NVA097_SPHV3T2_OMAP_ALPHA(i) (bits 576..607, eight targets), and
 * src/nouveau/compiler/nak/sph.rs (omap_target).
 */
#define NVRTX_SM75_COLOR_TARGETS	8U

/* What a uniform of the IR maps to: no slot. */
#define NVRTX_SM75_NO_SLOT		0xffffffffU

/*
 * What a value needs instead of a register of its own: nothing (it has one),
 * its defining word read in place as an operand (a constant, a push constant
 * or a uniform buffer word, the one immediate or constant buffer operand of
 * the arithmetic that reads it), or nothing at all because it is a product
 * FFMA computes in the one addition that reads it.
 */
#define NVRTX_SM75_FOLD_NONE		0U
#define NVRTX_SM75_FOLD_OPERAND		1U
#define NVRTX_SM75_FOLD_PRODUCT		2U

/*
 * The state of one compilation.
 *
 * Lives on the stack of nvrtx_sm75_compile_ir(); its arrays are allocated
 * there and freed before it returns.
 */
struct nvrtx_sm75_lowering {
	/* The shader model, the program, the result being built, and where a refusal is described. */
	uint32_t sm;
	const struct drv_gpu_shader_ir *ir;
	struct nvrtx_sm75_binary *binary;
	struct nvrtx_sm75_diagnostic *diagnostic;

	/* Each value's register (NVRTX_SM75_NO_REG while it has none), and the IR instruction that last reads it, plus one (0: none). */
	uint16_t *reg;
	uint32_t *last_use;

	/*
	 * Which registers are taken, one past the highest ever taken, and when
	 * each was given back (the instructions emitted by then, plus one; 0 for
	 * never taken).
	 */
	uint8_t taken[NVRTX_SM75_GPR_USABLE];
	uint32_t highest;
	uint32_t freed_at[NVRTX_SM75_GPR_USABLE];

	/* Each uniform's storage slot or constant buffer, NVRTX_SM75_NO_SLOT for neither. */
	uint32_t *slot;

	/* Each value's fold (NVRTX_SM75_FOLD_*), and the IR instruction that defines a folded one. */
	uint8_t *fold;
	uint32_t *def;

	/* The code: room in instructions and instructions emitted. */
	uint32_t capacity;
	uint32_t count;

	/* The IR instruction being lowered. */
	uint32_t index;

	/* A fragment program: each colour location's first output register (NVRTX_SM75_NO_REG when not written), and the register of 1 / w. */
	uint32_t color_reg[NVRTX_SM75_COLOR_TARGETS];
	uint32_t inverse_w;

	/* A vertex program: the attribute words its stores reach, in words (the header's store request). */
	uint32_t store_first;
	uint32_t store_last;

	/* The loops being lowered: each one's BSSY and first instruction, innermost last. */
	uint32_t loop_bssy[NVRTX_SM75_LOOP_DEPTH];
	uint32_t loop_start[NVRTX_SM75_LOOP_DEPTH];
	uint32_t loop_depth;
};

/*
 * ==== The entry and the program (compile.c) ====
 */

/* Records a refusal of the current instruction and reports its error. */
int nvrtx_sm75_refuse(struct nvrtx_sm75_lowering *lowering, int error, const char *reason);

/* Appends an instruction of a kind (NVRTX_SM75_FIXED ..), checked against the dependence marking's reading of it. */
int nvrtx_sm75_emit(struct nvrtx_sm75_lowering *lowering, struct nvrtx_sm75_word *word, int kind);

/*
 * ==== The register allocation (registers.c) ====
 */

/* Lists the values an instruction reads; reports their number, or -1 for an operation the compiler does not cover. */
int nvrtx_sm75_sources(const struct drv_gpu_shader_ir_inst *instruction, uint32_t *sources);

/* Reports nonzero when an instruction defines its destination value. */
int nvrtx_sm75_defines(const struct drv_gpu_shader_ir_inst *instruction);

/* Makes every value a loop reads but that was made before the loop live to the loop's end. */
int nvrtx_sm75_extend_loops(struct nvrtx_sm75_lowering *lowering);

/* Takes a free register, the one given back longest ago. */
int nvrtx_sm75_take(struct nvrtx_sm75_lowering *lowering, uint32_t *reg);

/* Takes an even register whose odd neighbour is free too. */
int nvrtx_sm75_take_pair(struct nvrtx_sm75_lowering *lowering, uint32_t *reg);

/* Gives a register back. */
void nvrtx_sm75_give(struct nvrtx_sm75_lowering *lowering, uint32_t reg);

/* Gives back the registers of the values an instruction read for the last time, and of a result nothing reads. */
void nvrtx_sm75_retire(struct nvrtx_sm75_lowering *lowering, const struct drv_gpu_shader_ir_inst *instruction);

/* Reports a value's register as a source operand. */
struct nvrtx_sm75_src nvrtx_sm75_value(const struct nvrtx_sm75_lowering *lowering, uint32_t value);

/* Chooses the values read in place as operands and the products fused into the addition that reads them. */
int nvrtx_sm75_plan_folds(struct nvrtx_sm75_lowering *lowering);

/* Reports a value as a source operand: its immediate or constant buffer word when it is read in place, else its register. */
struct nvrtx_sm75_src nvrtx_sm75_operand(const struct nvrtx_sm75_lowering *lowering, uint32_t value);

/*
 * ==== The arithmetic, the comparisons, the built-in values and the loops (lower.c) ====
 */

/* Lowers one IR instruction into dst (RZ when it has no result). */
int nvrtx_sm75_lower(struct nvrtx_sm75_lowering *lowering, const struct drv_gpu_shader_ir_inst *instruction, uint32_t dst);

/* Sets P0 from a Boolean value: it is not zero. */
int nvrtx_sm75_predicate(struct nvrtx_sm75_lowering *lowering, uint32_t value);

/*
 * ==== The integer division and remainder (lower-divide.c) ====
 */

/* Lowers the integer division and remainder, signed and unsigned. */
int nvrtx_sm75_lower_divide(struct nvrtx_sm75_lowering *lowering, const struct drv_gpu_shader_ir_inst *instruction, uint32_t dst);

/*
 * ==== The memory accesses and the synchronization (lower-memory.c) ====
 */

/* Lowers a storage buffer load or store, or the buffer's size. */
int nvrtx_sm75_lower_storage(struct nvrtx_sm75_lowering *lowering, const struct drv_gpu_shader_ir_inst *instruction, uint32_t dst);

/* Lowers an atomic on a storage buffer word or a shared memory word, returning the old value. */
int nvrtx_sm75_lower_atomic(struct nvrtx_sm75_lowering *lowering, const struct drv_gpu_shader_ir_inst *instruction, uint32_t dst);

/* Lowers a load or a store of a shared memory word. */
int nvrtx_sm75_lower_shared(struct nvrtx_sm75_lowering *lowering, const struct drv_gpu_shader_ir_inst *instruction, uint32_t dst);

/* Lowers a workgroup barrier and a memory fence. */
int nvrtx_sm75_lower_sync(struct nvrtx_sm75_lowering *lowering, const struct drv_gpu_shader_ir_inst *instruction);

/*
 * ==== The vertex and fragment programs (lower-graphics.c) ====
 */

/* Prepares a graphics program: a fragment program's colour output registers, a vertex program's empty store request. */
int nvrtx_sm75_prepare_graphics(struct nvrtx_sm75_lowering *lowering);

/* Starts a fragment program that interpolates in perspective with w and its reciprocal. */
int nvrtx_sm75_prologue(struct nvrtx_sm75_lowering *lowering);

/* Lowers an input: a vertex program's attribute word, vertex or instance number; a fragment program's interpolated word or position. */
int nvrtx_sm75_lower_input(struct nvrtx_sm75_lowering *lowering, const struct drv_gpu_shader_ir_inst *instruction, uint32_t dst);

/* Lowers an output: a vertex program's attribute word, a fragment program's colour word. */
int nvrtx_sm75_lower_output(struct nvrtx_sm75_lowering *lowering, const struct drv_gpu_shader_ir_inst *instruction);

/* Lowers a 2D texture sample or fetch. */
int nvrtx_sm75_lower_sample(struct nvrtx_sm75_lowering *lowering, const struct drv_gpu_shader_ir_inst *instruction);

/* Lowers a discard of the fragments where the Boolean holds. */
int nvrtx_sm75_lower_kill(struct nvrtx_sm75_lowering *lowering, const struct drv_gpu_shader_ir_inst *instruction);

/* Completes a graphics program's header (version 4). */
void nvrtx_sm75_header(struct nvrtx_sm75_lowering *lowering);

/* Reports nonzero for the texture operations, which define four results. */
int nvrtx_sm75_is_sample(enum drv_gpu_shader_ir_op op);

#endif
