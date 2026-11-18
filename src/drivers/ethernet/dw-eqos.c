/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The Synopsys DesignWare Ethernet QoS controller and its PHY.
 *
 * The controller moves frames between memory and the wire through one DMA
 * channel with a ring of descriptors in each direction.  Each descriptor
 * owns one fixed buffer for the life of the system, large enough for the
 * longest frame the interface takes (a jumbo frame where the packet pool
 * has jumbo packets, else 2 KiB): a transmitted
 * frame is copied into the buffer of the next free transmit descriptor,
 * unless the controller reaches the packet's own bytes (a coherent DMA and
 * a packet of the pool, whose physical address is known), when the
 * descriptor points at the packet, which the driver keeps until the
 * frame is sent; and a received frame is copied out of its buffer into a network packet
 * before the descriptor goes back to the controller.  One receive
 * descriptor is always kept back, so the ring's tail never meets its
 * head.  No checksum, segmentation or VLAN offload is used.
 *
 * The descriptors and buffers come from the DMA allocator as coherent
 * memory: cached memory the controller snoops when the board's DMA is
 * coherent, an uncached view when it is not.
 *
 * The PHY sits on the controller's MDIO bus.  It is reset and set to
 * autonegotiate at attach; afterwards only the controller's service
 * thread talks to it, every 250 ms, and moves the controller to the speed
 * and duplex the link settled on.  The same thread wakes the network
 * worker every 20 ms while the interface is open, so a lost interrupt
 * only delays a frame.
 *
 * The interrupt clears the channel's status and holds the receive and
 * transmit interrupts off until the network worker has drained the rings.
 */

#include <drivers/ethernet/dw-eqos.h>

#include <drivers/ethernet/mii-phy.h>
#include <drivers/ethernet/yt8531.h>
#include <drivers/generic/dma.h>
#include <kern/clock.h>
#include <kern/device-io.h>
#include <kern/irq.h>
#include <kern/kcrt.h>
#include <kern/klog.h>
#include <kern/lock.h>
#include <kern/net/net-device.h>
#include <kern/net/packet-buf.h>
#include <kern/random.h>
#include <kern/sched.h>
#include <kern/thread.h>
#include <uapi/errno.h>

#include <stdbool.h>

/*
 * The MAC's registers and fields.
 *
 * Values referenced: OpenBSD sys/dev/ic/dwqereg.h, master
 * 859883f17cb8e20088fece5398547e4264ad30b3 (GMAC_MAC_CONF,
 * GMAC_MAC_CONF_JE, GMAC_MAC_PACKET_FILTER, GMAC_MAC_HASH_TAB_REG0/1, GMAC_RXQ_CTRL0,
 * GMAC_INT_STATUS, GMAC_INT_EN, GMAC_MAC_1US_TIC_CTR, GMAC_VERSION,
 * GMAC_MAC_HW_FEATURE, GMAC_MAC_ADDR0_HI/LO, GMAC_MMC_RX/TX_INT_MASK and
 * their bits).
 */
#define EQOS_MAC_CONFIG			0x0000U
#define EQOS_MAC_CONFIG_RX		0x00000001U
#define EQOS_MAC_CONFIG_TX		0x00000002U
#define EQOS_MAC_CONFIG_FULL_DUPLEX	0x00002000U
#define EQOS_MAC_CONFIG_JUMBO		0x00010000U
#define EQOS_MAC_CONFIG_FAST		0x00004000U
#define EQOS_MAC_CONFIG_MII		0x00008000U
#define EQOS_MAC_CONFIG_SPEED_MASK	0x0000c000U
#define EQOS_MAC_FILTER			0x0008U
#define EQOS_MAC_FILTER_ALL_MULTICAST	0x00000010U
#define EQOS_MAC_HASH_LOW		0x0010U
#define EQOS_MAC_HASH_HIGH		0x0014U
#define EQOS_MAC_RX_QUEUES		0x00a0U
#define EQOS_MAC_RX_QUEUE0_GENERIC	0x00000002U
#define EQOS_MAC_INTERRUPT_STATUS	0x00b0U
#define EQOS_MAC_INTERRUPT_ENABLE	0x00b4U
#define EQOS_MAC_MICROSECOND_TICKS	0x00dcU
#define EQOS_MAC_VERSION		0x0110U
#define EQOS_MAC_VERSION_SYNOPSYS	0x000000ffU
#define EQOS_MAC_FEATURE		0x011cU
#define EQOS_MAC_ADDRESS_HIGH		0x0300U
#define EQOS_MAC_ADDRESS_LOW		0x0304U
#define EQOS_MMC_RX_INTERRUPT_MASK	0x070cU
#define EQOS_MMC_TX_INTERRUPT_MASK	0x0710U

/*
 * The fields of the second feature register.
 *
 * Values referenced: OpenBSD sys/dev/ic/dwqereg.h, master
 * 859883f17cb8e20088fece5398547e4264ad30b3
 * (GMAC_MAC_HW_FEATURE1_ADDR64_*, GMAC_MAC_HW_FEATURE1_TXFIFOSIZE,
 * GMAC_MAC_HW_FEATURE1_RXFIFOSIZE).
 */
#define EQOS_FEATURE1_ADDRESS_SHIFT	14U
#define EQOS_FEATURE1_ADDRESS_MASK	0x3U
#define EQOS_FEATURE1_ADDRESS_40	1U
#define EQOS_FEATURE1_ADDRESS_48	2U
#define EQOS_FEATURE1_TX_FIFO_SHIFT	6U
#define EQOS_FEATURE1_RX_FIFO_SHIFT	0U
#define EQOS_FEATURE1_FIFO_MASK		0x1fU

/*
 * The MDIO registers and fields, and the clock ranges of the management
 * clock divider.
 *
 * Values referenced: OpenBSD sys/dev/ic/dwqereg.h, master
 * 859883f17cb8e20088fece5398547e4264ad30b3 (GMAC_MAC_MDIO_ADDR,
 * GMAC_MAC_MDIO_DATA and the GMAC_MAC_MDIO_ADDR_* fields and
 * GMAC_MAC_MDIO_ADDR_CR_* ranges).
 */
#define EQOS_MDIO_ADDRESS		0x0200U
#define EQOS_MDIO_PHY_SHIFT		21U
#define EQOS_MDIO_REGISTER_SHIFT	16U
#define EQOS_MDIO_RANGE_SHIFT		8U
#define EQOS_MDIO_READ			0x0000000cU
#define EQOS_MDIO_WRITE			0x00000004U
#define EQOS_MDIO_BUSY			0x00000001U
#define EQOS_MDIO_DATA			0x0204U
#define EQOS_MDIO_RANGE_60_100		0U
#define EQOS_MDIO_RANGE_100_150		1U
#define EQOS_MDIO_RANGE_20_35		2U
#define EQOS_MDIO_RANGE_35_60		3U
#define EQOS_MDIO_RANGE_150_250		4U
#define EQOS_MDIO_RANGE_250_300		5U
#define EQOS_MDIO_RANGE_300_500		6U
#define EQOS_MDIO_RANGE_500_800		7U

/*
 * The transmit and receive queues' operation modes.
 *
 * Values referenced: OpenBSD sys/dev/ic/dwqereg.h, master
 * 859883f17cb8e20088fece5398547e4264ad30b3
 * (GMAC_MTL_CHAN_TX_OP_MODE, GMAC_MTL_CHAN_RX_OP_MODE and their fields).
 */
#define EQOS_MTL_TX_MODE		0x0d00U
#define EQOS_MTL_TX_SIZE_SHIFT		16U
#define EQOS_MTL_TX_SIZE_MASK		0x01ff0000U
#define EQOS_MTL_TX_THRESHOLD_MASK	0x00000070U
#define EQOS_MTL_TX_THRESHOLD_512	0x00000070U
#define EQOS_MTL_TX_QUEUE_MASK		0x0000000cU
#define EQOS_MTL_TX_QUEUE_ENABLED	0x00000008U
#define EQOS_MTL_TX_STORE_FORWARD	0x00000002U
#define EQOS_MTL_TX_FLUSH		0x00000001U
#define EQOS_MTL_RX_MODE		0x0d30U
#define EQOS_MTL_RX_SIZE_SHIFT		20U
#define EQOS_MTL_RX_SIZE_MASK		0x3ff00000U
#define EQOS_MTL_RX_STORE_FORWARD	0x00000020U
#define EQOS_MTL_RX_THRESHOLD_MASK	0x00000018U
#define EQOS_MTL_RX_THRESHOLD_128	0x00000018U

/*
 * The DMA's registers and fields, and its first channel's.
 *
 * Values referenced: OpenBSD sys/dev/ic/dwqereg.h, master
 * 859883f17cb8e20088fece5398547e4264ad30b3 (GMAC_BUS_MODE,
 * GMAC_SYS_BUS_MODE, GMAC_CHAN_* and their bits).
 */
