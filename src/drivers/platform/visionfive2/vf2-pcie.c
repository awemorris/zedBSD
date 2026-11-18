/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The VisionFive 2's two PCIe root complexes and the drivers behind them.
 *
 * For each enabled root complex the board sets the STG block's bus
 * settings, applies the pins, turns on the clocks, releases the resets,
 * tunes the PHY and finds PERST#, then hands the PLDA driver its
 * operations: PERST#, the link state in the STG block and the selection of
 * the root port's function there.  The two root complexes share the STG
 * block; each has its own register group in it, chosen by where its
 * configuration window is.
 */

#include "drivers/platform/visionfive2/vf2-pcie.h"
#include "drivers/platform/visionfive2/vf2-crg.h"
#include "drivers/platform/visionfive2/vf2-pinctrl.h"
#include "drivers/platform/visionfive2/vf2-syscon.h"
#include <drivers/pci/pci.h>
#include <drivers/pci/pci-ghci.h>
#include <drivers/pci/pci-nvme.h>
#include <drivers/pci/pci-plda.h>
#include <drivers/pci/pci-xhci.h>
#include <drivers/usb/usb.h>
#include <drivers/usb/usb-hid.h>
#include <drivers/usb/usb-uvc.h>
#include <drivers/usb/usb-hub.h>
#include <drivers/usb/usb-storage.h>
#include <kern/device-io.h>
#include <kern/kcrt.h>
#include <kern/klog.h>
#include <kern/kmem.h>
#include <kern/pmem.h>
#include <uapi/errno.h>

/*
 * Value(s) referenced: OpenBSD sys/dev/fdt/stfpcie.c 1.4 (ISC): each root
 * complex's register group in the STG block (0x048 for the one whose
 * configuration window is at 0x9_4000_0000, 0x1f8 for 0x9_c000_0000), and
 * in the group the read and write function fields with the function
 * number at bit 9 of the field, the reference clock source (2) and clock
 * request bits, the root port's no-endpoint bit and the data link active
 * bit.
 */
#define VF2_PCIE0_CONFIG		0x940000000ULL
#define VF2_PCIE1_CONFIG		0x9c0000000ULL
#define VF2_STG_PCIE0_GROUP		0x048U
#define VF2_STG_PCIE1_GROUP		0x1f8U
#define VF2_STG_READ_FUNCTION		0x078U
#define VF2_STG_READ_FUNCTION_MASK	0x07ffff00U
#define VF2_STG_READ_FUNCTION_SHIFT	8U
#define VF2_STG_WRITE_FUNCTION		0x07cU
#define VF2_STG_WRITE_FUNCTION_MASK	0x0007ffffU
#define VF2_STG_WRITE_FUNCTION_SHIFT	0U
#define VF2_STG_FUNCTION_SHIFT		9U
#define VF2_STG_REFERENCE_CLOCK_MASK	0x000c0000U
#define VF2_STG_REFERENCE_CLOCK		0x00080000U
#define VF2_STG_CLOCK_REQUEST		0x00400000U
#define VF2_STG_ROOT_PORT		0x0e8U
#define VF2_STG_ROOT_NO_ENDPOINT	0x00000100U
#define VF2_STG_LINK_STATUS		0x170U
#define VF2_STG_LINK_ACTIVE		0x00000020U

/*
 * Value(s) referenced: OpenBSD sys/dev/fdt/stfpciephy.c 1.4 (ISC): the
 * PHY's two tuning registers and the values written at attach.
 */
#define VF2_PHY_LEVEL			0x28U
#define VF2_PHY_LEVEL_TUNED		0x91U
#define VF2_PHY_SIGNAL			0x80U
#define VF2_PHY_SIGNAL_TUNED		0x0cU
#define VF2_PHY_SPAN			0x1000U

/* The board has two root complexes. */
#define VF2_PCIE_COUNT			2U

/*
 * The board's part of one root complex.
 *
 * group is the root complex's register group in the STG block; perst is
 * its reset line.  The record is filled before the PLDA driver starts and
 * read by the board operations for as long as the kernel runs.
 */
struct vf2_pcie {
	uint32_t group;
	struct drv_vf2_gpio perst;
	int perst_configured;
	struct drv_pci_plda *host;
};

