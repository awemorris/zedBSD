/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The Raspberry Pi 5's root complexes other than RP1's.
 *
 * The board's part of each: the bridge's reset line in the SoC's reset
 * block, the PHYs' resistor calibration shared by every root complex, and
 * the MIP that takes the functions' message interrupts.  The Broadcom STB
 * driver does the rest.  A root complex that cannot start is logged and
 * skipped; the boot goes on from the SD card.
 */

#include <drivers/platform/rpi5/rpi5-pcie.h>
#include <drivers/platform/rpi5/rpi5-mip.h>
#include <drivers/platform/rpi5/rp1.h>
#include <drivers/generic/fdt.h>
#include <drivers/pci/pci.h>
#include <drivers/pci/pci-brcmstb.h>
#if CONFIG_DRIVER_PCI_NVME
#include <drivers/pci/pci-nvme.h>
#endif
#include <kern/clock.h>
#include <kern/device-io.h>
#include <kern/kcrt.h>
#include <kern/klog.h>
#include <kern/pmem.h>
#include <uapi/errno.h>

/* The most bytes of the firmware's device tree the module reads. */
#define RPI5_PCIE_FDT_READ_LIMIT	(2U * 1024U * 1024U)

/* The root complexes the module starts besides RP1's: the BCM2712 has three in all. */
#define RPI5_PCIE_HOSTS			2U

/*
 * Where the root complex of the external connector (the M.2 HAT's FFC)
 * has its registers.  The firmware enables it for dtparam=pciex1; the
 * others are not the connector's, so their being disabled needs no hint.
 *
 * Value referenced: zedBSD-firmware rpi5-boot bcm2712-rpi-5-b.dtb and
 * bcm2712d0-rpi-5-b.dtb (the alias pciex1, /axi/pcie@1000110000).
 */
#define RPI5_PCIE_EXTERNAL_BASE		0x1000110000ULL

/*
 * The SoC's reset block: one bank of 32 lines per 0x18 bytes, each bank a
 * register that asserts the lines whose bits are written and one that
 * releases them, and the wait after a release before the block behind the
 * line can be reached.
 *
 * Values referenced: Raspberry Pi Linux rpi-6.12.y
 * drivers/reset/reset-brcmstb.c (SW_INIT_SET, SW_INIT_CLEAR,
 * SW_INIT_BANK_SIZE, SW_INIT_BIT(), SW_INIT_BANK(), and the 100 to 200 us
 * of brcmstb_reset_deassert()).
 */
#define RESET_BANK_BYTES		0x18U
#define RESET_SET			0x00U
#define RESET_CLEAR			0x04U
#define RESET_LINES_PER_BANK		32U
#define RESET_RELEASE_US		100U
#define RESET_RELEASE_MAX_US		200U

/*
 * The PHYs' resistor calibration: the start bit and the done bit, and how
 * long the calibration is waited for.
 *
 * Values referenced: Raspberry Pi Linux rpi-6.12.y
 * drivers/reset/reset-brcmstb-rescal.c (BRCM_RESCAL_START,
 * BRCM_RESCAL_START_BIT, BRCM_RESCAL_STATUS, BRCM_RESCAL_STATUS_BIT, and
 * the 100 us steps up to 1 ms of brcm_rescal_reset_set()).
 */
#define RESCAL_START			0x00U
#define RESCAL_START_BIT		0x00000001U
#define RESCAL_STATUS			0x08U
#define RESCAL_STATUS_DONE		0x00000001U
#define RESCAL_BYTES			0x10U
#define RESCAL_POLLS			10U
#define RESCAL_POLL_US			100U
#define RESCAL_POLL_MAX_US		200U

/*
 * The board's part of one root complex.
 *
 * Filled by drv_rpi5_pcie_init() before the root complex starts and read
 * by the board operations for as long as the kernel runs.  reset holds the
 * mapped reset bank the bridge's line is in, and reset_bit that line.
 */
struct rpi5_pcie {
	volatile uint8_t *reset;
	uint32_t reset_bit;
	struct drv_rpi5_mip *mip;
	struct drv_pci_brcmstb_board board;
	struct drv_pci_brcmstb *host;
};

/*
 * The device tree the root complexes were found in.
 *
 * Opened once by drv_rpi5_pcie_init(), during the platform's start-up,
 * and only read afterwards.
 */
static struct drv_fdt rpi5_pcie_fdt;

