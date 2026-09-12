/* -*- mode: c; c-file-style: "linux"; tab-width: 8; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* Verifies real full-library function lookup with independent standard API scopes. */

#include <assert.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "internal.h"

/* These explicit standard domains do not read implementation dispatch metadata. */
enum test_scope {
	TEST_GLOBAL,
	TEST_INSTANCE,
	TEST_DEVICE
};

/* Extension ownership remains separate from global, instance and device command scope. */
enum test_extension {
	TEST_CORE,
	TEST_SURFACE,
	TEST_DISPLAY,
	TEST_SWAPCHAIN,
	TEST_DISPLAY_SWAPCHAIN
};

/* Vulkan 1.0 defines exactly four commands callable without an instance. */
static const char *const global_names[] = {
	"vkCreateInstance", "vkEnumerateInstanceExtensionProperties",
	"vkEnumerateInstanceLayerProperties", "vkGetInstanceProcAddr"
};

/* All remaining non-device Vulkan 1.0 commands have an instance or physical-device first argument. */
static const char *const instance_names[] = {
	"vkDestroyInstance", "vkEnumeratePhysicalDevices",
	"vkGetPhysicalDeviceFeatures", "vkGetPhysicalDeviceFormatProperties",
	"vkGetPhysicalDeviceImageFormatProperties", "vkGetPhysicalDeviceProperties",
	"vkGetPhysicalDeviceQueueFamilyProperties", "vkGetPhysicalDeviceMemoryProperties",
	"vkCreateDevice", "vkEnumerateDeviceExtensionProperties",
	"vkEnumerateDeviceLayerProperties", "vkGetPhysicalDeviceSparseImageFormatProperties"
};

/* KHR_surface command availability is tied to instance extension enablement. */
static const char *const surface_names[] = {
	"vkDestroySurfaceKHR", "vkGetPhysicalDeviceSurfaceSupportKHR",
	"vkGetPhysicalDeviceSurfaceCapabilitiesKHR", "vkGetPhysicalDeviceSurfaceFormatsKHR",
	"vkGetPhysicalDeviceSurfacePresentModesKHR"
};

/* KHR_display exposes physical display and display-surface discovery through the instance. */
static const char *const display_names[] = {
	"vkGetPhysicalDeviceDisplayPropertiesKHR", "vkGetPhysicalDeviceDisplayPlanePropertiesKHR",
	"vkGetDisplayPlaneSupportedDisplaysKHR", "vkGetDisplayModePropertiesKHR",
	"vkCreateDisplayModeKHR", "vkGetDisplayPlaneCapabilitiesKHR",
	"vkCreateDisplayPlaneSurfaceKHR"
};

/* KHR_swapchain commands are device commands despite being discoverable through an instance. */
static const char *const swapchain_names[] = {
	"vkCreateSwapchainKHR", "vkDestroySwapchainKHR", "vkGetSwapchainImagesKHR",
	"vkAcquireNextImageKHR", "vkQueuePresentKHR"
};

static VkBool32 name_in(const char *name, const char *const *names, size_t count);
static enum test_scope classify(const char *name, enum test_extension *extension);
static void check_entry(void *library, const char *name, enum test_scope scope, enum test_extension extension, struct VkInstance_T *instance, struct VkDevice_T *device, struct VkPhysicalDevice_T *physical);
static void check_address(PFN_vkVoidFunction actual, PFN_vkVoidFunction expected, const char *name);
static void check_dependencies(struct VkInstance_T *instance, struct VkDevice_T *device);
static void check_unknown(struct VkInstance_T *instance, struct VkDevice_T *device);

/*
 * Loads the actual DSO containing all 18 implementation families without opening a GPU.
 */
