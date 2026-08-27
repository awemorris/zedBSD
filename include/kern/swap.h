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
#define SWAP_SOURCE_COUNT		4U
#define SWAP_SLOT_SOURCE_SHIFT		29U
#define SWAP_SLOT_LOCAL_MASK		UINT32_C(0x1fffffff)
#define SWAP_SLOT_SOURCE_MASK		UINT32_C(0x60000000)
#define SWAP_SLOT_VALID_MASK		UINT32_C(0x7fffffff)
#define SWAP_SOURCE_MAX_SLOTS		(UINT32_C(1) << SWAP_SLOT_SOURCE_SHIFT)
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

/*
 * A swap token is kernel-private VM state, not a backing-wide slot number.
 * Keeping the source ID in the token prevents removing or reusing another
 * source from changing the meaning of a live token.  Bit 31 is deliberately
 * clear for every valid token so UINT32_MAX remains the no-slot sentinel.
 */
_Static_assert(SWAP_SOURCE_COUNT == 4U,
    "the swap token reserves exactly two source bits");
_Static_assert((SWAP_SLOT_SOURCE_MASK | SWAP_SLOT_LOCAL_MASK) ==
    SWAP_SLOT_VALID_MASK, "swap token fields must cover bits 0 through 30");
_Static_assert((SWAP_SLOT_SOURCE_MASK & SWAP_SLOT_LOCAL_MASK) == 0,
    "swap token source and local fields must not overlap");
_Static_assert((SWAP_SLOT_NONE & ~SWAP_SLOT_VALID_MASK) != 0,
    "the no-slot sentinel must not be a valid swap token");

enum swap_source_state {
	SWAP_SOURCE_STATE_INACTIVE = 0,
	SWAP_SOURCE_STATE_PREPARED,
	SWAP_SOURCE_STATE_ACTIVE,
	SWAP_SOURCE_STATE_DRAINING,
	SWAP_SOURCE_STATE_REMOVING,
};

struct swap_source_stats {
	uint32_t source_id;
	uint32_t state;
	uint32_t total_slots;
	uint32_t free_slots;
	uint32_t allocated_slots;
	uint32_t inflight;
};

/* Public only because swap managers are statically embedded by the kernel. */
struct swap_backend_source {
	const struct swap_backend_ops *ops;
	void *data;
	uint32_t page_size;
	uint32_t slot_count;
	uint32_t free_slots;
	uint8_t *bitmap;
	uint32_t *slot_inflight;
	uint8_t *slot_pending_free;
	uint32_t inflight;
	uint32_t state;
};

struct swap_backend {
	struct swap_backend_source source[SWAP_SOURCE_COUNT];
	uint32_t slot_count;
	uint32_t free_slots;
	uint32_t source_count;
	unsigned enabled;
	unsigned shutting_down;
	unsigned inflight;
};

int
swap_slot_encode(
	unsigned source_id,
	uint32_t local_slot,
	uint32_t *slot);

int
swap_slot_decode(
	uint32_t slot,
	unsigned *source_id,
	uint32_t *local_slot);

void
swap_init(
	struct swap_backend *backend);

/*
 * Enable an empty manager.  An enabled manager can be selected as the system
 * backend before it owns any source, which reserves the singleton publication
 * point for later runtime additions.
 */
int
swap_manager_enable(
	struct swap_backend *backend);

int
swap_activate(
	struct swap_backend *backend,
	const struct swap_backend_ops *ops,
	void *data,
	uint32_t page_size,
	uint32_t slot_count);

/*
 * Allocate manager metadata without publishing capacity or transferring data
 * ownership.  PREPARED sources cannot allocate or perform I/O.  Publish is the
 * ownership-transfer point; cancel only releases manager metadata.
 */
int
swap_source_prepare(
	struct swap_backend *backend,
	unsigned source_id,
	const struct swap_backend_ops *ops,
	void *data,
	uint32_t page_size,
	uint32_t slot_count);

int
swap_source_publish(
	struct swap_backend *backend,
	unsigned source_id);

int
swap_source_cancel_prepare(
	struct swap_backend *backend,
	unsigned source_id);

/*
 * Add one explicitly numbered source.  On success the manager owns data and
 * eventually passes it to ops->destroy; on failure ownership stays with the
 * caller.  Allocation always visits active source IDs in numeric order.
 */
int
swap_source_add(
	struct swap_backend *backend,
	unsigned source_id,
	const struct swap_backend_ops *ops,
	void *data,
	uint32_t page_size,
	uint32_t slot_count);

/* Stop new allocation without preventing page-in from existing tokens. */
int
swap_source_begin_drain(
	struct swap_backend *backend,
	unsigned source_id);

/* Return a retained, failed-to-drain source to the allocation pool. */
int
swap_source_abort_drain(
	struct swap_backend *backend,
	unsigned source_id);

/*
 * Flush and destroy a draining source only when allocated and in-flight
 * counts are both zero.  A flush failure retains it in DRAINING state.
 */
int
swap_source_remove(
	struct swap_backend *backend,
	unsigned source_id);

int
swap_source_get_stats(
	struct swap_backend *backend,
	unsigned source_id,
	struct swap_source_stats *stats);

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
