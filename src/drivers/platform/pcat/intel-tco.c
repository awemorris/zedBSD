/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The Intel TCO watchdog: a machine that stops is reset.
 *
 * Intel's platform controller hubs carry a down counter, the TCO timer,
 * that resets the machine when it runs out, unless software reloads it
 * first.  The driver is off unless the boot parameter watchdog=<seconds>
 * asks for it.  It then programs the counter so that the machine resets
 * that many seconds after the last reload, and reloads it from one
 * kernel thread on each CPU: the reload happens only once every CPU's
 * thread has run since the one before.  A CPU that stops taking
 * interrupts, a scheduler that stops, or a panic, which halts its CPU with
 * interrupts off, therefore lets the counter run out and the machine is
 * reset.
 *
 * Two kinds of hub are known.  On the hubs from the 100 Series on, the
 * TCO registers are found through the SMBus function (device 31,
 * function 4); their counter resets the machine shortly after it first
 * runs out, and it counts only while the ACPI timer runs, which the power
 * management controller can stop.  On the ICH9 they lie in the ACPI I/O
 * space of the LPC bridge (device 31, function 0); its counter resets the
 * machine when it runs out the second time in a row, and the bit that
 * forbids the reset is in the chipset configuration space.
 *
 * The counter is halted while the system sleeps to idle, when no thread
 * runs to reload it, and armed again when the system wakes.
 *
 * The boot parameter watchdog.stop=<seconds> stops every CPU with its
 * interrupts off that many seconds after the watchdog is armed, to show
 * on a machine that the watchdog resets it.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <drivers/pci/pci.h>
#include <kern/clock.h>
#include <kern/device-io.h>
#include <kern/irq.h>
#include <kern/klog.h>
#include <kern/platform.h>
#include <kern/pmem.h>
#include <kern/sched.h>
#include <kern/thread.h>
#include <kern/types.h>
#include <uapi/errno.h>

#if CONFIG_DRIVER_ACPI
#include <drivers/acpi/acpi.h>
#endif

/* Marks a parameter of a driver callback that is not used. */
#define UNUSED_PARAMETER(parameter)	((void)(parameter))

/* The boot parameters that arm the watchdog and that stop the machine on purpose. */
#define TCO_PARAMETER_SECONDS		"watchdog="
#define TCO_PARAMETER_STOP		"watchdog.stop="

/* The word that leaves the watchdog off, as if watchdog= were absent. */
#define TCO_PARAMETER_OFF		"off"

/* The shortest and the longest time the boot parameter may give, in seconds. */
#define TCO_SECONDS_MIN			4U
#define TCO_SECONDS_MAX			600U

/* The longest time watchdog.stop= may give, in seconds (a day). */
#define TCO_STOP_SECONDS_MAX		86400U

/* How many reloads the threads make within one timeout. */
#define TCO_RELOADS_PER_TIMEOUT		4U

/* How long the driver watches the counter to see that it counts, in milliseconds. */
#define TCO_COUNT_CHECK_MS		1500U

/* The PCI vendor of every function the driver binds. */
#define TCO_VENDOR_INTEL		0x8086U

/*
 * The functions the driver binds.
 *
 * Values referenced:
 * Intel 700 Series Chipset Family On-Package PCH Datasheet Volume 1
 * (763122-001), the device ID table (51A3h, D31:F4 SMBus); the same
 * function on the Alder Lake-P hub reports 51A3h.  QEMU v10.0.0
 * hw/isa/lpc_ich9.c (the ICH9 LPC bridge's device ID 2918h).
 */
#define TCO_DEVICE_SMBUS_ADL_P		0x51a3U
#define TCO_DEVICE_LPC_ICH9		0x2918U

/*
 * Where the hub keeps the TCO base and its decode enable in the SMBus
 * function's configuration space.
 *
 * Values referenced:
 * Intel 700 Series Chipset Family On-Package PCH Datasheet Volume 2
 * (764889-001), 6.1.17 TCOBASE (offset 50h, base in bits 15:5) and 6.1.18
 * TCOCTL (offset 54h, TCO_BASE_EN bit 8).
 */
#define TCO_SMBUS_BASE			0x50U
#define TCO_SMBUS_BASE_MASK		0xffe0U
#define TCO_SMBUS_CONTROL		0x54U
#define TCO_SMBUS_CONTROL_DECODE	(1U << 8)

/*
 * The power management controller, whose memory holds the switch of the
 * ACPI timer the TCO counter counts with.
 *
 * Values referenced:
 * Intel 700 Series Chipset Family On-Package PCH Datasheet Volume 2
 * (764889-001), 4.1.5 PWRMBASE (D31:F2 offset 10h, a 64-bit BAR whose
 * base is in bits 31:13) and 4.3.71 ACPI_TMR_CTL (offset 18FCh,
 * ACPI_TIM_DIS bit 1).
 */
#define TCO_PMC_DEVICE			31U
#define TCO_PMC_FUNCTION		2U
#define TCO_PMC_BAR_LOW			0x10U
#define TCO_PMC_BAR_HIGH		0x14U
#define TCO_PMC_BAR_MASK		0xffffe000U
#define TCO_PMC_TIMER_CONTROL		0x18fcU
#define TCO_PMC_TIMER_DISABLE		(1U << 1)

/*
 * The ACPI name under which the firmware of a hub whose power management
 * controller is hidden from PCI publishes the controller's base.
 */
