/* -*- mode: c; c-basic-offset: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The target swap formatter's geometry and incomplete-I/O fixtures.
 */

#include "userland/base/mkswap/swap-format.h"

#include <errno.h>
#include <fcntl.h>
#include <kern/swap.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define DEFAULT_BYTES UINT64_C(67108864)
#define MAX_BYTES UINT64_C(2147479552)
#define FAULT_ERROR 1
#define FAULT_SHORT 2
#define FAULT_INTERRUPT 3

static unsigned checks;
static long writes;
static long reads;
static long write_fault = -1;
static long read_fault = -1;
static int fault_kind;
static int virtual_writes;
static uint64_t virtual_end;
static uint8_t virtual_header[64];

ssize_t swap_test_pwrite(int fd, const void *buffer, size_t length, off_t offset);
ssize_t swap_test_pread(int fd, void *buffer, size_t length, off_t offset);
static void require_check(int condition, const char *description);
static void reset_io(void);
static void build_image(int fd, uint64_t bytes);
static void check_boundaries(int fd);
static void check_faults(int fd);
static void check_parser(int fd, const char *legacy_path, const char *labeled_path);
static void check_prefilled(int fd, const char *reference_path);
static void read_header_file(const char *path, uint8_t *header, uint64_t *bytes);
static void put32(uint8_t *buffer, size_t offset, uint32_t value);

/*
 * Exercises the formatter and leaves its default image for builder comparison.
 */
int
main(
	int argc,
	char **argv)
{
	int output;
	int scratch;
	int error;

	/* Require disposable outputs and independent maintained parser fixtures. */
	require_check(argc == 6, "output, scratch, legacy, labeled, and reference arguments");
	output = open(argv[1], O_CREAT | O_EXCL | O_RDWR, 0600);
	require_check(output >= 0, "create output fixture");
	scratch = open(argv[2], O_CREAT | O_EXCL | O_RDWR, 0600);
	require_check(scratch >= 0, "create scratch fixture");

	/* Check size admission, transfer errors, and shared parser behavior. */
	check_boundaries(scratch);
	check_faults(scratch);
	check_parser(scratch, argv[3], argv[4]);
	check_prefilled(scratch, argv[5]);

	/* Retain fresh zero backing for whole-file equality with the host builder. */
	error = ftruncate(output, (off_t)DEFAULT_BYTES);
	require_check(error == 0, "size default fixture");
	reset_io();
	build_image(output, DEFAULT_BYTES);

	/* Close both descriptors and remove only the scratch image. */
	error = close(output);
	require_check(error == 0, "close default fixture");
	error = close(scratch);
	require_check(error == 0, "close scratch fixture");
	error = unlink(argv[2]);
	require_check(error == 0, "remove scratch fixture");

	/* Report all completed behavior checks. */
	printf("Swap formatter: %u checks PASS\n", checks);
	return 0;
}

/*
 * Injects transfer failures or accounts for a maximum-sized virtual output.
 */
ssize_t
swap_test_pwrite(
	int fd,
	const void *buffer,
	size_t length,
	off_t offset)
{
	long call;
	ssize_t count;

	/* Apply one scheduled incomplete transfer. */
	call = writes++;
	if (call == write_fault) {
		/* Preserve a positive partial result for the caller to reject. */
		if (fault_kind == FAULT_SHORT)
			return (ssize_t)(length - 1U);

		/* Distinguish retryable interruption from exhausted storage. */
		errno = fault_kind == FAULT_INTERRUPT ? EINTR : ENOSPC;
		return -1;
	}

	/* Account for the maximum geometry without allocating a two-GiB image. */
	if (virtual_writes) {
		/* Capture the final header after clearing only the reserved page. */
		if (offset == 0 && length == sizeof(virtual_header)) {
			require_check(virtual_end == SWAP_PAGE_SIZE, "header follows reserved-page initialization");
			memcpy(virtual_header, buffer, sizeof(virtual_header));
		} else {
			require_check(offset == 0 && virtual_end == 0, "clear only the header page");
			require_check(length == SWAP_PAGE_SIZE, "initialize complete reserved header page");
			virtual_end += length;
			require_check(virtual_end <= SWAP_PAGE_SIZE, "never initialize unallocated slots");
		}

		/* Report the accounted complete transfer. */
		return (ssize_t)length;
	}

	/* Forward the default path to an actual disposable file. */
	count = pwrite(fd, buffer, length, offset);
	return count;
}

