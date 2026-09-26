// zedBSD zwl: one quad of the glass look, placed by push constants.
// Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
#version 450

// The quad in normalized device coordinates and the part of the image it
// shows; the rest of the constants belong to the fragment shader.
layout(push_constant) uniform Panel {
	vec4 rect;
	vec4 texture;
	vec4 box;
	vec4 color;
	vec4 shape;
	vec4 screen;
} panel;

layout(location = 0) out vec2 texcoord;

void main()
{
	// Vertices 0..3 of a triangle strip: (0,0), (1,0), (0,1), (1,1).
	vec2 corner = vec2(float(gl_VertexIndex & 1), float(gl_VertexIndex >> 1));

	gl_Position = vec4(mix(panel.rect.xy, panel.rect.zw, corner), 0.0, 1.0);
	texcoord = mix(panel.texture.xy, panel.texture.zw, corner);
}
