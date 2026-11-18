/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The Mali render node's Vulkan executor.
 *
 * libvulkan sends the zedBSD GPU command protocol (uapi/gpu-op.h) to the
 * render node; the executor decodes it in the kernel.  The wire's
 * readers and writers and the generated record codec are the i915
 * executor's, used unchanged.  Objects are found by their wire identity
 * and kind; a command buffer keeps its commands as records, and a queue
 * submission turns the records into one command stream in the session's
 * stage memory (vk-queue.c), which the node's ring calls and waits for.
 *
 * One executor session belongs to one open of the node; the core admits
 * one of its ioctls at a time, which serializes everything here.
 */

#ifndef KERN_DRIVERS_GPU_MALI_VK_H
#define KERN_DRIVERS_GPU_MALI_VK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "drivers/gpu/i915/vk/vk-codec.h"
#include "drivers/gpu/mali/compiler/shader.h"
#include "drivers/gpu/mali/render.h"
#include "drivers/gpu/mali/vk/vk-surface.h"

/* The scratch one command decodes its records into, and the most live objects of a session. */
#define MALI_VK_ARENA_BYTES		(128U * 1024U)
#define MALI_VK_OBJECT_LIMIT		4096U
#define MALI_VK_OBJECT_BUCKETS		256U

/*
 * The memory types: device local (0) and host visible and coherent (1),
 * both the same uncached system memory of one heap.
 */
#define MALI_VK_MEMORY_DEVICE		0U
#define MALI_VK_MEMORY_HOST		1U
#define MALI_VK_MEMORY_TYPES		2U

/* Where a buffer starts in its memory, an image's alignment and its rows' alignment. */
#define MALI_VK_BUFFER_ALIGN		256U
#define MALI_VK_IMAGE_ALIGN		4096U
#define MALI_VK_ROW_ALIGN		64U

/* The limits of the layouts and of an image (its layers too). */
#define MALI_VK_IMAGE_LAYERS		256U
#define MALI_VK_BINDINGS_MAX		16U
#define MALI_VK_SETS_MAX		4U
#define MALI_VK_PUSH_BYTES		128U
#define MALI_VK_IMAGE_EDGE		8192U
#define MALI_VK_ATTACHMENTS_MAX		8U
#define MALI_VK_VERTEX_BINDINGS_MAX	16U
#define MALI_VK_VERTEX_ATTRIBUTES_MAX	16U

/* A command buffer's states. */
#define MALI_VK_COMMAND_INITIAL		0U
#define MALI_VK_COMMAND_RECORDING	1U
#define MALI_VK_COMMAND_EXECUTABLE	2U
#define MALI_VK_COMMAND_INVALID		3U

/*
 * One format the executor takes: its Vulkan number, its bytes per texel,
 * the hardware's pixel format word (the format in bits 12..19, sRGB in
 * bit 20, the component order below), the colour buffer's internal
 * format and its writeback format and component swizzle (zero for a
 * format that is not rendered to), and the vertex attribute format;
 * whether a storage image may have the format (storage) and a storage
 * image's texels change atomically (atomic), and whether it is a depth
 * format (sampled and compared; a depth and stencil attachment when
 * mali_surface_zs_find() knows it).
 */
struct mali_vk_format {
	uint32_t vk;
	uint32_t bytes;
	uint32_t pixel;
	uint32_t internal;
	uint32_t writeback;
	uint32_t swizzle;
	bool render;
	bool sample;
	bool vertex;
	bool storage;
	bool atomic;
	bool depth;
};

struct mali_vk_object;

/*
 * One executor session.
 *
 * objects lists every published identity, buckets holds the same ones by
 * their hash.  timelines has bit n set once a queue was bound to
 * timeline n.  closing is set once the close has begun.  tiler is the
 * memory the render passes' tiler uses (its heap chunk, its geometry
 * buffer and the sample positions), made by the first pass.
 */
