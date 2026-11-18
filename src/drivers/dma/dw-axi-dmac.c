/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The DesignWare AXI DMA controller.
 *
 * Every transfer is a chain of 64-byte descriptors the channel fetches
 * from memory, one block each.  A copy chains as many blocks as its size
 * needs and the driver polls for the end of the chain.  A cyclic transfer
 * has one descriptor a period, each marked the last of its chain: the
 * channel stops after every period and raises the transfer-done
 * interrupt, and the interrupt marks the descriptors valid again and
 * starts the channel on the next one, which the channel's pointer already
 * names.  The ring therefore plays continuously as long as the interrupt
 * comes back within the device's own buffering.
 *
 * The descriptors live in one coherent buffer per channel.  Only channels
 * 0 to 7 are supported, which is the register layout for controllers of
 * up to eight channels.
 */

#include <drivers/dma/dw-axi-dmac.h>
#include <drivers/generic/dma.h>
#include <drivers/generic/fdt.h>
#include <kern/clock.h>
#include <kern/device-io.h>
#include <kern/irq.h>
#include <kern/kcrt.h>
#include <kern/klog.h>
#include <kern/kmem.h>
#include <kern/lock.h>
#include <kern/pmem.h>
#include <uapi/errno.h>

/*
 * The controller's common registers.
 *
 * Values referenced:
 * Linux v6.12 drivers/dma/dw-axi-dmac/dw-axi-dmac.h (the common register
 * offsets, the global enable and interrupt enable bits of the
 * configuration register, the enable and write-enable bit positions of
 * the channel enable register for controllers of up to eight channels).
 */
#define AXI_GLOBAL_CONFIG		0x010U
#define AXI_GLOBAL_ENABLE		0x00000001U
#define AXI_GLOBAL_INTERRUPTS		0x00000002U
#define AXI_CHANNEL_ENABLE		0x018U
#define AXI_CHANNEL_WRITE_SHIFT		8U

/*
 * One channel's registers, from the channel's own block.
 *
 * Values referenced:
 * Linux v6.12 drivers/dma/dw-axi-dmac/dw-axi-dmac.h (the channel blocks
 * at 0x100 apart from 0x100, and the offsets of the configuration,
 * descriptor pointer and interrupt registers).
 */
#define AXI_CHANNEL_BASE		0x100U
#define AXI_CHANNEL_STRIDE		0x100U
#define AXI_CH_CONFIG_LOW		0x020U
#define AXI_CH_CONFIG_HIGH		0x024U
#define AXI_CH_CHAIN_LOW		0x028U
#define AXI_CH_CHAIN_HIGH		0x02cU
#define AXI_CH_STATUS_ENABLE		0x080U
#define AXI_CH_STATUS			0x088U
#define AXI_CH_SIGNAL_ENABLE		0x090U
#define AXI_CH_CLEAR			0x098U

/*
 * The channel interrupt bits: a finished chain, and the errors.
 *
 * Values referenced:
 * Linux v6.12 drivers/dma/dw-axi-dmac/dw-axi-dmac.h (the transfer-done
 * bit and the mask of all the error bits, 5 to 14 and 16 to 21).
 */
#define AXI_EVENT_TRANSFER_DONE		0x00000002U
#define AXI_EVENT_ERRORS		0x003f7fe0U
#define AXI_EVENT_ALL			0xffffffffU

/*
 * The fields of a channel's configuration.
 *
 * Both chains are descriptor lists (type 3).  Older controllers put the
 * flow control, the handshake selects, the request lines and the
 * priority in the high word at the first positions below; the JH7110's
 * controller uses the second layout, with the request lines in the low
 * word.
 *
 * Values referenced:
 * Linux v6.12 drivers/dma/dw-axi-dmac/dw-axi-dmac.h (the multi-block type
 * fields and the descriptor-list type, the two layouts of the request
 * line, flow control, handshake select and priority fields, the flow
 * control values memory to memory 0 and memory to device 1);
 * drivers/dma/dw-axi-dmac/dw-axi-dmac-platform.c (dw_dma_of_id_table:
 * starfive,jh7110-axi-dma uses the second layout).
 */
#define AXI_CONFIG_SOURCE_LIST		0x00000003U
#define AXI_CONFIG_DESTINATION_LIST	0x0000000cU
#define AXI_CONFIG_FLOW_MEMORY		0U
#define AXI_CONFIG_FLOW_TO_DEVICE	1U
#define AXI_CONFIG_OLD_DEVICE_SHIFT	12U
#define AXI_CONFIG_OLD_PRIORITY_SHIFT	17U
#define AXI_CONFIG_NEW_DEVICE_SHIFT	11U
#define AXI_CONFIG_NEW_PRIORITY_SHIFT	20U
#define AXI_CONFIG_REQUEST_MASK		0x3fU

/*
 * One descriptor: 64 bytes in memory, little endian.
 *
 * Values referenced:
 * Linux v6.12 drivers/dma/dw-axi-dmac/dw-axi-dmac.h (the descriptor's
 * layout: source, destination, block size, next pointer, control).
 */
#define AXI_DESCRIPTOR_BYTES		64U
#define AXI_DESC_SOURCE			0U
#define AXI_DESC_DESTINATION		8U
#define AXI_DESC_BLOCK			16U
#define AXI_DESC_NEXT			24U
#define AXI_DESC_CONTROL_LOW		32U
#define AXI_DESC_CONTROL_HIGH		36U

/*
 * The control fields of a descriptor.
 *
 * Low word: the address steps (0 steps, 1 holds), the widths as the
 * power of two of the bytes, and the burst sizes (1 is four beats).  High
 * word: the AXI burst length limits, the last-of-chain mark and the valid
 * mark the channel clears when it has used the descriptor.
 *
 * Values referenced:
 * Linux v6.12 drivers/dma/dw-axi-dmac/dw-axi-dmac.h (the positions of the
 * increment, width and burst size fields, the burst length enables and
 * fields, the last and valid bits).
 */
