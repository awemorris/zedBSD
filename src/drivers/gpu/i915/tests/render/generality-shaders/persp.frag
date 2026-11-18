/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The generality test's interpolation step, fragment stage of
 * the PERSP draw: every pixel writes the bits of the corners' x + 3
 * interpolated with perspective, which the kernel test compares within
 * I915_VKE2_LOOSE_ULPS of the value regenerate.py computed.
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
    color = ENCODE(floatBitsToUint(corner.perspective_x));
}
