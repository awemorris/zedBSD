/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The Raspberry Pi 5's RP1 south bridge, as the firmware leaves it.
 *
 * The device tree puts an rp1 node, a bus of RP1's peripherals, under the
 * PCIe x4 root complex.  The root complex's registers give the link state
 * and the configuration space of the bus behind the root port, where RP1
 * is device 0.  RP1's second BAR is the window of its peripherals; an
 * address in the rp1 node's space is an offset into that window.
 *
 * The firmware trains the link and, with pciex4_reset=0, leaves it up,
 * but the rest of what it leaves follows its own plan, not the device
 * tree's: the root port without bus numbers, its outbound window at a CPU
 * address the tree does not name and RP1's BARs elsewhere on the PCI bus.
 * The module sets all of them from the tree as an enumeration would: the
 * root port's buses and memory window, the outbound windows from the
 * ranges, RP1's peripheral BAR where the rp1 node's ranges place it, and
 * the inbound windows from the dma-ranges, because RP1's DMA and its
 * interrupt messages travel through them.
 */

#include <drivers/platform/rpi5/rp1.h>
#include <kern/device-io.h>
#include <kern/kcrt.h>
#include <kern/klog.h>
#include <kern/pmem.h>
#include <uapi/errno.h>

/* The most bytes of the firmware's device tree the module reads. */
#define RP1_FDT_READ_LIMIT		(2U * 1024U * 1024U)

/*
 * The root complex's registers: the link status, the window that reaches
 * the configuration space of a function behind the root port, which the
 * index register selects by bus, device and function, and the inbound
 * windows (an RC BAR's PCI base and size, and where it lands on the CPU's
 * bus).  The root port's own configuration space is at the start of the
 * registers.
 *
 * Values referenced: OpenBSD sys/dev/fdt/bcm2711_pcie.c rev 1.20
 * (PCIE_MISC_PCIE_STATUS and its PHYLINKUP and DL_ACTIVE bits,
 * PCIE_EXT_CFG_DATA, PCIE_EXT_CFG_INDEX, bcmpcie_make_tag(),
 * PCIE_MISC_RC_BAR1_CONFIG_LO/HI and the size code of
 * bcmpcie_setup_inbound(), PCIE_MISC_UBUS_BAR1_CONFIG_REMAP_LO/HI and
 * its enable bit).
 */
#define PCIE_MISC_PCIE_STATUS		0x4068U
#define PCIE_STATUS_PHYLINKUP		(1U << 4)
#define PCIE_STATUS_DL_ACTIVE		(1U << 5)
#define PCIE_EXT_CFG_DATA		0x8000U
#define PCIE_EXT_CFG_INDEX		0x9000U
#define PCIE_EXT_CFG_BUS_SHIFT		20U
#define PCIE_RC_BAR_CONFIG_LO		0x402cU
#define PCIE_RC_BAR_CONFIG_HI		0x4030U
#define PCIE_UBUS_BAR_REMAP_LO		0x40acU
#define PCIE_UBUS_BAR_REMAP_HI		0x40b0U
#define PCIE_INBOUND_STRIDE		8U
#define PCIE_INBOUND_COUNT		3U
#define PCIE_RC_BAR_SIZE_MASK		0x1fU
#define PCIE_UBUS_REMAP_ENABLE		(1U << 0)
#define PCIE_RC_BAR_SIZE_SMALL_BASE	0x1cU
#define PCIE_RC_BAR_SIZE_SMALL_FIRST	12U
#define PCIE_RC_BAR_SIZE_SMALL_LAST	15U
#define PCIE_RC_BAR_SIZE_LARGE_LAST	36U
#define PCIE_RC_BAR_SIZE_LARGE_BIAS	15U

/*
 * The root complex's controls the module sets: the mode in which a
 * configuration read that no function answers returns all ones instead
 * of aborting, and the outbound windows, the CPU's ways to the PCI bus
 * (each one's PCI base, and its CPU base and limit in MiB, the low twelve
 * bits in one register and the high bits in two more; the windows' PCI
 * bases and high bits are eight bytes apart, their low bits four).
 *
 * Values referenced: Raspberry Pi Linux rpi-6.12.y
 * drivers/pci/controller/pcie-brcmstb.c (PCIE_MISC_MISC_CTRL and its
 * CFG_READ_UR_MODE bit, PCIE_MISC_CPU_2_PCIE_MEM_WIN0_LO/HI,
 * PCIE_MISC_CPU_2_PCIE_MEM_WIN0_BASE_LIMIT with its base and limit
 * fields, PCIE_MISC_CPU_2_PCIE_MEM_WIN0_BASE_HI/LIMIT_HI and their masks,
 * as brcm_pcie_set_outbound_win() fills them).
 */
#define PCIE_MISC_CONTROL		0x4008U
#define PCIE_MISC_CONTROL_READ_UR_ONES	(1U << 13)
#define PCIE_OUTBOUND_PCI_LO		0x400cU
#define PCIE_OUTBOUND_PCI_HI		0x4010U
#define PCIE_OUTBOUND_BASE_LIMIT	0x4070U
#define PCIE_OUTBOUND_BASE_HI		0x4080U
#define PCIE_OUTBOUND_LIMIT_HI		0x4084U
#define PCIE_OUTBOUND_BASE_SHIFT	4U
#define PCIE_OUTBOUND_LIMIT_SHIFT	20U
#define PCIE_OUTBOUND_LOW_MIB_MASK	0xfffU
#define PCIE_OUTBOUND_HIGH_MIB_SHIFT	12U
#define PCIE_OUTBOUND_HIGH_MIB_MASK	0xffU
#define PCIE_MIB_SHIFT			20U
#define PCIE_OUTBOUND_COUNT		4U
#define PCIE_OUTBOUND_PAIR_STRIDE	8U
#define PCIE_OUTBOUND_LOW_STRIDE	4U
#define PCIE_OUTBOUND_BASE_FIELD	0x0000fff0U
#define PCIE_OUTBOUND_LIMIT_FIELD	0xfff00000U

/* The bytes of the root complex's register block the module maps. */
#define PCIE_REGISTER_BYTES		0xa000U

/* The configuration registers the module reads and writes. */
#define PCI_ID				0x00U
#define PCI_COMMAND			0x04U
#define PCI_HEADER			0x0cU
#define PCI_BUS_NUMBERS			0x18U
#define PCI_MEMORY_WINDOW		0x20U
#define PCI_PREFETCH_WINDOW		0x24U
#define PCI_PREFETCH_BASE_HIGH		0x28U
#define PCI_PREFETCH_LIMIT_HIGH		0x2cU
#define PCI_COMMAND_MEMORY		(1U << 1)
#define PCI_COMMAND_MASTER		(1U << 2)
#define PCI_BAR0			0x10U

/* The header type of a bridge, and the field that holds it. */
#define PCI_HEADER_TYPE_SHIFT		16U
#define PCI_HEADER_TYPE_MASK		0x7fU
#define PCI_HEADER_TYPE_BRIDGE		1U

/*
 * The bus numbers the module gives the root port the firmware left
 * unnumbered (primary 0, secondary and subordinate 1, the latency byte
 * kept), and the windows it gives every root port: memory (all of the
 * 32-bit space, where the outbound window and RP1's BARs are) and
 * prefetchable (base above limit: closed).
 */
