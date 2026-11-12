/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The PCIe binding of GHCI: a function of base class 03h (subclass 02h
 * or 80h), or one of the known GHCI boards whose endpoint may report
 * another class, whose BAR0 holds the register window (identified by
 * GHCI_MAGIC at offset 0), whose optional BAR2 is the window onto the
 * GPU's local memory, and whose one interrupt is a message (MSI-X or MSI)
 * or the INTx pin.
 *
 * The attach maps the windows only.  The interrupt is taken and bus
 * mastering turned on by the start (through ghci_bus_ops) once the GPU
 * has been reset, so that neither an interrupt nor a DMA a kernel before
 * this one left behind reaches this one.  Bus mastering that the firmware
 * left on (a boot display may be scanning through the GPU) is left on.
 *
 * A host that offers no interrupt (the VisionFive 2's PLDA root complexes
 * take no message, and a function may have no INTx pin), or a kernel
 * built with CONFIG_GHCI_POLL, leaves the device polled.  A bus whose DMA
 * does not snoop the processor's caches, or a kernel built with
 * CONFIG_GHCI_HOST_UNCACHED, has the processor reach host memory through
 * uncached views.
 *
 * Values referenced: amaterrace-gpu d9252e4 docs/ghci.md (section 15.2,
 * the PCIe binding: the class codes, BAR0, BAR2, the interrupt and bus
 * mastering before any DMA).
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <drivers/generic/dma.h>
#include <drivers/pci/pci.h>
#include <drivers/pci/pci-ghci.h>
#include <kern/kcrt.h>
#include <kern/klog.h>
#include <kern/kmem.h>
#include <uapi/errno.h>

#include "ghci.h"

/* A kernel that names neither leaves the interrupt and the cached host view to the bus. */
#ifndef CONFIG_GHCI_POLL
#define CONFIG_GHCI_POLL		0
#endif
#ifndef CONFIG_GHCI_HOST_UNCACHED
#define CONFIG_GHCI_HOST_UNCACHED	0
#endif

/*
 * The functions offered to the driver: base class 03h with subclass 02h
 * (3D controller) or 80h (another display controller), any vendor.
 */
#define CLASS_3D			0x030200U
#define CLASS_DISPLAY_OTHER		0x038000U
#define CLASS_MASK			0xffff00U

/*
 * The GHCI boards whose endpoint may report another class: the KC705
 * board's Xilinx endpoint and Amaterrace's own IDs.  The magic still
 * decides.
 */
#define VENDOR_XILINX			0x10eeU
#define DEVICE_KC705			0x7024U
#define VENDOR_AMATERRACE		0x5441U
#define DEVICE_AMATERRACE		0x4748U

/* The register window's BAR and the local window's BAR. */
#define BAR_REGISTERS			0U
#define BAR_WINDOW			2U

/* The register window the driver maps: one page, of which GHCI 1.0 uses the first 1 KiB. */
#define REGISTER_BYTES			0x1000U

/* The largest local window the driver maps. */
#define WINDOW_BYTES_MAX		(1ULL << 30)

/* The tries at letting the handler go while it may still run on another processor, a millisecond apart. */
#define RELEASE_TRIES			1000U

/* The PCI command register and its bus master bit. */
#define PCI_COMMAND			0x04U
#define PCI_COMMAND_MASTER		0x0004U

/*
 * The bus record of one function: the function, its mappings and its
 * interrupt, with the device it carries.  It lives from the attach to
 * the detach.
 */
struct ghci_pci {
	struct ghci_device device;
	struct drv_pci_device *pci;
	struct drv_pci_mapping register_map;
	struct drv_pci_mapping window_map;
	struct drv_pci_irq irq;
	unsigned irq_count;
	void *irq_cookie;
};

