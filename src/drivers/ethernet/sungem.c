/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The Sun GEM family of Ethernet controllers as a network interface: the
 * GMAC built into Apple's UniNorth, Pangea, Intrepid, K2 and Shasta
 * system controllers, and Sun's own GEM.
 *
 * The controller's registers and its descriptors are little-endian; a
 * big-endian processor swaps every word it reads or writes.  A
 * descriptor is two 64-bit words, the flags and the buffer's address.
 * Both rings and every packet slot live in memory the driver owns for as
 * long as the kernel runs, so the controller never reaches freed memory
 * across close and reopen.  A receive descriptor points at its slot for
 * good; a frame is copied out of the slot into the network packet pool
 * and the descriptor is handed back.  A transmitted frame is copied into
 * its slot.  The controller takes descriptors up to the index written to
 * its kick registers and reports how far it got in its completion
 * registers.  No checksum offload is used.
 *
 * The PHY sits on the controller's management interface (MIF), driven
 * one frame at a time; an Apple board wires it to the first MDIO line
 * and resets it through a line of its own, which the board's hook
 * pulses.  Without an interrupt the periodic service alone moves frames.
 */

#include <drivers/ethernet/sungem.h>
#include <drivers/ethernet/mii-phy.h>
#include <drivers/generic/dma.h>
#include <drivers/pci/pci.h>
#include <kern/clock.h>
#include <kern/device-io.h>
#include <kern/kcrt.h>
#include <kern/klog.h>
#include <kern/lock.h>
#include <kern/net/net-device.h>
#include <kern/net/packet-buf.h>
#include <kern/sched.h>
#include <kern/thread.h>
#include <uapi/errno.h>

/*
 * The global registers and their bits.
 *
 * Values referenced: FreeBSD sys/dev/gem/if_gemreg.h, release/14.3.0
 * (GEM_CONFIG, GEM_STATUS, GEM_INTMASK, GEM_PCI_ERROR_STATUS, the
 * GEM_CONFIG_* and GEM_INTR_* bits, GEM_RESET_TX and GEM_RESET_RX), and
 * release/12.4.0 of the same file for where the reset register is on a
 * PCI controller (GEM_PCI_BANK2_OFFSET plus GEM_RESET); the Linux
 * kernel's drivers/net/ethernet/sun/sungem.h (v7.2: GREG_SWRST) gives the
 * same place.
 */
#define GEM_CONFIG			0x0004U
#define GEM_CONFIG_BURST_INF		0x00000001U
#define GEM_CONFIG_TXDMA_LIMIT		0x0000003eU
#define GEM_CONFIG_RXDMA_LIMIT		0x000007c0U
#define GEM_CONFIG_RONPAULBIT		0x00000800U
#define GEM_CONFIG_BUG2FIX		0x00001000U
#define GEM_STATUS			0x000cU
#define GEM_INTMASK			0x0010U
#define GEM_INTR_TX_INTME		0x00000001U
#define GEM_INTR_TX_EMPTY		0x00000002U
#define GEM_INTR_RX_DONE		0x00000010U
#define GEM_INTR_RX_NOBUF		0x00000020U
#define GEM_INTR_RX_TAG_ERR		0x00000040U
#define GEM_INTR_PERR			0x00000080U
#define GEM_INTR_TX_MAC			0x00004000U
#define GEM_INTR_RX_MAC			0x00008000U
#define GEM_INTR_BERR			0x00040000U
#define GEM_PCI_ERROR_STATUS		0x1000U
#define GEM_RESET			0x1010U
#define GEM_RESET_TX			0x00000001U
#define GEM_RESET_RX			0x00000002U

/*
 * The transmit and receive DMA engines' registers and their bits.
 *
 * Values referenced: FreeBSD sys/dev/gem/if_gemreg.h, release/14.3.0
 * (GEM_TX_KICK, GEM_TX_CONFIG, GEM_TX_RING_PTR_LO, GEM_TX_RING_PTR_HI,
 * GEM_TX_COMPLETION, GEM_RX_CONFIG, GEM_RX_RING_PTR_LO,
 * GEM_RX_RING_PTR_HI, GEM_RX_PAUSE_THRESH, GEM_RX_KICK,
 * GEM_RX_COMPLETION, GEM_RX_BLANKING, GEM_RX_FIFO_SIZE and their bits,
 * GEM_RING_SZ_128, GEM_THRSH_1024).
 */
#define GEM_TX_KICK			0x2000U
#define GEM_TX_CONFIG			0x2004U
#define GEM_TX_CONFIG_TXDMA_EN		0x00000001U
#define GEM_TX_CONFIG_TXFIFO_TH		0x001ffc00U
#define GEM_TX_FIFO_THRESHOLD		(0x4ffU << 10)
#define GEM_TX_RING_PTR_LO		0x2008U
#define GEM_TX_RING_PTR_HI		0x200cU
#define GEM_TX_COMPLETION		0x2100U
#define GEM_TX_COMPLETION_MASK		0x00001fffU
#define GEM_RX_CONFIG			0x4000U
#define GEM_RX_CONFIG_RXDMA_EN		0x00000001U
#define GEM_RX_CONFIG_CXM_START_SHIFT	13U
#define GEM_RX_CONFIG_FIFO_THRS_SHIFT	24U
#define GEM_RX_RING_PTR_LO		0x4004U
#define GEM_RX_RING_PTR_HI		0x4008U
#define GEM_RX_PAUSE_THRESH		0x4020U
#define GEM_RX_KICK			0x4100U
#define GEM_RX_COMPLETION		0x4104U
#define GEM_RX_BLANKING			0x4108U
#define GEM_RX_BLANKING_TIME_SHIFT	12U
#define GEM_RX_FIFO_SIZE		0x4120U
#define GEM_RING_SIZE_128		(2U << 1)
#define GEM_THRESHOLD_1024		4U

/*
 * The MAC's registers and their bits.
 *
 * Values referenced: FreeBSD sys/dev/gem/if_gemreg.h, release/14.3.0
 * (GEM_MAC_TXRESET, GEM_MAC_RXRESET, GEM_MAC_SEND_PAUSE_CMD,
 * GEM_MAC_TX_STATUS, GEM_MAC_RX_STATUS, GEM_MAC_TX_MASK, GEM_MAC_RX_MASK,
 * GEM_MAC_CONTROL_MASK, GEM_MAC_TX_CONFIG, GEM_MAC_RX_CONFIG,
 * GEM_MAC_CONTROL_CONFIG, GEM_MAC_XIF_CONFIG, GEM_MAC_IPG0 to 2,
 * GEM_MAC_SLOT_TIME, GEM_MAC_MAC_MIN_FRAME, GEM_MAC_MAC_MAX_FRAME,
 * GEM_MAC_PREAMBLE_LEN, GEM_MAC_JAM_SIZE, GEM_MAC_ATTEMPT_LIMIT,
 * GEM_MAC_CONTROL_TYPE, GEM_MAC_ADDR0 to 8, GEM_MAC_ADDR_FILTER0 to 2,
 * GEM_MAC_ADR_FLT_MASK1_2, GEM_MAC_ADR_FLT_MASK0, GEM_MAC_HASH0, the
 * counters from GEM_MAC_NORM_COLL_CNT to GEM_MAC_RX_CODE_VIOL,
 * GEM_MAC_RANDOM_SEED, and the bits of each).
 */
#define GEM_MAC_TXRESET			0x6000U
#define GEM_MAC_RXRESET			0x6004U
#define GEM_MAC_SEND_PAUSE_CMD		0x6008U
#define GEM_MAC_TX_STATUS		0x6010U
#define GEM_MAC_TX_UNDERRUN		0x00000002U
#define GEM_MAC_TX_PKT_TOO_LONG		0x00000004U
#define GEM_MAC_RX_STATUS		0x6014U
#define GEM_MAC_RX_OVERFLOW		0x00000002U
#define GEM_MAC_TX_MASK			0x6020U
#define GEM_MAC_TX_XMIT_DONE		0x00000001U
#define GEM_MAC_TX_DEFER_EXP		0x00000080U
#define GEM_MAC_TX_PEAK_EXP		0x00000100U
#define GEM_MAC_RX_MASK			0x6024U
#define GEM_MAC_RX_DONE			0x00000001U
#define GEM_MAC_RX_FRAME_CNT		0x00000004U
#define GEM_MAC_CONTROL_MASK		0x6028U
#define GEM_MAC_PAUSED			0x00000001U
#define GEM_MAC_PAUSE			0x00000002U
#define GEM_MAC_RESUME			0x00000004U
#define GEM_MAC_TX_CONFIG		0x6030U
#define GEM_MAC_TX_ENABLE		0x00000001U
#define GEM_MAC_TX_IGN_CARRIER		0x00000002U
#define GEM_MAC_TX_IGN_COLLIS		0x00000004U
#define GEM_MAC_TX_ENA_IPG0		0x00000008U
#define GEM_MAC_TX_NGU			0x00000010U
#define GEM_MAC_TX_NGU_LIMIT		0x00000020U
#define GEM_MAC_TX_CARR_EXTEND		0x00000200U
#define GEM_MAC_RX_CONFIG		0x6034U
#define GEM_MAC_RX_ENABLE		0x00000001U
#define GEM_MAC_RX_STRIP_CRC		0x00000004U
#define GEM_MAC_RX_PROMISCUOUS		0x00000008U
#define GEM_MAC_RX_PROMISC_GRP		0x00000010U
#define GEM_MAC_RX_HASH_FILTER		0x00000020U
#define GEM_MAC_RX_ADDR_FILTER		0x00000040U
#define GEM_MAC_RX_CARR_EXTEND		0x00000100U
#define GEM_MAC_CONTROL_CONFIG		0x6038U
#define GEM_MAC_CC_TX_PAUSE		0x00000001U
#define GEM_MAC_CC_RX_PAUSE		0x00000002U
#define GEM_MAC_XIF_CONFIG		0x603cU
#define GEM_MAC_XIF_TX_MII_ENA		0x00000001U
#define GEM_MAC_XIF_ECHO_DISABL		0x00000004U
#define GEM_MAC_XIF_GMII_MODE		0x00000008U
#define GEM_MAC_XIF_MII_BUF_ENA		0x00000010U
#define GEM_MAC_XIF_LINK_LED		0x00000020U
#define GEM_MAC_XIF_FDPLX_LED		0x00000040U
#define GEM_MAC_IPG0			0x6040U
#define GEM_MAC_IPG1			0x6044U
#define GEM_MAC_IPG2			0x6048U
#define GEM_MAC_SLOT_TIME		0x604cU
#define GEM_MAC_SLOT_TIME_CARR_EXTEND	0x200U
#define GEM_MAC_SLOT_TIME_NORMAL	0x40U
#define GEM_MAC_MAC_MIN_FRAME		0x6050U
#define GEM_MAC_MAC_MAX_FRAME		0x6054U
#define GEM_MAC_PREAMBLE_LEN		0x6058U
#define GEM_MAC_JAM_SIZE		0x605cU
#define GEM_MAC_ATTEMPT_LIMIT		0x6060U
#define GEM_MAC_CONTROL_TYPE		0x6064U
#define GEM_MAC_ADDR0			0x6080U
#define GEM_MAC_ADDR1			0x6084U
#define GEM_MAC_ADDR2			0x6088U
#define GEM_MAC_ADDR3			0x608cU
#define GEM_MAC_ADDR4			0x6090U
#define GEM_MAC_ADDR5			0x6094U
#define GEM_MAC_ADDR6			0x6098U
#define GEM_MAC_ADDR7			0x609cU
#define GEM_MAC_ADDR8			0x60a0U
#define GEM_MAC_ADDR_FILTER0		0x60a4U
#define GEM_MAC_ADDR_FILTER1		0x60a8U
#define GEM_MAC_ADDR_FILTER2		0x60acU
#define GEM_MAC_ADR_FLT_MASK1_2		0x60b0U
#define GEM_MAC_ADR_FLT_MASK0		0x60b4U
#define GEM_MAC_HASH0			0x60c0U
#define GEM_MAC_HASH_COUNT		16U
#define GEM_MAC_NORM_COLL_CNT		0x6100U
#define GEM_MAC_RX_CODE_VIOL		0x6128U
#define GEM_MAC_RANDOM_SEED		0x6130U

