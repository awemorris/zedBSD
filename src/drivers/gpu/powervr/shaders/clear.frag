// The clear draw's fragment shader: each of the first OUTPUTS colour
// attachments takes its clear value from the push constants.
// Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
#version 450

layout(push_constant) uniform Clear {
	vec4 color0;
	vec4 color1;
	vec4 color2;
	vec4 color3;
	float depth;
} clear;

#if OUTPUTS > 0
layout(location = 0) out vec4 output0;
#endif
#if OUTPUTS > 1
layout(location = 1) out vec4 output1;
#endif
#if OUTPUTS > 2
layout(location = 2) out vec4 output2;
#endif
#if OUTPUTS > 3
layout(location = 3) out vec4 output3;
#endif

void main()
{
#if OUTPUTS > 0
	output0 = clear.color0;
#endif
#if OUTPUTS > 1
	output1 = clear.color1;
#endif
#if OUTPUTS > 2
	output2 = clear.color2;
#endif
#if OUTPUTS > 3
	output3 = clear.color3;
#endif
}
