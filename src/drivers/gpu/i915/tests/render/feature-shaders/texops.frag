/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The feature test's texture operand step: an 8x8 texture of
 * two levels (4x4 the second), sampled nearest and clamped to the edge over
 * the whole target, so a pixel is an eighth of a level-0 texel and
 * texture() picks level 0.  Bands of 16 rows: texture(); texture() with a
 * bias of 4 (level 1); textureLod() of level 1; textureOffset() by (1, -1)
 * on level 0 in the left half of the last band, textureLodOffset() of
 * level 1 by (-1, 1) in its right half.
 */
#version 450

layout(location = 0) in vec4 vertex_color;

layout(location = 0) out vec4 fragment_color;

layout(set = 0, binding = 0) uniform sampler2D mipped;

void main()
{
    vec2 uv;
    int band;

    uv = vertex_color.xy;
    band = int(gl_FragCoord.y) >> 4;
    if (band == 0)
        fragment_color = texture(mipped, uv);
    else if (band == 1)
        fragment_color = texture(mipped, uv, 4.0);
    else if (band == 2)
        fragment_color = textureLod(mipped, uv, 1.0);
    else if ((int(gl_FragCoord.x) & 32) == 0)
        fragment_color = textureOffset(mipped, uv, ivec2(1, -1));
    else
        fragment_color = textureLodOffset(mipped, uv, 1.0, ivec2(-1, 1));
}
