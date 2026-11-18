/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Places one vertex of the generality test's matrix step by a chain of
 * matrices of a uniform block: a row-major turn (read through transpose),
 * a column-major scale, and a function-local matrix built from both.  Only
 * the right chain puts the pixel coordinate where regenerate.py expects it.
 */
#version 450

layout(location = 0) in vec4 position;
layout(location = 1) in vec4 coordinate;

layout(location = 0) out vec4 pixel;

layout(set = 0, binding = 1, std140) uniform Placement {
    layout(row_major) mat4 turn;
    mat4 scale;
} placement;

void main()
{
    mat4 chain;

    chain = transpose(placement.turn) * placement.scale;
    pixel = coordinate;
    gl_Position = chain * position;
}
