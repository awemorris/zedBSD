/* -*- mode: c; c-basic-offset: 8; indent-tabs-mode: t; tab-width: 8 -*- */

/*
 * zedBSD Intel AX211 PCI/CNVio2 transport
 * Copyright (C) 2026 Awe Morris
 * SPDX-License-Identifier: Zlib
 */

#include <drivers/pci-intel-ax211.h>
#include <drivers/pci.h>

#include <errno.h>
#include <hal/hal.h>
#include <stdint.h>
#include <string.h>

#define AX211_PCI_VENDOR		0x8086U
#define AX211_PCI_PRODUCT		0x51f0U
#define AX211_PCI_SUBVENDOR		0x8086U
#define AX211_PCI_SUBPRODUCT		0x4090U
#define AX211_PCI_REVISION		0x01U
#define AX211_PCI_CLASS			0x028000U
#define AX211_PCI_CLASS_MASK		0xffffffU
#define AX211_PCI_COMMAND		0x04U
#define AX211_PCI_COMMAND_MEMORY	0x0002U
#define AX211_PCI_COMMAND_MASTER	0x0004U
#define AX211_BAR0_MINIMUM_SIZE		0x4000U

/*
 * These are stable device register encodings, not a copied implementation.
 * HW_REV bits 15:4 contain the MAC type. HW_RF_ID bits 23:12 contain the RF
 * type. The frozen q061 target is an So-family MAC with a Garfield Peak RF
 * module.
 */
#define AX211_CSR_HW_REV			0x028U
#define AX211_CSR_HW_RF_ID		0x09cU
#define AX211_CSR_HW_REV_TYPE_MASK	0x0000fff0U
#define AX211_CSR_HW_REV_TYPE_SHIFT	4U
#define AX211_CSR_HW_REV_TYPE_SO	0x037U
#define AX211_CSR_HW_REV_TYPE_SOF	0x043U
#define AX211_CSR_HW_RF_TYPE_MASK	0x00fff000U
#define AX211_CSR_HW_RF_TYPE_SHIFT	12U
#define AX211_CSR_HW_RF_TYPE_GF		0x10dU
#define AX211_CSR_HW_RF_CDB		0x10000000U

struct ax211_pci_inspection {
	struct drv_pci_device *device;
	struct drv_pci_mapping mapping;
	struct drv_pci_enable_state enable_state;
	struct drv_pci_bar original_bar;
	uint32_t hardware_revision;
	uint32_t radio_identity;
	unsigned bar_claimed;
	unsigned bar_mapped;
	unsigned bar_may_have_moved;
	unsigned original_bar_valid;
	unsigned state_saved;
};

static int ax211_pci_match(struct drv_pci_device *device,
	const struct drv_pci_id *identity);
static int ax211_pci_attach(struct drv_pci_device *device,
	const struct drv_pci_id *identity);
static int ax211_pci_detach(struct drv_pci_device *device, unsigned flags);
static int ax211_pci_identity_matches(const struct drv_pci_device *device);
static int ax211_pci_bar_validate(const struct drv_pci_bar *bar);
static uint32_t ax211_pci_read32(const struct ax211_pci_inspection *inspection,
	unsigned offset);
static int ax211_pci_hardware_validate(uint32_t hardware_revision,
	uint32_t radio_identity);
static int ax211_pci_restore_bar(struct ax211_pci_inspection *inspection);
static int ax211_pci_release(struct ax211_pci_inspection *inspection,
	int result);
static int ax211_pci_inspect(struct ax211_pci_inspection *inspection);

static const struct drv_pci_id ax211_pci_ids[] = {
	{
		.vendor = AX211_PCI_VENDOR,
		.device = AX211_PCI_PRODUCT,
		.subvendor = AX211_PCI_SUBVENDOR,
		.subdevice = AX211_PCI_SUBPRODUCT,
		.class_code = AX211_PCI_CLASS,
		.class_mask = AX211_PCI_CLASS_MASK,
	},
};