#define ROOT_PORT_BUS			1U
#define PCI_BUS_NUMBERS_KEEP		0xff000000U
#define PCI_MEMORY_WINDOW_ALL		0xfff00000U
#define PCI_PREFETCH_WINDOW_CLOSED	0x0000fff0U

/*
 * RP1's identity and its two BARs: the MSI-X table's and the peripherals'.
 *
 * Values referenced: OpenBSD sys/dev/pci/pcidevs (vendor RPI 0x1de4,
 * product RP1 0x0001) and sys/arch/arm64/dev/rpone.c rev 1.1
 * (RP1_MSIX_BAR, the first BAR, and RP1_PERIPH_BAR, the second).
 */
#define RP1_IDENTITY			0x00011de4U
#define RP1_BAR_COUNT			2U
#define RP1_MSIX_BAR			0U
#define RP1_PERIPHERAL_BAR		1U

/* A BAR's type bits: a 64-bit memory BAR carries its high half in the next register. */
#define PCI_BAR_TYPE_MASK		0x6U
#define PCI_BAR_TYPE_64BIT		0x4U
#define PCI_BAR_IO			0x1U
#define PCI_BAR_ADDRESS_MASK		0xfffffff0U
#define PCI_BAR_SIZING			0xffffffffU

/* The space code of a PCI address's first cell: memory, 32-bit or 64-bit. */
#define PCI_SPACE_SHIFT			24U
#define PCI_SPACE_MASK			0x3U
#define PCI_SPACE_MEMORY32		0x2U
#define PCI_SPACE_MEMORY64		0x3U

/* The cells of a PCI address in the device tree: the space code and two address cells. */
#define PCI_ADDRESS_CELLS		3U

/*
 * What the module found of RP1.
 *
 * Filled once by drv_rp1_init() during the platform's start-up, before
 * any peripheral driver asks, and not changed afterwards; valid is zero
 * until everything is found.
 */
struct rp1_state {
	int valid;
	struct drv_fdt fdt;
	uint32_t pcie_node;
	uint32_t rp1_node;
	volatile uint8_t *pcie_registers;
	uint8_t bus;
	uint64_t window_child_base;
	uint64_t window_pci_base;
	uint64_t window_size;
	uint64_t bar_pci_base[RP1_BAR_COUNT];
	uint64_t dma_bus_offset;
	uint64_t dma_limit;
	int dma_coherent;
};

/* The one RP1 a Pi 5 has. */
static struct rp1_state rp1;

/* The order in which the BARs are placed: the peripheral BAR first, the MSI-X table's last. */
static const uint8_t rp1_bar_order[] = { 1U, 2U, 3U, 4U, 5U, 0U };

static int find_rp1(void);
static int is_bus_of_rp1(uint32_t node);
static int read_window(void);
static int map_root_complex(void);
static int check_link(void);
static void report_root_port(void);
static int number_root_port(void);
static void open_root_port(void);
static int read_range(uint32_t entry, uint32_t *space, uint64_t *pci_base, uint64_t *cpu_base, uint64_t *size);
static int find_memory_range(uint64_t pci_address, uint64_t *pci_base, uint64_t *cpu_base, uint64_t *size);
static int program_outbound_windows(void);
static int assign_bars(void);
static uint32_t config_read32(unsigned offset);
static void config_write32(unsigned offset, uint32_t value);
static int read_bars(void);
static int read_inbound(unsigned index, uint64_t *pci_base, uint64_t *cpu_base, uint64_t *size);
static int inbound_to_cpu(uint64_t pci_address, uint64_t *cpu_address, uint64_t *remaining);
static int program_inbound_windows(void);
static uint32_t inbound_size_code(uint64_t size);
static int read_dma_window(void);

/*
 * Finds RP1 in the device tree at fdt_phys and checks it is reachable.
 */
int
drv_rp1_init(
	uint64_t fdt_phys)
{
	const void *blob;
	uint32_t identity;
	uint32_t command;
	uint32_t control;
	int error;

	/* Opens the device tree through the direct map. */
	blob = kern_pmem_to_kernel((kern_physaddr_t)fdt_phys);
	if (blob == NULL)
		return ENODEV;
	error = drv_fdt_open(&rp1.fdt, blob, RP1_FDT_READ_LIMIT);
	if (error != 0)
		return error;

	/* Finds the rp1 node and the root complex above it. */
	error = find_rp1();
	if (error != 0) {
		kern_logf("rp1: no RP1 in the device tree\n");
		return ENODEV;
	}

	/* Reads where the rp1 node's addresses lie. */
	error = read_window();
	if (error != 0) {
		kern_logf("rp1: unusable ranges (%d)\n", error);
		return error;
	}

	/* Maps the root complex's registers. */
	error = map_root_complex();
	if (error != 0) {
		kern_logf("rp1: root complex registers unavailable (%d)\n", error);
		return error;
	}

	/* Refuses a link the firmware took down; RP1 cannot be reached then. */
	error = check_link();
	if (error != 0)
		return error;

	/* Records the root port as the firmware left it, for the log. */
	report_root_port();

	/* Makes a configuration read that no function answers return ones rather than abort. */
	control = kern_mmio_read32(rp1.pcie_registers + PCIE_MISC_CONTROL);
	kern_mmio_write32(rp1.pcie_registers + PCIE_MISC_CONTROL, control | PCIE_MISC_CONTROL_READ_UR_ONES);

	/* Gives the root port the bus behind it when the firmware left none. */
	if (rp1.bus == 0) {
		error = number_root_port();
		if (error != 0)
			return error;
	}

	/* Lets the root port pass the 32-bit memory space both ways. */
	open_root_port();

	/* Sets the CPU's ways to the PCI bus from the tree's ranges. */
	error = program_outbound_windows();
	if (error != 0) {
		kern_logf("rp1: no outbound window (%d)\n", error);
		return error;
	}

	/* Checks that the function behind the root port is RP1. */
	identity = config_read32(PCI_ID);
	kern_logf("rp1: PCIe link up (status %08x), bus %u device 0 is %08x\n",
		  kern_mmio_read32(rp1.pcie_registers + PCIE_MISC_PCIE_STATUS),
		  (unsigned)rp1.bus,
		  identity);
	if (identity != RP1_IDENTITY) {
		kern_logf("rp1: bus %u device 0 is %08x, not RP1\n", (unsigned)rp1.bus, identity);
		return ENODEV;
	}

	/* Finds where the firmware placed the MSI-X table and the peripheral window. */
	error = read_bars();
	if (error != 0)
		return error;

	/*
	 * Places the BARs as the tree has them when the firmware left them
	 * unplaced (both at one address) or put the peripheral window
	 * elsewhere than the rp1 node's ranges say.
	 */
	if (rp1.bar_pci_base[RP1_MSIX_BAR] == rp1.bar_pci_base[RP1_PERIPHERAL_BAR] ||
	    rp1.bar_pci_base[RP1_PERIPHERAL_BAR] != rp1.window_pci_base) {
		error = assign_bars();
		if (error != 0)
			return error;
		error = read_bars();
		if (error != 0)
			return error;
	}

	/* Opens the paths RP1's DMA and interrupt messages take into memory. */
	error = program_inbound_windows();
	if (error != 0) {
		kern_logf("rp1: unusable dma-ranges (%d)\n", error);
		return error;
	}

	/* Finds the addresses RP1's peripherals use to reach memory. */
	error = read_dma_window();
	if (error != 0) {
		kern_logf("rp1: no DMA window for the peripherals (%d)\n", error);
		return error;
	}

	/* Lets RP1 decode its BARs and reach memory. */
	command = config_read32(PCI_COMMAND) & 0xffffU;
	command |= PCI_COMMAND_MEMORY;
	command |= PCI_COMMAND_MASTER;
	config_write32(PCI_COMMAND, command);

	/* Reports what was found. */
	kern_logf("rp1: bus %u, MSI-X table at PCI %llx, peripherals at PCI %llx (%llx bytes), DMA offset %llx\n",
		  (unsigned)rp1.bus,
		  (unsigned long long)rp1.bar_pci_base[RP1_MSIX_BAR],
		  (unsigned long long)rp1.bar_pci_base[RP1_PERIPHERAL_BAR],
		  (unsigned long long)rp1.window_size,
		  (unsigned long long)rp1.dma_bus_offset);

	/* Succeeded: RP1's peripherals can be reached. */
	rp1.valid = 1;
	return 0;
}

