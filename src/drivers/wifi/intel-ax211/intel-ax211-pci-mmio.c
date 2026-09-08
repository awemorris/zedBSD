/* -*- mode: c; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*- */

/*
 * zedBSD Intel AX211 PCI BAR backend
 * Copyright (C) 2026 Awe Morris
 * SPDX-License-Identifier: Zlib
 */

#include "intel-ax211-pci-mmio.h"

#include <errno.h>
#include <hal/hal.h>
#include <kern/sched.h>
#include <limits.h>
#include <string.h>

#define AX211_PCI_MMIO_MINIMUM_SIZE 0x4000U
#define AX211_HBUS_PRPH_WRITE_ADDRESS 0x0444U
#define AX211_HBUS_PRPH_READ_ADDRESS 0x0448U
#define AX211_HBUS_PRPH_WRITE_DATA 0x044cU
#define AX211_HBUS_PRPH_READ_DATA 0x0450U
#define AX211_PRPH_ACCESS_DWORD 0x03000000U
#define AX211_PRPH_ADDRESS_MASK 0x00ffffffU
#define AX211_MICROSECONDS_PER_SECOND 1000000ULL
#define AX211_COUNTER_FREQUENCY_MIN_HZ 1000000ULL
#define AX211_COUNTER_FREQUENCY_MAX_HZ 10000000000ULL
#define AX211_BUSY_WAIT_WINDOW_US 1000U

static int ax211_backend_csr_read32(void *argument, uint32_t offset, uint32_t *value);
static int ax211_backend_csr_write32(void *argument, uint32_t offset, uint32_t value);
static int ax211_backend_prph_read32(void *argument, uint32_t address, uint32_t *value);
static int ax211_backend_prph_write32(void *argument, uint32_t address, uint32_t value);
static int ax211_backend_delay_us(void *argument, uint32_t duration_us);
static int ax211_backend_clock_us(void *argument, uint64_t *time_us);
static void ax211_backend_relax(void);
static int ax211_backend_ticks_for_us(uint64_t frequency_hz, uint64_t microseconds, uint64_t *ticks);
static int ax211_backend_ticks_to_us(uint64_t frequency_hz, uint64_t ticks, uint64_t *microseconds);
static int ax211_backend_mul_div_reduced(uint64_t numerator, uint64_t multiplier, uint64_t divisor, uint64_t *quotient, uint64_t *remainder);
static int ax211_backend_counter_read_checked(struct intel_ax211_pci_mmio_backend *backend, uint64_t *ticks);
static int ax211_backend_microseconds_publish(struct intel_ax211_pci_mmio_backend *backend, uint64_t microseconds);

static const struct intel_ax211_mmio_ops ax211_pci_mmio_operations = {
	.csr_read32 = ax211_backend_csr_read32,
	.csr_write32 = ax211_backend_csr_write32,
	.prph_read32 = ax211_backend_prph_read32,
	.prph_write32 = ax211_backend_prph_write32,
	.delay_us = ax211_backend_delay_us,
	.clock_us = ax211_backend_clock_us,
	.trace_deadline = NULL};

/*
 * Implements the drv intel ax211 pci mmio backend init operation.
 */
int
drv_intel_ax211_pci_mmio_backend_init(
	struct intel_ax211_pci_mmio_backend *backend,
	void *registers,
	size_t mapping_size)
{
	struct intel_ax211_pci_mmio_backend candidate;
	uint64_t frequency_hz;
	uint64_t origin_counter;

	/* Handles the backend availability. */
	if (backend == NULL || registers == NULL ||
	    mapping_size < AX211_PCI_MMIO_MINIMUM_SIZE)

		/* Returns the computed result. */
		return EINVAL;
	memset(&candidate, 0, sizeof(candidate));

	/* Checks the hal rtc read counter result. */
	if (!hal_rtc_read_counter(&origin_counter, &frequency_hz) ||
	    frequency_hz < AX211_COUNTER_FREQUENCY_MIN_HZ ||
	    frequency_hz > AX211_COUNTER_FREQUENCY_MAX_HZ) {
		memset(backend, 0, sizeof(*backend));

		/* Returns the computed result. */
		return ENOTSUP;
	}
	candidate.registers = registers;
	candidate.mapping_size = mapping_size;
	candidate.counter_frequency_hz = frequency_hz;
	candidate.counter_origin = origin_counter;
	candidate.last_counter = origin_counter;
	candidate.counter_ready = 1U;
	*backend = candidate;
	/* Reports successful completion. */
	return 0;
}

/*
 * Implements the drv intel ax211 pci mmio ops operation.
 */
const struct intel_ax211_mmio_ops *
drv_intel_ax211_pci_mmio_ops(
	void)
{
	/* Returns the computed result. */
	return &ax211_pci_mmio_operations;
}

