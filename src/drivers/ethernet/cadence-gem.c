/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The Cadence Gigabit Ethernet MAC (GEM) as a network interface.
 *
 * Both descriptor rings and every packet slot live in memory the driver
 * owns for as long as the kernel runs, so the controller never reaches
 * freed memory across close and reopen.  A receive descriptor points at
 * its slot for good; a frame is copied out of the slot into the network
 * packet pool and the descriptor is handed back.  A transmitted frame is
 * copied into its slot.  The descriptors are two words with 32-bit
 * addresses or four words with 64-bit ones, as the board says.  No
 * checksum offload is used.
 */

#include <drivers/ethernet/cadence-gem.h>
#include <drivers/ethernet/mii-phy.h>
#include <drivers/generic/dma.h>
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
 * The GEM's registers and their bits.
 *
 * Values referenced: OpenBSD sys/dev/fdt/if_cad.c rev 1.17 (GEM_NETCTL,
 * GEM_NETCFG, GEM_NETSR, GEM_DMACR, GEM_TXSR, GEM_RXQBASE, GEM_TXQBASE,
 * GEM_RXSR, GEM_ISR, GEM_IER, GEM_IDR, GEM_PHYMNTNC, GEM_HASHL, GEM_HASHH,
 * GEM_LADDRL(), GEM_LADDRH(), GEM_MID, GEM_CFG6, GEM_CFG8, GEM_TXQ1BASE(),
 * GEM_RXQ1BASE(), GEM_TXQBASEHI, GEM_RXQBASEHI, GEM_SCR_TYPE1(),
 * GEM_SCR_TYPE2(), GEM_RXQ8BASE() and the bits of each).
 */
#define GEM_NETCTL			0x0000U
#define GEM_NETCTL_STARTTX		(1U << 9)
#define GEM_NETCTL_STATCLR		(1U << 5)
#define GEM_NETCTL_MDEN			(1U << 4)
#define GEM_NETCTL_TXEN			(1U << 3)
#define GEM_NETCTL_RXEN			(1U << 2)
#define GEM_NETCFG			0x0004U
#define GEM_NETCFG_SGMIIEN		(1U << 27)
#define GEM_NETCFG_RXCSUMEN		(1U << 24)
#define GEM_NETCFG_MDCCLKDIV_MASK	(0x7U << 18)
#define GEM_NETCFG_MDCCLKDIV_SHIFT	18U
#define GEM_NETCFG_FCSREM		(1U << 17)
#define GEM_NETCFG_RXOFFS_MASK		(0x3U << 14)
#define GEM_NETCFG_PCSSEL		(1U << 11)
#define GEM_NETCFG_1000			(1U << 10)
#define GEM_NETCFG_1536RXEN		(1U << 8)
#define GEM_NETCFG_UCASTHASHEN		(1U << 7)
#define GEM_NETCFG_MCASTHASHEN		(1U << 6)
#define GEM_NETCFG_BCASTDI		(1U << 5)
#define GEM_NETCFG_COPYALL		(1U << 4)
#define GEM_NETCFG_FDEN			(1U << 1)
#define GEM_NETCFG_100			(1U << 0)
#define GEM_NETSR			0x0008U
#define GEM_NETSR_PHY_MGMT_IDLE		(1U << 2)
#define GEM_DMACR			0x0010U
#define GEM_DMACR_DMA64			(1U << 30)
#define GEM_DMACR_AHBDISC		(1U << 24)
#define GEM_DMACR_RXBUF_MASK		(0xffU << 16)
#define GEM_DMACR_RXBUF_SHIFT		16U
#define GEM_DMACR_TXCSUMEN		(1U << 11)
#define GEM_DMACR_TXSIZE		(1U << 10)
#define GEM_DMACR_RXSIZE_MASK		(0x3U << 8)
#define GEM_DMACR_RXSIZE_8K		(0x3U << 8)
#define GEM_DMACR_ES_PDATA		(1U << 7)
#define GEM_DMACR_ES_DESCR		(1U << 6)
#define GEM_DMACR_BLEN_MASK		(0x1fU << 0)
#define GEM_DMACR_BLEN_16		(0x10U << 0)
#define GEM_TXSR			0x0014U
#define GEM_TXSR_TXGO			(1U << 3)
#define GEM_RXQBASE			0x0018U
#define GEM_TXQBASE			0x001cU
#define GEM_RXSR			0x0020U
#define GEM_ISR				0x0024U
#define GEM_IER				0x0028U
#define GEM_IDR				0x002cU
#define GEM_IXR_HRESP			(1U << 11)
#define GEM_IXR_RXOVR			(1U << 10)
#define GEM_IXR_TXDONE			(1U << 7)
#define GEM_IXR_RXDONE			(1U << 1)
#define GEM_PHYMNTNC			0x0034U
#define GEM_PHYMNTNC_CLAUSE_22		(1U << 30)
#define GEM_PHYMNTNC_OP_READ		(0x2U << 28)
#define GEM_PHYMNTNC_OP_WRITE		(0x1U << 28)
#define GEM_PHYMNTNC_ADDR_SHIFT		23U
#define GEM_PHYMNTNC_REG_SHIFT		18U
#define GEM_PHYMNTNC_MUST_10		(0x2U << 16)
#define GEM_PHYMNTNC_DATA_MASK		0xffffU
#define GEM_HASHL			0x0080U
#define GEM_HASHH			0x0084U
#define GEM_LADDRL			0x0088U
#define GEM_LADDRH			0x008cU
#define GEM_LADDR_STRIDE		8U
#define GEM_LADDRNUM			4U
#define GEM_MID				0x00fcU
#define GEM_MID_VERSION_SHIFT		16U
#define GEM_MID_VERSION_MASK		0xfffU
#define GEM_CFG6			0x0294U
#define GEM_CFG6_DMA64			(1U << 23)
#define GEM_CFG6_PRIQ_MASK		0xffffU
#define GEM_CFG8			0x029cU
#define GEM_CFG8_TYPE1_SHIFT		24U
#define GEM_CFG8_TYPE2_SHIFT		16U
#define GEM_TXQ1BASE			0x0440U
#define GEM_RXQ1BASE			0x0480U
#define GEM_QUEUE_BASE_DISABLE		(1U << 0)
#define GEM_TXQBASEHI			0x04c8U
#define GEM_RXQBASEHI			0x04d4U
#define GEM_SCR_TYPE1			0x0500U
#define GEM_SCR_TYPE2			0x0540U
#define GEM_RXQ8BASE			0x05c0U
#define GEM_MAX_PRIQ			16U

/*
 * The user I/O register with its RGMII bit, and the interrupt of a
 * receive descriptor read while still used.
 *
 * Values referenced: Linux v6.12 drivers/net/ethernet/cadence/macb.h
 * (GEM_USRIO, GEM_RGMII_OFFSET, MACB_RXUBR_OFFSET).
 */
#define GEM_USRIO			0x000cU
#define GEM_USRIO_RGMII			(1U << 0)
#define GEM_IXR_RXUBR			(1U << 2)

/* The first GEM version with the configuration registers CFG6 to CFG10. */
#define GEM_VERSION_WITH_CFG		0x7U

/*
 * A descriptor's words and their bits: a 32-bit descriptor has the first
 * two words, a 64-bit one all four.
 *
 * Values referenced: OpenBSD sys/dev/fdt/if_cad.c rev 1.17 (struct
 * cad_desc32, struct cad_desc64, GEM_RXD_ADDR_WRAP, GEM_RXD_ADDR_USED,
 * GEM_RXD_EOF, GEM_RXD_SOF, GEM_RXD_LEN_MASK, GEM_TXD_USED, GEM_TXD_WRAP,
 * GEM_TXD_RLIMIT, GEM_TXD_CORRUPT, GEM_TXD_LCOLL, GEM_TXD_LAST,
 * GEM_TXD_LEN_MASK).
 */
#define GEM_DESC_ADDRESS_LOW		0U
#define GEM_DESC_STATUS			1U
#define GEM_DESC_ADDRESS_HIGH		2U
#define GEM_DESC_RESERVED		3U
#define GEM_DESC_WORDS_32		2U
#define GEM_DESC_WORDS_64		4U
#define GEM_RXD_ADDR_WRAP		(1U << 1)
#define GEM_RXD_ADDR_USED		(1U << 0)
#define GEM_RXD_EOF			(1U << 15)
#define GEM_RXD_SOF			(1U << 14)
#define GEM_RXD_LEN_MASK		0x1fffU
#define GEM_TXD_USED			(1U << 31)
#define GEM_TXD_WRAP			(1U << 30)
#define GEM_TXD_RLIMIT			(1U << 29)
#define GEM_TXD_CORRUPT			(1U << 27)
#define GEM_TXD_LCOLL			(1U << 26)
#define GEM_TXD_LAST			(1U << 15)
#define GEM_TXD_LEN_MASK		0x3fffU

