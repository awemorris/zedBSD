/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The machine encoding of the PowerVR Rogue programmable data sequencer.
 *
 * The data sequencer runs a short program per task before the shading
 * cluster starts: it loads constants into the cluster's stores and then
 * issues the cluster task.  Each instruction is one 32-bit word; the
 * issue instructions take 64-bit and 32-bit data words the driver
 * places in the program's constants, built by the helpers here.
 *
 * Operands are register numbers in the ranges each instruction
 * accepts: 32-bit constants 0 to 127, temporaries from 128 and
 * persistent temporaries from 192 in the 32-bit space; 64-bit constants
 * 0 to 63, temporaries from 64 and persistent ones from 96 in the
 * 64-bit space.  The narrower temporary-only fields count from zero.
 *
 * The PowerVR BXE-4-32 has the double-rate DMA test fields and no
 * system level cache controls in the DMA source word, so the encoder
 * offers the former and never sets the latter.
 */

#ifndef KERN_DRIVERS_GPU_POWERVR_PDS_ENCODE_H
#define KERN_DRIVERS_GPU_POWERVR_PDS_ENCODE_H

#include <stdint.h>

/*
 * The comparisons a data sequencer compare can make.
 *
 * Values referenced:
 * Mesa 26.2.5 src/imagination/vulkan/pds/pvr_rogue_pds_defs.h
 * (PVR_ROGUE_PDSINST_COP_EQ 0, _GT 1, _LT 2, _NE 3).
 */
enum powervr_pds_compare {
	POWERVR_PDS_EQUAL = 0,
	POWERVR_PDS_GREATER = 1,
	POWERVR_PDS_LESS = 2,
	POWERVR_PDS_NOT_EQUAL = 3
};

/*
 * The destinations of a data output.
 *
 * Values referenced:
 * Mesa 26.2.5 src/imagination/vulkan/pds/pvr_rogue_pds_defs.h
 * (PVR_ROGUE_PDSINST_DSTDOUT_DOUTD 0, _DOUTW 1, _DOUTU 2, _DOUTV 3,
 * _DOUTI 4, _DOUTC 5).
 */
enum powervr_pds_output {
	POWERVR_PDS_OUTPUT_DMA = 0,
	POWERVR_PDS_OUTPUT_WRITE = 1,
	POWERVR_PDS_OUTPUT_SHADER_TASK = 2,
	POWERVR_PDS_OUTPUT_VERTEX_FENCE = 3,
	POWERVR_PDS_OUTPUT_ITERATOR = 4,
	POWERVR_PDS_OUTPUT_COMPUTE_FENCE = 5
};

int powervr_pds_add32(unsigned condition, unsigned is_signed, unsigned subtract, unsigned source_0, unsigned source_1, unsigned destination, uint32_t *word);
int powervr_pds_add64(unsigned condition, unsigned is_signed, unsigned subtract, unsigned source_0, unsigned source_1, unsigned destination, uint32_t *word);
int powervr_pds_shift_logic32(unsigned condition, unsigned logic, unsigned source_0, unsigned source_1, unsigned shift_immediate, unsigned shift, unsigned destination, uint32_t *word);
int powervr_pds_compare(unsigned condition, enum powervr_pds_compare compare, unsigned source_0, unsigned source_1, uint32_t *word);
int powervr_pds_compare_immediate(unsigned condition, enum powervr_pds_compare compare, unsigned source_0, unsigned immediate, uint32_t *word);
int powervr_pds_branch(unsigned predicate, unsigned negate, unsigned set_predicate, unsigned address, uint32_t *word);
int powervr_pds_load(unsigned condition, unsigned source, uint32_t *word);
int powervr_pds_store(unsigned condition, unsigned source, uint32_t *word);
int powervr_pds_wait_data(unsigned condition, uint32_t *word);
int powervr_pds_load_immediate(unsigned condition, unsigned destination, unsigned immediate, unsigned global, uint32_t *word);
int powervr_pds_halt(unsigned condition, uint32_t *word);
int powervr_pds_nothing(unsigned condition, uint32_t *word);
int powervr_pds_stream_clear(unsigned condition, unsigned mask, uint32_t *word);
int powervr_pds_dma_add(unsigned condition, unsigned end, unsigned source_0, unsigned source_1, unsigned source_2, unsigned source_3, uint32_t *word);
int powervr_pds_output(unsigned condition, unsigned end, enum powervr_pds_output output, unsigned source_0, unsigned source_1, uint32_t *word);

int powervr_pds_task_word(uint64_t address, unsigned temporaries, unsigned sample_rate, unsigned dual_phase, uint64_t *word);
int powervr_pds_dma_address_word(uint64_t address, unsigned offset, uint64_t *word);
int powervr_pds_dma_control_word(unsigned last, unsigned words, unsigned common_store, unsigned cache_mode, unsigned store_address, unsigned repeat, unsigned size, uint32_t *word);
int powervr_pds_dma_add_control_word(uint32_t control, unsigned test, uint32_t limit, uint64_t *word);
int powervr_pds_write_control_word(unsigned last, unsigned common_store, unsigned cache_mode, unsigned store_address, unsigned size, uint32_t *word);
int powervr_pds_load_word(uint64_t address, unsigned count, unsigned cache_mode, unsigned destination, uint64_t *word);
int powervr_pds_store_word(uint64_t address, unsigned count, unsigned cache_mode, unsigned source, uint64_t *word);

#endif /* KERN_DRIVERS_GPU_POWERVR_PDS_ENCODE_H */
