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

#define AX211_PCI_MMIO_MINIMUM_SIZE	0x4000U
#define AX211_HBUS_PRPH_WRITE_ADDRESS	0x0444U
#define AX211_HBUS_PRPH_READ_ADDRESS	0x0448U
#define AX211_HBUS_PRPH_WRITE_DATA	0x044cU
#define AX211_HBUS_PRPH_READ_DATA	0x0450U
#define AX211_PRPH_ACCESS_DWORD		0x03000000U
#define AX211_PRPH_ADDRESS_MASK		0x00ffffffU
#define AX211_MICROSECONDS_PER_SECOND	1000000ULL
#define AX211_COUNTER_FREQUENCY_MIN_HZ	1000000ULL
#define AX211_COUNTER_FREQUENCY_MAX_HZ	10000000000ULL
#define AX211_BUSY_WAIT_WINDOW_US	1000U

static int ax211_backend_csr_read32(void *argument, uint32_t offset,
	uint32_t *value);
static int ax211_backend_csr_write32(void *argument, uint32_t offset,
	uint32_t value);
static int ax211_backend_prph_read32(void *argument, uint32_t address,
	uint32_t *value);
static int ax211_backend_prph_write32(void *argument, uint32_t address,
	uint32_t value);
static int ax211_backend_delay_us(void *argument, uint32_t duration_us);
static int ax211_backend_clock_us(void *argument, uint64_t *time_us);
static void ax211_backend_relax(void);
static int ax211_backend_ticks_for_us(uint64_t frequency_hz,
	uint64_t microseconds, uint64_t *ticks);
static int ax211_backend_ticks_to_us(uint64_t frequency_hz, uint64_t ticks,
	uint64_t *microseconds);
static int ax211_backend_mul_div_reduced(uint64_t numerator,
	uint64_t multiplier, uint64_t divisor, uint64_t *quotient,
	uint64_t *remainder);
static int ax211_backend_counter_read_checked(
	struct intel_ax211_pci_mmio_backend *backend, uint64_t *ticks);
static int ax211_backend_microseconds_publish(
	struct intel_ax211_pci_mmio_backend *backend, uint64_t microseconds);

static const struct intel_ax211_mmio_ops ax211_pci_mmio_operations = {
	.csr_read32 = ax211_backend_csr_read32,
	.csr_write32 = ax211_backend_csr_write32,
	.prph_read32 = ax211_backend_prph_read32,
	.prph_write32 = ax211_backend_prph_write32,
	.delay_us = ax211_backend_delay_us,
	.clock_us = ax211_backend_clock_us,
	.trace_deadline = NULL
};

int
intel_ax211_pci_mmio_backend_init(
	struct intel_ax211_pci_mmio_backend *backend,
	void *registers,
	size_t mapping_size)
{
	struct intel_ax211_pci_mmio_backend candidate;
	uint64_t frequency_hz;
	uint64_t origin_counter;

	if (backend == NULL || registers == NULL ||
	    mapping_size < AX211_PCI_MMIO_MINIMUM_SIZE)
		return EINVAL;
	memset(&candidate, 0, sizeof(candidate));
	if (!hal_rtc_read_counter(&origin_counter, &frequency_hz) ||
	    frequency_hz < AX211_COUNTER_FREQUENCY_MIN_HZ ||
	    frequency_hz > AX211_COUNTER_FREQUENCY_MAX_HZ) {
		memset(backend, 0, sizeof(*backend));
		return ENOTSUP;
	}
	candidate.registers = registers;
	candidate.mapping_size = mapping_size;
	candidate.counter_frequency_hz = frequency_hz;
	candidate.counter_origin = origin_counter;
	candidate.last_counter = origin_counter;
	candidate.counter_ready = 1U;
	*backend = candidate;
	return 0;
}

const struct intel_ax211_mmio_ops *
intel_ax211_pci_mmio_ops(void)
{
	return &ax211_pci_mmio_operations;
}

static int
ax211_backend_range_valid(const struct intel_ax211_pci_mmio_backend *backend,
	uint32_t offset)
{
	if (backend == NULL || backend->registers == NULL ||
	    (offset & 3U) != 0U)
		return 0;
	return (size_t)offset <= backend->mapping_size &&
	    sizeof(uint32_t) <= backend->mapping_size - (size_t)offset;
}

