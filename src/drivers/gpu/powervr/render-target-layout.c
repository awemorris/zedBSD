/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The sizes and records of the PowerVR core's render targets and free
 * lists, and the commands that answer the firmware about them.  Nothing
 * here touches the core or memory; the render node (render-target.c)
 * calls it, and the host check builds it unchanged.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <uapi/errno.h>

#include "drivers/gpu/powervr/interface.h"
#include "drivers/gpu/powervr/render-target-layout.h"

/*
 * The tiling of this core: 16 x 16 pixel tiles, one sample per pixel
 * stored, the second simple parameter format (4 x 4 macrotiles, one
 * 6-byte region header per 2 x 2 tile group, no macrotile array).
 *
 * Values referenced:
 * Mesa 26.2.5 src/imagination/common/device_info/bxe-4-32.h
 * (tile_size_x 16, tile_size_y 16, isp_samples_per_pixel 1,
 * simple_internal_parameter_format, simple_parameter_format_version 2);
 * src/imagination/include/hwdef/rogue_hw_utils.h
 * (rogue_get_num_macrotiles_xy, rogue_get_region_header_size,
 * rogue_get_macrotile_array_size).
 */
#define TILE_PIXELS			16U
#define MACROTILES_PER_EDGE		4U
#define TILE_GROUP_EDGE			2U
#define REGION_HEADER_BYTES		6U

/*
 * The parameter manager's tables over the free lists' pages, and the
 * vertex heap table and tail pointer cache of the tiling engine, with
 * the boundaries each memory starts on.
 *
 * Values referenced:
 * Mesa 26.2.5 src/imagination/vulkan/pvr_arch_job_render.c
 * (ROGUE_NUM_PT_ENTRIES_PER_PAGE 0x200, ROGUE_NUM_PD_ENTRIES_PER_PAGE
 * 0x200, ROGUE_NUM_PC_ENTRIES_PER_PAGE 0x400);
 * src/imagination/include/hwdef/rogue_hw_defs.h
 * (ROGUE_NUM_PM_ADDRESS_SPACES 2, ROGUE_MLIST_ENTRY_STRIDE 4,
 * ROGUE_PM_VHEAP_ENTRY_SIZE 4, ROGUE_TAIL_POINTER_SIZE 8,
 * ROGUE_TE_TPC_CACHE_LINE_SIZE 64); src/imagination/csbgen/rogue/cr.xml
 * (PM_VHEAP_TABLE_SIZE 0x180; TE_PSGREGION_ADDR and TE_TPC_ADDR bases
 * shifted by 6).
 */
#define TABLE_ENTRIES_PER_PAGE		0x200U
#define DIRECTORY_ENTRIES_PER_PAGE	0x200U
#define CATALOGUE_ENTRIES_PER_PAGE	0x400U
#define PM_ADDRESS_SPACES		2U
#define MACROLIST_ENTRY_BYTES		4U
#define VERTEX_HEAP_ENTRIES		0x180U
#define VERTEX_HEAP_ENTRY_BYTES		4U
#define TAIL_POINTER_BYTES		8U
#define TAIL_CACHE_LINE			64U
#define REGION_HEADER_ALIGN		64U

/*
 * The fields of the screen and macrotile registers the firmware
 * programs from the shared record, and the sample positions of one and
 * of four samples in sixteenths of a pixel (one sample at the pixel's
 * centre; four at the standard positions), a byte a sample with x in
 * its low half, y in its high half, and 16 minus y when flipped.  With
 * four samples and one sample a pixel in the ISP, the tiling engine
 * doubles its tiles across and down (TE_AA x and y) and the ISP's
 * macrotile is twice as wide and high.
 *
 * Values referenced:
 * Linux v7.3 drivers/gpu/drm/imagination/pvr_rogue_cr_defs_client.h
 * (ROGUE_CR_PPP_SCREEN_PIXXMAX_SHIFT 0, ..._PIXYMAX_SHIFT 16;
 * ROGUE_CR_TE_SCREEN_XMAX_SHIFT 0, ..._YMAX_SHIFT 12;
 * ROGUE_CR_TE_MTILE1_X1_SHIFT 18; ROGUE_CR_TE_MTILE2_Y1_SHIFT 18;
 * ROGUE_CR_ISP_MTILE_SIZE_X_SHIFT 16, ..._Y_SHIFT 0); pvr_hwrt.c
 * (get_cr_multisamplectl_val: positions x 8 / 6, 14, 2, 10 and y 8 /
 * 2, 6, 10, 14, the byte i * 8, y shifted by 4, 16 - y masked when
 * flipped; get_cr_te_aa_val and get_cr_isp_mtile_size_val with one ISP
 * sample a pixel: 4 samples set Y_EN and X_EN and double x and y);
 * Mesa 26.2.6 src/imagination/csbgen/rogue/cr.xml (TE_AA: y 2, x 1);
 * the Vulkan 1.3 specification, "Multisampling" (the standard
 * locations of 4 samples: (0.375, 0.125), (0.875, 0.375),
 * (0.125, 0.625), (0.625, 0.875)).
 */
