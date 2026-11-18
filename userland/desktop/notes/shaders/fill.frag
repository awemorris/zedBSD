// zedBSD Notes: a filled triangle.  A fringe's pixels fade with their
// distance from the stroke's edge (extra.x runs from 0 at the edge to 1 a
// pixel away), which anti-aliases the stroke without multisampling; every
// other triangle has 0 there and is solid.  The result is premultiplied by
// its alpha.
// Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
#version 450

layout(location = 0) in vec2 extra;
layout(location = 1) in vec4 color;
layout(location = 0) out vec4 result;

void main()
{
	float coverage = 1.0 - smoothstep(0.0, 1.0, abs(extra.x));

	float a = color.a * coverage;
	result = vec4(color.rgb * a, a);
}
