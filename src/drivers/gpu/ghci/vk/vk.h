/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The Vulkan executor of a GHCI device.
 *
 * libvulkan sends the zedBSD GPU command protocol (uapi/gpu-op.h) to the
 * node.  The executor keeps the Vulkan objects in the kernel: it decodes
 * every command, answers the instance, the physical device and its
 * limits from the GPU's capability registers, keeps memory, buffers,
 * images, command pools and buffers, fences, semaphores and events, and
 * turns a submission into GHCI packets in the ring of the device.
 *
 * One VkDevice is one GHCI session (a number 1 to 15) with its own GPU
 * addresses, and one executor heap in them for what the driver itself
 * writes for the GPU.  The objects of a node session live in a table of
 * the session's, found by their kind and the identity libvulkan chose.
 *
 * Locks.  The GPU core admits one ordinary command of a node session at
 * a time, so the object table and the arena need no lock of their own.
 * What the service thread, the job ioctls and the GPU core's recovery
 * read besides (the sessions of the executor, each session's devices,
 * timelines, job markers, queued frames and last tickets, and the stop
 * marks) changes only under the device's irq_lock.  The executor calls
 * the core's session and memory functions without the device's lock.
 */

#ifndef DRIVERS_GPU_GHCI_VK_VK_H
#define DRIVERS_GPU_GHCI_VK_VK_H

#include <stddef.h>
#include <stdint.h>

#include <libc/vulkan/vulkan_core.h>

#include "drivers/gpu/i915/vk/vk-codec.h"

#include "../ghci.h"
#include "../ghci-cmd.h"
#include "../layout.h"
#include "../compiler/gsi1-abi.h"
#include "../compiler/gsi1-compile.h"

/*
 * The record decoders (drivers/gpu/i915/vk/vk-codec.inc) size each array
 * by its own count on the wire, which a client may make differ from the
 * Vulkan count field beside it.  The executor's decoders take their
 * arrays through ghci_vk_decode_array(), which keeps each array's
 * decoded length in the scratch just before it, so that every count
 * field is checked against the array (ghci_vk_decoded_count()) before
 * the array is read by it.
 */
#define i915_vkc_array(reader, arena, count, element) \
	ghci_vk_decode_array((reader), (arena), (count), (element))

/* The buckets of a session's object table, and the most objects a session keeps. */
#define GHCI_VK_BUCKETS			256U
#define GHCI_VK_OBJECTS_MAX		65536U

/* The scratch one command decodes its records into. */
#define GHCI_VK_ARENA_BYTES		(256U * 1024U)

/* The timelines a queue may name (1 to 63; 0 is the node session's own). */
#define GHCI_VK_TIMELINES		64U

/* The VkFormat values of Vulkan 1.0 (0 to VK_FORMAT_ASTC_12x12_SRGB_BLOCK). */
#define GHCI_VK_FORMATS			185U

/* The completions the service retires in one pass under the interrupt's lock. */
#define GHCI_VK_DONE_BATCH		32U

/* The size of the memory heap the device reports, and the largest allocation. */
#define GHCI_VK_HEAP_SIZE		(256ULL << 20)

/*
 * The memory types.  A unified GPU has one, device local, host visible
 * and coherent, in host memory.  A discrete GPU whose local memory is in
 * use has two: device local alone, in local memory (heap 0), and host
 * visible and coherent, in host memory (heap 1).
 */
#define GHCI_VK_TYPE_LOCAL		0U
#define GHCI_VK_TYPE_HOST		1U

/* The alignment of every buffer and image in memory (the tiled subresource's 256 bytes). */
#define GHCI_VK_ALIGN			256U

/* The first block of a device's executor heap, the largest, and the runs one block keeps. */
#define GHCI_VK_HEAP_FIRST_BYTES	(64U * 1024U)
#define GHCI_VK_HEAP_BLOCK_MAX		(16U << 20)
#define GHCI_VK_HEAP_EXTENTS		256U

/* The events whose words one submission's packets may write. */
#define GHCI_VK_SUBMIT_EVENTS		64U

/*
 * The generation of a ticket that names no fence but the device's held
 * submissions: its seq is the serial of the last one held when the
 * ticket was taken, and a job marker or a frame with it waits until that
 * submission is placed (it then takes that submission's ticket) or fails
 * with it.
 */
#define GHCI_VK_HELD_GENERATION		UINT64_MAX

/*
 * The barriers around a run of a submission's packets: at the head, the
 * host's writes (code, descriptors, buffers) seen; at the tail, the GPU's
 * writes in memory before the fence after them.
 */
#define GHCI_VK_HEAD_FLAGS		(GHCI_BARRIER_INVAL_ICACHE | GHCI_BARRIER_INVAL_TEX | GHCI_BARRIER_INVAL_L1_DATA | \
					 GHCI_BARRIER_INVAL_DESCRIPTOR)
#define GHCI_VK_TAIL_FLAGS		(GHCI_BARRIER_WAIT_IDLE | GHCI_BARRIER_FLUSH_L1_DATA | GHCI_BARRIER_FLUSH_L2)

/* The most bytes of recorded commands one command buffer keeps. */
#define GHCI_VK_RECORD_MAX		(16U << 20)

/*
 * The host bytes the objects of one node session may keep: shader
 * modules' words, pipelines (their records and runs of the executor
 * heap), descriptor set layouts, pipeline layouts, descriptor sets and
 * command buffers' recordings.  A create past it is answered
 * VK_ERROR_OUT_OF_HOST_MEMORY.
 */
#define GHCI_VK_BUDGET_BYTES		(64ULL << 20)

/* The pipelineCacheUUID the device reports and a pipeline cache's header carries. */
#define GHCI_VK_CACHE_UUID		"zedBSD-ghci-pc10"

/* The descriptor sets a pipeline layout binds (maxBoundDescriptorSets). */
#define GHCI_VK_SETS			4U

/* The descriptor types of Vulkan 1.0 (VK_DESCRIPTOR_TYPE_SAMPLER to INPUT_ATTACHMENT). */
#define GHCI_VK_DESCRIPTOR_TYPES	11U

/* The colour attachments of a subpass and the input attachments a fragment program reads. */
#define GHCI_VK_COLOR_ATTACHMENTS	8U

/*
 * The constant-bank words a draw or a dispatch loads: push constants in
 * c0-c23 and c32-c39, the dynamic offsets from c40.  c24-c31 hold the
 * hardware's mirror of the tables and are never loaded.
 */
#define GHCI_VK_BANK_WORDS		(40U + GHCI_GSI1_DYNAMIC_OFFSETS)

