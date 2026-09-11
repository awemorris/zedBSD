/* -*- mode: c; c-basic-offset: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The formatter frontend's admission and descriptor-owned lease fixtures.
 */

#include "userland/base/common/format-file.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <zedbsd/fcntl.h>

#define WRITER_FD 31
#define READER_FD 47
#define FILE_BYTES UINT64_C(16384)
#define UNPUBLISHED_SIZE UINT64_C(0xdeadbeef)
#define READER_CLOSE_FAILURE 1U
#define WRITER_CLOSE_FAILURE 2U

enum event {
	EVENT_NONE,
	INITIAL_STAT,
	VALIDATE_SIZE,
	OPEN_WRITER,
	OPENED_STAT,
	RESERVE,
	RESERVED_STAT,
	WRITE_CONTENTS,
	FLUSH,
	OPEN_READER,
	REOPENED_STAT,
	VERIFY_CONTENTS,
	CLOSE_READER,
	FINAL_DESCRIPTOR_STAT,
	FINAL_PATH_STAT,
	CLOSE_WRITER
};

enum change {
	CHANGE_NONE,
	CHANGE_DEVICE,
	CHANGE_INODE,
	CHANGE_KIND,
	CHANGE_LINKS,
	CHANGE_SIZE,
	CHANGE_FIFO
};

static unsigned checks;
static struct stat original;
static enum event failure;
static int failure_error;
static enum event changed_event;
static enum change changed_field;
static unsigned close_failures;
static enum event trace[32];
static unsigned trace_count;
static unsigned pathname_stats;
static unsigned writer_stats;
static unsigned write_calls;
static unsigned verify_calls;
static unsigned claim_calls;
static int writer_open;
static int reader_open;
static int lease_held;
static int claim_name_checked;
static int flushed;

int ff_test_lstat(const char *path, struct stat *status);
int ff_test_fstat(int fd, struct stat *status);
int ff_test_open(const char *path, int flags, ...);
int ff_test_ioctl(int fd, unsigned long request, ...);
int ff_test_fsync(int fd);
int ff_test_close(int fd);
static int fake_validate(uint64_t bytes);
static int fake_write(int fd, uint64_t bytes);
static int fake_verify(int fd, uint64_t bytes);
static void require_check(int condition, const char *description);
static void reset_scenario(void);
static int record_event(enum event current);
static void return_identity(enum event current, struct stat *status);
static void run_case(int expected_error, int expect_write);
static void check_admission(void);
static void check_identity_changes(void);
static void check_operation_failures(void);
static void check_failure_precedence(void);
static void check_cli(void);

/* Route command grammar through the real frontend and independent fake format. */
#define main mkfs_cli_main
#define ufs_format_validate_size fake_validate
#define ufs_format_write fake_write
#define ufs_format_verify fake_verify
#define ufs_format_feature_write fake_write
#define ufs_format_feature_verify fake_verify
#define ufs_format_pristine fake_verify
#define ufs_format_feature_pristine fake_verify
int mkfs_block_command(int argc, char **argv)
{
	(void)argc;
	(void)argv;
	abort();
	return 2;
}
#include "userland/base/mkfs/main.c"
#undef ufs_format_feature_pristine
#undef ufs_format_pristine
#undef ufs_format_feature_verify
#undef ufs_format_feature_write
#undef ufs_format_verify
#undef ufs_format_write
#undef ufs_format_validate_size
#undef main

/* Apply the same admission fixture to the swap command's separate grammar. */
#define main mkswap_cli_main
#define swap_format_validate_size fake_validate
#define swap_format_write fake_write
#define swap_format_verify fake_verify
#define swap_format_pristine fake_verify
#include "userland/base/mkswap/main.c"
#undef swap_format_pristine
#undef swap_format_verify
#undef swap_format_write
#undef swap_format_validate_size
#undef main

/*
 * Checks the real frontend against observable lease and publication invariants.
 */
