/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The S905X5M's USB host: the CRG controller with its USB 2 PHY.
 *
 * The CRG is an xHCI whose capability registers start at its base.  The
 * module wakes the USB 2 PHY the controller is wired to (its reset, its
 * host mode, its calibration and its PLL), puts the controller in host
 * mode, and then shows the controller to the PCI core as the one function
 * of a root bus that exists only here: the function's configuration space
 * is kept in memory, and its one BAR is the controller's registers.  The
 * xHCI driver binds to the function by its class, as to any xHCI.  The
 * controller's DMA reaches 32 bits only, so its DMA device keeps every
 * buffer below 4 GiB.
 *
 * The function's interrupt is a pin of this host.  The host serves it by
 * calling the PCI core's line from the controller's GIC interrupt, and
 * from a polling thread: often when the interrupt could not be routed,
 * rarely otherwise, so an edge that came while the pin was masked is not
 * lost.
 *
 * The controller sits in a power domain the secure firmware switches.
 * When the bootloader did not start USB that domain may be off, and a
 * read of the controller may then never return; the log names the read
 * before it is made, so a boot that stops there says where.
 */

#include "drivers/platform/s905x5m/s905x5m-usb.h"
#include "drivers/platform/s905x5m/s905x5m-soc.h"

#include <drivers/generic/dma.h>
#include <drivers/pci/pci.h>
#include <drivers/pci/pci-xhci.h>
#include <drivers/usb/usb.h>
#include <drivers/usb/usb-hid.h>
#include <drivers/usb/usb-hub.h>
#include <drivers/usb/usb-storage.h>
#include <drivers/usb/usb-uas.h>
#include <drivers/usb/usb-cdc-ncm.h>
#include <drivers/usb/usb-cdc-ecm.h>
#include <kern/clock.h>
#include <kern/device-io.h>
#include <kern/irq.h>
#include <kern/kcrt.h>
#include <kern/klog.h>
#include <kern/lock.h>
#include <kern/pmem.h>
#include <kern/sched.h>
#include <kern/thread.h>
#include <uapi/errno.h>

/*
 * The controller, its register block, and the USB 2 PHY's registers when
 * the device tree cannot be read: the PHY's control words, its efuse trim
 * word and its configuration block.
 *
 * Values referenced: Hardkernel common_drivers odroids7d-6.12.y @34daba9
 * arch/arm64/boot/dts/amlogic/meson-s7d.dtsi (crg@fe340000 reg
 * 0xfe340000/0x8000; usbphy1@fe348000 u2p@0 reg 0xfe348000/0x10,
 * 0xfe002000/0x100, 0xfe010330/4, 0xfe34c000/0x60).
 */
#define S905X5M_USB_REGISTERS		0xfe340000ULL
#define S905X5M_USB_REGISTER_BYTES	0x8000U
#define S905X5M_PHY_CONTROL		0xfe348000ULL
#define S905X5M_PHY_TRIM		0xfe010330ULL
#define S905X5M_PHY_CONFIG		0xfe34c000ULL
#define S905X5M_PAGE_MASK		0xfffULL
#define S905X5M_PAGE_BYTES		0x1000U

/*
 * The controller's peripheral gate.
 *
 * Value referenced: Hardkernel common_drivers odroids7d-6.12.y @34daba9
 * drivers/clk/meson/s7d.c (sys_usb: CLKCTRL_SYS_CLK_EN0_REG1 bit 26) and
 * meson-s7d.dtsi (the PHY's crg_general clock is CLKID_SYS_USB).
 */
#define S905X5M_USB_GATE_BIT		26U

/*
 * The PHY's resets when the device tree names none: the pulse that resets
 * the USB block once, and the level bit that holds the PHY in reset.
 *
 * Values referenced: Hardkernel common_drivers odroids7d-6.12.y @34daba9
 * meson-s7d.dtsi (usbphy1 u2p@0: usb-reset-bit 4, phy-reset-level-bits 9,
 * reset-level 0x40).
 */
#define S905X5M_USB_RESET_BIT		4U
#define S905X5M_PHY_RESET_LEVEL_BIT	9U

/*
 * The PHY's control words: r0 with the host mode and power-on reset bits,
 * r1 with the PHY's ready bit.
 *
 * Values referenced: Hardkernel common_drivers odroids7d-6.12.y @34daba9
 * include/linux/amlogic/usb-v2.h (union u2p_r0_v2: host_device bit 0, POR
 * bit 3; union u2p_r1_v2: phy_rdy bit 0).
 */
#define S905X5M_PHY_R0			0x0U
#define S905X5M_PHY_R1			0x4U
#define S905X5M_PHY_R0_HOST		0x00000001U
#define S905X5M_PHY_R0_POWER_ON_RESET	0x00000008U
#define S905X5M_PHY_R1_READY		0x00000001U

/*
 * The PHY's configuration block: the calibration trim, the disconnect and
 * squelch thresholds, the unused configuration disable bit, the edge
 * driver setting, and the PLL control with its enable, bias, reset,
 * lock enable and lock bits and its default value.
 *
 * Values referenced: Hardkernel common_drivers odroids7d-6.12.y @34daba9
 * drivers/usb/phy_meson_usb/socs/phy-meson-s7d-usb.c
 * (meson_usb2phy_s7d_set_calibration_trim: cfg + 0x10, the low 12 bits,
 * efuse bit 12 valid and bits 11:8 plus 2 capped at 12;
 * meson_usb2phy_s7d_cali_disc_squelch: cfg + 0xc = 0x39, cfg + 0x38 bit 27
 * cleared; meson_usb2phy_s7d_set_misc: cfg + 0x50; meson_usb2phy_s7d_set_pll:
 * cfg + 0x40, 0x549540, bits 27, 26, 25, 24 and the done bit 31) and
 * meson-s7d.dtsi (pll-settings: 0x7e18 and 9).
 */
#define S905X5M_PHY_TRIM_REGISTER	0x10U
#define S905X5M_PHY_TRIM_MASK		0x00000fffU
#define S905X5M_PHY_TRIM_VALID		0x00001000U
#define S905X5M_PHY_TRIM_SHIFT		8U
#define S905X5M_PHY_TRIM_FIELD		0xfU
#define S905X5M_PHY_TRIM_OFFSET		2U
#define S905X5M_PHY_TRIM_MAX		12U
#define S905X5M_PHY_TRIM_DEFAULT	9U
#define S905X5M_PHY_THRESHOLDS		0x0cU
#define S905X5M_PHY_THRESHOLDS_VALUE	0x39U
#define S905X5M_PHY_CONFIG_DISABLE	0x38U
#define S905X5M_PHY_CONFIG_DISABLE_BIT	0x08000000U
#define S905X5M_PHY_EDGE_DRIVER		0x50U
#define S905X5M_PHY_EDGE_DEFAULT	0x7e18U
#define S905X5M_PHY_PLL			0x40U
#define S905X5M_PHY_PLL_DEFAULT		0x00549540U
#define S905X5M_PHY_PLL_ENABLE		0x08000000U
#define S905X5M_PHY_PLL_BIAS		0x04000000U
#define S905X5M_PHY_PLL_RESET		0x02000000U
#define S905X5M_PHY_PLL_LOCK_ENABLE	0x01000000U
#define S905X5M_PHY_PLL_LOCKED		0x80000000U
#define S905X5M_PHY_PLL_TRIES		5U
#define S905X5M_PHY_PLL_POLLS		5U

