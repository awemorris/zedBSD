/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The records a Series6 core's firmware shares with the host.
 *
 * The firmware is a build of one interface, and every record it reads or
 * writes has the layout of that build: the init record that tells the
 * firmware where everything is, the kernel's command buffer and the
 * firmware's, the contexts, the trace buffer, the compatibility record,
 * and the commands a client puts in a context's buffer.  The layouts
 * here are those of the interface the G6430's firmware is built for,
 * with that core's errata and features resolved: a geometry command with
 * the region header of erratum 44455, a compute command with the stream
 * end of erratum 54441, a 2D command because the core has the 2D engine,
 * one firmware thread, five data masters.
 *
 * Each record is written out with explicit padding, every 64-bit field
 * on an 8-byte boundary as the META compiler lays it out, and its size
 * and the offsets the firmware checks are fixed by assertions below.
 * The firmware is told the same sizes and offsets in its alignment table
 * and refuses to run with a host that disagrees.
 *
 * A firmware address is a 32-bit META address; a device address is a
 * 64-bit address in the core's MMU.
 */

#ifndef KERN_DRIVERS_GPU_POWERVR_S6_FWIF_H
#define KERN_DRIVERS_GPU_POWERVR_S6_FWIF_H

#include <stddef.h>
#include <stdint.h>

/*
 * The interface version and the kernel build options the firmware is
 * expected to report in its compatibility record, and the record's own
 * layout version and longest version string.
 *
 * The build options are those of the vendor kernel built with this
 * interface: register access to the core and the DRM driver, without
 * parameter dumps or debug.
 *
 * Values referenced:
 * Android kernel/x86_64 android-x86_64-fugu-3.10-nougat-mr2 999603b9ab
 * drivers/staging/imgtec/pvrversion.h (PVRVERSION_MAJ 1, PVRVERSION_MIN 6,
 * PVRVERSION_BUILD 4278818, PVRVERSION_PACK major << 16 | minor),
 * rogue/rgx_options_km.h (RGX_BUILD_OPTIONS_KM with config_kernel.h's
 * SUPPORT_RGX bit 4 and SUPPORT_DRM bit 8, 0x110 when compiled;
 * RGX_BUILD_OPTIONS_MASK_KM 0xFFFF; OPTIONS_DEBUG_MASK bit 10) and
 * rogue/rgx_fwif_shared.h (RGXFWIF_COMPCHECKS_LAYOUT_VERSION 1,
 * RGXFWIF_COMPCHECKS_BVNC_V_LEN_MAX 3).
 */
#define POWERVR_S6_FW_INTERFACE_VERSION	0x00010006U
#define POWERVR_S6_FW_INTERFACE_BUILD	4278818U
#define POWERVR_S6_FW_KERNEL_OPTIONS	0x00000110U
#define POWERVR_S6_FW_KERNEL_OPTIONS_MASK 0x0000ffffU
#define POWERVR_S6_FW_DEBUG_OPTION	0x00000400U
#define POWERVR_S6_FW_COMPAT_LAYOUT	1U
#define POWERVR_S6_FW_COMPAT_VERSION_MAX 3U

/*
 * The data masters: the general one, the 2D engine, geometry, fragment
 * and compute.  The two ray tracing masters do not exist on this core.
 *
 * Values referenced:
 * Android kernel/x86_64 android-x86_64-fugu-3.10-nougat-mr2 999603b9ab
 * drivers/staging/imgtec/rogue/rgx_common.h (RGXFWIF_DM_GP 0, _2D 1, _TA 2,
 * _3D 3, _CDM 4, RGXFWIF_DM_MAX 5 without RGX_FEATURE_RAY_TRACING).
 */
#define POWERVR_S6_DM_GENERAL		0U
#define POWERVR_S6_DM_2D		1U
#define POWERVR_S6_DM_GEOMETRY		2U
#define POWERVR_S6_DM_FRAGMENT		3U
#define POWERVR_S6_DM_COMPUTE		4U
#define POWERVR_S6_DM_COUNT		5U

/*
 * The kernel's commands to the firmware, the sizes of the two command
 * buffers between them, and the flags of a cache invalidation.
 *
 * The flags are those of a core whose system level cache is not
 * virtually tagged: the page table, directory and catalogue caches, and
 * the TLBs of the parameter manager and the MMU.
 *
 * Values referenced:
 * Android kernel/x86_64 android-x86_64-fugu-3.10-nougat-mr2 999603b9ab
 * drivers/staging/imgtec/rogue/rgx_fwif_km.h (RGXFWIF_KCCB_CMD_KICK 101,
 * _MMUCACHE 102, _BP 104, _SLCBPCTL 106, _SYNC 107, _SLCFLUSHINVAL 108,
 * _CLEANUP 109, _POW 110, _HEALTH_CHECK 120, _LOGTYPE_UPDATE 124;
 * RGXFWIF_FWCCB_CMD_ZSBUFFER_BACKING 101 to _UPDATE_STATS 107;
 * RGXFWIF_CLEANUP_FWCOMMONCONTEXT 0, _HWRTDATA 1, _FREELIST 2, _ZSBUFFER 3;
 * RGXFWIF_POW_OFF_REQ 1, _FORCED_IDLE_REQ 2, _NUMDUST_CHANGE 3,
 * _APM_LATENCY_CHANGE 4; RGXFWIF_MMUCACHEDATA_FLAGS_PT 1, _PD 2, _PC 4,
 * _PMTLB 0x10, _TLB 0x18 without RGX_FEATURE_SLC_VIVT) and rgxfwutils.c
 * (RGXFWIF_KCCB_NUMCMDS_LOG2 7, RGXFWIF_FWCCB_NUMCMDS_LOG2 5).
 */
