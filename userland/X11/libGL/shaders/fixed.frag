#version 450
// libGL's fixed-function fragment stage (WS069 p005): the colour, times the texture (GL_MODULATE) or
// the texture itself (GL_REPLACE), and the alpha test.  FLAT takes the colour flat.
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
	vec4 u_scene_ambient;
	vec4 u_material_ambient;
	vec4 u_material_diffuse;
	vec4 u_material_specular;
	vec4 u_material_emission;
	vec4 u_light_position[8];
	vec4 u_light_ambient[8];
	vec4 u_light_diffuse[8];
	vec4 u_light_specular[8];
	vec4 u_light_attenuation[8];
	float u_shininess;
	float u_point_size;
	float u_alpha_ref;
	int u_lighting;
	int u_lights;
	int u_color_material;
	int u_normalize;
	int u_texturing;
	int u_alpha_func;
	int u_texture_replace;
};
layout(set = 0, binding = 1) uniform sampler2D u_texture;

void main()
{
	vec4 color = v_color;
	bool pass = true;

	// The texture, when GL_TEXTURE_2D is on.
	if (u_texturing != 0) {
		vec4 texel = texture(u_texture, v_texcoord.xy / v_texcoord.w);

		color = color * texel;
		if (u_texture_replace != 0)
			color = texel;
	}

	// The alpha test: GL_NEVER .. GL_ALWAYS as 0 .. 7 (0 when the test is off is taken as always).
	if (u_alpha_func == 1)
		pass = color.a < u_alpha_ref;
	else if (u_alpha_func == 2)
		pass = color.a == u_alpha_ref;
	else if (u_alpha_func == 3)
		pass = color.a <= u_alpha_ref;
	else if (u_alpha_func == 4)
		pass = color.a > u_alpha_ref;
	else if (u_alpha_func == 5)
		pass = color.a != u_alpha_ref;
	else if (u_alpha_func == 6)
		pass = color.a >= u_alpha_ref;
	else if (u_alpha_func == 8)
		pass = false;
	if (!pass)
		discard;
	o_color = color;
}
