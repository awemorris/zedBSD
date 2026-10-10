/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Broadcom STB PCIe root complex (BCM2711).
 *
 * The root complex is bus 0: its own configuration space is the start of its
 * register block and it presents itself as a PCI-to-PCI bridge (the root
 * port).  Everything below the root port is reached through an index
 * register and a 4 KiB data window.  The CPU reaches device BARs through one
 * outbound window, and devices reach system memory through one inbound
 * window.  The controller does not snoop the CPU caches.
 *
 * Nothing here relies on the firmware having set the controller up: the
 * start sequence resets it, powers the SerDes, programs both windows, releases
 * the endpoint from reset and waits for the link.  The PCI core reads bus
 * numbers and BARs but does not assign them, so this driver numbers the bus
 * below the root port and places the endpoints' memory BARs in the outbound
 * window before the core enumerates the tree.
 *
 * The register offsets and bit positions are those of the hardware; the
 * sequence is described in plan/ws048/design.md.
 */

#include <drivers/pci/pci-brcmstb.h>
#include <drivers/generic/dma.h>
#include <kern/clock.h>
#include <kern/device-io.h>
#include <kern/kcrt.h>
#include <kern/klog.h>
#include <kern/kmem.h>
#include <kern/lock.h>
#include <kern/pmem.h>
#include <uapi/errno.h>

/* The PCI Express capability of the root complex's own configuration space. */
#define BRCMSTB_RC_LINK_STATUS		(0x00acU + 0x12U)
#define BRCMSTB_LINK_SPEED		0x000fU
#define BRCMSTB_LINK_WIDTH		0x03f0U
#define BRCMSTB_LINK_WIDTH_SHIFT	4U

/* The byte order of inbound traffic; zero is little-endian. */
#define BRCMSTB_VENDOR_REG1		0x0188U
#define BRCMSTB_VENDOR_REG1_ENDIAN	0x0000000cU

/* The class code the root complex reports for itself. */
#define BRCMSTB_ID_VAL3			0x043cU
#define BRCMSTB_ID_VAL3_CLASS		0x00ffffffU
#define BRCMSTB_CLASS_PCI_BRIDGE	0x00060400U

/* System-bus access, error reporting of absent functions, burst and memory size. */
#define BRCMSTB_MISC_CTRL		0x4008U
#define BRCMSTB_MISC_CTRL_SCB_ACCESS	0x00001000U
#define BRCMSTB_MISC_CTRL_UR_MODE	0x00002000U
#define BRCMSTB_MISC_CTRL_BURST		0x00300000U
#define BRCMSTB_MISC_CTRL_BURST_128	0x00000000U
#define BRCMSTB_MISC_CTRL_SCB0_SIZE	0xf8000000U
#define BRCMSTB_MISC_CTRL_SCB0_SHIFT	27U

/* Outbound window 0: its PCI address and its CPU base and limit. */
#define BRCMSTB_WIN0_LO			0x400cU
#define BRCMSTB_WIN0_HI			0x4010U
#define BRCMSTB_WIN0_BASE_LIMIT		0x4070U
#define BRCMSTB_WIN0_BASE		0x0000fff0U
#define BRCMSTB_WIN0_BASE_SHIFT		4U
#define BRCMSTB_WIN0_LIMIT		0xfff00000U
#define BRCMSTB_WIN0_LIMIT_SHIFT	20U
#define BRCMSTB_WIN0_BASE_HI		0x4080U
#define BRCMSTB_WIN0_LIMIT_HI		0x4084U
#define BRCMSTB_WIN0_HI_MASK		0x000000ffU

/* The inbound windows; BAR2 carries system memory, the others are closed. */
#define BRCMSTB_RC_BAR1_LO		0x402cU
#define BRCMSTB_RC_BAR2_LO		0x4034U
#define BRCMSTB_RC_BAR2_HI		0x4038U
#define BRCMSTB_RC_BAR3_LO		0x403cU
#define BRCMSTB_RC_BAR_SIZE		0x0000001fU

/* The link state. */
#define BRCMSTB_STATUS			0x4068U
#define BRCMSTB_STATUS_PHYLINKUP	0x00000010U
#define BRCMSTB_STATUS_DL_ACTIVE	0x00000020U
#define BRCMSTB_STATUS_RC_MODE		0x00000080U
#define BRCMSTB_REVISION		0x406cU

/* SerDes power and reference-clock gating. */
#define BRCMSTB_HARD_DEBUG		0x4204U
#define BRCMSTB_HARD_DEBUG_CLKREQ	0x00000002U
#define BRCMSTB_HARD_DEBUG_SERDES_IDDQ	0x08000000U

/* The controller's own interrupt summary (link events, MSI). */
#define BRCMSTB_INTR2_CLEAR		0x4308U
#define BRCMSTB_INTR2_MASK_SET		0x4310U

/* Configuration access below the root port. */
#define BRCMSTB_EXT_CFG_DATA		0x8000U
#define BRCMSTB_EXT_CFG_INDEX		0x9000U
#define BRCMSTB_EXT_CFG_BUS_SHIFT	20U
#define BRCMSTB_EXT_CFG_DEVICE_SHIFT	15U
#define BRCMSTB_EXT_CFG_FUNCTION_SHIFT	12U

/* Bridge and endpoint reset. */
#define BRCMSTB_SW_INIT			0x9210U
#define BRCMSTB_SW_INIT_PERST		0x00000001U
#define BRCMSTB_SW_INIT_BRIDGE		0x00000002U

/* The register block must reach the last register above. */
#define BRCMSTB_REGISTER_SPAN		(BRCMSTB_SW_INIT + 4U)

/* Waits of the start sequence, in microseconds. */
#define BRCMSTB_RESET_US		100U
#define BRCMSTB_RESET_MAX_US		200U
#define BRCMSTB_LINK_POLL_US		5000U
#define BRCMSTB_LINK_POLL_MAX_US	6000U

/* PCIe asks for 100 ms between the end of reset and the first configuration request. */
#define BRCMSTB_LINK_SETTLE_POLLS	20U

/* The link gets as long again to come up after that. */
#define BRCMSTB_LINK_MAX_POLLS		40U

/* The outbound window's CPU side is programmed in megabytes. */
#define BRCMSTB_MEGABYTE		0x100000ULL

