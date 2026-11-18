/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Writes mview's varyings as the colour: the texture coordinate and the shade.
 */
#version 450

layout(location = 0) in vec2 texture_coordinate;
layout(location = 1) in float shade;

layout(location = 0) out vec4 color;

void main()
{
    color = vec4(texture_coordinate, shade, 1.0);
}
