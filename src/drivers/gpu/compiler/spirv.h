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

/*
 * The fragment shader's SampleMask input builtin (element 0 of the array),
 * read raw like a Flat input.  Only a parse with the GHCI features makes it.
 */
#define DRV_GPU_SHADER_LOCATION_SAMPLE_MASK 70U

/*
 * The fragment shader's SampleId input builtin, the number of the sample
 * the invocation runs for, read raw like a Flat input.  Only a parse with
 * the SAMPLE_ID feature makes it.
 */
#define DRV_GPU_SHADER_LOCATION_SAMPLE_ID 71U

/*
 * What a parse may lower beyond the profile every backend shares, as bits
 * of its `features`: the SampleMask input and output and the FragDepth
 * output builtins, the Centroid decoration recorded on the inputs,
 * OpImageQuerySamples, the storage images, storage texel buffers and
 * input attachments with OpImageRead and OpImageWrite, and images in a
 * compute shader.  A backend asks
 * only for what it lowers; without a bit the parse refuses what it names,
 * as before.
 */
#define DRV_GPU_SPIRV_FEATURE_SAMPLE_MASK 0x1U
#define DRV_GPU_SPIRV_FEATURE_FRAG_DEPTH 0x2U
#define DRV_GPU_SPIRV_FEATURE_CENTROID 0x4U
#define DRV_GPU_SPIRV_FEATURE_QUERY_SAMPLES 0x8U
#define DRV_GPU_SPIRV_FEATURE_STORAGE_IMAGE 0x10U
#define DRV_GPU_SPIRV_FEATURE_INPUT_ATTACHMENT 0x20U

/* A compute shader's sampled and storage images (refused otherwise). */
#define DRV_GPU_SPIRV_FEATURE_COMPUTE_IMAGES 0x40U

/*
 * A fragment shader's SampleId input and FragStencilRefEXT output
 * (SPV_EXT_shader_stencil_export) builtins (refused otherwise).
 */
#define DRV_GPU_SPIRV_FEATURE_SAMPLE_ID 0x80U
#define DRV_GPU_SPIRV_FEATURE_STENCIL_EXPORT 0x100U

/*
 * A module of several functions: the entry function's calls are inlined
 * before the parse (refused otherwise, as more than one function).
 */
#define DRV_GPU_SPIRV_FEATURE_INLINE_CALLS 0x200U

/* The features the GHCI backend lowers. */
#define DRV_GPU_SPIRV_FEATURES_GHCI 0x7FU

int drv_gpu_shader_parse(const uint32_t *words, size_t word_count, enum drv_gpu_shader_stage stage, struct drv_gpu_shader_ir **out, struct drv_gpu_compile_diagnostic *diagnostic);
int drv_gpu_shader_parse_features(const uint32_t *words, size_t word_count, enum drv_gpu_shader_stage stage, uint32_t features, struct drv_gpu_shader_ir **out, struct drv_gpu_compile_diagnostic *diagnostic);
void drv_gpu_shader_ir_free(struct drv_gpu_shader_ir *ir);

#endif /* DRIVERS_GPU_COMPILER_SPIRV_H */
