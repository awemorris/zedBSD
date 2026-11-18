/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Passes each vertex's value to the fragment shader unchanged: every vertex of a
 * cell carries the same value, so the value arrives at every pixel exactly.
 */
#version 450

layout(location = 0) in vec4 position;
layout(location = 1) in vec4 value;

layout(location = 0) out vec4 v;

void main()
{
    v = value;
    gl_Position = position;
}
