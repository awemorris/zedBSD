// zedBSD browser: colors one pixel of a display item (paint/vulkan.c).
//
// The coverage rules are the CPU reference renderer's (paint/software.c): a
// rectangle covers a pixel by the area of the pixel square it overlaps, and a
// glyph's coverage is its bitmap's texel at the pixel (the glyph's rectangle is
// on whole pixels).  An image covers a pixel as a rectangle does, with the
// color of its texel under the pixel's centre (the same steps as the CPU
// renderer's, in single precision).  The color goes out premultiplied (its
// alpha times the coverage, and the color times that), and the blend state
// (one, one minus source alpha) mixes it over the target.  The atlas is read
// through a nearest sampler at a texel's centre (no texelFetch, which the
// Raspberry Pi's V3D compiler does not take).
// Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
#version 450

// The atlas: each glyph's coverage in every channel, and the images' pixels.
layout(set = 0, binding = 0) uniform sampler2D glyphs;

// The atlas is this many texels on a side (PAINT_GPU_ATLAS_SIZE).
const float ATLAS = 2048.0;

layout(location = 5) noperspective in vec2 pixel_position;

layout(location = 0) flat in vec4 item_rect;
layout(location = 1) flat in vec4 item_color;
layout(location = 2) flat in vec4 item_atlas;
layout(location = 3) flat in vec4 item_source;
layout(location = 4) flat in vec4 item_texels;

layout(location = 0) out vec4 color;

// The texel at an index of the atlas.
vec4 fetch(vec2 index)
{
	return texture(glyphs, (index + 0.5) / ATLAS);
}

void main()
{
	vec2 pixel = floor(pixel_position);
	float coverage;

	if (item_atlas.z > 1.5 && item_atlas.z < 2.5) {
		// An image: the rectangle's overlap, and the texel under the pixel's centre.
		vec2 low = max(item_rect.xy, pixel);
		vec2 high = min(item_rect.zw, pixel + 1.0);
		vec2 overlap = clamp(high - low, 0.0, 1.0);
		vec2 place = floor((pixel + 0.5 - item_source.xy) * item_source.zw);
		place = clamp(place, vec2(0.0), item_texels.xy - 1.0);
		vec4 texel = fetch(floor(item_atlas.xy) + place);
		float alpha = texel.a * overlap.x * overlap.y;
		color = vec4(texel.rgb * alpha, alpha);
		return;
	}

	if (item_atlas.z < 0.5) {
		// A rectangle: the overlap of [pixel, pixel + 1) with it, on each axis.
		vec2 low = max(item_rect.xy, pixel);
		vec2 high = min(item_rect.zw, pixel + 1.0);
		vec2 overlap = clamp(high - low, 0.0, 1.0);
		coverage = overlap.x * overlap.y;
	} else if (item_atlas.z > 3.5) {
		// A counterclockwise sideways glyph: destination x runs through source rows, destination y through columns backwards.
		ivec2 offset = ivec2(pixel - item_source.xy);
		ivec2 texel = ivec2(item_atlas.xy) + ivec2(int(item_source.z) - 1 - offset.y, offset.x);
		coverage = fetch(vec2(texel)).a;
	} else if (item_atlas.z > 2.5) {
		// A clockwise sideways glyph: destination x runs through source rows backwards.
		ivec2 offset = ivec2(pixel - item_source.xy);
		ivec2 texel = ivec2(item_atlas.xy) + ivec2(offset.y, int(item_source.z) - 1 - offset.x);
		coverage = fetch(vec2(texel)).a;
	} else {
		// A glyph: its texel at this pixel.
		ivec2 texel = ivec2(item_atlas.xy + pixel - item_rect.xy);
		coverage = fetch(vec2(texel)).a;
	}

	float alpha = item_color.a * coverage;
	color = vec4(item_color.rgb * alpha, alpha);
}
