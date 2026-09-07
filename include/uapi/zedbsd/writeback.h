/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#ifndef ZEDBSD_UAPI_WRITEBACK_H
#define ZEDBSD_UAPI_WRITEBACK_H
#include <stdint.h>

#define WRITEBACK_REPORT_VERSION 1U
#define WRITEBACK_REPORT_MOUNTS 64U
#define WRITEBACK_PATH_MAX 256U
#define WRITEBACK_DEVICE_NAME_MAX 32U
#define WRITEBACK_STATE_LIVE 1U
#define WRITEBACK_STATE_PAUSED 2U

struct writeback_control {
	uint32_t version;
	uint32_t enabled;
	char path[WRITEBACK_PATH_MAX];
};
struct writeback_report_header {
	uint64_t high;
	uint64_t low;
	uint64_t device_high;
	uint64_t dirty;
	uint64_t reserved;
	uint64_t tickets;
	uint64_t refusals;
	uint64_t memory_bytes;
	uint64_t passes;
	uint64_t errors;
	uint32_t version;
	uint32_t count;
	uint32_t workers;
	uint32_t busy;
	int32_t last_error;
	uint32_t padding;
};
struct writeback_mount_info {
	char path[WRITEBACK_PATH_MAX];
	char device[WRITEBACK_DEVICE_NAME_MAX];
	/* These counters are shared by every mount on the named physical device. */
	uint64_t device_dirty;
	uint64_t device_reserved;
	uint64_t device_tickets;
	uint32_t state;
	uint32_t padding;
};
struct writeback_report {
	struct writeback_report_header header;
	struct writeback_mount_info mounts[WRITEBACK_REPORT_MOUNTS];
};
#endif
