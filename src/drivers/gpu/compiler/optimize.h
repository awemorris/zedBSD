/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The device-independent optimization of the scalar shader IR.
 *
 * A backend may pass a parsed program through it before lowering: repeated
 * pure operations are shared, operations reaching no output the backend
 * needs are removed, and, when asked, operations on known bits are folded
 * and exact identities removed.  Every output bit stays as the program
 * wrote it.  The backend says which outputs its consumer reads and whether
 * it runs every skippable block; the pass knows nothing else of the target.
 */

#ifndef DRIVERS_GPU_COMPILER_OPTIMIZE_H
#define DRIVERS_GPU_COMPILER_OPTIMIZE_H

#include "ir.h"

#include <stdint.h>

/*
 * The output slots of a program: location * 4 + component for the sixteen
 * user locations, then the four position components.  A store to another
 * location (a builtin such as the point size) is always kept.
 */
#define DRV_GPU_IR_OPTIMIZE_SLOTS 68U
#define DRV_GPU_IR_OPTIMIZE_POSITION 64U

/*
 * What a backend tells the optimization about its target.
 *
 * The caller owns the record; the optimization only reads it.
 */
struct drv_gpu_shader_optimize_options {
	/*
	 * Nonzero when the backend runs every skippable block for every thread
	 * and samples for every channel whatever a texture guard says: the skip
	 * markers and guards are then dropped.  When zero, a program that has
	 * either is left unchanged, since sharing a value across a block that
	 * may be skipped would read what the block did not compute.
	 */
	uint32_t runs_every_block;

	/* Nonzero to fold operations on known bits and remove exact identities. */
	uint32_t simplify;

	/* Nonzero for each output slot the stage's consumer reads; a store to another slot is removed. */
	uint8_t needed[DRV_GPU_IR_OPTIMIZE_SLOTS];
};

int drv_gpu_shader_optimize(const struct drv_gpu_shader_ir *ir, const struct drv_gpu_shader_optimize_options *options, struct drv_gpu_shader_ir *optimized);

#endif /* DRIVERS_GPU_COMPILER_OPTIMIZE_H */
