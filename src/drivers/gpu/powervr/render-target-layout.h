/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * What a render target costs the PowerVR core and what the firmware is
 * told of it: the sizes of the parameter manager's memory for one frame
 * size, the register values the firmware programs for it, the records of
 * a free list and of a render target data set, and the kernel commands
 * that answer the firmware's free list requests.  This part only
 * computes; it holds no lock and no memory, and it builds on the host
 * too.
 */

#ifndef KERN_DRIVERS_GPU_POWERVR_RENDER_TARGET_LAYOUT_H
#define KERN_DRIVERS_GPU_POWERVR_RENDER_TARGET_LAYOUT_H

#include <stddef.h>
#include <stdint.h>

/*
 * The parameter manager's page, the step a free list's size is a multiple
 * of, and the free lists every render target takes.
 *
 * The step is the larger of the free list's base alignment (16 bytes)
 * and the system level cache's line (64 bytes), counted in 4-byte
 * entries, one page each: 16 pages.  The local list is the core's
 * minimum of 25 pages rounded up to that step; the global list is one
 * fixed 16 MiB pool that never grows.
 *
 * Values referenced:
 * Linux v7.3 drivers/gpu/drm/imagination/pvr_rogue_defs.h
 * (ROGUE_BIF_PM_PHYSICAL_PAGE_ALIGNSHIFT 12,
 * ROGUE_BIF_PM_FREELIST_BASE_ADDR_ALIGNSIZE 16); pvr_free_list.c
 * (FREE_LIST_MIN_PAGES_ROGUEXE 25); Mesa 26.2.5
 * src/imagination/common/device_info/bxe-4-32.h
 * (slc_cache_line_size_bits 512, roguexe, no quirk 66011).
 */
#define POWERVR_PM_PAGE_SHIFT		12U
#define POWERVR_PM_PAGE_BYTES		0x1000U
#define POWERVR_FREE_LIST_STEP		16U
#define POWERVR_FREE_LIST_MIN_PAGES	25U
#define POWERVR_FREE_LIST_LOCAL_PAGES	32U
#define POWERVR_FREE_LIST_GLOBAL_PAGES	4096U
#define POWERVR_FREE_LIST_ENTRY_BYTES	4U

/* The largest edge of a render target, in pixels, and the most layers this driver gives one. */
#define POWERVR_TARGET_EDGE_MAX		8192U
#define POWERVR_TARGET_LAYERS_MAX	1U

/* The words of one kernel or firmware command. */
#define POWERVR_TARGET_COMMAND_WORDS	22U

/*
 * The most free lists one rebuild request names.
 *
 * Values referenced:
 * Linux v7.3 drivers/gpu/drm/imagination/pvr_rogue_fwif.h
 * (ROGUE_FWIF_MAX_FREELISTS_TO_RECONSTRUCT: 3 contexts x 3 lists x 2).
 */
#define POWERVR_REBUILD_LISTS_MAX	18U

/*
 * The states and flags of a render target data set the host changes
 * after a recovery: the recovered state, so the next geometry is not
 * taken for a first one, and the "last geometry seen" flag cleared.
 *
 * Values referenced:
 * Linux v7.3 drivers/gpu/drm/imagination/pvr_rogue_fwif.h
 * (ROGUE_FWIF_RTDATA_STATE_HWR 9, HWRTDATA_HAS_LAST_GEOM bit 2).
 */
#define POWERVR_TARGET_STATE_RECOVERED	9U
#define POWERVR_TARGET_LAST_GEOMETRY	0x4U

/*
 * The firmware's record of one free list.
 *
 * Values referenced:
 * Linux v7.3 drivers/gpu/drm/imagination/pvr_rogue_fwif_check.h
 * (struct rogue_fwif_freelist, 64 bytes, and the offset of each field).
 */
struct powervr_fw_free_list {
	uint64_t list_address;
	uint64_t top_address;
	uint32_t stack_top;
	uint32_t max_pages;
	uint32_t grow_pages;
	uint32_t current_pages;
	uint32_t allocated_pages;
	uint32_t allocated_table_pages;
	uint32_t identity;
	uint32_t grow_pending;
	uint32_t ready_pages;
	uint32_t flags;
	uint32_t global_buffer;
	uint32_t reserved;
};

