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
 * A display channel (the core channel and the window channels of the
 * Turing display) reads methods from a push buffer; this header names the
 * methods the driver sends, the objects it writes into the display's
 * instance memory (the hash table and the context DMA objects), and the
 * parameters the resource manager takes to make a channel.  The method
 * offsets and fields are those of the core (0xc57d), window (0xc57e) and
 * cursor (0xc57a) classes.  Everything here builds bytes in memory the
 * caller owns, so it is tested on the host.
 */

#ifndef DRIVERS_GPU_NVRTX_DISP_H
#define DRIVERS_GPU_NVRTX_DISP_H

#include <stddef.h>
#include <stdint.h>

/*
 * Turing's channel classes; a channel's handle is its class in the high half
 * and its instance in the low half.  The driver takes a generation's classes
 * from its profile (nvrtx-private.h); these name Turing's for the records'
 * tests.
 *
 * Values referenced: NVIDIA open-gpu-doc classes/display/clc57d.h
 * (NVC57D_CORE_CHANNEL_DMA) and clc57e.h (NVC57E_WINDOW_CHANNEL_DMA).
 */
#define NVRTX_DISP_CLASS_CORE		0xc57dU
#define NVRTX_DISP_CLASS_WINDOW		0xc57eU

/*
 * Turing's cursor channel (a PIO channel, one for each head: no push buffer,
 * no objects).
 *
 * Value referenced: NVIDIA open-gpu-doc classes/display/clc57a.h
 * (NVC57A_CURSOR_IMM_CHANNEL_PIO).
 */
#define NVRTX_DISP_CLASS_CURSOR		0xc57aU

/*
 * The channel numbers the hash table knows a channel by: the core is 0,
 * window w is 1 + w.
 *
 * Values referenced: NVIDIA open-gpu-kernel-modules 570.144
 * src/common/inc/swref/published/disp/v03_00/dev_disp.h
 * (NV_PDISP_CHN_NUM_CORE, NV_PDISP_CHN_NUM_WIN).
 */
#define NVRTX_DISP_CHID_CORE		0U
#define NVRTX_DISP_CHID_WINDOW_FIRST	1U

/*
 * The handles of the two context DMA objects every channel has: the
 * notifiers and the whole VRAM.
 *
 * Values referenced: Linux v6.19 drivers/gpu/drm/nouveau/dispnv50/handles.h
 * (NV50_DISP_HANDLE_SYNCBUF, NV50_DISP_HANDLE_VRAM).
 */
#define NVRTX_DISP_HANDLE_SYNC		0xf0000000U
#define NVRTX_DISP_HANDLE_VRAM		0xf0000001U

/*
 * The hash table at the start of the instance memory: 1024 entries of 8
 * bytes.
 *
 * Values referenced: Linux v6.19
 * drivers/gpu/drm/nouveau/nvkm/engine/disp/tu102.c (ramht_size) and
 * drivers/gpu/drm/nouveau/nvkm/core/ramht.c (nvkm_ramht_update, the entry's
 * two words).
 */
#define NVRTX_DISP_RAMHT_BYTES		0x2000U
#define NVRTX_DISP_RAMHT_ENTRIES	1024U
#define NVRTX_DISP_RAMHT_BITS		10U
#define NVRTX_DISP_RAMHT_ENTRY_BYTES	8U

/*
 * One context DMA object: 24 bytes on a 16-byte step, after the hash table.
 *
 * Value referenced: Linux v6.19
 * drivers/gpu/drm/nouveau/nvkm/engine/dma/usergv100.c (gv100_dmaobj_bind,
 * the object's size).
 */
#define NVRTX_DISP_CONTEXT_DMA_BYTES	24U
#define NVRTX_DISP_CONTEXT_DMA_STEP	32U

/*
 * The resource manager's parameters: the push buffer's registration and a
 * channel's allocation.
 *
 * Values referenced: NVIDIA open-gpu-kernel-modules 570.144
 * src/common/sdk/nvidia/inc/ctrl/ctrl2080/ctrl2080internal.h
 * (NV2080_CTRL_INTERNAL_DISPLAY_CHANNEL_PUSHBUFFER_PARAMS) and
 * src/common/sdk/nvidia/inc/nvos.h
 * (NV50VAIO_CHANNELDMA_ALLOCATION_PARAMETERS).
 */
