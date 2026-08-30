/* ISA IRQ routing through ACPI-described I/O APICs. */
/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */

#include <hal/hal.h>
#include "ioapic.h"
#include "early-init-policy.h"
#include "../defs.h"

struct ioapic_state {
	volatile uint32_t *base;
	uint32_t gsi_base;
	uint32_t redirections;
};

static struct ioapic_state controllers[AMD64_IOAPIC_MAX];
static unsigned controller_count;
static uint32_t irq_gsi[16];
static uint16_t irq_flags[16];
static uint32_t irq_destination[16];
static volatile unsigned ioapic_lock;

static uint32_t
read_reg_unlocked(struct ioapic_state *io, uint8_t reg)
{
	io->base[0] = reg;
	hal_io_mb();
	return io->base[4];
}

static void
write_reg_unlocked(struct ioapic_state *io, uint8_t reg, uint32_t value)
{
	io->base[0] = reg;
	hal_io_mb();
	io->base[4] = value;
	hal_io_mb();
}

static struct ioapic_state *
find_gsi(uint32_t gsi, unsigned *pin)
{
	unsigned i;
	for (i = 0; i < controller_count; i++)
		if (gsi >= controllers[i].gsi_base &&
		    gsi - controllers[i].gsi_base < controllers[i].redirections) {
			*pin = gsi - controllers[i].gsi_base;
			return &controllers[i];
		}
	return NULL;
}

static int
write_route(int irq, uint32_t apic_id, int masked)
{
	struct ioapic_state *io;
	unsigned pin;
	uint32_t low = INT_IRQ_BASE + (uint32_t)irq;
	uint16_t flags;
	bool enabled;

	if (irq < 0 || irq >= 16 || apic_id > 255U)
		return HAL_ERR_INVALID;
	io = find_gsi(irq_gsi[irq], &pin);
	if (io == NULL)
		return HAL_ERR_UNSUPPORTED;
	flags = irq_flags[irq];
	if ((flags & 3U) == 3U)
		low |= 1U << 13; /* active low */
	if (((flags >> 2) & 3U) == 3U)
		low |= 1U << 15; /* level triggered */
	if (masked)
		low |= 1U << 16;
	enabled = hal_irq_disable();
	while (__atomic_exchange_n(&ioapic_lock, 1U, __ATOMIC_ACQUIRE) != 0)
		__asm__ volatile("pause");
	write_reg_unlocked(io, (uint8_t)(0x11U + pin * 2U), apic_id << 24);
	write_reg_unlocked(io, (uint8_t)(0x10U + pin * 2U), low);
	__atomic_store_n(&ioapic_lock, 0U, __ATOMIC_RELEASE);
	if (enabled) hal_irq_enable();
	return HAL_OK;
}

int
amd64_ioapic_init(const struct amd64_acpi_info *acpi,
	uint32_t bootstrap_apic_id)
{
	struct amd64_ioapic_range ranges[AMD64_IOAPIC_MAX];
	unsigned i;

	if (acpi == NULL || acpi->ioapic_count == 0)
		return HAL_ERR_INVALID;
	controller_count = 0;
	for (i = 0; i < acpi->ioapic_count; i++) {
		struct hal_pmem_request request = {
			acpi->ioapics[i].address, 4096, 4096,
			HAL_PMEM_TYPE_MMIO, HAL_PMEM_ATTR_NOCACHE
		};
		struct hal_pmem memory;
		uint32_t version;
		enum amd64_ioapic_policy_result policy;
		hal_printf("A64 IOAPIC BEGIN index=%u address=%08X gsi=%u\n", i,
		    acpi->ioapics[i].address, acpi->ioapics[i].gsi_base);
		if (hal_pmem_alloc(&request, &memory) != HAL_OK) {
			hal_printf("A64 IOAPIC MAP FAIL index=%u\n", i);
			return HAL_ERR_UNSUPPORTED;
		}
		controllers[i].base = memory.vaddr;
		controllers[i].gsi_base = acpi->ioapics[i].gsi_base;
		{
			bool enabled = hal_irq_disable();
			while (__atomic_exchange_n(&ioapic_lock, 1U,
			    __ATOMIC_ACQUIRE) != 0)
				__asm__ volatile("pause");
			version = read_reg_unlocked(&controllers[i], 1);
			__atomic_store_n(&ioapic_lock, 0U, __ATOMIC_RELEASE);
			if (enabled) hal_irq_enable();
		}
		policy = amd64_ioapic_policy_evaluate(version,
		    controllers[i].gsi_base, ranges, i,
		    &controllers[i].redirections);
		if (policy != AMD64_IOAPIC_POLICY_OK) {
			hal_printf("A64 IOAPIC TOPOLOGY FAIL index=%u version=%08X "
			    "gsi=%u pins=%u result=%s\n", i, version,
			    controllers[i].gsi_base, controllers[i].redirections,
			    amd64_ioapic_policy_result_name(policy));
			return HAL_ERR_UNSUPPORTED;
		}
		ranges[i].gsi_base = controllers[i].gsi_base;
		ranges[i].redirections = controllers[i].redirections;
		hal_printf("A64 IOAPIC READY index=%u version=%08X pins=%u\n", i,
		    version, controllers[i].redirections);
		controller_count++;
	}
	for (i = 0; i < 16; i++) {
		irq_gsi[i] = acpi->isa[i].gsi;
		irq_flags[i] = acpi->isa[i].flags;
		irq_destination[i] = bootstrap_apic_id;
		if (write_route((int)i, bootstrap_apic_id, 1) != HAL_OK) {
			hal_printf("A64 IOAPIC ROUTE FAIL irq=%u gsi=%u\n", i,
			    irq_gsi[i]);
			return HAL_ERR_UNSUPPORTED;
		}
	}
	hal_printf("A64 IOAPIC ROUTING READY controllers=%u destination=%u\n",
	    controller_count, bootstrap_apic_id);
	return HAL_OK;
}

void amd64_ioapic_mask(int irq)
{ if (irq >= 0 && irq < 16) (void)write_route(irq, irq_destination[irq], 1); }
void amd64_ioapic_unmask(int irq)
{ if (irq >= 0 && irq < 16) (void)write_route(irq, irq_destination[irq], 0); }
int amd64_ioapic_route(int irq, uint32_t apic_id)
{
	int error = write_route(irq, apic_id, 1);
	if (error == HAL_OK) irq_destination[irq] = apic_id;
	return error;
}