#define TCO_PMC_ACPI_NAME		"\\PWRM"

/*
 * Where the ICH9 keeps its ACPI I/O base, the base's decode enable and
 * the chipset configuration base in the LPC bridge's configuration space,
 * and where the TCO registers and the reset's lock are in those.
 *
 * Values referenced:
 * QEMU v10.0.0 include/hw/southbridge/ich9.h (ICH9_LPC_PMBASE 40h with
 * the base in bits 15:7, ICH9_LPC_ACPI_CTRL 44h with ACPI_EN 80h,
 * ICH9_LPC_RCBA F0h with the base in bits 31:14 and the enable bit 0,
 * ICH9_CC_GCS 3410h with NO_REBOOT bit 5, ICH9_PMIO_TCO_RLD 60h).
 */
#define TCO_ICH9_ACPI_BASE		0x40U
#define TCO_ICH9_ACPI_BASE_MASK		0xff80U
#define TCO_ICH9_ACPI_CONTROL		0x44U
#define TCO_ICH9_ACPI_CONTROL_DECODE	0x80U
#define TCO_ICH9_CHIPSET_BASE		0xf0U
#define TCO_ICH9_CHIPSET_BASE_MASK	0xffffc000U
#define TCO_ICH9_CHIPSET_BASE_ENABLE	0x1U
#define TCO_ICH9_TCO_OFFSET		0x60U
#define TCO_ICH9_GENERAL_CONTROL	0x3410U
#define TCO_ICH9_GENERAL_NO_REBOOT	(1U << 5)

/*
 * The TCO registers, by their offset from the TCO base.
 *
 * Values referenced:
 * Intel 700 Series Chipset Family On-Package PCH Datasheet Volume 2
 * (764889-001), 32.1 SMBus TCO I/O Registers: TCO_RLD 0h, TCO1_STS 4h
 * (TIMEOUT bit 3), TCO2_STS 6h (SECOND_TO_STS bit 1,
 * NO_REBOOT_PIN_STRAP_STATUS bit 2), TCO1_CNT 8h (NR_MSUS bit 0, NMI_NOW
 * bit 8, TCO_TMR_HALT bit 11), TCO_TMR 12h (bits 9:0, about 0.6 s a
 * count, 0 and 1 ignored).  QEMU v10.0.0 include/hw/acpi/ich9_tco.h has
 * the same offsets and bits for the ICH9.
 */
#define TCO_RELOAD			0x00U
#define TCO_STATUS1			0x04U
#define TCO_STATUS1_TIMEOUT		(1U << 3)
#define TCO_STATUS2			0x06U
#define TCO_STATUS2_SECOND_TIMEOUT	(1U << 1)
#define TCO_STATUS2_STRAP_NO_REBOOT	(1U << 2)
#define TCO_CONTROL1			0x08U
#define TCO_CONTROL1_NO_REBOOT		(1U << 0)
#define TCO_CONTROL1_NMI_NOW		(1U << 8)
#define TCO_CONTROL1_HALT		(1U << 11)
#define TCO_TIMER			0x12U
#define TCO_COUNT_MASK			0x3ffU
#define TCO_COUNT_MIN			2U
#define TCO_COUNT_MAX			1023U

/*
 * The length of one count in tenths of a second, and how many times the
 * counter runs out before the reset on each kind of hub.
 *
 * The hub's first run out raises an SMI and starts a short timer of
 * fixed length, whose end resets the machine, so the programmed time is
 * the time to the reset: an Alder Lake-P hub loaded with 50 counts reset
 * the machine about 30 s later.
 *
 * Values referenced:
 * Intel 700 Series Chipset Family On-Package PCH Datasheet Volume 2
 * (764889-001), 32.1.11 TCO_TMR (about 0.6 s a count), and Volume 1
 * (763122-001), 4.1 System Management (the first timeout's SMI and the
 * second, hard-coded timer's reset).  QEMU v10.0.0 hw/acpi/ich9_tco.c
 * (tco_timer_expired: the ICH9's reset on the second run out).
 */
#define TCO_COUNT_TENTHS		6U
#define TCO_RUNS_HUB			1U
#define TCO_RUNS_ICH9			2U

/* The page the driver maps around one register of memory space. */
#define TCO_PAGE_SIZE			0x1000U
#define TCO_PAGE_MASK			0xfffU

/* The kinds of hub the driver knows. */
enum tco_kind {
	/* The TCO registers behind the SMBus function, the 100 Series and later. */
	TCO_KIND_HUB = 1,
	/* The TCO registers in the ICH9's ACPI I/O space. */
	TCO_KIND_ICH9
};

/*
 * The one watchdog of the machine.
 *
 * Filled by the attach of the one bound function and by
 * drv_intel_tco_start(); after the start only the threads' stamps and
 * the generation change, and the sleep's suspend and resume halt and arm
 * the counter.  A machine has one hub, so there is one record.
 */
struct tco_watchdog {
	/* The bound function, NULL until the attach. */
	struct drv_pci_device *device;

	/* The kind of hub, which decides where the registers are. */
	enum tco_kind kind;

	/* The I/O port of the TCO registers. */
	uint16_t base;

	/* The mapped page of the ICH9's reset lock, or NULL on a hub. */
	volatile uint8_t *general_page;

	/* The mapped page of the hub's ACPI timer switch, or NULL where it was not found. */
	volatile uint8_t *timer_page;

	/* The time from the last reload to the reset, in seconds. */
	uint32_t seconds;

