/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The resource manager's controls and object parameters for the address
 * space and the channel, and the commands a channel runs.
 *
 * A control is a 24-byte header (client, object, command, status, parameter
 * length, flags) followed by the parameters.  The parameters are written by
 * byte offset (the 570.144 layouts).  The commands are the host's methods in
 * a push buffer (a header word, then the data words) and the 8-byte entries
 * of the ring (GPFIFO) that point at push buffer segments.  Nothing here
 * touches the hardware.
 */

#include <stddef.h>
#include <stdint.h>

#include <uapi/errno.h>

#include "drivers/gpu/nvrtx/nvrtx-gsp.h"

/*
 * The control header's fields.
 *
 * Values referenced: NVIDIA open-gpu-kernel-modules 570.144
 * src/nvidia/generated/g_rpc-structures.h (rpc_gsp_rm_control_v03_00).
 */
#define OBJECTS_CONTROL_CLIENT_AT	0U
#define OBJECTS_CONTROL_OBJECT_AT	4U
#define OBJECTS_CONTROL_COMMAND_AT	8U
#define OBJECTS_CONTROL_PARAMS_SIZE_AT	16U

/*
 * The address space's parameters: the index (0, a new space) and the flags
 * (bit 3: owned by the driver).
 *
 * Values referenced: NVIDIA open-gpu-kernel-modules 570.144
 * src/common/sdk/nvidia/inc/nvos.h (NV_VASPACE_ALLOCATION_PARAMETERS,
 * NV_VASPACE_ALLOCATION_FLAGS_IS_EXTERNALLY_OWNED).
 */
#define OBJECTS_SPACE_FLAGS_AT		4U
#define OBJECTS_SPACE_EXTERNAL		(1U << 3)

/*
 * The page-directory control's parameters, and the root directory's
 * entries.
 *
 * Values referenced: NVIDIA open-gpu-kernel-modules 570.144
 * src/common/sdk/nvidia/inc/ctrl/ctrl0080/ctrl0080dma.h
 * (NV0080_CTRL_DMA_SET_PAGE_DIRECTORY_PARAMS) and Linux v6.19
 * drivers/gpu/drm/nouveau/nvkm/subdev/gsp/rm/r535/vmm.c
 * (r535_mmu_vaspace_new, the root's entry count).
 */
#define OBJECTS_PDIR_ADDRESS_AT		0U
#define OBJECTS_PDIR_ENTRIES_AT		8U
#define OBJECTS_PDIR_FLAGS_AT		12U
#define OBJECTS_PDIR_SPACE_AT		16U
#define OBJECTS_PDIR_ROOT_ENTRIES	4U

/*
 * The device information table: the entry count, and the entries of 100
 * bytes whose words 2 and 3 are the engine type and the runlist.
 *
 * Values referenced: NVIDIA open-gpu-kernel-modules 570.144
 * src/common/sdk/nvidia/inc/ctrl/ctrl2080/ctrl2080fifo.h
 * (NV2080_CTRL_FIFO_GET_DEVICE_INFO_TABLE_PARAMS,
 * NV2080_CTRL_FIFO_DEVICE_ENTRY) and Linux v6.19
 * drivers/gpu/drm/nouveau/nvkm/subdev/gsp/rm/r535/nvrm/fifo.h
 * (ENGINE_INFO_TYPE_RM_ENGINE_TYPE, ENGINE_INFO_TYPE_RUNLIST).
 */
#define OBJECTS_DEVINFO_COUNT_AT	4U
#define OBJECTS_DEVINFO_ENTRIES_AT	12U
#define OBJECTS_DEVINFO_ENTRY_BYTES	100U
#define OBJECTS_DEVINFO_MAX		32U
#define OBJECTS_DEVINFO_TYPE_AT		8U
#define OBJECTS_DEVINFO_RUNLIST_AT	12U

/*
 * An entry's engine descriptor (its word 0), and the first video
 * decoder's engine type in the table's numbering, the resource manager's
 * own (570 numbers twenty copy engines before it; the controls name the
 * decoder by another number, NVRTX_ENGINE_NVDEC0).
 *
 * Values referenced: Linux v6.19
 * drivers/gpu/drm/nouveau/nvkm/subdev/gsp/rm/r535/nvrm/fifo.h
 * (ENGINE_INFO_TYPE_ENG_DESC) and
 * drivers/gpu/drm/nouveau/nvkm/subdev/gsp/rm/r570/nvrm/engine.h
 * (RM_ENGINE_TYPE_NVDEC0).
 */
#define OBJECTS_DEVINFO_DESCRIPTOR_AT	0U
#define OBJECTS_ENGINE_NVDEC0		0x1dU

/*
 * The first video encoder's engine type in the table's numbering (after
 * the eight decoders; the controls name the encoder by another number,
 * NVRTX_ENGINE_NVENC0).
 *
 * Value referenced: NVIDIA open-gpu-kernel-modules 570.144
 * src/nvidia/inc/kernel/gpu/gpu_engine_type.h (RM_ENGINE_TYPE_NVENC0).
 */
#define OBJECTS_ENGINE_NVENC0		0x25U

/*
 * The engine types: graphics, and the ten copy engines (the same numbers in
 * both of the resource manager's numberings).
 *
 * Values referenced: NVIDIA open-gpu-kernel-modules 570.144
 * src/common/sdk/nvidia/inc/class/cl2080_notification.h
 * (NV2080_ENGINE_TYPE_GRAPHICS, NV2080_ENGINE_TYPE_COPY0,
 * NV2080_ENGINE_TYPE_COPY9) and Linux v6.19
 * drivers/gpu/drm/nouveau/nvkm/subdev/gsp/rm/r570/nvrm/engine.h
 * (RM_ENGINE_TYPE_GR0, RM_ENGINE_TYPE_COPY0, RM_ENGINE_TYPE_COPY9).
 */
#define OBJECTS_ENGINE_GRAPHICS		0x01U
#define OBJECTS_ENGINE_COPY_FIRST	0x09U
#define OBJECTS_ENGINE_COPY_LAST	0x12U

/*
 * The channel's parameters, and the memory spaces they name.
 *
 * Values referenced: NVIDIA open-gpu-kernel-modules 570.144
 * src/common/sdk/nvidia/inc/alloc/alloc_channel.h (NV_CHANNEL_ALLOC_PARAMS,
 * NV_MEMORY_DESC_PARAMS) and Linux v6.19
 * drivers/gpu/drm/nouveau/nvkm/subdev/gsp/rm/r535/nvrm/disp.h (ADDR_SYSMEM,
 * ADDR_FBMEM).
 */
