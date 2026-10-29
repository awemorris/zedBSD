/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The executor's objects: what a published identity of each kind stands for,
 * and the handlers that make them.
 *
 * Every image is pitch linear, four bytes a pixel: the display, the
 * texture header and the colour target all read that layout, so nothing
 * is converted between them.  A buffer or an image names the memory it is
 * bound to by holding that memory's object; the memory's storage arrives
 * with the blob libvulkan creates for it (vk-memory.c).
 */

#ifndef DRIVERS_GPU_NVRTX_VK_OBJECTS_H
#define DRIVERS_GPU_NVRTX_VK_OBJECTS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "drivers/gpu/nvrtx/vk/nvrtx-exec.h"
#include "drivers/gpu/nvrtx/vk/nvrtx-vk.h"

/*
 * An image's rows are this many bytes apart at least (a multiple of it),
 * and an image or a buffer starts on this boundary of its memory: the
 * display's offset (256) and stride (64), the texture header's pitch (32)
 * and the colour target's pitch (128) all divide it.
 */
#define NVRTX_VK_PITCH_ALIGN		256U
#define NVRTX_VK_RESOURCE_ALIGN		256U

/* The bytes of one pixel of every image format the executor takes. */
#define NVRTX_VK_PIXEL_BYTES		4U

/* The widest and tallest image (the colour target's and the texture header's limit). */
#define NVRTX_VK_IMAGE_SIDE_MAX		16384U

/* The memory type the executor reports (one: device local, host visible and coherent). */
#define NVRTX_VK_MEMORY_TYPE		0U

/*
 * A VkDeviceMemory.
 *
 * bytes is the size libvulkan asked for (whole pages).  Once the blob for
 * it is attached, cpu is the CPU's view of its first byte (through the
 * BAR1 window), va the GPU's address of it in the work channel's space,
 * and backing the node's record the memory holds a reference to.
 */
struct nvrtx_vk_memory {
	uint64_t bytes;
	uint8_t *cpu;
	uint64_t va;
	void *backing;
	bool attached;
};

/*
 * A VkBuffer: its size and usage, and once bound the memory object it
 * holds and its offset in that memory.
 */
struct nvrtx_vk_buffer {
	uint64_t size;
	uint32_t usage;
	struct nvrtx_vk_object *memory;
	uint64_t offset;
};

/* The levels an image has at most (a side of NVRTX_VK_IMAGE_SIDE_MAX halved to one). */
#define NVRTX_VK_LEVELS_MAX		15U

/*
 * A depth image is block linear in blocks of one GOB: its rows are a whole
 * number of GOBs wide and its height a whole number of GOBs tall, and it
 * starts on a GOB.
 *
 * Value referenced: Mesa 25.0.7 src/nouveau/nil/tiling.rs (the Fermi GOB of
 * 64 bytes by 8 rows, 512 bytes).
 */
#define NVRTX_VK_GOB_WIDTH		64U
#define NVRTX_VK_GOB_HEIGHT		8U
#define NVRTX_VK_GOB_BYTES		512U

/*
 * A depth image with a stencil takes whole pages of its memory: its pages
 * are of the S8Z24 memory kind while it is a depth target, which no other
 * resource's bytes may share.
 */
#define NVRTX_VK_KIND_ALIGN		4096U

/*
 * The two-plane 4:2:0 format of the video decoder's pictures, and the
 * boundary a video image starts on in its memory (whole blocks of its
 * block-linear planes).
 */
#define NVRTX_VK_FORMAT_NV12		VK_FORMAT_G8_B8R8_2PLANE_420_UNORM
#define NVRTX_VK_VIDEO_ALIGN		4096U

/*
 * A VkImage: a 2D image of levels levels, one layer and one sample, bytes
 * long; level n starts level_offset[n] bytes in, its rows level_pitch[n]
 * bytes apart (level 0's is pitch).  A colour image of one level is pitch
 * linear; one of more levels is tiled: block linear in one-GOB blocks, each
 * level a block-linear surface of its own after the one before (the
 * texture header's mipmapped layout for blocks one GOB high).  A depth
 * image (D32_SFLOAT, or D24_UNORM_S8_UINT with a stencil, four bytes a
 * pixel either way, one level) is block linear in one-GOB blocks too; one
 * with a stencil takes whole pages, which are of the S8Z24 kind once kinded
 * is set (its first use as a depth target).
 * A video image (NV12, one level) has two block-linear planes in blocks
 * two GOBs high, each pitch bytes a row, plane n at plane_offset[n] and
 * plane_rows[n] rows tall: the luma bytes, then the chroma pairs at half
 * the height; only the video decoder writes it and only the copy engine
 * reads it out.  Once bound, the memory object it holds and its offset in
 * that memory.
 */
