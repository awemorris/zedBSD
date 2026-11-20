/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Decides the Vulkan version a physical device names from what its
 * renderer reports, the same rule for every GPU.
 */

#include <stddef.h>
#include <stdint.h>

#include "version.h"

/*
 * The least values Vulkan 1.1 requires of the properties a renderer
 * reports: a subgroup of one invocation or more whose basic operations
 * run in compute shaders, six views and 2^27 - 1 instances of multiview,
 * 1024 descriptors in a set, and 2^30 bytes in one allocation.
 *
 * Values referenced: the Vulkan 1.1 specification (section 39.1, Feature
 * Requirements: multiview; section 40.1, Required Limits:
 * maxMultiviewViewCount 6, maxMultiviewInstanceIndex 2^27 - 1,
 * maxPerSetDescriptors 1024, maxMemoryAllocationSize 2^30; section 9.25,
 * VkPhysicalDeviceSubgroupProperties: supportedStages includes
 * VK_SHADER_STAGE_COMPUTE_BIT and supportedOperations
 * VK_SUBGROUP_FEATURE_BASIC_BIT, subgroupSize at least 1).
 */
#define CORE11_VIEW_COUNT		6U
#define CORE11_INSTANCE_INDEX		((1U << 27) - 1U)
#define CORE11_SET_DESCRIPTORS		1024U
#define CORE11_ALLOCATION_BYTES		(UINT64_C(1) << 30)
#define CORE11_SUBGROUP_BASIC		0x00000001U

/*
 * The least values Vulkan 1.2 requires of the properties a renderer
 * reports: a depth and a stencil resolve that take sample 0, and a single
 * sample of an integer colour attachment in a framebuffer.
 *
 * Values referenced: the Vulkan 1.2 specification (section 40.1, Required
 * Limits: supportedDepthResolveModes and supportedStencilResolveModes
 * include VK_RESOLVE_MODE_SAMPLE_ZERO_BIT, framebufferIntegerColorSampleCounts
 * includes VK_SAMPLE_COUNT_1_BIT; section 39.1, Feature Requirements:
 * subgroupBroadcastDynamicId, uniformBufferStandardLayout and
 * shaderSubgroupExtendedTypes, which the renderer's shaders carry, and
 * imagelessFramebuffer, separateDepthStencilLayouts, hostQueryReset and
 * timelineSemaphore, which this library makes).
 */
#define CORE12_RESOLVE_SAMPLE_ZERO	0x00000001U
#define CORE12_SAMPLE_COUNT_1		0x00000001U

/* The versions the rule names. */
#define VERSION_1_0			VK_MAKE_VERSION(1, 0, 0)
#define VERSION_1_1			VK_MAKE_VERSION(1, 1, 0)
#define VERSION_1_2			VK_MAKE_VERSION(1, 2, 0)

static void reason_add(const char **reasons, uint32_t capacity, uint32_t *count, const char *reason);
static void version_core12_reasons(const struct vulkan_core11_facts *facts, const struct vulkan_core12_facts *core12, const char **reasons, uint32_t capacity, uint32_t *count);

/*
 * Decides the version a physical device names: the highest of the
 * library's versions (library is the highest it implements) whose every
 * requirement the facts meet, never below 1.0, which every device names.
 * Writes to reasons (at most capacity) why the version above the one
 * decided is not named, and to count the number of every such reason,
 * which may be more than capacity.
 */
