/* Intel AX211 PCI BAR backend fixture. SPDX-License-Identifier: Zlib */

#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../../../src/drivers/intel-ax211-pci-mmio.h"

static uint64_t test_tsc;
static uint64_t test_counter_frequency;
static uint64_t test_relax_step;
static uint64_t test_yield_step;
static unsigned int test_relax_count;
static unsigned int test_yield_count;
static bool test_counter_available;

bool
hal_rtc_read_counter(uint64_t *counter, uint64_t *freq_hz)
{
	if (!test_counter_available || counter == NULL || freq_hz == NULL)
		return false;
	*counter = test_tsc;
	*freq_hz = test_counter_frequency;
	return true;
}

void
intel_ax211_pci_mmio_host_relax(void)
{
	test_relax_count++;
	if (UINT64_MAX - test_tsc < test_relax_step)
		test_tsc = UINT64_MAX;
	else
		test_tsc += test_relax_step;
}

void
sched_yield(void)
{
	test_yield_count++;
	if (UINT64_MAX - test_tsc < test_yield_step)
		test_tsc = UINT64_MAX;
	else
		test_tsc += test_yield_step;
}

void
hal_io_mb(void)
{
}

void
hal_io_rmb(void)
{
}

void
hal_io_wmb(void)
{
}

static void
test_conversion(void)
{
	uint64_t value;

	assert(intel_ax211_pci_mmio_host_ticks_for_us(2400000000ULL, 1U,
	    &value) == 0);
	assert(value == 2400U);
	assert(intel_ax211_pci_mmio_host_ticks_for_us(3333334ULL, 1U,
	    &value) == 0);
	assert(value == 4U);
	assert(intel_ax211_pci_mmio_host_ticks_for_us(1U, UINT64_MAX,
	    &value) == 0);
	assert(value == 18446744073710ULL);
	assert(intel_ax211_pci_mmio_host_ticks_to_us(2400000000ULL, 2400U,
	    &value) == 0);
	assert(value == 1U);
	assert(intel_ax211_pci_mmio_host_ticks_to_us(UINT64_MAX,
	    UINT64_MAX - 1U, &value) == 0);
	assert(value == 999999U);
	assert(intel_ax211_pci_mmio_host_ticks_for_us(0U, 1U, &value) ==
	    EINVAL);
	assert(intel_ax211_pci_mmio_host_ticks_to_us(0U, 1U, &value) ==
	    EINVAL);
	assert(intel_ax211_pci_mmio_host_ticks_for_us(UINT64_MAX, UINT64_MAX,
	    &value) == EOVERFLOW);
	assert(intel_ax211_pci_mmio_host_ticks_to_us(1U, UINT64_MAX, &value) ==
	    EOVERFLOW);
}

static void
test_calibration_rejection(void)
{
	uint32_t registers[0x4000U / sizeof(uint32_t)];
	struct intel_ax211_pci_mmio_backend backend;

	memset(registers, 0, sizeof(registers));
	memset(&backend, 0xa5, sizeof(backend));
	test_counter_available = false;
	assert(intel_ax211_pci_mmio_backend_init(&backend, registers,
	    sizeof(registers)) == ENOTSUP);
	assert(backend.registers == NULL);
	assert(backend.counter_ready == 0U);
	test_counter_available = true;
	test_counter_frequency = 999999U;
	assert(intel_ax211_pci_mmio_backend_init(&backend, registers,
	    sizeof(registers)) == ENOTSUP);
	test_counter_frequency = 10000000001ULL;
	assert(intel_ax211_pci_mmio_backend_init(&backend, registers,
	    sizeof(registers)) == ENOTSUP);
	test_counter_frequency = 3333334ULL;
	test_tsc = 9000U;
	assert(intel_ax211_pci_mmio_backend_init(&backend, registers,
	    sizeof(registers)) == 0);
	assert(backend.counter_frequency_hz == 3333334ULL);
}

