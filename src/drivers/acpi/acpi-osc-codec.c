/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The _OSC negotiation's pure parts: the UUIDs of the interfaces, the
 * Capabilities Buffer an evaluation passes, the reading of the buffer the
 * firmware returns, and the request a commit makes after a query.
 *
 * Nothing here evaluates AML or touches the hardware.
 */

#include <uapi/errno.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "acpi-osc-codec.h"

/*
 * The size of one DWORD of the Capabilities Buffer.
 */
#define DWORD_SIZE	4U

/*
 * The DWORDs of the Capabilities Buffer by their place: the status, the
 * support field and the control field.
 */
#define DWORD_STATUS	0U
#define DWORD_SUPPORT	1U
#define DWORD_CONTROL	2U

/*
 * The status bits with which the firmware turns the whole request down:
 * it could not process it, or it does not know the UUID or the revision.
 *
 * Values referenced: ACPI 6.5 section 6.2.11 (bits 1, 2 and 3 of the
 * returned first DWORD).
 */
#define STATUS_REFUSALS	(DRV_ACPI_OSC_STATUS_FAILURE | \
			 DRV_ACPI_OSC_STATUS_UNKNOWN_UUID | \
			 DRV_ACPI_OSC_STATUS_UNKNOWN_REVISION)

/*
 * The platform-wide interface's UUID, 0811B06E-4A27-44F9-8D60-3CBBC22E7B48,
 * with its first three groups stored lowest byte first as ToUUID stores
 * them.
 *
 * Values referenced: ACPI 6.5 section 6.2.11.2 (the UUID) and section
 * 19.6.142, Table 19.41 (the byte order of a ToUUID buffer).
 */
const uint8_t drv_acpi_osc_platform_uuid[DRV_ACPI_OSC_UUID_LENGTH] = {
	0x6E, 0xB0, 0x11, 0x08, 0x27, 0x4A, 0xF9, 0x44,
	0x8D, 0x60, 0x3C, 0xBB, 0xC2, 0x2E, 0x7B, 0x48
};

/*
 * The processor interface's UUID, 4077A616-290C-47BE-9EBD-D87058713953,
 * in ToUUID order.
 *
 * Values referenced: ACPI 6.5 Appendix C, the _PDC example (the UUID it
 * passes to the processor's _OSC), and section 19.6.142, Table 19.41.
 */
const uint8_t drv_acpi_osc_processor_uuid[DRV_ACPI_OSC_UUID_LENGTH] = {
	0x16, 0xA6, 0x77, 0x40, 0x0C, 0x29, 0xBE, 0x47,
	0x9E, 0xBD, 0xD8, 0x70, 0x58, 0x71, 0x39, 0x53
};

/*
 * The PCI Express host bridge interface's UUID,
 * 33DB4D5B-1FF7-401C-9657-7441C03DD766, in ToUUID order.
 *
 * Values referenced: the Dell Latitude 5330's DSDT (BIOS 1.31.1),
 * \_SB.PC00._OSC (the UUID it accepts), and ACPI 6.5 section 19.6.142,
 * Table 19.41.
 */
const uint8_t drv_acpi_osc_pcie_uuid[DRV_ACPI_OSC_UUID_LENGTH] = {
	0x5B, 0x4D, 0xDB, 0x33, 0xF7, 0x1F, 0x1C, 0x40,
	0x96, 0x57, 0x74, 0x41, 0xC0, 0x3D, 0xD7, 0x66
};

static void put_dword(uint8_t *buffer, unsigned index, uint32_t value);
static uint32_t get_dword(const uint8_t *buffer, unsigned index);
static bool count_valid(uint32_t count);

/*
 * Writes the Capabilities Buffer of a request.
 *
 * The buffer gets the status DWORD (the query bit alone for a query, zero
 * for a commit), then the support field, then the control field when the
 * request carries one.  Reports the bytes written, or zero when the
 * request's DWORD count is not 2 or 3 or the buffer is too small.
 */
