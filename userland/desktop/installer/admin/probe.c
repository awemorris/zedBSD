/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The probe (probe.h): the block devices under /dev as the kernel
 * describes them, the mount table and the swap sources, the partition
 * tables through diskpart's codec, and each FAT mounted read-only for a
 * moment to see its room and whether another system's files are on it.
 */

#include "probe.h"

#include "userland/base/common/sha256.h"
#include "userland/base/diskpart/table.h"

#include <uapi/gpt-type.h>
#include <uapi/mountinfo.h>
#include <uapi/sysctl.h>
#include <uapi/system.h>

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/sysctl.h>
#include <unistd.h>

/* The longest selector the kernel keeps for the ESP the loader came from. */
#define PROBE_SELECTOR_MAX	160U

/* The answers about a FAT's files. */
enum probe_answer {
	PROBE_NO = 0,
	PROBE_YES,
	PROBE_UNKNOWN
};

/* What one look into a FAT found. */
struct probe_fat {
	uint64_t free_bytes;
	enum probe_answer cfg;
	enum probe_answer boot_loader;
	enum probe_answer zedbsd_loader;
};

static int probe_query(int fd, struct kern_block_info *info);
static void probe_mark_mounts(struct probe_scan *scan);
static void probe_mark_swaps(struct probe_scan *scan);
static void probe_mark_firmware(struct probe_scan *scan, int *uefi);
static struct probe_device *probe_by_name(struct probe_scan *scan, const char *name);
static void probe_mark_live(struct probe_scan *scan, struct probe_device *device);
static int probe_device_read(void *context, uint64_t offset, void *data, size_t size);
static void probe_print_text(FILE *output, const char *text);
static const char *probe_answer_text(enum probe_answer answer);
static enum probe_answer probe_file_answer(const char *root, const char *path);
static void probe_look_into_fat(const struct probe_device *device, struct probe_fat *fat);
static void probe_report_memory(FILE *output);
static void probe_report_disk(FILE *output, const struct probe_scan *scan, const struct probe_device *disk);

/*
 * Scans /dev for block devices and marks how the running system uses
 * them: mounted, a swap source, or on a disk the system lives on.
 * Returns 0, or the errno value of opening /dev.  A device that cannot be
 * opened is left out.
 */
int
probe_scan_devices(
	struct probe_scan *scan)
{
	struct probe_device *device;
	struct dirent *entry;
	struct stat status;
	char path[KERN_BLOCK_NAME_MAX + 8U];
	DIR *directory;
	size_t length;
	mode_t type;
	int fd;
	int error;

	/* The directory of the devices. */
	memset(scan, 0, sizeof(*scan));
	directory = opendir("/dev");
	if (directory == NULL)
		return errno;

	/* Each block device the kernel describes. */
	for (;;) {
		entry = readdir(directory);
		if (entry == NULL)
			break;
		length = strlen(entry->d_name);
		if (length >= KERN_BLOCK_NAME_MAX || scan->count >= PROBE_DEVICES_MAX)
			continue;
		snprintf(path, sizeof(path), "/dev/%s", entry->d_name);
		error = stat(path, &status);
		if (error != 0)
			continue;
		type = status.st_mode & S_IFMT;
		if (type != S_IFBLK)
			continue;

		/* Its description, and its identity when it has one. */
		fd = open(path, O_RDONLY | O_CLOEXEC);
		if (fd < 0)
			continue;
		device = &scan->device[scan->count];
		memset(device, 0, sizeof(*device));
		error = probe_query(fd, &device->info);
		if (error == 0) {
			error = ioctl(fd, BLKGETIDENTITY, &device->identity);
			if (error == 0)
				device->has_identity = 1;
			scan->count++;
		}

		/* Closes the device once it has been described. */
		close(fd);
	}

	/* Closes the directory of the devices. */
	closedir(directory);

	/* How the running system uses them. */
	probe_mark_mounts(scan);
	probe_mark_swaps(scan);
	probe_mark_firmware(scan, &scan->uefi);

	/* Succeeded: the devices are known. */
	return 0;
}

