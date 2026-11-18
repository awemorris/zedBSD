/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * What the PowerPC Mac's shared support (src/hal/powermac/) asks of the
 * 64-bit PowerPC processor: the firmware's client interface, the time base and
 * the decrementer, the interrupt mask, the HAL's locks and the
 * processor's record.
 */

#ifndef KERN_HAL_PPC64_MACPPC64_MAC_ARCH_H
#define KERN_HAL_PPC64_MACPPC64_MAC_ARCH_H

#include <hal/hal.h>
#include "../asm.h"
#include "../percpu.h"
#include "../spin.h"

/* The word that starts the HAL's lines on this processor. */
#define MAC_ARCH_TAG		"PPC64"

/* The processor's name in the HAL's messages. */
#define MAC_ARCH_NAME		"ppc64"

/* The acknowledgement of the processor's own tick. */
#define MAC_ACK_LOCAL		PPC64_ACK_LOCAL

/* The processor's record: its id, the interrupt being handled and the ticks taken. */
typedef struct ppc64_percpu mac_percpu_t;

/* Reports the processor's record. */
static __inline mac_percpu_t *
mac_percpu_current(
	void)
{
	/* The one processor's record. */
	return ppc64_percpu_current();
}

/* Calls the firmware's client interface with an argument array. */
static __inline void
mac_firmware_call(
	void *arguments)
{
	/* The firmware answers in the array. */
	ppc64_firmware_call(arguments);
}

/* Reads the time base. */
static __inline uint64_t
mac_read_timebase(
	void)
{
	uint64_t value;

	/* The 64-bit count. */
	value = ppc64_read_timebase();

	/* Succeeded: the count. */
	return value;
}

/* Loads the decrementer. */
static __inline void
mac_write_decrementer(
	uint32_t value)
{
	/* The counts until the next decrementer interrupt. */
	ppc64_write_decrementer(value);
}

/* Turns interrupts off and reports whether they were on. */
static __inline bool
mac_irq_save(
	void)
{
	bool enabled;

	/* The machine state's enable bit. */
	enabled = ppc64_irq_save();

	/* Succeeded: whether interrupts were on. */
	return enabled;
}

/* Turns interrupts back on when they were on. */
static __inline void
mac_irq_restore(
	bool enabled)
{
	/* The machine state's enable bit, as it was. */
	ppc64_irq_restore(enabled);
}

/* Reports whether interrupts are off. */
static __inline bool
mac_irq_masked(
	void)
{
	bool masked;

	/* The machine state's enable bit, clear. */
	masked = ppc64_irq_masked();

	/* Succeeded: whether interrupts are off. */
	return masked;
}

/* Takes a HAL lock with interrupts off; reports whether they were on. */
static __inline bool
mac_spin_lock(
	volatile unsigned *lock)
{
	bool enabled;

	/* The lock, held until mac_spin_unlock(). */
	enabled = ppc64_spin_lock(lock);

	/* Succeeded: whether interrupts were on. */
	return enabled;
}

/* Releases a HAL lock and restores the interrupt mask. */
static __inline void
mac_spin_unlock(
	volatile unsigned *lock,
	bool enabled)
{
	/* The lock released, then the mask as it was. */
	ppc64_spin_unlock(lock, enabled);
}

#endif
