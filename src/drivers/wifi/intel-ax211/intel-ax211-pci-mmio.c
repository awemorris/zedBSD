/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Intel AX211 PCI BAR backend.
 */

#include "intel-ax211-pci-mmio.h"
#include <kern/kcrt.h>

#include <uapi/errno.h>
#include <kern/sched.h>
#include <limits.h>
#include "kern/device-io.h"
#include "kern/atomic.h"
#include "kern/clock.h"

/* The smallest BAR0 mapping that covers every register the driver uses. */
#define AX211_PCI_MMIO_MINIMUM_SIZE 0x4000U

/*
 * The indirect window into the periphery (PRPH) register space.
 *
 * Periphery registers are not mapped in BAR0.  A read writes the 24-bit
 * periphery address, with the access size code 3 (a full 32-bit word) in bits
 * 25:24, to READ_ADDRESS and then reads READ_DATA; a write writes the address
 * the same way to WRITE_ADDRESS and then the value to WRITE_DATA.  The device
 * must be held awake for the whole access, which the MMIO layer checks before
 * it calls this backend.
 *
 * Values referenced: Linux v6.17 drivers/net/wireless/intel/iwlwifi/iwl-csr.h
 * (the target periphery write address, read address, write data and read data
 * registers).
 */
#define AX211_CSR_PRPH_WRITE_ADDRESS 0x0444U
#define AX211_CSR_PRPH_READ_ADDRESS 0x0448U
#define AX211_CSR_PRPH_WRITE_DATA 0x044cU
#define AX211_CSR_PRPH_READ_DATA 0x0450U
#define AX211_PRPH_ACCESS_FULL_WORD 0x03000000U
#define AX211_PRPH_ADDRESS_MASK 0x00ffffffU

/* zedBSD's bounds of the counter that times the busy waits. */
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

/*
 * The MMIO operations of the PCI BAR backend.
 *
 * The MMIO layer calls them with the backend as the argument; the table lives
 * as long as the driver, and no deadline trace is installed.
 */
static const struct intel_ax211_mmio_ops ax211_pci_mmio_operations = {
	.csr_read32 = ax211_backend_csr_read32,
	.csr_write32 = ax211_backend_csr_write32,
	.prph_read32 = ax211_backend_prph_read32,
	.prph_write32 = ax211_backend_prph_write32,
	.delay_us = ax211_backend_delay_us,
	.clock_us = ax211_backend_clock_us,
	.trace_deadline = NULL};

static void ax211_backend_relax(void);
static int ax211_backend_ticks_for_us(uint64_t frequency_hz, uint64_t microseconds, uint64_t *ticks);
static int ax211_backend_ticks_to_us(uint64_t frequency_hz, uint64_t ticks, uint64_t *microseconds);
static int ax211_backend_mul_div_reduced(uint64_t numerator, uint64_t multiplier, uint64_t divisor, uint64_t *quotient, uint64_t *remainder);
static int ax211_backend_counter_read_checked(struct intel_ax211_pci_mmio_backend *backend, uint64_t *ticks);
static int ax211_backend_microseconds_publish(struct intel_ax211_pci_mmio_backend *backend, uint64_t microseconds);
static int ax211_backend_range_valid(const struct intel_ax211_pci_mmio_backend *backend, uint32_t offset);

