/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The SPARC V9 HAL's PCI I/O space.
 */

#ifndef KERN_HAL_SPARCV9_IO_H
#define KERN_HAL_SPARCV9_IO_H

#include <hal/types.h>

/*
 * Records where the PCI I/O space lies.
 */
void
sparcv9_io_init(
	uint64_t base);

/*
 * Reports the physical address of a PCI I/O port.
 */
uint64_t
sparcv9_io_port_address(
	uint16_t port);

#endif
