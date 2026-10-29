/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Realtek USB 2.5G and 5G Ethernet (RTL8156, RTL8157) in vendor mode
 *
 * These adapters also offer CDC NCM and ECM configurations; their own
 * configuration packs many frames into one bulk transfer each way and
 * computes the checksums, which the generic classes do not.  The driver
 * claims the vendor interface with a higher match score than CDC NCM, so
 * the USB core sets the device to that configuration.  The boot parameter
 * usb.rtl815x=off leaves the adapter to CDC NCM.
 */

#include <drivers/usb/usb-rtl815x.h>
#include <drivers/usb/usb.h>
#include <kern/boot.h>
#include <kern/clock.h>
#include <kern/kcrt.h>
#include <kern/klog.h>
#include <kern/kmem.h>
#include <kern/lock.h>
#include <kern/net/net-device.h>
#include <kern/net/packet-buf.h>
#include <kern/platform.h>
#include <kern/random.h>
#include <kern/sched.h>
#include <kern/thread.h>
#include <uapi/errno.h>

/* Marks a parameter a function takes for its signature's sake. */
#ifndef UNUSED_PARAMETER
#define UNUSED_PARAMETER(parameter) ((void)(parameter))
#endif

/*
 * The boot parameter that leaves the adapters to CDC NCM, should the
 * vendor mode misbehave on one of them.
 */
#define RTL_OFF_TOKEN			"usb.rtl815x=off"

/* Realtek's USB vendor ID, and the vendor interface's class. */
#define RTL_VENDOR_REALTEK		0x0bdaU
#define RTL_VENDOR_CLASS		0xffU

/*
 * The match score of the vendor interface: above CDC NCM's 100, so the
 * USB core picks the vendor configuration.
 */
#define RTL_MATCH_SCORE			120

/*
 * The endpoint numbers of the vendor interface: receive, transmit and
 * the link interrupt.
 *
 * Values referenced: Linux 7.3 drivers/net/usb/r8152.c
 * (rtl_check_vendor_ok).
 */
#define RTL_ENDPOINT_RX			1U
#define RTL_ENDPOINT_TX			2U
#define RTL_ENDPOINT_INTERRUPT		3U

/*
 * The transfers kept on the bus each way and their blocks: a receive
 * block of 48 KB on the RTL8156 and 32 KB on the RTL8157, and a 16 KB
 * transmit block, which a jumbo frame fits.
 *
 * Values referenced: FreeBSD 15.0 sys/dev/usb/net/if_urereg.h
 * (URE_8156_RX_BUFSZ, URE_TX_BUFSZ); Linux 7.3 drivers/net/usb/r8152.c
 * (rtl_ops_init: rx_buf_sz, agg_buf_sz).
 */
#define RTL_RX_URBS			4U
#define RTL_TX_URBS			4U
#define RTL_RX_BLOCK_8156		(48U * 1024U)
#define RTL_RX_BLOCK_8157		(32U * 1024U)
#define RTL_TX_BLOCK			(16U * 1024U)

/*
 * Frames waiting for a transmit transfer: a 64-bit machine's larger pool
 * holds several blocks' worth, the 32-bit boards' small pool a burst.
 * Beyond that, room is kept for small frames (a pure acknowledgement, an
 * ARP or neighbour message): a connection that sends at the link's rate
 * keeps the queue full, and the acknowledgements of one that receives at
 * the same time would otherwise be refused and wait for the stack's retry.
 */
#if PACKET_BUF_POOL_COUNT >= 256U
#define RTL_TX_QUEUE_MAX		128U
#define RTL_TX_QUEUE_SMALL_EXTRA	32U
#else
#define RTL_TX_QUEUE_MAX		16U
#define RTL_TX_QUEUE_SMALL_EXTRA	4U
#endif
#define RTL_TX_SMALL_FRAME		128U

/*
 * The headers a pure TCP acknowledgement is told by: an Ethernet frame of
 * IPv4 or IPv6 (no extension header) carrying TCP with no payload and
 * none of SYN, FIN or RST.
 */
#define RTL_ETHER_HEADER		14U
#define RTL_ETHERTYPE_IPV4		0x0800U
#define RTL_ETHERTYPE_IPV6		0x86ddU
#define RTL_IPV6_HEADER			40U
#define RTL_IP_PROTOCOL_TCP		6U
#define RTL_TCP_HEADER_MIN		20U
#define RTL_TCP_FLAGS_OPEN_CLOSE	0x07U
#define RTL_TX_BATCH_MAX		64U

#define RTL_TRANSFER_TIMEOUT_MS		5000U
#define RTL_CONTROL_TIMEOUT_MS		1000U
#define RTL_REARM_RETRY_MAX		3U
#define RTL_COMPLETION_KINDS		2U
#define RTL_COMPLETION_TX		0U
#define RTL_COMPLETION_RX		1U

/* The link interrupt's bit for a link. */
#define RTL_INTERRUPT_LINK		0x0004U

/* How often the worker reads the link of an open interface without one. */
#define RTL_LINK_POLL_MS			1000U

/* The work the worker thread is asked for. */
#define RTL_WORK_INTERRUPT		0x1U
#define RTL_WORK_LINK			0x2U

/*
 * The frame overhead the device counts in its maximum receive size: the
 * Ethernet header with a VLAN tag, and the FCS.
 */
#define RTL_FRAME_OVERHEAD		22U
#define RTL_MTU_DEFAULT			1500U

/*
 * PLA registers.
 *
 * Values referenced: FreeBSD 15.0 sys/dev/usb/net/if_urereg.h
 * (URE_PLA_*); Linux 7.3 drivers/net/usb/r8152.c (PLA_RCR1,
 * PLA_RXFIFO_FULL, PLA_RX_FIFO_FULL, PLA_RX_FIFO_EMPTY, PLA_TXFIFO_FULL).
 */
#define REG_MAC_ADDRESS			0xc000U
#define REG_RX_CONFIG			0xc010U
#define REG_RX_CONFIG_VLAN		0xc012U
#define REG_RX_FRAME_MAX		0xc016U
#define REG_RX_FIFO_THRESHOLD		0xc0a2U
#define REG_PAUSE_ON			0xc0a6U
#define REG_PAUSE_OFF			0xc0aaU
#define REG_FILTER_CONTROL		0xc0b4U
#define REG_WAKE_OFFLOAD		0xc0bcU
#define REG_MULTICAST_LOW		0xcd00U
#define REG_MULTICAST_HIGH		0xcd04U
#define REG_MAC_STORED			0xd000U
#define REG_WAKE_OFFLOAD_TIMER		0xd2ccU
#define REG_WAKE_TIMER			0xd2e8U
#define REG_SUSPEND_FLAGS		0xd38aU
#define REG_RUNTIME_FLAGS		0xd38cU
#define REG_LINK_FLAGS			0xd398U
#define REG_BOOT_STATUS			0xe004U
#define REG_MAC_POWER			0xe0c0U
#define REG_MAC_POWER2			0xe0caU
#define REG_MAC_POWER3			0xe0ccU
#define REG_MAC_POWER4			0xe0ceU
#define REG_WAKE_WATCHDOG		0xe428U
#define REG_VERSION			0xe612U
#define REG_TX_FRAME_MAX		0xe615U
#define REG_TX_FIFO_THRESHOLD		0xe618U
#define REG_TX_FIFO_FULL		0xe61aU
#define REG_COUNTER_RESET		0xe800U
#define REG_COMMAND			0xe813U
#define REG_CONFIG_WRITE		0xe81cU
#define REG_WAKE_CONFIG			0xe820U
#define REG_OOB_CONTROL			0xe84fU
#define REG_PLUS_COMMAND		0xe854U
#define REG_RX_DMA_GATE			0xe85aU
#define REG_PHY_WINDOW			0xe86cU
#define REG_MCU_STATUS			0xe8deU
#define REG_PHY_STATUS			0xe908U
#define REG_FLASH_STATUS		0xd3aeU
#define REG_FLOW_OPTION			0xdc6bU

/*
 * USB registers.
 *
 * Values referenced: FreeBSD 15.0 sys/dev/usb/net/if_urereg.h
 * (URE_USB_*) and if_ure.c (ure_rtl8153b_init: 0xd26b, 0xd32a, 0xcfee,
 * 0xcfff, 0xd284, 0xd4b4).
 */
#define UREG_U2P3			0xb460U
#define UREG_STORAGE_TIMER		0xcbfcU
#define UREG_LINK_POWER			0xcfd8U
#define UREG_CFEE			0xcfeeU
#define UREG_UPS_FLAGS			0xcfffU
#define UREG_D26B			0xd26bU
#define UREG_FLASH_LOAD			0xd284U
#define UREG_SPEED_OPTIONS		0xd32aU
#define UREG_FIRMWARE_CONTROL		0xd334U
#define UREG_FLOW_TIMER			0xd340U
#define UREG_TRANSFER_CONTROL		0xd406U
#define UREG_RX_BUFFER_THRESHOLD	0xd40cU
#define UREG_RX_QUIET_TIME		0xd42cU
#define UREG_RX_ROOM			0xd42eU
#define UREG_RX_HOLD_TIME		0xd432U
#define UREG_RX_DMA_OWNER		0xd437U
#define UREG_BUFFER_RESET		0xd4b0U
#define UREG_BUFFER_CONFIG		0xd4b4U
#define UREG_U1U2_TIMER			0xd4daU
#define UREG_FIRMWARE_TASK		0xd4e8U
#define UREG_POWER_CUT			0xd80aU
#define UREG_POWER_CUT_STATUS		0xd81aU

/*
 * PHY registers, reached through the OCP window, and the MII registers
 * at 0xa400 plus twice their number.
 *
 * Values referenced: FreeBSD 15.0 sys/dev/usb/net/if_urereg.h
 * (URE_OCP_*) and if_ure.c (ure_rtl8153b_init: 0xa466, 0xa468, 0xe000;
 * ure_ifmedia_upd: 0xa5d4); Linux 7.3 include/uapi/linux/mdio.h
 * (MDIO_AN_10GBT_CTRL_ADV2_5G, MDIO_AN_10GBT_CTRL_ADV5G).
 */
#define PHY_LINK_DOWN_SAVING		0x2010U
#define PHY_MII_BASE			0xa400U
#define PHY_STATE			0xa420U
#define PHY_POWER			0xa430U
#define PHY_A466			0xa466U
#define PHY_A468			0xa468U
#define PHY_MULTIGIG_ADVERTISE		0xa5d4U
#define PHY_E000			0xe000U
#define MII_BMCR			0U
#define MII_ANAR			4U
#define MII_GTCR			9U

/*
 * Register bits.
 *
 * Values referenced: FreeBSD 15.0 sys/dev/usb/net/if_urereg.h
 * (URE_RCR_*, URE_CR_*, URE_CRWECR_*, URE_NOW_IS_OOB,
 * URE_LINK_LIST_READY, URE_RXDY_GATED_EN, URE_MCU_BORW_EN,
 * URE_FMC_FCR_MCU_EN, URE_AUTOLOAD_DONE, URE_U2P3_ENABLE,
 * URE_LPM_U1U2_EN, URE_PWR_EN, URE_PCUT_STATUS, URE_UPS_EN,
 * URE_USP_PREWAKE, URE_UPCOMING_RUNTIME_D3, URE_LINK_CHG_EVENT,
 * URE_LINK_CHANGE_FLAG, URE_LINK_OFF_WAKE_EN, URE_CUR_LINK_OK,
 * URE_POLL_LINK_CHG, URE_MAC_CLK_SPDWN_EN, URE_PLA_MCU_SPDWN_EN,
 * URE_RX_AGG_DISABLE, URE_RX_ZERO_EN, URE_TALLY_RESET,
 * URE_BMU_RESET_EP_IN, URE_BMU_RESET_EP_OUT, URE_OWN_UPDATE,
 * URE_OWN_CLEAR, URE_FC_PATCH_TASK, URE_CTRL_TIMER_EN,
 * URE_FLOW_CTRL_PATCH_OPT, URE_TEREDO_*, URE_WDT6_SET_MODE,
 * URE_ENPDNPS, URE_LINKENA, URE_DIS_SDSAVE, URE_EN_ALDPS,
 * URE_PHY_STAT_*, URE_ADV_2500TFDX, URE_MTPS_JUMBO); the MII
 * registers' standard bits (IEEE 802.3 clause 22).
 */
#define RX_ACCEPT_PROMISCUOUS		0x00000001U
#define RX_ACCEPT_OWN			0x00000002U
#define RX_ACCEPT_MULTICAST		0x00000004U
#define RX_ACCEPT_BROADCAST		0x00000008U
#define RX_ACCEPT_ANY			(RX_ACCEPT_PROMISCUOUS | RX_ACCEPT_OWN | RX_ACCEPT_MULTICAST | RX_ACCEPT_BROADCAST)
#define RX_VLAN_STRIP			0x00c0U
#define COMMAND_RESET			0x10U
#define COMMAND_RECEIVE			0x08U
#define COMMAND_TRANSMIT		0x04U
#define CONFIG_WRITE_LOCKED		0x00U
#define CONFIG_WRITE_OPEN		0xc0U
#define OOB_ACTIVE			0x80U
#define RX_DMA_HELD			0x0008U
#define MCU_BORROW			0x4000U
#define FILTER_ENABLE			0x0001U
#define BOOT_LOADED			0x0002U
#define U2P3_ENABLE			0x0001U
#define LPM_U1U2_ENABLE			0x0001U
#define POWER_CUT_ENABLE		0x0001U
#define POWER_CUT_UPS			0x10U
#define POWER_CUT_PREWAKE		0x20U
#define MISC_0_PCUT_STATUS		0x0001U
#define INDICATE_RUNTIME_D3		0x01U
#define SUSPEND_LINK_CHANGE		0x01U
#define EXTRA_LINK_CHANGE_FLAG		0x0100U
#define EXTRA_CUR_LINK_OK		0x8000U
#define EXTRA_POLL_LINK_CHANGE		0x0001U
#define CONFIG34_LINK_OFF_WAKE		0x0008U
#define MAC_PWR_CTRL2_SPEED_DOWN	0x8000U
#define MAC_PWR_CTRL3_MCU_SPEED_DOWN	0x4000U
#define MAC_PWR_CTRL4_IDLE_SPEED_DOWN	0x0040U
#define USB_CTRL_RX_AGG_DISABLE		0x0010U
#define USB_CTRL_RX_ZERO		0x0080U
#define COUNTERS_CLEAR			0x0001U
#define BMU_RESET_IN_OUT		0x03U
#define RXDMA_OWN_UPDATE_CLEAR		0x03U
#define FW_TASK_FC_PATCH		0x0001U
#define FC_TIMER_ENABLE			0x8000U
#define FW_CTRL_FLOW_PATCH		0x0001U
#define TEREDO_ALL_OFF			0xffU
#define WDT6_SET_MODE			0x0010U
#define LINK_DOWN_SAVING_OFF		0x0310U
#define PHY_POWER_LINK_DOWN_SAVING	0x0004U
#define PHY_STATE_MASK			0x0007U
#define PHY_STATE_EXT_INIT		2U
#define PHY_STATE_LAN_ON		3U
#define PHY_STATE_POWER_DOWN		5U
#define MTPS_JUMBO			192U
#define BMCR_POWER_DOWN			0x0800U
#define BMCR_AUTONEG_RESTART		0x1200U
#define ANAR_ALL			0x0de1U
#define GTCR_1000			0x0300U
#define ADV_2500			0x0080U
#define ADV_5000			0x0100U

/*
 * Polls of a register that the device sets or clears on its own: ten
 * seconds at most, after which the setup goes on, as FreeBSD's does.
 */
#define RTL_POLL_TRIES			1000U
#define RTL_POLL_STEP_MS		10U

/*
 * The RTL8157's registers and bits beyond the RTL8156's: its own setup
 * registers, the window onto the buffer manager's and the IP block's
 * registers, the PHY's patch handshake, the PHY's two SRAM windows and
 * the PHY settings its setup changes.
 *
 * Values referenced: Linux 7.3 drivers/net/usb/r8152.c (USB_ADV_ADDR,
 * USB_ADV_DATA, USB_ADV_CMD, ADV_CMD_BMU, ADV_CMD_BUSY, ADV_CMD_WR,
 * ADV_CMD_IP, USB_U2P3_V2_CTRL, U2P3_V2_ENABLE, USB_MISC_2,
 * RX_DESC_16B, USB_RX_AGGR_NUM, RX_AGGR_NUM_MASK, PLA_PHY_PWR,
 * PFM_PWM_SWITCH, EEE_SPDWN_EN, OCP_SRAM_ADDR, OCP_SRAM_DATA,
 * OCP_SRAM2_ADDR, OCP_SRAM2_DATA, OCP_DOWN_SPEED, EN_EEE_100,
 * EN_EEE_1000, EN_10M_CLKDIV, EEE_CLKDIV_EN,
 * OCP_PHY_PATCH_STAT, OCP_PHY_PATCH_CMD, PATCH_READY, PATCH_REQUEST,
 * SRAM_GREEN_CFG, GREEN_ETH_EN; r8156_init: 0xcffe, 0xd3ca, 0xd4ae and
 * the buffer manager's 0x2300; rtl8156_up: PLA_RCR1 bit 3;
 * r8157_hw_phy_cfg: 0xa430, 0xa44a, 0xa428, 0xa5ea;
 * r8156_mdio_force_mode: 0xa5b4; r8153_aldps_en: PLA 0xe000).
 */
#define UREG_CFFE			0xcffeU
#define UREG_D3CA			0xd3caU
#define UREG_D4AE			0xd4aeU
#define UREG_RX_AGG_COUNT		0xd4eeU
#define UREG_INDIRECT_ADDRESS		0xd5d6U
#define UREG_INDIRECT_DATA		0xd5d8U
#define UREG_INDIRECT_COMMAND		0xd5dcU
#define REG_PHY_POWER_MODE		0xe84cU
#define REG_ALDPS_STATUS		0xe000U
#define INDIRECT_BMU			0x0000U
#define INDIRECT_BUSY			0x0001U
#define INDIRECT_WRITE			0x0002U
#define INDIRECT_IP			0x0004U
#define IP_U2P3				0xc2c0U
#define IP_U2P3_ENABLE			0x20000000U
#define BMU_RX_CONFIG			0x2300U
#define BMU_RX_ZERO			0x00000008U
#define RX_AGG_COUNT_MASK		0x01ffU
#define USB_CTRL_RX_DESC_16		0x0400U
#define UPS_FLAGS_POWER_CUT_GEN2	0x02U
#define RX_CONFIG_VLAN_GEN2		0x0008U
#define PHY_POWER_MODE_PFM		0x0040U
#define MAC_PWR_CTRL4_EEE_SPEED_DOWN	0x0001U
#define ALDPS_STATUS_OFF		0x0100U
#define PHY_SRAM_A_WINDOW		0xa436U
#define PHY_SRAM_A_PORT			0xa438U
#define PHY_SRAM_B_WINDOW		0xb87cU
#define PHY_SRAM_B_PORT			0xb87eU
#define PHY_CLOCK_SLOWDOWN		0xa442U
#define PHY_A428			0xa428U
#define PHY_A44A			0xa44aU
#define PHY_FORCE_MODE			0xa5b4U
#define PHY_A5EA			0xa5eaU
#define PHY_CHANGE_STATUS		0xb800U
#define PHY_CHANGE_CONTROL		0xb820U
#define SRAM_LOW_POWER			0x8011U
#define PHY_POWER_SAVING_PARAMETERS	0x0003U
#define PHY_POWER_EEE_SLOW_CLOCK	0x8000U
#define SLOWDOWN_10M_DIVIDE		0x0800U
#define SLOWDOWN_EEE_100		0x1000U
#define SLOWDOWN_EEE_1000		0x2000U
#define PHY_CHANGE_HELD			0x0040U
#define PHY_CHANGE_ASK			0x0010U
#define FORCE_MODE_MMD			0x8000U
#define SRAM_LOW_POWER_ON		0x8000U

/*
 * Polls of the indirect window's busy bit (Linux waits 20 ms at most)
 * and of the PHY's patch handshake (five seconds at most).
 */
#define RTL_INDIRECT_TRIES		10U
#define RTL_INDIRECT_STEP_MS		2U
#define RTL_PHY_HOLD_TRIES		5000U

/*
 * The adapter's driver state: the USB objects it uses, the chip and its
 * wire format, the transfers and their buffers, and the bookkeeping that
 * keeps the transfers, the polls, the worker thread and teardown apart.
 *
 * One lives from attach to the net device's release (or to detach, when
 * no net device was made).  The spin lock guards the counters and masks;
 * the register mutex keeps one register sequence at a time (open, close,
 * the MTU, the worker's link handling).
 */
struct rtl_adapter {
	struct drv_usb_device *usb_device;
	struct drv_usb_interface *interface;
	struct drv_usb_endpoint *bulk_in;
	struct drv_usb_endpoint *bulk_out;
	struct drv_usb_endpoint *interrupt_endpoint;
	enum drv_usb_rtl815x_chip chip;
	uint16_t version;
	struct drv_usb_rtl815x_format format;
	size_t rx_block_size;
	size_t interrupt_length;
	struct net_device *net_device;
	uint8_t mac[6];
	struct spinlock lock;
	struct mutex register_lock;

	/* The transfers. */
	struct drv_usb_urb *interrupt_urb;
	struct drv_usb_urb *rx_urbs[RTL_RX_URBS];
	struct drv_usb_urb *tx_urbs[RTL_TX_URBS];
	unsigned rx_urb_count;
	unsigned tx_urb_count;

	/*
	 * The receive buffers, one more than the receive transfers: a
	 * completed block is parsed from its buffer while its transfer goes
	 * back on with the spare, which the parsed buffer then becomes.  Only
	 * the poll that holds the block and the right to rearm changes them.
	 */
	uint8_t *rx_buffers[RTL_RX_URBS + 1U];
	uint8_t *rx_urb_buffers[RTL_RX_URBS];
	uint8_t *rx_spare;
	uint8_t *tx_buffers[RTL_TX_URBS];
	uint8_t *interrupt_buffer;

	/*
	 * Under the lock: the receive transfers on the bus in submission
	 * order (the controller completes them in that order); rx_done, a bit
	 * for each whose completion has come and not been taken; rx_idle, a
	 * bit for each off the bus waiting for a rearm.
	 */
	unsigned rx_order[RTL_RX_URBS];
	unsigned rx_order_head;
	unsigned rx_order_count;
	unsigned rx_done;
	unsigned rx_idle;

	/* Under the lock: free transmit transfers, and those completed and not taken. */
	unsigned tx_free;
	unsigned tx_done;

	/*
	 * Under the lock: the adapter's state.  ready is set at the end of
	 * attach and cleared at detach; opened between open and close;
	 * closing and stopping while a stop runs; quarantined after a stop or
	 * rearm that could not finish, which keeps the adapter down.
	 */
	unsigned ready;
	unsigned opened;
	unsigned closing;
	unsigned stopping;
	unsigned quarantined;

	/*
	 * Under the lock: the submits and polls in progress, which a stop
	 * waits out before it cancels the transfers.
	 */
	unsigned starts_active;
	unsigned polls_active;
	unsigned poll_cursor;

	/*
	 * Under the lock: rx_rearm asks for the idle receive transfers to go
	 * back on, rx_rearm_active is the right to do so (one holder), and
	 * rx_retries counts the rearms refused for want of resources.
	 */
	unsigned rx_rearm;
	unsigned rx_rearm_active;
	unsigned rx_retries;

