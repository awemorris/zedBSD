/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The ACPI driver's kernel side: the operating system services the AML
 * interpreter asks for (aml-os.h), the address space handlers for system
 * memory, system I/O and PCI configuration space, the SCI interrupt and
 * the thread that handles its events, and the attachment at boot that
 * finds the firmware's tables, loads them and starts the events and the
 * Embedded Controller.
 *
 * The RSDP comes from the platform as the boot handoff "acpi.rsdp" (the
 * physical address of the RSDP the HAL validated).  A platform that does
 * not give it has no ACPI, and the driver stays off.
 */

#include <stdarg.h>

#include <hal/hal.h>
#include <kern/kcrt.h>
#include <uapi/errno.h>

#include <drivers/acpi/acpi.h>
#include <kern/system-event.h>
#include <drivers/pci/pci.h>

#include "kern/clock.h"
#include "kern/irq.h"
#include "kern/klog.h"
#include "kern/kmem.h"
#include "kern/lock.h"
#include "kern/platform.h"
#include "kern/sched.h"
#include "kern/sleep.h"
#include "kern/thread.h"
#include "kern/waitq.h"

#include "acpi-tables.h"
#include "acpi-text.h"
#include "aml-internal.h"
#include "aml-os.h"

/*
 * The size of one page of the device mappings.
 */
#define PAGE_SIZE 4096U

/*
 * The FACS Global Lock (ACPI 6.5 table 5.10): its offset, and the end of
 * the dword, which the FACS must reach.
 */
#define FACS_GLOBAL_LOCK	16U
#define FACS_LOCK_END		20U

/* The FADT's flags, and their bit for a platform that idles in S0 at low power (ws052-p006). */
#define FADT_FLAGS_OFFSET	112U
#define FADT_LOW_POWER_S0_IDLE	(1U << 21)

/*
 * How many pages of system memory the handler keeps mapped.
 */
#define MEMORY_CACHE_SLOTS 16U

/*
 * The stack the interpreter may use below its entry, in bytes.
 *
 * A kernel thread has 16 KiB (AMD64_SYS_STACK_SIZE, no guard page), and
 * what calls the interpreter and what the interpreter calls at its deepest
 * (region handlers, the log) need their share of it, so AML that nests
 * deeper fails with E2BIG instead of overflowing.  The Dell Latitude 5330's
 * tables, in the interpreter built as the kernel builds it (clang -Os with
 * link-time optimization), take 5.1 KiB to load and 7.1 KiB when every
 * method runs (BUG-195, plan/ws049/tests/check-latitude5330.sh); 10 KiB
 * leaves room for the branches the machine's own values take.
 */
#define STACK_BUDGET (10U * 1024U)

/*
 * How long a log line may be.
 */
#define LOG_LINE_MAX 256U

/*
 * One page of system memory the handler has mapped.
 */
struct memory_mapping {
	uint64_t page;
	uint8_t *virtual_address;
	uint64_t used;
};

/*
 * The lock that serializes the interpreter.
 *
 * drv_acpi_attach() initializes it before the first table loads; the
 * interpreter takes it on entry and lets it go while AML sleeps.
 */
static struct mutex interpreter_lock;

/*
 * The firmware's tables, found at attachment and kept for LoadTable.
 *
 * drv_acpi_attach() fills it once, before the interpreter first runs, and
 * nothing changes it afterwards, so its readers need no lock.
 */
static struct drv_acpi_firmware firmware;

/*
 * The pages of system memory mapped for operation regions, and the
 * counter that orders their use.  Only the handler, under the interpreter
 * lock, touches them; a slot with a NULL address is free.
 */
static struct memory_mapping memory_cache[MEMORY_CACHE_SLOTS];
static uint64_t memory_cache_clock;

/*
 * The lock the SCI interrupt and the event code share, the queue the
 * event thread sleeps on, and the number of SCIs it has not handled yet.
 * event_work is read and written only under event_lock: the interrupt
 * adds to it, and the thread takes it as a whole; zero means the thread
 * has nothing to do and sleeps.
 */
static struct spinlock event_lock;
static struct wait_queue event_queue;
static unsigned event_work;

/*
 * The PCI function whose absence was logged last, as segment, bus, device
 * and function packed one above the other, plus one so that zero means none
 * yet.  Only the PCI_Config handler, under the interpreter lock, touches it;
 * it keeps a region read in a loop from logging once per access.
 */
static uint64_t pci_absent_logged;