/*
 * The MDIO clock dividers the configuration's field selects, in order.
 * The MDIO clock must not exceed 2.5 MHz.
 *
 * Values referenced: OpenBSD sys/dev/fdt/if_cad.c rev 1.17 (the
 * mdcclk_divs table of cad_reset()).
 */
#define GEM_MDC_DIVIDER_COUNT		8U
#define GEM_MDC_MAX_HZ			2500000U

/* Ring geometry, slot size, and frame bounds. */
#define GEM_RING_COUNT			256U
#define GEM_PACKET_BYTES		2048U
#define GEM_MIN_FRAME			60U
#define GEM_MAX_FRAME			1514U
#define GEM_HEADER_BYTES		14U
#define GEM_RING_ALIGN			4096U
#define GEM_PACKET_ALIGN		2048U

/* The highest PHY address on a clause-22 bus. */
#define GEM_PHY_ADDRESS_MAX		31U

/*
 * Bounded waits and the periodic service.  The MDIO and transmit-idle
 * waits run under the device lock and spin on the monotonic counter; a
 * clause-22 frame at the 2.5 MHz MDIO clock takes about 26 us.
 */
#define GEM_MDIO_TIMEOUT_US		2000U
#define GEM_TX_IDLE_TIMEOUT_US		10000U
#define GEM_WAIT_MAX_READS		1000000U
#define GEM_POLL_MS			20U
#define GEM_LINK_MS			250U

/* The interrupts the driver serves apart from the receive ones. */
#define GEM_OTHER_INTERRUPTS		(GEM_IXR_HRESP | GEM_IXR_RXOVR | GEM_IXR_TXDONE)

/*
 * One controller.
 *
 * It lives for as long as the kernel runs once attached: the descriptor
 * rings, the slots and the interface are never given back.  The lock
 * guards the rings, the counters and the controller's registers, and is
 * taken with interrupts off; opened says whether the rings belong to the
 * controller.  The configuration is the board's description, copied at
 * attachment.
 */
struct drv_cadence_gem {
	struct spinlock lock;
	struct drv_cadence_gem_config config;
	volatile uint8_t *registers;
	struct drv_mii_bus mdio;
	uint32_t netctl;
	uint32_t rx_interrupts;
	unsigned descriptor_words;
	unsigned phy_address;
	unsigned queue_mask;
	unsigned type1_screeners;
	unsigned type2_screeners;
	unsigned mdc_divider;
	struct drv_dma_buffer rings;
	struct drv_dma_buffer rx;
	struct drv_dma_buffer tx;
	volatile uint32_t *rx_descriptors;
	volatile uint32_t *tx_descriptors;
	struct net_device *net;
	struct thread *poll_thread;
	unsigned rx_next;
	unsigned tx_producer;
	unsigned tx_consumer;
	uint64_t next_link_check;
	unsigned opened;
	unsigned link;
	unsigned speed;
	unsigned mdio_fault;
	uint8_t mac[6];
};

static int gem_open(struct net_device *net);
static void gem_close(struct net_device *net);
static int gem_transmit(struct net_device *net, struct packet_buf *packet);
static unsigned gem_poll(struct net_device *net, unsigned budget);

/* The network callbacks; the slots outlive every close. */
static const struct net_device_ops gem_operations = {
	gem_open,
	gem_close,
	gem_transmit,
	gem_poll,
	NULL,
	NULL,
	NULL
};

/*
 * The MDIO clock dividers.
 *
 * Values referenced: OpenBSD sys/dev/fdt/if_cad.c rev 1.17 (mdcclk_divs).
 */
static const unsigned gem_mdc_dividers[GEM_MDC_DIVIDER_COUNT] = {
	8U, 16U, 32U, 48U, 64U, 96U, 128U, 224U
};

/*
 * The controllers, and how many of them attachments have taken.
 *
 * Attachment runs at boot, one board driver at a time, and takes the next
 * unit for good, failed or not; nothing gives a unit back.
 */
static struct drv_cadence_gem gem_units[DRV_CADENCE_GEM_UNIT_MAX];
static unsigned gem_unit_count;

static int check_config(const struct drv_cadence_gem_config *config);
static void read_configuration(struct drv_cadence_gem *gem);
static int choose_mac(struct drv_cadence_gem *gem);
static int find_phy(struct drv_cadence_gem *gem, uint32_t *identity);
static int allocate_buffers(struct drv_cadence_gem *gem);
static int publish_interface(struct drv_cadence_gem *gem);
static void reset_controller(struct drv_cadence_gem *gem);
static void initialize_rings(struct drv_cadence_gem *gem);
static void set_upper_words(struct drv_cadence_gem *gem, volatile uint32_t *descriptor, uint32_t high);
static void start_controller(struct drv_cadence_gem *gem);
static int stop_transmitter(struct drv_cadence_gem *gem);
static int wait_register(struct drv_cadence_gem *gem, unsigned offset, uint32_t mask, uint32_t wanted, unsigned microseconds);
static uint32_t read_register(struct drv_cadence_gem *gem, unsigned offset);
static void write_register(struct drv_cadence_gem *gem, unsigned offset, uint32_t bits);
static int wait_mdio(struct drv_cadence_gem *gem);
static int mdio_operate(struct drv_cadence_gem *gem, unsigned phy, uint32_t operation);
static int mdio_read(void *context, unsigned phy, unsigned reg, uint16_t *data);
static int mdio_write(void *context, unsigned phy, unsigned reg, uint16_t data);
static int read_link(struct drv_cadence_gem *gem, struct drv_mii_link *link);
static void update_link(struct drv_cadence_gem *gem);
static void reclaim_transmitted(struct drv_cadence_gem *gem);
static unsigned receive_packets(struct net_device *net, unsigned budget);
static void restart_receiver(struct drv_cadence_gem *gem);
static void gem_interrupt(void *argument);
static void poll_worker(void *argument);

/*
 * Resets a controller, brings up its PHY and publishes it as a network
 * interface.
 */
int
drv_cadence_gem_attach(
	const struct drv_cadence_gem_config *config,
	struct drv_cadence_gem **result)
{
	struct drv_cadence_gem *gem;
	const char *stage;
	uint32_t identity;
	int error;

	/* Refuses a description the driver cannot use, before a unit is taken. */
	error = check_config(config);
	if (error != 0) {
		kern_logf("gem: unusable controller description\n");
		return error;
	}

	/* Refuses a controller beyond the units the driver has. */
	if (gem_unit_count == DRV_CADENCE_GEM_UNIT_MAX) {
		kern_logf("gem: no unit left for %s\n", config->name);
		return EBUSY;
	}

	/* Takes the next unit for good and keeps the description. */
	gem = &gem_units[gem_unit_count];
	gem_unit_count++;
	gem->config = *config;
	gem->registers = config->registers;
	gem->phy_address = config->phy_address;

	/* Takes the descriptor format: four words for 64-bit addresses, two for 32-bit ones. */
	gem->descriptor_words = GEM_DESC_WORDS_32;
	if (config->descriptor_width == DRV_CADENCE_GEM_DESCRIPTOR_64)
		gem->descriptor_words = GEM_DESC_WORDS_64;

	/*
	 * The receive interrupts are held together while the network worker
	 * drains the ring; with the receive restart, a used descriptor's read
	 * is one of them, so a full ring does not restart receive on every
	 * frame that arrives.
	 */
	gem->rx_interrupts = GEM_IXR_RXDONE;
	if ((config->flags & DRV_CADENCE_GEM_FLAG_RESTART_RX_ON_USED_BIT) != 0)
		gem->rx_interrupts |= GEM_IXR_RXUBR;

	/* Prepares the lock of the rings and the registers, and the MDIO bus for the PHY. */
	spin_init(&gem->lock, LOCK_RANK_DEVICE, "Cadence GEM");
	gem->mdio.context = gem;
	gem->mdio.read = mdio_read;
	gem->mdio.write = mdio_write;