/*
 * The root complexes the module started, in the order the tree lists them.
 *
 * Filled once by drv_rpi5_pcie_init(); count says how many, and nothing
 * is removed afterwards.
 */
static struct rpi5_pcie rpi5_pcie_hosts[RPI5_PCIE_HOSTS];
static unsigned rpi5_pcie_count;

static void register_drivers(void);
static int is_rp1_root_complex(const struct drv_fdt *fdt, uint32_t node);
static void report_disabled(uint64_t base);
static int start_host(const struct drv_fdt *fdt, uint32_t node, struct rpi5_pcie *board);
static int find_reset(const struct drv_fdt *fdt, uint32_t node, const char *name, uint32_t *controller, uint32_t *line);
static int map_bridge_reset(const struct drv_fdt *fdt, uint32_t node, struct rpi5_pcie *board);
static int calibrate(const struct drv_fdt *fdt, uint32_t node);
static void start_mip(const struct drv_fdt *fdt, uint32_t node, struct rpi5_pcie *board);
static int bridge_reset(void *context, int asserted);

/*
 * Starts every enabled root complex but RP1's.
 */
int
drv_rpi5_pcie_init(
	uint64_t fdt_phys)
{
	const void *blob;
	uint64_t base;
	uint64_t size;
	uint32_t node;
	bool enabled;
	int rp1;
	int error;

	/* Brings the PCI core into service, even when no root complex follows. */
	error = drv_pci_init();
	if (error != 0 && error != EALREADY)
		return error;

	/* Registers the drivers, so enumeration binds the devices. */
	register_drivers();

	/* Opens the device tree through the direct map. */
	blob = kern_pmem_to_kernel((kern_physaddr_t)fdt_phys);
	if (blob == NULL)
		return ENODEV;
	error = drv_fdt_open(&rpi5_pcie_fdt, blob, RPI5_PCIE_FDT_READ_LIMIT);
	if (error != 0)
		return error;

	/* Looks at each BCM2712 root complex in turn. */
	node = DRV_FDT_NO_NODE;
	while (rpi5_pcie_count < RPI5_PCIE_HOSTS) {
		error = drv_fdt_find_compatible(&rpi5_pcie_fdt, "brcm,bcm2712-pcie", node, &node);
		if (error != 0)
			break;

		/* Leaves RP1's root complex as the firmware set it up. */
		rp1 = is_rp1_root_complex(&rpi5_pcie_fdt, node);
		if (rp1)
			continue;

		/* Names a root complex the tree leaves disabled, and skips it. */
		enabled = drv_fdt_node_enabled(&rpi5_pcie_fdt, node);
		if (!enabled) {
			base = 0;
			(void)drv_fdt_reg(&rpi5_pcie_fdt, node, 0, &base, &size);
			report_disabled(base);
			continue;
		}

		/* Starts it; a failure is logged and the next one is tried. */
		error = start_host(&rpi5_pcie_fdt, node, &rpi5_pcie_hosts[rpi5_pcie_count]);
		if (error != 0) {
			kern_logf("rpi5-pcie: root complex unavailable (%d)\n", error);
			continue;
		}

		/* Counts the root complex that came up. */
		rpi5_pcie_count++;
	}

	/* Reports a board where no root complex came up. */
	if (rpi5_pcie_count == 0)
		return ENODEV;

	/* Succeeded: the PCI core owns the started trees. */
	return 0;
}

/*
 * Publishes the NVMe namespaces once interrupts are on.
 */
void
drv_rpi5_pcie_refresh(
	void)
{
	/* Leaves a board without a started root complex alone. */
	if (rpi5_pcie_count == 0)
		return;

	/* Has each NVMe controller publish its namespace. */
#if CONFIG_DRIVER_PCI_NVME
	drv_pci_nvme_probe_namespaces();
#endif
}

/* Registers the drivers the configuration selects behind the root complexes. */
static void
register_drivers(
	void)
{
#if CONFIG_DRIVER_PCI_NVME
	int error;

	/* Registers the NVMe controller driver. */
	error = drv_pci_nvme_driver_register();
	if (error != 0)
		kern_logf("nvme: PCI driver registration failed (%d)\n", error);
#endif
}

/* Logs a root complex the device tree leaves disabled, with the fix for the connector's. */
static void
report_disabled(
	uint64_t base)
{
	/* The external connector's is enabled by a line in config.txt. */
	if (base == RPI5_PCIE_EXTERNAL_BASE) {
		kern_logf("rpi5-pcie: %llx disabled in the device tree (config.txt: dtparam=pciex1)\n", (unsigned long long)base);
		return;
	}

	/* Any other is simply not used on this board. */
	kern_logf("rpi5-pcie: %llx disabled in the device tree\n", (unsigned long long)base);
}