static int start_events(void);
static int start_global_lock(void);
static void start_ecdt(void);
static void sci_interrupt(int irq, kern_irq_ack_t acknowledge, void *argument);
static void event_thread(void *argument);
static void power_button(enum drv_acpi_fixed_event event, void *argument);
static int read_physical(uint64_t address, void *buffer, size_t length, void *argument);
static int memory_handler(const struct drv_acpi_region_access *access, uint64_t *value, void *argument);
static int memory_bytes(const struct drv_acpi_region_access *access, uint64_t *value);
static void memory_move(uint8_t *virtual_address, unsigned width, bool write, uint64_t *value);
static int memory_page(uint64_t page, uint8_t **virtual_address);
static void start_fixed_hardware(void);
static int io_handler(const struct drv_acpi_region_access *access, uint64_t *value, void *argument);
static int pci_handler(const struct drv_acpi_region_access *access, uint64_t *value, void *argument);
static void pci_absent(const struct drv_acpi_region_access *access, uint64_t *value);
static int pci_read(struct drv_pci_device *device, unsigned offset, unsigned width, uint64_t *value);
static int pci_write(struct drv_pci_device *device, unsigned offset, unsigned width, uint64_t value);

/*
 * Finds the firmware's ACPI tables, loads them, and initializes the
 * namespace and the devices.
 */
int
drv_acpi_attach(void)
{
	uint64_t *rsdp;
	int error;

	/* Stays off on a platform that gives no RSDP. */
	rsdp = kern_boot_handoff("acpi.rsdp");
	if (rsdp == NULL) {
		kern_logf("acpi: the platform gives no RSDP; ACPI is off\n");
		return ENODEV;
	}

	/* Prepares the lock before the interpreter first runs. */
	error = mutex_init(&interpreter_lock, LOCK_RANK_DEVICE, "acpi interpreter");
	if (error != 0)
		return error;

	/* Finds the tables from the RSDP. */
	error = drv_acpi_firmware_discover(*rsdp, read_physical, NULL, &firmware);
	if (error != 0) {
		kern_logf("acpi: no usable tables at RSDP 0x%llx (error %d)\n", (unsigned long long)*rsdp, error);
		return error;
	}

	/* Installs the handler of system memory, which the kernel maps directly. */
	error = drv_acpi_region_install(DRV_ACPI_SPACE_SYSTEM_MEMORY, memory_handler, NULL);
	if (error != 0)
		return error;

	/* Installs the handler of system I/O ports. */
	error = drv_acpi_region_install(DRV_ACPI_SPACE_SYSTEM_IO, io_handler, NULL);
	if (error != 0)
		return error;

	/* Installs the handler of PCI configuration space, through the PCI driver. */
	error = drv_acpi_region_install(DRV_ACPI_SPACE_PCI_CONFIG, pci_handler, NULL);
	if (error != 0)
		return error;

	/* Loads the DSDT and the SSDTs. */
	error = drv_acpi_firmware_load(&firmware);
	if (error != 0) {
		kern_logf("acpi: the DSDT did not load (error %d)\n", error);
		start_fixed_hardware();
		return error;
	}

	/* Prepares the objects that need it before any method runs; the rest of the namespace stays usable. */
	error = drv_acpi_initialize_objects();
	if (error != 0)
		kern_logf("acpi: object preparation failed (error %d)\n", error);

	/* Starts the ECDT's EC, so that its space is there when _REG and _INI run. */
	start_ecdt();

	/* Tells firmware with _REG that the spaces are there. */
	error = drv_acpi_region_connect_all();
	if (error != 0)
		kern_logf("acpi: _REG failed (error %d)\n", error);

	/* Runs _STA and _INI on the devices. */
	drv_acpi_initialize_devices();

	/* Starts the SCI and its thread; without them there are no events. */
	error = start_events();
	if (error != 0) {
		kern_logf("acpi: no ACPI events (error %d)\n", error);
	} else {
		/* Shares the FACS Global Lock with firmware, which needs the SCI's Global Lock event. */
		error = start_global_lock();
		if (error != 0)
			kern_logf("acpi: the FACS Global Lock is not shared (error %d)\n", error);
	}

	/* Attaches the Embedded Controller. */
	error = drv_acpi_ec_attach();
	if (error != 0 && error != ENODEV)
		kern_logf("acpi: the Embedded Controller did not attach (error %d)\n", error);

	/* Follows the lid, the AC adapter, the batteries and the buttons (ws132-p002). */
	error = drv_acpi_power_attach();
	if (error != 0 && error != ENODEV)
		kern_logf("acpi: the power devices did not attach (error %d)\n", error);

	/* Reads the temperature sensors for hw.thermal (ws134-p009); a machine without one has none listed. */
	error = drv_acpi_thermal_attach();
	if (error != 0 && error != ENODEV)
		kern_logf("acpi: the temperature sensors did not attach (error %d)\n", error);

	/* Finds the LPS0 device of S0 idle and its _DSM functions (ws052-p003); a platform without one only lacks S0 idle. */
	error = drv_acpi_lps0_attach();
	if (error != 0 && error != ENODEV)
		kern_logf("acpi: the LPS0 device did not attach (error %d)\n", error);

	/* Makes the namespace the platform side of the PCI functions' power (ws052-p004). */
	(void)drv_acpi_pci_power_attach();

	/* Publishes the namespace to user programs. */
	error = drv_acpi_device_register();
	if (error != 0)
		kern_logf("acpi: /dev/acpi was not published (error %d)\n", error);

	/* Succeeded: the namespace is loaded and drivers may evaluate it. */
	kern_logf("acpi: %u tables listed, namespace ready\n", firmware.count);
	return 0;
}

