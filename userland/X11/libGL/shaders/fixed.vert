#version 450
// libGL's fixed-function vertex stage (WS069 p005): the transforms, per-vertex lighting (up to 8
// lights, an infinite viewer) and the colour material of OpenGL 1.x.  FLAT makes the colour flat.
// The uniforms are one block at set 0 binding 0 (libGLESv2's convention for SPIR-V shaders).
#ifdef FLAT
#define SHADE flat
#else
#define SHADE
#endif

layout(location = 0) in vec4 a_position;
layout(location = 1) in vec4 a_color;
layout(location = 2) in vec3 a_normal;
layout(location = 3) in vec4 a_texcoord;
layout(location = 0) SHADE out vec4 v_color;
layout(location = 1) out vec4 v_texcoord;

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

void main()
{
	vec4 eye = u_modelview * a_position;
	vec4 ambient = u_material_ambient;
	vec4 diffuse = u_material_diffuse;
	vec3 normal = mat3(u_normal_matrix) * a_normal;
	vec4 color;
	int light;

	// The colour material: the vertex colour stands for the ambient and diffuse materials (1), or one of them.
	if (u_color_material == 1 || u_color_material == 2)
		diffuse = a_color;
	if (u_color_material == 1 || u_color_material == 3)
		ambient = a_color;

	// Without lighting the colour is the vertex's.
	color = a_color;
	if (u_lighting != 0) {
		if (u_normalize != 0)
			normal = normalize(normal);
		color = u_material_emission + u_scene_ambient * ambient;
		for (light = 0; light < 8; light++) {
			vec3 direction;
			float attenuation = 1.0;
			float lambert;

			if ((u_lights & (1 << light)) == 0)
				continue;

			// A directional light (w 0), or a positional one with its attenuation.
			if (u_light_position[light].w == 0.0) {
				direction = normalize(u_light_position[light].xyz);
			} else {
				vec3 toward = u_light_position[light].xyz / u_light_position[light].w - eye.xyz / eye.w;
				float distance = length(toward);

				direction = toward / max(distance, 1e-6);
				attenuation = 1.0 / max(u_light_attenuation[light].x + u_light_attenuation[light].y * distance +
							u_light_attenuation[light].z * distance * distance, 1e-6);
			}

			// Ambient, diffuse, and specular with an infinite viewer.
			lambert = max(dot(normal, direction), 0.0);
			color += attenuation * (u_light_ambient[light] * ambient + lambert * u_light_diffuse[light] * diffuse);
			if (lambert > 0.0) {
				vec3 half_vector = normalize(direction + vec3(0.0, 0.0, 1.0));

				color += attenuation * pow(max(dot(normal, half_vector), 0.0), u_shininess) *
					 u_light_specular[light] * u_material_specular;
			}
		}
		color.a = diffuse.a;
	}

	v_color = clamp(color, 0.0, 1.0);
	v_texcoord = u_texture_matrix * a_texcoord;
	gl_PointSize = u_point_size;
	gl_Position = u_mvp * a_position;
}