/*
 * The management interface's registers and bits, and the datapath mode
 * register of the PCS block.
 *
 * Values referenced: FreeBSD sys/dev/gem/if_gemreg.h, release/14.3.0
 * (GEM_MIF_FRAME, GEM_MIF_CONFIG, GEM_MIF_FRAME_DATA, GEM_MIF_FRAME_TA0,
 * GEM_MIF_FRAME_READ, GEM_MIF_FRAME_WRITE, GEM_MIF_REG_SHIFT,
 * GEM_MIF_PHY_SHIFT, GEM_MIF_CONFIG_PHY_SEL, GEM_MIF_CONFIG_POLL_ENA,
 * GEM_MIF_CONFIG_BB_ENA, GEM_MII_DATAPATH_MODE, GEM_MII_DATAPATH_MII,
 * GEM_PHYAD_EXTERNAL).
 */
#define GEM_MIF_FRAME			0x620cU
#define GEM_MIF_FRAME_DATA		0x0000ffffU
#define GEM_MIF_FRAME_TA0		0x00010000U
#define GEM_MIF_FRAME_READ		0x60020000U
#define GEM_MIF_FRAME_WRITE		0x50020000U
#define GEM_MIF_REG_SHIFT		18U
#define GEM_MIF_PHY_SHIFT		23U
#define GEM_MIF_CONFIG			0x6210U
#define GEM_MIF_CONFIG_PHY_SEL		0x00000001U
#define GEM_MIF_CONFIG_POLL_ENA		0x00000002U
#define GEM_MIF_CONFIG_BB_ENA		0x00000004U
#define GEM_MII_DATAPATH_MODE		0x9050U
#define GEM_MII_DATAPATH_MII		0x00000004U
#define GEM_PHY_ADDRESS_APPLE		0U

/*
 * A descriptor's flags.
 *
 * Values referenced: FreeBSD sys/dev/gem/if_gemreg.h, release/14.3.0
 * (struct gem_desc, GEM_TD_BUFSIZE, GEM_TD_END_OF_PACKET,
 * GEM_TD_START_OF_PACKET, GEM_RD_BUFSIZE, GEM_RD_OWN, GEM_RD_BAD_CRC,
 * GEM_RD_BUFSHIFT).
 */
#define GEM_TD_BUFSIZE			0x0000000000007fffULL
#define GEM_TD_END_OF_PACKET		0x0000000040000000ULL
#define GEM_TD_START_OF_PACKET		0x0000000080000000ULL
#define GEM_RD_BUFSIZE			0x000000007fff0000ULL
#define GEM_RD_OWN			0x0000000080000000ULL
#define GEM_RD_BAD_CRC			0x4000000000000000ULL
#define GEM_RD_BUFSHIFT			16U

/*
 * The Broadcom 10/100 PHYs of Apple's boards: their identities, the
 * register whose bit isolates the PHY from the MII so completely that it
 * does not answer, and the register of its own interrupts.
 *
 * Values referenced: the Linux kernel's include/linux/sungem_phy.h (v7.2:
 * MII_BCM5201_INTERRUPT, MII_BCM5201_MULTIPHY,
 * MII_BCM5201_MULTIPHY_SUPERISOLATE) and drivers/net/sungem_phy.c (v7.2:
 * the phy_id and phy_id_mask of the BCM5201 and BCM5221 entries).
 */
#define BCM5201_IDENTITY		0x00406210U
#define BCM5221_IDENTITY		0x004061e0U
#define BCM52X1_IDENTITY_MASK		0xfffffff0U
#define BCM52X1_INTERRUPT		0x1aU
#define BCM52X1_MULTIPHY		0x1eU
#define BCM52X1_MULTIPHY_SUPERISOLATE	0x0008U

/* The PCI identities: Apple's and Sun's vendor numbers. */
#define SUNGEM_VENDOR_APPLE		0x106bU
#define SUNGEM_VENDOR_SUN		0x108eU

/* The register BAR, and the least of it the registers need. */
#define SUNGEM_REGISTER_BAR		0U
#define SUNGEM_REGISTER_BYTES		0xa000U

/*
 * Ring geometry, slot size and frame bounds.  The rings are aligned to
 * 2 KiB as the controller requires; the receive kick advances four
 * descriptors at a time, which is how the controller fetches them.
 */
#define SUNGEM_RING_COUNT		128U
#define SUNGEM_DESCRIPTOR_BYTES		16U
#define SUNGEM_DESCRIPTOR_WORDS		4U
#define SUNGEM_RING_ALIGN		2048U
#define SUNGEM_PACKET_BYTES		2048U
#define SUNGEM_PACKET_ALIGN		2048U
#define SUNGEM_KICK_BATCH		4U
#define SUNGEM_MIN_FRAME		60U
#define SUNGEM_MAX_FRAME		1514U
#define SUNGEM_HEADER_BYTES		14U

/*
 * The frame bounds the MAC is told: Ethernet's 64 bytes, and 1518 with
 * room for a VLAN tag, with the largest burst in the upper half.
 *
 * Values referenced: FreeBSD sys/dev/gem/if_gem.c, release/14.3.0
 * (gem_init_regs(): ETHER_MIN_LEN, ETHER_MAX_LEN plus
 * ETHER_VLAN_ENCAP_LEN, and 0x2000 << 16).
 */
#define SUNGEM_MAC_MIN_FRAME		64U
#define SUNGEM_MAC_MAX_FRAME		(1518U + 4U)
#define SUNGEM_MAC_MAX_BURST		(0x2000U << 16)

/*
 * Where a received frame's TCP or UDP checksum would start: after the
 * Ethernet and IPv4 headers.
 */
#define SUNGEM_CHECKSUM_START		(14U + 20U)

/* The most controllers, and the longest interface name with its terminator. */
#define SUNGEM_UNIT_MAX			2U
#define SUNGEM_NAME_BYTES		8U

/* The highest PHY address on the management interface. */
#define SUNGEM_PHY_ADDRESS_MAX		31U

/*
 * Bounded waits.  A register wait spins on the monotonic counter; a
 * management frame takes some tens of microseconds, an engine's reset or
 * stop up to a second, and the PHY's reset is given ten milliseconds on
 * each side of its line and twenty to start answering, three times over.
 *
 * Values referenced: FreeBSD sys/dev/gem/if_gem.c, release/14.3.0
 * (GEM_TRIES with gem_bitwait()'s 100-microsecond step, and the 5 ms
 * extra wait of gem_reset_rx() and gem_reset_tx()); the Linux kernel's
 * drivers/net/ethernet/sun/sungem.c (v7.2: gem_init_phy(), three tries
 * 20 ms apart) and arch/powerpc/platforms/powermac/feature.c (v7.2:
 * core99_gmac_phy_reset(), 10 ms on each side).
 */
#define SUNGEM_MIF_TIMEOUT_US		1000U
#define SUNGEM_ENGINE_TIMEOUT_US	1000000U
#define SUNGEM_ENGINE_SETTLE_US		5000U
#define SUNGEM_PHY_TRIES		3U
#define SUNGEM_PHY_SETTLE_US		20000U
#define SUNGEM_WAIT_MAX_READS		10000000U

/* The periodic service, and how often it samples the link. */
#define SUNGEM_POLL_MS			20U
#define SUNGEM_LINK_MS			250U

/*
 * The interrupts the driver serves, and the receive ones among them that
 * are held off while the network worker drains the ring.
 *
 * Procedure referenced: FreeBSD sys/dev/gem/if_gem.c, release/14.3.0
 * (gem_init_locked(): the GEM_INTMASK value).
 */
#define SUNGEM_INTERRUPTS		(GEM_INTR_TX_INTME | GEM_INTR_TX_EMPTY | \
					 GEM_INTR_RX_DONE | GEM_INTR_RX_NOBUF | \
					 GEM_INTR_RX_TAG_ERR | GEM_INTR_PERR | \
					 GEM_INTR_BERR)
#define SUNGEM_RX_INTERRUPTS		(GEM_INTR_RX_DONE | GEM_INTR_RX_NOBUF)

/* The status the controller reads as when it has gone from the bus. */
#define SUNGEM_GONE			0xffffffffU

/*
 * One controller.
 *
 * It lives for as long as the kernel runs once attached: the rings, the
 * slots and the interface are never given back.  The lock guards the
 * rings, the counters and the controller's registers, and is taken with
 * interrupts off; opened says whether the rings belong to the
 * controller.  The transmit producer and consumer count frames from the
 * first open and only grow; their difference is the frames in flight.
 */
struct sungem_unit {
	struct spinlock lock;
	struct drv_pci_device *pci;
	struct drv_pci_mapping mapping;
	volatile uint8_t *registers;
	struct drv_mii_bus mdio;
	struct drv_dma_buffer rings;
	struct drv_dma_buffer rx;
	struct drv_dma_buffer tx;
	volatile uint32_t *tx_descriptors;
	volatile uint32_t *rx_descriptors;
	uint64_t tx_ring_address;
	uint64_t rx_ring_address;
	struct net_device *net;
	struct thread *poll_thread;
	struct drv_pci_irq irq;
	void *irq_cookie;
	unsigned has_irq;
	unsigned apple;
	unsigned phy_address;
	unsigned rx_fifo_bytes;
	uint32_t mac_rx_config;
	unsigned rx_next;
	unsigned tx_producer;
	unsigned tx_consumer;
	uint64_t next_link_check;
	unsigned opened;
	unsigned link;
	unsigned speed;
	unsigned full_duplex;
	unsigned mdio_fault;
	unsigned rx_reset_wanted;
	unsigned bus_error_seen;
	char name[SUNGEM_NAME_BYTES];
	uint8_t mac[6];
};

static int sungem_attach(struct drv_pci_device *pci, const struct drv_pci_id *id);
static int sungem_open(struct net_device *net);
static void sungem_close(struct net_device *net);
static int sungem_transmit(struct net_device *net, struct packet_buf *packet);
static unsigned sungem_poll(struct net_device *net, unsigned budget);

/*
 * The PCI identities the driver binds.
 *
 * Values referenced: FreeBSD sys/dev/gem/if_gem_pci.c, release/14.3.0
 * (gem_pci_devlist: Sun's GEM and Apple's UniNorth, Pangea, UniNorth2,
 * K2, Shasta and Intrepid 2 GMACs).
 */
