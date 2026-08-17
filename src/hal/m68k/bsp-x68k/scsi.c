/* X68000 internal MB89352 MMIO binding. */
/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */

#include "scsi.h"
#include "mmio.h"

#define X68K_SCSI_ID_SRAM_PHYSICAL 0x00ed0070U
#define X68K_SPC_POLL_LIMIT 10000000U

static uint8_t
spc_read_register(void *cookie, unsigned reg)
{
	(void)cookie;
	return x68k_spc_read(reg);
}

static void
spc_write_register(void *cookie, unsigned reg, uint8_t value)
{
	(void)cookie;
	x68k_spc_write(reg, value);
}

static void
spc_relax(void *cookie)
{
	(void)cookie;
	__asm__ volatile("nop");
}

void
x68k_bsp_spc_bus(struct x68k_spc_bus *bus)
{
	if (bus == NULL)
		return;
	bus->cookie = NULL;
	bus->read = spc_read_register;
	bus->write = spc_write_register;
	bus->relax = spc_relax;
	bus->poll_limit = X68K_SPC_POLL_LIMIT;
}

unsigned
x68k_bsp_scsi_initiator_id(void)
{
	return hal_mmio_read8((const volatile void *)X68K_DEVICE_ADDRESS(
	    X68K_SCSI_ID_SRAM_PHYSICAL)) & 7U;
}