/*
 * Reports the device tree RP1 was found in.
 */
const struct drv_fdt *
drv_rp1_fdt(
	void)
{
	/* No tree before RP1 was found. */
	if (!rp1.valid)
		return NULL;

	/* Succeeded: the opened tree. */
	return &rp1.fdt;
}

/*
 * Reports the rp1 node.
 */
uint32_t
drv_rp1_node(
	void)
{
	/* No node before RP1 was found. */
	if (!rp1.valid)
		return DRV_FDT_NO_NODE;

	/* Succeeded: the bus of RP1's peripherals. */
	return rp1.rp1_node;
}

/*
 * Reports the root complex above RP1.
 */
uint32_t
drv_rp1_root_complex_node(
	void)
{
	/* No node before RP1 was found. */
	if (!rp1.valid)
		return DRV_FDT_NO_NODE;

	/* Succeeded: the PCIe x4 root complex. */
	return rp1.pcie_node;
}

/*
 * Finds the next child of the rp1 node with a compatible string.
 */
int
drv_rp1_find_child(
	const char *compatible,
	uint32_t after,
	uint32_t *node)
{
	uint32_t child;
	unsigned index;
	int error;

	/* Refuses a request before RP1 was found, or with nowhere to put the answer. */
	if (!rp1.valid)
		return ENODEV;
	if (compatible == NULL || node == NULL)
		return EINVAL;

	/* Walks the rp1 node's children from the one after the last found. */
	child = after;
	for (;;) {
		error = drv_fdt_child(&rp1.fdt, rp1.rp1_node, child, &child);
		if (error != 0)
			return error;

		/* A child whose compatible list names the string is the answer. */
		error = drv_fdt_string_index(&rp1.fdt, child, "compatible", compatible, &index);
		if (error == 0)
			break;
	}

	/* Succeeded: the child. */
	*node = child;
	return 0;
}

/*
 * Reads one address and size pair of an rp1 child's reg, in CPU terms.
 */
int
drv_rp1_reg(
	uint32_t node,
	unsigned index,
	uint64_t *cpu_address,
	uint64_t *size)
{
	const uint8_t *value;
	uint64_t rp1_address;
	uint64_t pci_address;
	uint32_t length;
	uint32_t address_cells;
	uint32_t size_cells;
	uint32_t pair_cells;
	int error;

	/* Refuses a request before RP1 was found, or with nowhere to put the answer. */
	if (!rp1.valid)
		return ENODEV;
	if (cpu_address == NULL || size == NULL)
		return EINVAL;

	/* The rp1 node gives its children its own cell counts. */
	address_cells = drv_fdt_node_cells(&rp1.fdt, rp1.rp1_node, "#address-cells", 2U);
	size_cells = drv_fdt_node_cells(&rp1.fdt, rp1.rp1_node, "#size-cells", 1U);
	if (address_cells == 0 || address_cells > 2U || size_cells > 2U)
		return ERANGE;

	/* Finds the requested pair inside the reg property. */
	error = drv_fdt_property(&rp1.fdt, node, "reg", &value, &length);
	if (error != 0)
		return error;
	pair_cells = address_cells + size_cells;
	if ((uint64_t)(index + 1U) * pair_cells * 4U > length)
		return ENOENT;
	rp1_address = drv_fdt_cells_value(value, index * pair_cells, address_cells);
	*size = drv_fdt_cells_value(value, index * pair_cells + address_cells, size_cells);

	/* Moves the address into RP1's window on the PCI bus. */
	error = drv_rp1_to_pci(rp1_address, &pci_address);
	if (error != 0)
		return error;

	/* Moves it on through the root complex's outbound window. */
	error = drv_rp1_pci_to_cpu(pci_address, cpu_address);
	if (error != 0)
		return error;

	/* Succeeded: the pair in CPU terms. */
	return 0;
}

/*
 * Maps one address and size pair of an rp1 child's reg as device memory.
 */
int
drv_rp1_map(
	uint32_t node,
	unsigned index,
	volatile uint8_t **mapped,
	uint64_t *size)
{
	uint64_t cpu_address;
	void *address;
	int error;

	/* Refuses a request with nowhere to put the answer. */
	if (mapped == NULL || size == NULL)
		return EINVAL;

	/* Finds the pair on the CPU's bus. */
	error = drv_rp1_reg(node, index, &cpu_address, size);
	if (error != 0)
		return error;

	/* Refuses an empty pair. */
	if (*size == 0)
		return ERANGE;

	/* Maps it uncached, as every register block is. */
	error = kern_device_map(cpu_address, (size_t)*size, KERN_DEVICE_UNCACHED, &address);
	if (error != 0)
		return error;

	/* Succeeded: the registers can be reached. */
	*mapped = address;
	return 0;
}

/*
 * Translates an address of the rp1 node's own space to the PCI bus.
 */
int
drv_rp1_to_pci(
	uint64_t rp1_address,
	uint64_t *pci_address)
{
	uint64_t offset;

	/* Refuses a request before the window was read, or with nowhere to put the answer. */
	if (rp1.window_size == 0)
		return ENODEV;
	if (pci_address == NULL)
		return EINVAL;

	/* Refuses an address outside RP1's window. */
	if (rp1_address < rp1.window_child_base)
		return ENOENT;
	offset = rp1_address - rp1.window_child_base;
	if (offset >= rp1.window_size)
		return ENOENT;

	/* Succeeded: the same offset into the peripheral BAR. */
	*pci_address = rp1.bar_pci_base[RP1_PERIPHERAL_BAR] + offset;
	return 0;
}

/*
 * Reports how RP1's peripherals reach memory when they master.
 */
