/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * /dev/system
 */

#ifndef ZEDBSD_UAPI_SYSTEM_H
#define ZEDBSD_UAPI_SYSTEM_H

#include <stdint.h>
#include <stddef.h>
#include <sys/ioctl.h>

#define ZEDBSD_SYSTEM_IOC_GROUP 's'
#define ZEDBSD_SYSTEM_SWAP_PAGE_SIZE 4096U

struct system_info {
	uint32_t boot_bios_id;
	uint32_t device_count;
	uint32_t partition_count;
	uint32_t reserved;
};

struct system_device_info {
	uint32_t index;
	uint32_t device_class;
	uint32_t flags;
	uint32_t bios_id;
	uint32_t display_index;
	uint32_t heads;
	uint32_t sectors;
	uint32_t reserved;
};

struct vm_statistics {
	uint64_t physical_total, physical_reserved, physical_allocated,
	    physical_free;
	uint64_t image, heap_fixed, heap_current, heap_peak;
	uint64_t heap_largest_free, heap_largest_failed;
	uint64_t hal_tasks, hal_task_stack_bytes, hal_spaces, hal_page_tables;
	uint64_t vm_resident, vm_anonymous, vm_file, vm_wired, vm_busy,
	    vm_dirty;
	uint64_t vm_clean, vm_swapped, vm_faults, vm_page_in, vm_page_out;
	uint64_t vm_reclaims, vm_io_errors, swap_total, swap_free, swap_extents;
	uint64_t vm_commit_limit, vm_commit_used, vm_commit_available;
};

/*
 * Debug/validation snapshot.  Counts are live kernel objects, not capacity.
 */
struct system_resource_info {
	uint64_t process, thread, filedesc, file, pipe;
	uint64_t mount, inode, namecache;
	uint64_t vmspace, vm_object, vm_page, swap_slot;
	uint64_t disk, bio, socket, packet, net_device;
};

#define ZEDBSD_SYSTEM_PROCESS_COMMAND_MAX 64U
struct process_info {
	int32_t pid;
	int32_t ppid;
	uint32_t uid;
	uint32_t state;
	uint32_t threads;
	uint32_t gid;
	uint64_t virtual_bytes;
	char command[ZEDBSD_SYSTEM_PROCESS_COMMAND_MAX];
	uint32_t version;
	uint32_t struct_size;
	int32_t process_group;
	int32_t session;
	int32_t nice_value;
	uint32_t has_controlling_terminal;
	uint64_t cpu_ticks;
	uint64_t user_ticks;
	uint64_t system_ticks;
	uint64_t reserved[4];
};

#define ZEDBSD_SYSTEM_PROCESS_INFO_VERSION 1U

#define ZEDBSD_SYSTEM_FILE_USAGE_VERSION 1U
#define ZEDBSD_SYSTEM_FILE_USAGE_PATH_MAX 256U
#define ZEDBSD_SYSTEM_FILE_USAGE_QUERY_MOUNT 0x00000001U
#define ZEDBSD_SYSTEM_FILE_USAGE_CWD 0x00000001U
#define ZEDBSD_SYSTEM_FILE_USAGE_ROOT 0x00000002U
#define ZEDBSD_SYSTEM_FILE_USAGE_EXECUTABLE 0x00000004U
#define ZEDBSD_SYSTEM_FILE_USAGE_OPEN 0x00000008U
#define ZEDBSD_SYSTEM_FILE_USAGE_MAPPED 0x00000010U
#define ZEDBSD_SYSTEM_FILE_USAGE_SOCKET 0x00000020U

struct system_file_usage {
	uint32_t version;
	uint32_t struct_size;
	int32_t cursor_pid;
	int32_t pid;
	uint32_t uid;
	uint32_t usage_flags;
	uint32_t query_flags;
	uint32_t reserved0;
	char path[ZEDBSD_SYSTEM_FILE_USAGE_PATH_MAX];
	uint64_t reserved[4];
};

/*
 * Runtime swap control is a zedBSD extension.  The structures deliberately
 * contain no pointer-sized fields so one request number and one layout serve
 * both ILP32 and LP64 processes.
 */
