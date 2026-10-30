/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The PowerVR render node's Vulkan executor.
 *
 * libvulkan sends the zedBSD GPU command protocol (uapi/gpu-op.h) to the
 * render node; the executor decodes it in the kernel and does the work on
 * the core.  The wire's readers and writers and the generated record
 * codec are the i915 executor's, used unchanged.  The executor covers the
 * smallest compute path: the instance, the one physical device and its
 * queue, memory and buffers, descriptor sets, compute pipelines whose
 * SPIR-V the shader compiler takes (shader.h), command buffers
 * that bind and dispatch, and their submission; and the images, image
 * views and samplers of the formats of image-state.c.
 *
 * One executor session belongs to one open of the node.  It holds the
 * session's typed objects by their wire identity and the scratch one
 * command decodes its records into.  The open's ioctls are serialized by
 * the GPU core, which is what serializes everything here.
 */

#ifndef KERN_DRIVERS_GPU_POWERVR_VK_H
#define KERN_DRIVERS_GPU_POWERVR_VK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "drivers/gpu/i915/vk/vk-codec.h"
#include "drivers/gpu/powervr/descriptor-set.h"
#include "drivers/gpu/powervr/image-state.h"
#include "drivers/gpu/powervr/compiler/shader.h"

struct powervr_session;
struct powervr_resource;
struct powervr_buffer;
struct powervr_vk_executor;

/* The scratch one command decodes its records into, and the most live objects of a session. */
#define POWERVR_VK_ARENA_BYTES		(128U * 1024U)
#define POWERVR_VK_OBJECT_LIMIT		1024U

/*
 * The buckets a session's published identities are found by: the
 * compositor's session holds hundreds of objects, and every
 * recorded command finds its objects, so a walk of one list cost the
 * End of a frame's command buffer about 10 ms on the VisionFive 2.
 */
#define POWERVR_VK_OBJECT_BUCKETS	256U

/*
 * The memory types: device local only (type 0) and host visible and
 * coherent (type 1), both the same uncached system memory of one heap.
 * The core does not snoop the processor's caches, so host visible memory
 * is mapped through the uncached alias; a host cached type is not
 * offered, since the user library never asks the driver to flush or
 * invalidate a mapping.
 */
#define POWERVR_VK_MEMORY_DEVICE	0U
#define POWERVR_VK_MEMORY_HOST		1U
#define POWERVR_VK_MEMORY_TYPES		2U

/* The boundary a buffer starts on in its memory, the bindings of a set, the sets of a layout. */
#define POWERVR_VK_RESOURCE_ALIGN	256U
#define POWERVR_VK_BINDINGS_MAX		8U
#define POWERVR_VK_SETS_MAX		4U

/* The largest edge and the most layers of an image the device reports and takes. */
#define POWERVR_VK_IMAGE_EDGE		4096U
#define POWERVR_VK_IMAGE_LAYERS		256U

/* A command buffer's states. */
#define POWERVR_VK_COMMAND_INITIAL	0U
#define POWERVR_VK_COMMAND_RECORDING	1U
#define POWERVR_VK_COMMAND_EXECUTABLE	2U
#define POWERVR_VK_COMMAND_INVALID	3U

struct powervr_vk_object;

/*
 * One executor session.
 *
 * owner is the render node's session.  objects lists every published
 * identity, newest first, and buckets holds the same ones by the hash of
 * their kind and identity (powervr_vk_object_find); live_objects counts the payloads not yet
 * destroyed.  closing is set once the close has begun.  abandoned says a
 * submission's work did not finish in time: the core may still run it,
 * so the executor's own memory stays with the core.  timelines has bit n
 * set once a queue completes jobs on timeline n (read by the job
 * operations, written by vkGetDeviceQueue2).
 */
struct powervr_vk_session {
	struct powervr_session *owner;
	struct powervr_vk_object *objects;
	struct powervr_vk_object *buckets[POWERVR_VK_OBJECT_BUCKETS];
	uint32_t live_objects;
	bool closing;
	bool abandoned;
	uint32_t last_opcode;
	uint32_t failed_opcode;
	struct i915_wire_arena arena;
	uint64_t timelines;
	struct powervr_vk_executor *executor;
};

/*
 * One published wire identity of a kind and the payload it stands for.
 *
 * next links the session's list, bucket_next its bucket's (both while
 * published).  references counts the holders: the registry while
 * published, and every dependent object that retained it; the payload is
 * destroyed when the count reaches zero.
 */
struct powervr_vk_object {
	struct powervr_vk_object *next;
	struct powervr_vk_object *bucket_next;
	struct powervr_vk_session *session;
	enum i915_vk_object_kind kind;
	uint64_t identity;
	uint32_t references;
	bool published;
	void *payload;
	int (*destroy)(struct powervr_vk_session *session, void *payload);
};

