/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The generality test's 16-bit integer step.  The operands are
 * 32-bit boundary values truncated to 16 bits (int16_t of 0x12348000 is
 * -32768, its high half not zero where the compiler carries it), the first
 * by the column (x & 15), the second by the row group and the column's next
 * bits.  Row y runs operation y & 15: signed and unsigned division and
 * remainder (a division SPIR-V leaves undefined writes a marker), the
 * three shifts, wrapping multiply, add and subtract, signed and unsigned
 * minimum and maximum, the comparisons as bits (equality across
 * signedness among them), conversions to float, abs, sign and clamp, and
 * the packing bitcasts with a 16-bit dynamic index.  Every pixel writes
 * the 32 bits of its result.
 */
#version 450
#extension GL_EXT_shader_explicit_arithmetic_types_int16 : require
#extension GL_EXT_shader_explicit_arithmetic_types : require

layout(location = 0) in vec4 pixel;

layout(location = 0) out vec4 color;

/* The bytes of a word, low byte in red, as an RGBA8 target stores them. */
#define ENCODE(w) (vec4(float((w) & 255u), float(((w) >> 8) & 255u), float(((w) >> 16) & 255u), float((w) >> 24)) * (1.0 / 255.0))

/* What a division SPIR-V leaves undefined writes instead of its result. */
#define MARKER 0x5a5a5a5a

const int boundary[16] = int[16](0, 1, -1, 0x12348000, 0x00017fff, 0x7fff, -32768, 0x0003fffe, 2,
    -7, 0x10000, 0xffff, 0x2345ff9c, 100, -100, 0x7ffe0005);

const int table[4] = int[4](11, 22, 33, 44);

void main()
{
    int x;
    int y;
    int op;
    int r;
    int16_t a;
    int16_t b;
    uint16_t ua;
    uint16_t ub;
    int16_t index;
    bool undefined_signed;

    x = int(floor(pixel.x));
    y = int(floor(pixel.y));
    op = y & 15;
    a = int16_t(boundary[x & 15]);
    b = int16_t(boundary[(y >> 4) * 4 + ((x >> 4) & 3)]);
    ua = uint16_t(boundary[x & 15]);
    ub = uint16_t(boundary[(y >> 4) * 4 + ((x >> 4) & 3)]);
    undefined_signed = b == 0s || (a == int16_t(-32768) && b == -1s);

    if (op == 0) {
        r = MARKER;
        if (!undefined_signed)
            r = int(a / b);
    } else if (op == 1) {
        r = MARKER;
        if (!undefined_signed)
            r = int(a % b);
    } else if (op == 2) {
        r = MARKER;
        if (ub != 0us)
            r = int(uint(ua / ub));
    } else if (op == 3) {
        r = MARKER;
        if (ub != 0us)
            r = int(uint(ua % ub));
    } else if (op == 4) {
        r = int(a >> (ub & 15us));
    } else if (op == 5) {
        r = int(uint(ua >> (ub & 15us)));
    } else if (op == 6) {
        r = int(int16_t(a << (ub & 15us)));
    } else if (op == 7) {
        r = int(int16_t(a * b));
    } else if (op == 8) {
        r = int(int16_t(a + b));
    } else if (op == 9) {
        r = int(int16_t(a - b));
    } else if (op == 10) {
        r = int(min(a, b)) ^ (int(max(a, b)) << 8);
    } else if (op == 11) {
        r = int(uint(min(ua, ub)) ^ (uint(max(ua, ub)) << 8));
    } else if (op == 12) {
        r = int(a == b) | (int(a < b) << 1) | (int(a > b) << 2) | (int(ua < ub) << 3) | (int(ua > ub) << 4) |
            (int(ua != uint16_t(b)) << 5) | (int(a >= b) << 6) | (int(ua <= ub) << 7);
    } else if (op == 13) {
        r = floatBitsToInt(float(a)) ^ floatBitsToInt(float(ub));
    } else if (op == 14) {
        r = int(abs(a)) ^ (int(clamp(a, -100s, 100s)) << 16) ^ (int(sign(b)) * 7);
    } else {
        index = int16_t((boundary[x & 15] & 3) | 0x30000);
        r = int(pack32(i16vec2(a, b))) ^ (int(unpack16(uint(boundary[x & 15])).y) * 3) ^ table[index];  /* unpack16 of a uint gives a u16vec2 */
    }

    color = ENCODE(uint(r));
}
