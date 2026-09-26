#version 450
// libGL's fixed-function fragment stage (WS069 p005, reshaped for the i915 compiler in WS068 p006): the
// colour, times the texture (GL_MODULATE) or the texture itself (GL_REPLACE), and the alpha test.  FLAT
// takes the colour flat.
#ifdef FLAT
#define SHADE flat
#else
#define SHADE
#endif

layout(location = 0) SHADE in vec4 v_color;
layout(location = 1) in vec4 v_texcoord;
layout(location = 0) out vec4 o_color;

layout(set = 0, binding = 0) uniform Fixed {
	mat4 u_mvp;
	mat4 u_modelview;
	mat4 u_normal_matrix;
	mat4 u_texture_matrix;
	vec4 u_material[5];
	vec4 u_lights[40];
	vec4 u_flags;
	vec4 u_flags2;
	vec4 u_params;
};
layout(set = 0, binding = 1) uniform sampler2D u_texture;

void main()
{
	vec4 texel = texture(u_texture, v_texcoord.xy / v_texcoord.w);
	vec4 color = mix(v_color, mix(v_color * texel, texel, u_flags2.y), u_flags2.x);
	float a = color.a;
	float ref = u_flags2.w;
	float f = u_flags2.z;
	bool fail;

	// The alpha test: 1 never, 2 less, 3 equal, 4 less or equal, 5 greater, 6 not equal, 7 greater or equal, 8 always; 0 off.
	fail = (f == 1.0) || (f == 2.0 && !(a < ref)) || (f == 3.0 && !(a == ref)) || (f == 4.0 && !(a <= ref)) ||
	       (f == 5.0 && !(a > ref)) || (f == 6.0 && !(a != ref)) || (f == 7.0 && !(a >= ref));
	if (fail)
		discard;
	o_color = color;
}
