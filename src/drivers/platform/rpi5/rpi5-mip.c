/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * A BCM2712 MSI receiver (MIP) for the functions behind one root complex.
 *
 * The device tree names the MIP's registers, the doorbell address as the
 * PCI bus sees it, the first GIC line it drives with the number of lines,
 * and the offset of its first message number.  Message n of the MIP is
 * the number offset + n written to the doorbell, and arrives as an edge on
 * GIC line first + offset + n.  Each line handed out runs the PCI core's
 * handler for its function, without the MIP's lock held, and ends the
 * interrupt once.
 */

#include <drivers/platform/rpi5/rpi5-mip.h>
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
 * The MIP's registers: the host's trigger configuration and masks, and the
 * VideoCore's masks, each a low and a high word for 64 inputs.
 *
 * Values referenced: OpenBSD sys/arch/arm64/dev/bcm2712_mip.c 1.1
 * (MIP_INT_CFGL_HOST, MIP_INT_CFGH_HOST, MIP_INT_MASKL_HOST,
 * MIP_INT_MASKH_HOST, MIP_INT_MASKL_VPU, MIP_INT_MASKH_VPU); Raspberry Pi
 * Linux rpi-6.12.y drivers/irqchip/irq-bcm2712-mip.c (the same offsets,
 * read to check them).
 */
#define MIP_INT_CFGL_HOST		0x20U
#define MIP_INT_CFGH_HOST		0x30U
#define MIP_INT_MASKL_HOST		0x40U
#define MIP_INT_MASKH_HOST		0x50U
#define MIP_INT_MASKL_VPU		0x60U
#define MIP_INT_MASKH_VPU		0x70U
#define MIP_REGISTER_BYTES		0xc0U
#define MIP_INPUTS			64U

/*
 * How a message number and a GIC line follow from the device tree: the
 * number is brcm,msi-offset plus the message's index, and the line is the
 * first shared line of msi-ranges plus that number.
 *
 * Values referenced: Raspberry Pi Linux rpi-6.12.y
 * drivers/irqchip/irq-bcm2712-mip.c (mip_middle_domain_alloc(): hwirq is
 * the index plus msi_offset and the parent line is hwirq plus msi_base;
 * mip_compose_msi_msg(): the data is hwirq) and OpenBSD
 * sys/arch/arm64/dev/bcm2712_mip.c 1.1 (bcmmip_intr_establish_msi(): the
 * same sums).
 */
#define MIP_RANGES_KIND_CELL		0U
#define MIP_RANGES_LINE_CELL		1U
#define MIP_GIC_KIND_SPI		0U

/* The GIC's first shared line, after the private ones. */
#define GIC_SPI_BASE			32U

/* The most lines of one MIP the module hands out. */
#define MIP_LINES_MAX			32U

/* The GIC specifier the module understands: kind, line and trigger. */
#define MIP_GIC_SPECIFIER_CELLS		3U

/*
 * How many times a handler that found no work is run again, and how many
 * register reads the wait between those runs is.  A message travels
 * through a different inbound window from the function's data, so it can
 * arrive before the data it announces.
 */
#define MIP_RETRIES			8U
#define MIP_SETTLE_READS		16U

/* How long disestablish waits for a handler still running, in steps of a millisecond. */
#define MIP_DRAIN_POLLS			1000U
#define MIP_DRAIN_POLL_US		1000U

/*
 * One GIC line of the MIP, which carries one message.
 *
 * Run and argument are set by establish and cleared by disestablish under
 * the MIP's lock; in flight counts the interrupts inside run, so
 * disestablish can wait until none is.  irq and index do not change.
 */
struct rpi5_mip_line {
	struct drv_rpi5_mip *mip;
	drv_pci_msi_run_t run;
	void *argument;
	unsigned in_flight;
	unsigned index;
	int irq;
};

/*
 * One MIP the module started.
 *
 * It lives for as long as the kernel runs, from a successful start: the
 * root complex's board keeps it as the receiver of its messages.  The lock
 * guards each line's run, argument and in-flight count; everything else is
 * set once by the start.  late_noted makes the late message's log line
 * appear once.
 */
