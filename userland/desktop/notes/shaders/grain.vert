// zedBSD Notes: places one corner of a pencil's shape and finds the grain
// under it.  The corner's pixel and colour are placed as draw.vert places
// them; the grain's place is the page point under the pixel in cells of
// the tile (the page's top left the tile's corner, y downwards).
// Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
#version 450

// The target's size in pixels (size.xy), and the page under the draw:
// its top left in pixels (grain.xy) and the tile's cells a pixel (grain.z).
layout(push_constant) uniform Frame {
	vec4 size;
	vec4 grain;
} frame;

// The corner's pixel position (xy), its fringe distance (z) and its
// opacity before the grain (w), then its colour.
layout(location = 0) in vec4 corner;
layout(location = 1) in vec4 color_in;

// The fringe distance and the opacity (xy), and the grain's place (zw).
layout(location = 0) out vec4 extra;
layout(location = 1) out vec4 color;

void main()
{
	gl_Position = vec4(corner.xy / frame.size.xy * 2.0 - 1.0, 0.0, 1.0);
	extra = vec4(corner.zw, (corner.xy - frame.grain.xy) * frame.grain.z);
	color = color_in;
}