/*
 * Allocates interpreter memory from the kernel heap.
 */
void *
drv_acpi_os_alloc(
	size_t size)
{
	void *pointer;

	/* Takes the memory from the kernel heap. */
	pointer = kern_malloc(size);
	if (pointer == NULL)
		return NULL;

	/* Succeeded: the interpreter owns size bytes until it frees them. */
	return pointer;
}

/*
 * Frees interpreter memory.
 */
void
drv_acpi_os_free(
	void *pointer)
{
	/* Freeing nothing is allowed. */
	if (pointer == NULL)
		return;

	/* Gives the memory back to the kernel heap. */
	kern_free(pointer);
}

/*
 * Writes a line of the interpreter's log to the kernel log.
 */
void
drv_acpi_os_log(
	const char *format,
	...)
{
	char line[LOG_LINE_MAX];
	va_list arguments;

	/* Formats the line. */
	va_start(arguments, format);
	kern_vsnprintf(line, sizeof(line), format, arguments);
	va_end(arguments);

	/* Writes the formatted line to the kernel log. */
	kern_logf("%s", line);
}

/*
 * Reports the stack budget of the interpreter.
 */
size_t
drv_acpi_os_stack_budget(void)
{
	/* Reports the bytes of stack the interpreter may use below its entry. */
	return STACK_BUDGET;
}

/*
 * Sleeps for a number of milliseconds; the interpreter has let its lock go.
 */
void
drv_acpi_os_sleep(
	uint64_t milliseconds)
{
	uint64_t ticks;
	uint64_t deadline;

	/* Finds the tick the time ends at. */
	ticks = kern_ms_to_ticks(milliseconds);
	deadline = sched_ticks() + ticks;

	/* Sleeps until that tick. */
	sched_sleep(deadline);
}

/*
 * Waits for a number of microseconds without sleeping.
 *
 * ACPI's Stall keeps the processor: firmware stalls are short and are
 * taken with the interpreter lock (and the EC's Global Lock) held, a few
 * microseconds for each byte the embedded controller moves.  It spins on
 * the monotonic counter; kern_usleep_range() would sleep to the next tick
 * each time (BUG-202).
 */
void
drv_acpi_os_stall(
	uint64_t microseconds)
{
	uint64_t start;
	uint64_t now;
	uint64_t frequency;
	uint64_t rate;
	uint64_t span;
	bool available;

	/* A stall is 100 microseconds at most by the specification; a longer request is held to a second. */
	if (microseconds > 1000000ULL)
		microseconds = 1000000ULL;

	/* The counter and its rate; without them there is nothing to wait by. */
	available = kern_rtc_read_counter(&start, &frequency);
	if (!available || frequency == 0)
		return;

	/* The counts the stall lasts, rounded up. */
	span = (microseconds * frequency + 999999ULL) / 1000000ULL;

	/* Spins until they have passed; a counter that fails or changes rate ends the stall. */
	for (;;) {
		available = kern_rtc_read_counter(&now, &rate);
		if (!available || rate != frequency)
			return;
		if (now - start >= span)
			return;
	}
}

/*
 * Reads the monotonic clock in the 100-nanosecond units of the Timer
 * operator.
 */
uint64_t
drv_acpi_os_timer(void)
{
	uint64_t counter;
	uint64_t frequency;
	uint64_t units;
	bool available;

	/* Reads the counter; without one the clock stands still. */
	available = kern_rtc_read_counter(&counter, &frequency);
	if (!available || frequency == 0)
		return 0;

	/* Converts whole seconds and the rest separately so that nothing overflows. */
	units = (counter / frequency) * 10000000ULL;
	units += ((counter % frequency) * 10000000ULL) / frequency;

	/* Succeeded: reports the time in 100-nanosecond units. */
	return units;
}

/*
 * Takes the interpreter lock.
 */
void
drv_acpi_os_lock(void)
{
	/* Takes the interpreter lock, sleeping while another thread runs AML. */
	mutex_lock(&interpreter_lock);
}

/*
 * Lets the interpreter lock go.
 */
void
drv_acpi_os_unlock(void)
{
	/* Lets another thread run AML. */
	mutex_unlock(&interpreter_lock);
}

/*
 * Reports whether the calling thread holds the interpreter lock.
 */
bool
drv_acpi_os_lock_owned(void)
{
	int owned;

	/* Asks the mutex whether this thread holds it. */
	owned = mutex_owned(&interpreter_lock);
	if (owned)
		return true;

	/* Reports that another thread, or none, holds it. */
	return false;
}

/*
 * Reads an I/O port for the event and EC code.
 */
