/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The host unit test of the WMI mapping decoder (acpi-wmi-table.c): the
 * text form of a GUID, the entries of a _WDG buffer, and the names of a
 * block's control methods.  The expected values are the example _WDG of
 * Microsoft's "Windows Instrumentation: WMI and ACPI" and the _WDG of
 * \_SB.AMW0 in the Dell Latitude 5330's DSDT (BIOS 1.31.1).
 *
 * The last line is "test-wmi-table: PASS (N checks)" or
 * "test-wmi-table: FAIL (M of N checks)".
 */

#include <stdio.h>
#include <string.h>

#include <drivers/acpi/wmi.h>
#include <uapi/errno.h>

/*
 * The example _WDG of the document: a data block "BA" (3 instances,
 * EXPENSIVE), a method block "BB" (3 instances, METHOD) and an event with
 * the Notify value 0xB0 (EVENT).
 */
static const unsigned char example_wdg[60] = {
	0x6a, 0x0f, 0xbc, 0xab, 0xa1, 0x8e, 0xd1, 0x11, 0x00, 0xa0, 0xc9, 0x06, 0x29, 0x10, 0x00, 0x00,
	'B', 'A', 3, 0x01,
	0x6b, 0x0f, 0xbc, 0xab, 0xa1, 0x8e, 0xd1, 0x11, 0x00, 0xa0, 0xc9, 0x06, 0x29, 0x10, 0x00, 0x00,
	'B', 'B', 3, 0x02,
	0x6c, 0x0f, 0xbc, 0xab, 0xa1, 0x8e, 0xd1, 0x11, 0x00, 0xa0, 0xc9, 0x06, 0x29, 0x10, 0x00, 0x00,
	0xb0, 0x00, 1, 0x08
};

/*
 * The Latitude 5330's \_SB.AMW0._WDG (Buffer 0x8C), seven entries.
 */
static const unsigned char amw0_wdg[0x8c] = {
	0xbc, 0xdc, 0x9d, 0x8d, 0x97, 0xa9, 0xda, 0x11, 0xb0, 0x12, 0xb6, 0x22, 0xa1, 0xef, 0x54, 0x92,
	0x41, 0x41, 0x01, 0x00, 0x5f, 0x96, 0x32, 0x69, 0x71, 0x16, 0xeb, 0x4c, 0xb9, 0x88, 0xd3, 0xab,
	0x0a, 0x90, 0x19, 0x19, 0x44, 0x53, 0x01, 0x00, 0xce, 0x93, 0x05, 0xa8, 0x97, 0xa9, 0xda, 0x11,
	0xb0, 0x12, 0xb6, 0x22, 0xa1, 0xef, 0x54, 0x92, 0x42, 0x41, 0x01, 0x02, 0x94, 0x59, 0xbb, 0x9d,
	0x97, 0xa9, 0xda, 0x11, 0xb0, 0x12, 0xb6, 0x22, 0xa1, 0xef, 0x54, 0x92, 0xd0, 0x00, 0x01, 0x08,
	0xe0, 0x6c, 0x77, 0xa3, 0x88, 0x1e, 0xdb, 0x11, 0xa9, 0x8b, 0x08, 0x00, 0x20, 0x0c, 0x9a, 0x66,
	0x42, 0x43, 0x01, 0x00, 0x8a, 0xf0, 0x21, 0x5f, 0x6b, 0xf1, 0x0d, 0x46, 0xa2, 0x99, 0x7c, 0xfd,
	0x5b, 0x63, 0x45, 0xbc, 0x50, 0x53, 0x01, 0x02, 0x21, 0x12, 0x90, 0x05, 0x66, 0xd5, 0xd1, 0x11,
	0xb2, 0xf0, 0x00, 0xa0, 0xc9, 0x06, 0x29, 0x10, 0x4d, 0x4f, 0x01, 0x00
};

/* The number of checks run and failed. */
static unsigned checks;
static unsigned failures;

static void check(int passed, const char *what);
static void test_guid(void);
static void test_entries(void);
static void test_names(void);

/*
 * Runs every check and prints the verdict.
 */
int
main(void)
{
	/* The three parts of the decoder. */
	test_guid();
	test_entries();
	test_names();

	/* The verdict. */
	if (failures != 0) {
		printf("test-wmi-table: FAIL (%u of %u checks)\n", failures, checks);
		return 1;
	}

	/* Every check passed. */
	printf("test-wmi-table: PASS (%u checks)\n", checks);
	return 0;
}

/* Counts one check and prints it when it failed. */
static void
check(
	int passed,
	const char *what)
{
	/* Counts it. */
	checks++;
	if (passed)
		return;

	/* Reports the failure. */
	failures++;
	printf("FAIL: %s\n", what);
}

