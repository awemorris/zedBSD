// zedBSD Notes: the toolbar's picture, drawn on the CPU.  The picture is
// straight alpha; the result is premultiplied by its alpha.
// Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
#version 450

layout(set = 0, binding = 0) uniform sampler2D picture;

layout(location = 0) in vec2 extra;
layout(location = 1) in vec4 color;
layout(location = 0) out vec4 result;

void main()
{
	vec4 straight = texture(picture, extra) * color;
	result = vec4(straight.rgb * straight.a, straight.a);
}
