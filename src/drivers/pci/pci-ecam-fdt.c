/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * A generic ECAM PCI host described by a device tree.
 *
 * Every function's configuration space is a 4 KiB page of one memory
 * window, so an access is a plain load or store and needs no lock.  The
 * firmware of the emulators' virt board leaves the BARs at zero, and the
 * PCI core reads BARs but does not place them, so this driver places the
 * memory BARs of the root bus's endpoints in the node's 32-bit memory
 * window before the core enumerates the bus.  Bridges are left alone: the
 * board puts its devices on the root bus.  Only the legacy pins are
 * offered; each is looked up in the node's interrupt map.
 */

#include <drivers/pci/pci-ecam-fdt.h>
#include <drivers/generic/dma.h>
#include <kern/device-io.h>
#include <kern/kcrt.h>
#include <kern/klog.h>
#include <kern/kmem.h>
#include <kern/pmem.h>
#include <uapi/errno.h>

/*
 * Values referenced: PCI Express Base Specification, Revision 4.0,
 * section 7.2.2 (the enhanced configuration access layout: bus, device
 * and function above the 4 KiB function space).
 */
#define ECAM_BUS_SHIFT			20U
#define ECAM_DEVICE_SHIFT		15U
#define ECAM_FUNCTION_SHIFT		12U
#define ECAM_BUS_SPAN			(UINT64_C(1) << ECAM_BUS_SHIFT)
#define ECAM_FUNCTION_SPACE		4096U

/* Standard configuration header offsets and bits. */
#define PCI_VENDOR_ID			0x00U
#define PCI_COMMAND			0x04U
#define PCI_HEADER_TYPE			0x0eU
#define PCI_BAR0			0x10U
#define PCI_INTERRUPT_PIN		0x3dU
#define PCI_HEADER_LAYOUT		0x7fU
#define PCI_HEADER_MULTIFUNCTION	0x80U
#define PCI_HEADER_ENDPOINT		0x00U
#define PCI_BAR_IO			0x00000001U
#define PCI_BAR_TYPE			0x00000006U
#define PCI_BAR_TYPE_64			0x00000004U
#define PCI_BAR_MEMORY_MASK		0xfffffff0U
#define PCI_ENDPOINT_BARS		6U
#define PCI_INTX_PINS			4U

/*
 * Values referenced: IEEE Std 1275-1994 PCI Bus Binding, revision 2.1,
 * section 2.2.1 (the three cells of a PCI address and the space code in
 * the first one) and the Devicetree Specification v0.4, section 2.4.3
 * (interrupt-map and interrupt-map-mask).
 */
#define FDT_PCI_SPACE_SHIFT		24U
#define FDT_PCI_SPACE_MASK		3U
#define FDT_PCI_SPACE_MEMORY32		2U
#define FDT_PCI_ADDRESS_CELLS		3U
#define FDT_PCI_ADDRESS_LOW_CELL	1U
#define FDT_PCI_DEVFN_SHIFT		8U
#define FDT_PCI_BUS_SHIFT		16U

/* What devices behind the host can reach with DMA: memory below 4 GiB. */
#define ECAM_DMA_ADDRESS_BITS		32U
#define ECAM_DMA_MAX_SEGMENT		(16U * 1024U * 1024U)

/* The space below 4 GiB, where the memory window must lie. */
#define ECAM_32BIT_LIMIT		(UINT64_C(1) << 32)

/* Marks a parameter that an operation of the PCI core passes but this host does not need. */
#define UNUSED_PARAMETER(parameter)	((void)(parameter))

/*
 * One host the driver has started.
 *
 * It lives from a successful start for as long as the kernel runs: the PCI
 * core keeps it as the host of the root bus.  Window next is where the next
 * BAR goes; everything below it in the memory window is assigned.
 */
struct drv_pci_ecam_fdt {
	struct drv_pci_ecam_fdt_config config;
	volatile uint8_t *window;
	unsigned bus_count;
	struct drv_dma_device *dma;
	struct drv_pci_bus *root;
	uint64_t window_next;
};

