/*
 * Intel AX211 production firmware-file-loader fixture
 * Copyright (C) 2026 Awe Morris
 * SPDX-License-Identifier: Zlib
 */

#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../../src/drivers/intel-ax211-firmware.h"
#include "kern/file.h"
#include "kern/vfs.h"

#define TEST_CHECK(condition) do { \
	if (!(condition)) { \
		fprintf(stderr, "intel ax211 firmware loader: check failed at %s:%d\n", \
		    __FILE__, __LINE__); \
		return __LINE__; \
	} \
} while (0)

#define FIXTURE_GUARD_SIZE 32U
#define FIXTURE_ALLOCATION_COUNT 8U
#define FIXTURE_READ_CHUNK 4096U

extern const uint8_t _binary_ucode_bin_start[];
extern const uint8_t _binary_ucode_bin_end[];
extern const uint8_t _binary_pnvm_bin_start[];
extern const uint8_t _binary_pnvm_bin_end[];

struct cwdinfo kern_cwdinfo;

enum fixture_file_kind {
	FIXTURE_FILE_NONE = 0,
	FIXTURE_FILE_UCODE = 1,
	FIXTURE_FILE_PNVM = 2
};

enum fixture_fault {
	FIXTURE_FAULT_NONE = 0,
	FIXTURE_FAULT_OPEN,
	FIXTURE_FAULT_LEASE,
	FIXTURE_FAULT_SIZE,
	FIXTURE_FAULT_ALLOCATE,
	FIXTURE_FAULT_SHORT_READ,
	FIXTURE_FAULT_READ,
	FIXTURE_FAULT_DIGEST,
	FIXTURE_FAULT_PARSE,
	FIXTURE_FAULT_CLOSE
};

struct fixture_allocation {
	uint8_t *base;
	uint8_t *payload;
	size_t size;
	int active;
};

static struct fixture_allocation allocations[FIXTURE_ALLOCATION_COUNT];
static enum fixture_fault fixture_fault;
static enum fixture_file_kind fault_file;
static unsigned allocation_count;
static unsigned free_count;
static unsigned scrub_count;
static unsigned scrub_failure;
static unsigned open_count;
static unsigned lease_begin_count;
static unsigned lease_end_count;
static unsigned close_count;

static struct file *
fixture_file(enum fixture_file_kind kind)
{
	return (struct file *)(uintptr_t)kind;
}

static enum fixture_file_kind
fixture_file_kind(struct file *file)
{
	uintptr_t value = (uintptr_t)file;

	if (value == FIXTURE_FILE_UCODE || value == FIXTURE_FILE_PNVM)
		return (enum fixture_file_kind)value;
	return FIXTURE_FILE_NONE;
}

static size_t
fixture_size(enum fixture_file_kind kind)
{
	if (kind == FIXTURE_FILE_UCODE)
		return INTEL_AX211_FIRMWARE_SIZE;
	if (kind == FIXTURE_FILE_PNVM)
		return INTEL_AX211_PNVM_SIZE;
	return 0U;
}

static const uint8_t *
fixture_bytes(enum fixture_file_kind kind)
{
	if (kind == FIXTURE_FILE_UCODE)
		return _binary_ucode_bin_start;
	if (kind == FIXTURE_FILE_PNVM)
		return _binary_pnvm_bin_start;
	return NULL;
}

static size_t
fixture_embedded_size(enum fixture_file_kind kind)
{
	if (kind == FIXTURE_FILE_UCODE)
		return (size_t)(_binary_ucode_bin_end - _binary_ucode_bin_start);
	if (kind == FIXTURE_FILE_PNVM)
		return (size_t)(_binary_pnvm_bin_end - _binary_pnvm_bin_start);
	return 0U;
}

static unsigned
fixture_active_allocations(void)
{
	unsigned active = 0U;
	unsigned index;

	for (index = 0U; index < FIXTURE_ALLOCATION_COUNT; index++) {
		if (allocations[index].active)
			active++;
	}
	return active;
}

static int
fixture_guard_is(const uint8_t *bytes, size_t length, uint8_t value)
{
	size_t index;

	for (index = 0U; index < length; index++) {
		if (bytes[index] != value)
			return 0;
	}
	return 1;
}