#define POWERVR_S6_KCCB_KICK		101U
#define POWERVR_S6_KCCB_MMU_CACHE	102U
#define POWERVR_S6_KCCB_BREAKPOINT	104U
#define POWERVR_S6_KCCB_SLC_BYPASS	106U
#define POWERVR_S6_KCCB_SYNC		107U
#define POWERVR_S6_KCCB_SLC_FLUSH	108U
#define POWERVR_S6_KCCB_CLEANUP		109U
#define POWERVR_S6_KCCB_POWER		110U
#define POWERVR_S6_KCCB_HEALTH_CHECK	120U
#define POWERVR_S6_KCCB_LOG_TYPE	124U
#define POWERVR_S6_FWCCB_ZS_BACKING	101U
#define POWERVR_S6_FWCCB_ZS_UNBACKING	102U
#define POWERVR_S6_FWCCB_FREE_LIST_GROW	103U
#define POWERVR_S6_FWCCB_FREE_LIST_REBUILD 104U
#define POWERVR_S6_FWCCB_CONTEXT_RESET	105U
#define POWERVR_S6_FWCCB_DEBUG_DUMP	106U
#define POWERVR_S6_FWCCB_UPDATE_STATS	107U
#define POWERVR_S6_CLEANUP_CONTEXT	0U
#define POWERVR_S6_CLEANUP_TARGET_DATA	1U
#define POWERVR_S6_CLEANUP_FREE_LIST	2U
#define POWERVR_S6_CLEANUP_DEPTH_BUFFER	3U
#define POWERVR_S6_POWER_OFF		1U
#define POWERVR_S6_POWER_FORCED_IDLE	2U
#define POWERVR_S6_POWER_DUST_COUNT	3U
#define POWERVR_S6_POWER_IDLE_LATENCY	4U
#define POWERVR_S6_MMU_TABLES		0x01U
#define POWERVR_S6_MMU_DIRECTORIES	0x02U
#define POWERVR_S6_MMU_CATALOGUE	0x04U
#define POWERVR_S6_MMU_PM_TLB		0x10U
#define POWERVR_S6_MMU_TLB		0x18U
#define POWERVR_S6_KCCB_COMMANDS	128U
#define POWERVR_S6_FWCCB_COMMANDS	32U

/*
 * The commands a client puts in a context's command buffer.
 *
 * A work command has the task bit set; the fence, update and padding
 * commands do not.
 *
 * Values referenced:
 * Android kernel/x86_64 android-x86_64-fugu-3.10-nougat-mr2 999603b9ab
 * drivers/staging/imgtec/rogue/rgx_fwif_shared.h (RGX_CCB_TYPE_TASK bit 31,
 * RGXFWIF_CCB_CMD_TYPE_TA 201, _3D 202, _CDM 203, _TQ_3D 204, _TQ_2D 205,
 * _3D_PR 206, _NULL 207, _FENCE 212, _UPDATE 213, _RMW_UPDATE 214,
 * _FENCE_PR 215, _PRIORITY 216, _POST_TIMESTAMP 217, _UNFENCED_UPDATE 218,
 * _UNFENCED_RMW_UPDATE 219, _PADDING 220).
 */
#define POWERVR_S6_CCB_TASK		0x80000000U
#define POWERVR_S6_CCB_GEOMETRY		(201U | POWERVR_S6_CCB_TASK)
#define POWERVR_S6_CCB_FRAGMENT		(202U | POWERVR_S6_CCB_TASK)
#define POWERVR_S6_CCB_COMPUTE		(203U | POWERVR_S6_CCB_TASK)
#define POWERVR_S6_CCB_TRANSFER_3D	(204U | POWERVR_S6_CCB_TASK)
#define POWERVR_S6_CCB_TRANSFER_2D	(205U | POWERVR_S6_CCB_TASK)
#define POWERVR_S6_CCB_PARTIAL_RENDER	(206U | POWERVR_S6_CCB_TASK)
#define POWERVR_S6_CCB_NULL		(207U | POWERVR_S6_CCB_TASK)
#define POWERVR_S6_CCB_FENCE		212U
#define POWERVR_S6_CCB_UPDATE		213U
#define POWERVR_S6_CCB_RMW_UPDATE	214U
#define POWERVR_S6_CCB_FENCE_PARTIAL	215U
#define POWERVR_S6_CCB_PRIORITY		216U
#define POWERVR_S6_CCB_POST_TIMESTAMP	217U
#define POWERVR_S6_CCB_UNFENCED_UPDATE	218U
#define POWERVR_S6_CCB_UNFENCED_RMW	219U
#define POWERVR_S6_CCB_PADDING		220U

/* How many words the firmware's alignment table holds: the kernel's part, then the client's. */
#define POWERVR_S6_ALIGN_KERNEL_WORDS	19U
#define POWERVR_S6_ALIGN_CLIENT_WORDS	41U
#define POWERVR_S6_ALIGN_WORDS		(POWERVR_S6_ALIGN_KERNEL_WORDS + POWERVR_S6_ALIGN_CLIENT_WORDS)

