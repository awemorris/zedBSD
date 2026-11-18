/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The interrupts of RP1's peripherals.
 *
 * RP1 has one MSI-X vector per peripheral interrupt.  Each vector's table
 * entry holds the MIP's doorbell address on the PCI bus and a message
 * number; the MIP turns a write of number n into an edge on its n-th
 * shared GIC line.  RP1 also keeps a configuration register per vector:
 * whether the vector is enabled, and whether it is a level interrupt that
 * must be acknowledged after it is served before RP1 sends it again.
 */

#include <drivers/platform/rpi5/rp1.h>
#include <drivers/platform/rpi5/rp1-irq.h>
#include <kern/device-io.h>
#include <kern/irq.h>
#include <kern/klog.h>
#include <kern/lock.h>
#include <kern/pmem.h>
#include <uapi/errno.h>

/*
 * The MIP's registers: the host's trigger configuration and masks, and the
 * VideoCore's masks.
 *
 * Values referenced: OpenBSD sys/arch/arm64/dev/bcm2712_mip.c
 * (MIP_INT_CFGL_HOST, MIP_INT_CFGH_HOST, MIP_INT_MASKL_HOST,
 * MIP_INT_MASKH_HOST, MIP_INT_MASKL_VPU, MIP_INT_MASKH_VPU).
 */
#define MIP_INT_CFGL_HOST		0x20U
#define MIP_INT_CFGH_HOST		0x30U
#define MIP_INT_MASKL_HOST		0x40U
#define MIP_INT_MASKH_HOST		0x50U
#define MIP_INT_MASKL_VPU		0x60U
#define MIP_INT_MASKH_VPU		0x70U
#define MIP_REGISTER_BYTES		0xc0U

/*
 * The cells of the MIP's msi-ranges: the GIC, the kind of its line, the
 * first shared line, its trigger and the number of lines.
 */
#define MIP_RANGES_CELLS		5U
#define MIP_RANGES_FIRST_LINE		2U
#define MIP_RANGES_LINE_COUNT		4U

/* The most MIP lines the module hands out. */
#define RP1_IRQ_SLOT_LIMIT		64U

/* The GIC's first shared line, after the private ones. */
#define GIC_SPI_BASE			32U

/*
 * RP1's per-vector configuration registers in its peripheral window, their
 * set and clear aliases, and their bits.
 *
 * Values referenced: OpenBSD sys/arch/arm64/dev/rpone.c rev 1.1
 * (MSIX_CFG(), MSIX_CFG_SET(), MSIX_CFG_CLR(), MSIX_CFG_ENABLE,
 * MSIX_CFG_IACK, MSIX_CFG_IACK_EN, RP1_MSIX_VECTORS).
 */
#define RP1_MSIX_CFG_BASE		0x108000U
#define RP1_MSIX_CFG_FIRST		0x008U
#define RP1_MSIX_CFG_SET		0x800U
#define RP1_MSIX_CFG_CLEAR		0xc00U
#define RP1_MSIX_CFG_BYTES		0x1000U
#define RP1_MSIX_CFG_ENABLE		(1U << 0)
#define RP1_MSIX_CFG_IACK		(1U << 2)
#define RP1_MSIX_CFG_IACK_ENABLE	(1U << 3)
#define RP1_MSIX_VECTORS		61U

/* RP1's MSI-X table is in its first BAR, the vector configuration in its second. */
#define RP1_MSIX_BAR			0U
#define RP1_PERIPHERAL_BAR		1U

/*
 * The PCI capability list and the MSI-X capability: its control word's
 * table size, function mask and enable, its table offset and BAR, and a
 * table entry's address, data and mask.
 */
#define PCI_STATUS_COMMAND		0x04U
#define PCI_STATUS_CAPABILITIES		(1U << 20)
#define PCI_CAPABILITY_POINTER		0x34U
#define PCI_CAPABILITY_MSIX		0x11U
#define PCI_CAPABILITY_LIMIT		48U
#define PCI_MSIX_TABLE			0x04U
#define PCI_MSIX_CONTROL_SHIFT		16U
#define PCI_MSIX_TABLE_SIZE_MASK	0x07ffU
#define PCI_MSIX_FUNCTION_MASK		0x4000U
#define PCI_MSIX_ENABLE			0x8000U
#define PCI_MSIX_BIR_MASK		0x7U
#define PCI_MSIX_ENTRY_BYTES		16U
#define PCI_MSIX_ENTRY_ADDRESS_LOW	0x0U
#define PCI_MSIX_ENTRY_ADDRESS_HIGH	0x4U
#define PCI_MSIX_ENTRY_DATA		0x8U
#define PCI_MSIX_ENTRY_CONTROL		0xcU
#define PCI_MSIX_ENTRY_MASKED		0x1U