struct mali_vk_session {
	struct mali_session *owner;
	struct mali_vk_object *objects;
	struct mali_vk_object *buckets[MALI_VK_OBJECT_BUCKETS];
	uint32_t live_objects;
	bool closing;
	uint32_t last_opcode;
	uint32_t failed_opcode;
	struct i915_wire_arena arena;
	uint64_t timelines;
	struct mali_bo *tiler;
	struct mali_resource *tiler_resource;
};

/*
 * One published wire identity and the payload it stands for.
 *
 * references counts the holders: the registry while published and every
 * dependent object that retained it; the last release destroys the
 * payload.
 */
struct mali_vk_object {
	struct mali_vk_object *next;
	struct mali_vk_object *bucket_next;
	struct mali_vk_session *session;
	enum i915_vk_object_kind kind;
	uint64_t identity;
	uint32_t references;
	bool published;
	void *payload;
	int (*destroy)(struct mali_vk_session *session, void *payload);
};

/* A VkDeviceMemory: its bytes and type, and the buffer object and the session's handle on it. */
struct mali_vk_memory {
	uint64_t bytes;
	uint32_t type;
	struct mali_bo *bo;
	struct mali_resource *resource;
};

/* A VkBuffer: its size and usage, and once bound its memory and offset. */
struct mali_vk_buffer {
	uint64_t size;
	uint32_t usage;
	struct mali_vk_object *memory;
	uint64_t offset;
};

/*
 * A VkImage: its format, extent, levels and layers, its linear layout
 * (level 0's row pitch, the bytes of one layer with all its levels --
 * the array stride --, the bytes of all, and each level's offset in a
 * layer, row pitch, extent and bytes), its usage, and once bound its
 * memory and offset.
 */
struct mali_vk_image {
	const struct mali_vk_format *format;
	uint32_t width;
	uint32_t height;
	uint32_t layers;
	uint32_t levels;
	uint32_t usage;
	uint64_t row_pitch;
	uint64_t layer_bytes;
	uint64_t bytes;
	struct mali_surface_level level[MALI_SURFACE_LEVELS];
	struct mali_vk_object *memory;
	uint64_t offset;
};

/*
 * A VkImageView: the image, its format and the texture descriptor and
 * plane it reads as; its type (VkImageViewType), its layers from
 * base_layer (six a cube) and its levels from base_level.
 */
struct mali_vk_image_view {
	struct mali_vk_object *image;
	const struct mali_vk_format *format;
	uint32_t base_layer;
	uint32_t swizzle;
	uint32_t view_type;
	uint32_t layer_count;
	uint32_t base_level;
	uint32_t level_count;
};

/* A VkSampler: the sampler descriptor's eight words. */
struct mali_vk_sampler {
	uint32_t words[8];
};

/* A VkShaderModule: its SPIR-V words, after the structure in the same allocation. */
struct mali_vk_shader {
	uint32_t word_count;
	uint32_t *words;
};

/* One binding of a set layout: its number, its type and its count of descriptors. */
struct mali_vk_binding {
	uint32_t binding;
	uint32_t type;
	uint32_t count;
};

/* A VkDescriptorSetLayout: its bindings. */
struct mali_vk_set_layout {
	uint32_t count;
	struct mali_vk_binding bindings[MALI_VK_BINDINGS_MAX];
};

/* A VkPipelineLayout: only its set count is kept. */
struct mali_vk_pipeline_layout {
	uint32_t set_count;
};

/* A VkDescriptorPool: the most sets it hands out. */
struct mali_vk_descriptor_pool {
	uint32_t max_sets;
};

/*
 * One descriptor of a set: a buffer's range, or an image view with its
 * sampler, each held.
 */
struct mali_vk_descriptor {
	uint32_t type;
	struct mali_vk_object *buffer;
	uint64_t offset;
	uint64_t range;
	struct mali_vk_object *view;
	struct mali_vk_object *sampler;
};

/* A VkDescriptorSet: its layout's bindings and one descriptor per binding (element 0). */
struct mali_vk_descriptor_set {
	const void *pool;
	struct mali_vk_set_layout layout;
	struct mali_vk_descriptor slots[MALI_VK_BINDINGS_MAX];
};

