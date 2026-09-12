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
	uint32_t index;

	/* Leaves unknown and absent command names unresolved. */
	entry = vulkan_entrypoint_find(pName);
	if (entry == NULL)
		return NULL;

	/* Global commands remain available before any instance exists. */
	if (entry->scope == VULKAN_ENTRY_GLOBAL)
		return entry->function;

	/* Other command domains require an actual owning instance. */
	if (instance == NULL)
		return NULL;

	/* Vulkan 1.0 core commands have no extension enablement requirement. */
	if (entry->extension == 0)
		return entry->function;

	/* Instance extensions are visible only when enabled on this instance. */
	if (entry->scope == VULKAN_ENTRY_INSTANCE) {
		enabled = vulkan_instance_extension(instance, entry->extension);
		if (!enabled)
			return NULL;

		/* Succeeded: returns a command of an enabled instance extension. */
		return entry->function;
	}

	/* Device extension commands are visible when any owned physical device supports them. */
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
