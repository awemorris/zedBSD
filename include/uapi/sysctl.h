/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

#ifndef KERN_UAPI_SYSCTL_H
#define KERN_UAPI_SYSCTL_H

#include <stdint.h>

#define CTL_MAXNAME	8U

#define CTL_SYSCTL	0
#define CTL_HW	1
#define CTL_KERN	2
#define CTL_VFS	3

#define CTL_SYSCTL_NAME2OID	1
#define CTL_SYSCTL_NEXT	2
#define CTL_SYSCTL_OIDNAME	3

#define VFS_BUFCACHE	1
#define VFS_IO	2
#define VFS_CACHE_MEMORY	3
#define VFS_WRITEBACK	4
#define VFS_READAHEAD	5
#define VFS_READAHEAD_STATS	1
#define VFS_WRITEBACK_STATS	1
#define VFS_WRITEBACK_CONTROL	2
#define VFS_CACHE_MEMORY_STATS	1
#define VFS_CACHE_MEMORY_TARGET	2
#define VFS_IO_STATS	1
#define VFS_BUFCACHE_MAX_BYTES	1
#define VFS_BUFCACHE_CURRENT_BYTES	2
#define VFS_BUFCACHE_DIRTY_BYTES	3
#define VFS_BUFCACHE_STATS	4

#define HW_NCPU	1
#define HW_NCPUONLINE	2
#define HW_MEMORY_STATS	3

/* Firmware RAM and actually managed RAM are distinct. */
#define MEMORY_STATS_VERSION 2U
struct memory_stats {
	uint32_t version;
	uint32_t boot_ranges_valid;
	uint64_t boot_range_count;
	uint64_t boot_usable_bytes;
	uint64_t boot_highest_end;
	uint64_t boot_usable_highest_end;
	uint64_t direct_mapped_bytes;
	uint64_t allocator_initial_bytes;
	uint64_t physical_managed_bytes;
	uint64_t physical_reserved_bytes;
	uint64_t physical_allocated_bytes;
	uint64_t physical_free_bytes;
	uint64_t boot_reclaim_bytes;
	uint64_t allocator_metadata_bytes;
	uint64_t allocator_scan_words;
	uint64_t allocator_max_extent_scan_words;
	uint64_t allocator_max_irqoff_cycles;
	uint32_t boot_memory_source;
	uint32_t reserved;
};

#define KERN_MSGBUF	1
#define KERN_MSGBUF_SIZE	2
#define KERN_MSGBUF_DROPPED	3
#define KERN_HOSTNAME	4
#define KERN_BOOT_FIRMWARE 5
#define KERN_BOOT_CONFIGURATION 6
#define KERN_BOOT_CONFIG_MATCHES 7
#define KERN_BOOT_ROOT_IMAGE 8
#define ROOT_IMAGE_VERSION 1U
#define ROOT_IMAGE_OVERLAY 1U
#define ROOT_IMAGE_MOUNTED 2U
#define ROOT_IMAGE_READ_ONLY 4U
#define ROOT_IMAGE_LOOP_READ_ONLY 8U

/* One live root/lower/loop observation; zero flags denotes a native root. */
struct root_image_info {
	uint32_t version;
	uint32_t flags;
	uint64_t loop_device;
	uint64_t backing_device;
	uint64_t backing_inode;
	uint64_t backing_bytes;
};
#define KERN_HOST_NAME_MAX	64U

struct bufcache_stats {
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
	uint64_t capacity_failures;
	uint64_t physical_failures;
};

#endif
