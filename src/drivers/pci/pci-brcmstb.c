/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Broadcom STB PCIe root complex (BCM2711 and BCM2712).
 *
 * The root complex is bus 0: its own configuration space is the start of its
 * register block and it presents itself as a PCI-to-PCI bridge (the root
 * port).  Everything below the root port is reached through an index
 * register and a 4 KiB data window.  The CPU reaches device BARs through one
 * outbound window, and devices reach system memory through an inbound
 * window.  The controller does not snoop the CPU caches.
 *
 * Nothing here relies on the firmware having set the controller up: the
 * start sequence resets it, powers the SerDes, programs both windows, releases
 * the endpoint from reset and waits for the link.  The PCI core reads bus
 * numbers and BARs but does not assign them, so this driver numbers the bus
 * below the root port and places the endpoints' memory BARs in the outbound
 * window before the core enumerates the tree.
 *
 * The BCM2712 moves the bridge's reset out of the controller, into a reset
 * block the board drives, and PERST# into the controller's PCIe control
 * register.  Each of its inbound windows is an RC BAR with a remap of its
 * own onto the system bus, one of them carrying the board's receiver of
 * message interrupts.  Its PHY needs the reference clock and its system-bus
 * bridge needs its error replies and timeouts set before the link trains.
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

/*
 * The BCM2712's registers where they differ from the BCM2711's: the debug
 * register with its clock-request, reference-clock and SerDes bits, the
 * controller's own interrupt summary, PERST# in the PCIe control register
 * (its bit releases the endpoint when set), and the inbound windows, each
 * an RC BAR (its PCI base with the size code in the low bits) with a remap
 * onto the system bus (the CPU address with an enable bit).
 *
 * Values referenced: Raspberry Pi Linux rpi-6.12.y
 * drivers/pci/controller/pcie-brcmstb.c (pcie_offsets_bcm7712:
 * PCIE_HARD_DEBUG and PCIE_INTR2_CPU_BASE; the HARD_DEBUG masks
 * CLKREQ_DEBUG_ENABLE, PERST_ASSERT, REFCLK_OVRD_ENABLE, REFCLK_OVRD_OUT,
 * L1SS_ENABLE and SERDES_IDDQ; MSI_INT_CLR and MSI_INT_MASK_SET;
 * PCIE_MISC_PCIE_CTRL and its PCIE_PERSTB mask; brcm_bar_reg_offset() and
 * brcm_ubus_reg_offset() for BAR1 to BAR3 and the remap's ACCESS_EN
 * mask).
 */
#define BCM2712_HARD_DEBUG		0x4304U
#define BCM2712_HARD_DEBUG_CLKREQ	0x00000002U
#define BCM2712_HARD_DEBUG_PERST_ASSERT	0x00000008U
#define BCM2712_HARD_DEBUG_REFCLK_ENABLE	0x00010000U
#define BCM2712_HARD_DEBUG_REFCLK_OUT	0x00100000U
#define BCM2712_HARD_DEBUG_L1SS		0x00200000U
#define BCM2712_HARD_DEBUG_SERDES_IDDQ	0x08000000U
#define BCM2712_INTR2_CLEAR		0x4408U
#define BCM2712_INTR2_MASK_SET		0x4410U
#define BCM2712_PCIE_CTRL		0x4064U
#define BCM2712_PCIE_CTRL_PERSTB	0x00000004U
#define BCM2712_RC_BAR_LO		0x402cU
#define BCM2712_RC_BAR_HI		0x4030U
#define BCM2712_UBUS_REMAP_LO		0x40acU
#define BCM2712_UBUS_REMAP_HI		0x40b0U
#define BCM2712_INBOUND_STRIDE		8U
#define BCM2712_UBUS_REMAP_ENABLE	0x00000001U
#define BCM2712_UBUS_REMAP_PAGE		0x00000fffU

/*
 * The BCM2712's controls of the system bus: 512-byte bursts, and the read
 * completion boundary taken from the payload size and from 64 bytes, in
 * MISC_CTRL beside the BCM2711's bits.
 *
 * Values referenced: Raspberry Pi Linux rpi-6.12.y
 * drivers/pci/controller/pcie-brcmstb.c
 * (PCIE_MISC_MISC_CTRL_PCIE_RCB_64B_MODE_MASK,
 * PCIE_MISC_MISC_CTRL_PCIE_RCB_MPS_MODE_MASK, and the burst code 2 that
 * brcm_pcie_setup() writes for the chips other than the BCM2711, the
 * BCM7278 and the MIPS ones).
 */
#define BCM2712_MISC_CTRL_RCB_64B	0x00000080U
#define BCM2712_MISC_CTRL_RCB_MPS	0x00000400U
#define BCM2712_MISC_CTRL_BURST_512	0x00200000U

/*
 * The BCM2712's tuning after the windows: the PHY's registers behind the
 * MDIO port (the address packet's port, extended port, register and
 * command fields; the write data's busy bit), the PHY's power-management
 * clock period, the system-bus bridge's error replies, the data an
 * erroring read returns, the bridge's and the configuration retry's
 * timeouts, the AXI interface's QoS fixes and its request limit, the
 * vendor message QoS enable, and the queue-fill priority maps.
 *
 * Values referenced: Raspberry Pi Linux rpi-6.12.y
 * drivers/pci/controller/pcie-brcmstb.c (PCIE_RC_DL_MDIO_ADDR,
 * PCIE_RC_DL_MDIO_WR_DATA, MDIO_PORT_MASK, MDIO_PORT_EXT_MASK,
 * MDIO_REGAD_MASK, MDIO_CMD_MASK, MDIO_CMD_WRITE, MDIO_DATA_DONE_MASK,
 * SET_ADDR_OFFSET, PCIE_RC_PL_PHY_CTL_15 and its PM_CLK_PERIOD mask,
 * PCIE_MISC_UBUS_CTRL with UBUS_PCIE_REPLY_ERR_DIS and
 * UBUS_PCIE_REPLY_DECERR_DIS, PCIE_MISC_AXI_READ_ERROR_DATA,
 * PCIE_MISC_UBUS_TIMEOUT, PCIE_MISC_RC_CONFIG_RETRY_TIMEOUT,
 * PCIE_MISC_AXI_INTF_CTRL with AXI_EN_RCLK_QOS_ARRAY_FIX,
 * AXI_EN_QOS_UPDATE_TIMING_FIX, AXI_DIS_QOS_GATING_IN_MASTER,
 * AXI_REQFIFO_EN_QOS_PROPAGATION and
 * AXI_MASTER_MAX_OUTSTANDING_REQUESTS_MASK, PCIE_MISC_CTRL_1 with
 * EN_VDM_QOS_CONTROL, PCIE_MISC_TC_QUEUE_TO_QOS_MAP(); and the values
 * brcm_pcie_post_setup_bcm2712() writes: the PHY block 0x1600, the 54 MHz
 * reference clock's seven registers and data, the period 0x12, the
 * timeouts 0xb2d0000 and 0xaba0000, and the request limit 15).
 */