static const struct drv_pci_id sungem_pci_ids[] = {
	{ .vendor = SUNGEM_VENDOR_SUN, .device = 0x2bad, .subvendor = DRV_PCI_ANY_ID, .subdevice = DRV_PCI_ANY_ID },
	{ .vendor = SUNGEM_VENDOR_APPLE, .device = 0x0021, .subvendor = DRV_PCI_ANY_ID, .subdevice = DRV_PCI_ANY_ID },
	{ .vendor = SUNGEM_VENDOR_APPLE, .device = 0x0024, .subvendor = DRV_PCI_ANY_ID, .subdevice = DRV_PCI_ANY_ID },
	{ .vendor = SUNGEM_VENDOR_APPLE, .device = 0x0032, .subvendor = DRV_PCI_ANY_ID, .subdevice = DRV_PCI_ANY_ID },
	{ .vendor = SUNGEM_VENDOR_APPLE, .device = 0x004c, .subvendor = DRV_PCI_ANY_ID, .subdevice = DRV_PCI_ANY_ID },
	{ .vendor = SUNGEM_VENDOR_APPLE, .device = 0x0051, .subvendor = DRV_PCI_ANY_ID, .subdevice = DRV_PCI_ANY_ID },
	{ .vendor = SUNGEM_VENDOR_APPLE, .device = 0x006b, .subvendor = DRV_PCI_ANY_ID, .subdevice = DRV_PCI_ANY_ID },
};

/* The PCI driver of the family, constant once registered. */
static struct drv_pci_driver sungem_pci_driver = {
	.name = "sungem",
	.ids = sungem_pci_ids,
	.id_count = sizeof(sungem_pci_ids) / sizeof(sungem_pci_ids[0]),
	.attach = sungem_attach,
};

/* The network callbacks; the slots outlive every close. */
static const struct net_device_ops sungem_operations = {
	sungem_open,
	sungem_close,
	sungem_transmit,
	sungem_poll,
	NULL,
	NULL,
	NULL
};

/*
 * The board's hooks, set once before the bus is enumerated and kept;
 * NULL for a board that gives none.
 */
static const struct drv_sungem_board *sungem_board;

/*
 * The controllers, and how many of them attachments have taken.
 *
 * Attachment runs at boot, one controller at a time, and takes the next
 * unit for good, failed or not; nothing gives a unit back.
 */
static struct sungem_unit sungem_units[SUNGEM_UNIT_MAX];
static unsigned sungem_unit_count;

static int map_registers(struct sungem_unit *unit);
static int choose_mac(struct sungem_unit *unit);
static void reset_controller(struct sungem_unit *unit);
static void quiesce_receiver(struct sungem_unit *unit);
static void quiesce_transmitter(struct sungem_unit *unit);
static void reset_receiver(struct sungem_unit *unit);
static void reset_receive_mac(struct sungem_unit *unit);
static void disable_mac_receive(struct sungem_unit *unit);
static void disable_mac_transmit(struct sungem_unit *unit);
static void select_mii(struct sungem_unit *unit);
static int find_phy(struct sungem_unit *unit);
static void reset_board_phy(struct sungem_unit *unit);
static void prepare_broadcom_phy(struct sungem_unit *unit, uint32_t identity);
static int allocate_buffers(struct sungem_unit *unit);
static void setup_interrupt(struct sungem_unit *unit);
static int publish_interface(struct sungem_unit *unit);
static void initialize_mac(struct sungem_unit *unit);
static void initialize_transmit_ring(struct sungem_unit *unit);
static void start_receiver(struct sungem_unit *unit);
static void start_controller(struct sungem_unit *unit);
static void set_receive_filter(struct sungem_unit *unit);
static void apply_link(struct sungem_unit *unit);
static void update_link(struct sungem_unit *unit);
static void reclaim_transmitted(struct sungem_unit *unit);
static unsigned receive_packets(struct net_device *net, unsigned budget);
static void arm_receive_descriptor(struct sungem_unit *unit, unsigned slot);
static int sungem_interrupt(void *argument);
static void poll_worker(void *argument);
static void start_service(struct sungem_unit *unit);
static int mdio_read(void *context, unsigned phy, unsigned reg, uint16_t *data);
static int mdio_write(void *context, unsigned phy, unsigned reg, uint16_t data);
static int mif_frame(struct sungem_unit *unit, uint32_t frame, uint32_t *result);
static int wait_register(struct sungem_unit *unit, unsigned offset, uint32_t mask, uint32_t wanted, unsigned microseconds);
static void wait_microseconds(unsigned microseconds);
static uint32_t read_register(struct sungem_unit *unit, unsigned offset);
static void write_register(struct sungem_unit *unit, unsigned offset, uint32_t value);
static void write_descriptor(volatile uint32_t *descriptor, uint64_t flags, uint64_t address);
static uint64_t read_descriptor_flags(const volatile uint32_t *descriptor);
static uint32_t to_le32(uint32_t value);

/*
 * Gives the driver the board's hooks.
 */
void
drv_sungem_set_board(
	const struct drv_sungem_board *board)
{
	/* Keeps the description for the attachments that follow. */
	sungem_board = board;
}

/*
 * Registers the driver for Apple's GMAC and Sun's GEM controllers.
 */
int
drv_pci_sungem_driver_register(
	void)
{
	int error;

	/* Offers the family's PCI identities to the bus. */
	error = drv_pci_driver_register(&sungem_pci_driver);
	if (error != 0)
		return error;

	/* Succeeded: the bus binds the controllers it finds. */
	return 0;
}

/*
 * Takes a controller: its registers, its address, a reset, its PHY, its
 * rings and its interrupt, then publishes it as a network interface.
 *
 * Procedure referenced: FreeBSD sys/dev/gem/if_gem_pci.c and if_gem.c,
 * release/14.3.0 (gem_pci_attach() and gem_attach(): bus mastering, the
 * firmware's address, the reset, the MII datapath and the management
 * interface in frame mode, the first MDIO line and PHY address 0 for an
 * Apple GMAC, the receive FIFO's size).
 */
static int
sungem_attach(
	struct drv_pci_device *pci,
	const struct drv_pci_id *id)
{
	struct sungem_unit *unit;
	const char *stage;
	int error;

	/* Refuses a controller beyond the units the driver has. */
	if (sungem_unit_count == SUNGEM_UNIT_MAX) {
		kern_logf("sungem: no unit left\n");
		return ENOSPC;
	}

	/* Takes the next unit for good and names it after its place. */
	unit = &sungem_units[sungem_unit_count];
	unit->name[0] = 'g';
	unit->name[1] = 'e';
	unit->name[2] = 'm';
	unit->name[3] = (char)('0' + sungem_unit_count);
	unit->name[4] = '\0';
	sungem_unit_count++;
	unit->pci = pci;
	unit->apple = 0;
	if (id->vendor == SUNGEM_VENDOR_APPLE)
		unit->apple = 1;

	/* Prepares the lock of the rings and the registers, and the MDIO bus for the PHY. */
	spin_init(&unit->lock, LOCK_RANK_DEVICE, "sungem");
	unit->mdio.context = unit;
	unit->mdio.read = mdio_read;
	unit->mdio.write = mdio_write;

	/* Maps the registers and lets the controller reach memory. */
	stage = "registers";
	error = map_registers(unit);
	if (error != 0)
		goto fail;

	/* Takes the station address before the reset. */
	stage = "Ethernet address";
	error = choose_mac(unit);
	if (error != 0)
		goto fail;

	/* Stops whatever the firmware left running and resets both engines. */
	reset_controller(unit);

	/* Reads the receive FIFO's size, which the pause thresholds are cut from. */
	unit->rx_fifo_bytes = 64U * read_register(unit, GEM_RX_FIFO_SIZE);

	/* Puts the MAC on the MII and the management interface in frame mode on the PHY's line. */
	select_mii(unit);

	/* Finds the PHY and starts it negotiating. */
	stage = "PHY";
	error = find_phy(unit);
	if (error != 0)
		goto fail;

	/* Allocates the rings and the slots before anything is published. */
	stage = "DMA buffers";
	error = allocate_buffers(unit);
	if (error != 0)
		goto fail;

	/* Routes the controller's interrupt; without one the periodic service moves the frames. */
	setup_interrupt(unit);

	/* Publishes the interface. */
	stage = "interface";
	error = publish_interface(unit);
	if (error != 0)
		goto fail;

	/* Succeeded: networkd can open the interface. */
	return 0;

fail:
	/* Reports where the attachment stopped; what was taken is kept, as nothing reaches it. */
	kern_logf("sungem: %s initialization failed at %s (%d)\n", unit->name, stage, error);
	return error;
}

/*
 * Maps the register BAR uncached and turns on memory decoding and bus
 * mastering.
 */
static int
map_registers(
	struct sungem_unit *unit)
{
	struct drv_pci_bar bar;
	int error;

	/* Brings the function to full power, where its registers answer; one without power management always is. */
	error = drv_pci_device_set_power_state(unit->pci, DRV_PCI_D0);
	if (error != 0 && error != ENOENT)
		return error;

	/* Claims the register BAR. */
	error = drv_pci_device_claim_bar(unit->pci, SUNGEM_REGISTER_BAR);
	if (error != 0)
		return error;

	/* Reads where the BAR is and what it is. */
	error = drv_pci_device_bar(unit->pci, SUNGEM_REGISTER_BAR, &bar);
	if (error != 0)
		return error;

	/* Refuses a BAR that is not memory or too small for the registers. */
	if (bar.type != DRV_PCI_BAR_MEMORY32 && bar.type != DRV_PCI_BAR_MEMORY64)
		return ENODEV;
	if (bar.size < SUNGEM_REGISTER_BYTES)
		return ENODEV;

	/* Maps it uncached. */
	error = drv_pci_device_map_bar(unit->pci,
				       SUNGEM_REGISTER_BAR,
				       DRV_PCI_MAP_READ | DRV_PCI_MAP_WRITE | DRV_PCI_MAP_NOCACHE,
				       &unit->mapping);
	if (error != 0)
		return error;

	/* Refuses a mapping that does not cover the registers. */
	if (unit->mapping.address == NULL || unit->mapping.size < SUNGEM_REGISTER_BYTES)
		return EIO;
	unit->registers = unit->mapping.address;

	/* Turns on memory decoding. */
	error = drv_pci_device_enable_memory(unit->pci);
	if (error != 0)
		return error;

	/* Lets the controller reach memory. */
	error = drv_pci_device_set_bus_master(unit->pci, true);
	if (error != 0)
		return error;

	/* Succeeded: the registers are reachable. */
	return 0;
}

/*
 * Takes the station address: the one the board's firmware gives the
 * controller, or else one a firmware left in the controller's address
 * registers.  Refuses to invent one.
 */
static int
choose_mac(
	struct sungem_unit *unit)
{
	struct drv_pci_address address;
	uint32_t words[3];
	unsigned nonzero;
	unsigned index;
	int error;

	/* Asks the board's firmware. */
	error = ENOENT;
	if (sungem_board != NULL && sungem_board->station_address != NULL) {
		drv_pci_device_address(unit->pci, &address);
		error = sungem_board->station_address(&address, unit->mac);
	}

	/* Reads the address registers when the board gave nothing: the last two octets first. */
	if (error != 0) {
		words[0] = read_register(unit, GEM_MAC_ADDR0);
		words[1] = read_register(unit, GEM_MAC_ADDR1);
		words[2] = read_register(unit, GEM_MAC_ADDR2);
		unit->mac[0] = (uint8_t)(words[2] >> 8);
		unit->mac[1] = (uint8_t)words[2];
		unit->mac[2] = (uint8_t)(words[1] >> 8);
		unit->mac[3] = (uint8_t)words[1];
		unit->mac[4] = (uint8_t)(words[0] >> 8);
		unit->mac[5] = (uint8_t)words[0];
	}

