/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The I2C controllers of the Intel PCH's low power subsystem (LPSS), which
 * are Synopsys DesignWare APB I2C cores behind a PCI function.
 *
 * The function is brought to D0, its private reset register lets the core
 * out of reset, and the core is checked by its component type.  It is then
 * an I2C master in fast mode (400 kHz) or standard mode (100 kHz), whose
 * SCL counts come from the input clock, which the PCH generation sets
 * (the Alder Lake PCH's 133 MHz, the Tiger Lake-LP PCH's 120 MHz).  A
 * transfer runs without the
 * controller's interrupt: the commands go into the transmit FIFO, the
 * bytes read come out of the receive FIFO, and the thread sleeps a tick
 * whenever neither moved.  A 64-byte read at 400 kHz takes about 1.6 ms,
 * so a transfer sleeps a tick or two rather than spinning.  The bus is
 * registered with the I2C registry (i2c.c), where the I2C-HID driver finds
 * it by its ACPI path.
 */

#include <drivers/i2c/i2c.h>
#include <drivers/i2c/lpss-i2c.h>
#include <drivers/pci/pci.h>
#include <kern/clock.h>
#include <kern/device-io.h>
#include <kern/kcrt.h>
#include <kern/klog.h>
#include <kern/kmem.h>
#include <kern/sched.h>
#include <uapi/errno.h>

#include <stdbool.h>

/* The PCI vendor of Intel. */
#define LPSS_VENDOR_INTEL	0x8086U

/* The Alder Lake PCH's serial I/O I2C functions 0 to 5. */
#define LPSS_ADL_I2C0		0x51e8U
#define LPSS_ADL_I2C1		0x51e9U
#define LPSS_ADL_I2C2		0x51eaU
#define LPSS_ADL_I2C3		0x51ebU
#define LPSS_ADL_I2C4		0x51c5U
#define LPSS_ADL_I2C5		0x51c6U

/* The Tiger Lake-LP PCH's serial I/O I2C functions 0 to 7 (the Latitude 5320's touch pad is on 1). */
#define LPSS_TGL_LP_I2C0	0xa0e8U
#define LPSS_TGL_LP_I2C1	0xa0e9U
#define LPSS_TGL_LP_I2C2	0xa0eaU
#define LPSS_TGL_LP_I2C3	0xa0ebU
#define LPSS_TGL_LP_I2C4	0xa0c5U
#define LPSS_TGL_LP_I2C5	0xa0c6U
#define LPSS_TGL_LP_I2C6	0xa0d8U
#define LPSS_TGL_LP_I2C7	0xa0d9U

/* The least size of BAR0: the core's registers and the LPSS private ones after them. */
#define LPSS_BAR_SIZE		0x1000U

/* The PCI power management capability, its control register and the D0 state. */
#define PCI_CAPABILITY_PM	0x01U
#define PCI_PM_CONTROL		0x04U
#define PCI_PM_STATE_MASK	0x0003U

/* How long a function coming out of D3 is left alone (the PCI specification's 10 ms). */
#define LPSS_D3_RECOVERY_US	10000U

/* How long a transfer sleeps while the bus moves bytes, in milliseconds. */
#define LPSS_PAUSE_MS		1U

/* The LPSS private reset register, and its value with the core and its DMA out of reset. */
#define LPSS_PRIVATE_RESETS	0x204U
#define LPSS_RESETS_RELEASED	0x7U

/*
 * The input clocks of the I2C core, in kilohertz, which the identifier
 * tables carry as their driver data.
 *
 * Values referenced: Linux v6.8.12 drivers/mfd/intel-lpss-pci.c
 * (bxt_i2c_info for the Alder Lake PCH, spt_i2c_info for the Tiger Lake-LP
 * PCH).
 */

#define LPSS_ADL_CLOCK_KHZ	133000U
#define LPSS_TGL_LP_CLOCK_KHZ	120000U

