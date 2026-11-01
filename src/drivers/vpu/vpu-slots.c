/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The ties between an application's DPB slots and a decoder core's frame
 * buffers (see vpu-codec.h).
 *
 * A stateful core keeps its references in its own frame buffers and
 * refuses to decode into one it still references; the application keeps
 * the same references in its slots.  So each picture goes into a frame
 * buffer no live slot holds, and the slot it becomes then holds that
 * frame buffer.
 */

#include <stddef.h>
#include <stdint.h>

#include <uapi/errno.h>

#include "drivers/vpu/vpu-codec.h"

/*
 * Forgets every binding.
 */
void
vpu_slots_reset(
	struct vpu_slots *slots,
	uint32_t count)
{
	uint32_t index;

	/* No slot holds a frame buffer. */
	for (index = 0; index < VPU_SLOTS_MAX; index++)
		slots->buffer[index] = VPU_NONE;

	/* No frame buffer holds a slot's picture, and none was decoded into. */
	for (index = 0; index < VPU_BUFFERS_MAX; index++) {
		slots->slot[index] = VPU_NONE;
		slots->used[index] = 0;
	}

	/* The core's frame buffers, and no picture yet. */
	if (count > VPU_BUFFERS_MAX)
		count = VPU_BUFFERS_MAX;
	slots->count = count;
	slots->serial = 0;
}

/*
 * Reports the frame buffers the live slots hold, a bit each.
 */
uint32_t
vpu_slots_taken(
	const struct vpu_slots *slots,
	uint32_t references,
	uint32_t active,
	int32_t setup)
{
	uint32_t live;
	uint32_t taken;
	uint32_t slot;

	/* The live slots: the references, and the active ones but the setup slot. */
	live = active;
	if (setup >= 0 && setup < (int32_t)VPU_SLOTS_MAX)
		live &= ~(1U << (uint32_t)setup);
	live |= references;

	/* The frame buffers the live slots hold. */
	taken = 0;
	for (slot = 0; slot < VPU_SLOTS_MAX; slot++) {
		if ((live & (1U << slot)) == 0)
			continue;
		if (slots->buffer[slot] >= 0)
			taken |= 1U << (uint32_t)slots->buffer[slot];
	}

	/* Succeeded: the frame buffers no picture may be decoded into. */
	return taken;
}

/*
 * Picks the frame buffer a picture is decoded into.
 */
int
vpu_slots_pick(
	const struct vpu_slots *slots,
	uint32_t references,
	uint32_t active,
	int32_t setup,
	uint32_t *index)
{
	uint32_t taken;
	uint32_t candidate;
	uint32_t oldest;
	int found;

	/* The frame buffers the live slots hold. */
	taken = vpu_slots_taken(slots, references, active, setup);

	/* The free frame buffer decoded into longest ago. */
	found = 0;
	oldest = 0;
	for (candidate = 0; candidate < slots->count; candidate++) {
		if ((taken & (1U << candidate)) != 0)
			continue;
		if (!found || slots->used[candidate] < slots->used[oldest]) {
			oldest = candidate;
			found = 1;
		}
	}

	/* Every frame buffer holds a live slot's picture. */
	if (!found)
		return ENOSPC;

	/* Succeeded: the frame buffer to decode into. */
	*index = oldest;
	return 0;
}

/*
 * Binds a decoded picture to its setup slot.
 */
void
vpu_slots_bind(
	struct vpu_slots *slots,
	int32_t setup,
	uint32_t index)
{
	int32_t old;

	/* The frame buffer was just decoded into. */
	if (index >= slots->count)
		return;
	slots->serial++;
	slots->used[index] = slots->serial;

	/* The frame buffer's old slot holds nothing any more. */
	old = slots->slot[index];
	if (old >= 0)
		slots->buffer[old] = VPU_NONE;
	slots->slot[index] = VPU_NONE;

	/* A picture without a slot is held by none. */
	if (setup < 0 || setup >= (int32_t)VPU_SLOTS_MAX)
		return;

	/* The setup slot's old frame buffer holds no slot. */
	if (slots->buffer[setup] >= 0)
		slots->slot[slots->buffer[setup]] = VPU_NONE;

	/* The slot and the frame buffer hold each other. */
	slots->buffer[setup] = (int32_t)index;
	slots->slot[index] = setup;
}

/*
 * Forgets the slots outside keep.
 */
void
vpu_slots_keep(
	struct vpu_slots *slots,
	uint32_t keep)
{
	uint32_t slot;

	/* Each slot not kept lets its frame buffer go. */
	for (slot = 0; slot < VPU_SLOTS_MAX; slot++) {
		if ((keep & (1U << slot)) != 0 || slots->buffer[slot] < 0)
			continue;
		slots->slot[slots->buffer[slot]] = VPU_NONE;
		slots->buffer[slot] = VPU_NONE;
	}
}
