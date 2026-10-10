/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * BCM2711 GENET v5 Ethernet with the Raspberry Pi 4's BCM54213PE PHY.
 *
 * Ring descriptors reside in the controller's SRAM. Packet buffers reside
 * in uncached SCB-addressable RAM and are copied into the network packet
 * pool. A permanent platform owner retains both through close/reopen, so
 * hardware never reaches freed memory. No checksum or status-block offload
 * is enabled; the network stack receives ordinary Ethernet frames.
 */

#include "rpi4-ethernet.h"
#include <drivers/ethernet/bcm54213pe.h>
#include <drivers/generic/dma.h>
#include <kern/clock.h>
#include <kern/device-io.h>
#include <kern/irq.h>
#include <kern/kcrt.h>
#include <kern/klog.h>
#include <kern/lock.h>
#include <kern/net/net-device.h>
#include <kern/net/packet-buf.h>
#include <kern/pmem.h>
#include <kern/sched.h>
#include <kern/thread.h>
#include <uapi/errno.h>

/* GENET v5 register addresses and hardware-defined command bits. */
#define GENET_REVISION 0x000U
#define GENET_PORT 0x004U
#define GENET_RX_FLUSH 0x008U
#define GENET_TX_FLUSH 0x00cU
#define GENET_RGMII 0x08cU
#define GENET_IRQ_STATUS 0x200U
#define GENET_IRQ_CLEAR 0x208U
#define GENET_IRQ_MASK_SET 0x210U
#define GENET_IRQ_MASK_CLEAR 0x214U
#define GENET_IRQ1_CLEAR 0x248U
#define GENET_IRQ1_MASK_SET 0x250U
#define GENET_RX_CONTROL 0x300U
#define GENET_RX_CHECK 0x314U
#define GENET_BUFFER_SIZE 0x3b4U
#define GENET_TX_CONTROL 0x600U
#define GENET_MAC_COMMAND 0x808U
#define GENET_MAC_HIGH 0x80cU
#define GENET_MAC_LOW 0x810U
#define GENET_MAX_FRAME 0x814U
#define GENET_MAC_TX_FLUSH 0xb34U
#define GENET_COUNTER_RESET 0xd80U
#define GENET_MDIO 0xe14U
#define GENET_FILTER_ENABLE 0xe50U
#define GENET_RX_DESCRIPTORS 0x2000U
#define GENET_TX_DESCRIPTORS 0x4000U
#define GENET_RX_RING 0x3000U
#define GENET_TX_RING 0x5000U
#define GENET_RX_CONFIG 0x3040U
#define GENET_RX_DMA 0x3044U
#define GENET_RX_BURST 0x304cU
#define GENET_TX_CONFIG 0x5040U
#define GENET_TX_DMA 0x5044U
#define GENET_TX_BURST 0x504cU
#define GENET_IRQ_RX (1U << 13)
#define GENET_IRQ_TX (1U << 16)
#define GENET_PACKET_START (1U << 13)
#define GENET_PACKET_END (1U << 14)
#define GENET_RX_ERRORS 0x1fU
#define GENET_MDIO_BUSY (1U << 29)
#define GENET_MDIO_FAILED (1U << 28)
#define GENET_MAC_ENABLE 3U
#define GENET_MAC_PROMISCUOUS (1U << 4)
#define GENET_MAC_RESET (1U << 13)
#define GENET_MAC_LOOP (1U << 15)
#define GENET_RGMII_DISABLE_DELAY (1U << 16)
#define GENET_DMA_ENABLE ((1U << 17) | 1U)

/* Fixed hardware ring geometry and bounded software service intervals. */
#define GENET_RING_COUNT 256U
#define GENET_DESCRIPTOR_BYTES 12U
#define GENET_PACKET_BYTES 2048U
#define GENET_MAX_PACKET 1514U
#define GENET_RX_PADDING 2U
#define GENET_MDIO_TIMEOUT_US 2000U
#define GENET_POLL_MS 20U
#define GENET_LINK_MS 250U

#define RPI4_ETHERNET_FDT_LIMIT (2U * 1024U * 1024U)

/* One permanently attached platform NIC; the lock protects all live I/O. */
struct genet_device {
	struct spinlock lock;
	struct rpi4_ethernet_config config;
	struct drv_bcm54213pe phy;
	void *registers;
	struct drv_dma_device *dma;
	struct drv_dma_buffer rx;
	struct drv_dma_buffer tx;
	struct net_device *net;
	struct thread *poll_thread;
	uint16_t rx_consumer;
	uint16_t tx_producer;
	uint64_t next_link_check;
	unsigned opened;
	unsigned irq_registered;
	unsigned link;
	unsigned speed;
	unsigned mdio_fault;
};

/* The board has one GENET; successful attachment retains it until shutdown. */
static struct genet_device board_genet;

static int genet_open(struct net_device *net);
static void genet_close(struct net_device *net);
static int genet_transmit(struct net_device *net, struct packet_buf *packet);
static unsigned genet_poll(struct net_device *net, unsigned budget);

/* Network callbacks keep the platform-owned buffers across interface closes. */
static const struct net_device_ops genet_operations = {
	genet_open,
	genet_close,
	genet_transmit,
	genet_poll,
	NULL,
	NULL
};

static unsigned genet_poll_packets(struct net_device *net, unsigned budget);
static uint32_t read_register(struct genet_device *device, unsigned offset);
static void write_register(struct genet_device *device, unsigned offset, uint32_t bits);
static int wait_register(struct genet_device *device, unsigned offset, uint32_t mask, unsigned microseconds);
static int delay_microseconds(unsigned microseconds);
static int mdio_read(void *context, unsigned reg, uint16_t *data);
static int mdio_write(void *context, unsigned reg, uint16_t data);
static int reset_mac(struct genet_device *device);
static void initialize_rings(struct genet_device *device);
static void initialize_filter(struct genet_device *device);
static void update_link(struct genet_device *device);
static void genet_interrupt(int irq, kern_irq_ack_t acknowledge, void *argument);
static void poll_worker(void *argument);
static int allocate_buffers(struct genet_device *device);
static void release_unpublished(struct genet_device *device);
static int select_mac(struct genet_device *device);

static int attach_controller(const struct rpi4_ethernet_config *config);
static void start_polling(void);
static int describe_dma(const struct drv_fdt *fdt, uint32_t node, uint64_t *limit);
static int describe_phy(const struct drv_fdt *fdt, uint32_t node, struct rpi4_ethernet_config *config);

/*
 * Starts onboard Ethernet from the firmware tree without depending on PCIe.
 */