int
drv_acpi_os_port_read(
	uint32_t port,
	unsigned width,
	uint32_t *value)
{
	/* Refuses a port beyond the 64 KiB space. */
	if (port > 0xffffU)
		return EFAULT;

	/* Reads at the width. */
	switch (width) {
	case 8:
		*value = hal_io_inp8((uint16_t)port);
		break;
	case 16:
		*value = hal_io_inp16((uint16_t)port);
		break;
	case 32:
		*value = hal_io_inp32((uint16_t)port);
		break;
	default:
		/* Refuses another width. */
		return EINVAL;
	}

	/* Succeeded: value holds what the port gave. */
	return 0;
}

/*
 * Writes an I/O port for the event and EC code.
 */
int
drv_acpi_os_port_write(
	uint32_t port,
	unsigned width,
	uint32_t value)
{
	/* Refuses a port beyond the 64 KiB space. */
	if (port > 0xffffU)
		return EFAULT;

	/* Writes at the width. */
	switch (width) {
	case 8:
		hal_io_outp8((uint16_t)port, (uint8_t)value);
		break;
	case 16:
		hal_io_outp16((uint16_t)port, (uint16_t)value);
		break;
	case 32:
		hal_io_outp32((uint16_t)port, value);
		break;
	default:
		/* Refuses another width. */
		return EINVAL;
	}

	/* Succeeded: the port has the value. */
	return 0;
}

/*
 * Takes the lock the SCI interrupt shares with the event code.
 */
unsigned long
drv_acpi_os_event_lock(void)
{
	unsigned long state;

	/* Takes it with interrupts off, as the interrupt takes it too. */
	state = spin_lock_irqsave(&event_lock);

	/* Reports the interrupt state to restore. */
	return state;
}

/*
 * Lets the event lock go.
 */
void
drv_acpi_os_event_unlock(
	unsigned long state)
{
	/* Lets the event lock go and restores the interrupt state. */
	spin_unlock_irqrestore(&event_lock, state);
}

/*
 * Finds a table LoadTable asks for among the tables the firmware lists.
 */
int
drv_acpi_os_table(
	const char *signature,
	const char *oem_id,
	const char *oem_table_id,
	const uint8_t **data,
	size_t *length)
{
	int error;

	/* Looks it up in the root table's list. */
	error = drv_acpi_firmware_find(&firmware, signature, oem_id, oem_table_id, data, length);
	if (error != 0)
		return error;

	/* Succeeded: data and length name the table's bytes. */
	return 0;
}

/*
 * Puts the machine in ACPI mode when the namespace did not load, so that
 * its fixed hardware still works (BUG-196): without ACPI_ENABLE written to
 * SMI_CMD, SCI_EN stays clear, and a press of the power button turns an
 * Intel PCH off at once instead of reaching the operating system as the
 * power button's fixed event.  The events need no AML; the GPEs whose
 * methods did load run as usual.
 */
static void
start_fixed_hardware(void)
{
	int error;

	/* Enables ACPI mode, takes the SCI and posts the buttons as the system's events. */
	error = start_events();
	if (error != 0) {
		kern_logf("acpi: no ACPI events either (error %d)\n", error);
		return;
	}

	/* Succeeded: the buttons reach the system although the namespace did not load. */
	kern_logf("acpi: fixed hardware events on, without the namespace\n");
}

/* Reads the event hardware and starts the event thread and the SCI. */
static int
start_events(void)
{
	struct thread *thread;
	unsigned irq;
	int error;

	/* The event lock and queue exist before anything takes them. */
	spin_init(&event_lock, LOCK_RANK_DEVICE, "acpi event");
	waitq_init(&event_queue, "acpi event");

	/* Reads the hardware and enables the runtime GPEs. */
	error = drv_acpi_events_init(firmware.fadt, firmware.fadt_length);
	if (error != 0)
		return error;

	/* Creates the thread that handles the events. */
	error = kthread_create(event_thread, NULL, SCHED_PRIORITY_DEFAULT, &thread);
	if (error != 0)
		return error;

	/* Lets the scheduler run it; a created thread stays new until it is started. */
	thread_start(thread);

	/* Takes the SCI. */
	irq = drv_acpi_sci_irq();
	error = kern_irq_register((int)irq, sci_interrupt, NULL);
	if (error != 0)
		return error;

	/* Posts the power and sleep buttons as the system's events. */
	error = drv_acpi_fixed_event_install(DRV_ACPI_EVENT_POWER_BUTTON, power_button, NULL);
	if (error != 0 && error != ENODEV)
		return error;
	error = drv_acpi_fixed_event_install(DRV_ACPI_EVENT_SLEEP_BUTTON, power_button, NULL);
	if (error != 0 && error != ENODEV)
		return error;

	/*
	 * The SCI wakes the system from S0 idle (ws052-p006): every wake source
	 * of the platform comes through it, and its handler only records and
	 * masks, so it may run while the devices are suspended.
	 */
	error = kern_irq_set_wake((int)irq, 1);
	if (error != 0)
		kern_logf("acpi: the SCI cannot wake the system (error %d)\n", error);

	/*
	 * Lets the SCI line through.  A registered line stays masked until its
	 * owner unmasks it, so without this no SCI is ever delivered.
	 */
	kern_irq_unmask((int)irq);

	/* Succeeded: the SCI is delivered to sci_interrupt() from now on. */
	kern_logf("acpi: SCI on IRQ %u\n", irq);
	return 0;
}