/* The waits of the PHY's bring-up, in microseconds. */
#define S905X5M_PHY_RESET_US		50U
#define S905X5M_PHY_SETTLE_US		100U
#define S905X5M_PHY_PLL_STEP_US		20U
#define S905X5M_PHY_READY_POLL_US	5U
#define S905X5M_PHY_READY_POLLS		200U

/*
 * The controller's role register, whose bit 0 clear makes it a host.
 *
 * Value referenced: Hardkernel U-Boot odroidc5-v2023.01 @3c17bc80
 * drivers/amlogic/usb/host/xhci-crg.c (crg_set_mode: bit 0 of base +
 * 0x20fc cleared for the host role).
 */
#define S905X5M_USB_ROLE		0x20fcU
#define S905X5M_USB_ROLE_DEVICE		0x00000001U

/*
 * The xHCI capability registers the log reads: the capability length and
 * interface version, and the structural parameters with the port count;
 * and where a port's status sits in the operational registers.
 *
 * Values referenced: the xHCI specification 1.2, section 5.3 (CAPLENGTH,
 * HCIVERSION, HCSPARAMS1 MaxPorts bits 31:24) and 5.4.8 (PORTSC at
 * operational base + 0x400 + 0x10 per port).
 */
#define XHCI_CAPABILITY			0x00U
#define XHCI_STRUCTURE1			0x04U
#define XHCI_PORTS_SHIFT		24U
#define XHCI_PORT_STATUS		0x400U
#define XHCI_PORT_STRIDE		0x10U

/*
 * The configuration space the function shows: Amlogic's vendor number, a
 * product number of this host's own, the xHCI class, a 32-bit memory BAR,
 * and interrupt pin A.
 */
#define S905X5M_USB_IDENTITY		0x00011b8eU
#define S905X5M_USB_CLASS_XHCI		0x0c0330U
#define S905X5M_USB_INTERRUPT_PIN_A	1U

/* The configuration registers the host keeps for the function. */
#define PCI_ID				0x00U
#define PCI_COMMAND			0x04U
#define PCI_CLASS_REVISION		0x08U
#define PCI_BAR0			0x10U
#define PCI_SUBSYSTEM			0x2cU
#define PCI_INTERRUPT			0x3cU
#define PCI_INTERRUPT_PIN_SHIFT		8U
#define PCI_CLASS_SHIFT			8U
#define PCI_CONFIG_BYTES		256U

/*
 * The command bits the function keeps: memory decode, bus mastering and
 * the legacy interrupt's disable.  The rest read as zero.
 */
#define PCI_COMMAND_MEMORY		0x0002U
#define PCI_COMMAND_MASTER		0x0004U
#define PCI_COMMAND_INTX_DISABLE	0x0400U
#define PCI_COMMAND_KEPT		(PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER | PCI_COMMAND_INTX_DISABLE)

/* The host's root bus, and the pin's vector. */
#define S905X5M_USB_SEGMENT		0U
#define S905X5M_USB_BUS			0U
#define S905X5M_USB_VECTOR		1U

/* The DMA constraints of the controller: 32-bit addresses, no snooping unless the tree says so. */
#define S905X5M_USB_DMA_MAX_SEGMENT	(16U * 1024U * 1024U)
#define S905X5M_USB_DMA_ADDRESS_BITS	32U

/*
 * How often the polling thread serves the controller, in milliseconds:
 * when it has no interrupt, and when it has one.
 */
#define S905X5M_USB_POLL_MS		2U
#define S905X5M_USB_SAFETY_POLL_MS	50U

/* How long a removal waits for a running service, in steps of a millisecond. */
#define S905X5M_USB_DRAIN_POLLS		1000U
#define S905X5M_USB_DRAIN_POLL_US	1000U

/*
 * The USB 2 PHY's registers and settings, as the device tree describes
 * them.
 *
 * Filled by describe_phy() before the PHY is touched and read by the
 * bring-up only.
 */
struct s905x5m_usb_phy {
	uint64_t control;
	uint64_t trim;
	uint64_t config;
	unsigned usb_reset_bit;
	unsigned phy_reset_bit;
	uint32_t edge_driver;
	uint32_t trim_default;
};

/*
 * The host of the controller's root bus.
 *
 * It lives for as long as the kernel runs: the PCI core keeps it as the
 * host of the root bus.  The fields describing the controller are filled
 * by drv_s905x5m_usb_init() before the root bus exists and are constant
 * afterwards, except for the configuration registers (command, BAR) the
 * PCI core and the xHCI driver write and the pin's entry, which the lock
 * guards.  started is set once the root bus is published, polling once
 * the polling thread exists, and poll_ms, the thread's interval, before
 * it starts.
 */
struct s905x5m_usb_host {
	struct spinlock lock;
	int present;
	uint32_t node;
	uint64_t base;
	uint64_t size;
	volatile uint8_t *registers;
	struct drv_s905x5m_interrupt interrupt;
	int interrupt_routed;
	uint16_t command;
	uint32_t bar;
	drv_pci_intx_run_t run;
	void *line;
	int pin_enabled;
	unsigned in_flight;
	struct drv_dma_device *dma;
	struct drv_pci_bus *root;
	struct thread *poll_thread;
	int started;
	int polling;
	unsigned poll_ms;
};

static int host_config_read(void *context, const struct drv_pci_address *address, unsigned offset, unsigned width, uint32_t *value);
static int host_config_write(void *context, const struct drv_pci_address *address, unsigned offset, unsigned width, uint32_t value);
static int host_map_bar(void *context, struct drv_pci_device *device, const struct drv_pci_bar *bar, unsigned flags, struct drv_pci_mapping *mapping);
static void host_unmap_bar(void *context, struct drv_pci_mapping *mapping);
static int host_allocate_irqs(void *context, struct drv_pci_device *device, enum drv_pci_irq_type type, unsigned minimum, unsigned maximum, struct drv_pci_irq *irqs, unsigned *count);
static void host_free_irqs(void *context, struct drv_pci_device *device, struct drv_pci_irq *irqs, unsigned count);
static int host_intx_establish(void *context, unsigned vector, drv_pci_intx_run_t run, void *line);
static int host_intx_disestablish(void *context, unsigned vector, drv_pci_intx_run_t run, void *line);
static void host_intx_mask(void *context, unsigned vector);
static void host_intx_unmask(void *context, unsigned vector);

/*
 * The operations the PCI core calls on the host's root bus.
 *
 * The table is constant; the host arrives as the context of each call.
 */