static int ghci_pci_match(struct drv_pci_device *pci, const struct drv_pci_id *id);
static int ghci_pci_attach(struct drv_pci_device *pci, const struct drv_pci_id *id);
static int ghci_pci_detach(struct drv_pci_device *pci, unsigned flags);
static void ghci_pci_shutdown(struct drv_pci_device *pci);
static int map_registers(struct ghci_pci *bus);
static void map_window(struct ghci_pci *bus);
static void choose_host_view(struct ghci_pci *bus);
static int bus_master_found(struct ghci_pci *bus);
static int interrupt_take(struct ghci_device *device);
static int interrupt_release(struct ghci_device *device);
static int bus_master(struct ghci_device *device, int on);
static void release_bus(struct ghci_pci *bus);

/*
 * The operations the start calls on the bus; they live as long as the
 * kernel.
 */
static const struct ghci_bus_ops ghci_pci_bus_ops = {
	interrupt_take,
	interrupt_release,
	bus_master
};

/*
 * Registers the GHCI PCI driver for 3D and other display controllers and
 * for the known GHCI boards.
 *
 * The class match is the weakest, so a vendor's own driver of the same
 * function wins; the attach then keeps only a function whose BAR0 says
 * GHCI.
 */
int
drv_pci_ghci_driver_register(void)
{
	static const struct drv_pci_id identifiers[] = {
		{ VENDOR_XILINX, DEVICE_KC705, DRV_PCI_ANY_ID, DRV_PCI_ANY_ID, 0U, 0U, 1U },
		{ VENDOR_AMATERRACE, DEVICE_AMATERRACE, DRV_PCI_ANY_ID, DRV_PCI_ANY_ID, 0U, 0U, 1U },
		{ DRV_PCI_ANY_ID, DRV_PCI_ANY_ID, DRV_PCI_ANY_ID, DRV_PCI_ANY_ID, CLASS_3D, CLASS_MASK, 0U },
		{ DRV_PCI_ANY_ID, DRV_PCI_ANY_ID, DRV_PCI_ANY_ID, DRV_PCI_ANY_ID, CLASS_DISPLAY_OTHER, CLASS_MASK, 0U }
	};
	static struct drv_pci_driver driver = {
		"ghci",
		identifiers,
		sizeof(identifiers) / sizeof(identifiers[0]),
		ghci_pci_match,
		ghci_pci_attach,
		ghci_pci_detach,
		ghci_pci_shutdown,
		NULL,
		NULL,
		{ 0U, 0U, 0U, 0U }
	};
	int error;

	/* Hands probing and binding to PCI. */
	error = drv_pci_driver_register(&driver);
	if (error != 0)
		return error;

	/* Succeeded: matching functions are offered to the attach during PCI scanning. */
	return 0;
}

/* Matches a known board by its IDs, and any other function by class, below a vendor's driver. */
static int
ghci_pci_match(
	struct drv_pci_device *pci,
	const struct drv_pci_id *id)
{
	(void)pci;

	/* A known GHCI board is matched by its IDs. */
	if (id->driver_data != 0U)
		return DRV_PCI_MATCH_VENDOR;

	/* Succeeded: the weakest match that still binds. */
	return DRV_PCI_MATCH_CLASS;
}

/* Maps the function's windows and starts the device. */
static int
ghci_pci_attach(
	struct drv_pci_device *pci,
	const struct drv_pci_id *id)
{
	struct ghci_pci *bus;
	struct ghci_device *device;
	int error;

	(void)id;

	/* The bus record with the device, which lives until the detach. */
	bus = kern_calloc(1U, sizeof(*bus));
	if (bus == NULL)
		return ENOMEM;
	bus->pci = pci;
	device = &bus->device;
	device->bus = bus;
	device->bus_ops = &ghci_pci_bus_ops;
	device->pci_vendor = drv_pci_device_vendor(pci);
	device->pci_device = drv_pci_device_product(pci);

	/* The register window. */
	error = map_registers(bus);
	if (error != 0) {
		release_bus(bus);
		kern_free(bus);
		return error;
	}

	/* The local window when the function has one, how host memory is reached, and the bus master bit as found. */
	map_window(bus);
	choose_host_view(bus);
	device->bus_master_found = bus_master_found(bus);

	/* The record is the function's from here. */
	error = drv_pci_device_set_driver_data(pci, bus);
	if (error != 0) {
		release_bus(bus);
		kern_free(bus);
		return error;
	}

	/* Starts the device: identify, reset, capabilities, ring, interrupt, registration. */
	error = ghci_device_start(device);
	if (error != 0) {
		if (error != ENODEV)
			kern_logf("ghci: function not started (%d)\n", error);
		(void)drv_pci_device_set_driver_data(pci, NULL);

		/* A handler that may still run keeps the record it is given. */
		if (device->interrupt_taken) {
			kern_logf("ghci: %s: interrupt not let go; its record is kept\n", device->name);
			return error;
		}
		release_bus(bus);
		kern_free(bus);
		return error;
	}

	/* Succeeded: the function is a GHCI device. */
	return 0;
}

