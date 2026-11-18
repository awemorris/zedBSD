// zedBSD System Monitor: places one corner of a shape or a glyph (plan/ws134/design.md section 4.2).
// Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
#version 450

// The window's size in pixels (x, y); z is the time in seconds for the shapes that move; w is unused.
layout(push_constant) uniform Frame {
	vec4 size;
} frame;

// The corner's pixel position (xy) and its point in the shape (zw: from the shape's centre in pixels, or a glyph's
// place in the atlas), the shape (half width, half height, corner radius, kind), its colour and its second colour.
// Every value comes from the vertex buffer (no gl_VertexIndex, which i915's native compiler does not take).
layout(location = 0) in vec4 corner;
layout(location = 1) in vec4 shape_in;
layout(location = 2) in vec4 color_in;
layout(location = 3) in vec4 second_in;

layout(location = 0) out vec2 local;
layout(location = 1) out vec4 shape;
layout(location = 2) out vec4 color;
layout(location = 3) out vec4 second;

void main()
{
	gl_Position = vec4(corner.xy / frame.size.xy * 2.0 - 1.0, 0.0, 1.0);
	local = corner.zw;
	shape = shape_in;
	color = color_in;
	second = second_in;
}
