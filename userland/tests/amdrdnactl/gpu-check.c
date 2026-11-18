/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The GPU device check of the amdrdna control tool (amdrdnactl gpu): opens
 * the GPU device the driver registered and goes through what it offers
 * before a renderer is bound.  It names the device, makes a blob in the
 * VRAM and writes and reads it by transfers and through a CPU mapping,
 * makes ordinary storage and does the same, and shares the blob with a
 * second open of the device, which reads the same bytes.  Every step
 * prints one line; the first failure ends the check.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <uapi/gpu.h>

#include "amdrdnactl.h"

/* The blob's bytes and the bytes each transfer moves. */
#define GPU_CHECK_BLOB_BYTES		65536U
#define GPU_CHECK_BYTES			4096U

/* The shared blob's image: 32 by 32 pixels of four bytes. */
#define GPU_CHECK_EDGE			32U

/* The step of the pattern (odd, so its words all differ) and the value the mapping writes. */
#define GPU_CHECK_STEP			2654435761U
#define GPU_CHECK_MAPPED		1234567U

static int check_fail(const char *step, int error);
static int check_blob(int descriptor, uint64_t *handle);
static int check_transfer(int descriptor, uint64_t handle, const uint32_t *pattern, const char *what);
static int check_mapping(int descriptor, uint64_t handle, const uint32_t *pattern);
static int check_storage(int descriptor);
static int check_share(int descriptor, const char *path, uint64_t handle, const uint32_t *pattern);
static void check_destroy(int descriptor, uint64_t handle);

/*
 * Checks the GPU device at path; returns 0, or 1 after the first step that
 * failed.
 */
int
control_gpu(
	const char *path)
{
	struct gpu_info info;
	uint32_t pattern[GPU_CHECK_BYTES / 4U];
	uint64_t blob;
	unsigned index;
	int descriptor;
	int status;

	/* Opens the device. */
	descriptor = open(path, O_RDWR);
	if (descriptor < 0)
		return check_fail(path, errno);

	/* Names it. */
	memset(&info, 0, sizeof(info));
	info.version = GPU_ABI_VERSION;
	info.size = sizeof(info);
	status = ioctl(descriptor, GPU_GET_INFO, &info);
	if (status != 0) {
		(void)close(descriptor);
		return check_fail("info", errno);
	}

	/* Says what the device is. */
	printf("gpu %s driver=%s capabilities=0x%x resources=%u bytes=%llu\n",
	    path,
	    info.driver_name,
	    (unsigned)info.capabilities,
	    (unsigned)info.max_resources,
	    (unsigned long long)info.max_resource_bytes);

	/* The pattern the transfers write. */
	for (index = 0U; index < GPU_CHECK_BYTES / 4U; index++)
		pattern[index] = index * GPU_CHECK_STEP;

	/* A blob, its transfers, its mapping, storage, and the blob shared. */
	status = check_blob(descriptor, &blob);
	if (status == 0)
		status = check_transfer(descriptor, blob, pattern, "blob");
	if (status == 0)
		status = check_mapping(descriptor, blob, pattern);
	if (status == 0)
		status = check_storage(descriptor);
	if (status == 0)
		status = check_share(descriptor, path, blob, pattern);

	/* The blob goes, and the device is closed. */
	check_destroy(descriptor, blob);
	(void)close(descriptor);

	/* Reports the first failure. */
	if (status != 0)
		return status;

	/* Succeeded: every step passed. */
	printf("gpu ok\n");
	return 0;
}

/* Reports a failed step and gives the failing exit status. */
static int
check_fail(
	const char *step,
	int error)
{
	/* The line a reader of the run sees. */
	fprintf(stderr, "amdrdnactl: gpu %s: %s\n", step, strerror(error));
	return 1;
}

/* Makes a mappable, shareable blob in the VRAM. */
static int
check_blob(
	int descriptor,
	uint64_t *handle)
{
	struct gpu_blob_create request;
	int status;

	/* The request. */
	*handle = 0U;
	memset(&request, 0, sizeof(request));
	request.version = GPU_ABI_VERSION;
	request.size = sizeof(request);
	request.bytes = GPU_CHECK_BLOB_BYTES;
	request.flags = GPU_BLOB_MAPPABLE | GPU_BLOB_SHAREABLE;

	/* Makes it. */
	status = ioctl(descriptor, GPU_BLOB_CREATE, &request);
	if (status != 0)
		return check_fail("blob", errno);

	/* Succeeded: the blob is the open's. */
	*handle = request.handle;
	printf("gpu blob %u bytes resource=%u\n", GPU_CHECK_BLOB_BYTES, (unsigned)request.resource_id);
	return 0;
}

