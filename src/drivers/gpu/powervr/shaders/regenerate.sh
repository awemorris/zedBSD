#!/bin/sh
# Writes ../clear-draw-shader.h from clear.vert and clear.frag: the SPIR-V
# of the vertex shader and of the fragment shader for 0 to 4 colour
# outputs, compiled by "glslc --target-env=vulkan1.0 -O0".  Run from this
# directory after changing a shader, WORK a directory for the compiled
# files (kept, e.g. under build/).
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -eu
out=../clear-draw-shader.h
work=${1:?usage: regenerate.sh WORK}
mkdir -p "$work"

# Prints the SPIR-V words of FILE as a C table named NAME.
table() {
	echo ""
	echo "/* $3 */"
	echo "static const uint32_t $2[] = {"
	od -An -tx4 -v "$1" | awk '{ line = "\t"; for (i = 1; i <= NF; i++) line = line "0x" $i "U, "; sub(/ $/, "", line); print line }'
	echo "};"
}

glslc --target-env=vulkan1.0 -O0 -o "$work/vert.spv" clear.vert
for n in 0 1 2 3 4; do
	glslc --target-env=vulkan1.0 -O0 -DOUTPUTS=$n -fshader-stage=frag -o "$work/frag$n.spv" clear.frag
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
 * The SPIR-V of the draw that clears part of a render pass's attachments
 * (render-job.c): a vertex shader that places a position (R32G32, one
 * binding) at the depth in the push constants, and fragment shaders that
 * write the push constants' colours to the first 0 to 4 colour
 * attachments.  Written by shaders/regenerate.sh from shaders/clear.vert
 * and shaders/clear.frag; do not edit.
 */

#ifndef KERN_DRIVERS_GPU_POWERVR_CLEAR_DRAW_SHADER_H
#define KERN_DRIVERS_GPU_POWERVR_CLEAR_DRAW_SHADER_H

#include <stdint.h>
HEAD
	table "$work/vert.spv" powervr_clear_draw_vert "The vertex shader."
	for n in 0 1 2 3 4; do
		table "$work/frag$n.spv" powervr_clear_draw_frag$n "The fragment shader of $n colour outputs."
	done
	echo ""
	echo "#endif /* KERN_DRIVERS_GPU_POWERVR_CLEAR_DRAW_SHADER_H */"
} > "$out"