int
main(void)
{
	unsigned index;

	/* Verify the complete successful protocol and its exact event order. */
	reset_scenario();
	run_case(0, 1);
	require_check(trace_count == CLOSE_WRITER, "complete success event count");
	for (index = 0; index < trace_count; index++)
		require_check(trace[index] == (enum event)(index + 1U), "successful lifecycle ordering");

	/* Exercise rejection, namespace changes, and failures at every stage. */
	check_admission();
	check_identity_changes();
	check_operation_failures();
	check_failure_precedence();
	check_cli();

	/* Report the completed admission and ownership checks. */
	printf("Formatter frontend: %u checks PASS\n", checks);
	return 0;
}

/*
 * Supplies controlled initial, reserved, and final pathname identities.
 */
int
ff_test_lstat(
	const char *path,
	struct stat *status)
{
	enum event current;
	int failed;

	/* Select the pathname checkpoint while preserving no-follow semantics. */
	require_check(strcmp(path, "/staging/image") == 0, "use caller's exact pathname");
	pathname_stats++;
	current = pathname_stats == 1U ? INITIAL_STAT : pathname_stats == 2U ? RESERVED_STAT : FINAL_PATH_STAT;
	require_check(pathname_stats <= 3U, "bounded pathname identity checks");

	/* Require all later pathname checks to occur under the original lease. */
	if (current != INITIAL_STAT)
		require_check(lease_held && writer_open, "pathname revalidation retains lease");

	/* Return a scheduled lookup failure before exposing an identity. */
	failed = record_event(current);
	if (failed)
		return -1;

	/* Supply the requested identity checkpoint. */
	return_identity(current, status);
	if (current == RESERVED_STAT)
		claim_name_checked = 1;

	/* Report successful pathname inspection. */
	return 0;
}

/*
 * Supplies independently controlled opened and reopened descriptor identities.
 */
int
ff_test_fstat(
	int fd,
	struct stat *status)
{
	enum event current;
	int failed;

	/* Select the live descriptor's expected checkpoint. */
	if (fd == WRITER_FD) {
		require_check(writer_open, "stat only a live writer");
		writer_stats++;
		current = writer_stats == 1U ? OPENED_STAT : FINAL_DESCRIPTOR_STAT;
		require_check(writer_stats <= 2U, "bounded writer identity checks");
	} else {
		require_check(fd == READER_FD && reader_open, "stat only the reopened reader");
		require_check(lease_held && writer_open, "reader identity check retains writer lease");
		current = REOPENED_STAT;
	}

	/* Return a scheduled descriptor-inspection failure. */
	failed = record_event(current);
	if (failed)
		return -1;

	/* Supply the independently selected descriptor identity. */
	return_identity(current, status);
	return 0;
}

/*
 * Enforces non-creating opens and reader acquisition under the writer lease.
 */
int
ff_test_open(
	const char *path,
	int flags,
	...)
{
	int failed;

	/* Reject any request that could create, truncate, or follow a final link. */
	require_check(strcmp(path, "/staging/image") == 0, "open exact staging pathname");
	require_check((flags & (O_CREAT | O_TRUNC | O_EXCL)) == 0, "never create or truncate target");
	require_check((flags & O_NOFOLLOW) != 0, "never follow final symlink");
	require_check((flags & O_CLOEXEC) != 0, "prevent descriptor inheritance");
	require_check((flags & O_NONBLOCK) != 0, "avoid blocking on a replacement special file");

	/* Acquire the original writable description only once. */
	if ((flags & O_ACCMODE) == O_RDWR) {
		require_check(!writer_open && !reader_open, "single initial writer");
		failed = record_event(OPEN_WRITER);
		if (failed)
			return -1;

		/* Publish the live writer only after a successful open. */
		writer_open = 1;
		return WRITER_FD;
	}

	/* Require a readonly reopen before relinquishing the exclusion claim. */
	require_check((flags & O_ACCMODE) == O_RDONLY, "verification uses a readonly description");
	require_check(writer_open && lease_held && flushed, "reopen after flush while lease held");
	require_check(!reader_open, "single verification reader");
	failed = record_event(OPEN_READER);
	if (failed)
		return -1;

	/* Publish the live reader while preserving the original writer. */
	reader_open = 1;
	return READER_FD;
}

/*
 * Models successful or rejected descriptor-owned mutation reservation.
 */
