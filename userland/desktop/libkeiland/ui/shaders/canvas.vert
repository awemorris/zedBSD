// zedBSD Text Editor: places one corner of the quad that shows the canvas.
// Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
#version 450

// The window's size in pixels (x, y); z and w are unused.
layout(push_constant) uniform Frame {
	vec4 size;
} frame;

// The corner's pixel position (xy) and its place in the canvas (zw).  Every
// value comes from the vertex buffer (no gl_VertexIndex, which i915's
// native compiler does not take).
layout(location = 0) in vec4 corner;

layout(location = 0) out vec2 texcoord;

void main()
{
	gl_Position = vec4(corner.xy / frame.size.xy * 2.0 - 1.0, 0.0, 1.0);
	texcoord = corner.zw;
}
