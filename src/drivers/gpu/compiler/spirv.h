/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The device-independent SPIR-V frontend shared by the GPU backends.
 *
 * Parsing owns no hardware or GPU allocation. The caller selects a stage,
 * receives a complete scalar IR, and lowers it using its vendor backend.
 */

#ifndef DRIVERS_GPU_COMPILER_SPIRV_H
#define DRIVERS_GPU_COMPILER_SPIRV_H

#include "ir.h"

#include <stddef.h>
#include <stdint.h>

/*
 * The synthetic input locations after all user attributes and varyings.
 * Each backend maps them to its own vertex or fragment payload fields.
 */
#define DRV_GPU_SHADER_LOCATION_VERTEX_INDEX 64U
#define DRV_GPU_SHADER_LOCATION_INSTANCE_INDEX 65U
#define DRV_GPU_SHADER_LOCATION_FRONT_FACING 66U
#define DRV_GPU_SHADER_LOCATION_FRAG_COORD 67U
#define DRV_GPU_SHADER_LOCATION_POINT_COORD 68U
#define DRV_GPU_SHADER_LOCATION_PRIMITIVE_ID 69U

/* The current frontend profile retains its original compute admission limit. */
#define DRV_GPU_SPIRV_MAX_GROUP_INVOCATIONS 128U

/*
 * One module refusal, borrowed from the frontend's immutable reason strings.
 * The caller owns this record; successful parses and allocation failures
 * without a refused instruction leave it cleared.
 */
struct drv_gpu_compile_diagnostic {
	uint32_t opcode;
	uint32_t word_offset;
	const char *reason;
};

int drv_gpu_shader_parse(const uint32_t *words, size_t word_count, enum drv_gpu_shader_stage stage, struct drv_gpu_shader_ir **out, struct drv_gpu_compile_diagnostic *diagnostic);
void drv_gpu_shader_ir_free(struct drv_gpu_shader_ir *ir);

#endif /* DRIVERS_GPU_COMPILER_SPIRV_H */
