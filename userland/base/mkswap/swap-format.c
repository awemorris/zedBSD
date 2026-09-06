/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Writes the existing ZEDSWAP2 header without initializing unused slots.
 */

#include "swap-format.h"

#include <errno.h>
#include <kern/swap.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

static void put32(uint8_t *out, uint32_t value);
static void put64(uint8_t *out, uint64_t value);
static int write_exact(int fd, const void *data, size_t length, uint64_t offset);
static void make_header(uint8_t *header, uint64_t bytes);

/*
 * Checks the production file-backed swap geometry before mutation.
 */
int
swap_format_validate_size(
	uint64_t bytes)
{
	/* Requires a header page and at least one complete slot page. */
	if (bytes < SWAP_PAGE_SIZE * 2U || bytes % SWAP_PAGE_SIZE != 0)
		return EINVAL;

	/* Matches the current FAT-backed activation limit on every target ABI. */
	if (bytes > INT32_MAX)
		return EFBIG;

	/* Reports a usable production swap-file size. */
	return 0;
}

/*
 * Initializes the reserved header page and publishes the header last.
 *
 * The pager writes a complete page before publishing a slot for page-in.
 */
int
swap_format_write(
	int fd,
	uint64_t bytes)
{
	uint8_t block[SWAP_PAGE_SIZE];
	int error;

	/* Rejects unsupported sizes without issuing a write. */
	error = swap_format_validate_size(bytes);
	if (error != 0)
		return error;

	/* Invalidates any old header and clears the reserved header page. */
	memset(block, 0, sizeof(block));
	error = write_exact(fd, block, sizeof(block), 0);
	if (error != 0)
		return error;

	/* Publishes the same deterministic header as the maintained builder. */
	make_header(block, bytes);
	error = write_exact(fd, block, ZEDBSD_SWAP_HEADER_SIZE, 0);

	/* Leaves durability and descriptor ownership to the command frontend. */
	return error;
}

/*
 * Validates reopened bytes with the production swap parser.
 */
int
swap_format_verify(
	int fd,
	uint64_t bytes)
{
	struct swap_header_info parsed;
	uint8_t header[SWAP_PAGE_SIZE];
	uint8_t expected[SWAP_PAGE_SIZE];
	ssize_t done;
	int error;

	/* Requires the same size accepted by generation. */
	error = swap_format_validate_size(bytes);
	if (error != 0)
		return error;

	/* Reads the complete reserved page, retrying only interrupted reads. */
	do {
		done = pread(fd, header, sizeof(header), 0);
	} while (done < 0 && errno == EINTR);

	/* Preserves read failures and rejects truncated verification data. */
	if (done < 0)
		return errno;
	if (done != (ssize_t)sizeof(header))
		return EIO;

	/* Applies the shared kernel parser before checking generated invariants. */
	error = swap_header_parse(header, bytes, &parsed);
	if (error != 0)
		return error;

	/* Requires version two and the exact positive caller-sized slot count. */
	if (parsed.version != 2 || parsed.slot_count == 0 ||
	    parsed.slot_count != bytes / SWAP_PAGE_SIZE - 1)
		return EIO;

	/* Verifies the identity, checksum, and cleared reserved page contents. */
	memset(expected, 0, sizeof(expected));
	make_header(expected, bytes);
	if (memcmp(header, expected, sizeof(header)) != 0)
		return EIO;

	/* Reports a recognized and complete swap header. */
	return 0;
}

/* Encodes one little-endian word without alignment assumptions. */
static void
put32(
	uint8_t *out,
	uint32_t value)
{
	/* Stores bytes in the production on-disk order. */
	out[0] = (uint8_t)value;
	out[1] = (uint8_t)(value >> 8);
	out[2] = (uint8_t)(value >> 16);
	out[3] = (uint8_t)(value >> 24);
}

/* Encodes the two halves of a little-endian wide word. */
static void
put64(
	uint8_t *out,
	uint64_t value)
{
	/* Stores the low word before the high word. */
	put32(out, (uint32_t)value);
	put32(out + 4, (uint32_t)(value >> 32));
}

/* Rejects any partial write rather than reporting a complete format. */
static int
write_exact(
	int fd,
	const void *data,
	size_t length,
	uint64_t offset)
{
	ssize_t done;

	/* Retries only a write that transferred no bytes due to interruption. */
	do {
		done = pwrite(fd, data, length, (off_t)offset);
	} while (done < 0 && errno == EINTR);

	/* Preserves backend errors and detects incomplete writes. */
	if (done < 0)
		return errno;
	if (done != (ssize_t)length)
		return EIO;

	/* Reports the exact requested transfer. */
	return 0;
}

/* Builds the unlabeled version-two header with its production checksum. */
static void
make_header(
	uint8_t *header,
	uint64_t bytes)
{
	/* Initializes fixed fields and zero UUID/label/reserved bytes. */
	memset(header, 0, ZEDBSD_SWAP_HEADER_SIZE);
	memcpy(header, "ZEDSWAP2", 8);
	header[8] = 2;
	header[10] = ZEDBSD_SWAP_HEADER_SIZE;
	put32(header + 12, SWAP_PAGE_SIZE);
	put64(header + 16, bytes);
	put64(header + 24, bytes / SWAP_PAGE_SIZE - 1);
	put32(header + 60, swap_header_checksum(header));
}
