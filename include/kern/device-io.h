/* -*- mode: c; c-file-style: "linux"; tab-width: 8; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Device register access for drivers.
 *
 * Drivers reach hardware through the kernel, never through the HAL. These
 * calls forward to the port and memory-mapped accessors the HAL provides,
 * and they carry the same ordering guarantees.
 */

#ifndef KERN_DEVICE_IO_H
#define KERN_DEVICE_IO_H

#include <stdint.h>

/*
 * Port I/O.
 *
 * Architectures without a separate I/O space report a fixed value on read
 * and discard a write.
 */
uint8_t kern_io_in8(uint16_t port);
uint16_t kern_io_in16(uint16_t port);
uint32_t kern_io_in32(uint16_t port);
void kern_io_out8(uint16_t port, uint8_t value);
void kern_io_out16(uint16_t port, uint16_t value);
void kern_io_out32(uint16_t port, uint32_t value);

/*
 * Memory-mapped register access.
 *
 * The address must come from a mapping the kernel established, such as
 * kern_device_map().
 */
uint8_t kern_mmio_read8(const volatile void *address);
uint16_t kern_mmio_read16(const volatile void *address);
uint32_t kern_mmio_read32(const volatile void *address);
uint64_t kern_mmio_read64(const volatile void *address);
void kern_mmio_write8(volatile void *address, uint8_t value);
void kern_mmio_write16(volatile void *address, uint16_t value);
void kern_mmio_write32(volatile void *address, uint32_t value);
void kern_mmio_write64(volatile void *address, uint64_t value);

/*
 * Ordering barriers for device access.
 *
 * kern_io_barrier() orders reads and writes against each other,
 * kern_io_read_barrier() orders reads, and kern_io_write_barrier() orders
 * writes. kern_compiler_barrier() only stops the compiler from moving
 * accesses across it.
 */
void kern_io_barrier(void);
void kern_io_read_barrier(void);
void kern_io_write_barrier(void);
void kern_compiler_barrier(void);

#endif