#define AXI_CONTROL_SOURCE_HOLD		0x00000010U
#define AXI_CONTROL_DESTINATION_HOLD	0x00000040U
#define AXI_CONTROL_SOURCE_WIDTH_SHIFT	8U
#define AXI_CONTROL_DEST_WIDTH_SHIFT	11U
#define AXI_CONTROL_SOURCE_BURST_SHIFT	14U
#define AXI_CONTROL_DEST_BURST_SHIFT	18U
#define AXI_CONTROL_BURST_FOUR		1U
#define AXI_CONTROL_READ_LIMIT		0x00000040U
#define AXI_CONTROL_READ_LENGTH_SHIFT	7U
#define AXI_CONTROL_WRITE_LIMIT		0x00008000U
#define AXI_CONTROL_WRITE_LENGTH_SHIFT	16U
#define AXI_CONTROL_LAST		0x40000000U
#define AXI_CONTROL_VALID		0x80000000U

/* The widest single beat the driver uses toward a device: four bytes. */
#define AXI_DEVICE_WIDTH_MAX_LOG2	2U

/* The most descriptors one channel holds. */
#define AXI_CHAIN_MAX			DRV_DW_AXI_DMAC_PERIOD_MAX

/* How long a copy or a stop is waited for: polls of 20 to 40 us. */
#define AXI_COPY_POLLS			50000U
#define AXI_STOP_POLLS			2500U

/* The fewest bytes of registers a controller must have. */
#define AXI_REGISTER_BYTES		0x1000U

/* Marks a parameter that the interrupt layer passes but the handler does not need. */
#define UNUSED_PARAMETER(parameter)	((void)(parameter))

/*
 * One channel of a controller.
 *
 * Taken says a user holds the channel.  Running says a cyclic transfer is
 * under way; the interrupt restarts the channel only while it is set,
 * and the controller's lock protects it.  Current is the period the
 * channel works on, which the interrupt moves.
 */
struct drv_dw_axi_dmac_channel {
	struct drv_dw_axi_dmac *controller;
	unsigned index;
	volatile uint8_t *registers;
	struct drv_dma_buffer chain;
	int taken;
	int running;
	int prepared;
	uint32_t period_count;
	uint32_t period_bytes;
	volatile uint32_t current;
	drv_dw_axi_dmac_period_t period_done;
	void *argument;
};

/*
 * One attached controller.
 *
 * Filled by the attach and kept for good.  The lock protects the channels'
 * taken and running marks and the channel enable register.
 */
struct drv_dw_axi_dmac {
	volatile uint8_t *registers;
	uint64_t physical;
	unsigned irq;
	unsigned channel_count;
	unsigned data_width_log2;
	uint32_t block_beats[DRV_DW_AXI_DMAC_CHANNEL_MAX];
	uint32_t priority[DRV_DW_AXI_DMAC_CHANNEL_MAX];
	uint32_t burst_limit;
	int new_layout;
	struct drv_dma_device *dma;
	struct spinlock lock;
	struct drv_dw_axi_dmac_channel channels[DRV_DW_AXI_DMAC_CHANNEL_MAX];
};

static int describe(const struct drv_fdt *fdt, uint32_t node, struct drv_dw_axi_dmac *controller);
static int read_cells(const struct drv_fdt *fdt, uint32_t node, const char *name, uint32_t *cells, unsigned count);
static void reset_channels(struct drv_dw_axi_dmac *controller);
static uint32_t read_register(struct drv_dw_axi_dmac *controller, uint32_t offset);
static void write_register(struct drv_dw_axi_dmac *controller, uint32_t offset, uint32_t value);
static uint32_t read_channel(struct drv_dw_axi_dmac_channel *channel, uint32_t offset);
static void write_channel(struct drv_dw_axi_dmac_channel *channel, uint32_t offset, uint32_t value);
static void switch_channel(struct drv_dw_axi_dmac_channel *channel, int enable);
static int channel_enabled(struct drv_dw_axi_dmac_channel *channel);
static void write_config(struct drv_dw_axi_dmac_channel *channel, uint32_t flow, uint32_t request);
static void write_chain_start(struct drv_dw_axi_dmac_channel *channel, uint64_t address);
static void put_descriptor(struct drv_dw_axi_dmac_channel *channel, unsigned index, uint64_t source, uint64_t destination, uint32_t beats, uint32_t control_low, uint32_t control_high, uint64_t next);
static uint64_t descriptor_address(struct drv_dw_axi_dmac_channel *channel, unsigned index);
static void mark_descriptor_valid(struct drv_dw_axi_dmac_channel *channel, unsigned index);
static uint32_t burst_limit_bits(struct drv_dw_axi_dmac *controller);
static unsigned copy_width(struct drv_dw_axi_dmac *controller, uint64_t destination, uint64_t source, size_t size);
static int wait_idle(struct drv_dw_axi_dmac_channel *channel);
static unsigned finished_period(struct drv_dw_axi_dmac_channel *channel);
static void controller_interrupt(int irq, kern_irq_ack_t acknowledge, void *argument);

/*
 * Attaches the controller a device tree node describes.
 */
int
drv_dw_axi_dmac_attach(
	const struct drv_fdt *fdt,
	uint32_t node,
	int board_default_coherent,
	struct drv_dw_axi_dmac **controller)
{
	struct drv_dma_constraints constraints;
	struct drv_dw_axi_dmac_channel *channel;
	struct drv_dw_axi_dmac *dmac;
	uint64_t size;
	void *mapped;
	unsigned index;
	int coherent;
	int error;

	/* Refuses a missing tree or result. */
	if (fdt == NULL || controller == NULL)
		return EINVAL;

	/* Allocates the controller's record. */
	dmac = kern_calloc(1, sizeof(*dmac));
	if (dmac == NULL)
		return ENOMEM;

	/* Readies the lock that guards the channels' marks. */
	spin_init(&dmac->lock, LOCK_RANK_DEVICE, "dw axi dmac");

