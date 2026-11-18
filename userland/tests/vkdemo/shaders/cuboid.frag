/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* Fetch the actual uploaded checker texture with interpolated face UVs. */
#version 450

layout(set = 0, binding = 0) uniform sampler2D checker;
layout(location = 0) in vec2 texture_coordinate;
layout(location = 0) out vec4 fragment_color;

void main()
{
    fragment_color = texture(checker, texture_coordinate);
}
