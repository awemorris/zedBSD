#version 450
// vkmatrix perf-rcp (q1539): 32 dependent reciprocals of a push constant, then a colour;
// against perf-mul4 it tells what a reciprocal costs beside a multiply.
layout(push_constant) uniform Push { float red; } push;
layout(location = 0) out vec4 colour;
void main()
{
	float x = push.red + 0.5;
	x = 1.0 / x;
	x = 1.0 / x;
	x = 1.0 / x;
	x = 1.0 / x;
	x = 1.0 / x;
	x = 1.0 / x;
	x = 1.0 / x;
	x = 1.0 / x;
	x = 1.0 / x;
	x = 1.0 / x;
	x = 1.0 / x;
	x = 1.0 / x;
	x = 1.0 / x;
	x = 1.0 / x;
	x = 1.0 / x;
	x = 1.0 / x;
	x = 1.0 / x;
	x = 1.0 / x;
	x = 1.0 / x;
	x = 1.0 / x;
	x = 1.0 / x;
	x = 1.0 / x;
	x = 1.0 / x;
	x = 1.0 / x;
	x = 1.0 / x;
	x = 1.0 / x;
	x = 1.0 / x;
	x = 1.0 / x;
	x = 1.0 / x;
	x = 1.0 / x;
	x = 1.0 / x;
	x = 1.0 / x;
	colour = vec4(x, 0.0, 0.0, 1.0);
}
