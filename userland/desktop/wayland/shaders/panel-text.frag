// Keiland compositor: glyphs of the glass look drawn together (ws270-p007), in premultiplied alpha.
// Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
#version 450

layout(set = 0, binding = 0) uniform sampler2D image;

// The glyph's place in the atlas and its colour (panel-text.vert).
layout(location = 0) in vec2 texcoord;
layout(location = 1) in vec4 glyph_color;
layout(location = 0) out vec4 result;

void main()
{
	// Text as panel.frag's mode 5: the glyph's coverage in the atlas, in its colour (the opacity is in the colour's alpha).
	float cover = texture(image, texcoord).a;

	result = vec4(glyph_color.rgb, 1.0) * glyph_color.a * cover;
}
