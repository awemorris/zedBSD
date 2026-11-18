/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * 2^x, log2, pow and inversesqrt.
 */
#version 450

layout(location = 0) in vec4 v;

layout(location = 0) out vec4 color;

void main()
{
    color = vec4(exp2(v.z), log2(v.y), pow(v.y, v.w), inversesqrt(v.y));
}