static const struct drv_pci_bus_ops s905x5m_usb_bus_ops = {
	.config_space_size = PCI_CONFIG_BYTES,
	.config_read = host_config_read,
	.config_write = host_config_write,
	.map_bar = host_map_bar,
	.unmap_bar = host_unmap_bar,
	.allocate_irqs = host_allocate_irqs,
	.free_irqs = host_free_irqs,
	.intx_establish = host_intx_establish,
	.intx_disestablish = host_intx_disestablish,
	.intx_mask = host_intx_mask,
	.intx_unmask = host_intx_unmask
};

/* The one host of the controller. */
static struct s905x5m_usb_host s905x5m_usb;

static int find_controller(void);
static void describe_phy(struct s905x5m_usb_phy *phy);
static int map_page(uint64_t address, volatile uint8_t **mapped);
static int start_phy(const struct s905x5m_usb_phy *phy);
static void calibrate_phy(volatile uint8_t *config, volatile uint8_t *trim, const struct s905x5m_usb_phy *phy);
static int wait_phy_ready(volatile uint8_t *control);
static int lock_phy_pll(volatile uint8_t *config);
static int start_controller(void);
static void log_ports(const char *when);
static int publish_bus(void);
static void register_usb_drivers(void);
static int function_present(const struct drv_pci_address *address);
static uint32_t function_dword(unsigned offset);
static void function_store(unsigned offset, uint32_t dword);
static void establish_interrupt(void);
static void controller_interrupt(int irq, kern_irq_ack_t acknowledge, void *argument);
static void start_polling(void);
static void poll_worker(void *argument);
static void serve_function(void);

/*
 * Brings up the USB host the device tree names and hands the controller
 * to the xHCI driver.
 */
int
drv_s905x5m_usb_init(
	uint64_t fdt_phys)
{
	struct s905x5m_usb_phy phy;
	int error;

	/* Opens the device tree and maps the shared blocks. */
	error = drv_s905x5m_soc_init(fdt_phys);
	if (error != 0)
		return -error;

	/* Brings the PCI core into service. */
	error = drv_pci_init();
	if (error != 0 && error != EALREADY)
		return -error;

	/* Registers the USB drivers, so publishing the bus binds the controller. */
	register_usb_drivers();

	/* Prepares the lock of the configuration registers and the pin. */
	spin_init(&s905x5m_usb.lock, LOCK_RANK_DEVICE, "S905X5M USB");

	/* Finds the controller and its interrupt. */
	error = find_controller();
	if (error != 0)
		return -error;

	/* Reads where the PHY is and how it is set. */
	describe_phy(&phy);

	/* Switches the controller's gate on. */
	drv_s905x5m_clock_update(DRV_S905X5M_CLOCK_GATES1,
				 1U << S905X5M_USB_GATE_BIT,
				 1U << S905X5M_USB_GATE_BIT);

	/* Wakes the PHY. */
	error = start_phy(&phy);
	if (error != 0) {
		kern_logf("s905x5m-usb: PHY unavailable (%d)\n", error);
		return -error;
	}

	/* Makes the controller a host and reads its identity. */
	error = start_controller();
	if (error != 0)
		return -error;

	/* Shows the controller to the PCI core, which binds the xHCI driver. */
	s905x5m_usb.present = 1;
	error = publish_bus();
	if (error != 0) {
		kern_logf("s905x5m-usb: bus not published (%d)\n", error);
		return -error;
	}

	/* Succeeded: the controller is bound or reported. */
	return 0;
}

/*
 * Starts serving the controller's interrupt and has it look at its root
 * ports.
 */
void
drv_s905x5m_usb_refresh(
	void)
{
	/* Leaves a board without the bus alone. */
	if (!s905x5m_usb.started) {
		kern_logf("s905x5m-usb: no controller to start\n");
		return;
	}

	/* Routes the controller's interrupt, then starts the polling service. */
	establish_interrupt();
	start_polling();

	/* Has the xHCI controller look at its root ports. */
	drv_pci_xhci_probe_roots();

	/* Writes what the root ports show now. */
	log_ports("after the probe");
}

/* Finds the CRG controller, its registers and its interrupt. */
static int
find_controller(
	void)
{
	uint64_t size;
	int error;

	/* Finds the enabled controller at the SoC's address. */
	error = drv_s905x5m_find_node("amlogic, crg-host-drd", S905X5M_USB_REGISTERS, &s905x5m_usb.node);
	if (error != 0) {
		kern_logf("s905x5m-usb: no CRG controller at %llx in the device tree\n",
			  (unsigned long long)S905X5M_USB_REGISTERS);
		return ENODEV;
	}

	/* Reads the register block's size; the BAR must be a power of two. */
	error = drv_fdt_reg(drv_s905x5m_fdt(), s905x5m_usb.node, 0, &s905x5m_usb.base, &size);
	if (error != 0)
		return error;
	if (size < S905X5M_USB_REGISTER_BYTES)
		return EINVAL;
	s905x5m_usb.size = S905X5M_USB_REGISTER_BYTES;

	/* Reads the controller's interrupt. */
	error = drv_s905x5m_node_interrupt(s905x5m_usb.node, 0, &s905x5m_usb.interrupt);
	if (error != 0) {
		kern_logf("s905x5m-usb: no interrupt (%d); polling alone\n", error);
		s905x5m_usb.interrupt.irq = 0;
	}

	/* Succeeded: the controller is known. */
	return 0;
}

/*
 * Reads the PHY the controller's phys property names: its register
 * blocks, its reset bits and its settings.  What the tree does not say is
 * the S905X5M's.
 */
static void
describe_phy(
	struct s905x5m_usb_phy *phy)
{
	const uint8_t *value;
	uint64_t address;
	uint64_t size;
	uint32_t length;
	uint32_t phandle;
	uint32_t node;
	int error;

	/* Starts from the S905X5M's PHY. */
	phy->control = S905X5M_PHY_CONTROL;
	phy->trim = S905X5M_PHY_TRIM;
	phy->config = S905X5M_PHY_CONFIG;
	phy->usb_reset_bit = S905X5M_USB_RESET_BIT;
	phy->phy_reset_bit = S905X5M_PHY_RESET_LEVEL_BIT;
	phy->edge_driver = S905X5M_PHY_EDGE_DEFAULT;
	phy->trim_default = S905X5M_PHY_TRIM_DEFAULT;

	/* Finds the PHY's node; without it the defaults stand. */
	error = drv_fdt_property(drv_s905x5m_fdt(), s905x5m_usb.node, "phys", &value, &length);
	if (error != 0)
		return;
	if (length < 4U)
		return;
	phandle = (uint32_t)drv_fdt_cells_value(value, 0, 1U);
	error = drv_fdt_find_phandle(drv_s905x5m_fdt(), phandle, &node);
	if (error != 0)
		return;

	/* Reads the control words, the trim word and the configuration block. */
	error = drv_s905x5m_node_raw_reg(node, 0, &address, &size);
	if (error == 0)
		phy->control = address;
	error = drv_s905x5m_node_raw_reg(node, 2, &address, &size);
	if (error == 0)
		phy->trim = address;
	error = drv_s905x5m_node_raw_reg(node, 3, &address, &size);
	if (error == 0)
		phy->config = address;

