#version 450
// vkmatrix perf-immediate: an immediate colour, no push constants.
layout(location = 0) out vec4 colour;
void main()
{
	colour = vec4(1.0, 0.0, 0.0, 1.0);
}
