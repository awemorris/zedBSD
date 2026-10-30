/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The Realtek RTL8125 family of PCI Express Ethernet controllers.
 *
 * The RTL8125 (2.5 Gbit/s), RTL8126 (5 Gbit/s) and RTL8127 (10 Gbit/s)
 * share one register layout: a memory BAR of byte, word and longword
 * registers, and behind two of them the windows through which the MAC's
 * and the PHY's internal registers (the OCP space) are read and written.
 * The chip's version is read from the transmit configuration register and
 * picks the per-chip steps.
 *
 * At attach the controller is taken out of the firmware's out-of-band
 * mode, reset with its interrupts masked, and published as a network
 * interface under the station address the chip holds.  The interface has
 * no carrier until the PHY has been brought up and the rings started.
 */

#include <drivers/ethernet/rtl8125.h>

#include <drivers/generic/dma.h>
#include <drivers/pci/pci.h>
#include <kern/clock.h>
#include <kern/device-io.h>
#include <kern/kcrt.h>
#include <kern/klog.h>
#include <kern/kmem.h>
#include <kern/lock.h>
#include <kern/net/net-device.h>
#include <kern/net/packet-buf.h>
#include <kern/sched.h>
#include <kern/thread.h>
#include <uapi/errno.h>

#include <stdbool.h>
#include <stdint.h>

/*
 * The PCI identities of the controllers.
 *
 * The RTL8127's was read from the device; the others follow the same
 * numbering of the parts.
 */
#define RTL_PCI_VENDOR_REALTEK		0x10ecU
#define RTL_PCI_DEVICE_8125		0x8125U
#define RTL_PCI_DEVICE_8126		0x8126U
#define RTL_PCI_DEVICE_8127		0x8127U

/* The memory BAR that holds the registers, and the smallest one the driver takes. */
#define RTL_REGISTER_BAR		2U
#define RTL_REGISTER_BAR_MINIMUM	0x10000U

/*
 * The controller's registers.
 *
 * Values referenced: OpenBSD sys/dev/pci/if_rgereg.h 1.20 (RGE_MAC0,
 * RGE_MAC4, RGE_CMD, RGE_IMR, RGE_ISR, RGE_TXCFG, RGE_RXCFG, RGE_EECMD,
 * RGE_PHYSTAT, RGE_MACOCP, RGE_PHYOCP, RGE_TWICMD, RGE_MCUCMD, RGE_IM,
 * RGE_PPSW, RGE_ADDR0, RGE_ADDR1, RGE_INT_CFG0, RGE_TIMERINT0..3, RGE_CFG2,
 * RGE_CFG5, RGE_CSIDR, RGE_CSIAR, RGE_PMCH, RGE_EPHYAR; 0x00f1 from
 * rge_hw_init).
 */
#define RTL_REG_STATION0		0x0000U
#define RTL_REG_STATION4		0x0004U
#define RTL_REG_COMMAND			0x0037U
#define RTL_REG_INTERRUPT_MASK		0x0038U
#define RTL_REG_INTERRUPT_STATUS	0x003cU
#define RTL_REG_TX_CONFIG		0x0040U
#define RTL_REG_RX_CONFIG		0x0044U
#define RTL_REG_CONFIG_LOCK		0x0050U
#define RTL_REG_PHY_STATUS		0x006cU
#define RTL_REG_INTERRUPT_CONFIG0	0x0034U
#define RTL_REG_TIMER_INTERRUPT0	0x0058U
#define RTL_REG_TIMER_INTERRUPT1	0x005cU
#define RTL_REG_TIMER_INTERRUPT2	0x008cU
#define RTL_REG_TIMER_INTERRUPT3	0x00f4U
#define RTL_REG_CONFIG2			0x0053U
#define RTL_REG_CONFIG5			0x0056U
#define RTL_REG_CSI_DATA		0x0064U
#define RTL_REG_CSI_ADDRESS		0x0068U
#define RTL_REG_POWER_CONTROL		0x006fU
#define RTL_REG_EPHY			0x0080U
#define RTL_REG_MISC_F1			0x00f1U
#define RTL_REG_MAC_OCP			0x00b0U
#define RTL_REG_PHY_OCP			0x00b8U
#define RTL_REG_TWI_COMMAND		0x00d2U
#define RTL_REG_MCU_COMMAND		0x00d3U
#define RTL_REG_IDLE_STATUS		0x00e2U
#define RTL_REG_POWER_SWITCH		0x00f2U
#define RTL_REG_PERMANENT0		0x19e0U
#define RTL_REG_PERMANENT4		0x19e4U

/*
 * The registers the rings, the receive filter and the interrupt set-up
 * use.  The interrupt moderation register is the one whose low bits the
 * reset reads as the DMA's idle flags.
 *
 * Values referenced: FreeBSD sys/dev/rge/if_rgereg.h, main
 * 8fa55fdcae4ecbd2ec13ef91cc9d172095e21189 (RGE_MAR0, RGE_MAR4,
 * RGE_TXDESC_ADDR_LO, RGE_TXDESC_ADDR_HI, RGE_CFG1, RGE_TDFNR,
 * RGE_TIMERCNT, RGE_INT_CFG1, RGE_TXSTART, RGE_DLLPR, RGE_RXMAXSIZE,
 * RGE_CPLUSCMD, RGE_IM, RGE_RXDESC_ADDR_LO, RGE_RXDESC_ADDR_HI,
 * RGE_RADMFIFO_PROTECT, RGE_INTMITI, RGE_RSS_CTRL, RGE_RXQUEUE_CTRL,
 * RGE_EEE_TXIDLE_TIMER); sys/dev/rge/if_rge.c, the same commit
 * (rge_init_locked: 0x0382, 0x1880, 0x00d8, 0x20e4).
 */
#define RTL_REG_MULTICAST0		0x0008U
#define RTL_REG_MULTICAST4		0x000cU
#define RTL_REG_TX_RING_LOW		0x0020U
#define RTL_REG_TX_RING_HIGH		0x0024U
#define RTL_REG_TIMER_COUNT		0x0048U
#define RTL_REG_CONFIG1			0x0052U
#define RTL_REG_TX_FIRST_NUMBER		0x0057U
#define RTL_REG_INTERRUPT_CONFIG1	0x007aU
#define RTL_REG_TX_START		0x0090U
#define RTL_REG_DLL_POWER		0x00d0U
#define RTL_REG_DESCRIPTOR_MODE		0x00d8U
#define RTL_REG_RX_MAX_SIZE		0x00daU
#define RTL_REG_CPLUS_COMMAND		0x00e0U
#define RTL_REG_INTERRUPT_MODERATION	0x00e2U
#define RTL_REG_RX_RING_LOW		0x00e4U
#define RTL_REG_RX_RING_HIGH		0x00e8U
#define RTL_REG_LTR_8125		0x0382U
#define RTL_REG_RX_FIFO_PROTECT		0x0402U
#define RTL_REG_MITIGATION_FIRST	0x0a00U
#define RTL_REG_RX_LINK_8127		0x1880U
#define RTL_REG_DESCRIPTOR_8127		0x20e4U
#define RTL_REG_RSS_CONTROL		0x4500U
#define RTL_REG_RX_QUEUE_CONTROL	0x4800U
#define RTL_REG_EEE_TX_IDLE		0x6048U

/*
 * The fields of those registers, and the transmit and receive
 * configurations each member starts from.
 *
 * Values referenced: FreeBSD sys/dev/rge/if_rgereg.h, main
 * 8fa55fdcae4ecbd2ec13ef91cc9d172095e21189 (RGE_ISR_*, RGE_INTRS,
 * RGE_RXCFG_VLANSTRIP, RGE_CFG1_SPEED_DOWN, RGE_INT_CFG0_*,
 * RGE_TXSTART_START, RGE_DLLPR_PFM_EN, RGE_DLLPR_TX_10M_PS_EN,
 * RGE_CPLUSCMD_RXCSUM, RGE_TXCFG_CONFIG, RGE_RXCFG_CONFIG,
 * RGE_RXCFG_CONFIG_8125B, RGE_RXCFG_CONFIG_8125D, RGE_RXCFG_CONFIG_8126,
 * RGE_JUMBO_FRAMELEN, RGE_JUMBO_MTU); OpenBSD sys/dev/pci/if_rgereg.h
 * 1.20 (RGE_INT_CFG0_AUTO_CLEAR_IMR).
 */
#define RTL_INTERRUPT_RX_OK		0x00000001U
#define RTL_INTERRUPT_RX_ERROR		0x00000002U
#define RTL_INTERRUPT_TX_OK		0x00000004U
#define RTL_INTERRUPT_TX_ERROR		0x00000008U
#define RTL_INTERRUPT_LINK_CHANGE	0x00000020U
#define RTL_INTERRUPT_TX_UNAVAILABLE	0x00000080U
#define RTL_INTERRUPT_PCS_TIMEOUT	0x00004000U
#define RTL_INTERRUPT_SYSTEM_ERROR	0x00008000U
#define RTL_INTERRUPTS_RUNNING		(RTL_INTERRUPT_RX_OK | RTL_INTERRUPT_RX_ERROR | \
					 RTL_INTERRUPT_TX_ERROR | \
					 RTL_INTERRUPT_LINK_CHANGE | \
					 RTL_INTERRUPT_PCS_TIMEOUT | RTL_INTERRUPT_SYSTEM_ERROR)
#define RTL_RX_CONFIG_ALL_PHYSICAL	0x00000001U
#define RTL_RX_CONFIG_OWN_ADDRESS	0x00000002U
#define RTL_RX_CONFIG_MULTICAST		0x00000004U
#define RTL_RX_CONFIG_BROADCAST		0x00000008U
#define RTL_RX_CONFIG_VLAN_STRIP	0x00c00000U
#define RTL_RX_CONFIG_8125		0x41000700U
#define RTL_RX_CONFIG_8125B		0x41000c00U
#define RTL_RX_CONFIG_8125D		0x41200c00U
#define RTL_RX_CONFIG_8126		0x41200d00U
#define RTL_TX_CONFIG_START		0x03000700U
#define RTL_CONFIG1_SPEED_DOWN		0x10U
#define RTL_INTERRUPT_CONFIG0_ENABLE	0x01U
#define RTL_INTERRUPT_CONFIG0_TIMEOUT_BYPASS	0x02U
#define RTL_INTERRUPT_CONFIG0_MITIGATION_BYPASS	0x04U
#define RTL_INTERRUPT_CONFIG0_RDU_BYPASS	0x10U
#define RTL_INTERRUPT_CONFIG0_AUTO_CLEAR	0x20U
#define RTL_INTERRUPT_CONFIG0_AVOID_MISS	0x40U
#define RTL_TX_START_NORMAL		0x0001U
#define RTL_DLL_POWER_BITS		0xc0U
#define RTL_CPLUS_RX_CHECKSUM		0x0020U
#define RTL_EEE_TX_IDLE_VALUE		(9216U - 14U - 4U - 4U + 14U + 32U)

/*
 * The fields of those registers.
 *
 * Values referenced: OpenBSD sys/dev/pci/if_rgereg.h 1.20 (RGE_CMD_TXENB,
 * RGE_CMD_RXENB, RGE_CMD_RESET, RGE_CMD_STOPREQ, RGE_TXCFG_HWREV,
 * RGE_RXCFG_ALLPHYS, RGE_RXCFG_INDIV, RGE_RXCFG_MULTI, RGE_RXCFG_BROAD,
 * RGE_RXCFG_RUNT, RGE_RXCFG_ERRPKT, RGE_EECMD_WRITECFG,
 * RGE_MACOCP_DATA_MASK, RGE_MACOCP_BUSY, RGE_MACOCP_ADDR_SHIFT,
 * RGE_PHYOCP_DATA_MASK, RGE_PHYOCP_BUSY, RGE_PHYOCP_ADDR_SHIFT,
 * RGE_MCUCMD_RXFIFO_EMPTY, RGE_MCUCMD_TXFIFO_EMPTY, RGE_MCUCMD_IS_OOB).
 */
#define RTL_COMMAND_TX_ENABLE		0x04U
#define RTL_COMMAND_RX_ENABLE		0x08U
#define RTL_COMMAND_RESET		0x10U
#define RTL_COMMAND_STOP_REQUEST	0x80U
#define RTL_TX_CONFIG_VERSION_MASK	0x7cf00000U
#define RTL_RX_CONFIG_ACCEPT_MASK	0x0000003fU
#define RTL_CONFIG_LOCK_WRITABLE	0xc0U
#define RTL_OCP_DATA_MASK		0x0000ffffU
#define RTL_OCP_BUSY			0x80000000U
#define RTL_OCP_ADDRESS_SHIFT		16U
#define RTL_MCU_RX_FIFO_EMPTY		0x10U
#define RTL_MCU_TX_FIFO_EMPTY		0x20U
#define RTL_MCU_OUT_OF_BAND		0x80U

/*
 * The fields of the configuration, CSI and EPHY windows and of the PHY
 * status.
 *
 * Values referenced: OpenBSD sys/dev/pci/if_rgereg.h 1.20
 * (RGE_CFG2_CLKREQ_EN, RGE_CFG5_PME_STS, RGE_CSIAR_BYTE_EN,
 * RGE_CSIAR_BYTE_EN_SHIFT, RGE_CSIAR_ADDR_MASK, RGE_CSIAR_BUSY,
 * RGE_EPHYAR_DATA_MASK, RGE_EPHYAR_BUSY, RGE_EPHYAR_ADDR_MASK,
 * RGE_EPHYAR_ADDR_SHIFT, RGE_EPHYAR_EXT_ADDR, RGE_PHYSTAT_*); sys/dev/pci/if_rge.c
 * 1.48 (rge_disable_aspm_clkreq: INT_CFG0 bit 0x08; rge_hw_init: 0x00f1
 * bit 0x80; rge_set_phy_power: PMCH 0xc0).
 */
#define RTL_CONFIG2_CLKREQ		0x80U
#define RTL_CONFIG5_PME_STATUS		0x01U
#define RTL_INTERRUPT_CONFIG0_CLKREQ	0x08U
#define RTL_MISC_F1_BIT			0x80U
#define RTL_POWER_CONTROL_PHY_ON	0xc0U
#define RTL_CSI_BYTE_ENABLE		0x0000f000U
#define RTL_CSI_ADDRESS_MASK		0x00000fffU
#define RTL_CSI_BUSY			0x80000000U
#define RTL_EPHY_DATA_MASK		0x0000ffffU
#define RTL_EPHY_BUSY			0x80000000U
#define RTL_EPHY_ADDRESS_MASK		0x0000007fU
#define RTL_EPHY_ADDRESS_SHIFT		16U
#define RTL_EPHY_EXTENDED_ADDRESS	0x0ffeU
#define RTL_PHY_STATUS_FULL_DUPLEX	0x0001U
#define RTL_PHY_STATUS_LINK		0x0002U
#define RTL_PHY_STATUS_10		0x0004U
#define RTL_PHY_STATUS_100		0x0008U
#define RTL_PHY_STATUS_1000		0x0010U
#define RTL_PHY_STATUS_2500		0x0400U
#define RTL_PHY_STATUS_5000		0x1000U
#define RTL_PHY_STATUS_10000		0x4000U

/*
 * The transmit configuration's version of each controller.
 *
 * Values referenced: OpenBSD sys/dev/pci/if_rge.c 1.48 (rge_attach, the
 * hardware revision switch).
 */
#define RTL_VERSION_8125		0x60900000U
#define RTL_VERSION_8125B		0x64100000U
#define RTL_VERSION_8126_1		0x64900000U
#define RTL_VERSION_8126_2		0x64a00000U
#define RTL_VERSION_8125D_1		0x68800000U
#define RTL_VERSION_8125D_2		0x68900000U
#define RTL_VERSION_8127		0x6c900000U

/*
 * The MAC OCP registers and values the out-of-band exit and the version
 * check use.
 *
 * Values referenced: OpenBSD sys/dev/pci/if_rge.c 1.48 (rge_exit_oob,
 * rge_attach: the RTL8127ATF check).
 */
#define RTL_MAC_OCP_REALWOW		0xc0bcU
#define RTL_MAC_OCP_REALWOW_OFF		0x00ffU
#define RTL_MAC_OCP_OOB_CONTROL		0xe8deU
#define RTL_MAC_OCP_OOB_CONTROL_BIT	0x4000U
#define RTL_MAC_OCP_HANDOFF_A		0xc0aaU
#define RTL_MAC_OCP_HANDOFF_A_VALUE	0x07d0U
#define RTL_MAC_OCP_HANDOFF_B		0xc0a6U
#define RTL_MAC_OCP_HANDOFF_B_VALUE	0x01b5U
#define RTL_MAC_OCP_HANDOFF_C		0xc01eU
#define RTL_MAC_OCP_HANDOFF_C_VALUE	0x5555U
#define RTL_MAC_OCP_UPS			0xd42cU
#define RTL_MAC_OCP_UPS_RESUME		0x0100U
#define RTL_MAC_OCP_PORT_TYPE		0xd006U
#define RTL_MAC_OCP_PORT_TYPE_FIBER	0x0007U
#define RTL_TWI_READY			0x0200U
#define RTL_IDLE_STATUS_READY		0x0103U
#define RTL_POWER_SWITCH_RX_GATE	0x08U

/*
 * The PHY OCP registers the out-of-band exit uses.
 *
 * Values referenced: OpenBSD sys/dev/pci/if_rge.c 1.48 (rge_exit_oob).
 */
#define RTL_PHY_OCP_STATE		0xa420U
#define RTL_PHY_OCP_STATE_MASK		0x0007U
#define RTL_PHY_OCP_STATE_HANDED_BACK	0x0002U
#define RTL_PHY_OCP_UPS_A		0xa466U
#define RTL_PHY_OCP_UPS_A_BIT		0x0001U
#define RTL_PHY_OCP_UPS_B		0xa468U
#define RTL_PHY_OCP_UPS_B_BITS		0x000aU

/*
 * The clause-22 registers of the PHY and their bits (IEEE 802.3 clause 22:
 * the control, auto-negotiation advertisement and 1000BASE-T control
 * registers), and where the PHY's OCP space holds them: register r at
 * 0xa400 + 2r.
 *
 * Values referenced: OpenBSD sys/dev/pci/if_rgereg.h 1.20 (RGE_PHYBASE) and
 * sys/dev/pci/if_rge.c 1.48 (rge_write_phy).
 */
#define RTL_PHY_OCP_MII_BASE		0xa400U
#define RTL_MII_CONTROL			0U
#define RTL_MII_ADVERTISE		4U
#define RTL_MII_1000_CONTROL		9U
#define RTL_MII_CONTROL_RESET		0x8000U
#define RTL_MII_CONTROL_AUTONEG		0x1000U
#define RTL_MII_CONTROL_POWER_DOWN	0x0800U
#define RTL_MII_CONTROL_RESTART		0x0200U
#define RTL_MII_ADVERTISE_10		0x0020U
#define RTL_MII_ADVERTISE_10_FULL	0x0040U
#define RTL_MII_ADVERTISE_100		0x0080U
#define RTL_MII_ADVERTISE_100_FULL	0x0100U
#define RTL_MII_ADVERTISE_PAUSE		0x0400U
#define RTL_MII_ADVERTISE_PAUSE_ASYM	0x0800U
#define RTL_MII_1000_FULL		0x0200U
#define RTL_MII_1000_HALF		0x0100U

/*
 * The PHY OCP registers the PHY's power, reset, EEE and multi-gigabit
 * advertisement use.
 *
 * Values referenced: OpenBSD sys/dev/pci/if_rge.c 1.48 (rge_set_phy_power,
 * rge_phy_config, rge_ifmedia_upd) and sys/dev/pci/if_rgereg.h 1.20
 * (RGE_ADV_2500TFDX, RGE_ADV_5000TFDX, RGE_ADV_10000TFDX).
 */
#define RTL_PHY_OCP_STATE_READY		0x0003U
#define RTL_PHY_OCP_MULTIGIG		0xa5d4U
#define RTL_PHY_OCP_MULTIGIG_2500	0x0080U
#define RTL_PHY_OCP_MULTIGIG_5000	0x0100U
#define RTL_PHY_OCP_MULTIGIG_10000	0x1000U
#define RTL_PHY_OCP_INDEX		0xa436U
#define RTL_PHY_OCP_DATA		0xa438U
#define RTL_PHY_OCP_RAM_CODE_VERSION	0x801eU

/*
 * The PHY's patch mode: the request bit, the state bit that follows it,
 * and the bounded wait for the state.
 *
 * Values referenced: FreeBSD sys/dev/rge/if_rge_hw.c, main
 * 8fa55fdcae4ecbd2ec13ef91cc9d172095e21189 (rge_patch_phy_mcu).
 */
#define RTL_PHY_OCP_PATCH_REQUEST	0xb820U
#define RTL_PHY_OCP_PATCH_REQUEST_BIT	0x0010U
#define RTL_PHY_OCP_PATCH_STATE		0xb800U
#define RTL_PHY_OCP_PATCH_STATE_BIT	0x0040U
#define RTL_PATCH_POLLS			1000U
#define RTL_PATCH_POLL_US		100U

/*
 * The MAC OCP registers the hardware set-up uses.
 *
 * Values referenced: OpenBSD sys/dev/pci/if_rge.c 1.48 (rge_hw_init,
 * rge_phy_config: the EEE registers).
 */
#define RTL_MAC_OCP_UPS_CONTROL		0xd40aU
#define RTL_MAC_OCP_UPS_CONTROL_BIT	0x0010U
#define RTL_MAC_OCP_MCU_ENABLE		0xfc26U
#define RTL_MAC_OCP_MCU_BREAK_FIRST	0xfc28U
#define RTL_MAC_OCP_MCU_BREAK_ENABLE	0xfc48U
#define RTL_MAC_OCP_EEE			0xe040U
#define RTL_MAC_OCP_EEE_BITS		0x0003U
#define RTL_MAC_OCP_EEE_ADVANCED	0xe052U
#define RTL_MAC_OCP_EEE_ADVANCED_BIT	0x0001U
#define RTL_CSI_UNCORRECTABLE		0x0108U
#define RTL_CSI_UNCORRECTABLE_BIT	0x00100000U

/*
 * The bounded waits, as a number of polls and the delay between two.
 *
 * Values referenced: OpenBSD sys/dev/pci/if_rge.c 1.48 (rge_reset,
 * rge_exit_oob, rge_write_phy_ocp, rge_read_phy_ocp) and
 * sys/dev/pci/if_rgereg.h 1.20 (RGE_TIMEOUT).
 */
#define RTL_OCP_POLLS			20000U
#define RTL_OCP_POLL_US			1U
#define RTL_TWI_POLLS			10U
#define RTL_TWI_POLL_US			100U
#define RTL_DRAIN_POLLS			3000U
#define RTL_DRAIN_POLL_US		50U
#define RTL_STOP_SETTLE_US		200U
#define RTL_RESET_POLLS			100U
#define RTL_RESET_POLL_US		100U
#define RTL_PHY_STATE_POLLS		100U
#define RTL_PHY_STATE_POLL_US		1000U
#define RTL_PHY_RESET_POLLS		2500U
#define RTL_PHY_RESET_POLL_MS		1U
#define RTL_CSI_SETTLE_US		20U
#define RTL_MCU_STOP_US			3000U
#define RTL_LINK_MS			500U

/*
 * The PCI Express capability's link control register and the power
 * management bits in it (PCI Express Base Specification, the Link Control
 * register: ASPM Control in bits 1:0, Enable Clock Power Management in
 * bit 8).
 */
#define RTL_PCI_CAPABILITY_EXPRESS	0x10U
#define RTL_PCI_EXPRESS_LINK_CONTROL	0x10U
#define RTL_PCI_LINK_ASPM_MASK		0x0003U
#define RTL_PCI_LINK_CLOCK_PM		0x0100U

/*
 * The bits of the 32-byte transmit and receive descriptors.
 *
 * Values referenced: FreeBSD sys/dev/rge/if_rgereg.h, main
 * 8fa55fdcae4ecbd2ec13ef91cc9d172095e21189 (struct rge_tx_desc,
 * struct rge_rx_desc, RGE_TDCMDSTS_*, RGE_RDCMDSTS_*).
 */
#define RTL_DESCRIPTOR_OWN		0x80000000U
#define RTL_DESCRIPTOR_END_OF_RING	0x40000000U
#define RTL_TX_FIRST			0x20000000U
#define RTL_TX_LAST			0x10000000U
#define RTL_TX_ERROR			0x00800000U
#define RTL_RX_FIRST			0x02000000U
#define RTL_RX_LAST			0x01000000U
#define RTL_RX_ERROR			0x00100000U
#define RTL_RX_LENGTH_MASK		0x00003fffU

/*
 * The checksum bits of a descriptor's extended word: on transmit, the
 * IPv4 header's and the TCP checksum for the controller to compute (the
 * TCP one only together with the IPv4 one); on receive, what kind of
 * frame the controller checked and the checksums it found wrong.
 *
 * Values referenced: FreeBSD sys/dev/rge/if_rgereg.h, main
 * 8fa55fdcae4ecbd2ec13ef91cc9d172095e21189 (RGE_TDEXTSTS_IPCSUM,
 * RGE_TDEXTSTS_TCPCSUM, RGE_RDEXTSTS_TCPCSUMERR, RGE_RDEXTSTS_UDPCSUMERR,
 * RGE_RDEXTSTS_IPCSUMERR, RGE_RDEXTSTS_TCPPKT, RGE_RDEXTSTS_UDPPKT,
 * RGE_RDEXTSTS_IPV4, RGE_RDEXTSTS_IPV6); sys/dev/rge/if_rge.c, the same
 * commit (rge_encap: the TCP checksum takes effect only with the IPv4 one;
 * rge_rxeof: the checksum flags).
 */
#define RTL_TX_CHECKSUM_IPV4		0x20000000U
#define RTL_TX_CHECKSUM_TCP		0x40000000U
#define RTL_RX_TCP_CHECKSUM_ERROR	0x01000000U
#define RTL_RX_UDP_CHECKSUM_ERROR	0x02000000U
#define RTL_RX_IPV4_CHECKSUM_ERROR	0x04000000U
#define RTL_RX_TCP			0x10000000U
#define RTL_RX_UDP			0x20000000U
#define RTL_RX_IPV4			0x40000000U
#define RTL_RX_IPV6			0x80000000U

/*
 * The rings: descriptors per ring, the bytes of a receive buffer and of a
 * transmit bounce buffer, which hold a whole frame, and the alignments.
 * The receive ring holds about 1.2 ms of full-size frames at 10 Gbit/s,
 * for the network worker's pauses.
 *
 * Value referenced: FreeBSD sys/dev/rge/if_rgereg.h, main
 * 8fa55fdcae4ecbd2ec13ef91cc9d172095e21189 (RGE_RX_LIST_CNT).
 */
#define RTL_TX_COUNT			512U
#define RTL_RX_COUNT			1024U
#define RTL_DESCRIPTOR_BYTES		32U
#define RTL_BUFFER_BYTES		2048U
#define RTL_RING_ALIGNMENT		256U
#define RTL_BUFFER_ALIGNMENT		64U

/*
 * The frame sizes: an Ethernet header, the checksum the receiver leaves
 * on, the shortest frame the wire carries without its checksum, and the
 * longest one the interface sends.
 */
#define RTL_HEADER_BYTES		14U
#define RTL_CRC_BYTES			4U
#define RTL_MIN_FRAME			60U
#define RTL_FRAME_MAX			(RTL_DEFAULT_MTU + RTL_HEADER_BYTES)

/* How often the service thread wakes the network worker while the interface is open. */
#define RTL_POLL_MS			20U

/*
 * How many interrupt mitigation registers the parts have.
 *
 * Values referenced: FreeBSD sys/dev/rge/if_rge.c, main
 * 8fa55fdcae4ecbd2ec13ef91cc9d172095e21189 (rge_init_locked, num_miti).
 */
#define RTL_MITIGATION_COUNT_SHORT	32U
#define RTL_MITIGATION_COUNT_LONG	64U

/*
 * The tally counters the controller writes to memory on request: the
 * register that takes the block's address with the bit that starts the
 * write and stays set until it is done, the block's size and alignment,
 * and where the count of frames missed for want of a receive descriptor
 * sits in it (a 16-bit count that wraps).
 *
 * Values referenced: FreeBSD sys/dev/rge/if_rgereg.h, main
 * 8fa55fdcae4ecbd2ec13ef91cc9d172095e21189 (RGE_DTCCR_LO, RGE_DTCCR_HI,
 * RGE_DTCCR_CMD, struct rge_hw_mac_stats, RGE_STATS_ALIGNMENT).
 */
#define RTL_REG_TALLY_LOW		0x0010U
#define RTL_REG_TALLY_HIGH		0x0014U
#define RTL_TALLY_DUMP			0x00000008U
#define RTL_TALLY_BYTES			256U
#define RTL_TALLY_ALIGNMENT		64U
#define RTL_TALLY_MISSED		28U
#define RTL_TALLY_POLLS			1000U
#define RTL_TALLY_POLL_US		10U

/* The interface's MTU and how many controllers the driver serves. */
#define RTL_DEFAULT_MTU			1500U
#define RTL_MAX_DEVICES			4U

/* Marks a parameter that an interface passes but the callee does not need. */
#define UNUSED_PARAMETER(parameter)	((void)(parameter))

/* The members of the family whose steps differ. */
enum rtl_family {
	RTL_FAMILY_8125,
	RTL_FAMILY_8125B,
	RTL_FAMILY_8125D,
	RTL_FAMILY_8126,
	RTL_FAMILY_8127
};

/* What one step of a PHY register sequence does. */
enum rtl_phy_step_kind {
	RTL_PHY_STEP_WRITE,
	RTL_PHY_STEP_MODIFY,
	RTL_PHY_STEP_READ,
	RTL_PHY_STEP_PATCH
};

/*
 * One step of a PHY register sequence.
 *
 * A write stores value; a modify clears the mask bits and sets the value
 * bits of what the register holds; a read only reads, for a register whose
 * read has an effect; a patch step takes the PHY into its patch mode when
 * value is 1 and out of it when value is 0.
 */
struct rtl_phy_step {
	enum rtl_phy_step_kind kind;
	uint16_t reg;
	uint16_t mask;
	uint16_t value;
};

/* One write of a PCI Express PHY (EPHY) table. */
struct rtl_ephy_write {
	uint16_t reg;
	uint16_t value;
};

/*
 * One transmit descriptor: the command and status, the extended (offload)
 * word, the buffer's bus address, and four words the controller ignores.
 * The descriptors are little-endian, as the CPUs the driver runs on are.
 */
struct rtl_tx_descriptor {
	volatile uint32_t command;
	volatile uint32_t extended;
	volatile uint32_t address_low;
	volatile uint32_t address_high;
	volatile uint32_t reserved[4];
};

/*
 * One receive descriptor: four words the controller writes its hash and
 * header information to, the buffer's bus address, the extended (offload)
 * status, and the command and status with the frame's length.
 */
struct rtl_rx_descriptor {
	volatile uint32_t information[4];
	volatile uint32_t address_low;
	volatile uint32_t address_high;
	volatile uint32_t extended;
	volatile uint32_t command;
};

/*
 * One version of the controller the driver knows.
 *
 * The version is the transmit configuration's masked value; the family
 * picks the steps, and the revision tells the two parts of one family
 * apart where their steps differ.
 */
struct rtl_chip {
	uint32_t version;
	enum rtl_family family;
	unsigned revision;
	const char *name;
};

/*
 * The driver's own work, counted only in a kernel built with
 * CONFIG_DRIVER_PCI_RTL8125_PROFILE: how often the interrupt, the poll and
 * the transmit ran and how long each took, in the fine counter's units,
 * which the service thread reports once a second while frames move.  The
 * poll's time includes handing the received frames to the stack's input
 * queue but not the stack's work on them.  Guarded by the device's lock.
 */
struct rtl_profile {
	uint64_t interrupts;
	uint64_t interrupt_time;
	uint64_t polls;
	uint64_t full_polls;
	uint64_t poll_frames;
	uint64_t poll_time;
	uint64_t transmits;
	uint64_t transmit_copies;
	uint64_t transmit_refusals;
	uint64_t transmit_time;
	uint64_t reported_frames;
	uint64_t next_report;
	struct drv_dma_buffer tally;
	unsigned tally_allocated;
};

/*
 * One attached controller.
 *
 * The lock serializes the register windows and every register write after
 * attach; the interrupt takes it too.  An attached controller is kept for
 * the life of the system.
 */