	/* The count loaded at each reload. */
	uint32_t count;

	/* How long each thread sleeps between its stamps, in kernel ticks. */
	uint64_t period_ticks;

	/* The tick at which watchdog.stop= stops the machine, 0 when it is absent. */
	uint64_t stop_tick;

	/* How many CPUs carry a thread; the reload waits for each of them. */
	unsigned cpu_count;

	/* Nonzero once the counter is armed and the threads run. */
	unsigned running;
};

/* The watchdog; zero until its function is bound. */
static struct tco_watchdog tco;

/*
 * The generation of reloads.
 *
 * Every thread copies it into its CPU's stamp each time it runs; the
 * thread that finds every stamp equal to it moves it on by one and
 * reloads the counter.  It starts at 1 so that the stamps' zero is never
 * a match.
 */
static unsigned tco_generation = 1U;

/* The last generation each CPU's thread saw, written only by that thread. */
static unsigned tco_stamps[KERN_CPU_MAX];

/* Nonzero once one thread has said that watchdog.stop= stops the machine. */
static unsigned tco_stop_said;

/*
 * The reloading threads, one per CPU, made by tco_threads_start() before
 * any of them runs.  The start keeps them here rather than on its stack;
 * they are read only while they are made.
 */
static struct thread *tco_threads[KERN_CPU_MAX];

static int tco_attach(struct drv_pci_device *device, const struct drv_pci_id *id);
static int tco_suspend(struct drv_pci_device *device);
static int tco_resume(struct drv_pci_device *device);
static int tco_parameter_seconds(const char *line, const char *name, uint32_t *seconds);
static uint32_t tco_count_for(enum tco_kind kind, uint32_t seconds);
static int tco_locate_hub(struct drv_pci_device *device);
static int tco_locate_ich9(struct drv_pci_device *device);
static int tco_map_register(uint64_t address, volatile uint8_t **page);
static int tco_locate_timer_switch(void);
static int tco_pmc_base(uint64_t *base);
static uint16_t tco_read16(unsigned offset);
static void tco_write16(unsigned offset, uint16_t value);
static void tco_control_write(uint16_t control);
static void tco_halt(void);
static int tco_reset_allow(void);
static void tco_reset_forbid(void);
static void tco_timer_run(void);
static void tco_arm(void);
static int tco_counting(void);
static int tco_threads_start(void);
static void tco_thread_main(void *argument);
static void tco_stamp(unsigned cpu);
static void tco_stop_machine(void);

/*
 * Registers the TCO watchdog's driver with the PCI core.
 *
 * Only the functions that hold the TCO registers are listed; the attach
 * binds one of them when the boot parameter asks for the watchdog.
 */
int
drv_intel_tco_pci_register(void)
{
	static const struct drv_pci_id identifiers[] = {
		{ TCO_VENDOR_INTEL, TCO_DEVICE_SMBUS_ADL_P, DRV_PCI_ANY_ID, DRV_PCI_ANY_ID, 0U, 0U, TCO_KIND_HUB },
		{ TCO_VENDOR_INTEL, TCO_DEVICE_LPC_ICH9, DRV_PCI_ANY_ID, DRV_PCI_ANY_ID, 0U, 0U, TCO_KIND_ICH9 }
	};
	static struct drv_pci_driver driver = {
		"intel-tco", identifiers, sizeof(identifiers) / sizeof(identifiers[0]), NULL, tco_attach, NULL,
		NULL, tco_suspend, tco_resume, { 0U, 0U, 0U, 0U }
	};
	int error;

	/* Lets PCI bind the function when the host bridge is probed. */
	error = drv_pci_driver_register(&driver);
	if (error != 0)
		return error;

	/* Succeeded: the probe attaches the function. */
	return 0;
}

/*
 * Arms the watchdog the boot parameter asked for.
 *
 * Runs once the PCI functions and the ACPI namespace are attached, on a
 * thread that may sleep.  Lets the counter reset the machine, makes sure
 * it counts, and starts the threads that reload it.  A failure leaves
 * the counter halted and the reset forbidden, as the firmware left them.
 */