/* What a format is besides its size (ghci_vk_format_traits). */
#define GHCI_VK_FORMAT_INTEGER		(1U << 0)
#define GHCI_VK_FORMAT_SRGB		(1U << 1)
#define GHCI_VK_FORMAT_DEPTH		(1U << 2)
#define GHCI_VK_FORMAT_BLOCK		(1U << 3)

/* The kinds of object the executor keeps; a lookup names the kind it expects. */
enum ghci_vk_kind {
	GHCI_VK_DEVICE = 1,
	GHCI_VK_QUEUE,
	GHCI_VK_MEMORY,
	GHCI_VK_BUFFER,
	GHCI_VK_IMAGE,
	GHCI_VK_FENCE,
	GHCI_VK_SEMAPHORE,
	GHCI_VK_EVENT,
	GHCI_VK_COMMAND_POOL,
	GHCI_VK_COMMAND_BUFFER,
	GHCI_VK_SAMPLER,
	GHCI_VK_IMAGE_VIEW,
	GHCI_VK_BUFFER_VIEW,
	GHCI_VK_SHADER_MODULE,
	GHCI_VK_PIPELINE_CACHE,
	GHCI_VK_SET_LAYOUT,
	GHCI_VK_PIPELINE_LAYOUT,
	GHCI_VK_DESCRIPTOR_POOL,
	GHCI_VK_DESCRIPTOR_SET,
	GHCI_VK_PIPELINE,
	GHCI_VK_RENDER_PASS,
	GHCI_VK_FRAMEBUFFER,
	GHCI_VK_QUERY_POOL
};

/* The life of a command buffer: new or reset, recording, and ended (ready to submit). */
enum ghci_vk_recording {
	GHCI_VK_RECORD_INITIAL = 0,
	GHCI_VK_RECORD_RECORDING,
	GHCI_VK_RECORD_EXECUTABLE
};

struct ghci_vk_session;
struct ghci_vk_object;
struct ghci_vk_hold;

/*
 * The driver's own transfer programs (vk-meta.c): a compute program that
 * copies elements of 1 to 4 bytes between two buffers (copies, fills,
 * updates, buffer and image copies, single aspects of depth and
 * stencil), one that packs query results, and the vertex and fragment
 * programs of the draws that resolve (the average of 2 or 4 samples, or
 * sample 0 of an integer format) and blit (a texture sampled over the
 * destination's rectangle).
 */
enum ghci_vk_meta_kind {
	GHCI_VK_META_COPY = 0,
	GHCI_VK_META_QUERY,
	GHCI_VK_META_RESOLVE_AVERAGE2,
	GHCI_VK_META_RESOLVE_AVERAGE4,
	GHCI_VK_META_RESOLVE_FIRST,
	GHCI_VK_META_BLIT,
	GHCI_VK_META_KINDS
};

/* The programs of a kind: the compute (or vertex) program, and a draw's fragment program. */
#define GHCI_VK_META_PROGRAMS		2U

/* The push-constant words of a transfer program (c0 to c15). */
#define GHCI_VK_META_PUSH_WORDS		16U

/*
 * The copy program's push-constant words: the elements, the elements of
 * a row and the rows of a slice, the mask of an element's bytes, the
 * source's first byte (from its descriptor's address), element stride,
 * row pitch and slice pitch, the destination's four, the bytes of an
 * element, and the first element of the dispatch.
 */
#define GHCI_VK_META_COPY_COUNT		0U
#define GHCI_VK_META_COPY_PER_ROW	1U
#define GHCI_VK_META_COPY_ROWS		2U
#define GHCI_VK_META_COPY_MASK		3U
#define GHCI_VK_META_COPY_SOURCE	4U
#define GHCI_VK_META_COPY_DESTINATION	8U
#define GHCI_VK_META_COPY_BYTES		12U
#define GHCI_VK_META_COPY_FIRST		13U
#define GHCI_VK_META_COPY_WORDS		14U

/*
 * The query program's push-constant words: the queries, the first slot's
 * byte (from the source descriptor's address), the destination's first
 * byte and stride, the flags below, and the first query of the dispatch.
 */
#define GHCI_VK_META_QUERY_COUNT	0U
#define GHCI_VK_META_QUERY_SOURCE	1U
#define GHCI_VK_META_QUERY_DESTINATION	2U
#define GHCI_VK_META_QUERY_STRIDE	3U
#define GHCI_VK_META_QUERY_FLAGS	4U
#define GHCI_VK_META_QUERY_FIRST	5U
#define GHCI_VK_META_QUERY_WORDS	6U

/* The query program's flags: 64-bit results, the availability after each, and values not yet available written too. */
#define GHCI_VK_META_QUERY_64		1U
#define GHCI_VK_META_QUERY_AVAILABILITY	2U
#define GHCI_VK_META_QUERY_ANY		4U

/*
 * A resolve draw's push-constant words: the source's texel less the
 * destination's pixel, x and y (integers).
 */
#define GHCI_VK_META_RESOLVE_DX		0U
#define GHCI_VK_META_RESOLVE_DY		1U
#define GHCI_VK_META_RESOLVE_WORDS	2U

/*
 * A blit draw's push-constant words (integers): for x, then for y, the
 * source's two corners, the destination's two corners, the viewport's
 * first pixel and its end; then the source level's width and height.
 */
#define GHCI_VK_META_BLIT_X		0U
#define GHCI_VK_META_BLIT_Y		6U
#define GHCI_VK_META_BLIT_SIZE		12U
#define GHCI_VK_META_BLIT_WORDS		14U

/* The invocations of a transfer program's workgroup, and the most workgroups one dispatch of it runs. */
#define GHCI_VK_META_GROUP		64U
#define GHCI_VK_META_GROUPS_MAX		65535U

/* The most dwords one step of a transfer takes: its push constants (LOAD_CONST) and one DISPATCH. */
#define GHCI_VK_META_STEP_DWORDS	(3U + GHCI_VK_META_PUSH_WORDS + GHCI_DW_DISPATCH)

/* The dwords of a transfer's binds: its pipeline and its resource table. */
#define GHCI_VK_META_BIND_DWORDS	(GHCI_DW_BIND_PIPELINE + GHCI_DW_BIND_DESCRIPTOR_TABLE)

/*
 * One of the driver's transfer programs placed in a device's code heap:
 * its pipeline state record and code (one run, which goes with the
 * heap), the code's words (a draw's vertex program, then its fragment
 * program), and for a compute program the byte offsets in the resource
 * table (set 2) of its source and destination buffers' descriptors, and
 * the table's bytes; a draw reads texture and sampler slot 0, and
 * blend_word is its record's blend word of colour attachment 0 (no
 * blending, every component written).  A zero pso_va is a program not
 * placed yet.
 */