int
drv_rp1_dma_window(
	uint64_t *bus_offset,
	uint64_t *physical_limit,
	int *coherent)
{
	/* Refuses a request before RP1 was found, or with nowhere to put the answer. */
	if (!rp1.valid)
		return ENODEV;
	if (bus_offset == NULL || physical_limit == NULL || coherent == NULL)
		return EINVAL;

	/* Succeeded: the window read from the rp1 node's dma-ranges. */
	*bus_offset = rp1.dma_bus_offset;
	*physical_limit = rp1.dma_limit;
	*coherent = rp1.dma_coherent;
	return 0;
}

/*
 * Reads one 32-bit register of RP1's PCI configuration space.
 */
int
drv_rp1_config_read32(
	unsigned offset,
	uint32_t *value)
{
	/* Refuses a request before RP1 was found, or with nowhere to put the answer. */
	if (!rp1.valid)
		return ENODEV;
	if (value == NULL)
		return EINVAL;

	/* Succeeded: the register's value. */
	*value = config_read32(offset);
	return 0;
}

/*
 * Writes one 32-bit register of RP1's PCI configuration space.
 */
int
drv_rp1_config_write32(
	unsigned offset,
	uint32_t value)
{
	/* Refuses a request before RP1 was found. */
	if (!rp1.valid)
		return ENODEV;

	/* Writes the register through the configuration window. */
	config_write32(offset, value);

	/* Succeeded: the register holds the value. */
	return 0;
}

/*
 * Reports the address on the PCI bus of one of RP1's own BARs.
 */
int
drv_rp1_bar_pci_address(
	unsigned bar,
	uint64_t *pci_address)
{
	/* Refuses a request before RP1 was found, a BAR it does not have, or nowhere to put the answer. */
	if (!rp1.valid)
		return ENODEV;
	if (bar >= RP1_BAR_COUNT || pci_address == NULL)
		return EINVAL;

	/* Succeeded: where the firmware placed the BAR. */
	*pci_address = rp1.bar_pci_base[bar];
	return 0;
}

/*
 * Translates a PCI memory address to the CPU's through the root complex's
 * ranges, then through the buses above it.
 */
int
drv_rp1_pci_to_cpu(
	uint64_t pci_address,
	uint64_t *cpu_address)
{
	uint64_t pci_base;
	uint64_t cpu_base;
	uint64_t size;
	int error;

	/* Refuses a request with nowhere to put the answer. */
	if (cpu_address == NULL)
		return EINVAL;

	/* Finds the memory range that holds the address. */
	error = find_memory_range(pci_address, &pci_base, &cpu_base, &size);
	if (error != 0)
		return error;

	/* Succeeded: the same offset into the range on the CPU's bus. */
	*cpu_address = cpu_base + (pci_address - pci_base);
	return 0;
}

/*
 * Finds the rp1 node: a bus that is an interrupt controller, under an
 * enabled BCM2712 root complex.
 */
static int
find_rp1(
	void)
{
	uint32_t pcie;
	uint32_t child;
	bool enabled;
	int error;
	int found;

	/* Looks at each BCM2712 root complex in turn. */
	pcie = DRV_FDT_NO_NODE;
	for (;;) {
		error = drv_fdt_find_compatible(&rp1.fdt, "brcm,bcm2712-pcie", pcie, &pcie);
		if (error != 0)
			return error;

		/* A disabled root complex has no RP1 the firmware set up. */
		enabled = drv_fdt_node_enabled(&rp1.fdt, pcie);
		if (!enabled)
			continue;

		/* Looks at the root complex's children for RP1's bus. */
		child = DRV_FDT_NO_NODE;
		for (;;) {
			error = drv_fdt_child(&rp1.fdt, pcie, child, &child);
			if (error != 0)
				break;

			/* RP1's bus is the child that is also an interrupt controller. */
			found = is_bus_of_rp1(child);
			if (found) {
				rp1.pcie_node = pcie;
				rp1.rp1_node = child;

				/* Succeeded: the rp1 node and its root complex. */
				return 0;
			}
		}
	}
}

/* Tells whether a node is RP1's bus: a simple bus that is an interrupt controller. */
static int
is_bus_of_rp1(
	uint32_t node)
{
	const uint8_t *value;
	uint32_t length;
	unsigned index;
	int error;

	/* RP1's bus is a simple bus. */
	error = drv_fdt_string_index(&rp1.fdt, node, "compatible", "simple-bus", &index);
	if (error != 0)
		return 0;

	/* It is also the interrupt controller of its peripherals. */
	error = drv_fdt_property(&rp1.fdt, node, "interrupt-controller", &value, &length);
	if (error != 0)
		return 0;

	/* Succeeded: the node is RP1's bus. */
	return 1;
}

/*
 * Reads the rp1 node's ranges: the base and size of the space its
 * children's addresses are in, which is RP1's peripheral window.
 */
static int
read_window(
	void)
{
	const uint8_t *value;
	uint32_t length;
	uint32_t child_cells;
	uint32_t parent_cells;
	uint32_t size_cells;
	uint32_t entry_cells;
	int error;

	/* Reads the first entry of the ranges. */
	error = drv_fdt_property(&rp1.fdt, rp1.rp1_node, "ranges", &value, &length);
	if (error != 0)
		return error;

	/* An entry is a child address, a PCI address and a size. */
	child_cells = drv_fdt_node_cells(&rp1.fdt, rp1.rp1_node, "#address-cells", 2U);
	parent_cells = drv_fdt_node_cells(&rp1.fdt, rp1.pcie_node, "#address-cells", PCI_ADDRESS_CELLS);
	size_cells = drv_fdt_node_cells(&rp1.fdt, rp1.rp1_node, "#size-cells", 1U);
	if (child_cells == 0 || child_cells > 2U || size_cells == 0 || size_cells > 2U)
		return ERANGE;
	entry_cells = child_cells + parent_cells + size_cells;
	if (entry_cells * 4U > length)
		return ERANGE;

	/*
	 * Keeps the child base, the PCI base (skipping the space code) and
	 * the size.  The firmware places the peripheral BAR at that PCI base;
	 * the module places it there when the firmware did not.
	 */
	rp1.window_child_base = drv_fdt_cells_value(value, 0, child_cells);
	rp1.window_pci_base = drv_fdt_cells_value(value, child_cells + 1U, 2U);
	rp1.window_size = drv_fdt_cells_value(value, child_cells + parent_cells, size_cells);
	if (rp1.window_size == 0)
		return ERANGE;

	/* Succeeded: the window is known. */
	return 0;
}

/* Maps the root complex's register block. */
static int
map_root_complex(
	void)
{
	uint64_t register_base;
	uint64_t register_size;
	void *mapped;
	int error;

	/* Finds the block on the CPU's bus. */
	error = drv_fdt_reg(&rp1.fdt, rp1.pcie_node, 0, &register_base, &register_size);
	if (error != 0)
		return error;

	/* Covers at least the configuration window and its index register. */
	if (register_size < PCIE_REGISTER_BYTES)
		register_size = PCIE_REGISTER_BYTES;

	/* Maps it uncached. */
	error = kern_device_map(register_base, (size_t)register_size, KERN_DEVICE_UNCACHED, &mapped);
	if (error != 0)
		return error;

	/* Succeeded: the registers can be reached. */
	rp1.pcie_registers = mapped;
	return 0;
}

/*
 * Checks that the link to RP1 is up and finds the bus behind the root port.
 */