/*
 * Starts the EC of the ECDT, when firmware lists one, so that its _REG
 * runs with the other spaces and _INI can reach it.
 */
static void
start_ecdt(void)
{
	const uint8_t *ecdt;
	size_t length;
	int error;

	/* Most firmware lists none. */
	error = drv_acpi_firmware_find(&firmware, "ECDT", "", "", &ecdt, &length);
	if (error != 0)
		return;

	/* Starts the EC; drv_acpi_ec_attach() adds its GPE later. */
	error = drv_acpi_ec_ecdt(ecdt, length);
	if (error != 0)
		kern_logf("acpi: the ECDT's EC did not start (error %d)\n", error);
}

/*
 * Maps the FACS and shares its Global Lock with firmware from now on.
 * The amd64 device views are uncached; the locked exchange of the lock
 * works on such a mapping too.
 */
static int
start_global_lock(void)
{
	volatile uint8_t *facs;
	uint64_t page;
	uint64_t offset;
	size_t size;
	void *mapping;
	uint32_t length;
	int error;

	/* A FADT without a FACS has no Global Lock. */
	if (firmware.facs_address == 0)
		return ENODEV;

	/* Maps the pages the FACS header and lock lie in, for good. */
	page = firmware.facs_address & ~(uint64_t)(PAGE_SIZE - 1U);
	offset = firmware.facs_address - page;
	size = (size_t)((offset + FACS_LOCK_END + PAGE_SIZE - 1U) & ~(uint64_t)(PAGE_SIZE - 1U));
	error = hal_space_map_device((hal_physaddr_t)page, size, HAL_SPACE_READ | HAL_SPACE_WRITE, &mapping);
	if (error != HAL_OK)
		return EFAULT;

	/* Finds the FACS inside the mapping. */
	facs = (volatile uint8_t *)mapping + offset;

	/* Reads the table's length, little-endian. */
	length = (uint32_t)facs[4];
	length |= (uint32_t)facs[5] << 8;
	length |= (uint32_t)facs[6] << 16;
	length |= (uint32_t)facs[7] << 24;

	/* Refuses a table that is not a FACS, or one too short to hold the lock. */
	if (facs[0] != 'F' ||
	    facs[1] != 'A' ||
	    facs[2] != 'C' ||
	    facs[3] != 'S' ||
	    length < FACS_LOCK_END) {
		(void)hal_space_unmap_device(mapping, size);
		return EINVAL;
	}

	/* Hands the lock's dword to the interpreter. */
	error = drv_acpi_global_lock_attach((volatile uint32_t *)(facs + FACS_GLOBAL_LOCK));
	if (error != 0) {
		(void)hal_space_unmap_device(mapping, size);
		return error;
	}

	/* Succeeded: Acquire on a Global Lock mutex now takes the lock from firmware too. */
	return 0;
}

/* The SCI: masks and records the events that fired, then wakes the thread. */
static void
sci_interrupt(
	int irq,
	kern_irq_ack_t acknowledge,
	void *argument)
{
	unsigned long state;
	bool pending;

	UNUSED_PARAMETER(irq);
	UNUSED_PARAMETER(argument);

	/* Masks what fired; the level SCI goes quiet with it. */
	pending = drv_acpi_sci_interrupt();

	/*
	 * Hands the work to the thread: a nonzero event_work tells it that
	 * events wait, and the wake ends its sleep.
	 */
	if (pending) {
		state = spin_lock_irqsave(&event_lock);

		event_work++;
		waitq_wake_all(&event_queue);

		spin_unlock_irqrestore(&event_lock, state);
	}

	/* Ends the interrupt; the level SCI does not fire again, as its events are masked. */
	kern_irq_send_eoi(acknowledge);
}

/* Handles the events the SCI recorded, for the life of the system. */
static void
event_thread(
	void *argument)
{
	unsigned long state;
	uint64_t sequence;
	bool first;

	UNUSED_PARAMETER(argument);

	/* The first SCI is logged once, so that a boot log shows SCIs arrive. */
	first = true;

	/* Sleeps until the SCI records work, then handles it. */
	for (;;) {
		/* Waits for work and takes it. */
		state = spin_lock_irqsave(&event_lock);

		while (event_work == 0) {
			sequence = waitq_sequence(&event_queue);
			(void)waitq_sleep(&event_queue, &event_lock, sequence, 0, 0);
		}

		/* Takes all the work counted so far as a whole; zero tells the next SCI to wake the thread again. */
		event_work = 0;

		spin_unlock_irqrestore(&event_lock, state);

		/* Logs the first SCI the thread handles. */
		if (first) {
			kern_logf("acpi: first SCI handled\n");
			first = false;
		}

		/* Runs the handlers and the AML. */
		drv_acpi_events_process();
	}
}

