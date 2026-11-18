/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Absolute value, floor, fraction and square root.
 */
#version 450

layout(location = 0) in vec4 v;

layout(location = 0) out vec4 color;

void main()
{
    color = vec4(abs(v.x), floor(v.x), fract(v.x), sqrt(v.y));
}