/* Stops the device and gives the function back; EBUSY while a session is open. */
static int
ghci_pci_detach(
	struct drv_pci_device *pci,
	unsigned flags)
{
	struct ghci_pci *bus;
	int error;

	(void)flags;

	/* A function without a started device has nothing to stop. */
	bus = drv_pci_device_driver_data(pci);
	if (bus == NULL)
		return 0;

	/* The device stops only when no session is open, whatever the flags say. */
	error = ghci_device_stop(&bus->device);
	if (error != 0)
		return error;

	/* The bus resources and the record go. */
	(void)drv_pci_device_set_driver_data(pci, NULL);
	release_bus(bus);
	kern_free(bus);

	/* Succeeded: the function is free. */
	return 0;
}

/* Stops the GPU's memory accesses before the machine restarts. */
static void
ghci_pci_shutdown(
	struct drv_pci_device *pci)
{
	struct ghci_pci *bus;

	/* A function without a started device does nothing. */
	bus = drv_pci_device_driver_data(pci);
	if (bus == NULL)
		return;

	/* No interrupt, and the processor reset; its quiesce ends before bus mastering goes. */
	ghci_write(&bus->device, GHCI_REG_IRQ_ENABLE, 0U);
	ghci_write(&bus->device, GHCI_REG_CTRL, GHCI_CTRL_RESET);
	(void)ghci_wait_register(&bus->device, GHCI_REG_CTRL, GHCI_CTRL_RESET, 0U, bus->device.reset_wait_ms);
	(void)drv_pci_device_set_bus_master(pci, false);
}

/* Claims and maps BAR0, the register window. */
static int
map_registers(
	struct ghci_pci *bus)
{
	struct drv_pci_bar bar;
	int error;

	/* BAR0 must be memory of at least one page. */
	error = drv_pci_device_bar(bus->pci, BAR_REGISTERS, &bar);
	if (error != 0)
		return ENODEV;
	if (bar.type != DRV_PCI_BAR_MEMORY32 && bar.type != DRV_PCI_BAR_MEMORY64)
		return ENODEV;
	if (bar.size < REGISTER_BYTES)
		return ENODEV;

	/* The registers decode from here. */
	error = drv_pci_device_enable_memory(bus->pci);
	if (error != 0)
		return error;

	/* No other driver maps the window. */
	error = drv_pci_device_claim_bar(bus->pci, BAR_REGISTERS);
	if (error != 0)
		return error;

	/* Maps the first page, uncached. */
	error = drv_pci_device_map_bar_region(bus->pci,
	    BAR_REGISTERS,
	    0U,
	    REGISTER_BYTES,
	    DRV_PCI_MAP_READ | DRV_PCI_MAP_WRITE | DRV_PCI_MAP_NOCACHE,
	    &bus->register_map);
	if (error != 0) {
		drv_pci_device_release_bar(bus->pci, BAR_REGISTERS);
		return error;
	}

	/* The whole BAR the device decodes, for the console's leaving of the display. */
	bus->device.bars[bus->device.bar_count].physical = bus->register_map.physical_address;
	bus->device.bars[bus->device.bar_count].bytes = bar.size;
	bus->device.bar_count++;

	/* Succeeded: the registers are reached through the mapping. */
	bus->device.registers = bus->register_map.address;
	bus->device.register_bytes = REGISTER_BYTES;
	return 0;
}

/*
 * Claims and maps BAR2, the local window, when there is one.
 *
 * The window is optional: a BAR2 the host left unplaced (it did not fit
 * the host's window) or could not map leaves the device without one.
 */