_Static_assert(offsetof(struct powervr_fw_free_list, list_address) == 0, "free list: list address");
_Static_assert(offsetof(struct powervr_fw_free_list, top_address) == 8, "free list: top address");
_Static_assert(offsetof(struct powervr_fw_free_list, stack_top) == 16, "free list: stack top");
_Static_assert(offsetof(struct powervr_fw_free_list, max_pages) == 20, "free list: max pages");
_Static_assert(offsetof(struct powervr_fw_free_list, grow_pages) == 24, "free list: grow pages");
_Static_assert(offsetof(struct powervr_fw_free_list, current_pages) == 28, "free list: current pages");
_Static_assert(offsetof(struct powervr_fw_free_list, allocated_pages) == 32, "free list: allocated pages");
_Static_assert(offsetof(struct powervr_fw_free_list, allocated_table_pages) == 36, "free list: allocated table pages");
_Static_assert(offsetof(struct powervr_fw_free_list, identity) == 40, "free list: identity");
_Static_assert(offsetof(struct powervr_fw_free_list, grow_pending) == 44, "free list: grow pending");
_Static_assert(offsetof(struct powervr_fw_free_list, ready_pages) == 48, "free list: ready pages");
_Static_assert(offsetof(struct powervr_fw_free_list, flags) == 52, "free list: flags");
_Static_assert(offsetof(struct powervr_fw_free_list, global_buffer) == 56, "free list: global buffer");
_Static_assert(sizeof(struct powervr_fw_free_list) == 64, "free list: size");

/*
 * The firmware's record of what both halves of a render target data set
 * share: the screen, the macrotiles and the tail pointer cache.
 *
 * Values referenced:
 * Linux v7.3 drivers/gpu/drm/imagination/pvr_rogue_fwif_check.h
 * (struct rogue_fwif_hwrtdata_common, 88 bytes, and the offset of each
 * field).
 */
struct powervr_fw_target_common {
	uint32_t clear_geometry_caches;
	uint32_t screen_pixel_max;
	uint64_t multisample;
	uint64_t flipped_multisample;
	uint32_t tail_stride;
	uint32_t tail_bytes;
	uint32_t te_screen;
	uint32_t macrotile_stride;
	uint32_t te_antialias;
	uint32_t te_macrotile_x;
	uint32_t te_macrotile_y;
	uint32_t merge_lower_x;
	uint32_t merge_lower_y;
	uint32_t merge_upper_x;
	uint32_t merge_upper_y;
	uint32_t merge_scale_x;
	uint32_t merge_scale_y;
	uint32_t region_headers;
	uint32_t isp_macrotile_size;
	uint32_t reserved;
};

_Static_assert(offsetof(struct powervr_fw_target_common, screen_pixel_max) == 4, "target common: screen");
_Static_assert(offsetof(struct powervr_fw_target_common, multisample) == 8, "target common: multisample");
_Static_assert(offsetof(struct powervr_fw_target_common, flipped_multisample) == 16, "target common: flipped multisample");
_Static_assert(offsetof(struct powervr_fw_target_common, tail_stride) == 24, "target common: tail stride");
_Static_assert(offsetof(struct powervr_fw_target_common, tail_bytes) == 28, "target common: tail bytes");
_Static_assert(offsetof(struct powervr_fw_target_common, te_screen) == 32, "target common: te screen");
_Static_assert(offsetof(struct powervr_fw_target_common, macrotile_stride) == 36, "target common: macrotile stride");
_Static_assert(offsetof(struct powervr_fw_target_common, te_antialias) == 40, "target common: te antialias");
_Static_assert(offsetof(struct powervr_fw_target_common, te_macrotile_x) == 44, "target common: te macrotile x");
_Static_assert(offsetof(struct powervr_fw_target_common, te_macrotile_y) == 48, "target common: te macrotile y");
_Static_assert(offsetof(struct powervr_fw_target_common, merge_lower_x) == 52, "target common: merge lower x");
_Static_assert(offsetof(struct powervr_fw_target_common, merge_scale_y) == 72, "target common: merge scale y");
_Static_assert(offsetof(struct powervr_fw_target_common, region_headers) == 76, "target common: region headers");
_Static_assert(offsetof(struct powervr_fw_target_common, isp_macrotile_size) == 80, "target common: isp macrotile size");
_Static_assert(sizeof(struct powervr_fw_target_common) == 88, "target common: size");

/*
 * The firmware's record of a render target array's progress inside one
 * data set.
 *
 * Values referenced:
 * Linux v7.3 drivers/gpu/drm/imagination/pvr_rogue_fwif_check.h
 * (struct rogue_fwif_rta_ctl, 32 bytes, and the offset of each field).
 */
struct powervr_fw_target_array {
	uint32_t render_index;
	uint32_t current_target;
	uint32_t active_targets;
	uint32_t active_since_geometry;
	uint32_t valid_targets;
	uint32_t partial_renders;
	uint32_t max_targets;
	uint32_t flags;
};