	/*
	 * Under the lock: the transmit pump's flag (one thread builds and
	 * submits blocks at a time, so frames go out in order) and the frames
	 * waiting, linked through their next field.  Pure acknowledgements
	 * wait at the head, ahead of the data, in their own order;
	 * tx_queue_acks_last is the last of them, or NULL.
	 */
	unsigned tx_busy;
	struct packet_buf *tx_queue_head;
	struct packet_buf *tx_queue_tail;
	struct packet_buf *tx_queue_acks_last;
	unsigned tx_queue_count;

	/*
	 * The worker thread, which takes the link interrupts and reads the
	 * link with control transfers.  Under the lock: the work asked of it
	 * (RTL_WORK_*) and worker_stopping, which ends it at detach.
	 * link_up is the link the interrupt last told, which the worker
	 * compares with each interrupt.
	 */
	struct thread *worker;
	unsigned work;
	unsigned worker_stopping;
	unsigned link_up;
	unsigned interrupt_failures;
};

/*
 * The kinds of step in a table of PHY settings: bits of a PHY register
 * cleared and set, a PHY register written, or bits of a word in the
 * PHY's first or second SRAM window cleared and set.
 */
enum rtl_phy_step_kind {
	RTL_PHY_STEP_MODIFY,
	RTL_PHY_STEP_WRITE,
	RTL_PHY_STEP_SRAM_A,
	RTL_PHY_STEP_SRAM_B
};

/*
 * One step of a table of PHY settings: its kind, the register or SRAM
 * word, the bits cleared, and the bits set (the value, for a write).
 */
struct rtl_phy_step {
	enum rtl_phy_step_kind kind;
	uint16_t address;
	uint16_t clear;
	uint16_t set;
};

/*
 * Whether the boot parameters turn the driver off (rtl_boot_off), once
 * read (rtl_boot_checked).  The command line does not change after boot,
 * so the first match reads it and every later one uses the answer; the
 * USB core's enumeration thread is the only reader.
 */
static int rtl_boot_checked;
static int rtl_boot_off;

static int rtl_match(struct drv_usb_interface *interface, const struct drv_usb_id *id);
static int rtl_attach(struct drv_usb_interface *interface, const struct drv_usb_id *id);
static int rtl_detach(struct drv_usb_interface *interface, unsigned flags);
static void rtl_shutdown(struct drv_usb_interface *interface);
static int rtl_open(struct net_device *device);
static void rtl_close(struct net_device *device);
static int rtl_transmit(struct net_device *device, struct packet_buf *packet);
static unsigned rtl_poll_receive(struct net_device *device, unsigned budget);
static void rtl_release(void *driver_data);
static int rtl_set_mtu(struct net_device *device, unsigned mtu);

/*
 * The net device operations of an adapter.
 */
static const struct net_device_ops rtl_net_ops = {
	.open = rtl_open,
	.close = rtl_close,
	.transmit = rtl_transmit,
	.poll_receive = rtl_poll_receive,
	.release = rtl_release,
	.set_mtu = rtl_set_mtu
};

/*
 * The adapters this driver takes: Realtek's own RTL8156 and RTL8157 IDs,
 * on their vendor interface.  The chip itself is told from its version
 * register at attach.
 *
 * Values referenced: Linux 7.3 drivers/net/usb/r8152.c (rtl8152_table:
 * VENDOR_ID_REALTEK 0x8156, 0x8157).
 */
static const struct drv_usb_id rtl_ids[] = {
	{
		.match_flags = DRV_USB_ID_VENDOR | DRV_USB_ID_PRODUCT | DRV_USB_ID_IF_CLASS | DRV_USB_ID_IF_SUBCLASS,
		.vendor = RTL_VENDOR_REALTEK,
		.product = 0x8156U,
		.interface_class = RTL_VENDOR_CLASS,
		.interface_subclass = RTL_VENDOR_CLASS
	},
	{
		.match_flags = DRV_USB_ID_VENDOR | DRV_USB_ID_PRODUCT | DRV_USB_ID_IF_CLASS | DRV_USB_ID_IF_SUBCLASS,
		.vendor = RTL_VENDOR_REALTEK,
		.product = 0x8157U,
		.interface_class = RTL_VENDOR_CLASS,
		.interface_subclass = RTL_VENDOR_CLASS
	}
};

/*
 * The RTL8157's PHY tuning, one step per entry, applied in this order at
 * attach.  The entries tune the 5 and 2.5 Gbit/s receivers, the PHY's
 * low-power behaviour, how it treats some link partners, its resistance
 * to interference and its temperature limit.  Consecutive writes to an
 * SRAM port fill consecutive words after the address in its window.
 *
 * Values referenced: Linux 7.3 drivers/net/usb/r8152.c
 * (r8157_hw_phy_cfg, the RTL_VER_16 branch), as data.
 */
static const struct rtl_phy_step rtl_8157_phy_steps[] = {
	{ RTL_PHY_STEP_SRAM_A, 0x8183U, 0xff00U, 0x5900U },
	{ RTL_PHY_STEP_MODIFY, 0xa654U, 0x0000U, 0x0800U },
	{ RTL_PHY_STEP_MODIFY, 0xb648U, 0x0000U, 0x4000U },
	{ RTL_PHY_STEP_MODIFY, 0xad2cU, 0x8000U, 0x0000U },
	{ RTL_PHY_STEP_MODIFY, 0xad94U, 0x0000U, 0x0020U },
	{ RTL_PHY_STEP_MODIFY, 0xada0U, 0x0000U, 0x0002U },
	{ RTL_PHY_STEP_MODIFY, 0xae06U, 0xfc00U, 0x7c00U },
	{ RTL_PHY_STEP_SRAM_B, 0x8647U, 0xff00U, 0xe600U },
	{ RTL_PHY_STEP_SRAM_B, 0x8036U, 0xff00U, 0x3000U },
	{ RTL_PHY_STEP_SRAM_B, 0x8078U, 0xff00U, 0x3000U },
	{ RTL_PHY_STEP_SRAM_B, 0x89e9U, 0xff00U, 0x0000U },
	{ RTL_PHY_STEP_SRAM_B, 0x8ffdU, 0xff00U, 0x0100U },
	{ RTL_PHY_STEP_SRAM_B, 0x8ffeU, 0xff00U, 0x0200U },
	{ RTL_PHY_STEP_SRAM_B, 0x8fffU, 0xff00U, 0x0400U },
	{ RTL_PHY_STEP_SRAM_A, 0x8018U, 0xff00U, 0x7700U },
	{ RTL_PHY_STEP_WRITE, 0xa436U, 0x0000U, 0x8f9cU },
	{ RTL_PHY_STEP_WRITE, 0xa438U, 0x0000U, 0x0005U },
	{ RTL_PHY_STEP_WRITE, 0xa438U, 0x0000U, 0x0000U },
	{ RTL_PHY_STEP_WRITE, 0xa438U, 0x0000U, 0x00edU },
	{ RTL_PHY_STEP_WRITE, 0xa438U, 0x0000U, 0x0502U },
	{ RTL_PHY_STEP_WRITE, 0xa438U, 0x0000U, 0x0b00U },
	{ RTL_PHY_STEP_WRITE, 0xa438U, 0x0000U, 0xd401U },
	{ RTL_PHY_STEP_SRAM_A, 0x8fa8U, 0xff00U, 0x2900U },
	{ RTL_PHY_STEP_SRAM_B, 0x814bU, 0xff00U, 0x1100U },
	{ RTL_PHY_STEP_SRAM_B, 0x814dU, 0xff00U, 0x1100U },
	{ RTL_PHY_STEP_SRAM_B, 0x814fU, 0xff00U, 0x0b00U },
	{ RTL_PHY_STEP_SRAM_B, 0x8142U, 0xff00U, 0x0100U },
	{ RTL_PHY_STEP_SRAM_B, 0x8144U, 0xff00U, 0x0100U },
	{ RTL_PHY_STEP_SRAM_B, 0x8150U, 0xff00U, 0x0100U },
	{ RTL_PHY_STEP_SRAM_B, 0x8118U, 0xff00U, 0x0700U },
	{ RTL_PHY_STEP_SRAM_B, 0x811aU, 0xff00U, 0x0700U },
	{ RTL_PHY_STEP_SRAM_B, 0x811cU, 0xff00U, 0x0500U },
	{ RTL_PHY_STEP_SRAM_B, 0x810fU, 0xff00U, 0x0100U },
	{ RTL_PHY_STEP_SRAM_B, 0x8111U, 0xff00U, 0x0100U },
	{ RTL_PHY_STEP_SRAM_B, 0x811dU, 0xff00U, 0x0100U },
	{ RTL_PHY_STEP_MODIFY, 0xad1cU, 0x0100U, 0x0000U },
	{ RTL_PHY_STEP_MODIFY, 0xade8U, 0xffc0U, 0x1400U },
	{ RTL_PHY_STEP_SRAM_B, 0x864bU, 0xff00U, 0x9d00U },
	{ RTL_PHY_STEP_SRAM_B, 0x862cU, 0xff00U, 0x1200U },
	{ RTL_PHY_STEP_WRITE, 0xa436U, 0x0000U, 0x8566U },
	{ RTL_PHY_STEP_WRITE, 0xa438U, 0x0000U, 0x003fU },
	{ RTL_PHY_STEP_WRITE, 0xa438U, 0x0000U, 0x3f02U },
	{ RTL_PHY_STEP_WRITE, 0xa438U, 0x0000U, 0x023cU },
	{ RTL_PHY_STEP_WRITE, 0xa438U, 0x0000U, 0x3b0aU },
	{ RTL_PHY_STEP_WRITE, 0xa438U, 0x0000U, 0x1c00U },
	{ RTL_PHY_STEP_WRITE, 0xa438U, 0x0000U, 0x0000U },
	{ RTL_PHY_STEP_WRITE, 0xa438U, 0x0000U, 0x0000U },
	{ RTL_PHY_STEP_WRITE, 0xa438U, 0x0000U, 0x0000U },
	{ RTL_PHY_STEP_WRITE, 0xa438U, 0x0000U, 0x0000U },
	{ RTL_PHY_STEP_MODIFY, 0xad9cU, 0x0000U, 0x0020U },
	{ RTL_PHY_STEP_SRAM_B, 0x8122U, 0xff00U, 0x0c00U },
	{ RTL_PHY_STEP_WRITE, 0xb87cU, 0x0000U, 0x82c8U },
	{ RTL_PHY_STEP_WRITE, 0xb87eU, 0x0000U, 0x03edU },
	{ RTL_PHY_STEP_WRITE, 0xb87eU, 0x0000U, 0x03ffU },
	{ RTL_PHY_STEP_WRITE, 0xb87eU, 0x0000U, 0x0009U },
	{ RTL_PHY_STEP_WRITE, 0xb87eU, 0x0000U, 0x03feU },
	{ RTL_PHY_STEP_WRITE, 0xb87eU, 0x0000U, 0x000bU },
	{ RTL_PHY_STEP_WRITE, 0xb87eU, 0x0000U, 0x0021U },
	{ RTL_PHY_STEP_WRITE, 0xb87eU, 0x0000U, 0x03f7U },
	{ RTL_PHY_STEP_WRITE, 0xb87eU, 0x0000U, 0x03b8U },
	{ RTL_PHY_STEP_WRITE, 0xb87eU, 0x0000U, 0x03e0U },
	{ RTL_PHY_STEP_WRITE, 0xb87eU, 0x0000U, 0x0049U },
	{ RTL_PHY_STEP_WRITE, 0xb87eU, 0x0000U, 0x0049U },
	{ RTL_PHY_STEP_WRITE, 0xb87eU, 0x0000U, 0x03e0U },
	{ RTL_PHY_STEP_WRITE, 0xb87eU, 0x0000U, 0x03b8U },
	{ RTL_PHY_STEP_WRITE, 0xb87eU, 0x0000U, 0x03f7U },
	{ RTL_PHY_STEP_WRITE, 0xb87eU, 0x0000U, 0x0021U },
	{ RTL_PHY_STEP_WRITE, 0xb87eU, 0x0000U, 0x000bU },
	{ RTL_PHY_STEP_WRITE, 0xb87eU, 0x0000U, 0x03feU },
	{ RTL_PHY_STEP_WRITE, 0xb87eU, 0x0000U, 0x0009U },
	{ RTL_PHY_STEP_WRITE, 0xb87eU, 0x0000U, 0x03ffU },
	{ RTL_PHY_STEP_WRITE, 0xb87eU, 0x0000U, 0x03edU },
	{ RTL_PHY_STEP_SRAM_B, 0x80efU, 0xff00U, 0x0c00U },
	{ RTL_PHY_STEP_WRITE, 0xb87cU, 0x0000U, 0x82a0U },
	{ RTL_PHY_STEP_WRITE, 0xb87eU, 0x0000U, 0x000eU },
	{ RTL_PHY_STEP_WRITE, 0xb87eU, 0x0000U, 0x03feU },
	{ RTL_PHY_STEP_WRITE, 0xb87eU, 0x0000U, 0x03edU },
	{ RTL_PHY_STEP_WRITE, 0xb87eU, 0x0000U, 0x0006U },
	{ RTL_PHY_STEP_WRITE, 0xb87eU, 0x0000U, 0x001aU },
	{ RTL_PHY_STEP_WRITE, 0xb87eU, 0x0000U, 0x03f1U },
	{ RTL_PHY_STEP_WRITE, 0xb87eU, 0x0000U, 0x03d8U },
	{ RTL_PHY_STEP_WRITE, 0xb87eU, 0x0000U, 0x0023U },
	{ RTL_PHY_STEP_WRITE, 0xb87eU, 0x0000U, 0x0054U },
	{ RTL_PHY_STEP_WRITE, 0xb87eU, 0x0000U, 0x0322U },
	{ RTL_PHY_STEP_WRITE, 0xb87eU, 0x0000U, 0x00ddU },
	{ RTL_PHY_STEP_WRITE, 0xb87eU, 0x0000U, 0x03abU },
	{ RTL_PHY_STEP_WRITE, 0xb87eU, 0x0000U, 0x03dcU },
	{ RTL_PHY_STEP_WRITE, 0xb87eU, 0x0000U, 0x0027U },
	{ RTL_PHY_STEP_WRITE, 0xb87eU, 0x0000U, 0x000eU },
	{ RTL_PHY_STEP_WRITE, 0xb87eU, 0x0000U, 0x03e5U },
	{ RTL_PHY_STEP_WRITE, 0xb87eU, 0x0000U, 0x03f9U },
	{ RTL_PHY_STEP_WRITE, 0xb87eU, 0x0000U, 0x0012U },
	{ RTL_PHY_STEP_WRITE, 0xb87eU, 0x0000U, 0x0001U },
	{ RTL_PHY_STEP_WRITE, 0xb87eU, 0x0000U, 0x03f1U },
	{ RTL_PHY_STEP_MODIFY, 0xb54cU, 0xffc0U, 0x3700U },
	{ RTL_PHY_STEP_MODIFY, 0xb648U, 0x0000U, 0x0040U },
	{ RTL_PHY_STEP_SRAM_B, 0x8082U, 0xff00U, 0x5d00U },
	{ RTL_PHY_STEP_SRAM_B, 0x807cU, 0xff00U, 0x5000U },
	{ RTL_PHY_STEP_SRAM_B, 0x809dU, 0xff00U, 0x5000U },
};

/*
 * The driver as the USB core registers it.
 */
static struct drv_usb_driver rtl_driver = {
	.name = "usb-rtl815x",
	.ids = rtl_ids,
	.id_count = sizeof(rtl_ids) / sizeof(rtl_ids[0]),
	.match = rtl_match,
	.attach = rtl_attach,
	.detach = rtl_detach,
	.shutdown = rtl_shutdown
};

static void rtl_sleep_ms(unsigned milliseconds);
static int rtl_read(struct rtl_adapter *adapter, uint16_t space, uint16_t reg, unsigned width, uint32_t *value);
static int rtl_write(struct rtl_adapter *adapter, uint16_t space, uint16_t reg, unsigned width, uint32_t value);
static int rtl_modify(struct rtl_adapter *adapter, uint16_t space, uint16_t reg, unsigned width, uint32_t clear, uint32_t set);
static int rtl_wait_bits(struct rtl_adapter *adapter, uint16_t space, uint16_t reg, unsigned width, uint32_t mask, int set);
static int rtl_ocp_read(struct rtl_adapter *adapter, uint16_t address, uint16_t *value);
static int rtl_ocp_write(struct rtl_adapter *adapter, uint16_t address, uint16_t value);
static int rtl_ocp_modify(struct rtl_adapter *adapter, uint16_t address, uint16_t clear, uint16_t set);
static int rtl_indirect_wait(struct rtl_adapter *adapter);
static int rtl_indirect_read(struct rtl_adapter *adapter, uint16_t block, uint16_t address, uint32_t *value);
static int rtl_indirect_write(struct rtl_adapter *adapter, uint16_t block, uint16_t address, uint32_t value);
static int rtl_indirect_modify(struct rtl_adapter *adapter, uint16_t block, uint16_t address, uint32_t clear, uint32_t set);
static int rtl_sram_modify(struct rtl_adapter *adapter, uint16_t window, uint16_t data, uint16_t address, uint16_t clear, uint16_t set);
static int rtl_phy_steps_apply(struct rtl_adapter *adapter, const struct rtl_phy_step *steps, size_t count);
static int rtl_phy_hold_for_change(struct rtl_adapter *adapter, int request);
static int rtl_read_version(struct rtl_adapter *adapter);
static int rtl_aldps(struct rtl_adapter *adapter, int enable);
static int rtl_aldps_off_8157(struct rtl_adapter *adapter);
static int rtl_phy_wait_state(struct rtl_adapter *adapter, unsigned wanted, unsigned *state);
static int rtl_chip_init(struct rtl_adapter *adapter);
static int rtl_chip_init_first_8156(struct rtl_adapter *adapter);
static int rtl_chip_init_first_8157(struct rtl_adapter *adapter);
static int rtl_chip_wake(struct rtl_adapter *adapter);
static int rtl_wait_flash(struct rtl_adapter *adapter);
static int rtl_chip_init_power(struct rtl_adapter *adapter);
static int rtl_chip_init_finish(struct rtl_adapter *adapter);
static int rtl_chip_init_finish_8157(struct rtl_adapter *adapter);
static int rtl_phy_config_8157(struct rtl_adapter *adapter);
static int rtl_phy_config_8157_finish(struct rtl_adapter *adapter);
static int rtl_read_mac(struct rtl_adapter *adapter);
static int rtl_advertise(struct rtl_adapter *adapter);
static int rtl_nic_reset(struct rtl_adapter *adapter, unsigned mtu);
static int rtl_nic_reset_8157(struct rtl_adapter *adapter, unsigned mtu);
static int rtl_nic_fifo(struct rtl_adapter *adapter, unsigned mtu);
static int rtl_program_frame_size(struct rtl_adapter *adapter, unsigned mtu);
static int rtl_program_frame_fifo_8157(struct rtl_adapter *adapter, uint32_t frame);
static int rtl_nic_enable(struct rtl_adapter *adapter, unsigned mtu);
static int rtl_nic_disable(struct rtl_adapter *adapter);
static void rtl_link_update(struct rtl_adapter *adapter);
static void rtl_worker(void *argument);
static void rtl_worker_interrupt(struct rtl_adapter *adapter, int *link_check);
static void rtl_request(struct rtl_adapter *adapter, unsigned work);
static int rtl_worker_join(struct rtl_adapter *adapter);
static void rtl_completion(struct drv_usb_urb *urb, void *argument);
static int rtl_start_urb(struct rtl_adapter *adapter, struct drv_usb_urb *urb, void *buffer, size_t length, unsigned flags, unsigned timeout);
static int rtl_cancel_and_drain(struct drv_usb_urb *urb);
static void rtl_wait_activity(struct rtl_adapter *adapter);
static int rtl_stop(struct rtl_adapter *adapter);
static void rtl_tx_queue_free(struct rtl_adapter *adapter);
static void rtl_tx_pump(struct rtl_adapter *adapter);
static int rtl_tx_is_pure_ack(const struct packet_buf *packet);
static int rtl_tx_prepare(struct rtl_adapter *adapter, struct packet_buf *packet, struct drv_usb_rtl815x_tx_frame *frame);
static int rtl_tx_submit(struct rtl_adapter *adapter, unsigned index, struct packet_buf *packets);
static int rtl_rx_deliver(const void *frame, size_t length, unsigned checksums, void *argument);
static int rtl_rx_start_idle(struct rtl_adapter *adapter);
static int rtl_rx_head_done_locked(const struct rtl_adapter *adapter);
static unsigned rtl_lowest_bit(unsigned mask);
static int rtl_take_rx_rearm(struct rtl_adapter *adapter);
static int rtl_rearm(struct rtl_adapter *adapter);
static int rtl_poll_enter(struct rtl_adapter *adapter);
static void rtl_poll_exit(struct rtl_adapter *adapter);
static int rtl_poll_tx_completion(struct rtl_adapter *adapter);
static int rtl_poll_rx_completion(struct net_device *device, struct rtl_adapter *adapter);
static int rtl_has_poll_work(struct rtl_adapter *adapter);
static void rtl_set_ready(struct rtl_adapter *adapter, int ready);
static void rtl_urbs_free(struct rtl_adapter *adapter);
static int rtl_urbs_alloc(struct rtl_adapter *adapter);
static void rtl_buffers_free(struct rtl_adapter *adapter);
static int rtl_buffers_alloc(struct rtl_adapter *adapter);
static int rtl_net_device_create(struct rtl_adapter *adapter);
static int rtl_find_endpoints(struct drv_usb_interface *interface, struct drv_usb_endpoint **bulk_in, struct drv_usb_endpoint **bulk_out, struct drv_usb_endpoint **interrupt_endpoint);
static int rtl_turned_off(void);

/*
 * Registers this driver with the USB subsystem.
 */
int
drv_usb_rtl815x_driver_register(
	void)
{
	int error;

	/* Offers the vendor interfaces of the adapters to this driver. */
	error = drv_usb_driver_register(&rtl_driver);
	if (error != 0)
		return error;

	/* Succeeded: the driver takes the adapters it matches. */
	return 0;
}

/* Reports whether this driver takes an interface, above CDC NCM. */
static int
rtl_match(
	struct drv_usb_interface *interface,
	const struct drv_usb_id *id)
{
	struct drv_usb_endpoint *bulk_in;
	struct drv_usb_endpoint *bulk_out;
	struct drv_usb_endpoint *interrupt_endpoint;
	int error;
	int off;

	UNUSED_PARAMETER(id);

	/* The boot parameter leaves the adapter to CDC NCM. */
	off = rtl_turned_off();
	if (off)
		return 0;

	/* Takes only the vendor interface laid out as these chips lay it out. */
	error = rtl_find_endpoints(interface, &bulk_in, &bulk_out, &interrupt_endpoint);
	if (error != 0)
		return 0;

	/* Above CDC NCM, so the core picks the vendor configuration. */
	return RTL_MATCH_SCORE;
}