/* Finds a device of a scan by its registration number. */
const struct probe_device *
probe_find_device(
	const struct probe_scan *scan,
	uint32_t registration)
{
	unsigned i;

	/* Each device. */
	for (i = 0; i < scan->count; i++) {
		if (scan->device[i].info.device == registration)
			return &scan->device[i];
	}

	/* No device has the number. */
	return NULL;
}

/*
 * Finds the partition the kernel maps on a disk at a range of sectors,
 * the way a request's tuple names one.
 */
const struct probe_device *
probe_find_partition(
	const struct probe_scan *scan,
	uint32_t disk,
	uint64_t start,
	uint64_t sectors)
{
	const struct probe_device *device;
	unsigned i;

	/* Each partition of the disk. */
	for (i = 0; i < scan->count; i++) {
		device = &scan->device[i];
		if (device->info.parent_device != disk)
			continue;
		if (device->info.parent_offset == start && device->info.sector_count == sectors)
			return device;
	}

	/* The kernel maps nothing there. */
	return NULL;
}

/*
 * Reads a disk's partition table with diskpart's codec, through a
 * read-only descriptor closed before the return (the table is not written
 * through it).  Returns 0, EINVAL for a disk with no table the codec
 * reads, or another errno value.
 */
int
probe_table_load(
	const struct probe_device *disk,
	struct dp_table *table)
{
	struct dp_io io;
	char path[KERN_BLOCK_NAME_MAX + 8U];
	int fd;
	int error;

	/* The disk, read-only. */
	snprintf(path, sizeof(path), "/dev/%s", disk->info.name);
	fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return errno;

	/* The table. */
	memset(table, 0, sizeof(*table));
	memset(&io, 0, sizeof(io));
	io.context = &fd;
	io.sectors = disk->info.sector_count;
	io.sector_size = disk->info.sector_size;
	io.read = probe_device_read;
	error = dp_load(table, &io);
	close(fd);
	table->io.context = NULL;
	if (error != 0)
		return error;

	/* Succeeded: the table is the caller's to free. */
	return 0;
}

/*
 * Takes the SHA-256 of a partition table as a request quotes it: its
 * format, restrictions and slots, then each entry's slot, range, type,
 * PARTUUID, attributes and name, one line each.
 */
void
probe_table_digest(
	const struct dp_table *table,
	uint8_t digest[32])
{
	struct command_sha256_context context;
	const struct dp_part *part;
	char line[256];
	int length;
	unsigned i;

	/* The table's own fields. */
	command_sha256_init(&context);
	length = snprintf(line, sizeof(line), "table %u %u %u\n", table->format, table->restrictions, table->slots);
	(void)command_sha256_update(&context, (const uint8_t *)line, (size_t)length);

	/* Each entry. */
	for (i = 0; i < table->count; i++) {
		part = &table->parts[i];
		length = snprintf(
			line,
			sizeof(line),
			"part %u %llu %llu %s %s %llu %s\n",
			part->slot,
			(unsigned long long)part->start,
			(unsigned long long)part->count,
			part->type,
			part->uuid,
			(unsigned long long)part->attributes,
			part->name);
		if (length < 0 || (size_t)length >= sizeof(line))
			length = (int)sizeof(line) - 1;
		(void)command_sha256_update(&context, (const uint8_t *)line, (size_t)length);
	}

	/* Finishes the digest. */
	command_sha256_final(&context, digest);
}

/*
 * Writes the probe's records (probe.h) to an output.  Returns 0, or an
 * errno value when the devices cannot be scanned or the output fails.
 */
int
probe_report(
	FILE *output)
{
	struct probe_scan *scan;
	unsigned i;
	int failed;
	int error;

	/* The devices; the scan is too large for the stack. */
	scan = malloc(sizeof(*scan));
	if (scan == NULL)
		return ENOMEM;
	error = probe_scan_devices(scan);
	if (error != 0) {
		free(scan);
		return error;
	}

	/* The firmware the system started from, and the memory. */
	if (scan->uefi)
		fprintf(output, "firmware\tuefi\n");
	else
		fprintf(output, "firmware\tother\n");
	probe_report_memory(output);

	/* Each disk, with its partitions. */
	for (i = 0; i < scan->count; i++) {
		if ((scan->device[i].info.flags & KERN_BLOCK_PARTITION) != 0U || scan->device[i].info.parent_device != 0U)
			continue;
		probe_report_disk(output, scan, &scan->device[i]);
	}

	/* Ends the records and releases the scan. */
	fprintf(output, "end\n");
	free(scan);

	/* The output, whole. */
	error = fflush(output);
	failed = ferror(output);
	if (error != 0 || failed)
		return EIO;

	/* Succeeded: the records are written. */
	return 0;
}