/*
 * The SCL high and low times the counts are made for, in nanoseconds:
 * fast mode leaves the I2C specification's least times (0.6 and 1.3 us)
 * room for the rise and the fall, and standard mode the same for its 4.0
 * and 4.7 us.
 */
#define FAST_HIGH_NS		750U
#define FAST_LOW_NS		1500U
#define STANDARD_HIGH_NS	4150U
#define STANDARD_LOW_NS		4900U

/* The DesignWare core's registers. */
#define IC_CON			0x00U
#define IC_TAR			0x04U
#define IC_DATA_CMD		0x10U
#define IC_SS_SCL_HCNT		0x14U
#define IC_SS_SCL_LCNT		0x18U
#define IC_FS_SCL_HCNT		0x1cU
#define IC_FS_SCL_LCNT		0x20U
#define IC_INTR_MASK		0x30U
#define IC_RAW_INTR_STAT	0x34U
#define IC_RX_TL		0x38U
#define IC_TX_TL		0x3cU
#define IC_CLR_INTR		0x40U
#define IC_CLR_TX_ABRT		0x54U
#define IC_CLR_STOP_DET		0x60U
#define IC_ENABLE		0x6cU
#define IC_STATUS		0x70U
#define IC_TXFLR		0x74U
#define IC_RXFLR		0x78U
#define IC_TX_ABRT_SOURCE	0x80U
#define IC_ENABLE_STATUS	0x9cU
#define IC_COMP_PARAM_1		0xf4U
#define IC_COMP_TYPE		0xfcU

/* The component type every DesignWare APB I2C core reports. */
#define IC_COMP_TYPE_VALUE	0x44570140U

/* IC_CON: master, the speed field, repeated starts allowed, the slave side off. */
#define IC_CON_MASTER		0x01U
#define IC_CON_SPEED_STANDARD	0x02U
#define IC_CON_SPEED_FAST	0x04U
#define IC_CON_RESTART_EN	0x20U
#define IC_CON_SLAVE_DISABLE	0x40U

/* IC_DATA_CMD: a read command, the stop after this byte, a repeated start before it. */
#define IC_DATA_CMD_READ	0x100U
#define IC_DATA_CMD_STOP	0x200U
#define IC_DATA_CMD_RESTART	0x400U

/* IC_RAW_INTR_STAT: the transfer was aborted, and a stop condition was sent. */
#define IC_INTR_TX_ABRT		0x40U
#define IC_INTR_STOP_DET	0x200U

/* IC_ENABLE: the core is on, and an abort of the transfer under way. */
#define IC_ENABLE_ON		0x1U
#define IC_ENABLE_ABORT		0x2U

/* IC_STATUS: the master is busy on the bus. */
#define IC_STATUS_MASTER_ACTIVITY	0x20U

/* IC_TX_ABRT_SOURCE: the device acknowledged no 7-bit address. */
#define IC_ABRT_7BIT_ADDRESS_NACK	0x01U

/* The 7-bit address space. */
#define I2C_ADDRESS_7BIT_MAX	0x7fU

/* How long a transfer and a change of the enable bit may take, in milliseconds. */
#define TRANSFER_TIMEOUT_MS	100U
#define ENABLE_TIMEOUT_MS	10U

/*
 * One controller: its PCI function, its mapped registers, the depths of
 * its FIFOs, the input clock the counts of each speed are made from, and
 * its bus.
 *
 * An instance lives from attach for as long as the kernel runs: the bus it
 * registers is never removed, so detach refuses.
 */
struct lpss_i2c {
	struct drv_pci_device *pci;
	struct drv_pci_mapping registers;
	struct drv_pci_enable_state enable_state;
	unsigned tx_depth;
	unsigned rx_depth;
	uint32_t clock_khz;
	struct drv_i2c_bus *bus;

	/* Nonzero from a suspend, which holds the bus, to its resume (ws052-p005). */
	unsigned suspended;
};

