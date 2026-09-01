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
#define AX211_CPUID_TSC			(1U << 4)
#define AX211_CPUID_INVARIANT_TSC	(1U << 8)
#define AX211_CPUID_LEAF_TSC_RATIO	0x00000015U
#define AX211_CPUID_EXTENDED_MAX		0x80000000U
#define AX211_CPUID_EXTENDED_POWER	0x80000007U
#define AX211_TSC_FREQUENCY_MIN_HZ	1000000ULL
#define AX211_TSC_FREQUENCY_MAX_HZ	10000000000ULL
#define AX211_CPUID_VENDOR_GENU		0x756e6547U
#define AX211_CPUID_VENDOR_INEI		0x49656e69U
#define AX211_CPUID_VENDOR_NTEL		0x6c65746eU
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
static void ax211_backend_cpuid(uint32_t leaf, uint32_t subleaf,
	uint32_t *eax, uint32_t *ebx, uint32_t *ecx, uint32_t *edx);
static uint64_t ax211_backend_rdtsc(void);
static void ax211_backend_relax(void);
static int ax211_backend_calibrate_tsc(uint64_t *frequency_hz,
	uint64_t *origin_tsc);
static int ax211_backend_ticks_for_us(uint64_t frequency_hz,
	uint64_t microseconds, uint64_t *ticks);
static int ax211_backend_ticks_to_us(uint64_t frequency_hz, uint64_t ticks,
	uint64_t *microseconds);
static int ax211_backend_mul_div_reduced(uint64_t numerator,
	uint64_t multiplier, uint64_t divisor, uint64_t *quotient,
	uint64_t *remainder);
