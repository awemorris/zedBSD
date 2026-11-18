/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Passes sixteen varyings to the generality test's varying steps: the pixel
 * coordinate at location 0, and at location k (1 .. 15) the seed times
 * k + 1 plus (k, -k, k / 2, 16 - k), the same at every vertex.
 */
#version 450

layout(location = 0) in vec4 position;
layout(location = 1) in vec4 coordinate;
layout(location = 2) in vec4 seed;

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
    v1 = seed * 2.0 + vec4(1.0, -1.0, 0.5, 15.0);
    v2 = seed * 3.0 + vec4(2.0, -2.0, 1.0, 14.0);
    v3 = seed * 4.0 + vec4(3.0, -3.0, 1.5, 13.0);
    v4 = seed * 5.0 + vec4(4.0, -4.0, 2.0, 12.0);
    v5 = seed * 6.0 + vec4(5.0, -5.0, 2.5, 11.0);
    v6 = seed * 7.0 + vec4(6.0, -6.0, 3.0, 10.0);
    v7 = seed * 8.0 + vec4(7.0, -7.0, 3.5, 9.0);
    v8 = seed * 9.0 + vec4(8.0, -8.0, 4.0, 8.0);
    v9 = seed * 10.0 + vec4(9.0, -9.0, 4.5, 7.0);
    v10 = seed * 11.0 + vec4(10.0, -10.0, 5.0, 6.0);
    v11 = seed * 12.0 + vec4(11.0, -11.0, 5.5, 5.0);
    v12 = seed * 13.0 + vec4(12.0, -12.0, 6.0, 4.0);
    v13 = seed * 14.0 + vec4(13.0, -13.0, 6.5, 3.0);
    v14 = seed * 15.0 + vec4(14.0, -14.0, 7.0, 2.0);
    v15 = seed * 16.0 + vec4(15.0, -15.0, 7.5, 1.0);
    gl_Position = position;
}