static struct drv_pci_driver ax211_pci_driver = {
	.name = "intel-ax211",
	.ids = ax211_pci_ids,
	.id_count = sizeof(ax211_pci_ids) / sizeof(ax211_pci_ids[0]),
	.match = ax211_pci_match,
	.attach = ax211_pci_attach,
	.detach = ax211_pci_detach,
};

/*
 * Registers the exact Intel AX211 PCI transport driver.
 */
int
drv_pci_intel_ax211_driver_register(void)
{
	return drv_pci_driver_register(&ax211_pci_driver);
}

/* Matches only the frozen q061 PCI identity. */
static int
ax211_pci_match(
	struct drv_pci_device *device,
	const struct drv_pci_id *identity)
{
	(void)identity;

	/* Rejects every neighboring revision and identity. */
	if (!ax211_pci_identity_matches(device))
		return DRV_PCI_MATCH_NONE;

	return DRV_PCI_MATCH_EXACT;
}

/* Inspects the exact device without resetting it or starting DMA. */
static int
ax211_pci_attach(
	struct drv_pci_device *device,
	const struct drv_pci_id *identity)
{
	struct ax211_pci_inspection inspection;
	struct drv_pci_address address;
	int error;

	(void)identity;

	/* Refuses a direct attach which bypassed the driver's match callback. */
	if (!ax211_pci_identity_matches(device))
		return ENODEV;

	/* Builds a private, unpublished inspection lease. */
	memset(&inspection, 0, sizeof(inspection));
	inspection.device = device;

	/* Acquires and inspects only BAR0 and the two read-only identity CSRs. */
	error = ax211_pci_inspect(&inspection);
	if (error != 0)
		return ax211_pci_release(&inspection, error);

	/* Reports the bounded milestone without publishing a WLAN device. */
	drv_pci_device_address(device, &address);
	hal_printf(
	    "intel-ax211: pci %04x:%02x:%02x.%u SO-family/GF detected hw-rev=%08x rf-id=%08x; firmware transport unavailable\n",
	    address.segment, address.bus, address.device, address.function,
	    inspection.hardware_revision, inspection.radio_identity);

	/* Restores the inherited PCI state before declining the binding. */
	return ax211_pci_release(&inspection, ENOTSUP);
}

/* Handles a defensive detach for a driver which publishes no device yet. */
static int
ax211_pci_detach(
	struct drv_pci_device *device,
	unsigned flags)
{
	(void)device;
	(void)flags;

	return 0;
}

/* Checks the complete frozen q061 PCI tuple. */
static int
ax211_pci_identity_matches(
	const struct drv_pci_device *device)
{
	/* Rejects a missing device before using its accessors. */
	if (device == NULL)
		return 0;

	/* Checks the vendor and product pair. */
	if (drv_pci_device_vendor(device) != AX211_PCI_VENDOR)
		return 0;
	if (drv_pci_device_product(device) != AX211_PCI_PRODUCT)
		return 0;

	/* Checks the exact subsystem identity. */
	if (drv_pci_device_subvendor(device) != AX211_PCI_SUBVENDOR)
		return 0;
	if (drv_pci_device_subproduct(device) != AX211_PCI_SUBPRODUCT)
		return 0;

	/* Checks the exact revision and network-controller class. */
	if (drv_pci_device_revision(device) != AX211_PCI_REVISION)
		return 0;
	if ((drv_pci_device_class(device) & AX211_PCI_CLASS_MASK) !=
	    AX211_PCI_CLASS)
		return 0;

	return 1;
}

