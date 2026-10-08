/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The host test of context.c's reading of the native word in the
 * renderer's capability record (ws083 R-S6): vulkan_context_open runs
 * against a stand-in GPU node (open and ioctl are this file's) that
 * answers GPU_GET_INFO and GPU_GET_CAPSET with a record built here.
 * Checked: a 176-byte record with the native tag and the video bit
 * promises H.264 decode (other bits ignored); without the bit, with
 * another tag, or a 168-byte record (Venus's fork), there is no promise;
 * the vendor flags of the 168 bytes are read as before in both lengths.
 */

#include "internal.h"
#include <uapi/gpu.h>
#include <uapi/gpu-job.h>

#include <assert.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* The descriptor the stand-in node gives. */
#define CAPSET_FD		97

/* The record's tags and flags, as the renderer writes them. */
#define CAPSET_VENDOR_MAGIC	0x5a424453U
#define CAPSET_NATIVE_MAGIC	0x5a4e4154U
#define CAPSET_VENDOR_FLAGS	0xfU

/* What the next GPU_GET_CAPSET answers: the record's length, the native tag and the native flags. */
static uint32_t record_bytes;
static uint32_t record_native_magic;
static uint32_t record_native_flags;

int open(const char *path, int flags, ...);
int ioctl(int fd, unsigned long request, ...);
static void put_word(uint8_t *data, size_t at, uint32_t value);
static VkBool32 promise(uint32_t bytes, uint32_t magic, uint32_t flags, VkBool32 *strict);

/* The stand-in GPU node: any path opens it. */
int
open(
	const char *path,
	int flags,
	...)
{
	/* The one node. */
	(void)path;
	(void)flags;
	return CAPSET_FD;
}

/* The stand-in node's two questions: its limits and its capability record. */
int
ioctl(
	int fd,
	unsigned long request,
	...)
{
	struct gpu_capset *capset;
	struct gpu_info *information;
	va_list arguments;
	void *argument;

	/* Only the node's descriptor. */
	va_start(arguments, request);
	argument = va_arg(arguments, void *);
	va_end(arguments);
	if (fd != CAPSET_FD) {
		errno = EBADF;
		return -1;
	}

	/* The limits: every capability the transport and the devices need. */
	if (request == GPU_GET_INFO) {
		information = argument;
		information->capabilities = GPU_CAP_CAPSET | GPU_CAP_BLOB | GPU_CAP_TRANSFER | GPU_CAP_COMMAND | GPU_CAP_MAPPING | GPU_CAP_JOB | GPU_CAP_JOB_CAPACITY;
		information->max_resource_bytes = 1U << 30;
		return 0;
	}

	/* The capability record: the protocol, a timeline, the vendor suffix and the native word. */
	if (request == GPU_GET_CAPSET) {
		capset = argument;
		memset(capset->data, 0, sizeof(capset->data));
		put_word(capset->data, 0, 1U);
		put_word(capset->data, 4, VK_MAKE_VERSION(1, 3, 269));
		put_word(capset->data, 152, 64U);
		put_word(capset->data, 160, CAPSET_VENDOR_MAGIC);
		put_word(capset->data, 164, CAPSET_VENDOR_FLAGS);
		put_word(capset->data, 168, record_native_magic);
		put_word(capset->data, 172, record_native_flags);
		capset->bytes = record_bytes;
		return 0;
	}

	/* No other question. */
	errno = ENOTTY;
	return -1;
}

/* Runs every case and reports the result on one line. */
int
main(void)
{
	VkBool32 strict;

	/* The native record with the video bit, and with another bit beside it. */
	assert(promise(176U, CAPSET_NATIVE_MAGIC, 1U, &strict) == VK_TRUE);
	assert(strict == VK_TRUE);
	assert(promise(176U, CAPSET_NATIVE_MAGIC, 0x21U, &strict) == VK_TRUE);

	/* The native record without the bit, or with another tag. */
	assert(promise(176U, CAPSET_NATIVE_MAGIC, 0U, &strict) == VK_FALSE);
	assert(promise(176U, 0x12345678U, 1U, &strict) == VK_FALSE);

	/* Venus's 168 bytes: no native word, the vendor flags as before. */
	assert(promise(168U, CAPSET_NATIVE_MAGIC, 1U, &strict) == VK_FALSE);
	assert(strict == VK_TRUE);

	/* Every case passed. */
	printf("ws083 libvulkan capset host test PASS\n");
	return 0;
}

/* Stores a little-endian word into the record. */
static void
put_word(
	uint8_t *data,
	size_t at,
	uint32_t value)
{
	/* The low byte first. */
	data[at] = (uint8_t)value;
	data[at + 1] = (uint8_t)(value >> 8);
	data[at + 2] = (uint8_t)(value >> 16);
	data[at + 3] = (uint8_t)(value >> 24);
}

/* Opens a session over a record of a length, a native tag and flags; reports the video promise and the strict queue. */
static VkBool32
promise(
	uint32_t bytes,
	uint32_t magic,
	uint32_t flags,
	VkBool32 *strict)
{
	struct vulkan_context context;
	VkBool32 video;
	VkResult status;

	/* The record, then the session over it. */
	record_bytes = bytes;
	record_native_magic = magic;
	record_native_flags = flags;
	status = vulkan_context_open(&context, "/dev/gpu0");
	assert(status == VK_SUCCESS);
	video = context.video_h264;
	*strict = context.strict_queue;

	/* The session goes (its descriptor is the stand-in's; closing it fails harmlessly). */
	(void)vulkan_context_close(&context);
	return video;
}
