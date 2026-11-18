/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Places one vertex of the generality test where it is and passes its
 * pixel coordinate on, which the fragment shaders take their operands from.
 */
#version 450

layout(location = 0) in vec4 position;
layout(location = 1) in vec4 coordinate;

layout(location = 0) out vec4 pixel;

void main()
{
    pixel = coordinate;
    gl_Position = position;
}
