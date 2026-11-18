/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Transforms one model vertex to clip space and lights it.
 *
 * The clip transform and the normal rotation arrive as columns so no matrix
 * type is needed.  Lambert lighting from a fixed view-space direction plus an
 * ambient term gives a per-vertex shade that the fragment stage multiplies in.
 * Each instance is one copy of the model (--copies): its place in the grid is
 * a shift of the clip position (xy of the per-instance offset; zw is the same
 * shift in view space, which the per-pixel shading uses).
 */
#version 450

layout(location = 0) in vec3 position;
layout(location = 1) in vec3 normal;
layout(location = 2) in vec2 texture_position;
layout(location = 3) in vec4 copy_offset;

layout(location = 0) out vec2 texture_coordinate;
layout(location = 1) out float shade;

layout(push_constant) uniform View {
    vec4 clip_x;
    vec4 clip_y;
    vec4 clip_z;
    vec4 clip_w;
    vec4 normal_x;
    vec4 normal_y;
    vec4 normal_z;
    vec4 color;
} view;

void main()
{
    vec3 turned = normalize(normal.x * view.normal_x.xyz +
                            normal.y * view.normal_y.xyz +
                            normal.z * view.normal_z.xyz);
    float lambert = max(dot(turned, vec3(0.267261, 0.534522, 0.801784)), 0.0);

    shade = 0.35 + 0.65 * lambert;
    texture_coordinate = texture_position;
    gl_Position = position.x * view.clip_x +
                  position.y * view.clip_y +
                  position.z * view.clip_z +
                  view.clip_w +
                  vec4(copy_offset.xy, 0.0, 0.0);
}
