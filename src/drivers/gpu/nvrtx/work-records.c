/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The work channel's arithmetic and push buffers, which touch no hardware
 * (a host test runs them).
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "drivers/gpu/nvrtx/nvrtx-gsp.h"
#include "drivers/gpu/nvrtx/nvrtx-work.h"

/*
 * A method header: incrementing, then the count, the subchannel and the
 * method's word address.
 *
 * Values referenced: Mesa 25.3.6 src/nouveau/headers/nv_push.h
 * (NVC0_FIFO_PKHDR_SQ).
 */
#define WORK_METHOD_INCREMENT		(1U << 29)
#define WORK_METHOD_COUNT_SHIFT		16U
#define WORK_METHOD_SUBCHANNEL_SHIFT	13U

/*
 * The method that binds a class to a subchannel.
 *
 * Value referenced: NVIDIA open-gpu-doc classes/host/clc46f.h
 * (NVC46F_SET_OBJECT).
 */
#define WORK_SET_OBJECT			0x0000U

static size_t work_method(uint8_t *push, size_t at, uint32_t subchannel, uint32_t method, uint32_t count);

/*
 * Reports whether the semaphore's value has reached a fence's.
 *
 * Fence values grow by one and wrap; a value at most half the range ahead
 * of what the semaphore holds has not passed yet.
 */
bool
nvrtx_work_fence_passed(
	uint32_t seen,
	uint32_t value)
{
	int32_t ahead;

	/* How far the fence lies ahead of the semaphore, as a signed distance. */
	ahead = (int32_t)(value - seen);

	/* A fence ahead of the semaphore has not passed. */
	if (ahead > 0)
		return false;

	/* Succeeded: the semaphore is at or past the fence. */
	return true;
}

/*
 * Writes the push buffer that binds the 3D and compute classes to their
 * subchannels (0 and 1).
 *
 * Returns the length in bytes.
 *
 * Procedure referenced: Mesa 25.3.6 src/nouveau/vulkan/nvk_queue.c
 * (nvk_queue_init_context_state).
 */
size_t
nvrtx_work_push_objects(
	uint8_t *push,
	uint32_t threed_class,
	uint32_t compute_class)
{
	size_t at;

	/* The 3D class on its subchannel. */
	at = work_method(push, 0U, NVRTX_WORK_SUBCHANNEL_THREED, WORK_SET_OBJECT, 1U);
	nvrtx_put32(push, at, threed_class);
	at += 4U;

	/* The compute class on its subchannel. */
	at = work_method(push, at, NVRTX_WORK_SUBCHANNEL_COMPUTE, WORK_SET_OBJECT, 1U);
	nvrtx_put32(push, at, compute_class);
	at += 4U;

	/* Succeeded: the push buffer's length. */
	return at;
}

/*
 * Writes the push buffer that binds the copy class to its subchannel (4).
 *
 * Returns the length in bytes.
 */
size_t
nvrtx_work_push_copy_object(
	uint8_t *push,
	uint32_t copy_class)
{
	size_t at;

	/* The copy class on its subchannel. */
	at = work_method(push, 0U, NVRTX_WORK_SUBCHANNEL_COPY, WORK_SET_OBJECT, 1U);
	nvrtx_put32(push, at, copy_class);
	at += 4U;

	/* Succeeded: the push buffer's length. */
	return at;
}

/* Writes an incrementing method header for a subchannel and returns where its data goes. */
static size_t
work_method(
	uint8_t *push,
	size_t at,
	uint32_t subchannel,
	uint32_t method,
	uint32_t count)
{
	uint32_t header;

	/* The header's fields. */
	header = WORK_METHOD_INCREMENT |
	    (count << WORK_METHOD_COUNT_SHIFT) |
	    (subchannel << WORK_METHOD_SUBCHANNEL_SHIFT) |
	    (method >> 2);
	nvrtx_put32(push, at, header);

	/* Succeeded: the data follows the header. */
	return at + 4U;
}
