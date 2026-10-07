/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * A mock register file behind struct i915_mmio_ops.
 *
 * It holds a small set of registers and models the forcewake handshake: a
 * domain acknowledges once it has been asked to wake, unless the test tells
 * it never to acknowledge.  Every bus access and every forcewake request is
 * counted, so a test can tell a read-modify-write from a single write.
 */

#ifndef DRIVERS_GPU_I915_TESTS_CONTRACTS_MOCK_MMIO_H
#define DRIVERS_GPU_I915_TESTS_CONTRACTS_MOCK_MMIO_H

#include "../../mmio.h"

#include <stdint.h>

/* How many distinct registers the mock can hold. */
#define MOCK_MMIO_REGISTERS	256

/*
 * One register the mock has seen.
 *
 * A slot is claimed by the first write or preset of its offset and is never
 * given back until the mock is reset.
 */
struct mock_mmio_register {
	uint32_t offset;
	uint32_t value;
	int used;
};

struct mock_mmio;

/*
 * A test's model of what the hardware does on a write.
 *
 * It is called after the written value is stored and may change any
 * register, for example clear a request bit the hardware acknowledges.
 */
typedef void (*mock_mmio_write_hook_fn)(struct mock_mmio *mock, uint32_t offset, uint32_t value, void *context);

/*
 * The register file and forcewake state of one mock device.
 *
 * A test owns it for the whole program; mock_mmio_reset() empties it.
 */
struct mock_mmio {
	/* The registers written or preset so far; any other register reads zero. */
	struct mock_mmio_register registers[MOCK_MMIO_REGISTERS];

	/* The last wake state each domain was asked for. */
	int forcewake_wake[I915_FORCEWAKE_DOMAIN_COUNT];

	/* Nonzero makes the domain never acknowledge, to force an acknowledge timeout. */
	int forcewake_never_ack[I915_FORCEWAKE_DOMAIN_COUNT];

	/* How many wake and sleep requests each domain received. */
	int forcewake_request_calls[I915_FORCEWAKE_DOMAIN_COUNT];

	/* How many bus reads and writes reached the register file. */
	int read_calls;
	int write_calls;

	/* The test's model of the hardware's answer to a write, and its context; NULL only stores (ws083-p007). */
	mock_mmio_write_hook_fn write_hook;
	void *write_hook_context;
};

const struct i915_mmio_ops *mock_mmio_ops(void);
const struct i915_mmio_range *mock_mmio_ranges(unsigned *count);
void mock_mmio_reset(struct mock_mmio *mock);
void mock_mmio_preset(struct mock_mmio *mock, uint32_t offset, uint32_t value);
uint32_t mock_mmio_peek(const struct mock_mmio *mock, uint32_t offset);

#endif