/* Standard configuration header offsets. */
#define PCI_VENDOR_ID			0x00U
#define PCI_COMMAND			0x04U
#define PCI_HEADER_TYPE			0x0eU
#define PCI_BAR0			0x10U
#define PCI_INTERRUPT_PIN		0x3dU
#define PCI_BRIDGE_BUSES		0x18U
#define PCI_BRIDGE_IO_BASE		0x1cU
#define PCI_BRIDGE_IO_LIMIT		0x1dU
#define PCI_BRIDGE_MEMORY		0x20U
#define PCI_BRIDGE_PREFETCH		0x24U
#define PCI_BRIDGE_PREFETCH_BASE_HI	0x28U
#define PCI_BRIDGE_PREFETCH_LIMIT_HI	0x2cU
#define PCI_BRIDGE_IO_HI		0x30U
#define PCI_COMMAND_MEMORY		0x0002U
#define PCI_COMMAND_MASTER		0x0004U
#define PCI_HEADER_LAYOUT		0x7fU
#define PCI_HEADER_MULTIFUNCTION	0x80U
#define PCI_HEADER_BRIDGE		0x01U
#define PCI_BAR_IO			0x00000001U
#define PCI_BAR_TYPE			0x00000006U
#define PCI_BAR_TYPE_64			0x00000004U
#define PCI_BAR_MEMORY_MASK		0xfffffff0U
#define PCI_ENDPOINT_BARS		6U

/* The bus the root port leads to; the root complex itself is bus 0. */
#define BRCMSTB_ROOT_BUS		0U
#define BRCMSTB_SECONDARY_BUS		1U

/* The space code of the first cell of a PCI address in a device tree. */
#define FDT_PCI_SPACE_SHIFT		24U
#define FDT_PCI_SPACE_MASK		3U
#define FDT_PCI_SPACE_MEMORY32		2U
#define FDT_PCI_SPACE_MEMORY64		3U

/* The cells of a PCI address, and which of them carry the address. */
#define FDT_PCI_ADDRESS_CELLS		3U
#define FDT_PCI_ADDRESS_LOW_CELL	1U

/* The ARM GIC's interrupt specifier: kind, number and trigger. */
#define FDT_GIC_INTERRUPT_CELLS		3U
#define FDT_GIC_SPI			0U
#define FDT_GIC_PPI			1U
#define GIC_SPI_BASE			32U
#define GIC_PPI_BASE			16U

/* What devices behind the controller can reach with DMA. */
#define BRCMSTB_DMA_ADDRESS_BITS	31U
#define BRCMSTB_DMA_MAX_SEGMENT		(16U * 1024U * 1024U)

/*
 * One root complex the driver has started.
 *
 * It lives from a successful start for as long as the kernel runs: the PCI
 * core keeps it as the host of the root bus.  The configuration lock
 * serializes the index register and the data window it selects, which form
 * one access and must not interleave with another.  Window next is where
 * the next endpoint BAR goes; everything below it in the outbound window is
 * assigned.
 */
struct drv_pci_brcmstb {
	struct drv_pci_brcmstb_config config;
	volatile uint8_t *registers;
	struct spinlock config_lock;
	struct drv_dma_device *dma;
	struct drv_pci_bus *root;
	uint64_t window_next;
	bool link_up;
};

static int describe_outbound(const struct drv_fdt *fdt, uint32_t node, struct drv_pci_brcmstb_config *config);
static int describe_inbound(const struct drv_fdt *fdt, uint32_t node, struct drv_pci_brcmstb_config *config);
static int describe_interrupts(const struct drv_fdt *fdt, uint32_t node, struct drv_pci_brcmstb_config *config);
static int decode_gic_interrupt(const struct drv_fdt *fdt, uint32_t controller, const uint8_t *specifier, unsigned *irq);
static int check_config(const struct drv_pci_brcmstb_config *config);
static int bring_up(struct drv_pci_brcmstb *host);
static uint32_t register_read(struct drv_pci_brcmstb *host, unsigned offset);
static void register_write(struct drv_pci_brcmstb *host, unsigned offset, uint32_t value);
static void register_update(struct drv_pci_brcmstb *host, unsigned offset, uint32_t clear, uint32_t set);
static void reset_controller(struct drv_pci_brcmstb *host);
static int set_inbound_window(struct drv_pci_brcmstb *host);
static int inbound_size_log2(uint64_t size, unsigned *log2);
static int train_link(struct drv_pci_brcmstb *host);
static bool link_is_up(struct drv_pci_brcmstb *host);
static void set_outbound_window(struct drv_pci_brcmstb *host);
static void report_link(struct drv_pci_brcmstb *host);
static int config_window(struct drv_pci_brcmstb *host, const struct drv_pci_address *address, unsigned offset, volatile uint8_t **window);
static bool config_access_valid(const struct drv_pci_address *address, unsigned offset, unsigned width);
static int host_config_read(void *context, const struct drv_pci_address *address, unsigned offset, unsigned width, uint32_t *value);
static int host_config_write(void *context, const struct drv_pci_address *address, unsigned offset, unsigned width, uint32_t value);
static int assign_resources(struct drv_pci_brcmstb *host);
static int assign_function(struct drv_pci_brcmstb *host, const struct drv_pci_address *address, bool *multifunction);
static int assign_bar(struct drv_pci_brcmstb *host, const struct drv_pci_address *address, unsigned index, bool *wide);
static void configure_root_port(struct drv_pci_brcmstb *host);
static int host_map_bar(void *context, struct drv_pci_device *device, const struct drv_pci_bar *bar, unsigned flags, struct drv_pci_mapping *mapping);
static void host_unmap_bar(void *context, struct drv_pci_mapping *mapping);
static int host_allocate_irqs(void *context, struct drv_pci_device *device, enum drv_pci_irq_type type, unsigned minimum, unsigned maximum, struct drv_pci_irq *irqs, unsigned *count);
static void host_free_irqs(void *context, struct drv_pci_device *device, struct drv_pci_irq *irqs, unsigned count);

/*
 * The operations the PCI core calls on the root bus and every bus below it.
 *
 * The table is constant; the host each call works on arrives as its context.
 */
static const struct drv_pci_bus_ops brcmstb_bus_ops = {
	.config_space_size = 4096,
	.config_read = host_config_read,
	.config_write = host_config_write,
	.map_bar = host_map_bar,
	.unmap_bar = host_unmap_bar,
	.allocate_irqs = host_allocate_irqs,
	.free_irqs = host_free_irqs
};

/*
 * Describes a root complex from its device tree node.
 *
 * Reports ENODEV for a node the tree marks as not present, which is how
 * QEMU's raspi4b hides the PCIe it does not emulate.
 */
int
drv_pci_brcmstb_describe(
	const struct drv_fdt *fdt,
	uint32_t node,
	struct drv_pci_brcmstb_config *config)
{
	bool enabled;
	int error;

	/* Refuses missing arguments. */
	if (fdt == NULL || config == NULL)
		return EINVAL;

	/* Leaves a disabled controller alone. */
	enabled = drv_fdt_node_enabled(fdt, node);
	if (!enabled)
		return ENODEV;