	/* Finds out whether any octet is set. */
	nonzero = 0;
	for (index = 0; index < sizeof(unit->mac); index++)
		nonzero |= unit->mac[index];

	/* Refuses an empty address and a multicast one. */
	if (nonzero == 0 || (unit->mac[0] & 1U) != 0) {
		kern_logf("sungem: %s has no Ethernet address from the firmware\n", unit->name);
		return ENODEV;
	}

	/* Succeeded: a unicast address. */
	return 0;
}

/*
 * Stops the MAC and both DMA engines, resets the engines together, then
 * the receive MAC.
 *
 * Procedure referenced: FreeBSD sys/dev/gem/if_gem.c, release/14.3.0
 * (gem_reset(): both halves stopped, then reset together and waited
 * for; gem_reset_rx(): the receive MAC reset after its engine).
 */
static void
reset_controller(
	struct sungem_unit *unit)
{
	int error;

	/* Stops the receive half, then the transmit half. */
	quiesce_receiver(unit);
	quiesce_transmitter(unit);

	/* Resets both engines and waits for the bits to clear. */
	write_register(unit, GEM_RESET, GEM_RESET_RX | GEM_RESET_TX);
	error = wait_register(unit, GEM_RESET, GEM_RESET_RX | GEM_RESET_TX, 0, SUNGEM_ENGINE_TIMEOUT_US);
	if (error != 0)
		kern_logf("sungem: %s did not reset (%d)\n", unit->name, error);

	/* Resets the receive MAC. */
	reset_receive_mac(unit);
}

/*
 * Stops the receive MAC and DMA engine, then resets the engine and the
 * receive MAC alone, leaving the transmit half running.
 *
 * Procedure referenced: FreeBSD sys/dev/gem/if_gem.c, release/14.3.0
 * (gem_reset_rx(): the engine reset, then the MAC reset).
 */
static void
reset_receiver(
	struct sungem_unit *unit)
{
	int error;

	/* Stops the receive half. */
	quiesce_receiver(unit);

	/* Resets the receive engine. */
	write_register(unit, GEM_RESET, GEM_RESET_RX);
	error = wait_register(unit, GEM_RESET, GEM_RESET_RX, 0, SUNGEM_ENGINE_TIMEOUT_US);
	if (error != 0)
		kern_logf("sungem: %s receive engine did not reset (%d)\n", unit->name, error);

	/* Resets the receive MAC. */
	reset_receive_mac(unit);
}

/* Resets the receive MAC and waits for its bit to clear. */
static void
reset_receive_mac(
	struct sungem_unit *unit)
{
	int error;

	/* Starts the reset; the bit clears itself when it is done. */
	write_register(unit, GEM_MAC_RXRESET, 1U);
	error = wait_register(unit, GEM_MAC_RXRESET, 1U, 0, SUNGEM_ENGINE_TIMEOUT_US);
	if (error != 0)
		kern_logf("sungem: %s receive MAC did not reset (%d)\n", unit->name, error);
}

/*
 * Stops the receive MAC and the receive DMA engine, and lets what was in
 * flight land.
 *
 * Procedure referenced: FreeBSD sys/dev/gem/if_gem.c, release/14.3.0
 * (gem_reset_rx(): the MAC disabled, the DMA turned off and waited for,
 * then 5 ms more).
 */
static void
quiesce_receiver(
	struct sungem_unit *unit)
{
	int error;

	/* Stops taking frames off the wire. */
	disable_mac_receive(unit);

	/* Turns the DMA off and waits for it, then lets what was in flight land. */
	write_register(unit, GEM_RX_CONFIG, 0);
	error = wait_register(unit, GEM_RX_CONFIG, GEM_RX_CONFIG_RXDMA_EN, 0, SUNGEM_ENGINE_TIMEOUT_US);
	if (error != 0)
		kern_logf("sungem: %s receive DMA did not stop (%d)\n", unit->name, error);
	wait_microseconds(SUNGEM_ENGINE_SETTLE_US);
}

/*
 * Stops the transmit MAC and the transmit DMA engine, and lets what was
 * in flight land.
 *
 * Procedure referenced: FreeBSD sys/dev/gem/if_gem.c, release/14.3.0
 * (gem_reset_tx(): the MAC disabled, the DMA turned off and waited for,
 * then 5 ms more).
 */
static void
quiesce_transmitter(
	struct sungem_unit *unit)
{
	int error;

	/* Stops putting frames on the wire. */
	disable_mac_transmit(unit);

	/* Turns the DMA off and waits for it, then lets what was in flight land. */
	write_register(unit, GEM_TX_CONFIG, 0);
	error = wait_register(unit, GEM_TX_CONFIG, GEM_TX_CONFIG_TXDMA_EN, 0, SUNGEM_ENGINE_TIMEOUT_US);
	if (error != 0)
		kern_logf("sungem: %s transmit DMA did not stop (%d)\n", unit->name, error);
	wait_microseconds(SUNGEM_ENGINE_SETTLE_US);
}

/* Turns the receive MAC off and waits until it is. */
static void
disable_mac_receive(
	struct sungem_unit *unit)
{
	uint32_t config;
	int error;

	/* Clears the enable bit. */
	config = read_register(unit, GEM_MAC_RX_CONFIG);
	write_register(unit, GEM_MAC_RX_CONFIG, config & ~GEM_MAC_RX_ENABLE);

	/* Waits for the MAC to finish the frame it was taking. */
	error = wait_register(unit, GEM_MAC_RX_CONFIG, GEM_MAC_RX_ENABLE, 0, SUNGEM_ENGINE_TIMEOUT_US);
	if (error != 0)
		kern_logf("sungem: %s receive MAC did not stop (%d)\n", unit->name, error);
}

/* Turns the transmit MAC off and waits until it is. */
static void
disable_mac_transmit(
	struct sungem_unit *unit)
{
	uint32_t config;
	int error;

	/* Clears the enable bit. */
	config = read_register(unit, GEM_MAC_TX_CONFIG);
	write_register(unit, GEM_MAC_TX_CONFIG, config & ~GEM_MAC_TX_ENABLE);

	/* Waits for the MAC to finish the frame it was sending. */
	error = wait_register(unit, GEM_MAC_TX_CONFIG, GEM_MAC_TX_ENABLE, 0, SUNGEM_ENGINE_TIMEOUT_US);
	if (error != 0)
		kern_logf("sungem: %s transmit MAC did not stop (%d)\n", unit->name, error);
}

/*
 * Puts the MAC's datapath on the MII and the management interface in
 * frame mode, without polling, on the line the PHY is wired to: the
 * first on an Apple board, the second (an external PHY) on Sun's.
 *
 * Procedure referenced: FreeBSD sys/dev/gem/if_gem.c, release/14.3.0
 * (gem_attach(): GEM_MII_DATAPATH_MII; gem_mifinit(): bit-bang off; the
 * PHY select cleared for the internal line).
 */
static void
select_mii(
	struct sungem_unit *unit)
{
	uint32_t config;

	/* Takes frames through the MII rather than the serial link. */
	write_register(unit, GEM_MII_DATAPATH_MODE, GEM_MII_DATAPATH_MII);

	/* Frame mode, no polling, and the line the board wires its PHY to. */
	config = read_register(unit, GEM_MIF_CONFIG);
	config &= ~GEM_MIF_CONFIG_BB_ENA;
	config &= ~GEM_MIF_CONFIG_POLL_ENA;
	config &= ~GEM_MIF_CONFIG_PHY_SEL;
	if (unit->apple == 0)
		config |= GEM_MIF_CONFIG_PHY_SEL;
	write_register(unit, GEM_MIF_CONFIG, config);

	/* Turns the MII's outputs on. */
	write_register(unit, GEM_MAC_XIF_CONFIG, GEM_MAC_XIF_TX_MII_ENA);
}

/*
 * Brings the PHY out of reset, finds it on the management interface,
 * resets it and starts it negotiating.  An Apple board's PHY is at
 * address 0; another board's is the first that answers.
 */
static int
find_phy(
	struct sungem_unit *unit)
{
	uint32_t identity;
	unsigned address;
	int error;

	/* Has the board pulse the PHY's reset line, which an Apple board needs. */
	if (unit->apple != 0)
		reset_board_phy(unit);

	/* Asks the board's address first; a PHY there is the one. */
	identity = 0;
	address = GEM_PHY_ADDRESS_APPLE;
	error = drv_mii_phy_read_identity(&unit->mdio, address, &identity);

	/* Asks every other address in turn and takes the first that answers. */
	if (error == ENXIO) {
		for (address = 0; address <= SUNGEM_PHY_ADDRESS_MAX; address++) {
			/* Skips the address already asked. */
			if (address == GEM_PHY_ADDRESS_APPLE)
				continue;

			/* Stops at a PHY, and at a bus that failed. */
			error = drv_mii_phy_read_identity(&unit->mdio, address, &identity);
			if (error != ENXIO)
				break;
		}
	}

	/* Reports a bus with no PHY on it, or one that failed. */
	if (error != 0) {
		kern_logf("sungem: %s has no PHY (%d)\n", unit->name, error);
		return error;
	}
	unit->phy_address = address;

	/* Brings a Broadcom 10/100 PHY out of its isolation. */
	prepare_broadcom_phy(unit, identity);

	/* Resets the PHY to its defaults. */
	error = drv_mii_phy_reset(&unit->mdio, unit->phy_address);
	if (error != 0)
		return error;

	/* Advertises every mode it has and starts negotiating. */
	error = drv_mii_phy_start_autonegotiation(&unit->mdio, unit->phy_address, 1);
	if (error != 0)
		return error;

	/* Records the PHY found on the bus. */
	kern_logf("sungem: %s PHY %u id %08x\n", unit->name, unit->phy_address, identity);

	/* Succeeded: the PHY negotiates with its link partner. */
	return 0;
}

/*
 * Pulses the PHY's reset line through the board's hook and resets the
 * PHY over the management interface, up to three times, until the PHY
 * answers.  A board without the hook leaves the PHY as it is.
 *
 * Procedure referenced: the Linux kernel's
 * drivers/net/ethernet/sun/sungem.c, v7.2 (gem_init_phy(): the line's
 * pulse, 20 ms, a reset written to the control register, 20 ms, and the
 * control register read for all ones, three times at most).
 */
static void
reset_board_phy(
	struct sungem_unit *unit)
{
	uint16_t control;
	unsigned attempt;
	int error;

	/* A board without the line leaves the PHY as the firmware left it. */
	if (sungem_board == NULL || sungem_board->reset_phy == NULL)
		return;

	/* Pulses the line and resets the PHY until it answers. */
	for (attempt = 0; attempt < SUNGEM_PHY_TRIES; attempt++) {
		/* Pulses the line; a board that has none stops here. */
		error = sungem_board->reset_phy();
		if (error != 0)
			return;
		wait_microseconds(SUNGEM_PHY_SETTLE_US);

		/* Resets the PHY by its control register as well, which some PHYs need to answer again. */
		(void)mdio_write(unit, GEM_PHY_ADDRESS_APPLE, 0, 0x8000U);
		wait_microseconds(SUNGEM_PHY_SETTLE_US);

		/* A control register that is not all ones belongs to a PHY that answers. */
		error = mdio_read(unit, GEM_PHY_ADDRESS_APPLE, 0, &control);
		if (error == 0 && control != 0xffffU)
			return;
	}

	/* Reports a PHY that did not come back; the search goes on. */
	kern_logf("sungem: %s PHY does not answer after its reset\n", unit->name);
}