/*
 * Tells whether a root complex is RP1's: one of its children is RP1's
 * bus, a simple bus that is also an interrupt controller.
 */
static int
is_rp1_root_complex(
	const struct drv_fdt *fdt,
	uint32_t node)
{
	const uint8_t *value;
	uint32_t length;
	uint32_t child;
	unsigned index;
	int error;

	/* Looks at each child in turn. */
	child = DRV_FDT_NO_NODE;
	for (;;) {
		error = drv_fdt_child(fdt, node, child, &child);
		if (error != 0)
			break;

		/* Skips a child that is not a simple bus. */
		error = drv_fdt_string_index(fdt, child, "compatible", "simple-bus", &index);
		if (error != 0)
			continue;

		/* A simple bus that is an interrupt controller is RP1's. */
		error = drv_fdt_property(fdt, child, "interrupt-controller", &value, &length);
		if (error == 0)
			return 1;
	}

	/* Succeeded: no child is RP1's bus. */
	return 0;
}

/*
 * Does the board's part of one root complex and starts it.
 *
 * Procedure referenced: Raspberry Pi Linux rpi-6.12.y
 * drivers/pci/controller/pcie-brcmstb.c (brcm_pcie_probe(): the rescal
 * reset before the setup, the bridge reset through the reset controller
 * named "bridge").
 */
static int
start_host(
	const struct drv_fdt *fdt,
	uint32_t node,
	struct rpi5_pcie *board)
{
	struct drv_pci_brcmstb_config config;
	uint32_t domain;
	int error;

	/* Reads the root complex's windows, pins and BCM2712 settings. */
	kern_memset(board, 0, sizeof(*board));
	error = drv_pci_brcmstb_describe(fdt, node, &config);
	if (error != 0)
		return error;

	/* Takes the tree's PCI domain as the segment; RP1's USB is segment 0 already. */
	domain = drv_fdt_node_cells(fdt, node, "linux,pci-domain", 0);
	if (domain == 0 || domain > 0xffffU) {
		kern_logf("rpi5-pcie: PCI domain %u cannot be a segment of its own\n", domain);
		return EBUSY;
	}

	/* Finds and maps the bridge's reset line. */
	error = map_bridge_reset(fdt, node, board);
	if (error != 0)
		return error;

	/* Makes sure the PHYs are calibrated. */
	error = calibrate(fdt, node);
	if (error != 0)
		return error;

	/* Starts the MIP the message interrupts go to; without it the pins serve. */
	start_mip(fdt, node, board);

	/* Hands the driver the board's operations. */
	board->board.context = board;
	board->board.bridge_reset = bridge_reset;
	board->board.msi_establish = NULL;
	board->board.msi_disestablish = NULL;
	if (board->mip != NULL) {
		board->board.msi_establish = drv_rpi5_mip_establish;
		board->board.msi_disestablish = drv_rpi5_mip_disestablish;
	}

	/* Completes the description with what the board knows. */
	config.segment = (uint16_t)domain;
	config.coherent = drv_fdt_dma_coherent(fdt, node, 0);
	config.board = &board->board;

	/* Resets the root complex, trains its link and assigns its bus. */
	error = drv_pci_brcmstb_start(&config, &board->host);
	if (error != 0)
		return error;

	/* Enumerates the bus, which binds the registered drivers. */
	error = drv_pci_brcmstb_publish(board->host);
	if (error != 0)
		return error;

	/* Records which interrupts the root complex's functions get. */
	if (board->mip != NULL)
		kern_logf("rpi5-pcie: segment %u started, message interrupts through the MIP\n", domain);
	else
		kern_logf("rpi5-pcie: segment %u started, legacy interrupts only\n", domain);

	/* Succeeded: the PCI core owns the root complex's tree. */
	return 0;
}

/*
 * Finds the reset controller and its line that the node's resets and
 * reset-names give the name.
 */
