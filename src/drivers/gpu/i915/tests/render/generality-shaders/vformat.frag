/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The generality test's vertex format step, fragment stage:
 * the twelve words of the flat input array, copied into a local one and
 * picked through a dynamic index, word x % 12 at column x.
 */
#version 450

layout(location = 0) flat in uvec4 words[3];

layout(location = 0) out vec4 color;

/* The bytes of a word, low byte in red, as an RGBA8 target stores them. */
#define ENCODE(w) (vec4(float((w) & 255u), float(((w) >> 8) & 255u), float(((w) >> 16) & 255u), float((w) >> 24)) * (1.0 / 255.0))

void main()
{
    uvec4 copied[3];
    int k;

    copied = words;
    k = int(gl_FragCoord.x) % 12;
    color = ENCODE(copied[k >> 2][k & 3]);
}
