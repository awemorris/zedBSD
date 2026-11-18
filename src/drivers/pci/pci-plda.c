/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * PLDA PCIe root complex, as the StarFive JH7110 has two of.
 *
 * The driver opens the configuration window and the memory windows with the
 * block's address translation entries, makes the block a root port, holds
 * the endpoint in reset while it does, and trains the link.  The root port
 * leads to bus 1 alone, whose device 0 gets its memory BARs from the start
 * of the 32-bit window.  Devices below reach memory below 4 GiB.
 *
 * The four legacy pins, the message interrupts and the block's own events
 * share one interrupt line.  The PCI core hands each pin's line to the
 * host's pin hooks, and, when the device tree calls the block an MSI
 * controller, each function's message to the host's message hooks.  The
 * host's handler reads the status, runs each raised pin's line without its
 * own lock held, acknowledges the status after every handler, then runs
 * each message that arrived, and ends the interrupt once.  Events stay
 * masked, and messages are let through only while one is established.
 */

#include <drivers/pci/pci-plda.h>
#include <drivers/generic/dma.h>
#include <kern/clock.h>
#include <kern/device-io.h>
#include <kern/irq.h>
#include <kern/kcrt.h>
#include <kern/klog.h>
#include <kern/kmem.h>
#include <kern/lock.h>
#include <kern/pmem.h>
#include <uapi/errno.h>

/*
 * Value(s) referenced: OpenBSD sys/dev/fdt/stfpcie.c 1.4 (ISC): the
 * register block's offsets and bits below -- the root port type, the
 * class code word, the function disable, the 64-bit prefetchable window,
 * the local interrupt mask and status with the pins at bits 24 to 27, the
 * LTR message bit and the slave address translation entries.
 */
#define PLDA_GEN_SETTINGS		0x080U
#define PLDA_GEN_ROOT_PORT		0x00000001U
#define PLDA_CLASS_WORD			0x09cU
#define PLDA_FUNCTION_WORD		0x0b4U
#define PLDA_FUNCTION_DISABLE		0x00008000U
#define PLDA_BAR_WINDOW			0x0fcU
#define PLDA_BAR_WINDOW_PREFETCH64	0x00000008U
#define PLDA_INTERRUPT_MASK		0x180U
#define PLDA_INTERRUPT_STATUS		0x184U
#define PLDA_INTERRUPT_PIN_SHIFT	24U
#define PLDA_MESSAGE_SUPPORT		0x3f0U
#define PLDA_MESSAGE_LTR		0x00000004U
#define PLDA_ATR_PARAMETER(n)		(0x800U + (n) * 0x20U)
#define PLDA_ATR_SOURCE_HIGH(n)		(0x804U + (n) * 0x20U)
#define PLDA_ATR_TARGET_LOW(n)		(0x808U + (n) * 0x20U)
#define PLDA_ATR_TARGET_HIGH(n)		(0x80cU + (n) * 0x20U)
#define PLDA_ATR_TARGET_KIND(n)		(0x810U + (n) * 0x20U)
#define PLDA_ATR_IMPLEMENTED		0x00000001U
#define PLDA_ATR_SIZE_SHIFT		1U
#define PLDA_ATR_KIND_MEMORY		0U
#define PLDA_ATR_KIND_CONFIG		1U

/*
 * Value(s) referenced: OpenBSD sys/arch/riscv64/dev/stfpcie.c 1.4 (ISC),
 * its message interrupt part (under "notyet" there): the message bit 28
 * of the local interrupt mask and status, the message address register,
 * the message status register with one bit per vector, 32 vectors, and
 * the data a function writes being its vector.  Linux
 * drivers/pci/controller/plda/pcie-plda.h (GPL-2.0), values only --
 * IMSI_ADDR 0x190, ISTATUS_MSI 0x194, PM_MSI_INT_MSI bit 28,
 * PLDA_MAX_NUM_MSI_IRQS 32 -- the same values, read to check them, and
 * the message address 0x190 its StarFive host gives functions when the
 * message address register is not read (the JH7110's reads 0, seen on the
 * VisionFive 2 on 2026-10-26); no code is taken from it (the user,
 * 2026-10-26: "Linux も値だけ参考").
 */
#define PLDA_INTERRUPT_MSI		0x10000000U
#define PLDA_MSI_ADDRESS		0x190U
#define PLDA_MSI_STATUS			0x194U
#define PLDA_MSI_VECTORS		32U
#define PLDA_MSI_DEFAULT_ADDRESS	0x190U

/*
 * How many times one interrupt reads the message status, how many times a
 * handler that found no work is run again, and how many register reads
 * the wait between those runs is.
 */
#define PLDA_MSI_PASSES			8U
#define PLDA_MSI_RETRIES		8U
#define PLDA_MSI_SETTLE_READS		16U

/* The capability that describes a function's MSI. */
#define PCI_CAPABILITY_MSI		0x05U

/* The bytes of the register block the driver reaches: up to the last entry. */
#define PLDA_REGISTER_SPAN		0x1000U

/* The class code word that makes the root port a PCI-to-PCI bridge. */
#define PLDA_CLASS_BRIDGE		0x06040000U

/* The root port's functions beyond the first, which are switched off. */
#define PLDA_EXTRA_FUNCTIONS		3U

/*
 * Values referenced: PCI Express Base Specification, Revision 4.0,
 * section 7.2.2 (the enhanced configuration access layout) and section
 * 6.6.1 (PERST# held for 100 ms after power is stable, and 100 ms from
 * the link coming up before the first configuration request).
 */
#define PLDA_ECAM_BUS_SHIFT		20U
#define PLDA_ECAM_DEVICE_SHIFT		15U
#define PLDA_ECAM_FUNCTION_SHIFT	12U
#define PLDA_ECAM_BUSES			2U
#define PLDA_FUNCTION_SPACE		4096U
#define PLDA_RESET_US			100000U
#define PLDA_SETTLE_US			100000U
#define PLDA_LINK_POLL_US		1000U
#define PLDA_LINK_POLLS			100U

/* The root port's bus, and the one bus below it. */
#define PLDA_ROOT_BUS			0U
#define PLDA_SECONDARY_BUS		1U

/* Standard configuration header offsets and bits. */
#define PCI_VENDOR_ID			0x00U
#define PCI_COMMAND			0x04U
#define PCI_CLASS_REVISION		0x08U
#define PCI_HEADER_TYPE			0x0eU
#define PCI_BAR0			0x10U
#define PCI_BAR1_END			0x18U
#define PCI_BRIDGE_BUSES		0x18U
#define PCI_BRIDGE_IO_BASE		0x1cU
#define PCI_BRIDGE_IO_LIMIT		0x1dU
#define PCI_BRIDGE_MEMORY		0x20U
#define PCI_BRIDGE_PREFETCH		0x24U
#define PCI_BRIDGE_PREFETCH_BASE_HI	0x28U
#define PCI_BRIDGE_PREFETCH_LIMIT_HI	0x2cU
#define PCI_BRIDGE_IO_HI		0x30U
#define PCI_INTERRUPT_PIN		0x3dU
#define PCI_COMMAND_MEMORY		0x0002U
#define PCI_COMMAND_MASTER		0x0004U
#define PCI_HEADER_LAYOUT		0x7fU
#define PCI_HEADER_MULTIFUNCTION	0x80U
#define PCI_HEADER_ENDPOINT		0x00U
#define PCI_BAR_IO			0x00000001U
#define PCI_BAR_TYPE			0x00000006U
#define PCI_BAR_TYPE_64			0x00000004U
#define PCI_BAR_MEMORY_MASK		0xfffffff0U
#define PCI_ENDPOINT_BARS		6U

/*
 * Values referenced: eXtensible Host Controller Interface for USB, Revision
 * 1.2, sections 5.3.1 and 5.4.2 (CAPLENGTH, USBSTS.HCHalted), and NVM
 * Express Base Specification 2.0, section 3.1.3 (CC.EN at 0x14, CSTS.RDY
 * at 0x1c): what the firmware left the endpoint doing.
 */
#define PCI_CLASS_XHCI			0x0c0330U
#define PCI_CLASS_NVME			0x010802U
#define XHCI_USBSTS			0x04U
#define XHCI_HALTED			0x00000001U
#define NVME_CC				0x14U
#define NVME_CSTS			0x1cU
#define NVME_ENABLE			0x00000001U
#define ENDPOINT_PROBE_SPAN		0x1000U

/*
 * Values referenced: IEEE Std 1275-1994 PCI Bus Binding, revision 2.1,
 * section 2.2.1 (the three cells of a PCI address, the space code and the
 * prefetchable bit in the first one) and the Devicetree Specification
 * v0.4, section 2.4.3 (interrupt-map and interrupt-map-mask).
 */
