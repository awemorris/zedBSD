/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * A geometry shader that reads its inputs and writes its outputs but emits
 * no vertex: located per-vertex inputs at a
 * vertex chosen at run time and at a constant one, gl_in's position at
 * both, gl_PrimitiveIDIn, and gl_Layer and gl_PrimitiveID written.  The
 * host fixture runs it on the EU model and reads the staged VUE.
 */
#version 450

layout(triangles) in;
layout(triangle_strip, max_vertices = 3) out;

layout(location = 0) in vec4 v_colour[];
layout(location = 2) in vec2 v_uv[];

layout(location = 0) out vec4 g_colour;

void main()
{
    int i = min(gl_PrimitiveIDIn & 3, 2);

    g_colour = v_colour[i] + vec4(v_uv[1], 0.0, 0.0) + gl_in[2].gl_Position;
    gl_Position = gl_in[i].gl_Position;
    gl_Layer = 1;
    gl_PrimitiveID = gl_PrimitiveIDIn;
}
