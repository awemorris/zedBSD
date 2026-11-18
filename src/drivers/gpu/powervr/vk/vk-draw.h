/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The data of the draw-state records the Venus executor keeps
 * (vk-draw.c writes them, vk-pass.c reads them into the render jobs).
 */

#ifndef KERN_DRIVERS_GPU_POWERVR_VK_DRAW_H
#define KERN_DRIVERS_GPU_POWERVR_VK_DRAW_H

#include <stdint.h>

#include <libc/vulkan/vulkan_core.h>

#include "drivers/gpu/powervr/render-pass.h"
#include "drivers/gpu/powervr/render-draw.h"
#include "drivers/gpu/powervr/compiler/shader.h"

/* The data of a vertex buffer bind: the first binding, the buffers' offsets. */
struct powervr_vk_vertex_bind {
	uint32_t first;
	uint32_t count;
	uint64_t offsets[POWERVR_DRAW_VERTEX_BUFFERS];
};

/*
 * The dynamic offsets of a graphics bind of descriptor sets, in the
 * order the command gives them: the sets' in order, each set's by
 * binding and element.
 */
struct powervr_vk_set_offsets {
	uint32_t count;
	uint32_t offsets[POWERVR_DRAW_DYNAMIC_OFFSETS];
};

/* The data of a viewport or scissor set and of a push constant update. */
struct powervr_vk_state_record {
	VkViewport viewport;
	VkRect2D scissor;
	uint32_t push_offset;
	uint32_t push_bytes;
	uint8_t push[POWERVR_DRAW_PUSH_BYTES];
};

/*
 * A graphics pipeline: its fixed state, its two compiled shaders and the
 * varyings between them; the render job reads it to place the draw's
 * programs.
 */
struct powervr_vk_graphics {
	struct powervr_draw_fixed fixed;
	struct powervr_shader_program vertex;
	struct powervr_shader_program fragment;
	uint8_t *vertex_code;
	uint8_t *fragment_code;
	struct powervr_draw_varyings varyings;
};

#endif /* KERN_DRIVERS_GPU_POWERVR_VK_DRAW_H */