/*
 * Clears a Broadcom BCM5201 or BCM5221's super-isolation, which leaves
 * it unreachable from the MII, and turns its own interrupts off; another
 * PHY is left alone.
 */
static void
prepare_broadcom_phy(
	struct sungem_unit *unit,
	uint32_t identity)
{
	uint16_t multiphy;
	int error;

	/* Leaves any other PHY alone. */
	if ((identity & BCM52X1_IDENTITY_MASK) != BCM5201_IDENTITY &&
	    (identity & BCM52X1_IDENTITY_MASK) != BCM5221_IDENTITY)
		return;

	/* Clears the super-isolation bit. */
	error = mdio_read(unit, unit->phy_address, BCM52X1_MULTIPHY, &multiphy);
	if (error != 0)
		return;
	(void)mdio_write(unit, unit->phy_address, BCM52X1_MULTIPHY, multiphy & (uint16_t)~BCM52X1_MULTIPHY_SUPERISOLATE);

	/* Turns the PHY's interrupts off; the driver reads the link by polling. */
	(void)mdio_write(unit, unit->phy_address, BCM52X1_INTERRUPT, 0);
}

/*
 * Allocates both descriptor rings and the receive and transmit slots in
 * memory the controller reaches: the transmit ring first, then the
 * receive ring, each aligned as the controller requires.
 */
static int
allocate_buffers(
	struct sungem_unit *unit)
{
	struct drv_dma_device *dma;
	size_t ring_bytes;
	int error;

	/* The bus's DMA device. */
	dma = drv_pci_device_dma(unit->pci);
	if (dma == NULL)
		return ENODEV;

	/* Allocates both rings in one block, the transmit ring first. */
	ring_bytes = (size_t)SUNGEM_RING_COUNT * SUNGEM_DESCRIPTOR_BYTES;
	error = drv_dma_alloc_coherent(dma, 2U * ring_bytes, SUNGEM_RING_ALIGN, &unit->rings);
	if (error != 0)
		return error;
	unit->tx_descriptors = unit->rings.address;
	unit->rx_descriptors = unit->tx_descriptors + SUNGEM_RING_COUNT * SUNGEM_DESCRIPTOR_WORDS;
	unit->tx_ring_address = unit->rings.device_address;
	unit->rx_ring_address = unit->rings.device_address + ring_bytes;

	/* Allocates the receive slots. */
	error = drv_dma_alloc_coherent(dma, (size_t)SUNGEM_RING_COUNT * SUNGEM_PACKET_BYTES, SUNGEM_PACKET_ALIGN, &unit->rx);
	if (error != 0)
		return error;

	/* Allocates the transmit slots. */
	error = drv_dma_alloc_coherent(dma, (size_t)SUNGEM_RING_COUNT * SUNGEM_PACKET_BYTES, SUNGEM_PACKET_ALIGN, &unit->tx);
	if (error != 0)
		return error;

	/* Succeeded: the rings and the slots are ready. */
	return 0;
}

/*
 * Masks the controller's interrupts and connects its pin to the
 * handler; without a pin the periodic service moves the frames.
 */
static void
setup_interrupt(
	struct sungem_unit *unit)
{
	unsigned count;
	int error;

	/* Masks every source until the interface opens. */
	write_register(unit, GEM_INTMASK, 0xffffffffU);
	unit->has_irq = 0;

	/* Takes the pin's line. */
	error = drv_pci_device_allocate_irqs(unit->pci, DRV_PCI_IRQ_ALLOW_INTX, 1U, 1U, &unit->irq, &count);
	if (error != 0 || count != 1U) {
		kern_logf("sungem: %s has no interrupt (%d); it is polled\n", unit->name, error);
		return;
	}

	/* Connects the handler. */
	error = drv_pci_device_establish_irq(unit->pci, &unit->irq, sungem_interrupt, unit, "sungem", &unit->irq_cookie);
	if (error != 0) {
		drv_pci_device_free_irqs(unit->pci, &unit->irq, 1U);
		kern_logf("sungem: %s interrupt unavailable (%d); it is polled\n", unit->name, error);
		return;
	}

	/* The line reaches the handler. */
	unit->has_irq = 1;
}

/*
 * Describes the interface (its name and address, broadcast and
 * multicast) and publishes it.
 */
static int
publish_interface(
	struct sungem_unit *unit)
{
	struct net_device *net;
	int error;

	/* Obtains the interface's registry slot. */
	net = net_device_alloc();
	if (net == NULL)
		return ENOMEM;

	/* Keeps the interface with the controller; the service and the interrupt reach it there. */
	unit->net = net;

	/* Describes the interface. */
	kern_memcpy(net->name, unit->name, kern_strlen(unit->name) + 1U);
	net->mtu = 1500U;
	net->mtu_max = 1500U;
	net->hwaddr_len = 6U;
	kern_memcpy(net->hwaddr, unit->mac, 6U);
	net->flags = NET_DEVICE_BROADCAST | NET_DEVICE_MULTICAST;
	net->ops = &sungem_operations;
	net->driver_data = unit;

	/* Publishes it. */
	error = net_device_create(net);
	if (error != 0)
		return error;

	/* Reports the interface, and whether it is polled. */
	kern_logf("sungem: %s ready MAC %02x:%02x:%02x:%02x:%02x:%02x%s\n",
		  net->name,
		  unit->mac[0],
		  unit->mac[1],
		  unit->mac[2],
		  unit->mac[3],
		  unit->mac[4],
		  unit->mac[5],
		  unit->has_irq != 0 ? "" : " (polled)");

	/* Succeeded: the interface is in the registry. */
	return 0;
}

/*
 * Writes the MAC's timing, frame bounds, addresses and filters, and
 * zeroes its counters.  The caller holds the lock.
 *
 * Procedure referenced: FreeBSD sys/dev/gem/if_gem.c, release/14.3.0
 * (gem_init_regs()).
 */
static void
initialize_mac(
	struct sungem_unit *unit)
{
	unsigned offset;

	/* The inter-packet gaps, frame bounds, preamble, jam, attempts and the pause frame's type. */
	write_register(unit, GEM_MAC_IPG0, 0);
	write_register(unit, GEM_MAC_IPG1, 8U);
	write_register(unit, GEM_MAC_IPG2, 4U);
	write_register(unit, GEM_MAC_MAC_MIN_FRAME, SUNGEM_MAC_MIN_FRAME);
	write_register(unit, GEM_MAC_MAC_MAX_FRAME, SUNGEM_MAC_MAX_FRAME | SUNGEM_MAC_MAX_BURST);
	write_register(unit, GEM_MAC_PREAMBLE_LEN, 0x7U);
	write_register(unit, GEM_MAC_JAM_SIZE, 0x4U);
	write_register(unit, GEM_MAC_ATTEMPT_LIMIT, 0x10U);
	write_register(unit, GEM_MAC_CONTROL_TYPE, 0x8808U);

	/* Seeds the backoff from the station address. */
	write_register(unit, GEM_MAC_RANDOM_SEED, (((uint32_t)unit->mac[5] << 8) | unit->mac[4]) & 0x3ffU);

	/* Empties the alternate address. */
	write_register(unit, GEM_MAC_ADDR3, 0);
	write_register(unit, GEM_MAC_ADDR4, 0);
	write_register(unit, GEM_MAC_ADDR5, 0);

	/* The MAC control address, 01:80:c2:00:00:01, the last two octets first. */
	write_register(unit, GEM_MAC_ADDR6, 0x0001U);
	write_register(unit, GEM_MAC_ADDR7, 0xc200U);
	write_register(unit, GEM_MAC_ADDR8, 0x0180U);

	/* Empties the address filter and its masks. */
	write_register(unit, GEM_MAC_ADDR_FILTER0, 0);
	write_register(unit, GEM_MAC_ADDR_FILTER1, 0);
	write_register(unit, GEM_MAC_ADDR_FILTER2, 0);
	write_register(unit, GEM_MAC_ADR_FLT_MASK1_2, 0);
	write_register(unit, GEM_MAC_ADR_FLT_MASK0, 0);

	/* Zeroes the collision, defer, attempt and receive error counters. */
	for (offset = GEM_MAC_NORM_COLL_CNT; offset <= GEM_MAC_RX_CODE_VIOL; offset += 4U)
		write_register(unit, offset, 0);

	/* The pause time this end asks for. */
	write_register(unit, GEM_MAC_SEND_PAUSE_CMD, 0x1bf0U);

	/* The station address, the last two octets first. */
	write_register(unit, GEM_MAC_ADDR0, ((uint32_t)unit->mac[4] << 8) | unit->mac[5]);
	write_register(unit, GEM_MAC_ADDR1, ((uint32_t)unit->mac[2] << 8) | unit->mac[3]);
	write_register(unit, GEM_MAC_ADDR2, ((uint32_t)unit->mac[0] << 8) | unit->mac[1]);

	/* Turns the MII's outputs on. */
	write_register(unit, GEM_MAC_XIF_CONFIG, GEM_MAC_XIF_TX_MII_ENA);
}

/*
 * Marks every transmit descriptor as empty and starts the cursors at
 * the ring's start.  The caller holds the lock with the engine stopped.
 */
static void
initialize_transmit_ring(
	struct sungem_unit *unit)
{
	unsigned index;

	/* Empties every descriptor. */
	for (index = 0; index < SUNGEM_RING_COUNT; index++)
		write_descriptor(unit->tx_descriptors + index * SUNGEM_DESCRIPTOR_WORDS, 0, 0);

	/* No frame is in flight. */
	unit->tx_producer = 0;
	unit->tx_consumer = 0;

	/* Publishes the descriptors before the controller is given them. */
	kern_io_write_barrier();
}

/*
 * Gives every receive descriptor its slot, hands the ring to the
 * receive engine and starts it.  The caller holds the lock with the
 * engine stopped.
 *
 * Procedure referenced: FreeBSD sys/dev/gem/if_gem.c, release/14.3.0
 * (gem_init_locked() steps 6, 10 and 15, and gem_reset_rxdma(): the ring
 * base, the configuration with the ring's size, the checksum start and
 * the FIFO threshold, the interrupt blanking, the pause thresholds, the
 * kick four short of the ring's end, the MAC's receive mask).
 */
static void
start_receiver(
	struct sungem_unit *unit)
{
	uint32_t config;
	unsigned index;

	/* Hands every descriptor to the controller, pointing at its slot. */
	for (index = 0; index < SUNGEM_RING_COUNT; index++)
		arm_receive_descriptor(unit, index);
	unit->rx_next = 0;
	kern_io_write_barrier();

	/* Gives the engine the ring. */
	write_register(unit, GEM_RX_RING_PTR_HI, (uint32_t)(unit->rx_ring_address >> 32));
	write_register(unit, GEM_RX_RING_PTR_LO, (uint32_t)unit->rx_ring_address);

	/* The ring's size, where checksums start, and when the FIFO is emptied to memory. */
	config = GEM_RING_SIZE_128;
	config |= SUNGEM_CHECKSUM_START << GEM_RX_CONFIG_CXM_START_SHIFT;
	config |= GEM_THRESHOLD_1024 << GEM_RX_CONFIG_FIFO_THRS_SHIFT;
	write_register(unit, GEM_RX_CONFIG, config);

	/* Holds an interrupt for up to six frames or a short time. */
	write_register(unit, GEM_RX_BLANKING, (1U << GEM_RX_BLANKING_TIME_SHIFT) | 6U);

