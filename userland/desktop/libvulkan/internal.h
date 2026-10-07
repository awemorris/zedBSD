/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Shares Vulkan ownership and transport contracts between library modules.
 */

#ifndef VULKAN_INTERNAL_H
#define VULKAN_INTERNAL_H

#include <stddef.h>
#include <stdint.h>
#include <pthread.h>
#include <vulkan/vulkan.h>
#include <vulkan/vulkan_wayland.h>
#include <uapi/gpu-op.h>

/* The pinned renderer reserves ring zero and owns at most sixty-three device timelines. */
#define VULKAN_QUEUE_TIMELINE_COUNT 64U

/*
 * The queue flags a family has only on a renderer with video coding.  The
 * encode flag (0x40) is a beta enumerant in the pinned 1.3.269 header, so it
 * is written by value.
 */
#define VULKAN_VIDEO_QUEUE_FLAGS ((uint32_t)VK_QUEUE_VIDEO_DECODE_BIT_KHR | 0x40U)

/*
 * The format features that name video decode and encode pictures.  The two
 * encode features (0x08000000, 0x10000000) are beta enumerants in the
 * pinned 1.3.269 header, so they are written by value.
 */
#define VULKAN_VIDEO_FORMAT_FEATURES ((uint32_t)VK_FORMAT_FEATURE_VIDEO_DECODE_OUTPUT_BIT_KHR | \
	(uint32_t)VK_FORMAT_FEATURE_VIDEO_DECODE_DPB_BIT_KHR | \
	0x08000000U | \
	0x10000000U)

struct vulkan_context;
struct vulkan_transport_storage;
struct vulkan_instance_context;
struct vulkan_object;
struct vulkan_memory;
struct gpu_placement;
struct vulkan_image;
struct vulkan_command_pool;
struct vulkan_writer;
struct vulkan_queue_fence;
struct vulkan_reader;

/* Internal kinds are independent of later-core VkObjectType declarations. */
enum vulkan_object_kind {
	VULKAN_OBJECT_INSTANCE,
	VULKAN_OBJECT_PHYSICAL_DEVICE,
	VULKAN_OBJECT_DEVICE,
	VULKAN_OBJECT_QUEUE,
	VULKAN_OBJECT_SEMAPHORE,
	VULKAN_OBJECT_COMMAND_BUFFER,
	VULKAN_OBJECT_FENCE,
	VULKAN_OBJECT_DEVICE_MEMORY,
	VULKAN_OBJECT_BUFFER,
	VULKAN_OBJECT_IMAGE,
	VULKAN_OBJECT_EVENT,
	VULKAN_OBJECT_QUERY_POOL,
	VULKAN_OBJECT_BUFFER_VIEW,
	VULKAN_OBJECT_IMAGE_VIEW,
	VULKAN_OBJECT_SHADER_MODULE,
	VULKAN_OBJECT_PIPELINE_CACHE,
	VULKAN_OBJECT_PIPELINE_LAYOUT,
	VULKAN_OBJECT_RENDER_PASS,
	VULKAN_OBJECT_PIPELINE,
	VULKAN_OBJECT_DESCRIPTOR_SET_LAYOUT,
	VULKAN_OBJECT_SAMPLER,
	VULKAN_OBJECT_DESCRIPTOR_POOL,
	VULKAN_OBJECT_DESCRIPTOR_SET,
	VULKAN_OBJECT_FRAMEBUFFER,
	VULKAN_OBJECT_COMMAND_POOL,
	VULKAN_OBJECT_SURFACE,
	VULKAN_OBJECT_SWAPCHAIN,
	VULKAN_OBJECT_DISPLAY,
	VULKAN_OBJECT_DISPLAY_MODE,
	VULKAN_OBJECT_VIDEO_SESSION,
	VULKAN_OBJECT_VIDEO_SESSION_PARAMETERS
};

/* Retains the application callbacks used for one ownership lifetime. */
struct vulkan_allocator {
	VkAllocationCallbacks callbacks;
	VkBool32 has_callbacks;
};

