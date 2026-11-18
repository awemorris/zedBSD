/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The X68000's interrupts on its one processor.
 *
 * A numbered IRQ is an exception vector.  The MFP's sixteen sources are
 * vectors 0x40 to 0x4f, and their masks are the MFP's interrupt mask
 * registers; the other vectors have no mask the HAL keeps.
 */

#include <hal/hal.h>
#include "../defs.h"
#include "mmio.h"

/* The vectors an IRQ number names. */
#define X68K_IRQ_COUNT		256

/* The MFP's vectors, and its two interrupt mask registers. */
#define X68K_MFP_VECTOR_FIRST	0x40
#define X68K_MFP_VECTOR_LAST	0x4f
#define MFP_IMRA		9U
#define MFP_IMRB		10U

/*
 * One vector's handler, its argument, and whether an interrupt of it is
 * being handled.
 */
struct x68k_irq_slot {
	hal_irq_handler_t handler;
	void *argument;
	unsigned in_flight;
};

/*
 * The handler of every vector.  Changed with interrupts off; in_flight is
 * set by x68k_irq_dispatch() and cleared by hal_irq_send_eoi().
 */
static struct x68k_irq_slot slots[X68K_IRQ_COUNT];

static void mfp_mask(unsigned vector, int enabled);

/*
 * Masks a numbered IRQ.
 */
void
hal_irq_mask(
	int irq_num)
{
	/* Refuses an IRQ that does not exist. */
	if (irq_num < 0 || irq_num >= X68K_IRQ_COUNT)
		HAL_FATAL("invalid X68k IRQ mask");

	/* Only the MFP's sources have a mask. */
	if (irq_num >= X68K_MFP_VECTOR_FIRST && irq_num <= X68K_MFP_VECTOR_LAST)
		mfp_mask((unsigned)irq_num, 0);
}

/*
 * Unmasks a numbered IRQ.
 */
void
hal_irq_unmask(
	int irq_num)
{
	/* Refuses an IRQ that does not exist. */
	if (irq_num < 0 || irq_num >= X68K_IRQ_COUNT)
		HAL_FATAL("invalid X68k IRQ unmask");

	/* Only the MFP's sources have a mask. */
	if (irq_num >= X68K_MFP_VECTOR_FIRST && irq_num <= X68K_MFP_VECTOR_LAST)
		mfp_mask((unsigned)irq_num, 1);
}

/*
 * Ends the handling of the interrupt an acknowledgement names.
 */
void
hal_irq_send_eoi(
	hal_irq_ack_t acknowledge)
{
	unsigned vector;

	/* Refuses an acknowledgement that names no vector. */
	if (acknowledge == HAL_IRQ_ACK_NONE || acknowledge > X68K_IRQ_COUNT)
		HAL_FATAL("invalid X68k IRQ acknowledgement");

	/* Refuses one of a vector not being handled. */
	vector = (unsigned)acknowledge - 1U;
	if (!slots[vector].in_flight)
		HAL_FATAL("stale X68k IRQ acknowledgement");

	/* The vector may be taken again. */
	slots[vector].in_flight = 0;
}

/*
 * Registers the handler of a numbered IRQ, an exception vector.
 */
int
hal_irq_register(
	int irq_num,
	hal_irq_handler_t func,
	void *arg)
{
	bool enabled;

	/* Refuses an IRQ that does not exist, or no handler. */
	if (irq_num < 0 || irq_num >= X68K_IRQ_COUNT)
		return HAL_ERR_INVALID;
	if (func == NULL)
		return HAL_ERR_INVALID;

	/* Installs the handler unless the vector has one or is being handled. */
	enabled = hal_irq_disable();

	if (slots[irq_num].handler != NULL || slots[irq_num].in_flight) {
		if (enabled)
			hal_irq_enable();
		return HAL_ERR_BUSY;
	}
	slots[irq_num].handler = func;
	slots[irq_num].argument = arg;

	if (enabled)
		hal_irq_enable();

	/* Succeeded: the vector reaches the handler. */
	return HAL_OK;
}