/* One fence or update: a sync word's firmware address and the value. */
struct powervr_s6_fw_sync {
	uint32_t address;
	uint32_t value;
};

/* The head of every command in a client's command buffer. */
struct powervr_s6_fw_command_head {
	uint32_t type;
	uint32_t bytes;
	uint32_t external_job;
	uint32_t internal_job;
};

/* The offsets of a client's command buffer, shared by host and firmware. */
struct powervr_s6_fw_client_control {
	uint32_t write_offset;
	uint32_t read_offset;
	uint32_t dependency_offset;
	uint32_t wrap_mask;
};

/* The offsets of the kernel's or the firmware's command buffer. */
struct powervr_s6_fw_queue_control {
	uint32_t write_offset;
	uint32_t read_offset;
	uint32_t wrap_mask;
	uint32_t command_bytes;
};

/* The counters a cleanup request waits on: commands handed in and done. */
struct powervr_s6_fw_cleanup_counts {
	uint32_t submitted;
	uint32_t executed;
};

/* A kick: the context, its new write offset, and the cleanup counters its commands touch. */
struct powervr_s6_fw_kick {
	uint32_t context;
	uint32_t write_offset;
	uint32_t cleanup_count;
	uint32_t cleanup[4];
};

/* An invalidation of the core's MMU caches for a memory context. */
struct powervr_s6_fw_mmu_cache {
	uint32_t memory_context;
	uint32_t flags;
};

/* A flush, and with invalidate set an invalidation, of the system level cache. */
struct powervr_s6_fw_slc_flush {
	uint32_t context;
	uint32_t invalidate;
	uint32_t context_only;
	uint32_t data_master;
};

/* A request to forget a firmware object, completed through a sync word. */
struct powervr_s6_fw_cleanup {
	uint32_t kind;
	uint32_t object;
	uint32_t sync;
};

/* A power request and its one argument. */
struct powervr_s6_fw_power {
	uint32_t kind;
	uint32_t value;
};

/*
 * One command in the kernel's command buffer.
 *
 * The data is 40 bytes on an 8-byte boundary; only the member the type
 * names is meaningful.
 */
struct powervr_s6_fw_kernel_command {
	uint32_t type;
	uint32_t data_master;
	union {
		struct powervr_s6_fw_kick kick;
		struct powervr_s6_fw_mmu_cache mmu_cache;
		struct powervr_s6_fw_slc_flush slc_flush;
		struct powervr_s6_fw_cleanup cleanup;
		struct powervr_s6_fw_power power;
		uint64_t words[5];
	} data;
};

/* One command in the firmware's command buffer: its type and 24 bytes of data. */
struct powervr_s6_fw_firmware_command {
	uint32_t type;
	uint32_t reserved;
	uint64_t data[3];
};

/* A core's name as the compatibility record holds it. */
struct powervr_s6_fw_compat_core {
	uint32_t layout;
	uint32_t version_max;
	uint32_t bnc;
	char version[4];
};

/*
 * The compatibility record: what the firmware found the core to be,
 * what it was built for, and its build's interface version and options.
 * The firmware sets updated when it has filled the record.
 */
struct powervr_s6_fw_compat {
	struct powervr_s6_fw_compat_core hardware;
	struct powervr_s6_fw_compat_core firmware;
	uint32_t processor_version;
	uint32_t interface_version;
	uint32_t interface_build;
	uint32_t build_options;
	uint32_t updated;
	uint32_t reserved;
};

/* One data master's signature buffer and how many words are left in it. */
struct powervr_s6_fw_signature_buffer {
	uint32_t buffer;
	uint32_t words_left;
};

/* One heap's memory tiling: base, length and the stride in tiles. */
struct powervr_s6_fw_tiling {
	uint64_t base;
	uint64_t bytes;
	uint64_t stride;
};

/* A block known to the firmware both by device address and by firmware address. */
struct powervr_s6_fw_dual_address {
	uint64_t device;
	uint32_t firmware;
	uint32_t reserved;
};

/*
 * The init record.
 *
 * The host fills it before the firmware starts and hands its firmware
 * address to the bootloader.  The firmware writes back started, the
 * started time stamp and the compatibility record.
 */
struct powervr_s6_fw_init {
	uint64_t fault_page;
	uint64_t pds_code_base;
	uint64_t usc_code_base;
	uint64_t result_dump_base;
	uint64_t dpx_stream_base;
	uint64_t ray_heap_base;
	uint32_t first_geometry;
	uint32_t first_fragment;
	uint32_t framework_after_init;
	uint32_t hwperf_enable;
	uint32_t hwperf_custom_unfiltered;
	uint32_t power_sync;
	uint32_t filter_flags;
	uint32_t kernel_queue_control;
	uint32_t kernel_queue;
	uint32_t firmware_queue_control;
	uint32_t firmware_queue;
	uint32_t reserved_0;
	struct powervr_s6_fw_signature_buffer signature_buffers[POWERVR_S6_DM_COUNT];
	uint32_t logging_enable;
	uint32_t config_flags;
	uint32_t breakpoint_temps;
	uint32_t breakpoint_shareds;
	uint32_t hwr_dump_limit;
	uint32_t reserved_1;
	struct powervr_s6_fw_tiling tiling[4];
	uint32_t runtime_config;
	uint32_t trace_buffer;
	uint64_t hwperf_filter;
	uint32_t hwr_info;
	uint32_t utilisation;
	uint32_t register_config;
	uint32_t hwperf_control;
	uint32_t alignment_table;
	uint32_t core_clock_hz;
	uint32_t idle_latency_ms;
	uint32_t started;
	uint32_t marker;
	uint32_t started_time;
	uint32_t jones_disable_mask;
	uint32_t reserved_2;
	struct powervr_s6_fw_compat compat;
	struct powervr_s6_fw_dual_address core_memory_data;
	uint32_t perf_config;
	uint32_t thread1_stack;
};