size_t
drv_acpi_osc_encode(
	const struct drv_acpi_osc_request *request,
	bool query,
	uint8_t *buffer,
	size_t size)
{
	size_t length;
	uint32_t status;
	bool valid;

	/* Refuses a request without a buffer to write. */
	if (request == NULL || buffer == NULL)
		return 0;

	/* Refuses a DWORD count other than the status with one or two fields. */
	valid = count_valid(request->count);
	if (!valid)
		return 0;

	/* Refuses a buffer that cannot hold every DWORD. */
	length = (size_t)request->count * DWORD_SIZE;
	if (size < length)
		return 0;

	/*
	 * The query bit asks what the firmware would grant without granting
	 * it; a commit clears it, and the other bits of the first DWORD are
	 * the firmware's to set.
	 */
	status = 0;
	if (query)
		status = DRV_ACPI_OSC_STATUS_QUERY;

	/* Writes the status and the support field. */
	put_dword(buffer, DWORD_STATUS, status);
	put_dword(buffer, DWORD_SUPPORT, request->support);

	/* Writes the control field of an interface that has one. */
	if (request->count > DWORD_CONTROL)
		put_dword(buffer, DWORD_CONTROL, request->control);

	/* Succeeded: reports the length of the Capabilities Buffer. */
	return length;
}

/*
 * Reads the Capabilities Buffer the firmware returned for a request.
 *
 * buffer is NULL when the evaluation returned something other than a
 * Buffer; that, and a buffer shorter than the DWORDs passed, is BROKEN.
 * A refusal in the status is REFUSED.  Otherwise the returned support and
 * control fields are cut to the bits asked, and the outcome is MASKED when
 * the firmware says it masked bits or kept fewer than asked, and GRANTED
 * when it kept them all.  Reports EINVAL for a request or a result that
 * cannot be read.
 */
int
drv_acpi_osc_decode(
	const struct drv_acpi_osc_request *request,
	const uint8_t *buffer,
	size_t length,
	struct drv_acpi_osc_result *result)
{
	uint32_t returned;
	bool valid;

	/* Refuses a missing request or result. */
	if (request == NULL || result == NULL)
		return EINVAL;

	/* Refuses a request whose DWORD count is not 2 or 3. */
	valid = count_valid(request->count);
	if (!valid)
		return EINVAL;

	/* Starts from nothing granted. */
	result->outcome = DRV_ACPI_OSC_BROKEN;
	result->status = 0;
	result->support = 0;
	result->control = 0;

	/* Something other than a Buffer grants nothing. */
	if (buffer == NULL)
		return 0;

	/* A buffer shorter than the DWORDs passed grants nothing. */
	if (length < (size_t)request->count * DWORD_SIZE)
		return 0;

	/* A refusal grants nothing. */
	result->status = get_dword(buffer, DWORD_STATUS);
	if ((result->status & STATUS_REFUSALS) != 0) {
		result->outcome = DRV_ACPI_OSC_REFUSED;
		return 0;
	}

	/* Keeps the support bits the firmware acknowledged of those asked. */
	returned = get_dword(buffer, DWORD_SUPPORT);
	result->support = returned & request->support;

	/* Keeps the control bits it acknowledged, for an interface that has them. */
	if (request->count > DWORD_CONTROL) {
		returned = get_dword(buffer, DWORD_CONTROL);
		result->control = returned & request->control;
	}

	/*
	 * MASKED: the firmware says it cleared bits, or it kept fewer than
	 * asked without saying so.  GRANTED: it kept every bit asked.
	 */
	if ((result->status & DRV_ACPI_OSC_STATUS_MASKED) != 0) {
		result->outcome = DRV_ACPI_OSC_MASKED;
	} else if (result->support != request->support) {
		result->outcome = DRV_ACPI_OSC_MASKED;
	} else if (result->control != request->control) {
		result->outcome = DRV_ACPI_OSC_MASKED;
	} else {
		result->outcome = DRV_ACPI_OSC_GRANTED;
	}

