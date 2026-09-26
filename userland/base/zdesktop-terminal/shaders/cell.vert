// zedBSD zdesktop-terminal: places one corner of a character cell.
// Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
#version 450

// The window's size in pixels (x, y); z and w are unused.
layout(push_constant) uniform Frame {
	vec4 size;
} frame;

// The corner's pixel position (xy) and its place in the glyph atlas (zw),
// then the cell's foreground and background colours.  Every value comes
// from the vertex buffer (no gl_VertexIndex, which i915's native compiler
// does not take).
layout(location = 0) in vec4 corner;
layout(location = 1) in vec4 foreground_in;
layout(location = 2) in vec4 background_in;

layout(location = 0) out vec2 texcoord;
layout(location = 1) out vec4 foreground;
layout(location = 2) out vec4 background;

void main()
{
	gl_Position = vec4(corner.xy / frame.size.xy * 2.0 - 1.0, 0.0, 1.0);
	texcoord = corner.zw;
	foreground = foreground_in;
	background = background_in;
}
