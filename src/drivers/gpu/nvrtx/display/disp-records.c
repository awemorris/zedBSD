/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The records of the display channels (the takeover stage) that touch no
 * hardware.
 *
 * The push-buffer methods of the core and window channels, the hash-table
 * entries and context DMA objects of the display's instance memory, and the
 * resource manager's parameters for a display channel.  See nvrtx-disp.h.
 */

#include <stddef.h>
#include <stdint.h>

#include "drivers/gpu/nvrtx/display/nvrtx-disp.h"
#include "drivers/gpu/nvrtx/nvrtx-gsp.h"

/*
 * A method header: the count in bits 27:18, the method's word offset in bits
 * 13:2 (opcode 0: incrementing).
 *
 * Values referenced: NVIDIA open-gpu-doc classes/display/clc57d.h
 * (NVC57D_DMA_OPCODE_METHOD, NVC57D_DMA_METHOD_COUNT,
 * NVC57D_DMA_METHOD_OFFSET).
 */
#define RECORDS_HEADER_COUNT_SHIFT	18U
#define RECORDS_HEADER_METHOD_MASK	0x3ffcU

/*
 * The hash table's entry: the channel number goes in bits 31:25, the
 * object's offset from bit 9, the client in bits 13:0.
 *
 * Values referenced: Linux v6.19
 * drivers/gpu/drm/nouveau/nvkm/subdev/gsp/rm/r535/disp.c (r535_dmac_bind)
 * and drivers/gpu/drm/nouveau/nvkm/core/ramht.c (nvkm_ramht_update).
 */
#define RECORDS_RAMHT_CHID_SHIFT	25U
#define RECORDS_RAMHT_OBJECT_SHIFT	9U
#define RECORDS_RAMHT_CLIENT_MASK	0x3fffU

/* A context DMA object's first word: the VRAM target, read and write, small pages. */
#define RECORDS_CONTEXT_DMA_FLAGS	NVRTX_DISP_CONTEXT_DMA_FLAGS

/*
 * The push buffer's registration: system memory, snooped, PCI-coherent, on
 * subdevice 0.
 *
 * Values referenced: Linux v6.19
 * drivers/gpu/drm/nouveau/nvkm/subdev/gsp/rm/r570/disp.c
 * (r570_disp_chan_set_pushbuf) and r570/nvrm/disp.h (ADDR_SYSMEM,
 * PBTARGETAPERTURE).
 */
#define RECORDS_SPACE_SYSTEM		1U
#define RECORDS_CACHE_SNOOP		1U
#define RECORDS_APERTURE_COHERENT	3U
#define RECORDS_SUBDEVICE_FIRST		1U

/*
 * The registration's fields.
 *
 * Values referenced: NVIDIA open-gpu-kernel-modules 570.144
 * src/common/sdk/nvidia/inc/ctrl/ctrl2080/ctrl2080internal.h
 * (NV2080_CTRL_INTERNAL_DISPLAY_CHANNEL_PUSHBUFFER_PARAMS).
 */
#define RECORDS_PB_SPACE_AT		0U
#define RECORDS_PB_ADDRESS_AT		8U
#define RECORDS_PB_LIMIT_AT		16U
#define RECORDS_PB_SNOOP_AT		24U
#define RECORDS_PB_CLASS_AT		28U
#define RECORDS_PB_INSTANCE_AT		32U
#define RECORDS_PB_VALID_AT		36U
#define RECORDS_PB_APERTURE_AT		40U
#define RECORDS_PB_SUBDEVICE_AT		48U

/*
 * The channel allocation's fields.
 *
 * Values referenced: NVIDIA open-gpu-kernel-modules 570.144
 * src/common/sdk/nvidia/inc/nvos.h
 * (NV50VAIO_CHANNELDMA_ALLOCATION_PARAMETERS).
 */
#define RECORDS_CHANNEL_INSTANCE_AT	0U
#define RECORDS_CHANNEL_SUBDEVICE_AT	32U

/*
 * The output-resource assignment's fields (570.144's layout): the display,
 * the excluded SORs, the assigned list.
 *
 * Values referenced: NVIDIA open-gpu-kernel-modules 570.144
 * src/common/sdk/nvidia/inc/ctrl/ctrl0073/ctrl0073dfp.h
 * (NV0073_CTRL_DFP_ASSIGN_SOR_PARAMS,
 * NV0073_CTRL_CMD_DFP_ASSIGN_SOR_MAX_SORS).
 */
#define RECORDS_ASSIGN_DISPLAY_AT	4U
#define RECORDS_ASSIGN_EXCLUDE_AT	8U
#define RECORDS_ASSIGN_TAGGED_AT	40U
#define RECORDS_ASSIGN_TAGGED_BYTES	8U
#define RECORDS_ASSIGN_SOR_MAX		4U

/*
 * The windows the core's bounds and owners name: at most 8 on the Turing
 * display.
 *
 * Value referenced: Linux v6.19 drivers/gpu/drm/nouveau/dispnv50/corec57d.c
 * (corec57d_init, the windows it bounds).
 */
#define RECORDS_WINDOW_MAX		8U

/*
 * The window's format bounds: the packed RGB formats of 1, 2, 4 and 8 bytes.
 *
 * Value referenced: Linux v6.19 drivers/gpu/drm/nouveau/dispnv50/corec57d.c
 * (corec57d_init) and NVIDIA open-gpu-doc classes/display/clc57d.h.
 */
#define RECORDS_FORMAT_BOUNDS_RGB	0x0000000fU

/*
 * The window's usage bounds: 0x7fff pixels a line, the input LUT allowed,
 * two scaler taps, no upscaling.
 *
 * Value referenced: Linux v6.19 drivers/gpu/drm/nouveau/dispnv50/corec57d.c
 * (corec57d_init) and NVIDIA open-gpu-doc classes/display/clc57d.h.
 */
#define RECORDS_USAGE_BOUNDS		0x00117fffU

