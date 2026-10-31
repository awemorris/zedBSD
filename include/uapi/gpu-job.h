/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Producer-owned GPU work reservations with autonomous completion supervision.
 */

#ifndef KERN_UAPI_GPU_JOB_H
#define KERN_UAPI_GPU_JOB_H

#include <uapi/gpu.h>

#define GPU_CAP_JOB		8192U
#define GPU_CAP_JOB_CAPACITY	16384U
#define GPU_JOB_CANCEL_FAULT	1U
#define GPU_JOB_CAPACITY_QUERY	1U

#define GPU_JOB_RESERVE		_IOWR('G', 34, struct gpu_job_reserve)
#define GPU_JOB_COMMIT		_IOW('G', 35, struct gpu_job_action)
#define GPU_JOB_CANCEL		_IOW('G', 36, struct gpu_job_action)
#define GPU_JOB_CAPACITY		_IOWR('G', 37, struct gpu_job_capacity)
#define GPU_JOB_POLICY		_IOWR('G', 38, struct gpu_job_policy)

/*
 * One reservation owns completion storage and backend submission capacity before
 * native submission. The backend validates the queue completion domain.
 * Optional fd/generation use -1/zero when only the command record is observed.
 * Flags, reserved and the output sequence begin at zero. GPU_COMMAND_WAIT owns
 * terminal observation and consumption of successfully committed sequences.
 * Reserved generations have a producer deadline but are not GPU dependencies.
 * The sequence is copied out before the driver reserves, so a refused
 * reservation leaves a number in it that names no record (ENOENT to a wait).
 */
struct gpu_job_reserve {
	uint32_t version;
	uint32_t size;
	int32_t fd;
	uint32_t flags;
	uint64_t generation;
	uint32_t timeline;
	uint32_t reserved;
	uint64_t sequence;
};

/*
 * One exact open description commits or cancels its retained reservation.
 * COMMIT follows native success and publishes using already reserved storage.
 * CANCEL with flags zero is valid only after definite native nonacceptance; it
 * discards the record and leaves the optional fence unsignaled and unbound.
 * CANCEL_FAULT instead reports uncertain native work or device loss, retaining
 * terminal ERROR for observation and uncertain DMA until checked recovery.
 * COMMIT accepts flags zero only. The reserved field always begins at zero.
 */
struct gpu_job_action {
	uint32_t version;
	uint32_t size;
	uint64_t sequence;
	uint32_t flags;
	uint32_t reserved;
};

/*
 * One capacity observation precedes userspace reclamation and a nonblocking
 * reservation attempt. QUERY returns the sequence immediately; WAIT returns
 * when that sequence changes or both backend and open-record capacity exist.
 * Existing terminal records alone do not make repeated waits readable.
 * QUERY inputs other than the header, flag and domain are zero. WAIT supplies
 * an observed nonzero sequence and a relative timeout, with output fields zero.
 * Zero timeout observes, UINT64_MAX waits interruptibly, and neither mode owns
 * a job or guarantees that another caller cannot win the subsequent reservation.
 */
struct gpu_job_capacity {
	uint32_t version;
	uint32_t size;
	uint32_t flags;
	uint32_t domain;
	uint64_t timeout_ns;
	uint64_t observed_sequence;
	uint64_t sequence;
	uint32_t available;
	uint32_t reserved;
};

/*
 * One immutable snapshot of administrator-selected supervision intervals.
 * Inputs other than version and size are zero; clients cannot extend deadlines.
 * Admission waiting is independent from these accepted-work timeout intervals.
 */
struct gpu_job_policy {
	uint32_t version;
	uint32_t size;
	uint64_t reservation_timeout_ns;
	uint64_t execution_timeout_ns;
	uint64_t stop_timeout_ns;
	uint32_t flags;
	uint32_t reserved;
};

#endif
