// zedBSD
// Copyright (C) 2026 Awe Morris
//
// SPDX-License-Identifier: Zlib

// xserver's drawing layer: one box a drawing instance, two triangles from
// its corners on the target image (whose size is in the push constants).
#version 450

layout(location = 0) in ivec4 box;

layout(push_constant) uniform Paint {
	ivec2 target_size;
	ivec2 origin;
	ivec2 pattern_size;
	int mode;
	uint foreground;
	uint background;
} paint;

void main()
{
	int corner;
	int right;
	int below;
	vec2 point;

	// The six corners of the two triangles: (0,0) (1,0) (0,1), (0,1) (1,0) (1,1).
	corner = gl_VertexIndex;
	right = 0;
	if (corner == 1 || corner == 4 || corner == 5)
		right = 1;
	below = 0;
	if (corner == 2 || corner == 3 || corner == 5)
		below = 1;

	// The corner on the image, x0 or x1 and y0 or y1.
	point.x = float(box.x);
	if (right == 1)
		point.x = float(box.z);
	point.y = float(box.y);
	if (below == 1)
		point.y = float(box.w);

	// Pixels to normalized device coordinates (the first row at the top).
	gl_Position = vec4(point / vec2(paint.target_size) * 2.0 - 1.0, 0.0, 1.0);
}
