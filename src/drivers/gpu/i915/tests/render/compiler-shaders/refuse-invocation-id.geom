/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Refused: gl_InvocationID, which only means something with more than one
 * invocation.
 */
#version 450

layout(points) in;
layout(points, max_vertices = 1) out;

void main()
{
    gl_Position = gl_in[0].gl_Position + vec4(float(gl_InvocationID), 0.0, 0.0, 0.0);
    EmitVertex();
}