	/* Sends a pause at three quarters of the FIFO and ends it at a quarter. */
	write_register(unit, GEM_RX_PAUSE_THRESH, (3U * unit->rx_fifo_bytes / 256U) | ((unit->rx_fifo_bytes / 256U) << 12));

	/* Starts the engine. */
	write_register(unit, GEM_RX_CONFIG, config | GEM_RX_CONFIG_RXDMA_EN);

	/* Leaves out the MAC's receive events the driver does not serve. */
	write_register(unit, GEM_MAC_RX_MASK, GEM_MAC_RX_DONE | GEM_MAC_RX_FRAME_CNT);

	/* Offers the controller all but the last four descriptors. */
	write_register(unit, GEM_RX_KICK, SUNGEM_RING_COUNT - SUNGEM_KICK_BATCH);

	/* Sets the filter, which starts the receive MAC again on a link that is up. */
	set_receive_filter(unit);
}

/*
 * Resets the controller and starts it with fresh rings: the MAC, the
 * transmit engine, the receive engine, the interrupts, then the link.
 * The caller holds the lock.
 *
 * Procedure referenced: FreeBSD sys/dev/gem/if_gem.c, release/14.3.0
 * (gem_init_locked(): the reset, the MIF, the rings, gem_init_regs(),
 * the ring bases, GEM_CONFIG with infinite bursts and, for an Apple
 * GMAC, the two bug fixes, the interrupt masks, the transmit
 * configuration, the receive configuration, then the MAC).
 */
static void
start_controller(
	struct sungem_unit *unit)
{
	uint32_t config;

	/* Stops and resets both engines, and puts the management interface back in frame mode. */
	reset_controller(unit);
	select_mii(unit);

	/* Empties the transmit ring and writes the MAC's registers. */
	initialize_transmit_ring(unit);
	initialize_mac(unit);

	/* Gives the transmit engine its ring. */
	write_register(unit, GEM_TX_RING_PTR_HI, (uint32_t)(unit->tx_ring_address >> 32));
	write_register(unit, GEM_TX_RING_PTR_LO, (uint32_t)unit->tx_ring_address);

	/* Lets the engines burst a whole frame; an Apple GMAC also takes its two bug fixes. */
	config = GEM_CONFIG_TXDMA_LIMIT | GEM_CONFIG_RXDMA_LIMIT | GEM_CONFIG_BURST_INF;
	if (unit->apple != 0)
		config |= GEM_CONFIG_RONPAULBIT | GEM_CONFIG_BUG2FIX;
	write_register(unit, GEM_CONFIG, config);

	/* Leaves out the MAC's transmit and control events the driver does not serve. */
	write_register(unit, GEM_MAC_TX_MASK, GEM_MAC_TX_XMIT_DONE | GEM_MAC_TX_DEFER_EXP | GEM_MAC_TX_PEAK_EXP);
	write_register(unit, GEM_MAC_CONTROL_MASK, GEM_MAC_PAUSED | GEM_MAC_PAUSE | GEM_MAC_RESUME);

	/* Starts the transmit engine with its ring's size and FIFO threshold. */
	write_register(unit, GEM_TX_CONFIG, GEM_RING_SIZE_128 | (GEM_TX_FIFO_THRESHOLD & GEM_TX_CONFIG_TXFIFO_TH) | GEM_TX_CONFIG_TXDMA_EN);

	/* The receive MAC strips the frame check sequence; the filter decides the rest. */
	unit->mac_rx_config = GEM_MAC_RX_STRIP_CRC;

	/* Starts the receive engine and its MAC's filter. */
	start_receiver(unit);

	/* Takes the interrupts the driver serves, or none when it polls. */
	if (unit->has_irq != 0)
		write_register(unit, GEM_INTMASK, ~SUNGEM_INTERRUPTS);

	/* Reads the link and starts the MAC for it. */
	unit->link = 0;
	unit->speed = 0;
	unit->full_duplex = 0;
	unit->next_link_check = 0;
	update_link(unit);
}

/*
 * Accepts the station's own frames, broadcasts and every multicast
 * group (the hash table all ones), and starts the receive MAC again on a
 * link that is up.  The caller holds the lock.
 *
 * Procedure referenced: FreeBSD sys/dev/gem/if_gem.c, release/14.3.0
 * (gem_setladrf(): the receive MAC disabled, the filter bits cleared,
 * IFF_ALLMULTI as all ones in the hash table with the hash filter on).
 */
static void
set_receive_filter(
	struct sungem_unit *unit)
{
	uint32_t config;
	unsigned index;

	/* Stops the receive MAC while the filter changes. */
	disable_mac_receive(unit);

	/* Every multicast group passes the hash table. */
	for (index = 0; index < GEM_MAC_HASH_COUNT; index++)
		write_register(unit, GEM_MAC_HASH0 + index * 4U, 0xffffU);

	/* Turns the hash filter on, and the other filters off. */
	config = unit->mac_rx_config;
	config &= ~(GEM_MAC_RX_HASH_FILTER | GEM_MAC_RX_PROMISCUOUS | GEM_MAC_RX_PROMISC_GRP | GEM_MAC_RX_ADDR_FILTER);
	config |= GEM_MAC_RX_HASH_FILTER;
	unit->mac_rx_config = config;

	/* Starts the receive MAC again when the interface is up on a link. */
	if (unit->link != 0)
		config |= GEM_MAC_RX_ENABLE;
	write_register(unit, GEM_MAC_RX_CONFIG, config);
}

/*
 * Sets the MAC for the link the PHY settled on and starts it on a link
 * that is up.  The caller holds the lock.
 *
 * Procedure referenced: FreeBSD sys/dev/gem/if_gem.c, release/14.3.0
 * (gem_mii_statchg(): the transmit configuration, carrier and collisions
 * ignored on full duplex, carrier extension on half-duplex gigabit, pause
 * off, the slot time, the XIF configuration, then both MACs enabled).
 */
static void
apply_link(
	struct sungem_unit *unit)
{
	uint32_t rx_config;
	uint32_t tx_config;
	uint32_t control;
	uint32_t xif;
	unsigned gigabit;

	/* Whether the link runs at a gigabit. */
	gigabit = 0;
	if (unit->speed == 1000U)
		gigabit = 1;

	/* The receive and transmit configurations for the duplex and the speed. */
	rx_config = unit->mac_rx_config & ~GEM_MAC_RX_CARR_EXTEND;
	tx_config = GEM_MAC_TX_ENA_IPG0 | GEM_MAC_TX_NGU | GEM_MAC_TX_NGU_LIMIT;
	if (unit->full_duplex != 0) {
		tx_config |= GEM_MAC_TX_IGN_CARRIER | GEM_MAC_TX_IGN_COLLIS;
	} else if (gigabit != 0) {
		rx_config |= GEM_MAC_RX_CARR_EXTEND;
		tx_config |= GEM_MAC_TX_CARR_EXTEND;
	}
	unit->mac_rx_config = rx_config;

	/* Stops both MACs and writes their configurations. */
	disable_mac_transmit(unit);
	write_register(unit, GEM_MAC_TX_CONFIG, tx_config);
	disable_mac_receive(unit);
	write_register(unit, GEM_MAC_RX_CONFIG, rx_config);

	/* Neither sends nor obeys pause frames. */
	control = read_register(unit, GEM_MAC_CONTROL_CONFIG);
	control &= ~(GEM_MAC_CC_RX_PAUSE | GEM_MAC_CC_TX_PAUSE);
	write_register(unit, GEM_MAC_CONTROL_CONFIG, control);

	/* The slot time: extended for carrier extension on half-duplex gigabit. */
	if (unit->full_duplex == 0 && gigabit != 0) {
		write_register(unit, GEM_MAC_SLOT_TIME, GEM_MAC_SLOT_TIME_CARR_EXTEND);
	} else {
		write_register(unit, GEM_MAC_SLOT_TIME, GEM_MAC_SLOT_TIME_NORMAL);
	}

	/* The MII's outputs and LEDs, the receive buffers of the first line, GMII at a gigabit. */
	xif = GEM_MAC_XIF_LINK_LED | GEM_MAC_XIF_TX_MII_ENA;
	if (unit->apple != 0) {
		xif |= GEM_MAC_XIF_MII_BUF_ENA;
	} else if (unit->full_duplex == 0) {
		xif |= GEM_MAC_XIF_ECHO_DISABL;
	}
	if (gigabit != 0)
		xif |= GEM_MAC_XIF_GMII_MODE;
	if (unit->full_duplex != 0)
		xif |= GEM_MAC_XIF_FDPLX_LED;
	write_register(unit, GEM_MAC_XIF_CONFIG, xif);

	/* Starts both MACs on a link that is up. */
	if (unit->link != 0) {
		write_register(unit, GEM_MAC_TX_CONFIG, tx_config | GEM_MAC_TX_ENABLE);
		write_register(unit, GEM_MAC_RX_CONFIG, rx_config | GEM_MAC_RX_ENABLE);
	}
}

/*
 * Reads the PHY's link at a bounded rate and sets the MAC for a link
 * that changed.  The caller holds the lock.
 */
static void
update_link(
	struct sungem_unit *unit)
{
	struct drv_mii_link link;
	uint64_t now;
	unsigned full_duplex;
	int error;

	/* Samples the PHY at most once per interval. */
	now = clock_ticks();
	if (unit->next_link_check != 0 && now < unit->next_link_check)
		return;
	unit->next_link_check = now + kern_ms_to_ticks(SUNGEM_LINK_MS);

	/* Reads the negotiated speed and duplex. */
	error = drv_mii_phy_read_link(&unit->mdio, unit->phy_address, &link);
	if (error != 0) {
		/* Reports a failing management interface once, and stops traffic until it answers. */
		if (unit->mdio_fault == 0)
			kern_logf("sungem: %s PHY status failed (%d)\n", unit->name, error);
		unit->mdio_fault = 1;
		link.speed_mbps = 0;
		link.full_duplex = 0;
	} else {
		/* The PHY answered: a later failure is reported again. */
		unit->mdio_fault = 0;
	}

	/* Leaves the MAC alone while the link is as it was. */
	full_duplex = 0;
	if (link.full_duplex)
		full_duplex = 1;
	if (link.speed_mbps == unit->speed && full_duplex == unit->full_duplex)
		return;

	/* Reports the change: a pulled cable, or a negotiated speed. */
	if (link.speed_mbps == 0) {
		kern_logf("sungem: %s link down\n", unit->name);
	} else if (full_duplex != 0) {
		kern_logf("sungem: %s link up %u Mbps full duplex\n", unit->name, link.speed_mbps);
	} else {
		kern_logf("sungem: %s link up %u Mbps half duplex\n", unit->name, link.speed_mbps);
	}

	/* Publishes the link and sets the MAC for it. */
	unit->speed = link.speed_mbps;
	unit->full_duplex = full_duplex;
	unit->link = 0;
	if (link.speed_mbps != 0)
		unit->link = 1;
	unit->net->link_mbps = link.speed_mbps;
	apply_link(unit);
}

/* Opens the interface with fresh rings, and starts the periodic service. */
static int
sungem_open(
	struct net_device *net)
{
	struct sungem_unit *unit;
	unsigned long irq;
	unsigned link;

	/* Serializes the start against the interrupt and the transmit path. */
	unit = net->driver_data;
	irq = spin_lock_irqsave(&unit->lock);

	/* A repeated open keeps the rings as they are. */
	if (unit->opened != 0) {
		spin_unlock_irqrestore(&unit->lock, irq);
		return 0;
	}

