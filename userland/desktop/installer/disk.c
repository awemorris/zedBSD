/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The disks as the helper's probe told them (admin/probe.h), and the
 * rules of what the screens may choose: the disks that take an
 * installation (S2), those whose partitions may be chosen (S3), the ESP
 * that is enough, the free room a new partition goes in (S4), the sizes
 * of the whole disk's layout, the swap file and the root the units need.
 * The helper checks each request again; these rules only keep the
 * screens from offering what it would refuse.
 */

#include "installer.h"

#include <uapi/block.h>
#include <uapi/gpt-type.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* The most fields of one record of the probe. */
#define DISK_FIELDS_MAX		16U

/* The most partitions one disk's table holds that diskpart edits. */
#define DISK_ENTRIES_MAX	16U

/* A disk smaller than this has a swap partition of 1 GiB. */
#define DISK_SMALL_BYTES	(32ULL * INST_GIB)

/* The swap partition of the whole disk's layout: as large as the memory, within these. */
#define DISK_SWAP_LOW		INST_GIB
#define DISK_SWAP_HIGH		(8ULL * INST_GIB)

/*
 * The names of the partition types the screens show.  The table is
 * constant for the life of the program.
 */
static const char *const disk_type_names[][2] = {
	{ KERN_GPT_TYPE_EFI_SYSTEM, "EFI system" },
	{ KERN_GPT_TYPE_ZEDBSD_UFS, "System (UFS)" },
	{ KERN_GPT_TYPE_ZEDBSD_SWAP, "Swap" },
	{ KERN_GPT_TYPE_ZEDBSD_UNASSIGNED, "Unassigned" },
	{ KERN_GPT_TYPE_BASIC_DATA, "Microsoft basic data" },
	{ KERN_GPT_TYPE_MICROSOFT_RESERVED, "Microsoft reserved" },
	{ KERN_GPT_TYPE_WINDOWS_RECOVERY, "Windows recovery" },
	{ KERN_GPT_TYPE_LINUX_FILESYSTEM, "Linux file system" },
	{ KERN_GPT_TYPE_LINUX_SWAP, "Linux swap" },
	{ KERN_GPT_TYPE_FREEBSD_BOOT, "FreeBSD boot" },
	{ KERN_GPT_TYPE_APPLE_APFS, "Apple APFS" }
};

static unsigned disk_split(char *line, char **field, unsigned most);
static void disk_copy(char *target, size_t size, const char *source);
static enum inst_answer disk_answer(const char *text);
static int disk_esp_enough(const struct inst_app *app, const struct inst_disk *disk, const struct inst_part *part);
static uint64_t disk_grid(const struct inst_disk *disk);
static int disk_gaps(const struct inst_disk *disk, const struct inst_probe *probe, uint64_t *start, uint64_t *sectors, unsigned most);

/*
 * Reads one line of a probe into it.  Returns 0 for a record read, 1 for
 * the probe's end, -1 for a line the installer does not understand.
 */
