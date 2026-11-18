#version 450
// vkmatrix sop probe (q1548): the sample written as t * 0.5, one form of the pixel output for the driver's sum-of-products output.
layout(set = 0, binding = 0) uniform sampler2D picture;
layout(location = 0) in vec2 place;
layout(location = 0) out vec4 colour;
void main()
{
	vec4 t = texture(picture, place);
	colour = t * 0.5;
}