struct ghci_vk_meta_pipeline {
	struct ghci_vk_heap_block *block;
	uint32_t offset;
	uint64_t pso_va;
	uint64_t code_va;
	uint32_t code_words;
	uint64_t fragment_va;
	uint32_t fragment_words;
	uint32_t source_entry;
	uint32_t destination_entry;
	uint32_t resource_bytes;
	uint32_t blend_word;
};

/*
 * A copy of elements the copy program makes: the GPU addresses of the
 * first element's first byte on each side, the bytes of an element (1
 * to 4), the elements of a row, the rows of a slice and the slices, and
 * on each side the element stride, the row pitch and the slice pitch in
 * bytes.  A stride of 0 reads one element again (a fill).
 */
struct ghci_vk_meta_copy {
	uint64_t source;
	uint64_t destination;
	uint32_t element_bytes;
	uint32_t per_row;
	uint32_t rows;
	uint32_t slices;
	uint32_t source_step[3];
	uint32_t destination_step[3];
};

/*
 * One transfer as the executor places it: the program, the buffers its
 * descriptors give (address and bytes, both within 32-bit addresses),
 * the push-constant words, which word is the first element of a
 * dispatch, and the elements in all (dispatched GHCI_VK_META_GROUP to a
 * workgroup).
 */
struct ghci_vk_meta_dispatch {
	enum ghci_vk_meta_kind kind;
	uint64_t source;
	uint32_t source_bytes;
	uint64_t destination;
	uint32_t destination_bytes;
	uint32_t push[GHCI_VK_META_PUSH_WORDS];
	uint32_t push_words;
	uint32_t first_word;
	uint32_t elements;
};

/*
 * The driver's copy program placed in session 0 for the copies of rows
 * the core of the driver asks for (the display's copied presents): one
 * run of host memory mapped once into session 0, executable and read-only
 * to the GPU, that holds the program's pipeline state record and code and,
 * at table_at, the resource table each copy writes before it is placed
 * (only the driver writes the run; the GPU only reads it).  memory is
 * NULL until the first copy, and the run stays until the detach.  lock
 * makes the placement, a copy's table and its packets in the ring one
 * step.
 */
struct ghci_vk_rows {
	struct mutex lock;
	struct ghci_allocation *memory;
	struct ghci_mapping *mapping;
	uint32_t table_at;
	struct ghci_vk_meta_pipeline pipeline;
};

/*
 * The Vulkan format features of each VkFormat on optimal and linear
 * tiling and in buffers, made once at the attach from the FMT block (or
 * the table of the specification without it) and not changed after.
 * from_registers says that FMT was read.
 */
struct ghci_vk_formats {
	uint32_t optimal[GHCI_VK_FORMATS];
	uint32_t linear[GHCI_VK_FORMATS];
	uint32_t buffer[GHCI_VK_FORMATS];
	int from_registers;
};

/*
 * The executor of one GHCI device, from the attach to the detach.
 *
 * sessions lists the node sessions with the executor's side open
 * (irq_lock), for the service thread's retirement.  completing_wake
 * wakes a drain once a session's last retired completion has been
 * reported to the core.  refusals counts the refused commands the log
 * has shown, so that a client cannot flood it; it is counted without a
 * lock (two sessions at once may show one more).  meta holds the
 * driver's own transfer programs (vk-meta.c), each compiled at its first
 * use and kept to the detach; a slot changes from NULL to its program
 * once, under the device's lock.  meta_refused marks a kind whose
 * program the compiler refused (or that does not fit its pipeline
 * state), set once under the device's lock, so that it is not compiled
 * again.  rows is the copy program placed in session 0 for the core's
 * copies of rows.
 */
struct ghci_vk {
	struct ghci_device *device;
	struct ghci_vk_formats formats;
	struct ghci_vk_session *sessions;
	struct wait_queue completing_wake;
	unsigned refusals;
	struct ghci_gsi1_program *meta[GHCI_VK_META_KINDS][GHCI_VK_META_PROGRAMS];
	int meta_refused[GHCI_VK_META_KINDS];
	struct ghci_vk_rows rows;
};

/*
 * The two heaps of a device: code (mapped executable: pipeline state
 * records, programs and the driver's table of set 3) and data (never
 * executable: scratch memory, event words, query pools and the
 * temporary memory of a submission), so that nothing the GPU writes or
 * a client fills lies where the GPU may run it as code.
 */
enum ghci_vk_heap {
	GHCI_VK_HEAP_CODE = 0,
	GHCI_VK_HEAP_DATA,
	GHCI_VK_HEAPS
};

/*
 * One block of a device's executor heap: host memory mapped once into
 * the device's GHCI session, and the allocator of its bytes.  The block
 * lives until its device is destroyed.
 */
struct ghci_vk_heap_block {
	struct ghci_vk_heap_block *next;
	struct ghci_allocation *allocation;
	struct ghci_mapping *mapping;
	struct ghci_range_map map;
	struct ghci_range_extent extents[GHCI_VK_HEAP_EXTENTS];
};

/*
 * A run of the executor heap let go while work placed before may still
 * read it: it goes back to its block once the ticket has passed.
 */
struct ghci_vk_retired {
	struct ghci_vk_retired *next;
	struct ghci_vk_heap_block *block;
	uint32_t offset;
	struct ghci_ticket ticket;
};

/*
 * One VkDevice: its GHCI session, its executor heaps (code and data, the
 * bytes of each), the runs let go that wait for a ticket, the ticket of
 * the last work it placed in the ring, and the submissions it holds back
 * (holds, oldest first; hold_serial is the serial of the last one held
 * and hold_count how many are held; failed_timelines has a bit for each
 * timeline whose next context fence reports held work that failed).  It
 * lives from vkCreateDevice to vkDestroyDevice (or the node session's
 * close).  next, last, submitted, hold_serial, hold_count and
 * failed_timelines change under irq_lock; the rest belongs to
 * whoever holds the session's hold_lock.  meta are the driver's transfer
 * programs placed in its code heap at their first use, which go with the
 * heap.
 */
struct ghci_vk_device {
	struct ghci_vk_session *session;
	struct ghci_vk_device *next;
	struct ghci_vm *vm;
	struct ghci_vk_heap_block *heap[GHCI_VK_HEAPS];
	uint64_t heap_bytes[GHCI_VK_HEAPS];
	struct ghci_vk_retired *retired;
	struct ghci_ticket last;
	int submitted;
	int robust;
	struct ghci_vk_meta_pipeline meta[GHCI_VK_META_KINDS];
	struct ghci_vk_hold *holds;
	uint64_t hold_serial;
	unsigned hold_count;
	uint64_t failed_timelines;
};

