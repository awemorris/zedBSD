#version 450
// The vertex shader's value, mixed with the pixel's place.
layout(location = 0) flat in uint value;
layout(location = 0) out uint color;
void main() {
	uvec2 p = uvec2(gl_FragCoord.xy);
	color = value ^ (p.x + 64u * p.y);
}
