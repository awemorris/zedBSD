/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The generality test's float step.  Row y runs operation y & 7: mod by a
 * positive and by a negative divisor (x - y * floor(x / y), the quotient
 * never near a whole number), roundEven and round of halves (ties to
 * even), trunc, ceil and sign, step and floor, smoothstep, fract and abs.
 * Every pixel writes the bits of its result.
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
    float fx;
    float fy;
    float h;
    float r;

    x = int(floor(pixel.x));
    y = int(floor(pixel.y));
    op = y & 7;
    fx = float(x) * 0.53 + 0.11 - 11.0;
    fy = float(y >> 3) * 0.53 + 0.73;
    h = float(x - 32) * 0.5;

    if (op == 0)
        r = mod(fx, fy);
    else if (op == 1)
        r = mod(fx, -fy);
    else if (op == 2)
        r = roundEven(h);
    else if (op == 3)
        r = round(h) + trunc(h * 0.75);
    else if (op == 4)
        r = ceil(h * 0.75) + sign(h) * 4.0;
    else if (op == 5)
        r = step(1.0, h) + floor(h * 0.3);
    else if (op == 6)
        r = smoothstep(-16.0, 16.0, h);
    else
        r = fract(fx) + abs(fx);

    color = ENCODE(floatBitsToUint(r));
}
