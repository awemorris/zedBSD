/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * What the amdrdna driver writes for a compiled program: the values of the
 * shader registers a program needs, and the buffer descriptors of its
 * resource table.
 */

#ifndef DRIVERS_GPU_AMDRDNA_COMPILER_GFX10_PROGRAM_H
#define DRIVERS_GPU_AMDRDNA_COMPILER_GFX10_PROGRAM_H

#include "gfx10-compile.h"

#include <stdint.h>

/*
 * The formats a vertex buffer descriptor can fetch: one to four 32-bit
 * floats, one to four 32-bit unsigned integers, and four 8-bit unsigned
 * normalized bytes.
 *
 * Values referenced: Mesa 25.0.7 src/amd/registers/gfx10-rsrc.json
 * (the enum GFX10_FORMAT: 32_UINT 20, 32_FLOAT 22, 8_8_8_8_UNORM 56,
 * 32_32_UINT 62, 32_32_FLOAT 64, 32_32_32_UINT 72, 32_32_32_FLOAT 74,
 * 32_32_32_32_UINT 75, 32_32_32_32_FLOAT 77).
 */
#define AMDRDNA_GFX10_VERTEX_FLOAT1	22U
#define AMDRDNA_GFX10_VERTEX_FLOAT2	64U
#define AMDRDNA_GFX10_VERTEX_FLOAT3	74U
#define AMDRDNA_GFX10_VERTEX_FLOAT4	77U
#define AMDRDNA_GFX10_VERTEX_UINT1	20U
#define AMDRDNA_GFX10_VERTEX_UINT2	62U
#define AMDRDNA_GFX10_VERTEX_UINT3	72U
#define AMDRDNA_GFX10_VERTEX_UINT4	75U
#define AMDRDNA_GFX10_VERTEX_UNORM8X4	56U

/*
 * The register values one program needs, by what they set.
 *
 * Every stage: the register counts and float mode word, and the inputs
 * word (user SGPRs, the system values the hardware delivers, the shared
 * memory size).  A vertex program: the parameter export count, the
 * position export formats, the output control bits (the point size) and
 * the primitive number's delivery.  A pixel program: the input VGPRs
 * enabled, the colour export formats (one for each colour export, in
 * order without holes) and the colour unit's export mask (four bits for
 * each target a colour location is written for), the depth export format,
 * the interpolant count, one
 * control word per attribute, the interpolation control (the point sprite
 * coordinate's channels), whether it discards (the depth unit's kill
 * enable, which the driver sets), and the depth unit's control bits that
 * take its exported depth and sample mask (the driver adds them to its
 * own).  A vertex program run as a primitive
 * shader says so, and whether it culls (the driver then loads its culling
 * user SGPRs and oversubscribes the parameter cache by its parameter
 * count); its count and inputs words are then those of the geometry
 * stage's registers.
 */
struct amdrdna_gfx10_registers {
	uint32_t primitive_shader;
	uint32_t primitive_culling;
	uint32_t parameter_count;
	uint32_t counts;
	uint32_t inputs;
	uint32_t vertex_export_config;
	uint32_t position_formats;
	uint32_t vertex_output_control;
	uint32_t primitive_id_enable;
	uint32_t interpolation_control;
	uint32_t pixel_input_enable;
	uint32_t color_formats;
	uint32_t color_mask;
	uint32_t depth_format;
	uint32_t depth_control;
	uint32_t interpolant_control;
	uint32_t attribute_control[AMDRDNA_GFX10_LOCATIONS];
	uint32_t attribute_count;
	uint32_t discards;
};

/* Computes the register values of a compiled program. */
void amdrdna_gfx10_program_registers(const struct amdrdna_gfx10_program *program, struct amdrdna_gfx10_registers *registers);

/* Writes the four words of a raw buffer descriptor: bytes addressed by byte offset, past the size read as 0. */
void amdrdna_gfx10_raw_buffer_descriptor(uint64_t address, uint32_t size, uint32_t *words);

/*
 * Writes the four words of a vertex buffer descriptor: records of stride
 * bytes from address, count of them, each fetched in a format
 * (AMDRDNA_GFX10_VERTEX_*) of components channels, the missing channels
 * read as 0, 0, 0, 1.
 */
void amdrdna_gfx10_vertex_descriptor(uint64_t address, uint32_t stride, uint32_t count, uint32_t format, uint32_t components, uint32_t *words);

#endif /* DRIVERS_GPU_AMDRDNA_COMPILER_GFX10_PROGRAM_H */
