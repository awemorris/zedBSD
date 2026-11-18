/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* Places one vertex of the feature test where it is and passes its colour on unchanged. */
#version 450

layout(location = 0) in vec4 position;
layout(location = 1) in vec4 color;

layout(location = 0) out vec4 vertex_color;

void main()
{
    vertex_color = color;
    gl_Position = position;
}
