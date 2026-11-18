#version 450
// textureGather of channel (x / 3) % 4 at a texel corner of a 4 x 4 image, moved one texel right by a constant
// offset on every other band of three rows; the four texels' bytes packed into the colour.
layout(set = 0, binding = 0) uniform sampler2D tex;
layout(location = 0) out uint color;
void main() {
	uvec2 p = uvec2(gl_FragCoord.xy);
	vec2 uv = vec2(float(p.x % 3u + 1u), float(p.y % 3u + 1u)) / 4.0;
	uint channel = (p.x / 3u) % 4u;
	bool shifted = ((p.y / 3u) % 2u) == 1u;
	vec4 g;
	if (channel == 0u)
		g = shifted ? textureGatherOffset(tex, uv, ivec2(1, 0), 0) : textureGather(tex, uv, 0);
	else if (channel == 1u)
		g = shifted ? textureGatherOffset(tex, uv, ivec2(1, 0), 1) : textureGather(tex, uv, 1);
	else if (channel == 2u)
		g = shifted ? textureGatherOffset(tex, uv, ivec2(1, 0), 2) : textureGather(tex, uv, 2);
	else
		g = shifted ? textureGatherOffset(tex, uv, ivec2(1, 0), 3) : textureGather(tex, uv, 3);
	uvec4 b = uvec4(g * 255.0 + 0.5);
	color = b.x | (b.y << 8) | (b.z << 16) | (b.w << 24);
}