struct rtl_device {
	struct spinlock lock;
	struct drv_pci_device *pci;
	struct drv_pci_enable_state enable_state;
	struct drv_pci_mapping mapping;
	struct drv_pci_irq irq;
	void *irq_cookie;
	/*
	 * What attach has taken so far, which a failed attach gives back in
	 * the reverse order.
	 */
	unsigned bar_claimed;
	unsigned enable_saved;
	unsigned bar_mapped;
	unsigned descriptors_allocated;
	unsigned tx_buffers_allocated;
	unsigned rx_buffers_allocated;
	/*
	 * The rings, allocated at attach and kept: one coherent block of the
	 * transmit then the receive descriptors, the receive buffers by slot,
	 * and the transmit bounce buffers by slot for frames the controller
	 * cannot reach in place.
	 */
	struct drv_dma_device *dma;
	struct drv_dma_buffer descriptors;
	struct drv_dma_buffer tx_buffers;
	struct drv_dma_buffer rx_buffers;
	struct rtl_tx_descriptor *tx_ring;
	struct rtl_rx_descriptor *rx_ring;
	uint64_t tx_ring_address;
	uint64_t rx_ring_address;
	/*
	 * The packets the transmit descriptors point at in place, by slot (NULL
	 * for a frame copied into the slot's bounce buffer), and the ones the
	 * controller has finished, linked by next, which the caller of
	 * reclaim_tx frees once the lock is released.  The producer and
	 * consumer count descriptors without wrapping; the slot is the count
	 * modulo the ring.  Guarded by the lock.
	 */
	struct packet_buf *tx_packets[RTL_TX_COUNT];
	struct packet_buf *tx_done;
	unsigned tx_producer;
	unsigned tx_consumer;
	unsigned rx_consumer;
	/*
	 * Whether the interface is open, and the link as last announced.
	 * Guarded by the lock; the service thread reads the PHY status only
	 * while the interface is open.  A system error the interrupt saw is
	 * reported once by the poll.
	 */
	unsigned opened;
	unsigned carrier;
	uint32_t link_mbps;
	unsigned system_error;
	unsigned system_error_reported;
	struct thread *link_thread;
	uint16_t ram_code_version;
	volatile uint8_t *registers;
	const struct rtl_chip *chip;
	unsigned fiber;
	struct net_device *net;
	char name[NET_DEVICE_NAME_MAX];
	uint8_t mac[6];
	/*
	 * The profile, the fine counter's rate and what one read of it costs,
	 * in its units, measured at attach.
	 */
	struct rtl_profile profile;
	uint64_t counter_hz;
	uint64_t counter_cost;
};

/*
 * The versions the driver knows, in the order the parts appeared.
 *
 * Values referenced: OpenBSD sys/dev/pci/if_rge.c 1.48 (rge_attach, the
 * hardware revision switch).
 */
static const struct rtl_chip rtl_chips[] = {
	{ RTL_VERSION_8125, RTL_FAMILY_8125, 1U, "RTL8125" },
	{ RTL_VERSION_8125B, RTL_FAMILY_8125B, 1U, "RTL8125B" },
	{ RTL_VERSION_8126_1, RTL_FAMILY_8126, 1U, "RTL8126" },
	{ RTL_VERSION_8126_2, RTL_FAMILY_8126, 2U, "RTL8126" },
	{ RTL_VERSION_8125D_1, RTL_FAMILY_8125D, 1U, "RTL8125D" },
	{ RTL_VERSION_8125D_2, RTL_FAMILY_8125D, 2U, "RTL8125D" },
	{ RTL_VERSION_8127, RTL_FAMILY_8127, 1U, "RTL8127" },
};

/*
 * The RTL8127's PCI Express PHY settings.
 *
 * Values referenced: OpenBSD sys/dev/pci/if_rgereg.h 1.20 (mac_r27_ephy).
 */
static const struct rtl_ephy_write rtl_ephy_8127[] = {
	{ 0x8088, 0x0064 }, { 0x8488, 0x0064 }, { 0x8888, 0x0064 },
	{ 0x8c88, 0x0064 }, { 0x8188, 0x0064 }, { 0x8588, 0x0064 },
	{ 0x8988, 0x0064 }, { 0x8d88, 0x0064 }, { 0x808c, 0x09b0 },
	{ 0x848c, 0x09b0 }, { 0x888c, 0x0f90 }, { 0x8c8c, 0x0f90 },
	{ 0x818c, 0x09b0 }, { 0x858c, 0x09b0 }, { 0x898c, 0x0f90 },
	{ 0x8d8c, 0x0f90 }, { 0x808a, 0x09b8 }, { 0x848a, 0x09b8 },
	{ 0x888a, 0x0f98 }, { 0x8c8a, 0x0f98 }, { 0x818a, 0x09b8 },
	{ 0x858a, 0x09b8 }, { 0x898a, 0x0f98 }, { 0x8d8a, 0x0f98 },
	{ 0x9020, 0x0080 }, { 0x9420, 0x0080 }, { 0x9820, 0x0080 },
	{ 0x9c20, 0x0080 }, { 0x901e, 0x0190 }, { 0x941e, 0x0190 },
	{ 0x981e, 0x0140 }, { 0x9c1e, 0x0140 }, { 0x901c, 0x0190 },
	{ 0x941c, 0x0190 }, { 0x981c, 0x0140 }, { 0x9c1c, 0x0140 }
};

/*
 * The RTL8127's PHY settings after its reset, in order, without the PHY
 * microcode patch that the reference loads first.
 *
 * Procedure and values referenced: OpenBSD sys/dev/pci/if_rge.c 1.48
 * (rge_phy_config_mac_r27, with its mac_cfg_value, mac_cfg2_value and
 * mac_cfg_a438_value tables written out in place).
 */
static const struct rtl_phy_step rtl_phy_8127[] = {
	{ RTL_PHY_STEP_WRITE, 0xa4d2, 0x0000, 0x0000 },
	{ RTL_PHY_STEP_READ, 0xa4d4, 0x0000, 0x0000 },
	{ RTL_PHY_STEP_MODIFY, 0xa442, 0x0800, 0x0000 },
	{ RTL_PHY_STEP_WRITE, 0xa436, 0x0000, 0x8415 },
	{ RTL_PHY_STEP_MODIFY, 0xa438, 0xff00, 0x9300 },
	{ RTL_PHY_STEP_WRITE, 0xa436, 0x0000, 0x81a3 },
	{ RTL_PHY_STEP_MODIFY, 0xa438, 0xff00, 0x0f00 },
	{ RTL_PHY_STEP_WRITE, 0xa436, 0x0000, 0x81ae },
	{ RTL_PHY_STEP_MODIFY, 0xa438, 0xff00, 0x0f00 },
	{ RTL_PHY_STEP_WRITE, 0xa436, 0x0000, 0x81b9 },
	{ RTL_PHY_STEP_MODIFY, 0xa438, 0xff00, 0xb900 },
	{ RTL_PHY_STEP_WRITE, 0xb87c, 0x0000, 0x83b0 },
	{ RTL_PHY_STEP_MODIFY, 0xb87e, 0x0e00, 0x0000 },
	{ RTL_PHY_STEP_WRITE, 0xb87c, 0x0000, 0x83c5 },
	{ RTL_PHY_STEP_MODIFY, 0xb87e, 0x0e00, 0x0000 },
	{ RTL_PHY_STEP_WRITE, 0xb87c, 0x0000, 0x83da },
	{ RTL_PHY_STEP_MODIFY, 0xb87e, 0x0e00, 0x0000 },
	{ RTL_PHY_STEP_WRITE, 0xb87c, 0x0000, 0x83ef },
	{ RTL_PHY_STEP_MODIFY, 0xb87e, 0x0e00, 0x0000 },
	{ RTL_PHY_STEP_MODIFY, 0xbf38, 0x01f0, 0x0160 },
	{ RTL_PHY_STEP_MODIFY, 0xbf3a, 0x001f, 0x0014 },
	{ RTL_PHY_STEP_MODIFY, 0xbf28, 0x6000, 0x0000 },
	{ RTL_PHY_STEP_MODIFY, 0xbf2c, 0xc000, 0x0000 },
	{ RTL_PHY_STEP_MODIFY, 0xbf28, 0x1fff, 0x0187 },
	{ RTL_PHY_STEP_MODIFY, 0xbf2a, 0x003f, 0x0003 },
	{ RTL_PHY_STEP_WRITE, 0xa436, 0x0000, 0x8173 },
	{ RTL_PHY_STEP_WRITE, 0xa438, 0x0000, 0x8620 },
	{ RTL_PHY_STEP_WRITE, 0xa436, 0x0000, 0x8175 },
	{ RTL_PHY_STEP_WRITE, 0xa438, 0x0000, 0x8671 },
	{ RTL_PHY_STEP_WRITE, 0xa436, 0x0000, 0x817c },
	{ RTL_PHY_STEP_MODIFY, 0xa438, 0x0000, 0x2000 },
	{ RTL_PHY_STEP_WRITE, 0xa436, 0x0000, 0x8187 },
	{ RTL_PHY_STEP_MODIFY, 0xa438, 0x0000, 0x2000 },
	{ RTL_PHY_STEP_WRITE, 0xa436, 0x0000, 0x8192 },
	{ RTL_PHY_STEP_MODIFY, 0xa438, 0x0000, 0x2000 },
	{ RTL_PHY_STEP_WRITE, 0xa436, 0x0000, 0x819d },
	{ RTL_PHY_STEP_MODIFY, 0xa438, 0x0000, 0x2000 },
	{ RTL_PHY_STEP_WRITE, 0xa436, 0x0000, 0x81a8 },
	{ RTL_PHY_STEP_MODIFY, 0xa438, 0x2000, 0x0000 },
	{ RTL_PHY_STEP_WRITE, 0xa436, 0x0000, 0x81b3 },
	{ RTL_PHY_STEP_MODIFY, 0xa438, 0x2000, 0x0000 },
	{ RTL_PHY_STEP_WRITE, 0xa436, 0x0000, 0x81be },
	{ RTL_PHY_STEP_MODIFY, 0xa438, 0x0000, 0x2000 },
	{ RTL_PHY_STEP_WRITE, 0xa436, 0x0000, 0x817d },
	{ RTL_PHY_STEP_MODIFY, 0xa438, 0xff00, 0xa600 },
	{ RTL_PHY_STEP_WRITE, 0xa436, 0x0000, 0x8188 },
	{ RTL_PHY_STEP_MODIFY, 0xa438, 0xff00, 0xa600 },
	{ RTL_PHY_STEP_WRITE, 0xa436, 0x0000, 0x8193 },
	{ RTL_PHY_STEP_MODIFY, 0xa438, 0xff00, 0xa600 },
	{ RTL_PHY_STEP_WRITE, 0xa436, 0x0000, 0x819e },
	{ RTL_PHY_STEP_MODIFY, 0xa438, 0xff00, 0xa600 },
	{ RTL_PHY_STEP_WRITE, 0xa436, 0x0000, 0x81a9 },
	{ RTL_PHY_STEP_MODIFY, 0xa438, 0xff00, 0x1400 },
	{ RTL_PHY_STEP_WRITE, 0xa436, 0x0000, 0x81b4 },
	{ RTL_PHY_STEP_MODIFY, 0xa438, 0xff00, 0x1400 },
	{ RTL_PHY_STEP_WRITE, 0xa436, 0x0000, 0x81bf },
	{ RTL_PHY_STEP_MODIFY, 0xa438, 0xff00, 0xa600 },
	{ RTL_PHY_STEP_MODIFY, 0xaeaa, 0x0028, 0x0000 },
	{ RTL_PHY_STEP_WRITE, 0xb87c, 0x0000, 0x84f0 },
	{ RTL_PHY_STEP_WRITE, 0xb87e, 0x0000, 0x201c },
	{ RTL_PHY_STEP_WRITE, 0xb87c, 0x0000, 0x84f2 },
	{ RTL_PHY_STEP_WRITE, 0xb87e, 0x0000, 0x3117 },
	{ RTL_PHY_STEP_WRITE, 0xaec6, 0x0000, 0x0000 },
	{ RTL_PHY_STEP_WRITE, 0xae20, 0x0000, 0xffff },
	{ RTL_PHY_STEP_WRITE, 0xaece, 0x0000, 0xffff },
	{ RTL_PHY_STEP_WRITE, 0xaed2, 0x0000, 0xffff },
	{ RTL_PHY_STEP_WRITE, 0xaec8, 0x0000, 0x0000 },
	{ RTL_PHY_STEP_MODIFY, 0xaed0, 0x0001, 0x0000 },
	{ RTL_PHY_STEP_WRITE, 0xadb8, 0x0000, 0x0150 },
	{ RTL_PHY_STEP_WRITE, 0xb87c, 0x0000, 0x8197 },
	{ RTL_PHY_STEP_MODIFY, 0xb87e, 0xff00, 0x5000 },
	{ RTL_PHY_STEP_WRITE, 0xb87c, 0x0000, 0x8231 },
	{ RTL_PHY_STEP_MODIFY, 0xb87e, 0xff00, 0x5000 },
	{ RTL_PHY_STEP_WRITE, 0xb87c, 0x0000, 0x82cb },
	{ RTL_PHY_STEP_MODIFY, 0xb87e, 0xff00, 0x5000 },
	{ RTL_PHY_STEP_WRITE, 0xb87c, 0x0000, 0x82cd },
	{ RTL_PHY_STEP_MODIFY, 0xb87e, 0xff00, 0x5700 },
	{ RTL_PHY_STEP_WRITE, 0xb87c, 0x0000, 0x8233 },
	{ RTL_PHY_STEP_MODIFY, 0xb87e, 0xff00, 0x5700 },
	{ RTL_PHY_STEP_WRITE, 0xb87c, 0x0000, 0x8199 },
	{ RTL_PHY_STEP_MODIFY, 0xb87e, 0xff00, 0x5700 },
	{ RTL_PHY_STEP_WRITE, 0xb87c, 0x0000, 0x815a },
	{ RTL_PHY_STEP_WRITE, 0xb87e, 0x0000, 0x0150 },
	{ RTL_PHY_STEP_WRITE, 0xb87c, 0x0000, 0x81f4 },
	{ RTL_PHY_STEP_WRITE, 0xb87e, 0x0000, 0x0150 },
	{ RTL_PHY_STEP_WRITE, 0xb87c, 0x0000, 0x828e },
	{ RTL_PHY_STEP_WRITE, 0xb87e, 0x0000, 0x0150 },
	{ RTL_PHY_STEP_WRITE, 0xb87c, 0x0000, 0x81b1 },
	{ RTL_PHY_STEP_WRITE, 0xb87e, 0x0000, 0x0000 },
	{ RTL_PHY_STEP_WRITE, 0xb87c, 0x0000, 0x824b },
	{ RTL_PHY_STEP_WRITE, 0xb87e, 0x0000, 0x0000 },
	{ RTL_PHY_STEP_WRITE, 0xb87c, 0x0000, 0x82e5 },
	{ RTL_PHY_STEP_WRITE, 0xb87e, 0x0000, 0x0000 },
	{ RTL_PHY_STEP_WRITE, 0xb87c, 0x0000, 0x84f7 },
	{ RTL_PHY_STEP_MODIFY, 0xb87e, 0xff00, 0x2800 },
	{ RTL_PHY_STEP_MODIFY, 0xaec2, 0x0000, 0x1000 },
	{ RTL_PHY_STEP_WRITE, 0xb87c, 0x0000, 0x81b3 },
	{ RTL_PHY_STEP_MODIFY, 0xb87e, 0xff00, 0xad00 },
	{ RTL_PHY_STEP_WRITE, 0xb87c, 0x0000, 0x824d },
	{ RTL_PHY_STEP_MODIFY, 0xb87e, 0xff00, 0xad00 },
	{ RTL_PHY_STEP_WRITE, 0xb87c, 0x0000, 0x82e7 },
	{ RTL_PHY_STEP_MODIFY, 0xb87e, 0xff00, 0xad00 },
	{ RTL_PHY_STEP_MODIFY, 0xae4e, 0x000f, 0x0001 },
	{ RTL_PHY_STEP_WRITE, 0xb87c, 0x0000, 0x82ce },
	{ RTL_PHY_STEP_MODIFY, 0xb87e, 0xf000, 0x4000 },
	{ RTL_PHY_STEP_WRITE, 0xb87c, 0x0000, 0x83a5 },
	{ RTL_PHY_STEP_MODIFY, 0xb87e, 0xff00, 0x0400 },
	{ RTL_PHY_STEP_WRITE, 0xb87c, 0x0000, 0x83a6 },
	{ RTL_PHY_STEP_MODIFY, 0xb87e, 0xff00, 0x0400 },
	{ RTL_PHY_STEP_WRITE, 0xb87c, 0x0000, 0x83a7 },
	{ RTL_PHY_STEP_MODIFY, 0xb87e, 0xff00, 0x0400 },
	{ RTL_PHY_STEP_WRITE, 0xb87c, 0x0000, 0x83a8 },
	{ RTL_PHY_STEP_MODIFY, 0xb87e, 0xff00, 0x0400 },
	{ RTL_PHY_STEP_WRITE, 0xb87c, 0x0000, 0x84ac },
	{ RTL_PHY_STEP_WRITE, 0xb87e, 0x0000, 0x0000 },
	{ RTL_PHY_STEP_WRITE, 0xb87c, 0x0000, 0x84ae },
	{ RTL_PHY_STEP_WRITE, 0xb87e, 0x0000, 0x0000 },
	{ RTL_PHY_STEP_WRITE, 0xb87c, 0x0000, 0x84b0 },
	{ RTL_PHY_STEP_WRITE, 0xb87e, 0x0000, 0xf818 },
	{ RTL_PHY_STEP_WRITE, 0xb87c, 0x0000, 0x84b2 },
	{ RTL_PHY_STEP_MODIFY, 0xb87e, 0xff00, 0x6000 },
	{ RTL_PHY_STEP_WRITE, 0xb87c, 0x0000, 0x8ffc },
	{ RTL_PHY_STEP_WRITE, 0xb87e, 0x0000, 0x6008 },
	{ RTL_PHY_STEP_WRITE, 0xb87c, 0x0000, 0x8ffe },
	{ RTL_PHY_STEP_WRITE, 0xb87e, 0x0000, 0xf4ff },
	{ RTL_PHY_STEP_WRITE, 0xb87c, 0x0000, 0x8015 },
	{ RTL_PHY_STEP_MODIFY, 0xb87e, 0x0000, 0x0200 },
	{ RTL_PHY_STEP_WRITE, 0xb87c, 0x0000, 0x8016 },
	{ RTL_PHY_STEP_MODIFY, 0xb87e, 0x0800, 0x0000 },
	{ RTL_PHY_STEP_WRITE, 0xb87c, 0x0000, 0x8fe6 },
	{ RTL_PHY_STEP_MODIFY, 0xb87e, 0xff00, 0x0800 },
	{ RTL_PHY_STEP_WRITE, 0xb87c, 0x0000, 0x8fe4 },
	{ RTL_PHY_STEP_WRITE, 0xb87e, 0x0000, 0x2114 },
	{ RTL_PHY_STEP_WRITE, 0xb87c, 0x0000, 0x8647 },
	{ RTL_PHY_STEP_WRITE, 0xb87e, 0x0000, 0xa7b1 },
	{ RTL_PHY_STEP_WRITE, 0xb87c, 0x0000, 0x8649 },
	{ RTL_PHY_STEP_WRITE, 0xb87e, 0x0000, 0xbbca },
	{ RTL_PHY_STEP_WRITE, 0xb87c, 0x0000, 0x864b },
	{ RTL_PHY_STEP_MODIFY, 0xb87e, 0xff00, 0xdc00 },
	{ RTL_PHY_STEP_WRITE, 0xb87c, 0x0000, 0x8154 },
	{ RTL_PHY_STEP_MODIFY, 0xb87e, 0xc000, 0x4000 },
	{ RTL_PHY_STEP_WRITE, 0xb87c, 0x0000, 0x8158 },
	{ RTL_PHY_STEP_MODIFY, 0xb87e, 0xc000, 0x0000 },
	{ RTL_PHY_STEP_WRITE, 0xb87c, 0x0000, 0x826c },
	{ RTL_PHY_STEP_WRITE, 0xb87e, 0x0000, 0xffff },
	{ RTL_PHY_STEP_WRITE, 0xb87c, 0x0000, 0x826e },
	{ RTL_PHY_STEP_WRITE, 0xb87e, 0x0000, 0xffff },
	{ RTL_PHY_STEP_WRITE, 0xb87c, 0x0000, 0x8872 },
	{ RTL_PHY_STEP_MODIFY, 0xb87e, 0xff00, 0x0e00 },
	{ RTL_PHY_STEP_WRITE, 0xa436, 0x0000, 0x8012 },
	{ RTL_PHY_STEP_MODIFY, 0xa438, 0x0000, 0x0800 },
	{ RTL_PHY_STEP_WRITE, 0xa436, 0x0000, 0x8012 },
	{ RTL_PHY_STEP_MODIFY, 0xa438, 0x0000, 0x4000 },
	{ RTL_PHY_STEP_MODIFY, 0xb576, 0x0000, 0x0001 },
	{ RTL_PHY_STEP_WRITE, 0xa436, 0x0000, 0x834a },
	{ RTL_PHY_STEP_MODIFY, 0xa438, 0xff00, 0x0700 },
	{ RTL_PHY_STEP_WRITE, 0xb87c, 0x0000, 0x8217 },
	{ RTL_PHY_STEP_MODIFY, 0xb87e, 0x3f00, 0x2a00 },
	{ RTL_PHY_STEP_WRITE, 0xa436, 0x0000, 0x81b1 },
	{ RTL_PHY_STEP_MODIFY, 0xa438, 0xff00, 0x0b00 },
	{ RTL_PHY_STEP_WRITE, 0xb87c, 0x0000, 0x8fed },
	{ RTL_PHY_STEP_MODIFY, 0xb87e, 0xff00, 0x4e00 },
	{ RTL_PHY_STEP_WRITE, 0xb87c, 0x0000, 0x88ac },
	{ RTL_PHY_STEP_MODIFY, 0xb87e, 0xff00, 0x2300 },
	{ RTL_PHY_STEP_MODIFY, 0xbf0c, 0x0000, 0x3800 },
	{ RTL_PHY_STEP_WRITE, 0xb87c, 0x0000, 0x88de },
	{ RTL_PHY_STEP_MODIFY, 0xb87e, 0xff00, 0x0000 },
	{ RTL_PHY_STEP_WRITE, 0xb87c, 0x0000, 0x80b4 },
	{ RTL_PHY_STEP_WRITE, 0xb87e, 0x0000, 0x5195 },
	{ RTL_PHY_STEP_WRITE, 0xa436, 0x0000, 0x8370 },
	{ RTL_PHY_STEP_WRITE, 0xa438, 0x0000, 0x8671 },
	{ RTL_PHY_STEP_WRITE, 0xa436, 0x0000, 0x8372 },
	{ RTL_PHY_STEP_WRITE, 0xa438, 0x0000, 0x86c8 },
	{ RTL_PHY_STEP_WRITE, 0xa436, 0x0000, 0x8401 },
	{ RTL_PHY_STEP_WRITE, 0xa438, 0x0000, 0x86c8 },
	{ RTL_PHY_STEP_WRITE, 0xa436, 0x0000, 0x8403 },
	{ RTL_PHY_STEP_WRITE, 0xa438, 0x0000, 0x86da },
	{ RTL_PHY_STEP_WRITE, 0xa436, 0x0000, 0x8406 },
	{ RTL_PHY_STEP_MODIFY, 0xa438, 0x1800, 0x1000 },
	{ RTL_PHY_STEP_WRITE, 0xa436, 0x0000, 0x8408 },
	{ RTL_PHY_STEP_MODIFY, 0xa438, 0x1800, 0x1000 },
	{ RTL_PHY_STEP_WRITE, 0xa436, 0x0000, 0x840a },
	{ RTL_PHY_STEP_MODIFY, 0xa438, 0x1800, 0x1000 },
	{ RTL_PHY_STEP_WRITE, 0xa436, 0x0000, 0x840c },
	{ RTL_PHY_STEP_MODIFY, 0xa438, 0x1800, 0x1000 },
	{ RTL_PHY_STEP_WRITE, 0xa436, 0x0000, 0x840e },
	{ RTL_PHY_STEP_MODIFY, 0xa438, 0x1800, 0x1000 },
	{ RTL_PHY_STEP_WRITE, 0xa436, 0x0000, 0x8410 },
	{ RTL_PHY_STEP_MODIFY, 0xa438, 0x1800, 0x1000 },
	{ RTL_PHY_STEP_WRITE, 0xa436, 0x0000, 0x8412 },
	{ RTL_PHY_STEP_MODIFY, 0xa438, 0x1800, 0x1000 },
	{ RTL_PHY_STEP_WRITE, 0xa436, 0x0000, 0x8414 },
	{ RTL_PHY_STEP_MODIFY, 0xa438, 0x1800, 0x1000 },
	{ RTL_PHY_STEP_WRITE, 0xa436, 0x0000, 0x8416 },
	{ RTL_PHY_STEP_MODIFY, 0xa438, 0x1800, 0x1000 },
	{ RTL_PHY_STEP_WRITE, 0xa436, 0x0000, 0x82bd },
	{ RTL_PHY_STEP_WRITE, 0xa438, 0x0000, 0x1f40 },
	{ RTL_PHY_STEP_MODIFY, 0xbfb4, 0x07ff, 0x0328 },
	{ RTL_PHY_STEP_WRITE, 0xbfb6, 0x0000, 0x3e14 },
	{ RTL_PHY_STEP_WRITE, 0xa436, 0x0000, 0x81c4 },
	{ RTL_PHY_STEP_WRITE, 0xa438, 0x0000, 0x003b },
	{ RTL_PHY_STEP_WRITE, 0xa438, 0x0000, 0x0086 },
	{ RTL_PHY_STEP_WRITE, 0xa438, 0x0000, 0x00b7 },
	{ RTL_PHY_STEP_WRITE, 0xa438, 0x0000, 0x00db },
	{ RTL_PHY_STEP_WRITE, 0xa438, 0x0000, 0x00fe },
	{ RTL_PHY_STEP_WRITE, 0xa438, 0x0000, 0x00fe },
	{ RTL_PHY_STEP_WRITE, 0xa438, 0x0000, 0x00fe },
	{ RTL_PHY_STEP_WRITE, 0xa438, 0x0000, 0x00fe },
	{ RTL_PHY_STEP_WRITE, 0xa438, 0x0000, 0x00c3 },
	{ RTL_PHY_STEP_WRITE, 0xa438, 0x0000, 0x0078 },
	{ RTL_PHY_STEP_WRITE, 0xa438, 0x0000, 0x0047 },
	{ RTL_PHY_STEP_WRITE, 0xa438, 0x0000, 0x0023 },
	{ RTL_PHY_STEP_WRITE, 0xb87c, 0x0000, 0x88d7 },
	{ RTL_PHY_STEP_WRITE, 0xb87e, 0x0000, 0x01a0 },
	{ RTL_PHY_STEP_WRITE, 0xb87c, 0x0000, 0x88d9 },
	{ RTL_PHY_STEP_WRITE, 0xb87e, 0x0000, 0x01a0 },
	{ RTL_PHY_STEP_WRITE, 0xb87c, 0x0000, 0x8ffa },
	{ RTL_PHY_STEP_WRITE, 0xb87e, 0x0000, 0x002a },
	{ RTL_PHY_STEP_WRITE, 0xb87c, 0x0000, 0x8fee },
	{ RTL_PHY_STEP_WRITE, 0xb87e, 0x0000, 0xffdf },
	{ RTL_PHY_STEP_WRITE, 0xb87c, 0x0000, 0x8ff0 },
	{ RTL_PHY_STEP_WRITE, 0xb87e, 0x0000, 0xffff },
	{ RTL_PHY_STEP_WRITE, 0xb87c, 0x0000, 0x8ff2 },
	{ RTL_PHY_STEP_WRITE, 0xb87e, 0x0000, 0x0a4a },
	{ RTL_PHY_STEP_WRITE, 0xb87c, 0x0000, 0x8ff4 },
	{ RTL_PHY_STEP_WRITE, 0xb87e, 0x0000, 0xaa5a },
	{ RTL_PHY_STEP_WRITE, 0xb87c, 0x0000, 0x8ff6 },
	{ RTL_PHY_STEP_WRITE, 0xb87e, 0x0000, 0x0a4a },
	{ RTL_PHY_STEP_WRITE, 0xb87c, 0x0000, 0x8ff8 },
	{ RTL_PHY_STEP_WRITE, 0xb87e, 0x0000, 0xaa5a },
	{ RTL_PHY_STEP_WRITE, 0xb87c, 0x0000, 0x88d5 },
	{ RTL_PHY_STEP_MODIFY, 0xb87e, 0xff00, 0x0200 },
	{ RTL_PHY_STEP_WRITE, 0xa436, 0x0000, 0x84bb },
	{ RTL_PHY_STEP_MODIFY, 0xa438, 0xff00, 0x0a00 },
	{ RTL_PHY_STEP_WRITE, 0xa436, 0x0000, 0x84c0 },
	{ RTL_PHY_STEP_MODIFY, 0xa438, 0xff00, 0x1600 },
	{ RTL_PHY_STEP_MODIFY, 0xa430, 0x0000, 0x0003 },
	{ RTL_PHY_STEP_MODIFY, 0xa442, 0x0000, 0x0080 },
	{ RTL_PHY_STEP_MODIFY, 0xa430, 0x1000, 0x0000 },
};

/*
 * The RTL8125's PCI Express PHY table writes, without the read-modify-write
 * settings that the reference applies after them.
 *
 * Values referenced: FreeBSD sys/dev/rge/if_rge_microcode.h, main
 * 8fa55fdcae4ecbd2ec13ef91cc9d172095e21189 (mac_r25_ephy).
 */
static const struct rtl_ephy_write rtl_ephy_8125[] = {
	{ 0x0004, 0xd000 }, { 0x000a, 0x8653 }, { 0x0023, 0xab66 },
	{ 0x0020, 0x9455 }, { 0x0021, 0x99ff }, { 0x0029, 0xfe04 },
	{ 0x0044, 0xd000 }, { 0x004a, 0x8653 }, { 0x0063, 0xab66 },
	{ 0x0060, 0x9455 }, { 0x0061, 0x99ff }, { 0x0069, 0xfe04 }
};

/*
 * The RTL8125B's PCI Express PHY settings.
 *
 * Values referenced: FreeBSD sys/dev/rge/if_rge_microcode.h, main
 * 8fa55fdcae4ecbd2ec13ef91cc9d172095e21189 (mac_r25b_ephy).
 */
static const struct rtl_ephy_write rtl_ephy_8125b[] = {
	{ 0x000b, 0xa908 }, { 0x001e, 0x20eb }, { 0x0022, 0x0023 },
	{ 0x0002, 0x60c2 }, { 0x0029, 0xff00 }, { 0x004b, 0xa908 },
	{ 0x005e, 0x28eb }, { 0x0062, 0x0023 }, { 0x0042, 0x60c2 },
	{ 0x0069, 0xff00 }
};

/*
 * The RTL8125's PHY settings after its reset, in order, without the PHY
 * microcode patch that the reference loads first; the PATCH steps
 * bracket the settings the PHY takes only in its patch mode.
 *
 * Procedure and values referenced: FreeBSD sys/dev/rge/if_rge_hw.c, main
 * 8fa55fdcae4ecbd2ec13ef91cc9d172095e21189 (rge_phy_config_mac_r25).
 */
