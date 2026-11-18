/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The GLSL compiler's types: the built-in scalars, vectors, matrices,
 * samplers and images (static, shared by every compile and never
 * written), arrays and image variables' types of a format made in a
 * compile's arena, and the std140 layout the default uniform block uses.
 *
 * The 64-bit floats (double, dvecN, dmatCxR) take two words a component
 * in a block: std140 and std430 align them as the 32-bit types with every
 * size doubled.
 */

#include "internal.h"

#include <stdio.h>
#include <string.h>

/*
 * A built-in type's name and the versions it exists in.
 */
struct types_name {
	const char *name;
	const struct glsl_type *type;
	unsigned versions;
};

/* void, and the type of an expression that already had an error. */
static const struct glsl_type types_void = { GLSL_KIND_VOID, GLSL_BASE_NONE, 0U, 0U, 0U, 0U, "void", NULL, 0U, NULL, 0U, 0U, 0U, 0U, 0U };
static const struct glsl_type types_error = { GLSL_KIND_ERROR, GLSL_BASE_NONE, 0U, 0U, 0U, 0U, "<error>", NULL, 0U, NULL, 0U, 0U, 0U, 0U, 0U };

/*
 * The scalars, by base (GLSL_BASE_BOOL .. GLSL_BASE_DOUBLE; index 0 is
 * not a type).
 */
static const struct glsl_type types_scalars[6] = {
	{ GLSL_KIND_ERROR, GLSL_BASE_NONE, 0U, 0U, 0U, 0U, "<error>", NULL, 0U, NULL, 0U, 0U, 0U, 0U, 0U },
	{ GLSL_KIND_SCALAR, GLSL_BASE_BOOL, 1U, 1U, 0U, 0U, "bool", NULL, 0U, NULL, 0U, 0U, 0U, 0U, 0U },
	{ GLSL_KIND_SCALAR, GLSL_BASE_INT, 1U, 1U, 0U, 0U, "int", NULL, 0U, NULL, 0U, 0U, 0U, 0U, 0U },
	{ GLSL_KIND_SCALAR, GLSL_BASE_UINT, 1U, 1U, 0U, 0U, "uint", NULL, 0U, NULL, 0U, 0U, 0U, 0U, 0U },
	{ GLSL_KIND_SCALAR, GLSL_BASE_FLOAT, 1U, 1U, 0U, 0U, "float", NULL, 0U, NULL, 0U, 0U, 0U, 0U, 0U },
	{ GLSL_KIND_SCALAR, GLSL_BASE_DOUBLE, 1U, 1U, 0U, 0U, "double", NULL, 0U, NULL, 0U, 0U, 0U, 0U, 0U }
};

/*
 * The vectors, by base and then by size - 2 (vec2, vec3, vec4).
 */
static const struct glsl_type types_vectors[6][3] = {
	{
		{ GLSL_KIND_ERROR, GLSL_BASE_NONE, 0U, 0U, 0U, 0U, "<error>", NULL, 0U, NULL, 0U, 0U, 0U, 0U, 0U },
		{ GLSL_KIND_ERROR, GLSL_BASE_NONE, 0U, 0U, 0U, 0U, "<error>", NULL, 0U, NULL, 0U, 0U, 0U, 0U, 0U },
		{ GLSL_KIND_ERROR, GLSL_BASE_NONE, 0U, 0U, 0U, 0U, "<error>", NULL, 0U, NULL, 0U, 0U, 0U, 0U, 0U }
	},
	{
		{ GLSL_KIND_VECTOR, GLSL_BASE_BOOL, 2U, 1U, 0U, 0U, "bvec2", NULL, 0U, NULL, 0U, 0U, 0U, 0U, 0U },
		{ GLSL_KIND_VECTOR, GLSL_BASE_BOOL, 3U, 1U, 0U, 0U, "bvec3", NULL, 0U, NULL, 0U, 0U, 0U, 0U, 0U },
		{ GLSL_KIND_VECTOR, GLSL_BASE_BOOL, 4U, 1U, 0U, 0U, "bvec4", NULL, 0U, NULL, 0U, 0U, 0U, 0U, 0U }
	},
	{
		{ GLSL_KIND_VECTOR, GLSL_BASE_INT, 2U, 1U, 0U, 0U, "ivec2", NULL, 0U, NULL, 0U, 0U, 0U, 0U, 0U },
		{ GLSL_KIND_VECTOR, GLSL_BASE_INT, 3U, 1U, 0U, 0U, "ivec3", NULL, 0U, NULL, 0U, 0U, 0U, 0U, 0U },
		{ GLSL_KIND_VECTOR, GLSL_BASE_INT, 4U, 1U, 0U, 0U, "ivec4", NULL, 0U, NULL, 0U, 0U, 0U, 0U, 0U }
	},
	{
		{ GLSL_KIND_VECTOR, GLSL_BASE_UINT, 2U, 1U, 0U, 0U, "uvec2", NULL, 0U, NULL, 0U, 0U, 0U, 0U, 0U },
		{ GLSL_KIND_VECTOR, GLSL_BASE_UINT, 3U, 1U, 0U, 0U, "uvec3", NULL, 0U, NULL, 0U, 0U, 0U, 0U, 0U },
		{ GLSL_KIND_VECTOR, GLSL_BASE_UINT, 4U, 1U, 0U, 0U, "uvec4", NULL, 0U, NULL, 0U, 0U, 0U, 0U, 0U }
	},
	{
		{ GLSL_KIND_VECTOR, GLSL_BASE_FLOAT, 2U, 1U, 0U, 0U, "vec2", NULL, 0U, NULL, 0U, 0U, 0U, 0U, 0U },
		{ GLSL_KIND_VECTOR, GLSL_BASE_FLOAT, 3U, 1U, 0U, 0U, "vec3", NULL, 0U, NULL, 0U, 0U, 0U, 0U, 0U },
		{ GLSL_KIND_VECTOR, GLSL_BASE_FLOAT, 4U, 1U, 0U, 0U, "vec4", NULL, 0U, NULL, 0U, 0U, 0U, 0U, 0U }
	},
	{
		{ GLSL_KIND_VECTOR, GLSL_BASE_DOUBLE, 2U, 1U, 0U, 0U, "dvec2", NULL, 0U, NULL, 0U, 0U, 0U, 0U, 0U },
		{ GLSL_KIND_VECTOR, GLSL_BASE_DOUBLE, 3U, 1U, 0U, 0U, "dvec3", NULL, 0U, NULL, 0U, 0U, 0U, 0U, 0U },
		{ GLSL_KIND_VECTOR, GLSL_BASE_DOUBLE, 4U, 1U, 0U, 0U, "dvec4", NULL, 0U, NULL, 0U, 0U, 0U, 0U, 0U }
	}
};

/*
 * The float matrices, by columns - 2 and then rows - 2 (matCxR).
 */