static int describe_memory(const struct drv_fdt *fdt, uint32_t node, struct drv_pci_ecam_fdt_config *config);
static int describe_interrupts(const struct drv_fdt *fdt, uint32_t node, struct drv_pci_ecam_fdt_config *config);
static int config_address(struct drv_pci_ecam_fdt *host, const struct drv_pci_address *address, unsigned offset, unsigned width, volatile uint8_t **target);
static int host_config_read(void *context, const struct drv_pci_address *address, unsigned offset, unsigned width, uint32_t *value);
static int host_config_write(void *context, const struct drv_pci_address *address, unsigned offset, unsigned width, uint32_t value);
static int assign_resources(struct drv_pci_ecam_fdt *host);
static int assign_function(struct drv_pci_ecam_fdt *host, const struct drv_pci_address *address, bool *multifunction);
static int assign_bar(struct drv_pci_ecam_fdt *host, const struct drv_pci_address *address, unsigned index, bool *wide);
static int host_map_bar(void *context, struct drv_pci_device *device, const struct drv_pci_bar *bar, unsigned flags, struct drv_pci_mapping *mapping);
static void host_unmap_bar(void *context, struct drv_pci_mapping *mapping);
static int host_allocate_irqs(void *context, struct drv_pci_device *device, enum drv_pci_irq_type type, unsigned minimum, unsigned maximum, struct drv_pci_irq *irqs, unsigned *count);
static void host_free_irqs(void *context, struct drv_pci_device *device, struct drv_pci_irq *irqs, unsigned count);

/*
 * The operations the PCI core calls on the root bus and every bus below it.
 *
 * The table is constant; the host each call works on arrives as its context.
 */
static const struct drv_pci_bus_ops ecam_fdt_bus_ops = {
	.config_space_size = ECAM_FUNCTION_SPACE,
	.config_read = host_config_read,
	.config_write = host_config_write,
	.map_bar = host_map_bar,
	.unmap_bar = host_unmap_bar,
	.allocate_irqs = host_allocate_irqs,
	.free_irqs = host_free_irqs
};

/*
 * Describes a host from its device tree node.
 *
 * Reports ENODEV for a node the tree marks as not present.
 */
int
drv_pci_ecam_fdt_describe(
	const struct drv_fdt *fdt,
	uint32_t node,
	struct drv_pci_ecam_fdt_config *config)
{
	const uint8_t *value;
	uint32_t length;
	bool enabled;
	int error;

	/* Refuses missing arguments. */
	if (fdt == NULL || config == NULL)
		return EINVAL;

	/* Leaves a disabled host alone. */
	enabled = drv_fdt_node_enabled(fdt, node);
	if (!enabled)
		return ENODEV;

	/* Starts with an empty configuration: bus 0, no window and no route. */
	kern_memset(config, 0, sizeof(*config));

	/* Reads where the configuration window is. */
	error = drv_fdt_reg(fdt, node, 0, &config->config_base, &config->config_size);
	if (error != 0)
		return error;

	/* Reads the first bus the window starts with; without bus-range it is bus 0. */
	error = drv_fdt_property(fdt, node, "bus-range", &value, &length);
	if (error == 0 && length >= 8U)
		config->first_bus = (unsigned)drv_fdt_cells_value(value, 0, 1U);
	if (config->first_bus > 0xffU)
		return EINVAL;

	/* Notes whether the host's DMA snoops the CPU caches. */
	error = drv_fdt_property(fdt, node, "dma-coherent", &value, &length);
	config->coherent = error == 0;

	/* Reads the window the CPU reaches device BARs through. */
	error = describe_memory(fdt, node, config);
	if (error != 0)
		return error;

	/* Reads where the legacy pins of each slot arrive. */
	error = describe_interrupts(fdt, node, config);
	if (error != 0)
		return error;

	/* Succeeded: the configuration describes the host. */
	return 0;
}

/*
 * Maps a host's configuration window and places the root bus's BARs.
 *
 * On success the host is ready for drv_pci_ecam_fdt_publish().
 */
int
drv_pci_ecam_fdt_start(
	const struct drv_pci_ecam_fdt_config *config,
	struct drv_pci_ecam_fdt **result)
{
	struct drv_pci_ecam_fdt *host;
	uint64_t buses;
	void *window;
	int error;

