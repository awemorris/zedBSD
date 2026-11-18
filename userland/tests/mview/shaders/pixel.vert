/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Transforms one model vertex for per-pixel lighting (--shading=pixel).
 *
 * The model, view and projection matrices and the normal matrix come from
 * the scene's uniform buffer.  The vertex passes its view-space position and
 * normal on; the fragment stage lights every pixel.  Each instance is one
 * copy of the model (--copies), shifted in view space by the zw of its
 * per-instance offset.
 */
#version 450

layout(location = 0) in vec3 position;
layout(location = 1) in vec3 normal;
layout(location = 2) in vec2 texture_position;
layout(location = 3) in vec4 copy_offset;

layout(location = 0) out vec2 texture_coordinate;
layout(location = 1) out vec3 view_position;
layout(location = 2) out vec3 view_normal;

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

void main()
{
    vec4 eye = scene.view * (scene.model * vec4(position, 1.0)) + vec4(copy_offset.zw, 0.0, 0.0);

    view_position = eye.xyz;
    view_normal = (scene.normal_matrix * vec4(normal, 0.0)).xyz;
    texture_coordinate = texture_position;
    gl_Position = scene.projection * eye;
}
