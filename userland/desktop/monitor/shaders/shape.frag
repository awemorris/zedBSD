// zedBSD System Monitor: the shapes -- a solid fill, a plate (a rounded rectangle with its edge and its lit top),
// a line, a vertical gradient, the area under a graph and a disc (plan/ws134/design.md sections 2.4 and 4.2).
// The result is premultiplied: each shape works out its straight colour and alpha, and the colour is multiplied by
// the alpha last, so the pixels are the same as straight-alpha blending gave.
// Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
#version 450

layout(location = 0) in vec2 local;
layout(location = 1) in vec4 shape;
layout(location = 2) in vec4 color;
layout(location = 3) in vec4 second;
layout(location = 0) out vec4 result;

// The distance from a rounded rectangle of a half size and a corner radius (negative inside).
float rounded(vec2 point, vec2 half_size, float radius)
{
	vec2 q = abs(point) - half_size + vec2(radius);
	return length(max(q, vec2(0.0))) + min(max(q.x, q.y), 0.0) - radius;
}

void main()
{
	float kind = shape.w;
	if (kind < 0.5) {
		// A solid fill.
		result = vec4(color.rgb * color.a, color.a);
	} else if (kind < 1.5) {
		// A plate: the fill lighter at the top, the edge in the second colour, the top's inner edge lit, the corners smooth.
		float d = rounded(local, shape.xy, shape.z);
		float inside = clamp(0.5 - d, 0.0, 1.0);
		float edge = clamp(1.0 - abs(d + 0.75), 0.0, 1.0);
		float t = clamp(0.5 - local.y / (2.0 * shape.y), 0.0, 1.0);
		vec3 fill = color.rgb * (0.94 + 0.12 * t);
		float lit = clamp(1.0 - abs(local.y + shape.y - 1.5), 0.0, 1.0) * step(abs(local.x), shape.x - shape.z);
		fill = mix(fill, second.rgb, edge * second.a);
		fill = fill + vec3(0.06) * lit;
		float a = color.a * inside;
		result = vec4(fill * a, a);
	} else if (kind < 2.5) {
		// A line: full within its half width (shape.y less a pixel), fading over the last pixel.
		float cover = clamp(shape.y - abs(local.y), 0.0, 1.0);
		float a = color.a * cover;
		result = vec4(color.rgb * a, a);
	} else if (kind < 3.5) {
		// A vertical gradient from the colour at the top to the second at the bottom.
		float t = clamp(local.y / (2.0 * shape.y) + 0.5, 0.0, 1.0);
		vec4 straight = mix(color, second, t);
		result = vec4(straight.rgb * straight.a, straight.a);
	} else if (kind < 4.5) {
		// The area under a graph: the colour at the line, fading to the second colour at the bottom.
		float t = clamp(local.y / (2.0 * shape.y) + 0.5, 0.0, 1.0);
		vec4 straight = mix(color, second, t);
		result = vec4(straight.rgb * straight.a, straight.a);
	} else {
		// A disc of radius shape.x with a soft rim.
		float d = length(local) - shape.x;
		float a = color.a * clamp(0.5 - d, 0.0, 1.0);
		result = vec4(color.rgb * a, a);
	}
}
