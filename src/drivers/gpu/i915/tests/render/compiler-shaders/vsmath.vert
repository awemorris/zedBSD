/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Computes normalize, max and clamp in the vertex stage, the way mview's vertex
 * shader lights a vertex, and hands the results on as the varying.
 */
#version 450

layout(location = 0) in vec4 position;
layout(location = 1) in vec4 value;

layout(location = 0) out vec4 v;

void main()
{
    vec3 n = normalize(value.xyz);
    float lambert = max(dot(n, vec3(0.267261, 0.534522, 0.801784)), 0.0);

    v = vec4(n.x, n.y, lambert, clamp(value.w, 0.0, 1.0));
    gl_Position = position;
}