static const struct glsl_type types_matrices[3][3] = {
	{
		{ GLSL_KIND_MATRIX, GLSL_BASE_FLOAT, 2U, 2U, 0U, 0U, "mat2", NULL, 0U, NULL, 0U, 0U, 0U, 0U, 0U },
		{ GLSL_KIND_MATRIX, GLSL_BASE_FLOAT, 3U, 2U, 0U, 0U, "mat2x3", NULL, 0U, NULL, 0U, 0U, 0U, 0U, 0U },
		{ GLSL_KIND_MATRIX, GLSL_BASE_FLOAT, 4U, 2U, 0U, 0U, "mat2x4", NULL, 0U, NULL, 0U, 0U, 0U, 0U, 0U }
	},
	{
		{ GLSL_KIND_MATRIX, GLSL_BASE_FLOAT, 2U, 3U, 0U, 0U, "mat3x2", NULL, 0U, NULL, 0U, 0U, 0U, 0U, 0U },
		{ GLSL_KIND_MATRIX, GLSL_BASE_FLOAT, 3U, 3U, 0U, 0U, "mat3", NULL, 0U, NULL, 0U, 0U, 0U, 0U, 0U },
		{ GLSL_KIND_MATRIX, GLSL_BASE_FLOAT, 4U, 3U, 0U, 0U, "mat3x4", NULL, 0U, NULL, 0U, 0U, 0U, 0U, 0U }
	},
	{
		{ GLSL_KIND_MATRIX, GLSL_BASE_FLOAT, 2U, 4U, 0U, 0U, "mat4x2", NULL, 0U, NULL, 0U, 0U, 0U, 0U, 0U },
		{ GLSL_KIND_MATRIX, GLSL_BASE_FLOAT, 3U, 4U, 0U, 0U, "mat4x3", NULL, 0U, NULL, 0U, 0U, 0U, 0U, 0U },
		{ GLSL_KIND_MATRIX, GLSL_BASE_FLOAT, 4U, 4U, 0U, 0U, "mat4", NULL, 0U, NULL, 0U, 0U, 0U, 0U, 0U }
	}
};

/*
 * The double matrices, by columns - 2 and then rows - 2 (dmatCxR).
 */
static const struct glsl_type types_dmatrices[3][3] = {
	{
		{ GLSL_KIND_MATRIX, GLSL_BASE_DOUBLE, 2U, 2U, 0U, 0U, "dmat2", NULL, 0U, NULL, 0U, 0U, 0U, 0U, 0U },
		{ GLSL_KIND_MATRIX, GLSL_BASE_DOUBLE, 3U, 2U, 0U, 0U, "dmat2x3", NULL, 0U, NULL, 0U, 0U, 0U, 0U, 0U },
		{ GLSL_KIND_MATRIX, GLSL_BASE_DOUBLE, 4U, 2U, 0U, 0U, "dmat2x4", NULL, 0U, NULL, 0U, 0U, 0U, 0U, 0U }
	},
	{
		{ GLSL_KIND_MATRIX, GLSL_BASE_DOUBLE, 2U, 3U, 0U, 0U, "dmat3x2", NULL, 0U, NULL, 0U, 0U, 0U, 0U, 0U },
		{ GLSL_KIND_MATRIX, GLSL_BASE_DOUBLE, 3U, 3U, 0U, 0U, "dmat3", NULL, 0U, NULL, 0U, 0U, 0U, 0U, 0U },
		{ GLSL_KIND_MATRIX, GLSL_BASE_DOUBLE, 4U, 3U, 0U, 0U, "dmat3x4", NULL, 0U, NULL, 0U, 0U, 0U, 0U, 0U }
	},
	{
		{ GLSL_KIND_MATRIX, GLSL_BASE_DOUBLE, 2U, 4U, 0U, 0U, "dmat4x2", NULL, 0U, NULL, 0U, 0U, 0U, 0U, 0U },
		{ GLSL_KIND_MATRIX, GLSL_BASE_DOUBLE, 3U, 4U, 0U, 0U, "dmat4x3", NULL, 0U, NULL, 0U, 0U, 0U, 0U, 0U },
		{ GLSL_KIND_MATRIX, GLSL_BASE_DOUBLE, 4U, 4U, 0U, 0U, "dmat4", NULL, 0U, NULL, 0U, 0U, 0U, 0U, 0U }
	}
};

/*
 * The samplers: float, int and uint texels of each dimension, then the
 * shadow samplers, the 2D arrays, desktop GLSL 1.40's rectangle and
 * buffer samplers, 1.50's multisample samplers and 4.00's cube map
 * arrays.
 */