	/* Reads the controller's shape from its node. */
	error = describe(fdt, node, dmac);
	if (error != 0) {
		kern_free(dmac);
		return error;
	}

	/* Finds the registers. */
	error = drv_fdt_reg(fdt, node, 0, &dmac->physical, &size);
	if (error != 0) {
		kern_free(dmac);
		return error;
	}

	/* Refuses a register block smaller than the channels' registers need. */
	if (size < AXI_REGISTER_BYTES) {
		kern_free(dmac);
		return ERANGE;
	}

	/* Maps the registers. */
	error = kern_device_map(dmac->physical, (size_t)size, KERN_DEVICE_UNCACHED, &mapped);
	if (error != 0) {
		kern_free(dmac);
		return error;
	}

	/* Keeps the mapping. */
	dmac->registers = mapped;

	/*
	 * The DMA owner: the controller sees memory at its physical
	 * addresses, and the board or the node says whether it snoops the
	 * caches.
	 */
	coherent = drv_fdt_dma_coherent(fdt, node, board_default_coherent);
	kern_memset(&constraints, 0, sizeof(constraints));
	constraints.address_bits = 64U;
	constraints.max_segment_size = 1024U * 1024U;
	constraints.segment_boundary = 0;
	constraints.coherent = coherent;
	error = drv_dma_device_create(&constraints, &dmac->dma);
	if (error != 0)
		return error;

	/* Gives each channel its registers and its descriptors. */
	for (index = 0; index < dmac->channel_count; index++) {
		/* Names the channel and its register block. */
		channel = &dmac->channels[index];
		channel->controller = dmac;
		channel->index = index;
		channel->registers = dmac->registers + AXI_CHANNEL_BASE + index * AXI_CHANNEL_STRIDE;

		/* Allocates the channel's descriptors. */
		error = drv_dma_alloc_coherent(dmac->dma,
					       AXI_CHAIN_MAX * AXI_DESCRIPTOR_BYTES,
					       AXI_DESCRIPTOR_BYTES,
					       &channel->chain);
		if (error != 0)
			return error;

		/* Starts every descriptor empty and not valid. */
		kern_memset(channel->chain.address, 0, AXI_CHAIN_MAX * AXI_DESCRIPTOR_BYTES);
	}

	/* Leaves every channel stopped and silent. */
	reset_channels(dmac);

	/* Masks the interrupt line and sets it level-triggered, active high. */
	kern_irq_mask((int)dmac->irq);
	error = kern_irq_set_mode((int)dmac->irq, KERN_IRQ_TRIGGER_LEVEL, KERN_IRQ_POLARITY_HIGH);
	if (error != 0)
		return error;

	/* Takes the interrupt line while nothing can raise it. */
	error = kern_irq_register((int)dmac->irq, controller_interrupt, dmac);
	if (error != 0)
		return error;

	/* Turns the controller and its interrupt output on. */
	write_register(dmac, AXI_GLOBAL_CONFIG, AXI_GLOBAL_ENABLE | AXI_GLOBAL_INTERRUPTS);
	kern_irq_unmask((int)dmac->irq);

	/* Reports the controller. */
	kern_logf("dw-axi-dmac: %llx, %u channels, irq %u, %s\n",
		  (unsigned long long)dmac->physical,
		  dmac->channel_count,
		  dmac->irq,
		  coherent ? "coherent" : "non-coherent");

	/* Succeeded: the controller hands out channels. */
	*controller = dmac;
	return 0;
}

/*
 * Reports the DMA owner of the controller.
 */
struct drv_dma_device *
drv_dw_axi_dmac_dma_device(
	struct drv_dw_axi_dmac *controller)
{
	/* A missing controller has no owner. */
	if (controller == NULL)
		return NULL;

	/* Succeeded: the owner the controller's buffers come from. */
	return controller->dma;
}

/*
 * Takes a free channel for one user.
 */
int
drv_dw_axi_dmac_channel_get(
	struct drv_dw_axi_dmac *controller,
	struct drv_dw_axi_dmac_channel **channel)
{
	unsigned long enabled;
	unsigned index;

	/* Refuses a missing controller or result. */
	if (controller == NULL || channel == NULL)
		return EINVAL;

	/* Marks the first free channel taken. */
	enabled = spin_lock_irqsave(&controller->lock);

	for (index = 0; index < controller->channel_count; index++) {
		if (controller->channels[index].taken == 0)
			break;
	}

	/* The mark keeps every other user off the channel until it is put back. */
	if (index < controller->channel_count)
		controller->channels[index].taken = 1;

	spin_unlock_irqrestore(&controller->lock, enabled);

	/* Every channel is in use. */
	if (index == controller->channel_count)
		return EBUSY;

	/* Succeeded: the caller owns the channel. */
	*channel = &controller->channels[index];
	return 0;
}

/*
 * Gives a channel back.
 */
void
drv_dw_axi_dmac_channel_put(
	struct drv_dw_axi_dmac_channel *channel)
{
	unsigned long enabled;

	/* Ignores a missing channel. */
	if (channel == NULL)
		return;

	/* Forgets the transfer and frees the channel. */
	enabled = spin_lock_irqsave(&channel->controller->lock);

	channel->prepared = 0;
	channel->taken = 0;

	spin_unlock_irqrestore(&channel->controller->lock, enabled);
}

/*
 * Copies memory to memory and waits for the copy to end.
 *
 * Procedure referenced:
 * Linux v6.12 drivers/dma/dw-axi-dmac/dw-axi-dmac-platform.c
 * (dma_chan_prep_dma_memcpy: one descriptor a block of at most the block
 * size, the common width of both addresses and the size, four-beat
 * bursts, the last descriptor marked; axi_chan_block_xfer_start: the
 * list types, the chain pointer, the interrupt status enable and the
 * channel enable).
 */