/*
 * Takes a press of the fixed power or sleep button: logged, and posted as
 * the system's event for the desktop (ws132-p002).
 */
static void
power_button(
	enum drv_acpi_fixed_event event,
	void *argument)
{
	UNUSED_PARAMETER(argument);

	/* The sleep button. */
	if (event == DRV_ACPI_EVENT_SLEEP_BUTTON) {
		kern_logf("acpi: sleep button\n");
		kern_system_event_post(KERN_SYSTEM_EVENT_POWER, KERN_SYSTEM_EVENT_PRESS, 1, "sleep-button", "");
		return;
	}

	/* The power button, which also wakes the system from S0 idle. */
	kern_logf("acpi: power button\n");
	kern_sleep_note_wake(KERN_SLEEP_WAKE_POWER_BUTTON);
	kern_system_event_post(KERN_SYSTEM_EVENT_POWER, KERN_SYSTEM_EVENT_PRESS, 1, "power-button", "");
}

/*
 * Reports whether the firmware says the platform idles in S0 at low
 * power (the FADT's LOW_POWER_S0_IDLE_CAPABLE, ws052-p006).  Returns 1 or
 * 0.
 */
int
drv_acpi_s0_idle_capable(void)
{
	const uint8_t *fadt;
	uint32_t flags;

	/* A FADT long enough to have the flags. */
	fadt = firmware.fadt;
	if (fadt == NULL || firmware.fadt_length < FADT_FLAGS_OFFSET + 4U)
		return 0;

	/* The flags, little-endian. */
	flags = (uint32_t)fadt[FADT_FLAGS_OFFSET];
	flags |= (uint32_t)fadt[FADT_FLAGS_OFFSET + 1U] << 8;
	flags |= (uint32_t)fadt[FADT_FLAGS_OFFSET + 2U] << 16;
	flags |= (uint32_t)fadt[FADT_FLAGS_OFFSET + 3U] << 24;

	/* The low-power S0 idle bit. */
	if ((flags & FADT_LOW_POWER_S0_IDLE) == 0U)
		return 0;

	/* Succeeded: the platform declares S0 idle. */
	return 1;
}

/* Reads physical memory for the table finder, one page at a time. */
static int
read_physical(
	uint64_t address,
	void *buffer,
	size_t length,
	void *argument)
{
	uint8_t *destination;
	void *mapping;
	uint64_t page;
	size_t offset;
	size_t part;
	int error;

	UNUSED_PARAMETER(argument);

	/* Copies each page's part of the range. */
	destination = buffer;
	while (length != 0) {
		/* Finds the page the next byte lies in. */
		page = address & ~(uint64_t)(PAGE_SIZE - 1U);
		offset = (size_t)(address - page);

		/* Takes the rest of the page, or less when the range ends first. */
		part = PAGE_SIZE - offset;
		if (part > length)
			part = length;

		/* Maps the page for reading. */
		error = hal_space_map_device((hal_physaddr_t)page, PAGE_SIZE, HAL_SPACE_READ, &mapping);
		if (error != HAL_OK)
			return EFAULT;

		/* Copies the part. */
		kern_memcpy(destination, (const uint8_t *)mapping + offset, part);

		/* Lets the mapping go; a mapping that will not go only wastes address space. */
		(void)hal_space_unmap_device(mapping, PAGE_SIZE);

		/* Moves on past the part. */
		destination += part;
		address += part;
		length -= part;
	}

	/* Succeeded. */
	return 0;
}

/* Reads or writes system memory for an operation region. */
static int
memory_handler(
	const struct drv_acpi_region_access *access,
	uint64_t *value,
	void *argument)
{
	uint8_t *virtual_address;
	uint64_t page;
	uint64_t last_page;
	unsigned bytes;
	int error;

	UNUSED_PARAMETER(argument);

	/* Finds the pages the first and the last byte lie in. */
	bytes = access->width / 8U;
	page = access->address & ~(uint64_t)(PAGE_SIZE - 1U);
	last_page = (access->address + bytes - 1U) & ~(uint64_t)(PAGE_SIZE - 1U);

	/* An access that crosses a page is made one byte at a time. */
	if (last_page != page) {
		error = memory_bytes(access, value);
		if (error != 0)
			return error;

		/* Succeeded: every byte of the access moved. */
		return 0;
	}

	/* Maps the page. */
	error = memory_page(page, &virtual_address);
	if (error != 0)
		return error;

	/* Finds the access inside the page. */
	virtual_address += access->address - page;

	/* Makes the access at its width. */
	memory_move(virtual_address, access->width, access->write, value);

	/* Succeeded. */
	return 0;
}