static const struct rtl_phy_step rtl_phy_8125[] = {
	{ RTL_PHY_STEP_MODIFY, 0xad4e, 0x0000, 0x0010 },
	{ RTL_PHY_STEP_MODIFY, 0xad16, 0x03ff, 0x03ff },
	{ RTL_PHY_STEP_MODIFY, 0xad32, 0x003f, 0x0006 },
	{ RTL_PHY_STEP_MODIFY, 0xac08, 0x1000, 0x0000 },
	{ RTL_PHY_STEP_MODIFY, 0xac08, 0x0100, 0x0000 },
	{ RTL_PHY_STEP_MODIFY, 0xacc0, 0x0003, 0x0002 },
	{ RTL_PHY_STEP_MODIFY, 0xad40, 0x00e0, 0x0040 },
	{ RTL_PHY_STEP_MODIFY, 0xad40, 0x0007, 0x0004 },
	{ RTL_PHY_STEP_MODIFY, 0xac14, 0x0080, 0x0000 },
	{ RTL_PHY_STEP_MODIFY, 0xac80, 0x0300, 0x0000 },
	{ RTL_PHY_STEP_MODIFY, 0xac5e, 0x0007, 0x0002 },
	{ RTL_PHY_STEP_WRITE, 0xad4c, 0x0000, 0x00a8 },
	{ RTL_PHY_STEP_WRITE, 0xac5c, 0x0000, 0x01ff },
	{ RTL_PHY_STEP_MODIFY, 0xac8a, 0x00f0, 0x0030 },
	{ RTL_PHY_STEP_WRITE, 0xb87c, 0x0000, 0x8157 },
	{ RTL_PHY_STEP_MODIFY, 0xb87e, 0xff00, 0x0500 },
	{ RTL_PHY_STEP_WRITE, 0xb87c, 0x0000, 0x8159 },
	{ RTL_PHY_STEP_MODIFY, 0xb87e, 0xff00, 0x0700 },
	{ RTL_PHY_STEP_WRITE, 0xb87c, 0x0000, 0x80a2 },
	{ RTL_PHY_STEP_WRITE, 0xb87e, 0x0000, 0x0153 },
	{ RTL_PHY_STEP_WRITE, 0xb87c, 0x0000, 0x809c },
	{ RTL_PHY_STEP_WRITE, 0xb87e, 0x0000, 0x0153 },
	{ RTL_PHY_STEP_WRITE, 0xa436, 0x0000, 0x81b3 },
	{ RTL_PHY_STEP_WRITE, 0xa438, 0x0000, 0x0043 },
	{ RTL_PHY_STEP_WRITE, 0xa438, 0x0000, 0x00a7 },
	{ RTL_PHY_STEP_WRITE, 0xa438, 0x0000, 0x00d6 },
	{ RTL_PHY_STEP_WRITE, 0xa438, 0x0000, 0x00ec },
	{ RTL_PHY_STEP_WRITE, 0xa438, 0x0000, 0x00f6 },
	{ RTL_PHY_STEP_WRITE, 0xa438, 0x0000, 0x00fb },
	{ RTL_PHY_STEP_WRITE, 0xa438, 0x0000, 0x00fd },
	{ RTL_PHY_STEP_WRITE, 0xa438, 0x0000, 0x00ff },
	{ RTL_PHY_STEP_WRITE, 0xa438, 0x0000, 0x00bb },
	{ RTL_PHY_STEP_WRITE, 0xa438, 0x0000, 0x0058 },
	{ RTL_PHY_STEP_WRITE, 0xa438, 0x0000, 0x0029 },
	{ RTL_PHY_STEP_WRITE, 0xa438, 0x0000, 0x0013 },
	{ RTL_PHY_STEP_WRITE, 0xa438, 0x0000, 0x0009 },
	{ RTL_PHY_STEP_WRITE, 0xa438, 0x0000, 0x0004 },
	{ RTL_PHY_STEP_WRITE, 0xa438, 0x0000, 0x0002 },
	{ RTL_PHY_STEP_WRITE, 0xa438, 0x0000, 0x0000 },
	{ RTL_PHY_STEP_WRITE, 0xa438, 0x0000, 0x0000 },
	{ RTL_PHY_STEP_WRITE, 0xa438, 0x0000, 0x0000 },
	{ RTL_PHY_STEP_WRITE, 0xa438, 0x0000, 0x0000 },
	{ RTL_PHY_STEP_WRITE, 0xa438, 0x0000, 0x0000 },
	{ RTL_PHY_STEP_WRITE, 0xa438, 0x0000, 0x0000 },
	{ RTL_PHY_STEP_WRITE, 0xa438, 0x0000, 0x0000 },
	{ RTL_PHY_STEP_WRITE, 0xa438, 0x0000, 0x0000 },
	{ RTL_PHY_STEP_WRITE, 0xa438, 0x0000, 0x0000 },
	{ RTL_PHY_STEP_WRITE, 0xa438, 0x0000, 0x0000 },
	{ RTL_PHY_STEP_WRITE, 0xa438, 0x0000, 0x0000 },
	{ RTL_PHY_STEP_WRITE, 0xa438, 0x0000, 0x0000 },
	{ RTL_PHY_STEP_WRITE, 0xa438, 0x0000, 0x0000 },
	{ RTL_PHY_STEP_WRITE, 0xa438, 0x0000, 0x0000 },
	{ RTL_PHY_STEP_WRITE, 0xa438, 0x0000, 0x0000 },
	{ RTL_PHY_STEP_WRITE, 0xa438, 0x0000, 0x0000 },
	{ RTL_PHY_STEP_WRITE, 0xa438, 0x0000, 0x0000 },
	{ RTL_PHY_STEP_WRITE, 0xa438, 0x0000, 0x0000 },
	{ RTL_PHY_STEP_WRITE, 0xa438, 0x0000, 0x0000 },
	{ RTL_PHY_STEP_WRITE, 0xa438, 0x0000, 0x0000 },
	{ RTL_PHY_STEP_WRITE, 0xa438, 0x0000, 0x0000 },
	{ RTL_PHY_STEP_WRITE, 0xa438, 0x0000, 0x0000 },
	{ RTL_PHY_STEP_WRITE, 0xa438, 0x0000, 0x0000 },
	{ RTL_PHY_STEP_WRITE, 0xa438, 0x0000, 0x0000 },
	{ RTL_PHY_STEP_WRITE, 0xa438, 0x0000, 0x0000 },
	{ RTL_PHY_STEP_WRITE, 0xa438, 0x0000, 0x0000 },
	{ RTL_PHY_STEP_WRITE, 0xa436, 0x0000, 0x8257 },
	{ RTL_PHY_STEP_WRITE, 0xa438, 0x0000, 0x020f },
	{ RTL_PHY_STEP_WRITE, 0xa436, 0x0000, 0x80ea },
	{ RTL_PHY_STEP_WRITE, 0xa438, 0x0000, 0x7843 },
	{ RTL_PHY_STEP_PATCH, 0x0000, 0x0000, 0x0001 },
	{ RTL_PHY_STEP_MODIFY, 0xb896, 0x0001, 0x0000 },
	{ RTL_PHY_STEP_MODIFY, 0xb892, 0xff00, 0x0000 },
	{ RTL_PHY_STEP_WRITE, 0xb88e, 0x0000, 0xc091 },
	{ RTL_PHY_STEP_WRITE, 0xb890, 0x0000, 0x6e12 },
	{ RTL_PHY_STEP_WRITE, 0xb88e, 0x0000, 0xc092 },
	{ RTL_PHY_STEP_WRITE, 0xb890, 0x0000, 0x1214 },
	{ RTL_PHY_STEP_WRITE, 0xb88e, 0x0000, 0xc094 },
	{ RTL_PHY_STEP_WRITE, 0xb890, 0x0000, 0x1516 },
	{ RTL_PHY_STEP_WRITE, 0xb88e, 0x0000, 0xc096 },
	{ RTL_PHY_STEP_WRITE, 0xb890, 0x0000, 0x171b },
	{ RTL_PHY_STEP_WRITE, 0xb88e, 0x0000, 0xc098 },
	{ RTL_PHY_STEP_WRITE, 0xb890, 0x0000, 0x1b1c },
	{ RTL_PHY_STEP_WRITE, 0xb88e, 0x0000, 0xc09a },
	{ RTL_PHY_STEP_WRITE, 0xb890, 0x0000, 0x1f1f },
	{ RTL_PHY_STEP_WRITE, 0xb88e, 0x0000, 0xc09c },
	{ RTL_PHY_STEP_WRITE, 0xb890, 0x0000, 0x2021 },
	{ RTL_PHY_STEP_WRITE, 0xb88e, 0x0000, 0xc09e },
	{ RTL_PHY_STEP_WRITE, 0xb890, 0x0000, 0x2224 },
	{ RTL_PHY_STEP_WRITE, 0xb88e, 0x0000, 0xc0a0 },
	{ RTL_PHY_STEP_WRITE, 0xb890, 0x0000, 0x2424 },
	{ RTL_PHY_STEP_WRITE, 0xb88e, 0x0000, 0xc0a2 },
	{ RTL_PHY_STEP_WRITE, 0xb890, 0x0000, 0x2424 },
	{ RTL_PHY_STEP_WRITE, 0xb88e, 0x0000, 0xc0a4 },
	{ RTL_PHY_STEP_WRITE, 0xb890, 0x0000, 0x2424 },
	{ RTL_PHY_STEP_WRITE, 0xb88e, 0x0000, 0xc018 },
	{ RTL_PHY_STEP_WRITE, 0xb890, 0x0000, 0x0af2 },
	{ RTL_PHY_STEP_WRITE, 0xb88e, 0x0000, 0xc01a },
	{ RTL_PHY_STEP_WRITE, 0xb890, 0x0000, 0x0d4a },
	{ RTL_PHY_STEP_WRITE, 0xb88e, 0x0000, 0xc01c },
	{ RTL_PHY_STEP_WRITE, 0xb890, 0x0000, 0x0f26 },
	{ RTL_PHY_STEP_WRITE, 0xb88e, 0x0000, 0xc01e },
	{ RTL_PHY_STEP_WRITE, 0xb890, 0x0000, 0x118d },
	{ RTL_PHY_STEP_WRITE, 0xb88e, 0x0000, 0xc020 },
	{ RTL_PHY_STEP_WRITE, 0xb890, 0x0000, 0x14f3 },
	{ RTL_PHY_STEP_WRITE, 0xb88e, 0x0000, 0xc022 },
	{ RTL_PHY_STEP_WRITE, 0xb890, 0x0000, 0x175a },
	{ RTL_PHY_STEP_WRITE, 0xb88e, 0x0000, 0xc024 },
	{ RTL_PHY_STEP_WRITE, 0xb890, 0x0000, 0x19c0 },
	{ RTL_PHY_STEP_WRITE, 0xb88e, 0x0000, 0xc026 },
	{ RTL_PHY_STEP_WRITE, 0xb890, 0x0000, 0x1c26 },
	{ RTL_PHY_STEP_WRITE, 0xb88e, 0x0000, 0xc089 },
	{ RTL_PHY_STEP_WRITE, 0xb890, 0x0000, 0x6050 },
	{ RTL_PHY_STEP_WRITE, 0xb88e, 0x0000, 0xc08a },
	{ RTL_PHY_STEP_WRITE, 0xb890, 0x0000, 0x5f6e },
	{ RTL_PHY_STEP_WRITE, 0xb88e, 0x0000, 0xc08c },
	{ RTL_PHY_STEP_WRITE, 0xb890, 0x0000, 0x6e6e },
	{ RTL_PHY_STEP_WRITE, 0xb88e, 0x0000, 0xc08e },
	{ RTL_PHY_STEP_WRITE, 0xb890, 0x0000, 0x6e6e },
	{ RTL_PHY_STEP_WRITE, 0xb88e, 0x0000, 0xc090 },
	{ RTL_PHY_STEP_WRITE, 0xb890, 0x0000, 0x6e12 },
	{ RTL_PHY_STEP_MODIFY, 0xb896, 0x0000, 0x0001 },
	{ RTL_PHY_STEP_PATCH, 0x0000, 0x0000, 0x0000 },
	{ RTL_PHY_STEP_MODIFY, 0xd068, 0x0000, 0x2000 },
	{ RTL_PHY_STEP_WRITE, 0xa436, 0x0000, 0x81a2 },
	{ RTL_PHY_STEP_MODIFY, 0xa438, 0x0000, 0x0100 },
	{ RTL_PHY_STEP_MODIFY, 0xb54c, 0xff00, 0xdb00 },
	{ RTL_PHY_STEP_MODIFY, 0xa454, 0x0001, 0x0000 },
	{ RTL_PHY_STEP_MODIFY, 0xa5d4, 0x0000, 0x0020 },
	{ RTL_PHY_STEP_MODIFY, 0xad4e, 0x0010, 0x0000 },
	{ RTL_PHY_STEP_MODIFY, 0xa86a, 0x0001, 0x0000 },
	{ RTL_PHY_STEP_MODIFY, 0xa442, 0x0000, 0x0800 },
	{ RTL_PHY_STEP_MODIFY, 0xa424, 0x0000, 0x0008 },
};

/*
 * The RTL8125B's PHY settings after its reset, in order, without the PHY
 * microcode patch that the reference loads first.
 *
 * Procedure and values referenced: FreeBSD sys/dev/rge/if_rge_hw.c, main
 * 8fa55fdcae4ecbd2ec13ef91cc9d172095e21189 (rge_phy_config_mac_r25b).
 */
static const struct rtl_phy_step rtl_phy_8125b[] = {
	{ RTL_PHY_STEP_MODIFY, 0xa442, 0x0000, 0x0800 },
	{ RTL_PHY_STEP_MODIFY, 0xac46, 0x00f0, 0x0090 },
	{ RTL_PHY_STEP_MODIFY, 0xad30, 0x0003, 0x0001 },
	{ RTL_PHY_STEP_WRITE, 0xb87c, 0x0000, 0x80f5 },
	{ RTL_PHY_STEP_WRITE, 0xb87e, 0x0000, 0x760e },
	{ RTL_PHY_STEP_WRITE, 0xb87c, 0x0000, 0x8107 },
	{ RTL_PHY_STEP_WRITE, 0xb87e, 0x0000, 0x360e },
	{ RTL_PHY_STEP_WRITE, 0xb87c, 0x0000, 0x8551 },
	{ RTL_PHY_STEP_MODIFY, 0xb87e, 0xff00, 0x0800 },
	{ RTL_PHY_STEP_MODIFY, 0xbf00, 0xe000, 0xa000 },
	{ RTL_PHY_STEP_MODIFY, 0xbf46, 0x0f00, 0x0300 },
	{ RTL_PHY_STEP_WRITE, 0xa436, 0x0000, 0x8044 },
	{ RTL_PHY_STEP_WRITE, 0xa438, 0x0000, 0x2417 },
	{ RTL_PHY_STEP_WRITE, 0xa436, 0x0000, 0x804a },
	{ RTL_PHY_STEP_WRITE, 0xa438, 0x0000, 0x2417 },
	{ RTL_PHY_STEP_WRITE, 0xa436, 0x0000, 0x8050 },
	{ RTL_PHY_STEP_WRITE, 0xa438, 0x0000, 0x2417 },
	{ RTL_PHY_STEP_WRITE, 0xa436, 0x0000, 0x8056 },
	{ RTL_PHY_STEP_WRITE, 0xa438, 0x0000, 0x2417 },
	{ RTL_PHY_STEP_WRITE, 0xa436, 0x0000, 0x805c },
	{ RTL_PHY_STEP_WRITE, 0xa438, 0x0000, 0x2417 },
	{ RTL_PHY_STEP_WRITE, 0xa436, 0x0000, 0x8062 },
	{ RTL_PHY_STEP_WRITE, 0xa438, 0x0000, 0x2417 },
	{ RTL_PHY_STEP_WRITE, 0xa436, 0x0000, 0x8068 },
	{ RTL_PHY_STEP_WRITE, 0xa438, 0x0000, 0x2417 },
	{ RTL_PHY_STEP_WRITE, 0xa436, 0x0000, 0x806e },
	{ RTL_PHY_STEP_WRITE, 0xa438, 0x0000, 0x2417 },
	{ RTL_PHY_STEP_WRITE, 0xa436, 0x0000, 0x8074 },
	{ RTL_PHY_STEP_WRITE, 0xa438, 0x0000, 0x2417 },
	{ RTL_PHY_STEP_WRITE, 0xa436, 0x0000, 0x807a },
	{ RTL_PHY_STEP_WRITE, 0xa438, 0x0000, 0x2417 },
	{ RTL_PHY_STEP_MODIFY, 0xa4ca, 0x0000, 0x0040 },
	{ RTL_PHY_STEP_MODIFY, 0xbf84, 0xe000, 0xa000 },
	{ RTL_PHY_STEP_WRITE, 0xa436, 0x0000, 0x8170 },
	{ RTL_PHY_STEP_MODIFY, 0xa438, 0x2700, 0xd800 },
	{ RTL_PHY_STEP_MODIFY, 0xa424, 0x0000, 0x0008 },
};

/*
 * The RTL8125D's (first revision) PHY settings after its reset, in order,
 * without the PHY microcode patch that the reference loads first; the
 * PATCH steps bracket the settings the PHY takes only in its patch mode.
 *
 * Procedure and values referenced: FreeBSD sys/dev/rge/if_rge_hw.c, main
 * 8fa55fdcae4ecbd2ec13ef91cc9d172095e21189 (rge_phy_config_mac_r25d_1).
 */
static const struct rtl_phy_step rtl_phy_8125d_1[] = {
	{ RTL_PHY_STEP_MODIFY, 0xa442, 0x0000, 0x0800 },
	{ RTL_PHY_STEP_PATCH, 0x0000, 0x0000, 0x0001 },
	{ RTL_PHY_STEP_MODIFY, 0xbf96, 0x0000, 0x8000 },
	{ RTL_PHY_STEP_MODIFY, 0xbf94, 0x0007, 0x0005 },
	{ RTL_PHY_STEP_MODIFY, 0xbf8e, 0x3c00, 0x2800 },
	{ RTL_PHY_STEP_MODIFY, 0xbcd8, 0xc000, 0x4000 },
	{ RTL_PHY_STEP_MODIFY, 0xbcd8, 0x0000, 0xc000 },
	{ RTL_PHY_STEP_MODIFY, 0xbcd8, 0xc000, 0x4000 },
	{ RTL_PHY_STEP_MODIFY, 0xbc80, 0x001f, 0x0004 },
	{ RTL_PHY_STEP_MODIFY, 0xbc82, 0x0000, 0xe000 },
	{ RTL_PHY_STEP_MODIFY, 0xbc82, 0x0000, 0x1c00 },
	{ RTL_PHY_STEP_MODIFY, 0xbc80, 0x001f, 0x0005 },
	{ RTL_PHY_STEP_MODIFY, 0xbc82, 0x00e0, 0x0040 },
	{ RTL_PHY_STEP_MODIFY, 0xbc82, 0x0000, 0x001c },
	{ RTL_PHY_STEP_MODIFY, 0xbcd8, 0xc000, 0x0000 },
	{ RTL_PHY_STEP_MODIFY, 0xbcd8, 0xc000, 0x8000 },
	{ RTL_PHY_STEP_MODIFY, 0xbcd8, 0xc000, 0x0000 },
	{ RTL_PHY_STEP_MODIFY, 0xbd70, 0x0100, 0x0000 },
	{ RTL_PHY_STEP_MODIFY, 0xa466, 0x0000, 0x0002 },
	{ RTL_PHY_STEP_WRITE, 0xa436, 0x0000, 0x836a },
	{ RTL_PHY_STEP_MODIFY, 0xa438, 0xff00, 0x0000 },
	{ RTL_PHY_STEP_PATCH, 0x0000, 0x0000, 0x0000 },
	{ RTL_PHY_STEP_WRITE, 0xb87c, 0x0000, 0x832c },
	{ RTL_PHY_STEP_MODIFY, 0xb87e, 0xff00, 0x0500 },
	{ RTL_PHY_STEP_MODIFY, 0xb106, 0x0700, 0x0100 },
	{ RTL_PHY_STEP_MODIFY, 0xb206, 0x0700, 0x0200 },
	{ RTL_PHY_STEP_MODIFY, 0xb306, 0x0700, 0x0300 },
	{ RTL_PHY_STEP_WRITE, 0xb87c, 0x0000, 0x80cb },
	{ RTL_PHY_STEP_MODIFY, 0xb87e, 0xff00, 0x0300 },
	{ RTL_PHY_STEP_WRITE, 0xbcf4, 0x0000, 0x0000 },
	{ RTL_PHY_STEP_WRITE, 0xbcf6, 0x0000, 0x0000 },
	{ RTL_PHY_STEP_WRITE, 0xbc12, 0x0000, 0x0000 },
	{ RTL_PHY_STEP_WRITE, 0xb87c, 0x0000, 0x844d },
	{ RTL_PHY_STEP_MODIFY, 0xb87e, 0xff00, 0x0200 },
	{ RTL_PHY_STEP_WRITE, 0xb87c, 0x0000, 0x8feb },
	{ RTL_PHY_STEP_MODIFY, 0xb87e, 0xff00, 0x0100 },
	{ RTL_PHY_STEP_WRITE, 0xb87c, 0x0000, 0x8fe9 },
	{ RTL_PHY_STEP_MODIFY, 0xb87e, 0xff00, 0x0600 },
	{ RTL_PHY_STEP_MODIFY, 0xac7e, 0x01fc, 0x00b4 },
	{ RTL_PHY_STEP_WRITE, 0xb87c, 0x0000, 0x8105 },
	{ RTL_PHY_STEP_MODIFY, 0xb87e, 0xff00, 0x7a00 },
	{ RTL_PHY_STEP_WRITE, 0xb87c, 0x0000, 0x8117 },
	{ RTL_PHY_STEP_MODIFY, 0xb87e, 0xff00, 0x3a00 },
	{ RTL_PHY_STEP_WRITE, 0xb87c, 0x0000, 0x8103 },
	{ RTL_PHY_STEP_MODIFY, 0xb87e, 0xff00, 0x7400 },
	{ RTL_PHY_STEP_WRITE, 0xb87c, 0x0000, 0x8115 },
	{ RTL_PHY_STEP_MODIFY, 0xb87e, 0xff00, 0x3400 },
	{ RTL_PHY_STEP_MODIFY, 0xad40, 0x0030, 0x0000 },
	{ RTL_PHY_STEP_MODIFY, 0xad66, 0x000f, 0x0007 },
	{ RTL_PHY_STEP_MODIFY, 0xad68, 0xf000, 0x8000 },
	{ RTL_PHY_STEP_MODIFY, 0xad68, 0x0f00, 0x0500 },
	{ RTL_PHY_STEP_MODIFY, 0xad68, 0x000f, 0x0002 },
	{ RTL_PHY_STEP_MODIFY, 0xad6a, 0xf000, 0x7000 },
	{ RTL_PHY_STEP_WRITE, 0xac50, 0x0000, 0x01e8 },
	{ RTL_PHY_STEP_WRITE, 0xa436, 0x0000, 0x81fa },
	{ RTL_PHY_STEP_MODIFY, 0xa438, 0xff00, 0x5400 },
	{ RTL_PHY_STEP_MODIFY, 0xa864, 0x00f0, 0x00c0 },
	{ RTL_PHY_STEP_MODIFY, 0xa42c, 0x00ff, 0x0002 },
	{ RTL_PHY_STEP_WRITE, 0xa436, 0x0000, 0x80e1 },
	{ RTL_PHY_STEP_MODIFY, 0xa438, 0xff00, 0x0f00 },
	{ RTL_PHY_STEP_WRITE, 0xa436, 0x0000, 0x80de },
	{ RTL_PHY_STEP_MODIFY, 0xa438, 0xf000, 0x0700 },
	{ RTL_PHY_STEP_MODIFY, 0xa846, 0x0000, 0x0080 },
	{ RTL_PHY_STEP_WRITE, 0xa436, 0x0000, 0x80ba },
	{ RTL_PHY_STEP_WRITE, 0xa438, 0x0000, 0x8a04 },
	{ RTL_PHY_STEP_WRITE, 0xa436, 0x0000, 0x80bd },
	{ RTL_PHY_STEP_MODIFY, 0xa438, 0xff00, 0xca00 },
	{ RTL_PHY_STEP_WRITE, 0xa436, 0x0000, 0x80b7 },
	{ RTL_PHY_STEP_MODIFY, 0xa438, 0xff00, 0xb300 },
	{ RTL_PHY_STEP_WRITE, 0xa436, 0x0000, 0x80ce },
	{ RTL_PHY_STEP_WRITE, 0xa438, 0x0000, 0x8a04 },
	{ RTL_PHY_STEP_WRITE, 0xa436, 0x0000, 0x80d1 },
	{ RTL_PHY_STEP_MODIFY, 0xa438, 0xff00, 0xca00 },
	{ RTL_PHY_STEP_WRITE, 0xa436, 0x0000, 0x80cb },
	{ RTL_PHY_STEP_MODIFY, 0xa438, 0xff00, 0xbb00 },
	{ RTL_PHY_STEP_WRITE, 0xa436, 0x0000, 0x80a6 },
	{ RTL_PHY_STEP_WRITE, 0xa438, 0x0000, 0x4909 },
	{ RTL_PHY_STEP_WRITE, 0xa436, 0x0000, 0x80a8 },
	{ RTL_PHY_STEP_WRITE, 0xa438, 0x0000, 0x05b8 },
	{ RTL_PHY_STEP_WRITE, 0xa436, 0x0000, 0x8200 },
	{ RTL_PHY_STEP_MODIFY, 0xa438, 0xff00, 0x5800 },
	{ RTL_PHY_STEP_WRITE, 0xa436, 0x0000, 0x8ff1 },
	{ RTL_PHY_STEP_WRITE, 0xa438, 0x0000, 0x7078 },
	{ RTL_PHY_STEP_WRITE, 0xa436, 0x0000, 0x8ff3 },
	{ RTL_PHY_STEP_WRITE, 0xa438, 0x0000, 0x5d78 },
	{ RTL_PHY_STEP_WRITE, 0xa436, 0x0000, 0x8ff5 },
	{ RTL_PHY_STEP_WRITE, 0xa438, 0x0000, 0x7862 },
	{ RTL_PHY_STEP_WRITE, 0xa436, 0x0000, 0x8ff7 },
	{ RTL_PHY_STEP_MODIFY, 0xa438, 0xff00, 0x1400 },
	{ RTL_PHY_STEP_WRITE, 0xa436, 0x0000, 0x814c },
	{ RTL_PHY_STEP_WRITE, 0xa438, 0x0000, 0x8455 },
	{ RTL_PHY_STEP_WRITE, 0xa436, 0x0000, 0x814e },
	{ RTL_PHY_STEP_WRITE, 0xa438, 0x0000, 0x84a6 },
	{ RTL_PHY_STEP_WRITE, 0xa436, 0x0000, 0x8163 },
	{ RTL_PHY_STEP_MODIFY, 0xa438, 0xff00, 0x0600 },
	{ RTL_PHY_STEP_WRITE, 0xa436, 0x0000, 0x816a },
	{ RTL_PHY_STEP_MODIFY, 0xa438, 0xff00, 0x0500 },
	{ RTL_PHY_STEP_WRITE, 0xa436, 0x0000, 0x8171 },
	{ RTL_PHY_STEP_MODIFY, 0xa438, 0xff00, 0x1f00 },
	{ RTL_PHY_STEP_MODIFY, 0xbc3a, 0x000f, 0x0006 },
	{ RTL_PHY_STEP_WRITE, 0xa436, 0x0000, 0x8064 },
	{ RTL_PHY_STEP_MODIFY, 0xa438, 0x0700, 0x0000 },
	{ RTL_PHY_STEP_WRITE, 0xa436, 0x0000, 0x8067 },
	{ RTL_PHY_STEP_MODIFY, 0xa438, 0x0700, 0x0000 },
	{ RTL_PHY_STEP_WRITE, 0xa436, 0x0000, 0x806a },
	{ RTL_PHY_STEP_MODIFY, 0xa438, 0x0700, 0x0000 },
	{ RTL_PHY_STEP_WRITE, 0xa436, 0x0000, 0x806d },
	{ RTL_PHY_STEP_MODIFY, 0xa438, 0x0700, 0x0000 },
	{ RTL_PHY_STEP_WRITE, 0xa436, 0x0000, 0x8070 },
	{ RTL_PHY_STEP_MODIFY, 0xa438, 0x0700, 0x0000 },
	{ RTL_PHY_STEP_WRITE, 0xa436, 0x0000, 0x8073 },
	{ RTL_PHY_STEP_MODIFY, 0xa438, 0x0700, 0x0000 },
	{ RTL_PHY_STEP_WRITE, 0xa436, 0x0000, 0x8076 },
	{ RTL_PHY_STEP_MODIFY, 0xa438, 0x0700, 0x0000 },
	{ RTL_PHY_STEP_WRITE, 0xa436, 0x0000, 0x8079 },
	{ RTL_PHY_STEP_MODIFY, 0xa438, 0x0700, 0x0000 },
	{ RTL_PHY_STEP_WRITE, 0xa436, 0x0000, 0x807c },
	{ RTL_PHY_STEP_MODIFY, 0xa438, 0x0700, 0x0000 },
	{ RTL_PHY_STEP_WRITE, 0xa436, 0x0000, 0x807f },
	{ RTL_PHY_STEP_MODIFY, 0xa438, 0x0700, 0x0000 },
	{ RTL_PHY_STEP_MODIFY, 0xbfa0, 0xff70, 0x5500 },
	{ RTL_PHY_STEP_WRITE, 0xbfa2, 0x0000, 0x9d00 },
	{ RTL_PHY_STEP_WRITE, 0xa436, 0x0000, 0x8165 },
	{ RTL_PHY_STEP_MODIFY, 0xa438, 0x0700, 0x0200 },
	{ RTL_PHY_STEP_WRITE, 0xa436, 0x0000, 0x8019 },
	{ RTL_PHY_STEP_MODIFY, 0xa438, 0x0000, 0x0100 },
	{ RTL_PHY_STEP_WRITE, 0xa436, 0x0000, 0x8fe3 },
	{ RTL_PHY_STEP_WRITE, 0xa438, 0x0000, 0x0005 },
	{ RTL_PHY_STEP_WRITE, 0xa438, 0x0000, 0x0000 },
	{ RTL_PHY_STEP_WRITE, 0xa438, 0x0000, 0x00ed },
	{ RTL_PHY_STEP_WRITE, 0xa438, 0x0000, 0x0502 },
	{ RTL_PHY_STEP_WRITE, 0xa438, 0x0000, 0x0b00 },
	{ RTL_PHY_STEP_WRITE, 0xa438, 0x0000, 0xd401 },
	{ RTL_PHY_STEP_MODIFY, 0xa438, 0xff00, 0x2900 },
	{ RTL_PHY_STEP_WRITE, 0xa436, 0x0000, 0x8018 },
	{ RTL_PHY_STEP_MODIFY, 0xa438, 0xff00, 0x1700 },
	{ RTL_PHY_STEP_WRITE, 0xa436, 0x0000, 0x815b },
	{ RTL_PHY_STEP_MODIFY, 0xa438, 0xff00, 0x1700 },
	{ RTL_PHY_STEP_MODIFY, 0xa4e0, 0x8000, 0x0000 },
	{ RTL_PHY_STEP_MODIFY, 0xa5d4, 0x0020, 0x0000 },
	{ RTL_PHY_STEP_MODIFY, 0xa654, 0x0800, 0x0000 },
	{ RTL_PHY_STEP_MODIFY, 0xa430, 0x0000, 0x1001 },
	{ RTL_PHY_STEP_MODIFY, 0xa442, 0x0000, 0x0080 },
};

/*
 * The RTL8125D's (second revision) PHY settings after its reset, in order,
 * without the PHY microcode patch that the reference loads first; the
 * PATCH steps bracket the settings the PHY takes only in its patch mode.
 *
 * Procedure and values referenced: FreeBSD sys/dev/rge/if_rge_hw.c, main
 * 8fa55fdcae4ecbd2ec13ef91cc9d172095e21189 (rge_phy_config_mac_r25d_2).
 */