/*
 * Injects one controlled header-read failure around the production verifier.
 */
ssize_t
swap_test_pread(
	int fd,
	void *buffer,
	size_t length,
	off_t offset)
{
	long call;
	ssize_t count;

	/* Apply one scheduled read interruption or truncation. */
	call = reads++;
	if (call == read_fault) {
		/* Return an incomplete header without silently supplying its tail. */
		if (fault_kind == FAULT_SHORT)
			return (ssize_t)(length - 1U);

		/* Preserve permanent media errors and retryable interruptions. */
		errno = fault_kind == FAULT_INTERRUPT ? EINTR : EIO;
		return -1;
	}

	/* Forward ordinary reads to the disposable file. */
	count = pread(fd, buffer, length, offset);
	return count;
}

/* Stops the fixture with a useful behavioral assertion failure. */
static void
require_check(
	int condition,
	const char *description)
{
	/* Count every check and stop at the first failure. */
	checks++;
	if (!condition) {
		fprintf(stderr, "FAIL: %s (errno %d)\n", description, errno);
		exit(1);
	}
}

/* Restores ordinary descriptor operations. */
static void
reset_io(void)
{
	/* Clear injected failures and virtual geometry accounting. */
	writes = 0;
	reads = 0;
	write_fault = -1;
	read_fault = -1;
	fault_kind = 0;
	virtual_writes = 0;
	virtual_end = 0;
	memset(virtual_header, 0, sizeof(virtual_header));
}

/* Generates and verifies a caller-sized disposable swap file. */
static void
build_image(
	int fd,
	uint64_t bytes)
{
	int error;
	struct stat status;

	/* Establish the exact file length before invoking the no-resize API. */
	error = ftruncate(fd, (off_t)bytes);
	require_check(error == 0, "size swap fixture");
	error = swap_format_write(fd, bytes);
	require_check(error == 0, "generate swap fixture");
	error = fsync(fd);
	require_check(error == 0, "flush swap fixture");

	/* Verify the generated header through the production parser. */
	error = swap_format_verify(fd, bytes);
	require_check(error == 0, "verify swap fixture");
	error = fstat(fd, &status);
	require_check(error == 0, "stat swap fixture");
	require_check((uint64_t)status.st_size == bytes, "preserve caller size");
}

/* Checks supported boundaries and rejects malformed sizes before I/O. */
static void
check_boundaries(
	int fd)
{
	static const uint64_t sizes[] = {
		0U, 4096U, 8191U, 8193U, MAX_BYTES + 4096U, UINT64_MAX
	};
	struct swap_header_info parsed;
	size_t index;
	int error;

	/* Reject invalid sizes without inspecting or mutating the descriptor. */
	reset_io();
	for (index = 0; index < sizeof(sizes) / sizeof(sizes[0]); index++) {
		error = swap_format_validate_size(sizes[index]);
		require_check(error != 0, "reject invalid swap geometry");
		error = swap_format_write(fd, sizes[index]);
		require_check(error != 0, "reject invalid write geometry");
		error = swap_format_verify(fd, sizes[index]);
		require_check(error != 0, "reject invalid verify geometry");
	}

	/* Confirm all geometry failures precede descriptor access. */
	require_check(writes == 0 && reads == 0, "no I/O for rejected geometry");
	error = swap_format_validate_size(MAX_BYTES + 4096U);
	require_check(error == EFBIG, "report activation-size overflow");

	/* Exercise one-slot and two-slot geometries with actual files. */
	build_image(fd, 8192U);
	build_image(fd, 12288U);

	/* Exercise maximum-size geometry without initializing its unused slots. */
	reset_io();
	virtual_writes = 1;
	error = swap_format_write(fd, MAX_BYTES);
	require_check(error == 0, "generate maximum supported geometry");
	require_check(virtual_end == SWAP_PAGE_SIZE, "bound writes independently of slot count");
	require_check(writes == 2, "publish maximum header after one bounded clear");
	error = swap_header_parse(virtual_header, MAX_BYTES, &parsed);
	require_check(error == 0, "production parser accepts maximum output");
	require_check(parsed.slot_count == MAX_BYTES / 4096U - 1U, "maximum output has exact slot count");
	reset_io();
}

