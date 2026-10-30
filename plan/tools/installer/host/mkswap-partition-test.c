/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The host test of mkswap's partition mode: the size rules (a 64-bit size
 * past the regular file's limit, whole pages, 32-bit slots) and the header
 * page written and reread through a sparse file that stands for a
 * partition.
 */

#include "userland/base/mkswap/swap-format.h"

#include <kern/swap.h>

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void check(int condition, const char *what);

/* How many checks failed. */
static int failures;

/*
 * Runs the checks against a sparse file in the directory named by argv[1].
 */
int
main(
	int argc,
	char **argv)
{
	struct swap_header_info parsed;
	uint8_t page[SWAP_PAGE_SIZE];
	char path[4096];
	uint64_t bytes;
	ssize_t done;
	int error;
	int fd;

	/* Takes the scratch directory. */
	if (argc != 2) {
		fprintf(stderr, "usage: mkswap-partition-test DIRECTORY\n");
		return 2;
	}

	/* Checks the size rules. */
	check(swap_format_validate_partition(8192) == 0, "two pages");
	check(swap_format_validate_partition(4096) == EINVAL, "one page is too small");
	check(swap_format_validate_partition(8192 + 512) == EINVAL, "a partial page");
	check(swap_format_validate_partition(UINT64_C(5) << 30) == 0, "5 GiB, past the file limit");
	check(swap_format_validate_size(UINT64_C(5) << 30) == EFBIG, "the file mode keeps its limit");
	bytes = ((uint64_t)UINT32_MAX + 2U) * SWAP_PAGE_SIZE;
	check(swap_format_validate_partition(bytes) == EFBIG, "more slots than 32 bits");
	bytes = ((uint64_t)UINT32_MAX + 1U) * SWAP_PAGE_SIZE;
	check(swap_format_validate_partition(bytes) == 0, "the most slots");

	/* Makes a 5-GiB sparse file filled with a pattern in its first page. */
	snprintf(path, sizeof(path), "%s/partition", argv[1]);
	fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0600);
	if (fd < 0) {
		perror(path);
		return 1;
	}
	bytes = UINT64_C(5) << 30;
	memset(page, 0xa5, sizeof(page));
	done = pwrite(fd, page, sizeof(page), 0);
	check(done == (ssize_t)sizeof(page), "the pattern");
	error = ftruncate(fd, (off_t)bytes);
	check(error == 0, "the size");

	/* Writes the header page and rereads it with the shared parser. */
	error = swap_format_write_partition(fd, bytes);
	check(error == 0, "write the header page");
	error = swap_format_verify_partition(fd, bytes);
	check(error == 0, "verify the header page");
	done = pread(fd, page, sizeof(page), 0);
	check(done == (ssize_t)sizeof(page), "reread the page");
	error = swap_header_parse(page, bytes, &parsed);
	check(error == 0, "the kernel's parser accepts it");
	check(parsed.version == 2, "version two");
	check(parsed.slot_count == bytes / SWAP_PAGE_SIZE - 1U, "the slot count");
	check(page[SWAP_PAGE_SIZE - 1] == 0, "the rest of the page is zero");

	/* Refuses a header page checked against another size. */
	error = swap_format_verify_partition(fd, bytes - SWAP_PAGE_SIZE);
	check(error != 0, "a header of another size is refused");
	close(fd);

	/* Reports the verdict. */
	if (failures != 0) {
		printf("mkswap-partition-test: FAIL (%d)\n", failures);
		return 1;
	}
	printf("mkswap-partition-test: PASS\n");
	return 0;
}

/* Counts and prints a failed check. */
static void
check(
	int condition,
	const char *what)
{
	/* Reports a failure by name. */
	if (!condition) {
		printf("FAIL: %s\n", what);
		failures++;
	}
}