/* Moves an access that crosses a page one byte at a time. */
static int
memory_bytes(
	const struct drv_acpi_region_access *access,
	uint64_t *value)
{
	uint8_t *virtual_address;
	uint64_t address;
	uint64_t byte;
	unsigned bytes;
	unsigned index;
	int error;

	/* A read starts from zero. */
	bytes = access->width / 8U;
	if (!access->write)
		*value = 0;

	/* Moves each byte through its own page. */
	for (index = 0; index < bytes; index++) {
		/* Maps the byte's page. */
		address = access->address + index;
		error = memory_page(address & ~(uint64_t)(PAGE_SIZE - 1U), &virtual_address);
		if (error != 0)
			return error;

		/* Finds the byte inside the page. */
		virtual_address += address % PAGE_SIZE;

		/* Moves the byte; a read puts it in its place in the value. */
		byte = (*value >> (index * 8U)) & 0xffU;
		memory_move(virtual_address, 8, access->write, &byte);
		if (!access->write)
			*value |= byte << (index * 8U);
	}

	/* Succeeded: every byte moved. */
	return 0;
}

/* Reads or writes mapped memory at one width. */
static void
memory_move(
	uint8_t *virtual_address,
	unsigned width,
	bool write,
	uint64_t *value)
{
	/* Chooses the accessor by the width. */
	switch (width) {
	case 8:
		/* Writes or reads one byte. */
		if (write) {
			hal_mmio_write8(virtual_address, (uint8_t)*value);
		} else {
			*value = hal_mmio_read8(virtual_address);
		}

		break;
	case 16:
		/* Writes or reads one word. */
		if (write) {
			hal_mmio_write16(virtual_address, (uint16_t)*value);
		} else {
			*value = hal_mmio_read16(virtual_address);
		}

		break;
	case 32:
		/* Writes or reads one double word. */
		if (write) {
			hal_mmio_write32(virtual_address, (uint32_t)*value);
		} else {
			*value = hal_mmio_read32(virtual_address);
		}

		break;
	default:
		/* Writes or reads one quad word, the only width left. */
		if (write) {
			hal_mmio_write64(virtual_address, *value);
		} else {
			*value = hal_mmio_read64(virtual_address);
		}

		break;
	}
}

/* Finds a mapping of a page of system memory, mapping it in place of the oldest. */
static int
memory_page(
	uint64_t page,
	uint8_t **virtual_address)
{
	struct memory_mapping *slot;
	struct memory_mapping *oldest;
	void *mapping;
	unsigned index;
	int error;

	/*
	 * Moves the clock, which stamps each use so that the slot used least
	 * recently is the one a new page replaces.
	 */
	memory_cache_clock++;

	/* Uses a mapping of the page when there is one. */
	oldest = &memory_cache[0];
	for (index = 0; index < MEMORY_CACHE_SLOTS; index++) {
		/* Reports the cached page, stamped as used now. */
		slot = &memory_cache[index];
		if (slot->virtual_address != NULL && slot->page == page) {
			slot->used = memory_cache_clock;
			*virtual_address = slot->virtual_address;
			return 0;
		}

		/* Remembers the least recently used slot, a free one first. */
		if (slot->virtual_address == NULL || slot->used < oldest->used)
			oldest = slot;
	}

	/* Maps the page uncached, as firmware memory and devices want it. */
	error = hal_space_map_device((hal_physaddr_t)page, PAGE_SIZE, HAL_SPACE_READ | HAL_SPACE_WRITE | HAL_SPACE_NOCACHE, &mapping);
	if (error != HAL_OK)
		return EFAULT;

	/* Lets the oldest mapping go; a mapping that will not go only wastes address space. */
	if (oldest->virtual_address != NULL)
		(void)hal_space_unmap_device(oldest->virtual_address, PAGE_SIZE);

	/* Keeps the new mapping in the slot, stamped as used now. */
	oldest->page = page;
	oldest->virtual_address = mapping;
	oldest->used = memory_cache_clock;

	/* Succeeded: the caller reaches the page through the mapping. */
	*virtual_address = mapping;
	return 0;
}

/* Reads or writes system I/O ports for an operation region. */
static int
io_handler(
	const struct drv_acpi_region_access *access,
	uint64_t *value,
	void *argument)
{
	uint64_t last;
	uint16_t port;

	UNUSED_PARAMETER(argument);

	/* Refuses an access whose last byte lies beyond the 64 KiB space, so that no port wraps around. */
	last = access->address + access->width / 8U - 1U;
	if (last > 0xffffU)
		return EFAULT;

	/* Takes the first port. */
	port = (uint16_t)access->address;

	/* Makes the access at its width; 64 bits are two double words. */
	switch (access->width) {
	case 8:
		/* Writes or reads one byte. */
		if (access->write) {
			hal_io_outp8(port, (uint8_t)*value);
		} else {
			*value = hal_io_inp8(port);
		}

		break;
	case 16:
		/* Writes or reads one word. */
		if (access->write) {
			hal_io_outp16(port, (uint16_t)*value);
		} else {
			*value = hal_io_inp16(port);
		}

		break;
	case 32:
		/* Writes or reads one double word. */
		if (access->write) {
			hal_io_outp32(port, (uint32_t)*value);
		} else {
			*value = hal_io_inp32(port);
		}

		break;
	default:
		/* Writes or reads two double words, the lower first. */
		if (access->write) {
			hal_io_outp32(port, (uint32_t)*value);
			hal_io_outp32((uint16_t)(port + 4U), (uint32_t)(*value >> 32));
		} else {
			*value = hal_io_inp32(port);
			*value |= (uint64_t)hal_io_inp32((uint16_t)(port + 4U)) << 32;
		}

		break;
	}

	/* Succeeded: a read leaves the ports' value in value. */
	return 0;
}