/*
 * The head's cursor turned off, with the A8R8G8B8 format named.
 *
 * Value referenced: Linux v6.19 drivers/gpu/drm/nouveau/dispnv50/headc37d.c
 * (headc37d_curs_clr) and NVIDIA open-gpu-doc classes/display/clc57d.h.
 */
#define RECORDS_CURSOR_OFF		0x000000cfU

/*
 * The core's notifier control: write the notifier at the offset (bits
 * 11:4), notify on.
 *
 * Values referenced: Linux v6.19 drivers/gpu/drm/nouveau/dispnv50/corec37d.c
 * (corec37d_update) and NVIDIA open-gpu-doc classes/display/clc57d.h
 * (NVC57D_SET_NOTIFIER_CONTROL).
 */
#define RECORDS_NOTIFY_ENABLE		0x00001000U
#define RECORDS_NOTIFIER_OFFSET_MASK	0x00000ff0U

/*
 * The head's usage bounds: 256x256 cursors, the output LUT allowed, two
 * output scaler taps, upscaling allowed.
 *
 * Value referenced: Linux v6.19 drivers/gpu/drm/nouveau/dispnv50/headc57d.c
 * (headc57d_mode) and NVIDIA open-gpu-doc classes/display/clc57d.h.
 */
#define RECORDS_HEAD_USAGE_BOUNDS	0x00001114U

/*
 * The head's processing: RGB, no chroma filter, the VESA range.
 *
 * Value referenced: Linux v6.19 drivers/gpu/drm/nouveau/dispnv50/headc57d.c
 * (headc57d_procamp) and NVIDIA open-gpu-doc classes/display/clc57d.h.
 */
#define RECORDS_PROCAMP_RGB		0x00000000U

/*
 * A progressive raster (the head control's structure).
 *
 * Value referenced: NVIDIA open-gpu-doc classes/display/clc57d.h
 * (NVC57D_HEAD_SET_CONTROL_STRUCTURE).
 */
#define RECORDS_CONTROL_PROGRESSIVE	0x00000000U

/*
 * The pixel clock's hertz (bits 30:0; bit 31 is the 1000/1001 adjustment).
 *
 * Value referenced: NVIDIA open-gpu-doc classes/display/clc57d.h
 * (NVC57D_HEAD_SET_PIXEL_CLOCK_FREQUENCY_HERTZ,
 * NVC57D_HEAD_SET_PIXEL_CLOCK_FREQUENCY_ADJ1000DIV1001).
 */
#define RECORDS_CLOCK_HERTZ_MASK	0x7fffffffU

/*
 * An update that releases the state the channel gathered.
 *
 * Value referenced: Linux v6.19 drivers/gpu/drm/nouveau/dispnv50/corec37d.c
 * (corec37d_update) and wndwc37e.c (wndwc37e_update).
 */
#define RECORDS_UPDATE			0x00000001U

/*
 * The window's image: one frame between presents and a non-tearing flip;
 * pitch layout; A8R8G8B8.
 *
 * Values referenced: Linux v6.19 drivers/gpu/drm/nouveau/dispnv50/wndwc57e.c
 * (wndwc57e_image_set) and NVIDIA open-gpu-doc classes/display/clc57e.h.
 */
#define RECORDS_PRESENT_NON_TEARING	0x00000001U
#define RECORDS_STORAGE_PITCH		0x00000010U
#define RECORDS_PITCH_SHIFT		6U
#define RECORDS_OFFSET_SHIFT		8U

/*
 * An opaque window: depth 255, constant alpha 255, source times 1 over
 * destination times 0.
 *
 * Values referenced: Linux v6.19 drivers/gpu/drm/nouveau/dispnv50/wndwc37e.c
 * (wndwc37e_blend_set) and NVIDIA open-gpu-doc classes/display/clc57e.h.
 */
#define RECORDS_COMPOSITION_DEPTH	0x00000ff0U
#define RECORDS_CONSTANT_ALPHA		0x000000ffU
#define RECORDS_FACTOR_SELECT		0x00004422U
#define RECORDS_KEY_FULL_RANGE		0xffff0000U

/*
 * The format coefficients of the identity matrix (1.0 is 0x10000), 3 rows of
 * 4.
 *
 * Values referenced: Linux v6.19 drivers/gpu/drm/nouveau/dispnv50/wndwc57e.c
 * (wndwc57e_csc_clr).
 */
#define RECORDS_COEFFICIENT_COUNT	12U
#define RECORDS_COEFFICIENT_ONE		0x00010000U

/*
 * A shown cursor: enabled, A8R8G8B8, 64 by 64, its hot spot at the top left,
 * no de-gamma.
 *
 * Value referenced: Linux v6.19 drivers/gpu/drm/nouveau/dispnv50/headc37d.c
 * (headc37d_curs_set) and NVIDIA open-gpu-doc classes/display/clc57d.h.
 */
#define RECORDS_CURSOR_ON		0x800001cfU

/*
 * The cursor's blending: K1 255, the cursor times K1 over the picture times
 * one less the cursor's alpha (premultiplied alpha).
 *
 * Value referenced: Linux v6.19 drivers/gpu/drm/nouveau/dispnv50/headc37d.c
 * (headc37d_curs_set) and NVIDIA open-gpu-doc classes/display/clc57d.h.
 */
#define RECORDS_CURSOR_COMPOSITION	0x000072ffU

/*
 * The scale of the cursor's colours: 255/256, where 0x1000 is one and the
 * colours follow the scale linearly.
 *
 * As the head's CRCs showed on a TU106: a window's 8-bit channel c enters
 * the composition as c/256 through the identity input LUT (disp-first.c,
 * the DIRECT10 table whose entry i is i/1024), and this scale puts a
 * cursor's channel c at the same value (the CRCs of a white, grey or black
 * cursor equal those of the same square drawn into the window).  The
 * reset value, one, puts a channel of 255 at the top of the output LUT,
 * which shows it black: a white cursor came out black, its black stayed
 * black, and a grey one came out lighter than the window's grey.
 *
 * Value referenced: NVIDIA open-gpu-doc classes/display/clc57d.h
 * (NVC57D_HEAD_SET_CURSOR_COLOR_NORM_SCALE, 16 bits; the same method in
 * clc67d.h and clc77d.h).
 */