/*
 * Binds a backend to a mapped BAR0 and to the platform's counter.
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
	int counter_read;

	/*
	 * Refuses a missing mapping or one too small for the driver's
	 * registers.
	 */
	if (backend == NULL ||
	    registers == NULL ||
	    mapping_size < AX211_PCI_MMIO_MINIMUM_SIZE) {
		return EINVAL;
	}

	/* Reads the platform counter, which times every busy wait. */
	kern_memset(&candidate, 0, sizeof(candidate));
	counter_read = kern_rtc_read_counter(&origin_counter, &frequency_hz);

	/*
	 * Refuses a platform without a counter, or with one of unusable
	 * frequency.
	 */
	if (!counter_read ||
	    frequency_hz < AX211_COUNTER_FREQUENCY_MIN_HZ ||
	    frequency_hz > AX211_COUNTER_FREQUENCY_MAX_HZ) {
		kern_memset(backend, 0, sizeof(*backend));
		return ENOTSUP;
	}

	/* Publishes the mapping with the counter's origin as time zero. */
	candidate.registers = registers;
	candidate.mapping_size = mapping_size;
	candidate.counter_frequency_hz = frequency_hz;
	candidate.counter_origin = origin_counter;
	candidate.last_counter = origin_counter;
	candidate.counter_ready = 1U;
	*backend = candidate;

	/* Succeeded: the backend is ready for the MMIO layer. */
	return 0;
}

/*
 * Reports the MMIO operations of the PCI BAR backend.
 */
const struct intel_ax211_mmio_ops *
drv_intel_ax211_pci_mmio_ops(
	void)
{
	/* The table is the same for every device. */
	return &ax211_pci_mmio_operations;
}

#ifdef INTEL_AX211_PCI_MMIO_HOST_TEST
/*
 * Converts microseconds to counter ticks for the host test.
 */