static int ax211_backend_tsc_read_checked(
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
	uint64_t origin_tsc;
	int error;

	if (backend == NULL || registers == NULL ||
	    mapping_size < AX211_PCI_MMIO_MINIMUM_SIZE)
		return EINVAL;
	memset(&candidate, 0, sizeof(candidate));
	error = ax211_backend_calibrate_tsc(&frequency_hz, &origin_tsc);
	if (error != 0) {
		memset(backend, 0, sizeof(*backend));
		return error;
	}
	candidate.registers = registers;
	candidate.mapping_size = mapping_size;
	candidate.tsc_frequency_hz = frequency_hz;
	candidate.tsc_origin = origin_tsc;
	candidate.last_tsc = origin_tsc;
	candidate.tsc_calibrated = 1U;
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
ax211_backend_cpuid(
	uint32_t leaf,
	uint32_t subleaf,
	uint32_t *eax,
	uint32_t *ebx,
	uint32_t *ecx,
	uint32_t *edx)
{
#ifdef INTEL_AX211_PCI_MMIO_HOST_TEST
	extern void intel_ax211_pci_mmio_host_cpuid(uint32_t, uint32_t,
		uint32_t *, uint32_t *, uint32_t *, uint32_t *);

	intel_ax211_pci_mmio_host_cpuid(leaf, subleaf, eax, ebx, ecx, edx);
#elif defined(__amd64__) || defined(__x86_64__)
	uint32_t a = leaf;
	uint32_t b;
	uint32_t c = subleaf;
	uint32_t d;

	__asm__ volatile("cpuid" : "+a"(a), "=b"(b), "+c"(c), "=d"(d)
	    : : "memory");
	*eax = a;
	*ebx = b;
	*ecx = c;
	*edx = d;
#else
	(void)leaf;
	(void)subleaf;
	*eax = 0U;
	*ebx = 0U;
	*ecx = 0U;
	*edx = 0U;
#endif
}

static uint64_t
ax211_backend_rdtsc(void)
{
#ifdef INTEL_AX211_PCI_MMIO_HOST_TEST
	extern uint64_t intel_ax211_pci_mmio_host_rdtsc(void);

	return intel_ax211_pci_mmio_host_rdtsc();
#elif defined(__amd64__) || defined(__x86_64__)
	uint32_t high;
	uint32_t low;

	/* Intel documents LFENCE;RDTSC as ordered after prior local loads. */
	__asm__ volatile("lfence\n\trdtsc" : "=a"(low), "=d"(high) : :
	    "memory");
	return ((uint64_t)high << 32) | low;
#else
	return 0U;
#endif
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
ax211_backend_calibrate_tsc(
	uint64_t *frequency_hz,
	uint64_t *origin_tsc)
{
	uint32_t eax;
	uint32_t ebx;
	uint32_t ecx;
	uint32_t edx;
	uint64_t scaled_hz;
	uint64_t calibrated_hz;

	if (frequency_hz == NULL || origin_tsc == NULL)
		return EINVAL;
#if !defined(INTEL_AX211_PCI_MMIO_HOST_TEST) && \
    !defined(__amd64__) && !defined(__x86_64__)
	return ENOTSUP;
#endif
	ax211_backend_cpuid(0U, 0U, &eax, &ebx, &ecx, &edx);
	if (ebx != AX211_CPUID_VENDOR_GENU || edx != AX211_CPUID_VENDOR_INEI ||
	    ecx != AX211_CPUID_VENDOR_NTEL || eax < AX211_CPUID_LEAF_TSC_RATIO)
		return ENOTSUP;
	ax211_backend_cpuid(1U, 0U, &eax, &ebx, &ecx, &edx);
	if ((edx & AX211_CPUID_TSC) == 0U)
		return ENOTSUP;
	ax211_backend_cpuid(AX211_CPUID_EXTENDED_MAX, 0U, &eax, &ebx, &ecx,
	    &edx);
	if (eax < AX211_CPUID_EXTENDED_POWER)
		return ENOTSUP;
	ax211_backend_cpuid(AX211_CPUID_EXTENDED_POWER, 0U, &eax, &ebx, &ecx,
	    &edx);
	if ((edx & AX211_CPUID_INVARIANT_TSC) == 0U)
		return ENOTSUP;
	ax211_backend_cpuid(AX211_CPUID_LEAF_TSC_RATIO, 0U, &eax, &ebx, &ecx,
	    &edx);
	if (eax == 0U || ebx == 0U || ecx == 0U)
		return ENOTSUP;
	if ((uint64_t)ecx > UINT64_MAX / (uint64_t)ebx)
		return EOVERFLOW;
	scaled_hz = (uint64_t)ecx * (uint64_t)ebx;
	calibrated_hz = scaled_hz / eax;
	if (scaled_hz % eax != 0U) {
		if (calibrated_hz == UINT64_MAX)
			return EOVERFLOW;
		calibrated_hz++;
	}
	if (calibrated_hz < AX211_TSC_FREQUENCY_MIN_HZ ||
	    calibrated_hz > AX211_TSC_FREQUENCY_MAX_HZ)
		return ENOTSUP;

	/*
	 * CPUID.16 reports a nominal processor base frequency, not an
	 * architectural TSC frequency.  It is therefore deliberately not used as
	 * a fallback when CPUID.15 omits its crystal clock; an unsafe estimate
	 * could make a hardware deadline expire early.
	 */
	*frequency_hz = calibrated_hz;
	*origin_tsc = ax211_backend_rdtsc();
	return 0;
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
ax211_backend_tsc_read_checked(
	struct intel_ax211_pci_mmio_backend *backend,
	uint64_t *ticks)
{
	uint64_t current;
	uint64_t observed;

	if (backend == NULL || ticks == NULL || !backend->tsc_calibrated)
		return EINVAL;
	current = ax211_backend_rdtsc();
	observed = __atomic_load_n(&backend->last_tsc, __ATOMIC_ACQUIRE);
	for (;;) {
		if (current < observed)
			return EIO;
		if (current == observed)
			break;
		if (__atomic_compare_exchange_n(&backend->last_tsc, &observed,
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
	    !backend->tsc_calibrated)
		return EINVAL;
	if (duration_us == 0U)
		return 0;
	error = ax211_backend_ticks_for_us(backend->tsc_frequency_hz,
	    duration_us, &wait_ticks);
	if (error != 0)
		return error;
	error = ax211_backend_ticks_for_us(backend->tsc_frequency_hz,
	    AX211_BUSY_WAIT_WINDOW_US, &yield_threshold_ticks);
	if (error != 0)
		return error;
	error = ax211_backend_tsc_read_checked(backend, &now);
	if (error != 0)
		return error;
	if (wait_ticks > UINT64_MAX - now)
		return EOVERFLOW;
	deadline = now + wait_ticks;
	for (;;) {
		error = ax211_backend_tsc_read_checked(backend, &now);
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
	    !backend->tsc_calibrated)
		return EINVAL;
	error = ax211_backend_tsc_read_checked(backend, &now);
	if (error != 0)
		return error;
	if (now < backend->tsc_origin)
		return EIO;
	elapsed = now - backend->tsc_origin;
	error = ax211_backend_ticks_to_us(backend->tsc_frequency_hz, elapsed,
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