int
drv_intel_tco_start(void)
{
	const char *line;
	uint32_t stop_seconds;
	uint64_t period_ms;
	uint16_t status;
	int error;

	/* Nothing to arm when no function was bound. */
	if (tco.device == NULL)
		return ENODEV;

	/* Reads when watchdog.stop= stops the machine, if it is given. */
	line = kern_boot_handoff("boot.command-line");
	stop_seconds = 0U;
	error = tco_parameter_seconds(line, TCO_PARAMETER_STOP, &stop_seconds);
	if (error == EINVAL) {
		kern_logf("watchdog: %s takes a number of seconds; ignored\n", TCO_PARAMETER_STOP);
		stop_seconds = 0U;
	} else if (error == 0 && stop_seconds > TCO_STOP_SECONDS_MAX) {
		kern_logf("watchdog: %s takes at most %u seconds; ignored\n",
			  TCO_PARAMETER_STOP, TCO_STOP_SECONDS_MAX);
		stop_seconds = 0U;
	}

	/* The ICH9's reset lock is reached through the chipset configuration space. */
	if (tco.kind == TCO_KIND_ICH9) {
		error = tco_locate_ich9(tco.device);
		if (error != 0)
			return error;
	}

	/* The hub's counter needs the ACPI timer, which the controller may have stopped. */
	if (tco.kind == TCO_KIND_HUB) {
		error = tco_locate_timer_switch();
		if (error != 0)
			kern_logf("watchdog: the ACPI timer's switch is not found (%d); the counter runs only if the timer does\n", error);
	}

	/* Reads the second status, which holds the strap and the last reset's cause. */
	status = tco_read16(TCO_STATUS2);

	/* Refuses a hub whose strap forbids the reset: the counter would run out for nothing. */
	if (tco.kind == TCO_KIND_HUB && (status & TCO_STATUS2_STRAP_NO_REBOOT) != 0U) {
		kern_logf("watchdog: the board's strap forbids the reset; not armed\n");
		return ENODEV;
	}

	/* Says when the reset before this boot was the watchdog's own. */
	if ((status & TCO_STATUS2_SECOND_TIMEOUT) != 0U)
		kern_logf("watchdog: the last reset was the TCO watchdog's\n");

	/* Lets the counter reset the machine. */
	error = tco_reset_allow();
	if (error != 0) {
		kern_logf("watchdog: the reset cannot be allowed (%d); not armed\n", error);
		return error;
	}

	/* Loads the counter and lets it run. */
	tco_arm();

	/* A counter that does not count would never reset the machine. */
	error = tco_counting();
	if (error != 0) {
		tco_halt();
		tco_reset_forbid();
		kern_logf("watchdog: the TCO counter does not count; not armed\n");
		return error;
	}

	/* Each thread stamps a few times per timeout, so that a late one is still in time. */
	period_ms = (uint64_t)tco.seconds * 1000U / TCO_RELOADS_PER_TIMEOUT;
	tco.period_ticks = kern_ms_to_ticks(period_ms);

	/* The stop on purpose is counted from now. */
	tco.stop_tick = 0U;
	if (stop_seconds != 0U)
		tco.stop_tick = sched_ticks() + kern_ms_to_ticks((uint64_t)stop_seconds * 1000U);

	/* Starts one reloading thread on each CPU. */
	error = tco_threads_start();
	if (error != 0) {
		tco_halt();
		tco_reset_forbid();
		kern_logf("watchdog: the reloading threads cannot start (%d); not armed\n", error);
		return error;
	}

	/* Tells the log what was armed. */
	kern_logf("watchdog: armed, %u s from the last reload to the reset (TCO at 0x%x, count %u, %u CPUs)\n",
		  tco.seconds, (unsigned)tco.base, tco.count, tco.cpu_count);

	/* Tells the log when the machine is to stop on purpose. */
	if (stop_seconds != 0U)
		kern_logf("watchdog: every CPU stops in %u s, as %s asks\n", stop_seconds, TCO_PARAMETER_STOP);

	/* Succeeded: the threads keep the machine from being reset while it runs. */
	return 0;
}

/*
 * Binds the function that holds the TCO registers when the boot
 * parameter asks for the watchdog.
 */
static int
tco_attach(
	struct drv_pci_device *device,
	const struct drv_pci_id *id)
{
	const char *line;
	uint32_t seconds;
	int error;

	/* One watchdog per machine. */
	if (tco.device != NULL)
		return EBUSY;

	/* Leaves the function alone unless the boot parameter asks for the watchdog. */
	line = kern_boot_handoff("boot.command-line");
	error = tco_parameter_seconds(line, TCO_PARAMETER_SECONDS, &seconds);
	if (error == ENOENT)
		return ENODEV;
	if (error != 0) {
		kern_logf("watchdog: %s takes a number of seconds or %s; not armed\n",
			  TCO_PARAMETER_SECONDS, TCO_PARAMETER_OFF);
		return ENODEV;
	}

	/* Refuses a time outside what the counter and the reloads can keep. */
	if (seconds < TCO_SECONDS_MIN || seconds > TCO_SECONDS_MAX) {
		kern_logf("watchdog: %s takes %u to %u seconds; not armed\n",
			  TCO_PARAMETER_SECONDS, TCO_SECONDS_MIN, TCO_SECONDS_MAX);
		return ENODEV;
	}

	/* Finds the hub's TCO registers now; the ICH9's need memory mapped at the start. */
	tco.kind = (enum tco_kind)id->driver_data;
	if (tco.kind == TCO_KIND_HUB) {
		error = tco_locate_hub(device);
		if (error != 0)
			return error;
	}

	/* Keeps the function and the time for the start. */
	tco.device = device;
	tco.seconds = seconds;
	tco.count = tco_count_for(tco.kind, seconds);

	/* Succeeded: drv_intel_tco_start() arms the counter. */
	return 0;
}

/*
 * Halts the counter before the system sleeps to idle, when no thread
 * runs to reload it.
 */
static int
tco_suspend(
	struct drv_pci_device *device)
{
	UNUSED_PARAMETER(device);

	/* A watchdog that was never armed has nothing to halt. */
	if (!tco.running)
		return 0;

	/* Halts the counter where it is. */
	tco_halt();

	/* Succeeded: the counter waits for the resume. */
	return 0;
}

/*
 * Arms the counter again once the system is back from sleeping to idle.
 */
static int
tco_resume(
	struct drv_pci_device *device)
{
	int error;

	UNUSED_PARAMETER(device);

	/* A watchdog that was never armed stays off. */
	if (!tco.running)
		return 0;

	/* Lets the counter reset the machine again, in case the sleep forbade it. */
	error = tco_reset_allow();
	if (error != 0) {
		kern_logf("watchdog: the reset cannot be allowed after the sleep (%d)\n", error);
		return 0;
	}

