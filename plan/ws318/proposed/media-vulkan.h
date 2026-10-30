/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * libmedia on the program's own Vulkan device.
 *
 * The program asks what libmedia needs of an instance and of a physical
 * device, makes its instance and device with that added to its own, and
 * hands them to media_context_create (MEDIA_DEVICE_SHARED).  libmedia then
 * decodes, converts, encodes and receives camera frames on that device,
 * and hands the program frames whose planes are images of it.  libmedia
 * reaches Vulkan only through the program's vkGetInstanceProcAddr and
 * uses the Vulkan 1.0 core with extensions (KHR entry points), as the
 * system's Vulkan offers.  Memory that must be shared with a kernel
 * driver (a camera writing into GPU memory) is exported and imported
 * inside libmedia; no handle leaves it.
 *
 * Images libmedia hands out are created with VK_SHARING_MODE_CONCURRENT
 * over libmedia's queue families and the program's reader families when
 * those are more than one family, and with VK_SHARING_MODE_EXCLUSIVE when
 * they are all one family; either way no queue family ownership transfer
 * is needed on either side.
 *
 * Queues are externally synchronized (Vulkan): every queue libmedia uses
 * that the program also uses carries the program's lock, and the program
 * holds that same lock around its own vkQueueSubmit, vkQueuePresentKHR
 * and vkQueueWaitIdle on it.  While any libmedia object of a context
 * lives, the program does not call vkDeviceWaitIdle (it waits on its own
 * fences or calls vkQueueWaitIdle under the lock instead).
 */

#ifndef LIBMEDIA_MEDIA_VULKAN_H
#define LIBMEDIA_MEDIA_VULKAN_H

#include "userland/desktop/libmedia/media.h"

#include <vulkan/vulkan.h>

/* The most names a requirement lists, queues a context uses, reader families, and planes of a frame. */
#define MEDIA_VULKAN_EXTENSIONS_MAX	16U
#define MEDIA_VULKAN_QUEUES_MAX		4U
#define MEDIA_VULKAN_READERS_MAX	4U
#define MEDIA_VULKAN_PLANES_MAX		3U

/* What libmedia does with a queue (bits). */
#define MEDIA_QUEUE_DECODE	0x1U	/* Vulkan Video decode */
#define MEDIA_QUEUE_ENCODE	0x2U	/* Vulkan Video encode */
#define MEDIA_QUEUE_CONVERT	0x4U	/* conversions (drawing passes: a graphics-capable family), uploads and copies */

/*
 * What an instance must have: the API version to ask for (libmedia
 * needs only 1.0) and the instance extensions to enable, all of them
 * present on this system (the names are libmedia's constant strings).
 */
struct media_vulkan_instance_requirements {
	uint32_t api_version;
	const char *extensions[MEDIA_VULKAN_EXTENSIONS_MAX];
	unsigned extension_count;
};

/*
 * Fills what an instance needs.  get is the program's
 * vkGetInstanceProcAddr (libmedia asks it for
 * vkEnumerateInstanceExtensionProperties).  Returns 0, or ENOTSUP when
 * the system lacks an instance extension libmedia cannot share a device
 * without (the program then makes a context without a shared device),
 * or EINVAL.
 */
int media_vulkan_instance_requirements(PFN_vkGetInstanceProcAddr get, struct media_vulkan_instance_requirements *requirements);

/*
 * One queue family libmedia asks for: the family, how many queues of it
 * libmedia would like for itself (the program may give fewer and share
 * its own), and for what.  shared_hint is set when the family has fewer
 * queues than the program's and libmedia's uses together are likely to
 * need (a device with one queue for everything), so the program plans to
 * share a queue with its lock.
 */
struct media_vulkan_queue_request {
	uint32_t family;
	uint32_t count;
	unsigned roles;
	int shared_hint;
};

/*
 * What a device must be made with for libmedia: the device extensions to
 * enable (those of roles the device can serve), whether
 * synchronization2 must be enabled, and the queues asked.  decode_codecs
 * and encode_codecs say what the device does in hardware (bit 1 <<
 * MEDIA_CODEC_*), so that a program choosing among devices may prefer
 * one; convert says whether libmedia can convert formats on it (a
 * graphics-capable family exists).
 */
struct media_vulkan_requirements {
	const char *extensions[MEDIA_VULKAN_EXTENSIONS_MAX];
	unsigned extension_count;
	VkBool32 synchronization2;
	struct media_vulkan_queue_request queues[MEDIA_VULKAN_QUEUES_MAX];
	unsigned queue_count;
	uint32_t decode_codecs;
	uint32_t encode_codecs;
	int convert;
};

/*
 * Fills what a device of a physical device needs for the roles asked
 * (MEDIA_QUEUE_*).  The instance must have been made with
 * media_vulkan_instance_requirements.  Returns 0 (with fewer roles served
 * when the device cannot do some: the codec masks and convert say which),
 * EINVAL.
 */
int media_vulkan_device_requirements(PFN_vkGetInstanceProcAddr get, VkInstance instance, VkPhysicalDevice physical, unsigned roles, struct media_vulkan_requirements *requirements);