#define OBJECTS_CHAN_RING_AT		8U
#define OBJECTS_CHAN_RING_ENTRIES_AT	16U
#define OBJECTS_CHAN_FLAGS_AT		20U
#define OBJECTS_CHAN_SPACE_AT		28U
#define OBJECTS_CHAN_ENGINE_AT		128U
#define OBJECTS_CHAN_INSTANCE_AT	144U
#define OBJECTS_CHAN_USERD_AT		168U
#define OBJECTS_CHAN_RAMFC_AT		192U
#define OBJECTS_CHAN_METHODS_AT		216U
#define OBJECTS_CHAN_INTERNAL_AT	244U
#define OBJECTS_MEMORY_BASE_AT		0U
#define OBJECTS_MEMORY_SIZE_AT		8U
#define OBJECTS_MEMORY_SPACE_AT		16U
#define OBJECTS_MEMORY_CACHE_AT		20U
#define OBJECTS_SPACE_SYSTEM		1U
#define OBJECTS_SPACE_VRAM		2U

/*
 * The channel's flags: USERD index in bits 10:8, USERD page in bits 20:12
 * with its fixed bit 21.
 *
 * Values referenced: NVIDIA open-gpu-kernel-modules 570.144
 * src/common/sdk/nvidia/inc/alloc/alloc_channel.h
 * (NVOS04_FLAGS_CHANNEL_USERD_INDEX_VALUE,
 * NVOS04_FLAGS_CHANNEL_USERD_INDEX_PAGE_VALUE,
 * NVOS04_FLAGS_CHANNEL_USERD_INDEX_PAGE_FIXED) and Linux v6.19
 * drivers/gpu/drm/nouveau/nvkm/subdev/gsp/rm/r535/fifo.c (CHID_PER_USERD).
 */
#define OBJECTS_FLAG_USERD_INDEX_SHIFT	8U
#define OBJECTS_FLAG_USERD_PAGE_SHIFT	12U
#define OBJECTS_FLAG_USERD_PAGE_FIXED	(1U << 21)
#define OBJECTS_CHANNELS_PER_USERD	8U

/*
 * The channel's internal flags: user privilege, and no error notifiers
 * (type "none" = 1 in bits 3:2 and 5:4).
 *
 * Values referenced: Linux v6.19
 * drivers/gpu/drm/nouveau/nvkm/subdev/gsp/rm/r535/nvrm/fifo.h
 * (NV_KERNELCHANNEL_ALLOC_INTERNALFLAGS_ERROR_NOTIFIER_TYPE,
 * NV_KERNELCHANNEL_ALLOC_INTERNALFLAGS_ECC_ERROR_NOTIFIER_TYPE,
 * ERROR_NOTIFIER_TYPE_NONE).
 */
#define OBJECTS_INTERNAL_NO_NOTIFIERS	((1U << 2) | (1U << 4))

/*
 * A privileged channel: the flag's bit 5, and the admin privilege (1) in the
 * internal flags' bits 1:0.
 *
 * Values referenced: NVIDIA open-gpu-kernel-modules 570.144
 * src/common/sdk/nvidia/inc/alloc/alloc_channel.h
 * (NVOS04_FLAGS_PRIVILEGED_CHANNEL) and Linux v6.19
 * drivers/gpu/drm/nouveau/nvkm/subdev/gsp/rm/r535/nvrm/fifo.h
 * (NV_KERNELCHANNEL_ALLOC_INTERNALFLAGS_PRIVILEGE_ADMIN).
 */
#define OBJECTS_FLAG_PRIVILEGED		(1U << 5)
#define OBJECTS_INTERNAL_ADMIN		1U

/*
 * The copy of the server's reserved directories: the range, the level count
 * and three levels of 24 bytes.
 *
 * Values referenced: NVIDIA open-gpu-kernel-modules 570.144
 * src/common/sdk/nvidia/inc/ctrl/ctrl90f1.h
 * (NV90F1_CTRL_VASPACE_COPY_SERVER_RESERVED_PDES_PARAMS) and Linux v6.19
 * drivers/gpu/drm/nouveau/nvkm/subdev/gsp/rm/r535/vmm.c
 * (r535_mmu_vaspace_new, the levels' aperture).
 */
#define OBJECTS_PDES_PAGE_SIZE_AT	8U
#define OBJECTS_PDES_LOW_AT		16U
#define OBJECTS_PDES_HIGH_AT		24U
#define OBJECTS_PDES_LEVELS_AT		32U
#define OBJECTS_PDES_LEVEL_FIRST_AT	40U
#define OBJECTS_PDES_LEVEL_BYTES	24U
#define OBJECTS_PDES_LEVEL_ADDRESS_AT	0U
#define OBJECTS_PDES_LEVEL_SIZE_AT	8U
#define OBJECTS_PDES_LEVEL_APERTURE_AT	16U
#define OBJECTS_PDES_LEVEL_SHIFT_AT	20U
#define OBJECTS_PDES_APERTURE_VRAM	1U
#define OBJECTS_PDES_LEVEL_COUNT	3U

/*
 * The context buffer information: engine 0's 26 entries of {size,
 * alignment}.
 *
 * Values referenced: NVIDIA open-gpu-kernel-modules 570.144
 * src/common/sdk/nvidia/inc/ctrl/ctrl2080/ctrl2080internal.h
 * (NV2080_CTRL_INTERNAL_ENGINE_CONTEXT_PROPERTIES_ENGINE_ID_COUNT,
 * NV2080_CTRL_INTERNAL_ENGINE_CONTEXT_BUFFER_INFO).
 */
#define OBJECTS_GR_INFO_ENTRIES		26U
#define OBJECTS_GR_INFO_ENTRY_BYTES	8U

/*
 * The engine numbers in that table, and the promote call's buffer numbers.
 *
 * Values referenced: NVIDIA open-gpu-kernel-modules 570.144
 * src/common/sdk/nvidia/inc/ctrl/ctrl0080/ctrl0080fifo.h
 * (NV0080_CTRL_FIFO_GET_ENGINE_CONTEXT_PROPERTIES_ENGINE_ID_GRAPHICS_*) and
 * ctrl/ctrl2080/ctrl2080gpu.h (NV2080_CTRL_GPU_PROMOTE_CTX_BUFFER_ID_*).
 */