static void
map_window(
	struct ghci_pci *bus)
{
	struct drv_pci_bar bar;
	uint64_t bytes;
	int error;

	/* A function without a memory BAR2 has no window. */
	error = drv_pci_device_bar(bus->pci, BAR_WINDOW, &bar);
	if (error != 0)
		return;
	if (bar.type != DRV_PCI_BAR_MEMORY32 && bar.type != DRV_PCI_BAR_MEMORY64)
		return;
	if (bar.size == 0U)
		return;

	/* A BAR the host did not place decodes nothing. */
	if (bar.bus_address == 0U) {
		kern_logf("ghci: BAR2 (%llu MiB) not placed; no local window\n", (unsigned long long)(bar.size >> 20));
		return;
	}

	/* No other driver maps the window. */
	error = drv_pci_device_claim_bar(bus->pci, BAR_WINDOW);
	if (error != 0) {
		kern_logf("ghci: BAR2 not claimed (%d); no local window\n", error);
		return;
	}

	/* Maps it uncached, so that a write reaches the GPU's memory in program order. */
	bytes = bar.size;
	if (bytes > WINDOW_BYTES_MAX)
		bytes = WINDOW_BYTES_MAX;
	error = drv_pci_device_map_bar_region(bus->pci,
	    BAR_WINDOW,
	    0U,
	    (size_t)bytes,
	    DRV_PCI_MAP_READ | DRV_PCI_MAP_WRITE | DRV_PCI_MAP_NOCACHE,
	    &bus->window_map);
	if (error != 0) {
		kern_logf("ghci: BAR2 not mapped (%d); no local window\n", error);
		drv_pci_device_release_bar(bus->pci, BAR_WINDOW);
		return;
	}

	/* The whole BAR, not only the part mapped. */
	bus->device.bars[bus->device.bar_count].physical = bus->window_map.physical_address;
	bus->device.bars[bus->device.bar_count].bytes = bar.size;
	bus->device.bar_count++;

	/* Succeeded: the processor's view of the window and its physical place. */
	bus->device.window = bus->window_map.address;
	bus->device.window_physical = bus->window_map.physical_address;
	bus->device.window_bytes = bytes;
}

/*
 * Decides whether the processor reaches host memory through uncached
 * views: when the bus's DMA does not snoop the caches, or when the kernel
 * is built to.
 */
static void
choose_host_view(
	struct ghci_pci *bus)
{
	struct drv_dma_device *dma;
	int coherent;

	/* The bus's DMA: a function without one is taken as coherent, as on the PC. */
	coherent = 1;
	dma = drv_pci_device_dma(bus->pci);
	if (dma != NULL)
		coherent = drv_dma_device_is_coherent(dma);

	/* A coherent bus shares the direct map, unless the kernel says otherwise. */
	bus->device.host_uncached = 0;
	if (!coherent || CONFIG_GHCI_HOST_UNCACHED)
		bus->device.host_uncached = 1;

	/* Says which view the processor uses. */
	if (bus->device.host_uncached)
		kern_logf("ghci: host memory reached uncached (bus DMA %s)\n", coherent ? "coherent" : "not coherent");
}

/* Reports whether bus mastering is on as the driver found the function: 1 when it is. */
static int
bus_master_found(
	struct ghci_pci *bus)
{
	uint16_t command;
	int error;

	/* The command register. */
	error = drv_pci_device_config_read16(bus->pci, PCI_COMMAND, &command);
	if (error != 0)
		return 0;

	/* Succeeded: the bus master bit. */
	if ((command & PCI_COMMAND_MASTER) != 0U)
		return 1;
	return 0;
}

/*
 * Takes one interrupt, a message where the function offers one, and puts
 * the handler on it; a host that offers none, or a kernel built to poll,
 * leaves the device polled.
 */
static int
interrupt_take(
	struct ghci_device *device)
{
	struct ghci_pci *bus;
	int error;

