/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * A geometry shader that spills: 96 values read from the
 * input vertices stay live over three emits, each vertex summing all of
 * them with weights of its own, so they do not fit the registers below the
 * staged VUE and some go to scratch memory.  Every value is an integer, so
 * the sums are exact in any order.
 */
#version 450

layout(triangles) in;
layout(triangle_strip, max_vertices = 3) out;

layout(location = 0) out vec4 g_sum;

void main()
{
    float a0 = gl_in[0].gl_Position.x + 0.0;
    float a1 = gl_in[1].gl_Position.x + 1.0;
    float a2 = gl_in[2].gl_Position.x + 2.0;
    float a3 = gl_in[0].gl_Position.y + 3.0;
    float a4 = gl_in[1].gl_Position.y + 4.0;
    float a5 = gl_in[2].gl_Position.y + 5.0;
    float a6 = gl_in[0].gl_Position.z + 6.0;
    float a7 = gl_in[1].gl_Position.z + 7.0;
    float a8 = gl_in[2].gl_Position.z + 8.0;
    float a9 = gl_in[0].gl_Position.w + 9.0;
    float a10 = gl_in[1].gl_Position.w + 10.0;
    float a11 = gl_in[2].gl_Position.w + 11.0;
    float a12 = gl_in[0].gl_Position.x + 12.0;
    float a13 = gl_in[1].gl_Position.x + 13.0;
    float a14 = gl_in[2].gl_Position.x + 14.0;
    float a15 = gl_in[0].gl_Position.y + 15.0;
    float a16 = gl_in[1].gl_Position.y + 16.0;
    float a17 = gl_in[2].gl_Position.y + 17.0;
    float a18 = gl_in[0].gl_Position.z + 18.0;
    float a19 = gl_in[1].gl_Position.z + 19.0;
    float a20 = gl_in[2].gl_Position.z + 20.0;
    float a21 = gl_in[0].gl_Position.w + 21.0;
    float a22 = gl_in[1].gl_Position.w + 22.0;
    float a23 = gl_in[2].gl_Position.w + 23.0;
    float a24 = gl_in[0].gl_Position.x + 24.0;
    float a25 = gl_in[1].gl_Position.x + 25.0;
    float a26 = gl_in[2].gl_Position.x + 26.0;
    float a27 = gl_in[0].gl_Position.y + 27.0;
    float a28 = gl_in[1].gl_Position.y + 28.0;
    float a29 = gl_in[2].gl_Position.y + 29.0;
    float a30 = gl_in[0].gl_Position.z + 30.0;
    float a31 = gl_in[1].gl_Position.z + 31.0;
    float a32 = gl_in[2].gl_Position.z + 32.0;
    float a33 = gl_in[0].gl_Position.w + 33.0;
    float a34 = gl_in[1].gl_Position.w + 34.0;
    float a35 = gl_in[2].gl_Position.w + 35.0;
    float a36 = gl_in[0].gl_Position.x + 36.0;
    float a37 = gl_in[1].gl_Position.x + 37.0;
    float a38 = gl_in[2].gl_Position.x + 38.0;
    float a39 = gl_in[0].gl_Position.y + 39.0;
    float a40 = gl_in[1].gl_Position.y + 40.0;
    float a41 = gl_in[2].gl_Position.y + 41.0;
    float a42 = gl_in[0].gl_Position.z + 42.0;
    float a43 = gl_in[1].gl_Position.z + 43.0;
    float a44 = gl_in[2].gl_Position.z + 44.0;
    float a45 = gl_in[0].gl_Position.w + 45.0;
    float a46 = gl_in[1].gl_Position.w + 46.0;
    float a47 = gl_in[2].gl_Position.w + 47.0;
    float a48 = gl_in[0].gl_Position.x + 48.0;
    float a49 = gl_in[1].gl_Position.x + 49.0;
    float a50 = gl_in[2].gl_Position.x + 50.0;
    float a51 = gl_in[0].gl_Position.y + 51.0;
    float a52 = gl_in[1].gl_Position.y + 52.0;
    float a53 = gl_in[2].gl_Position.y + 53.0;
    float a54 = gl_in[0].gl_Position.z + 54.0;
    float a55 = gl_in[1].gl_Position.z + 55.0;
    float a56 = gl_in[2].gl_Position.z + 56.0;
    float a57 = gl_in[0].gl_Position.w + 57.0;
    float a58 = gl_in[1].gl_Position.w + 58.0;
    float a59 = gl_in[2].gl_Position.w + 59.0;
    float a60 = gl_in[0].gl_Position.x + 60.0;
    float a61 = gl_in[1].gl_Position.x + 61.0;
    float a62 = gl_in[2].gl_Position.x + 62.0;
    float a63 = gl_in[0].gl_Position.y + 63.0;
    float a64 = gl_in[1].gl_Position.y + 64.0;
    float a65 = gl_in[2].gl_Position.y + 65.0;
    float a66 = gl_in[0].gl_Position.z + 66.0;
    float a67 = gl_in[1].gl_Position.z + 67.0;
    float a68 = gl_in[2].gl_Position.z + 68.0;
    float a69 = gl_in[0].gl_Position.w + 69.0;
    float a70 = gl_in[1].gl_Position.w + 70.0;
    float a71 = gl_in[2].gl_Position.w + 71.0;
    float a72 = gl_in[0].gl_Position.x + 72.0;
    float a73 = gl_in[1].gl_Position.x + 73.0;
    float a74 = gl_in[2].gl_Position.x + 74.0;
    float a75 = gl_in[0].gl_Position.y + 75.0;
    float a76 = gl_in[1].gl_Position.y + 76.0;
    float a77 = gl_in[2].gl_Position.y + 77.0;
    float a78 = gl_in[0].gl_Position.z + 78.0;
    float a79 = gl_in[1].gl_Position.z + 79.0;
    float a80 = gl_in[2].gl_Position.z + 80.0;
    float a81 = gl_in[0].gl_Position.w + 81.0;
    float a82 = gl_in[1].gl_Position.w + 82.0;
    float a83 = gl_in[2].gl_Position.w + 83.0;
    float a84 = gl_in[0].gl_Position.x + 84.0;
    float a85 = gl_in[1].gl_Position.x + 85.0;
    float a86 = gl_in[2].gl_Position.x + 86.0;
    float a87 = gl_in[0].gl_Position.y + 87.0;
    float a88 = gl_in[1].gl_Position.y + 88.0;
    float a89 = gl_in[2].gl_Position.y + 89.0;
    float a90 = gl_in[0].gl_Position.z + 90.0;
    float a91 = gl_in[1].gl_Position.z + 91.0;
    float a92 = gl_in[2].gl_Position.z + 92.0;
    float a93 = gl_in[0].gl_Position.w + 93.0;
    float a94 = gl_in[1].gl_Position.w + 94.0;
    float a95 = gl_in[2].gl_Position.w + 95.0;

    g_sum = vec4(a0 * 1.0 + a1 * 2.0 + a2 * 3.0 + a3 * 4.0 + a4 * 5.0 + a5 * 1.0 +
        a6 * 2.0 + a7 * 3.0 + a8 * 4.0 + a9 * 5.0 + a10 * 1.0 + a11 * 2.0 +
        a12 * 3.0 + a13 * 4.0 + a14 * 5.0 + a15 * 1.0 + a16 * 2.0 + a17 * 3.0 +
        a18 * 4.0 + a19 * 5.0 + a20 * 1.0 + a21 * 2.0 + a22 * 3.0 + a23 * 4.0 +
        a24 * 5.0 + a25 * 1.0 + a26 * 2.0 + a27 * 3.0 + a28 * 4.0 + a29 * 5.0 +
        a30 * 1.0 + a31 * 2.0 + a32 * 3.0 + a33 * 4.0 + a34 * 5.0 + a35 * 1.0 +
        a36 * 2.0 + a37 * 3.0 + a38 * 4.0 + a39 * 5.0 + a40 * 1.0 + a41 * 2.0 +
        a42 * 3.0 + a43 * 4.0 + a44 * 5.0 + a45 * 1.0 + a46 * 2.0 + a47 * 3.0 +
        a48 * 4.0 + a49 * 5.0 + a50 * 1.0 + a51 * 2.0 + a52 * 3.0 + a53 * 4.0 +
        a54 * 5.0 + a55 * 1.0 + a56 * 2.0 + a57 * 3.0 + a58 * 4.0 + a59 * 5.0 +
        a60 * 1.0 + a61 * 2.0 + a62 * 3.0 + a63 * 4.0 + a64 * 5.0 + a65 * 1.0 +
        a66 * 2.0 + a67 * 3.0 + a68 * 4.0 + a69 * 5.0 + a70 * 1.0 + a71 * 2.0 +
        a72 * 3.0 + a73 * 4.0 + a74 * 5.0 + a75 * 1.0 + a76 * 2.0 + a77 * 3.0 +
        a78 * 4.0 + a79 * 5.0 + a80 * 1.0 + a81 * 2.0 + a82 * 3.0 + a83 * 4.0 +
        a84 * 5.0 + a85 * 1.0 + a86 * 2.0 + a87 * 3.0 + a88 * 4.0 + a89 * 5.0 +
        a90 * 1.0 + a91 * 2.0 + a92 * 3.0 + a93 * 4.0 + a94 * 5.0 + a95 * 1.0,
        0.0, 0.0, 1.0);
    gl_Position = gl_in[0].gl_Position;
    EmitVertex();

    g_sum = vec4(a0 * 2.0 + a1 * 3.0 + a2 * 4.0 + a3 * 5.0 + a4 * 1.0 + a5 * 2.0 +
        a6 * 3.0 + a7 * 4.0 + a8 * 5.0 + a9 * 1.0 + a10 * 2.0 + a11 * 3.0 +
        a12 * 4.0 + a13 * 5.0 + a14 * 1.0 + a15 * 2.0 + a16 * 3.0 + a17 * 4.0 +
        a18 * 5.0 + a19 * 1.0 + a20 * 2.0 + a21 * 3.0 + a22 * 4.0 + a23 * 5.0 +
        a24 * 1.0 + a25 * 2.0 + a26 * 3.0 + a27 * 4.0 + a28 * 5.0 + a29 * 1.0 +
        a30 * 2.0 + a31 * 3.0 + a32 * 4.0 + a33 * 5.0 + a34 * 1.0 + a35 * 2.0 +
        a36 * 3.0 + a37 * 4.0 + a38 * 5.0 + a39 * 1.0 + a40 * 2.0 + a41 * 3.0 +
        a42 * 4.0 + a43 * 5.0 + a44 * 1.0 + a45 * 2.0 + a46 * 3.0 + a47 * 4.0 +
        a48 * 5.0 + a49 * 1.0 + a50 * 2.0 + a51 * 3.0 + a52 * 4.0 + a53 * 5.0 +
        a54 * 1.0 + a55 * 2.0 + a56 * 3.0 + a57 * 4.0 + a58 * 5.0 + a59 * 1.0 +
        a60 * 2.0 + a61 * 3.0 + a62 * 4.0 + a63 * 5.0 + a64 * 1.0 + a65 * 2.0 +
        a66 * 3.0 + a67 * 4.0 + a68 * 5.0 + a69 * 1.0 + a70 * 2.0 + a71 * 3.0 +
        a72 * 4.0 + a73 * 5.0 + a74 * 1.0 + a75 * 2.0 + a76 * 3.0 + a77 * 4.0 +
        a78 * 5.0 + a79 * 1.0 + a80 * 2.0 + a81 * 3.0 + a82 * 4.0 + a83 * 5.0 +
        a84 * 1.0 + a85 * 2.0 + a86 * 3.0 + a87 * 4.0 + a88 * 5.0 + a89 * 1.0 +
        a90 * 2.0 + a91 * 3.0 + a92 * 4.0 + a93 * 5.0 + a94 * 1.0 + a95 * 2.0,
        1.0, 0.0, 1.0);
    gl_Position = gl_in[1].gl_Position;
    EmitVertex();

    g_sum = vec4(a0 * 3.0 + a1 * 4.0 + a2 * 5.0 + a3 * 1.0 + a4 * 2.0 + a5 * 3.0 +
        a6 * 4.0 + a7 * 5.0 + a8 * 1.0 + a9 * 2.0 + a10 * 3.0 + a11 * 4.0 +
        a12 * 5.0 + a13 * 1.0 + a14 * 2.0 + a15 * 3.0 + a16 * 4.0 + a17 * 5.0 +
        a18 * 1.0 + a19 * 2.0 + a20 * 3.0 + a21 * 4.0 + a22 * 5.0 + a23 * 1.0 +
        a24 * 2.0 + a25 * 3.0 + a26 * 4.0 + a27 * 5.0 + a28 * 1.0 + a29 * 2.0 +
        a30 * 3.0 + a31 * 4.0 + a32 * 5.0 + a33 * 1.0 + a34 * 2.0 + a35 * 3.0 +
        a36 * 4.0 + a37 * 5.0 + a38 * 1.0 + a39 * 2.0 + a40 * 3.0 + a41 * 4.0 +
        a42 * 5.0 + a43 * 1.0 + a44 * 2.0 + a45 * 3.0 + a46 * 4.0 + a47 * 5.0 +
        a48 * 1.0 + a49 * 2.0 + a50 * 3.0 + a51 * 4.0 + a52 * 5.0 + a53 * 1.0 +
        a54 * 2.0 + a55 * 3.0 + a56 * 4.0 + a57 * 5.0 + a58 * 1.0 + a59 * 2.0 +
        a60 * 3.0 + a61 * 4.0 + a62 * 5.0 + a63 * 1.0 + a64 * 2.0 + a65 * 3.0 +
        a66 * 4.0 + a67 * 5.0 + a68 * 1.0 + a69 * 2.0 + a70 * 3.0 + a71 * 4.0 +
        a72 * 5.0 + a73 * 1.0 + a74 * 2.0 + a75 * 3.0 + a76 * 4.0 + a77 * 5.0 +
        a78 * 1.0 + a79 * 2.0 + a80 * 3.0 + a81 * 4.0 + a82 * 5.0 + a83 * 1.0 +
        a84 * 2.0 + a85 * 3.0 + a86 * 4.0 + a87 * 5.0 + a88 * 1.0 + a89 * 2.0 +
        a90 * 3.0 + a91 * 4.0 + a92 * 5.0 + a93 * 1.0 + a94 * 2.0 + a95 * 3.0,
        2.0, 0.0, 1.0);
    gl_Position = gl_in[2].gl_Position;
    EmitVertex();
}
