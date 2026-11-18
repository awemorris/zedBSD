/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Colours a cut-out fragment: fragments whose alpha is below one half are
 * discarded, the rest are written opaque.
 */
#version 450

layout(set = 0, binding = 0) uniform sampler2D surface_texture;

layout(location = 0) in vec2 texture_coordinate;
layout(location = 1) in float shade;

layout(location = 0) out vec4 fragment_color;

layout(push_constant) uniform Material {
    layout(offset = 112) vec4 color;
} material;

void main()
{
    vec4 base = texture(surface_texture, texture_coordinate) * material.color;

    if (base.a < 0.5) {
        discard;
    }

    fragment_color = vec4(base.rgb * shade, 1.0);
}