int
drv_rpi4_ethernet_init(
	uint64_t fdt_physical)
{
	struct rpi4_ethernet_config config;
	struct drv_fdt fdt;
	const void *blob;
	uint32_t node;
	int error;

	/* Maps the firmware blob through managed physical RAM. */
	blob = kern_pmem_to_kernel(fdt_physical);
	if (blob == NULL)
		return ENODEV;

	/* Opens the bounded Pi 4 firmware tree before searching its devices. */
	error = drv_fdt_open(&fdt, blob, RPI4_ETHERNET_FDT_LIMIT);
	if (error != 0)
		return error;

	/* Locates GENET v5 rather than assuming a fixed peripheral address. */
	error = drv_fdt_find_compatible(
		&fdt,
		"brcm,bcm2711-genet-v5",
		DRV_FDT_NO_NODE,
		&node);
	if (error != 0)
		return error;

	/* Translates board-specific resources into the hardware driver's description. */
	error = drv_rpi4_ethernet_describe(&fdt, node, &config);
	if (error != 0)
		return error;

	/* Starts the MAC, PHY, and network interface using the existing kernel APIs. */
	error = attach_controller(&config);
	if (error != 0)
		return error;

	/* Succeeded: the onboard interface is available independently of USB. */
	return 0;
}

/*
 * Starts periodic onboard Ethernet service after platform discovery.
 */
void
drv_rpi4_ethernet_refresh(
	void)
{
	/* Delegates link and completion service to the hardware owner. */
	start_polling();
}

/*
 * Describes an enabled GENET controller without accessing its registers.
 */
int
drv_rpi4_ethernet_describe(
	const struct drv_fdt *fdt,
	uint32_t node,
	struct rpi4_ethernet_config *config)
{
	const uint8_t *property;
	uint32_t length;
	uint64_t interrupt;
	bool enabled;
	int error;

	/* Requires storage for the firmware description. */
	if (fdt == NULL || config == NULL)
		return EINVAL;

	/* Leaves firmware-disabled hardware alone. */
	enabled = drv_fdt_node_enabled(fdt, node);
	if (!enabled)
		return ENODEV;

	/* Resolves the SCB address through the parent bus ranges. */
	kern_memset(config, 0, sizeof(*config));
	error = drv_fdt_reg(fdt, node, 0, &config->physical, &config->size);
	if (error != 0)
		return error;

	/* Requires the complete register bank and a representable mapping. */
	if (config->size < 0x10000U || config->size > SIZE_MAX)
		return EINVAL;

	/* Requires the primary GIC SPI to be active-high level-triggered. */
	error = drv_fdt_property(fdt, node, "interrupts", &property, &length);
	if (error != 0)
		return error;

	/* Rejects truncated interrupt cells. */
	if (length < 12U)
		return EINVAL;

	/* Accepts the BCM2711 GIC binding rather than guessing an IRQ number. */
	interrupt = drv_fdt_cells_value(property, 0, 1);
	if (interrupt != 0)
		return ENOTSUP;

	/* Restricts the SPI to the GIC's external interrupt range. */
	interrupt = drv_fdt_cells_value(property, 1, 1);
	if (interrupt > 987U)
		return EINVAL;
	config->irq = (unsigned)interrupt + 32U;

	/* Refuses a trigger that the driver's level acknowledgement cannot serve. */
	interrupt = drv_fdt_cells_value(property, 2, 1);
	if (interrupt != 4U)
		return ENOTSUP;

	/* Finds the physical RAM window that the SCB DMA engine can reach. */
	error = describe_dma(fdt, node, &config->dma_limit);
	if (error != 0)
		return error;

	/* Resolves the external PHY and its clock delays. */
	error = describe_phy(fdt, node, config);
	if (error != 0)
		return error;

	/* Prefers an explicit MAC override to the firmware's local address. */
	error = drv_fdt_property(fdt, node, "mac-address", &property, &length);
	if (error == ENOENT) {
		error = drv_fdt_property(
			fdt,
			node,
			"local-mac-address",
			&property,
			&length);
	}

	/* Preserves the pre-reset UMAC fallback when firmware supplies no MAC. */
	if (error == ENOENT)
		return 0;

	/* Reports malformed MAC properties rather than partially copying one. */
	if (error != 0)
		return error;

	/* Requires all six Ethernet address octets. */
	if (length != sizeof(config->mac))
		return EINVAL;

	/* Keeps the address in network byte order. */
	kern_memcpy(config->mac, property, sizeof(config->mac));

	/* Succeeded: the platform can map and start this controller. */
	return 0;
}

/* Starts GENET from board resources and binds its external PHY through MDIO. */
static int
attach_controller(
	const struct rpi4_ethernet_config *config)
{
	struct genet_device *device;
	uint32_t revision;
	uint32_t phy_identity;
	int error;

	/* Rejects repeated attachment without changing the existing interface. */
	device = &board_genet;
	if (device->registers != NULL)
		return EALREADY;

	/* Requires a complete platform description before touching controller MMIO. */
	if (config == NULL ||
	    config->size < 0x10000U ||
	    config->size > SIZE_MAX ||
	    config->phy_address > 31U ||
	    config->irq > 1019U)
		return EINVAL;

	/* Copies the platform-owned description into the permanent hardware owner. */
	device->config = *config;

	/* Maps peripheral registers with the existing kernel device attributes. */
	spin_init(&device->lock, LOCK_RANK_DEVICE, "GENET");
	error = kern_device_map(
		device->config.physical,
		(size_t)device->config.size,
		KERN_DEVICE_UNCACHED,
		&device->registers);
	if (error != 0)
		return error;

	/* Verifies the silicon's v5 revision before resetting anything. */
	revision = read_register(device, GENET_REVISION);
	kern_logf(
		"genet: at %llx revision %08x irq %u\n",
		(unsigned long long)device->config.physical,
		revision,
		device->config.irq);
	if (((revision >> 24) & 15U) != 6U) {
		error = ENOTSUP;
		goto fail;
	}

	/* Saves a valid firmware or pre-reset hardware Ethernet address. */
	error = select_mac(device);
	if (error != 0)
		goto fail;

	/* Masks both interrupt banks and removes inherited MAC/DMA traffic. */
	write_register(device, GENET_IRQ_MASK_SET, UINT32_MAX);
	write_register(device, GENET_IRQ1_MASK_SET, UINT32_MAX);
	write_register(device, GENET_RX_DMA, 0);
	write_register(device, GENET_TX_DMA, 0);
	error = reset_mac(device);
	if (error != 0)
		goto fail;

	/* Sets the external gigabit PHY port before issuing clause-22 transactions. */
	write_register(device, GENET_PORT, 3U);
	device->phy.context = device;
	device->phy.read = mdio_read;
	device->phy.write = mdio_write;
	device->phy.rx_delay = config->rx_delay;
	device->phy.tx_delay = config->tx_delay;
	error = drv_bcm54213pe_initialize(&device->phy, &phy_identity);
	if (error != 0)
		goto fail;

	/* Records the external PHY detected through the platform MDIO engine. */
	kern_logf(
		"genet: PHY %u id %08x\n",
		device->config.phy_address,
		phy_identity);

	/* Allocates uncached packet slots before publishing a network interface. */
	error = allocate_buffers(device);
	if (error != 0)
		goto fail;

	/* Configures the primary SPI while both controller interrupt banks are masked. */
	kern_irq_mask((int)device->config.irq);
	error = kern_irq_set_mode(
		(int)device->config.irq,
		KERN_IRQ_TRIGGER_LEVEL,
		KERN_IRQ_POLARITY_HIGH);
	if (error != 0)
		goto fail;

	/* Binds DMA completion to the existing network worker. */
	error = kern_irq_register(
		(int)device->config.irq,
		genet_interrupt,
		device);
	if (error != 0)
		goto fail;
	device->irq_registered = 1;

	/* Obtains a registry slot only after all hardware resources are ready. */
	device->net = net_device_alloc();
	if (device->net == NULL) {
		error = ENOMEM;
		goto fail;
	}

	/* Names the onboard NIC independently of any USB Ethernet adapter. */
	kern_memcpy(device->net->name, "en0", 4U);
	device->net->mtu = 1500U;
	device->net->hwaddr_len = 6U;
	kern_memcpy(device->net->hwaddr, device->config.mac, 6U);
	device->net->flags = NET_DEVICE_BROADCAST | NET_DEVICE_MULTICAST;
	device->net->ops = &genet_operations;
	device->net->driver_data = device;
	error = net_device_create(device->net);
	if (error != 0)
		goto fail;

	/* Retains the allocation owner's reference for this permanent platform device. */
	kern_logf(
		"genet: en0 ready MAC %02x:%02x:%02x:%02x:%02x:%02x\n",
		device->config.mac[0],
		device->config.mac[1],
		device->config.mac[2],
		device->config.mac[3],
		device->config.mac[4],
		device->config.mac[5]);

	/* Succeeded: networkd can open the interface and obtain a DHCP lease. */
	return 0;

fail:
	/* Removes only unpublished resources; no hardware has seen these buffers. */
	kern_logf("genet: initialization failed (%d)\n", error);
	release_unpublished(device);

	/* Reports the initialization failure to the platform's nonfatal caller. */
	return error;
}