/*
 * One MIP line handed to an RP1 vector.
 *
 * Filled by drv_rp1_irq_establish() before the line's handler is
 * registered and constant afterwards; the interrupt reads it.
 */
struct rp1_irq_slot {
	unsigned vector;
	unsigned level;
	int irq;
	drv_rp1_irq_handler_t handler;
	void *argument;
};

/*
 * The MIP and RP1's MSI-X table as the module found them.
 *
 * Filled once by drv_rp1_irq_init() and not changed afterwards, except
 * slot_count, which drv_rp1_irq_establish() moves up under the lock as it
 * hands out lines; a slot below slot_count is in use for good.
 */
struct rp1_irq_state {
	int ready;
	struct spinlock lock;
	volatile uint8_t *mip;
	volatile uint8_t *msix_table;
	volatile uint8_t *msix_config;
	uint64_t doorbell;
	unsigned first_line;
	unsigned line_count;
	unsigned message_offset;
	unsigned table_size;
	unsigned slot_count;
	struct rp1_irq_slot slots[RP1_IRQ_SLOT_LIMIT];
};

/* The one interrupt path of RP1. */
static struct rp1_irq_state rp1_irq;

static int find_mip(void);
static int read_doorbell(uint32_t mip);
static int find_msix(void);
static int map_window(unsigned bar, uint64_t offset, size_t size, volatile uint8_t **mapped);
static void dispatch(int irq, kern_irq_ack_t acknowledge, void *argument);

/*
 * Finds the MSI receiver and RP1's MSI-X table and enables them.
 */
int
drv_rp1_irq_init(
	void)
{
	int error;

	/* Does nothing a second time. */
	if (rp1_irq.ready)
		return 0;

	/* Finds the MIP RP1's root complex sends messages to. */
	error = find_mip();
	if (error != 0) {
		kern_logf("rp1: no MSI receiver (%d)\n", error);
		return error;
	}

	/* Finds RP1's MSI-X table and turns MSI-X on. */
	error = find_msix();
	if (error != 0) {
		kern_logf("rp1: no MSI-X table (%d)\n", error);
		return error;
	}

	/* Maps RP1's per-vector configuration registers. */
	error = map_window(RP1_PERIPHERAL_BAR, RP1_MSIX_CFG_BASE, RP1_MSIX_CFG_BYTES, &rp1_irq.msix_config);
	if (error != 0) {
		kern_logf("rp1: vector configuration unavailable (%d)\n", error);
		return error;
	}

	/*
	 * Sends every MIP line to the host, not the VideoCore, and makes them
	 * edge triggered, as the messages are.
	 *
	 * Procedure referenced: OpenBSD sys/arch/arm64/dev/bcm2712_mip.c
	 * (bcmmip_attach(): mask the VPU, configure the host's lines, unmask
	 * the host).
	 */
	kern_mmio_write32(rp1_irq.mip + MIP_INT_MASKL_VPU, 0xffffffffU);
	kern_mmio_write32(rp1_irq.mip + MIP_INT_MASKH_VPU, 0xffffffffU);
	kern_mmio_write32(rp1_irq.mip + MIP_INT_CFGL_HOST, 0xffffffffU);
	kern_mmio_write32(rp1_irq.mip + MIP_INT_CFGH_HOST, 0xffffffffU);
	kern_mmio_write32(rp1_irq.mip + MIP_INT_MASKL_HOST, 0);
	kern_mmio_write32(rp1_irq.mip + MIP_INT_MASKH_HOST, 0);

	/* Reports the path. */
	kern_logf("rp1: interrupts through the MIP at %llx, lines %u-%u, %u vectors\n",
		  (unsigned long long)rp1_irq.doorbell,
		  rp1_irq.first_line,
		  rp1_irq.first_line + rp1_irq.line_count - 1U,
		  rp1_irq.table_size);

	/* Prepares the lock of the MIP lines handed out. */
	spin_init(&rp1_irq.lock, LOCK_RANK_DEVICE, "RP1 interrupts");

	/* Succeeded: vectors can be routed. */
	rp1_irq.ready = 1;
	return 0;
}

/*
 * Reads the vector and trigger of an rp1 child's interrupt.
 */
