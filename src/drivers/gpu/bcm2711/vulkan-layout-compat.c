/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* Immutable native layout definitions preserve ordinary graphics descriptor and push compatibility. */
#include <uapi/errno.h>

#include "drivers/gpu/bcm2711/vulkan-layout.h"

/*
 * Compares canonical single-element set definitions independently of their public identities.
 */
int
bcm2711_vulkan_layout_set_compatible(
	const struct bcm2711_vulkan_set_layout *first,
	const struct bcm2711_vulkan_set_layout *second)
{
	const struct bcm2711_vulkan_binding_layout *left;
	const struct bcm2711_vulkan_binding_layout *right;
	uint32_t index;

	/* Canonical order removes declaration order while preserving immutable sampler object identity. */
	if (first->count != second->count)
		return EINVAL;
	for (index = 0; index < first->count; index++) {
		left = &first->bindings[index];
		right = &second->bindings[index];
		if (left->number != right->number ||
		    left->type != right->type ||
		    left->stages != right->stages ||
		    left->immutable != right->immutable)
			return EINVAL;
	}

	/* Succeeded: both retained definitions identify the same finite descriptor interface. */
	return 0;
}

/*
 * Compares exact push range collections without confusing combined and separate stage declarations.
 */
int
bcm2711_vulkan_layout_push_compatible(
	const struct bcm2711_vulkan_pipeline_layout *first,
	const struct bcm2711_vulkan_pipeline_layout *second)
{
	const VkPushConstantRange *left;
	const VkPushConstantRange *right;
	uint32_t index;
	uint32_t candidate;
	bool found;

	/* Range collections contain at most one declaration for each implemented stage. */
	if (first->range_count != second->range_count)
		return EINVAL;
	for (index = 0; index < first->range_count; index++) {
		left = &first->ranges[index];
		found = false;
		for (candidate = 0; candidate < second->range_count; candidate++) {
			right = &second->ranges[candidate];
			if (left->stageFlags == right->stageFlags &&
			    left->offset == right->offset &&
			    left->size == right->size) {
				found = true;
				break;
			}
		}

		/* A missing exact range prevents use of previously pushed values. */
		if (!found)
			return EINVAL;
	}

	/* Succeeded: declaration order differs at most, and all exact push ranges agree. */
	return 0;
}

/*
 * Checks push ranges and every descriptor prefix definition required for compatibility at one set number.
 */
int
bcm2711_vulkan_layout_compatible(
	const struct bcm2711_vulkan_pipeline_layout *first,
	const struct bcm2711_vulkan_pipeline_layout *second,
	uint32_t set)
{
	uint32_t index;
	int error;

	/* Both retained interfaces must actually declare the complete selected prefix. */
	if (set >= first->count || set >= second->count)
		return EINVAL;

	/* Ordinary layouts include exact push ranges in descriptor prefix compatibility. */
	error = bcm2711_vulkan_layout_push_compatible(first, second);
	if (error != 0)
		return error;

	/* Each prefix definition can match another independently created but identical retained layout. */
	for (index = 0; index <= set; index++) {
		error = bcm2711_vulkan_layout_set_compatible(first->sets[index]->payload, second->sets[index]->payload);
		if (error != 0)
			return error;
	}

	/* Succeeded: a descriptor selected using either layout has the same pipeline-visible prefix. */
	return 0;
}
