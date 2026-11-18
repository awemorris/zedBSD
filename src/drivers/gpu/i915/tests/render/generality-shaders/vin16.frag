/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Writes the bits of vin16.vert's weighted sum of attributes, its
 * components weighed 1, 3, 5 and 7, plus the pixel coordinate.
 */
#version 450

layout(location = 0) in vec4 pixel;
layout(location = 3) in vec4 sum;

layout(location = 0) out vec4 color;

/* The bytes of a word, low byte in red, as an RGBA8 target stores them. */
#define ENCODE(w) (vec4(float((w) & 255u), float(((w) >> 8) & 255u), float(((w) >> 16) & 255u), float((w) >> 24)) * (1.0 / 255.0))

void main()
{
    float s;

    s = floor(pixel.x) * 1000.0 + floor(pixel.y) * 100000.0 + dot(sum, vec4(1.0, 3.0, 5.0, 7.0));
    color = ENCODE(floatBitsToUint(s));
}
