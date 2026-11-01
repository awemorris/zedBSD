#version 450
// nv12-board: the luma plane (R8) and the chroma plane (R8G8) at the coordinate, as red and green-blue.
layout(set = 0, binding = 0) uniform sampler2D luma;
layout(set = 0, binding = 1) uniform sampler2D chroma;
layout(location = 0) in vec2 place;
layout(location = 0) out vec4 result;
void main() { result = vec4(texture(luma, place).r, texture(chroma, place).rg, 1.0); }