#define NVRTX_DISP_PUSHBUFFER_BYTES	56U
#define NVRTX_DISP_CHANNEL_BYTES	40U

/*
 * The resource manager's allocation of a PIO channel (the cursor's): its
 * instance, a notifier handle, a pointer.
 *
 * Value referenced: NVIDIA open-gpu-kernel-modules 570.144
 * src/common/sdk/nvidia/inc/nvos.h
 * (NV50VAIO_CHANNELPIO_ALLOCATION_PARAMETERS).
 */
#define NVRTX_DISP_PIO_CHANNEL_BYTES	16U

/*
 * The resource manager's request to assign an output resource to a display.
 *
 * Values referenced: Linux v6.19
 * drivers/gpu/drm/nouveau/nvkm/subdev/gsp/rm/r535/disp.c (the output resource's
 * acquire).
 */
#define NVRTX_DISP_ASSIGN_SOR_BYTES	80U
#define NVRTX_DISP_SOR_NONE		0xffffffffU

/*
 * The longest run of data one method header carries.
 *
 * Value referenced: NVIDIA open-gpu-doc classes/display/clc57d.h
 * (NVC57D_DMA_METHOD_COUNT).
 */
#define NVRTX_DISP_METHOD_COUNT_MAX	1023U

/*
 * The notifier's status (bits 31:30 of its first word): finished is 2, not
 * begun is 0.
 *
 * Values referenced: NVIDIA open-gpu-doc classes/display/clc57d.h
 * (NV_DISP_NOTIFIER__0_STATUS).
 */
#define NVRTX_DISP_NOTIFIER_STATUS_SHIFT	30U
#define NVRTX_DISP_NOTIFIER_FINISHED		2U

/*
 * The window's storage word for a pitch-linear surface on Turing (its layout
 * field); Ampere's window has none.
 *
 * Value referenced: NVIDIA open-gpu-doc classes/display/clc57e.h
 * (NVC57E_SET_STORAGE_MEMORY_LAYOUT_PITCH).
 */
#define NVRTX_DISP_STORAGE_PITCH_TURING		0x00000010U

/*
 * The window's storage word for a block-linear surface on Turing: the
 * memory layout field (bit 4) block linear (0), the block height in GOBs
 * as its log2 in bits 3:0 (0 for one GOB).
 *
 * Values referenced: NVIDIA open-gpu-doc classes/display/clc57e.h
 * (NVC57E_SET_STORAGE_BLOCK_HEIGHT, NVC57E_SET_STORAGE_MEMORY_LAYOUT_BLOCKLINEAR).
 */
#define NVRTX_DISP_STORAGE_BLOCK_HEIGHT_MASK	0x0000000fU

/*
 * The window's pixel formats: A8R8G8B8 is B, G, R, A in memory (BGRA8888),
 * A8B8G8R8 is R, G, B, A.
 *
 * Values referenced: NVIDIA open-gpu-doc classes/display/clc57e.h
 * (NVC57E_SET_PARAMS_FORMAT_A8R8G8B8, NVC57E_SET_PARAMS_FORMAT_A8B8G8R8).
 */
#define NVRTX_DISP_FORMAT_A8R8G8B8		0xcfU
#define NVRTX_DISP_FORMAT_A8B8G8R8		0xd5U

/*
 * A push buffer's jump: the opcode in bits 31:29, the byte offset in bits
 * 11:2.
 *
 * Values referenced: NVIDIA open-gpu-doc classes/display/clc57d.h
 * (NVC57D_DMA_OPCODE_JUMP, NVC57D_DMA_JUMP_OFFSET).
 */
#define NVRTX_DISP_JUMP				0x20000000U
#define NVRTX_DISP_JUMP_OFFSET_MASK		0x00000ffcU

/*
 * The core channel's methods.
 *
 * Values referenced: NVIDIA open-gpu-doc classes/display/clc57d.h.
 */