#define PPP_SCREEN_Y_SHIFT		16U
#define TE_SCREEN_Y_SHIFT		12U
#define TE_MACROTILE_FIRST_SHIFT	18U
#define ISP_MACROTILE_X_SHIFT		16U
#define SAMPLE_Y_SHIFT			4U
#define SAMPLE_EDGE			16U
#define TE_ANTIALIAS_X			(1U << 1)
#define TE_ANTIALIAS_Y			(1U << 2)

/* The sample positions of one sample and of four, x then y (see above). */
static const uint8_t positions_one[2][1] = { { 8 }, { 8 } };
static const uint8_t positions_four[2][4] = { { 6, 14, 2, 10 }, { 2, 6, 10, 14 } };

/* The words of a command before its data: its type and its flags. */
#define COMMAND_HEADER_WORDS		2U

/*
 * Empty records the fillers start from, so a field the host does not
 * name is zero.  Read only.
 */
static const struct powervr_fw_free_list empty_free_list;
static const struct powervr_fw_target_common empty_target_common;
static const struct powervr_fw_target_data empty_target_data;

static uint32_t divide_up(uint32_t value, uint32_t divisor);
static uint64_t align_up(uint64_t value, uint64_t boundary);
static uint32_t next_power_of_two(uint32_t value);
static uint64_t sample_positions(const uint8_t *x, const uint8_t *y, uint32_t count, bool flipped);
static void clear_command(uint32_t command[POWERVR_TARGET_COMMAND_WORDS]);

/*
 * Computes what a render target of a shape needs.
 *
 * The macrotiles cover the screen in 4 x 4, each a whole number of
 * 2 x 2 tile groups; the region headers and the tail pointer cache are
 * sized for those macrotiles, the macrolist for the parameter manager's
 * tables over both free lists' pages.  With four samples the tiles of
 * a macrotile are twice as many across and down (an ISP tile covers 8 x 8
 * pixels), so the region headers and the tail pointer cache are sized
 * for them; the screen's tiles and the tiling engine's macrotiles stay
 * the pixels'.
 *
 * Procedure referenced:
 * Mesa 26.2.5 src/imagination/vulkan/pvr_arch_job_render.c
 * (pvr_arch_rt_mtile_info_init, pvr_rt_get_isp_region_size,
 * pvr_rt_vheap_rtc_data_init, pvr_rt_get_tail_ptr_stride_size,
 * pvr_rt_get_mlist_size, pvr_rt_get_region_headers_stride_size;
 * pvr_get_samples_in_xy: 4 samples 2 x 2, the macrotile's tiles times
 * them);
 * Linux v7.3 drivers/gpu/drm/imagination/pvr_hwrt.c
 * (hwrt_init_common_fw_structure: the screen, macrotile and ISP values).
 */
int
powervr_target_lay_out(
	const struct powervr_target_shape *shape,
	uint32_t local_pages,
	uint32_t global_pages,
	struct powervr_target_layout *layout)
{
	uint32_t tile_max_x;
	uint32_t tile_max_y;
	uint32_t covered_x;
	uint32_t covered_y;
	uint32_t sample_scale;
	uint32_t tail_edge;
	uint32_t tail_edge_y;
	uint32_t table_pages;
	uint32_t directory_pages;
	uint32_t catalogue_pages;
	uint64_t tail_used;

