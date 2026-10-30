/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The activity LED of the Raspberry Pi 4: GPIO 42, lit when high.
 */

#include <hal/hal.h>
#include "../defs.h"
#include "board.h"
#include "../led.h"

/* The GPIO block's function select for pins 40 to 49, and the set and clear registers of pins 32 to 57. */
#define GPFSEL4			0x10U
#define GPSET1			0x20U
#define GPCLR1			0x2cU

/* GPIO 42: bits 6 to 8 of GPFSEL4, and bit 10 of the second set and clear. */
#define LED_FUNCTION_SHIFT	6U
#define LED_BIT			(1U << 10)

static volatile uint8_t *gpio_registers(void);

/*
 * Makes GPIO 42 an output, and turns the LED off.
 */
void
arm64_led_init(
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
	arm64_led_set(0);
}

/*
 * Turns the LED on or off.
 */
void
arm64_led_set(
	int on)
{
	volatile uint8_t *base;

	/* The set register lights it, and the clear register darkens it. */
	base = gpio_registers();
	if (on) {
		hal_mmio_write32(base + GPSET1, LED_BIT);
	} else {
		hal_mmio_write32(base + GPCLR1, LED_BIT);
	}
}

/* Reports the GPIO registers in the kernel's direct map. */
static volatile uint8_t *
gpio_registers(
	void)
{
	volatile uint8_t *registers;

	/* The physical block through the direct map. */
	registers = (volatile uint8_t *)(ARM64_DIRECT_BASE + RPI4_GPIO_BASE);

	/* Succeeded: the block's first register. */
	return registers;
}
