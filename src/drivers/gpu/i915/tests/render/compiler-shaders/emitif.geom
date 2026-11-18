/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Emits under selections: an odd primitive emits its first
 * vertex, every primitive its second, a primitive whose number is below 4
 * its third, and the strip ends only for the even ones -- each channel
 * counts its own vertices.
 */
#version 450

layout(points) in;
layout(triangle_strip, max_vertices = 3) out;

layout(location = 0) out vec4 g_value;

void main()
{
    g_value = vec4(1.0, 0.0, 0.0, 0.0);
    gl_Position = gl_in[0].gl_Position;
    if ((gl_PrimitiveIDIn & 1) != 0)
        EmitVertex();
    g_value = vec4(2.0, 0.0, 0.0, 0.0);
    EmitVertex();
    if (gl_PrimitiveIDIn < 4) {
        g_value = vec4(3.0, 0.0, 0.0, 0.0);
        EmitVertex();
    }
    if ((gl_PrimitiveIDIn & 1) == 0)
        EndPrimitive();
}