int
drv_dw_axi_dmac_copy(
	struct drv_dw_axi_dmac_channel *channel,
	uint64_t destination,
	uint64_t source,
	size_t size)
{
	struct drv_dw_axi_dmac *controller;
	uint32_t control_low;
	uint32_t control_high;
	uint32_t status;
	uint64_t block_bytes;
	uint64_t piece;
	uint64_t next;
	unsigned width;
	unsigned count;
	unsigned polls;
	int error;

	/* Refuses a missing channel, an empty copy or a busy channel. */
	if (channel == NULL || size == 0)
		return EINVAL;
	if (channel->running != 0)
		return EBUSY;
	controller = channel->controller;

	/* The widest beat both addresses and the size allow, and a block's bytes. */
	width = copy_width(controller, destination, source, size);
	block_bytes = (uint64_t)controller->block_beats[channel->index] << width;

	/* Refuses a copy longer than the channel's descriptors can chain. */
	if ((size + block_bytes - 1U) / block_bytes > AXI_CHAIN_MAX)
		return E2BIG;

	/* Both addresses step; both bursts are four beats of the common width. */
	control_low = width << AXI_CONTROL_SOURCE_WIDTH_SHIFT;
	control_low |= width << AXI_CONTROL_DEST_WIDTH_SHIFT;
	control_low |= AXI_CONTROL_BURST_FOUR << AXI_CONTROL_SOURCE_BURST_SHIFT;
	control_low |= AXI_CONTROL_BURST_FOUR << AXI_CONTROL_DEST_BURST_SHIFT;
	control_high = AXI_CONTROL_VALID | burst_limit_bits(controller);

	/* Writes one descriptor a block, each linked to the next and the last marked and unlinked. */
	count = 0;
	while (size > 0) {
		/* Takes a block's bytes, or what is left. */
		piece = size;
		if (piece > block_bytes)
			piece = block_bytes;
		size -= (size_t)piece;

		/* Links the descriptor to the next one, or marks it the last. */
		next = descriptor_address(channel, count + 1U);
		if (size == 0) {
			control_high |= AXI_CONTROL_LAST;
			next = 0;
		}

		/* Writes the descriptor and moves past its block. */
		put_descriptor(channel,
			       count,
			       source,
			       destination,
			       (uint32_t)(piece >> width),
			       control_low,
			       control_high,
			       next);
		source += piece;
		destination += piece;
		count++;
	}

	/* Sets the channel up for a memory copy from the first descriptor, reporting by status only. */
	write_config(channel, AXI_CONFIG_FLOW_MEMORY, 0);
	write_chain_start(channel, channel->chain.device_address);
	write_channel(channel, AXI_CH_CLEAR, AXI_EVENT_ALL);
	write_channel(channel, AXI_CH_SIGNAL_ENABLE, 0);
	write_channel(channel, AXI_CH_STATUS_ENABLE, AXI_EVENT_TRANSFER_DONE | AXI_EVENT_ERRORS);

	/* Starts the channel once the descriptors are in memory. */
	kern_io_write_barrier();
	switch_channel(channel, 1);

	/* Waits for the end of the chain or an error. */
	status = 0;
	for (polls = 0; polls < AXI_COPY_POLLS; polls++) {
		/* Stops polling once the channel reports the end or an error. */
		status = read_channel(channel, AXI_CH_STATUS);
		if ((status & (AXI_EVENT_TRANSFER_DONE | AXI_EVENT_ERRORS)) != 0)
			break;

		/* Gives the channel time to move more data. */
		kern_usleep_range(20U, 40U);
	}

	/* Clears what the channel reported and stops it reporting. */
	write_channel(channel, AXI_CH_CLEAR, AXI_EVENT_ALL);
	write_channel(channel, AXI_CH_STATUS_ENABLE, 0);

	/* A copy that never ended is stopped. */
	if (polls == AXI_COPY_POLLS) {
		switch_channel(channel, 0);
		error = wait_idle(channel);
		kern_logf("dw-axi-dmac: channel %u copy timed out (stop %d)\n", channel->index, error);
		return ETIMEDOUT;
	}

	/* A copy that hit an error is stopped and reported. */
	if ((status & AXI_EVENT_ERRORS) != 0) {
		switch_channel(channel, 0);
		error = wait_idle(channel);
		kern_logf("dw-axi-dmac: channel %u copy error %x (stop %d)\n", channel->index, status, error);
		return EIO;
	}

	/* Succeeded: the destination holds the source's bytes. */
	return 0;
}

/*
 * Builds the descriptors of a cyclic transfer without starting it.
 *
 * Procedure referenced:
 * Linux v6.12 drivers/dma/dw-axi-dmac/dw-axi-dmac-platform.c
 * (dw_axi_dma_chan_prep_cyclic and dw_axi_dma_set_hw_desc: one
 * descriptor a period, each marked last, chained back to the first; the
 * memory side at most four bytes wide and stepping, the device side at
 * the register's width and held, four-beat bursts).
 */
