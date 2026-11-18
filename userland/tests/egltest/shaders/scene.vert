#version 450
// egltest's scene (ws068-p008): a position, a colour and texture coordinates per vertex; the
// matrix places each shape, the tint scales its colour.  The uniforms other than samplers are one
// block at set 0 binding 0 (libGLESv2's convention for SPIR-V shaders).
layout(location = 0) in vec4 a_position;
layout(location = 1) in vec4 a_color;
layout(location = 2) in vec2 a_uv;
layout(location = 0) out vec4 v_color;
layout(location = 1) out vec2 v_uv;
layout(set = 0, binding = 0) uniform Uniforms {
	mat4 u_matrix;
	vec4 u_tint;
	float u_textured;
};

void main()
{
	v_color = a_color * u_tint;
	v_uv = a_uv;
	gl_Position = u_matrix * a_position;
}
