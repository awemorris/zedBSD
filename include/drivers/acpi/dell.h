/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The Dell laptop's thermal provider over ACPI WMI.
 *
 * A Dell laptop's firmware offers, through WMI method blocks that its AML
 * implements: the fans' speeds read from the embedded controller (the
 * "DDV" interface), the BIOS thermal mode set through the SMBIOS calling
 * interface (a buffer the AML hands to System Management Mode), and the
 * legacy SMM fan calls.  The provider turns them into a fan of the thermal
 * core, a receiver of the strategy that picks the BIOS thermal mode, and,
 * when the kernel is built with CONFIG_DELL_SMM_BOOST, the fan's boost.
 *
 * The functions below the attach are the provider's pure parts: they only
 * read and write the bytes they are given.
 */

#ifndef KERN_DRIVERS_ACPI_DELL_H
#define KERN_DRIVERS_ACPI_DELL_H

#include <stddef.h>
#include <stdint.h>

#include <kern/thermal.h>

/*
 * The BIOS thermal modes, numbered as the firmware's ACPI variable that
 * mirrors the last mode asked for numbers them.  The BIOS setup calls the
 * first one "Optimized" and the last one "UltraPerformance".
 */
#define DRV_ACPI_DELL_MODE_OPTIMIZED	0U
#define DRV_ACPI_DELL_MODE_COOL		1U
#define DRV_ACPI_DELL_MODE_QUIET	2U
#define DRV_ACPI_DELL_MODE_PERFORMANCE	3U
#define DRV_ACPI_DELL_MODE_COUNT	4U

/* No mode: FIRMWARE, which writes nothing, or a mode not known. */
#define DRV_ACPI_DELL_MODE_NONE		0xffffffffU

/*
 * The size of the SMBIOS calling interface's buffer with no extension, and
 * of the four-register SMM call's buffer.
 */
#define DRV_ACPI_DELL_SMBIOS_SIZE	0x2CU
#define DRV_ACPI_DELL_SMM_SIZE		32U

/*
 * The highest state of a fan the SMM fan call sets on a machine not known
 * to have more (off, low, high).
 *
 * Value referenced: Linux 6.12 include/uapi/linux/i8k.h (the high fan
 * state, which dell-smm-hwmon.c takes as the highest by default).
 */
#define DRV_ACPI_DELL_FAN_STATE_HIGH	2U

/* The most fans the provider takes from the DDV fan information. */
#define DRV_ACPI_DELL_FANS_MAX		4U

/* One fan of the DDV fan information: its kind and its speed. */
struct drv_acpi_dell_fan {
	uint8_t type;
	uint16_t rpm;
};

int
drv_acpi_dell_attach(void);

uint32_t
drv_acpi_dell_mode_for(
	const struct kern_thermal_strategy *strategy);

const char *
drv_acpi_dell_mode_name(
	uint32_t mode);

int
drv_acpi_dell_fans_decode(
	uint64_t length,
	const uint8_t *bytes,
	size_t size,
	struct drv_acpi_dell_fan *fans,
	unsigned capacity,
	unsigned *count);

void
drv_acpi_dell_mode_request(
	uint32_t mode,
	uint8_t request[DRV_ACPI_DELL_SMBIOS_SIZE]);

int
drv_acpi_dell_mode_answer(
	const uint8_t *answer,
	size_t size,
	uint32_t *status);

void
drv_acpi_dell_fan_get_request(
	uint32_t fan,
	uint8_t request[DRV_ACPI_DELL_SMM_SIZE]);

void
drv_acpi_dell_fan_set_request(
	uint32_t fan,
	uint32_t state,
	uint8_t request[DRV_ACPI_DELL_SMM_SIZE]);

int
drv_acpi_dell_smm_answer(
	const uint8_t *answer,
	size_t size,
	const uint8_t request[DRV_ACPI_DELL_SMM_SIZE],
	uint32_t *eax);

#endif