	/* Reads where the registers are. */
	kern_memset(config, 0, sizeof(*config));
	error = drv_fdt_reg(fdt, node, 0, &config->register_base, &config->register_size);
	if (error != 0)
		return error;

	/* Reads the window the CPU reaches device BARs through. */
	error = describe_outbound(fdt, node, config);
	if (error != 0)
		return error;

	/* Reads the window devices reach system memory through. */
	error = describe_inbound(fdt, node, config);
	if (error != 0)
		return error;

	/* Reads where the four legacy interrupt pins arrive. */
	error = describe_interrupts(fdt, node, config);
	if (error != 0)
		return error;

	/* Succeeded: the configuration describes the controller. */
	return 0;
}

/*
 * Resets a root complex, trains its link and assigns the bus below it.
 *
 * On success the host is ready for drv_pci_brcmstb_publish(); in between,
 * the platform may talk to the endpoints with drv_pci_brcmstb_config_read().
 * On failure the endpoint is held in reset and nothing is published.
 */
int
drv_pci_brcmstb_start(
	const struct drv_pci_brcmstb_config *config,
	struct drv_pci_brcmstb **result)
{
	struct drv_pci_brcmstb *host;
	void *registers;
	int error;

	/* Refuses missing arguments. */
	if (config == NULL || result == NULL)
		return EINVAL;

	/* Refuses windows the controller cannot express. */
	error = check_config(config);
	if (error != 0)
		return error;

	/* Allocates the host record. */
	host = kern_malloc(sizeof(*host));
	if (host == NULL)
		return ENOMEM;

	/* Records the configuration; the outbound window is still empty. */
	kern_memset(host, 0, sizeof(*host));
	host->config = *config;
	host->window_next = config->outbound_pci_base;
	spin_init(&host->config_lock, LOCK_RANK_DEVICE, "brcmstb configuration");

	/* Maps the register block. */
	error = kern_device_map(config->register_base, (size_t)config->register_size, KERN_DEVICE_UNCACHED, &registers);
	if (error != 0) {
		kern_free(host);
		return error;
	}

	/* Names the mapped controller without reading its reset-gated registers. */
	host->registers = registers;
	kern_logf("pcie: brcmstb resetting at %llx\n", (unsigned long long)config->register_base);

	/* Runs the start sequence; a failure leaves the endpoint in reset. */
	error = bring_up(host);
	if (error != 0) {
		register_update(host, BRCMSTB_SW_INIT, 0, BRCMSTB_SW_INIT_PERST);
		(void)kern_device_unmap(registers, (size_t)config->register_size);
		kern_free(host);

		/* Reports why the controller could not be started. */
		return error;
	}

	/* Succeeded: the host is ready to be published. */
	*result = host;
	return 0;
}

/*
 * Reads the configuration space of one function below a started host.
 *
 * The platform uses it between start and publish, for example to find an
 * endpoint that needs firmware before its driver attaches.
 */
int
drv_pci_brcmstb_config_read(
	struct drv_pci_brcmstb *host,
	const struct drv_pci_address *address,
	unsigned offset,
	unsigned width,
	uint32_t *value)
{
	int error;

	/* Refuses a missing host. */
	if (host == NULL)
		return EINVAL;

	/* Reads through the same path as the PCI core. */
	error = host_config_read(host, address, offset, width, value);
	if (error != 0)
		return error;

	/* Succeeded: value holds the register. */
	return 0;
}

/*
 * Places the endpoints' BARs again, before publication.
 *
 * Something the platform does between start and publish, such as the
 * Raspberry Pi firmware loading the VL805's own firmware, may reset an
 * endpoint and lose its BARs.  The placement starts again from the beginning
 * of the window, so an endpoint that kept its BARs gets the same addresses.
 */
int
drv_pci_brcmstb_reassign(
	struct drv_pci_brcmstb *host)
{
	int error;

	/* Refuses a missing host and one the PCI core already enumerated. */
	if (host == NULL)
		return EINVAL;
	if (host->root != NULL)
		return EBUSY;

	/* Empties the window and places every BAR again. */
	host->window_next = host->config.outbound_pci_base;
	error = assign_resources(host);
	if (error != 0)
		return error;

	/* Succeeded: the endpoints decode their places again. */
	return 0;
}

/*
 * Hands a started host to the PCI core, which enumerates and binds its tree.
 */
int
drv_pci_brcmstb_publish(
	struct drv_pci_brcmstb *host)
{
	struct drv_dma_constraints constraints;
	int error;

	/* Refuses a missing host and a second publication. */
	if (host == NULL)
		return EINVAL;
	if (host->root != NULL)
		return EBUSY;

	/*
	 * Describes what devices can reach.  The inbound window starts at CPU 0
	 * but PCI addresses from the outbound window upward are not system
	 * memory, so DMA stays below 2 GiB, which every board's window covers.
	 * The controller does not snoop the CPU caches.
	 */
	kern_memset(&constraints, 0, sizeof(constraints));
	constraints.address_bits = BRCMSTB_DMA_ADDRESS_BITS;
	constraints.max_segment_size = BRCMSTB_DMA_MAX_SEGMENT;
	constraints.segment_boundary = 0;
	constraints.coherent = 0;
	error = drv_dma_device_create(&constraints, &host->dma);
	if (error != 0)
		return error;

	/* Creates bus 0, whose only function is the root port. */
	error = drv_pci_bus_create_root(0, BRCMSTB_ROOT_BUS, &brcmstb_bus_ops, host, host->dma, &host->root);
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

/* Reads the outbound memory window from the node's ranges. */
static int
describe_outbound(
	const struct drv_fdt *fdt,
	uint32_t node,
	struct drv_pci_brcmstb_config *config)
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
	int error;

	/* Reads the ranges. */
	error = drv_fdt_property(fdt, node, "ranges", &value, &length);
	if (error != 0)
		return error;

	/* Finds the bus the controller sits on, which sets the parent address width. */
	error = drv_fdt_parent(fdt, node, &parent);
	if (error != 0)
		return error;

	/* A PCI bus has three address cells; the parent bus says its own count. */
	parent_cells = drv_fdt_node_cells(fdt, parent, "#address-cells", 2U);
	size_cells = drv_fdt_node_cells(fdt, node, "#size-cells", 2U);
	if (parent_cells > 2U || size_cells > 2U)
		return ERANGE;