#define NVRTX_CORE_UPDATE			0x0200U
#define NVRTX_CORE_SET_CONTEXT_DMA_NOTIFIER	0x0208U
#define NVRTX_CORE_SET_NOTIFIER_CONTROL		0x020cU
#define NVRTX_CORE_SET_INTERLOCK_FLAGS		0x0218U
#define NVRTX_CORE_SOR_SET_CONTROL		0x0300U
#define NVRTX_CORE_SOR_STRIDE			0x0020U
#define NVRTX_CORE_WINDOW_SET_CONTROL		0x1000U
#define NVRTX_CORE_WINDOW_FORMAT_BOUNDS		0x1004U
#define NVRTX_CORE_WINDOW_USAGE_BOUNDS		0x1010U
#define NVRTX_CORE_WINDOW_STRIDE		0x0080U
#define NVRTX_CORE_HEAD_FIRST			0x2000U
#define NVRTX_CORE_HEAD_STRIDE			0x0400U

/*
 * The head methods the takeover sets, as offsets inside one head's block.
 *
 * Values referenced: NVIDIA open-gpu-doc classes/display/clc57d.h.
 */
#define NVRTX_HEAD_PROCAMP			0x000U
#define NVRTX_HEAD_CONTROL_OUTPUT_RESOURCE	0x004U
#define NVRTX_HEAD_CONTROL			0x008U
#define NVRTX_HEAD_PIXEL_CLOCK			0x00cU
#define NVRTX_HEAD_DITHER_CONTROL		0x018U
#define NVRTX_HEAD_DISPLAY_ID			0x020U
#define NVRTX_HEAD_PIXEL_CLOCK_MAX		0x028U
#define NVRTX_HEAD_USAGE_BOUNDS			0x030U
#define NVRTX_HEAD_VIEWPORT_SIZE_IN		0x04cU
#define NVRTX_HEAD_VIEWPORT_SIZE_OUT		0x058U
#define NVRTX_HEAD_RASTER_SIZE			0x064U
#define NVRTX_HEAD_RASTER_SYNC_END		0x068U
#define NVRTX_HEAD_RASTER_BLANK_END		0x06cU
#define NVRTX_HEAD_RASTER_BLANK_START		0x070U
#define NVRTX_HEAD_RASTER_VERT_BLANK2		0x074U
#define NVRTX_HEAD_CONTEXT_DMA_CURSOR		0x088U
#define NVRTX_HEAD_OFFSET_CURSOR		0x090U
#define NVRTX_HEAD_CONTROL_CURSOR		0x09cU
#define NVRTX_HEAD_CONTROL_CURSOR_COMPOSITION	0x0a0U
#define NVRTX_HEAD_CONTEXT_DMA_OLUT		0x288U

/* How many head methods the takeover reads from the armed state, and where each is in the copy. */
#define NVRTX_DISP_HEAD_COPIED			15U
#define NVRTX_DISP_ARMED_VIEWPORT_IN		0U
#define NVRTX_DISP_ARMED_VIEWPORT_OUT		1U
#define NVRTX_DISP_ARMED_RASTER_SIZE		2U
#define NVRTX_DISP_ARMED_SYNC_END		3U
#define NVRTX_DISP_ARMED_BLANK_END		4U
#define NVRTX_DISP_ARMED_BLANK_START		5U
#define NVRTX_DISP_ARMED_VERT_BLANK2		6U
#define NVRTX_DISP_ARMED_CONTROL		7U
#define NVRTX_DISP_ARMED_PIXEL_CLOCK		8U
#define NVRTX_DISP_ARMED_DITHER			11U
#define NVRTX_DISP_ARMED_OUTPUT_RESOURCE	14U

/*
 * An output resource's control: the owner heads in bits 7:0, the protocol in
 * bits 11:8.
 *
 * Value referenced: NVIDIA open-gpu-doc classes/display/clc57d.h
 * (NVC57D_SOR_SET_CONTROL_PROTOCOL).
 */
#define NVRTX_DISP_SOR_PROTOCOL_SHIFT		8U

/*
 * The parameters of a head's mode set (the view, mode, dither, procamp and
 * output-resource methods for one head).
 *
 * active is the visible size (width in bits 15:0, height in 31:16), which
 * is the viewport's output size (its input is the picture's size, which the
 * head's scaler enlarges to it); the raster words are the methods' values;
 * clock_hz the pixel clock; dither and output_resource the methods' values.
 *
 * Procedure referenced: Linux v6.19
 * drivers/gpu/drm/nouveau/dispnv50/head907d.c and headc57d.c (the head's
 * methods).
 */