/*
 * A VkDeviceMemory: its bytes and type, whether it is exported or
 * imported, and its storage, a resource of the session over a buffer of
 * its own or of the imported blob (mapped for the core).  A blob
 * libvulkan makes for it maps the same buffer once more.
 */
struct powervr_vk_memory {
	uint64_t bytes;
	uint32_t type;
	bool exported;
	bool imported;
	struct powervr_resource *storage;
};

/*
 * The handle types an allocation may be exported by: an opaque file
 * descriptor and a dma-buf (VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT
 * and _DMA_BUF_BIT_EXT).
 */
#define POWERVR_VK_EXTERNAL_HANDLES	0x201U

/* An allocation's extension: whether it is exported, and the blob it imports (0 for none). */
struct powervr_vk_memory_chain {
	bool exported;
	uint32_t imported;
};

int powervr_vk_memory_chain(bool present, uint32_t extension, uint32_t value, struct powervr_vk_memory_chain *chain);

/* A VkBuffer: its size and usage, and once bound the memory object it holds and its offset there. */
struct powervr_vk_buffer {
	uint64_t size;
	uint32_t usage;
	struct powervr_vk_object *memory;
	uint64_t offset;
};

/*
 * A VkImage: its format, shape and layout, its usage, whether it declared
 * external memory, and once bound the memory object it holds and its
 * offset there.
 */
struct powervr_vk_image {
	const struct powervr_format *format;
	struct powervr_image_shape shape;
	struct powervr_image_layout layout;
	uint32_t usage;
	bool external;
	struct powervr_vk_object *memory;
	uint64_t offset;
};

/* A VkImageView: the image it holds, the aspect it shows and the texture unit's two words of it. */
struct powervr_vk_image_view {
	struct powervr_vk_object *image;
	uint32_t aspect;
	uint32_t base_level;
	uint32_t base_layer;
	uint64_t words[2];
};

/* A VkSampler: the texture unit's two words of it. */
struct powervr_vk_sampler {
	uint64_t words[2];
};

/*
 * A VkShaderModule: its SPIR-V words (held in the same allocation, after
 * the structure), and whether the compiler reads a compute entry from
 * them (0) or why not.
 */
struct powervr_vk_shader {
	int status;
	uint32_t word_count;
	uint32_t *words;
};

/*
 * A VkDescriptorSetLayout: its binding count, and where each binding
 * lies in a set's memory.
 */
struct powervr_vk_set_layout {
	uint32_t count;
	struct powervr_set_layout layout;
};

/* A VkPipelineLayout: only its set count is kept. */

struct powervr_vk_pipeline_layout {
	uint32_t set_count;
};

/* A VkDescriptorPool: the most sets it hands out. */
struct powervr_vk_descriptor_pool {
	uint32_t max_sets;
};

/* One binding of a descriptor set: a storage buffer's range, the buffer held. */
struct powervr_vk_descriptor_slot {
	struct powervr_vk_object *buffer;
	uint64_t offset;
	uint64_t range;
};

/*
 * A VkDescriptorSet: the pool it came from (only compared), its storage
 * buffers by binding number for the compute path, and its memory's bytes
 * as its layout places the descriptors (a copy the executor puts in the
 * core's memory when a draw reads it).
 */
struct powervr_vk_descriptor_set {
	const void *pool;
	struct powervr_vk_descriptor_slot slots[POWERVR_VK_BINDINGS_MAX];
	struct powervr_set_layout layout;
	uint8_t *bytes;
};

/*
 * A VkPipeline: a compute pipeline's own copy of its module's SPIR-V
 * (the module may go first), compiled for each workgroup it runs.
 */
struct powervr_vk_graphics;
struct powervr_vk_pipeline {
	uint32_t word_count;
	uint32_t *words;
	struct powervr_vk_graphics *graphics;
};

/*
 * The records of a graphics bind of a pipeline and of descriptor sets:
 * opcodes of their own, so the compute submission's walk never takes
 * them for its own binds.
 */
#define POWERVR_VK_RECORD_BIND_GRAPHICS	0x10000U
#define POWERVR_VK_RECORD_SETS_GRAPHICS	0x10001U

/* A VkCommandPool: its queue family. */
struct powervr_vk_command_pool {
	uint32_t family;
};

/*
 * One recorded command: a bound pipeline, bound sets from first (objects
 * i is set first + i), or a dispatch's counts in values.
 */