/* Reports whether the boot parameters turn the driver off, read once. */
static int
rtl_turned_off(void)
{
	const char *line;
	int present;

	/* Answers from the first reading. */
	if (rtl_boot_checked)
		return rtl_boot_off;

	/* The boot command line, which an early boot may not have. */
	present = 0;
	line = kern_boot_handoff("boot.command-line");
	if (line != NULL)
		present = kern_boot_parameters_token_present(line, RTL_OFF_TOKEN);
	rtl_boot_off = present != 0;
	rtl_boot_checked = 1;

	/* Whether the token is there. */
	return rtl_boot_off;
}

/*
 * Finds the vendor interface's bulk-in, bulk-out and interrupt-in
 * endpoints at the numbers these chips give them.
 */
static int
rtl_find_endpoints(
	struct drv_usb_interface *interface,
	struct drv_usb_endpoint **bulk_in,
	struct drv_usb_endpoint **bulk_out,
	struct drv_usb_endpoint **interrupt_endpoint)
{
	uint8_t address;

	/* The receive endpoint. */
	*bulk_in = drv_usb_interface_find_endpoint(interface, DRV_USB_TRANSFER_BULK, DRV_USB_DIR_IN, NULL);
	if (*bulk_in == NULL)
		return ENODEV;
	address = drv_usb_endpoint_address(*bulk_in);
	if ((address & 0x0fU) != RTL_ENDPOINT_RX)
		return ENODEV;

	/* The transmit endpoint. */
	*bulk_out = drv_usb_interface_find_endpoint(interface, DRV_USB_TRANSFER_BULK, DRV_USB_DIR_OUT, NULL);
	if (*bulk_out == NULL)
		return ENODEV;
	address = drv_usb_endpoint_address(*bulk_out);
	if ((address & 0x0fU) != RTL_ENDPOINT_TX)
		return ENODEV;

	/* The link interrupt's endpoint. */
	*interrupt_endpoint = drv_usb_interface_find_endpoint(interface, DRV_USB_TRANSFER_INTERRUPT, DRV_USB_DIR_IN, NULL);
	if (*interrupt_endpoint == NULL)
		return ENODEV;
	address = drv_usb_endpoint_address(*interrupt_endpoint);
	if ((address & 0x0fU) != RTL_ENDPOINT_INTERRUPT)
		return ENODEV;

	/* Succeeded: the interface is the vendor one. */
	return 0;
}

/* Sleeps the calling thread for about the milliseconds given. */
static void
rtl_sleep_ms(
	unsigned milliseconds)
{
	uint64_t ticks;
	uint64_t now;

	/* At least one tick. */
	ticks = kern_ms_to_ticks(milliseconds);
	if (ticks == 0U)
		ticks = 1U;

	/* Until the tick count passes the deadline. */
	now = sched_ticks();
	sched_sleep(now + ticks);
}

/*
 * Reads a register of width 1, 2 or 4 bytes.
 *
 * Procedure referenced: FreeBSD 15.0 sys/dev/usb/net/if_ure.c (ure_ctl,
 * ure_read_1, ure_read_2, ure_read_4): a vendor read of the aligned dword.
 */