	/* Succeeded: result holds the outcome and the bits granted. */
	return 0;
}

/*
 * Makes the commit that follows a query.
 *
 * The commit asks for the bits both asked for and acknowledged by the
 * query, and is made even when that leaves nothing, so the firmware learns
 * that the operating system evaluates _OSC.  Reports false, with a commit
 * asking for nothing, when the query was not GRANTED or MASKED and no
 * commit is to follow.
 */
bool
drv_acpi_osc_commit_request(
	const struct drv_acpi_osc_request *asked,
	const struct drv_acpi_osc_result *query,
	struct drv_acpi_osc_request *commit)
{
	/* The commit names the same interface in the same form as the query. */
	commit->uuid = asked->uuid;
	commit->revision = asked->revision;
	commit->count = asked->count;
	commit->support = 0;
	commit->control = 0;

	/* A query the firmware refused, or that broke, is not followed by a commit. */
	if (query->outcome != DRV_ACPI_OSC_GRANTED && query->outcome != DRV_ACPI_OSC_MASKED)
		return false;

	/* Asks for what the query acknowledged, which decode cut to what was asked. */
	commit->support = query->support & asked->support;
	commit->control = query->control & asked->control;

	/* Succeeded: commit is the request to evaluate next. */
	return true;
}

/*
 * Names an outcome for the log.
 */
const char *
drv_acpi_osc_outcome_name(
	enum drv_acpi_osc_outcome outcome)
{
	/* Chooses the name by the outcome. */
	switch (outcome) {
	case DRV_ACPI_OSC_ABSENT:
		return "absent";
	case DRV_ACPI_OSC_GRANTED:
		return "granted";
	case DRV_ACPI_OSC_MASKED:
		return "masked";
	case DRV_ACPI_OSC_REFUSED:
		return "refused";
	case DRV_ACPI_OSC_BROKEN:
		return "broken";
	default:
		break;
	}

	/* A value outside the enumeration has no name. */
	return "unknown";
}

/*
 * Stores one DWORD of the Capabilities Buffer, lowest byte first.
 *
 * Value referenced: ACPI 6.5 section 19.3.5, Table 19.5 (an Integer is a
 * little-endian unsigned integer; CreateDWordField reads a buffer so).
 */
static void
put_dword(
	uint8_t *buffer,
	unsigned index,
	uint32_t value)
{
	uint8_t *dword;

	/* Finds the DWORD's first byte. */
	dword = buffer + (size_t)index * DWORD_SIZE;

	/* Stores the four bytes, lowest first. */
	dword[0] = (uint8_t)value;
	dword[1] = (uint8_t)(value >> 8);
	dword[2] = (uint8_t)(value >> 16);
	dword[3] = (uint8_t)(value >> 24);
}

/* Reads one DWORD of the Capabilities Buffer, lowest byte first. */
static uint32_t
get_dword(
	const uint8_t *buffer,
	unsigned index)
{
	const uint8_t *dword;
	uint32_t value;

	/* Finds the DWORD's first byte. */
	dword = buffer + (size_t)index * DWORD_SIZE;

	/* Assembles the four bytes, lowest first. */
	value = (uint32_t)dword[0];
	value |= (uint32_t)dword[1] << 8;
	value |= (uint32_t)dword[2] << 16;
	value |= (uint32_t)dword[3] << 24;

	/* Reports the DWORD. */
	return value;
}

/* Reports whether a DWORD count is the status with one or two fields. */
static bool
count_valid(
	uint32_t count)
{
	/* The status and the support field. */
	if (count == DRV_ACPI_OSC_PLATFORM_DWORDS)
		return true;

	/* The status, the support field and the control field. */
	if (count == DRV_ACPI_OSC_DWORDS_MAX)
		return true;

	/* Any other count is not a form this negotiation uses. */
	return false;
}
