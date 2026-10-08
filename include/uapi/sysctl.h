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
/*
 * hw.gpu.attaching (uint32_t): how many GPU devices a driver has attached but
 * not yet published a node (/dev/gpuN) for or given up on.  The graphical
 * login waits for /dev/gpu0 only while it is nonzero (BUG-092).
 */
#define HW_GPU_ATTACHING	4
/*
 * hw.gpu.start (uint64_t): how many GPU devices a driver holds for a start
 * root asks for (the i915 with the boot parameter i915.start=manual).
 * Writing 1 as the superuser starts them; with none held the write fails
 * with ENODEV.
 */
#define HW_GPU_START	5
/*
 * hw.cputimes: each CPU's time since boot, counted in clock ticks of hz a
 * second (ws134-p005).  The value is a struct cpu_times_header followed by
 * count struct cpu_times_entry, one a CPU in the order of their numbers.
 * Read-only.  A buffer too small fails with ENOMEM and the length needed;
 * a reader asks for the length first (no buffer) or tries again.
 *
 * Each tick is charged to what its CPU was doing: user (a user thread in
 * user mode), system (a thread in the kernel, or a kernel thread), idle
 * (the CPU's idle thread), or other (between threads: the running thread
 * was going to sleep or leaving).  Interrupts are not counted apart: an
 * interrupt or a page fault that stopped a user thread is that thread's
 * user time.  A CPU that is not online has every count 0.
 */
#define HW_CPUTIMES	6

/* The version of the hw.cputimes layout. */
#define CPU_TIMES_VERSION	1U

/*
 * The head of hw.cputimes: the layout's version, the header's size, one
 * entry's size, how many entries follow, and the ticks a second.  Every
 * field has a fixed width, so one layout serves ILP32 and LP64 processes.
 */
struct cpu_times_header {
	uint32_t version;
	uint32_t struct_size;
	uint32_t element_size;
	uint32_t count;
	uint32_t hz;
	uint32_t reserved;
};

/* One CPU's ticks in hw.cputimes. */
struct cpu_times_entry {
	uint64_t user;
	uint64_t system;
	uint64_t idle;
	uint64_t other;
};

_Static_assert(sizeof(struct cpu_times_header) == 24U,
    "hw.cputimes header ABI must be identical on ILP32 and LP64");
_Static_assert(sizeof(struct cpu_times_entry) == 32U,
    "hw.cputimes entry ABI must be identical on ILP32 and LP64");

/*
 * hw.diskstats: each physical whole disk's work since it appeared
 * (ws134-p006).  The value is a struct disk_stats_header followed by count
 * struct disk_stats_entry, in the order the disks appeared.  Partitions,
 * loop disks and a file system's own disks are not listed: a partition's
 * work is its whole disk's.  Read-only.  A buffer too small fails with
 * ENOMEM and the length needed.
 *
 * read_ns and write_ns add up the time from each request's submission to
 * its completion (their change over the change of the ops is the mean
 * latency); busy_ns is the time the disk had any request outstanding.
 * Only reads and writes count (not flushes); bytes are what completed.
 * A disk that is unplugged and plugged again comes back with another id;
 * the header's generation changes whenever a disk appears or goes.
 */
#define HW_DISKSTATS	7

/* The version of the hw.diskstats layout. */
#define DISK_STATS_VERSION	1U

/* What kind of device a disk is (struct disk_stats_entry's kind). */
#define DISK_STATS_KIND_OTHER	1U
#define DISK_STATS_KIND_NVME	2U
#define DISK_STATS_KIND_USB	3U
#define DISK_STATS_KIND_UAS	4U
#define DISK_STATS_KIND_IDE	5U
#define DISK_STATS_KIND_SDMMC	6U
#define DISK_STATS_KIND_SCSI	7U

/* A disk's flags in hw.diskstats. */
#define DISK_STATS_READ_ONLY	0x00000001U
#define DISK_STATS_REMOVABLE	0x00000002U

/*
 * The head of hw.diskstats: the layout's version, the header's size, one
 * entry's size, how many entries follow, and the generation of the set of
 * disks.  Every field has a fixed width, so one layout serves ILP32 and
 * LP64 processes.
 */
struct disk_stats_header {
	uint32_t version;
	uint32_t struct_size;
	uint32_t element_size;
	uint32_t count;
	uint32_t generation;
	uint32_t reserved;
};

/*
 * One disk in hw.diskstats: its name, kind and flags, its id (never
 * reused while the system runs), the generation it appeared in, its
 * completed reads and writes and their bytes and times, the time it was
 * busy (nanoseconds), and the requests outstanding now.
 */
struct disk_stats_entry {
	char name[32];
	uint32_t kind;
	uint32_t flags;
	uint64_t id;
	uint64_t generation;
	uint64_t read_ops;
	uint64_t write_ops;
	uint64_t read_bytes;
	uint64_t write_bytes;
	uint64_t read_ns;
	uint64_t write_ns;
	uint64_t busy_ns;
	uint32_t inflight;
	uint32_t reserved;
};

_Static_assert(sizeof(struct disk_stats_header) == 24U,
    "hw.diskstats header ABI must be identical on ILP32 and LP64");
