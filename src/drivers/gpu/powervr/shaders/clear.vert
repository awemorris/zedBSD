// The clear draw's vertex shader: a vertex of the two triangles over the
// framebuffer at the clear's depth (the push constants' word 16).
// Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
#version 450

layout(push_constant) uniform Clear {
	vec4 color0;
	vec4 color1;
	vec4 color2;
	vec4 color3;
	float depth;
} clear;

layout(location = 0) in vec2 position;

void main()
{
	gl_Position = vec4(position, clear.depth, 1.0);
}
