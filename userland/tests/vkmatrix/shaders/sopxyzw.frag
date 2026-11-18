#version 450
// vkmatrix sop probe (q1548): four float channels worked out from the varying, blended: more registers than one group's upper sources.
layout(location = 0) in vec2 place;
layout(location = 0) out vec4 colour;
void main()
{
	colour = vec4(place.x, place.y, place.x * place.y, 1.0 - place.x * 0.5);
}
