// zedBSD zwl: samples a window's image.
// Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
#version 450

layout(set = 0, binding = 0) uniform sampler2D image;

layout(location = 0) in vec2 texcoord;
layout(location = 0) out vec4 color;

void main()
{
	color = texture(image, texcoord);
}
