/* Intel AX211 PCI BAR backend fixture. SPDX-License-Identifier: Zlib */

#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../../../src/drivers/intel-ax211-pci-mmio.h"

enum test_cpuid_mode {
	TEST_CPUID_VALID,
	TEST_CPUID_NON_INTEL,
	TEST_CPUID_NO_TSC,
	TEST_CPUID_NO_EXTENDED_POWER,
	TEST_CPUID_NO_INVARIANT,
	TEST_CPUID_NO_RATIO,
	TEST_CPUID_NO_CRYSTAL,
	TEST_CPUID_FRACTIONAL,
	TEST_CPUID_TOO_HIGH
};

static enum test_cpuid_mode cpuid_mode;
static uint64_t test_tsc;
static uint64_t test_relax_step;
static uint64_t test_yield_step;
static unsigned int test_relax_count;
static unsigned int test_yield_count;
static unsigned int test_leaf16_calls;

void
intel_ax211_pci_mmio_host_cpuid(
	uint32_t leaf,
	uint32_t subleaf,
	uint32_t *eax,
	uint32_t *ebx,
	uint32_t *ecx,
	uint32_t *edx)
{
	(void)subleaf;
	*eax = 0U;
	*ebx = 0U;
	*ecx = 0U;
	*edx = 0U;
	if (leaf == 0U) {
		*eax = 0x16U;
		*ebx = cpuid_mode == TEST_CPUID_NON_INTEL ? 0U : 0x756e6547U;
		*edx = 0x49656e69U;
		*ecx = 0x6c65746eU;
	} else if (leaf == 1U) {
		if (cpuid_mode != TEST_CPUID_NO_TSC)
			*edx = 1U << 4;
	} else if (leaf == 0x80000000U) {
		*eax = cpuid_mode == TEST_CPUID_NO_EXTENDED_POWER ?
		    0x80000006U : 0x80000007U;
	} else if (leaf == 0x80000007U) {
		if (cpuid_mode != TEST_CPUID_NO_INVARIANT)
			*edx = 1U << 8;
	} else if (leaf == 0x15U) {
		*eax = cpuid_mode == TEST_CPUID_NO_RATIO ? 0U : 1U;
		*ebx = 96U;
		*ecx = cpuid_mode == TEST_CPUID_NO_CRYSTAL ? 0U : 25000000U;
		if (cpuid_mode == TEST_CPUID_FRACTIONAL) {
			*eax = 3U;
			*ebx = 1U;
			*ecx = 10000000U;
		} else if (cpuid_mode == TEST_CPUID_TOO_HIGH) {
			*eax = 1U;
			*ebx = 500U;
			*ecx = 25000000U;
		}
	} else if (leaf == 0x16U) {
		test_leaf16_calls++;
		*eax = 2400U;
	}
}

uint64_t
intel_ax211_pci_mmio_host_rdtsc(void)
{
	return test_tsc;
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
	static const enum test_cpuid_mode rejected[] = {
		TEST_CPUID_NON_INTEL,
		TEST_CPUID_NO_TSC,
		TEST_CPUID_NO_EXTENDED_POWER,
		TEST_CPUID_NO_INVARIANT,
		TEST_CPUID_NO_RATIO,
		TEST_CPUID_NO_CRYSTAL,
		TEST_CPUID_TOO_HIGH
	};
	size_t index;

	memset(registers, 0, sizeof(registers));
	for (index = 0U; index < sizeof(rejected) / sizeof(rejected[0]);
	    index++) {
		memset(&backend, 0xa5, sizeof(backend));
		cpuid_mode = rejected[index];
		test_tsc = 1000U;
		test_leaf16_calls = 0U;
		assert(intel_ax211_pci_mmio_backend_init(&backend, registers,
		    sizeof(registers)) == ENOTSUP);
		assert(backend.registers == NULL);
		assert(backend.tsc_calibrated == 0U);
		assert(test_leaf16_calls == 0U);
	}

	cpuid_mode = TEST_CPUID_FRACTIONAL;
	test_tsc = 9000U;
	assert(intel_ax211_pci_mmio_backend_init(&backend, registers,
	    sizeof(registers)) == 0);
	assert(backend.tsc_frequency_hz == 3333334ULL);
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
	cpuid_mode = TEST_CPUID_VALID;
	test_tsc = 100000U;
	assert(intel_ax211_pci_mmio_backend_init(NULL, registers,
	    sizeof(registers)) != 0);
	assert(intel_ax211_pci_mmio_backend_init(&backend, NULL,
	    sizeof(registers)) != 0);
	assert(intel_ax211_pci_mmio_backend_init(&backend, registers,
	    sizeof(registers) - 1U) != 0);
	assert(intel_ax211_pci_mmio_backend_init(&backend, registers,
	    sizeof(registers)) == 0);
	assert(backend.tsc_calibrated == 1U);
	assert(backend.tsc_frequency_hz == 2400000000ULL);
	assert(backend.tsc_origin == 100000U);
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
	cpuid_mode = TEST_CPUID_VALID;
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