/*
 * A VkDeviceMemory: an allocation of its own (or the allocation of an
 * imported resource), which it holds, and its mapping in the device's
 * GHCI session.
 */
struct ghci_vk_memory {
	struct ghci_allocation *allocation;
	struct ghci_mapping *mapping;
	uint64_t bytes;
	uint32_t type;
	int imported;
};

/*
 * A VkBuffer: its size and use, and where it is bound: the identity of
 * the memory object (0 until then; the memory may be freed before the
 * buffer, so it is found again by its identity) and the offset.
 */
struct ghci_vk_buffer {
	uint64_t size;
	uint32_t usage;
	uint32_t flags;
	uint64_t memory;
	uint64_t offset;
};

/*
 * A VkImage: what it was made as, its linear layout (section 8.5.2 of
 * the specification) and its size, and where it is bound (the memory
 * object's identity, as a buffer's).
 */
struct ghci_vk_image {
	uint32_t format;
	uint32_t type;
	uint32_t tiling;
	uint32_t usage;
	uint32_t flags;
	struct ghci_layout_image layout;
	uint64_t bytes;
	uint64_t memory;
	uint64_t offset;
};

/*
 * A VkFence: signaled on the host, or by the ticket of the submission
 * that signals it.  held says that the submission is held back and has
 * no ticket yet; lost that it failed before it was placed.
 */
struct ghci_vk_fence {
	int signaled;
	int submitted;
	int held;
	int lost;
	struct ghci_ticket ticket;
};

/*
 * A VkEvent: one 64-bit word in the device's data heap that the host and
 * the GPU set, and the ticket of the last submission whose packets write
 * it (gpu_written says that there was one).
 */
struct ghci_vk_event {
	struct ghci_vk_heap_block *block;
	uint32_t offset;
	volatile uint64_t *word;
	struct ghci_ticket written;
	int gpu_written;
};

/* A VkQueue: the timeline its jobs complete on. */
struct ghci_vk_queue {
	uint32_t timeline;
	uint32_t family;
};

/* A VkCommandPool: its flags and queue family. */
struct ghci_vk_command_pool {
	uint32_t flags;
	uint32_t family;
};

/*
 * A VkCommandBuffer: its pool, its level, its recording's state, and the
 * recorded commands as they came on the wire ([number][0][buffer] and
 * the arguments, one after another), which a submission executes.
 * failed says that a command could not be kept (the end of the
 * recording reports it).
 */
struct ghci_vk_command_buffer {
	uint64_t pool;
	uint32_t level;
	enum ghci_vk_recording state;
	int failed;
	uint8_t *bytes;
	uint32_t used;
	uint32_t capacity;
	uint32_t commands;
};

/* A VkSampler: its sampler descriptor (section 8.3 of the specification). */
struct ghci_vk_sampler {
	uint8_t descriptor[GHCI_SAMPLER_DESC_BYTES];
};

/*
 * A VkImageView: the image, what part of it the view is, and the two
 * texture descriptors a set takes of it: sampled (the view's levels from
 * its first, for a sampled image) and single (the first level and layer
 * of the view alone, linear, one level, for a storage image and an input
 * attachment).  va is the address of the first level and layer, where an
 * attachment of the view starts, row_pitch its row, and layer_stride the
 * bytes from one layer of the image to the next.
 */
struct ghci_vk_image_view {
	uint64_t image;
	uint32_t format;
	uint32_t view_type;
	uint32_t aspect;
	uint32_t base_level;
	uint32_t levels;
	uint32_t base_layer;
	uint32_t layers;
	uint32_t samples;
	uint32_t width;
	uint32_t height;
	uint32_t row_pitch;
	uint32_t layer_stride;
	uint64_t va;
	uint8_t sampled[GHCI_TEXTURE_DESC_BYTES];
	uint8_t single[GHCI_TEXTURE_DESC_BYTES];
};

/*
 * A VkBufferView: the buffer descriptor of the view (stride = the texel's
 * bytes) and, for a uniform texel buffer, the texture descriptor of the
 * view read as a linear 2D image 256 texels wide.
 */
struct ghci_vk_buffer_view {
	uint64_t buffer;
	uint32_t format;
	uint32_t elements;
	uint8_t buffer_descriptor[GHCI_BUFFER_DESC_BYTES];
	uint8_t texture[GHCI_TEXTURE_DESC_BYTES];
};

/* A VkShaderModule: a copy of its SPIR-V words. */
struct ghci_vk_shader_module {
	uint32_t *words;
	uint32_t word_count;
};

/*
 * One binding of a descriptor set layout: its number, type, count and
 * stages, the index of its first descriptor in a set, and whether its
 * samplers are the layout's immutable ones.
 */
struct ghci_vk_layout_binding {
	uint32_t binding;
	uint32_t type;
	uint32_t count;
	uint32_t stages;
	uint32_t first;
	uint32_t immutable;
};

/*
 * A VkDescriptorSetLayout, shared by the layout object, the pipeline
 * layouts and the sets made with it (holds counts them; the last to let
 * go frees it and gives charged back to its session's budget).  The
 * bindings are in increasing binding order, which is the order of a
 * set's descriptors and of its dynamic offsets; samplers holds the
 * immutable samplers' descriptors by descriptor index.
 */
struct ghci_vk_set_layout {
	unsigned holds;
	struct ghci_vk_session *session;
	uint64_t charged;
	uint32_t binding_count;
	uint32_t descriptor_count;
	uint32_t dynamic_count;
	struct ghci_vk_layout_binding *bindings;
	uint8_t *samplers;
};

/*
 * A VkPipelineLayout: its set layouts (held), the push-constant bytes and
 * the dynamic buffers in the order of their offsets (one per descriptor).
 * dynamic_base[i] is the index of set i's first dynamic offset and
 * dynamic_sets[i] how many set i has (0 for a set past set_count), so
 * that the dynamic offsets of a bind of sets i and on land at their place.
 */
struct ghci_vk_pipeline_layout {
	uint32_t set_count;
	struct ghci_vk_set_layout *sets[GHCI_VK_SETS];
	uint32_t push_bytes;
	uint32_t dynamic_count;
	struct ghci_gsi1_dynamic dynamic[GHCI_GSI1_DYNAMIC_OFFSETS];
	uint32_t dynamic_base[GHCI_VK_SETS];
	uint32_t dynamic_sets[GHCI_VK_SETS];
};