uint32_t
vulkan_version_decide(
	uint32_t library,
	const struct vulkan_core11_facts *facts,
	const struct vulkan_core12_facts *core12,
	const char **reasons,
	uint32_t capacity,
	uint32_t *count)
{
	/* No reason yet. */
	*count = 0U;

	/* A library of Vulkan 1.0 names nothing later, whatever the renderer has. */
	if (library < VERSION_1_1) {
		reason_add(reasons, capacity, count, "the library implements Vulkan 1.0 only");
		return VERSION_1_0;
	}

	/* The renderer's own version and its answers to the 1.1 queries. */
	if (facts->api_version < VERSION_1_1)
		reason_add(reasons, capacity, count, "the renderer's apiVersion is below 1.1");
	if (!facts->reported)
		reason_add(reasons, capacity, count, "the renderer does not answer the Vulkan 1.1 feature and property queries");

	/* The one feature Vulkan 1.1 requires. */
	if (facts->reported && !facts->multiview)
		reason_add(reasons, capacity, count, "no multiview (a feature Vulkan 1.1 requires)");

	/* The subgroup's basic operations in compute shaders. */
	if (facts->reported && facts->subgroup_size == 0U)
		reason_add(reasons, capacity, count, "a subgroup size of 0");
	if (facts->reported && (facts->subgroup_stages & VK_SHADER_STAGE_COMPUTE_BIT) == 0U)
		reason_add(reasons, capacity, count, "no subgroup operations in compute shaders");
	if (facts->reported && (facts->subgroup_operations & CORE11_SUBGROUP_BASIC) == 0U)
		reason_add(reasons, capacity, count, "no basic subgroup operations");

	/* The limits Vulkan 1.1 raises or adds. */
	if (facts->reported && facts->max_multiview_view_count < CORE11_VIEW_COUNT)
		reason_add(reasons, capacity, count, "maxMultiviewViewCount below 6");
	if (facts->reported && facts->max_multiview_instance_index < CORE11_INSTANCE_INDEX)
		reason_add(reasons, capacity, count, "maxMultiviewInstanceIndex below 2^27 - 1");
	if (facts->reported && facts->max_per_set_descriptors < CORE11_SET_DESCRIPTORS)
		reason_add(reasons, capacity, count, "maxPerSetDescriptors below 1024");
	if (facts->reported && facts->max_memory_allocation_size < CORE11_ALLOCATION_BYTES)
		reason_add(reasons, capacity, count, "maxMemoryAllocationSize below 2^30");

	/* Any requirement unmet keeps the device at Vulkan 1.0. */
	if (*count != 0U)
		return VERSION_1_0;

	/* A library of Vulkan 1.1 names nothing later. */
	if (library < VERSION_1_2)
		return VERSION_1_1;

	/* Any requirement of Vulkan 1.2 unmet keeps the device at Vulkan 1.1. */
	version_core12_reasons(facts, core12, reasons, capacity, count);
	if (*count != 0U)
		return VERSION_1_1;

	/* Succeeded: the device names Vulkan 1.2. */
	return VERSION_1_2;
}

/*
 * Gives the reasons a device that meets Vulkan 1.1 does not name 1.2: the
 * renderer's own version, its answers to the 1.2 queries, the features of
 * Vulkan 1.2 that its shaders carry and the limits 1.2 raises.
 */
static void
version_core12_reasons(
	const struct vulkan_core11_facts *facts,
	const struct vulkan_core12_facts *core12,
	const char **reasons,
	uint32_t capacity,
	uint32_t *count)
{
	const VkPhysicalDeviceVulkan12Features *features;
	const VkPhysicalDeviceVulkan12Properties *properties;

	/* The renderer's own version and its answers to the 1.2 queries. */
	if (facts->api_version < VERSION_1_2)
		reason_add(reasons, capacity, count, "the renderer's apiVersion is below 1.2");
	if (!core12->reported) {
		reason_add(reasons, capacity, count, "the renderer does not answer the Vulkan 1.2 feature and property queries");
		return;
	}

	/* The features Vulkan 1.2 requires of the renderer's shaders. */
	features = &core12->features;
	if (!features->subgroupBroadcastDynamicId)
		reason_add(reasons, capacity, count, "no subgroupBroadcastDynamicId (a feature Vulkan 1.2 requires)");
	if (!features->uniformBufferStandardLayout)
		reason_add(reasons, capacity, count, "no uniformBufferStandardLayout (a feature Vulkan 1.2 requires)");
	if (!features->shaderSubgroupExtendedTypes)
		reason_add(reasons, capacity, count, "no shaderSubgroupExtendedTypes (a feature Vulkan 1.2 requires)");

	/* The limits Vulkan 1.2 adds. */
	properties = &core12->properties;
	if ((properties->supportedDepthResolveModes & CORE12_RESOLVE_SAMPLE_ZERO) == 0U)
		reason_add(reasons, capacity, count, "no depth resolve of sample 0");
	if ((properties->supportedStencilResolveModes & CORE12_RESOLVE_SAMPLE_ZERO) == 0U)
		reason_add(reasons, capacity, count, "no stencil resolve of sample 0");
	if ((properties->framebufferIntegerColorSampleCounts & CORE12_SAMPLE_COUNT_1) == 0U)
		reason_add(reasons, capacity, count, "framebufferIntegerColorSampleCounts without 1 sample");

	/* Succeeded: every reason is counted. */
	return;
}

/* Adds one reason to the list while it has room; the count goes on regardless. */
static void
reason_add(
	const char **reasons,
	uint32_t capacity,
	uint32_t *count,
	const char *reason)
{
	/* The caller's room. */
	if (*count < capacity)
		reasons[*count] = reason;

	/* Succeeded: the reason is counted. */
	(*count)++;
}