int
drv_intel_ax211_pci_mmio_host_ticks_for_us(
	uint64_t frequency_hz,
	uint64_t microseconds,
	uint64_t *ticks)
{
	int error;

	/* Converts the duration. */
	error =
		ax211_backend_ticks_for_us(frequency_hz, microseconds, ticks);
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Converts counter ticks to microseconds for the host test.
 */
int
drv_intel_ax211_pci_mmio_host_ticks_to_us(
	uint64_t frequency_hz,
	uint64_t ticks,
	uint64_t *microseconds)
{
	int error;

	/* Converts the count. */
	error =
		ax211_backend_ticks_to_us(frequency_hz, ticks, microseconds);
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}
#endif

/*
 * Reports whether a 32-bit register lies inside the mapping: 1 when it does, 0
 * when not.
 */
static int
ax211_backend_range_valid(
	const struct intel_ax211_pci_mmio_backend *backend,
	uint32_t offset)
{
	/* Rejects a missing mapping and an unaligned offset. */
	if (backend == NULL ||
	    backend->registers == NULL ||
	    (offset & 3U) != 0U) {
		return 0;
	}

	/* Rejects an offset past the end of the mapping. */
	if ((size_t)offset > backend->mapping_size)
		return 0;

	/*
	 * Rejects a register that does not fit before the end of the mapping.
	 */
	if (sizeof(uint32_t) > backend->mapping_size - (size_t)offset)
		return 0;

	/* The register lies inside the mapping. */
	return 1;
}

/* Reads a 32-bit register of BAR0. */
static int
ax211_backend_csr_read32(
	void *argument,
	uint32_t offset,
	uint32_t *value)
{
	struct intel_ax211_pci_mmio_backend *backend = argument;
	int valid;

	/* Refuses a missing output. */
	if (value == NULL)
		return EINVAL;

	/* Refuses a register outside the mapping. */
	valid = ax211_backend_range_valid(backend, offset);
	if (!valid)
		return EINVAL;

	/* Reads the register, ordered before any later read of DMA memory. */
	*value = *(volatile uint32_t *)(backend->registers + offset);
	kern_io_read_barrier();

	/*
	 * A register that reads all ones means the device has dropped off the
	 * bus.
	 */
	if (*value == UINT32_MAX)
		return EIO;

	/* Succeeded. */
	return 0;
}

/* Writes a 32-bit register of BAR0. */
static int
ax211_backend_csr_write32(
	void *argument,
	uint32_t offset,
	uint32_t value)
{
	struct intel_ax211_pci_mmio_backend *backend = argument;
	int valid;

	/* Refuses a register outside the mapping. */
	valid = ax211_backend_range_valid(backend, offset);
	if (!valid)
		return EINVAL;

	/* Writes the register, ordered after earlier writes. */
	*(volatile uint32_t *)(backend->registers + offset) = value;
	kern_io_write_barrier();

	/* Succeeded. */
	return 0;
}

/* Reads a periphery register through the indirect window. */
static int
ax211_backend_prph_read32(
	void *argument,
	uint32_t address,
	uint32_t *value)
{
	struct intel_ax211_pci_mmio_backend *backend = argument;
	int error;

	/*
	 * Refuses an address beyond the periphery space, or a missing output.
	 */
	if (address > AX211_PRPH_ADDRESS_MASK || value == NULL)
		return EINVAL;

	/* Selects the register and the full-word access. */
	error = ax211_backend_csr_write32(backend, AX211_CSR_PRPH_READ_ADDRESS,
					  address | AX211_PRPH_ACCESS_FULL_WORD);
	if (error != 0)
		return error;

	/* Orders the selection before the data read. */
	kern_io_barrier();

	/* Reads the data. */
	error = ax211_backend_csr_read32(
		backend, AX211_CSR_PRPH_READ_DATA, value);
	if (error != 0)
		return error;

	/* Succeeded: *value holds the register. */
	return 0;
}

/* Writes a periphery register through the indirect window. */
static int
ax211_backend_prph_write32(
	void *argument,
	uint32_t address,
	uint32_t value)
{
	struct intel_ax211_pci_mmio_backend *backend = argument;
	int error;

	/* Refuses an address beyond the periphery space. */
	if (address > AX211_PRPH_ADDRESS_MASK)
		return EINVAL;

	/* Selects the register and the full-word access. */
	error = ax211_backend_csr_write32(backend,
					  AX211_CSR_PRPH_WRITE_ADDRESS,
					  address | AX211_PRPH_ACCESS_FULL_WORD);
	if (error != 0)
		return error;

	/* Orders the selection before the data write. */
	kern_io_write_barrier();

	/* Writes the data. */
	error = ax211_backend_csr_write32(
		backend, AX211_CSR_PRPH_WRITE_DATA, value);
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Spends one short pause of a busy wait. */
static void
ax211_backend_relax(
	void)
{
#ifdef INTEL_AX211_PCI_MMIO_HOST_TEST
	extern void intel_ax211_pci_mmio_host_relax(void);

	/* The host test counts the pauses instead of spinning. */
	intel_ax211_pci_mmio_host_relax();
#else
	/* Tells the processor that this is a spin loop. */
	atomic_spin_hint();
#endif
}

/* Computes numerator * multiplier / divisor exactly without a 128-bit type. */
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

	/*
	 * Refuses a zero divisor, a numerator not below it, and missing
	 * outputs.
	 */
	if (divisor == 0U ||
	    numerator >= divisor ||
	    quotient == NULL ||
	    remainder == NULL) {
		return EINVAL;
	}

	/* Starts from zero with the most significant multiplier bit. */
	reduced = 0U;
	result = 0U;
	mask = UINT64_C(1) << 63;

	/*
	 * Accumulate the exact quotient and remainder one multiplier bit at a
	 * time.
	 */
	while (mask != 0U) {
		/*
		 * Doubles the remainder modulo the divisor, carrying into the
		 * quotient.
		 */
		threshold = divisor - reduced;
		if (reduced >= threshold) {
			reduced -= threshold;
			carry = 1U;
		} else {
			reduced += reduced;
			carry = 0U;
		}

		/* Refuses a quotient that overflows 64 bits. */
		if (result > UINT64_MAX / 2U)
			return EOVERFLOW;
		result = result * 2U + carry;

		/* Adds the numerator for a set multiplier bit. */
		if ((multiplier & mask) != 0U) {
			/*
			 * Adds it modulo the divisor, carrying into the
			 * quotient.
			 */
			threshold = divisor - numerator;
			if (reduced >= threshold) {
				reduced -= threshold;

				/* Refuses a quotient that overflows 64 bits. */
				if (result == UINT64_MAX)
					return EOVERFLOW;
				result++;
			} else {
				reduced += numerator;
			}
		}

		/* Moves to the next lower multiplier bit. */
		mask >>= 1;
	}

	/* Reports the quotient and the remainder. */
	*quotient = result;
	*remainder = reduced;

	/* Succeeded. */
	return 0;
}

/* Converts microseconds to counter ticks, rounding up. */
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

	/* Refuses a zero frequency and a missing output. */
	if (frequency_hz == 0U || ticks == NULL)
		return EINVAL;

	/* Converts the whole ticks per microsecond, refusing an overflow. */
	whole = frequency_hz / AX211_MICROSECONDS_PER_SECOND;
	if (whole != 0U && microseconds > UINT64_MAX / whole)
		return EOVERFLOW;
	result = whole * microseconds;

	/* Adds the fractional ticks per microsecond times the duration. */
	error = ax211_backend_mul_div_reduced(
		frequency_hz % AX211_MICROSECONDS_PER_SECOND, microseconds,
		AX211_MICROSECONDS_PER_SECOND, &fractional,
		&fractional_remainder);
	if (error != 0)
		return error;

	/* Refuses a sum that overflows. */
	if (fractional > UINT64_MAX - result)
		return EOVERFLOW;
	result += fractional;

	/* Rounds a remaining fraction of a tick up. */
	if (fractional_remainder != 0U) {
		/* Refuses a count that overflows. */
		if (result == UINT64_MAX)
			return EOVERFLOW;
		result++;
	}

	/* Reports the tick count. */
	*ticks = result;

	/* Succeeded. */
	return 0;
}