static const struct glsl_type types_samplers[] = {
	{ GLSL_KIND_SAMPLER, GLSL_BASE_FLOAT, 4U, 1U, GLSL_SAMPLER_1D, 0U, "sampler1D", NULL, 0U, NULL, 0U, 0U, 0U, 0U, 0U },
	{ GLSL_KIND_SAMPLER, GLSL_BASE_FLOAT, 4U, 1U, GLSL_SAMPLER_2D, 0U, "sampler2D", NULL, 0U, NULL, 0U, 0U, 0U, 0U, 0U },
	{ GLSL_KIND_SAMPLER, GLSL_BASE_FLOAT, 4U, 1U, GLSL_SAMPLER_3D, 0U, "sampler3D", NULL, 0U, NULL, 0U, 0U, 0U, 0U, 0U },
	{ GLSL_KIND_SAMPLER, GLSL_BASE_FLOAT, 4U, 1U, GLSL_SAMPLER_CUBE, 0U, "samplerCube", NULL, 0U, NULL, 0U, 0U, 0U, 0U, 0U },
	{ GLSL_KIND_SAMPLER, GLSL_BASE_INT, 4U, 1U, GLSL_SAMPLER_1D, 0U, "isampler1D", NULL, 0U, NULL, 0U, 0U, 0U, 0U, 0U },
	{ GLSL_KIND_SAMPLER, GLSL_BASE_INT, 4U, 1U, GLSL_SAMPLER_2D, 0U, "isampler2D", NULL, 0U, NULL, 0U, 0U, 0U, 0U, 0U },
	{ GLSL_KIND_SAMPLER, GLSL_BASE_INT, 4U, 1U, GLSL_SAMPLER_3D, 0U, "isampler3D", NULL, 0U, NULL, 0U, 0U, 0U, 0U, 0U },
	{ GLSL_KIND_SAMPLER, GLSL_BASE_INT, 4U, 1U, GLSL_SAMPLER_CUBE, 0U, "isamplerCube", NULL, 0U, NULL, 0U, 0U, 0U, 0U, 0U },
	{ GLSL_KIND_SAMPLER, GLSL_BASE_UINT, 4U, 1U, GLSL_SAMPLER_1D, 0U, "usampler1D", NULL, 0U, NULL, 0U, 0U, 0U, 0U, 0U },
	{ GLSL_KIND_SAMPLER, GLSL_BASE_UINT, 4U, 1U, GLSL_SAMPLER_2D, 0U, "usampler2D", NULL, 0U, NULL, 0U, 0U, 0U, 0U, 0U },
	{ GLSL_KIND_SAMPLER, GLSL_BASE_UINT, 4U, 1U, GLSL_SAMPLER_3D, 0U, "usampler3D", NULL, 0U, NULL, 0U, 0U, 0U, 0U, 0U },
	{ GLSL_KIND_SAMPLER, GLSL_BASE_UINT, 4U, 1U, GLSL_SAMPLER_CUBE, 0U, "usamplerCube", NULL, 0U, NULL, 0U, 0U, 0U, 0U, 0U },
	{ GLSL_KIND_SAMPLER, GLSL_BASE_FLOAT, 4U, 1U, GLSL_SAMPLER_1D, 1U, "sampler1DShadow", NULL, 0U, NULL, 0U, 0U, 0U, 0U, 0U },
	{ GLSL_KIND_SAMPLER, GLSL_BASE_FLOAT, 4U, 1U, GLSL_SAMPLER_2D, 1U, "sampler2DShadow", NULL, 0U, NULL, 0U, 0U, 0U, 0U, 0U },
	{ GLSL_KIND_SAMPLER, GLSL_BASE_FLOAT, 4U, 1U, GLSL_SAMPLER_CUBE, 1U, "samplerCubeShadow", NULL, 0U, NULL, 0U, 0U, 0U, 0U, 0U },
	{ GLSL_KIND_SAMPLER, GLSL_BASE_FLOAT, 4U, 1U, GLSL_SAMPLER_2D, 0U, "sampler2DArray", NULL, 0U, NULL, 0U, 1U, 0U, 0U, 0U },
	{ GLSL_KIND_SAMPLER, GLSL_BASE_INT, 4U, 1U, GLSL_SAMPLER_2D, 0U, "isampler2DArray", NULL, 0U, NULL, 0U, 1U, 0U, 0U, 0U },
	{ GLSL_KIND_SAMPLER, GLSL_BASE_UINT, 4U, 1U, GLSL_SAMPLER_2D, 0U, "usampler2DArray", NULL, 0U, NULL, 0U, 1U, 0U, 0U, 0U },
	{ GLSL_KIND_SAMPLER, GLSL_BASE_FLOAT, 4U, 1U, GLSL_SAMPLER_2D, 1U, "sampler2DArrayShadow", NULL, 0U, NULL, 0U, 1U, 0U, 0U, 0U },
	{ GLSL_KIND_SAMPLER, GLSL_BASE_FLOAT, 4U, 1U, GLSL_SAMPLER_RECT, 0U, "sampler2DRect", NULL, 0U, NULL, 0U, 0U, 0U, 0U, 0U },
	{ GLSL_KIND_SAMPLER, GLSL_BASE_INT, 4U, 1U, GLSL_SAMPLER_RECT, 0U, "isampler2DRect", NULL, 0U, NULL, 0U, 0U, 0U, 0U, 0U },
	{ GLSL_KIND_SAMPLER, GLSL_BASE_UINT, 4U, 1U, GLSL_SAMPLER_RECT, 0U, "usampler2DRect", NULL, 0U, NULL, 0U, 0U, 0U, 0U, 0U },
	{ GLSL_KIND_SAMPLER, GLSL_BASE_FLOAT, 4U, 1U, GLSL_SAMPLER_RECT, 1U, "sampler2DRectShadow", NULL, 0U, NULL, 0U, 0U, 0U, 0U, 0U },
	{ GLSL_KIND_SAMPLER, GLSL_BASE_FLOAT, 4U, 1U, GLSL_SAMPLER_BUFFER, 0U, "samplerBuffer", NULL, 0U, NULL, 0U, 0U, 0U, 0U, 0U },
	{ GLSL_KIND_SAMPLER, GLSL_BASE_INT, 4U, 1U, GLSL_SAMPLER_BUFFER, 0U, "isamplerBuffer", NULL, 0U, NULL, 0U, 0U, 0U, 0U, 0U },
	{ GLSL_KIND_SAMPLER, GLSL_BASE_UINT, 4U, 1U, GLSL_SAMPLER_BUFFER, 0U, "usamplerBuffer", NULL, 0U, NULL, 0U, 0U, 0U, 0U, 0U },
	{ GLSL_KIND_SAMPLER, GLSL_BASE_FLOAT, 4U, 1U, GLSL_SAMPLER_MS, 0U, "sampler2DMS", NULL, 0U, NULL, 0U, 0U, 0U, 0U, 0U },
	{ GLSL_KIND_SAMPLER, GLSL_BASE_INT, 4U, 1U, GLSL_SAMPLER_MS, 0U, "isampler2DMS", NULL, 0U, NULL, 0U, 0U, 0U, 0U, 0U },
	{ GLSL_KIND_SAMPLER, GLSL_BASE_UINT, 4U, 1U, GLSL_SAMPLER_MS, 0U, "usampler2DMS", NULL, 0U, NULL, 0U, 0U, 0U, 0U, 0U },
	{ GLSL_KIND_SAMPLER, GLSL_BASE_FLOAT, 4U, 1U, GLSL_SAMPLER_CUBE, 0U, "samplerCubeArray", NULL, 0U, NULL, 0U, 1U, 0U, 0U, 0U },
	{ GLSL_KIND_SAMPLER, GLSL_BASE_INT, 4U, 1U, GLSL_SAMPLER_CUBE, 0U, "isamplerCubeArray", NULL, 0U, NULL, 0U, 1U, 0U, 0U, 0U },
	{ GLSL_KIND_SAMPLER, GLSL_BASE_UINT, 4U, 1U, GLSL_SAMPLER_CUBE, 0U, "usamplerCubeArray", NULL, 0U, NULL, 0U, 1U, 0U, 0U, 0U },
	{ GLSL_KIND_SAMPLER, GLSL_BASE_FLOAT, 4U, 1U, GLSL_SAMPLER_CUBE, 1U, "samplerCubeArrayShadow", NULL, 0U, NULL, 0U, 1U, 0U, 0U, 0U }
};

/*
 * The images of GLSL ES 3.10 (opaque like the samplers, of their kind,
 * with image set): float, int and uint texels of 2D, 3D, cube map and 2D
 * array images.  A variable's type is a copy with its layout's format
 * (glsl_type_image_format).
 */
static const struct glsl_type types_images[] = {
	{ GLSL_KIND_SAMPLER, GLSL_BASE_FLOAT, 4U, 1U, GLSL_SAMPLER_2D, 0U, "image2D", NULL, 0U, NULL, 0U, 0U, 0U, 1U, 0U },
	{ GLSL_KIND_SAMPLER, GLSL_BASE_INT, 4U, 1U, GLSL_SAMPLER_2D, 0U, "iimage2D", NULL, 0U, NULL, 0U, 0U, 0U, 1U, 0U },
	{ GLSL_KIND_SAMPLER, GLSL_BASE_UINT, 4U, 1U, GLSL_SAMPLER_2D, 0U, "uimage2D", NULL, 0U, NULL, 0U, 0U, 0U, 1U, 0U },
	{ GLSL_KIND_SAMPLER, GLSL_BASE_FLOAT, 4U, 1U, GLSL_SAMPLER_3D, 0U, "image3D", NULL, 0U, NULL, 0U, 0U, 0U, 1U, 0U },
	{ GLSL_KIND_SAMPLER, GLSL_BASE_INT, 4U, 1U, GLSL_SAMPLER_3D, 0U, "iimage3D", NULL, 0U, NULL, 0U, 0U, 0U, 1U, 0U },
	{ GLSL_KIND_SAMPLER, GLSL_BASE_UINT, 4U, 1U, GLSL_SAMPLER_3D, 0U, "uimage3D", NULL, 0U, NULL, 0U, 0U, 0U, 1U, 0U },
	{ GLSL_KIND_SAMPLER, GLSL_BASE_FLOAT, 4U, 1U, GLSL_SAMPLER_CUBE, 0U, "imageCube", NULL, 0U, NULL, 0U, 0U, 0U, 1U, 0U },
	{ GLSL_KIND_SAMPLER, GLSL_BASE_INT, 4U, 1U, GLSL_SAMPLER_CUBE, 0U, "iimageCube", NULL, 0U, NULL, 0U, 0U, 0U, 1U, 0U },
	{ GLSL_KIND_SAMPLER, GLSL_BASE_UINT, 4U, 1U, GLSL_SAMPLER_CUBE, 0U, "uimageCube", NULL, 0U, NULL, 0U, 0U, 0U, 1U, 0U },
	{ GLSL_KIND_SAMPLER, GLSL_BASE_FLOAT, 4U, 1U, GLSL_SAMPLER_2D, 0U, "image2DArray", NULL, 0U, NULL, 0U, 1U, 0U, 1U, 0U },
	{ GLSL_KIND_SAMPLER, GLSL_BASE_INT, 4U, 1U, GLSL_SAMPLER_2D, 0U, "iimage2DArray", NULL, 0U, NULL, 0U, 1U, 0U, 1U, 0U },
	{ GLSL_KIND_SAMPLER, GLSL_BASE_UINT, 4U, 1U, GLSL_SAMPLER_2D, 0U, "uimage2DArray", NULL, 0U, NULL, 0U, 1U, 0U, 1U, 0U }
};