struct drv_rpi5_mip {
	volatile uint8_t *registers;
	uint64_t doorbell;
	unsigned first_line;
	unsigned message_offset;
	unsigned line_count;
	struct spinlock lock;
	bool late_noted;
	struct rpi5_mip_line lines[MIP_LINES_MAX];
};

static int read_lines(const struct drv_fdt *fdt, uint32_t node, struct drv_rpi5_mip *mip);
static int read_doorbell(const struct drv_fdt *fdt, uint32_t node, struct drv_rpi5_mip *mip);
static int check_lines(struct drv_rpi5_mip *mip);
static void settle(struct drv_rpi5_mip *mip);
static void interrupt(int irq, kern_irq_ack_t acknowledge, void *argument);

/*
 * Starts the MIP at a device tree node.
 */
int
drv_rpi5_mip_start(
	const struct drv_fdt *fdt,
	uint32_t node,
	struct drv_rpi5_mip **result)
{
	struct drv_rpi5_mip *mip;
	uint64_t address;
	uint64_t size;
	unsigned index;
	void *mapped;
	int error;

	/* Refuses missing arguments. */
	if (fdt == NULL || result == NULL)
		return EINVAL;

	/* Refuses a node that is not a BCM2712 MIP. */
	error = drv_fdt_string_index(fdt, node, "compatible", "brcm,bcm2712-mip", &index);
	if (error != 0)
		return ENODEV;

	/* Allocates the MIP's record. */
	mip = kern_malloc(sizeof(*mip));
	if (mip == NULL)
		return ENOMEM;
	kern_memset(mip, 0, sizeof(*mip));

	/* Reads the GIC lines the MIP drives and its first message number. */
	error = read_lines(fdt, node, mip);
	if (error != 0) {
		kern_free(mip);
		return error;
	}

	/* Reads the doorbell's PCI address. */
	error = read_doorbell(fdt, node, mip);
	if (error != 0) {
		kern_free(mip);
		return error;
	}

	/* Finds the MIP's registers. */
	error = drv_fdt_reg(fdt, node, 0, &address, &size);
	if (error != 0) {
		kern_free(mip);
		return error;
	}

	/* Maps at least the registers the module writes. */
	if (size < MIP_REGISTER_BYTES)
		size = MIP_REGISTER_BYTES;
	error = kern_device_map(address, (size_t)size, KERN_DEVICE_UNCACHED, &mapped);
	if (error != 0) {
		kern_free(mip);
		return error;
	}

	/* Prepares the lines, all free. */
	mip->registers = mapped;
	spin_init(&mip->lock, LOCK_RANK_DEVICE, "BCM2712 MIP");
	for (index = 0; index < mip->line_count; index++) {
		mip->lines[index].mip = mip;
		mip->lines[index].index = index;
		mip->lines[index].irq = (int)(GIC_SPI_BASE + mip->first_line + mip->message_offset + index);
	}

	/* Refuses a MIP whose lines the GIC does not have or cannot make edge triggered. */
	error = check_lines(mip);
	if (error != 0) {
		(void)kern_device_unmap(mapped, (size_t)size);
		kern_free(mip);
		return error;
	}

	/*
	 * Sends every input to the host, not the VideoCore, as an edge.
	 *
	 * Procedure referenced: OpenBSD sys/arch/arm64/dev/bcm2712_mip.c 1.1
	 * (bcmmip_attach(): mask the VPU, configure the host's inputs, unmask
	 * the host).
	 */
	kern_mmio_write32(mip->registers + MIP_INT_MASKL_VPU, 0xffffffffU);
	kern_mmio_write32(mip->registers + MIP_INT_MASKH_VPU, 0xffffffffU);
	kern_mmio_write32(mip->registers + MIP_INT_CFGL_HOST, 0xffffffffU);
	kern_mmio_write32(mip->registers + MIP_INT_CFGH_HOST, 0xffffffffU);
	kern_mmio_write32(mip->registers + MIP_INT_MASKL_HOST, 0);
	kern_mmio_write32(mip->registers + MIP_INT_MASKH_HOST, 0);