struct nvrtx_vk_image {
	uint32_t format;
	uint32_t width;
	uint32_t height;
	uint32_t usage;
	uint32_t pitch;
	uint64_t bytes;
	uint32_t levels;
	bool depth;
	bool stencil;
	bool kinded;
	bool tiled;
	bool video;
	uint64_t plane_offset[2];
	uint32_t plane_rows[2];
	uint64_t level_offset[NVRTX_VK_LEVELS_MAX];
	uint32_t level_pitch[NVRTX_VK_LEVELS_MAX];
	struct nvrtx_vk_object *memory;
	uint64_t offset;
};

/*
 * A VkImageView: the image object it holds, the format it reads the image
 * as, the first level it sees and how many, where each of red, green,
 * blue and alpha comes from (VkComponentSwizzle, identity resolved), and
 * for a view of one plane of a video image the plane (1 the luma plane as
 * R8, 2 the chroma plane as R8G8; 0 any other view).
 */
struct nvrtx_vk_view {
	struct nvrtx_vk_object *image;
	uint32_t format;
	uint32_t base_level;
	uint32_t level_count;
	uint32_t swizzle[4];
	uint32_t plane;
};

/*
 * A VkSampler: the filters, the levels' range (minLod and maxLod as float
 * bits) and the address modes a texture read uses, and whether coordinates
 * are in pixels.
 */
struct nvrtx_vk_sampler {
	uint32_t mag_filter;
	uint32_t min_filter;
	uint32_t mipmap_mode;
	uint32_t min_lod;
	uint32_t max_lod;
	uint32_t address_u;
	uint32_t address_v;
	uint32_t address_w;
	uint32_t border_color;
	uint32_t unnormalized;
};

/* The binding numbers a descriptor set layout may use, and the sets a pipeline layout holds. */
#define NVRTX_VK_BINDINGS_MAX		16U
#define NVRTX_VK_SETS_MAX		4U

/* The vertex buffers and attributes a pipeline reads. */
#define NVRTX_VK_VERTEX_BINDINGS_MAX	4U
#define NVRTX_VK_VERTEX_ATTRIBUTES_MAX	8U

/* The sampled images one program reads (each a texture handle in constant buffer 0). */
#define NVRTX_VK_TEXTURES_MAX		8U

/* The push constant bytes a pipeline takes (constant buffer 0 from 0). */
#define NVRTX_VK_PUSH_BYTES		256U

/*
 * A VkShaderModule: a copy of its SPIR-V words, allocated with it.
 */
struct nvrtx_vk_shader {
	uint32_t word_count;
	uint32_t *words;
};

/* One binding of a descriptor set layout: its number, descriptor type and count. */
struct nvrtx_vk_layout_binding {
	uint32_t binding;
	uint32_t type;
	uint32_t count;
};

/*
 * A VkDescriptorSetLayout: its bindings.  Combined image samplers and
 * uniform buffers are bound to something.
 */
struct nvrtx_vk_set_layout {
	uint32_t count;
	struct nvrtx_vk_layout_binding bindings[NVRTX_VK_BINDINGS_MAX];
};

/* A VkPipelineLayout: how many sets it binds and how many push constant bytes it declares. */
struct nvrtx_vk_pipeline_layout {
	uint32_t set_count;
	uint32_t push_bytes;
};

/* A VkDescriptorPool: the most sets it hands out (nothing else is bounded). */
struct nvrtx_vk_descriptor_pool {
	uint32_t max_sets;
};

/*
 * One binding of a descriptor set: the image view and the sampler a
 * combined image sampler names, or the buffer, offset and range a uniform
 * buffer names, each object held while the set names it.
 */
struct nvrtx_vk_descriptor_slot {
	struct nvrtx_vk_object *view;
	struct nvrtx_vk_object *sampler;
	struct nvrtx_vk_object *buffer;
	uint64_t offset;
	uint64_t range;
};

/*
 * A VkDescriptorSet: the pool it came from (only compared, to free the
 * pool's sets with it) and its bindings by number.
 */
struct nvrtx_vk_descriptor_set {
	const void *pool;
	struct nvrtx_vk_descriptor_slot slots[NVRTX_VK_BINDINGS_MAX];
};

/* The attachments a render pass and a framebuffer have at most (one colour, one depth). */
#define NVRTX_VK_ATTACHMENTS_MAX	2U

/*
 * A VkRenderPass of one subpass writing at most one colour attachment and
 * at most one depth attachment: whether the subpass writes each, its
 * number among the pass's attachments, its format and its load operation
 * (the depth attachment's stencil load operation too).
 */
struct nvrtx_vk_render_pass {
	uint32_t colour;
	uint32_t colour_attachment;
	uint32_t format;
	uint32_t load_op;
	uint32_t depth;
	uint32_t depth_attachment;
	uint32_t depth_load_op;
	uint32_t stencil_load_op;
};

