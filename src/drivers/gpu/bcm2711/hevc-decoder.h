/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The Raspberry Pi 5's HEVC decoder block: found in the device tree, its
 * clock asked of the firmware, and its two phases started and looked at.
 *
 * Phase 1 runs a command list from memory (the parameters, the slice
 * messages and the bitstream of a picture) and writes the picture's PU
 * and coefficient lists; phase 2 reads them back with the references and
 * writes the picture in the SAND column layout.  Each phase ends with a
 * latched bit of the block's interrupt control word, which the caller
 * polls; nothing here waits.  One picture is in the block at a time, and
 * the caller (the executor's decode, under the controller mutex) is the
 * only user.
 */

#ifndef KERN_DRIVERS_GPU_BCM2711_HEVC_DECODER_H
#define KERN_DRIVERS_GPU_BCM2711_HEVC_DECODER_H

#include <stdbool.h>
#include <stdint.h>

struct drv_fdt;

/* Phase 1's buffers: the command list and the two lists it writes, each a bus address, and their sizes. */
struct bcm2712_hevc_phase1_buffers {
	uint64_t commands;
	uint32_t command_count;
	uint64_t pu;
	uint64_t pu_bytes;
	uint64_t coefficients;
	uint64_t coefficient_bytes;
	uint32_t ctb_rows;
};

/*
 * Phase 2's buffers and words: the lists phase 1 wrote, the output planes
 * and their column strides, the sixteen reference pictures' planes (any
 * valid picture for an entry no slice names), the motion vector stores
 * written and read (0 for none) with their stride, and the picture's
 * CONFIG2, FRAMESIZE, CURRPOC and CTB rows.
 */
struct bcm2712_hevc_phase2_buffers {
	uint64_t pu;
	uint64_t pu_bytes;
	uint64_t coefficients;
	uint64_t coefficient_bytes;
	uint64_t luma;
	uint64_t chroma;
	uint32_t luma_stride;
	uint32_t chroma_stride;
	uint64_t reference_luma[16];
	uint64_t reference_chroma[16];
	uint64_t motion_write;
	uint64_t motion_read;
	uint32_t motion_stride;
	uint32_t config2;
	uint32_t frame_size;
	uint32_t current_poc;
	uint32_t ctb_rows;
};

/* What a phase that ended reports: done, still running, out of PU or coefficient room (phase 1), or failed. */
enum bcm2712_hevc_result {
	BCM2712_HEVC_DONE = 0,
	BCM2712_HEVC_RUNNING,
	BCM2712_HEVC_PU_FULL,
	BCM2712_HEVC_COEFFICIENTS_FULL,
	BCM2712_HEVC_FAILED
};

/* Finds the block in the device tree and maps its registers; ENODEV when the board has none. */
int bcm2712_hevc_decoder_discover(const struct drv_fdt *fdt);

/* Reports whether the block was found and may decode. */
bool bcm2712_hevc_decoder_present(void);

/* Takes or gives back the block's clock: the first user powers it at its highest rate, the last one stops it. */
int bcm2712_hevc_decoder_hold(void);
void bcm2712_hevc_decoder_release(void);

/* Starts phase 1 of a picture. */
void bcm2712_hevc_phase1_start(const struct bcm2712_hevc_phase1_buffers *buffers);

/* Looks at phase 1: RUNNING, DONE, PU_FULL or COEFFICIENTS_FULL (run it again with that list larger), or FAILED. */
enum bcm2712_hevc_result bcm2712_hevc_phase1_poll(void);

/* Starts phase 2 of a picture whose phase 1 is done. */
void bcm2712_hevc_phase2_start(const struct bcm2712_hevc_phase2_buffers *buffers);

/* Looks at phase 2: RUNNING or DONE. */
enum bcm2712_hevc_result bcm2712_hevc_phase2_poll(void);

/* Clears the block's latched completions after a phase was given up. */
void bcm2712_hevc_decoder_clear(void);

#endif
