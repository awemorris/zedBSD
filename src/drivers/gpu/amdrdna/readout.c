/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Stage A1: reading what the firmware put on the display.
 *
 * The firmware's graphics driver (GOP) lit one timing generator and fed it
 * through an output processor, a blend unit and a plane from a surface in
 * VRAM, and drives a connector through one encoder.  The stage follows
 * that chain backwards from the lit timing generator, reads the plane's
 * surface, the scaler that draws it into the active area and the display
 * clocks, and checks that the surface is the picture the loader handed
 * the kernel as its console.  The loader's mode need not be the monitor's:
 * the firmware may fetch a smaller surface and stretch it, so the picture
 * is compared with the part of the surface the plane fetches, not with the
 * timing.  The stage then plans the VRAM layout around that picture.
 * Nothing on the display is written.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <kern/platform.h>
#include <uapi/amdrdna.h>
#include <uapi/errno.h>

#include "drivers/gpu/amdrdna/amdrdna-private.h"
#include "drivers/gpu/amdrdna/amdrdna-regs.h"

/* The name prefix of the boot handoff values that describe the firmware's framebuffer. */
#define READOUT_FRAMEBUFFER_HANDOFF	"pcat.framebuffer"

/* The bytes of one pixel of the firmware's 32-bit picture. */
#define READOUT_PIXEL_BYTES		4U

/*
 * The fields of the display registers the readout decodes: a head's VGA
 * mode bit; a timing generator's enable bit, its 15-bit totals and the
 * start (bits 14:0) and end (bits 30:16) of its blanking; the combiner's
 * two-segment bit and its first and second sources; the blend tree root's
 * blend unit and the blend unit's top plane, bottom unit and output
 * processor (0xf for none); the plane's blank bit, timing-generator select
 * and underflow bits, its pixel format, its tiling mode, its pitch word,
 * its viewport's start and size (the same two 14-bit fields), the high 16
 * bits of its address and its compression bit; the update lock; an
 * encoder's back-end enable, the one-hot front end it takes, its mode and
 * hot-plug select, and the front end's timing generator; and the scaler's
 * mode, its output rectangle's start (13-bit fields) and size (14-bit
 * fields, the second at bit 16), its 26-bit ratio words and its two-bit
 * automatic mode.
 *
 * Values referenced: Linux v6.19
 * drivers/gpu/drm/amd/include/asic_reg/dcn/dcn_2_0_0_sh_mask.h
 * (D1VGA_CONTROL__D1VGA_MODE_ENABLE, OTG0_OTG_CONTROL__OTG_MASTER_EN,
 * OTG0_OTG_H_TOTAL__OTG_H_TOTAL, OTG0_OTG_H_BLANK_START_END__OTG_H_BLANK_START,
 * OTG0_OTG_H_BLANK_START_END__OTG_H_BLANK_END,
 * ODM0_OPTC_DATA_SOURCE_SELECT__OPTC_NUM_OF_INPUT_SEGMENT,
 * ODM0_OPTC_DATA_SOURCE_SELECT__OPTC_SEG0_SRC_SEL,
 * ODM0_OPTC_DATA_SOURCE_SELECT__OPTC_SEG1_SRC_SEL, MPC_OUT0_MUX__MPC_OUT_MUX,
 * MPCC0_MPCC_TOP_SEL__MPCC_TOP_SEL, MPCC0_MPCC_BOT_SEL__MPCC_BOT_SEL,
 * MPCC0_MPCC_OPP_ID__MPCC_OPP_ID, HUBP0_DCHUBP_CNTL__HUBP_BLANK_EN,
 * HUBP0_DCHUBP_CNTL__HUBP_VTG_SEL, HUBP0_DCHUBP_CNTL__HUBP_UNDERFLOW_STATUS,
 * HUBP0_DCSURF_SURFACE_CONFIG__SURFACE_PIXEL_FORMAT,
 * HUBP0_DCSURF_TILING_CONFIG__SW_MODE, HUBPREQ0_DCSURF_SURFACE_PITCH__PITCH,
 * HUBP0_DCSURF_PRI_VIEWPORT_DIMENSION__PRI_VIEWPORT_WIDTH,
 * HUBP0_DCSURF_PRI_VIEWPORT_DIMENSION__PRI_VIEWPORT_HEIGHT,
 * HUBPREQ0_DCSURF_PRIMARY_SURFACE_ADDRESS_HIGH__PRIMARY_SURFACE_ADDRESS_HIGH,
 * HUBPREQ0_DCSURF_SURFACE_CONTROL__PRIMARY_SURFACE_DCC_EN,
 * OTG0_OTG_MASTER_UPDATE_LOCK__OTG_MASTER_UPDATE_LOCK,
 * DIG0_DIG_BE_EN_CNTL__DIG_ENABLE, DIG0_DIG_BE_CNTL__DIG_FE_SOURCE_SELECT,
 * DIG0_DIG_BE_CNTL__DIG_MODE, DIG0_DIG_BE_CNTL__DIG_HPD_SELECT,
 * DIG0_DIG_FE_CNTL__DIG_SOURCE_SELECT,
 * HUBP0_DCSURF_PRI_VIEWPORT_START__PRI_VIEWPORT_X_START,
 * HUBP0_DCSURF_PRI_VIEWPORT_START__PRI_VIEWPORT_Y_START,
 * DSCL0_SCL_MODE__DSCL_MODE, DSCL0_RECOUT_START__RECOUT_START_X,
 * DSCL0_RECOUT_START__RECOUT_START_Y, DSCL0_RECOUT_SIZE__RECOUT_WIDTH,
 * DSCL0_RECOUT_SIZE__RECOUT_HEIGHT,
 * DSCL0_SCL_HORZ_FILTER_SCALE_RATIO__SCL_H_SCALE_RATIO,
 * DSCL0_SCL_VERT_FILTER_SCALE_RATIO__SCL_V_SCALE_RATIO,
 * DSCL0_DSCL_AUTOCAL__AUTOCAL_MODE).
 */
