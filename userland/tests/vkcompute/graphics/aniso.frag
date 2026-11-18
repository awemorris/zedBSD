#version 450
// Texel (x % 4, y % 4) of the 4 x 4 image read at its centre through an anisotropic sampler, its bytes packed.
layout(set = 0, binding = 0) uniform sampler2D tex;
layout(location = 0) out uint color;
void main() {
	uvec2 p = uvec2(gl_FragCoord.xy) % 4u;
	uvec4 t = uvec4(textureLod(tex, (vec2(p) + 0.5) / 4.0, 0.0) * 255.0 + 0.5);
	color = t.x | (t.y << 8) | (t.z << 16) | (t.w << 24);
}