/* Validates BAR0 before the PCI command register or MMIO is changed. */
static int
ax211_pci_bar_validate(
	const struct drv_pci_bar *bar)
{
	/* Requires a memory BAR large enough for the AX211 CSR aperture. */
	if (bar == NULL)
		return EINVAL;
	if (bar->type != DRV_PCI_BAR_MEMORY32 &&
	    bar->type != DRV_PCI_BAR_MEMORY64)
		return ENODEV;
	if (bar->size < AX211_BAR0_MINIMUM_SIZE)
		return ENODEV;

	return 0;
}

/* Reads one naturally aligned CSR from the validated BAR0 mapping. */
static uint32_t
ax211_pci_read32(
	const struct ax211_pci_inspection *inspection,
	unsigned offset)
{
	volatile uint8_t *registers;
	uint32_t value;

	registers = inspection->mapping.address;
	value = *(volatile uint32_t *)(registers + offset);
	hal_io_rmb();

	return value;
}

/* Validates the frozen So-family MAC and Garfield Peak RF identities. */
static int
ax211_pci_hardware_validate(
	uint32_t hardware_revision,
	uint32_t radio_identity)
{
	uint32_t mac_type;
	uint32_t radio_type;

	mac_type = (hardware_revision & AX211_CSR_HW_REV_TYPE_MASK) >>
	    AX211_CSR_HW_REV_TYPE_SHIFT;
	radio_type = (radio_identity & AX211_CSR_HW_RF_TYPE_MASK) >>
	    AX211_CSR_HW_RF_TYPE_SHIFT;

	/* Rejects a neighboring Intel MAC or companion RF module. */
	if (mac_type != AX211_CSR_HW_REV_TYPE_SO &&
	    mac_type != AX211_CSR_HW_REV_TYPE_SOF)
		return ENODEV;
	if (radio_type != AX211_CSR_HW_RF_TYPE_GF)
		return ENODEV;

	/* The frozen integrated CNVio2 target is not a CDB radio. */
	if ((radio_identity & AX211_CSR_HW_RF_CDB) != 0U)
		return ENODEV;

	return 0;
}

/* Restores BAR0 if the host mapper assigned a temporary MMIO address. */
static int
ax211_pci_restore_bar(
	struct ax211_pci_inspection *inspection)
{
	struct drv_pci_bar current_bar;
	int error;

	/* Skips a BAR which could not have been changed by the mapper. */
	if (!inspection->bar_may_have_moved)
		return 0;
	if (!inspection->original_bar_valid)
		return EIO;

	/* Compares the current address with the pre-map snapshot. */
	error = drv_pci_device_bar(inspection->device, 0U, &current_bar);
	if (error != 0)
		return error;
	if (current_bar.bus_address != inspection->original_bar.bus_address) {
		error = drv_pci_device_assign_bar(inspection->device, 0U,
		    inspection->original_bar.bus_address);
		if (error != 0)
			return error;
	}

	inspection->bar_may_have_moved = 0;
	inspection->original_bar_valid = 0;
	return 0;
}

/* Releases the temporary inspection lease in exact reverse order. */
static int
ax211_pci_release(
	struct ax211_pci_inspection *inspection,
	int result)
{
	int bar_error;
	int quiesce_error;
	int restore_error;

	bar_error = 0;
	restore_error = 0;

	/* Removes the CPU mapping before restoring PCI decode state. */
	if (inspection->bar_mapped) {
		drv_pci_device_unmap_bar(inspection->device,
		    &inspection->mapping);
		inspection->bar_mapped = 0;
	}

	/* Restores an address which the platform mapper may have reassigned. */
	bar_error = ax211_pci_restore_bar(inspection);

	/* Restores the inherited I/O, memory, and bus-master enable bits. */
	if (inspection->state_saved && bar_error == 0) {
		restore_error = drv_pci_device_restore_enable_state(
		    inspection->device, &inspection->enable_state);
		if (restore_error == 0) {
			inspection->state_saved = 0;
		} else {
			/* Leaves DMA disabled when the exact restoration cannot be proven. */
			quiesce_error = drv_pci_device_set_bus_master(
			    inspection->device, false);
			if (quiesce_error != 0)
				hal_printf(
				    "intel-ax211: PCI restore failed (%d), bus-master quiesce failed (%d)\n",
				    restore_error, quiesce_error);
		}
	} else if (inspection->state_saved) {
		/* Keeps DMA disabled when BAR restoration itself failed. */
		quiesce_error = drv_pci_device_set_bus_master(
		    inspection->device, false);
		if (quiesce_error != 0)
			hal_printf(
			    "intel-ax211: BAR restore failed (%d), bus-master quiesce failed (%d)\n",
			    bar_error, quiesce_error);
	}

	/* Releases BAR ownership last. */
	if (inspection->bar_claimed) {
		drv_pci_device_release_bar(inspection->device, 0U);
		inspection->bar_claimed = 0;
	}

	/* Makes a failed restoration more important than the inspection result. */
	if (bar_error != 0)
		return bar_error;
	if (restore_error != 0)
		return restore_error;

	return result;
}

