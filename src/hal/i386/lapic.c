/* -*- mode: c; c-file-style: "linux"; tab-width: 8; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The shared i386 xAPIC implementation.
 */

#include <hal/hal.h>

#include "defs.h"
#include "lapic.h"

#define LAPIC_ID 0x020U
#define LAPIC_EOI 0x0b0U
#define LAPIC_SVR 0x0f0U
#define LAPIC_ESR 0x280U
#define LAPIC_ICR_LOW 0x300U
#define LAPIC_ICR_HIGH 0x310U
#define LAPIC_LVT_TIMER 0x320U
#define LAPIC_TIMER_INITIAL 0x380U
#define LAPIC_TIMER_CURRENT 0x390U
#define LAPIC_TIMER_DIVIDE 0x3e0U
#define LAPIC_ENABLE 0x100U
#define LAPIC_MASKED 0x10000U
#define LAPIC_PERIODIC 0x20000U
#define LAPIC_TIMER_VECTOR 0xe0U
#define LAPIC_SPURIOUS_VECTOR 0xdfU

static volatile uint32_t *base;

static uint32_t lapic_read(unsigned reg);
static void lapic_write(unsigned reg, uint32_t value);
static int wait_icr(void);
static int send(uint8_t id, uint32_t low);

/*
 * Maps and enables the local APIC.
 */
int
i386_lapic_init(
	uint32_t address)
{
	uint32_t high;
	uint32_t low;

	/* Rejects addresses outside the architectural local-APIC window. */
	if (address < 0xfec00000U || address >= 0xff000000U ||
	    (address & 0xfffU) != 0U) {
		return HAL_ERR_UNSUPPORTED;
	}

	/* Publishes the direct-mapped local-APIC register base. */
	base = (volatile uint32_t *)(uintptr_t)address;

	/* Enables the selected physical APIC base through IA32_APIC_BASE. */
	__asm__ volatile("rdmsr"
	    : "=a"(low), "=d"(high)
	    : "c"(0x1bU));
	low = (low & 0x00000fffU) | address | (1U << 11);
	__asm__ volatile("wrmsr"
	    :
	    : "a"(low), "d"(high), "c"(0x1bU));

	/* Initializes this CPU's local controller registers. */
	i386_lapic_init_cpu();

	/* Reports an enabled local APIC. */
	return HAL_OK;
}

/*
 * Initializes local-APIC registers for the current CPU.
 */
void
i386_lapic_init_cpu(
	void)
{
	/* Enables the APIC and installs its spurious interrupt vector. */
	lapic_write(LAPIC_SVR, LAPIC_ENABLE | LAPIC_SPURIOUS_VECTOR);

	/* Clears both architecturally required error-status samples. */
	lapic_write(LAPIC_ESR, 0);
	lapic_write(LAPIC_ESR, 0);

	/* Leaves the timer masked with the kernel's timer vector selected. */
	lapic_write(LAPIC_LVT_TIMER, LAPIC_MASKED | LAPIC_TIMER_VECTOR);
	lapic_write(LAPIC_TIMER_DIVIDE, 3U);

	/* Clears any outstanding local interrupt acknowledgement. */
	lapic_write(LAPIC_EOI, 0);
}

/*
 * Reads the current local-APIC identifier.
 */
uint8_t
i386_lapic_id(
	void)
{
	uint32_t id_register;

	/* Reads and extracts the xAPIC identifier field. */
	id_register = lapic_read(LAPIC_ID);

	/* Returns the eight-bit local-APIC identifier. */
	return (uint8_t)(id_register >> 24);
}

/*
 * Completes the current local-APIC interrupt.
 */
void
i386_lapic_eoi(
	void)
{
	/* Writes the local end-of-interrupt register. */
	lapic_write(LAPIC_EOI, 0);
}

/*
 * Sends an INIT assertion and deassertion to one local APIC.
 */
int
i386_lapic_send_init(
	uint8_t id)
{
	volatile unsigned delay_count;
	int result;

	/* Sends the INIT assertion before the required delivery delay. */
	result = send(id, 0x0000c500U);

	/* Propagates a failed INIT assertion without delaying or deasserting. */
	if (result != HAL_OK)
		return result;

	/* Delays between INIT assertion and deassertion. */
	for (delay_count = 0; delay_count < 100000U; delay_count++)
		__asm__ volatile("pause");

	/* Sends the matching INIT deassertion. */
	result = send(id, 0x00008500U);

	/* Returns the deassertion delivery result. */
	return result;
}

