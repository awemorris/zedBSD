/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * What the Type-C layer, the UCSI core and its ACPI transport need from the
 * kernel (typec-os.h, ws050-p003): the records' lock, the log, the mapping
 * of the mailbox, the signal of the ACPI notification, the driver's thread
 * and the diagnostic /dev/typec.
 */

#include <stdarg.h>
#include <stdbool.h>

#include <hal/hal.h>
#include <kern/kcrt.h>
#include <uapi/errno.h>

#include <drivers/typec/typec.h>

#include "kern/cdev.h"
#include "kern/clock.h"
#include "kern/file.h"
#include "kern/klog.h"
#include "kern/kmem.h"
#include "kern/lock.h"
#include "kern/page.h"
#include "kern/sched.h"
#include "kern/thread.h"
#include "kern/waitq.h"

#include "typec-os.h"

/* The longest log line written (a longer one is cut short). */
#define TYPEC_LOG_LINE_MAX	256U

/* The device number of /dev/typec (the next free one after the smart cards' range). */
#define TYPEC_DEVICE_NUMBER	0x00110000U

/* The room of one open's text: a line of a few hundred bytes for each connector. */
#define TYPEC_TEXT_MAX		8192U

/*
 * What one open of /dev/typec reads: the text taken at the open, its
 * length, and how far it has been read.  The lock keeps two reads of the
 * same file from moving the position at once.
 */
struct typec_open {
	struct mutex lock;
	char text[TYPEC_TEXT_MAX];
	size_t length;
	size_t position;
};

/*
 * The states of the lock's making: not made, being made by one thread,
 * made.
 */
#define TYPEC_LOCK_UNMADE	0
#define TYPEC_LOCK_MAKING	1
#define TYPEC_LOCK_MADE		2

/*
 * The lock of the connector records.
 *
 * Made on first use, by whichever thread uses the layer first (the UCSI
 * driver's attach, a driver registering a listener, or the display
 * driver's report, ws177-p003): typec_lock_state goes from UNMADE to
 * MAKING for exactly one thread, which makes the lock and sets MADE; any
 * other thread that comes meanwhile waits for MADE.
 */
static struct mutex typec_lock;
static int typec_lock_state;

/*
 * The signal of the ACPI notification: typec_signalled is raised by the
 * notification handler and lowered by the thread that waited, both under
 * typec_signal_lock (interrupts off), and typec_signal_waitq is where the
 * thread sleeps.  Prepared by drv_typec_os_signal_init() before the
 * handler is installed; they live as long as the kernel.
 */
static struct spinlock typec_signal_lock;
static struct wait_queue typec_signal_waitq;
static bool typec_signalled;

static int typec_open(struct file *file);
static int typec_close(struct file *file);
static ssize_t typec_read(struct file *file, void *buffer, size_t size);

/*
 * The operations of /dev/typec.
 */
static const struct cdev_ops typec_ops = {
	.open = typec_open,
	.close = typec_close,
	.read = typec_read,
};

/*
 * Takes the lock of the connector records.
 */
