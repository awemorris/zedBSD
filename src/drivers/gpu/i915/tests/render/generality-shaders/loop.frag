/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The generality test's loop step: loops whose trip counts differ from
 * pixel to pixel, so the eight channels of a dispatch leave them at
 * different passes.  A for loop to x % 17 with a continue and a break, a
 * for loop around a while loop with an if / else and a break inside, a
 * do-while (Collatz steps, at most 40) and a while (true) left by a break,
 * accumulating a float.  Every pixel writes the 32 bits of a mix of the
 * four results.
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
    int n;
    int acc;
    int i;
    int s;
    int j;
    int k;
    int t;
    int c;
    int m;
    float f;
    uint r;

    x = int(floor(pixel.x));
    y = int(floor(pixel.y));

    n = x % 17;
    acc = 1;
    for (i = 0; i < n; i++) {
        if (i == (y & 3))
            continue;
        if (acc > 3000 + y * 16)
            break;
        if ((i & 1) == 0)
            acc = acc * 3 + i;
        else
            acc = acc + (x ^ i);
    }

    s = 0;
    for (j = 0; j < y % 7; j++) {
        k = 0;
        while (k <= x % 5) {
            if (((j + k) & 1) == 0)
                s += j * k + 1;
            else
                s -= k;
            k++;
            if (s > 20 + (y >> 3))
                break;
        }
    }

    t = x + y + 1;
    c = 0;
    do {
        if ((t & 1) == 1)
            t = 3 * t + 1;
        else
            t = t / 2;
        c++;
    } while (t != 1 && c < 40);

    f = 0.0;
    m = 0;
    while (true) {
        if (m >= (y & 7))
            break;
        f += float(m) * 0.5;
        m++;
    }

    r = uint(acc) ^ (uint(s & 255) << 16) ^ (uint(c) << 24) ^ (uint(f * 2.0) << 8);
    color = ENCODE(r);
}
