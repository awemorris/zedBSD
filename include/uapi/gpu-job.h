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
#define GPU_CAP_JOB_SUBMIT	262144U
#define GPU_CAP_JOB_RETIRE	524288U
#define GPU_JOB_CANCEL_FAULT	1U
#define GPU_JOB_CAPACITY_QUERY	1U

#define GPU_JOB_RESERVE		_IOWR('G', 34, struct gpu_job_reserve)
#define GPU_JOB_COMMIT		_IOW('G', 35, struct gpu_job_action)
#define GPU_JOB_CANCEL		_IOW('G', 36, struct gpu_job_action)
#define GPU_JOB_CAPACITY		_IOWR('G', 37, struct gpu_job_capacity)
#define GPU_JOB_POLICY		_IOWR('G', 38, struct gpu_job_policy)
#define GPU_JOB_SUBMIT		_IOWR('G', 49, struct gpu_job_submit)
#define GPU_JOB_RETIRE		_IOWR('G', 50, struct gpu_job_retire)

/* GPU_JOB_SUBMIT's outcomes. */
#define GPU_JOB_SUBMIT_COMMITTED	1U
#define GPU_JOB_SUBMIT_CANCELED		2U
#define GPU_JOB_SUBMIT_FAULTED		3U
#define GPU_JOB_SUBMIT_DEFERRED		4U

/* GPU_JOB_RETIRE's most entries and each entry's state. */
#define GPU_JOB_RETIRE_MAX		GPU_SUBMIT_MAX
#define GPU_JOB_RETIRE_PENDING		0U
#define GPU_JOB_RETIRE_RETIRED		1U
#define GPU_JOB_RETIRE_MISSING		2U

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

/*
 * One reservation, its native command stream run at once on the caller's
 * thread, and the commit or cancel its outcome decides, in one call.  fd,
 * generation and timeline are as gpu_job_reserve's; address and bytes as
 * gpu_command's.  flags, sequence, outcome and error begin at zero.
 *
 * An errno (EINVAL, EACCES, EOPNOTSUPP, EFAULT for the input, ENOMEM,
 * EAGAIN when no record, fence slot or backend marker is free, EBUSY when
 * the payload is bound elsewhere, ENODEV or the open's error): nothing
 * native ran and no record or binding is left.  Return 0: outcome says
 * what became of the job:
 *
 *  - COMMITTED: the stream ran and every queue operation in it was
 *    accepted; sequence names the job, committed as after GPU_JOB_COMMIT.
 *  - CANCELED: no work of the stream reached the GPU (it ran no queue
 *    operation, or it failed before its first run, error saying why); the
 *    job is gone as after GPU_JOB_CANCEL and the payload stays unsignaled.
 *  - FAULTED: work of the stream may have reached the GPU but not all of
 *    it was accepted, or the stream failed after that; the job ends with
 *    an error and the open is lost, as after GPU_JOB_CANCEL with
 *    GPU_JOB_CANCEL_FAULT.
 *  - DEFERRED: the backend could not run the stream now; nothing native
 *    ran and the job is gone (the caller uses GPU_JOB_RESERVE, GPU_COMMAND
 *    and GPU_JOB_COMMIT instead).
 *
 * The caller never sends GPU_JOB_COMMIT or GPU_JOB_CANCEL for it.  EFAULT
 * after the outcome was decided leaves the job as the outcome says; the
 * caller then treats its context as lost.
 */
struct gpu_job_submit {
	uint32_t version;
	uint32_t size;
	int32_t fd;
	uint32_t flags;
	uint64_t generation;
	uint32_t timeline;
	uint32_t bytes;
	uint64_t address;
	uint64_t sequence;
	uint32_t outcome;
	int32_t error;
};

/*
 * One named sequence for GPU_JOB_RETIRE: in, the sequence; out, its state
 * and, when RETIRED, its status as GPU_COMMAND_WAIT's.
 */
struct gpu_job_retire_entry {
	uint64_t sequence;
	uint32_t state;
	uint32_t status;
};

/*
 * Consumes the named job records that have ended, as a GPU_COMMAND_WAIT of
 * no time with consumption would each: up to GPU_JOB_RETIRE_MAX entries at
 * address, each written back as RETIRED (consumed), PENDING (not ended,
 * observed or consumed by another caller, or named twice) or MISSING (no
 * job record of that sequence).  retired is how many were consumed.
 * flags, retired and reserved begin at zero, as do each entry's outputs.
 */
struct gpu_job_retire {
	uint32_t version;
	uint32_t size;
	uint32_t count;
	uint32_t flags;
	uint64_t address;
	uint32_t retired;
	uint32_t reserved;
};

#endif
