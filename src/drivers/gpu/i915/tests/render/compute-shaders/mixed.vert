/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * MIXED: a position and a colour per vertex, the colour flat.
 */
#version 450

layout(location = 0) in vec4 position;
layout(location = 1) in vec4 colour;

layout(location = 0) flat out vec4 shade;

void main()
{
    gl_Position = position;
    shade = colour;
}
