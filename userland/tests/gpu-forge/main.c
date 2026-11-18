/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Tests that the compositor refuses a GPU buffer whose fd is not the image
 * its description claims (ws103-p004, WS103 V3).
 *
 * The test exports an ordinary Vulkan allocation (an allocation capability,
 * not an image capability) and sends it with a plausible image description
 * through kl_gpu_buffer_v1.create_buffer.  The compositor imports a
 * client buffer only as a dedicated import of an image capability, checked
 * against the kernel's record of it, so it must end the connection with a
 * protocol error.  A renderer that cannot export an allocation (the native
 * i915) cannot run the test: it reports SKIP.
 *
 * Prints "GPUFORGE RESULT refused=0|1 error=N" and "gpu-forge: PASS", "FAIL"
 * or "SKIP".
 */

#include <vulkan/vulkan.h>
#include <vulkan/vulkan_external.h>
#include <wayland-client.h>
#include "userland/desktop/libwayland/keiland-gpu-buffer-v1-client-protocol.h"
#include <uapi/gpu.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/*
 * The test's Wayland objects: the connection and the GPU buffer factory
 * bound from its registry.  They live for the whole run.
 */
struct forge_wayland {
	struct wl_display *display;
	struct wl_registry *registry;
	struct kl_gpu_buffer_v1 *factory;
};

/*
 * The test's Vulkan objects: an image of the claimed layout and the exported
 * allocation it would use.  They live for the whole run.
 */
struct forge_vulkan {
	VkInstance instance;
	VkPhysicalDevice physical;
	VkDevice device;
	VkImage image;
	VkDeviceMemory memory;
	VkMemoryRequirements requirements;
	VkSubresourceLayout layout;
	uint32_t memory_type;
};

static void registry_global(void *data, struct wl_registry *registry, uint32_t name, const char *interface, uint32_t version);
static void registry_remove(void *data, struct wl_registry *registry, uint32_t name);
static int forge_connect(struct forge_wayland *wayland);
static int forge_export(struct forge_vulkan *vulkan, int *fd);

/* The registry listener that binds the GPU buffer factory. */
static const struct wl_registry_listener registry_listener = {
	registry_global,
	registry_remove
};

/*
 * Sends an allocation capability as an image buffer and reports whether the compositor refused it.
 */
int
main(
	void)
{
	struct forge_wayland wayland;
	struct forge_vulkan vulkan;
	struct gpu_image_descriptor described;
	struct wl_array metadata;
	struct wl_buffer *buffer;
	void *slot;
	int fd;
	int error;
	int status;
	int refused;

	/* The compositor and its GPU buffer factory. */
	memset(&wayland, 0, sizeof(wayland));
	error = forge_connect(&wayland);
	if (error != 0) {
		printf("gpu-forge: FAIL (no compositor or factory, error=%d)\n", error);
		return 1;
	}

	/* An exported allocation that is not an image capability. */
	memset(&vulkan, 0, sizeof(vulkan));
	fd = -1;
	error = forge_export(&vulkan, &fd);
	if (error != 0) {
		printf("GPUFORGE RESULT refused=0 error=%d\n", error);
		printf("gpu-forge: SKIP (this renderer exports no allocation)\n");
		return 0;
	}

	/* A description that is plausible for the image, as a client would claim it. */
	memset(&described, 0, sizeof(described));
	described.version = GPU_ABI_VERSION;
	described.size = sizeof(described);
	described.width = 64U;
	described.height = 64U;
	described.format = GPU_PIXEL_BGRA8888;
	described.stride = (uint32_t)vulkan.layout.rowPitch;
	described.offset = vulkan.layout.offset;
	described.allocation_bytes = vulkan.requirements.size;
	described.memory_type = vulkan.memory_type;
	described.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
	described.tiling = GPU_IMAGE_LINEAR;

	/* Makes room for the description in the request's array. */
	wl_array_init(&metadata);
	slot = wl_array_add(&metadata, sizeof(described));
	if (slot == NULL) {
		printf("gpu-forge: FAIL (no memory)\n");
		return 1;
	}

	/* Copies the claimed description into the request. */
	memcpy(slot, &described, sizeof(described));

	/* Sends the forged buffer; the compositor answers on the next round trip. */
	buffer = kl_gpu_buffer_v1_create_buffer(wayland.factory, fd, &metadata);
	(void)buffer;

	/* Drops the local copies of the array and the fd (the request holds its own). */
	wl_array_release(&metadata);
	close(fd);

	/* Waits for the compositor's answer: a refusal ends the connection with a protocol error. */
	status = wl_display_roundtrip(wayland.display);
	error = wl_display_get_error(wayland.display);

	/* Reports whether the connection was ended. */
	refused = 0;
	if (status < 0)
		refused = 1;
	printf("GPUFORGE RESULT refused=%d error=%d\n", refused, error);

	/* An accepted forgery fails the test. */
	if (status >= 0) {
		printf("gpu-forge: FAIL (the compositor accepted the buffer)\n");
		return 1;
	}

	/* Succeeded: the compositor refused the buffer. */
	printf("gpu-forge: PASS\n");
	return 0;
}

