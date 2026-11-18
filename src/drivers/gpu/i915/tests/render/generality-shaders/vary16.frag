/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Reads all sixteen varyings of vary16.vert and writes the bits of their
 * weighted sum (component c of varying k weighs 4 k + c + 1), plus the
 * pixel coordinate, so a varying read from the wrong slot shows.
 */
#version 450

layout(location = 0) in vec4 pixel;
layout(location = 1) in vec4 v1;
layout(location = 2) in vec4 v2;
layout(location = 3) in vec4 v3;
layout(location = 4) in vec4 v4;
layout(location = 5) in vec4 v5;
layout(location = 6) in vec4 v6;
layout(location = 7) in vec4 v7;
layout(location = 8) in vec4 v8;
layout(location = 9) in vec4 v9;
layout(location = 10) in vec4 v10;
layout(location = 11) in vec4 v11;
layout(location = 12) in vec4 v12;
layout(location = 13) in vec4 v13;
layout(location = 14) in vec4 v14;
layout(location = 15) in vec4 v15;

layout(location = 0) out vec4 color;

/* The bytes of a word, low byte in red, as an RGBA8 target stores them. */
#define ENCODE(w) (vec4(float((w) & 255u), float(((w) >> 8) & 255u), float(((w) >> 16) & 255u), float((w) >> 24)) * (1.0 / 255.0))

void main()
{
    float s;

    s = floor(pixel.x) * 1000.0 + floor(pixel.y) * 100000.0;
    s += dot(v1, vec4(5.0, 6.0, 7.0, 8.0));
    s += dot(v2, vec4(9.0, 10.0, 11.0, 12.0));
    s += dot(v3, vec4(13.0, 14.0, 15.0, 16.0));
    s += dot(v4, vec4(17.0, 18.0, 19.0, 20.0));
    s += dot(v5, vec4(21.0, 22.0, 23.0, 24.0));
    s += dot(v6, vec4(25.0, 26.0, 27.0, 28.0));
    s += dot(v7, vec4(29.0, 30.0, 31.0, 32.0));
    s += dot(v8, vec4(33.0, 34.0, 35.0, 36.0));
    s += dot(v9, vec4(37.0, 38.0, 39.0, 40.0));
    s += dot(v10, vec4(41.0, 42.0, 43.0, 44.0));
    s += dot(v11, vec4(45.0, 46.0, 47.0, 48.0));
    s += dot(v12, vec4(49.0, 50.0, 51.0, 52.0));
    s += dot(v13, vec4(53.0, 54.0, 55.0, 56.0));
    s += dot(v14, vec4(57.0, 58.0, 59.0, 60.0));
    s += dot(v15, vec4(61.0, 62.0, 63.0, 64.0));
    color = ENCODE(floatBitsToUint(s));
}