	/* Reads the controller's version, queues and MDIO divider. */
	read_configuration(gem);

	/* Takes the board's address before the reset clears the filters. */
	stage = "MAC address";
	error = choose_mac(gem);
	if (error != 0)
		goto fail;

	/* Resets the controller and turns the MDIO bus on. */
	reset_controller(gem);

	/* Has the board reset the PHY. */
	if (config->reset_phy != NULL) {
		stage = "PHY reset";
		error = config->reset_phy(config->context);
		if (error != 0)
			goto fail;
	}

	/* Finds the PHY on the bus. */
	stage = "PHY";
	error = find_phy(gem, &identity);
	if (error != 0)
		goto fail;

	/* Has the board start the PHY over MDIO. */
	if (config->setup_phy != NULL) {
		error = config->setup_phy(config->context, &gem->mdio, gem->phy_address, &identity);
		if (error != 0)
			goto fail;
	}

	/* Records the PHY found on the bus. */
	kern_logf("gem: PHY %u id %08x\n", gem->phy_address, identity);

	/* Allocates the rings and the slots before anything is published. */
	stage = "DMA buffers";
	error = allocate_buffers(gem);
	if (error != 0)
		goto fail;

	/* Routes the controller's interrupt; its sources stay disabled until open. */
	stage = "interrupt";
	error = config->establish_irq(config->context, gem_interrupt, gem);
	if (error != 0)
		goto fail;

	/* Publishes the interface. */
	error = publish_interface(gem);
	if (error != 0)
		return error;

	/* Succeeded: networkd can open the interface. */
	*result = gem;
	return 0;

fail:
	/* Reports where the attachment stopped; what was taken is kept, as nothing reaches it. */
	kern_logf("gem: initialization failed at %s (%d)\n", stage, error);
	return error;
}

/*
 * Starts the periodic service of the link and of missed interrupts.
 */
void
drv_cadence_gem_start_service(
	struct drv_cadence_gem *gem)
{
	int error;

	/* Leaves an absent interface or a running service alone. */
	if (gem->net == NULL || gem->poll_thread != NULL)
		return;

	/* Creates the service thread. */
	error = kthread_create(poll_worker, gem, SCHED_PRIORITY_DEFAULT, &gem->poll_thread);
	if (error != 0) {
		kern_logf("gem: periodic poll unavailable (%d)\n", error);
		return;
	}

	/* Holds the interface for the thread, which runs for as long as the kernel. */
	net_device_ref(gem->net);

	/* Makes the thread runnable. */
	thread_start(gem->poll_thread);
}

/*
 * Refuses a description without registers, DMA, an interrupt route or an
 * MDIO clock, with an unknown descriptor format, an impossible PHY
 * address, or a name the interface cannot carry.
 */
static int
check_config(
	const struct drv_cadence_gem_config *config)
{
	size_t length;

	/* Requires a description. */
	if (config == NULL)
		return EINVAL;

	/* Requires the mapped registers. */
	if (config->registers == NULL)
		return EINVAL;

	/* Requires the DMA device the rings and slots come from. */
	if (config->dma == NULL)
		return EINVAL;

	/* Requires the route of the controller's interrupt. */
	if (config->establish_irq == NULL)
		return EINVAL;

	/* Requires the clock the MDIO clock is divided from. */
	if (config->pclk_hz == 0)
		return EINVAL;

	/* Requires one of the two descriptor formats. */
	if (config->descriptor_width != DRV_CADENCE_GEM_DESCRIPTOR_32 &&
	    config->descriptor_width != DRV_CADENCE_GEM_DESCRIPTOR_64)
		return EINVAL;

	/* Requires a clause-22 PHY address. */
	if (config->phy_address > GEM_PHY_ADDRESS_MAX)
		return EINVAL;

	/* Requires a name. */
	if (config->name == NULL)
		return EINVAL;

	/* Requires the name to fit the interface with its terminator. */
	length = kern_strlen(config->name);
	if (length == 0 || length >= NET_DEVICE_NAME_MAX)
		return EINVAL;

	/* Succeeded: the description can be used. */
	return 0;
}

/*
 * Reads the controller's version and, on a version that has them, its
 * priority queues, screeners and DMA width, and chooses the MDIO divider.
 *
 * Procedure referenced: OpenBSD sys/dev/fdt/if_cad.c rev 1.17
 * (cad_attach(): CFG6 and CFG8 only from version 7 on; cad_reset(): the
 * smallest divider that keeps the MDIO clock at 2.5 MHz or below).
 */
static void
read_configuration(
	struct drv_cadence_gem *gem)
{
	uint32_t revision;
	uint32_t version;
	uint32_t config;
	uint64_t rate;
	unsigned divider;

	/* Reads the version; queue 0 is always present. */
	revision = read_register(gem, GEM_MID);
	version = (revision >> GEM_MID_VERSION_SHIFT) & GEM_MID_VERSION_MASK;
	gem->queue_mask = 1U;
	gem->type1_screeners = 0;
	gem->type2_screeners = 0;

	/* Reads the queues and the screeners of a version that has CFG6 and CFG8. */
	if (version >= GEM_VERSION_WITH_CFG) {
		/* Reads the queues the controller has, and whether it reports 64-bit DMA. */
		config = read_register(gem, GEM_CFG6);
		gem->queue_mask |= config & GEM_CFG6_PRIQ_MASK;
		if ((config & GEM_CFG6_DMA64) == 0)
			kern_logf("gem: no 64-bit DMA reported\n");

		/* Reads how many screeners of each type the controller has. */
		config = read_register(gem, GEM_CFG8);
		gem->type1_screeners = (config >> GEM_CFG8_TYPE1_SHIFT) & 0xffU;
		gem->type2_screeners = (config >> GEM_CFG8_TYPE2_SHIFT) & 0xffU;
	}

	/* Takes the smallest divider that keeps MDIO slow enough. */
	rate = gem->config.pclk_hz;
	for (divider = 0; divider < GEM_MDC_DIVIDER_COUNT - 1U; divider++) {
		/* Stops at the first divider that is slow enough. */
		if (rate / gem_mdc_dividers[divider] <= GEM_MDC_MAX_HZ)
			break;
	}

	/* Keeps the divider for every reset of the controller. */
	gem->mdc_divider = divider;

	/* Reports the controller. */
	kern_logf("gem: revision %08x, queues %04x, MDIO divider %u\n",
		  revision,
		  gem->queue_mask,
		  gem_mdc_dividers[divider]);
}

/*
 * Takes the board's address: the one the description carries, or else,
 * when the board allows it, one the firmware left in the controller's
 * filters.
 */
static int
choose_mac(
	struct drv_cadence_gem *gem)
{
	uint32_t low;
	uint32_t high;
	unsigned nonzero;
	unsigned index;

	/* Takes the described address. */
	kern_memcpy(gem->mac, gem->config.mac, sizeof(gem->mac));

	/* Finds out whether any octet of it is set. */
	nonzero = 0;
	for (index = 0; index < sizeof(gem->mac); index++)
		nonzero |= gem->mac[index];

	/* Keeps a described unicast address that is not all zeros. */
	if (nonzero != 0 && (gem->mac[0] & 1U) == 0)
		return 0;

	/* Refuses to invent an address when the filters are not to be read. */
	if ((gem->config.flags & DRV_CADENCE_GEM_FLAG_MAC_FROM_FILTERS) == 0) {
		kern_logf("gem: no Ethernet address from the firmware\n");
		return ENODEV;
	}

	/* Looks in the controller's filters for a unicast address the firmware left. */
	for (index = 0; index < GEM_LADDRNUM; index++) {
		low = read_register(gem, GEM_LADDRL + index * GEM_LADDR_STRIDE);
		high = read_register(gem, GEM_LADDRH + index * GEM_LADDR_STRIDE);

		/* Skips an empty filter and a multicast one. */
		if (low == 0 && (high & 0xffffU) == 0)
			continue;
		if ((low & 1U) != 0)
			continue;

		/* Unpacks the filter, the first octet in the low byte. */
		gem->mac[0] = (uint8_t)low;
		gem->mac[1] = (uint8_t)(low >> 8);
		gem->mac[2] = (uint8_t)(low >> 16);
		gem->mac[3] = (uint8_t)(low >> 24);
		gem->mac[4] = (uint8_t)high;
		gem->mac[5] = (uint8_t)(high >> 8);
		break;
	}