/* Starts link and missed-interrupt polling after platform discovery. */
static void
start_polling(
	void)
{
	struct genet_device *device;
	int error;

	/* Leaves an absent or already serviced controller alone. */
	device = &board_genet;
	if (device->net == NULL || device->poll_thread != NULL)
		return;

	/* Creates a low-frequency service thread, separate from the packet worker. */
	error = kthread_create(
		poll_worker,
		device,
		SCHED_PRIORITY_DEFAULT,
		&device->poll_thread);
	if (error != 0) {
		kern_logf("genet: periodic poll unavailable (%d)\n", error);
		return;
	}

	/* Retains the interface slot until the permanent service thread ends at shutdown. */
	net_device_ref(device->net);

	/* Makes the permanent board service runnable. */
	thread_start(device->poll_thread);
}

/* Reads an ordered register from the controller's device mapping. */
static uint32_t
read_register(
	struct genet_device *device,
	unsigned offset)
{
	uint32_t bits;

	/* Samples the controller through the kernel's MMIO accessor. */
	bits = kern_mmio_read32((const uint8_t *)device->registers + offset);

	/* Reports the hardware register contents. */
	return bits;
}

/* Writes an ordered register in the controller's device mapping. */
static void
write_register(
	struct genet_device *device,
	unsigned offset,
	uint32_t bits)
{
	/* Publishes the register update with device-access ordering. */
	kern_mmio_write32((uint8_t *)device->registers + offset, bits);
}

/* Waits for a controller command without yielding while a device lock is held. */
static int
wait_register(
	struct genet_device *device,
	unsigned offset,
	uint32_t mask,
	unsigned microseconds)
{
	uint64_t start;
	uint64_t frequency;
	uint64_t now;
	uint64_t current_frequency;
	uint64_t duration;
	uint32_t bits;
	bool counter_ready;
	unsigned attempts;

	/* Requires a monotonic counter so command deadlines cannot grow per poll. */
	counter_ready = kern_rtc_read_counter(&start, &frequency);
	if (!counter_ready || frequency == 0)
		return EIO;
	duration = frequency / 1000000U * microseconds;
	duration += (frequency % 1000000U * microseconds + 999999U) / 1000000U;

	/* Bounds both wall time and iterations if the counter ever stops advancing. */
	for (attempts = 0; attempts < 1000000U; attempts++) {
		/* Completes as soon as hardware retires the command. */
		bits = read_register(device, offset);
		if ((bits & mask) == 0)
			return 0;

		/* Rejects a changed or unavailable time base. */
		counter_ready = kern_rtc_read_counter(&now, &current_frequency);
		if (!counter_ready || current_frequency != frequency)
			return EIO;

		/* Reports a command that exceeded its original deadline. */
		if (now - start >= duration)
			return ETIMEDOUT;
	}

	/* Refuses a controller or counter that never completed within the bound. */
	return ETIMEDOUT;
}

/* Waits a short reset interval using the monotonic counter without sleeping. */
static int
delay_microseconds(
	unsigned microseconds)
{
	uint64_t start;
	uint64_t frequency;
	uint64_t now;
	uint64_t current_frequency;
	uint64_t duration;
	bool counter_ready;
	unsigned attempts;

	/* Samples one time base for the entire reset hold interval. */
	counter_ready = kern_rtc_read_counter(&start, &frequency);
	if (!counter_ready || frequency == 0)
		return EIO;
	duration = frequency / 1000000U * microseconds;
	duration += (frequency % 1000000U * microseconds + 999999U) / 1000000U;

	/* Caps a stuck counter independently of the hardware timer IRQ. */
	for (attempts = 0; attempts < 1000000U; attempts++) {
		/* Stops on a time-base failure rather than waiting forever. */
		counter_ready = kern_rtc_read_counter(&now, &current_frequency);
		if (!counter_ready || current_frequency != frequency)
			return EIO;

		/* Succeeded: the hardware reset was held for its minimum duration. */
		if (now - start >= duration)
			return 0;
	}

	/* Reports a nonadvancing counter to the reset caller. */
	return ETIMEDOUT;
}

/* Reads one clause-22 PHY register and checks the MDIO completion status. */
static int
mdio_read(
	void *context,
	unsigned reg,
	uint16_t *data)
{
	struct genet_device *device;
	uint32_t command;
	int error;

	/* Resolves the GENET owner for this PHY transaction. */
	device = context;

	/* Joins any previous transaction before replacing the command register. */
	error = wait_register(
		device,
		GENET_MDIO,
		GENET_MDIO_BUSY,
		GENET_MDIO_TIMEOUT_US);
	if (error != 0)
		return error;

	/* Starts a read at the firmware-described PHY address. */
	command = GENET_MDIO_BUSY | (1U << 27);
	command |= device->config.phy_address << 21;
	command |= reg << 16;
	write_register(device, GENET_MDIO, command);
	error = wait_register(
		device,
		GENET_MDIO,
		GENET_MDIO_BUSY,
		GENET_MDIO_TIMEOUT_US);
	if (error != 0)
		return error;

	/* Refuses a completed read for which no PHY answered. */
	command = read_register(device, GENET_MDIO);
	if ((command & GENET_MDIO_FAILED) != 0)
		return ENXIO;
	*data = (uint16_t)command;

	/* Succeeded: data contains the PHY register. */
	return 0;
}

