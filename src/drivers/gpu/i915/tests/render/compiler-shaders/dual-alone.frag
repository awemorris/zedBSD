/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Writes a second colour (Location 0 Index 1) without the first, which the
 * compiler refuses: a dual-source write needs both.
 */
#version 450

layout(location = 0) in vec4 v;

layout(location = 0, index = 1) out vec4 second;

void main()
{
    second = v;
}