	/* Reads the reset bits. */
	phy->usb_reset_bit = drv_s905x5m_node_number(node, "usb-reset-bit", S905X5M_USB_RESET_BIT);
	phy->phy_reset_bit = drv_s905x5m_node_number(node, "phy-reset-level-bits", S905X5M_PHY_RESET_LEVEL_BIT);

	/* Reads the edge driver setting and the default trim from the PLL settings. */
	error = drv_fdt_property(drv_s905x5m_fdt(), node, "pll-settings", &value, &length);
	if (error != 0)
		return;
	if (length < 5U * 4U)
		return;
	phy->edge_driver = (uint32_t)drv_fdt_cells_value(value, 3U, 1U);
	phy->trim_default = (uint32_t)drv_fdt_cells_value(value, 4U, 1U);
}

/* Maps the page that holds a register; reports the register's address in the mapping. */
static int
map_page(
	uint64_t address,
	volatile uint8_t **mapped)
{
	void *window;
	int error;

	/* Maps the page uncached. */
	error = kern_device_map(address & ~S905X5M_PAGE_MASK, S905X5M_PAGE_BYTES, KERN_DEVICE_UNCACHED, &window);
	if (error != 0)
		return error;

	/* Succeeded: the register inside the page. */
	*mapped = (volatile uint8_t *)window + (address & S905X5M_PAGE_MASK);
	return 0;
}

/*
 * Wakes the USB 2 PHY: takes it out of reset, resets the USB block,
 * selects the host mode, resets the PHY's configuration, calibrates it,
 * waits for it to be ready and locks its PLL.
 *
 * Procedure referenced: Hardkernel common_drivers odroids7d-6.12.y
 * @34daba9 drivers/usb/phy_meson_usb/phy-meson-aml-usb2-com.c
 * (meson_u2phy_aml_init, meson_u2phy_hold_reset, meson_u2phy_usb_reset,
 * meson_u2phy_set_mode, meson_u2phy_reset_phycfg,
 * meson_usb2phy_wait_ready) and socs/phy-meson-s7d-usb.c
 * (meson_usb2phy_s7d_cali, meson_usb2phy_s7d_set_pll).
 */
static int
start_phy(
	const struct s905x5m_usb_phy *phy)
{
	volatile uint8_t *control;
	volatile uint8_t *trim;
	volatile uint8_t *config;
	uint32_t r0;
	uint32_t r1;
	uint32_t pll;
	int error;

	/* Maps the PHY's control words, its trim word and its configuration block. */
	error = map_page(phy->control, &control);
	if (error != 0)
		return error;
	error = map_page(phy->trim, &trim);
	if (error != 0)
		return error;
	error = map_page(phy->config, &config);
	if (error != 0)
		return error;

	/* Names the reads to come, which an unpowered block may never answer. */
	kern_logf("s905x5m-usb: waking the PHY at %llx (configuration %llx)\n",
		  (unsigned long long)phy->control,
		  (unsigned long long)phy->config);

	/* Takes the PHY out of reset. */
	drv_s905x5m_reset_release(phy->phy_reset_bit);

	/* Resets the USB block once, and gives it time. */
	drv_s905x5m_reset_pulse(phy->usb_reset_bit);
	drv_s905x5m_delay_us(S905X5M_PHY_RESET_US);
	drv_s905x5m_delay_us(S905X5M_PHY_SETTLE_US);

	/* Selects the host mode and ends the power-on reset. */
	r0 = kern_mmio_read32(control + S905X5M_PHY_R0);
	r0 |= S905X5M_PHY_R0_HOST;
	r0 &= ~S905X5M_PHY_R0_POWER_ON_RESET;
	kern_mmio_write32(control + S905X5M_PHY_R0, r0);
	drv_s905x5m_delay_us(S905X5M_PHY_SETTLE_US);

	/* Resets the PHY's state machine with the mode in place. */
	drv_s905x5m_reset_hold(phy->phy_reset_bit);
	drv_s905x5m_delay_us(S905X5M_PHY_SETTLE_US);
	drv_s905x5m_reset_release(phy->phy_reset_bit);
	drv_s905x5m_delay_us(S905X5M_PHY_SETTLE_US);

	/* Calibrates the PHY. */
	calibrate_phy(config, trim, phy);

	/* Waits for the PHY to be ready; a PHY that is not goes on, as the vendor's driver does. */
	error = wait_phy_ready(control);
	if (error != 0) {
		r1 = kern_mmio_read32(control + S905X5M_PHY_R1);
		kern_logf("s905x5m-usb: PHY not ready (r1 %08x)\n", r1);
	}

	/* Locks the PHY's PLL. */
	error = lock_phy_pll(config);
	if (error != 0)
		return error;

	/* Writes the PHY's state: its mode, its ready bit and its PLL's lock. */
	r0 = kern_mmio_read32(control + S905X5M_PHY_R0);
	r1 = kern_mmio_read32(control + S905X5M_PHY_R1);
	pll = kern_mmio_read32(config + S905X5M_PHY_PLL);
	kern_logf("s905x5m-usb: PHY r0 %08x r1 %08x, PLL %08x\n", r0, r1, pll);

	/* Succeeded: the PHY runs. */
	return 0;
}

/*
 * Writes the PHY's calibration: its trim, from the efuse when the efuse
 * holds one, its disconnect and squelch thresholds, and its edge driver.
 */
static void
calibrate_phy(
	volatile uint8_t *config,
	volatile uint8_t *trim,
	const struct s905x5m_usb_phy *phy)
{
	uint32_t efuse;
	uint32_t steps;
	uint32_t value;
	uint32_t bit;

	/* Takes the trim from the efuse when it is valid there, the default otherwise. */
	efuse = kern_mmio_read32(trim);
	steps = phy->trim_default;
	if ((efuse & S905X5M_PHY_TRIM_VALID) != 0) {
		steps = ((efuse >> S905X5M_PHY_TRIM_SHIFT) & S905X5M_PHY_TRIM_FIELD) + S905X5M_PHY_TRIM_OFFSET;
		if (steps > S905X5M_PHY_TRIM_MAX)
			steps = S905X5M_PHY_TRIM_MAX;
	}

	/* Writes the trim as that many low bits set. */
	value = kern_mmio_read32(config + S905X5M_PHY_TRIM_REGISTER);
	value &= ~S905X5M_PHY_TRIM_MASK;
	for (bit = 0; bit < steps; bit++)
		value |= 1U << bit;
	kern_mmio_write32(config + S905X5M_PHY_TRIM_REGISTER, value);

	/* Writes the disconnect and squelch thresholds, and clears the unused disable bit. */
	kern_mmio_write32(config + S905X5M_PHY_THRESHOLDS, S905X5M_PHY_THRESHOLDS_VALUE);
	value = kern_mmio_read32(config + S905X5M_PHY_CONFIG_DISABLE);
	value &= ~S905X5M_PHY_CONFIG_DISABLE_BIT;
	kern_mmio_write32(config + S905X5M_PHY_CONFIG_DISABLE, value);

	/* Writes the edge driver setting for the signal's quality. */
	kern_mmio_write32(config + S905X5M_PHY_EDGE_DRIVER, phy->edge_driver);

	/* Writes the trim used. */
	kern_logf("s905x5m-usb: PHY trim efuse %08x, %u steps (%08x)\n", efuse, steps, value);
}