/* Writes one clause-22 register after the previous MDIO command retires. */
static int
mdio_write(
	void *context,
	unsigned reg,
	uint16_t data)
{
	struct genet_device *device;
	uint32_t command;
	int error;

	/* Resolves the GENET owner for this PHY transaction. */
	device = context;

	/* Serializes this write with the preceding PHY transaction. */
	error = wait_register(
		device,
		GENET_MDIO,
		GENET_MDIO_BUSY,
		GENET_MDIO_TIMEOUT_US);
	if (error != 0)
		return error;

	/* Starts a bounded clause-22 write. */
	command = GENET_MDIO_BUSY | (1U << 26);
	command |= device->config.phy_address << 21;
	command |= reg << 16;
	command |= data;
	write_register(device, GENET_MDIO, command);
	error = wait_register(
		device,
		GENET_MDIO,
		GENET_MDIO_BUSY,
		GENET_MDIO_TIMEOUT_US);
	if (error != 0)
		return error;

	/* Succeeded: the MDIO engine has retired the write. */
	return 0;
}

/* Removes firmware traffic and resets UniMAC with a stable local-loop clock. */
static int
reset_mac(
	struct genet_device *device)
{
	uint32_t flush;
	int error;

	/* Pulses the receive-buffer reset without changing other flush controls. */
	flush = read_register(device, GENET_RX_FLUSH);
	write_register(device, GENET_RX_FLUSH, flush | 2U);
	error = delay_microseconds(10U);
	if (error != 0)
		return error;

	/* Deasserts receive reset before releasing the receive buffer. */
	write_register(device, GENET_RX_FLUSH, flush & ~2U);
	error = delay_microseconds(10U);
	if (error != 0)
		return error;

	/* Gives the cleared receive buffer a full reset interval. */
	write_register(device, GENET_RX_FLUSH, 0);
	error = delay_microseconds(10U);
	if (error != 0)
		return error;

	/* Resets UniMAC while the local-loop clock is independent of PHY carrier. */
	write_register(device, GENET_MAC_COMMAND, 0);
	write_register(
		device,
		GENET_MAC_COMMAND,
		GENET_MAC_RESET | GENET_MAC_LOOP);
	error = delay_microseconds(10U);
	if (error != 0)
		return error;

	/* Leaves MAC traffic disabled and clears inherited statistics. */
	write_register(device, GENET_MAC_COMMAND, 0);
	write_register(device, GENET_COUNTER_RESET, 7U);
	write_register(device, GENET_COUNTER_RESET, 0);

	/* Succeeded: ring and filter setup can start without firmware traffic. */
	return 0;
}

/* Allocates permanent uncached RX and TX storage within the SCB RAM window. */
static int
allocate_buffers(
	struct genet_device *device)
{
	struct drv_dma_constraints constraints;
	int error;

	/* Constrains DMA to low physical RAM and disables cached CPU aliases. */
	kern_memset(&constraints, 0, sizeof(constraints));
	constraints.address_bits = 32U;
	constraints.max_segment_size = GENET_RING_COUNT * GENET_PACKET_BYTES;
	constraints.coherent = 0;
	error = drv_dma_device_create(&constraints, &device->dma);
	if (error != 0)
		return error;

	/* Allocates all receive slots before exposing their addresses to hardware. */
	error = drv_dma_alloc_coherent(
		device->dma,
		GENET_RING_COUNT * GENET_PACKET_BYTES,
		64U,
		&device->rx);
	if (error != 0)
		return error;

	/* Allocates transmit slots separately so either allocation failure unwinds. */
	error = drv_dma_alloc_coherent(
		device->dma,
		GENET_RING_COUNT * GENET_PACKET_BYTES,
		64U,
		&device->tx);
	if (error != 0)
		return error;

	/* Refuses buffers outside the FDT's physical RAM DMA window. */
	if (device->rx.device_address > device->config.dma_limit ||
	    device->rx.size - 1U >
		device->config.dma_limit - device->rx.device_address)
		return EOVERFLOW;

	/* Checks the second run independently to preserve subtraction bounds. */
	if (device->tx.device_address > device->config.dma_limit ||
	    device->tx.size - 1U >
		device->config.dma_limit - device->tx.device_address)
		return EOVERFLOW;

	/* Succeeded: descriptor slots can reference permanent uncached payloads. */
	return 0;
}

/* Releases an attachment that never published or started DMA rings. */
static void
release_unpublished(
	struct genet_device *device)
{
	int error;

	/* Retires an unpublished network slot while its static owner is still valid. */
	if (device->net != NULL) {
		net_device_destroy(device->net);
		device->net = NULL;
	}

	/* Unbinds the masked interrupt before removing the register mapping. */
	if (device->irq_registered != 0) {
		error = kern_irq_unregister(
			(int)device->config.irq,
			genet_interrupt,
			device);
		if (error != 0) {
			kern_logf("genet: IRQ release failed (%d); resources "
				  "retained\n",
				  error);
			return;
		}

		/* Records that no handler can reach the retained mapping. */
		device->irq_registered = 0;
	}

	/* Returns each DMA allocation to its existing subsystem owner. */
	if (device->rx.address != NULL)
		drv_dma_free_coherent(device->dma, &device->rx);

	/* Releases the transmit allocation even after a receive-allocation failure. */
	if (device->tx.address != NULL)
		drv_dma_free_coherent(device->dma, &device->tx);

	/* Preserves a refused allocation release so its owner can retry safely. */
	if (device->rx.address != NULL || device->tx.address != NULL) {
		kern_logf("genet: DMA release refused; resources retained\n");
		return;
	}

	/* Drops the DMA allocator only after both payloads have been released. */
	if (device->dma != NULL) {
		error = drv_dma_device_destroy(device->dma);
		if (error != 0) {
			kern_logf(
				"genet: DMA owner release failed (%d)\n",
				error);
			return;
		}

		/* Clears the allocator handle only after destruction succeeds. */
		device->dma = NULL;
	}

	/* Removes the unused MMIO range and diagnoses a refused release. */
	if (device->registers != NULL) {
		error = kern_device_unmap(
			device->registers,
			(size_t)device->config.size);
		if (error != 0) {
			kern_logf("genet: MMIO release failed (%d)\n", error);
			return;
		}
	}

	/* Resets the static owner so a later explicit platform retry is possible. */
	kern_memset(device, 0, sizeof(*device));
}

/* Keeps a usable firmware MAC or reads the inherited UniMAC address. */
static int
select_mac(
	struct genet_device *device)
{
	uint32_t high;
	uint32_t low;
	unsigned i;
	unsigned nonzero;

