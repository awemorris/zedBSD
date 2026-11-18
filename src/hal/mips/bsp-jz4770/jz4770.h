/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The Ingenic JZ4770's registers and the RG350's pins that the board
 * reaches before anything else runs.  The values are plain numbers, so
 * that the assembly of the boot's marks reads them as well as C.
 */

#ifndef KERN_HAL_MIPS_BSP_JZ4770_JZ4770_H
#define KERN_HAL_MIPS_BSP_JZ4770_JZ4770_H

/*
 * The GPIO block through KSEG1: one bank of registers a port, 0x100
 * bytes apart from port A.  Each register has a set and a clear alias 4
 * and 8 bytes above it.  A pin is a plain output when its INT bit is
 * clear, its MSK bit set and its PAT1 bit clear; PAT0 then drives it.
 *
 * Values referenced: Linux master drivers/pinctrl/pinctrl-ingenic.c
 * (GPIO_MSK, JZ4770_GPIO_INT, JZ4770_GPIO_PAT1, JZ4770_GPIO_PAT0,
 * REG_SET, REG_CLEAR, the port stride of jz4770_pinctrl_info and the
 * output path of ingenic_pinmux_gpio_set_direction); Linux master
 * arch/mips/boot/dts/ingenic/jz4770.dtsi (the GPIO block at 0x10010000).
 */
#define JZ4770_GPIO_BASE		0xb0010000
#define JZ4770_GPIO_PORT_STRIDE		0x100
#define JZ4770_GPIO_INT_CLEAR		0x18
#define JZ4770_GPIO_MASK_SET		0x24
#define JZ4770_GPIO_PAT1_CLEAR		0x38
#define JZ4770_GPIO_PAT0_SET		0x44
#define JZ4770_GPIO_PAT0_CLEAR		0x48

/*
 * The RG350's power LED, on PB30 and lit when the pin is low, and the
 * backlight's enable, PE1, which the board drives as a plain output
 * (high = on) until the backlight's PWM is set up.
 *
 * Values referenced: OpenDingux linux jz-6.1
 * arch/mips/boot/dts/ingenic/bits/gcw0-base.dtsi (the LED on gpb 30,
 * GPIO_ACTIVE_LOW; the backlight's PWM 1 on PE1).
 */
#define JZ4770_GPIO_PORT_B		1
#define JZ4770_GPIO_PORT_E		4
#define RG350_LED_PORT_OFFSET		(JZ4770_GPIO_PORT_B * JZ4770_GPIO_PORT_STRIDE)
#define RG350_LED_BIT			0x40000000
#define RG350_BACKLIGHT_PORT_OFFSET	(JZ4770_GPIO_PORT_E * JZ4770_GPIO_PORT_STRIDE)
#define RG350_BACKLIGHT_BIT		0x00000002

/*
 * The loops of one unit of the marks' timing, about 0.15 s: two
 * instructions a loop on the 996 MHz core from the cache.  Fetched
 * without the cache the unit is several times longer, which still
 * counts by eye.  The core's clock is the boot loader's.
 *
 * Values referenced: pcercuei UBIBoot master src/config-gcw0.h (the CPU
 * clock of 996 MHz on the RG350).
 */
#define RG350_MARK_UNIT_LOOPS		75000000

/* The units of the marks: a blink, the gap after it, and the gap before a series. */
#define RG350_MARK_ON_UNITS		2
#define RG350_MARK_OFF_UNITS		2
#define RG350_MARK_LEAD_UNITS		6
#define RG350_FAULT_LONG_UNITS		8

#endif