static const struct rtl_phy_step rtl_phy_8125d_2[] = {
	{ RTL_PHY_STEP_MODIFY, 0xa442, 0x0000, 0x0800 },
	{ RTL_PHY_STEP_PATCH, 0x0000, 0x0000, 0x0001 },
	{ RTL_PHY_STEP_MODIFY, 0xbcd8, 0xc000, 0x4000 },
	{ RTL_PHY_STEP_MODIFY, 0xbcd8, 0x0000, 0xc000 },
	{ RTL_PHY_STEP_MODIFY, 0xbcd8, 0xc000, 0x4000 },
	{ RTL_PHY_STEP_MODIFY, 0xbc80, 0x001f, 0x0004 },
	{ RTL_PHY_STEP_MODIFY, 0xbc82, 0x0000, 0xe000 },
	{ RTL_PHY_STEP_MODIFY, 0xbc82, 0x0000, 0x1c00 },
	{ RTL_PHY_STEP_MODIFY, 0xbc80, 0x001f, 0x0005 },
	{ RTL_PHY_STEP_MODIFY, 0xbc82, 0x00e0, 0x0040 },
	{ RTL_PHY_STEP_MODIFY, 0xbc82, 0x0000, 0x001c },
	{ RTL_PHY_STEP_MODIFY, 0xbcd8, 0xc000, 0x0000 },
	{ RTL_PHY_STEP_MODIFY, 0xbcd8, 0xc000, 0x8000 },
	{ RTL_PHY_STEP_MODIFY, 0xbcd8, 0xc000, 0x0000 },
	{ RTL_PHY_STEP_PATCH, 0x0000, 0x0000, 0x0000 },
	{ RTL_PHY_STEP_MODIFY, 0xac7e, 0x01fc, 0x00b4 },
	{ RTL_PHY_STEP_WRITE, 0xb87c, 0x0000, 0x8105 },
	{ RTL_PHY_STEP_MODIFY, 0xb87e, 0xff00, 0x7a00 },
	{ RTL_PHY_STEP_WRITE, 0xb87c, 0x0000, 0x8117 },
	{ RTL_PHY_STEP_MODIFY, 0xb87e, 0xff00, 0x3a00 },
	{ RTL_PHY_STEP_WRITE, 0xb87c, 0x0000, 0x8103 },
	{ RTL_PHY_STEP_MODIFY, 0xb87e, 0xff00, 0x7400 },
	{ RTL_PHY_STEP_WRITE, 0xb87c, 0x0000, 0x8115 },
	{ RTL_PHY_STEP_MODIFY, 0xb87e, 0xff00, 0x3400 },
	{ RTL_PHY_STEP_WRITE, 0xb87c, 0x0000, 0x8feb },
	{ RTL_PHY_STEP_MODIFY, 0xb87e, 0xff00, 0x0500 },
	{ RTL_PHY_STEP_WRITE, 0xb87c, 0x0000, 0x8fea },
	{ RTL_PHY_STEP_MODIFY, 0xb87e, 0xff00, 0x0700 },
	{ RTL_PHY_STEP_WRITE, 0xb87c, 0x0000, 0x80d6 },
	{ RTL_PHY_STEP_MODIFY, 0xb87e, 0xff00, 0xef00 },
	{ RTL_PHY_STEP_MODIFY, 0xa5d4, 0x0020, 0x0000 },
	{ RTL_PHY_STEP_MODIFY, 0xa654, 0x0800, 0x0000 },
	{ RTL_PHY_STEP_MODIFY, 0xa448, 0x0400, 0x0000 },
	{ RTL_PHY_STEP_MODIFY, 0xa586, 0x0400, 0x0000 },
	{ RTL_PHY_STEP_MODIFY, 0xa430, 0x0000, 0x1001 },
	{ RTL_PHY_STEP_MODIFY, 0xa442, 0x0000, 0x0080 },
};

/*
 * The RTL8126's (first revision) PHY settings after its reset, in order,
 * without the PHY microcode patch that the reference loads first; the
 * PATCH steps bracket the settings the PHY takes only in its patch mode.
 *
 * Procedure and values referenced: FreeBSD sys/dev/rge/if_rge_hw.c, main
 * 8fa55fdcae4ecbd2ec13ef91cc9d172095e21189 (rge_phy_config_mac_r26_1).
 */
static const struct rtl_phy_step rtl_phy_8126_1[] = {
	{ RTL_PHY_STEP_MODIFY, 0xa442, 0x0000, 0x0800 },
	{ RTL_PHY_STEP_WRITE, 0xa436, 0x0000, 0x80bf },
	{ RTL_PHY_STEP_MODIFY, 0xa438, 0xff00, 0xed00 },
	{ RTL_PHY_STEP_WRITE, 0xa436, 0x0000, 0x80cd },
	{ RTL_PHY_STEP_MODIFY, 0xa438, 0xff00, 0x1000 },
	{ RTL_PHY_STEP_WRITE, 0xa436, 0x0000, 0x80d1 },
	{ RTL_PHY_STEP_MODIFY, 0xa438, 0xff00, 0xc800 },
	{ RTL_PHY_STEP_WRITE, 0xa436, 0x0000, 0x80d4 },
	{ RTL_PHY_STEP_MODIFY, 0xa438, 0xff00, 0xc800 },
	{ RTL_PHY_STEP_WRITE, 0xa436, 0x0000, 0x80e1 },
	{ RTL_PHY_STEP_WRITE, 0xa438, 0x0000, 0x10cc },
	{ RTL_PHY_STEP_WRITE, 0xa436, 0x0000, 0x80e5 },
	{ RTL_PHY_STEP_WRITE, 0xa438, 0x0000, 0x4f0c },
	{ RTL_PHY_STEP_WRITE, 0xa436, 0x0000, 0x8387 },
	{ RTL_PHY_STEP_MODIFY, 0xa438, 0xff00, 0x4700 },
	{ RTL_PHY_STEP_MODIFY, 0xa80c, 0x00c0, 0x0080 },
	{ RTL_PHY_STEP_MODIFY, 0xac90, 0x0010, 0x0000 },
	{ RTL_PHY_STEP_MODIFY, 0xad2c, 0x8000, 0x0000 },
	{ RTL_PHY_STEP_WRITE, 0xb87c, 0x0000, 0x8321 },
	{ RTL_PHY_STEP_MODIFY, 0xb87e, 0xff00, 0x1100 },
	{ RTL_PHY_STEP_MODIFY, 0xacf8, 0x0000, 0x000c },
	{ RTL_PHY_STEP_WRITE, 0xa436, 0x0000, 0x8183 },
	{ RTL_PHY_STEP_MODIFY, 0xa438, 0xff00, 0x5900 },
	{ RTL_PHY_STEP_MODIFY, 0xad94, 0x0000, 0x0020 },
	{ RTL_PHY_STEP_MODIFY, 0xa654, 0x0800, 0x0000 },
	{ RTL_PHY_STEP_MODIFY, 0xb648, 0x0000, 0x4000 },
	{ RTL_PHY_STEP_WRITE, 0xb87c, 0x0000, 0x839e },
	{ RTL_PHY_STEP_MODIFY, 0xb87e, 0xff00, 0x2f00 },
	{ RTL_PHY_STEP_WRITE, 0xb87c, 0x0000, 0x83f2 },
	{ RTL_PHY_STEP_MODIFY, 0xb87e, 0xff00, 0x0800 },
	{ RTL_PHY_STEP_MODIFY, 0xada0, 0x0000, 0x0002 },
	{ RTL_PHY_STEP_WRITE, 0xb87c, 0x0000, 0x80f3 },
	{ RTL_PHY_STEP_MODIFY, 0xb87e, 0xff00, 0x9900 },
	{ RTL_PHY_STEP_WRITE, 0xb87c, 0x0000, 0x8126 },
	{ RTL_PHY_STEP_MODIFY, 0xb87e, 0xff00, 0xc100 },
	{ RTL_PHY_STEP_WRITE, 0xb87c, 0x0000, 0x893a },
	{ RTL_PHY_STEP_WRITE, 0xb87e, 0x0000, 0x8080 },
	{ RTL_PHY_STEP_WRITE, 0xb87c, 0x0000, 0x8647 },
	{ RTL_PHY_STEP_MODIFY, 0xb87e, 0xff00, 0xe600 },
	{ RTL_PHY_STEP_WRITE, 0xb87c, 0x0000, 0x862c },
	{ RTL_PHY_STEP_MODIFY, 0xb87e, 0xff00, 0x1200 },
	{ RTL_PHY_STEP_WRITE, 0xb87c, 0x0000, 0x864a },
	{ RTL_PHY_STEP_MODIFY, 0xb87e, 0xff00, 0xe600 },
	{ RTL_PHY_STEP_WRITE, 0xb87c, 0x0000, 0x80a0 },
	{ RTL_PHY_STEP_WRITE, 0xb87e, 0x0000, 0xbcbc },
	{ RTL_PHY_STEP_WRITE, 0xb87c, 0x0000, 0x805e },
	{ RTL_PHY_STEP_WRITE, 0xb87e, 0x0000, 0xbcbc },
	{ RTL_PHY_STEP_WRITE, 0xb87c, 0x0000, 0x8056 },
	{ RTL_PHY_STEP_WRITE, 0xb87e, 0x0000, 0x3077 },
	{ RTL_PHY_STEP_WRITE, 0xb87c, 0x0000, 0x8058 },
	{ RTL_PHY_STEP_MODIFY, 0xb87e, 0xff00, 0x5a00 },
	{ RTL_PHY_STEP_WRITE, 0xb87c, 0x0000, 0x8098 },
	{ RTL_PHY_STEP_WRITE, 0xb87e, 0x0000, 0x3077 },
	{ RTL_PHY_STEP_WRITE, 0xb87c, 0x0000, 0x809a },
	{ RTL_PHY_STEP_MODIFY, 0xb87e, 0xff00, 0x5a00 },
	{ RTL_PHY_STEP_WRITE, 0xb87c, 0x0000, 0x8052 },
	{ RTL_PHY_STEP_WRITE, 0xb87e, 0x0000, 0x3733 },
	{ RTL_PHY_STEP_WRITE, 0xb87c, 0x0000, 0x8094 },
	{ RTL_PHY_STEP_WRITE, 0xb87e, 0x0000, 0x3733 },
	{ RTL_PHY_STEP_WRITE, 0xb87c, 0x0000, 0x807f },
	{ RTL_PHY_STEP_WRITE, 0xb87e, 0x0000, 0x7c75 },
	{ RTL_PHY_STEP_WRITE, 0xb87c, 0x0000, 0x803d },
	{ RTL_PHY_STEP_WRITE, 0xb87e, 0x0000, 0x7c75 },
	{ RTL_PHY_STEP_WRITE, 0xb87c, 0x0000, 0x8036 },
	{ RTL_PHY_STEP_MODIFY, 0xb87e, 0xff00, 0x3000 },
	{ RTL_PHY_STEP_WRITE, 0xb87c, 0x0000, 0x8078 },
	{ RTL_PHY_STEP_MODIFY, 0xb87e, 0xff00, 0x3000 },
	{ RTL_PHY_STEP_WRITE, 0xb87c, 0x0000, 0x8031 },
	{ RTL_PHY_STEP_MODIFY, 0xb87e, 0xff00, 0x3300 },
	{ RTL_PHY_STEP_WRITE, 0xb87c, 0x0000, 0x8073 },
	{ RTL_PHY_STEP_MODIFY, 0xb87e, 0xff00, 0x3300 },
	{ RTL_PHY_STEP_MODIFY, 0xae06, 0xfc00, 0x7c00 },
	{ RTL_PHY_STEP_WRITE, 0xb87c, 0x0000, 0x89d1 },
	{ RTL_PHY_STEP_WRITE, 0xb87e, 0x0000, 0x0004 },
	{ RTL_PHY_STEP_WRITE, 0xa436, 0x0000, 0x8fbd },
	{ RTL_PHY_STEP_MODIFY, 0xa438, 0xff00, 0x0a00 },
	{ RTL_PHY_STEP_WRITE, 0xa436, 0x0000, 0x8fbe },
	{ RTL_PHY_STEP_WRITE, 0xa438, 0x0000, 0x0d09 },
	{ RTL_PHY_STEP_WRITE, 0xb87c, 0x0000, 0x89cd },
	{ RTL_PHY_STEP_WRITE, 0xb87e, 0x0000, 0x0f0f },
	{ RTL_PHY_STEP_WRITE, 0xb87c, 0x0000, 0x89cf },
	{ RTL_PHY_STEP_WRITE, 0xb87e, 0x0000, 0x0f0f },
	{ RTL_PHY_STEP_WRITE, 0xb87c, 0x0000, 0x83a4 },
	{ RTL_PHY_STEP_WRITE, 0xb87e, 0x0000, 0x6600 },
	{ RTL_PHY_STEP_WRITE, 0xb87c, 0x0000, 0x83a6 },
	{ RTL_PHY_STEP_WRITE, 0xb87e, 0x0000, 0x6601 },
	{ RTL_PHY_STEP_WRITE, 0xb87c, 0x0000, 0x83c0 },
	{ RTL_PHY_STEP_WRITE, 0xb87e, 0x0000, 0x6600 },
	{ RTL_PHY_STEP_WRITE, 0xb87c, 0x0000, 0x83c2 },
	{ RTL_PHY_STEP_WRITE, 0xb87e, 0x0000, 0x6601 },
	{ RTL_PHY_STEP_WRITE, 0xb87c, 0x0000, 0x8414 },
	{ RTL_PHY_STEP_WRITE, 0xb87e, 0x0000, 0x6600 },
	{ RTL_PHY_STEP_WRITE, 0xb87c, 0x0000, 0x8416 },
	{ RTL_PHY_STEP_WRITE, 0xb87e, 0x0000, 0x6601 },
	{ RTL_PHY_STEP_WRITE, 0xb87c, 0x0000, 0x83f8 },
	{ RTL_PHY_STEP_WRITE, 0xb87e, 0x0000, 0x6600 },
	{ RTL_PHY_STEP_WRITE, 0xb87c, 0x0000, 0x83fa },
	{ RTL_PHY_STEP_WRITE, 0xb87e, 0x0000, 0x6601 },
	{ RTL_PHY_STEP_PATCH, 0x0000, 0x0000, 0x0001 },
	{ RTL_PHY_STEP_MODIFY, 0xbd96, 0x1f00, 0x1000 },
	{ RTL_PHY_STEP_MODIFY, 0xbf1c, 0x0007, 0x0007 },
	{ RTL_PHY_STEP_MODIFY, 0xbfbe, 0x8000, 0x0000 },
	{ RTL_PHY_STEP_MODIFY, 0xbf40, 0x0380, 0x0280 },
	{ RTL_PHY_STEP_MODIFY, 0xbf90, 0x0080, 0x0060 },
	{ RTL_PHY_STEP_MODIFY, 0xbf90, 0x0010, 0x000c },
	{ RTL_PHY_STEP_PATCH, 0x0000, 0x0000, 0x0000 },
	{ RTL_PHY_STEP_WRITE, 0xa436, 0x0000, 0x843b },
	{ RTL_PHY_STEP_MODIFY, 0xa438, 0xff00, 0x2000 },
	{ RTL_PHY_STEP_WRITE, 0xa436, 0x0000, 0x843d },
	{ RTL_PHY_STEP_MODIFY, 0xa438, 0xff00, 0x2000 },
	{ RTL_PHY_STEP_MODIFY, 0xb516, 0x007f, 0x0000 },
	{ RTL_PHY_STEP_MODIFY, 0xbf80, 0x0030, 0x0000 },
	{ RTL_PHY_STEP_WRITE, 0xa436, 0x0000, 0x8188 },
	{ RTL_PHY_STEP_WRITE, 0xa438, 0x0000, 0x0044 },
	{ RTL_PHY_STEP_WRITE, 0xa438, 0x0000, 0x00a8 },
	{ RTL_PHY_STEP_WRITE, 0xa438, 0x0000, 0x00d6 },
	{ RTL_PHY_STEP_WRITE, 0xa438, 0x0000, 0x00ec },
	{ RTL_PHY_STEP_WRITE, 0xa438, 0x0000, 0x00f6 },
	{ RTL_PHY_STEP_WRITE, 0xa438, 0x0000, 0x00fc },
	{ RTL_PHY_STEP_WRITE, 0xa438, 0x0000, 0x00fe },
	{ RTL_PHY_STEP_WRITE, 0xa438, 0x0000, 0x00fe },
	{ RTL_PHY_STEP_WRITE, 0xa438, 0x0000, 0x00bc },
	{ RTL_PHY_STEP_WRITE, 0xa438, 0x0000, 0x0058 },
	{ RTL_PHY_STEP_WRITE, 0xa438, 0x0000, 0x002a },
	{ RTL_PHY_STEP_WRITE, 0xb87c, 0x0000, 0x8015 },
	{ RTL_PHY_STEP_MODIFY, 0xb87e, 0xff00, 0x0800 },
	{ RTL_PHY_STEP_WRITE, 0xb87c, 0x0000, 0x8ffd },
	{ RTL_PHY_STEP_MODIFY, 0xb87e, 0xff00, 0x0000 },
	{ RTL_PHY_STEP_WRITE, 0xb87c, 0x0000, 0x8fff },
	{ RTL_PHY_STEP_MODIFY, 0xb87e, 0xff00, 0x7f00 },
	{ RTL_PHY_STEP_WRITE, 0xb87c, 0x0000, 0x8ffb },
	{ RTL_PHY_STEP_MODIFY, 0xb87e, 0xff00, 0x0100 },
	{ RTL_PHY_STEP_WRITE, 0xb87c, 0x0000, 0x8fe9 },
	{ RTL_PHY_STEP_WRITE, 0xb87e, 0x0000, 0x0002 },
	{ RTL_PHY_STEP_WRITE, 0xb87c, 0x0000, 0x8fef },
	{ RTL_PHY_STEP_WRITE, 0xb87e, 0x0000, 0x00a5 },
	{ RTL_PHY_STEP_WRITE, 0xb87c, 0x0000, 0x8ff1 },
	{ RTL_PHY_STEP_WRITE, 0xb87e, 0x0000, 0x0106 },
	{ RTL_PHY_STEP_WRITE, 0xb87c, 0x0000, 0x8fe1 },
	{ RTL_PHY_STEP_WRITE, 0xb87e, 0x0000, 0x0102 },
	{ RTL_PHY_STEP_WRITE, 0xb87c, 0x0000, 0x8fe3 },
	{ RTL_PHY_STEP_MODIFY, 0xb87e, 0xff00, 0x0400 },
	{ RTL_PHY_STEP_MODIFY, 0xa654, 0x0000, 0x0800 },
	{ RTL_PHY_STEP_MODIFY, 0xa654, 0x0003, 0x0000 },
	{ RTL_PHY_STEP_WRITE, 0xac3a, 0x0000, 0x5851 },
	{ RTL_PHY_STEP_MODIFY, 0xac3c, 0xd000, 0x2000 },
	{ RTL_PHY_STEP_MODIFY, 0xac42, 0x0200, 0x01c0 },
	{ RTL_PHY_STEP_MODIFY, 0xac3e, 0xe000, 0x0000 },
	{ RTL_PHY_STEP_MODIFY, 0xac42, 0x0038, 0x0000 },
	{ RTL_PHY_STEP_MODIFY, 0xac42, 0x0002, 0x0005 },
	{ RTL_PHY_STEP_WRITE, 0xac1a, 0x0000, 0x00db },
	{ RTL_PHY_STEP_WRITE, 0xade4, 0x0000, 0x01b5 },
	{ RTL_PHY_STEP_MODIFY, 0xad9c, 0x0c00, 0x0000 },
	{ RTL_PHY_STEP_WRITE, 0xb87c, 0x0000, 0x814b },
	{ RTL_PHY_STEP_MODIFY, 0xb87e, 0xff00, 0x1100 },
	{ RTL_PHY_STEP_WRITE, 0xb87c, 0x0000, 0x814d },
	{ RTL_PHY_STEP_MODIFY, 0xb87e, 0xff00, 0x1100 },
	{ RTL_PHY_STEP_WRITE, 0xb87c, 0x0000, 0x814f },
	{ RTL_PHY_STEP_MODIFY, 0xb87e, 0xff00, 0x0b00 },
	{ RTL_PHY_STEP_WRITE, 0xb87c, 0x0000, 0x8142 },
	{ RTL_PHY_STEP_MODIFY, 0xb87e, 0xff00, 0x0100 },
	{ RTL_PHY_STEP_WRITE, 0xb87c, 0x0000, 0x8144 },
	{ RTL_PHY_STEP_MODIFY, 0xb87e, 0xff00, 0x0100 },
	{ RTL_PHY_STEP_WRITE, 0xb87c, 0x0000, 0x8150 },
	{ RTL_PHY_STEP_MODIFY, 0xb87e, 0xff00, 0x0100 },
	{ RTL_PHY_STEP_WRITE, 0xb87c, 0x0000, 0x8118 },
	{ RTL_PHY_STEP_MODIFY, 0xb87e, 0xff00, 0x0700 },
	{ RTL_PHY_STEP_WRITE, 0xb87c, 0x0000, 0x811a },
	{ RTL_PHY_STEP_MODIFY, 0xb87e, 0xff00, 0x0700 },
	{ RTL_PHY_STEP_WRITE, 0xb87c, 0x0000, 0x811c },
	{ RTL_PHY_STEP_MODIFY, 0xb87e, 0xff00, 0x0500 },
	{ RTL_PHY_STEP_WRITE, 0xb87c, 0x0000, 0x810f },
	{ RTL_PHY_STEP_MODIFY, 0xb87e, 0xff00, 0x0100 },
	{ RTL_PHY_STEP_WRITE, 0xb87c, 0x0000, 0x8111 },
	{ RTL_PHY_STEP_MODIFY, 0xb87e, 0xff00, 0x0100 },
	{ RTL_PHY_STEP_WRITE, 0xb87c, 0x0000, 0x811d },
	{ RTL_PHY_STEP_MODIFY, 0xb87e, 0xff00, 0x0100 },
	{ RTL_PHY_STEP_MODIFY, 0xac36, 0x0000, 0x1000 },
	{ RTL_PHY_STEP_MODIFY, 0xad1c, 0x0100, 0x0000 },
	{ RTL_PHY_STEP_MODIFY, 0xade8, 0xffc0, 0x1400 },
	{ RTL_PHY_STEP_WRITE, 0xb87c, 0x0000, 0x864b },
	{ RTL_PHY_STEP_MODIFY, 0xb87e, 0xff00, 0x9d00 },
	{ RTL_PHY_STEP_WRITE, 0xa436, 0x0000, 0x8f97 },
	{ RTL_PHY_STEP_WRITE, 0xa438, 0x0000, 0x003f },
	{ RTL_PHY_STEP_WRITE, 0xa438, 0x0000, 0x3f02 },
	{ RTL_PHY_STEP_WRITE, 0xa438, 0x0000, 0x023c },
	{ RTL_PHY_STEP_WRITE, 0xa438, 0x0000, 0x3b0a },
	{ RTL_PHY_STEP_WRITE, 0xa438, 0x0000, 0x1c00 },
	{ RTL_PHY_STEP_WRITE, 0xa438, 0x0000, 0x0000 },
	{ RTL_PHY_STEP_WRITE, 0xa438, 0x0000, 0x0000 },
	{ RTL_PHY_STEP_WRITE, 0xa438, 0x0000, 0x0000 },
	{ RTL_PHY_STEP_WRITE, 0xa438, 0x0000, 0x0000 },
	{ RTL_PHY_STEP_MODIFY, 0xad9c, 0x0000, 0x0020 },
	{ RTL_PHY_STEP_WRITE, 0xb87c, 0x0000, 0x8122 },
	{ RTL_PHY_STEP_MODIFY, 0xb87e, 0xff00, 0x0c00 },
	{ RTL_PHY_STEP_WRITE, 0xb87c, 0x0000, 0x82c8 },
	{ RTL_PHY_STEP_WRITE, 0xb87e, 0x0000, 0x03ed },
	{ RTL_PHY_STEP_WRITE, 0xb87e, 0x0000, 0x03ff },
	{ RTL_PHY_STEP_WRITE, 0xb87e, 0x0000, 0x0009 },
	{ RTL_PHY_STEP_WRITE, 0xb87e, 0x0000, 0x03fe },
	{ RTL_PHY_STEP_WRITE, 0xb87e, 0x0000, 0x000b },
	{ RTL_PHY_STEP_WRITE, 0xb87e, 0x0000, 0x0021 },
	{ RTL_PHY_STEP_WRITE, 0xb87e, 0x0000, 0x03f7 },
	{ RTL_PHY_STEP_WRITE, 0xb87e, 0x0000, 0x03b8 },
	{ RTL_PHY_STEP_WRITE, 0xb87e, 0x0000, 0x03e0 },
	{ RTL_PHY_STEP_WRITE, 0xb87e, 0x0000, 0x0049 },
	{ RTL_PHY_STEP_WRITE, 0xb87e, 0x0000, 0x0049 },
	{ RTL_PHY_STEP_WRITE, 0xb87e, 0x0000, 0x03e0 },
	{ RTL_PHY_STEP_WRITE, 0xb87e, 0x0000, 0x03b8 },
	{ RTL_PHY_STEP_WRITE, 0xb87e, 0x0000, 0x03f7 },
	{ RTL_PHY_STEP_WRITE, 0xb87e, 0x0000, 0x0021 },
	{ RTL_PHY_STEP_WRITE, 0xb87e, 0x0000, 0x000b },
	{ RTL_PHY_STEP_WRITE, 0xb87e, 0x0000, 0x03fe },
	{ RTL_PHY_STEP_WRITE, 0xb87e, 0x0000, 0x0009 },
	{ RTL_PHY_STEP_WRITE, 0xb87e, 0x0000, 0x03ff },
	{ RTL_PHY_STEP_WRITE, 0xb87e, 0x0000, 0x03ed },
	{ RTL_PHY_STEP_WRITE, 0xb87c, 0x0000, 0x80ef },
	{ RTL_PHY_STEP_MODIFY, 0xb87e, 0xff00, 0x0c00 },
	{ RTL_PHY_STEP_WRITE, 0xb87c, 0x0000, 0x82a0 },
	{ RTL_PHY_STEP_WRITE, 0xb87e, 0x0000, 0x000e },
	{ RTL_PHY_STEP_WRITE, 0xb87e, 0x0000, 0x03fe },
	{ RTL_PHY_STEP_WRITE, 0xb87e, 0x0000, 0x03ed },
	{ RTL_PHY_STEP_WRITE, 0xb87e, 0x0000, 0x0006 },
	{ RTL_PHY_STEP_WRITE, 0xb87e, 0x0000, 0x001a },
	{ RTL_PHY_STEP_WRITE, 0xb87e, 0x0000, 0x03f1 },
	{ RTL_PHY_STEP_WRITE, 0xb87e, 0x0000, 0x03d8 },
	{ RTL_PHY_STEP_WRITE, 0xb87e, 0x0000, 0x0023 },
	{ RTL_PHY_STEP_WRITE, 0xb87e, 0x0000, 0x0054 },
	{ RTL_PHY_STEP_WRITE, 0xb87e, 0x0000, 0x0322 },
	{ RTL_PHY_STEP_WRITE, 0xb87e, 0x0000, 0x00dd },
	{ RTL_PHY_STEP_WRITE, 0xb87e, 0x0000, 0x03ab },
	{ RTL_PHY_STEP_WRITE, 0xb87e, 0x0000, 0x03dc },
	{ RTL_PHY_STEP_WRITE, 0xb87e, 0x0000, 0x0027 },
	{ RTL_PHY_STEP_WRITE, 0xb87e, 0x0000, 0x000e },
	{ RTL_PHY_STEP_WRITE, 0xb87e, 0x0000, 0x03e5 },
	{ RTL_PHY_STEP_WRITE, 0xb87e, 0x0000, 0x03f9 },
	{ RTL_PHY_STEP_WRITE, 0xb87e, 0x0000, 0x0012 },
	{ RTL_PHY_STEP_WRITE, 0xb87e, 0x0000, 0x0001 },
	{ RTL_PHY_STEP_WRITE, 0xb87e, 0x0000, 0x03f1 },
	{ RTL_PHY_STEP_WRITE, 0xa436, 0x0000, 0x8018 },
	{ RTL_PHY_STEP_MODIFY, 0xa438, 0x0000, 0x2000 },
	{ RTL_PHY_STEP_WRITE, 0xb87c, 0x0000, 0x8fe4 },
	{ RTL_PHY_STEP_MODIFY, 0xb87e, 0xff00, 0x0000 },
	{ RTL_PHY_STEP_MODIFY, 0xb54c, 0xffc0, 0x3700 },
};

/*
 * The RTL8126's (second revision) PHY settings after its reset, in order,
 * without the PHY microcode patch that the reference loads first.
 *
 * Procedure and values referenced: FreeBSD sys/dev/rge/if_rge_hw.c, main
 * 8fa55fdcae4ecbd2ec13ef91cc9d172095e21189 (rge_phy_config_mac_r26_2).
 */
