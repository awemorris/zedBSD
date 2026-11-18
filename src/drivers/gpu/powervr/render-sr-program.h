/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The driver's own programs a render context needs when the firmware
 * switches its geometry work out and back in: the shading cluster
 * programs that store and load the shared registers, the data sequencer
 * programs that start them, the data sequencer programs that store and
 * resume the persistent temporaries, and the task words that name them
 * in the context's static state.
 *
 * This part only computes; it holds no memory, and the same code runs
 * on the build host.
 */

#ifndef KERN_DRIVERS_GPU_POWERVR_RENDER_SR_PROGRAM_H
#define KERN_DRIVERS_GPU_POWERVR_RENDER_SR_PROGRAM_H

#include <stddef.h>
#include <stdint.h>

#include "drivers/gpu/powervr/render-kick-layout.h"

/*
 * The buffers a render context's switch writes: the VDM's resume stream,
 * the TA's state, the persistent temporaries, and the shared registers
 * after a reserved head whose first word holds their count; and the VDM
 * call stack of one level.  Each is aligned to 16 bytes.
 *
 * Values referenced:
 * Mesa 26.2.5 src/imagination/csbgen/rogue/lls.xml
 * (VDM_CONTEXT_RESUME_BUFFER_SIZE 92, TA_STATE_BUFFER_SIZE 484,
 * PDS_PERSISTENT_TEMPS_BUFFER_SIZE 128, USC_SHARED_REGS_BUFFER_SIZE
 * 16384, each _ALIGNMENT 16); src/imagination/include/hwdef/
 * rogue_hw_defs.h (ROGUE_LLS_SHARED_REGS_RESERVE_SIZE 48);
 * src/imagination/vulkan/pvr_arch_job_context.c
 * (PVR_VDM_CALLSTACK_MAX_DEPTH 1, one 64-bit entry each).
 */
#define POWERVR_SWITCH_VDM_BYTES		92U
#define POWERVR_SWITCH_TA_BYTES			484U
#define POWERVR_SWITCH_PERSISTENT_BYTES		128U
#define POWERVR_SWITCH_SHARED_RESERVE		48U
#define POWERVR_SWITCH_SHARED_BYTES		(16384U + POWERVR_SWITCH_SHARED_RESERVE)
#define POWERVR_SWITCH_CALL_STACK_BYTES		8U
#define POWERVR_SWITCH_ALIGN			16U

/*
 * The data sequencer programs' sizes in 32-bit words: the shared
 * register program's data (two 64-bit writes with their control words,
 * then the task word in a block of its own) and code (two writes and
 * the task issue); the persistent temporaries' store (three stores, a
 * wait, the end) and resume (a clear of the stream predicates, three
 * loads, a wait, the end), each with three 64-bit source words in data
 * rounded up to 16 bytes.
 */
#define POWERVR_SHARED_PDS_DATA_WORDS		16U
#define POWERVR_SHARED_PDS_CODE_WORDS		3U
#define POWERVR_PERSISTENT_DATA_WORDS		8U
#define POWERVR_PERSISTENT_STORE_CODE_WORDS	5U
#define POWERVR_PERSISTENT_RESUME_CODE_WORDS	6U

/* The temporaries the shared register programs use, and the most bytes one takes. */
#define POWERVR_SHARED_USC_TEMPORARIES		13U
#define POWERVR_SHARED_USC_BYTES_MAX		512U

/*
 * Where one buffer's programs are, as offsets from the base of the data
 * sequencer's window: the shared register store and load programs' data
 * and code, and the persistent temporaries' store and resume programs
 * (code right after data).
 */
struct powervr_switch_programs {
	uint32_t shared_store_data;
	uint32_t shared_store_code;
	uint32_t shared_load_data;
	uint32_t shared_load_code;
	uint32_t persistent_store_data;
	uint32_t persistent_resume_data;
};

