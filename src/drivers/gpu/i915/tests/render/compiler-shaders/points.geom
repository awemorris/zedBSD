/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * glxtest's points shader (userland/x11/glxtest/gl32.c) in Vulkan GLSL: a
 * point becomes a square of four vertices around it, of the push constant's
 * colour.
 */
#version 450

layout(points) in;
layout(triangle_strip, max_vertices = 4) out;

layout(push_constant) uniform Colour {
    vec4 colour;
} u;

layout(location = 0) out vec4 g_colour;

void main()
{
    vec4 centre = gl_in[0].gl_Position;
    g_colour = u.colour;
    gl_Position = centre + vec4(-1.0, -1.0, 0.0, 0.0);
    EmitVertex();
    gl_Position = centre + vec4(1.0, -1.0, 0.0, 0.0);
    EmitVertex();
    gl_Position = centre + vec4(-1.0, 1.0, 0.0, 0.0);
    EmitVertex();
    gl_Position = centre + vec4(1.0, 1.0, 0.0, 0.0);
    EmitVertex();
    EndPrimitive();
}