	/* Refuses missing arguments and a window smaller than one bus. */
	if (config == NULL || result == NULL)
		return EINVAL;
	buses = config->config_size / ECAM_BUS_SPAN;
	if (buses == 0)
		return EINVAL;
	if (buses > 256U - config->first_bus)
		buses = 256U - config->first_bus;

	/* Allocates the host record. */
	host = kern_malloc(sizeof(*host));
	if (host == NULL)
		return ENOMEM;

	/* Starts the record with the configuration and the memory window still empty. */
	kern_memset(host, 0, sizeof(*host));
	host->config = *config;
	host->bus_count = (unsigned)buses;
	host->window_next = config->memory_pci_base;

	/* Maps the configuration space of every bus the window holds. */
	error = kern_device_map(config->config_base, (size_t)(buses * ECAM_BUS_SPAN), KERN_DEVICE_UNCACHED, &window);
	if (error != 0) {
		kern_free(host);
		return error;
	}

	/* Keeps the mapping and reports where the host is. */
	host->window = window;
	kern_logf("pci: ECAM host at %llx, buses %u-%u, memory %llx (%llu MiB)\n",
		  (unsigned long long)config->config_base,
		  config->first_bus,
		  config->first_bus + host->bus_count - 1U,
		  (unsigned long long)config->memory_cpu_base,
		  (unsigned long long)(config->memory_size >> 20));

	/* Places the BARs of the root bus's endpoints. */
	error = assign_resources(host);
	if (error != 0) {
		(void)kern_device_unmap(window, (size_t)(buses * ECAM_BUS_SPAN));
		kern_free(host);
		return error;
	}

	/* Succeeded: the host is ready to be published. */
	*result = host;
	return 0;
}

/*
 * Hands a started host to the PCI core, which enumerates and binds its tree.
 */
int
drv_pci_ecam_fdt_publish(
	struct drv_pci_ecam_fdt *host)
{
	struct drv_dma_constraints constraints;
	int error;

	/* Refuses a missing host and a second publication. */
	if (host == NULL)
		return EINVAL;
	if (host->root != NULL)
		return EBUSY;

	/*
	 * Keeps DMA below 4 GiB, which does not depend on each device's
	 * 64-bit addressing.  There is no translation between the bus and
	 * memory.
	 */
	kern_memset(&constraints, 0, sizeof(constraints));
	constraints.address_bits = ECAM_DMA_ADDRESS_BITS;
	constraints.max_segment_size = ECAM_DMA_MAX_SEGMENT;
	constraints.segment_boundary = 0;
	constraints.coherent = host->config.coherent ? 1 : 0;
	error = drv_dma_device_create(&constraints, &host->dma);
	if (error != 0)
		return error;

	/* Creates the root bus. */
	error = drv_pci_bus_create_root(0, (uint8_t)host->config.first_bus, &ecam_fdt_bus_ops, host, host->dma, &host->root);
	if (error != 0) {
		(void)drv_dma_device_destroy(host->dma);
		host->dma = NULL;
		return error;
	}

	/* Enumerates the tree, which attaches the registered drivers. */
	error = drv_pci_bus_scan_tree(host->root);
	if (error != 0)
		return error;

	/* Succeeded: the PCI core owns the tree. */
	return 0;
}

/*
 * Reads the first 32-bit memory window from the node's ranges.
 *
 * A 64-bit window and the I/O window are not used: every BAR the board's
 * devices have fits below 4 GiB.
 */
static int
describe_memory(
	const struct drv_fdt *fdt,
	uint32_t node,
	struct drv_pci_ecam_fdt_config *config)
{
	const uint8_t *value;
	uint32_t length;
	uint32_t parent;
	uint32_t parent_cells;
	uint32_t size_cells;
	uint32_t entry_cells;
	uint32_t entry;
	uint32_t space;
	uint64_t parent_address;
	uint64_t end;
	bool found;
	int error;

	/* Reads the ranges and the widths of the parent's addresses and the sizes. */
	error = drv_fdt_property(fdt, node, "ranges", &value, &length);
	if (error != 0)
		return error;
	error = drv_fdt_parent(fdt, node, &parent);
	if (error != 0)
		return error;
	parent_cells = drv_fdt_node_cells(fdt, parent, "#address-cells", 2U);
	size_cells = drv_fdt_node_cells(fdt, node, "#size-cells", 2U);
	if (parent_cells > 2U || size_cells > 2U)
		return ERANGE;

