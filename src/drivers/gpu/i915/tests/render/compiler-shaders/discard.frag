/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Discards an 8x8 checkerboard, whole dispatches at a time, and inside the top
 * half every other column right of x = 16, a pixel at a time.
 */
#version 450

layout(location = 0) in vec4 v;

layout(location = 0) out vec4 color;

void main()
{
    float cell = floor(v.x * 0.125) + floor(v.y * 0.125);

    if (fract(cell * 0.5) > 0.25) {
        discard;
    }

    vec4 c = vec4(0.5, 0.25, 1.0, 1.0);

    if (v.y < 32.0) {
        if (fract(v.x * 0.5) < 0.5 && v.x > 16.0) {
            discard;
        }
        c.r = 1.0;
    }

    color = c;
}
