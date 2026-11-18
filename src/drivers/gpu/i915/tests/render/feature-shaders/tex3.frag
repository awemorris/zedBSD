/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Samples three textures from two descriptor sets at the coordinate the
 * vertex colour carries, and writes the red of the first, the green of the
 * second and the blue of the third: a texture read through the wrong
 * binding table entry or sampler shows in its channel.
 */
#version 450

layout(location = 0) in vec4 vertex_color;

layout(location = 0) out vec4 fragment_color;

layout(set = 0, binding = 0) uniform sampler2D first_texture;
layout(set = 0, binding = 2) uniform sampler2D second_texture;
layout(set = 1, binding = 1) uniform sampler2D third_texture;

void main()
{
    vec4 first = texture(first_texture, vertex_color.xy);
    vec4 second = texture(second_texture, vertex_color.xy);
    vec4 third = texture(third_texture, vertex_color.xy);

    fragment_color = vec4(first.r, second.g, third.b, 1.0);
}
