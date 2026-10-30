/* -*- mode: c; c-file-style: "linux"; tab-width: 8; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Interrupt registration and control for drivers.
 *
 * Each call forwards to the HAL. The acknowledgement token is passed
 * through unchanged; kern_irq_ack_t and hal_irq_ack_t have the same
 * representation, so a driver can hold one without naming a HAL type.
 */

#include <uapi/errno.h>
#include <hal/hal.h>

#include "kern/irq.h"

/*
 * Reports an errno for one HAL status.
 */
static int
irq_error(
	int status)
{
	/* Maps the statuses the interrupt paths can report. */
	switch (status) {
	case HAL_OK:
		return 0;
	case HAL_ERR_INVALID:
		return EINVAL;
	case HAL_ERR_BUSY:
		return EBUSY;
	case HAL_ERR_NOMEM:
		return ENOMEM;
	case HAL_ERR_UNSUPPORTED:
		return ENOTSUP;
	case HAL_ERR_STATE:
		return EBUSY;
	default:
		return EIO;
	}
}

/*
 * Installs a handler for one numbered interrupt.
 */
int
kern_irq_register(
	int irq,
	kern_irq_handler_t handler,
	void *argument)
{
	/* The token types match, so the handler passes through unchanged. */
	return irq_error(hal_irq_register(irq, (hal_irq_handler_t)handler,
					  argument));
}

/*
 * Removes the handler registered for one numbered interrupt.
 */
int
kern_irq_unregister(
	int irq,
	kern_irq_handler_t handler,
	void *argument)
{
	/* Removal must name the registration it owns. */
	return irq_error(hal_irq_unregister(irq, (hal_irq_handler_t)handler,
					    argument));
}

/*
 * Allocates one message-signalled interrupt for a device.
 */
int
kern_irq_register_msi(
	const char *source,
	kern_irq_handler_t handler,
	void *argument,
	int *mapped_irq,
	uint64_t *mapped_address,
	uint32_t *mapped_event)
{
	hal_physaddr_t address;
	int status;

	/* Allocates the vector and its architecture mapping. */
	address = 0;
	status = hal_irq_register_msi(source, (hal_irq_handler_t)handler,
				      argument, mapped_irq, &address,
				      mapped_event);
	if (status != HAL_OK)
		return irq_error(status);

	/* Publishes the message address the device must write. */
	if (mapped_address != NULL)
		*mapped_address = (uint64_t)address;
	return 0;
}

/*
 * Releases one message-signalled interrupt.
 */
int
kern_irq_unregister_msi(
	int mapped_irq)
{
	/* Releases the vector and its mapping. */
	return irq_error(hal_irq_unregister_msi(mapped_irq));
}

/*
 * Allocates one message-signalled interrupt without a handler.
 */
int
kern_irq_alloc_msi(
	const char *source,
	int *mapped_irq,
	uint64_t *mapped_address,
	uint32_t *mapped_event)
{
	hal_physaddr_t address;
	int status;

	/* Reserves the vector and its routing; the device is not touched. */
	address = 0;
	status = hal_irq_alloc_msi(source, mapped_irq, &address, mapped_event);
	if (status != HAL_OK)
		return irq_error(status);

	/* Publishes the message address the device must write. */
	if (mapped_address != NULL)
		*mapped_address = (uint64_t)address;

	/* Succeeded: the vector is reserved for the caller. */
	return 0;
}

/*
 * Attaches a handler to a vector from kern_irq_alloc_msi().
 */
int
kern_irq_attach_msi(
	int mapped_irq,
	kern_irq_handler_t handler,
	void *argument)
{
	int status;

	/* The token types match, so the handler passes through unchanged. */
	status = hal_irq_attach_msi(mapped_irq, (hal_irq_handler_t)handler,
				    argument);
	if (status != HAL_OK)
		return irq_error(status);

	/* Succeeded: the next message runs the handler. */
	return 0;
}

/*
 * Detaches a handler and waits until no CPU still runs it.
 */
int
kern_irq_detach_msi_sync(
	int mapped_irq,
	kern_irq_handler_t handler,
	void *argument)
{
	int status;

	/* Removal must name the handler and argument that were attached. */
	status = hal_irq_detach_msi_sync(mapped_irq,
					 (hal_irq_handler_t)handler, argument);
	if (status != HAL_OK)
		return irq_error(status);

	/* Succeeded: the caller may free the handler's argument. */
	return 0;
}

/*
 * Releases a vector from kern_irq_alloc_msi().
 */
int
kern_irq_free_msi(
	int mapped_irq)
{
	int status;

	/* A vector with a handler still attached is refused as busy. */
	status = hal_irq_free_msi(mapped_irq);
	if (status != HAL_OK)
		return irq_error(status);

	/* Succeeded: the vector may be allocated again. */
	return 0;
}

/*
 * Sets the trigger mode and polarity of one numbered interrupt.
 */
int
kern_irq_set_mode(
	int irq,
	unsigned trigger,
	unsigned polarity)
{
	int hal_trigger;
	int hal_polarity;

	/* The kernel's values as the HAL's. */
	if (trigger != KERN_IRQ_TRIGGER_EDGE && trigger != KERN_IRQ_TRIGGER_LEVEL)
		return EINVAL;
	if (polarity != KERN_IRQ_POLARITY_HIGH && polarity != KERN_IRQ_POLARITY_LOW)
		return EINVAL;
	hal_trigger = HAL_IRQ_TRIGGER_EDGE;
	if (trigger == KERN_IRQ_TRIGGER_LEVEL)
		hal_trigger = HAL_IRQ_TRIGGER_LEVEL;
	hal_polarity = HAL_IRQ_POLARITY_HIGH;
	if (polarity == KERN_IRQ_POLARITY_LOW)
		hal_polarity = HAL_IRQ_POLARITY_LOW;

	/* Succeeded or not, as the HAL says. */
	return irq_error(hal_irq_set_mode(irq, hal_trigger, hal_polarity));
}

/*
 * Arms or disarms an interrupt as a wake source of a system sleep.
 */
int
kern_irq_set_wake(
	int irq,
	int enable)
{
	bool armed;
	int status;

	/* The kernel's flag as the HAL's. */
	armed = false;
	if (enable)
		armed = true;

	/* Succeeded or not, as the HAL says. */
	status = hal_irq_set_wake(irq, armed);
	if (status != HAL_OK)
		return irq_error(status);

	/* Succeeded: kept until it is changed. */
	return 0;
}

/*
 * Stops delivery of one numbered interrupt.
 */
void
kern_irq_mask(
	int irq)
{
	hal_irq_mask(irq);
}

/*
 * Resumes delivery of one numbered interrupt.
 */
void
kern_irq_unmask(
	int irq)
{
	hal_irq_unmask(irq);
}

/*
 * Retires one acknowledgement.
 */
void
kern_irq_send_eoi(
	kern_irq_ack_t acknowledge)
{
	hal_irq_send_eoi((hal_irq_ack_t)acknowledge);
}

/*
 * Disables interrupt delivery on the current processor.
 */
bool
kern_irq_disable(
	void)
{
	/* Reports whether delivery was enabled beforehand. */
	return hal_irq_disable();
}

/*
 * Restores interrupt delivery on the current processor.
 */
void
kern_irq_enable(
	void)
{
	hal_irq_enable();
}
