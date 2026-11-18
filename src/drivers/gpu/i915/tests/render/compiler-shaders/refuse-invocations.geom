/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Refused: two invocations for each input primitive.
 */
#version 450

layout(points, invocations = 2) in;
layout(points, max_vertices = 1) out;

void main()
{
    gl_Position = gl_in[0].gl_Position;
    EmitVertex();
}