void *
kern_malloc(size_t size)
{
	unsigned index;

	if (size != INTEL_AX211_FIRMWARE_SIZE && size != INTEL_AX211_PNVM_SIZE)
		return NULL;
	if (fixture_fault == FIXTURE_FAULT_ALLOCATE &&
	    fixture_size(fault_file) == size)
		return NULL;
	for (index = 0U; index < FIXTURE_ALLOCATION_COUNT; index++) {
		struct fixture_allocation *allocation = &allocations[index];

		if (allocation->active)
			continue;
		allocation->base = malloc(size + FIXTURE_GUARD_SIZE * 2U);
		if (allocation->base == NULL)
			return NULL;
		allocation->payload = allocation->base + FIXTURE_GUARD_SIZE;
		allocation->size = size;
		allocation->active = 1;
		memset(allocation->base, 0xa5, FIXTURE_GUARD_SIZE);
		memset(allocation->payload, 0xcc, size);
		memset(allocation->payload + size, 0x5a, FIXTURE_GUARD_SIZE);
		allocation_count++;
		return allocation->payload;
	}
	return NULL;
}

void
kern_free(void *pointer)
{
	unsigned index;

	for (index = 0U; index < FIXTURE_ALLOCATION_COUNT; index++) {
		struct fixture_allocation *allocation = &allocations[index];

		if (!allocation->active || allocation->payload != pointer)
			continue;
		if (!fixture_guard_is(allocation->base, FIXTURE_GUARD_SIZE,
		    0xa5U) ||
		    !fixture_guard_is(allocation->payload, allocation->size, 0U) ||
		    !fixture_guard_is(allocation->payload + allocation->size,
		    FIXTURE_GUARD_SIZE, 0x5aU))
			scrub_failure = 1U;
		else
			scrub_count++;
		free(allocation->base);
		memset(allocation, 0, sizeof(*allocation));
		free_count++;
		return;
	}
	scrub_failure = 1U;
}

int
file_openat(struct cwdinfo *context, const char *path, int flags,
	mode_t mode, struct file **result)
{
	enum fixture_file_kind kind = FIXTURE_FILE_NONE;

	if (context != &kern_cwdinfo || path == NULL || result == NULL ||
	    flags != (O_RDONLY | O_NOFOLLOW) || mode != 0)
		return EINVAL;
	if (strcmp(path, INTEL_AX211_FIRMWARE_VFS_PATH) == 0)
		kind = FIXTURE_FILE_UCODE;
	else if (strcmp(path, INTEL_AX211_PNVM_VFS_PATH) == 0)
		kind = FIXTURE_FILE_PNVM;
	else
		return ENOENT;
	open_count++;
	if (fixture_fault == FIXTURE_FAULT_OPEN && fault_file == kind)
		return EACCES;
	*result = fixture_file(kind);
	return 0;
}

int
file_content_lease_begin(struct file *file, struct file_content_lease *lease)
{
	enum fixture_file_kind kind = fixture_file_kind(file);

	if (kind == FIXTURE_FILE_NONE || lease == NULL)
		return EINVAL;
	lease_begin_count++;
	if (fixture_fault == FIXTURE_FAULT_LEASE && fault_file == kind)
		return EBUSY;
	memset(lease, 0, sizeof(*lease));
	lease->file = file;
	lease->size = (off_t)fixture_size(kind);
	if (fixture_fault == FIXTURE_FAULT_SIZE && fault_file == kind)
		lease->size--;
	lease->active = 1U;
	return 0;
}

ssize_t
file_content_lease_pread(struct file_content_lease *lease, void *buffer,
	size_t length, off_t offset)
{
	enum fixture_file_kind kind;
	const uint8_t *source;
	size_t source_size;
	size_t source_offset;

	if (lease == NULL || !lease->active || buffer == NULL || offset < 0)
		return -EINVAL;
	kind = fixture_file_kind(lease->file);
	source = fixture_bytes(kind);
	source_size = fixture_embedded_size(kind);
	source_offset = (size_t)offset;
	if (kind == FIXTURE_FILE_NONE || source == NULL ||
	    source_size != fixture_size(kind) || source_offset > source_size)
		return -EIO;
	if (fixture_fault == FIXTURE_FAULT_SHORT_READ && fault_file == kind &&
	    source_offset >= FIXTURE_READ_CHUNK)
		return 0;
	if (fixture_fault == FIXTURE_FAULT_READ && fault_file == kind &&
	    source_offset >= FIXTURE_READ_CHUNK)
		return -EIO;
	if (length > FIXTURE_READ_CHUNK)
		length = FIXTURE_READ_CHUNK;
	if (length > source_size - source_offset)
		length = source_size - source_offset;
	memcpy(buffer, source + source_offset, length);
	if (fixture_fault == FIXTURE_FAULT_DIGEST && fault_file == kind &&
	    source_offset == 0U && length != 0U)
		((uint8_t *)buffer)[0] ^= 1U;
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
	enum fixture_file_kind kind = fixture_file_kind(file);

	if (kind == FIXTURE_FILE_NONE)
		return EINVAL;
	close_count++;
	if (fixture_fault == FIXTURE_FAULT_CLOSE && fault_file == kind)
		return EIO;
	return 0;
}