int
drv_rp1_irq_of(
	uint32_t node,
	unsigned index,
	unsigned *vector,
	unsigned *trigger)
{
	const struct drv_fdt *fdt;
	const uint8_t *value;
	uint32_t length;
	uint32_t cells;
	int error;

	/* Refuses a request before RP1 was found, or with nowhere to put the answer. */
	fdt = drv_rp1_fdt();
	if (fdt == NULL)
		return ENODEV;
	if (vector == NULL || trigger == NULL)
		return EINVAL;

	/* An RP1 specifier is the vector and the trigger. */
	cells = drv_fdt_node_cells(fdt, drv_rp1_node(), "#interrupt-cells", 2U);
	if (cells != 2U)
		return ERANGE;

	/* Finds the requested specifier. */
	error = drv_fdt_property(fdt, node, "interrupts", &value, &length);
	if (error != 0)
		return error;
	if ((uint64_t)(index + 1U) * cells * 4U > length)
		return ENOENT;

	/* Succeeded: the vector and its trigger. */
	*vector = (unsigned)drv_fdt_cells_value(value, index * cells, 1U);
	*trigger = (unsigned)drv_fdt_cells_value(value, index * cells + 1U, 1U);
	return 0;
}

/*
 * Routes one of RP1's vectors to a handler.
 */
int
drv_rp1_irq_establish(
	unsigned vector,
	unsigned trigger,
	drv_rp1_irq_handler_t handler,
	void *argument)
{
	struct rp1_irq_slot *slot;
	volatile uint8_t *entry;
	volatile uint8_t *config;
	unsigned long enabled;
	unsigned index;
	unsigned message;
	int irq;
	int error;

	/* Refuses a request before the path was found, a vector RP1 does not have, or no handler. */
	if (!rp1_irq.ready)
		return ENODEV;
	if (vector >= rp1_irq.table_size || vector >= RP1_MSIX_VECTORS || handler == NULL)
		return EINVAL;

	/* Takes the next free MIP line. */
	enabled = spin_lock_irqsave(&rp1_irq.lock);

	index = rp1_irq.slot_count;
	if (index < rp1_irq.line_count && index < RP1_IRQ_SLOT_LIMIT)
		rp1_irq.slot_count++;

	spin_unlock_irqrestore(&rp1_irq.lock, enabled);

	/* Refuses a request when every line is taken. */
	if (index >= rp1_irq.line_count || index >= RP1_IRQ_SLOT_LIMIT)
		return ENOSPC;

	/* Fills the slot before its line can fire. */
	message = rp1_irq.message_offset + index;
	irq = (int)(GIC_SPI_BASE + rp1_irq.first_line + message);
	slot = &rp1_irq.slots[index];
	slot->vector = vector;
	slot->level = 0;
	if (trigger == DRV_RP1_IRQ_LEVEL)
		slot->level = 1;
	slot->irq = irq;
	slot->handler = handler;
	slot->argument = argument;

	/* Makes the GIC line edge triggered, as the MIP drives it. */
	error = kern_irq_set_mode(irq, KERN_IRQ_TRIGGER_EDGE, KERN_IRQ_POLARITY_HIGH);
	if (error != 0)
		return error;

	/* Puts the dispatcher on the line. */
	error = kern_irq_register(irq, dispatch, slot);
	if (error != 0)
		return error;

	/* Points the vector's table entry at the MIP line and unmasks it. */
	entry = rp1_irq.msix_table + vector * PCI_MSIX_ENTRY_BYTES;
	kern_mmio_write32(entry + PCI_MSIX_ENTRY_ADDRESS_LOW, (uint32_t)rp1_irq.doorbell);
	kern_mmio_write32(entry + PCI_MSIX_ENTRY_ADDRESS_HIGH, (uint32_t)(rp1_irq.doorbell >> 32));
	kern_mmio_write32(entry + PCI_MSIX_ENTRY_DATA, message);
	kern_mmio_write32(entry + PCI_MSIX_ENTRY_CONTROL, 0);

	/*
	 * Tells RP1 whether the vector waits for an acknowledgement, then
	 * enables it.
	 *
	 * Procedure referenced: OpenBSD sys/arch/arm64/dev/rpone.c rev 1.1
	 * (rpone_intr_establish(): IACK_EN set for a level interrupt and
	 * cleared otherwise, then ENABLE set).
	 */
	config = rp1_irq.msix_config + RP1_MSIX_CFG_FIRST + vector * 4U;
	if (slot->level) {
		kern_mmio_write32(config + RP1_MSIX_CFG_SET, RP1_MSIX_CFG_IACK_ENABLE);
	} else {
		kern_mmio_write32(config + RP1_MSIX_CFG_CLEAR, RP1_MSIX_CFG_IACK_ENABLE);
	}

	/* Enables the vector. */
	kern_mmio_write32(config + RP1_MSIX_CFG_SET, RP1_MSIX_CFG_ENABLE);

	/*
	 * Unmasks the GIC line: registering a handler leaves the line
	 * disabled, so without this every RP1 vector was served only by its
	 * driver's polling (the USB ports every 50 ms, the pointer at 20 Hz).
	 */
	kern_irq_unmask(irq);

	/* Reports the route. */
	kern_logf("rp1: vector %u (%s) on line %d\n", vector, slot->level ? "level" : "edge", irq);

	/* Succeeded: the vector interrupts the handler. */
	return 0;
}