/*
 * Removes the handler of a numbered IRQ.
 */
int
hal_irq_unregister(
	int irq_num,
	hal_irq_handler_t func,
	void *arg)
{
	bool enabled;

	/* Refuses an IRQ that does not exist. */
	if (irq_num < 0 || irq_num >= X68K_IRQ_COUNT)
		return HAL_ERR_INVALID;

	/* Removes the handler when it is the one named and not being handled. */
	enabled = hal_irq_disable();

	if (slots[irq_num].handler != func || slots[irq_num].argument != arg) {
		if (enabled)
			hal_irq_enable();
		return HAL_ERR_INVALID;
	}
	if (slots[irq_num].in_flight) {
		if (enabled)
			hal_irq_enable();
		return HAL_ERR_BUSY;
	}
	slots[irq_num].handler = NULL;
	slots[irq_num].argument = NULL;

	if (enabled)
		hal_irq_enable();

	/* Succeeded: the vector reaches no handler. */
	return HAL_OK;
}

/*
 * Hands an interrupt vector to its handler.
 *
 * Returns 1 when a handler took it, or 0 when the vector has none.
 */
int
x68k_irq_dispatch(
	unsigned vector)
{
	/* A vector without a handler is not the board's. */
	if (vector >= X68K_IRQ_COUNT || slots[vector].handler == NULL)
		return 0;

	/* One interrupt of a vector is handled at a time. */
	if (slots[vector].in_flight)
		HAL_FATAL("nested X68k IRQ acknowledgement");

	/*
	 * The acknowledgement names the vector, offset by one so that vector 0
	 * is not HAL_IRQ_ACK_NONE; the handler passes it back.
	 */
	slots[vector].in_flight = 1;
	slots[vector].handler((int)vector, (hal_irq_ack_t)vector + 1U, slots[vector].argument);

	/* Succeeded: the handler took the interrupt. */
	return 1;
}

/*
 * Accepts an affinity that includes the only processor.
 */
int
hal_irq_set_affinity(
	int irq,
	const struct hal_cpu_mask *requested)
{
	bool includes_boot_cpu;

	/* Refuses an IRQ that does not exist, or no mask. */
	if (irq < 0 || irq >= X68K_IRQ_COUNT || requested == NULL)
		return HAL_ERR_INVALID;

	/* Refuses a mask without the only processor. */
	includes_boot_cpu = hal_cpu_mask_test(requested, 0);
	if (!includes_boot_cpu)
		return HAL_ERR_INVALID;

	/* Succeeded: every IRQ goes to the only processor. */
	return HAL_OK;
}

/*
 * Refuses a change of trigger mode or polarity: the X68000's interrupt
 * levels have none to configure.
 */
int
hal_irq_set_mode(
	int irq,
	int trigger,
	int polarity)
{
	/* Refuses an IRQ, a trigger mode or a polarity that does not exist. */
	if (irq < 0 || irq >= X68K_IRQ_COUNT)
		return HAL_ERR_INVALID;
	if (trigger != HAL_IRQ_TRIGGER_EDGE && trigger != HAL_IRQ_TRIGGER_LEVEL)
		return HAL_ERR_INVALID;
	if (polarity != HAL_IRQ_POLARITY_HIGH && polarity != HAL_IRQ_POLARITY_LOW)
		return HAL_ERR_INVALID;

	/* Failed: the configuration is fixed. */
	return HAL_ERR_UNSUPPORTED;
}

/*
 * Reports the affinity of a numbered IRQ: the only processor, asked for
 * and in effect.
 */
int
hal_irq_get_affinity(
	int irq,
	struct hal_cpu_mask *requested,
	struct hal_cpu_mask *effective)
{
	/* Refuses an IRQ that does not exist. */
	if (irq < 0 || irq >= X68K_IRQ_COUNT)
		return HAL_ERR_INVALID;

