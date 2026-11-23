// zedBSD
// Copyright (C) 2026 Awe Morris
//
// SPDX-License-Identifier: Zlib

// xserver's drawing layer: the source a fill or a copy paints a pixel with.
// The pipeline joins it with the target's pixel by the graphics context's
// function (a logic operation) and writes only the planes it may.
//
// mode 0: the foreground.
// mode 1: the tile's pixel (the tile repeated from its origin).
// mode 2: the foreground where the stipple is set, nothing where it is not.
// mode 3: the foreground where the stipple is set, the background where not.
// mode 4: the source image's pixel at the fragment less the origin.
//
// The caller gives a pattern's origin as its place less one pattern's size,
// so the pattern's coordinate is never negative and % is defined.
#version 450

layout(set = 0, binding = 0) uniform sampler2D pattern;

layout(push_constant) uniform Paint {
	ivec2 target_size;
	ivec2 origin;
	ivec2 pattern_size;
	int mode;
	uint foreground;
	uint background;
} paint;

layout(location = 0) out vec4 color;

// An X pixel (0x00RRGGBB) as the colour the image's format stores, opaque.
vec4 unpack(uint pixel)
{
	vec4 channels;

	channels.r = float((pixel >> 16) & 255u);
	channels.g = float((pixel >> 8) & 255u);
	channels.b = float(pixel & 255u);
	channels.a = 255.0;
	return channels / 255.0;
}

// The X pixel the pattern holds at a texel.
uint texel(ivec2 place)
{
	uvec4 channels;

	channels = uvec4(round(texelFetch(pattern, place, 0) * 255.0));
	return (channels.r << 16) | (channels.g << 8) | channels.b;
}

void main()
{
	ivec2 place;
	uint source;

	// The source pixel, by the mode.
	place = ivec2(gl_FragCoord.xy) - paint.origin;
	if (paint.mode == 0) {
		source = paint.foreground;
	} else if (paint.mode == 4) {
		source = texel(place);
	} else if (paint.mode == 1) {
		source = texel(place % paint.pattern_size);
	} else {
		source = paint.foreground;
		if ((texel(place % paint.pattern_size) & 1u) == 0u) {
			if (paint.mode == 2)
				discard;
			source = paint.background;
		}
	}

	color = unpack(source);
}