	/* Checks whether firmware supplied a unicast nonzero address. */
	nonzero = 0;
	for (i = 0; i < 6U; i++)
		nonzero |= device->config.mac[i];

	/* Uses the firmware's unicast override when present. */
	if (nonzero != 0 && (device->config.mac[0] & 1U) == 0)
		return 0;

	/* Reads the pre-reset UniMAC address in its register byte order. */
	high = read_register(device, GENET_MAC_HIGH);
	low = read_register(device, GENET_MAC_LOW);
	device->config.mac[0] = (uint8_t)(high >> 24);
	device->config.mac[1] = (uint8_t)(high >> 16);
	device->config.mac[2] = (uint8_t)(high >> 8);
	device->config.mac[3] = (uint8_t)high;
	device->config.mac[4] = (uint8_t)(low >> 8);
	device->config.mac[5] = (uint8_t)low;

	/* Requires a genuine address rather than manufacturing a duplicate NIC MAC. */
	nonzero = high | (low & 0xffffU);
	if (nonzero == 0 || (device->config.mac[0] & 1U) != 0)
		return ENODEV;

	/* Succeeded: the initial hardware address survived in the platform owner. */
	return 0;
}

/* Configures one hardware ring per direction in controller SRAM. */
static void
initialize_rings(
	struct genet_device *device)
{
	uint64_t address;
	unsigned descriptor;
	unsigned index;

	/* Returns all software sequence counters to the freshly reset rings. */
	device->rx_consumer = 0;
	device->tx_producer = 0;

	/* Fills every receive slot before the receive engine can consume it. */
	for (index = 0; index < GENET_RING_COUNT; index++) {
		/* Gives each descriptor a unique aligned uncached RAM payload. */
		descriptor =
		    GENET_RX_DESCRIPTORS + index * GENET_DESCRIPTOR_BYTES;
		address =
		    device->rx.device_address + index * GENET_PACKET_BYTES;
		write_register(device, descriptor + 4U, (uint32_t)address);
		write_register(
			device,
			descriptor + 8U,
			(uint32_t)(address >> 32));
		write_register(device, descriptor, 0);

		/* Clears stale transmit descriptor state before a later producer publish. */
		descriptor =
		    GENET_TX_DESCRIPTORS + index * GENET_DESCRIPTOR_BYTES;
		write_register(device, descriptor, 0);
		write_register(device, descriptor + 4U, 0);
		write_register(device, descriptor + 8U, 0);
	}

	/* Describes the entire default receive ring in 32-bit SRAM words. */
	write_register(device, GENET_RX_BURST, 8U);
	write_register(device, GENET_RX_RING + 0x00U, 0);
	write_register(device, GENET_RX_RING + 0x04U, 0);
	write_register(device, GENET_RX_RING + 0x08U, 0);
	write_register(device, GENET_RX_RING + 0x0cU, 0);
	write_register(
		device,
		GENET_RX_RING + 0x10U,
		(GENET_RING_COUNT << 16) | GENET_PACKET_BYTES);
	write_register(device, GENET_RX_RING + 0x14U, 0);
	write_register(device, GENET_RX_RING + 0x18U, 0);
	write_register(
		device,
		GENET_RX_RING + 0x1cU,
		GENET_RING_COUNT * GENET_DESCRIPTOR_BYTES / 4U - 1U);
	write_register(device, GENET_RX_RING + 0x20U, 0);
	write_register(device, GENET_RX_RING + 0x24U, 1U);
	write_register(
		device,
		GENET_RX_RING + 0x28U,
		(5U << 16) | (GENET_RING_COUNT >> 4));
	write_register(device, GENET_RX_RING + 0x2cU, 0);
	write_register(device, GENET_RX_RING + 0x30U, 0);
	write_register(device, GENET_RX_CONFIG, 1U << 16);

	/* Describes the default transmit ring without priority queues or coalescing. */
	write_register(device, GENET_TX_BURST, 8U);
	write_register(device, GENET_TX_RING + 0x00U, 0);
	write_register(device, GENET_TX_RING + 0x04U, 0);
	write_register(device, GENET_TX_RING + 0x08U, 0);
	write_register(device, GENET_TX_RING + 0x0cU, 0);
	write_register(
		device,
		GENET_TX_RING + 0x10U,
		(GENET_RING_COUNT << 16) | GENET_PACKET_BYTES);
	write_register(device, GENET_TX_RING + 0x14U, 0);
	write_register(device, GENET_TX_RING + 0x18U, 0);
	write_register(
		device,
		GENET_TX_RING + 0x1cU,
		GENET_RING_COUNT * GENET_DESCRIPTOR_BYTES / 4U - 1U);
	write_register(device, GENET_TX_RING + 0x20U, 0);
	write_register(device, GENET_TX_RING + 0x24U, 1U);
	write_register(device, GENET_TX_RING + 0x28U, 0);
	write_register(device, GENET_TX_RING + 0x2cU, 0);
	write_register(device, GENET_TX_RING + 0x30U, 0);
	write_register(device, GENET_TX_CONFIG, 1U << 16);

	/* Publishes all SRAM descriptors and uncached RAM before enabling DMA. */
	kern_io_write_barrier();
	write_register(device, GENET_RX_DMA, GENET_DMA_ENABLE);
	write_register(device, GENET_TX_DMA, GENET_DMA_ENABLE);
}

/* Sets the station address and admits multicast for the generic IPv6 stack. */
static void
initialize_filter(
	struct genet_device *device)
{
	uint32_t high;
	uint32_t low;

	/* Writes the station address in UniMAC's big-endian register packing. */
	high = (uint32_t)device->config.mac[0] << 24;
	high |= (uint32_t)device->config.mac[1] << 16;
	high |= (uint32_t)device->config.mac[2] << 8;
	high |= device->config.mac[3];
	low = (uint32_t)device->config.mac[4] << 8;
	low |= device->config.mac[5];
	write_register(device, GENET_MAC_HIGH, high);
	write_register(device, GENET_MAC_LOW, low);

	/* Disables exact-match filtering because net_device has no multicast-list hook. */
	write_register(device, GENET_FILTER_ENABLE, 0);
	write_register(device, GENET_MAC_COMMAND, GENET_MAC_PROMISCUOUS);
}

/* Opens an interface with fresh indices and no inherited DMA traffic. */
static int
genet_open(
	struct net_device *net)
{
	struct genet_device *device;
	unsigned long irq;
	unsigned link;
	int error;

	/* Serializes the hardware reset against interrupt and transmit producers. */
	device = net->driver_data;
	irq = spin_lock_irqsave(&device->lock);

	/* A repeated driver open preserves outstanding transmit descriptors. */
	if (device->opened != 0) {
		spin_unlock_irqrestore(&device->lock, irq);
		return 0;
	}

	/* Resets the MAC before rebuilding descriptor and filter state. */
	error = reset_mac(device);
	if (error != 0) {
		spin_unlock_irqrestore(&device->lock, irq);
		return error;
	}

