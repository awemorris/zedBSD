// zedBSD Video Player: a decoded YCbCr 4:2:0 picture drawn in RGB (ws236-p011).
// Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
#version 450

// The picture's planes: its luma (R8) and its chroma at half the width and
// height (RG8: Cb in red, Cr in green), sampled with the same coordinates.
layout(set = 0, binding = 0) uniform sampler2D luma;
layout(set = 0, binding = 1) uniform sampler2D chroma;

// The colour conversion: red, green and blue are each dot((Y, Cb, Cr, 1),
// row), the samples in 0..1 as the planes give them.  The rows carry the
// matrix (BT.601, BT.709 or BT.2020) and the range's scale and offset, so
// one shader serves every picture.
layout(push_constant) uniform Picture {
	vec4 size;
	vec4 red;
	vec4 green;
	vec4 blue;
} picture;

layout(location = 0) in vec2 texcoord;
layout(location = 0) out vec4 color;

void main()
{
	vec4 sample_yuv = vec4(texture(luma, texcoord).r, texture(chroma, texcoord).rg, 1.0);

	color = vec4(clamp(dot(sample_yuv, picture.red), 0.0, 1.0),
		     clamp(dot(sample_yuv, picture.green), 0.0, 1.0),
		     clamp(dot(sample_yuv, picture.blue), 0.0, 1.0),
		     1.0);
}
