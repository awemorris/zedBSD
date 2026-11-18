/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The BCM2712's GPIO blocks of the brcmstb kind.
 *
 * A block is a run of banks of 32 pins.  Each bank has its own registers:
 * the open-drain enables, the data, and the direction (a set bit is an
 * input); the interrupt registers that follow are not used here.  The
 * registers are read and written directly, without a shadow copy.
 */

#include <drivers/platform/rpi5/rpi5-gio.h>
#include <kern/device-io.h>
#include <kern/lock.h>
#include <kern/pmem.h>
#include <uapi/errno.h>

/*
 * A bank's registers.
 *
 * Values referenced: Raspberry Pi Linux rpi-6.12.y
 * drivers/gpio/gpio-brcmstb.c (GIO_BANK_SIZE, GIO_DATA, GIO_IODIR and the
 * order of the GIO_REG_ registers; the direction bit is 1 for an input).
 */
#define GIO_BANK_BYTES		0x20U
#define GIO_BANK_PINS		32U
#define GIO_DATA		0x04U
#define GIO_IODIR		0x08U

/*
 * Orders the read-modify-write of every block's registers.
 *
 * Taken with interrupts off; a block has no other user.
 */
static struct spinlock gio_lock = {
	{ 0 }, LOCK_RANK_DEVICE, "rpi5 gio", 0, 0
};

static int find_pin(struct drv_rpi5_gio *gio, unsigned pin, volatile uint8_t **bank, uint32_t *bit);

/*
 * Maps the GPIO block a device tree node is.
 */
int
drv_rpi5_gio_open(
	const struct drv_fdt *fdt,
	uint32_t node,
	struct drv_rpi5_gio *gio)
{
	uint64_t address;
	uint64_t size;
	unsigned index;
	void *mapped;
	int error;

	/* Refuses a node that is not a brcmstb GPIO block. */
	if (fdt == NULL || gio == NULL)
		return EINVAL;
	error = drv_fdt_string_index(fdt, node, "compatible", "brcm,brcmstb-gpio", &index);
	if (error != 0)
		return ENODEV;

	/* Finds the block's registers. */
	error = drv_fdt_reg(fdt, node, 0, &address, &size);
	if (error != 0)
		return error;
	if (size < GIO_BANK_BYTES)
		return ERANGE;

	/* Maps them uncached. */
	error = kern_device_map(address, (size_t)size, KERN_DEVICE_UNCACHED, &mapped);
	if (error != 0)
		return error;

	/* Succeeded: the block's banks can be reached. */
	gio->registers = mapped;
	gio->size = size;
	gio->pin_count = (unsigned)(size / GIO_BANK_BYTES) * GIO_BANK_PINS;
	return 0;
}

/*
 * Makes a pin an output at the given level.
 */
int
drv_rpi5_gio_set_output(
	struct drv_rpi5_gio *gio,
	unsigned pin,
	int level)
{
	volatile uint8_t *bank;
	unsigned long enabled;
	uint32_t bits;
	uint32_t bit;
	int error;

	/* Finds the pin's bank. */
	error = find_pin(gio, pin, &bank, &bit);
	if (error != 0)
		return error;

	/* Sets the level first, then the direction, so the pin never drives the old level. */
	enabled = spin_lock_irqsave(&gio_lock);

	bits = kern_mmio_read32(bank + GIO_DATA);
	if (level != 0) {
		bits |= bit;
	} else {
		bits &= ~bit;
	}

	/* Writes the level, then makes the pin an output. */
	kern_mmio_write32(bank + GIO_DATA, bits);
	bits = kern_mmio_read32(bank + GIO_IODIR);
	bits &= ~bit;
	kern_mmio_write32(bank + GIO_IODIR, bits);

	spin_unlock_irqrestore(&gio_lock, enabled);

	/* Succeeded: the pin drives the level. */
	return 0;
}

/*
 * Reads a pin's level and whether it is an output.
 */
int
drv_rpi5_gio_get(
	struct drv_rpi5_gio *gio,
	unsigned pin,
	int *level,
	int *output)
{
	volatile uint8_t *bank;
	uint32_t data;
	uint32_t direction;
	uint32_t bit;
	int error;

	/* Refuses nowhere to put the answers. */
	if (level == NULL || output == NULL)
		return EINVAL;

	/* Finds the pin's bank. */
	error = find_pin(gio, pin, &bank, &bit);
	if (error != 0)
		return error;

	/* Reads the data and the direction. */
	data = kern_mmio_read32(bank + GIO_DATA);
	direction = kern_mmio_read32(bank + GIO_IODIR);

	/* The level is high when the data bit is set. */
	*level = 0;
	if ((data & bit) != 0)
		*level = 1;

	/* The pin is an output when its direction bit is clear. */
	*output = 0;
	if ((direction & bit) == 0)
		*output = 1;

	/* Succeeded: the level and the direction. */
	return 0;
}

/*
 * Reads the data and direction words of the bank that holds a pin.
 */
int
drv_rpi5_gio_get_bank(
	struct drv_rpi5_gio *gio,
	unsigned pin,
	uint32_t *data,
	uint32_t *direction)
{
	volatile uint8_t *bank;
	uint32_t bit;
	int error;

	/* Refuses nowhere to put the answers. */
	if (data == NULL || direction == NULL)
		return EINVAL;

	/* Finds the pin's bank. */
	error = find_pin(gio, pin, &bank, &bit);
	if (error != 0)
		return error;

	/* Succeeded: the bank's two words. */
	*data = kern_mmio_read32(bank + GIO_DATA);
	*direction = kern_mmio_read32(bank + GIO_IODIR);
	return 0;
}

/* Finds the bank of a pin and the pin's bit in it. */
static int
find_pin(
	struct drv_rpi5_gio *gio,
	unsigned pin,
	volatile uint8_t **bank,
	uint32_t *bit)
{
	/* Refuses an unmapped block or a pin it does not have. */
	if (gio == NULL || gio->registers == NULL)
		return ENODEV;
	if (pin >= gio->pin_count || pin >= DRV_RPI5_GIO_PIN_MAX)
		return EINVAL;

	/* Succeeded: the bank's registers and the bit. */
	*bank = gio->registers + (pin / GIO_BANK_PINS) * GIO_BANK_BYTES;
	*bit = 1U << (pin % GIO_BANK_PINS);
	return 0;
}
