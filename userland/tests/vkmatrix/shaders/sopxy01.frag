#version 450
// vkmatrix sop probe (q1548): two varyings and two constants as the colour, the floats of the sum-of-products output.
layout(location = 0) in vec2 place;
layout(location = 0) out vec4 colour;
void main()
{
	colour = vec4(place.x, place.y, 0.0, 1.0);
}
