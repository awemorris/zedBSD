/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Refused: gl_ViewportIndex, since the device has one viewport.
 */
#version 450

layout(points) in;
layout(points, max_vertices = 1) out;

void main()
{
    gl_Position = gl_in[0].gl_Position;
    gl_ViewportIndex = 1;
    EmitVertex();
}
