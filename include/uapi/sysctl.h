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
#define CTL_NET	4

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
 * login waits for /dev/gpu0 only while it is nonzero.
 */
#define HW_GPU_ATTACHING	4
/*
 * hw.gpu.start (uint64_t): how many GPU devices a driver holds for a start
 * root asks for (the i915 with the boot parameter i915.start=manual, the
 * Raspberry Pi 5's V3D unless the boot parameter rpi5gpu.start=boot).
 * Writing 1 as the superuser starts them; with none held the write fails
 * with ENODEV.
 */
#define HW_GPU_START	5
/*
 * hw.cputimes: each CPU's time since boot, counted in clock ticks of hz a
 * second.  The value is a struct cpu_times_header followed by
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
 * hw.diskstats: each physical whole disk's work since it appeared.
 * The value is a struct disk_stats_header followed by count
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
 * hw.gputelemetry: each GPU's work as its driver keeps it.
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
 * hw.thermal: the machine's temperature sensors as the kernel reads them:
 * today the ACPI thermal zones and the ACPI devices with a
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
/*
 * hw.usb.trace (uint64_t): which USB transfers the kernel describes in its
 * log, as bits.  HW_USB_TRACE_STORAGE writes a line "usb-storage-trace:"
 * for each mass storage command with how long each of its stages took.  0,
 * the default, describes none.  Only the superuser may write it.
 */
#define HW_USB_TRACE	10
#define HW_USB_TRACE_STORAGE	1U
/*
 * hw.gpu.powervr.stages (string, read only): how far the PowerVR driver has
 * come since boot, one "<stage>=<state>" word a stage separated by spaces,
 * where the state is "ok", "-" (not reached) or "fail:<reason>", for
 * example "firmware=ok render-node=ok compute=- ...".  ENOENT without the
 * driver.
 */
#define HW_GPU_STAGES	11

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

/*
 * net.tcpstats: each TCP socket's state and what it has done since it was
 * created, to follow a transfer's control over time.  The
 * value is a struct tcp_stats_header followed by count struct
 * tcp_stats_entry, in the order of the kernel's socket registry.
 * Read-only.  A buffer too small fails with ENOMEM and the length needed.
 *
 * Sequence numbers and windows are the connection's own, in bytes.  The
 * counts only grow.  features says which of the fields that depend on a
 * part of TCP this kernel may not have are kept (TCP_STATS_HAVE_*); a
 * field whose bit is clear is 0 and means "not kept", not "zero".
 */
#define NET_TCPSTATS	1

/*
 * net.tcp.congestion: the congestion control a new TCP socket starts with,
 * as its name ("reno" or "cubic", TCP_CONGESTION's names).  Reading gives
 * the name and its NUL; the superuser sets another by its name (EINVAL for
 * an empty one, ESRCH for one the kernel does not have).  A socket that
 * already exists keeps its own.
 */
#define NET_TCP			2
#define NET_TCP_CONGESTION	1

/* The version of the net.tcpstats layout (2: the congestion control's fields at the end of an entry). */
#define TCP_STATS_VERSION	2U

/* The address family of an entry: its addresses are IPv4 (4 bytes) or IPv6 (16). */
#define TCP_STATS_FAMILY_INET	4U
#define TCP_STATS_FAMILY_INET6	6U

/* The fields an entry keeps beyond the ones every version has. */
#define TCP_STATS_HAVE_CWND	0x00000001U
#define TCP_STATS_HAVE_RTT	0x00000002U
#define TCP_STATS_HAVE_WSCALE	0x00000004U
#define TCP_STATS_HAVE_SACK	0x00000008U
#define TCP_STATS_HAVE_FAST_RETRANSMIT	0x00000010U
/*
 * send_unacknowledged, send_next and receive_next are given: the reader is
 * the superuser.  Without the bit the three are 0.  The
 * addresses, ports and every other field are given to every reader, as
 * netstat shows them on other systems.
 */