#define RECORDS_CURSOR_NORM_SCALE	0x00000ff0U

/*
 * Where a PIO channel's allocation names its instance.
 *
 * Value referenced: NVIDIA open-gpu-kernel-modules 570.144
 * src/common/sdk/nvidia/inc/nvos.h
 * (NV50VAIO_CHANNELPIO_ALLOCATION_PARAMETERS).
 */
#define RECORDS_PIO_INSTANCE_AT		0U

/*
 * The cursor's position fields are 16 bits each; a position is held inside
 * them.
 *
 * Values referenced: NVIDIA open-gpu-doc classes/display/clc57a.h
 * (NVC57A_SET_CURSOR_HOT_SPOT_POINT_OUT_X,
 * NVC57A_SET_CURSOR_HOT_SPOT_POINT_OUT_Y).
 */
#define RECORDS_POINT_MIN		(-32768)
#define RECORDS_POINT_MAX		32767
#define RECORDS_POINT_FIELD_MASK	0xffffU
#define RECORDS_POINT_Y_SHIFT		16U

/* The red and blue bytes of a 32-bit pixel, which RGBA and A8R8G8B8 hold in swapped places. */
#define RECORDS_PIXEL_LOW_BYTE		0x000000ffU
#define RECORDS_PIXEL_THIRD_BYTE	0x00ff0000U
#define RECORDS_PIXEL_KEPT_BYTES	0xff00ff00U
#define RECORDS_PIXEL_SWAP_SHIFT	16U

/* The colours of the test bands, A8R8G8B8: white, yellow, cyan, green, magenta, red, blue, black. */
#define RECORDS_BAND_COUNT		8U

/*
 * The head methods the takeover copies from the armed state.
 *
 * The same methods a head is given on a mode set (the view, the mode, the
 * output resource, the colour processing and the display id), so the head
 * keeps the timing and output the firmware gave it.
 *
 * Procedure referenced: Linux v6.19 drivers/gpu/drm/nouveau/dispnv50/head.c
 * (nv50_head_flush_set).
 */
const uint32_t nvrtx_disp_head_copied[NVRTX_DISP_HEAD_COPIED] = {
	NVRTX_HEAD_VIEWPORT_SIZE_IN,
	NVRTX_HEAD_VIEWPORT_SIZE_OUT,
	NVRTX_HEAD_RASTER_SIZE,
	NVRTX_HEAD_RASTER_SYNC_END,
	NVRTX_HEAD_RASTER_BLANK_END,
	NVRTX_HEAD_RASTER_BLANK_START,
	NVRTX_HEAD_RASTER_VERT_BLANK2,
	NVRTX_HEAD_CONTROL,
	NVRTX_HEAD_PIXEL_CLOCK,
	NVRTX_HEAD_PIXEL_CLOCK_MAX,
	NVRTX_HEAD_USAGE_BOUNDS,
	NVRTX_HEAD_DITHER_CONTROL,
	NVRTX_HEAD_PROCAMP,
	NVRTX_HEAD_DISPLAY_ID,
	NVRTX_HEAD_CONTROL_OUTPUT_RESOURCE
};

/*
 * The colours of the test bands, top to bottom.
 *
 * Read-only for the life of the kernel.
 */
static const uint32_t records_bands[RECORDS_BAND_COUNT] = {
	0xffffffffU,
	0xffffff00U,
	0xff00ffffU,
	0xff00ff00U,
	0xffff00ffU,
	0xffff0000U,
	0xff0000ffU,
	0xff000000U
};

static void records_clear(uint8_t *bytes, size_t length);

/*
 * Starts a push buffer in caller-owned memory of capacity words.
 */
void
nvrtx_push_init(
	struct nvrtx_push *push,
	uint8_t *bytes,
	uint32_t capacity)
{
	/* The buffer starts empty. */
	push->bytes = bytes;
	push->capacity = capacity;
	push->count = 0U;
	push->overflow = 0;
}

/*
 * Writes one method header and its data words (incrementing methods).
 *
 * A method that does not fit writes nothing and sets the overflow flag.
 */
void
nvrtx_push_method(
	struct nvrtx_push *push,
	uint32_t method,
	const uint32_t *values,
	uint32_t count)
{
	uint32_t header;
	uint32_t index;

	/* A run too long for one header, or one past the buffer's end, is refused. */
	if (count > NVRTX_DISP_METHOD_COUNT_MAX) {
		push->overflow = 1;
		return;
	}

	if (push->count + 1U + count > push->capacity) {
		push->overflow = 1;
		return;
	}

	/* Writes the header: the count and the method's word offset. */
	header = (count << RECORDS_HEADER_COUNT_SHIFT) | (method & RECORDS_HEADER_METHOD_MASK);
	nvrtx_put32(push->bytes, (size_t)push->count * 4U, header);
	push->count++;

	/* Writes the data words, one for each method from the first on. */
	for (index = 0U; index < count; index++) {
		nvrtx_put32(push->bytes, (size_t)push->count * 4U, values[index]);
		push->count++;
	}
}

/*
 * Writes one method with one data word.
 */
void
nvrtx_push_one(
	struct nvrtx_push *push,
	uint32_t method,
	uint32_t value)
{
	/* A run of one. */
	nvrtx_push_method(push, method, &value, 1U);
}

/*
 * Writes a jump to a byte offset of the push buffer (the wind of a display
 * channel): the channel's fetch goes on from there.
 *
 * A jump that does not fit writes nothing and sets the overflow flag.
 *
 * Procedure referenced: Linux v6.19 drivers/gpu/drm/nouveau/dispnv50/disp.c
 * (nv50_dmac_wind).
 */