static const struct rtl_phy_step rtl_phy_8126_2[] = {
	{ RTL_PHY_STEP_MODIFY, 0xa442, 0x0000, 0x0800 },
	{ RTL_PHY_STEP_WRITE, 0xa436, 0x0000, 0x8183 },
	{ RTL_PHY_STEP_MODIFY, 0xa438, 0xff00, 0x5900 },
	{ RTL_PHY_STEP_MODIFY, 0xa654, 0x0000, 0x0800 },
	{ RTL_PHY_STEP_MODIFY, 0xb648, 0x0000, 0x4000 },
	{ RTL_PHY_STEP_MODIFY, 0xad2c, 0x0000, 0x8000 },
	{ RTL_PHY_STEP_MODIFY, 0xad94, 0x0000, 0x0020 },
	{ RTL_PHY_STEP_MODIFY, 0xada0, 0x0000, 0x0002 },
	{ RTL_PHY_STEP_MODIFY, 0xae06, 0xfc00, 0x7c00 },
	{ RTL_PHY_STEP_WRITE, 0xb87c, 0x0000, 0x8647 },
	{ RTL_PHY_STEP_MODIFY, 0xb87e, 0xff00, 0xe600 },
	{ RTL_PHY_STEP_WRITE, 0xb87c, 0x0000, 0x8036 },
	{ RTL_PHY_STEP_MODIFY, 0xb87e, 0xff00, 0x3000 },
	{ RTL_PHY_STEP_WRITE, 0xb87c, 0x0000, 0x8078 },
	{ RTL_PHY_STEP_MODIFY, 0xb87e, 0xff00, 0x3000 },
	{ RTL_PHY_STEP_WRITE, 0xb87c, 0x0000, 0x89e9 },
	{ RTL_PHY_STEP_MODIFY, 0xb87e, 0x0000, 0xff00 },
	{ RTL_PHY_STEP_WRITE, 0xb87c, 0x0000, 0x8ffd },
	{ RTL_PHY_STEP_MODIFY, 0xb87e, 0xff00, 0x0100 },
	{ RTL_PHY_STEP_WRITE, 0xb87c, 0x0000, 0x8ffe },
	{ RTL_PHY_STEP_MODIFY, 0xb87e, 0xff00, 0x0200 },
	{ RTL_PHY_STEP_WRITE, 0xb87c, 0x0000, 0x8fff },
	{ RTL_PHY_STEP_MODIFY, 0xb87e, 0xff00, 0x0400 },
	{ RTL_PHY_STEP_WRITE, 0xa436, 0x0000, 0x8018 },
	{ RTL_PHY_STEP_MODIFY, 0xa438, 0xff00, 0x7700 },
	{ RTL_PHY_STEP_WRITE, 0xa436, 0x0000, 0x8f9c },
	{ RTL_PHY_STEP_WRITE, 0xa438, 0x0000, 0x0005 },
	{ RTL_PHY_STEP_WRITE, 0xa438, 0x0000, 0x0000 },
	{ RTL_PHY_STEP_WRITE, 0xa438, 0x0000, 0x00ed },
	{ RTL_PHY_STEP_WRITE, 0xa438, 0x0000, 0x0502 },
	{ RTL_PHY_STEP_WRITE, 0xa438, 0x0000, 0x0b00 },
	{ RTL_PHY_STEP_WRITE, 0xa438, 0x0000, 0xd401 },
	{ RTL_PHY_STEP_WRITE, 0xa436, 0x0000, 0x8fa8 },
	{ RTL_PHY_STEP_MODIFY, 0xa438, 0xff00, 0x2900 },
	{ RTL_PHY_STEP_WRITE, 0xb87c, 0x0000, 0x814b },
	{ RTL_PHY_STEP_MODIFY, 0xb87e, 0xff00, 0x1100 },
	{ RTL_PHY_STEP_WRITE, 0xb87c, 0x0000, 0x814d },
	{ RTL_PHY_STEP_MODIFY, 0xb87e, 0xff00, 0x1100 },
	{ RTL_PHY_STEP_WRITE, 0xb87c, 0x0000, 0x814f },
	{ RTL_PHY_STEP_MODIFY, 0xb87e, 0xff00, 0x0b00 },
	{ RTL_PHY_STEP_WRITE, 0xb87c, 0x0000, 0x8142 },
	{ RTL_PHY_STEP_MODIFY, 0xb87e, 0xff00, 0x0100 },
	{ RTL_PHY_STEP_WRITE, 0xb87c, 0x0000, 0x8144 },
	{ RTL_PHY_STEP_MODIFY, 0xb87e, 0xff00, 0x0100 },
	{ RTL_PHY_STEP_WRITE, 0xb87c, 0x0000, 0x8150 },
	{ RTL_PHY_STEP_MODIFY, 0xb87e, 0xff00, 0x0100 },
	{ RTL_PHY_STEP_WRITE, 0xb87c, 0x0000, 0x8118 },
	{ RTL_PHY_STEP_MODIFY, 0xb87e, 0xff00, 0x0700 },
	{ RTL_PHY_STEP_WRITE, 0xb87c, 0x0000, 0x811a },
	{ RTL_PHY_STEP_MODIFY, 0xb87e, 0xff00, 0x0700 },
	{ RTL_PHY_STEP_WRITE, 0xb87c, 0x0000, 0x811c },
	{ RTL_PHY_STEP_MODIFY, 0xb87e, 0xff00, 0x0500 },
	{ RTL_PHY_STEP_WRITE, 0xb87c, 0x0000, 0x810f },
	{ RTL_PHY_STEP_MODIFY, 0xb87e, 0xff00, 0x0100 },
	{ RTL_PHY_STEP_WRITE, 0xb87c, 0x0000, 0x8111 },
	{ RTL_PHY_STEP_MODIFY, 0xb87e, 0xff00, 0x0100 },
	{ RTL_PHY_STEP_WRITE, 0xb87c, 0x0000, 0x811d },
	{ RTL_PHY_STEP_MODIFY, 0xb87e, 0xff00, 0x0100 },
	{ RTL_PHY_STEP_MODIFY, 0xad1c, 0x0000, 0x0100 },
	{ RTL_PHY_STEP_MODIFY, 0xade8, 0xffc0, 0x1400 },
	{ RTL_PHY_STEP_WRITE, 0xb87c, 0x0000, 0x864b },
	{ RTL_PHY_STEP_MODIFY, 0xb87e, 0xff00, 0x9d00 },
	{ RTL_PHY_STEP_WRITE, 0xb87c, 0x0000, 0x862c },
	{ RTL_PHY_STEP_MODIFY, 0xb87e, 0xff00, 0x1200 },
	{ RTL_PHY_STEP_WRITE, 0xa436, 0x0000, 0x8566 },
	{ RTL_PHY_STEP_WRITE, 0xa438, 0x0000, 0x003f },
	{ RTL_PHY_STEP_WRITE, 0xa438, 0x0000, 0x3f02 },
	{ RTL_PHY_STEP_WRITE, 0xa438, 0x0000, 0x023c },
	{ RTL_PHY_STEP_WRITE, 0xa438, 0x0000, 0x3b0a },
	{ RTL_PHY_STEP_WRITE, 0xa438, 0x0000, 0x1c00 },
	{ RTL_PHY_STEP_WRITE, 0xa438, 0x0000, 0x0000 },
	{ RTL_PHY_STEP_WRITE, 0xa438, 0x0000, 0x0000 },
	{ RTL_PHY_STEP_WRITE, 0xa438, 0x0000, 0x0000 },
	{ RTL_PHY_STEP_WRITE, 0xa438, 0x0000, 0x0000 },
	{ RTL_PHY_STEP_MODIFY, 0xad9c, 0x0000, 0x0020 },
	{ RTL_PHY_STEP_WRITE, 0xb87c, 0x0000, 0x8122 },
	{ RTL_PHY_STEP_MODIFY, 0xb87e, 0xff00, 0x0c00 },
	{ RTL_PHY_STEP_WRITE, 0xb87c, 0x0000, 0x82c8 },
	{ RTL_PHY_STEP_WRITE, 0xb87e, 0x0000, 0x03ed },
	{ RTL_PHY_STEP_WRITE, 0xb87e, 0x0000, 0x03ff },
	{ RTL_PHY_STEP_WRITE, 0xb87e, 0x0000, 0x0009 },
	{ RTL_PHY_STEP_WRITE, 0xb87e, 0x0000, 0x03fe },
	{ RTL_PHY_STEP_WRITE, 0xb87e, 0x0000, 0x000b },
	{ RTL_PHY_STEP_WRITE, 0xb87e, 0x0000, 0x0021 },
	{ RTL_PHY_STEP_WRITE, 0xb87e, 0x0000, 0x03f7 },
	{ RTL_PHY_STEP_WRITE, 0xb87e, 0x0000, 0x03b8 },
	{ RTL_PHY_STEP_WRITE, 0xb87e, 0x0000, 0x03e0 },
	{ RTL_PHY_STEP_WRITE, 0xb87e, 0x0000, 0x0049 },
	{ RTL_PHY_STEP_WRITE, 0xb87e, 0x0000, 0x0049 },
	{ RTL_PHY_STEP_WRITE, 0xb87e, 0x0000, 0x03e0 },
	{ RTL_PHY_STEP_WRITE, 0xb87e, 0x0000, 0x03b8 },
	{ RTL_PHY_STEP_WRITE, 0xb87e, 0x0000, 0x03f7 },
	{ RTL_PHY_STEP_WRITE, 0xb87e, 0x0000, 0x0021 },
	{ RTL_PHY_STEP_WRITE, 0xb87e, 0x0000, 0x000b },
	{ RTL_PHY_STEP_WRITE, 0xb87e, 0x0000, 0x03fe },
	{ RTL_PHY_STEP_WRITE, 0xb87e, 0x0000, 0x0009 },
	{ RTL_PHY_STEP_WRITE, 0xb87e, 0x0000, 0x03ff },
	{ RTL_PHY_STEP_WRITE, 0xb87e, 0x0000, 0x03ed },
	{ RTL_PHY_STEP_WRITE, 0xb87c, 0x0000, 0x80ef },
	{ RTL_PHY_STEP_MODIFY, 0xb87e, 0xff00, 0x0c00 },
	{ RTL_PHY_STEP_WRITE, 0xb87e, 0x0000, 0x82a0 },
	{ RTL_PHY_STEP_WRITE, 0xb87e, 0x0000, 0x000e },
	{ RTL_PHY_STEP_WRITE, 0xb87e, 0x0000, 0x03fe },
	{ RTL_PHY_STEP_WRITE, 0xb87e, 0x0000, 0x03ed },
	{ RTL_PHY_STEP_WRITE, 0xb87e, 0x0000, 0x0006 },
	{ RTL_PHY_STEP_WRITE, 0xb87e, 0x0000, 0x001a },
	{ RTL_PHY_STEP_WRITE, 0xb87e, 0x0000, 0x03f1 },
	{ RTL_PHY_STEP_WRITE, 0xb87e, 0x0000, 0x03d8 },
	{ RTL_PHY_STEP_WRITE, 0xb87e, 0x0000, 0x0023 },
	{ RTL_PHY_STEP_WRITE, 0xb87e, 0x0000, 0x0054 },
	{ RTL_PHY_STEP_WRITE, 0xb87e, 0x0000, 0x0322 },
	{ RTL_PHY_STEP_WRITE, 0xb87e, 0x0000, 0x00dd },
	{ RTL_PHY_STEP_WRITE, 0xb87e, 0x0000, 0x03ab },
	{ RTL_PHY_STEP_WRITE, 0xb87e, 0x0000, 0x03dc },
	{ RTL_PHY_STEP_WRITE, 0xb87e, 0x0000, 0x0027 },
	{ RTL_PHY_STEP_WRITE, 0xb87e, 0x0000, 0x000e },
	{ RTL_PHY_STEP_WRITE, 0xb87e, 0x0000, 0x03e5 },
	{ RTL_PHY_STEP_WRITE, 0xb87e, 0x0000, 0x03f9 },
	{ RTL_PHY_STEP_WRITE, 0xb87e, 0x0000, 0x0012 },
	{ RTL_PHY_STEP_WRITE, 0xb87e, 0x0000, 0x0001 },
	{ RTL_PHY_STEP_WRITE, 0xb87e, 0x0000, 0x03f1 },
	{ RTL_PHY_STEP_MODIFY, 0xa430, 0x0000, 0x0003 },
	{ RTL_PHY_STEP_MODIFY, 0xb54c, 0xffc0, 0x3700 },
	{ RTL_PHY_STEP_MODIFY, 0xb648, 0x0000, 0x0040 },
	{ RTL_PHY_STEP_WRITE, 0xb87c, 0x0000, 0x8082 },
	{ RTL_PHY_STEP_MODIFY, 0xb87e, 0xff00, 0x5d00 },
	{ RTL_PHY_STEP_WRITE, 0xb87c, 0x0000, 0x807c },
	{ RTL_PHY_STEP_MODIFY, 0xb87e, 0xff00, 0x5000 },
	{ RTL_PHY_STEP_WRITE, 0xb87c, 0x0000, 0x809d },
	{ RTL_PHY_STEP_MODIFY, 0xb87e, 0xff00, 0x5000 },
};

/* The PCI identities the driver binds. */
static const struct drv_pci_id rtl_pci_ids[] = {
	{
		.vendor = RTL_PCI_VENDOR_REALTEK,
		.device = RTL_PCI_DEVICE_8125,
		.subvendor = DRV_PCI_ANY_ID,
		.subdevice = DRV_PCI_ANY_ID,
	},
	{
		.vendor = RTL_PCI_VENDOR_REALTEK,
		.device = RTL_PCI_DEVICE_8126,
		.subvendor = DRV_PCI_ANY_ID,
		.subdevice = DRV_PCI_ANY_ID,
	},
	{
		.vendor = RTL_PCI_VENDOR_REALTEK,
		.device = RTL_PCI_DEVICE_8127,
		.subvendor = DRV_PCI_ANY_ID,
		.subdevice = DRV_PCI_ANY_ID,
	},
};

static int rtl_attach(struct drv_pci_device *pci, const struct drv_pci_id *id);

/* The PCI driver of the family. */
static struct drv_pci_driver rtl_pci_driver = {
	.name = "rtl8125",
	.ids = rtl_pci_ids,
	.id_count = sizeof(rtl_pci_ids) / sizeof(rtl_pci_ids[0]),
	.attach = rtl_attach,
};

/*
 * The attached controllers.
 *
 * A slot whose pci is NULL is free.  Slots are taken by attach on the boot
 * thread and never given back once the interface is published.
 */
static struct rtl_device rtl_devices[RTL_MAX_DEVICES];

static int rtl_open(struct net_device *net);
static void rtl_close(struct net_device *net);
static int rtl_transmit(struct net_device *net, struct packet_buf *packet);
static unsigned rtl_poll(struct net_device *net, unsigned budget);

/* The network callbacks. */
static const struct net_device_ops rtl_operations = {
	rtl_open,
	rtl_close,
	rtl_transmit,
	rtl_poll,
	NULL,
	NULL,
	NULL
};

static uint8_t read8(const struct rtl_device *device, uint32_t offset);
static uint16_t read16(const struct rtl_device *device, uint32_t offset);
static uint32_t read32(const struct rtl_device *device, uint32_t offset);
static void write8(struct rtl_device *device, uint32_t offset, uint8_t value);
static void write32(struct rtl_device *device, uint32_t offset, uint32_t value);
static void write16(struct rtl_device *device, uint32_t offset, uint16_t value);
static void clear16(struct rtl_device *device, uint32_t offset, uint16_t bits);
static void clear32(struct rtl_device *device, uint32_t offset, uint32_t bits);
static void mac_ocp_set(struct rtl_device *device, uint16_t reg, uint16_t bits);
static void mac_ocp_modify(struct rtl_device *device, uint16_t reg, uint16_t mask, uint16_t value);
static int allocate_rings(struct rtl_device *device);
static void prepare_rings(struct rtl_device *device);
static void arm_rx_slot(struct rtl_device *device, unsigned slot);
static void configure_mac(struct rtl_device *device);
static void configure_rings_and_pcie(struct rtl_device *device);
static void configure_mac_ocp(struct rtl_device *device);
static void configure_interrupt_type(struct rtl_device *device);
static void configure_receiver(struct rtl_device *device);
static void stop_controller(struct rtl_device *device);
static unsigned reclaim_tx(struct rtl_device *device);
static struct packet_buf *take_tx_done_locked(struct rtl_device *device);
static void free_tx_done(struct packet_buf *list);
static unsigned receive_frames(struct net_device *net, unsigned budget);
static uint32_t rx_checksum_flags(uint32_t extended);
static void report_tx_errors(struct net_device *net, unsigned errors);
static void set8(struct rtl_device *device, uint32_t offset, uint8_t bits);
static void clear8(struct rtl_device *device, uint32_t offset, uint8_t bits);
static int phy_ocp_modify(struct rtl_device *device, uint16_t reg, uint16_t mask, uint16_t value);
static int mii_read(struct rtl_device *device, unsigned reg, uint16_t *value);
static int mii_write(struct rtl_device *device, unsigned reg, uint16_t value);
static uint32_t csi_read(struct rtl_device *device, uint32_t reg);
static void csi_write(struct rtl_device *device, uint32_t reg, uint32_t value);
static void ephy_write(struct rtl_device *device, uint16_t reg, uint16_t value);
static int run_phy_steps(struct rtl_device *device, const struct rtl_phy_step *steps, unsigned count);
static int set_phy_patch_mode(struct rtl_device *device, int enter);
static uint16_t ephy_read(struct rtl_device *device, uint16_t reg);
static void ephy_modify(struct rtl_device *device, uint16_t reg, uint16_t mask, uint16_t value);
static void configure_ephy_8125(struct rtl_device *device);
static int phy_steps_of(const struct rtl_device *device, const struct rtl_phy_step **steps, unsigned *count);
static int bring_up_link(struct rtl_device *device);
static int power_up_phy(struct rtl_device *device);
static void disable_clock_request(struct rtl_device *device);
static void stop_mac_mcu(struct rtl_device *device);
static void configure_hardware(struct rtl_device *device);
static void configure_ephy(struct rtl_device *device);
static int reset_phy(struct rtl_device *device);
static int configure_phy(struct rtl_device *device);
static int disable_eee(struct rtl_device *device);
static int advertise_all(struct rtl_device *device);
static void start_link_thread(struct rtl_device *device);
static void link_worker(void *argument);
static void check_link(struct rtl_device *device);
static uint32_t speed_of(uint16_t status);
static void profile_calibrate(struct rtl_device *device);
static uint64_t profile_now(void);
static void profile_interrupt(struct rtl_device *device, uint64_t started);
static void profile_poll(struct rtl_device *device, uint64_t started, unsigned count, unsigned budget);
static void profile_transmit(struct rtl_device *device, uint64_t started, int in_place, int error);
static void profile_report(struct rtl_device *device);
static uint64_t profile_microseconds(const struct rtl_device *device, uint64_t time);
static int profile_missed(struct rtl_device *device, uint16_t *missed);
static uint16_t mac_ocp_read(struct rtl_device *device, uint16_t reg);
static void mac_ocp_write(struct rtl_device *device, uint16_t reg, uint16_t value);
static void mac_ocp_clear(struct rtl_device *device, uint16_t reg, uint16_t bits);
static int phy_ocp_read(struct rtl_device *device, uint16_t reg, uint16_t *value);
static int phy_ocp_write(struct rtl_device *device, uint16_t reg, uint16_t value);
static int phy_ocp_clear(struct rtl_device *device, uint16_t reg, uint16_t bits);
static struct rtl_device *take_slot(void);
static void release_unpublished(struct rtl_device *device);
static int map_registers(struct rtl_device *device);
static int identify_chip(struct rtl_device *device);
static void disable_link_power_management(struct rtl_device *device);
static void mask_interrupts(struct rtl_device *device);
static int reset_controller(struct rtl_device *device);
static int exit_out_of_band(struct rtl_device *device);
static void wait_twi_ready(struct rtl_device *device);
static int hand_back_phy(struct rtl_device *device);
static void read_station_address(struct rtl_device *device);
static int setup_interrupt(struct rtl_device *device);
static int rtl_interrupt(void *argument);
static int publish_interface(struct rtl_device *device);

/*
 * Registers the driver for the RTL8125, RTL8126 and RTL8127 controllers.
 */
int
drv_pci_rtl8125_driver_register(
	void)
{
	int error;

	/* Offers the family's PCI identities to the bus. */
	error = drv_pci_driver_register(&rtl_pci_driver);
	if (error != 0)
		return error;

	/* Succeeded: the bus binds the controllers it finds. */
	return 0;
}

/*
 * Takes a controller out of the firmware's hands and publishes it.
 *
 * Procedure referenced: OpenBSD sys/dev/pci/if_rge.c 1.48 (rge_attach,
 * rge_chipinit, rge_hw_reset).
 */
static int
rtl_attach(
	struct drv_pci_device *pci,
	const struct drv_pci_id *id)
{
	struct rtl_device *device;
	const char *suffix;
	uint32_t version;
	int error;

	UNUSED_PARAMETER(id);

	/* Takes a free slot for the controller. */
	device = take_slot();
	if (device == NULL)
		return ENOSPC;
	device->pci = pci;
	spin_init(&device->lock, LOCK_RANK_DEVICE, "rtl8125");

	/* Maps the registers and turns on memory decoding and bus mastering. */
	error = map_registers(device);
	if (error != 0) {
		kern_logf("rtl8125: registers unavailable (%d)\n", error);
		release_unpublished(device);
		return error;
	}

	/* Learns which member of the family this is; an unknown one is left alone. */
	error = identify_chip(device);
	if (error != 0) {
		version = read32(device, RTL_REG_TX_CONFIG) & RTL_TX_CONFIG_VERSION_MASK;
		kern_logf("rtl8125: unknown version 0x%08x\n", version);
		release_unpublished(device);
		return error;
	}

	/* Keeps the link out of the power states the controller mishandles. */
	disable_link_power_management(device);

	/* Masks the controller's interrupts before anything can raise one. */
	mask_interrupts(device);

	/* Takes the controller back from the firmware and resets it. */
	error = exit_out_of_band(device);
	if (error != 0) {
		kern_logf("rtl8125: %s did not leave out-of-band mode (%d)\n",
			  device->chip->name, error);
		release_unpublished(device);
		return error;
	}

	/* Masks the interrupts again, which the reset may have changed. */
	mask_interrupts(device);

	/* Takes the address the chip was given at manufacture. */
	read_station_address(device);

	/* Allocates the rings, which the controller sees only once the interface opens. */
	error = allocate_rings(device);
	if (error != 0) {
		kern_logf("rtl8125: %s rings unavailable (%d)\n", device->chip->name, error);
		release_unpublished(device);
		return error;
	}

	/* Connects the controller's interrupt. */
	error = setup_interrupt(device);
	if (error != 0) {
		kern_logf("rtl8125: interrupt unavailable (%d)\n", error);
		release_unpublished(device);
		return error;
	}

	/* Publishes the interface; the interrupt stays connected but masked if it cannot be. */
	error = publish_interface(device);
	if (error != 0) {
		kern_logf("rtl8125: interface unavailable (%d)\n", error);
		return error;
	}

	/* Measures the profile's counter, then follows the link from here on. */
	profile_calibrate(device);
	start_link_thread(device);

	/* Names the RTL8127's SFP+ part, which the log tells from the copper one. */
	suffix = "";
	if (device->fiber)
		suffix = "ATF";

	/* Tells which part was found and under which address. */
	kern_logf("rtl8125: %s: %s%s rev %u, address %02x:%02x:%02x:%02x:%02x:%02x\n",
		  device->name,
		  device->chip->name,
		  suffix,
		  device->chip->revision,
		  device->mac[0],
		  device->mac[1],
		  device->mac[2],
		  device->mac[3],
		  device->mac[4],
		  device->mac[5]);

	/* Keeps the controller with its PCI device. */
	(void)drv_pci_device_set_driver_data(pci, device);

	/* Succeeded: the controller is published. */
	return 0;
}

/* Reads one byte register. */
static uint8_t
read8(
	const struct rtl_device *device,
	uint32_t offset)
{
	uint8_t value;

	/* Reads it with device ordering. */
	value = kern_mmio_read8(device->registers + offset);

	/* Reports the register's contents. */
	return value;
}

/* Reads one word register. */
static uint16_t
read16(
	const struct rtl_device *device,
	uint32_t offset)
{
	uint16_t value;

	/* Reads it with device ordering. */
	value = kern_mmio_read16(device->registers + offset);

	/* Reports the register's contents. */
	return value;
}

/* Reads one longword register. */
static uint32_t
read32(
	const struct rtl_device *device,
	uint32_t offset)
{
	uint32_t value;

	/* Reads it with device ordering. */
	value = kern_mmio_read32(device->registers + offset);

	/* Reports the register's contents. */
	return value;
}

/* Writes one byte register. */
static void
write8(
	struct rtl_device *device,
	uint32_t offset,
	uint8_t value)
{
	/* Writes it after every earlier store. */
	kern_mmio_write8(device->registers + offset, value);
}

/* Writes one longword register. */
static void
write32(
	struct rtl_device *device,
	uint32_t offset,
	uint32_t value)
{
	/* Writes it after every earlier store. */
	kern_mmio_write32(device->registers + offset, value);
}

/* Writes one word register. */
static void
write16(
	struct rtl_device *device,
	uint32_t offset,
	uint16_t value)
{
	/* Writes it after every earlier store. */
	kern_mmio_write16(device->registers + offset, value);
}

/* Clears bits of one word register. */
static void
clear16(
	struct rtl_device *device,
	uint32_t offset,
	uint16_t bits)
{
	uint16_t value;

	/* Writes the register back without the bits. */
	value = read16(device, offset);
	write16(device, offset, (uint16_t)(value & ~bits));
}

/* Clears bits of one longword register. */
static void
clear32(
	struct rtl_device *device,
	uint32_t offset,
	uint32_t bits)
{
	uint32_t value;

	/* Writes the register back without the bits. */
	value = read32(device, offset);
	write32(device, offset, value & ~bits);
}

/*
 * Reads one register of the MAC's OCP space.
 *
 * The window takes the register's word address in its upper half and
 * returns the data in its lower half at once.
 *
 * Procedure referenced: OpenBSD sys/dev/pci/if_rge.c 1.48
 * (rge_read_mac_ocp).
 */
static uint16_t
mac_ocp_read(
	struct rtl_device *device,
	uint16_t reg)
{
	uint32_t window;

	/* Names the register. */
	window = (uint32_t)(reg >> 1) << RTL_OCP_ADDRESS_SHIFT;
	write32(device, RTL_REG_MAC_OCP, window);

	/* Reports what the window returned. */
	window = read32(device, RTL_REG_MAC_OCP);
	return (uint16_t)(window & RTL_OCP_DATA_MASK);
}

/*
 * Writes one register of the MAC's OCP space.
 *
 * Procedure referenced: OpenBSD sys/dev/pci/if_rge.c 1.48
 * (rge_write_mac_ocp).
 */
static void
mac_ocp_write(
	struct rtl_device *device,
	uint16_t reg,
	uint16_t value)
{
	uint32_t window;

	/* Names the register and the value with the write bit. */
	window = (uint32_t)(reg >> 1) << RTL_OCP_ADDRESS_SHIFT;
	window |= value;
	window |= RTL_OCP_BUSY;
	write32(device, RTL_REG_MAC_OCP, window);
}

/* Clears bits of one register of the MAC's OCP space. */
static void
mac_ocp_clear(
	struct rtl_device *device,
	uint16_t reg,
	uint16_t bits)
{
	uint16_t value;

	/* Writes the register back without the bits. */
	value = mac_ocp_read(device, reg);
	mac_ocp_write(device, reg, (uint16_t)(value & ~bits));
}

/* Sets bits of one register of the MAC's OCP space. */
static void
mac_ocp_set(
	struct rtl_device *device,
	uint16_t reg,
	uint16_t bits)
{
	uint16_t value;

	/* Writes the register back with the bits. */
	value = mac_ocp_read(device, reg);
	mac_ocp_write(device, reg, (uint16_t)(value | bits));
}

/* Clears the mask bits and sets the value bits of one register of the MAC's OCP space. */
static void
mac_ocp_modify(
	struct rtl_device *device,
	uint16_t reg,
	uint16_t mask,
	uint16_t value)
{
	uint16_t current;

	/* Writes the register back changed. */
	current = mac_ocp_read(device, reg);
	mac_ocp_write(device, reg, (uint16_t)((current & ~mask) | value));
}

/*
 * Reads one register of the PHY's OCP space.
 *
 * The window sets its flag once the data has arrived.
 *
 * Procedure referenced: OpenBSD sys/dev/pci/if_rge.c 1.48
 * (rge_read_phy_ocp).
 */
static int
phy_ocp_read(
	struct rtl_device *device,
	uint16_t reg,
	uint16_t *value)
{
	uint32_t window;
	unsigned poll;

	/* Names the register. */
	window = (uint32_t)(reg >> 1) << RTL_OCP_ADDRESS_SHIFT;
	write32(device, RTL_REG_PHY_OCP, window);

	/* Waits for the PHY to answer. */
	for (poll = 0; poll < RTL_OCP_POLLS; poll++) {
		kern_usleep_range(RTL_OCP_POLL_US, RTL_OCP_POLL_US);

		/* The data is valid once the flag is set. */
		window = read32(device, RTL_REG_PHY_OCP);
		if ((window & RTL_OCP_BUSY) != 0) {
			*value = (uint16_t)(window & RTL_OCP_DATA_MASK);
			return 0;
		}
	}

	/* A PHY that never answers is reported. */
	return ETIMEDOUT;
}

/*
 * Writes one register of the PHY's OCP space.
 *
 * The window clears its flag once the PHY has taken the value.
 *
 * Procedure referenced: OpenBSD sys/dev/pci/if_rge.c 1.48
 * (rge_write_phy_ocp).
 */
static int
phy_ocp_write(
	struct rtl_device *device,
	uint16_t reg,
	uint16_t value)
{
	uint32_t window;
	unsigned poll;

	/* Names the register and the value with the write bit. */
	window = (uint32_t)(reg >> 1) << RTL_OCP_ADDRESS_SHIFT;
	window |= value;
	window |= RTL_OCP_BUSY;
	write32(device, RTL_REG_PHY_OCP, window);

	/* Waits for the PHY to take it. */
	for (poll = 0; poll < RTL_OCP_POLLS; poll++) {
		kern_usleep_range(RTL_OCP_POLL_US, RTL_OCP_POLL_US);

		/* The write is done once the flag is clear. */
		window = read32(device, RTL_REG_PHY_OCP);
		if ((window & RTL_OCP_BUSY) == 0)
			return 0;
	}

	/* A PHY that never takes the value is reported. */
	return ETIMEDOUT;
}

/* Clears bits of one register of the PHY's OCP space. */
static int
phy_ocp_clear(
	struct rtl_device *device,
	uint16_t reg,
	uint16_t bits)
{
	uint16_t value;
	int error;

	/* Reads the register. */
	error = phy_ocp_read(device, reg, &value);
	if (error != 0)
		return error;

	/* Writes it back without the bits. */
	error = phy_ocp_write(device, reg, (uint16_t)(value & ~bits));
	if (error != 0)
		return error;

	/* Succeeded: the bits are clear. */
	return 0;
}

/* Takes a free controller slot. */
static struct rtl_device *
take_slot(
	void)
{
	unsigned index;

	/* Looks for a slot no controller holds. */
	for (index = 0; index < RTL_MAX_DEVICES; index++) {
		/* A slot without a PCI device is free. */
		if (rtl_devices[index].pci == NULL)
			return &rtl_devices[index];
	}

	/* Every slot is taken. */
	return NULL;
}

/* Gives back what a failed attach took, in the reverse order, and frees the slot. */
static void
release_unpublished(
	struct rtl_device *device)
{
	/* Frees the receive buffers, which the controller never saw. */
	if (device->rx_buffers_allocated) {
		drv_dma_free_coherent(device->dma, &device->rx_buffers);
		device->rx_buffers_allocated = 0U;
	}

	/* Frees the transmit bounce buffers. */
	if (device->tx_buffers_allocated) {
		drv_dma_free_coherent(device->dma, &device->tx_buffers);
		device->tx_buffers_allocated = 0U;
	}

	/* Frees the descriptors. */
	if (device->descriptors_allocated) {
		drv_dma_free_coherent(device->dma, &device->descriptors);
		device->descriptors_allocated = 0U;
	}

	/* Unmaps the registers. */
	if (device->bar_mapped) {
		drv_pci_device_unmap_bar(device->pci, &device->mapping);
		device->bar_mapped = 0U;
		device->registers = NULL;
	}

	/* Puts decoding and bus mastering back as the firmware left them. */
	if (device->enable_saved) {
		(void)drv_pci_device_restore_enable_state(device->pci, &device->enable_state);
		device->enable_saved = 0U;
	}

	/* Gives the BAR back to the bus. */
	if (device->bar_claimed) {
		drv_pci_device_release_bar(device->pci, RTL_REGISTER_BAR);
		device->bar_claimed = 0U;
	}

	/* Frees the slot. */
	device->pci = NULL;
}

/* Maps the register BAR uncached and enables decoding and bus mastering. */
static int
map_registers(
	struct rtl_device *device)
{
	struct drv_pci_bar bar;
	int error;

	/* Brings the function to full power, where its registers answer. */
	error = drv_pci_device_set_power_state(device->pci, DRV_PCI_D0);
	if (error != 0)
		return error;

	/* Claims the register BAR. */
	error = drv_pci_device_claim_bar(device->pci, RTL_REGISTER_BAR);
	if (error != 0)
		return error;
	device->bar_claimed = 1U;

	/* Reads where the BAR is and what it is. */
	error = drv_pci_device_bar(device->pci, RTL_REGISTER_BAR, &bar);
	if (error != 0)
		return error;

	/* Refuses a BAR that is not memory or too small for the registers. */
	if (bar.type != DRV_PCI_BAR_MEMORY32 && bar.type != DRV_PCI_BAR_MEMORY64)
		return ENODEV;
	if (bar.size < RTL_REGISTER_BAR_MINIMUM)
		return ENODEV;

	/* Saves the decoding and bus mastering the firmware left, for a failed attach. */
	error = drv_pci_device_save_enable_state(device->pci, &device->enable_state);
	if (error != 0)
		return error;
	device->enable_saved = 1U;

	/* Maps it uncached. */
	error = drv_pci_device_map_bar(device->pci,
				       RTL_REGISTER_BAR,
				       DRV_PCI_MAP_READ | DRV_PCI_MAP_WRITE | DRV_PCI_MAP_NOCACHE,
				       &device->mapping);
	if (error != 0)
		return error;
	device->bar_mapped = 1U;

	/* Refuses a mapping that does not cover the registers. */
	if (device->mapping.address == NULL || device->mapping.size < RTL_REGISTER_BAR_MINIMUM)
		return EIO;
	device->registers = device->mapping.address;

	/* Turns on memory decoding. */
	error = drv_pci_device_enable_memory(device->pci);
	if (error != 0)
		return error;

	/* Lets the controller reach memory. */
	error = drv_pci_device_set_bus_master(device->pci, true);
	if (error != 0)
		return error;

	/* Succeeded: the registers are reachable. */
	return 0;
}

/*
 * Finds the controller's version in the table.
 *
 * Procedure referenced: OpenBSD sys/dev/pci/if_rge.c 1.48 (rge_attach, the
 * hardware revision switch and the RTL8127ATF check).
 */
static int
identify_chip(
	struct rtl_device *device)
{
	uint32_t version;
	uint16_t port;
	unsigned index;

	/* Reads the version from the transmit configuration. */
	version = read32(device, RTL_REG_TX_CONFIG) & RTL_TX_CONFIG_VERSION_MASK;

	/* Looks it up. */
	device->chip = NULL;
	for (index = 0; index < sizeof(rtl_chips) / sizeof(rtl_chips[0]); index++) {
		/* The first entry with the version is the chip. */
		if (rtl_chips[index].version == version) {
			device->chip = &rtl_chips[index];
			break;
		}
	}

	/* A version the table does not have has no known steps. */
	if (device->chip == NULL)
		return ENODEV;

	/* Tells the RTL8127's SFP+ part from the copper one. */
	device->fiber = 0U;
	if (device->chip->family == RTL_FAMILY_8127) {
		port = mac_ocp_read(device, RTL_MAC_OCP_PORT_TYPE);
		if (port == RTL_MAC_OCP_PORT_TYPE_FIBER)
			device->fiber = 1U;
	}

	/* Succeeded: the chip's steps are known. */
	return 0;
}

/*
 * Turns off ASPM and clock power management on the controller's link.
 *
 * Procedure referenced: OpenBSD sys/dev/pci/if_rge.c 1.48 (rge_attach,
 * the PCI Express check).
 */
static void
disable_link_power_management(
	struct rtl_device *device)
{
	unsigned capability;
	uint16_t control;
	int error;

	/* Finds the PCI Express capability; a device without one has no link to set. */
	error = drv_pci_device_find_capability(device->pci, RTL_PCI_CAPABILITY_EXPRESS, &capability);
	if (error != 0)
		return;

	/* Reads the link control register. */
	error = drv_pci_device_config_read16(device->pci,
					     capability + RTL_PCI_EXPRESS_LINK_CONTROL,
					     &control);
	if (error != 0)
		return;

	/* Writes it back without ASPM and clock power management. */
	control &= (uint16_t)~(RTL_PCI_LINK_ASPM_MASK | RTL_PCI_LINK_CLOCK_PM);
	(void)drv_pci_device_config_write16(device->pci,
					    capability + RTL_PCI_EXPRESS_LINK_CONTROL,
					    control);
}

/*
 * Masks every interrupt source and acknowledges what is pending.
 *
 * Procedure referenced: OpenBSD sys/dev/pci/if_rge.c 1.48 (rge_hw_reset,
 * the single-vector case).
 */
static void
mask_interrupts(
	struct rtl_device *device)
{
	uint32_t status;