/* Binds the GPU buffer factory when the registry announces it. */
static void
registry_global(
	void *data,
	struct wl_registry *registry,
	uint32_t name,
	const char *interface,
	uint32_t version)
{
	struct forge_wayland *wayland;
	int match;

	(void)version;

	/* Only the GPU buffer factory is needed. */
	wayland = data;
	match = strcmp(interface, "kl_gpu_buffer_v1");
	if (match != 0)
		return;

	/* Revision one has create_buffer. */
	wayland->factory = wl_registry_bind(registry, name, &kl_gpu_buffer_v1_interface, 1U);

	/* Succeeded: the factory is bound. */
	return;
}

/* Ignores globals that go away. */
static void
registry_remove(
	void *data,
	struct wl_registry *registry,
	uint32_t name)
{
	(void)data;
	(void)registry;
	(void)name;

	/* Succeeded: nothing the test uses goes away. */
	return;
}

/* Connects to the compositor and binds its GPU buffer factory; returns an errno value. */
static int
forge_connect(
	struct forge_wayland *wayland)
{
	int status;

	/* The compositor of WAYLAND_DISPLAY. */
	wayland->display = wl_display_connect(NULL);
	if (wayland->display == NULL)
		return ECONNREFUSED;

	/* Its registry. */
	wayland->registry = wl_display_get_registry(wayland->display);
	if (wayland->registry == NULL)
		return ENOMEM;

	/* Listens for the globals, and waits one round trip for them. */
	(void)wl_registry_add_listener(wayland->registry, &registry_listener, wayland);
	status = wl_display_roundtrip(wayland->display);
	if (status < 0)
		return EPROTO;

	/* A compositor without the factory takes no GPU buffer. */
	if (wayland->factory == NULL)
		return ENOENT;

	/* Succeeded: the factory is bound. */
	return 0;
}