/*
 * Sends a startup IPI to one local APIC.
 */
int
i386_lapic_send_startup(
	uint8_t id,
	uint8_t vector)
{
	int result;

	/* Sends the startup delivery mode with the trampoline vector. */
	result = send(id, 0x00004600U | vector);

	/* Returns the startup delivery result. */
	return result;
}

/*
 * Sends a fixed-vector IPI to one local APIC.
 */
int
i386_lapic_send_fixed(
	uint8_t id,
	uint8_t vector)
{
	int result;

	/* Sends the fixed delivery mode with the requested vector. */
	result = send(id, vector);

	/* Returns the fixed-vector delivery result. */
	return result;
}

/*
 * Starts a masked local timer calibration interval.
 */
void
i386_lapic_timer_prepare(
	void)
{
	/* Programs the divider, masked vector, and maximum initial count. */
	lapic_write(LAPIC_TIMER_DIVIDE, 3U);
	lapic_write(LAPIC_LVT_TIMER, LAPIC_MASKED | LAPIC_TIMER_VECTOR);
	lapic_write(LAPIC_TIMER_INITIAL, 0xffffffffU);
}

/*
 * Reports elapsed local-timer ticks during calibration.
 */
uint32_t
i386_lapic_timer_elapsed(
	void)
{
	uint32_t current;

	/* Reads the remaining decrementing timer count. */
	current = lapic_read(LAPIC_TIMER_CURRENT);

	/* Returns the ticks elapsed from the maximum initial count. */
	return 0xffffffffU - current;
}

/*
 * Starts the periodic local-APIC timer.
 */
void
i386_lapic_timer_start(
	uint32_t ticks)
{
	uint32_t initial;

	/* Converts a zero interval to the minimum programmable count. */
	initial = ticks != 0U ? ticks : 1U;

	/* Programs the divider, periodic vector, and initial count in order. */
	lapic_write(LAPIC_TIMER_DIVIDE, 3U);
	lapic_write(LAPIC_LVT_TIMER, LAPIC_PERIODIC | LAPIC_TIMER_VECTOR);
	lapic_write(LAPIC_TIMER_INITIAL, initial);
}

/*
 * Stops and masks the local-APIC timer.
 */
void
i386_lapic_timer_stop(
	void)
{
	/* Masks the timer before clearing its initial count. */
	lapic_write(LAPIC_LVT_TIMER, LAPIC_MASKED | LAPIC_TIMER_VECTOR);
	lapic_write(LAPIC_TIMER_INITIAL, 0);
}

/* Reads one local-APIC register. */
static uint32_t
lapic_read(
	unsigned reg)
{
	/* Returns the volatile MMIO register value. */
	return base[reg / 4U];
}

/* Writes one local-APIC register and completes its I/O ordering. */
static void
lapic_write(
	unsigned reg,
	uint32_t value)
{
	/* Publishes the volatile MMIO write before the full I/O barrier. */
	base[reg / 4U] = value;
	hal_io_mb();
}

/* Waits for the local-APIC interrupt-command register to become idle. */
static int
wait_icr(
	void)
{
	uint32_t command;
	unsigned attempt;

	/* Polls the delivery-status bit for a bounded number of pauses. */
	for (attempt = 0; attempt < 1000000U; attempt++) {
		command = lapic_read(LAPIC_ICR_LOW);

		/* Reports readiness at the first idle delivery-status sample. */
		if ((command & (1U << 12)) == 0U)
			return HAL_OK;
		__asm__ volatile("pause");
	}

	/* Reports an interrupt-command timeout. */
	return HAL_ERR_TIMEOUT;
}

/* Sends one local-APIC interrupt-command value. */
static int
send(
	uint8_t id,
	uint32_t low)
{
	int result;

	/* Waits until no earlier interrupt command is in flight. */
	result = wait_icr();

	/* Reports a timeout before publishing a new interrupt command. */
	if (result != HAL_OK)
		return HAL_ERR_TIMEOUT;

	/* Writes the destination before publishing the low command word. */
	lapic_write(LAPIC_ICR_HIGH, (uint32_t)id << 24);
	lapic_write(LAPIC_ICR_LOW, low);

	/* Waits for this interrupt-command delivery to complete. */
	result = wait_icr();

	/* Returns the delivery completion result. */
	return result;
}
