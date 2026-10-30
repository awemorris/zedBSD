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
 * and hands the program frames that are images of it.  libmedia reaches
 * Vulkan only through the program's vkGetInstanceProcAddr.  Memory that
 * must be shared with a kernel driver (a camera writing into an image) is
 * exported and imported inside libmedia; no handle leaves it.
 *
 * Images libmedia makes are created with VK_SHARING_MODE_CONCURRENT over
 * libmedia's queue families and the program's reader families, so no
 * queue family ownership transfer is needed on either side.
 */

#ifndef LIBMEDIA_MEDIA_VULKAN_H
#define LIBMEDIA_MEDIA_VULKAN_H

#include "userland/desktop/libmedia/media.h"

#include <vulkan/vulkan.h>

/* The most names a requirement lists, queues a context uses, and reader families. */
#define MEDIA_VULKAN_EXTENSIONS_MAX	16U
#define MEDIA_VULKAN_QUEUES_MAX		4U
#define MEDIA_VULKAN_READERS_MAX	4U

/* What libmedia does with a queue (bits). */
#define MEDIA_QUEUE_DECODE	0x1U	/* Vulkan Video decode */
#define MEDIA_QUEUE_ENCODE	0x2U	/* Vulkan Video encode */
#define MEDIA_QUEUE_COMPUTE	0x4U	/* conversion, upload and copies (a compute-capable family) */

/*
 * What an instance must have: the lowest API version and the instance
 * extensions to enable, all of them present on this system (the names
 * are libmedia's constant strings).
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
 * the system lacks an instance extension libmedia cannot work without
 * (libmedia then works only without a device), or EINVAL.
 */
int media_vulkan_instance_requirements(PFN_vkGetInstanceProcAddr get, struct media_vulkan_instance_requirements *requirements);

/* One queue family libmedia asks queues of, how many (at most), and for what. */
struct media_vulkan_queue_request {
	uint32_t family;
	uint32_t count;
	unsigned roles;
};

/*
 * What a device must be made with for libmedia: the device extensions to
 * enable (those of roles the device can serve), whether
 * synchronization2 must be enabled, and the queues asked.  decode_codecs
 * and encode_codecs say what the device does in hardware (bit 1 <<
 * MEDIA_CODEC_*), so that a program choosing among devices may prefer
 * one.  A program that cannot give a queue of its own to compute shares
 * one of its own queues (struct media_vulkan_queue's lock).
 */
struct media_vulkan_requirements {
	const char *extensions[MEDIA_VULKAN_EXTENSIONS_MAX];
	unsigned extension_count;
	VkBool32 synchronization2;
	struct media_vulkan_queue_request queues[MEDIA_VULKAN_QUEUES_MAX];
	unsigned queue_count;
	uint32_t decode_codecs;
	uint32_t encode_codecs;
};

/*
 * Fills what a device of a physical device needs for the roles asked
 * (MEDIA_QUEUE_*; COMPUTE is always included).  The instance must have
 * been made with media_vulkan_instance_requirements.  Returns 0 (with
 * fewer roles served when the device cannot do some: the codec masks say
 * which), ENOTSUP when the device has no compute-capable family, EINVAL.
 */
int media_vulkan_device_requirements(PFN_vkGetInstanceProcAddr get, VkInstance instance, VkPhysicalDevice physical, unsigned roles, struct media_vulkan_requirements *requirements);

/*
 * One queue libmedia may submit to: its family and index, the handle,
 * the roles, and -- when the program also submits to it -- the program's
 * lock around submissions (libmedia holds it only around
 * vkQueueSubmit/vkQueueSubmit2; NULL lock when libmedia alone submits).
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
 * A frame as an image of the context's device.
 *
 * image is valid while the program holds a reference; identity names the
 * image for the program's caches of views (it never names another image
 * while this one lives).  format, create_flags, usage and tiling are the
 * image's own; a multi-planar format has plane_count planes whose views
 * use plane_formats (R8 and R8G8 for NV12: create_flags then holds
 * VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT and VK_IMAGE_CREATE_EXTENDED_USAGE_BIT).
 * image_width and image_height are the image's extent, x, y, width and
 * height the visible window.  layout is the layout the image is in; the
 * program reads it in that layout and does not change it.  The pixels
 * are complete when the frame is handed out (libmedia waited for the
 * work that wrote them); the program drops its reference only after its
 * own GPU work that reads the image has completed.
 */
struct media_vulkan_frame {
	VkImage image;
	uint64_t identity;
	VkFormat format;
	VkImageCreateFlags create_flags;
	VkImageUsageFlags usage;
	VkImageTiling tiling;
	unsigned plane_count;
	VkFormat plane_formats[3];
	uint32_t image_width;
	uint32_t image_height;
	uint32_t x;
	uint32_t y;
	uint32_t width;
	uint32_t height;
	VkImageLayout layout;
};

/* Describes a Vulkan frame: 0, or ENOTSUP for a frame in the CPU's memory. */
int media_frame_vulkan(const struct media_frame *frame, struct media_vulkan_frame *vulkan);

/* The VkFormat of a MEDIA_FORMAT_* (VK_FORMAT_UNDEFINED for none). */
VkFormat media_vulkan_format(unsigned format);

/*
 * Wraps an image of the program's as a frame, for an encoder to read: the
 * description (image, format, extent, window, layout, the image's
 * creation), a binary semaphore libmedia waits on before reading (signalled
 * by the program's work that wrote the image; VK_NULL_HANDLE when the work
 * is known complete), and the function called on a libmedia thread when
 * the last reference goes and libmedia's reads have completed.  The image
 * must have been made concurrent over libmedia's families or the program
 * must have released it to them.  Returns 0, EINVAL, ENOTSUP (a format
 * libmedia does not read), ENOMEM.
 */
int media_frame_wrap_vulkan(struct media_context *context, const struct media_vulkan_frame *vulkan, VkSemaphore written, void (*release)(void *data, VkImage image), void *release_data, struct media_frame **frame);

/*
 * Says that the program's GPU work writing a frame made by
 * media_frame_create (MEDIA_MEMORY_VULKAN) signals a binary semaphore;
 * libmedia waits on it before its first read.  0, EINVAL.
 */
int media_frame_written(struct media_frame *frame, VkSemaphore written);

#endif