/* Waits up to about a millisecond for the PHY's ready bit. */
static int
wait_phy_ready(
	volatile uint8_t *control)
{
	uint32_t r1;
	unsigned poll;

	/* Polls the ready bit. */
	for (poll = 0; poll < S905X5M_PHY_READY_POLLS; poll++) {
		/* A ready PHY ends the wait. */
		r1 = kern_mmio_read32(control + S905X5M_PHY_R1);
		if ((r1 & S905X5M_PHY_R1_READY) != 0)
			return 0;

		/* Gives the PHY a moment. */
		drv_s905x5m_delay_us(S905X5M_PHY_READY_POLL_US);
	}

	/* The PHY never said it was ready. */
	return ETIMEDOUT;
}

/*
 * Starts the PHY's PLL step by step and waits for its lock, trying again
 * from its default value a few times.
 */
static int
lock_phy_pll(
	volatile uint8_t *config)
{
	uint32_t value;
	unsigned attempt;
	unsigned poll;

	/* Gives the PLL to the control register. */
	value = kern_mmio_read32(config + S905X5M_PHY_PLL);
	value |= S905X5M_PHY_PLL_ENABLE;
	kern_mmio_write32(config + S905X5M_PHY_PLL, value);
	drv_s905x5m_delay_us(S905X5M_PHY_PLL_STEP_US);

	/* Starts the PLL from its default value, as often as it takes to lock. */
	for (attempt = 0; attempt < S905X5M_PHY_PLL_TRIES; attempt++) {
		/* Writes the default value with the control enabled. */
		value = S905X5M_PHY_PLL_DEFAULT | S905X5M_PHY_PLL_ENABLE;
		kern_mmio_write32(config + S905X5M_PHY_PLL, value);
		drv_s905x5m_delay_us(S905X5M_PHY_PLL_STEP_US);

		/* Turns the bias on. */
		value |= S905X5M_PHY_PLL_BIAS;
		kern_mmio_write32(config + S905X5M_PHY_PLL, value);
		drv_s905x5m_delay_us(S905X5M_PHY_PLL_STEP_US);

		/* Releases the PLL's reset. */
		value |= S905X5M_PHY_PLL_RESET;
		kern_mmio_write32(config + S905X5M_PHY_PLL, value);
		drv_s905x5m_delay_us(S905X5M_PHY_PLL_STEP_US);

		/* Lets the PLL lock. */
		value |= S905X5M_PHY_PLL_LOCK_ENABLE;
		kern_mmio_write32(config + S905X5M_PHY_PLL, value);

		/* Waits for the lock. */
		for (poll = 0; poll < S905X5M_PHY_PLL_POLLS; poll++) {
			/* A locked PLL ends the bring-up. */
			drv_s905x5m_delay_us(S905X5M_PHY_PLL_STEP_US);
			value = kern_mmio_read32(config + S905X5M_PHY_PLL);
			if ((value & S905X5M_PHY_PLL_LOCKED) != 0)
				return 0;
		}

		/* Reports the try that did not lock. */
		kern_logf("s905x5m-usb: PHY PLL not locked (%08x), try %u\n", value, attempt + 1U);
	}

	/* The PLL never locked. */
	return ETIMEDOUT;
}

/*
 * Maps the controller, makes it a host, and writes its capability
 * registers and its ports to the log.
 */
static int
start_controller(
	void)
{
	uint32_t role;
	uint32_t host_role;
	uint32_t capability;
	uint32_t structure;
	void *mapped;
	int error;

	/* Maps the controller's registers, kept for the log of its ports. */
	error = kern_device_map(s905x5m_usb.base, (size_t)s905x5m_usb.size, KERN_DEVICE_UNCACHED, &mapped);
	if (error != 0)
		return error;
	s905x5m_usb.registers = mapped;

	/* Names the read to come, which an unpowered controller may never answer. */
	kern_logf("s905x5m-usb: about to read the controller at %llx (role register, then xHCI CAPLENGTH)\n",
		  (unsigned long long)s905x5m_usb.base);

	/* Makes the controller a host. */
	role = kern_mmio_read32(s905x5m_usb.registers + S905X5M_USB_ROLE);
	kern_mmio_write32(s905x5m_usb.registers + S905X5M_USB_ROLE, role & ~S905X5M_USB_ROLE_DEVICE);
	host_role = kern_mmio_read32(s905x5m_usb.registers + S905X5M_USB_ROLE);

	/* Reads the capability length, the interface version and the structural parameters. */
	capability = kern_mmio_read32(s905x5m_usb.registers + XHCI_CAPABILITY);
	structure = kern_mmio_read32(s905x5m_usb.registers + XHCI_STRUCTURE1);
	kern_logf("s905x5m-usb: role %08x -> %08x, CAPLENGTH %02x HCIVERSION %04x HCSPARAMS1 %08x\n",
		  role,
		  host_role,
		  capability & 0xffU,
		  capability >> 16,
		  structure);

	/* Refuses a controller that reads as nothing. */
	if (capability == 0 || capability == 0xffffffffU) {
		kern_logf("s905x5m-usb: no xHCI answers at %llx\n", (unsigned long long)s905x5m_usb.base);
		return ENODEV;
	}

	/* Writes what the root ports show before the xHCI driver resets them. */
	log_ports("before the reset");

	/* Succeeded: the controller is a host. */
	return 0;
}

/* Writes the status of every root port. */
static void
log_ports(
	const char *when)
{
	uint32_t capability;
	uint32_t structure;
	uint32_t ports;
	uint32_t port;
	uint32_t status;
	uint32_t offset;

	/* Reads where the operational registers start and how many ports there are. */
	capability = kern_mmio_read32(s905x5m_usb.registers + XHCI_CAPABILITY);
	structure = kern_mmio_read32(s905x5m_usb.registers + XHCI_STRUCTURE1);
	ports = structure >> XHCI_PORTS_SHIFT;

	/* Writes each port's status. */
	for (port = 0; port < ports; port++) {
		/* Reads the port's status register. */
		offset = (capability & 0xffU) + XHCI_PORT_STATUS + port * XHCI_PORT_STRIDE;
		if (offset + 4U > s905x5m_usb.size)
			break;
		status = kern_mmio_read32(s905x5m_usb.registers + offset);
		kern_logf("s905x5m-usb: port %u PORTSC %08x (%s)\n", port + 1U, status, when);
	}
}

/*
 * Creates the DMA device and the root bus with the controller on it, and
 * has the PCI core enumerate it.
 */
