/* X68000 MFP-vectored interrupt dispatch and UP interrupt services. */
/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */

#include <hal/hal.h>
#include "mmio.h"

#define X68K_IRQ_COUNT 256
#define MFP_IMRA 9U
#define MFP_IMRB 10U

struct x68k_irq_slot {
	void (*handler)(void *);
	void *argument;
};

static struct x68k_irq_slot slots[X68K_IRQ_COUNT];
static unsigned isr_depth;

static void
mfp_mask(unsigned vector, int enabled)
{
	unsigned source = vector & 15U;
	unsigned reg = source >= 8U ? MFP_IMRA : MFP_IMRB;
	uint8_t bit = (uint8_t)(1U << (source & 7U));
	uint8_t mask = x68k_mfp_read(reg);
	x68k_mfp_write(reg, enabled ? (uint8_t)(mask | bit) :
	    (uint8_t)(mask & (uint8_t)~bit));
}

irqlock_t irq_acquire_lock(void) { return hal_irq_disable() ? 1 : 0; }
void irq_unacquire_lock(irqlock_t lock) { if (lock != 0) hal_irq_enable(); }

void
hal_irq_mask(int irq)
{
	if (irq < 0 || irq >= X68K_IRQ_COUNT)
		HAL_FATAL("invalid X68k IRQ mask");
	if (irq >= 0x40 && irq <= 0x4f)
		mfp_mask((unsigned)irq, 0);
}

void
hal_irq_unmask(int irq)
{
	if (irq < 0 || irq >= X68K_IRQ_COUNT)
		HAL_FATAL("invalid X68k IRQ unmask");
	if (irq >= 0x40 && irq <= 0x4f)
		mfp_mask((unsigned)irq, 1);
}

void hal_irq_send_eoi(int irq) { (void)irq; }

void
hal_irq_set_handler(int irq, void (*handler)(void *), void *argument)
{
	if (irq < 0 || irq >= X68K_IRQ_COUNT)
		HAL_FATAL("invalid X68k IRQ handler");
	slots[irq].handler = handler;
	slots[irq].argument = argument;
}

int
x68k_irq_dispatch(unsigned vector)
{
	if (vector >= X68K_IRQ_COUNT || slots[vector].handler == NULL)
		return 0;
	slots[vector].handler(slots[vector].argument);
	return 1;
}

void irq_enter_isr(int irq) { (void)irq; isr_depth++; }
void irq_leave_isr(int irq) { (void)irq; if (isr_depth != 0) isr_depth--; }
int hal_get_current_cpu(void) { return 0; }
int hal_get_cpu_count(void) { return 1; }
void hal_send_ipi_one(int cpu, int ipi) { (void)cpu; (void)ipi; }
void hal_send_ipi_mask(uint8 *mask, int ipi) { (void)mask; (void)ipi; }
void hal_send_ipi_others(int ipi) { (void)ipi; }
