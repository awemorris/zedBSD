#version 450
// vkmatrix: red from a 4-byte push constant.
layout(push_constant) uniform Push { float red; } push;
layout(location = 0) out vec4 colour;
void main()
{
	colour = vec4(push.red, 0.0, 0.0, 1.0);
}