int
inst_probe_line(
	struct inst_probe *probe,
	char *line)
{
	struct inst_disk *disk;
	struct inst_part *part;
	char *field[DISK_FIELDS_MAX];
	unsigned count;
	unsigned i;
	uint32_t registration;
	unsigned slot;
	int same;

	/* Splits the line into its fields. */
	count = disk_split(line, field, DISK_FIELDS_MAX);

	/* The end of the probe. */
	same = strcmp(field[0], "end");
	if (same == 0)
		return 1;

	/* The firmware, which says whether the machine boots by UEFI. */
	same = strcmp(field[0], "firmware");
	if (same == 0 && count >= 2U) {
		probe->uefi = 0;
		same = strcmp(field[1], "uefi");
		if (same == 0)
			probe->uefi = 1;
		return 0;
	}

	/* The memory, in bytes. */
	same = strcmp(field[0], "memory");
	if (same == 0 && count >= 2U) {
		probe->memory = strtoull(field[1], NULL, 10);
		return 0;
	}

	/* A disk. */
	same = strcmp(field[0], "disk");
	if (same == 0 && count >= 12U) {
		if (probe->disk_count >= INST_DISKS_MAX)
			return 0;
		disk = &probe->disk[probe->disk_count];
		memset(disk, 0, sizeof(*disk));
		disk_copy(disk->name, sizeof(disk->name), field[1]);
		disk->registration = (uint32_t)strtoul(field[2], NULL, 10);
		disk_copy(disk->kind, sizeof(disk->kind), field[3]);
		disk->sector_bytes = (uint32_t)strtoul(field[4], NULL, 10);
		disk->sectors = strtoull(field[5], NULL, 10);
		disk->flags = (unsigned)strtoul(field[6], NULL, 10);
		disk->live = (int)strtol(field[7], NULL, 10);
		disk_copy(disk->table, sizeof(disk->table), field[8]);
		disk->restrictions = (unsigned)strtoul(field[9], NULL, 10);
		disk->entries = (unsigned)strtoul(field[10], NULL, 10);
		disk_copy(disk->digest, sizeof(disk->digest), field[11]);
		disk->first_part = probe->part_count;
		disk->part_count = 0;
		probe->disk_count++;
		return 0;
	}

	/* A partition of the disk before it. */
	same = strcmp(field[0], "part");
	if (same == 0 && count >= 15U) {
		if (probe->disk_count == 0U || probe->part_count >= INST_PARTS_MAX)
			return 0;
		disk = &probe->disk[probe->disk_count - 1U];
		part = &probe->part[probe->part_count];
		memset(part, 0, sizeof(*part));
		part->disk = (uint32_t)strtoul(field[1], NULL, 10);
		part->slot = (unsigned)strtoul(field[2], NULL, 10);
		part->start = strtoull(field[3], NULL, 10);
		part->sectors = strtoull(field[4], NULL, 10);
		disk_copy(part->type, sizeof(part->type), field[5]);
		disk_copy(part->partuuid, sizeof(part->partuuid), field[6]);
		part->attributes = strtoull(field[7], NULL, 10);
		disk_copy(part->device, sizeof(part->device), field[8]);
		disk_copy(part->fstype, sizeof(part->fstype), field[9]);
		disk_copy(part->fsuuid, sizeof(part->fsuuid), field[10]);
		part->mounted = (int)strtol(field[11], NULL, 10);
		part->swap = (int)strtol(field[12], NULL, 10);
		disk_copy(part->name, sizeof(part->name), field[13]);
		disk_copy(part->fslabel, sizeof(part->fslabel), field[14]);
		part->cfg = INST_NO;
		part->boot_loader = INST_NO;
		part->zedbsd_loader = INST_NO;
		probe->part_count++;
		disk->part_count++;
		return 0;
	}

	/* What a FAT holds, for the partition it is on. */
	same = strcmp(field[0], "fat");
	if (same == 0 && count >= 7U) {
		registration = (uint32_t)strtoul(field[1], NULL, 10);
		slot = (unsigned)strtoul(field[2], NULL, 10);
		for (i = 0; i < probe->part_count; i++) {
			part = &probe->part[i];
			if (part->disk != registration || part->slot != slot)
				continue;
			part->fat = 1;
			part->fat_free = strtoull(field[3], NULL, 10);
			part->cfg = disk_answer(field[4]);
			part->boot_loader = disk_answer(field[5]);
			part->zedbsd_loader = disk_answer(field[6]);
		}

		/* The record is taken. */
		return 0;
	}

	/* A record of another kind. */
	return -1;
}

/* Finds a disk of a probe by its registration number; NULL when it is gone. */
const struct inst_disk *
inst_disk_find(
	const struct inst_probe *probe,
	uint32_t registration)
{
	unsigned i;

	/* Each disk. */
	for (i = 0; i < probe->disk_count; i++) {
		if (probe->disk[i].registration == registration)
			return &probe->disk[i];
	}

	/* No such disk. */
	return NULL;
}

/* Finds a partition of a probe by its tuple; NULL when no partition is that one any more. */
const struct inst_part *
inst_part_find(
	const struct inst_probe *probe,
	const struct inst_tuple *tuple)
{
	struct inst_tuple other;
	unsigned i;
	int same;

	/* Each partition. */
	for (i = 0; i < probe->part_count; i++) {
		inst_tuple_of(&probe->part[i], &other);
		same = inst_tuple_same(&other, tuple);
		if (same)
			return &probe->part[i];
	}

	/* No such partition. */
	return NULL;
}

/* Gives a partition's tuple. */
void
inst_tuple_of(
	const struct inst_part *part,
	struct inst_tuple *tuple)
{
	/* Its disk, its sectors and its PARTUUID. */
	memset(tuple, 0, sizeof(*tuple));
	tuple->disk = part->disk;
	tuple->start = part->start;
	tuple->sectors = part->sectors;
	disk_copy(tuple->partuuid, sizeof(tuple->partuuid), part->partuuid);
}