int
main(
	int argc,
	char **argv)
{
	struct VkInstance_T instance;
	struct VkDevice_T device;
	struct VkPhysicalDevice_T physical[2];
	struct VkPhysicalDevice_T *physical_array[2];
	FILE *names;
	void *library;
	char line[256];
	char name[128];
	char seen[155][128];
	char *read_result;
	const char *error;
	enum test_scope scope;
	enum test_extension extension;
	unsigned counts[5][3];
	unsigned total;
	unsigned index;
	int comparison;
	int result;

	/* The caller supplies the actual shared library and a names-only enumeration source. */
	assert(argc == 3);
	library = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
	if (library == NULL) {
		error = dlerror();
		fprintf(stderr, "full-library load failed: %s\n", error);
		return 1;
	}

	/* Properly related dispatch objects expose supported versus enabled extension differences. */
	memset(&instance, 0, sizeof(instance));
	memset(&device, 0, sizeof(device));
	memset(physical, 0, sizeof(physical));
	memset(counts, 0, sizeof(counts));
	instance.object.kind = VULKAN_OBJECT_INSTANCE;
	instance.api_version = VK_API_VERSION_1_0;
	device.object.kind = VULKAN_OBJECT_DEVICE;
	device.object.parent = &instance.object;
	physical[0].object.kind = VULKAN_OBJECT_PHYSICAL_DEVICE;
	physical[0].object.parent = &instance.object;
	physical[1].object.kind = VULKAN_OBJECT_PHYSICAL_DEVICE;
	physical[1].object.parent = &instance.object;
	physical[0].instance = &instance;
	physical[1].instance = &instance;
	physical_array[0] = &physical[0];
	physical_array[1] = &physical[1];
	instance.physical_devices = physical_array;
	instance.physical_device_count = 2U;
	device.physical = &physical[0];
	names = fopen(argv[2], "r");
	assert(names != NULL);
	read_result = fgets(line, sizeof(line), names);
	assert(read_result != NULL);
	result = sscanf(line, "%127s", name);
	assert(result == 1);
	comparison = strcmp(name, "name");
	assert(comparison == 0);
	total = 0U;

	/* Only the first column enumerates names; scopes and extension expectations come from explicit lists above. */
	for (;;) {
		read_result = fgets(line, sizeof(line), names);
		if (read_result == NULL)
			break;

		/* Duplicate metadata names cannot conceal a missing public command. */
		result = sscanf(line, "%127s", name);
		assert(result == 1);
		assert(total < 155U);
		for (index = 0U; index < total; index++) {
			comparison = strcmp(seen[index], name);
			assert(comparison != 0);
		}

		/* Every selected name must resolve to the real exported implementation with standard visibility. */
		strcpy(seen[total], name);
		scope = classify(name, &extension);
		counts[extension][scope]++;
		check_entry(library, name, scope, extension, &instance, &device, physical);
		total++;
	}

	/* The independent domain totals enforce all 137 core and 18 selected extension commands. */
	assert(total == 155U);
	assert(counts[TEST_CORE][TEST_GLOBAL] == 4U);
	assert(counts[TEST_CORE][TEST_INSTANCE] == 12U);
	assert(counts[TEST_CORE][TEST_DEVICE] == 121U);
	assert(counts[TEST_SURFACE][TEST_INSTANCE] == 5U);
	assert(counts[TEST_DISPLAY][TEST_INSTANCE] == 7U);
	assert(counts[TEST_SWAPCHAIN][TEST_DEVICE] == 5U);
	assert(counts[TEST_DISPLAY_SWAPCHAIN][TEST_DEVICE] == 1U);
	check_dependencies(&instance, &device);
	check_unknown(&instance, &device);
	result = fclose(names);
	assert(result == 0);
	result = dlclose(library);
	assert(result == 0);

	/* Succeeded: function addresses originate from the actual complete library, with no substituted Vulkan entry point. */
	puts("libvulkan dispatch: PASS (155 real exports, 137 core, 18 WSI, independent scopes, extension gating)");
	return 0;
}

/* Finds exact names only in the independently maintained standard-domain lists. */
static VkBool32
name_in(
	const char *name,
	const char *const *names,
	size_t count)
{
	size_t index;
	int comparison;

	/* Prefixes and neighboring command names do not belong to the same lookup entry. */
	for (index = 0U; index < count; index++) {
		comparison = strcmp(name, names[index]);
		if (comparison == 0)
			return VK_TRUE;
	}

	/* The command belongs to another explicitly defined domain. */
	return VK_FALSE;
}

/* Classifies standard commands without reading dispatch-table scope or extension columns. */
static enum test_scope
classify(
	const char *name,
	enum test_extension *extension)
{
	VkBool32 found;
	const char *suffix;
	int comparison;

	/* Four standard global commands remain usable before vkCreateInstance. */
	*extension = TEST_CORE;
	found = name_in(name, global_names, sizeof(global_names) / sizeof(global_names[0]));
	if (found)
		return TEST_GLOBAL;

