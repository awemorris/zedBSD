#version 450
// vkmatrix sop probe (q1548): two channels of the sample and two constants.
layout(set = 0, binding = 0) uniform sampler2D picture;
layout(location = 0) in vec2 place;
layout(location = 0) out vec4 colour;
void main()
{
	vec4 t = texture(picture, place);
	colour = vec4(t.r, 0.5, t.b, 0.25);
}