	/* Reports the path a message takes. */
	kern_logf("rpi5-mip: messages to PCI %llx, data %u-%u, GIC lines %d-%d\n",
		  (unsigned long long)mip->doorbell,
		  mip->message_offset,
		  mip->message_offset + mip->line_count - 1U,
		  mip->lines[0].irq,
		  mip->lines[mip->line_count - 1U].irq);

	/* Succeeded: messages can be handed out. */
	*result = mip;
	return 0;
}

/*
 * Takes a free message of the MIP for (run, argument).
 */
int
drv_rpi5_mip_establish(
	void *context,
	drv_pci_msi_run_t run,
	void *argument,
	unsigned *handle,
	uint64_t *address,
	uint32_t *data)
{
	struct drv_rpi5_mip *mip;
	struct rpi5_mip_line *line;
	unsigned long enabled;
	unsigned index;
	int error;

	/* Refuses missing arguments. */
	mip = context;
	if (mip == NULL || run == NULL)
		return EINVAL;
	if (handle == NULL || address == NULL || data == NULL)
		return EINVAL;

	/* Takes the first line that no message holds and no interrupt runs. */
	enabled = spin_lock_irqsave(&mip->lock);

	line = NULL;
	for (index = 0; index < mip->line_count; index++) {
		/* Stops at a free line and fills it. */
		if (mip->lines[index].run == NULL && mip->lines[index].in_flight == 0) {
			line = &mip->lines[index];
			line->run = run;
			line->argument = argument;
			break;
		}
	}

	spin_unlock_irqrestore(&mip->lock, enabled);

	/* Reports a MIP whose lines are all taken. */
	if (line == NULL)
		return ENOSPC;

	/* Puts the MIP's handler on the line. */
	error = kern_irq_register(line->irq, interrupt, line);
	if (error != 0) {
		enabled = spin_lock_irqsave(&mip->lock);

		line->run = NULL;
		line->argument = NULL;

		spin_unlock_irqrestore(&mip->lock, enabled);

		/* Reports why the line could not be taken. */
		return error;
	}

	/* Lets the line interrupt. */
	kern_irq_unmask(line->irq);

	/* Succeeded: the function writes the line's number to the doorbell. */
	*handle = line->index;
	*address = mip->doorbell;
	*data = mip->message_offset + line->index;
	return 0;
}

/*
 * Frees a message of the MIP and waits until no interrupt still runs it.
 *
 * After one second of waiting the message is put back as it was and
 * EBUSY is reported.
 */
int
drv_rpi5_mip_disestablish(
	void *context,
	unsigned handle)
{
	struct drv_rpi5_mip *mip;
	struct rpi5_mip_line *line;
	drv_pci_msi_run_t run;
	unsigned long enabled;
	unsigned in_flight;
	unsigned poll;
	void *argument;
	int error;

	/* Refuses a line the MIP does not have. */
	mip = context;
	if (mip == NULL || handle >= mip->line_count)
		return EINVAL;

	/* Stops the line before its entry is emptied. */
	line = &mip->lines[handle];
	kern_irq_mask(line->irq);

	/* Empties the entry, keeping what it held in case the wait runs out. */
	enabled = spin_lock_irqsave(&mip->lock);

	run = line->run;
	argument = line->argument;
	line->run = NULL;
	line->argument = NULL;

	spin_unlock_irqrestore(&mip->lock, enabled);

	/* Refuses a line no message held; it stays masked, as it was. */
	if (run == NULL)
		return EINVAL;

	/* Waits, a millisecond at a time, for the interrupts still inside run. */
	in_flight = 0;
	for (poll = 0; poll < MIP_DRAIN_POLLS; poll++) {
		/* Reads how many interrupts still hold the line. */
		enabled = spin_lock_irqsave(&mip->lock);

		in_flight = line->in_flight;

		spin_unlock_irqrestore(&mip->lock, enabled);

		/* Drained: no CPU runs the line's handler and none will. */
		if (in_flight == 0)
			break;

		/* Lets the other CPU finish. */
		kern_usleep_range(MIP_DRAIN_POLL_US, MIP_DRAIN_POLL_US);
	}

	/* Puts the entry back when an interrupt did not come back within the bound. */
	if (in_flight != 0) {
		enabled = spin_lock_irqsave(&mip->lock);

		line->run = run;
		line->argument = argument;

		spin_unlock_irqrestore(&mip->lock, enabled);

		/* Lets the kept message interrupt again and reports the stuck handler. */
		kern_irq_unmask(line->irq);
		kern_logf("rpi5-mip: line %d still running after a second\n", line->irq);
		return EBUSY;
	}

	/* Takes the MIP's handler off the line. */
	error = kern_irq_unregister(line->irq, interrupt, line);
	if (error != 0)
		return error;

	/* Succeeded: the line is free and no interrupt runs it. */
	return 0;
}