	/* Takes the first 32-bit memory window. */
	found = false;
	entry_cells = FDT_PCI_ADDRESS_CELLS + parent_cells + size_cells;
	for (entry = 0; (entry + 1U) * entry_cells * 4U <= length; entry++) {
		/* Skips anything but 32-bit memory space. */
		space = (uint32_t)drv_fdt_cells_value(value, entry * entry_cells, 1U);
		space = (space >> FDT_PCI_SPACE_SHIFT) & FDT_PCI_SPACE_MASK;
		if (space != FDT_PCI_SPACE_MEMORY32)
			continue;

		/* Decodes the bus address, the parent-bus address and the size. */
		config->memory_pci_base = drv_fdt_cells_value(value, entry * entry_cells + FDT_PCI_ADDRESS_LOW_CELL, 2U);
		parent_address = drv_fdt_cells_value(value, entry * entry_cells + FDT_PCI_ADDRESS_CELLS, parent_cells);
		config->memory_size = drv_fdt_cells_value(value, entry * entry_cells + FDT_PCI_ADDRESS_CELLS + parent_cells, size_cells);

		/* Requires a non-empty window that 32-bit BARs can address. */
		end = config->memory_pci_base + config->memory_size;
		if (config->memory_size == 0 ||
		    end < config->memory_pci_base ||
		    end > ECAM_32BIT_LIMIT)
			return ERANGE;

		/* Moves the parent-bus address into the CPU's space. */
		error = drv_fdt_translate(fdt, node, parent_address, &config->memory_cpu_base);
		if (error != 0)
			return error;

		/* The window is recorded; later ones are not used. */
		found = true;
		break;
	}

	/* Refuses a host without a 32-bit memory window. */
	if (!found)
		return ENOENT;

	/* Succeeded: the window is recorded. */
	return 0;
}

/*
 * Reads the interrupt map: which interrupt each slot's pins arrive on.
 *
 * Only an interrupt controller with a one-cell specifier, the source
 * number, is understood; the RISC-V PLIC is one.
 */
static int
describe_interrupts(
	const struct drv_fdt *fdt,
	uint32_t node,
	struct drv_pci_ecam_fdt_config *config)
{
	struct drv_pci_ecam_fdt_route *route;
	const uint8_t *value;
	uint32_t length;
	uint32_t child_cells;
	uint32_t position;
	uint32_t phandle;
	uint32_t controller;
	uint32_t controller_address_cells;
	uint32_t controller_interrupt_cells;
	int error;

	/* Reads the masks; without them every bit of the key counts. */
	config->devfn_mask = UINT32_MAX;
	config->pin_mask = UINT32_MAX;
	error = drv_fdt_property(fdt, node, "interrupt-map-mask", &value, &length);
	if (error == 0 && length >= (FDT_PCI_ADDRESS_CELLS + 1U) * 4U) {
		config->devfn_mask = (uint32_t)drv_fdt_cells_value(value, 0, 1U);
		config->pin_mask = (uint32_t)drv_fdt_cells_value(value, FDT_PCI_ADDRESS_CELLS, 1U);
	}

	/* A host without a map has no legacy interrupts; its devices poll or fail. */
	error = drv_fdt_property(fdt, node, "interrupt-map", &value, &length);
	if (error != 0)
		return 0;

	/* A key is a PCI address and a pin. */
	child_cells = FDT_PCI_ADDRESS_CELLS + drv_fdt_node_cells(fdt, node, "#interrupt-cells", 1U);