	/* Masks the sources. */
	write32(device, RTL_REG_INTERRUPT_MASK, 0U);

	/* Acknowledges whatever is already latched. */
	status = read32(device, RTL_REG_INTERRUPT_STATUS);
	write32(device, RTL_REG_INTERRUPT_STATUS, status);
}

/*
 * Stops the receiver and transmitter, lets the FIFOs drain and resets the
 * MAC.
 *
 * Procedure referenced: OpenBSD sys/dev/pci/if_rge.c 1.48 (rge_reset).
 */
static int
reset_controller(
	struct rtl_device *device)
{
	uint32_t config;
	uint16_t idle;
	uint8_t command;
	uint8_t mcu;
	unsigned poll;

	/* Stops accepting frames of every kind. */
	config = read32(device, RTL_REG_RX_CONFIG);
	write32(device, RTL_REG_RX_CONFIG, config & ~RTL_RX_CONFIG_ACCEPT_MASK);

	/* Gates the receive data path. */
	command = read8(device, RTL_REG_POWER_SWITCH);
	write8(device, RTL_REG_POWER_SWITCH, (uint8_t)(command | RTL_POWER_SWITCH_RX_GATE));

	/* Asks the MAC to stop and gives it time to see the request. */
	command = read8(device, RTL_REG_COMMAND);
	write8(device, RTL_REG_COMMAND, (uint8_t)(command | RTL_COMMAND_STOP_REQUEST));
	kern_usleep_range(RTL_STOP_SETTLE_US, RTL_STOP_SETTLE_US);

	/* Waits for both FIFOs to empty; a reset follows either way. */
	for (poll = 0; poll < RTL_DRAIN_POLLS; poll++) {
		kern_usleep_range(RTL_DRAIN_POLL_US, RTL_DRAIN_POLL_US);

		/* The FIFOs are drained once both flags are set. */
		mcu = read8(device, RTL_REG_MCU_COMMAND);
		if ((mcu & (RTL_MCU_RX_FIFO_EMPTY | RTL_MCU_TX_FIFO_EMPTY)) ==
		    (RTL_MCU_RX_FIFO_EMPTY | RTL_MCU_TX_FIFO_EMPTY))
			break;
	}

	/* Waits for the later parts' DMA to go idle as well. */
	if (device->chip->family != RTL_FAMILY_8125) {
		for (poll = 0; poll < RTL_DRAIN_POLLS; poll++) {
			kern_usleep_range(RTL_DRAIN_POLL_US, RTL_DRAIN_POLL_US);

			/* The DMA is idle once every idle flag is set. */
			idle = read16(device, RTL_REG_IDLE_STATUS);
			if ((idle & RTL_IDLE_STATUS_READY) == RTL_IDLE_STATUS_READY)
				break;
		}
	}

	/* Drops every command bit but the two enables, then resets. */
	command = read8(device, RTL_REG_COMMAND);
	write8(device, RTL_REG_COMMAND, (uint8_t)(command & (RTL_COMMAND_TX_ENABLE | RTL_COMMAND_RX_ENABLE)));
	write8(device, RTL_REG_COMMAND, RTL_COMMAND_RESET);

	/* Waits for the reset to finish. */
	for (poll = 0; poll < RTL_RESET_POLLS; poll++) {
		kern_usleep_range(RTL_RESET_POLL_US, RTL_RESET_POLL_US);

		/* The MAC is out of reset once the bit clears. */
		command = read8(device, RTL_REG_COMMAND);
		if ((command & RTL_COMMAND_RESET) == 0)
			return 0;
	}

	/* A MAC that stays in reset is reported. */
	return ETIMEDOUT;
}

/*
 * Takes the controller from the firmware's out-of-band management.
 *
 * Procedure referenced: OpenBSD sys/dev/pci/if_rge.c 1.48 (rge_exit_oob).
 */
static int
exit_out_of_band(
	struct rtl_device *device)
{
	uint16_t ups;
	uint8_t mcu;
	int error;

	/* Turns the firmware's wake-on-LAN agent off. */
	mac_ocp_write(device, RTL_MAC_OCP_REALWOW, RTL_MAC_OCP_REALWOW_OFF);

	/* Resets the MAC. */
	error = reset_controller(device);
	if (error != 0)
		return error;

	/* Leaves out-of-band mode. */
	mcu = read8(device, RTL_REG_MCU_COMMAND);
	write8(device, RTL_REG_MCU_COMMAND, (uint8_t)(mcu & ~RTL_MCU_OUT_OF_BAND));
	mac_ocp_clear(device, RTL_MAC_OCP_OOB_CONTROL, RTL_MAC_OCP_OOB_CONTROL_BIT);
	wait_twi_ready(device);

	/* Hands the MAC's management back to the driver. */
	mac_ocp_write(device, RTL_MAC_OCP_HANDOFF_A, RTL_MAC_OCP_HANDOFF_A_VALUE);
	mac_ocp_write(device, RTL_MAC_OCP_HANDOFF_B, RTL_MAC_OCP_HANDOFF_B_VALUE);
	mac_ocp_write(device, RTL_MAC_OCP_HANDOFF_C, RTL_MAC_OCP_HANDOFF_C_VALUE);
	wait_twi_ready(device);

	/* Brings the PHY back from the firmware's power saving when it is there. */
	ups = mac_ocp_read(device, RTL_MAC_OCP_UPS);
	if ((ups & RTL_MAC_OCP_UPS_RESUME) != 0) {
		error = hand_back_phy(device);
		if (error != 0)
			return error;
	}

	/* Succeeded: the driver owns the controller. */
	return 0;
}

/* Gives the MAC's two-wire interface a bounded time to report ready. */
static void
wait_twi_ready(
	struct rtl_device *device)
{
	uint16_t status;
	unsigned poll;

	/* Polls the ready flag; the steps after it run either way. */
	for (poll = 0; poll < RTL_TWI_POLLS; poll++) {
		kern_usleep_range(RTL_TWI_POLL_US, RTL_TWI_POLL_US);

		/* The interface is ready once the flag is set. */
		status = read16(device, RTL_REG_TWI_COMMAND);
		if ((status & RTL_TWI_READY) != 0)
			break;
	}
}

/*
 * Waits for the PHY to leave the firmware's power saving and clears the
 * marks it left.
 *
 * Procedure referenced: OpenBSD sys/dev/pci/if_rge.c 1.48 (rge_exit_oob).
 */
static int
hand_back_phy(
	struct rtl_device *device)
{
	uint16_t state;
	unsigned poll;
	int error;

	/* Waits for the PHY to report it is handed back; the clears follow either way. */
	for (poll = 0; poll < RTL_PHY_STATE_POLLS; poll++) {
		/* A PHY that does not answer ends the wait. */
		error = phy_ocp_read(device, RTL_PHY_OCP_STATE, &state);
		if (error != 0)
			return error;

		/* The PHY is back once its state says so. */
		if ((state & RTL_PHY_OCP_STATE_MASK) == RTL_PHY_OCP_STATE_HANDED_BACK)
			break;

		/* Gives the PHY a millisecond before asking again. */
		kern_usleep_range(RTL_PHY_STATE_POLL_US, RTL_PHY_STATE_POLL_US);
	}

	/* Clears the MAC's resume mark. */
	mac_ocp_clear(device, RTL_MAC_OCP_UPS, RTL_MAC_OCP_UPS_RESUME);

	/* Clears the PHY's marks; the first RTL8125 has only the second. */
	if (device->chip->family != RTL_FAMILY_8125) {
		error = phy_ocp_clear(device, RTL_PHY_OCP_UPS_A, RTL_PHY_OCP_UPS_A_BIT);
		if (error != 0)
			return error;
	}

	/* Clears the mark every member has. */
	error = phy_ocp_clear(device, RTL_PHY_OCP_UPS_B, RTL_PHY_OCP_UPS_B_BITS);
	if (error != 0)
		return error;

	/* Succeeded: the PHY is the driver's. */
	return 0;
}

/*
 * Reads the station address the chip was given at manufacture and makes it
 * the one the MAC filters on.
 *
 * Procedure referenced: OpenBSD sys/dev/pci/if_rge.c 1.48
 * (rge_get_macaddr, rge_set_macaddr).
 */
static void
read_station_address(
	struct rtl_device *device)
{
	uint32_t low;
	uint32_t high;
	uint8_t lock;

	/* Reads the permanent address, low byte first. */
	low = read32(device, RTL_REG_PERMANENT0);
	high = read32(device, RTL_REG_PERMANENT4);
	device->mac[0] = (uint8_t)low;
	device->mac[1] = (uint8_t)(low >> 8);
	device->mac[2] = (uint8_t)(low >> 16);
	device->mac[3] = (uint8_t)(low >> 24);
	device->mac[4] = (uint8_t)high;
	device->mac[5] = (uint8_t)(high >> 8);

	/* Opens the configuration registers for writing. */
	lock = read8(device, RTL_REG_CONFIG_LOCK);
	write8(device, RTL_REG_CONFIG_LOCK, (uint8_t)(lock | RTL_CONFIG_LOCK_WRITABLE));

	/* Writes the address into the station registers. */
	write32(device, RTL_REG_STATION0, low);
	write32(device, RTL_REG_STATION4, high & 0xffffU);

	/* Locks the configuration registers again. */
	lock = read8(device, RTL_REG_CONFIG_LOCK);
	write8(device, RTL_REG_CONFIG_LOCK, (uint8_t)(lock & ~RTL_CONFIG_LOCK_WRITABLE));
}

/* Allocates one interrupt vector and connects the handler to it. */
static int
setup_interrupt(
	struct rtl_device *device)
{
	unsigned count;
	int error;

	/* Takes one vector, preferring MSI-X, then MSI, then the pin. */
	error = drv_pci_device_allocate_irqs(device->pci,
					     DRV_PCI_IRQ_ALLOW_MSIX | DRV_PCI_IRQ_ALLOW_MSI | DRV_PCI_IRQ_ALLOW_INTX,
					     1U,
					     1U,
					     &device->irq,
					     &count);
	if (error != 0)
		return error;

	/* Refuses an allocation that gave no vector. */
	if (count != 1U) {
		if (count != 0U)
			drv_pci_device_free_irqs(device->pci, &device->irq, count);
		return EIO;
	}

	/* Connects the handler. */
	error = drv_pci_device_establish_irq(device->pci,
					     &device->irq,
					     rtl_interrupt,
					     device,
					     "rtl8125",
					     &device->irq_cookie);
	if (error != 0) {
		drv_pci_device_free_irqs(device->pci, &device->irq, 1U);
		return error;
	}

	/* Succeeded: the vector reaches the handler. */
	return 0;
}

/*
 * Acknowledges the controller's interrupt and, for frames sent or
 * received, holds the interrupts off and wakes the network worker.
 *
 * Runs in interrupt context.  A shared pin may call it for another
 * device's interrupt, so it reports whether the controller had one.
 *
 * Procedure referenced: FreeBSD sys/dev/rge/if_rge.c, main
 * 8fa55fdcae4ecbd2ec13ef91cc9d172095e21189 (rge_intr_msi).
 */
static int
rtl_interrupt(
	void *argument)
{
	struct rtl_device *device;
	unsigned long irq;
	uint64_t started;
	uint32_t status;
	unsigned schedule;

	/* Notes when the interrupt began, for the profile. */
	started = profile_now();

	/* Samples and acknowledges what the controller latched. */
	device = argument;
	schedule = 0U;
	irq = spin_lock_irqsave(&device->lock);

	status = read32(device, RTL_REG_INTERRUPT_STATUS);

	/* Writes the latched bits back to clear them; all ones is a device gone from the bus. */
	if (status != 0U && status != 0xffffffffU) {
		write32(device, RTL_REG_INTERRUPT_STATUS, status);

		/* Remembers a system error for the poll to report. */
		if ((status & RTL_INTERRUPT_SYSTEM_ERROR) != 0)
			device->system_error = 1U;

		/* Holds the interrupts off until the worker has drained the rings. */
		if (device->opened && (status & RTL_INTERRUPTS_RUNNING) != 0) {
			write32(device, RTL_REG_INTERRUPT_MASK, 0U);
			schedule = 1U;
		}

		/* Counts the controller's interrupt in the profile. */
		profile_interrupt(device, started);
	}

	spin_unlock_irqrestore(&device->lock, irq);

	/* Nothing latched means the interrupt was another device's. */
	if (status == 0U || status == 0xffffffffU)
		return 0;

	/* Wakes the network worker outside the lock. */
	if (schedule)
		net_device_schedule_poll(device->net);

	/* The interrupt was the controller's. */
	return 1;
}

/* Publishes the controller as a network interface without a carrier. */
static int
publish_interface(
	struct rtl_device *device)
{
	unsigned index;
	int error;

	/* Names it after its slot. */
	index = (unsigned)(device - rtl_devices);
	device->name[0] = 'r';
	device->name[1] = 'g';
	device->name[2] = 'e';
	device->name[3] = (char)('0' + index);
	device->name[4] = '\0';

	/* Takes an interface slot. */
	device->net = net_device_alloc();
	if (device->net == NULL)
		return ENOMEM;

	/* Describes an Ethernet port with the controller's address. */
	kern_memcpy(device->net->name, device->name, 5U);
	device->net->mtu = RTL_DEFAULT_MTU;
	device->net->mtu_max = RTL_DEFAULT_MTU;
	device->net->hwaddr_len = 6U;
	kern_memcpy(device->net->hwaddr, device->mac, 6U);
	device->net->flags = NET_DEVICE_BROADCAST | NET_DEVICE_MULTICAST;
	device->net->capabilities = NET_DEVICE_CAP_TX_CSUM_IPV4_TCP;
	device->net->ops = &rtl_operations;
	device->net->driver_data = device;

	/* Publishes it. */
	error = net_device_create(device->net);
	if (error != 0)
		return error;

	/* Succeeded: the interface exists. */
	return 0;
}

/*
 * Opens the interface: the controller and the PHY are set up again, the
 * rings given to the controller, and the receiver, the transmitter and
 * the interrupts started.
 *
 * Procedure referenced: FreeBSD sys/dev/rge/if_rge.c, main
 * 8fa55fdcae4ecbd2ec13ef91cc9d172095e21189 (rge_init_locked).
 */
static int
rtl_open(
	struct net_device *net)
{
	struct rtl_device *device;
	unsigned long irq;
	uint32_t status;
	unsigned opened;
	int error;

	/* Samples whether the interface is open already. */
	device = net->driver_data;
	irq = spin_lock_irqsave(&device->lock);

	opened = device->opened;

	spin_unlock_irqrestore(&device->lock, irq);

	/* A repeated open keeps the running rings. */
	if (opened)
		return 0;

	/* Brings the link up from reset, which leaves the interrupts masked. */
	error = bring_up_link(device);
	if (error != 0) {
		kern_logf("rtl8125: %s link set-up failed (%d)\n", device->name, error);
		return error;
	}

	/*
	 * Empties both rings and sets the MAC up around them; nothing else
	 * touches the registers while the interface is not open.
	 */
	prepare_rings(device);
	configure_mac(device);

	/* Starts the receiver, the transmitter and the interrupts, and lets the service thread in. */
	irq = spin_lock_irqsave(&device->lock);

	write8(device, RTL_REG_COMMAND, RTL_COMMAND_TX_ENABLE | RTL_COMMAND_RX_ENABLE);
	write32(device, RTL_REG_TIMER_INTERRUPT0, 0U);
	write16(device, RTL_REG_INTERRUPT_MODERATION, 0U);
	status = read32(device, RTL_REG_INTERRUPT_STATUS);
	write32(device, RTL_REG_INTERRUPT_STATUS, status);
	write32(device, RTL_REG_INTERRUPT_MASK, RTL_INTERRUPTS_RUNNING);
	device->opened = 1U;

	spin_unlock_irqrestore(&device->lock, irq);

	/* Tells which PHY microcode the link runs on. */
	kern_logf("rtl8125: %s PHY microcode version 0x%04x\n", device->name, device->ram_code_version);

	/* Succeeded: frames move by interrupt and the service thread, the carrier follows the link. */
	return 0;
}

/*
 * Closes the interface: no more interrupts, the receiver and the
 * transmitter stopped and the MAC reset, and the packets the transmit ring
 * held freed.  The rings stay.
 *
 * Procedure referenced: FreeBSD sys/dev/rge/if_rge.c, main
 * 8fa55fdcae4ecbd2ec13ef91cc9d172095e21189 (rge_stop_locked).
 */
static void
rtl_close(
	struct net_device *net)
{
	struct rtl_device *device;
	struct packet_buf *done;
	unsigned long irq;
	unsigned slot;

	/* Closes the producers' gate and masks the interrupts. */
	device = net->driver_data;
	irq = spin_lock_irqsave(&device->lock);

	device->opened = 0U;
	write32(device, RTL_REG_INTERRUPT_MASK, 0U);

	spin_unlock_irqrestore(&device->lock, irq);

	/* Stops the receiver and the transmitter and waits for the DMA to stop. */
	stop_controller(device);

	/* Takes the packets the descriptors still held, as the DMA has stopped. */
	irq = spin_lock_irqsave(&device->lock);

	/* Puts each packet still held in place on the list to free. */
	for (slot = 0; slot < RTL_TX_COUNT; slot++) {
		/* A slot with a bounce copy or nothing has no packet to free. */
		if (device->tx_packets[slot] == NULL)
			continue;

		/* Moves the packet onto the list to free and empties the slot. */
		device->tx_packets[slot]->next = device->tx_done;
		device->tx_done = device->tx_packets[slot];
		device->tx_packets[slot] = NULL;
	}

	/* Takes the whole list for freeing outside the lock. */
	done = take_tx_done_locked(device);

	spin_unlock_irqrestore(&device->lock, irq);

	/* Frees them outside the lock. */
	free_tx_done(done);
}

/*
 * Hands one frame to the transmit ring, in place when the controller
 * reaches the packet's bytes, else copied into the slot's bounce buffer.
 *
 * Procedure referenced: FreeBSD sys/dev/rge/if_rge.c, main
 * 8fa55fdcae4ecbd2ec13ef91cc9d172095e21189 (rge_encap, rge_txstart).
 */
static int
rtl_transmit(
	struct net_device *net,
	struct packet_buf *packet)
{
	struct rtl_device *device;
	struct rtl_tx_descriptor *descriptor;
	struct packet_buf *done;
	uint8_t *buffer;
	uint64_t address;
	uint64_t physical;
	uint64_t started;
	unsigned long irq;
	uint32_t command;
	uint32_t extended;
	unsigned errors;
	unsigned slot;
	size_t length;
	int in_place;
	int error;

	/* Refuses a missing packet. */
	if (packet == NULL)
		return EINVAL;

	/* Notes when the transmit began, for the profile. */
	started = profile_now();

	/* Drops a frame too short for a header or too long for the interface. */
	device = net->driver_data;
	length = packet->length;
	if (length < RTL_HEADER_BYTES || length > RTL_FRAME_MAX) {
		packet_buf_free(packet);
		return EMSGSIZE;
	}

	/*
	 * The controller computes the checksums the stack left to it, but not
	 * those of a short frame, which is padded: they are computed here.
	 */
	extended = 0U;
	if ((packet->flags & PACKET_BUF_TX_CSUM_TCP) != 0) {
		if (length >= RTL_MIN_FRAME)
			extended = RTL_TX_CHECKSUM_IPV4 | RTL_TX_CHECKSUM_TCP;
		else
			packet_buf_checksum_finish(packet);
	}

	/*
	 * Points the descriptor at the packet itself when the controller
	 * reaches its bytes; a short frame, which is padded, is copied.
	 */
	in_place = 0;
	if (length >= RTL_MIN_FRAME) {
		error = packet_buf_physical(packet, &physical);
		if (error == 0) {
			error = drv_dma_physical_address(device->dma, physical, length, 64U, &address);
			if (error == 0)
				in_place = 1;
		}
	}

	/* Takes a free descriptor, if the interface runs and has a link. */
	error = 0;
	errors = 0U;
	irq = spin_lock_irqsave(&device->lock);

	if (!device->opened || !device->carrier) {
		error = ENETDOWN;
	} else {
		/*
		 * Frees what the controller finished only when the ring looks
		 * half full (the network worker's poll frees the rest), then
		 * checks one descriptor is left.
		 */
		if (device->tx_producer - device->tx_consumer >= RTL_TX_COUNT / 2U)
			errors = reclaim_tx(device);
		if (device->tx_producer - device->tx_consumer >= RTL_TX_COUNT - 1U)
			error = ENOBUFS;
	}

	/* Fills the descriptor and hands it over. */
	if (error == 0) {
		/*
		 * The packet in place, kept until the frame is sent; or the frame
		 * copied, a short one padded so no old bytes go out.
		 */
		slot = device->tx_producer % RTL_TX_COUNT;
		if (in_place) {
			device->tx_packets[slot] = packet;
		} else {
			buffer = (uint8_t *)device->tx_buffers.address + slot * RTL_BUFFER_BYTES;
			kern_memcpy(buffer, packet->data, length);

			/* Pads a short frame with zeros up to the shortest the wire carries. */
			if (length < RTL_MIN_FRAME) {
				kern_memset(buffer + length, 0, RTL_MIN_FRAME - length);
				length = RTL_MIN_FRAME;
			}

			/* Names the bounce buffer's bus address. */
			address = device->tx_buffers.device_address + slot * RTL_BUFFER_BYTES;
		}

		/* Points the descriptor at the bytes as one whole frame. */
		descriptor = &device->tx_ring[slot];
		descriptor->address_low = (uint32_t)address;
		descriptor->address_high = (uint32_t)(address >> 32);
		descriptor->extended = extended;
		command = RTL_DESCRIPTOR_OWN | RTL_TX_FIRST | RTL_TX_LAST | (uint32_t)length;

		/* Marks the last descriptor of the ring. */
		if (slot == RTL_TX_COUNT - 1U)
			command |= RTL_DESCRIPTOR_END_OF_RING;

		/* Hands the descriptor over only after the buffer and the other words are written. */
		kern_io_write_barrier();
		descriptor->command = command;

		/* Tells the controller the ring has a new frame, after the descriptor is written. */
		device->tx_producer++;
		kern_io_write_barrier();
		write16(device, RTL_REG_TX_START, RTL_TX_START_NORMAL);
	}

	/* Takes the finished packets for freeing and counts the transmit in the profile. */
	done = take_tx_done_locked(device);
	profile_transmit(device, started, in_place, error);

	spin_unlock_irqrestore(&device->lock, irq);

	/*
	 * Releases the packet unless a descriptor holds it, the packets of the
	 * frames finished, and counts the ones that failed, outside the lock.
	 */
	if (!in_place || error != 0)
		packet_buf_free(packet);
	free_tx_done(done);
	report_tx_errors(net, errors);

	/* Reports a closed interface, no link or a full ring. */
	if (error != 0)
		return error;

	/* Succeeded: the frame is on the ring. */
	return 0;
}

/*
 * Sends finished transmit descriptors back to the free side, receives up
 * to the budget's frames, and lets the controller interrupt again.
 */
static unsigned
rtl_poll(
	struct net_device *net,
	unsigned budget)
{
	struct rtl_device *device;
	struct packet_buf *done;
	unsigned long irq;
	uint64_t started;
	unsigned errors;
	unsigned count;
	unsigned report;

	/* Notes when the poll began, for the profile. */
	started = profile_now();

	/* Frees what the controller has sent. */
	device = net->driver_data;
	errors = 0U;
	irq = spin_lock_irqsave(&device->lock);

	/* Reclaims descriptors only while the interface is open. */
	if (device->opened)
		errors = reclaim_tx(device);

	/* Takes the finished packets for freeing outside the lock. */
	done = take_tx_done_locked(device);

	spin_unlock_irqrestore(&device->lock, irq);

	/* The packets of the sent frames, outside the lock. */
	free_tx_done(done);

	/* Counts sent frames that failed. */
	report_tx_errors(net, errors);

	/* Passes received frames to the stack. */
	count = receive_frames(net, budget);

	/*
	 * Lets the controller interrupt again once the receive ring is drained;
	 * a poll that used its whole budget is run again by the worker with the
	 * interrupts still held off, and a closed interface stays quiet.
	 */
	report = 0U;
	irq = spin_lock_irqsave(&device->lock);

	if (device->opened && count < budget)
		write32(device, RTL_REG_INTERRUPT_MASK, RTL_INTERRUPTS_RUNNING);

	/* Counts the poll in the profile. */
	profile_poll(device, started, count, budget);

	/* Takes a system error for reporting, once. */
	if (device->system_error && !device->system_error_reported) {
		device->system_error_reported = 1U;
		report = 1U;
	}

	spin_unlock_irqrestore(&device->lock, irq);

	/* Reports a system error once. */
	if (report)
		kern_logf("rtl8125: %s PCI Express system error\n", device->name);

	/* Reports the frames received, dropped ones included. */
	return count;
}

/*
 * Allocates the descriptors and the buffers of both rings.
 *
 * The controller reaches them through the PCI bus's DMA, whose addresses
 * the bus limits; the descriptors take a 64-bit address.
 */
static int
allocate_rings(
	struct rtl_device *device)
{
	size_t ring_bytes;
	int error;

	/* Takes the bus's DMA. */
	device->dma = drv_pci_device_dma(device->pci);
	if (device->dma == NULL)
		return ENODEV;

	/* Allocates the descriptors, transmit ring first. */
	ring_bytes = (RTL_TX_COUNT + RTL_RX_COUNT) * RTL_DESCRIPTOR_BYTES;
	error = drv_dma_alloc_coherent(device->dma, ring_bytes, RTL_RING_ALIGNMENT, &device->descriptors);
	if (error != 0)
		return error;
	device->descriptors_allocated = 1U;

	/* Allocates the transmit bounce buffers. */
	error = drv_dma_alloc_coherent(device->dma,
				       RTL_TX_COUNT * RTL_BUFFER_BYTES,
				       RTL_BUFFER_ALIGNMENT,
				       &device->tx_buffers);
	if (error != 0)
		return error;
	device->tx_buffers_allocated = 1U;

	/* Allocates the receive buffers. */
	error = drv_dma_alloc_coherent(device->dma,
				       RTL_RX_COUNT * RTL_BUFFER_BYTES,
				       RTL_BUFFER_ALIGNMENT,
				       &device->rx_buffers);
	if (error != 0)
		return error;
	device->rx_buffers_allocated = 1U;

	/* Splits the descriptor block into the two rings. */
	device->tx_ring = device->descriptors.address;
	device->rx_ring = (struct rtl_rx_descriptor *)(device->tx_ring + RTL_TX_COUNT);
	device->tx_ring_address = device->descriptors.device_address;
	device->rx_ring_address = device->tx_ring_address + RTL_TX_COUNT * RTL_DESCRIPTOR_BYTES;

	/* Succeeded: the rings and buffers exist, unseen by the controller. */
	return 0;
}

/*
 * Empties the transmit ring and gives every receive descriptor to the
 * controller.  The last descriptor of each ring marks the ring's end.
 *
 * Procedure referenced: FreeBSD sys/dev/rge/if_rge.c, main
 * 8fa55fdcae4ecbd2ec13ef91cc9d172095e21189 (rge_rx_list_init,
 * rge_tx_list_init, rge_newbuf).
 */
static void
prepare_rings(
	struct rtl_device *device)
{
	unsigned slot;

	/* Takes every descriptor back to the CPU. */
	kern_memset(device->tx_ring, 0, RTL_TX_COUNT * RTL_DESCRIPTOR_BYTES);
	kern_memset(device->rx_ring, 0, RTL_RX_COUNT * RTL_DESCRIPTOR_BYTES);

	/* Starts both rings at their first descriptor. */
	device->tx_producer = 0U;
	device->tx_consumer = 0U;
	device->rx_consumer = 0U;

	/* Marks the end of the transmit ring. */
	device->tx_ring[RTL_TX_COUNT - 1U].command = RTL_DESCRIPTOR_END_OF_RING;

	/* Gives every receive descriptor to the controller. */
	for (slot = 0; slot < RTL_RX_COUNT; slot++)
		arm_rx_slot(device, slot);

	/* Makes the descriptors visible before the controller is told where they are. */
	kern_io_write_barrier();
}

/*
 * Points one receive descriptor at its buffer and gives it to the
 * controller.
 *
 * Procedure referenced: FreeBSD sys/dev/rge/if_rge.c, main
 * 8fa55fdcae4ecbd2ec13ef91cc9d172095e21189 (rge_newbuf).
 */
static void
arm_rx_slot(
	struct rtl_device *device,
	unsigned slot)
{
	struct rtl_rx_descriptor *descriptor;
	uint64_t address;
	uint32_t command;

	/* Points the descriptor at the slot's buffer, whose size it carries. */
	descriptor = &device->rx_ring[slot];
	address = device->rx_buffers.device_address + slot * RTL_BUFFER_BYTES;
	descriptor->address_low = (uint32_t)address;
	descriptor->address_high = (uint32_t)(address >> 32);
	descriptor->extended = 0U;
	command = RTL_BUFFER_BYTES;
	if (slot == RTL_RX_COUNT - 1U)
		command |= RTL_DESCRIPTOR_END_OF_RING;

	/* Hands it over after the address is written. */
	kern_io_write_barrier();
	descriptor->command = command | RTL_DESCRIPTOR_OWN;
}

/*
 * Sets the MAC up around the rings: their addresses, the transmit and
 * receive configurations, the MAC's OCP settings, the interrupt type and
 * the receive filter.  The receiver, the transmitter and the interrupts
 * stay off.
 *
 * Procedure referenced: FreeBSD sys/dev/rge/if_rge.c, main
 * 8fa55fdcae4ecbd2ec13ef91cc9d172095e21189 (rge_init_locked).
 */
static void
configure_mac(
	struct rtl_device *device)
{
	uint8_t lock;

	/* Writes the station address again, as the open's resets came after attach wrote it. */
	read_station_address(device);

	/* Opens the configuration registers for writing. */
	lock = read8(device, RTL_REG_CONFIG_LOCK);
	write8(device, RTL_REG_CONFIG_LOCK, (uint8_t)(lock | RTL_CONFIG_LOCK_WRITABLE));

	/* Keeps the link out of CLKREQ power saving. */
	clear8(device, RTL_REG_MISC_F1, RTL_MISC_F1_BIT);
	disable_clock_request(device);
	write16(device, RTL_REG_EEE_TX_IDLE, (uint16_t)RTL_EEE_TX_IDLE_VALUE);

	/* Tells the controller where the rings are and how the function behaves on the link. */
	configure_rings_and_pcie(device);

	/* Applies the MAC's internal settings. */
	configure_mac_ocp(device);

	/* Runs the interrupts unmoderated on one vector. */
	configure_interrupt_type(device);

	/* Sets the receiver's frame size and filter. */
	configure_receiver(device);

	/* Keeps CLKREQ off and locks the configuration registers again. */
	disable_clock_request(device);
	clear8(device, RTL_REG_CONFIG_LOCK, RTL_CONFIG_LOCK_WRITABLE);
	kern_usleep_range(10U, 10U);
}

/*
 * Writes the rings' addresses, the transmit and receive configurations
 * and the function's PCI Express settings, and runs one receive queue.
 *
 * Procedure and values referenced: FreeBSD sys/dev/rge/if_rge.c, main
 * 8fa55fdcae4ecbd2ec13ef91cc9d172095e21189 (rge_init_locked, from the
 * descriptor addresses to RGE_CFG1).
 */
static void
configure_rings_and_pcie(
	struct rtl_device *device)
{
	uint32_t config;
	uint32_t value;
	uint16_t queues;

	/* Tells the controller where the rings are. */
	write32(device, RTL_REG_RX_RING_LOW, (uint32_t)device->rx_ring_address);
	write32(device, RTL_REG_RX_RING_HIGH, (uint32_t)(device->rx_ring_address >> 32));
	write32(device, RTL_REG_TX_RING_LOW, (uint32_t)device->tx_ring_address);
	write32(device, RTL_REG_TX_RING_HIGH, (uint32_t)(device->tx_ring_address >> 32));

	/* Picks the member's receive configuration, which selects the 32-byte descriptors. */
	switch (device->chip->family) {
	case RTL_FAMILY_8125:
		config = RTL_RX_CONFIG_8125;
		break;
	case RTL_FAMILY_8125B:
		config = RTL_RX_CONFIG_8125B;
		break;
	case RTL_FAMILY_8125D:
		config = RTL_RX_CONFIG_8125D;
		break;
	default:
		config = RTL_RX_CONFIG_8126;
		break;
	}

	/* Writes the receive and transmit configurations. */
	write32(device, RTL_REG_RX_CONFIG, config);
	write32(device, RTL_REG_TX_CONFIG, RTL_TX_CONFIG_START);