/* Writes the pattern into a resource by a transfer, reads it back and compares. */
static int
check_transfer(
	int descriptor,
	uint64_t handle,
	const uint32_t *pattern,
	const char *what)
{
	struct gpu_transfer request;
	uint32_t back[GPU_CHECK_BYTES / 4U];
	int differs;
	int status;

	/* Writes the pattern at the start. */
	memset(&request, 0, sizeof(request));
	request.version = GPU_ABI_VERSION;
	request.size = sizeof(request);
	request.handle = handle;
	request.offset = 0U;
	request.address = (uint64_t)(uintptr_t)pattern;
	request.bytes = GPU_CHECK_BYTES;
	status = ioctl(descriptor, GPU_RESOURCE_WRITE, &request);
	if (status != 0)
		return check_fail("write", errno);

	/* Reads it back. */
	memset(back, 0, sizeof(back));
	request.address = (uint64_t)(uintptr_t)back;
	status = ioctl(descriptor, GPU_RESOURCE_READ, &request);
	if (status != 0)
		return check_fail("read", errno);

	/* Compares. */
	differs = memcmp(back, pattern, GPU_CHECK_BYTES);
	if (differs != 0) {
		fprintf(stderr, "amdrdnactl: gpu %s: read back differs\n", what);
		return 1;
	}

	/* Succeeded: the bytes came back. */
	printf("gpu %s transfer %u bytes ok\n", what, GPU_CHECK_BYTES);
	return 0;
}

/*
 * Maps the blob, compares the mapping with the pattern, writes a word
 * through it and reads that word back by a transfer.
 */
static int
check_mapping(
	int descriptor,
	uint64_t handle,
	const uint32_t *pattern)
{
	struct gpu_resource_map request;
	struct gpu_transfer transfer;
	volatile uint32_t *words;
	uint32_t word;
	void *address;
	unsigned index;
	int status;

	/* The mapping's offset. */
	memset(&request, 0, sizeof(request));
	request.version = GPU_ABI_VERSION;
	request.size = sizeof(request);
	request.handle = handle;
	status = ioctl(descriptor, GPU_RESOURCE_MAP, &request);
	if (status != 0)
		return check_fail("map", errno);

	/* Maps it. */
	address = mmap(NULL, (size_t)request.bytes, PROT_READ | PROT_WRITE, MAP_SHARED, descriptor, (off_t)request.offset);
	if (address == MAP_FAILED)
		return check_fail("mmap", errno);

	/* Compares what the transfer wrote. */
	words = address;
	for (index = 0U; index < GPU_CHECK_BYTES / 4U; index++) {
		if (words[index] != pattern[index]) {
			fprintf(stderr, "amdrdnactl: gpu mapping word %u is 0x%08x, not 0x%08x\n", index, (unsigned)words[index], (unsigned)pattern[index]);
			(void)munmap(address, (size_t)request.bytes);
			return 1;
		}
	}

	/* Writes past the pattern through the mapping. */
	words[GPU_CHECK_BYTES / 4U] = GPU_CHECK_MAPPED;
	(void)munmap(address, (size_t)request.bytes);

	/* Reads that word by a transfer. */
	memset(&transfer, 0, sizeof(transfer));
	transfer.version = GPU_ABI_VERSION;
	transfer.size = sizeof(transfer);
	transfer.handle = handle;
	transfer.offset = GPU_CHECK_BYTES;
	transfer.address = (uint64_t)(uintptr_t)&word;
	transfer.bytes = sizeof(word);
	word = 0U;
	status = ioctl(descriptor, GPU_RESOURCE_READ, &transfer);
	if (status != 0)
		return check_fail("read after map", errno);
	if (word != GPU_CHECK_MAPPED) {
		fprintf(stderr, "amdrdnactl: gpu the mapped write reads 0x%08x\n", (unsigned)word);
		return 1;
	}

	/* Succeeded: the mapping sees the blob. */
	printf("gpu blob mapping %llu bytes ok\n", (unsigned long long)request.bytes);
	return 0;
}

