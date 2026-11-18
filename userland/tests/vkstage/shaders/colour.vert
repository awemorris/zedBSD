#version 450
// vkstage draw and depth: the vertex's position (z is the depth) and its colour.
layout(location = 0) in vec3 position;
layout(location = 1) in vec4 colour;
layout(location = 0) out vec4 shade;
void main() { gl_Position = vec4(position, 1.0); shade = colour; }