static int
check_link(
	void)
{
	uint32_t status;
	uint32_t numbers;

	/* The physical link and the data link layer must both be up. */
	status = kern_mmio_read32(rp1.pcie_registers + PCIE_MISC_PCIE_STATUS);
	if ((status & PCIE_STATUS_PHYLINKUP) == 0 || (status & PCIE_STATUS_DL_ACTIVE) == 0) {
		kern_logf("rp1: PCIe link down (status %08x); config.txt needs pciex4_reset=0\n", status);
		return ENODEV;
	}

	/* The root port's secondary bus is where RP1 is; zero means the firmware numbered none. */
	numbers = kern_mmio_read32(rp1.pcie_registers + PCI_BUS_NUMBERS);
	rp1.bus = (uint8_t)(numbers >> 8);

	/* Succeeded: the link is up. */
	return 0;
}

/*
 * Logs the root port's configuration and the first outbound window as the
 * firmware left them.
 */
static void
report_root_port(
	void)
{
	volatile uint8_t *registers;

	registers = rp1.pcie_registers;

	/* The root port's own configuration header. */
	kern_logf("rp1: root port: command %08x, header %08x, buses %08x, windows %08x/%08x, control %08x\n",
		  kern_mmio_read32(registers + PCI_COMMAND),
		  kern_mmio_read32(registers + PCI_HEADER),
		  kern_mmio_read32(registers + PCI_BUS_NUMBERS),
		  kern_mmio_read32(registers + PCI_MEMORY_WINDOW),
		  kern_mmio_read32(registers + PCI_PREFETCH_WINDOW),
		  kern_mmio_read32(registers + PCIE_MISC_CONTROL));

	/* The CPU's way to the PCI bus. */
	kern_logf("rp1: outbound window: PCI %08x%08x, MiB %08x high %08x/%08x\n",
		  kern_mmio_read32(registers + PCIE_OUTBOUND_PCI_HI),
		  kern_mmio_read32(registers + PCIE_OUTBOUND_PCI_LO),
		  kern_mmio_read32(registers + PCIE_OUTBOUND_BASE_LIMIT),
		  kern_mmio_read32(registers + PCIE_OUTBOUND_BASE_HI),
		  kern_mmio_read32(registers + PCIE_OUTBOUND_LIMIT_HI));
}

/*
 * Numbers the bus behind a root port the firmware left unnumbered, as an
 * enumeration would: the root port's secondary bus is 1.
 */
static int
number_root_port(
	void)
{
	volatile uint8_t *registers;
	uint32_t header;
	uint32_t numbers;

	registers = rp1.pcie_registers;

	/* Refuses a root port that is not a bridge: it has no bus numbers. */
	header = kern_mmio_read32(registers + PCI_HEADER);
	if (((header >> PCI_HEADER_TYPE_SHIFT) & PCI_HEADER_TYPE_MASK) != PCI_HEADER_TYPE_BRIDGE) {
		kern_logf("rp1: the root port has no bus behind it and is no bridge (header %08x)\n", header);
		return ENODEV;
	}

	/* Numbers the buses: primary 0, secondary and subordinate 1. */
	numbers = kern_mmio_read32(registers + PCI_BUS_NUMBERS) & PCI_BUS_NUMBERS_KEEP;
	numbers |= ROOT_PORT_BUS << 16;
	numbers |= ROOT_PORT_BUS << 8;
	kern_mmio_write32(registers + PCI_BUS_NUMBERS, numbers);

	/* Records the change. */
	rp1.bus = (uint8_t)ROOT_PORT_BUS;
	kern_logf("rp1: the root port had no bus behind it; numbered bus %u (buses now %08x)\n",
		  (unsigned)rp1.bus,
		  kern_mmio_read32(registers + PCI_BUS_NUMBERS));

	/* Succeeded: RP1 can be reached on the bus. */
	return 0;
}

/*
 * Opens the root port's windows as the module places RP1's BARs: the
 * memory window passes the whole 32-bit space, the prefetchable one is
 * closed, and the port forwards memory accesses down and RP1's up.
 */
static void
open_root_port(
	void)
{
	volatile uint8_t *registers;
	uint32_t command;

	registers = rp1.pcie_registers;

	/* Passes all of the 32-bit memory space downstream and closes the prefetchable window. */
	kern_mmio_write32(registers + PCI_MEMORY_WINDOW, PCI_MEMORY_WINDOW_ALL);
	kern_mmio_write32(registers + PCI_PREFETCH_WINDOW, PCI_PREFETCH_WINDOW_CLOSED);
	kern_mmio_write32(registers + PCI_PREFETCH_BASE_HIGH, 0);
	kern_mmio_write32(registers + PCI_PREFETCH_LIMIT_HIGH, 0);

	/* Forwards memory accesses down and RP1's up. */
	command = kern_mmio_read32(registers + PCI_COMMAND) & 0xffffU;
	command |= PCI_COMMAND_MEMORY;
	command |= PCI_COMMAND_MASTER;
	kern_mmio_write32(registers + PCI_COMMAND, command);
}

/*
 * Reads one entry of the root complex's ranges: its space code, its PCI
 * base, the CPU address of that base and its size.  ENOENT past the last
 * entry.
 */
static int
read_range(
	uint32_t entry,
	uint32_t *space,
	uint64_t *pci_base,
	uint64_t *cpu_base,
	uint64_t *size)
{
	const uint8_t *value;
	uint64_t parent_base;
	uint32_t length;
	uint32_t above;
	uint32_t child_cells;
	uint32_t parent_cells;
	uint32_t size_cells;
	uint32_t entry_cells;
	int error;

	/* Reads the root complex's ranges and the widths of their entries. */
	error = drv_fdt_property(&rp1.fdt, rp1.pcie_node, "ranges", &value, &length);
	if (error != 0)
		return error;
	error = drv_fdt_parent(&rp1.fdt, rp1.pcie_node, &above);
	if (error != 0)
		return error;
	child_cells = drv_fdt_node_cells(&rp1.fdt, rp1.pcie_node, "#address-cells", PCI_ADDRESS_CELLS);
	parent_cells = drv_fdt_node_cells(&rp1.fdt, above, "#address-cells", 2U);
	size_cells = drv_fdt_node_cells(&rp1.fdt, rp1.pcie_node, "#size-cells", 2U);

	/* A PCI address has the space code and two address cells. */
	if (child_cells != PCI_ADDRESS_CELLS || parent_cells == 0 || parent_cells > 2U)
		return ERANGE;
	if (size_cells == 0 || size_cells > 2U)
		return ERANGE;

	/* Refuses an entry past the last. */
	entry_cells = child_cells + parent_cells + size_cells;
	if ((uint64_t)(entry + 1U) * entry_cells * 4U > length)
		return ENOENT;

	/* Reads the entry's space code, PCI base, CPU-side base and size. */
	*space = (uint32_t)(drv_fdt_cells_value(value, entry * entry_cells, 1U) >> PCI_SPACE_SHIFT) & PCI_SPACE_MASK;
	*pci_base = drv_fdt_cells_value(value, entry * entry_cells + 1U, 2U);
	parent_base = drv_fdt_cells_value(value, entry * entry_cells + child_cells, parent_cells);
	*size = drv_fdt_cells_value(value, entry * entry_cells + child_cells + parent_cells, size_cells);

	/* Moves the CPU side up through the buses above the root complex. */
	error = drv_fdt_translate(&rp1.fdt, rp1.pcie_node, parent_base, cpu_base);
	if (error != 0)
		return error;

	/* Succeeded: the entry's values. */
	return 0;
}