/* A graphics pipeline's fixed state the draw descriptors take. */
struct mali_vk_graphics {
	struct mali_shader_program *vertex;
	struct mali_shader_program *fragment;
	uint32_t topology;
	uint32_t binding_count;
	uint32_t binding_stride[MALI_VK_VERTEX_BINDINGS_MAX];
	uint32_t binding_rate[MALI_VK_VERTEX_BINDINGS_MAX];
	uint32_t attribute_count;
	uint32_t attribute_location[MALI_VK_VERTEX_ATTRIBUTES_MAX];
	uint32_t attribute_binding[MALI_VK_VERTEX_ATTRIBUTES_MAX];
	uint32_t attribute_offset[MALI_VK_VERTEX_ATTRIBUTES_MAX];
	const struct mali_vk_format *attribute_format[MALI_VK_VERTEX_ATTRIBUTES_MAX];
	uint32_t cull_mode;
	uint32_t front_face;
	uint32_t blend_count;
	bool blend_enable[MALI_VK_ATTACHMENTS_MAX];
	uint32_t blend_equation[MALI_VK_ATTACHMENTS_MAX];
	uint32_t write_mask[MALI_VK_ATTACHMENTS_MAX];
	bool viewport_static;
	VkViewport viewport;
	bool scissor_static;
	VkRect2D scissor;

	/*
	 * The depth and stencil state (the tests, the faces and the bias;
	 * the pass's aspects and the program's writes are the draw's), and
	 * which of it the commands set instead (VK_DYNAMIC_STATE_*: the
	 * bias, the compare and write masks and the reference).
	 */
	struct mali_surface_zs_state zs;
	bool dynamic_bias;
	bool dynamic_compare_mask;
	bool dynamic_write_mask;
	bool dynamic_reference;
};

/*
 * A VkPipeline: compute or graphics, its compiled programs, and the
 * shader code in a buffer object of the session, mapped.
 */
struct mali_vk_pipeline {
	bool compute;
	struct mali_shader_program *program;
	struct mali_vk_graphics *graphics;
	struct mali_bo *code;
	struct mali_resource *code_resource;
	uint64_t code_address;
	uint64_t vertex_address;
	uint64_t varying_address;
	uint64_t fragment_address;
};

/*
 * A VkRenderPass of one subpass: its attachments' formats, load and
 * store ops (the stencil's too), the subpass's colour references, and
 * its depth and stencil attachment (VK_ATTACHMENT_UNUSED for none) with
 * the tile buffer's format of it.
 */
struct mali_vk_render_pass {
	uint32_t attachment_count;
	const struct mali_vk_format *format[MALI_VK_ATTACHMENTS_MAX];
	uint32_t load_op[MALI_VK_ATTACHMENTS_MAX];
	uint32_t store_op[MALI_VK_ATTACHMENTS_MAX];
	uint32_t stencil_load_op[MALI_VK_ATTACHMENTS_MAX];
	uint32_t stencil_store_op[MALI_VK_ATTACHMENTS_MAX];
	uint32_t color_count;
	uint32_t color[MALI_VK_ATTACHMENTS_MAX];
	uint32_t depth;
	const struct mali_surface_zs_format *zs_format;
};

/* A VkFramebuffer: its views, held, and its extent. */
struct mali_vk_framebuffer {
	uint32_t count;
	struct mali_vk_object *views[MALI_VK_ATTACHMENTS_MAX];
	uint32_t width;
	uint32_t height;
};

/* A VkCommandPool: its queue family. */
struct mali_vk_command_pool {
	uint32_t family;
};

/*
 * One recorded command.  The objects it names are held (objects); its
 * scalar arguments are in values, its wider ones in data (owned).
 */
#define MALI_VK_RECORD_OBJECTS		8U
struct mali_vk_record {
	uint32_t opcode;
	struct mali_vk_object *objects[MALI_VK_RECORD_OBJECTS];
	uint32_t object_count;
	uint32_t values[8];
	uint64_t wide[MALI_VK_RECORD_OBJECTS];
	void *data;
	uint32_t data_bytes;
};

/* A VkCommandBuffer: its pool (only compared), its state, its records and its first error. */
struct mali_vk_command_buffer {
	const void *pool;
	uint32_t state;
	struct mali_vk_record *records;
	uint32_t record_count;
	uint32_t record_capacity;
	int error;
};