	/* Instance and physical-device core commands share instance dispatch scope. */
	found = name_in(name, instance_names, sizeof(instance_names) / sizeof(instance_names[0]));
	if (found)
		return TEST_INSTANCE;

	/* The selected surface and direct-display extensions belong to instance scope. */
	found = name_in(name, surface_names, sizeof(surface_names) / sizeof(surface_names[0]));
	if (found) {
		*extension = TEST_SURFACE;
		return TEST_INSTANCE;
	}

	/* Direct-display discovery and surface creation require KHR_display enablement. */
	found = name_in(name, display_names, sizeof(display_names) / sizeof(display_names[0]));
	if (found) {
		*extension = TEST_DISPLAY;
		return TEST_INSTANCE;
	}

	/* Ordinary swapchain operations belong to an enabled logical device. */
	found = name_in(name, swapchain_names, sizeof(swapchain_names) / sizeof(swapchain_names[0]));
	if (found) {
		*extension = TEST_SWAPCHAIN;
		return TEST_DEVICE;
	}

	/* Shared direct-display swapchains add exactly one selected device extension command. */
	comparison = strcmp(name, "vkCreateSharedSwapchainsKHR");
	if (comparison == 0) {
		*extension = TEST_DISPLAY_SWAPCHAIN;
		return TEST_DEVICE;
	}

	/* Every other enumerated core command has a device, queue or command-buffer first argument. */
	suffix = strstr(name, "KHR");
	assert(suffix == NULL);
	return TEST_DEVICE;
}

/* Compares actual exported addresses against lookup under independent supported/enabled combinations. */
static void
check_entry(
	void *library,
	const char *name,
	enum test_scope scope,
	enum test_extension extension,
	struct VkInstance_T *instance,
	struct VkDevice_T *device,
	struct VkPhysicalDevice_T *physical)
{
	void *symbol;
	PFN_vkVoidFunction exported;
	PFN_vkVoidFunction actual;
	PFN_vkVoidFunction expected;
	uint64_t required;
	unsigned instance_mask;
	unsigned device_mask;
	unsigned support_mask;
	unsigned supported_index;

	/* POSIX dlsym supplies the real implementation address without introducing a Vulkan mock. */
	symbol = dlsym(library, name);
	assert(symbol != NULL);
	assert(sizeof(exported) == sizeof(symbol));
	memcpy(&exported, &symbol, sizeof(exported));
	expected = NULL;

	/* A NULL instance exposes only standard global commands. */
	if (scope == TEST_GLOBAL)
		expected = exported;

	/* Device lookup with a NULL device never resolves global, instance or device commands. */
	actual = vkGetInstanceProcAddr(NULL, name);
	check_address(actual, expected, name);
	actual = vkGetDeviceProcAddr(NULL, name);
	check_address(actual, NULL, name);
	required = 0U;

	/* Expected extension masks are independently selected from the public ownership domain. */
	switch (extension) {
	case TEST_SURFACE:
		required = VULKAN_INSTANCE_SURFACE;
		break;
	case TEST_DISPLAY:
		required = VULKAN_INSTANCE_DISPLAY;
		break;
	case TEST_SWAPCHAIN:
		required = VULKAN_DEVICE_SWAPCHAIN;
		break;
	case TEST_DISPLAY_SWAPCHAIN:
		required = VULKAN_DEVICE_DISPLAY_SWAPCHAIN;
		break;
	default:
		/* Core commands require no extension capability. */
		break;
	}

	/* Test every two-bit combination, including support on either physical device independently. */
	for (instance_mask = 0U; instance_mask < 4U; instance_mask++) {
		instance->enabled_extensions = instance_mask;
		for (device_mask = 0U; device_mask < 4U; device_mask++) {
			device->enabled_extensions = device_mask;
			for (support_mask = 0U; support_mask < 4U; support_mask++) {
				for (supported_index = 0U; supported_index < 2U; supported_index++) {
					physical[0].supported_extensions = 0U;
					physical[1].supported_extensions = 0U;
					physical[supported_index].supported_extensions = support_mask;
					expected = exported;

					/* Instance extensions require enablement, while device extensions require any-physical support. */
					if (extension != TEST_CORE) {
						if (scope == TEST_INSTANCE) {
							if ((instance_mask & required) != required)
								expected = NULL;
						} else {
							if ((support_mask & required) != required)
								expected = NULL;
						}
					}

					/* A logical device's enabled extensions do not restrict instance lookup on other physical devices. */
					actual = vkGetInstanceProcAddr(instance, name);
					check_address(actual, expected, name);
					expected = NULL;

					/* Device commands depend only on their logical-device enablement, excluding global and instance domains. */
					if (scope == TEST_DEVICE) {
						if ((device_mask & required) == required)
							expected = exported;
					}

					/* Actual lookup must return the same complete implementation as the exported symbol. */
					actual = vkGetDeviceProcAddr(device, name);
					check_address(actual, expected, name);
				}
			}
		}
	}