#define TCP_STATS_HAVE_SEQUENCE	0x00000020U
/*
 * cc, cubic_w_max_kb, cubic_k_ms and cwnd_state are kept (the congestion
 * control: TCP_STATS_CC_*).
 */
#define TCP_STATS_HAVE_CC	0x00000040U

/* The congestion control of an entry (cc): RFC 5681's (NewReno) or CUBIC (RFC 9438). */
#define TCP_STATS_CC_RENO	0U
#define TCP_STATS_CC_CUBIC	1U

/*
 * cwnd_state's bits: the window held back a queued segment in this round
 * (it is in use); a CUBIC epoch runs; the next CUBIC epoch starts after a
 * timeout (K = 0).
 */
#define TCP_STATS_CWND_BLOCKED		0x00000001U
#define TCP_STATS_CWND_EPOCH		0x00000002U
#define TCP_STATS_CWND_AFTER_TIMEOUT	0x00000004U

/*
 * The head of net.tcpstats: the layout's version, the header's size, one
 * entry's size and how many entries follow; the standard packets of the
 * network stack's pool (how many there are, how many are taken now, the
 * most ever taken, and how many allocations found none free to them: a
 * copy kept for retransmission does not take the last few, which stay for
 * the receive and for what only passes through); the time of
 * the reading in milliseconds since boot; and the pool's jumbo packets in
 * the same counts, with how many of them are kept for the drivers'
 * receive (an allocation outside that reserve that finds none free is a
 * failure).  Every field has a fixed width, so one layout serves ILP32
 * and LP64 processes.
 */
struct tcp_stats_header {
	uint32_t version;
	uint32_t struct_size;
	uint32_t element_size;
	uint32_t count;
	uint32_t packets_total;
	uint32_t packets_used;
	uint32_t packets_used_max;
	uint32_t reserved;
	uint64_t packet_alloc_failures;
	uint64_t time_ms;
	uint32_t jumbo_total;
	uint32_t jumbo_used;
	uint32_t jumbo_used_max;
	uint32_t jumbo_reserve;
	uint64_t jumbo_alloc_failures;
};

/*
 * One TCP socket in net.tcpstats.
 *
 * The state is the kernel's enum tcp_state (0 closed, 1 listen, 2 SYN
 * sent, 3 SYN received, 4 established, 5 FIN wait 1, 6 FIN wait 2, 7 close
 * wait, 8 last ACK, 9 time wait).  An IPv4 address is in the first 4 bytes
 * of its field, in network order.  backoff is how many times the
 * retransmission timeout is doubled: the timeouts since the last valid
 * round-trip sample (a probe of a closed window answered or
 * sent again as it opens sets it back too, and the probes themselves are
 * the persist timer's, apart from it); rto_ms is the timeout
 * it waits now (the connection's, so doubled) and retransmit_in_ms how
 * long is left of it (0: no segment outstanding).  With TCP_STATS_HAVE_RTT, srtt_us and
 * rttvar_us are RFC 6298's smoothed round-trip time and its variation and
 * rtt_samples how many samples they come from; while rtt_samples is 0 the
 * two mean nothing (no sample yet, not a round trip of 0).  rtt_samples
 * took the place of a reserved field of the same size (the layout and
 * version are unchanged; an older kernel leaves it 0).
 * send_window is the window the peer last advertised; receive_window the
 * one this end last put in a segment.  send_next is the end of what was
 * handed to the device (RFC 9293's SND.NXT; segments queued behind a
 * refused one are not part of it), and in_flight_bytes the
 * sequence space sent and not acknowledged; send_segments the segments in
 * the ring, unsent ones included, of send_ring.  receive_bytes and
 * receive_packets are what waits for the reader, of receive_limit_bytes;
 * reorder_segments the segments kept ahead of a gap.
 *
 * The counts: segments sent (every kind) and received; data segments
 * queued for their first transmission and their bytes (send_refused counts
 * the times the device or the packet pool refused one, which then waited
 * in the ring unsent to be sent again, and send_buffer_sleeps the times a
 * writer found no packet for a segment's copy and slept before it tried
 * again); payload bytes taken in order; segments resent by the
 * retransmission path that the device took and their bytes, and how many
 * times that resend was made due at once
 * (immediate_resends: a fast retransmit, a partial acknowledgement in a
 * recovery, or a window that opened for a probe) rather than
 * by a timeout; segments the stack failed to hand to the
 * device (segment_send_failures, an acknowledgement or a resend lost on
 * the way out); acknowledgements received, those that acknowledged
 * nothing new while data was outstanding (duplicate_acks_in), and those
 * ignored as outside the outstanding sequence space (acks_outside); zero
 * windows received and sent; window updates sent by a read; segments
 * wholly before receive_next (old_segments_in) and ahead of it
 * (out_of_order_in, of them out_of_order_kept); in-order segments the
 * receive queue had no room for (receive_refused); and fast
 * retransmissions (with TCP_STATS_HAVE_FAST_RETRANSMIT).
 *
 * With TCP_STATS_HAVE_CC: the congestion control (cc, TCP_STATS_CC_*);
 * CUBIC's window before the last loss in KiB and its K in milliseconds
 * (0 for Reno); and cwnd_state (TCP_STATS_CWND_*).
 */
