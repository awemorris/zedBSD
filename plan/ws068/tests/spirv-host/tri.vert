#version 450
// egltest's vertex shader: a position and a colour per vertex, a matrix and an offset from the default uniform block.
layout(location = 0) in vec4 a_position;
layout(location = 1) in vec4 a_color;
layout(location = 2) in vec2 a_uv;
layout(location = 0) out vec4 v_color;
layout(location = 1) out vec2 v_uv;
layout(set = 0, binding = 0) uniform Uniforms {
	mat4 u_matrix;
	vec4 u_tint;
	float u_depth;
	vec2 u_offsets[3];
};
void main()
{
	v_color = a_color * u_tint;
	v_uv = a_uv;
	gl_Position = u_matrix * a_position + vec4(u_offsets[gl_VertexIndex % 3] * 0.0, u_depth, 0.0);
}