/* Makes ordinary storage, writes and reads it, and destroys it. */
static int
check_storage(
	int descriptor)
{
	struct gpu_resource_create request;
	uint32_t pattern[GPU_CHECK_BYTES / 4U];
	unsigned index;
	int status;

	/* Makes it. */
	memset(&request, 0, sizeof(request));
	request.version = GPU_ABI_VERSION;
	request.size = sizeof(request);
	request.bytes = GPU_CHECK_BYTES;
	request.usage = GPU_RESOURCE_USAGE_STORAGE;
	status = ioctl(descriptor, GPU_RESOURCE_CREATE, &request);
	if (status != 0)
		return check_fail("storage", errno);

	/* A pattern of its own, written and read back. */
	for (index = 0U; index < GPU_CHECK_BYTES / 4U; index++)
		pattern[index] = ~index;
	status = check_transfer(descriptor, request.handle, pattern, "storage");

	/* The storage goes. */
	check_destroy(descriptor, request.handle);

	/* Reports a failed transfer. */
	if (status != 0)
		return status;

	/* Succeeded: storage works. */
	return 0;
}

/*
 * Exports the blob, imports it into a second open of the device and reads
 * the pattern there.
 */
static int
check_share(
	int descriptor,
	const char *path,
	uint64_t handle,
	const uint32_t *pattern)
{
	struct gpu_resource_export exported;
	struct gpu_resource_import imported;
	struct gpu_transfer transfer;
	uint32_t back[GPU_CHECK_BYTES / 4U];
	int differs;
	int other;
	int status;

	/* Exports it as a linear image of 32 by 32 pixels. */
	memset(&exported, 0, sizeof(exported));
	exported.version = GPU_ABI_VERSION;
	exported.size = sizeof(exported);
	exported.handle = handle;
	exported.fd = -1;
	exported.image.version = GPU_ABI_VERSION;
	exported.image.size = sizeof(exported.image);
	exported.image.width = GPU_CHECK_EDGE;
	exported.image.height = GPU_CHECK_EDGE;
	exported.image.format = GPU_PIXEL_BGRA8888;
	exported.image.stride = GPU_CHECK_EDGE * 4U;
	exported.image.allocation_bytes = GPU_CHECK_BLOB_BYTES;
	exported.image.usage = 1U;
	exported.image.tiling = GPU_IMAGE_LINEAR;
	status = ioctl(descriptor, GPU_RESOURCE_EXPORT, &exported);
	if (status != 0)
		return check_fail("export", errno);

	/* A second open of the device. */
	other = open(path, O_RDWR);
	if (other < 0) {
		(void)close(exported.fd);
		return check_fail("second open", errno);
	}

	/* Imports the blob there. */
	memset(&imported, 0, sizeof(imported));
	imported.version = GPU_ABI_VERSION;
	imported.size = sizeof(imported);
	imported.fd = exported.fd;
	status = ioctl(other, GPU_RESOURCE_IMPORT, &imported);
	(void)close(exported.fd);
	if (status != 0) {
		(void)close(other);
		return check_fail("import", errno);
	}

	/* Reads the pattern through the import. */
	memset(&transfer, 0, sizeof(transfer));
	transfer.version = GPU_ABI_VERSION;
	transfer.size = sizeof(transfer);
	transfer.handle = imported.handle;
	transfer.address = (uint64_t)(uintptr_t)back;
	transfer.bytes = GPU_CHECK_BYTES;
	status = ioctl(other, GPU_RESOURCE_READ, &transfer);
	if (status != 0) {
		(void)close(other);
		return check_fail("read the import", errno);
	}

	/* The import goes with the second open. */
	check_destroy(other, imported.handle);
	(void)close(other);

	/* Compares. */
	differs = memcmp(back, pattern, GPU_CHECK_BYTES);
	if (differs != 0) {
		fprintf(stderr, "amdrdnactl: gpu the import reads other bytes\n");
		return 1;
	}

	/* Succeeded: the second open sees the blob's bytes. */
	printf("gpu share export/import resource=%u ok\n", (unsigned)imported.resource_id);
	return 0;
}

/* Destroys a resource; a handle of 0 is none. */
static void
check_destroy(
	int descriptor,
	uint64_t handle)
{
	struct gpu_resource_destroy request;

	/* No resource. */
	if (handle == 0U)
		return;

	/* The request. */
	memset(&request, 0, sizeof(request));
	request.version = GPU_ABI_VERSION;
	request.size = sizeof(request);
	request.handle = handle;
	(void)ioctl(descriptor, GPU_RESOURCE_DESTROY, &request);
}
