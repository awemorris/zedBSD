/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The forms the commands of the Vulkan executor share: the tail of a
 * create and the destroy form, bounded array counts, the replies of a
 * create and of a result, the VkResult of an errno, and the float words
 * of the limits, which the kernel builds from integers.
 */

#include <stddef.h>
#include <stdint.h>

#include <kern/kcrt.h>
#include <uapi/errno.h>

#include "vk.h"

/* The most elements one array of a command may carry. */
#define REPLY_ARRAY_MAX			65536U

/* The parts of an IEEE 754 single: the mantissa's bits, the exponent's bias and its place. */
#define FLOAT_MANTISSA_BITS		23U
#define FLOAT_EXPONENT_BIAS		127
#define FLOAT_MANTISSA_MASK		0x007fffffU

/*
 * Reads the tail of the create form, [u64 0][u64 1][identity], and
 * reports the identity.  A tail of another shape latches the reader's
 * error.
 */
uint64_t
ghci_vk_create_tail(
	struct i915_wire_reader *reader)
{
	uint64_t allocator;
	uint64_t present;
	uint64_t identity;

	/* No allocator, the identity's presence word, then the identity. */
	allocator = drv_i915_wire_read_u64(reader);
	present = drv_i915_wire_read_u64(reader);
	identity = drv_i915_wire_read_u64(reader);

	/* An allocator, an absent identity or the null handle is not the shape libvulkan sends. */
	if (allocator != 0U || present != 1U)
		reader->error = 1;
	if (identity == 0U)
		reader->error = 1;

	/* Reports the identity (meaningless after the reader's error). */
	return identity;
}

/*
 * Reads the destroy form, [device][object][u64 0], and reports the
 * object's identity (0 for the null handle) and the device's.
 */
uint64_t
ghci_vk_destroy_form(
	struct i915_wire_reader *reader,
	uint64_t *device)
{
	uint64_t identity;
	uint64_t allocator;

	/* The device, the object, the allocator. */
	*device = drv_i915_wire_read_u64(reader);
	identity = drv_i915_wire_read_u64(reader);
	allocator = drv_i915_wire_read_u64(reader);
	if (allocator != 0U)
		reader->error = 1;

	/* Reports the identity. */
	return identity;
}

/*
 * Reads an array's u64 element count, latching the reader's error for a
 * count past the bound, which must not size a walk.
 */
uint64_t
ghci_vk_read_count(
	struct i915_wire_reader *reader)
{
	uint64_t count;

	/* The count, and its bound. */
	count = drv_i915_wire_read_u64(reader);
	if (count > REPLY_ARRAY_MAX) {
		reader->error = 1;
		return 0U;
	}

	/* Succeeded: the count. */
	return count;
}

/* Skips a run of bytes of the command, latching the reader's error for one past its end. */
void
ghci_vk_skip(
	struct i915_wire_reader *reader,
	uint64_t bytes)
{
	/* A prior error moves nothing. */
	if (reader->error != 0)
		return;

	/* A run past the end fails the command. */
	if (bytes > (uint64_t)(reader->size - reader->offset)) {
		reader->error = 1;
		return;
	}

	/* The cursor passes the run. */
	reader->offset += (size_t)bytes;
}

/*
 * Writes the reply of a create when it was asked for: the result, and
 * the identity when the object was made.
 */
void
ghci_vk_reply_created(
	struct i915_wire_writer *reply,
	uint32_t requested,
	uint32_t result,
	uint64_t identity)
{
	/* A recorded or transport command has no reply. */
	if (requested == 0U)
		return;

	/* The result, then the identity of an object that was made. */
	drv_i915_wire_reply_u32(reply, result);
	if (result == (uint32_t)VK_SUCCESS) {
		drv_i915_wire_reply_u64(reply, 1U);
		drv_i915_wire_reply_u64(reply, identity);
	}
}

/* Writes the VkResult of a command when its reply was asked for. */
void
ghci_vk_reply_result(
	struct i915_wire_writer *reply,
	uint32_t requested,
	uint32_t result)
{
	/* The result alone. */
	if (requested != 0U)
		drv_i915_wire_reply_u32(reply, result);
}

