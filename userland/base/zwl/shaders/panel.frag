// zedBSD zwl: the shapes of the glass look, in premultiplied alpha.
// Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
#version 450

// box: the rounded rectangle in output pixels (x, y, width, height).
// color: a straight color and its opacity.
// shape: corner radius, mode, softness or thickness, opaque image (1).
// screen: output width and height, edge highlight, the opacity of the whole shape.
layout(push_constant) uniform Panel {
	vec4 rect;
	vec4 texture;
	vec4 box;
	vec4 color;
	vec4 shape;
	vec4 screen;
} panel;

layout(set = 0, binding = 0) uniform sampler2D image;

layout(location = 0) in vec2 texcoord;
layout(location = 1) in vec2 pixel;
layout(location = 0) out vec4 result;

// The modes are whole numbers in a float, compared in ranges (an integer
// chain of comparisons becomes an OpSwitch, which i915's native compiler
// does not take): 0 glass, 1 shadow, 2 image, 3 solid, 4 ring, 5 text.

// Signed distance from a pixel to the rounded rectangle (negative inside).
float rounded(vec2 point, vec4 box, float radius)
{
	vec2 half_size = box.zw * 0.5;
	vec2 centre = box.xy + half_size;
	vec2 q = abs(point - centre) - half_size + vec2(radius);

	return length(max(q, vec2(0.0))) + min(max(q.x, q.y), 0.0) - radius;
}

void main()
{
	float mode = panel.shape.y;
	vec2 point = pixel;
	float radius = panel.shape.x;
	float distance = rounded(point, panel.box, radius);
	float cover = clamp(0.5 - distance, 0.0, 1.0);
	vec4 colour;

	// One exit, with no early return (an early return becomes an OpSwitch).
	if (mode < 0.5) {
		// Frosted glass: the blurred wallpaper under the panel, whitened, with a bright edge.
		vec3 under = texture(image, point / panel.screen.xy).rgb;
		vec3 glass = mix(under, panel.color.rgb, panel.color.a);
		float depth = clamp((point.y - panel.box.y) / max(panel.box.w, 1.0), 0.0, 1.0);
		float edge = clamp(1.0 - abs(distance + 1.0), 0.0, 1.0);

		glass += vec3(0.05) * (1.0 - depth);
		glass = mix(glass, vec3(1.0), edge * panel.screen.z);
		colour = vec4(glass * cover, cover);
	} else if (mode < 1.5) {
		// A soft shadow that fades over the softness outside the box.
		float softness = max(panel.shape.z, 1.0);
		float alpha = panel.color.a * (1.0 - smoothstep(-softness * 0.5, softness, distance));

		colour = vec4(panel.color.rgb * alpha, alpha);
	} else if (mode < 2.5) {
		// A window's image with rounded corners (an opaque image has no alpha of its own).
		vec4 texel = texture(image, texcoord);

		colour = vec4(texel.rgb, max(texel.a, panel.shape.w)) * cover;
	} else if (mode < 3.5) {
		// A solid color.
		colour = vec4(panel.color.rgb, 1.0) * panel.color.a * cover;
	} else if (mode < 4.5) {
		// An outline of the given thickness.
		cover = cover - clamp(0.5 - (distance + panel.shape.z), 0.0, 1.0);
		colour = vec4(panel.color.rgb, 1.0) * panel.color.a * cover;
	} else {
		// Text: the glyph's coverage in the atlas.
		cover = texture(image, texcoord).a;
		colour = vec4(panel.color.rgb, 1.0) * panel.color.a * cover;
	}

	// The whole shape faded by its opacity.
	result = colour * panel.screen.w;
}