#define FDT_PCI_SPACE_SHIFT		24U
#define FDT_PCI_SPACE_MASK		3U
#define FDT_PCI_SPACE_MEMORY32		2U
#define FDT_PCI_SPACE_MEMORY64		3U
#define FDT_PCI_PREFETCHABLE		0x40000000U
#define FDT_PCI_ADDRESS_CELLS		3U
#define FDT_PCI_ADDRESS_LOW_CELL	1U

/* What devices behind the host reach with DMA: memory below 4 GiB. */
#define PLDA_DMA_ADDRESS_BITS		32U
#define PLDA_DMA_MAX_SEGMENT		(16U * 1024U * 1024U)

/* How long a pin's removal waits for interrupts still running it: one second. */
#define PLDA_DRAIN_POLL_US		1000U
#define PLDA_DRAIN_POLLS		1000U

/* The space below 4 GiB, where the BARs are placed. */
#define PLDA_32BIT_LIMIT		(UINT64_C(1) << 32)

/* Marks a parameter that an operation of the PCI core passes but this host does not need. */
#define UNUSED_PARAMETER(parameter)	((void)(parameter))

/*
 * One pin's entry in the host's table.
 *
 * run and line are empty while no PCI core line owns the pin.  in_flight
 * counts the interrupts that took the entry and have not yet come back
 * from run; a removal waits for it to reach zero.
 */
struct plda_pin {
	drv_pci_intx_run_t run;
	void *line;
	unsigned in_flight;
};

/*
 * One message vector's entry in the host's table.
 *
 * run and argument are empty while no PCI core cookie owns the vector;
 * in_flight is as for a pin.
 */
struct plda_message {
	drv_pci_msi_run_t run;
	void *argument;
	unsigned in_flight;
};

/*
 * One root complex the driver has started.
 *
 * It lives from a successful start for as long as the kernel runs: the PCI
 * core keeps it as the host of the root bus.  The lock guards the pin
 * table, the enabled pins and the interrupt mask register, and is always
 * taken with interrupts off.  Window next is where the next BAR goes in
 * the 32-bit window.
 */
struct drv_pci_plda {
	struct drv_pci_plda_config config;
	struct drv_pci_plda_board board;
	volatile uint8_t *registers;
	volatile uint8_t *config_space;
	struct spinlock lock;
	struct plda_pin pins[DRV_PCI_PLDA_INTX_COUNT];
	uint32_t enabled_pins;
	struct plda_message messages[PLDA_MSI_VECTORS];
	unsigned messages_used;
	uint32_t msi_address;
	bool msi_ready;
	bool late_message_noted;
	const struct drv_pci_plda_window *bar_window;
	uint64_t window_next;
	struct drv_dma_device *dma;
	struct drv_pci_bus *root;
	bool link_up;
	bool line_installed;
};

static int describe_windows(const struct drv_fdt *fdt, uint32_t node, struct drv_pci_plda_config *config);
static int describe_interrupts(const struct drv_fdt *fdt, uint32_t node, struct drv_pci_plda_config *config);
static int size_log2(uint64_t size, unsigned *log2);
static int check_config(const struct drv_pci_plda_config *config);
static uint32_t register_read(struct drv_pci_plda *host, unsigned offset);
static void register_write(struct drv_pci_plda *host, unsigned offset, uint32_t value);
static void register_update(struct drv_pci_plda *host, unsigned offset, uint32_t clear, uint32_t set);
static void set_translation(struct drv_pci_plda *host, unsigned entry, uint64_t cpu_base, uint64_t size, uint64_t pci_base, uint32_t kind);
static void set_translations(struct drv_pci_plda *host);
static void report_endpoint(struct drv_pci_plda *host);
static void configure_root_complex(struct drv_pci_plda *host);
static void train_link(struct drv_pci_plda *host);
static int config_address(struct drv_pci_plda *host, const struct drv_pci_address *address, unsigned offset, unsigned width, volatile uint8_t **target);
static bool root_port_bar(const struct drv_pci_address *address, unsigned offset);
static int host_config_read(void *context, const struct drv_pci_address *address, unsigned offset, unsigned width, uint32_t *value);
static int host_config_write(void *context, const struct drv_pci_address *address, unsigned offset, unsigned width, uint32_t value);
static int assign_resources(struct drv_pci_plda *host);
static int assign_function(struct drv_pci_plda *host, const struct drv_pci_address *address, bool *multifunction);
static int assign_bar(struct drv_pci_plda *host, const struct drv_pci_address *address, unsigned index, bool *wide);
static void configure_root_port(struct drv_pci_plda *host);
static int host_map_bar(void *context, struct drv_pci_device *device, const struct drv_pci_bar *bar, unsigned flags, struct drv_pci_mapping *mapping);
static void host_unmap_bar(void *context, struct drv_pci_mapping *mapping);
static int host_allocate_irqs(void *context, struct drv_pci_device *device, enum drv_pci_irq_type type, unsigned minimum, unsigned maximum, struct drv_pci_irq *irqs, unsigned *count);
static void host_free_irqs(void *context, struct drv_pci_device *device, struct drv_pci_irq *irqs, unsigned count);
static int host_intx_establish(void *context, unsigned vector, drv_pci_intx_run_t run, void *line);
static int host_intx_disestablish(void *context, unsigned vector, drv_pci_intx_run_t run, void *line);
static void host_intx_mask(void *context, unsigned vector);
static void host_intx_unmask(void *context, unsigned vector);
static int host_msi_establish(void *context, struct drv_pci_device *device, drv_pci_msi_run_t run, void *argument, unsigned *handle, uint64_t *address, uint32_t *data);
static int host_msi_disestablish(void *context, unsigned handle);
static void settle(struct drv_pci_plda *host);
static void note_late_message(struct drv_pci_plda *host, unsigned vector, unsigned retries);
static void serve_messages(struct drv_pci_plda *host);
static void start_messages(struct drv_pci_plda *host);
static void write_interrupt_mask(struct drv_pci_plda *host);
static void host_interrupt(int irq, kern_irq_ack_t acknowledge, void *argument);
static int install_line(struct drv_pci_plda *host);

/*
 * The operations the PCI core calls on the root bus and every bus below it.
 *
 * The table is constant; the host each call works on arrives as its
 * context.  The pin hooks make the core hand each pin to this host rather
 * than to the interrupt controller.
 */
static const struct drv_pci_bus_ops plda_bus_ops = {
	.config_space_size = PLDA_FUNCTION_SPACE,
	.config_read = host_config_read,
	.config_write = host_config_write,
	.map_bar = host_map_bar,
	.unmap_bar = host_unmap_bar,
	.allocate_irqs = host_allocate_irqs,
	.free_irqs = host_free_irqs,
	.intx_establish = host_intx_establish,
	.intx_disestablish = host_intx_disestablish,
	.intx_mask = host_intx_mask,
	.intx_unmask = host_intx_unmask,
	.msi_establish = host_msi_establish,
	.msi_disestablish = host_msi_disestablish
};

/*
 * Describes a root complex from its device tree node.
 *
 * Reports ENODEV for a node the tree marks as not present.
 */
int
drv_pci_plda_describe(
	const struct drv_fdt *fdt,
	uint32_t node,
	struct drv_pci_plda_config *config)
{
	const uint8_t *value;
	uint32_t length;
	unsigned config_index;
	unsigned register_index;
	bool enabled;
	int error;

	/* Refuses missing arguments. */
	if (fdt == NULL || config == NULL)
		return EINVAL;

	/* Leaves a disabled root complex alone. */
	enabled = drv_fdt_node_enabled(fdt, node);
	if (!enabled)
		return ENODEV;

	/* Starts with an empty configuration: no segment, window or pin. */
	kern_memset(config, 0, sizeof(*config));

	/* Finds the configuration window and the register block by name, or in that order. */
	config_index = 0;
	register_index = 1;
	error = drv_fdt_string_index(fdt, node, "reg-names", "cfg", &config_index);
	if (error != 0)
		config_index = 0;
	error = drv_fdt_string_index(fdt, node, "reg-names", "apb", &register_index);
	if (error != 0)
		register_index = 1;

	/* Reads the configuration window. */
	error = drv_fdt_reg(fdt, node, config_index, &config->config_base, &config->config_size);
	if (error != 0)
		return error;

	/* Reads the register block. */
	error = drv_fdt_reg(fdt, node, register_index, &config->register_base, &config->register_size);
	if (error != 0)
		return error;

	/* Reads the interrupt line the pins and events share. */
	error = drv_fdt_property(fdt, node, "interrupts", &value, &length);
	if (error != 0)
		return error;
	if (length < 4U)
		return EINVAL;
	config->irq = (unsigned)drv_fdt_cells_value(value, 0, 1U);

	/* Takes the PCI domain as the segment; without one it is 0. */
	error = drv_fdt_property(fdt, node, "linux,pci-domain", &value, &length);
	if (error == 0 && length >= 4U)
		config->segment = (unsigned)drv_fdt_cells_value(value, 0, 1U);