/* Checks the text form of a GUID. */
static void
test_guid(void)
{
	static const char *refused[] = {
		"",
		"ABBC0F6A-8EA1-11D1-00A0-C9062910000",
		"ABBC0F6A-8EA1-11D1-00A0-C906291000000",
		"ABBC0F6A08EA1-11D1-00A0-C90629100000",
		"ABBC0F6-A8EA1-11D1-00A0-C90629100000",
		"ABBC0F6A-8EA1-11D1-00A0-C9062910000G",
		"ABBC0F6A-8EA1-11D1-00A0-C9062910 000",
		"ABBC0F6A-8EA1-11D1-00A0-C9062910-000",
		"ABBC0F6A-8EA1-11D1-00A0+C90629100000"
	};
	unsigned char guid[DRV_ACPI_WMI_GUID_SIZE];
	unsigned index;
	int error;

	/* The document's example GUID, upper case, gives the bytes its _WDG lists. */
	memset(guid, 0, sizeof(guid));
	error = drv_acpi_wmi_guid_parse("ABBC0F6A-8EA1-11D1-00A0-C90629100000", guid);
	check(error == 0, "guid: the example parses");
	check(memcmp(guid, example_wdg, 16) == 0, "guid: the example's bytes are _WDG's");

	/* The same in lower case. */
	memset(guid, 0, sizeof(guid));
	error = drv_acpi_wmi_guid_parse("abbc0f6a-8ea1-11d1-00a0-c90629100000", guid);
	check(error == 0, "guid: lower case parses");
	check(memcmp(guid, example_wdg, 16) == 0, "guid: lower case gives the same bytes");

	/* The Latitude 5330's Dell descriptor block (AMW0 "AA"). */
	memset(guid, 0, sizeof(guid));
	error = drv_acpi_wmi_guid_parse("8D9DDCBC-A997-11DA-B012-B622A1EF5492", guid);
	check(error == 0, "guid: 8D9DDCBC parses");
	check(memcmp(guid, amw0_wdg, 16) == 0, "guid: 8D9DDCBC is AMW0's first entry");

	/* Its SMBIOS call block (AMW0 "BA"), the third entry. */
	memset(guid, 0, sizeof(guid));
	error = drv_acpi_wmi_guid_parse("A80593CE-A997-11DA-B012-B622A1EF5492", guid);
	check(error == 0, "guid: A80593CE parses");
	check(memcmp(guid, amw0_wdg + 40, 16) == 0, "guid: A80593CE is AMW0's third entry");

	/* Texts that are not GUIDs are refused. */
	for (index = 0; index < sizeof(refused) / sizeof(refused[0]); index++) {
		error = drv_acpi_wmi_guid_parse(refused[index], guid);
		check(error == EINVAL, refused[index]);
	}
}