	/* Only one- or four-sample, one-layer targets with edges the screen registers hold. */
	if (shape->width == 0U || shape->width > POWERVR_TARGET_EDGE_MAX)
		return EINVAL;
	if (shape->height == 0U || shape->height > POWERVR_TARGET_EDGE_MAX)
		return EINVAL;
	if (shape->samples != 1U && shape->samples != 4U)
		return EINVAL;
	if (shape->layers != 1U)
		return EINVAL;

	/* The tiles, and the last tile of each edge rounded up to a whole tile group. */
	layout->tiles_x = divide_up(shape->width, TILE_PIXELS);
	layout->tiles_y = divide_up(shape->height, TILE_PIXELS);
	tile_max_x = (uint32_t)align_up(layout->tiles_x, TILE_GROUP_EDGE) - 1U;
	tile_max_y = (uint32_t)align_up(layout->tiles_y, TILE_GROUP_EDGE) - 1U;

	/* One macrotile's edge in tiles: an eighth of the screen's, in whole tile groups. */
	layout->macrotile_x = divide_up(layout->tiles_x, 8U) * TILE_GROUP_EDGE;
	layout->macrotile_y = divide_up(layout->tiles_y, 8U) * TILE_GROUP_EDGE;
	sample_scale = shape->samples == 4U ? 2U : 1U;
	covered_x = layout->macrotile_x * sample_scale * MACROTILES_PER_EDGE;
	covered_y = layout->macrotile_y * sample_scale * MACROTILES_PER_EDGE;

	/* The region headers: one per tile group of the macrotiles, the bytes on the base's boundary. */
	layout->region_headers = covered_x * covered_y / (TILE_GROUP_EDGE * TILE_GROUP_EDGE);
	layout->region_header_bytes = (uint64_t)(covered_x / TILE_GROUP_EDGE) * (covered_y / TILE_GROUP_EDGE) * REGION_HEADER_BYTES;
	layout->region_header_bytes = align_up(layout->region_header_bytes, REGION_HEADER_ALIGN);

	/* The tail pointer cache: one pointer per tile group of a square of a power of two tiles. */
	tail_edge = next_power_of_two(covered_x);
	tail_edge_y = next_power_of_two(covered_y);
	if (tail_edge_y > tail_edge)
		tail_edge = tail_edge_y;
	tail_used = (uint64_t)tail_edge * tail_edge / (TILE_GROUP_EDGE * TILE_GROUP_EDGE) * TAIL_POINTER_BYTES;
	layout->tail_used_bytes = (uint32_t)tail_used;
	layout->tail_bytes = align_up(tail_used, TAIL_CACHE_LINE);

	/* The vertex heap table; one layer has no render target cache. */
	layout->vertex_heap_bytes = (uint64_t)VERTEX_HEAP_ENTRIES * VERTEX_HEAP_ENTRY_BYTES;

	/* The macrolist: the parameter manager's table, directory and catalogue pages over every free page, per address space. */
	table_pages = divide_up(local_pages + global_pages, TABLE_ENTRIES_PER_PAGE);
	directory_pages = divide_up(table_pages, DIRECTORY_ENTRIES_PER_PAGE);
	catalogue_pages = divide_up(directory_pages, CATALOGUE_ENTRIES_PER_PAGE);
	layout->macrolist_bytes = (uint64_t)(table_pages + directory_pages + catalogue_pages) * PM_ADDRESS_SPACES * MACROLIST_ENTRY_BYTES;
	layout->macrolist_bytes = align_up(layout->macrolist_bytes, POWERVR_PM_PAGE_BYTES);

	/* The screen's last pixel, the last tile and the macrotile edges, in their register fields. */
	layout->screen_pixel_max = (shape->width - 1U) | ((shape->height - 1U) << PPP_SCREEN_Y_SHIFT);
	layout->te_screen = tile_max_x | (tile_max_y << TE_SCREEN_Y_SHIFT);
	layout->te_macrotile_x = layout->macrotile_x << TE_MACROTILE_FIRST_SHIFT;
	layout->te_macrotile_y = layout->macrotile_y << TE_MACROTILE_FIRST_SHIFT;
	layout->macrotile_stride = layout->macrotile_x * layout->macrotile_y;
	layout->isp_macrotile_size = ((layout->macrotile_x * sample_scale) << ISP_MACROTILE_X_SHIFT) | (layout->macrotile_y * sample_scale);

