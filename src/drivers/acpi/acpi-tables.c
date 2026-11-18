/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The firmware's ACPI tables (ACPI 6.5 section 5.2): the RSDP, the root
 * table (XSDT, or RSDT on revision 1), the FADT and the DSDT and SSDTs it
 * leads to.  Physical memory is read through a reader the platform
 * supplies, so the same code runs over a memory image in the host tests.
 */

#include <kern/kcrt.h>
#include <uapi/errno.h>

#include <drivers/acpi/acpi.h>

#include "acpi-tables.h"
#include "aml-os.h"

/*
 * The sizes of the RSDP of revision 1 and of revision 2 and later.
 */
#define RSDP_V1_SIZE		20U
#define RSDP_V2_SIZE		36U

/*
 * The size of the header every system description table starts with.
 */
#define TABLE_HEADER_SIZE	36U

/*
 * The largest table read, which keeps a damaged length from exhausting
 * the kernel heap.
 */
#define TABLE_SIZE_MAX		(16U * 1024U * 1024U)

/*
 * The largest number of tables a root table may list.
 */
#define ROOT_ENTRIES_MAX	256U

/*
 * The FADT fields the loader reads (ACPI 6.5 table 5.9): the 32-bit and
 * 64-bit addresses of the FACS and of the DSDT.
 */
#define FADT_FIRMWARE_CTRL	36U
#define FADT_DSDT		40U
#define FADT_X_FIRMWARE_CTRL	132U
#define FADT_X_DSDT		140U

static int read_rsdp(struct drv_acpi_firmware *firmware, uint64_t address, uint64_t *root, unsigned *entry_size);
static int read_root(struct drv_acpi_firmware *firmware, uint64_t address, unsigned entry_size);
static int read_header(struct drv_acpi_firmware *firmware, uint64_t address, struct drv_acpi_firmware_table *table);
static int read_fadt(struct drv_acpi_firmware *firmware, const struct drv_acpi_firmware_table *entry);
static int table_bytes(struct drv_acpi_firmware *firmware, struct drv_acpi_firmware_table *table);
static int load_one(struct drv_acpi_firmware *firmware, struct drv_acpi_firmware_table *table);
static bool identifier_equal(const char *field, size_t size, const char *text);
static bool checksum_ok(const uint8_t *bytes, size_t length);
static uint32_t load_u32(const uint8_t *bytes);
static uint64_t load_u64(const uint8_t *bytes);

/*
 * Finds the firmware's tables from the RSDP: every table the root table
 * lists, the FADT's copy, and the DSDT and FACS the FADT points at.
 */
int
drv_acpi_firmware_discover(
	uint64_t rsdp_address,
	drv_acpi_memory_reader_t read,
	void *argument,
	struct drv_acpi_firmware *firmware)
{
	uint64_t root;
	unsigned entry_size;
	unsigned index;
	int compared;
	int error;

	/* Starts with nothing found and the platform's reader. */
	kern_memset(firmware, 0, sizeof(*firmware));
	firmware->read = read;
	firmware->argument = argument;

	/* Reads the RSDP, which says where the root table is. */
	error = read_rsdp(firmware, rsdp_address, &root, &entry_size);
	if (error != 0)
		return error;

	/* Reads the root table's list of tables. */
	error = read_root(firmware, root, entry_size);
	if (error != 0) {
		drv_acpi_firmware_release(firmware);
		return error;
	}

	/* Reads the FADT, which says where the DSDT and the FACS are. */
	for (index = 0; index < firmware->count; index++) {
		/* Only the FADT carries the pointers. */
		compared = kern_strcmp(firmware->tables[index].signature, "FACP");
		if (compared != 0)
			continue;

		/* Reads the FADT and the DSDT header and FACS address it points at. */
		error = read_fadt(firmware, &firmware->tables[index]);
		if (error != 0) {
			drv_acpi_firmware_release(firmware);
			return error;
		}

		/* Stops at the first FADT. */
		break;
	}

	/* Refuses firmware without a DSDT: it has no AML to run. */
	if (firmware->dsdt.address == 0) {
		drv_acpi_os_log("ACPI: the FADT names no DSDT\n");
		drv_acpi_firmware_release(firmware);
		return ENOENT;
	}

	/* Succeeded: the record lists the tables and knows the DSDT, the FADT and the FACS. */
	return 0;
}

/*
 * Loads the DSDT and then every SSDT and PSDT in the root table's order
 * into the interpreter.
 *
 * An SSDT that fails is logged and skipped; the DSDT must load.
 */
int
drv_acpi_firmware_load(
	struct drv_acpi_firmware *firmware)
{
	struct drv_acpi_firmware_table *table;
	unsigned index;
	int ssdt;
	int psdt;
	int error;