/*
 * The board's root complexes, in the order the tree lists them.
 *
 * Filled once by drv_vf2_pcie_init() during boot, before any interrupt can
 * reach the root complexes, and never emptied.
 */
static struct vf2_pcie pcie_hosts[VF2_PCIE_COUNT];

static void register_drivers(void);
static int start_host(const struct drv_fdt *fdt, uint32_t node, int dma_default_coherent, struct vf2_pcie *board);
static int choose_group(uint64_t config_base, uint32_t *group);
static int set_bus(const struct drv_fdt *fdt, uint32_t node, uint32_t group);
static int enable_clocks(const struct drv_fdt *fdt, uint32_t node);
static int release_resets(const struct drv_fdt *fdt, uint32_t node);
static int tune_phy(const struct drv_fdt *fdt, uint32_t node);
static void board_perst(void *context, int asserted);
static int board_link_active(void *context);
static void board_select_function(void *context, unsigned function);

/*
 * Registers the drivers and starts every enabled root complex.
 */
int
drv_vf2_pcie_init(
	const struct drv_fdt *fdt,
	int dma_default_coherent)
{
	uint32_t node;
	unsigned started;
	unsigned found;
	int error;

	/* Brings the PCI core into service, even when no host follows. */
	error = drv_pci_init();
	if (error != 0 && error != EALREADY)
		return error;

	/* Registers the drivers, so enumeration binds the devices. */
	register_drivers();

	/* A board without a tree has no root complex to start. */
	if (fdt == NULL)
		return ENODEV;

	/* Starts each root complex the tree lists, up to the board's two. */
	node = DRV_FDT_NO_NODE;
	found = 0;
	started = 0;
	while (found < VF2_PCIE_COUNT) {
		/* Finds the next root complex. */
		error = drv_fdt_find_compatible(fdt, "starfive,jh7110-pcie", node, &node);
		if (error != 0)
			break;

		/* Starts it; a disabled or failing one is skipped. */
		error = start_host(fdt, node, dma_default_coherent, &pcie_hosts[found]);
		found++;
		if (error == ENODEV)
			continue;
		if (error != 0) {
			kern_logf("visionfive2: PCIe root complex unavailable (%d)\n", error);
			continue;
		}

		/* Counts the root complex that came up. */
		started++;
	}

	/* Reports a board whose tree has no usable root complex. */
	if (started == 0)
		return ENODEV;

	/* Succeeded: the PCI core owns the started trees. */
	return 0;
}

/*
 * Looks behind the USB root ports and publishes the NVMe namespaces.
 */
void
drv_vf2_pcie_refresh(
	void)
{
	/* Has each xHCI controller look at its root ports. */
#if CONFIG_DRIVER_PCI_XHCI
	drv_pci_xhci_probe_roots();
#endif

	/* Has each NVMe controller publish its namespace. */
#if CONFIG_DRIVER_PCI_NVME
	drv_pci_nvme_probe_namespaces();
#endif
}

/*
 * Lets the USB keyboards and mice type once the input core is ready.
 */
void
drv_vf2_pcie_input_ready(
	void)
{
	/* Starts the HID devices found so far and those found later. */
#if CONFIG_DRIVER_USB_HID
	drv_usb_hid_input_ready();
#endif
}

/*
 * Registers the drivers the configuration selects behind the root
 * complexes: xHCI with its USB class drivers, NVMe and GHCI.
 */
