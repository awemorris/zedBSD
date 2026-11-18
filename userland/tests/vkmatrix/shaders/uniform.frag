#version 450
// vkmatrix uniform-ramp and perf-uniform: a ramp from the push constant's low colour to its high one across the quad; the span and its scale only the push constant decides (q1546).
layout(push_constant) uniform Push { vec4 low; vec4 high; } push;
layout(location = 0) in vec2 place;
layout(location = 0) out vec4 colour;
void main()
{
	vec4 span = (push.high - push.low) * (1.0 / max(push.high.w + push.low.w, 0.5));
	colour = push.low + span * place.x;
}
