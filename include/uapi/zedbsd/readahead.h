/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#ifndef ZEDBSD_UAPI_READAHEAD_H
#define ZEDBSD_UAPI_READAHEAD_H
#include <stdint.h>
#define READAHEAD_REPORT_VERSION 1U

/* Cumulative observations, not a durability certificate or a reset interface.
 * Confirmed useful is exact for forward consumption, a lower bound for arbitrary
 * within-page revisits. Retired uncredited is an upper bound on unused bytes.
 * Discarded fills include canceled work which never reached the backend. */
struct readahead_report {
	uint64_t requested_bytes;
	uint64_t started_bytes;
	uint64_t published_bytes;
	uint64_t confirmed_useful_bytes;
	uint64_t retired_uncredited_bytes;
	uint64_t discarded_fill_bytes;
	uint64_t errors;
	uint64_t queue_refusals;
	uint64_t memory_bytes;
	uint32_t version;
	uint32_t jobs;
	uint32_t running;
	uint32_t demand;
};
#endif
