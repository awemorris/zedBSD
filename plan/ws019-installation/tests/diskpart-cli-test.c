/* Production CLI and parser, memory-only syscall/confirmation adapters.
 * Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#define _POSIX_C_SOURCE 200809L
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <zedbsd/block.h>

static int fake_open(const char *, int, ...);
static int fake_close(int);
static int fake_fstat(int, struct stat *);
static int fake_stat(const char *, struct stat *);
static int fake_ioctl(int, unsigned long, ...);
static ssize_t fake_pread(int, void *, size_t, off_t);
static ssize_t fake_pwrite(int, const void *, size_t, off_t);
static int fake_fsync(int);
static DIR *fake_opendir(const char *);
static struct dirent *fake_readdir(DIR *);
static int fake_closedir(DIR *);
static char *fake_fgets(char *, int, FILE *);
#define open fake_open
#define close fake_close
#define fstat fake_fstat
#define stat(...) fake_stat(__VA_ARGS__)
#define ioctl fake_ioctl
#define pread fake_pread
#define pwrite fake_pwrite
#define fsync fake_fsync
#define opendir fake_opendir
#define readdir fake_readdir
#define closedir fake_closedir
#define fgets fake_fgets
#define main diskpart_main
#include "userland/base/diskpart/main.c"
#undef main
#undef open
#undef close
#undef fstat
#undef stat
#undef ioctl
#undef pread
#undef pwrite
#undef fsync
#undef opendir
#undef readdir
#undef closedir
#undef fgets

static unsigned checks, reads, writes, syncs, reloads, opens, closes, confirms;
static unsigned flags, enumerate, directory_at;
static int regular, write_error, flush_error, reload_errors[4];
static const char *answer;
static uint8_t media[8192];
#define CHECK(x) do { checks++; if (!(x)) { \
	fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); abort(); \
} } while (0)
static int fail(int error) { errno = error; return -1; }
static int fake_open(const char *path, int mode, ...)
{ (void)mode; CHECK(!strcmp(path, "/dev/mock") || !strcmp(path, "/dev/child")); opens++; return !strcmp(path, "/dev/mock") ? 42 : 43; }
static int fake_close(int fd)
{ CHECK(fd == 42 || fd == 43); closes++; return 0; }
static int fake_fstat(int fd, struct stat *st)
{ (void)fd; memset(st, 0, sizeof(*st)); st->st_mode = regular ? 0100000 : 0060000; return 0; }
static int fake_stat(const char *path, struct stat *st)
{ (void)path; return fake_fstat(43, st); }
static int fake_ioctl(int fd, unsigned long op, ...)
{
	if (op == BLKGETINFO) {
		va_list ap; struct zedbsd_block_info *i;
		va_start(ap, op); i = va_arg(ap, struct zedbsd_block_info *); va_end(ap);
		CHECK(i->version == 1 && i->struct_size == sizeof(*i));
		i->device = fd == 42 ? 7 : 8; strcpy(i->name, fd == 42 ? "mock" : "child");
		i->sector_size = 512; i->sector_count = fd == 42 ? 16 : 3;
		i->parent_device = fd == 42 ? 0 : 7; i->parent_offset = 4;
		i->flags = fd == 42 ? flags : ZEDBSD_BLOCK_PARTITION;
		return 0;
	}
	CHECK(fd == 42 && op == BLKREREADPART && reloads < 4);
	int e = reload_errors[reloads++]; return e ? fail(e) : 0;
}
static ssize_t fake_pread(int fd, void *p, size_t n, off_t at)
{ CHECK(fd == 42 && at >= 0 && (uint64_t)at + n <= sizeof(media)); reads++; memcpy(p, media + at, n); return (ssize_t)n; }
static ssize_t fake_pwrite(int fd, const void *p, size_t n, off_t at)
{ CHECK(fd == 42 && at >= 0 && (uint64_t)at + n <= sizeof(media)); writes++; if (write_error) return fail(write_error); memcpy(media + at, p, n); return (ssize_t)n; }
static int fake_fsync(int fd)
{ CHECK(fd == 42); syncs++; return flush_error ? fail(flush_error) : 0; }
static DIR *fake_opendir(const char *path)
{ CHECK(!strcmp(path, "/dev")); directory_at = 0; return (DIR *)(uintptr_t)1; }
static struct dirent *fake_readdir(DIR *dir)
{ static struct dirent e; (void)dir; if (!enumerate || directory_at++) return NULL; strcpy(e.d_name, "child"); return &e; }
static int fake_closedir(DIR *dir) { (void)dir; return 0; }
static char *fake_fgets(char *p, int n, FILE *f)
{ (void)f; confirms++; if (!answer) return NULL; CHECK(strlen(answer) < (unsigned)n); strcpy(p, answer); return p; }
static void reset(void)
{
	reads = writes = syncs = reloads = opens = closes = confirms = 0;
	flags = enumerate = regular = write_error = flush_error = 0;
	memset(reload_errors, 0, sizeof(reload_errors));
	answer = "WRITE mock:7\n";
	memset(media, 0, sizeof(media)); media[510] = 0x55; media[511] = 0xaa;
	media[450] = 0x83; media[454] = 4; media[458] = 2;
}
static int run(char **argv)
{ int n = 0, result; while (argv[n]) n++; result = diskpart_main(n, argv); CHECK(opens == closes); return result; }
int main(void)
{
	char *add[] = {"diskpart", "add", "mock", "2", "8", "2", "83", NULL};
	char *del[] = {"diskpart", "delete", "mock", "1", NULL};
	char *show_args[] = {"diskpart", "show", "mock", NULL};
	char *reload[] = {"diskpart", "reload", "mock", NULL};
	reset(); CHECK(run(show_args) == 0 && reads && !writes && !reloads);
	reset(); CHECK(run(add) == 0 && writes == 1 && syncs == 1 && reloads == 2);
	reset(); CHECK(run(del) == 0 && writes == 1 && reloads == 3);
	reset(); CHECK(run(reload) == 0 && !reads && !writes && reloads == 1);
	reset(); reload_errors[0] = EBUSY; CHECK(run(reload) == 1 && !writes);
	reset(); flags = ZEDBSD_BLOCK_PARTITION; CHECK(run(add) == 1 && !writes && !reloads);
	reset(); flags = ZEDBSD_BLOCK_READ_ONLY; CHECK(run(add) == 1 && !writes && !reloads);
	reset(); regular = 1; CHECK(run(add) == 1 && !writes && !reloads);
	reset(); answer = "WRITE mock:8\n"; CHECK(run(add) == 1 && confirms == 1 && !writes);
	reset(); answer = NULL; CHECK(run(add) == 1 && !writes);
	reset(); reload_errors[0] = EBUSY; CHECK(run(del) == 1 && !confirms && !writes);
	reset(); reload_errors[1] = EBUSY; CHECK(run(del) == 1 && confirms == 1 && !writes);
	reset(); reload_errors[0] = reload_errors[1] = EBUSY; CHECK(run(add) == 3 && writes == 1 && syncs == 1);
	reset(); reload_errors[1] = EIO; CHECK(run(add) == 3 && writes == 1);
	reset(); reload_errors[0] = EPERM; CHECK(run(add) == 1 && !reads && !writes);
	reset(); write_error = EIO; CHECK(run(add) == 1 && writes == 1 && reloads == 1);
	reset(); flush_error = EIO; CHECK(run(add) == 1 && writes == 1 && reloads == 1);
	reset(); enumerate = 1; CHECK(run(add) == 1 && !confirms && !writes); /* stale child extent */
	reset(); add[3] = "0"; CHECK(run(add) == 2 && !opens);
	reset(); add[3] = "2"; add[4] = "18446744073709551616"; CHECK(run(add) == 2 && !opens);
	printf("diskpart production CLI: %u checks PASS\n", checks);
	return 0;
}
