#version 450
// vkmatrix: the 32-byte push constant's colour (fragment stage).
layout(push_constant) uniform Push { vec4 offset; vec4 colour; } push;
layout(location = 0) out vec4 colour;
void main()
{
	colour = push.colour;
}
