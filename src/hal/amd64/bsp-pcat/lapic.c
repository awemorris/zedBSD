/* xAPIC local controller, periodic timer, and CPU notifications. */
/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */

#include <hal/hal.h>
#include "lapic.h"
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

static volatile uint32_t *lapic;
static uint32_t timer_initial;

static uint32_t read_reg(unsigned offset)
{ return lapic[offset / 4U]; }
static void write_reg(unsigned offset, uint32_t value)
{ lapic[offset / 4U] = value; hal_io_mb(); }

static int
wait_icr(void)
{
	unsigned count;
	for (count = 0; count < 1000000U; count++)
		if ((read_reg(LAPIC_ICR_LOW) & ICR_PENDING) == 0)
			return HAL_OK;
	return HAL_ERR_TIMEOUT;
}

int
amd64_lapic_init(uint32_t physical_address)
{
	struct hal_pmem_request request = {
		physical_address, 4096, 4096, HAL_PMEM_TYPE_MMIO,
		HAL_PMEM_ATTR_NOCACHE
	};
	struct hal_pmem memory;
	uint64_t apic_base;

	if (hal_pmem_alloc(&request, &memory) != HAL_OK)
		return HAL_ERR_UNSUPPORTED;
	lapic = memory.vaddr;
	apic_base = asm_read_msr(0x1bU);
	asm_write_msr(0x1bU, (apic_base & ~0xfffULL) | (1U << 11));
	amd64_lapic_init_cpu();
	return HAL_OK;
}

void
amd64_lapic_init_cpu(void)
{
	if (lapic == NULL)
		HAL_FATAL("Local APIC not mapped");
	write_reg(LAPIC_SVR, LAPIC_ENABLE | AMD64_VECTOR_SPURIOUS);
	write_reg(LAPIC_LVT_ERROR, AMD64_VECTOR_ERROR);
	write_reg(LAPIC_ESR, 0);
	write_reg(LAPIC_ESR, 0);
}

uint32_t amd64_lapic_id(void) { return read_reg(LAPIC_ID) >> 24; }
void amd64_lapic_eoi(void) { write_reg(LAPIC_EOI, 0); }

static void
pit_wait_10ms(void)
{
	uint8_t value = asm_inb(0x61U);
	asm_outb(0x61U, (uint8_t)((value & ~2U) | 1U));
	asm_outb(0x43U, 0xb0U);
	asm_outb(0x42U, (uint8_t)11932U);
	asm_outb(0x42U, (uint8_t)(11932U >> 8));
	while ((asm_inb(0x61U) & 0x20U) == 0)
		__asm__ volatile("pause");
}

void
amd64_lapic_timer_start(void)
{
	if (timer_initial == 0) {
		uint32_t elapsed;
		write_reg(LAPIC_TIMER_DIVIDE, 0x3U); /* divide by 16 */
		write_reg(LAPIC_LVT_TIMER, LAPIC_MASKED | INT_IRQ_BASE);
		write_reg(LAPIC_TIMER_INITIAL, 0xffffffffU);
		pit_wait_10ms();
		elapsed = 0xffffffffU - read_reg(LAPIC_TIMER_CURRENT);
		write_reg(LAPIC_TIMER_INITIAL, 0);
		if (elapsed < 100U)
			HAL_FATAL("Local APIC timer calibration failed");
		timer_initial = elapsed;
	}
	write_reg(LAPIC_TIMER_DIVIDE, 0x3U);
	write_reg(LAPIC_LVT_TIMER, LAPIC_PERIODIC | INT_IRQ_BASE);
	write_reg(LAPIC_TIMER_INITIAL, timer_initial);
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
