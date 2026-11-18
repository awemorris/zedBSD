// Keiland compositor: solid quads of the glass look drawn together (ws211-p003), in premultiplied alpha.
// Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
#version 450

// The quad's values from its vertices (panel-batch.vert): its rounded
// rectangle in output pixels, its colour with its opacity, its radius.
layout(location = 0) in vec2 pixel;
layout(location = 1) flat in vec4 quad_box;
layout(location = 2) flat in vec4 quad_color;
layout(location = 3) flat in float quad_radius;
layout(location = 0) out vec4 result;

// Signed distance from a pixel to the rounded rectangle (negative inside), as panel.frag.
float rounded(vec2 point, vec4 box, float radius)
{
	vec2 half_size = box.zw * 0.5;
	vec2 centre = box.xy + half_size;
	vec2 q = abs(point - centre) - half_size + vec2(radius);

	return length(max(q, vec2(0.0))) + min(max(q.x, q.y), 0.0) - radius;
}

void main()
{
	// A solid colour cut by the rounded rectangle, as panel.frag's mode 3 (the opacity is in the colour's alpha).
	float cover = clamp(0.5 - rounded(pixel, quad_box, quad_radius), 0.0, 1.0);

	result = vec4(quad_color.rgb, 1.0) * quad_color.a * cover;
}