/*
 * Finds the root complex's memory range that holds a PCI address: its PCI
 * base, the CPU address of that base and its size.  ENOENT when no memory
 * range holds the address.
 */
static int
find_memory_range(
	uint64_t pci_address,
	uint64_t *pci_base,
	uint64_t *cpu_base,
	uint64_t *size)
{
	uint64_t range_pci_base;
	uint64_t range_cpu_base;
	uint64_t range_size;
	uint32_t entry;
	uint32_t space;
	int error;

	/* Looks at each range in turn. */
	for (entry = 0;
	     ;
	     entry++) {
		error = read_range(entry, &space, &range_pci_base, &range_cpu_base, &range_size);
		if (error != 0)
			return error;

		/* Only a memory range carries a BAR. */
		if (space != PCI_SPACE_MEMORY32 && space != PCI_SPACE_MEMORY64)
			continue;

		/* Skips a range that does not hold the address. */
		if (pci_address < range_pci_base)
			continue;
		if (pci_address - range_pci_base >= range_size)
			continue;

		/* Succeeded: the range. */
		*pci_base = range_pci_base;
		*cpu_base = range_cpu_base;
		*size = range_size;
		return 0;
	}
}

/*
 * Sets one outbound window for each memory range of the root complex, in
 * the ranges' order, logging any window the firmware had left otherwise.
 * The firmware leaves its own window (on a Pi 5 seen at CPU
 * 0x1c_0000_0000 for PCI 0x8000_0000), which is not where the tree, and
 * so the module, reaches RP1.
 *
 * Procedure referenced: Raspberry Pi Linux rpi-6.12.y
 * drivers/pci/controller/pcie-brcmstb.c (brcm_pcie_setup(): one window
 * per memory range, up to BRCM_NUM_PCIE_OUT_WINS; and
 * brcm_pcie_set_outbound_win(): the PCI base in two registers, the CPU
 * base and limit in MiB, their low twelve bits in one register and their
 * high bits in two more).
 */
static int
program_outbound_windows(
	void)
{
	volatile uint8_t *registers;
	uint64_t pci_base;
	uint64_t cpu_base;
	uint64_t size;
	uint64_t base_mib;
	uint64_t limit_mib;
	uint32_t old_pci_low;
	uint32_t old_base_limit;
	uint32_t old_base_high;
	uint32_t base_limit;
	uint32_t base_high;
	uint32_t limit_high;
	uint32_t entry;
	uint32_t space;
	unsigned window;
	unsigned pair;
	unsigned low;
	int error;

	registers = rp1.pcie_registers;

	/* Walks the ranges, giving each memory range the next window. */
	window = 0;
	for (entry = 0;
	     window < PCIE_OUTBOUND_COUNT;
	     entry++) {
		error = read_range(entry, &space, &pci_base, &cpu_base, &size);
		if (error == ENOENT)
			break;
		if (error != 0)
			return error;

		/* Only a memory range needs a window. */
		if (space != PCI_SPACE_MEMORY32 && space != PCI_SPACE_MEMORY64)
			continue;
		if (size == 0)
			continue;

		/* The CPU's side in MiB: the first and the last. */
		base_mib = cpu_base >> PCIE_MIB_SHIFT;
		limit_mib = (cpu_base + size - 1U) >> PCIE_MIB_SHIFT;

		/* The window's registers. */
		pair = window * PCIE_OUTBOUND_PAIR_STRIDE;
		low = window * PCIE_OUTBOUND_LOW_STRIDE;

		/* Reads what the firmware left, to report a difference. */
		old_pci_low = kern_mmio_read32(registers + PCIE_OUTBOUND_PCI_LO + pair);
		old_base_limit = kern_mmio_read32(registers + PCIE_OUTBOUND_BASE_LIMIT + low);
		old_base_high = kern_mmio_read32(registers + PCIE_OUTBOUND_BASE_HI + pair);

		/* The low twelve bits of the base and the limit, the other bits kept. */
		base_limit = old_base_limit & ~(PCIE_OUTBOUND_BASE_FIELD | PCIE_OUTBOUND_LIMIT_FIELD);
		base_limit |= (uint32_t)(base_mib & PCIE_OUTBOUND_LOW_MIB_MASK) << PCIE_OUTBOUND_BASE_SHIFT;
		base_limit |= (uint32_t)(limit_mib & PCIE_OUTBOUND_LOW_MIB_MASK) << PCIE_OUTBOUND_LIMIT_SHIFT;

		/* The high bits of the base and the limit, the other bits kept. */
		base_high = old_base_high & ~PCIE_OUTBOUND_HIGH_MIB_MASK;
		base_high |= (uint32_t)(base_mib >> PCIE_OUTBOUND_HIGH_MIB_SHIFT) & PCIE_OUTBOUND_HIGH_MIB_MASK;
		limit_high = kern_mmio_read32(registers + PCIE_OUTBOUND_LIMIT_HI + pair) & ~PCIE_OUTBOUND_HIGH_MIB_MASK;
		limit_high |= (uint32_t)(limit_mib >> PCIE_OUTBOUND_HIGH_MIB_SHIFT) & PCIE_OUTBOUND_HIGH_MIB_MASK;

		/* Writes the PCI base, then the CPU base and limit. */
		kern_mmio_write32(registers + PCIE_OUTBOUND_PCI_LO + pair, (uint32_t)pci_base);
		kern_mmio_write32(registers + PCIE_OUTBOUND_PCI_HI + pair, (uint32_t)(pci_base >> 32));
		kern_mmio_write32(registers + PCIE_OUTBOUND_BASE_LIMIT + low, base_limit);
		kern_mmio_write32(registers + PCIE_OUTBOUND_BASE_HI + pair, base_high);
		kern_mmio_write32(registers + PCIE_OUTBOUND_LIMIT_HI + pair, limit_high);

		/* Reports a window the firmware had set otherwise. */
		if (old_pci_low != (uint32_t)pci_base || old_base_limit != base_limit || old_base_high != base_high) {
			kern_logf("rp1: outbound window %u was PCI %08x MiB %08x high %08x; now PCI %llx at CPU %llx (%llx bytes)\n",
				  window,
				  old_pci_low,
				  old_base_limit,
				  old_base_high,
				  (unsigned long long)pci_base,
				  (unsigned long long)cpu_base,
				  (unsigned long long)size);
		}

		window++;
	}

	/* Refuses a root complex without any memory range: the CPU could not reach RP1. */
	if (window == 0)
		return ENOENT;

	/* Succeeded: the CPU reaches the PCI bus where the tree says. */
	return 0;
}

/*
 * Places RP1's 32-bit memory BARs where the tree has them: the peripheral
 * BAR at the PCI base the rp1 node's ranges name, and each other BAR
 * after it in turn, aligned to its size.
 */