#define OBJECTS_GR_ID_MAIN		0x00U
#define OBJECTS_GR_ID_PAGEPOOL		0x0dU
#define OBJECTS_GR_ID_PATCH		0x10U
#define OBJECTS_GR_ID_BUNDLE		0x11U
#define OBJECTS_GR_ID_ATTRIBUTE		0x13U
#define OBJECTS_GR_ID_RTV		0x14U
#define OBJECTS_GR_ID_FECS_EVENT	0x17U
#define OBJECTS_GR_ID_PRIV_MAP		0x18U
#define OBJECTS_BUFFER_MAIN		0U
#define OBJECTS_BUFFER_PATCH		2U
#define OBJECTS_BUFFER_BUNDLE		3U
#define OBJECTS_BUFFER_PAGEPOOL		4U
#define OBJECTS_BUFFER_ATTRIBUTE	5U
#define OBJECTS_BUFFER_RTV		6U
#define OBJECTS_BUFFER_FECS_EVENT	9U
#define OBJECTS_BUFFER_PRIV_MAP		10U
#define OBJECTS_BUFFER_UNRESTRICTED_MAP	11U

/*
 * The main buffer carries 64 pages of subcontext headers after the context.
 *
 * Value referenced: Linux v6.19
 * drivers/gpu/drm/nouveau/nvkm/subdev/gsp/rm/r535/gr.c
 * (r535_gr_get_ctxbuf_info).
 */
#define OBJECTS_GR_MAIN_EXTRA		(64U * 0x1000U)

/*
 * The promote call: its header fields and the 32-byte entries.
 *
 * Values referenced: NVIDIA open-gpu-kernel-modules 570.144
 * src/common/sdk/nvidia/inc/ctrl/ctrl2080/ctrl2080gpu.h
 * (NV2080_CTRL_GPU_PROMOTE_CTX_PARAMS,
 * NV2080_CTRL_GPU_PROMOTE_CTX_BUFFER_ENTRY,
 * NV2080_CTRL_GPU_PROMOTE_CONTEXT_MAX_ENTRIES) and Linux v6.19
 * drivers/gpu/drm/nouveau/nvkm/subdev/gsp/rm/r535/gr.c (r535_gr_promote_ctx,
 * the VRAM's attribute).
 */
#define OBJECTS_PROMOTE_ENGINE_AT	0U
#define OBJECTS_PROMOTE_CHAN_CLIENT_AT	12U
#define OBJECTS_PROMOTE_OBJECT_AT	16U
#define OBJECTS_PROMOTE_COUNT_AT	40U
#define OBJECTS_PROMOTE_ENTRIES_AT	48U
#define OBJECTS_PROMOTE_ENTRY_BYTES	32U
#define OBJECTS_PROMOTE_MAX		16U
#define OBJECTS_PROMOTE_PHYSICAL_AT	0U
#define OBJECTS_PROMOTE_VIRTUAL_AT	8U
#define OBJECTS_PROMOTE_SIZE_AT		16U
#define OBJECTS_PROMOTE_ATTRIBUTE_AT	24U
#define OBJECTS_PROMOTE_BUFFER_AT	28U
#define OBJECTS_PROMOTE_INITIALIZE_AT	30U
#define OBJECTS_PROMOTE_NONMAPPED_AT	31U
#define OBJECTS_PROMOTE_ATTRIBUTE_VRAM	4U

/*
 * The graphics context buffers the driver makes, in the order of the
 * engine numbers: the table's engine number, the promote call's buffer
 * number, and whether the buffer is global, initialized and read-only.
 */
struct objects_gr_kind {
	uint32_t engine_id;
	uint32_t buffer_id;
	int global;
	int initialize;
	int read_only;
};

static const struct objects_gr_kind objects_gr_kinds[] = {
	{ OBJECTS_GR_ID_MAIN, OBJECTS_BUFFER_MAIN, 0, 1, 0 },
	{ OBJECTS_GR_ID_PAGEPOOL, OBJECTS_BUFFER_PAGEPOOL, 1, 0, 0 },
	{ OBJECTS_GR_ID_PATCH, OBJECTS_BUFFER_PATCH, 0, 1, 0 },
	{ OBJECTS_GR_ID_BUNDLE, OBJECTS_BUFFER_BUNDLE, 1, 0, 0 },
	{ OBJECTS_GR_ID_ATTRIBUTE, OBJECTS_BUFFER_ATTRIBUTE, 1, 0, 0 },
	{ OBJECTS_GR_ID_RTV, OBJECTS_BUFFER_RTV, 1, 0, 0 },
	{ OBJECTS_GR_ID_FECS_EVENT, OBJECTS_BUFFER_FECS_EVENT, 1, 1, 0 },
	{ OBJECTS_GR_ID_PRIV_MAP, OBJECTS_BUFFER_PRIV_MAP, 1, 1, 1 },
};

/*
 * The method header: the opcode (incrementing) in bits 31:29, count 28:16,
 * subchannel 15:13, method/4 in 11:0.
 *
 * Values referenced: NVIDIA open-gpu-doc classes/host/clc46f.h
 * (NVC46F_DMA_INCR_OPCODE, NVC46F_DMA_INCR_COUNT, NVC46F_DMA_INCR_SUBCHANNEL,
 * NVC46F_DMA_INCR_ADDRESS).
 */
#define OBJECTS_METHOD_INCREMENT	(1U << 29)

/*
 * The host's methods and values for a fence.
 *
 * Values referenced: NVIDIA open-gpu-doc classes/host/clc46f.h
 * (NVC46F_NON_STALL_INTERRUPT, NVC46F_MEM_OP_A, NVC46F_SEM_ADDR_LO,
 * NVC46F_SEM_EXECUTE, NVC46F_SEM_EXECUTE_OPERATION_RELEASE,
 * NVC46F_SEM_EXECUTE_RELEASE_WFI, NVC46F_SEM_EXECUTE_RELEASE_TIMESTAMP,
 * NVC46F_MEM_OP_D_OPERATION_MEMBAR).
 */
#define OBJECTS_HOST_NON_STALL		0x020U
#define OBJECTS_HOST_MEM_OP_A		0x028U
#define OBJECTS_HOST_SEM_ADDRESS_LOW	0x05cU
#define OBJECTS_HOST_SEM_EXECUTE	0x06cU
#define OBJECTS_SEM_RELEASE		0x1U
#define OBJECTS_SEM_RELEASE_WFI		(1U << 20)
#define OBJECTS_SEM_RELEASE_TIMESTAMP	(1U << 25)
#define OBJECTS_MEM_OP_MEMBAR		(5U << 27)

