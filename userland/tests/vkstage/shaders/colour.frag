#version 450
// vkstage draw and depth: the interpolated colour.
layout(location = 0) in vec4 shade;
layout(location = 0) out vec4 result;
void main() { result = shade; }
