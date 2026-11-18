/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The generality test's pixel position step: gl_FragCoord.
 * Every pixel writes its x and y as bytes, then one bit each for x and y
 * being the pixel's centre (a fraction of one half), z being the quad's
 * depth 0, w being 1 / w of the quad's vertices (1), and the position
 * agreeing with the interpolated pixel coordinate.
 */
#version 450

layout(location = 0) in vec4 pixel;

layout(location = 0) out vec4 color;

/* The bytes of a word, low byte in red, as an RGBA8 target stores them. */
#define ENCODE(w) (vec4(float((w) & 255u), float(((w) >> 8) & 255u), float(((w) >> 16) & 255u), float((w) >> 24)) * (1.0 / 255.0))

void main()
{
    vec4 f;
    uint word;

    f = gl_FragCoord;
    word = uint(f.x) | (uint(f.y) << 8);
    if (fract(f.x) == 0.5)
        word |= 1u << 16;
    if (fract(f.y) == 0.5)
        word |= 1u << 17;
    if (f.z == 0.0)
        word |= 1u << 18;
    if (f.w == 1.0)
        word |= 1u << 19;
    if (floor(pixel.x) == floor(f.x) && floor(pixel.y) == floor(f.y))
        word |= 1u << 20;

    color = ENCODE(word);
}