/* Checks the decoding of _WDG entries. */
static void
test_entries(void)
{
	static const char amw0_ids[7][3] = { "AA", "DS", "BA", "", "BC", "PS", "MO" };
	static const unsigned char amw0_flags[7] = { 0x00, 0x00, 0x02, 0x08, 0x00, 0x02, 0x00 };
	struct drv_acpi_wmi_entry entries[8];
	unsigned count;
	unsigned index;
	int error;

	/* The document's example: three entries. */
	memset(entries, 0, sizeof(entries));
	error = drv_acpi_wmi_entries_parse(example_wdg, sizeof(example_wdg), entries, 8, &count);
	check(error == 0, "entries: the example parses");
	check(count == 3, "entries: the example has 3");
	check(memcmp(entries[0].guid, example_wdg, 16) == 0, "entries: BA's GUID");
	check(entries[0].id[0] == 'B' && entries[0].id[1] == 'A', "entries: BA's identifier");
	check(entries[0].instance_count == 3, "entries: BA has 3 instances");
	check(entries[0].flags == DRV_ACPI_WMI_FLAG_EXPENSIVE, "entries: BA is EXPENSIVE");
	check(entries[1].id[0] == 'B' && entries[1].id[1] == 'B', "entries: BB's identifier");
	check(entries[1].instance_count == 3, "entries: BB has 3 instances");
	check(entries[1].flags == DRV_ACPI_WMI_FLAG_METHOD, "entries: BB is METHOD");
	check(memcmp(entries[2].guid, example_wdg + 40, 16) == 0, "entries: the event's GUID");
	check(entries[2].id[0] == 0xb0, "entries: the event's Notify value");
	check(entries[2].instance_count == 1, "entries: the event has 1 instance");
	check(entries[2].flags == DRV_ACPI_WMI_FLAG_EVENT, "entries: the event is EVENT");

	/* The Latitude 5330's AMW0: seven entries with their identifiers and flags. */
	memset(entries, 0, sizeof(entries));
	error = drv_acpi_wmi_entries_parse(amw0_wdg, sizeof(amw0_wdg), entries, 8, &count);
	check(error == 0, "entries: AMW0 parses");
	check(count == 7, "entries: AMW0 has 7");
	for (index = 0; index < 7; index++) {
		check(entries[index].flags == amw0_flags[index], "entries: AMW0's flags");
		check(entries[index].instance_count == 1, "entries: AMW0's instance counts");
		if (amw0_flags[index] == DRV_ACPI_WMI_FLAG_EVENT) {
			check(entries[index].id[0] == 0xd0, "entries: AMW0's event is Notify 0xD0");
		} else {
			check(entries[index].id[0] == (unsigned char)amw0_ids[index][0], "entries: AMW0's identifiers (first)");
			check(entries[index].id[1] == (unsigned char)amw0_ids[index][1], "entries: AMW0's identifiers (second)");
		}
	}

	/* An empty _WDG has no entry. */
	count = 99;
	error = drv_acpi_wmi_entries_parse(example_wdg, 0, entries, 8, &count);
	check(error == 0, "entries: an empty _WDG parses");
	check(count == 0, "entries: an empty _WDG has none");

	/* A _WDG cut inside an entry is refused whole. */
	count = 99;
	error = drv_acpi_wmi_entries_parse(example_wdg, sizeof(example_wdg) - 1, entries, 8, &count);
	check(error == EINVAL, "entries: 59 bytes are refused");
	check(count == 0, "entries: 59 bytes give no entry");
	error = drv_acpi_wmi_entries_parse(example_wdg, 19, entries, 8, &count);
	check(error == EINVAL, "entries: 19 bytes are refused");
	error = drv_acpi_wmi_entries_parse(example_wdg, 21, entries, 8, &count);
	check(error == EINVAL, "entries: 21 bytes are refused");

	/* More entries than room: the first ones, and E2BIG. */
	memset(entries, 0, sizeof(entries));
	error = drv_acpi_wmi_entries_parse(example_wdg, sizeof(example_wdg), entries, 2, &count);
	check(error == E2BIG, "entries: 3 in room for 2 is E2BIG");
	check(count == 2, "entries: 3 in room for 2 gives 2");
	check(entries[1].id[1] == 'B', "entries: the second is decoded");
	check(entries[2].flags == 0 && entries[2].id[0] == 0, "entries: nothing past the room is written");

	/* No room at all. */
	error = drv_acpi_wmi_entries_parse(example_wdg, sizeof(example_wdg), entries, 0, &count);
	check(error == E2BIG, "entries: no room is E2BIG");
	check(count == 0, "entries: no room gives none");
}

/* Checks the names of the control methods. */
static void
test_names(void)
{
	struct drv_acpi_wmi_entry entry;
	char name[5];
	int error;

	/* A data block's query and a method block's method. */
	memset(&entry, 0, sizeof(entry));
	entry.id[0] = 'B';
	entry.id[1] = 'A';
	error = drv_acpi_wmi_control_name(&entry, 'W', 'Q', name);
	check(error == 0, "names: WQBA is made");
	check(strcmp(name, "WQBA") == 0, "names: WQBA");
	entry.flags = DRV_ACPI_WMI_FLAG_METHOD;
	error = drv_acpi_wmi_control_name(&entry, 'W', 'M', name);
	check(error == 0 && strcmp(name, "WMBA") == 0, "names: WMBA");

	/* Digits and the underscore are name characters. */
	entry.id[0] = '0';
	entry.id[1] = '_';
	error = drv_acpi_wmi_control_name(&entry, 'W', 'C', name);
	check(error == 0 && strcmp(name, "WC0_") == 0, "names: WC0_");

	/* An event has no control method by identifier. */
	entry.flags = DRV_ACPI_WMI_FLAG_EVENT;
	entry.id[0] = 'D';
	entry.id[1] = '0';
	error = drv_acpi_wmi_control_name(&entry, 'W', 'Q', name);
	check(error == EINVAL, "names: an event is refused");

	/* Identifiers that cannot be in an ACPI name are refused. */
	entry.flags = 0;
	entry.id[0] = 'b';
	entry.id[1] = 'A';
	error = drv_acpi_wmi_control_name(&entry, 'W', 'Q', name);
	check(error == EINVAL, "names: a lower-case letter is refused");
	entry.id[0] = 'B';
	entry.id[1] = 0;
	error = drv_acpi_wmi_control_name(&entry, 'W', 'Q', name);
	check(error == EINVAL, "names: a NUL is refused");
	entry.id[0] = 0xd0;
	entry.id[1] = 'A';
	error = drv_acpi_wmi_control_name(&entry, 'W', 'Q', name);
	check(error == EINVAL, "names: a byte past ASCII is refused");
}
