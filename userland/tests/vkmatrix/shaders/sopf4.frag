#version 450
// vkmatrix sop probe (q1548): float channels from the varying, four computed.
layout(location = 0) in vec2 place;
layout(location = 0) out vec4 colour;
void main()
{
	colour = vec4(place.x, place.y, place.x * place.y, place.x + place.y);
}
