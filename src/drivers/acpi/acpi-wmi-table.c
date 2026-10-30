/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The decoding of WMI mapping data: the entries of a _WDG buffer, the text
 * form of a GUID, and the names of a block's control methods.
 *
 * Nothing here evaluates AML or touches the hardware; the functions only
 * read the bytes and the text they are given.
 */

#include <drivers/acpi/wmi.h>
#include <kern/kcrt.h>
#include <uapi/errno.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Where the dashes of a GUID's text form stand.
 */
#define GUID_DASH_1	8U
#define GUID_DASH_2	13U
#define GUID_DASH_3	18U
#define GUID_DASH_4	23U

/*
 * The offsets in one _WDG entry: the GUID, the two-character identifier
 * (or the Notify value of an event), the instance count and the flags.
 */
#define ENTRY_GUID		0U
#define ENTRY_ID		16U
#define ENTRY_INSTANCE_COUNT	18U
#define ENTRY_FLAGS		19U

static bool guid_dash_position(size_t position);
static int hex_digit(char character, uint8_t *value);
static bool name_character(uint8_t character);

/*
 * Converts the text form of a GUID into the bytes _WDG stores.
 *
 * The text is "XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX" in upper or lower
 * case.  Returns 0, or EINVAL when the text is not a GUID.
 */
int
drv_acpi_wmi_guid_parse(
	const char *text,
	uint8_t guid[DRV_ACPI_WMI_GUID_SIZE])
{
	uint8_t ordered[DRV_ACPI_WMI_GUID_SIZE];
	uint8_t high;
	uint8_t low;
	size_t length;
	size_t position;
	unsigned count;
	bool dash;
	int error;

	/* Refuses a text of another length. */
	length = kern_strlen(text);
	if (length != DRV_ACPI_WMI_GUID_TEXT_LENGTH)
		return EINVAL;

	/* Reads the 32 hexadecimal digits in the text's order, two to a byte, around the four dashes. */
	count = 0;
	position = 0;
	while (position < length) {
		/* A dash stands where the groups meet, and nowhere else. */
		dash = guid_dash_position(position);
		if (dash) {
			if (text[position] != '-')
				return EINVAL;
			position++;
			continue;
		}

		/* The byte's high digit. */
		error = hex_digit(text[position], &high);
		if (error != 0)
			return error;

		/* Its low digit, which a dash never splits from the high one. */
		error = hex_digit(text[position + 1U], &low);
		if (error != 0)
			return error;

		/* The byte, in the text's order. */
		ordered[count] = (uint8_t)((high << 4) | low);
		count++;
		position += 2U;
	}

	/*
	 * Value referenced: Microsoft, "GUID structure (guiddef.h)",
	 * learn.microsoft.com, 2022-08-12.  The first group of eight digits
	 * is Data1, an unsigned long; the next two groups of four are Data2
	 * and Data3, unsigned shorts; the last 16 digits are the eight bytes
	 * of Data4.  _WDG stores the structure as the processor does, the
	 * three integers little-endian (the _WDG example of "Windows
	 * Instrumentation: WMI and ACPI" lists 0x6a, 0x0f, 0xBC, 0xAB first).
	 */
	guid[0] = ordered[3];
	guid[1] = ordered[2];
	guid[2] = ordered[1];
	guid[3] = ordered[0];
	guid[4] = ordered[5];
	guid[5] = ordered[4];
	guid[6] = ordered[7];
	guid[7] = ordered[6];
	kern_memcpy(&guid[8], &ordered[8], 8U);

	/* Succeeded: guid holds the bytes in _WDG's order. */
	return 0;
}

/*
 * Decodes the entries of a _WDG buffer.
 *
 * At most capacity entries are written, in the buffer's order, and count
 * says how many.  Returns 0; EINVAL, with no entry, when the length is
 * not a whole number of entries; or E2BIG when the buffer has more entries
 * than capacity, after the first capacity of them.
 */
