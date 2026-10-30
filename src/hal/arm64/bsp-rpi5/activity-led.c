/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The activity LED of the Raspberry Pi 5: pin 9 of the first bank of the
 * always-on GPIO block, lit when the pin is driven low.
 */

#include <hal/hal.h>
#include "../defs.h"
#include "board.h"
#include "../led.h"

/*
 * A bank's data register holds the pin levels and its direction register
 * a one for each input; the LED is pin 9.
 *
 * Values referenced: Raspberry Pi Linux rpi-6.12.y
 * arch/arm64/boot/dts/broadcom/bcm2712-rpi-5-b.dts (led_act on gio_aon 9,
 * active low) and drivers/gpio/gpio-brcmstb.c (the bank's data and
 * direction register offsets).
 */
#define AON_GPIO_DATA		0x04U
#define AON_GPIO_IODIR		0x08U
#define LED_BIT			(1U << 9)

static volatile uint8_t *gpio_registers(void);

/*
 * Makes the activity LED's pin an output, and turns the LED off.
 */
void
arm64_led_init(
	void)
{
	volatile uint8_t *base;
	uint32_t direction;

	/* Dark first, so the pin does not light it when it turns into an output. */
	arm64_led_set(0);

	/* Clears the pin's input bit: the block drives it from now on. */
	base = gpio_registers();
	direction = hal_mmio_read32(base + AON_GPIO_IODIR);
	direction &= ~LED_BIT;
	hal_mmio_write32(base + AON_GPIO_IODIR, direction);
}

/*
 * Turns the LED on or off.
 */
void
arm64_led_set(
	int on)
{
	volatile uint8_t *base;
	uint32_t levels;

	/* Reads the bank's levels, which the other pins keep. */
	base = gpio_registers();
	levels = hal_mmio_read32(base + AON_GPIO_DATA);

	/* A low pin lights the LED, and a high one darkens it. */
	if (on) {
		levels &= ~LED_BIT;
	} else {
		levels |= LED_BIT;
	}
	hal_mmio_write32(base + AON_GPIO_DATA, levels);
}

/* Reports the always-on GPIO registers in the kernel's direct map. */
static volatile uint8_t *
gpio_registers(
	void)
{
	volatile uint8_t *registers;

	/* The physical block through the direct map. */
	registers = (volatile uint8_t *)(ARM64_DIRECT_BASE + RPI5_AON_GPIO_BASE);

	/* Succeeded: the block's first register. */
	return registers;
}