/*
 * Finds the MIP the root complex's msi-parent names, maps its registers
 * and reads its lines and doorbell.
 */
static int
find_mip(
	void)
{
	const struct drv_fdt *fdt;
	const uint8_t *value;
	uint64_t address;
	uint64_t size;
	uint32_t length;
	uint32_t phandle;
	uint32_t mip;
	void *mapped;
	int error;

	/* Follows the root complex's msi-parent. */
	fdt = drv_rp1_fdt();
	error = drv_fdt_property(fdt, drv_rp1_root_complex_node(), "msi-parent", &value, &length);
	if (error != 0)
		return error;
	if (length < 4U)
		return ERANGE;
	phandle = (uint32_t)drv_fdt_cells_value(value, 0, 1U);
	error = drv_fdt_find_phandle(fdt, phandle, &mip);
	if (error != 0)
		return error;

	/* Reads the MIP's shared lines. */
	error = drv_fdt_property(fdt, mip, "msi-ranges", &value, &length);
	if (error != 0)
		return error;
	if (length < MIP_RANGES_CELLS * 4U)
		return ERANGE;
	rp1_irq.first_line = (unsigned)drv_fdt_cells_value(value, MIP_RANGES_FIRST_LINE, 1U);
	rp1_irq.line_count = (unsigned)drv_fdt_cells_value(value, MIP_RANGES_LINE_COUNT, 1U);
	rp1_irq.message_offset = drv_fdt_node_cells(fdt, mip, "brcm,msi-offset", 0);

	/* Reads the doorbell RP1 writes to. */
	error = read_doorbell(mip);
	if (error != 0)
		return error;

	/* Maps the MIP's registers. */
	error = drv_fdt_reg(fdt, mip, 0, &address, &size);
	if (error != 0)
		return error;
	if (size < MIP_REGISTER_BYTES)
		size = MIP_REGISTER_BYTES;
	error = kern_device_map(address, (size_t)size, KERN_DEVICE_UNCACHED, &mapped);
	if (error != 0)
		return error;

	/* Succeeded: the MIP is ready to be set up. */
	rp1_irq.mip = mapped;
	return 0;
}

/*
 * Reads the MIP's second reg entry, which is its doorbell as the PCI bus
 * sees it (the root complex's inbound window carries it to the MIP).
 */
static int
read_doorbell(
	uint32_t mip)
{
	const struct drv_fdt *fdt;
	const uint8_t *value;
	uint32_t length;
	uint32_t parent;
	uint32_t address_cells;
	uint32_t size_cells;
	uint32_t pair_cells;
	int error;

	/* Reads the cell counts of the MIP's bus. */
	fdt = drv_rp1_fdt();
	error = drv_fdt_parent(fdt, mip, &parent);
	if (error != 0)
		return error;
	address_cells = drv_fdt_node_cells(fdt, parent, "#address-cells", 2U);
	size_cells = drv_fdt_node_cells(fdt, parent, "#size-cells", 1U);
	if (address_cells == 0 || address_cells > 2U || size_cells > 2U)
		return ERANGE;

	/* Takes the second pair's address as it is written. */
	error = drv_fdt_property(fdt, mip, "reg", &value, &length);
	if (error != 0)
		return error;
	pair_cells = address_cells + size_cells;
	if (2U * pair_cells * 4U > length)
		return ENOENT;

	/* Succeeded: the doorbell's PCI address. */
	rp1_irq.doorbell = drv_fdt_cells_value(value, pair_cells, address_cells);
	return 0;
}

/*
 * Finds RP1's MSI-X capability, maps its table, masks every entry and
 * turns MSI-X on.
 */