#define READOUT_HEAD_LEGACY		0x00000001U
#define READOUT_TIMING_ENABLED		0x00000001U
#define READOUT_TIMING_FIELD		0x00007fffU
#define READOUT_TIMING_END_SHIFT	16U
#define READOUT_COMBINER_TWO		0x00000001U
#define READOUT_COMBINER_FIRST_SHIFT	8U
#define READOUT_COMBINER_SECOND_SHIFT	12U
#define READOUT_UNIT_FIELD		0x0000000fU
#define READOUT_UNIT_NONE		0x0000000fU
#define READOUT_PLANE_BLANK		0x00000001U
#define READOUT_PLANE_TIMING_SHIFT	4U
#define READOUT_PLANE_UNDERFLOW_SHIFT	28U
#define READOUT_PLANE_UNDERFLOW_FIELD	0x00000007U
#define READOUT_FORMAT_FIELD		0x0000007fU
#define READOUT_TILING_FIELD		0x0000001fU
#define READOUT_PITCH_FIELD		0x00003fffU
#define READOUT_VIEWPORT_FIELD		0x00003fffU
#define READOUT_VIEWPORT_HEIGHT_SHIFT	16U
#define READOUT_ADDRESS_HIGH_FIELD	0x0000ffffU
#define READOUT_SURFACE_COMPRESSED	0x00000002U
#define READOUT_UPDATE_LOCKED		0x00000001U
#define READOUT_ENCODER_ENABLED		0x00000001U
#define READOUT_ENCODER_FRONT_SHIFT	8U
#define READOUT_ENCODER_FRONT_FIELD	0x0000007fU
#define READOUT_ENCODER_MODE_SHIFT	16U
#define READOUT_ENCODER_MODE_FIELD	0x00000007U
#define READOUT_ENCODER_HOTPLUG_SHIFT	28U
#define READOUT_ENCODER_HOTPLUG_FIELD	0x00000007U
#define READOUT_FRONT_TIMING_FIELD	0x00000007U
#define READOUT_SCALER_MODE_FIELD	0x00000007U
#define READOUT_SCALER_START_FIELD	0x00001fffU
#define READOUT_SCALER_SIZE_FIELD	0x00003fffU
#define READOUT_SCALER_SECOND_SHIFT	16U
#define READOUT_SCALER_RATIO_FIELD	0x03ffffffU
#define READOUT_SCALER_AUTOMATIC_FIELD	0x00000003U

/* The number of front ends an encoder's one-hot select may name. */
#define READOUT_FRONT_ENDS		7U

static uint32_t display_read(struct amdrdna_device *device, enum amdrdna_dcn_register which, unsigned instance);
static int readout_heads(struct amdrdna_device *device, unsigned *lit_count);
static int readout_chain(struct amdrdna_device *device);
static void readout_plane(struct amdrdna_device *device);
static void readout_output(struct amdrdna_device *device);
static void readout_clocks(struct amdrdna_device *device);
static void readout_scaler(struct amdrdna_device *device);
static int readout_loader(struct amdrdna_device *device);
static int readout_picture(struct amdrdna_device *device);
static const char *output_kind_name(unsigned kind);

/*
 * Runs stage A1: reads the firmware's display chain and plans the VRAM
 * layout.
 *
 * Returns 0 when the chain was read (or nothing is lit) and the picture
 * the loader handed over is this surface, ENOTSUP for a head left in VGA
 * mode, a chain the driver cannot follow or a viewport moved into the
 * surface, or EINVAL when the loader's picture and the fetched surface
 * differ in place or in size.
 */
int
amdrdna_a1_readout(
	struct amdrdna_device *device)
{
	unsigned lit_count;
	int error;