/*
 * The built-in type names and the versions each exists in (a name
 * outside its versions is an ordinary identifier).
 */
static const struct types_name types_names[] = {
	{ "void", &types_void, GLSL_IN_ALL },
	{ "bool", &types_scalars[GLSL_BASE_BOOL], GLSL_IN_ALL },
	{ "int", &types_scalars[GLSL_BASE_INT], GLSL_IN_ALL },
	{ "uint", &types_scalars[GLSL_BASE_UINT], GLSL_IN_130_UP },
	{ "float", &types_scalars[GLSL_BASE_FLOAT], GLSL_IN_ALL },
	{ "bvec2", &types_vectors[GLSL_BASE_BOOL][0], GLSL_IN_ALL },
	{ "bvec3", &types_vectors[GLSL_BASE_BOOL][1], GLSL_IN_ALL },
	{ "bvec4", &types_vectors[GLSL_BASE_BOOL][2], GLSL_IN_ALL },
	{ "ivec2", &types_vectors[GLSL_BASE_INT][0], GLSL_IN_ALL },
	{ "ivec3", &types_vectors[GLSL_BASE_INT][1], GLSL_IN_ALL },
	{ "ivec4", &types_vectors[GLSL_BASE_INT][2], GLSL_IN_ALL },
	{ "uvec2", &types_vectors[GLSL_BASE_UINT][0], GLSL_IN_130_UP },
	{ "uvec3", &types_vectors[GLSL_BASE_UINT][1], GLSL_IN_130_UP },
	{ "uvec4", &types_vectors[GLSL_BASE_UINT][2], GLSL_IN_130_UP },
	{ "vec2", &types_vectors[GLSL_BASE_FLOAT][0], GLSL_IN_ALL },
	{ "vec3", &types_vectors[GLSL_BASE_FLOAT][1], GLSL_IN_ALL },
	{ "vec4", &types_vectors[GLSL_BASE_FLOAT][2], GLSL_IN_ALL },
	{ "mat2", &types_matrices[0][0], GLSL_IN_ALL },
	{ "mat3", &types_matrices[1][1], GLSL_IN_ALL },
	{ "mat4", &types_matrices[2][2], GLSL_IN_ALL },
	{ "mat2x2", &types_matrices[0][0], GLSL_IN_120_UP },
	{ "mat2x3", &types_matrices[0][1], GLSL_IN_120_UP },
	{ "mat2x4", &types_matrices[0][2], GLSL_IN_120_UP },
	{ "mat3x2", &types_matrices[1][0], GLSL_IN_120_UP },
	{ "mat3x3", &types_matrices[1][1], GLSL_IN_120_UP },
	{ "mat3x4", &types_matrices[1][2], GLSL_IN_120_UP },
	{ "mat4x2", &types_matrices[2][0], GLSL_IN_120_UP },
	{ "mat4x3", &types_matrices[2][1], GLSL_IN_120_UP },
	{ "mat4x4", &types_matrices[2][2], GLSL_IN_120_UP },
	{ "double", &types_scalars[GLSL_BASE_DOUBLE], GLSL_IN_FP64 },
	{ "dvec2", &types_vectors[GLSL_BASE_DOUBLE][0], GLSL_IN_FP64 },
	{ "dvec3", &types_vectors[GLSL_BASE_DOUBLE][1], GLSL_IN_FP64 },
	{ "dvec4", &types_vectors[GLSL_BASE_DOUBLE][2], GLSL_IN_FP64 },
	{ "dmat2", &types_dmatrices[0][0], GLSL_IN_FP64 },
	{ "dmat3", &types_dmatrices[1][1], GLSL_IN_FP64 },
	{ "dmat4", &types_dmatrices[2][2], GLSL_IN_FP64 },
	{ "dmat2x2", &types_dmatrices[0][0], GLSL_IN_FP64 },
	{ "dmat2x3", &types_dmatrices[0][1], GLSL_IN_FP64 },
	{ "dmat2x4", &types_dmatrices[0][2], GLSL_IN_FP64 },
	{ "dmat3x2", &types_dmatrices[1][0], GLSL_IN_FP64 },
	{ "dmat3x3", &types_dmatrices[1][1], GLSL_IN_FP64 },
	{ "dmat3x4", &types_dmatrices[1][2], GLSL_IN_FP64 },
	{ "dmat4x2", &types_dmatrices[2][0], GLSL_IN_FP64 },
	{ "dmat4x3", &types_dmatrices[2][1], GLSL_IN_FP64 },
	{ "dmat4x4", &types_dmatrices[2][2], GLSL_IN_FP64 },
	{ "sampler1D", &types_samplers[0], GLSL_IN_DESKTOP },
	{ "sampler2D", &types_samplers[1], GLSL_IN_ALL },
	{ "sampler3D", &types_samplers[2], GLSL_IN_DESKTOP | GLSL_IN_ES300 },
	{ "samplerCube", &types_samplers[3], GLSL_IN_ALL },
	{ "isampler1D", &types_samplers[4], GLSL_IN_DESKTOP_130_UP },
	{ "isampler2D", &types_samplers[5], GLSL_IN_130_UP },
	{ "isampler3D", &types_samplers[6], GLSL_IN_130_UP },
	{ "isamplerCube", &types_samplers[7], GLSL_IN_130_UP },
	{ "usampler1D", &types_samplers[8], GLSL_IN_DESKTOP_130_UP },
	{ "usampler2D", &types_samplers[9], GLSL_IN_130_UP },
	{ "usampler3D", &types_samplers[10], GLSL_IN_130_UP },
	{ "usamplerCube", &types_samplers[11], GLSL_IN_130_UP },
	{ "sampler1DShadow", &types_samplers[12], GLSL_IN_DESKTOP },
	{ "sampler2DShadow", &types_samplers[13], GLSL_IN_DESKTOP | GLSL_IN_ES300 },
	{ "samplerCubeShadow", &types_samplers[14], GLSL_IN_130_UP },
	{ "sampler2DArray", &types_samplers[15], GLSL_IN_130_UP },
	{ "isampler2DArray", &types_samplers[16], GLSL_IN_130_UP },
	{ "usampler2DArray", &types_samplers[17], GLSL_IN_130_UP },
	{ "sampler2DArrayShadow", &types_samplers[18], GLSL_IN_130_UP },
	{ "sampler2DRect", &types_samplers[19], GLSL_IN_DESKTOP_140_UP },
	{ "isampler2DRect", &types_samplers[20], GLSL_IN_DESKTOP_140_UP },
	{ "usampler2DRect", &types_samplers[21], GLSL_IN_DESKTOP_140_UP },
	{ "sampler2DRectShadow", &types_samplers[22], GLSL_IN_DESKTOP_140_UP },
	{ "samplerBuffer", &types_samplers[23], GLSL_IN_DESKTOP_140_UP },
	{ "isamplerBuffer", &types_samplers[24], GLSL_IN_DESKTOP_140_UP },
	{ "usamplerBuffer", &types_samplers[25], GLSL_IN_DESKTOP_140_UP },
	{ "sampler2DMS", &types_samplers[26], GLSL_IN_150_UP | GLSL_IN_ES310 },
	{ "isampler2DMS", &types_samplers[27], GLSL_IN_150_UP | GLSL_IN_ES310 },
	{ "usampler2DMS", &types_samplers[28], GLSL_IN_150_UP | GLSL_IN_ES310 },
	{ "samplerCubeArray", &types_samplers[29], GLSL_IN_CUBE_ARRAY },
	{ "isamplerCubeArray", &types_samplers[30], GLSL_IN_CUBE_ARRAY },
	{ "usamplerCubeArray", &types_samplers[31], GLSL_IN_CUBE_ARRAY },
	{ "samplerCubeArrayShadow", &types_samplers[32], GLSL_IN_CUBE_ARRAY },
	{ "image2D", &types_images[0], GLSL_IN_ES310 },
	{ "iimage2D", &types_images[1], GLSL_IN_ES310 },
	{ "uimage2D", &types_images[2], GLSL_IN_ES310 },
	{ "image3D", &types_images[3], GLSL_IN_ES310 },
	{ "iimage3D", &types_images[4], GLSL_IN_ES310 },
	{ "uimage3D", &types_images[5], GLSL_IN_ES310 },
	{ "imageCube", &types_images[6], GLSL_IN_ES310 },
	{ "iimageCube", &types_images[7], GLSL_IN_ES310 },
	{ "uimageCube", &types_images[8], GLSL_IN_ES310 },
	{ "image2DArray", &types_images[9], GLSL_IN_ES310 },
	{ "iimage2DArray", &types_images[10], GLSL_IN_ES310 },
	{ "uimage2DArray", &types_images[11], GLSL_IN_ES310 }
};