static int lpss_attach(struct drv_pci_device *device, const struct drv_pci_id *id);
static int lpss_detach(struct drv_pci_device *device, unsigned flags);
static int lpss_suspend(struct drv_pci_device *device);
static int lpss_resume(struct drv_pci_device *device);
static int lpss_power_on(struct lpss_i2c *controller);
static int lpss_core_start(struct lpss_i2c *controller);
static int lpss_transfer(void *argument, uint16_t address, uint32_t speed, const uint8_t *write, size_t write_length, uint8_t *read, size_t read_length);
static int lpss_run(struct lpss_i2c *controller, const uint8_t *write, size_t write_length, uint8_t *read, size_t read_length);
static int lpss_set_enabled(struct lpss_i2c *controller, bool on);
static uint32_t lpss_count(const struct lpss_i2c *controller, unsigned nanoseconds);
static uint32_t lpss_read(struct lpss_i2c *controller, unsigned offset);
static void lpss_write(struct lpss_i2c *controller, unsigned offset, uint32_t value);
static void lpss_pause(void);

/*
 * Registers the driver for the Alder Lake and Tiger Lake-LP PCHs' I2C controllers.
 */
int
drv_pci_lpss_i2c_driver_register(void)
{
	static const struct drv_pci_id identifiers[] = {
		{ LPSS_VENDOR_INTEL, LPSS_ADL_I2C0, DRV_PCI_ANY_ID, DRV_PCI_ANY_ID, 0U, 0U, LPSS_ADL_CLOCK_KHZ },
		{ LPSS_VENDOR_INTEL, LPSS_ADL_I2C1, DRV_PCI_ANY_ID, DRV_PCI_ANY_ID, 0U, 0U, LPSS_ADL_CLOCK_KHZ },
		{ LPSS_VENDOR_INTEL, LPSS_ADL_I2C2, DRV_PCI_ANY_ID, DRV_PCI_ANY_ID, 0U, 0U, LPSS_ADL_CLOCK_KHZ },
		{ LPSS_VENDOR_INTEL, LPSS_ADL_I2C3, DRV_PCI_ANY_ID, DRV_PCI_ANY_ID, 0U, 0U, LPSS_ADL_CLOCK_KHZ },
		{ LPSS_VENDOR_INTEL, LPSS_ADL_I2C4, DRV_PCI_ANY_ID, DRV_PCI_ANY_ID, 0U, 0U, LPSS_ADL_CLOCK_KHZ },
		{ LPSS_VENDOR_INTEL, LPSS_ADL_I2C5, DRV_PCI_ANY_ID, DRV_PCI_ANY_ID, 0U, 0U, LPSS_ADL_CLOCK_KHZ },
		{ LPSS_VENDOR_INTEL, LPSS_TGL_LP_I2C0, DRV_PCI_ANY_ID, DRV_PCI_ANY_ID, 0U, 0U, LPSS_TGL_LP_CLOCK_KHZ },
		{ LPSS_VENDOR_INTEL, LPSS_TGL_LP_I2C1, DRV_PCI_ANY_ID, DRV_PCI_ANY_ID, 0U, 0U, LPSS_TGL_LP_CLOCK_KHZ },
		{ LPSS_VENDOR_INTEL, LPSS_TGL_LP_I2C2, DRV_PCI_ANY_ID, DRV_PCI_ANY_ID, 0U, 0U, LPSS_TGL_LP_CLOCK_KHZ },
		{ LPSS_VENDOR_INTEL, LPSS_TGL_LP_I2C3, DRV_PCI_ANY_ID, DRV_PCI_ANY_ID, 0U, 0U, LPSS_TGL_LP_CLOCK_KHZ },
		{ LPSS_VENDOR_INTEL, LPSS_TGL_LP_I2C4, DRV_PCI_ANY_ID, DRV_PCI_ANY_ID, 0U, 0U, LPSS_TGL_LP_CLOCK_KHZ },
		{ LPSS_VENDOR_INTEL, LPSS_TGL_LP_I2C5, DRV_PCI_ANY_ID, DRV_PCI_ANY_ID, 0U, 0U, LPSS_TGL_LP_CLOCK_KHZ },
		{ LPSS_VENDOR_INTEL, LPSS_TGL_LP_I2C6, DRV_PCI_ANY_ID, DRV_PCI_ANY_ID, 0U, 0U, LPSS_TGL_LP_CLOCK_KHZ },
		{ LPSS_VENDOR_INTEL, LPSS_TGL_LP_I2C7, DRV_PCI_ANY_ID, DRV_PCI_ANY_ID, 0U, 0U, LPSS_TGL_LP_CLOCK_KHZ }
	};
	static struct drv_pci_driver driver = {
		"lpss-i2c", identifiers, sizeof(identifiers) / sizeof(identifiers[0]), NULL, lpss_attach, lpss_detach,
		NULL, lpss_suspend, lpss_resume, { 0U, 0U, 0U, 0U }
	};
	int error;

	/* Lets PCI bind every listed controller. */
	error = drv_pci_driver_register(&driver);
	if (error != 0)
		return error;

	/* Succeeded: later PCI probing attaches each controller. */
	return 0;
}

