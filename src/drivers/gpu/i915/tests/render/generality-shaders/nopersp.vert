/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The generality test's interpolation step, vertex stage: the
 * quad's corners at (x, y) with a w of their own (position.w), sent as
 * (x w, y w, 0, w) so they land where (x, y) says; the corner's x passed on
 * interpolated without perspective, x + 3 with it, and the corner's number,
 * flat (the provoking vertex's), through an output block whose members
 * carry the interpolation.
 */
#version 450

layout(location = 0) in vec4 position;
layout(location = 1) in vec4 coordinate;

layout(location = 0) out Corner {
    noperspective float linear_x;
    float perspective_x;
    flat int number;
} corner;

void main()
{
    corner.linear_x = position.x;
    corner.perspective_x = position.x + 3.0;
    corner.number = int(coordinate.z);
    gl_Position = vec4(position.xy * position.w, 0.0, position.w);
}