	/* Sets the function's PCI Express settings in its configuration space. */
	value = csi_read(device, 0x070cU) & ~0x3f000000U;
	csi_write(device, 0x070cU, value | 0x27000000U);

	/* Sets the link's L1 timeout or latency tolerance as the member needs. */
	if (device->chip->family == RTL_FAMILY_8126 || device->chip->family == RTL_FAMILY_8127) {
		/* Turns the L1 timeout off. */
		value = csi_read(device, 0x0890U) & ~0x00000001U;
		csi_write(device, 0x0890U, value);
	} else if (device->chip->family != RTL_FAMILY_8125D) {
		/* Sets the older parts' latency tolerance. */
		write16(device, RTL_REG_LTR_8125, 0x221bU);
	}

	/* Runs one receive queue without receive-side scaling. */
	write32(device, RTL_REG_RSS_CONTROL, 0U);
	queues = read16(device, RTL_REG_RX_QUEUE_CONTROL);
	write16(device, RTL_REG_RX_QUEUE_CONTROL, (uint16_t)(queues & ~0x001cU));

	/* Keeps the link from slowing down on its own. */
	clear8(device, RTL_REG_CONFIG1, RTL_CONFIG1_SPEED_DOWN);
}

/*
 * Applies the MAC's OCP settings for the 32-byte descriptors and one
 * queue.
 *
 * Procedure and values referenced: FreeBSD sys/dev/rge/if_rge.c, main
 * 8fa55fdcae4ecbd2ec13ef91cc9d172095e21189 (rge_init_locked, from MAC OCP
 * 0xc140 to 0x1880); OpenBSD sys/dev/pci/if_rge.c 1.48 (rge_init: MAC OCP
 * 0xd40c, which the FreeBSD driver writes through the PHY's window).
 */
static void
configure_mac_ocp(
	struct rtl_device *device)
{
	enum rtl_family family;
	uint16_t value;

	/* Opens the MAC's filters fully. */
	family = device->chip->family;
	mac_ocp_write(device, 0xc140U, 0xffffU);
	mac_ocp_write(device, 0xc142U, 0xffffU);

	/* Selects the 32-byte descriptors. */
	mac_ocp_set(device, 0xeb58U, 0x0001U);

	/* Clears the descriptor mode bits of the RTL8126 and RTL8127, and the RTL8127's further ones. */
	if (family == RTL_FAMILY_8126 || family == RTL_FAMILY_8127) {
		clear8(device, RTL_REG_DESCRIPTOR_MODE, 0x02U);
		if (family == RTL_FAMILY_8127) {
			clear8(device, RTL_REG_DESCRIPTOR_8127, 0x04U);
			mac_ocp_clear(device, 0xe00cU, 0x1000U);
			mac_ocp_clear(device, 0xc0c2U, 0x0040U);
		}
	}

	/* Sets the transmit FIFO thresholds by the member. */
	value = mac_ocp_read(device, 0xe614U);
	if (family == RTL_FAMILY_8127) {
		value = (uint16_t)((value & ~0x0f00U) | 0x0f00U);
	} else if (family == RTL_FAMILY_8125B) {
		value = (uint16_t)((value & ~0x0700U) | 0x0200U);
	} else {
		value = (uint16_t)((value & ~0x0700U) | 0x0300U);
	}

	/* Writes the thresholds back. */
	mac_ocp_write(device, 0xe614U, value);

	/* Runs one transmit queue. */
	mac_ocp_modify(device, 0xe63eU, 0x0c00U, 0U);
	mac_ocp_modify(device, 0xe63eU, 0x0030U, 0x0020U);

	/* Restarts the MAC's transmit engine with its flow settings. */
	mac_ocp_clear(device, 0xc0b4U, 0x0001U);
	mac_ocp_set(device, 0xc0b4U, 0x0001U);
	mac_ocp_set(device, 0xc0b4U, 0x000cU);
	mac_ocp_modify(device, 0xeb6aU, 0x00ffU, 0x0033U);
	mac_ocp_modify(device, 0xeb50U, 0x03e0U, 0x0040U);
	mac_ocp_clear(device, 0xe056U, 0x00f0U);
	write8(device, RTL_REG_TX_FIRST_NUMBER, 0x10U);
	mac_ocp_clear(device, 0xe040U, 0x1000U);
	mac_ocp_modify(device, 0xea1cU, 0x0003U, 0x0001U);

	/* Selects the flow setting of the member. */
	if (family == RTL_FAMILY_8125D) {
		mac_ocp_write(device, 0xe0c0U, 0x4403U);
	} else {
		mac_ocp_write(device, 0xe0c0U, 0x4000U);
	}

	/* Sets and clears the remaining flow bits. */
	mac_ocp_set(device, 0xe052U, 0x0060U);
	mac_ocp_clear(device, 0xe052U, 0x0088U);
	mac_ocp_modify(device, 0xd430U, 0x0fffU, 0x045fU);

	/* Lets the link's DLL power down in the states that allow it. */
	set8(device, RTL_REG_DLL_POWER, RTL_DLL_POWER_BITS);

	/* The first RTL8125 sets one more bit of the MCU command register. */
	if (family == RTL_FAMILY_8125)
		set8(device, RTL_REG_MCU_COMMAND, 0x01U);

	/* Turns EEE plus off where the part has it. */
	if (family != RTL_FAMILY_8125D)
		mac_ocp_clear(device, 0xe080U, 0x0002U);

	/* Clears the advanced EEE bits; the RTL8126 and RTL8127 have one more. */
	if (family == RTL_FAMILY_8126 || family == RTL_FAMILY_8127) {
		mac_ocp_clear(device, 0xea1cU, 0x0304U);
	} else {
		mac_ocp_clear(device, 0xea1cU, 0x0004U);
	}

	/* Clears the TCAM entries. */
	mac_ocp_set(device, 0xeb54U, 0x0001U);
	kern_usleep_range(1U, 1U);
	mac_ocp_clear(device, 0xeb54U, 0x0001U);
	clear16(device, RTL_REG_RX_LINK_8127, 0x0030U);

	/* Sets the RTL8127's MAC power settings. */
	if (family == RTL_FAMILY_8127)
		mac_ocp_modify(device, 0xd40cU, 0xe038U, 0x8020U);
}

/*
 * Runs the interrupts on one vector without moderation: no timer
 * interrupts and every mitigation register cleared.
 *
 * Procedure and values referenced: OpenBSD sys/dev/pci/if_rge.c 1.48
 * (rge_init, "Config interrupt type", the single-vector RTL8127 case);
 * FreeBSD sys/dev/rge/if_rge.c, main
 * 8fa55fdcae4ecbd2ec13ef91cc9d172095e21189 (rge_init_locked, from the
 * interrupt type to MAC OCP 0xe092).
 */
static void
configure_interrupt_type(
	struct rtl_device *device)
{
	enum rtl_family family;
	unsigned count;
	unsigned index;
	uint32_t value;
	uint8_t config;

	/* Takes the RTL8127 out of its multi-vector mode; the others out of the interrupt engine. */
	family = device->chip->family;
	if (family == RTL_FAMILY_8127) {
		config = read8(device, RTL_REG_INTERRUPT_CONFIG0);
		config &= (uint8_t)~(RTL_INTERRUPT_CONFIG0_ENABLE | RTL_INTERRUPT_CONFIG0_AUTO_CLEAR |
				     RTL_INTERRUPT_CONFIG0_AVOID_MISS);
		write8(device, RTL_REG_INTERRUPT_CONFIG0, config);
	} else if (family != RTL_FAMILY_8125) {
		clear8(device, RTL_REG_INTERRUPT_CONFIG0, RTL_INTERRUPT_CONFIG0_ENABLE);
	}

	/* Clears the timer interrupts. */
	write32(device, RTL_REG_TIMER_INTERRUPT0, 0U);
	write32(device, RTL_REG_TIMER_INTERRUPT1, 0U);
	write32(device, RTL_REG_TIMER_INTERRUPT2, 0U);
	write32(device, RTL_REG_TIMER_INTERRUPT3, 0U);

	/* Clears every interrupt mitigation register the part has. */
	count = RTL_MITIGATION_COUNT_LONG;
	if (family == RTL_FAMILY_8125B || family == RTL_FAMILY_8126)
		count = RTL_MITIGATION_COUNT_SHORT;
	for (index = 0; index < count; index++)
		write32(device, RTL_REG_MITIGATION_FIRST + index * 4U, 0U);

	/* Turns the RTL8126's interrupt bypasses off. */
	if (family == RTL_FAMILY_8126) {
		clear8(device,
		       RTL_REG_INTERRUPT_CONFIG0,
		       RTL_INTERRUPT_CONFIG0_TIMEOUT_BYPASS | RTL_INTERRUPT_CONFIG0_RDU_BYPASS |
			       RTL_INTERRUPT_CONFIG0_MITIGATION_BYPASS);
		write16(device, RTL_REG_INTERRUPT_CONFIG1, 0U);
	}

	/* Applies the MAC's remaining settings. */
	mac_ocp_set(device, 0xc0acU, 0x1f80U);
	mac_ocp_write(device, 0xe098U, 0xc302U);
	mac_ocp_clear(device, 0xe032U, 0x0003U);
	value = csi_read(device, 0x0098U) & ~0x0000ff00U;
	csi_write(device, 0x0098U, value);

	/* Sets the last window of the MAC's settings as the member needs. */
	if (family == RTL_FAMILY_8125D) {
		mac_ocp_modify(device, 0xe092U, 0x00ffU, 0x0008U);
	} else {
		mac_ocp_clear(device, 0xe092U, 0x00ffU);
	}
}

/*
 * Sets the receiver up: no VLAN stripping or checksum offload, frames no
 * longer than one buffer, the receive data path open, and a filter that
 * takes the station's frames, broadcasts and every multicast, as the stack
 * keeps no multicast list for the driver.
 *
 * Procedure and values referenced: FreeBSD sys/dev/rge/if_rge.c, main
 * 8fa55fdcae4ecbd2ec13ef91cc9d172095e21189 (rge_init_locked, from the
 * VLAN tagging to RGE_RADMFIFO_PROTECT; rge_iff_locked).
 */
static void
configure_receiver(
	struct rtl_device *device)
{
	uint32_t filter;

	/* Leaves VLAN tags to the stack, and checks the checksums of what it receives. */
	clear32(device, RTL_REG_RX_CONFIG, RTL_RX_CONFIG_VLAN_STRIP);
	write16(device, RTL_REG_CPLUS_COMMAND,
		(uint16_t)(read16(device, RTL_REG_CPLUS_COMMAND) | RTL_CPLUS_RX_CHECKSUM));
	(void)read16(device, RTL_REG_CPLUS_COMMAND);

	/* Takes no frame longer than one receive buffer. */
	write16(device, RTL_REG_RX_MAX_SIZE, (uint16_t)RTL_BUFFER_BYTES);

	/* Opens the receive data path and gives it time to settle. */
	clear8(device, RTL_REG_POWER_SWITCH, RTL_POWER_SWITCH_RX_GATE);
	kern_usleep_range(2000U, 2000U);

	/* Takes the station's frames, broadcasts and every multicast. */
	filter = read32(device, RTL_REG_RX_CONFIG);
	filter &= ~RTL_RX_CONFIG_ALL_PHYSICAL;
	filter |= RTL_RX_CONFIG_OWN_ADDRESS | RTL_RX_CONFIG_BROADCAST | RTL_RX_CONFIG_MULTICAST;
	write32(device, RTL_REG_RX_CONFIG, filter);
	write32(device, RTL_REG_MULTICAST0, 0xffffffffU);
	write32(device, RTL_REG_MULTICAST4, 0xffffffffU);

	/* Clears the RTL8127's receive FIFO protection (the low byte of the drivers' 0x2001). */
	if (device->chip->family == RTL_FAMILY_8127)
		clear8(device, RTL_REG_RX_FIFO_PROTECT, 0x01U);
}

/*
 * Stops the controller: no frame accepted, the interrupts and timers
 * cleared, the FIFOs drained and the MAC reset.
 *
 * Procedure referenced: FreeBSD sys/dev/rge/if_rge.c, main
 * 8fa55fdcae4ecbd2ec13ef91cc9d172095e21189 (rge_stop_locked) and
 * sys/dev/rge/if_rge_hw.c, the same commit (rge_hw_reset).
 */
static void
stop_controller(
	struct rtl_device *device)
{
	int error;

	/* Stops accepting frames, then clears the interrupts and the timers. */
	clear32(device, RTL_REG_RX_CONFIG, RTL_RX_CONFIG_ACCEPT_MASK);
	mask_interrupts(device);
	write32(device, RTL_REG_TIMER_INTERRUPT0, 0U);
	write32(device, RTL_REG_TIMER_INTERRUPT1, 0U);
	write32(device, RTL_REG_TIMER_INTERRUPT2, 0U);
	write32(device, RTL_REG_TIMER_INTERRUPT3, 0U);

	/* Drains the FIFOs and resets the MAC, which stops its DMA. */
	error = reset_controller(device);
	if (error != 0)
		kern_logf("rtl8125: %s reset at close timed out\n", device->name);

	/* Turns the MAC's transmit settings back off. */
	mac_ocp_clear(device, 0xc0acU, 0x1f80U);
}

/*
 * Moves the transmit consumer past every descriptor the controller has
 * finished.  The caller holds the lock.
 *
 * Returns how many of them failed on the wire.
 *
 * Procedure referenced: FreeBSD sys/dev/rge/if_rge.c, main
 * 8fa55fdcae4ecbd2ec13ef91cc9d172095e21189 (rge_txeof).
 */
static unsigned
reclaim_tx(
	struct rtl_device *device)
{
	struct rtl_tx_descriptor *descriptor;
	struct packet_buf *packet;
	uint32_t status;
	unsigned errors;
	unsigned slot;

	/* Walks the ring from the oldest frame on it. */
	errors = 0U;
	while (device->tx_consumer != device->tx_producer) {
		/* Stops at the first descriptor the controller still owns. */
		slot = device->tx_consumer % RTL_TX_COUNT;
		descriptor = &device->tx_ring[slot];
		status = descriptor->command;
		if ((status & RTL_DESCRIPTOR_OWN) != 0)
			break;

		/* Counts a frame the MAC reported an error for. */
		kern_io_read_barrier();
		if ((status & RTL_TX_ERROR) != 0)
			errors++;

		/* Frees the descriptor, keeping the ring's end, and puts the packet it held on the list to free. */
		descriptor->command = status & RTL_DESCRIPTOR_END_OF_RING;
		packet = device->tx_packets[slot];
		if (packet != NULL) {
			device->tx_packets[slot] = NULL;
			packet->next = device->tx_done;
			device->tx_done = packet;
		}

		/* Moves the consumer past the descriptor it has freed. */
		device->tx_consumer++;
	}

	/* Reports the failed frames. */
	return errors;
}

/*
 * Takes the list of the packets whose frames the controller has finished,
 * for the caller to free once it has released the lock.  The caller holds
 * the lock.
 */
static struct packet_buf *
take_tx_done_locked(
	struct rtl_device *device)
{
	struct packet_buf *list;

	/* The whole list, the device's emptied. */
	list = device->tx_done;
	device->tx_done = NULL;

	/* Succeeded: the list, or none. */
	return list;
}

/* Frees a list of packets linked by next. */
static void
free_tx_done(
	struct packet_buf *list)
{
	struct packet_buf *next;

	/* Frees each packet in turn. */
	while (list != NULL) {
		next = list->next;
		list->next = NULL;
		packet_buf_free(list);
		list = next;
	}
}

/*
 * Tells which checksums of a received frame the controller checked and
 * found right, from its descriptor's extended word, as packet flags
 * (PACKET_BUF_RX_CSUM_*): the IPv4 header's, and the TCP or UDP one over
 * IPv4 or IPv6.
 *
 * Procedure referenced: FreeBSD sys/dev/rge/if_rge.c, main
 * 8fa55fdcae4ecbd2ec13ef91cc9d172095e21189 (rge_rxeof, the checksum flags).
 */
static uint32_t
rx_checksum_flags(
	uint32_t extended)
{
	uint32_t flags;

	/* The IPv4 header's. */
	flags = 0U;
	if ((extended & RTL_RX_IPV4) != 0 && (extended & RTL_RX_IPV4_CHECKSUM_ERROR) == 0)
		flags |= PACKET_BUF_RX_CSUM_IPV4;

	/* The transport's, over either version. */
	if ((extended & (RTL_RX_IPV4 | RTL_RX_IPV6)) != 0) {
		if ((extended & RTL_RX_TCP) != 0 && (extended & RTL_RX_TCP_CHECKSUM_ERROR) == 0)
			flags |= PACKET_BUF_RX_CSUM_L4;
		if ((extended & RTL_RX_UDP) != 0 && (extended & RTL_RX_UDP_CHECKSUM_ERROR) == 0)
			flags |= PACKET_BUF_RX_CSUM_L4;
	}

	/* Reports the checksums found right. */
	return flags;
}

/*
 * Copies up to the budget's received frames into packets, gives their
 * descriptors back, and passes the packets to the stack.
 *
 * Procedure referenced: FreeBSD sys/dev/rge/if_rge.c, main
 * 8fa55fdcae4ecbd2ec13ef91cc9d172095e21189 (rge_rxeof).
 */
static unsigned
receive_frames(
	struct net_device *net,
	unsigned budget)
{
	struct rtl_device *device;
	struct rtl_rx_descriptor *descriptor;
	struct packet_buf *packet;
	unsigned long irq;
	uint32_t status;
	uint8_t *payload;
	unsigned slot;
	unsigned count;
	size_t length;
	int alloc_error;
	int valid;
	int empty;

	/* Takes at most the budget, so one port cannot starve the worker. */
	device = net->driver_data;
	for (count = 0; count < budget; count++) {
		packet = NULL;
		valid = 0;
		empty = 0;
		irq = spin_lock_irqsave(&device->lock);

		/* Looks at the oldest descriptor; a closed ring or one the controller owns ends the batch. */
		slot = device->rx_consumer % RTL_RX_COUNT;
		descriptor = &device->rx_ring[slot];
		status = descriptor->command;
		if (!device->opened || (status & RTL_DESCRIPTOR_OWN) != 0)
			empty = 1;

		/* Reads the frame the controller wrote back. */
		if (!empty) {
			/* Reads the buffer only after the write-back that released it. */
			kern_io_read_barrier();
			length = status & RTL_RX_LENGTH_MASK;

			/* Takes a whole, good frame that fits one buffer; counts anything else. */
			if ((status & (RTL_RX_FIRST | RTL_RX_LAST)) == (RTL_RX_FIRST | RTL_RX_LAST) &&
			    (status & RTL_RX_ERROR) == 0 &&
			    length >= RTL_HEADER_BYTES + RTL_CRC_BYTES &&
			    length <= RTL_FRAME_MAX + RTL_CRC_BYTES) {
				/*
				 * Copies the frame without its checksum into a packet;
				 * without one the frame is dropped.
				 */
				length -= RTL_CRC_BYTES;
				payload = NULL;
				packet = packet_buf_alloc_for(PACKET_BUF_DEFAULT_HEADROOM,
							      length,
							      PACKET_BUF_ALLOC_RECEIVE,
							      &alloc_error);
				if (packet != NULL)
					payload = packet_buf_append(packet, length);
				if (payload != NULL) {
					kern_memcpy(payload,
						    (uint8_t *)device->rx_buffers.address + slot * RTL_BUFFER_BYTES,
						    length);
					packet->flags |= rx_checksum_flags(descriptor->extended);
					valid = 1;
				}
			} else {
				/* A frame with an error, split across buffers, or of a size the stack does not take. */
				net->rx_errors++;
			}

			/* Counts a frame dropped for want of a packet or for an error. */
			if (!valid)
				net->rx_dropped++;

			/* Gives the descriptor back to the controller. */
			arm_rx_slot(device, slot);
			device->rx_consumer++;
		}

		spin_unlock_irqrestore(&device->lock, irq);

		/* Ends the batch at an empty ring. */
		if (empty) {
			if (packet != NULL)
				packet_buf_free(packet);
			break;
		}

		/* Passes the packet to the stack, or frees one that holds no frame. */
		if (valid) {
			net_device_receive(net, packet);
		} else if (packet != NULL) {
			packet_buf_free(packet);
		}
	}

	/* Reports the frames taken off the ring, dropped ones included. */
	return count;
}

/* Counts sent frames that failed on the wire. */
static void
report_tx_errors(
	struct net_device *net,
	unsigned errors)
{
	/* Records each failure once. */
	while (errors != 0) {
		net_device_tx_error(net);
		errors--;
	}
}

/* Sets bits of one byte register. */
static void
set8(
	struct rtl_device *device,
	uint32_t offset,
	uint8_t bits)
{
	uint8_t value;

	/* Writes the register back with the bits. */
	value = read8(device, offset);
	write8(device, offset, (uint8_t)(value | bits));
}

/* Clears bits of one byte register. */
static void
clear8(
	struct rtl_device *device,
	uint32_t offset,
	uint8_t bits)
{
	uint8_t value;

	/* Writes the register back without the bits. */
	value = read8(device, offset);
	write8(device, offset, (uint8_t)(value & ~bits));
}

/* Clears the mask bits and sets the value bits of one PHY OCP register. */
static int
phy_ocp_modify(
	struct rtl_device *device,
	uint16_t reg,
	uint16_t mask,
	uint16_t value)
{
	uint16_t current;
	int error;

	/* Reads the register. */
	error = phy_ocp_read(device, reg, &current);
	if (error != 0)
		return error;

	/* Writes it back changed. */
	error = phy_ocp_write(device, reg, (uint16_t)((current & ~mask) | value));
	if (error != 0)
		return error;

	/* Succeeded: the register holds the new bits. */
	return 0;
}

/* Reads one clause-22 register of the PHY through its OCP space. */
static int
mii_read(
	struct rtl_device *device,
	unsigned reg,
	uint16_t *value)
{
	int error;

	/* Reads the register's OCP copy. */
	error = phy_ocp_read(device, (uint16_t)(RTL_PHY_OCP_MII_BASE + reg * 2U), value);
	if (error != 0)
		return error;

	/* Succeeded: the value is the register's. */
	return 0;
}

/* Writes one clause-22 register of the PHY through its OCP space. */
static int
mii_write(
	struct rtl_device *device,
	unsigned reg,
	uint16_t value)
{
	int error;

	/* Writes the register's OCP copy. */
	error = phy_ocp_write(device, (uint16_t)(RTL_PHY_OCP_MII_BASE + reg * 2U), value);
	if (error != 0)
		return error;

	/* Succeeded: the PHY took the value. */
	return 0;
}

/*
 * Reads one longword of the function's configuration space through the
 * controller's CSI window.
 *
 * The window sets its flag once the data has arrived; the read goes on
 * after a bounded wait either way.
 *
 * Procedure referenced: OpenBSD sys/dev/pci/if_rge.c 1.48 (rge_read_csi).
 */
static uint32_t
csi_read(
	struct rtl_device *device,
	uint32_t reg)
{
	uint32_t window;
	uint32_t value;
	unsigned poll;

	/* Names the longword with every byte enabled. */
	write32(device, RTL_REG_CSI_ADDRESS, (reg & RTL_CSI_ADDRESS_MASK) | RTL_CSI_BYTE_ENABLE);

	/* Waits for the data. */
	for (poll = 0; poll < RTL_OCP_POLLS; poll++) {
		kern_usleep_range(RTL_OCP_POLL_US, RTL_OCP_POLL_US);

		/* The data is valid once the flag is set. */
		window = read32(device, RTL_REG_CSI_ADDRESS);
		if ((window & RTL_CSI_BUSY) != 0)
			break;
	}

	/* Gives the window time to settle, then reads the longword. */
	kern_usleep_range(RTL_CSI_SETTLE_US, RTL_CSI_SETTLE_US);
	value = read32(device, RTL_REG_CSI_DATA);

	/* Reports the longword the window returned. */
	return value;
}

/*
 * Writes one longword of the function's configuration space through the
 * controller's CSI window.
 *
 * Procedure referenced: OpenBSD sys/dev/pci/if_rge.c 1.48 (rge_write_csi).
 */
static void
csi_write(
	struct rtl_device *device,
	uint32_t reg,
	uint32_t value)
{
	uint32_t window;
	unsigned poll;

	/* Hands over the data and names the longword with every byte enabled. */
	write32(device, RTL_REG_CSI_DATA, value);
	write32(device,
		RTL_REG_CSI_ADDRESS,
		(reg & RTL_CSI_ADDRESS_MASK) | RTL_CSI_BYTE_ENABLE | RTL_CSI_BUSY);

	/* Waits for the write to finish; the window settles either way. */
	for (poll = 0; poll < RTL_OCP_POLLS; poll++) {
		kern_usleep_range(RTL_OCP_POLL_US, RTL_OCP_POLL_US);

		/* The write is done once the flag is clear. */
		window = read32(device, RTL_REG_CSI_ADDRESS);
		if ((window & RTL_CSI_BUSY) == 0)
			break;
	}

	/* Gives the window time to settle. */
	kern_usleep_range(RTL_CSI_SETTLE_US, RTL_CSI_SETTLE_US);
}

/*
 * Writes one register of the PCI Express PHY.
 *
 * Procedure referenced: OpenBSD sys/dev/pci/if_rge.c 1.48 (rge_write_ephy).
 */
static void
ephy_write(
	struct rtl_device *device,
	uint16_t reg,
	uint16_t value)
{
	uint32_t window;
	unsigned poll;

	/* Names the register and the value with the write bit. */
	window = (uint32_t)(reg & RTL_EPHY_ADDRESS_MASK) << RTL_EPHY_ADDRESS_SHIFT;
	window |= RTL_EPHY_BUSY | (value & RTL_EPHY_DATA_MASK);
	write32(device, RTL_REG_EPHY, window);

	/* Waits for the write to finish; the window settles either way. */
	for (poll = 0; poll < RTL_OCP_POLLS; poll++) {
		kern_usleep_range(RTL_OCP_POLL_US, RTL_OCP_POLL_US);

		/* The write is done once the flag is clear. */
		window = read32(device, RTL_REG_EPHY);
		if ((window & RTL_EPHY_BUSY) == 0)
			break;
	}

	/* Gives the window time to settle. */
	kern_usleep_range(RTL_CSI_SETTLE_US, RTL_CSI_SETTLE_US);
}

/* Runs a PHY register sequence in order and stops at the first PHY that does not answer. */
static int
run_phy_steps(
	struct rtl_device *device,
	const struct rtl_phy_step *steps,
	unsigned count)
{
	uint16_t ignored;
	unsigned index;
	int error;

	/* Applies each step. */
	for (index = 0; index < count; index++) {
		/* Does what the step says. */
		switch (steps[index].kind) {
		case RTL_PHY_STEP_WRITE:
			error = phy_ocp_write(device, steps[index].reg, steps[index].value);
			break;
		case RTL_PHY_STEP_MODIFY:
			error = phy_ocp_modify(device, steps[index].reg, steps[index].mask, steps[index].value);
			break;
		case RTL_PHY_STEP_PATCH:
			error = set_phy_patch_mode(device, steps[index].value != 0U);
			break;
		default:
			error = phy_ocp_read(device, steps[index].reg, &ignored);
			break;
		}

		/* A PHY that stops answering ends the sequence. */
		if (error != 0)
			return error;
	}

	/* Succeeded: every step was applied. */
	return 0;
}

/*
 * Brings the controller and its PHY from reset to an autonegotiating link.
 *
 * It runs in thread context because the PHY's reset takes up to seconds.
 * The MAC's and the PHY's microcode patches are not loaded: the MAC's
 * microcode stays stopped and the PHY runs what it came up with.
 *
 * Procedure referenced: OpenBSD sys/dev/pci/if_rge.c 1.48 (rge_init,
 * rge_chipinit, rge_phy_config, rge_ifmedia_upd).
 */
static int
bring_up_link(
	struct rtl_device *device)
{
	int error;

	/* Takes the controller back from the firmware and resets it. */
	error = exit_out_of_band(device);
	if (error != 0)
		return error;

	/* Powers the PHY up. */
	error = power_up_phy(device);
	if (error != 0)
		return error;

	/* Sets the MAC up without its microcode, then resets it with the interrupts masked. */
	configure_hardware(device);
	mask_interrupts(device);
	write32(device, RTL_REG_TIMER_INTERRUPT0, 0U);
	write32(device, RTL_REG_TIMER_INTERRUPT1, 0U);
	write32(device, RTL_REG_TIMER_INTERRUPT2, 0U);
	write32(device, RTL_REG_TIMER_INTERRUPT3, 0U);
	error = reset_controller(device);
	if (error != 0)
		return error;

	/* Sets the PCI Express PHY up. */
	configure_ephy(device);

	/* Resets the PHY with nothing advertised. */
	error = reset_phy(device);
	if (error != 0)
		return error;

	/* Applies the PHY's settings. */
	error = configure_phy(device);
	if (error != 0)
		return error;

	/* Turns energy-efficient Ethernet off. */
	error = disable_eee(device);
	if (error != 0)
		return error;

	/* Advertises every speed the part has and restarts the negotiation. */
	error = advertise_all(device);
	if (error != 0)
		return error;

	/* Succeeded: the PHY is negotiating. */
	return 0;
}

/*
 * Powers the PHY up and waits for it to report it is ready.
 *
 * Procedure referenced: OpenBSD sys/dev/pci/if_rge.c 1.48
 * (rge_set_phy_power).
 */
static int
power_up_phy(
	struct rtl_device *device)
{
	uint16_t state;
	unsigned poll;
	int error;

	/* Turns the PHY's power on and lets it autonegotiate. */
	set8(device, RTL_REG_POWER_CONTROL, RTL_POWER_CONTROL_PHY_ON);
	error = mii_write(device, RTL_MII_CONTROL, RTL_MII_CONTROL_AUTONEG);
	if (error != 0)
		return error;

	/* Waits for the PHY to be ready; a slow one is set up anyway. */
	for (poll = 0; poll < RTL_PHY_STATE_POLLS; poll++) {
		/* A PHY that does not answer ends the wait. */
		error = phy_ocp_read(device, RTL_PHY_OCP_STATE, &state);
		if (error != 0)
			return error;

		/* The PHY is ready once its state says so. */
		if ((state & RTL_PHY_OCP_STATE_MASK) == RTL_PHY_OCP_STATE_READY)
			break;

		/* Gives the PHY a millisecond before asking again. */
		kern_usleep_range(RTL_PHY_STATE_POLL_US, RTL_PHY_STATE_POLL_US);
	}

	/* Succeeded: the PHY is powered. */
	return 0;
}

/*
 * Keeps the link out of CLKREQ power saving and clears a pending PME.
 *
 * Procedure referenced: OpenBSD sys/dev/pci/if_rge.c 1.48
 * (rge_disable_aspm_clkreq).
 */
static void
disable_clock_request(
	struct rtl_device *device)
{
	uint8_t lock;
	unsigned unlocked;

	/* Opens the configuration registers unless they are open already. */
	lock = read8(device, RTL_REG_CONFIG_LOCK);
	unlocked = 0U;
	if ((lock & RTL_CONFIG_LOCK_WRITABLE) != RTL_CONFIG_LOCK_WRITABLE) {
		set8(device, RTL_REG_CONFIG_LOCK, RTL_CONFIG_LOCK_WRITABLE);
		unlocked = 1U;
	}

	/* Clears CLKREQ where the part keeps it. */
	if (device->chip->family == RTL_FAMILY_8126 || device->chip->family == RTL_FAMILY_8127) {
		clear8(device, RTL_REG_INTERRUPT_CONFIG0, RTL_INTERRUPT_CONFIG0_CLKREQ);
	} else {
		clear8(device, RTL_REG_CONFIG2, RTL_CONFIG2_CLKREQ);
	}

	/* Clears a pending PME. */
	clear8(device, RTL_REG_CONFIG5, RTL_CONFIG5_PME_STATUS);

	/* Locks the configuration registers again if this opened them. */
	if (unlocked)
		clear8(device, RTL_REG_CONFIG_LOCK, RTL_CONFIG_LOCK_WRITABLE);
}

/*
 * Stops the MAC's microcode and clears its break points.
 *
 * Procedure referenced: OpenBSD sys/dev/pci/if_rge.c 1.48 (rge_hw_init).
 */
static void
stop_mac_mcu(
	struct rtl_device *device)
{
	uint16_t reg;

	/* Disables the break points, then clears each one. */
	mac_ocp_write(device, RTL_MAC_OCP_MCU_BREAK_ENABLE, 0U);
	for (reg = RTL_MAC_OCP_MCU_BREAK_FIRST; reg < RTL_MAC_OCP_MCU_BREAK_ENABLE; reg = (uint16_t)(reg + 2U))
		mac_ocp_write(device, reg, 0U);