/* Checks strict short transfers, error propagation, and interruption retries. */
static void
check_faults(
	int fd)
{
	int kind;
	long position;
	int error;

	/* Visit reserved-page clearing and final header publication. */
	for (kind = FAULT_ERROR; kind <= FAULT_INTERRUPT; kind++) {
		/* Inject the selected result at every transfer of a three-page file. */
		for (position = 0; position < 2; position++) {
			reset_io();
			write_fault = position;
			fault_kind = kind;
			error = swap_format_write(fd, 12288U);

			/* Accept only a fully retried interruption. */
			if (kind == FAULT_INTERRUPT) {
				require_check(error == 0, "retry interrupted swap write");
				require_check(writes == 3, "perform exactly one write retry");
			} else {
				require_check(error == (kind == FAULT_ERROR ? ENOSPC : EIO), "reject failed or partial swap write");
				require_check(writes == position + 1, "stop after incomplete swap write");
			}
		}
	}

	/* Begin read faults with a complete actual header. */
	reset_io();
	build_image(fd, 12288U);

	/* Inject each read failure convention in the production verification path. */
	for (kind = FAULT_ERROR; kind <= FAULT_INTERRUPT; kind++) {
		reset_io();
		read_fault = 0;
		fault_kind = kind;
		error = swap_format_verify(fd, 12288U);

		/* Retry an interruption while rejecting a partial or failed header. */
		if (kind == FAULT_INTERRUPT) {
			require_check(error == 0 && reads == 2, "retry interrupted header read once");
		} else {
			require_check(error == EIO && reads == 1, "reject incomplete header read");
		}
	}
	reset_io();
}

/* Checks reserved-page replacement while preserving every unused slot byte. */
static void
check_prefilled(
	int fd,
	const char *reference_path)
{
	uint8_t actual[SWAP_PAGE_SIZE];
	uint8_t expected[SWAP_PAGE_SIZE];
	uint8_t stale[SWAP_PAGE_SIZE];
	uint64_t offset;
	ssize_t count;
	int reference;
	int error;

	/* Fill the old header and all slot pages with recognizable stale data. */
	error = ftruncate(fd, (off_t)DEFAULT_BYTES);
	require_check(error == 0, "size prefilled swap fixture");
	memset(stale, 0x73, sizeof(stale));
	for (offset = 0; offset < DEFAULT_BYTES; offset += sizeof(stale)) {
		count = pwrite(fd, stale, sizeof(stale), (off_t)offset);
		require_check(count == (ssize_t)sizeof(stale), "prefill old swap contents");
	}

	/* Generate the default geometry without touching inactive slot pages. */
	reset_io();
	build_image(fd, DEFAULT_BYTES);
	require_check(writes == 2, "bound formatting independently of swap size");

	/* Compare the entire reserved page with the maintained Noct builder. */
	reference = open(reference_path, O_RDONLY);
	require_check(reference >= 0, "open independent swap reference");
	count = pread(reference, expected, sizeof(expected), 0);
	require_check(count == (ssize_t)sizeof(expected), "read maintained swap header page");
	count = pread(fd, actual, sizeof(actual), 0);
	require_check(count == (ssize_t)sizeof(actual), "read initialized swap header page");
	require_check(memcmp(actual, expected, sizeof(actual)) == 0, "replace every old reserved-page byte");
	error = close(reference);
	require_check(error == 0, "close independent swap reference");

	/* Confirm that no unused slot is initialized or interpreted as metadata. */
	for (offset = SWAP_PAGE_SIZE; offset < DEFAULT_BYTES; offset += sizeof(actual)) {
		count = pread(fd, actual, sizeof(actual), (off_t)offset);
		require_check(count == (ssize_t)sizeof(actual), "read unused slot page");
		require_check(memcmp(actual, stale, sizeof(actual)) == 0, "preserve unspecified unused swap slot");
	}

	/* Reject corruption in reserved padding beyond the parsed header fields. */
	count = pwrite(fd, stale, 1U, SWAP_PAGE_SIZE - 1U);
	require_check(count == 1, "corrupt reserved swap header-page padding");
	error = swap_format_verify(fd, DEFAULT_BYTES);
	require_check(error == EIO, "reject changed reserved header-page padding");
}