#define EQOS_DMA_MODE			0x1000U
#define EQOS_DMA_MODE_RESET		0x00000001U
#define EQOS_DMA_BUS			0x1004U
#define EQOS_DMA_BUS_LPI		0x80000000U
#define EQOS_DMA_BUS_WRITE_LIMIT_SHIFT	24U
#define EQOS_DMA_BUS_WRITE_LIMIT_MASK	0x0f000000U
#define EQOS_DMA_BUS_READ_LIMIT_SHIFT	16U
#define EQOS_DMA_BUS_READ_LIMIT_MASK	0x000f0000U
#define EQOS_DMA_BUS_MIXED_BURST	0x00004000U
#define EQOS_DMA_BUS_ALIGNED		0x00001000U
#define EQOS_DMA_BUS_EXTENDED_ADDRESS	0x00000800U
#define EQOS_DMA_BUS_BURST_256		0x00000080U
#define EQOS_DMA_BUS_BURST_128		0x00000040U
#define EQOS_DMA_BUS_BURST_64		0x00000020U
#define EQOS_DMA_BUS_BURST_32		0x00000010U
#define EQOS_DMA_BUS_BURST_16		0x00000008U
#define EQOS_DMA_BUS_BURST_8		0x00000004U
#define EQOS_DMA_BUS_BURST_4		0x00000002U
#define EQOS_DMA_BUS_BURST_MASK		0x000000feU
#define EQOS_DMA_BUS_FIXED_BURST	0x00000001U
#define EQOS_CHANNEL_CONTROL		0x1100U
#define EQOS_CHANNEL_CONTROL_BURST_X8	0x00010000U
#define EQOS_CHANNEL_TX_CONTROL		0x1104U
#define EQOS_CHANNEL_RX_CONTROL		0x1108U
#define EQOS_CHANNEL_BURST_SHIFT	16U
#define EQOS_CHANNEL_BURST_MASK		0x003f0000U
#define EQOS_CHANNEL_TX_SECOND_FRAME	0x00000010U
#define EQOS_CHANNEL_START		0x00000001U
#define EQOS_CHANNEL_TX_BASE_HIGH	0x1110U
#define EQOS_CHANNEL_TX_BASE_LOW	0x1114U
#define EQOS_CHANNEL_RX_BASE_HIGH	0x1118U
#define EQOS_CHANNEL_RX_BASE_LOW	0x111cU
#define EQOS_CHANNEL_TX_TAIL		0x1120U
#define EQOS_CHANNEL_RX_TAIL		0x1128U
#define EQOS_CHANNEL_TX_RING_LENGTH	0x112cU
#define EQOS_CHANNEL_RX_RING_LENGTH	0x1130U
#define EQOS_CHANNEL_INTERRUPT_ENABLE	0x1134U
#define EQOS_CHANNEL_STATUS		0x1160U
#define EQOS_CHANNEL_NORMAL		0x00008000U
#define EQOS_CHANNEL_ABNORMAL		0x00004000U
#define EQOS_CHANNEL_BUS_ERROR		0x00001000U
#define EQOS_CHANNEL_RX_UNAVAILABLE	0x00000080U
#define EQOS_CHANNEL_RX_DONE		0x00000040U
#define EQOS_CHANNEL_TX_DONE		0x00000001U

/*
 * The receive channel's buffer size field.
 *
 * Values referenced: Linux v6.12
 * drivers/net/ethernet/stmicro/stmmac/dwmac4_dma.h (DMA_RBSZ_MASK,
 * DMA_RBSZ_SHIFT).
 */
#define EQOS_CHANNEL_RX_BUFFER_SHIFT	1U
#define EQOS_CHANNEL_RX_BUFFER_MASK	0x00007ffeU

/*
 * The descriptor words' bits.
 *
 * Values referenced: OpenBSD sys/dev/ic/dwqereg.h, master
 * 859883f17cb8e20088fece5398547e4264ad30b3 (TDES2_IC, TDES3_OWN,
 * TDES3_FS, TDES3_LS, TDES3_ES, RDES3_OWN, RDES3_IC, RDES3_BUF1V,
 * RDES3_LENGTH, RDES3_ES, RDES3_LD, RDES3_FD, RDES3_CTXT).
 */
#define EQOS_DESCRIPTOR_OWN		0x80000000U
#define EQOS_TX_INTERRUPT		0x80000000U
#define EQOS_TX_FIRST			0x20000000U
#define EQOS_TX_LAST			0x10000000U
#define EQOS_TX_ERROR			0x00008000U
#define EQOS_RX_INTERRUPT		0x40000000U
#define EQOS_RX_BUFFER1_VALID		0x01000000U
#define EQOS_RX_CONTEXT			0x40000000U
#define EQOS_RX_FIRST			0x20000000U
#define EQOS_RX_LAST			0x10000000U
#define EQOS_RX_ERROR			0x00008000U
#define EQOS_RX_LENGTH_MASK		0x00007fffU

/* The channel interrupts while the interface runs, and while a drain is pending. */
#define EQOS_INTERRUPTS_RUNNING		(EQOS_CHANNEL_NORMAL | EQOS_CHANNEL_ABNORMAL | \
					 EQOS_CHANNEL_BUS_ERROR | EQOS_CHANNEL_RX_UNAVAILABLE | \
					 EQOS_CHANNEL_RX_DONE | EQOS_CHANNEL_TX_DONE)
#define EQOS_INTERRUPTS_DRAINING	(EQOS_CHANNEL_ABNORMAL | EQOS_CHANNEL_BUS_ERROR)

/*
 * The rings' geometry and the frames that fit in one buffer.  A frame is
 * its Ethernet header, a VLAN tag and the MTU (EQOS_FRAME_OVERHEAD), and
 * the controller writes its CRC after it into the receive buffer.  Where
 * the pool has jumbo packets the buffers hold a 9000-byte MTU's frame and
 * the MAC takes jumbo frames (the controller's limit with JE is 9018
 * bytes untagged); the interface still starts at 1500.
 */
#define EQOS_TX_COUNT			128U
#define EQOS_RX_COUNT			128U
#define EQOS_DESCRIPTOR_BYTES		16U
#if NET_MTU_MAX > 1500U
#define EQOS_BUFFER_BYTES		9216U
#else
#define EQOS_BUFFER_BYTES		2048U
#endif
#define EQOS_BUFFER_ALIGNMENT		4096U
#define EQOS_MIN_FRAME			60U
#define EQOS_FRAME_OVERHEAD		18U
#define EQOS_DEFAULT_MTU		1500U
#define EQOS_HEADER_BYTES		14U
#define EQOS_CRC_BYTES			4U

/* The bounded waits and the service intervals. */
#define EQOS_RESET_TIMEOUT_US		300000U
#define EQOS_MDIO_TIMEOUT_US		20000U
#define EQOS_FLUSH_TIMEOUT_US		10000U
#define EQOS_POLL_MS			20U
#define EQOS_LINK_MS			250U

/* How many controllers the driver serves. */
#define EQOS_MAX_DEVICES		2U

/* Marks a parameter that the interrupt layer passes but the handler does not need. */
#define UNUSED_PARAMETER(parameter)	((void)(parameter))

/*
 * One DMA descriptor as the controller reads and writes it.
 *
 * The CPU fills the first three words and then hands the descriptor over
 * by writing the fourth with the owner bit; the controller hands it back
 * by clearing the bit in its write-back.
 */
struct eqos_descriptor {
	volatile uint32_t word0;
	volatile uint32_t word1;
	volatile uint32_t word2;
	volatile uint32_t word3;
};

/*
 * One attached controller.
 *
 * The lock serializes the rings, the indices, the open state, the link
 * state the MAC is set to, and every register write after attach; the
 * interrupt takes it too.  The MDIO bus is used only by attach and then
 * by the service thread, so it needs no lock.  An attached controller is
 * kept for the life of the system.
 */
struct eqos_device {
	struct spinlock lock;
	struct drv_dw_eqos_config config;
	struct drv_mii_bus mdio;
	volatile uint8_t *registers;
	uint32_t mdio_range;
	uint32_t features[4];
	unsigned address_bits;
	/*
	 * The largest MTU the interface takes and the longest frame (without
	 * its CRC) that goes with it, fixed at attach: the buffers' size, and
	 * in store-and-forward mode the transmit FIFO, which must hold a
	 * whole frame.
	 */
	unsigned mtu_max;
	size_t frame_max;
	int phy_is_yt8531;
	struct drv_dma_device *dma;
	struct drv_dma_buffer descriptors;
	struct drv_dma_buffer tx_buffers;
	/*
	 * The packets the transmit descriptors point at in place, by slot (NULL
	 * for a frame copied into the slot's buffer), and the ones the
	 * controller has finished, linked by next, which the caller of
	 * reclaim_tx frees once the lock is released.  Guarded by the lock.
	 */
	struct packet_buf *tx_packets[EQOS_TX_COUNT];
	struct packet_buf *tx_done;
	struct drv_dma_buffer rx_buffers;
	struct eqos_descriptor *tx_ring;
	struct eqos_descriptor *rx_ring;
	uint64_t tx_ring_address;
	uint64_t rx_ring_address;
	unsigned tx_producer;
	unsigned tx_consumer;
	unsigned rx_producer;
	unsigned rx_consumer;
	struct net_device *net;
	struct thread *poll_thread;
	unsigned opened;
	unsigned irq_registered;
	unsigned speed;
	unsigned full_duplex;
	unsigned mdio_fault;
	unsigned bus_error;
	unsigned bus_error_reported;
	uint8_t mac[6];
};

/*
 * The attached controllers.
 *
 * A slot whose registers are NULL is free.  Slots are taken by attach on
 * the boot thread and never given back once the interface is published.
 */
static struct eqos_device eqos_devices[EQOS_MAX_DEVICES];

static int eqos_open(struct net_device *net);
static void eqos_close(struct net_device *net);
static int eqos_transmit(struct net_device *net, struct packet_buf *packet);
static unsigned eqos_poll(struct net_device *net, unsigned budget);

/* The network callbacks; the buffers stay with the controller across closes. */
static const struct net_device_ops eqos_operations = {
	eqos_open,
	eqos_close,
	eqos_transmit,
	eqos_poll,
	NULL,
	NULL,
	NULL
};

static uint32_t read_register(const struct eqos_device *device, uint32_t offset);
static void write_register(struct eqos_device *device, uint32_t offset, uint32_t value);
static int wait_register_clear(const struct eqos_device *device, uint32_t offset, uint32_t mask, uint64_t microseconds);
static int start_deadline(uint64_t microseconds, uint64_t *deadline);
static int deadline_passed(uint64_t deadline);
static int mdio_read(void *context, unsigned phy, unsigned reg, uint16_t *data);
static int mdio_write(void *context, unsigned phy, unsigned reg, uint16_t data);
static uint32_t mdio_range_for(uint64_t rate);
static void choose_mtu_max(struct eqos_device *device);
static int choose_mac(struct eqos_device *device);
static int reset_controller(struct eqos_device *device);
static void configure_bus(struct eqos_device *device);
static uint32_t burst_bit(uint32_t length);
static int bring_up_phy(struct eqos_device *device);
static int allocate_rings(struct eqos_device *device);
static void release_unpublished(struct eqos_device *device);
static int publish_interface(struct eqos_device *device);
static void start_service(struct eqos_device *device);
static void prepare_rings(struct eqos_device *device);
static void arm_rx_slot(struct eqos_device *device, unsigned slot);
static void configure_queues(struct eqos_device *device);
static void apply_link(struct eqos_device *device);
static unsigned reclaim_tx(struct eqos_device *device);
static struct packet_buf *take_tx_done_locked(struct eqos_device *device);
static void free_tx_done(struct packet_buf *list);
static unsigned receive_frames(struct net_device *net, unsigned budget);
static void report_tx_errors(struct net_device *net, unsigned errors);
static void eqos_interrupt(int irq, kern_irq_ack_t acknowledge, void *argument);
static void service_worker(void *argument);
static void check_link(struct eqos_device *device);

