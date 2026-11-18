/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The generality test's integer step.  Row y runs operation y & 15 on two
 * integers made from the pixel's coordinate by an integer hash: all 32 bits
 * (row group 0, and a large odd divisor in every fourth column), a small
 * signed value (group 1), a byte around zero (group 2) or a negative value
 * (group 3); the divisor is a power of two (1 .. 32768) or not (3, 40, ...),
 * negated in the right half.  The operations: add, subtract, multiply
 * (wrapping), signed division and modulus, unsigned division and remainder,
 * the shifts, the bitwise operations, negate / abs / sign / min / max /
 * clamp, their unsigned forms, the ten comparisons as bits, the
 * conversions both ways and a mix.  Every pixel writes the 32 bits of its
 * result.
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
    int variant;
    uint h;
    int a;
    int b;
    int k;
    int r;
    uint ua;
    uint ub;
    float f;

    x = int(floor(pixel.x));
    y = int(floor(pixel.y));
    op = y & 15;
    variant = y >> 4;

    h = uint(x) * 2654435761u + uint(y) * 2246822519u;
    h = h ^ (h >> 15);
    h = h * 668265263u;
    h = h ^ (h >> 13);

    a = int(h);
    if (variant == 1)
        a = a >> 20;
    else if (variant == 2)
        a = (a & 255) - 128;
    else if (variant == 3)
        a = a | int(0x80000000u);

    k = x & 31;
    if (k < 16)
        b = 1 << k;
    else
        b = (k - 16) * 37 + 3;
    if (((x >> 5) & 1) == 1)
        b = -b;
    if (variant == 0 && (x & 3) == 3)
        b = int(h * 2654435761u) | 1;
    if ((x & 7) == 5 && a != 0)
        b = a;

    if (op == 0) {
        r = a + b;
    } else if (op == 1) {
        r = a - b;
    } else if (op == 2) {
        r = a * b;
    } else if (op == 3) {
        r = a / b;
    } else if (op == 4) {
        r = a % b;
    } else if (op == 5) {
        r = int(uint(a) / uint(b));
    } else if (op == 6) {
        r = int(uint(a) % uint(b));
    } else if (op == 7) {
        r = (a << (x & 31)) ^ (a >> (y & 31)) ^ int(uint(a) >> ((x + y) & 31));
    } else if (op == 8) {
        r = ((a & b) ^ (a | ~b)) + (a ^ b);
    } else if (op == 9) {
        r = (-a) ^ abs(b) ^ (sign(a) * 7) ^ (min(a, b) * 3) ^ max(a, b) ^ clamp(a, -1000, 1000 + x);
    } else if (op == 10) {
        ua = uint(a);
        ub = uint(b);
        r = int(min(ua, ub) ^ (max(ua, ub) >> 1) ^ clamp(ua, min(ub, 1000u), max(ub, 1000u)));
    } else if (op == 11) {
        r = int(a == b) | (int(a != b) << 1) | (int(a < b) << 2) | (int(a <= b) << 3) |
            (int(a > b) << 4) | (int(a >= b) << 5) | (int(uint(a) < uint(b)) << 6) |
            (int(uint(a) <= uint(b)) << 7) | (int(uint(a) > uint(b)) << 8) | (int(uint(a) >= uint(b)) << 9);
    } else if (op == 12) {
        r = floatBitsToInt(float(uint(a)));
    } else if (op == 13) {
        r = floatBitsToInt(float(a));
    } else if (op == 14) {
        f = float(a >> 7) * 0.37;
        r = int(f) * 3 + int(uint(abs(f)) >> 1);
    } else {
        r = int(uint(a) >> 3) - (a >> 3) + (-a) * (b | 1);
    }

    color = ENCODE(uint(r));
}
