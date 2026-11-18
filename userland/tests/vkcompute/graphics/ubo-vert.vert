#version 450
// A vertex shader that reads a 4 KiB uniform block (read from memory) and a push constant.
layout(set = 0, binding = 0) uniform Big { uvec4 v[256]; } big;
layout(push_constant) uniform Push { uint k; } push;
layout(location = 0) in vec4 position;
layout(location = 0) flat out uint value;
void main() {
	gl_Position = position;
	value = big.v[3].x ^ big.v[128].y ^ big.v[255].z ^ push.k;
}
