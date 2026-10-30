/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Raspberry Pi 4 GPIO block: pin functions and pull resistors.
 *
 * GPFSELn holds a 3-bit function code for each of ten pins, starting at
 * offset 0; GPIO_PUP_PDN_CNTRL_REGn holds a 2-bit resistor code for each of
 * sixteen pins, starting at offset 0xe4 (BCM2711 ARM Peripherals, chapter
 * 5).  The Wi-Fi pins 34 to 39 and the Bluetooth pins 30 to 33 share
 * GPFSEL3, and pins 32 to 47 share one pull register, so every change is a
 * read-modify-write under gpio_lock.
 *
 * The HAL's LED and UART code and the headphone driver write GPFSEL1 and
 * GPFSEL4 directly, before or apart from these helpers; the Wi-Fi and
 * Bluetooth drivers use only these.
 */

#include "drivers/platform/rpi4/rpi4-gpio.h"
#include <kern/atomic.h>
#include <kern/clock.h>
#include <kern/device-io.h>
#include <kern/lock.h>
#include <kern/pmem.h>
#include <uapi/errno.h>

/* The function select registers: ten pins of three bits each per word. */
#define GPIO_FUNCTION_SELECT		0x00U
#define GPIO_FUNCTION_PINS		10U
#define GPIO_FUNCTION_BITS		3U
#define GPIO_FUNCTION_MASK		0x7U

/* The pull registers: sixteen pins of two bits each per word. */
#define GPIO_PULL_CONTROL		0xe4U
#define GPIO_PULL_PINS			16U
#define GPIO_PULL_BITS			2U
#define GPIO_PULL_MASK			0x3U

/*
 * The register's resistor codes.  Up and down are the reverse of the
 * device tree's numbers.
 */
#define GPIO_PULL_CODE_NONE		0x0U
#define GPIO_PULL_CODE_UP		0x1U
#define GPIO_PULL_CODE_DOWN		0x2U

/* The device tree's brcm,pull numbers. */
#define GPIO_DT_PULL_NONE		0U
#define GPIO_DT_PULL_DOWN		1U
#define GPIO_DT_PULL_UP			2U

/*
 * The registers mapped: up to the end of the last pull register.  The Pi
 * 4's device tree gives the block only 0xb4 bytes, which stops short of the
 * pull registers, so the span is fixed here instead of read from it.
 */
#define GPIO_REGISTER_SPAN		0xf4U

/* The least block a tree may describe: the six function select registers. */
#define GPIO_REGISTER_MINIMUM		0x18U

/* How long a caller waits between looks at another CPU's preparation. */
#define GPIO_PREPARE_POLL_US		10U
#define GPIO_PREPARE_POLL_MAX_US	20U

/*
 * The steps of the block's one-time preparation.
 *
 * ABSENT: nobody has mapped it, or the last attempt failed.  PREPARING: one
 * caller is mapping it.  READY: the mapping is filled and never changes.
 */
#define GPIO_STATE_ABSENT		0U
#define GPIO_STATE_PREPARING		1U
#define GPIO_STATE_READY		2U

/*
 * The block's registers.
 *
 * Mapped once by drv_rpi4_gpio_init() and kept for as long as the kernel
 * runs.  Only read after gpio_state has been seen READY with an acquire
 * load, which orders the read after the store that filled it.
 */
static volatile uint8_t *gpio_registers;

/*
 * How far the block's preparation has come: one of GPIO_STATE_*.
 *
 * The caller that moves it from ABSENT to PREPARING with a
 * compare-and-exchange is the only one that fills gpio_registers; it then
 * stores READY with release.  A failed preparation stores ABSENT again so a
 * later caller may retry.  Zero means every helper but the conversion
 * reports ENODEV.
 */
static volatile unsigned gpio_state;

/*
 * Serializes every read-modify-write of the function and pull registers.
 *
 * A leaf lock: nothing else is taken while it is held.  Taken with
 * interrupts held off, so a helper may also run from an interrupt.
 */
static struct spinlock gpio_lock = {
	{ 0 }, LOCK_RANK_DEVICE, "rpi4 gpio", 0, 0
};

static int prepare_registers(const struct drv_fdt *fdt);
static int registers_ready(void);
static int pull_code(enum drv_rpi4_gpio_pull pull, uint32_t *code);
static void update_field(unsigned offset, unsigned shift, uint32_t mask, uint32_t value);

/*
 * Finds the GPIO block in the device tree and maps its registers.
 *
 * Several drivers may call it, possibly on different CPUs at once; exactly
 * one of them maps the block and the others wait until it is ready.
 */