/*
 * The copy engine's methods and values for a fill: the constant, the
 * component setup, the destination, the launch (one line, or several
 * lines a pitch apart).
 *
 * Values referenced: NVIDIA open-gpu-doc classes/host/clc46f.h
 * (NVC46F_SET_OBJECT) and classes/dma-copy/clc5b5.h (NVC5B5_LAUNCH_DMA and
 * its MULTI_LINE_ENABLE field, NVC5B5_OFFSET_OUT_UPPER to NVC5B5_LINE_COUNT,
 * NVC5B5_LINE_LENGTH_IN, NVC5B5_SET_REMAP_CONST_A,
 * NVC5B5_SET_REMAP_COMPONENTS and their fields).
 */
#define OBJECTS_COPY_SET_OBJECT		0x000U
#define OBJECTS_COPY_LAUNCH		0x300U
#define OBJECTS_COPY_OFFSET_OUT_UPPER	0x408U
#define OBJECTS_COPY_LINE_LENGTH	0x418U
#define OBJECTS_COPY_REMAP_CONST_A	0x700U
#define OBJECTS_COPY_REMAP_COMPONENTS	0x708U
#define OBJECTS_COPY_REMAP_FILL_A	(0x4U | (0x3U << 16))
#define OBJECTS_COPY_LAUNCH_FILL	(0x2U | (1U << 2) | (1U << 7) | (1U << 8) | (1U << 10))
#define OBJECTS_COPY_LAUNCH_MULTI_LINE	(1U << 9)

static void objects_clear(uint8_t *bytes, size_t length);
static size_t objects_method(uint8_t *push, size_t at, uint32_t method, uint32_t count);
static size_t objects_method_on(uint8_t *push, size_t at, uint32_t subchannel, uint32_t method, uint32_t count);
static void objects_memory(uint8_t *params, size_t at, uint64_t base, uint64_t size, uint32_t space, uint32_t cache);
static uint32_t objects_log2_up(uint64_t value);
static size_t objects_fence(uint8_t *push, uint64_t semaphore, uint32_t value, uint32_t execute);

/*
 * Writes a control call: the header, then the parameters.
 *
 * Returns the call's length, or 0 when it does not fit.
 */
size_t
nvrtx_control_call(
	uint8_t *call,
	size_t capacity,
	uint32_t client,
	uint32_t object,
	uint32_t command,
	const uint8_t *parameters,
	size_t length)
{
	size_t at;

	/* Refuses a call that does not fit. */
	if (NVRTX_CONTROL_HEADER_BYTES + length > capacity)
		return 0U;

	/* Writes the header; the status and flags stay zero. */
	objects_clear(call, NVRTX_CONTROL_HEADER_BYTES);
	nvrtx_put32(call, OBJECTS_CONTROL_CLIENT_AT, client);
	nvrtx_put32(call, OBJECTS_CONTROL_OBJECT_AT, object);
	nvrtx_put32(call, OBJECTS_CONTROL_COMMAND_AT, command);
	nvrtx_put32(call, OBJECTS_CONTROL_PARAMS_SIZE_AT, (uint32_t)length);

	/* Copies the parameters. */
	for (at = 0U; at < length; at++)
		call[NVRTX_CONTROL_HEADER_BYTES + at] = parameters[at];

	/* Succeeded: the call's length. */
	return NVRTX_CONTROL_HEADER_BYTES + length;
}

/*
 * Writes the parameters of an address space whose page tables the driver owns.
 */
void
nvrtx_space_params(
	uint8_t *params)
{
	/* A new space (index 0), externally owned. */
	objects_clear(params, NVRTX_SPACE_PARAMS_BYTES);
	nvrtx_put32(params, OBJECTS_SPACE_FLAGS_AT, OBJECTS_SPACE_EXTERNAL);
}

/*
 * Writes the parameters that give an address space its root page directory in the VRAM.
 */
void
nvrtx_pdir_params(
	uint8_t *params,
	uint64_t root,
	uint32_t space)
{
	/* The root's VRAM address, its four entries, the VRAM aperture (0) and the space. */
	objects_clear(params, NVRTX_PDIR_PARAMS_BYTES);
	nvrtx_put64(params, OBJECTS_PDIR_ADDRESS_AT, root);
	nvrtx_put32(params, OBJECTS_PDIR_ENTRIES_AT, OBJECTS_PDIR_ROOT_ENTRIES);
	nvrtx_put32(params, OBJECTS_PDIR_FLAGS_AT, 0U);
	nvrtx_put32(params, OBJECTS_PDIR_SPACE_AT, space);
}

/*
 * Picks the copy engine for the first channel from the device information table.
 *
 * The graphics engine's runlist is found first; the first copy engine on
 * another runlist is chosen (a copy engine that shares the graphics runlist
 * belongs to it).  The first video decoder, the first video encoder and the
 * first copy engine on the graphics runlist are noted too, for the
 * decoder's and the encoder's channels and the work channel's copy
 * object.  Returns ENOENT when there is no copy engine of a
 * runlist of its own.
 */
int
nvrtx_devinfo_pick(
	const uint8_t *params,
	size_t length,
	struct nvrtx_engine_pick *pick)
{
	uint32_t count;
	uint32_t index;
	uint32_t type;
	uint32_t runlist;
	uint32_t graphics_runlist;
	size_t at;

	/* The table must hold its count and every entry it counts. */
	if (length < OBJECTS_DEVINFO_ENTRIES_AT)
		return EINVAL;
	count = nvrtx_get32(params, OBJECTS_DEVINFO_COUNT_AT);
	if (count > OBJECTS_DEVINFO_MAX || OBJECTS_DEVINFO_ENTRIES_AT + (size_t)count * OBJECTS_DEVINFO_ENTRY_BYTES > length)
		return EINVAL;

	/* Finds the graphics engine's runlist. */
	graphics_runlist = 0xffffffffU;
	for (index = 0U; index < count; index++) {
		at = OBJECTS_DEVINFO_ENTRIES_AT + (size_t)index * OBJECTS_DEVINFO_ENTRY_BYTES;
		type = nvrtx_get32(params, at + OBJECTS_DEVINFO_TYPE_AT);
		if (type == OBJECTS_ENGINE_GRAPHICS)
			graphics_runlist = nvrtx_get32(params, at + OBJECTS_DEVINFO_RUNLIST_AT);
	}

	pick->graphics_runlist = graphics_runlist;

