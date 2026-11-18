#version 450
// Twelve uniform blocks: eleven small ones the push data holds and a 64 KiB one read from memory, at constant
// offsets and, in its tail, at one of sixteen chosen per pixel.
layout(set = 0, binding = 0) uniform B0 { uvec4 v; } b0;
layout(set = 0, binding = 1) uniform B1 { uvec4 v; } b1;
layout(set = 0, binding = 2) uniform B2 { uvec4 v; } b2;
layout(set = 0, binding = 3) uniform B3 { uvec4 v; } b3;
layout(set = 0, binding = 4) uniform B4 { uvec4 v; } b4;
layout(set = 0, binding = 5) uniform B5 { uvec4 v; } b5;
layout(set = 0, binding = 6) uniform B6 { uvec4 v; } b6;
layout(set = 0, binding = 7) uniform B7 { uvec4 v; } b7;
layout(set = 0, binding = 8) uniform B8 { uvec4 v; } b8;
layout(set = 0, binding = 9) uniform B9 { uvec4 v; } b9;
layout(set = 0, binding = 10) uniform B10 { uvec4 v; } b10;
layout(set = 0, binding = 11) uniform Big { uvec4 v[4080]; uvec4 tail[16]; } big;
layout(location = 0) out uint color;
void main() {
	uvec2 p = uvec2(gl_FragCoord.xy);
	uint s = b0.v.x + b1.v.y + b2.v.z + b3.v.w + b4.v.x + b5.v.y + b6.v.z + b7.v.w + b8.v.x + b9.v.y + b10.v.z;
	uint t = big.v[0].x ^ big.v[1234].y ^ big.v[4079].w ^ big.tail[p.x % 16u].z;
	color = (p.x + 64u * p.y) ^ s ^ t;
}
