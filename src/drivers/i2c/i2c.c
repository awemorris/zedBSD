/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The registry of the kernel's I2C buses (ws159-p002).
 *
 * Controller drivers register their buses while PCI binds them, before the
 * ACPI tables are loaded.  A client finds a bus later by the ACPI path its
 * I2cSerialBus resource names: the path's _ADR gives the PCI device and
 * function of the controller, which is compared with the buses' own.  The
 * Intel PCH's serial I/O controllers sit on the root bus of segment 0,
 * where their ACPI devices are, so the bus number compared is 0.
 */

#include <drivers/i2c/i2c.h>
#include <kern/kcrt.h>
#include <kern/lock.h>
#include <uapi/errno.h>

#if CONFIG_DRIVER_ACPI
#include <drivers/acpi/acpi.h>
#endif

#include <stdbool.h>

/* The bus number of the root bus the PCH's controllers are on. */
#define I2C_ROOT_BUS		0U

/*
 * One registered bus: what its controller gave, and the lock that keeps
 * its transfers one at a time.
 *
 * A bus is registered once and never removed: the LPSS controllers are
 * part of the chipset and are not unplugged.
 */
struct drv_i2c_bus {
	struct drv_i2c_bus_ops ops;
	struct mutex lock;
	bool used;
};

/*
 * The buses, filled in the order the controllers attach.
 *
 * registry_lock protects bus_count and the used flags; a slot, once used,
 * keeps its ops until the kernel stops.  bus_count is 0 until the first
 * controller attaches.
 */
static struct drv_i2c_bus buses[DRV_I2C_BUSES_MAX];
static unsigned bus_count;

/*
 * The lock of the registry, and whether it has been made.
 *
 * The first registration makes it; the controllers attach one at a time
 * from PCI's probe, so that first registration does not race another.
 */
static struct mutex registry_lock;
static bool registry_ready;

/*
 * Registers one bus of a controller and gives the caller its handle.
 */
int
drv_i2c_bus_register(
	const struct drv_i2c_bus_ops *ops,
	struct drv_i2c_bus **result)
{
	struct drv_i2c_bus *bus;
	int error;

	/* Refuses a bus without a transfer or a place for the handle. */
	if (ops == NULL || ops->transfer == NULL || result == NULL)
		return EINVAL;

	/* Makes the registry's lock at the first registration. */
	if (!registry_ready) {
		error = mutex_init(&registry_lock, LOCK_RANK_DEVICE, "i2c registry");
		if (error != 0)
			return error;
		registry_ready = true;
	}

	/* Takes the next free slot. */
	mutex_lock(&registry_lock);

	bus = NULL;
	if (bus_count < DRV_I2C_BUSES_MAX) {
		bus = &buses[bus_count];
		bus_count++;
	}

	mutex_unlock(&registry_lock);

	/* Every slot is used. */
	if (bus == NULL)
		return ENOSPC;

	/* Fills the bus and its transfer lock. */
	error = mutex_init(&bus->lock, LOCK_RANK_DEVICE, "i2c bus");
	if (error != 0)
		return error;
	bus->ops = *ops;
	bus->used = true;

	/* Succeeded: the bus can be found and used. */
	*result = bus;
	return 0;
}

/*
 * Finds the bus whose controller is the ACPI device at path.
 */
int
drv_i2c_bus_find_acpi(
	const char *path,
	struct drv_i2c_bus **result)
{
#if CONFIG_DRIVER_ACPI
	struct drv_acpi_node *node;
	uint64_t address;
	unsigned device;
	unsigned function;
	unsigned index;
	int error;

	/* Refuses a missing path or result. */
	if (path == NULL || result == NULL)
		return EINVAL;

	/* Finds the controller's device in the namespace. */
	error = drv_acpi_lookup(NULL, path, &node);
	if (error != 0)
		return error;

	/* Its address on the PCI bus: the device in the high word, the function in the low one. */
	error = drv_acpi_evaluate_integer(node, "_ADR", &address);
	if (error != 0)
		return error;
	device = (unsigned)((address >> 16) & 0xffffU);
	function = (unsigned)(address & 0xffffU);

	/* No registry yet: no controller has attached. */
	if (!registry_ready)
		return ENODEV;

	/* Looks for the bus of that function. */
	mutex_lock(&registry_lock);

	*result = NULL;
	for (index = 0; index < bus_count; index++) {
		/* A bus of another function, or of another bus or segment, is not it. */
		if (buses[index].ops.pci_segment != 0U)
			continue;
		if (buses[index].ops.pci_bus != I2C_ROOT_BUS)
			continue;
		if (buses[index].ops.pci_device != device)
			continue;
		if (buses[index].ops.pci_function != function)
			continue;

		/* The controller's bus. */
		*result = &buses[index];
		break;
	}

	mutex_unlock(&registry_lock);

	/* No driver drives that controller. */
	if (*result == NULL)
		return ENODEV;

	/* Succeeded: the bus is found. */
	return 0;
#else
	/* Without ACPI no path names a bus. */
	(void)path;
	(void)result;
	return ENODEV;
#endif
}

/*
 * Makes one combined write-read transfer on a bus, after the transfers
 * other clients have started on it.
 */
int
drv_i2c_transfer(
	struct drv_i2c_bus *bus,
	uint16_t address,
	uint32_t speed,
	const uint8_t *write,
	size_t write_length,
	uint8_t *read,
	size_t read_length)
{
	int error;

	/* Refuses a missing bus, or a buffer missing for its length. */
	if (bus == NULL || !bus->used)
		return EINVAL;
	if (write_length != 0U && write == NULL)
		return EINVAL;
	if (read_length != 0U && read == NULL)
		return EINVAL;

	/* Lets the controller make the transfer, one at a time on the bus. */
	mutex_lock(&bus->lock);

	error = bus->ops.transfer(bus->ops.argument, address, speed, write, write_length, read, read_length);

	mutex_unlock(&bus->lock);

	/* Reports the controller's failure. */
	if (error != 0)
		return error;

	/* Succeeded: the bytes were written and read. */
	return 0;
}

/*
 * Holds a bus for its controller's suspend: waits for the transfer under
 * way and keeps later ones waiting until drv_i2c_bus_release().
 */
void
drv_i2c_bus_hold(
	struct drv_i2c_bus *bus)
{
	/* Takes the lock every transfer takes. */
	mutex_lock(&bus->lock);
}

/*
 * Lets the transfers that waited for a suspended controller run.
 */
void
drv_i2c_bus_release(
	struct drv_i2c_bus *bus)
{
	/* Gives the lock back. */
	mutex_unlock(&bus->lock);
}