	/* Refuses to invent an address. */
	if (index == GEM_LADDRNUM) {
		kern_logf("gem: no Ethernet address from the firmware\n");
		return ENODEV;
	}

	/* Succeeded: a unicast address from the filters. */
	return 0;
}

/*
 * Settles the PHY's address: the described one, or, when the board asks
 * for a scan and nothing answers there, the first address that answers.
 * Stores the identity read, or zero when the board knows its PHY and no
 * MDIO frame is sent.
 */
static int
find_phy(
	struct drv_cadence_gem *gem,
	uint32_t *identity)
{
	unsigned address;
	int error;

	/* A board that knows its PHY leaves its identity to its own setup. */
	*identity = 0;
	if (!gem->config.phy_scan)
		return 0;

	/* Asks the described address first; a PHY there is the one. */
	error = drv_mii_phy_read_identity(&gem->mdio, gem->phy_address, identity);
	if (error == 0)
		return 0;

	/* Gives up on a bus that failed, rather than on a silent address. */
	if (error != ENXIO)
		return error;

	/* Asks every other address in turn and takes the first that answers. */
	for (address = 0; address <= GEM_PHY_ADDRESS_MAX; address++) {
		/* Skips the address already asked. */
		if (address == gem->phy_address)
			continue;

		/* Stops at a PHY and keeps its address. */
		error = drv_mii_phy_read_identity(&gem->mdio, address, identity);
		if (error == 0) {
			gem->phy_address = address;
			return 0;
		}

		/* Passes over a silent address, but not a bus that failed. */
		if (error != ENXIO)
			return error;
	}

	/* Reports a bus with no PHY on it. */
	kern_logf("gem: no PHY answered\n");
	return ENODEV;
}

/*
 * Allocates the descriptor rings (each with a spare descriptor for the
 * unused queues) and the receive and transmit slots, in memory the
 * controller reaches.
 */
static int
allocate_buffers(
	struct drv_cadence_gem *gem)
{
	size_t descriptor_bytes;
	int error;

	/* Allocates both rings, receive first. */
	descriptor_bytes = gem->descriptor_words * sizeof(uint32_t);
	error = drv_dma_alloc_coherent(gem->config.dma, 2U * (GEM_RING_COUNT + 1U) * descriptor_bytes, GEM_RING_ALIGN, &gem->rings);
	if (error != 0)
		return error;
	gem->rx_descriptors = gem->rings.address;
	gem->tx_descriptors = gem->rx_descriptors + (GEM_RING_COUNT + 1U) * gem->descriptor_words;

	/* Allocates the receive slots. */
	error = drv_dma_alloc_coherent(gem->config.dma, GEM_RING_COUNT * GEM_PACKET_BYTES, GEM_PACKET_ALIGN, &gem->rx);
	if (error != 0)
		return error;

	/* Allocates the transmit slots. */
	error = drv_dma_alloc_coherent(gem->config.dma, GEM_RING_COUNT * GEM_PACKET_BYTES, GEM_PACKET_ALIGN, &gem->tx);
	if (error != 0)
		return error;

	/* Succeeded: the rings and the slots are ready. */
	return 0;
}

/*
 * Describes the interface (the board's name and address, broadcast and
 * multicast) and publishes it.
 */
static int
publish_interface(
	struct drv_cadence_gem *gem)
{
	struct net_device *net;
	const char *stage;
	size_t length;
	int error;

	/* Obtains the interface's registry slot. */
	stage = "network allocation";
	net = net_device_alloc();
	if (net == NULL) {
		error = ENOMEM;
		goto fail;
	}

	/* Keeps the interface with the controller; the service and the interrupt reach it there. */
	gem->net = net;

	/* Describes the interface. */
	length = kern_strlen(gem->config.name);
	kern_memcpy(net->name, gem->config.name, length + 1U);
	net->mtu = 1500U;
	net->hwaddr_len = 6U;
	kern_memcpy(net->hwaddr, gem->mac, 6U);
	net->flags = NET_DEVICE_BROADCAST | NET_DEVICE_MULTICAST;
	net->ops = &gem_operations;
	net->driver_data = gem;

	/* Publishes it. */
	stage = "network registration";
	error = net_device_create(net);
	if (error != 0)
		goto fail;

	/* Reports the interface. */
	kern_logf("gem: %s ready MAC %02x:%02x:%02x:%02x:%02x:%02x\n",
		  net->name,
		  gem->mac[0],
		  gem->mac[1],
		  gem->mac[2],
		  gem->mac[3],
		  gem->mac[4],
		  gem->mac[5]);

	/* Succeeded: the interface is in the registry. */
	return 0;

fail:
	/* Reports where the publication stopped. */
	kern_logf("gem: initialization failed at %s (%d)\n", stage, error);
	return error;
}

/*
 * Stops the controller, empties its queue bases and screeners, and turns
 * the MDIO bus on at its divider.
 *
 * Procedure referenced: OpenBSD sys/dev/fdt/if_cad.c rev 1.17
 * (cad_reset()).
 */
static void
reset_controller(
	struct drv_cadence_gem *gem)
{
	uint32_t netcfg;
	unsigned queue;
	unsigned index;

	/* Stops traffic, masks every interrupt and clears the status. */
	write_register(gem, GEM_NETCTL, 0);
	write_register(gem, GEM_IDR, 0xffffffffU);
	write_register(gem, GEM_RXSR, 0);
	write_register(gem, GEM_TXSR, 0);

	/* Empties the upper halves of queue 0's bases, which only a 64-bit controller has. */
	if (gem->descriptor_words == GEM_DESC_WORDS_64) {
		write_register(gem, GEM_RXQBASEHI, 0);
		write_register(gem, GEM_TXQBASEHI, 0);
	}

	/* Empties queue 0's bases. */
	write_register(gem, GEM_RXQBASE, 0);
	write_register(gem, GEM_TXQBASE, 0);

	/* Empties the priority queues' bases. */
	for (queue = 1; queue < GEM_MAX_PRIQ; queue++) {
		/* Skips a queue the controller does not have. */
		if ((gem->queue_mask & (1U << queue)) == 0)
			continue;

		/* Empties the queue's receive base, in its bank. */
		if (queue < 8U) {
			write_register(gem, GEM_RXQ1BASE + (queue - 1U) * 4U, 0);
		} else {
			write_register(gem, GEM_RXQ8BASE + (queue - 8U) * 4U, 0);
		}

		/* Empties the queue's transmit base. */
		write_register(gem, GEM_TXQ1BASE + (queue - 1U) * 4U, 0);
	}

	/* Turns every type 1 screener off so all frames arrive on queue 0. */
	for (index = 0; index < gem->type1_screeners; index++)
		write_register(gem, GEM_SCR_TYPE1 + index * 4U, 0);

	/* Turns every type 2 screener off for the same reason. */
	for (index = 0; index < gem->type2_screeners; index++)
		write_register(gem, GEM_SCR_TYPE2 + index * 4U, 0);

	/* Sets the MDIO divider. */
	netcfg = read_register(gem, GEM_NETCFG);
	netcfg &= ~GEM_NETCFG_MDCCLKDIV_MASK;
	netcfg |= gem->mdc_divider << GEM_NETCFG_MDCCLKDIV_SHIFT;
	write_register(gem, GEM_NETCFG, netcfg);

	/* Turns the MDIO bus on. */
	gem->netctl = GEM_NETCTL_MDEN;
	write_register(gem, GEM_NETCTL, gem->netctl);
}

/*
 * Fills both rings: every receive descriptor owned by the controller and
 * pointing at its slot, every transmit descriptor owned by the driver,
 * and the spare descriptors closing the unused queues.  The caller holds
 * the lock with the controller stopped.
 */
static void
initialize_rings(
	struct drv_cadence_gem *gem)
{
	volatile uint32_t *descriptor;
	uint64_t address;
	uint32_t wrap;
	unsigned index;

	/* Points each receive descriptor at its slot, handed to the controller. */
	for (index = 0; index < GEM_RING_COUNT; index++) {
		descriptor = gem->rx_descriptors + index * gem->descriptor_words;
		address = gem->rx.device_address + (uint64_t)index * GEM_PACKET_BYTES;
		wrap = 0;
		if (index == GEM_RING_COUNT - 1U)
			wrap = GEM_RXD_ADDR_WRAP;
		set_upper_words(gem, descriptor, (uint32_t)(address >> 32));
		descriptor[GEM_DESC_STATUS] = 0;
		descriptor[GEM_DESC_ADDRESS_LOW] = (uint32_t)address | wrap;
	}