	/*
	 * Walks the entries.  Each is the child key, the controller's phandle,
	 * the controller's unit address and its interrupt specifier; the last
	 * two have widths the controller's node states.
	 */
	position = 0;
	while ((position + child_cells + 1U) * 4U <= length) {
		/* Finds the controller the entry names. */
		phandle = (uint32_t)drv_fdt_cells_value(value, position + child_cells, 1U);
		error = drv_fdt_find_phandle(fdt, phandle, &controller);
		if (error != 0)
			return error;
		/*
		 * Refuses a controller whose specifier is not one cell, an entry
		 * that runs off the map, and more entries than the configuration
		 * holds.
		 */
		controller_address_cells = drv_fdt_node_cells(fdt, controller, "#address-cells", 0);
		controller_interrupt_cells = drv_fdt_node_cells(fdt, controller, "#interrupt-cells", 0);
		if (controller_interrupt_cells != 1U)
			return ENOTSUP;
		if ((position + child_cells + 1U + controller_address_cells + 1U) * 4U > length)
			return EINVAL;
		if (config->route_count == DRV_PCI_ECAM_FDT_MAP_MAX)
			return ENOSPC;

		/* Records the key and the source number it arrives on. */
		route = &config->routes[config->route_count];
		route->devfn = (uint32_t)drv_fdt_cells_value(value, position, 1U) & config->devfn_mask;
		route->pin = (uint32_t)drv_fdt_cells_value(value, position + child_cells - 1U, 1U) & config->pin_mask;
		route->irq = (unsigned)drv_fdt_cells_value(value, position + child_cells + 1U + controller_address_cells, 1U);
		config->route_count++;

		/* Moves to the next entry. */
		position += child_cells + 1U + controller_address_cells + 1U;
	}

	/* Succeeded: the routes the map names are recorded. */
	return 0;
}

/*
 * Finds a function's configuration register in the window.
 *
 * Reports ENODEV for a bus the window does not hold and EINVAL for a
 * malformed access.
 */
static int
config_address(
	struct drv_pci_ecam_fdt *host,
	const struct drv_pci_address *address,
	unsigned offset,
	unsigned width,
	volatile uint8_t **target)
{
	uint64_t position;
	unsigned bus;

	/* Requires a function address the bus can carry. */
	if (address == NULL || address->segment != 0)
		return EINVAL;
	if (address->device >= 32U || address->function >= 8U)
		return EINVAL;

	/* Requires an aligned byte, halfword or word inside the function's space. */
	if (width != 1U && width != 2U && width != 4U)
		return EINVAL;
	if (offset >= ECAM_FUNCTION_SPACE || ECAM_FUNCTION_SPACE - offset < width)
		return EINVAL;
	if ((offset & (width - 1U)) != 0)
		return EINVAL;

	/* A bus outside the window has no functions. */
	if (address->bus < host->config.first_bus)
		return ENODEV;
	bus = address->bus - host->config.first_bus;
	if (bus >= host->bus_count)
		return ENODEV;

	/* Succeeded: the register's place in the window. */
	position = (uint64_t)bus << ECAM_BUS_SHIFT;
	position |= (uint64_t)address->device << ECAM_DEVICE_SHIFT;
	position |= (uint64_t)address->function << ECAM_FUNCTION_SHIFT;
	*target = host->window + position + offset;
	return 0;
}

/*
 * Reads a function's configuration space for the PCI core.
 *
 * A function on a bus outside the window reads as all-ones, as an absent
 * function does.
 */
static int
host_config_read(
	void *context,
	const struct drv_pci_address *address,
	unsigned offset,
	unsigned width,
	uint32_t *value)
{
	volatile uint8_t *target;
	int error;

	/* Refuses a malformed access. */
	if (value == NULL)
		return EINVAL;
	error = config_address(context, address, offset, width, &target);
	if (error == EINVAL)
		return EINVAL;

	/* Reads the register at the access's own width, or all-ones. */
	if (error != 0)
		*value = width == 4U ? 0xffffffffU : (1U << (width * 8U)) - 1U;
	else if (width == 1U)
		*value = kern_mmio_read8(target);
	else if (width == 2U)
		*value = kern_mmio_read16(target);
	else
		*value = kern_mmio_read32(target);

	/* Succeeded: value holds the register or all-ones. */
	return 0;
}

/*
 * Writes a function's configuration space for the PCI core.
 *
 * A write to a bus outside the window is dropped.
 */