/* Asks the kernel to describe a block device. */
static int
probe_query(
	int fd,
	struct kern_block_info *info)
{
	int error;

	/* The request, its version and size given. */
	memset(info, 0, sizeof(*info));
	info->version = KERN_BLOCK_VERSION;
	info->struct_size = sizeof(*info);
	error = ioctl(fd, BLKGETINFO, info);
	if (error != 0)
		return errno;

	/* Succeeded: the description. */
	return 0;
}

/*
 * Marks the mounted devices; the root's, the boot FAT's (under /boot)
 * and /opt's make their disk live.
 */
static void
probe_mark_mounts(
	struct probe_scan *scan)
{
	struct kern_mount_query *query;
	const struct kern_mount_info *entry;
	struct probe_device *device;
	char source[KERN_MOUNT_INFO_PATH_MAX + 1U];
	unsigned i;
	int root_diff;
	int boot_diff;
	int opt_diff;
	int fd;
	int error;

	/* Room for the whole table. */
	query = calloc(1, sizeof(*query) + KERN_MOUNT_INFO_MAX * sizeof(query->entries[0]));
	if (query == NULL)
		return;
	query->version = KERN_MOUNT_INFO_VERSION;
	query->struct_size = sizeof(*query);
	query->capacity = KERN_MOUNT_INFO_MAX;

	/* The kernel's mount table, read once. */
	fd = open("/dev/system", O_RDONLY | O_CLOEXEC);
	if (fd < 0) {
		free(query);
		return;
	}

	/* Takes the table and closes the device. */
	error = ioctl(fd, KERN_SYSTEM_GET_MOUNTS, query);
	close(fd);
	if (error != 0) {
		free(query);
		return;
	}

	/* Each entry mounted from a block device (its source is the device's name). */
	for (i = 0; i < query->count && i < KERN_MOUNT_INFO_MAX; i++) {
		entry = &query->entries[i];
		if (entry->device == 0U || (entry->kind & KERN_MOUNT_INFO_BIND) != 0U)
			continue;
		snprintf(source, sizeof(source), "%.*s", (int)sizeof(entry->source), entry->source);
		device = probe_by_name(scan, source);
		if (device == NULL)
			continue;
		device->mounted = 1;

		/* The running system's own filesystems. */
		root_diff = strcmp(entry->target, "/");
		boot_diff = strncmp(entry->target, "/boot", 5U);
		opt_diff = strncmp(entry->target, "/opt", 4U);
		if (root_diff == 0 || boot_diff == 0 || opt_diff == 0)
			probe_mark_live(scan, device);
	}

	/* Releases the table. */
	free(query);
}

/* Marks the devices the kernel swaps to, which make their disk live. */
static void
probe_mark_swaps(
	struct probe_scan *scan)
{
	struct system_swap_source_info info;
	struct probe_device *device;
	const char *name;
	unsigned source;
	int prefix;
	int fd;
	int error;