void
nvrtx_push_jump(
	struct nvrtx_push *push,
	uint32_t offset)
{
	/* The jump takes one word. */
	if (push->count + 1U > push->capacity) {
		push->overflow = 1;
		return;
	}

	/* The opcode and the word-aligned offset. */
	nvrtx_put32(push->bytes, (size_t)push->count * 4U, NVRTX_DISP_JUMP | (offset & NVRTX_DISP_JUMP_OFFSET_MASK));
	push->count++;
}

/*
 * Reports the hash-table slot an object's handle starts its search at.
 *
 * The handle is folded 10 bits at a time, and the channel number moves the
 * slot, for the display's 1024-entry table.
 *
 * Procedure referenced: Linux v6.19 drivers/gpu/drm/nouveau/nvkm/core/ramht.c
 * (nvkm_ramht_hash).
 */
uint32_t
nvrtx_disp_ramht_hash(
	uint32_t chid,
	uint32_t handle)
{
	uint32_t hash;
	uint32_t rest;

	/* Folds the handle into 10 bits. */
	hash = 0U;
	rest = handle;
	while (rest != 0U) {
		hash ^= rest & (NVRTX_DISP_RAMHT_ENTRIES - 1U);
		rest >>= NVRTX_DISP_RAMHT_BITS;
	}

	/* Mixes the channel in above the low 6 bits. */
	hash ^= chid << (NVRTX_DISP_RAMHT_BITS - 4U);

	/* Succeeded: the slot, inside the table. */
	return hash & (NVRTX_DISP_RAMHT_ENTRIES - 1U);
}

/*
 * Writes one hash-table entry: the handle, then the channel, client and object's offset.
 *
 * The client is the low bits of the resource-manager client that owns the
 * channel; the object's offset is from the start of the instance memory.
 */
void
nvrtx_disp_ramht_entry(
	uint8_t *entry,
	uint32_t handle,
	uint32_t chid,
	uint32_t client,
	uint32_t object_offset)
{
	uint32_t context;

	/* Composes the context word. */
	context = chid << RECORDS_RAMHT_CHID_SHIFT;
	context |= client & RECORDS_RAMHT_CLIENT_MASK;
	context |= object_offset << RECORDS_RAMHT_OBJECT_SHIFT;

	/* Writes the handle the method names, then the context. */
	nvrtx_put32(entry, 0U, handle);
	nvrtx_put32(entry, 4U, context);
}

/*
 * Writes one context DMA object over a range of the VRAM (start to limit, inclusive).
 */
void
nvrtx_disp_context_dma(
	uint8_t *object,
	uint64_t start,
	uint64_t limit)
{
	/* The VRAM, read and write, small pages. */
	nvrtx_disp_context_dma_flags(object, RECORDS_CONTEXT_DMA_FLAGS, start, limit);
}

/*
 * Writes one context DMA object with its first word given (the target,
 * the access and the page size) over a range of the VRAM.
 */
void
nvrtx_disp_context_dma_flags(
	uint8_t *object,
	uint32_t flags,
	uint64_t start,
	uint64_t limit)
{
	uint64_t start_units;
	uint64_t limit_units;

	/* The range is kept in 256-byte units. */
	start_units = start >> 8;
	limit_units = limit >> 8;

	/* Writes the flags, the start and the limit; the last word is left zero. */
	records_clear(object, NVRTX_DISP_CONTEXT_DMA_BYTES);
	nvrtx_put32(object, 0x00U, flags);
	nvrtx_put32(object, 0x04U, (uint32_t)start_units);
	nvrtx_put32(object, 0x08U, (uint32_t)(start_units >> 32));
	nvrtx_put32(object, 0x0cU, (uint32_t)limit_units);
	nvrtx_put32(object, 0x10U, (uint32_t)(limit_units >> 32));
}

/*
 * Writes the registration of a channel's push buffer in coherent system memory.
 */
void
nvrtx_disp_pushbuffer_params(
	uint8_t *params,
	uint64_t bus_address,
	uint32_t size,
	uint32_t class_number,
	uint32_t instance)
{
	/* Names the memory: system memory, its address and limit, snooped and PCI-coherent. */
	records_clear(params, NVRTX_DISP_PUSHBUFFER_BYTES);
	nvrtx_put32(params, RECORDS_PB_SPACE_AT, RECORDS_SPACE_SYSTEM);
	nvrtx_put64(params, RECORDS_PB_ADDRESS_AT, bus_address);
	nvrtx_put64(params, RECORDS_PB_LIMIT_AT, (uint64_t)size - 1U);
	nvrtx_put32(params, RECORDS_PB_SNOOP_AT, RECORDS_CACHE_SNOOP);
	nvrtx_put32(params, RECORDS_PB_APERTURE_AT, RECORDS_APERTURE_COHERENT);

	/* Names the channel it belongs to; the size field's 0 is 4 KiB. */
	nvrtx_put32(params, RECORDS_PB_CLASS_AT, class_number);
	nvrtx_put32(params, RECORDS_PB_INSTANCE_AT, instance);
	params[RECORDS_PB_VALID_AT] = 1U;
	nvrtx_put32(params, RECORDS_PB_SUBDEVICE_AT, RECORDS_SUBDEVICE_FIRST);
}

/*
 * Writes a display channel's allocation: its instance, put at 0, subdevice 0.
 */
void
nvrtx_disp_channel_params(
	uint8_t *params,
	uint32_t instance)
{
	/* The instance and the subdevice; the push buffer is the one registered. */
	records_clear(params, NVRTX_DISP_CHANNEL_BYTES);
	nvrtx_put32(params, RECORDS_CHANNEL_INSTANCE_AT, instance);
	nvrtx_put32(params, RECORDS_CHANNEL_SUBDEVICE_AT, RECORDS_SUBDEVICE_FIRST);
}

/*
 * Writes the push-buffer registration of a PIO channel (the cursor's): no
 * memory and not valid, only the class, the instance and subdevice 0, the
 * registration of a channel without a buffer.
 *
 * Procedure referenced: Linux v6.19
 * drivers/gpu/drm/nouveau/nvkm/subdev/gsp/rm/r570/disp.c
 * (r570_disp_chan_set_pushbuf).
 */