	/* Loads the DSDT first: it sets the integer width for the others. */
	error = load_one(firmware, &firmware->dsdt);
	if (error != 0)
		return error;

	/* Loads each secondary table in order. */
	for (index = 0; index < firmware->count; index++) {
		/* Only SSDTs and the older PSDTs hold AML to load. */
		table = &firmware->tables[index];
		ssdt = kern_strcmp(table->signature, "SSDT");
		psdt = kern_strcmp(table->signature, "PSDT");
		if (ssdt != 0 && psdt != 0)
			continue;

		/* Loads it; a failing one does not stop the rest. */
		error = load_one(firmware, table);
		if (error != 0)
			drv_acpi_os_log("ACPI: %s %s did not load (error %d)\n", table->signature, table->oem_table_id, error);
	}

	/* Succeeded: the DSDT is loaded. */
	return 0;
}

/*
 * Finds a table by its signature and OEM identifiers, as LoadTable asks,
 * and reports its bytes, which stay valid for the life of the record.
 *
 * An empty identifier matches any.
 */
int
drv_acpi_firmware_find(
	struct drv_acpi_firmware *firmware,
	const char *signature,
	const char *oem_id,
	const char *oem_table_id,
	const uint8_t **data,
	size_t *length)
{
	struct drv_acpi_firmware_table *table;
	struct drv_acpi_firmware_table *found;
	unsigned index;
	bool equal;
	int error;

	/* Finds the first listed table that matches. */
	found = NULL;
	for (index = 0; index < firmware->count; index++) {
		/* Skips a table with another signature. */
		table = &firmware->tables[index];
		equal = identifier_equal(table->signature, 4, signature);
		if (!equal)
			continue;

		/* Skips a table with another OEM ID. */
		equal = identifier_equal(table->oem_id, 6, oem_id);
		if (oem_id[0] != '\0' && !equal)
			continue;

		/* Skips a table with another OEM table ID. */
		equal = identifier_equal(table->oem_table_id, 8, oem_table_id);
		if (oem_table_id[0] != '\0' && !equal)
			continue;

		/* Stops at the match. */
		found = table;
		break;
	}

	/* Reports that the firmware lists no such table. */
	if (found == NULL)
		return ENOENT;

	/* Reads the table's bytes. */
	error = table_bytes(firmware, found);
	if (error != 0)
		return error;

	/* Hands the bytes to the caller. */
	*data = found->copy;
	*length = found->length;

	/* Succeeded: the bytes stay valid for the life of the record. */
	return 0;
}

/*
 * Frees what a firmware record holds.
 */
void
drv_acpi_firmware_release(
	struct drv_acpi_firmware *firmware)
{
	unsigned index;

	/* Frees each table's copy. */
	for (index = 0; index < firmware->count; index++)
		drv_acpi_os_free(firmware->tables[index].copy);

	/* Frees the list. */
	drv_acpi_os_free(firmware->tables);

	/* Frees the DSDT's copy and the FADT's. */
	drv_acpi_os_free(firmware->dsdt.copy);
	drv_acpi_os_free(firmware->fadt);

	/* Forgets everything. */
	kern_memset(firmware, 0, sizeof(*firmware));
}

/* Reads and checks the RSDP and reports the root table and its entry size. */
static int
read_rsdp(
	struct drv_acpi_firmware *firmware,
	uint64_t address,
	uint64_t *root,
	unsigned *entry_size)
{
	uint8_t rsdp[RSDP_V2_SIZE];
	uint32_t length;
	uint64_t xsdt;
	bool valid;
	int compared;
	int error;

	/* Reads the part every revision has. */
	error = firmware->read(address, rsdp, RSDP_V1_SIZE, firmware->argument);
	if (error != 0)
		return error;

	/* Refuses anything that is not an RSDP. */
	compared = kern_memcmp(rsdp, "RSD PTR ", 8);
	if (compared != 0)
		return EINVAL;
	valid = checksum_ok(rsdp, RSDP_V1_SIZE);
	if (!valid)
		return EINVAL;

	/* Keeps the revision, which says which root table the RSDP can name. */
	firmware->rsdp_revision = rsdp[15];

	/* A revision 1 RSDP has only the RSDT, of 32-bit entries. */
	if (firmware->rsdp_revision < 2) {
		*root = load_u32(rsdp + 16);
		*entry_size = 4;
		return 0;
	}

	/* Reads the extended part, which the extended checksum covers. */
	error = firmware->read(address, rsdp, RSDP_V2_SIZE, firmware->argument);
	if (error != 0)
		return error;

	/* Logs an extended part whose length or checksum is wrong, and uses it anyway, as firmware gets it wrong. */
	length = load_u32(rsdp + 20);
	valid = checksum_ok(rsdp, RSDP_V2_SIZE);
	if (length != RSDP_V2_SIZE || !valid)
		drv_acpi_os_log("ACPI: RSDP extended part does not check (length %u)\n", (unsigned)length);

	/* Prefers the XSDT, of 64-bit entries, when there is one. */
	xsdt = load_u64(rsdp + 24);
	if (xsdt != 0) {
		*root = xsdt;
		*entry_size = 8;
		return 0;
	}

	/* Succeeded: falls back to the RSDT. */
	*root = load_u32(rsdp + 16);
	*entry_size = 4;
	return 0;
}