struct powervr_vk_record {
	uint32_t opcode;
	void *data;
	struct powervr_vk_object *objects[POWERVR_VK_SETS_MAX];
	uint32_t object_count;
	uint32_t first;
	uint32_t values[3];
};

/*
 * A VkCommandBuffer: its pool (only compared), its state, its records in
 * order (a growing array) and the first error its recording met.
 */
struct powervr_vk_command_buffer {
	const void *pool;
	uint32_t state;
	struct powervr_vk_record *records;
	uint32_t record_count;
	uint32_t record_capacity;
	int error;
};

/*
 * A VkFence: whether it is signaled, and else the completion point of the
 * submission that will signal it (kind POWERVR_POINT_NONE when none).
 */
struct powervr_vk_fence {
	bool signaled;
	unsigned kind;
	uint32_t value;
};

/* A command's handler: sets *handled when the opcode is its own. */
typedef int (*powervr_vk_route)(struct powervr_vk_session *session, uint32_t opcode, uint32_t requested, struct i915_wire_reader *reader, struct i915_wire_writer *reply, int *handled);

/* The session, its objects and the shared parts of creates and destroys (vk-object.c). */
int powervr_vk_open(struct powervr_session *owner, struct powervr_vk_session **result);
void powervr_vk_close(struct powervr_vk_session *session);
int powervr_vk_object_publish(struct powervr_vk_session *session, enum i915_vk_object_kind kind, uint64_t identity, void *payload, int (*destroy)(struct powervr_vk_session *, void *));
struct powervr_vk_object *powervr_vk_object_find(struct powervr_vk_session *session, enum i915_vk_object_kind kind, uint64_t identity);
int powervr_vk_object_retain(struct powervr_vk_object *object);
int powervr_vk_object_release(struct powervr_vk_object *object);
int powervr_vk_object_remove(struct powervr_vk_session *session, enum i915_vk_object_kind kind, uint64_t identity);
void *powervr_vk_payload(struct powervr_vk_session *session, enum i915_vk_object_kind kind, uint64_t identity);
void *powervr_vk_arena_take(struct powervr_vk_session *session, size_t bytes);
uint32_t powervr_vk_result(int error);
uint64_t powervr_vk_create_tail(struct i915_wire_reader *reader);
void powervr_vk_create_reply(struct powervr_vk_session *session, struct i915_wire_writer *reply, enum i915_vk_object_kind kind, uint64_t identity, void *payload, int (*destroy)(struct powervr_vk_session *, void *), int error);
int powervr_vk_destroy_plain(struct powervr_vk_session *session, struct i915_wire_reader *reader, enum i915_vk_object_kind kind);
int powervr_vk_free_payload(struct powervr_vk_session *session, void *payload);

/* The transport: one frame of commands (vk-stream.c). */
int powervr_vk_execute(struct powervr_vk_session *session, const void *wire, uint32_t bytes);

/* The handlers of each group of commands. */
int powervr_vk_device_route(struct powervr_vk_session *session, uint32_t opcode, uint32_t requested, struct i915_wire_reader *reader, struct i915_wire_writer *reply, int *handled);
int powervr_vk_memory_route(struct powervr_vk_session *session, uint32_t opcode, uint32_t requested, struct i915_wire_reader *reader, struct i915_wire_writer *reply, int *handled);
int powervr_vk_image_route(struct powervr_vk_session *session, uint32_t opcode, uint32_t requested, struct i915_wire_reader *reader, struct i915_wire_writer *reply, int *handled);
int powervr_vk_layout_route(struct powervr_vk_session *session, uint32_t opcode, uint32_t requested, struct i915_wire_reader *reader, struct i915_wire_writer *reply, int *handled);
int powervr_vk_command_route(struct powervr_vk_session *session, uint32_t opcode, uint32_t requested, struct i915_wire_reader *reader, struct i915_wire_writer *reply, int *handled);
int powervr_vk_pass_route(struct powervr_vk_session *session, uint32_t opcode, uint32_t requested, struct i915_wire_reader *reader, struct i915_wire_writer *reply, int *handled);
int powervr_vk_queue_route(struct powervr_vk_session *session, uint32_t opcode, uint32_t requested, struct i915_wire_reader *reader, struct i915_wire_writer *reply, int *handled);

/* The render pass records and the render jobs of a command buffer (vk-pass.c). */
struct powervr_render_job;
int powervr_vk_record_begin_pass(struct powervr_vk_session *session, struct i915_wire_reader *reader, struct powervr_vk_record *entry);
int powervr_vk_record_clear_attachments(struct powervr_vk_session *session, struct i915_wire_reader *reader, struct powervr_vk_record *entry);
/* Records a transfer command (copies, a blit of the same size, a colour clear) with its objects and regions. */
int powervr_vk_record_transfer(struct powervr_vk_session *session, uint32_t opcode, struct i915_wire_reader *reader, struct powervr_vk_record *entry);

