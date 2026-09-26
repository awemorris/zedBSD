// zedBSD zwl: one textured quad, placed by push constants.
// Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
#version 450

// rect is the quad's corners in normalized device coordinates (x0, y0, x1,
// y1); texture is the part of the image it shows (u0, v0, u1, v1).
layout(push_constant) uniform Quad {
	vec4 rect;
	vec4 texture;
} quad;

layout(location = 0) out vec2 texcoord;

void main()
{
	// Vertices 0..3 of a triangle strip: (0,0), (1,0), (0,1), (1,1).
	vec2 corner = vec2(float(gl_VertexIndex & 1), float(gl_VertexIndex >> 1));

	gl_Position = vec4(mix(quad.rect.xy, quad.rect.zw, corner), 0.0, 1.0);
	texcoord = mix(quad.texture.xy, quad.texture.zw, corner);
}