static int
find_msix(
	void)
{
	volatile uint8_t *entry;
	uint32_t dword;
	uint32_t table;
	uint32_t control;
	unsigned capability;
	unsigned steps;
	unsigned index;
	int error;

	/* Refuses a function without a capability list. */
	error = drv_rp1_config_read32(PCI_STATUS_COMMAND, &dword);
	if (error != 0)
		return error;
	if ((dword & PCI_STATUS_CAPABILITIES) == 0)
		return ENOENT;

	/* Walks the list to the MSI-X capability, a bounded number of steps. */
	error = drv_rp1_config_read32(PCI_CAPABILITY_POINTER, &dword);
	if (error != 0)
		return error;
	capability = dword & 0xfcU;
	for (steps = 0; steps < PCI_CAPABILITY_LIMIT; steps++) {
		/* The end of the list has no MSI-X. */
		if (capability == 0)
			return ENOENT;

		/* Stops at the MSI-X capability. */
		error = drv_rp1_config_read32(capability, &dword);
		if (error != 0)
			return error;
		if ((dword & 0xffU) == PCI_CAPABILITY_MSIX)
			break;

		/* Moves to the next capability. */
		capability = (dword >> 8) & 0xfcU;
	}

	/* Refuses a list that does not end. */
	if (steps == PCI_CAPABILITY_LIMIT)
		return ELOOP;

	/* Reads the table's size and place. */
	control = dword >> PCI_MSIX_CONTROL_SHIFT;
	rp1_irq.table_size = (control & PCI_MSIX_TABLE_SIZE_MASK) + 1U;
	error = drv_rp1_config_read32(capability + PCI_MSIX_TABLE, &table);
	if (error != 0)
		return error;

	/* Refuses a table outside the BAR the module knows. */
	if ((table & PCI_MSIX_BIR_MASK) != RP1_MSIX_BAR)
		return ENOTSUP;

	/* Maps the table. */
	error = map_window(RP1_MSIX_BAR, table & ~PCI_MSIX_BIR_MASK, (size_t)rp1_irq.table_size * PCI_MSIX_ENTRY_BYTES, &rp1_irq.msix_table);
	if (error != 0)
		return error;

	/* Masks every entry, so no vector fires before it is routed. */
	for (index = 0; index < rp1_irq.table_size; index++) {
		entry = rp1_irq.msix_table + index * PCI_MSIX_ENTRY_BYTES;
		kern_mmio_write32(entry + PCI_MSIX_ENTRY_CONTROL, PCI_MSIX_ENTRY_MASKED);
	}

	/* Turns MSI-X on, without the function mask. */
	control |= PCI_MSIX_ENABLE;
	control &= ~PCI_MSIX_FUNCTION_MASK;
	dword &= 0xffffU;
	dword |= control << PCI_MSIX_CONTROL_SHIFT;
	error = drv_rp1_config_write32(capability, dword);
	if (error != 0)
		return error;

	/* Succeeded: RP1 sends messages through the table. */
	return 0;
}

/* Maps a range of one of RP1's BARs. */
static int
map_window(
	unsigned bar,
	uint64_t offset,
	size_t size,
	volatile uint8_t **mapped)
{
	uint64_t pci_address;
	uint64_t cpu_address;
	void *address;
	int error;

	/* Finds the range on the PCI bus. */
	error = drv_rp1_bar_pci_address(bar, &pci_address);
	if (error != 0)
		return error;

	/* Finds it on the CPU's bus. */
	error = drv_rp1_pci_to_cpu(pci_address + offset, &cpu_address);
	if (error != 0)
		return error;

	/* Maps it uncached. */
	error = kern_device_map(cpu_address, size, KERN_DEVICE_UNCACHED, &address);
	if (error != 0)
		return error;

	/* Succeeded: the range can be reached. */
	*mapped = address;
	return 0;
}

/*
 * Serves one MIP line: runs the vector's handler, acknowledges a level
 * vector in RP1 so it can be sent again, and ends the interrupt.
 */
static void
dispatch(
	int irq,
	kern_irq_ack_t acknowledge,
	void *argument)
{
	struct rp1_irq_slot *slot;
	volatile uint8_t *config;

	(void)irq;

	/* Runs the handler of the vector the line was given to. */
	slot = argument;
	slot->handler(slot->argument);

	/* Lets RP1 send a level vector again now that it is served. */
	if (slot->level) {
		config = rp1_irq.msix_config + RP1_MSIX_CFG_FIRST + slot->vector * 4U;
		kern_mmio_write32(config + RP1_MSIX_CFG_SET, RP1_MSIX_CFG_IACK);
	}

	/* Ends the interrupt. */
	kern_irq_send_eoi(acknowledge);
}