/* Reads independently generated header bytes and their actual backing size. */
static void
read_header_file(
	const char *path,
	uint8_t *header,
	uint64_t *bytes)
{
	struct stat status;
	int fd;
	int error;
	ssize_t count;

	/* Read exactly the header without allocating a backing-sized buffer. */
	fd = open(path, O_RDONLY);
	require_check(fd >= 0, "open maintained parser fixture");
	count = pread(fd, header, 64U, 0);
	require_check(count == 64, "read maintained parser fixture");
	error = fstat(fd, &status);
	require_check(error == 0, "stat maintained parser fixture");
	*bytes = (uint64_t)status.st_size;
	error = close(fd);
	require_check(error == 0, "close maintained parser fixture");
}

/* Encodes a replacement checksum without depending on native alignment. */
static void
put32(
	uint8_t *buffer,
	size_t offset,
	uint32_t value)
{
	/* Store the low-to-high bytes of the little-endian field. */
	buffer[offset] = (uint8_t)value;
	buffer[offset + 1U] = (uint8_t)(value >> 8);
	buffer[offset + 2U] = (uint8_t)(value >> 16);
	buffer[offset + 3U] = (uint8_t)(value >> 24);
}

/* Checks extracted production-parser compatibility and verification failures. */
static void
check_parser(
	int fd,
	const char *legacy_path,
	const char *labeled_path)
{
	struct swap_header_info parsed;
	uint8_t header[64];
	uint64_t bytes;
	uint32_t checksum;
	ssize_t count;
	int error;
	unsigned index;
	char uuid[17];

	/* Preserve recognition of maintained version-one images after extraction. */
	read_header_file(legacy_path, header, &bytes);
	error = swap_header_parse(header, bytes, &parsed);
	require_check(error == 0, "accept maintained version-one header");
	require_check(parsed.version == 1U && parsed.slot_count == 8191U, "preserve version-one geometry");
	error = swap_header_parse(header, bytes + 4096U, NULL);
	require_check(error == EINVAL, "reject legacy backing-size mismatch");

	/* Preserve labeled version-two images and the existing UUID presentation. */
	read_header_file(labeled_path, header, &bytes);
	error = swap_header_parse(header, bytes, &parsed);
	require_check(error == 0, "accept maintained labeled version-two header");
	require_check(parsed.version == 2U && parsed.slot_count == 255U, "preserve labeled version-two geometry");
	require_check(strcmp(parsed.label, "TESTSWAP") == 0, "preserve version-two label");
	error = swap_header_uuid_format(&parsed, uuid, sizeof(uuid));
	require_check(error == 0, "format preserved swap UUID");
	require_check(strcmp(uuid, "0123456789ABCDEF") == 0, "preserve swap UUID bytes");

	/* Reject every one-byte corruption through the production checksum. */
	for (index = 0; index < sizeof(header); index++) {
		header[index] ^= 0x80U;
		error = swap_header_parse(header, bytes, NULL);
		require_check(error != 0, "reject damaged production header");
		header[index] ^= 0x80U;
	}

	/* Reject malformed label padding even after repairing its checksum. */
	header[49U] = 'X';
	checksum = swap_header_checksum(header);
	put32(header, 60U, checksum);
	error = swap_header_parse(header, bytes, NULL);
	require_check(error == EINVAL, "reject nonzero terminated-label padding");

	/* Begin deterministic-output validation from a complete generated image. */
	build_image(fd, 12288U);
	count = pread(fd, header, sizeof(header), 0);
	require_check(count == (ssize_t)sizeof(header), "read generated verification header");
	header[60U] ^= 1U;
	count = pwrite(fd, header, sizeof(header), 0);
	require_check(count == (ssize_t)sizeof(header), "damage generated checksum");
	error = swap_format_verify(fd, 12288U);
	require_check(error == EINVAL, "propagate production-parser rejection");

	/* Distinguish a valid alternate UUID from the generated zero-UUID result. */
	header[32U] = 1U;
	checksum = swap_header_checksum(header);
	put32(header, 60U, checksum);
	error = swap_header_parse(header, 12288U, NULL);
	require_check(error == 0, "accept valid alternate UUID in production parser");
	count = pwrite(fd, header, sizeof(header), 0);
	require_check(count == (ssize_t)sizeof(header), "write valid alternate UUID");
	error = swap_format_verify(fd, 12288U);
	require_check(error == EIO, "reject changed deterministic generated identity");
}
