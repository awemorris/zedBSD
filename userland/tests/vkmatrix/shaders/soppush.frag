#version 450
// vkmatrix sop probe (q1548): the colour a 16-byte push constant holds, blended.
layout(push_constant) uniform Push { vec4 colour; } push;
layout(location = 0) out vec4 colour;
void main()
{
	colour = push.colour;
}