	/* Notes the first video decoder, the first video encoder and the first copy engine on the graphics runlist. */
	pick->graphics_copy = 0xffffffffU;
	pick->nvdec_found = 0U;
	pick->nvdec_runlist = 0U;
	pick->nvdec_descriptor = 0U;
	pick->nvenc_found = 0U;
	pick->nvenc_runlist = 0U;
	pick->nvenc_descriptor = 0U;
	for (index = 0U; index < count; index++) {
		at = OBJECTS_DEVINFO_ENTRIES_AT + (size_t)index * OBJECTS_DEVINFO_ENTRY_BYTES;
		type = nvrtx_get32(params, at + OBJECTS_DEVINFO_TYPE_AT);
		runlist = nvrtx_get32(params, at + OBJECTS_DEVINFO_RUNLIST_AT);

		/* The first decoder's runlist and descriptor. */
		if (type == OBJECTS_ENGINE_NVDEC0 && pick->nvdec_found == 0U) {
			pick->nvdec_found = 1U;
			pick->nvdec_runlist = runlist;
			pick->nvdec_descriptor = nvrtx_get32(params, at + OBJECTS_DEVINFO_DESCRIPTOR_AT);
		}

		/* The first encoder's runlist and descriptor. */
		if (type == OBJECTS_ENGINE_NVENC0 && pick->nvenc_found == 0U) {
			pick->nvenc_found = 1U;
			pick->nvenc_runlist = runlist;
			pick->nvenc_descriptor = nvrtx_get32(params, at + OBJECTS_DEVINFO_DESCRIPTOR_AT);
		}

		/* The first copy engine that shares the graphics runlist. */
		if (type >= OBJECTS_ENGINE_COPY_FIRST &&
		    type <= OBJECTS_ENGINE_COPY_LAST &&
		    runlist == graphics_runlist &&
		    pick->graphics_copy == 0xffffffffU)
			pick->graphics_copy = type;
	}

	/* Takes the first copy engine on a runlist of its own. */
	for (index = 0U; index < count; index++) {
		at = OBJECTS_DEVINFO_ENTRIES_AT + (size_t)index * OBJECTS_DEVINFO_ENTRY_BYTES;
		type = nvrtx_get32(params, at + OBJECTS_DEVINFO_TYPE_AT);
		runlist = nvrtx_get32(params, at + OBJECTS_DEVINFO_RUNLIST_AT);
		if (type < OBJECTS_ENGINE_COPY_FIRST || type > OBJECTS_ENGINE_COPY_LAST)
			continue;
		if (runlist == graphics_runlist)
			continue;

		/* Succeeded: the copy engine and its runlist. */
		pick->engine = type;
		pick->runlist = runlist;
		return 0;
	}

	/* No copy engine of its own runlist. */
	return ENOENT;
}

/*
 * Writes the parameters of a channel.
 *
 * The ring is at its GPU virtual address; the instance block (whose first
 * 0x200 bytes are the RAMFC) and USERD are in the VRAM; the method buffer
 * is in system memory.  The channel is a user channel without error
 * notifiers.
 */
void
nvrtx_channel_params(
	uint8_t *params,
	const struct nvrtx_channel_setup *setup)
{
	uint32_t flags;

	/* Clears every field, the reserved ones included. */
	objects_clear(params, NVRTX_CHANNEL_PARAMS_BYTES);

	/* The ring: its address and its entry count. */
	nvrtx_put64(params, OBJECTS_CHAN_RING_AT, setup->ring_va);
	nvrtx_put32(params, OBJECTS_CHAN_RING_ENTRIES_AT, setup->ring_entries);

	/* A physical, user channel whose USERD place follows its number (eight to a page, the page fixed). */
	flags = (setup->chid % OBJECTS_CHANNELS_PER_USERD) << OBJECTS_FLAG_USERD_INDEX_SHIFT;
	flags |= (setup->chid / OBJECTS_CHANNELS_PER_USERD) << OBJECTS_FLAG_USERD_PAGE_SHIFT;
	flags |= OBJECTS_FLAG_USERD_PAGE_FIXED;

	/* A privileged channel says so in its flags. */
	if (setup->privileged != 0U)
		flags |= OBJECTS_FLAG_PRIVILEGED;
	nvrtx_put32(params, OBJECTS_CHAN_FLAGS_AT, flags);

	/* The address space and the engine. */
	nvrtx_put32(params, OBJECTS_CHAN_SPACE_AT, setup->space);
	nvrtx_put32(params, OBJECTS_CHAN_ENGINE_AT, setup->engine);

	/* The instance block, USERD and RAMFC in the VRAM, the method buffer in system memory. */
	objects_memory(params, OBJECTS_CHAN_INSTANCE_AT, setup->instance, NVRTX_INSTANCE_BYTES, OBJECTS_SPACE_VRAM, 1U);
	objects_memory(params, OBJECTS_CHAN_USERD_AT, setup->userd, NVRTX_USERD_BYTES, OBJECTS_SPACE_VRAM, 1U);
	objects_memory(params, OBJECTS_CHAN_RAMFC_AT, setup->instance, NVRTX_RAMFC_BYTES, OBJECTS_SPACE_VRAM, 1U);
	objects_memory(params, OBJECTS_CHAN_METHODS_AT, setup->methods, setup->methods_size, OBJECTS_SPACE_SYSTEM, 0U);

	/* User privilege (admin for a privileged channel), no error notifiers. */
	if (setup->privileged != 0U) {
		nvrtx_put32(params, OBJECTS_CHAN_INTERNAL_AT, OBJECTS_INTERNAL_NO_NOTIFIERS | OBJECTS_INTERNAL_ADMIN);
	} else {
		nvrtx_put32(params, OBJECTS_CHAN_INTERNAL_AT, OBJECTS_INTERNAL_NO_NOTIFIERS);
	}
}

/*
 * Writes the push buffer of one fence: release a semaphore after the work, then a system memory barrier.
 *
 * Returns the length in bytes (a multiple of 4).
 */
size_t
nvrtx_push_fence(
	uint8_t *push,
	uint64_t semaphore,
	uint32_t value)
{
	size_t length;

	/* A release that waits for the work before it. */
	length = objects_fence(push, semaphore, value, OBJECTS_SEM_RELEASE | OBJECTS_SEM_RELEASE_WFI);

	/* Succeeded: the push buffer's length. */
	return length;
}

/*
 * Writes the push buffer of one fence that also records when it passed.
 *
 * As nvrtx_push_fence(), but the release writes 16 bytes at the semaphore
 * (which must lie on a 16-byte boundary): the value in the first word and
 * the GPU's nanosecond timer in the last eight bytes, the timer written
 * before the value, so a CPU that sees the value reads a timer that is
 * already in place.  Returns the length in bytes.
 */
size_t
nvrtx_push_fence_timed(
	uint8_t *push,
	uint64_t semaphore,
	uint32_t value)
{
	size_t length;

	/* A release that waits for the work before it and writes the timer. */
	length = objects_fence(push, semaphore, value, OBJECTS_SEM_RELEASE | OBJECTS_SEM_RELEASE_WFI | OBJECTS_SEM_RELEASE_TIMESTAMP);

	/* Succeeded: the push buffer's length. */
	return length;
}

