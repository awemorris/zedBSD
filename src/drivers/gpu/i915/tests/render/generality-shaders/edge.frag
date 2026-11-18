/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The generality test's integer boundary step.  The operands
 * come from a table of boundary values -- zero, one, minus one, the
 * limits of 8, 16 and 32 bits, INT_MIN and INT_MIN + 1, large primes --
 * the first by the column (x & 15), the second by the row group and the
 * column's next bits.  Row y runs operation y & 15: signed division and
 * modulus, unsigned division and remainder (a division SPIR-V leaves
 * undefined, by zero or INT_MIN by -1, writes a marker instead), the
 * three shifts by 0 to 31, wrapping multiply, add and subtract, negation,
 * signed and unsigned minimum and maximum, the comparisons as bits, and
 * conversions to float and back.  Every pixel writes the 32 bits of its
 * result.
 */
#version 450

layout(location = 0) in vec4 pixel;

layout(location = 0) out vec4 color;

/* The bytes of a word, low byte in red, as an RGBA8 target stores them. */
#define ENCODE(w) (vec4(float((w) & 255u), float(((w) >> 8) & 255u), float(((w) >> 16) & 255u), float((w) >> 24)) * (1.0 / 255.0))

/* What a division SPIR-V leaves undefined writes instead of its result. */
#define MARKER 0x5a5a5a5a

const int boundary[16] = int[16](0, 1, -1, 2, -2, 3, -7, 255, 256, 32767, -32768, 2147483647,
    -2147483647 - 1, -2147483647, 1000000007, -1000000007);

void main()
{
    int x;
    int y;
    int op;
    int a;
    int b;
    int r;
    uint ua;
    uint ub;
    bool undefined_signed;

    x = int(floor(pixel.x));
    y = int(floor(pixel.y));
    op = y & 15;
    a = boundary[x & 15];
    b = boundary[(y >> 4) * 4 + ((x >> 4) & 3)];
    ua = uint(a);
    ub = uint(b);
    undefined_signed = b == 0 || (a == -2147483647 - 1 && b == -1);

    if (op == 0) {
        r = MARKER;
        if (!undefined_signed)
            r = a / b;
    } else if (op == 1) {
        r = MARKER;
        if (!undefined_signed)
            r = a % b;
    } else if (op == 2) {
        r = MARKER;
        if (b != 0)
            r = int(ua / ub);
    } else if (op == 3) {
        r = MARKER;
        if (b != 0)
            r = int(ua % ub);
    } else if (op == 4) {
        r = a << (b & 31);
    } else if (op == 5) {
        r = a >> (b & 31);
    } else if (op == 6) {
        r = int(ua >> (b & 31));
    } else if (op == 7) {
        r = a * b;
    } else if (op == 8) {
        r = a + b;
    } else if (op == 9) {
        r = a - b;
    } else if (op == 10) {
        r = -a;
    } else if (op == 11) {
        r = min(a, b) ^ (max(a, b) * 3);
    } else if (op == 12) {
        r = int(min(ua, ub) ^ (max(ua, ub) >> 1));
    } else if (op == 13) {
        r = int(a == b) | (int(a < b) << 1) | (int(a > b) << 2) | (int(ua < ub) << 3) | (int(ua > ub) << 4);
    } else if (op == 14) {
        r = floatBitsToInt(float(a)) ^ floatBitsToInt(float(ub));
    } else {
        r = int(uint(float(ua >> 1)));
    }

    color = ENCODE(uint(r));
}