/* Creates a 64x64 linear image and an exported allocation of its size; returns an errno value. */
static int
forge_export(
	struct forge_vulkan *vulkan,
	int *fd)
{
	static const char *const instance_extensions[] = {
		VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME,
		VK_KHR_EXTERNAL_MEMORY_CAPABILITIES_EXTENSION_NAME
	};
	static const char *const device_extensions[] = {
		VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME,
		VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME
	};
	VkInstanceCreateInfo instance;
	VkDeviceQueueCreateInfo queue;
	VkDeviceCreateInfo device;
	VkExternalMemoryImageCreateInfo external;
	VkImageCreateInfo image;
	VkImageSubresource subresource;
	VkExportMemoryAllocateInfo export;
	VkMemoryAllocateInfo allocate;
	VkMemoryGetFdInfoKHR get_fd;
	PFN_vkGetMemoryFdKHR get_memory_fd;
	uint32_t count;
	float priority;
	VkResult result;

	/* The instance with the external memory queries. */
	memset(&instance, 0, sizeof(instance));
	instance.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
	instance.enabledExtensionCount = 2U;
	instance.ppEnabledExtensionNames = instance_extensions;
	result = vkCreateInstance(&instance, NULL, &vulkan->instance);
	if (result != VK_SUCCESS)
		return ENODEV;

	/* The first physical device. */
	count = 1U;
	result = vkEnumeratePhysicalDevices(vulkan->instance, &count, &vulkan->physical);
	if (result != VK_SUCCESS && result != VK_INCOMPLETE)
		return ENODEV;

	/* A system without a device cannot run the test. */
	if (count == 0U)
		return ENODEV;

	/* One queue of family 0. */
	priority = 1.0f;
	memset(&queue, 0, sizeof(queue));
	queue.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
	queue.queueFamilyIndex = 0U;
	queue.queueCount = 1U;
	queue.pQueuePriorities = &priority;

	/* A device with external memory fds and that queue. */
	memset(&device, 0, sizeof(device));
	device.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
	device.queueCreateInfoCount = 1U;
	device.pQueueCreateInfos = &queue;
	device.enabledExtensionCount = 2U;
	device.ppEnabledExtensionNames = device_extensions;
	result = vkCreateDevice(vulkan->physical, &device, NULL, &vulkan->device);
	if (result != VK_SUCCESS)
		return ENODEV;

	/* The image may be shared through an fd. */
	memset(&external, 0, sizeof(external));
	external.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
	external.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;

	/* A 64x64 linear BGRA image. */
	memset(&image, 0, sizeof(image));
	image.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	image.pNext = &external;
	image.imageType = VK_IMAGE_TYPE_2D;
	image.format = VK_FORMAT_B8G8R8A8_UNORM;
	image.extent.width = 64U;
	image.extent.height = 64U;
	image.extent.depth = 1U;
	image.mipLevels = 1U;
	image.arrayLayers = 1U;
	image.samples = VK_SAMPLE_COUNT_1_BIT;
	image.tiling = VK_IMAGE_TILING_LINEAR;
	image.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
	image.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	image.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	result = vkCreateImage(vulkan->device, &image, NULL, &vulkan->image);
	if (result != VK_SUCCESS)
		return ENODEV;

	/* Reads the image's memory requirements. */
	vkGetImageMemoryRequirements(vulkan->device, vulkan->image, &vulkan->requirements);

	/* Finds the first memory type the image may use. */
	vulkan->memory_type = 0U;
	while (vulkan->memory_type < 32U) {
		/* A type the image may use ends the search. */
		if ((vulkan->requirements.memoryTypeBits & (1U << vulkan->memory_type)) != 0U)
			break;
		vulkan->memory_type++;
	}

	/* An image that may use no memory type cannot be allocated. */
	if (vulkan->memory_type == 32U)
		return ENODEV;

	/* Reads the row layout of the one color subresource. */
	memset(&subresource, 0, sizeof(subresource));
	subresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	vkGetImageSubresourceLayout(vulkan->device, vulkan->image, &subresource, &vulkan->layout);

	/* The allocation may be exported as an fd. */
	memset(&export, 0, sizeof(export));
	export.sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO;
	export.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;

	/* An allocation of the image's size. */
	memset(&allocate, 0, sizeof(allocate));
	allocate.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	allocate.pNext = &export;
	allocate.allocationSize = vulkan->requirements.size;
	allocate.memoryTypeIndex = vulkan->memory_type;
	result = vkAllocateMemory(vulkan->device, &allocate, NULL, &vulkan->memory);
	if (result != VK_SUCCESS)
		return ENOTSUP;

	/* Finds the extension's export entry point. */
	get_memory_fd = (PFN_vkGetMemoryFdKHR)vkGetDeviceProcAddr(vulkan->device, "vkGetMemoryFdKHR");
	if (get_memory_fd == NULL)
		return ENOTSUP;

	/* Exports the allocation's fd (an allocation capability, not an image capability). */
	memset(&get_fd, 0, sizeof(get_fd));
	get_fd.sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR;
	get_fd.memory = vulkan->memory;
	get_fd.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
	result = get_memory_fd(vulkan->device, &get_fd, fd);
	if (result != VK_SUCCESS)
		return ENOTSUP;

	/* Succeeded: the fd names an allocation of the image's size. */
	return 0;
}