	/* Disables status blocks and checksum offload while retaining two-byte RX alignment. */
	write_register(device, GENET_PORT, 3U);
	write_register(device, GENET_MAX_FRAME, 1536U);
	write_register(device, GENET_RX_CONTROL, 2U);
	write_register(device, GENET_RX_CHECK, 0);
	write_register(device, GENET_TX_CONTROL, 0);
	write_register(device, GENET_BUFFER_SIZE, 1U);
	initialize_filter(device);
	initialize_rings(device);

	/* Publishes opened only after DMA slots and indices are ready. */
	device->opened = 1;
	device->link = 0;
	device->speed = 0;
	device->next_link_check = 0;
	update_link(device);
	link = device->link;
	write_register(device, GENET_IRQ_CLEAR, UINT32_MAX);
	write_register(device, GENET_IRQ1_CLEAR, UINT32_MAX);
	write_register(
		device,
		GENET_IRQ_MASK_CLEAR,
		GENET_IRQ_RX | GENET_IRQ_TX);

	spin_unlock_irqrestore(&device->lock, irq);

	/* Announces carrier outside the higher-ranked hardware lock. */
	kern_irq_unmask((int)device->config.irq);
	(void)net_device_set_carrier(net, (int)link);
	kern_logf("genet: en0 RX/TX enabled\n");

	/* Succeeded: packets and PHY changes can be serviced by IRQ and periodic polling. */
	return 0;
}

/* Stops software producers and hardware traffic before a synchronous close returns. */
static void
genet_close(
	struct net_device *net)
{
	struct genet_device *device;
	unsigned long irq;
	int error;

	/* Prevents the GIC from admitting another controller interrupt. */
	device = net->driver_data;
	kern_irq_mask((int)device->config.irq);

	/* Closes the producer gate before stopping the MAC and DMA engines. */
	irq = spin_lock_irqsave(&device->lock);

	device->opened = 0;
	device->link = 0;
	device->speed = 0;
	write_register(device, GENET_IRQ_MASK_SET, UINT32_MAX);
	write_register(device, GENET_IRQ1_MASK_SET, UINT32_MAX);
	write_register(device, GENET_MAC_COMMAND, 0);
	error = delay_microseconds(2000U);

	/* Stops both DMA rings even if the time base could not confirm MAC quiescence. */
	write_register(device, GENET_RX_DMA, 0);
	write_register(device, GENET_TX_DMA, 0);
	write_register(device, GENET_MAC_TX_FLUSH, 1U);
	write_register(device, GENET_TX_FLUSH, 1U);

	/* Drains the transmitter before removing its flush request. */
	if (error == 0)
		error = delay_microseconds(2000U);

	/* Leaves the controller masked with permanent payload storage retained. */
	write_register(device, GENET_MAC_TX_FLUSH, 0);
	write_register(device, GENET_TX_FLUSH, 0);
	write_register(device, GENET_IRQ_CLEAR, UINT32_MAX);
	write_register(device, GENET_IRQ1_CLEAR, UINT32_MAX);

	spin_unlock_irqrestore(&device->lock, irq);

	/* Withdraws carrier and diagnoses a reset counter failure without freeing DMA. */
	(void)net_device_set_carrier(net, 0);
	if (error != 0) {
		kern_logf(
			"genet: close timing failed (%d); DMA buffers retained\n",
			error);
	}
}

/* Copies one consumed network packet into a free hardware transmit slot. */
static int
genet_transmit(
	struct net_device *net,
	struct packet_buf *packet)
{
	struct genet_device *device;
	uint8_t *payload;
	uint64_t address;
	uint16_t consumer;
	unsigned slot;
	unsigned descriptor;
	unsigned long irq;
	size_t length;
	int error;

	/* Rejects a missing packet without inspecting its payload. */
	if (packet == NULL)
		return EINVAL;

	/* Consumes an oversized or truncated Ethernet frame on every error path. */
	length = packet->length;
	if (length < 14U || length > GENET_MAX_PACKET) {
		packet_buf_free(packet);
		return EMSGSIZE;
	}

	/* Serializes transmit admission with hardware completion and interface close. */
	device = net->driver_data;
	error = 0;
	irq = spin_lock_irqsave(&device->lock);

	/* Refuses a closed interface or a cable without negotiated full duplex. */
	if (device->opened == 0 || device->link == 0) {
		error = ENETDOWN;
	} else {
		/* Uses the hardware consumer sequence to protect slots still owned by DMA. */
		consumer =
		    (uint16_t)read_register(device, GENET_TX_RING + 0x08U);
		if ((uint16_t)(device->tx_producer - consumer) >=
		    GENET_RING_COUNT)
			error = ENOBUFS;
	}

	/* Publishes exactly one complete frame when a hardware slot is available. */
	if (error == 0) {
		/* Copies before padding to prevent leakage from a previous shorter packet. */
		slot = device->tx_producer & (GENET_RING_COUNT - 1U);
		payload =
		    (uint8_t *)device->tx.address + slot * GENET_PACKET_BYTES;
		kern_memcpy(payload, packet->data, length);

		/* Pads short Ethernet frames to the minimum length excluding the generated CRC. */
		if (length < 60U) {
			kern_memset(payload + length, 0, 60U - length);
			length = 60U;
		}

		/* Orders payload writes before handing the SRAM descriptor to DMA. */
		kern_io_write_barrier();
		descriptor =
		    GENET_TX_DESCRIPTORS + slot * GENET_DESCRIPTOR_BYTES;
		address = device->tx.device_address + slot * GENET_PACKET_BYTES;
		write_register(device, descriptor + 4U, (uint32_t)address);
		write_register(
			device,
			descriptor + 8U,
			(uint32_t)(address >> 32));
		write_register(
			device,
			descriptor,
			((uint32_t)length << 16) | GENET_PACKET_START |
			    GENET_PACKET_END | 0x1f80U | (1U << 6));

		/* The wrapping 16-bit producer sequence transfers one new slot to hardware. */
		device->tx_producer++;
		write_register(
			device,
			GENET_TX_RING + 0x0cU,
			device->tx_producer);
	}

	spin_unlock_irqrestore(&device->lock, irq);

	/* Releases the stack packet outside the hardware lock regardless of admission. */
	packet_buf_free(packet);

	/* Reports backpressure or link loss to the network caller. */
	if (error != 0)
		return error;

	/* Succeeded: the permanent DMA slot contains the accepted frame. */
	return 0;
}

/* Updates PHY carrier and the MAC speed while the hardware lock is held. */
static void
update_link(
	struct genet_device *device)
{
	uint32_t command;
	uint32_t rgmii;
	uint64_t now;
	unsigned speed;
	unsigned encoding;
	int error;

	/* Samples PHY state at a bounded rate even during sustained packet receive. */
	now = clock_ticks();
	if (device->next_link_check != 0 && now < device->next_link_check)
		return;
	device->next_link_check = now + KERN_MS_TO_TICKS(GENET_LINK_MS);