int
drv_rpi4_gpio_init(
	const struct drv_fdt *fdt)
{
	unsigned state;
	unsigned expected;
	int claimed;
	int error;

	/* Refuses a missing tree. */
	if (fdt == NULL)
		return EINVAL;

	/*
	 * Takes the preparation, or waits while another CPU holds it.  The
	 * holder waits on nothing but the device tree and the mapping, so the
	 * wait ends.
	 */
	for (;;) {
		/* Does nothing once the block is ready. */
		state = atomic_raw_load_acquire(&gpio_state);
		if (state == GPIO_STATE_READY)
			return 0;

		/* Becomes the one caller that maps the block. */
		expected = GPIO_STATE_ABSENT;
		claimed = atomic_raw_compare_exchange(&gpio_state, &expected, GPIO_STATE_PREPARING);
		if (claimed)
			break;

		/* Waits one polling interval for the other CPU's preparation. */
		kern_usleep_range(GPIO_PREPARE_POLL_US, GPIO_PREPARE_POLL_MAX_US);
	}

	/* Finds and maps the registers. */
	error = prepare_registers(fdt);
	if (error != 0) {
		/* Gives the preparation back, so a later caller may try again. */
		atomic_raw_store_release(&gpio_state, GPIO_STATE_ABSENT);
		return error;
	}

	/* READY publishes the mapping: a CPU that loads it with acquire sees it filled. */
	atomic_raw_store_release(&gpio_state, GPIO_STATE_READY);

	/* Succeeded: the helpers may now change pins. */
	return 0;
}

/*
 * Selects a pin's function.
 */
int
drv_rpi4_gpio_set_function(
	unsigned pin,
	enum drv_rpi4_gpio_function function)
{
	unsigned offset;
	unsigned shift;
	uint32_t code;
	int error;

	/* Refuses a pin the block does not have. */
	if (pin >= DRV_RPI4_GPIO_PIN_COUNT)
		return EINVAL;

	/* Refuses a code wider than the three bits of a function. */
	code = (uint32_t)function;
	if (code > GPIO_FUNCTION_MASK)
		return EINVAL;

	/* Refuses a change before the block is mapped. */
	error = registers_ready();
	if (error != 0)
		return error;

	/* Finds the pin's register and its three bits. */
	offset = GPIO_FUNCTION_SELECT + 4U * (pin / GPIO_FUNCTION_PINS);
	shift = GPIO_FUNCTION_BITS * (pin % GPIO_FUNCTION_PINS);

	/* Writes the new function, keeping the other nine pins of the register. */
	update_field(offset, shift, GPIO_FUNCTION_MASK, code);

	/* Succeeded: the pin has its new function. */
	return 0;
}

/*
 * Reads a pin's current function.
 */
int
drv_rpi4_gpio_get_function(
	unsigned pin,
	enum drv_rpi4_gpio_function *function)
{
	unsigned long enabled;
	unsigned offset;
	unsigned shift;
	uint32_t select;
	int error;

	/* Refuses a pin the block does not have. */
	if (pin >= DRV_RPI4_GPIO_PIN_COUNT)
		return EINVAL;

	/* Refuses nowhere to put the answer. */
	if (function == NULL)
		return EINVAL;

	/* Refuses a read before the block is mapped. */
	error = registers_ready();
	if (error != 0)
		return error;

	/* Finds the pin's register and its three bits. */
	offset = GPIO_FUNCTION_SELECT + 4U * (pin / GPIO_FUNCTION_PINS);
	shift = GPIO_FUNCTION_BITS * (pin % GPIO_FUNCTION_PINS);

	/* Reads the register between two writers, not in the middle of one. */
	enabled = spin_lock_irqsave(&gpio_lock);

	select = kern_mmio_read32(gpio_registers + offset);

	spin_unlock_irqrestore(&gpio_lock, enabled);

	/* Every 3-bit code names a function, so the answer needs no check. */
	*function = (enum drv_rpi4_gpio_function)((select >> shift) & GPIO_FUNCTION_MASK);

	/* Succeeded: function holds the pin's current function. */
	return 0;
}

/*
 * Sets a pin's pull resistor.
 */
int
drv_rpi4_gpio_set_pull(
	unsigned pin,
	enum drv_rpi4_gpio_pull pull)
{
	unsigned offset;
	unsigned shift;
	uint32_t code;
	int error;

	/* Refuses a pin the block does not have. */
	if (pin >= DRV_RPI4_GPIO_PIN_COUNT)
		return EINVAL;

	/* Converts the resistor to the register's code, refusing any other number. */
	error = pull_code(pull, &code);
	if (error != 0)
		return error;

	/* Refuses a change before the block is mapped. */
	error = registers_ready();
	if (error != 0)
		return error;