/* Reports whether two tuples name the same partition. */
int
inst_tuple_same(
	const struct inst_tuple *left,
	const struct inst_tuple *right)
{
	int same;

	/* The disk and the sectors. */
	if (left->disk != right->disk || left->start != right->start || left->sectors != right->sectors)
		return 0;

	/* The PARTUUID, in either case. */
	same = strcasecmp(left->partuuid, right->partuuid);
	if (same != 0)
		return 0;

	/* The same partition. */
	return 1;
}

/*
 * Reports why a disk cannot take an installation (S2), or NULL when it
 * can: the running system uses it, it is read-only or a file's, its
 * sectors are of a size the tools do not take, or it is too small.
 */
const char *
inst_disk_refusal(
	const struct inst_app *app,
	const struct inst_disk *disk)
{
	uint64_t needed;
	uint64_t disk_bytes;

	/* The running system's own. */
	if (disk->live)
		return "The running system uses this disk.";

	/* Read-only, or a file's. */
	if ((disk->flags & KERN_BLOCK_READ_ONLY) != 0U)
		return "This disk is read-only.";
	if ((disk->flags & KERN_BLOCK_FILE_BACKED) != 0U)
		return "This disk is a file, not a device.";

	/* A sector size of the tools. */
	if (disk->sector_bytes != 512U && disk->sector_bytes != 4096U)
		return "This disk's sector size is not supported.";

	/* Room for the ESP, the swap partition and the base system. */
	needed = INST_ESP_BYTES + DISK_SWAP_LOW + inst_root_needed(app, 1) + 3U * INST_MIB;
	disk_bytes = inst_disk_bytes(disk);
	if (disk_bytes < needed)
		return "This disk is too small.";

	/* The disk can take the installation. */
	return NULL;
}

/*
 * Reports why the partitions of a disk cannot be chosen (S3), or NULL
 * when they can: a disk without GPT, a table diskpart does not edit, a
 * full table, another zedBSD on it (or a FAT not known), or ESPs none of
 * which is enough.
 */
const char *
inst_chosen_refusal(
	const struct inst_app *app,
	const struct inst_disk *disk)
{
	const struct inst_part *part;
	unsigned i;
	int esps;
	int same;
	int is_esp;

	/* GPT, as diskpart edits it. */
	same = strcmp(disk->table, "gpt");
	if (same != 0)
		return "The disk has no GPT partition table.";
	if (disk->restrictions != 0U)
		return "The disk's partition table cannot be edited here.";
	if (disk->entries >= DISK_ENTRIES_MAX)
		return "The disk's partition table is full.";

	/* Another zedBSD: a FAT with zedbsd.cfg, or one that could not be looked into. */
	esps = 0;
	for (i = 0; i < disk->part_count; i++) {
		part = &app->probe.part[disk->first_part + i];
		if (part->fat && part->cfg != INST_NO)
			return "Another installation of this system may be on this disk.";

		/* An ESP must have been looked into. */
		is_esp = inst_part_esp(part);
		if (is_esp) {
			esps++;
			if (!part->fat)
				return "The disk's EFI system partition cannot be read.";
		}
	}

	/* An ESP that is enough, when the disk has one. */
	if (esps > 0) {
		part = inst_disk_esp(app, disk);
		if (part == NULL)
			return "The disk's EFI system partition is too small or cannot be used.";
	}

	/* The partitions may be chosen. */
	return NULL;
}

/* Gives the first ESP of a disk that is enough for the installation, or NULL. */
const struct inst_part *
inst_disk_esp(
	const struct inst_app *app,
	const struct inst_disk *disk)
{
	const struct inst_part *found;
	const struct inst_part *part;
	unsigned i;
	int enough;

	/* The enough ESP of the lowest slot. */
	found = NULL;
	for (i = 0; i < disk->part_count; i++) {
		part = &app->probe.part[disk->first_part + i];
		enough = disk_esp_enough(app, disk, part);
		if (!enough)
			continue;
		if (found == NULL || part->slot < found->slot)
			found = part;
	}

	/* That ESP, or none. */
	return found;
}

