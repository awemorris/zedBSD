// Keiland compositor: glyphs of the glass look drawn together (ws270-p007), each vertex carrying its glyph's corner.
// Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
#version 450

// The glass look's push constants (panel.vert); none is read here, as the
// glyphs' places, atlas cells and colours come in their vertices.
layout(push_constant) uniform Panel {
	vec4 rect;
	vec4 texture;
	vec4 box;
	vec4 color;
	vec4 shape;
	vec4 screen;
} panel;

// The vertex's place in normalized device coordinates, its place in the
// atlas, and its glyph's colour (straight, its alpha with the opacity).
layout(location = 0) in vec2 place;
layout(location = 1) in vec2 cell;
layout(location = 2) in vec4 color;

// The same for every vertex of a glyph, so interpolated they stay its own.
layout(location = 0) out vec2 texcoord;
layout(location = 1) out vec4 glyph_color;

void main()
{
	gl_Position = vec4(place, 0.0, 1.0);
	texcoord = cell;
	glyph_color = color;
}