	/* The system device. */
	fd = open("/dev/system", O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return;

	/* Each source the kernel may have. */
	for (source = 0; source < KERN_SYSTEM_SWAP_SOURCE_COUNT; source++) {
		memset(&info, 0, sizeof(info));
		info.version = KERN_SYSTEM_SWAP_VERSION;
		info.struct_size = sizeof(info);
		info.source_id = source;
		error = ioctl(fd, KERN_SYSTEM_GET_SWAP_SOURCE, &info);
		if (error != 0 || info.state == KERN_SYSTEM_SWAP_STATE_INACTIVE)
			continue;

		/* A device's name, under /dev or bare; a file is on the root, live already. */
		name = info.source;
		prefix = strncmp(name, "/dev/", 5U);
		if (prefix == 0)
			name += 5;
		device = probe_by_name(scan, name);
		if (device == NULL)
			continue;
		device->swap = 1;
		probe_mark_live(scan, device);
	}

	/* Closes the system device. */
	close(fd);
}

/*
 * Marks the ESP the loader came from (the kernel keeps its selector,
 * UUID= or PARTUUID=), which makes its disk live, and reports whether
 * there was one: a start through the UEFI loader.
 */
static void
probe_mark_firmware(
	struct probe_scan *scan,
	int *uefi)
{
	static const int name[2] = { CTL_KERN, KERN_BOOT_FIRMWARE };
	char selector[PROBE_SELECTOR_MAX];
	struct probe_device *device;
	const char *value;
	size_t length;
	unsigned i;
	int same;
	int partuuid_diff;
	int uuid_diff;
	int error;

	/* The selector, empty when no UEFI loader gave one. */
	*uefi = 0;
	length = sizeof(selector);
	memset(selector, 0, sizeof(selector));
	error = sysctl(name, 2, selector, &length, NULL, 0);
	if (error != 0 || selector[0] == '\0')
		return;
	selector[sizeof(selector) - 1U] = '\0';
	*uefi = 1;

	/* The device the selector names. */
	for (i = 0; i < scan->count; i++) {
		device = &scan->device[i];
		if (!device->has_identity)
			continue;
		same = 1;
		partuuid_diff = strncmp(selector, "PARTUUID=", 9U);
		uuid_diff = strncmp(selector, "UUID=", 5U);
		if (partuuid_diff == 0) {
			value = selector + 9;
			if ((device->identity.flags & KERN_BLKID_PARTUUID) != 0U)
				same = strcasecmp(device->identity.partuuid, value);
		} else if (uuid_diff == 0) {
			value = selector + 5;
			if ((device->identity.flags & KERN_BLKID_UUID) != 0U)
				same = strcasecmp(device->identity.uuid, value);
		}

		/* Marks the disk of the device the selector names. */
		if (same == 0)
			probe_mark_live(scan, device);
	}
}

/* Finds a device of a scan by its name under /dev. */
static struct probe_device *
probe_by_name(
	struct probe_scan *scan,
	const char *name)
{
	unsigned i;
	int same;

	/* Each device. */
	for (i = 0; i < scan->count; i++) {
		same = strncmp(scan->device[i].info.name, name, sizeof(scan->device[i].info.name));
		if (same == 0)
			return &scan->device[i];
	}

	/* No device has the name. */
	return NULL;
}

/* Marks the disk a device is on (the device itself for a disk) as live. */
static void
probe_mark_live(
	struct probe_scan *scan,
	struct probe_device *device)
{
	unsigned i;

	/* A disk is its own. */
	if (device->info.parent_device == 0U) {
		device->live = 1;
		return;
	}

	/* The partition's disk. */
	for (i = 0; i < scan->count; i++) {
		if (scan->device[i].info.device == device->info.parent_device)
			scan->device[i].live = 1;
	}

	/* The partition itself. */
	device->live = 1;
}

/* Reads bytes of a disk for diskpart's codec. */
static int
probe_device_read(
	void *context,
	uint64_t offset,
	void *data,
	size_t size)
{
	ssize_t count;
	int fd;

	/* One positioned read, again when a signal cut it. */
	fd = *(int *)context;
	do {
		count = pread(fd, data, size, (off_t)offset);
	} while (count < 0 && errno == EINTR);
	if (count < 0)
		return errno;
	if ((size_t)count != size)
		return EIO;

	/* Succeeded: the bytes are read. */
	return 0;
}

/* Writes a name or a label as one field: "-" for none, '?' for a TAB or a control character. */
static void
probe_print_text(
	FILE *output,
	const char *text)
{
	size_t i;
	unsigned char c;

	/* Nothing. */
	if (text == NULL || text[0] == '\0') {
		fputc('-', output);
		return;
	}

	/* Each byte. */
	for (i = 0; text[i] != '\0'; i++) {
		c = (unsigned char)text[i];
		if (c < 0x20U || c == 0x7fU)
			c = '?';
		fputc(c, output);
	}
}

/* Names an answer about a FAT's file. */
static const char *
probe_answer_text(
	enum probe_answer answer)
{
	/* The three answers. */
	if (answer == PROBE_YES)
		return "yes";
	if (answer == PROBE_NO)
		return "no";
	return "unknown";
}

/* Reports whether a path is under a mounted FAT. */
static enum probe_answer
probe_file_answer(
	const char *root,
	const char *path)
{
	char full[256];
	struct stat status;
	int error;

	/* The path under the mount point. */
	snprintf(full, sizeof(full), "%s/%s", root, path);
	error = lstat(full, &status);
	if (error == 0)
		return PROBE_YES;
	if (errno == ENOENT || errno == ENOTDIR)
		return PROBE_NO;

	/* Neither is known. */
	return PROBE_UNKNOWN;
}

/*
 * Mounts a FAT read-only for a moment and reports its room and the files
 * that make it another system's.  A FAT that is mounted already (perhaps
 * for writing) or cannot be mounted is unknown throughout.
 */
static void
probe_look_into_fat(
	const struct probe_device *device,
	struct probe_fat *fat)
{
	struct mount_args arguments;
	struct statvfs room;
	int error;