	/* Reloads the counter and lets it run, the ACPI timer first. */
	tco_arm();

	/* Succeeded: the threads reload the counter again. */
	return 0;
}

/*
 * Reads the number of seconds a boot parameter gives.
 *
 * Reports 0 with the number, ENOENT when the parameter is absent or
 * says off, and EINVAL when it gives anything else.
 */
static int
tco_parameter_seconds(
	const char *line,
	const char *name,
	uint32_t *seconds)
{
	const char *cursor;
	const char *value;
	size_t name_length;
	size_t index;
	uint32_t number;
	unsigned digits;
	bool at_word;

	/* A missing line has no parameter. */
	if (line == NULL)
		return ENOENT;

	/* Measures the name, which ends in its equals sign. */
	name_length = 0U;
	while (name[name_length] != '\0')
		name_length++;

	/* Finds the parameter at the start of a word. */
	value = NULL;
	at_word = true;
	for (cursor = line; *cursor != '\0'; cursor++) {
		/* A blank ends a word; the next character starts one. */
		if (*cursor == ' ' || *cursor == '\t') {
			at_word = true;
			continue;
		}

		/* Only a word's first character can start the name. */
		if (!at_word)
			continue;
		at_word = false;

		/* Compares the word with the name. */
		for (index = 0U; index < name_length; index++) {
			if (cursor[index] != name[index])
				break;
		}

		/* The whole name matched: its value follows. */
		if (index == name_length) {
			value = cursor + name_length;
			break;
		}
	}

	/* The parameter is absent. */
	if (value == NULL)
		return ENOENT;

	/* The word off leaves the parameter as if it were absent. */
	for (index = 0U; TCO_PARAMETER_OFF[index] != '\0'; index++) {
		if (value[index] != TCO_PARAMETER_OFF[index])
			break;
	}

	/* The whole word matched: the value is off when the end or a blank follows it. */
	if (TCO_PARAMETER_OFF[index] == '\0') {
		if (value[index] == '\0' ||
		    value[index] == ' ' ||
		    value[index] == '\t')
			return ENOENT;
	}

	/* Reads the decimal digits, refusing a number past what a day needs. */
	number = 0U;
	digits = 0U;
	for (cursor = value; *cursor >= '0' && *cursor <= '9'; cursor++) {
		number = number * 10U + (uint32_t)(*cursor - '0');
		digits++;
		if (digits > 6U)
			return EINVAL;
	}

	/* Refuses a value without digits or with something after them. */
	if (digits == 0U)
		return EINVAL;

	/* Refuses a number followed by anything but the end of the word. */
	if (*cursor != '\0' &&
	    *cursor != ' ' &&
	    *cursor != '\t')
		return EINVAL;

	/* Succeeded: reports the number. */
	*seconds = number;
	return 0;
}

/*
 * Reports the count that makes the reset come the given number of
 * seconds after a reload, rounded up to a whole count.
 */
static uint32_t
tco_count_for(
	enum tco_kind kind,
	uint32_t seconds)
{
	uint32_t runs;
	uint32_t tenths_per_run;
	uint32_t count;

	/* The ICH9 resets on the second run out, so each run is half the time. */
	runs = TCO_RUNS_HUB;
	if (kind == TCO_KIND_ICH9)
		runs = TCO_RUNS_ICH9;

	/* Divides the time of one run by the length of a count, rounding up. */
	tenths_per_run = seconds * 10U / runs;
	count = (tenths_per_run + TCO_COUNT_TENTHS - 1U) / TCO_COUNT_TENTHS;

	/* Keeps the count within what the register takes. */
	if (count < TCO_COUNT_MIN)
		count = TCO_COUNT_MIN;
	if (count > TCO_COUNT_MAX)
		count = TCO_COUNT_MAX;

	/* Reports the count. */
	return count;
}

/*
 * Finds the hub's TCO registers through the SMBus function, which the
 * firmware has given the TCO base and its decode.
 */
static int
tco_locate_hub(
	struct drv_pci_device *device)
{
	uint32_t base;
	uint32_t control;
	int error;

	/* Reads the TCO base. */
	error = drv_pci_device_config_read32(device, TCO_SMBUS_BASE, &base);
	if (error != 0)
		return error;

	/* Reads whether the base is decoded. */
	error = drv_pci_device_config_read32(device, TCO_SMBUS_CONTROL, &control);
	if (error != 0)
		return error;

	/* A base the firmware did not decode cannot be reached. */
	if ((control & TCO_SMBUS_CONTROL_DECODE) == 0U) {
		kern_logf("watchdog: the firmware left the TCO registers undecoded\n");
		return ENODEV;
	}

	/* A decoded base of zero names no register. */
	if ((base & TCO_SMBUS_BASE_MASK) == 0U) {
		kern_logf("watchdog: the firmware gave the TCO registers no base\n");
		return ENODEV;
	}

	/* Succeeded: the registers are at the base. */
	tco.base = (uint16_t)(base & TCO_SMBUS_BASE_MASK);
	return 0;
}

/*
 * Finds the ICH9's TCO registers in its ACPI I/O space, and maps the
 * register whose bit forbids the reset.
 */
static int
tco_locate_ich9(
	struct drv_pci_device *device)
{
	uint32_t acpi_base;
	uint32_t chipset_base;
	uint8_t acpi_control;
	int error;