_Static_assert(offsetof(struct powervr_fw_target_array, valid_targets) == 16, "target array: valid targets");
_Static_assert(offsetof(struct powervr_fw_target_array, partial_renders) == 20, "target array: partial renders");
_Static_assert(offsetof(struct powervr_fw_target_array, max_targets) == 24, "target array: max targets");
_Static_assert(sizeof(struct powervr_fw_target_array) == 32, "target array: size");

/*
 * The count the firmware keeps of a cleanup's commands, which a kick
 * names so a later cleanup knows whether the object is still in use.
 *
 * Values referenced:
 * Linux v7.3 drivers/gpu/drm/imagination/pvr_rogue_fwif_shared.h
 * (struct rogue_fwif_cleanup_ctl: commands submitted, commands
 * executed, aligned to 8).
 */
struct powervr_fw_cleanup_count {
	uint32_t submitted;
	uint32_t executed;
};

/*
 * The firmware's record of one half of a render target data set: the
 * parameter manager's lists, the free lists, the heaps of the tiling
 * engine and the cleanup count the kicks name.
 *
 * Values referenced:
 * Linux v7.3 drivers/gpu/drm/imagination/pvr_rogue_fwif_check.h
 * (struct rogue_fwif_hwrtdata, 384 bytes, and the offset of each field,
 * cleanup_state at 320); pvr_rogue_fwif.h (MAX_FREELISTS_SIZE 3).
 */
struct powervr_fw_target_data {
	uint64_t macrolist_address;
	uint64_t vce_catalogue[4];
	uint64_t vce_last_catalogue[4];
	uint64_t te_catalogue[4];
	uint64_t te_last_catalogue[4];
	uint64_t alist_catalogue;
	uint64_t alist_last_catalogue;
	uint64_t alist_stack_pointer;
	uint32_t macrolist_stack_pointer;
	uint32_t common_address;
	uint32_t flags;
	uint32_t state;
	uint32_t free_lists[3];
	uint32_t free_list_snapshot[3];
	uint64_t vertex_heap_address;
	struct powervr_fw_target_array array;
	uint64_t tail_address;
	uint64_t macrotile_array_address;
	uint64_t region_header_address;
	uint64_t render_target_cache_address;
	uint32_t owner_geometry;
	uint32_t clear_geometry_caches;
	uint8_t reserved[40];
	struct powervr_fw_cleanup_count cleanup;
	uint8_t tail_reserved[56];
};

_Static_assert(offsetof(struct powervr_fw_target_data, vce_catalogue) == 8, "target data: vce catalogue");
_Static_assert(offsetof(struct powervr_fw_target_data, alist_catalogue) == 136, "target data: alist catalogue");
_Static_assert(offsetof(struct powervr_fw_target_data, alist_stack_pointer) == 152, "target data: alist stack");
_Static_assert(offsetof(struct powervr_fw_target_data, macrolist_stack_pointer) == 160, "target data: macrolist stack");
_Static_assert(offsetof(struct powervr_fw_target_data, common_address) == 164, "target data: common");
_Static_assert(offsetof(struct powervr_fw_target_data, flags) == 168, "target data: flags");
_Static_assert(offsetof(struct powervr_fw_target_data, state) == 172, "target data: state");
_Static_assert(offsetof(struct powervr_fw_target_data, free_lists) == 176, "target data: free lists");
_Static_assert(offsetof(struct powervr_fw_target_data, free_list_snapshot) == 188, "target data: free list snapshot");
_Static_assert(offsetof(struct powervr_fw_target_data, vertex_heap_address) == 200, "target data: vertex heap");
_Static_assert(offsetof(struct powervr_fw_target_data, array) == 208, "target data: array control");
_Static_assert(offsetof(struct powervr_fw_target_data, tail_address) == 240, "target data: tail pointers");
_Static_assert(offsetof(struct powervr_fw_target_data, macrotile_array_address) == 248, "target data: macrotile array");
_Static_assert(offsetof(struct powervr_fw_target_data, region_header_address) == 256, "target data: region headers");
_Static_assert(offsetof(struct powervr_fw_target_data, render_target_cache_address) == 264, "target data: render target cache");
_Static_assert(offsetof(struct powervr_fw_target_data, owner_geometry) == 272, "target data: owner geometry");
_Static_assert(offsetof(struct powervr_fw_target_data, clear_geometry_caches) == 276, "target data: clear caches");
_Static_assert(offsetof(struct powervr_fw_target_data, cleanup) == 320, "target data: cleanup count");
_Static_assert(sizeof(struct powervr_fw_target_data) == 384, "target data: size");

/*
 * Where the records of one render target lie in its one firmware block:
 * the two halves first (each on a 64-byte boundary, as the cleanup count
 * in it must be), then what they share.
 */