int
drv_dw_axi_dmac_cyclic_prepare(
	struct drv_dw_axi_dmac_channel *channel,
	const struct drv_dw_axi_dmac_cyclic *cyclic)
{
	struct drv_dw_axi_dmac *controller;
	uint32_t control_low;
	uint32_t control_high;
	uint64_t next;
	uint32_t beats;
	unsigned memory_width;
	unsigned device_width;
	unsigned index;

	/* Refuses a missing channel or transfer, or a running channel. */
	if (channel == NULL || cyclic == NULL)
		return EINVAL;
	if (channel->running != 0)
		return EBUSY;
	controller = channel->controller;

	/* Refuses a ring the descriptors cannot describe. */
	if (cyclic->period_count < 2U || cyclic->period_count > AXI_CHAIN_MAX)
		return EINVAL;
	if (cyclic->period_bytes == 0 || (cyclic->period_bytes & 3U) != 0)
		return EINVAL;
	if ((cyclic->ring_address & 3U) != 0)
		return EINVAL;
	if (cyclic->handshake > AXI_CONFIG_REQUEST_MASK)
		return EINVAL;

	/* The device register's width as a power of two. */
	if (cyclic->device_width == 1U) {
		device_width = 0;
	} else if (cyclic->device_width == 2U) {
		device_width = 1;
	} else if (cyclic->device_width == 4U) {
		device_width = AXI_DEVICE_WIDTH_MAX_LOG2;
	} else {
		return EINVAL;
	}

	/* The memory side reads four bytes a beat; a period must fit one block. */
	memory_width = AXI_DEVICE_WIDTH_MAX_LOG2;
	if (controller->data_width_log2 < memory_width)
		memory_width = controller->data_width_log2;
	beats = cyclic->period_bytes >> memory_width;
	if (beats > controller->block_beats[channel->index])
		return E2BIG;

	/* The memory steps, the device register holds; both bursts are four beats. */
	control_low = AXI_CONTROL_DESTINATION_HOLD;
	control_low |= memory_width << AXI_CONTROL_SOURCE_WIDTH_SHIFT;
	control_low |= device_width << AXI_CONTROL_DEST_WIDTH_SHIFT;
	control_low |= AXI_CONTROL_BURST_FOUR << AXI_CONTROL_SOURCE_BURST_SHIFT;
	control_low |= AXI_CONTROL_BURST_FOUR << AXI_CONTROL_DEST_BURST_SHIFT;
	control_high = AXI_CONTROL_VALID | AXI_CONTROL_LAST | burst_limit_bits(controller);

	/* Writes one descriptor a period, each ending its chain and linked to the next, the last to the first. */
	for (index = 0; index < cyclic->period_count; index++) {
		next = descriptor_address(channel, (index + 1U) % cyclic->period_count);
		put_descriptor(channel,
			       index,
			       cyclic->ring_address + (uint64_t)index * cyclic->period_bytes,
			       cyclic->device_address,
			       beats,
			       control_low,
			       control_high,
			       next);
	}

	/* Records the transfer for the start and the interrupt. */
	channel->period_count = cyclic->period_count;
	channel->period_bytes = cyclic->period_bytes;
	channel->period_done = cyclic->period_done;
	channel->argument = cyclic->argument;
	channel->current = 0;
	channel->prepared = 1;

	/* Sets the channel up for memory to device, paced by the device's request line. */
	write_config(channel, AXI_CONFIG_FLOW_TO_DEVICE, cyclic->handshake);

	/* Succeeded: the transfer can start. */
	return 0;
}

/*
 * Starts a prepared cyclic transfer from its first period.
 *
 * Procedure referenced:
 * Linux v6.12 drivers/dma/dw-axi-dmac/dw-axi-dmac-platform.c
 * (axi_chan_block_xfer_start: the chain pointer, the interrupt signal
 * enable of the transfer-done and error events, then the channel enable).
 */
int
drv_dw_axi_dmac_cyclic_start(
	struct drv_dw_axi_dmac_channel *channel)
{
	unsigned long enabled;
	unsigned index;

	/* Refuses a missing or unprepared channel, or one already running. */
	if (channel == NULL)
		return EINVAL;
	if (channel->prepared == 0)
		return EINVAL;
	if (channel->running != 0)
		return EBUSY;

	/* Marks every descriptor usable again and starts from the first. */
	for (index = 0; index < channel->period_count; index++)
		mark_descriptor_valid(channel, index);
	channel->current = 0;
	write_chain_start(channel, channel->chain.device_address);

	/* Lets the end of each period and any error interrupt. */
	write_channel(channel, AXI_CH_CLEAR, AXI_EVENT_ALL);
	write_channel(channel, AXI_CH_STATUS_ENABLE, AXI_EVENT_TRANSFER_DONE | AXI_EVENT_ERRORS);
	write_channel(channel, AXI_CH_SIGNAL_ENABLE, AXI_EVENT_TRANSFER_DONE | AXI_EVENT_ERRORS);

	/* Starts the channel with the interrupt's restart armed. */
	kern_io_write_barrier();
	enabled = spin_lock_irqsave(&channel->controller->lock);

	channel->running = 1;
	switch_channel(channel, 1);

	spin_unlock_irqrestore(&channel->controller->lock, enabled);

	/* Succeeded: the ring plays. */
	return 0;
}

/*
 * Stops a cyclic transfer and waits until the channel is idle.
 *
 * Procedure referenced:
 * Linux v6.12 drivers/dma/dw-axi-dmac/dw-axi-dmac-platform.c
 * (dma_chan_terminate_all: clear the channel's enable, then poll the
 * enable register until the channel reports idle).
 */
void
drv_dw_axi_dmac_cyclic_stop(
	struct drv_dw_axi_dmac_channel *channel)
{
	unsigned long enabled;
	int error;

	/* Ignores a missing channel. */
	if (channel == NULL)
		return;

	/*
	 * Disarms the interrupt's restart, silences the channel's interrupt
	 * first (the interrupt skips a channel no longer running, so a
	 * period ending now must not hold the shared line raised), and stops
	 * the channel.
	 */
	enabled = spin_lock_irqsave(&channel->controller->lock);

	channel->running = 0;
	write_channel(channel, AXI_CH_SIGNAL_ENABLE, 0);
	switch_channel(channel, 0);

	spin_unlock_irqrestore(&channel->controller->lock, enabled);

	/* Waits for the channel's last beats. */
	error = wait_idle(channel);
	if (error != 0)
		kern_logf("dw-axi-dmac: channel %u did not stop\n", channel->index);

	/* Clears what the channel reported and stops it reporting. */
	write_channel(channel, AXI_CH_STATUS_ENABLE, 0);
	write_channel(channel, AXI_CH_CLEAR, AXI_EVENT_ALL);
}

/*
 * Reports the byte offset in the ring of the period the channel works on.
 */
uint32_t
drv_dw_axi_dmac_cyclic_position(
	struct drv_dw_axi_dmac_channel *channel)
{
	uint32_t current;