#define ZEDBSD_SYSTEM_SWAP_VERSION 1U
#define ZEDBSD_SYSTEM_SWAP_SOURCE_COUNT 4U
#define ZEDBSD_SYSTEM_SWAP_SOURCE_MAX 256U
#define ZEDBSD_SYSTEM_SWAP_UUID_SIZE 8U
#define ZEDBSD_SYSTEM_SWAP_LABEL_SIZE 20U

#define ZEDBSD_SYSTEM_SWAP_STATE_INACTIVE 0U
#define ZEDBSD_SYSTEM_SWAP_STATE_ACTIVE 1U
#define ZEDBSD_SYSTEM_SWAP_STATE_DRAINING 2U

struct system_swap_control {
	uint32_t version;
	uint32_t struct_size;
	uint32_t flags;
	uint32_t reserved0;
	char source[ZEDBSD_SYSTEM_SWAP_SOURCE_MAX];
	uint32_t reserved[8];
} __attribute__((aligned(4)));

struct system_swap_source_info {
	uint32_t version;
	uint32_t struct_size;
	uint32_t flags;
	uint32_t source_id;
	uint32_t state;
	uint32_t header_version;
	uint32_t total_pages;
	uint32_t used_pages;
	uint8_t uuid[ZEDBSD_SYSTEM_SWAP_UUID_SIZE];
	char label[ZEDBSD_SYSTEM_SWAP_LABEL_SIZE];
	char source[ZEDBSD_SYSTEM_SWAP_SOURCE_MAX];
	uint32_t reserved[8];
} __attribute__((aligned(4)));

_Static_assert(sizeof(struct system_swap_control) == 304U,
    "runtime swap control ABI must be identical on ILP32 and LP64");
_Static_assert(offsetof(struct system_swap_control, source) == 16U,
    "runtime swap source selector offset is an ABI contract");
_Static_assert(sizeof(struct system_swap_source_info) == 348U,
    "runtime swap source ABI must be identical on ILP32 and LP64");
_Static_assert(offsetof(struct system_swap_source_info, source) == 60U,
    "runtime swap diagnostic source offset is an ABI contract");

#define ZEDBSD_SYSTEM_GET_INFO                                                 \
	_IOR(ZEDBSD_SYSTEM_IOC_GROUP, 1, struct system_info)
#define ZEDBSD_SYSTEM_GET_DEVICE                                               \
	_IOWR(ZEDBSD_SYSTEM_IOC_GROUP, 2, struct system_device_info)
#define ZEDBSD_SYSTEM_GET_VMSTAT                                               \
	_IOR(ZEDBSD_SYSTEM_IOC_GROUP, 3, struct vm_statistics)
#define ZEDBSD_SYSTEM_HALT _IO(ZEDBSD_SYSTEM_IOC_GROUP, 4)
#define ZEDBSD_SYSTEM_REBOOT _IO(ZEDBSD_SYSTEM_IOC_GROUP, 5)
#define ZEDBSD_SYSTEM_GET_RESOURCES                                            \
	_IOR(ZEDBSD_SYSTEM_IOC_GROUP, 6, struct system_resource_info)
#define ZEDBSD_SYSTEM_GET_PROCESS                                              \
	_IOWR(ZEDBSD_SYSTEM_IOC_GROUP, 7, struct process_info)
#define ZEDBSD_SYSTEM_GET_FILE_USAGE                                           \
	_IOWR(ZEDBSD_SYSTEM_IOC_GROUP, 8, struct system_file_usage)
#define ZEDBSD_SYSTEM_SWAP_ADD                                                 \
	_IOW(ZEDBSD_SYSTEM_IOC_GROUP, 9, struct system_swap_control)
#define ZEDBSD_SYSTEM_SWAP_REMOVE                                              \
	_IOW(ZEDBSD_SYSTEM_IOC_GROUP, 10, struct system_swap_control)
#define ZEDBSD_SYSTEM_GET_SWAP_SOURCE                                          \
	_IOWR(ZEDBSD_SYSTEM_IOC_GROUP, 11, struct system_swap_source_info)

#endif