static void
register_drivers(
	void)
{
#if CONFIG_DRIVER_PCI_XHCI || CONFIG_DRIVER_PCI_NVME || CONFIG_DRIVER_PCI_GHCI
	int error;
#endif

#if CONFIG_DRIVER_PCI_XHCI
	/* Brings up the USB core. */
	error = drv_usb_init();
	if (error != 0) {
		kern_logf("usb: core initialization failed (%d)\n", error);
	} else {
		/* Registers bulk-only USB disks. */
#if CONFIG_DRIVER_USB_STORAGE
		error = drv_usb_storage_driver_register();
		if (error != 0)
			kern_logf("usb: storage registration failed (%d)\n", error);
#endif

		/* Registers the USB Video Class cameras. */
#if CONFIG_DRIVER_USB_UVC
		error = drv_usb_uvc_driver_register();
		if (error != 0)
			kern_logf("usb: video class registration failed (%d)\n", error);
#endif

		/* Registers the keyboard and mouse driver. */
#if CONFIG_DRIVER_USB_HID
		error = drv_usb_hid_driver_register();
		if (error != 0)
			kern_logf("usb: HID input driver registration failed (%d)\n", error);
#endif

		/* Registers the hub driver. */
#if CONFIG_DRIVER_USB_HUB
		error = drv_usb_hub_driver_register();
		if (error != 0)
			kern_logf("usb: hub driver registration failed (%d)\n", error);
#endif

		/* Registers the host controller driver. */
		error = drv_pci_xhci_driver_register();
		if (error != 0)
			kern_logf("usb: xHCI PCI driver registration failed (%d)\n", error);
	}
#endif

	/* Registers the NVMe controller driver. */
#if CONFIG_DRIVER_PCI_NVME
	error = drv_pci_nvme_driver_register();
	if (error != 0)
		kern_logf("nvme: PCI driver registration failed (%d)\n", error);
#endif

	/*
	 * Offers 3D and other display controllers to the GHCI driver, which
	 * keeps those whose BAR0 says GHCI.
	 */
#if CONFIG_DRIVER_PCI_GHCI
	error = drv_pci_ghci_driver_register();
	if (error != 0)
		kern_logf("ghci: PCI driver registration failed (%d)\n", error);
#endif
}

/*
 * Does the board's part of one root complex and starts it.
 *
 * Procedure referenced: OpenBSD sys/dev/fdt/stfpcie.c 1.4 (ISC), the
 * attach: the STG settings, the pins, the clocks and the resets before
 * the root complex is configured.
 */
static int
start_host(
	const struct drv_fdt *fdt,
	uint32_t node,
	int dma_default_coherent,
	struct vf2_pcie *board)
{
	struct drv_pci_plda_config *config;
	struct drv_pci_plda_board operations;
	unsigned changed;
	int coherent;
	int error;

	/* Allocates the root complex's description, which is too large for the stack. */
	config = kern_malloc(sizeof(*config));
	if (config == NULL)
		return ENOMEM;

	/* Reads the root complex's windows and pins. */
	error = drv_pci_plda_describe(fdt, node, config);
	if (error != 0) {
		kern_free(config);
		return error;
	}

	/* Takes the DMA coherency the tree states, or the board's. */
	coherent = drv_fdt_dma_coherent(fdt, node, dma_default_coherent);
	config->coherent = coherent != 0;

	/* Starts the board's record empty, PERST# not yet configured. */
	kern_memset(board, 0, sizeof(*board));

	/* Finds the root complex's register group in the STG block. */
	error = choose_group(config->config_base, &board->group);
	if (error != 0) {
		kern_free(config);
		return error;
	}

	/*
	 * Keeps the USB controller's root complex on its pins, as it was
	 * brought up: only the M.2 slot's offers MSI, which the device tree
	 * allows on both.
	 */
	if (board->group == VF2_STG_PCIE0_GROUP)
		config->msi = false;

	/* Applies the clock request and wake pins; ENOENT means there are none. */
	changed = 0;
	error = drv_vf2_pinctrl_apply_default(fdt, node, &changed);
	if (error != 0 && error != ENOENT)
		kern_logf("pcie%u: pins not applied (%d)\n", config->segment, error);

	/* Sets the root complex's bus settings. */
	error = set_bus(fdt, node, board->group);
	if (error != 0) {
		kern_free(config);
		return error;
	}

	/* Turns the clocks on. */
	error = enable_clocks(fdt, node);
	if (error != 0) {
		kern_free(config);
		return error;
	}

	/* Releases the resets. */
	error = release_resets(fdt, node);
	if (error != 0) {
		kern_free(config);
		return error;
	}

	/* Tunes the PHY. */
	error = tune_phy(fdt, node);
	if (error != 0) {
		kern_free(config);
		return error;
	}

	/* Finds PERST#. */
	error = drv_vf2_gpio_lookup(fdt, node, "perst-gpios", &board->perst);
	if (error != 0) {
		kern_free(config);
		return error;
	}

	/* Hands the PLDA driver the board's operations on PERST#, the link and the function selection. */
	operations.context = board;
	operations.perst = board_perst;
	operations.link_active = board_link_active;
	operations.select_function = board_select_function;

