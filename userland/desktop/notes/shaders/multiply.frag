// zedBSD Notes: a watercolour's triangle, multiplied into what is under it.
// The blend multiplies the target's colour by this one and keeps its
// alpha, so a colour cs at opacity A (its alpha, faded by a fringe as in
// fill.frag) gives the target's colour times 1 - A + A cs: the PDF's
// Multiply at that opacity over an opaque page.
// Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
#version 450

layout(location = 0) in vec2 extra;
layout(location = 1) in vec4 color;
layout(location = 0) out vec4 result;

void main()
{
	float coverage = 1.0 - smoothstep(0.0, 1.0, abs(extra.x));
	float a = color.a * coverage;

	result = vec4(vec3(1.0 - a) + color.rgb * a, 1.0);
}
