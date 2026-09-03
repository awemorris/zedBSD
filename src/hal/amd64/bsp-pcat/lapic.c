/* xAPIC local controller, periodic timer, and CPU notifications. */
/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */

#include <hal/hal.h>
#include "lapic.h"
#include "acpi.h"
#include "early-init-policy.h"
#include "timecounter.h"
#include "../asm.h"
#include "../defs.h"

#define LAPIC_ID       0x020U
#define LAPIC_EOI      0x0b0U
#define LAPIC_SVR      0x0f0U
#define LAPIC_ESR      0x280U
#define LAPIC_ICR_LOW  0x300U
#define LAPIC_ICR_HIGH 0x310U
#define LAPIC_LVT_TIMER 0x320U
#define LAPIC_LVT_ERROR 0x370U
#define LAPIC_TIMER_INITIAL 0x380U
#define LAPIC_TIMER_CURRENT 0x390U
#define LAPIC_TIMER_DIVIDE  0x3e0U

#define LAPIC_ENABLE   0x100U
#define LAPIC_MASKED   0x10000U
#define LAPIC_PERIODIC 0x20000U
#define ICR_PENDING    0x1000U
#define PIT_POLL_LIMIT 1000000U
#define PIT_INPUT_HZ   1193182ULL
#define PIT_CAL_TICKS  11932U

#define CPUID_ARCH_CAPABILITIES (1U << 29)
#define ARCH_CAP_XAPIC_DISABLE_STATUS (1ULL << 21)
#define MSR_IA32_APIC_BASE 0x01bU
#define MSR_IA32_ARCH_CAPABILITIES 0x10aU
#define MSR_IA32_XAPIC_DISABLE_STATUS 0x0bdU

static volatile uint32_t *lapic;
static uint32_t timer_initial;
static uint32_t lapic_physical_address;

static uint32_t read_reg(unsigned offset)
{ return lapic[offset / 4U]; }
static void write_reg(unsigned offset, uint32_t value)
{ lapic[offset / 4U] = value; hal_io_mb(); }

static void
cpuid_count(uint32_t leaf, uint32_t subleaf, uint32_t *eax, uint32_t *ebx,
	uint32_t *ecx, uint32_t *edx)
{
	uint32_t a = leaf, b, c = subleaf, d;

	__asm__ volatile("cpuid" : "+a"(a), "=b"(b), "+c"(c), "=d"(d));
	*eax = a;
	*ebx = b;
	*ecx = c;
	*edx = d;
}

static enum amd64_apic_policy_result
preflight_cpu(uint32_t expected_apic_id, uint32_t physical_address,
	uint32_t *actual_apic_id)
{
	struct amd64_apic_policy_input input;
	uint32_t max_leaf, eax, ebx, ecx, edx;
	enum amd64_apic_policy_result result;

	hal_memset(&input, 0, sizeof(input));
	cpuid_count(0, 0, &max_leaf, &ebx, &ecx, &edx);
	if (max_leaf >= 1U) {
		cpuid_count(1, 0, &eax, &input.cpuid1_ebx,
		    &input.cpuid1_ecx, &input.cpuid1_edx);
		(void)eax;
		if ((input.cpuid1_edx & (1U << 9)) != 0)
			input.apic_base = asm_read_msr(MSR_IA32_APIC_BASE);
	}
	if ((input.cpuid1_edx & (1U << 9)) != 0 && max_leaf >= 7U) {
		cpuid_count(7, 0, &eax, &ebx, &ecx, &edx);
		if ((edx & CPUID_ARCH_CAPABILITIES) != 0) {
			uint64_t capabilities =
			    asm_read_msr(MSR_IA32_ARCH_CAPABILITIES);
			if ((capabilities & ARCH_CAP_XAPIC_DISABLE_STATUS) != 0) {
				input.xapic_disable_status =
				    asm_read_msr(MSR_IA32_XAPIC_DISABLE_STATUS);
				input.lock_status_valid = 1;
			}
		}
	}
	input.madt_base = physical_address;
	input.expected_apic_id = expected_apic_id;
	result = amd64_apic_policy_evaluate(&input, actual_apic_id);
	hal_printf(result == AMD64_APIC_POLICY_OK ?
	    "A64 APIC PREFLIGHT PASS id=%u base=%08X:%08X madt=%08X "
	    "lock=%u result=%s\n" :
	    "A64 APIC PREFLIGHT FAIL id=%u base=%08X:%08X madt=%08X "
	    "lock=%u result=%s\n", *actual_apic_id,
	    (uint32_t)(input.apic_base >> 32), (uint32_t)input.apic_base,
	    physical_address, input.lock_status_valid ?
	    (unsigned)(input.xapic_disable_status & 1U) : 0U,
	    amd64_apic_policy_result_name(result));
	return result;
}

