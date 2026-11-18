/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The generality test's discard-in-a-loop step.  Each pixel
 * loops x & 15 times and discards in the pass y & 15, so in one SIMD8
 * dispatch some channels leave the loop by discard while the others go
 * on, and a dispatch whose channels all discard ends early.  A pixel that
 * stays writes its accumulator with the top byte set; a discarded one keeps
 * the clear colour (zero).
 */
#version 450

layout(location = 0) in vec4 pixel;

layout(location = 0) out vec4 color;

/* The bytes of a word, low byte in red, as an RGBA8 target stores them. */
#define ENCODE(w) (vec4(float((w) & 255u), float(((w) >> 8) & 255u), float(((w) >> 16) & 255u), float((w) >> 24)) * (1.0 / 255.0))

void main()
{
    int x;
    int y;
    int i;
    uint acc;

    x = int(floor(pixel.x));
    y = int(floor(pixel.y));
    acc = 1u;
    for (i = 0; i < (x & 15); i++) {
        if (i == (y & 15))
            discard;
        acc = acc * 3u + uint(i);
    }

    color = ENCODE((acc & 0x00ffffffu) | 0x81000000u);
}