	/* Closes the receive ring's spare descriptor for the unused queues. */
	descriptor = gem->rx_descriptors + GEM_RING_COUNT * gem->descriptor_words;
	set_upper_words(gem, descriptor, 0);
	descriptor[GEM_DESC_STATUS] = 0;
	descriptor[GEM_DESC_ADDRESS_LOW] = GEM_RXD_ADDR_USED | GEM_RXD_ADDR_WRAP;

	/* Marks each transmit descriptor as the driver's. */
	for (index = 0; index < GEM_RING_COUNT; index++) {
		descriptor = gem->tx_descriptors + index * gem->descriptor_words;
		wrap = 0;
		if (index == GEM_RING_COUNT - 1U)
			wrap = GEM_TXD_WRAP;
		descriptor[GEM_DESC_ADDRESS_LOW] = 0;
		set_upper_words(gem, descriptor, 0);
		descriptor[GEM_DESC_STATUS] = GEM_TXD_USED | wrap;
	}

	/* Closes the transmit ring's spare descriptor. */
	descriptor = gem->tx_descriptors + GEM_RING_COUNT * gem->descriptor_words;
	descriptor[GEM_DESC_ADDRESS_LOW] = 0;
	set_upper_words(gem, descriptor, 0);
	descriptor[GEM_DESC_STATUS] = GEM_TXD_USED | GEM_TXD_WRAP;

	/* Starts the software cursors at the start of the rings. */
	gem->rx_next = 0;
	gem->tx_producer = 0;
	gem->tx_consumer = 0;

	/* Publishes the descriptors before the controller is given them. */
	kern_io_write_barrier();
}

/*
 * Writes a 64-bit descriptor's upper address bits and clears its reserved
 * word; a 32-bit descriptor has neither, and the next descriptor starts
 * where they would be.
 */
static void
set_upper_words(
	struct drv_cadence_gem *gem,
	volatile uint32_t *descriptor,
	uint32_t high)
{
	/* Leaves a two-word descriptor's neighbour alone. */
	if (gem->descriptor_words != GEM_DESC_WORDS_64)
		return;

	/* Writes the upper address bits and the reserved word. */
	descriptor[GEM_DESC_ADDRESS_HIGH] = high;
	descriptor[GEM_DESC_RESERVED] = 0;
}

/*
 * Gives the controller its rings, its address, its interface, its frame
 * format and its DMA settings, then starts receive and transmit.  The
 * caller holds the lock.
 *
 * Procedure referenced: OpenBSD sys/dev/fdt/if_cad.c rev 1.17 (cad_up():
 * the queue bases, the spare descriptor for the unused queues, the
 * address filter, NETCFG and DMACR, the statistics clear, then RXEN and
 * TXEN and the interrupt enables).
 */
static void
start_controller(
	struct drv_cadence_gem *gem)
{
	uint64_t rx_base;
	uint64_t tx_base;
	uint64_t rx_spare;
	uint64_t tx_spare;
	uint64_t ring_bytes;
	uint32_t netcfg;
	uint32_t dmacr;
	uint32_t low;
	uint32_t high;
	unsigned queue;
	unsigned index;

	/* Finds queue 0's rings: the receive ring, then the transmit ring, each with its spare. */
	ring_bytes = (uint64_t)(GEM_RING_COUNT + 1U) * gem->descriptor_words * sizeof(uint32_t);
	rx_base = gem->rings.device_address;
	tx_base = rx_base + ring_bytes;

	/* Gives a 64-bit controller the upper half of the receive ring's address. */
	if (gem->descriptor_words == GEM_DESC_WORDS_64)
		write_register(gem, GEM_RXQBASEHI, (uint32_t)(rx_base >> 32));

	/* Gives queue 0 its receive ring. */
	write_register(gem, GEM_RXQBASE, (uint32_t)rx_base);

	/* Gives a 64-bit controller the upper half of the transmit ring's address. */
	if (gem->descriptor_words == GEM_DESC_WORDS_64)
		write_register(gem, GEM_TXQBASEHI, (uint32_t)(tx_base >> 32));

	/* Gives queue 0 its transmit ring. */
	write_register(gem, GEM_TXQBASE, (uint32_t)tx_base);

	/* Points every priority queue at the spare descriptors and disables it. */
	rx_spare = rx_base + (uint64_t)GEM_RING_COUNT * gem->descriptor_words * sizeof(uint32_t);
	tx_spare = tx_base + (uint64_t)GEM_RING_COUNT * gem->descriptor_words * sizeof(uint32_t);
	for (queue = 1; queue < GEM_MAX_PRIQ; queue++) {
		/* Skips a queue the controller does not have. */
		if ((gem->queue_mask & (1U << queue)) == 0)
			continue;

		/* Closes the queue's receive base, in its bank. */
		if (queue < 8U) {
			write_register(gem, GEM_RXQ1BASE + (queue - 1U) * 4U, (uint32_t)rx_spare | GEM_QUEUE_BASE_DISABLE);
		} else {
			write_register(gem, GEM_RXQ8BASE + (queue - 8U) * 4U, (uint32_t)rx_spare | GEM_QUEUE_BASE_DISABLE);
		}

		/* Closes the queue's transmit base. */
		write_register(gem, GEM_TXQ1BASE + (queue - 1U) * 4U, (uint32_t)tx_spare | GEM_QUEUE_BASE_DISABLE);
	}

	/* Writes the station address into the first filter, the first octet in the low byte. */
	low = gem->mac[0];
	low |= (uint32_t)gem->mac[1] << 8;
	low |= (uint32_t)gem->mac[2] << 16;
	low |= (uint32_t)gem->mac[3] << 24;
	high = gem->mac[4];
	high |= (uint32_t)gem->mac[5] << 8;
	write_register(gem, GEM_LADDRL, low);
	write_register(gem, GEM_LADDRH, high);

	/* Empties the other filters. */
	for (index = 1; index < GEM_LADDRNUM; index++) {
		write_register(gem, GEM_LADDRL + index * GEM_LADDR_STRIDE, 0);
		write_register(gem, GEM_LADDRH + index * GEM_LADDR_STRIDE, 0);
	}

	/*
	 * Selects the RGMII interface on a board that wires the PHY that way
	 * through the user I/O register.
	 *
	 * Procedure referenced: Linux v6.12
	 * drivers/net/ethernet/cadence/macb_main.c (macb_init(): the user I/O
	 * value of an RGMII interface).
	 */
	if ((gem->config.flags & DRV_CADENCE_GEM_FLAG_USRIO_RGMII) != 0)
		write_register(gem, GEM_USRIO, GEM_USRIO_RGMII);

	/*
	 * Takes every frame, because the interface has no multicast list to
	 * filter by, and removes the FCS; broadcast is received, frames start
	 * at the slot's first byte, and the speed waits for the link.
	 */
	write_register(gem, GEM_HASHL, 0xffffffffU);
	write_register(gem, GEM_HASHH, 0xffffffffU);
	netcfg = read_register(gem, GEM_NETCFG);
	netcfg |= GEM_NETCFG_FCSREM;
	netcfg |= GEM_NETCFG_1536RXEN;
	netcfg |= GEM_NETCFG_FDEN;
	netcfg |= GEM_NETCFG_COPYALL;
	netcfg &= ~GEM_NETCFG_RXCSUMEN;
	netcfg &= ~GEM_NETCFG_UCASTHASHEN;
	netcfg &= ~GEM_NETCFG_MCASTHASHEN;
	netcfg &= ~GEM_NETCFG_BCASTDI;
	netcfg &= ~GEM_NETCFG_RXOFFS_MASK;
	netcfg &= ~GEM_NETCFG_SGMIIEN;
	netcfg &= ~GEM_NETCFG_PCSSEL;
	write_register(gem, GEM_NETCFG, netcfg);

	/* Reads the DMA settings the firmware left. */
	dmacr = read_register(gem, GEM_DMACR);

	/* Has a 64-bit controller use its four-word descriptors. */
	if (gem->descriptor_words == GEM_DESC_WORDS_64)
		dmacr |= GEM_DMACR_DMA64;