/*
 * Writes the push buffer of one time stamp: a release that does not wait
 * for any work, writing a value and the GPU's nanosecond timer at a
 * semaphore on a 16-byte boundary (laid out as nvrtx_push_fence_timed()
 * writes it).  It marks when the channel reached it.
 *
 * Returns the length in bytes.
 */
size_t
nvrtx_push_stamp(
	uint8_t *push,
	uint64_t semaphore,
	uint32_t value)
{
	size_t at;

	/* The semaphore's address and value. */
	at = objects_method(push, 0U, OBJECTS_HOST_SEM_ADDRESS_LOW, 3U);
	nvrtx_put32(push, at, (uint32_t)(semaphore & 0xfffffff0U));
	nvrtx_put32(push, at + 4U, (uint32_t)(semaphore >> 32));
	nvrtx_put32(push, at + 8U, value);
	at += 12U;

	/* Releases it at once, with the timer. */
	at = objects_method(push, at, OBJECTS_HOST_SEM_EXECUTE, 1U);
	nvrtx_put32(push, at, OBJECTS_SEM_RELEASE | OBJECTS_SEM_RELEASE_TIMESTAMP);
	at += 4U;

	/* Succeeded: the push buffer's length. */
	return at;
}

/*
 * Writes the push buffer of a copy-engine fill on a subchannel: the class
 * bound there (the generation's copy class), the constant, and one line of
 * words at the destination.
 *
 * Returns the length in bytes.
 */
size_t
nvrtx_push_copy_fill(
	uint8_t *push,
	uint32_t subchannel,
	uint32_t copy_class,
	uint64_t destination,
	uint32_t words,
	uint32_t value)
{
	size_t at;

	/* Binds the copy class to the subchannel. */
	at = objects_method_on(push, 0U, subchannel, OBJECTS_COPY_SET_OBJECT, 1U);
	nvrtx_put32(push, at, copy_class);
	at += 4U;

	/* Fills from one 4-byte constant. */
	at = objects_method_on(push, at, subchannel, OBJECTS_COPY_REMAP_CONST_A, 1U);
	nvrtx_put32(push, at, value);
	at += 4U;
	at = objects_method_on(push, at, subchannel, OBJECTS_COPY_REMAP_COMPONENTS, 1U);
	nvrtx_put32(push, at, OBJECTS_COPY_REMAP_FILL_A);
	at += 4U;

	/* The destination's virtual address, then one line of the given words (out offset, pitches, length, count). */
	at = objects_method_on(push, at, subchannel, OBJECTS_COPY_OFFSET_OUT_UPPER, 2U);
	nvrtx_put32(push, at, (uint32_t)(destination >> 32));
	nvrtx_put32(push, at + 4U, (uint32_t)(destination & 0xffffffffU));
	at += 8U;
	at = objects_method_on(push, at, subchannel, OBJECTS_COPY_LINE_LENGTH, 2U);
	nvrtx_put32(push, at, words);
	nvrtx_put32(push, at + 4U, 1U);
	at += 8U;

	/* Launches a non-pipelined, flushed, pitch-to-pitch fill. */
	at = objects_method_on(push, at, subchannel, OBJECTS_COPY_LAUNCH, 1U);
	nvrtx_put32(push, at, OBJECTS_COPY_LAUNCH_FILL);
	at += 4U;

	/* Succeeded: the push buffer's length. */
	return at;
}

/*
 * Writes the push buffer of a copy-engine fill of lines rows of line_words
 * 4-byte words, line_words * 4 bytes apart (one run of
 * lines * line_words words), with one constant, on a subchannel the copy
 * class is already bound to.
 *
 * Returns the length in bytes (at most NVRTX_PUSH_COPY_CLEAR_BYTES).
 *
 * Procedure referenced: Mesa 25.0.7 src/nouveau/vulkan/nvk_cmd_copy.c
 * (nvk_CmdFillBuffer: the constant and the components, then the
 * destination, both pitches, the line's length in components and the
 * lines, then a multi-line launch with the remap).
 */
size_t
nvrtx_push_copy_clear(
	uint8_t *push,
	uint32_t subchannel,
	uint64_t destination,
	uint32_t line_words,
	uint32_t lines,
	uint32_t value)
{
	size_t at;

	/* Fills from one 4-byte constant. */
	at = objects_method_on(push, 0U, subchannel, OBJECTS_COPY_REMAP_CONST_A, 1U);
	nvrtx_put32(push, at, value);
	at += 4U;
	at = objects_method_on(push, at, subchannel, OBJECTS_COPY_REMAP_COMPONENTS, 1U);
	nvrtx_put32(push, at, OBJECTS_COPY_REMAP_FILL_A);
	at += 4U;

	/*
	 * The destination's virtual address, then the rows: the source's and
	 * the destination's pitch in bytes (the engine checks both of a
	 * multi-line launch), their length in words, their count.
	 */
	at = objects_method_on(push, at, subchannel, OBJECTS_COPY_OFFSET_OUT_UPPER, 6U);
	nvrtx_put32(push, at, (uint32_t)(destination >> 32));
	nvrtx_put32(push, at + 4U, (uint32_t)(destination & 0xffffffffU));
	nvrtx_put32(push, at + 8U, line_words * 4U);
	nvrtx_put32(push, at + 12U, line_words * 4U);
	nvrtx_put32(push, at + 16U, line_words);
	nvrtx_put32(push, at + 20U, lines);
	at += 24U;

	/* Launches a non-pipelined, flushed, pitch-to-pitch fill of the rows. */
	at = objects_method_on(push, at, subchannel, OBJECTS_COPY_LAUNCH, 1U);
	nvrtx_put32(push, at, OBJECTS_COPY_LAUNCH_FILL | OBJECTS_COPY_LAUNCH_MULTI_LINE);
	at += 4U;

	/* Succeeded: the push buffer's length. */
	return at;
}

/*
 * Writes one ring entry pointing at a push buffer segment of a number of words.
 */
void
nvrtx_ring_entry(
	uint8_t *entry,
	uint64_t va,
	uint32_t words)
{
	/* The low word: the segment's address bits 31:2 (fetched unconditionally). */
	nvrtx_put32(entry, 0U, (uint32_t)(va & 0xfffffffcU));

	/* The high word: the address bits 39:32, the main level, and the length in words. */
	nvrtx_put32(entry, 4U, (uint32_t)((va >> 32) & 0xffU) | (words << 10));
}

/*
 * Writes the parameters of the free call for one object of the driver's client.
 */
