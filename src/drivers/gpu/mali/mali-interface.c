/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Reading the Mali command stream firmware's interface out of the shared
 * section, and the request words the host writes into it.
 *
 * Every address the firmware gives for an input or output part is a
 * microcontroller address; the reader accepts it only when the whole
 * part lies inside the shared section, and turns it into a pointer into
 * the view it was given.  Nothing is written while reading.
 */

#include <stddef.h>
#include <stdint.h>

#include <uapi/errno.h>

#include "drivers/gpu/mali/mali-interface.h"

/* The microseconds of one second, for the timer conversion. */
#define MICROSECONDS_PER_SECOND		1000000ULL

static volatile uint8_t *part_at(volatile uint8_t *shared, uint32_t shared_address, uint32_t bytes, uint32_t address, uint32_t length);
static int read_group(volatile uint8_t *shared, uint32_t shared_address, uint32_t bytes, uint32_t offset, struct mali_group_interface *group);
static int read_stream(volatile uint8_t *shared, uint32_t shared_address, uint32_t bytes, uint32_t offset, struct mali_stream_interface *stream);
static int same_group(const struct mali_group_interface *first, const struct mali_group_interface *group);

/*
 * Reads and checks the interface in a view of the shared section.
 *
 * EINVAL when the firmware has not published a version (it did not
 * start), when a part lies outside the section, or when the counts of
 * groups or streams are outside what a working firmware offers; EPROTO
 * when the group slots or the streams differ from the first.
 */
int
mali_iface_read(
	volatile uint8_t *shared,
	uint32_t shared_address,
	uint32_t bytes,
	struct mali_interfaces *interfaces)
{
	struct mali_group_interface *group;
	uint64_t offset;
	uint32_t index;
	uint32_t input;
	uint32_t output;
	int same;
	int error;

	/* The global block's control part is the section's first bytes. */
	if (bytes < MALI_IFACE_GROUP_OFFSET)
		return EINVAL;
	interfaces->control = shared;

	/* A version of zero means the firmware never filled the block. */
	interfaces->version = mali_iface_read32(shared, MALI_GLB_CONTROL_VERSION);
	if (interfaces->version == 0U)
		return EINVAL;

	/* What the firmware reports of itself. */
	interfaces->features = mali_iface_read32(shared, MALI_GLB_CONTROL_FEATURES);
	interfaces->group_count = mali_iface_read32(shared, MALI_GLB_CONTROL_GROUPS);
	interfaces->group_stride = mali_iface_read32(shared, MALI_GLB_CONTROL_GROUP_STRIDE);

	/* The global input and output parts, each wholly inside the section. */
	input = mali_iface_read32(shared, MALI_GLB_CONTROL_INPUT);
	output = mali_iface_read32(shared, MALI_GLB_CONTROL_OUTPUT);
	interfaces->input = part_at(shared, shared_address, bytes, input, MALI_GLB_INPUT_BYTES);
	interfaces->output = part_at(shared, shared_address, bytes, output, MALI_GLB_OUTPUT_BYTES);
	if (interfaces->input == NULL || interfaces->output == NULL)
		return EINVAL;

	/* A count of groups a working firmware offers, and a stride that keeps them apart. */
	if (interfaces->group_count < MALI_GROUPS_MIN || interfaces->group_count > MALI_GROUPS_MAX)
		return EINVAL;
	if (interfaces->group_stride < MALI_CSG_CONTROL_BYTES)
		return EINVAL;

	/* Every group slot, each like the first. */
	for (index = 0U; index < interfaces->group_count; index++) {
		/* The slot's block is past the global one, a stride apart from the next. */
		group = &interfaces->groups[index];
		offset = MALI_IFACE_GROUP_OFFSET + (uint64_t)index * interfaces->group_stride;
		if (offset > bytes)
			return EINVAL;

		error = read_group(shared, shared_address, bytes, (uint32_t)offset, group);
		if (error != 0)
			return error;

		/* The scheduler treats the slots as one kind, so they must be. */
		if (index == 0U)
			continue;

		same = same_group(&interfaces->groups[0], group);
		if (!same)
			return EPROTO;
	}

	/* Succeeded: every part is inside the section. */
	return 0;
}

/*
 * Builds the input word that flips the bits of mask away from the
 * acknowledge word.
 */
uint32_t
mali_iface_toggle(
	uint32_t request,
	uint32_t acknowledge,
	uint32_t mask)
{
	uint32_t flipped;

	/* Under the mask, the opposite of what was acknowledged; elsewhere, the request as it was. */
	flipped = (acknowledge ^ mask) & mask;
	flipped |= request & ~mask;

	/* Succeeded: the new request word. */
	return flipped;
}