	/*
	 * Sets the descriptors in the CPU's byte order, 2 KiB receive slots,
	 * full-size packet memories and 16-beat bursts, without checksum
	 * offload.
	 */
	dmacr &= ~GEM_DMACR_ES_DESCR;
	dmacr &= ~GEM_DMACR_ES_PDATA;
	dmacr &= ~GEM_DMACR_TXCSUMEN;
	dmacr |= GEM_DMACR_AHBDISC;
	dmacr |= GEM_DMACR_TXSIZE;
	dmacr &= ~GEM_DMACR_RXSIZE_MASK;
	dmacr |= GEM_DMACR_RXSIZE_8K;
	dmacr &= ~GEM_DMACR_RXBUF_MASK;
	dmacr |= (GEM_PACKET_BYTES / 64U) << GEM_DMACR_RXBUF_SHIFT;
	dmacr &= ~GEM_DMACR_BLEN_MASK;
	dmacr |= GEM_DMACR_BLEN_16;
	write_register(gem, GEM_DMACR, dmacr);

	/* Clears the statistics. */
	write_register(gem, GEM_NETCTL, gem->netctl | GEM_NETCTL_STATCLR);

	/* Starts receive and transmit. */
	gem->netctl |= GEM_NETCTL_RXEN;
	gem->netctl |= GEM_NETCTL_TXEN;
	write_register(gem, GEM_NETCTL, gem->netctl);

	/* Clears what is pending and enables the interrupts the driver serves. */
	write_register(gem, GEM_ISR, 0xffffffffU);
	write_register(gem, GEM_IER, GEM_OTHER_INTERRUPTS | gem->rx_interrupts);
}

/*
 * Waits for the transmitter to go idle after transmit was turned off.
 * The caller holds the lock.
 */
static int
stop_transmitter(
	struct drv_cadence_gem *gem)
{
	int error;

	/* Lets the frame on the wire finish. */
	error = wait_register(gem, GEM_TXSR, GEM_TXSR_TXGO, 0, GEM_TX_IDLE_TIMEOUT_US);
	if (error != 0)
		return error;

	/* Succeeded: the transmitter is idle. */
	return 0;
}

/*
 * Waits until the masked bits of a register read as wanted, spinning on
 * the monotonic counter.  It never yields: the callers hold the device
 * lock with interrupts off, and a thread that slept here would leave the
 * lock held while another thread on the same CPU spun on it for ever.
 */
static int
wait_register(
	struct drv_cadence_gem *gem,
	unsigned offset,
	uint32_t mask,
	uint32_t wanted,
	unsigned microseconds)
{
	uint64_t start;
	uint64_t frequency;
	uint64_t now;
	uint64_t current_frequency;
	uint64_t duration;
	uint32_t bits;
	unsigned reads;
	bool counted;

	/* Fixes the deadline once, from one time base. */
	counted = kern_rtc_read_counter(&start, &frequency);
	if (!counted || frequency == 0)
		return EIO;

	/* Converts the wait to counter ticks, rounded up. */
	duration = frequency / 1000000U * microseconds;
	duration += (frequency % 1000000U * microseconds + 999999U) / 1000000U;

	/* Bounds the reads as well, in case the counter stops. */
	for (reads = 0; reads < GEM_WAIT_MAX_READS; reads++) {
		/* Stops once the register shows the wanted state. */
		bits = read_register(gem, offset);
		if ((bits & mask) == wanted)
			return 0;

		/* Refuses a changed or unavailable time base. */
		counted = kern_rtc_read_counter(&now, &current_frequency);
		if (!counted || current_frequency != frequency)
			return EIO;

		/* Reports a register that kept its state past the deadline. */
		if (now - start >= duration)
			return ETIMEDOUT;
	}

	/* Reports a wait that ran out of reads. */
	return ETIMEDOUT;
}

/* Reads one controller register. */
static uint32_t
read_register(
	struct drv_cadence_gem *gem,
	unsigned offset)
{
	uint32_t bits;

	/* Reads it through the uncached mapping. */
	bits = kern_mmio_read32(gem->registers + offset);

	/* Succeeded: the register's contents. */
	return bits;
}

/* Writes one controller register. */
static void
write_register(
	struct drv_cadence_gem *gem,
	unsigned offset,
	uint32_t bits)
{
	/* Writes it through the uncached mapping, in order with other device accesses. */
	kern_mmio_write32(gem->registers + offset, bits);
}

/*
 * Waits for the MDIO bus to finish its operation without yielding: the
 * link check runs it under the device lock.
 */
static int
wait_mdio(
	struct drv_cadence_gem *gem)
{
	int error;

	/* Waits for the idle bit. */
	error = wait_register(gem, GEM_NETSR, GEM_NETSR_PHY_MGMT_IDLE, GEM_NETSR_PHY_MGMT_IDLE, GEM_MDIO_TIMEOUT_US);
	if (error != 0)
		return error;

	/* Succeeded: the bus is idle. */
	return 0;
}

/*
 * Runs one clause-22 operation on a PHY's address.
 *
 * Procedure referenced: OpenBSD sys/dev/fdt/if_cad.c rev 1.17
 * (cad_mii_oper(): wait for idle, write the operation, wait for idle).
 */
static int
mdio_operate(
	struct drv_cadence_gem *gem,
	unsigned phy,
	uint32_t operation)
{
	int error;

	/* Joins the operation before. */
	error = wait_mdio(gem);
	if (error != 0)
		return error;

	/* Starts this one. */
	operation |= (phy & GEM_PHY_ADDRESS_MAX) << GEM_PHYMNTNC_ADDR_SHIFT;
	operation |= GEM_PHYMNTNC_CLAUSE_22;
	operation |= GEM_PHYMNTNC_MUST_10;
	write_register(gem, GEM_PHYMNTNC, operation);

	/* Waits for it to end. */
	error = wait_mdio(gem);
	if (error != 0)
		return error;

	/* Succeeded: the operation ended. */
	return 0;
}

/* Reads one PHY register. */
static int
mdio_read(
	void *context,
	unsigned phy,
	unsigned reg,
	uint16_t *data)
{
	struct drv_cadence_gem *gem;
	uint32_t bits;
	int error;

	/* Runs the read. */
	gem = context;
	error = mdio_operate(gem, phy, GEM_PHYMNTNC_OP_READ | ((reg & 0x1fU) << GEM_PHYMNTNC_REG_SHIFT));
	if (error != 0)
		return error;

	/* Succeeded: the register's value. */
	bits = read_register(gem, GEM_PHYMNTNC);
	*data = (uint16_t)(bits & GEM_PHYMNTNC_DATA_MASK);
	return 0;
}

/* Writes one PHY register. */
static int
mdio_write(
	void *context,
	unsigned phy,
	unsigned reg,
	uint16_t data)
{
	struct drv_cadence_gem *gem;
	int error;

	/* Runs the write. */
	gem = context;
	error = mdio_operate(gem, phy, GEM_PHYMNTNC_OP_WRITE | ((reg & 0x1fU) << GEM_PHYMNTNC_REG_SHIFT) | data);
	if (error != 0)
		return error;

	/* Succeeded: the PHY holds the value. */
	return 0;
}

/* Opens the interface with fresh rings. */
static int
gem_open(
	struct net_device *net)
{
	struct drv_cadence_gem *gem;
	unsigned long irq;
	unsigned link;

	/* Serializes the start against the interrupt and the transmit path. */
	gem = net->driver_data;
	irq = spin_lock_irqsave(&gem->lock);

	/* A repeated open keeps the rings as they are. */
	if (gem->opened != 0) {
		spin_unlock_irqrestore(&gem->lock, irq);
		return 0;
	}

	/* Resets the controller, fills the rings and starts it. */
	reset_controller(gem);
	initialize_rings(gem);
	start_controller(gem);

	/* Publishes opened once the rings belong to the controller, and reads the link. */
	gem->opened = 1;
	gem->link = 0;
	gem->speed = 0;
	gem->next_link_check = 0;
	update_link(gem);
	link = gem->link;

	spin_unlock_irqrestore(&gem->lock, irq);

	/* Announces the carrier outside the lock. */
	(void)net_device_set_carrier(net, (int)link);
	kern_logf("gem: %s RX/TX enabled\n", net->name);

	/* Succeeded: frames move by interrupt and the periodic service. */
	return 0;
}

/* Stops the interface: no interrupt, no transmit, the controller idle. */
static void
gem_close(
	struct net_device *net)
{
	struct drv_cadence_gem *gem;
	unsigned long irq;
	int error;

