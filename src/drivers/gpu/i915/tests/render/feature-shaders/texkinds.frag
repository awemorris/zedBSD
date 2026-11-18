/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The feature test's texture kinds step: every texture 4x4
 * texels (the 1D one 4x1), sampled nearest and clamped to the edge, so the
 * 16-pixel column `region` of a band of 8 rows reads texel column
 * `region` and every 2 rows of the band the next texel row.  The bands:
 * layer 1 of a 2D array; depth 0.625 (slice 2) of a 3D texture; a cube
 * map in four directions, one a region; a 1D texture; texelFetch() of an
 * RGBA8_UINT texture; a depth comparison (LESS_OR_EQUAL 0.5) of an R32
 * float texture; textureGrad() of the 8x8 texture of two levels (with the
 * fine y derivative in the right half); its size and level count,
 * textureProj() and texelFetch() of its level 1.
 */
#version 450

layout(location = 0) in vec4 vertex_color;

layout(location = 0) out vec4 fragment_color;

layout(set = 0, binding = 0) uniform sampler2DArray layered;
layout(set = 0, binding = 1) uniform sampler3D volume;
layout(set = 0, binding = 2) uniform samplerCube cube;
layout(set = 0, binding = 3) uniform sampler1D line;
layout(set = 0, binding = 4) uniform usampler2D integers;
layout(set = 0, binding = 5) uniform sampler2DShadow depth;
layout(set = 0, binding = 6) uniform sampler2D mipped;

void main()
{
    const vec3 directions[4] = vec3[4](vec3(1.0, 0.1, 0.1), vec3(-1.0, 0.1, 0.1), vec3(0.1, 1.0, 0.1),
                                       vec3(0.1, 0.1, -1.0));
    vec2 uv;
    ivec2 texel;
    int band;
    int region;
    float lit;

    uv = vec2(vertex_color.x, fract(gl_FragCoord.y / 8.0));
    band = int(gl_FragCoord.y) >> 3;
    region = int(gl_FragCoord.x) >> 4;
    texel = ivec2(region, (int(gl_FragCoord.y) & 7) >> 1);
    if (band == 0) {
        fragment_color = texture(layered, vec3(uv, 1.0));
    } else if (band == 1) {
        fragment_color = texture(volume, vec3(uv, 0.625));
    } else if (band == 2) {
        fragment_color = texture(cube, directions[region]);
    } else if (band == 3) {
        fragment_color = texture(line, uv.x);
    } else if (band == 4) {
        fragment_color = vec4(texelFetch(integers, texel, 0)) / 255.0;
    } else if (band == 5) {
        lit = texture(depth, vec3(uv, 0.5));
        fragment_color = vec4(lit, lit, lit, 1.0);
    } else if (band == 6) {
        if (region < 2)
            fragment_color = textureGrad(mipped, uv, vec2(0.25, 0.0), vec2(0.0, 0.25));
        else
            fragment_color = textureGrad(mipped, uv, vec2(0.0), dFdyFine(uv) * 16.0);
    } else if (region == 0) {
        fragment_color = vec4(vec2(textureSize(mipped, 0)), float(textureQueryLevels(mipped)), 255.0) / 255.0;
    } else if (region == 1) {
        fragment_color = textureProj(mipped, vec3(uv * 2.0, 2.0));
    } else {
        fragment_color = texelFetch(mipped, texel, 1);
    }
}
