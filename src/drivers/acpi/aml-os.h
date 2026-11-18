/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * What the AML interpreter needs from the operating system.
 *
 * The kernel implements these in acpi-kern.c; the host test harness
 * implements them over the host C library.  The interpreter calls nothing
 * else outside kcrt.
 */

#ifndef KERN_DRIVERS_ACPI_AML_OS_H
#define KERN_DRIVERS_ACPI_AML_OS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

void *
drv_acpi_os_alloc(
	size_t size);

void
drv_acpi_os_free(
	void *pointer);

void
drv_acpi_os_log(
	const char *format,
	...) __attribute__((format(printf, 1, 2)));

size_t
drv_acpi_os_stack_budget(void);

void
drv_acpi_os_sleep(
	uint64_t milliseconds);

void
drv_acpi_os_stall(
	uint64_t microseconds);

uint64_t
drv_acpi_os_timer(void);

void
drv_acpi_os_lock(void);

void
drv_acpi_os_unlock(void);

bool
drv_acpi_os_lock_owned(void);

int
drv_acpi_os_port_read(
	uint32_t port,
	unsigned width,
	uint32_t *value);

int
drv_acpi_os_port_write(
	uint32_t port,
	unsigned width,
	uint32_t value);

unsigned long
drv_acpi_os_event_lock(void);

void
drv_acpi_os_event_unlock(
	unsigned long state);

int
drv_acpi_os_table(
	const char *signature,
	const char *oem_id,
	const char *oem_table_id,
	const uint8_t **data,
	size_t *length);

#endif
