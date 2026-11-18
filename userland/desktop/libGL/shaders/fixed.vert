#version 450
// libGL's fixed-function vertex stage (WS069 p005, reshaped for the i915 compiler in WS068 p006): the
// transforms, per-vertex lighting (LIGHTS of the 8 lights, the enabled ones packed first by libGL; an
// infinite viewer) and the colour material of OpenGL 1.x.
// FLAT makes the colour flat (the i915 compiler has no flat inputs; libGL draws glBegin/glEnd's flat
// primitives with this smooth program, each vertex given the provoking vertex's colour and normal).
// The uniforms are one block at set 0 binding 0 (libGLESv2's convention for SPIR-V shaders), of few
// members, the flags floats, and every array index constant.
#ifndef LIGHTS
#define LIGHTS 8
#endif

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

// u_material: scene ambient (light model ambient), ambient, diffuse, specular, emission.
// u_lights: 5 a light: position (eye space), ambient, diffuse, specular, attenuation (constant, linear,
// quadratic, enabled 0 or 1).  u_flags: lighting, colour for ambient, colour for diffuse, normalize.
// u_flags2: texturing, texture replaces, alpha test function (0 off, 1 .. 8), alpha reference.
// u_params: shininess, point size (not written: the i915 compiler stores only gl_Position; points take the
// device's size).
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

#define LIGHT(n) \
	{ \
		vec4 position = u_lights[n * 5]; \
		vec4 attenuation = u_lights[n * 5 + 4]; \
		vec3 toward = position.xyz - eye.xyz * position.w; \
		float distance = length(toward); \
		vec3 direction = toward / max(distance, 1e-6); \
		float fall = mix(1.0, 1.0 / max(attenuation.x + attenuation.y * distance + attenuation.z * distance * distance, 1e-6), position.w); \
		float lambert = max(dot(normal, direction), 0.0); \
		vec3 half_vector = normalize(direction + vec3(0.0, 0.0, 1.0)); \
		float shine = pow(max(dot(normal, half_vector), 1e-6), u_params.x) * step(1e-6, lambert); \
		lit += attenuation.w * fall * (u_lights[n * 5 + 1] * ambient + lambert * u_lights[n * 5 + 2] * diffuse + \
			shine * u_lights[n * 5 + 3] * u_material[3]); \
	}

void main()
{
	vec4 eye = u_modelview * a_position;
	vec4 ambient = mix(u_material[1], a_color, u_flags.y);
	vec4 diffuse = mix(u_material[2], a_color, u_flags.z);
	vec3 normal = mat3(u_normal_matrix) * a_normal;
	vec4 lit;

	// The normal made unit when asked, and the lights' sum over the emission and the scene's ambient.
	normal = mix(normal, normalize(normal), u_flags.w);
	lit = u_material[4] + u_material[0] * ambient;
	LIGHT(0)
#if LIGHTS > 1
	LIGHT(1)
#endif
#if LIGHTS > 2
	LIGHT(2)
	LIGHT(3)
#endif
#if LIGHTS > 4
	LIGHT(4)
	LIGHT(5)
	LIGHT(6)
	LIGHT(7)
#endif
	lit.a = diffuse.a;

	// Lit, or the vertex's colour without lighting.
	v_color = clamp(mix(a_color, lit, u_flags.x), 0.0, 1.0);
	v_texcoord = u_texture_matrix * a_texcoord;
	gl_Position = u_mvp * a_position;
}