struct nvrtx_disp_mode {
	uint32_t active;
	uint32_t raster_size;
	uint32_t sync_end;
	uint32_t blank_end;
	uint32_t blank_start;
	uint32_t vert_blank2;
	uint32_t clock_hz;
	uint32_t dither;
	uint32_t output_resource;
};

/*
 * The cursor's image: 64 by 64 pixels of A8R8G8B8 (premultiplied), rows
 * packed, on a 256-byte boundary of the VRAM (the core's cursor size W64_H64).
 *
 * Value referenced: NVIDIA open-gpu-doc classes/display/clc57d.h
 * (NVC57D_HEAD_SET_CONTROL_CURSOR_SIZE_W64_H64).
 */
#define NVRTX_DISP_CURSOR_SIDE			64U
#define NVRTX_DISP_CURSOR_BYTES			(NVRTX_DISP_CURSOR_SIDE * NVRTX_DISP_CURSOR_SIDE * 4U)

/*
 * The cursor channel's registers, from its user area: the free entries, the
 * update and the position.
 *
 * Values referenced: NVIDIA open-gpu-doc classes/display/clc57a.h
 * (NVC57A_FREE, NVC57A_UPDATE, NVC57A_SET_CURSOR_HOT_SPOT_POINT_OUT).
 */
#define NVRTX_CURSOR_FREE			0x008U
#define NVRTX_CURSOR_FREE_COUNT_MASK		0x3fU
#define NVRTX_CURSOR_UPDATE			0x200U
#define NVRTX_CURSOR_POINT_OUT			0x208U

/*
 * The head's frame counter: bits 31:16 of the raster generator's position
 * (RG_DPCA).
 *
 * Values referenced: NVIDIA open-gpu-kernel-modules 570.144
 * src/common/inc/swref/published/disp/v03_00/dev_disp.h
 * (NV_PDISP_RG_DPCA_FRM_CNT).
 */
#define NVRTX_DISP_FRAME_COUNT_SHIFT		16U
#define NVRTX_DISP_FRAME_COUNT_MASK		0xffffU

/*
 * The window channel's methods.
 *
 * Values referenced: NVIDIA open-gpu-doc classes/display/clc57e.h.
 */
#define NVRTX_WINDOW_UPDATE			0x200U
#define NVRTX_WINDOW_SET_CONTEXT_DMA_NOTIFIER	0x21cU
#define NVRTX_WINDOW_SET_NOTIFIER_CONTROL	0x220U
#define NVRTX_WINDOW_SET_SIZE			0x224U
#define NVRTX_WINDOW_SET_STORAGE		0x228U
#define NVRTX_WINDOW_SET_PARAMS			0x22cU
#define NVRTX_WINDOW_SET_PLANAR_STORAGE		0x230U
#define NVRTX_WINDOW_SET_CONTEXT_DMA_ISO	0x240U
#define NVRTX_WINDOW_SET_OFFSET			0x260U
#define NVRTX_WINDOW_SET_POINT_IN		0x290U
#define NVRTX_WINDOW_SET_SIZE_IN		0x298U
#define NVRTX_WINDOW_SET_SIZE_OUT		0x2a4U
#define NVRTX_WINDOW_SET_COMPOSITION_CONTROL	0x2ecU
#define NVRTX_WINDOW_SET_PRESENT_CONTROL	0x308U
#define NVRTX_WINDOW_SET_INTERLOCK_FLAGS	0x370U
#define NVRTX_WINDOW_SET_FMT_COEFFICIENT	0x400U
#define NVRTX_WINDOW_SET_CONTEXT_DMA_ILUT	0x444U

/*
 * A push buffer being written: the memory, how many words fit, how many are written.
 *
 * The words are written in the GPU's (little-endian) order.  A write past
 * the end writes nothing and sets the overflow flag, so a builder never
 * returns an error and the caller checks once before it moves the channel's
 * put.
 */