int
intel_ax211_firmware_loader_host_parse(const uint8_t *bytes, size_t length,
	struct intel_ax211_firmware_manifest *manifest)
{
	if (fixture_fault == FIXTURE_FAULT_PARSE &&
	    fault_file == FIXTURE_FILE_UCODE)
		return INTEL_AX211_INVALID;
	return intel_ax211_firmware_parse(bytes, length, manifest);
}

int
intel_ax211_firmware_loader_host_inspect_pnvm(const uint8_t *bytes,
	size_t length, struct intel_ax211_pnvm_inventory *inventory)
{
	if (fixture_fault == FIXTURE_FAULT_PARSE &&
	    fault_file == FIXTURE_FILE_PNVM)
		return INTEL_AX211_INVALID;
	return intel_ax211_pnvm_inspect(bytes, length, inventory);
}

static void
fixture_set_fault(enum fixture_fault fault, enum fixture_file_kind kind)
{
	fixture_fault = fault;
	fault_file = kind;
}

static int
fixture_expect_failure(struct intel_ax211_firmware_files *files,
	enum fixture_fault fault, enum fixture_file_kind kind, int expected)
{
	struct intel_ax211_firmware_files before = *files;
	unsigned active_before = fixture_active_allocations();
	int error;

	fixture_set_fault(fault, kind);
	error = intel_ax211_firmware_files_load(files);
	fixture_set_fault(FIXTURE_FAULT_NONE, FIXTURE_FILE_NONE);
	TEST_CHECK(error == expected);
	TEST_CHECK(memcmp(files, &before, sizeof(before)) == 0);
	TEST_CHECK(fixture_active_allocations() == active_before);
	TEST_CHECK(!scrub_failure);
	TEST_CHECK(scrub_count == free_count);
	return 0;
}

static int
fixture_sha256_test(void)
{
	static const uint8_t expected[32] = {
		0xbaU, 0x78U, 0x16U, 0xbfU, 0x8fU, 0x01U, 0xcfU, 0xeaU,
		0x41U, 0x41U, 0x40U, 0xdeU, 0x5dU, 0xaeU, 0x22U, 0x23U,
		0xb0U, 0x03U, 0x61U, 0xa3U, 0x96U, 0x17U, 0x7aU, 0x9cU,
		0xb4U, 0x10U, 0xffU, 0x61U, 0xf2U, 0x00U, 0x15U, 0xadU
	};
	uint8_t actual[32];

	TEST_CHECK(intel_ax211_firmware_loader_test_sha256("abc", 3U,
	    actual) == 0);
	TEST_CHECK(memcmp(actual, expected, sizeof(actual)) == 0);
	return 0;
}