/* Local and remote identities have distinct lifetimes. */
struct vulkan_object {
	enum vulkan_object_kind kind;
	struct vulkan_object *parent;
	struct vulkan_context *context;
	uint64_t wire_id;
	struct vulkan_allocator allocator;
	VkSystemAllocationScope scope;
	struct vulkan_object *first_child;
	struct vulkan_object *next_sibling;
	VkBool32 published;
	void (*release_storage)(struct vulkan_object *object);
};

/* One local instance can contain a remote instance for each compatible GPU. */
struct VkInstance_T {
	struct vulkan_object object;
	uint32_t api_version;
	uint64_t enabled_extensions;
	struct vulkan_instance_context *contexts;
	struct VkPhysicalDevice_T **physical_devices;
	uint32_t physical_device_count;
	pthread_mutex_t mutex;
};

/* Caches one renderer physical device owned by its public instance. */
struct VkPhysicalDevice_T {
	struct vulkan_object object;
	struct VkInstance_T *instance;
	struct vulkan_instance_context *instance_context;
	VkPhysicalDeviceProperties properties;
	VkPhysicalDeviceFeatures features;
	VkPhysicalDeviceMemoryProperties memory;
	VkQueueFamilyProperties *queue_families;
	uint32_t queue_family_count;
	uint64_t supported_extensions;
	/*
	 * The video codec operations of each queue family, by family index.
	 * They are asked of the renderer once, and only when its native word
	 * promises video decode; otherwise every entry stays zero.
	 */
	VkVideoCodecOperationFlagsKHR queue_video_operations[VULKAN_QUEUE_TIMELINE_COUNT];
};

/* Owns logical-device children and briefly serializes host sync state. */
struct VkDevice_T {
	struct vulkan_object object;
	struct VkPhysicalDevice_T *physical;
	VkPhysicalDeviceFeatures enabled_features;
	uint64_t enabled_extensions;
	struct VkQueue_T **queues;
	uint32_t queue_count;
	pthread_mutex_t mutex;
	VkResult error;
};

/* Retains one requested queue and serializes internal submission users. */
struct VkQueue_T {
	struct vulkan_object object;
	struct VkDevice_T *device;
	uint32_t family;
	uint32_t index;
	uint32_t timeline_index;
	struct vulkan_queue_fence *private_fences;
	pthread_mutex_t mutex;
};

/* Keeps one encoded command independent of other recording threads. */
struct vulkan_writer {
	struct vulkan_allocator allocator;
	VkSystemAllocationScope scope;
	uint32_t opcode;
	uint32_t external_memory_type;
	uint8_t *data;
	size_t bytes;
	size_t capacity;
	VkResult error;
};

/* Tracks a command buffer whose storage is owned by its allocating pool. */
struct VkCommandBuffer_T {
	struct vulkan_object object;
	struct vulkan_command_pool *pool;
	VkCommandBufferLevel level;
	struct vulkan_writer recording;
	uint32_t state;
	VkResult error;
};

/* Keeps one allocation alive independently of its active mapped view. */
struct vulkan_memory {
	struct vulkan_object object;
	VkDeviceSize bytes;
	uint32_t type_index;
	VkMemoryPropertyFlags properties;
	VkExternalMemoryHandleTypeFlags export_types;
	void *backing_private;
	void *mapped_base;
	VkDeviceSize mapped_offset;
	VkDeviceSize mapped_bytes;
	/*
	 * The image a dedicated import of an image capability was checked
	 * against (VK_KHR_dedicated_allocation), or 0.  Only that image may be
	 * bound to the memory, at offset 0, so the kernel's description the
	 * import compared stays the description of what is bound.
	 */
	uint64_t dedicated_image;
};