static unsigned types_round(unsigned value, unsigned alignment);
static unsigned types_scalar_size(unsigned base);
static unsigned types_vector_alignment(unsigned base, unsigned components);

/*
 * Returns void.
 */
const struct glsl_type *
glsl_type_void(void)
{
	/* The one void. */
	return &types_void;
}

/*
 * Returns the type of an expression that already had an error (no
 * further error is reported about it).
 */
const struct glsl_type *
glsl_type_error(void)
{
	/* The one error type. */
	return &types_error;
}

/*
 * Returns the scalar type of a base.
 */
const struct glsl_type *
glsl_type_scalar(
	unsigned base)
{
	/* A base outside the table is an error. */
	if (base < GLSL_BASE_BOOL || base > GLSL_BASE_DOUBLE)
		return &types_error;

	/* The scalar. */
	return &types_scalars[base];
}

/*
 * Returns the vector type of a base and a size (a size of 1 is the
 * scalar).
 */
const struct glsl_type *
glsl_type_vector(
	unsigned base,
	unsigned components)
{
	/* One component is the scalar. */
	if (components == 1U)
		return glsl_type_scalar(base);

	/* A base or size outside the table is an error. */
	if (base < GLSL_BASE_BOOL || base > GLSL_BASE_DOUBLE)
		return &types_error;
	if (components < 2U || components > 4U)
		return &types_error;

	/* The vector. */
	return &types_vectors[base][components - 2U];
}

/*
 * Returns the float or double matrix type of a number of columns and
 * rows.
 */
const struct glsl_type *
glsl_type_matrix(
	unsigned base,
	unsigned columns,
	unsigned rows)
{
	/* A shape outside the table is an error. */
	if (columns < 2U || columns > 4U)
		return &types_error;
	if (rows < 2U || rows > 4U)
		return &types_error;

	/* A double matrix. */
	if (base == GLSL_BASE_DOUBLE)
		return &types_dmatrices[columns - 2U][rows - 2U];

	/* Only floats and doubles make matrices. */
	if (base != GLSL_BASE_FLOAT)
		return &types_error;

	/* The float matrix. */
	return &types_matrices[columns - 2U][rows - 2U];
}

/*
 * Reports whether a base is a floating-point one (float or double).
 */
int
glsl_base_floating(
	unsigned base)
{
	/* A 32-bit float. */
	if (base == GLSL_BASE_FLOAT)
		return 1;

	/* A 64-bit float. */
	if (base == GLSL_BASE_DOUBLE)
		return 1;

	/* Booleans and integers. */
	return 0;
}

/*
 * Returns the GLSL_IN_* bit of a shader's version.
 */
unsigned
glsl_version_mask(
	const struct glsl_shader *shader)
{
	unsigned extensions;
	unsigned doubles;
	unsigned query_lod;

	/* OpenGL ES 1.00, 3.00, and 3.10 (everything of 3.00 and its own, ws101-p008). */
	if (shader->es && shader->version >= GLSL_VERSION_ES310)
		return GLSL_IN_ES300 | GLSL_IN_ES310;
	if (shader->es && shader->version >= GLSL_VERSION_ES300)
		return GLSL_IN_ES300;
	if (shader->es)
		return GLSL_IN_ES100;

	/* The doubles of 4.00 on, on a device that has them. */
	doubles = 0U;
	if ((shader->features & GLSL_FEATURE_FP64) != 0U)
		doubles = GLSL_IN_FP64;

	/* GL_ARB_texture_query_lod's spelling of textureQueryLod, in any desktop version that enabled it. */
	query_lod = 0U;
	if (shader->texture_query_lod)
		query_lod = GLSL_IN_QUERY_LOD_ARB;

	/* Desktop 4.30 on: everything of 4.00, with OpenGL ES 3.10's compute shaders and shader storage blocks. */
	if (shader->version >= GLSL_VERSION_430)
		return GLSL_IN_330 | GLSL_IN_400 | GLSL_IN_TESS | GLSL_IN_SAMPLE | GLSL_IN_GATHER | GLSL_IN_CUBE_ARRAY | GLSL_IN_SHADER5 | doubles | GLSL_IN_ES310 |
		       GLSL_IN_QUERY_LOD | query_lod | GLSL_IN_SUBROUTINE;

	/*
	 * Desktop 4.00 on: everything of 3.30, with tessellation, per-sample
	 * shading, texture gathers, cube map arrays, GL_ARB_gpu_shader5's
	 * built-ins, doubles, textureQueryLod and subroutines.
	 */
	if (shader->version >= GLSL_VERSION_400)
		return GLSL_IN_330 | GLSL_IN_400 | GLSL_IN_TESS | GLSL_IN_SAMPLE | GLSL_IN_GATHER | GLSL_IN_CUBE_ARRAY | GLSL_IN_SHADER5 | doubles |
		       GLSL_IN_QUERY_LOD | query_lod | GLSL_IN_SUBROUTINE;

	/*
	 * The extensions of earlier versions (GL_ARB_tessellation_shader,
	 * GL_ARB_sample_shading, GL_ARB_texture_gather,
	 * GL_ARB_texture_cube_map_array, GL_ARB_gpu_shader_fp64,
	 * GL_ARB_gpu_shader5, GL_ARB_texture_query_lod,
	 * GL_ARB_shader_subroutine).
	 */
	extensions = 0U;
	if (shader->tessellation)
		extensions |= GLSL_IN_TESS;
	if (shader->sample_shading)
		extensions |= GLSL_IN_SAMPLE;
	if (shader->texture_gather)
		extensions |= GLSL_IN_GATHER;
	if (shader->cube_map_array)
		extensions |= GLSL_IN_CUBE_ARRAY;
	if (shader->fp64)
		extensions |= GLSL_IN_FP64;
	if (shader->gpu_shader5)
		extensions |= GLSL_IN_SHADER5;
	if (shader->shader_subroutine)
		extensions |= GLSL_IN_SUBROUTINE;
	extensions |= query_lod;

	/* GL_ARB_gpu_shader5 extends the gathers, so it has GL_ARB_texture_gather's too (as Mesa gives them). */
	if (shader->gpu_shader5)
		extensions |= GLSL_IN_GATHER;

	/* Desktop 1.10 to 3.30, with the extensions enabled. */
	switch (shader->version) {
	case GLSL_VERSION_110:
		return GLSL_IN_110 | extensions;
	case GLSL_VERSION_120:
		return GLSL_IN_120 | extensions;
	case GLSL_VERSION_130:
		return GLSL_IN_130 | extensions;
	case GLSL_VERSION_140:
		return GLSL_IN_140 | extensions;
	case GLSL_VERSION_150:
		return GLSL_IN_150 | extensions;
	default:
		break;
	}

	/* 3.30. */
	return GLSL_IN_330 | extensions;
}