static int
host_config_write(
	void *context,
	const struct drv_pci_address *address,
	unsigned offset,
	unsigned width,
	uint32_t value)
{
	volatile uint8_t *target;
	int error;

	/* Refuses a malformed access and drops one no function would receive. */
	error = config_address(context, address, offset, width, &target);
	if (error == EINVAL)
		return EINVAL;
	if (error != 0)
		return 0;

	/* Writes the register at the access's own width. */
	if (width == 1U)
		kern_mmio_write8(target, (uint8_t)value);
	else if (width == 2U)
		kern_mmio_write16(target, (uint16_t)value);
	else
		kern_mmio_write32(target, value);

	/* Succeeded: the write reached the function. */
	return 0;
}

/*
 * Places the memory BARs of every endpoint on the root bus.
 */
static int
assign_resources(
	struct drv_pci_ecam_fdt *host)
{
	struct drv_pci_address address;
	bool multifunction;
	unsigned device;
	unsigned function;
	int error;

	/* Visits every function of every device on the root bus. */
	kern_memset(&address, 0, sizeof(address));
	address.bus = (uint8_t)host->config.first_bus;
	for (device = 0; device < 32U; device++) {
		/* Starts at the device's function 0, not knowing yet whether it has more. */
		address.device = (uint8_t)device;
		multifunction = false;
		for (function = 0; function < 8U; function++) {
			/* Stops after function 0 unless the device has more. */
			if (function != 0 && !multifunction)
				break;

			/* Places the function's BARs. */
			address.function = (uint8_t)function;
			error = assign_function(host, &address, &multifunction);
			if (error != 0)
				return error;
		}
	}

	/* Succeeded: the root bus is ready for enumeration. */
	return 0;
}

/*
 * Places the memory BARs of one function.
 *
 * Multifunction reports, for function 0, whether the device has others.  An
 * absent function and anything but an endpoint are left alone.
 */
static int
assign_function(
	struct drv_pci_ecam_fdt *host,
	const struct drv_pci_address *address,
	bool *multifunction)
{
	uint32_t identity;
	uint32_t header;
	uint32_t command;
	unsigned index;
	bool wide;
	int error;

	/* Skips an absent function. */
	identity = 0xffffffffU;
	header = 0xffU;
	command = 0;
	(void)host_config_read(host, address, PCI_VENDOR_ID, 4U, &identity);
	if ((identity & 0xffffU) == 0xffffU)
		return 0;

	/* Learns whether the device has more functions and what this one is. */
	(void)host_config_read(host, address, PCI_HEADER_TYPE, 1U, &header);
	if (address->function == 0 && (header & PCI_HEADER_MULTIFUNCTION) != 0)
		*multifunction = true;
	if ((header & PCI_HEADER_LAYOUT) != PCI_HEADER_ENDPOINT) {
		kern_logf("pci: ECAM %02x:%02x.%u is not an endpoint, left unassigned\n",
			  address->bus,
			  address->device,
			  address->function);
		return 0;
	}

	/* Stops the function decoding while its BARs are measured. */
	(void)host_config_read(host, address, PCI_COMMAND, 2U, &command);
	(void)host_config_write(host, address, PCI_COMMAND, 2U, 0);

	/* Places each BAR; a 64-bit BAR takes the next slot as its upper half. */
	for (index = 0; index < PCI_ENDPOINT_BARS; index++) {
		error = assign_bar(host, address, index, &wide);
		if (error != 0)
			return error;

		/* Skips the upper half of a 64-bit BAR. */
		if (wide)
			index++;
	}

	/* Gives the function back the decoding it had; its driver enables what it uses. */
	(void)host_config_write(host, address, PCI_COMMAND, 2U, command);

	/* Succeeded: the function's memory BARs are placed. */
	return 0;
}

/*
 * Measures one BAR and places it in the memory window.
 *
 * Wide reports a 64-bit BAR, whose upper half is the next slot.  An I/O BAR
 * is left unassigned: the window used has no I/O space.
 */