void
drv_typec_os_lock(void)
{
	int expected;
	int state;
	bool claimed;
	int error;

	/* The lock, made by the first thread that claims its making. */
	expected = TYPEC_LOCK_UNMADE;
	claimed = __atomic_compare_exchange_n(&typec_lock_state, &expected, TYPEC_LOCK_MAKING, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
	if (claimed) {
		error = mutex_init(&typec_lock, LOCK_RANK_DEVICE, "typec");
		if (error != 0)
			kern_logf("typec: lock not made (%d)\n", error);

		/* MADE publishes the lock to every thread that waits for it. */
		__atomic_store_n(&typec_lock_state, TYPEC_LOCK_MADE, __ATOMIC_RELEASE);
	}

	/* Another thread's making, waited for; it is a few instructions. */
	state = __atomic_load_n(&typec_lock_state, __ATOMIC_ACQUIRE);
	while (state != TYPEC_LOCK_MADE)
		state = __atomic_load_n(&typec_lock_state, __ATOMIC_ACQUIRE);

	/* Taken. */
	mutex_lock(&typec_lock);
}

/*
 * Releases the lock of the connector records.
 */
void
drv_typec_os_unlock(void)
{
	/* Released. */
	mutex_unlock(&typec_lock);
}

/*
 * Reports the milliseconds since the kernel started, from the scheduler's
 * ticks.
 */
uint64_t
drv_typec_os_now_ms(void)
{
	uint64_t ticks;
	uint64_t milliseconds;

	/* The ticks counted so far, in milliseconds. */
	ticks = sched_ticks();
	milliseconds = kern_ticks_to_ms(ticks);

	/* Succeeded: the time. */
	return milliseconds;
}

/*
 * Writes a line of the driver's log to the kernel's log.
 */
void
drv_typec_os_log(
	const char *format,
	...)
{
	char line[TYPEC_LOG_LINE_MAX];
	va_list arguments;
	int length;

	/* The line, cut short when it is too long. */
	va_start(arguments, format);
	length = kern_vsnprintf(line, sizeof(line), format, arguments);
	va_end(arguments);
	if (length < 0)
		return;
	if ((size_t)length >= sizeof(line))
		length = (int)sizeof(line) - 1;

	/* Into the log. */
	kern_log_write(line, (size_t)length);
}

/*
 * Maps a device's physical range uncached; the HAL refuses a range that
 * holds RAM, so a mailbox firmware left in memory the kernel hands out is
 * never used.  The range need not start on a page.
 */
int
drv_typec_os_map(
	uint64_t physical,
	size_t size,
	volatile uint8_t **mapping)
{
	uint64_t page;
	size_t offset;
	size_t length;
	void *window;
	int error;

	/* The pages that hold the range. */
	page = physical & ~(uint64_t)(KERN_PAGE_SIZE - 1U);
	offset = (size_t)(physical - page);
	length = (offset + size + KERN_PAGE_SIZE - 1U) & ~(size_t)(KERN_PAGE_SIZE - 1U);

	/* Maps them for reading and writing, uncached as the device wants. */
	error = hal_space_map_device((hal_physaddr_t)page, length, HAL_SPACE_READ | HAL_SPACE_WRITE | HAL_SPACE_NOCACHE, &window);
	if (error != HAL_OK)
		return EFAULT;

	/* Succeeded: the range is reached through the window, for as long as the kernel runs. */
	*mapping = (volatile uint8_t *)window + offset;
	return 0;
}

/*
 * Removes a mapping drv_typec_os_map() made of a range of the same size:
 * the same pages, given back to the HAL's window.
 */
void
drv_typec_os_unmap(
	volatile uint8_t *mapping,
	size_t size)
{
	uintptr_t address;
	uintptr_t page;
	size_t offset;
	size_t length;
	int error;

	/* The pages the mapping took. */
	address = (uintptr_t)mapping;
	page = address & ~(uintptr_t)(KERN_PAGE_SIZE - 1U);
	offset = (size_t)(address - page);
	length = (offset + size + KERN_PAGE_SIZE - 1U) & ~(size_t)(KERN_PAGE_SIZE - 1U);

	/* Given back; a refusal is only logged (the window keeps them). */
	error = hal_space_unmap_device((void *)page, length);
	if (error != HAL_OK)
		kern_logf("typec: the mailbox's mapping was not removed (%d)\n", error);
}

/*
 * Reads a byte of a mapped device range.
 */
uint8_t
drv_typec_os_read8(
	const volatile uint8_t *address)
{
	uint8_t value;

	/* One byte access, as firmware's ByteAcc fields make. */
	value = hal_mmio_read8(address);

	/* Succeeded: the byte. */
	return value;
}

/*
 * Writes a byte of a mapped device range.
 */
void
drv_typec_os_write8(
	volatile uint8_t *address,
	uint8_t value)
{
	/* One byte access, as firmware's ByteAcc fields make. */
	hal_mmio_write8(address, value);
}

/*
 * Prepares the signal of the ACPI notification.
 */
void
drv_typec_os_signal_init(void)
{
	/* The lock and the queue, lowered. */
	spin_init(&typec_signal_lock, LOCK_RANK_DEVICE, "typec signal");
	waitq_init(&typec_signal_waitq, "typec signal");
	typec_signalled = false;
}

/*
 * Raises the signal and wakes the thread that waits for it.
 */
void
drv_typec_os_signal(void)
{
	unsigned long irq;

	/* Raised; the waiting thread lowers it when it wakes. */
	irq = spin_lock_irqsave(&typec_signal_lock);

	typec_signalled = true;
	waitq_wake_all(&typec_signal_waitq);

	spin_unlock_irqrestore(&typec_signal_lock, irq);
}

/*
 * Waits for the signal for at most some milliseconds and lowers it.
 *
 * Returns 1 when it was raised (before the call or during it), 0 when the
 * time passed without it.
 */
int
drv_typec_os_wait(
	uint32_t milliseconds)
{
	unsigned long irq;
	uint64_t deadline;
	uint64_t observed;
	uint64_t now;
	bool raised;
	int error;

	/* Sleeps until the signal or the deadline. */
	deadline = sched_ticks() + KERN_MS_TO_TICKS(milliseconds);
	irq = spin_lock_irqsave(&typec_signal_lock);

	for (;;) {
		/* A raised signal ends the wait. */
		if (typec_signalled)
			break;

		/* So does the deadline. */
		now = sched_ticks();
		if (now >= deadline)
			break;

		/* Sleeps until a wake or the deadline. */
		observed = waitq_sequence(&typec_signal_waitq);
		error = waitq_sleep(&typec_signal_waitq, &typec_signal_lock, observed, deadline, 0U);
		(void)error;
	}

	/* Lowers what was raised. */
	raised = typec_signalled;
	typec_signalled = false;

	spin_unlock_irqrestore(&typec_signal_lock, irq);

	/* The time passed without the signal. */
	if (!raised)
		return 0;

	/* Succeeded: the signal came. */
	return 1;
}

/*
 * Starts the driver's thread.
 */
int
drv_typec_os_thread_start(
	void (*body)(void *argument),
	void *argument)
{
	struct thread *thread;
	int error;

	/* The thread. */
	error = kthread_create(body, argument, SCHED_PRIORITY_DEFAULT, &thread);
	if (error != 0)
		return error;

	/* Succeeded: it runs. */
	thread_start(thread);
	return 0;
}

/*
 * Publishes /dev/typec.
 */
int
drv_typec_os_device_register(void)
{
	int error;

	/* The character device. */
	error = cdev_register("typec", (dev_t)TYPEC_DEVICE_NUMBER, &typec_ops, NULL);
	if (error != 0)
		return error;

	/* Succeeded: user programs can read /dev/typec. */
	return 0;
}

/* Takes the text of every connector as it is at the open. */
static int
typec_open(
	struct file *file)
{
	struct typec_open *state;
	int error;

	/* The open's state. */
	state = kern_malloc(sizeof(*state));
	if (state == NULL)
		return ENOMEM;

	/* Nothing read yet. */
	kern_memset(state, 0, sizeof(*state));

	/* The lock that keeps two reads from moving the position at once. */
	error = mutex_init(&state->lock, LOCK_RANK_DEVICE, "typec device");
	if (error != 0) {
		kern_free(state);
		return error;
	}

	/* The text, from copies of the records. */
	state->length = drv_typec_text(state->text, sizeof(state->text));

	/* Succeeded: the file holds the state until it closes. */
	file->f_data = state;
	return 0;
}

/* Frees an open's state. */
static int
typec_close(
	struct file *file)
{
	/* The state, when there is one. */
	if (file->f_data != NULL)
		kern_free(file->f_data);

	/* Succeeded: the file holds nothing any more. */
	file->f_data = NULL;
	return 0;
}

/* Reads what is left of the open's text. */
static ssize_t
typec_read(
	struct file *file,
	void *buffer,
	size_t size)
{
	struct typec_open *state;
	size_t count;

	/* What is left, up to the caller's buffer, and the position past it. */
	state = file->f_data;
	mutex_lock(&state->lock);

	count = state->length - state->position;
	if (count > size)
		count = size;
	if (count != 0)
		kern_memcpy(buffer, state->text + state->position, count);
	state->position += count;

	mutex_unlock(&state->lock);

	/* Succeeded: zero bytes at the end of the text. */
	return (ssize_t)count;
}