	/* Configures the root complex, resets the endpoint and trains the link. */
	error = drv_pci_plda_start(config, &operations, &board->host);
	kern_free(config);
	if (error != 0)
		return error;

	/* Enumerates the tree, which binds the registered drivers. */
	error = drv_pci_plda_publish(board->host);
	if (error != 0)
		return error;

	/* Succeeded: the root complex's tree is the PCI core's. */
	return 0;
}

/*
 * Chooses a root complex's register group in the STG block.
 *
 * The device tree does not say which group is whose; the configuration
 * window's address does.
 */
static int
choose_group(
	uint64_t config_base,
	uint32_t *group)
{
	/* Tells the board's two root complexes apart by their windows. */
	if (config_base == VF2_PCIE0_CONFIG) {
		/* The first root complex. */
		*group = VF2_STG_PCIE0_GROUP;
	} else if (config_base == VF2_PCIE1_CONFIG) {
		/* The second root complex. */
		*group = VF2_STG_PCIE1_GROUP;
	} else {
		/* Refuses a root complex the board does not have. */
		return ENODEV;
	}

	/* Succeeded: the root complex's group. */
	return 0;
}

/*
 * Sets the root complex's bus settings in the STG block.
 *
 * The root port is marked as having no endpoint of its own, the reference
 * clock comes from the source the board wires, and clock requests are on.
 */
static int
set_bus(
	const struct drv_fdt *fdt,
	uint32_t node,
	uint32_t group)
{
	enum drv_vf2_syscon syscon;
	uint32_t arguments[1];
	unsigned count;
	int error;

	/* Requires the property to name the STG block. */
	error = drv_vf2_syscon_lookup(fdt, node, "starfive,stg-syscon", &syscon, arguments, 1U, &count);
	if (error != 0)
		return error;

	/* Refuses a property that names another block. */
	if (syscon != DRV_VF2_SYSCON_STG)
		return ENODEV;

	/* Marks the root port. */
	error = drv_vf2_syscon_update(DRV_VF2_SYSCON_STG, group + VF2_STG_ROOT_PORT, VF2_STG_ROOT_NO_ENDPOINT, VF2_STG_ROOT_NO_ENDPOINT);
	if (error != 0)
		return error;

	/* Selects the reference clock source. */
	error = drv_vf2_syscon_update(DRV_VF2_SYSCON_STG, group + VF2_STG_WRITE_FUNCTION, VF2_STG_REFERENCE_CLOCK_MASK, VF2_STG_REFERENCE_CLOCK);
	if (error != 0)
		return error;

	/* Turns clock requests on. */
	error = drv_vf2_syscon_update(DRV_VF2_SYSCON_STG, group + VF2_STG_WRITE_FUNCTION, VF2_STG_CLOCK_REQUEST, VF2_STG_CLOCK_REQUEST);
	if (error != 0)
		return error;

	/* Succeeded: the bus settings are in place. */
	return 0;
}

/* Turns on the root complex's four clocks by their names. */
static int
enable_clocks(
	const struct drv_fdt *fdt,
	uint32_t node)
{
	static const char *const names[] = {"noc", "tl", "axi_mst0", "apb"};
	struct drv_vf2_clock clock;
	unsigned index;
	int error;

	/* Finds and turns on each clock. */
	for (index = 0; index < sizeof(names) / sizeof(names[0]); index++) {
		/* Finds the clock the name selects. */
		error = drv_vf2_clock_lookup(fdt, node, names[index], &clock);
		if (error != 0)
			return error;

		/* Opens its gate and those above it. */
		error = drv_vf2_clock_enable(&clock);
		if (error != 0)
			return error;
	}

	/* Succeeded: every clock runs. */
	return 0;
}

/* Releases the root complex's six resets by their names. */
static int
release_resets(
	const struct drv_fdt *fdt,
	uint32_t node)
{
	static const char *const names[] = {"mst0", "slv0", "slv", "brg", "core", "apb"};
	struct drv_vf2_reset reset;
	unsigned index;
	int error;

	/* Finds and releases each reset. */
	for (index = 0; index < sizeof(names) / sizeof(names[0]); index++) {
		/* Finds the reset the name selects. */
		error = drv_vf2_reset_lookup(fdt, node, names[index], &reset);
		if (error != 0)
			return error;

		/* Releases it and waits for the status to follow. */
		error = drv_vf2_reset_deassert(&reset);
		if (error != 0)
			return error;
	}

	/* Succeeded: the root complex is out of reset. */
	return 0;
}

