/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Writes the varying as the colour.
 */
#version 450

layout(location = 0) in vec4 v;

layout(location = 0) out vec4 color;

void main()
{
    color = v;
}