/*
 * Resets a controller, brings up its PHY and publishes it as a network
 * interface.
 *
 * Procedure referenced: OpenBSD sys/dev/ic/dwqe.c, master
 * 859883f17cb8e20088fece5398547e4264ad30b3 (dwqe_attach, dwqe_reset);
 * OpenBSD sys/dev/fdt/if_dwqe_fdt.c (dwqe_fdt_attach).
 */
int
drv_dw_eqos_attach(
	const struct drv_dw_eqos_config *config)
{
	struct eqos_device *device;
	const char *stage;
	uint32_t version;
	unsigned index;
	int error;

	/* Refuses a missing description, or one without registers or a name. */
	if (config == NULL ||
	    config->registers == NULL ||
	    config->name == NULL)
		return EINVAL;

	/* Refuses a PHY address beyond the MDIO bus's 32. */
	if (config->phy_address > 31U)
		return EINVAL;

	/* Finds a free slot; a controller already attached keeps its slot. */
	device = NULL;
	for (index = 0; index < EQOS_MAX_DEVICES; index++) {
		/* Refuses a second attach of the same registers. */
		if (eqos_devices[index].registers == config->registers)
			return EALREADY;

		/* Takes the first free slot. */
		if (device == NULL && eqos_devices[index].registers == NULL)
			device = &eqos_devices[index];
	}

	/* Refuses more controllers than the driver serves. */
	if (device == NULL)
		return ENOSPC;

	/* Takes the board's description into the slot. */
	kern_memset(device, 0, sizeof(*device));
	device->config = *config;
	device->registers = config->registers;

	/* Readies the lock that serializes the rings and the registers. */
	spin_init(&device->lock, LOCK_RANK_DEVICE, "dw-eqos");

	/* Wires the MDIO bus the PHY drivers talk through. */
	device->mdio.context = device;
	device->mdio.read = mdio_read;
	device->mdio.write = mdio_write;

	/* Reads the version; a bus with nothing behind it reads all ones or zeros. */
	stage = "version";
	version = read_register(device, EQOS_MAC_VERSION);
	kern_logf("dw-eqos: %s version %x irq %u\n", config->name, version, config->irq);
	if (version == 0 || version == 0xffffffffU) {
		error = ENODEV;
		goto fail;
	}

	/* Reads the four feature registers. */
	for (index = 0; index < 4U; index++)
		device->features[index] = read_register(device, EQOS_MAC_FEATURE + index * 4U);

	/* Decodes how wide an address the DMA can drive. */
	device->address_bits = 32U;
	switch ((device->features[1] >> EQOS_FEATURE1_ADDRESS_SHIFT) & EQOS_FEATURE1_ADDRESS_MASK) {
	case EQOS_FEATURE1_ADDRESS_40:
		device->address_bits = 40U;
		break;
	case EQOS_FEATURE1_ADDRESS_48:
		device->address_bits = 48U;
		break;
	default:
		break;
	}

	/* Works out the largest MTU the buffers and the transmit FIFO allow. */
	choose_mtu_max(device);

	/* Picks the station address before the reset clears the registers that may hold it. */
	stage = "MAC address";
	error = choose_mac(device);
	if (error != 0)
		goto fail;

	/* Resets the controller, which also stops whatever the firmware left running. */
	stage = "reset";
	error = reset_controller(device);
	if (error != 0)
		goto fail;

	/* Sets the DMA's bus behaviour and bursts from the board's description. */
	configure_bus(device);

	/* Picks the MDIO clock divider for the register clock's rate. */
	device->mdio_range = mdio_range_for(config->csr_clock_hz);

	/* Resets the PHY and starts autonegotiation. */
	stage = "PHY";
	error = bring_up_phy(device);
	if (error != 0)
		goto fail;

	/* Allocates the rings and buffers before anything can reference them. */
	stage = "DMA rings";
	error = allocate_rings(device);
	if (error != 0)
		goto fail;

	/* Holds the interrupt off at the PLIC; the controller's are off since the reset. */
	stage = "interrupt";
	kern_irq_mask((int)config->irq);

	/* Asks for the level-triggered, active-high line the controller drives. */
	error = kern_irq_set_mode((int)config->irq, KERN_IRQ_TRIGGER_LEVEL, KERN_IRQ_POLARITY_HIGH);
	if (error != 0)
		goto fail;

	/* Binds the handler; the flag tells a failed attach to unbind it. */
	error = kern_irq_register((int)config->irq, eqos_interrupt, device);
	if (error != 0)
		goto fail;

	/* Records the binding, which a failed attach undoes. */
	device->irq_registered = 1;

	/* Publishes the network interface. */
	stage = "network interface";
	error = publish_interface(device);
	if (error != 0)
		goto fail;

	/* Starts the link and missed-interrupt service. */
	start_service(device);

	/* Reports the controller ready, with its address and its DMA. */
	kern_logf("dw-eqos: %s ready, MAC %02x:%02x:%02x:%02x:%02x:%02x, DMA %u bit%s\n",
		  config->name,
		  device->mac[0],
		  device->mac[1],
		  device->mac[2],
		  device->mac[3],
		  device->mac[4],
		  device->mac[5],
		  device->address_bits,
		  config->dma_coherent ? ", coherent" : ", uncached");

	/* Succeeded: networkd can open the interface. */
	return 0;

fail:
	/* Gives back what the attach took; nothing the controller can reach is freed. */
	kern_logf("dw-eqos: %s failed at %s (%d)\n", config->name, stage, error);
	release_unpublished(device);

	/* Reports why the controller is not attached. */
	return error;
}

/* Reads one controller register. */
static uint32_t
read_register(
	const struct eqos_device *device,
	uint32_t offset)
{
	uint32_t value;

	/* Reads it with device ordering. */
	value = kern_mmio_read32(device->registers + offset);

	/* Reports the register's contents. */
	return value;
}

/* Writes one controller register. */
static void
write_register(
	struct eqos_device *device,
	uint32_t offset,
	uint32_t value)
{
	/* Writes it after every earlier store, including descriptor updates. */
	kern_mmio_write32(device->registers + offset, value);
}

/* Waits until a register's masked bits read zero. */
static int
wait_register_clear(
	const struct eqos_device *device,
	uint32_t offset,
	uint32_t mask,
	uint64_t microseconds)
{
	uint64_t deadline;
	uint32_t value;
	int passed;
	int error;

	/* Sets how long the bits may stay set. */
	error = start_deadline(microseconds, &deadline);
	if (error != 0)
		return error;

	/* Polls the register. */
	for (;;) {
		/* The controller has finished once the bits are clear. */
		value = read_register(device, offset);
		if ((value & mask) == 0)
			break;

		/* A controller that never finishes is given up on. */
		passed = deadline_passed(deadline);
		if (passed)
			return ETIMEDOUT;
	}

	/* Succeeded: the bits are clear. */
	return 0;
}

/* Sets a deadline on the monotonic counter, a number of microseconds away. */
static int
start_deadline(
	uint64_t microseconds,
	uint64_t *deadline)
{
	uint64_t frequency;
	uint64_t now;
	bool counted;

	/* Reads the monotonic counter; without one no wait can end. */
	counted = kern_rtc_read_counter(&now, &frequency);
	if (!counted || frequency == 0)
		return ENODEV;

	/* Succeeded: the counter value the wait ends at. */
	*deadline = now + frequency * microseconds / 1000000ULL;
	return 0;
}

/* Reports whether the monotonic counter has passed a deadline. */
static int
deadline_passed(
	uint64_t deadline)
{
	uint64_t frequency;
	uint64_t now;
	bool counted;

	/* A counter that cannot be read ends the wait rather than spinning forever. */
	counted = kern_rtc_read_counter(&now, &frequency);
	if (!counted)
		return 1;

	/* The deadline has passed once the counter reaches it. */
	if (now >= deadline)
		return 1;

	/* The wait goes on. */
	return 0;
}

/*
 * Reads one clause-22 register of a PHY on the controller's MDIO bus.
 *
 * Procedure referenced: OpenBSD sys/dev/ic/dwqe.c, master
 * 859883f17cb8e20088fece5398547e4264ad30b3 (dwqe_mii_readreg).
 */
static int
mdio_read(
	void *context,
	unsigned phy,
	unsigned reg,
	uint16_t *data)
{
	struct eqos_device *device;
	uint32_t command;
	int error;

	/* Refuses an address or register beyond clause 22's five bits. */
	device = context;
	if (phy > 31U || reg > 31U)
		return EINVAL;

	/* Waits out a transaction still on the bus. */
	error = wait_register_clear(device, EQOS_MDIO_ADDRESS, EQOS_MDIO_BUSY, EQOS_MDIO_TIMEOUT_US);
	if (error != 0)
		return error;

	/* Starts the read. */
	command = device->mdio_range << EQOS_MDIO_RANGE_SHIFT;
	command |= (uint32_t)phy << EQOS_MDIO_PHY_SHIFT;
	command |= (uint32_t)reg << EQOS_MDIO_REGISTER_SHIFT;
	command |= EQOS_MDIO_READ | EQOS_MDIO_BUSY;
	write_register(device, EQOS_MDIO_ADDRESS, command);

	/* Waits for the read to finish. */
	error = wait_register_clear(device, EQOS_MDIO_ADDRESS, EQOS_MDIO_BUSY, EQOS_MDIO_TIMEOUT_US);
	if (error != 0)
		return error;

	/* Succeeded: the register's sixteen bits. */
	*data = (uint16_t)read_register(device, EQOS_MDIO_DATA);
	return 0;
}

/*
 * Writes one clause-22 register of a PHY on the controller's MDIO bus.
 *
 * Procedure referenced: OpenBSD sys/dev/ic/dwqe.c, master
 * 859883f17cb8e20088fece5398547e4264ad30b3 (dwqe_mii_writereg).
 */
