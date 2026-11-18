// zedBSD files: the canvas drawn by the CPU, texel for pixel, with its
// premultiplied alpha (a see-through swapchain shows the desktop where it is clear).
// Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
#version 450

layout(set = 0, binding = 0) uniform sampler2D canvas;

layout(location = 0) in vec2 texcoord;
layout(location = 0) out vec4 color;

void main()
{
	color = texture(canvas, texcoord);
}
