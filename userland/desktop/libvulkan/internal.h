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
#include "include/libc/vulkan/vulkan_zedbsd.h"
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
	/*
	 * Advances (atomically) whenever an accepted submission may have written
	 * swapchain images this library could not name, so a presentation that
	 * compares it with the value it saw at its last copy copies again.
	 */
	uint64_t content_epoch;
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

/*
 * A batch of a long recording waiting for the recording thread: the
 * command buffer, its bytes (the storage stays the batch's place's), the
 * replies its head asks for, whether it waits, and the next one waiting.
 */
#define VULKAN_RECORDING_BATCHES 2U
struct vulkan_recording_batch {
	struct VkCommandBuffer_T *command;
	struct vulkan_writer part;
	uint32_t expected[2];
	uint32_t count;
	VkBool32 waiting;
	struct vulkan_recording_batch *next;
};

/* The swapchain images one command buffer may name as written before it counts them unknown. */
#define VULKAN_CONTENT_IMAGES 8U

/* Tracks a command buffer whose storage is owned by its allocating pool. */
struct VkCommandBuffer_T {
	struct vulkan_object object;
	struct vulkan_command_pool *pool;
	VkCommandBufferLevel level;
	struct vulkan_writer recording;
	/* Where the record being encoded straight into recording begins. */
	size_t record_start;
	/*
	 * The recording's head: a Reset left for the next Begin
	 * (reset_pending, with reset_flags) and the Begin, encoded at the
	 * recording's start (head_bytes) and sent with its first bytes; their
	 * replies (head_opcodes, head_replies of them) are read there.
	 */
	VkBool32 reset_pending;
	uint32_t reset_flags;
	size_t head_bytes;
	uint32_t head_opcodes[2];
	uint32_t head_replies;
	/*
	 * A primary command buffer's End, kept with its recording for the
	 * submission that names it (end_pending): the recording travels in
	 * that submission's transaction, ahead of the submit.  end_serial is
	 * the submission attempt that last wrote it, so a command buffer named
	 * twice travels once.
	 */
	VkBool32 end_pending;
	uint64_t end_serial;
	uint32_t state;
	VkResult error;
	/*
	 * The swapchain images the recording writes with a transfer command,
	 * without repeats, and whether it wrote more (or executed a secondary
	 * that did) than the table holds.  Begin and reset empty it; only an
	 * accepted submission reads it, while valid usage keeps every named
	 * image alive.
	 */
	struct vulkan_image *content_images[VULKAN_CONTENT_IMAGES];
	uint32_t content_count;
	VkBool32 content_unknown;
	/*
	 * A long recording's batches sent by the recording thread
	 * (commands.c): two places a batch's bytes wait in while the thread
	 * sends them, the place the next batch takes, how many batches are
	 * still to be sent, and the first failure they met, which the next
	 * Begin, End or reset makes the command buffer's.  The places' storage
	 * is kept for the next batches.  sent_pending and sent_error, and a
	 * place while it waits, belong to the recording thread's lock.
	 */
	struct vulkan_recording_batch batches[VULKAN_RECORDING_BATCHES];
	uint32_t batch_next;
	uint32_t sent_pending;
	VkResult sent_error;
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
	/*
	 * The layout the image was placed in (VkImageLayoutZEDBSD at its
	 * creation): a GPU_IMAGE_* tiling and its parameter; zero for none.
	 */
	uint32_t layout_tiling;
	uint32_t layout_parameter;
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
	/*
	 * For a swapchain image, advances (atomically) with every accepted
	 * submission that writes it with a transfer command; a presentation
	 * whose last copy saw the same value may show that copy again.
	 */
	uint64_t content_serial;
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

/*
 * The layout a WSI image is placed in (a GPU_IMAGE_* tiling and its
 * parameter), chained after the external-image declaration: the renderer
 * places an optimal-tiling image so.  Sent only to a context whose device
 * names its own layouts (GPU_CAP_IMAGE_LAYOUTS); its type is the library's
 * own, the native executor's private record.
 */
#define VULKAN_STRUCTURE_TYPE_IMAGE_LAYOUT VK_STRUCTURE_TYPE_IMAGE_LAYOUT_ZEDBSD
struct vulkan_image_layout_info {
	VkStructureType sType;
	const void *pNext;
	uint32_t tiling;
	uint32_t parameter;
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

/*
 * The fd of an exportable swapchain's image (wsi-image.c), which
 * vkGetMemoryFdKHR gives for a swapchain image.  It is weak because the
 * host tests of the memory family link no WSI; a null one exports nothing.
 */
extern VkResult vulkan_wsi_swapchain_image_fd(struct VkDevice_T *device, VkImage image, int *fd) __attribute__((weak));
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
	/*
	 * The native renderer's promise that GPU_COMMAND runs a transaction at
	 * once on the caller's thread, its replies written when the call
	 * returns (EAGAIN while earlier work of the session is queued).
	 */
	VkBool32 sync_command;
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

/*
 * Separates global, instance, and device function lookup rules.
 * VULKAN_ENTRY_PHYSICAL is a physical-device command of a device
 * extension (vkGetPhysicalDeviceVideoCapabilitiesKHR): instance-level, but
 * visible when a physical device supports its device extension, which no
 * instance enables (ws202, T1-529).
 */
enum vulkan_entry_scope {
	VULKAN_ENTRY_GLOBAL,
	VULKAN_ENTRY_INSTANCE,
	VULKAN_ENTRY_DEVICE,
	VULKAN_ENTRY_PHYSICAL
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

/* Records which swapchain images a command buffer writes; the device mutex is not needed. */
void
vulkan_content_note_image(
	struct VkCommandBuffer_T *command,
	VkImage image);

void
vulkan_content_note_secondaries(
	struct VkCommandBuffer_T *command,
	uint32_t count,
	const VkCommandBuffer *buffers);

void
vulkan_content_clear(
	struct VkCommandBuffer_T *command);

/* Counts the writes of an accepted submission; the caller holds the device mutex. */
void
vulkan_content_commit(
	struct VkDevice_T *device,
	uint32_t count,
	const VkSubmitInfo *submits);

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

/*
 * Appends a little-endian 32-bit protocol word as vulkan_write_u32() does,
 * inline: the storage grows (out of line) only when the word does not
 * fit.  The vkCmd* recordings write through here, several words a
 * command.
 */
static __inline void
vulkan_record_u32(
	struct vulkan_writer *writer,
	uint32_t word)
{
	uint8_t *bytes;

	/* Reserves the complete scalar before modifying the stream length; an earlier failure stays. */
	if (writer->error != VK_SUCCESS || writer->capacity - writer->bytes < 4U) {
		vulkan_writer_reserve(writer, 4);
		if (writer->error != VK_SUCCESS)
			return;
	}

	/* Stores wire byte order independently of host alignment or endianness. */
	bytes = writer->data + writer->bytes;
	bytes[0] = (uint8_t)word;
	bytes[1] = (uint8_t)(word >> 8);
	bytes[2] = (uint8_t)(word >> 16);
	bytes[3] = (uint8_t)(word >> 24);
	writer->bytes += 4;
}

/*
 * Appends a little-endian 64-bit protocol scalar as vulkan_write_u64()
 * does, inline, the storage grown out of line only when it does not fit.
 */
static __inline void
vulkan_record_u64(
	struct vulkan_writer *writer,
	uint64_t number)
{
	uint8_t *bytes;

	/* Reserves both words atomically before exposing any scalar bytes; an earlier failure stays. */
	if (writer->error != VK_SUCCESS || writer->capacity - writer->bytes < 8U) {
		vulkan_writer_reserve(writer, 8);
		if (writer->error != VK_SUCCESS)
			return;
	}

	/* Stores every byte without requiring 64-bit host alignment. */
	bytes = writer->data + writer->bytes;
	bytes[0] = (uint8_t)number;
	bytes[1] = (uint8_t)(number >> 8);
	bytes[2] = (uint8_t)(number >> 16);
	bytes[3] = (uint8_t)(number >> 24);
	bytes[4] = (uint8_t)(number >> 32);
	bytes[5] = (uint8_t)(number >> 40);
	bytes[6] = (uint8_t)(number >> 48);
	bytes[7] = (uint8_t)(number >> 56);
	writer->bytes += 8;
}

/*
 * Appends a non-dispatchable or dispatchable handle's renderer identity
 * (0 for the null handle) as vulkan_encode_handle() does, inline.
 */
static __inline void
vulkan_record_handle(
	struct vulkan_writer *writer,
	uint64_t handle)
{
	const struct vulkan_object *object;
	uint64_t identity;

	/* The standard null handle is 0 on the wire; another is its object's identity. */
	object = (const struct vulkan_object *)(uintptr_t)handle;
	identity = 0;
	if (object != NULL)
		identity = object->wire_id;
	vulkan_record_u64(writer, identity);
}

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

/*
 * A command buffer whose End waits for its submission: its recording is
 * written ahead of the submit (one serial per attempt), then its replies
 * read in the same order; the caller holds the device mutex.
 */
VkResult
vulkan_command_pending_write(
	struct VkCommandBuffer_T *command,
	struct vulkan_writer *writer,
	uint64_t serial,
	uint32_t *replies);

VkResult
vulkan_command_pending_read(
	struct VkCommandBuffer_T *command,
	struct vulkan_reader *reader,
	uint64_t serial);

/* Checks a video profile list on an image query against the one supported profile. */
VkResult
vulkan_video_profile_list_check(
	struct VkPhysicalDevice_T *physical,
	const VkVideoProfileListInfoKHR *list);

/*
 * The submission timing, a diagnostic that is off unless the environment
 * names VULKAN_ZEDBSD_SUBMIT_TIMING: where vkQueueSubmit and the
 * presentations' submissions spend their time, and how long transactions
 * wait for and hold the context lock, said to stderr every five seconds.
 */
enum vulkan_timing_part {
	VULKAN_TIMING_SUBMIT,
	VULKAN_TIMING_QUIESCE,
	VULKAN_TIMING_QUEUE_LOCK,
	VULKAN_TIMING_DEVICE_LOCK,
	VULKAN_TIMING_PREPARE,
	VULKAN_TIMING_RESERVE,
	VULKAN_TIMING_EXECUTE,
	VULKAN_TIMING_FINISH,
	VULKAN_TIMING_CAPACITY,
	VULKAN_TIMING_CONTEXT_WAIT,
	VULKAN_TIMING_CONTEXT_HOLD,
	VULKAN_TIMING_PRESENT_PREPARE,
	VULKAN_TIMING_PRESENT_RECORD,
	VULKAN_TIMING_PRESENT_SUBMIT,
	VULKAN_TIMING_PRESENT,
	VULKAN_TIMING_PRIVATE_FENCE,
	VULKAN_TIMING_REAP,
	VULKAN_TIMING_COMMAND_RAN,
	VULKAN_TIMING_COMMAND_DEFERRED,
	VULKAN_TIMING_PARTS
};

/* The clock now in nanoseconds when the timing is on, else 0 (a part started at 0 is not added). */
uint64_t vulkan_timing_start(void);

/* Adds the time since start to a part; opcode names the transaction of a context hold. */
void vulkan_timing_add(enum vulkan_timing_part part, uint64_t start, uint32_t opcode);

/* Says the sums when five seconds passed since the last report, and clears them. */
void vulkan_timing_report(void);

/* The caller owns queue->mutex across submission and presentation publication. */
void vulkan_queue_finish(struct VkQueue_T *queue);
VkResult vulkan_queue_submit_locked(struct VkQueue_T *queue, uint32_t count, const VkSubmitInfo *submits, VkFence fence);

#include "codec.h"

#endif
