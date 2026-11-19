/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Applies standard global, instance, and enabled-device dispatch visibility.
 */

#include <string.h>
#include "internal.h"

/*
 * Immutable names and typed function addresses cover the selected public API.
 */
static const struct vulkan_entrypoint vulkan_entrypoints[] = {
#include "dispatch-table.inc"
};

static const struct vulkan_entrypoint *vulkan_entrypoint_find(const char *name);

/*
 * Tests instance extension enablement without consulting mutable host state.
 */
VkBool32
vulkan_instance_extension(
	struct VkInstance_T *instance,
	uint64_t bits)
{
	/* Refuses instance-scoped operations without an owning instance. */
	if (instance == NULL)
		return VK_FALSE;

	/* Requires every dependency requested by the caller's capability mask. */
	if ((instance->enabled_extensions & bits) != bits)
		return VK_FALSE;

	/* Succeeded: the requested instance extension dependencies are enabled. */
	return VK_TRUE;
}

/*
 * Tests the extensions enabled when a logical device was created.
 */
VkBool32
vulkan_device_extension(
	struct VkDevice_T *device,
	uint64_t bits)
{
	/* Refuses device-scoped operations without a logical device. */
	if (device == NULL)
		return VK_FALSE;

	/* Requires all requested extensions rather than any overlapping bit. */
	if ((device->enabled_extensions & bits) != bits)
		return VK_FALSE;

	/* Succeeded: the logical device enabled these extension commands. */
	return VK_TRUE;
}

/*
 * The core version in effect on an instance: the application's apiVersion
 * (1.0 when it names none), at most the library's own.  A command of a
 * later core than this is not visible through the instance.
 *
 * Procedure referenced: the Vulkan 1.1 specification (section 4.2,
 * VkApplicationInfo: the highest version the application uses; section
 * 3.1, vkGetInstanceProcAddr and vkGetDeviceProcAddr: a core command of a
 * version above the one supported gives no pointer).
 */
uint32_t
vulkan_instance_version(
	struct VkInstance_T *instance)
{
	uint32_t version;

	/* The major and minor of the application's version; the patch never decides a command. */
	version = VK_MAKE_VERSION(VK_API_VERSION_MAJOR(instance->api_version), VK_API_VERSION_MINOR(instance->api_version), 0);

	/* No more than the library implements. */
	if (version > VULKAN_LIBRARY_VERSION)
		version = VULKAN_LIBRARY_VERSION;

	/* Succeeded: the instance's core version. */
	return version;
}

/*
 * The core version in effect on a logical device: the lower of its
 * instance's and the version its physical device names.
 */
uint32_t
vulkan_device_version(
	struct VkDevice_T *device)
{
	struct VkPhysicalDevice_T *physical;
	uint32_t version;
	uint32_t named;

	/* The instance's version and the physical device's, without its patch. */
	physical = device->physical;
	version = vulkan_instance_version(physical->instance);
	named = VK_MAKE_VERSION(VK_API_VERSION_MAJOR(physical->properties.apiVersion), VK_API_VERSION_MINOR(physical->properties.apiVersion), 0);

	/* The lower of the two. */
	if (named < version)
		version = named;

	/* Succeeded: the device's core version. */
	return version;
}

