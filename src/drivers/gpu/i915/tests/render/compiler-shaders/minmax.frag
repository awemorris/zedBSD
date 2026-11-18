/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * min, max, clamp and mix.
 */
#version 450

layout(location = 0) in vec4 v;

layout(location = 0) out vec4 color;

void main()
{
    color = vec4(min(v.x, v.z), max(v.x, v.z), clamp(v.x, -0.5, 0.75), mix(v.x, v.z, v.w));
}