	/* Reports the only processor for what the caller asked for. */
	if (requested != NULL) {
		hal_cpu_mask_zero(requested);
		hal_cpu_mask_set(requested, 0);
	}
	if (effective != NULL) {
		hal_cpu_mask_zero(effective);
		hal_cpu_mask_set(effective, 0);
	}

	/* Succeeded. */
	return HAL_OK;
}

/*
 * Refuses a message-signalled IRQ: the X68000 has none.
 */
int
hal_irq_register_msi(
	const char *source,
	hal_irq_handler_t handler,
	void *handler_arg,
	int *mapped_irq,
	paddr_t *mapped_addr,
	uint32_t *mapped_event)
{
	UNUSED_PARAMETER(source);
	UNUSED_PARAMETER(handler);
	UNUSED_PARAMETER(handler_arg);
	UNUSED_PARAMETER(mapped_irq);
	UNUSED_PARAMETER(mapped_addr);
	UNUSED_PARAMETER(mapped_event);

	/* Failed: no message-signalled IRQ exists on this board. */
	return HAL_ERR_UNSUPPORTED;
}

/*
 * Refuses to remove a message-signalled IRQ, which is never made.
 */
int
hal_irq_unregister_msi(
	int mapped_irq)
{
	UNUSED_PARAMETER(mapped_irq);

	/* Failed: no message-signalled IRQ exists on this board. */
	return HAL_ERR_UNSUPPORTED;
}

/*
 * Refuses to allocate a message-signalled IRQ.
 */
int
hal_irq_alloc_msi(
	const char *source,
	int *mapped_irq,
	paddr_t *mapped_addr,
	uint32_t *mapped_event)
{
	UNUSED_PARAMETER(source);
	UNUSED_PARAMETER(mapped_irq);
	UNUSED_PARAMETER(mapped_addr);
	UNUSED_PARAMETER(mapped_event);

	/* Failed: no message-signalled IRQ exists on this board. */
	return HAL_ERR_UNSUPPORTED;
}

/*
 * Refuses to attach a handler to a message-signalled IRQ.
 */
int
hal_irq_attach_msi(
	int mapped_irq,
	hal_irq_handler_t handler,
	void *handler_arg)
{
	UNUSED_PARAMETER(mapped_irq);
	UNUSED_PARAMETER(handler);
	UNUSED_PARAMETER(handler_arg);

	/* Failed: no message-signalled IRQ exists on this board. */
	return HAL_ERR_UNSUPPORTED;
}

/*
 * Refuses to detach a handler from a message-signalled IRQ.
 */
int
hal_irq_detach_msi_sync(
	int mapped_irq,
	hal_irq_handler_t handler,
	void *handler_arg)
{
	UNUSED_PARAMETER(mapped_irq);
	UNUSED_PARAMETER(handler);
	UNUSED_PARAMETER(handler_arg);

	/* Failed: no message-signalled IRQ exists on this board. */
	return HAL_ERR_UNSUPPORTED;
}

/*
 * Refuses to free a message-signalled IRQ.
 */
int
hal_irq_free_msi(
	int mapped_irq)
{
	UNUSED_PARAMETER(mapped_irq);

	/* Failed: no message-signalled IRQ exists on this board. */
	return HAL_ERR_UNSUPPORTED;
}

/*
 * Masks or unmasks one MFP source.
 *
 * The low eight sources are in the B mask register and the high eight in
 * the A one, each source's bit by its number within the eight.
 */
static void
mfp_mask(
	unsigned vector,
	int enabled)
{
	unsigned source;
	unsigned reg;
	uint8_t bit;
	uint8_t mask;

	/* Finds the source's register and bit. */
	source = vector & 15U;
	reg = MFP_IMRB;
	if (source >= 8U)
		reg = MFP_IMRA;
	bit = (uint8_t)(1U << (source & 7U));

	/* Sets or clears the bit, keeping the others. */
	mask = x68k_mfp_read(reg);
	if (enabled) {
		mask = (uint8_t)(mask | bit);
	} else {
		mask = (uint8_t)(mask & (uint8_t)~bit);
	}
	x68k_mfp_write(reg, mask);
}
