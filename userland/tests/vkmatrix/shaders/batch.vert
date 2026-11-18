#version 450
// vkmatrix: Keiland's batch vertex (R32G32 position, two R32G32B32A32 and an R32 attribute, one binding).
layout(location = 0) in vec2 position;
layout(location = 1) in vec4 colour;
layout(location = 2) in vec4 extra;
layout(location = 3) in float alpha;
layout(location = 0) out vec4 mixed;
void main()
{
	gl_Position = vec4(position, 0.0, 1.0);
	mixed = vec4(colour.rgb * alpha, alpha) + extra;
}