	/* Resets the controller, gives it fresh rings and starts it on the link. */
	start_controller(unit);

	/* Publishes opened once the rings belong to the controller. */
	unit->opened = 1;
	unit->rx_reset_wanted = 0;
	link = unit->link;

	spin_unlock_irqrestore(&unit->lock, irq);

	/* Announces the carrier outside the lock, and keeps the interface served. */
	(void)net_device_set_carrier(net, (int)link);
	start_service(unit);
	kern_logf("sungem: %s RX/TX enabled\n", net->name);

	/* Succeeded: frames move by interrupt and the periodic service. */
	return 0;
}

/* Stops the interface: no interrupt, both MACs and engines stopped. */
static void
sungem_close(
	struct net_device *net)
{
	struct sungem_unit *unit;
	unsigned long irq;

	/* Closes the producer gate and stops the controller. */
	unit = net->driver_data;
	irq = spin_lock_irqsave(&unit->lock);

	unit->opened = 0;
	unit->link = 0;
	unit->speed = 0;
	write_register(unit, GEM_INTMASK, 0xffffffffU);
	reset_controller(unit);
	(void)read_register(unit, GEM_STATUS);

	spin_unlock_irqrestore(&unit->lock, irq);

	/* Withdraws the carrier. */
	(void)net_device_set_carrier(net, 0);
}

/* Copies one frame into the next transmit slot and kicks the transmit engine. */
static int
sungem_transmit(
	struct net_device *net,
	struct packet_buf *packet)
{
	struct sungem_unit *unit;
	uint8_t *payload;
	uint64_t address;
	uint64_t flags;
	unsigned long irq;
	unsigned slot;
	size_t length;
	int error;

	/* Refuses a missing packet. */
	if (packet == NULL)
		return EINVAL;

	/* Consumes a frame that is too short or too long for one slot. */
	length = packet->length;
	if (length < SUNGEM_HEADER_BYTES || length > SUNGEM_MAX_FRAME) {
		packet_buf_free(packet);
		return EMSGSIZE;
	}

	/* Serializes transmit with completion and close. */
	unit = net->driver_data;
	error = 0;
	irq = spin_lock_irqsave(&unit->lock);

	/* Takes back the slots the controller has sent. */
	if (unit->opened != 0)
		reclaim_transmitted(unit);

	/* Refuses a closed interface, no link, or a full ring (one slot always stays empty). */
	if (unit->opened == 0 || unit->link == 0) {
		error = ENETDOWN;
	} else if (unit->tx_producer - unit->tx_consumer >= SUNGEM_RING_COUNT - 1U) {
		error = ENOBUFS;
	}

	/* Hands one complete frame to the controller. */
	if (error == 0) {
		/* Copies the frame and pads a short one with zeros. */
		slot = unit->tx_producer % SUNGEM_RING_COUNT;
		payload = (uint8_t *)unit->tx.address + (size_t)slot * SUNGEM_PACKET_BYTES;
		kern_memcpy(payload, packet->data, length);
		if (length < SUNGEM_MIN_FRAME) {
			kern_memset(payload + length, 0, SUNGEM_MIN_FRAME - length);
			length = SUNGEM_MIN_FRAME;
		}

		/* Describes the slot as one whole frame. */
		address = unit->tx.device_address + (uint64_t)slot * SUNGEM_PACKET_BYTES;
		flags = ((uint64_t)length & GEM_TD_BUFSIZE) | GEM_TD_START_OF_PACKET | GEM_TD_END_OF_PACKET;
		write_descriptor(unit->tx_descriptors + slot * SUNGEM_DESCRIPTOR_WORDS, flags, address);

		/* Publishes the frame and the descriptor, then moves the kick past it. */
		unit->tx_producer++;
		kern_io_write_barrier();
		write_register(unit, GEM_TX_KICK, unit->tx_producer % SUNGEM_RING_COUNT);
	}

	spin_unlock_irqrestore(&unit->lock, irq);

	/* Releases the stack's packet whatever happened. */
	packet_buf_free(packet);

	/* Reports a refused frame. */
	if (error != 0)
		return error;

	/* Succeeded: the slot holds the frame. */
	return 0;
}

/*
 * Takes back the transmit slots the controller has sent: those before
 * the index its completion register reports.  The caller holds the lock.
 *
 * Procedure referenced: FreeBSD sys/dev/gem/if_gem.c, release/14.3.0
 * (gem_tint(): GEM_TX_COMPLETION is the last descriptor processed plus
 * one).
 */
static void
reclaim_transmitted(
	struct sungem_unit *unit)
{
	unsigned completion;

	/* Where the controller has got to. */
	completion = read_register(unit, GEM_TX_COMPLETION) & GEM_TX_COMPLETION_MASK;
	completion %= SUNGEM_RING_COUNT;

	/* Moves past every slot before it, never past the frames handed over. */
	while (unit->tx_consumer != unit->tx_producer &&
	       unit->tx_consumer % SUNGEM_RING_COUNT != completion)
		unit->tx_consumer++;
}

/*
 * Points a receive descriptor at its slot and hands it to the
 * controller: the address first, the flags with the ownership last.
 * The caller holds the lock.
 */
static void
arm_receive_descriptor(
	struct sungem_unit *unit,
	unsigned slot)
{
	uint64_t address;
	uint64_t flags;

	/* The slot's address, its size and the controller's ownership. */
	address = unit->rx.device_address + (uint64_t)slot * SUNGEM_PACKET_BYTES;
	flags = (((uint64_t)SUNGEM_PACKET_BYTES << GEM_RD_BUFSHIFT) & GEM_RD_BUFSIZE) | GEM_RD_OWN;
	write_descriptor(unit->rx_descriptors + slot * SUNGEM_DESCRIPTOR_WORDS, flags, address);
}

/*
 * Delivers received frames within the network worker's budget: those
 * before the index the receive completion register reports, each
 * descriptor handed back as its frame is copied out, and the kick moved
 * four descriptors at a time.
 *
 * Procedure referenced: FreeBSD sys/dev/gem/if_gem.c, release/14.3.0
 * (gem_rint(): the completion register read, a descriptor still owned by
 * the controller ends the pass, a bad CRC dropped, the kick written
 * when the pointer reaches a multiple of four, four behind it).
 */
static unsigned
receive_packets(
	struct net_device *net,
	unsigned budget)
{
	struct sungem_unit *unit;
	struct packet_buf *packet;
	uint64_t flags;
	unsigned long irq;
	unsigned completion;
	unsigned count;
	unsigned slot;
	size_t length;
	void *payload;
	int valid;

	/* Takes a finite batch so one interface cannot starve the stack. */
	unit = net->driver_data;
	for (count = 0; count < budget; count++) {
		/* Takes a stack packet before the lock. */
		packet = packet_buf_alloc(PACKET_BUF_DEFAULT_HEADROOM);
		valid = 0;
		irq = spin_lock_irqsave(&unit->lock);

		/* Stops on a closed interface and at the slot the controller has not filled. */
		slot = unit->rx_next;
		completion = 0;
		if (unit->opened != 0)
			completion = read_register(unit, GEM_RX_COMPLETION) % SUNGEM_RING_COUNT;
		if (unit->opened == 0 || slot == completion) {
			spin_unlock_irqrestore(&unit->lock, irq);
			packet_buf_free(packet);
			break;
		}

		/* Stops at a descriptor the controller has not given back yet. */
		kern_io_read_barrier();
		flags = read_descriptor_flags(unit->rx_descriptors + slot * SUNGEM_DESCRIPTOR_WORDS);
		if ((flags & GEM_RD_OWN) != 0) {
			spin_unlock_irqrestore(&unit->lock, irq);
			packet_buf_free(packet);
			break;
		}
		length = (size_t)((flags & GEM_RD_BUFSIZE) >> GEM_RD_BUFSHIFT);

		/* Copies a sound frame of a sensible length out of the slot. */
		if ((flags & GEM_RD_BAD_CRC) == 0 &&
		    length >= SUNGEM_HEADER_BYTES &&
		    length <= SUNGEM_MAX_FRAME + 4U) {
			/* Appends the frame to the stack's packet. */
			if (packet != NULL) {
				payload = packet_buf_append(packet, length);
				if (payload != NULL) {
					kern_memcpy(payload, (const uint8_t *)unit->rx.address + (size_t)slot * SUNGEM_PACKET_BYTES, length);
					valid = 1;
				}
			}
		} else {
			/* Counts a broken frame; its slot goes back all the same. */
			net->rx_errors++;
		}

		/* Counts a frame dropped for want of a packet. */
		if (valid == 0)
			net->rx_dropped++;

		/* Gives the slot back after the copy, and moves to the next. */
		arm_receive_descriptor(unit, slot);
		unit->rx_next = (slot + 1U) % SUNGEM_RING_COUNT;

		/* Offers the controller each batch of four the driver has given back. */
		if (unit->rx_next % SUNGEM_KICK_BATCH == 0) {
			kern_io_write_barrier();
			write_register(unit, GEM_RX_KICK, (unit->rx_next + SUNGEM_RING_COUNT - SUNGEM_KICK_BATCH) % SUNGEM_RING_COUNT);
		}

		spin_unlock_irqrestore(&unit->lock, irq);

		/* Hands the packet to the stack outside the lock. */
		if (valid != 0) {
			net_device_receive(net, packet);
		} else {
			packet_buf_free(packet);
		}
	}

	/* Succeeded: the slots consumed, broken and dropped frames included. */
	return count;
}

/*
 * Serves the link, a receiver to restart, sent frames and received
 * frames, then takes the receive interrupts again.
 */
static unsigned
sungem_poll(
	struct net_device *net,
	unsigned budget)
{
	struct sungem_unit *unit;
	unsigned long irq;
	unsigned link;
	unsigned count;

	/* Samples the link, restarts a stuck receiver and takes back sent slots under the lock. */
	unit = net->driver_data;
	irq = spin_lock_irqsave(&unit->lock);

	/* A closed interface has no link to follow and no slots in flight. */
	if (unit->opened != 0) {
		update_link(unit);

		/* Restarts a receive engine the interrupt found stuck, with its ring from the start. */
		if (unit->rx_reset_wanted != 0) {
			unit->rx_reset_wanted = 0;
			reset_receiver(unit);
			start_receiver(unit);
		}

		reclaim_transmitted(unit);
	}

	/* Takes the link for the announcement outside the lock. */
	link = unit->link;

	spin_unlock_irqrestore(&unit->lock, irq);

	/* Announces the carrier, then delivers a batch of frames. */
	(void)net_device_set_carrier(net, (int)link);
	count = receive_packets(net, budget);

	/* Takes the receive interrupts again after the batch. */
	irq = spin_lock_irqsave(&unit->lock);

	/* A closed or polled interface keeps its interrupts off. */
	if (unit->opened != 0 && unit->has_irq != 0)
		write_register(unit, GEM_INTMASK, ~SUNGEM_INTERRUPTS);

	spin_unlock_irqrestore(&unit->lock, irq);

	/* Succeeded: the frames handled. */
	return count;
}

/*
 * Serves the controller's interrupt: reads the status, which clears its
 * latched causes, holds the receive interrupts off until the network
 * worker has drained the ring, and notes what the worker must repair.
 * Reports whether the interrupt was the controller's.
 *
 * Procedure referenced: FreeBSD sys/dev/gem/if_gem.c, release/14.3.0
 * (gem_intr() and gem_eint(): a tag error or a receive overflow resets
 * the receive engine, a bus or parity error is reported with the PCI
 * error status).
 */
