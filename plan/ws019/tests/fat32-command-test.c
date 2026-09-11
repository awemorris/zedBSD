/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "userland/base/mkfs/block-command.h"
#include "userland/base/mkfs/fat32-format.h"
#include "userland/base/mkfs/ufs-format.h"
#include <zedbsd/block.h>
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static unsigned step, fail_at, live, reserved, writes, verifies, closes;
static unsigned readonly, nonblock, sectorsize, badname, overflow;
static const char *answer;
static struct zedbsd_block_info info;

static int event(void)
{
	step++;
	if (step == fail_at) { errno = EIO; return -1; }
	return 0;
}
int fc_open(const char *path, int flags, ...)
{
	assert(!strcmp(path, "/dev/nvme0n1p1"));
	assert((flags & (O_RDWR | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC)) == flags);
	assert(flags & O_NOFOLLOW);
	assert((flags & O_ACCMODE) == O_RDWR);
	if (event() < 0) return -1;
	live = 1; return 7;
}
int fc_fstat(int fd, struct stat *st)
{
	assert(fd == 7 && live);
	if (event() < 0) return -1;
	memset(st, 0, sizeof(*st));
	st->st_mode = nonblock ? S_IFREG : S_IFBLK;
	return 0;
}
int fc_ioctl(int fd, unsigned long request, ...)
{
	va_list ap;
	struct zedbsd_block_info *p;
	assert(fd == 7 && live);
	va_start(ap, request); p = va_arg(ap, struct zedbsd_block_info *); va_end(ap);
	if (event() < 0) return -1;
	if (request == BLKGETINFO) {
		assert(p->version == ZEDBSD_BLOCK_VERSION && p->struct_size == sizeof(*p));
		memset(&info, 0, sizeof(info));
		info.version = ZEDBSD_BLOCK_VERSION; info.struct_size = sizeof(info);
		info.device = 123; info.parent_device = 122;
		info.sector_size = sectorsize; info.sector_count = 131072;
		if (overflow) info.sector_count = UINT64_MAX;
		info.parent_offset = 2048; info.flags = ZEDBSD_BLOCK_PARTITION;
		if (readonly) info.flags |= ZEDBSD_BLOCK_READ_ONLY;
		strcpy(info.name, badname ? "other" : "nvme0n1p1");
		*p = info;
	} else {
		assert(request == BLKRESERVE && !reserved && !memcmp(p, &info, sizeof(info)));
		reserved = 1;
	}
	return 0;
}
int fc_close(int fd)
{
	assert(fd == 7 && live);
	live = reserved = 0; closes++;
	return event();
}
int fc_fflush(FILE *file)
{
	assert(file == stdout);
	return event();
}
char *fc_fgets(char *buffer, int size, FILE *file)
{
	assert(file == stdin && live && reserved);
	if (event() < 0 || answer == NULL) return NULL;
	assert(strlen(answer) < (size_t)size);
	strcpy(buffer, answer); return buffer;
}
int fat32_format_geometry(uint64_t count, uint32_t size, uint64_t hidden,
    uint32_t serial, struct fat32_format_geometry *g)
{
	assert(live && !reserved && count == 131072 && size == 512 && hidden == 2048 && serial == 123);
	if (event() < 0) return EIO;
	memset(g, 0, sizeof(*g)); return 0;
}
int fat32_format_write(int fd, const struct fat32_format_geometry *g)
{
	(void)g; assert(fd == 7 && live && reserved); writes++;
	return event() < 0 ? EIO : 0;
}
int fat32_format_verify(int fd, const struct fat32_format_geometry *g)
{
	(void)g; assert(fd == 7 && live && reserved && writes == 1); verifies++;
	return event() < 0 ? EIO : 0;
}

static void reset(void)
{
	step = fail_at = live = reserved = writes = verifies = closes = 0;
	readonly = nonblock = badname = overflow = 0; sectorsize = 512;
	answer = "FORMAT nvme0n1p1:123\n";
}

int ufs_format_native_validate_size(uint64_t bytes)
{
	assert(bytes == 131072U * 512U && live && !reserved);
	return event() < 0 ? EIO : 0;
}
int ufs_format_native_capacity(uint64_t bytes, struct ufs_format_capacity *capacity)
{
	(void)bytes;
	(void)capacity;
	assert(!"reserved formatting must not call the public capacity query");
	return EINVAL;
}
int ufs_format_native_write(int fd, uint64_t bytes)
{
	assert(bytes == 131072U * 512U);
	return fat32_format_write(fd, NULL);
}
int ufs_format_native_verify(int fd, uint64_t bytes)
{
	assert(bytes == 131072U * 512U);
	return fat32_format_verify(fd, NULL);
}

int main(void)
{
	char *argv[] = {"mkfs", "-t", "fat32", "nvme0n1p1", NULL};
	char *native_argv[] = {"mkfs", "-t", "ufs", "--profile=native", "nvme0n1p1", NULL};
	const char *invalid[] = {NULL, "", "NO\n", "FORMAT nvme0n1p1:124\n",
	    "FORMAT nvme0n1p1:123", "FORMAT nvme0n1p1:\r123\n", "FORMAT nvme0n1p1:123\nextra"};
	unsigned count, n;
	reset(); assert(mkfs_block_command(4, argv) == 0);
	assert(!live && !reserved && writes == 1 && verifies == 1 && closes == 1);
	count = step;
	for (n = 1; n <= count; n++) {
		reset(); fail_at = n;
		assert(mkfs_block_command(4, argv) == 1);
		assert(!live && !reserved && closes == (n != 1));
	}
	for (n = 0; n < sizeof(invalid) / sizeof(invalid[0]); n++) {
		reset(); answer = invalid[n];
		assert(mkfs_block_command(4, argv) == 1 && writes == 0 && !live);
	}
	reset(); answer = "FORMAT nvme0n1p1:123\r\n";
	assert(mkfs_block_command(4, argv) == 0);
	for (n = 0; n < 4; n++) {
		reset();
		if (n == 0) readonly = 1;
		if (n == 1) nonblock = 1;
		if (n == 2) badname = 1;
		if (n == 3) sectorsize = 4096;
		assert(mkfs_block_command(4, argv) == 1 && !live && !writes);
	}
	reset(); argv[3] = "../bad";
	assert(mkfs_block_command(4, argv) == 2 && step == 0);
	assert(mkfs_block_command(3, argv) == 2 && step == 0);
	reset(); assert(mkfs_block_command(5, native_argv) == 0);
	assert(!live && writes == 1 && verifies == 1);
	count = step;
	for (n = 1; n <= count; n++) {
		reset(); fail_at = n;
		assert(mkfs_block_command(5, native_argv) == 1 && !live && !reserved);
	}
	for (n = 0; n < sizeof(invalid) / sizeof(invalid[0]); n++) {
		reset(); answer = invalid[n];
		assert(mkfs_block_command(5, native_argv) == 1 && !writes && !live);
	}
	reset(); assert(mkfs_block_command(4, native_argv) == 2 && !step);
	reset(); overflow = 1;
	assert(mkfs_block_command(5, native_argv) == 1 && !writes && !live);
	reset(); overflow = 1; argv[3] = "nvme0n1p1";
	assert(mkfs_block_command(4, argv) == 1 && !writes && !live);
	puts("FAT32 command lifecycle, refusal and faults PASS");
	return 0;
}