/*
 * Reads the MIP's msi-ranges and brcm,msi-offset: the first shared GIC
 * line, the number of lines and the first message number.
 */
static int
read_lines(
	const struct drv_fdt *fdt,
	uint32_t node,
	struct drv_rpi5_mip *mip)
{
	const uint8_t *value;
	uint32_t length;
	uint32_t phandle;
	uint32_t controller;
	uint32_t specifier_cells;
	uint32_t kind;
	int error;

	/* Reads the ranges: the GIC's phandle, its specifier and the count. */
	error = drv_fdt_property(fdt, node, "msi-ranges", &value, &length);
	if (error != 0)
		return error;
	if (length < 4U)
		return ERANGE;

	/* Finds the GIC, which says how wide its specifier is. */
	phandle = (uint32_t)drv_fdt_cells_value(value, 0, 1U);
	error = drv_fdt_find_phandle(fdt, phandle, &controller);
	if (error != 0)
		return error;

	/* Only the GIC's three-cell specifier is understood. */
	specifier_cells = drv_fdt_node_cells(fdt, controller, "#interrupt-cells", 0);
	if (specifier_cells != MIP_GIC_SPECIFIER_CELLS)
		return ENOTSUP;
	if ((1U + specifier_cells + 1U) * 4U > length)
		return ERANGE;

	/* Refuses lines that are not shared lines. */
	kind = (uint32_t)drv_fdt_cells_value(value, 1U + MIP_RANGES_KIND_CELL, 1U);
	if (kind != MIP_GIC_KIND_SPI)
		return ENOTSUP;

	/* Reads the first line, the count and the first message number. */
	mip->first_line = (unsigned)drv_fdt_cells_value(value, 1U + MIP_RANGES_LINE_CELL, 1U);
	mip->line_count = (unsigned)drv_fdt_cells_value(value, 1U + specifier_cells, 1U);
	mip->message_offset = drv_fdt_node_cells(fdt, node, "brcm,msi-offset", 0);

	/* Refuses a MIP without lines, or with message numbers past its 64 inputs. */
	if (mip->line_count == 0)
		return ERANGE;
	if (mip->message_offset >= MIP_INPUTS)
		return ERANGE;
	if (mip->line_count > MIP_INPUTS - mip->message_offset)
		return ERANGE;

	/* Keeps no more lines than the module hands out. */
	if (mip->line_count > MIP_LINES_MAX)
		mip->line_count = MIP_LINES_MAX;

	/* Succeeded: the lines are known. */
	return 0;
}

/*
 * Reads the MIP's second reg entry, the doorbell as the PCI bus sees it.
 *
 * The address is taken as it is written: it is a PCI address, outside the
 * ranges of the buses above, and the root complex's inbound window
 * carries it to the MIP's registers.
 */
static int
read_doorbell(
	const struct drv_fdt *fdt,
	uint32_t node,
	struct drv_rpi5_mip *mip)
{
	const uint8_t *value;
	uint32_t length;
	uint32_t parent;
	uint32_t address_cells;
	uint32_t size_cells;
	uint32_t pair_cells;
	int error;

