/*
 * RTL8822B production firmware-loader fixture
 * Copyright (C) 2026 Awe Morris
 * SPDX-License-Identifier: Zlib
 */

#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "../../../src/drivers/rtl8822b-internal.h"
#include "kern/file.h"
#include "kern/vfs.h"

#define TEST_CHECK(condition) do { \
	if (!(condition)) \
		return __LINE__; \
} while (0)

extern const uint8_t _binary_firmware_bin_start[];
extern const uint8_t _binary_firmware_bin_end[];

struct cwdinfo kern_cwdinfo;

static uint8_t heap[2][RTL8822B_FIRMWARE_SIZE];
static unsigned heap_used[2];
static unsigned allocation_count;
static unsigned free_count;
static unsigned scrub_failure;
static unsigned open_count;
static unsigned lease_begin_count;
static unsigned lease_end_count;
static unsigned close_count;
static unsigned corrupt_read;

void *
kern_malloc(size_t size)
{
	unsigned index;

	if (size != RTL8822B_FIRMWARE_SIZE)
		return NULL;
	for (index = 0; index < 2U; index++) {
		if (!heap_used[index]) {
			heap_used[index] = 1U;
			allocation_count++;
			return heap[index];
		}
	}
	return NULL;
}

void
kern_free(void *pointer)
{
	unsigned index;

	for (index = 0; index < 2U; index++) {
		size_t byte;

		if (pointer != heap[index])
			continue;
		if (!heap_used[index]) {
			scrub_failure = 1U;
			return;
		}
		for (byte = 0; byte < RTL8822B_FIRMWARE_SIZE; byte++) {
			if (heap[index][byte] != 0U) {
				scrub_failure = 1U;
				break;
			}
		}
		heap_used[index] = 0U;
		free_count++;
		return;
	}
	scrub_failure = 1U;
}

int
file_openat(struct cwdinfo *context, const char *path, int flags,
	mode_t mode, struct file **result)
{
	if (context != &kern_cwdinfo || path == NULL || result == NULL ||
	    strcmp(path, RTL8822B_FIRMWARE_PATH) != 0 ||
	    flags != (O_RDONLY | O_NOFOLLOW) || mode != 0)
		return EINVAL;
	open_count++;
	*result = (struct file *)(uintptr_t)1U;
	return 0;
}

int
file_content_lease_begin(struct file *file,
	struct file_content_lease *lease)
{
	if (file != (struct file *)(uintptr_t)1U || lease == NULL)
		return EINVAL;
	memset(lease, 0, sizeof(*lease));
	lease->file = file;
	lease->size = RTL8822B_FIRMWARE_SIZE;
	lease->active = 1U;
	lease_begin_count++;
	return 0;
}

ssize_t
file_content_lease_pread(struct file_content_lease *lease, void *buffer,
	size_t length, off_t offset)
{
	size_t source_size;
	size_t source_offset;

	if (lease == NULL || !lease->active || buffer == NULL || offset < 0)
		return -EINVAL;
	source_size = (size_t)((uintptr_t)_binary_firmware_bin_end -
	    (uintptr_t)_binary_firmware_bin_start);
	if (source_size != RTL8822B_FIRMWARE_SIZE)
		return -EIO;
	source_offset = (size_t)offset;
	if (source_offset > source_size)
		return -EINVAL;
	if (length > source_size - source_offset)
		length = source_size - source_offset;
	memcpy(buffer, _binary_firmware_bin_start + source_offset, length);
	if (corrupt_read && source_offset <= RTL8822B_FIRMWARE_HEADER_SIZE &&
	    length > RTL8822B_FIRMWARE_HEADER_SIZE - source_offset) {
		uint8_t *bytes = buffer;

		bytes[RTL8822B_FIRMWARE_HEADER_SIZE - source_offset] ^= 1U;
	}
	return (ssize_t)length;
}

void
file_content_lease_end(struct file_content_lease *lease)
{
	if (lease == NULL || !lease->active) {
		scrub_failure = 1U;
		return;
	}
	lease->active = 0U;
	lease_end_count++;
}

int
file_close(struct file *file)
{
	if (file != (struct file *)(uintptr_t)1U)
		return EINVAL;
	close_count++;
	return 0;
}

int
main(void)
{
	struct rtl8822b_firmware_blob firmware;
	uint8_t *first;
	uint8_t *second;
	unsigned opens;

	memset(&firmware, 0, sizeof(firmware));
	TEST_CHECK(rtl8822b_firmware_load(&firmware) == 0);
	TEST_CHECK(firmware.bytes != NULL);
	TEST_CHECK(firmware.size == RTL8822B_FIRMWARE_SIZE);
	TEST_CHECK(memcmp(firmware.bytes, _binary_firmware_bin_start,
	    firmware.size) == 0);
	first = firmware.bytes;

	TEST_CHECK(rtl8822b_firmware_load(&firmware) == 0);
	second = firmware.bytes;
	TEST_CHECK(second != NULL && second != first);
	TEST_CHECK(free_count == 1U && !scrub_failure);
	TEST_CHECK(!heap_used[0] && heap_used[1]);

	corrupt_read = 1U;
	TEST_CHECK(rtl8822b_firmware_load(&firmware) == EILSEQ);
	corrupt_read = 0U;
	TEST_CHECK(firmware.bytes == second);
	TEST_CHECK(free_count == 2U && !scrub_failure);
	TEST_CHECK(!heap_used[0] && heap_used[1]);

	rtl8822b_firmware_release(&firmware);
	TEST_CHECK(firmware.bytes == NULL && firmware.size == 0U);
	TEST_CHECK(free_count == 3U && !scrub_failure);
	TEST_CHECK(!heap_used[0] && !heap_used[1]);
	TEST_CHECK(allocation_count == 3U);
	TEST_CHECK(open_count == 3U && lease_begin_count == 3U);
	TEST_CHECK(lease_end_count == 3U && close_count == 3U);

	opens = open_count;
	firmware.size = 1U;
	TEST_CHECK(rtl8822b_firmware_load(&firmware) == EINVAL);
	TEST_CHECK(open_count == opens && firmware.size == 1U);
	return 0;
}