/*
 * Reports whether a shader's version has a feature that came with a
 * desktop version and an OpenGL ES version (0: never in OpenGL ES).
 */
int
glsl_since(
	const struct glsl_shader *shader,
	unsigned desktop,
	unsigned es)
{
	/* OpenGL ES from its version, when it has the feature at all. */
	if (shader->es) {
		if (es != 0U && shader->version >= es)
			return 1;
		return 0;
	}

	/* Desktop from its version. */
	if (shader->version >= desktop)
		return 1;

	/* Too old. */
	return 0;
}

/*
 * Returns the sampler type of a texel base, a dimension and shadow.
 */
const struct glsl_type *
glsl_type_sampler(
	unsigned base,
	unsigned sampler,
	unsigned shadow)
{
	size_t index;
	const struct glsl_type *type;

	/* The sampler of that kind. */
	for (index = 0U; index < sizeof(types_samplers) / sizeof(types_samplers[0]); index++) {
		type = &types_samplers[index];
		if (type->base == base && type->sampler == sampler && type->shadow == shadow && !type->arrayed)
			return type;
	}

	/* None. */
	return &types_error;
}

/*
 * Returns an image variable's type: a copy of an image type with a
 * layout's format (GLSL_IMAGE_FORMAT_*), made in the arena.
 */
const struct glsl_type *
glsl_type_image_format(
	struct glsl_arena *arena,
	const struct glsl_type *image,
	unsigned format)
{
	struct glsl_type *type;

	/* The copy, of the format. */
	type = glsl_alloc(arena, sizeof(*type));
	*type = *image;
	type->image_format = format;

	/* Succeeded: the variable's type. */
	return type;
}

/*
 * Returns the texel base an image format holds (GLSL_BASE_FLOAT, _INT,
 * _UINT), GLSL_BASE_NONE for no format.
 */
unsigned
glsl_image_format_base(
	unsigned format)
{
	/* The formats of signed and unsigned integers, then the normalized and floating-point ones. */
	switch (format) {
	case GLSL_IMAGE_FORMAT_RGBA32I:
	case GLSL_IMAGE_FORMAT_RGBA16I:
	case GLSL_IMAGE_FORMAT_RGBA8I:
	case GLSL_IMAGE_FORMAT_R32I:
		return GLSL_BASE_INT;
	case GLSL_IMAGE_FORMAT_RGBA32UI:
	case GLSL_IMAGE_FORMAT_RGBA16UI:
	case GLSL_IMAGE_FORMAT_RGBA8UI:
	case GLSL_IMAGE_FORMAT_R32UI:
		return GLSL_BASE_UINT;
	case GLSL_IMAGE_FORMAT_RGBA32F:
	case GLSL_IMAGE_FORMAT_RGBA16F:
	case GLSL_IMAGE_FORMAT_R32F:
	case GLSL_IMAGE_FORMAT_RGBA8:
	case GLSL_IMAGE_FORMAT_RGBA8_SNORM:
		return GLSL_BASE_FLOAT;
	default:
		break;
	}

	/* No format. */
	return GLSL_BASE_NONE;
}

/*
 * Returns the built-in type a name spells, or NULL.  *versions receives
 * the GLSL_IN_* versions the name exists in.
 */
const struct glsl_type *
glsl_type_named(
	const char *name,
	unsigned length,
	unsigned *versions)
{
	size_t index;
	size_t name_length;
	int differs;

	/* The name of that spelling. */
	for (index = 0U; index < sizeof(types_names) / sizeof(types_names[0]); index++) {
		name_length = strlen(types_names[index].name);
		if (name_length != length)
			continue;
		differs = memcmp(types_names[index].name, name, length);
		if (differs != 0)
			continue;

		/* The type and its versions. */
		*versions = types_names[index].versions;
		return types_names[index].type;
	}

	/* Not a built-in type. */
	*versions = 0U;
	return NULL;
}

/*
 * Makes an array type of an element type and a length (0: unsized).
 */
const struct glsl_type *
glsl_type_array(
	struct glsl_arena *arena,
	const struct glsl_type *element,
	unsigned length)
{
	struct glsl_type *type;

	/* The array type. */
	type = glsl_alloc(arena, sizeof(*type));
	type->kind = GLSL_KIND_ARRAY;
	type->base = GLSL_BASE_NONE;
	type->element = element;
	type->length = length;

	/* Succeeded: the new type. */
	return type;
}

/*
 * Returns the type of a matrix's column (or of a vector's component, or
 * an array's element).
 */
const struct glsl_type *
glsl_type_column(
	const struct glsl_type *type)
{
	/* The kinds that have parts. */
	switch (type->kind) {
	case GLSL_KIND_MATRIX:
		return glsl_type_vector(type->base, type->components);
	case GLSL_KIND_VECTOR:
		return glsl_type_scalar(type->base);
	case GLSL_KIND_ARRAY:
		return type->element;
	default:
		break;
	}

	/* Anything else has none. */
	return &types_error;
}

/*
 * Returns the scalar, vector or matrix of another base with the shape
 * of a scalar, vector or matrix (a matrix only of float or double).
 */
const struct glsl_type *
glsl_type_with_base(
	const struct glsl_type *type,
	unsigned base)
{
	/* A matrix of the other floating-point base. */
	if (type->kind == GLSL_KIND_MATRIX)
		return glsl_type_matrix(base, type->columns, type->components);

	/* Only scalars, vectors and matrices change their base. */
	if (type->kind != GLSL_KIND_SCALAR && type->kind != GLSL_KIND_VECTOR)
		return &types_error;

	/* The same size, the other base. */
	return glsl_type_vector(base, type->components);
}

/*
 * Reports whether two types are the same type.
 */
int
glsl_type_equal(
	const struct glsl_type *left,
	const struct glsl_type *right)
{
	int same;

	/* The same object (every built-in type and struct is unique). */
	if (left == right)
		return 1;

	/* Two images of the same dimension, layers and texels, whatever their variables' formats. */
	if (left->kind == GLSL_KIND_SAMPLER &&
	    left->image &&
	    right->kind == GLSL_KIND_SAMPLER &&
	    right->image) {
		if (left->base != right->base || left->sampler != right->sampler)
			return 0;
		if (left->arrayed != right->arrayed)
			return 0;
		return 1;
	}

	/* Two arrays of the same element and length. */
	if (left->kind != GLSL_KIND_ARRAY || right->kind != GLSL_KIND_ARRAY)
		return 0;
	if (left->length != right->length)
		return 0;
	same = glsl_type_equal(left->element, right->element);
	if (!same)
		return 0;

	/* Succeeded: the same array type. */
	return 1;
}

