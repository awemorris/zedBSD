/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The comparisons, the logical operations and selections, each result coded
 * as a sum of powers of two so that one wrong bit shows.
 */
#version 450

layout(location = 0) in vec4 v;

layout(location = 0) out vec4 color;

void main()
{
    float order = float(v.x < v.y) +
                  2.0 * float(v.x > v.y) +
                  4.0 * float(v.x <= v.y) +
                  8.0 * float(v.x >= v.y);
    float equality = float(v.x == v.y) +
                     2.0 * float(v.x != v.y) +
                     4.0 * float(v.x < v.y && v.z > v.w) +
                     8.0 * float(v.x < v.y || v.z > v.w);
    float picked = (v.x < v.z) ? v.y : v.w;
    float negated = float(!(v.x < v.y)) + 2.0 * mix(v.z, v.w, v.z < v.w);

    color = vec4(order, equality, picked, negated);
}
