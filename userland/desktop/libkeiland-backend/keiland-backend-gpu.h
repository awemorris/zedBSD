/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The GPU buffers between the Keiland compositor and its operating system
 * (WS131 p009, plan/ws131/design.md section 3.5).
 *
 * libkeiland-backend owns the protocol of the clients' GPU buffers: the
 * global's name and version, the requests' decoding, the checks of a
 * buffer's description, its import into the compositor's Vulkan device,
 * and on Linux and FreeBSD the dma-buf modifiers and the export of the
 * implicit acquire fence.  The compositor keeps the wire objects, the
 * wl_buffer's lifetime, the images it draws and the surfaces' acquire
 * fences; it lends them to the backend through struct
 * kl_backend_protocol_host, whose resources are the compositor's wire
 * objects as opaque handles.  This header names Vulkan types and is
 * included only where Vulkan is.
 */

#ifndef KL_BACKEND_GPU_H
#define KL_BACKEND_GPU_H

#include <stddef.h>
#include <stdint.h>
#include <vulkan/vulkan.h>

/* One of the compositor's wire objects, as the backend sees it. */
struct kl_backend_resource;

/* What a resource is (the compositor's kind of wire object). */
#define KL_BACKEND_ROLE_OTHER		0U
#define KL_BACKEND_ROLE_FACTORY		1U
#define KL_BACKEND_ROLE_GPU_OBJECT	2U
#define KL_BACKEND_ROLE_BUFFER		3U
#define KL_BACKEND_ROLE_SURFACE		4U

/*
 * The compositor's Vulkan device as the imports use it, and what it can
 * take: the largest image side and the number of memory types.  Valid from
 * the device's creation to its destruction.
 */
struct kl_backend_gpu_device {
	VkInstance instance;
	VkPhysicalDevice physical;
	VkDevice device;
	uint32_t max_dimension;
	uint32_t memory_type_count;
};

/*
 * What the compositor lends the backend for the GPU buffers' protocol.
 *
 * Every function is called on the compositor's event loop's thread, within
 * a kl_backend_gpu_* call.  A resource stays valid until resource_destroy
 * or until the compositor frees it (kl_backend_gpu_resource_free first).
 *
 *   resource_create / resource_create_server   a new wire object of the
 *       client of parent (a client's new_id, or one the server numbers)
 *   resource_find   the client's object of an id, NULL unless it has the
 *       role (a buffer of shared memory is not a GPU buffer)
 *   resource_destroy, resource_role, resource_id, resource_version,
 *   client_number, resource_private (the backend's own record, NULL at
 *       first, freed by the backend in kl_backend_gpu_resource_free)
 *   emit, post_error (a protocol error that ends the client: code, or the
 *       generic invalid-request error when code is UINT32_MAX)
 *   take_fd   the next fd the client sent, or -1 when it has not arrived
 *   buffer_adopt   gives a buffer its imported image (the compositor owns
 *       the image and the memory from then on, and frees them on failure)
 *   buffer_set_alpha   opaque (0) or premultiplied alpha (1); the next
 *       frame is drawn again
 *   surface_fence   joins a fence to the surface's next commit; ENOSPC
 *       when the surface has as many as it takes (the fd is the caller's
 *       then), 0 when the surface owns it
 *   surface_fence_full   whether no more fences can join
 *   device   the Vulkan device of any resource's compositor, NULL before it
 *       is made
 *   log_frames   whether the per-frame log lines were asked for
 */
struct kl_backend_protocol_host {
	struct kl_backend_resource *(*resource_create)(struct kl_backend_resource *parent, uint32_t id, unsigned role, uint32_t version);
	struct kl_backend_resource *(*resource_create_server)(struct kl_backend_resource *parent, unsigned role, uint32_t version);
	struct kl_backend_resource *(*resource_find)(struct kl_backend_resource *any, uint32_t id, unsigned role);
	void (*resource_destroy)(struct kl_backend_resource *resource);
	unsigned (*resource_role)(const struct kl_backend_resource *resource);
	uint32_t (*resource_id)(const struct kl_backend_resource *resource);
	uint32_t (*resource_version)(const struct kl_backend_resource *resource);
	uint64_t (*client_number)(const struct kl_backend_resource *resource);
	void **(*resource_private)(struct kl_backend_resource *resource);
	int (*emit)(struct kl_backend_resource *resource, uint32_t opcode, const void *payload, size_t size);
	int (*post_error)(struct kl_backend_resource *resource, uint32_t code, const char *reason);
	int (*take_fd)(struct kl_backend_resource *resource);
	VkResult (*buffer_adopt)(struct kl_backend_resource *buffer, VkImage image, VkDeviceMemory memory, uint32_t width, uint32_t height, VkFormat format);
	void (*buffer_set_alpha)(struct kl_backend_resource *buffer, uint32_t alpha);
	int (*surface_fence)(struct kl_backend_resource *surface, int fd, uint64_t generation);
	int (*surface_fence_full)(const struct kl_backend_resource *surface);
	const struct kl_backend_gpu_device *(*device)(const struct kl_backend_resource *any);
	int (*log_frames)(const struct kl_backend_resource *any);
};

/* The generic protocol error a post_error with this code sends. */
#define KL_BACKEND_ERROR_INVALID	UINT32_MAX

/* Names the operating system's Wayland global for GPU buffers. */
const char *kl_backend_gpu_global_interface(void);

/* Reports the version offered for that global. */
uint32_t kl_backend_gpu_global_version(void);

/* Tells a newly bound factory what it needs first (the dma-buf formats). */
int kl_backend_gpu_bind(const struct kl_backend_protocol_host *host, struct kl_backend_resource *factory);

/* Carries out a request of the factory or of a GPU object: 0, EAGAIN for a fd still on its way, or an errno that ends the client. */
int kl_backend_gpu_request(const struct kl_backend_protocol_host *host, struct kl_backend_resource *resource, uint32_t opcode, const unsigned char *bytes, size_t size);

/* Takes a buffer's own acquire fence before a surface's commit moves its fences (Linux and FreeBSD). */
void kl_backend_gpu_commit(const struct kl_backend_protocol_host *host, struct kl_backend_resource *surface, struct kl_backend_resource *buffer);

/* Lets go of the backend's record of a resource the compositor frees. */
void kl_backend_gpu_resource_free(const struct kl_backend_protocol_host *host, struct kl_backend_resource *resource);

/* Copies up to capacity extension names; returns how many are needed (the device fails when more than capacity). */
uint32_t kl_backend_gpu_instance_extensions(const char **names, uint32_t capacity);
uint32_t kl_backend_gpu_device_extensions(VkPhysicalDevice physical, const char **names, uint32_t capacity);

/* Reports the frame fence's exported handle type, or 0 for status polling. */
VkExternalFenceHandleTypeFlagBits kl_backend_gpu_frame_fence_type(void);

/*
 * Explains, in one sentence for the person who started the compositor,
 * why the system offers it no usable GPU (text holds at most size bytes
 * with the NUL): 0 with the sentence, ENOENT when the system knows no
 * reason, EINVAL for a missing buffer.
 */
int kl_backend_gpu_unavailable_reason(char *text, size_t size);

#endif
