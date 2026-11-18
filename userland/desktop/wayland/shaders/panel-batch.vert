// Keiland compositor: solid quads of the glass look drawn together (ws211-p003), each vertex carrying its quad.
// Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
#version 450

// The glass look's push constants (panel.vert); only screen (the output's
// width and height) is read here, the rest are each quad's, in its vertices.
layout(push_constant) uniform Panel {
	vec4 rect;
	vec4 texture;
	vec4 box;
	vec4 color;
	vec4 shape;
	vec4 screen;
} panel;

// The vertex's place in normalized device coordinates, and its quad's
// rounded rectangle in output pixels, colour (premultiplied by its
// opacity) and corner radius.
layout(location = 0) in vec2 place;
layout(location = 1) in vec4 box;
layout(location = 2) in vec4 color;
layout(location = 3) in float radius;

// The fragment's place in output pixels (instead of gl_FragCoord, which
// i915's native compiler does not take), and the quad's own values.
layout(location = 0) out vec2 pixel;
layout(location = 1) flat out vec4 quad_box;
layout(location = 2) flat out vec4 quad_color;
layout(location = 3) flat out float quad_radius;

void main()
{
	gl_Position = vec4(place, 0.0, 1.0);
	pixel = (place * 0.5 + vec2(0.5)) * panel.screen.xy;
	quad_box = box;
	quad_color = color;
	quad_radius = radius;
}
