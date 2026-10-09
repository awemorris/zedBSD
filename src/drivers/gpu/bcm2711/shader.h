/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* Private scalar SPIR-V lowering returns native code and its immutable draw-time interface. */
#ifndef KERN_DRIVERS_GPU_BCM2711_SHADER_H
#define KERN_DRIVERS_GPU_BCM2711_SHADER_H

#include <stddef.h>
#include <stdint.h>

/* Thirty-two scalar interface words leave room for expression registers in a two-thread shader. */
#define BCM2711_SHADER_INTERFACE_WORDS 32U

/* Native vertex shaders have distinct coordinate and render-stage VPM output layouts. */
enum bcm2711_shader_stage {
	BCM2711_SHADER_COORDINATE,
	BCM2711_SHADER_VERTEX,
	BCM2711_SHADER_FRAGMENT
};

/* One draw-time uniform describes either exact constant bits or a checked caller-owned binding. */
enum bcm2711_shader_uniform_kind {
	BCM2711_SHADER_CONSTANT,
	BCM2711_SHADER_PUSH,
	BCM2711_SHADER_BLOCK,
	BCM2711_SHADER_TEXTURE,
	BCM2711_SHADER_SAMPLER,
	BCM2711_SHADER_VIEWPORT_X,
	BCM2711_SHADER_VIEWPORT_Y,
	BCM2711_SHADER_VIEWPORT_Z,
	BCM2711_SHADER_DEPTH_OFFSET
};

/* One varying component identifies the canonical FIFO order shared by both graphics stages. */
struct bcm2711_shader_component {
	uint32_t location;
	uint32_t component;
	uint32_t flat;
	uint32_t noperspective;
};

/* One pipeline key keeps interpolation and tile-output choices out of mutable shader modules. */
struct bcm2711_shader_key {
	struct bcm2711_shader_component varyings[BCM2711_SHADER_INTERFACE_WORDS];
	uint32_t varying_count;
	uint32_t swap_red_blue;
	uint32_t premultiplied_blend;
};

/* One uniform belongs to one emitted hardware consumption, including implicit tile writes. */
struct bcm2711_shader_uniform {
	enum bcm2711_shader_uniform_kind kind;
	uint32_t bits;
	uint32_t set;
	uint32_t binding;
	uint32_t offset;
};

/* One fully compiled program owns its arrays until the caller releases it after copying into GPU storage. */
struct bcm2711_shader_binary {
	uint64_t *code;
	uint32_t code_count;
	struct bcm2711_shader_uniform *uniforms;
	uint32_t uniform_count;
	enum bcm2711_shader_stage stage;
	struct bcm2711_shader_component inputs[BCM2711_SHADER_INTERFACE_WORDS];
	uint32_t input_count;
	struct bcm2711_shader_component varyings[BCM2711_SHADER_INTERFACE_WORDS];
	uint32_t varying_count;
	uint32_t push_bytes;
	uint32_t vpm_output_words;
	uint32_t registers_used;
	uint32_t threads;
	uint32_t starts_final;
};

/* One refusal names the source instruction; its reason points to immutable implementation text. */
struct bcm2711_shader_diagnostic {
	uint32_t instruction;
	const char *reason;
};

int bcm2711_shader_compile(const uint32_t *words, size_t word_count, enum bcm2711_shader_stage stage, const struct bcm2711_shader_key *key, struct bcm2711_shader_binary **binary, struct bcm2711_shader_diagnostic *diagnostic);
void bcm2711_shader_binary_free(struct bcm2711_shader_binary *binary);

#endif