/* A VkFramebuffer: its size and the views of its attachments in order, held. */
struct nvrtx_vk_framebuffer {
	uint32_t width;
	uint32_t height;
	uint32_t view_count;
	struct nvrtx_vk_object *views[NVRTX_VK_ATTACHMENTS_MAX];
};

/* Where a program's sampled image or uniform buffer comes from: a descriptor set and a binding. */
struct nvrtx_vk_texture_source {
	uint32_t set;
	uint32_t binding;
};

/* The uniform buffers one program reads (constant buffers 1 on). */
#define NVRTX_VK_UNIFORMS_MAX		7U

/*
 * One compiled program of a pipeline: its address (its header, the code
 * after it), its registers, the descriptors its texture handles come
 * from, in the order of their slots in constant buffer 0, and the
 * descriptors of its uniform buffers, in the order of their constant
 * buffers from 1.
 */
struct nvrtx_vk_program {
	uint64_t va;
	uint32_t gprs;
	uint32_t texture_count;
	struct nvrtx_vk_texture_source textures[NVRTX_VK_TEXTURES_MAX];
	uint32_t uniform_count;
	struct nvrtx_vk_texture_source uniforms[NVRTX_VK_UNIFORMS_MAX];
};

/* A vertex buffer binding of a pipeline: its number, stride and whether it steps per instance. */
struct nvrtx_vk_vertex_binding {
	uint32_t binding;
	uint32_t stride;
	uint32_t instance;
};

/* A vertex attribute of a pipeline: its location, binding, format and offset. */
struct nvrtx_vk_vertex_attribute {
	uint32_t location;
	uint32_t binding;
	uint32_t format;
	uint32_t offset;
};

/*
 * A graphics VkPipeline: its two programs in a run of the executor's GPU
 * memory (code_token names the run to the node), the vertex input, the
 * topology, the culling, the depth and stencil tests, the blend of the
 * colour attachment, and the viewport and scissor unless they are dynamic
 * (floats as their bits).
 */
struct nvrtx_vk_pipeline {
	void *code_token;
	struct nvrtx_vk_program vertex;
	struct nvrtx_vk_program fragment;
	uint32_t push_bytes;
	uint32_t binding_count;
	struct nvrtx_vk_vertex_binding bindings[NVRTX_VK_VERTEX_BINDINGS_MAX];
	uint32_t attribute_count;
	struct nvrtx_vk_vertex_attribute attributes[NVRTX_VK_VERTEX_ATTRIBUTES_MAX];
	uint32_t topology;
	uint32_t cull_mode;
	uint32_t front_face;
	uint32_t depth_test;
	uint32_t depth_write;
	uint32_t depth_compare;
	uint32_t stencil_test;
	struct nvrtx_exec_stencil_face stencil_front;
	struct nvrtx_exec_stencil_face stencil_back;
	uint32_t blend_enable;
	uint32_t blend_src_colour;
	uint32_t blend_dst_colour;
	uint32_t blend_colour_op;
	uint32_t blend_src_alpha;
	uint32_t blend_dst_alpha;
	uint32_t blend_alpha_op;
	uint32_t write_mask;
	uint32_t dynamic_viewport;
	uint32_t dynamic_scissor;
	uint32_t viewport[6];
	VkRect2D scissor;
};

/* The regions one copy, blit or clear records. */
#define NVRTX_VK_REGIONS_MAX		16U

/* A command buffer's states. */
#define NVRTX_VK_COMMAND_INITIAL	0U
#define NVRTX_VK_COMMAND_RECORDING	1U
#define NVRTX_VK_COMMAND_EXECUTABLE	2U
#define NVRTX_VK_COMMAND_INVALID	3U

/*
 * A VkFence: whether it is signaled, and whether a submission named it and
 * the session's newest run then, which signals it once it has ended.
 */
struct nvrtx_vk_fence {
	bool signaled;
	bool submitted;
	struct nvrtx_vk_ticket run;
};

/* A VkCommandPool: only compared, to free and reset its command buffers with it. */
struct nvrtx_vk_command_pool {
	uint32_t family;
};

/*
 * One recorded command: its opcode, the objects it names (each held while
 * the record lives), and its values.  regions points at its own run of
 * VkImageCopy, VkImageBlit or VkBufferImageCopy records; a push constant
 * record's count bytes are at push_at in its command buffer's push bytes.
 */
struct nvrtx_vk_record {
	uint32_t opcode;
	struct nvrtx_vk_object *objects[NVRTX_VK_SETS_MAX];
	uint32_t object_count;
	uint64_t offsets[NVRTX_VK_VERTEX_BINDINGS_MAX];
	uint32_t first;
	uint32_t count;
	uint32_t values[6];
	VkRect2D rect;
	void *regions;
	uint32_t region_count;
	uint32_t push_at;
};