	/* Unknown until it is looked into. */
	fat->free_bytes = 0;
	fat->cfg = PROBE_UNKNOWN;
	fat->boot_loader = PROBE_UNKNOWN;
	fat->zedbsd_loader = PROBE_UNKNOWN;
	if (device->mounted)
		return;

	/* The mount, read-only, running and set-ID nothing. */
	memset(&arguments, 0, sizeof(arguments));
	arguments.size = sizeof(arguments);
	arguments.version = KERN_MOUNT_ARGS_VERSION;
	snprintf(arguments.fspec, sizeof(arguments.fspec), "%s", device->info.name);
	error = mount("fat", PROBE_MOUNT_POINT, (int)(MNT_RDONLY | MNT_NOSUID | MNT_NOEXEC), &arguments);
	if (error != 0)
		return;

	/* The room, and the files. */
	error = statvfs(PROBE_MOUNT_POINT, &room);
	if (error == 0)
		fat->free_bytes = (uint64_t)room.f_bavail * (uint64_t)room.f_frsize;
	fat->cfg = probe_file_answer(PROBE_MOUNT_POINT, "zedbsd.cfg");
	fat->boot_loader = probe_file_answer(PROBE_MOUNT_POINT, "EFI/BOOT/BOOTX64.EFI");
	fat->zedbsd_loader = probe_file_answer(PROBE_MOUNT_POINT, "EFI/ZEDBSD/BOOTX64.EFI");