static int
ax211_backend_csr_read32(
	void *argument,
	uint32_t offset,
	uint32_t *value)
{
	struct intel_ax211_pci_mmio_backend *backend = argument;

	if (value == NULL || !ax211_backend_range_valid(backend, offset))
		return EINVAL;
	*value = *(volatile uint32_t *)(backend->registers + offset);
	hal_io_rmb();
	if (*value == UINT32_MAX)
		return EIO;
	return 0;
}

static int
ax211_backend_csr_write32(
	void *argument,
	uint32_t offset,
	uint32_t value)
{
	struct intel_ax211_pci_mmio_backend *backend = argument;

	if (!ax211_backend_range_valid(backend, offset))
		return EINVAL;
	*(volatile uint32_t *)(backend->registers + offset) = value;
	hal_io_wmb();
	return 0;
}

static int
ax211_backend_prph_read32(
	void *argument,
	uint32_t address,
	uint32_t *value)
{
	struct intel_ax211_pci_mmio_backend *backend = argument;
	int error;

	if (address > AX211_PRPH_ADDRESS_MASK || value == NULL)
		return EINVAL;
	error = ax211_backend_csr_write32(backend,
	    AX211_HBUS_PRPH_READ_ADDRESS,
	    address | AX211_PRPH_ACCESS_DWORD);
	if (error != 0)
		return error;
	hal_io_mb();
	return ax211_backend_csr_read32(backend, AX211_HBUS_PRPH_READ_DATA,
	    value);
}

static int
ax211_backend_prph_write32(
	void *argument,
	uint32_t address,
	uint32_t value)
{
	struct intel_ax211_pci_mmio_backend *backend = argument;
	int error;

	if (address > AX211_PRPH_ADDRESS_MASK)
		return EINVAL;
	error = ax211_backend_csr_write32(backend,
	    AX211_HBUS_PRPH_WRITE_ADDRESS,
	    address | AX211_PRPH_ACCESS_DWORD);
	if (error != 0)
		return error;
	hal_io_wmb();
	return ax211_backend_csr_write32(backend, AX211_HBUS_PRPH_WRITE_DATA,
	    value);
}

static void
ax211_backend_relax(void)
{
#ifdef INTEL_AX211_PCI_MMIO_HOST_TEST
	extern void intel_ax211_pci_mmio_host_relax(void);

	intel_ax211_pci_mmio_host_relax();
#else
	hal_atomic_relax();
#endif
}

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

	if (divisor == 0U || numerator >= divisor || quotient == NULL ||
	    remainder == NULL)
		return EINVAL;
	reduced = 0U;
	result = 0U;
	mask = UINT64_C(1) << 63;

	/* Accumulate the exact quotient and remainder one multiplier bit at a time. */
	while (mask != 0U) {
		threshold = divisor - reduced;
		if (reduced >= threshold) {
			reduced -= threshold;
			carry = 1U;
		} else {
			reduced += reduced;
			carry = 0U;
		}
		if (result > UINT64_MAX / 2U)
			return EOVERFLOW;
		result = result * 2U + carry;
		if ((multiplier & mask) != 0U) {
			threshold = divisor - numerator;
			if (reduced >= threshold) {
				reduced -= threshold;
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
	return 0;
}

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

	if (frequency_hz == 0U || ticks == NULL)
		return EINVAL;
	whole = frequency_hz / AX211_MICROSECONDS_PER_SECOND;
	if (whole != 0U && microseconds > UINT64_MAX / whole)
		return EOVERFLOW;
	result = whole * microseconds;
	error = ax211_backend_mul_div_reduced(
	    frequency_hz % AX211_MICROSECONDS_PER_SECOND,
	    microseconds, AX211_MICROSECONDS_PER_SECOND, &fractional,
	    &fractional_remainder);
	if (error != 0)
		return error;
	if (fractional > UINT64_MAX - result)
		return EOVERFLOW;
	result += fractional;
	if (fractional_remainder != 0U) {
		if (result == UINT64_MAX)
			return EOVERFLOW;
		result++;
	}
	*ticks = result;
	return 0;
}

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

	if (frequency_hz == 0U || microseconds == NULL)
		return EINVAL;
	whole = ticks / frequency_hz;
	if (whole > UINT64_MAX / AX211_MICROSECONDS_PER_SECOND)
		return EOVERFLOW;
	result = whole * AX211_MICROSECONDS_PER_SECOND;
	error = ax211_backend_mul_div_reduced(ticks % frequency_hz,
	    AX211_MICROSECONDS_PER_SECOND, frequency_hz, &fractional,
	    &fractional_remainder);
	if (error != 0)
		return error;
	(void)fractional_remainder;
	if (fractional > UINT64_MAX - result)
		return EOVERFLOW;
	*microseconds = result + fractional;
	return 0;
}

