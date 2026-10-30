/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The harness's calls of the Dell provider (src/drivers/acpi/acpi-dell.c).
 */

#ifndef AML_HOST_DELL_H
#define AML_HOST_DELL_H

int
host_dell_attach(void);

int
host_dell_apply(
	const char *request);

int
host_dell_poll(void);

int
host_dell_fan(void);

int
host_dell_boost(
	const char *request);

#endif
