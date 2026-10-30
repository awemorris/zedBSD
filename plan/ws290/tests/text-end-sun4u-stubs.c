/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The kernel's services as stubs for text-end-sun4u-host-test.c: locks
 * that count what is held, the interrupt switch, the character device
 * registry, the user copies, the 16550's I/O ports (the bytes sent are
 * kept), the input thread, and the hardware layer's character output.
 */

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "hal/hal.h"
#include "kern/cdev.h"
#include "kern/klog.h"
#include "kern/lock.h"
#include "kern/thread.h"
#include "kern/uaccess.h"
#include "kern/waitq.h"
#include "uapi/errno.h"

/* The 16550's line status and transmit registers, as the console reads them. */
#define STUB_UART_LSR		5U
#define STUB_UART_LSR_THRE	0x20U

/* The kernel's character output, which the console publishes. */
void (*kernel_putc)(int c);

/* What the stubs saw: the device /dev/graphics registered and its table. */
const struct cdev_ops *stub_graphics_ops;
unsigned stub_cdev_registers;

/* How deep interrupts are off, and the deepest a sleeping call saw. */
int stub_irq_off;

/* The sleepable locks held now, by name, and how many. */
const char *stub_held[8];
unsigned stub_held_count;

/* The 16550's port the console was given, and the bytes it sent there. */
uint16_t stub_uart_port;
char stub_uart_sent[512];
unsigned stub_uart_sent_count;

/* The characters the hardware layer drew itself (none once the kernel published its own). */
unsigned stub_hal_drawn;

/* Answers the console's look at a port: the transmitter is always ready. */
uint8_t
kern_io_in8(
	uint16_t port)
{
	/* The line status says the transmitter has room. */
	if (port == stub_uart_port + STUB_UART_LSR)
		return STUB_UART_LSR_THRE;

	/* Nothing was received. */
	return 0;
}

/* Keeps a byte the console sent to the 16550's transmitter. */
void
kern_io_out8(
	uint16_t port,
	uint8_t value)
{
	/* Only the transmitter of the console's port is kept. */
	if (port != stub_uart_port || stub_uart_sent_count + 1U >= sizeof(stub_uart_sent))
		return;
	stub_uart_sent[stub_uart_sent_count] = (char)value;
	stub_uart_sent_count++;
	stub_uart_sent[stub_uart_sent_count] = '\0';
}

/* The early console: delegates once the kernel published one, else draws itself. */
void
kern_console_early_putc(
	int c)
{
	void (*published)(int);

	/* The kernel's own output, once published, takes every character. */
	published = __atomic_load_n(&kernel_putc, __ATOMIC_ACQUIRE);
	if (published != NULL) {
		published(c);
		return;
	}

	/* The hardware layer's early console draws the screen. */
	stub_hal_drawn++;
}

/* Turns interrupts off and reports whether they were on. */
bool
kern_irq_disable(
	void)
{
	/* Counts the depth. */
	stub_irq_off++;

	/* They were on at the outermost level. */
	if (stub_irq_off == 1)
		return true;
	return false;
}

/* Turns interrupts back on. */
void
kern_irq_enable(
	void)
{
	/* Counts the depth down. */
	stub_irq_off--;
}

/* Starts a spin lock: nothing to do on the host. */
void
spin_init(
	struct spinlock *lock,
	enum lock_rank rank,
	const char *name)
{
	(void)lock;
	(void)rank;
	(void)name;
}

/* Takes a spin lock: one thread on the host. */
void
spin_lock(
	struct spinlock *lock)
{
	(void)lock;
}

/* Gives a spin lock back. */
void
spin_unlock(
	struct spinlock *lock)
{
	(void)lock;
}

/* Takes a spin lock with interrupts off. */
unsigned long
spin_lock_irqsave(
	struct spinlock *lock)
{
	(void)lock;

	/* Counts the depth as kern_irq_disable() does. */
	stub_irq_off++;
	return 1;
}

/* Gives a spin lock back and interrupts with it. */
void
spin_unlock_irqrestore(
	struct spinlock *lock,
	unsigned long enabled)
{
	(void)lock;
	(void)enabled;

	/* Counts the depth down. */
	stub_irq_off--;
}

/* Keeps nothing of the log: the console is loud. */
void
kern_log_write(
	const char *bytes,
	size_t length)
{
	(void)bytes;
	(void)length;
}

/* Ignores the quiet switch. */
void
kern_log_set_quiet(
	int quiet)
{
	(void)quiet;
}

/* Reports a loud log. */
int
kern_log_quiet(
	void)
{
	return 0;
}

/* Starts a sleepable lock, naming it. */
int
mutex_init(
	struct mutex *mutex,
	enum lock_rank rank,
	const char *name)
{
	(void)rank;

	/* The name tells the stubs which lock is held. */
	memset(mutex, 0, sizeof(*mutex));
	mutex->guard.name = name;
	return 0;
}

/* Takes a sleepable lock, which must not be taken with interrupts off. */
void
mutex_lock(
	struct mutex *mutex)
{
	/* Records the lock as held. */
	if (stub_held_count < 8U)
		stub_held[stub_held_count] = mutex->guard.name;
	stub_held_count++;
	mutex->locked = 1;
}

/* Gives a sleepable lock back. */
void
mutex_unlock(
	struct mutex *mutex)
{
	/* The last lock taken is the one given back. */
	mutex->locked = 0;
	stub_held_count--;
}

/* Wakes nobody: the test has no waiters. */
void
waitq_wake_all(
	struct wait_queue *queue)
{
	(void)queue;
}

/* Records the device /dev/graphics registers. */
int
cdev_register(
	const char *name,
	dev_t rdev,
	const struct cdev_ops *ops,
	void *data)
{
	(void)rdev;
	(void)data;

	/* Only /dev/graphics registers here. */
	if (strcmp(name, "graphics") != 0)
		return EINVAL;
	stub_graphics_ops = ops;
	stub_cdev_registers++;
	return 0;
}

/* Copies a request in from the test's memory. */
int
copyin(
	uintptr_t source,
	void *destination,
	size_t size)
{
	memcpy(destination, (const void *)source, size);
	return 0;
}

/* Copies a reply out to the test's memory. */
int
copyout(
	const void *source,
	uintptr_t destination,
	size_t size)
{
	memcpy((void *)destination, source, size);
	return 0;
}

/* Makes no input thread: the test reads no serial port. */
int
kthread_create(
	void (*entry)(void *),
	void *argument,
	int priority,
	struct thread **result)
{
	(void)entry;
	(void)argument;
	(void)priority;
	(void)result;
	return ENOMEM;
}

/* Starts nothing. */
void
thread_start(
	struct thread *thread)
{
	(void)thread;
}

/* Takes no input. */
void
tty_console_input_byte(
	uint8_t byte)
{
	(void)byte;
}

/* Reports no time passing. */
uint64_t
sched_ticks(
	void)
{
	return 0;
}

/* Sleeps for no time. */
void
sched_sleep(
	uint64_t timeout_tick)
{
	(void)timeout_tick;
}

/* A tick of the test's clock is one millisecond. */
uint64_t
kern_ms_to_ticks(
	uint64_t milliseconds)
{
	return milliseconds;
}