/* Brings one controller up and registers its bus. */
static int
lpss_attach(
	struct drv_pci_device *device,
	const struct drv_pci_id *id)
{
	struct lpss_i2c *controller;
	struct drv_i2c_bus_ops ops;
	struct drv_pci_address address;
	int error;

	/* Allocates the controller's state. */
	controller = kern_calloc(1U, sizeof(*controller));
	if (controller == NULL)
		return ENOMEM;
	controller->pci = device;

	/* The input clock of the PCH generation the matched identifier names. */
	controller->clock_khz = (uint32_t)id->driver_data;

	/* Gives PCI the owner before the hardware is touched. */
	error = drv_pci_device_set_driver_data(device, controller);
	if (error != 0) {
		kern_free(controller);
		return error;
	}

	/* Powers the function and maps its registers. */
	error = lpss_power_on(controller);
	if (error != 0) {
		kern_logf("lpss-i2c: attach failed while powering on (%d)\n", error);
		return error;
	}

	/* Lets the core out of reset and makes it a master. */
	error = lpss_core_start(controller);
	if (error != 0) {
		kern_logf("lpss-i2c: attach failed while starting the core (%d)\n", error);
		return error;
	}

	/* Registers the bus under the function's PCI address. */
	drv_pci_device_address(device, &address);
	kern_memset(&ops, 0, sizeof(ops));
	ops.pci_segment = address.segment;
	ops.pci_bus = address.bus;
	ops.pci_device = address.device;
	ops.pci_function = address.function;
	ops.transfer = lpss_transfer;
	ops.argument = controller;
	error = drv_i2c_bus_register(&ops, &controller->bus);
	if (error != 0) {
		kern_logf("lpss-i2c: bus registration failed (%d)\n", error);
		return error;
	}

	/* Succeeded: the log says which bus came up and its FIFOs. */
	kern_logf("lpss-i2c: %02x:%02x.%u ready, FIFO tx %u rx %u, clock %u kHz\n",
		  (unsigned)address.bus,
		  (unsigned)address.device,
		  (unsigned)address.function,
		  controller->tx_depth,
		  controller->rx_depth,
		  (unsigned)controller->clock_khz);
	return 0;
}

/* Refuses to detach: a registered bus stays for the kernel's life. */
static int
lpss_detach(
	struct drv_pci_device *device,
	unsigned flags)
{
	/* The I2C registry has no removal; the controller keeps its bus. */
	(void)device;
	(void)flags;
	return EBUSY;
}

/*
 * Suspends the controller for S0 idle (ws052-p005): the bus is held, so the
 * transfer under way ends and the clients' later transfers (the touch
 * pad's reads) wait, and the core is turned off.  The PCI power code then
 * saves the function's configuration and puts it in D3hot, which resets
 * the core.
 */