	/* A kernel built to poll takes no interrupt. */
	bus = device->bus;
	device->polled = 0;
	if (CONFIG_GHCI_POLL) {
		kern_logf("ghci: %s: polled every %u ms (CONFIG_GHCI_POLL)\n", device->name, (unsigned)GHCI_POLL_MS);
		device->polled = 1;
		return 0;
	}

	/* One vector: MSI-X, MSI, or the INTx pin. */
	error = drv_pci_device_allocate_irqs(bus->pci,
	    DRV_PCI_IRQ_ALLOW_MSIX | DRV_PCI_IRQ_ALLOW_MSI | DRV_PCI_IRQ_ALLOW_INTX,
	    1U,
	    1U,
	    &bus->irq,
	    &bus->irq_count);
	if (error == ENOTSUP || error == ENODEV) {
		/* No message and no pin: the poll stands in for the interrupt. */
		kern_logf("ghci: %s: no interrupt offered (%d); polled every %u ms\n", device->name, error, (unsigned)GHCI_POLL_MS);
		bus->irq_count = 0U;
		device->polled = 1;
		return 0;
	}
	if (error != 0)
		return error;
	if (bus->irq_count != 1U) {
		(void)interrupt_release(device);
		return ENODEV;
	}

	/* The handler. */
	error = drv_pci_device_establish_irq(bus->pci, &bus->irq, ghci_cp_interrupt, device, "ghci", &bus->irq_cookie);
	if (error != 0) {
		(void)interrupt_release(device);
		return error;
	}

	/* Says when the GPU raises the INTx pin rather than a message. */
	if (bus->irq.type == DRV_PCI_IRQ_INTX)
		kern_logf("ghci: %s: interrupt on the INTx pin\n", device->name);

	/* Succeeded: the handler serves the GPU's interrupt. */
	return 0;
}

/*
 * Lets go of the handler and the interrupt.  A handler still running on
 * another processor is waited for a bounded time; returns 0, or EBUSY
 * with the handler kept, after which its argument must not be freed.
 */
static int
interrupt_release(
	struct ghci_device *device)
{
	struct ghci_pci *bus;
	unsigned tries;
	int error;

	/* The handler first, retried while it may still run. */
	bus = device->bus;
	error = 0;
	if (bus->irq_cookie != NULL) {
		for (tries = 0U; tries < RELEASE_TRIES; tries++) {
			error = drv_pci_device_disestablish_irq_checked(bus->pci, bus->irq_cookie);
			if (error != EBUSY)
				break;
			ghci_sleep_ms(1U);
		}
		if (error == EBUSY)
			return EBUSY;
		bus->irq_cookie = NULL;
	}

	/* Then the vector. */
	if (bus->irq_count != 0U) {
		drv_pci_device_free_irqs(bus->pci, &bus->irq, bus->irq_count);
		bus->irq_count = 0U;
	}

	/* Succeeded: no handler of the device is left. */
	return 0;
}

/* Turns bus mastering on or off. */
static int
bus_master(
	struct ghci_device *device,
	int on)
{
	struct ghci_pci *bus;
	int error;

	/* The function's command register. */
	bus = device->bus;
	error = drv_pci_device_set_bus_master(bus->pci, on != 0);
	if (error != 0) {
		kern_logf("ghci: %s: bus master %s failed (%d)\n", device->name, on ? "on" : "off", error);
		return error;
	}

	/* Succeeded: the GPU may (or may no longer) reach memory. */
	return 0;
}

/* Gives back whatever of the bus the attach took, in the reverse order. */
static void
release_bus(
	struct ghci_pci *bus)
{
	/* The interrupt, if the start left it. */
	(void)interrupt_release(&bus->device);

	/* The local window. */
	if (bus->device.window != NULL) {
		drv_pci_device_unmap_bar(bus->pci, &bus->window_map);
		drv_pci_device_release_bar(bus->pci, BAR_WINDOW);
		bus->device.window = NULL;
	}

	/* The register window. */
	if (bus->device.registers != NULL) {
		drv_pci_device_unmap_bar(bus->pci, &bus->register_map);
		drv_pci_device_release_bar(bus->pci, BAR_REGISTERS);
		bus->device.registers = NULL;
	}
}