static int
assign_bars(
	void)
{
	uint64_t next;
	uint64_t size;
	uint32_t command;
	uint32_t saved;
	uint32_t sized;
	unsigned offset;
	unsigned index;

	/* Stops RP1 decoding while its BARs move. */
	command = config_read32(PCI_COMMAND) & 0xffffU;
	config_write32(PCI_COMMAND, command & ~(PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER));

	/* Places each BAR in turn, the peripheral one first. */
	next = rp1.window_pci_base;
	for (index = 0; index < sizeof(rp1_bar_order) / sizeof(rp1_bar_order[0]); index++) {
		offset = PCI_BAR0 + 4U * rp1_bar_order[index];

		/* Sizes the BAR: the bits that stay zero after all ones are written. */
		saved = config_read32(offset);
		config_write32(offset, PCI_BAR_SIZING);
		sized = config_read32(offset);
		config_write32(offset, saved);

		/* Skips an absent BAR. */
		if (sized == 0 || sized == PCI_BAR_SIZING)
			continue;

		/* Refuses an I/O or 64-bit BAR, which RP1 does not have. */
		if ((sized & PCI_BAR_IO) != 0 || (sized & PCI_BAR_TYPE_MASK) != 0) {
			kern_logf("rp1: BAR %u is not a 32-bit memory BAR (%08x)\n", (unsigned)rp1_bar_order[index], sized);
			config_write32(PCI_COMMAND, command);
			return ENOTSUP;
		}

		/* Aligns the place to the size and writes it. */
		size = (uint64_t)(~(sized & PCI_BAR_ADDRESS_MASK)) + 1U;
		size &= 0xffffffffULL;
		next = (next + size - 1U) & ~(size - 1U);
		config_write32(offset, (uint32_t)next);
		kern_logf("rp1: BAR %u was at PCI %08x; now at PCI %llx (%llx bytes)\n",
			  (unsigned)rp1_bar_order[index],
			  saved & PCI_BAR_ADDRESS_MASK,
			  (unsigned long long)next,
			  (unsigned long long)size);
		next += size;
	}

	/* Restores the command; the caller turns decoding on later. */
	config_write32(PCI_COMMAND, command);

	/* Succeeded: every BAR has its own place. */
	return 0;
}

/* Reads a 32-bit register of RP1's configuration space. */
static uint32_t
config_read32(
	unsigned offset)
{
	uint32_t value;

	/* Selects RP1, device 0 function 0 of the bus behind the root port. */
	kern_mmio_write32(rp1.pcie_registers + PCIE_EXT_CFG_INDEX, (uint32_t)rp1.bus << PCIE_EXT_CFG_BUS_SHIFT);

	/* Reads the register through the configuration window. */
	value = kern_mmio_read32(rp1.pcie_registers + PCIE_EXT_CFG_DATA + (offset & ~3U));

	/* Succeeded: the register's value. */
	return value;
}

/* Writes a 32-bit register of RP1's configuration space. */
static void
config_write32(
	unsigned offset,
	uint32_t value)
{
	/* Selects RP1, then writes the register through the configuration window. */
	kern_mmio_write32(rp1.pcie_registers + PCIE_EXT_CFG_INDEX, (uint32_t)rp1.bus << PCIE_EXT_CFG_BUS_SHIFT);
	kern_mmio_write32(rp1.pcie_registers + PCIE_EXT_CFG_DATA + (offset & ~3U), value);
}

/* Reads where the firmware placed RP1's two BARs on the PCI bus. */
static int
read_bars(
	void)
{
	unsigned bar;
	unsigned offset;
	uint32_t low;
	uint32_t high;

	/* Reads each BAR, the MSI-X table's first, from consecutive registers. */
	offset = PCI_BAR0;
	for (bar = 0; bar < RP1_BAR_COUNT; bar++) {
		/* The BAR's low half; a 64-bit BAR has its high half next. */
		low = config_read32(offset);
		high = 0;
		offset += 4U;
		if ((low & PCI_BAR_TYPE_MASK) == PCI_BAR_TYPE_64BIT) {
			high = config_read32(offset);
			offset += 4U;
		}

		/* Refuses a BAR that cannot be read. */
		if (low == 0xffffffffU) {
			kern_logf("rp1: BAR %u unreadable\n", bar);
			return EIO;
		}

		/* Keeps the BAR's address on the PCI bus. */
		rp1.bar_pci_base[bar] = ((uint64_t)high << 32) | (low & PCI_BAR_ADDRESS_MASK);
	}

	/* Succeeded: both BARs are known. */
	return 0;
}

/*
 * Reads one entry of the root complex's dma-ranges: the PCI base of an
 * inbound window, the CPU address it lands on and its size.  ENOENT past
 * the last entry.
 */
static int
read_inbound(
	unsigned index,
	uint64_t *pci_base,
	uint64_t *cpu_base,
	uint64_t *size)
{
	const uint8_t *value;
	uint64_t parent_base;
	uint32_t length;
	uint32_t above;
	uint32_t parent_cells;
	uint32_t size_cells;
	uint32_t entry_cells;
	int error;

	/* Reads the dma-ranges and the widths of their entries. */
	error = drv_fdt_property(&rp1.fdt, rp1.pcie_node, "dma-ranges", &value, &length);
	if (error != 0)
		return error;
	error = drv_fdt_parent(&rp1.fdt, rp1.pcie_node, &above);
	if (error != 0)
		return error;
	parent_cells = drv_fdt_node_cells(&rp1.fdt, above, "#address-cells", 2U);
	size_cells = drv_fdt_node_cells(&rp1.fdt, rp1.pcie_node, "#size-cells", 2U);
	if (parent_cells == 0 || parent_cells > 2U || size_cells == 0 || size_cells > 2U)
		return ERANGE;

	/* Refuses an entry past the last. */
	entry_cells = PCI_ADDRESS_CELLS + parent_cells + size_cells;
	if ((uint64_t)(index + 1U) * entry_cells * 4U > length)
		return ENOENT;

	/* Reads the PCI side, skipping the space code, and the size. */
	*pci_base = drv_fdt_cells_value(value, index * entry_cells + 1U, 2U);
	parent_base = drv_fdt_cells_value(value, index * entry_cells + PCI_ADDRESS_CELLS, parent_cells);
	*size = drv_fdt_cells_value(value, index * entry_cells + PCI_ADDRESS_CELLS + parent_cells, size_cells);

	/* Moves the CPU side up through the buses above the root complex. */
	error = drv_fdt_translate(&rp1.fdt, rp1.pcie_node, parent_base, cpu_base);
	if (error != 0)
		return error;

	/* Succeeded: the window's three values. */
	return 0;
}

/*
 * Finds the inbound window that holds a PCI address and reports the CPU
 * address it reaches and how many bytes of the window follow it.
 */
static int
inbound_to_cpu(
	uint64_t pci_address,
	uint64_t *cpu_address,
	uint64_t *remaining)
{
	uint64_t pci_base;
	uint64_t cpu_base;
	uint64_t size;
	unsigned index;
	int error;

	/* Looks at each inbound window in turn. */
	for (index = 0;
	     ;
	     index++) {
		error = read_inbound(index, &pci_base, &cpu_base, &size);
		if (error != 0)
			return error;

		/* Skips a window that does not hold the address. */
		if (pci_address < pci_base)
			continue;
		if (pci_address - pci_base >= size)
			continue;

		/* Succeeded: the address on the CPU's bus. */
		*cpu_address = cpu_base + (pci_address - pci_base);
		*remaining = size - (pci_address - pci_base);
		return 0;
	}
}

