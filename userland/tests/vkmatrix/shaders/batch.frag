#version 450
// vkmatrix: the batch vertex's mixed colour.
layout(location = 0) in vec4 mixed;
layout(location = 0) out vec4 colour;
void main()
{
	colour = mixed;
}