/* Retains ordinary image properties required by resource and WSI operations. */
struct vulkan_image {
	struct vulkan_object object;
	VkFormat format;
	VkImageType type;
	VkExtent3D extent;
	uint32_t mip_levels;
	uint32_t array_layers;
	VkSampleCountFlagBits samples;
	VkImageUsageFlags usage;
	VkImageTiling tiling;
	VkSharingMode sharing_mode;
	VkBool32 swapchain_owned;
	/*
	 * The renderer's answers about this image, kept after the first query:
	 * its memory requirements, and the layout of its first color
	 * subresource (level 0, layer 0).  An image's answers never change, and
	 * an importer asks them twice (itself, then libvulkan's check of a
	 * dedicated import), which on a remote renderer is a round trip each
	 * (ws103-p007).
	 */
	VkBool32 requirements_known;
	VkMemoryRequirements requirements;
	VkBool32 color_layout_known;
	VkSubresourceLayout color_layout;
};

/* Renderer external-storage declaration; never exposed as a guest fd ABI. */
#define VULKAN_EXTERNAL_MEMORY_DMABUF 0x200U
#define VULKAN_EXTERNAL_MEMORY_OPAQUE 0x1U

/* Private WSI creation chain following the pinned Vulkan declaration layout. */
struct vulkan_external_image_info {
	VkStructureType sType;
	const void *pNext;
	uint32_t handle_types;
};

/* Private buffer storage declaration uses the renderer's external-memory ABI. */
struct vulkan_external_buffer_info {
	VkStructureType sType;
	const void *pNext;
	uint32_t handle_types;
};

struct gpu_image_descriptor;
struct gpu_allocation_descriptor;

VkResult vulkan_resource_blob_flags(struct vulkan_context *context, uint64_t bytes, uint64_t blob_id, uint32_t flags, uint64_t *handle, uint32_t *resource_id);
VkResult vulkan_memory_allocate(VkDevice device, const VkMemoryAllocateInfo *info, const VkAllocationCallbacks *allocator, VkBool32 shared, VkDeviceMemory *memory);
VkResult vulkan_memory_allocate_placed(VkDevice device, const VkMemoryAllocateInfo *info, const VkAllocationCallbacks *allocator, const struct gpu_placement *placement, VkDeviceMemory *memory);
VkResult vulkan_memory_image_fd(struct VkDevice_T *device, VkDeviceMemory memory, struct gpu_image_descriptor *image, int *fd);
VkResult vulkan_memory_allocation_fd(struct VkDevice_T *device, VkDeviceMemory memory, struct gpu_allocation_descriptor *allocation, int *fd);
VkResult vulkan_memory_import(struct VkDevice_T *device, const VkMemoryAllocateInfo *info, const VkAllocationCallbacks *allocator, uint32_t resource, uint64_t alias, VkDeviceMemory *memory);
void vulkan_encode_image_external(struct vulkan_writer *writer, const void *chain);
void vulkan_encode_buffer_external(struct vulkan_writer *writer, const void *chain);
VkResult vulkan_physical_identity(struct VkPhysicalDevice_T *physical, VkPhysicalDeviceIDProperties *identity);

/* Instance capabilities are enabled explicitly by VkInstanceCreateInfo. */
enum vulkan_instance_extension_bits {
	VULKAN_INSTANCE_SURFACE = 1,
	VULKAN_INSTANCE_DISPLAY = 2,
	VULKAN_INSTANCE_WAYLAND = 4,
	VULKAN_INSTANCE_PROPERTIES2 = 8,
	VULKAN_INSTANCE_EXTERNAL_MEMORY = 16,
	VULKAN_INSTANCE_EXTERNAL_FENCE = 32,
	VULKAN_INSTANCE_SURFACE_COUNTER = 64
};

