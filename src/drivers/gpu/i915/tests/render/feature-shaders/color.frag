/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* Writes the vertex colour, which the blend steps blend over the target. */
#version 450

layout(location = 0) in vec4 vertex_color;

layout(location = 0) out vec4 fragment_color;

void main()
{
    fragment_color = vertex_color;
}
