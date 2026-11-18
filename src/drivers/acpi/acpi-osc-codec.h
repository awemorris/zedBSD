/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The pure parts of the _OSC negotiation: the UUIDs of the platform-wide,
 * processor and PCI Express host bridge interfaces, the bits the operating
 * system asks for, the Capabilities Buffer an evaluation passes, and the
 * checks of the buffer the firmware returns.
 *
 * Nothing here evaluates AML, so the host tests build it alone.
 */

#ifndef KERN_DRIVERS_ACPI_ACPI_OSC_CODEC_H
#define KERN_DRIVERS_ACPI_ACPI_OSC_CODEC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * The length of a UUID as ToUUID lays it out in a Buffer.
 */
#define DRV_ACPI_OSC_UUID_LENGTH	16U

/*
 * The most DWORDs a Capabilities Buffer the negotiation builds holds: the
 * status, the support field and the control field.
 */
#define DRV_ACPI_OSC_DWORDS_MAX		3U

/*
 * The first DWORD of the Capabilities Buffer.  The operating system sets
 * the query bit to ask without taking anything; the firmware answers with
 * the failure, unrecognized UUID, unrecognized revision and capabilities
 * masked bits.
 *
 * Values referenced: ACPI 6.5 section 6.2.11 (the first DWORD of the
 * Capabilities Buffer, as passed and as returned).
 */
#define DRV_ACPI_OSC_STATUS_QUERY		0x00000001U
#define DRV_ACPI_OSC_STATUS_FAILURE		0x00000002U
#define DRV_ACPI_OSC_STATUS_UNKNOWN_UUID	0x00000004U
#define DRV_ACPI_OSC_STATUS_UNKNOWN_REVISION	0x00000008U
#define DRV_ACPI_OSC_STATUS_MASKED		0x00000010U

/*
 * The platform-wide interface (\_SB._OSC): its revision, its DWORD count
 * and the support bits that say the operating system reads the _CPC
 * object and its revision 2.
 *
 * Values referenced: ACPI 6.5 section 6.2.11.2 (the arguments) and
 * Table 6.13 (bit 5, CPPC Support; bit 6, CPPC 2 Support).
 */
#define DRV_ACPI_OSC_PLATFORM_REVISION		1U
#define DRV_ACPI_OSC_PLATFORM_DWORDS		2U
#define DRV_ACPI_OSC_PLATFORM_CPPC		0x00000020U
#define DRV_ACPI_OSC_PLATFORM_CPPC2		0x00000040U

/*
 * The processor interface: its revision and DWORD count.  Its support
 * bits are the processor vendor's; the operating system asks for none.
 *
 * Values referenced: ACPI 6.5 Appendix C, the _PDC example (the revision
 * and the two DWORDs it passes to the processor's _OSC).
 */
#define DRV_ACPI_OSC_PROCESSOR_REVISION		1U
#define DRV_ACPI_OSC_PROCESSOR_DWORDS		2U

/*
 * The PCI Express host bridge interface: its revision, its DWORD count,
 * the support bits (extended configuration space, ASPM, clock power
 * management, segment groups, MSI) and the control bits (native PCI
 * Express hot plug, SHPC hot plug, PME, AER, the PCI Express Capability
 * structure, LTR, DPC).
 *
 * Values referenced: Linux v6.12.5 include/linux/acpi.h (the PCI host
 * bridge _OSC support and control DWORD bits).
 * Values referenced: the Dell Latitude 5330's DSDT (BIOS 1.31.1),
 * \_SB.PC00._OSC (the three DWORDs, revision 1, and the control bits 0 to
 * 3 it drops).
 */
#define DRV_ACPI_OSC_PCIE_REVISION		1U
#define DRV_ACPI_OSC_PCIE_DWORDS		3U
#define DRV_ACPI_OSC_PCIE_EXTENDED_CONFIG	0x00000001U
#define DRV_ACPI_OSC_PCIE_ASPM			0x00000002U
#define DRV_ACPI_OSC_PCIE_CLOCK_PM		0x00000004U
#define DRV_ACPI_OSC_PCIE_SEGMENTS		0x00000008U
#define DRV_ACPI_OSC_PCIE_MSI			0x00000010U
#define DRV_ACPI_OSC_PCIE_NATIVE_HOTPLUG	0x00000001U
#define DRV_ACPI_OSC_PCIE_SHPC_HOTPLUG		0x00000002U
#define DRV_ACPI_OSC_PCIE_PME			0x00000004U
#define DRV_ACPI_OSC_PCIE_AER			0x00000008U
#define DRV_ACPI_OSC_PCIE_CAPABILITY		0x00000010U
#define DRV_ACPI_OSC_PCIE_LTR			0x00000020U
#define DRV_ACPI_OSC_PCIE_DPC			0x00000080U

/*
 * What one _OSC negotiation came to.
 *
 * ABSENT: the device has no _OSC.  GRANTED: the firmware acknowledged
 * every bit asked.  MASKED: it acknowledged fewer bits than asked (the
 * bits it kept are the operating system's).  REFUSED: it reported a
 * failure, an unrecognized UUID or an unrecognized revision, and nothing
 * is granted.  BROKEN: the evaluation failed, or what came back is not a
 * buffer of the DWORDs passed, and nothing is granted.
 */
enum drv_acpi_osc_outcome {
	DRV_ACPI_OSC_ABSENT = 0,
	DRV_ACPI_OSC_GRANTED,
	DRV_ACPI_OSC_MASKED,
	DRV_ACPI_OSC_REFUSED,
	DRV_ACPI_OSC_BROKEN
};

/*
 * One request to a device's _OSC: which interface (the UUID in ToUUID
 * order and its revision), how many DWORDs the Capabilities Buffer holds
 * (2 without a control field, 3 with one), and the support and control
 * fields asked for.  The UUID is borrowed and outlives the request.
 */
struct drv_acpi_osc_request {
	const uint8_t *uuid;
	uint32_t revision;
	uint32_t count;
	uint32_t support;
	uint32_t control;
};

/*
 * What a returned Capabilities Buffer says: the outcome, the status DWORD
 * as returned, and the support and control fields cut to the bits the
 * request asked for (zero for a field the request did not carry, and both
 * zero unless the outcome is GRANTED or MASKED).
 */
struct drv_acpi_osc_result {
	enum drv_acpi_osc_outcome outcome;
	uint32_t status;
	uint32_t support;
	uint32_t control;
};

/*
 * The UUIDs of the three interfaces, in ToUUID order.
 */
extern const uint8_t drv_acpi_osc_platform_uuid[DRV_ACPI_OSC_UUID_LENGTH];
extern const uint8_t drv_acpi_osc_processor_uuid[DRV_ACPI_OSC_UUID_LENGTH];
extern const uint8_t drv_acpi_osc_pcie_uuid[DRV_ACPI_OSC_UUID_LENGTH];

size_t
drv_acpi_osc_encode(
	const struct drv_acpi_osc_request *request,
	bool query,
	uint8_t *buffer,
	size_t size);

int
drv_acpi_osc_decode(
	const struct drv_acpi_osc_request *request,
	const uint8_t *buffer,
	size_t length,
	struct drv_acpi_osc_result *result);

bool
drv_acpi_osc_commit_request(
	const struct drv_acpi_osc_request *asked,
	const struct drv_acpi_osc_result *query,
	struct drv_acpi_osc_request *commit);

const char *
drv_acpi_osc_outcome_name(
	enum drv_acpi_osc_outcome outcome);

#endif
