/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The GSP firmware files of the nvrtx driver.
 *
 * The files live below /lib/firmware/nvidia/ (the nvidia-gsp-firmware
 * package), in the chip directories the chip's profile names.  The small
 * ones are read whole into kernel memory.  The GSP firmware itself is
 * 28.5 MB (Turing) or 63.6 MB (Ampere and Ada): only its ELF header and section table are read
 * into kernel memory, and its image section is read straight into the DMA
 * chunks the GSP will read, so no buffer of the file's size is ever made.
 * Every read waits for the root file system, so it runs on the worker thread.
 */

#include <stddef.h>
#include <stdint.h>

#include <kern/clock.h>
#include <kern/file.h>
#include <kern/kcrt.h>
#include <kern/kmem.h>
#include <kern/mount.h>
#include <kern/namei.h>
#include <kern/sched.h>
#include <uapi/errno.h>
#include <uapi/fcntl.h>

#include "drivers/gpu/nvrtx/nvrtx-gpu.h"

/* The longest path a request builds, with its terminating zero. */
#define FILE_PATH_BYTES			160U

/* How long a read waits for the root file system (60 s), and each step of the wait (100 ms). */
#define FILE_ROOT_WAIT_TICKS		kern_ms_to_ticks(60000U)
#define FILE_ROOT_POLL_TICKS		kern_ms_to_ticks(100U)

/* The most bytes of a section table, or of the names section, read into kernel memory. */
#define FILE_SECTION_TABLE_MAX		0x10000U

/*
 * One firmware file open for reading.
 *
 * The content lease fixes the size the reads see; the file stays open until
 * the close.
 */
struct firmware_file {
	struct file *file;
	struct file_content_lease lease;
	size_t size;
};

static int firmware_open(const char *name, struct firmware_file *open);
static void firmware_close(struct firmware_file *open);
static int firmware_read(struct firmware_file *open, size_t offset, uint8_t *buffer, size_t length);
static int firmware_read_sections(struct nvrtx_gpu *gpu, struct firmware_file *open);
static int firmware_read_image(struct nvrtx_gpu *gpu, struct firmware_file *open, const struct nvrtx_section *image);
static struct mount *firmware_root_wait(void);

/*
 * Reads a small firmware file whole into kernel memory.
 *
 * Returns ENOENT when the file is absent (the firmware package is not
 * installed), EFBIG when it is larger than the limit.
 */
int
nvrtx_file_load(
	const char *name,
	size_t limit,
	struct nvrtx_blob *blob)
{
	struct firmware_file open;
	uint8_t *data;
	int error;

	/* Starts with nothing, so a failure hands back no bytes. */
	blob->data = NULL;
	blob->size = 0U;

	/* Opens the file. */
	error = firmware_open(name, &open);
	if (error != 0)
		return error;

	/* Refuses an empty file and one past the limit. */
	if (open.size == 0U || open.size > limit) {
		firmware_close(&open);
		return EFBIG;
	}

	/* Allocates the buffer the file is handed back in. */
	data = kern_malloc(open.size);
	if (data == NULL) {
		firmware_close(&open);
		return ENOMEM;
	}

	/* Reads the whole file, then closes it. */
	error = firmware_read(&open, 0U, data, open.size);
	firmware_close(&open);
	if (error != 0) {
		kern_free(data);
		return error;
	}

	/* Succeeded: the caller owns the bytes until it unloads them. */
	blob->data = data;
	blob->size = open.size;
	return 0;
}

/*
 * Frees a file read whole.
 */
void
nvrtx_file_unload(
	struct nvrtx_blob *blob)
{
	/* A blob that never loaded holds nothing. */
	if (blob->data != NULL)
		kern_free(blob->data);

	/* Forgets the bytes. */
	blob->data = NULL;
	blob->size = 0U;
}

/*
 * Reads the GSP firmware's image and signature sections into DMA memory.
 *
 * Fills the image chunks (1 MiB each, the last one partly used) and the
 * signature page of the boot memory.  Returns ENOENT when a section is
 * missing, ENOMEM when the DMA memory cannot be had.  On a failure the
 * caller frees what was allocated (nvrtx_gsp_release).
 */
int
nvrtx_file_load_image(
	struct nvrtx_gpu *gpu,
	const char *name)
{
	struct firmware_file open;
	int error;

	/* Opens the file. */
	error = firmware_open(name, &open);
	if (error != 0)
		return error;

	/* Reads the two sections. */
	error = firmware_read_sections(gpu, &open);

	/* Closes the file whatever the reads reported. */
	firmware_close(&open);

	/* Reports a failed read. */
	if (error != 0)
		return error;

	/* Succeeded: the image and its signature are in DMA memory. */
	return 0;
}