static int
rtl_read(
	struct rtl_adapter *adapter,
	uint16_t space,
	uint16_t reg,
	unsigned width,
	uint32_t *value)
{
	struct drv_usb_rtl815x_access access;
	uint8_t bytes[4];
	size_t actual;
	uint32_t dword;
	int error;

	/* Lays the access out. */
	error = drv_usb_rtl815x_access_for(reg, width, &access);
	if (error != 0)
		return error;

	/* Reads the dword. */
	actual = 0;
	error = drv_usb_control(adapter->usb_device,
				DRV_USB_DIR_IN | DRV_USB_REQUEST_VENDOR | DRV_USB_RECIP_DEVICE,
				DRV_USB_RTL815X_REQUEST,
				access.address,
				space,
				bytes,
				sizeof(bytes),
				RTL_CONTROL_TIMEOUT_MS,
				&actual);
	if (error != 0)
		return error;
	if (actual != sizeof(bytes))
		return EIO;

	/* Takes the bytes of the width out of the dword. */
	dword = (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) |
		((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
	dword >>= access.shift;
	if (width == 1)
		dword &= 0xffU;
	else if (width == 2)
		dword &= 0xffffU;
	*value = dword;

	/* Succeeded: the register's value. */
	return 0;
}

/*
 * Writes a register of width 1, 2 or 4 bytes.
 *
 * Procedure referenced: FreeBSD 15.0 sys/dev/usb/net/if_ure.c (ure_ctl,
 * ure_write_1, ure_write_2, ure_write_4): a vendor write of the aligned
 * dword, the byte enable in the index selecting the bytes written.
 */
static int
rtl_write(
	struct rtl_adapter *adapter,
	uint16_t space,
	uint16_t reg,
	unsigned width,
	uint32_t value)
{
	struct drv_usb_rtl815x_access access;
	uint8_t bytes[4];
	size_t actual;
	uint32_t dword;
	int error;

	/* Lays the access out. */
	error = drv_usb_rtl815x_access_for(reg, width, &access);
	if (error != 0)
		return error;

	/* Shifts the value into its lanes. */
	dword = value << access.shift;
	bytes[0] = (uint8_t)dword;
	bytes[1] = (uint8_t)(dword >> 8);
	bytes[2] = (uint8_t)(dword >> 16);
	bytes[3] = (uint8_t)(dword >> 24);

	/* Writes the selected bytes. */
	actual = 0;
	error = drv_usb_control(adapter->usb_device,
				DRV_USB_DIR_OUT | DRV_USB_REQUEST_VENDOR | DRV_USB_RECIP_DEVICE,
				DRV_USB_RTL815X_REQUEST,
				access.address,
				(uint16_t)(space | access.byte_enable),
				bytes,
				sizeof(bytes),
				RTL_CONTROL_TIMEOUT_MS,
				&actual);
	if (error != 0)
		return error;

	/* Succeeded: the register is written. */
	return 0;
}

/* Clears and then sets bits of a register. */
static int
rtl_modify(
	struct rtl_adapter *adapter,
	uint16_t space,
	uint16_t reg,
	unsigned width,
	uint32_t clear,
	uint32_t set)
{
	uint32_t value;
	int error;

	/* Reads the register. */
	error = rtl_read(adapter, space, reg, width, &value);
	if (error != 0)
		return error;

	/* Writes it back changed. */
	value = (value & ~clear) | set;
	error = rtl_write(adapter, space, reg, width, value);
	if (error != 0)
		return error;

	/* Succeeded: the bits are changed. */
	return 0;
}

/*
 * Waits until bits of a register are all set (set) or all clear, for a
 * second at most; ETIMEDOUT when they never are.
 */
static int
rtl_wait_bits(
	struct rtl_adapter *adapter,
	uint16_t space,
	uint16_t reg,
	unsigned width,
	uint32_t mask,
	int set)
{
	uint32_t value;
	unsigned tries;
	int error;

	/* Reads until the bits are as wanted. */
	for (tries = 0; tries < RTL_POLL_TRIES; tries++) {
		error = rtl_read(adapter, space, reg, width, &value);
		if (error != 0)
			return error;
		if (set && (value & mask) == mask)
			return 0;
		if (!set && (value & mask) == 0)
			return 0;
		rtl_sleep_ms(RTL_POLL_STEP_MS);
	}

	/* The bits never came. */
	return ETIMEDOUT;
}

/*
 * Reads a PHY register through the OCP window.
 *
 * Procedure referenced: FreeBSD 15.0 sys/dev/usb/net/if_ure.c
 * (ure_ocp_reg_read): the window base takes the address's top four bits.
 */
static int
rtl_ocp_read(
	struct rtl_adapter *adapter,
	uint16_t address,
	uint16_t *value)
{
	uint32_t word;
	int error;

	/* Points the window at the address's page. */
	error = rtl_write(adapter, DRV_USB_RTL815X_SPACE_PLA, REG_PHY_WINDOW, 2, address & 0xf000U);
	if (error != 0)
		return error;

	/* Reads the register through it. */
	error = rtl_read(adapter, DRV_USB_RTL815X_SPACE_PLA, (uint16_t)((address & 0x0fffU) | 0xb000U), 2, &word);
	if (error != 0)
		return error;
	*value = (uint16_t)word;

	/* Succeeded: the PHY register's value. */
	return 0;
}

/* Writes a PHY register through the OCP window. */
static int
rtl_ocp_write(
	struct rtl_adapter *adapter,
	uint16_t address,
	uint16_t value)
{
	int error;

	/* Points the window at the address's page. */
	error = rtl_write(adapter, DRV_USB_RTL815X_SPACE_PLA, REG_PHY_WINDOW, 2, address & 0xf000U);
	if (error != 0)
		return error;

	/* Writes the register through it. */
	error = rtl_write(adapter, DRV_USB_RTL815X_SPACE_PLA, (uint16_t)((address & 0x0fffU) | 0xb000U), 2, value);
	if (error != 0)
		return error;

	/* Succeeded: the PHY register is written. */
	return 0;
}

/* Clears and then sets bits of a PHY register. */
static int
rtl_ocp_modify(
	struct rtl_adapter *adapter,
	uint16_t address,
	uint16_t clear,
	uint16_t set)
{
	uint16_t value;
	int error;

	/* Reads the register. */
	error = rtl_ocp_read(adapter, address, &value);
	if (error != 0)
		return error;

	/* Writes it back changed. */
	value = (uint16_t)((value & ~clear) | set);
	error = rtl_ocp_write(adapter, address, value);
	if (error != 0)
		return error;

	/* Succeeded: the bits are changed. */
	return 0;
}

/*
 * Waits for the indirect window to finish its last command; ETIMEDOUT
 * when it stays busy.
 *
 * Procedure referenced: Linux 7.3 drivers/net/usb/r8152.c
 * (wait_cmd_ready): the busy bit of the command, 20 ms at most.
 */
static int
rtl_indirect_wait(
	struct rtl_adapter *adapter)
{
	uint32_t command;
	unsigned tries;
	int error;

	/* Reads the command until its busy bit is clear. */
	for (tries = 0; tries < RTL_INDIRECT_TRIES; tries++) {
		error = rtl_read(adapter, DRV_USB_RTL815X_SPACE_USB, UREG_INDIRECT_COMMAND, 2, &command);
		if (error != 0)
			return error;
		if ((command & INDIRECT_BUSY) == 0)
			return 0;
		rtl_sleep_ms(RTL_INDIRECT_STEP_MS);
	}

	/* The window stayed busy. */
	return ETIMEDOUT;
}

/*
 * Reads a register of the RTL8157's buffer manager (INDIRECT_BMU) or IP
 * block (INDIRECT_IP) through the indirect window.
 *
 * Procedure referenced: Linux 7.3 drivers/net/usb/r8152.c
 * (ocp_adv_read): the address, then the command with the busy bit, a
 * wait, and the data.
 */
static int
rtl_indirect_read(
	struct rtl_adapter *adapter,
	uint16_t block,
	uint16_t address,
	uint32_t *value)
{
	int error;

	/* The window takes no command while it runs one. */
	error = rtl_indirect_wait(adapter);
	if (error != 0)
		return error;

	/* Asks for the register. */
	error = rtl_write(adapter, DRV_USB_RTL815X_SPACE_USB, UREG_INDIRECT_ADDRESS, 2, address);
	if (error != 0)
		return error;
	error = rtl_write(adapter, DRV_USB_RTL815X_SPACE_USB, UREG_INDIRECT_COMMAND, 2, (uint32_t)block | INDIRECT_BUSY);
	if (error != 0)
		return error;

	/* Waits for the read and takes its value. */
	error = rtl_indirect_wait(adapter);
	if (error != 0)
		return error;
	error = rtl_read(adapter, DRV_USB_RTL815X_SPACE_USB, UREG_INDIRECT_DATA, 4, value);
	if (error != 0)
		return error;

	/* Succeeded: the register's value. */
	return 0;
}

/*
 * Writes a register of the RTL8157's buffer manager or IP block through
 * the indirect window.  The write runs on in the chip; the next command
 * waits for it.
 *
 * Procedure referenced: Linux 7.3 drivers/net/usb/r8152.c
 * (ocp_adv_write): the data, the address, then the command with the
 * write and busy bits.
 */
static int
rtl_indirect_write(
	struct rtl_adapter *adapter,
	uint16_t block,
	uint16_t address,
	uint32_t value)
{
	int error;

	/* The window takes no command while it runs one. */
	error = rtl_indirect_wait(adapter);
	if (error != 0)
		return error;

	/* Gives the value and the register, then starts the write. */
	error = rtl_write(adapter, DRV_USB_RTL815X_SPACE_USB, UREG_INDIRECT_DATA, 4, value);
	if (error != 0)
		return error;
	error = rtl_write(adapter, DRV_USB_RTL815X_SPACE_USB, UREG_INDIRECT_ADDRESS, 2, address);
	if (error != 0)
		return error;
	error = rtl_write(adapter,
			  DRV_USB_RTL815X_SPACE_USB,
			  UREG_INDIRECT_COMMAND,
			  2,
			  (uint32_t)block | INDIRECT_WRITE | INDIRECT_BUSY);
	if (error != 0)
		return error;

	/* Succeeded: the write is started. */
	return 0;
}

/* Clears and then sets bits of a buffer manager or IP block register. */
static int
rtl_indirect_modify(
	struct rtl_adapter *adapter,
	uint16_t block,
	uint16_t address,
	uint32_t clear,
	uint32_t set)
{
	uint32_t value;
	int error;

	/* Reads the register. */
	error = rtl_indirect_read(adapter, block, address, &value);
	if (error != 0)
		return error;

	/* Writes it back changed. */
	value = (value & ~clear) | set;
	error = rtl_indirect_write(adapter, block, address, value);
	if (error != 0)
		return error;

	/* Succeeded: the bits are changed. */
	return 0;
}

/*
 * Clears and then sets bits of a word in one of the PHY's SRAM windows
 * (an address register and the data register after it).
 *
 * Procedure referenced: Linux 7.3 drivers/net/usb/r8152.c
 * (sram_write_w0w1, sram2_write_w0w1): the word's address, a read of the
 * data, and the changed data written back without a new address.
 */
static int
rtl_sram_modify(
	struct rtl_adapter *adapter,
	uint16_t window,
	uint16_t data,
	uint16_t address,
	uint16_t clear,
	uint16_t set)
{
	uint16_t value;
	int error;

	/* Points the window at the word and reads it. */
	error = rtl_ocp_write(adapter, window, address);
	if (error != 0)
		return error;
	error = rtl_ocp_read(adapter, data, &value);
	if (error != 0)
		return error;

	/* Writes it back changed. */
	value = (uint16_t)((value & ~clear) | set);
	error = rtl_ocp_write(adapter, data, value);
	if (error != 0)
		return error;

	/* Succeeded: the bits are changed. */
	return 0;
}

/* Applies a table of PHY settings in order, stopping at the first failure. */
static int
rtl_phy_steps_apply(
	struct rtl_adapter *adapter,
	const struct rtl_phy_step *steps,
	size_t count)
{
	const struct rtl_phy_step *step;
	size_t index;
	int error;

	/* Applies each step by its kind. */
	for (index = 0; index < count; index++) {
		step = &steps[index];
		error = EINVAL;

		/* Reaches the register or the SRAM word as the kind says. */
		switch (step->kind) {
		case RTL_PHY_STEP_MODIFY:
			error = rtl_ocp_modify(adapter, step->address, step->clear, step->set);
			break;
		case RTL_PHY_STEP_WRITE:
			error = rtl_ocp_write(adapter, step->address, step->set);
			break;
		case RTL_PHY_STEP_SRAM_A:
			error = rtl_sram_modify(adapter, PHY_SRAM_A_WINDOW, PHY_SRAM_A_PORT, step->address, step->clear, step->set);
			break;
		case RTL_PHY_STEP_SRAM_B:
			error = rtl_sram_modify(adapter, PHY_SRAM_B_WINDOW, PHY_SRAM_B_PORT, step->address, step->clear, step->set);
			break;
		}

		/* A step that failed ends the table. */
		if (error != 0)
			return error;
	}

	/* Succeeded: every step is applied. */
	return 0;
}

/*
 * Asks the PHY to stop for a change of its settings (request), or lets
 * it go on, and waits for it to say so.
 *
 * Procedure referenced: Linux 7.3 drivers/net/usb/r8152.c
 * (rtl_phy_hold_for_change): the request bit, then the ready bit set (or
 * cleared) within five seconds; a request never granted is withdrawn.
 */
static int
rtl_phy_hold_for_change(
	struct rtl_adapter *adapter,
	int request)
{
	uint16_t status;
	uint16_t wanted;
	unsigned tries;
	int error;

	/* Raises or lowers the request. */
	if (request) {
		error = rtl_ocp_modify(adapter, PHY_CHANGE_CONTROL, 0, PHY_CHANGE_ASK);
		wanted = PHY_CHANGE_HELD;
	} else {
		error = rtl_ocp_modify(adapter, PHY_CHANGE_CONTROL, PHY_CHANGE_ASK, 0);
		wanted = 0;
	}

	/* Reports a patch request the PHY could not take. */
	if (error != 0)
		return error;

	/* Waits for the ready bit to follow. */
	for (tries = 0; tries < RTL_PHY_HOLD_TRIES; tries++) {
		rtl_sleep_ms(1U);
		error = rtl_ocp_read(adapter, PHY_CHANGE_STATUS, &status);
		if (error != 0)
			return error;
		if ((status & PHY_CHANGE_HELD) == wanted)
			return 0;
	}

	/* A request never granted is withdrawn. */
	if (request)
		(void)rtl_ocp_modify(adapter, PHY_CHANGE_CONTROL, PHY_CHANGE_ASK, 0);

	/* The PHY did not answer. */
	return ETIMEDOUT;
}

/*
 * Reads the chip's version and tells the chip from it.
 *
 * Procedure referenced: FreeBSD 15.0 sys/dev/usb/net/if_ure.c
 * (ure_read_chipver): REG_VERSION masked with 0x7cf0.
 */
static int
rtl_read_version(
	struct rtl_adapter *adapter)
{
	uint32_t value;
	int error;

	/* Reads the version register. */
	error = rtl_read(adapter, DRV_USB_RTL815X_SPACE_PLA, REG_VERSION, 2, &value);
	if (error != 0)
		return error;

	/* Tells the chip. */
	adapter->version = (uint16_t)(value & 0x7cf0U);
	adapter->chip = drv_usb_rtl815x_chip_from_version(adapter->version);

	/* Succeeded: the chip is known (or known not to be driven). */
	return 0;
}

/*
 * Turns the PHY's link-down power saving on or off.
 *
 * Procedure referenced: FreeBSD 15.0 sys/dev/usb/net/if_ure.c
 * (ure_enable_aldps).
 */
static int
rtl_aldps(
	struct rtl_adapter *adapter,
	int enable)
{
	uint16_t value;
	unsigned tries;
	int error;

	/* Enabling sets one bit of the power configuration. */
	if (enable) {
		error = rtl_ocp_modify(adapter, PHY_POWER, 0, PHY_POWER_LINK_DOWN_SAVING);
		if (error != 0)
			return error;
		return 0;
	}

	/* The RTL8157 turns it off its own way. */
	if (adapter->chip == DRV_USB_RTL815X_CHIP_RTL8157) {
		error = rtl_aldps_off_8157(adapter);
		if (error != 0)
			return error;
		return 0;
	}

	/* Disabling writes the configuration and waits for the PHY to leave the mode. */
	error = rtl_ocp_write(adapter, PHY_LINK_DOWN_SAVING, LINK_DOWN_SAVING_OFF);
	if (error != 0)
		return error;

	/* Waits up to about 20 ms for the PHY to report it. */
	for (tries = 0; tries < 20U; tries++) {
		rtl_sleep_ms(1U);
		error = rtl_ocp_read(adapter, PHY_E000, &value);
		if (error != 0)
			return error;
		if ((value & 0x0100U) != 0)
			break;
	}

	/* Succeeded: the power saving is off (or as off as the PHY says). */
	return 0;
}

/*
 * Turns the RTL8157 PHY's link-down power saving off and waits about
 * 20 ms at most for the MAC to see the mode end.
 *
 * Procedure referenced: Linux 7.3 drivers/net/usb/r8152.c
 * (r8153_aldps_en): the bit of the power configuration, then PLA 0xe000
 * bit 8.
 */
static int
rtl_aldps_off_8157(
	struct rtl_adapter *adapter)
{
	uint32_t value;
	unsigned tries;
	int error;

	/* Clears the power saving's bit. */
	error = rtl_ocp_modify(adapter, PHY_POWER, PHY_POWER_LINK_DOWN_SAVING, 0);
	if (error != 0)
		return error;

	/* Waits for the MAC to report the mode off. */
	for (tries = 0; tries < 20U; tries++) {
		rtl_sleep_ms(1U);
		error = rtl_read(adapter, DRV_USB_RTL815X_SPACE_PLA, REG_ALDPS_STATUS, 2, &value);
		if (error != 0)
			return error;
		if ((value & ALDPS_STATUS_OFF) != 0)
			break;
	}

	/* Succeeded: the power saving is off (or as off as the MAC says). */
	return 0;
}

/*
 * Waits for the PHY to reach a state (wanted), or any settled one when
 * wanted is 0, and reports the state it is in.
 *
 * Procedure referenced: FreeBSD 15.0 sys/dev/usb/net/if_ure.c
 * (ure_phy_status).
 */
static int
rtl_phy_wait_state(
	struct rtl_adapter *adapter,
	unsigned wanted,
	unsigned *state)
{
	uint16_t value;
	unsigned current;
	unsigned tries;
	int error;

	/* Reads the state until it is the wanted one or a settled one. */
	current = 0;
	for (tries = 0; tries < RTL_POLL_TRIES; tries++) {
		error = rtl_ocp_read(adapter, PHY_STATE, &value);
		if (error != 0)
			return error;
		current = value & PHY_STATE_MASK;
		*state = current;
		if (wanted != 0 && current == wanted)
			return 0;
		if (wanted == 0 &&
		    (current == PHY_STATE_LAN_ON ||
		     current == PHY_STATE_POWER_DOWN ||
		     current == PHY_STATE_EXT_INIT))
			return 0;
		rtl_sleep_ms(RTL_POLL_STEP_MS);
	}

	/* The PHY did not settle. */
	return ETIMEDOUT;
}

/*
 * Brings the chip out of its power-on state, once at attach.
 *
 * Procedure referenced: FreeBSD 15.0 sys/dev/usb/net/if_ure.c
 * (ure_rtl8153b_init, the RTL8156 and RTL8156B branches); the RTL8157's
 * first settings from Linux 7.3 drivers/net/usb/r8152.c (r8156_init,
 * the RTL_VER_16 branch).
 */
static int
rtl_chip_init(
	struct rtl_adapter *adapter)
{
	int error;

	/* The first settings, which differ by chip. */
	if (adapter->chip == DRV_USB_RTL815X_CHIP_RTL8157)
		error = rtl_chip_init_first_8157(adapter);
	else
		error = rtl_chip_init_first_8156(adapter);
	if (error != 0)
		return error;

	/* The link power states stay off while the chip is set up. */
	error = rtl_modify(adapter, DRV_USB_RTL815X_SPACE_USB, UREG_LINK_POWER, 2, LPM_U1U2_ENABLE, 0);
	if (error != 0)
		return error;

	/* The chip's own load, and the PHY on the LAN. */
	error = rtl_chip_wake(adapter);
	if (error != 0)
		return error;

	/* The power settings and the rest. */
	error = rtl_chip_init_power(adapter);
	if (error != 0)
		return error;
	error = rtl_chip_init_finish(adapter);
	if (error != 0)
		return error;

	/* Succeeded: the chip is ready to be brought up. */
	return 0;
}

/*
 * The RTL8156 family's first settings, with the PHY's power saving off.
 *
 * Procedure referenced: FreeBSD 15.0 sys/dev/usb/net/if_ure.c
 * (ure_rtl8153b_init, the RTL8156 and RTL8156B branches).
 */
static int
rtl_chip_init_first_8156(
	struct rtl_adapter *adapter)
{
	int error;

	/* The RTL8156 family's first settings. */
	error = rtl_modify(adapter, DRV_USB_RTL815X_SPACE_USB, UREG_D26B, 1, 0x01U, 0);
	if (error != 0)
		return error;
	error = rtl_write(adapter, DRV_USB_RTL815X_SPACE_USB, UREG_SPEED_OPTIONS, 2, 0);
	if (error != 0)
		return error;
	error = rtl_modify(adapter, DRV_USB_RTL815X_SPACE_USB, UREG_CFEE, 2, 0, 0x0020U);
	if (error != 0)
		return error;

	/* The RTL8156B's extra one. */
	if (adapter->chip == DRV_USB_RTL815X_CHIP_RTL8156B) {
		error = rtl_modify(adapter, DRV_USB_RTL815X_SPACE_USB, UREG_U2P3, 2, 0, 0x08U);
		if (error != 0)
			return error;
	}

	/* The PHY's power saving stays off while it is set up. */
	error = rtl_aldps(adapter, 0);
	if (error != 0)
		return error;

	/* Succeeded: the first settings are made. */
	return 0;
}

/*
 * The RTL8157's first settings: two of its own, all-speed mode off and
 * the MAC reset bypassed; the speed options are left alone.
 *
 * Values and procedure referenced: Linux 7.3 drivers/net/usb/r8152.c
 * (r8156_init: 0xcffe bit 3 set, 0xd3ca bit 0 cleared, USB_ECM_OP
 * EN_ALL_SPEED cleared, USB_ECM_OPTION BYPASS_MAC_RESET set).
 */
static int
rtl_chip_init_first_8157(
	struct rtl_adapter *adapter)
{
	int error;

	/* The RTL8157's own two. */
	error = rtl_modify(adapter, DRV_USB_RTL815X_SPACE_USB, UREG_CFFE, 1, 0, 0x08U);
	if (error != 0)
		return error;
	error = rtl_modify(adapter, DRV_USB_RTL815X_SPACE_USB, UREG_D3CA, 1, 0x01U, 0);
	if (error != 0)
		return error;

	/* The ones it shares with the RTL8156. */
	error = rtl_modify(adapter, DRV_USB_RTL815X_SPACE_USB, UREG_D26B, 1, 0x01U, 0);
	if (error != 0)
		return error;
	error = rtl_modify(adapter, DRV_USB_RTL815X_SPACE_USB, UREG_CFEE, 2, 0, 0x0020U);
	if (error != 0)
		return error;

	/* Succeeded: the first settings are made. */
	return 0;
}

/*
 * Waits for the chip to load its PHY settings from flash and its
 * defaults, lets a PHY in its external initialization go on, powers the
 * PHY up and waits for it to be on the LAN.  A chip that never reports a
 * step is set up anyway, as FreeBSD's is.
 *
 * Procedure referenced: FreeBSD 15.0 sys/dev/usb/net/if_ure.c
 * (ure_rtl8153b_init); the flash load of 0x7420 and the RTL8157 as
 * Linux 7.3 drivers/net/usb/r8152.c (r8156_init,
 * r8156b_wait_loading_flash) treats them.
 */
static int
rtl_chip_wake(
	struct rtl_adapter *adapter)
{
	uint16_t phy;
	unsigned state;
	int error;

	/* A chip that loads its PHY settings from flash finishes the load first. */
	if (adapter->version == 0x7410U ||
	    adapter->version == 0x7420U ||
	    adapter->chip == DRV_USB_RTL815X_CHIP_RTL8157) {
		error = rtl_wait_flash(adapter);
		if (error != 0)
			return error;
	}

	/* Waits for the chip to have loaded its defaults; one that never says so is set up anyway. */
	error = rtl_wait_bits(adapter, DRV_USB_RTL815X_SPACE_PLA, REG_BOOT_STATUS, 2, BOOT_LOADED, 1);
	if (error == ETIMEDOUT)
		kern_logf("usb-rtl815x: chip autoload not reported, going on\n");
	else if (error != 0)
		return error;

	/* A PHY still in its external initialization is let go on; one that never settles is set up anyway. */
	state = 0;
	error = rtl_phy_wait_state(adapter, 0, &state);
	if (error == ETIMEDOUT)
		kern_logf("usb-rtl815x: PHY state %u did not settle, going on\n", state);
	else if (error != 0)
		return error;
	if (state == PHY_STATE_EXT_INIT) {
		error = rtl_ocp_modify(adapter, PHY_A468, 0x000aU, 0);
		if (error != 0)
			return error;
		if (adapter->chip != DRV_USB_RTL815X_CHIP_RTL8156) {
			error = rtl_ocp_modify(adapter, PHY_A466, 0x0001U, 0);
			if (error != 0)
				return error;
		}
	}

	/* Powers the PHY up and waits for it to be on the LAN. */
	error = rtl_ocp_read(adapter, PHY_MII_BASE + MII_BMCR * 2U, &phy);
	if (error != 0)
		return error;
	if ((phy & BMCR_POWER_DOWN) != 0) {
		error = rtl_ocp_write(adapter, PHY_MII_BASE + MII_BMCR * 2U, (uint16_t)(phy & ~BMCR_POWER_DOWN));
		if (error != 0)
			return error;
	}

	/* Waits for the PHY to reach its LAN-on state. */
	error = rtl_phy_wait_state(adapter, PHY_STATE_LAN_ON, &state);
	if (error == ETIMEDOUT)
		kern_logf("usb-rtl815x: PHY state %u is not LAN-on, going on\n", state);
	else if (error != 0)
		return error;

	/* Succeeded: the PHY is up. */
	return 0;
}

/*
 * Waits for a chip that loads its PHY settings from flash to finish the
 * load, unless the load is bypassed.
 *
 * Values and procedure referenced: Linux 7.3 drivers/net/usb/r8152.c
 * (r8156b_wait_loading_flash: PLA_GPHY_CTRL GPHY_FLASH, USB_GPHY_CTRL
 * BYPASS_FLASH and GPHY_PATCH_DONE).
 */
static int
rtl_wait_flash(
	struct rtl_adapter *adapter)
{
	uint32_t value;
	int error;

	/* A chip without the flash has nothing to wait for. */
	error = rtl_read(adapter, DRV_USB_RTL815X_SPACE_PLA, REG_FLASH_STATUS, 2, &value);
	if (error != 0)
		return error;
	if ((value & 0x0002U) == 0)
		return 0;

	/* Nor has one told to bypass it. */
	error = rtl_read(adapter, DRV_USB_RTL815X_SPACE_USB, UREG_FLASH_LOAD, 2, &value);
	if (error != 0)
		return error;
	if ((value & 0x0020U) != 0)
		return 0;

	/* Waits for the load to be done; one that never says so is set up anyway. */
	(void)rtl_wait_bits(adapter, DRV_USB_RTL815X_SPACE_USB, UREG_FLASH_LOAD, 2, 0x0004U, 1);

	/* Succeeded: the flash is loaded (or as loaded as the chip says). */
	return 0;
}

/*
 * Turns the chip's power saving off: the U2P3 state, power cut, UPS, the
 * wake on queue and runtime suspend, which an adapter that stays in use
 * does not want; and sets the timers of the link power states.
 *
 * Procedure referenced: FreeBSD 15.0 sys/dev/usb/net/if_ure.c
 * (ure_rtl8153b_init).  The suspend and wake flags are PLA registers
 * (Linux 7.3 drivers/net/usb/r8152.c: r8153_queue_wake,
 * rtl_runtime_suspend_enable).  The RTL8157's U2P3 and power cut as
 * Linux 7.3 drivers/net/usb/r8152.c (r8157_u2p3en, r8157_power_cut_en)
 * sets them.
 */
static int
rtl_chip_init_power(
	struct rtl_adapter *adapter)
{
	int error;

	/* No U2P3 while setting up: the RTL8157 keeps the bit in its IP block. */
	if (adapter->chip == DRV_USB_RTL815X_CHIP_RTL8157)
		error = rtl_indirect_modify(adapter, INDIRECT_IP, IP_U2P3, IP_U2P3_ENABLE, 0);
	else
		error = rtl_modify(adapter, DRV_USB_RTL815X_SPACE_USB, UREG_U2P3, 2, U2P3_ENABLE, 0);
	if (error != 0)
		return error;

	/* The MSC timer at 32760 ms and the U1/U2/L1 idle timer at 500 us. */
	error = rtl_write(adapter, DRV_USB_RTL815X_SPACE_USB, UREG_STORAGE_TIMER, 2, 0x0fffU);
	if (error != 0)
		return error;
	error = rtl_write(adapter, DRV_USB_RTL815X_SPACE_USB, UREG_U1U2_TIMER, 2, 500U);
	if (error != 0)
		return error;

	/* No power cut and no UPS. */
	error = rtl_modify(adapter, DRV_USB_RTL815X_SPACE_USB, UREG_POWER_CUT, 2, POWER_CUT_ENABLE, 0);
	if (error != 0)
		return error;
	error = rtl_modify(adapter, DRV_USB_RTL815X_SPACE_USB, UREG_POWER_CUT_STATUS, 2, MISC_0_PCUT_STATUS, 0);
	if (error != 0)
		return error;
	if (adapter->chip == DRV_USB_RTL815X_CHIP_RTL8157) {
		error = rtl_modify(adapter, DRV_USB_RTL815X_SPACE_USB, UREG_UPS_FLAGS, 1, UPS_FLAGS_POWER_CUT_GEN2, 0);
		if (error != 0)
			return error;
	}

	/* Clears the UPS and early-wake bits of the power cut. */
	error = rtl_modify(adapter, DRV_USB_RTL815X_SPACE_USB, UREG_POWER_CUT, 1, POWER_CUT_UPS | POWER_CUT_PREWAKE, 0);
	if (error != 0)
		return error;
	error = rtl_modify(adapter, DRV_USB_RTL815X_SPACE_USB, UREG_UPS_FLAGS, 1, 0x01U, 0);
	if (error != 0)
		return error;

	/* No wake on queue. */
	error = rtl_modify(adapter, DRV_USB_RTL815X_SPACE_PLA, REG_RUNTIME_FLAGS, 1, INDICATE_RUNTIME_D3, 0);
	if (error != 0)
		return error;
	error = rtl_modify(adapter, DRV_USB_RTL815X_SPACE_PLA, REG_SUSPEND_FLAGS, 1, SUSPEND_LINK_CHANGE, 0);
	if (error != 0)
		return error;
	error = rtl_modify(adapter, DRV_USB_RTL815X_SPACE_PLA, REG_LINK_FLAGS, 2, EXTRA_LINK_CHANGE_FLAG, 0);
	if (error != 0)
		return error;

	/* No runtime suspend: the wake on link off goes, the configuration registers opened around it. */
	error = rtl_write(adapter, DRV_USB_RTL815X_SPACE_PLA, REG_CONFIG_WRITE, 1, CONFIG_WRITE_OPEN);
	if (error != 0)
		return error;
	error = rtl_modify(adapter, DRV_USB_RTL815X_SPACE_PLA, REG_WAKE_CONFIG, 2, CONFIG34_LINK_OFF_WAKE, 0);
	if (error != 0)
		return error;
	error = rtl_write(adapter, DRV_USB_RTL815X_SPACE_PLA, REG_CONFIG_WRITE, 1, CONFIG_WRITE_LOCKED);
	if (error != 0)
		return error;

	/* Succeeded: the power saving is off. */
	return 0;
}

/*
 * Finishes the attach-time setup: the link power states, the RTL8156B's
 * flow control, the link-change flags, the MAC clock, receive
 * aggregation, the RTL8157's own settings and the counters.
 *
 * Procedure referenced: FreeBSD 15.0 sys/dev/usb/net/if_ure.c
 * (ure_rtl8153b_init).  The RTL8157 keeps the MCU's clock setting and
 * clears the 16-byte descriptor bit, not the empty-transfer one (Linux
 * 7.3 drivers/net/usb/r8152.c r8156_init).
 */
static int
rtl_chip_init_finish(
	struct rtl_adapter *adapter)
{
	enum drv_usb_speed speed;
	uint32_t value;
	uint32_t status;
	int error;

	/* U1 and U2 on a SuperSpeed link. */
	speed = drv_usb_device_speed(adapter->usb_device);
	if (speed >= DRV_USB_SPEED_SUPER) {
		error = rtl_modify(adapter, DRV_USB_RTL815X_SPACE_USB, UREG_LINK_POWER, 2, 0, LPM_U1U2_ENABLE);
		if (error != 0)
			return error;
	}

	/* The RTL8156B's flow control: a 600 ms timer and the firmware's patch. */
	if (adapter->chip == DRV_USB_RTL815X_CHIP_RTL8156B) {
		error = rtl_modify(adapter, DRV_USB_RTL815X_SPACE_PLA, REG_RX_CONFIG, 2, 0x0800U, 0);
		if (error != 0)
			return error;
		error = rtl_modify(adapter, DRV_USB_RTL815X_SPACE_PLA, REG_PLUS_COMMAND, 2, 0, 0x0001U);
		if (error != 0)
			return error;
		error = rtl_write(adapter, DRV_USB_RTL815X_SPACE_USB, UREG_FLOW_TIMER, 2, FC_TIMER_ENABLE | (600U / 8U));
		if (error != 0)
			return error;
		error = rtl_read(adapter, DRV_USB_RTL815X_SPACE_PLA, REG_FLOW_OPTION, 1, &value);
		if (error != 0)
			return error;
		if ((value & 0x80U) == 0) {
			error = rtl_modify(adapter, DRV_USB_RTL815X_SPACE_USB, UREG_FIRMWARE_CONTROL, 2, 0x08U, FW_CTRL_FLOW_PATCH | 0x0100U);
			if (error != 0)
				return error;
		}

		/* Turns on the firmware's flow-control patch. */
		error = rtl_modify(adapter, DRV_USB_RTL815X_SPACE_USB, UREG_FIRMWARE_TASK, 2, 0, FW_TASK_FC_PATCH);
		if (error != 0)
			return error;
	}

	/* Tells the chip's firmware the link as it is now, and to watch it. */
	error = rtl_read(adapter, DRV_USB_RTL815X_SPACE_PLA, REG_PHY_STATUS, 2, &status);
	if (error != 0)
		return error;
	error = rtl_read(adapter, DRV_USB_RTL815X_SPACE_PLA, REG_LINK_FLAGS, 2, &value);
	if (error != 0)
		return error;
	if ((status & DRV_USB_RTL815X_PHY_STATUS_LINK) != 0)
		value |= EXTRA_CUR_LINK_OK;
	else
		value &= ~EXTRA_CUR_LINK_OK;
	value |= EXTRA_POLL_LINK_CHANGE;
	error = rtl_write(adapter, DRV_USB_RTL815X_SPACE_PLA, REG_LINK_FLAGS, 2, value);
	if (error != 0)
		return error;

	/* The MAC clock slows down when idle. */
	error = rtl_write(adapter, DRV_USB_RTL815X_SPACE_PLA, REG_MAC_POWER, 2, 0x0403U);
	if (error != 0)
		return error;
	error = rtl_modify(adapter, DRV_USB_RTL815X_SPACE_PLA, REG_MAC_POWER2, 2, 0x00ffU, MAC_PWR_CTRL2_SPEED_DOWN | 0x03U);
	if (error != 0)
		return error;

	/* The RTL8156 family's MCU clock does not; the RTL8157 keeps its own setting. */
	if (adapter->chip != DRV_USB_RTL815X_CHIP_RTL8157) {
		error = rtl_modify(adapter, DRV_USB_RTL815X_SPACE_PLA, REG_MAC_POWER3, 2, MAC_PWR_CTRL3_MCU_SPEED_DOWN, 0);
		if (error != 0)
			return error;
	}

	/* Receive aggregation: many frames to a transfer, in the RTL8157's 16-byte descriptors. */
	if (adapter->chip == DRV_USB_RTL815X_CHIP_RTL8157) {
		error = rtl_modify(adapter,
				   DRV_USB_RTL815X_SPACE_USB,
				   UREG_TRANSFER_CONTROL,
				   2,
				   USB_CTRL_RX_AGG_DISABLE | USB_CTRL_RX_DESC_16,
				   0);
	} else {
		error = rtl_modify(adapter,
				   DRV_USB_RTL815X_SPACE_USB,
				   UREG_TRANSFER_CONTROL,
				   2,
				   USB_CTRL_RX_AGG_DISABLE | USB_CTRL_RX_ZERO,
				   0);
	}

	/* Reports a failed change of the transfer control. */
	if (error != 0)
		return error;

	/* The RTL8157's own: no empty receive transfers, the transmit descriptors' signature, MII forcing. */
	if (adapter->chip == DRV_USB_RTL815X_CHIP_RTL8157) {
		error = rtl_chip_init_finish_8157(adapter);
		if (error != 0)
			return error;
	}

	/* The first RTL8156's DMA setting. */
	if (adapter->chip == DRV_USB_RTL815X_CHIP_RTL8156) {
		error = rtl_modify(adapter, DRV_USB_RTL815X_SPACE_USB, UREG_BUFFER_CONFIG, 1, 0, 0x02U);
		if (error != 0)
			return error;
	}

	/* Clears the counters. */
	error = rtl_modify(adapter, DRV_USB_RTL815X_SPACE_PLA, REG_COUNTER_RESET, 2, 0, COUNTERS_CLEAR);
	if (error != 0)
		return error;

	/* Succeeded: the chip's setup is finished. */
	return 0;
}

/*
 * The RTL8157's last attach-time settings: no empty receive transfers
 * (a buffer manager register), the transmit descriptors' signature
 * checked, and the speed forcing through the MII registers.
 *
 * Values and procedure referenced: Linux 7.3 drivers/net/usb/r8152.c
 * (r8156_init, the RTL_VER_16 branch: the buffer manager's 0x2300 bit
 * 3, 0xd4ae bit 1; r8156_mdio_force_mode: 0xa5b4 bit 15).
 */
static int
rtl_chip_init_finish_8157(
	struct rtl_adapter *adapter)
{
	int error;

	/* No empty receive transfers. */
	error = rtl_indirect_modify(adapter, INDIRECT_BMU, BMU_RX_CONFIG, BMU_RX_ZERO, 0);
	if (error != 0)
		return error;

	/* The transmit descriptors' signature. */
	error = rtl_modify(adapter, DRV_USB_RTL815X_SPACE_USB, UREG_D4AE, 1, 0x02U, 0);
	if (error != 0)
		return error;

	/* Speeds are forced through the MII registers, not the MMD ones. */
	error = rtl_ocp_modify(adapter, PHY_FORCE_MODE, FORCE_MODE_MMD, 0);
	if (error != 0)
		return error;

	/* Succeeded: the RTL8157's settings are made. */
	return 0;
}

/*
 * Sets the RTL8157's PHY up, once at attach after the chip: the PHY
 * powered and on the LAN, its power modes, and its parameter table.
 * The PHY firmware Linux may load is not loaded, and EEE is left as the
 * chip has it.
 *
 * Values and procedure referenced: Linux 7.3 drivers/net/usb/r8152.c
 * (r8157_hw_phy_cfg: USB_MISC_0 PCUT_STATUS, 0xa466 bit 0 and 0xa468
 * bits 1 and 3 in the external initialization, PLA_PHY_PWR
 * PFM_PWM_SWITCH, 0xa430 bits 0 and 1, 0xa44a bit 2).
 */
static int
rtl_phy_config_8157(
	struct rtl_adapter *adapter)
{
	uint16_t phy;
	unsigned state;
	int error;

	/* The PHY settings from flash, and no power cut left reported. */
	error = rtl_wait_flash(adapter);
	if (error != 0)
		return error;
	error = rtl_modify(adapter, DRV_USB_RTL815X_SPACE_USB, UREG_POWER_CUT_STATUS, 2, MISC_0_PCUT_STATUS, 0);
	if (error != 0)
		return error;

	/* A PHY still in its external initialization is let go on. */
	state = 0;
	error = rtl_phy_wait_state(adapter, 0, &state);
	if (error != 0 && error != ETIMEDOUT)
		return error;
	if (state == PHY_STATE_EXT_INIT) {
		error = rtl_ocp_modify(adapter, PHY_A466, 0x0001U, 0);
		if (error != 0)
			return error;
		error = rtl_ocp_modify(adapter, PHY_A468, 0x000aU, 0);
		if (error != 0)
			return error;
	}

	/* The PHY powered up, its power saving off, and on the LAN. */
	error = rtl_ocp_read(adapter, PHY_MII_BASE + MII_BMCR * 2U, &phy);
	if (error != 0)
		return error;
	if ((phy & BMCR_POWER_DOWN) != 0) {
		error = rtl_ocp_write(adapter, PHY_MII_BASE + MII_BMCR * 2U, (uint16_t)(phy & ~BMCR_POWER_DOWN));
		if (error != 0)
			return error;
	}

	/* Turns the PHY's power saving off. */
	error = rtl_aldps(adapter, 0);
	if (error != 0)
		return error;
	error = rtl_phy_wait_state(adapter, PHY_STATE_LAN_ON, &state);
	if (error == ETIMEDOUT)
		kern_logf("usb-rtl815x: PHY state %u is not LAN-on, going on\n", state);
	else if (error != 0)
		return error;

	/* The PFM mode off, the power saving parameters on, the power saving's force mode off. */
	error = rtl_modify(adapter, DRV_USB_RTL815X_SPACE_PLA, REG_PHY_POWER_MODE, 2, PHY_POWER_MODE_PFM, 0);
	if (error != 0)
		return error;
	error = rtl_ocp_modify(adapter, PHY_POWER, 0, PHY_POWER_SAVING_PARAMETERS);
	if (error != 0)
		return error;
	error = rtl_ocp_modify(adapter, PHY_A44A, 0x0004U, 0);
	if (error != 0)
		return error;

	/* The parameter table. */
	error = rtl_phy_steps_apply(adapter, rtl_8157_phy_steps, sizeof(rtl_8157_phy_steps) / sizeof(rtl_8157_phy_steps[0]));
	if (error != 0)
		return error;

	/* The clocks under EEE and the rest. */
	error = rtl_phy_config_8157_finish(adapter);
	if (error != 0)
		return error;

	/* Succeeded: the PHY is set up. */
	return 0;
}

/*
 * Finishes the RTL8157's PHY setup: with the PHY stopped for the change,
 * the MAC's and the PHY's clocks slow down under EEE and the 10 Mbit/s
 * clock is divided; then the low-power mode on, two PHY settings off,
 * and the PHY's power saving back on.  A PHY that never stops for the change
 * keeps its settings, as Linux leaves it.
 *
 * Values and procedure referenced: Linux 7.3 drivers/net/usb/r8152.c
 * (r8157_hw_phy_cfg: PLA_MAC_PWR_CTRL4 EEE_SPDWN_EN, OCP_DOWN_SPEED,
 * OCP_POWER_CFG EEE_CLKDIV_EN, rtl_green_en, 0xa428 bit 9, 0xa5ea bits
 * 0 and 1).
 */
static int
rtl_phy_config_8157_finish(
	struct rtl_adapter *adapter)
{
	int error;

	/* Stops the PHY for the change; one that does not stop keeps its settings. */
	error = rtl_phy_hold_for_change(adapter, 1);
	if (error == ETIMEDOUT) {
		kern_logf("usb-rtl815x: PHY did not stop for its settings, going on\n");
		return 0;
	}

	/* Reports a PHY that failed to stop for reasons other than a timeout. */
	if (error != 0)
		return error;

	/* The clocks under EEE. */
	error = rtl_modify(adapter, DRV_USB_RTL815X_SPACE_PLA, REG_MAC_POWER4, 2, 0, MAC_PWR_CTRL4_EEE_SPEED_DOWN);
	if (error != 0)
		return error;
	error = rtl_ocp_modify(adapter,
			       PHY_CLOCK_SLOWDOWN,
			       SLOWDOWN_EEE_100 | SLOWDOWN_EEE_1000,
			       SLOWDOWN_10M_DIVIDE);
	if (error != 0)
		return error;
	error = rtl_ocp_modify(adapter, PHY_POWER, 0, PHY_POWER_EEE_SLOW_CLOCK);
	if (error != 0)
		return error;

	/* Lets the PHY go on. */
	error = rtl_phy_hold_for_change(adapter, 0);
	if (error == ETIMEDOUT)
		kern_logf("usb-rtl815x: PHY did not resume after its settings, going on\n");
	else if (error != 0)
		return error;

	/* The low-power mode on, and two settings off. */
	error = rtl_sram_modify(adapter, PHY_SRAM_A_WINDOW, PHY_SRAM_A_PORT, SRAM_LOW_POWER, 0, SRAM_LOW_POWER_ON);
	if (error != 0)
		return error;
	error = rtl_ocp_modify(adapter, PHY_A428, 0x0200U, 0);
	if (error != 0)
		return error;
	error = rtl_ocp_modify(adapter, PHY_A5EA, 0x0003U, 0);
	if (error != 0)
		return error;

	/* The PHY's power saving back on. */
	error = rtl_aldps(adapter, 1);
	if (error != 0)
		return error;

	/* Succeeded: the PHY's setup is finished. */
	return 0;
}

/*
 * Reads the adapter's MAC address, or makes a locally administered one
 * when the chip has none.
 *
 * Procedure referenced: FreeBSD 15.0 sys/dev/usb/net/if_ure.c
 * (ure_attach_post: the address in REG_MAC_STORED).
 */
static int
rtl_read_mac(
	struct rtl_adapter *adapter)
{
	uint32_t low;
	uint32_t high;
	unsigned index;
	unsigned zero;
	int error;

	/* Reads the six bytes as a dword and a word. */
	error = rtl_read(adapter, DRV_USB_RTL815X_SPACE_PLA, REG_MAC_STORED, 4, &low);
	if (error != 0)
		return error;
	error = rtl_read(adapter, DRV_USB_RTL815X_SPACE_PLA, REG_MAC_STORED + 4U, 2, &high);
	if (error != 0)
		return error;
	adapter->mac[0] = (uint8_t)low;
	adapter->mac[1] = (uint8_t)(low >> 8);
	adapter->mac[2] = (uint8_t)(low >> 16);
	adapter->mac[3] = (uint8_t)(low >> 24);
	adapter->mac[4] = (uint8_t)high;
	adapter->mac[5] = (uint8_t)(high >> 8);

	/* Counts the zero bytes: an address of all zeros was never set. */
	zero = 0;
	for (index = 0; index < 6U; index++) {
		if (adapter->mac[index] == 0)
			zero++;
	}

	/* A real unicast address is kept. */
	if (zero != 6U && (adapter->mac[0] & 0x01U) == 0)
		return 0;

	/* Otherwise a random, locally administered unicast address. */
	error = kern_random_read(adapter->mac, sizeof(adapter->mac), 0);
	if (error != 0)
		return error;
	adapter->mac[0] = (uint8_t)((adapter->mac[0] & ~0x01U) | 0x02U);
	kern_logf("usb-rtl815x: no MAC address on the chip, using a random one\n");

	/* Succeeded: the adapter has an address. */
	return 0;
}

/*
 * Advertises every speed the chip has and restarts the negotiation, once
 * at attach.
 *
 * Procedure referenced: FreeBSD 15.0 sys/dev/usb/net/if_ure.c
 * (ure_ifmedia_upd, IFM_AUTO).
 */
static int
rtl_advertise(
	struct rtl_adapter *adapter)
{
	uint16_t clear;
	uint16_t set;
	int error;

	/* 10 and 100 Mbit/s both duplexes, with pause. */
	error = rtl_ocp_write(adapter, PHY_MII_BASE + MII_ANAR * 2U, ANAR_ALL);
	if (error != 0)
		return error;

	/* 1000 Mbit/s both duplexes. */
	error = rtl_ocp_write(adapter, PHY_MII_BASE + MII_GTCR * 2U, GTCR_1000);
	if (error != 0)
		return error;

	/* 2500 Mbit/s, and 5000 on the RTL8157. */
	clear = ADV_2500 | ADV_5000;
	set = ADV_2500;
	if (adapter->chip == DRV_USB_RTL815X_CHIP_RTL8157)
		set |= ADV_5000;
	error = rtl_ocp_modify(adapter, PHY_MULTIGIG_ADVERTISE, clear, set);
	if (error != 0)
		return error;

	/* Restarts the negotiation with it. */
	error = rtl_ocp_write(adapter, PHY_MII_BASE + MII_BMCR * 2U, BMCR_AUTONEG_RESTART);
	if (error != 0)
		return error;

	/* Succeeded: the negotiation runs. */
	return 0;
}

/*
 * Resets the MAC for an up: receive off, the reset, the buffer manager
 * reset, and the frame size.
 *
 * Procedure referenced: FreeBSD 15.0 sys/dev/usb/net/if_ure.c
 * (ure_rtl8153b_nic_reset, ure_disable_teredo, ure_reset).
 */
static int
rtl_nic_reset(
	struct rtl_adapter *adapter,
	unsigned mtu)
{
	int error;

	/* The RTL8157 is reset its own way. */
	if (adapter->chip == DRV_USB_RTL815X_CHIP_RTL8157) {
		error = rtl_nic_reset_8157(adapter, mtu);
		if (error != 0)
			return error;
		return 0;
	}

	/* The link power states and the PHY's power saving off. */
	error = rtl_modify(adapter, DRV_USB_RTL815X_SPACE_USB, UREG_LINK_POWER, 2, LPM_U1U2_ENABLE, 0);
	if (error != 0)
		return error;
	error = rtl_modify(adapter, DRV_USB_RTL815X_SPACE_USB, UREG_U2P3, 2, U2P3_ENABLE, 0);
	if (error != 0)
		return error;
	error = rtl_aldps(adapter, 0);
	if (error != 0)
		return error;

	/* Holds receive DMA, turns the wake-up offloads off and accepts nothing. */
	error = rtl_modify(adapter, DRV_USB_RTL815X_SPACE_PLA, REG_RX_DMA_GATE, 2, 0, RX_DMA_HELD);
	if (error != 0)
		return error;
	error = rtl_write(adapter, DRV_USB_RTL815X_SPACE_PLA, REG_WAKE_OFFLOAD, 1, TEREDO_ALL_OFF);
	if (error != 0)
		return error;
	error = rtl_write(adapter, DRV_USB_RTL815X_SPACE_PLA, REG_WAKE_WATCHDOG, 2, WDT6_SET_MODE);
	if (error != 0)
		return error;
	error = rtl_write(adapter, DRV_USB_RTL815X_SPACE_PLA, REG_WAKE_TIMER, 2, 0);
	if (error != 0)
		return error;
	error = rtl_write(adapter, DRV_USB_RTL815X_SPACE_PLA, REG_WAKE_OFFLOAD_TIMER, 4, 0);
	if (error != 0)
		return error;
	error = rtl_modify(adapter, DRV_USB_RTL815X_SPACE_PLA, REG_RX_CONFIG, 4, RX_ACCEPT_ANY, 0);
	if (error != 0)
		return error;

	/* Resets the MAC and waits for the reset to end. */
	error = rtl_write(adapter, DRV_USB_RTL815X_SPACE_PLA, REG_COMMAND, 1, COMMAND_RESET);
	if (error != 0)
		return error;
	error = rtl_wait_bits(adapter, DRV_USB_RTL815X_SPACE_PLA, REG_COMMAND, 1, COMMAND_RESET, 0);
	if (error == ETIMEDOUT)
		kern_logf("usb-rtl815x: MAC reset did not end, going on\n");
	else if (error != 0)
		return error;

	/* Resets the buffer manager of both endpoints. */
	error = rtl_modify(adapter, DRV_USB_RTL815X_SPACE_USB, UREG_BUFFER_RESET, 1, BMU_RESET_IN_OUT, 0);
	if (error != 0)
		return error;
	error = rtl_modify(adapter, DRV_USB_RTL815X_SPACE_USB, UREG_BUFFER_RESET, 1, 0, BMU_RESET_IN_OUT);
	if (error != 0)
		return error;

	/* Leaves the out-of-band mode and the MCU's borrowing. */
	error = rtl_modify(adapter, DRV_USB_RTL815X_SPACE_PLA, REG_OOB_CONTROL, 1, OOB_ACTIVE, 0);
	if (error != 0)
		return error;
	error = rtl_modify(adapter, DRV_USB_RTL815X_SPACE_PLA, REG_MCU_STATUS, 2, MCU_BORROW, 0);
	if (error != 0)
		return error;

	/* Keeps VLAN tags in the frames: the stack reads them there. */
	error = rtl_modify(adapter, DRV_USB_RTL815X_SPACE_PLA, REG_RX_CONFIG_VLAN, 2, RX_VLAN_STRIP, 0);
	if (error != 0)
		return error;

	/* The frame size and the FIFOs. */
	error = rtl_nic_fifo(adapter, mtu);
	if (error != 0)
		return error;

	/* Succeeded: the MAC is reset for the MTU. */
	return 0;
}

/*
 * Sets the frame size, the flow control and the FIFO thresholds, then
 * turns the power saving back on.
 *
 * Procedure referenced: FreeBSD 15.0 sys/dev/usb/net/if_ure.c
 * (ure_rtl8153b_nic_reset, the RTL8156 and RTL8156B branches).
 */
static int
rtl_nic_fifo(
	struct rtl_adapter *adapter,
	unsigned mtu)
{
	enum drv_usb_speed speed;
	uint32_t pause_on;
	uint32_t pause_off;
	int error;

	/* The frame size. */
	error = rtl_program_frame_size(adapter, mtu);
	if (error != 0)
		return error;
	error = rtl_write(adapter, DRV_USB_RTL815X_SPACE_PLA, REG_TX_FRAME_MAX, 1, MTPS_JUMBO);
	if (error != 0)
		return error;

	/* The flow control's pause thresholds, in 16-byte units. */
	pause_on = 0x0400U;
	pause_off = 0x0800U;
	if (adapter->chip == DRV_USB_RTL815X_CHIP_RTL8156B) {
		pause_on = 0x0200U;
		pause_off = 0x0400U;
	}

	/* Writes the pause thresholds. */
	error = rtl_write(adapter, DRV_USB_RTL815X_SPACE_PLA, REG_PAUSE_ON, 2, pause_on);
	if (error != 0)
		return error;
	error = rtl_write(adapter, DRV_USB_RTL815X_SPACE_PLA, REG_PAUSE_OFF, 2, pause_off);
	if (error != 0)
		return error;

	/* The receive FIFO's thresholds. */
	error = rtl_modify(adapter, DRV_USB_RTL815X_SPACE_PLA, REG_RX_FIFO_THRESHOLD, 2, 0x0fffU, 0x08U);
	if (error != 0)
		return error;
	error = rtl_write(adapter, DRV_USB_RTL815X_SPACE_USB, UREG_RX_BUFFER_THRESHOLD, 4, 0x00600400U);
	if (error != 0)
		return error;

	/* The transmit FIFO's thresholds. */
	if (adapter->chip == DRV_USB_RTL815X_CHIP_RTL8156B) {
		error = rtl_write(adapter, DRV_USB_RTL815X_SPACE_PLA, REG_TX_FIFO_THRESHOLD, 2, 0x0008U);
		if (error != 0)
			return error;
	} else {
		error = rtl_write(adapter, DRV_USB_RTL815X_SPACE_PLA, REG_TX_FIFO_THRESHOLD, 2, 0x0008U);
		if (error != 0)
			return error;
		error = rtl_modify(adapter, DRV_USB_RTL815X_SPACE_USB, UREG_BUFFER_CONFIG, 2, 0, 0x0002U);
		if (error != 0)
			return error;
	}

	/* The MCU keeps its clock, and the speed options go. */
	error = rtl_modify(adapter, DRV_USB_RTL815X_SPACE_PLA, REG_MAC_POWER3, 2, MAC_PWR_CTRL3_MCU_SPEED_DOWN, 0);
	if (error != 0)
		return error;
	error = rtl_modify(adapter, DRV_USB_RTL815X_SPACE_USB, UREG_SPEED_OPTIONS, 2, 0x0300U, 0);
	if (error != 0)
		return error;

	/* The power saving back on. */
	error = rtl_aldps(adapter, 1);
	if (error != 0)
		return error;
	error = rtl_modify(adapter, DRV_USB_RTL815X_SPACE_USB, UREG_U2P3, 2, 0, U2P3_ENABLE);
	if (error != 0)
		return error;

	/* U1 and U2 on a SuperSpeed link. */
	speed = drv_usb_device_speed(adapter->usb_device);
	if (speed >= DRV_USB_SPEED_SUPER) {
		error = rtl_modify(adapter, DRV_USB_RTL815X_SPACE_USB, UREG_LINK_POWER, 2, 0, LPM_U1U2_ENABLE);
		if (error != 0)
			return error;
	}

	/* Succeeded: the FIFOs are set. */
	return 0;
}

/*
 * Resets the RTL8157's MAC for an up: receive held and off, transmit and
 * receive stopped (the RTL8157 has no MAC reset here), the buffer
 * manager reset, and the frame size and FIFOs.  Its U2P3 stays off and
 * its U1 and U2 are not turned back on.
 *
 * Values and procedure referenced: Linux 7.3 drivers/net/usb/r8152.c
 * (rtl8156_up and rtl8152_nic_reset, the RTL_VER_16 branches: PLA_RCR1
 * bit 3, PLA_TXFIFO_CTRL 512 / 64, PLA_RXFIFO_FULL, USB_RX_BUF_TH).
 */
static int
rtl_nic_reset_8157(
	struct rtl_adapter *adapter,
	unsigned mtu)
{
	int error;

	/* The link power states and the PHY's power saving off. */
	error = rtl_modify(adapter, DRV_USB_RTL815X_SPACE_USB, UREG_LINK_POWER, 2, LPM_U1U2_ENABLE, 0);
	if (error != 0)
		return error;
	error = rtl_aldps(adapter, 0);
	if (error != 0)
		return error;

	/* Holds receive DMA, turns the wake-up offloads off and accepts nothing. */
	error = rtl_modify(adapter, DRV_USB_RTL815X_SPACE_PLA, REG_RX_DMA_GATE, 2, 0, RX_DMA_HELD);
	if (error != 0)
		return error;
	error = rtl_write(adapter, DRV_USB_RTL815X_SPACE_PLA, REG_WAKE_OFFLOAD, 1, TEREDO_ALL_OFF);
	if (error != 0)
		return error;
	error = rtl_write(adapter, DRV_USB_RTL815X_SPACE_PLA, REG_WAKE_WATCHDOG, 2, WDT6_SET_MODE);
	if (error != 0)
		return error;
	error = rtl_write(adapter, DRV_USB_RTL815X_SPACE_PLA, REG_WAKE_TIMER, 2, 0);
	if (error != 0)
		return error;
	error = rtl_write(adapter, DRV_USB_RTL815X_SPACE_PLA, REG_WAKE_OFFLOAD_TIMER, 4, 0);
	if (error != 0)
		return error;
	error = rtl_modify(adapter, DRV_USB_RTL815X_SPACE_PLA, REG_RX_CONFIG, 4, RX_ACCEPT_ANY, 0);
	if (error != 0)
		return error;

	/* Stops transmit and receive, and resets the buffer manager of both endpoints. */
	error = rtl_modify(adapter, DRV_USB_RTL815X_SPACE_PLA, REG_COMMAND, 1, COMMAND_RECEIVE | COMMAND_TRANSMIT, 0);
	if (error != 0)
		return error;
	error = rtl_modify(adapter, DRV_USB_RTL815X_SPACE_USB, UREG_BUFFER_RESET, 1, BMU_RESET_IN_OUT, 0);
	if (error != 0)
		return error;
	error = rtl_modify(adapter, DRV_USB_RTL815X_SPACE_USB, UREG_BUFFER_RESET, 1, 0, BMU_RESET_IN_OUT);
	if (error != 0)
		return error;

	/* Leaves the out-of-band mode, one receive setting and the MCU's borrowing. */
	error = rtl_modify(adapter, DRV_USB_RTL815X_SPACE_PLA, REG_OOB_CONTROL, 1, OOB_ACTIVE, 0);
	if (error != 0)
		return error;
	error = rtl_modify(adapter, DRV_USB_RTL815X_SPACE_PLA, REG_RX_CONFIG_VLAN, 2, RX_CONFIG_VLAN_GEN2, 0);
	if (error != 0)
		return error;
	error = rtl_modify(adapter, DRV_USB_RTL815X_SPACE_PLA, REG_MCU_STATUS, 2, MCU_BORROW, 0);
	if (error != 0)
		return error;

	/* Keeps VLAN tags in the frames: the stack reads them there. */
	error = rtl_modify(adapter, DRV_USB_RTL815X_SPACE_PLA, REG_RX_CONFIG_VLAN, 2, RX_VLAN_STRIP, 0);
	if (error != 0)
		return error;

	/* The frame size, which also sets the pause thresholds and the transmit FIFO's full mark. */
	error = rtl_program_frame_size(adapter, mtu);
	if (error != 0)
		return error;
	error = rtl_write(adapter, DRV_USB_RTL815X_SPACE_PLA, REG_TX_FRAME_MAX, 1, MTPS_JUMBO);
	if (error != 0)
		return error;

	/* The FIFOs' thresholds, and the MCU keeps its clock. */
	error = rtl_write(adapter, DRV_USB_RTL815X_SPACE_PLA, REG_TX_FIFO_THRESHOLD, 2, 512U / 64U);
	if (error != 0)
		return error;
	error = rtl_modify(adapter, DRV_USB_RTL815X_SPACE_PLA, REG_RX_FIFO_THRESHOLD, 2, 0x0fffU, 0x08U);
	if (error != 0)
		return error;
	error = rtl_modify(adapter, DRV_USB_RTL815X_SPACE_PLA, REG_MAC_POWER3, 2, MAC_PWR_CTRL3_MCU_SPEED_DOWN, 0);
	if (error != 0)
		return error;
	error = rtl_write(adapter, DRV_USB_RTL815X_SPACE_USB, UREG_RX_BUFFER_THRESHOLD, 4, 0x00600400U);
	if (error != 0)
		return error;

	/* The PHY's power saving back on. */
	error = rtl_aldps(adapter, 1);
	if (error != 0)
		return error;

	/* Succeeded: the MAC is reset for the MTU. */
	return 0;
}

/*
 * Writes the sizes that follow the MTU: the largest frame received, the
 * room a receive block keeps for its last frame, and the RTL8156B's and
 * the RTL8157's transmit FIFO thresholds.
 *
 * Values referenced: FreeBSD 15.0 sys/dev/usb/net/if_ure.c (ure_init:
 * the RTL8156's early size; ure_rtl8153b_nic_reset: REG_RX_FRAME_MAX and 0xe61a);
 * Linux 7.3 drivers/net/usb/r8152.c (r8153_set_rx_early_size).
 */
static int
rtl_program_frame_size(
	struct rtl_adapter *adapter,
	unsigned mtu)
{
	uint32_t frame;
	uint32_t reserved;
	uint32_t early;
	int error;

	/* The largest frame: the MTU, the header with a VLAN tag, and the FCS. */
	frame = mtu + RTL_FRAME_OVERHEAD;
	error = rtl_write(adapter, DRV_USB_RTL815X_SPACE_PLA, REG_RX_FRAME_MAX, 2, frame);
	if (error != 0)
		return error;

	/* The block closes while one more frame and its descriptor still fit. */
	reserved = frame + (uint32_t)adapter->format.rx_desc_size + (uint32_t)adapter->format.rx_align;
	early = (uint32_t)adapter->rx_block_size - reserved;
	if (adapter->format.generation == 2)
		early /= 16U;
	else
		early /= 8U;
	error = rtl_write(adapter, DRV_USB_RTL815X_SPACE_USB, UREG_RX_ROOM, 2, early);
	if (error != 0)
		return error;

	/* The RTL8156B's transmit FIFO threshold follows the frame. */
	if (adapter->chip == DRV_USB_RTL815X_CHIP_RTL8156B) {
		error = rtl_write(adapter, DRV_USB_RTL815X_SPACE_PLA, REG_TX_FIFO_FULL, 2, (frame + 0x100U) / 16U);
		if (error != 0)
			return error;
	}

	/* The RTL8157's pause thresholds and transmit FIFO's full mark follow it too. */
	if (adapter->chip == DRV_USB_RTL815X_CHIP_RTL8157) {
		error = rtl_program_frame_fifo_8157(adapter, frame);
		if (error != 0)
			return error;
	}

	/* Succeeded: the sizes follow the MTU. */
	return 0;
}

/*
 * Writes the RTL8157's sizes that follow its largest frame: the pause
 * thresholds, 6 KB and 14 KB above the frame rounded up to 1 KB, and the
 * transmit FIFO's full mark, the frame and its descriptor rounded up to
 * 1 KB; all in 16-byte units.
 *
 * Values referenced: Linux 7.3 drivers/net/usb/r8152.c
 * (fc_pause_on_auto, fc_pause_off_auto, r8156_fc_parameter,
 * rtl8156_change_mtu).
 */
static int
rtl_program_frame_fifo_8157(
	struct rtl_adapter *adapter,
	uint32_t frame)
{
	uint32_t rounded;
	uint32_t transmit;
	int error;

	/* The pause thresholds. */
	rounded = (frame + 1023U) & ~1023U;
	error = rtl_write(adapter, DRV_USB_RTL815X_SPACE_PLA, REG_PAUSE_ON, 2, (rounded + 6U * 1024U) / 16U);
	if (error != 0)
		return error;
	error = rtl_write(adapter, DRV_USB_RTL815X_SPACE_PLA, REG_PAUSE_OFF, 2, (rounded + 14U * 1024U) / 16U);
	if (error != 0)
		return error;

	/* The transmit FIFO's full mark. */
	transmit = (frame + (uint32_t)adapter->format.tx_desc_size + 1023U) & ~1023U;
	error = rtl_write(adapter, DRV_USB_RTL815X_SPACE_PLA, REG_TX_FIFO_FULL, 2, transmit / 16U);
	if (error != 0)
		return error;

	/* Succeeded: the RTL8157's sizes follow the frame. */
	return 0;
}

/*
 * Brings the MAC up for traffic after a reset: its address, the receive
 * aggregation's timers, the packet filter, transmit and receive on.
 *
 * Procedure referenced: FreeBSD 15.0 sys/dev/usb/net/if_ure.c (ure_init,
 * ure_rxfilter); the receive timers' values from Linux 7.3
 * drivers/net/usb/r8152.c (r8153_set_rx_early_timeout: 640 / 8, and a
 * coalesce of 15 us / 8 for the RTL8156 and RTL8157; rtl8156_enable:
 * USB_RX_AGGR_NUM's RX_AGGR_NUM_MASK cleared, which the driver does on
 * the RTL8157 only).
 */
static int
rtl_nic_enable(
	struct rtl_adapter *adapter,
	unsigned mtu)
{
	uint32_t low;
	uint32_t high;
	int error;

	/* The MAC address, written with the configuration registers open. */
	low = (uint32_t)adapter->mac[0] | ((uint32_t)adapter->mac[1] << 8) |
	      ((uint32_t)adapter->mac[2] << 16) | ((uint32_t)adapter->mac[3] << 24);
	high = (uint32_t)adapter->mac[4] | ((uint32_t)adapter->mac[5] << 8);
	error = rtl_write(adapter, DRV_USB_RTL815X_SPACE_PLA, REG_CONFIG_WRITE, 1, CONFIG_WRITE_OPEN);
	if (error != 0)
		return error;
	error = rtl_write(adapter, DRV_USB_RTL815X_SPACE_PLA, REG_MAC_ADDRESS, 4, low);
	if (error != 0)
		return error;
	error = rtl_write(adapter, DRV_USB_RTL815X_SPACE_PLA, REG_MAC_ADDRESS + 4U, 2, high);
	if (error != 0)
		return error;
	error = rtl_write(adapter, DRV_USB_RTL815X_SPACE_PLA, REG_CONFIG_WRITE, 1, CONFIG_WRITE_LOCKED);
	if (error != 0)
		return error;

	/* The RTL8157 closes a block on its timers and room, not on a count of frames. */
	if (adapter->chip == DRV_USB_RTL815X_CHIP_RTL8157) {
		error = rtl_modify(adapter, DRV_USB_RTL815X_SPACE_USB, UREG_RX_AGG_COUNT, 2, RX_AGG_COUNT_MASK, 0);
		if (error != 0)
			return error;
	}

	/* The receive block closes after 640 ns of quiet, or 15 us at most. */
	error = rtl_write(adapter, DRV_USB_RTL815X_SPACE_USB, UREG_RX_QUIET_TIME, 2, 640U / 8U);
	if (error != 0)
		return error;
	error = rtl_write(adapter, DRV_USB_RTL815X_SPACE_USB, UREG_RX_HOLD_TIME, 2, 15000U / 8U);
	if (error != 0)
		return error;

	/* Or when it has no room for another frame. */
	error = rtl_program_frame_size(adapter, mtu);
	if (error != 0)
		return error;
	error = rtl_write(adapter, DRV_USB_RTL815X_SPACE_USB, UREG_RX_DMA_OWNER, 1, RXDMA_OWN_UPDATE_CLEAR);
	if (error != 0)
		return error;

	/* The RTL8156B's flow-control patch restarts. */
	if (adapter->chip == DRV_USB_RTL815X_CHIP_RTL8156B) {
		error = rtl_modify(adapter, DRV_USB_RTL815X_SPACE_USB, UREG_FIRMWARE_TASK, 2, FW_TASK_FC_PATCH, 0);
		if (error != 0)
			return error;
		rtl_sleep_ms(2U);
		error = rtl_modify(adapter, DRV_USB_RTL815X_SPACE_USB, UREG_FIRMWARE_TASK, 2, 0, FW_TASK_FC_PATCH);
		if (error != 0)
			return error;
	}

	/* Resets the packet filter. */
	error = rtl_modify(adapter, DRV_USB_RTL815X_SPACE_PLA, REG_FILTER_CONTROL, 2, FILTER_ENABLE, 0);
	if (error != 0)
		return error;
	error = rtl_modify(adapter, DRV_USB_RTL815X_SPACE_PLA, REG_FILTER_CONTROL, 2, 0, FILTER_ENABLE);
	if (error != 0)
		return error;

	/* Transmit and receive on, receive DMA let go. */
	error = rtl_modify(adapter, DRV_USB_RTL815X_SPACE_PLA, REG_COMMAND, 1, 0, COMMAND_RECEIVE | COMMAND_TRANSMIT);
	if (error != 0)
		return error;
	error = rtl_modify(adapter, DRV_USB_RTL815X_SPACE_PLA, REG_RX_DMA_GATE, 2, RX_DMA_HELD, 0);
	if (error != 0)
		return error;

	/* Accepts frames to this address, broadcasts and every multicast. */
	error = rtl_write(adapter, DRV_USB_RTL815X_SPACE_PLA, REG_MULTICAST_LOW, 4, 0xffffffffU);
	if (error != 0)
		return error;
	error = rtl_write(adapter, DRV_USB_RTL815X_SPACE_PLA, REG_MULTICAST_HIGH, 4, 0xffffffffU);
	if (error != 0)
		return error;
	error = rtl_modify(adapter, DRV_USB_RTL815X_SPACE_PLA, REG_RX_CONFIG, 4, RX_ACCEPT_ANY, RX_ACCEPT_OWN | RX_ACCEPT_BROADCAST | RX_ACCEPT_MULTICAST);
	if (error != 0)
		return error;

	/* Succeeded: the MAC carries traffic. */
	return 0;
}

/* Stops the MAC carrying traffic, at close. */
static int
rtl_nic_disable(
	struct rtl_adapter *adapter)
{
	int error;

	/* Accepts nothing more. */
	error = rtl_modify(adapter, DRV_USB_RTL815X_SPACE_PLA, REG_RX_CONFIG, 4, RX_ACCEPT_ANY, 0);
	if (error != 0)
		return error;

	/* Transmit and receive off. */
	error = rtl_modify(adapter, DRV_USB_RTL815X_SPACE_PLA, REG_COMMAND, 1, COMMAND_RECEIVE | COMMAND_TRANSMIT, 0);
	if (error != 0)
		return error;

	/* Succeeded: the MAC is quiet. */
	return 0;
}

/*
 * Reads the link from the PHY status and publishes it: the carrier, the
 * speed, and the idle clock that a 2.5 Gbit/s link does not take.
 *
 * Procedure referenced: FreeBSD 15.0 sys/dev/usb/net/if_ure.c
 * (ure_link_state).  Called by the worker with the register lock held.
 */
static void
rtl_link_update(
	struct rtl_adapter *adapter)
{
	struct net_device *device;
	const char *duplex;
	unsigned long irq;
	uint32_t status;
	unsigned opened;
	unsigned mbps;
	int carrier;
	int was;
	int error;

	/* The PHY status. */
	error = rtl_read(adapter, DRV_USB_RTL815X_SPACE_PLA, REG_PHY_STATUS, 2, &status);
	if (error != 0)
		return;
	mbps = drv_usb_rtl815x_link_mbps((uint16_t)status);
	carrier = (status & DRV_USB_RTL815X_PHY_STATUS_LINK) != 0;
	adapter->link_up = (unsigned)carrier;

	/* Whether the interface is open: a closed one keeps its MAC quiet. */
	irq = spin_lock_irqsave(&adapter->lock);

	opened = adapter->opened;

	spin_unlock_irqrestore(&adapter->lock, irq);

	/*
	 * A link that came to an open interface: transmit and receive on,
	 * and the RTL8156's idle clock for its speed.  The RTL8157 keeps its
	 * idle clock as it is (Linux 7.3 drivers/net/usb/r8152.c
	 * rtl8156_enable).
	 */
	if (carrier && opened) {
		(void)rtl_modify(adapter, DRV_USB_RTL815X_SPACE_PLA, REG_COMMAND, 1, 0, COMMAND_RECEIVE | COMMAND_TRANSMIT);
		if (adapter->chip == DRV_USB_RTL815X_CHIP_RTL8157) {
			/* The RTL8157's idle clock is left alone. */
		} else if (mbps >= 2500U) {
			/* A 2.5 Gbit/s link does not slow the idle clock. */
			(void)rtl_modify(adapter, DRV_USB_RTL815X_SPACE_PLA, REG_MAC_POWER4, 2, MAC_PWR_CTRL4_IDLE_SPEED_DOWN, 0);
		} else {
			/* A slower one does. */
			(void)rtl_modify(adapter, DRV_USB_RTL815X_SPACE_PLA, REG_MAC_POWER4, 2, 0, MAC_PWR_CTRL4_IDLE_SPEED_DOWN);
		}
	}

	/* Publishes the link, and logs a change. */
	device = adapter->net_device;
	if (device == NULL)
		return;
	was = net_device_carrier(device);
	device->link_mbps = mbps;
	(void)net_device_set_carrier(device, carrier);
	if (was && !carrier) {
		kern_logf("usb-rtl815x: %s link down\n", device->name);
	} else if (!was && carrier) {
		/* A 2.5 Gbit/s link is always full duplex. */
		duplex = "half";
		if ((status & DRV_USB_RTL815X_PHY_STATUS_FULL_DUPLEX) != 0)
			duplex = "full";
		else if (mbps >= 2500U)
			duplex = "full";

		/* Logs the link that came up. */
		kern_logf("usb-rtl815x: %s link up %u Mbit/s %s-duplex\n",
			  device->name,
			  mbps,
			  duplex);
	}
}

/*
 * Runs the adapter's link: takes each link interrupt, reads the link when
 * it changed or was asked for, and puts the interrupt transfer back on.
 */
static void
rtl_worker(
	void *argument)
{
	struct rtl_adapter *adapter;
	unsigned long irq;
	unsigned work;
	unsigned stopping;
	unsigned opened;
	int link_check;
	int error;

	/* Until the detach. */
	adapter = argument;
	for (;;) {
		/* The work asked since the last round. */
		irq = spin_lock_irqsave(&adapter->lock);

		work = adapter->work;
		adapter->work = 0;
		stopping = adapter->worker_stopping;
		opened = adapter->opened;

		spin_unlock_irqrestore(&adapter->lock, irq);

		/* The detach ends the worker. */
		if (stopping)
			return;

		/*
		 * Nothing to do: an open interface without a link reads the
		 * link again each second (should the interrupt not tell it);
		 * otherwise the worker sleeps until asked (a request made just
		 * before is kept).
		 */
		if (work == 0) {
			if (opened && !adapter->link_up) {
				rtl_sleep_ms(RTL_LINK_POLL_MS);
				work = RTL_WORK_LINK;
			} else {
				kern_thread_block();
				continue;
			}
		}

		/* A link interrupt: whether the link changed, and the transfer back on. */
		link_check = (work & RTL_WORK_LINK) != 0;
		if ((work & RTL_WORK_INTERRUPT) != 0)
			rtl_worker_interrupt(adapter, &link_check);

		/* Reads and publishes the link. */
		if (link_check) {
			mutex_lock(&adapter->register_lock);

			rtl_link_update(adapter);

			mutex_unlock(&adapter->register_lock);
		}

		/* Puts the interrupt transfer back on after an interrupt. */
		if ((work & RTL_WORK_INTERRUPT) != 0) {
			error = rtl_start_urb(adapter, adapter->interrupt_urb, adapter->interrupt_buffer,
					      adapter->interrupt_length, 0, 0);
			if (error != 0 && error != ENETDOWN)
				kern_logf("usb-rtl815x: link interrupt not rearmed (%d)\n", error);
		}
	}
}

/*
 * Takes a finished link interrupt: asks for the link to be read when the
 * device tells one other than the last.
 */
static void
rtl_worker_interrupt(
	struct rtl_adapter *adapter,
	int *link_check)
{
	enum drv_usb_urb_status status;
	unsigned link;
	size_t actual;
	int error;

	/* The transfer retires before its bytes are read. */
	error = drv_usb_urb_drain(adapter->interrupt_urb, RTL_TRANSFER_TIMEOUT_MS);
	if (error != 0)
		return;

	/* A failed interrupt tells nothing; the link is read instead. */
	status = drv_usb_urb_status(adapter->interrupt_urb);
	actual = drv_usb_urb_actual_length(adapter->interrupt_urb);
	if (status != DRV_USB_URB_COMPLETE || actual < 2U) {
		if (status != DRV_USB_URB_CANCELLED)
			adapter->interrupt_failures++;
		*link_check = 1;
		return;
	}

	/*
	 * The interrupt's first 16-bit word has the link bit.
	 *
	 * Values referenced: Linux 7.3 drivers/net/usb/r8152.c (INTR_LINK,
	 * INTBUFSIZE).
	 */
	link = 0;
	if ((((unsigned)adapter->interrupt_buffer[0] | ((unsigned)adapter->interrupt_buffer[1] << 8)) &
	     RTL_INTERRUPT_LINK) != 0)
		link = 1;
	if (link != adapter->link_up)
		*link_check = 1;
}

/* Asks the worker for work and wakes it (any context; it does not sleep). */
static void
rtl_request(
	struct rtl_adapter *adapter,
	unsigned work)
{
	struct thread *worker;
	unsigned long irq;

	/* The request, and the worker to wake. */
	irq = spin_lock_irqsave(&adapter->lock);

	adapter->work |= work;
	worker = adapter->worker;

	spin_unlock_irqrestore(&adapter->lock, irq);

	/* A worker not started yet finds the request when it starts. */
	if (worker != NULL)
		kern_thread_wakeup(worker);
}

/* Ends the worker thread and collects it. */
static int
rtl_worker_join(
	struct rtl_adapter *adapter)
{
	struct thread *worker;
	unsigned long irq;
	unsigned state;
	int error;

	/* Asks it to end. */
	irq = spin_lock_irqsave(&adapter->lock);

	adapter->worker_stopping = 1;
	worker = adapter->worker;

	spin_unlock_irqrestore(&adapter->lock, irq);

	/* No worker was started. */
	if (worker == NULL)
		return 0;

	/* The worker cannot wait for itself. */
	if (worker == curthread)
		return EBUSY;

	/* Wakes it; it sees worker_stopping and leaves. */
	kern_thread_wakeup(worker);

	/* Yields until it has ended (a zombie waits for thread_wait). */
	for (;;) {
		state = atomic_raw_load_acquire((volatile unsigned *)&worker->state);
		if (state == THREAD_ZOMBIE)
			break;
		sched_yield();
	}

	/* Its end is collected. */
	error = thread_wait(worker, NULL);
	if (error != 0)
		return error;

	/* Takes the worker out of the adapter once it has ended. */
	irq = spin_lock_irqsave(&adapter->lock);

	adapter->worker = NULL;

	spin_unlock_irqrestore(&adapter->lock, irq);

	/* Succeeded: the worker is gone. */
	return 0;
}

/* Takes one finished transfer (any context). */
static void
rtl_completion(
	struct drv_usb_urb *urb,
	void *argument)
{
	struct rtl_adapter *adapter = argument;
	enum drv_usb_urb_status status;
	unsigned long irq;
	unsigned index;
	int tx_error;
	int poll;

	/* The link interrupt goes to the worker. */
	if (urb == adapter->interrupt_urb) {
		rtl_request(adapter, RTL_WORK_INTERRUPT);
		return;
	}

	/* The transfer's end, read before the lock. */
	status = drv_usb_urb_status(urb);
	tx_error = 0;
	poll = 0;

	/* Marks the transfer that ended for the poll to take. */
	irq = spin_lock_irqsave(&adapter->lock);

	for (index = 0; index < adapter->rx_urb_count; index++) {
		if (urb == adapter->rx_urbs[index]) {
			adapter->rx_done |= 1U << index;
			poll = 1;
		}
	}

	/* Marks the transmit transfers that finished. */
	for (index = 0; index < adapter->tx_urb_count; index++) {
		if (urb != adapter->tx_urbs[index])
			continue;
		adapter->tx_done |= 1U << index;
		poll = 1;
		if (status == DRV_USB_URB_STALL || status == DRV_USB_URB_TIMEOUT ||
		    status == DRV_USB_URB_IO_ERROR)
			tx_error = 1;
	}

	spin_unlock_irqrestore(&adapter->lock, irq);

	/* Counts a transmit transfer that failed. */
	if (tx_error)
		net_device_tx_error(adapter->net_device);

	/* The poll takes the transfer. */
	if (poll)
		net_device_schedule_poll(adapter->net_device);
}

/* Puts one transfer on its endpoint, while the adapter is open. */
static int
rtl_start_urb(
	struct rtl_adapter *adapter,
	struct drv_usb_urb *urb,
	void *buffer,
	size_t length,
	unsigned flags,
	unsigned timeout)
{
	unsigned long irq;
	int error;

	/* Refuses a start for an adapter going down, and counts this one. */
	irq = spin_lock_irqsave(&adapter->lock);

	if (!adapter->ready || !adapter->opened || adapter->closing ||
	    adapter->quarantined) {
		spin_unlock_irqrestore(&adapter->lock, irq);
		return ENETDOWN;
	}

	/* Counts this start as running, so a stop waits for it. */
	adapter->starts_active++;

	spin_unlock_irqrestore(&adapter->lock, irq);

	/* Sets the transfer up and submits it. */
	error = drv_usb_urb_setup(urb, buffer, length, flags, timeout, rtl_completion, adapter);
	if (error == 0)
		error = drv_usb_urb_submit(urb);

	/* The start is over. */
	irq = spin_lock_irqsave(&adapter->lock);

	if (adapter->starts_active == 0)
		__builtin_trap();
	adapter->starts_active--;

	spin_unlock_irqrestore(&adapter->lock, irq);

	/* Reports a transfer that did not go. */
	if (error != 0)
		return error;

	/* Succeeded: the transfer is on the bus. */
	return 0;
}

/* Cancels a transfer and waits for it to leave. */
static int
rtl_cancel_and_drain(
	struct drv_usb_urb *urb)
{
	enum drv_usb_urb_status status;
	int error;

	/* No transfer was made. */
	if (urb == NULL)
		return 0;

	/* Cancels one still on the bus. */
	status = drv_usb_urb_status(urb);
	if (status == DRV_USB_URB_PENDING)
		(void)drv_usb_urb_cancel(urb);

	/* Waits for it to be the driver's again. */
	error = drv_usb_urb_drain(urb, RTL_TRANSFER_TIMEOUT_MS);
	if (error != 0)
		return error;

	/* Succeeded: the transfer is off the bus. */
	return 0;
}

/* Waits for the submits and polls in progress to finish. */
static void
rtl_wait_activity(
	struct rtl_adapter *adapter)
{
	unsigned long irq;
	unsigned active;

	/* Yields until none is left. */
	for (;;) {
		irq = spin_lock_irqsave(&adapter->lock);

		active = adapter->starts_active + adapter->polls_active;

		spin_unlock_irqrestore(&adapter->lock, irq);

		/* Waits until no start or poll is running any more. */
		if (active == 0)
			return;
		sched_yield();
	}
}

/*
 * Stops the adapter's traffic: shuts the gates, waits out the submits and
 * polls, cancels every transfer and drops the frames waiting.  A transfer
 * that does not come back leaves the adapter quarantined.
 */
static int
rtl_stop(
	struct rtl_adapter *adapter)
{
	unsigned long irq;
	unsigned index;
	int candidate;
	int error;

	/* Waits for another stop to finish, then shuts the gates. */
	for (;;) {
		irq = spin_lock_irqsave(&adapter->lock);
		if (!adapter->stopping)
			break;
		spin_unlock_irqrestore(&adapter->lock, irq);
		sched_yield();
	}

	/* Marks the adapter as stopping and closing. */
	adapter->stopping = 1;
	adapter->closing = 1;
	adapter->opened = 0;

	spin_unlock_irqrestore(&adapter->lock, irq);

	/* A poll or a submit admitted before the gates shut finishes first. */
	rtl_wait_activity(adapter);

	/*
	 * The link interrupt, then the receive and transmit transfers.  Where
	 * several are queued on an endpoint, the first cancel ends them all
	 * and the others are only waited for.
	 */
	error = rtl_cancel_and_drain(adapter->interrupt_urb);
	for (index = 0; index < adapter->rx_urb_count; index++) {
		candidate = rtl_cancel_and_drain(adapter->rx_urbs[index]);
		if (error == 0)
			error = candidate;
	}

	/* Cancels and drains every transmit transfer. */
	for (index = 0; index < adapter->tx_urb_count; index++) {
		candidate = rtl_cancel_and_drain(adapter->tx_urbs[index]);
		if (error == 0)
			error = candidate;
	}

	/* Every transfer is off the bus: all receive ones wait for a rearm, all transmit ones are free. */
	irq = spin_lock_irqsave(&adapter->lock);

	if (error == 0) {
		adapter->rx_done = 0;
		adapter->rx_order_head = 0;
		adapter->rx_order_count = 0;
		adapter->rx_idle = (1U << adapter->rx_urb_count) - 1U;
		adapter->tx_done = 0;
		adapter->tx_free = (1U << adapter->tx_urb_count) - 1U;
		adapter->rx_rearm = 0;
		adapter->rx_rearm_active = 0;
		adapter->rx_retries = 0;
		adapter->tx_busy = 0;
	} else {
		adapter->quarantined = 1;
	}

	/* Lets a later open or poll go on. */
	adapter->closing = 0;
	adapter->stopping = 0;

	spin_unlock_irqrestore(&adapter->lock, irq);

	/* Drops the frames still waiting; no transfer refers to them. */
	rtl_tx_queue_free(adapter);

	/* A stop that failed leaves the adapter without a link. */
	if (error != 0 && adapter->net_device != NULL)
		(void)net_device_set_carrier(adapter->net_device, 0);

	/* Reports a transfer that did not come back. */
	if (error != 0)
		return error;

	/* Succeeded: the adapter is quiet. */
	return 0;
}

/* Brings the interface up: the MAC, then the transfers. */
static int
rtl_open(
	struct net_device *device)
{
	struct rtl_adapter *adapter = device->driver_data;
	unsigned long irq;
	unsigned rearm;
	int error;

	/* Admits one open of a ready, closed adapter. */
	irq = spin_lock_irqsave(&adapter->lock);

	if (!adapter->ready || adapter->opened || adapter->closing ||
	    adapter->stopping || adapter->quarantined) {
		error = EBUSY;
		if (!adapter->ready)
			error = ENETDOWN;
		else if (adapter->quarantined)
			error = EIO;
		spin_unlock_irqrestore(&adapter->lock, irq);
		return error;
	}

	/* Marks the adapter as open. */
	adapter->opened = 1;

	spin_unlock_irqrestore(&adapter->lock, irq);

	/* Resets and enables the MAC for the interface's MTU. */
	mutex_lock(&adapter->register_lock);

	error = rtl_nic_reset(adapter, device->mtu);
	if (error != 0)
		kern_logf("usb-rtl815x: %s reset failed (%d)\n", device->name, error);
	if (error == 0) {
		error = rtl_nic_enable(adapter, device->mtu);
		if (error != 0)
			kern_logf("usb-rtl815x: %s enable failed (%d)\n", device->name, error);
	}

	mutex_unlock(&adapter->register_lock);

	/* Starts the link interrupt and every receive transfer. */
	if (error == 0) {
		error = rtl_start_urb(adapter, adapter->interrupt_urb, adapter->interrupt_buffer,
				      adapter->interrupt_length, 0, 0);
	}

	/* Starts the link interrupt and every receive transfer. */
	if (error == 0) {
		/* Holds the right to rearm while every receive transfer goes on. */
		irq = spin_lock_irqsave(&adapter->lock);

		adapter->rx_rearm_active = 1;

		spin_unlock_irqrestore(&adapter->lock, irq);

		/* Starts every idle receive transfer. */
		error = rtl_rx_start_idle(adapter);

		/* Gives the right up; a rearm asked for meanwhile runs in a poll. */
		irq = spin_lock_irqsave(&adapter->lock);

		adapter->rx_rearm_active = 0;
		rearm = adapter->rx_rearm;

		spin_unlock_irqrestore(&adapter->lock, irq);

		/* Schedules a poll for a rearm that was asked for while the right was held. */
		if (error == 0 && rearm)
			net_device_schedule_poll(device);
	}

	/* Undoes an open that did not finish. */
	if (error != 0) {
		kern_logf("usb-rtl815x: %s open failed (%d)\n", device->name, error);
		(void)rtl_stop(adapter);
		return error;
	}

	/* Reads the link as it is now. */
	kern_logf("usb-rtl815x: %s up, mtu %u\n", device->name, device->mtu);
	rtl_request(adapter, RTL_WORK_LINK);

	/* Succeeded: the interface carries traffic once it has a link. */
	return 0;
}

/* Takes the interface down. */
static void
rtl_close(
	struct net_device *device)
{
	struct rtl_adapter *adapter = device->driver_data;

	/* Stops the transfers. */
	(void)rtl_stop(adapter);

	/* Quiets the MAC; a device already gone refuses, which is fine. */
	mutex_lock(&adapter->register_lock);

	(void)rtl_nic_disable(adapter);

	mutex_unlock(&adapter->register_lock);
}

/*
 * Changes the MTU: the device's frame size follows at once when the
 * interface is open, and at the next open otherwise.  The buffers do not
 * depend on it.
 */
static int
rtl_set_mtu(
	struct net_device *device,
	unsigned mtu)
{
	struct rtl_adapter *adapter = device->driver_data;
	unsigned long irq;
	unsigned opened;
	int error;

	/* Refuses a size the device does not take. */
	if (mtu < NET_MTU_MIN || mtu > device->mtu_max)
		return EINVAL;

	/* Whether the device runs now. */
	irq = spin_lock_irqsave(&adapter->lock);

	opened = adapter->opened;

	spin_unlock_irqrestore(&adapter->lock, irq);

	/* A closed interface takes it at the next open. */
	if (!opened)
		return 0;

	/* Writes the sizes that follow the MTU. */
	mutex_lock(&adapter->register_lock);

	error = rtl_program_frame_size(adapter, mtu);

	mutex_unlock(&adapter->register_lock);

	/* Reports a device that refused. */
	if (error != 0)
		return error;

	/* Succeeded: the device takes the new frames. */
	return 0;
}

/*
 * Queues one frame and, when no thread sends and a transmit transfer is
 * free, sends the queue.
 */
static int
rtl_transmit(
	struct net_device *device,
	struct packet_buf *packet)
{
	struct rtl_adapter *adapter = device->driver_data;
	unsigned long irq;
	unsigned limit;
	int pump;
	int ack;

	/* Refuses a missing frame. */
	if (packet == NULL)
		return EINVAL;

	/* Tells a pure acknowledgement, outside the lock. */
	ack = rtl_tx_is_pure_ack(packet);

	/* Refuses a frame for an adapter that is not up. */
	irq = spin_lock_irqsave(&adapter->lock);

	if (!adapter->ready || !adapter->opened || adapter->closing ||
	    adapter->quarantined) {
		spin_unlock_irqrestore(&adapter->lock, irq);
		packet_buf_free(packet);
		return ENETDOWN;
	}

	/*
	 * Only a full queue drops the frame; TCP keeps it and hands it over
	 * again.  A small frame may use the room kept beyond the limit.
	 */
	limit = RTL_TX_QUEUE_MAX;
	if (packet->length <= RTL_TX_SMALL_FRAME)
		limit += RTL_TX_QUEUE_SMALL_EXTRA;
	if (adapter->tx_queue_count >= limit) {
		spin_unlock_irqrestore(&adapter->lock, irq);
		packet_buf_free(packet);
		return ENOBUFS;
	}

	/*
	 * Queues a pure acknowledgement behind the other acknowledgements,
	 * ahead of the data: one waiting behind a full queue of another
	 * connection's data delays what this end receives.  It may pass its
	 * own connection's data, whose older acknowledgement and window the
	 * peer then takes as old (RFC 9293 3.10.7.4).  Any other frame goes
	 * behind those waiting, so a connection's segments go out in order.
	 */
	if (ack) {
		if (adapter->tx_queue_acks_last != NULL) {
			packet->next = adapter->tx_queue_acks_last->next;
			adapter->tx_queue_acks_last->next = packet;
		} else {
			packet->next = adapter->tx_queue_head;
			adapter->tx_queue_head = packet;
		}
		adapter->tx_queue_acks_last = packet;
		if (packet->next == NULL)
			adapter->tx_queue_tail = packet;
	} else {
		packet->next = NULL;
		if (adapter->tx_queue_tail != NULL)
			adapter->tx_queue_tail->next = packet;
		else
			adapter->tx_queue_head = packet;
		adapter->tx_queue_tail = packet;
	}
	adapter->tx_queue_count++;

	/* Becomes the sender when none sends and a transfer is free; starts_active holds off teardown. */
	pump = 0;
	if (!adapter->tx_busy && adapter->tx_free != 0) {
		adapter->tx_busy = 1;
		adapter->starts_active++;
		pump = 1;
	}

	spin_unlock_irqrestore(&adapter->lock, irq);

	/* Sends the queue. */
	if (pump)
		rtl_tx_pump(adapter);

	/* Succeeded: the frame is queued or sent. */
	return 0;
}

/*
 * Tells whether a frame is a pure TCP acknowledgement: IPv4 or IPv6 (no
 * extension header) carrying TCP with no payload and none of SYN, FIN or
 * RST.  Anything it cannot read is not one.
 */
static int
rtl_tx_is_pure_ack(
	const struct packet_buf *packet)
{
	const uint8_t *frame;
	const uint8_t *tcp;
	size_t length;
	size_t ip_header;
	size_t ip_length;
	size_t tcp_header;
	unsigned type;
	unsigned protocol;

	/* Only a small frame can be one. */
	frame = packet->data;
	length = packet->length;
	if (length > RTL_TX_SMALL_FRAME || length < RTL_ETHER_HEADER + 20U + RTL_TCP_HEADER_MIN)
		return 0;

	/* The network header: its length, the transport's and the length it says it carries. */
	type = ((unsigned)frame[12] << 8) | frame[13];
	if (type == RTL_ETHERTYPE_IPV4) {
		if ((frame[RTL_ETHER_HEADER] >> 4) != 4U)
			return 0;
		/* A fragment (more fragments, or an offset) carries no header to read. */
		if ((frame[RTL_ETHER_HEADER + 6U] & 0x3fU) != 0 || frame[RTL_ETHER_HEADER + 7U] != 0)
			return 0;
		ip_header = (size_t)(frame[RTL_ETHER_HEADER] & 0x0fU) * 4U;
		protocol = frame[RTL_ETHER_HEADER + 9U];
		ip_length = ((size_t)frame[RTL_ETHER_HEADER + 2U] << 8) | frame[RTL_ETHER_HEADER + 3U];
	} else if (type == RTL_ETHERTYPE_IPV6) {
		if (length < RTL_ETHER_HEADER + RTL_IPV6_HEADER + RTL_TCP_HEADER_MIN ||
		    (frame[RTL_ETHER_HEADER] >> 4) != 6U)
			return 0;
		ip_header = RTL_IPV6_HEADER;
		protocol = frame[RTL_ETHER_HEADER + 6U];
		ip_length = RTL_IPV6_HEADER +
		    (((size_t)frame[RTL_ETHER_HEADER + 4U] << 8) | frame[RTL_ETHER_HEADER + 5U]);
	} else {
		return 0;
	}

	/* TCP, its whole header within the frame. */
	if (protocol != RTL_IP_PROTOCOL_TCP || ip_header < 20U ||
	    RTL_ETHER_HEADER + ip_header + RTL_TCP_HEADER_MIN > length)
		return 0;
	tcp = frame + RTL_ETHER_HEADER + ip_header;
	tcp_header = (size_t)(tcp[12] >> 4) * 4U;
	if (tcp_header < RTL_TCP_HEADER_MIN || RTL_ETHER_HEADER + ip_header + tcp_header > length)
		return 0;

	/* No payload, and neither opening, closing nor resetting. */
	if (ip_length != ip_header + tcp_header)
		return 0;
	if ((tcp[13] & RTL_TCP_FLAGS_OPEN_CLOSE) != 0)
		return 0;

	/* Succeeded: a pure acknowledgement. */
	return 1;
}

/*
 * Sends the queued frames, a block a free transmit transfer at a time,
 * until the queue is empty, no transfer is free, or the adapter goes down.
 *
 * The caller has set tx_busy and counted itself in starts_active; this
 * gives both up in the section that finds nothing more to send, so a
 * completion that frees a transfer afterwards finds tx_busy clear.
 */
static void
rtl_tx_pump(
	struct rtl_adapter *adapter)
{
	struct packet_buf *packets;
	struct packet_buf *last;
	unsigned long irq;
	unsigned index;
	unsigned taken;
	int error;
	int acks;

	/* Sends block after block. */
	for (;;) {
		irq = spin_lock_irqsave(&adapter->lock);

		/* Stops when going down, with nothing queued, or with no transfer free. */
		if (adapter->closing || !adapter->opened ||
		    adapter->tx_queue_head == NULL || adapter->tx_free == 0) {
			if (adapter->starts_active == 0)
				__builtin_trap();
			adapter->tx_busy = 0;
			adapter->starts_active--;
			spin_unlock_irqrestore(&adapter->lock, irq);
			break;
		}

		/* Takes a free transfer. */
		index = rtl_lowest_bit(adapter->tx_free);
		adapter->tx_free &= ~(1U << index);

		/* Takes the waiting frames one block may carry off the queue as one list. */
		packets = adapter->tx_queue_head;
		last = packets;
		acks = (last == adapter->tx_queue_acks_last);
		for (taken = 1; taken < RTL_TX_BATCH_MAX && last->next != NULL; taken++) {
			last = last->next;
			if (last == adapter->tx_queue_acks_last)
				acks = 1;
		}
		if (acks)
			adapter->tx_queue_acks_last = NULL;
		adapter->tx_queue_head = last->next;
		if (adapter->tx_queue_head == NULL)
			adapter->tx_queue_tail = NULL;
		adapter->tx_queue_count -= taken;
		last->next = NULL;

		spin_unlock_irqrestore(&adapter->lock, irq);

		/* Builds and submits the block; the frames it does not carry go back to the head. */
		error = rtl_tx_submit(adapter, index, packets);
		if (error == 0)
			continue;

		/* A transfer that did not go keeps nothing: it is free again and its frames are dropped. */
		irq = spin_lock_irqsave(&adapter->lock);

		adapter->tx_free |= 1U << index;

		spin_unlock_irqrestore(&adapter->lock, irq);

		/* Counts the frame as dropped on the interface. */
		if (adapter->net_device != NULL)
			adapter->net_device->tx_dropped++;
	}
}

/*
 * Describes one frame for the transmit block, asking the device for the
 * checksums the stack left to it; those the device cannot take are
 * computed here.
 */
static int
rtl_tx_prepare(
	struct rtl_adapter *adapter,
	struct packet_buf *packet,
	struct drv_usb_rtl815x_tx_frame *frame)
{
	const uint8_t *ip;
	size_t l3;
	size_t l4;

	/* The frame's bytes, without a request. */
	frame->data = packet->data;
	frame->length = packet->length;
	frame->checksums = 0;
	frame->l4_offset = 0;

	/* Nothing was left to the device. */
	if ((packet->flags & (PACKET_BUF_TX_CSUM_TCP | PACKET_BUF_TX_CSUM_IPV4)) == 0)
		return 0;

	/* A frame whose headers cannot be found, or a short one the device pads, is finished here. */
	if (packet->l3_offset == PACKET_OFFSET_NONE ||
	    packet->storage + packet->l3_offset < packet->data ||
	    packet->length < 60U) {
		packet_buf_checksum_finish(packet);
		return 0;
	}

	/* The transport header follows the IPv4 header, whose length is in its first byte. */
	l3 = (size_t)(packet->storage + packet->l3_offset - packet->data);
	if (l3 + 20U > packet->length) {
		packet_buf_checksum_finish(packet);
		return 0;
	}

	/* Finds the IP header and the transport header after it. */
	ip = packet->data + l3;
	l4 = l3 + (size_t)(ip[0] & 0x0fU) * 4U;

	/* A transport header further than the descriptor reaches is finished here. */
	if (l4 > adapter->format.l4_offset_max || l4 >= packet->length) {
		packet_buf_checksum_finish(packet);
		return 0;
	}

	/* Asks the device for the checksums. */
	if ((packet->flags & PACKET_BUF_TX_CSUM_TCP) != 0)
		frame->checksums |= DRV_USB_RTL815X_TX_CSUM_TCP;
	if ((packet->flags & PACKET_BUF_TX_CSUM_IPV4) != 0)
		frame->checksums |= DRV_USB_RTL815X_TX_CSUM_IPV4;
	frame->l4_offset = (uint32_t)l4;

	/* Succeeded: the device completes the checksums. */
	return 0;
}

/*
 * Packs frames into one transmit block and submits it on transfer index.
 *
 * packets is a list of frames in order.  The block carries as many as fit;
 * those it does not carry go back to the head of the queue, in order.  The
 * frames carried are consumed, on a failure too.
 */
static int
rtl_tx_submit(
	struct rtl_adapter *adapter,
	unsigned index,
	struct packet_buf *packets)
{
	struct drv_usb_rtl815x_tx_frame frames[RTL_TX_BATCH_MAX];
	struct packet_buf *list[RTL_TX_BATCH_MAX];
	struct packet_buf *rest;
	struct packet_buf *tail;
	unsigned long irq;
	size_t block_length;
	size_t count;
	size_t taken;
	size_t frame;
	unsigned left;
	int error;

	/* Collects the frames in order, each described for the device. */
	count = 0;
	rest = packets;
	while (rest != NULL && count < RTL_TX_BATCH_MAX) {
		list[count] = rest;
		(void)rtl_tx_prepare(adapter, rest, &frames[count]);
		count++;
		rest = rest->next;
	}

	/* Builds a block of as many frames as fit; a failure drops them all. */
	block_length = 0;
	taken = 0;
	error = drv_usb_rtl815x_build_tx(&adapter->format, frames, count,
					 adapter->tx_buffers[index], RTL_TX_BLOCK,
					 &block_length, &taken);
	if (error != 0)
		taken = count;

	/* The frames the block does not carry, with the rest of the list, go back to the head. */
	if (taken < count) {
		list[count - 1U]->next = rest;
		rest = list[taken];
	}

	/* Counts the frames that go back and finds the last of them. */
	left = 0;
	tail = NULL;
	for (packets = rest; packets != NULL; packets = packets->next) {
		tail = packets;
		left++;
	}

	/* Refuses a block for an adapter going down, and puts the frames not carried back. */
	irq = spin_lock_irqsave(&adapter->lock);

	if (error == 0 && (adapter->closing || !adapter->opened))
		error = ENETDOWN;
	if (rest != NULL) {
		tail->next = adapter->tx_queue_head;
		if (adapter->tx_queue_head == NULL)
			adapter->tx_queue_tail = tail;
		adapter->tx_queue_head = rest;
		adapter->tx_queue_count += left;
	}

	spin_unlock_irqrestore(&adapter->lock, irq);

	/* Frees the frames carried (or, on a failure, those that would have been); the core counted them. */
	for (frame = 0; frame < taken; frame++) {
		list[frame]->next = NULL;
		packet_buf_free(list[frame]);
	}

	/* Submits the block; a block of a whole number of packets ends with a zero-length one. */
	if (error == 0) {
		error = drv_usb_urb_setup(adapter->tx_urbs[index], adapter->tx_buffers[index],
					  block_length, DRV_USB_URB_ZERO_PACKET,
					  RTL_TRANSFER_TIMEOUT_MS, rtl_completion, adapter);
	}

	/* Submits the transmit block. */
	if (error == 0)
		error = drv_usb_urb_submit(adapter->tx_urbs[index]);

	/* Reports a block that did not go. */
	if (error != 0)
		return error;

	/* Succeeded: the block is on the bus. */
	return 0;
}

/* Frees every frame still waiting for a transmit transfer. */
static void
rtl_tx_queue_free(
	struct rtl_adapter *adapter)
{
	struct packet_buf *packets;
	struct packet_buf *packet;
	unsigned long irq;

	/* Takes the whole list under the lock. */
	irq = spin_lock_irqsave(&adapter->lock);

	packets = adapter->tx_queue_head;
	adapter->tx_queue_head = NULL;
	adapter->tx_queue_tail = NULL;
	adapter->tx_queue_acks_last = NULL;
	adapter->tx_queue_count = 0;

	spin_unlock_irqrestore(&adapter->lock, irq);

	/* Frees it outside the lock. */
	for (;;) {
		packet = packets;
		if (packet == NULL)
			break;
		packets = packet->next;
		packet_buf_free(packet);
	}
}

/*
 * Copies one received frame into a packet and hands it to the stack, with
 * the checksums the device found right.
 */
static int
rtl_rx_deliver(
	const void *frame,
	size_t length,
	unsigned checksums,
	void *argument)
{
	struct rtl_adapter *adapter = argument;
	struct packet_buf *packet;
	void *destination;
	int error;

	/*
	 * A packet of the class the frame needs (a driver's receive may take
	 * the reserved jumbo ones).  Without one this frame is dropped and the
	 * block's others still delivered.
	 */
	packet = packet_buf_alloc_for(0, length, PACKET_BUF_ALLOC_RECEIVE, &error);
	if (packet == NULL) {
		adapter->net_device->rx_dropped++;
		return 0;
	}

	/* Copies the frame. */
	destination = packet_buf_append(packet, length);
	if (destination == NULL) {
		packet_buf_free(packet);
		adapter->net_device->rx_dropped++;
		return 0;
	}

	/* Copies the frame into the packet. */
	kern_memcpy(destination, frame, length);

	/* Tells the stack the checksums it need not compute. */
	if ((checksums & DRV_USB_RTL815X_RX_CSUM_IPV4) != 0)
		packet->flags |= PACKET_BUF_RX_CSUM_IPV4;
	if ((checksums & DRV_USB_RTL815X_RX_CSUM_L4) != 0)
		packet->flags |= PACKET_BUF_RX_CSUM_L4;

	/* Hands it up. */
	net_device_receive(adapter->net_device, packet);

	/* Succeeded: the frame is delivered. */
	return 0;
}

/*
 * Puts every receive transfer that is off the bus back on, noting it in
 * the submission order first (a completion may come before the submit
 * returns).  The caller holds the right to rearm.  A transfer whose
 * submit fails stays off the bus for the next rearm.
 */
static int
rtl_rx_start_idle(
	struct rtl_adapter *adapter)
{
	unsigned long irq;
	unsigned index;
	unsigned tail;
	int error;

	/* One transfer after another until none is off the bus. */
	error = 0;
	for (;;) {
		irq = spin_lock_irqsave(&adapter->lock);

		if (adapter->rx_idle == 0) {
			spin_unlock_irqrestore(&adapter->lock, irq);
			break;
		}

		/* Takes the lowest idle receive transfer and queues it in completion order. */
		index = rtl_lowest_bit(adapter->rx_idle);
		adapter->rx_idle &= ~(1U << index);
		tail = (adapter->rx_order_head + adapter->rx_order_count) % RTL_RX_URBS;
		adapter->rx_order[tail] = index;
		adapter->rx_order_count++;

		spin_unlock_irqrestore(&adapter->lock, irq);

		/* Submits it. */
		error = rtl_start_urb(adapter, adapter->rx_urbs[index], adapter->rx_urb_buffers[index],
				      adapter->rx_block_size, 0, 0);
		if (error == 0)
			continue;

		/* It did not go: out of the order (it is the last) and off the bus again. */
		irq = spin_lock_irqsave(&adapter->lock);

		adapter->rx_order_count--;
		adapter->rx_idle |= 1U << index;

		spin_unlock_irqrestore(&adapter->lock, irq);
		break;
	}

	/* Reports a transfer that did not go. */
	if (error != 0)
		return error;

	/* Succeeded: every receive transfer is on the bus. */
	return 0;
}

/* Tells whether the oldest receive transfer on the bus has completed, under the lock. */
static int
rtl_rx_head_done_locked(
	const struct rtl_adapter *adapter)
{
	unsigned index;

	/* None is on the bus. */
	if (adapter->rx_order_count == 0)
		return 0;

	/* The oldest one's completion has come. */
	index = adapter->rx_order[adapter->rx_order_head];
	if ((adapter->rx_done & (1U << index)) != 0)
		return 1;

	/* It is still on the bus. */
	return 0;
}

/* Returns the index of the lowest bit set in a nonzero mask. */
static unsigned
rtl_lowest_bit(
	unsigned mask)
{
	unsigned index;

	/* Counts the clear bits below it. */
	for (index = 0; index < 31U; index++) {
		if ((mask & (1U << index)) != 0)
			break;
	}

	/* The bit's index. */
	return index;
}

/* Claims the right to put the receive transfers back on. */
static int
rtl_take_rx_rearm(
	struct rtl_adapter *adapter)
{
	unsigned long irq;
	int taken;

	/* Takes a pending rearm no one else holds. */
	irq = spin_lock_irqsave(&adapter->lock);

	taken = 0;
	if (adapter->rx_rearm != 0 && adapter->rx_rearm_active == 0) {
		adapter->rx_rearm = 0;
		adapter->rx_rearm_active = 1;
		taken = 1;
	}

	spin_unlock_irqrestore(&adapter->lock, irq);

	/* Whether this caller holds the right. */
	return taken;
}

/*
 * Puts the idle receive transfers back on, for the holder of the right.
 * A refusal for want of resources is retried a few times by later polls;
 * past that, or for any other failure, the adapter is quarantined.
 */
static int
rtl_rearm(
	struct rtl_adapter *adapter)
{
	unsigned long irq;
	int retryable;
	int stopping;
	int quarantine;
	int retry;
	int error;

	/* Puts every receive transfer off the bus back on. */
	error = rtl_rx_start_idle(adapter);

	/* Settles the rearm: its retry count, a new request, and the right. */
	irq = spin_lock_irqsave(&adapter->lock);

	retryable = error == EBUSY || error == ENOMEM || error == EAGAIN || error == ENOBUFS;
	stopping = !adapter->ready || !adapter->opened || adapter->closing || adapter->stopping;
	quarantine = 0;
	retry = 0;
	if (adapter->rx_rearm_active == 0)
		__builtin_trap();
	if (error == 0) {
		adapter->rx_retries = 0;
	} else if (stopping) {
		/* A rearm racing a close is an orderly end, not a failure. */
		adapter->rx_retries = 0;
		error = 0;
	} else if (retryable && adapter->rx_retries < RTL_REARM_RETRY_MAX) {
		adapter->rx_retries++;
		adapter->rx_rearm = 1;
		retry = 1;
	} else {
		adapter->rx_rearm = 0;
		adapter->quarantined = 1;
		adapter->opened = 0;
		quarantine = 1;
	}

	/* Gives the right to rearm up. */
	adapter->rx_rearm_active = 0;

	spin_unlock_irqrestore(&adapter->lock, irq);

	/* A quarantined adapter has no link. */
	if (quarantine && adapter->net_device != NULL) {
		kern_logf("usb-rtl815x: %s receive stopped (%d)\n", adapter->net_device->name, error);
		(void)net_device_set_carrier(adapter->net_device, 0);
	}

	/* A retry is asked of a later poll. */
	if (retry)
		return EAGAIN;

	/* Reports a rearm that failed for good. */
	if (error != 0)
		return error;

	/* Succeeded: the receive transfers are on the bus. */
	return 0;
}

/* Admits one poll of a running adapter. */
static int
rtl_poll_enter(
	struct rtl_adapter *adapter)
{
	unsigned long irq;
	int admitted;

	/* Counts the poll in when the adapter runs. */
	irq = spin_lock_irqsave(&adapter->lock);

	admitted = 0;
	if (adapter->ready && adapter->opened && !adapter->closing &&
	    !adapter->stopping && !adapter->quarantined) {
		adapter->polls_active++;
		admitted = 1;
	}

	spin_unlock_irqrestore(&adapter->lock, irq);

	/* Whether the poll may run. */
	return admitted;
}

/* Counts a poll out. */
static void
rtl_poll_exit(
	struct rtl_adapter *adapter)
{
	unsigned long irq;

	/* The poll is over; a stop waiting for it may go on. */
	irq = spin_lock_irqsave(&adapter->lock);

	if (adapter->polls_active == 0)
		__builtin_trap();
	adapter->polls_active--;

	spin_unlock_irqrestore(&adapter->lock, irq);
}

/*
 * Takes the transmit completions: each transfer that ended is free again,
 * and, when no thread sends and frames wait, this poll sends them.
 */
static int
rtl_poll_tx_completion(
	struct rtl_adapter *adapter)
{
	unsigned long irq;
	unsigned done;
	unsigned index;
	int error;
	int pump;

	/* Takes the completions that came. */
	irq = spin_lock_irqsave(&adapter->lock);

	done = adapter->tx_done;
	adapter->tx_done = 0;

	spin_unlock_irqrestore(&adapter->lock, irq);

	/* None came. */
	if (done == 0)
		return 0;

	/* Frees each transfer once its completion has wholly returned; one still returning is taken later. */
	for (index = 0; index < adapter->tx_urb_count; index++) {
		if ((done & (1U << index)) == 0)
			continue;
		error = drv_usb_urb_drain(adapter->tx_urbs[index], RTL_TRANSFER_TIMEOUT_MS);
		irq = spin_lock_irqsave(&adapter->lock);
		if (error == 0)
			adapter->tx_free |= 1U << index;
		else
			adapter->tx_done |= 1U << index;
		spin_unlock_irqrestore(&adapter->lock, irq);
	}

	/* Becomes the sender when none sends, a transfer is free and frames wait. */
	irq = spin_lock_irqsave(&adapter->lock);

	pump = 0;
	if (!adapter->tx_busy && adapter->tx_free != 0 &&
	    adapter->tx_queue_head != NULL && adapter->opened &&
	    !adapter->closing) {
		adapter->tx_busy = 1;
		adapter->starts_active++;
		pump = 1;
	}

	spin_unlock_irqrestore(&adapter->lock, irq);

	/* Sends the waiting frames. */
	if (pump)
		rtl_tx_pump(adapter);

	/* Succeeded: a completion was taken. */
	return 1;
}

/*
 * Takes the oldest receive transfer, when it has completed: it goes back
 * on at once with the spare buffer, so the device fills the next block
 * while this one is parsed and handed up.
 */
static int
rtl_poll_rx_completion(
	struct net_device *device,
	struct rtl_adapter *adapter)
{
	struct drv_usb_urb *urb;
	enum drv_usb_urb_status status;
	unsigned long irq;
	unsigned index;
	uint8_t *block;
	size_t length;
	size_t count;
	int rearm;
	int done;
	int error;

	/* The oldest transfer on the bus, if its completion has come. */
	irq = spin_lock_irqsave(&adapter->lock);

	done = rtl_rx_head_done_locked(adapter);
	index = adapter->rx_order[adapter->rx_order_head];

	spin_unlock_irqrestore(&adapter->lock, irq);

	/* Still on the bus. */
	if (!done)
		return 0;

	/* Waits for its completion to return wholly; a later poll takes one still returning. */
	urb = adapter->rx_urbs[index];
	error = drv_usb_urb_drain(urb, RTL_TRANSFER_TIMEOUT_MS);
	if (error != 0)
		return 0;

	/* Takes it off the bus and out of the order. */
	irq = spin_lock_irqsave(&adapter->lock);

	adapter->rx_done &= ~(1U << index);
	adapter->rx_order_head = (adapter->rx_order_head + 1U) % RTL_RX_URBS;
	adapter->rx_order_count--;

	spin_unlock_irqrestore(&adapter->lock, irq);

	/* The block received, and the spare buffer for the transfer's next. */
	status = drv_usb_urb_status(urb);
	length = drv_usb_urb_actual_length(urb);
	block = adapter->rx_urb_buffers[index];
	adapter->rx_urb_buffers[index] = adapter->rx_spare;
	adapter->rx_spare = NULL;

	/* Asks for the transfer to be armed again, and arms it when no one else holds the right. */
	irq = spin_lock_irqsave(&adapter->lock);

	adapter->rx_idle |= 1U << index;
	adapter->rx_rearm = 1;

	spin_unlock_irqrestore(&adapter->lock, irq);

	/* Rearms the receive transfers when one was asked for. */
	rearm = rtl_take_rx_rearm(adapter);
	if (rearm)
		(void)rtl_rearm(adapter);

	/* Parses the block and hands its frames up; a failed transfer is a receive error. */
	count = 0;
	error = EIO;
	if (status == DRV_USB_URB_COMPLETE)
		error = drv_usb_rtl815x_parse_rx(&adapter->format, block, length, rtl_rx_deliver, adapter, &count);

	/* The parsed block's buffer is the next spare. */
	adapter->rx_spare = block;

	/* Counts a block that could not be wholly taken. */
	if (error != 0 && status != DRV_USB_URB_CANCELLED)
		device->rx_errors++;

	/* Succeeded: a block was taken. */
	return 1;
}

/* Asks whether the poll has anything left to do. */
static int
rtl_has_poll_work(
	struct rtl_adapter *adapter)
{
	unsigned long irq;
	int head_done;
	int pending;

	/* Completions to take or a rearm to make, on a running adapter. */
	irq = spin_lock_irqsave(&adapter->lock);

	pending = 0;
	if (adapter->ready && adapter->opened && !adapter->closing &&
	    !adapter->stopping && !adapter->quarantined) {
		head_done = rtl_rx_head_done_locked(adapter);
		if (head_done || adapter->tx_done != 0)
			pending = 1;
		else if (adapter->rx_rearm && !adapter->rx_rearm_active)
			pending = 1;
	}

	spin_unlock_irqrestore(&adapter->lock, irq);

	/* Whether another poll is wanted. */
	return pending;
}

/* Serves one round of polling for this adapter. */
static unsigned
rtl_poll_receive(
	struct net_device *device,
	unsigned budget)
{
	struct rtl_adapter *adapter = device->driver_data;
	unsigned long irq;
	unsigned cursor;
	unsigned index;
	unsigned kind;
	unsigned work;
	int completed;
	int rx_progress;
	int wanted;

	/* Only a running adapter is polled. */
	completed = rtl_poll_enter(adapter);
	if (!completed)
		return 0;

	/* The kind of completion taken first turns, so neither starves the other. */
	irq = spin_lock_irqsave(&adapter->lock);

	cursor = adapter->poll_cursor % RTL_COMPLETION_KINDS;

	spin_unlock_irqrestore(&adapter->lock, irq);

	/* One completion of each kind. */
	work = 0;
	rx_progress = 0;
	for (index = 0; index < RTL_COMPLETION_KINDS && work < budget; index++) {
		kind = (cursor + index) % RTL_COMPLETION_KINDS;
		if (kind == RTL_COMPLETION_TX)
			completed = rtl_poll_tx_completion(adapter);
		else
			completed = rtl_poll_rx_completion(device, adapter);
		if (!completed)
			continue;
		if (kind == RTL_COMPLETION_RX)
			rx_progress = 1;
		work++;
		irq = spin_lock_irqsave(&adapter->lock);
		adapter->poll_cursor = (kind + 1U) % RTL_COMPLETION_KINDS;
		spin_unlock_irqrestore(&adapter->lock, irq);
	}

	/* The receive blocks that completed behind the first, each handed up whole. */
	while (work < budget) {
		completed = rtl_poll_rx_completion(device, adapter);
		if (!completed)
			break;
		rx_progress = 1;
		work++;
	}

	/*
	 * A rearm a completion asked for belongs to its work; a standalone
	 * retry costs one unit so lasting backpressure stays in the budget.
	 */
	if (rx_progress) {
		wanted = rtl_take_rx_rearm(adapter);
		if (wanted)
			(void)rtl_rearm(adapter);
	} else if (work < budget) {
		wanted = rtl_take_rx_rearm(adapter);
		if (wanted) {
			(void)rtl_rearm(adapter);
			work++;
		}
	}

	/* Asks for another poll when work is left. */
	wanted = rtl_has_poll_work(adapter);
	if (wanted)
		net_device_schedule_poll(device);
	rtl_poll_exit(adapter);

	/* The work done. */
	return work;
}

/* Frees the adapter once the net device's last reference is gone. */
static void
rtl_release(
	void *driver_data)
{
	kern_free(driver_data);
}

/* Marks the adapter ready to carry traffic, or not. */
static void
rtl_set_ready(
	struct rtl_adapter *adapter,
	int ready)
{
	unsigned long irq;

	/* Publishes the state. */
	irq = spin_lock_irqsave(&adapter->lock);

	adapter->ready = ready != 0;

	spin_unlock_irqrestore(&adapter->lock, irq);
}

/* Gives the transfers back. */
static void
rtl_urbs_free(
	struct rtl_adapter *adapter)
{
	unsigned index;

	/* The transmit ones. */
	for (index = 0; index < RTL_TX_URBS; index++) {
		drv_usb_urb_free(adapter->tx_urbs[index]);
		adapter->tx_urbs[index] = NULL;
	}

	/* The receive ones. */
	for (index = 0; index < RTL_RX_URBS; index++) {
		drv_usb_urb_free(adapter->rx_urbs[index]);
		adapter->rx_urbs[index] = NULL;
	}

	/* The link interrupt's. */
	drv_usb_urb_free(adapter->interrupt_urb);
	adapter->interrupt_urb = NULL;
}

/* Makes the transfers, as many each way as the host controller queues. */
static int
rtl_urbs_alloc(
	struct rtl_adapter *adapter)
{
	unsigned index;

	/* The link interrupt's. */
	adapter->interrupt_urb = drv_usb_urb_alloc(adapter->usb_device, adapter->interrupt_endpoint, 0);
	if (adapter->interrupt_urb == NULL) {
		rtl_urbs_free(adapter);
		return ENOMEM;
	}

	/* The receive ones. */
	for (index = 0; index < adapter->rx_urb_count; index++) {
		adapter->rx_urbs[index] = drv_usb_urb_alloc(adapter->usb_device, adapter->bulk_in, 0);
		if (adapter->rx_urbs[index] == NULL) {
			rtl_urbs_free(adapter);
			return ENOMEM;
		}
	}

	/* The transmit ones. */
	for (index = 0; index < adapter->tx_urb_count; index++) {
		adapter->tx_urbs[index] = drv_usb_urb_alloc(adapter->usb_device, adapter->bulk_out, 0);
		if (adapter->tx_urbs[index] == NULL) {
			rtl_urbs_free(adapter);
			return ENOMEM;
		}
	}

	/* All are off the bus. */
	adapter->rx_idle = (1U << adapter->rx_urb_count) - 1U;
	adapter->tx_free = (1U << adapter->tx_urb_count) - 1U;

	/* Succeeded: the transfers exist. */
	return 0;
}

/* Gives the buffers back. */
static void
rtl_buffers_free(
	struct rtl_adapter *adapter)
{
	unsigned index;

	/* The transmit blocks. */
	for (index = 0; index < RTL_TX_URBS; index++) {
		kern_free(adapter->tx_buffers[index]);
		adapter->tx_buffers[index] = NULL;
	}

	/* The receive blocks and the spare. */
	for (index = 0; index < RTL_RX_URBS + 1U; index++) {
		kern_free(adapter->rx_buffers[index]);
		adapter->rx_buffers[index] = NULL;
	}

	/* Forgets the per-transfer receive buffer pointers and the spare. */
	for (index = 0; index < RTL_RX_URBS; index++)
		adapter->rx_urb_buffers[index] = NULL;
	adapter->rx_spare = NULL;

	/* The link interrupt's. */
	kern_free(adapter->interrupt_buffer);
	adapter->interrupt_buffer = NULL;
}

/* Makes the buffers: a block for each transfer, and a spare receive block. */
static int
rtl_buffers_alloc(
	struct rtl_adapter *adapter)
{
	unsigned index;

	/* The link interrupt's. */
	adapter->interrupt_buffer = kern_malloc(adapter->interrupt_length);
	if (adapter->interrupt_buffer == NULL) {
		rtl_buffers_free(adapter);
		return ENOMEM;
	}

	/* The receive blocks and the spare. */
	for (index = 0; index < adapter->rx_urb_count + 1U; index++) {
		adapter->rx_buffers[index] = kern_malloc(adapter->rx_block_size);
		if (adapter->rx_buffers[index] == NULL) {
			rtl_buffers_free(adapter);
			return ENOMEM;
		}
	}

	/* The transmit blocks. */
	for (index = 0; index < adapter->tx_urb_count; index++) {
		adapter->tx_buffers[index] = kern_malloc(RTL_TX_BLOCK);
		if (adapter->tx_buffers[index] == NULL) {
			rtl_buffers_free(adapter);
			return ENOMEM;
		}
	}

	/* Each receive transfer starts with its own block; the last is the spare. */
	for (index = 0; index < adapter->rx_urb_count; index++)
		adapter->rx_urb_buffers[index] = adapter->rx_buffers[index];
	adapter->rx_spare = adapter->rx_buffers[adapter->rx_urb_count];

	/* Succeeded: the buffers exist. */
	return 0;
}

/* Publishes the adapter as a network interface, named like the other USB ones. */
static int
rtl_net_device_create(
	struct rtl_adapter *adapter)
{
	struct net_device *device;
	unsigned index;
	int error;

	/* A net device of the adapter's address, MTU and checksum offload. */
	device = net_device_alloc();
	if (device == NULL)
		return ENOSPC;
	device->flags = NET_DEVICE_BROADCAST | NET_DEVICE_MULTICAST;
	device->mtu = RTL_MTU_DEFAULT;
	device->mtu_max = NET_MTU_MAX;
	kern_memcpy(device->hwaddr, adapter->mac, 6U);
	device->hwaddr_len = 6U;
	device->capabilities = NET_DEVICE_CAP_TX_CSUM_IPV4_TCP;
	device->ops = &rtl_net_ops;
	device->driver_data = adapter;

	/* The first free name of ue0 to ue7. */
	error = ENOSPC;
	for (index = 0; index < NET_DEVICE_MAX; index++) {
		device->name[0] = 'u';
		device->name[1] = 'e';
		device->name[2] = (char)('0' + index);
		device->name[3] = '\0';
		error = net_device_create(device);
		if (error != EEXIST)
			break;
	}

	/* An unpublished device gives the adapter back to the attach's owner. */
	if (error != 0) {
		device->driver_data = NULL;
		net_device_destroy(device);
		return error;
	}

	/* Publishes the interface to the adapter. */
	adapter->net_device = device;

	/* Succeeded: the interface exists. */
	return 0;
}

/*
 * Binds the driver to a matched vendor interface: tells the chip, sets it
 * up, and publishes the interface.
 */
static int
rtl_attach(
	struct drv_usb_interface *interface,
	const struct drv_usb_id *id)
{
	struct drv_usb_endpoint *bulk_in;
	struct drv_usb_endpoint *bulk_out;
	struct drv_usb_endpoint *interrupt_endpoint;
	struct rtl_adapter *adapter;
	const char *chip_name;
	size_t packet_size;
	int error;

	UNUSED_PARAMETER(id);

	/* The interface's endpoints. */
	error = rtl_find_endpoints(interface, &bulk_in, &bulk_out, &interrupt_endpoint);
	if (error != 0)
		return error;

	/* The adapter's state. */
	adapter = kern_malloc(sizeof(*adapter));
	if (adapter == NULL)
		return ENOMEM;
	kern_memset(adapter, 0, sizeof(*adapter));
	adapter->usb_device = drv_usb_interface_device(interface);
	adapter->interface = interface;
	adapter->bulk_in = bulk_in;
	adapter->bulk_out = bulk_out;
	adapter->interrupt_endpoint = interrupt_endpoint;
	packet_size = drv_usb_endpoint_max_packet_size(interrupt_endpoint);
	adapter->interrupt_length = packet_size;
	if (packet_size < 2U)
		adapter->interrupt_length = 2U;
	spin_init(&adapter->lock, LOCK_RANK_DEVICE, "usb-rtl815x");
	(void)mutex_init(&adapter->register_lock, LOCK_RANK_DEVICE, "usb-rtl815x registers");

	/* The USB core owns failed-attach cleanup through the binding; the adapter is published first. */
	error = drv_usb_interface_set_driver_data(interface, adapter);
	if (error != 0) {
		kern_free(adapter);
		return error;
	}

	/* Tells the chip and refuses one this driver does not drive. */
	error = rtl_read_version(adapter);
	if (error != 0) {
		kern_logf("usb-rtl815x: attach failed at %s (%d)\n", "version", error);
		return error;
	}

	/* Logs the chip that is being attached. */
	chip_name = drv_usb_rtl815x_chip_name(adapter->chip);
	kern_logf("usb-rtl815x: attaching %s version 0x%04x\n", chip_name, adapter->version);
	error = drv_usb_rtl815x_format_for(adapter->chip, &adapter->format);
	if (error != 0) {
		kern_logf("usb-rtl815x: chip %s (version 0x%04x) is not driven in vendor mode; "
			  "boot with %s to use CDC NCM\n",
			  drv_usb_rtl815x_chip_name(adapter->chip), adapter->version, RTL_OFF_TOKEN);
		return error;
	}

	/* The receive block of the chip. */
	if (adapter->chip == DRV_USB_RTL815X_CHIP_RTL8157)
		adapter->rx_block_size = RTL_RX_BLOCK_8157;
	else
		adapter->rx_block_size = RTL_RX_BLOCK_8156;

	/* Sets the chip up, reads its address and starts the negotiation. */
	error = rtl_chip_init(adapter);
	if (error != 0) {
		kern_logf("usb-rtl815x: chip setup failed (%d)\n", error);
		return error;
	}

	/* Sets up the RTL8157's PHY. */
	if (adapter->chip == DRV_USB_RTL815X_CHIP_RTL8157) {
		error = rtl_phy_config_8157(adapter);
		if (error != 0) {
			kern_logf("usb-rtl815x: attach failed at %s (%d)\n", "PHY setup", error);
			return error;
		}
	}

	/* Reads the MAC address. */
	error = rtl_read_mac(adapter);
	if (error != 0) {
		kern_logf("usb-rtl815x: attach failed at %s (%d)\n", "mac", error);
		return error;
	}

	/* Sets the advertised link modes. */
	error = rtl_advertise(adapter);
	if (error != 0) {
		kern_logf("usb-rtl815x: attach failed at %s (%d)\n", "advertise", error);
		return error;
	}

	/* Asks the host controller to keep several bulk transfers on the bus each way. */
	error = drv_usb_endpoint_set_queue_depth(bulk_in, RTL_RX_URBS, &adapter->rx_urb_count);
	if (error != 0)
		adapter->rx_urb_count = 1;
	error = drv_usb_endpoint_set_queue_depth(bulk_out, RTL_TX_URBS, &adapter->tx_urb_count);
	if (error != 0)
		adapter->tx_urb_count = 1;

	/* The buffers, the transfers, and the interface. */
	error = rtl_buffers_alloc(adapter);
	if (error != 0) {
		kern_logf("usb-rtl815x: attach failed at %s (%d)\n", "buffers", error);
		return error;
	}

	/* Allocates the transfers. */
	error = rtl_urbs_alloc(adapter);
	if (error != 0) {
		kern_logf("usb-rtl815x: attach failed at %s (%d)\n", "transfers", error);
		return error;
	}

	/* Creates the network interface. */
	error = rtl_net_device_create(adapter);
	if (error != 0) {
		kern_logf("usb-rtl815x: attach failed at %s (%d)\n", "interface", error);
		return error;
	}

	/* The worker that follows the link. */
	error = kthread_create(rtl_worker, adapter, SCHED_PRIORITY_DEFAULT, &adapter->worker);
	if (error != 0) {
		kern_logf("usb-rtl815x: attach failed at %s (%d)\n", "worker", error);
		return error;
	}

	/* Starts the worker. */
	thread_start(adapter->worker);

	/* Ready for traffic. */
	rtl_set_ready(adapter, 1);
	kern_logf("usb-rtl815x: %s chip=%s version=0x%04x mac=%02x:%02x:%02x:%02x:%02x:%02x "
		  "mtu_max=%u urbs=%u/%u rx_block=%u\n",
		  adapter->net_device->name, drv_usb_rtl815x_chip_name(adapter->chip), adapter->version,
		  adapter->mac[0], adapter->mac[1], adapter->mac[2], adapter->mac[3], adapter->mac[4],
		  adapter->mac[5], adapter->net_device->mtu_max, adapter->rx_urb_count,
		  adapter->tx_urb_count, (unsigned)adapter->rx_block_size);

	/* Succeeded: the interface is published. */
	return 0;
}

/* Gives the interface up and everything held for it. */
static int
rtl_detach(
	struct drv_usb_interface *interface,
	unsigned flags)
{
	struct rtl_adapter *adapter;
	int error;

	UNUSED_PARAMETER(flags);

	/* No adapter was made. */
	adapter = drv_usb_interface_driver_data(interface);
	if (adapter == NULL)
		return 0;

	/* Shuts the adapter and stops its transfers. */
	rtl_set_ready(adapter, 0);
	error = rtl_stop(adapter);
	if (error != 0)
		return error;

	/* Ends the worker. */
	error = rtl_worker_join(adapter);
	if (error != 0)
		return error;

	/* Withdraws the interface. */
	if (adapter->net_device != NULL) {
		error = net_device_gone(adapter->net_device);
		if (error != 0)
			return error;
	}

	/* Frees the transfers and buffers, and the adapter with the interface. */
	rtl_urbs_free(adapter);
	rtl_buffers_free(adapter);
	if (adapter->net_device != NULL)
		net_device_destroy(adapter->net_device);
	else
		kern_free(adapter);

	/* Succeeded: the adapter is gone. */
	return 0;
}

/* Takes the adapter out of service at system shutdown. */
static void
rtl_shutdown(
	struct drv_usb_interface *interface)
{
	struct rtl_adapter *adapter;

	/* Stops the adapter there is. */
	adapter = drv_usb_interface_driver_data(interface);
	if (adapter != NULL) {
		rtl_set_ready(adapter, 0);
		(void)rtl_stop(adapter);
	}
}
