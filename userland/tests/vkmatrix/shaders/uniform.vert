#version 450
// vkmatrix uniform-ramp and perf-uniform: a quad and its place across the target, 0 at the left edge and 1 at the right.
layout(location = 0) in vec2 position;
layout(location = 0) out vec2 place;
void main()
{
	gl_Position = vec4(position, 0.0, 1.0);
	place = position * 0.5 + 0.5;
}
