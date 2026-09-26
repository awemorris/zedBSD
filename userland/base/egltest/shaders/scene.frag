#version 450
// egltest's scene (ws068-p008): the colour, times the texture when u_textured is 1.
layout(location = 0) in vec4 v_color;
layout(location = 1) in vec2 v_uv;
layout(location = 0) out vec4 o_color;
layout(set = 0, binding = 0) uniform Uniforms {
	mat4 u_matrix;
	vec4 u_tint;
	float u_textured;
};
layout(set = 0, binding = 1) uniform sampler2D u_texture;

void main()
{
	vec4 texel = texture(u_texture, v_uv);
	o_color = mix(v_color, v_color * texel, u_textured);
}
