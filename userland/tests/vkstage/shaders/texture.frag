#version 450
// vkstage sample: the texture at the coordinate.
layout(set = 0, binding = 0) uniform sampler2D picture;
layout(location = 0) in vec2 place;
layout(location = 0) out vec4 result;
void main() { result = texture(picture, place); }