static int
lpss_suspend(
	struct drv_pci_device *device)
{
	struct lpss_i2c *controller;

	/* A controller without a bus has nothing to suspend. */
	controller = drv_pci_device_driver_data(device);
	if (controller == NULL || controller->bus == NULL)
		return 0;

	/* Holds the bus, then turns the core off. */
	drv_i2c_bus_hold(controller->bus);
	(void)lpss_set_enabled(controller, false);

	/* suspended tells the resume to start the core and release the bus. */
	controller->suspended = 1U;

	/* Succeeded: the controller may go to D3hot. */
	return 0;
}

/*
 * Resumes the controller after S0 idle: the core, reset by D3hot, is let
 * out of reset and set up again, and the bus is released.  A core that
 * does not start is reported; the bus is released anyway, and its
 * transfers fail.
 */
static int
lpss_resume(
	struct drv_pci_device *device)
{
	struct lpss_i2c *controller;
	int error;

	/* A controller that was not suspended has nothing to resume. */
	controller = drv_pci_device_driver_data(device);
	if (controller == NULL || controller->suspended == 0U)
		return 0;

	/* Starts the core again, then lets the waiting transfers run. */
	error = lpss_core_start(controller);
	controller->suspended = 0U;
	drv_i2c_bus_release(controller->bus);
	if (error != 0) {
		kern_logf("lpss-i2c: the core did not start after the resume (%d)\n", error);
		return error;
	}

	/* Succeeded: the bus carries transfers again. */
	return 0;
}

/*
 * Brings the function to D0, enables its memory decoding and maps BAR0.
 */
static int
lpss_power_on(
	struct lpss_i2c *controller)
{
	struct drv_pci_address address;
	struct drv_pci_bar bar;
	unsigned capability;
	uint16_t control;
	int error;

	/* Saves the command bits for a later restore, and enables register decoding. */
	error = drv_pci_device_save_enable_state(controller->pci, &controller->enable_state);
	if (error != 0)
		return error;
	error = drv_pci_device_enable_memory(controller->pci);
	if (error != 0)
		return error;

	/* Finds the power management capability; a function without one is always in D0. */
	error = drv_pci_device_find_capability(controller->pci, PCI_CAPABILITY_PM, &capability);
	if (error == 0) {
		/* Reads the power state. */
		error = drv_pci_device_config_read16(controller->pci, capability + PCI_PM_CONTROL, &control);
		if (error != 0)
			return error;

		/* A function in D1 to D3 is brought to D0 and given its recovery time. */
		if ((control & PCI_PM_STATE_MASK) != 0U) {
			control = (uint16_t)(control & ~PCI_PM_STATE_MASK);
			error = drv_pci_device_config_write16(controller->pci, capability + PCI_PM_CONTROL, control);
			if (error != 0)
				return error;
			kern_usleep_range(LPSS_D3_RECOVERY_US, LPSS_D3_RECOVERY_US);
		}
	}

	/* Names the function in what the log says about its BAR. */
	drv_pci_device_address(controller->pci, &address);

	/* Checks that BAR0 is a memory window holding the core and the private registers. */
	error = drv_pci_device_bar(controller->pci, 0U, &bar);
	if (error != 0) {
		kern_logf("lpss-i2c: %04x:%02x:%02x.%x has no BAR0 (%d)\n",
			  address.segment,
			  address.bus,
			  address.device,
			  address.function,
			  error);
		return error;
	}
	if (bar.type != DRV_PCI_BAR_MEMORY32 && bar.type != DRV_PCI_BAR_MEMORY64)
		return ENODEV;
	if (bar.size < LPSS_BAR_SIZE)
		return ENODEV;