/*
 * Sets the root complex's inbound windows from its dma-ranges, as the
 * first three RC BARs with their UBUS remaps, logging any the firmware
 * had left different.
 *
 * Procedure referenced: OpenBSD sys/dev/fdt/bcm2711_pcie.c rev 1.20
 * (bcmpcie_setup_inbound(), the BCM2712 branch: each dma-ranges entry is
 * one RC BAR, its size as a power-of-two code, and its CPU base in the
 * matching UBUS remap register with the enable bit).
 */
static int
program_inbound_windows(
	void)
{
	uint64_t pci_base;
	uint64_t cpu_base;
	uint64_t size;
	uint32_t config_low;
	uint32_t config_high;
	uint32_t remap_low;
	uint32_t remap_high;
	uint32_t old_config_low;
	uint32_t old_remap_low;
	unsigned index;
	unsigned offset;
	int error;

	/* Sets one RC BAR for each entry, as far as the controller has them. */
	for (index = 0; index < PCIE_INBOUND_COUNT; index++) {
		error = read_inbound(index, &pci_base, &cpu_base, &size);
		if (error == ENOENT)
			break;
		if (error != 0)
			return error;

		/* Encodes the window: its PCI base with the size code, and its CPU base. */
		config_low = ((uint32_t)pci_base & ~PCIE_RC_BAR_SIZE_MASK) | inbound_size_code(size);
		config_high = (uint32_t)(pci_base >> 32);
		remap_low = (uint32_t)cpu_base | PCIE_UBUS_REMAP_ENABLE;
		remap_high = (uint32_t)(cpu_base >> 32);

		/* Reads what the firmware left, to report a difference. */
		offset = index * PCIE_INBOUND_STRIDE;
		old_config_low = kern_mmio_read32(rp1.pcie_registers + PCIE_RC_BAR_CONFIG_LO + offset);
		old_remap_low = kern_mmio_read32(rp1.pcie_registers + PCIE_UBUS_BAR_REMAP_LO + offset);

		/* Reports a window the firmware had set otherwise. */
		if (old_config_low != config_low || old_remap_low != remap_low) {
			kern_logf("rp1: inbound window %u was %08x/%08x, now %08x/%08x\n",
				  index,
				  old_config_low,
				  old_remap_low,
				  config_low,
				  remap_low);
		}

		/* Writes the window. */
		kern_mmio_write32(rp1.pcie_registers + PCIE_RC_BAR_CONFIG_LO + offset, config_low);
		kern_mmio_write32(rp1.pcie_registers + PCIE_RC_BAR_CONFIG_HI + offset, config_high);
		kern_mmio_write32(rp1.pcie_registers + PCIE_UBUS_BAR_REMAP_LO + offset, remap_low);
		kern_mmio_write32(rp1.pcie_registers + PCIE_UBUS_BAR_REMAP_HI + offset, remap_high);
	}

	/* Refuses a root complex without any inbound window: nothing could reach memory. */
	if (index == 0)
		return ENOENT;

	/* Succeeded: RP1's writes reach memory and the interrupt receiver. */
	return 0;
}

/*
 * Encodes an inbound window's size as the RC BAR's size code: the power
 * of two that covers it, 4 KiB to 32 KiB as 0x1c to 0x1f and 64 KiB to
 * 64 GiB as 1 to 21; zero turns the window off.
 */
static uint32_t
inbound_size_code(
	uint64_t size)
{
	unsigned shift;

	/* Finds the smallest power of two that covers the window. */
	shift = 0;
	while (shift < 63U && ((uint64_t)1 << shift) < size)
		shift++;

	/* A small window takes the codes at the top of the field. */
	if (shift >= PCIE_RC_BAR_SIZE_SMALL_FIRST && shift <= PCIE_RC_BAR_SIZE_SMALL_LAST)
		return PCIE_RC_BAR_SIZE_SMALL_BASE + (shift - PCIE_RC_BAR_SIZE_SMALL_FIRST);

	/* A large one counts up from 64 KiB. */
	if (shift > PCIE_RC_BAR_SIZE_SMALL_LAST && shift <= PCIE_RC_BAR_SIZE_LARGE_LAST)
		return shift - PCIE_RC_BAR_SIZE_LARGE_BIAS;

	/* Succeeded: no code fits, so the window is off. */
	return 0;
}

/*
 * Reads the rp1 node's dma-ranges for the window its peripherals reach
 * memory through: the first entry whose PCI side lands on the start of
 * the CPU's memory.
 */
static int
read_dma_window(
	void)
{
	const uint8_t *value;
	uint64_t child_base;
	uint64_t pci_base;
	uint64_t size;
	uint64_t cpu_base;
	uint64_t remaining;
	uint32_t length;
	uint32_t child_cells;
	uint32_t size_cells;
	uint32_t entry_cells;
	uint32_t entry;
	int found;
	int error;

	/* Reads the dma-ranges and the widths of their entries. */
	error = drv_fdt_property(&rp1.fdt, rp1.rp1_node, "dma-ranges", &value, &length);
	if (error != 0)
		return error;
	child_cells = drv_fdt_node_cells(&rp1.fdt, rp1.rp1_node, "#address-cells", 2U);
	size_cells = drv_fdt_node_cells(&rp1.fdt, rp1.rp1_node, "#size-cells", 1U);
	if (child_cells == 0 || child_cells > 2U || size_cells == 0 || size_cells > 2U)
		return ERANGE;

	/* Looks for the entry that reaches the start of memory. */
	entry_cells = child_cells + PCI_ADDRESS_CELLS + size_cells;
	found = 0;
	for (entry = 0; (entry + 1U) * entry_cells * 4U <= length; entry++) {
		child_base = drv_fdt_cells_value(value, entry * entry_cells, child_cells);
		pci_base = drv_fdt_cells_value(value, entry * entry_cells + child_cells + 1U, 2U);
		size = drv_fdt_cells_value(value, entry * entry_cells + child_cells + PCI_ADDRESS_CELLS, size_cells);

		/* Skips an entry no inbound window of the root complex holds. */
		error = inbound_to_cpu(pci_base, &cpu_base, &remaining);
		if (error != 0)
			continue;

		/* Skips an entry that does not start at the start of memory. */
		if (cpu_base != 0)
			continue;

		/* The window ends at the shorter of the two. */
		if (remaining < size)
			size = remaining;

		/* Stops at the first entry that reaches memory. */
		if (size != 0) {
			found = 1;
			break;
		}
	}

	/* Refuses a tree where no entry reaches memory. */
	if (!found)
		return ENOENT;

	/* Keeps the window and whether it is coherent. */
	rp1.dma_bus_offset = child_base;
	rp1.dma_limit = size - 1U;
	rp1.dma_coherent = drv_fdt_dma_coherent(&rp1.fdt, rp1.rp1_node, 0);

	/* Succeeded: the peripherals' addresses for memory. */
	return 0;
}