struct nvrtx_push {
	uint8_t *bytes;
	uint32_t capacity;
	uint32_t count;
	int overflow;
};

/*
 * The step of a surface's pitch the window takes (it is given the pitch in
 * these units).
 *
 * Value referenced: Linux v6.19 drivers/gpu/drm/nouveau/dispnv50/wndwc57e.c
 * (wndwc57e_image_set, the planar storage's pitch).
 */
#define NVRTX_DISP_PITCH_UNIT		64U

/*
 * One surface a window shows: 32-bit pixels in the VRAM.
 *
 * The offset is the surface's VRAM address (a multiple of 256), the pitch
 * the bytes from one row to the next (a multiple of 64), the handle the
 * context DMA object the window reads it through, the format the window's
 * pixel format (NVRTX_DISP_FORMAT_*).  block_linear is nonzero for a
 * block-linear surface (Turing's window only), its rows then rows of GOBs
 * pitch bytes wide and its blocks 2^block_height GOBs tall; zero for
 * linear rows.
 */
struct nvrtx_disp_surface {
	uint64_t offset;
	uint32_t width;
	uint32_t height;
	uint32_t pitch;
	uint32_t handle;
	uint32_t format;
	int block_linear;
	uint32_t block_height;
};

/* The head methods the takeover reads from the armed state, in the order of the NVRTX_DISP_ARMED_ indexes. */
extern const uint32_t nvrtx_disp_head_copied[NVRTX_DISP_HEAD_COPIED];

void nvrtx_push_init(struct nvrtx_push *push, uint8_t *bytes, uint32_t capacity);
void nvrtx_push_method(struct nvrtx_push *push, uint32_t method, const uint32_t *values, uint32_t count);
void nvrtx_push_one(struct nvrtx_push *push, uint32_t method, uint32_t value);
void nvrtx_push_jump(struct nvrtx_push *push, uint32_t offset);

uint32_t nvrtx_disp_ramht_hash(uint32_t chid, uint32_t handle);
void nvrtx_disp_ramht_entry(uint8_t *entry, uint32_t handle, uint32_t chid, uint32_t client, uint32_t object_offset);
void nvrtx_disp_context_dma(uint8_t *object, uint64_t start, uint64_t limit);
void nvrtx_disp_pushbuffer_params(uint8_t *params, uint64_t bus_address, uint32_t size, uint32_t class_number, uint32_t instance);
void nvrtx_disp_channel_params(uint8_t *params, uint32_t instance);
void nvrtx_disp_pio_pushbuffer_params(uint8_t *params, uint32_t class_number, uint32_t instance);
void nvrtx_disp_pio_channel_params(uint8_t *params, uint32_t instance);
void nvrtx_disp_assign_sor_params(uint8_t *params, uint32_t display_id, uint32_t exclude_mask);
uint32_t nvrtx_disp_assign_sor_result(const uint8_t *params, uint32_t display_id);

void nvrtx_disp_core_init(struct nvrtx_push *push, uint32_t window_mask);
void nvrtx_disp_core_sor(struct nvrtx_push *push, uint32_t sor, uint32_t control);
void nvrtx_disp_core_owners(struct nvrtx_push *push, uint32_t window_mask);
void nvrtx_disp_core_head_planes_off(struct nvrtx_push *push, uint32_t head);
void nvrtx_disp_core_display_id(struct nvrtx_push *push, uint32_t head, uint32_t display_id);
void nvrtx_disp_core_head_mode(struct nvrtx_push *push, uint32_t head, const struct nvrtx_disp_mode *mode);
void nvrtx_disp_mode_from_armed(const uint32_t *armed, struct nvrtx_disp_mode *mode);
void nvrtx_disp_core_update(struct nvrtx_push *push, uint32_t notifier_offset, uint32_t window_interlock);
void nvrtx_disp_core_cursor_set(struct nvrtx_push *push, uint32_t head, uint32_t handle, uint64_t offset);
void nvrtx_disp_core_cursor_clear(struct nvrtx_push *push, uint32_t head);
void nvrtx_disp_core_update_cursor(struct nvrtx_push *push, uint32_t notifier_offset);
uint32_t nvrtx_disp_cursor_point(int32_t x, int32_t y);
uint32_t nvrtx_disp_cursor_pixel(uint32_t pixel, int rgba);
uint32_t nvrtx_disp_frame_advance(uint32_t last, uint32_t now);

