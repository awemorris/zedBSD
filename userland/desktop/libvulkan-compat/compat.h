/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The Linux Vulkan chain's internal records; dispatchable handles remain backend handles.
 */

#ifndef KEILAND_VULKAN_COMPAT_H
#define KEILAND_VULKAN_COMPAT_H

#include <drm/drm.h>
#include <drm/drm_mode.h>
#include <pthread.h>
#include <stdint.h>
#include <vulkan/vulkan.h>
#include <vulkan/vulkan_wayland.h>
#include <wayland-client.h>
#include "linux-dmabuf-v1-client-protocol.h"

#define COMPAT_INSTANCE_SURFACE 1U
#define COMPAT_INSTANCE_WAYLAND 2U
#define COMPAT_INSTANCE_DISPLAY 4U
#define COMPAT_INSTANCE_DIRECT 8U
#define COMPAT_INSTANCE_DRM 16U
#define COMPAT_WSI_NONE 0U
#define COMPAT_WSI_MODIFIER 1U
#define COMPAT_WSI_LINEAR 2U

/* Backend entry points resolved directly by dlsym, never through an interposed procedure query. */
struct compat_backend {
	void *handle;
	PFN_vkGetInstanceProcAddr get_instance_proc;
	PFN_vkGetDeviceProcAddr get_device_proc;
	PFN_vkCreateInstance create_instance;
	PFN_vkDestroyInstance destroy_instance;
	PFN_vkEnumerateInstanceExtensionProperties instance_extensions;
	PFN_vkEnumerateInstanceLayerProperties instance_layers;
	PFN_vkEnumerateInstanceVersion instance_version;
	PFN_vkEnumerateDeviceExtensionProperties device_extensions;
	PFN_vkCreateDevice create_device;
	PFN_vkDestroyDevice destroy_device;
	PFN_vkGetDeviceQueue get_queue;
	PFN_vkGetDeviceQueue2 get_queue2;
};

/* One live backend instance, owned until destruction with its physical-device ownership list. */
struct compat_instance {
	struct compat_instance *next;
	VkInstance handle;
	uint32_t api_version;
	unsigned enabled;
	PFN_vkGetPhysicalDeviceProperties properties;
	PFN_vkGetPhysicalDeviceFormatProperties2 format_properties;
	PFN_vkGetPhysicalDeviceImageFormatProperties2 image_properties;
	PFN_vkGetPhysicalDeviceMemoryProperties memory_properties;
	PFN_vkGetPhysicalDeviceQueueFamilyProperties queue_properties;
	PFN_vkGetPhysicalDeviceExternalSemaphoreProperties semaphore_properties;
	PFN_vkGetPhysicalDeviceExternalFenceProperties fence_properties;
	uint32_t physical_count;
	VkPhysicalDevice *physicals;
};

/* One live backend device, owned until destruction and referring to its still-live instance. */
struct compat_device {
	struct compat_device *next;
	VkDevice handle;
	VkPhysicalDevice physical;
	struct compat_instance *instance;
	unsigned swapchain;
	unsigned path;
	unsigned implicit_sync;
	unsigned foreign;
	PFN_vkCreateImage create_image;
	PFN_vkDestroyImage destroy_image;
	PFN_vkGetImageMemoryRequirements2 image_requirements;
	PFN_vkGetImageMemoryRequirements image_requirements1;
	PFN_vkAllocateMemory allocate_memory;
	PFN_vkFreeMemory free_memory;
	PFN_vkBindImageMemory bind_image;
	PFN_vkGetMemoryFdKHR memory_fd;
	PFN_vkGetImageDrmFormatModifierPropertiesEXT image_modifier;
	PFN_vkGetImageSubresourceLayout image_layout;
	PFN_vkCreateCommandPool create_pool;
	PFN_vkDestroyCommandPool destroy_pool;
	PFN_vkAllocateCommandBuffers allocate_commands;
	PFN_vkBeginCommandBuffer begin_command;
	PFN_vkEndCommandBuffer end_command;
	PFN_vkCmdPipelineBarrier barrier;
	PFN_vkCmdCopyImageToBuffer copy_image;
	PFN_vkQueueSubmit submit;
	PFN_vkQueueWaitIdle queue_idle;
	PFN_vkCreateSemaphore create_semaphore;
	PFN_vkDestroySemaphore destroy_semaphore;
	PFN_vkGetSemaphoreFdKHR semaphore_fd;
	PFN_vkImportSemaphoreFdKHR import_semaphore;
	PFN_vkCreateFence create_fence;
	PFN_vkDestroyFence destroy_fence;
	PFN_vkImportFenceFdKHR import_fence;
	PFN_vkWaitForFences wait_fences;
	PFN_vkResetFences reset_fences;
	PFN_vkCreateBuffer create_buffer;
	PFN_vkDestroyBuffer destroy_buffer;
	PFN_vkGetBufferMemoryRequirements buffer_requirements;
	PFN_vkBindBufferMemory bind_buffer;
	PFN_vkMapMemory map_memory;
	PFN_vkUnmapMemory unmap_memory;
	uint32_t api_version;
};

