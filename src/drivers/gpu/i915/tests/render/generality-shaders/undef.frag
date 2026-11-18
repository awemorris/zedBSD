/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The generality test's undefined-result step.  Even rows
 * write a defined word, the guard; odd rows run, by (y >> 1) & 7, an
 * operation whose result SPIR-V leaves undefined: signed and unsigned
 * division and remainder by a zero made at run time, INT_MIN / -1 and
 * INT_MIN % -1, and shifts by 32 or more.  The step passes when the draw
 * finishes and every guard word is right; what the hardware gives on the
 * odd rows is logged, not judged (Vulkan allows any value).
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
    int op;
    int a;
    int zero;
    int minimum;
    int r;

    x = int(floor(pixel.x));
    y = int(floor(pixel.y));
    op = (y >> 1) & 7;
    a = int(uint(x + 1) * 2654435761u);
    zero = x - x;
    minimum = -2147483647 - 1 + zero;

    if ((y & 1) == 0) {
        r = x * 977 + y;
    } else if (op == 0) {
        r = a / zero;
    } else if (op == 1) {
        r = a % zero;
    } else if (op == 2) {
        r = int(uint(a) / uint(zero));
    } else if (op == 3) {
        r = int(uint(a) % uint(zero));
    } else if (op == 4) {
        r = minimum / (zero - 1);
    } else if (op == 5) {
        r = minimum % (zero - 1);
    } else if (op == 6) {
        r = a << (32 + (x & 31));
    } else {
        r = int(uint(a) >> (33 + (x & 15)));
    }

    color = ENCODE(uint(r));
}
