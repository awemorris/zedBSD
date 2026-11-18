/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The generality test's no-input step: a fragment shader that
 * reads no input, only gl_FragCoord, behind a vertex shader that writes no
 * varying (3DSTATE_SBE with no attribute).  Every pixel writes its x and y
 * as bytes and a mark in the top byte.
 */
#version 450

layout(location = 0) out vec4 color;

/* The bytes of a word, low byte in red, as an RGBA8 target stores them. */
#define ENCODE(w) (vec4(float((w) & 255u), float(((w) >> 8) & 255u), float(((w) >> 16) & 255u), float((w) >> 24)) * (1.0 / 255.0))

void main()
{
    uint word;

    word = uint(gl_FragCoord.x) | (uint(gl_FragCoord.y) << 8) | 0xa5000000u;
    color = ENCODE(word);
}
