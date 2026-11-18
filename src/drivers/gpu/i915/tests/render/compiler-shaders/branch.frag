/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Nested if / else on the pixel position, with locals, a short-circuit and a
 * selection: the branch goes different ways inside one SIMD8 dispatch.
 */
#version 450

layout(location = 0) in vec4 v;

layout(location = 0) out vec4 color;

void main()
{
    vec4 c = vec4(0.0, 0.0, 0.0, 1.0);
    float t = 1.0;

    if (v.x < 21.0) {
        if (v.y < 11.0) {
            c.r = 1.0;
        } else {
            c.g = 1.0;
            t = t * 2.0;
        }
    } else if (v.y < 33.0 && v.x > 41.0) {
        c.b = 1.0;
        t = 3.0;
    } else {
        c = vec4(0.25);
    }

    float s = (v.x < v.y + 0.5) ? t : -t;

    color = c + vec4(0.0, 0.0, 0.0, s);
}