	/* Reads the memory windows. */
	error = describe_windows(fdt, node, config);
	if (error != 0)
		return error;

	/* Reads the host's own number for each pin. */
	error = describe_interrupts(fdt, node, config);
	if (error != 0)
		return error;

	/* Learns whether the block is to receive message interrupts. */
	error = drv_fdt_property(fdt, node, "msi-controller", &value, &length);
	config->msi = error == 0;

	/* Succeeded: the configuration describes the root complex. */
	return 0;
}

/*
 * Configures a root complex, resets its endpoint and trains the link.
 *
 * On success the host is ready for drv_pci_plda_publish().  The firmware
 * may have left the link up and the endpoint running; the driver reports
 * what the endpoint was doing and resets it all the same.
 */
int
drv_pci_plda_start(
	const struct drv_pci_plda_config *config,
	const struct drv_pci_plda_board *board,
	struct drv_pci_plda **result)
{
	struct drv_pci_plda *host;
	void *registers;
	void *config_space;
	int error;

	/* Refuses missing arguments and a board without its operations. */
	if (config == NULL || board == NULL || result == NULL)
		return EINVAL;
	if (board->perst == NULL || board->link_active == NULL || board->select_function == NULL)
		return EINVAL;

	/* Refuses windows the translation entries cannot express. */
	error = check_config(config);
	if (error != 0)
		return error;

	/* Allocates the host record. */
	host = kern_malloc(sizeof(*host));
	if (host == NULL)
		return ENOMEM;

	/* Starts the record with the configuration, the board and every pin empty and masked. */
	kern_memset(host, 0, sizeof(*host));
	host->config = *config;
	host->board = *board;
	spin_init(&host->lock, LOCK_RANK_DEVICE, "plda pins");

	/* Maps the register block. */
	error = kern_device_map(config->register_base, PLDA_REGISTER_SPAN, KERN_DEVICE_UNCACHED, &registers);
	if (error != 0) {
		kern_free(host);
		return error;
	}

	/* Keeps the register block's mapping. */
	host->registers = registers;

	/* Maps the configuration space of the root port's bus and the one below. */
	error = kern_device_map(config->config_base, (size_t)PLDA_ECAM_BUSES << PLDA_ECAM_BUS_SHIFT, KERN_DEVICE_UNCACHED, &config_space);
	if (error != 0) {
		(void)kern_device_unmap(registers, PLDA_REGISTER_SPAN);
		kern_free(host);
		return error;
	}

	/* Keeps the configuration space's mapping and reports where the block is. */
	host->config_space = config_space;
	kern_logf("pcie%u: PLDA at %llx, configuration %llx, interrupt %u\n",
		  config->segment,
		  (unsigned long long)config->register_base,
		  (unsigned long long)config->config_base,
		  config->irq);

	/* Masks and acknowledges every interrupt of the block. */
	register_write(host, PLDA_INTERRUPT_MASK, 0);
	register_write(host, PLDA_INTERRUPT_STATUS, 0xffffffffU);

	/* Opens the configuration and memory windows. */
	set_translations(host);

	/* Readies the message interrupts when the block is an MSI controller. */
	start_messages(host);

	/* Reports what the firmware left the endpoint doing. */
	report_endpoint(host);

	/* Makes the block a root port with the endpoint held in reset, then trains the link. */
	configure_root_complex(host);
	train_link(host);

	/* Places the endpoint's BARs below a live link. */
	if (host->link_up) {
		error = assign_resources(host);
		if (error != 0)
			kern_logf("pcie%u: BAR placement failed (%d)\n", config->segment, error);
	}