/* Device capabilities are enabled explicitly by VkDeviceCreateInfo. */
enum vulkan_device_extension_bits {
	VULKAN_DEVICE_SWAPCHAIN = 1,
	VULKAN_DEVICE_DISPLAY_SWAPCHAIN = 2,
	VULKAN_DEVICE_EXTERNAL_MEMORY = 4,
	VULKAN_DEVICE_EXTERNAL_MEMORY_FD = 8,
	VULKAN_DEVICE_EXTERNAL_FENCE = 16,
	VULKAN_DEVICE_EXTERNAL_FENCE_FD = 32,
	VULKAN_DEVICE_MEMORY_REQUIREMENTS2 = 64,
	VULKAN_DEVICE_DEDICATED_ALLOCATION = 128,
	VULKAN_DEVICE_SYNCHRONIZATION2 = 256,
	VULKAN_DEVICE_VIDEO_QUEUE = 512,
	VULKAN_DEVICE_VIDEO_DECODE_QUEUE = 1024,
	VULKAN_DEVICE_VIDEO_DECODE_H264 = 2048,
	VULKAN_DEVICE_DISPLAY_CONTROL = 4096,
	VULKAN_DEVICE_INCREMENTAL_PRESENT = 8192
};

/* Owns a completed reply after the transport transaction unlocks. */
struct vulkan_reader {
	struct vulkan_allocator allocator;
	uint8_t *data;
	size_t bytes;
	size_t cursor;
	VkResult error;
};

/* Retains one supervised job until terminal observation or context close. */
struct vulkan_notification {
	struct vulkan_notification *next;
	uint64_t sequence;
	unsigned waiters;
};

/* One blocked operation owns independent error and image-transition wake storage. */
struct vulkan_wake {
	struct vulkan_wake *next;
	struct VkDevice_T *device;
	struct vulkan_context *context;
	int read_fd;
	int write_fd;
};

VkResult vulkan_wake_create(struct vulkan_wake *wake, struct VkDevice_T *device);
void vulkan_wake_destroy(struct vulkan_wake *wake);
void vulkan_wake_notify(struct VkDevice_T *device, struct vulkan_context *context);
void vulkan_wake_drain(struct vulkan_wake *wake);
void vulkan_context_error(struct vulkan_context *context, VkResult error);
void vulkan_device_error(struct VkDevice_T *device, VkResult error);

/* Serializes a single GPU session without serializing GPU completion waits. */
struct vulkan_context {
	int fd;
	char device_path[262];
	struct vulkan_transport_storage *storage;
	struct vulkan_notification *notifications;
	void *reply_mapping;
	void *stream_mapping;
	VkBool32 mutex_ready;
	pthread_mutex_t mutex;
	VkResult error;
	uint64_t queue_timelines;
	uint32_t wire_version;
	uint32_t xml_version;
	uint32_t external_memory_type;
	VkBool32 strict_queue;
	VkBool32 native_quiescence;
	VkBool32 copy_display;
	/*
	 * The native renderer's promise of an H.264 decode queue family.  Only
	 * a session whose capability record carries the native word can set it,
	 * and it is what lets the video extensions be named at all.
	 */
	VkBool32 video_h264;
	uint64_t max_resource_bytes;
	uint32_t capabilities;
	uint64_t stream_handle;
	uint32_t stream_resource;
	size_t stream_capacity;
	uint64_t reply_handle;
	uint32_t reply_resource;
	size_t reply_capacity;
};

/* Associates one composite public instance with one renderer instance. */
struct vulkan_instance_context {
	struct VkInstance_T *instance;
	struct vulkan_context *context;
	uint64_t wire_id;
	struct vulkan_instance_context *next;
};

/* Separates global, instance, and device function lookup rules. */
enum vulkan_entry_scope {
	VULKAN_ENTRY_GLOBAL,
	VULKAN_ENTRY_INSTANCE,
	VULKAN_ENTRY_DEVICE
};

/* Describes a callable name without advertising unimplemented capabilities. */
struct vulkan_entrypoint {
	const char *name;
	PFN_vkVoidFunction function;
	uint32_t scope;
	uint64_t extension;
};

/* Handles from standard public types are converted in exactly one place. */
struct VkInstance_T *
vulkan_instance(
	VkInstance instance);

struct VkPhysicalDevice_T *
vulkan_physical_device(
	VkPhysicalDevice physical);

struct VkDevice_T *
vulkan_device(
	VkDevice device);

