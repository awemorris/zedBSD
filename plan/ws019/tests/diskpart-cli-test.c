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
static int fake_fflush(FILE *);
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
#define fflush fake_fflush
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
#undef fflush

static unsigned checks, reads, writes, syncs, reloads, opens, closes, confirms;
static unsigned flags, enumerate, directory_at;
static int regular, write_error, flush_error, reload_errors[4];
static const char *answer;
static uint8_t media[131072];
static unsigned sectors, reserves, reserved, init_mode, output_calls;
static int reserve_error, close_error, short_write, output_fail_at, changed_during_confirm;
#define CHECK(x) do { checks++; if (!(x)) { \
	fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); abort(); \
} } while (0)
static int fail(int error) { errno = error; return -1; }
static int fake_open(const char *path, int mode, ...)
{ (void)mode; CHECK(!strcmp(path, "/dev/mock") || !strcmp(path, "/dev/child")); opens++; return !strcmp(path, "/dev/mock") ? 42 : 43; }
static int fake_close(int fd)
{ CHECK(fd == 42 || fd == 43); closes++; if (fd == 42) reserved = 0; return close_error ? fail(close_error) : 0; }
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
		i->sector_size = 512; i->sector_count = fd == 42 ? sectors : 3;
		i->parent_device = fd == 42 ? 0 : 7; i->parent_offset = 4;
		i->flags = fd == 42 ? flags : ZEDBSD_BLOCK_PARTITION;
		return 0;
	}
	if (op == BLKRESERVE) {
		va_list ap; struct zedbsd_block_info *info;
		va_start(ap, op); info = va_arg(ap, struct zedbsd_block_info *); va_end(ap);
		CHECK(fd == 42 && info->device == 7 && !reserved);
		reserves++;
		if (reserve_error) return fail(reserve_error);
		reserved = 1;
		return 0;
	}
	CHECK(fd == 42 && op == BLKREREADPART && reloads < 4);
	if (init_mode) CHECK(reserved);
	int e = reload_errors[reloads++]; return e ? fail(e) : 0;
}
static ssize_t fake_pread(int fd, void *p, size_t n, off_t at)
{ CHECK(fd == 42 && at >= 0 && (uint64_t)at + n <= sizeof(media)); if (init_mode) CHECK(reserved); reads++; memcpy(p, media + at, n); return (ssize_t)n; }
static ssize_t fake_pwrite(int fd, const void *p, size_t n, off_t at)
{ CHECK(fd == 42 && at >= 0 && (uint64_t)at + n <= sizeof(media)); if (init_mode) CHECK(reserved); writes++; if (write_error) return fail(write_error); if (short_write) n /= 2; memcpy(media + at, p, n); return (ssize_t)n; }
static int fake_fsync(int fd)
{ CHECK(fd == 42); syncs++; return flush_error ? fail(flush_error) : 0; }
static DIR *fake_opendir(const char *path)
{ CHECK(!strcmp(path, "/dev")); directory_at = 0; return (DIR *)(uintptr_t)1; }
static struct dirent *fake_readdir(DIR *dir)
{ static struct dirent e; (void)dir; if (!enumerate || directory_at++) return NULL; strcpy(e.d_name, "child"); return &e; }
static int fake_closedir(DIR *dir) { (void)dir; return 0; }
static char *fake_fgets(char *p, int n, FILE *f)
{ (void)f; confirms++; if (init_mode) CHECK(reserved); if (changed_during_confirm) media[0] ^= 1; if (!answer) return NULL; CHECK(strlen(answer) < (unsigned)n); strcpy(p, answer); return p; }
static int fake_fflush(FILE *f)
{ output_calls++; if (output_fail_at && output_calls == (unsigned)output_fail_at) return fail(ENOSPC); return fflush(f); }
static void reset(void)
{
	reads = writes = syncs = reloads = opens = closes = confirms = 0;
	flags = enumerate = regular = write_error = flush_error = 0;
	sectors = 16; reserves = reserved = init_mode = output_calls = 0;
	reserve_error = close_error = short_write = output_fail_at = changed_during_confirm = 0;
	memset(reload_errors, 0, sizeof(reload_errors));
	answer = "WRITE mock:7\n";
	memset(media, 0, sizeof(media)); media[510] = 0x55; media[511] = 0xaa;
	media[450] = 0x83; media[454] = 4; media[458] = 2;
}
static int run(char **argv)
{ int n = 0, result; while (argv[n]) n++; result = diskpart_main(n, argv); CHECK(opens == closes && !reserved); return result; }
static void reset_init(void)
{
	reset(); init_mode = 1; sectors = sizeof(media) / 512;
	memset(media, 0xa5, sizeof(media)); answer = "ERASE mock:7\n";
}
static void test_init(void)
{
	char *args[] = {"diskpart", "init", "mock", "12345678-1234-4567-890a-bcdef0123456",
	    "34", "20", "c12a7328-f81f-11d2-ba4b-00a0c93ec93b", "aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee", "ESP",
	    "54", "100", "516e7cb6-6ecf-11d6-8ff8-00022d09712b", "cccccccc-bbbb-4ccc-8ddd-eeeeeeeeeeee", "root", NULL};
	char *empty[] = {"diskpart", "init", "mock", args[3], NULL};
	char *incomplete[] = {"diskpart", "init", "mock", args[3], "34", NULL};
	char *machine[] = {"diskpart", "--machine", "init", "mock", args[3], NULL};
	reset_init(); CHECK(run(args) == 0 && reserves == 1 && confirms == 1 && writes == 5 && syncs == 3 && reloads == 1);
	CHECK(media[450] == 0xee && !memcmp(media + 512, "EFI PART", 8));
	reset_init(); memset(media, 0, sizeof(media)); CHECK(run(empty) == 0 && writes == 5);
	reset_init(); CHECK(run(incomplete) == 2 && !opens && !writes);
	reset_init(); CHECK(run(machine) == 2 && !opens);
	reset_init(); reserve_error = EBUSY; CHECK(run(args) == 1 && !reads && !confirms && !writes);
	reset_init(); reserve_error = ESTALE; CHECK(run(args) == 1 && !confirms && !writes);
	reset_init(); flags = ZEDBSD_BLOCK_READ_ONLY; CHECK(run(args) == 1 && !reserves && !writes);
	reset_init(); answer = "ERASE mock:8\n"; CHECK(run(args) == 1 && confirms == 1 && !writes && !reloads);
	reset_init(); answer = NULL; CHECK(run(args) == 1 && !writes);
	reset_init(); answer = "ERASE mock:7"; CHECK(run(args) == 1 && !writes);
	reset_init(); answer = "ERASE mock:7\rjunk\n"; CHECK(run(args) == 1 && !writes);
	reset_init(); answer = "ERASE mock:7\r\n"; CHECK(run(args) == 0);
	reset_init(); output_fail_at = 1; CHECK(run(args) == 1 && !confirms && !writes);
	reset_init(); output_fail_at = 2; CHECK(run(args) == 1 && writes == 5 && reloads == 1);
	reset_init(); close_error = EIO; CHECK(run(args) == 1 && writes == 5 && reloads == 1);
	reset_init(); changed_during_confirm = 1; CHECK(run(args) == 1 && !writes && !reloads);
	reset_init(); write_error = EIO; CHECK(run(args) == 1 && writes == 1 && !reloads);
	reset_init(); short_write = 1; CHECK(run(args) == 1 && writes == 1 && !reloads);
	reset_init(); flush_error = EIO; CHECK(run(args) == 1 && writes == 2 && !reloads);
	reset_init(); reload_errors[0] = EIO; CHECK(run(args) == 3 && writes == 5 && syncs == 3);
	reset_init(); args[4] = "33"; CHECK(run(args) == 1 && !confirms && !writes); args[4] = "34";
	reset_init(); args[9] = "53"; CHECK(run(args) == 1 && !confirms && !writes); args[9] = "54";
	reset_init(); args[12] = args[7]; CHECK(run(args) == 1 && !confirms && !writes);
	args[12] = "cccccccc-bbbb-4ccc-8ddd-eeeeeeeeeeee";
	reset_init(); args[13] = "control\n"; CHECK(run(args) == 1 && !confirms && !writes); args[13] = "root";
	reset_init(); args[4] = "18446744073709551616"; CHECK(run(args) == 2 && !opens);
	args[4] = "34";
	reset_init(); empty[3] = "invalid"; CHECK(run(empty) == 2 && !opens);
}
int main(void)
{
	char *add[] = {"diskpart", "add", "mock", "2", "8", "2", "83", NULL};
	char *del[] = {"diskpart", "delete", "mock", "1", NULL};
	char *show_args[] = {"diskpart", "show", "mock", NULL};
	char *reload[] = {"diskpart", "reload", "mock", NULL};
	char *machine_show[] = {"diskpart", "--machine", "show", "mock", NULL};
	char *machine_list[] = {"diskpart", "--machine", "list", NULL};
	char *machine_edit[] = {"diskpart", "--machine", "delete", "mock", "1", NULL};
	FILE *capture;
	int output_fd;
	char record[1024];
	size_t length;

	/* Check exact machine records and prove no confirmation or mutation occurs. */
	reset(); capture = tmpfile(); CHECK(capture != NULL);
	fflush(stdout); output_fd = dup(STDOUT_FILENO); CHECK(output_fd >= 0);
	CHECK(dup2(fileno(capture), STDOUT_FILENO) >= 0);
	CHECK(run(machine_show) == 0 && reads && !writes && !reloads && !confirms);
	fflush(stdout); CHECK(dup2(output_fd, STDOUT_FILENO) >= 0); close(output_fd);
	rewind(capture); length = fread(record, 1, sizeof(record) - 1, capture);
	record[length] = 0; fclose(capture);
	CHECK(!strcmp(record, "device\tmock\t7\t0\t0\t512\t16\t4\n"
	    "table\tmbr\t0\t1\nrange\t0\t512\npartition\t1\t4\t2\t83\t00000000-01\t0\n"));
	reset(); enumerate = 1; capture = tmpfile(); CHECK(capture != NULL);
	fflush(stdout); output_fd = dup(STDOUT_FILENO); CHECK(output_fd >= 0);
	CHECK(dup2(fileno(capture), STDOUT_FILENO) >= 0);
	CHECK(run(machine_list) == 0 && !writes && !reloads);
	fflush(stdout); CHECK(dup2(output_fd, STDOUT_FILENO) >= 0); close(output_fd);
	rewind(capture); length = fread(record, 1, sizeof(record) - 1, capture);
	record[length] = 0; fclose(capture);
	CHECK(!strcmp(record, "device\tchild\t8\t7\t4\t512\t3\t4\n"));
	reset(); CHECK(run(machine_edit) == 2 && !opens && !writes && !reloads);
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
	test_init();
	printf("diskpart production CLI: %u checks PASS\n", checks);
	return 0;
}