	/* Claims and maps the register window, uncached. */
	error = drv_pci_device_claim_bar(controller->pci, 0U);
	if (error != 0)
		return error;
	error = drv_pci_device_map_bar(controller->pci,
				       0U,
				       DRV_PCI_MAP_READ | DRV_PCI_MAP_WRITE | DRV_PCI_MAP_NOCACHE,
				       &controller->registers);
	if (error != 0) {
		/* Says which window would not map (BUG-195: two controllers failed with EINVAL on the 5330). */
		kern_logf("lpss-i2c: %04x:%02x:%02x.%x BAR0 type %d at 0x%llx size 0x%llx did not map (%d)\n",
			  address.segment,
			  address.bus,
			  address.device,
			  address.function,
			  (int)bar.type,
			  (unsigned long long)bar.bus_address,
			  (unsigned long long)bar.size,
			  error);
		return error;
	}

	/* Succeeded: the registers can be read. */
	return 0;
}

/*
 * Lets the core out of reset, checks it is a DesignWare I2C core, and sets
 * it up as a master with its interrupts masked.
 */
static int
lpss_core_start(
	struct lpss_i2c *controller)
{
	uint32_t type;
	uint32_t parameters;
	int error;

	/* Releases the core and its DMA from reset. */
	lpss_write(controller, LPSS_PRIVATE_RESETS, LPSS_RESETS_RELEASED);

	/* Refuses a function whose core is not a DesignWare I2C core. */
	type = lpss_read(controller, IC_COMP_TYPE);
	if (type != IC_COMP_TYPE_VALUE) {
		kern_logf("lpss-i2c: component type 0x%08x is not an I2C core\n", type);
		return ENODEV;
	}

	/* The FIFO depths, each one more than its field. */
	parameters = lpss_read(controller, IC_COMP_PARAM_1);
	controller->tx_depth = ((parameters >> 16) & 0xffU) + 1U;
	controller->rx_depth = ((parameters >> 8) & 0xffU) + 1U;

	/* The core is set up while it is off. */
	error = lpss_set_enabled(controller, false);
	if (error != 0)
		return error;

	/* The SCL counts of both speeds, from the input clock. */
	lpss_write(controller, IC_SS_SCL_HCNT, lpss_count(controller, STANDARD_HIGH_NS));
	lpss_write(controller, IC_SS_SCL_LCNT, lpss_count(controller, STANDARD_LOW_NS));
	lpss_write(controller, IC_FS_SCL_HCNT, lpss_count(controller, FAST_HIGH_NS));
	lpss_write(controller, IC_FS_SCL_LCNT, lpss_count(controller, FAST_LOW_NS));

	/* A fast mode master; transfers choose the speed again (lpss_transfer). */
	lpss_write(controller, IC_CON, IC_CON_MASTER | IC_CON_SPEED_FAST | IC_CON_RESTART_EN | IC_CON_SLAVE_DISABLE);

	/* No interrupt is used: the transfers watch the FIFOs and the raw status. */
	lpss_write(controller, IC_INTR_MASK, 0U);
	lpss_write(controller, IC_RX_TL, 0U);
	lpss_write(controller, IC_TX_TL, 0U);
	(void)lpss_read(controller, IC_CLR_INTR);

	/* Succeeded: the core waits for its first transfer. */
	return 0;
}

/*
 * Makes one combined transfer for the I2C registry: the address and the
 * speed are set while the core is off, then the core runs the transfer.
 */
