#version 450
// An array of three sampled images, each a view of the 4 x 4 image with its own swizzle: element x % 3 at texel (2, 1).
layout(set = 0, binding = 0) uniform sampler2D tex[3];
layout(location = 0) out uint color;
void main() {
	uint i = uint(gl_FragCoord.x) % 3u;
	vec4 t;
	if (i == 0u)
		t = textureLod(tex[0], vec2(0.625, 0.375), 0.0);
	else if (i == 1u)
		t = textureLod(tex[1], vec2(0.625, 0.375), 0.0);
	else
		t = textureLod(tex[2], vec2(0.625, 0.375), 0.0);
	uvec4 b = uvec4(t * 255.0 + 0.5);
	color = b.x | (b.y << 8) | (b.z << 16) | (b.w << 24);
}