struct VkQueue_T *
vulkan_queue(
	VkQueue queue);

struct vulkan_image *
vulkan_image(
	VkImage image);

struct vulkan_memory *
vulkan_memory(
	VkDeviceMemory memory);

/* The kernel's description of an image capability (uapi/gpu.h). */
struct gpu_image_descriptor;

VkResult
vulkan_dedicated_check(
	const struct vulkan_image *image,
	const VkMemoryAllocateInfo *info,
	const VkMemoryRequirements *requirements,
	const VkSubresourceLayout *layout,
	const struct gpu_image_descriptor *described);

struct vulkan_object *
vulkan_nondispatchable_object(
	uint64_t handle);

uint64_t
vulkan_nondispatchable_handle(
	struct vulkan_object *object);

/* Selects the command allocator, then the effective policy of its owning parent. */
VkResult
vulkan_object_alloc(
	size_t bytes,
	size_t alignment,
	enum vulkan_object_kind kind,
	struct vulkan_object *parent,
	struct vulkan_context *context,
	const VkAllocationCallbacks *allocator,
	VkSystemAllocationScope scope,
	struct vulkan_object **result);

void
vulkan_object_free(
	struct vulkan_object *object);

void
vulkan_object_free_with_allocator(
	struct vulkan_object *object,
	const VkAllocationCallbacks *allocator);

VkResult
vulkan_object_create_complete(
	struct VkDevice_T *device,
	struct vulkan_object *object,
	struct vulkan_writer *writer,
	uint32_t destroy_opcode);

VkResult
vulkan_object_destroy_remote(
	struct VkDevice_T *device,
	struct vulkan_object *object,
	uint32_t opcode);

VkResult
vulkan_reply_finish(
	struct vulkan_context *context,
	struct vulkan_reader *reader,
	VkResult status);

VkBool32
vulkan_reply_pointer(
	struct vulkan_reader *reader);

VkResult
vulkan_object_reserve_id(
	struct vulkan_object *object);

VkResult
vulkan_object_publish(
	struct vulkan_object *object);

void
vulkan_object_unpublish(
	struct vulkan_object *object);

uint64_t
vulkan_object_wire_id(
	const struct vulkan_object *object);

void *
vulkan_allocate(
	const struct vulkan_allocator *allocator,
	size_t bytes,
	size_t alignment,
	VkSystemAllocationScope scope);

void
vulkan_free(
	const struct vulkan_allocator *allocator,
	void *allocation);

/* These acquire internal locks; callers must hold no queue or context lock. */
VkResult
vulkan_queue_submit(
	struct VkQueue_T *queue,
	uint32_t count,
	const VkSubmitInfo *submits,
	VkFence fence);

VkResult
vulkan_queue_idle(
	struct VkQueue_T *queue);

VkResult
vulkan_fences_wait(
	struct VkDevice_T *device,
	uint32_t count,
	const VkFence *fences,
	VkBool32 all,
	uint64_t timeout_ns);

VkResult
vulkan_wsi_acquire_signal(
	struct VkDevice_T *device,
	VkSemaphore semaphore,
	VkFence fence);

/* WSI helpers are called outside wire locks and own only presentation state. */
VkResult
vulkan_wsi_queue_idle(
	struct VkQueue_T *queue);

VkResult
vulkan_wsi_device_idle(
	struct VkDevice_T *device);

void
vulkan_wsi_device_finish(
	struct VkDevice_T *device);

void
vulkan_wsi_instance_finish(
	struct VkInstance_T *instance);

/* Queries attachment use without exposing native renderer objects. */
VkResult
vulkan_render_pass_subpass(
	VkRenderPass render_pass,
	uint32_t index,
	VkBool32 *color,
	VkBool32 *depth_stencil);

const VkAttachmentDescription *
vulkan_render_pass_attachment(
	VkRenderPass render_pass,
	uint32_t index);

/* Both ordinary and WSI command recording use one backend layout conversion. */
uint32_t
vulkan_wire_image_layout(
	VkImageLayout layout);