	/* The samples' positions, upright and flipped, and the tiling engine's doubled tiles of four samples. */
	if (shape->samples == 4U) {
		layout->multisample = sample_positions(positions_four[0], positions_four[1], 4U, false);
		layout->flipped_multisample = sample_positions(positions_four[0], positions_four[1], 4U, true);
		layout->te_antialias = TE_ANTIALIAS_X | TE_ANTIALIAS_Y;
	} else {
		layout->multisample = sample_positions(positions_one[0], positions_one[1], 1U, false);
		layout->flipped_multisample = sample_positions(positions_one[0], positions_one[1], 1U, true);
		layout->te_antialias = 0U;
	}
	layout->samples = shape->samples;

	/* Succeeded: the layout is filled. */
	return 0;
}

/*
 * Fills the shared record of a render target.
 *
 * The merge limits stay zero: this core's 16-pixel tiles leave the ISP's
 * merge enhancement off.
 *
 * Procedure referenced:
 * Mesa 26.2.5 src/imagination/vulkan/pvr_arch_job_render.c
 * (pvr_rt_dataset_ws_create_info_init: the merge values only without
 * roguexe 16-pixel tiles); Linux v7.3
 * drivers/gpu/drm/imagination/pvr_hwrt.c
 * (hwrt_init_common_fw_structure).
 */
void
powervr_target_common_record(
	const struct powervr_target_layout *layout,
	struct powervr_fw_target_common *record)
{
	/* Nothing named is zero. */
	*record = empty_target_common;

	/* The screen and the sample positions. */
	record->screen_pixel_max = layout->screen_pixel_max;
	record->multisample = layout->multisample;
	record->flipped_multisample = layout->flipped_multisample;

	/* The tail pointer cache of one layer: no stride, its used bytes. */
	record->tail_stride = 0U;
	record->tail_bytes = layout->tail_used_bytes;

	/* The tiling engine's screen and macrotiles, and the ISP's macrotile. */
	record->te_screen = layout->te_screen;
	record->macrotile_stride = layout->macrotile_stride;
	record->te_antialias = layout->te_antialias;
	record->te_macrotile_x = layout->te_macrotile_x;
	record->te_macrotile_y = layout->te_macrotile_y;
	record->region_headers = layout->region_headers;
	record->isp_macrotile_size = layout->isp_macrotile_size;
}

/*
 * Fills the record of one half of a render target.
 *
 * The local free list comes first and the global one second; the state
 * starts as none, and one layer needs no target arrays.
 *
 * Procedure referenced:
 * Linux v7.3 drivers/gpu/drm/imagination/pvr_hwrt.c
 * (hwrt_data_init_fw_structure); include/uapi/drm/pvr_drm.h
 * (PVR_DRM_HWRT_FREE_LIST_LOCAL 0, PVR_DRM_HWRT_FREE_LIST_GLOBAL 1).
 */
void
powervr_target_data_record(
	const struct powervr_target_places *places,
	struct powervr_fw_target_data *record)
{
	/* Nothing named is zero. */
	*record = empty_target_data;

	/* The shared record and the two free lists. */
	record->common_address = places->common;
	record->free_lists[0] = places->local_list;
	record->free_lists[1] = places->global_list;

	/* The tiling engine's memories; this core has no macrotile array, one layer no render target cache. */
	record->tail_address = places->tail;
	record->vertex_heap_address = places->vertex_heap;
	record->macrolist_address = places->macrolist;
	record->region_header_address = places->region_headers;

	/* One layer. */
	record->array.max_targets = POWERVR_TARGET_LAYERS_MAX;
}

/*
 * Fills the record of a free list.
 *
 * Every page is on the list from the start and none is held ready, so
 * the top of the stack is the list's last entry and the list's current
 * address is its first.
 *
 * Procedure referenced:
 * Linux v7.3 drivers/gpu/drm/imagination/pvr_free_list.c
 * (free_list_fw_init, calculate_free_list_ready_pages_locked: no ready
 * pages without a grow size).
 */
void
powervr_free_list_record(
	uint64_t list_address,
	uint32_t pages,
	uint32_t identity,
	struct powervr_fw_free_list *record)
{
	/* Nothing named is zero. */
	*record = empty_free_list;

	/* The list's size: all of it current, nothing to grow or held ready. */
	record->max_pages = pages;
	record->current_pages = pages;
	record->grow_pages = 0U;
	record->ready_pages = 0U;