	/* Takes the first memory window; an I/O window has no use here. */
	entry_cells = FDT_PCI_ADDRESS_CELLS + parent_cells + size_cells;
	for (entry = 0; (entry + 1U) * entry_cells * 4U <= length; entry++) {
		/* Skips anything but 32-bit and 64-bit memory space. */
		space = (uint32_t)drv_fdt_cells_value(value, entry * entry_cells, 1U);
		space = (space >> FDT_PCI_SPACE_SHIFT) & FDT_PCI_SPACE_MASK;
		if (space != FDT_PCI_SPACE_MEMORY32 && space != FDT_PCI_SPACE_MEMORY64)
			continue;

		/* Decodes the PCI address, the parent-bus address and the size. */
		config->outbound_pci_base = drv_fdt_cells_value(value, entry * entry_cells + FDT_PCI_ADDRESS_LOW_CELL, 2U);
		parent_address = drv_fdt_cells_value(value, entry * entry_cells + FDT_PCI_ADDRESS_CELLS, parent_cells);
		config->outbound_size = drv_fdt_cells_value(value, entry * entry_cells + FDT_PCI_ADDRESS_CELLS + parent_cells, size_cells);

		/* Moves the parent-bus address into the CPU's space. */
		error = drv_fdt_translate(fdt, node, parent_address, &config->outbound_cpu_base);
		if (error != 0)
			return error;

		/* Succeeded: the first memory window is recorded. */
		return 0;
	}

	/* Reports a controller without a memory window. */
	return ENOENT;
}

/*
 * Reads the inbound window from the node's dma-ranges.
 *
 * The parent-bus address is kept as the CPU address: the buses above the
 * BCM2711's controller pass system memory through unchanged.
 */
static int
describe_inbound(
	const struct drv_fdt *fdt,
	uint32_t node,
	struct drv_pci_brcmstb_config *config)
{
	const uint8_t *value;
	uint32_t length;
	uint32_t parent;
	uint32_t parent_cells;
	uint32_t size_cells;
	uint32_t entry_cells;
	int error;

	/* Reads the dma-ranges. */
	error = drv_fdt_property(fdt, node, "dma-ranges", &value, &length);
	if (error != 0)
		return error;

	/* Finds the bus the controller sits on, which sets the parent address width. */
	error = drv_fdt_parent(fdt, node, &parent);
	if (error != 0)
		return error;

	/* The parent bus says how wide its addresses are. */
	parent_cells = drv_fdt_node_cells(fdt, parent, "#address-cells", 2U);
	size_cells = drv_fdt_node_cells(fdt, node, "#size-cells", 2U);
	entry_cells = FDT_PCI_ADDRESS_CELLS + parent_cells + size_cells;
	if (parent_cells > 2U || size_cells > 2U)
		return ERANGE;
	if (entry_cells * 4U > length)
		return ENOENT;

	/* Decodes the PCI address, the system-memory address and the size. */
	config->inbound_pci_base = drv_fdt_cells_value(value, FDT_PCI_ADDRESS_LOW_CELL, 2U);
	config->inbound_cpu_base = drv_fdt_cells_value(value, FDT_PCI_ADDRESS_CELLS, parent_cells);
	config->inbound_size = drv_fdt_cells_value(value, FDT_PCI_ADDRESS_CELLS + parent_cells, size_cells);

	/* Succeeded: the inbound window is recorded. */
	return 0;
}

/* Reads which interrupt each legacy pin arrives on from the interrupt map. */
static int
describe_interrupts(
	const struct drv_fdt *fdt,
	uint32_t node,
	struct drv_pci_brcmstb_config *config)
{
	const uint8_t *value;
	uint32_t length;
	uint32_t child_cells;
	uint32_t position;
	uint32_t phandle;
	uint32_t controller;
	uint32_t controller_address_cells;
	uint32_t controller_interrupt_cells;
	uint32_t pin;
	unsigned irq;
	int error;

	/* Reads the map and the width of one pin's key. */
	error = drv_fdt_property(fdt, node, "interrupt-map", &value, &length);
	if (error != 0)
		return error;

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
		pin = (uint32_t)drv_fdt_cells_value(value, position + child_cells - 1U, 1U);
		phandle = (uint32_t)drv_fdt_cells_value(value, position + child_cells, 1U);
		error = drv_fdt_find_phandle(fdt, phandle, &controller);
		if (error != 0)
			return error;

		/* An absent #address-cells gives the controller no unit address. */
		controller_address_cells = drv_fdt_node_cells(fdt, controller, "#address-cells", 0);
		controller_interrupt_cells = drv_fdt_node_cells(fdt, controller, "#interrupt-cells", 0);
		position += child_cells + 1U + controller_address_cells;
		if ((position + controller_interrupt_cells) * 4U > length)
			return EINVAL;

		/* Records the interrupt of each of the four pins. */
		if (pin >= 1U && pin <= DRV_PCI_BRCMSTB_INTX_COUNT) {
			/* Only the GIC's three-cell specifier is understood. */
			if (controller_interrupt_cells != FDT_GIC_INTERRUPT_CELLS)
				return ENOTSUP;

			/* Converts the specifier to the kernel's number. */
			error = decode_gic_interrupt(fdt, controller, value + position * 4U, &irq);
			if (error != 0)
				return error;

			/* Records the pin's interrupt. */
			config->intx_irq[pin - 1U] = irq;
		}

		/* Moves to the next entry. */
		position += controller_interrupt_cells;
	}

	/* Succeeded: the pins the map names are recorded. */
	return 0;
}

/* Converts a GIC interrupt specifier into the kernel's interrupt number. */
static int
decode_gic_interrupt(
	const struct drv_fdt *fdt,
	uint32_t controller,
	const uint8_t *specifier,
	unsigned *irq)
{
	const uint8_t *marker;
	uint32_t length;
	uint32_t kind;
	uint32_t number;
	int error;

	/* Refuses a parent that is not an interrupt controller. */
	error = drv_fdt_property(fdt, controller, "interrupt-controller", &marker, &length);
	if (error != 0)
		return error;

	/* The kernel numbers GIC interrupts by their interrupt ID. */
	kind = (uint32_t)drv_fdt_cells_value(specifier, 0, 1U);
	number = (uint32_t)drv_fdt_cells_value(specifier, 1U, 1U);
	if (kind == FDT_GIC_SPI) {
		*irq = number + GIC_SPI_BASE;
	} else if (kind == FDT_GIC_PPI) {
		*irq = number + GIC_PPI_BASE;
	} else {
		/* Refuses a kind of interrupt the GIC does not have. */
		return EINVAL;
	}

	/* Succeeded: irq holds the interrupt ID. */
	return 0;
}

