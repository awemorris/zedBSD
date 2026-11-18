/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Writes the pushed colour of the executor test, scaled by the vertex
 * colour: a white vertex leaves the pushed colour as it is.
 *
 * The colour sits at byte 112 of the push block, where the model viewer
 * pushes its material colour.
 */
#version 450

layout(location = 0) in vec4 vertex_color;

layout(location = 0) out vec4 fragment_color;

layout(push_constant) uniform Material {
    layout(offset = 112) vec4 color;
} material;

void main()
{
    fragment_color = vertex_color * material.color;
}
