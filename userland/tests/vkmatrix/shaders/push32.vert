#version 450
// vkmatrix: a quad moved by the 32-byte push constant's offset (vertex stage).
layout(push_constant) uniform Push { vec4 offset; vec4 colour; } push;
layout(location = 0) in vec2 position;
void main()
{
	gl_Position = vec4(position + push.offset.xy, 0.0, 1.0);
}