static int
mdio_write(
	void *context,
	unsigned phy,
	unsigned reg,
	uint16_t data)
{
	struct eqos_device *device;
	uint32_t command;
	int error;

	/* Refuses an address or register beyond clause 22's five bits. */
	device = context;
	if (phy > 31U || reg > 31U)
		return EINVAL;

	/* Waits out a transaction still on the bus. */
	error = wait_register_clear(device, EQOS_MDIO_ADDRESS, EQOS_MDIO_BUSY, EQOS_MDIO_TIMEOUT_US);
	if (error != 0)
		return error;

	/* Starts the write with the data in place. */
	write_register(device, EQOS_MDIO_DATA, data);
	command = device->mdio_range << EQOS_MDIO_RANGE_SHIFT;
	command |= (uint32_t)phy << EQOS_MDIO_PHY_SHIFT;
	command |= (uint32_t)reg << EQOS_MDIO_REGISTER_SHIFT;
	command |= EQOS_MDIO_WRITE | EQOS_MDIO_BUSY;
	write_register(device, EQOS_MDIO_ADDRESS, command);

	/* Waits for the write to finish. */
	error = wait_register_clear(device, EQOS_MDIO_ADDRESS, EQOS_MDIO_BUSY, EQOS_MDIO_TIMEOUT_US);
	if (error != 0)
		return error;

	/* Succeeded: the PHY has the value. */
	return 0;
}

/*
 * Picks the MDIO clock divider that keeps the management clock within
 * its limit for a register clock rate; an unknown rate gets the largest
 * divider, which is slow but safe.
 *
 * Procedure referenced: OpenBSD sys/dev/fdt/if_dwqe_fdt.c, master
 * 859883f17cb8e20088fece5398547e4264ad30b3 (dwqe_fdt_attach, the
 * choice of sc_clk).
 */
static uint32_t
mdio_range_for(
	uint64_t rate)
{
	/* Picks the range the rate falls in, from the fastest down. */
	if (rate == 0)
		return EQOS_MDIO_RANGE_500_800;
	if (rate > 500000000ULL)
		return EQOS_MDIO_RANGE_500_800;
	if (rate > 300000000ULL)
		return EQOS_MDIO_RANGE_300_500;
	if (rate > 250000000ULL)
		return EQOS_MDIO_RANGE_250_300;
	if (rate > 150000000ULL)
		return EQOS_MDIO_RANGE_150_250;
	if (rate > 100000000ULL)
		return EQOS_MDIO_RANGE_100_150;
	if (rate > 60000000ULL)
		return EQOS_MDIO_RANGE_60_100;
	if (rate > 35000000ULL)
		return EQOS_MDIO_RANGE_35_60;

	/* Reports the slowest clocks' range. */
	return EQOS_MDIO_RANGE_20_35;
}

/*
 * Works out the largest MTU the interface takes: what the buffers hold,
 * and in store-and-forward mode no more than the transmit FIFO holds with
 * the frame's header, tag and CRC (threshold mode streams a frame longer
 * than the FIFO).  Never below the 1500 every Ethernet port takes.
 */
static void
choose_mtu_max(
	struct eqos_device *device)
{
	uint32_t fifo;
	unsigned mtu;

	/* The buffers' limit, and the pool's. */
	mtu = EQOS_BUFFER_BYTES - EQOS_FRAME_OVERHEAD - EQOS_CRC_BYTES;
	if (mtu > NET_MTU_MAX)
		mtu = NET_MTU_MAX;

	/* The transmit FIFO's, when it must hold a whole frame. */
	if (!device->config.threshold_mode) {
		fifo = device->config.tx_fifo_bytes;
		if (fifo == 0)
			fifo = 128U << ((device->features[1] >> EQOS_FEATURE1_TX_FIFO_SHIFT) & EQOS_FEATURE1_FIFO_MASK);
		if (fifo < mtu + EQOS_FRAME_OVERHEAD + EQOS_CRC_BYTES)
			mtu = fifo - EQOS_FRAME_OVERHEAD - EQOS_CRC_BYTES;
	}

	/* No less than Ethernet's own. */
	if (mtu < EQOS_DEFAULT_MTU)
		mtu = EQOS_DEFAULT_MTU;

	/* The MTU and the frame that goes with it. */
	device->mtu_max = mtu;
	device->frame_max = (size_t)mtu + EQOS_FRAME_OVERHEAD;
}

/*
 * Picks the station address: the board's, else the one the firmware left
 * in the controller, else a random locally administered one.
 *
 * Procedure referenced: OpenBSD sys/dev/ic/dwqe.c, master
 * 859883f17cb8e20088fece5398547e4264ad30b3 (dwqe_lladdr_read).
 */
static int
choose_mac(
	struct eqos_device *device)
{
	uint32_t high;
	uint32_t low;
	unsigned nonzero;
	unsigned index;
	int error;

	/* Uses the board's unicast address when it gives one. */
	if (device->config.has_mac && (device->config.mac[0] & 1U) == 0) {
		kern_memcpy(device->mac, device->config.mac, sizeof(device->mac));
		return 0;
	}

	/* Reads the address the firmware left in the controller, lowest byte first. */
	high = read_register(device, EQOS_MAC_ADDRESS_HIGH);
	low = read_register(device, EQOS_MAC_ADDRESS_LOW);
	device->mac[0] = (uint8_t)low;
	device->mac[1] = (uint8_t)(low >> 8);
	device->mac[2] = (uint8_t)(low >> 16);
	device->mac[3] = (uint8_t)(low >> 24);
	device->mac[4] = (uint8_t)high;
	device->mac[5] = (uint8_t)(high >> 8);

	/* Notes whether any byte of it is set. */
	nonzero = 0;
	for (index = 0; index < sizeof(device->mac); index++)
		nonzero |= device->mac[index];

	/* Keeps it when it is a real unicast address rather than the reset value. */
	if (nonzero != 0 &&
	    (device->mac[0] & 1U) == 0 &&
	    !(low == 0xffffffffU && (high & 0xffffU) == 0xffffU))
		return 0;

	/* Makes up a random address as the last resort. */
	error = kern_random_read(device->mac, sizeof(device->mac), 0);
	if (error != 0)
		return ENODEV;

	/* Marks it locally administered and unicast, so it cannot clash with a vendor's. */
	device->mac[0] = (uint8_t)((device->mac[0] & 0xfeU) | 0x02U);

	/* Says the address is made up. */
	kern_logf("dw-eqos: %s has no address from the board, using a random one\n", device->config.name);

	/* Succeeded: the made-up address. */
	return 0;
}

/*
 * Resets the controller's MAC, queues and DMA to their defaults.
 *
 * The reset finishes only while the PHY supplies the receive clock.
 *
 * Procedure referenced: OpenBSD sys/dev/ic/dwqe.c, master
 * 859883f17cb8e20088fece5398547e4264ad30b3 (dwqe_reset).
 */
static int
reset_controller(
	struct eqos_device *device)
{
	uint32_t mode;
	int error;

	/* Holds every interrupt off. */
	write_register(device, EQOS_MAC_INTERRUPT_ENABLE, 0);
	write_register(device, EQOS_CHANNEL_INTERRUPT_ENABLE, 0);

	/* Starts the software reset. */
	mode = read_register(device, EQOS_DMA_MODE);
	write_register(device, EQOS_DMA_MODE, mode | EQOS_DMA_MODE_RESET);

	/* Waits for the reset bit to clear itself. */
	error = wait_register_clear(device, EQOS_DMA_MODE, EQOS_DMA_MODE_RESET, EQOS_RESET_TIMEOUT_US);
	if (error != 0)
		return error;

	/* Masks the statistics counters' interrupts, which nothing reads. */
	write_register(device, EQOS_MMC_RX_INTERRUPT_MASK, 0xffffffffU);
	write_register(device, EQOS_MMC_TX_INTERRUPT_MASK, 0xffffffffU);

	/* Succeeded: the controller is idle with its defaults. */
	return 0;
}

/*
 * Sets the DMA's bus behaviour, the AXI limits and bursts, and the
 * channel's burst lengths.
 *
 * Procedure referenced: OpenBSD sys/dev/ic/dwqe.c, master
 * 859883f17cb8e20088fece5398547e4264ad30b3 (dwqe_attach, the DMA engine,
 * channel 0 and AXI master configuration).
 */
static void
configure_bus(
	struct eqos_device *device)
{
	const struct drv_dw_eqos_config *config;
	uint32_t bus;
	uint32_t control;
	uint32_t burst;
	unsigned index;

	/* Starts the bus mode without the bits the description decides. */
	config = &device->config;
	bus = read_register(device, EQOS_DMA_BUS);
	bus &= ~(EQOS_DMA_BUS_FIXED_BURST | EQOS_DMA_BUS_MIXED_BURST | EQOS_DMA_BUS_ALIGNED);
	bus &= ~EQOS_DMA_BUS_EXTENDED_ADDRESS;

	/* Uses only the burst lengths the bus allows when the board asks for fixed bursts. */
	if (config->fixed_burst)
		bus |= EQOS_DMA_BUS_FIXED_BURST;

	/* Mixes fixed and undefined-length bursts when the board asks for it. */
	if (config->mixed_burst)
		bus |= EQOS_DMA_BUS_MIXED_BURST;

	/* Aligns bursts to their addresses when the board asks for it. */
	if (config->address_aligned)
		bus |= EQOS_DMA_BUS_ALIGNED;

	/* Drives the upper address bits when buffers may lie above 4 GiB. */
	if (device->address_bits > 32U)
		bus |= EQOS_DMA_BUS_EXTENDED_ADDRESS;

	/* Sets the AXI master's outstanding requests, bursts and low-power idle as the board says. */
	if (config->axi_present) {
		/* Starts the AXI fields from nothing. */
		bus &= ~(EQOS_DMA_BUS_LPI | EQOS_DMA_BUS_WRITE_LIMIT_MASK | EQOS_DMA_BUS_READ_LIMIT_MASK);
		bus &= ~EQOS_DMA_BUS_BURST_MASK;

		/* Lets the AXI master idle in low power when the board says so. */
		if (config->axi_lpi)
			bus |= EQOS_DMA_BUS_LPI;

		/* Limits the outstanding writes and reads. */
		bus |= (config->axi_write_limit << EQOS_DMA_BUS_WRITE_LIMIT_SHIFT) & EQOS_DMA_BUS_WRITE_LIMIT_MASK;
		bus |= (config->axi_read_limit << EQOS_DMA_BUS_READ_LIMIT_SHIFT) & EQOS_DMA_BUS_READ_LIMIT_MASK;

		/* Allows each burst length the board lists. */
		for (index = 0; index < DRV_DW_EQOS_BURST_LENGTHS; index++)
			bus |= burst_bit(config->axi_burst_lengths[index]);
	}