/* A VkDescriptorPool: its flags, its sets, and the descriptors of each type it has and has given. */
struct ghci_vk_descriptor_pool {
	uint32_t flags;
	uint32_t max_sets;
	uint32_t sets;
	uint32_t capacity[GHCI_VK_DESCRIPTOR_TYPES];
	uint32_t used[GHCI_VK_DESCRIPTOR_TYPES];
};

/*
 * One descriptor of a set as the hardware tables take it: its type (as
 * written), the texture descriptor of an image or texel buffer, the
 * sampler descriptor, and the buffer descriptor of a buffer or texel
 * buffer.  An unwritten descriptor is zero.
 */
struct ghci_vk_descriptor {
	uint32_t written;
	uint8_t texture[GHCI_TEXTURE_DESC_BYTES];
	uint8_t sampler[GHCI_SAMPLER_DESC_BYTES];
	uint8_t buffer[GHCI_BUFFER_DESC_BYTES];
};

/* A VkDescriptorSet: its pool, its layout (held), and its descriptors. */
struct ghci_vk_set {
	uint64_t pool;
	struct ghci_vk_set_layout *layout;
	struct ghci_vk_descriptor *descriptors;
};

/*
 * The state of a pipeline that its pipeline state record does not keep,
 * which the executor sets with
 * SET_DYNAMIC_STATE, SET_VIEWPORT and SET_SCISSOR at each bind: the
 * VkDynamicState values the pipeline made dynamic (bit per value), and
 * the static values of the others.  Floats are kept as their bits; the
 * payloads are SET_DYNAMIC_STATE's (depth bias constant, slope and clamp;
 * stencil compare mask, write mask and reference per face in one word),
 * and blend_attachment the blend word of colour attachments 0 to
 * blend_count - 1 for a device with independent blending.
 */
struct ghci_vk_static_state {
	uint32_t dynamic;
	uint32_t viewport[6];
	int32_t scissor_offset[2];
	uint32_t scissor_extent[2];
	uint32_t line_width;
	uint32_t depth_bias[3];
	uint32_t blend_constants[4];
	uint32_t stencil[2];
	uint32_t blend_count;
	uint32_t blend_attachment[GHCI_VK_COLOR_ATTACHMENTS];
	uint32_t rasterizer_discard;
};

/*
 * What a pipeline knows of its subpass: the colour attachments' formats,
 * the depth attachment's, the samples, and the input attachments'
 * formats and samples by InputAttachmentIndex.  The render pass's record
 * gives it; without one the pipeline's own state does.
 */
struct ghci_vk_subpass {
	uint32_t color_count;
	uint32_t color_format[GHCI_VK_COLOR_ATTACHMENTS];
	uint32_t depth_format;
	uint32_t samples;
	uint32_t input_format[GHCI_GSI1_INPUT_ATTACHMENTS];
	uint32_t input_samples[GHCI_GSI1_INPUT_ATTACHMENTS];
};

/*
 * One attachment of a render pass: its format and samples, its load and
 * store operations (the hardware's numbers are Vulkan's) and the first
 * and last subpass that use it.
 */
struct ghci_vk_pass_attachment {
	uint32_t format;
	uint32_t samples;
	uint32_t load;
	uint32_t store;
	uint32_t stencil_load;
	uint32_t stencil_store;
	uint32_t first;
	uint32_t last;
};

/*
 * The attachments one subpass names, by their index in the render pass
 * (VK_ATTACHMENT_UNUSED for none): colour, resolve, depth and stencil,
 * and input.
 */
struct ghci_vk_pass_references {
	uint32_t color_count;
	uint32_t color[GHCI_VK_COLOR_ATTACHMENTS];
	uint32_t resolve[GHCI_VK_COLOR_ATTACHMENTS];
	uint32_t depth;
	uint32_t input_count;
	uint32_t input[GHCI_GSI1_INPUT_ATTACHMENTS];
};

/*
 * A VkRenderPass: what each subpass is to a pipeline, the attachments
 * each names, and the attachments.
 */
struct ghci_vk_render_pass {
	uint32_t subpass_count;
	struct ghci_vk_subpass *subpasses;
	struct ghci_vk_pass_references *references;
	uint32_t attachment_count;
	struct ghci_vk_pass_attachment *attachments;
};

/* A VkFramebuffer: its image views (by identity, in attachment order) and its size. */
struct ghci_vk_framebuffer {
	uint32_t attachment_count;
	uint64_t *views;
	uint32_t width;
	uint32_t height;
	uint32_t layers;
};

/*
 * A VkQueryPool: its type, its queries, and its slots in the device's
 * data heap (16 bytes each: the result, then the availability word that
 * END_QUERY or a fence after WRITE_TIMESTAMP writes), where the host
 * reads them.
 */
struct ghci_vk_query_pool {
	uint32_t type;
	uint32_t count;
	struct ghci_vk_heap_block *block;
	uint32_t offset;
	uint64_t va;
	uint64_t bus;
	uint8_t *view;
};

/*
 * A VkPipeline: its pipeline state record in the device's executor heap
 * (the 128 bytes, then the code of its programs, then the driver's table
 * of set 3) and a copy of the record, its scratch memory, the tables its
 * programs share, the push-constant bytes and dynamic offsets they read,
 * the vertex bindings' strides, and the state the record does not keep.
 * table_va is 0 for a pipeline without the driver's table.  layout,
 * render_pass and subpass are what it was made with, and dynamic_base
 * and dynamic_sets its layout's places of each set's dynamic offsets.
 * fan says that the topology is a triangle fan, which the record draws
 * as triangles from an index list the executor makes.  charged is what
 * the record and its runs take of the session's budget.
 */
struct ghci_vk_pipeline {
	uint32_t bind_point;
	struct ghci_vk_heap_block *block;
	uint32_t offset;
	uint64_t pso_va;
	uint64_t code_va[2];
	uint64_t table_va;
	struct ghci_vk_heap_block *scratch_block;
	uint32_t scratch_offset;
	uint8_t pso[GHCI_PSO_BYTES];
	struct ghci_gsi1_bindings bindings;
	uint32_t push_bytes;
	uint32_t dynamic_count;
	uint32_t dynamic_base[GHCI_VK_SETS];
	uint32_t dynamic_sets[GHCI_VK_SETS];
	uint32_t vertex_bindings;
	uint32_t strides[GHCI_VERTEX_BINDINGS];
	uint32_t fan;
	uint32_t primitive;
	uint32_t samples;
	uint64_t layout;
	uint64_t render_pass;
	uint32_t subpass;
	struct ghci_vk_static_state state;
	uint64_t charged;
};