static int ax211_backend_range_valid(const struct intel_ax211_pci_mmio_backend *backend, uint32_t offset);

/* Supports the ax211 backend range valid operation. */
static int
ax211_backend_range_valid(
	const struct intel_ax211_pci_mmio_backend *backend,
	uint32_t offset)
{
	int function_result;

	/* Handles the backend availability. */
	if (backend == NULL || backend->registers == NULL ||
	    (offset & 3U) != 0U)

		/* Reports successful completion. */
		return 0;

	/* Computes the function result. */
	function_result =
		(size_t)offset <= backend->mapping_size &&
		sizeof(uint32_t) <= backend->mapping_size - (size_t)offset;

	/* Returns the computed result. */
	return function_result;
}

/* Supports the ax211 backend csr read32 operation. */
static int
ax211_backend_csr_read32(
	void *argument,
	uint32_t offset,
	uint32_t *value)
{
	struct intel_ax211_pci_mmio_backend *backend = argument;

	/* Checks the ax211 backend range valid result. */
	if (value == NULL || !ax211_backend_range_valid(backend, offset))
		return EINVAL;
	*value = *(volatile uint32_t *)(backend->registers + offset);
	hal_io_rmb();

	/* Validates the current value. */
	if (*value == UINT32_MAX)
		return EIO;

	/* Reports successful completion. */
	return 0;
}

/* Supports the ax211 backend csr write32 operation. */
static int
ax211_backend_csr_write32(
	void *argument,
	uint32_t offset,
	uint32_t value)
{
	struct intel_ax211_pci_mmio_backend *backend = argument;

	/* Checks the ax211 backend range valid result. */
	if (!ax211_backend_range_valid(backend, offset))
		return EINVAL;
	*(volatile uint32_t *)(backend->registers + offset) = value;
	hal_io_wmb();

	/* Reports successful completion. */
	return 0;
}

/* Supports the ax211 backend prph read32 operation. */
static int
ax211_backend_prph_read32(
	void *argument,
	uint32_t address,
	uint32_t *value)
{
	int function_result;
	struct intel_ax211_pci_mmio_backend *backend = argument;
	int error;

	/* Handles the value availability. */
	if (address > AX211_PRPH_ADDRESS_MASK || value == NULL)
		return EINVAL;
	error = ax211_backend_csr_write32(backend, AX211_HBUS_PRPH_READ_ADDRESS,
					  address | AX211_PRPH_ACCESS_DWORD);

	/* Checks the operation status. */
	if (error != 0)
		return error;
	hal_io_mb();

	/* Obtains the ax211 backend csr read32 result. */
	function_result = ax211_backend_csr_read32(
		backend, AX211_HBUS_PRPH_READ_DATA, value);

	/* Returns the computed result. */
	return function_result;
}

/* Supports the ax211 backend prph write32 operation. */
static int
ax211_backend_prph_write32(
	void *argument,
	uint32_t address,
	uint32_t value)
{
	int function_result;
	struct intel_ax211_pci_mmio_backend *backend = argument;
	int error;

	/* Handles the address condition. */
	if (address > AX211_PRPH_ADDRESS_MASK)
		return EINVAL;
	error = ax211_backend_csr_write32(backend,
					  AX211_HBUS_PRPH_WRITE_ADDRESS,
					  address | AX211_PRPH_ACCESS_DWORD);

	/* Checks the operation status. */
	if (error != 0)
		return error;
	hal_io_wmb();

	/* Obtains the ax211 backend csr write32 result. */
	function_result = ax211_backend_csr_write32(
		backend, AX211_HBUS_PRPH_WRITE_DATA, value);

	/* Returns the computed result. */
	return function_result;
}

/* Supports the ax211 backend relax operation. */
static void
ax211_backend_relax(
	void)
{
#ifdef INTEL_AX211_PCI_MMIO_HOST_TEST
	extern void intel_ax211_pci_mmio_host_relax(void);

	intel_ax211_pci_mmio_host_relax();
#else
	hal_atomic_relax();
#endif
}