static int
acpi_has_apic_id(const struct amd64_acpi_info *acpi, uint32_t apic_id)
{
	unsigned index;

	for (index = 0; index < acpi->cpu_count; index++)
		if (acpi->cpus[index].apic_id == apic_id)
			return 1;
	return 0;
}

static int
wait_icr(void)
{
	unsigned count;
	for (count = 0; count < 1000000U; count++)
		if ((read_reg(LAPIC_ICR_LOW) & ICR_PENDING) == 0)
			return HAL_OK;
	return HAL_ERR_TIMEOUT;
}

static void
init_registers(void)
{
	write_reg(LAPIC_SVR, LAPIC_ENABLE | AMD64_VECTOR_SPURIOUS);
	write_reg(LAPIC_LVT_ERROR, AMD64_VECTOR_ERROR);
	write_reg(LAPIC_ESR, 0);
	write_reg(LAPIC_ESR, 0);
}

int
amd64_lapic_init(const struct amd64_acpi_info *acpi)
{
	struct hal_pmem_request request;
	struct hal_pmem memory;
	uint32_t apic_id;
	enum amd64_apic_policy_result policy;

	if (acpi == NULL)
		return HAL_ERR_INVALID;
	policy = preflight_cpu(AMD64_APIC_EXPECT_ANY, acpi->lapic_address,
	    &apic_id);
	if (policy != AMD64_APIC_POLICY_OK)
		return HAL_ERR_UNSUPPORTED;
	if (!acpi_has_apic_id(acpi, apic_id)) {
		hal_printf("A64 APIC PREFLIGHT FAIL id=%u result=madt-id-missing\n",
		    apic_id);
		return HAL_ERR_UNSUPPORTED;
	}
	request.paddr = acpi->lapic_address;
	request.size = 4096;
	request.alignment = 4096;
	request.type = HAL_PMEM_TYPE_MMIO;
	request.attr = HAL_PMEM_ATTR_NOCACHE;
	if (hal_pmem_alloc(&request, &memory) != HAL_OK) {
		hal_puts("A64 LAPIC MAP FAIL\n");
		return HAL_ERR_UNSUPPORTED;
	}
	lapic = memory.vaddr;
	lapic_physical_address = acpi->lapic_address;
	if (amd64_lapic_id() != apic_id) {
		hal_printf("A64 LAPIC ID FAIL cpuid=%u mmio=%u\n", apic_id,
		    amd64_lapic_id());
		return HAL_ERR_UNSUPPORTED;
	}
	init_registers();
	hal_printf("A64 LAPIC READY id=%u base=%08X\n", apic_id,
	    lapic_physical_address);
	return HAL_OK;
}