static int
assign_bar(
	struct drv_pci_ecam_fdt *host,
	const struct drv_pci_address *address,
	unsigned index,
	bool *wide)
{
	uint32_t original;
	uint32_t low_mask;
	uint32_t high_mask;
	uint64_t mask;
	uint64_t size;
	uint64_t placed;
	uint64_t window_end;
	unsigned offset;

	/* Reads the BAR's kind; a refused read leaves the all-ones of no device. */
	offset = PCI_BAR0 + index * 4U;
	*wide = false;
	original = 0xffffffffU;
	low_mask = 0;
	(void)host_config_read(host, address, offset, 4U, &original);
	if ((original & PCI_BAR_IO) != 0)
		return 0;

	/* A 64-bit BAR in the last slot has no upper half and is sized as 32-bit. */
	if ((original & PCI_BAR_TYPE) == PCI_BAR_TYPE_64 && index + 1U < PCI_ENDPOINT_BARS)
		*wide = true;

	/* Measures the BAR by writing all-ones and reading back what sticks. */
	(void)host_config_write(host, address, offset, 4U, 0xffffffffU);
	(void)host_config_read(host, address, offset, 4U, &low_mask);
	high_mask = 0xffffffffU;
	if (*wide) {
		(void)host_config_write(host, address, offset + 4U, 4U, 0xffffffffU);
		(void)host_config_read(host, address, offset + 4U, 4U, &high_mask);
	}

	/* A BAR that keeps no address bits is not implemented. */
	if ((low_mask & PCI_BAR_MEMORY_MASK) == 0) {
		(void)host_config_write(host, address, offset, 4U, 0);
		if (*wide)
			(void)host_config_write(host, address, offset + 4U, 4U, 0);
		return 0;
	}

	/* Aligns the BAR to its own size inside the window. */
	mask = ((uint64_t)high_mask << 32) | (low_mask & PCI_BAR_MEMORY_MASK);
	size = ~mask + 1U;
	placed = (host->window_next + size - 1U) & ~(size - 1U);
	window_end = host->config.memory_pci_base + host->config.memory_size;
	if (size == 0 ||
	    placed < host->window_next ||
	    placed > window_end ||
	    size > window_end - placed) {
		kern_logf("pci: ECAM BAR%u of %02x:%02x.%u does not fit the window\n",
			  index,
			  address->bus,
			  address->device,
			  address->function);
		return ENOSPC;
	}

	/* Programs the address; the window lies below 4 GiB. */
	(void)host_config_write(host, address, offset, 4U, (uint32_t)placed);
	if (*wide)
		(void)host_config_write(host, address, offset + 4U, 4U, (uint32_t)(placed >> 32));

	/* Records the space as used. */
	host->window_next = placed + size;

	/* Reports where the BAR went. */
	kern_logf("pci: ECAM %02x:%02x.%u BAR%u at %llx (%llu bytes)\n",
		  address->bus,
		  address->device,
		  address->function,
		  index,
		  (unsigned long long)placed,
		  (unsigned long long)size);

	/* Succeeded: the BAR decodes its place in the window. */
	return 0;
}

/*
 * Maps a memory BAR for its driver.
 *
 * The BAR's bus address lies in the memory window; the CPU reaches it at
 * the same offset from the window's CPU base.
 */
static int
host_map_bar(
	void *context,
	struct drv_pci_device *device,
	const struct drv_pci_bar *bar,
	unsigned flags,
	struct drv_pci_mapping *mapping)
{
	struct drv_pci_ecam_fdt *host;
	uint64_t window_end;
	uint64_t cpu_address;
	void *address;
	int error;

	UNUSED_PARAMETER(device);
	UNUSED_PARAMETER(flags);

	/* Refuses an I/O BAR, an empty one and missing storage. */
	host = context;
	if (bar == NULL || mapping == NULL)
		return EINVAL;
	if (bar->type == DRV_PCI_BAR_IO ||
	    bar->type == DRV_PCI_BAR_NONE ||
	    bar->size == 0)
		return EINVAL;

	/* Refuses a BAR outside the memory window. */
	window_end = host->config.memory_pci_base + host->config.memory_size;
	if (bar->bus_address < host->config.memory_pci_base)
		return EINVAL;
	if (bar->bus_address >= window_end || bar->size > window_end - bar->bus_address)
		return EINVAL;

	/* Maps the CPU side of the BAR as uncached device memory. */
	cpu_address = host->config.memory_cpu_base + (bar->bus_address - host->config.memory_pci_base);
	error = kern_device_map(cpu_address, (size_t)bar->size, KERN_DEVICE_UNCACHED, &address);
	if (error != 0)
		return error;

