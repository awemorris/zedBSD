/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The GLSL 1.50 geometry test (glsl150-geometry.geom) in Vulkan GLSL: located per-vertex inputs, an
 * array of interface blocks, gl_PrimitiveIDIn read and gl_PrimitiveID and
 * gl_Layer written.
 */
#version 450

layout(triangles) in;
layout(triangle_strip, max_vertices = 6) out;

layout(location = 0) in vec3 v_colour[];

layout(location = 1) in Data {
    vec2 uv;
    flat int id;
} data[];

layout(push_constant) uniform Offset {
    float offset;
} u;

layout(location = 0) out vec3 g_colour;
layout(location = 1) flat out int g_id;

void main()
{
    for (int copy = 0; copy < 2; copy++) {
        for (int i = 0; i < gl_in.length(); i++) {
            gl_Position = gl_in[i].gl_Position + vec4(u.offset * float(copy), 0.0, 0.0, 0.0);
            g_colour = v_colour[i] * vec3(data[i].uv, 1.0);
            g_id = data[i].id + gl_PrimitiveIDIn;
            gl_Layer = 0;
            gl_PrimitiveID = gl_PrimitiveIDIn;
            EmitVertex();
        }
        EndPrimitive();
    }
}