void
nvrtx_disp_pio_pushbuffer_params(
	uint8_t *params,
	uint32_t class_number,
	uint32_t instance)
{
	/* Every field not named is zero: no address space, no address, no limit, not valid. */
	records_clear(params, NVRTX_DISP_PUSHBUFFER_BYTES);
	nvrtx_put32(params, RECORDS_PB_CLASS_AT, class_number);
	nvrtx_put32(params, RECORDS_PB_INSTANCE_AT, instance);
	nvrtx_put32(params, RECORDS_PB_SUBDEVICE_AT, RECORDS_SUBDEVICE_FIRST);
}

/*
 * Writes a PIO channel's allocation: its instance (the head), no notifier, no
 * pointer.
 *
 * Procedure referenced: Linux v6.19
 * drivers/gpu/drm/nouveau/nvkm/subdev/gsp/rm/r535/disp.c (r535_curs_init).
 */
void
nvrtx_disp_pio_channel_params(
	uint8_t *params,
	uint32_t instance)
{
	/* The instance alone. */
	records_clear(params, NVRTX_DISP_PIO_CHANNEL_BYTES);
	nvrtx_put32(params, RECORDS_PIO_INSTANCE_AT, instance);
}

/*
 * Writes the request that assigns an output resource to one display (the
 * acquire of a SOR under the GSP): subdevice 0, the display, the SORs not to
 * use.
 *
 * Procedure referenced: Linux v6.19
 * drivers/gpu/drm/nouveau/nvkm/subdev/gsp/rm/r535/disp.c (r535_outp_acquire).
 */
void
nvrtx_disp_assign_sor_params(
	uint8_t *params,
	uint32_t display_id,
	uint32_t exclude_mask)
{
	/* Every field not named is zero: no slave display, no forced link, no flags. */
	records_clear(params, NVRTX_DISP_ASSIGN_SOR_BYTES);
	nvrtx_put32(params, RECORDS_ASSIGN_DISPLAY_AT, display_id);
	params[RECORDS_ASSIGN_EXCLUDE_AT] = (uint8_t)exclude_mask;
}

/*
 * Reports the SOR the reply assigned to a display: the first of the tagged
 * list whose display mask has it, or NVRTX_DISP_SOR_NONE.
 */
uint32_t
nvrtx_disp_assign_sor_result(
	const uint8_t *params,
	uint32_t display_id)
{
	uint32_t sor;
	uint32_t mask;

	/* Walks the tagged list in SOR order. */
	for (sor = 0U; sor < RECORDS_ASSIGN_SOR_MAX; sor++) {
		mask = nvrtx_get32(params, RECORDS_ASSIGN_TAGGED_AT + sor * RECORDS_ASSIGN_TAGGED_BYTES);
		if ((mask & display_id) != 0U)
			return sor;
	}

	/* No SOR drives the display. */
	return NVRTX_DISP_SOR_NONE;
}

/*
 * Writes the core channel's start: its notifier object and every window's bounds.
 */
void
nvrtx_disp_core_init(
	struct nvrtx_push *push,
	uint32_t window_mask)
{
	uint32_t bounds[2];
	uint32_t window;
	uint32_t base;

	/* The notifiers are read through the sync object. */
	nvrtx_push_one(push, NVRTX_CORE_SET_CONTEXT_DMA_NOTIFIER, NVRTX_DISP_HANDLE_SYNC);

	/* Allows the packed RGB formats (none rotated) and the usage every window may need. */
	for (window = 0U; window < RECORDS_WINDOW_MAX; window++) {
		/* A window the hardware lacks takes no method. */
		if ((window_mask & (1U << window)) == 0U)
			continue;

		base = window * NVRTX_CORE_WINDOW_STRIDE;
		bounds[0] = RECORDS_FORMAT_BOUNDS_RGB;
		bounds[1] = 0U;
		nvrtx_push_method(push, NVRTX_CORE_WINDOW_FORMAT_BOUNDS + base, bounds, 2U);
		nvrtx_push_one(push, NVRTX_CORE_WINDOW_USAGE_BOUNDS + base, RECORDS_USAGE_BOUNDS);
	}
}

/*
 * Writes one output resource's control word.
 */
void
nvrtx_disp_core_sor(
	struct nvrtx_push *push,
	uint32_t sor,
	uint32_t control)
{
	/* The owner heads and the protocol. */
	nvrtx_push_one(push, NVRTX_CORE_SOR_SET_CONTROL + sor * NVRTX_CORE_SOR_STRIDE, control);
}

/*
 * Gives every window to its head: windows 2h and 2h + 1 belong to head h.
 */
void
nvrtx_disp_core_owners(
	struct nvrtx_push *push,
	uint32_t window_mask)
{
	uint32_t window;

	/* Names the owner head of each window that exists. */
	for (window = 0U; window < RECORDS_WINDOW_MAX; window++) {
		/* A window the hardware lacks takes no method. */
		if ((window_mask & (1U << window)) == 0U)
			continue;

		nvrtx_push_one(push, NVRTX_CORE_WINDOW_SET_CONTROL + window * NVRTX_CORE_WINDOW_STRIDE, window >> 1);
	}
}

/*
 * Turns one head's cursor and output LUT off, so no object of the firmware's is named.
 */
void
nvrtx_disp_core_head_planes_off(
	struct nvrtx_push *push,
	uint32_t head)
{
	uint32_t base;

	/* The cursor off and without its object; the output LUT without its object. */
	base = NVRTX_CORE_HEAD_FIRST + head * NVRTX_CORE_HEAD_STRIDE;
	nvrtx_push_one(push, base + NVRTX_HEAD_CONTROL_CURSOR, RECORDS_CURSOR_OFF);
	nvrtx_push_one(push, base + NVRTX_HEAD_CONTEXT_DMA_CURSOR, 0U);
	nvrtx_push_one(push, base + NVRTX_HEAD_CONTEXT_DMA_OLUT, 0U);
}

/*
 * Writes one head's display id: the display (one bit per display) it drives, or 0 for none.
 */