struct tcp_stats_entry {
	uint32_t family;
	uint32_t state;
	uint32_t features;
	uint32_t backoff;
	uint8_t local_address[16];
	uint8_t remote_address[16];
	uint16_t local_port;
	uint16_t remote_port;
	uint32_t rtt_samples;
	uint32_t send_unacknowledged;
	uint32_t send_next;
	uint32_t send_window;
	uint32_t receive_next;
	uint32_t receive_window;
	uint32_t in_flight_bytes;
	uint32_t send_segments;
	uint32_t send_ring;
	uint32_t send_mss;
	uint32_t receive_mss;
	uint32_t reorder_segments;
	uint32_t receive_packets;
	uint32_t receive_bytes;
	uint32_t receive_limit_bytes;
	uint32_t cwnd;
	uint32_t ssthresh;
	uint32_t srtt_us;
	uint32_t rttvar_us;
	uint32_t rto_ms;
	uint32_t retransmit_in_ms;
	uint64_t segments_out;
	uint64_t segments_in;
	uint64_t data_segments_out;
	uint64_t data_bytes_out;
	uint64_t data_bytes_in;
	uint64_t retransmit_segments;
	uint64_t retransmit_bytes;
	uint64_t immediate_resends;
	uint64_t send_refused;
	uint64_t send_buffer_sleeps;
	uint64_t segment_send_failures;
	uint64_t acks_in;
	uint64_t duplicate_acks_in;
	uint64_t acks_outside;
	uint64_t zero_windows_in;
	uint64_t zero_windows_out;
	uint64_t window_updates_out;
	uint64_t old_segments_in;
	uint64_t out_of_order_in;
	uint64_t out_of_order_kept;
	uint64_t receive_refused;
	uint64_t fast_retransmits;
	uint32_t cc;
	uint32_t cubic_w_max_kb;
	uint32_t cubic_k_ms;
	uint32_t cwnd_state;
};

_Static_assert(sizeof(struct tcp_stats_header) == 72U,
    "net.tcpstats header ABI must be identical on ILP32 and LP64");
_Static_assert(sizeof(struct tcp_stats_entry) == 328U,
    "net.tcpstats entry ABI must be identical on ILP32 and LP64");

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
/* The login= boot parameter: "graphical", "console", or "" when it was not given. */
#define KERN_BOOT_LOGIN 9
/* The kmsg= boot parameter: "quiet", "console", or "" when it was not given. */
#define KERN_BOOT_KMSG 10
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