	/* Writes the bus mode. */
	write_register(device, EQOS_DMA_BUS, bus);

	/* Multiplies the burst lengths by eight unless the board says not to. */
	control = read_register(device, EQOS_CHANNEL_CONTROL);
	control &= ~EQOS_CHANNEL_CONTROL_BURST_X8;
	if (config->burst_x8)
		control |= EQOS_CHANNEL_CONTROL_BURST_X8;
	write_register(device, EQOS_CHANNEL_CONTROL, control);

	/* Sets the transmit burst and lets a second frame start before the first's status. */
	burst = config->tx_burst;
	if (burst == 0)
		burst = 8U;
	control = read_register(device, EQOS_CHANNEL_TX_CONTROL);
	control &= ~EQOS_CHANNEL_BURST_MASK;
	control |= (burst << EQOS_CHANNEL_BURST_SHIFT) & EQOS_CHANNEL_BURST_MASK;
	control |= EQOS_CHANNEL_TX_SECOND_FRAME;
	write_register(device, EQOS_CHANNEL_TX_CONTROL, control);

	/* Sets the receive burst and the receive buffers' size. */
	burst = config->rx_burst;
	if (burst == 0)
		burst = 8U;
	control = read_register(device, EQOS_CHANNEL_RX_CONTROL);
	control &= ~(EQOS_CHANNEL_BURST_MASK | EQOS_CHANNEL_RX_BUFFER_MASK);
	control |= (burst << EQOS_CHANNEL_BURST_SHIFT) & EQOS_CHANNEL_BURST_MASK;
	control |= (EQOS_BUFFER_BYTES << EQOS_CHANNEL_RX_BUFFER_SHIFT) & EQOS_CHANNEL_RX_BUFFER_MASK;
	write_register(device, EQOS_CHANNEL_RX_CONTROL, control);
}

/*
 * Turns one AXI burst length into its bus mode bit, or no bit for a
 * length the controller lacks (the board pads its list with zeros).
 *
 * Values referenced: OpenBSD sys/dev/ic/dwqereg.h, master
 * 859883f17cb8e20088fece5398547e4264ad30b3 (GMAC_SYS_BUS_MODE_BLEN_*).
 */
static uint32_t
burst_bit(
	uint32_t length)
{
	/* Picks the bit of the length. */
	switch (length) {
	case 256U:
		return EQOS_DMA_BUS_BURST_256;
	case 128U:
		return EQOS_DMA_BUS_BURST_128;
	case 64U:
		return EQOS_DMA_BUS_BURST_64;
	case 32U:
		return EQOS_DMA_BUS_BURST_32;
	case 16U:
		return EQOS_DMA_BUS_BURST_16;
	case 8U:
		return EQOS_DMA_BUS_BURST_8;
	case 4U:
		return EQOS_DMA_BUS_BURST_4;
	default:
		break;
	}

	/* Reports no bit for a length the controller lacks. */
	return 0;
}

/*
 * Identifies the PHY, resets it, writes a YT8531's board settings or has
 * the board set up another PHY, and starts autonegotiation.
 */
static int
bring_up_phy(
	struct eqos_device *device)
{
	uint32_t identity;
	unsigned phy;
	int error;

	/* Reads who is at the board's PHY address. */
	phy = device->config.phy_address;
	error = drv_mii_phy_read_identity(&device->mdio, phy, &identity);
	if (error != 0) {
		kern_logf("dw-eqos: %s no PHY at %u (%d)\n", device->config.name, phy, error);
		return ENODEV;
	}

	/* Tells a YT8531, whose settings the board describes, from any other PHY. */
	device->phy_is_yt8531 = 0;
	if (identity == DRV_YT8531_IDENTITY)
		device->phy_is_yt8531 = 1;

	/* Reports the PHY found. */
	kern_logf("dw-eqos: %s PHY %u id %08x%s\n",
		  device->config.name,
		  phy,
		  identity,
		  device->phy_is_yt8531 ? " (YT8531)" : " (generic)");

	/* Resets the PHY. */
	error = drv_mii_phy_reset(&device->mdio, phy);
	if (error != 0)
		return error;

	/* Writes a YT8531's delays and drive strengths. */
	if (device->phy_is_yt8531) {
		error = drv_yt8531_configure(&device->mdio, phy, &device->config.yt8531);
		if (error != 0)
			return error;
	}

	/* Lets the board set up any other PHY it knows, such as its RGMII delays. */
	if (!device->phy_is_yt8531 && device->config.configure_phy != NULL) {
		error = device->config.configure_phy(device->config.phy_context, &device->mdio, phy, identity);
		if (error != 0)
			return error;
	}

	/* Advertises every speed up to gigabit and starts negotiating. */
	error = drv_mii_phy_start_autonegotiation(&device->mdio, phy, 1);
	if (error != 0)
		return error;

	/* Succeeded: the PHY negotiates with its link partner. */
	return 0;
}

/*
 * Allocates both rings in one block and one fixed buffer per descriptor.
 *
 * The block is aligned to its size, so neither ring crosses a 4 GiB
 * boundary the tail registers could not express.
 */
static int
allocate_rings(
	struct eqos_device *device)
{
	struct drv_dma_constraints constraints;
	size_t ring_bytes;
	int error;

	/* Describes what the controller's DMA reaches and whether it snoops the caches. */
	kern_memset(&constraints, 0, sizeof(constraints));
	constraints.address_bits = device->address_bits;
	constraints.max_segment_size = EQOS_RX_COUNT * EQOS_BUFFER_BYTES;
	constraints.coherent = device->config.dma_coherent;
	error = drv_dma_device_create(&constraints, &device->dma);
	if (error != 0)
		return error;

	/* Allocates the descriptors, transmit ring first. */
	ring_bytes = (EQOS_TX_COUNT + EQOS_RX_COUNT) * EQOS_DESCRIPTOR_BYTES;
	error = drv_dma_alloc_coherent(device->dma, ring_bytes, ring_bytes, &device->descriptors);
	if (error != 0)
		return error;

	/* Allocates the transmit buffers. */
	error = drv_dma_alloc_coherent(device->dma,
				       EQOS_TX_COUNT * EQOS_BUFFER_BYTES,
				       EQOS_BUFFER_ALIGNMENT,
				       &device->tx_buffers);
	if (error != 0)
		return error;

	/* Allocates the receive buffers. */
	error = drv_dma_alloc_coherent(device->dma,
				       EQOS_RX_COUNT * EQOS_BUFFER_BYTES,
				       EQOS_BUFFER_ALIGNMENT,
				       &device->rx_buffers);
	if (error != 0)
		return error;

	/* Splits the descriptor block into the two rings. */
	device->tx_ring = device->descriptors.address;
	device->rx_ring = device->tx_ring + EQOS_TX_COUNT;
	device->tx_ring_address = device->descriptors.device_address;
	device->rx_ring_address = device->tx_ring_address + EQOS_TX_COUNT * EQOS_DESCRIPTOR_BYTES;

	/* Succeeded: the rings and buffers exist, unseen by the controller. */
	return 0;
}

/* Gives back what an attach that failed took; the controller never saw the rings. */
static void
release_unpublished(
	struct eqos_device *device)
{
	int error;

	/* Withdraws an interface slot that was taken but not published. */
	if (device->net != NULL) {
		net_device_destroy(device->net);
		device->net = NULL;
	}

	/* Unbinds the interrupt before anything it reaches goes away. */
	if (device->irq_registered) {
		error = kern_irq_unregister((int)device->config.irq, eqos_interrupt, device);
		if (error != 0) {
			kern_logf("dw-eqos: interrupt release failed (%d); slot kept\n", error);
			return;
		}

		/* Records that no handler reaches the slot any more. */
		device->irq_registered = 0;
	}

	/* Returns the receive buffers. */
	if (device->rx_buffers.address != NULL)
		drv_dma_free_coherent(device->dma, &device->rx_buffers);

	/* Returns the transmit buffers. */
	if (device->tx_buffers.address != NULL)
		drv_dma_free_coherent(device->dma, &device->tx_buffers);

	/* Returns the descriptors. */
	if (device->descriptors.address != NULL)
		drv_dma_free_coherent(device->dma, &device->descriptors);

	/* Drops the DMA device once nothing is allocated from it. */
	if (device->dma != NULL) {
		error = drv_dma_device_destroy(device->dma);
		if (error != 0) {
			kern_logf("dw-eqos: DMA release failed (%d); slot kept\n", error);
			return;
		}
	}

	/* Frees the slot for a later attach. */
	kern_memset(device, 0, sizeof(*device));
}

/* Names the controller's network interface and publishes it. */
static int
publish_interface(
	struct eqos_device *device)
{
	size_t length;
	int error;

	/* Refuses a name that does not fit the interface's. */
	length = kern_strlen(device->config.name);
	if (length == 0 || length >= NET_DEVICE_NAME_MAX)
		return EINVAL;

	/* Takes an interface slot. */
	device->net = net_device_alloc();
	if (device->net == NULL)
		return ENOMEM;

	/* Describes an Ethernet port with the controller's address. */
	kern_memcpy(device->net->name, device->config.name, length + 1U);
	device->net->mtu = EQOS_DEFAULT_MTU;
	device->net->mtu_max = device->mtu_max;
	device->net->hwaddr_len = 6U;
	kern_memcpy(device->net->hwaddr, device->mac, 6U);
	device->net->flags = NET_DEVICE_BROADCAST | NET_DEVICE_MULTICAST;
	device->net->ops = &eqos_operations;
	device->net->driver_data = device;

	/* Publishes it. */
	error = net_device_create(device->net);
	if (error != 0)
		return error;

	/* Succeeded: the interface exists. */
	return 0;
}

/* Starts the thread that watches the link and wakes the worker after a lost interrupt. */
static void
start_service(
	struct eqos_device *device)
{
	int error;

