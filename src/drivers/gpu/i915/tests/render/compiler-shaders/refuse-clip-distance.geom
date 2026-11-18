/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Refused: a gl_in member other than gl_Position and gl_PointSize.
 */
#version 450

layout(points) in;
layout(points, max_vertices = 1) out;

in gl_PerVertex {
    vec4 gl_Position;
    float gl_ClipDistance[1];
} gl_in[];

void main()
{
    gl_Position = gl_in[0].gl_Position + vec4(gl_in[0].gl_ClipDistance[0], 0.0, 0.0, 0.0);
    EmitVertex();
}
