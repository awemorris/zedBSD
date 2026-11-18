#!/bin/sh
# Writes ../load-draw-shader.h from load.vert and load.frag: the SPIR-V of
# the vertex shader and of the fragment shader for each colour attachment
# location 0 to 3, each kind (floats, unsigned and signed integers) and
# one sample or several (MULTISAMPLED), compiled by
# "glslc --target-env=vulkan1.0".  Run from this directory
# after changing a shader, WORK a directory for the compiled files (kept,
# e.g. under build/).
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -eu
out=../load-draw-shader.h
work=${1:?usage: regenerate-load.sh WORK}
mkdir -p "$work"
kinds="float unsigned signed"

# Prints the SPIR-V words of FILE as a C table named NAME.
table() {
	echo ""
	echo "/* $3 */"
	echo "static const uint32_t $2[] = {"
	od -An -tx4 -v "$1" | awk '{ line = "\t"; for (i = 1; i <= NF; i++) line = line "0x" $i "U, "; sub(/ $/, "", line); print line }'
	echo "};"
}

glslc --target-env=vulkan1.0 -o "$work/vert.spv" load.vert
for location in 0 1 2 3; do
	kind=0
	for name in $kinds; do
		glslc --target-env=vulkan1.0 -DLOCATION=$location -DKIND=$kind -DMULTISAMPLED=0 -fshader-stage=frag -o "$work/frag$location-$name.spv" load.frag
		glslc --target-env=vulkan1.0 -DLOCATION=$location -DKIND=$kind -DMULTISAMPLED=1 -fshader-stage=frag -o "$work/frag$location-$name-ms.spv" load.frag
		kind=$((kind + 1))
	done
done

{
	cat <<'HEAD'
/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The SPIR-V of the draw that loads a colour attachment (render-job.c):
 * a vertex shader that passes a position and a texture coordinate
 * (R32G32 each, one binding), and fragment shaders that sample the
 * combined image sampler of set 0, binding 0 there into the colour
 * attachment at one location, of floats, unsigned or signed integers;
 * and for an attachment of several samples, run for each sample, the
 * texel of the pixel and sample fetched instead.
 * Written by shaders/regenerate-load.sh from shaders/load.vert and
 * shaders/load.frag; do not edit.
 */

#ifndef KERN_DRIVERS_GPU_POWERVR_LOAD_DRAW_SHADER_H
#define KERN_DRIVERS_GPU_POWERVR_LOAD_DRAW_SHADER_H

#include <stdint.h>
HEAD
	table "$work/vert.spv" powervr_load_draw_vert "The vertex shader."
	for location in 0 1 2 3; do
		for name in $kinds; do
			table "$work/frag$location-$name.spv" powervr_load_draw_frag${location}_$name "The fragment shader of location $location, $name."
			table "$work/frag$location-$name-ms.spv" powervr_load_draw_frag${location}_${name}_multisampled "The fragment shader of location $location, $name, of several samples."
		done
	done
	echo ""
	echo "#endif /* KERN_DRIVERS_GPU_POWERVR_LOAD_DRAW_SHADER_H */"
} > "$out"
