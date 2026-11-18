/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Reading the boot database the security processor may leave 1 MiB below
 * the top of VRAM.
 *
 * When it is there it tells, among other things, whether the board wants
 * the second, long stage of memory training run by the driver.  A board
 * without it starts with no database at all, and the bytes there are
 * whatever the VRAM held.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <uapi/errno.h>

#include "drivers/gpu/amdrdna/amdrdna-hw.h"

/*
 * The database's header (its mark and its version), the directory after it
 * (an entry count, then entries of a type dword, an offset word and a
 * length word), the most entries a sane directory lists, and the type of
 * the boot configuration entry with its smallest length.
 *
 * Values referenced: Linux v6.19
 * drivers/gpu/drm/amd/amdgpu/amdgpu_psp.h (PSP_RUNTIME_DB_COOKIE_ID,
 * PSP_RUNTIME_DB_DIAG_ENTRY_MAX_COUNT, PSP_RUNTIME_ENTRY_TYPE_BOOT_CONFIG,
 * struct psp_runtime_data_header, struct psp_runtime_entry,
 * struct psp_runtime_data_directory, struct psp_runtime_boot_cfg_entry).
 */
#define DATABASE_MARK			0x0ed5U
#define DATABASE_VERSION_AT		2U
#define DATABASE_COUNT_AT		4U
#define DATABASE_ENTRIES_AT		8U
#define DATABASE_ENTRY_BYTES		8U
#define DATABASE_ENTRY_OFFSET_AT	4U
#define DATABASE_ENTRY_LENGTH_AT	6U
#define DATABASE_ENTRY_LIMIT		0x40U
#define DATABASE_TYPE_BOOT_CONFIG	5U
#define DATABASE_BOOT_CONFIG_BYTES	8U

/*
 * Reads the boot database's header and directory.
 *
 * Returns 0 with database->present false when there is no database, or
 * true with its version and entry count; when the directory lists a boot
 * configuration entry, has_config is set with the entry's offset, and its
 * bits are filled too when the entry lies inside the bytes given.  Returns
 * EINVAL for a database whose directory is damaged.
 *
 * Procedure referenced: Linux v6.19
 * drivers/gpu/drm/amd/amdgpu/amdgpu_psp.c (psp_get_runtime_db_entry).
 */
int
amdrdna_boot_database_parse(
	const uint8_t *bytes,
	size_t length,
	struct amdrdna_boot_database *database)
{
	unsigned index;
	uint32_t type;
	uint16_t mark;
	uint16_t entry_offset;
	uint16_t entry_length;
	size_t entry;

	/* Starts with no database. */
	database->present = false;
	database->version = 0U;
	database->entries = 0U;
	database->has_config = false;
	database->config_offset = 0U;
	database->config_read = false;
	database->config_bits = 0U;

	/* Fewer bytes than the header and the directory's count hold no database. */
	if (length < DATABASE_ENTRIES_AT)
		return 0;

	/* Without the mark the VRAM holds no database. */
	mark = amdrdna_le16(bytes, 0U);
	if (mark != DATABASE_MARK)
		return 0;

	/* Records the database's version and its entry count. */
	database->present = true;
	database->version = amdrdna_le16(bytes, DATABASE_VERSION_AT);
	database->entries = amdrdna_le16(bytes, DATABASE_COUNT_AT);

	/* A count at or above the limit says the directory is damaged. */
	if (database->entries >= DATABASE_ENTRY_LIMIT)
		return EINVAL;

	/* The whole directory must lie inside the bytes given. */
	if (DATABASE_ENTRIES_AT + (size_t)database->entries * DATABASE_ENTRY_BYTES > length)
		return EINVAL;

	/* Looks for the boot configuration entry. */
	for (index = 0U; index < database->entries; index++) {
		/* Skips an entry of another type. */
		entry = DATABASE_ENTRIES_AT + (size_t)index * DATABASE_ENTRY_BYTES;
		type = amdrdna_le32(bytes, entry);
		if (type != DATABASE_TYPE_BOOT_CONFIG)
			continue;

		/* An entry too short for the configuration bits is damaged. */
		entry_length = amdrdna_le16(bytes, entry + DATABASE_ENTRY_LENGTH_AT);
		if (entry_length < DATABASE_BOOT_CONFIG_BYTES)
			return EINVAL;

		/* Records where the entry lies, from the database's start. */
		entry_offset = amdrdna_le16(bytes, entry + DATABASE_ENTRY_OFFSET_AT);
		database->has_config = true;
		database->config_offset = entry_offset;
		break;
	}

	/* Reads the configuration bits when the entry lies inside the bytes given. */
	if (database->has_config && (size_t)database->config_offset + DATABASE_BOOT_CONFIG_BYTES <= length)
		amdrdna_boot_database_set_config(database, bytes + database->config_offset);

	/* Succeeded: the database is described. */
	return 0;
}

/*
 * Records the bits of a boot configuration entry the caller read on its own.
 */
void
amdrdna_boot_database_set_config(
	struct amdrdna_boot_database *database,
	const uint8_t *entry)
{
	/* The first dword of the entry holds the bits. */
	database->config_bits = amdrdna_le32(entry, 0U);
	database->config_read = true;
}
