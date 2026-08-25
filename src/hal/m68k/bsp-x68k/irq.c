/* X68000 MFP-vectored interrupt dispatch and UP interrupt services. */
/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */

#include <hal/hal.h>
#include "mmio.h"

#define X68K_IRQ_COUNT 256
#define MFP_IMRA 9U
#define MFP_IMRB 10U

struct x68k_irq_slot {
	hal_irq_handler_t handler;
	void *argument;
	unsigned in_flight;
};

static struct x68k_irq_slot slots[X68K_IRQ_COUNT];

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

void
hal_irq_send_eoi(hal_irq_ack_t acknowledge)
{
	unsigned vector;
	if (acknowledge == HAL_IRQ_ACK_NONE ||
	    acknowledge > X68K_IRQ_COUNT)
		HAL_FATAL("invalid X68k IRQ acknowledgement");
	vector = (unsigned)acknowledge - 1U;
	if (!slots[vector].in_flight)
		HAL_FATAL("stale X68k IRQ acknowledgement");
	slots[vector].in_flight = 0;
}

int
hal_irq_set_handler(int irq, hal_irq_handler_t handler, void *argument)
{
	bool enabled;
	if (irq < 0 || irq >= X68K_IRQ_COUNT ||
	    (handler == NULL && argument != NULL))
		return HAL_ERR_INVALID;
	enabled = hal_irq_disable();
	if (slots[irq].in_flight) {
		if (enabled)
			hal_irq_enable();
		return HAL_ERR_BUSY;
	}
	slots[irq].handler = handler;
	slots[irq].argument = argument;
	if (enabled)
		hal_irq_enable();
	return HAL_OK;
}

int
x68k_irq_dispatch(unsigned vector)
{
	if (vector >= X68K_IRQ_COUNT || slots[vector].handler == NULL)
		return 0;
	if (slots[vector].in_flight)
		HAL_FATAL("nested X68k IRQ acknowledgement");
	slots[vector].in_flight = 1;
	slots[vector].handler((int)vector, (hal_irq_ack_t)vector + 1U,
	    slots[vector].argument);
	return 1;
}

int
hal_irq_service_wait(int irq, hal_irq_ack_t *acknowledge)
{
	(void)irq;
	(void)acknowledge;
	return HAL_ERR_UNSUPPORTED;
}

int
hal_irq_set_affinity(int irq, const struct hal_cpu_mask *requested)
{
	if (irq < 0 || irq >= X68K_IRQ_COUNT || requested == NULL ||
	    !hal_cpu_mask_test(requested, 0))
		return HAL_ERR_INVALID;
	return HAL_OK;
}

int
hal_irq_get_affinity(int irq, struct hal_irq_affinity *result)
{
	if (irq < 0 || irq >= X68K_IRQ_COUNT || result == NULL)
		return HAL_ERR_INVALID;
	hal_cpu_mask_zero(&result->requested);
	hal_cpu_mask_set(&result->requested, 0);
	result->effective = result->requested;
	return HAL_OK;
}

int
hal_irq_register_msi(const char *source, hal_irq_handler_t handler,
	void *handler_arg, int *mapped_irq, paddr_t *mapped_addr,
	uint32_t *mapped_event)
{
	(void)source;
	(void)handler;
	(void)handler_arg;
	(void)mapped_irq;
	(void)mapped_addr;
	(void)mapped_event;
	return HAL_ERR_UNSUPPORTED;
}

int
hal_irq_unregister_msi(int mapped_irq)
{
	(void)mapped_irq;
	return HAL_ERR_UNSUPPORTED;
}
