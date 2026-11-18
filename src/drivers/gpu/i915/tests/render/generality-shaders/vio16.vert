/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Reads sixteen vertex attributes and writes sixteen varyings: the position
 * at location 0, the pixel coordinate at 1 and data at 2 .. 15 in; the
 * coordinate at location 0 and, at location k (1 .. 15), data weighted by
 * k + 1 plus the next data attribute out.  The payload and a staged VUE do
 * not fit the registers together, so the compiler gathers the VUE at the end
 * and spills what does not fit.
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

layout(location = 0) out vec4 v0;
layout(location = 1) out vec4 v1;
layout(location = 2) out vec4 v2;
layout(location = 3) out vec4 v3;
layout(location = 4) out vec4 v4;
layout(location = 5) out vec4 v5;
layout(location = 6) out vec4 v6;
layout(location = 7) out vec4 v7;
layout(location = 8) out vec4 v8;
layout(location = 9) out vec4 v9;
layout(location = 10) out vec4 v10;
layout(location = 11) out vec4 v11;
layout(location = 12) out vec4 v12;
layout(location = 13) out vec4 v13;
layout(location = 14) out vec4 v14;
layout(location = 15) out vec4 v15;

void main()
{
    v0 = coordinate;
    v1 = a2 * 2.0 + a3;
    v2 = a3 * 3.0 + a4;
    v3 = a4 * 4.0 + a5;
    v4 = a5 * 5.0 + a6;
    v5 = a6 * 6.0 + a7;
    v6 = a7 * 7.0 + a8;
    v7 = a8 * 8.0 + a9;
    v8 = a9 * 9.0 + a10;
    v9 = a10 * 10.0 + a11;
    v10 = a11 * 11.0 + a12;
    v11 = a12 * 12.0 + a13;
    v12 = a13 * 13.0 + a14;
    v13 = a14 * 14.0 + a15;
    v14 = a15 * 15.0 + a2;
    v15 = a2 * 16.0 + a15;
    gl_Position = position;
}
