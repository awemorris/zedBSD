/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Samples the texture of the executor test's mip steps at the coordinate
 * the vertex colour carries in its red and green.
 *
 * texture() takes its level of detail from the coordinate's rate of change
 * across the pixel quad, so a quad drawn smaller than the texture reads a
 * smaller mip level.
 */
#version 450

layout(location = 0) in vec4 vertex_color;

layout(location = 0) out vec4 fragment_color;

layout(set = 0, binding = 0) uniform sampler2D mip_texture;

void main()
{
    fragment_color = texture(mip_texture, vertex_color.xy);
}