int
main(void)
{
	struct intel_ax211_firmware_files files;
	uint8_t *first_ucode;
	uint8_t *first_pnvm;
	uint8_t *second_ucode;
	uint8_t *second_pnvm;
	unsigned opens;
	int error;

	TEST_CHECK(fixture_embedded_size(FIXTURE_FILE_UCODE) ==
	    INTEL_AX211_FIRMWARE_SIZE);
	TEST_CHECK(fixture_embedded_size(FIXTURE_FILE_PNVM) ==
	    INTEL_AX211_PNVM_SIZE);
	TEST_CHECK(strcmp(INTEL_AX211_FIRMWARE_VFS_PATH,
	    "/lib/firmware/intel/iwlwifi/iwlwifi-so-a0-gf-a0-89.ucode") == 0);
	TEST_CHECK(strcmp(INTEL_AX211_PNVM_VFS_PATH,
	    "/lib/firmware/intel/iwlwifi/iwlwifi-so-a0-gf-a0.pnvm") == 0);
	error = fixture_sha256_test();
	if (error != 0)
		return error;

	memset(&files, 0, sizeof(files));
	TEST_CHECK(intel_ax211_firmware_files_load(&files) == 0);
	TEST_CHECK(files.ucode_size == INTEL_AX211_FIRMWARE_SIZE);
	TEST_CHECK(files.pnvm_size == INTEL_AX211_PNVM_SIZE);
	TEST_CHECK(files.ucode_manifest.api_major == INTEL_AX211_FIRMWARE_API);
	TEST_CHECK(files.ucode_manifest.api_minor == INTEL_AX211_FIRMWARE_MINOR);
	TEST_CHECK(files.pnvm_inventory.sku_count != 0U);
	TEST_CHECK(files.pnvm_inventory.supported_hardware_type_count != 0U);
	TEST_CHECK(memcmp(files.ucode_bytes, _binary_ucode_bin_start,
	    files.ucode_size) == 0);
	TEST_CHECK(memcmp(files.pnvm_bytes, _binary_pnvm_bin_start,
	    files.pnvm_size) == 0);
	first_ucode = files.ucode_bytes;
	first_pnvm = files.pnvm_bytes;
	TEST_CHECK(fixture_active_allocations() == 2U);

	TEST_CHECK(intel_ax211_firmware_files_load(&files) == 0);
	second_ucode = files.ucode_bytes;
	second_pnvm = files.pnvm_bytes;
	TEST_CHECK(second_ucode != first_ucode && second_pnvm != first_pnvm);
	TEST_CHECK(fixture_active_allocations() == 2U);
	TEST_CHECK(free_count == 2U && scrub_count == 2U && !scrub_failure);

	error = fixture_expect_failure(&files, FIXTURE_FAULT_OPEN,
	    FIXTURE_FILE_UCODE, EACCES);
	if (error != 0)
		return error;
	error = fixture_expect_failure(&files, FIXTURE_FAULT_LEASE,
	    FIXTURE_FILE_PNVM, EBUSY);
	if (error != 0)
		return error;
	error = fixture_expect_failure(&files, FIXTURE_FAULT_SIZE,
	    FIXTURE_FILE_UCODE, EINVAL);
	if (error != 0)
		return error;
	error = fixture_expect_failure(&files, FIXTURE_FAULT_SIZE,
	    FIXTURE_FILE_PNVM, EINVAL);
	if (error != 0)
		return error;
	error = fixture_expect_failure(&files, FIXTURE_FAULT_ALLOCATE,
	    FIXTURE_FILE_UCODE, ENOMEM);
	if (error != 0)
		return error;
	error = fixture_expect_failure(&files, FIXTURE_FAULT_ALLOCATE,
	    FIXTURE_FILE_PNVM, ENOMEM);
	if (error != 0)
		return error;
	error = fixture_expect_failure(&files, FIXTURE_FAULT_SHORT_READ,
	    FIXTURE_FILE_UCODE, EIO);
	if (error != 0)
		return error;
	error = fixture_expect_failure(&files, FIXTURE_FAULT_SHORT_READ,
	    FIXTURE_FILE_PNVM, EIO);
	if (error != 0)
		return error;
	error = fixture_expect_failure(&files, FIXTURE_FAULT_READ,
	    FIXTURE_FILE_UCODE, EIO);
	if (error != 0)
		return error;
	error = fixture_expect_failure(&files, FIXTURE_FAULT_DIGEST,
	    FIXTURE_FILE_UCODE, EILSEQ);
	if (error != 0)
		return error;
	error = fixture_expect_failure(&files, FIXTURE_FAULT_DIGEST,
	    FIXTURE_FILE_PNVM, EILSEQ);
	if (error != 0)
		return error;
	error = fixture_expect_failure(&files, FIXTURE_FAULT_PARSE,
	    FIXTURE_FILE_UCODE, EILSEQ);
	if (error != 0)
		return error;
	error = fixture_expect_failure(&files, FIXTURE_FAULT_PARSE,
	    FIXTURE_FILE_PNVM, EILSEQ);
	if (error != 0)
		return error;
	error = fixture_expect_failure(&files, FIXTURE_FAULT_CLOSE,
	    FIXTURE_FILE_UCODE, EIO);
	if (error != 0)
		return error;
	error = fixture_expect_failure(&files, FIXTURE_FAULT_CLOSE,
	    FIXTURE_FILE_PNVM, EIO);
	if (error != 0)
		return error;

	intel_ax211_firmware_files_release(&files);
	TEST_CHECK(files.ucode_bytes == NULL && files.ucode_size == 0U);
	TEST_CHECK(files.pnvm_bytes == NULL && files.pnvm_size == 0U);
	TEST_CHECK(fixture_active_allocations() == 0U);
	TEST_CHECK(scrub_count == free_count && !scrub_failure);
	TEST_CHECK(open_count != 0U && lease_begin_count != 0U);
	TEST_CHECK(lease_begin_count == lease_end_count + 1U);
	TEST_CHECK(close_count + 1U == open_count);

	opens = open_count;
	files.ucode_size = 1U;
	TEST_CHECK(intel_ax211_firmware_files_load(&files) == EINVAL);
	TEST_CHECK(open_count == opens && files.ucode_size == 1U);
	memset(&files, 0, sizeof(files));
	return 0;
}
