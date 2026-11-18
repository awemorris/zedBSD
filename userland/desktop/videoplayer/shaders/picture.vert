// zedBSD Video Player: places one corner of a quad -- the decoded picture, or the bar.
// Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
#version 450

// The window's size in pixels (x, y); the rest of the block is the fragment shader's.
layout(push_constant) uniform Picture {
	vec4 size;
	vec4 red;
	vec4 green;
	vec4 blue;
} picture;

// The corner's pixel position (xy) and its place in the textures (zw).  Every
// value comes from the vertex buffer (no gl_VertexIndex, which i915's
// native compiler does not take).
layout(location = 0) in vec4 corner;

layout(location = 0) out vec2 texcoord;

void main()
{
	gl_Position = vec4(corner.xy / picture.size.xy * 2.0 - 1.0, 0.0, 1.0);
	texcoord = corner.zw;
}
