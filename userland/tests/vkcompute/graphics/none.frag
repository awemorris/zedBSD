#version 450
// Black, for the checks that draw nothing into an 8-bit target (their vertex shaders' stores are what is checked).
layout(location = 0) out vec4 color;
void main() { color = vec4(0.0); }