int
ff_test_ioctl(
	int fd,
	unsigned long request,
	...)
{
	va_list arguments;
	const struct zedbsd_file_format_reserve *reserve;
	int failed;

	/* Require the expected lease request on the original writable description. */
	require_check(fd == WRITER_FD && writer_open, "reserve opened writer");
	require_check(request == ZEDBSD_FILE_FORMAT_RESERVE, "request format reservation only");
	require_check(!lease_held && write_calls == 0, "reserve before any generation");
	va_start(arguments, request);
	reserve = va_arg(arguments, const struct zedbsd_file_format_reserve *);
	va_end(arguments);
	require_check(reserve->version == ZEDBSD_FILE_FORMAT_VERSION, "reserve supported ABI version");
	require_check(reserve->struct_size == sizeof(*reserve), "reserve complete ABI structure");
	require_check(reserve->size_bytes == FILE_BYTES, "reserve caller-selected exact size");
	require_check(reserve->reserved[0] == 0 && reserve->reserved[1] == 0, "zero reserved ABI fields");

	/* Refuse a busy backing object without granting mutation authority. */
	claim_calls++;
	failed = record_event(RESERVE);
	if (failed)
		return -1;

	/* Grant exclusion until the final close of the writer description. */
	lease_held = 1;
	return 0;
}

/*
 * Requires durable flushing of the generated image before verification.
 */
int
ff_test_fsync(
	int fd)
{
	int failed;

	/* Keep the generated descriptor reserved throughout its flush. */
	require_check(fd == WRITER_FD && writer_open && lease_held, "flush reserved writer");
	require_check(write_calls == 1U, "flush after generation");
	failed = record_event(FLUSH);
	if (failed)
		return -1;

	/* Record successful durability before a reader may open. */
	flushed = 1;
	return 0;
}

/*
 * Checks reverse-order descriptor release even when close reports an error.
 */
int
ff_test_close(
	int fd)
{
	int failed;

	/* Release the verification reader before releasing writer exclusion. */
	if (fd == READER_FD) {
		require_check(reader_open && writer_open && lease_held, "close reader while writer still reserved");
		failed = record_event(CLOSE_READER);
		reader_open = 0;

		/* Exercise cleanup failures independently of the first operation error. */
		if ((close_failures & READER_CLOSE_FAILURE) != 0) {
			errno = EBADF;
			return -1;
		}

		/* Preserve the scheduled close result. */
		return failed ? -1 : 0;
	}

	/* Ensure writer release cannot strand a verification descriptor. */
	require_check(fd == WRITER_FD && writer_open, "close original writer exactly once");
	require_check(!reader_open, "reader closes before original writer");
	failed = record_event(CLOSE_WRITER);
	writer_open = 0;
	lease_held = 0;

	/* Exercise final-close failure without hiding an earlier error. */
	if ((close_failures & WRITER_CLOSE_FAILURE) != 0) {
		errno = EPIPE;
		return -1;
	}

	/* Preserve the scheduled final-close result. */
	return failed ? -1 : 0;
}

/* Stops the fixture on a violated public lifecycle invariant. */
static void
require_check(
	int condition,
	const char *description)
{
	/* Count checks and report the active checkpoint on failure. */
	checks++;
	if (!condition) {
		fprintf(stderr, "FAIL: %s (failure %d, identity %d/%d)\n", description, failure, changed_event, changed_field);
		exit(1);
	}
}

/* Restores a valid single-link staging object and empty descriptor state. */
static void
reset_scenario(void)
{
	/* Initialize a caller-sized regular object with a stable identity. */
	memset(&original, 0, sizeof(original));
	original.st_mode = S_IFREG | 0600;
	original.st_dev = 3;
	original.st_ino = 41;
	original.st_nlink = 1;
	original.st_size = (off_t)FILE_BYTES;

	/* Clear all faults, observations, and open-description ownership. */
	failure = EVENT_NONE;
	failure_error = EIO;
	changed_event = EVENT_NONE;
	changed_field = CHANGE_NONE;
	close_failures = 0;
	trace_count = 0;
	pathname_stats = 0;
	writer_stats = 0;
	write_calls = 0;
	verify_calls = 0;
	claim_calls = 0;
	writer_open = 0;
	reader_open = 0;
	lease_held = 0;
	claim_name_checked = 0;
	flushed = 0;
}