	/* A missing channel is at the start. */
	if (channel == NULL)
		return 0;

	/* Succeeded: the start of the current period. */
	current = channel->current;
	return current * channel->period_bytes;
}

/*
 * Reads the controller's shape from its node: interrupt, channels, data
 * width, block sizes, priorities, burst limit and register layout.
 */
static int
describe(
	const struct drv_fdt *fdt,
	uint32_t node,
	struct drv_dw_axi_dmac *controller)
{
	uint32_t cells[DRV_DW_AXI_DMAC_CHANNEL_MAX];
	unsigned layout;
	unsigned index;
	int error;

	/* The single interrupt line of all the channels. */
	error = read_cells(fdt, node, "interrupts", cells, 1);
	if (error != 0)
		return error;
	controller->irq = cells[0];

	/* The number of channels, up to the eight of the register layout. */
	error = read_cells(fdt, node, "dma-channels", cells, 1);
	if (error != 0)
		return error;
	if (cells[0] == 0 || cells[0] > DRV_DW_AXI_DMAC_CHANNEL_MAX)
		return ENOTSUP;
	controller->channel_count = cells[0];

	/* The memory bus width, as a power of two of bytes. */
	error = read_cells(fdt, node, "snps,data-width", cells, 1);
	if (error != 0)
		return error;
	if (cells[0] > 6U)
		return EINVAL;
	controller->data_width_log2 = cells[0];

	/* Each channel's longest block, in beats. */
	error = read_cells(fdt, node, "snps,block-size", cells, controller->channel_count);
	if (error != 0)
		return error;

	/* Keeps each channel's block size, refusing an empty one. */
	for (index = 0; index < controller->channel_count; index++) {
		if (cells[index] == 0)
			return EINVAL;
		controller->block_beats[index] = cells[index];
	}

	/* Each channel's priority, below the channel count. */
	error = read_cells(fdt, node, "snps,priority", cells, controller->channel_count);
	if (error != 0)
		return error;

	/* Keeps each channel's priority, refusing one outside the channels. */
	for (index = 0; index < controller->channel_count; index++) {
		if (cells[index] >= controller->channel_count)
			return EINVAL;
		controller->priority[index] = cells[index];
	}

	/* The optional AXI burst length limit, 1 to 256 beats. */
	controller->burst_limit = 0;
	error = read_cells(fdt, node, "snps,axi-max-burst-len", cells, 1);
	if (error == 0) {
		if (cells[0] == 0 || cells[0] > 256U)
			return EINVAL;
		controller->burst_limit = cells[0];
	}

	/* The JH7110's controller puts the request lines in the low configuration word. */
	controller->new_layout = 0;
	error = drv_fdt_string_index(fdt, node, "compatible", "starfive,jh7110-axi-dma", &layout);
	if (error == 0)
		controller->new_layout = 1;

	/* Succeeded: the controller's shape is known. */
	return 0;
}

/* Reads a property of exactly count 32-bit cells. */
static int
read_cells(
	const struct drv_fdt *fdt,
	uint32_t node,
	const char *name,
	uint32_t *cells,
	unsigned count)
{
	const uint8_t *value;
	uint32_t length;
	unsigned index;
	int error;

	/* Finds the property and checks its length. */
	error = drv_fdt_property(fdt, node, name, &value, &length);
	if (error != 0)
		return error;
	if (length != count * 4U)
		return EINVAL;

	/* Decodes each cell. */
	for (index = 0; index < count; index++)
		cells[index] = (uint32_t)drv_fdt_cells_value(value, index, 1);

	/* Succeeded: every cell is read. */
	return 0;
}

/* Stops every channel, masks its events and clears what it had raised. */
static void
reset_channels(
	struct drv_dw_axi_dmac *controller)
{
	struct drv_dw_axi_dmac_channel *channel;
	unsigned index;
	int error;

	/* Silences and stops each channel the firmware may have left running. */
	for (index = 0; index < controller->channel_count; index++) {
		/* Masks the channel's events and stops it. */
		channel = &controller->channels[index];
		write_channel(channel, AXI_CH_SIGNAL_ENABLE, 0);
		write_channel(channel, AXI_CH_STATUS_ENABLE, 0);
		switch_channel(channel, 0);

		/* Waits for the channel to go idle. */
		error = wait_idle(channel);
		if (error != 0)
			kern_logf("dw-axi-dmac: channel %u stays busy\n", index);

		/* Clears what the channel had raised. */
		write_channel(channel, AXI_CH_CLEAR, AXI_EVENT_ALL);
	}
}

/* Reads one of the controller's common registers. */
static uint32_t
read_register(
	struct drv_dw_axi_dmac *controller,
	uint32_t offset)
{
	uint32_t value;

	/* Reads the register. */
	value = kern_mmio_read32(controller->registers + offset);

	/* Succeeded: the register's contents. */
	return value;
}

/* Writes one of the controller's common registers. */
static void
write_register(
	struct drv_dw_axi_dmac *controller,
	uint32_t offset,
	uint32_t value)
{
	/* Writes the register. */
	kern_mmio_write32(controller->registers + offset, value);
}

/* Reads one of a channel's registers. */
static uint32_t
read_channel(
	struct drv_dw_axi_dmac_channel *channel,
	uint32_t offset)
{
	uint32_t value;

	/* Reads the register. */
	value = kern_mmio_read32(channel->registers + offset);

	/* Succeeded: the register's contents. */
	return value;
}

/* Writes one of a channel's registers. */
static void
write_channel(
	struct drv_dw_axi_dmac_channel *channel,
	uint32_t offset,
	uint32_t value)
{
	/* Writes the register. */
	kern_mmio_write32(channel->registers + offset, value);
}

/*
 * Starts or stops one channel.
 *
 * The write-enable bit of the channel makes the write touch that channel
 * only, so the others' enables need not be read first.
 *
 * Procedure referenced:
 * Linux v6.12 drivers/dma/dw-axi-dmac/dw-axi-dmac-platform.c
 * (axi_chan_enable and axi_chan_disable: the channel's enable bit with
 * its write-enable bit eight above, for up to eight channels).
 */
