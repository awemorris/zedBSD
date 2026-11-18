/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The requests install-admin's session reads from the installer, and the
 * plan of an installation, read and checked again by the helper (it does
 * not believe the installer).  A request is one line of tokens separated
 * by one space and ended by a newline:
 *
 *   probe
 *   cancel
 *   edit delete DISK part TUPLE
 *   edit create DISK new START COUNT
 *   edit create-with-esp DISK esp START COUNT new START COUNT
 *   install DISK LAYOUT units N NAME... user FIELD fullname FIELD
 *           password FIELD root-password FIELD autologin yes|no
 *           hostname FIELD
 *
 * where DISK is "disk REGISTRATION NAME SECTOR-BYTES SECTORS DIGEST" (the
 * disk's registration number, its device name, its logical sector size,
 * its size in sectors and the SHA-256, in hex, of the partition table the
 * screen showed), LAYOUT is "whole ESP-BYTES SWAP-BYTES" (the whole disk,
 * no swap partition when SWAP-BYTES is 0) or "chosen esp TUPLE root TUPLE
 * swap TUPLE|none", and TUPLE is REGISTRATION:START:SECTORS:PARTUUID (the
 * disk's registration number, the partition's first sector and its size
 * in sectors, and its PARTUUID).  START and COUNT are sectors.  A FIELD,
 * the text a person typed, is LENGTH:BYTES -- the byte count in decimal,
 * a colon and exactly that many bytes -- so no space or newline in it can
 * break the request.  Other tokens are printable ASCII without a space.
 */

#ifndef USERLAND_DESKTOP_INSTALLER_ADMIN_PLAN_H
#define USERLAND_DESKTOP_INSTALLER_ADMIN_PLAN_H

#include "index.h"

#include <stddef.h>
#include <stdint.h>

/* The longest request the session holds. */
#define PLAN_REQUEST_MAX	16384U

/* The longest device name, login name, display name, password and hostname. */
#define PLAN_DEVICE_MAX		32U
#define PLAN_USER_MAX		32U
#define PLAN_FULLNAME_MAX	64U
#define PLAN_PASSWORD_MAX	256U
#define PLAN_HOSTNAME_MAX	63U

/* The text of a GUID: 8-4-4-4-12 hex digits. */
#define PLAN_GUID_TEXT		36U

/* The units one plan may name. */
#define PLAN_UNITS_MAX		INDEX_UNITS_MAX

/* The alignment of the partitions a plan makes, and the smallest new ESP. */
#define PLAN_ALIGN_BYTES	(1024ULL * 1024ULL)
#define PLAN_ESP_MIN_BYTES	(512ULL * 1024ULL * 1024ULL)

/* The cfg file's longest text. */
#define PLAN_CFG_MAX		512U

/* What a request asks for. */
enum plan_verb {
	PLAN_PROBE = 1,
	PLAN_CANCEL,
	PLAN_EDIT_DELETE,
	PLAN_EDIT_CREATE,
	PLAN_EDIT_CREATE_WITH_ESP,
	PLAN_INSTALL
};

/* How an installation lays out its disk. */
enum plan_layout {
	PLAN_WHOLE = 1,
	PLAN_CHOSEN
};

/* The disk a request is about, as the screen saw it. */
struct plan_disk {
	uint64_t registration;
	char name[PLAN_DEVICE_MAX + 1U];
	uint32_t sector_bytes;
	uint64_t sectors;
	uint8_t table_digest[32];
};

/*
 * A partition, named by the four things that cannot be mistaken for
 * another one's: its disk's registration number, its first sector, its
 * size in sectors and its PARTUUID (lower case).  A slot number or a
 * device name may name another range after a delete and an add.
 */
struct plan_tuple {
	uint64_t disk;
	uint64_t start;
	uint64_t sectors;
	char partuuid[PLAN_GUID_TEXT + 1U];
};

/* A range of sectors a new partition is to take. */
struct plan_extent {
	uint64_t start;
	uint64_t sectors;
};

/* An installation: where it goes, what goes and whom the system is for. */
struct install_plan {
	enum plan_layout layout;
	uint64_t whole_esp_bytes;
	uint64_t whole_swap_bytes;
	struct plan_tuple esp;
	struct plan_tuple root;
	struct plan_tuple swap;
	int has_swap_partition;
	char unit[PLAN_UNITS_MAX][INDEX_NAME_MAX];
	unsigned unit_count;
	char user[PLAN_USER_MAX + 1U];
	char fullname[PLAN_FULLNAME_MAX + 1U];
	char password[PLAN_PASSWORD_MAX + 1U];
	char root_password[PLAN_PASSWORD_MAX + 1U];
	int autologin;
	char hostname[PLAN_HOSTNAME_MAX + 1U];
};

/* One request, as it was read. */
struct plan_request {
	enum plan_verb verb;
	struct plan_disk disk;
	struct plan_tuple part;
	struct plan_extent esp_new;
	struct plan_extent part_new;
	struct install_plan install;
};

/* What the cfg file of an installed system says. */
struct plan_cfg {
	const char *root_partuuid;
	const char *swap_partuuid;
	int graphical;
};

int plan_request_read(const char *text, size_t length, struct plan_request *request, size_t *consumed);
int plan_install_check(const struct plan_disk *disk, const struct install_plan *plan);
int plan_edit_check(const struct plan_request *request);
int plan_user_valid(const char *name);
int plan_fullname_valid(const char *text);
int plan_password_valid(const char *password);
int plan_hostname_valid(const char *name);
int plan_guid_valid(const char *text);
int plan_tuple_same(const struct plan_tuple *left, const struct plan_tuple *right);
int plan_cfg_text(const struct plan_cfg *cfg, char *text, size_t capacity, size_t *length);
void plan_request_forget(struct plan_request *request);

#endif
