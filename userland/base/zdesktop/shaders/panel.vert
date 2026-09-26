// zedBSD zdesktop: one quad of the glass look, placed by push constants.
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

// The quad's corner, from zdesktop's vertex buffer of two triangles.
layout(location = 0) in vec2 corner;

layout(location = 0) out vec2 texcoord;

// The fragment's place in output pixels (instead of gl_FragCoord, which
// i915's native compiler does not take).
layout(location = 1) out vec2 pixel;

void main()
{
	vec2 place = mix(panel.rect.xy, panel.rect.zw, corner);

	gl_Position = vec4(place, 0.0, 1.0);
	texcoord = mix(panel.texture.xy, panel.texture.zw, corner);
	pixel = (place * 0.5 + vec2(0.5)) * panel.screen.xy;
}