/* Records a public operation and returns its independently selected fault. */
static int
record_event(
	enum event current)
{
	/* Append the finite observable protocol trace. */
	require_check(trace_count < sizeof(trace) / sizeof(trace[0]), "bounded frontend operation trace");
	trace[trace_count++] = current;

	/* Fail exactly the selected operation while preserving its errno. */
	if (current == failure) {
		errno = failure_error;
		return 1;
	}

	/* Report an ordinary successful operation. */
	return 0;
}

/* Returns the selected independent identity mutation at one checkpoint. */
static void
return_identity(
	enum event current,
	struct stat *status)
{
	/* Start from the original stable object. */
	*status = original;
	if (current != changed_event)
		return;

	/* Change exactly one property without changing the other observations. */
	switch (changed_field) {
	case CHANGE_DEVICE:
		status->st_dev++;
		break;
	case CHANGE_INODE:
		status->st_ino++;
		break;
	case CHANGE_KIND:
		status->st_mode = S_IFLNK | 0777;
		break;
	case CHANGE_LINKS:
		status->st_nlink++;
		break;
	case CHANGE_SIZE:
		status->st_size += 4096;
		break;
	case CHANGE_FIFO:
		status->st_mode = S_IFIFO | 0600;
		break;
	default:
		require_check(0, "valid identity mutation selector");
	}
}

/* Models format-specific geometry rejection without descriptor access. */
static int
fake_validate(
	uint64_t bytes)
{
	int failed;

	/* Check geometry before the writer is opened or reserved. */
	require_check(bytes == FILE_BYTES, "validate initial caller-selected size");
	require_check(!writer_open && !lease_held, "validate before writable open");
	failed = record_event(VALIDATE_SIZE);
	if (failed)
		return failure_error;

	/* Report accepted test geometry. */
	return 0;
}

/* Models the only destructive operation under verified exclusive ownership. */
static int
fake_write(
	int fd,
	uint64_t bytes)
{
	int failed;

	/* Require successful claim and name revalidation before any generation. */
	require_check(fd == WRITER_FD && writer_open && lease_held, "generate only under writer lease");
	require_check(claim_name_checked && claim_calls == 1U, "generate after successful claim revalidation");
	require_check(bytes == FILE_BYTES, "generate exact original size");
	write_calls++;
	failed = record_event(WRITE_CONTENTS);
	if (failed)
		return failure_error;

	/* Report successful format generation. */
	return 0;
}

/* Models decoder validation while the original writer still owns exclusion. */
static int
fake_verify(
	int fd,
	uint64_t bytes)
{
	int failed;

	/* Require a readonly reopened descriptor and a successfully flushed writer. */
	require_check(fd == READER_FD && reader_open, "verify reopened reader");
	require_check(writer_open && lease_held && flushed, "verify before lease release");
	require_check(bytes == FILE_BYTES, "verify original selected size");
	verify_calls++;
	failed = record_event(VERIFY_CONTENTS);
	if (failed)
		return failure_error;

	/* Report recognized generated metadata. */
	return 0;
}

/* Checks result publication and cleanup for one independently selected case. */
static void
run_case(
	int expected_error,
	int expect_write)
{
	static const struct format_file_ops operations = {
		fake_validate, fake_write, fake_verify
	};
	uint64_t size;
	int error;

	/* Call the real frontend with a visibly unpublished result slot. */
	size = UNPUBLISHED_SIZE;
	error = format_file_run("/staging/image", &operations, &size);
	require_check(error == expected_error, "preserve expected first operation error");
	require_check(write_calls == (unsigned)expect_write, "respect pre-mutation admission boundary");
	require_check(!reader_open && !writer_open && !lease_held, "release all descriptions and claims");

	/* Publish success only after the complete protocol and all closes succeed. */
	if (expected_error == 0) {
		require_check(size == FILE_BYTES, "publish size after completed success");
		require_check(verify_calls == 1U, "success includes production validation");
	} else {
		require_check(size == UNPUBLISHED_SIZE, "never publish size on failure");
	}
}