/* Finds the two sections of the open GSP firmware and reads them into DMA memory. */
static int
firmware_read_sections(
	struct nvrtx_gpu *gpu,
	struct firmware_file *open)
{
	struct nvrtx_boot_memory *memory;
	struct nvrtx_elf elf;
	struct nvrtx_section names;
	struct nvrtx_section image;
	struct nvrtx_section signature;
	uint8_t header[NVRTX_ELF_HEADER_BYTES];
	uint8_t *table;
	uint8_t *name_bytes;
	int error;

	memory = &gpu->memory;

	/* Reads the ELF header and checks the section table's place. */
	error = firmware_read(open, 0U, header, sizeof(header));
	if (error != 0)
		return error;

	error = nvrtx_elf_header(header, open->size, &elf);
	if (error != 0)
		return error;

	/* A section table larger than any this firmware has is refused. */
	if (elf.table_bytes > FILE_SECTION_TABLE_MAX)
		return EINVAL;

	/* Reads the section table. */
	table = kern_malloc(elf.table_bytes);
	if (table == NULL)
		return ENOMEM;

	error = firmware_read(open, (size_t)elf.table_at, table, elf.table_bytes);
	if (error != 0) {
		kern_free(table);
		return error;
	}

	/* Finds the names section, which must be small. */
	error = nvrtx_elf_entry(&elf, table, elf.names_index, &names);
	if (error != 0) {
		kern_free(table);
		return error;
	}

	if (names.size > FILE_SECTION_TABLE_MAX) {
		kern_free(table);
		return EINVAL;
	}

	/* Reads the names section. */
	name_bytes = kern_malloc(names.size);
	if (name_bytes == NULL) {
		kern_free(table);
		return ENOMEM;
	}

	error = firmware_read(open, names.at, name_bytes, names.size);
	if (error != 0)
		goto out;

	/* Finds the image and its Turing signature. */
	error = nvrtx_elf_find(&elf, table, name_bytes, names.size, NVRTX_GSP_IMAGE_SECTION, &image);
	if (error != 0)
		goto out;

	error = nvrtx_elf_find(&elf, table, name_bytes, names.size, gpu->identity.chip.profile->signature_section, &signature);

out:
	/* Gives the table and the names back whatever the lookup found. */
	kern_free(name_bytes);
	kern_free(table);
	if (error != 0)
		return error;

	/* The signature fits one page; the image fits the three-level table. */
	if (signature.size == 0U || signature.size > NVRTX_GSP_PAGE_SIZE)
		return EINVAL;
	if (image.size == 0U || image.size > NVRTX_IMAGE_MAX_BYTES)
		return EINVAL;

	/* Reads the signature into its own page. */
	error = nvrtx_buffer_alloc(gpu, NVRTX_GSP_PAGE_SIZE, &memory->signature);
	if (error != 0)
		return error;

	error = firmware_read(open, signature.at, (uint8_t *)memory->signature.dma.address, signature.size);
	if (error != 0)
		return error;

	memory->signature_size = signature.size;

	/* Reads the image into its chunks. */
	error = firmware_read_image(gpu, open, &image);
	if (error != 0)
		return error;

	/* Succeeded: both sections are in DMA memory. */
	return 0;
}

/* Allocates the image's chunks and reads the image section into them. */
static int
firmware_read_image(
	struct nvrtx_gpu *gpu,
	struct firmware_file *open,
	const struct nvrtx_section *image)
{
	struct nvrtx_boot_memory *memory;
	size_t chunk_count;
	size_t chunk;
	size_t offset;
	size_t length;
	int error;

	memory = &gpu->memory;

	/* Counts the chunks and allocates their bookkeeping. */
	chunk_count = (image->size + NVRTX_IMAGE_CHUNK_BYTES - 1U) / NVRTX_IMAGE_CHUNK_BYTES;
	memory->image_chunks = kern_calloc(chunk_count, sizeof(*memory->image_chunks));
	memory->image.chunk_cpu = kern_calloc(chunk_count, sizeof(*memory->image.chunk_cpu));
	memory->image.chunk_bus = kern_calloc(chunk_count, sizeof(*memory->image.chunk_bus));
	if (memory->image_chunks == NULL ||
	    memory->image.chunk_cpu == NULL ||
	    memory->image.chunk_bus == NULL)
		return ENOMEM;

	memory->image.chunk_count = chunk_count;
	memory->image.chunk_bytes = NVRTX_IMAGE_CHUNK_BYTES;
	memory->image.page_count = (image->size + NVRTX_GSP_PAGE_SIZE - 1U) / NVRTX_GSP_PAGE_SIZE;
	memory->image_size = image->size;

	/* Allocates each chunk and reads its part of the image; the tail of the last stays zero. */
	for (chunk = 0U; chunk < chunk_count; chunk++) {
		/* Allocates the chunk. */
		error = nvrtx_buffer_alloc(gpu, NVRTX_IMAGE_CHUNK_BYTES, &memory->image_chunks[chunk]);
		if (error != 0)
			return error;

		memory->image.chunk_cpu[chunk] = (uint8_t *)memory->image_chunks[chunk].dma.address;
		memory->image.chunk_bus[chunk] = memory->image_chunks[chunk].dma.device_address;

		/* Reads this chunk's part. */
		offset = chunk * NVRTX_IMAGE_CHUNK_BYTES;
		length = image->size - offset;
		if (length > NVRTX_IMAGE_CHUNK_BYTES)
			length = NVRTX_IMAGE_CHUNK_BYTES;
		error = firmware_read(open, image->at + offset, memory->image.chunk_cpu[chunk], length);
		if (error != 0)
			return error;
	}

	/* Succeeded: the image is in its chunks. */
	return 0;
}