void nvrtx_disp_window_image(struct nvrtx_push *push, const struct nvrtx_disp_surface *surface);
void nvrtx_disp_window_defaults(struct nvrtx_push *push);
void nvrtx_disp_window_flip(struct nvrtx_push *push, uint64_t offset, uint32_t notifier_offset);
void nvrtx_disp_window_update(struct nvrtx_push *push, int with_core, uint32_t window);

uint32_t nvrtx_disp_band_color(uint32_t band);


/*
 * The display's own resource-manager client and its device and subdevice (the
 * display-client stage): the channels and the display's objects belong to it,
 * and the low 14 bits of its handle are in every hash-table entry of the
 * display's instance memory.
 *
 * Procedure referenced: Linux v6.19
 * drivers/gpu/drm/nouveau/nvkm/subdev/gsp/rm/r535/disp.c (the display's own
 * client).
 */
#define NVRTX_DISP_CLIENT			0xc1d00001U
#define NVRTX_DISP_DEVICE			0xde1d0000U
#define NVRTX_DISP_SUBDEVICE			0x5d1d0000U

/*
 * The window-immediate class, the event class, and the windows a Turing
 * display has.
 *
 * Values referenced: NVIDIA open-gpu-doc classes/display/clc57b.h
 * (NVC57B_WINDOW_IMM_CHANNEL_DMA) and Linux v6.19
 * drivers/gpu/drm/nouveau/nvkm/subdev/gsp/rm/r535/nvrm/event.h
 * (NV01_EVENT_KERNEL_CALLBACK_EX) and
 * drivers/gpu/drm/nouveau/dispnv50/corec57d.c (corec57d_init, the windows
 * it bounds).
 */
#define NVRTX_DISP_CLASS_WIMM			0xc57bU
#define NVRTX_DISP_CLASS_EVENT			0x007eU
#define NVRTX_DISP_WINDOWS			8U

/*
 * The first word of the notifiers' and the VRAM's context DMA objects: the
 * VRAM, read and write, small pages.
 *
 * Value referenced: Linux v6.19
 * drivers/gpu/drm/nouveau/nvkm/engine/dma/usergv100.c (gv100_dmaobj_new,
 * the first word's flags).
 */
#define NVRTX_DISP_CONTEXT_DMA_FLAGS		0x00000045U

/*
 * An output whose resource is a SOR (the resource manager's output type),
 * and the most heads a display has.
 *
 * Values referenced: Linux v6.19
 * drivers/gpu/drm/nouveau/nvkm/subdev/gsp/rm/r535/nvrm/disp.h
 * (NV0073_CTRL_SPECIFIC_OR_TYPE_SOR) and NVIDIA open-gpu-kernel-modules
 * 570.144 src/common/inc/swref/published/disp/v03_00/dev_disp.h
 * (NV_PDISP_FE_HW_SYS_CAP_HEAD_EXISTS__SIZE_1).
 */
#define NVRTX_DISP_OUTPUT_SOR			2U
#define NVRTX_DISP_HEAD_MAX			8U

/*
 * The context DMA object a window reads the shown surface through (all the
 * VRAM, large pages): its handle, and the first word of the object.
 *
 * Value referenced: Linux v6.19
 * drivers/gpu/drm/nouveau/nvkm/engine/dma/usergv100.c (gv100_dmaobj_new,
 * the first word's flags).
 */
#define NVRTX_DISP_HANDLE_FB			0xfb000000U
#define NVRTX_DISP_CONTEXT_DMA_FB_FLAGS		0x00000005U

/*
 * A LUT's bytes: a 32-byte header and 1025 entries of 8 bytes.
 *
 * Value referenced: Linux v6.19 drivers/gpu/drm/nouveau/dispnv50/headc57d.c
 * (headc57d_olut_load) and lut.c (nv50_lut_init).
 */
#define NVRTX_DISP_LUT_BYTES			0x2008U