void
nvrtx_disp_core_display_id(
	struct nvrtx_push *push,
	uint32_t head,
	uint32_t display_id)
{
	uint32_t base;

	/* The first display-id slot of the head. */
	base = NVRTX_CORE_HEAD_FIRST + head * NVRTX_CORE_HEAD_STRIDE;
	nvrtx_push_one(push, base + NVRTX_HEAD_DISPLAY_ID, display_id);
}

/*
 * Writes one head's mode set in the order of a head's flush: the viewport,
 * the raster and clock, the dither, the processing and the output resource's
 * control.
 *
 * Procedure referenced: Linux v6.19 drivers/gpu/drm/nouveau/dispnv50/head.c
 * (nv50_head_flush_set).
 */
void
nvrtx_disp_core_head_mode(
	struct nvrtx_push *push,
	uint32_t head,
	const struct nvrtx_disp_mode *mode)
{
	uint32_t base;
	uint32_t raster[4];

	base = NVRTX_CORE_HEAD_FIRST + head * NVRTX_CORE_HEAD_STRIDE;

	/* The view: the visible size in and out, unscaled. */
	nvrtx_push_one(push, base + NVRTX_HEAD_VIEWPORT_SIZE_IN, mode->active);
	nvrtx_push_one(push, base + NVRTX_HEAD_VIEWPORT_SIZE_OUT, mode->active);

	/* The mode: the raster's four words in one run, the second vertical blank, progressive. */
	raster[0] = mode->raster_size;
	raster[1] = mode->sync_end;
	raster[2] = mode->blank_end;
	raster[3] = mode->blank_start;
	nvrtx_push_method(push, base + NVRTX_HEAD_RASTER_SIZE, raster, 4U);
	nvrtx_push_one(push, base + NVRTX_HEAD_RASTER_VERT_BLANK2, mode->vert_blank2);
	nvrtx_push_one(push, base + NVRTX_HEAD_CONTROL, RECORDS_CONTROL_PROGRESSIVE);

	/*
	 * The pixel clock, its maximum the same, and the head's usage bounds.
	 */
	nvrtx_push_one(push, base + NVRTX_HEAD_PIXEL_CLOCK, mode->clock_hz & RECORDS_CLOCK_HERTZ_MASK);
	nvrtx_push_one(push, base + NVRTX_HEAD_PIXEL_CLOCK_MAX, mode->clock_hz & RECORDS_CLOCK_HERTZ_MASK);
	nvrtx_push_one(push, base + NVRTX_HEAD_USAGE_BOUNDS, RECORDS_HEAD_USAGE_BOUNDS);

	/* The dither, the processing and the output resource's control. */
	nvrtx_push_one(push, base + NVRTX_HEAD_DITHER_CONTROL, mode->dither);
	nvrtx_push_one(push, base + NVRTX_HEAD_PROCAMP, RECORDS_PROCAMP_RGB);
	nvrtx_push_one(push, base + NVRTX_HEAD_CONTROL_OUTPUT_RESOURCE, mode->output_resource);
}

/*
 * Derives a head's mode from the state the firmware armed it with.
 *
 * The visible size is the blank start less the blank end (as the readout of
 * display-client shows it), not the armed viewport, which may be a scaled one.
 */
void
nvrtx_disp_mode_from_armed(
	const uint32_t *armed,
	struct nvrtx_disp_mode *mode)
{
	uint32_t width;
	uint32_t height;
	uint32_t blank_end;
	uint32_t blank_start;

	/* The visible size from the blank's start and end. */
	blank_end = armed[NVRTX_DISP_ARMED_BLANK_END];
	blank_start = armed[NVRTX_DISP_ARMED_BLANK_START];
	width = (blank_start & 0xffffU) - (blank_end & 0xffffU);
	height = (blank_start >> 16) - (blank_end >> 16);
	mode->active = (width & 0xffffU) | ((height & 0xffffU) << 16);

	/* The raster and the clock as armed. */
	mode->raster_size = armed[NVRTX_DISP_ARMED_RASTER_SIZE];
	mode->sync_end = armed[NVRTX_DISP_ARMED_SYNC_END];
	mode->blank_end = blank_end;
	mode->blank_start = blank_start;
	mode->vert_blank2 = armed[NVRTX_DISP_ARMED_VERT_BLANK2];
	mode->clock_hz = armed[NVRTX_DISP_ARMED_PIXEL_CLOCK];

	/* The dither and the output resource's control (its sync polarities and depth) as armed. */
	mode->dither = armed[NVRTX_DISP_ARMED_DITHER];
	mode->output_resource = armed[NVRTX_DISP_ARMED_OUTPUT_RESOURCE];
}

/*
 * Writes the core's update with its notifier, interlocked with the windows in the mask.
 */
void
nvrtx_disp_core_update(
	struct nvrtx_push *push,
	uint32_t notifier_offset,
	uint32_t window_interlock)
{
	uint32_t interlock[2];

	/* Asks for the notifier at its offset in the sync object. */
	nvrtx_push_one(push, NVRTX_CORE_SET_NOTIFIER_CONTROL, RECORDS_NOTIFY_ENABLE | (notifier_offset & RECORDS_NOTIFIER_OFFSET_MASK));

	/* No cursor interlock; the windows the update waits for. */
	interlock[0] = 0U;
	interlock[1] = window_interlock;
	nvrtx_push_method(push, NVRTX_CORE_SET_INTERLOCK_FLAGS, interlock, 2U);

	/* Releases the gathered state, then turns the notifier off again. */
	nvrtx_push_one(push, NVRTX_CORE_UPDATE, RECORDS_UPDATE);
	nvrtx_push_one(push, NVRTX_CORE_SET_NOTIFIER_CONTROL, 0U);
}

/*
 * Writes one head's cursor on: its control and blending, its colours'
 * scale, the object it is read through and its VRAM offset.
 *
 * Procedure referenced: Linux v6.19 drivers/gpu/drm/nouveau/dispnv50/headc37d.c
 * (headc37d_curs_set).
 */