	/* Its identity, and where the stack of its pages starts. */
	record->identity = identity;
	record->stack_top = pages - 1U;
	record->list_address = list_address;
	record->top_address = list_address;
}

/* Reports the entry of a free list's page list that names one page of a run. */
uint32_t
powervr_free_list_entry(
	uint64_t physical,
	uint32_t index)
{
	uint64_t frame;

	/* The parameter manager's page number of that page. */
	frame = (physical >> POWERVR_PM_PAGE_SHIFT) + index;

	/* Succeeded: the entry. */
	return (uint32_t)frame;
}

/* Reads the free list a grow request names. */
int
powervr_grow_request(
	const uint32_t notice[POWERVR_TARGET_COMMAND_WORDS],
	uint32_t *identity)
{
	/* Only a grow request. */
	if (notice[POWERVR_COMMAND_TYPE / 4U] != POWERVR_NOTICE_FREE_LIST_GROW)
		return EINVAL;

	/* Succeeded: the list's identity. */
	*identity = notice[COMMAND_HEADER_WORDS + POWERVR_GROW_REQUEST_LIST / 4U];
	return 0;
}

/*
 * Builds the answer to a grow request.
 *
 * The firmware asks once it has used the ready pages, and none were
 * held; the list does not grow, so the answer adds nothing, holds
 * nothing ready and leaves the list's pages as they are.
 *
 * Procedure referenced:
 * Linux v7.3 drivers/gpu/drm/imagination/pvr_free_list.c
 * (pvr_free_list_process_grow_req: the pages added zero when the grow
 * fails, the pages in all the current ones plus the ready ones).
 */
void
powervr_grow_answer(
	uint32_t list_address,
	uint32_t pages,
	uint32_t command[POWERVR_TARGET_COMMAND_WORDS])
{
	/* The command, the rest zero. */
	clear_command(command);
	command[POWERVR_COMMAND_TYPE / 4U] = POWERVR_COMMAND_GROW_ANSWER;

	/* The list, nothing added, its pages unchanged, nothing ready. */
	command[COMMAND_HEADER_WORDS + POWERVR_GROW_ANSWER_LIST / 4U] = list_address;
	command[COMMAND_HEADER_WORDS + POWERVR_GROW_ANSWER_ADDED / 4U] = 0U;
	command[COMMAND_HEADER_WORDS + POWERVR_GROW_ANSWER_PAGES / 4U] = pages;
	command[COMMAND_HEADER_WORDS + POWERVR_GROW_ANSWER_READY / 4U] = 0U;
}

/*
 * Reads the free lists a rebuild request names.
 *
 * Procedure referenced:
 * Linux v7.3 drivers/gpu/drm/imagination/pvr_free_list.c
 * (pvr_free_list_process_reconstruct_req: the count limited to the
 * array's size).
 */
int
powervr_rebuild_request(
	const uint32_t notice[POWERVR_TARGET_COMMAND_WORDS],
	uint32_t identities[POWERVR_REBUILD_LISTS_MAX],
	uint32_t *count)
{
	uint32_t index;

	/* Only a rebuild request. */
	if (notice[POWERVR_COMMAND_TYPE / 4U] != POWERVR_NOTICE_FREE_LIST_REBUILD)
		return EINVAL;

	/* The count, no more than the request holds. */
	*count = notice[COMMAND_HEADER_WORDS + POWERVR_REBUILD_REQUEST_COUNT / 4U];
	if (*count > POWERVR_REBUILD_LISTS_MAX)
		*count = POWERVR_REBUILD_LISTS_MAX;

	/* Each list it names. */
	for (index = 0; index < *count; index++)
		identities[index] = notice[COMMAND_HEADER_WORDS + POWERVR_REBUILD_REQUEST_LISTS / 4U + index];

	/* Succeeded: the lists are read. */
	return 0;
}

/*
 * Builds the answer to a rebuild request.
 *
 * Procedure referenced:
 * Linux v7.3 drivers/gpu/drm/imagination/pvr_free_list.c
 * (pvr_free_list_process_reconstruct_req: the count and the identities
 * of the request sent back).
 */