/* Refuses a configuration the controller's registers cannot express. */
static int
check_config(
	const struct drv_pci_brcmstb_config *config)
{
	/* Requires the whole register block, up to the reset register. */
	if (config->register_size < BRCMSTB_REGISTER_SPAN)
		return EINVAL;

	/* Requires a non-empty outbound window on megabyte boundaries. */
	if (config->outbound_size == 0)
		return EINVAL;
	if ((config->outbound_cpu_base % BRCMSTB_MEGABYTE) != 0)
		return EINVAL;
	if ((config->outbound_size % BRCMSTB_MEGABYTE) != 0)
		return EINVAL;

	/* Requires BARs to be placeable with 32-bit PCI addresses. */
	if (config->outbound_pci_base > 0xffffffffULL)
		return EINVAL;
	if (config->outbound_size > 0x100000000ULL - config->outbound_pci_base)
		return EINVAL;

	/* The inbound window maps PCI addresses onto system memory from CPU 0. */
	if (config->inbound_size == 0)
		return EINVAL;
	if (config->inbound_cpu_base != 0)
		return EINVAL;

	/* Succeeded: the controller can be programmed with this configuration. */
	return 0;
}

/*
 * Runs the start sequence on a mapped controller.
 *
 * The bridge and endpoint are reset, the SerDes powered, both windows
 * opened, the link trained and the bus below the root port assigned.
 */
static int
bring_up(
	struct drv_pci_brcmstb *host)
{
	uint32_t revision;
	int error;

	/* Puts the bridge and the endpoint into reset and powers the SerDes. */
	reset_controller(host);

	/* Reads the revision only after the bridge and SerDes are awake. */
	revision = register_read(host, BRCMSTB_REVISION);
	kern_logf("pcie: brcmstb at %llx revision %x\n", (unsigned long long)host->config.register_base, revision);

	/* Opens the inbound window and quiets the controller's own interrupts. */
	error = set_inbound_window(host);
	if (error != 0)
		return error;

	/* Releases the endpoint and waits for the link. */
	error = train_link(host);
	if (error != 0)
		return error;

	/* Opens the outbound window and presents the root port as a bridge. */
	set_outbound_window(host);
	register_update(host, BRCMSTB_ID_VAL3, BRCMSTB_ID_VAL3_CLASS, BRCMSTB_CLASS_PCI_BRIDGE);
	register_update(host, BRCMSTB_HARD_DEBUG, 0, BRCMSTB_HARD_DEBUG_CLKREQ);
	report_link(host);

	/* Numbers the bus below the root port and places its BARs. */
	error = assign_resources(host);
	if (error != 0)
		return error;

	/* Succeeded: the tree below the root port is ready. */
	return 0;
}

/* Reads one controller register. */
static uint32_t
register_read(
	struct drv_pci_brcmstb *host,
	unsigned offset)
{
	uint32_t value;

	/* Reads the register through the device mapping. */
	value = kern_mmio_read32(host->registers + offset);

	/* Reports the register's value. */
	return value;
}

/* Writes one controller register. */
static void
register_write(
	struct drv_pci_brcmstb *host,
	unsigned offset,
	uint32_t value)
{
	/* Writes the register through the device mapping. */
	kern_mmio_write32(host->registers + offset, value);
}

/* Replaces the bits of one field of a controller register. */
static void
register_update(
	struct drv_pci_brcmstb *host,
	unsigned offset,
	uint32_t clear,
	uint32_t set)
{
	uint32_t value;

	/* Reads, changes the field and writes back. */
	value = register_read(host, offset);
	value &= ~clear;
	value |= set;
	register_write(host, offset, value);
}

/*
 * Resets the bridge and the endpoint, then brings the bridge back.
 *
 * The endpoint stays in reset (PERST# asserted) until the link is trained.
 */
static void
reset_controller(
	struct drv_pci_brcmstb *host)
{
	/* Holds the bridge and the endpoint in reset. */
	register_update(host, BRCMSTB_SW_INIT, 0, BRCMSTB_SW_INIT_BRIDGE | BRCMSTB_SW_INIT_PERST);
	kern_usleep_range(BRCMSTB_RESET_US, BRCMSTB_RESET_MAX_US);

	/* Releases the bridge alone. */
	register_update(host, BRCMSTB_SW_INIT, BRCMSTB_SW_INIT_BRIDGE, 0);

	/* Powers the SerDes and lets it settle. */
	register_update(host, BRCMSTB_HARD_DEBUG, BRCMSTB_HARD_DEBUG_SERDES_IDDQ, 0);
	kern_usleep_range(BRCMSTB_RESET_US, BRCMSTB_RESET_MAX_US);
}

/*
 * Lets the controller reach system memory and maps it for devices.
 *
 * Absent functions are made to read as all-ones instead of raising an
 * error the CPU would take as an external abort.
 */
static int
set_inbound_window(
	struct drv_pci_brcmstb *host)
{
	uint64_t window;
	uint32_t size_code;
	unsigned log2;
	int error;

	/* Rounds the window up to the power of two the registers express. */
	error = inbound_size_log2(host->config.inbound_size, &log2);
	if (error != 0)
		return error;

	/* Refuses a PCI base the rounded window is not aligned to. */
	window = (uint64_t)1 << log2;
	if ((host->config.inbound_pci_base & (window - 1U)) != 0)
		return EINVAL;

	/* The size code is log2 - 12 + 0x1c below 64 KiB and log2 - 15 above. */
	if (log2 < 16U)
		size_code = log2 - 12U + 0x1cU;
	else
		size_code = log2 - 15U;

	/* Opens system-bus access, all-ones for absent functions, 128-byte bursts, memory size. */
	register_update(host,
			BRCMSTB_MISC_CTRL,
			BRCMSTB_MISC_CTRL_BURST | BRCMSTB_MISC_CTRL_SCB0_SIZE,
			BRCMSTB_MISC_CTRL_SCB_ACCESS |
			BRCMSTB_MISC_CTRL_UR_MODE |
			BRCMSTB_MISC_CTRL_BURST_128 |
			((log2 - 15U) << BRCMSTB_MISC_CTRL_SCB0_SHIFT));

	/* Maps the PCI window onto system memory from CPU address 0. */
	register_write(host, BRCMSTB_RC_BAR2_LO, (uint32_t)host->config.inbound_pci_base | size_code);
	register_write(host, BRCMSTB_RC_BAR2_HI, (uint32_t)(host->config.inbound_pci_base >> 32));

	/* Closes the other two inbound windows. */
	register_update(host, BRCMSTB_RC_BAR1_LO, BRCMSTB_RC_BAR_SIZE, 0);
	register_update(host, BRCMSTB_RC_BAR3_LO, BRCMSTB_RC_BAR_SIZE, 0);

	/* Takes inbound traffic as little-endian. */
	register_update(host, BRCMSTB_VENDOR_REG1, BRCMSTB_VENDOR_REG1_ENDIAN, 0);

	/* Masks and clears the controller's own interrupts; nothing here uses them. */
	register_write(host, BRCMSTB_INTR2_MASK_SET, 0xffffffffU);
	register_write(host, BRCMSTB_INTR2_CLEAR, 0xffffffffU);

