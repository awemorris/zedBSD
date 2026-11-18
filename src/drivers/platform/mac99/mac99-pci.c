/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The PowerPC Mac's PCI host bridges (UniNorth): the expansion bus's,
 * where the I/O controller and the USB controllers sit, and the internal
 * one, where a PowerBook's Ethernet controller (GMAC) and its ATA-6
 * controller sit.
 *
 * Configuration space is reached through two ports in the bridge's
 * register block: the address port takes which function and register,
 * and the data port then reads or writes it, little-endian.  On the
 * bridge's own bus a function is selected by its IDSEL line, one bit per
 * device from bit 11 up, so devices 0 to 10 cannot be reached; behind a
 * bridge the address is the usual type 1 one.  Memory BARs are at the
 * same address for the processor as on the bus.  A device's interrupt
 * is the interrupt controller's source the firmware's device tree routes
 * its pin to, which the hardware layer hands over as
 * "mac99.pci.interrupts" (the internal host's as
 * "mac99.pci.internal.interrupts"); behind a bridge the pin is swizzled
 * to the bridge's own, as the PCI-to-PCI bridge specification does.
 *
 * The expansion bus is segment 0 and its bus number 0.  The internal
 * host is segment 1, its bus numbered as the firmware's bus-range
 * starts, so the firmware's routes match its devices.  The GMAC's clock
 * is gated in the system controller (uni-n) and the GMAC does not answer
 * its configuration cycles while it is off, so the clock is turned on
 * before the internal bus is enumerated, and left on.
 *
 * Two words of the firmware's command line change this for a machine
 * whose devices stop the boot: "mac99.pciirq=off" gives no device an
 * interrupt, so each driver serves its device by polling, and
 * "mac99.pci.hide=" with device numbers in hexadecimal, separated by
 * commas (as the firmware's paths name them: 18 for usb@18), makes those
 * devices of the bridge's own bus read as absent, so no driver takes them.
 * "mac99.pci.internal=off" leaves the internal host alone.
 *
 * Values referenced: the Linux kernel's arch/powerpc/platforms/powermac/
 * pci.c (macrisc_cfg_map_bus: the address port at the block's start plus
 * 0x800000, the data port at plus 0xc00000, the address read back before
 * the data is touched, and the data port's offset the register's low
 * three bits), and QEMU's hw/pci-host/uninorth.c, which decodes the same;
 * the PCI-to-PCI Bridge Architecture Specification, Revision 1.2, section
 * 9.1 (the pin of a device behind a bridge as seen on the bridge's
 * primary side).
 */

#include "drivers/platform/mac99/mac99-pci.h"
#include <drivers/generic/dma.h>
#include <drivers/pci/pci.h>
#include <kern/clock.h>
#include <kern/device-io.h>
#include <kern/klog.h>
#include <kern/kcrt.h>
#include <kern/platform.h>
#include <kern/pmem.h>
#include <uapi/errno.h>

/* Where the ports are in the bridge's register block, and their size. */
#define MAC99_PCI_ADDRESS_PORT		0x00800000U
#define MAC99_PCI_DATA_PORT		0x00c00000U
#define MAC99_PCI_PORT_SIZE		0x1000U

/*
 * The devices the bridge's own bus can select by IDSEL: bits 11 to 30 of
 * the address (bit 31 asks the bridge for another form of address).
 */
#define MAC99_PCI_FIRST_IDSEL		11U
#define MAC99_PCI_LAST_IDSEL		30U

/* The configuration register that names a function's pin, and the pins, INTA# to INTD#. */
#define PCI_INTERRUPT_PIN		0x3dU
#define MAC99_PCI_PINS			4U

/* Where the routes start in the handoff's table, and the words of a route. */
#define MAC99_ROUTE_TABLE_FIRST		3U
#define MAC99_ROUTE_WORDS		4U

/* The command line's words that turn the interrupts off, hide devices and leave the internal host alone. */
#define MAC99_PCI_IRQ_OFF		"mac99.pciirq=off"
#define MAC99_PCI_HIDE			"mac99.pci.hide="
#define MAC99_PCI_INTERNAL_OFF		"mac99.pci.internal=off"

/* The hosts: the expansion bus's and the internal one, and their PCI segments. */
#define MAC99_HOST_EXPANSION		0U
#define MAC99_HOST_INTERNAL		1U
#define MAC99_HOSTS			2U

/*
 * The system controller's clock gates and the GMAC's gate among them,
 * the size of the register block mapped for it, and how long the GMAC
 * is given to start once its clock runs.
 *
 * Values referenced: FreeBSD sys/powerpc/powermac/uninorthvar.h
 * (release/14.3.0: UNIN_CLOCKCNTL, UNIN_CLOCKCNTL_GMAC); the Linux
 * kernel's arch/powerpc/include/asm/uninorth.h and
 * arch/powerpc/platforms/powermac/feature.c (v7.2: UNI_N_CLOCK_CNTL,
 * UNI_N_CLOCK_CNTL_GMAC, and the 20 us of core99_gmac_enable()).
 */
#define UNIN_CLOCK_CONTROL		0x20U
#define UNIN_CLOCK_CONTROL_GMAC		0x00000002U
#define UNIN_REGISTERS_SIZE		0x1000U
#define UNIN_GMAC_SETTLE_US		20U

/* The words of the hardware layer's "mac99.pci.internal": the register block and the bus's number. */
#define MAC99_INTERNAL_BASE		0U
#define MAC99_INTERNAL_BUS		1U

/* Marks a parameter the bus operations pass but this host does not need. */
#define UNUSED_PARAMETER(parameter)	((void)(parameter))

/*
 * One host bridge: its ports, the number of its own bus (where devices
 * are selected by IDSEL), the devices of that bus the command line hid
 * (one bit a device number), and the firmware's routes of its devices'
 * interrupts (NULL when there are none).  The bus operations get the
 * host as their context.
 */
struct mac99_pci_host {
	volatile uint32_t *address_port;
	volatile uint8_t *data_port;
	uint8_t first_bus;
	uint32_t hidden_devices;
	const uint32_t *routes;
};

/*
 * The host bridges, filled once by drv_mac99_pci_init() and kept for the
 * life of the system.  A host whose ports are NULL is not there, and
 * every configuration access to it fails.
 */
static struct mac99_pci_host hosts[MAC99_HOSTS];

/*
 * The DMA device the bus's functions share, made once with the root bus.
 */
static struct drv_dma_device *mac99_pci_dma;

/*
 * Whether the command line turned the devices' interrupts off, the
 * devices of the expansion bus's own bus it hid, one bit a device
 * number, and whether it left the internal host alone.  Read once by
 * drv_mac99_pci_init() and kept.
 */
static int interrupts_off;
static uint32_t hidden_devices;
static int internal_off;

static int config_read(void *context, const struct drv_pci_address *address, unsigned offset, unsigned width, uint32_t *result);
static int config_write(void *context, const struct drv_pci_address *address, unsigned offset, unsigned width, uint32_t value);
static int map_bar(void *context, struct drv_pci_device *device, const struct drv_pci_bar *bar, unsigned flags, struct drv_pci_mapping *mapping);
static void unmap_bar(void *context, struct drv_pci_mapping *mapping);
static int allocate_irqs(void *context, struct drv_pci_device *device, enum drv_pci_irq_type type, unsigned minimum, unsigned maximum, struct drv_pci_irq *irqs, unsigned *count);
static void free_irqs(void *context, struct drv_pci_device *device, struct drv_pci_irq *irqs, unsigned count);
static int select_register(struct mac99_pci_host *host, const struct drv_pci_address *address, unsigned offset, unsigned width, volatile uint8_t **data);
static int start_host(unsigned index, uint64_t base, uint8_t first_bus, const uint32_t *routes);
static void start_internal_host(void);
static void enable_gmac_clock(void);
static void wait_microseconds(unsigned microseconds);
static void read_options(void);
static const char *option_value(const char *line, const char *word);
static int hex_digit(char character);

/*
 * The bus operations of the host bridge, constant for the life of the
 * system.
 */
static const struct drv_pci_bus_ops mac99_pci_bus_ops = {
	.config_space_size = 0,
	.config_read = config_read,
	.config_write = config_write,
	.map_bar = map_bar,
	.unmap_bar = unmap_bar,
	.allocate_irqs = allocate_irqs,
	.free_irqs = free_irqs,
};

/*
 * Starts the PCI core and enumerates the buses behind the host bridges.
 */
int
drv_mac99_pci_init(
	void)
{
	const struct drv_dma_constraints constraints = {
		.address_bits = 32,
		.max_segment_size = 16U * 1024U * 1024U,
		.segment_boundary = 0,
		.coherent = 1};
	const uint64_t *base;
	const uint32_t *routes;
	int error;

	/* Brings the PCI core into service, even when no host follows. */
	error = drv_pci_init();
	if (error != 0 && error != EALREADY)
		return error;

	/* Finds the expansion bus's register block; a board without one has nothing to do. */
	base = kern_boot_handoff("mac99.pci.base");
	if (base == NULL)
		return ENODEV;

	/* What the command line asks of the buses. */
	read_options();

	/* Describes what the buses can address for DMA: all 32 bits, coherent. */
	error = drv_dma_device_create(&constraints, &mac99_pci_dma);
	if (error != 0)
		return error;

	/* Enumerates the expansion bus with the firmware's routes, the devices the command line hid left out. */
	routes = kern_boot_handoff("mac99.pci.interrupts");
	hosts[MAC99_HOST_EXPANSION].hidden_devices = hidden_devices;
	error = start_host(MAC99_HOST_EXPANSION, *base, 0, routes);
	if (error != 0)
		return error;

	/* Enumerates the internal bus, whose absence the boot goes on without. */
	start_internal_host();

	/* Lists what the enumeration found. */
	drv_pci_dump();

	/* Succeeded: the PCI core owns the hosts' trees. */
	return 0;
}

/*
 * Maps a host bridge's ports, makes its root bus (segment index, numbered
 * first_bus) and enumerates the bus and the buses behind its bridges.
 */
static int
start_host(
	unsigned index,
	uint64_t base,
	uint8_t first_bus,
	const uint32_t *routes)
{
	struct mac99_pci_host *host;
	struct drv_pci_bus *root;
	void *window;
	int error;

	/* The host's state, its bus's number and its devices' routes. */
	host = &hosts[index];
	host->first_bus = first_bus;
	host->routes = routes;

	/* Maps the address port. */
	error = kern_device_map(base + MAC99_PCI_ADDRESS_PORT, MAC99_PCI_PORT_SIZE, KERN_DEVICE_UNCACHED, &window);
	if (error != 0) {
		kern_logf("pci: mac99 host %u address port unavailable (%d)\n", index, error);
		return error;
	}
	host->address_port = window;

	/* Maps the data port. */
	error = kern_device_map(base + MAC99_PCI_DATA_PORT, MAC99_PCI_PORT_SIZE, KERN_DEVICE_UNCACHED, &window);
	if (error != 0) {
		kern_logf("pci: mac99 host %u data port unavailable (%d)\n", index, error);
		return error;
	}
	host->data_port = window;

	/* Makes the root bus, the host its operations' context. */
	error = drv_pci_bus_create_root((uint16_t)index, first_bus, &mac99_pci_bus_ops, host, mac99_pci_dma, &root);
	if (error != 0) {
		kern_logf("pci: mac99 host %u root bus unavailable (%d)\n", index, error);
		return error;
	}

	/* Enumerates the bus and the buses behind its bridges. */
	kern_logf("pci: mac99 host %u at %llx, bus %u\n", index, (unsigned long long)base, (unsigned)first_bus);
	error = drv_pci_bus_scan_tree(root);
	if (error != 0) {
		kern_logf("pci: mac99 host %u enumeration failed (%d)\n", index, error);
		return error;
	}

	/* Succeeded: the PCI core owns the host's tree. */
	return 0;
}

/*
 * Enumerates the internal host's bus with the GMAC's clock on, when the
 * firmware named the host and the command line did not leave it alone.
 */
static void
start_internal_host(
	void)
{
	const uint64_t *internal;
	const uint32_t *routes;
	int error;

	/* The command line may leave the internal host alone. */
	if (internal_off) {
		kern_logf("pci: mac99 internal host left alone by the command line\n");
		return;
	}

	/* A board without the internal host has nothing more to enumerate. */
	internal = kern_boot_handoff("mac99.pci.internal");
	if (internal == NULL)
		return;

	/* Lets the GMAC answer its configuration cycles. */
	enable_gmac_clock();

	/* Enumerates the bus with its routes; a failure leaves the expansion bus in service. */
	routes = kern_boot_handoff("mac99.pci.internal.interrupts");
	error = start_host(MAC99_HOST_INTERNAL,
			   internal[MAC99_INTERNAL_BASE],
			   (uint8_t)internal[MAC99_INTERNAL_BUS],
			   routes);
	if (error != 0)
		kern_logf("pci: mac99 internal host unavailable (%d)\n", error);
}

/*
 * Turns the GMAC's clock on in the system controller, and gives the
 * GMAC time to start.  Without the system controller's registers the
 * clock is left as the firmware left it.
 *
 * Procedure referenced: FreeBSD sys/powerpc/powermac/uninorth.c
 * (release/14.3.0: unin_enable_gmac(), the gate's bit set in the clock
 * control register when the firmware's tree names a GMAC).
 */
static void
enable_gmac_clock(
	void)
{
	const uint64_t *base;
	volatile uint8_t *registers;
	void *window;
	uint32_t gates;
	int error;

	/* Finds the system controller; without it nothing can be turned on. */
	base = kern_boot_handoff("mac99.uni-n.base");
	if (base == NULL) {
		kern_logf("pci: mac99 has no system controller; GMAC clock as the firmware left it\n");
		return;
	}

	/* Maps its registers. */
	error = kern_device_map(*base, UNIN_REGISTERS_SIZE, KERN_DEVICE_UNCACHED, &window);
	if (error != 0) {
		kern_logf("pci: mac99 system controller unavailable (%d)\n", error);
		return;
	}
	registers = window;

	/* Sets the GMAC's gate, big-endian like the rest of the controller, and reads it back so the write lands. */
	gates = kern_mmio_read32(registers + UNIN_CLOCK_CONTROL);
	kern_mmio_write32(registers + UNIN_CLOCK_CONTROL, gates | UNIN_CLOCK_CONTROL_GMAC);
	gates = kern_mmio_read32(registers + UNIN_CLOCK_CONTROL);

	/* Gives the GMAC time to start before its configuration space is read. */
	wait_microseconds(UNIN_GMAC_SETTLE_US);
	kern_logf("pci: mac99 GMAC clock on (clock control %08x)\n", gates);

	/* Releases the window; the gate stays as set. */
	(void)kern_device_unmap(window, UNIN_REGISTERS_SIZE);
}

/*
 * Spins for at least the given time on the monotonic counter; the boot
 * has no scheduler to sleep on yet.  Without a counter it returns at once.
 */
static void
wait_microseconds(
	unsigned microseconds)
{
	uint64_t start;
	uint64_t now;
	uint64_t frequency;
	uint64_t duration;
	bool counted;

	/* The start and the length of the wait in counter ticks. */
	counted = kern_rtc_read_counter(&start, &frequency);
	if (!counted || frequency == 0)
		return;
	duration = frequency * microseconds / 1000000U + 1U;

	/* Spins until the counter has moved that far. */
	for (;;) {
		counted = kern_rtc_read_counter(&now, &frequency);
		if (!counted)
			return;
		if (now - start >= duration)
			return;
	}
}

/*
 * Reads a configuration register.  A function the bridge cannot select
 * reads as all ones, as an absent one does.
 */
static int
config_read(
	void *context,
	const struct drv_pci_address *address,
	unsigned offset,
	unsigned width,
	uint32_t *result)
{
	volatile uint8_t *data;
	int error;

	/* Requires somewhere to put the value. */
	if (result == NULL)
		return EINVAL;

	/* Selects the register; an unreachable function reads as absent. */
	error = select_register(context, address, offset, width, &data);
	if (error == ENODEV) {
		*result = 0xffffffffU;
		return 0;
	}
	if (error != 0)
		return error;

	/* Reads the register's bytes, little-endian on the bus. */
	switch (width) {
	case 1:
		*result = *data;
		break;
	case 2:
		*result = __builtin_bswap16(*(volatile uint16_t *)(volatile void *)data);
		break;
	default:
		*result = __builtin_bswap32(*(volatile uint32_t *)(volatile void *)data);
		break;
	}

	/* Succeeded: the register's value. */
	return 0;
}

/*
 * Writes a configuration register.  A write to a function the bridge
 * cannot select goes nowhere, as one to an absent function does.
 */
static int
config_write(
	void *context,
	const struct drv_pci_address *address,
	unsigned offset,
	unsigned width,
	uint32_t value)
{
	volatile uint8_t *data;
	int error;

	/* Selects the register; an unreachable function takes nothing. */
	error = select_register(context, address, offset, width, &data);
	if (error == ENODEV)
		return 0;
	if (error != 0)
		return error;

	/* Writes the register's bytes, little-endian on the bus. */
	switch (width) {
	case 1:
		*data = (uint8_t)value;
		break;
	case 2:
		*(volatile uint16_t *)(volatile void *)data = __builtin_bswap16((uint16_t)value);
		break;
	default:
		*(volatile uint32_t *)(volatile void *)data = __builtin_bswap32(value);
		break;
	}

	/* Orders the write before whatever follows. */
	__asm__ volatile("sync" : : : "memory");

	/* Succeeded: the register holds the value. */
	return 0;
}

/*
 * Maps a memory BAR for its driver; the processor reaches it at its bus
 * address.
 */
static int
map_bar(
	void *context,
	struct drv_pci_device *device,
	const struct drv_pci_bar *bar,
	unsigned flags,
	struct drv_pci_mapping *mapping)
{
	void *address;
	int error;

	UNUSED_PARAMETER(context);
	UNUSED_PARAMETER(device);
	UNUSED_PARAMETER(flags);

	/* Refuses an I/O BAR, an empty one and missing storage. */
	if (bar == NULL || mapping == NULL)
		return EINVAL;
	if (bar->type == DRV_PCI_BAR_IO ||
	    bar->type == DRV_PCI_BAR_NONE ||
	    bar->size == 0)
		return EINVAL;

	/* Maps the BAR as uncached device memory. */
	error = kern_device_map(bar->bus_address, (size_t)bar->size, KERN_DEVICE_UNCACHED, &address);
	if (error != 0)
		return error;

	/* Describes the mapping; the first private word marks it as owned here. */
	mapping->address = address;
	mapping->physical_address = bar->bus_address;
	mapping->size = (size_t)bar->size;
	mapping->type = bar->type;
	mapping->private_data[0] = 1U;
	mapping->private_data[1] = 0;

	/* Succeeded: the driver reaches the BAR at address. */
	return 0;
}

/* Removes a BAR mapping made by map_bar(). */
static void
unmap_bar(
	void *context,
	struct drv_pci_mapping *mapping)
{
	UNUSED_PARAMETER(context);

	/* Ignores storage that holds no mapping of ours. */
	if (mapping == NULL || mapping->private_data[0] != 1U)
		return;

	/* Releases the window. */
	(void)kern_device_unmap(mapping->address, mapping->size);
	kern_memset(mapping, 0, sizeof(*mapping));
}

/*
 * Gives a device its legacy interrupt: the controller's source the
 * firmware routes its pin to.  MSI is not offered: the board has no
 * message receiver.
 */
static int
allocate_irqs(
	void *context,
	struct drv_pci_device *device,
	enum drv_pci_irq_type type,
	unsigned minimum,
	unsigned maximum,
	struct drv_pci_irq *irqs,
	unsigned *count)
{
	struct mac99_pci_host *host;
	const uint32_t *table;
	const uint32_t *route;
	struct drv_pci_address address;
	struct drv_pci_device *walk;
	struct drv_pci_bus *bus;
	struct drv_pci_bus *above;
	uint32_t key;
	unsigned index;
	uint8_t pin;
	int error;

	/* Refuses missing arguments, more than one line, and anything but the pins. */
	if (device == NULL || irqs == NULL || count == NULL)
		return EINVAL;
	*count = 0;
	if (minimum != 1U || maximum < minimum)
		return ENOTSUP;
	if (type != DRV_PCI_IRQ_INTX)
		return ENOTSUP;

	/* The command line may have turned every interrupt off. */
	if (interrupts_off)
		return ENODEV;

	/* Reads which pin the device uses; zero means none. */
	error = drv_pci_device_config_read8(device, PCI_INTERRUPT_PIN, &pin);
	if (error != 0)
		return ENODEV;
	if (pin == 0 || pin > MAC99_PCI_PINS)
		return ENODEV;

	/* Swizzles the pin by the device number at each bus below the root bus. */
	walk = device;
	for (;;) {
		/* Stops at the root bus, which the firmware's routes describe. */
		bus = drv_pci_device_bus(walk);
		above = drv_pci_bus_parent(bus);
		if (above == NULL)
			break;

		/* Rotates the pin by the device's slot and moves to the bridge above. */
		drv_pci_device_address(walk, &address);
		pin = (uint8_t)(((pin - 1U + address.device) % MAC99_PCI_PINS) + 1U);
		walk = drv_pci_bus_bridge(bus);
	}

	/* The firmware's routes of the host's devices; without them no device has a line. */
	host = context;
	table = host->routes;
	if (table == NULL)
		return ENODEV;

	/* The root-bus function's address, under the routes' mask. */
	drv_pci_device_address(walk, &address);
	key = ((uint32_t)address.bus << 16) |
	      ((uint32_t)address.device << 11) |
	      ((uint32_t)address.function << 8);
	key &= table[1];

	/* Takes the first route of the address whose pin matches, or that takes any pin. */
	for (index = 0; index < table[0]; index++) {
		route = &table[MAC99_ROUTE_TABLE_FIRST + index * MAC99_ROUTE_WORDS];
		if (route[0] != key)
			continue;
		if (route[1] != 0 && route[1] != ((uint32_t)pin & table[2]))
			continue;

		/* Succeeded: the device's line is the routed source. */
		irqs[0].type = DRV_PCI_IRQ_INTX;
		irqs[0].index = 0;
		irqs[0].vector = route[2];
		irqs[0].private_data[0] = 0;
		irqs[0].private_data[1] = 0;
		*count = 1U;
		return 0;
	}

	/* Reports a pin the firmware does not route. */
	return ENODEV;
}

/* Releases interrupts; none were given. */
static void
free_irqs(
	void *context,
	struct drv_pci_device *device,
	struct drv_pci_irq *irqs,
	unsigned count)
{
	UNUSED_PARAMETER(context);
	UNUSED_PARAMETER(device);
	UNUSED_PARAMETER(irqs);
	UNUSED_PARAMETER(count);
}

/*
 * Points the address port at a register and reports where its bytes are
 * in the data port.  Returns ENODEV for a function the bridge cannot
 * select, EINVAL for a malformed access.
 */
static int
select_register(
	struct mac99_pci_host *host,
	const struct drv_pci_address *address,
	unsigned offset,
	unsigned width,
	volatile uint8_t **data)
{
	uint32_t selector;

	/* Refuses a malformed access, and one before the ports are mapped. */
	if (host == NULL || address == NULL || data == NULL)
		return EINVAL;
	if (width != 1U && width != 2U && width != 4U)
		return EINVAL;
	if (offset > 0xffU || (offset & (width - 1U)) != 0)
		return EINVAL;
	if (address->device > 31U || address->function > 7U)
		return EINVAL;
	if (host->address_port == NULL || host->data_port == NULL)
		return ENODEV;

	/*
	 * On the bridge's own bus, the function's IDSEL bit, its number and
	 * the register; behind a bridge, a type 1 address.
	 */
	if (address->bus == host->first_bus) {
		if (address->device < MAC99_PCI_FIRST_IDSEL ||
		    address->device > MAC99_PCI_LAST_IDSEL)
			return ENODEV;
		if ((host->hidden_devices & (1U << address->device)) != 0)
			return ENODEV;
		selector = (1U << address->device) |
			   ((uint32_t)address->function << 8) |
			   (offset & 0xfcU);
	} else {
		selector = ((uint32_t)address->bus << 16) |
			   ((uint32_t)address->device << 11) |
			   ((uint32_t)address->function << 8) |
			   (offset & 0xfcU) | 1U;
	}

	/* Writes the address little-endian and reads it back, which the bridge needs before the data. */
	*host->address_port = __builtin_bswap32(selector);
	__asm__ volatile("sync" : : : "memory");
	(void)*host->address_port;

	/* Succeeded: the register's bytes sit at its low three bits into the data port. */
	*data = host->data_port + (offset & 0x07U);
	return 0;
}

/*
 * Reads the command line's words for the buses: interrupts off, the
 * internal host left alone, and the devices to hide.  Says what it took.
 */
static void
read_options(
	void)
{
	const char *line;
	const char *value;
	unsigned device;
	int digit;

	/* Without a command line nothing changes. */
	line = kern_boot_handoff("boot.command-line");
	if (line == NULL)
		return;

	/* The interrupts. */
	if (option_value(line, MAC99_PCI_IRQ_OFF) != NULL) {
		interrupts_off = 1;
		kern_logf("pci: mac99 device interrupts off by the command line; drivers poll\n");
	}

	/* The internal host. */
	if (option_value(line, MAC99_PCI_INTERNAL_OFF) != NULL)
		internal_off = 1;

	/* The hidden devices: hexadecimal numbers separated by commas, to the word's end. */
	value = option_value(line, MAC99_PCI_HIDE);
	if (value == NULL)
		return;
	device = 0;
	for (;;) {
		digit = hex_digit(*value);
		if (digit >= 0) {
			device = device * 16U + (unsigned)digit;
			value++;
			continue;
		}
		if (device <= 31U)
			hidden_devices |= 1U << device;
		device = 0;
		if (*value != ',')
			break;
		value++;
	}
	kern_logf("pci: mac99 devices hidden by the command line: mask %08x\n", hidden_devices);
}

/*
 * Finds a word that starts one of the command line's space-separated
 * words, and reports where the rest of that word starts; NULL when no
 * word starts so.
 */
static const char *
option_value(
	const char *line,
	const char *word)
{
	size_t start;
	size_t index;

	/* Each word of the line, its start compared with the word asked for. */
	start = 0;
	while (line[start] != '\0') {
		while (line[start] == ' ')
			start++;
		for (index = 0; word[index] != '\0' && line[start + index] == word[index]; index++)
			continue;
		if (word[index] == '\0')
			return &line[start + index];
		while (line[start] != '\0' && line[start] != ' ')
			start++;
	}

	/* No word starts so. */
	return NULL;
}

/* Reports a hexadecimal digit's value, or -1 for anything else. */
static int
hex_digit(
	char character)
{
	/* The three ranges of digits. */
	if (character >= '0' && character <= '9')
		return character - '0';
	if (character >= 'a' && character <= 'f')
		return character - 'a' + 10;
	if (character >= 'A' && character <= 'F')
		return character - 'A' + 10;

	/* Not a digit. */
	return -1;
}