/*
 * The hardware tables and constant-bank words of one draw or dispatch
 * (compiler/gsi1-abi.h): set 0's texture descriptors and set 1's
 * sampler descriptors by slot, set 2's resource table, and bank words
 * c0 to c(bank_count - 1) (c24-c31 are zero and never loaded).
 */
struct ghci_vk_tables {
	uint8_t textures[GHCI_GSI1_TEXTURE_SLOTS * GHCI_TEXTURE_DESC_BYTES];
	uint32_t texture_count;
	uint8_t samplers[GHCI_GSI1_SAMPLER_SLOTS * GHCI_SAMPLER_DESC_BYTES];
	uint32_t sampler_count;
	uint8_t resources[GSI1_ABI_RESOURCE_TABLE_BYTES];
	uint32_t resource_bytes;
	uint32_t bank[GHCI_VK_BANK_WORDS];
	uint32_t bank_count;
};

/* One run of the data heap a submission's temporary memory takes. */
struct ghci_vk_temp {
	struct ghci_vk_heap_block *block;
	uint32_t offset;
};

/*
 * A point where a submission's packets may be cut in pieces placed apart
 * (a dword between commands, outside a render pass): at, and the events
 * (count of them from first in the submission's waits) that must be set
 * before anything after it is placed; a plain cut has none.
 */
struct ghci_vk_cut {
	uint32_t at;
	uint32_t first;
	uint32_t count;
};

/*
 * A submission being made: the packets of its command buffers (between
 * the barriers at its head and its tail) and the runs of temporary
 * memory they read (the hardware tables, render pass descriptions,
 * clear colours, index lists and data), taken of the device's data heap
 * a chunk at a time (chunk_used and chunk_bytes are of the one being
 * filled, temp_bytes the bytes of every chunk), the events its packets write (and the dwords of each one's
 * first and last write), and its cuts in order of their dwords with the events
 * they wait for (waiting is the number of cuts with events).  The runs
 * go back once the submission's last ticket has passed, and the events
 * know the ticket of the piece that wrote them.
 */
struct ghci_vk_submission {
	uint32_t *words;
	uint32_t used;
	uint32_t capacity;
	struct ghci_vk_temp *temps;
	uint32_t temp_count;
	uint32_t temp_capacity;
	uint32_t chunk_used;
	uint32_t chunk_bytes;
	uint64_t temp_bytes;
	uint64_t events[GHCI_VK_SUBMIT_EVENTS];
	uint32_t event_first[GHCI_VK_SUBMIT_EVENTS];
	uint32_t event_last[GHCI_VK_SUBMIT_EVENTS];
	uint32_t event_count;
	struct ghci_vk_cut *cuts;
	uint32_t cut_count;
	uint32_t cut_capacity;
	uint64_t *waits;
	uint32_t wait_count;
	uint32_t wait_capacity;
	uint32_t waiting;
};

/*
 * A submission a device holds back until the events it waits for are
 * set (or until the earlier held submissions of the device are placed):
 * its packets, of which the first placed dwords are in the ring (in
 * pieces, each with its fence; ticket is the last piece's, when placed
 * says one was), the cut the next piece starts at or after, the VkFence
 * it signals (its identity, 0 for none), its serial among the device's
 * held submissions, and the bytes it takes of the session's budget while
 * it is held (its host memory and its temporary memory in the data heap).
 */
struct ghci_vk_hold {
	struct ghci_vk_hold *next;
	struct ghci_vk_submission submission;
	uint32_t placed;
	uint32_t cut;
	uint64_t fence;
	uint64_t serial;
	struct ghci_ticket ticket;
	int ticketed;
	uint64_t charged;
};

/*
 * One object of a node session, in the session's table from its create to
 * its destroy.  device is the VkDevice it was made on (a device's own
 * object points at its record).  charged is what it takes of the
 * session's budget, given back at its destroy.
 */
struct ghci_vk_object {
	struct ghci_vk_object *next;
	enum ghci_vk_kind kind;
	uint64_t identity;
	struct ghci_vk_device *device;
	uint64_t charged;
	union {
		struct ghci_vk_memory memory;
		struct ghci_vk_buffer buffer;
		struct ghci_vk_image image;
		struct ghci_vk_fence fence;
		struct ghci_vk_event event;
		struct ghci_vk_queue queue;
		struct ghci_vk_command_pool pool;
		struct ghci_vk_command_buffer commands;
		struct ghci_vk_sampler sampler;
		struct ghci_vk_image_view image_view;
		struct ghci_vk_buffer_view buffer_view;
		struct ghci_vk_shader_module module;
		struct ghci_vk_set_layout *set_layout;
		struct ghci_vk_pipeline_layout *pipeline_layout;
		struct ghci_vk_descriptor_pool pool_of_sets;
		struct ghci_vk_set *set;
		struct ghci_vk_pipeline *pipeline;
		struct ghci_vk_render_pass *render_pass;
		struct ghci_vk_framebuffer framebuffer;
		struct ghci_vk_query_pool query_pool;
	} as;
};

/*
 * A queued frame of a node session (irq_lock).  A frame of timeline 0
 * (the decoder's notification) completes once it has run; a context
 * fence of a timeline waits for the work of the timeline's device placed
 * until it ran, after the earlier frames of its timeline.  A frame with
 * nothing to wait for has ticket.seq 0; one behind held submissions has
 * a ticket of GHCI_VK_HELD_GENERATION and held, the device that holds
 * them; failed ends it with an error.
 */
struct ghci_vk_frame {
	struct ghci_vk_frame *next;
	struct drv_gpu_completion *completion;
	uint32_t timeline;
	struct ghci_ticket ticket;
	struct ghci_vk_device *held;
	int failed;
};

/*
 * The executor's side of one node session, from its open to its close.
 *
 * The object table, the arena, the budget the objects have taken
 * (budget_used, of GHCI_VK_BUDGET_BYTES) and the opcodes for the log
 * belong to whoever holds hold_lock: the session's commands, which take
 * it for each frame, and the service thread, which only tries it to
 * place the submissions the session's devices hold back.  Under
 * irq_lock: next (the executor's list), the devices, the device of each
 * timeline, the job markers (the timeline each was reserved on, and the
 * device whose held submissions a committed one waits for), the
 * queued frames, completing (the completions a retirement took and has
 * not yet reported to the core), the submissions held (hold_count, of
 * every device) and the service rounds that look at them (holding), the
 * ticket of the session's last work, and the marks the GPU core's stop
 * and isolation set.
 */