#define BCM2712_MDIO_ADDRESS		0x1100U
#define BCM2712_MDIO_WRITE_DATA		0x1104U
#define BCM2712_MDIO_PORT_SHIFT		16U
#define BCM2712_MDIO_PORT_MASK		0x000f0000U
#define BCM2712_MDIO_PORT_EXT_SHIFT	21U
#define BCM2712_MDIO_PORT_EXT_MASK	0x00200000U
#define BCM2712_MDIO_REGISTER_MASK	0x0000ffffU
#define BCM2712_MDIO_COMMAND_WRITE	0x00000000U
#define BCM2712_MDIO_BUSY		0x80000000U
#define BCM2712_MDIO_PORT		0U
#define BCM2712_MDIO_SELECT_BLOCK	0x1fU
#define BCM2712_MDIO_REFCLK_BLOCK	0x1600U
#define BCM2712_MDIO_REFCLK_WRITES	7U
#define BCM2712_PHY_CTL_15		0x184cU
#define BCM2712_PHY_CTL_15_PM_PERIOD	0x000000ffU
#define BCM2712_PHY_PM_PERIOD_54MHZ	0x00000012U
#define BCM2712_UBUS_CTRL		0x40a4U
#define BCM2712_UBUS_CTRL_REPLY_ERR_OFF	0x00002000U
#define BCM2712_UBUS_CTRL_DECERR_OFF	0x00080000U
#define BCM2712_AXI_READ_ERROR_DATA	0x4170U
#define BCM2712_UBUS_TIMEOUT		0x40a8U
#define BCM2712_UBUS_TIMEOUT_250MS	0x0b2d0000U
#define BCM2712_CONFIG_RETRY_TIMEOUT	0x405cU
#define BCM2712_CONFIG_RETRY_240MS	0x0aba0000U
#define BCM2712_AXI_INTF_CTRL		0x416cU
#define BCM2712_AXI_RCLK_QOS_FIX	0x00002000U
#define BCM2712_AXI_QOS_TIMING_FIX	0x00001000U
#define BCM2712_AXI_NO_QOS_GATING	0x00000800U
#define BCM2712_AXI_QOS_PROPAGATION	0x00000080U
#define BCM2712_AXI_OUTSTANDING_MASK	0x0000003fU
#define BCM2712_AXI_OUTSTANDING_LIMIT	15U
#define BCM2712_MISC_CTRL_1		0x40a0U
#define BCM2712_MISC_CTRL_1_VDM_QOS	0x00000020U
#define BCM2712_QOS_MAP_FIRST		0x4160U
#define BCM2712_QOS_MAP_STRIDE		4U
#define BCM2712_QOS_MAP_COUNT		8U
#define BCM2712_QOS_LEVEL_MASK		0x0fU
#define BCM2712_QOS_LEVEL_BITS		4U

/*
 * The root complex's own link capability and link control 2 (each with the
 * highest generation in its low four bits), and its root capability with
 * the L1 substate mode field.
 *
 * Values referenced: Raspberry Pi Linux rpi-6.12.y
 * drivers/pci/controller/pcie-brcmstb.c (PCIE_RC_CFG_PRIV1_LINK_CAPABILITY
 * and its MAX_LINK_SPEED mask, PCIE_RC_CFG_PRIV1_ROOT_CAP and its
 * L1SS_MODE mask, the mode 2 that brcm_config_clkreq() writes for "safe",
 * and BRCM_PCIE_CAP_REGS); PCI Express Base Specification 4.0, section
 * 7.5.3.19 (Link Control 2 at 0x30 of the capability, the target link
 * speed in bits 3:0).
 */
#define BCM2712_LINK_CAPABILITY		0x04dcU
#define BCM2712_LINK_SPEED_MASK		0x0000000fU
#define BCM2712_RC_LINK_CONTROL2	(0x00acU + 0x30U)
#define BCM2712_ROOT_CAP		0x04f8U
#define BCM2712_ROOT_CAP_L1SS_MODE	0x000000f8U
#define BCM2712_ROOT_CAP_L1SS_OFF	0x00000010U
#define BCM2712_LINK_SPEED_MAX		3U

/*
 * The BCM2712's record of link training: four registers, each holding
 * four of the link states (LTSSM) the controller last passed through, one
 * byte each, 0xff for an entry not used yet.  Read only to report a link
 * that did not come up.
 *
 * Values referenced: Raspberry Pi Linux rpi-6.12.y
 * drivers/pci/controller/pcie-brcmstb.c (PCIE_RC_PL_LTSSM_HIST_0 and
 * PCIE_RC_PL_LTSSM_HIST(n)).
 */
#define BCM2712_LTSSM_HISTORY		0x1cecU
#define BCM2712_LTSSM_HISTORY_COUNT	4U

/* The BCM2712's register block must reach the configuration index register. */
#define BCM2712_REGISTER_SPAN		(BRCMSTB_EXT_CFG_INDEX + 4U)

/* The largest inbound window an RC BAR expresses, as log2 of its bytes. */
#define BCM2712_INBOUND_LOG2_MAX	36U

/* The longest PERST# lead time a device tree may ask for, in milliseconds. */
#define BCM2712_TPERST_MAX_MS		1000U

/* How often, and how far apart, an MDIO write is checked for its end. */
#define BCM2712_MDIO_POLLS		10U
#define BCM2712_MDIO_POLL_US		10U
#define BCM2712_MDIO_POLL_MAX_US	20U

/* The capabilities that describe a function's message interrupts. */
#define PCI_CAPABILITY_MSI		0x05U
#define PCI_CAPABILITY_MSIX		0x11U

/*
 * The MSI capability's message control word and its bit for a 64-bit
 * message address.
 *
 * Values referenced: PCI Local Bus Specification 3.0, section 6.8.1.3
 * (Message Control at offset 2 of the capability, bit 7 64-bit address
 * capable).
 */
#define PCI_MSI_CONTROL			0x02U
#define PCI_MSI_CONTROL_64BIT		0x0080U

/*
 * How long an endpoint may answer configuration requests with a retry
 * after its reset, in steps of 10 ms up to a second, and the vendor
 * numbers that mean it has not answered yet.
 *
 * Values referenced: PCI Express Base Specification 4.0, section 6.6.1
 * (Configuration Request Retry Status for up to 1.0 s after a
 * conventional reset) and section 7.5.1.1 (the vendor ID 0x0001 a root
 * port returns for a retried request when retry visibility is on).
 */
#define BRCMSTB_ENDPOINT_POLLS		100U
#define BRCMSTB_ENDPOINT_POLL_US	10000U
#define BRCMSTB_ENDPOINT_POLL_MAX_US	11000U
#define PCI_VENDOR_ABSENT		0xffffU
#define PCI_VENDOR_RETRY		0x0001U

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

/*
 * What devices behind the controller can reach with DMA: the BCM2711 31
 * bits, the BCM2712 any address its inbound window offsets.
 */
#define BRCMSTB_DMA_ADDRESS_BITS	31U
#define BCM2712_DMA_ADDRESS_BITS	64U
#define BRCMSTB_DMA_MAX_SEGMENT		(16U * 1024U * 1024U)

/*
 * One root complex the driver has started.
 *
 * It lives from a successful start for as long as the kernel runs: the PCI
 * core keeps it as the host of the root bus.  The configuration lock
 * serializes the index register and the data window it selects, which form
 * one access and must not interleave with another.  Window next is where
 * the next endpoint BAR goes; everything below it in the outbound window is
 * assigned.  Bridge ready says the BCM2712's bridge is out of reset, so its
 * registers may be written on the way out of a failed start.
 */
struct drv_pci_brcmstb {
	struct drv_pci_brcmstb_config config;
	volatile uint8_t *registers;
	struct spinlock config_lock;
	struct drv_dma_device *dma;
	struct drv_pci_bus *root;
	uint64_t window_next;
	bool link_up;
	bool bridge_ready;
};