	/* Reads the cell counts of the MIP's bus. */
	error = drv_fdt_parent(fdt, node, &parent);
	if (error != 0)
		return error;
	address_cells = drv_fdt_node_cells(fdt, parent, "#address-cells", 2U);
	size_cells = drv_fdt_node_cells(fdt, parent, "#size-cells", 1U);
	if (address_cells == 0 || address_cells > 2U || size_cells > 2U)
		return ERANGE;

	/* Finds the second pair. */
	error = drv_fdt_property(fdt, node, "reg", &value, &length);
	if (error != 0)
		return error;
	pair_cells = address_cells + size_cells;
	if (2U * pair_cells * 4U > length)
		return ENOENT;

	/* Succeeded: the doorbell's PCI address. */
	mip->doorbell = drv_fdt_cells_value(value, pair_cells, address_cells);
	return 0;
}

/*
 * Makes every line of the MIP an edge-triggered GIC line, masked.
 *
 * A line the GIC does not have is refused here, at the start, so the root
 * complex offers no messages at all rather than failing a function's
 * attach later, when the PCI core no longer falls back to the legacy pins.
 */
static int
check_lines(
	struct drv_rpi5_mip *mip)
{
	unsigned index;
	int error;

	/* Configures each line in turn. */
	for (index = 0; index < mip->line_count; index++) {
		/* Keeps the line masked while its trigger changes. */
		kern_irq_mask(mip->lines[index].irq);

		/* Makes it an edge, as the MIP drives it. */
		error = kern_irq_set_mode(mip->lines[index].irq, KERN_IRQ_TRIGGER_EDGE, KERN_IRQ_POLARITY_HIGH);
		if (error != 0) {
			kern_logf("rpi5-mip: GIC line %d unusable (%d); no messages\n", mip->lines[index].irq, error);
			return error;
		}
	}

	/* Succeeded: every line can carry a message. */
	return 0;
}

/*
 * Waits a short while in interrupt context, by reading the MIP's host
 * mask MIP_SETTLE_READS times.
 */
static void
settle(
	struct drv_rpi5_mip *mip)
{
	unsigned read;

	/* Reads a register that does not change, a fixed number of times. */
	for (read = 0; read < MIP_SETTLE_READS; read++)
		(void)kern_mmio_read32(mip->registers + MIP_INT_MASKL_HOST);
}

/*
 * Serves one MIP line: runs the message's handler, again after a short
 * wait while it finds no work, and ends the interrupt.
 */
static void
interrupt(
	int irq,
	kern_irq_ack_t acknowledge,
	void *argument)
{
	struct rpi5_mip_line *line;
	struct drv_rpi5_mip *mip;
	drv_pci_msi_run_t run;
	unsigned long enabled;
	unsigned retries;
	void *run_argument;
	int found;

	(void)irq;

	/* Takes the line's handler and marks it as running. */
	line = argument;
	mip = line->mip;
	enabled = spin_lock_irqsave(&mip->lock);

	run = line->run;
	run_argument = line->argument;
	if (run != NULL)
		line->in_flight++;

	spin_unlock_irqrestore(&mip->lock, enabled);

	/* Runs the handler, again while it finds no work, up to the bound. */
	if (run != NULL) {
		found = run(run_argument);
		retries = 0;
		while (found == 0 && retries < MIP_RETRIES) {
			settle(mip);
			found = run(run_argument);
			retries++;
		}

		/* Notes once that a message came before its data. */
		if (found != 0 &&
		    retries != 0 &&
		    !mip->late_noted) {
			mip->late_noted = true;
			kern_logf("rpi5-mip: line %d found its work after %u retries\n", line->irq, retries);
		}

		/* Marks the handler as no longer running. */
		enabled = spin_lock_irqsave(&mip->lock);

		line->in_flight--;

		spin_unlock_irqrestore(&mip->lock, enabled);
	}

	/* Ends the interrupt. */
	kern_irq_send_eoi(acknowledge);
}