/*
 * Builds the input word that sets the bits of mask to those of value.
 */
uint32_t
mali_iface_update(
	uint32_t request,
	uint32_t value,
	uint32_t mask)
{
	uint32_t updated;

	/* Under the mask, the value; elsewhere, the request as it was. */
	updated = request & ~mask;
	updated |= value & mask;

	/* Succeeded: the new request word. */
	return updated;
}

/*
 * Reports whether every bit of mask is acknowledged.
 */
int
mali_iface_acked(
	uint32_t request,
	uint32_t acknowledge,
	uint32_t mask)
{
	/* A bit that still differs is a request the firmware has not answered. */
	if (((request ^ acknowledge) & mask) != 0U)
		return 0;

	/* Succeeded: every bit is answered. */
	return 1;
}

/*
 * Builds a timer word for a time counted on the GPU's clock.
 *
 * The count is in units of 1024 cycles, rounded up, and saturates at the
 * largest count the word holds.  A rate of zero gives the largest count,
 * so the firmware still powers cores off, only late.
 *
 * Procedure referenced:
 * Linux v7.3 drivers/gpu/drm/panthor/panthor_fw.c
 * (panthor_fw_conv_timeout).
 */
uint32_t
mali_iface_timer(
	uint64_t microseconds,
	uint64_t rate)
{
	uint64_t divisor;
	uint64_t count;

	/* Without a rate, the longest time the word can say. */
	if (rate == 0U)
		return MALI_TIMER_COUNT_MASK | MALI_TIMER_GPU_COUNTER;

	/* Cycles in units of 1024, rounded up; a product that would overflow saturates. */
	divisor = MICROSECONDS_PER_SECOND << MALI_TIMER_SCALE_SHIFT;
	count = MALI_TIMER_COUNT_MASK;
	if (microseconds <= UINT64_MAX / rate)
		count = (microseconds * rate + divisor - 1U) / divisor;

	/* A time longer than the word holds is the longest it holds. */
	if (count > MALI_TIMER_COUNT_MASK)
		count = MALI_TIMER_COUNT_MASK;

	/* Succeeded: the count on the GPU's own counter. */
	return (uint32_t)count | MALI_TIMER_GPU_COUNTER;
}

/*
 * Reads one 32-bit word of an interface part.
 */
uint32_t
mali_iface_read32(
	const volatile uint8_t *part,
	uint32_t offset)
{
	const volatile uint32_t *word;

	/* The parts are word aligned, and every field of them too. */
	word = (const volatile uint32_t *)(const volatile void *)(part + offset);

	/* Succeeded: the word's value. */
	return *word;
}

/*
 * Reads one 64-bit word of an interface part, low half first.
 */
uint64_t
mali_iface_read64(
	const volatile uint8_t *part,
	uint32_t offset)
{
	uint64_t low;
	uint64_t high;

	/* The two halves, low then high, as the firmware writes them. */
	low = mali_iface_read32(part, offset);
	high = mali_iface_read32(part, offset + 4U);

	/* Succeeded: the word's value. */
	return low | (high << 32);
}

/*
 * Writes one 32-bit word of an interface part.
 */
void
mali_iface_write32(
	volatile uint8_t *part,
	uint32_t offset,
	uint32_t value)
{
	volatile uint32_t *word;

	/* The parts are word aligned, and every field of them too. */
	word = (volatile uint32_t *)(volatile void *)(part + offset);
	*word = value;
}

/*
 * Writes one 64-bit word of an interface part, low half first.
 */
void
mali_iface_write64(
	volatile uint8_t *part,
	uint32_t offset,
	uint64_t value)
{
	/* The two halves, low then high. */
	mali_iface_write32(part, offset, (uint32_t)value);
	mali_iface_write32(part, offset + 4U, (uint32_t)(value >> 32));
}

/*
 * Finds a part of length bytes at a microcontroller address inside the
 * shared section; NULL when any of it lies outside.
 */
static volatile uint8_t *
part_at(
	volatile uint8_t *shared,
	uint32_t shared_address,
	uint32_t bytes,
	uint32_t address,
	uint32_t length)
{
	uint32_t offset;

	/* Before the section. */
	if (address < shared_address)
		return NULL;

	/* Past its end, or running past it. */
	offset = address - shared_address;
	if (offset > bytes || length > bytes - offset)
		return NULL;

	/* A field read as a word must start on one. */
	if ((offset & 3U) != 0U)
		return NULL;

	/* Succeeded: the part's place in the view. */
	return shared + offset;
}

