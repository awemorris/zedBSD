// zedBSD System Monitor: a glyph from the atlas (white, its coverage as alpha) in the vertex's colour,
// premultiplied by its alpha.
// Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
#version 450

layout(set = 0, binding = 0) uniform sampler2D atlas;

layout(location = 0) in vec2 local;
layout(location = 1) in vec4 shape;
layout(location = 2) in vec4 color;
layout(location = 3) in vec4 second;
layout(location = 0) out vec4 result;

void main()
{
	float a = color.a * texture(atlas, local).a;
	result = vec4(color.rgb * a, a);
}