	/* No physical-device support means device extensions are absent even with instance extensions enabled. */
	instance->physical_device_count = 0U;
	expected = exported;
	if (scope == TEST_DEVICE) {
		if (extension != TEST_CORE)
			expected = NULL;
	}

	/* An empty physical-device collection still permits ordinary global, instance and core-device lookup. */
	actual = vkGetInstanceProcAddr(instance, name);
	check_address(actual, expected, name);
	instance->physical_device_count = 2U;

	/* Succeeded: this actual entry point obeys every independent visibility combination. */
	return;
}

/* Reports the failing public name before asserting an exact function-address comparison. */
static void
check_address(
	PFN_vkVoidFunction actual,
	PFN_vkVoidFunction expected,
	const char *name)
{
	/* A named failure makes a scope or wrong-symbol regression actionable. */
	if (actual != expected)
		fprintf(stderr, "dispatch address mismatch: %s\n", name);

	/* Succeeded: the selected address is exactly the intended native public implementation. */
	assert(actual == expected);
	return;
}

/* Verifies dependency masks require all requested extension bits, including the empty mask. */
static void
check_dependencies(
	struct VkInstance_T *instance,
	struct VkDevice_T *device)
{
	unsigned mask;
	VkBool32 expected;
	VkBool32 actual;

	/* Either bit alone cannot satisfy a request for both dependent extension capabilities. */
	for (mask = 0U; mask < 4U; mask++) {
		instance->enabled_extensions = mask;
		device->enabled_extensions = mask;
		expected = VK_FALSE;

		/* Both standard dependencies must be enabled together for the combined check. */
		if (mask == 3U)
			expected = VK_TRUE;

		/* Core command checks use an empty dependency mask on a valid owner. */
		actual = vulkan_instance_extension(instance, VULKAN_INSTANCE_SURFACE | VULKAN_INSTANCE_DISPLAY);
		assert(actual == expected);
		actual = vulkan_device_extension(device, VULKAN_DEVICE_SWAPCHAIN | VULKAN_DEVICE_DISPLAY_SWAPCHAIN);
		assert(actual == expected);
		actual = vulkan_instance_extension(instance, 0U);
		assert(actual == VK_TRUE);
		actual = vulkan_device_extension(device, 0U);
		assert(actual == VK_TRUE);
	}

	/* A missing owner cannot satisfy even an empty extension request. */
	actual = vulkan_instance_extension(NULL, 0U);
	assert(actual == VK_FALSE);
	actual = vulkan_device_extension(NULL, 0U);
	assert(actual == VK_FALSE);

	/* Succeeded: overlapping bits cannot accidentally enable a dependent operation. */
	return;
}

/* Rejects NULL, unknown, case-changed and prefix/suffix-neighbor public names. */
static void
check_unknown(
	struct VkInstance_T *instance,
	struct VkDevice_T *device)
{
	static const char *const names[] = { NULL, "", "vk", "vkCreateBuffe", "vkCreateBufferKHR", "vkcreateBuffer", "vkCreateBuffer ", "vkCreateBufferExtra", "vkUnknownCommand" };
	PFN_vkVoidFunction actual;
	size_t index;

	/* Exact public spelling is required in both lookup domains. */
	for (index = 0U; index < sizeof(names) / sizeof(names[0]); index++) {
		actual = vkGetInstanceProcAddr(instance, names[index]);
		check_address(actual, NULL, "unknown instance name");
		actual = vkGetDeviceProcAddr(device, names[index]);
		check_address(actual, NULL, "unknown device name");
		actual = vkGetInstanceProcAddr(NULL, names[index]);
		check_address(actual, NULL, "unknown global name");
	}

	/* Succeeded: neighboring or absent names cannot resolve an unrelated command. */
	return;
}