/* Checks wrong object kinds, aliases, zero sizes, and refused claims. */
static void
check_admission(void)
{
	static const mode_t kinds[] = {
		S_IFDIR, S_IFLNK, S_IFBLK, S_IFCHR, S_IFIFO, S_IFSOCK
	};
	size_t index;

	/* Refuse object kinds before opening potentially blocking or raw objects. */
	for (index = 0; index < sizeof(kinds) / sizeof(kinds[0]); index++) {
		reset_scenario();
		original.st_mode = kinds[index] | 0600;
		run_case(EINVAL, 0);
		require_check(trace_count == 1U, "wrong kinds only receive initial lstat");
	}

	/* Refuse an empty or negative-sized object before writable open. */
	reset_scenario();
	original.st_size = 0;
	run_case(EINVAL, 0);
	reset_scenario();
	original.st_size = -1;
	run_case(EINVAL, 0);

	/* Refuse both unlinked and multiply linked staging objects. */
	reset_scenario();
	original.st_nlink = 0;
	run_case(EBUSY, 0);
	reset_scenario();
	original.st_nlink = 2;
	run_case(EBUSY, 0);

	/* Preserve a format-specific size rejection before opening the target. */
	reset_scenario();
	failure = VALIDATE_SIZE;
	failure_error = EFBIG;
	run_case(EFBIG, 0);
	require_check(trace_count == 2U, "format size rejection precedes open");

	/* Propagate kernel refusal of mounted, active, or aliased backing objects. */
	reset_scenario();
	failure = RESERVE;
	failure_error = EBUSY;
	run_case(EBUSY, 0);
	require_check(claim_calls == 1U, "attempt one conservative claim");
}

/* Checks each identity property at all independent revalidation checkpoints. */
static void
check_identity_changes(void)
{
	static const enum event checkpoints[] = {
		OPENED_STAT, RESERVED_STAT, REOPENED_STAT,
		FINAL_DESCRIPTOR_STAT, FINAL_PATH_STAT
	};
	size_t index;
	int property;

	/* Change one property at a time across descriptor and pathname checks. */
	for (index = 0; index < sizeof(checkpoints) / sizeof(checkpoints[0]); index++) {
		/* Test backing device, inode, kind, aliases, and caller-selected length. */
		for (property = CHANGE_DEVICE; property <= CHANGE_SIZE; property++) {
			reset_scenario();
			changed_event = checkpoints[index];
			changed_field = (enum change)property;
			run_case(EBUSY, checkpoints[index] > WRITE_CONTENTS);
		}
	}

	/* Refuse a FIFO substituted between the initial pathname check and open. */
	reset_scenario();
	changed_event = OPENED_STAT;
	changed_field = CHANGE_FIFO;
	run_case(EBUSY, 0);
	require_check(claim_calls == 0, "reject replaced FIFO before claiming it");

	/* Refuse a FIFO substituted before the readonly verification reopen. */
	reset_scenario();
	changed_event = REOPENED_STAT;
	changed_field = CHANGE_FIFO;
	run_case(EBUSY, 1);
	require_check(verify_calls == 0, "reject replaced FIFO before decoder access");
}

/* Checks every syscall, generator, and decoder failure including both closes. */
static void
check_operation_failures(void)
{
	int current;

	/* Fail each observable operation independently without skipping cleanup. */
	for (current = INITIAL_STAT; current <= CLOSE_WRITER; current++) {
		reset_scenario();
		failure = (enum event)current;
		failure_error = EIO;
		run_case(EIO, current >= WRITE_CONTENTS);
	}
}

/* Checks cleanup errors cannot overwrite an earlier operation's errno. */
static void
check_failure_precedence(void)
{
	/* Preserve a descriptor-inspection failure over writer-close failure. */
	reset_scenario();
	failure = OPENED_STAT;
	failure_error = ENOENT;
	close_failures = WRITER_CLOSE_FAILURE;
	run_case(ENOENT, 0);

	/* Preserve generation failure over final writer-close failure. */
	reset_scenario();
	failure = WRITE_CONTENTS;
	failure_error = ENOSPC;
	close_failures = WRITER_CLOSE_FAILURE;
	run_case(ENOSPC, 1);

	/* Preserve decoder failure while still attempting both descriptor closes. */
	reset_scenario();
	failure = VERIFY_CONTENTS;
	failure_error = EINVAL;
	close_failures = READER_CLOSE_FAILURE | WRITER_CLOSE_FAILURE;
	run_case(EINVAL, 1);

	/* Preserve the reader-close error when the final writer close also fails. */
	reset_scenario();
	close_failures = READER_CLOSE_FAILURE | WRITER_CLOSE_FAILURE;
	run_case(EBADF, 1);

	/* Surface an otherwise successful operation's final close failure. */
	reset_scenario();
	close_failures = WRITER_CLOSE_FAILURE;
	run_case(EPIPE, 1);
}

