/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Places one vertex of the executor test: its position moved by the pushed
 * offset, its colour passed on unchanged.
 *
 * The push block is the one the fragment stage reads too: the offset at its
 * start, the colour at byte 112 (the layout of the model viewer's block).
 */
#version 450

layout(location = 0) in vec4 position;
layout(location = 1) in vec4 color;

layout(location = 0) out vec4 vertex_color;

layout(push_constant) uniform Placement {
    vec4 offset;
    layout(offset = 112) vec4 color;
} placement;

void main()
{
    vertex_color = color;
    gl_Position = position + placement.offset;
}
