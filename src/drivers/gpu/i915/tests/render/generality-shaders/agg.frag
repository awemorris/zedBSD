/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The generality test's aggregate step: a local array written
 * and read through dynamic indices, in a loop and after it, a constant
 * array read through a dynamic index, a local array of structures holding
 * an array (copied whole, a member of a dynamically indexed element
 * changed, an element read whole through a dynamic index).  Every pixel
 * writes the 32 bits of a mix of what it read.
 */
#version 450

layout(location = 0) in vec4 pixel;

layout(location = 0) out vec4 color;

/* The bytes of a word, low byte in red, as an RGBA8 target stores them. */
#define ENCODE(w) (vec4(float((w) & 255u), float(((w) >> 8) & 255u), float(((w) >> 16) & 255u), float((w) >> 24)) * (1.0 / 255.0))

struct Pair {
    int a;
    ivec2 b;
    float c[2];
};

const int table[5] = int[5](3, -7, 11, 19, -23);

void main()
{
    int x;
    int y;
    int i;
    int values[8] = int[8](0, 0, 0, 0, 0, 0, 0, 0);
    Pair pairs[2];
    Pair picked;
    uint r;

    x = int(floor(pixel.x));
    y = int(floor(pixel.y));

    for (i = 0; i < 8; i++)
        values[i] = (x * (i + 1)) ^ (y << i);
    values[(x + y) & 7] += table[y % 5];

    pairs[0].a = x;
    pairs[0].b = ivec2(y, x + y);
    pairs[0].c[0] = float(x) * 0.5;
    pairs[0].c[1] = float(y);
    pairs[1] = pairs[0];
    pairs[1].b.y = values[y & 7];
    pairs[x & 1].a += 100;
    picked = pairs[y & 1];

    r = uint(values[x & 7]) * 31u + uint(values[(x * 3 + y) & 7]);
    r ^= uint(pairs[0].a) + uint(pairs[1].a) * 7u + uint(pairs[1].b.y) * 13u + uint(picked.b.x) + uint(picked.c[x & 1] * 2.0);
    r += uint(picked.a) << 20;

    color = ENCODE(r);
}