/* Reads one group slot's block at offset in the section, and its streams. */
static int
read_group(
	volatile uint8_t *shared,
	uint32_t shared_address,
	uint32_t bytes,
	uint32_t offset,
	struct mali_group_interface *group)
{
	struct mali_stream_interface *stream;
	uint64_t stream_offset;
	uint32_t index;
	uint32_t input;
	uint32_t output;
	int error;

	/* The control part, wholly inside the section. */
	if (MALI_CSG_CONTROL_BYTES > bytes - offset)
		return EINVAL;
	group->control = shared + offset;

	/* What the slot reports of itself. */
	group->features = mali_iface_read32(group->control, MALI_CSG_CONTROL_FEATURES);
	group->suspend_bytes = mali_iface_read32(group->control, MALI_CSG_CONTROL_SUSPEND);
	group->protected_suspend_bytes = mali_iface_read32(group->control, MALI_CSG_CONTROL_PROTM_SUSPEND);
	group->stream_count = mali_iface_read32(group->control, MALI_CSG_CONTROL_STREAMS);
	group->stream_stride = mali_iface_read32(group->control, MALI_CSG_CONTROL_STREAM_STRIDE);

	/* The input and output parts. */
	input = mali_iface_read32(group->control, MALI_CSG_CONTROL_INPUT);
	output = mali_iface_read32(group->control, MALI_CSG_CONTROL_OUTPUT);
	group->input = part_at(shared, shared_address, bytes, input, MALI_CSG_INPUT_BYTES);
	group->output = part_at(shared, shared_address, bytes, output, MALI_CSG_OUTPUT_BYTES);
	if (group->input == NULL || group->output == NULL)
		return EINVAL;

	/* A count of streams a working firmware offers, and a stride that keeps them apart. */
	if (group->stream_count < MALI_STREAMS_MIN || group->stream_count > MALI_STREAMS_MAX)
		return EINVAL;
	if (group->stream_stride < MALI_CS_CONTROL_BYTES)
		return EINVAL;

	/* Every stream of the slot, each like the slot's first. */
	for (index = 0U; index < group->stream_count; index++) {
		/* The stream's block follows the slot's own, a stride apart from the next. */
		stream = &group->streams[index];
		stream_offset = (uint64_t)offset + MALI_IFACE_STREAM_OFFSET + (uint64_t)index * group->stream_stride;
		if (stream_offset > bytes)
			return EINVAL;

		error = read_stream(shared, shared_address, bytes, (uint32_t)stream_offset, stream);
		if (error != 0)
			return error;

		/* Every stream of a slot runs the same programs, so it must be the same kind. */
		if (index != 0U && stream->features != group->streams[0].features)
			return EPROTO;
	}

	/* Succeeded: the slot and its streams are inside the section. */
	return 0;
}

/* Reads one stream's block at offset in the section. */
static int
read_stream(
	volatile uint8_t *shared,
	uint32_t shared_address,
	uint32_t bytes,
	uint32_t offset,
	struct mali_stream_interface *stream)
{
	uint32_t input;
	uint32_t output;

	/* The control part, wholly inside the section. */
	if (MALI_CS_CONTROL_BYTES > bytes - offset)
		return EINVAL;
	stream->control = shared + offset;
	stream->features = mali_iface_read32(stream->control, MALI_CS_CONTROL_FEATURES);

	/* The input and output parts. */
	input = mali_iface_read32(stream->control, MALI_CS_CONTROL_INPUT);
	output = mali_iface_read32(stream->control, MALI_CS_CONTROL_OUTPUT);
	stream->input = part_at(shared, shared_address, bytes, input, MALI_CS_INPUT_BYTES);
	stream->output = part_at(shared, shared_address, bytes, output, MALI_CS_OUTPUT_BYTES);
	if (stream->input == NULL || stream->output == NULL)
		return EINVAL;

	/* Succeeded: the stream is inside the section. */
	return 0;
}

/* Reports whether a group slot is of the same kind as the first one. */
static int
same_group(
	const struct mali_group_interface *first,
	const struct mali_group_interface *group)
{
	/* The features, both suspend sizes and the count of streams must agree. */
	if (group->features != first->features)
		return 0;
	if (group->suspend_bytes != first->suspend_bytes)
		return 0;
	if (group->protected_suspend_bytes != first->protected_suspend_bytes)
		return 0;
	if (group->stream_count != first->stream_count)
		return 0;

	/* Succeeded: the slots are interchangeable. */
	return 1;
}