/*
 * The BCM2712 PHY registers, and what each is given, that take the
 * 54 MHz crystal as the reference clock.
 *
 * The two tables are read in step, entry by entry, inside the PHY block
 * that BCM2712_MDIO_REFCLK_BLOCK selects.  They are constant.
 *
 * Values referenced: Raspberry Pi Linux rpi-6.12.y
 * drivers/pci/controller/pcie-brcmstb.c (brcm_pcie_post_setup_bcm2712():
 * the registers 0x16, 0x17, 0x18, 0x19, 0x1b, 0x1c and 0x1e, and the data
 * 0x50b9, 0xbda1, 0x0094, 0x97b4, 0x5030, 0x5030 and 0x0007).
 */
static const uint8_t bcm2712_refclk_registers[BCM2712_MDIO_REFCLK_WRITES] = {
	0x16U, 0x17U, 0x18U, 0x19U, 0x1bU, 0x1cU, 0x1eU
};
static const uint16_t bcm2712_refclk_data[BCM2712_MDIO_REFCLK_WRITES] = {
	0x50b9U, 0xbda1U, 0x0094U, 0x97b4U, 0x5030U, 0x5030U, 0x0007U
};

static int describe_outbound(const struct drv_fdt *fdt, uint32_t node, struct drv_pci_brcmstb_config *config);
static int describe_inbound(const struct drv_fdt *fdt, uint32_t node, struct drv_pci_brcmstb_config *config);
static int describe_bcm2712(const struct drv_fdt *fdt, uint32_t node, struct drv_pci_brcmstb_config *config);
static int describe_inbound_windows(const struct drv_fdt *fdt, uint32_t node, struct drv_pci_brcmstb_config *config);
static int describe_interrupts(const struct drv_fdt *fdt, uint32_t node, struct drv_pci_brcmstb_config *config);
static int decode_gic_interrupt(const struct drv_fdt *fdt, uint32_t controller, const uint8_t *specifier, unsigned *irq);
static int check_config(const struct drv_pci_brcmstb_config *config);
static int check_config_bcm2712(const struct drv_pci_brcmstb_config *config);
static int check_inbound_window(const struct drv_pci_brcmstb_inbound *window);
static int bring_up(struct drv_pci_brcmstb *host);
static int bring_up_bcm2712(struct drv_pci_brcmstb *host);
static uint32_t register_read(struct drv_pci_brcmstb *host, unsigned offset);
static void register_write(struct drv_pci_brcmstb *host, unsigned offset, uint32_t value);
static void register_update(struct drv_pci_brcmstb *host, unsigned offset, uint32_t clear, uint32_t set);
static void reset_controller(struct drv_pci_brcmstb *host);
static int reset_bridge_bcm2712(struct drv_pci_brcmstb *host);
static int set_inbound_windows_bcm2712(struct drv_pci_brcmstb *host);
static int tune_bcm2712(struct drv_pci_brcmstb *host);
static int mdio_write(struct drv_pci_brcmstb *host, unsigned port, unsigned reg, uint16_t data);
static void set_queue_priorities(struct drv_pci_brcmstb *host);
static void limit_link_speed(struct drv_pci_brcmstb *host);
static void set_clock_request_safe(struct drv_pci_brcmstb *host);
static void hold_endpoint(struct drv_pci_brcmstb *host);
static void stop_bcm2712(struct drv_pci_brcmstb *host);
static void wait_for_endpoint(struct drv_pci_brcmstb *host);
static void release_endpoint(struct drv_pci_brcmstb *host);
static int set_inbound_window(struct drv_pci_brcmstb *host);
static int inbound_size_log2(uint64_t size, unsigned *log2);
static int train_link(struct drv_pci_brcmstb *host);
static bool link_is_up(struct drv_pci_brcmstb *host);
static void report_link_down(struct drv_pci_brcmstb *host);
static void set_outbound_window(struct drv_pci_brcmstb *host);
static void report_link(struct drv_pci_brcmstb *host);
static int config_window(struct drv_pci_brcmstb *host, const struct drv_pci_address *address, unsigned offset, volatile uint8_t **window);
static bool config_access_valid(struct drv_pci_brcmstb *host, const struct drv_pci_address *address, unsigned offset, unsigned width);
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
static int allocate_message(struct drv_pci_brcmstb *host, struct drv_pci_device *device, enum drv_pci_irq_type type, struct drv_pci_irq *irqs, unsigned *count);
static int host_msi_establish(void *context, struct drv_pci_device *device, drv_pci_msi_run_t run, void *argument, unsigned *handle, uint64_t *address, uint32_t *data);
static int host_msi_disestablish(void *context, unsigned handle);

/*
 * The operations the PCI core calls on the root bus and every bus below it.
 *
 * The table is constant; the host each call works on arrives as its context.
 * It offers the legacy pins only.
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
 * The same operations for a root complex whose board receives message
 * interrupts: the PCI core then takes each function's message from the
 * host, which passes the request on to the board.
 */