	/* Says the stage starts. */
	amdrdna_mark("A1 begin");

	/* Refuses VGA mode, and finds the lit timing generators. */
	error = readout_heads(device, &lit_count);
	if (error != 0)
		return error;

	/* Follows the chain of the first lit one, when there is one. */
	if (device->gop.lit) {
		error = readout_chain(device);
		if (error != 0)
			return error;

		/* Reads the plane, the scaler, the encoder and the clocks of that chain. */
		readout_plane(device);
		readout_scaler(device);
		readout_output(device);
		readout_clocks(device);
	}

	/* Compares the loader's picture with the plane's surface: the same VRAM. */
	error = readout_loader(device);
	if (error != 0)
		return error;

	/* Compares their sizes, and finds how the surface reaches the screen. */
	error = readout_picture(device);
	if (error != 0)
		return error;

	/* Plans the VRAM around the picture and the fixed ranges. */
	error = amdrdna_plan_layout(device);
	if (error != 0) {
		amdrdna_mark("A1 fail: VRAM layout not planned (%d)", error);
		return error;
	}

	/* Marks a GPU nothing is lit on. */
	if (!device->gop.lit) {
		amdrdna_mark("A1 ok nothing lit; cpu=0-%lluMiB gpu=%llu-%lluMiB kept=%u",
		    (unsigned long long)(device->layout.cpu_pool_end >> 20),
		    (unsigned long long)(device->layout.gpu_pool_start >> 20),
		    (unsigned long long)(device->layout.gpu_pool_end >> 20),
		    device->layout.reserved_count);
		return 0;
	}

	/* Marks the chain, the picture and the layout. */
	amdrdna_mark("A1 ok otg=%u opp=%u mpcc=%u hubp=%u dig=%d mode=%s %ux%u on %ux%u %s fb=+0x%llx pitch=0x%x stride=%u fmt=%u lit=%u",
	    device->gop.timing,
	    device->gop.output_processor,
	    device->gop.blend,
	    device->gop.plane,
	    device->gop.has_output ? (int)device->gop.output : -1,
	    output_kind_name(device->gop.output_kind),
	    (unsigned)device->gop.viewport_width,
	    (unsigned)device->gop.viewport_height,
	    (unsigned)device->gop.width,
	    (unsigned)device->gop.height,
	    amdrdna_picture_shape_name(device->gop.picture.shape),
	    (unsigned long long)device->gop.surface_offset,
	    (unsigned)device->gop.pitch_word,
	    (unsigned)device->gop.loader_stride,
	    (unsigned)device->gop.format,
	    lit_count);
	amdrdna_mark("A1 clk dispclk_cntl=0x%08x pixel_rate=0x%08x dto=0x%08x/0x%08x underflow=%u lock=%u",
	    (unsigned)device->gop.display_clock_divider,
	    (unsigned)device->gop.pixel_rate_control,
	    (unsigned)device->gop.dp_clock_phase,
	    (unsigned)device->gop.dp_clock_modulo,
	    (unsigned)device->gop.underflow,
	    device->gop.locked ? 1U : 0U);
	amdrdna_mark("A1 vram cpu=0-%lluMiB gpu=%llu-%lluMiB kept=%u",
	    (unsigned long long)(device->layout.cpu_pool_end >> 20),
	    (unsigned long long)(device->layout.gpu_pool_start >> 20),
	    (unsigned long long)(device->layout.gpu_pool_end >> 20),
	    device->layout.reserved_count);

	/* Succeeded: the chain and the picture are recorded. */
	return 0;
}

/*
 * Plans the VRAM layout from the VRAM size, the CPU window, the firmware's
 * picture and the ranges the video BIOS keeps, and marks the kept ranges.
 */
int
amdrdna_plan_layout(
	struct amdrdna_device *device)
{
	struct amdrdna_layout_input input;
	const struct amdrdna_range *range;
	uint32_t pixels;
	uint32_t lines;
	unsigned index;
	int error;

	/* The VRAM and the part of it the CPU window shows, at most 256 MiB. */
	input.vram_bytes = device->vram_bytes;
	input.window_bytes = device->vram_window_bytes;
	if (input.window_bytes > AMDRDNA_CPU_WINDOW_BYTES)
		input.window_bytes = AMDRDNA_CPU_WINDOW_BYTES;

	/*
	 * The firmware's picture, by the wider of the plane's pitch (counted
	 * as the pitch or the pitch less one, which the firmware and the
	 * hardware read differently) and the loader's stride, and by the
	 * tallest of the active area, the viewport and the loader's picture
	 * (a scaled picture is shorter than the screen; the larger range keeps
	 * whichever the firmware really uses).
	 */
	input.gop_present = false;
	input.gop_offset = 0U;
	input.gop_bytes = 0U;
	pixels = device->gop.pitch_word + 1U;
	if (device->gop.loader_stride > pixels)
		pixels = device->gop.loader_stride;
	lines = device->gop.height;
	if (device->gop.viewport_height > lines)
		lines = device->gop.viewport_height;
	if (device->gop.loader_height > lines)
		lines = device->gop.loader_height;
	if (device->gop.lit && device->gop.surface_in_vram) {
		input.gop_present = true;
		input.gop_offset = device->gop.surface_offset;
		input.gop_bytes = (uint64_t)pixels * lines * READOUT_PIXEL_BYTES;
	}

