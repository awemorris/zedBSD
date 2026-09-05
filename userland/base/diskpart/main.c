/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "table.h"
#include <zedbsd/block.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int fd_read(void *context, uint64_t offset, void *data, size_t size)
{
	int fd = *(int *)context;
	ssize_t n;
	do { n = pread(fd, data, size, (off_t)offset); } while (n < 0 && errno == EINTR);
	return n < 0 ? errno : (size_t)n == size ? 0 : EIO;
}
static int fd_write(void *context, uint64_t offset, const void *data, size_t size)
{
	int fd = *(int *)context;
	ssize_t n;
	/* A short write may be partial publication: never silently retry it. */
	do { n = pwrite(fd, data, size, (off_t)offset); } while (n < 0 && errno == EINTR);
	return n < 0 ? errno : (size_t)n == size ? 0 : EIO;
}
static int fd_flush(void *context)
{ return fsync(*(int *)context) < 0 ? errno : 0; }

static int query(int fd, struct zedbsd_block_info *info)
{
	memset(info, 0, sizeof(*info));
	info->version = ZEDBSD_BLOCK_VERSION; info->struct_size = sizeof(*info);
	return ioctl(fd, BLKGETINFO, info) < 0 ? errno : 0;
}

static int device_open(const char *operand, int writable, int *fd,
	struct zedbsd_block_info *info)
{
	char path[64];
	struct stat st;
	const char *name = !strncmp(operand, "/dev/", 5) ? operand + 5 : operand;
	int error;
	if (!*name || strlen(name) >= ZEDBSD_BLOCK_NAME_MAX || strchr(name, '/')) return EINVAL;
	snprintf(path, sizeof(path), "/dev/%s", name);
	*fd = open(path, writable ? O_RDWR : O_RDONLY);
	if (*fd < 0) return errno;
	if (fstat(*fd, &st) < 0) error = errno;
	else if (!S_ISBLK(st.st_mode)) error = EINVAL;
	else error = query(*fd, info);
	if (!error && (info->parent_device || (info->flags & ZEDBSD_BLOCK_PARTITION))) error = EINVAL;
	if (!error && writable && (info->flags & ZEDBSD_BLOCK_READ_ONLY)) error = EROFS;
	if (error) { close(*fd); *fd = -1; }
	return error;
}

static void help(void)
{
	puts("usage: diskpart [list | show DISK | reload DISK | help]");
	puts("       diskpart add DISK SLOT START COUNT TYPE [PARTUUID [NAME]]");
	puts("       diskpart delete DISK SLOT");
	puts("SLOT is one-based; START/COUNT are decimal logical sectors.");
	puts("TYPE: GPT GUID (PARTUUID required), or hexadecimal primary-MBR type.");
	puts("Edits require interactive device-identity confirmation. No force mode.");
	puts("Existing matching GPT or primary MBR only; no init/format/resize/move.");
	puts("Extended MBR is displayed as a container; EBR chains are unsupported.");
	puts("GPT writes require 128-byte entries, 92-byte headers, and <=16 active slots.");
	puts("A mounted partition, even unchanged/read-only, prevents whole-disk reload.");
	puts("Write success and reload success differ; a busy disk requires reboot.");
}

static int list(void)
{
	DIR *dir = opendir("/dev");
	struct dirent *entry;
	int errors = 0;
	if (!dir) return errno;
	puts("DEVICE       REGISTRATION  SECTOR-SIZE  SECTORS  FLAGS");
	while ((entry = readdir(dir)) != NULL) {
		char path[64];
		struct stat st;
		struct zedbsd_block_info info;
		int fd, error;
		if (strlen(entry->d_name) >= ZEDBSD_BLOCK_NAME_MAX) continue;
		snprintf(path, sizeof(path), "/dev/%.*s", (int)ZEDBSD_BLOCK_NAME_MAX - 1, entry->d_name);
		if (stat(path, &st) < 0 || !S_ISBLK(st.st_mode)) continue;
		fd = open(path, O_RDONLY);
		error = fd < 0 ? errno : query(fd, &info);
		if (fd >= 0) close(fd);
		if (error) { fprintf(stderr, "diskpart: %s: %s\n", path, strerror(error)); errors = error; continue; }
		if (info.flags & ZEDBSD_BLOCK_PARTITION) continue;
		printf("%s %u %u %llu %s\n", info.name, info.device, info.sector_size,
		    (unsigned long long)info.sector_count,
		    info.flags & ZEDBSD_BLOCK_READ_ONLY ? "ro" : "rw");
	}
	closedir(dir); return errors;
}

