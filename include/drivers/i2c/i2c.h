/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The I2C buses of the kernel (ws159-p002).
 *
 * A controller driver (the Intel LPSS DesignWare one, lpss-i2c.c) registers
 * each bus it drives with the PCI function it is, and a client driver (the
 * I2C-HID one) finds the bus an ACPI device names in its I2cSerialBus
 * resource by the controller's ACPI path, then makes combined write-read
 * transfers on it.  Transfers on one bus are serialized by the bus.
 */

#ifndef DRIVERS_I2C_I2C_H
#define DRIVERS_I2C_I2C_H

#include <stddef.h>
#include <stdint.h>

/* The most buses the kernel keeps. */
#define DRV_I2C_BUSES_MAX	8U

/* The standard and fast mode speeds in hertz. */
#define DRV_I2C_SPEED_STANDARD	100000U
#define DRV_I2C_SPEED_FAST	400000U

struct drv_i2c_bus;

/*
 * What a controller driver gives for one bus: its PCI function (segment,
 * bus, device and function), the transfer it makes, and the argument the
 * transfer gets.
 *
 * The transfer writes write_length bytes to the device at address, then,
 * when read_length is not zero, reads read_length bytes from it after a
 * repeated start; write_length may be zero for a read alone.  It reports 0,
 * ENXIO when the device did not acknowledge its address, EIO for another
 * failure of the bus, or ETIMEDOUT.  The registry serializes the calls of
 * one bus.
 */
struct drv_i2c_bus_ops {
	uint16_t pci_segment;
	uint8_t pci_bus;
	uint8_t pci_device;
	uint8_t pci_function;
	int (*transfer)(void *argument, uint16_t address, uint32_t speed, const uint8_t *write, size_t write_length, uint8_t *read, size_t read_length);
	void *argument;
};

int drv_i2c_bus_register(const struct drv_i2c_bus_ops *ops, struct drv_i2c_bus **result);
int drv_i2c_bus_find_acpi(const char *path, struct drv_i2c_bus **result);
int drv_i2c_transfer(struct drv_i2c_bus *bus, uint16_t address, uint32_t speed, const uint8_t *write, size_t write_length, uint8_t *read, size_t read_length);

/*
 * Holds a bus for its controller's suspend (ws052-p005): the transfer under
 * way ends, and later transfers wait until the release.  The controller's
 * driver holds it from its suspend to its resume, on the PCI power code's
 * one thread.
 */
void drv_i2c_bus_hold(struct drv_i2c_bus *bus);
void drv_i2c_bus_release(struct drv_i2c_bus *bus);

#endif
