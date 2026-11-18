/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Refused by the code generator: 256 vertices of fifteen
 * varyings need an output URB entry of 72 KiB, past the 32 KiB the URB
 * takes.
 */
#version 450

layout(points) in;
layout(points, max_vertices = 256) out;

layout(location = 0) out vec4 g_values[15];

void main()
{
    g_values[0] = vec4(0.0);
    g_values[1] = vec4(1.0);
    g_values[2] = vec4(2.0);
    g_values[3] = vec4(3.0);
    g_values[4] = vec4(4.0);
    g_values[5] = vec4(5.0);
    g_values[6] = vec4(6.0);
    g_values[7] = vec4(7.0);
    g_values[8] = vec4(8.0);
    g_values[9] = vec4(9.0);
    g_values[10] = vec4(10.0);
    g_values[11] = vec4(11.0);
    g_values[12] = vec4(12.0);
    g_values[13] = vec4(13.0);
    g_values[14] = vec4(14.0);
    gl_Position = gl_in[0].gl_Position;
    EmitVertex();
}