/* Runs a recorded transfer command on the session's transfer context, region by region. */
int powervr_vk_transfer_run(struct powervr_vk_session *session, const struct powervr_vk_record *entry);

int powervr_vk_render_jobs(const struct powervr_vk_command_buffer *command, struct powervr_render_job *jobs, uint32_t room, uint32_t *count);

/* The graphics pipelines and the draw records (vk-draw.c). */
int powervr_vk_graphics_decode(struct powervr_vk_session *session, struct i915_wire_reader *reader, struct powervr_vk_pipeline *pipeline);
void powervr_vk_graphics_free(struct powervr_vk_graphics *graphics);
int powervr_vk_record_draw(struct powervr_vk_session *session, uint32_t opcode, struct i915_wire_reader *reader, struct powervr_vk_record *entry);

/* A command buffer emptied of its records (vk-command.c). */
void powervr_vk_command_reset(struct powervr_vk_command_buffer *command);

/* The buffer of a VkDeviceMemory, for the blob libvulkan creates for it (vk-memory.c). */
int powervr_vk_memory_buffer(struct powervr_vk_session *session, uint64_t identity, uint64_t bytes, struct powervr_buffer **buffer);

/* A buffer's device address, from its memory (vk-memory.c). */
int powervr_vk_buffer_address(const struct powervr_vk_buffer *buffer, uint64_t *address);

/* An image's device address, from its memory (vk-image.c). */
int powervr_vk_image_address(const struct powervr_vk_image *image, uint64_t *address);

/* The executor's own memory on the core, and its release (vk-queue.c). */
void powervr_vk_executor_free(struct powervr_vk_session *session);

/*
 * Whether the executor sums where a submission's time goes and a thread
 * of its own says the sums every five seconds in which submissions ran
 * (1), or not (0).  Off by default: the report is written to the serial
 * console at its speed, which takes a CPU for about a tenth of a second.
 * A measurement build turns it on (ZEDBSD_TEST_CPPFLAGS +=
 * -DPOWERVR_SUBMIT_TIMING=1).
 */
#ifndef POWERVR_SUBMIT_TIMING
#define POWERVR_SUBMIT_TIMING	0
#endif

/* The parts of a submission's time the executor sums, in timer ticks. */
enum powervr_timing_part {
	POWERVR_TIMING_SUBMIT,
	POWERVR_TIMING_JOB_SETUP,
	POWERVR_TIMING_JOB_MAP,
	POWERVR_TIMING_JOB_BUILD,
	POWERVR_TIMING_JOB_KICK,
	POWERVR_TIMING_JOB_WAIT,
	POWERVR_TIMING_JOB_RELEASE,
	POWERVR_TIMING_TRANSFER,
	POWERVR_TIMING_DISPATCH,
	POWERVR_TIMING_WAIT_TA,
	POWERVR_TIMING_WAIT_3D,
	POWERVR_TIMING_WAIT_SMALL,
	POWERVR_TIMING_BUILD_PROGRAMS,
	POWERVR_TIMING_BUILD_DRAWS,
	POWERVR_TIMING_DRAW_MEMORY,
	POWERVR_TIMING_DRAW_CODE,
	POWERVR_TIMING_RENDER_JOBS,
	POWERVR_TIMING_ORDER_WAIT,
	POWERVR_TIMING_REAP,
	POWERVR_TIMING_TARGET_MAKE,
	POWERVR_TIMING_BUFFER_RUN,
	POWERVR_TIMING_BUFFER_CLEAR,
	POWERVR_TIMING_BUFFER_ALIAS,
	POWERVR_TIMING_BUFFER_MAP,
	POWERVR_TIMING_LIST_MAKE,
	POWERVR_TIMING_CONTEXT_MAKE,
	POWERVR_TIMING_MMU_FLUSH,
	POWERVR_TIMING_GPU_TA,
	POWERVR_TIMING_GPU_3D,
	POWERVR_TIMING_GPU_FRAME,
	POWERVR_TIMING_PARTS
};

/* Adds the ticks since start to a part's sum and counts one more of it (render-queue.c). */
void powervr_timing_add(enum powervr_timing_part part, uint64_t start);

/* Adds the ticks from start to end to a part's sum and counts one more of it (render-queue.c). */
void powervr_timing_add_span(enum powervr_timing_part part, uint64_t start, uint64_t end);

/* The timer's ticks now, for a part's start (render-queue.c). */
uint64_t powervr_timing_now(void);


#endif /* KERN_DRIVERS_GPU_POWERVR_VK_H */