void
nvrtx_disp_core_cursor_set(
	struct nvrtx_push *push,
	uint32_t head,
	uint32_t handle,
	uint64_t offset)
{
	uint32_t control[2];
	uint32_t base;

	/* The head's block of methods. */
	base = NVRTX_CORE_HEAD_FIRST + head * NVRTX_CORE_HEAD_STRIDE;

	/* Shown, 64 by 64 A8R8G8B8, blended as premultiplied alpha. */
	control[0] = RECORDS_CURSOR_ON;
	control[1] = RECORDS_CURSOR_COMPOSITION;
	nvrtx_push_method(push, base + NVRTX_HEAD_CONTROL_CURSOR, control, 2U);

	/* Its colours on the windows' scale. */
	nvrtx_push_one(push, base + NVRTX_HEAD_CURSOR_COLOR_NORM_SCALE, RECORDS_CURSOR_NORM_SCALE);

	/* Read through the object, from the offset in 256-byte units. */
	nvrtx_push_one(push, base + NVRTX_HEAD_CONTEXT_DMA_CURSOR, handle);
	nvrtx_push_one(push, base + NVRTX_HEAD_OFFSET_CURSOR, (uint32_t)(offset >> RECORDS_OFFSET_SHIFT));
}

/*
 * Writes one head's cursor off and without its object.
 *
 * Procedure referenced: Linux v6.19 drivers/gpu/drm/nouveau/dispnv50/headc37d.c
 * (headc37d_curs_clr).
 */
void
nvrtx_disp_core_cursor_clear(
	struct nvrtx_push *push,
	uint32_t head)
{
	uint32_t base;

	/* The head's block of methods. */
	base = NVRTX_CORE_HEAD_FIRST + head * NVRTX_CORE_HEAD_STRIDE;

	/* Off (its format stays A8R8G8B8), then no object. */
	nvrtx_push_one(push, base + NVRTX_HEAD_CONTROL_CURSOR, RECORDS_CURSOR_OFF);
	nvrtx_push_one(push, base + NVRTX_HEAD_CONTEXT_DMA_CURSOR, 0U);
}

/*
 * Writes the core's update with its notifier and no interlock: the update
 * that shows or hides the cursor (for a cursor whose position changed, or
 * one turned off).  The cursor channel's own update is not interlocked with
 * the core (its SET_INTERLOCK_FLAGS is never written), so a core update
 * interlocked with the cursor would wait for it forever.
 *
 * Procedure referenced: Linux v6.19 drivers/gpu/drm/nouveau/dispnv50/corec37d.c
 * (corec37d_update).
 */
void
nvrtx_disp_core_update_cursor(
	struct nvrtx_push *push,
	uint32_t notifier_offset)
{
	uint32_t interlock[2];

	/* Asks for the notifier at its offset in the sync object. */
	nvrtx_push_one(push, NVRTX_CORE_SET_NOTIFIER_CONTROL, RECORDS_NOTIFY_ENABLE | (notifier_offset & RECORDS_NOTIFIER_OFFSET_MASK));

	/* No cursor, no window. */
	interlock[0] = 0U;
	interlock[1] = 0U;
	nvrtx_push_method(push, NVRTX_CORE_SET_INTERLOCK_FLAGS, interlock, 2U);

	/* Releases the gathered state, then turns the notifier off again. */
	nvrtx_push_one(push, NVRTX_CORE_UPDATE, RECORDS_UPDATE);
	nvrtx_push_one(push, NVRTX_CORE_SET_NOTIFIER_CONTROL, 0U);
}

/*
 * Reports the cursor channel's position word: x in bits 15:0 and y in bits
 * 31:16, each a 16-bit two's complement (a cursor partly left of or above
 * the output has a negative position).  A position outside 16 bits is held
 * at its nearest end.
 *
 * Procedure referenced: Linux v6.19 drivers/gpu/drm/nouveau/dispnv50/cursc37a.c
 * (cursc37a_point).
 */
uint32_t
nvrtx_disp_cursor_point(
	int32_t x,
	int32_t y)
{
	uint32_t field_x;
	uint32_t field_y;

	/* Holds x inside the field. */
	if (x < RECORDS_POINT_MIN)
		x = RECORDS_POINT_MIN;
	else if (x > RECORDS_POINT_MAX)
		x = RECORDS_POINT_MAX;

	/* Holds y inside the field. */
	if (y < RECORDS_POINT_MIN)
		y = RECORDS_POINT_MIN;
	else if (y > RECORDS_POINT_MAX)
		y = RECORDS_POINT_MAX;

	/* Each one's low 16 bits. */
	field_x = (uint32_t)x & RECORDS_POINT_FIELD_MASK;
	field_y = (uint32_t)y & RECORDS_POINT_FIELD_MASK;

	/* Succeeded: the word the channel takes. */
	return field_x | (field_y << RECORDS_POINT_Y_SHIFT);
}

/*
 * Reports one pixel of a cursor image as the cursor's A8R8G8B8 word: a
 * BGRA8888 pixel is that already, an RGBA8888 one has red and blue swapped.
 */
uint32_t
nvrtx_disp_cursor_pixel(
	uint32_t pixel,
	int rgba)
{
	uint32_t red;
	uint32_t blue;

	/* B, G, R, A in memory is the cursor's own order. */
	if (rgba == 0)
		return pixel;

	/* R, G, B, A: red in the low byte and blue in the third change places. */
	red = pixel & RECORDS_PIXEL_LOW_BYTE;
	blue = (pixel & RECORDS_PIXEL_THIRD_BYTE) >> RECORDS_PIXEL_SWAP_SHIFT;

	/* Succeeded: green and alpha stay, red goes up, blue comes down. */
	return (pixel & RECORDS_PIXEL_KEPT_BYTES) | (red << RECORDS_PIXEL_SWAP_SHIFT) | blue;
}

/*
 * Reports how many frames a head's 16-bit frame counter moved from one
 * reading to the next (modulo its wrap: a gap longer than 65535 frames
 * counts short, never backwards).
 */
