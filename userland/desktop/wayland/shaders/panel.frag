// Keiland compositor: the shapes of the glass look, in premultiplied alpha.
// Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
#version 450

// box: the rounded rectangle in output pixels (x, y, width, height).
// color: a straight color and its opacity.
// shape: corner radius, mode, softness or thickness (flatness for glass), opaque image (1).
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
layout(location = 2) in vec2 screen_uv;
layout(location = 0) out vec4 result;

// The modes are whole numbers in a float, compared in ranges (an integer
// chain of comparisons becomes an OpSwitch, which i915's native compiler
// does not take): 0 glass, 1 shadow, 2 image, 3 solid, 4 ring, 5 text, 6 blur.
// Glass in the dark appearance is mode -1 (ws089-p017, glass.c); its own
// variant is KWL_PANEL_MODE 7 (ws211-p011), as light glass leaves out its
// darkening.

// The least brightness (Rec. 709 luma of the stored colour) of white glass
// (ws099-p005, the criterion C7): over a dark wallpaper the glass is
// whitened further, up to this, so that the dark text on it keeps its
// contrast; over a bright one it is left as it is.
const float GLASS_LEAST_LUMA = 0.85;

// The most brightness of dark glass (ws089-p017, the dark appearance): over a
// bright wallpaper the glass is darkened further, down to this, so that the
// light text on it keeps its contrast; over a dark one it is left as it is.
const float GLASS_MOST_LUMA = 0.15;

// Signed distance from a pixel to the rounded rectangle (negative inside).
float rounded(vec2 point, vec4 box, float radius)
{
	vec2 half_size = box.zw * 0.5;
	vec2 centre = box.xy + half_size;
	vec2 q = abs(point - centre) - half_size + vec2(radius);

	return length(max(q, vec2(0.0))) + min(max(q.x, q.y), 0.0) - radius;
}

// KWL_PANEL_INSIDE: the inside of a shape drawn by itself (glass.c, the
// glass, shadow, image and solid modes): its pixels lie far enough within the
// box that every pixel is covered wholly, none is on the glass's bright edge
// and a shadow there is at its full alpha (the distance at most -3 within
// the box less its corner radius, and at most half the softness below 0 for
// a shadow).  A distance far below any of them gives the same colour without
// working the distance out.