	/* Closes the producer gate and stops receive and transmit. */
	gem = net->driver_data;
	irq = spin_lock_irqsave(&gem->lock);

	gem->opened = 0;
	gem->link = 0;
	gem->speed = 0;
	write_register(gem, GEM_IDR, 0xffffffffU);
	gem->netctl &= ~GEM_NETCTL_RXEN;
	gem->netctl &= ~GEM_NETCTL_TXEN;
	write_register(gem, GEM_NETCTL, gem->netctl);
	error = stop_transmitter(gem);
	write_register(gem, GEM_ISR, 0xffffffffU);

	spin_unlock_irqrestore(&gem->lock, irq);

	/* Withdraws the carrier and reports a transmitter that did not stop. */
	(void)net_device_set_carrier(net, 0);
	if (error != 0)
		kern_logf("gem: transmitter not idle at close (%d); slots kept\n", error);
}

/* Copies one frame into the next transmit slot and starts the transmitter. */
static int
gem_transmit(
	struct net_device *net,
	struct packet_buf *packet)
{
	struct drv_cadence_gem *gem;
	volatile uint32_t *descriptor;
	uint8_t *payload;
	uint64_t address;
	uint32_t status;
	unsigned long irq;
	unsigned slot;
	size_t length;
	int error;

	/* Refuses a missing packet. */
	if (packet == NULL)
		return EINVAL;

	/* Consumes a frame that is too short or too long for one slot. */
	length = packet->length;
	if (length < GEM_HEADER_BYTES || length > GEM_MAX_FRAME) {
		packet_buf_free(packet);
		return EMSGSIZE;
	}

	/* Serializes transmit with completion and close. */
	gem = net->driver_data;
	error = 0;
	irq = spin_lock_irqsave(&gem->lock);

	/* Takes back the slots the controller has sent. */
	if (gem->opened != 0)
		reclaim_transmitted(gem);

	/* Refuses a closed interface, no link, or a full ring (one slot always stays the driver's). */
	if (gem->opened == 0 || gem->link == 0) {
		error = ENETDOWN;
	} else if (gem->tx_producer - gem->tx_consumer >= GEM_RING_COUNT - 1U) {
		error = ENOBUFS;
	}

	/* Hands one complete frame to the controller. */
	if (error == 0) {
		/* Copies the frame and pads a short one with zeros. */
		slot = gem->tx_producer % GEM_RING_COUNT;
		payload = (uint8_t *)gem->tx.address + (size_t)slot * GEM_PACKET_BYTES;
		kern_memcpy(payload, packet->data, length);
		if (length < GEM_MIN_FRAME) {
			kern_memset(payload + length, 0, GEM_MIN_FRAME - length);
			length = GEM_MIN_FRAME;
		}

		/* Points the descriptor at the slot. */
		descriptor = gem->tx_descriptors + slot * gem->descriptor_words;
		address = gem->tx.device_address + (uint64_t)slot * GEM_PACKET_BYTES;
		descriptor[GEM_DESC_ADDRESS_LOW] = (uint32_t)address;

		/* A 64-bit descriptor also takes the slot's upper address bits. */
		if (gem->descriptor_words == GEM_DESC_WORDS_64)
			descriptor[GEM_DESC_ADDRESS_HIGH] = (uint32_t)(address >> 32);

		/* Gives the descriptor to the controller in one write, after the address. */
		status = ((uint32_t)length & GEM_TXD_LEN_MASK) | GEM_TXD_LAST;
		if (slot == GEM_RING_COUNT - 1U)
			status |= GEM_TXD_WRAP;
		kern_io_write_barrier();
		descriptor[GEM_DESC_STATUS] = status;
		kern_io_write_barrier();

		/* Moves the producer and starts the transmitter. */
		gem->tx_producer++;
		write_register(gem, GEM_NETCTL, gem->netctl | GEM_NETCTL_STARTTX);
	}

	spin_unlock_irqrestore(&gem->lock, irq);

	/* Releases the stack's packet whatever happened. */
	packet_buf_free(packet);

	/* Reports a refused frame. */
	if (error != 0)
		return error;

	/* Succeeded: the slot holds the frame. */
	return 0;
}

/*
 * Takes back the transmit slots the controller marked as sent, counting
 * the frames it could not send.  The caller holds the lock.
 */
static void
reclaim_transmitted(
	struct drv_cadence_gem *gem)
{
	volatile uint32_t *descriptor;
	uint32_t status;
	unsigned slot;

	/* Walks the slots from the oldest in flight. */
	while (gem->tx_consumer != gem->tx_producer) {
		/* Stops at a slot the controller still owns. */
		slot = gem->tx_consumer % GEM_RING_COUNT;
		descriptor = gem->tx_descriptors + slot * gem->descriptor_words;
		status = descriptor[GEM_DESC_STATUS];
		if ((status & GEM_TXD_USED) == 0)
			break;

		/* Counts a frame the controller gave up on. */
		if ((status & (GEM_TXD_RLIMIT | GEM_TXD_CORRUPT | GEM_TXD_LCOLL)) != 0)
			gem->net->tx_errors++;

		/* Moves past the slot. */
		gem->tx_consumer++;
	}
}

/*
 * Reads the link through the board's hook, or through the PHY's standard
 * registers when the board has none.  The caller holds the lock.
 */
static int
read_link(
	struct drv_cadence_gem *gem,
	struct drv_mii_link *link)
{
	int error;

	/* Asks the board, which knows its PHY. */
	if (gem->config.read_link != NULL) {
		error = gem->config.read_link(gem->config.context, &gem->mdio, gem->phy_address, link);
		if (error != 0)
			return error;

		/* Succeeded: the board's reading. */
		return 0;
	}

	/* Reads the standard registers. */
	error = drv_mii_phy_read_link(&gem->mdio, gem->phy_address, link);
	if (error != 0)
		return error;

	/* Succeeded: the standard reading. */
	return 0;
}

/*
 * Reads the PHY's link at a bounded rate, has the board follow a new speed
 * with its transmit clock, and sets the controller's speed and duplex.
 * The caller holds the lock.
 *
 * Procedure referenced: OpenBSD sys/dev/fdt/if_cad.c rev 1.17
 * (cad_mii_statchg(): the duplex, 100 for 100 Mbit/s, 100 and 1000 for
 * 1000 Mbit/s, then the transmit clock).
 */
static void
update_link(
	struct drv_cadence_gem *gem)
{
	struct drv_mii_link link;
	uint32_t netcfg;
	uint64_t now;
	unsigned speed;
	int error;

	/* Samples the PHY at most once per interval. */
	now = clock_ticks();
	if (gem->next_link_check != 0 && now < gem->next_link_check)
		return;
	gem->next_link_check = now + kern_ms_to_ticks(GEM_LINK_MS);

	/* Reads the negotiated speed and duplex. */
	error = read_link(gem, &link);
	if (error != 0) {
		/* Reports a failing MDIO bus once, and stops traffic until it answers. */
		if (gem->mdio_fault == 0)
			kern_logf("gem: PHY status failed (%d)\n", error);
		gem->mdio_fault = 1;
		gem->link = 0;
		gem->speed = 0;
		gem->net->link_mbps = 0;
		return;
	}

	/* The PHY answered: a later failure is reported again. */
	gem->mdio_fault = 0;
	speed = link.speed_mbps;

	/* Reports a change of the link, and has the board's transmit clock follow a new speed. */
	if (speed != gem->speed) {
		/* Tells a pulled cable from a negotiated speed. */
		if (speed == 0) {
			kern_logf("gem: %s link down\n", gem->net->name);
		} else if (link.full_duplex) {
			kern_logf("gem: %s link up %u Mbps full duplex\n", gem->net->name, speed);
		} else {
			kern_logf("gem: %s link up %u Mbps half duplex\n", gem->net->name, speed);
		}

		/* Retimes the transmit clock of a board that clocks it for the speed. */
		if (speed != 0 && gem->config.set_tx_clock != NULL)
			gem->config.set_tx_clock(gem->config.context, speed);
	}

	/* Publishes the speed. */
	gem->speed = speed;
	gem->net->link_mbps = speed;
	gem->link = 0;
	if (speed != 0)
		gem->link = 1;

	/* Sets the controller's duplex as the link settled on it. */
	netcfg = read_register(gem, GEM_NETCFG);
	netcfg &= ~GEM_NETCFG_FDEN;
	if (link.full_duplex)
		netcfg |= GEM_NETCFG_FDEN;