static int
ax211_backend_counter_read_checked(
	struct intel_ax211_pci_mmio_backend *backend,
	uint64_t *ticks)
{
	uint64_t current;
	uint64_t frequency_hz;
	uint64_t observed;

	if (backend == NULL || ticks == NULL || !backend->counter_ready)
		return EINVAL;
	if (!hal_rtc_read_counter(&current, &frequency_hz) ||
	    frequency_hz != backend->counter_frequency_hz)
		return EIO;
	observed = __atomic_load_n(&backend->last_counter, __ATOMIC_ACQUIRE);
	for (;;) {
		if (current < observed)
			return EIO;
		if (current == observed)
			break;
		if (__atomic_compare_exchange_n(&backend->last_counter, &observed,
		    current, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
			break;
	}
	*ticks = current;
	return 0;
}

static int
ax211_backend_microseconds_publish(
	struct intel_ax211_pci_mmio_backend *backend,
	uint64_t microseconds)
{
	uint64_t observed;

	observed = __atomic_load_n(&backend->last_microseconds, __ATOMIC_ACQUIRE);
	for (;;) {
		if (microseconds < observed)
			return EIO;
		if (microseconds == observed)
			return 0;
		if (__atomic_compare_exchange_n(&backend->last_microseconds,
		    &observed, microseconds, 0, __ATOMIC_ACQ_REL,
		    __ATOMIC_ACQUIRE))
			return 0;
	}
}

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

	if (backend == NULL || backend->registers == NULL ||
	    !backend->counter_ready)
		return EINVAL;
	if (duration_us == 0U)
		return 0;
	error = ax211_backend_ticks_for_us(backend->counter_frequency_hz,
	    duration_us, &wait_ticks);
	if (error != 0)
		return error;
	error = ax211_backend_ticks_for_us(backend->counter_frequency_hz,
	    AX211_BUSY_WAIT_WINDOW_US, &yield_threshold_ticks);
	if (error != 0)
		return error;
	error = ax211_backend_counter_read_checked(backend, &now);
	if (error != 0)
		return error;
	if (wait_ticks > UINT64_MAX - now)
		return EOVERFLOW;
	deadline = now + wait_ticks;
	for (;;) {
		error = ax211_backend_counter_read_checked(backend, &now);
		if (error != 0)
			return error;
		if (now >= deadline)
			return 0;
		if (deadline - now >= yield_threshold_ticks)
			sched_yield();
		else
			ax211_backend_relax();
	}
}

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

	if (backend == NULL || backend->registers == NULL || time_us == NULL ||
	    !backend->counter_ready)
		return EINVAL;
	error = ax211_backend_counter_read_checked(backend, &now);
	if (error != 0)
		return error;
	if (now < backend->counter_origin)
		return EIO;
	elapsed = now - backend->counter_origin;
	error = ax211_backend_ticks_to_us(backend->counter_frequency_hz, elapsed,
	    &converted);
	if (error != 0)
		return error;
	error = ax211_backend_microseconds_publish(backend, converted);
	if (error != 0)
		return error;
	*time_us = converted;
	return 0;
}

#ifdef INTEL_AX211_PCI_MMIO_HOST_TEST
int
intel_ax211_pci_mmio_host_ticks_for_us(
	uint64_t frequency_hz,
	uint64_t microseconds,
	uint64_t *ticks)
{
	return ax211_backend_ticks_for_us(frequency_hz, microseconds, ticks);
}

int
intel_ax211_pci_mmio_host_ticks_to_us(
	uint64_t frequency_hz,
	uint64_t ticks,
	uint64_t *microseconds)
{
	return ax211_backend_ticks_to_us(frequency_hz, ticks, microseconds);
}
#endif