struct ghci_vk_session {
	struct ghci_device *device;
	struct ghci_session *owner;
	struct ghci_vk_object *buckets[GHCI_VK_BUCKETS];
	unsigned object_count;
	uint64_t budget_used;
	struct i915_wire_arena arena;
	uint32_t last_opcode;
	uint32_t failed_opcode;
	struct ghci_vk_session *next;
	struct ghci_vk_device *devices;
	struct ghci_vk_device *timeline_device[GHCI_VK_TIMELINES];
	struct ghci_job_table jobs;
	uint32_t marker_timeline[GHCI_JOB_MARKERS];
	struct ghci_vk_device *marker_device[GHCI_JOB_MARKERS];
	struct ghci_vk_frame *frames;
	unsigned frame_count;
	unsigned completing;
	struct mutex hold_lock;
	unsigned hold_count;
	unsigned holding;
	struct ghci_ticket last;
	int submitted;
	int stopping;
	int isolated;
};

/*
 * A handler of a group of commands: it runs the command when it owns the
 * opcode (and sets *handled), with the reader after the command's header
 * and the reply writer after the echoed opcode (written only when
 * requested).  Returns 0, or the error that ends the frame.
 */
typedef int (*ghci_vk_route)(struct ghci_vk_session *session, uint32_t opcode, uint32_t requested, struct i915_wire_reader *reader, struct i915_wire_writer *reply, int *handled);

/* vk-node.c: the executor's attach and the node's operations. */
int ghci_vk_attach(struct ghci_device *device);

/* vk-object.c: the session's side of the node, its objects and its arena. */
int ghci_vk_session_open(struct ghci_device *device, struct ghci_session *owner);
void ghci_vk_session_close(struct ghci_device *device, struct ghci_session *owner);
struct ghci_vk_object *ghci_vk_object_find(struct ghci_vk_session *session, enum ghci_vk_kind kind, uint64_t identity);
int ghci_vk_object_add(struct ghci_vk_session *session, enum ghci_vk_kind kind, uint64_t identity, struct ghci_vk_device *device, struct ghci_vk_object **result);
void ghci_vk_object_destroy(struct ghci_vk_session *session, struct ghci_vk_object *object);
void ghci_vk_objects_of_device(struct ghci_vk_session *session, struct ghci_vk_device *device);
void *ghci_vk_arena_take(struct ghci_vk_session *session, size_t bytes);
void *ghci_vk_decode_array(struct i915_wire_reader *reader, struct i915_wire_arena *arena, uint64_t count, size_t element);
uint64_t ghci_vk_decoded_count(const struct ghci_vk_session *session, const void *array);
int ghci_vk_budget_take(struct ghci_vk_session *session, uint64_t bytes);
void ghci_vk_budget_give(struct ghci_vk_session *session, uint64_t bytes);

/* vk-reply.c: the forms every command shares, the results and the float words. */
uint64_t ghci_vk_create_tail(struct i915_wire_reader *reader);
uint64_t ghci_vk_destroy_form(struct i915_wire_reader *reader, uint64_t *device);
uint64_t ghci_vk_read_count(struct i915_wire_reader *reader);
void ghci_vk_skip(struct i915_wire_reader *reader, uint64_t bytes);
void ghci_vk_reply_created(struct i915_wire_writer *reply, uint32_t requested, uint32_t result, uint64_t identity);
void ghci_vk_reply_result(struct i915_wire_writer *reply, uint32_t requested, uint32_t result);
uint32_t ghci_vk_result(int error);
uint32_t ghci_vk_float_ratio(uint64_t numerator, uint64_t denominator);
void ghci_vk_set_float(float *destination, uint32_t bits);