static int
find_reset(
	const struct drv_fdt *fdt,
	uint32_t node,
	const char *name,
	uint32_t *controller,
	uint32_t *line)
{
	const uint8_t *value;
	uint32_t length;
	uint32_t position;
	uint32_t cells;
	uint32_t phandle;
	unsigned wanted;
	unsigned entry;
	int error;

	/* Finds which entry carries the name. */
	error = drv_fdt_string_index(fdt, node, "reset-names", name, &wanted);
	if (error != 0)
		return error;

	/* Reads the resets. */
	error = drv_fdt_property(fdt, node, "resets", &value, &length);
	if (error != 0)
		return error;

	/* Walks the entries; each is a controller's phandle and its own count of cells. */
	position = 0;
	for (entry = 0; (position + 1U) * 4U <= length; entry++) {
		/* Finds the entry's controller, which says how many cells follow. */
		phandle = (uint32_t)drv_fdt_cells_value(value, position, 1U);
		error = drv_fdt_find_phandle(fdt, phandle, controller);
		if (error != 0)
			return error;
		cells = drv_fdt_node_cells(fdt, *controller, "#reset-cells", 0);
		if ((position + 1U + cells) * 4U > length)
			return ERANGE;

		/* Stops at the named entry; a controller without cells has line 0. */
		if (entry == wanted) {
			*line = 0;
			if (cells != 0)
				*line = (uint32_t)drv_fdt_cells_value(value, position + 1U, 1U);

			/* Succeeded: the controller and its line. */
			return 0;
		}

		/* Moves to the next entry. */
		position += 1U + cells;
	}

	/* Reports a name the resets do not reach. */
	return ENOENT;
}

/*
 * Finds the bridge's reset line in the SoC's reset block and maps the
 * line's bank.
 */
static int
map_bridge_reset(
	const struct drv_fdt *fdt,
	uint32_t node,
	struct rpi5_pcie *board)
{
	uint32_t controller;
	uint32_t line;
	uint64_t base;
	uint64_t size;
	uint64_t bank;
	unsigned index;
	void *mapped;
	int error;

	/* Finds the controller and the line named "bridge". */
	error = find_reset(fdt, node, "bridge", &controller, &line);
	if (error != 0)
		return error;

	/* Refuses a controller that is not the reset block the module drives. */
	error = drv_fdt_string_index(fdt, controller, "compatible", "brcm,brcmstb-reset", &index);
	if (error != 0)
		return ENOTSUP;

	/* Finds the block and refuses a line beyond it. */
	error = drv_fdt_reg(fdt, controller, 0, &base, &size);
	if (error != 0)
		return error;
	bank = (uint64_t)(line / RESET_LINES_PER_BANK) * RESET_BANK_BYTES;
	if (bank + RESET_CLEAR + 4U > size)
		return ERANGE;

	/* Maps the line's bank. */
	error = kern_device_map(base + bank, RESET_BANK_BYTES, KERN_DEVICE_UNCACHED, &mapped);
	if (error != 0)
		return error;

	/* Succeeded: the bridge's line can be driven. */
	board->reset = mapped;
	board->reset_bit = 1U << (line % RESET_LINES_PER_BANK);
	return 0;
}

/*
 * Makes sure the PHYs' resistor calibration, shared by every root
 * complex, has been done.
 *
 * The firmware calibrates when it brings up RP1's link, and calibrating
 * again under a live link might upset it, so the calibration runs only
 * when the done bit is clear and RP1's link is not up.
 *
 * Procedure referenced: Raspberry Pi Linux rpi-6.12.y
 * drivers/reset/reset-brcmstb-rescal.c (brcm_rescal_reset_set(): set the
 * start bit, check it reads back, wait for the done bit, clear the start
 * bit).
 */
