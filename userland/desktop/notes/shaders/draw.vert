// zedBSD Notes: places one corner of a rectangle, a stroke's triangle or the toolbar.
// Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
#version 450

// The window's size in pixels (x, y); z and w are unused.
layout(push_constant) uniform Frame {
	vec4 size;
} frame;

// The corner's pixel position (xy) and its extra pair (zw: a fringe's
// distance from the edge in x, or the toolbar picture's place), then its
// colour.  Every value comes from the vertex buffer (no gl_VertexIndex,
// which i915's native compiler does not take).
layout(location = 0) in vec4 corner;
layout(location = 1) in vec4 color_in;

layout(location = 0) out vec2 extra;
layout(location = 1) out vec4 color;

void main()
{
	gl_Position = vec4(corner.xy / frame.size.xy * 2.0 - 1.0, 0.0, 1.0);
	extra = corner.zw;
	color = color_in;
}