static void
test_backend_io_and_clock(void)
{
	uint32_t registers[0x4000U / sizeof(uint32_t)];
	struct intel_ax211_pci_mmio_backend backend;
	const struct intel_ax211_mmio_ops *ops;
	uint32_t value;
	uint64_t now;

	memset(registers, 0, sizeof(registers));
	memset(&backend, 0xa5, sizeof(backend));
	test_counter_available = true;
	test_counter_frequency = 2400000000ULL;
	test_tsc = 100000U;
	assert(intel_ax211_pci_mmio_backend_init(NULL, registers,
	    sizeof(registers)) != 0);
	assert(intel_ax211_pci_mmio_backend_init(&backend, NULL,
	    sizeof(registers)) != 0);
	assert(intel_ax211_pci_mmio_backend_init(&backend, registers,
	    sizeof(registers) - 1U) != 0);
	assert(intel_ax211_pci_mmio_backend_init(&backend, registers,
	    sizeof(registers)) == 0);
	assert(backend.counter_ready == 1U);
	assert(backend.counter_frequency_hz == 2400000000ULL);
	assert(backend.counter_origin == 100000U);
	ops = intel_ax211_pci_mmio_ops();
	assert(ops != NULL);

	assert(ops->csr_write32(&backend, 0x20U, 0x11223344U) == 0);
	assert(registers[0x20U / 4U] == 0x11223344U);
	assert(ops->csr_read32(&backend, 0x20U, &value) == 0);
	assert(value == 0x11223344U);
	registers[0x20U / 4U] = UINT32_MAX;
	assert(ops->csr_read32(&backend, 0x20U, &value) == EIO);
	assert(ops->csr_read32(&backend, 3U, &value) != 0);
	assert(ops->csr_write32(&backend, 0x4000U, 0U) != 0);

	registers[0x450U / 4U] = 0x55667788U;
	assert(ops->prph_read32(&backend, 0x1234U, &value) == 0);
	assert(registers[0x448U / 4U] == 0x03001234U);
	assert(value == 0x55667788U);
	assert(ops->prph_write32(&backend, 0xabcdefU, 0x89abcdefU) == 0);
	assert(registers[0x444U / 4U] == 0x03abcdefU);
	assert(registers[0x44cU / 4U] == 0x89abcdefU);
	assert(ops->prph_read32(&backend, 0x01000000U, &value) != 0);

	assert(ops->clock_us(&backend, &now) == 0);
	assert(now == 0U);
	test_tsc += 2399U;
	assert(ops->clock_us(&backend, &now) == 0);
	assert(now == 0U);
	test_tsc++;
	assert(ops->clock_us(&backend, &now) == 0);
	assert(now == 1U);

	test_relax_step = 600U;
	test_yield_step = 1440000U;
	test_relax_count = 0U;
	test_yield_count = 0U;
	assert(ops->delay_us(&backend, 10U) == 0);
	assert(test_relax_count == 40U);
	assert(test_yield_count == 0U);
	assert(ops->delay_us(&backend, 1000U) == 0);
	assert(test_relax_count == 1640U);
	assert(test_yield_count == 1U);
	assert(ops->clock_us(&backend, &now) == 0);
	assert(now == 1011U);

	test_tsc--;
	assert(ops->clock_us(&backend, &now) == EIO);
	assert(ops->delay_us(&backend, 1U) == EIO);
}

static void
test_delay_overflow(void)
{
	uint32_t registers[0x4000U / sizeof(uint32_t)];
	struct intel_ax211_pci_mmio_backend backend;
	const struct intel_ax211_mmio_ops *ops;

	memset(registers, 0, sizeof(registers));
	test_counter_available = true;
	test_counter_frequency = 2400000000ULL;
	test_tsc = UINT64_MAX - 1000U;
	assert(intel_ax211_pci_mmio_backend_init(&backend, registers,
	    sizeof(registers)) == 0);
	ops = intel_ax211_pci_mmio_ops();
	assert(ops->delay_us(&backend, 1U) == EOVERFLOW);
}

int
main(void)
{
	test_conversion();
	test_calibration_rejection();
	test_backend_io_and_clock();
	test_delay_overflow();
	puts("intel ax211 PCI MMIO backend: PASS");
	return 0;
}