static int
calibrate(
	const struct drv_fdt *fdt,
	uint32_t node)
{
	volatile uint8_t *registers;
	uint32_t controller;
	uint32_t line;
	uint32_t value;
	uint64_t base;
	uint64_t size;
	unsigned index;
	unsigned poll;
	void *mapped;
	bool done;
	int error;

	/* Leaves a root complex the tree gives no calibration alone. */
	error = find_reset(fdt, node, "rescal", &controller, &line);
	if (error != 0)
		return 0;

	/* Refuses a calibration block the module does not know. */
	error = drv_fdt_string_index(fdt, controller, "compatible", "brcm,bcm7216-pcie-sata-rescal", &index);
	if (error != 0)
		return ENOTSUP;

	/* Maps the calibration's registers. */
	error = drv_fdt_reg(fdt, controller, 0, &base, &size);
	if (error != 0)
		return error;
	error = kern_device_map(base, RESCAL_BYTES, KERN_DEVICE_UNCACHED, &mapped);
	if (error != 0)
		return error;
	registers = mapped;

	/* Keeps a calibration that is done already. */
	value = kern_mmio_read32(registers + RESCAL_STATUS);
	if ((value & RESCAL_STATUS_DONE) != 0) {
		kern_logf("rpi5-pcie: PHY calibration done already\n");
		(void)kern_device_unmap(mapped, RESCAL_BYTES);
		return 0;
	}

	/* Keeps the calibration under RP1's live link. */
	if (drv_rp1_fdt() != NULL) {
		kern_logf("rpi5-pcie: PHY calibration left as it is: RP1's link is up\n");
		(void)kern_device_unmap(mapped, RESCAL_BYTES);
		return 0;
	}

	/* Starts the calibration and refuses a block that does not take the start. */
	value = kern_mmio_read32(registers + RESCAL_START);
	kern_mmio_write32(registers + RESCAL_START, value | RESCAL_START_BIT);
	value = kern_mmio_read32(registers + RESCAL_START);
	if ((value & RESCAL_START_BIT) == 0) {
		kern_logf("rpi5-pcie: PHY calibration did not start\n");
		(void)kern_device_unmap(mapped, RESCAL_BYTES);
		return EIO;
	}

	/* Waits for the done bit. */
	done = false;
	for (poll = 0; poll < RESCAL_POLLS; poll++) {
		/* Waits one polling interval. */
		kern_usleep_range(RESCAL_POLL_US, RESCAL_POLL_MAX_US);

		/* Stops once the calibration is done. */
		value = kern_mmio_read32(registers + RESCAL_STATUS);
		if ((value & RESCAL_STATUS_DONE) != 0) {
			done = true;
			break;
		}
	}

	/* Ends the start request whatever happened. */
	value = kern_mmio_read32(registers + RESCAL_START);
	kern_mmio_write32(registers + RESCAL_START, value & ~RESCAL_START_BIT);
	(void)kern_device_unmap(mapped, RESCAL_BYTES);

	/* Refuses to go on without a calibration. */
	if (!done) {
		kern_logf("rpi5-pcie: PHY calibration timed out\n");
		return ETIMEDOUT;
	}

	/* Succeeded: the PHYs are calibrated. */
	kern_logf("rpi5-pcie: PHY calibration done\n");
	return 0;
}

/*
 * Starts the MIP the root complex's msi-parent names, when it is one.
 *
 * A root complex whose messages would go to its own receiver, or whose
 * MIP does not start, serves its functions through the legacy pins.
 */
static void
start_mip(
	const struct drv_fdt *fdt,
	uint32_t node,
	struct rpi5_pcie *board)
{
	const uint8_t *value;
	uint32_t length;
	uint32_t phandle;
	uint32_t parent;
	int error;

	/* Follows the msi-parent; none leaves the legacy pins. */
	board->mip = NULL;
	error = drv_fdt_property(fdt, node, "msi-parent", &value, &length);
	if (error != 0 || length < 4U)
		return;
	phandle = (uint32_t)drv_fdt_cells_value(value, 0, 1U);
	error = drv_fdt_find_phandle(fdt, phandle, &parent);
	if (error != 0)
		return;

	/* Leaves a root complex that is its own receiver on the legacy pins. */
	if (parent == node)
		return;

	/* Starts the MIP; a failure leaves the legacy pins. */
	error = drv_rpi5_mip_start(fdt, parent, &board->mip);
	if (error != 0) {
		kern_logf("rpi5-pcie: no MSI receiver (%d); legacy interrupts only\n", error);
		board->mip = NULL;
	}
}

/*
 * Asserts or releases the bridge's reset line, for the Broadcom STB
 * driver; a release waits until the bridge can be reached.
 */
static int
bridge_reset(
	void *context,
	int asserted)
{
	struct rpi5_pcie *board;

	/* Refuses a board whose line was never found. */
	board = context;
	if (board == NULL || board->reset == NULL)
		return ENODEV;

	/* Asserts the line by writing its bit to the bank's set register. */
	if (asserted) {
		kern_mmio_write32(board->reset + RESET_SET, board->reset_bit);
		return 0;
	}

	/* Releases it by writing its bit to the clear register, and waits. */
	kern_mmio_write32(board->reset + RESET_CLEAR, board->reset_bit);
	kern_usleep_range(RESET_RELEASE_US, RESET_RELEASE_MAX_US);

	/* Succeeded: the bridge is out of reset. */
	return 0;
}
