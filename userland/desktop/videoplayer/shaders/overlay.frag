// zedBSD Video Player: the bar and the notices, drawn by the CPU into a
// texture when they change, over the picture with their premultiplied alpha.
// Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
#version 450

layout(set = 0, binding = 0) uniform sampler2D overlay;

layout(location = 0) in vec2 texcoord;
layout(location = 0) out vec4 color;

void main()
{
	color = texture(overlay, texcoord);
}