static int
sungem_interrupt(
	void *argument)
{
	struct sungem_unit *unit;
	unsigned long enabled;
	uint32_t status;
	uint32_t mac_status;
	uint32_t pci_error;
	unsigned schedule;
	unsigned ours;

	/* Reads and clears the latched causes. */
	unit = argument;
	schedule = 0;
	ours = 0;
	enabled = spin_lock_irqsave(&unit->lock);

	status = read_register(unit, GEM_STATUS);

	/* A controller gone from the bus, or with nothing the driver serves, did not interrupt. */
	if (status != SUNGEM_GONE &&
	    (status & (SUNGEM_INTERRUPTS | GEM_INTR_TX_MAC | GEM_INTR_RX_MAC)) != 0)
		ours = 1;

	/* Holds the receive interrupts until the worker drains a batch. */
	if (ours != 0 && unit->opened != 0 && (status & SUNGEM_RX_INTERRUPTS) != 0) {
		write_register(unit, GEM_INTMASK, ~(SUNGEM_INTERRUPTS & ~SUNGEM_RX_INTERRUPTS));
		schedule = 1;
	}

	/* Has the worker take back sent slots. */
	if (ours != 0 && (status & (GEM_INTR_TX_EMPTY | GEM_INTR_TX_INTME)) != 0)
		schedule = 1;

	/* Has the worker restart a receive engine that lost its framing. */
	if (ours != 0 && (status & GEM_INTR_RX_TAG_ERR) != 0) {
		unit->net->rx_errors++;
		unit->rx_reset_wanted = 1;
		schedule = 1;
	}

	/* Has the worker restart a receive engine whose FIFO overflowed, which may hang it. */
	if (ours != 0 && (status & GEM_INTR_RX_MAC) != 0) {
		mac_status = read_register(unit, GEM_MAC_RX_STATUS);
		if ((mac_status & GEM_MAC_RX_OVERFLOW) != 0) {
			unit->net->rx_errors++;
			unit->rx_reset_wanted = 1;
			schedule = 1;
		}
	}

	/* Counts a frame the transmit MAC could not send. */
	if (ours != 0 && (status & GEM_INTR_TX_MAC) != 0) {
		mac_status = read_register(unit, GEM_MAC_TX_STATUS);
		if ((mac_status & (GEM_MAC_TX_UNDERRUN | GEM_MAC_TX_PKT_TOO_LONG)) != 0)
			unit->net->tx_errors++;
	}

	/* Reports a bus or parity error once, with the PCI error status. */
	if (ours != 0 && (status & (GEM_INTR_PERR | GEM_INTR_BERR)) != 0 && unit->bus_error_seen == 0) {
		unit->bus_error_seen = 1;
		pci_error = read_register(unit, GEM_PCI_ERROR_STATUS);
		kern_logf("sungem: %s bus error, status %08x, PCI error %08x\n", unit->name, status, pci_error);
	}

	spin_unlock_irqrestore(&unit->lock, enabled);

	/* Wakes the network worker outside the lock. */
	if (schedule != 0)
		net_device_schedule_poll(unit->net);

	/* Reports whether the line was the controller's. */
	if (ours == 0)
		return 0;

	/* The interrupt was the controller's. */
	return 1;
}

/*
 * Starts the periodic service once, the first time the interface opens;
 * it runs for as long as the kernel.
 */
static void
start_service(
	struct sungem_unit *unit)
{
	int error;

	/* Leaves a running service alone. */
	if (unit->poll_thread != NULL)
		return;

	/* Creates the service thread. */
	error = kthread_create(poll_worker, unit, SCHED_PRIORITY_DEFAULT, &unit->poll_thread);
	if (error != 0) {
		unit->poll_thread = NULL;
		kern_logf("sungem: %s periodic service unavailable (%d)\n", unit->name, error);
		return;
	}

	/* Holds the interface for the thread, which runs for as long as the kernel. */
	net_device_ref(unit->net);

	/* Makes the thread runnable. */
	thread_start(unit->poll_thread);
}

/*
 * Schedules a service pass at the polling interval while the interface
 * is open: the link, missed interrupts, and the frames of a controller
 * without an interrupt.
 */
static void
poll_worker(
	void *argument)
{
	struct sungem_unit *unit;
	unsigned long irq;
	unsigned opened;
	uint64_t deadline;

	/* Serves the interface for as long as the kernel runs. */
	unit = argument;
	for (;;) {
		/* Samples whether the interface is open. */
		irq = spin_lock_irqsave(&unit->lock);

		opened = unit->opened;

		spin_unlock_irqrestore(&unit->lock, irq);

		/* Wakes the network worker while the interface has users. */
		if (opened != 0)
			net_device_schedule_poll(unit->net);

		/* Sleeps on scheduler ticks until the next pass. */
		deadline = sched_ticks() + kern_ms_to_ticks(SUNGEM_POLL_MS);
		sched_sleep(deadline);
	}
}

/* Reads one PHY register through the management interface. */
static int
mdio_read(
	void *context,
	unsigned phy,
	unsigned reg,
	uint16_t *data)
{
	uint32_t frame;
	uint32_t result;
	int error;

	/* Runs a read frame. */
	frame = GEM_MIF_FRAME_READ;
	frame |= (phy & SUNGEM_PHY_ADDRESS_MAX) << GEM_MIF_PHY_SHIFT;
	frame |= (reg & 0x1fU) << GEM_MIF_REG_SHIFT;
	error = mif_frame(context, frame, &result);
	if (error != 0)
		return error;

	/* Succeeded: the register's value. */
	*data = (uint16_t)(result & GEM_MIF_FRAME_DATA);
	return 0;
}

/* Writes one PHY register through the management interface. */
static int
mdio_write(
	void *context,
	unsigned phy,
	unsigned reg,
	uint16_t data)
{
	uint32_t frame;
	uint32_t result;
	int error;

	/* Runs a write frame. */
	frame = GEM_MIF_FRAME_WRITE;
	frame |= (phy & SUNGEM_PHY_ADDRESS_MAX) << GEM_MIF_PHY_SHIFT;
	frame |= (reg & 0x1fU) << GEM_MIF_REG_SHIFT;
	frame |= data;
	error = mif_frame(context, frame, &result);
	if (error != 0)
		return error;

	/* Succeeded: the PHY holds the value. */
	return 0;
}

/*
 * Runs one management frame and waits for its turnaround bit.
 *
 * Procedure referenced: FreeBSD sys/dev/gem/if_gem.c, release/14.3.0
 * (gem_mii_readreg() and gem_mii_writereg(): the frame written, then
 * GEM_MIF_FRAME_TA0 waited for).
 */
static int
mif_frame(
	struct sungem_unit *unit,
	uint32_t frame,
	uint32_t *result)
{
	int error;

	/* Starts the frame. */
	write_register(unit, GEM_MIF_FRAME, frame);

	/* Waits for it to end. */
	error = wait_register(unit, GEM_MIF_FRAME, GEM_MIF_FRAME_TA0, GEM_MIF_FRAME_TA0, SUNGEM_MIF_TIMEOUT_US);
	if (error != 0)
		return error;

	/* Succeeded: the frame with the PHY's answer. */
	*result = read_register(unit, GEM_MIF_FRAME);
	return 0;
}

/*
 * Waits until the masked bits of a register read as wanted, spinning on
 * the monotonic counter.  It never yields: the callers hold the device
 * lock with interrupts off, or run before the scheduler.
 */
static int
wait_register(
	struct sungem_unit *unit,
	unsigned offset,
	uint32_t mask,
	uint32_t wanted,
	unsigned microseconds)
{
	uint64_t start;
	uint64_t now;
	uint64_t frequency;
	uint64_t duration;
	uint32_t bits;
	unsigned reads;
	bool counted;

	/* Fixes the deadline once, from one time base. */
	counted = kern_rtc_read_counter(&start, &frequency);
	if (!counted || frequency == 0)
		return EIO;
	duration = frequency * microseconds / 1000000U + 1U;

	/* Bounds the reads as well, in case the counter stops. */
	for (reads = 0; reads < SUNGEM_WAIT_MAX_READS; reads++) {
		/* Stops once the register shows the wanted state. */
		bits = read_register(unit, offset);
		if ((bits & mask) == wanted)
			return 0;

		/* Refuses an unavailable time base. */
		counted = kern_rtc_read_counter(&now, &frequency);
		if (!counted)
			return EIO;

		/* Reports a register that kept its state past the deadline. */
		if (now - start >= duration)
			return ETIMEDOUT;
	}

	/* Reports a wait that ran out of reads. */
	return ETIMEDOUT;
}

/*
 * Spins for at least the given time on the monotonic counter; without a
 * counter it returns at once.
 */
static void
wait_microseconds(
	unsigned microseconds)
{
	uint64_t start;
	uint64_t now;
	uint64_t frequency;
	uint64_t duration;
	bool counted;

	/* The start and the length of the wait in counter ticks. */
	counted = kern_rtc_read_counter(&start, &frequency);
	if (!counted || frequency == 0)
		return;
	duration = frequency * microseconds / 1000000U + 1U;

	/* Spins until the counter has moved that far. */
	for (;;) {
		counted = kern_rtc_read_counter(&now, &frequency);
		if (!counted)
			return;
		if (now - start >= duration)
			return;
	}
}

/* Reads one controller register, little-endian on the bus. */
static uint32_t
read_register(
	struct sungem_unit *unit,
	unsigned offset)
{
	uint32_t raw;

	/* Reads it through the uncached mapping. */
	raw = kern_mmio_read32(unit->registers + offset);

	/* Succeeded: the register's value in the processor's order. */
	return to_le32(raw);
}

/* Writes one controller register, little-endian on the bus. */
static void
write_register(
	struct sungem_unit *unit,
	unsigned offset,
	uint32_t value)
{
	/* Writes it through the uncached mapping, in order with other device accesses. */
	kern_mmio_write32(unit->registers + offset, to_le32(value));
}

/*
 * Writes a descriptor's two little-endian 64-bit words: the address
 * first, then the flags' upper half, then their lower half, which holds
 * a receive descriptor's ownership bit, so the controller never sees the
 * ownership before the rest.
 */
static void
write_descriptor(
	volatile uint32_t *descriptor,
	uint64_t flags,
	uint64_t address)
{
	/* The buffer's address, its lower half in the first word. */
	descriptor[2] = to_le32((uint32_t)address);
	descriptor[3] = to_le32((uint32_t)(address >> 32));
	kern_io_write_barrier();

	/* The flags' upper half, then the lower half with the ownership. */
	descriptor[1] = to_le32((uint32_t)(flags >> 32));
	kern_io_write_barrier();
	descriptor[0] = to_le32((uint32_t)flags);
}

/* Reads a descriptor's little-endian flags. */
static uint64_t
read_descriptor_flags(
	const volatile uint32_t *descriptor)
{
	uint64_t low;
	uint64_t high;

	/* The lower half, which holds the ownership, then the upper half. */
	low = to_le32(descriptor[0]);
	kern_io_read_barrier();
	high = to_le32(descriptor[1]);

	/* Succeeded: the flags in the processor's order. */
	return (high << 32) | low;
}

/* Converts a word between the processor's order and little-endian, which are the same or reversed. */
static uint32_t
to_le32(
	uint32_t value)
{
	/* A big-endian processor reverses the bytes. */
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
	return __builtin_bswap32(value);
#else
	return value;
#endif
}
