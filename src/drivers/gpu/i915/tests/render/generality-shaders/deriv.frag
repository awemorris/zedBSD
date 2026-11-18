/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The generality test's derivative step: dFdx, dFdy and fwidth
 * of a value linear in the pixel position (3 x + 5 y: 3, 5 and 8 at every
 * pixel), and the coarse x, fine x and coarse y derivatives of x * y, which
 * tell the rows and columns of a 2x2 quad apart: the coarse ones take the
 * quad's top row and left column (y and x of its first pixel), the fine x
 * one each row's own y.  Every pixel writes the six as bit fields.
 */
#version 450

layout(location = 0) in vec4 pixel;

layout(location = 0) out vec4 color;

/* The bytes of a word, low byte in red, as an RGBA8 target stores them. */
#define ENCODE(w) (vec4(float((w) & 255u), float(((w) >> 8) & 255u), float(((w) >> 16) & 255u), float((w) >> 24)) * (1.0 / 255.0))

void main()
{
    vec2 f;
    float linear;
    float product;
    uint word;

    f = gl_FragCoord.xy;
    linear = f.x * 3.0 + f.y * 5.0;
    product = f.x * f.y;

    word = uint(dFdx(linear)) | (uint(dFdy(linear)) << 2) | (uint(fwidth(linear)) << 5);
    word |= (uint(dFdxCoarse(product) * 2.0) & 127u) << 9;
    word |= (uint(dFdxFine(product) * 2.0) & 127u) << 16;
    word |= (uint(dFdyCoarse(product) * 2.0) & 127u) << 23;

    color = ENCODE(word);
}