/*
 * Builds the data sequencer program that starts a shared register store
 * or load: it writes the state buffer's address and the address of its
 * registers' part into the task's first four vertex inputs, then issues
 * the shading cluster program at usc_offset in its window.
 */
int powervr_shared_pds_program(uint64_t state, uint64_t usc_offset, unsigned temporaries, uint32_t data[POWERVR_SHARED_PDS_DATA_WORDS], uint32_t code[POWERVR_SHARED_PDS_CODE_WORDS]);

/* Builds the data sequencer program that stores the persistent temporaries to a buffer. */
int powervr_persistent_store_program(uint64_t buffer, uint32_t data[POWERVR_PERSISTENT_DATA_WORDS], uint32_t code[POWERVR_PERSISTENT_STORE_CODE_WORDS]);

/* Builds the data sequencer program that loads the persistent temporaries back from a buffer. */
int powervr_persistent_resume_program(uint64_t buffer, uint32_t data[POWERVR_PERSISTENT_DATA_WORDS], uint32_t code[POWERVR_PERSISTENT_RESUME_CODE_WORDS]);

/*
 * Builds the shading cluster program that stores (loading 0) or loads
 * (loading 1) the task's shared registers through the addresses its
 * vertex inputs hold.
 */
int powervr_shared_usc_program(unsigned loading, uint8_t *output, size_t room, size_t *length);

/*
 * The compute context's switch programs' sizes in 32-bit words: the
 * shared register programs' code (the coefficient pass's skip, then the
 * two writes and the task issue) and the fence terminate program's data
 * and code; and the words of the compute context's static state.
 */
#define POWERVR_COMPUTE_SHARED_CODE_WORDS	6U
#define POWERVR_COMPUTE_TERMINATE_DATA_WORDS	4U
#define POWERVR_COMPUTE_TERMINATE_CODE_WORDS	5U
#define POWERVR_COMPUTE_SWITCH_WORDS		7U

/*
 * Where the compute context's switch programs are, as offsets from the
 * data sequencer window's base: each of the two buffers' store and load
 * programs' data and code, and the fence terminate program's.
 */
struct powervr_compute_switch_programs {
	uint32_t store_data[2];
	uint32_t store_code[2];
	uint32_t load_data[2];
	uint32_t load_code[2];
	uint32_t terminate_data;
	uint32_t terminate_code;
};

/*
 * Builds the data sequencer program that starts a compute context's
 * shared register store or load: the render context's program behind a
 * skip of the coefficient pass, its task issue conditional.
 */
int powervr_compute_shared_pds_program(uint64_t state, uint64_t usc_offset, unsigned temporaries, uint32_t data[POWERVR_SHARED_PDS_DATA_WORDS], uint32_t code[POWERVR_COMPUTE_SHARED_CODE_WORDS]);

/* Builds the data sequencer program that ends a compute context's switch with a compute fence. */
int powervr_compute_terminate_program(uint32_t data[POWERVR_COMPUTE_TERMINATE_DATA_WORDS], uint32_t code[POWERVR_COMPUTE_TERMINATE_CODE_WORDS]);

/*
 * Builds the compute context's static state: context PDS0 and PDS1,
 * terminate PDS and PDS1, resume PDS0, the second buffer's context PDS0
 * and resume PDS0, in that order.
 */
void powervr_compute_switch_words(const struct powervr_compute_switch_programs *programs, uint64_t words[POWERVR_COMPUTE_SWITCH_WORDS]);

/* Builds one buffer's store and resume task words from its programs' places. */
void powervr_switch_task_words(const struct powervr_switch_programs *programs, struct powervr_switch_tasks *tasks);

/* Builds the register value of a VDM or TA context state base from its address. */
uint64_t powervr_switch_base_word(uint64_t address);

/* The compute command's resume control word, its word in the command, for a context with the switch's state. */
#define POWERVR_COMPUTE_RESUME_PDS1_WORD	15U
uint32_t powervr_compute_resume_pds1(uint32_t common_bytes);

#endif