/*
 * Reports the VkResult an errno of the executor becomes in a create's
 * reply: the GPU's memory (a heap, a pool or CP_HEAP's room) used up
 * (ENOSPC) is out of device memory; the kernel's own storage for an
 * object or its work (ENOMEM, ENOBUFS) is out of host memory; all of the
 * hardware's session numbers in use (EBUSY) is too many objects;
 * something the device lacks or a size past its limits (ENOTSUP, E2BIG)
 * is a missing feature; a bad argument and anything else (EINVAL,
 * ERANGE, a lost session) failed the object's initialization.  A refused
 * create never loses the device; only a submission whose GPU work failed
 * does.
 */
uint32_t
ghci_vk_result(
	int error)
{
	/* Success is VK_SUCCESS. */
	if (error == 0)
		return (uint32_t)VK_SUCCESS;

	/* The GPU's memory, then the kernel's. */
	if (error == ENOSPC)
		return (uint32_t)VK_ERROR_OUT_OF_DEVICE_MEMORY;
	if (error == ENOMEM || error == ENOBUFS)
		return (uint32_t)VK_ERROR_OUT_OF_HOST_MEMORY;

	/* The hardware's sessions, all in use. */
	if (error == EBUSY)
		return (uint32_t)VK_ERROR_TOO_MANY_OBJECTS;

	/* Something the device lacks, or more than it holds. */
	if (error == ENOTSUP || error == E2BIG)
		return (uint32_t)VK_ERROR_FEATURE_NOT_PRESENT;

	/* Anything else failed the object's initialization. */
	return (uint32_t)VK_ERROR_INITIALIZATION_FAILED;
}

/*
 * Reports the IEEE 754 single nearest to numerator / denominator (both
 * nonzero, the quotient a normal single), rounded to the nearest and to
 * even on a tie, built by long division so that the kernel never touches
 * a floating-point register.
 */
uint32_t
ghci_vk_float_ratio(
	uint64_t numerator,
	uint64_t denominator)
{
	uint64_t mantissa;
	uint32_t round;
	uint32_t index;
	int exponent;
	int sticky;

	/* Nothing to divide. */
	if (numerator == 0U || denominator == 0U)
		return 0U;

	/* Scales the division so that denominator <= numerator < 2 * denominator; exponent counts the scaling. */
	exponent = 0;
	while (numerator >= (denominator << 1) && denominator < (1ULL << 62)) {
		denominator <<= 1;
		exponent++;
	}
	while (numerator < denominator) {
		numerator <<= 1;
		exponent--;
	}

	/* The leading bit, the 23 bits of the fraction and one bit to round on. */
	mantissa = 0U;
	for (index = 0U; index < FLOAT_MANTISSA_BITS + 2U; index++) {
		mantissa <<= 1;
		if (numerator >= denominator) {
			numerator -= denominator;
			mantissa |= 1U;
		}
		numerator <<= 1;
	}
	sticky = 0;
	if (numerator != 0U)
		sticky = 1;

	/* Rounds to the nearest, to even on a tie; a carry out moves the exponent. */
	round = (uint32_t)(mantissa & 1U);
	mantissa >>= 1;
	if (round != 0U && (sticky || (mantissa & 1U) != 0U))
		mantissa++;
	if (mantissa == (1ULL << (FLOAT_MANTISSA_BITS + 1U))) {
		mantissa >>= 1;
		exponent++;
	}

	/* Succeeded: the biased exponent and the fraction. */
	return ((uint32_t)(exponent + FLOAT_EXPONENT_BIAS) << FLOAT_MANTISSA_BITS) | ((uint32_t)mantissa & FLOAT_MANTISSA_MASK);
}

/* Stores a float field by its bits, so that no floating-point register is touched. */
void
ghci_vk_set_float(
	float *destination,
	uint32_t bits)
{
	/* The bits unchanged. */
	kern_memcpy(destination, &bits, sizeof(bits));
}