/* Acquires BAR0 and validates read-only hardware identity CSRs. */
static int
ax211_pci_inspect(
	struct ax211_pci_inspection *inspection)
{
	struct drv_pci_bar bar;
	uint16_t command;
	int error;

	/* Claims BAR0 before inspecting or mapping it. */
	error = drv_pci_device_claim_bar(inspection->device, 0U);
	if (error != 0)
		return error;
	inspection->bar_claimed = 1;

	/* Validates the immutable BAR type and minimum aperture size. */
	error = drv_pci_device_bar(inspection->device, 0U, &bar);
	if (error != 0)
		return error;
	error = ax211_pci_bar_validate(&bar);
	if (error != 0)
		return error;
	inspection->original_bar = bar;
	inspection->original_bar_valid = 1;

	/* Saves the inherited decode and DMA enable state. */
	error = drv_pci_device_save_enable_state(inspection->device,
	    &inspection->enable_state);
	if (error != 0)
		return error;
	inspection->state_saved = 1;

	/* Disables bus mastering before mapping or reading any device CSR. */
	error = drv_pci_device_set_bus_master(inspection->device, false);
	if (error != 0)
		return error;

	/* Maps BAR0 read-only and records that the host may reassign its address. */
	inspection->bar_may_have_moved = 1;
	error = drv_pci_device_map_bar(inspection->device, 0U,
	    DRV_PCI_MAP_READ | DRV_PCI_MAP_NOCACHE, &inspection->mapping);
	if (error != 0)
		return error;
	inspection->bar_mapped = 1;

	/* Rejects a truncated or otherwise inconsistent host mapping. */
	if (inspection->mapping.address == NULL)
		return EIO;
	if (inspection->mapping.type != DRV_PCI_BAR_MEMORY32 &&
	    inspection->mapping.type != DRV_PCI_BAR_MEMORY64)
		return EIO;
	if (inspection->mapping.size < AX211_BAR0_MINIMUM_SIZE)
		return EIO;
	if (inspection->mapping.size > bar.size)
		return EIO;

	/* Enables only memory decode and proves bus mastering remains disabled. */
	error = drv_pci_device_enable_memory(inspection->device);
	if (error != 0)
		return error;
	error = drv_pci_device_config_read16(inspection->device,
	    AX211_PCI_COMMAND, &command);
	if (error != 0)
		return error;
	if ((command & AX211_PCI_COMMAND_MEMORY) == 0U)
		return EIO;
	if ((command & AX211_PCI_COMMAND_MASTER) != 0U)
		return EIO;

	/* Reads only the MAC and RF identity CSRs. */
	inspection->hardware_revision = ax211_pci_read32(inspection,
	    AX211_CSR_HW_REV);
	inspection->radio_identity = ax211_pci_read32(inspection,
	    AX211_CSR_HW_RF_ID);

	return ax211_pci_hardware_validate(inspection->hardware_revision,
	    inspection->radio_identity);
}
