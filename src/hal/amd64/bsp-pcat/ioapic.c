/* -*- mode: c; c-file-style: "linux"; tab-width: 8; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The ACPI-described amd64 I/O APIC routing implementation.
 */

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

static uint32_t read_reg_unlocked(struct ioapic_state *ioapic, uint8_t reg);
static void write_reg_unlocked(struct ioapic_state *ioapic, uint8_t reg, uint32_t value);
static struct ioapic_state *find_gsi(uint32_t gsi, unsigned *pin);
static int write_route(int irq, uint32_t apic_id, int masked);

/*
 * Initializes every ACPI-described I/O APIC and ISA route.
 */
int
amd64_ioapic_init(
	const struct amd64_acpi_info *acpi,
	uint32_t bootstrap_apic_id)
{
	struct amd64_ioapic_range ranges[AMD64_IOAPIC_MAX];
	struct hal_pmem_request request;
	struct hal_pmem memory;
	const char *policy_name;
	uint32_t version;
	unsigned index;
	bool enabled;
	enum amd64_ioapic_policy_result policy;
	int error;

	/* Requires at least one firmware-described controller. */
	if (acpi == NULL || acpi->ioapic_count == 0)
		return HAL_ERR_INVALID;

	/* Maps and validates each controller in firmware order. */
	controller_count = 0;
	for (index = 0; index < acpi->ioapic_count; index++) {
		hal_printf(
			"A64 IOAPIC BEGIN index=%u address=%08X gsi=%u\n",
			index,
			acpi->ioapics[index].address,
			acpi->ioapics[index].gsi_base);

		/* Claims the controller's uncached MMIO page. */
		request.paddr = acpi->ioapics[index].address;
		request.size = 4096;
		request.alignment = 4096;
		request.type = HAL_PMEM_TYPE_MMIO;
		request.attr = HAL_PMEM_ATTR_NOCACHE;
		error = hal_pmem_alloc(&request, &memory);
		if (error != HAL_OK) {
			hal_printf("A64 IOAPIC MAP FAIL index=%u\n", index);
			return HAL_ERR_UNSUPPORTED;
		}

		/* Records the persistent controller mapping and GSI base. */
		controllers[index].base = memory.vaddr;
		controllers[index].gsi_base = acpi->ioapics[index].gsi_base;

		/* Reads the volatile version register under the MMIO lock. */
		enabled = hal_irq_disable();
		while (__atomic_exchange_n(
		    &ioapic_lock,
		    1U,
		    __ATOMIC_ACQUIRE) != 0) {
			__asm__ volatile("pause");
		}
		version = read_reg_unlocked(&controllers[index], 1);
		__atomic_store_n(&ioapic_lock, 0U, __ATOMIC_RELEASE);

		/* Restores interrupts only when they were previously enabled. */
		if (enabled)
			hal_irq_enable();

		/* Validates pin capacity and nonoverlap with earlier controllers. */
		policy = amd64_ioapic_policy_evaluate(
			version,
			controllers[index].gsi_base,
			ranges,
			index,
			&controllers[index].redirections);
		if (policy != AMD64_IOAPIC_POLICY_OK) {
			policy_name = amd64_ioapic_policy_result_name(policy);
			hal_printf(
				"A64 IOAPIC TOPOLOGY FAIL index=%u version=%08X "
				"gsi=%u pins=%u result=%s\n",
				index,
				version,
				controllers[index].gsi_base,
				controllers[index].redirections,
				policy_name);
			return HAL_ERR_UNSUPPORTED;
		}

		/* Publishes this controller for subsequent overlap checks. */
		ranges[index].gsi_base = controllers[index].gsi_base;
		ranges[index].redirections = controllers[index].redirections;
		hal_printf(
			"A64 IOAPIC READY index=%u version=%08X pins=%u\n",
			index,
			version,
			controllers[index].redirections);
		controller_count++;
	}

	/* Installs every ACPI-translated ISA route in masked state. */
	for (index = 0; index < 16; index++) {
		irq_gsi[index] = acpi->isa[index].gsi;
		irq_flags[index] = acpi->isa[index].flags;
		irq_destination[index] = bootstrap_apic_id;

		/* Programs and validates this ISA redirection entry. */
		error = write_route((int)index, bootstrap_apic_id, 1);
		if (error != HAL_OK) {
			hal_printf(
				"A64 IOAPIC ROUTE FAIL irq=%u gsi=%u\n",
				index,
				irq_gsi[index]);
			return HAL_ERR_UNSUPPORTED;
		}
	}

	/* Reports the completely routed controller set. */
	hal_printf(
		"A64 IOAPIC ROUTING READY controllers=%u destination=%u\n",
		controller_count,
		bootstrap_apic_id);

	/* Reports successful I/O APIC initialization. */
	return HAL_OK;
}

