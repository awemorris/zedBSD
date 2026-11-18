/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * A hundred and forty vertices of a line strip with room for a hundred and
 * sixty, the strip ended after every fifth: 160 cut bits,
 * written a dword at a time with a channel mask and per-slot offsets.
 */
#version 450

layout(points) in;
layout(line_strip, max_vertices = 160) out;

void main()
{
    int i;

    for (i = 0; i < 140; i++) {
        gl_Position = gl_in[0].gl_Position + vec4(float(i), 0.0, 0.0, 0.0);
        EmitVertex();
        if (i % 5 == 4)
            EndPrimitive();
    }
}