	/* Succeeded: devices can reach system memory once the link is up. */
	return 0;
}

/* Finds the smallest power of two, as its log2, that holds the window. */
static int
inbound_size_log2(
	uint64_t size,
	unsigned *log2)
{
	unsigned bits;

	/* Grows the power of two until it holds the window. */
	bits = 12U;
	while (bits < 37U && ((uint64_t)1 << bits) < size)
		bits++;

	/* Refuses a window larger than the registers express. */
	if (((uint64_t)1 << bits) < size)
		return ERANGE;

	/* Succeeded: log2 is the exponent. */
	*log2 = bits;
	return 0;
}

/*
 * Releases the endpoint from reset and waits for the link.
 *
 * The first configuration request may go out no sooner than 100 ms after
 * reset ends, so the wait always lasts that long; the link then gets as long
 * again to come up.
 */
static int
train_link(
	struct drv_pci_brcmstb *host)
{
	uint32_t status;
	unsigned polls;
	bool up;

	/* Releases the endpoint. */
	register_update(host, BRCMSTB_SW_INIT, BRCMSTB_SW_INIT_PERST, 0);

	/* Waits the settle time, then until the link is up or the limit passes. */
	up = false;
	for (polls = 0; polls < BRCMSTB_LINK_MAX_POLLS; polls++) {
		/* Waits one polling interval. */
		kern_usleep_range(BRCMSTB_LINK_POLL_US, BRCMSTB_LINK_POLL_MAX_US);

		/* Checks the link only once the settle time is over. */
		if (polls + 1U < BRCMSTB_LINK_SETTLE_POLLS)
			continue;

		/* Stops as soon as the link is up. */
		up = link_is_up(host);
		if (up)
			break;
	}

	/* Refuses a link that never came up; nothing answers below the root port. */
	if (!up) {
		kern_logf("pcie: brcmstb link down\n");
		return ETIMEDOUT;
	}

	/* Refuses a controller strapped as an endpoint. */
	status = register_read(host, BRCMSTB_STATUS);
	if ((status & BRCMSTB_STATUS_RC_MODE) == 0) {
		kern_logf("pcie: brcmstb is not in root complex mode\n");
		return ENODEV;
	}

	/* Succeeded: functions below the root port can be reached. */
	host->link_up = true;
	return 0;
}

/* Reports whether the physical and data-link layers are both up. */
static bool
link_is_up(
	struct drv_pci_brcmstb *host)
{
	uint32_t status;

	/* Reads the link state. */
	status = register_read(host, BRCMSTB_STATUS);

	/* A link is usable only when the physical layer is up. */
	if ((status & BRCMSTB_STATUS_PHYLINKUP) == 0)
		return false;

	/* It also needs the data-link layer active. */
	if ((status & BRCMSTB_STATUS_DL_ACTIVE) == 0)
		return false;

	/* Reports a usable link. */
	return true;
}

/* Opens the outbound window from its CPU range to its PCI range. */
static void
set_outbound_window(
	struct drv_pci_brcmstb *host)
{
	uint64_t base_mb;
	uint64_t limit_mb;
	uint32_t base_limit;

	/* Sets the PCI address the window's start decodes to. */
	register_write(host, BRCMSTB_WIN0_LO, (uint32_t)host->config.outbound_pci_base);
	register_write(host, BRCMSTB_WIN0_HI, (uint32_t)(host->config.outbound_pci_base >> 32));

	/* The CPU side is set as the megabytes of its first and last bytes. */
	base_mb = host->config.outbound_cpu_base / BRCMSTB_MEGABYTE;
	limit_mb = (host->config.outbound_cpu_base + host->config.outbound_size - 1U) / BRCMSTB_MEGABYTE;

	/* The low twelve bits of both megabyte numbers share one register. */
	base_limit = ((uint32_t)base_mb << BRCMSTB_WIN0_BASE_SHIFT) & BRCMSTB_WIN0_BASE;
	base_limit |= ((uint32_t)limit_mb << BRCMSTB_WIN0_LIMIT_SHIFT) & BRCMSTB_WIN0_LIMIT;
	register_update(host, BRCMSTB_WIN0_BASE_LIMIT, BRCMSTB_WIN0_BASE | BRCMSTB_WIN0_LIMIT, base_limit);

	/* The higher bits of each go to a register of their own. */
	register_update(host, BRCMSTB_WIN0_BASE_HI, BRCMSTB_WIN0_HI_MASK, (uint32_t)(base_mb >> 12) & BRCMSTB_WIN0_HI_MASK);
	register_update(host, BRCMSTB_WIN0_LIMIT_HI, BRCMSTB_WIN0_HI_MASK, (uint32_t)(limit_mb >> 12) & BRCMSTB_WIN0_HI_MASK);
}

/* Records the negotiated link speed and width. */
static void
report_link(
	struct drv_pci_brcmstb *host)
{
	uint16_t status;

	/* Reads the root port's link status. */
	status = kern_mmio_read16(host->registers + BRCMSTB_RC_LINK_STATUS);
	kern_logf("pcie: brcmstb link up, generation %u, x%u\n",
		  (unsigned)(status & BRCMSTB_LINK_SPEED),
		  (unsigned)((status & BRCMSTB_LINK_WIDTH) >> BRCMSTB_LINK_WIDTH_SHIFT));
}

/*
 * Finds the window through which one function's configuration space is seen.
 *
 * The caller holds the configuration lock, because below the root port the
 * window is selected by writing the index register.  ENODEV means the
 * function cannot exist, and the caller answers as for an absent function.
 */
static int
config_window(
	struct drv_pci_brcmstb *host,
	const struct drv_pci_address *address,
	unsigned offset,
	volatile uint8_t **window)
{
	uint32_t index;

	/* The root complex is bus 0 and has one function, the root port. */
	if (address->bus == BRCMSTB_ROOT_BUS) {
		/* No other device answers on bus 0. */
		if (address->device != 0 || address->function != 0)
			return ENODEV;

		/* Succeeded: the root port's space is the start of the registers. */
		*window = host->registers + offset;
		return 0;
	}

	/* Nothing below the root port answers while the link is down. */
	if (!host->link_up)
		return ENODEV;

	/* The link below the root port leads to device 0 only. */
	if (address->bus == BRCMSTB_SECONDARY_BUS && address->device != 0)
		return ENODEV;

	/* Selects the function and hands out the data window. */
	index = (uint32_t)address->bus << BRCMSTB_EXT_CFG_BUS_SHIFT;
	index |= (uint32_t)address->device << BRCMSTB_EXT_CFG_DEVICE_SHIFT;
	index |= (uint32_t)address->function << BRCMSTB_EXT_CFG_FUNCTION_SHIFT;
	register_write(host, BRCMSTB_EXT_CFG_INDEX, index);

	/* Succeeded: the function's space starts at the data window. */
	*window = host->registers + BRCMSTB_EXT_CFG_DATA + offset;
	return 0;
}