/* Checks unsupported command grammar cannot reach any file operation. */
static void
check_cli(void)
{
	static char *mkfs_cases[][6] = {
		{ "mkfs", NULL, NULL, NULL, NULL, NULL },
		{ "mkfs", "-t", "ufs", NULL, NULL, NULL },
		{ "mkfs", "-t", "ufs2", "/staging/image", NULL, NULL },
		{ "mkfs", "-t", "ufs1", "/staging/image", NULL, NULL },
		{ "mkfs", "-f", "ufs", "/staging/image", NULL, NULL },
		{ "mkfs", "-t", "ufs", "/staging/image", "extra", NULL }
	};
	static const int mkfs_counts[] = { 1, 3, 4, 4, 4, 5 };
	static char *swap_cases[][4] = {
		{ "mkswap", NULL, NULL, NULL },
		{ "mkswap", "--format=v1", NULL, NULL },
		{ "mkswap", "/staging/image", "extra", NULL }
	};
	static const int swap_counts[] = { 1, 2, 3 };
	char *valid_mkfs[] = { "mkfs", "-t", "ufs", "/staging/image", NULL };
	char *feature_mkfs[] = { "mkfs", "-t", "ufs", "--profile=journal-snapshot", "/staging/image", NULL };
	char *valid_swap[] = { "mkswap", "/staging/image", NULL };
	size_t index;
	int result;

	/* Reject omitted types, unknown types, wrong switches, and extra operands. */
	for (index = 0; index < sizeof(mkfs_counts) / sizeof(mkfs_counts[0]); index++) {
		reset_scenario();
		result = mkfs_cli_main(mkfs_counts[index], mkfs_cases[index]);
		require_check(result == 2, "mkfs rejects unsupported grammar");
		require_check(trace_count == 0, "mkfs grammar failure precedes all file operations");
	}

	/* Reject swap option extensions and missing or repeated operands. */
	for (index = 0; index < sizeof(swap_counts) / sizeof(swap_counts[0]); index++) {
		reset_scenario();
		result = mkswap_cli_main(swap_counts[index], swap_cases[index]);
		require_check(result == 2, "mkswap rejects unsupported grammar");
		require_check(trace_count == 0, "mkswap grammar failure precedes all file operations");
	}

	/* Require a successful complete lifecycle before the commands report zero. */
	reset_scenario();
	result = mkfs_cli_main(4, valid_mkfs);
	require_check(result == 0 && verify_calls == 1U, "mkfs success requires completed verification");
	require_check(!writer_open && !reader_open && !lease_held, "mkfs success releases ownership");
	reset_scenario();
	result = mkfs_cli_main(5, feature_mkfs);
	require_check(result == 0 && verify_calls == 1U, "feature profile completes same reservation lifecycle");
	require_check(!writer_open && !reader_open && !lease_held, "feature profile releases ownership");
	reset_scenario();
	result = mkswap_cli_main(2, valid_swap);
	require_check(result == 0 && verify_calls == 1U, "mkswap success requires completed verification");
	require_check(!writer_open && !reader_open && !lease_held, "mkswap success releases ownership");

	/* Preserve operation failure status without entering format generation. */
	reset_scenario();
	failure = RESERVE;
	failure_error = EBUSY;
	result = mkfs_cli_main(4, valid_mkfs);
	require_check(result == 1 && write_calls == 0, "mkfs refuses busy object before mutation");
	reset_scenario();
	failure = RESERVE;
	failure_error = EBUSY;
	result = mkswap_cli_main(2, valid_swap);
	require_check(result == 1 && write_calls == 0, "mkswap refuses busy object before mutation");
}