static const struct drv_pci_bus_ops brcmstb_message_bus_ops = {
	.config_space_size = 4096,
	.config_read = host_config_read,
	.config_write = host_config_write,
	.map_bar = host_map_bar,
	.unmap_bar = host_unmap_bar,
	.allocate_irqs = host_allocate_irqs,
	.free_irqs = host_free_irqs,
	.msi_establish = host_msi_establish,
	.msi_disestablish = host_msi_disestablish
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
	unsigned index;
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

	/* Learns whether the controller is a BCM2712; anything else stays a BCM2711. */
	error = drv_fdt_string_index(fdt, node, "compatible", "brcm,bcm2712-pcie", &index);
	if (error != 0)
		return 0;

	/* Reads what only a BCM2712 has. */
	error = describe_bcm2712(fdt, node, config);
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
		hold_endpoint(host);
		stop_bcm2712(host);
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
	const struct drv_pci_bus_ops *ops;
	struct drv_dma_constraints constraints;
	uint64_t bus_offset;
	uint64_t physical_limit;
	int error;

	/* Refuses a missing host and a second publication. */
	if (host == NULL)
		return EINVAL;
	if (host->root != NULL)
		return EBUSY;

	/*
	 * Keeps backing below 2 GiB and inside the firmware's usable DMA range.
	 * The inbound window starts at CPU 0, but its PCI address need not be
	 * zero. Devices must receive that PCI alias rather than the CPU address.
	 * Identity windows retain the existing 31-bit mask and the controller
	 * never snoops the CPU caches.
	 */
	kern_memset(&constraints, 0, sizeof(constraints));
	constraints.address_bits = BRCMSTB_DMA_ADDRESS_BITS;
	constraints.max_segment_size = BRCMSTB_DMA_MAX_SEGMENT;
	constraints.segment_boundary = 0;
	constraints.coherent = 0;
	bus_offset = host->config.inbound_pci_base;
	physical_limit = (UINT64_C(1) << BRCMSTB_DMA_ADDRESS_BITS) - 1U;

	/* Restricts allocations to memory that the device tree permits DMA to reach. */
	if (physical_limit >= host->config.inbound_size)
		physical_limit = host->config.inbound_size - 1U;

	/* Accounts for the nonzero PCI alias in the device-visible address width. */
	if (bus_offset != 0)
		constraints.address_bits = 64U;

	/*
	 * Lets a BCM2712's devices reach all the memory its window covers, at
	 * any width: its window is not limited to 2 GiB.  The device tree says
	 * whether its accesses snoop the CPU caches.
	 */
	if (host->config.variant == DRV_PCI_BRCMSTB_BCM2712) {
		constraints.address_bits = BCM2712_DMA_ADDRESS_BITS;
		constraints.coherent = host->config.coherent;
		physical_limit = host->config.inbound_size - 1U;
	}

	/* Publishes the translated DMA space after both windows are programmed. */
	error = drv_dma_device_create_window(&constraints,
					     bus_offset,
					     physical_limit,
					     &host->dma);
	if (error != 0)
		return error;

	/* Takes the operations with message interrupts when the board receives them. */
	ops = &brcmstb_bus_ops;
	if (host->config.board != NULL && host->config.board->msi_establish != NULL)
		ops = &brcmstb_message_bus_ops;

	/* Creates bus 0 of the host's segment, whose only function is the root port. */
	error = drv_pci_bus_create_root(host->config.segment, BRCMSTB_ROOT_BUS, ops, host, host->dma, &host->root);
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

/*
 * Reads what a BCM2712 has beyond a BCM2711: every inbound window, the
 * highest link generation and the request queue's priorities.
 */
static int
describe_bcm2712(
	const struct drv_fdt *fdt,
	uint32_t node,
	struct drv_pci_brcmstb_config *config)
{
	const uint8_t *value;
	uint32_t length;
	unsigned level;
	int error;

	/* Marks the controller as the BCM2712 kind. */
	config->variant = DRV_PCI_BRCMSTB_BCM2712;

	/* Reads every window devices reach the system bus through. */
	error = describe_inbound_windows(fdt, node, config);
	if (error != 0)
		return error;

	/* Reads the highest generation the link may train to; zero leaves it alone. */
	config->max_link_speed = drv_fdt_node_cells(fdt, node, "max-link-speed", 0);

	/* Reads how long the reference clock runs before the endpoint leaves reset. */
	config->tperst_ms = drv_fdt_node_cells(fdt, node, "brcm,tperst-clk-ms", 0);

	/* Reads the priority of each quartile of the request queue, when the tree gives four. */
	config->qos_valid = 0;
	error = drv_fdt_property(fdt, node, "brcm,fifo-qos-map", &value, &length);
	if (error == 0 && length == DRV_PCI_BRCMSTB_QOS_LEVELS) {
		/* Copies the four one-byte priorities. */
		for (level = 0; level < DRV_PCI_BRCMSTB_QOS_LEVELS; level++)
			config->qos[level] = value[level];
		config->qos_valid = 1;
	}

	/* Succeeded: the BCM2712's own description is recorded. */
	return 0;
}

/*
 * Reads every entry of the node's dma-ranges as an inbound window.
 *
 * The entry that lands on CPU address 0 is system memory; the single
 * inbound fields name it as well, since DMA goes through it.  The
 * parent-bus address is kept as the CPU address: the buses above the
 * BCM2712's controllers pass it through unchanged.
 */
static int
describe_inbound_windows(
	const struct drv_fdt *fdt,
	uint32_t node,
	struct drv_pci_brcmstb_config *config)
{
	struct drv_pci_brcmstb_inbound *window;
	const uint8_t *value;
	uint32_t length;
	uint32_t parent;
	uint32_t parent_cells;
	uint32_t size_cells;
	uint32_t entry_cells;
	uint32_t entry;
	bool memory_found;
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
	if (parent_cells > 2U || size_cells > 2U)
		return ERANGE;
	entry_cells = FDT_PCI_ADDRESS_CELLS + parent_cells + size_cells;

	/* Refuses more windows than the controller has RC BARs for. */
	if (length / (entry_cells * 4U) > DRV_PCI_BRCMSTB_INBOUND_MAX)
		return ERANGE;

	/* Records each entry in order, and the first one onto system memory. */
	config->inbound_count = 0;
	memory_found = false;
	for (entry = 0; (entry + 1U) * entry_cells * 4U <= length; entry++) {
		/* Decodes the PCI address, the system-bus address and the size. */
		window = &config->inbound[entry];
		window->pci_base = drv_fdt_cells_value(value, entry * entry_cells + FDT_PCI_ADDRESS_LOW_CELL, 2U);
		window->cpu_base = drv_fdt_cells_value(value, entry * entry_cells + FDT_PCI_ADDRESS_CELLS, parent_cells);
		window->size = drv_fdt_cells_value(value, entry * entry_cells + FDT_PCI_ADDRESS_CELLS + parent_cells, size_cells);
		config->inbound_count++;

		/* Takes the first window onto CPU address 0 as the DMA window. */
		if (!memory_found && window->cpu_base == 0) {
			config->inbound_pci_base = window->pci_base;
			config->inbound_cpu_base = 0;
			config->inbound_size = window->size;
			memory_found = true;
		}
	}

	/* Refuses a tree whose windows do not reach system memory. */
	if (!memory_found)
		return ENOENT;

	/* Succeeded: every inbound window is recorded. */
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
	int error;

	/* Requires the whole register block: a BCM2711's up to its reset register. */
	if (config->variant == DRV_PCI_BRCMSTB_BCM2712) {
		/* A BCM2712's reaches the configuration index register. */
		if (config->register_size < BCM2712_REGISTER_SPAN)
			return EINVAL;
	} else if (config->register_size < BRCMSTB_REGISTER_SPAN) {
		/* Refuses a BCM2711 block that stops before its reset register. */
		return EINVAL;
	}

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

	/* Refuses an inbound alias whose last byte wraps around PCI address space. */
	if (config->inbound_pci_base > UINT64_MAX - (config->inbound_size - 1U))
		return EINVAL;

	/* Checks what only a BCM2712 has. */
	if (config->variant == DRV_PCI_BRCMSTB_BCM2712) {
		error = check_config_bcm2712(config);
		if (error != 0)
			return error;
	}

	/* Succeeded: the controller can be programmed with this configuration. */
	return 0;
}

/*
 * Refuses a BCM2712 configuration its registers or its board cannot carry
 * out.
 */
static int
check_config_bcm2712(
	const struct drv_pci_brcmstb_config *config)
{
	unsigned index;
	int error;

	/* Requires a board that resets the bridge, which the controller cannot do itself. */
	if (config->board == NULL)
		return EINVAL;
	if (config->board->bridge_reset == NULL)
		return EINVAL;

	/* Requires the two message operations together or neither. */
	if (config->board->msi_establish == NULL && config->board->msi_disestablish != NULL)
		return EINVAL;
	if (config->board->msi_establish != NULL && config->board->msi_disestablish == NULL)
		return EINVAL;

	/* Requires one to three inbound windows, one RC BAR each. */
	if (config->inbound_count == 0)
		return EINVAL;
	if (config->inbound_count > DRV_PCI_BRCMSTB_INBOUND_MAX)
		return EINVAL;

	/* Requires each window to be one an RC BAR and its remap express. */
	for (index = 0; index < config->inbound_count; index++) {
		error = check_inbound_window(&config->inbound[index]);
		if (error != 0)
			return error;
	}

	/* Refuses a generation PCIe does not have below 4. */
	if (config->max_link_speed > BCM2712_LINK_SPEED_MAX)
		return EINVAL;

	/* Refuses a PERST# lead time longer than a second. */
	if (config->tperst_ms > BCM2712_TPERST_MAX_MS)
		return EINVAL;

	/* Succeeded: the BCM2712 can be programmed with this configuration. */
	return 0;
}

/*
 * Refuses an inbound window an RC BAR cannot express: empty, larger than
 * 64 GiB, a PCI base not aligned to the power of two that holds it, a last
 * byte past the end of PCI address space, or a CPU base off a 4 KiB page.
 */
static int
check_inbound_window(
	const struct drv_pci_brcmstb_inbound *window)
{
	uint64_t rounded;
	unsigned log2;
	int error;

	/* Refuses an empty window. */
	if (window->size == 0)
		return EINVAL;

	/* Finds the power of two the RC BAR rounds the window up to. */
	error = inbound_size_log2(window->size, &log2);
	if (error != 0)
		return error;

	/* Refuses a window above the largest size code. */
	if (log2 > BCM2712_INBOUND_LOG2_MAX)
		return ERANGE;

	/* Refuses a PCI base the rounded window is not aligned to. */
	rounded = (uint64_t)1 << log2;
	if ((window->pci_base & (rounded - 1U)) != 0)
		return EINVAL;

	/* Refuses a window whose last byte wraps around PCI address space. */
	if (window->pci_base > UINT64_MAX - (window->size - 1U))
		return EINVAL;

	/* Refuses a CPU base the remap cannot hold: it keeps whole 4 KiB pages. */
	if ((window->cpu_base & BCM2712_UBUS_REMAP_PAGE) != 0)
		return EINVAL;

	/* Succeeded: the window can be programmed. */
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

	/* A BCM2712 has a sequence of its own. */
	if (host->config.variant == DRV_PCI_BRCMSTB_BCM2712) {
		error = bring_up_bcm2712(host);
		if (error != 0)
			return error;

		/* Succeeded: the tree below the BCM2712's root port is ready. */
		return 0;
	}

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

/*
 * Runs the BCM2712's start sequence on a mapped controller.
 *
 * The board resets the bridge, the endpoint is held in reset, the SerDes
 * powered, the system bus and the inbound windows opened, the PHY given
 * its reference clock and the bridge its error replies, timeouts and
 * priorities; then the link is trained, the reference clock driven
 * without power saving, the outbound window opened and the bus below the
 * root port assigned.
 *
 * Procedure referenced: Raspberry Pi Linux rpi-6.12.y
 * drivers/pci/controller/pcie-brcmstb.c (brcm_pcie_setup() with the
 * BCM2712's configuration, then brcm_pcie_start_link() and
 * brcm_config_clkreq()).
 */
static int
bring_up_bcm2712(
	struct drv_pci_brcmstb *host)
{
	uint32_t revision;
	int error;

	/* Resets the bridge, holds the endpoint in reset and powers the SerDes. */
	error = reset_bridge_bcm2712(host);
	if (error != 0)
		return error;

	/* Reads the revision now that the bridge and SerDes are awake. */
	revision = register_read(host, BRCMSTB_REVISION);
	kern_logf("pcie: brcmstb at %llx revision %x (BCM2712)\n", (unsigned long long)host->config.register_base, revision);

	/* Opens system-bus access, all-ones for absent functions, 512-byte bursts and the completion boundaries. */
	register_update(host,
			BRCMSTB_MISC_CTRL,
			BRCMSTB_MISC_CTRL_BURST,
			BRCMSTB_MISC_CTRL_SCB_ACCESS |
			BRCMSTB_MISC_CTRL_UR_MODE |
			BCM2712_MISC_CTRL_BURST_512 |
			BCM2712_MISC_CTRL_RCB_MPS |
			BCM2712_MISC_CTRL_RCB_64B);

	/* Opens the windows devices reach the system bus through. */
	error = set_inbound_windows_bcm2712(host);
	if (error != 0)
		return error;

	/* Takes inbound traffic as little-endian. */
	register_update(host, BRCMSTB_VENDOR_REG1, BRCMSTB_VENDOR_REG1_ENDIAN, 0);

	/* Masks and clears the controller's own interrupts; nothing here uses them. */
	register_write(host, BCM2712_INTR2_MASK_SET, 0xffffffffU);
	register_write(host, BCM2712_INTR2_CLEAR, 0xffffffffU);

	/* Gives the PHY its reference clock and the bridge its replies, timeouts and priorities. */
	error = tune_bcm2712(host);
	if (error != 0)
		return error;

	/* Keeps the link at or below the generation the device tree allows. */
	limit_link_speed(host);

	/* Releases the endpoint and waits for the link. */
	error = train_link(host);
	if (error != 0)
		return error;

	/* Drives the reference clock all the time, with no clock-request power saving. */
	set_clock_request_safe(host);

	/* Waits until the endpoint answers rather than asks for a retry. */
	wait_for_endpoint(host);

	/* Opens the outbound window and presents the root port as a bridge. */
	set_outbound_window(host);
	register_update(host, BRCMSTB_ID_VAL3, BRCMSTB_ID_VAL3_CLASS, BRCMSTB_CLASS_PCI_BRIDGE);
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
 * Has the board reset the BCM2712's bridge, then holds the endpoint in
 * reset and powers the SerDes.
 *
 * PERST# is asserted explicitly, so the endpoint stays in reset whatever
 * the firmware left in the PCIe control register.
 */
static int
reset_bridge_bcm2712(
	struct drv_pci_brcmstb *host)
{
	const struct drv_pci_brcmstb_board *board;
	int error;

	board = host->config.board;

	/* Holds the bridge in reset. */
	error = board->bridge_reset(board->context, 1);
	if (error != 0)
		return error;

	/* Keeps it there for the reset's minimum length. */
	kern_usleep_range(BRCMSTB_RESET_US, BRCMSTB_RESET_MAX_US);

	/* Releases the bridge; the board waits until it can be reached. */
	error = board->bridge_reset(board->context, 0);
	if (error != 0)
		return error;

	/* Marks the bridge's registers as reachable from here on. */
	host->bridge_ready = true;

	/* Holds the endpoint in reset until the link is trained. */
	hold_endpoint(host);

	/* Powers the SerDes and lets it settle. */
	register_update(host, BCM2712_HARD_DEBUG, BCM2712_HARD_DEBUG_SERDES_IDDQ, 0);
	kern_usleep_range(BRCMSTB_RESET_US, BRCMSTB_RESET_MAX_US);

	/* Succeeded: the bridge is out of reset and the endpoint in it. */
	return 0;
}

/*
 * Opens the BCM2712's inbound windows, one RC BAR each from BAR1, in the
 * device tree's order.
 *
 * An RC BAR holds the window's PCI base with its size as a power-of-two
 * code; its remap holds the system-bus address the window lands on.  The
 * RC BARs after the last window are left as the bridge's reset left them.
 *
 * Procedure referenced: Raspberry Pi Linux rpi-6.12.y
 * drivers/pci/controller/pcie-brcmstb.c (brcm_pcie_get_inbound_wins() and
 * set_inbound_win_registers() for the BCM7712 kind: one RC BAR from each
 * dma-ranges entry, its remap with the access bit; and
 * brcm_pcie_encode_ibar_size()).
 */
static int
set_inbound_windows_bcm2712(
	struct drv_pci_brcmstb *host)
{
	const struct drv_pci_brcmstb_inbound *window;
	uint32_t size_code;
	uint32_t bar_low;
	uint32_t remap_low;
	unsigned offset;
	unsigned index;
	unsigned log2;
	int error;

	/* Opens each window in turn. */
	for (index = 0; index < host->config.inbound_count; index++) {
		/* Finds the power of two the window is rounded up to. */
		window = &host->config.inbound[index];
		error = inbound_size_log2(window->size, &log2);
		if (error != 0)
			return error;

		/* The size code is log2 - 12 + 0x1c below 64 KiB and log2 - 15 above. */
		if (log2 < 16U)
			size_code = log2 - 12U + 0x1cU;
		else
			size_code = log2 - 15U;

		/* Composes the RC BAR's low word and the remap's low word. */
		bar_low = ((uint32_t)window->pci_base & ~BRCMSTB_RC_BAR_SIZE) | size_code;
		remap_low = ((uint32_t)window->cpu_base & ~BCM2712_UBUS_REMAP_PAGE) | BCM2712_UBUS_REMAP_ENABLE;

		/* Writes the window's RC BAR and its remap. */
		offset = index * BCM2712_INBOUND_STRIDE;
		register_write(host, BCM2712_RC_BAR_LO + offset, bar_low);
		register_write(host, BCM2712_RC_BAR_HI + offset, (uint32_t)(window->pci_base >> 32));
		register_write(host, BCM2712_UBUS_REMAP_LO + offset, remap_low);
		register_write(host, BCM2712_UBUS_REMAP_HI + offset, (uint32_t)(window->cpu_base >> 32));

		/* Exposes the window for diagnosis on the board. */
		kern_logf("pcie: brcmstb inbound %u PCI %llx CPU %llx size %llx\n",
			  index + 1U,
			  (unsigned long long)window->pci_base,
			  (unsigned long long)window->cpu_base,
			  (unsigned long long)window->size);
	}

	/* Succeeded: devices can reach the system bus once the link is up. */
	return 0;
}

/*
 * Gives the BCM2712's PHY its reference clock and its system-bus bridge
 * its error replies, timeouts and priorities.
 *
 * A read that fails on the far side of the bridge then returns all-ones
 * instead of an error the CPU would take as an external abort.
 *
 * Procedure referenced: Raspberry Pi Linux rpi-6.12.y
 * drivers/pci/controller/pcie-brcmstb.c (brcm_pcie_post_setup_bcm2712(),
 * the branch for brcm,fifo-qos-map; the one for brcm,vdm-qos-map is not
 * taken).
 */
static int
tune_bcm2712(
	struct drv_pci_brcmstb *host)
{
	uint32_t interface;
	unsigned index;
	int error;

	/* Selects the PHY block that holds the reference clock's settings. */
	error = mdio_write(host, BCM2712_MDIO_PORT, BCM2712_MDIO_SELECT_BLOCK, BCM2712_MDIO_REFCLK_BLOCK);
	if (error != 0)
		return error;

	/* Takes the 54 MHz crystal as the reference clock, one PHY register at a time. */
	for (index = 0; index < BCM2712_MDIO_REFCLK_WRITES; index++) {
		error = mdio_write(host, BCM2712_MDIO_PORT, bcm2712_refclk_registers[index], bcm2712_refclk_data[index]);
		if (error != 0)
			return error;
	}

	/* Lets the PHY settle on the new clock. */
	kern_usleep_range(BRCMSTB_RESET_US, BRCMSTB_RESET_MAX_US);

	/* Tells the PHY's power management the period of that clock, 18.52 ns. */
	register_update(host, BCM2712_PHY_CTL_15, BCM2712_PHY_CTL_15_PM_PERIOD, BCM2712_PHY_PM_PERIOD_54MHZ);

	/* Makes a failed read of the far side return all-ones instead of an error reply. */
	register_update(host, BCM2712_UBUS_CTRL, 0, BCM2712_UBUS_CTRL_REPLY_ERR_OFF | BCM2712_UBUS_CTRL_DECERR_OFF);
	register_write(host, BCM2712_AXI_READ_ERROR_DATA, 0xffffffffU);

	/* Waits about 250 ms on the bridge and 240 ms on a retried configuration request. */
	register_write(host, BCM2712_UBUS_TIMEOUT, BCM2712_UBUS_TIMEOUT_250MS);
	register_write(host, BCM2712_CONFIG_RETRY_TIMEOUT, BCM2712_CONFIG_RETRY_240MS);

	/* Turns off the broken QoS forwarding and turns on the interface's QoS fixes. */
	register_update(host,
			BCM2712_AXI_INTF_CTRL,
			BCM2712_AXI_QOS_PROPAGATION,
			BCM2712_AXI_RCLK_QOS_FIX |
			BCM2712_AXI_QOS_TIMING_FIX |
			BCM2712_AXI_NO_QOS_GATING);

	/* A chip where the timing fix does not stick limits its outstanding requests instead. */
	interface = register_read(host, BCM2712_AXI_INTF_CTRL);
	if ((interface & BCM2712_AXI_QOS_TIMING_FIX) == 0)
		register_update(host, BCM2712_AXI_INTF_CTRL, BCM2712_AXI_OUTSTANDING_MASK, BCM2712_AXI_OUTSTANDING_LIMIT);

	/* Ignores the priorities vendor messages would set. */
	register_update(host, BCM2712_MISC_CTRL_1, BCM2712_MISC_CTRL_1_VDM_QOS, 0);

	/* Gives each fill level of the request queue the device tree's priority. */
	set_queue_priorities(host);

	/* Succeeded: the PHY and the bridge are tuned. */
	return 0;
}

/*
 * Writes one PHY register through the BCM2712's MDIO port and waits until
 * the write is done.
 *
 * The address packet names the port and the register; the data goes with
 * its busy bit set, and the port clears the bit when the write is done.
 * Reports ETIMEDOUT for a write that does not finish within about 100 us.
 */
static int
mdio_write(
	struct drv_pci_brcmstb *host,
	unsigned port,
	unsigned reg,
	uint16_t data)
{
	uint32_t packet;
	uint32_t status;
	unsigned poll;
	bool done;

	/* Composes the address packet: port, extended port, register and the write command. */
	packet = ((uint32_t)port << BCM2712_MDIO_PORT_SHIFT) & BCM2712_MDIO_PORT_MASK;
	packet |= ((uint32_t)(port >> 4) << BCM2712_MDIO_PORT_EXT_SHIFT) & BCM2712_MDIO_PORT_EXT_MASK;
	packet |= (uint32_t)reg & BCM2712_MDIO_REGISTER_MASK;
	packet |= BCM2712_MDIO_COMMAND_WRITE;

	/* Sends the address and reads it back, so it reaches the port before the data. */
	register_write(host, BCM2712_MDIO_ADDRESS, packet);
	(void)register_read(host, BCM2712_MDIO_ADDRESS);

	/* Sends the data with the busy bit, which starts the write. */
	register_write(host, BCM2712_MDIO_WRITE_DATA, BCM2712_MDIO_BUSY | data);

	/* Waits for the port to clear the busy bit. */
	done = false;
	for (poll = 0; poll < BCM2712_MDIO_POLLS; poll++) {
		/* Stops as soon as the write is done. */
		status = register_read(host, BCM2712_MDIO_WRITE_DATA);
		if ((status & BCM2712_MDIO_BUSY) == 0) {
			done = true;
			break;
		}

		/* Waits one polling interval. */
		kern_usleep_range(BCM2712_MDIO_POLL_US, BCM2712_MDIO_POLL_MAX_US);
	}

	/* Refuses to go on with a PHY that did not take the write. */
	if (!done) {
		kern_logf("pcie: brcmstb PHY register %x write did not finish\n", reg);
		return ETIMEDOUT;
	}

	/* Succeeded: the PHY register holds the data. */
	return 0;
}

/*
 * Gives each quarter of the request queue's fill level its priority on the
 * system bus, the same for every traffic class.  A device tree without the
 * map leaves the bridge's own priorities.
 */
static void
set_queue_priorities(
	struct drv_pci_brcmstb *host)
{
	uint32_t map;
	unsigned level;
	unsigned index;

	/* Leaves the priorities alone without a map. */
	if (!host->config.qos_valid)
		return;

	/* Packs the four priorities, four bits each, from the emptiest quarter up. */
	map = 0;
	for (level = 0; level < DRV_PCI_BRCMSTB_QOS_LEVELS; level++)
		map |= ((uint32_t)host->config.qos[level] & BCM2712_QOS_LEVEL_MASK) << (level * BCM2712_QOS_LEVEL_BITS);

	/* Gives every traffic class the same map; their registers go downwards. */
	for (index = 0; index < BCM2712_QOS_MAP_COUNT; index++)
		register_write(host, BCM2712_QOS_MAP_FIRST - index * BCM2712_QOS_MAP_STRIDE, map);
}

/*
 * Keeps the BCM2712's link at or below the generation the device tree
 * allows, in the root port's advertised capability and in its target.
 *
 * Procedure referenced: Raspberry Pi Linux rpi-6.12.y
 * drivers/pci/controller/pcie-brcmstb.c (brcm_pcie_set_gen()).
 */
static void
limit_link_speed(
	struct drv_pci_brcmstb *host)
{
	uint16_t control;
	unsigned speed;

	/* Leaves the controller's own generation when the tree names none. */
	speed = host->config.max_link_speed;
	if (speed == 0)
		return;

	/* Advertises no generation above it. */
	register_update(host, BCM2712_LINK_CAPABILITY, BCM2712_LINK_SPEED_MASK, speed);

	/* Aims the link at no generation above it. */
	control = kern_mmio_read16(host->registers + BCM2712_RC_LINK_CONTROL2);
	control = (uint16_t)((control & ~BCM2712_LINK_SPEED_MASK) | speed);
	kern_mmio_write16(host->registers + BCM2712_RC_LINK_CONTROL2, control);
}

/*
 * Drives the BCM2712's reference clock all the time and stops offering L1
 * substates, which need clock requests: the "safe" clock-request mode.
 *
 * The kernel never turns on link power management, so this loses nothing
 * it uses, and it does not depend on the board wiring CLKREQ#.
 *
 * Procedure referenced: Raspberry Pi Linux rpi-6.12.y
 * drivers/pci/controller/pcie-brcmstb.c (brcm_config_clkreq(), its "safe"
 * branch).  That branch writes the debug register whole from a value it
 * never reads; here only the clock-request bits change and the SerDes
 * and PERST# bits are kept.
 */
static void
set_clock_request_safe(
	struct drv_pci_brcmstb *host)
{
	/* Overrides the reference clock on and stops listening to CLKREQ#. */
	register_update(host,
			BCM2712_HARD_DEBUG,
			BCM2712_HARD_DEBUG_CLKREQ |
			BCM2712_HARD_DEBUG_REFCLK_ENABLE |
			BCM2712_HARD_DEBUG_REFCLK_OUT |
			BCM2712_HARD_DEBUG_L1SS,
			BCM2712_HARD_DEBUG_REFCLK_OUT |
			BCM2712_HARD_DEBUG_REFCLK_ENABLE);

	/* Stops advertising L1 substates. */
	register_update(host, BCM2712_ROOT_CAP, BCM2712_ROOT_CAP_L1SS_MODE, BCM2712_ROOT_CAP_L1SS_OFF);
}

/*
 * Leaves a BCM2712 whose start failed with its SerDes off and its bridge
 * in reset, as the board found it before the start.
 *
 * Procedure referenced: Raspberry Pi Linux rpi-6.12.y
 * drivers/pci/controller/pcie-brcmstb.c (brcm_pcie_turn_off(): the SerDes
 * powered down, then the bridge reset asserted).
 */
static void
stop_bcm2712(
	struct drv_pci_brcmstb *host)
{
	const struct drv_pci_brcmstb_board *board;

	/* Leaves a BCM2711, and a bridge that never left reset, alone. */
	if (host->config.variant != DRV_PCI_BRCMSTB_BCM2712)
		return;
	if (!host->bridge_ready)
		return;

	/* Powers the SerDes down. */
	register_update(host, BCM2712_HARD_DEBUG, 0, BCM2712_HARD_DEBUG_SERDES_IDDQ);

	/* Puts the bridge back into reset; its registers are out of reach from here. */
	board = host->config.board;
	(void)board->bridge_reset(board->context, 1);
	host->bridge_ready = false;
}

/*
 * Waits up to a second after link-up until the endpoint answers its
 * vendor number, rather than all-ones or the retry value.
 *
 * An endpoint may ask for configuration requests to be retried for a
 * second after its reset, longer than the bridge's own retry timeout;
 * an endpoint that still does not answer is left to the assignment,
 * which skips it as absent.
 */
static void
wait_for_endpoint(
	struct drv_pci_brcmstb *host)
{
	struct drv_pci_address address;
	uint32_t identity;
	unsigned vendor;
	unsigned poll;

	/* Addresses function 0 of the device below the root port. */
	kern_memset(&address, 0, sizeof(address));
	address.segment = host->config.segment;
	address.bus = BRCMSTB_SECONDARY_BUS;

	/* Reads the vendor number until it is one, up to the bound. */
	vendor = PCI_VENDOR_ABSENT;
	for (poll = 0; poll < BRCMSTB_ENDPOINT_POLLS; poll++) {
		identity = 0xffffffffU;
		(void)host_config_read(host, &address, PCI_VENDOR_ID, 4U, &identity);
		vendor = identity & 0xffffU;

		/* Stops at a vendor number that is neither absent nor a retry. */
		if (vendor != PCI_VENDOR_ABSENT && vendor != PCI_VENDOR_RETRY)
			break;

		/* Waits one step. */
		kern_usleep_range(BRCMSTB_ENDPOINT_POLL_US, BRCMSTB_ENDPOINT_POLL_MAX_US);
	}

	/* Records an endpoint that took its time, or never answered. */
	if (poll != 0)
		kern_logf("pcie: brcmstb endpoint answered after %u ms (vendor %04x)\n", poll * 10U, vendor);
}

/*
 * Holds the endpoint in reset (PERST# asserted).
 *
 * The BCM2711 does it in its reset register, which is always reachable.
 * The BCM2712 does it in its PCIe control register, which is written only
 * once the bridge is out of reset.
 */
static void
hold_endpoint(
	struct drv_pci_brcmstb *host)
{
	/* A BCM2712 clears its release bit, once its registers can be reached. */
	if (host->config.variant == DRV_PCI_BRCMSTB_BCM2712) {
		/* Leaves a bridge that is still in reset alone. */
		if (!host->bridge_ready)
			return;

		/* Asserts PERST#. */
		register_update(host, BCM2712_PCIE_CTRL, BCM2712_PCIE_CTRL_PERSTB, 0);
		return;
	}

	/* A BCM2711 sets its PERST# bit. */
	register_update(host, BRCMSTB_SW_INIT, 0, BRCMSTB_SW_INIT_PERST);
}

/*
 * Releases the endpoint from reset (PERST# deasserted).
 *
 * The BCM2712 first stops listening to CLKREQ#, so the endpoint's clock
 * request cannot gate the reference clock while the link trains.
 *
 * Procedure referenced: Raspberry Pi Linux rpi-6.12.y
 * drivers/pci/controller/pcie-brcmstb.c (brcm_pcie_start_link(): the
 * clock-request bits cleared, then PERST# released, held at the
 * connector for brcm,tperst-clk-ms first when the tree gives it).
 */
static void
release_endpoint(
	struct drv_pci_brcmstb *host)
{
	/* A BCM2712 clears the clock-request bits and sets its release bit. */
	if (host->config.variant == DRV_PCI_BRCMSTB_BCM2712) {
		/* Stops listening to CLKREQ# while the link trains. */
		register_update(host,
				BCM2712_HARD_DEBUG,
				BCM2712_HARD_DEBUG_CLKREQ |
				BCM2712_HARD_DEBUG_REFCLK_ENABLE |
				BCM2712_HARD_DEBUG_REFCLK_OUT |
				BCM2712_HARD_DEBUG_L1SS,
				0);

		/* Deasserts PERST# at once without a reference clock lead time. */
		if (host->config.tperst_ms == 0) {
			register_update(host, BCM2712_PCIE_CTRL, 0, BCM2712_PCIE_CTRL_PERSTB);
			return;
		}

		/*
		 * Starts the reference clock with PERST# still held at the
		 * connector, waits the lead time, then lets PERST# go.
		 */
		register_update(host, BCM2712_HARD_DEBUG, 0, BCM2712_HARD_DEBUG_PERST_ASSERT);
		register_update(host, BCM2712_PCIE_CTRL, 0, BCM2712_PCIE_CTRL_PERSTB);
		kern_usleep_range(host->config.tperst_ms * 1000U, host->config.tperst_ms * 1000U + 1000U);
		register_update(host, BCM2712_HARD_DEBUG, BCM2712_HARD_DEBUG_PERST_ASSERT, 0);
		return;
	}

	/* A BCM2711 clears its PERST# bit. */
	register_update(host, BRCMSTB_SW_INIT, BRCMSTB_SW_INIT_PERST, 0);
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

	/* Exposes the firmware-selected DMA alias for physical diagnosis. */
	kern_logf("pcie: brcmstb DMA PCI %llx CPU %llx size %llx window %llx\n",
		  (unsigned long long)host->config.inbound_pci_base,
		  (unsigned long long)host->config.inbound_cpu_base,
		  (unsigned long long)host->config.inbound_size,
		  (unsigned long long)window);

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
	release_endpoint(host);

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
		report_link_down(host);
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

/*
 * Reports a link that did not come up, with what the controller saw.
 *
 * A BCM2712 also gives its PCIe control and debug registers and its record
 * of link training, which tells an absent or unpowered device (training
 * never leaves detection) from one that answers but does not train.
 */
static void
report_link_down(
	struct drv_pci_brcmstb *host)
{
	uint32_t status;
	uint32_t control;
	uint32_t debug;
	uint32_t history[BCM2712_LTSSM_HISTORY_COUNT];
	unsigned index;

	/* Reads the link state every controller has. */
	status = register_read(host, BRCMSTB_STATUS);

	/* A BCM2711 has nothing more to give. */
	if (host->config.variant != DRV_PCI_BRCMSTB_BCM2712) {
		kern_logf("pcie: brcmstb link down (status %x)\n", status);
		return;
	}

	/* Reads PERST# and the clock bits, then the training record. */
	control = register_read(host, BCM2712_PCIE_CTRL);
	debug = register_read(host, BCM2712_HARD_DEBUG);
	for (index = 0; index < BCM2712_LTSSM_HISTORY_COUNT; index++)
		history[index] = register_read(host, BCM2712_LTSSM_HISTORY + index * 4U);

	/* Logs them together, so one line shows how far training went. */
	kern_logf("pcie: brcmstb link down (status %x, control %x, debug %x, LTSSM %08x %08x %08x %08x)\n",
		  status,
		  control,
		  debug,
		  history[0],
		  history[1],
		  history[2],
		  history[3]);
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
	bool up;

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

	/*
	 * A BCM2712 also checks the link at each access: a request sent down
	 * a link that has gone away would end in an abort.
	 *
	 * Procedure referenced: Raspberry Pi Linux rpi-6.12.y
	 * drivers/pci/controller/pcie-brcmstb.c (brcm_pcie_map_bus(): the
	 * link checked before every access below the root port).
	 */
	if (host->config.variant == DRV_PCI_BRCMSTB_BCM2712) {
		up = link_is_up(host);
		if (!up)
			return ENODEV;
	}

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
	struct drv_pci_brcmstb *host,
	const struct drv_pci_address *address,
	unsigned offset,
	unsigned width)
{
	/* Requires a function address the bus can carry, in the host's segment. */
	if (address == NULL)
		return false;
	if (address->segment != host->config.segment)
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
	valid = config_access_valid(host, address, offset, width);
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
	valid = config_access_valid(host, address, offset, width);
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
	address.segment = host->config.segment;
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
	address.segment = host->config.segment;
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
 * Gives a device its message or its legacy interrupt.
 *
 * A message is offered only when the board receives them (the BCM2712's
 * MIP); the BCM2711's own MSI receiver is not used.  The device's pin is
 * swizzled across each bridge between it and the root port, then mapped
 * through the device tree's interrupt map.
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

	/* Offers one message of either kind through the board. */
	if (type == DRV_PCI_IRQ_MSI || type == DRV_PCI_IRQ_MSIX) {
		error = allocate_message(host, device, type, irqs, count);
		if (error != 0)
			return error;

		/* Succeeded: the PCI core establishes the message through the board. */
		return 0;
	}

	/* Offers nothing else but the legacy pins. */
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

/*
 * Describes one message of a function for the PCI core, which then
 * establishes it through the board.
 *
 * ENOTSUP sends the core on to the next kind: the board receives no
 * messages, or the function does not have the capability asked for.
 */
static int
allocate_message(
	struct drv_pci_brcmstb *host,
	struct drv_pci_device *device,
	enum drv_pci_irq_type type,
	struct drv_pci_irq *irqs,
	unsigned *count)
{
	const struct drv_pci_brcmstb_board *board;
	unsigned capability;
	uint16_t control;
	uint8_t identity;
	int error;

	/* Refuses messages a board does not receive. */
	board = host->config.board;
	if (board == NULL || board->msi_establish == NULL)
		return ENOTSUP;

	/* Finds the function's capability for the kind asked for. */
	identity = PCI_CAPABILITY_MSI;
	if (type == DRV_PCI_IRQ_MSIX)
		identity = PCI_CAPABILITY_MSIX;
	error = drv_pci_device_find_capability(device, identity, &capability);
	if (error != 0)
		return ENOTSUP;

	/*
	 * Refuses MSI that cannot carry the board's address above 4 GiB, so
	 * the core falls back to the legacy pins now rather than failing the
	 * function's attach later.
	 */
	if (type == DRV_PCI_IRQ_MSI) {
		error = drv_pci_device_config_read16(device, capability + PCI_MSI_CONTROL, &control);
		if (error != 0)
			return ENOTSUP;
		if ((control & PCI_MSI_CONTROL_64BIT) == 0)
			return ENOTSUP;
	}

	/* Succeeded: one message, whose capability the core programs. */
	irqs[0].type = type;
	irqs[0].index = 0;
	irqs[0].vector = 0;
	irqs[0].private_data[0] = capability;
	irqs[0].private_data[1] = 0;
	*count = 1U;
	return 0;
}

/*
 * Takes one of the board's messages for a function, for the PCI core.
 *
 * The board picks the message and reports what the function writes, and
 * where, to send it.
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
	const struct drv_pci_brcmstb_board *board;
	struct drv_pci_brcmstb *host;
	int error;

	(void)device;

	/* Refuses a host whose board receives no messages. */
	host = context;
	board = host->config.board;
	if (board == NULL || board->msi_establish == NULL)
		return ENOTSUP;

	/* Asks the board for a message. */
	error = board->msi_establish(board->context, run, argument, handle, address, data);
	if (error != 0)
		return error;

	/* Succeeded: the function's message reaches run through the board. */
	return 0;
}

/*
 * Gives one of the board's messages back, for the PCI core.
 */
static int
host_msi_disestablish(
	void *context,
	unsigned handle)
{
	const struct drv_pci_brcmstb_board *board;
	struct drv_pci_brcmstb *host;
	int error;

	/* Refuses a host whose board receives no messages. */
	host = context;
	board = host->config.board;
	if (board == NULL || board->msi_disestablish == NULL)
		return ENOTSUP;

	/* Has the board free the message and wait for its handler. */
	error = board->msi_disestablish(board->context, handle);
	if (error != 0)
		return error;

	/* Succeeded: the message is free and no handler runs it. */
	return 0;
}
