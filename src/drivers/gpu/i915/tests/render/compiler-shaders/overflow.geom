/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Emits five vertices with room for three: the vertices past
 * OutputVertices are lost, never written past the output entry.
 */
#version 450

layout(points) in;
layout(line_strip, max_vertices = 3) out;

void main()
{
    int i;

    for (i = 0; i < 5; i++) {
        gl_Position = gl_in[0].gl_Position + vec4(float(i), 0.0, 0.0, 0.0);
        EmitVertex();
    }
}
