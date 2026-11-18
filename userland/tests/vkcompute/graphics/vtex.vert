#version 450
// A vertex shader that samples texel (2, 1) of the 4 x 4 image and passes its bytes on.
layout(set = 0, binding = 0) uniform sampler2D tex;
layout(location = 0) in vec4 position;
layout(location = 0) flat out uint value;
void main() {
	gl_Position = position;
	uvec4 t = uvec4(textureLod(tex, vec2(0.625, 0.375), 0.0) * 255.0 + 0.5);
	value = t.x | (t.y << 8) | (t.z << 16) | (t.w << 24);
}