	/* The ranges the video BIOS keeps, once it has been read. */
	input.firmware_start = (uint64_t)device->vbios_info.firmware_start_kib << 10;
	input.firmware_bytes = (uint64_t)device->vbios_info.firmware_kib << 10;
	input.driver_start = (uint64_t)device->vbios_info.driver_start_kib << 10;
	input.driver_bytes = (uint64_t)device->vbios_info.driver_kib << 10;

	/* Plans the layout. */
	error = amdrdna_layout_plan(&input, &device->layout);
	if (error != 0)
		return error;

	/* Marks every kept range. */
	for (index = 0U; index < device->layout.reserved_count; index++) {
		range = &device->layout.reserved[index];
		amdrdna_mark("vram kept %s 0x%llx-0x%llx",
		    amdrdna_range_reason_name(range->reason),
		    (unsigned long long)range->start,
		    (unsigned long long)range->end);
	}

	/* Succeeded: the layout is planned. */
	return 0;
}

/* Reads one instance of a named display register; a register the display block lacks reads as zero. */
static uint32_t
display_read(
	struct amdrdna_device *device,
	enum amdrdna_dcn_register which,
	unsigned instance)
{
	uint32_t index;
	uint32_t value;

	/* Finds the register's index in the display block's windows. */
	index = amdrdna_dcn_index(&device->blocks.blocks[AMDRDNA_BLOCK_DCN], which, instance);
	if (index == 0U)
		return 0U;

	/* Reads it. */
	value = amdrdna_read(device, index);

	/* Succeeded: the register's value. */
	return value;
}

/* Refuses a head left in VGA mode and finds the lit timing generators. */
static int
readout_heads(
	struct amdrdna_device *device,
	unsigned *lit_count)
{
	uint32_t word;
	unsigned head;
	unsigned timing;

	/* A head in VGA mode shows text the plane does not feed; the driver cannot keep it. */
	for (head = 0U; head < AMDRDNA_DISPLAY_PIPES; head++) {
		/* Stops at the first head in VGA mode. */
		word = display_read(device, AMDRDNA_DCN_HEAD_LEGACY_MODE, head);
		if ((word & READOUT_HEAD_LEGACY) != 0U) {
			amdrdna_mark("A1 fail: head %u is in VGA mode (0x%08x)", head, (unsigned)word);
			return ENOTSUP;
		}
	}

	/* Finds the lit timing generators; the first one is the firmware's console. */
	*lit_count = 0U;
	device->gop.lit = false;
	for (timing = 0U; timing < AMDRDNA_DISPLAY_PIPES; timing++) {
		/* Skips a timing generator that is off. */
		word = display_read(device, AMDRDNA_DCN_TIMING_CONTROL, timing);
		if ((word & READOUT_TIMING_ENABLED) == 0U)
			continue;

		/* Counts it, and keeps the first. */
		(*lit_count)++;
		if (!device->gop.lit) {
			device->gop.lit = true;
			device->gop.timing = timing;
		}
	}

	/* Succeeded: the lit timing generator, if any, is recorded. */
	return 0;
}

/*
 * Reads the lit timing generator's timing and follows its chain back to
 * the plane.
 *
 * Procedure referenced: Linux v6.19
 * drivers/gpu/drm/amd/display/dc/optc/dcn10/dcn10_optc.c
 * (optc1_get_otg_active_size), dc/optc/dcn20/dcn20_optc.c
 * (optc2_get_optc_source) and dc/mpc/dcn10/dcn10_mpc.c
 * (mpc1_get_mpc_out_mux, mpc1_init_mpcc_list_from_hw).
 */
static int
readout_chain(
	struct amdrdna_device *device)
{
	struct amdrdna_gop *gop;
	uint32_t word;
	uint32_t start;
	uint32_t end;
	uint32_t second;

	/* The timing: the totals, and the active size between the blanking's end and start. */
	gop = &device->gop;
	gop->h_total = (display_read(device, AMDRDNA_DCN_TIMING_H_TOTAL, gop->timing) & READOUT_TIMING_FIELD) + 1U;
	gop->v_total = (display_read(device, AMDRDNA_DCN_TIMING_V_TOTAL, gop->timing) & READOUT_TIMING_FIELD) + 1U;

