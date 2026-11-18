#version 450
// vkmatrix: the combined image sampler of set 0, binding 0.
layout(set = 0, binding = 0) uniform sampler2D picture;
layout(location = 0) in vec2 place;
layout(location = 0) out vec4 colour;
void main()
{
	colour = texture(picture, place);
}