/*
 * A VkCommandBuffer: the pool it came from (only compared), its state, its
 * records in order (a growing array), the bytes of its push constant
 * records one after another (a growing run, push_used of push_capacity in
 * use) and the first error its recording met, which its End reports.  Both
 * runs stay for the next recording.
 */
struct nvrtx_vk_command_buffer {
	const void *pool;
	uint32_t state;
	struct nvrtx_vk_record *records;
	uint32_t record_count;
	uint32_t record_capacity;
	uint8_t *push_bytes;
	uint32_t push_used;
	uint32_t push_capacity;
	int error;
};

int nvrtx_vk_memory_route(struct nvrtx_vk_session *session, uint32_t opcode, uint32_t requested, struct i915_wire_reader *reader, struct i915_wire_writer *reply, int *handled);
int nvrtx_vk_resource_route(struct nvrtx_vk_session *session, uint32_t opcode, uint32_t requested, struct i915_wire_reader *reader, struct i915_wire_writer *reply, int *handled);
int nvrtx_vk_layout_route(struct nvrtx_vk_session *session, uint32_t opcode, uint32_t requested, struct i915_wire_reader *reader, struct i915_wire_writer *reply, int *handled);
int nvrtx_vk_target_route(struct nvrtx_vk_session *session, uint32_t opcode, uint32_t requested, struct i915_wire_reader *reader, struct i915_wire_writer *reply, int *handled);
int nvrtx_vk_pipeline_route(struct nvrtx_vk_session *session, uint32_t opcode, uint32_t requested, struct i915_wire_reader *reader, struct i915_wire_writer *reply, int *handled);
int nvrtx_vk_command_route(struct nvrtx_vk_session *session, uint32_t opcode, uint32_t requested, struct i915_wire_reader *reader, struct i915_wire_writer *reply, int *handled);
int nvrtx_vk_record_route(struct nvrtx_vk_session *session, uint32_t opcode, uint32_t requested, struct i915_wire_reader *reader, struct i915_wire_writer *reply, int *handled);
void nvrtx_vk_command_reset(struct nvrtx_vk_command_buffer *command);
int nvrtx_vk_queue_route(struct nvrtx_vk_session *session, uint32_t opcode, uint32_t requested, struct i915_wire_reader *reader, struct i915_wire_writer *reply, int *handled);
int nvrtx_vk_execute(struct nvrtx_vk_session *session, struct nvrtx_vk_command_buffer *command);

int nvrtx_vk_video_route(struct nvrtx_vk_session *session, uint32_t opcode, uint32_t requested, struct i915_wire_reader *reader, struct i915_wire_writer *reply, int *handled);
bool nvrtx_vk_video_recorded(uint32_t opcode);
int nvrtx_vk_video_record(struct nvrtx_vk_session *session, struct nvrtx_vk_command_buffer *command, uint32_t opcode, struct i915_wire_reader *reader, struct nvrtx_vk_record *record);
void nvrtx_vk_video_drop(struct nvrtx_vk_record *record);
int nvrtx_vk_video_run(struct nvrtx_vk_session *session, const struct nvrtx_vk_record *record);

uint32_t nvrtx_vk_result(int error);
uint64_t nvrtx_vk_create_tail(struct i915_wire_reader *reader);
void nvrtx_vk_create_reply(struct nvrtx_vk_session *session, struct i915_wire_writer *reply, enum i915_vk_object_kind kind, uint64_t identity, void *payload, int (*destroy)(struct nvrtx_vk_session *, void *), int error);
int nvrtx_vk_destroy_plain(struct nvrtx_vk_session *session, struct i915_wire_reader *reader, enum i915_vk_object_kind kind);
void *nvrtx_vk_payload(struct nvrtx_vk_session *session, enum i915_vk_object_kind kind, uint64_t identity);
int nvrtx_vk_free_payload(struct nvrtx_vk_session *session, void *payload);

bool nvrtx_vk_format_colour(uint32_t format);
bool nvrtx_vk_format_depth(uint32_t format);
void nvrtx_vk_image_level(const struct nvrtx_vk_image *image, uint32_t level, uint64_t *offset, uint32_t *width, uint32_t *height, uint32_t *pitch);
int nvrtx_vk_buffer_address(const struct nvrtx_vk_buffer *buffer, uint64_t *va, uint8_t **cpu);
int nvrtx_vk_image_address(const struct nvrtx_vk_image *image, uint64_t *va, uint8_t **cpu);
int nvrtx_vk_buffer_cpu(struct nvrtx_vk_session *session, const struct nvrtx_vk_buffer *buffer, uint8_t **cpu);
int nvrtx_vk_image_cpu(struct nvrtx_vk_session *session, const struct nvrtx_vk_image *image, uint8_t **cpu);

#endif /* DRIVERS_GPU_NVRTX_VK_OBJECTS_H */