	/* The active width lies between the end and the start of the horizontal blanking. */
	word = display_read(device, AMDRDNA_DCN_TIMING_H_BLANK, gop->timing);
	start = word & READOUT_TIMING_FIELD;
	end = (word >> READOUT_TIMING_END_SHIFT) & READOUT_TIMING_FIELD;
	gop->width = start - end;

	/* The active height lies between the end and the start of the vertical blanking. */
	word = display_read(device, AMDRDNA_DCN_TIMING_V_BLANK, gop->timing);
	start = word & READOUT_TIMING_FIELD;
	end = (word >> READOUT_TIMING_END_SHIFT) & READOUT_TIMING_FIELD;
	gop->height = start - end;

	/* The update lock; a held lock keeps a new surface address from taking effect. */
	word = display_read(device, AMDRDNA_DCN_TIMING_UPDATE_LOCK, gop->timing);
	gop->locked = false;
	if ((word & READOUT_UPDATE_LOCKED) != 0U)
		gop->locked = true;

	/*
	 * The combiner names the output processor; two segments (a second
	 * source other than 0xf) mean two processors share the picture.
	 */
	word = display_read(device, AMDRDNA_DCN_COMBINER_SOURCE, gop->timing);
	gop->output_processor = (word >> READOUT_COMBINER_FIRST_SHIFT) & READOUT_UNIT_FIELD;
	second = (word >> READOUT_COMBINER_SECOND_SHIFT) & READOUT_UNIT_FIELD;
	gop->combined = false;
	if ((word & READOUT_COMBINER_TWO) != 0U && second != READOUT_UNIT_NONE)
		gop->combined = true;

	/* A picture split over two processors is a mode the driver does not keep. */
	if (gop->combined) {
		amdrdna_mark("A1 fail: otg %u combines two output processors", gop->timing);
		return ENOTSUP;
	}

	/* An output processor past the pipes is a chain the driver cannot follow. */
	if (gop->output_processor >= AMDRDNA_DISPLAY_PIPES) {
		amdrdna_mark("A1 fail: otg %u has no output processor (0x%08x)", gop->timing, (unsigned)word);
		return ENOTSUP;
	}

	/* The output processor's blend tree root names the top blend unit. */
	word = display_read(device, AMDRDNA_DCN_BLEND_TREE_ROOT, gop->output_processor);
	gop->blend = word & READOUT_UNIT_FIELD;
	if (gop->blend >= AMDRDNA_DISPLAY_PIPES) {
		amdrdna_mark("A1 fail: opp %u feeds no blend unit (0x%08x)", gop->output_processor, (unsigned)word);
		return ENOTSUP;
	}

	/* The blend unit's top input names the plane. */
	word = display_read(device, AMDRDNA_DCN_BLEND_TOP, gop->blend);
	gop->plane = word & READOUT_UNIT_FIELD;
	if (gop->plane >= AMDRDNA_DISPLAY_PIPES) {
		amdrdna_mark("A1 fail: mpcc %u has no plane (0x%08x)", gop->blend, (unsigned)word);
		return ENOTSUP;
	}

	/* A blend unit below the top one means more than one plane, which is only noted. */
	word = display_read(device, AMDRDNA_DCN_BLEND_BOTTOM, gop->blend) & READOUT_UNIT_FIELD;
	if (word != READOUT_UNIT_NONE && word != gop->blend)
		amdrdna_mark("A1 note: mpcc %u has mpcc %u below it", gop->blend, (unsigned)word);

	/* Succeeded: the chain from the timing generator to the plane is known. */
	return 0;
}

/*
 * Reads the plane's control word and its surface.
 *
 * Procedure referenced: Linux v6.19
 * drivers/gpu/drm/amd/display/dc/hubp/dcn20/dcn20_hubp.c
 * (hubp2_read_state_common).
 */
static void
readout_plane(
	struct amdrdna_device *device)
{
	struct amdrdna_gop *gop;
	uint32_t word;
	uint32_t high;
	uint32_t low;
	unsigned selected;

	/* The control word: the timing generator the plane follows, and its underflow bits. */
	gop = &device->gop;
	gop->plane_control = display_read(device, AMDRDNA_DCN_PLANE_CONTROL, gop->plane);
	selected = (gop->plane_control >> READOUT_PLANE_TIMING_SHIFT) & READOUT_UNIT_FIELD;
	gop->underflow = (gop->plane_control >> READOUT_PLANE_UNDERFLOW_SHIFT) & READOUT_PLANE_UNDERFLOW_FIELD;
	if (selected != gop->timing)
		amdrdna_mark("A1 note: hubp %u follows otg %u, not otg %u", gop->plane, selected, gop->timing);

	/* The pixel format, the tiling mode and the pitch word. */
	gop->format = display_read(device, AMDRDNA_DCN_PLANE_FORMAT, gop->plane) & READOUT_FORMAT_FIELD;
	gop->tiling = display_read(device, AMDRDNA_DCN_PLANE_TILING, gop->plane) & READOUT_TILING_FIELD;
	gop->pitch_word = display_read(device, AMDRDNA_DCN_PLANE_PITCH, gop->plane) & READOUT_PITCH_FIELD;