/*
 * Reports whether a type is a scalar, vector or matrix of numbers (not
 * bool).
 */
int
glsl_type_numeric(
	const struct glsl_type *type)
{
	/* The shapes that hold numbers. */
	if (type->kind != GLSL_KIND_SCALAR && type->kind != GLSL_KIND_VECTOR && type->kind != GLSL_KIND_MATRIX)
		return 0;

	/* Numbers, not bool. */
	if (type->base == GLSL_BASE_BOOL)
		return 0;

	/* Succeeded: numeric. */
	return 1;
}

/*
 * Reports whether a type is or contains a sampler.
 */
int
glsl_type_contains_sampler(
	const struct glsl_type *type)
{
	unsigned index;
	int contains;

	/* A sampler, or an array of something containing one. */
	if (type->kind == GLSL_KIND_SAMPLER)
		return 1;
	if (type->kind == GLSL_KIND_ARRAY) {
		contains = glsl_type_contains_sampler(type->element);
		return contains;
	}

	/* A struct with a member containing one. */
	if (type->kind == GLSL_KIND_STRUCT) {
		for (index = 0U; index < type->field_count; index++) {
			contains = glsl_type_contains_sampler(type->fields[index].type);
			if (contains)
				return 1;
		}
	}

	/* Anything else has none. */
	return 0;
}

/*
 * Returns how many texture units a uniform of a type takes: 1 for a
 * sampler, the length for an array of samplers (ws068-p004), 0 for any
 * other type (a struct with a sampler member is not supported).
 */
unsigned
glsl_type_sampler_units(
	const struct glsl_type *type)
{
	/* A sampler (an image takes an image unit, not a texture unit). */
	if (type->kind == GLSL_KIND_SAMPLER && !type->image)
		return 1U;

	/* An array of samplers: one unit an element. */
	if (type->kind == GLSL_KIND_ARRAY && type->element->kind == GLSL_KIND_SAMPLER && !type->element->image)
		return type->length;

	/* Anything else takes none. */
	return 0U;
}

/*
 * Returns how many scalars a value of a type has (the length of its
 * flattened constant).
 */
unsigned
glsl_type_scalars(
	const struct glsl_type *type)
{
	unsigned count;
	unsigned index;

	/* The shapes of scalars. */
	switch (type->kind) {
	case GLSL_KIND_SCALAR:
		return 1U;
	case GLSL_KIND_VECTOR:
		return type->components;
	case GLSL_KIND_MATRIX:
		return type->components * type->columns;
	case GLSL_KIND_ARRAY:
		count = glsl_type_scalars(type->element);
		return count * type->length;
	case GLSL_KIND_STRUCT:
		count = 0U;
		for (index = 0U; index < type->field_count; index++)
			count += glsl_type_scalars(type->fields[index].type);
		return count;
	default:
		break;
	}

	/* Anything else has none. */
	return 0U;
}

/*
 * Returns how many locations a varying or an attribute of a type takes.
 */
unsigned
glsl_type_locations(
	const struct glsl_type *type)
{
	unsigned count;
	unsigned index;

	/* A matrix takes one per column (two per dvec3 or dvec4 column), an array one per element's worth, a struct its members'. */
	switch (type->kind) {
	case GLSL_KIND_MATRIX:
		if (type->base == GLSL_BASE_DOUBLE && type->components > 2U)
			return 2U * type->columns;
		return type->columns;
	case GLSL_KIND_ARRAY:
		count = glsl_type_locations(type->element);
		return count * type->length;
	case GLSL_KIND_STRUCT:
		count = 0U;
		for (index = 0U; index < type->field_count; index++)
			count += glsl_type_locations(type->fields[index].type);
		return count;
	default:
		break;
	}

	/* A dvec3 or dvec4 takes two. */
	if (type->base == GLSL_BASE_DOUBLE && type->components > 2U)
		return 2U;

	/* A scalar or a vector takes one. */
	return 1U;
}

/*
 * Writes a type's GLSL spelling (for messages).
 */
void
glsl_type_name(
	const struct glsl_type *type,
	char *out,
	size_t size)
{
	char element[64];

	/* An array: its element and its length. */
	if (type->kind == GLSL_KIND_ARRAY) {
		glsl_type_name(type->element, element, sizeof(element));
		if (type->length == 0U) {
			(void)snprintf(out, size, "%s[]", element);
		} else {
			(void)snprintf(out, size, "%s[%u]", element, type->length);
		}

		/* The array is spelled. */
		return;
	}

	/* A struct: "struct NAME"; anything else its name. */
	if (type->kind == GLSL_KIND_STRUCT) {
		(void)snprintf(out, size, "struct %s", type->name);
	} else {
		(void)snprintf(out, size, "%s", type->name);
	}
}

/*
 * Returns a type's alignment in a std140 block (a column-major matrix's;
 * glsl_std140_member_size and the member alignment of a struct take a
 * row-major one's rows into account).
 */
unsigned
glsl_std140_alignment(
	const struct glsl_type *type)
{
	unsigned alignment;
	unsigned member;
	unsigned index;

	/* A scalar its size, a two-vector twice that, three- and four-vectors four times; a matrix its columns' stride. */
	switch (type->kind) {
	case GLSL_KIND_SCALAR:
		return types_scalar_size(type->base);
	case GLSL_KIND_VECTOR:
		return types_vector_alignment(type->base, type->components);
	case GLSL_KIND_MATRIX:
		return glsl_std140_matrix_stride(type, 0U);
	case GLSL_KIND_ARRAY:
		alignment = glsl_std140_alignment(type->element);
		return types_round(alignment, 16U);
	case GLSL_KIND_STRUCT:
		alignment = 16U;
		for (index = 0U; index < type->field_count; index++) {
			member = glsl_std140_member_alignment(type->fields[index].type, type->fields[index].row_major);
			if (member > alignment)
				alignment = member;
		}

		/* The largest member's, at least 16. */
		return types_round(alignment, 16U);
	default:
		break;
	}

	/* Anything else is not in a block. */
	return 4U;
}

/*
 * Returns a type's size in a std140 block (a struct's and an array's
 * rounded up to their alignment).
 */
unsigned
glsl_std140_size(
	const struct glsl_type *type)
{
	unsigned offset;
	unsigned alignment;
	unsigned index;
	unsigned stride;

	/* The kinds of types. */
	switch (type->kind) {
	case GLSL_KIND_SCALAR:
		return types_scalar_size(type->base);
	case GLSL_KIND_VECTOR:
		return types_scalar_size(type->base) * type->components;
	case GLSL_KIND_MATRIX:
		return glsl_std140_matrix_stride(type, 0U) * type->columns;
	case GLSL_KIND_ARRAY:
		stride = glsl_std140_stride(type);
		return stride * type->length;
	case GLSL_KIND_STRUCT:
		offset = 0U;
		for (index = 0U; index < type->field_count; index++) {
			alignment = glsl_std140_member_alignment(type->fields[index].type, type->fields[index].row_major);
			offset = types_round(offset, alignment);
			offset += glsl_std140_member_size(type->fields[index].type, type->fields[index].row_major);
		}

		/* The end rounded to the struct's alignment. */
		alignment = glsl_std140_alignment(type);
		return types_round(offset, alignment);
	default:
		break;
	}

	/* Anything else is not in a block. */
	return 0U;
}

/*
 * Returns the std140 stride of an array type's elements.
 */