/*
 * One queue libmedia may submit to: its family and index, the handle,
 * the roles libmedia uses it for, and -- when the program also uses it --
 * the program's lock (libmedia holds it only around its own
 * vkQueueSubmit; NULL lock when libmedia alone uses the queue).  Any role,
 * decode included, may be on a shared queue.
 */
struct media_vulkan_queue {
	uint32_t family;
	uint32_t index;
	VkQueue queue;
	unsigned roles;
	void (*lock)(void *data, VkQueue queue);
	void (*unlock)(void *data, VkQueue queue);
	void *lock_data;
};

/*
 * The program's device as given to media_context_create: how libmedia
 * reaches Vulkan, the instance, physical device and device, the API
 * version the instance was made with, the device extensions actually
 * enabled (libmedia uses only what is both asked and enabled), whether
 * synchronization2 is enabled, the queues libmedia may use, and the queue
 * families in which the program reads libmedia's frames.  The strings and
 * arrays are read during the call only.
 */
struct media_vulkan_device {
	PFN_vkGetInstanceProcAddr get_instance_proc_addr;
	VkInstance instance;
	VkPhysicalDevice physical;
	VkDevice device;
	uint32_t api_version;
	const char *const *extensions;
	unsigned extension_count;
	VkBool32 synchronization2;
	struct media_vulkan_queue queues[MEDIA_VULKAN_QUEUES_MAX];
	unsigned queue_count;
	uint32_t reader_families[MEDIA_VULKAN_READERS_MAX];
	unsigned reader_family_count;
};

/*
 * One plane of a frame as the program samples it: the image that holds
 * it, the aspect to view (VK_IMAGE_ASPECT_PLANE_n_BIT of a multi-planar
 * image, or VK_IMAGE_ASPECT_COLOR_BIT of an image of its own), the format
 * of the view (R8, R8G8, R8G8B8A8 or B8G8R8A8 UNORM) and the plane's
 * extent.
 */
struct media_vulkan_plane {
	VkImage image;
	VkImageAspectFlags aspect;
	VkFormat view_format;
	uint32_t width;
	uint32_t height;
};

/*
 * A frame as images of the context's device.
 *
 * format is the frame's MEDIA_FORMAT_*; its planes follow (one for the
 * RGB formats, two for NV12, three for I420), each an image of its own
 * or a plane of one multi-planar image (the program creates one view per
 * plane from the plane's fields and needs no other knowledge of how
 * libmedia keeps them).  x, y, width and height are the visible window
 * in the first plane's pixels.  layout is the layout every plane is in;
 * the program reads them in that layout and does not change it.
 *
 * identity names this set of images for the program's cache of views:
 * it is drawn from a counter that only grows and is never given to other
 * images, even after these are destroyed.  A program keeps its cache
 * small (least recently used first out, destroying a view only after its
 * own GPU work that used it completed); a frame whose identity is not in
 * the cache gets new views.
 *
 * The pixels are complete when the frame is handed out (libmedia waited
 * for the work that wrote them).  The program drops its reference only
 * after its own GPU work that reads the planes has completed.
 */
struct media_vulkan_frame {
	uint64_t identity;
	unsigned format;
	unsigned plane_count;
	struct media_vulkan_plane planes[MEDIA_VULKAN_PLANES_MAX];
	uint32_t x;
	uint32_t y;
	uint32_t width;
	uint32_t height;
	VkImageLayout layout;
};

/* Describes a Vulkan frame: 0, or ENOTSUP for a frame in the CPU's memory. */
int media_frame_vulkan(const struct media_frame *frame, struct media_vulkan_frame *vulkan);

/*
 * Wraps images of the program's as a frame, for an encoder to read: the
 * description (as media_frame_vulkan gives; identity is ignored), a
 * binary semaphore signalled by the program's work that wrote the images
 * and submitted before the frame is sent (VK_NULL_HANDLE when that work is
 * known complete; libmedia waits on it once, at its first read, and orders
 * every later read of its own after that one), and the function called
 * when the last reference goes and libmedia's reads have completed.  The
 * function runs on whichever thread dropped the last reference or saw the
 * reads complete (an encoder's send, receive or close on the caller's
 * thread, or a libmedia thread), with no libmedia lock held, and must not
 * call libmedia.  Returns 0, EINVAL, ENOTSUP (a format or layout libmedia
 * does not read), ENOMEM.
 */
int media_frame_wrap_vulkan(struct media_context *context, const struct media_vulkan_frame *vulkan, VkSemaphore written, void (*release)(void *data, const struct media_vulkan_frame *vulkan), void *release_data, struct media_frame **frame);

/*
 * Says that the program wrote a frame made by media_frame_create
 * (MEDIA_MEMORY_VULKAN): the layout it left the planes in, and a binary
 * semaphore its work signals, submitted before the frame is sent
 * (VK_NULL_HANDLE when known complete).  libmedia waits on it once.
 * 0, EINVAL (a frame not made by media_frame_create, a layout libmedia
 * cannot leave).
 */
int media_frame_written(struct media_frame *frame, VkImageLayout layout, VkSemaphore written);

#endif