	/* The viewport's start in the surface. */
	word = display_read(device, AMDRDNA_DCN_PLANE_VIEWPORT_START, gop->plane);
	gop->viewport_x = word & READOUT_VIEWPORT_FIELD;
	gop->viewport_y = (word >> READOUT_VIEWPORT_HEIGHT_SHIFT) & READOUT_VIEWPORT_FIELD;

	/* The viewport's size: the part of the surface the plane fetches. */
	word = display_read(device, AMDRDNA_DCN_PLANE_VIEWPORT_SIZE, gop->plane);
	gop->viewport_width = word & READOUT_VIEWPORT_FIELD;
	gop->viewport_height = (word >> READOUT_VIEWPORT_HEIGHT_SHIFT) & READOUT_VIEWPORT_FIELD;

	/* Whether the surface is compressed. */
	word = display_read(device, AMDRDNA_DCN_PLANE_SURFACE_CONTROL, gop->plane);
	gop->compressed = false;
	if ((word & READOUT_SURFACE_COMPRESSED) != 0U)
		gop->compressed = true;

	/* The surface's address in the GPU's address space, 48 bits. */
	high = display_read(device, AMDRDNA_DCN_PLANE_ADDRESS_HIGH, gop->plane) & READOUT_ADDRESS_HIGH_FIELD;
	low = display_read(device, AMDRDNA_DCN_PLANE_ADDRESS_LOW, gop->plane);
	gop->surface_mc = ((uint64_t)high << 32) | low;

	/* The surface's offset in VRAM, when it lies in the frame buffer. */
	gop->surface_in_vram = false;
	gop->surface_offset = 0U;
	if (gop->surface_mc >= device->frame_buffer_base && gop->surface_mc <= device->frame_buffer_top) {
		gop->surface_in_vram = true;
		gop->surface_offset = gop->surface_mc - device->frame_buffer_base;
	}
}

/*
 * Finds the encoder whose front end takes the lit timing generator, and
 * its kind.
 *
 * Procedure referenced: Linux v6.19
 * drivers/gpu/drm/amd/display/dc/dio/dcn10/dcn10_link_encoder.c
 * (dcn10_is_dig_enabled, dcn10_get_dig_frontend, dcn10_get_dig_mode) and
 * dc/dio/dcn10/dcn10_stream_encoder.c (enc1_dig_source_otg).
 */
static void
readout_output(
	struct amdrdna_device *device)
{
	struct amdrdna_gop *gop;
	uint32_t enable;
	uint32_t back;
	uint32_t front_bits;
	uint32_t front;
	unsigned output;
	unsigned bit;
	unsigned source;

	/* Looks at every encoder's back end. */
	gop = &device->gop;
	gop->has_output = false;
	for (output = 0U; output < AMDRDNA_DISPLAY_PIPES; output++) {
		/* Skips a back end that is off. */
		enable = display_read(device, AMDRDNA_DCN_ENCODER_BACK_ENABLE, output);
		if ((enable & READOUT_ENCODER_ENABLED) == 0U)
			continue;

		/* The back end takes one front end, named by one bit. */
		back = display_read(device, AMDRDNA_DCN_ENCODER_BACK_CONTROL, output);
		front_bits = (back >> READOUT_ENCODER_FRONT_SHIFT) & READOUT_ENCODER_FRONT_FIELD;
		front = READOUT_FRONT_ENDS;
		for (bit = 0U; bit < READOUT_FRONT_ENDS; bit++) {
			/* Stops at the set bit. */
			if ((front_bits & (1U << bit)) != 0U) {
				front = bit;
				break;
			}
		}

		/* Skips a back end without a front end, or with one past the pipes. */
		if (front >= AMDRDNA_DISPLAY_PIPES)
			continue;

		/* The front end must take the lit timing generator. */
		source = display_read(device, AMDRDNA_DCN_ENCODER_FRONT_CONTROL, front) & READOUT_FRONT_TIMING_FIELD;
		if (source != gop->timing)
			continue;

		/* Records the encoder, its front end, its kind and its hot-plug line. */
		gop->has_output = true;
		gop->output = output;
		gop->output_front = front;
		gop->output_kind = (back >> READOUT_ENCODER_MODE_SHIFT) & READOUT_ENCODER_MODE_FIELD;
		gop->output_hotplug = (back >> READOUT_ENCODER_HOTPLUG_SHIFT) & READOUT_ENCODER_HOTPLUG_FIELD;
		break;
	}

	/* An encoder-less picture is only noted; the plane is still the console. */
	if (!gop->has_output)
		amdrdna_mark("A1 note: no encoder takes otg %u", gop->timing);
}

