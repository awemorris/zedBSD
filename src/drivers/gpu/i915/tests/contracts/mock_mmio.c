/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * A mock register file behind struct i915_mmio_ops (see mock_mmio.h).
 */

#include "mock_mmio.h"

#include <stddef.h>

/*
 * The register-to-domain map the mock device uses.
 *
 * The render command-streamer bands need the render domain, the 0x9xxx band
 * needs the GT domain, and every other register is always on.  The table
 * never changes and every mock shares it.
 */
static const struct i915_mmio_range mock_mmio_range_table[] = {
	{ 0x2000U, 0x2fffU, I915_FORCEWAKE_RENDER },
	{ 0x9000U, 0x9fffU, I915_FORCEWAKE_GT },
	{ 0xe000U, 0xefffU, I915_FORCEWAKE_RENDER }
};

static int mock_mmio_find(const struct mock_mmio *mock, uint32_t offset);
static int mock_mmio_slot(struct mock_mmio *mock, uint32_t offset);
static uint32_t mock_mmio_read32(void *context, uint32_t offset);
static void mock_mmio_write32(void *context, uint32_t offset, uint32_t value);
static void mock_mmio_forcewake_request(void *context, int domain, int wake);
static int mock_mmio_forcewake_ack(void *context, int domain);

/*
 * Returns the operations that reach a mock register file.
 *
 * The context those operations receive is a struct mock_mmio.
 */
const struct i915_mmio_ops *
mock_mmio_ops(void)
{
	static const struct i915_mmio_ops ops = {
		mock_mmio_read32,
		mock_mmio_write32,
		mock_mmio_forcewake_request,
		mock_mmio_forcewake_ack
	};

	/* Succeeded: the operations reach the mock. */
	return &ops;
}

/*
 * Returns the mock's register-to-domain map and its length.
 */
const struct i915_mmio_range *
mock_mmio_ranges(
	unsigned *count)
{
	/* Reports the table length alongside the table. */
	*count = sizeof(mock_mmio_range_table) / sizeof(mock_mmio_range_table[0]);

	/* Succeeded: the table is static and shared. */
	return mock_mmio_range_table;
}

/*
 * Empties the register file and forgets every forcewake request.
 */
void
mock_mmio_reset(
	struct mock_mmio *mock)
{
	int index;

	/* Frees every register slot. */
	for (index = 0; index < MOCK_MMIO_REGISTERS; index++) {
		mock->registers[index].used = 0;
		mock->registers[index].offset = 0U;
		mock->registers[index].value = 0U;
	}

	/* Leaves every domain asleep and willing to acknowledge. */
	for (index = 0; index < I915_FORCEWAKE_DOMAIN_COUNT; index++) {
		mock->forcewake_wake[index] = 0;
		mock->forcewake_never_ack[index] = 0;
		mock->forcewake_request_calls[index] = 0;
	}

	/* Starts the bus counters. */
	mock->read_calls = 0;
	mock->write_calls = 0;

	/* A write only stores its value. */
	mock->write_hook = NULL;
	mock->write_hook_context = NULL;
}

/*
 * Stores a register value without counting it as a bus write.
 */
void
mock_mmio_preset(
	struct mock_mmio *mock,
	uint32_t offset,
	uint32_t value)
{
	int slot;

	/* Claims the register's slot and stores the value. */
	slot = mock_mmio_slot(mock, offset);
	if (slot < 0)
		return;

	mock->registers[slot].value = value;
}

/*
 * Reports a register value without counting it as a bus read.
 */
uint32_t
mock_mmio_peek(
	const struct mock_mmio *mock,
	uint32_t offset)
{
	int slot;

	/* A register never written reads zero. */
	slot = mock_mmio_find(mock, offset);
	if (slot < 0)
		return 0U;

	/* Succeeded: reports the stored value. */
	return mock->registers[slot].value;
}

/* Finds the slot that holds a register, or reports -1. */
static int
mock_mmio_find(
	const struct mock_mmio *mock,
	uint32_t offset)
{
	int index;

	/* Looks through the claimed slots. */
	for (index = 0; index < MOCK_MMIO_REGISTERS; index++) {
		if (mock->registers[index].used == 0)
			continue;
		if (mock->registers[index].offset != offset)
			continue;

		/* Succeeded: the register lives in this slot. */
		return index;
	}

	/* The register has never been written. */
	return -1;
}

/* Finds or claims the slot of a register, or reports -1 when the file is full. */
static int
mock_mmio_slot(
	struct mock_mmio *mock,
	uint32_t offset)
{
	int index;

	/* A register already held keeps its slot. */
	index = mock_mmio_find(mock, offset);
	if (index >= 0)
		return index;

	/* Claims the first free slot for the register, starting at zero. */
	for (index = 0; index < MOCK_MMIO_REGISTERS; index++) {
		if (mock->registers[index].used != 0)
			continue;

		mock->registers[index].used = 1;
		mock->registers[index].offset = offset;
		mock->registers[index].value = 0U;

		/* Succeeded: the register now lives in this slot. */
		return index;
	}

	/* Every slot is taken. */
	return -1;
}

/* Reads one register and counts the bus read. */
static uint32_t
mock_mmio_read32(
	void *context,
	uint32_t offset)
{
	struct mock_mmio *mock;
	int slot;

	/* Counts the read before looking the register up. */
	mock = context;
	mock->read_calls++;

	/* A register never written reads zero. */
	slot = mock_mmio_find(mock, offset);
	if (slot < 0)
		return 0U;

	/* Succeeded: reports the stored value. */
	return mock->registers[slot].value;
}

/* Writes one register and counts the bus write. */
static void
mock_mmio_write32(
	void *context,
	uint32_t offset,
	uint32_t value)
{
	struct mock_mmio *mock;
	int slot;

	/* Counts the write before storing it. */
	mock = context;
	mock->write_calls++;

	/* Stores the value in the register's slot; a full file drops it. */
	slot = mock_mmio_slot(mock, offset);
	if (slot < 0)
		return;

	mock->registers[slot].value = value;

	/* Lets the test's model of the hardware answer the write. */
	if (mock->write_hook != NULL)
		mock->write_hook(mock, offset, value, mock->write_hook_context);
}

/* Records a wake or sleep request of a domain. */
static void
mock_mmio_forcewake_request(
	void *context,
	int domain,
	int wake)
{
	struct mock_mmio *mock;

	/* Remembers the requested state, which the acknowledge reflects. */
	mock = context;
	mock->forcewake_wake[domain] = wake;
	mock->forcewake_request_calls[domain]++;
}

/* Acknowledges a domain that was asked to wake, unless told never to. */
static int
mock_mmio_forcewake_ack(
	void *context,
	int domain)
{
	struct mock_mmio *mock;

	/* A domain told never to acknowledge models a hung power well. */
	mock = context;
	if (mock->forcewake_never_ack[domain] != 0)
		return 0;

	/* A domain asked to wake acknowledges at once. */
	if (mock->forcewake_wake[domain] != 0)
		return 1;

	/* A sleeping domain does not acknowledge. */
	return 0;
}