/* Reports whether a partition is an ESP by its type. */
int
inst_part_esp(
	const struct inst_part *part)
{
	int same;

	/* The EFI system type. */
	same = strcasecmp(part->type, KERN_GPT_TYPE_EFI_SYSTEM);
	if (same == 0)
		return 1;

	/* Another type. */
	return 0;
}

/* Reports whether a partition has zedBSD's unassigned type (made by S4). */
int
inst_part_unassigned(
	const struct inst_part *part)
{
	int same;

	/* The unassigned type. */
	same = strcasecmp(part->type, KERN_GPT_TYPE_ZEDBSD_UNASSIGNED);
	if (same == 0)
		return 1;

	/* Another type. */
	return 0;
}

/* Gives the name of a partition type, or the GUID itself for a type without one. */
const char *
inst_type_name(
	const char *type)
{
	unsigned i;
	int same;

	/* Each known type. */
	for (i = 0; i < sizeof(disk_type_names) / sizeof(disk_type_names[0]); i++) {
		same = strcasecmp(type, disk_type_names[i][0]);
		if (same == 0)
			return kl_tr(disk_type_names[i][1]);
	}

	/* The GUID. */
	return type;
}

/* Writes a size for people: MiB below 1 GiB, GiB (one decimal below 100) above. */
void
inst_bytes_text(
	uint64_t bytes,
	char *text,
	size_t size)
{
	/* Below a GiB, in whole MiB. */
	if (bytes < INST_GIB) {
		(void)snprintf(text, size, "%llu MiB", (unsigned long long)(bytes / INST_MIB));
		return;
	}

	/* Below 100 GiB, with a tenth. */
	if (bytes < 100ULL * INST_GIB) {
		(void)snprintf(text, size, "%.1f GiB", (double)bytes / (double)INST_GIB);
		return;
	}

	/* Larger, in whole GiB. */
	(void)snprintf(text, size, "%llu GiB", (unsigned long long)(bytes / INST_GIB));
}

/* Gives a partition's size in bytes. */
uint64_t
inst_part_bytes(
	const struct inst_disk *disk,
	const struct inst_part *part)
{
	/* Its sectors of the disk's size. */
	return part->sectors * disk->sector_bytes;
}

/* Gives a disk's size in bytes. */
uint64_t
inst_disk_bytes(
	const struct inst_disk *disk)
{
	/* Its sectors. */
	return disk->sectors * disk->sector_bytes;
}

/*
 * Finds the largest free room of a disk on the 1 MiB grid, outside its
 * first and last MiB.  Returns 0 with it, or ENOSPC when it has none.
 */
int
inst_largest_gap(
	const struct inst_disk *disk,
	const struct inst_probe *probe,
	uint64_t *start,
	uint64_t *sectors)
{
	uint64_t gap_start[INST_PARTS_MAX + 1U];
	uint64_t gap_sectors[INST_PARTS_MAX + 1U];
	int count;
	int i;

	/* The gaps, and the largest of them. */
	count = disk_gaps(disk, probe, gap_start, gap_sectors, INST_PARTS_MAX + 1U);
	*start = 0;
	*sectors = 0;
	for (i = 0; i < count; i++) {
		if (gap_sectors[i] <= *sectors)
			continue;
		*start = gap_start[i];
		*sectors = gap_sectors[i];
	}

	/* A disk with no gap has no room. */
	if (*sectors == 0U)
		return ENOSPC;

	/* Succeeded: the largest gap. */
	return 0;
}

/*
 * Places a new partition of a size (and, when asked, a new ESP before it)
 * in the first free room it fits, on the 1 MiB grid.  Returns 0 with the
 * ESP's first sector, the partition's first sector and its size, or
 * ENOSPC.
 */
int
inst_place(
	const struct inst_disk *disk,
	const struct inst_probe *probe,
	uint64_t bytes,
	int with_esp,
	uint64_t *esp_start,
	uint64_t *start,
	uint64_t *sectors)
{
	uint64_t gap_start[INST_PARTS_MAX + 1U];
	uint64_t gap_sectors[INST_PARTS_MAX + 1U];
	uint64_t esp_sectors;
	uint64_t wanted;
	uint64_t grid;
	int count;
	int i;

	/* The sizes in sectors, on the grid. */
	grid = disk_grid(disk);
	wanted = (bytes / disk->sector_bytes) / grid * grid;
	esp_sectors = 0;
	if (with_esp)
		esp_sectors = INST_ESP_BYTES / disk->sector_bytes;
	if (wanted == 0U)
		return EINVAL;

	/* The first gap both fit in (the lowest sectors). */
	count = disk_gaps(disk, probe, gap_start, gap_sectors, INST_PARTS_MAX + 1U);
	for (i = 0; i < count; i++) {
		if (gap_sectors[i] < esp_sectors + wanted)
			continue;
		*esp_start = gap_start[i];
		*start = gap_start[i] + esp_sectors;
		*sectors = wanted;
		return 0;
	}

	/* No gap is large enough. */
	return ENOSPC;
}