/* The physical device's actual export and synchronization capabilities, queried before device creation. */
struct compat_capabilities {
	unsigned path;
	unsigned implicit_sync;
	unsigned foreign;
};

/* One retrieved queue, owned by its device and retired before that device is destroyed. */
struct compat_queue {
	struct compat_queue *next;
	VkQueue handle;
	struct compat_device *device;
	uint32_t family;
};

/* One KMS mode, retained with its display for the process lifetime so exported handles remain stable. */
struct compat_display_mode {
	struct compat_display_mode *next;
	struct compat_display *display;
	struct drm_mode_modeinfo mode;
};

/* One connected connector and its modes; acquisition owns a duplicated master descriptor until release. */
struct compat_display {
	struct compat_display *next;
	uint32_t connector;
	uint32_t crtc;
	uint32_t width_mm;
	uint32_t height_mm;
	char name[64];
	struct compat_display_mode *modes;
	struct compat_display_mode *preferred;
	int master_fd;
	/* Only a direct application lets this library change master authority. */
	unsigned master_owned;
	struct drm_mode_crtc saved;
	unsigned saved_valid;
	unsigned active;
	struct compat_swapchain *scanout_chain;
};

/* One mapped primary-plane dumb buffer, owned by a display swapchain until scanout retirement. */
struct compat_dumb {
	int fd;
	uint32_t handle;
	uint32_t framebuffer;
	uint32_t pitch;
	uint64_t size;
	void *mapping;
};

/* One compositor-advertised pixel layout, retained until its surface is destroyed. */
struct compat_modifier {
	uint32_t format;
	uint64_t modifier;
};

/* One private presentation surface; swapchains keep their callback storage alive on its retired list. */
struct compat_surface {
	struct compat_instance *instance;
	struct compat_display *kms;
	struct compat_display_mode *display_mode;
	VkAllocationCallbacks allocator;
	unsigned allocated;
	struct wl_display *display;
	struct wl_display *display_wrapper;
	struct wl_surface *surface_wrapper;
	struct wl_event_queue *queue;
	struct wl_registry *registry;
	struct zwp_linux_dmabuf_v1 *dmabuf;
	struct wl_callback *frame;
	struct compat_modifier *modifiers;
	uint32_t modifier_count;
	unsigned lost;
	unsigned sync_done;
	VkExtent2D extent;
	struct compat_swapchain *retired;
};

/* One exported swapchain image; release callback data stays live until its proxy is retired. */
struct compat_image {
	struct compat_swapchain *chain;
	VkImage image;
	VkDeviceMemory memory;
	struct wl_buffer *buffer;
	int fd;
	unsigned busy;
	unsigned acquired;
	unsigned presented;
};