	/* Obtains negotiated carrier through the external PHY driver's MDIO interface. */
	error = drv_bcm54213pe_read_link(&device->phy, &speed);

	/* Reports an unresponsive MDIO engine once per fault episode. */
	if (error != 0) {
		/* Stops MAC traffic until a future periodic poll confirms PHY recovery. */
		if (device->mdio_fault == 0)
			kern_logf("genet: PHY status failed (%d)\n", error);
		device->mdio_fault = 1;
		device->link = 0;
		device->speed = 0;
		device->net->link_mbps = 0;
		write_register(
			device,
			GENET_MAC_COMMAND,
			GENET_MAC_PROMISCUOUS);
		return;
	}

	/* Converts the PHY driver's full-duplex result into UniMAC speed bits. */
	device->mdio_fault = 0;
	encoding = 0;
	if (speed == 1000U) {
		encoding = 2U;
	} else if (speed == 100U) {
		encoding = 1U;
	}

	/* Emits only link transitions or negotiated speed changes. */
	if (speed != device->speed) {
		/* Distinguishes unplugged cables from a successfully negotiated speed. */
		if (speed == 0) {
			kern_logf("genet: en0 link down\n");
		} else {
			kern_logf(
				"genet: en0 link up %u Mbps full duplex\n",
				speed);
		}
	}

	/* Publishes speed for the generic interface statistics API. */
	device->speed = speed;
	device->net->link_mbps = speed;
	device->link = 0;
	if (speed != 0)
		device->link = 1;

	/* Updates RGMII mode while preserving unrelated external port controls. */
	rgmii = read_register(device, GENET_RGMII);
	rgmii &= ~((1U << 5) | (1U << 4) | GENET_RGMII_DISABLE_DELAY);
	rgmii |= 1U << 6;

	/* Plain RGMII omits the MAC internal delay, matching the external-port mode. */
	if (device->config.rx_delay == 0 && device->config.tx_delay == 0)
		rgmii |= GENET_RGMII_DISABLE_DELAY;

	/* Asserts the out-of-band link only for a successfully negotiated cable. */
	if (device->link != 0)
		rgmii |= 1U << 4;
	write_register(device, GENET_RGMII, rgmii);

	/* Enables MAC packet traffic only after the PHY speed is known. */
	command = GENET_MAC_PROMISCUOUS | (encoding << 2);
	if (device->link != 0)
		command |= GENET_MAC_ENABLE;
	write_register(device, GENET_MAC_COMMAND, command);
}

/* Delivers receive slots within the network worker's packet budget. */
static unsigned
genet_poll_packets(
	struct net_device *net,
	unsigned budget)
{
	struct genet_device *device;
	struct packet_buf *packet;
	void *payload;
	unsigned long irq;
	uint16_t producer;
	unsigned pending;
	unsigned slot;
	uint32_t status;
	size_t length;
	unsigned count;
	int valid;

	/* Processes a finite batch so one NIC cannot starve the protocol worker. */
	device = net->driver_data;
	for (count = 0; count < budget; count++) {
		/* Obtains a stack packet before taking the higher-ranked hardware lock. */
		packet = packet_buf_alloc(PACKET_BUF_DEFAULT_HEADROOM);
		valid = 0;
		irq = spin_lock_irqsave(&device->lock);

		/* Samples the hardware producer only while the interface owns its rings. */
		producer = device->rx_consumer;
		if (device->opened != 0)
			producer = (uint16_t)read_register(device, GENET_RX_RING + 0x08U);
		pending = (uint16_t)(producer - device->rx_consumer);

		/* Leaves an empty or closed ring without consuming a nonexistent frame. */
		if (pending == 0) {
			spin_unlock_irqrestore(&device->lock, irq);
			packet_buf_free(packet);
			break;
		}

		/* Drops overwritten slots if hardware advanced more than a complete ring. */
		if (pending > GENET_RING_COUNT) {
			net->rx_dropped += pending - GENET_RING_COUNT;
			device->rx_consumer =
			    (uint16_t)(producer - GENET_RING_COUNT);
		}

		/* Reads descriptor completion before touching the uncached payload. */
		slot = device->rx_consumer & (GENET_RING_COUNT - 1U);
		status =
		    read_register(device, GENET_RX_DESCRIPTORS +
					      slot * GENET_DESCRIPTOR_BYTES);
		kern_io_read_barrier();
		length = (status >> 16) & 0x0fffU;

		/* Requires a complete ordinary Ethernet frame with no receive errors. */
		if ((status & (GENET_PACKET_START | GENET_PACKET_END |
			       GENET_RX_ERRORS)) ==
			(GENET_PACKET_START | GENET_PACKET_END) &&
		    length >= 14U + GENET_RX_PADDING &&
		    length <= GENET_MAX_PACKET + GENET_RX_PADDING) {
			/* Copies the aligned payload before returning its ring slot to DMA. */
			if (packet != NULL) {
				payload = packet_buf_append(
					packet,
					length - GENET_RX_PADDING);
				if (payload != NULL) {
					kern_memcpy(
						payload,
						(uint8_t *)device->rx.address +
						    slot * GENET_PACKET_BYTES +
						    GENET_RX_PADDING,
						length - GENET_RX_PADDING);
					valid = 1;
				}
			}
		} else {
			/* Accounts malformed frames while still releasing their hardware slots. */
			net->rx_errors++;
		}

		/* Accounts exhausted packet-pool storage independently of malformed frames. */
		if (valid == 0)
			net->rx_dropped++;

		/* Returns exactly this slot after its copy, including on all drop paths. */
		kern_io_barrier();
		device->rx_consumer++;
		write_register(
			device,
			GENET_RX_RING + 0x0cU,
			device->rx_consumer);

		spin_unlock_irqrestore(&device->lock, irq);

		/* Transfers packet ownership to the network core outside the hardware lock. */
		if (valid != 0) {
			net_device_receive(net, packet);
		} else {
			packet_buf_free(packet);
		}
	}

	/* Reports consumed slots, including malformed packets and allocation drops. */
	return count;
}

/* Services carrier and receive packets, then rearms the level interrupt. */
static unsigned
genet_poll(
	struct net_device *net,
	unsigned budget)
{
	struct genet_device *device;
	unsigned long irq;
	unsigned link;
	unsigned count;

	/* Samples the PHY in the same serialized domain as packet producers. */
	device = net->driver_data;
	irq = spin_lock_irqsave(&device->lock);

	/* Leaves closed hardware untouched if lifecycle retirement won the poll race. */
	if (device->opened != 0)
		update_link(device);
	link = device->link;

	spin_unlock_irqrestore(&device->lock, irq);

	/* Announces link changes before protocol timers attempt further transmits. */
	(void)net_device_set_carrier(net, (int)link);
	count = genet_poll_packets(net, budget);

	/* Rearms RX only after the finite receive batch, without clearing new completions. */
	irq = spin_lock_irqsave(&device->lock);

	/* A concurrent interface close must leave the controller masked. */
	if (device->opened != 0)
		write_register(device, GENET_IRQ_MASK_CLEAR, GENET_IRQ_RX);

	spin_unlock_irqrestore(&device->lock, irq);

	/* Reports work to the common network budget scheduler. */
	return count;
}

