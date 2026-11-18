/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Places one vertex of the feature test's uniform step by the transform of
 * a uniform block: a column-major mat4 (std140, MatrixStride 16), then an
 * offset added after it.
 */
#version 450

layout(location = 0) in vec4 position;
layout(location = 1) in vec4 color;

layout(location = 0) out vec4 vertex_color;

layout(set = 0, binding = 0, std140) uniform Transform {
    mat4 transform;
    vec4 offset;
} placement;

void main()
{
    vertex_color = color;
    gl_Position = placement.transform * position + placement.offset;
}