/* Reads the root table and the header of every table it lists. */
static int
read_root(
	struct drv_acpi_firmware *firmware,
	uint64_t address,
	unsigned entry_size)
{
	struct drv_acpi_firmware_table root;
	struct drv_acpi_firmware_table *entry;
	uint64_t table_address;
	unsigned count;
	unsigned index;
	int error;

	/* Reads the root table's header. */
	error = read_header(firmware, address, &root);
	if (error != 0)
		return error;

	/* Reads the root table whole. */
	error = table_bytes(firmware, &root);
	if (error != 0)
		return error;

	/* Counts its entries. */
	count = (unsigned)((root.length - TABLE_HEADER_SIZE) / entry_size);
	if (count > ROOT_ENTRIES_MAX)
		count = ROOT_ENTRIES_MAX;

	/* Allocates the list. */
	firmware->tables = drv_acpi_os_alloc((size_t)count * sizeof(firmware->tables[0]) + 1U);
	if (firmware->tables == NULL) {
		drv_acpi_os_free(root.copy);
		return ENOMEM;
	}

	/* Every entry starts empty. */
	kern_memset(firmware->tables, 0, (size_t)count * sizeof(firmware->tables[0]));

	/* Reads the header of each listed table; one that cannot be read is left out. */
	for (index = 0; index < count; index++) {
		/* Reads the entry's address. */
		if (entry_size == 8) {
			table_address = load_u64(root.copy + TABLE_HEADER_SIZE + index * 8U);
		} else {
			table_address = load_u32(root.copy + TABLE_HEADER_SIZE + index * 4U);
		}

		/* Reads the header there. */
		entry = &firmware->tables[firmware->count];
		error = read_header(firmware, table_address, entry);
		if (error != 0) {
			drv_acpi_os_log("ACPI: table %u of the root table cannot be read\n", index);
			continue;
		}

		/* Keeps it. */
		firmware->count++;
	}

	/* Succeeded: the root table itself is no longer needed. */
	drv_acpi_os_free(root.copy);
	return 0;
}

/* Reads a table's header and records its identity and length. */
static int
read_header(
	struct drv_acpi_firmware *firmware,
	uint64_t address,
	struct drv_acpi_firmware_table *table)
{
	uint8_t header[TABLE_HEADER_SIZE];
	int error;

	/* Starts the record empty, so that a failure leaves nothing half known. */
	kern_memset(table, 0, sizeof(*table));

	/* Reads the header. */
	error = firmware->read(address, header, sizeof(header), firmware->argument);
	if (error != 0)
		return error;

	/* Refuses a length too small or too large to be a table. */
	table->length = load_u32(header + 4);
	if (table->length < TABLE_HEADER_SIZE || table->length > TABLE_SIZE_MAX)
		return EINVAL;

	/* Records where it is and who it is. */
	table->address = address;
	kern_memcpy(table->signature, header, 4);
	kern_memcpy(table->oem_id, header + 10, 6);
	kern_memcpy(table->oem_table_id, header + 16, 8);

	/* Succeeded: the record knows the table's place, identity and length. */
	return 0;
}