uint32_t
nvrtx_disp_frame_advance(
	uint32_t last,
	uint32_t now)
{
	/* The counter's difference within its 16 bits. */
	return (now - last) & NVRTX_DISP_FRAME_COUNT_MASK;
}

/*
 * Writes a window's image: one surface shown whole at its own size.
 */
void
nvrtx_disp_window_image(
	struct nvrtx_push *push,
	const struct nvrtx_disp_surface *surface)
{
	uint32_t size;
	uint32_t layout[4];

	/* The surface's size, used for its size, the input size and the output size. */
	size = surface->width | (surface->height << 16);

	/* Flips on a frame boundary, at most one each frame. */
	nvrtx_push_one(push, NVRTX_WINDOW_SET_PRESENT_CONTROL, RECORDS_PRESENT_NON_TEARING);

	/* The size, the pitch layout, the format and the pitch in 64-byte units. */
	layout[0] = size;
	layout[1] = RECORDS_STORAGE_PITCH;
	layout[2] = surface->format;
	layout[3] = surface->pitch >> RECORDS_PITCH_SHIFT;
	nvrtx_push_method(push, NVRTX_WINDOW_SET_SIZE, layout, 4U);

	/* Where it lies: the object, then the offset in 256-byte units. */
	nvrtx_push_one(push, NVRTX_WINDOW_SET_CONTEXT_DMA_ISO, surface->handle);
	nvrtx_push_one(push, NVRTX_WINDOW_SET_OFFSET, (uint32_t)(surface->offset >> RECORDS_OFFSET_SHIFT));

	/* All of it, unscaled. */
	nvrtx_push_one(push, NVRTX_WINDOW_SET_POINT_IN, 0U);
	nvrtx_push_one(push, NVRTX_WINDOW_SET_SIZE_IN, size);
	nvrtx_push_one(push, NVRTX_WINDOW_SET_SIZE_OUT, size);
}

/*
 * Writes a window's colour defaults: no input LUT, the identity matrix, opaque blending.
 */
void
nvrtx_disp_window_defaults(
	struct nvrtx_push *push)
{
	uint32_t coefficients[RECORDS_COEFFICIENT_COUNT];
	uint32_t blend[7];
	uint32_t index;

	/* No input LUT object. */
	nvrtx_push_one(push, NVRTX_WINDOW_SET_CONTEXT_DMA_ILUT, 0U);

	/* The identity: 1.0 on the diagonal of the 3 by 4 matrix. */
	for (index = 0U; index < RECORDS_COEFFICIENT_COUNT; index++)
		coefficients[index] = 0U;
	coefficients[0] = RECORDS_COEFFICIENT_ONE;
	coefficients[5] = RECORDS_COEFFICIENT_ONE;
	coefficients[10] = RECORDS_COEFFICIENT_ONE;
	nvrtx_push_method(push, NVRTX_WINDOW_SET_FMT_COEFFICIENT, coefficients, RECORDS_COEFFICIENT_COUNT);

	/* Opaque: no colour key, the top depth, alpha 255, the source over nothing, keys over the full range. */
	blend[0] = RECORDS_COMPOSITION_DEPTH;
	blend[1] = RECORDS_CONSTANT_ALPHA;
	blend[2] = RECORDS_FACTOR_SELECT;
	blend[3] = RECORDS_KEY_FULL_RANGE;
	blend[4] = RECORDS_KEY_FULL_RANGE;
	blend[5] = RECORDS_KEY_FULL_RANGE;
	blend[6] = RECORDS_KEY_FULL_RANGE;
	nvrtx_push_method(push, NVRTX_WINDOW_SET_COMPOSITION_CONTROL, blend, 7U);
}

/*
 * Writes a flip: the window's notifier at its offset, then the new surface offset.
 */
void
nvrtx_disp_window_flip(
	struct nvrtx_push *push,
	uint64_t offset,
	uint32_t notifier_offset)
{
	uint32_t notifier[2];

	/* The notifier is written through the sync object at its offset. */
	notifier[0] = NVRTX_DISP_HANDLE_SYNC;
	notifier[1] = notifier_offset & RECORDS_NOTIFIER_OFFSET_MASK;
	nvrtx_push_method(push, NVRTX_WINDOW_SET_CONTEXT_DMA_NOTIFIER, notifier, 2U);

	/* On a frame boundary, to the new surface. */
	nvrtx_push_one(push, NVRTX_WINDOW_SET_PRESENT_CONTROL, RECORDS_PRESENT_NON_TEARING);
	nvrtx_push_one(push, NVRTX_WINDOW_SET_OFFSET, (uint32_t)(offset >> RECORDS_OFFSET_SHIFT));
}

/*
 * Writes a window's update, interlocked with itself and, when asked, with the core.
 */
void
nvrtx_disp_window_update(
	struct nvrtx_push *push,
	int with_core,
	uint32_t window)
{
	uint32_t interlock[2];

	/* No cursor interlock; the window's own bit. */
	interlock[0] = 0U;
	interlock[1] = 1U << window;

	/* Bit 0 holds the window's update until the core's update releases it too. */
	if (with_core != 0)
		interlock[0] = 1U;

	/* Names the interlocks. */
	nvrtx_push_method(push, NVRTX_WINDOW_SET_INTERLOCK_FLAGS, interlock, 2U);

	/* Releases the gathered state. */
	nvrtx_push_one(push, NVRTX_WINDOW_UPDATE, RECORDS_UPDATE);
}

/*
 * Reports the colour of one test band (A8R8G8B8); the bands repeat after eight.
 */
uint32_t
nvrtx_disp_band_color(
	uint32_t band)
{
	/* Succeeded: the band's colour. */
	return records_bands[band % RECORDS_BAND_COUNT];
}

/* Clears a record before its fields are written, so every field not written is zero. */
static void
records_clear(
	uint8_t *bytes,
	size_t length)
{
	size_t at;

	/* Zeroes each byte. */
	for (at = 0U; at < length; at++)
		bytes[at] = 0U;
}