/* Reads or writes PCI configuration space for an operation region. */
static int
pci_handler(
	const struct drv_acpi_region_access *access,
	uint64_t *value,
	void *argument)
{
	struct drv_pci_address address;
	struct drv_pci_device *device;
	int error;

	UNUSED_PARAMETER(argument);

	/*
	 * Finds the function the region belongs to among the enumerated ones,
	 * on any bus.  The address starts zeroed, so that no byte of it, the
	 * padding included, is left undefined.
	 */
	kern_memset(&address, 0, sizeof(address));
	address.segment = access->pci_segment;
	address.bus = access->pci_bus;
	address.device = access->pci_device;
	address.function = access->pci_function;
	device = drv_pci_find_device(&address);
	if (device == NULL) {
		pci_absent(access, value);
		return 0;
	}

	/* Refuses an offset outside the extended configuration space. */
	if (access->address > 0xfffU)
		return EFAULT;

	/* Makes the access. */
	if (access->write) {
		error = pci_write(device, (unsigned)access->address, access->width, *value);
	} else {
		error = pci_read(device, (unsigned)access->address, access->width, value);
	}

	/* Reports a failed access. */
	if (error != 0)
		return error;

	/* Succeeded: a read leaves the configuration space's value in value. */
	return 0;
}

/*
 * Answers an access to the configuration space of a function that is not
 * there, as the PCI bus does: a read finds every bit set and a write is
 * dropped.  Firmware tests for exactly that (a vendor ID of 0xFFFF) to skip
 * a device the BIOS disabled, so failing the access would stop the table.
 */
static void
pci_absent(
	const struct drv_acpi_region_access *access,
	uint64_t *value)
{
	uint64_t function;

	/* Packs the function's address, plus one so that zero stays "none". */
	function = (uint64_t)access->pci_segment << 16;
	function |= (uint64_t)access->pci_bus << 8;
	function |= (uint64_t)access->pci_device << 3;
	function |= access->pci_function;
	function++;

	/* Logs a function once, not once per access of a region read in a loop. */
	if (function != pci_absent_logged) {
		kern_logf("acpi: PCI_Config region of absent function %x:%x.%x reads as all ones\n",
			  access->pci_bus,
			  access->pci_device,
			  access->pci_function);
		pci_absent_logged = function;
	}

	/* A write goes nowhere. */
	if (access->write)
		return;

	/* A read finds every bit of its width set; 64 bits are all of them. */
	if (access->width >= 64U) {
		*value = UINT64_MAX;
	} else {
		*value = ((uint64_t)1 << access->width) - 1U;
	}
}

/* Reads configuration space at one width; 64 bits are two double words. */
static int
pci_read(
	struct drv_pci_device *device,
	unsigned offset,
	unsigned width,
	uint64_t *value)
{
	uint32_t low;
	uint32_t high;
	uint16_t word;
	uint8_t byte;
	int error;

	/* Chooses the access by the width. */
	switch (width) {
	case 8:
		error = drv_pci_device_config_read8(device, offset, &byte);
		*value = byte;
		break;
	case 16:
		error = drv_pci_device_config_read16(device, offset, &word);
		*value = word;
		break;
	case 32:
		error = drv_pci_device_config_read32(device, offset, &low);
		*value = low;
		break;
	default:
		error = drv_pci_device_config_read32(device, offset, &low);
		if (error != 0)
			break;
		error = drv_pci_device_config_read32(device, offset + 4U, &high);
		*value = (uint64_t)low | (uint64_t)high << 32;
		break;
	}

	/* Reports a failed read. */
	if (error != 0)
		return EIO;

	/* Succeeded: value holds the configuration space's bits. */
	return 0;
}

/* Writes configuration space at one width; 64 bits are two double words. */
static int
pci_write(
	struct drv_pci_device *device,
	unsigned offset,
	unsigned width,
	uint64_t value)
{
	int error;

	/* Chooses the access by the width. */
	switch (width) {
	case 8:
		error = drv_pci_device_config_write8(device, offset, (uint8_t)value);
		break;
	case 16:
		error = drv_pci_device_config_write16(device, offset, (uint16_t)value);
		break;
	case 32:
		error = drv_pci_device_config_write32(device, offset, (uint32_t)value);
		break;
	default:
		error = drv_pci_device_config_write32(device, offset, (uint32_t)value);
		if (error != 0)
			break;
		error = drv_pci_device_config_write32(device, offset + 4U, (uint32_t)(value >> 32));
		break;
	}

	/* Reports a failed write. */
	if (error != 0)
		return EIO;

	/* Succeeded: the configuration space has the value. */
	return 0;
}
