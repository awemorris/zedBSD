/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#ifndef ZEDBSD_UAPI_SYSCTL_H
#define ZEDBSD_UAPI_SYSCTL_H

#include <stdint.h>

#define CTL_MAXNAME 8U

#define CTL_SYSCTL 0
#define CTL_HW     1
#define CTL_KERN   2
#define CTL_VFS    3

#define CTL_SYSCTL_NAME2OID 1
#define CTL_SYSCTL_NEXT     2
#define CTL_SYSCTL_OIDNAME  3

#define VFS_BUFCACHE 1
#define VFS_BUFCACHE_MAX_BYTES     1
#define VFS_BUFCACHE_CURRENT_BYTES 2
#define VFS_BUFCACHE_DIRTY_BYTES   3
#define VFS_BUFCACHE_STATS         4

#define HW_NCPU        1
#define HW_NCPUONLINE  2

#define KERN_MSGBUF         1
#define KERN_MSGBUF_SIZE    2
#define KERN_MSGBUF_DROPPED 3
#define KERN_HOSTNAME       4
#define ZEDBSD_HOST_NAME_MAX 64U

struct zedbsd_bufcache_stats {
	uint64_t max_bytes;
	uint64_t current_bytes;
	uint64_t data_bytes;
	uint64_t metadata_bytes;
	uint64_t dirty_bytes;
	uint64_t buffers;
	uint64_t hits;
	uint64_t misses;
	uint64_t read_bios;
	uint64_t write_bios;
	uint64_t evictions;
	uint64_t waits;
	uint64_t writeback_errors;
};

#endif
