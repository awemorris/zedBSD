#version 450
// The vertex position as given, and a point size of 2.0 for the geometry shader to read.
layout(location = 0) in vec4 position;
void main() {
	gl_Position = position;
	gl_PointSize = 2.0;
}
