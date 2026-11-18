/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * /dev/fpga0: the programmable logic of the Zynq-7000, written through the
 * device configuration interface's PCAP and reached through the GP0 window.
 *
 * An open of the device (root's only) collects a bitstream with write(2)
 * in one physically contiguous uncached buffer, and FPGA_PROGRAM hands the
 * buffer to the configuration DMA in one transfer, waits for the logic to
 * say it is configured and enables it: the level shifters between the PS
 * and the logic open, the logic's resets are released and the window of
 * the block the logic holds is mapped.  The registers of the block are
 * then read and written one word at a time, and only while the logic is
 * enabled: an access to the window of an unconfigured logic gets no
 * answer on the bus and stops the processor.
 *
 * The driver leaves the logic as it finds it: a logic written over JTAG
 * (plan/tools/zybo/pl-jtag.sh) is enabled by FPGA_ENABLE, which looks at
 * PCFG_DONE in INT_STS, so attaching does not clear INT_STS.
 *
 * The block's interrupt is IRQ_F2P[0], GIC interrupt 61, a level.  Its
 * handler masks it and counts it, and FPGA_IRQ_WAIT unmasks it only when
 * nothing is pending: the waiter clears the block's status before it
 * waits again, so the line has fallen by the time it is unmasked.
 *
 * In the emulator's model the configuration logic never raises INIT nor
 * PCFG_DONE.  With the boot parameter fpga.no-pl-wait=1, and only on the
 * emulator (PSS_IDCODE reads 0), the waits on the logic are skipped and
 * the status says FPGA_STATUS_EMULATED; the logic is not enabled.
 *
 * Procedure referenced: Linux v6.12 drivers/fpga/zynq-fpga.c,
 * zynq_fpga_ops_write_init (the PROG_B sequence, PCAP_PR and PCAP_MODE,
 * the DMA queue check, PCAP_LPBK), zynq_fpga_ops_write (clearing INT_STS
 * before the DMA, the last-transfer mark, the invalid destination) and
 * zynq_fpga_ops_write_complete (PCFG_DONE in INT_STS, clearing PCAP_PR,
 * the level shifters and FPGA_RST_CTRL).  Values referenced: the same
 * file's *_OFFSET and *_MASK constants, INIT_POLL_TIMEOUT, and
 * arch/arm/mach-zynq/slcr.c's SLCR_UNLOCK_MAGIC and SLCR_LOCK_MAGIC.
 */

#include <stddef.h>
#include <stdint.h>

#include <drivers/generic/dma.h>
#include <drivers/platform/zybo/zynq-devcfg.h>
#include <drivers/platform/zybo/zynq-slcr.h>
#include <kern/boot.h>
#include <kern/cdev.h>
#include <kern/clock.h>
#include <kern/cred.h>
#include <kern/device-io.h>
#include <kern/file.h>
#include <kern/irq.h>
#include <kern/kcrt.h>
#include <kern/klog.h>
#include <kern/kmem.h>
#include <kern/lock.h>
#include <kern/panic.h>
#include <kern/platform.h>
#include <kern/pmem.h>
#include <kern/sched.h>
#include <kern/uaccess.h>
#include <kern/waitq.h>
#include <uapi/errno.h>
#include <uapi/fpga.h>

/* How long a register poll sleeps before the next read, in milliseconds (a tick at least). */
#define DEVCFG_POLL_MS 1U

/* The device number of /dev/fpga0 (the next free one after the DMA test's 0x00170000 and 0x00180000). */
#define FPGA_DEVICE_NUMBER		0x00190000U

/*
 * The registers of the SLCR the driver uses, through the SLCR's driver
 * (zynq-slcr.c), which also serves the SD card's clocks, and the clock
 * enable of PCAP_CLK_CTRL.
 */
#define SLCR_PCAP_CLK_CTRL		0x168U
#define SLCR_FPGA_RST_CTRL		0x240U
#define SLCR_PSS_IDCODE			0x530U
#define SLCR_LVL_SHFTR_EN		0x900U
#define SLCR_PCAP_CLKACT		0x00000001U
#define SLCR_WHOLE_WORD			0xffffffffU

/* The logic's four resets held, or all released. */
#define FPGA_RST_ALL			0x0000000fU
#define FPGA_RST_NONE			0x00000000U

/* The level shifters: PS to logic only (while configuring), or both ways. */
#define LVL_SHFTR_NONE			0x00000000U
#define LVL_SHFTR_PS_TO_PL		0x0000000aU
#define LVL_SHFTR_BOTH			0x0000000fU

/* The device configuration interface and the registers the driver uses. */
#define DEVCFG_PHYS			0xf8007000U
#define DEVCFG_BYTES			0x1000U
#define DEVCFG_CTRL			0x000U
#define DEVCFG_INT_STS			0x00cU
#define DEVCFG_INT_MASK			0x010U
#define DEVCFG_STATUS			0x014U
#define DEVCFG_DMA_SRC_ADDR		0x018U
#define DEVCFG_DMA_DST_ADDR		0x01cU
#define DEVCFG_DMA_SRC_LEN		0x020U
#define DEVCFG_DMA_DEST_LEN		0x024U
#define DEVCFG_UNLOCK			0x034U
#define DEVCFG_MCTRL			0x080U

/* STATUS: the DMA queue full and empty, and the configuration logic's INIT. */
#define STATUS_DMA_Q_F			0x80000000U
#define STATUS_DMA_Q_E			0x40000000U
#define STATUS_PCFG_INIT		0x00000010U

/* INT_STS: the DMA's PCAP transfer done, the logic configured, the errors, and every bit. */
#define IXR_D_P_DONE			0x00001000U
#define IXR_PCFG_DONE			0x00000004U
#define IXR_ERROR_FLAGS			0x00f0c860U
#define IXR_ALL				0xf8f7f87fU

/* MCTRL: the PCAP's loopback, which must be off for the logic to take the data. */
#define MCTRL_PCAP_LPBK			0x00000010U

/* The DMA's source mark for the last transfer, and the destination that means the PCAP. */
#define DMA_SRC_LAST			0x00000001U
#define DMA_DST_PCAP			0xffffffffU

/* How long the logic is given for INIT and PCFG_DONE, and the DMA for its transfer. */
#define INIT_WAIT_MS			2500U
#define DONE_WAIT_MS			2500U
#define DMA_WAIT_MS			5000U

/* The window of the block in GP0, and the logic's interrupt (IRQ_F2P[0]). */
#define WINDOW_PHYS			0x43c00000U
#define WINDOW_BYTES			0x10000U
#define PL_IRQ				61

/* The boot parameter that lets the emulator's model skip the waits on the logic. */
#define NO_PL_WAIT_TOKEN		"fpga.no-pl-wait=1"

/* The size of a register of the block. */
#define REG_BYTES			4U

/* Marks a parameter the interrupt handler's signature carries but the driver does not need. */
#define UNUSED_PARAMETER(parameter)	((void)(parameter))

/*
 * One open of /dev/fpga0: the bitstream written to it so far, in the
 * buffer the configuration DMA reads.  The buffer is allocated at the
 * first write and freed at the close; the open's lock serializes its
 * writes and its FPGA_PROGRAM.
 */
struct fpga_open {
	struct mutex lock;
	struct drv_dma_buffer buffer;
	int allocated;
	size_t length;
};

/*
 * The device: the mapped registers, the DMA device the buffers come from,
 * and the state of the logic.
 *
 * lock serializes FPGA_PROGRAM, FPGA_ENABLE, FPGA_DISABLE and the window's
 * accesses, so the window is never unmapped under an access; enabled,
 * window, programming, last_error and emulated are kept under it.
 * irq_lock (taken from the interrupt handler) keeps irq_pending, the
 * interrupts since the last FPGA_IRQ_WAIT returned, and irq_waiting, set
 * while one waits.
 */
struct fpga_device {
	volatile uint8_t *devcfg;
	struct drv_dma_device *dma;
	struct mutex lock;
	int enabled;
	volatile uint8_t *window;
	int programming;
	uint32_t last_error;
	int emulated;
	int skip_waits;
	struct spinlock irq_lock;
	struct wait_queue irq_wake;
	uint32_t irq_pending;
	int irq_waiting;
};

static int fpga_open(struct file *file);
static int fpga_close(struct file *file);
static ssize_t fpga_write(struct file *file, const void *buffer, size_t size);
static int fpga_ioctl(struct file *file, unsigned long request, uintptr_t argument);

/* The operations of /dev/fpga0. */
static const struct cdev_ops fpga_cdev_ops = {
	.open = fpga_open,
	.close = fpga_close,
	.write = fpga_write,
	.ioctl = fpga_ioctl
};

/*
 * The one device, filled by drv_zynq_devcfg_attach() and kept for the
 * life of the system, since the interrupt handler and the opens read it.
 */
static struct fpga_device fpga;

static uint32_t slcr_read(uint32_t offset);
static void slcr_write_unlocked(uint32_t offset, uint32_t value);
static uint32_t devcfg_read(uint32_t offset);
static void devcfg_write(uint32_t offset, uint32_t value);
static void ctrl_update(uint32_t set, uint32_t clear);
static int wait_for(uint32_t offset, uint32_t mask, uint32_t expected, unsigned milliseconds);
static int program_logic(struct fpga_open *state);
static int configure_logic(struct fpga_open *state);
static int enable_logic(void);
static void disable_logic(void);
static int ioctl_program(struct file *file);
static int ioctl_status(uintptr_t argument);
static int ioctl_enable(void);
static int ioctl_disable(void);
static int ioctl_reg_read(uintptr_t argument);
static int ioctl_reg_write(uintptr_t argument);
static int ioctl_irq_wait(uintptr_t argument);
static int window_offset_valid(uint32_t offset);
static void pl_interrupt(int irq, kern_irq_ack_t acknowledge, void *argument);

/*
 * Starts the driver of /dev/fpga0.
 */
int
drv_zynq_devcfg_attach(
	void)
{
	struct drv_dma_constraints constraints;
	const char *command_line;
	void *mapped;
	uint32_t idcode;
	uint32_t status;
	uint32_t interrupts;
	int asked;
	int error;

	/* Reaches the SLCR, for the logic's clock, resets and level shifters. */
	error = drv_zynq_slcr_init();
	if (error != 0)
		return error;

	/* Maps the device configuration interface. */
	error = kern_device_map(DEVCFG_PHYS, DEVCFG_BYTES, KERN_DEVICE_UNCACHED, &mapped);
	if (error != 0)
		return error;
	fpga.devcfg = mapped;

	/* The buffers the configuration DMA reads: uncached, below 4 GiB, one segment. */
	kern_memset(&constraints, 0, sizeof(constraints));
	constraints.address_bits = 32;
	constraints.max_segment_size = FPGA_BITSTREAM_MAX;
	constraints.segment_boundary = 0;
	constraints.coherent = 0;
	error = drv_dma_device_create(&constraints, &fpga.dma);
	if (error != 0)
		return error;

	/* The device's locks and the interrupt's waiters. */
	error = mutex_init(&fpga.lock, LOCK_RANK_DEVICE, "fpga");
	if (error != 0)
		return error;
	spin_init(&fpga.irq_lock, LOCK_RANK_DEVICE, "fpga irq");
	waitq_init(&fpga.irq_wake, "fpga irq");

	/*
	 * The emulator's PSS_IDCODE reads 0; only there may the boot
	 * parameter skip the waits on a logic the model does not have.
	 */
	idcode = slcr_read(SLCR_PSS_IDCODE);
	if (idcode == 0)
		fpga.emulated = 1;

	/* Honors the boot parameter that skips the waits, on the emulator only. */
	command_line = kern_boot_handoff("boot.command-line");
	asked = kern_boot_parameters_token_present(command_line, NO_PL_WAIT_TOKEN);
	if (asked && fpga.emulated)
		fpga.skip_waits = 1;
	else if (asked)
		kern_logf("fpga: %s ignored on the board\n", NO_PL_WAIT_TOKEN);

	/* Installs the logic's interrupt, a level, masked until a wait unmasks it. */
	error = kern_irq_register(PL_IRQ, pl_interrupt, &fpga);
	if (error != 0)
		return error;
	error = kern_irq_set_mode(PL_IRQ, KERN_IRQ_TRIGGER_LEVEL, KERN_IRQ_POLARITY_HIGH);
	if (error != 0)
		kern_logf("fpga: interrupt %d stays in its default mode (%d)\n", PL_IRQ, error);
	kern_irq_mask(PL_IRQ);

	/* Publishes the device. */
	error = cdev_register("fpga0", (dev_t)FPGA_DEVICE_NUMBER, &fpga_cdev_ops, NULL);
	if (error != 0)
		return error;

	/* Reports what the configuration interface says of the logic. */
	status = devcfg_read(DEVCFG_STATUS);
	interrupts = devcfg_read(DEVCFG_INT_STS);
	kern_logf("fpga: devcfg at %x, status %08x, int_sts %08x%s\n",
		  (unsigned)DEVCFG_PHYS,
		  (unsigned)status,
		  (unsigned)interrupts,
		  fpga.skip_waits ? ", emulated" : "");

	/* Succeeded: /dev/fpga0 is there. */
	return 0;
}

/* Reads one register of the SLCR; 0 if the SLCR cannot be read. */
static uint32_t
slcr_read(
	uint32_t offset)
{
	uint32_t value;
	int error;

	/* Reads through the SLCR's driver, which attach has started. */
	error = drv_zynq_slcr_read(offset, &value);
	if (error != 0)
		return 0;

	/* Succeeded: the register's value. */
	return value;
}

/*
 * Writes one register of the SLCR whole; the SLCR's driver unlocks it for
 * the write and locks it again, apart from the SD card's writes.
 */
static void
slcr_write_unlocked(
	uint32_t offset,
	uint32_t value)
{
	int error;

	/* Writes through the SLCR's driver; the offsets are the driver's own constants. */
	error = drv_zynq_slcr_modify(offset, SLCR_WHOLE_WORD, value);
	if (error != 0)
		kern_logf("fpga: SLCR write at %x failed (%d)\n", (unsigned)offset, error);
}

/* Reads one register of the configuration interface. */
static uint32_t
devcfg_read(
	uint32_t offset)
{
	uint32_t value;

	/* Every register of the interface is a word. */
	value = kern_mmio_read32(fpga.devcfg + offset);

	/* Succeeded: the register's value. */
	return value;
}

/* Writes one register of the configuration interface. */
static void
devcfg_write(
	uint32_t offset,
	uint32_t value)
{
	/* Every register of the interface is a word. */
	kern_mmio_write32(fpga.devcfg + offset, value);
}

/*
 * Sets and clears bits of CTRL, the only way the driver writes it.  A
 * request outside the configuration's bits would reach the PS's reset or
 * the JTAG chain, and is a bug of the driver.
 */
static void
ctrl_update(
	uint32_t set,
	uint32_t clear)
{
	uint32_t control;
	int valid;

	/* Refuses to touch any bit but the configuration's. */
	valid = drv_zynq_devcfg_ctrl_request_valid(set, clear);
	if (!valid)
		KERN_FATAL("fpga: CTRL request outside the configuration bits");

	/* Changes the requested bits and keeps the others as they are. */
	control = devcfg_read(DEVCFG_CTRL);
	control |= set;
	control &= ~clear;
	devcfg_write(DEVCFG_CTRL, control);
}

/*
 * Waits, a bounded time, for the masked bits of a register of the
 * configuration interface to read as expected.  Returns 0 or ETIMEDOUT.
 */
static int
wait_for(
	uint32_t offset,
	uint32_t mask,
	uint32_t expected,
	unsigned milliseconds)
{
	uint64_t deadline;
	uint64_t now;
	uint32_t value;

	/* Reads the register a millisecond at a time until the bits say so or time is up. */
	deadline = sched_ticks() + kern_ms_to_ticks(milliseconds);
	for (;;) {
		value = devcfg_read(offset);
		if ((value & mask) == expected)
			break;

		/* Gives up at the deadline, or waits a millisecond for the next read. */
		now = sched_ticks();
		if (now >= deadline)
			return ETIMEDOUT;
		sched_sleep(now + kern_ms_to_ticks(DEVCFG_POLL_MS));
	}

	/* Succeeded: the bits read as expected. */
	return 0;
}

/* Admits an open by root; the bitstream buffer comes with the first write. */
static int
fpga_open(
	struct file *file)
{
	struct fpga_open *state;
	const struct ucred *credentials;
	int superuser;
	int error;

	/* Refuses anyone but root, whatever the node's mode says. */
	credentials = cred_current();
	superuser = cred_is_superuser(credentials);
	if (!superuser)
		return EPERM;

	/* The open's state. */
	state = kern_malloc(sizeof(*state));
	if (state == NULL)
		return ENOMEM;
	kern_memset(state, 0, sizeof(*state));
	error = mutex_init(&state->lock, LOCK_RANK_DEVICE, "fpga open");
	if (error != 0) {
		kern_free(state);
		return error;
	}

	/* Succeeded: the open holds no bitstream yet. */
	file->f_data = state;
	return 0;
}

/* Frees the open's bitstream buffer. */
static int
fpga_close(
	struct file *file)
{
	struct fpga_open *state;

	/* An open that never took its state has nothing to free. */
	state = file->f_data;
	if (state == NULL)
		return 0;

	/* Gives the buffer back to the DMA device and the state to the heap. */
	if (state->allocated)
		drv_dma_free_coherent(fpga.dma, &state->buffer);
	kern_free(state);
	file->f_data = NULL;

	/* Succeeded: nothing of the open is left. */
	return 0;
}

/*
 * Appends to the open's bitstream.  Returns the bytes taken, or -EFBIG past
 * FPGA_BITSTREAM_MAX, -ENOMEM without a buffer.
 */
static ssize_t
fpga_write(
	struct file *file,
	const void *buffer,
	size_t size)
{
	struct fpga_open *state;
	uint8_t *destination;
	ssize_t taken;
	int error;

	/* Finds the open's bitstream. */
	state = file->f_data;
	if (state == NULL)
		return -EBADF;

	/* Serializes the open's writes and its FPGA_PROGRAM. */
	mutex_lock(&state->lock);

	/* Refuses a bitstream larger than the logic of any Zynq-7000 takes. */
	if (size > FPGA_BITSTREAM_MAX - state->length) {
		mutex_unlock(&state->lock);
		return -EFBIG;
	}

	/* Allocates the whole buffer at the first write, in one piece. */
	if (!state->allocated) {
		error = drv_dma_alloc_coherent(fpga.dma, FPGA_BITSTREAM_MAX, 0, &state->buffer);
		if (error != 0) {
			mutex_unlock(&state->lock);
			return -ENOMEM;
		}

		/* The buffer stays with the open until its close. */
		state->allocated = 1;
	}

	/* Copies the data after what the open already holds. */
	destination = (uint8_t *)state->buffer.address + state->length;
	kern_memcpy(destination, buffer, size);
	state->length += size;
	taken = (ssize_t)size;

	mutex_unlock(&state->lock);

	/* Succeeded: the bytes are in the buffer. */
	return taken;
}

/* Dispatches the requests of /dev/fpga0. */
static int
fpga_ioctl(
	struct file *file,
	unsigned long request,
	uintptr_t argument)
{
	int error;

	/* Selects the request. */
	switch (request) {
	case FPGA_PROGRAM:
		error = ioctl_program(file);
		break;
	case FPGA_STATUS:
		error = ioctl_status(argument);
		break;
	case FPGA_ENABLE:
		error = ioctl_enable();
		break;
	case FPGA_DISABLE:
		error = ioctl_disable();
		break;
	case FPGA_REG_READ:
		error = ioctl_reg_read(argument);
		break;
	case FPGA_REG_WRITE:
		error = ioctl_reg_write(argument);
		break;
	case FPGA_IRQ_WAIT:
		error = ioctl_irq_wait(argument);
		break;
	default:
		error = ENOTTY;
		break;
	}

	/* Reports why the request was refused. */
	if (error != 0)
		return error;

	/* Succeeded: the request was carried out. */
	return 0;
}

/* Writes the open's bitstream into the logic and enables it. */
static int
ioctl_program(
	struct file *file)
{
	struct fpga_open *state;
	int error;

	/* Finds the open's bitstream. */
	state = file->f_data;
	if (state == NULL)
		return EBADF;

	/* Holds the open's buffer still while the DMA reads it. */
	mutex_lock(&state->lock);

	/* Refuses an open with no bitstream. */
	if (!state->allocated || state->length == 0) {
		mutex_unlock(&state->lock);
		return EINVAL;
	}

	/*
	 * Programs, enables and records the outcome under the device's lock:
	 * the logic has one configuration port, so a second program waits for
	 * the first.
	 */
	mutex_lock(&fpga.lock);

	error = program_logic(state);
	fpga.last_error = (uint32_t)error;

	mutex_unlock(&fpga.lock);
	mutex_unlock(&state->lock);

	/* Reports why the logic was not configured. */
	if (error != 0) {
		kern_logf("fpga: program failed (%d)\n", error);
		return error;
	}

	/* Succeeded: the logic holds the bitstream. */
	return 0;
}

/*
 * Programs the logic with an open's bitstream and enables it; the device's
 * lock and the open's are held.  Returns 0, or why it failed.
 */
static int
program_logic(
	struct fpga_open *state)
{
	int error;
	int valid;

	/* Refuses a buffer the DMA cannot read in one transfer. */
	valid = drv_zynq_devcfg_dma_valid(state->buffer.device_address, state->length);
	if (!valid)
		return EINVAL;

	/* Refuses a bitstream without its sync word: it is not a .bin, or not one for this order. */
	valid = drv_zynq_devcfg_has_sync(state->buffer.address, state->length);
	if (!valid)
		return EINVAL;

	/* Closes the window, which the new logic will not answer until it is enabled again. */
	disable_logic();

	/*
	 * Writes the bitstream.  PROGRAMMING tells FPGA_STATUS, which does not
	 * wait for the device's lock, that the logic is being written.
	 */
	fpga.programming = 1;
	error = configure_logic(state);
	fpga.programming = 0;

	/* Reports a configuration that did not complete. */
	if (error != 0)
		return error;

	/* The emulator's model holds no logic to enable. */
	if (fpga.skip_waits)
		return 0;

	/* Opens the logic to the PS and maps its window. */
	error = enable_logic();
	if (error != 0)
		return error;

	/* Succeeded: the logic is configured and enabled. */
	return 0;
}

/*
 * Runs the configuration: the logic's reset, the PCAP and one DMA
 * transfer of the bitstream, and the wait for PCFG_DONE.
 */
static int
configure_logic(
	struct fpga_open *state)
{
	uint32_t clock;
	uint32_t control;
	uint32_t status;
	uint32_t interrupts;
	uint64_t deadline;
	uint64_t now;
	int queue_empty;
	int error;

	/* Turns the PCAP's clock on when the loader left it off. */
	clock = slcr_read(SLCR_PCAP_CLK_CTRL);
	if ((clock & SLCR_PCAP_CLKACT) == 0)
		slcr_write_unlocked(SLCR_PCAP_CLK_CTRL, clock | SLCR_PCAP_CLKACT);

	/*
	 * Holds the logic in reset and lets only the PS drive it while it is
	 * configured.  The JTAG boot runs no ps7_post_config, so the driver
	 * unlocks the interface itself.
	 */
	slcr_write_unlocked(SLCR_FPGA_RST_CTRL, FPGA_RST_ALL);
	slcr_write_unlocked(SLCR_LVL_SHFTR_EN, LVL_SHFTR_NONE);
	slcr_write_unlocked(SLCR_LVL_SHFTR_EN, LVL_SHFTR_PS_TO_PL);
	devcfg_write(DEVCFG_UNLOCK, DRV_ZYNQ_DEVCFG_UNLOCK_KEY);

	/* Clears the logic with PROG_B: first high, until INIT. */
	ctrl_update(DRV_ZYNQ_DEVCFG_CTRL_PCFG_PROG_B, 0);
	if (!fpga.skip_waits) {
		error = wait_for(DEVCFG_STATUS, STATUS_PCFG_INIT, STATUS_PCFG_INIT, INIT_WAIT_MS);
		if (error != 0)
			return error;
	}

	/* Then low, until INIT falls: the logic is cleared. */
	ctrl_update(0, DRV_ZYNQ_DEVCFG_CTRL_PCFG_PROG_B);
	if (!fpga.skip_waits) {
		error = wait_for(DEVCFG_STATUS, STATUS_PCFG_INIT, 0, INIT_WAIT_MS);
		if (error != 0)
			return error;
	}

	/* Then high again, until INIT: the logic is ready for the data. */
	ctrl_update(DRV_ZYNQ_DEVCFG_CTRL_PCFG_PROG_B, 0);
	if (!fpga.skip_waits) {
		error = wait_for(DEVCFG_STATUS, STATUS_PCFG_INIT, STATUS_PCFG_INIT, INIT_WAIT_MS);
		if (error != 0)
			return error;
	}

	/* Routes the configuration through the PCAP, unencrypted. */
	ctrl_update(DRV_ZYNQ_DEVCFG_CTRL_PCAP_PR | DRV_ZYNQ_DEVCFG_CTRL_PCAP_MODE, 0);

	/*
	 * Refuses to queue a transfer behind another the interface still
	 * holds.  The emulator's model keeps no queue state in STATUS (it
	 * reads 0, the queue neither empty nor full), so there only a full
	 * queue refuses.
	 */
	status = devcfg_read(DEVCFG_STATUS);
	queue_empty = 0;
	if ((status & STATUS_DMA_Q_E) != 0)
		queue_empty = 1;
	if (fpga.skip_waits)
		queue_empty = 1;
	if (!queue_empty || (status & STATUS_DMA_Q_F) != 0)
		return EBUSY;

	/* Sends the data to the logic, not back to memory. */
	control = devcfg_read(DEVCFG_MCTRL);
	devcfg_write(DEVCFG_MCTRL, control & ~MCTRL_PCAP_LPBK);

	/*
	 * Clears every interrupt status, so that a DONE left by the BootROM, a
	 * JTAG write or the last program is not read as this one's.
	 */
	devcfg_write(DEVCFG_INT_MASK, IXR_ALL);
	devcfg_write(DEVCFG_INT_STS, IXR_ALL);

	/* Starts the one transfer once the bitstream has reached memory. */
	kern_io_write_barrier();
	devcfg_write(DEVCFG_DMA_SRC_ADDR, (uint32_t)state->buffer.device_address | DMA_SRC_LAST);
	devcfg_write(DEVCFG_DMA_DST_ADDR, DMA_DST_PCAP);
	devcfg_write(DEVCFG_DMA_SRC_LEN, (uint32_t)(state->length / REG_BYTES));
	devcfg_write(DEVCFG_DMA_DEST_LEN, 0);

	/* Waits, a bounded time, for the transfer to reach the logic or fail. */
	deadline = sched_ticks() + kern_ms_to_ticks(DMA_WAIT_MS);
	for (;;) {
		interrupts = devcfg_read(DEVCFG_INT_STS);
		if ((interrupts & IXR_ERROR_FLAGS) != 0)
			break;
		if ((interrupts & IXR_D_P_DONE) != 0)
			break;

		/* Gives up at the deadline, or waits a millisecond for the next read. */
		now = sched_ticks();
		if (now >= deadline)
			break;
		sched_sleep(now + kern_ms_to_ticks(DEVCFG_POLL_MS));
	}

	/* Clears what the transfer raised, but not PCFG_DONE, which is the logic's answer. */
	devcfg_write(DEVCFG_INT_STS, interrupts & ~IXR_PCFG_DONE);

	/* Reports a transfer the interface stopped. */
	if ((interrupts & IXR_ERROR_FLAGS) != 0) {
		kern_logf("fpga: configuration error, int_sts %08x\n", (unsigned)interrupts);
		ctrl_update(0, DRV_ZYNQ_DEVCFG_CTRL_PCAP_PR);
		return EIO;
	}

	/* Reports a transfer that did not end in time. */
	if ((interrupts & IXR_D_P_DONE) == 0) {
		ctrl_update(0, DRV_ZYNQ_DEVCFG_CTRL_PCAP_PR);
		return ETIMEDOUT;
	}

	/* Waits for the logic to say it is configured. */
	if (!fpga.skip_waits) {
		error = wait_for(DEVCFG_INT_STS, IXR_PCFG_DONE, IXR_PCFG_DONE, DONE_WAIT_MS);
		if (error != 0) {
			ctrl_update(0, DRV_ZYNQ_DEVCFG_CTRL_PCAP_PR);
			return error;
		}
	}

	/* Gives the configuration port back to the logic's own ICAP. */
	ctrl_update(0, DRV_ZYNQ_DEVCFG_CTRL_PCAP_PR);

	/* Succeeded: the logic is configured. */
	return 0;
}

/*
 * Opens a configured logic to the PS: both level shifters, the resets
 * released, the window mapped.  The device's lock is held.  Returns 0, or
 * EIO when the logic is not configured.
 */
static int
enable_logic(
	void)
{
	uint32_t interrupts;
	void *mapped;
	int error;

	/* An enabled logic stays as it is. */
	if (fpga.enabled)
		return 0;

	/* Refuses a logic that does not say it is configured: its window would not answer. */
	interrupts = devcfg_read(DEVCFG_INT_STS);
	if ((interrupts & IXR_PCFG_DONE) == 0)
		return EIO;

	/* Opens the level shifters both ways and releases the logic's resets. */
	slcr_write_unlocked(SLCR_LVL_SHFTR_EN, LVL_SHFTR_BOTH);
	slcr_write_unlocked(SLCR_FPGA_RST_CTRL, FPGA_RST_NONE);

	/* Maps the window of the block as device memory. */
	error = kern_device_map(WINDOW_PHYS, WINDOW_BYTES, KERN_DEVICE_UNCACHED, &mapped);
	if (error != 0)
		return error;

	/* The window and the interrupt may be used from now on. */
	fpga.window = mapped;
	fpga.enabled = 1;

	/* Succeeded: the block's registers are reachable. */
	return 0;
}

/*
 * Closes the window and holds the logic in reset behind closed level
 * shifters.  The device's lock is held.
 */
static void
disable_logic(
	void)
{
	unsigned long irq_state;

	/* The interrupt is masked and its count dropped: the logic it came from goes. */
	irq_state = spin_lock_irqsave(&fpga.irq_lock);

	kern_irq_mask(PL_IRQ);
	fpga.irq_pending = 0;
	waitq_wake_all(&fpga.irq_wake);

	spin_unlock_irqrestore(&fpga.irq_lock, irq_state);

	/* Unmaps the window before the logic stops answering it. */
	if (fpga.window != NULL) {
		(void)kern_device_unmap((void *)fpga.window, WINDOW_BYTES);
		fpga.window = NULL;
	}

	/* Holds the logic in reset and closes the level shifters. */
	fpga.enabled = 0;
	slcr_write_unlocked(SLCR_FPGA_RST_CTRL, FPGA_RST_ALL);
	slcr_write_unlocked(SLCR_LVL_SHFTR_EN, LVL_SHFTR_NONE);
}

/* Reports the state of the logic and of the device. */
static int
ioctl_status(
	uintptr_t argument)
{
	struct fpga_status status;
	uint32_t interrupts;
	uint32_t devcfg_status;
	int error;

	/* Starts from a status that says nothing. */
	kern_memset(&status, 0, sizeof(status));

	/*
	 * Reads the state without the device's lock, which a program holds
	 * for its whole run: a program in progress is what the status shows.
	 */
	interrupts = devcfg_read(DEVCFG_INT_STS);
	devcfg_status = devcfg_read(DEVCFG_STATUS);

	/* The flags: what the interface says of the logic, and the driver's own state. */
	if ((interrupts & IXR_PCFG_DONE) != 0)
		status.flags |= FPGA_STATUS_DONE;
	if ((devcfg_status & STATUS_PCFG_INIT) != 0)
		status.flags |= FPGA_STATUS_INIT;
	if (fpga.enabled)
		status.flags |= FPGA_STATUS_ENABLED;
	if (fpga.programming)
		status.flags |= FPGA_STATUS_PROGRAMMING;
	if (fpga.skip_waits)
		status.flags |= FPGA_STATUS_EMULATED;
	status.error = fpga.last_error;
	status.devcfg_status = devcfg_status;
	status.window_base = WINDOW_PHYS;
	status.window_size = WINDOW_BYTES;

	/* Hands the state to the caller. */
	error = copyout(&status, argument, sizeof(status));
	if (error != 0)
		return error;

	/* Succeeded: the caller has the state. */
	return 0;
}

/* Enables a logic configured by FPGA_PROGRAM or over JTAG. */
static int
ioctl_enable(
	void)
{
	int error;

	/* Enables under the device's lock. */
	mutex_lock(&fpga.lock);

	error = enable_logic();

	mutex_unlock(&fpga.lock);

	/* Reports a logic that is not configured. */
	if (error != 0)
		return error;

	/* Succeeded: the logic is enabled. */
	return 0;
}

/* Disables the logic. */
static int
ioctl_disable(
	void)
{
	/* Disables under the device's lock, after any access in progress. */
	mutex_lock(&fpga.lock);

	disable_logic();

	mutex_unlock(&fpga.lock);

	/* Succeeded: the logic is held in reset. */
	return 0;
}

/* Reports whether an offset names one register of the window: 1 when it does. */
static int
window_offset_valid(
	uint32_t offset)
{
	/* Registers are whole words. */
	if ((offset % REG_BYTES) != 0)
		return 0;

	/* The last register ends at the window's end. */
	if (offset > WINDOW_BYTES - REG_BYTES)
		return 0;

	/* Succeeded: the offset is a register's. */
	return 1;
}

/* Reads one register of the block. */
static int
ioctl_reg_read(
	uintptr_t argument)
{
	struct fpga_reg reg;
	int valid;
	int error;

	/* Takes the offset from the caller. */
	error = copyin(argument, &reg, sizeof(reg));
	if (error != 0)
		return error;
	valid = window_offset_valid(reg.offset);
	if (!valid)
		return EINVAL;

	/* Reads only while the logic is enabled: otherwise nothing answers. */
	mutex_lock(&fpga.lock);

	/* A logic that is not enabled leaves the window unmapped. */
	if (!fpga.enabled) {
		mutex_unlock(&fpga.lock);
		return EIO;
	}

	/* One 32-bit read of the register. */
	reg.value = kern_mmio_read32(fpga.window + reg.offset);

	mutex_unlock(&fpga.lock);

	/* Hands the value to the caller. */
	error = copyout(&reg, argument, sizeof(reg));
	if (error != 0)
		return error;

	/* Succeeded: the caller has the register's value. */
	return 0;
}

/* Writes one register of the block. */
static int
ioctl_reg_write(
	uintptr_t argument)
{
	struct fpga_reg reg;
	int valid;
	int error;

	/* Takes the offset and the value from the caller. */
	error = copyin(argument, &reg, sizeof(reg));
	if (error != 0)
		return error;
	valid = window_offset_valid(reg.offset);
	if (!valid)
		return EINVAL;

	/* Writes only while the logic is enabled: otherwise nothing answers. */
	mutex_lock(&fpga.lock);

	/* A logic that is not enabled leaves the window unmapped. */
	if (!fpga.enabled) {
		mutex_unlock(&fpga.lock);
		return EIO;
	}

	/* One 32-bit write of the register. */
	kern_mmio_write32(fpga.window + reg.offset, reg.value);

	mutex_unlock(&fpga.lock);

	/* Succeeded: the register holds the value. */
	return 0;
}

/*
 * Waits for the block's interrupt: returns at once with the count of the
 * interrupts that came since the last wait returned, or unmasks the line
 * and sleeps until one comes or the time is up.
 */
static int
ioctl_irq_wait(
	uintptr_t argument)
{
	struct fpga_irq_wait request;
	unsigned long irq_state;
	uint64_t deadline;
	uint64_t observed;
	int enabled;
	int error;

	/* Takes the timeout from the caller. */
	error = copyin(argument, &request, sizeof(request));
	if (error != 0)
		return error;

	/* Refuses a wait on a logic that is not enabled: no interrupt can come. */
	mutex_lock(&fpga.lock);
	enabled = fpga.enabled;
	mutex_unlock(&fpga.lock);
	if (!enabled)
		return EIO;

	/* Waits under the interrupt's lock, one waiter at a time. */
	deadline = sched_ticks() + kern_ms_to_ticks(request.timeout_ms);
	error = 0;
	irq_state = spin_lock_irqsave(&fpga.irq_lock);

	/* Refuses a second waiter: the count goes to one. */
	if (fpga.irq_waiting) {
		spin_unlock_irqrestore(&fpga.irq_lock, irq_state);
		return EBUSY;
	}

	/*
	 * Marks the wait, and lets the line through only when nothing is
	 * pending: a pending interrupt left it masked and returns at once.
	 */
	fpga.irq_waiting = 1;
	if (fpga.irq_pending == 0)
		kern_irq_unmask(PL_IRQ);

	/* Sleeps until an interrupt is counted, the time is up or a signal comes. */
	while (fpga.irq_pending == 0) {
		observed = waitq_sequence(&fpga.irq_wake);
		error = waitq_sleep(&fpga.irq_wake, &fpga.irq_lock, observed, deadline, WAITQ_INTERRUPTIBLE);
		if (error == EAGAIN) {
			error = 0;
			continue;
		}

		/* A timeout or a signal ends the wait. */
		if (error != 0)
			break;
	}

	/*
	 * Hands over the interrupts that came; the line stays masked until the
	 * next wait, by which time the waiter has cleared the block's status.
	 */
	request.count = fpga.irq_pending;
	fpga.irq_pending = 0;
	fpga.irq_waiting = 0;

	/* An interrupt that came with the timeout or the signal still counts. */
	if (request.count != 0)
		error = 0;

	spin_unlock_irqrestore(&fpga.irq_lock, irq_state);

	/* Reports a wait that ended without an interrupt. */
	if (error != 0)
		return error;

	/* Hands the count to the caller. */
	error = copyout(&request, argument, sizeof(request));
	if (error != 0)
		return error;

	/* Succeeded: the interrupt came. */
	return 0;
}

/*
 * Serves the logic's interrupt: masks the level until the next wait,
 * counts it and wakes the waiter.
 */
static void
pl_interrupt(
	int irq,
	kern_irq_ack_t acknowledge,
	void *argument)
{
	struct fpga_device *device;

	UNUSED_PARAMETER(irq);

	/* Finds the device the handler was installed for. */
	device = (struct fpga_device *)argument;

	/* Counts the interrupt and wakes whoever waits for it. */
	spin_lock(&device->irq_lock);

	kern_irq_mask(PL_IRQ);
	device->irq_pending++;
	waitq_wake_all(&device->irq_wake);

	spin_unlock(&device->irq_lock);

	/* Ends the interrupt at its controller. */
	kern_irq_send_eoi(acknowledge);
}
