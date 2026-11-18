#version 450
// vkmatrix: the 128-byte push constant's last vector, plus a channel of a middle one.
layout(push_constant) uniform Push { vec4 offset; vec4 middle[6]; vec4 colour; } push;
layout(location = 0) out vec4 colour;
void main()
{
	colour = push.colour + vec4(0.0, push.middle[3].y, 0.0, 0.0);
}
