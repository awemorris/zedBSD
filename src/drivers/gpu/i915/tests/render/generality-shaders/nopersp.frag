/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The generality test's interpolation step, fragment stage of
 * the NOPERSP draw: the corners' x interpolated without perspective is the
 * pixel centre's own x in normalized device coordinates, (x + 0.5) / 32 - 1,
 * so 512 (x + 1) is 16 x + 8, a whole number.  Every pixel writes it in the
 * low half and the flat corner number in the top bits.
 */
#version 450

layout(location = 0) in Corner {
    noperspective float linear_x;
    float perspective_x;
    flat int number;
} corner;

layout(location = 0) out vec4 color;

/* The bytes of a word, low byte in red, as an RGBA8 target stores them. */
#define ENCODE(w) (vec4(float((w) & 255u), float(((w) >> 8) & 255u), float(((w) >> 16) & 255u), float((w) >> 24)) * (1.0 / 255.0))

void main()
{
    uint word;

    word = uint(round((corner.linear_x + 1.0) * 512.0));
    word |= uint(corner.number & 3) << 30;

    color = ENCODE(word);
}