static void
switch_channel(
	struct drv_dw_axi_dmac_channel *channel,
	int enable)
{
	uint32_t value;

	/* The channel's write enable, with its enable set or clear. */
	value = 1U << (channel->index + AXI_CHANNEL_WRITE_SHIFT);
	if (enable != 0)
		value |= 1U << channel->index;

	/* Writes the enable register. */
	write_register(channel->controller, AXI_CHANNEL_ENABLE, value);
}

/* Reports whether the controller still shows a channel enabled. */
static int
channel_enabled(
	struct drv_dw_axi_dmac_channel *channel)
{
	uint32_t value;

	/* Reads the enable register. */
	value = read_register(channel->controller, AXI_CHANNEL_ENABLE);

	/* A set bit means the channel is still moving data. */
	if ((value & (1U << channel->index)) != 0)
		return 1;

	/* Succeeded: the channel is idle. */
	return 0;
}

/*
 * Writes a channel's configuration: descriptor lists on both sides, the
 * flow control, hardware handshakes, the request line toward a device and
 * the channel's priority.
 */
static void
write_config(
	struct drv_dw_axi_dmac_channel *channel,
	uint32_t flow,
	uint32_t request)
{
	struct drv_dw_axi_dmac *controller;
	uint32_t low;
	uint32_t high;
	uint32_t priority;

	/* Both chains are descriptor lists; the handshakes stay in hardware (0). */
	controller = channel->controller;
	priority = controller->priority[channel->index];
	low = AXI_CONFIG_SOURCE_LIST | AXI_CONFIG_DESTINATION_LIST;
	high = flow;

	/* Places the request line and priority where the controller's layout has them. */
	if (controller->new_layout != 0) {
		low |= (request & AXI_CONFIG_REQUEST_MASK) << AXI_CONFIG_NEW_DEVICE_SHIFT;
		high |= priority << AXI_CONFIG_NEW_PRIORITY_SHIFT;
	} else {
		high |= (request & AXI_CONFIG_REQUEST_MASK) << AXI_CONFIG_OLD_DEVICE_SHIFT;
		high |= priority << AXI_CONFIG_OLD_PRIORITY_SHIFT;
	}

	/* Writes both words. */
	write_channel(channel, AXI_CH_CONFIG_LOW, low);
	write_channel(channel, AXI_CH_CONFIG_HIGH, high);
}

/* Points a channel at the descriptor it fetches first. */
static void
write_chain_start(
	struct drv_dw_axi_dmac_channel *channel,
	uint64_t address)
{
	/* Writes the low word, then the high word; the low bits select master 0. */
	write_channel(channel, AXI_CH_CHAIN_LOW, (uint32_t)address);
	write_channel(channel, AXI_CH_CHAIN_HIGH, (uint32_t)(address >> 32));
}

/* Writes one descriptor of a channel's chain with its link to the next one. */
static void
put_descriptor(
	struct drv_dw_axi_dmac_channel *channel,
	unsigned index,
	uint64_t source,
	uint64_t destination,
	uint32_t beats,
	uint32_t control_low,
	uint32_t control_high,
	uint64_t next)
{
	volatile uint8_t *descriptor;

	/* Clears the whole descriptor, including the status the channel writes back. */
	descriptor = (volatile uint8_t *)channel->chain.address + (size_t)index * AXI_DESCRIPTOR_BYTES;
	kern_memset((void *)descriptor, 0, AXI_DESCRIPTOR_BYTES);

	/* The addresses and the block's beats less one. */
	*(volatile uint64_t *)(descriptor + AXI_DESC_SOURCE) = source;
	*(volatile uint64_t *)(descriptor + AXI_DESC_DESTINATION) = destination;
	*(volatile uint32_t *)(descriptor + AXI_DESC_BLOCK) = beats - 1U;

	/* The link and the control words. */
	*(volatile uint64_t *)(descriptor + AXI_DESC_NEXT) = next;
	*(volatile uint32_t *)(descriptor + AXI_DESC_CONTROL_LOW) = control_low;
	*(volatile uint32_t *)(descriptor + AXI_DESC_CONTROL_HIGH) = control_high;
}

/* The controller's address of one descriptor of a channel's chain. */
static uint64_t
descriptor_address(
	struct drv_dw_axi_dmac_channel *channel,
	unsigned index)
{
	/* Succeeded: the descriptor's place in the chain's buffer. */
	return channel->chain.device_address + (uint64_t)index * AXI_DESCRIPTOR_BYTES;
}

/* Sets a descriptor's valid mark again after the channel has used it. */
static void
mark_descriptor_valid(
	struct drv_dw_axi_dmac_channel *channel,
	unsigned index)
{
	volatile uint8_t *descriptor;
	uint32_t control_high;

	/* The descriptor's control word with the valid mark. */
	descriptor = (volatile uint8_t *)channel->chain.address + (size_t)index * AXI_DESCRIPTOR_BYTES;
	control_high = *(volatile uint32_t *)(descriptor + AXI_DESC_CONTROL_HIGH);
	*(volatile uint32_t *)(descriptor + AXI_DESC_CONTROL_HIGH) = control_high | AXI_CONTROL_VALID;
}

/*
 * The AXI burst length limit bits of a descriptor's high control word,
 * or none when the node sets no limit.
 *
 * The fields hold the burst length less one, as AXI counts beats.
 */
static uint32_t
burst_limit_bits(
	struct drv_dw_axi_dmac *controller)
{
	uint32_t bits;

	/* No limit: the controller's own maximum applies. */
	if (controller->burst_limit == 0)
		return 0;

