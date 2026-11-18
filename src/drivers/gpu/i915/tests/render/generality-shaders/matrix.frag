/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The generality test's matrix step: products of matrices from a uniform
 * block (column-major and row-major, a mat3 with MatrixStride 16) and from
 * push constants (row-major and column-major), a function-local matrix
 * written by column and replaced under a condition, a transpose, an outer
 * product, a matrix times a scalar and a vector times a matrix.  Every
 * pixel writes the bits of one of the results, chosen by its coordinate.
 * The inputs are small multiples of 0.25, so every result is exact.
 */
#version 450

layout(location = 0) in vec4 pixel;

layout(location = 0) out vec4 color;

layout(set = 0, binding = 0, std140) uniform Matrices {
    mat4 m;
    layout(row_major) mat4 r;
    mat3 m3;
    vec4 extra;
} u;

layout(push_constant, std430) uniform Push {
    layout(row_major) mat4 p;
    mat4 q;
} pc;

/* The bytes of a word, low byte in red, as an RGBA8 target stores them. */
#define ENCODE(w) (vec4(float((w) & 255u), float(((w) >> 8) & 255u), float(((w) >> 16) & 255u), float((w) >> 24)) * (1.0 / 255.0))

void main()
{
    int x;
    int y;
    int k;
    int j;
    vec4 v;
    mat4 l;
    vec4 a;
    vec4 b;
    vec4 c;
    mat4x3 o;
    vec3 d;
    vec3 e;
    vec3 g;
    mat2 n2;
    vec2 w2;
    vec4 s0;
    vec4 s1;
    vec4 s2;
    vec4 s3;
    vec4 s;
    float f;

    x = int(floor(pixel.x));
    y = int(floor(pixel.y));
    v = vec4(float(x & 7) - 3.0, float(y & 3) * 0.5, float((x >> 4) & 3), 1.0 + float(y >> 3));

    l = u.m * u.r;
    l[2] = l[2] + v;
    if ((y & 8) == 8)
        l = transpose(l);

    a = l * v;
    b = v * pc.p;
    c = (pc.q * 0.5) * v;
    o = outerProduct(v.xyz, a);
    d = o * vec4(1.0, -2.0, 0.5, 3.0);
    e = u.m3 * v.zyx + u.extra.xyz;
    g = transpose(u.m3) * v.xyz;
    n2 = mat2(v.xy, a.zw);
    w2 = n2 * vec2(2.0, -1.0);

    if ((y & 4) == 0) {
        s0 = a;
        s1 = b;
        s2 = c;
        s3 = vec4(d, e.x);
    } else {
        s0 = vec4(e.yz, g.xy);
        s1 = vec4(g.z, w2, l[0].x);
        s2 = vec4(l[1].y, l[3].z, dot(a, b), o[3].x);
        s3 = vec4(o[3].yz, (pc.p * v).x, (u.r * v).w);
    }

    k = x & 15;
    if (k < 4)
        s = s0;
    else if (k < 8)
        s = s1;
    else if (k < 12)
        s = s2;
    else
        s = s3;

    j = k & 3;
    if (j == 0)
        f = s.x;
    else if (j == 1)
        f = s.y;
    else if (j == 2)
        f = s.z;
    else
        f = s.w;

    color = ENCODE(floatBitsToUint(f));
}