/* Acknowledges completion and schedules thread-context receive work. */
static void
genet_interrupt(
	int irq,
	kern_irq_ack_t acknowledge,
	void *argument)
{
	struct genet_device *device;
	unsigned long enabled;
	uint32_t pending;
	unsigned schedule;

	/* The opaque acknowledgement token already identifies the GIC delivery. */
	(void)irq;

	/* Captures and clears only the completion sources this driver enabled. */
	device = argument;
	schedule = 0;
	enabled = spin_lock_irqsave(&device->lock);

	pending = read_register(
		device,
		GENET_IRQ_STATUS) & (GENET_IRQ_RX | GENET_IRQ_TX);
	write_register(device, GENET_IRQ_CLEAR, pending);

	/* Masks receive notifications until the network worker drains a batch. */
	if (device->opened != 0 && (pending & GENET_IRQ_RX) != 0) {
		write_register(device, GENET_IRQ_MASK_SET, GENET_IRQ_RX);
		schedule = 1;
	}

	spin_unlock_irqrestore(&device->lock, enabled);

	/* Completes the GIC level acknowledgement before waking the lower-ranked stack. */
	kern_irq_send_eoi(acknowledge);
	if (schedule != 0)
		net_device_schedule_poll(device->net);
}

/* Schedules a bounded service pass even after a missed IRQ or cable change. */
static void
poll_worker(
	void *argument)
{
	struct genet_device *device;
	unsigned long irq;
	unsigned opened;
	uint64_t deadline;

	/* Services the statically owned board NIC for the lifetime of the kernel. */
	device = argument;
	for (;;) {
		/* Samples producer admission without calling the stack under a device lock. */
		irq = spin_lock_irqsave(&device->lock);

		opened = device->opened;

		spin_unlock_irqrestore(&device->lock, irq);

		/* Wakes the network worker only while the interface has active users. */
		if (opened != 0)
			net_device_schedule_poll(device->net);

		/* Sleeps on scheduler ticks rather than spinning a dedicated Ethernet thread. */
		deadline = sched_ticks() + KERN_MS_TO_TICKS(GENET_POLL_MS);
		sched_sleep(deadline);
	}
}

/* Finds the identity RAM window and restricts allocations to it. */
static int
describe_dma(
	const struct drv_fdt *fdt,
	uint32_t node,
	uint64_t *limit)
{
	const uint8_t *property;
	uint32_t bus;
	uint32_t parent;
	uint32_t length;
	uint32_t child_cells;
	uint32_t parent_cells;
	uint32_t size_cells;
	uint32_t stride;
	uint32_t offset;
	uint64_t child;
	uint64_t physical;
	uint64_t size;
	int error;

	/* Reads the SCB bus and its parent's address-cell width. */
	error = drv_fdt_parent(fdt, node, &bus);
	if (error != 0)
		return error;

	/* Requires a parent bus to interpret the DMA window. */
	error = drv_fdt_parent(fdt, bus, &parent);
	if (error != 0)
		return error;

	/* Requires an explicit SCB DMA description. */
	error = drv_fdt_property(fdt, bus, "dma-ranges", &property, &length);
	if (error != 0)
		return error;

	/* Decodes only address widths representable by the DMA API. */
	child_cells = drv_fdt_node_cells(fdt, bus, "#address-cells", 2);
	parent_cells = drv_fdt_node_cells(fdt, parent, "#address-cells", 2);
	size_cells = drv_fdt_node_cells(fdt, bus, "#size-cells", 1);
	if (child_cells == 0 ||
	    child_cells > 2 ||
	    parent_cells == 0 ||
	    parent_cells > 2 ||
	    size_cells == 0 ||
	    size_cells > 2)
		return ENOTSUP;

	/* Requires complete DMA range tuples. */
	stride = child_cells + parent_cells + size_cells;
	if (length == 0 || length % (stride * 4U) != 0)
		return EINVAL;

	/* Selects RAM starting at zero rather than the peripheral DMA alias. */
	for (offset = 0; offset < length / 4U; offset += stride) {
		/* Separates the bus address, CPU address, and window extent. */
		child = drv_fdt_cells_value(property, offset, child_cells);
		physical = drv_fdt_cells_value(
			property,
			offset + child_cells,
			parent_cells);
		size = drv_fdt_cells_value(
			property,
			offset + child_cells + parent_cells,
			size_cells);

		/* The allocator's physical address must already be a device address. */
		if (child != 0 || physical != 0 || size == 0)
			continue;

		/* Keeps this first implementation within the low 32-bit RAM window. */
		*limit = size - 1U;
		if (*limit > UINT32_MAX)
			*limit = UINT32_MAX;

		/* Succeeded: allocations below the limit need no DMA alias. */
		return 0;
	}

	/* Refuses a bus that would require an unimplemented DMA translation. */
	return ENOTSUP;
}

/* Reads the external PHY address and the firmware's RGMII delay selection. */
static int
describe_phy(
	const struct drv_fdt *fdt,
	uint32_t node,
	struct rpi4_ethernet_config *config)
{
	const uint8_t *property;
	uint32_t length;
	uint32_t phy;
	uint32_t phandle;
	int comparison;
	int error;

	/* Requires a terminated interface-mode string. */
	error = drv_fdt_property(fdt, node, "phy-mode", &property, &length);
	if (error != 0)
		return error;

	/* Requires a terminated interface-mode string inside the property. */
	if (length == 0 || property[length - 1U] != 0)
		return EINVAL;

	/* Requires the Pi 4's known receive-delay arrangement for this board driver. */
	comparison = kern_strcmp((const char *)property, "rgmii-rxid");
	if (comparison != 0)
		return ENOTSUP;
	config->rx_delay = 1;
	config->tx_delay = 0;

	/* Resolves the PHY phandle to its MDIO node. */
	error = drv_fdt_property(fdt, node, "phy-handle", &property, &length);
	if (error != 0)
		return error;

	/* Requires exactly one complete firmware cell. */
	if (length != 4U)
		return EINVAL;
	phandle = (uint32_t)drv_fdt_cells_value(property, 0, 1);
	error = drv_fdt_find_phandle(fdt, phandle, &phy);
	if (error != 0)
		return error;

	/* Requires a clause-22 PHY address. */
	error = drv_fdt_property(fdt, phy, "reg", &property, &length);
	if (error != 0)
		return error;

	/* Requires exactly one complete firmware cell. */
	if (length != 4U)
		return EINVAL;
	config->phy_address = (unsigned)drv_fdt_cells_value(property, 0, 1);
	if (config->phy_address > 31U)
		return EINVAL;

	/* Succeeded: MDIO can select this PHY and its RGMII clocks. */
	return 0;
}