unsigned
glsl_std140_stride(
	const struct glsl_type *type)
{
	unsigned size;
	unsigned alignment;

	/* The element's size rounded up to its alignment and to 16. */
	size = glsl_std140_size(type->element);
	alignment = glsl_std140_alignment(type->element);
	size = types_round(size, alignment);

	/* Succeeded: the stride. */
	return types_round(size, 16U);
}

/*
 * Returns the std140 size of a struct member, whose matrices (or array
 * of matrices) may be row-major: then each row is a vector of the
 * columns' count, a row's stride apart.
 */
unsigned
glsl_std140_member_size(
	const struct glsl_type *type,
	unsigned row_major)
{
	unsigned stride;
	unsigned size;

	/* Column-major, or not a matrix: the type's own size. */
	if (!row_major) {
		size = glsl_std140_size(type);
		return size;
	}

	/* A row-major matrix: one row per component of a column. */
	if (type->kind == GLSL_KIND_MATRIX) {
		stride = glsl_std140_matrix_stride(type, 1U);
		return stride * type->components;
	}

	/* An array of them: the rows' stride times the length. */
	if (type->kind == GLSL_KIND_ARRAY && type->element->kind == GLSL_KIND_MATRIX) {
		stride = glsl_std140_member_stride(type, row_major);
		return stride * type->length;
	}

	/* Anything else is not a matrix. */
	size = glsl_std140_size(type);
	return size;
}

/*
 * Returns the std140 stride of an array member's elements, row-major
 * matrices taking a row per component.
 */
unsigned
glsl_std140_member_stride(
	const struct glsl_type *type,
	unsigned row_major)
{
	unsigned stride;

	/* An array of row-major matrices. */
	if (row_major && type->element->kind == GLSL_KIND_MATRIX) {
		stride = glsl_std140_matrix_stride(type->element, 1U);
		return stride * type->element->components;
	}

	/* Any other array. */
	stride = glsl_std140_stride(type);
	return stride;
}

/*
 * Returns the std140 stride of a matrix's columns (or, row-major, its
 * rows), or of the matrices of an array of them: the vector's alignment
 * rounded up to 16 (16 for a float matrix, 16 or 32 for a double one).
 */
unsigned
glsl_std140_matrix_stride(
	const struct glsl_type *type,
	unsigned row_major)
{
	unsigned components;
	unsigned alignment;

	/* An array's matrices. */
	if (type->kind == GLSL_KIND_ARRAY)
		type = type->element;

	/* A column has a component per row; a row one per column. */
	components = type->components;
	if (row_major)
		components = type->columns;

	/* The vector's alignment, at least a vec4's. */
	alignment = types_vector_alignment(type->base, components);
	return types_round(alignment, 16U);
}

/*
 * Returns a std140 struct member's alignment: a row-major matrix (or an
 * array of them) aligns as its rows.
 */
unsigned
glsl_std140_member_alignment(
	const struct glsl_type *type,
	unsigned row_major)
{
	unsigned alignment;

	/* A row-major matrix, or an array of them. */
	if (row_major && type->kind == GLSL_KIND_MATRIX) {
		alignment = glsl_std140_matrix_stride(type, 1U);
		return alignment;
	}
	if (row_major && type->kind == GLSL_KIND_ARRAY && type->element->kind == GLSL_KIND_MATRIX) {
		alignment = glsl_std140_matrix_stride(type->element, 1U);
		return alignment;
	}

	/* Anything else aligns as its type. */
	alignment = glsl_std140_alignment(type);
	return alignment;
}

/* Rounds a value up to a multiple of an alignment (a power of two). */
static unsigned
types_round(
	unsigned value,
	unsigned alignment)
{
	/* The next multiple. */
	return (value + alignment - 1U) & ~(alignment - 1U);
}

/* Returns the bytes one scalar of a base takes in a block: 8 for a double, 4 for the others. */
static unsigned
types_scalar_size(
	unsigned base)
{
	/* A double's two words. */
	if (base == GLSL_BASE_DOUBLE)
		return 8U;

	/* One word. */
	return 4U;
}

/* Returns the base alignment of a vector of a base: twice a scalar's for two components, four times for three or four. */
static unsigned
types_vector_alignment(
	unsigned base,
	unsigned components)
{
	unsigned size;

	/* A scalar's size. */
	size = types_scalar_size(base);
	if (components <= 1U)
		return size;

	/* A two-vector, a three- or four-vector. */
	if (components == 2U)
		return 2U * size;
	return 4U * size;
}

/*
 * Returns a type's base alignment in a std430 block (ws101-p008): as
 * std140's, but an array's and a struct's are not rounded up to 16.  A
 * matrix is an array of its columns.
 */
unsigned
glsl_std430_alignment(
	const struct glsl_type *type)
{
	unsigned alignment;
	unsigned member;
	unsigned index;

	/* A scalar its size, a two-vector twice that, three- and four-vectors four times; a matrix as its columns. */
	switch (type->kind) {
	case GLSL_KIND_SCALAR:
		return types_scalar_size(type->base);
	case GLSL_KIND_VECTOR:
		return types_vector_alignment(type->base, type->components);
	case GLSL_KIND_MATRIX:
		return glsl_std430_column_stride(type);
	case GLSL_KIND_ARRAY:
		return glsl_std430_alignment(type->element);
	case GLSL_KIND_STRUCT:
		alignment = 4U;
		for (index = 0U; index < type->field_count; index++) {
			member = glsl_std430_alignment(type->fields[index].type);
			if (member > alignment)
				alignment = member;
		}

		/* The largest member's. */
		return alignment;
	default:
		break;
	}

	/* Anything else is not in a block. */
	return 4U;
}

/*
 * Returns a type's size in a std430 block (ws101-p008): a struct's
 * rounded up to its alignment; a run-time array (length 0) has none.
 */
unsigned
glsl_std430_size(
	const struct glsl_type *type)
{
	unsigned offset;
	unsigned alignment;
	unsigned index;

	/* The kinds of types. */
	switch (type->kind) {
	case GLSL_KIND_SCALAR:
		return types_scalar_size(type->base);
	case GLSL_KIND_VECTOR:
		return types_scalar_size(type->base) * type->components;
	case GLSL_KIND_MATRIX:
		return glsl_std430_column_stride(type) * type->columns;
	case GLSL_KIND_ARRAY:
		return glsl_std430_stride(type) * type->length;
	case GLSL_KIND_STRUCT:
		offset = 0U;
		for (index = 0U; index < type->field_count; index++) {
			alignment = glsl_std430_alignment(type->fields[index].type);
			offset = types_round(offset, alignment);
			offset += glsl_std430_size(type->fields[index].type);
		}

		/* The end rounded to the struct's alignment. */
		alignment = glsl_std430_alignment(type);
		return types_round(offset, alignment);
	default:
		break;
	}

	/* Anything else is not in a block. */
	return 0U;
}

/* Returns the std430 stride of an array type's elements: the element's size rounded up to its alignment (ws101-p008). */
unsigned
glsl_std430_stride(
	const struct glsl_type *type)
{
	unsigned size;
	unsigned alignment;

	/* The element's size and alignment. */
	size = glsl_std430_size(type->element);
	alignment = glsl_std430_alignment(type->element);

	/* Succeeded: the stride. */
	return types_round(size, alignment);
}

/* Returns the std430 stride of a column-major matrix's columns: a column vector's alignment (8 or 16 for floats, 16 or 32 for doubles). */
unsigned
glsl_std430_column_stride(
	const struct glsl_type *type)
{
	unsigned alignment;

	/* A column is a vector of the rows. */
	alignment = types_vector_alignment(type->base, type->components);
	return alignment;
}