/*
 * Gives the swap partition of the whole disk's layout: as large
 * as the memory, 1 to 8 GiB, 1 GiB on a disk smaller than 32 GiB.
 */
uint64_t
inst_whole_swap_bytes(
	const struct inst_app *app,
	const struct inst_disk *disk)
{
	uint64_t bytes;
	uint64_t disk_bytes;

	/* A small disk's. */
	disk_bytes = inst_disk_bytes(disk);
	if (disk_bytes < DISK_SMALL_BYTES)
		return DISK_SWAP_LOW;

	/* The memory's, within the bounds, on the MiB. */
	bytes = app->probe.memory;
	if (bytes < DISK_SWAP_LOW)
		bytes = DISK_SWAP_LOW;
	if (bytes > DISK_SWAP_HIGH)
		bytes = DISK_SWAP_HIGH;
	return bytes / INST_MIB * INST_MIB;
}

/* Gives the swap file's size: the memory's, at most 2 GiB less 4 KiB, whole pages. */
uint64_t
inst_swapfile_bytes(
	const struct inst_app *app)
{
	uint64_t bytes;

	/* The memory, within the file's bound. */
	bytes = app->probe.memory;
	if (bytes > INST_SWAPFILE_MAX)
		bytes = INST_SWAPFILE_MAX;
	return bytes & ~(uint64_t)4095U;
}

/*
 * Gives the smallest root: the units' size and a tenth, the
 * swap file when one is to be made, and 1 GiB to spare.  base_only counts
 * the base system alone (S2 and S5, before the units are chosen).
 */
uint64_t
inst_root_needed(
	const struct inst_app *app,
	int base_only)
{
	uint64_t bytes;
	int base;

	/* The units' bytes. */
	bytes = app->catalog.chosen_bytes;
	if (base_only) {
		bytes = 0;
		base = inst_catalog_unit_find(&app->catalog, "base");
		if (base >= 0)
			bytes = app->catalog.unit[base].bytes;
	}

	/* With a tenth, the swap file, and the spare. */
	bytes += bytes / 10U;
	if (!base_only && app->whole == 0 && !app->has_swap)
		bytes += inst_swapfile_bytes(app);
	return bytes + INST_ROOT_SPARE;
}

/*
 * Reads a size a person typed: a number, a tenth allowed, and MiB (the
 * default), M, GiB, G, TiB or T.  Returns 0 with the bytes, or EINVAL.
 */
int
inst_size_parse(
	const char *text,
	uint64_t *bytes)
{
	double number;
	double unit;
	char *end;

	/* The number. */
	while (*text == ' ')
		text++;
	number = strtod(text, &end);
	if (end == text || number <= 0.0)
		return EINVAL;
	while (*end == ' ')
		end++;

	/* The unit. */
	unit = (double)INST_MIB;
	if (*end == 'G' || *end == 'g')
		unit = (double)INST_GIB;
	else if (*end == 'T' || *end == 't')
		unit = (double)INST_GIB * 1024.0;
	else if (*end != 'M' && *end != 'm' && *end != '\0')
		return EINVAL;
	if (*end != '\0')
		end++;
	if (*end == 'i' || *end == 'I')
		end++;
	if (*end == 'B' || *end == 'b')
		end++;
	while (*end == ' ')
		end++;
	if (*end != '\0')
		return EINVAL;
	if (number * unit > 1.0e18)
		return EINVAL;

	/* Succeeded: whole MiB. */
	*bytes = (uint64_t)(number * unit) / INST_MIB * INST_MIB;
	if (*bytes == 0U)
		return EINVAL;
	return 0;
}

