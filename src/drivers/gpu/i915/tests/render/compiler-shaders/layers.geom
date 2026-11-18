/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * glxtest's layered shader in Vulkan GLSL: each triangle is drawn into
 * layer 0 in green and layer 1 in red -- two loops, gl_in chosen at run
 * time, gl_Layer written and a strip ended inside the outer loop.
 */
#version 450

layout(triangles) in;
layout(triangle_strip, max_vertices = 6) out;

layout(location = 0) out vec4 g_colour;

void main()
{
    int layer;
    int i;
    for (layer = 0; layer < 2; layer++) {
        for (i = 0; i < 3; i++) {
            gl_Layer = layer;
            g_colour = vec4(float(layer), float(1 - layer), 0.0, 1.0);
            gl_Position = gl_in[i].gl_Position;
            EmitVertex();
        }
        EndPrimitive();
    }
}