	/* Takes the block's interrupt line. */
	error = install_line(host);
	if (error != 0) {
		host->board.perst(host->board.context, 1);
		(void)kern_device_unmap(config_space, (size_t)PLDA_ECAM_BUSES << PLDA_ECAM_BUS_SHIFT);
		(void)kern_device_unmap(registers, PLDA_REGISTER_SPAN);
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
drv_pci_plda_publish(
	struct drv_pci_plda *host)
{
	struct drv_dma_constraints constraints;
	int error;

	/* Refuses a missing host and a second publication. */
	if (host == NULL)
		return EINVAL;
	if (host->root != NULL)
		return EBUSY;

	/*
	 * Keeps DMA below 4 GiB.  The device tree names no inbound window and
	 * the driver writes none; memory below 4 GiB is what the root complex
	 * is known to reach.  There is no translation between the bus and
	 * memory.
	 */
	kern_memset(&constraints, 0, sizeof(constraints));
	constraints.address_bits = PLDA_DMA_ADDRESS_BITS;
	constraints.max_segment_size = PLDA_DMA_MAX_SEGMENT;
	constraints.segment_boundary = 0;
	constraints.coherent = host->config.coherent ? 1 : 0;
	error = drv_dma_device_create(&constraints, &host->dma);
	if (error != 0)
		return error;

	/* Creates bus 0, whose only function is the root port. */
	error = drv_pci_bus_create_root((uint16_t)host->config.segment, PLDA_ROOT_BUS, &plda_bus_ops, host, host->dma, &host->root);
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
 * Reads the memory windows from the node's ranges.
 *
 * I/O space is skipped: nothing below the root complex needs it.
 */
static int
describe_windows(
	const struct drv_fdt *fdt,
	uint32_t node,
	struct drv_pci_plda_config *config)
{
	struct drv_pci_plda_window *window;
	const uint8_t *value;
	uint32_t length;
	uint32_t parent;
	uint32_t parent_cells;
	uint32_t size_cells;
	uint32_t entry_cells;
	uint32_t entry;
	uint32_t flags;
	uint32_t space;
	uint64_t parent_address;
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

	/* Records each memory window. */
	entry_cells = FDT_PCI_ADDRESS_CELLS + parent_cells + size_cells;
	for (entry = 0; (entry + 1U) * entry_cells * 4U <= length; entry++) {
		/* Skips anything but memory space. */
		flags = (uint32_t)drv_fdt_cells_value(value, entry * entry_cells, 1U);
		space = (flags >> FDT_PCI_SPACE_SHIFT) & FDT_PCI_SPACE_MASK;
		if (space != FDT_PCI_SPACE_MEMORY32 && space != FDT_PCI_SPACE_MEMORY64)
			continue;

		/* Refuses more memory windows than the configuration holds. */
		if (config->window_count == DRV_PCI_PLDA_WINDOW_MAX)
			return ENOSPC;

		/* Decodes the bus address, the parent-bus address and the size. */
		window = &config->windows[config->window_count];
		window->pci_base = drv_fdt_cells_value(value, entry * entry_cells + FDT_PCI_ADDRESS_LOW_CELL, 2U);
		parent_address = drv_fdt_cells_value(value, entry * entry_cells + FDT_PCI_ADDRESS_CELLS, parent_cells);
		window->size = drv_fdt_cells_value(value, entry * entry_cells + FDT_PCI_ADDRESS_CELLS + parent_cells, size_cells);
		window->wide = space == FDT_PCI_SPACE_MEMORY64;
		window->prefetchable = (flags & FDT_PCI_PREFETCHABLE) != 0;

		/* Moves the parent-bus address into the CPU's space. */
		error = drv_fdt_translate(fdt, node, parent_address, &window->cpu_base);
		if (error != 0)
			return error;

		/* Keeps the window. */
		config->window_count++;
	}

	/* Refuses a root complex without a memory window. */
	if (config->window_count == 0)
		return ENOENT;

	/* Succeeded: every memory window is recorded. */
	return 0;
}

/*
 * Reads the interrupt map: the host's own number for each pin.
 *
 * The map names the root complex's own interrupt controller, whose one
 * cell is the pin's number there.  Only entries for the root port's slot
 * are kept; the mask of the board's map leaves the slot out.
 */
static int
describe_interrupts(
	const struct drv_fdt *fdt,
	uint32_t node,
	struct drv_pci_plda_config *config)
{
	const uint8_t *value;
	uint32_t length;
	uint32_t devfn_mask;
	uint32_t pin_mask;
	uint32_t child_cells;
	uint32_t position;
	uint32_t phandle;
	uint32_t controller;
	uint32_t controller_address_cells;
	uint32_t controller_interrupt_cells;
	uint32_t devfn;
	uint32_t pin;
	unsigned vector;
	int error;

	/* Reads the masks; without them every bit of the key counts. */
	devfn_mask = UINT32_MAX;
	pin_mask = UINT32_MAX;
	error = drv_fdt_property(fdt, node, "interrupt-map-mask", &value, &length);
	if (error == 0 && length >= (FDT_PCI_ADDRESS_CELLS + 1U) * 4U) {
		devfn_mask = (uint32_t)drv_fdt_cells_value(value, 0, 1U);
		pin_mask = (uint32_t)drv_fdt_cells_value(value, FDT_PCI_ADDRESS_CELLS, 1U);
	}

	/* A root complex without a map has no legacy interrupts. */
	error = drv_fdt_property(fdt, node, "interrupt-map", &value, &length);
	if (error != 0)
		return 0;

	/*
	 * Walks the entries: the child key, the controller's phandle, the
	 * controller's unit address and its one-cell specifier.
	 */
	child_cells = FDT_PCI_ADDRESS_CELLS + drv_fdt_node_cells(fdt, node, "#interrupt-cells", 1U);
	position = 0;
	while ((position + child_cells + 1U) * 4U <= length) {
		/* Finds the controller the entry names. */
		phandle = (uint32_t)drv_fdt_cells_value(value, position + child_cells, 1U);
		error = drv_fdt_find_phandle(fdt, phandle, &controller);
		if (error != 0)
			return error;
		/* Refuses a controller whose specifier is not one cell, or an entry that runs off the map. */
		controller_address_cells = drv_fdt_node_cells(fdt, controller, "#address-cells", 0);
		controller_interrupt_cells = drv_fdt_node_cells(fdt, controller, "#interrupt-cells", 0);
		if (controller_interrupt_cells != 1U)
			return ENOTSUP;
		if ((position + child_cells + 1U + controller_address_cells + 1U) * 4U > length)
			return EINVAL;

		/* Decodes the key and the host's number for the pin. */
		devfn = (uint32_t)drv_fdt_cells_value(value, position, 1U) & devfn_mask;
		pin = (uint32_t)drv_fdt_cells_value(value, position + child_cells - 1U, 1U) & pin_mask;
		vector = (unsigned)drv_fdt_cells_value(value, position + child_cells + 1U + controller_address_cells, 1U);

		/* Keeps the root port slot's pins that name one of the host's four. */
		if (devfn == 0 &&
		    pin >= 1U && pin <= DRV_PCI_PLDA_INTX_COUNT &&
		    vector >= 1U && vector <= DRV_PCI_PLDA_INTX_COUNT)
			config->intx_vector[pin - 1U] = vector;

		/* Moves to the next entry. */
		position += child_cells + 1U + controller_address_cells + 1U;
	}

	/* Succeeded: the pins the map routes are recorded. */
	return 0;
}

/*
 * Reports the base-2 logarithm of a window size.
 *
 * A translation entry covers a power of two of at least 4 KiB.
 */
static int
size_log2(
	uint64_t size,
	unsigned *log2)
{
	unsigned bits;

	/* Refuses a size that is not a power of two or is below one page. */
	if (size < PLDA_FUNCTION_SPACE)
		return EINVAL;
	if ((size & (size - 1U)) != 0)
		return EINVAL;

	/* Counts the zero bits below the one set bit. */
	bits = 0;
	while ((size >> bits) != 1U)
		bits++;

	/* Succeeded: log2 is the window's size exponent. */
	*log2 = bits;
	return 0;
}

/*
 * Checks that every window fits a translation entry and the BARs a window.
 */
static int
check_config(
	const struct drv_pci_plda_config *config)
{
	const struct drv_pci_plda_window *window;
	unsigned log2;
	unsigned index;
	int error;

	/* The configuration window must hold the two buses used. */
	error = size_log2(config->config_size, &log2);
	if (error != 0)
		return error;
	if (log2 < PLDA_ECAM_BUS_SHIFT + 1U)
		return EINVAL;

	/* Every memory window must be a power of two the CPU and the bus align to. */
	for (index = 0; index < config->window_count; index++) {
		window = &config->windows[index];
		error = size_log2(window->size, &log2);
		if (error != 0)
			return error;
		if ((window->cpu_base & (window->size - 1U)) != 0)
			return EINVAL;
	}

	/* Succeeded: the translation entries can express the windows. */
	return 0;
}

/* Reads one register of the block. */
static uint32_t
register_read(
	struct drv_pci_plda *host,
	unsigned offset)
{
	uint32_t value;

	/* Reads the 32-bit register. */
	value = kern_mmio_read32(host->registers + offset);

	/* Succeeded: the register's value. */
	return value;
}

/* Writes one register of the block. */
static void
register_write(
	struct drv_pci_plda *host,
	unsigned offset,
	uint32_t value)
{
	/* Writes the 32-bit register. */
	kern_mmio_write32(host->registers + offset, value);
}

/* Clears and sets bits of one register of the block. */
static void
register_update(
	struct drv_pci_plda *host,
	unsigned offset,
	uint32_t clear,
	uint32_t set)
{
	uint32_t value;

	/* Reads, changes and writes back the register. */
	value = register_read(host, offset);
	value = (value & ~clear) | set;
	register_write(host, offset, value);
}

/*
 * Writes one slave address translation entry.
 *
 * The entry sends CPU accesses to the window at cpu_base, of size bytes,
 * to pci_base on the bus as memory or configuration requests.
 *
 * Procedure referenced: OpenBSD sys/dev/fdt/stfpcie.c 1.4 (ISC), the
 * attach's address translation: the source address low word carries the
 * implemented bit and the size exponent, then the source high word, the
 * translated address and the request kind.
 */
static void
set_translation(
	struct drv_pci_plda *host,
	unsigned entry,
	uint64_t cpu_base,
	uint64_t size,
	uint64_t pci_base,
	uint32_t kind)
{
	uint32_t parameter;
	unsigned log2;

	/* The size was checked with the configuration. */
	log2 = 0;
	(void)size_log2(size, &log2);

	/* Describes the source window: its low address bits, enabled, and its size. */
	parameter = (uint32_t)cpu_base | PLDA_ATR_IMPLEMENTED | ((uint32_t)log2 << PLDA_ATR_SIZE_SHIFT);
	register_write(host, PLDA_ATR_PARAMETER(entry), parameter);
	register_write(host, PLDA_ATR_SOURCE_HIGH(entry), (uint32_t)(cpu_base >> 32));

	/* Names the address on the bus and the kind of request. */
	register_write(host, PLDA_ATR_TARGET_LOW(entry), (uint32_t)pci_base);
	register_write(host, PLDA_ATR_TARGET_HIGH(entry), (uint32_t)(pci_base >> 32));
	register_write(host, PLDA_ATR_TARGET_KIND(entry), kind);
}

/*
 * Opens the configuration window as entry 0 and each memory window after it.
 *
 * The windows are the same ones the firmware opens, so rewriting them
 * while the endpoint still runs moves nothing.
 */
static void
set_translations(
	struct drv_pci_plda *host)
{
	const struct drv_pci_plda_window *window;
	unsigned index;

	/* Sends the configuration window to configuration requests from bus 0. */
	set_translation(host, 0, host->config.config_base, host->config.config_size, 0, PLDA_ATR_KIND_CONFIG);

	/* Sends each memory window to its own bus addresses. */
	for (index = 0; index < host->config.window_count; index++) {
		window = &host->config.windows[index];
		set_translation(host, index + 1U, window->cpu_base, window->size, window->pci_base, PLDA_ATR_KIND_MEMORY);
	}
}

/*
 * Logs what the firmware left the endpoint below the link doing.
 *
 * The firmware may have started a USB or an NVMe controller and left it
 * running.  The endpoint is reset afterwards in any case; the line only
 * records whether the firmware stopped it before handing over.
 */
static void
report_endpoint(
	struct drv_pci_plda *host)
{
	struct drv_pci_address address;
	const struct drv_pci_plda_window *window;
	volatile uint8_t *probe;
	uint32_t buses;
	uint32_t identity;
	uint32_t class_code;
	uint32_t command;
	uint32_t bar;
	uint32_t bar_high;
	uint32_t value;
	uint32_t ready;
	uint64_t bus_address;
	uint64_t cpu_address;
	unsigned index;
	int active;
	void *mapping;
	int error;

	/* Nothing is below a link that is down. */
	active = host->board.link_active(host->board.context);
	if (!active) {
		kern_logf("pcie%u: firmware left the link down\n", host->config.segment);
		return;
	}

	/* Numbers the bus below the root port, as the firmware did, to reach the endpoint. */
	host->link_up = true;
	kern_memset(&address, 0, sizeof(address));
	address.bus = PLDA_ROOT_BUS;
	buses = (uint32_t)PLDA_ROOT_BUS;
	buses |= (uint32_t)PLDA_SECONDARY_BUS << 8;
	buses |= (uint32_t)PLDA_SECONDARY_BUS << 16;
	(void)host_config_write(host, &address, PCI_BRIDGE_BUSES, 4U, buses);

	/* Reads the endpoint's identity, kind, decoding and first BAR. */
	address.bus = PLDA_SECONDARY_BUS;
	identity = 0xffffffffU;
	class_code = 0;
	command = 0;
	bar = 0;
	bar_high = 0;
	(void)host_config_read(host, &address, PCI_VENDOR_ID, 4U, &identity);
	(void)host_config_read(host, &address, PCI_CLASS_REVISION, 4U, &class_code);
	(void)host_config_read(host, &address, PCI_COMMAND, 2U, &command);
	(void)host_config_read(host, &address, PCI_BAR0, 4U, &bar);
	if ((bar & PCI_BAR_TYPE) == PCI_BAR_TYPE_64)
		(void)host_config_read(host, &address, PCI_BAR0 + 4U, 4U, &bar_high);

	/*
	 * Closes the bus below again until the link is trained, and keeps the
	 * class code's upper three bytes.
	 */
	host->link_up = false;
	class_code >>= 8;

	/* Reports a live link with no endpoint behind it. */
	if ((identity & 0xffffU) == 0xffffU) {
		kern_logf("pcie%u: firmware left the link up with no endpoint\n", host->config.segment);
		return;
	}

	/* Reports the endpoint the firmware left. */
	kern_logf("pcie%u: firmware left %04x:%04x class %06x, command %04x\n",
		  host->config.segment,
		  identity & 0xffffU,
		  identity >> 16,
		  class_code,
		  command);

	/* Only a decoding USB or NVMe controller has a state worth reading. */
	if (class_code != PCI_CLASS_XHCI && class_code != PCI_CLASS_NVME)
		return;
	if ((command & PCI_COMMAND_MEMORY) == 0 || (bar & PCI_BAR_IO) != 0)
		return;

	/* Finds the BAR's place in a memory window. */
	bus_address = ((uint64_t)bar_high << 32) | (bar & PCI_BAR_MEMORY_MASK);
	cpu_address = 0;
	for (index = 0; index < host->config.window_count; index++) {
		window = &host->config.windows[index];
		if (bus_address < window->pci_base)
			continue;
		if (bus_address - window->pci_base > window->size - ENDPOINT_PROBE_SPAN)
			continue;
		cpu_address = window->cpu_base + (bus_address - window->pci_base);
		break;
	}

	/* Reports a BAR that lies outside every window, which cannot be read. */
	if (cpu_address == 0) {
		kern_logf("pcie%u: firmware's BAR %llx is outside the windows\n",
			  host->config.segment,
			  (unsigned long long)bus_address);
		return;
	}

	/* Maps the start of the BAR to read the controller's state. */
	error = kern_device_map(cpu_address, ENDPOINT_PROBE_SPAN, KERN_DEVICE_UNCACHED, &mapping);
	if (error != 0)
		return;
	probe = mapping;

	/* Reads whether the controller was halted, or enabled and ready. */
	if (class_code == PCI_CLASS_XHCI) {
		value = kern_mmio_read8(probe);
		value = kern_mmio_read32(probe + value + XHCI_USBSTS);
		kern_logf("pcie%u: xHCI USBSTS %08x, HCHalted %u\n",
			  host->config.segment,
			  value,
			  value & XHCI_HALTED);
	} else {
		value = kern_mmio_read32(probe + NVME_CC);
		ready = kern_mmio_read32(probe + NVME_CSTS);
		kern_logf("pcie%u: NVMe CC.EN %u, CSTS.RDY %u\n",
			  host->config.segment,
			  value & NVME_ENABLE,
			  ready & NVME_ENABLE);
	}

	/* Releases the probe's mapping. */
	(void)kern_device_unmap(mapping, ENDPOINT_PROBE_SPAN);
}

/*
 * Holds the endpoint in reset and makes the block a root port.
 *
 * Procedure referenced: OpenBSD sys/dev/fdt/stfpcie.c 1.4 (ISC), the
 * attach: PERST# asserted, the root port's extra functions disabled one
 * by one through the board's function selection, then the root port type,
 * the bridge class, the 64-bit prefetchable window and no LTR messages.
 */
static void
configure_root_complex(
	struct drv_pci_plda *host)
{
	unsigned function;

	/* Holds the endpoint in reset. */
	host->board.perst(host->board.context, 1);

	/* Switches off each of the root port's extra functions. */
	for (function = 1; function <= PLDA_EXTRA_FUNCTIONS; function++) {
		host->board.select_function(host->board.context, function);
		register_update(host, PLDA_FUNCTION_WORD, 0, PLDA_FUNCTION_DISABLE);
	}

	/* Points the function registers back at the root port itself. */
	host->board.select_function(host->board.context, 0);

	/* Makes the block a root port that presents itself as a PCI bridge. */
	register_update(host, PLDA_GEN_SETTINGS, 0, PLDA_GEN_ROOT_PORT);
	register_write(host, PLDA_CLASS_WORD, PLDA_CLASS_BRIDGE);

	/* Lets the bridge's prefetchable window reach above 4 GiB. */
	register_update(host, PLDA_BAR_WINDOW, 0, PLDA_BAR_WINDOW_PREFETCH64);

	/* Stops forwarding the endpoint's latency messages. */
	register_update(host, PLDA_MESSAGE_SUPPORT, PLDA_MESSAGE_LTR, 0);
}

/*
 * Releases the endpoint from reset and waits for the link.
 *
 * PERST# stays asserted 100 ms, as after power-up; once the link is up the
 * endpoint gets another 100 ms before its first configuration request.
 */
static void
train_link(
	struct drv_pci_plda *host)
{
	unsigned poll;
	int active;

	/* Keeps the endpoint in reset long enough for a cold start. */
	kern_usleep_range(PLDA_RESET_US, PLDA_RESET_US);

	/* Releases the endpoint. */
	host->board.perst(host->board.context, 0);

	/* Waits for the data link layer, one millisecond at a time. */
	active = 0;
	for (poll = 0; poll < PLDA_LINK_POLLS; poll++) {
		active = host->board.link_active(host->board.context);
		if (active)
			break;
		kern_usleep_range(PLDA_LINK_POLL_US, PLDA_LINK_POLL_US);
	}

	/* A link that stays down leaves the root port alone on the tree. */
	if (!active) {
		kern_logf("pcie%u: link down\n", host->config.segment);
		return;
	}

	/* Gives the endpoint its time before the first configuration request. */
	kern_usleep_range(PLDA_SETTLE_US, PLDA_SETTLE_US);
	host->link_up = true;
	kern_logf("pcie%u: link up\n", host->config.segment);
}

/*
 * Finds a function's configuration register in the window.
 *
 * Bus 0 holds the root port alone and bus 1 its one endpoint, reached only
 * while the link is up.  Reports ENODEV for a function that cannot exist
 * and EINVAL for a malformed access.
 */
static int
config_address(
	struct drv_pci_plda *host,
	const struct drv_pci_address *address,
	unsigned offset,
	unsigned width,
	volatile uint8_t **target)
{
	uint64_t position;

	/* Requires a function address the bus can carry. */
	if (address == NULL)
		return EINVAL;
	if (address->device >= 32U || address->function >= 8U)
		return EINVAL;

	/* Requires an aligned byte, halfword or word inside the function's space. */
	if (width != 1U && width != 2U && width != 4U)
		return EINVAL;
	if (offset >= PLDA_FUNCTION_SPACE || PLDA_FUNCTION_SPACE - offset < width)
		return EINVAL;
	if ((offset & (width - 1U)) != 0)
		return EINVAL;

	/* The root port is the only function on bus 0. */
	if (address->bus == PLDA_ROOT_BUS) {
		if (address->device != 0 || address->function != 0)
			return ENODEV;
	} else if (address->bus == PLDA_SECONDARY_BUS) {
		/* The link leads to device 0 only, and only while it is up. */
		if (!host->link_up)
			return ENODEV;
		if (address->device != 0)
			return ENODEV;
	} else {
		/* No bus is numbered below the endpoint. */
		return ENODEV;
	}

	/* Succeeded: the register's place in the window. */
	position = (uint64_t)address->bus << PLDA_ECAM_BUS_SHIFT;
	position |= (uint64_t)address->device << PLDA_ECAM_DEVICE_SHIFT;
	position |= (uint64_t)address->function << PLDA_ECAM_FUNCTION_SHIFT;
	*target = host->config_space + position + offset;
	return 0;
}

/*
 * Reports whether an access falls on the root port's own BARs.
 *
 * Nothing uses those BARs, and sizing them would rewrite the block's own
 * decoding while devices may run, so they read as zero and ignore writes.
 */
static bool
root_port_bar(
	const struct drv_pci_address *address,
	unsigned offset)
{
	/* Only the root port's first two BAR slots are hidden. */
	if (address->bus != PLDA_ROOT_BUS)
		return false;
	if (offset < PCI_BAR0 || offset >= PCI_BAR1_END)
		return false;

	/* The access touches a hidden BAR. */
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
	volatile uint8_t *target;
	bool hidden;
	int error;

	/* Refuses a malformed access. */
	if (value == NULL)
		return EINVAL;
	error = config_address(context, address, offset, width, &target);
	if (error == EINVAL)
		return EINVAL;

	/* Reads a hidden BAR as zero and an impossible function as all-ones. */
	hidden = false;
	if (error == 0)
		hidden = root_port_bar(address, offset);
	if (error != 0) {
		*value = width == 4U ? 0xffffffffU : (1U << (width * 8U)) - 1U;
	} else if (hidden) {
		*value = 0;
	} else if (width == 1U) {
		*value = kern_mmio_read8(target);
	} else if (width == 2U) {
		*value = kern_mmio_read16(target);
	} else {
		*value = kern_mmio_read32(target);
	}

	/* Succeeded: value holds the register, zero or all-ones. */
	return 0;
}

/*
 * Writes a function's configuration space for the PCI core.
 *
 * A write to a function that cannot exist, or to a hidden BAR, is dropped.
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
	bool hidden;
	int error;

	/* Refuses a malformed access and drops one no function would receive. */
	error = config_address(context, address, offset, width, &target);
	if (error == EINVAL)
		return EINVAL;
	if (error != 0)
		return 0;
	hidden = root_port_bar(address, offset);
	if (hidden)
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
 * Numbers the bus below the root port and places its endpoint's BARs.
 *
 * Every function of device 0 on bus 1 gets its memory BARs from the start
 * of the first window below 4 GiB, and the root port forwards the part of
 * the window that was used.
 */
static int
assign_resources(
	struct drv_pci_plda *host)
{
	const struct drv_pci_plda_window *window;
	struct drv_pci_address address;
	uint32_t buses;
	bool multifunction;
	unsigned function;
	unsigned index;
	int error;

	/* Picks the first memory window the 32-bit BARs can address. */
	host->bar_window = NULL;
	for (index = 0; index < host->config.window_count; index++) {
		window = &host->config.windows[index];
		if (window->pci_base + window->size > PLDA_32BIT_LIMIT)
			continue;
		host->bar_window = window;
		break;
	}

	/* Refuses a host without such a window; the BARs start at its base. */
	if (host->bar_window == NULL)
		return ENOSPC;
	host->window_next = host->bar_window->pci_base;

	/* Makes bus 1 the root port's secondary and subordinate bus. */
	kern_memset(&address, 0, sizeof(address));
	address.bus = PLDA_ROOT_BUS;
	buses = (uint32_t)PLDA_ROOT_BUS;
	buses |= (uint32_t)PLDA_SECONDARY_BUS << 8;
	buses |= (uint32_t)PLDA_SECONDARY_BUS << 16;
	(void)host_config_write(host, &address, PCI_BRIDGE_BUSES, 4U, buses);

	/* Places the BARs of every function of the device on bus 1. */
	address.bus = PLDA_SECONDARY_BUS;
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
 * absent function or anything but an endpoint is left alone.
 */
static int
assign_function(
	struct drv_pci_plda *host,
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
		kern_logf("pcie%u: %02x:%02x.%u is not an endpoint, left unassigned\n",
			  host->config.segment,
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
 * Measures one BAR and places it in the 32-bit window.
 *
 * Wide reports a 64-bit BAR, whose upper half is the next slot.  An I/O BAR
 * is left unassigned: the root complex has no I/O window.  A BAR that does
 * not fit what is left of the window is set to 0 and left unassigned.
 */
static int
assign_bar(
	struct drv_pci_plda *host,
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
	window_end = host->bar_window->pci_base + host->bar_window->size;
	if (size == 0 ||
	    placed < host->window_next ||
	    placed > window_end ||
	    size > window_end - placed) {
		/*
		 * Leaves this BAR at 0, not at the sizing value, and goes on, so
		 * the other BARs and the root port's window are still set up.
		 */
		(void)host_config_write(host, address, offset, 4U, 0);
		if (*wide)
			(void)host_config_write(host, address, offset + 4U, 4U, 0);
		kern_logf("pcie%u: BAR%u of %02x:%02x.%u (%llu bytes) does not fit the window, left unassigned\n",
			  host->config.segment,
			  index,
			  address->bus,
			  address->device,
			  address->function,
			  (unsigned long long)size);
		return 0;
	}

	/* Programs the address; the window lies below 4 GiB. */
	(void)host_config_write(host, address, offset, 4U, (uint32_t)placed);
	if (*wide)
		(void)host_config_write(host, address, offset + 4U, 4U, (uint32_t)(placed >> 32));

	/* Records the space as used. */
	host->window_next = placed + size;

	/* Reports where the BAR went. */
	kern_logf("pcie%u: %02x:%02x.%u BAR%u at %llx (%llu bytes)\n",
		  host->config.segment,
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
 * Lets the root port forward the assigned part of the 32-bit window.
 *
 * The I/O and prefetchable windows stay closed (base above limit).  The
 * root port also becomes a bus master, so devices below it reach memory.
 */
static void
configure_root_port(
	struct drv_pci_plda *host)
{
	struct drv_pci_address address;
	uint32_t base;
	uint32_t limit;
	uint32_t memory;

	/* Addresses the root port. */
	kern_memset(&address, 0, sizeof(address));
	address.bus = PLDA_ROOT_BUS;

	/* Forwards the used megabytes of the window, or nothing if none was used. */
	memory = 0x0000fff0U;
	if (host->window_next != host->bar_window->pci_base) {
		base = (uint32_t)(host->bar_window->pci_base >> 16) & 0xfff0U;
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
 * The BAR's PCI address lies in one memory window; the CPU reaches it at
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
	const struct drv_pci_plda_window *window;
	struct drv_pci_plda *host;
	uint64_t cpu_address;
	unsigned index;
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

	/* Finds the window the BAR lies in. */
	cpu_address = 0;
	for (index = 0; index < host->config.window_count; index++) {
		window = &host->config.windows[index];
		if (bar->bus_address < window->pci_base)
			continue;
		if (bar->bus_address - window->pci_base >= window->size)
			continue;
		if (bar->size > window->size - (bar->bus_address - window->pci_base))
			continue;
		cpu_address = window->cpu_base + (bar->bus_address - window->pci_base);
		break;
	}

	/* Refuses a BAR outside every window. */
	if (cpu_address == 0)
		return EINVAL;

	/* Maps the CPU side of the BAR as uncached device memory. */
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
 * Gives a device its legacy interrupt, or one MSI message.
 *
 * The device's pin is swizzled across each bridge between it and the root
 * bus, then the root port slot's pin is looked up in the interrupt map.
 * The vector is the host's own pin number, which the pin hooks take.  MSI
 * is offered, one message, when the block receives messages and the
 * function has the capability; the message hooks then give the vector.
 * MSI-X is not offered.
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
	struct drv_pci_plda *host;
	struct drv_pci_address address;
	struct drv_pci_device *walk;
	struct drv_pci_bus *bus;
	struct drv_pci_bus *above;
	unsigned capability;
	unsigned vector;
	uint8_t pin;
	int error;

	/* Refuses missing arguments and a request for more than one line. */
	host = context;
	if (device == NULL || irqs == NULL || count == NULL)
		return EINVAL;
	if (minimum != 1U || maximum < minimum)
		return ENOTSUP;

	/* Offers one MSI message when the block receives them. */
	if (type == DRV_PCI_IRQ_MSI) {
		/* Refuses without the block's messages or the function's capability. */
		if (!host->msi_ready)
			return ENOTSUP;
		error = drv_pci_device_find_capability(device, PCI_CAPABILITY_MSI, &capability);
		if (error != 0)
			return ENOTSUP;

		/* Succeeded: the core establishes the message through the hooks. */
		irqs[0].type = DRV_PCI_IRQ_MSI;
		irqs[0].index = 0;
		irqs[0].vector = 0;
		irqs[0].private_data[0] = capability;
		irqs[0].private_data[1] = 0;
		*count = 1U;
		return 0;
	}

	/* Offers no other kind but the legacy pins. */
	if (type != DRV_PCI_IRQ_INTX)
		return ENOTSUP;

	/* Reads which pin the device uses; zero means none. */
	error = drv_pci_device_config_read8(device, PCI_INTERRUPT_PIN, &pin);
	if (error != 0)
		return ENODEV;
	if (pin == 0 || pin > DRV_PCI_PLDA_INTX_COUNT)
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
		pin = (uint8_t)(((pin - 1U + address.device) % DRV_PCI_PLDA_INTX_COUNT) + 1U);
		walk = drv_pci_bus_bridge(bus);
	}

	/* Refuses a pin the interrupt map does not route. */
	vector = host->config.intx_vector[pin - 1U];
	if (vector == 0)
		return ENODEV;

	/* Succeeded: the device's line is the host's pin. */
	irqs[0].type = DRV_PCI_IRQ_INTX;
	irqs[0].index = 0;
	irqs[0].vector = vector;
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

/*
 * Puts a PCI core line in a pin's entry, the pin still masked.
 *
 * Called with the core's line lock held and interrupts off.  Reports EBUSY
 * when another line holds the pin.
 */
static int
host_intx_establish(
	void *context,
	unsigned vector,
	drv_pci_intx_run_t run,
	void *line)
{
	struct drv_pci_plda *host;
	struct plda_pin *pin;
	unsigned long enabled;
	int error;

	/* Refuses a pin the host does not have and an empty line. */
	host = context;
	if (vector < 1U || vector > DRV_PCI_PLDA_INTX_COUNT)
		return EINVAL;
	if (run == NULL || line == NULL)
		return EINVAL;

	/* Fills the pin's entry unless another line holds it. */
	enabled = spin_lock_irqsave(&host->lock);

	pin = &host->pins[vector - 1U];
	error = 0;
	if (pin->run != NULL) {
		error = EBUSY;
	} else {
		pin->run = run;
		pin->line = line;
	}

	spin_unlock_irqrestore(&host->lock, enabled);

	/* Failed: the pin belongs to another line. */
	if (error != 0)
		return error;

	/* Succeeded: the line is in the table, its pin still masked. */
	return 0;
}

/*
 * Empties a pin's entry and waits until no interrupt still runs it.
 *
 * Called in thread context.  After one second of waiting the entry and the
 * pin are put back as they were and EBUSY is reported.
 */
static int
host_intx_disestablish(
	void *context,
	unsigned vector,
	drv_pci_intx_run_t run,
	void *line)
{
	struct drv_pci_plda *host;
	struct plda_pin *pin;
	unsigned long enabled;
	uint32_t enabled_before;
	unsigned in_flight;
	unsigned poll;

	/* Refuses a pin the host does not have. */
	host = context;
	if (vector < 1U || vector > DRV_PCI_PLDA_INTX_COUNT)
		return EINVAL;

	/* Masks the pin and empties its entry, after checking the entry is this line's. */
	enabled = spin_lock_irqsave(&host->lock);

	pin = &host->pins[vector - 1U];
	if (pin->run != run || pin->line != line) {
		spin_unlock_irqrestore(&host->lock, enabled);
		return EINVAL;
	}

	/* Keeps the enabled pins to put back if the drain times out. */
	enabled_before = host->enabled_pins;
	host->enabled_pins &= ~(1U << (vector - 1U));
	write_interrupt_mask(host);
	pin->run = NULL;
	pin->line = NULL;

	spin_unlock_irqrestore(&host->lock, enabled);

	/* Waits, a millisecond at a time, for the interrupts still inside run. */
	for (poll = 0; poll < PLDA_DRAIN_POLLS; poll++) {
		/* Reads how many interrupts still hold the pin. */
		enabled = spin_lock_irqsave(&host->lock);

		in_flight = pin->in_flight;

		spin_unlock_irqrestore(&host->lock, enabled);

		/* Drained: no hart runs the line and none will. */
		if (in_flight == 0)
			break;

		/* Lets the other hart finish. */
		kern_usleep_range(PLDA_DRAIN_POLL_US, PLDA_DRAIN_POLL_US);
	}

	/*
	 * Puts the entry and the pin back as they were before the call when an
	 * interrupt did not come back within the bound.
	 */
	if (in_flight != 0) {
		enabled = spin_lock_irqsave(&host->lock);

		pin->run = run;
		pin->line = line;
		host->enabled_pins = enabled_before;
		write_interrupt_mask(host);

		spin_unlock_irqrestore(&host->lock, enabled);

		/* Reports the pin whose interrupt did not come back. */
		kern_logf("pcie%u: pin %u still running after a second\n", host->config.segment, vector);
		return EBUSY;
	}

	/* Succeeded: the entry is empty and no interrupt runs the line. */
	return 0;
}

/* Stops a pin from interrupting; called with interrupts off. */
static void
host_intx_mask(
	void *context,
	unsigned vector)
{
	struct drv_pci_plda *host;
	unsigned long enabled;

	/* Ignores a pin the host does not have. */
	host = context;
	if (vector < 1U || vector > DRV_PCI_PLDA_INTX_COUNT)
		return;

	/* Clears the pin's enable bit in the table and the block. */
	enabled = spin_lock_irqsave(&host->lock);

	host->enabled_pins &= ~(1U << (vector - 1U));
	write_interrupt_mask(host);

	spin_unlock_irqrestore(&host->lock, enabled);
}

/* Lets a pin interrupt; called with interrupts off. */
static void
host_intx_unmask(
	void *context,
	unsigned vector)
{
	struct drv_pci_plda *host;
	unsigned long enabled;

	/* Ignores a pin the host does not have. */
	host = context;
	if (vector < 1U || vector > DRV_PCI_PLDA_INTX_COUNT)
		return;

	/* Sets the pin's enable bit in the table and the block. */
	enabled = spin_lock_irqsave(&host->lock);

	host->enabled_pins |= 1U << (vector - 1U);
	write_interrupt_mask(host);

	spin_unlock_irqrestore(&host->lock, enabled);
}

/*
 * Writes the enabled pins, and the messages, to the block's interrupt mask.
 *
 * The caller holds the host lock.  Events stay masked, and messages are
 * let through only while at least one is established.
 */
static void
write_interrupt_mask(
	struct drv_pci_plda *host)
{
	uint32_t mask;

	/* Pin v's enable bit is bit 24 + (v - 1). */
	mask = host->enabled_pins << PLDA_INTERRUPT_PIN_SHIFT;

	/* The messages share one bit. */
	if (host->messages_used != 0)
		mask |= PLDA_INTERRUPT_MSI;

	register_write(host, PLDA_INTERRUPT_MASK, mask);
}

/*
 * Reads where the block receives message interrupts, when the device tree
 * calls it an MSI controller, and clears any message already raised.
 *
 * The address is the block's own: the function writes its vector there.
 * Procedure referenced: OpenBSD sys/arch/riscv64/dev/stfpcie.c 1.4 (ISC),
 * the attach: the message address is read from the register, not set.
 * A register that reads 0 gives the address the StarFive block is known
 * to receive at (Value(s) referenced above).
 */
static void
start_messages(
	struct drv_pci_plda *host)
{
	uint32_t address;

	/* Leaves the messages off where the tree does not ask for them. */
	if (!host->config.msi)
		return;

	/* Clears what any earlier owner of the block left raised. */
	register_write(host, PLDA_MSI_STATUS, 0xffffffffU);

	/* Reads the address; an empty register means the block's known one. */
	address = register_read(host, PLDA_MSI_ADDRESS);
	host->msi_address = address != 0 ? address : PLDA_MSI_DEFAULT_ADDRESS;

	/* Succeeded: functions may be offered MSI. */
	host->msi_ready = true;
	kern_logf("pcie%u: MSI at %x (register %x), %u vectors\n",
		  host->config.segment,
		  host->msi_address,
		  address,
		  PLDA_MSI_VECTORS);
}

/*
 * Takes a free message vector for a PCI core cookie.
 *
 * The function writes the vector as its data to the block's message
 * address.  The block's message bit is let through from the first message
 * on.  Called in thread context.
 */
static int
host_msi_establish(
	void *context,
	struct drv_pci_device *device,
	drv_pci_msi_run_t run,
	void *argument,
	unsigned *handle,
	uint64_t *address,
	uint32_t *data)
{
	struct drv_pci_plda *host;
	struct plda_message *message;
	unsigned long enabled;
	unsigned vector;

	UNUSED_PARAMETER(device);

	/* Refuses missing arguments and a block that receives no messages. */
	host = context;
	if (run == NULL || handle == NULL || address == NULL || data == NULL)
		return EINVAL;
	if (!host->msi_ready)
		return ENOTSUP;

	/* Fills the first free entry and lets the messages through. */
	enabled = spin_lock_irqsave(&host->lock);

	for (vector = 0; vector < PLDA_MSI_VECTORS; vector++) {
		message = &host->messages[vector];
		if (message->run == NULL && message->in_flight == 0)
			break;
	}
	if (vector < PLDA_MSI_VECTORS) {
		message->run = run;
		message->argument = argument;
		host->messages_used++;
		write_interrupt_mask(host);
	}

	spin_unlock_irqrestore(&host->lock, enabled);

	/* Failed: every vector is taken. */
	if (vector == PLDA_MSI_VECTORS)
		return ENOSPC;

	/* Succeeded: the vector is the handle and the data. */
	*handle = vector;
	*address = host->msi_address;
	*data = vector;
	return 0;
}

/*
 * Empties a message vector's entry and waits until no interrupt still runs
 * it.
 *
 * Called in thread context.  After one second of waiting the entry is put
 * back and EBUSY is reported.
 */
static int
host_msi_disestablish(
	void *context,
	unsigned handle)
{
	struct drv_pci_plda *host;
	struct plda_message *message;
	drv_pci_msi_run_t run;
	unsigned long enabled;
	unsigned in_flight;
	unsigned poll;
	void *argument;

	/* Refuses a vector the host does not have or has not given out. */
	host = context;
	if (handle >= PLDA_MSI_VECTORS)
		return EINVAL;

	/* Empties the entry; the messages stay masked once the last is gone. */
	enabled = spin_lock_irqsave(&host->lock);

	message = &host->messages[handle];
	run = message->run;
	argument = message->argument;
	if (run == NULL) {
		spin_unlock_irqrestore(&host->lock, enabled);
		return EINVAL;
	}
	message->run = NULL;
	message->argument = NULL;
	host->messages_used--;
	write_interrupt_mask(host);

	spin_unlock_irqrestore(&host->lock, enabled);

	/* Waits, a millisecond at a time, for the interrupts still inside run. */
	in_flight = 0;
	for (poll = 0; poll < PLDA_DRAIN_POLLS; poll++) {
		/* Reads how many interrupts still hold the vector. */
		enabled = spin_lock_irqsave(&host->lock);

		in_flight = message->in_flight;

		spin_unlock_irqrestore(&host->lock, enabled);

		/* Drained: no hart runs the vector and none will. */
		if (in_flight == 0)
			break;

		/* Lets the other hart finish. */
		kern_usleep_range(PLDA_DRAIN_POLL_US, PLDA_DRAIN_POLL_US);
	}

	/* Puts the entry back when an interrupt did not come back within the bound. */
	if (in_flight != 0) {
		enabled = spin_lock_irqsave(&host->lock);

		message->run = run;
		message->argument = argument;
		host->messages_used++;
		write_interrupt_mask(host);

		spin_unlock_irqrestore(&host->lock, enabled);

		/* Reports the vector whose interrupt did not come back. */
		kern_logf("pcie%u: MSI %u still running after a second\n", host->config.segment, handle);
		return EBUSY;
	}

	/* Succeeded: the entry is free and no interrupt runs it. */
	return 0;
}

/*
 * Waits a short while in interrupt context, by reading the block's
 * interrupt mask PLDA_MSI_SETTLE_READS times.
 */
static void
settle(
	struct drv_pci_plda *host)
{
	unsigned read;

	for (read = 0; read < PLDA_MSI_SETTLE_READS; read++)
		(void)register_read(host, PLDA_INTERRUPT_MASK);
}

/*
 * Notes, once per host, a message whose handler found its work only when
 * run again.
 */
static void
note_late_message(
	struct drv_pci_plda *host,
	unsigned vector,
	unsigned retries)
{
	if (host->late_message_noted)
		return;
	host->late_message_noted = true;
	kern_logf("pcie%u: MSI %u found its work after %u retries (the message came before the data)\n",
		  host->config.segment,
		  vector,
		  retries);
}

/*
 * Runs each message vector that arrived.
 *
 * The message status is read and acknowledged first, then the block's
 * message bit: on the JH7110, with the message bit cleared first, the bit
 * was raised again once the message status was acknowledged (seen on the
 * VisionFive 2 on 2026-10-26), and each message brought one more interrupt
 * with nothing to serve.  A message that
 * arrives after the status was read raises its bits again; one that
 * arrives in between is taken by reading the status again after the
 * handlers, until it reads 0.  A message is an edge: its status is
 * acknowledged before its handler.
 *
 * On the JH7110 a message can also arrive before the memory writes the
 * function made before sending it are seen (seen on the VisionFive 2 on
 * 2026-10-26: the NVMe controller's first completion was not in memory
 * when its message was served, and no other message came).  A handler
 * that finds no work is run again after a short wait, up to
 * PLDA_MSI_RETRIES times.
 *
 * Procedure referenced: OpenBSD sys/arch/riscv64/dev/stfpcie.c 1.4 (ISC),
 * the message handler: read the status, write it back, run each bit's
 * handler.
 */
static void
serve_messages(
	struct drv_pci_plda *host)
{
	struct plda_message *message;
	drv_pci_msi_run_t run;
	unsigned long enabled;
	uint32_t status;
	unsigned vector;
	unsigned pass;
	unsigned retry;
	void *argument;
	int handled;

	/* Serves the vectors until the message status reads 0. */
	for (pass = 0; pass < PLDA_MSI_PASSES; pass++) {
		/* Reads which vectors arrived and acknowledges them, then the message bit. */
		status = register_read(host, PLDA_MSI_STATUS);
		if (status != 0)
			register_write(host, PLDA_MSI_STATUS, status);
		register_write(host, PLDA_INTERRUPT_STATUS, PLDA_INTERRUPT_MSI);
		if (status == 0)
			break;

		/* Runs each arrived vector that a cookie owns. */
		for (vector = 0; vector < PLDA_MSI_VECTORS; vector++) {
			/* Skips a vector that did not arrive. */
			if ((status & (1U << vector)) == 0)
				continue;

			/* Takes the vector's entry and counts this interrupt on it. */
			enabled = spin_lock_irqsave(&host->lock);

			message = &host->messages[vector];
			run = message->run;
			argument = message->argument;
			if (run != NULL)
				message->in_flight++;

			spin_unlock_irqrestore(&host->lock, enabled);

			/* An empty vector is not served. */
			if (run == NULL)
				continue;

			/* Runs the cookie's handler with the host lock released. */
			handled = run(argument);

			/*
			 * Runs it again, a little later, while it finds no work:
			 * the message can arrive before the function's data.
			 */
			for (retry = 0; handled == 0 && retry < PLDA_MSI_RETRIES; retry++) {
				settle(host);
				handled = run(argument);
			}
			if (retry != 0 && handled != 0)
				note_late_message(host, vector, retry);

			/* Gives the vector back to a waiting removal. */
			enabled = spin_lock_irqsave(&host->lock);

			message->in_flight--;

			spin_unlock_irqrestore(&host->lock, enabled);
		}
	}
}

/*
 * Serves one interrupt of the block's line.
 *
 * It runs each pin the status raised and the PCI core let through, without
 * the host lock held, acknowledges the whole status after every handler (a
 * level pin stays raised until its device is served), then runs the
 * messages that arrived, and ends the interrupt once, whatever the status
 * held.
 *
 * Procedure referenced: OpenBSD sys/dev/fdt/stfpcie.c 1.4 (ISC), the
 * interrupt handler: read the local status, serve the pins, write the
 * status back to clear it, then serve the messages.
 */
static void
host_interrupt(
	int irq,
	kern_irq_ack_t acknowledge,
	void *argument)
{
	struct drv_pci_plda *host;
	struct plda_pin *pin;
	drv_pci_intx_run_t run;
	unsigned long enabled;
	uint32_t status;
	uint32_t pin_bit;
	unsigned vector;
	void *line;

	UNUSED_PARAMETER(irq);

	/* Takes the host the line was registered with. */
	host = argument;

	/* Reads what the block raised. */
	status = register_read(host, PLDA_INTERRUPT_STATUS);

	/* Runs each raised pin that is enabled and owned by a line. */
	for (vector = 1; vector <= DRV_PCI_PLDA_INTX_COUNT; vector++) {
		/* Skips a pin the status did not raise. */
		pin_bit = 1U << (PLDA_INTERRUPT_PIN_SHIFT + vector - 1U);
		if ((status & pin_bit) == 0)
			continue;

		/* Takes the pin's entry and counts this interrupt on it. */
		enabled = spin_lock_irqsave(&host->lock);

		pin = &host->pins[vector - 1U];
		run = pin->run;
		line = pin->line;
		if ((host->enabled_pins & (1U << (vector - 1U))) == 0)
			run = NULL;
		if (run != NULL)
			pin->in_flight++;

		spin_unlock_irqrestore(&host->lock, enabled);

		/* A masked or empty pin is not served. */
		if (run == NULL)
			continue;

		/* Runs the line's handlers with the host lock released. */
		run(line);

		/* Gives the pin back to a waiting removal. */
		enabled = spin_lock_irqsave(&host->lock);

		pin->in_flight--;

		spin_unlock_irqrestore(&host->lock, enabled);
	}

	/*
	 * Acknowledges what was read, only after every pin's handler; the
	 * message bit is the messages' own to clear.
	 */
	register_write(host, PLDA_INTERRUPT_STATUS, status & ~PLDA_INTERRUPT_MSI);

	/* Runs the messages that arrived. */
	if ((status & PLDA_INTERRUPT_MSI) != 0)
		serve_messages(host);

	/* Ends the line's interrupt once. */
	kern_irq_send_eoi(acknowledge);
}

/*
 * Takes the block's interrupt line with every pin masked.
 *
 * The PCI core never sees the line: it hands pins to the hooks.
 */
static int
install_line(
	struct drv_pci_plda *host)
{
	int error;

	/* Installs the handler while the block raises nothing. */
	error = kern_irq_register((int)host->config.irq, host_interrupt, host);
	if (error != 0) {
		kern_logf("pcie%u: interrupt %u unavailable (%d)\n", host->config.segment, host->config.irq, error);
		return error;
	}

	/* Clears anything raised while the line was being taken, then lets it through. */
	register_write(host, PLDA_INTERRUPT_STATUS, 0xffffffffU);
	kern_irq_unmask((int)host->config.irq);
	host->line_installed = true;

	/* Succeeded: the line reaches the handler. */
	return 0;
}