#define POWERVR_TARGET_DATA_OFFSET(half)	((uint32_t)(half) * (uint32_t)sizeof(struct powervr_fw_target_data))
#define POWERVR_TARGET_COMMON_OFFSET		(2U * (uint32_t)sizeof(struct powervr_fw_target_data))
#define POWERVR_TARGET_RECORD_BYTES		(POWERVR_TARGET_COMMON_OFFSET + (uint32_t)sizeof(struct powervr_fw_target_common))

/* The frame size and samples a render target is made for. */
struct powervr_target_shape {
	uint32_t width;
	uint32_t height;
	uint32_t samples;
	uint32_t layers;
};

/*
 * What one render target needs: the bytes of each of its memories (one
 * half's macrolist and region headers; the vertex heap table and the tail
 * pointer cache shared by both halves), and the register values of its
 * shared record.
 */
struct powervr_target_layout {
	uint32_t tiles_x;
	uint32_t tiles_y;
	uint32_t macrotile_x;
	uint32_t macrotile_y;
	uint64_t macrolist_bytes;
	uint64_t region_header_bytes;
	uint64_t vertex_heap_bytes;
	uint64_t tail_bytes;
	uint32_t tail_used_bytes;
	uint32_t region_headers;
	uint32_t screen_pixel_max;
	uint32_t te_screen;
	uint32_t te_macrotile_x;
	uint32_t te_macrotile_y;
	uint32_t macrotile_stride;
	uint32_t isp_macrotile_size;
	uint64_t multisample;
	uint64_t flipped_multisample;
	uint32_t te_antialias;
	uint32_t samples;
};

/* The device addresses and firmware addresses one half of a render target is told. */
struct powervr_target_places {
	uint64_t macrolist;
	uint64_t region_headers;
	uint64_t vertex_heap;
	uint64_t tail;
	uint32_t common;
	uint32_t local_list;
	uint32_t global_list;
};

/*
 * Computes what a render target of a shape needs; EINVAL for a shape this
 * driver does not make (an edge of 0 or above the limit, a sample count
 * other than 1 or 4, more than one layer).
 */
int powervr_target_lay_out(const struct powervr_target_shape *shape, uint32_t local_pages, uint32_t global_pages, struct powervr_target_layout *layout);

/* Fills the shared record of a render target. */
void powervr_target_common_record(const struct powervr_target_layout *layout, struct powervr_fw_target_common *record);

/* Fills the record of one half of a render target. */
void powervr_target_data_record(const struct powervr_target_places *places, struct powervr_fw_target_data *record);

/*
 * Fills the record of a free list of pages pages whose page list is at a
 * device address; the list is full from the start and never grows.
 */
void powervr_free_list_record(uint64_t list_address, uint32_t pages, uint32_t identity, struct powervr_fw_free_list *record);

/* Reports the entry of a free list's page list that names its index-th page of a run starting at physical. */
uint32_t powervr_free_list_entry(uint64_t physical, uint32_t index);

/*
 * Reads the free list a grow request names; EINVAL when the request is
 * not one.
 */
int powervr_grow_request(const uint32_t notice[POWERVR_TARGET_COMMAND_WORDS], uint32_t *identity);

/*
 * Builds the answer to a grow request: no pages added, none held ready,
 * the list's pages unchanged.
 */
void powervr_grow_answer(uint32_t list_address, uint32_t pages, uint32_t command[POWERVR_TARGET_COMMAND_WORDS]);

/*
 * Reads the free lists a rebuild request names, at most
 * POWERVR_REBUILD_LISTS_MAX; EINVAL when the request is not one.
 */
int powervr_rebuild_request(const uint32_t notice[POWERVR_TARGET_COMMAND_WORDS], uint32_t identities[POWERVR_REBUILD_LISTS_MAX], uint32_t *count);

/* Builds the answer to a rebuild request naming the lists that were rebuilt. */
void powervr_rebuild_answer(const uint32_t identities[POWERVR_REBUILD_LISTS_MAX], uint32_t count, uint32_t command[POWERVR_TARGET_COMMAND_WORDS]);

/*
 * Reads which counter a statistics notice raises and by how much; EINVAL
 * when the notice is not one.
 */
int powervr_statistics_notice(const uint32_t notice[POWERVR_TARGET_COMMAND_WORDS], uint32_t *counter, int32_t *adjustment);

/* Builds the command that has the firmware clean up one of its objects of a kind. */
void powervr_cleanup_command(uint32_t kind, uint32_t address, uint32_t command[POWERVR_TARGET_COMMAND_WORDS]);

#endif
