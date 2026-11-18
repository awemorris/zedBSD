#version 450
// The vertex position as given: a triangle that covers the 64 x 64 target.
layout(location = 0) in vec4 position;
void main() { gl_Position = position; }