static int
publish_bus(
	void)
{
	struct drv_dma_constraints constraints;
	int error;

	/* Describes the controller's DMA: 32-bit addresses, coherence as the tree says. */
	kern_memset(&constraints, 0, sizeof(constraints));
	constraints.address_bits = S905X5M_USB_DMA_ADDRESS_BITS;
	constraints.max_segment_size = S905X5M_USB_DMA_MAX_SEGMENT;
	constraints.segment_boundary = 0;
	constraints.coherent = drv_fdt_dma_coherent(drv_s905x5m_fdt(), s905x5m_usb.node, 0);
	error = drv_dma_device_create(&constraints, &s905x5m_usb.dma);
	if (error != 0)
		return error;

	/* The BAR starts at the controller's registers, which the bus addresses as the CPU does. */
	s905x5m_usb.bar = (uint32_t)s905x5m_usb.base;

	/* Creates the root bus. */
	error = drv_pci_bus_create_root(S905X5M_USB_SEGMENT,
					S905X5M_USB_BUS,
					&s905x5m_usb_bus_ops,
					&s905x5m_usb,
					s905x5m_usb.dma,
					&s905x5m_usb.root);
	if (error != 0) {
		(void)drv_dma_device_destroy(s905x5m_usb.dma);
		s905x5m_usb.dma = NULL;
		return error;
	}

	/* Marks the bus published: refresh serves it from now on. */
	s905x5m_usb.started = 1;

	/* Enumerates the function, which attaches the xHCI driver. */
	error = drv_pci_bus_scan_tree(s905x5m_usb.root);
	if (error != 0)
		return error;

	/* Succeeded: the PCI core owns the bus. */
	return 0;
}

/*
 * Registers the USB core and the USB drivers the configuration selects.
 *
 * They must be known before the PCI core enumerates the bus, because
 * enumeration binds the xHCI controller and it then finds its devices.
 * A failure is recorded; the boot goes on without that driver.
 */
static void
register_usb_drivers(
	void)
{
#if CONFIG_DRIVER_PCI_XHCI
	int error;

	/* Brings up the USB core. */
	error = drv_usb_init();
	if (error != 0) {
		kern_logf("usb: core initialization failed (%d)\n", error);
		return;
	}

	/* Registers bulk-only USB disks. */
#if CONFIG_DRIVER_USB_STORAGE
	error = drv_usb_storage_driver_register();
	if (error != 0)
		kern_logf("usb: storage registration failed (%d)\n", error);
#endif

	/* Registers USB Attached SCSI disks. */
#if CONFIG_DRIVER_USB_STORAGE
	error = drv_usb_uas_driver_register();
	if (error != 0)
		kern_logf("usb: UAS registration failed (%d)\n", error);
#endif

	/* Registers USB NCM network interfaces. */
#if CONFIG_DRIVER_USB_CDC_NCM
	error = drv_usb_cdc_ncm_driver_register();
	if (error != 0)
		kern_logf("usb: CDC NCM registration failed (%d)\n", error);
#endif

	/* Registers USB ECM network interfaces. */
#if CONFIG_DRIVER_USB_CDC_ECM
	error = drv_usb_cdc_ecm_driver_register();
	if (error != 0)
		kern_logf("usb: CDC ECM registration failed (%d)\n", error);
#endif

	/* Registers the keyboard and mouse driver. */
#if CONFIG_DRIVER_USB_HID
	error = drv_usb_hid_driver_register();
	if (error != 0)
		kern_logf("usb: HID input driver registration failed (%d)\n", error);
#endif

	/* Registers the hub driver, for the board's hub and what is behind it. */
#if CONFIG_DRIVER_USB_HUB
	error = drv_usb_hub_driver_register();
	if (error != 0)
		kern_logf("usb: hub driver registration failed (%d)\n", error);
#endif

	/* Registers the host controller driver. */
	error = drv_pci_xhci_driver_register();
	if (error != 0)
		kern_logf("usb: xHCI PCI driver registration failed (%d)\n", error);
#endif
}

/* Reports whether an address names the one function, device 0 of the root bus. */
static int
function_present(
	const struct drv_pci_address *address)
{
	/* Refuses a missing address and another bus. */
	if (address == NULL)
		return 0;
	if (address->segment != S905X5M_USB_SEGMENT)
		return 0;
	if (address->bus != S905X5M_USB_BUS)
		return 0;

	/* Refuses another device or function. */
	if (address->device != 0)
		return 0;
	if (address->function != 0)
		return 0;

	/* A controller that did not start is not shown. */
	if (!s905x5m_usb.present)
		return 0;

	/* Succeeded: the address names the controller. */
	return 1;
}

/*
 * Reads one dword of the function's configuration space; the caller holds
 * the host's lock.
 */
static uint32_t
function_dword(
	unsigned offset)
{
	uint32_t size_mask;
	uint32_t dword;

	/* Answers each register the function has; the others read as zero. */
	switch (offset) {
	case PCI_ID:
	case PCI_SUBSYSTEM:
		dword = S905X5M_USB_IDENTITY;
		break;
	case PCI_COMMAND:
		dword = s905x5m_usb.command;
		break;
	case PCI_CLASS_REVISION:
		dword = S905X5M_USB_CLASS_XHCI << PCI_CLASS_SHIFT;
		break;
	case PCI_BAR0:
		/* The BAR keeps only the bits above its size, as the sizing probe expects; it is a 32-bit memory BAR. */
		size_mask = ~((uint32_t)s905x5m_usb.size - 1U);
		dword = s905x5m_usb.bar & size_mask;
		break;
	case PCI_INTERRUPT:
		dword = S905X5M_USB_INTERRUPT_PIN_A << PCI_INTERRUPT_PIN_SHIFT;
		break;
	default:
		dword = 0;
		break;
	}

	/* Succeeded: the register's value. */
	return dword;
}

/*
 * Stores a written dword into the function's configuration space; the
 * caller holds the host's lock.  Only the command and the BAR are
 * writable.
 */
static void
function_store(
	unsigned offset,
	uint32_t dword)
{
	/* Keeps the writable registers; the others ignore writes. */
	switch (offset) {
	case PCI_COMMAND:
		s905x5m_usb.command = (uint16_t)(dword & PCI_COMMAND_KEPT);
		break;
	case PCI_BAR0:
		s905x5m_usb.bar = dword & ~0xfU;
		break;
	default:
		break;
	}
}

/* Reads the function's configuration space at any width. */
static int
host_config_read(
	void *context,
	const struct drv_pci_address *address,
	unsigned offset,
	unsigned width,
	uint32_t *value)
{
	unsigned long enabled;
	uint32_t dword;
	unsigned shift;
	int present;

	(void)context;

	/* Refuses a malformed access. */
	if (value == NULL)
		return EINVAL;
	if (offset >= PCI_CONFIG_BYTES)
		return EINVAL;
	if (width != 1U && width != 2U && width != 4U)
		return EINVAL;
	if ((offset & (width - 1U)) != 0)
		return EINVAL;

	/* An absent function reads as all ones. */
	present = function_present(address);
	if (!present) {
		*value = 0xffffffffU;
		if (width < 4U)
			*value &= (1U << (width * 8U)) - 1U;
		return 0;
	}

	/* Reads the dword the access is in. */
	enabled = spin_lock_irqsave(&s905x5m_usb.lock);

	dword = function_dword(offset & ~3U);

	spin_unlock_irqrestore(&s905x5m_usb.lock, enabled);

	/* Narrows it to the access. */
	shift = (offset & 3U) * 8U;
	dword >>= shift;
	if (width < 4U)
		dword &= (1U << (width * 8U)) - 1U;

	/* Succeeded: the register's value. */
	*value = dword;
	return 0;
}