/*
 * The part every firmware context starts with.
 *
 * The waiting and run nodes are the firmware's own list links (previous
 * and next); the host leaves them zero.
 */
struct powervr_s6_fw_common_context {
	uint32_t client_control;
	uint32_t client_queue;
	uint32_t waiting_node[2];
	uint32_t run_node[2];
	uint32_t memory_context;
	uint32_t reserved_0;
	uint32_t context_state;
	uint32_t reserved_1;
	uint32_t resume_command;
	uint32_t flags;
	uint32_t priority;
	uint32_t priority_sequence;
	uint64_t fence_address;
	uint32_t server_identity;
	uint32_t process;
	uint32_t stats_pending;
	int32_t stats_stores;
	int32_t stats_out_of_memory;
	int32_t stats_partial_renders;
	uint32_t data_master;
	uint32_t reserved_2;
};

/* A render context: a geometry and a fragment context and the totals they count. */
struct powervr_s6_fw_render_context {
	struct powervr_s6_fw_common_context geometry;
	struct powervr_s6_fw_common_context fragment;
	uint32_t partial_renders;
	uint32_t out_of_memory;
};

/* A memory context: the page catalogue the core walks and the breakpoint set on it. */
struct powervr_s6_fw_memory_context {
	uint64_t catalogue;
	uint32_t catalogue_register;
	uint32_t breakpoint_address;
	uint32_t breakpoint_handler;
	uint32_t breakpoint_control;
};

/* Where the firmware failed an assertion: file, message and line. */
struct powervr_s6_fw_assertion {
	char path[200];
	char message[200];
	uint32_t line;
	uint32_t reserved;
};

/*
 * One firmware thread's log.
 *
 * Host_view is a slot the host may keep its own pointer to the log in;
 * the firmware never reads it.
 */
struct powervr_s6_fw_thread_log {
	uint32_t write_pointer;
	uint32_t log;
	uint64_t host_view;
	struct powervr_s6_fw_assertion assertion;
};

/*
 * The trace buffer: the firmware's log, its power state, and its
 * recovery counters per data master.
 */
struct powervr_s6_fw_trace {
	uint32_t log_type;
	uint32_t power_state;
	struct powervr_s6_fw_thread_log thread[1];
	uint16_t locked_up[POWERVR_S6_DM_COUNT];
	uint16_t overran[POWERVR_S6_DM_COUNT];
	uint16_t recovered[POWERVR_S6_DM_COUNT];
	uint16_t false_detect[POWERVR_S6_DM_COUNT];
	uint32_t hwr_counter;
	uint32_t poll_address[1];
	uint32_t poll_mask[1];
	uint32_t hwr_state;
	uint32_t recovery_flags[POWERVR_S6_DM_COUNT];
	uint32_t hwperf_read;
	uint32_t hwperf_write;
	uint32_t hwperf_wraps;
	uint32_t hwperf_bytes;
	uint32_t hwperf_drops;
	uint32_t hwperf_utilisation;
	uint32_t first_drop;
	uint32_t last_drop;
	uint32_t interrupts;
	uint32_t kernel_commands_done;
	uint32_t reserved;
	uint64_t idle_since;
	uint32_t energy;
	uint32_t thread1_pc[16];
	uint32_t thread1_pc_write;
};

/* One performance counter block's set-up. */
struct powervr_s6_fw_counter_block {
	uint32_t valid;
	uint32_t enabled;
	uint32_t block;
	uint32_t counter_mask;
	uint64_t counter_config[4];
};

/* The counters picked for one custom counter block. */
struct powervr_s6_fw_counter_pick {
	uint32_t count;
	uint32_t counters[8];
};

/* The performance counter set-up. */
struct powervr_s6_fw_counters {
	uint32_t reset_ordinal;
	uint32_t enabled_blocks;
	struct powervr_s6_fw_counter_block blocks[16];
	uint32_t custom_block_mask;
	uint32_t reserved;
	struct powervr_s6_fw_counter_pick custom[4];
};

/* Where a guest system's command buffers are, registered with the firmware. */
struct powervr_s6_fw_guest_queues {
	uint32_t guest;
	uint32_t reserved;
	uint32_t kernel_queue_control;
	uint32_t kernel_queue;
	uint32_t firmware_queue_control;
	uint32_t firmware_queue;
};

/* The firmware's record of one parameter manager free list. */
struct powervr_s6_fw_free_list {
	uint64_t list;
	uint64_t current;
	uint32_t stack_top;
	uint32_t max_pages;
	uint32_t grow_pages;
	uint32_t current_pages;
	uint32_t allocated_pages;
	uint32_t allocated_table_pages;
	uint32_t hwr_counter;
	uint32_t identity;
	uint32_t grow_pending;
	uint32_t reserved;
};

/* The firmware's record of one render target. */
struct powervr_s6_fw_render_target {
	uint64_t vheap_table;
	uint32_t zero_caches;
	uint32_t reserved;
};

/*
 * The firmware's record of one render target data set: the parameter
 * manager's lists and catalogues for one frame, its free lists, and the
 * tiling state of the frame.
 */