	/* Reads the ACPI I/O base. */
	error = drv_pci_device_config_read32(device, TCO_ICH9_ACPI_BASE, &acpi_base);
	if (error != 0)
		return error;

	/* Reads whether the ACPI I/O space is decoded. */
	error = drv_pci_device_config_read8(device, TCO_ICH9_ACPI_CONTROL, &acpi_control);
	if (error != 0)
		return error;

	/* An ACPI space the firmware did not decode cannot be reached. */
	if ((acpi_control & TCO_ICH9_ACPI_CONTROL_DECODE) == 0U) {
		kern_logf("watchdog: the firmware left the ACPI I/O space undecoded\n");
		return ENODEV;
	}

	/* A decoded base of zero names no register. */
	if ((acpi_base & TCO_ICH9_ACPI_BASE_MASK) == 0U) {
		kern_logf("watchdog: the firmware gave the ACPI I/O space no base\n");
		return ENODEV;
	}

	/* Reads the chipset configuration base, which holds the reset's lock. */
	error = drv_pci_device_config_read32(device, TCO_ICH9_CHIPSET_BASE, &chipset_base);
	if (error != 0)
		return error;

	/* A configuration space the firmware did not enable cannot be reached. */
	if ((chipset_base & TCO_ICH9_CHIPSET_BASE_ENABLE) == 0U) {
		kern_logf("watchdog: the firmware left the chipset configuration space disabled\n");
		return ENODEV;
	}

	/* Maps the page of the reset's lock. */
	error = tco_map_register((uint64_t)(chipset_base & TCO_ICH9_CHIPSET_BASE_MASK) + TCO_ICH9_GENERAL_CONTROL,
				 &tco.general_page);
	if (error != 0)
		return error;

	/* Succeeded: the TCO registers follow the ACPI registers. */
	tco.base = (uint16_t)((acpi_base & TCO_ICH9_ACPI_BASE_MASK) + TCO_ICH9_TCO_OFFSET);
	return 0;
}

/*
 * Maps the page around one register of memory space for the kernel's
 * life; the caller adds the register's offset in the page.
 */
static int
tco_map_register(
	uint64_t address,
	volatile uint8_t **page)
{
	void *mapped;
	int error;

	/* Maps the page that holds the register, uncached. */
	error = kern_device_map(address & ~(uint64_t)TCO_PAGE_MASK, TCO_PAGE_SIZE, KERN_DEVICE_UNCACHED, &mapped);
	if (error != 0)
		return error;

	/* Succeeded: the page stays mapped. */
	*page = mapped;
	return 0;
}

/*
 * Finds and maps the power management controller's ACPI timer switch.
 */
static int
tco_locate_timer_switch(void)
{
	uint64_t base;
	int error;

	/* Finds the controller. */
	error = tco_pmc_base(&base);
	if (error != 0)
		return error;

	/* Maps the switch. */
	error = tco_map_register(base + TCO_PMC_TIMER_CONTROL, &tco.timer_page);
	if (error != 0)
		return error;

	/* Succeeded: the switch is mapped. */
	return 0;
}

/*
 * Finds the power management controller's memory: its BAR when the
 * function answers on PCI, else the base the firmware's ACPI tables
 * publish for their own use when the firmware hides the function.
 */
static int
tco_pmc_base(
	uint64_t *base)
{
	struct drv_pci_address address;
	struct drv_pci_device *controller;
	uint16_t vendor;
	uint32_t low;
	uint32_t high;
	int error;
#if CONFIG_DRIVER_ACPI
	uint64_t published;
#endif

	/* Looks for the controller's function next to the SMBus function. */
	address.segment = 0U;
	address.bus = 0U;
	address.device = TCO_PMC_DEVICE;
	address.function = TCO_PMC_FUNCTION;
	controller = drv_pci_find_device(&address);
	vendor = 0U;
	if (controller != NULL)
		vendor = drv_pci_device_vendor(controller);

	/* Reads the BAR of a controller that answers. */
	if (vendor == TCO_VENDOR_INTEL) {
		error = drv_pci_device_config_read32(controller, TCO_PMC_BAR_LOW, &low);
		if (error != 0)
			return error;
		error = drv_pci_device_config_read32(controller, TCO_PMC_BAR_HIGH, &high);
		if (error != 0)
			return error;

		/* A BAR the firmware left empty names nothing. */
		if ((low & TCO_PMC_BAR_MASK) == 0U && high == 0U)
			return ENODEV;

		/* Succeeded: the BAR's base. */
		*base = ((uint64_t)high << 32) | (uint64_t)(low & TCO_PMC_BAR_MASK);
		return 0;
	}

#if CONFIG_DRIVER_ACPI
	/* Reads the base the firmware published for its own methods. */
	error = drv_acpi_evaluate_integer(NULL, TCO_PMC_ACPI_NAME, &published);
	if (error != 0)
		return error;

	/* Refuses a value that cannot be the controller's 8 KiB of memory. */
	if (published == 0U)
		return ENODEV;

	/* Refuses a value that is not aligned to the controller's memory. */
	if ((published & ~(uint64_t)TCO_PMC_BAR_MASK) != 0U)
		return EINVAL;

	/* Succeeded: the published base. */
	*base = published;
	return 0;
#else
	/* Without ACPI a hidden controller cannot be found. */
	return ENODEV;
#endif
}

