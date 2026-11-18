/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* Rotate the original cuboid vertices and project into Vulkan clip space. */
#version 450

layout(location = 0) in vec3 position;
layout(location = 1) in vec2 texture_position;
layout(location = 0) out vec2 texture_coordinate;

layout(push_constant) uniform Animation {
    float seconds;
} animation;

void main()
{
    float angle_x = 0.30 + 0.43 * animation.seconds;
    float angle_y = 0.40 + 0.70 * animation.seconds;
    float sx = sin(angle_x);
    float cx = cos(angle_x);
    float sy = sin(angle_y);
    float cy = cos(angle_y);
    vec3 rotated_x = vec3(position.x,
                         cx * position.y - sx * position.z,
                         sx * position.y + cx * position.z);
    vec3 view = vec3(cy * rotated_x.x + sy * rotated_x.z,
                     rotated_x.y,
                     -sy * rotated_x.x + cy * rotated_x.z + 3.0);

    gl_Position = vec4(1.2 * view.x,
                       -1.6 * view.y,
                       (10.0 / 9.9) * view.z - (1.0 / 9.9),
                       view.z);
    texture_coordinate = texture_position;
}
