// The load draw's fragment shader: the combined image sampler of set 0,
// binding 0 sampled into the colour attachment at LOCATION, of floats
// (KIND 0), unsigned integers (KIND 1) or signed integers (KIND 2); with
// MULTISAMPLED 1, an image of several samples whose texel of the pixel and
// of the sample being shaded is fetched (the shader runs for each sample).
// Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
#version 450
#if MULTISAMPLED
#define SAMPLER sampler2DMS
#define USAMPLER usampler2DMS
#define ISAMPLER isampler2DMS
#else
#define SAMPLER sampler2D
#define USAMPLER usampler2D
#define ISAMPLER isampler2D
#endif
#if KIND == 1
layout(set = 0, binding = 0) uniform USAMPLER picture;
layout(location = LOCATION) out uvec4 colour;
#elif KIND == 2
layout(set = 0, binding = 0) uniform ISAMPLER picture;
layout(location = LOCATION) out ivec4 colour;
#else
layout(set = 0, binding = 0) uniform SAMPLER picture;
layout(location = LOCATION) out vec4 colour;
#endif
layout(location = 0) in vec2 place;
void main()
{
#if MULTISAMPLED
	colour = texelFetch(picture, ivec2(gl_FragCoord.xy), gl_SampleID);
#else
	colour = texture(picture, place);
#endif
}
