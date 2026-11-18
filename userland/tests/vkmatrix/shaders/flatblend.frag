#version 450
// vkmatrix perf-flat-blend (q1547): perf-flat's red from a 4-byte push constant at alpha 0.5, premultiplied, for the OVER blend.
layout(push_constant) uniform Push { float red; } push;
layout(location = 0) out vec4 colour;
void main()
{
	colour = vec4(push.red * 0.5, 0.0, 0.0, 0.5);
}