/* Supports the ax211 backend mul div reduced operation. */
static int
ax211_backend_mul_div_reduced(
	uint64_t numerator,
	uint64_t multiplier,
	uint64_t divisor,
	uint64_t *quotient,
	uint64_t *remainder)
{
	uint64_t carry;
	uint64_t mask;
	uint64_t reduced;
	uint64_t result;
	uint64_t threshold;

	/* Handles the quotient availability. */
	if (divisor == 0U || numerator >= divisor || quotient == NULL ||
	    remainder == NULL)

		/* Returns the computed result. */
		return EINVAL;
	reduced = 0U;
	result = 0U;
	mask = UINT64_C(1) << 63;

	/*
 * Accumulate the exact quotient and remainder one multiplier bit at a
	 * time. */
	/* Continue while the operation condition remains true. */
	while (mask != 0U) {
		threshold = divisor - reduced;

		/* Handles the reduced condition. */
		if (reduced >= threshold) {
			reduced -= threshold;
			carry = 1U;
		} else {
			reduced += reduced;
			carry = 0U;
		}

		/* Checks the operation result. */
		if (result > UINT64_MAX / 2U)
			return EOVERFLOW;
		result = result * 2U + carry;

		/* Handles the multiplier condition. */
		if ((multiplier & mask) != 0U) {
			threshold = divisor - numerator;

			/* Handles the reduced condition. */
			if (reduced >= threshold) {
				reduced -= threshold;

				/* Checks the operation result. */
				if (result == UINT64_MAX)
					return EOVERFLOW;
				result++;
			} else {
				reduced += numerator;
			}
		}
		mask >>= 1;
	}
	*quotient = result;
	*remainder = reduced;
	/* Reports successful completion. */
	return 0;
}

/* Supports the ax211 backend ticks for us operation. */
static int
ax211_backend_ticks_for_us(
	uint64_t frequency_hz,
	uint64_t microseconds,
	uint64_t *ticks)
{
	uint64_t fractional;
	uint64_t fractional_remainder;
	uint64_t result;
	uint64_t whole;
	int error;

	/* Handles the ticks availability. */
	if (frequency_hz == 0U || ticks == NULL)
		return EINVAL;
	whole = frequency_hz / AX211_MICROSECONDS_PER_SECOND;

	/* Handles the whole condition. */
	if (whole != 0U && microseconds > UINT64_MAX / whole)
		return EOVERFLOW;
	result = whole * microseconds;
	error = ax211_backend_mul_div_reduced(
		frequency_hz % AX211_MICROSECONDS_PER_SECOND, microseconds,
		AX211_MICROSECONDS_PER_SECOND, &fractional,
		&fractional_remainder);

	/* Checks the operation status. */
	if (error != 0)
		return error;

	/* Handles the fractional condition. */
	if (fractional > UINT64_MAX - result)
		return EOVERFLOW;
	result += fractional;

	/* Handles the fractional remainder condition. */
	if (fractional_remainder != 0U) {
		/* Checks the operation result. */
		if (result == UINT64_MAX)
			return EOVERFLOW;
		result++;
	}
	*ticks = result;
	/* Reports successful completion. */
	return 0;
}

/* Supports the ax211 backend ticks to us operation. */
static int
ax211_backend_ticks_to_us(
	uint64_t frequency_hz,
	uint64_t ticks,
	uint64_t *microseconds)
{
	uint64_t fractional;
	uint64_t fractional_remainder;
	uint64_t result;
	uint64_t whole;
	int error;

	/* Handles the microseconds availability. */
	if (frequency_hz == 0U || microseconds == NULL)
		return EINVAL;
	whole = ticks / frequency_hz;

	/* Handles the whole condition. */
	if (whole > UINT64_MAX / AX211_MICROSECONDS_PER_SECOND)
		return EOVERFLOW;
	result = whole * AX211_MICROSECONDS_PER_SECOND;
	error = ax211_backend_mul_div_reduced(
		ticks % frequency_hz, AX211_MICROSECONDS_PER_SECOND,
		frequency_hz, &fractional, &fractional_remainder);

	/* Checks the operation status. */
	if (error != 0)
		return error;
	(void)fractional_remainder;

	/* Handles the fractional condition. */
	if (fractional > UINT64_MAX - result)
		return EOVERFLOW;
	*microseconds = result + fractional;
	/* Reports successful completion. */
	return 0;
}