static int
lpss_transfer(
	void *argument,
	uint16_t address,
	uint32_t speed,
	const uint8_t *write,
	size_t write_length,
	uint8_t *read,
	size_t read_length)
{
	struct lpss_i2c *controller;
	uint32_t control;
	int error;
	int stopped;

	/* The controller the bus was registered with. */
	controller = argument;

	/* Refuses an empty transfer and a 10-bit address, which no client uses. */
	if (write_length == 0U && read_length == 0U)
		return EINVAL;
	if (address > I2C_ADDRESS_7BIT_MAX)
		return EINVAL;

	/* The core is turned off to take a new target and speed. */
	error = lpss_set_enabled(controller, false);
	if (error != 0)
		return error;

	/* Fast mode at 400 kHz and above, standard mode below it. */
	control = IC_CON_MASTER | IC_CON_RESTART_EN | IC_CON_SLAVE_DISABLE;
	if (speed >= DRV_I2C_SPEED_FAST) {
		control |= IC_CON_SPEED_FAST;
	} else {
		control |= IC_CON_SPEED_STANDARD;
	}

	/* The control and the target, written while the core is off. */
	lpss_write(controller, IC_CON, control);
	lpss_write(controller, IC_TAR, address);

	/* Clears what an earlier transfer left, and turns the core on. */
	(void)lpss_read(controller, IC_CLR_INTR);
	error = lpss_set_enabled(controller, true);
	if (error != 0)
		return error;

	/* Runs the transfer. */
	error = lpss_run(controller, write, write_length, read, read_length);

	/* The core is left off between transfers. */
	stopped = lpss_set_enabled(controller, false);

	/* Reports a failed transfer before a core that would not stop. */
	if (error != 0)
		return error;
	if (stopped != 0)
		return stopped;

	/* Succeeded: the bytes were written and read. */
	return 0;
}

/*
 * Feeds the commands of one transfer into the transmit FIFO and takes the
 * bytes read out of the receive FIFO until the stop has been sent.
 *
 * The writes come first, then the reads, the first read after a repeated
 * start and the last command with the stop.  No more reads are queued than
 * the receive FIFO can hold, so no byte read is lost.
 */
static int
lpss_run(
	struct lpss_i2c *controller,
	const uint8_t *write,
	size_t write_length,
	uint8_t *read,
	size_t read_length)
{
	uint64_t deadline;
	uint64_t now;
	uint32_t command;
	uint32_t raw;
	uint32_t source;
	size_t total;
	size_t queued;
	size_t received;
	size_t reads_queued;
	unsigned level;
	bool moved;

	/* Every byte written and every byte read is one command. */
	total = write_length + read_length;
	queued = 0;
	received = 0;
	deadline = sched_ticks() + kern_ms_to_ticks(TRANSFER_TIMEOUT_MS);

	/* Moves the commands and the bytes until the stop is sent, the transfer aborts, or the time is up. */
	for (;;) {
		moved = false;

		/* Queues commands while the transmit FIFO has room and the reads fit the receive FIFO. */
		level = (unsigned)lpss_read(controller, IC_TXFLR);
		while (queued < total && level < controller->tx_depth) {
			/* A read waits while the reads queued and not taken would fill the receive FIFO. */
			reads_queued = 0;
			if (queued > write_length)
				reads_queued = queued - write_length;
			if (queued >= write_length && reads_queued - received >= controller->rx_depth)
				break;

			/* A byte to write, or a read. */
			if (queued < write_length) {
				command = write[queued];
			} else {
				command = IC_DATA_CMD_READ;
			}

			/* The first read after the writes starts again; the last command stops. */
			if (queued == write_length && write_length != 0U)
				command |= IC_DATA_CMD_RESTART;
			if (queued + 1U == total)
				command |= IC_DATA_CMD_STOP;

			/* Queues the command. */
			lpss_write(controller, IC_DATA_CMD, command);
			queued++;
			level++;
			moved = true;
		}

		/* Takes the bytes that have been read. */
		level = (unsigned)lpss_read(controller, IC_RXFLR);
		while (level != 0U && received < read_length) {
			read[received] = (uint8_t)(lpss_read(controller, IC_DATA_CMD) & 0xffU);
			received++;
			level--;
			moved = true;
		}

		/* An aborted transfer: the device's missing acknowledgement or another fault of the bus. */
		raw = lpss_read(controller, IC_RAW_INTR_STAT);
		if ((raw & IC_INTR_TX_ABRT) != 0U) {
			source = lpss_read(controller, IC_TX_ABRT_SOURCE);
			(void)lpss_read(controller, IC_CLR_TX_ABRT);
			if ((source & IC_ABRT_7BIT_ADDRESS_NACK) != 0U)
				return ENXIO;
			kern_logf("lpss-i2c: transfer aborted, source 0x%08x\n", source);
			return EIO;
		}

		/* Every command was sent and every byte taken: the stop ends the transfer. */
		if (queued == total && received == read_length && (raw & IC_INTR_STOP_DET) != 0U) {
			(void)lpss_read(controller, IC_CLR_STOP_DET);
			break;
		}

		/* A transfer that has not finished in time is aborted. */
		now = sched_ticks();
		if (now >= deadline) {
			lpss_write(controller, IC_ENABLE, IC_ENABLE_ON | IC_ENABLE_ABORT);
			kern_logf("lpss-i2c: transfer timed out, %u of %u commands, %u of %u bytes read\n",
				  (unsigned)queued,
				  (unsigned)total,
				  (unsigned)received,
				  (unsigned)read_length);
			return ETIMEDOUT;
		}

		/* Nothing moved: the bus is busy with the bytes queued, so the thread waits. */
		if (!moved)
			lpss_pause();
	}

	/* Succeeded: the stop was sent after the last byte. */
	return 0;
}

