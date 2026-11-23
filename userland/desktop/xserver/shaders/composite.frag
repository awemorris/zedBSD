// zedBSD
// Copyright (C) 2026 Awe Morris
//
// SPDX-License-Identifier: Zlib

// xserver's drawing layer: a composite of RENDER at a pixel of the target
// (an image of depth 24, its alpha 1).  The source is read, scaled by the
// mask, and joined with the target's pixel (read from a copy of the part
// drawn, taken before) by a Porter-Duff operator, a channel at a time in
// 8 bits with each product rounded as pixman does, so that the pixels are
// those render-composite.c makes in memory.
//
// A picture is none (mode 0, the mask only: all of the source), one colour
// (mode 1), pixels in the buffer (mode 2: a8r8g8b8, a rectangle of the
// picture's box, rows of its width), or the image on the GPU the sampler
// reads (mode 3, the source only).  A place outside the picture's box
// comes inside by its repeat (0 none, 1 normal, 2 pad, 3 reflect), or is
// clear.
//
// Procedure referenced: pixman 0.44.0 pixman/pixman-combine32.c (the
// unified combiners combine_*_u and the component-alpha ones
// combine_*_ca), as render-composite.c follows it.
#version 450

layout(set = 0, binding = 0) uniform sampler2D source_image;

layout(std430, set = 0, binding = 1) readonly buffer Pixels {
	uint pixels[];
} buffer_pixels;

layout(set = 0, binding = 2) uniform sampler2D target_copy;

layout(push_constant) uniform Composite {
	ivec2 target_size;
	ivec2 target_origin;
	ivec2 source_offset;
	ivec2 source_size;
	ivec2 source_base;
	ivec2 source_extent;
	ivec2 mask_offset;
	ivec2 mask_size;
	ivec2 mask_base;
	ivec2 mask_extent;
	int op;
	int component;
	int source_mode;
	int source_repeat;
	int mask_mode;
	int mask_repeat;
	uint source_color;
	uint mask_color;
	uint source_start;
	uint mask_start;
} composite;

layout(location = 0) out vec4 color;

// A value modulo a size (the size positive), never negative.
int wrap(int value, int size)
{
	if (value >= 0)
		return value % size;
	return size - 1 - ((-value - 1) % size);
}

// Brings a place along one side inside a picture of a size by a repeat;
// false when it stays outside (no repeat) or the picture is empty.
bool inside(int value, int size, int repeat, out int place)
{
	int period;

	place = 0;
	if (size <= 0)
		return false;
	if (repeat == 1) {
		place = wrap(value, size);
	} else if (repeat == 2) {
		place = clamp(value, 0, size - 1);
	} else if (repeat == 3) {
		period = size * 2;
		place = wrap(value, period);
		if (place >= size)
			place = period - 1 - place;
	} else {
		if (value < 0 || value >= size)
			return false;
		place = value;
	}
	return true;
}

// An a8r8g8b8 pixel's channels (red, green, blue, alpha).
uvec4 channels(uint pixel)
{
	return uvec4((pixel >> 16) & 255u, (pixel >> 8) & 255u, pixel & 255u, pixel >> 24);
}

// A texel of an image of B8G8R8A8_UNORM as 8-bit channels (red, green, blue, alpha).
uvec4 texel(sampler2D image, ivec2 place)
{
	return uvec4(round(texelFetch(image, place, 0) * 255.0));
}

// A picture's pixel at a place of its own, from the buffer (clear outside).
uvec4 from_buffer(ivec2 place, ivec2 size, int repeat, ivec2 base, ivec2 extent, uint start)
{
	ivec2 at;
	ivec2 local;
	bool seen;

	seen = inside(place.x, size.x, repeat, at.x);
	if (seen)
		seen = inside(place.y, size.y, repeat, at.y);
	if (!seen)
		return uvec4(0u);
	local = at - base;
	if (local.x < 0 || local.y < 0 || local.x >= extent.x || local.y >= extent.y)
		return uvec4(0u);
	return channels(buffer_pixels.pixels[start + uint(local.y) * uint(extent.x) + uint(local.x)]);
}

