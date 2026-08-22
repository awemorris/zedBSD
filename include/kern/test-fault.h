/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#ifndef ZEDBSD_KERN_TEST_FAULT_H
#define ZEDBSD_KERN_TEST_FAULT_H

#include <stddef.h>
#include <stdint.h>

enum kern_test_fault_id {
	KERN_TEST_FAULT_NONE = 0,
	KERN_TEST_FAULT_VM_PAGE_ALLOC,
	KERN_TEST_FAULT_VFS_COPYUP_WRITE,
	KERN_TEST_FAULT_UFS_CG_WRITE,
	KERN_TEST_FAULT_NET_PACKET_ALLOC,
	KERN_TEST_FAULT_UNIX_STREAM_ALLOC,
	KERN_TEST_FAULT_COUNT
};

struct kern_test_fault_result {
	int error;
	size_t short_count;
};

#ifdef ZEDBSD_TEST_FAULTS
#define KERN_TEST_FAULT_LOG_CAPACITY 128U
#define KERN_TEST_FAULT_ANY_CONTEXT UINT32_MAX

struct kern_test_fault_config {
	enum kern_test_fault_id id;
	uint64_t fail_at;
	uint32_t cpu;
	uint32_t tid;
	int error;
	size_t short_count;
};

struct kern_test_fault_log_entry {
	uint64_t sequence;
	uint64_t ordinal;
	uint32_t id;
	uint32_t cpu;
	uint32_t tid;
	int32_t error;
	uint64_t short_count;
};

void kern_test_fault_reset(void);
int kern_test_fault_configure(const struct kern_test_fault_config *);
int kern_test_fault_hit(enum kern_test_fault_id, uint32_t, uint32_t,
	struct kern_test_fault_result *);
size_t kern_test_fault_log(struct kern_test_fault_log_entry *, size_t);
#define KERN_TEST_FAULT(id, cpu, tid, result) \
	kern_test_fault_hit((id), (cpu), (tid), (result))
#else
#define KERN_TEST_FAULT(id, cpu, tid, result) \
	((void)(id), (void)(cpu), (void)(tid), (void)(result), 0)
#endif

#endif