/* Reads the display clock controls of the lit timing generator, as raw words for the log. */
static void
readout_clocks(
	struct amdrdna_device *device)
{
	struct amdrdna_gop *gop;

	/* The display clock divider and the timing generator's pixel rate and DisplayPort clock. */
	gop = &device->gop;
	gop->display_clock_divider = display_read(device, AMDRDNA_DCN_DISPLAY_CLOCK_DIVIDER, 0U);
	gop->pixel_rate_control = display_read(device, AMDRDNA_DCN_PIXEL_RATE_CONTROL, gop->timing);
	gop->dp_clock_phase = display_read(device, AMDRDNA_DCN_DP_CLOCK_PHASE, gop->timing);
	gop->dp_clock_modulo = display_read(device, AMDRDNA_DCN_DP_CLOCK_MODULO, gop->timing);
}

/*
 * Reads the scaler of the plane's pixel processor: its mode, the rectangle
 * of the active area it draws the viewport into, and its two ratios.
 *
 * Procedure referenced: Linux v6.19
 * drivers/gpu/drm/amd/display/dc/dpp/dcn10/dcn10_dpp_dscl.c
 * (dpp1_dscl_set_scaler_manual_scale, dpp1_dscl_set_recout,
 * dpp1_dscl_set_manual_ratio_init, and the automatic modes the file
 * names); the pixel processor of a pipe is the one of the same number as
 * its plane.
 */
static void
readout_scaler(
	struct amdrdna_device *device)
{
	struct amdrdna_gop *gop;
	uint32_t word;

	/* The mode: 0 passes the pixels through, the others scale. */
	gop = &device->gop;
	gop->scaler_mode = display_read(device, AMDRDNA_DCN_SCALER_MODE, gop->plane) & READOUT_SCALER_MODE_FIELD;

	/* The rectangle's start in the active area. */
	word = display_read(device, AMDRDNA_DCN_SCALER_OUTPUT_START, gop->plane);
	gop->scaler_x = word & READOUT_SCALER_START_FIELD;
	gop->scaler_y = (word >> READOUT_SCALER_SECOND_SHIFT) & READOUT_SCALER_START_FIELD;

	/* The rectangle's size. */
	word = display_read(device, AMDRDNA_DCN_SCALER_OUTPUT_SIZE, gop->plane);
	gop->scaler_width = word & READOUT_SCALER_SIZE_FIELD;
	gop->scaler_height = (word >> READOUT_SCALER_SECOND_SHIFT) & READOUT_SCALER_SIZE_FIELD;

	/* The ratios: the viewport's size over the rectangle's, unless the scaler works them out itself. */
	gop->horizontal_ratio = display_read(device, AMDRDNA_DCN_SCALER_H_RATIO, gop->plane) & READOUT_SCALER_RATIO_FIELD;
	gop->vertical_ratio = display_read(device, AMDRDNA_DCN_SCALER_V_RATIO, gop->plane) & READOUT_SCALER_RATIO_FIELD;

	/* The automatic mode: whether the scaler works out the ratios, or centres the viewport, by itself. */
	gop->scaler_automatic = display_read(device, AMDRDNA_DCN_SCALER_AUTOMATIC, gop->plane) & READOUT_SCALER_AUTOMATIC_FIELD;
}

/*
 * Compares the picture the loader handed the kernel with the plane's
 * surface: both must be the same VRAM, and the same size.
 */
static int
readout_loader(
	struct amdrdna_device *device)
{
	struct kern_boot_framebuffer boot_framebuffer;
	const struct kern_boot_framebuffer *framebuffer;
	struct amdrdna_gop *gop;
	int error;

	/* A boot without a framebuffer has no picture to compare. */
	gop = &device->gop;
	gop->loader_present = false;
	framebuffer = &boot_framebuffer;
	error = kern_boot_framebuffer(READOUT_FRAMEBUFFER_HANDOFF, &boot_framebuffer);
	if (error != 0) {
		amdrdna_mark("A1 note: the loader handed over no framebuffer");
		return 0;
	}

	/* Records the loader's picture and whether it lies in this GPU's VRAM window. */
	gop->loader_present = true;
	gop->loader_physical = framebuffer->physical_base;
	gop->loader_width = framebuffer->width;
	gop->loader_height = framebuffer->height;
	gop->loader_stride = framebuffer->pitch;
	gop->loader_on_gpu = false;
	if (framebuffer->physical_base >= device->vram_bus &&
	    framebuffer->physical_base - device->vram_bus < device->vram_window_bytes) {
		gop->loader_on_gpu = true;
		gop->loader_offset = framebuffer->physical_base - device->vram_bus;
	}

	/* A picture on another device is noted; this GPU's display is then not the console. */
	if (!gop->loader_on_gpu) {
		amdrdna_mark("A1 note: the loader's framebuffer at 0x%llx is not on this GPU",
		    (unsigned long long)framebuffer->physical_base);
		return 0;
	}