/*
 * Resolves commands visible through a standard Vulkan instance.
 */
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vkGetInstanceProcAddr(
	VkInstance instance,
	const char *pName)
{
	const struct vulkan_entrypoint *entry;
	struct VkPhysicalDevice_T *physical;
	VkBool32 enabled;
	VkBool32 supported;
	uint32_t version;
	uint32_t index;

	/* Leaves unknown and absent command names unresolved. */
	entry = vulkan_entrypoint_find(pName);
	if (entry == NULL)
		return NULL;

	/* Global commands remain available before any instance exists, those the library implements. */
	if (entry->scope == VULKAN_ENTRY_GLOBAL) {
		if (entry->version > VULKAN_LIBRARY_VERSION)
			return NULL;

		/* Succeeded: a global command of an implemented core. */
		return entry->function;
	}

	/* Other command domains require an actual owning instance. */
	if (instance == NULL)
		return NULL;

	/* A command of a later core than the instance's is not visible. */
	version = vulkan_instance_version(instance);
	if (entry->version > version)
		return NULL;

	/* Vulkan 1.0 core commands have no extension enablement requirement. */
	if (entry->extension == 0 && entry->version == VULKAN_CORE_1_0)
		return entry->function;

	/* Instance and physical-device commands of a later core need nothing more than the instance's version. */
	if (entry->extension == 0 && entry->scope != VULKAN_ENTRY_DEVICE)
		return entry->function;

	/* A device command of a later core is visible when any owned physical device names that version. */
	if (entry->extension == 0) {
		supported = VK_FALSE;
		for (index = 0; index < instance->physical_device_count; index++) {
			physical = instance->physical_devices[index];

			/* The version the physical device names, decided from its renderer's answers. */
			if (physical->properties.apiVersion >= entry->version) {
				supported = VK_TRUE;
				break;
			}
		}

		/* Refuses a core command no owned physical device offers. */
		if (!supported)
			return NULL;

		/* Succeeded: at least one physical device names this command's version. */
		return entry->function;
	}

	/* Instance extensions are visible only when enabled on this instance. */
	if (entry->scope == VULKAN_ENTRY_INSTANCE) {
		enabled = vulkan_instance_extension(instance, entry->extension);
		if (!enabled)
			return NULL;

		/* Succeeded: returns a command of an enabled instance extension. */
		return entry->function;
	}

	/*
	 * Device extension commands, and physical-device commands of a device
	 * extension, are visible when any owned physical device supports them.
	 */
	supported = VK_FALSE;
	for (index = 0; index < instance->physical_device_count; index++) {
		physical = instance->physical_devices[index];

		/* Keeps backend-dependent extension availability tied to real physical devices. */
		if ((physical->supported_extensions & entry->extension) == entry->extension) {
			supported = VK_TRUE;
			break;
		}
	}

	/* Refuses an extension unavailable from every owned physical device. */
	if (!supported)
		return NULL;

	/* Succeeded: at least one physical device provides this extension command. */
	return entry->function;
}

/*
 * Resolves commands enabled for one standard Vulkan logical device.
 */
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vkGetDeviceProcAddr(
	VkDevice device,
	const char *pName)
{
	const struct vulkan_entrypoint *entry;
	VkBool32 enabled;
	uint32_t version;

	/* Device lookup never substitutes a global or instance dispatch object. */
	if (device == NULL)
		return NULL;

	/* Leaves unknown command names unresolved. */
	entry = vulkan_entrypoint_find(pName);
	if (entry == NULL)
		return NULL;

	/* Instance and global commands cannot be invoked through device dispatch. */
	if (entry->scope != VULKAN_ENTRY_DEVICE)
		return NULL;

	/* A command of a later core than the device's version is not visible. */
	version = vulkan_device_version(vulkan_device(device));
	if (entry->version > version)
		return NULL;

	/* Extension commands require explicit logical-device enablement. */
	enabled = vulkan_device_extension(device, entry->extension);
	if (!enabled)
		return NULL;

	/* Succeeded: returns the device command with its standard function signature. */
	return entry->function;
}

/* Finds one immutable public command record by its exact standard spelling. */
static const struct vulkan_entrypoint *
vulkan_entrypoint_find(
	const char *name)
{
	size_t index;
	int comparison;
	const struct vulkan_entrypoint *entry;

	/* Refuses an absent name without asking string routines to read it. */
	if (name == NULL)
		return NULL;

	/* Searches the finite API name set independently of object allocation count. */
	entry = NULL;
	for (index = 0; index < sizeof(vulkan_entrypoints) / sizeof(vulkan_entrypoints[0]); index++) {
		comparison = strcmp(name, vulkan_entrypoints[index].name);
		if (comparison == 0) {
			entry = &vulkan_entrypoints[index];
			break;
		}
	}

	/* Refuses names absent from the selected public API. */
	if (entry == NULL)
		return NULL;

	/* Succeeded: returns the immutable record for this exact command name. */
	return entry;
}
