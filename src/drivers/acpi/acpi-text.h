/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The namespace and evaluation results as text, for /dev/acpi and the
 * host tests.
 */

#ifndef KERN_DRIVERS_ACPI_ACPI_TEXT_H
#define KERN_DRIVERS_ACPI_ACPI_TEXT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <drivers/acpi/acpi.h>

/*
 * A growing text: its characters (terminated), their count, the room it
 * has, and the error that stopped it growing (zero while it can grow).
 */
struct drv_acpi_text {
	char *data;
	size_t length;
	size_t capacity;
	int error;
};

void
drv_acpi_text_init(
	struct drv_acpi_text *text);

void
drv_acpi_text_release(
	struct drv_acpi_text *text);

void
drv_acpi_text_printf(
	struct drv_acpi_text *text,
	const char *format,
	...) __attribute__((format(printf, 2, 3)));

int
drv_acpi_text_namespace(
	struct drv_acpi_text *text);

int
drv_acpi_text_evaluate(
	struct drv_acpi_text *text,
	struct drv_acpi_node *scope,
	const char *path,
	const char *label);

/*
 * Publishes /dev/acpi, which reads this text (the kernel's acpi-dev.c).
 */
int
drv_acpi_device_register(void);

#endif