/* Reads one 16-bit TCO register. */
static uint16_t
tco_read16(
	unsigned offset)
{
	uint16_t value;

	/* Reads the register from I/O space. */
	value = kern_io_in16((uint16_t)(tco.base + offset));

	/* Reports the register's value. */
	return value;
}

/* Writes one 16-bit TCO register. */
static void
tco_write16(
	unsigned offset,
	uint16_t value)
{
	/* Writes the register in I/O space. */
	kern_io_out16((uint16_t)(tco.base + offset), value);
}

/*
 * Writes the first control register, never with NMI_NOW, whose one would
 * raise a nonmaskable interrupt.
 */
static void
tco_control_write(
	uint16_t control)
{
	/* Writes the control with NMI_NOW cleared. */
	tco_write16(TCO_CONTROL1, (uint16_t)(control & ~TCO_CONTROL1_NMI_NOW));
}

/* Halts the counter where it is. */
static void
tco_halt(void)
{
	uint16_t control;

	/* Sets the halt, keeping the other bits. */
	control = tco_read16(TCO_CONTROL1);
	tco_control_write((uint16_t)(control | TCO_CONTROL1_HALT));
}

/*
 * Clears the bit that forbids the counter's reset, and reads it back.
 * Reports 0, or EIO when the bit does not clear.
 */
static int
tco_reset_allow(void)
{
	uint16_t control;
	uint32_t general;

	/* The hub keeps the bit in the TCO's first control register. */
	if (tco.kind == TCO_KIND_HUB) {
		control = tco_read16(TCO_CONTROL1);
		tco_control_write((uint16_t)(control & ~TCO_CONTROL1_NO_REBOOT));

		/* A bit the strap holds reads back as one. */
		control = tco_read16(TCO_CONTROL1);
		if ((control & TCO_CONTROL1_NO_REBOOT) != 0U)
			return EIO;

		/* Succeeded: the counter may reset the machine. */
		return 0;
	}

	/* The ICH9 keeps it in the general control register of the configuration space. */
	general = kern_mmio_read32(tco.general_page + (TCO_ICH9_GENERAL_CONTROL & TCO_PAGE_MASK));
	kern_mmio_write32(tco.general_page + (TCO_ICH9_GENERAL_CONTROL & TCO_PAGE_MASK),
			  general & ~TCO_ICH9_GENERAL_NO_REBOOT);

	/* Reads the bit back. */
	general = kern_mmio_read32(tco.general_page + (TCO_ICH9_GENERAL_CONTROL & TCO_PAGE_MASK));
	if ((general & TCO_ICH9_GENERAL_NO_REBOOT) != 0U)
		return EIO;

	/* Succeeded: the counter may reset the machine. */
	return 0;
}

/* Sets the bit that forbids the counter's reset again, as the firmware left it. */
static void
tco_reset_forbid(void)
{
	uint16_t control;
	uint32_t general;

	/* The hub keeps the bit in the TCO's first control register. */
	if (tco.kind == TCO_KIND_HUB) {
		control = tco_read16(TCO_CONTROL1);
		tco_control_write((uint16_t)(control | TCO_CONTROL1_NO_REBOOT));
		return;
	}

	/* The ICH9 keeps it in the general control register. */
	general = kern_mmio_read32(tco.general_page + (TCO_ICH9_GENERAL_CONTROL & TCO_PAGE_MASK));
	kern_mmio_write32(tco.general_page + (TCO_ICH9_GENERAL_CONTROL & TCO_PAGE_MASK),
			  general | TCO_ICH9_GENERAL_NO_REBOOT);
}

/*
 * Lets the ACPI timer run, which the hub's TCO counter counts with.
 *
 * The controller's switch stops the timer to save power, and an
 * operating system may set it on the way into deep idle; the counter then
 * stands still.  The watchdog needs the timer for as long as it is
 * armed, which is the kernel's life, so the switch is not set back.
 */
static void
tco_timer_run(void)
{
	volatile uint8_t *control;
	uint32_t value;

	/* Nothing to do where the switch was not found. */
	if (tco.timer_page == NULL)
		return;

	/* Clears the switch when it is set. */
	control = tco.timer_page + (TCO_PMC_TIMER_CONTROL & TCO_PAGE_MASK);
	value = kern_mmio_read32(control);
	if ((value & TCO_PMC_TIMER_DISABLE) != 0U) {
		kern_mmio_write32(control, value & ~TCO_PMC_TIMER_DISABLE);
		kern_logf("watchdog: the ACPI timer was stopped; it runs again for the TCO counter\n");
	}
}

/*
 * Loads the counter with the time and lets it run: the halt first, so
 * that no run out happens half programmed, the old timeouts cleared, the
 * ACPI timer started, and the halt lifted last.
 */
static void
tco_arm(void)
{
	uint16_t control;
	uint16_t timer;

	/* Halts the counter while it is programmed. */
	tco_halt();

	/* Sets the count each reload loads, keeping the register's other bits. */
	timer = tco_read16(TCO_TIMER);
	timer = (uint16_t)((timer & ~TCO_COUNT_MASK) | (tco.count & TCO_COUNT_MASK));
	tco_write16(TCO_TIMER, timer);

	/* Clears the timeouts of an earlier run; the status bits clear when one is written. */
	tco_write16(TCO_STATUS1, TCO_STATUS1_TIMEOUT);
	tco_write16(TCO_STATUS2, TCO_STATUS2_SECOND_TIMEOUT);

	/* Gives the hub's counter its clock. */
	if (tco.kind == TCO_KIND_HUB)
		tco_timer_run();

	/* Loads the count, then lets it run. */
	tco_write16(TCO_RELOAD, 1U);
	control = tco_read16(TCO_CONTROL1);
	tco_control_write((uint16_t)(control & ~TCO_CONTROL1_HALT));
	tco_write16(TCO_RELOAD, 1U);
}