/* Reports whether a configuration access has a valid address, width and alignment. */
static bool
config_access_valid(
	const struct drv_pci_address *address,
	unsigned offset,
	unsigned width)
{
	/* Requires a function address the bus can carry. */
	if (address == NULL)
		return false;
	if (address->segment != 0)
		return false;
	if (address->device >= 32U || address->function >= 8U)
		return false;

	/* Requires a byte, halfword or word inside the 4 KiB space. */
	if (width != 1U && width != 2U && width != 4U)
		return false;
	if (offset >= 4096U || 4096U - offset < width)
		return false;

	/* Requires the access to be aligned to its width. */
	if ((offset & (width - 1U)) != 0)
		return false;

	/* Reports a valid access. */
	return true;
}

/*
 * Reads a function's configuration space for the PCI core.
 *
 * A function that cannot exist reads as all-ones, as an absent function
 * does on a real bus.
 */
static int
host_config_read(
	void *context,
	const struct drv_pci_address *address,
	unsigned offset,
	unsigned width,
	uint32_t *value)
{
	struct drv_pci_brcmstb *host;
	volatile uint8_t *window;
	unsigned long irq;
	bool valid;
	int error;

	/* Refuses a malformed access. */
	host = context;
	valid = config_access_valid(address, offset, width);
	if (!valid || value == NULL)
		return EINVAL;

	/* Selects the function and reads it at the access's own width. */
	irq = spin_lock_irqsave(&host->config_lock);

	/* An impossible function reads as all-ones. */
	error = config_window(host, address, offset, &window);
	if (error != 0) {
		*value = 0xffffffffU;
	} else if (width == 1U) {
		*value = kern_mmio_read8(window);
	} else if (width == 2U) {
		*value = kern_mmio_read16(window);
	} else {
		*value = kern_mmio_read32(window);
	}

	/* Lets the next access select its own function. */
	spin_unlock_irqrestore(&host->config_lock, irq);

	/* Narrows the all-ones of an absent function to the access width. */
	if (error != 0 && width < 4U)
		*value &= (1U << (width * 8U)) - 1U;

	/* Succeeded: value holds the register or all-ones. */
	return 0;
}

/*
 * Writes a function's configuration space for the PCI core.
 *
 * A write to a function that cannot exist is dropped, as on a real bus.
 */
static int
host_config_write(
	void *context,
	const struct drv_pci_address *address,
	unsigned offset,
	unsigned width,
	uint32_t value)
{
	struct drv_pci_brcmstb *host;
	volatile uint8_t *window;
	unsigned long irq;
	bool valid;
	int error;

	/* Refuses a malformed access. */
	host = context;
	valid = config_access_valid(address, offset, width);
	if (!valid)
		return EINVAL;

	/* Selects the function and writes it at the access's own width. */
	irq = spin_lock_irqsave(&host->config_lock);

	/* A write to an impossible function goes nowhere. */
	error = config_window(host, address, offset, &window);
	if (error != 0) {
		/* Drops a write no function would receive. */
	} else if (width == 1U) {
		kern_mmio_write8(window, (uint8_t)value);
	} else if (width == 2U) {
		kern_mmio_write16(window, (uint16_t)value);
	} else {
		kern_mmio_write32(window, value);
	}

	/* Lets the next access select its own function. */
	spin_unlock_irqrestore(&host->config_lock, irq);

	/* Succeeded: the write reached the function or had nowhere to go. */
	return 0;
}

/*
 * Numbers the bus below the root port and places its endpoints' BARs.
 *
 * The root port leads to bus 1 alone.  Every function of device 0 there
 * gets its memory BARs from the start of the outbound window, and the root
 * port forwards the part of the window that was used.
 */
static int
assign_resources(
	struct drv_pci_brcmstb *host)
{
	struct drv_pci_address address;
	bool multifunction;
	unsigned function;
	int error;

	/* Makes bus 1 the root port's secondary and subordinate bus. */
	kern_memset(&address, 0, sizeof(address));
	address.bus = BRCMSTB_ROOT_BUS;
	(void)host_config_write(host, &address, PCI_BRIDGE_BUSES, 4U,
				(uint32_t)BRCMSTB_ROOT_BUS |
				((uint32_t)BRCMSTB_SECONDARY_BUS << 8) |
				((uint32_t)BRCMSTB_SECONDARY_BUS << 16));

	/* Places the BARs of every function of the device on bus 1. */
	address.bus = BRCMSTB_SECONDARY_BUS;
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

	/* Opens the root port's forwarding over what was assigned. */
	configure_root_port(host);

	/* Succeeded: the tree below the root port is ready for enumeration. */
	return 0;
}

/*
 * Places the memory BARs of one function.
 *
 * Multifunction reports, for function 0, whether the device has others.  An
 * absent function or a bridge is left alone.
 */
static int
assign_function(
	struct drv_pci_brcmstb *host,
	const struct drv_pci_address *address,
	bool *multifunction)
{
	uint32_t identity;
	uint32_t header;
	unsigned index;
	bool wide;
	int error;

	/* Skips an absent function. */
	(void)host_config_read(host, address, PCI_VENDOR_ID, 4U, &identity);
	if ((identity & 0xffffU) == 0xffffU)
		return 0;

	/* Learns whether the device has more functions and what this one is. */
	(void)host_config_read(host, address, PCI_HEADER_TYPE, 1U, &header);
	if (address->function == 0 && (header & PCI_HEADER_MULTIFUNCTION) != 0)
		*multifunction = true;

	/* Leaves a bridge below the root port unassigned; the Pi 4 has none. */
	if ((header & PCI_HEADER_LAYOUT) == PCI_HEADER_BRIDGE) {
		kern_logf("pcie: brcmstb bridge at %02x:%02x.%u left unassigned\n",
			  address->bus, address->device, address->function);
		return 0;
	}

	/* Places each BAR; a 64-bit BAR takes the next slot as its upper half. */
	for (index = 0; index < PCI_ENDPOINT_BARS; index++) {
		error = assign_bar(host, address, index, &wide);
		if (error != 0)
			return error;

		/* Skips the upper half of a 64-bit BAR. */
		if (wide)
			index++;
	}

	/* Succeeded: the function's memory BARs are placed. */
	return 0;
}

/*
 * Measures one BAR and places it in the outbound window.
 *
 * Wide reports a 64-bit BAR, whose upper half is the next slot.  An I/O BAR
 * is left unassigned: the controller has no I/O window.
 */
