/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Reads only locations 0, 1, 6, 11 and 15 of vary16.vert's sixteen
 * varyings, declared out of order, and writes the bits of their weighted
 * sum as vary16.frag weighs them.
 */
#version 450

layout(location = 11) in vec4 v11;
layout(location = 1) in vec4 v1;
layout(location = 15) in vec4 v15;
layout(location = 0) in vec4 pixel;
layout(location = 6) in vec4 v6;

layout(location = 0) out vec4 color;

/* The bytes of a word, low byte in red, as an RGBA8 target stores them. */
#define ENCODE(w) (vec4(float((w) & 255u), float(((w) >> 8) & 255u), float(((w) >> 16) & 255u), float((w) >> 24)) * (1.0 / 255.0))

void main()
{
    float s;

    s = floor(pixel.x) * 1000.0 + floor(pixel.y) * 100000.0;
    s += dot(v11, vec4(45.0, 46.0, 47.0, 48.0));
    s += dot(v1, vec4(5.0, 6.0, 7.0, 8.0));
    s += dot(v15, vec4(61.0, 62.0, 63.0, 64.0));
    s += dot(v6, vec4(25.0, 26.0, 27.0, 28.0));
    color = ENCODE(floatBitsToUint(s));
}