/* Writes the function's configuration space at any width. */
static int
host_config_write(
	void *context,
	const struct drv_pci_address *address,
	unsigned offset,
	unsigned width,
	uint32_t value)
{
	unsigned long enabled;
	uint32_t dword;
	uint32_t mask;
	unsigned shift;
	int present;

	(void)context;

	/* Refuses a malformed access. */
	if (offset >= PCI_CONFIG_BYTES)
		return EINVAL;
	if (width != 1U && width != 2U && width != 4U)
		return EINVAL;
	if ((offset & (width - 1U)) != 0)
		return EINVAL;

	/* An absent function ignores writes. */
	present = function_present(address);
	if (!present)
		return 0;

	/* Finds the bytes of the dword the access writes. */
	shift = (offset & 3U) * 8U;
	mask = 0xffffffffU;
	if (width < 4U)
		mask = ((1U << (width * 8U)) - 1U) << shift;

	/* Merges the access into its dword and stores the dword. */
	enabled = spin_lock_irqsave(&s905x5m_usb.lock);

	dword = function_dword(offset & ~3U);
	dword &= ~mask;
	dword |= (value << shift) & mask;
	function_store(offset & ~3U, dword);

	spin_unlock_irqrestore(&s905x5m_usb.lock, enabled);

	/* Succeeded: the register holds the value. */
	return 0;
}

/* Maps part of the function's BAR: the controller's registers. */
static int
host_map_bar(
	void *context,
	struct drv_pci_device *device,
	const struct drv_pci_bar *bar,
	unsigned flags,
	struct drv_pci_mapping *mapping)
{
	struct drv_pci_address address;
	uint64_t offset;
	uint64_t cpu_address;
	void *mapped;
	int present;
	int error;

	(void)context;
	(void)flags;

	/* Refuses missing arguments and anything but the function's memory BAR. */
	if (device == NULL || bar == NULL || mapping == NULL)
		return EINVAL;
	if (bar->index != 0 || bar->size == 0)
		return EINVAL;
	drv_pci_device_address(device, &address);
	present = function_present(&address);
	if (!present)
		return EINVAL;

	/* Refuses a range outside the controller's registers. */
	if (bar->bus_address < s905x5m_usb.base)
		return EINVAL;
	offset = bar->bus_address - s905x5m_usb.base;
	if (offset >= s905x5m_usb.size || bar->size > s905x5m_usb.size - offset)
		return EINVAL;

	/* Maps the registers uncached. */
	cpu_address = s905x5m_usb.base + offset;
	error = kern_device_map(cpu_address, (size_t)bar->size, KERN_DEVICE_UNCACHED, &mapped);
	if (error != 0)
		return error;

	/* Describes the mapping; the first private word marks it as owned here. */
	mapping->address = mapped;
	mapping->physical_address = cpu_address;
	mapping->size = (size_t)bar->size;
	mapping->type = bar->type;
	mapping->private_data[0] = 1U;
	mapping->private_data[1] = 0;

	/* Succeeded: the driver reaches the controller at address. */
	return 0;
}

/* Removes a mapping made by host_map_bar(). */
static void
host_unmap_bar(
	void *context,
	struct drv_pci_mapping *mapping)
{
	(void)context;

	/* Ignores storage that holds no mapping of ours. */
	if (mapping == NULL)
		return;
	if (mapping->private_data[0] != 1U)
		return;

	/* Releases the window; a refusal changes nothing. */
	(void)kern_device_unmap(mapping->address, mapping->size);
	kern_memset(mapping, 0, sizeof(*mapping));
}

/* Gives the function its interrupt pin, the host's one vector. */
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
	struct drv_pci_address address;
	int present;

	(void)context;

	/* Refuses missing arguments and a request for more than one line. */
	if (device == NULL || irqs == NULL || count == NULL)
		return EINVAL;
	if (minimum != 1U || maximum < minimum)
		return ENOTSUP;

	/* Offers only the pin. */
	if (type != DRV_PCI_IRQ_INTX)
		return ENOTSUP;

	/* Refuses a function the bus does not have. */
	drv_pci_device_address(device, &address);
	present = function_present(&address);
	if (!present)
		return ENODEV;

	/* Succeeded: the function's pin. */
	irqs[0].type = DRV_PCI_IRQ_INTX;
	irqs[0].index = 0;
	irqs[0].vector = S905X5M_USB_VECTOR;
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

/*
 * Puts a PCI core line in the pin's entry, the pin still masked.
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
	unsigned long enabled;
	int error;

	(void)context;

	/* Refuses a pin the host does not have and an empty line. */
	if (vector != S905X5M_USB_VECTOR)
		return EINVAL;
	if (run == NULL || line == NULL)
		return EINVAL;

	/* Fills the pin's entry unless another line holds it. */
	enabled = spin_lock_irqsave(&s905x5m_usb.lock);

	error = 0;
	if (s905x5m_usb.run != NULL) {
		error = EBUSY;
	} else {
		s905x5m_usb.run = run;
		s905x5m_usb.line = line;
		s905x5m_usb.pin_enabled = 0;
	}

	spin_unlock_irqrestore(&s905x5m_usb.lock, enabled);

	/* Failed: the pin belongs to another line. */
	if (error != 0)
		return error;

	/* Succeeded: the line is in the table, its pin still masked. */
	return 0;
}

/*
 * Empties the pin's entry and waits until no service still runs it.
 *
 * Called in thread context.  After one second of waiting the entry is put
 * back as it was and EBUSY is reported.
 */
