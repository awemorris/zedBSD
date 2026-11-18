/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Reads sixteen vertex attributes: the position at location 0, the pixel
 * coordinate at 1 and data at 2 .. 15.  Passes the coordinate at location 0
 * and the data weighted by their locations, summed, at location 3.
 */
#version 450

layout(location = 0) in vec4 position;
layout(location = 1) in vec4 coordinate;
layout(location = 2) in vec4 a2;
layout(location = 3) in vec4 a3;
layout(location = 4) in vec4 a4;
layout(location = 5) in vec4 a5;
layout(location = 6) in vec4 a6;
layout(location = 7) in vec4 a7;
layout(location = 8) in vec4 a8;
layout(location = 9) in vec4 a9;
layout(location = 10) in vec4 a10;
layout(location = 11) in vec4 a11;
layout(location = 12) in vec4 a12;
layout(location = 13) in vec4 a13;
layout(location = 14) in vec4 a14;
layout(location = 15) in vec4 a15;

layout(location = 0) out vec4 pixel;
layout(location = 3) out vec4 sum;

void main()
{
    pixel = coordinate;
    sum = a2 * 2.0;
    sum += a3 * 3.0;
    sum += a4 * 4.0;
    sum += a5 * 5.0;
    sum += a6 * 6.0;
    sum += a7 * 7.0;
    sum += a8 * 8.0;
    sum += a9 * 9.0;
    sum += a10 * 10.0;
    sum += a11 * 11.0;
    sum += a12 * 12.0;
    sum += a13 * 13.0;
    sum += a14 * 14.0;
    sum += a15 * 15.0;
    gl_Position = position;
}
