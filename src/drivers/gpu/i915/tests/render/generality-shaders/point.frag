/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The generality test's point step, fragment stage: where in
 * its point the pixel is, gl_PointCoord in sixteenths (a pixel centre of a
 * point of an even size centred on a pixel corner is an odd number of them
 * for a size of 8, (2 k + 1) * 2 for a size of 4), and the point's number.
 * The top byte is 1, which no pixel outside a point has.
 */
#version 450

layout(location = 0) flat in int number;

layout(location = 0) out vec4 color;

/* The bytes of a word, low byte in red, as an RGBA8 target stores them. */
#define ENCODE(w) (vec4(float((w) & 255u), float(((w) >> 8) & 255u), float(((w) >> 16) & 255u), float((w) >> 24)) * (1.0 / 255.0))

void main()
{
    uint word;

    word = uint(round(gl_PointCoord.x * 16.0)) | (uint(round(gl_PointCoord.y * 16.0)) << 8);
    word |= uint(number & 255) << 16;
    word |= 1u << 24;

    color = ENCODE(word);
}