	/* Limits both reads and writes. */
	bits = AXI_CONTROL_READ_LIMIT;
	bits |= (controller->burst_limit - 1U) << AXI_CONTROL_READ_LENGTH_SHIFT;
	bits |= AXI_CONTROL_WRITE_LIMIT;
	bits |= (controller->burst_limit - 1U) << AXI_CONTROL_WRITE_LENGTH_SHIFT;

	/* Succeeded: the limit's bits. */
	return bits;
}

/*
 * The widest beat, as a power of two of bytes, that both addresses and
 * the size are aligned to, up to the memory bus width.
 */
static unsigned
copy_width(
	struct drv_dw_axi_dmac *controller,
	uint64_t destination,
	uint64_t source,
	size_t size)
{
	uint64_t mixed;
	unsigned width;

	/* Any bit set below the bus width in an address or the size narrows the beat. */
	mixed = destination | source | (uint64_t)size;
	width = 0;
	while (width < controller->data_width_log2 && (mixed & ((uint64_t)1U << width)) == 0)
		width++;

	/* Succeeded: the common width. */
	return width;
}

/* Waits until the controller shows a channel idle. */
static int
wait_idle(
	struct drv_dw_axi_dmac_channel *channel)
{
	unsigned polls;
	int busy;

	/* Polls the enable register for the channel's bit to fall. */
	busy = 1;
	for (polls = 0; polls < AXI_STOP_POLLS; polls++) {
		/* Stops polling once the channel is idle. */
		busy = channel_enabled(channel);
		if (busy == 0)
			break;

		/* Gives the channel time to finish its beats. */
		kern_usleep_range(20U, 40U);
	}

	/* Reports a channel that kept moving data. */
	if (busy != 0)
		return ETIMEDOUT;

	/* Succeeded: the channel is idle. */
	return 0;
}

/*
 * Tells which period a cyclic channel has just finished.
 *
 * The channel's chain pointer already names the descriptor after the
 * finished one; a pointer outside the chain falls back to the period
 * after the one recorded as current.
 */
static unsigned
finished_period(
	struct drv_dw_axi_dmac_channel *channel)
{
	uint64_t pointer;
	uint64_t offset;
	unsigned next;

	/* The descriptor the channel would fetch next. */
	pointer = read_channel(channel, AXI_CH_CHAIN_LOW);
	pointer |= (uint64_t)read_channel(channel, AXI_CH_CHAIN_HIGH) << 32;
	pointer &= ~(uint64_t)(AXI_DESCRIPTOR_BYTES - 1U);

	/* A pointer outside the chain says nothing; the recorded period has finished. */
	if (pointer < channel->chain.device_address)
		return channel->current;
	offset = pointer - channel->chain.device_address;
	if (offset >= (uint64_t)channel->period_count * AXI_DESCRIPTOR_BYTES)
		return channel->current;

	/* Succeeded: the period before the next one. */
	next = (unsigned)(offset / AXI_DESCRIPTOR_BYTES);
	return (next + channel->period_count - 1U) % channel->period_count;
}

/*
 * Serves the controller's interrupt: clears each channel's events,
 * restarts a cyclic channel on its next period and reports the finished
 * one, and stops a channel that reported an error.
 *
 * Procedure referenced:
 * Linux v6.12 drivers/dma/dw-axi-dmac/dw-axi-dmac-platform.c
 * (dw_axi_dma_interrupt: read and clear every channel's status;
 * axi_chan_block_xfer_complete: for a cyclic transfer, find the period
 * from the chain pointer, set the valid mark again and enable the
 * channel; axi_chan_handle_err: disable the channel on an error).
 */
static void
controller_interrupt(
	int irq,
	kern_irq_ack_t acknowledge,
	void *argument)
{
	struct drv_dw_axi_dmac *controller;
	struct drv_dw_axi_dmac_channel *channel;
	drv_dw_axi_dmac_period_t callbacks[DRV_DW_AXI_DMAC_CHANNEL_MAX];
	void *arguments[DRV_DW_AXI_DMAC_CHANNEL_MAX];
	unsigned long enabled;
	uint32_t status;
	unsigned finished;
	unsigned index;

	UNUSED_PARAMETER(irq);

	/* Takes the controller the line was registered with. */
	controller = argument;

	/* Serves every channel's events with the restart decisions under the lock. */
	enabled = spin_lock_irqsave(&controller->lock);

	for (index = 0; index < controller->channel_count; index++) {
		/* Starts with no period to report for the channel. */
		channel = &controller->channels[index];
		callbacks[index] = NULL;
		arguments[index] = NULL;

		/*
		 * Only a running cyclic transfer signals; a copy polls its own
		 * status, which the interrupt must leave for it.
		 */
		if (channel->running == 0)
			continue;

		/* Takes the channel's events. */
		status = read_channel(channel, AXI_CH_STATUS);
		if (status == 0)
			continue;

		/* Clears the events that were taken. */
		write_channel(channel, AXI_CH_CLEAR, status);

		/* An error stops the cyclic transfer for good. */
		if ((status & AXI_EVENT_ERRORS) != 0) {
			channel->running = 0;
			switch_channel(channel, 0);
			kern_logf("dw-axi-dmac: channel %u error %x, stopped\n", index, status);
			continue;
		}

		/* A finished period restarts the channel on the next. */
		if ((status & AXI_EVENT_TRANSFER_DONE) != 0) {
			finished = finished_period(channel);
			mark_descriptor_valid(channel, finished);
			mark_descriptor_valid(channel, (finished + 1U) % channel->period_count);
			channel->current = (finished + 1U) % channel->period_count;
			kern_io_write_barrier();
			switch_channel(channel, 1);
			callbacks[index] = channel->period_done;
			arguments[index] = channel->argument;
		}
	}

	spin_unlock_irqrestore(&controller->lock, enabled);

	/* Reports the finished periods outside the lock. */
	for (index = 0; index < controller->channel_count; index++) {
		if (callbacks[index] != NULL)
			callbacks[index](arguments[index]);
	}

	/* Retires the interrupt. */
	kern_irq_send_eoi(acknowledge);
}