int
drv_acpi_wmi_entries_parse(
	const uint8_t *bytes,
	size_t length,
	struct drv_acpi_wmi_entry *entries,
	unsigned capacity,
	unsigned *count)
{
	struct drv_acpi_wmi_entry *entry;
	const uint8_t *source;
	size_t listed;
	unsigned index;

	/* Starts with no entry. */
	*count = 0;

	/* Refuses a buffer cut inside an entry. */
	if (length % DRV_ACPI_WMI_ENTRY_SIZE != 0U)
		return EINVAL;

	/*
	 * Values referenced: Microsoft, "Windows Instrumentation: WMI and
	 * ACPI" (learn.microsoft.com, dn614028, archived 2017-06-01),
	 * "ACPI Control Method Naming Conventions and Functionality": each
	 * entry is the GUID (16 bytes), the two-character object identifier
	 * or the Notify value and a reserved byte, the instance count, and
	 * the flags.  The document's structure gives the last two as USHORT,
	 * but its _WDG examples give one byte each, and so does firmware: an
	 * entry is 20 bytes.
	 */
	listed = length / DRV_ACPI_WMI_ENTRY_SIZE;
	for (index = 0; index < listed && index < capacity; index++) {
		source = bytes + (size_t)index * DRV_ACPI_WMI_ENTRY_SIZE;
		entry = &entries[index];
		kern_memcpy(entry->guid, source + ENTRY_GUID, DRV_ACPI_WMI_GUID_SIZE);
		entry->id[0] = source[ENTRY_ID];
		entry->id[1] = source[ENTRY_ID + 1U];
		entry->instance_count = source[ENTRY_INSTANCE_COUNT];
		entry->flags = source[ENTRY_FLAGS];
	}

	/* Says how many were decoded. */
	*count = index;

	/* Reports the entries that did not fit. */
	if (listed > capacity)
		return E2BIG;

	/* Succeeded: every entry is decoded. */
	return 0;
}

/*
 * Writes the name of a block's control method: the two prefix characters
 * (such as 'W' 'Q' for the query, 'W' 'M' for the method) and the block's
 * identifier, as a NUL-terminated name of four characters.
 *
 * Returns 0, or EINVAL for an event, whose identifier is a Notify value,
 * and for an identifier that cannot be part of an ACPI name.
 */
int
drv_acpi_wmi_control_name(
	const struct drv_acpi_wmi_entry *entry,
	char prefix0,
	char prefix1,
	char name[5])
{
	bool valid;

	/* Refuses an event: its identifier is a Notify value, not two characters. */
	if ((entry->flags & DRV_ACPI_WMI_FLAG_EVENT) != 0U)
		return EINVAL;

	/* Refuses an identifier with a character no ACPI name has. */
	valid = name_character(entry->id[0]);
	if (!valid)
		return EINVAL;
	valid = name_character(entry->id[1]);
	if (!valid)
		return EINVAL;

	/*
	 * Procedure referenced: Microsoft, "Windows Instrumentation: WMI and
	 * ACPI" (dn614028), "Control Method Summary": the control methods of
	 * a block are named WQxx, WSxx, WMxx and WCxx, where xx is the
	 * two-character identifier _WDG maps to the block's GUID.
	 */
	name[0] = prefix0;
	name[1] = prefix1;
	name[2] = (char)entry->id[0];
	name[3] = (char)entry->id[1];
	name[4] = '\0';

	/* Succeeded: name holds the control method's name. */
	return 0;
}

/* Tells whether a position of a GUID's text form holds a dash. */
static bool
guid_dash_position(
	size_t position)
{
	/* The four places where the groups meet. */
	if (position == GUID_DASH_1)
		return true;
	if (position == GUID_DASH_2)
		return true;
	if (position == GUID_DASH_3)
		return true;
	if (position == GUID_DASH_4)
		return true;

	/* A digit. */
	return false;
}

/* Converts one hexadecimal digit, in either case, to its value. */
static int
hex_digit(
	char character,
	uint8_t *value)
{
	/* A decimal digit, a lower-case letter, or an upper-case letter. */
	if (character >= '0' && character <= '9') {
		*value = (uint8_t)(character - '0');
	} else if (character >= 'a' && character <= 'f') {
		*value = (uint8_t)(character - 'a' + 10);
	} else if (character >= 'A' && character <= 'F') {
		*value = (uint8_t)(character - 'A' + 10);
	} else {
		/* Anything else is not a digit. */
		return EINVAL;
	}

	/* Succeeded: value holds the digit's value. */
	return 0;
}

/* Tells whether a byte may stand in an ACPI name segment (A-Z, 0-9, _). */
static bool
name_character(
	uint8_t character)
{
	/* An upper-case letter, a digit or the underscore. */
	if (character >= 'A' && character <= 'Z')
		return true;
	if (character >= '0' && character <= '9')
		return true;
	if (character == '_')
		return true;

	/* Anything else. */
	return false;
}