	/* Creates the thread; without it the link never comes up. */
	error = kthread_create(service_worker, device, SCHED_PRIORITY_DEFAULT, &device->poll_thread);
	if (error != 0) {
		kern_logf("dw-eqos: %s service thread unavailable (%d)\n", device->config.name, error);
		return;
	}

	/* Keeps the interface for the thread, which lives as long as the system. */
	net_device_ref(device->net);

	/* Lets it run. */
	thread_start(device->poll_thread);
}

/*
 * Opens the interface: fresh rings, the station address and filter, the
 * queues, and the DMA and MAC started.
 *
 * Procedure referenced: OpenBSD sys/dev/ic/dwqe.c, master
 * 859883f17cb8e20088fece5398547e4264ad30b3 (dwqe_up).
 */
static int
eqos_open(
	struct net_device *net)
{
	struct eqos_device *device;
	unsigned long irq;
	uint32_t control;
	uint32_t high;
	uint32_t low;
	uint64_t ticks;
	unsigned carrier;

	/* A repeated open keeps the running rings. */
	device = net->driver_data;
	irq = spin_lock_irqsave(&device->lock);

	if (device->opened) {
		spin_unlock_irqrestore(&device->lock, irq);
		return 0;
	}

	/* Gives every receive descriptor but one to the controller and empties the transmit ring. */
	prepare_rings(device);

	/* Writes the station address. */
	high = (uint32_t)device->mac[5] << 8;
	high |= device->mac[4];
	low = (uint32_t)device->mac[3] << 24;
	low |= (uint32_t)device->mac[2] << 16;
	low |= (uint32_t)device->mac[1] << 8;
	low |= device->mac[0];
	write_register(device, EQOS_MAC_ADDRESS_HIGH, high);
	write_register(device, EQOS_MAC_ADDRESS_LOW, low);

	/* Admits every multicast frame, since the stack keeps no multicast list for the driver. */
	write_register(device, EQOS_MAC_HASH_LOW, 0);
	write_register(device, EQOS_MAC_HASH_HIGH, 0);
	write_register(device, EQOS_MAC_FILTER, EQOS_MAC_FILTER_ALL_MULTICAST);

	/* Tells the MAC how many register clock ticks make a microsecond. */
	ticks = device->config.csr_clock_hz / 1000000ULL;
	if (ticks != 0)
		write_register(device, EQOS_MAC_MICROSECOND_TICKS, (uint32_t)(ticks - 1U));

	/* Sizes and enables the transmit and receive queues. */
	configure_queues(device);

	/* Starts the receive and transmit DMA. */
	control = read_register(device, EQOS_CHANNEL_RX_CONTROL);
	write_register(device, EQOS_CHANNEL_RX_CONTROL, control | EQOS_CHANNEL_START);
	control = read_register(device, EQOS_CHANNEL_TX_CONTROL);
	write_register(device, EQOS_CHANNEL_TX_CONTROL, control | EQOS_CHANNEL_START);

	/* Marks the interface open, then sets the MAC to the link and starts it. */
	device->opened = 1;
	apply_link(device);
	carrier = 0;
	if (device->speed != 0)
		carrier = 1;

	/* Clears stale status and lets the channel interrupt. */
	write_register(device, EQOS_CHANNEL_STATUS, 0xffffffffU);
	write_register(device, EQOS_CHANNEL_INTERRUPT_ENABLE, EQOS_INTERRUPTS_RUNNING);

	spin_unlock_irqrestore(&device->lock, irq);

	/* Lets the PLIC deliver the interrupt and announces the carrier outside the lock. */
	kern_irq_unmask((int)device->config.irq);
	(void)net_device_set_carrier(net, (int)carrier);
	kern_logf("dw-eqos: %s RX/TX enabled\n", device->config.name);

	/* Succeeded: frames move by interrupt and the service thread. */
	return 0;
}

/*
 * Closes the interface: no more interrupts, the MAC and the DMA stopped,
 * and the transmit queue flushed.  The buffers stay.
 *
 * Procedure referenced: OpenBSD sys/dev/ic/dwqe.c, master
 * 859883f17cb8e20088fece5398547e4264ad30b3 (dwqe_down).
 */
static void
eqos_close(
	struct net_device *net)
{
	struct eqos_device *device;
	struct packet_buf *done;
	unsigned long irq;
	uint32_t value;
	unsigned slot;
	int error;

	/* Stops the PLIC from delivering another interrupt. */
	device = net->driver_data;
	kern_irq_mask((int)device->config.irq);

	/* Closes the producers' gate and stops the receiver and both DMA directions. */
	irq = spin_lock_irqsave(&device->lock);

	device->opened = 0;
	write_register(device, EQOS_CHANNEL_INTERRUPT_ENABLE, 0);
	value = read_register(device, EQOS_MAC_CONFIG);
	write_register(device, EQOS_MAC_CONFIG, value & ~EQOS_MAC_CONFIG_RX);
	value = read_register(device, EQOS_CHANNEL_RX_CONTROL);
	write_register(device, EQOS_CHANNEL_RX_CONTROL, value & ~EQOS_CHANNEL_START);
	value = read_register(device, EQOS_CHANNEL_TX_CONTROL);
	write_register(device, EQOS_CHANNEL_TX_CONTROL, value & ~EQOS_CHANNEL_START);
	value = read_register(device, EQOS_MTL_TX_MODE);
	write_register(device, EQOS_MTL_TX_MODE, value | EQOS_MTL_TX_FLUSH);

	spin_unlock_irqrestore(&device->lock, irq);

	/* Waits for the transmit queue to drain; nothing else touches the closed controller. */
	error = wait_register_clear(device, EQOS_MTL_TX_MODE, EQOS_MTL_TX_FLUSH, EQOS_FLUSH_TIMEOUT_US);
	if (error != 0)
		kern_logf("dw-eqos: %s transmit queue flush timed out\n", device->config.name);

	/* Stops the transmitter and clears what the channel reported. */
	irq = spin_lock_irqsave(&device->lock);

	value = read_register(device, EQOS_MAC_CONFIG);
	write_register(device, EQOS_MAC_CONFIG, value & ~EQOS_MAC_CONFIG_TX);
	write_register(device, EQOS_CHANNEL_STATUS, 0xffffffffU);

	/* The packets descriptors still held go on the list to free: the DMA has stopped. */
	for (slot = 0; slot < EQOS_TX_COUNT; slot++) {
		if (device->tx_packets[slot] == NULL)
			continue;
		device->tx_packets[slot]->next = device->tx_done;
		device->tx_done = device->tx_packets[slot];
		device->tx_packets[slot] = NULL;
	}

	/* Takes the list of packets whose transmission is finished. */
	done = take_tx_done_locked(device);

	spin_unlock_irqrestore(&device->lock, irq);

	/* Frees them outside the lock. */
	free_tx_done(done);

	/* Withdraws the carrier. */
	(void)net_device_set_carrier(net, 0);
}

/*
 * Copies one frame into the next free transmit buffer and hands its
 * descriptor to the controller.
 *
 * Procedure referenced: OpenBSD sys/dev/ic/dwqe.c, master
 * 859883f17cb8e20088fece5398547e4264ad30b3 (dwqe_start, dwqe_encap).
 */
static int
eqos_transmit(
	struct net_device *net,
	struct packet_buf *packet)
{
	struct eqos_device *device;
	struct eqos_descriptor *descriptor;
	struct packet_buf *done;
	uint8_t *buffer;
	uint64_t address;
	uint64_t physical;
	unsigned long irq;
	unsigned errors;
	unsigned slot;
	size_t length;
	int in_place;
	int error;

	/* Refuses a missing packet. */
	if (packet == NULL)
		return EINVAL;

	/* Drops a frame too short for a header or too long for the interface. */
	device = net->driver_data;
	length = packet->length;
	if (length < EQOS_HEADER_BYTES || length > device->frame_max) {
		packet_buf_free(packet);
		return EMSGSIZE;
	}

	/*
	 * Points the descriptor at the packet itself when the controller
	 * reaches its bytes; a short frame, which is padded, is copied.
	 */
	in_place = 0;
	if (length >= EQOS_MIN_FRAME) {
		error = packet_buf_physical(packet, &physical);
		if (error == 0)
			error = drv_dma_physical_address(device->dma, physical, length, 0, &address);
		if (error == 0)
			in_place = 1;
	}

	/* Takes a free descriptor, if the interface runs and has a link. */
	error = 0;
	errors = 0;
	irq = spin_lock_irqsave(&device->lock);

	if (!device->opened || device->speed == 0) {
		error = ENETDOWN;
	} else {
		/*
		 * Frees what the controller finished only when the ring looks
		 * full (the network worker's poll frees the rest), then checks
		 * one descriptor is left.
		 */
		if (device->tx_producer - device->tx_consumer >= EQOS_TX_COUNT / 2U)
			errors = reclaim_tx(device);
		if (device->tx_producer - device->tx_consumer >= EQOS_TX_COUNT - 1U)
			error = ENOBUFS;
	}

	/* Fills the descriptor and hands it over. */
	if (error == 0) {
		/*
		 * The packet in place, kept until the frame is sent; or the frame
		 * copied, a short one padded so no old bytes go out.
		 */
		slot = device->tx_producer % EQOS_TX_COUNT;
		if (in_place) {
			device->tx_packets[slot] = packet;
		} else {
			buffer = (uint8_t *)device->tx_buffers.address + slot * EQOS_BUFFER_BYTES;
			kern_memcpy(buffer, packet->data, length);
			if (length < EQOS_MIN_FRAME) {
				kern_memset(buffer + length, 0, EQOS_MIN_FRAME - length);
				length = EQOS_MIN_FRAME;
			}

			/* The bus address of the copy the controller reads. */
			address = device->tx_buffers.device_address + slot * EQOS_BUFFER_BYTES;
		}

		/* Points the descriptor at the bytes as one whole frame, interrupting when sent. */
		descriptor = &device->tx_ring[slot];
		descriptor->word0 = (uint32_t)address;
		descriptor->word1 = (uint32_t)(address >> 32);
		descriptor->word2 = EQOS_TX_INTERRUPT | (uint32_t)length;

		/* Hands the descriptor over only after the buffer and the other words are written. */
		kern_io_write_barrier();
		descriptor->word3 = EQOS_DESCRIPTOR_OWN | EQOS_TX_FIRST | EQOS_TX_LAST | (uint32_t)length;

		/* Moves the tail past the descriptor, which starts the transmit DMA. */
		device->tx_producer++;
		write_register(device,
			       EQOS_CHANNEL_TX_TAIL,
			       (uint32_t)(device->tx_ring_address +
					  (device->tx_producer % EQOS_TX_COUNT) * EQOS_DESCRIPTOR_BYTES));
	}

