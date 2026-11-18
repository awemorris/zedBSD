// Keiland compositor: the turn pass's quad over the whole output (WS217).
// Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
#version 450

// The texture coordinates are two rows of an affine map of the output's
// corner (0 to 1, y downward): texcoord = (dot(u.xy, corner) + u.z,
// dot(v.xy, corner) + v.z).  For a turned output they put the logical
// image's corners in another order (rotate.c, kwl_turn_coefficients).
layout(push_constant) uniform Turn {
	vec4 u;
	vec4 v;
} turn;

// The quad's corner, from the compositor's vertex buffer of two triangles
// (a vertex buffer rather than gl_VertexIndex, which i915's native compiler
// does not take).
layout(location = 0) in vec2 corner;

layout(location = 0) out vec2 texcoord;

void main()
{
	gl_Position = vec4(corner * 2.0 - 1.0, 0.0, 1.0);
	texcoord = vec2(dot(turn.u.xy, corner) + turn.u.z, dot(turn.v.xy, corner) + turn.v.z);
}