/*
 * Watches the counter for a while and reports 0 when it went down, or
 * EIO when it stood still.
 */
static int
tco_counting(void)
{
	uint16_t before;
	uint16_t after;

	/* Reads the count, waits for more than two counts, and reads it again. */
	before = (uint16_t)(tco_read16(TCO_RELOAD) & TCO_COUNT_MASK);
	sched_sleep(sched_ticks() + kern_ms_to_ticks(TCO_COUNT_CHECK_MS));
	after = (uint16_t)(tco_read16(TCO_RELOAD) & TCO_COUNT_MASK);

	/* A count that stood still has no clock. */
	if (after >= before)
		return EIO;

	/* Succeeded: the count went down from the reload. */
	return 0;
}

/*
 * Creates one reloading thread bound to each CPU and starts them all,
 * or none: a CPU without its thread would hold every reload back.
 */
static int
tco_threads_start(void)
{
	unsigned count;
	unsigned cpu;
	unsigned made;
	int error;

	/* Counts the CPUs the reload waits for. */
	count = kern_cpu_count();
	if (count == 0U || count > KERN_CPU_MAX)
		return ENODEV;

	/* Creates and binds a thread for each CPU, before any of them runs. */
	error = 0;
	for (made = 0U; made < count; made++) {
		error = kthread_create(tco_thread_main, (void *)(uintptr_t)made, SCHED_PRIOR_HIGH, &tco_threads[made]);
		if (error != 0)
			break;

		/* Binds the thread to its CPU for good. */
		error = sched_set_cpu(tco_threads[made], (kern_cpu_id_t)made, SCHED_CPU_BOUND);
		if (error != 0) {
			(void)thread_abort_new(tco_threads[made]);
			break;
		}
	}

	/* Takes back the threads made before a failure. */
	if (error != 0) {
		/* Aborts each thread that was made and never started. */
		for (cpu = 0U; cpu < made; cpu++)
			(void)thread_abort_new(tco_threads[cpu]);
		return error;
	}

	/* The reload waits for this many stamps from now on. */
	tco.cpu_count = count;
	tco.running = 1U;

	/* Starts every thread; each releases itself if it ever ends. */
	for (cpu = 0U; cpu < count; cpu++) {
		tco_threads[cpu]->detached = 1;
		thread_start(tco_threads[cpu]);
	}

	/* Succeeded: the threads reload the counter. */
	return 0;
}

/*
 * The reloading thread of one CPU: stamps the CPU's turn, reloads the
 * counter when the stamp is the last one missing, and sleeps a quarter
 * of the timeout.  It never ends.
 */
static void
tco_thread_main(
	void *argument)
{
	unsigned cpu;
	uint64_t now;

	/* The argument is the CPU the thread is bound to. */
	cpu = (unsigned)(uintptr_t)argument;

	/* Stamps and sleeps for the kernel's life. */
	for (;;) {
		/* Stops the machine on purpose once watchdog.stop='s time has come. */
		now = sched_ticks();
		if (tco.stop_tick != 0U && now >= tco.stop_tick)
			tco_stop_machine();

		/* Stamps this CPU's turn, reloading when it completes the round. */
		tco_stamp(cpu);

		/* Sleeps until the next turn. */
		sched_sleep(now + tco.period_ticks);
	}
}

/*
 * Records that a CPU's thread ran in this generation, and reloads the
 * counter when every CPU's thread has.
 */
static void
tco_stamp(
	unsigned cpu)
{
	unsigned generation;
	unsigned expected;
	unsigned index;
	unsigned stamp;
	bool won;

	/* Stamps the CPU with the generation under way. */
	generation = __atomic_load_n(&tco_generation, __ATOMIC_ACQUIRE);
	__atomic_store_n(&tco_stamps[cpu], generation, __ATOMIC_RELEASE);

	/* Leaves the reload to a later stamp while a CPU is missing. */
	for (index = 0U; index < tco.cpu_count; index++) {
		stamp = __atomic_load_n(&tco_stamps[index], __ATOMIC_ACQUIRE);
		if (stamp != generation)
			return;
	}

	/* Moves the generation on; only the one thread that moves it reloads. */
	expected = generation;
	won = __atomic_compare_exchange_n(&tco_generation, &expected, generation + 1U, false,
					  __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
	if (!won)
		return;

	/* Reloads the counter: the time to the reset starts again. */
	tco_write16(TCO_RELOAD, 1U);
}

/*
 * Stops this CPU for good with its interrupts off, as watchdog.stop=
 * asks; when every CPU has, nothing reloads the counter and it resets
 * the machine.
 */
static void
tco_stop_machine(void)
{
	unsigned expected;
	bool first;

	/* The first CPU says why the machine stops. */
	expected = 0U;
	first = __atomic_compare_exchange_n(&tco_stop_said, &expected, 1U, false,
					    __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
	if (first)
		kern_logf("watchdog: stopping every CPU with its interrupts off, as %s asks\n", TCO_PARAMETER_STOP);

	/* Turns the interrupts off and spins for good. */
	(void)kern_irq_disable();
	for (;;)
		kern_compiler_barrier();
}