	/* The loader's picture must be the lit plane's surface. */
	if (!gop->lit || !gop->surface_in_vram || gop->loader_offset != gop->surface_offset) {
		amdrdna_mark("A1 fail: loader framebuffer +0x%llx is not the plane's surface +0x%llx (lit=%u)",
		    (unsigned long long)gop->loader_offset,
		    (unsigned long long)gop->surface_offset,
		    gop->lit ? 1U : 0U);
		return EINVAL;
	}

	/* Succeeded: the console's picture lies where the lit plane fetches its surface. */
	return 0;
}

/*
 * Compares the size of the loader's picture with the part of the surface
 * the plane fetches, finds how that surface reaches the screen, and marks
 * the scaler.
 */
static int
readout_picture(
	struct amdrdna_device *device)
{
	struct amdrdna_picture_input input;
	struct amdrdna_gop *gop;
	int error;

	/* Nothing lit shows no picture to compare. */
	gop = &device->gop;
	if (!gop->lit)
		return 0;

	/* Gathers the timing, the viewport, the scaler and, when it is this GPU's, the loader's picture. */
	input.active_width = gop->width;
	input.active_height = gop->height;
	input.viewport_x = gop->viewport_x;
	input.viewport_y = gop->viewport_y;
	input.viewport_width = gop->viewport_width;
	input.viewport_height = gop->viewport_height;
	input.scaler_mode = gop->scaler_mode;
	input.scaler_x = gop->scaler_x;
	input.scaler_y = gop->scaler_y;
	input.scaler_width = gop->scaler_width;
	input.scaler_height = gop->scaler_height;
	input.horizontal_ratio = gop->horizontal_ratio;
	input.vertical_ratio = gop->vertical_ratio;
	input.scaler_automatic = gop->scaler_automatic;
	input.loader_present = gop->loader_on_gpu;
	input.loader_width = gop->loader_width;
	input.loader_height = gop->loader_height;

	/* Marks the scaler as read, whatever the comparison finds. */
	amdrdna_mark("A1 scaler dpp=%u mode=%u auto=%u out=%u,%u %ux%u ratio=0x%07x/0x%07x viewport=%u,%u %ux%u active=%ux%u",
	    gop->plane,
	    (unsigned)gop->scaler_mode,
	    (unsigned)gop->scaler_automatic,
	    (unsigned)gop->scaler_x,
	    (unsigned)gop->scaler_y,
	    (unsigned)gop->scaler_width,
	    (unsigned)gop->scaler_height,
	    (unsigned)gop->horizontal_ratio,
	    (unsigned)gop->vertical_ratio,
	    (unsigned)gop->viewport_x,
	    (unsigned)gop->viewport_y,
	    (unsigned)gop->viewport_width,
	    (unsigned)gop->viewport_height,
	    (unsigned)gop->width,
	    (unsigned)gop->height);

	/* Compares the pictures and finds the shape. */
	error = amdrdna_picture_check(&input, &gop->picture);
	if (error == ENOTSUP) {
		amdrdna_mark("A1 fail: hubp %u fetches from %u,%u of its surface, not from the loader's origin",
		    gop->plane,
		    (unsigned)gop->viewport_x,
		    (unsigned)gop->viewport_y);
		return error;
	}

	/* Any other refusal is a viewport that is empty or of another size than the loader's picture. */
	if (error != 0) {
		amdrdna_mark("A1 fail: loader framebuffer %ux%u is not hubp %u's viewport %ux%u (active %ux%u)",
		    (unsigned)gop->loader_width,
		    (unsigned)gop->loader_height,
		    gop->plane,
		    (unsigned)gop->viewport_width,
		    (unsigned)gop->viewport_height,
		    (unsigned)gop->width,
		    (unsigned)gop->height);
		return error;
	}

	/* A rectangle outside the screen is only noted; the plane still fetches the console. */
	if (!gop->picture.rectangle_inside)
		amdrdna_mark("A1 note: the scaler's rectangle is not inside the active area");

	/* So is a scaler whose mode or ratios do not fit the sizes. */
	if (!gop->picture.ratios_agree)
		amdrdna_mark("A1 note: the scaler's mode and ratios do not fit the viewport and the rectangle");

	/* Succeeded: the loader's picture is the fetched surface, and its shape is known. */
	return 0;
}

/* Names an encoder mode for the log. */
static const char *
output_kind_name(
	unsigned kind)
{
	/* Each mode the display engine knows. */
	switch (kind) {
	case AMDRDNA_OUTPUT_DP:
		return "DP";
	case AMDRDNA_OUTPUT_DVI:
		return "DVI";
	case AMDRDNA_OUTPUT_HDMI:
		return "HDMI";
	case AMDRDNA_OUTPUT_DP_MST:
		return "DP-MST";
	default:
		break;
	}

	/* No encoder, or a mode the driver does not name. */
	return "none";
}
