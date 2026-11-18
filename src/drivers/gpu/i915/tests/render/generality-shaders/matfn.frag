/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The generality test's matrix function step: determinant of
 * a mat2, a mat3 and a mat4, inverse of each, and the half-float packing.
 * The matrices are made of small integers from the pixel, their
 * determinants of powers of two where they are inverted, so every result
 * is exact but for the reciprocal of the determinant.  Row groups of eight
 * pick what a pixel writes: the bits of the determinants' sum, an element
 * of an inverse (column x & 3, row x >> 2 & 3 of the mat4's; the mat3's and
 * the mat2's in the next group), PackHalf2x16 of two floats of the pixel,
 * and the bits of the sum of UnpackHalf2x16 of a word of normal halves.
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
    int group;
    float a;
    float b;
    float c;
    float d;
    mat2 m2;
    mat3 m3;
    mat4 m4;
    mat3 g3;
    mat4 inv4;
    mat3 inv3;
    mat2 inv2;
    float sum;
    uint word;
    vec2 back;

    x = int(floor(pixel.x));
    y = int(floor(pixel.y));
    group = (y >> 3) & 3;

    a = float((x & 3) - 1);
    b = float((y & 3) - 2);
    c = float((x >> 3) & 3);
    d = float((y >> 5) + 1);

    m2 = mat2(2.0, a, 0.0, 1.0);
    m3 = mat3(1.0, a, b, 0.0, 2.0, c, 0.0, 0.0, 4.0);
    m4 = mat4(1.0, 0.0, 0.0, 0.0, a, 1.0, 0.0, 0.0, b, c, 2.0, 0.0, d, a, b, 1.0);
    g3 = mat3(1.0, a, b, c, d, 1.0, 2.0, a, 0.0);

    if (group == 0) {
        sum = determinant(m2) + determinant(m3) * 4.0 + determinant(m4) * 64.0 + determinant(g3) * 256.0;
        word = floatBitsToUint(sum);
    } else if (group == 1) {
        inv4 = inverse(m4);
        word = floatBitsToUint(inv4[x & 3][(x >> 2) & 3]);
    } else if (group == 2) {
        inv3 = inverse(m3);
        inv2 = inverse(m2);
        if ((x & 8) == 0)
            word = floatBitsToUint(inv3[x % 3][(x >> 4) % 3]);
        else
            word = floatBitsToUint(inv2[x & 1][(x >> 1) & 1]);
    } else if ((x & 1) == 0) {
        word = packHalf2x16(vec2(a * 0.3 + float(y), b * 1.7 - float(x)));
    } else {
        back = unpackHalf2x16(((uint(x * 40503 + y * 977) & 0x83ff83ffu) | 0x3c004000u));
        word = floatBitsToUint(back.x * 4.0 + back.y);
    }

    color = ENCODE(word);
}