/* Converts counter ticks to microseconds, rounding down. */
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

	/* Refuses a zero frequency and a missing output. */
	if (frequency_hz == 0U || microseconds == NULL)
		return EINVAL;

	/* Converts the whole seconds, refusing an overflow. */
	whole = ticks / frequency_hz;
	if (whole > UINT64_MAX / AX211_MICROSECONDS_PER_SECOND)
		return EOVERFLOW;
	result = whole * AX211_MICROSECONDS_PER_SECOND;

	/*
	 * Adds the microseconds of the remaining ticks; the fraction is
	 * dropped.
	 */
	error = ax211_backend_mul_div_reduced(
		ticks % frequency_hz, AX211_MICROSECONDS_PER_SECOND,
		frequency_hz, &fractional, &fractional_remainder);
	if (error != 0)
		return error;
	(void)fractional_remainder;

	/* Refuses a sum that overflows. */
	if (fractional > UINT64_MAX - result)
		return EOVERFLOW;
	*microseconds = result + fractional;

	/* Succeeded. */
	return 0;
}

/* Reads the counter and keeps the reading monotonic across callers. */
static int
ax211_backend_counter_read_checked(
	struct intel_ax211_pci_mmio_backend *backend,
	uint64_t *ticks)
{
	uint64_t current;
	uint64_t frequency_hz;
	uint64_t observed;
	int counter_read;
	int exchanged;

	/* Refuses a backend without a counter, and a missing output. */
	if (backend == NULL || ticks == NULL || !backend->counter_ready)
		return EINVAL;

	/* Reads the counter. */
	counter_read = kern_rtc_read_counter(&current, &frequency_hz);

	/* Refuses a failed read and a counter whose frequency changed. */
	if (!counter_read ||
	    frequency_hz != backend->counter_frequency_hz) {
		return EIO;
	}

	/*
	 * Advances the last reading to this one, or fails if the counter went
	 * back.
	 */
	observed = __atomic_load_n(&backend->last_counter, __ATOMIC_ACQUIRE);
	for (;;) {
		/* A reading behind the last one means the counter went back. */
		if (current < observed)
			return EIO;

		/* The last reading is already this one. */
		if (current == observed)
			break;

		/*
		 * Publishes the reading; a failed exchange reloads observed and
		 * retries.
		 */
		exchanged = __atomic_compare_exchange_n(
			&backend->last_counter, &observed, current, 0,
			__ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
		if (exchanged)
			break;
	}

	/* Reports the reading. */
	*ticks = current;

	/* Succeeded. */
	return 0;
}

/* Advances the last reported time, refusing a time that went back. */
static int
ax211_backend_microseconds_publish(
	struct intel_ax211_pci_mmio_backend *backend,
	uint64_t microseconds)
{
	uint64_t observed;
	int exchanged;

	/* Advances the last reported time to this one. */
	observed =
		__atomic_load_n(&backend->last_microseconds, __ATOMIC_ACQUIRE);
	for (;;) {
		/* A time behind the last one means the clock went back. */
		if (microseconds < observed)
			return EIO;

		/* The last reported time is already this one. */
		if (microseconds == observed)
			return 0;

		/*
		 * Publishes the time; a failed exchange reloads observed and
		 * retries.
		 */
		exchanged = __atomic_compare_exchange_n(&backend->last_microseconds,
							&observed, microseconds, 0,
							__ATOMIC_ACQ_REL,
							__ATOMIC_ACQUIRE);
		if (exchanged)
			return 0;
	}
}

/*
 * Waits the given time, yielding while the wait is long and spinning at its
 * end.
 */
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

	/* Refuses a backend without a mapping or a counter. */
	if (backend == NULL ||
	    backend->registers == NULL ||
	    !backend->counter_ready) {
		return EINVAL;
	}

	/* Nothing to wait for. */
	if (duration_us == 0U)
		return 0;

	/* Converts the wait into counter ticks. */
	error = ax211_backend_ticks_for_us(backend->counter_frequency_hz,
					   duration_us, &wait_ticks);
	if (error != 0)
		return error;

	/*
	 * Converts the window below which the wait spins instead of yielding.
	 */
	error = ax211_backend_ticks_for_us(backend->counter_frequency_hz,
					   AX211_BUSY_WAIT_WINDOW_US,
					   &yield_threshold_ticks);
	if (error != 0)
		return error;

	/* Reads the start of the wait. */
	error = ax211_backend_counter_read_checked(backend, &now);
	if (error != 0)
		return error;

	/* Refuses a deadline that overflows the counter. */
	if (wait_ticks > UINT64_MAX - now)
		return EOVERFLOW;
	deadline = now + wait_ticks;

	/* Waits until the counter passes the deadline. */
	for (;;) {
		/* Reads the counter. */
		error = ax211_backend_counter_read_checked(backend, &now);
		if (error != 0)
			return error;

		/* The deadline has passed. */
		if (now >= deadline)
			return 0;

		/*
		 * Yields while the rest of the wait is long, spins when it is
		 * short.
		 */
		if (deadline - now >= yield_threshold_ticks)
			sched_yield();
		else
			ax211_backend_relax();
	}
}

/* Reports the microseconds since the backend was bound. */
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

	/*
	 * Refuses a backend without a mapping or a counter, and a missing
	 * output.
	 */
	if (backend == NULL ||
	    backend->registers == NULL ||
	    time_us == NULL ||
	    !backend->counter_ready) {
		return EINVAL;
	}

	/* Reads the counter. */
	error = ax211_backend_counter_read_checked(backend, &now);
	if (error != 0)
		return error;

	/* Refuses a reading before the origin. */
	if (now < backend->counter_origin)
		return EIO;

	/* Converts the ticks since the origin. */
	elapsed = now - backend->counter_origin;
	error = ax211_backend_ticks_to_us(backend->counter_frequency_hz,
					  elapsed, &converted);
	if (error != 0)
		return error;

	/* Refuses a time behind the last one reported. */
	error = ax211_backend_microseconds_publish(backend, converted);
	if (error != 0)
		return error;

	/* Reports the time. */
	*time_us = converted;

	/* Succeeded. */
	return 0;
}