// The source's pixel at a place of its own.
uvec4 source_pixel(ivec2 place)
{
	ivec2 at;
	bool seen;

	if (composite.source_mode == 1)
		return channels(composite.source_color);
	if (composite.source_mode == 2)
		return from_buffer(place, composite.source_size, composite.source_repeat, composite.source_base, composite.source_extent, composite.source_start);
	seen = inside(place.x, composite.source_size.x, composite.source_repeat, at.x);
	if (seen)
		seen = inside(place.y, composite.source_size.y, composite.source_repeat, at.y);
	if (!seen)
		return uvec4(0u);
	return texel(source_image, composite.source_base + at);
}

// The mask's pixel at a place of its own (all set without a mask).
uvec4 mask_pixel(ivec2 place)
{
	if (composite.mask_mode == 0)
		return uvec4(255u);
	if (composite.mask_mode == 1)
		return channels(composite.mask_color);
	return from_buffer(place, composite.mask_size, composite.mask_repeat, composite.mask_base, composite.mask_extent, composite.mask_start);
}

// Two 8-bit values multiplied as fractions of 255, rounded (pixman's MUL_UN8).
uint multiply(uint a, uint b)
{
	uint product;

	product = a * b + 128u;
	return ((product >> 8) + product) >> 8;
}

// Two 8-bit values added, kept at most 255.
uint add(uint a, uint b)
{
	return min(a + b, 255u);
}

// x by a plus y by b (fractions of 255), each product rounded, the sum kept at most 255.
uint multiply_add(uint x, uint a, uint y, uint b)
{
	return add(multiply(x, a), multiply(y, b));
}

// Every channel of a pixel scaled by an 8-bit alpha.
uvec4 scaled(uvec4 pixel, uint alpha)
{
	return uvec4(multiply(pixel.r, alpha), multiply(pixel.g, alpha), multiply(pixel.b, alpha), multiply(pixel.a, alpha));
}

// One channel joined by a Porter-Duff operator: the source s and its alpha
// sa (for component alpha, the channel's own), the target d and its alpha
// da.  Saturate is Add here (its source is scaled before).
uint joined(int op, uint s, uint sa, uint d, uint da)
{
	if (op == 0)
		return 0u;
	if (op == 1)
		return s;
	if (op == 2)
		return d;
	if (op == 3)
		return add(s, multiply(d, 255u - sa));
	if (op == 4)
		return add(d, multiply(s, 255u - da));
	if (op == 5)
		return multiply(s, da);
	if (op == 6)
		return multiply(d, sa);
	if (op == 7)
		return multiply(s, 255u - da);
	if (op == 8)
		return multiply(d, 255u - sa);
	if (op == 9)
		return multiply_add(s, da, d, 255u - sa);
	if (op == 10)
		return multiply_add(s, 255u - da, d, sa);
	if (op == 11)
		return multiply_add(s, 255u - da, d, 255u - sa);
	return add(s, d);
}

void main()
{
	ivec2 place;
	uvec4 source;
	uvec4 mask;
	uvec4 target;
	uvec4 alphas;
	uvec4 result;
	uint factor;
	int op;

	// The source, the mask and the target at the pixel.
	place = ivec2(gl_FragCoord.xy);
	source = source_pixel(place + composite.source_offset);
	mask = mask_pixel(place + composite.mask_offset);
	target = texel(target_copy, place - composite.target_origin);
	op = composite.op;

	// The source by the mask: each channel by the mask's alpha, or (component alpha) by the mask's same channel.
	if (composite.component != 0) {
		alphas = uvec4(multiply(mask.r, source.a), multiply(mask.g, source.a), multiply(mask.b, source.a), multiply(mask.a, source.a));
		source = uvec4(multiply(source.r, mask.r), multiply(source.g, mask.g), multiply(source.b, mask.b), multiply(source.a, mask.a));
	} else {
		source = scaled(source, mask.a);
		alphas = uvec4(source.a);
	}

	// Saturate (not of component alpha): the source scaled down to fit the room the target's alpha leaves, then added.
	if (op == 13) {
		if (source.a > 255u - target.a) {
			factor = ((255u - target.a) * 255u + source.a / 2u) / source.a;
			source = scaled(source, factor);
			alphas = uvec4(source.a);
		}
		op = 12;
	}

	// Each colour channel joined (the alpha is the target's, 1).
	result.r = joined(op, source.r, alphas.r, target.r, target.a);
	result.g = joined(op, source.g, alphas.g, target.g, target.a);
	result.b = joined(op, source.b, alphas.b, target.b, target.a);
	result.a = 255u;
	color = vec4(result) / 255.0;
}