void
powervr_rebuild_answer(
	const uint32_t identities[POWERVR_REBUILD_LISTS_MAX],
	uint32_t count,
	uint32_t command[POWERVR_TARGET_COMMAND_WORDS])
{
	uint32_t index;

	/* The command, the rest zero. */
	clear_command(command);
	command[POWERVR_COMMAND_TYPE / 4U] = POWERVR_COMMAND_REBUILD_ANSWER;

	/* The count and each list rebuilt. */
	command[COMMAND_HEADER_WORDS + POWERVR_REBUILD_ANSWER_COUNT / 4U] = count;
	for (index = 0; index < count; index++)
		command[COMMAND_HEADER_WORDS + POWERVR_REBUILD_ANSWER_LISTS / 4U + index] = identities[index];
}

/* Reads which counter a statistics notice raises and by how much. */
int
powervr_statistics_notice(
	const uint32_t notice[POWERVR_TARGET_COMMAND_WORDS],
	uint32_t *counter,
	int32_t *adjustment)
{
	/* Only a statistics notice. */
	if (notice[POWERVR_COMMAND_TYPE / 4U] != POWERVR_NOTICE_STATISTICS)
		return EINVAL;

	/* The counter and the signed adjustment. */
	*counter = notice[COMMAND_HEADER_WORDS + POWERVR_STATISTICS_COUNTER / 4U];
	*adjustment = (int32_t)notice[COMMAND_HEADER_WORDS + POWERVR_STATISTICS_ADJUSTMENT / 4U];

	/* Succeeded: the notice is read. */
	return 0;
}

/*
 * Builds the command that has the firmware clean up one of its objects.
 *
 * Procedure referenced:
 * Linux v7.3 drivers/gpu/drm/imagination/pvr_fw.c
 * (pvr_fw_structure_cleanup: the kind, then the object's firmware
 * address).
 */
void
powervr_cleanup_command(
	uint32_t kind,
	uint32_t address,
	uint32_t command[POWERVR_TARGET_COMMAND_WORDS])
{
	/* The command, the rest zero. */
	clear_command(command);
	command[POWERVR_COMMAND_TYPE / 4U] = POWERVR_COMMAND_CLEANUP;

	/* The kind and the object. */
	command[COMMAND_HEADER_WORDS + POWERVR_CLEANUP_TYPE / 4U] = kind;
	command[COMMAND_HEADER_WORDS + POWERVR_CLEANUP_CONTEXT / 4U] = address;
}

/* Divides, rounding up. */
static uint32_t
divide_up(
	uint32_t value,
	uint32_t divisor)
{
	/* The quotient of the value plus all but one of the divisor. */
	return (value + divisor - 1U) / divisor;
}

/* Rounds a value up to a power-of-two boundary. */
static uint64_t
align_up(
	uint64_t value,
	uint64_t boundary)
{
	/* The value plus all but one of the boundary, with the low bits cleared. */
	return (value + boundary - 1U) & ~(boundary - 1U);
}

/* Reports the least power of two at or above a value, one for zero. */
static uint32_t
next_power_of_two(
	uint32_t value)
{
	uint32_t power;

	/* Doubles from one until it reaches the value. */
	power = 1U;
	while (power < value)
		power <<= 1;

	/* Succeeded: the power. */
	return power;
}

/*
 * Reports the sample positions register of count samples: a byte a
 * sample from the low one, x in its low four bits and y (16 minus y,
 * masked, when flipped) in its high four.
 */
static uint64_t
sample_positions(
	const uint8_t *x,
	const uint8_t *y,
	uint32_t count,
	bool flipped)
{
	uint64_t value;
	uint32_t vertical;
	uint32_t index;

	/* Each sample's byte. */
	value = 0;
	for (index = 0; index < count; index++) {
		vertical = y[index];
		if (flipped)
			vertical = (SAMPLE_EDGE - vertical) & 0xfU;
		value |= (uint64_t)(x[index] | (vertical << SAMPLE_Y_SHIFT)) << (index * 8U);
	}

	/* Succeeded: the register. */
	return value;
}

/* Zeroes a command's words. */
static void
clear_command(
	uint32_t command[POWERVR_TARGET_COMMAND_WORDS])
{
	uint32_t index;

	/* Every word. */
	for (index = 0; index < POWERVR_TARGET_COMMAND_WORDS; index++)
		command[index] = 0U;
}