_Static_assert(sizeof(struct disk_stats_entry) == 120U,
    "hw.diskstats entry ABI must be identical on ILP32 and LP64");

/*
 * hw.gputelemetry: each GPU's work as its driver keeps it (ws134-p007).
 * The value is a struct gpu_telemetry_header followed by count struct
 * gpu_telemetry_entry, one a GPU whose driver reports telemetry (none for a
 * driver that keeps nothing, such as the virtual GPU).  Read-only.  A
 * buffer too small fails with ENOMEM and the length needed.
 *
 * valid says which fields the driver filled (GPU_TELEMETRY_*).  busy_ns
 * adds up the time any engine ran a request, up to time_ns (the driver's
 * clock when it was read), so the change of busy_ns over the change of
 * time_ns is the GPU's use.  The frequencies are in MHz: the one the
 * hardware runs at, the one the driver last asked for, and the range it
 * asks within.  objects_bytes is the memory the GPU's objects hold, of
 * objects_limit.
 */
#define HW_GPUTELEMETRY	8

/* The version of the hw.gputelemetry layout. */
#define GPU_TELEMETRY_VERSION	1U

/* The fields a driver filled (struct gpu_telemetry_entry's valid). */
#define GPU_TELEMETRY_BUSY	0x00000001U
#define GPU_TELEMETRY_CUR_MHZ	0x00000002U
#define GPU_TELEMETRY_REQ_MHZ	0x00000004U
#define GPU_TELEMETRY_RANGE_MHZ	0x00000008U
#define GPU_TELEMETRY_OBJECTS	0x00000010U

/*
 * The head of hw.gputelemetry: the layout's version, the header's size,
 * one entry's size and how many entries follow.  Every field has a fixed
 * width, so one layout serves ILP32 and LP64 processes.
 */
struct gpu_telemetry_header {
	uint32_t version;
	uint32_t struct_size;
	uint32_t element_size;
	uint32_t count;
	uint32_t reserved[2];
};

/* One GPU in hw.gputelemetry. */
struct gpu_telemetry_entry {
	char driver[16];
	uint32_t valid;
	uint32_t reserved;
	uint64_t time_ns;
	uint64_t busy_ns;
	uint64_t objects_bytes;
	uint64_t objects_limit;
	uint32_t cur_mhz;
	uint32_t req_mhz;
	uint32_t min_mhz;
	uint32_t max_mhz;
};

_Static_assert(sizeof(struct gpu_telemetry_header) == 24U,
    "hw.gputelemetry header ABI must be identical on ILP32 and LP64");
_Static_assert(sizeof(struct gpu_telemetry_entry) == 72U,
    "hw.gputelemetry entry ABI must be identical on ILP32 and LP64");

/*
 * hw.thermal: the machine's temperature sensors as the kernel reads them
 * (ws134-p009): today the ACPI thermal zones and the ACPI devices with a
 * _TMP (the DPTF participants of an Intel laptop).  The value is a struct
 * thermal_header followed by count struct thermal_entry.  Read-only.  A
 * buffer too small fails with ENOMEM and the length needed.
 *
 * name is the sensor's ACPI path ("\_SB_.PC00.TCPU").  valid says which of
 * the values are there (THERMAL_HAVE_*): the temperature, and the passive
 * and critical trip points, in thousandths of a degree Celsius.  time_ns is
 * when the temperature was read (the kernel reads every few seconds, not
 * at each sysctl).  THERMAL_FLAG_CPU marks the processor's sensor.
 */
#define HW_THERMAL	9

/* The version of the hw.thermal layout. */
#define THERMAL_VERSION	1U

/* What kind of sensor an entry is. */
#define THERMAL_KIND_ZONE	1U
#define THERMAL_KIND_DEVICE	2U

/* The values an entry has (struct thermal_entry's valid). */
#define THERMAL_HAVE_TEMPERATURE	0x00000001U
#define THERMAL_HAVE_PASSIVE		0x00000002U
#define THERMAL_HAVE_CRITICAL		0x00000004U

/* An entry's flags: the processor's sensor. */
#define THERMAL_FLAG_CPU	0x00000001U

/*
 * The head of hw.thermal: the layout's version, the header's size, one
 * entry's size and how many entries follow.  Every field has a fixed
 * width, so one layout serves ILP32 and LP64 processes.
 */
struct thermal_header {
	uint32_t version;
	uint32_t struct_size;
	uint32_t element_size;
	uint32_t count;
	uint32_t reserved[2];
};

/* One sensor in hw.thermal. */
struct thermal_entry {
	char name[32];
	uint32_t kind;
	uint32_t flags;
	uint32_t valid;
	int32_t milli_celsius;
	int32_t passive_milli_celsius;
	int32_t critical_milli_celsius;
	uint64_t time_ns;
};

_Static_assert(sizeof(struct thermal_header) == 24U,
    "hw.thermal header ABI must be identical on ILP32 and LP64");
_Static_assert(sizeof(struct thermal_entry) == 64U,
    "hw.thermal entry ABI must be identical on ILP32 and LP64");

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
/* The login= boot parameter (ws035-p098): "graphical", "console", or "" when it was not given. */
#define KERN_BOOT_LOGIN 9
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
