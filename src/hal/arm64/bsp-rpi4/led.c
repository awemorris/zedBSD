/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The activity LED of the Raspberry Pi 4 (GPIO 42, lit when high), used to
 * count the stages of the early boot in blinks, so that how far a board
 * gets can be seen without a serial console or a working display.
 */

#include <hal/hal.h>
#include "../defs.h"
#include "led.h"

/* The GPIO block (the default low-peripheral address) and its registers. */
#define GPIO_BASE	0xfe200000ULL
#define GPFSEL4		0x10U
#define GPSET1		0x20U
#define GPCLR1		0x2cU

/* GPIO 42: bits 6 to 8 of GPFSEL4, and bit 10 of the second set and clear. */
#define LED_FUNCTION_SHIFT	6U
#define LED_BIT			(1U << 10)

/* How long a blink is lit and dark, and the pause after a stage. */
#define BLINK_ON_MS		250U
#define BLINK_OFF_MS		350U
#define STAGE_PAUSE_MS		1200U

static volatile uint8_t *gpio_registers(void);
static uint64_t counter(void);
static uint64_t counter_frequency(void);

/*
 * Makes GPIO 42 an output, and turns the LED off.
 */
void
rpi4_led_init(
	void)
{
	volatile uint8_t *base;
	uint32_t select;

	/* The function of pin 42: output (001). */
	base = gpio_registers();
	select = hal_mmio_read32(base + GPFSEL4);
	select &= ~(7U << LED_FUNCTION_SHIFT);
	select |= 1U << LED_FUNCTION_SHIFT;
	hal_mmio_write32(base + GPFSEL4, select);

	/* Dark until a stage is shown. */
	rpi4_led_set(0);
}

/*
 * Turns the LED on or off.
 */
void
rpi4_led_set(
	int on)
{
	volatile uint8_t *base;

	/* The set register lights it, and the clear register darkens it. */
	base = gpio_registers();
	if (on)
		hal_mmio_write32(base + GPSET1, LED_BIT);
	else
		hal_mmio_write32(base + GPCLR1, LED_BIT);
}

/*
 * Shows a stage of the boot: as many blinks as its number, then a pause.
 */
void
rpi4_led_stage(
	unsigned stage)
{
	unsigned blink;

	/* The blinks. */
	for (blink = 0; blink < stage; blink++) {
		rpi4_led_set(1);
		rpi4_delay_ms(BLINK_ON_MS);
		rpi4_led_set(0);
		rpi4_delay_ms(BLINK_OFF_MS);
	}

	/* The pause that separates one stage from the next. */
	rpi4_delay_ms(STAGE_PAUSE_MS);
}

/*
 * Waits for a number of milliseconds on the generic timer.
 */
void
rpi4_delay_ms(
	unsigned milliseconds)
{
	uint64_t frequency;
	uint64_t deadline;
	uint64_t now;

	/* The count the wait ends at. */
	frequency = counter_frequency();
	now = counter();
	deadline = now + frequency / 1000U * milliseconds;

	/* Until the counter reaches it. */
	while (now < deadline)
		now = counter();
}

/* Returns the GPIO registers in the kernel's direct map. */
static volatile uint8_t *
gpio_registers(
	void)
{
	/* The physical block through the direct map. */
	return (volatile uint8_t *)(ARM64_DIRECT_BASE + GPIO_BASE);
}

/* Returns the physical count of the generic timer. */
static uint64_t
counter(
	void)
{
	uint64_t value;

	/* CNTPCT_EL0. */
	__asm__ volatile("isb; mrs %0, cntpct_el0" : "=r"(value));
	return value;
}

/* Returns the frequency of the generic timer. */
static uint64_t
counter_frequency(
	void)
{
	uint64_t value;

	/* CNTFRQ_EL0. */
	__asm__ volatile("mrs %0, cntfrq_el0" : "=r"(value));
	return value;
}