struct powervr_s6_fw_target_data {
	uint32_t state;
	uint32_t partial_renders;
	uint64_t mlist;
	uint64_t vce_catalogue;
	uint64_t vce_last_catalogue;
	uint64_t te_catalogue;
	uint64_t te_last_catalogue;
	uint64_t alist_catalogue;
	uint64_t alist_last_catalogue;
	uint64_t alist_stack;
	uint32_t mlist_stack;
	uint32_t reserved_0;
	uint32_t free_lists[2];
	uint32_t free_list_snapshot[2];
	uint32_t parent_target;
	uint32_t reserved_1;
	struct powervr_s6_fw_cleanup_counts geometry_cleanup;
	struct powervr_s6_fw_cleanup_counts fragment_cleanup;
	uint32_t cleanup_status;
	uint32_t array_control;
	uint32_t has_last_geometry;
	uint32_t partial_rendered;
	uint32_t screen;
	uint32_t grid_offset;
	uint64_t multisample;
	uint32_t tail_stride;
	uint32_t reserved_2;
	uint64_t tail_pointers;
	uint32_t tail_bytes;
	uint32_t tiling_screen;
	uint32_t mtile_stride;
	uint32_t tiling_aa;
	uint32_t mtile_1;
	uint32_t mtile_2;
	uint32_t merge_lower_x;
	uint32_t merge_lower_y;
	uint32_t merge_upper_x;
	uint32_t merge_upper_y;
	uint32_t merge_scale_x;
	uint32_t merge_scale_y;
};

/*
 * The register values of a geometry command, in the order the firmware
 * programs them: the macrotile array, the primitive set-up control and
 * w clamp, the tiling engine's control and region header base, the
 * vertex data master's stream base, stream end and sync data, the tail
 * pointer cache, and the texture unit's border colours, YUV
 * coefficients, control and arrays.
 */
struct powervr_s6_fw_geometry_registers {
	uint64_t mtile_array;
	uint64_t primitive_control;
	uint64_t w_clamp;
	uint64_t tiling_control;
	uint64_t region_headers;
	uint64_t stream_base;
	uint64_t stream_end;
	uint64_t sync_data;
	uint64_t tail_cache;
	uint64_t border_colours;
	uint64_t yuv_coefficients;
	uint64_t texture_control;
	uint64_t texture_arrays;
};

/* The store and resume tasks of one of the two geometry state buffers. */
struct powervr_s6_fw_geometry_tasks {
	uint64_t store[3];
	uint64_t resume[3];
};

/* What a geometry context switch needs: the state bases and the two buffers' tasks. */
struct powervr_s6_fw_geometry_switch {
	uint64_t vdm_state;
	uint64_t ta_state;
	struct powervr_s6_fw_geometry_tasks buffers[2];
};

/*
 * A geometry command.
 *
 * The partial render fences hold back a partial render until the
 * fragment side and the hardware are ready.  The last register is the
 * region header base the firmware reprograms for erratum 44455.
 */
struct powervr_s6_fw_geometry_command {
	struct powervr_s6_fw_geometry_registers registers;
	struct powervr_s6_fw_geometry_switch context_switch;
	uint32_t flags;
	uint32_t frame;
	uint32_t target_data;
	uint32_t depth_buffer;
	uint32_t stencil_buffer;
	struct powervr_s6_fw_sync partial_render_fence;
	struct powervr_s6_fw_sync partial_render_hw_fence;
	uint32_t deadline_ms;
	uint64_t erratum_region_headers;
};

/*
 * The register values of a fragment command: the pixel output and the
 * eight clear registers, the image synthesis set-up (macrotiles, grid,
 * multisampling, scissor and depth bias tables, background object,
 * anti-aliasing, occlusion queries), the depth and stencil load and
 * store, the region base and control, the parameter manager's fragment
 * stack, the end of tile program, the pixel back end's three words for
 * each of eight targets, the texture unit, the background program and
 * the partial render's background program, and one fix-up register.
 */
struct powervr_s6_fw_fragment_registers {
	uint64_t pixel_output;
	uint64_t clear[8];
	uint64_t mtile_base;
	uint64_t mtile_size;
	uint64_t grid_offset;
	uint64_t multisample;
	uint64_t scissor_base;
	uint64_t depth_bias_base;
	uint64_t background_depth;
	uint64_t background_values;
	uint64_t anti_alias;
	uint64_t occlusion_base;
	uint64_t zls_control;
	uint64_t mask_load_base;
	uint64_t mask_store_base;
	uint64_t depth_load_base;
	uint64_t depth_store_base;
	uint64_t stencil_load_base;
	uint64_t stencil_store_base;
	uint64_t zls_pixels;
	uint64_t region_base;
	uint64_t isp_control;
	uint64_t zls_compression;
	uint64_t fragment_stack_base;
	uint64_t fragment_stack;
	uint64_t end_of_tile_code;
	uint64_t end_of_tile_data;
	uint64_t end_of_tile_info;
	uint64_t back_end[8][3];
	uint64_t back_end_control;
	uint64_t border_colours;
	uint64_t yuv_coefficients;
	uint64_t texture_control;
	uint64_t texture_arrays;
	uint64_t texture_madd_control;
	uint64_t background[4];
	uint64_t partial_background[4];
	uint64_t fix_up;
};

