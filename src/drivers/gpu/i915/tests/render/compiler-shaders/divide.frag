/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Division, and normalize of a vector.
 */
#version 450

layout(location = 0) in vec4 v;

layout(location = 0) out vec4 color;

void main()
{
    vec3 n = normalize(v.xyz);

    color = vec4(v.x / v.y, 1.0 / v.z, n.x, n.z);
}