	/* Finds the pin's register and its two bits. */
	offset = GPIO_PULL_CONTROL + 4U * (pin / GPIO_PULL_PINS);
	shift = GPIO_PULL_BITS * (pin % GPIO_PULL_PINS);

	/* Writes the new resistor, keeping the other fifteen pins of the register. */
	update_field(offset, shift, GPIO_PULL_MASK, code);

	/* Succeeded: the pin has its new resistor. */
	return 0;
}

/*
 * Converts the device tree's brcm,pull value to a pull resistor.
 *
 * This is the one place that knows the tree's numbers.
 */
int
drv_rpi4_gpio_pull_from_dt(
	uint32_t brcm_pull,
	enum drv_rpi4_gpio_pull *pull)
{
	/* Refuses nowhere to put the answer. */
	if (pull == NULL)
		return EINVAL;

	/* Names the tree's number; anything else is not a resistor. */
	switch (brcm_pull) {
	case GPIO_DT_PULL_NONE:
		*pull = DRV_RPI4_GPIO_PULL_NONE;
		break;
	case GPIO_DT_PULL_DOWN:
		*pull = DRV_RPI4_GPIO_PULL_DOWN;
		break;
	case GPIO_DT_PULL_UP:
		*pull = DRV_RPI4_GPIO_PULL_UP;
		break;
	default:
		return EINVAL;
	}

	/* Succeeded: pull holds the tree's resistor. */
	return 0;
}

/*
 * Finds the GPIO block in the device tree and maps its registers.
 *
 * Only the caller holding the PREPARING state runs it, so it fills
 * gpio_registers without a lock; nothing reads it until READY.
 */
static int
prepare_registers(
	const struct drv_fdt *fdt)
{
	uint64_t address;
	uint64_t size;
	uint32_t node;
	bool enabled;
	void *registers;
	int error;

	/* Finds the block the firmware's tree describes. */
	error = drv_fdt_find_compatible(fdt, "brcm,bcm2711-gpio", DRV_FDT_NO_NODE, &node);
	if (error != 0)
		return ENODEV;

	/* Refuses a disabled block. */
	enabled = drv_fdt_node_enabled(fdt, node);
	if (!enabled)
		return ENODEV;

	/* Reads where its registers are. */
	error = drv_fdt_reg(fdt, node, 0, &address, &size);
	if (error != 0)
		return error;

	/* Refuses a block too small to hold the function select registers. */
	if (size < GPIO_REGISTER_MINIMUM)
		return EINVAL;

	/* Maps the function and pull registers. */
	error = kern_device_map(address, GPIO_REGISTER_SPAN, KERN_DEVICE_UNCACHED, &registers);
	if (error != 0)
		return error;

	/* Fills the mapping; the caller publishes it. */
	gpio_registers = registers;

	/* Succeeded: the mapping is ready to publish. */
	return 0;
}

/*
 * Reports ENODEV until the block is mapped.
 *
 * The acquire load orders the caller's register accesses after the store
 * that filled the mapping.
 */
static int
registers_ready(void)
{
	unsigned state;

	/* Refuses a block that is not mapped yet. */
	state = atomic_raw_load_acquire(&gpio_state);
	if (state != GPIO_STATE_READY)
		return ENODEV;

	/* Succeeded: gpio_registers may be used. */
	return 0;
}

/*
 * Converts a pull resistor to the register's code.
 *
 * This is the one place that knows the register's codes.
 */
static int
pull_code(
	enum drv_rpi4_gpio_pull pull,
	uint32_t *code)
{
	/* Names the register's code; a raw number from the tree or the register is refused. */
	switch (pull) {
	case DRV_RPI4_GPIO_PULL_NONE:
		*code = GPIO_PULL_CODE_NONE;
		break;
	case DRV_RPI4_GPIO_PULL_UP:
		*code = GPIO_PULL_CODE_UP;
		break;
	case DRV_RPI4_GPIO_PULL_DOWN:
		*code = GPIO_PULL_CODE_DOWN;
		break;
	default:
		return EINVAL;
	}

	/* Succeeded: code holds the register's bits. */
	return 0;
}

/*
 * Replaces one pin's field of a shared register.
 *
 * The read, the change and the write happen under gpio_lock, so two pins of
 * the same register changed on different CPUs both keep their change.
 */
static void
update_field(
	unsigned offset,
	unsigned shift,
	uint32_t mask,
	uint32_t value)
{
	unsigned long enabled;
	uint32_t word;

	/* Changes only this pin's bits of the shared register. */
	enabled = spin_lock_irqsave(&gpio_lock);

	word = kern_mmio_read32(gpio_registers + offset);
	word &= ~(mask << shift);
	word |= value << shift;
	kern_mmio_write32(gpio_registers + offset, word);

	spin_unlock_irqrestore(&gpio_lock, enabled);
}