int
amd64_lapic_init_secondary(uint32_t expected_apic_id,
	unsigned *failure_reason)
{
	uint32_t actual_apic_id;
	enum amd64_apic_policy_result policy;

	if (failure_reason == NULL)
		return HAL_ERR_INVALID;
	*failure_reason = AMD64_APIC_POLICY_OK;
	if (lapic == NULL) {
		*failure_reason = AMD64_APIC_POLICY_INVALID_MODE;
		return HAL_ERR_STATE;
	}
	policy = preflight_cpu(expected_apic_id, lapic_physical_address,
	    &actual_apic_id);
	if (policy != AMD64_APIC_POLICY_OK) {
		*failure_reason = (unsigned)policy;
		return HAL_ERR_UNSUPPORTED;
	}
	if (amd64_lapic_id() != actual_apic_id) {
		hal_printf("A64 LAPIC AP ID FAIL expected=%u mmio=%u\n",
		    actual_apic_id, amd64_lapic_id());
		*failure_reason = AMD64_APIC_POLICY_ID_MISMATCH;
		return HAL_ERR_UNSUPPORTED;
	}
	init_registers();
	return HAL_OK;
}

uint32_t amd64_lapic_id(void) { return read_reg(LAPIC_ID) >> 24; }
void amd64_lapic_eoi(void) { write_reg(LAPIC_EOI, 0); }

static int
pit_wait_level(struct amd64_pit_poll *poll, int expected_high,
	uint8_t *last_port61)
{
	enum amd64_pit_poll_result result;
	uint8_t current;

	for (;;) {
		current = asm_inb(0x61U);
		result = amd64_pit_poll_step(poll, current, expected_high);
		if (result != AMD64_PIT_POLL_WAIT)
			break;
		__asm__ volatile("pause");
	}
	if (last_port61 != NULL)
		*last_port61 = current;
	return result == AMD64_PIT_POLL_READY ? HAL_OK : HAL_ERR_TIMEOUT;
}

static int
pit_wait_10ms(uint8_t *last_port61, unsigned *polls, const char **stage,
	int measure_tsc, uint64_t *tsc_start, uint64_t *tsc_end)
{
	uint8_t value = asm_inb(0x61U);
	struct amd64_pit_poll poll;
	int error;

	/* Mode 0 drives OUT low when the count is loaded.  Observe that edge
	 * before raising GATE, otherwise a stuck-high port can look successful. */
	asm_outb(0x61U, (uint8_t)(value & ~3U));
	asm_outb(0x43U, 0xb0U);
	asm_outb(0x42U, (uint8_t)PIT_CAL_TICKS);
	asm_outb(0x42U, (uint8_t)(PIT_CAL_TICKS >> 8));
	amd64_pit_poll_init(&poll, PIT_POLL_LIMIT);
	error = pit_wait_level(&poll, 0, last_port61);
	if (error != HAL_OK) {
		if (stage != NULL)
			*stage = "out-low";
		goto out;
	}
	if (measure_tsc)
		*tsc_start = amd64_timecounter_sample_serialized();
	asm_outb(0x61U, (uint8_t)((value & ~2U) | 1U));
	amd64_pit_poll_init(&poll, PIT_POLL_LIMIT);
	error = pit_wait_level(&poll, 1, last_port61);
	if (error == HAL_OK && measure_tsc)
		*tsc_end = amd64_timecounter_sample_serialized();
	if (error != HAL_OK && stage != NULL)
		*stage = "out-high";
out:
	asm_outb(0x61U, value);
	if (polls != NULL)
		*polls = PIT_POLL_LIMIT - poll.remaining;
	return error;
}