/* Turns the core on or off and waits until it says it is. */
static int
lpss_set_enabled(
	struct lpss_i2c *controller,
	bool on)
{
	uint64_t deadline;
	uint64_t now;
	uint32_t wanted;
	uint32_t status;

	/* Asks for the state. */
	wanted = 0U;
	if (on)
		wanted = IC_ENABLE_ON;
	lpss_write(controller, IC_ENABLE, wanted);

	/* Waits for the core to follow (it finishes a byte on the bus first). */
	deadline = sched_ticks() + kern_ms_to_ticks(ENABLE_TIMEOUT_MS) + 1U;
	for (;;) {
		/* The core is in the state asked for. */
		status = lpss_read(controller, IC_ENABLE_STATUS) & IC_ENABLE_ON;
		if (status == wanted)
			break;

		/* A core that does not follow in time. */
		now = sched_ticks();
		if (now >= deadline) {
			kern_logf("lpss-i2c: core did not turn on or off (asked %u)\n", (unsigned)wanted);
			return ETIMEDOUT;
		}

		/* Waits a tick for the core. */
		lpss_pause();
	}

	/* Succeeded: the core is on or off. */
	return 0;
}

/* Gives the count of the controller's input clock periods that lasts a time, rounded to the nearest. */
static uint32_t
lpss_count(
	const struct lpss_i2c *controller,
	unsigned nanoseconds)
{
	uint64_t count;

	/* Clock periods in kilohertz times nanoseconds, over a million, rounded. */
	count = ((uint64_t)controller->clock_khz * nanoseconds + 500000U) / 1000000U;

	/* Succeeded: the count of the time. */
	return (uint32_t)count;
}

/* Reads a register of the core or of the LPSS private block. */
static uint32_t
lpss_read(
	struct lpss_i2c *controller,
	unsigned offset)
{
	uint32_t value;

	/* One 32-bit read of the mapped window. */
	value = kern_mmio_read32((uint8_t *)controller->registers.address + offset);

	/* Succeeded: the register's value. */
	return value;
}

/* Writes a register of the core or of the LPSS private block. */
static void
lpss_write(
	struct lpss_i2c *controller,
	unsigned offset,
	uint32_t value)
{
	/* One 32-bit write of the mapped window. */
	kern_mmio_write32((uint8_t *)controller->registers.address + offset, value);
}

/*
 * Waits while the bus moves bytes, for a millisecond.  Transfers are made
 * by the clients' threads, never while the platform starts.
 */
static void
lpss_pause(void)
{
	/* A short sleep lets other threads run while the bus works. */
	sched_sleep(sched_ticks() + kern_ms_to_ticks(LPSS_PAUSE_MS));
}
