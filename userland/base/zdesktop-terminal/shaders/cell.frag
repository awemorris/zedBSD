// zedBSD zdesktop-terminal: a character cell, its glyph's coverage between
// the background and the foreground.
// Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
#version 450

layout(set = 0, binding = 0) uniform sampler2D atlas;

layout(location = 0) in vec2 texcoord;
layout(location = 1) in vec4 foreground;
layout(location = 2) in vec4 background;
layout(location = 0) out vec4 color;

void main()
{
	float cover = texture(atlas, texcoord).a;

	color = vec4(mix(background.rgb, foreground.rgb, cover), 1.0);
}