static int
host_intx_disestablish(
	void *context,
	unsigned vector,
	drv_pci_intx_run_t run,
	void *line)
{
	unsigned long enabled;
	unsigned in_flight;
	unsigned poll;
	int was_enabled;

	(void)context;

	/* Refuses a pin the host does not have. */
	if (vector != S905X5M_USB_VECTOR)
		return EINVAL;

	/* Masks the pin and empties its entry, after checking the entry is this line's. */
	enabled = spin_lock_irqsave(&s905x5m_usb.lock);

	if (s905x5m_usb.run != run || s905x5m_usb.line != line) {
		spin_unlock_irqrestore(&s905x5m_usb.lock, enabled);
		return EINVAL;
	}

	/* Keeps the pin's enable to put back, then empties the entry. */
	was_enabled = s905x5m_usb.pin_enabled;
	s905x5m_usb.pin_enabled = 0;
	s905x5m_usb.run = NULL;
	s905x5m_usb.line = NULL;

	spin_unlock_irqrestore(&s905x5m_usb.lock, enabled);

	/* Waits, a millisecond at a time, for a service still inside run. */
	in_flight = 0;
	for (poll = 0; poll < S905X5M_USB_DRAIN_POLLS; poll++) {
		/* Reads how many services still hold the pin. */
		enabled = spin_lock_irqsave(&s905x5m_usb.lock);

		in_flight = s905x5m_usb.in_flight;

		spin_unlock_irqrestore(&s905x5m_usb.lock, enabled);

		/* Drained: no service runs the line and none will. */
		if (in_flight == 0)
			break;

		/* Lets the other service finish. */
		kern_usleep_range(S905X5M_USB_DRAIN_POLL_US, S905X5M_USB_DRAIN_POLL_US);
	}

	/* Puts the entry back as it was when a service did not come back. */
	if (in_flight != 0) {
		enabled = spin_lock_irqsave(&s905x5m_usb.lock);

		s905x5m_usb.run = run;
		s905x5m_usb.line = line;
		s905x5m_usb.pin_enabled = was_enabled;

		spin_unlock_irqrestore(&s905x5m_usb.lock, enabled);

		/* Reports the pin whose service did not come back. */
		kern_logf("s905x5m-usb: pin still running after a second\n");
		return EBUSY;
	}

	/* Succeeded: the entry is empty and no service runs the line. */
	return 0;
}

/* Stops the pin from being served; called with interrupts off. */
static void
host_intx_mask(
	void *context,
	unsigned vector)
{
	unsigned long enabled;

	(void)context;

	/* Ignores a pin the host does not have. */
	if (vector != S905X5M_USB_VECTOR)
		return;

	/* Clears the pin's enable. */
	enabled = spin_lock_irqsave(&s905x5m_usb.lock);

	s905x5m_usb.pin_enabled = 0;

	spin_unlock_irqrestore(&s905x5m_usb.lock, enabled);
}

/* Lets the pin be served; called with interrupts off. */
static void
host_intx_unmask(
	void *context,
	unsigned vector)
{
	unsigned long enabled;

	(void)context;

	/* Ignores a pin the host does not have. */
	if (vector != S905X5M_USB_VECTOR)
		return;

	/* Sets the pin's enable. */
	enabled = spin_lock_irqsave(&s905x5m_usb.lock);

	s905x5m_usb.pin_enabled = 1;

	spin_unlock_irqrestore(&s905x5m_usb.lock, enabled);
}

/*
 * Routes the controller's GIC interrupt to the pin's service and chooses
 * the polling interval: rare when the interrupt is routed.
 */
static void
establish_interrupt(
	void)
{
	unsigned trigger;
	int irq;
	int error;

	/* Polls often unless the interrupt is routed. */
	s905x5m_usb.poll_ms = S905X5M_USB_POLL_MS;

	/* A controller without an interrupt is served by the polling alone. */
	if (s905x5m_usb.interrupt.irq == 0)
		return;
	irq = (int)s905x5m_usb.interrupt.irq;

	/* Holds the line off until the handler is in place. */
	kern_irq_mask(irq);

	/* Asks for the trigger the device tree names. */
	trigger = KERN_IRQ_TRIGGER_LEVEL;
	if (s905x5m_usb.interrupt.edge)
		trigger = KERN_IRQ_TRIGGER_EDGE;
	error = kern_irq_set_mode(irq, trigger, KERN_IRQ_POLARITY_HIGH);
	if (error != 0) {
		kern_logf("s905x5m-usb: interrupt %d mode not set (%d)\n", irq, error);
		return;
	}

	/* Binds the handler. */
	error = kern_irq_register(irq, controller_interrupt, &s905x5m_usb);
	if (error != 0) {
		kern_logf("s905x5m-usb: interrupt %d not routed (%d)\n", irq, error);
		return;
	}

	/* Lets the line in; the polling now only covers a lost edge. */
	kern_irq_unmask(irq);
	s905x5m_usb.interrupt_routed = 1;
	s905x5m_usb.poll_ms = S905X5M_USB_SAFETY_POLL_MS;
	kern_logf("s905x5m-usb: interrupt %d routed (%s)\n", irq, s905x5m_usb.interrupt.edge ? "edge" : "level");
}

/* Serves the controller's pin from its GIC interrupt. */
static void
controller_interrupt(
	int irq,
	kern_irq_ack_t acknowledge,
	void *argument)
{
	(void)irq;
	(void)argument;

	/* Runs the pin's line as the interrupt asks. */
	serve_function();

	/* Ends the interrupt. */
	kern_irq_send_eoi(acknowledge);
}

/* Starts the thread that serves the controller's pin. */
static void
start_polling(
	void)
{
	int error;

	/* Leaves a thread that already runs alone. */
	if (s905x5m_usb.polling)
		return;

	/* Creates the service thread. */
	error = kthread_create(poll_worker, &s905x5m_usb, SCHED_PRIORITY_DEFAULT, &s905x5m_usb.poll_thread);
	if (error != 0) {
		kern_logf("s905x5m-usb: polling unavailable (%d)\n", error);
		return;
	}

	/* Makes it runnable for as long as the kernel runs. */
	s905x5m_usb.polling = 1;
	thread_start(s905x5m_usb.poll_thread);
	kern_logf("s905x5m-usb: polling the controller every %u ms\n", s905x5m_usb.poll_ms);
}

/* Serves the controller's pin, then sleeps, for as long as the kernel runs. */
static void
poll_worker(
	void *argument)
{
	uint64_t deadline;

	(void)argument;

	/* Serves the pin at the polling interval. */
	for (;;) {
		/* Serves the controller. */
		serve_function();

		/* Sleeps on scheduler ticks until the next round. */
		deadline = sched_ticks() + kern_ms_to_ticks(s905x5m_usb.poll_ms);
		sched_sleep(deadline);
	}
}

/*
 * Serves the controller's pin as its interrupt would: runs the PCI core's
 * line, with interrupts off, when the pin is established, unmasked and
 * the function has not disabled its interrupt.
 */
static void
serve_function(
	void)
{
	drv_pci_intx_run_t run;
	unsigned long enabled;
	bool interrupts;
	void *line;

	/* Takes the pin's entry and counts this service on it. */
	run = NULL;
	line = NULL;
	enabled = spin_lock_irqsave(&s905x5m_usb.lock);

	if (s905x5m_usb.run != NULL &&
	    s905x5m_usb.pin_enabled &&
	    (s905x5m_usb.command & PCI_COMMAND_INTX_DISABLE) == 0) {
		run = s905x5m_usb.run;
		line = s905x5m_usb.line;
		s905x5m_usb.in_flight++;
	}

	spin_unlock_irqrestore(&s905x5m_usb.lock, enabled);

	/* Leaves a pin nobody serves. */
	if (run == NULL)
		return;

	/* Runs the line as interrupt context would, with interrupts off. */
	interrupts = kern_irq_disable();
	run(line);
	if (interrupts)
		kern_irq_enable();

	/* Drops this service from the pin, which lets a removal finish. */
	enabled = spin_lock_irqsave(&s905x5m_usb.lock);

	s905x5m_usb.in_flight--;

	spin_unlock_irqrestore(&s905x5m_usb.lock, enabled);
}