int
amd64_lapic_timer_start(void)
{
	if (timer_initial == 0) {
		uint32_t elapsed;
		uint64_t tsc_start = 0U;
		uint64_t tsc_end = 0U;
		uint8_t port61;
		unsigned polls;
		const char *stage = "complete";
		enum amd64_tsc_frequency_policy_result tsc_policy;
		int measure_tsc;

		hal_puts("A64 TIMER CAL BEGIN\n");
		tsc_policy = amd64_timecounter_bsp_prepare();
		measure_tsc = tsc_policy == AMD64_TSC_FREQUENCY_NEEDS_PIT;
		write_reg(LAPIC_TIMER_DIVIDE, 0x3U); /* divide by 16 */
		write_reg(LAPIC_LVT_TIMER, LAPIC_MASKED | INT_IRQ_BASE);
		write_reg(LAPIC_TIMER_INITIAL, 0xffffffffU);
		if (pit_wait_10ms(&port61, &polls, &stage, measure_tsc,
		    &tsc_start, &tsc_end) != HAL_OK) {
			uint32_t current = read_reg(LAPIC_TIMER_CURRENT);
			write_reg(LAPIC_LVT_TIMER,
			    LAPIC_MASKED | INT_IRQ_BASE);
			write_reg(LAPIC_TIMER_INITIAL, 0);
			amd64_timecounter_bsp_abort();
			hal_printf("A64 TIMER CAL TIMEOUT stage=%s port61=%02X "
			    "lapic-current=%08X polls=%u\n", stage, port61,
			    current, polls);
			return HAL_ERR_TIMEOUT;
		}
		elapsed = 0xffffffffU - read_reg(LAPIC_TIMER_CURRENT);
		write_reg(LAPIC_TIMER_INITIAL, 0);
		if (elapsed < 100U) {
			write_reg(LAPIC_LVT_TIMER,
			    LAPIC_MASKED | INT_IRQ_BASE);
			amd64_timecounter_bsp_abort();
			hal_printf("A64 TIMER CAL INVALID elapsed=%u\n", elapsed);
			return HAL_ERR_STATE;
		}
		timer_initial = elapsed;
		if (measure_tsc)
			amd64_timecounter_pit_complete(tsc_start, tsc_end,
			    PIT_INPUT_HZ, PIT_CAL_TICKS);
		hal_printf("A64 TIMER CAL READY ticks=%u\n", timer_initial);
	}
	write_reg(LAPIC_TIMER_DIVIDE, 0x3U);
	write_reg(LAPIC_LVT_TIMER, LAPIC_PERIODIC | INT_IRQ_BASE);
	write_reg(LAPIC_TIMER_INITIAL, timer_initial);
	return HAL_OK;
}

void
amd64_lapic_timer_stop(void)
{
	write_reg(LAPIC_LVT_TIMER, LAPIC_MASKED | INT_IRQ_BASE);
	write_reg(LAPIC_TIMER_INITIAL, 0);
}

static int
send_icr(uint32_t apic_id, uint32_t low)
{
	int error = wait_icr();
	if (error != HAL_OK)
		return error;
	write_reg(LAPIC_ICR_HIGH, apic_id << 24);
	write_reg(LAPIC_ICR_LOW, low);
	return wait_icr();
}

int
amd64_lapic_send_init(uint32_t apic_id)
{
	volatile unsigned delay;
	int error = send_icr(apic_id, 0x0000c500U); /* INIT level assert */
	if (error != HAL_OK)
		return error;
	for (delay = 0; delay < 100000U; delay++)
		__asm__ volatile("pause");
	return send_icr(apic_id, 0x00008500U); /* INIT level deassert */
}
int amd64_lapic_send_startup(uint32_t apic_id, uint8_t vector)
{ return send_icr(apic_id, 0x00004600U | vector); }
int amd64_lapic_notify(uint32_t apic_id)
{ return send_icr(apic_id, AMD64_VECTOR_NOTIFY); }
int amd64_lapic_send_vector(uint32_t apic_id, uint8_t vector)
{
	if (vector < 0x20U)
		return HAL_ERR_INVALID;
	return send_icr(apic_id, vector);
}

_Noreturn void
amd64_lapic_panic_all(void)
{
	(void)wait_icr();
	write_reg(LAPIC_ICR_HIGH, 0);
	write_reg(LAPIC_ICR_LOW, 0x000c0400U); /* NMI, all excluding self */
	(void)hal_irq_disable();
	for (;;)
		asm_hlt();
}
