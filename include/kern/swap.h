/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Swap
 */

#ifndef ZEDBSD_KERN_SWAP_H
#define ZEDBSD_KERN_SWAP_H

#include <stddef.h>
#include <stdint.h>

#define SWAP_SLOT_NONE			UINT32_MAX
#define SWAP_PAGE_SIZE			4096U
#define ZEDBSD_SWAP_FILE_MIN_BYTES	(32U * 1024U * 1024U)
#define ZEDBSD_SWAP_FILE_MAX_BYTES	(64U * 1024U * 1024U)
#define ZEDBSD_SWAP_HEADER_SIZE		64U
#define ZEDBSD_SWAP_V2_UUID_SIZE		8U
#define ZEDBSD_SWAP_V2_LABEL_SIZE	20U

struct swap_header_info {
	uint32_t version;
	uint64_t backing_bytes;
	uint64_t slot_count;
	uint8_t uuid[ZEDBSD_SWAP_V2_UUID_SIZE];
	char label[ZEDBSD_SWAP_V2_LABEL_SIZE];
};

struct swap_backend_ops {
	int (
		*read_page)(
		void *,
		uint32_t,
		void *);
	int (
		*write_page)(
		void *,
		uint32_t,
		const void *);
	int (
		*flush)(
		void *);
	void (
		*destroy)(
		void *);
};

struct swap_backend {
	const struct swap_backend_ops *ops;
	void *data;
	uint32_t page_size;
	uint32_t slot_count;
	uint32_t free_slots;
	uint8_t *bitmap;
	uint32_t *slot_inflight;
	uint8_t *slot_pending_free;
	unsigned enabled;
	unsigned shutting_down;
	unsigned inflight;
};

void
swap_init(
	struct swap_backend *backend);

int
swap_activate(
	struct swap_backend *backend,
	const struct swap_backend_ops *ops,
	void *data,
	uint32_t page_size,
	uint32_t slot_count);

int
swap_alloc_slot(
	struct swap_backend *backend,
	uint32_t *slot);

void
swap_free_slot(
	struct swap_backend *backend,
	uint32_t slot);

int
swap_read_page(
	struct swap_backend *backend,
	uint32_t slot,
	void *page);

int
swap_write_page(
	struct swap_backend *backend,
	uint32_t slot,
	const void *page);

int
swap_flush(
	struct swap_backend *backend);

int
swap_shutdown(
	struct swap_backend *backend);


int
swap_set_system_backend(
	struct swap_backend *backend);

struct swap_backend *
swap_system_backend(void);

int
swap_get_stats(
	struct swap_backend *backend,
	uint32_t *total,
	uint32_t *free_slots);

uint32_t
swap_header_checksum(
	const uint8_t *header);

int
swap_header_validate(
	const uint8_t *header,
	uint64_t backing_bytes);

int
swap_header_parse(
	const uint8_t *header,
	uint64_t backing_bytes,
	struct swap_header_info *result);

int
swap_header_uuid_format(
	const struct swap_header_info *header,
	char *output,
	size_t capacity);

#endif
