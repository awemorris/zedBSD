// zedBSD Notes: a pencil's triangle through the paper's grain.  The opacity
// a (extra.y) is laid in two coats, a flat one of a times the grain's floor
// and one of a times the grain h under the pixel, so the pixel's alpha is
// 1 - (1 - a floor)(1 - a h) as the PDF's two fills give it; a fringe
// fades it as fill.frag does.  The result is premultiplied by its alpha.
// Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
#version 450

layout(set = 0, binding = 0) uniform sampler2D grain;

layout(location = 0) in vec4 extra;
layout(location = 1) in vec4 color;
layout(location = 0) out vec4 result;

// The share of the opacity the grain never takes away (NOTES_GRAIN_FLOOR).
const float grain_floor = 0.35;

void main()
{
	float coverage = 1.0 - smoothstep(0.0, 1.0, abs(extra.x));
	float a = extra.y;
	float h = texture(grain, extra.zw).r;
	float alpha = coverage * (1.0 - (1.0 - a * grain_floor) * (1.0 - a * h));

	result = vec4(color.rgb * alpha, alpha);
}