/*
 * Writes the PHY's tuning values.
 *
 * Procedure referenced: OpenBSD sys/dev/fdt/stfpciephy.c 1.4 (ISC), the
 * attach: two register writes, nothing else.
 */
static int
tune_phy(
	const struct drv_fdt *fdt,
	uint32_t node)
{
	const uint8_t *value;
	uint32_t length;
	uint32_t phandle;
	uint32_t phy;
	uint64_t base;
	uint64_t size;
	volatile uint8_t *registers;
	void *mapping;
	int error;

	/* Reads the property that names the PHY, refusing one without a phandle. */
	error = drv_fdt_property(fdt, node, "phys", &value, &length);
	if (error != 0)
		return error;
	if (length < 4U)
		return EINVAL;

	/* Finds the PHY's node by its phandle. */
	phandle = (uint32_t)drv_fdt_cells_value(value, 0, 1U);
	error = drv_fdt_find_phandle(fdt, phandle, &phy);
	if (error != 0)
		return error;

	/* Reads where the PHY's registers are. */
	error = drv_fdt_reg(fdt, phy, 0, &base, &size);
	if (error != 0)
		return error;

	/* Maps the PHY's first page. */
	error = kern_device_map(base, VF2_PHY_SPAN, KERN_DEVICE_UNCACHED, &mapping);
	if (error != 0)
		return error;
	registers = mapping;

	/* Writes the two tuning values. */
	kern_mmio_write32(registers + VF2_PHY_LEVEL, VF2_PHY_LEVEL_TUNED);
	kern_mmio_write32(registers + VF2_PHY_SIGNAL, VF2_PHY_SIGNAL_TUNED);

	/* Releases the mapping; the PHY keeps the values. */
	(void)kern_device_unmap(mapping, VF2_PHY_SPAN);

	/* Succeeded: the PHY is tuned. */
	return 0;
}

/*
 * Drives PERST# for the PLDA driver.
 *
 * The first call makes the pin an output; later ones change its level.
 */
static void
board_perst(
	void *context,
	int asserted)
{
	struct vf2_pcie *board;
	int error;

	/* Makes the pin an output at the asserted level, or changes the level. */
	board = context;
	if (!board->perst_configured) {
		error = drv_vf2_gpio_configure_output(&board->perst, asserted);
		if (error == 0)
			board->perst_configured = 1;
	} else {
		error = drv_vf2_gpio_write(&board->perst, asserted);
	}

	/* A pin that cannot be driven leaves the endpoint as it was. */
	if (error != 0)
		kern_logf("visionfive2: PERST# not driven (%d)\n", error);
}

/* Reports whether the root complex's data link layer is up. */
static int
board_link_active(
	void *context)
{
	struct vf2_pcie *board;
	uint32_t status;
	int error;

	/* Reads the link status in the root complex's STG group. */
	board = context;
	status = 0;
	error = drv_vf2_syscon_read(DRV_VF2_SYSCON_STG, board->group + VF2_STG_LINK_STATUS, &status);
	if (error != 0)
		return 0;

	/* Reports the data link active bit. */
	if ((status & VF2_STG_LINK_ACTIVE) == 0)
		return 0;
	return 1;
}

/*
 * Points the root port's function registers at one of its functions.
 *
 * The same function number goes into the read and the write field.
 */
static void
board_select_function(
	void *context,
	unsigned function)
{
	struct vf2_pcie *board;
	uint32_t field;

	/* Selects the function for reads and for writes. */
	board = context;
	field = (uint32_t)function << VF2_STG_FUNCTION_SHIFT;
	(void)drv_vf2_syscon_update(DRV_VF2_SYSCON_STG, board->group + VF2_STG_READ_FUNCTION, VF2_STG_READ_FUNCTION_MASK, field << VF2_STG_READ_FUNCTION_SHIFT);
	(void)drv_vf2_syscon_update(DRV_VF2_SYSCON_STG, board->group + VF2_STG_WRITE_FUNCTION, VF2_STG_WRITE_FUNCTION_MASK, field << VF2_STG_WRITE_FUNCTION_SHIFT);
}
