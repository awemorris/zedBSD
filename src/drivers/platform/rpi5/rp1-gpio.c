/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * RP1's GPIO pins.
 *
 * RP1's GPIO node has three register blocks: the pins' control registers
 * (each pin's function selector and overrides), the registered I/O block
 * that a pin in its GPIO function is driven from, and the pads (input
 * enable, output disable, pulls).  The 54 pins are in three banks, each
 * with its own place in every block.
 */

#include <drivers/platform/rpi5/rp1.h>
#include <drivers/platform/rpi5/rp1-gpio.h>
#include <kern/device-io.h>
#include <kern/klog.h>
#include <kern/lock.h>
#include <uapi/errno.h>

/*
 * The register blocks' order in the node's reg, a pin's control register,
 * the bits of the control register and of a pad the module changes, the
 * registered I/O block's output and output-enable registers, and the
 * offsets of a block's atomic set and clear aliases.
 *
 * Values referenced: Raspberry Pi Linux rpi-6.12.y
 * drivers/pinctrl/pinctrl-rp1.c (RP1_GPIO_CTRL, RP1_GPIO_CTRL_FUNCSEL_MASK,
 * RP1_GPIO_CTRL_OUTOVER_MASK, RP1_GPIO_CTRL_OEOVER_MASK, RP1_OUTOVER_PERI,
 * RP1_OEOVER_PERI, RP1_PAD_PULL_MASK, RP1_PAD_PULL_LSB,
 * RP1_PAD_IN_ENABLE_MASK, RP1_PAD_OUT_DISABLE_MASK,
 * RP1_RIO_OUT, RP1_RIO_OE, RP1_SET_OFFSET, RP1_CLR_OFFSET, the stride of
 * a pin's registers in rp1_pinctrl_probe()).
 */
#define RP1_GPIO_BLOCK_CONTROL		0U
#define RP1_GPIO_BLOCK_RIO		1U
#define RP1_GPIO_BLOCK_PADS		2U
#define RP1_GPIO_BLOCK_COUNT		3U
#define RP1_GPIO_CONTROL_STRIDE		8U
#define RP1_GPIO_CONTROL		4U
#define RP1_GPIO_FUNCSEL_MASK		0x0000001fU
#define RP1_GPIO_OUTOVER_MASK		0x00003000U
#define RP1_GPIO_OEOVER_MASK		0x0000c000U
#define RP1_GPIO_PAD_STRIDE		4U
#define RP1_GPIO_PAD_PULL_MASK		0x0000000cU
#define RP1_GPIO_PAD_PULL_SHIFT		2U
#define RP1_GPIO_PAD_INPUT_ENABLE	0x00000040U
#define RP1_GPIO_PAD_OUTPUT_DISABLE	0x00000080U
#define RP1_GPIO_RIO_OUT		0x00U
#define RP1_GPIO_RIO_OE			0x04U
#define RP1_GPIO_SET_ALIAS		0x2000U
#define RP1_GPIO_CLEAR_ALIAS		0x3000U

/* RP1's banks of pins. */
#define RP1_GPIO_BANK_COUNT		3U

/* The highest function selector a pin has. */
#define RP1_GPIO_FUNCTION_MAX		8U

/*
 * One bank of pins: the first pin, how many, and where the bank's
 * registers start in the control, registered I/O and pads blocks.
 */
struct rp1_gpio_bank {
	unsigned first_pin;
	unsigned pin_count;
	unsigned control_offset;
	unsigned rio_offset;
	unsigned pads_offset;
};

/*
 * The mapped GPIO block.
 *
 * Filled once by drv_rp1_gpio_init() during the platform's start-up and
 * not changed afterwards; mapped is zero until then.  The lock orders the
 * read-modify-write of control and pad registers between drivers.
 */
struct rp1_gpio_state {
	int mapped;
	volatile uint8_t *blocks[RP1_GPIO_BLOCK_COUNT];
	struct spinlock lock;
};

/*
 * RP1's three banks.
 *
 * Values referenced: Raspberry Pi Linux rpi-6.12.y
 * drivers/pinctrl/pinctrl-rp1.c (rp1_iobanks: pins 0-27, 28-33 and 34-53,
 * at 0x0000, 0x4000 and 0x8000, pads after a 4-byte voltage register).
 */
static const struct rp1_gpio_bank rp1_gpio_banks[RP1_GPIO_BANK_COUNT] = {
	{ 0U, 28U, 0x0000U, 0x0000U, 0x0004U },
	{ 28U, 6U, 0x4000U, 0x4000U, 0x4004U },
	{ 34U, 20U, 0x8000U, 0x8000U, 0x8004U },
};

/* The one GPIO block of RP1. */
static struct rp1_gpio_state rp1_gpio;

static int find_bank(unsigned pin, const struct rp1_gpio_bank **bank, unsigned *index);
static void enable_pad(const struct rp1_gpio_bank *bank, unsigned index);
static void select_function(const struct rp1_gpio_bank *bank, unsigned index, unsigned function);

/*
 * Finds RP1's GPIO block and maps it.
 */
int
drv_rp1_gpio_init(
	void)
{
	uint64_t size;
	uint32_t node;
	unsigned block;
	int error;

	/* Does nothing a second time. */
	if (rp1_gpio.mapped)
		return 0;

	/* Finds the GPIO node among RP1's peripherals. */
	error = drv_rp1_find_child("raspberrypi,rp1-gpio", DRV_FDT_NO_NODE, &node);
	if (error != 0) {
		kern_logf("rp1: no GPIO block in the device tree\n");
		return ENODEV;
	}

	/* Maps its three register blocks. */
	for (block = 0; block < RP1_GPIO_BLOCK_COUNT; block++) {
		error = drv_rp1_map(node, block, &rp1_gpio.blocks[block], &size);
		if (error != 0) {
			kern_logf("rp1: GPIO block %u unavailable (%d)\n", block, error);
			return error;
		}
	}

	/* Prepares the lock of the control and pad registers. */
	spin_init(&rp1_gpio.lock, LOCK_RANK_DEVICE, "RP1 GPIO");

	/* Succeeded: the pins can be set. */
	rp1_gpio.mapped = 1;
	return 0;
}

/*
 * Connects a pin to one of its peripheral functions.
 */
int
drv_rp1_gpio_set_function(
	unsigned pin,
	unsigned function)
{
	const struct rp1_gpio_bank *bank;
	unsigned long enabled;
	unsigned index;
	int error;

	/* Refuses a request before the block was mapped, or a selector a pin does not have. */
	if (!rp1_gpio.mapped)
		return ENODEV;
	if (function > RP1_GPIO_FUNCTION_MAX)
		return EINVAL;

	/* Finds the pin's bank. */
	error = find_bank(pin, &bank, &index);
	if (error != 0)
		return error;

	/* Enables the pad, then hands the pin to the function. */
	enabled = spin_lock_irqsave(&rp1_gpio.lock);

	enable_pad(bank, index);
	select_function(bank, index, function);

	spin_unlock_irqrestore(&rp1_gpio.lock, enabled);

	/* Succeeded: the pin carries the function. */
	return 0;
}

/*
 * Sets the pull of a pin's pad.
 */
int
drv_rp1_gpio_set_pull(
	unsigned pin,
	unsigned pull)
{
	const struct rp1_gpio_bank *bank;
	volatile uint8_t *pad;
	unsigned long enabled;
	unsigned index;
	uint32_t bits;
	int error;

	/* Refuses a request before the block was mapped, or a pull the pad does not have. */
	if (!rp1_gpio.mapped)
		return ENODEV;
	if (pull > DRV_RP1_GPIO_PULL_UP)
		return EINVAL;

	/* Finds the pin's bank. */
	error = find_bank(pin, &bank, &index);
	if (error != 0)
		return error;
	pad = rp1_gpio.blocks[RP1_GPIO_BLOCK_PADS] + bank->pads_offset + index * RP1_GPIO_PAD_STRIDE;

	/* Rewrites the pad's pull field, leaving its other settings. */
	enabled = spin_lock_irqsave(&rp1_gpio.lock);

	bits = kern_mmio_read32(pad);
	bits &= ~RP1_GPIO_PAD_PULL_MASK;
	bits |= pull << RP1_GPIO_PAD_PULL_SHIFT;
	kern_mmio_write32(pad, bits);

	spin_unlock_irqrestore(&rp1_gpio.lock, enabled);

	/* Succeeded: the pad has the pull. */
	return 0;
}

/*
 * Makes a pin a GPIO output at the given level.
 */
int
drv_rp1_gpio_set_output(
	unsigned pin,
	int level)
{
	const struct rp1_gpio_bank *bank;
	volatile uint8_t *rio;
	unsigned long enabled;
	unsigned index;
	uint32_t bit;
	int error;

	/* Refuses a request before the block was mapped. */
	if (!rp1_gpio.mapped)
		return ENODEV;

	/* Finds the pin's bank. */
	error = find_bank(pin, &bank, &index);
	if (error != 0)
		return error;
	rio = rp1_gpio.blocks[RP1_GPIO_BLOCK_RIO] + bank->rio_offset;
	bit = 1U << index;

	/*
	 * Sets the level first and then the output enable, so the pin never
	 * drives the old level, and only then gives it to the GPIO function.
	 */
	enabled = spin_lock_irqsave(&rp1_gpio.lock);

	if (level != 0) {
		kern_mmio_write32(rio + RP1_GPIO_RIO_OUT + RP1_GPIO_SET_ALIAS, bit);
	} else {
		kern_mmio_write32(rio + RP1_GPIO_RIO_OUT + RP1_GPIO_CLEAR_ALIAS, bit);
	}

	/* Turns the output on and hands the pin to the GPIO function. */
	kern_mmio_write32(rio + RP1_GPIO_RIO_OE + RP1_GPIO_SET_ALIAS, bit);
	enable_pad(bank, index);
	select_function(bank, index, DRV_RP1_GPIO_FUNCTION_GPIO);

	spin_unlock_irqrestore(&rp1_gpio.lock, enabled);

	/* Succeeded: the pin drives the level. */
	return 0;
}

/* Finds the bank of a pin and the pin's place in it. */
static int
find_bank(
	unsigned pin,
	const struct rp1_gpio_bank **bank,
	unsigned *index)
{
	unsigned entry;

	/* Looks for the bank whose range holds the pin. */
	for (entry = 0; entry < RP1_GPIO_BANK_COUNT; entry++) {
		/* Skips a bank that starts after the pin. */
		if (pin < rp1_gpio_banks[entry].first_pin)
			continue;

		/* Stops at the bank that does not end before it. */
		if (pin - rp1_gpio_banks[entry].first_pin < rp1_gpio_banks[entry].pin_count)
			break;
	}

	/* Refuses a pin RP1 does not have. */
	if (entry == RP1_GPIO_BANK_COUNT)
		return EINVAL;

	/* Succeeded: the pin's bank and its index there. */
	*bank = &rp1_gpio_banks[entry];
	*index = pin - rp1_gpio_banks[entry].first_pin;
	return 0;
}

/* Turns a pin's input buffer on and its output disable off; the caller holds the lock. */
static void
enable_pad(
	const struct rp1_gpio_bank *bank,
	unsigned index)
{
	volatile uint8_t *pad;
	uint32_t bits;

	/* Updates the pin's pad register. */
	pad = rp1_gpio.blocks[RP1_GPIO_BLOCK_PADS] + bank->pads_offset + index * RP1_GPIO_PAD_STRIDE;
	bits = kern_mmio_read32(pad);
	bits |= RP1_GPIO_PAD_INPUT_ENABLE;
	bits &= ~RP1_GPIO_PAD_OUTPUT_DISABLE;
	kern_mmio_write32(pad, bits);
}

/*
 * Selects a pin's function with the function controlling the output and
 * its enable; the caller holds the lock.
 *
 * Procedure referenced: Raspberry Pi Linux rpi-6.12.y
 * drivers/pinctrl/pinctrl-rp1.c (rp1_set_fsel(): the overrides set to the
 * peripheral in the same write as the selector).
 */
static void
select_function(
	const struct rp1_gpio_bank *bank,
	unsigned index,
	unsigned function)
{
	volatile uint8_t *control;
	uint32_t bits;

	/* Rewrites the selector with both overrides at zero, which is the peripheral. */
	control = rp1_gpio.blocks[RP1_GPIO_BLOCK_CONTROL] + bank->control_offset + index * RP1_GPIO_CONTROL_STRIDE + RP1_GPIO_CONTROL;
	bits = kern_mmio_read32(control);
	bits &= ~RP1_GPIO_OUTOVER_MASK;
	bits &= ~RP1_GPIO_OEOVER_MASK;
	bits &= ~RP1_GPIO_FUNCSEL_MASK;
	bits |= function;
	kern_mmio_write32(control, bits);
}