	/* Takes the list of packets whose transmission is finished. */
	done = take_tx_done_locked(device);

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
 * to the budget's frames, and lets the channel interrupt again.
 */
static unsigned
eqos_poll(
	struct net_device *net,
	unsigned budget)
{
	struct eqos_device *device;
	struct packet_buf *done;
	unsigned long irq;
	unsigned errors;
	unsigned count;
	unsigned report;

	/* Frees what the controller has sent. */
	device = net->driver_data;
	errors = 0;
	irq = spin_lock_irqsave(&device->lock);

	if (device->opened)
		errors = reclaim_tx(device);
	done = take_tx_done_locked(device);

	spin_unlock_irqrestore(&device->lock, irq);

	/* The packets of the sent frames, outside the lock. */
	free_tx_done(done);

	/* Counts sent frames that failed. */
	report_tx_errors(net, errors);

	/* Passes received frames to the stack. */
	count = receive_frames(net, budget);

	/* Lets the channel interrupt again once the batch is done; a closed one stays quiet. */
	report = 0;
	irq = spin_lock_irqsave(&device->lock);

	if (device->opened)
		write_register(device, EQOS_CHANNEL_INTERRUPT_ENABLE, EQOS_INTERRUPTS_RUNNING);

	/* Takes a fatal bus error for reporting, once. */
	if (device->bus_error && !device->bus_error_reported) {
		device->bus_error_reported = 1;
		report = 1;
	}

	spin_unlock_irqrestore(&device->lock, irq);

	/* Reports a fatal bus error once; the DMA stays stopped until the controller is reset. */
	if (report)
		kern_logf("dw-eqos: %s DMA fatal bus error\n", device->config.name);

	/* Reports the frames received, dropped ones included. */
	return count;
}

/*
 * Empties the transmit ring and gives every receive descriptor but the
 * last to the controller, with the channel's ring registers to match.
 */
static void
prepare_rings(
	struct eqos_device *device)
{
	unsigned slot;

	/* Takes every descriptor back to the CPU. */
	kern_memset(device->tx_ring, 0, EQOS_TX_COUNT * EQOS_DESCRIPTOR_BYTES);
	kern_memset(device->rx_ring, 0, EQOS_RX_COUNT * EQOS_DESCRIPTOR_BYTES);

	/* Starts both rings at their first descriptor. */
	device->tx_producer = 0;
	device->tx_consumer = 0;
	device->rx_consumer = 0;
	device->rx_producer = 0;

	/* Gives every receive descriptor but the last to the controller. */
	for (slot = 0; slot < EQOS_RX_COUNT - 1U; slot++)
		arm_rx_slot(device, slot);

	/* Records the last descriptor as the one kept back. */
	device->rx_producer = EQOS_RX_COUNT - 1U;

	/* Describes the transmit ring, empty: its tail is its head. */
	write_register(device, EQOS_CHANNEL_TX_BASE_HIGH, (uint32_t)(device->tx_ring_address >> 32));
	write_register(device, EQOS_CHANNEL_TX_BASE_LOW, (uint32_t)device->tx_ring_address);
	write_register(device, EQOS_CHANNEL_TX_RING_LENGTH, EQOS_TX_COUNT - 1U);
	write_register(device, EQOS_CHANNEL_TX_TAIL, (uint32_t)device->tx_ring_address);

	/* Describes the receive ring, whose tail is the one descriptor kept back. */
	write_register(device, EQOS_CHANNEL_RX_BASE_HIGH, (uint32_t)(device->rx_ring_address >> 32));
	write_register(device, EQOS_CHANNEL_RX_BASE_LOW, (uint32_t)device->rx_ring_address);
	write_register(device, EQOS_CHANNEL_RX_RING_LENGTH, EQOS_RX_COUNT - 1U);
	write_register(device,
		       EQOS_CHANNEL_RX_TAIL,
		       (uint32_t)(device->rx_ring_address + device->rx_producer * EQOS_DESCRIPTOR_BYTES));
}

/*
 * Points one receive descriptor at its buffer and gives it to the
 * controller.
 *
 * Procedure referenced: OpenBSD sys/dev/ic/dwqe.c, master
 * 859883f17cb8e20088fece5398547e4264ad30b3 (dwqe_fill_rx_ring).
 */
static void
arm_rx_slot(
	struct eqos_device *device,
	unsigned slot)
{
	struct eqos_descriptor *descriptor;
	uint64_t address;

	/* Points the descriptor at the slot's buffer. */
	descriptor = &device->rx_ring[slot];
	address = device->rx_buffers.device_address + slot * EQOS_BUFFER_BYTES;
	descriptor->word0 = (uint32_t)address;
	descriptor->word1 = (uint32_t)(address >> 32);
	descriptor->word2 = 0;

	/* Hands it over, asking for an interrupt when a frame lands, after the address is written. */
	kern_io_write_barrier();
	descriptor->word3 = EQOS_DESCRIPTOR_OWN | EQOS_RX_INTERRUPT | EQOS_RX_BUFFER1_VALID;
}

/*
 * Sizes the transmit and receive queues and picks store-and-forward or
 * threshold mode, then enables the receive queue.
 *
 * Procedure referenced: OpenBSD sys/dev/ic/dwqe.c, master
 * 859883f17cb8e20088fece5398547e4264ad30b3 (dwqe_up, the MTL queue
 * operation modes).
 */
static void
configure_queues(
	struct eqos_device *device)
{
	uint32_t fifo;
	uint32_t mode;

	/* Finds the receive FIFO's size: the board's, else the controller's. */
	fifo = device->config.rx_fifo_bytes;
	if (fifo == 0)
		fifo = 128U << ((device->features[1] >> EQOS_FEATURE1_RX_FIFO_SHIFT) & EQOS_FEATURE1_FIFO_MASK);

	/* Sets the receive queue's size and when it passes frames to the DMA. */
	mode = read_register(device, EQOS_MTL_RX_MODE);
	mode &= ~(EQOS_MTL_RX_SIZE_MASK | EQOS_MTL_RX_STORE_FORWARD | EQOS_MTL_RX_THRESHOLD_MASK);
	mode |= ((fifo / 256U - 1U) << EQOS_MTL_RX_SIZE_SHIFT) & EQOS_MTL_RX_SIZE_MASK;
	if (device->config.threshold_mode) {
		mode |= EQOS_MTL_RX_THRESHOLD_128;
	} else {
		mode |= EQOS_MTL_RX_STORE_FORWARD;
	}

	/* Writes the receive queue's new mode. */
	write_register(device, EQOS_MTL_RX_MODE, mode);

	/* Finds the transmit FIFO's size: the board's, else the controller's. */
	fifo = device->config.tx_fifo_bytes;
	if (fifo == 0)
		fifo = 128U << ((device->features[1] >> EQOS_FEATURE1_TX_FIFO_SHIFT) & EQOS_FEATURE1_FIFO_MASK);

	/* Sets the transmit queue's size and mode, and enables it. */
	mode = read_register(device, EQOS_MTL_TX_MODE);
	mode &= ~(EQOS_MTL_TX_SIZE_MASK | EQOS_MTL_TX_STORE_FORWARD | EQOS_MTL_TX_THRESHOLD_MASK);
	mode &= ~(EQOS_MTL_TX_QUEUE_MASK | EQOS_MTL_TX_FLUSH);
	mode |= ((fifo / 256U - 1U) << EQOS_MTL_TX_SIZE_SHIFT) & EQOS_MTL_TX_SIZE_MASK;
	if (device->config.threshold_mode) {
		mode |= EQOS_MTL_TX_THRESHOLD_512;
	} else {
		mode |= EQOS_MTL_TX_STORE_FORWARD;
	}

	/* Writes the transmit queue's new mode with the queue enabled. */
	mode |= EQOS_MTL_TX_QUEUE_ENABLED;
	write_register(device, EQOS_MTL_TX_MODE, mode);

	/* Enables the receive queue for ordinary traffic; it is off after the reset. */
	write_register(device, EQOS_MAC_RX_QUEUES, EQOS_MAC_RX_QUEUE0_GENERIC);
}

/*
 * Sets the MAC's speed and duplex to the link's and runs the transmitter
 * and receiver while the interface is open.  The caller holds the lock.
 *
 * Procedure referenced: OpenBSD sys/dev/ic/dwqe.c, master
 * 859883f17cb8e20088fece5398547e4264ad30b3 (dwqe_mii_statchg).
 */
static void
apply_link(
	struct eqos_device *device)
{
	uint32_t value;

	/* A closed interface keeps its MAC stopped. */
	if (!device->opened)
		return;

	/* Picks the port's speed bits: gigabit has neither. */
	value = read_register(device, EQOS_MAC_CONFIG);
	value &= ~(EQOS_MAC_CONFIG_SPEED_MASK | EQOS_MAC_CONFIG_FULL_DUPLEX);
	if (device->speed == 100U) {
		value |= EQOS_MAC_CONFIG_MII | EQOS_MAC_CONFIG_FAST;
	} else if (device->speed == 10U) {
		value |= EQOS_MAC_CONFIG_MII;
	}

	/* Runs full duplex when the link does. */
	if (device->full_duplex)
		value |= EQOS_MAC_CONFIG_FULL_DUPLEX;

	/* Takes jumbo frames when the interface may run above 1500 (the stack drops what exceeds the MTU). */
	if (device->mtu_max > EQOS_DEFAULT_MTU)
		value |= EQOS_MAC_CONFIG_JUMBO;

