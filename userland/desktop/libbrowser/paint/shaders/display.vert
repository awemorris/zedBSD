// zedBSD browser: places one display item's quad (paint/vulkan.c).
// Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
#version 450

// The target's size in pixels (x, y); z and w are unused.
layout(push_constant) uniform Frame {
	vec4 size;
} frame;

// The corner of the unit square (xy; zw unused), from the vertex buffer (no
// gl_VertexIndex, which i915's native compiler does not take).  Each item has
// six vertices of its own, with the same item in each: no instance-rate input,
// which the Raspberry Pi's V3D does not take.
layout(location = 0) in vec4 corner;

// The item: its exact rectangle in pixels (left, top,
// right, bottom), its straight color, the atlas place of a glyph or an image
// (u, v in texels) with its kind (z: 0 rectangle, 1 glyph, 2 image, 3 sideways glyph),
// and for an image where it starts (x, y, the unclipped rectangle's top left)
// with its texels to a pixel (z, w), and its size in texels (x, y).
layout(location = 1) in vec4 rect;
layout(location = 2) in vec4 color;
layout(location = 3) in vec4 atlas;
layout(location = 4) in vec4 source;
layout(location = 5) in vec4 texels;

// The position in pixels (what gl_FragCoord is at the fragment shader, which the
// V3D's compiler does not take there): it interpolates linearly in the target.
layout(location = 5) noperspective out vec2 pixel_position;

layout(location = 0) flat out vec4 item_rect;
layout(location = 1) flat out vec4 item_color;
layout(location = 2) flat out vec4 item_atlas;
layout(location = 3) flat out vec4 item_source;
layout(location = 4) flat out vec4 item_texels;

void main()
{
	// The quad covers every pixel the rectangle touches, so that a partly
	// covered edge pixel is drawn and the fragment shader weighs it.
	vec2 low = floor(rect.xy);
	vec2 high = ceil(rect.zw);
	vec2 position = mix(low, high, corner.xy);

	gl_Position = vec4(position / frame.size.xy * 2.0 - 1.0, 0.0, 1.0);
	pixel_position = position;
	item_rect = rect;
	item_color = color;
	item_atlas = atlas;
	item_source = source;
	item_texels = texels;
}