static void show(const struct dp_table *t)
{
	printf("On-disk %s table%s%s; %u active partitions (kernel mapping may differ)\n",
	    t->format == DP_GPT ? "GPT" : "MBR",
	    t->restrictions & DP_DEGRADED ? " [degraded; editing refused]" : "",
	    t->restrictions & DP_UNSUPPORTED ? " [unsupported for editing]" : "", t->count);
	for (unsigned i = 0; i < t->count; i++) {
		const struct dp_part *p = &t->parts[i];
		printf("%u start=%llu count=%llu type=%s uuid=%s name=%s\n", p->slot,
		    (unsigned long long)p->start, (unsigned long long)p->count, p->type, p->uuid, p->name);
	}
}

static int decimal(const char *s, uint64_t *value)
{
	uint64_t n = 0;
	if (!s || !*s) return EINVAL;
	while (*s) {
		unsigned v = (unsigned char)*s++ - '0';
		if (v > 9 || n > (UINT64_MAX - v) / 10) return EINVAL;
		n = n * 10 + v;
	}
	*value = n; return 0;
}

/* Refuse editing against a stale live mapping. In particular an addition
 * cannot overlap an old root extent merely because disk bytes were changed
 * earlier and that change has not been accepted by the kernel. */
static int live_extents_match(const struct zedbsd_block_info *parent,
	const struct dp_table *t)
{
	DIR *dir = opendir("/dev");
	struct dirent *entry;
	int error = 0;
	if (!dir) return errno;
	while (!error && (entry = readdir(dir)) != NULL) {
		char path[64];
		struct stat st;
		struct zedbsd_block_info child;
		int fd, found = 0;
		if (strlen(entry->d_name) >= ZEDBSD_BLOCK_NAME_MAX) continue;
		snprintf(path, sizeof(path), "/dev/%.*s", (int)ZEDBSD_BLOCK_NAME_MAX - 1, entry->d_name);
		if (stat(path, &st) < 0 || !S_ISBLK(st.st_mode)) continue;
		fd = open(path, O_RDONLY);
		error = fd < 0 ? errno : query(fd, &child);
		if (fd >= 0) close(fd);
		if (error || child.parent_device != parent->device) continue;
		for (unsigned i = 0; i < t->count; i++)
			if (child.parent_offset == t->parts[i].start && child.sector_count == t->parts[i].count)
				found = 1;
		if (!found) error = EBUSY;
	}
	closedir(dir); return error;
}

