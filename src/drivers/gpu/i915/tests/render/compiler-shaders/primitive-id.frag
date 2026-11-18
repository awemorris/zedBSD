/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Writes the primitive's number as the colour's red: gl_PrimitiveID, a Flat
 * input at its own location.
 */
#version 450

layout(location = 0) out vec4 color;

void main()
{
    color = vec4(float(gl_PrimitiveID), 0.0, 0.0, 1.0);
}
