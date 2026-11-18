#version 450
// vkmatrix: a quad's position as it is; the 4-byte push constant is the fragment's.
layout(location = 0) in vec2 position;
void main()
{
	gl_Position = vec4(position, 0.0, 1.0);
}
