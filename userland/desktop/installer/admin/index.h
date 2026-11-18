/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The installer's index (/opt/installer/index): the units an installed
 * system is made of, the boot files, and the set-user-ID and
 * set-group-ID files a unit may carry.  The build writes it
 * (tools/build/installer-units.py), one record a line, its fields
 * separated by TABs:
 *
 *   format  1
 *   unit    NAME LABEL GROUP VERSION TAR-BYTES BYTES ENTRIES SHA-256 DEPENDENCIES
 *   boot    NAME PLACE BYTES SHA-256
 *   setid   UNIT PATH MODE
 *   license UNIT PACKAGE PATH
 *   item    ID PARENT DEFAULT LABEL UNITS
 *
 * DEPENDENCIES are unit names separated by spaces, or "-".  The helper
 * reads the units, the boot files and the set-ID files; the licenses and
 * the items are the installer's.
 */

#ifndef USERLAND_DESKTOP_INSTALLER_ADMIN_INDEX_H
#define USERLAND_DESKTOP_INSTALLER_ADMIN_INDEX_H

#include <stddef.h>
#include <stdint.h>

/* The most units, boot files and set-ID files one index may name. */
#define INDEX_UNITS_MAX		128U
#define INDEX_BOOTS_MAX		8U
#define INDEX_SETIDS_MAX	64U

/* The most units one unit may depend on. */
#define INDEX_DEPENDENCIES_MAX	32U

/* The longest name, label, group, version and path a record may give. */
#define INDEX_NAME_MAX		64U
#define INDEX_LABEL_MAX		128U
#define INDEX_GROUP_MAX		16U
#define INDEX_VERSION_MAX	48U
#define INDEX_PATH_MAX		256U

/* The largest index the helper reads. */
#define INDEX_BYTES_MAX		(1024U * 1024U)

/*
 * One unit: a tarball of /opt/installer/units and what is known of it
 * before it is opened.
 */
struct index_unit {
	char name[INDEX_NAME_MAX];
	char label[INDEX_LABEL_MAX];
	char group[INDEX_GROUP_MAX];
	char version[INDEX_VERSION_MAX];
	uint64_t tar_bytes;
	uint64_t bytes;
	uint64_t entries;
	uint8_t sha256[32];
	unsigned dependency[INDEX_DEPENDENCIES_MAX];
	unsigned dependency_count;
};

/*
 * One boot file: the loader, which goes to the ESP, or the kernel and the
 * logo, which go to the installed root.
 */
struct index_boot {
	char name[INDEX_NAME_MAX];
	char place[INDEX_PATH_MAX];
	uint64_t bytes;
	uint8_t sha256[32];
};

/* One file a unit may carry with its set-user-ID or set-group-ID bit. */
struct index_setid {
	unsigned unit;
	char path[INDEX_PATH_MAX];
	unsigned mode;
};

/* The whole index, as one read of it gave it. */
struct installer_index {
	struct index_unit unit[INDEX_UNITS_MAX];
	unsigned unit_count;
	struct index_boot boot[INDEX_BOOTS_MAX];
	unsigned boot_count;
	struct index_setid setid[INDEX_SETIDS_MAX];
	unsigned setid_count;
};

int index_parse(const char *text, size_t length, struct installer_index *index);
int index_digest_matches(const char *text, size_t length, const char *expected);
int index_find_unit(const struct installer_index *index, const char *name, unsigned *slot);
int index_closure(const struct installer_index *index, const unsigned *chosen, unsigned chosen_count, unsigned *order, unsigned *order_count);
int index_setid_allowed(const struct installer_index *index, unsigned unit, const char *path, unsigned mode);
int index_hex_digest(const char *text, uint8_t digest[32]);

#endif