/* A reply is independently owned after execute releases the context lock. */
void
vulkan_writer_init(
	struct vulkan_writer *writer);

void
vulkan_writer_init_for_object(
	struct vulkan_writer *writer,
	const struct vulkan_object *object);

void
vulkan_writer_finish(
	struct vulkan_writer *writer);

void
vulkan_write_u32(
	struct vulkan_writer *writer,
	uint32_t value);

void
vulkan_write_u64(
	struct vulkan_writer *writer,
	uint64_t value);

void
vulkan_write_bytes(
	struct vulkan_writer *writer,
	const void *bytes,
	size_t count);

VkResult
vulkan_context_execute(
	struct vulkan_context *context,
	const struct vulkan_writer *writer,
	size_t reply_capacity,
	struct vulkan_reader *reply);

void
vulkan_reader_finish(
	struct vulkan_reader *reader);

VkResult
vulkan_context_open(
	struct vulkan_context *context,
	const char *path);

VkResult
vulkan_context_close(
	struct vulkan_context *context);

void
vulkan_context_lock(
	struct vulkan_context *context);

void
vulkan_context_unlock(
	struct vulkan_context *context);

VkResult
vulkan_resource_blob(
	struct vulkan_context *context,
	uint64_t bytes,
	uint64_t blob_id,
	uint64_t *handle,
	uint32_t *resource_id);

VkResult
vulkan_resource_destroy(
	struct vulkan_context *context,
	uint64_t handle);

VkResult
vulkan_resource_copy(
	struct vulkan_context *context,
	uint64_t handle,
	uint64_t offset,
	void *bytes,
	size_t count,
	VkBool32 write);

VkBool32
vulkan_instance_extension(
	struct VkInstance_T *instance,
	uint64_t bits);

VkBool32
vulkan_device_extension(
	struct VkDevice_T *device,
	uint64_t bits);

void
vulkan_command_begin(
	struct vulkan_writer *writer,
	uint32_t opcode);

VkResult
vulkan_command_execute(
	struct vulkan_context *context,
	const struct vulkan_writer *writer,
	size_t reply_capacity,
	struct vulkan_reader *reply,
	VkBool32 has_result);

void
vulkan_writer_reserve(
	struct vulkan_writer *writer,
	size_t additional);

void
vulkan_write_float(
	struct vulkan_writer *writer,
	float number);

void
vulkan_write_pointer(
	struct vulkan_writer *writer,
	const void *pointer);

void
vulkan_write_string(
	struct vulkan_writer *writer,
	const char *text);

void
vulkan_reader_init(
	struct vulkan_reader *reader,
	uint8_t *owned_data,
	size_t bytes);

uint32_t
vulkan_read_u32(
	struct vulkan_reader *reader);

uint64_t
vulkan_read_u64(
	struct vulkan_reader *reader);

float
vulkan_read_float(
	struct vulkan_reader *reader);

void
vulkan_read_bytes(
	struct vulkan_reader *reader,
	void *destination,
	size_t bytes);

VkResult
vulkan_read_result(
	struct vulkan_reader *reader);

/* Video commands are recorded with the same framing as the ordinary vkCmd* calls. */
VkBool32
vulkan_command_record_begin(
	struct VkCommandBuffer_T *command,
	struct vulkan_writer *writer,
	uint32_t opcode);

void
vulkan_command_record_finish(
	struct VkCommandBuffer_T *command,
	struct vulkan_writer *writer);

/* Checks a video profile list on an image query against the one supported profile. */
VkResult
vulkan_video_profile_list_check(
	struct VkPhysicalDevice_T *physical,
	const VkVideoProfileListInfoKHR *list);

/* The caller owns queue->mutex across submission and presentation publication. */
void vulkan_queue_finish(struct VkQueue_T *queue);
VkResult vulkan_queue_submit_locked(struct VkQueue_T *queue, uint32_t count, const VkSubmitInfo *submits, VkFence fence);

#include "codec.h"

#endif
