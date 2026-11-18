/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Forty vertices of a line strip with room for sixty-four, the strip ended
 * after every third: 64 cut bits, written a dword at a time
 * with a channel mask.
 */
#version 450

layout(points) in;
layout(line_strip, max_vertices = 64) out;

void main()
{
    int i;

    for (i = 0; i < 40; i++) {
        gl_Position = gl_in[0].gl_Position + vec4(float(i), 0.0, 0.0, 0.0);
        EmitVertex();
        if (i % 3 == 2)
            EndPrimitive();
    }
}
