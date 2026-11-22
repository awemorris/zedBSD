// zedBSD terminal: a character cell, its glyph's coverage between
// the background and the foreground, or a colour glyph's own colours
// over the background.
// Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
#version 450

layout(set = 0, binding = 0) uniform sampler2D atlas;

// The foreground's alpha is the cell's mode: 1 reads the atlas as a
// coverage, 0 as premultiplied colours.  The two are mixed by it rather
// than chosen by a branch, which i915's native compiler does not take.
layout(location = 0) in vec2 texcoord;
layout(location = 1) in vec4 foreground;
layout(location = 2) in vec4 background;
layout(location = 0) out vec4 color;

void main()
{
	vec4 texel = texture(atlas, texcoord);
	vec3 coverage = mix(background.rgb, foreground.rgb, texel.a);
	vec3 colour = texel.rgb + background.rgb * (1.0 - texel.a);

	color = vec4(mix(coverage, colour, 1.0 - foreground.a), 1.0);
}