void main()
{
	float mode = panel.shape.y;
	vec2 point = pixel;
#if defined(KWL_PANEL_INSIDE)
	float distance = -1000000.0;
#else
	float radius = panel.shape.x;
	float distance = rounded(point, panel.box, radius);
#endif
	float cover = clamp(0.5 - distance, 0.0, 1.0);
	vec4 colour;

	// One exit, with no early return (an early return becomes an OpSwitch).
#if !defined(KWL_PANEL_MODE) || KWL_PANEL_MODE == 0 || KWL_PANEL_MODE == 7
#if !defined(KWL_PANEL_MODE)
	if (mode < 0.5) {
#endif
		// Frosted glass: the blurred wallpaper under the panel, whitened, with a bright edge.
		// The values that are the same for the whole shape come in texture
		// (glass.c, ws211-p011), as glass samples by position, not texcoord:
		// x whether the glass is white (its colour 0.9 or more in every
		// channel), y whether it is dark glass (mode -1), z the sheen from the
		// top (none on flat glass, shape.z 1), w one over the box's height.
#if defined(KWL_PANEL_MAP)
		// KWL_PANEL_MAP: the inside of flat glass whose colour was worked
		// out for every output pixel beforehand (glass-map.c), by this
		// shader over the same blurred wallpaper: the image is that colour,
		// read at the pixel itself; flat glass has no sheen to add.
		vec3 glass = texture(image, screen_uv).rgb;
#else
		vec3 under = texture(image, screen_uv).rgb;
		vec3 glass = mix(under, panel.color.rgb, panel.color.a);
		float luma = dot(glass, vec3(0.2126, 0.7152, 0.0722));
		float lift = panel.texture.x * clamp((GLASS_LEAST_LUMA - luma) / max(1.0 - luma, 0.001), 0.0, 1.0);
		glass = mix(glass, vec3(1.0), lift);
#if !defined(KWL_PANEL_MODE) || KWL_PANEL_MODE == 7
		// Only dark glass is darkened (mode 7 is the dark glass of mode 0).
		float shade = panel.texture.y * clamp((luma - GLASS_MOST_LUMA) / max(luma, 0.001), 0.0, 1.0);
		glass = mix(glass, vec3(0.0), shade);
#endif
		float depth = clamp((point.y - panel.box.y) * panel.texture.w, 0.0, 1.0);
#if !defined(KWL_PANEL_INSIDE)
		float edge = clamp(1.0 - abs(distance + 1.0), 0.0, 1.0);
#endif

		// A sheen from the top, except on flat glass (a window's panels).
		glass += vec3(panel.texture.z) * (1.0 - depth);
#endif
#if !defined(KWL_PANEL_INSIDE)
		// The bright edge, which an inside's pixels are off (the mix by 0 leaves the glass as it is).
		glass = mix(glass, vec3(1.0), edge * panel.screen.z);
#endif
		colour = vec4(glass * cover, cover);
#endif
#if !defined(KWL_PANEL_MODE) || KWL_PANEL_MODE == 1
#if !defined(KWL_PANEL_MODE)
	} else if (mode < 1.5) {
#endif
		// A soft shadow that fades over the softness outside the box.
		float softness = max(panel.shape.z, 1.0);
		float alpha = panel.color.a * (1.0 - smoothstep(-softness * 0.5, softness, distance));

		colour = vec4(panel.color.rgb * alpha, alpha);
#endif
#if !defined(KWL_PANEL_MODE) || KWL_PANEL_MODE == 2
#if !defined(KWL_PANEL_MODE)
	} else if (mode < 2.5) {
#endif
		// A window's image with rounded corners (an opaque image has no alpha of its own).
#if defined(KWL_PANEL_CROSSFADE)
		// KWL_PANEL_CROSSFADE: two pictures side by side in one image (the
		// left half and the right), the second faded in over the first by
		// color.a, in one draw instead of two (glass.c).
		vec4 texel = mix(texture(image, texcoord), texture(image, texcoord + vec2(0.5, 0.0)), panel.color.a);
#else
		vec4 texel = texture(image, texcoord);
#endif

		colour = vec4(texel.rgb, max(texel.a, panel.shape.w)) * cover;
#endif
#if !defined(KWL_PANEL_MODE) || KWL_PANEL_MODE == 3
#if !defined(KWL_PANEL_MODE)
	} else if (mode < 3.5) {
#endif
		// A solid color.
		colour = vec4(panel.color.rgb, 1.0) * panel.color.a * cover;
#endif
#if !defined(KWL_PANEL_MODE) || KWL_PANEL_MODE == 4
#if !defined(KWL_PANEL_MODE)
	} else if (mode < 4.5) {
#endif
		// An outline of the given thickness.
		cover = cover - clamp(0.5 - (distance + panel.shape.z), 0.0, 1.0);
		colour = vec4(panel.color.rgb, 1.0) * panel.color.a * cover;
#endif
#if !defined(KWL_PANEL_MODE) || KWL_PANEL_MODE == 5
#if !defined(KWL_PANEL_MODE)
	} else if (mode < 5.5) {
#endif
		// Text: the glyph's coverage in the atlas.
		cover = texture(image, texcoord).a;
		colour = vec4(panel.color.rgb, 1.0) * panel.color.a * cover;
#endif
#if !defined(KWL_PANEL_MODE) || KWL_PANEL_MODE == 6
#if !defined(KWL_PANEL_MODE)
	} else {
#endif
		// One pass of a Gaussian blur along a step (color.xy, a texel across or down): nine taps, read as five.
		vec2 step = panel.color.xy;

		colour = texture(image, texcoord) * 0.227027;
		colour += texture(image, texcoord + step * 1.384615) * 0.316216;
		colour += texture(image, texcoord - step * 1.384615) * 0.316216;
		colour += texture(image, texcoord + step * 3.230769) * 0.070270;
		colour += texture(image, texcoord - step * 3.230769) * 0.070270;
#endif
#if !defined(KWL_PANEL_MODE)
	}
#endif

	// The whole shape faded by its opacity.  KWL_PANEL_OPAQUE: the inside of
	// an opaque shape at full opacity (glass.c), whose alpha is 1 and whose
	// opacity is 1, so its colour is written as it is.
#if defined(KWL_PANEL_OPAQUE)
	result = vec4(colour.rgb, 1.0);
#else
	result = colour * panel.screen.w;
#endif
}
