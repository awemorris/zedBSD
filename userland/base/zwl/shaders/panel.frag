// zedBSD zwl: the shapes of the glass look, in premultiplied alpha.
// Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
#version 450

// box: the rounded rectangle in output pixels (x, y, width, height).
// color: a straight color and its opacity.
// shape: corner radius, mode, softness or thickness, opaque image (1).
// screen: output width and height, edge highlight, the image's opacity.
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
layout(location = 0) out vec4 result;

const int MODE_GLASS = 0;
const int MODE_SHADOW = 1;
const int MODE_IMAGE = 2;
const int MODE_SOLID = 3;
const int MODE_RING = 4;
const int MODE_TEXT = 5;

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
	int mode = int(panel.shape.y + 0.5);
	vec2 point = gl_FragCoord.xy;
	float radius = panel.shape.x;
	float distance = rounded(point, panel.box, radius);
	float cover = clamp(0.5 - distance, 0.0, 1.0);

	// Frosted glass: the blurred wallpaper under the panel, whitened, with a bright edge.
	if (mode == MODE_GLASS) {
		vec3 under = texture(image, point / panel.screen.xy).rgb;
		vec3 glass = mix(under, panel.color.rgb, panel.color.a);
		float depth = clamp((point.y - panel.box.y) / max(panel.box.w, 1.0), 0.0, 1.0);
		float edge = clamp(1.0 - abs(distance + 1.0), 0.0, 1.0);

		glass += vec3(0.05) * (1.0 - depth);
		glass = mix(glass, vec3(1.0), edge * panel.screen.z);
		result = vec4(glass * cover, cover);
		return;
	}

	// A soft shadow that fades over the softness outside the box.
	if (mode == MODE_SHADOW) {
		float softness = max(panel.shape.z, 1.0);
		float alpha = panel.color.a * (1.0 - smoothstep(-softness * 0.5, softness, distance));

		result = vec4(panel.color.rgb * alpha, alpha);
		return;
	}

	// A window's image with rounded corners (an opaque image has no alpha of its own).
	if (mode == MODE_IMAGE) {
		vec4 texel = texture(image, texcoord);

		if (panel.shape.w > 0.5)
			texel.a = 1.0;
		result = texel * cover * panel.screen.w;
		return;
	}

	// An outline of the given thickness.
	if (mode == MODE_RING)
		cover = cover - clamp(0.5 - (distance + panel.shape.z), 0.0, 1.0);

	// Text: the glyph's coverage in the atlas.
	if (mode == MODE_TEXT)
		cover = texture(image, texcoord).a;

	// A solid color.
	result = vec4(panel.color.rgb, 1.0) * panel.color.a * cover;
}