int main(int argc, char **argv)
{
	struct zedbsd_block_info info;
	struct dp_table table;
	struct dp_io io;
	const char *verb = argc < 2 ? "list" : argv[1];
	int error = 0, fd = -1, editing, started = 0;
	uint64_t slot = 0, start = 0, count = 0;
	if (!strcmp(verb, "help") && argc == 2) { help(); return 0; }
	if (!strcmp(verb, "list") && argc <= 2) {
		error = list();
		if (error) fprintf(stderr, "diskpart: list incomplete: %s\n", strerror(error));
		return error ? 1 : 0;
	}
	editing = !strcmp(verb, "add") || !strcmp(verb, "delete");
	if ((!strcmp(verb, "show") || !strcmp(verb, "reload")) ? argc != 3 :
	    !strcmp(verb, "delete") ? argc != 4 :
	    !strcmp(verb, "add") ? argc < 7 || argc > 9 : 1) { help(); return 2; }
	if (editing && (decimal(argv[3], &slot) || !slot || slot > DP_MAX_SLOTS)) { help(); return 2; }
	if (!strcmp(verb, "add") && (decimal(argv[4], &start) || decimal(argv[5], &count))) { help(); return 2; }
	error = device_open(argv[2], editing, &fd, &info);
	if (error) goto done;
	if (!strcmp(verb, "reload")) {
		error = ioctl(fd, BLKREREADPART, 0) < 0 ? errno : 0;
		if (!error) puts("Kernel partition devices reloaded.");
		else if (error == EBUSY) puts("Disk busy: live devices unchanged; reboot required for an edited table.");
		goto done;
	}
	/* A preflight reload catches hidden mounts/claims for deletion, and makes
	 * idle devices current. An addition may continue on EBUSY only; the final
	 * notification remains authoritative. This does not reserve future use. */
	if (editing && ioctl(fd, BLKREREADPART, 0) < 0) {
		error = errno;
		if (error != EBUSY || !strcmp(verb, "delete")) goto done;
		error = 0;
	}
	io = (struct dp_io){ &fd, info.sector_count, info.sector_size, fd_read, fd_write, fd_flush };
	error = dp_load(&table, &io);
	if (error) goto done;
	printf("Target /dev/%s registration=%u sector-size=%u sectors=%llu\n", info.name,
	    info.device, info.sector_size, (unsigned long long)info.sector_count);
	show(&table);
	if (editing) {
		char expected[80], answer[96];
		error = live_extents_match(&info, &table);
		if (error) goto free_table;
		if (!strcmp(verb, "add")) {
			if (table.count >= 16) { error = ENOSPC; goto free_table; }
			error = dp_add(&table, (unsigned)slot, start, count, argv[6],
			    argc >= 8 ? argv[7] : NULL, argc >= 9 ? argv[8] : NULL);
		} else error = dp_delete(&table, (unsigned)slot);
		if (error) goto free_table;
		puts("Proposed table (no filesystem data is moved or formatted):"); show(&table);
		snprintf(expected, sizeof(expected), "WRITE %s:%u", info.name, info.device);
		printf("Single-administrator operation; not crash-atomic. Type '%s' to write: ", expected);
		fflush(stdout);
		if (!fgets(answer, sizeof(answer), stdin) || !strchr(answer, '\n')) { error = ECANCELED; goto free_table; }
		answer[strcspn(answer, "\r\n")] = 0;
		if (strcmp(answer, expected)) { error = ECANCELED; goto free_table; }
		/* Recheck after the interactive pause. Delete refuses every busy
		 * consumer, including mounts not visible in the ordinary namespace. */
		if (!strcmp(verb, "delete"))
			error = ioctl(fd, BLKREREADPART, 0) < 0 ? errno : 0;
		else error = live_extents_match(&info, &table);
		if (error) goto free_table;
		error = dp_write(&table, &started);
		if (error) {
			fprintf(stderr, "diskpart: %s\n", started ?
			    "Write/flush/verification failed; table may be partially changed. No reload attempted." :
			    "Write preflight failed; no table bytes written.");
			goto free_table;
		}
		if (ioctl(fd, BLKREREADPART, 0) < 0) {
			error = errno;
			fprintf(stderr, "diskpart: Table written and verified, but reload failed: %s.%s\n",
			    strerror(error), error == EBUSY ? " Reboot required; live devices unchanged" : " No rollback attempted");
			dp_free(&table); close(fd); return 3;
		}
		puts("Table written, flushed, verified; kernel partition devices reloaded.");
	}
free_table:
	dp_free(&table);
done:
	if (fd >= 0) close(fd);
	if (error) fprintf(stderr, "diskpart: %s: %s\n", argv[2], strerror(error));
	return error ? 1 : 0;
}