/*
 * Masks one ISA I/O APIC route.
 */
void
amd64_ioapic_mask(
	int irq)
{
	/* Ignores values outside the ISA routing table. */
	if (irq < 0 || irq >= 16)
		return;

	/* Rewrites the route with its current destination while masked. */
	(void)write_route(irq, irq_destination[irq], 1);
}

/*
 * Unmasks one ISA I/O APIC route.
 */
void
amd64_ioapic_unmask(
	int irq)
{
	/* Ignores values outside the ISA routing table. */
	if (irq < 0 || irq >= 16)
		return;

	/* Rewrites the route with its current destination while unmasked. */
	(void)write_route(irq, irq_destination[irq], 0);
}

/*
 * Routes one masked ISA interrupt to an APIC destination.
 */
int
amd64_ioapic_route(
	int irq,
	uint32_t apic_id)
{
	int error;

	/* Programs the masked hardware route before publishing destination. */
	error = write_route(irq, apic_id, 1);
	if (error == HAL_OK)
		irq_destination[irq] = apic_id;

	/* Returns the route-programming result unchanged. */
	return error;
}

/* Reads one selected volatile I/O APIC register. */
static uint32_t
read_reg_unlocked(
	struct ioapic_state *ioapic,
	uint8_t reg)
{
	uint32_t value;

	/* Publishes the selector before sampling the data window. */
	ioapic->base[0] = reg;
	hal_io_mb();
	value = ioapic->base[4];

	/* Returns the volatile register sample. */
	return value;
}

/* Writes one selected volatile I/O APIC register. */
static void
write_reg_unlocked(
	struct ioapic_state *ioapic,
	uint8_t reg,
	uint32_t value)
{
	/* Orders the selector before the data-window write. */
	ioapic->base[0] = reg;
	hal_io_mb();
	ioapic->base[4] = value;
	hal_io_mb();
}

/* Finds the controller and pin that own one global interrupt. */
static struct ioapic_state *
find_gsi(
	uint32_t gsi,
	unsigned *pin)
{
	unsigned index;

	/* Searches controllers in firmware discovery order. */
	for (index = 0; index < controller_count; index++) {
		/* Selects the controller whose range contains this GSI. */
		if (gsi >= controllers[index].gsi_base &&
		    gsi - controllers[index].gsi_base <
		    controllers[index].redirections) {
			*pin = gsi - controllers[index].gsi_base;

			/* Returns the first controller containing the GSI. */
			return &controllers[index];
		}
	}

	/* Reports a GSI outside every discovered controller. */
	return NULL;
}

/* Programs both halves of one I/O APIC redirection entry. */
static int
write_route(
	int irq,
	uint32_t apic_id,
	int masked)
{
	struct ioapic_state *ioapic;
	uint32_t low;
	uint16_t flags;
	unsigned pin;
	bool enabled;

	/* Builds the fixed vector before validating the route identity. */
	low = INT_IRQ_BASE + (uint32_t)irq;

	/* Validates the ISA line and xAPIC destination width. */
	if (irq < 0 || irq >= 16 || apic_id > 255U)
		return HAL_ERR_INVALID;

	/* Resolves the ACPI-translated GSI to a controller pin. */
	ioapic = find_gsi(irq_gsi[irq], &pin);
	if (ioapic == NULL)
		return HAL_ERR_UNSUPPORTED;

	/* Encodes ACPI polarity, trigger, and requested mask state. */
	flags = irq_flags[irq];
	if ((flags & 3U) == 3U)
		low |= 1U << 13;
	if (((flags >> 2) & 3U) == 3U)
		low |= 1U << 15;
	if (masked)
		low |= 1U << 16;

	/* Writes destination before control under the global MMIO lock. */
	enabled = hal_irq_disable();
	while (__atomic_exchange_n(
	    &ioapic_lock,
	    1U,
	    __ATOMIC_ACQUIRE) != 0) {
		__asm__ volatile("pause");
	}
	write_reg_unlocked(
		ioapic,
		(uint8_t)(0x11U + pin * 2U),
		apic_id << 24);
	write_reg_unlocked(
		ioapic,
		(uint8_t)(0x10U + pin * 2U),
		low);
	__atomic_store_n(&ioapic_lock, 0U, __ATOMIC_RELEASE);

	/* Restores interrupts only when they were previously enabled. */
	if (enabled)
		hal_irq_enable();

	/* Reports a completed redirection update. */
	return HAL_OK;
}
