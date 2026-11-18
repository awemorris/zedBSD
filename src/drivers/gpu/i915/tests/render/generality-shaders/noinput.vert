/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Places one vertex of the generality test's no-input step and
 * passes nothing on: the fragment shader reads no varying, so the setup
 * stage gets no attribute.
 */
#version 450

layout(location = 0) in vec4 position;

void main()
{
    gl_Position = position;
}
