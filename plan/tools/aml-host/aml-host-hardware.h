/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The simulated ACPI hardware of the host harness (WS049).
 */

#ifndef WS049_AML_HOST_HARDWARE_H
#define WS049_AML_HOST_HARDWARE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

int
hardware_port(
	uint32_t port,
	unsigned width,
	bool write,
	uint32_t *value);

void
hardware_raise_gpe(
	unsigned gpe);

void
hardware_raise_fixed(
	unsigned bit);

void
hardware_ec_query(
	uint8_t query);

void
hardware_ec_ram(
	uint8_t address,
	uint8_t value);

void
hardware_ec_ports(
	uint32_t data,
	uint32_t command);

uint8_t
hardware_ec_ram_read(
	uint8_t address);

volatile uint32_t *
hardware_global_lock(void);

void
hardware_tick(void);

size_t
hardware_default_fadt(
	uint8_t *fadt,
	size_t size);

#endif
