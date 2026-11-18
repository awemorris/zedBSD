/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * glxtest's lines-with-adjacency shader in Vulkan GLSL: a square over the
 * target, blue when the fourth vertex lies right of 0.5, red otherwise (a
 * store under a selection).
 */
#version 450

layout(lines_adjacency) in;
layout(triangle_strip, max_vertices = 4) out;

layout(location = 0) out vec4 g_colour;

void main()
{
    g_colour = vec4(1.0, 0.0, 0.0, 1.0);
    if (gl_in.length() == 4 && gl_in[3].gl_Position.x > 0.5)
        g_colour = vec4(0.0, 0.0, 1.0, 1.0);
    gl_Position = vec4(-1.0, -1.0, 0.0, 1.0);
    EmitVertex();
    gl_Position = vec4(1.0, -1.0, 0.0, 1.0);
    EmitVertex();
    gl_Position = vec4(-1.0, 1.0, 0.0, 1.0);
    EmitVertex();
    gl_Position = vec4(1.0, 1.0, 0.0, 1.0);
    EmitVertex();
    EndPrimitive();
}