void
nvrtx_free_params(
	uint8_t *params,
	uint32_t client,
	uint32_t object)
{
	/* The client, no parent, the object, and a zero status. */
	objects_clear(params, NVRTX_FREE_PARAMS_BYTES);
	nvrtx_put32(params, 0U, client);
	nvrtx_put32(params, 8U, object);
}

/*
 * Writes the parameters of an address space the resource manager shares.
 *
 * A new space (index 0) with no flags: not owned by the driver, so the
 * resource manager keeps a range of it for itself (the copy of the server's
 * directories names where).
 */
void
nvrtx_space_params_shared(
	uint8_t *params)
{
	/* Clears every field: index 0 and no flags. */
	objects_clear(params, NVRTX_SPACE_PARAMS_BYTES);
}

/*
 * Writes the parameters that hand the resource manager the driver's top three directories.
 *
 * The resource manager keeps 512 MiB at 4 GiB for itself and writes its
 * entries for that range into the driver's directories: the root (4
 * entries), the second level and the first level (512 entries each), each
 * in the VRAM, along the path of index 0.
 */
void
nvrtx_copy_pdes_params(
	uint8_t *params,
	uint64_t root,
	uint64_t directory2,
	uint64_t directory1)
{
	uint64_t addresses[OBJECTS_PDES_LEVEL_COUNT];
	uint64_t sizes[OBJECTS_PDES_LEVEL_COUNT];
	uint32_t shifts[OBJECTS_PDES_LEVEL_COUNT];
	size_t at;
	unsigned level;

	/* The levels from the root down: their addresses, sizes and the address bit each indexes from. */
	addresses[0] = root;
	addresses[1] = directory2;
	addresses[2] = directory1;
	sizes[0] = 4U * 8U;
	sizes[1] = 512U * 8U;
	sizes[2] = 512U * 8U;
	shifts[0] = 47U;
	shifts[1] = 38U;
	shifts[2] = 29U;

	/* The range, in pieces of 512 MiB (the first level's coverage), on the device's subdevice 0. */
	objects_clear(params, NVRTX_COPY_PDES_PARAMS_BYTES);
	nvrtx_put64(params, OBJECTS_PDES_PAGE_SIZE_AT, NVRTX_SERVER_VA_BYTES);
	nvrtx_put64(params, OBJECTS_PDES_LOW_AT, NVRTX_SERVER_VA);
	nvrtx_put64(params, OBJECTS_PDES_HIGH_AT, NVRTX_SERVER_VA + NVRTX_SERVER_VA_BYTES - 1U);
	nvrtx_put32(params, OBJECTS_PDES_LEVELS_AT, OBJECTS_PDES_LEVEL_COUNT);

	/* Describes each level. */
	for (level = 0U; level < OBJECTS_PDES_LEVEL_COUNT; level++) {
		at = OBJECTS_PDES_LEVEL_FIRST_AT + (size_t)level * OBJECTS_PDES_LEVEL_BYTES;
		nvrtx_put64(params, at + OBJECTS_PDES_LEVEL_ADDRESS_AT, addresses[level]);
		nvrtx_put64(params, at + OBJECTS_PDES_LEVEL_SIZE_AT, sizes[level]);
		nvrtx_put32(params, at + OBJECTS_PDES_LEVEL_APERTURE_AT, OBJECTS_PDES_APERTURE_VRAM);
		nvrtx_put32(params, at + OBJECTS_PDES_LEVEL_SHIFT_AT, shifts[level]);
	}
}

/*
 * Reads the graphics context buffers from the resource manager's information.
 *
 * Takes engine 0's entries for the buffers the driver makes, in the order
 * of their engine numbers, and adds the unrestricted copy of the access map
 * after the access map.  The main buffer grows by its subcontext headers.
 * The page size is 2 MiB, 64 KiB or 4 KiB by the size, and the alignment
 * is the page's, but the attribute buffer's is its size rounded up to a
 * power of two.  Returns the number of buffers written, 0 when the
 * information is short or a buffer the driver needs is missing.
 */
size_t
nvrtx_gr_buffers(
	const uint8_t *info,
	size_t length,
	struct nvrtx_gr_buffer *buffers,
	size_t capacity)
{
	const struct objects_gr_kind *kind;
	struct nvrtx_gr_buffer *buffer;
	uint64_t size;
	size_t count;
	size_t index;

	/* The information must hold engine 0's table, and the list every buffer. */
	if (length < (size_t)OBJECTS_GR_INFO_ENTRIES * OBJECTS_GR_INFO_ENTRY_BYTES)
		return 0U;
	if (capacity < sizeof(objects_gr_kinds) / sizeof(objects_gr_kinds[0]) + 1U)
		return 0U;

	/* Describes each buffer the driver makes. */
	count = 0U;
	for (index = 0U; index < sizeof(objects_gr_kinds) / sizeof(objects_gr_kinds[0]); index++) {
		kind = &objects_gr_kinds[index];
		size = nvrtx_get32(info, (size_t)kind->engine_id * OBJECTS_GR_INFO_ENTRY_BYTES);
		if (size == 0U)
			return 0U;

		/* The main buffer holds the subcontext headers after the context. */
		if (kind->buffer_id == OBJECTS_BUFFER_MAIN)
			size = ((size + 0xfffU) & ~(uint64_t)0xfffU) + OBJECTS_GR_MAIN_EXTRA;

		buffer = &buffers[count];
		buffer->buffer_id = kind->buffer_id;
		buffer->size = size;
		buffer->global = kind->global;
		buffer->initialize = kind->initialize;
		buffer->read_only = kind->read_only;

		/* The page by the size. */
		if (size >= 0x200000U) {
			buffer->page_shift = 21U;
		} else if (size >= 0x10000U) {
			buffer->page_shift = 16U;
		} else {
			buffer->page_shift = 12U;
		}

		/* The alignment of the page, but the attribute buffer's size's. */
		if (kind->buffer_id == OBJECTS_BUFFER_ATTRIBUTE) {
			buffer->align_shift = objects_log2_up(size);
		} else {
			buffer->align_shift = buffer->page_shift;
		}
		count++;

		/* The access map has an unrestricted twin. */
		if (kind->buffer_id == OBJECTS_BUFFER_PRIV_MAP) {
			buffers[count] = *buffer;
			buffers[count].buffer_id = OBJECTS_BUFFER_UNRESTRICTED_MAP;
			count++;
		}
	}

	/* Succeeded: the list of buffers. */
	return count;
}