/* vk-dispatch.c and vk-stream.c: the command stream. */
int ghci_vk_dispatch(struct ghci_vk_session *session, uint32_t opcode, uint32_t requested, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
int ghci_vk_stream_execute(struct ghci_vk_session *session, const void *wire, uint32_t bytes);

/* vk-caps.c: the answers the capability registers give. */
void ghci_vk_caps_features(const struct ghci_caps *caps, VkPhysicalDeviceFeatures *features);
void ghci_vk_caps_limits(const struct ghci_caps *caps, VkPhysicalDeviceLimits *limits);
void ghci_vk_caps_memory(const struct ghci_caps *caps, uint64_t local_bytes, VkPhysicalDeviceMemoryProperties *memory);
void ghci_vk_caps_family(const struct ghci_caps *caps, VkQueueFamilyProperties *family);
uint32_t ghci_vk_caps_sample_counts(const struct ghci_caps *caps);
void ghci_vk_caps_format(const struct ghci_vk_formats *formats, uint32_t format, VkFormatProperties *properties);
uint32_t ghci_vk_caps_image_format(const struct ghci_caps *caps, const struct ghci_vk_formats *formats, uint32_t format, uint32_t type, uint32_t tiling, uint32_t usage, uint32_t flags, VkImageFormatProperties *properties);
int ghci_vk_caps_features_within(const struct ghci_caps *caps, const VkPhysicalDeviceFeatures *asked);
unsigned ghci_vk_caps_shortfalls(const struct ghci_caps *caps, const struct ghci_vk_formats *formats, const char **names, unsigned capacity);
int ghci_vk_caps_blitter(const struct ghci_caps *caps);

/* vk-format.c: the formats the hardware takes, and their features. */
int ghci_vk_format_size(uint32_t format, uint32_t *bytes, uint32_t *block);
void ghci_vk_formats_table(const struct ghci_caps *caps, struct ghci_vk_formats *formats);
void ghci_vk_formats_from_bits(const struct ghci_caps *caps, uint32_t format, uint32_t optimal, uint32_t linear, uint32_t buffer, struct ghci_vk_formats *formats);
void ghci_vk_formats_read(struct ghci_device *device, struct ghci_vk_formats *formats);
uint32_t ghci_vk_format_traits(uint32_t format, uint32_t *flags);

/* The routes of the command groups. */
int ghci_vk_device_route(struct ghci_vk_session *session, uint32_t opcode, uint32_t requested, struct i915_wire_reader *reader, struct i915_wire_writer *reply, int *handled);
int ghci_vk_memory_route(struct ghci_vk_session *session, uint32_t opcode, uint32_t requested, struct i915_wire_reader *reader, struct i915_wire_writer *reply, int *handled);
int ghci_vk_command_route(struct ghci_vk_session *session, uint32_t opcode, uint32_t requested, struct i915_wire_reader *reader, struct i915_wire_writer *reply, int *handled);
int ghci_vk_queue_route(struct ghci_vk_session *session, uint32_t opcode, uint32_t requested, struct i915_wire_reader *reader, struct i915_wire_writer *reply, int *handled);
int ghci_vk_layout_route(struct ghci_vk_session *session, uint32_t opcode, uint32_t requested, struct i915_wire_reader *reader, struct i915_wire_writer *reply, int *handled);
int ghci_vk_pipeline_route(struct ghci_vk_session *session, uint32_t opcode, uint32_t requested, struct i915_wire_reader *reader, struct i915_wire_writer *reply, int *handled);
int ghci_vk_query_route(struct ghci_vk_session *session, uint32_t opcode, uint32_t requested, struct i915_wire_reader *reader, struct i915_wire_writer *reply, int *handled);

/* vk-memory.c: memory, buffers, images and the executor heap. */
struct ghci_allocation *ghci_vk_memory_blob(struct ghci_device *device, struct ghci_session *owner, uint64_t blob_id);
int ghci_vk_heap_take(struct ghci_vk_session *session, struct ghci_vk_device *device, enum ghci_vk_heap heap, uint32_t bytes, uint32_t alignment, struct ghci_vk_heap_block **block, uint32_t *offset);
void ghci_vk_heap_give(struct ghci_vk_heap_block *block, uint32_t offset);
void ghci_vk_heap_retire(struct ghci_vk_device *device, struct ghci_vk_heap_block *block, uint32_t offset, const struct ghci_ticket *ticket);
void ghci_vk_heap_reclaim(struct ghci_vk_session *session, struct ghci_vk_device *device);
void ghci_vk_heap_fini(struct ghci_vk_session *session, struct ghci_vk_device *device);
void ghci_vk_memory_release(struct ghci_vk_session *session, struct ghci_vk_object *object);
void ghci_vk_image_layout(uint32_t width, uint32_t height, uint32_t depth, uint32_t levels, uint32_t layers, uint32_t samples, uint32_t texel_bytes, uint32_t block, struct ghci_layout_image *layout);

/* vk-layout.c: samplers, views, descriptor sets and their layouts, and the hardware tables of a draw. */
int ghci_vk_bound_va(struct ghci_vk_session *session, uint64_t memory, uint64_t offset, uint64_t *va);
void ghci_vk_layout_release(struct ghci_vk_session *session, struct ghci_vk_object *object);
void ghci_vk_set_layout_hold(struct ghci_vk_set_layout *layout);
void ghci_vk_set_layout_unhold(struct ghci_vk_set_layout *layout);
int ghci_vk_layout_tables(struct ghci_vk_session *session, const struct ghci_vk_pipeline *pipeline, const uint64_t *sets, const uint32_t *dynamic_offsets, uint32_t dynamic_count, const uint8_t *push, uint32_t push_bytes, struct ghci_vk_tables *tables);

/* vk-pass.c: render passes and framebuffers. */
int ghci_vk_pass_route(struct ghci_vk_session *session, uint32_t opcode, uint32_t requested, struct i915_wire_reader *reader, struct i915_wire_writer *reply, int *handled);
void ghci_vk_pass_release(struct ghci_vk_object *object);

/* vk-execute.c: the recorded commands of a submission made into packets. */
int ghci_vk_execute(struct ghci_vk_session *session, struct ghci_vk_device *device, const VkSubmitInfo *submits, uint64_t count, struct ghci_vk_submission *submission);
int ghci_vk_execute_clear(struct ghci_vk_session *session, struct ghci_vk_device *device, uint64_t va, uint64_t bytes, struct ghci_vk_submission *submission);
void ghci_vk_submission_end(struct ghci_vk_device *device, struct ghci_vk_submission *submission, const struct ghci_ticket *ticket, int placed);
void ghci_vk_submission_written(struct ghci_vk_device *device, const struct ghci_vk_submission *submission, uint32_t end, const struct ghci_ticket *ticket);

/* vk-meta.c: the driver's own transfer programs. */
int ghci_vk_meta_compile(const struct ghci_caps *caps, enum ghci_vk_meta_kind kind, struct ghci_gsi1_program **programs);
int ghci_vk_meta_pipeline(struct ghci_vk_session *session, struct ghci_vk_device *device, enum ghci_vk_meta_kind kind, const struct ghci_vk_meta_pipeline **pipeline);
int ghci_vk_meta_copy(const struct ghci_vk_meta_copy *copy, struct ghci_vk_meta_dispatch *dispatch);
void ghci_vk_meta_table(const struct ghci_vk_meta_pipeline *pipeline, const struct ghci_vk_meta_dispatch *dispatch, uint8_t *view);
uint32_t ghci_vk_meta_bind(const struct ghci_vk_meta_pipeline *pipeline, uint64_t table, uint32_t *words);
uint32_t ghci_vk_meta_step(const struct ghci_vk_meta_dispatch *dispatch, uint64_t *first, uint32_t *words);
int ghci_vk_copy_rows(struct ghci_device *device, const struct ghci_rows *rows, struct ghci_ticket *ticket);
void ghci_vk_rows_release(struct ghci_device *device);
int ghci_vk_meta_query(uint64_t source, uint32_t count, uint64_t destination, uint32_t stride, uint32_t flags, struct ghci_vk_meta_dispatch *dispatch);

/* vk-pipeline.c: what a shader module or a pipeline lets go of. */
void ghci_vk_pipeline_release(struct ghci_vk_session *session, struct ghci_vk_object *object);

/* vk-command.c: what a command buffer lets go of. */
void ghci_vk_commands_release(struct ghci_vk_object *object);

/* vk-queue.c: the completions, the drain and the stop. */
void ghci_vk_service(struct ghci_device *device);
void ghci_vk_drain(struct ghci_device *device, struct ghci_vk_session *session);
void ghci_vk_settle(struct ghci_device *device, struct ghci_vk_session *session);
int ghci_vk_frame_new(struct ghci_vk_session *session, uint32_t timeline, struct drv_gpu_completion *completion, struct ghci_vk_frame **result);
int ghci_vk_frame_queue(struct ghci_vk_session *session, struct ghci_vk_frame *frame);
void ghci_vk_submitted(struct ghci_vk_device *device, const struct ghci_ticket *ticket);
int ghci_vk_session_pending(struct ghci_vk_session *session);
void ghci_vk_session_fail(struct ghci_vk_session *session);
int ghci_vk_holds_advance(struct ghci_vk_session *session, struct ghci_vk_device *device, int wait);
void ghci_vk_holds_fail(struct ghci_vk_session *session, struct ghci_vk_device *device);
int ghci_vk_held_ticket(struct ghci_vk_device *device, struct ghci_ticket *ticket);

#endif
