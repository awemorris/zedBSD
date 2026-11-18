/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The building of one submission's command stream from command buffer
 * records, shared by the queue (vk-queue.c) and the render passes and
 * draws (vk-draw.c).
 *
 * The stream is written into the session's stage memory, and so are the
 * descriptors its work reads; the stage lives until the submission's
 * work ended.  The bound state (pipelines, sets, push constants, vertex
 * and index buffers, viewport and scissor) carries from record to
 * record as Vulkan says.
 */

#ifndef KERN_DRIVERS_GPU_MALI_VK_BUILD_H
#define KERN_DRIVERS_GPU_MALI_VK_BUILD_H

#include <stdbool.h>
#include <stdint.h>

#include "drivers/gpu/mali/cs/cs-build.h"
#include "drivers/gpu/mali/vk/vk.h"

/*
 * The work registers the stream's own loops and moves use, above the
 * staging registers every run reads (r0..r60): sixteen data registers,
 * then ten scratch ones.  The ring's are the last four (from 92).
 */
#define MALI_VK_DATA_REGISTER		64U
#define MALI_VK_SCRATCH_REGISTER	80U
#define MALI_VK_REGISTERS_NEEDED	92U

/* The scoreboard entries: loads and stores signal 0, endpoint work 1. */
#define MALI_VK_SLOT_LOAD_STORE		0U
#define MALI_VK_SLOT_ENDPOINT		1U
#define MALI_VK_WAIT_ALL		0xffffU

/* The most instructions of one submission's stream (256 KiB). */
#define MALI_VK_STREAM_INSTRUCTIONS	32768U

/*
 * One submission being built: the session, the stream, the state bound
 * so far, and the first error.
 */
struct mali_vk_build {
	struct mali_vk_session *session;
	struct mali_cs cs;
	uint64_t stream_address;
	int error;
	const struct mali_vk_pipeline *compute;
	const struct mali_vk_pipeline *graphics;
	const struct mali_vk_descriptor_set *compute_sets[MALI_VK_SETS_MAX];
	const struct mali_vk_descriptor_set *graphics_sets[MALI_VK_SETS_MAX];
	uint8_t push[MALI_VK_PUSH_BYTES];
	const struct mali_vk_buffer *vertex_buffers[MALI_VK_VERTEX_BINDINGS_MAX];
	uint64_t vertex_offsets[MALI_VK_VERTEX_BINDINGS_MAX];
	const struct mali_vk_buffer *index_buffer;
	uint64_t index_offset;
	uint32_t index_type;
	bool viewport_set;
	VkViewport viewport;
	bool scissor_set;
	VkRect2D scissor;
	bool load_warned;

	/*
	 * aspect_warned says a copy or a clear of one aspect of a depth and
	 * stencil image was left out (said once a submission).
	 */
	bool aspect_warned;

	/*
	 * The state the commands set for a pipeline that takes it from them:
	 * the depth bias's constant, clamp and slope factors (float bits),
	 * and each face's (front 0, back 1) stencil masks and reference.
	 */
	uint32_t bias[3];
	uint32_t compare_mask[2];
	uint32_t write_mask[2];
	uint32_t reference[2];
};

/* Takes stage memory of the submission; NULL with the build's error set when none is left. */
volatile uint8_t *mali_vk_build_take(struct mali_vk_build *build, uint64_t bytes, uint64_t align, uint64_t *gpu_address);

/*
 * Writes a program's FAU words (push constants, buffer addresses, and a
 * vertex program's viewport) and its resource table (textures and
 * samplers, and for a vertex program its attributes and vertex buffers)
 * into the stage, for the sets bound (the system set's buffer at
 * system_address); the FAU pointer word and the table pointer word as
 * the staging registers take them.
 */
int mali_vk_build_resources(struct mali_vk_build *build, const struct mali_shader_program *program, const struct mali_vk_descriptor_set *const *sets, const struct mali_vk_graphics *graphics, const uint32_t *viewport_words, uint64_t system_address, uint64_t *fau, uint64_t *table);

/* Keeps the state a binding record binds (vk-queue.c). */
void mali_vk_build_bind(struct mali_vk_build *build, const struct mali_vk_record *entry);

/* Keeps the depth bias or a stencil mask or reference a record sets (vk-draw.c). */
void mali_vk_draw_set_state(struct mali_vk_build *build, const struct mali_vk_record *entry);

/*
 * Writes a thread storage descriptor into the stage with thread local
 * storage for a program of tls_bytes a thread (the program's tls_bytes;
 * none for zero) and no workgroup memory, and reports its address (zero
 * with the build's error set when the stage has no room).
 */
uint64_t mali_vk_build_thread_storage(struct mali_vk_build *build, uint32_t tls_bytes);

/*
 * Writes a compute run into the stream: the staging registers of the
 * tables, the FAU, the program, the storage, the workgroup size and the
 * workgroup counts, the run, and a wait for it.
 */
void mali_vk_build_compute(struct mali_vk_build *build, uint64_t table, uint64_t fau, uint64_t shader, uint64_t storage, const uint32_t *local_size, const uint32_t *counts);

/* Writes a shader program descriptor into the stage. */
uint64_t mali_vk_build_shader(struct mali_vk_build *build, unsigned stage, uint32_t work_registers, uint32_t preload, uint64_t code);

#endif
