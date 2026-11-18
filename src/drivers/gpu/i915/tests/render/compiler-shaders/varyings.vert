/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The vertex shader before varyings.geom: it writes the
 * locations the geometry shader reads -- a colour at 0, the block's uv at 1
 * and its flat id at 2 -- so a three-stage pipeline links.
 */
#version 450

layout(location = 0) in vec4 position;
layout(location = 1) in vec4 value;

layout(location = 0) out vec3 v_colour;

layout(location = 1) out Data {
    vec2 uv;
    flat int id;
} data;

void main()
{
    v_colour = value.rgb;
    data.uv = value.xy;
    data.id = int(value.w);
    gl_Position = position;
}