	/* Gives the microcode time to stop, then turns it off. */
	kern_usleep_range(RTL_MCU_STOP_US, RTL_MCU_STOP_US);
	mac_ocp_write(device, RTL_MAC_OCP_MCU_ENABLE, 0U);
}

/*
 * Sets the MAC up for the driver: no CLKREQ, no UPS, the microcode
 * stopped, and uncorrectable PCI Express errors reported.
 *
 * Procedure referenced: OpenBSD sys/dev/pci/if_rge.c 1.48 (rge_hw_init),
 * without the microcode patch.
 */
static void
configure_hardware(
	struct rtl_device *device)
{
	uint32_t status;

	/* Keeps the link out of CLKREQ power saving. */
	disable_clock_request(device);
	clear8(device, RTL_REG_MISC_F1, RTL_MISC_F1_BIT);

	/* Turns the firmware's power saving (UPS) off. */
	mac_ocp_clear(device, RTL_MAC_OCP_UPS_CONTROL, RTL_MAC_OCP_UPS_CONTROL_BIT);

	/* Stops the MAC's microcode, which runs without a patch from here on. */
	disable_clock_request(device);
	stop_mac_mcu(device);

	/* Reports uncorrectable errors in the PCI Express status. */
	status = csi_read(device, RTL_CSI_UNCORRECTABLE);
	csi_write(device, RTL_CSI_UNCORRECTABLE, status | RTL_CSI_UNCORRECTABLE_BIT);
}

/*
 * Writes the part's PCI Express PHY table.
 *
 * The RTL8127's registers are named by a page in an extended address
 * register and an offset; the page goes back to 0 at the end.
 *
 * Procedure referenced: OpenBSD sys/dev/pci/if_rge.c 1.48
 * (rge_ephy_config_mac_r27, rge_r27_write_ephy, rge_check_ephy_ext_add).
 */
static void
configure_ephy(
	struct rtl_device *device)
{
	unsigned index;
	uint16_t reg;

	/* The RTL8125 has its own table and settings. */
	if (device->chip->family == RTL_FAMILY_8125) {
		configure_ephy_8125(device);
		return;
	}

	/* The RTL8125B has its own table. */
	if (device->chip->family == RTL_FAMILY_8125B) {
		/* Writes each entry of the table. */
		for (index = 0; index < sizeof(rtl_ephy_8125b) / sizeof(rtl_ephy_8125b[0]); index++)
			ephy_write(device, rtl_ephy_8125b[index].reg, rtl_ephy_8125b[index].value);
		return;
	}

	/* The RTL8125D and RTL8126 keep their reset values. */
	if (device->chip->family != RTL_FAMILY_8127)
		return;

	/* Writes each entry through its page. */
	for (index = 0; index < sizeof(rtl_ephy_8127) / sizeof(rtl_ephy_8127[0]); index++) {
		reg = rtl_ephy_8127[index].reg;
		ephy_write(device, RTL_EPHY_EXTENDED_ADDRESS, (uint16_t)(reg >> 12));
		ephy_write(device, (uint16_t)(reg & 0x0fffU), rtl_ephy_8127[index].value);
	}

	/* Returns to the first page. */
	ephy_write(device, RTL_EPHY_EXTENDED_ADDRESS, 0U);
}

/*
 * Clears what the PHY advertises and resets it.
 *
 * Procedure referenced: OpenBSD sys/dev/pci/if_rge.c 1.48 (rge_phy_config,
 * the PHY reset).
 */
static int
reset_phy(
	struct rtl_device *device)
{
	uint16_t advertise;
	uint16_t multigig;
	uint16_t control;
	uint64_t deadline;
	unsigned poll;
	int error;

	/* Advertises no 10 or 100 Mbit/s mode. */
	error = mii_read(device, RTL_MII_ADVERTISE, &advertise);
	if (error != 0)
		return error;
	advertise &= (uint16_t)~(RTL_MII_ADVERTISE_100_FULL | RTL_MII_ADVERTISE_100 |
				  RTL_MII_ADVERTISE_10_FULL | RTL_MII_ADVERTISE_10);
	error = mii_write(device, RTL_MII_ADVERTISE, advertise);
	if (error != 0)
		return error;

	/* Advertises no gigabit mode. */
	error = mii_read(device, RTL_MII_1000_CONTROL, &control);
	if (error != 0)
		return error;
	control &= (uint16_t)~(RTL_MII_1000_FULL | RTL_MII_1000_HALF);
	error = mii_write(device, RTL_MII_1000_CONTROL, control);
	if (error != 0)
		return error;

	/* Advertises none of the multi-gigabit modes the part has. */
	multigig = RTL_PHY_OCP_MULTIGIG_2500;
	if (device->chip->family == RTL_FAMILY_8126)
		multigig |= RTL_PHY_OCP_MULTIGIG_5000;
	if (device->chip->family == RTL_FAMILY_8127)
		multigig |= RTL_PHY_OCP_MULTIGIG_5000 | RTL_PHY_OCP_MULTIGIG_10000;
	error = phy_ocp_modify(device, RTL_PHY_OCP_MULTIGIG, multigig, 0U);
	if (error != 0)
		return error;

	/* Resets the PHY. */
	error = mii_write(device,
			  RTL_MII_CONTROL,
			  RTL_MII_CONTROL_RESET | RTL_MII_CONTROL_AUTONEG | RTL_MII_CONTROL_RESTART);
	if (error != 0)
		return error;

	/* Waits for the reset to finish. */
	for (poll = 0; poll < RTL_PHY_RESET_POLLS; poll++) {
		/* A PHY that does not answer ends the wait. */
		error = mii_read(device, RTL_MII_CONTROL, &control);
		if (error != 0)
			return error;

		/* The PHY is out of reset once the bit clears. */
		if ((control & RTL_MII_CONTROL_RESET) == 0)
			return 0;

		/* Sleeps a millisecond before asking again. */
		deadline = sched_ticks() + kern_ms_to_ticks(RTL_PHY_RESET_POLL_MS);
		sched_sleep(deadline);
	}

	/* A PHY that stays in reset is reported. */
	return ETIMEDOUT;
}

/*
 * Records the PHY's microcode version and applies the part's PHY settings.
 *
 * Procedure referenced: OpenBSD sys/dev/pci/if_rge.c 1.48 (rge_phy_config).
 */
static int
configure_phy(
	struct rtl_device *device)
{
	const struct rtl_phy_step *steps;
	unsigned count;
	int error;

	/* Reads the version of the microcode the PHY runs. */
	error = phy_ocp_write(device, RTL_PHY_OCP_INDEX, RTL_PHY_OCP_RAM_CODE_VERSION);
	if (error != 0)
		return error;
	error = phy_ocp_read(device, RTL_PHY_OCP_DATA, &device->ram_code_version);
	if (error != 0)
		return error;

	/* Applies the part's settings. */
	error = phy_steps_of(device, &steps, &count);
	if (error != 0)
		return error;
	error = run_phy_steps(device, steps, count);
	if (error != 0)
		return error;

	/* Clears the bit every part's settings end with. */
	error = phy_ocp_modify(device, 0xa5b4U, 0x8000U, 0U);
	if (error != 0)
		return error;

	/* Succeeded: the PHY is set up. */
	return 0;
}

/*
 * Turns energy-efficient Ethernet and its advanced mode off in the MAC and
 * the PHY.
 *
 * Procedure and values referenced: OpenBSD sys/dev/pci/if_rge.c 1.48
 * (rge_phy_config, "Disable EEE" and "Disable advanced EEE").
 */
static int
disable_eee(
	struct rtl_device *device)
{
	static const struct rtl_phy_step common[] = {
		{ RTL_PHY_STEP_MODIFY, 0xa6d8, 0x0010, 0x0000 },
		{ RTL_PHY_STEP_MODIFY, 0xa428, 0x0080, 0x0000 },
		{ RTL_PHY_STEP_MODIFY, 0xa4a2, 0x0200, 0x0000 },
	};
	static const struct rtl_phy_step advanced[] = {
		{ RTL_PHY_STEP_MODIFY, 0xa442, 0x3000, 0x0000 },
		{ RTL_PHY_STEP_MODIFY, 0xa430, 0x8000, 0x0000 },
	};
	uint16_t bits;
	int error;

	/* Turns EEE off in the MAC. */
	mac_ocp_clear(device, RTL_MAC_OCP_EEE, RTL_MAC_OCP_EEE_BITS);

	/* Turns the PHY's EEE advertisement off; the RTL8127 has one more speed of it. */
	bits = 0x0006U;
	if (device->chip->family == RTL_FAMILY_8127)
		bits = 0x000eU;
	error = phy_ocp_modify(device, 0xa5d0U, bits, 0U);
	if (error != 0)
		return error;

	/* Turns the PHY's multi-gigabit EEE off; the RTL8126 and RTL8127 have two bits of it. */
	bits = 0x0001U;
	if (device->chip->family == RTL_FAMILY_8126 || device->chip->family == RTL_FAMILY_8127)
		bits = 0x0003U;
	error = phy_ocp_modify(device, 0xa6d4U, bits, 0U);
	if (error != 0)
		return error;

	/* Turns the rest of the PHY's EEE off. */
	error = run_phy_steps(device, common, sizeof(common) / sizeof(common[0]));
	if (error != 0)
		return error;

	/* Turns advanced EEE off in the MAC and the PHY. */
	mac_ocp_clear(device, RTL_MAC_OCP_EEE_ADVANCED, RTL_MAC_OCP_EEE_ADVANCED_BIT);
	error = run_phy_steps(device, advanced, sizeof(advanced) / sizeof(advanced[0]));
	if (error != 0)
		return error;

	/* Succeeded: EEE is off. */
	return 0;
}

/*
 * Advertises every speed the part has, with pause, and restarts the
 * negotiation.
 *
 * Procedure and values referenced: OpenBSD sys/dev/pci/if_rge.c 1.48
 * (rge_ifmedia_upd, the automatic media).
 */
static int
advertise_all(
	struct rtl_device *device)
{
	uint16_t multigig;
	uint16_t lite;
	uint16_t value;
	int error;

	/* Turns Gigabit Lite off. */
	error = phy_ocp_modify(device, 0xa428U, 0x0200U, 0U);
	if (error != 0)
		return error;
	lite = 0x0001U;
	if (device->chip->family == RTL_FAMILY_8126 || device->chip->family == RTL_FAMILY_8127)
		lite = 0x0007U;
	error = phy_ocp_modify(device, 0xa5eaU, lite, 0U);
	if (error != 0)
		return error;

	/* Chooses the multi-gigabit speeds the part has. */
	multigig = RTL_PHY_OCP_MULTIGIG_2500;
	if (device->chip->family == RTL_FAMILY_8126)
		multigig |= RTL_PHY_OCP_MULTIGIG_5000;
	if (device->chip->family == RTL_FAMILY_8127)
		multigig |= RTL_PHY_OCP_MULTIGIG_5000 | RTL_PHY_OCP_MULTIGIG_10000;

	/* Advertises 10 and 100 Mbit/s at both duplexes, with pause. */
	error = mii_write(device,
			  RTL_MII_ADVERTISE,
			  RTL_MII_ADVERTISE_100_FULL | RTL_MII_ADVERTISE_100 |
				  RTL_MII_ADVERTISE_10_FULL | RTL_MII_ADVERTISE_10 |
				  RTL_MII_ADVERTISE_PAUSE_ASYM | RTL_MII_ADVERTISE_PAUSE);
	if (error != 0)
		return error;

	/* Advertises gigabit at both duplexes. */
	error = mii_write(device, RTL_MII_1000_CONTROL, RTL_MII_1000_FULL | RTL_MII_1000_HALF);
	if (error != 0)
		return error;

	/* Advertises the multi-gigabit speeds. */
	error = phy_ocp_modify(device, RTL_PHY_OCP_MULTIGIG, multigig, multigig);
	if (error != 0)
		return error;

	/* Restarts the negotiation. */
	value = RTL_MII_CONTROL_RESET | RTL_MII_CONTROL_AUTONEG | RTL_MII_CONTROL_RESTART;
	error = mii_write(device, RTL_MII_CONTROL, value);
	if (error != 0)
		return error;

	/* Succeeded: the PHY is negotiating. */
	return 0;
}

/* Starts the thread that follows the link while the interface is open. */
static void
start_link_thread(
	struct rtl_device *device)
{
	int error;

	/* Creates the thread; without it the carrier never comes up. */
	error = kthread_create(link_worker, device, SCHED_PRIORITY_DEFAULT, &device->link_thread);
	if (error != 0) {
		kern_logf("rtl8125: %s link thread unavailable (%d)\n", device->name, error);
		return;
	}

	/* Keeps the interface for the thread, which lives as long as the system. */
	net_device_ref(device->net);

	/* Lets it run. */
	thread_start(device->link_thread);
}

/*
 * Follows the link every RTL_LINK_MS and, while the interface is open,
 * wakes the network worker every RTL_POLL_MS for anything a lost
 * interrupt left on the rings, for the life of the system.
 */
static void
link_worker(
	void *argument)
{
	struct rtl_device *device;
	unsigned long irq;
	uint64_t next_link;
	uint64_t now;
	uint64_t deadline;
	unsigned opened;

	/* Checks the link at once, then on its interval. */
	device = argument;
	next_link = 0U;
	for (;;) {
		/* Follows the link when its interval has passed. */
		now = sched_ticks();
		if (now >= next_link) {
			check_link(device);
			next_link = now + kern_ms_to_ticks(RTL_LINK_MS);
		}

		/* Samples whether the interface is open. */
		irq = spin_lock_irqsave(&device->lock);

		opened = device->opened;

		spin_unlock_irqrestore(&device->lock, irq);

		/* Wakes the worker for the rings of an open interface. */
		if (opened)
			net_device_schedule_poll(device->net);

		/* Reports the profile when its second has passed. */
		profile_report(device);

		/* Sleeps until the next service. */
		deadline = sched_ticks() + kern_ms_to_ticks(RTL_POLL_MS);
		sched_sleep(deadline);
	}
}

/*
 * Reads the link from the PHY status and announces a change.
 *
 * Procedure referenced: OpenBSD sys/dev/pci/if_rge.c 1.48
 * (rge_get_link_status, rge_link_state).
 */
static void
check_link(
	struct rtl_device *device)
{
	unsigned long irq;
	uint16_t status;
	uint32_t mbps;
	unsigned opened;
	unsigned carrier;
	unsigned changed;
	const char *state;
	const char *duplex;

	/* Samples the PHY status while the interface is open. */
	irq = spin_lock_irqsave(&device->lock);

	opened = device->opened;
	status = 0U;
	if (opened)
		status = read16(device, RTL_REG_PHY_STATUS);

	spin_unlock_irqrestore(&device->lock, irq);

	/* A closed interface has no carrier. */
	carrier = 0U;
	mbps = 0U;
	if (opened && (status & RTL_PHY_STATUS_LINK) != 0) {
		carrier = 1U;
		mbps = speed_of(status);
	}

	/* Records the link and whether it moved. */
	irq = spin_lock_irqsave(&device->lock);

	changed = 0U;
	if (carrier != device->carrier || mbps != device->link_mbps) {
		device->carrier = carrier;
		device->link_mbps = mbps;
		changed = 1U;
	}

	spin_unlock_irqrestore(&device->lock, irq);

	/* Announces a change. */
	if (changed) {
		device->net->link_mbps = mbps;
		(void)net_device_set_carrier(device->net, (int)carrier);

		/* Words the carrier and the duplex for the log. */
		state = "down";
		if (carrier)
			state = "up";
		duplex = "";
		if ((status & RTL_PHY_STATUS_FULL_DUPLEX) != 0)
			duplex = " full duplex";

		/* Tells the new link state. */
		kern_logf("rtl8125: %s link %s %u Mbit/s%s\n", device->name, state, (unsigned)mbps, duplex);
	}
}

/* Reports the speed a PHY status names, in Mbit/s. */
static uint32_t
speed_of(
	uint16_t status)
{
	/* Takes the fastest speed bit that is set. */
	if ((status & RTL_PHY_STATUS_10000) != 0)
		return 10000U;
	if ((status & RTL_PHY_STATUS_5000) != 0)
		return 5000U;
	if ((status & RTL_PHY_STATUS_2500) != 0)
		return 2500U;
	if ((status & RTL_PHY_STATUS_1000) != 0)
		return 1000U;
	if ((status & RTL_PHY_STATUS_100) != 0)
		return 100U;
	if ((status & RTL_PHY_STATUS_10) != 0)
		return 10U;

	/* No speed bit is set. */
	return 0U;
}

/*
 * Measures the fine counter's rate and what one read of it costs, for the
 * profile; without the profile the counter is not read.
 */
static void
profile_calibrate(
	struct rtl_device *device)
{
	uint64_t first;
	uint64_t last;
	uint64_t hz;
	unsigned reads;
	bool available;
	int error;

	/* Leaves the profile off unless the kernel keeps it. */
	if (!CONFIG_DRIVER_PCI_RTL8125_PROFILE)
		return;

	/* Takes the counter's rate; without a counter the profile stays off. */
	available = kern_rtc_read_counter(&first, &hz);
	if (!available || hz == 0U)
		return;

	/* Times a run of reads to know what one costs. */
	for (reads = 0; reads < 64U; reads++)
		(void)kern_rtc_read_counter(&last, &hz);

	/* Takes a block for the tally counters; without one the profile goes without the missed frames. */
	error = drv_dma_alloc_coherent(device->dma, RTL_TALLY_BYTES, RTL_TALLY_ALIGNMENT, &device->profile.tally);
	if (error == 0)
		device->profile.tally_allocated = 1U;

	/* Keeps the rate and the cost of one read. */
	device->counter_cost = (last - first) / 64U;
	device->counter_hz = hz;
	kern_logf("rtl8125: %s profile on, counter %llu Hz, %llu ns a read\n",
		  device->name,
		  (unsigned long long)hz,
		  (unsigned long long)(device->counter_cost * 1000000000ULL / hz));
}

/* Reads the fine counter for the profile, or 0 in a kernel without it. */
static uint64_t
profile_now(
	void)
{
	uint64_t counter;
	uint64_t hz;
	bool available;

	/* Reads nothing unless the kernel keeps the profile. */
	if (!CONFIG_DRIVER_PCI_RTL8125_PROFILE)
		return 0U;

	/* Reads the counter; one that is unavailable counts as 0. */
	available = kern_rtc_read_counter(&counter, &hz);
	if (!available)
		return 0U;

	/* Succeeded: the counter's value. */
	return counter;
}

/*
 * Counts one interrupt of the controller and the time since it began.  The
 * caller holds the lock.
 */
static void
profile_interrupt(
	struct rtl_device *device,
	uint64_t started)
{
	uint64_t now;

	/* Counts nothing without a calibrated profile. */
	if (device->counter_hz == 0U)
		return;

	/* Adds the interrupt and its time. */
	now = profile_now();
	device->profile.interrupts++;
	device->profile.interrupt_time += now - started;
}

/*
 * Counts one poll, the frames it took off the receive ring, whether it used
 * the whole budget, and the time since it began.  The caller holds the
 * lock.
 */
static void
profile_poll(
	struct rtl_device *device,
	uint64_t started,
	unsigned count,
	unsigned budget)
{
	uint64_t now;

	/* Counts nothing without a calibrated profile. */
	if (device->counter_hz == 0U)
		return;

	/* Adds the poll, its frames and its time. */
	now = profile_now();
	device->profile.polls++;
	device->profile.poll_frames += count;
	if (count >= budget)
		device->profile.full_polls++;
	device->profile.poll_time += now - started;
}

/*
 * Counts one transmit, whether the frame was copied or refused, and the
 * time since it began.  The caller holds the lock.
 */
static void
profile_transmit(
	struct rtl_device *device,
	uint64_t started,
	int in_place,
	int error)
{
	uint64_t now;

	/* Counts nothing without a calibrated profile. */
	if (device->counter_hz == 0U)
		return;

	/* Adds the transmit, how it went and its time. */
	now = profile_now();
	device->profile.transmits++;
	if (error != 0) {
		device->profile.transmit_refusals++;
	} else if (!in_place) {
		device->profile.transmit_copies++;
	}

	/* Adds the time the transmit took. */
	device->profile.transmit_time += now - started;
}

/*
 * Logs the profile's totals once a second while frames move, the times in
 * microseconds with the counter's own reads still in them.
 */
static void
profile_report(
	struct rtl_device *device)
{
	struct rtl_profile profile;
	unsigned long irq;
	uint64_t now;
	uint64_t frames;
	uint64_t uptime_ms;
	uint64_t interrupt_us;
	uint64_t poll_us;
	uint64_t transmit_us;
	uint16_t missed;
	unsigned report;
	int error;

	/* Reports nothing without a calibrated profile. */
	if (device->counter_hz == 0U)
		return;

	/* Takes a copy of the totals when the second has passed and frames moved since the last report. */
	now = sched_ticks();
	report = 0U;
	irq = spin_lock_irqsave(&device->lock);

	frames = device->profile.poll_frames + device->profile.transmits;
	if (now >= device->profile.next_report && frames != device->profile.reported_frames) {
		device->profile.reported_frames = frames;
		device->profile.next_report = now + kern_ms_to_ticks(1000U);
		profile = device->profile;
		report = 1U;
	}

	spin_unlock_irqrestore(&device->lock, irq);

	/* Nothing to log until the second has passed and frames moved. */
	if (!report)
		return;

	/* Reads the frames the controller missed; a failed read reports none. */
	missed = 0U;
	error = profile_missed(device, &missed);
	if (error != 0)
		missed = 0U;

	/* Converts the counter's spans into the microseconds the log shows. */
	uptime_ms = profile_microseconds(device, profile_now()) / 1000U;
	interrupt_us = profile_microseconds(device, profile.interrupt_time);
	poll_us = profile_microseconds(device, profile.poll_time);
	transmit_us = profile_microseconds(device, profile.transmit_time);

	/* Logs the totals outside the lock. */
	kern_logf("rtl8125: %s profile ms %llu irq %llu %llu poll %llu %llu %llu %llu tx %llu %llu %llu %llu missed %u\n",
		  device->name,
		  (unsigned long long)uptime_ms,
		  (unsigned long long)profile.interrupts,
		  (unsigned long long)interrupt_us,
		  (unsigned long long)profile.polls,
		  (unsigned long long)profile.full_polls,
		  (unsigned long long)profile.poll_frames,
		  (unsigned long long)poll_us,
		  (unsigned long long)profile.transmits,
		  (unsigned long long)profile.transmit_copies,
		  (unsigned long long)profile.transmit_refusals,
		  (unsigned long long)transmit_us,
		  (unsigned)missed);
}

/*
 * Has the controller write its tally counters and reads the frames it
 * missed for want of a receive descriptor, since its reset.
 *
 * Procedure referenced: FreeBSD sys/dev/rge/if_rge_stats.c, main
 * 8fa55fdcae4ecbd2ec13ef91cc9d172095e21189 (rge_hw_mac_stats_fetch).
 */
static int
profile_missed(
	struct rtl_device *device,
	uint16_t *missed)
{
	const volatile uint8_t *block;
	unsigned long irq;
	uint32_t command;
	unsigned polls;

	/* Without a block there is nothing to read. */
	if (!device->profile.tally_allocated)
		return ENODEV;

	/* Gives the block's address and starts the write. */
	irq = spin_lock_irqsave(&device->lock);

	write32(device, RTL_REG_TALLY_HIGH, (uint32_t)(device->profile.tally.device_address >> 32));
	(void)read8(device, RTL_REG_COMMAND);
	write32(device, RTL_REG_TALLY_LOW, (uint32_t)device->profile.tally.device_address);
	write32(device, RTL_REG_TALLY_LOW, (uint32_t)device->profile.tally.device_address | RTL_TALLY_DUMP);

	spin_unlock_irqrestore(&device->lock, irq);

	/* Waits for the controller to clear the bit, which ends the write. */
	for (polls = 0; polls < RTL_TALLY_POLLS; polls++) {
		command = read32(device, RTL_REG_TALLY_LOW);

		/* The write has ended once the controller clears the bit. */
		if ((command & RTL_TALLY_DUMP) == 0)
			break;

		/* Gives the controller time before reading again. */
		kern_usleep_range(RTL_TALLY_POLL_US, RTL_TALLY_POLL_US);
	}

	/* A write that did not end leaves the block unread. */
	if ((command & RTL_TALLY_DUMP) != 0)
		return ETIMEDOUT;

	/* Reads the count after the write that ended. */
	kern_io_read_barrier();
	block = device->profile.tally.address;
	*missed = (uint16_t)(block[RTL_TALLY_MISSED] | (block[RTL_TALLY_MISSED + 1U] << 8));

	/* Succeeded: the count of missed frames. */
	return 0;
}

/* Converts a span of the fine counter into microseconds. */
static uint64_t
profile_microseconds(
	const struct rtl_device *device,
	uint64_t time)
{
	uint64_t whole;
	uint64_t part;

	/* Splits the span into whole seconds and the rest, so the product cannot overflow. */
	whole = time / device->counter_hz;
	part = time % device->counter_hz;

	/* Succeeded: the span in microseconds. */
	return whole * 1000000U + part * 1000000U / device->counter_hz;
}

/*
 * Takes the PHY into its patch mode or out of it and waits for its state to
 * follow.  A state that does not follow is logged and the sequence goes on,
 * as the reference does.
 *
 * Procedure referenced: FreeBSD sys/dev/rge/if_rge_hw.c, main
 * 8fa55fdcae4ecbd2ec13ef91cc9d172095e21189 (rge_patch_phy_mcu).
 */
static int
set_phy_patch_mode(
	struct rtl_device *device,
	int enter)
{
	const char *direction;
	uint16_t state;
	uint16_t wanted;
	uint16_t clear_bits;
	uint16_t set_bits;
	unsigned polls;
	int error;

	/* Chooses whether the request bit is set or cleared. */
	clear_bits = RTL_PHY_OCP_PATCH_REQUEST_BIT;
	set_bits = 0U;
	if (enter) {
		clear_bits = 0U;
		set_bits = RTL_PHY_OCP_PATCH_REQUEST_BIT;
	}

	/* Sets or clears the request. */
	error = phy_ocp_modify(device, RTL_PHY_OCP_PATCH_REQUEST, clear_bits, set_bits);
	if (error != 0)
		return error;

	/* Waits for the state bit to match the request. */
	wanted = 0U;
	if (enter)
		wanted = RTL_PHY_OCP_PATCH_STATE_BIT;
	state = (uint16_t)~wanted;
	for (polls = 0; polls < RTL_PATCH_POLLS; polls++) {
		error = phy_ocp_read(device, RTL_PHY_OCP_PATCH_STATE, &state);
		if (error != 0)
			return error;

		/* The state has followed once its bit matches the request. */
		if ((state & RTL_PHY_OCP_PATCH_STATE_BIT) == wanted)
			break;

		/* Gives the PHY time before asking again. */
		kern_usleep_range(RTL_PATCH_POLL_US, RTL_PATCH_POLL_US);
	}

	/* Names the change that did not follow, for the log. */
	direction = "end";
	if (enter)
		direction = "start";

	/* Logs a state that did not follow; the settings go on. */
	if ((state & RTL_PHY_OCP_PATCH_STATE_BIT) != wanted)
		kern_logf("rtl8125: %s PHY patch mode did not %s\n", device->name, direction);

	/* Succeeded: the request is made. */
	return 0;
}

/*
 * Reads one PCI Express PHY register.
 *
 * Procedure referenced: FreeBSD sys/dev/rge/if_rge_hw.c, main
 * 8fa55fdcae4ecbd2ec13ef91cc9d172095e21189 (rge_read_ephy).
 */
static uint16_t
ephy_read(
	struct rtl_device *device,
	uint16_t reg)
{
	uint32_t window;
	unsigned poll;

	/* Names the register without the flag, which the controller sets once the data is in. */
	window = (uint32_t)(reg & RTL_EPHY_ADDRESS_MASK) << RTL_EPHY_ADDRESS_SHIFT;
	write32(device, RTL_REG_EPHY, window);

	/* Waits for the read to finish; the window settles either way. */
	for (poll = 0; poll < RTL_OCP_POLLS; poll++) {
		kern_usleep_range(RTL_OCP_POLL_US, RTL_OCP_POLL_US);

		/* The data is in once the flag is set. */
		window = read32(device, RTL_REG_EPHY);
		if ((window & RTL_EPHY_BUSY) != 0)
			break;
	}

	/* Gives the window time to settle. */
	kern_usleep_range(RTL_CSI_SETTLE_US, RTL_CSI_SETTLE_US);

	/* Succeeded: the register's value, or what the window held when the wait ran out. */
	return (uint16_t)(window & RTL_EPHY_DATA_MASK);
}

/* Clears the mask bits and sets the value bits of one PCI Express PHY register. */
static void
ephy_modify(
	struct rtl_device *device,
	uint16_t reg,
	uint16_t mask,
	uint16_t value)
{
	uint16_t current;

	/* Writes the register back with the bits changed. */
	current = ephy_read(device, reg);
	ephy_write(device, reg, (uint16_t)((current & ~mask) | value));
}

/*
 * Writes the RTL8125's PCI Express PHY table and the settings that follow
 * it on both lanes.
 *
 * Procedure and values referenced: FreeBSD sys/dev/rge/if_rge_hw.c, main
 * 8fa55fdcae4ecbd2ec13ef91cc9d172095e21189 (rge_ephy_config_mac_r25).
 */
static void
configure_ephy_8125(
	struct rtl_device *device)
{
	unsigned index;

	/* Writes the table. */
	for (index = 0; index < sizeof(rtl_ephy_8125) / sizeof(rtl_ephy_8125[0]); index++)
		ephy_write(device, rtl_ephy_8125[index].reg, rtl_ephy_8125[index].value);

	/* Sets the first lane. */
	ephy_modify(device, 0x002aU, 0x7000U, 0x3000U);
	ephy_modify(device, 0x0019U, 0x0040U, 0U);
	ephy_modify(device, 0x001bU, 0U, 0x0e00U);
	ephy_modify(device, 0x001bU, 0x7000U, 0U);
	ephy_write(device, 0x0002U, 0x6042U);
	ephy_write(device, 0x0006U, 0x0014U);

	/* Sets the second lane the same way. */
	ephy_modify(device, 0x006aU, 0x7000U, 0x3000U);
	ephy_modify(device, 0x0059U, 0x0040U, 0U);
	ephy_modify(device, 0x005bU, 0U, 0x0e00U);
	ephy_modify(device, 0x005bU, 0x7000U, 0U);
	ephy_write(device, 0x0042U, 0x6042U);
	ephy_write(device, 0x0046U, 0x0014U);
}

/*
 * Picks the PHY settings of the controller's part and revision.  Every
 * part the table of versions names has its settings.
 */
static int
phy_steps_of(
	const struct rtl_device *device,
	const struct rtl_phy_step **steps,
	unsigned *count)
{
	unsigned second;

	/* The two revisions of a family differ in their settings. */
	second = 0U;
	if (device->chip->revision == 2U)
		second = 1U;

	/* Takes the table of the family and revision. */
	switch (device->chip->family) {
	case RTL_FAMILY_8125:
		*steps = rtl_phy_8125;
		*count = sizeof(rtl_phy_8125) / sizeof(rtl_phy_8125[0]);
		break;
	case RTL_FAMILY_8125B:
		*steps = rtl_phy_8125b;
		*count = sizeof(rtl_phy_8125b) / sizeof(rtl_phy_8125b[0]);
		break;
	case RTL_FAMILY_8125D:
		/* The second revision has its own settings. */
		if (second) {
			*steps = rtl_phy_8125d_2;
			*count = sizeof(rtl_phy_8125d_2) / sizeof(rtl_phy_8125d_2[0]);
		} else {
			*steps = rtl_phy_8125d_1;
			*count = sizeof(rtl_phy_8125d_1) / sizeof(rtl_phy_8125d_1[0]);
		}

		break;
	case RTL_FAMILY_8126:
		/* The second revision has its own settings. */
		if (second) {
			*steps = rtl_phy_8126_2;
			*count = sizeof(rtl_phy_8126_2) / sizeof(rtl_phy_8126_2[0]);
		} else {
			*steps = rtl_phy_8126_1;
			*count = sizeof(rtl_phy_8126_1) / sizeof(rtl_phy_8126_1[0]);
		}

		break;
	case RTL_FAMILY_8127:
		*steps = rtl_phy_8127;
		*count = sizeof(rtl_phy_8127) / sizeof(rtl_phy_8127[0]);
		break;
	default:
		return ENODEV;
	}

	/* Succeeded: the part's settings. */
	return 0;
}
