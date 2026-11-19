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

/* The versions the rule names. */
#define VERSION_1_0			VK_MAKE_VERSION(1, 0, 0)
#define VERSION_1_1			VK_MAKE_VERSION(1, 1, 0)

static void reason_add(const char **reasons, uint32_t capacity, uint32_t *count, const char *reason);

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

	/* Succeeded: the device names Vulkan 1.1. */
	return VERSION_1_1;
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
