/* m68k memory-mapped I/O accessors.  X68k has no separate port space. */
/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */

#include <hal/hal.h>

uint8_t
hal_mmio_read8(const volatile void *address)
{
	uint8_t value = *(const volatile uint8_t *)address;
	hal_io_rmb();
	return value;
}

uint16_t
hal_mmio_read16(const volatile void *address)
{
	uint16_t value = *(const volatile uint16_t *)address;
	hal_io_rmb();
	return value;
}

uint32_t
hal_mmio_read32(const volatile void *address)
{
	uint32_t value = *(const volatile uint32_t *)address;
	hal_io_rmb();
	return value;
}

uint64_t
hal_mmio_read64(const volatile void *address)
{
	uint64_t value = *(const volatile uint64_t *)address;
	hal_io_rmb();
	return value;
}

void hal_mmio_write8(volatile void *a, uint8_t v)
{ *(volatile uint8_t *)a = v; hal_io_wmb(); }
void hal_mmio_write16(volatile void *a, uint16_t v)
{ *(volatile uint16_t *)a = v; hal_io_wmb(); }
void hal_mmio_write32(volatile void *a, uint32_t v)
{ *(volatile uint32_t *)a = v; hal_io_wmb(); }
void hal_mmio_write64(volatile void *a, uint64_t v)
{ *(volatile uint64_t *)a = v; hal_io_wmb(); }

static void
port_io_unsupported(void)
{
	HAL_FATAL("m68k/X68k has no port-I/O address space");
}

uint8_t hal_io_inp8(uint16_t p) { (void)p; port_io_unsupported(); return 0; }
uint16_t hal_io_inp16(uint16_t p) { (void)p; port_io_unsupported(); return 0; }
uint32_t hal_io_inp32(uint16_t p) { (void)p; port_io_unsupported(); return 0; }
void hal_io_outp8(uint16_t p, uint8_t v)
{ (void)p; (void)v; port_io_unsupported(); }
void hal_io_outp16(uint16_t p, uint16_t v)
{ (void)p; (void)v; port_io_unsupported(); }
void hal_io_outp32(uint16_t p, uint32_t v)
{ (void)p; (void)v; port_io_unsupported(); }