/* Reads the FADT and the DSDT header and FACS address it points at. */
static int
read_fadt(
	struct drv_acpi_firmware *firmware,
	const struct drv_acpi_firmware_table *entry)
{
	struct drv_acpi_firmware_table fadt;
	uint64_t dsdt;
	uint64_t facs;
	uint64_t wide_facs;
	uint64_t wide_dsdt;
	int error;

	/* Reads the FADT. */
	fadt = *entry;
	fadt.copy = NULL;
	error = table_bytes(firmware, &fadt);
	if (error != 0)
		return error;

	/* Keeps its copy, which the event code reads. */
	firmware->fadt = fadt.copy;
	firmware->fadt_length = fadt.length;

	/* Takes the 32-bit pointers every FADT has. */
	dsdt = 0;
	facs = 0;
	if (fadt.length >= FADT_DSDT + 4U) {
		facs = load_u32(fadt.copy + FADT_FIRMWARE_CTRL);
		dsdt = load_u32(fadt.copy + FADT_DSDT);
	}

	/* The 64-bit FACS pointer wins when a newer FADT has one. */
	wide_facs = 0;
	if (fadt.length >= FADT_X_FIRMWARE_CTRL + 8U)
		wide_facs = load_u64(fadt.copy + FADT_X_FIRMWARE_CTRL);
	if (wide_facs != 0)
		facs = wide_facs;

	/* The 64-bit DSDT pointer wins when a newer FADT has one. */
	wide_dsdt = 0;
	if (fadt.length >= FADT_X_DSDT + 8U)
		wide_dsdt = load_u64(fadt.copy + FADT_X_DSDT);
	if (wide_dsdt != 0)
		dsdt = wide_dsdt;

	/* Keeps the FACS address, which the Global Lock needs. */
	firmware->facs_address = facs;

	/* A FADT without a DSDT leaves it unknown, which the caller refuses. */
	if (dsdt == 0)
		return 0;

	/* Reads the DSDT's header. */
	error = read_header(firmware, dsdt, &firmware->dsdt);
	if (error != 0)
		return error;

	/* Succeeded: the record knows the FADT, the FACS address and the DSDT. */
	return 0;
}

/* Reads a whole table into its copy, once. */
static int
table_bytes(
	struct drv_acpi_firmware *firmware,
	struct drv_acpi_firmware_table *table)
{
	uint8_t *copy;
	bool valid;
	int error;

	/* A table read before is kept. */
	if (table->copy != NULL)
		return 0;

	/* Allocates the copy. */
	copy = drv_acpi_os_alloc(table->length);
	if (copy == NULL)
		return ENOMEM;

	/* Reads the table. */
	error = firmware->read(table->address, copy, table->length, firmware->argument);
	if (error != 0) {
		drv_acpi_os_free(copy);
		return error;
	}

	/* A wrong checksum is logged and tolerated, as firmware gets it wrong. */
	valid = checksum_ok(copy, table->length);
	if (!valid)
		drv_acpi_os_log("ACPI: table %s has a wrong checksum\n", table->signature);

	/* Keeps the copy for the record's life. */
	table->copy = copy;

	/* Succeeded: the table's bytes are in its copy. */
	return 0;
}

/* Reads one AML table and loads it into the interpreter. */
static int
load_one(
	struct drv_acpi_firmware *firmware,
	struct drv_acpi_firmware_table *table)
{
	int error;

	/* Reads the table's bytes. */
	error = table_bytes(firmware, table);
	if (error != 0)
		return error;

	/* Loads its AML into the namespace. */
	error = drv_acpi_load_table(table->copy, table->length);
	if (error != 0)
		return error;

	/* Succeeded: the table's objects are in the namespace. */
	return 0;
}

/*
 * Compares a fixed-size identifier field with a text: the text's
 * characters must match and the rest of the field be NUL or blank.
 */
static bool
identifier_equal(
	const char *field,
	size_t size,
	const char *text)
{
	size_t index;

	/* Compares the text's characters. */
	for (index = 0; index < size && text[index] != '\0'; index++) {
		/* Stops at the first difference. */
		if (field[index] != text[index])
			return false;
	}

	/* A text longer than the field does not match. */
	if (index == size && text[index] != '\0')
		return false;

	/* The rest of the field must be padding. */
	for (;
	     index < size;
	     index++) {
		/* Stops at a character that is not padding. */
		if (field[index] != '\0' && field[index] != ' ')
			return false;
	}

	/* Reports a match. */
	return true;
}

/* Reports whether the bytes of a table sum to zero. */
static bool
checksum_ok(
	const uint8_t *bytes,
	size_t length)
{
	uint8_t sum;
	size_t index;

	/* Sums the bytes. */
	sum = 0;
	for (index = 0; index < length; index++)
		sum = (uint8_t)(sum + bytes[index]);

	/* A valid table sums to zero. */
	if (sum == 0)
		return true;

	/* Reports a table whose bytes do not sum to zero. */
	return false;
}

/* Reads a little-endian 32-bit value. */
static uint32_t
load_u32(
	const uint8_t *bytes)
{
	uint32_t value;

	/* Assembles the bytes, lowest first. */
	value = (uint32_t)bytes[0];
	value |= (uint32_t)bytes[1] << 8;
	value |= (uint32_t)bytes[2] << 16;
	value |= (uint32_t)bytes[3] << 24;

	/* Reports the assembled value. */
	return value;
}

/* Reads a little-endian 64-bit value. */
static uint64_t
load_u64(
	const uint8_t *bytes)
{
	uint64_t value;

	/* Assembles the two halves. */
	value = (uint64_t)load_u32(bytes);
	value |= (uint64_t)load_u32(bytes + 4) << 32;

	/* Reports the assembled value. */
	return value;
}
