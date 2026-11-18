/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Lights a cut-out fragment per pixel (--shading=pixel): the lighting of
 * pixel.frag, but a fragment whose alpha is below one half is discarded and
 * the rest are written opaque.
 */
#version 450

layout(set = 0, binding = 0) uniform sampler2D surface_texture;

layout(set = 0, binding = 1) uniform Scene {
    mat4 model;
    mat4 view;
    mat4 projection;
    mat4 normal_matrix;
    vec4 ambient;
    vec4 light_position[3];
    vec4 light_color[3];
    vec4 light_factors[3];
} scene;

layout(location = 0) in vec2 texture_coordinate;
layout(location = 1) in vec3 view_position;
layout(location = 2) in vec3 view_normal;

layout(location = 0) out vec4 fragment_color;

layout(push_constant) uniform Material {
    layout(offset = 112) vec4 color;
} material;

void main()
{
    vec3 normal = normalize(view_normal);
    vec3 eye = normalize(-view_position);
    vec3 diffuse = scene.ambient.rgb;
    vec3 specular = vec3(0.0);
    vec4 base;
    int index;

    for (index = 0; index < 3; index++) {
        vec4 light = scene.light_position[index];
        vec4 factors = scene.light_factors[index];
        vec3 direction;
        vec3 offset;
        float distance;
        float attenuation;
        float lambert;
        float highlight;

        if (light.w == 0.0) {
            direction = normalize(light.xyz);
            attenuation = 1.0;
        } else {
            offset = light.xyz - view_position;
            distance = sqrt(dot(offset, offset));
            direction = offset / distance;
            attenuation = 1.0 / (factors.x + factors.y * distance + factors.z * distance * distance);
        }

        lambert = max(dot(normal, direction), 0.0);
        highlight = pow(max(dot(normal, normalize(direction + eye)), 0.0), factors.w);
        if (lambert <= 0.0) {
            highlight = 0.0;
        }

        diffuse += scene.light_color[index].rgb * (lambert * attenuation);
        specular += scene.light_color[index].rgb * (scene.light_color[index].a * highlight * attenuation);
    }

    base = texture(surface_texture, texture_coordinate) * material.color;
    if (base.a < 0.5) {
        discard;
    }

    fragment_color = vec4(base.rgb * diffuse + specular, 1.0);
}