	/* Starts the transmitter and receiver. */
	value |= EQOS_MAC_CONFIG_TX | EQOS_MAC_CONFIG_RX;
	write_register(device, EQOS_MAC_CONFIG, value);
}

/*
 * Moves the transmit consumer past every descriptor the controller has
 * finished.  The caller holds the lock.
 *
 * Returns how many of them failed on the wire.
 *
 * Procedure referenced: OpenBSD sys/dev/ic/dwqe.c, master
 * 859883f17cb8e20088fece5398547e4264ad30b3 (dwqe_tx_proc).
 */
static unsigned
reclaim_tx(
	struct eqos_device *device)
{
	struct eqos_descriptor *descriptor;
	struct packet_buf *packet;
	uint32_t status;
	unsigned errors;

	/* Walks the ring from the oldest frame on it. */
	errors = 0;
	while (device->tx_consumer != device->tx_producer) {
		/* Stops at the first descriptor the controller still owns. */
		descriptor = &device->tx_ring[device->tx_consumer % EQOS_TX_COUNT];
		status = descriptor->word3;
		if ((status & EQOS_DESCRIPTOR_OWN) != 0)
			break;

		/* Counts a frame the MAC reported an error for. */
		kern_io_read_barrier();
		if ((status & EQOS_TX_ERROR) != 0)
			errors++;

		/* Frees the descriptor, and puts the packet it held on the list to free. */
		descriptor->word3 = 0;
		packet = device->tx_packets[device->tx_consumer % EQOS_TX_COUNT];
		if (packet != NULL) {
			device->tx_packets[device->tx_consumer % EQOS_TX_COUNT] = NULL;
			packet->next = device->tx_done;
			device->tx_done = packet;
		}
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
	struct eqos_device *device)
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

	/* Each packet in turn. */
	while (list != NULL) {
		next = list->next;
		list->next = NULL;
		packet_buf_free(list);
		list = next;
	}
}

/*
 * Copies up to the budget's received frames into packets, gives their
 * descriptors back, and passes the packets to the stack.
 *
 * Procedure referenced: OpenBSD sys/dev/ic/dwqe.c, master
 * 859883f17cb8e20088fece5398547e4264ad30b3 (dwqe_rx_proc,
 * dwqe_fill_rx_ring).
 */
static unsigned
receive_frames(
	struct net_device *net,
	unsigned budget)
{
	struct eqos_device *device;
	struct eqos_descriptor *descriptor;
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
		slot = device->rx_consumer % EQOS_RX_COUNT;
		descriptor = &device->rx_ring[slot];
		status = descriptor->word3;
		if (!device->opened || (status & EQOS_DESCRIPTOR_OWN) != 0)
			empty = 1;

		/* Reads the frame the controller wrote back. */
		if (!empty) {
			/* Reads the buffer only after the write-back that released it. */
			kern_io_read_barrier();
			length = status & EQOS_RX_LENGTH_MASK;

			/* Takes a whole, good, ordinary frame that fits one buffer; counts anything else. */
			if ((status & (EQOS_RX_FIRST | EQOS_RX_LAST)) == (EQOS_RX_FIRST | EQOS_RX_LAST) &&
			    (status & (EQOS_RX_ERROR | EQOS_RX_CONTEXT)) == 0 &&
			    length >= EQOS_HEADER_BYTES + EQOS_CRC_BYTES &&
			    length <= device->frame_max + EQOS_CRC_BYTES) {
				/*
				 * Copies the frame without its checksum into a packet of the
				 * class it needs (a driver's receive may take the reserved
				 * jumbo packets); without one the frame is dropped.
				 */
				length -= EQOS_CRC_BYTES;
				payload = NULL;
				packet = packet_buf_alloc_for(PACKET_BUF_DEFAULT_HEADROOM,
							      length,
							      PACKET_BUF_ALLOC_RECEIVE,
							      &alloc_error);
				if (packet != NULL)
					payload = packet_buf_append(packet, length);
				if (payload != NULL) {
					kern_memcpy(payload,
						    (uint8_t *)device->rx_buffers.address + slot * EQOS_BUFFER_BYTES,
						    length);
					valid = 1;
				}
			} else {
				/* A frame with an error, split across buffers, or a context descriptor. */
				net->rx_errors++;
			}

			/* Counts a frame dropped for want of a packet or for an error. */
			if (!valid)
				net->rx_dropped++;

			/*
			 * Gives back the descriptor kept in reserve and keeps this one
			 * instead, then moves the tail past the one given back.
			 */
			device->rx_consumer++;
			arm_rx_slot(device, device->rx_producer % EQOS_RX_COUNT);
			device->rx_producer++;
			write_register(device,
				       EQOS_CHANNEL_RX_TAIL,
				       (uint32_t)(device->rx_ring_address +
						  (device->rx_producer % EQOS_RX_COUNT) * EQOS_DESCRIPTOR_BYTES));
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

/*
 * Clears the channel's status and, for frames sent or received, holds
 * those interrupts off and wakes the network worker.
 *
 * Procedure referenced: OpenBSD sys/dev/ic/dwqe.c, master
 * 859883f17cb8e20088fece5398547e4264ad30b3 (dwqe_intr).
 */
static void
eqos_interrupt(
	int irq,
	kern_irq_ack_t acknowledge,
	void *argument)
{
	struct eqos_device *device;
	unsigned long enabled;
	uint32_t status;
	unsigned schedule;

	UNUSED_PARAMETER(irq);

	/* Reads and clears what the channel and the MAC report. */
	device = argument;
	schedule = 0;
	enabled = spin_lock_irqsave(&device->lock);

	status = read_register(device, EQOS_CHANNEL_STATUS);
	write_register(device, EQOS_CHANNEL_STATUS, status);
	(void)read_register(device, EQOS_MAC_INTERRUPT_STATUS);

	/* Remembers a fatal bus error for the worker to report; the DMA has stopped. */
	if ((status & EQOS_CHANNEL_BUS_ERROR) != 0)
		device->bus_error = 1;

	/* Holds the frame interrupts off until the worker has drained the rings. */
	if (device->opened &&
	    (status & (EQOS_CHANNEL_RX_DONE | EQOS_CHANNEL_TX_DONE | EQOS_CHANNEL_RX_UNAVAILABLE)) != 0) {
		write_register(device, EQOS_CHANNEL_INTERRUPT_ENABLE, EQOS_INTERRUPTS_DRAINING);
		schedule = 1;
	}

	spin_unlock_irqrestore(&device->lock, enabled);

	/* Completes the PLIC's claim before waking the lower-ranked network worker. */
	kern_irq_send_eoi(acknowledge);
	if (schedule)
		net_device_schedule_poll(device->net);
}

/*
 * Watches the link every 250 ms and wakes the network worker every 20 ms
 * while the interface is open, for the life of the system.
 */
static void
service_worker(
	void *argument)
{
	struct eqos_device *device;
	unsigned long irq;
	uint64_t next_link;
	uint64_t now;
	unsigned opened;

	/* Checks the link at once, then on its interval. */
	device = argument;
	next_link = 0;
	for (;;) {
		/* Follows the link when its interval has passed. */
		now = sched_ticks();
		if (now >= next_link) {
			check_link(device);
			next_link = now + kern_ms_to_ticks(EQOS_LINK_MS);
		}

		/* Samples whether the interface is open. */
		irq = spin_lock_irqsave(&device->lock);

		opened = device->opened;

		spin_unlock_irqrestore(&device->lock, irq);

		/* Wakes the worker for anything a lost interrupt left on the rings. */
		if (opened)
			net_device_schedule_poll(device->net);

		/* Sleeps until the next service. */
		sched_sleep(sched_ticks() + kern_ms_to_ticks(EQOS_POLL_MS));
	}
}

/*
 * Reads the link from the PHY and, when it changed, moves the PHY's and
 * the board's transmit clock and the MAC to it and announces the carrier.
 */
static void
check_link(
	struct eqos_device *device)
{
	struct drv_mii_link link;
	unsigned long irq;
	unsigned carrier;
	int error;

	/* Reads what the link settled on. */
	error = drv_mii_phy_read_link(&device->mdio, device->config.phy_address, &link);
	if (error != 0) {
		/* Reports an unresponsive PHY once per episode and treats the link as down. */
		if (!device->mdio_fault)
			kern_logf("dw-eqos: %s PHY status failed (%d)\n", device->config.name, error);
		device->mdio_fault = 1;
		link.speed_mbps = 0;
		link.full_duplex = 0;
	} else {
		/* Ends a fault episode. */
		device->mdio_fault = 0;
	}

	/* A link that stayed as it was needs nothing. */
	if (link.speed_mbps == device->speed && (unsigned)link.full_duplex == device->full_duplex)
		return;

	/* Moves the PHY's and the board's transmit clock to the new speed first. */
	if (link.speed_mbps != 0) {
		/* Inverts the YT8531's transmit clock as the board says for the speed. */
		if (device->phy_is_yt8531) {
			error = drv_yt8531_set_speed(&device->mdio,
						     device->config.phy_address,
						     &device->config.yt8531,
						     link.speed_mbps);
			if (error != 0) {
				kern_logf("dw-eqos: %s PHY clock for %u Mbps failed (%d)\n",
					  device->config.name,
					  link.speed_mbps,
					  error);
			}
		}

		/* Lets the board move its transmit clock. */
		if (device->config.set_speed != NULL) {
			error = device->config.set_speed(device->config.context, link.speed_mbps);
			if (error != 0) {
				kern_logf("dw-eqos: %s board clock for %u Mbps failed (%d)\n",
					  device->config.name,
					  link.speed_mbps,
					  error);
			}
		}
	}

	/* Records the link and sets the MAC to it. */
	irq = spin_lock_irqsave(&device->lock);

	device->speed = link.speed_mbps;
	device->full_duplex = (unsigned)link.full_duplex;
	device->net->link_mbps = link.speed_mbps;
	apply_link(device);

	spin_unlock_irqrestore(&device->lock, irq);

	/* Announces the carrier, which a link of any speed carries. */
	carrier = 0;
	if (link.speed_mbps != 0)
		carrier = 1;
	(void)net_device_set_carrier(device->net, (int)carrier);

	/* Writes the change to the boot log. */
	if (carrier) {
		kern_logf("dw-eqos: %s link up %u Mbps %s duplex\n",
			  device->config.name,
			  link.speed_mbps,
			  link.full_duplex ? "full" : "half");
	} else {
		kern_logf("dw-eqos: %s link down\n", device->config.name);
	}
}