/* Supports the ax211 backend counter read checked operation. */
static int
ax211_backend_counter_read_checked(
	struct intel_ax211_pci_mmio_backend *backend,
	uint64_t *ticks)
{
	uint64_t current;
	uint64_t frequency_hz;
	uint64_t observed;

	/* Handles the backend availability. */
	if (backend == NULL || ticks == NULL || !backend->counter_ready)
		return EINVAL;

	/* Checks the hal rtc read counter result. */
	if (!hal_rtc_read_counter(&current, &frequency_hz) ||
	    frequency_hz != backend->counter_frequency_hz)

		/* Returns the computed result. */
		return EIO;
	observed = __atomic_load_n(&backend->last_counter, __ATOMIC_ACQUIRE);
	/* Continue until the operation reaches a terminal state. */
	for (;;) {
		/* Handles the current condition. */
		if (current < observed)
			return EIO;

		/* Handles the current condition. */
		if (current == observed)
			break;

		/* Checks the atomic compare exchange n result. */
		if (__atomic_compare_exchange_n(
			    &backend->last_counter, &observed, current, 0,
			    __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
			break;
	}
	*ticks = current;
	/* Reports successful completion. */
	return 0;
}

/* Supports the ax211 backend microseconds publish operation. */
static int
ax211_backend_microseconds_publish(
	struct intel_ax211_pci_mmio_backend *backend,
	uint64_t microseconds)
{
	uint64_t observed;

	observed =
		__atomic_load_n(&backend->last_microseconds, __ATOMIC_ACQUIRE);
	/* Continue until the operation reaches a terminal state. */
	for (;;) {
		/* Handles the microseconds condition. */
		if (microseconds < observed)
			return EIO;

		/* Handles the microseconds condition. */
		if (microseconds == observed)
			return 0;

		/* Checks the atomic compare exchange n result. */
		if (__atomic_compare_exchange_n(&backend->last_microseconds,
						&observed, microseconds, 0,
						__ATOMIC_ACQ_REL,
						__ATOMIC_ACQUIRE))

			/* Reports successful completion. */
			return 0;
	}
}

/* Supports the ax211 backend delay us operation. */
static int
ax211_backend_delay_us(
	void *argument,
	uint32_t duration_us)
{
	struct intel_ax211_pci_mmio_backend *backend = argument;
	uint64_t deadline;
	uint64_t now;
	uint64_t wait_ticks;
	uint64_t yield_threshold_ticks;
	int error;

	/* Handles the backend availability. */
	if (backend == NULL || backend->registers == NULL ||
	    !backend->counter_ready)

		/* Returns the computed result. */
		return EINVAL;

	/* Handles the duration us condition. */
	if (duration_us == 0U)
		return 0;
	error = ax211_backend_ticks_for_us(backend->counter_frequency_hz,
					   duration_us, &wait_ticks);

	/* Checks the operation status. */
	if (error != 0)
		return error;
	error = ax211_backend_ticks_for_us(backend->counter_frequency_hz,
					   AX211_BUSY_WAIT_WINDOW_US,
					   &yield_threshold_ticks);

	/* Checks the operation status. */
	if (error != 0)
		return error;
	error = ax211_backend_counter_read_checked(backend, &now);

	/* Checks the operation status. */
	if (error != 0)
		return error;

	/* Handles the wait ticks condition. */
	if (wait_ticks > UINT64_MAX - now)
		return EOVERFLOW;
	deadline = now + wait_ticks;
	/* Continue until the operation reaches a terminal state. */
	for (;;) {
		error = ax211_backend_counter_read_checked(backend, &now);

		/* Checks the operation status. */
		if (error != 0)
			return error;

		/* Handles the now condition. */
		if (now >= deadline)
			return 0;

		/* Handles the deadline condition. */
		if (deadline - now >= yield_threshold_ticks)
			sched_yield();
		else
			ax211_backend_relax();
	}
}

/* Supports the ax211 backend clock us operation. */
static int
ax211_backend_clock_us(
	void *argument,
	uint64_t *time_us)
{
	struct intel_ax211_pci_mmio_backend *backend = argument;
	uint64_t elapsed;
	uint64_t now;
	uint64_t converted;
	int error;

	/* Handles the backend availability. */
	if (backend == NULL || backend->registers == NULL || time_us == NULL ||
	    !backend->counter_ready)

		/* Returns the computed result. */
		return EINVAL;
	error = ax211_backend_counter_read_checked(backend, &now);

	/* Checks the operation status. */
	if (error != 0)
		return error;

	/* Handles the now condition. */
	if (now < backend->counter_origin)
		return EIO;
	elapsed = now - backend->counter_origin;
	error = ax211_backend_ticks_to_us(backend->counter_frequency_hz,
					  elapsed, &converted);

	/* Checks the operation status. */
	if (error != 0)
		return error;
	error = ax211_backend_microseconds_publish(backend, converted);

	/* Checks the operation status. */
	if (error != 0)
		return error;
	*time_us = converted;
	/* Reports successful completion. */
	return 0;
}

#ifdef INTEL_AX211_PCI_MMIO_HOST_TEST
/*
 * Implements the drv intel ax211 pci mmio host ticks for us operation.
 */
int
drv_intel_ax211_pci_mmio_host_ticks_for_us(
	uint64_t frequency_hz,
	uint64_t microseconds,
	uint64_t *ticks)
{
	int function_result;

	/* Obtains the ax211 backend ticks for us result. */
	function_result =
		ax211_backend_ticks_for_us(frequency_hz, microseconds, ticks);

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the drv intel ax211 pci mmio host ticks to us operation.
 */
int
drv_intel_ax211_pci_mmio_host_ticks_to_us(
	uint64_t frequency_hz,
	uint64_t ticks,
	uint64_t *microseconds)
{
	int function_result;

	/* Obtains the ax211 backend ticks to us result. */
	function_result =
		ax211_backend_ticks_to_us(frequency_hz, ticks, microseconds);

	/* Returns the computed result. */
	return function_result;
}
#endif