	/* Sets the controller's speed: 100 for 100 Mbit/s, both bits for gigabit. */
	netcfg &= ~GEM_NETCFG_100;
	netcfg &= ~GEM_NETCFG_1000;
	if (speed == 1000U) {
		netcfg |= GEM_NETCFG_100;
		netcfg |= GEM_NETCFG_1000;
	} else if (speed == 100U) {
		netcfg |= GEM_NETCFG_100;
	}

	/* Writes the speed and the duplex. */
	write_register(gem, GEM_NETCFG, netcfg);
}

/* Delivers received frames within the network worker's budget. */
static unsigned
receive_packets(
	struct net_device *net,
	unsigned budget)
{
	struct drv_cadence_gem *gem;
	struct packet_buf *packet;
	volatile uint32_t *descriptor;
	uint64_t address;
	uint32_t address_low;
	uint32_t status;
	unsigned long irq;
	unsigned count;
	unsigned slot;
	size_t length;
	void *payload;
	int valid;

	/* Takes a finite batch so one interface cannot starve the stack. */
	gem = net->driver_data;
	for (count = 0; count < budget; count++) {
		/* Takes a stack packet before the lock. */
		packet = packet_buf_alloc(PACKET_BUF_DEFAULT_HEADROOM);
		valid = 0;
		irq = spin_lock_irqsave(&gem->lock);

		/* Stops at a slot the controller has not filled, or on a closed interface. */
		slot = gem->rx_next;
		descriptor = gem->rx_descriptors + slot * gem->descriptor_words;
		address_low = descriptor[GEM_DESC_ADDRESS_LOW];
		if (gem->opened == 0 || (address_low & GEM_RXD_ADDR_USED) == 0) {
			spin_unlock_irqrestore(&gem->lock, irq);
			packet_buf_free(packet);
			break;
		}

		/* Reads the status only after the ownership. */
		kern_io_read_barrier();
		status = descriptor[GEM_DESC_STATUS];
		length = status & GEM_RXD_LEN_MASK;

		/* Copies a whole frame of a sensible length out of the slot. */
		if ((status & (GEM_RXD_SOF | GEM_RXD_EOF)) == (GEM_RXD_SOF | GEM_RXD_EOF) &&
		    length >= GEM_HEADER_BYTES &&
		    length <= GEM_MAX_FRAME) {
			/* Appends the frame to the stack's packet. */
			if (packet != NULL) {
				payload = packet_buf_append(packet, length);
				if (payload != NULL) {
					kern_memcpy(payload, (const uint8_t *)gem->rx.address + (size_t)slot * GEM_PACKET_BYTES, length);
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

		/* Gives the slot back to the controller after the copy. */
		address = gem->rx.device_address + (uint64_t)slot * GEM_PACKET_BYTES;
		descriptor[GEM_DESC_STATUS] = 0;
		kern_io_barrier();
		if (slot == GEM_RING_COUNT - 1U) {
			descriptor[GEM_DESC_ADDRESS_LOW] = (uint32_t)address | GEM_RXD_ADDR_WRAP;
		} else {
			descriptor[GEM_DESC_ADDRESS_LOW] = (uint32_t)address;
		}

		/* Moves to the next slot. */
		gem->rx_next = (slot + 1U) % GEM_RING_COUNT;

		spin_unlock_irqrestore(&gem->lock, irq);

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

/* Serves the link, sent frames and received frames, then re-enables receive interrupts. */
static unsigned
gem_poll(
	struct net_device *net,
	unsigned budget)
{
	struct drv_cadence_gem *gem;
	unsigned long irq;
	unsigned link;
	unsigned count;

	/* Samples the link and takes back sent slots under the lock. */
	gem = net->driver_data;
	irq = spin_lock_irqsave(&gem->lock);

	/* A closed interface has no link to follow and no slots in flight. */
	if (gem->opened != 0) {
		update_link(gem);
		reclaim_transmitted(gem);
	}

	/* Takes the link for the announcement outside the lock. */
	link = gem->link;

	spin_unlock_irqrestore(&gem->lock, irq);

	/* Announces the carrier, then delivers a batch of frames. */
	(void)net_device_set_carrier(net, (int)link);
	count = receive_packets(net, budget);

	/* Re-enables the receive interrupts after the batch. */
	irq = spin_lock_irqsave(&gem->lock);

	/* A closed interface keeps its interrupts off. */
	if (gem->opened != 0)
		write_register(gem, GEM_IER, gem->rx_interrupts);

	spin_unlock_irqrestore(&gem->lock, irq);

	/* Succeeded: the frames handled. */
	return count;
}

/*
 * Turns receive off and on again, which restarts a receive DMA the
 * controller stopped on a used descriptor.  The caller holds the lock.
 *
 * Procedure referenced: Linux v6.12
 * drivers/net/ethernet/cadence/macb_main.c (macb_interrupt(): the RXUBR
 * case, receive enable cleared, a write barrier, receive enable set).
 */
static void
restart_receiver(
	struct drv_cadence_gem *gem)
{
	/* Turns receive off. */
	write_register(gem, GEM_NETCTL, gem->netctl & ~GEM_NETCTL_RXEN);

	/* Orders the two writes. */
	kern_io_write_barrier();

	/* Turns it on again. */
	write_register(gem, GEM_NETCTL, gem->netctl);
}

/*
 * Serves the controller's interrupt: clears what it raised, holds receive
 * interrupts until the network worker has drained the ring, restarts a
 * stopped receiver on a board that needs it, and stops the interface on
 * a DMA error.
 */
static void
gem_interrupt(
	void *argument)
{
	struct drv_cadence_gem *gem;
	unsigned long enabled;
	uint32_t pending;
	unsigned schedule;

	/* Reads and clears the raised causes. */
	gem = argument;
	schedule = 0;
	enabled = spin_lock_irqsave(&gem->lock);

	pending = read_register(gem, GEM_ISR);
	write_register(gem, GEM_ISR, pending);

	/* Restarts a receiver that read a used descriptor, on a board whose controller stops there. */
	if (gem->opened != 0 &&
	    (gem->config.flags & DRV_CADENCE_GEM_FLAG_RESTART_RX_ON_USED_BIT) != 0 &&
	    (pending & GEM_IXR_RXUBR) != 0)
		restart_receiver(gem);

	/* Holds receive interrupts until the worker drains a batch. */
	if (gem->opened != 0 && (pending & gem->rx_interrupts) != 0) {
		write_register(gem, GEM_IDR, gem->rx_interrupts);
		schedule = 1;
	}

	/* Has the worker take back sent slots. */
	if (gem->opened != 0 && (pending & GEM_IXR_TXDONE) != 0)
		schedule = 1;

	/* Counts a frame lost to a full ring. */
	if ((pending & GEM_IXR_RXOVR) != 0 && gem->net != NULL)
		gem->net->rx_errors++;

	/* Stops the interface after a DMA error, to limit the damage. */
	if ((pending & GEM_IXR_HRESP) != 0) {
		gem->netctl &= ~GEM_NETCTL_RXEN;
		gem->netctl &= ~GEM_NETCTL_TXEN;
		write_register(gem, GEM_NETCTL, gem->netctl);
		write_register(gem, GEM_IDR, 0xffffffffU);
		kern_logf("gem: DMA error, interface stopped\n");
	}

	spin_unlock_irqrestore(&gem->lock, enabled);

	/* Wakes the network worker outside the lock. */
	if (schedule != 0)
		net_device_schedule_poll(gem->net);
}

/* Schedules a service pass at the polling interval while the interface is open. */
static void
poll_worker(
	void *argument)
{
	struct drv_cadence_gem *gem;
	unsigned long irq;
	unsigned opened;
	uint64_t deadline;

	/* Serves the interface for as long as the kernel runs. */
	gem = argument;
	for (;;) {
		/* Samples whether the interface is open. */
		irq = spin_lock_irqsave(&gem->lock);

		opened = gem->opened;

		spin_unlock_irqrestore(&gem->lock, irq);

		/* Wakes the network worker while the interface has users. */
		if (opened != 0)
			net_device_schedule_poll(gem->net);

		/* Sleeps on scheduler ticks until the next pass. */
		deadline = sched_ticks() + kern_ms_to_ticks(GEM_POLL_MS);
		sched_sleep(deadline);
	}
}