static int
assign_bar(
	struct drv_pci_brcmstb *host,
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

	/* Reads the BAR's kind. */
	offset = PCI_BAR0 + index * 4U;
	*wide = false;
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
	mask = ((uint64_t)high_mask << 32) | (low_mask & PCI_BAR_MEMORY_MASK);
	if ((low_mask & PCI_BAR_MEMORY_MASK) == 0) {
		(void)host_config_write(host, address, offset, 4U, 0);
		return 0;
	}

	/* Aligns the BAR to its own size inside the window. */
	size = ~mask + 1U;
	placed = (host->window_next + size - 1U) & ~(size - 1U);
	window_end = host->config.outbound_pci_base + host->config.outbound_size;
	if (placed < host->window_next ||
	    placed > window_end ||
	    size > window_end - placed) {
		kern_logf("pcie: brcmstb BAR%u of %02x:%02x.%u does not fit the window\n",
			  index, address->bus, address->device, address->function);
		return ENOSPC;
	}

	/* Programs the address; the upper half of a 64-bit BAR is zero. */
	(void)host_config_write(host, address, offset, 4U, (uint32_t)placed);
	if (*wide)
		(void)host_config_write(host, address, offset + 4U, 4U, (uint32_t)(placed >> 32));

	/* Records the space as used. */
	host->window_next = placed + size;
	kern_logf("pcie: brcmstb %02x:%02x.%u BAR%u at %llx (%llu bytes)\n",
		  address->bus, address->device, address->function, index,
		  (unsigned long long)placed, (unsigned long long)size);

	/* Succeeded: the BAR decodes its place in the window. */
	return 0;
}

/*
 * Lets the root port forward the assigned part of the outbound window.
 *
 * The I/O and prefetchable windows stay closed (base above limit).  The root
 * port also becomes a bus master, so devices below it can reach memory.
 */
static void
configure_root_port(
	struct drv_pci_brcmstb *host)
{
	struct drv_pci_address address;
	uint32_t base;
	uint32_t limit;
	uint32_t memory;

	/* Addresses the root port. */
	kern_memset(&address, 0, sizeof(address));
	address.bus = BRCMSTB_ROOT_BUS;

	/* Forwards the used megabytes of the window, or nothing if none was used. */
	memory = 0x0000fff0U;
	if (host->window_next != host->config.outbound_pci_base) {
		base = (uint32_t)(host->config.outbound_pci_base >> 16) & 0xfff0U;
		limit = (uint32_t)((host->window_next - 1U) >> 16) & 0xfff0U;
		memory = base | (limit << 16);
	}

	/* Programs the memory window. */
	(void)host_config_write(host, &address, PCI_BRIDGE_MEMORY, 4U, memory);

	/* Closes the prefetchable and I/O windows. */
	(void)host_config_write(host, &address, PCI_BRIDGE_PREFETCH, 4U, 0x0000fff0U);
	(void)host_config_write(host, &address, PCI_BRIDGE_PREFETCH_BASE_HI, 4U, 0);
	(void)host_config_write(host, &address, PCI_BRIDGE_PREFETCH_LIMIT_HI, 4U, 0);
	(void)host_config_write(host, &address, PCI_BRIDGE_IO_BASE, 1U, 0xf0U);
	(void)host_config_write(host, &address, PCI_BRIDGE_IO_LIMIT, 1U, 0);
	(void)host_config_write(host, &address, PCI_BRIDGE_IO_HI, 4U, 0);

	/* Enables forwarding of memory and of the devices' DMA. */
	(void)host_config_write(host, &address, PCI_COMMAND, 2U, PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER);
}

/*
 * Maps a memory BAR for its driver.
 *
 * The BAR's PCI address lies in the outbound window; the CPU reaches it at
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
	struct drv_pci_brcmstb *host;
	uint64_t window_end;
	uint64_t cpu_address;
	void *address;
	int error;

	/* Every BAR of this host is mapped the same way, whatever the flags ask. */
	(void)device;
	(void)flags;

	/* Refuses an I/O BAR, an empty one and missing storage. */
	host = context;
	if (bar == NULL || mapping == NULL)
		return EINVAL;
	if (bar->type == DRV_PCI_BAR_IO ||
	    bar->type == DRV_PCI_BAR_NONE ||
	    bar->size == 0)
		return EINVAL;

	/* Refuses a BAR outside the outbound window. */
	window_end = host->config.outbound_pci_base + host->config.outbound_size;
	if (bar->bus_address < host->config.outbound_pci_base)
		return EINVAL;
	if (bar->bus_address >= window_end || bar->size > window_end - bar->bus_address)
		return EINVAL;

	/* Maps the CPU side of the BAR as uncached device memory. */
	cpu_address = host->config.outbound_cpu_base + (bar->bus_address - host->config.outbound_pci_base);
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
	(void)context;

	/* Ignores storage that holds no mapping of ours. */
	if (mapping == NULL || mapping->private_data[0] != 1U)
		return;

	/* Releases the window; arm64 keeps its alias, so a refusal changes nothing. */
	(void)kern_device_unmap(mapping->address, mapping->size);
	kern_memset(mapping, 0, sizeof(*mapping));
}

/*
 * Gives a device its legacy interrupt.
 *
 * MSI is not offered: the controller's MSI receiver needs HAL support that
 * does not exist yet.  The device's pin is swizzled across each bridge
 * between it and the root port, then mapped through the device tree's
 * interrupt map.
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
	struct drv_pci_brcmstb *host;
	struct drv_pci_address address;
	struct drv_pci_device *walk;
	struct drv_pci_bus *bus;
	struct drv_pci_bus *above;
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
	if (pin == 0 || pin > DRV_PCI_BRCMSTB_INTX_COUNT)
		return ENODEV;

	/* Swizzles the pin by the device number at each bus below the root bus. */
	walk = device;
	for (;;) {
		/* Stops at the root bus, where the interrupt map applies. */
		bus = drv_pci_device_bus(walk);
		above = drv_pci_bus_parent(bus);
		if (above == NULL)
			break;

		/* Rotates the pin by the device's slot and moves to the bridge above. */
		drv_pci_device_address(walk, &address);
		pin = (uint8_t)(((pin - 1U + address.device) % DRV_PCI_BRCMSTB_INTX_COUNT) + 1U);
		walk = drv_pci_bus_bridge(bus);
	}

	/* Refuses a pin the interrupt map does not route. */
	if (host->config.intx_irq[pin - 1U] == 0)
		return ENODEV;

	/* Succeeded: the device's line is the mapped interrupt. */
	irqs[0].type = DRV_PCI_IRQ_INTX;
	irqs[0].index = 0;
	irqs[0].vector = host->config.intx_irq[pin - 1U];
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
	(void)context;
	(void)device;
	(void)irqs;
	(void)count;
}