/* A fragment command. */
struct powervr_s6_fw_fragment_command {
	struct powervr_s6_fw_fragment_registers registers;
	uint32_t flags;
	uint32_t frame;
	uint32_t target_data;
	uint32_t depth_buffer;
	uint32_t stencil_buffer;
	uint32_t depth_stride;
	uint32_t stencil_stride;
	uint32_t deadline_ms;
};

/*
 * The register values of a 3D transfer: the background object, the
 * pixel output and one clear register, the background program, the
 * texture unit, the render set-up (macrotiles, pixel base, origin,
 * control, anti-aliasing), the end of tile program and the pixel back
 * end's nine words.
 */
struct powervr_s6_fw_transfer_registers {
	uint64_t background_values;
	uint64_t background_depth;
	uint64_t pixel_output;
	uint64_t clear;
	uint64_t background[4];
	uint64_t border_colours;
	uint64_t yuv_coefficients;
	uint64_t isp_render;
	uint64_t mtile_size;
	uint64_t mtile_base;
	uint64_t pixel_base;
	uint64_t render_origin;
	uint64_t isp_control;
	uint64_t anti_alias;
	uint64_t end_of_tile_info;
	uint64_t end_of_tile_code;
	uint64_t end_of_tile_data;
	uint64_t back_end[9];
};

/* A 3D transfer command. */
struct powervr_s6_fw_transfer_command {
	struct powervr_s6_fw_transfer_registers registers;
	uint32_t frame;
	uint32_t flags;
};

/* The register values of a 2D command: its stream and the compressed source and destination. */
struct powervr_s6_fw_2d_registers {
	uint64_t stream;
	uint64_t source_region;
	uint64_t source_control;
	uint64_t target_region;
	uint64_t target_control;
};

/* A 2D command. */
struct powervr_s6_fw_2d_command {
	struct powervr_s6_fw_2d_registers registers;
	uint32_t frame;
	uint32_t flags;
};

/*
 * The register values of a compute command: the stream base, the
 * texture unit's border colours, the thread barrier, the data
 * sequencer's control, and the texture unit's YUV coefficients, control
 * and arrays.
 */
struct powervr_s6_fw_compute_registers {
	uint64_t stream_base;
	uint64_t border_colours;
	uint64_t thread_barrier;
	uint64_t pds_control;
	uint64_t yuv_coefficients;
	uint64_t texture_control;
	uint64_t texture_arrays;
};

/* What a compute context switch needs: the state base and the store, end and resume programs. */
struct powervr_s6_fw_compute_switch {
	uint64_t state_base;
	uint64_t store[2];
	uint64_t terminate[2];
	uint64_t resume[2];
};

/*
 * A compute command.
 *
 * Stream_end is the stream end register the firmware programs for
 * erratum 54441.
 */
struct powervr_s6_fw_compute_command {
	struct powervr_s6_fw_compute_registers registers;
	struct powervr_s6_fw_compute_switch context_switch;
	uint64_t stream_end;
	uint32_t flags;
	uint32_t deadline_ms;
};

/*
 * The fixed layouts.
 *
 * Values referenced:
 * Android kernel/x86_64 android-x86_64-fugu-3.10-nougat-mr2 999603b9ab
 * drivers/staging/imgtec/rogue/rgx_fwif_shared.h (RGXFWIF_UFO,
 * RGXFWIF_CCB_CMD_HEADER, RGXFWIF_CCCB_CTL, RGXFWIF_CLEANUP_CTL,
 * RGXFWIF_FREELIST, RGXFWIF_RENDER_TARGET, RGXFWIF_HWRTDATA,
 * RGXFWIF_COMPCHECKS_BVNC, RGXFWIF_COMPCHECKS), rgx_fwif_km.h
 * (RGXFWIF_CCB_CTL, RGXFWIF_KCCB_CMD and its data, RGXFWIF_FWCCB_CMD,
 * RGXFWIF_FWCOMMONCONTEXT, RGXFWIF_FWRENDERCONTEXT, RGXFWIF_FWMEMCONTEXT,
 * RGXFWIF_SIGBUF_CTL, RGXFWIF_INIT), rgx_fwif.h (RGXFWIF_ASSERTBUF,
 * RGXFWIF_TRACEBUF_SPACE, RGXFWIF_TRACEBUF), rgx_fwif_hwperf.h
 * (RGXFWIF_HWPERF_CTL_BLK, RGXFWIF_HWPERF_CTL), rgx_fwif_client.h
 * (RGXFWIF_TAREGISTERS, RGXFWIF_TAREGISTERS_CSWITCH, RGXFWIF_CMDTA,
 * RGXFWIF_3DREGISTERS, RGXFWIF_CMD3D, RGXFWIF_TRANSFERREGISTERS,
 * RGXFWIF_CMDTRANSFER, RGXFWIF_2DREGISTERS, RGXFWIF_CMD2D,
 * RGXFWIF_CDM_REGISTERS, RGXFWIF_CDM_REGISTERS_CSWITCH,
 * RGXFWIF_CMD_COMPUTE) with config_kernel.h, rgxcore_km_1.72.4.12.h and
 * rgxconfig_km_1.V.4.12.h: the sizes and offsets read from those headers
 * compiled for this core, which match the alignment tables of
 * rgx_fwif_alignchecks_km.h and rgx_fwif_alignchecks.h.
 */
