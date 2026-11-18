/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Colours the feature test's uniform step from a uniform block (std140): a
 * colour, scaled by the second element of an array (ArrayStride 16), plus
 * the third column of a mat4 (MatrixStride 16).  The first array element and
 * the other columns hold values that would show if the wrong one were read.
 */
#version 450

layout(location = 0) in vec4 vertex_color;

layout(location = 0) out vec4 fragment_color;

layout(set = 0, binding = 1, std140) uniform Material {
    vec4 color;
    vec4 scale[2];
    mat4 extra;
} material;

void main()
{
    fragment_color = material.color * material.scale[1] + material.extra[2];
}