/*
 * Writes the promote call that hands a graphics channel its context buffers.
 *
 * The call goes to the subdevice; the channel is named by the driver's
 * client and its handle.  Each entry names the buffer, its address in the
 * channel's space (unless not mapped), and for a buffer the resource
 * manager fills, its VRAM address and size.  At most 16 entries.
 */
void
nvrtx_promote_params(
	uint8_t *params,
	uint32_t client,
	uint32_t channel,
	const struct nvrtx_promote_entry *entries,
	size_t count)
{
	const struct nvrtx_promote_entry *entry;
	size_t at;
	size_t index;

	/* The graphics engine, and the channel. */
	objects_clear(params, NVRTX_PROMOTE_PARAMS_BYTES);
	nvrtx_put32(params, OBJECTS_PROMOTE_ENGINE_AT, NVRTX_ENGINE_GRAPHICS);
	nvrtx_put32(params, OBJECTS_PROMOTE_CHAN_CLIENT_AT, client);
	nvrtx_put32(params, OBJECTS_PROMOTE_OBJECT_AT, channel);

	/* Keeps to the call's entry count. */
	if (count > OBJECTS_PROMOTE_MAX)
		count = OBJECTS_PROMOTE_MAX;
	nvrtx_put32(params, OBJECTS_PROMOTE_COUNT_AT, (uint32_t)count);

	/* Writes each entry. */
	for (index = 0U; index < count; index++) {
		entry = &entries[index];
		at = OBJECTS_PROMOTE_ENTRIES_AT + index * OBJECTS_PROMOTE_ENTRY_BYTES;
		nvrtx_put8(params, at + OBJECTS_PROMOTE_BUFFER_AT, (uint8_t)(entry->buffer_id & 0xffU));
		nvrtx_put8(params, at + OBJECTS_PROMOTE_BUFFER_AT + 1U, (uint8_t)((entry->buffer_id >> 8) & 0xffU));
		nvrtx_put8(params, at + OBJECTS_PROMOTE_INITIALIZE_AT, (uint8_t)(entry->initialize != 0));
		nvrtx_put8(params, at + OBJECTS_PROMOTE_NONMAPPED_AT, (uint8_t)(entry->nonmapped != 0));

		/* A mapped buffer's address in the channel's space. */
		if (entry->nonmapped == 0)
			nvrtx_put64(params, at + OBJECTS_PROMOTE_VIRTUAL_AT, entry->virtual_address);

		/* A buffer the resource manager fills: its VRAM address and size. */
		if (entry->initialize != 0) {
			nvrtx_put64(params, at + OBJECTS_PROMOTE_PHYSICAL_AT, entry->physical);
			nvrtx_put64(params, at + OBJECTS_PROMOTE_SIZE_AT, entry->size);
			nvrtx_put32(params, at + OBJECTS_PROMOTE_ATTRIBUTE_AT, OBJECTS_PROMOTE_ATTRIBUTE_VRAM);
		}
	}
}

/* Clears a range of bytes. */
static void
objects_clear(
	uint8_t *bytes,
	size_t length)
{
	size_t at;

	/* Zeroes each byte. */
	for (at = 0U; at < length; at++)
		bytes[at] = 0U;
}

/* Writes an incrementing method header for subchannel 0 (the host's methods) and returns where its data goes. */
static size_t
objects_method(
	uint8_t *push,
	size_t at,
	uint32_t method,
	uint32_t count)
{
	size_t data;

	/* The host's methods go on subchannel 0. */
	data = objects_method_on(push, at, 0U, method, count);

	/* Succeeded: the data follows the header. */
	return data;
}

/* Writes an incrementing method header for a subchannel and returns where its data goes. */
static size_t
objects_method_on(
	uint8_t *push,
	size_t at,
	uint32_t subchannel,
	uint32_t method,
	uint32_t count)
{
	/* The header: incrementing, the count, the subchannel, the method's word address. */
	nvrtx_put32(push, at, OBJECTS_METHOD_INCREMENT | (count << 16) | (subchannel << 13) | (method >> 2));

	/* Succeeded: the data follows the header. */
	return at + 4U;
}

/* Writes one memory description of the channel's parameters. */
static void
objects_memory(
	uint8_t *params,
	size_t at,
	uint64_t base,
	uint64_t size,
	uint32_t space,
	uint32_t cache)
{
	/* The base, the size, the address space and the cache attribute. */
	nvrtx_put64(params, at + OBJECTS_MEMORY_BASE_AT, base);
	nvrtx_put64(params, at + OBJECTS_MEMORY_SIZE_AT, size);
	nvrtx_put32(params, at + OBJECTS_MEMORY_SPACE_AT, space);
	nvrtx_put32(params, at + OBJECTS_MEMORY_CACHE_AT, cache);
}

/* Reports the smallest n with 2^n at least the value (the value is not 0). */
static uint32_t
objects_log2_up(
	uint64_t value)
{
	uint32_t shift;

	/* Grows the power of two until it holds the value. */
	shift = 0U;
	while (shift < 63U && (1ULL << shift) < value)
		shift++;

	/* Reports the exponent. */
	return shift;
}

/* Writes a fence's push buffer: the release as execute says, a system memory barrier, the non-stalling interrupt. */
static size_t
objects_fence(
	uint8_t *push,
	uint64_t semaphore,
	uint32_t value,
	uint32_t execute)
{
	size_t at;

	/* The semaphore's address and value. */
	at = objects_method(push, 0U, OBJECTS_HOST_SEM_ADDRESS_LOW, 3U);
	nvrtx_put32(push, at, (uint32_t)(semaphore & 0xfffffffcU));
	nvrtx_put32(push, at + 4U, (uint32_t)(semaphore >> 32));
	nvrtx_put32(push, at + 8U, value);
	at += 12U;

	/* Releases it once the work before it is done. */
	at = objects_method(push, at, OBJECTS_HOST_SEM_EXECUTE, 1U);
	nvrtx_put32(push, at, execute);
	at += 4U;

	/* Makes the release visible to the CPU: a system memory barrier. */
	at = objects_method(push, at, OBJECTS_HOST_MEM_OP_A, 4U);
	nvrtx_put32(push, at, 0U);
	nvrtx_put32(push, at + 4U, 0U);
	nvrtx_put32(push, at + 8U, 0U);
	nvrtx_put32(push, at + 12U, OBJECTS_MEM_OP_MEMBAR);
	at += 16U;

	/* Raises the non-stalling interrupt (the driver polls, so nothing waits on it). */
	at = objects_method(push, at, OBJECTS_HOST_NON_STALL, 1U);
	nvrtx_put32(push, at, 0U);
	at += 4U;

	/* Succeeded: the push buffer's length. */
	return at;
}
