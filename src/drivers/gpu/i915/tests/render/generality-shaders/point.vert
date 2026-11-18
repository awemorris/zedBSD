/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The generality test's point step, vertex stage: a point at
 * (x, y) of the size position.w, its number (coordinate.x) passed on.
 */
#version 450

layout(location = 0) in vec4 position;
layout(location = 1) in vec4 coordinate;

layout(location = 0) flat out int number;

void main()
{
    number = int(coordinate.x);
    gl_PointSize = position.w;
    gl_Position = vec4(position.xy, 0.0, 1.0);
}
