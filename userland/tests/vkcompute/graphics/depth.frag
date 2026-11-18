#version 450
// Colour 1 at the depth (x + 0.5) / 64 the shader computes.
layout(location = 0) out uint color;
void main() {
	color = 1u;
	gl_FragDepth = gl_FragCoord.x / 64.0;
}