_Static_assert(sizeof(struct powervr_s6_fw_sync) == 8, "sync size");
_Static_assert(sizeof(struct powervr_s6_fw_command_head) == 16, "command head size");
_Static_assert(sizeof(struct powervr_s6_fw_client_control) == 16, "client control size");
_Static_assert(sizeof(struct powervr_s6_fw_queue_control) == 16, "queue control size");
_Static_assert(offsetof(struct powervr_s6_fw_queue_control, command_bytes) == 12, "queue control: command bytes");
_Static_assert(sizeof(struct powervr_s6_fw_kernel_command) == 48, "kernel command size");
_Static_assert(offsetof(struct powervr_s6_fw_kernel_command, data) == 8, "kernel command: data");
_Static_assert(offsetof(struct powervr_s6_fw_kernel_command, data.kick.cleanup) == 20, "kick: cleanup");
_Static_assert(offsetof(struct powervr_s6_fw_kernel_command, data.slc_flush.data_master) == 20, "slc flush: data master");
_Static_assert(offsetof(struct powervr_s6_fw_kernel_command, data.cleanup.sync) == 16, "cleanup: sync");
_Static_assert(sizeof(struct powervr_s6_fw_firmware_command) == 32, "firmware command size");
_Static_assert(sizeof(struct powervr_s6_fw_compat_core) == 16, "compat core size");
_Static_assert(sizeof(struct powervr_s6_fw_compat) == 56, "compat size");
_Static_assert(offsetof(struct powervr_s6_fw_compat, updated) == 48, "compat: updated");
_Static_assert(sizeof(struct powervr_s6_fw_init) == 400, "init size");
_Static_assert(offsetof(struct powervr_s6_fw_init, fault_page) == 0, "init: fault page");
_Static_assert(offsetof(struct powervr_s6_fw_init, pds_code_base) == 8, "init: pds base");
_Static_assert(offsetof(struct powervr_s6_fw_init, usc_code_base) == 16, "init: usc base");
_Static_assert(offsetof(struct powervr_s6_fw_init, kernel_queue_control) == 76, "init: kernel queue control");
_Static_assert(offsetof(struct powervr_s6_fw_init, kernel_queue) == 80, "init: kernel queue");
_Static_assert(offsetof(struct powervr_s6_fw_init, firmware_queue_control) == 84, "init: firmware queue control");
_Static_assert(offsetof(struct powervr_s6_fw_init, firmware_queue) == 88, "init: firmware queue");
_Static_assert(offsetof(struct powervr_s6_fw_init, signature_buffers) == 96, "init: signature buffers");
_Static_assert(offsetof(struct powervr_s6_fw_init, tiling) == 160, "init: tiling");
_Static_assert(offsetof(struct powervr_s6_fw_init, trace_buffer) == 260, "init: trace buffer");
_Static_assert(offsetof(struct powervr_s6_fw_init, hwperf_filter) == 264, "init: hwperf filter");
_Static_assert(offsetof(struct powervr_s6_fw_init, alignment_table) == 288, "init: alignment table");
_Static_assert(offsetof(struct powervr_s6_fw_init, core_clock_hz) == 292, "init: core clock");
_Static_assert(offsetof(struct powervr_s6_fw_init, started) == 300, "init: started");
_Static_assert(offsetof(struct powervr_s6_fw_init, compat) == 320, "init: compat");
_Static_assert(offsetof(struct powervr_s6_fw_init, core_memory_data) == 376, "init: core memory data");
_Static_assert(offsetof(struct powervr_s6_fw_init, thread1_stack) == 396, "init: thread 1 stack");
_Static_assert(sizeof(struct powervr_s6_fw_common_context) == 96, "common context size");
_Static_assert(offsetof(struct powervr_s6_fw_common_context, client_queue) == 4, "common context: queue");
_Static_assert(offsetof(struct powervr_s6_fw_common_context, run_node) == 16, "common context: run node");
_Static_assert(offsetof(struct powervr_s6_fw_common_context, memory_context) == 24, "common context: memory context");
_Static_assert(offsetof(struct powervr_s6_fw_common_context, context_state) == 32, "common context: state");
_Static_assert(offsetof(struct powervr_s6_fw_common_context, fence_address) == 56, "common context: fence");
_Static_assert(offsetof(struct powervr_s6_fw_common_context, data_master) == 88, "common context: data master");
_Static_assert(sizeof(struct powervr_s6_fw_render_context) == 200, "render context size");
_Static_assert(offsetof(struct powervr_s6_fw_render_context, fragment) == 96, "render context: fragment");
_Static_assert(sizeof(struct powervr_s6_fw_memory_context) == 24, "memory context size");
_Static_assert(sizeof(struct powervr_s6_fw_thread_log) == 424, "thread log size");
_Static_assert(sizeof(struct powervr_s6_fw_trace) == 632, "trace size");
_Static_assert(offsetof(struct powervr_s6_fw_trace, thread) == 8, "trace: thread log");
_Static_assert(offsetof(struct powervr_s6_fw_trace, locked_up) == 432, "trace: locked up");
_Static_assert(offsetof(struct powervr_s6_fw_trace, overran) == 442, "trace: overran");
_Static_assert(offsetof(struct powervr_s6_fw_trace, recovered) == 452, "trace: recovered");
_Static_assert(offsetof(struct powervr_s6_fw_trace, false_detect) == 462, "trace: false detect");
_Static_assert(offsetof(struct powervr_s6_fw_trace, idle_since) == 552, "trace: idle since");
_Static_assert(offsetof(struct powervr_s6_fw_trace, thread1_pc_write) == 628, "trace: pc write");
_Static_assert(sizeof(struct powervr_s6_fw_counter_block) == 48, "counter block size");
_Static_assert(sizeof(struct powervr_s6_fw_counters) == 928, "counters size");
_Static_assert(offsetof(struct powervr_s6_fw_counters, custom) == 784, "counters: custom");
_Static_assert(sizeof(struct powervr_s6_fw_guest_queues) == 24, "guest queues size");
_Static_assert(sizeof(struct powervr_s6_fw_free_list) == 56, "free list size");
_Static_assert(offsetof(struct powervr_s6_fw_free_list, max_pages) == 20, "free list: max pages");
_Static_assert(offsetof(struct powervr_s6_fw_free_list, current_pages) == 28, "free list: current pages");
_Static_assert(offsetof(struct powervr_s6_fw_free_list, hwr_counter) == 40, "free list: hwr counter");
_Static_assert(sizeof(struct powervr_s6_fw_render_target) == 16, "render target size");
_Static_assert(sizeof(struct powervr_s6_fw_target_data) == 216, "target data size");
_Static_assert(offsetof(struct powervr_s6_fw_target_data, mlist) == 8, "target data: mlist");
_Static_assert(offsetof(struct powervr_s6_fw_target_data, vce_catalogue) == 16, "target data: vce catalogue");
_Static_assert(offsetof(struct powervr_s6_fw_target_data, free_lists) == 80, "target data: free lists");
_Static_assert(offsetof(struct powervr_s6_fw_target_data, parent_target) == 96, "target data: parent");
_Static_assert(offsetof(struct powervr_s6_fw_target_data, multisample) == 144, "target data: multisample");
_Static_assert(offsetof(struct powervr_s6_fw_target_data, tail_pointers) == 160, "target data: tail pointers");
_Static_assert(offsetof(struct powervr_s6_fw_target_data, merge_scale_y) == 212, "target data: merge scale");
_Static_assert(sizeof(struct powervr_s6_fw_geometry_registers) == 104, "geometry registers size");
_Static_assert(sizeof(struct powervr_s6_fw_geometry_switch) == 112, "geometry switch size");
_Static_assert(sizeof(struct powervr_s6_fw_geometry_command) == 264, "geometry command size");
_Static_assert(offsetof(struct powervr_s6_fw_geometry_command, context_switch) == 104, "geometry: switch");
_Static_assert(offsetof(struct powervr_s6_fw_geometry_command, flags) == 216, "geometry: flags");
_Static_assert(offsetof(struct powervr_s6_fw_geometry_command, target_data) == 224, "geometry: target data");
_Static_assert(offsetof(struct powervr_s6_fw_geometry_command, partial_render_fence) == 236, "geometry: partial render fence");
_Static_assert(offsetof(struct powervr_s6_fw_geometry_command, deadline_ms) == 252, "geometry: deadline");
_Static_assert(offsetof(struct powervr_s6_fw_geometry_command, erratum_region_headers) == 256, "geometry: erratum register");
_Static_assert(sizeof(struct powervr_s6_fw_fragment_registers) == 592, "fragment registers size");
_Static_assert(offsetof(struct powervr_s6_fw_fragment_registers, mtile_base) == 72, "fragment: mtile base");
_Static_assert(offsetof(struct powervr_s6_fw_fragment_registers, back_end) == 280, "fragment: back end");
_Static_assert(offsetof(struct powervr_s6_fw_fragment_registers, background) == 520, "fragment: background");
_Static_assert(offsetof(struct powervr_s6_fw_fragment_registers, fix_up) == 584, "fragment: fix up");
_Static_assert(sizeof(struct powervr_s6_fw_fragment_command) == 624, "fragment command size");
_Static_assert(offsetof(struct powervr_s6_fw_fragment_command, deadline_ms) == 620, "fragment: deadline");
_Static_assert(sizeof(struct powervr_s6_fw_transfer_registers) == 232, "transfer registers size");
_Static_assert(offsetof(struct powervr_s6_fw_transfer_registers, back_end) == 160, "transfer: back end");
_Static_assert(sizeof(struct powervr_s6_fw_transfer_command) == 240, "transfer command size");
_Static_assert(sizeof(struct powervr_s6_fw_2d_command) == 48, "2d command size");
_Static_assert(sizeof(struct powervr_s6_fw_compute_registers) == 56, "compute registers size");
_Static_assert(sizeof(struct powervr_s6_fw_compute_switch) == 56, "compute switch size");
_Static_assert(sizeof(struct powervr_s6_fw_compute_command) == 128, "compute command size");
_Static_assert(offsetof(struct powervr_s6_fw_compute_command, stream_end) == 112, "compute: stream end");
_Static_assert(offsetof(struct powervr_s6_fw_compute_command, flags) == 120, "compute: flags");

/*
 * Builds the alignment table the host hands the firmware.
 *
 * Writes POWERVR_S6_ALIGN_WORDS words from this file's layouts: the
 * kernel's part, then the client's.  Returns the count, or zero when the
 * capacity is short.
 */
size_t powervr_s6_fw_alignment_table(uint32_t *words, size_t capacity);

/*
 * Compares two alignment tables.
 *
 * Returns the index of the first word that differs, or count when the
 * two agree.
 */
size_t powervr_s6_fw_alignment_compare(const uint32_t *left, const uint32_t *right, size_t count);

#endif