/* Opens a firmware file below the firmware directory and takes its content lease. */
static int
firmware_open(
	const char *name,
	struct firmware_file *open)
{
	char path[FILE_PATH_BYTES];
	struct mount *root;
	struct cwdinfo context;
	struct path root_path;
	size_t directory_length;
	size_t name_length;
	int error;

	/* Joins the directory and the name, refusing a path that does not fit. */
	directory_length = kern_strlen(NVRTX_FIRMWARE_DIRECTORY);
	name_length = kern_strlen(name);
	if (name_length >= FILE_PATH_BYTES - directory_length)
		return ENAMETOOLONG;

	kern_memcpy(path, NVRTX_FIRMWARE_DIRECTORY, directory_length);
	kern_memcpy(path + directory_length, name, name_length + 1U);

	/* Waits for the root file system the file lives on. */
	root = firmware_root_wait();
	if (root == NULL)
		return ENOENT;

	/* Roots a working directory of the driver's own at the mount. */
	path_init(&root_path);
	path_set(&root_path, root, root->m_root);
	error = cwdinfo_init(&context, &root_path);
	path_release(&root_path);
	mount_release(root);
	if (error != 0)
		return error;

	/* Opens the file; an absent one is ENOENT. */
	open->file = NULL;
	error = file_openat(&context, path, O_RDONLY, 0, &open->file);
	cwdinfo_destroy(&context);
	if (error != 0)
		return error;

	/* Takes the lease that fixes the size the reads see. */
	kern_memset(&open->lease, 0, sizeof(open->lease));
	error = file_content_lease_begin(open->file, &open->lease);
	if (error != 0) {
		(void)file_close(open->file);
		return error;
	}

	/* A negative size is not a file to read. */
	if (open->lease.size < 0) {
		file_content_lease_end(&open->lease);
		(void)file_close(open->file);
		return EINVAL;
	}

	/* Succeeded: the file is open with its size fixed. */
	open->size = (size_t)open->lease.size;
	return 0;
}

/* Ends the lease and closes the file. */
static void
firmware_close(
	struct firmware_file *open)
{
	/* Ends the lease, then closes the file. */
	file_content_lease_end(&open->lease);
	(void)file_close(open->file);
	open->file = NULL;
}

/* Reads a range of an open file whole, refusing a short read. */
static int
firmware_read(
	struct firmware_file *open,
	size_t offset,
	uint8_t *buffer,
	size_t length)
{
	size_t done;
	ssize_t count;

	/* The range must lie inside the file. */
	if (offset > open->size || length > open->size - offset)
		return EINVAL;

	/* Reads until the whole range is in the buffer. */
	done = 0U;
	while (done < length) {
		/* Reads the next part. */
		count = file_content_lease_pread(&open->lease, buffer + done, length - done, (off_t)(offset + done));
		if (count < 0)
			return (int)-count;

		/* A read that returns nothing, or more than asked, is not the file the lease measured. */
		if (count == 0 || (size_t)count > length - done)
			return EIO;

		/* Moves past what was read. */
		done += (size_t)count;
	}

	/* Succeeded: the range is in the buffer. */
	return 0;
}

/* Takes a reference to the live root file system, waiting up to the bound; NULL when none came. */
static struct mount *
firmware_root_wait(void)
{
	struct mount *root;
	uint64_t deadline;
	uint64_t now;

	/* Sleeps in short steps until the root is live or the bound is spent. */
	deadline = sched_ticks() + FILE_ROOT_WAIT_TICKS;
	for (;;) {
		/* Stops as soon as the kernel has a live root. */
		root = mount_root_get_ref();
		if (root != NULL)
			break;

		/* The bound is spent without a root. */
		now = sched_ticks();
		if (now >= deadline)
			return NULL;

		/* Sleeps one step; the worker may sleep. */
		sched_sleep(now + FILE_ROOT_POLL_TICKS);
	}

	/* Succeeded: the caller holds a reference to the root. */
	return root;
}
