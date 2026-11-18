/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * MIXED: the vertex's colour.
 */
#version 450

layout(location = 0) flat in vec4 shade;

layout(location = 0) out vec4 colour;

void main()
{
    colour = shade;
}