/*
 * The event objects' parameters, the client-side flag of their
 * notification, and a repeated notification's parameters.
 *
 * Values referenced: Linux v6.19
 * drivers/gpu/drm/nouveau/nvkm/subdev/gsp/rm/r535/nvrm/event.h
 * (NV0005_ALLOC_PARAMETERS, NV01_EVENT_CLIENT_RM,
 * NV2080_CTRL_EVENT_SET_NOTIFICATION_PARAMS,
 * NV2080_CTRL_EVENT_SET_NOTIFICATION_ACTION_REPEAT).
 */
#define NVRTX_DISP_EVENT_BYTES			24U
#define NVRTX_DISP_EVENT_CLIENT			0x04000000U
#define NVRTX_DISP_NOTIFICATION_BYTES		20U
#define NVRTX_DISP_NOTIFY_REPEAT		2U

/*
 * One mode's timing as a display mode gives it: the visible size, the sync
 * start and end and the total of each direction, the clock in kHz, and
 * whether each sync is negative.
 */
struct nvrtx_disp_timing {
	uint32_t hdisplay;
	uint32_t hsync_start;
	uint32_t hsync_end;
	uint32_t htotal;
	uint32_t vdisplay;
	uint32_t vsync_start;
	uint32_t vsync_end;
	uint32_t vtotal;
	uint32_t clock_khz;
	int nhsync;
	int nvsync;
};

/* What the EDID says of the sink: an HDMI sink, with audio, and its capabilities for the resource manager. */
struct nvrtx_disp_sink {
	int hdmi;
	int audio;
	uint32_t caps;
};

/*
 * The first commit on the head the firmware lit: the head, the SOR and
 * display id that drive it, the protocol the SOR is given, the mode, and
 * the viewport's input size (width in bits 15:0, height in 31:16): the size
 * of the picture the window shows, which the head's scaler enlarges to the
 * mode's visible size when it is smaller, as the firmware does with a
 * console smaller than the monitor's mode.
 */
struct nvrtx_disp_first {
	uint32_t head;
	uint32_t sor;
	uint32_t display_id;
	uint32_t protocol;
	struct nvrtx_disp_mode mode;
	uint32_t view_in;
};

void nvrtx_disp_first_core_start(struct nvrtx_push *push);
void nvrtx_disp_first_core_mode(struct nvrtx_push *push, const struct nvrtx_disp_first *first);
void nvrtx_disp_first_core_release(struct nvrtx_push *push, uint32_t window_interlock);
void nvrtx_disp_first_core_view(struct nvrtx_push *push, uint32_t head, uint32_t view_in, uint32_t view_out);
void nvrtx_disp_first_core_olut(struct nvrtx_push *push, uint32_t head, uint64_t offset);
void nvrtx_disp_first_window(struct nvrtx_push *push, uint32_t window, const struct nvrtx_disp_surface *surface, uint32_t storage, uint32_t notifier_offset, uint64_t ilut_offset);
void nvrtx_disp_first_window_flip(struct nvrtx_push *push, uint32_t window, const struct nvrtx_disp_surface *surface, uint32_t storage, uint32_t notifier_offset, int with_core);
void nvrtx_disp_first_mode(const struct nvrtx_disp_timing *timing, struct nvrtx_disp_mode *mode);
void nvrtx_disp_first_timing_from_armed(const uint32_t *armed, struct nvrtx_disp_timing *timing);
int nvrtx_disp_first_edid_timing(const uint8_t *edid, size_t length, struct nvrtx_disp_timing *timing);
void nvrtx_disp_first_edid_sink(const uint8_t *edid, size_t length, struct nvrtx_disp_sink *sink);
void nvrtx_disp_first_avi(uint32_t *words);
void nvrtx_disp_first_olut_fill(uint8_t *lut);
void nvrtx_disp_first_ilut_fill(uint8_t *lut);
void nvrtx_disp_first_event_params(uint8_t *params, uint32_t client, uint32_t notify);
void nvrtx_disp_first_notification_params(uint8_t *params, uint32_t notify);
void nvrtx_disp_first_display_params(uint8_t *params, size_t length, uint32_t display_id);
void nvrtx_disp_context_dma_flags(uint8_t *object, uint32_t flags, uint64_t start, uint64_t limit);

#endif