/* Cuts a line at its TABs into fields; returns how many. */
static unsigned
disk_split(
	char *line,
	char **field,
	unsigned most)
{
	unsigned count;
	char *tab;
	int same;

	/* Each field up to the next TAB ("-" is an empty one). */
	count = 0;
	while (count < most) {
		field[count] = line;
		count++;
		tab = strchr(line, '\t');
		if (tab != NULL)
			*tab = '\0';
		same = strcmp(field[count - 1U], "-");
		if (same == 0)
			field[count - 1U] = (char *)"";
		if (tab == NULL)
			break;
		line = tab + 1;
	}

	/* The fields found. */
	return count;
}

/* Copies a text into a buffer, cut to fit with its NUL. */
static void
disk_copy(
	char *target,
	size_t size,
	const char *source)
{
	/* The text, cut at the buffer's end. */
	(void)snprintf(target, size, "%s", source);
}

/* Reads yes, no or unknown. */
static enum inst_answer
disk_answer(
	const char *text)
{
	int same;

	/* The two answers; anything else is not known. */
	same = strcmp(text, "no");
	if (same == 0)
		return INST_NO;
	same = strcmp(text, "yes");
	if (same == 0)
		return INST_YES;

	/* Anything else is not known. */
	return INST_UNKNOWN;
}

/*
 * Reports whether an existing ESP is enough: a FAT the helper
 * looked into, without zedbsd.cfg or \EFI\ZEDBSD\BOOTX64.EFI, whose
 * fallback loader is known, with room for the loader and 16 MiB.
 *
 * The helper mounted the FAT to look into it, which is what shows that it
 * holds one; the partition's file system type is not asked for, since the
 * kernel names it when the partition appears and an ESP S4 makes is
 * formatted after that.
 */
static int
disk_esp_enough(
	const struct inst_app *app,
	const struct inst_disk *disk,
	const struct inst_part *part)
{
	int is_esp;

	/* The disk is part of the signature the callers share; the partition decides. */
	(void)disk;

	/* An ESP holding a FAT. */
	is_esp = inst_part_esp(part);
	if (!is_esp)
		return 0;
	if (!part->fat)
		return 0;

	/* Nothing of ours on it, and its fallback loader known. */
	if (part->cfg != INST_NO || part->zedbsd_loader != INST_NO || part->boot_loader == INST_UNKNOWN)
		return 0;

	/* Room for the loader and the spare. */
	if (part->fat_free < app->catalog.loader_bytes + INST_ESP_SPARE)
		return 0;

	/* The ESP is enough. */
	return 1;
}

/* Gives the sectors of 1 MiB on a disk. */
static uint64_t
disk_grid(
	const struct inst_disk *disk)
{
	/* A MiB of the disk's sectors. */
	return INST_MIB / disk->sector_bytes;
}

/*
 * Finds the free room of a disk on the 1 MiB grid, between its first and
 * last MiB, from the lowest sectors.  Returns how many gaps it found.
 */
static int
disk_gaps(
	const struct inst_disk *disk,
	const struct inst_probe *probe,
	uint64_t *start,
	uint64_t *sectors,
	unsigned most)
{
	const struct inst_part *part;
	uint64_t cursor;
	uint64_t limit;
	uint64_t grid;
	uint64_t next;
	uint64_t next_end;
	uint64_t from;
	uint64_t to;
	unsigned count;
	unsigned i;

	/* The room the partitions may take. */
	grid = disk_grid(disk);
	if (disk->sectors < 3U * grid)
		return 0;
	cursor = grid;
	limit = (disk->sectors - grid) / grid * grid;
	count = 0;

	/* From the lowest sectors: each partition after the cursor ends a gap. */
	while (cursor < limit && count < most) {
		/* The next partition at or after the cursor (or one that holds it). */
		next = limit;
		next_end = limit;
		for (i = 0; i < disk->part_count; i++) {
			part = &probe->part[disk->first_part + i];
			if (part->start + part->sectors <= cursor)
				continue;
			if (part->start < next) {
				next = part->start;
				next_end = part->start + part->sectors;
			}
		}

		/* A partition that began before the cursor leaves no gap before it. */
		if (next < cursor)
			next = cursor;

		/* The gap before it, on the grid. */
		from = (cursor + grid - 1U) / grid * grid;
		to = next / grid * grid;
		if (to > from) {
			start[count] = from;
			sectors[count] = to - from;
			count++;
		}

		/* Past the partition. */
		if (next >= limit)
			break;
		cursor = next_end;
	}

	/* The gaps found. */
	return (int)count;
}