	/* Gone again at once; a FAT left mounted is not one the installer may use. */
	error = unmount(PROBE_MOUNT_POINT, 0);
	if (error != 0) {
		fat->cfg = PROBE_UNKNOWN;
		fat->boot_loader = PROBE_UNKNOWN;
		fat->zedbsd_loader = PROBE_UNKNOWN;
	}
}

/* Writes the memory the system manages. */
static void
probe_report_memory(
	FILE *output)
{
	static const int name[2] = { CTL_HW, HW_MEMORY_STATS };
	struct memory_stats stats;
	size_t length;
	int error;

	/* The kernel's counts; 0 when they cannot be read. */
	memset(&stats, 0, sizeof(stats));
	length = sizeof(stats);
	error = sysctl(name, 2, &stats, &length, NULL, 0);
	if (error != 0)
		stats.physical_managed_bytes = 0;
	fprintf(output, "memory\t%llu\n", (unsigned long long)stats.physical_managed_bytes);
}

/* Writes a disk's record, its partitions' and its FATs'. */
static void
probe_report_disk(
	FILE *output,
	const struct probe_scan *scan,
	const struct probe_device *disk)
{
	const struct probe_device *device;
	const struct dp_part *part;
	struct dp_table table;
	struct probe_fat fat;
	uint8_t digest[32];
	const char *kind;
	const char *format;
	unsigned i;
	const char *type_text;
	const char *uuid_text;
	const char *label_text;
	unsigned restrictions;
	unsigned entries;
	int esp;
	int esp_diff;
	int fs_fat;
	int vfat_diff;
	int disk_diff;
	int error;

	/* The kind of disk, by what the kernel says of it. */
	kind = "fixed";
	if ((disk->info.flags & KERN_BLOCK_REMOVABLE) != 0U)
		kind = "removable";
	disk_diff = strncmp(disk->info.name, "nvme", 4U);
	if (disk_diff == 0)
		kind = "nvme";

	/* Its table: GPT, MBR, or none the codec reads. */
	error = probe_table_load(disk, &table);
	format = "none";
	if (error == 0 && table.format == DP_GPT)
		format = "gpt";
	else if (error == 0 && table.format == DP_MBR)
		format = "mbr";
	memset(digest, 0, sizeof(digest));
	if (error == 0)
		probe_table_digest(&table, digest);

	/* The table's restrictions and entries, none for a disk without one. */
	restrictions = 0U;
	entries = 0U;
	if (error == 0) {
		restrictions = table.restrictions;
		entries = table.count;
	}

	/* The disk's record. */
	fprintf(
		output,
		"disk\t%s\t%u\t%s\t%u\t%llu\t%u\t%d\t%s\t%u\t%u\t",
		disk->info.name,
		disk->info.device,
		kind,
		disk->info.sector_size,
		(unsigned long long)disk->info.sector_count,
		disk->info.flags,
		disk->live,
		format,
		restrictions,
		entries);
	for (i = 0; i < 32U; i++)
		fprintf(output, "%02x", digest[i]);
	fputc('\n', output);
	if (error != 0)
		return;

	/* Each entry of the table, with the device the kernel maps there. */
	for (i = 0; i < table.count; i++) {
		part = &table.parts[i];
		device = probe_find_partition(scan, disk->info.device, part->start, part->count);
		fprintf(
			output,
			"part\t%u\t%u\t%llu\t%llu\t%s\t%s\t%llu\t",
			disk->info.device,
			part->slot,
			(unsigned long long)part->start,
			(unsigned long long)part->count,
			part->type,
			part->uuid,
			(unsigned long long)part->attributes);

		/* The identity of the device, when it has one. */
		type_text = NULL;
		uuid_text = NULL;
		label_text = NULL;
		if (device != NULL && device->has_identity) {
			type_text = device->identity.type;
			uuid_text = device->identity.uuid;
			label_text = device->identity.label;
		}

		/* The device the kernel maps there, or dashes for none. */
		if (device == NULL) {
			fprintf(output, "-\t-\t-\t0\t0\t");
		} else {
			fprintf(output, "%s\t", device->info.name);
			probe_print_text(output, type_text);
			fputc('\t', output);
			probe_print_text(output, uuid_text);
			fprintf(output, "\t%d\t%d\t", device->mounted, device->swap);
		}

		/* The partition's name and the file system's label. */
		probe_print_text(output, part->name);
		fputc('\t', output);
		probe_print_text(output, label_text);
		fputc('\n', output);

		/* A FAT, or an ESP whatever it holds, is looked into. */
		esp_diff = strcasecmp(part->type, KERN_GPT_TYPE_EFI_SYSTEM);
		esp = 0;
		if (esp_diff == 0)
			esp = 1;
		fs_fat = 0;
		if (device != NULL && device->has_identity) {
			vfat_diff = strcmp(device->identity.type, "vfat");
			if (vfat_diff == 0)
				fs_fat = 1;
		}

		/* Skips what is neither a FAT nor an ESP. */
		if (!esp && !fs_fat)
			continue;

		/* Looks into the FAT, or reports it unknown when no device is mapped. */
		if (device == NULL) {
			fat.free_bytes = 0;
			fat.cfg = PROBE_UNKNOWN;
			fat.boot_loader = PROBE_UNKNOWN;
			fat.zedbsd_loader = PROBE_UNKNOWN;
		} else {
			probe_look_into_fat(device, &fat);
		}

		/* The FAT's record. */
		fprintf(
			output,
			"fat\t%u\t%u\t%llu\t%s\t%s\t%s\n",
			disk->info.device,
			part->slot,
			(unsigned long long)fat.free_bytes,
			probe_answer_text(fat.cfg),
			probe_answer_text(fat.boot_loader),
			probe_answer_text(fat.zedbsd_loader));
	}

	/* Releases the table. */
	dp_free(&table);
}
