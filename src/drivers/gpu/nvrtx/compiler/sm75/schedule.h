/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The dependence marking of the nvrtx shader compiler (operands.c and
 * schedule.c).
 *
 * After the lowering has emitted a program, this pass sets every
 * instruction's scheduling control (isa.h, NVRTX_SM75_DELAY_LOW ..):
 * a fixed-latency result holds its reader back only as many cycles as
 * the shader model's latency table says for that writer and reader
 * (Turing's, or the one Ampere and Ada share), and a variable-latency
 * instruction releases a scoreboard of its own that only the
 * instructions that need its result (or its sources' registers) wait
 * for.  Branches, convergence barriers, barriers, discards, EXIT and
 * the targets of branches start a new block with every result
 * complete.
 *
 * The pass reads the instructions back from their words: it knows the
 * operands of every instruction the lowering emits and refuses any other.
 */

#ifndef DRIVERS_GPU_NVRTX_COMPILER_SM75_SCHEDULE_H
#define DRIVERS_GPU_NVRTX_COMPILER_SM75_SCHEDULE_H

#include "drivers/gpu/nvrtx/compiler/sm75/isa.h"

#include <stdint.h>

/*
 * How an instruction's results become ready (Turing's latency classes).
 * The numbers are zedBSD's own.
 *
 * Values referenced: Mesa 25.3.6
 * src/nouveau/compiler/nak/sm75_instr_latencies.rs, enum RegLatencySM75
 * (lines 18-38: CoupledAlu, CoupledFMA, IMADLo, IMADWideLower and
 * IMADWideUpper, CoupledDisp, Decoupled).
 */
#define NVRTX_SM75_CLASS_ALU		0U
#define NVRTX_SM75_CLASS_FMA		1U
#define NVRTX_SM75_CLASS_IMAD		2U
#define NVRTX_SM75_CLASS_IMAD_WIDE	3U
#define NVRTX_SM75_CLASS_DISPATCH	4U
#define NVRTX_SM75_CLASS_DECOUPLED	5U

/*
 * A read by a variable-latency instruction of the address unit (the global
 * and shared memory accesses and the attribute stores), which Ampere's and
 * Ada's table tells from the other variable-latency reads; only a read
 * has it, the instruction's class is NVRTX_SM75_CLASS_DECOUPLED.
 *
 * Value referenced: Mesa 25.3.6
 * src/nouveau/compiler/nak/sm80_instr_latencies.rs, enum RegLatencySM80
 * (DecoupledAgu, line 41).
 */
#define NVRTX_SM75_CLASS_AGU		6U

/* The most registers and predicates one instruction reads and writes. */
#define NVRTX_SM75_OPERANDS_READS	8U
#define NVRTX_SM75_OPERANDS_WRITES	4U

/*
 * The fewest cycles after a barrier or a memory barrier (its execution takes 5
 * on Turing, 6 from SM80 on), and after EXIT.
 *
 * Values referenced: Mesa 25.3.6 src/nouveau/compiler/nak/sm70.rs,
 * exec_latency (lines 169-185), and
 * src/nouveau/compiler/nak/calc_instr_deps.rs, calc_delays (lines 811-818: a
 * NOP of 2 cycles after an instruction with an execution latency).
 *
 * Value referenced: the SM75 code the SASS compiler emits (EXIT's delay of
 * 5).
 */
#define NVRTX_SM75_DELAY_SYNC_MIN	6U
#define NVRTX_SM75_DELAY_EXIT_MIN	5U
#define NVRTX_SM75_DELAY_AFTER_SYNC	2U

/* No scoreboard, no predicate, and a register never written. */
#define NVRTX_SM75_NO_BAR		0xffU
#define NVRTX_SM75_NO_PREDICATE		NVRTX_SM75_PT

/*
 * What one instruction reads and writes, as the dependence marking sees
 * it: the general registers (RZ left out) with, for each read, the class
 * the read counts as (an IMAD.WIDE's addend reads late); the guard
 * predicate and the predicate sources (PT left out); the predicate it
 * writes; its latency class; whether it ends a block; and the fewest
 * cycles before the next instruction may issue.
 */
struct nvrtx_sm75_operands {
	uint32_t reads[NVRTX_SM75_OPERANDS_READS];
	uint32_t read_class[NVRTX_SM75_OPERANDS_READS];
	uint32_t read_count;
	uint32_t writes[NVRTX_SM75_OPERANDS_WRITES];
	uint32_t write_count;
	uint32_t guard;
	uint32_t predicate_read;
	uint32_t predicate_write;
	uint32_t class_of;
	int boundary;
	uint32_t min_delay;
};

/* Reads an instruction's operands from its four words; nonzero for an instruction the pass does not know. */
int nvrtx_sm75_operands(const uint32_t *words, struct nvrtx_sm75_operands *operands);

/*
 * Reorders the instructions inside each block of a program for a shader
 * model (NVRTX_SM_*) so that the long-latency ones issue early; nonzero,
 * with the instruction and a reason, when one is not known or there is no
 * memory.  Branches, convergence and barriers, NOPs and the blocks' bounds
 * keep their places, so every branch offset stays valid.
 */
int nvrtx_sm75_reorder(uint32_t *code, uint32_t count, uint32_t sm, uint32_t *instruction, const char **reason);

/*
 * Sets the scheduling control of a program's instructions for a shader
 * model (NVRTX_SM_*); nonzero, with the instruction and a reason, when one
 * is not known or a delay would not fit its field.
 */
int nvrtx_sm75_schedule(uint32_t *code, uint32_t count, uint32_t sm, uint32_t *instruction, const char **reason);

#endif
