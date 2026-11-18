/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * What the Type-C layer, the UCSI core and its ACPI transport need from the
 * operating system.
 *
 * The kernel implements these in typec-kern.c (ws050-p003); the host tests
 * implement them over the host C library.  The layer, the core and the
 * transport call nothing else outside kcrt and the ACPI driver.
 */

#ifndef KERN_DRIVERS_TYPEC_TYPEC_OS_H
#define KERN_DRIVERS_TYPEC_TYPEC_OS_H

#include <stddef.h>
#include <stdint.h>

/*
 * Takes the lock of the connector records.
 */
void
drv_typec_os_lock(void);

/*
 * Releases the lock of the connector records.
 */
void
drv_typec_os_unlock(void);

/*
 * Reports the milliseconds since the kernel started (a clock that only
 * moves forward).
 */
uint64_t
drv_typec_os_now_ms(void);

/*
 * Writes a line of the driver's log.
 */
void
drv_typec_os_log(
	const char *format,
	...) __attribute__((format(printf, 1, 2)));

/*
 * Maps a range of physical memory that belongs to a device (the UCSI
 * mailbox) uncached for reading and writing; refuses a range of RAM.
 */
int
drv_typec_os_map(
	uint64_t physical,
	size_t size,
	volatile uint8_t **mapping);

/*
 * Removes a mapping drv_typec_os_map() made, of the same size (the driver
 * stopped, ws177-p003).
 */
void
drv_typec_os_unmap(
	volatile uint8_t *mapping,
	size_t size);

/*
 * Reads a byte of a mapped device range.
 */
uint8_t
drv_typec_os_read8(
	const volatile uint8_t *address);

/*
 * Writes a byte of a mapped device range.
 */
void
drv_typec_os_write8(
	volatile uint8_t *address,
	uint8_t value);

/*
 * Prepares the signal the ACPI notification gives the driver's thread.
 */
void
drv_typec_os_signal_init(void);

/*
 * Raises the signal: called inside the ACPI interpreter, it neither
 * blocks nor takes a sleeping lock.
 */
void
drv_typec_os_signal(void);

/*
 * Waits for the signal for at most some milliseconds and lowers it:
 * returns 1 when it was raised, 0 when the time passed without it.
 */
int
drv_typec_os_wait(
	uint32_t milliseconds);

/*
 * Starts the driver's thread.
 */
int
drv_typec_os_thread_start(
	void (*body)(void *argument),
	void *argument);

/*
 * Publishes the diagnostic device /dev/typec.
 */
int
drv_typec_os_device_register(void);

#endif