	/* Describes the mapping; the first private word marks it as owned here. */
	mapping->address = address;
	mapping->physical_address = cpu_address;
	mapping->size = (size_t)bar->size;
	mapping->type = bar->type;
	mapping->private_data[0] = 1U;
	mapping->private_data[1] = 0;

	/* Succeeded: the driver reaches the BAR at address. */
	return 0;
}

/* Removes a BAR mapping made by host_map_bar(). */
static void
host_unmap_bar(
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
 * Gives a device its legacy interrupt.
 *
 * The device's pin is swizzled across each bridge between it and the root
 * bus, then the root-bus slot and pin are looked up in the interrupt map.
 * MSI is not offered: the board has no message receiver.
 */
static int
host_allocate_irqs(
	void *context,
	struct drv_pci_device *device,
	enum drv_pci_irq_type type,
	unsigned minimum,
	unsigned maximum,
	struct drv_pci_irq *irqs,
	unsigned *count)
{
	const struct drv_pci_ecam_fdt_route *route;
	struct drv_pci_ecam_fdt *host;
	struct drv_pci_address address;
	struct drv_pci_device *walk;
	struct drv_pci_bus *bus;
	struct drv_pci_bus *above;
	uint32_t devfn;
	uint32_t key;
	unsigned index;
	uint8_t pin;
	int error;

	/* Refuses missing arguments and a request for more than one line. */
	host = context;
	if (device == NULL || irqs == NULL || count == NULL)
		return EINVAL;
	if (minimum != 1U || maximum < minimum)
		return ENOTSUP;

	/* Offers only the legacy pins. */
	if (type != DRV_PCI_IRQ_INTX)
		return ENOTSUP;

	/* Reads which pin the device uses; zero means none. */
	error = drv_pci_device_config_read8(device, PCI_INTERRUPT_PIN, &pin);
	if (error != 0)
		return ENODEV;
	if (pin == 0 || pin > PCI_INTX_PINS)
		return ENODEV;

	/*
	 * Swizzles the pin by the device number at each bus below the root bus.
	 *
	 * Procedure referenced: PCI-to-PCI Bridge Architecture Specification,
	 * Revision 1.2, section 9.1 (the interrupt pin of a device behind a
	 * bridge as seen on the bridge's primary side).
	 */
	walk = device;
	for (;;) {
		/* Stops at the root bus, where the interrupt map applies. */
		bus = drv_pci_device_bus(walk);
		above = drv_pci_bus_parent(bus);
		if (above == NULL)
			break;

		/* Rotates the pin by the device's slot and moves to the bridge above. */
		drv_pci_device_address(walk, &address);
		pin = (uint8_t)(((pin - 1U + address.device) % PCI_INTX_PINS) + 1U);
		walk = drv_pci_bus_bridge(bus);
	}

	/* Builds the root-bus function's key. */
	drv_pci_device_address(walk, &address);
	devfn = (uint32_t)address.bus << FDT_PCI_BUS_SHIFT;
	devfn |= (uint32_t)address.device << (FDT_PCI_DEVFN_SHIFT + 3U);
	devfn |= (uint32_t)address.function << FDT_PCI_DEVFN_SHIFT;
	devfn &= host->config.devfn_mask;
	key = (uint32_t)pin & host->config.pin_mask;

	/* Looks the key up in the interrupt map. */
	route = NULL;
	for (index = 0; index < host->config.route_count; index++) {
		/* Takes the first entry with the function's key and pin. */
		if (host->config.routes[index].devfn != devfn)
			continue;
		if (host->config.routes[index].pin != key)
			continue;
		route = &host->config.routes[index];
		break;
	}

	/* Refuses a pin the interrupt map does not route. */
	if (route == NULL)
		return ENODEV;

	/* Succeeded: the device's line is the mapped interrupt. */
	irqs[0].type = DRV_PCI_IRQ_INTX;
	irqs[0].index = 0;
	irqs[0].vector = route->irq;
	irqs[0].private_data[0] = 0;
	irqs[0].private_data[1] = 0;
	*count = 1U;
	return 0;
}

/* Releases interrupts from host_allocate_irqs(), which hold nothing. */
static void
host_free_irqs(
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
