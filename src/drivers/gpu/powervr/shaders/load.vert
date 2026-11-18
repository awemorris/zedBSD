// The load draw's vertex shader: a vertex of the two triangles over the
// framebuffer and its texture coordinate (R32G32 each, one binding).
// Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
#version 450
layout(location = 0) in vec2 position;
layout(location = 1) in vec2 coordinate;
layout(location = 0) out vec2 place;
void main()
{
	gl_Position = vec4(position, 0.0, 1.0);
	place = coordinate;
}