/* A VkFence: whether it is signaled. */
struct mali_vk_fence {
	bool signaled;
};

/* A command's handler: sets *handled when the opcode is its own. */
typedef int (*mali_vk_route)(struct mali_vk_session *session, uint32_t opcode, struct i915_wire_reader *reader, struct i915_wire_writer *reply, int *handled);

/* The registry and the shared parts of creates and destroys (vk-object.c). */
int mali_vk_object_publish(struct mali_vk_session *session, enum i915_vk_object_kind kind, uint64_t identity, void *payload, int (*destroy)(struct mali_vk_session *, void *));
struct mali_vk_object *mali_vk_object_find(struct mali_vk_session *session, enum i915_vk_object_kind kind, uint64_t identity);
int mali_vk_object_retain(struct mali_vk_object *object);
void mali_vk_object_release(struct mali_vk_object *object);
int mali_vk_object_remove(struct mali_vk_session *session, enum i915_vk_object_kind kind, uint64_t identity);
void *mali_vk_payload(struct mali_vk_session *session, enum i915_vk_object_kind kind, uint64_t identity);
void *mali_vk_arena_take(struct mali_vk_session *session, size_t bytes);
uint32_t mali_vk_result(int error);
uint64_t mali_vk_create_tail(struct i915_wire_reader *reader);
void mali_vk_create_reply(struct mali_vk_session *session, struct i915_wire_writer *reply, enum i915_vk_object_kind kind, uint64_t identity, void *payload, int (*destroy)(struct mali_vk_session *, void *), int error);
int mali_vk_destroy_plain(struct mali_vk_session *session, struct i915_wire_reader *reader, enum i915_vk_object_kind kind);
int mali_vk_free_payload(struct mali_vk_session *session, void *payload);
uint64_t mali_vk_handle(const void *handle);

/* The handlers of each group of commands. */
int mali_vk_device_route(struct mali_vk_session *session, uint32_t opcode, struct i915_wire_reader *reader, struct i915_wire_writer *reply, int *handled);
int mali_vk_memory_route(struct mali_vk_session *session, uint32_t opcode, struct i915_wire_reader *reader, struct i915_wire_writer *reply, int *handled);
int mali_vk_layout_route(struct mali_vk_session *session, uint32_t opcode, struct i915_wire_reader *reader, struct i915_wire_writer *reply, int *handled);
int mali_vk_pipeline_route(struct mali_vk_session *session, uint32_t opcode, struct i915_wire_reader *reader, struct i915_wire_writer *reply, int *handled);
int mali_vk_command_route(struct mali_vk_session *session, uint32_t opcode, struct i915_wire_reader *reader, struct i915_wire_writer *reply, int *handled);
int mali_vk_queue_route(struct mali_vk_session *session, uint32_t opcode, struct i915_wire_reader *reader, struct i915_wire_writer *reply, int *handled);

/* The formats (vk-format.c). */
const struct mali_vk_format *mali_vk_format_find(uint32_t format);
uint32_t mali_vk_format_features(const struct mali_vk_format *format, bool linear);

/* The GPU address of a bound buffer or image, zero when not bound (vk-memory.c). */
uint64_t mali_vk_buffer_address(const struct mali_vk_buffer *buffer);
uint64_t mali_vk_image_address(const struct mali_vk_image *image);

/* A bound image's level of a layer: its GPU address (zero when not bound or outside the image) and its row pitch (zero outside). */
uint64_t mali_vk_image_level_address(const struct mali_vk_image *image, uint32_t level, uint32_t layer);
uint32_t mali_vk_image_level_pitch(const struct mali_vk_image *image, uint32_t level);

/* A command buffer emptied of its records (vk-command.c). */
void mali_vk_command_reset(struct mali_vk_command_buffer *command);

/* The draw records and the render passes, built into a command stream (vk-draw.c). */
struct mali_vk_build;
int mali_vk_draw_pass(struct mali_vk_build *build, const struct mali_vk_command_buffer *command, uint32_t *index);

#endif