/* One externally synchronized swapchain, including private GPU completion resources and up to eight images. */
struct compat_swapchain {
	struct compat_swapchain *next;
	struct compat_surface *surface;
	struct compat_device *device;
	VkAllocationCallbacks allocator;
	unsigned allocated;
	unsigned retired;
	unsigned fallback;
	unsigned foreign;
	unsigned path;
	uint32_t count;
	VkFormat format;
	uint32_t fourcc;
	uint64_t modifier;
	VkExtent2D extent;
	VkPresentModeKHR mode;
	VkQueue queue;
	uint32_t family;
	VkCommandPool pool;
	VkCommandBuffer release_command[8];
	VkCommandBuffer acquire_command[8];
	VkFence fence[8];
	VkSemaphore semaphore[8];
	VkBuffer readback;
	VkDeviceMemory readback_memory;
	void *readback_mapping;
	struct compat_dumb dumb[2];
	unsigned front;
	unsigned scanout;
	unsigned master_owned;
	struct compat_image images[8];
};

/* One immutable maintained procedure name and its forwarding or interception responsibility. */
struct compat_name {
	const char *name;
	char kind;
	PFN_vkVoidFunction address;
};

/* Published by pthread_once; backend code remains loaded for the lifetime of the process. */
extern struct compat_backend compat_backend;

/* Protects the instance, device and queue ownership lists; callers obey Vulkan handle lifetime rules. */
extern pthread_mutex_t compat_mutex;

int compat_backend_ready(void);
void compat_forward_fill(void *handle);
void compat_enter(const char *name);
void compat_leave(void);
void compat_missing(const char *name) __attribute__((noreturn));
int compat_wsi_extension(const char *name);
unsigned compat_instance_extension(const char *name);
int compat_extension_has(const VkExtensionProperties *properties, uint32_t count, const char *name);
VkResult compat_backend_extensions(VkPhysicalDevice physical, const char *layer, uint32_t *count, VkExtensionProperties **properties);
int compat_swapchain_available(VkPhysicalDevice physical);
void compat_physical_capabilities(VkPhysicalDevice physical, struct compat_capabilities *capabilities);
VkBool32 compat_image_supported(VkPhysicalDevice physical, VkFormat format, unsigned path, uint64_t modifier);
VkResult compat_extensions(VkPhysicalDevice physical, const char *layer, uint32_t *count, VkExtensionProperties *properties);
struct compat_instance *compat_instance_get(VkInstance handle);
struct compat_instance *compat_instance_for_physical(VkPhysicalDevice physical);
struct compat_device *compat_device_get(VkDevice handle);
struct compat_queue *compat_queue_get(VkQueue handle);

void *compat_object_allocate(size_t size, const VkAllocationCallbacks *allocator);
void compat_object_free(void *object, unsigned allocated, const VkAllocationCallbacks *allocator);
int compat_surface_progress(struct compat_surface *surface, uint64_t timeout);
uint64_t compat_time(void);
int compat_surface_modifier(struct compat_surface *surface, VkPhysicalDevice physical, VkFormat format, unsigned path, uint32_t *fourcc, uint64_t *modifier);
void compat_swapchain_collect(struct compat_surface *surface, unsigned force);
struct compat_queue *compat_device_queue(struct compat_device *device);

VkResult compat_kms_displays(struct compat_display **displays, uint32_t *count);
VkResult compat_kms_card_matches(int fd);
VkResult compat_kms_acquire(struct compat_display *display, int fd);
void compat_kms_release(struct compat_display *display);
VkResult compat_kms_dumb_create(struct compat_display *display, VkExtent2D extent, struct compat_dumb *buffer);
void compat_kms_dumb_destroy(struct compat_display *display, struct compat_dumb *buffer);
VkResult compat_kms_present(struct compat_swapchain *chain);
void compat_kms_restore(struct compat_display *display);
VkResult compat_display_swapchain_create(struct compat_device *device, const VkSwapchainCreateInfoKHR *create, const VkAllocationCallbacks *allocator, VkSwapchainKHR *swapchain);
void compat_display_chain_free(struct compat_swapchain *chain);

#endif
