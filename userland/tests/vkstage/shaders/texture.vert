#version 450
// vkstage sample: the corner's position and texture coordinate.
layout(location = 0) in vec2 position;
layout(location = 1) in vec2 coordinate;
layout(location = 0) out vec2 place;
void main() { gl_Position = vec4(position, 0.0, 1.0); place = coordinate; }
