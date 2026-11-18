/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Writes two colours for a dual-source blend: the input as the
 * first (Location 0 Index 0) and its reverse with one minus its first
 * component as the second (Location 0 Index 1).
 */
#version 450

layout(location = 0) in vec4 v;

layout(location = 0, index = 0) out vec4 color;
layout(location = 0, index = 1) out vec4 second;

void main()
{
    color = v;
    second = vec4(v.w, v.z, v.y, 1.0 - v.x);
}
