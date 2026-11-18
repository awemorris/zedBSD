/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Stage A10: reading the sink of the firmware's output, without changing
 * the picture.
 *
 * The output the firmware lit is driven by one display transmitter (the
 * encoder back end's number); the video BIOS's path table names the
 * connector behind it, its I2C line and its pad.  The EDID is read twice:
 * by the display engine's I2C engine on that line, and by the video BIOS's
 * own I2C routine run by the driver's runner (bytecode.c) on the board's
 * registers, which checks the runner on the hardware; the two must read
 * the same bytes.  A DisplayPort signal also has its receiver's
 * capabilities read over the AUX channel.  Of the modes the EDID lists
 * the output shows those inside the signal's pixel clock, held by the
 * display's frames and planned at the clocks the board runs (mode-plan.c):
 * the display's mode enumeration offers those, and a present of one of
 * their sizes sets it.  A monitor's plug reads the sink again the same
 * way.
 *
 * Every engine is taken through its arbitration and given back, and the
 * pad's mode word is put back as it was.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <kern/clock.h>
#include <kern/kcrt.h>
#include <kern/kmem.h>
#include <uapi/amdrdna.h>
#include <uapi/errno.h>

#include "drivers/gpu/amdrdna/amdrdna-bytecode.h"
#include "drivers/gpu/amdrdna/amdrdna-private.h"
#include "drivers/gpu/amdrdna/amdrdna-sink.h"

/*
 * The I2C engine's words (GC... the display block's window 2).
 *
 * The control word: go (bit 0), soft reset (1), send a bus reset (2),
 * reset the status (3), the line (bits 10:8, the line less one) and the
 * transactions less one (21:20).  The arbitration word: the owner (bits
 * 3:2: 0 idle, 1 software, 2 the hardware or the micro-controller),
 * queue no software start (4), ask for the engine (20), give it back
 * (21).  The status word: the state (bits 1:0, 1 busy for software), done
 * (2), aborted (4), timed out (5), stopped on a NACK (8).  A line's speed
 * word: the threshold (bits 1:0), the start and stop timing (9:8) and the
 * prescale (31:16); its setup word: the data and clock drive (0, 7), the
 * drive select (1), the bus reset length (2), enable (6), the delays
 * (15:8, 23:16) and the time limit (31:24).  A transaction word: read
 * (bit 0), stop on a NACK (8), start (12), stop (13) and the count
 * (25:16).  The data word: read (bit 0), the byte (15:8), the index
 * (25:16) and write the index (31).  The microsecond time base: its
 * divider (bits 6:0) and the crystal's divider (14:8).
 *
 * Values referenced: Linux v6.19
 * drivers/gpu/drm/amd/include/asic_reg/dcn/dcn_2_0_0_sh_mask.h
 * (DC_I2C_CONTROL, DC_I2C_ARBITRATION, DC_I2C_SW_STATUS,
 * DC_I2C_DDC1_SPEED, DC_I2C_DDC1_SETUP, DC_I2C_TRANSACTION0, DC_I2C_DATA,
 * MICROSECOND_TIME_BASE_DIV) and display/dc/dce/dce_i2c_hw.h
 * (enum dc_i2c_status, DC_I2C_REG_RW_CNTL_STATUS_DMCU_ONLY).
 */
#define I2C_GO				0x00000001U
#define I2C_SOFT_RESET			0x00000002U
#define I2C_SEND_RESET			0x00000004U
#define I2C_STATUS_RESET		0x00000008U
#define I2C_LINE_SHIFT			8U
#define I2C_LINE_FIELD			0x00000700U
#define I2C_COUNT_SHIFT			20U
#define I2C_COUNT_FIELD			0x00300000U
#define I2C_OWNER_SHIFT			2U
#define I2C_OWNER_FIELD			0x3U
#define I2C_OWNER_SOFTWARE		1U
#define I2C_OWNER_OTHER			2U
#define I2C_NO_QUEUED_GO		0x00000010U
#define I2C_ASK				0x00100000U
#define I2C_GIVE_BACK			0x00200000U
#define I2C_STATE_FIELD			0x00000003U
#define I2C_STATE_BUSY			1U
#define I2C_DONE			0x00000004U
#define I2C_ABORTED			0x00000010U
#define I2C_TIMED_OUT			0x00000020U
#define I2C_NACK			0x00000100U
#define I2C_THRESHOLD_FIELD		0x00000003U
#define I2C_START_STOP_FIELD		0x00000300U
#define I2C_START_STOP_SHIFT		8U
#define I2C_PRESCALE_SHIFT		16U
#define I2C_PRESCALE_FIELD		0xffff0000U
#define I2C_DRIVES			0x00ffff83U
#define I2C_RESET_LENGTH		0x00000004U
#define I2C_ENABLE			0x00000040U
#define I2C_LIMIT_SHIFT			24U
#define I2C_LIMIT_FIELD			0xff000000U
#define I2C_READ			0x00000001U
#define I2C_STOP_ON_NACK		0x00000100U
#define I2C_START			0x00001000U
#define I2C_STOP			0x00002000U
#define I2C_BYTES_SHIFT			16U
#define I2C_DATA_READ			0x00000001U
#define I2C_DATA_SHIFT			8U
#define I2C_DATA_FIELD			0x0000ff00U
#define I2C_INDEX_SHIFT			16U
#define I2C_INDEX_WRITE			0x80000000U
#define TIME_BASE_FIELD			0x0000007fU
#define TIME_CRYSTAL_SHIFT		8U
#define TIME_CRYSTAL_FIELD		0x7fU

/*
 * What the I2C engine is set to: 100 kHz (with the threshold 2 and the
 * start and stop timing of a speed over 50 kHz, 2), a time limit of 3
 * and the 9-clock bus reset; the reference clock when the time base gives
 * none (the crystal's 100 MHz, from the board's display table), the
 * crystal divider when it gives none (2), and the engine's buffer.
 *
 * Values referenced: Linux v6.19
 * drivers/gpu/drm/amd/display/dc/resource/dcn20/dcn20_resource.c
 * (i2c_speed_in_khz 100), display/dc/dce/dce_i2c_hw.c (set_speed,
 * setup_engine, dcn2_i2c_hw_construct) and dce_i2c_hw.h
 * (I2C_SETUP_TIME_LIMIT_DCN 3, I2C_SEND_RESET_LENGTH_9,
 * I2C_HW_BUFFER_SIZE_DCE100 538).
 */
#define I2C_SPEED_KHZ			100U
#define I2C_THRESHOLD			2U
#define I2C_START_STOP			2U
#define I2C_LIMIT			3U
#define I2C_REFERENCE_KHZ		100000U
#define I2C_CRYSTAL_DIVIDER		2U
#define I2C_BUFFER_BYTES		538U

/* How long an I2C transfer is waited for, in steps of 10 us (100 ms). */
#define I2C_WAIT_STEPS			10000U
#define I2C_WAIT_STEP_US		10U

/*
 * A line's pad word: the clock and data outputs left to the engine (bits
 * 0 and 8 clear), their pull-downs (4 and 12), and the AUX pad mode (16;
 * clear for I2C).  The output word of a line's pad is the word after it
 * (the pin table names that one).
 *
 * Values referenced: Linux v6.19
 * drivers/gpu/drm/amd/include/asic_reg/dcn/dcn_2_0_0_sh_mask.h
 * (DC_GPIO_DDC1_MASK: DC_GPIO_DDC1CLK_MASK, DC_GPIO_DDC1CLK_PD_EN,
 * DC_GPIO_DDC1DATA_MASK, DC_GPIO_DDC1DATA_PD_EN, AUX_PAD1_MODE),
 * dcn_2_0_0_offset.h (mmDC_GPIO_DDC1_MASK 0x28d0, mmDC_GPIO_DDC1_A 0x28d1),
 * and display/dc/gpio/hw_ddc.c, hw_gpio.c (set_config,
 * dal_hw_gpio_config_mode: the hardware mode and the I2C pad mode).
 */
#define PAD_CLOCK_SOFTWARE		0x00000001U
#define PAD_DATA_SOFTWARE		0x00000100U
#define PAD_CLOCK_PULL_DOWN		0x00000010U
#define PAD_DATA_PULL_DOWN		0x00001000U
#define PAD_AUX_MODE			0x00010000U
#define PAD_OUTPUT_AFTER_MASK		1U

/*
 * The AUX channel's words: enable (bit 0) in its control; go (0), the
 * start delay (7:4) and the bytes to send (20:16) in its request control;
 * the owner (3:2: 1 software, 2 the micro-controller), ask (16) and give
 * back (17) in its arbitration; acknowledge done (1) in its interrupt
 * control; done (0), the receive timeouts (6:4, 7), the hot-plug
 * disconnect (9), the receive errors (14, 20, 22, 23) and the reply's
 * bytes (28:24) in its status; read (0), the byte (15:8), the index
 * (20:16) and no index step (31) in its data word.
 *
 * Values referenced: Linux v6.19
 * drivers/gpu/drm/amd/include/asic_reg/dcn/dcn_2_0_0_sh_mask.h
 * (DP_AUX0_AUX_CONTROL, DP_AUX0_AUX_SW_CONTROL, DP_AUX0_AUX_ARB_CONTROL,
 * DP_AUX0_AUX_INTERRUPT_CONTROL, DP_AUX0_AUX_SW_STATUS,
 * DP_AUX0_AUX_SW_DATA) and display/dc/dce/dce_aux.c (SW_CAN_ACCESS_AUX 1,
 * DMCU_CAN_ACCESS_AUX 2).
 */
#define AUX_ENABLE			0x00000001U
#define AUX_GO				0x00000001U
#define AUX_DELAY_FIELD			0x000000f0U
#define AUX_SEND_SHIFT			16U
#define AUX_SEND_FIELD			0x001f0000U
#define AUX_OWNER_SHIFT			2U
#define AUX_OWNER_FIELD			0x3U
#define AUX_OWNER_SOFTWARE		1U
#define AUX_OWNER_OTHER			2U
#define AUX_ASK				0x00010000U
#define AUX_GIVE_BACK			0x00020000U
#define AUX_DONE_ACK			0x00000002U
#define AUX_DONE			0x00000001U
#define AUX_TIMED_OUT			0x000000f0U
#define AUX_DISCONNECTED		0x00000200U
#define AUX_BAD_REPLY			0x00d04000U
#define AUX_REPLY_SHIFT			24U
#define AUX_REPLY_FIELD			0x1fU
#define AUX_DATA_READ			0x00000001U
#define AUX_DATA_SHIFT			8U
#define AUX_DATA_FIELD			0x0000ff00U
#define AUX_INDEX_SHIFT			16U
#define AUX_NO_STEP			0x80000000U

/*
 * A native AUX read's command nibble (9), a reply's acknowledgement (the
 * high nibble of its first byte 0), the most bytes one request moves (16),
 * and how long it is waited for (10 ms in steps of 10 us).
 *
 * Values referenced: VESA DisplayPort Standard 1.4, section 2.7.4 (the
 * AUX request and reply commands, 16 bytes at most) and Linux v6.19
 * drivers/gpu/drm/amd/display/dc/dce/dce_aux.c (submit_channel_request,
 * read_channel_reply: the header bytes, the index reset, the reply's
 * status in the high nibble).
 */
#define AUX_NATIVE_READ			0x90U
#define AUX_REPLY_ACK			0U
#define AUX_BYTES_MAX			16U
#define AUX_WAIT_STEPS			1000U
#define AUX_WAIT_STEP_US		10U

/*
 * The receiver capability bytes the driver reads (the first 16 of the
 * DPCD) and those it names: the revision (0), the highest link rate (1)
 * and the lane count (2, bits 4:0).
 *
 * Values referenced: VESA DisplayPort Standard 1.4, section 2.9.3.1
 * (DPCD 00000h DPCD_REV, 00001h MAX_LINK_RATE, 00002h MAX_LANE_COUNT).
 */
#define DPCD_CAPABILITY_BYTES		16U
#define DPCD_REVISION			0U
#define DPCD_RATE			1U
#define DPCD_LANES			2U
#define DPCD_LANES_FIELD		0x1fU

/*
 * The I2C address of the EDID (0x50, 0xa0 to write and 0xa1 to read on the
 * wire).
 *
 * Value referenced: VESA Enhanced Display Data Channel Standard (E-DDC)
 * Version 1.2, section 2.2 (the EDID's address A0h/A1h).
 */
#define EDID_ADDRESS			0x50U

/*
 * The video BIOS's I2C routine (54 in the command list) and its eight
 * parameter bytes: the speed in kHz (0), the register index, which comes
 * back as the status (1; 1 for success), the data to write (2, 3), the
 * flag (4; 0 to read), the bytes (5), the address on the wire (6) and the
 * line's identifier (7); what it reads lands at the start of the scratch
 * area; its speed of 50 kHz and its limit of 255 bytes a read.
 *
 * Values referenced: Linux v6.19
 * drivers/gpu/drm/amd/include/atomfirmware.h
 * (struct atom_master_list_of_command_functions_v2_1:
 * processi2cchanneltransaction), atombios.h
 * (PROCESS_I2C_CHANNEL_TRANSACTION_PARAMETERS, HW_I2C_READ,
 * HW_ASSISTED_I2C_STATUS_SUCCESS) and
 * drivers/gpu/drm/amd/amdgpu/atombios_i2c.c (TARGET_HW_I2C_CLOCK 50,
 * ATOM_MAX_HW_I2C_READ 255, the data read from the scratch area's start).
 */
#define BIOS_I2C_ROUTINE		0x36U
#define BIOS_I2C_SPEED_KHZ		50U
#define BIOS_I2C_READ			0U
#define BIOS_I2C_OK			1U
#define BIOS_I2C_PARAMETER_WORDS	2U

/* The scratch area the video BIOS's routines are given (20 KiB, as the reference gives one without a reservation). */
#define BIOS_SCRATCH_BYTES		(20U * 1024U)

/*
 * The pixel clock each signal carries at most here: a single-link DVI
 * link 165 MHz, an HDMI link without scrambling 340 MHz; a DisplayPort
 * link its lanes times its rate in 10 bits a symbol carrying 8, over 24
 * bits a pixel.
 *
 * Values referenced: DVI 1.0 section 2.2.4 (165 MHz single link), HDMI
 * 1.4b section 6.1 (340 MHz TMDS character rate) and VESA DisplayPort
 * Standard 1.4, section 3.5 (8b/10b, link rates in 0.27 Gbps units).
 */
#define LIMIT_DVI_KHZ			165000U
#define LIMIT_HDMI_KHZ			340000U
#define DP_RATE_UNIT_KHZ		270000U

/* How near two refreshes are to be the same mode (half a hertz). */
#define SINK_SAME_REFRESH_MILLIHZ	500U

/* No limit on the pixel clock (in kHz); the bytes of a pixel of the surface. */
#define SINK_NO_LIMIT_KHZ		4294967295U
#define SINK_PIXEL_BYTES		4U

/* The names of the transmitters for the log. */
static const char transmitter_letters[AMDRDNA_TRANSMITTERS + 1U] = "ABCDEFG";

/* What a run of the video BIOS's routines reaches: the device's registers. */
struct sink_bios {
	struct amdrdna_device *device;
};

static int sink_path(struct amdrdna_device *device, struct amdrdna_sink *sink);
static const char *sink_connector_name(enum amdrdna_connector_kind kind);
static int sink_edid_engine(struct amdrdna_device *device, struct amdrdna_sink *sink);
static int sink_edid_bios(struct amdrdna_device *device, struct amdrdna_sink *sink);
static int sink_bios_run(struct amdrdna_device *device, unsigned routine, uint32_t *parameters, uint32_t parameter_words, uint32_t *steps, uint32_t *scratch_out, uint32_t scratch_words);
static int sink_dpcd(struct amdrdna_device *device, struct amdrdna_sink *sink);
static void sink_limit(struct amdrdna_device *device, struct amdrdna_sink *sink);
static void sink_carried(struct amdrdna_device *device, struct amdrdna_sink *sink);
static bool sink_same_refresh(uint32_t listed, uint32_t asked);
static unsigned sink_find(const struct amdrdna_sink *sink, uint32_t width, uint32_t height, uint32_t refresh_millihz);
static int i2c_read(struct amdrdna_device *device, unsigned line, uint8_t offset, uint8_t *bytes, unsigned count);
static int i2c_take(struct amdrdna_device *device, unsigned line);
static void i2c_give_back(struct amdrdna_device *device, unsigned line);
static int i2c_wait(struct amdrdna_device *device);
static int aux_read(struct amdrdna_device *device, unsigned channel, uint32_t address, uint8_t *bytes, unsigned count);
static uint32_t sink_read(struct amdrdna_device *device, enum amdrdna_dcn_register which, unsigned instance);
static void sink_write(struct amdrdna_device *device, enum amdrdna_dcn_register which, unsigned instance, uint32_t value);
static uint32_t bios_read(void *owner, uint32_t index);
static void bios_write(void *owner, uint32_t index, uint32_t value);
static void bios_wait(void *owner, uint32_t microseconds);

/*
 * Stage A10: finds the firmware's output's path, reads the sink's EDID by
 * the I2C engine and by the video BIOS's routine and compares them, reads
 * a DisplayPort receiver's capabilities, and keeps the modes the output
 * can show.  Nothing on the screen changes.
 *
 * Returns 0, or the error of the step that failed (no output, no path,
 * no EDID).
 */
int
amdrdna_a10_sink(
	struct amdrdna_device *device)
{
	struct amdrdna_sink *sink;
	struct amdrdna_timing preferred;
	const char *atom_word;
	const char *dpcd_word;
	int error;

	/* The sink, read into the device's record and published. */
	sink = &device->sink;
	error = amdrdna_sink_read(device, sink);
	if (error != 0)
		return error;
	__atomic_store_n(&sink->ready, true, __ATOMIC_RELEASE);

	/* Says what was read, with the preferred mode when the EDID names one. */
	atom_word = "same";
	dpcd_word = "none";
	if (sink->dpcd_read)
		dpcd_word = "read";
	if (sink->bios_error != 0)
		atom_word = "differs";
	preferred = (struct amdrdna_timing){ 0 };
	if (sink->edid.mode_count != 0U && sink->edid.modes[0].source == AMDRDNA_TIMING_PREFERRED)
		preferred = sink->edid.modes[0];
	amdrdna_mark("A10 ok conn=%s signal=%u ddc=%u edid=%s %04x \"%s\" %ux%u@%u.%03u blocks=%u modes=%u/%u limit=%ukHz atom=%s(%d, %u steps) dpcd=%s",
	    sink_connector_name(sink->connector.kind),
	    device->gop.output_kind,
	    sink->ddc_line,
	    sink->edid.vendor,
	    (unsigned)sink->edid.product,
	    sink->edid.name,
	    (unsigned)preferred.width,
	    (unsigned)preferred.height,
	    (unsigned)(preferred.refresh_millihz / 1000U),
	    (unsigned)(preferred.refresh_millihz % 1000U),
	    sink->edid.blocks,
	    sink->usable_modes,
	    sink->edid.mode_count,
	    (unsigned)sink->limit_khz,
	    atom_word,
	    sink->bios_error,
	    (unsigned)sink->bios_steps,
	    dpcd_word);
	if (sink->dpcd_read) {
		amdrdna_mark("A10 dpcd rev=%u.%u rate=0x%02x lanes=%u",
		    (unsigned)(sink->dpcd[DPCD_REVISION] >> 4),
		    (unsigned)(sink->dpcd[DPCD_REVISION] & 0xfU),
		    (unsigned)sink->dpcd[DPCD_RATE],
		    (unsigned)(sink->dpcd[DPCD_LANES] & DPCD_LANES_FIELD));
	}

	/* Succeeded: the sink is read. */
	return 0;
}

/*
 * Reads the firmware's output's sink into a record, which is not
 * published: the output's path, the EDID by the I2C engine and by the
 * video BIOS's routine, a DisplayPort receiver's capabilities, the
 * signal's limit and the modes the output shows.  The bring-up thread
 * calls it (it runs the video BIOS).
 *
 * Returns 0, or the error of the step that failed (no output, no path,
 * no EDID).
 */
int
amdrdna_sink_read(
	struct amdrdna_device *device,
	struct amdrdna_sink *sink)
{
	unsigned usable;
	unsigned index;
	int error;

	/* Starts over. */
	*sink = (struct amdrdna_sink){ 0 };

	/* The output's path. */
	error = sink_path(device, sink);
	if (error != 0)
		return error;

	/* The EDID by the I2C engine; a sink that does not answer has no modes to offer. */
	error = sink_edid_engine(device, sink);
	if (error != 0) {
		amdrdna_mark("A10 fail: no EDID on line %u (%d)", sink->ddc_line, error);
		return error;
	}

	/* The same block by the video BIOS's routine (the runner on the board); a difference is noted, not fatal. */
	sink->bios_error = sink_edid_bios(device, sink);

	/* A DisplayPort receiver's capabilities. */
	if (device->gop.output_kind == AMDRDNA_OUTPUT_DP || device->gop.output_kind == AMDRDNA_OUTPUT_DP_MST)
		sink->dpcd_error = sink_dpcd(device, sink);

	/* What the signal carries, and the modes the output shows. */
	sink_limit(device, sink);
	sink_carried(device, sink);
	usable = 0U;
	for (index = 0U; index < sink->edid.mode_count; index++) {
		if (sink->carried[index])
			usable++;
	}
	sink->usable_modes = usable;

	/* Succeeded: the record is complete. */
	return 0;
}

/*
 * Reports the sink stage A10 read, or NULL before it did (the stage's
 * thread publishes it once, after every field).
 */
const struct amdrdna_sink *
amdrdna_sink_ready(
	const struct amdrdna_device *device)
{
	bool ready;

	/* Published once. */
	ready = __atomic_load_n(&device->sink.ready, __ATOMIC_ACQUIRE);
	if (!ready)
		return NULL;

	/* Succeeded: the sink is read. */
	return &device->sink;
}

/*
 * Reports the index-th mode the output offers (index 0 is the current
 * one, then the sink's modes the output carries, without the current
 * one's double); returns false past the last.  count gets how many there
 * are.
 */
bool
amdrdna_sink_mode(
	const struct amdrdna_device *device,
	unsigned index,
	uint32_t *width,
	uint32_t *height,
	uint32_t *refresh_millihz,
	unsigned *count)
{
	const struct amdrdna_sink *sink;
	const struct amdrdna_display *display;
	const struct amdrdna_timing *mode;
	unsigned listed;
	unsigned at;
	bool current;

	/* The current mode is always the first. */
	display = &device->display;
	sink = amdrdna_sink_ready(device);
	listed = 1U;
	if (index == 0U) {
		*width = display->width;
		*height = display->height;
		*refresh_millihz = display->refresh_millihz;
	}

	/* The sink's modes the output carries, each but the current one's double. */
	for (at = 0U; sink != NULL && at < sink->edid.mode_count; at++) {
		mode = &sink->edid.modes[at];
		if (!sink->carried[at])
			continue;
		current = false;
		if (mode->width == display->width && mode->height == display->height) {
			current = sink_same_refresh(mode->refresh_millihz, display->refresh_millihz);
		}
		if (current)
			continue;
		if (listed == index) {
			*width = mode->width;
			*height = mode->height;
			*refresh_millihz = mode->refresh_millihz;
		}
		listed++;
	}

	/* The count, and whether the index was one of them. */
	*count = listed;
	if (index >= listed)
		return false;

	/* Succeeded: the mode is given. */
	return true;
}

/*
 * Checks a mode without setting it: the current one, or a mode the sink
 * lists that the output carries; *refresh_millihz 0 asks for the
 * preferred refresh of that size (the current one's, else the sink's).
 * Returns 0 with the refresh, ERANGE for a listed mode past what the
 * output carries now, or EINVAL for one the sink does not list.
 */
int
amdrdna_sink_check(
	const struct amdrdna_device *device,
	uint32_t width,
	uint32_t height,
	uint32_t *refresh_millihz)
{
	const struct amdrdna_sink *sink;
	const struct amdrdna_display *display;
	const struct amdrdna_timing *mode;
	unsigned index;
	unsigned listed;
	bool current;

	/* The current mode at its refresh (none asked for, or within half a hertz of it). */
	display = &device->display;
	current = false;
	if (width == display->width && height == display->height && *refresh_millihz == 0U) {
		current = true;
	} else if (width == display->width && height == display->height) {
		current = sink_same_refresh(display->refresh_millihz, *refresh_millihz);
	}
	if (current) {
		*refresh_millihz = display->refresh_millihz;
		return 0;
	}

	/* The firmware's mode, which a mode set left (it is always shown again). */
	if (display->mode_known && width == display->firmware_mode.width && height == display->firmware_mode.height) {
		current = true;
		if (*refresh_millihz != 0U)
			current = sink_same_refresh(display->firmware_mode.refresh_millihz, *refresh_millihz);
		if (current) {
			*refresh_millihz = display->firmware_mode.refresh_millihz;
			return 0;
		}
	}

	/* A mode the sink lists. */
	sink = amdrdna_sink_ready(device);
	if (sink == NULL)
		return EINVAL;
	index = sink_find(sink, width, height, *refresh_millihz);
	if (index >= sink->edid.mode_count) {
		/* Listed but not shown here, or not listed at all. */
		listed = amdrdna_edid_find(&sink->edid, width, height, *refresh_millihz);
		if (listed < sink->edid.mode_count)
			return ERANGE;
		return EINVAL;
	}
	mode = &sink->edid.modes[index];
	*refresh_millihz = mode->refresh_millihz;

	/* Succeeded: the mode can be lit. */
	return 0;
}

/*
 * Gives the timing of a mode a present asks for: the firmware's (its
 * size, at its refresh or none asked for), else a mode the sink lists
 * that the output shows (0 asks for the first of that size).
 *
 * Returns 0; EINVAL for a size neither is; ERANGE for a listed mode the
 * output does not show.
 */
int
amdrdna_sink_choose(
	const struct amdrdna_device *device,
	uint32_t width,
	uint32_t height,
	uint32_t refresh_millihz,
	struct amdrdna_timing *timing)
{
	const struct amdrdna_display *display;
	const struct amdrdna_sink *sink;
	unsigned index;
	unsigned listed;
	bool same;

	/* The firmware's mode. */
	display = &device->display;
	if (display->mode_known && width == display->firmware_mode.width && height == display->firmware_mode.height) {
		same = true;
		if (refresh_millihz != 0U)
			same = sink_same_refresh(display->firmware_mode.refresh_millihz, refresh_millihz);
		if (same) {
			*timing = display->firmware_mode;
			return 0;
		}
	}

	/* A mode the sink lists. */
	sink = amdrdna_sink_ready(device);
	if (sink == NULL)
		return EINVAL;
	index = sink_find(sink, width, height, refresh_millihz);
	if (index >= sink->edid.mode_count) {
		/* Listed but not shown here, or not listed at all. */
		listed = amdrdna_edid_find(&sink->edid, width, height, refresh_millihz);
		if (listed < sink->edid.mode_count)
			return ERANGE;
		return EINVAL;
	}

	/* Succeeded: the timing is given. */
	*timing = sink->edid.modes[index];
	return 0;
}

/*
 * Tells whether a sink record lists a mode with a timing's size and
 * refresh (within half a hertz) that the output shows.
 */
bool
amdrdna_sink_carries(
	const struct amdrdna_sink *sink,
	const struct amdrdna_timing *timing)
{
	const struct amdrdna_timing *mode;
	unsigned at;
	bool same;

	/* Each mode the output shows, until one is the timing's. */
	for (at = 0U; at < sink->edid.mode_count; at++) {
		mode = &sink->edid.modes[at];
		if (!sink->carried[at])
			continue;
		if (mode->width != timing->width || mode->height != timing->height)
			continue;
		same = sink_same_refresh(mode->refresh_millihz, timing->refresh_millihz);
		if (same)
			return true;
	}

	/* Succeeded: no; none is. */
	return false;
}

/*
 * Gives the mode a sink record prefers that the output shows: the EDID's
 * preferred mode, else the first it lists that the output shows.  Returns
 * false when the output shows none.
 */
bool
amdrdna_sink_preferred(
	const struct amdrdna_sink *sink,
	struct amdrdna_timing *timing)
{
	unsigned at;

	/* The preferred mode, when the output shows it. */
	if (sink->edid.mode_count != 0U && sink->edid.modes[0].source == AMDRDNA_TIMING_PREFERRED && sink->carried[0]) {
		*timing = sink->edid.modes[0];
		return true;
	}

	/* Else the first the output shows. */
	for (at = 0U; at < sink->edid.mode_count; at++) {
		if (!sink->carried[at])
			continue;
		*timing = sink->edid.modes[at];
		return true;
	}

	/* Succeeded: no; the output shows none. */
	return false;
}

/*
 * Runs a video BIOS routine on the board's registers with its parameters
 * (the runner and a scratch area of its own, made for the run); steps
 * gets the instructions it ran.  The scratch area's first dwords, where a
 * routine leaves what it reads, are copied to scratch_out (scratch_words
 * of them, none when NULL).
 *
 * Returns 0; ENOMEM; or the runner's error (the reason is said).
 */
int
amdrdna_bios_run(
	struct amdrdna_device *device,
	unsigned routine,
	uint32_t *parameters,
	uint32_t parameter_words,
	uint32_t *steps)
{
	int error;

	/* The run without its scratch area's bytes. */
	error = sink_bios_run(device, routine, parameters, parameter_words, steps, NULL, 0U);
	if (error != 0)
		return error;

	/* Succeeded: the routine ran. */
	return 0;
}

/*
 * Finds the path of the firmware's output: its transmitter (the encoder
 * back end's number), the video BIOS's path of it, and the I2C line whose
 * pad the path's I2C identifier names.
 */
static int
sink_path(
	struct amdrdna_device *device,
	struct amdrdna_sink *sink)
{
	uint32_t pad;
	unsigned line;
	int error;

	/* An output the firmware lit, and the image to read its path from. */
	if (!device->gop.has_output) {
		amdrdna_mark("A10 fail: the firmware lit no output");
		return ENODEV;
	}
	if (device->vbios == NULL) {
		amdrdna_mark("A10 fail: no video BIOS image");
		return ENOENT;
	}

	/* The path of the transmitter. */
	error = amdrdna_vbios_connector(device->vbios, device->vbios_bytes, &device->vbios_info, device->gop.output, &sink->connector);
	if (error != 0) {
		amdrdna_mark("A10 fail: no path for transmitter %u (%d)", device->gop.output, error);
		return error;
	}

	/* The line whose pad's output word the pin table names (the word after the line's mask word). */
	sink->ddc_line = 0U;
	for (line = 0U; line < AMDRDNA_DISPLAY_PIPES; line++) {
		pad = amdrdna_dcn_index(&device->blocks.blocks[AMDRDNA_BLOCK_DCN], AMDRDNA_DCN_DDC_PAD_MASK, line);
		if (pad != 0U && pad + PAD_OUTPUT_AFTER_MASK == sink->connector.ddc_pad_register) {
			sink->ddc_line = line + 1U;
			break;
		}
	}
	amdrdna_mark("A10 path transmitter=%c connector=0x%04x encoder=0x%04x i2c=0x%02x pad=0x%x line=%u hotplug=%u",
	    transmitter_letters[device->gop.output < AMDRDNA_TRANSMITTERS ? device->gop.output : AMDRDNA_TRANSMITTERS],
	    (unsigned)sink->connector.connector_object,
	    (unsigned)sink->connector.encoder_object,
	    (unsigned)sink->connector.ddc_identifier,
	    (unsigned)sink->connector.ddc_pad_register,
	    sink->ddc_line,
	    (unsigned)sink->connector.hotplug_pin);
	if (sink->ddc_line == 0U)
		return ENOENT;

	/* Succeeded: the path is known. */
	return 0;
}

/*
 * Reads the EDID by the I2C engine: the base block, then the extensions
 * it announces up to what one segment holds (two blocks), and decodes it.
 */
static int
sink_edid_engine(
	struct amdrdna_device *device,
	struct amdrdna_sink *sink)
{
	unsigned blocks;
	unsigned block;
	int error;

	/* The base block. */
	error = i2c_read(device, sink->ddc_line, 0U, sink->edid_bytes, AMDRDNA_EDID_BLOCK_BYTES);
	if (error != 0)
		return error;
	sink->edid_length = AMDRDNA_EDID_BLOCK_BYTES;

	/* The extensions of the first segment. */
	blocks = 1U + sink->edid_bytes[AMDRDNA_EDID_BLOCK_BYTES - 2U];
	if (blocks > 2U)
		blocks = 2U;
	for (block = 1U; block < blocks; block++) {
		error = i2c_read(device, sink->ddc_line, (uint8_t)(block * AMDRDNA_EDID_BLOCK_BYTES),
		    sink->edid_bytes + block * AMDRDNA_EDID_BLOCK_BYTES, AMDRDNA_EDID_BLOCK_BYTES);
		if (error != 0)
			break;
		sink->edid_length += AMDRDNA_EDID_BLOCK_BYTES;
	}

	/* The decode. */
	error = amdrdna_edid_decode(sink->edid_bytes, sink->edid_length, &sink->edid);
	if (error != 0)
		return error;

	/* Succeeded: the EDID is read. */
	return 0;
}

/* Runs a routine as amdrdna_bios_run() does, copying the scratch area's first words out. */
static int
sink_bios_run(
	struct amdrdna_device *device,
	unsigned routine,
	uint32_t *parameters,
	uint32_t parameter_words,
	uint32_t *steps,
	uint32_t *scratch_out,
	uint32_t scratch_words)
{
	struct amdrdna_bytecode_access access;
	struct amdrdna_bytecode *runner;
	struct sink_bios bios;
	uint32_t *scratch;
	int error;

	/* The runner and its scratch area. */
	*steps = 0U;
	runner = kern_calloc(1U, sizeof(*runner));
	scratch = kern_calloc(1U, BIOS_SCRATCH_BYTES);
	if (runner == NULL || scratch == NULL) {
		if (runner != NULL)
			kern_free(runner);
		if (scratch != NULL)
			kern_free(scratch);
		return ENOMEM;
	}
	bios.device = device;
	access.owner = &bios;
	access.read = bios_read;
	access.write = bios_write;
	access.wait = bios_wait;
	error = amdrdna_bytecode_open(runner, device->vbios, device->vbios_bytes, &access, scratch, BIOS_SCRATCH_BYTES);

	/* The routine. */
	if (error == 0) {
		error = amdrdna_bytecode_run(runner, routine, parameters, parameter_words);
		*steps = (uint32_t)runner->steps;
		if (error != 0)
			amdrdna_mark("the video BIOS's routine 0x%02x stopped: %s at 0x%x (%d)", routine,
			    runner->failure != NULL ? runner->failure : "?", (unsigned)runner->failed_at, error);
	}

	/* What it left in the scratch area. */
	if (error == 0 && scratch_out != NULL)
		kern_memcpy(scratch_out, scratch, (size_t)scratch_words * 4U);

	/* The runner given back. */
	kern_free(scratch);
	kern_free(runner);
	if (error != 0)
		return error;

	/* Succeeded: the routine ran. */
	return 0;
}

/*
 * Reads the base block again by the video BIOS's I2C routine, run on the
 * board's registers, and compares it with the engine's.  Returns 0 when
 * the two are the same; ENOENT when the image has no such routine; EIO
 * when the routine reported a failure or read other bytes; the runner's
 * error when it stopped.
 */
static int
sink_edid_bios(
	struct amdrdna_device *device,
	struct amdrdna_sink *sink)
{
	uint32_t parameters[BIOS_I2C_PARAMETER_WORDS];
	uint32_t block[AMDRDNA_EDID_BLOCK_BYTES / 4U];
	uint8_t status;
	int error;

	/* A read of the base block on the path's line: speed, index 0, read, the bytes, the address, the line. */
	parameters[0] = BIOS_I2C_SPEED_KHZ;
	parameters[1] = BIOS_I2C_READ | ((uint32_t)AMDRDNA_EDID_BLOCK_BYTES << 8) | ((uint32_t)(EDID_ADDRESS << 1) << 16) |
			((uint32_t)sink->connector.ddc_identifier << 24);
	error = sink_bios_run(device, BIOS_I2C_ROUTINE, parameters, BIOS_I2C_PARAMETER_WORDS, &sink->bios_steps, block,
	    AMDRDNA_EDID_BLOCK_BYTES / 4U);
	if (error != 0)
		return error;

	/* Its status, then its bytes against the engine's. */
	status = (uint8_t)(parameters[0] >> 8);
	if (status != BIOS_I2C_OK)
		error = EIO;
	else if (kern_memcmp(block, sink->edid_bytes, AMDRDNA_EDID_BLOCK_BYTES) != 0)
		error = EIO;
	if (error != 0) {
		amdrdna_mark("A10 note: the video BIOS's I2C read status %u, first bytes %08x %08x", (unsigned)status,
		    (unsigned)block[0], (unsigned)block[1]);
		return error;
	}

	/* Succeeded: the routine read the same block. */
	return 0;
}

/* Reads a DisplayPort receiver's first capability bytes over the path's AUX channel (the line's). */
static int
sink_dpcd(
	struct amdrdna_device *device,
	struct amdrdna_sink *sink)
{
	int error;

	/* Sixteen bytes from 0. */
	error = aux_read(device, sink->ddc_line - 1U, 0U, sink->dpcd, DPCD_CAPABILITY_BYTES);
	if (error != 0) {
		amdrdna_mark("A10 note: no DPCD on AUX %u (%d)", sink->ddc_line - 1U, error);
		return error;
	}
	sink->dpcd_read = true;

	/* Succeeded: the capabilities are read. */
	return 0;
}

/*
 * Works out the highest pixel clock the output's signal carries: the
 * signal's limit (a DisplayPort link's from its receiver's rate and
 * lanes; the boot display clock for a signal of no known limit) and the
 * sink's range limit.  The display clock the board runs now is the
 * plan's to judge (sink_carried()).
 */
static void
sink_limit(
	struct amdrdna_device *device,
	struct amdrdna_sink *sink)
{
	uint32_t limit;
	uint32_t lanes;

	/* No limit until the signal's. */
	limit = SINK_NO_LIMIT_KHZ;

	/* The signal's limit. */
	switch (device->gop.output_kind) {
	case AMDRDNA_OUTPUT_DVI:
		if (LIMIT_DVI_KHZ < limit)
			limit = LIMIT_DVI_KHZ;
		break;
	case AMDRDNA_OUTPUT_HDMI:
		if (LIMIT_HDMI_KHZ < limit)
			limit = LIMIT_HDMI_KHZ;
		break;
	case AMDRDNA_OUTPUT_DP:
	case AMDRDNA_OUTPUT_DP_MST:
		/* Lanes times the rate's symbols, each carrying a byte, over three bytes a pixel. */
		lanes = sink->dpcd[DPCD_LANES] & DPCD_LANES_FIELD;
		if (sink->dpcd_read && (uint64_t)lanes * sink->dpcd[DPCD_RATE] * DP_RATE_UNIT_KHZ / 10U / 3U < limit)
			limit = (uint32_t)((uint64_t)lanes * sink->dpcd[DPCD_RATE] * DP_RATE_UNIT_KHZ / 10U / 3U);
		break;
	default:
		limit = device->vbios_info.boot_display_clock_10khz * 10U;
		break;
	}

	/* The sink's own range limit. */
	if (sink->edid.max_pixel_khz != 0U && sink->edid.max_pixel_khz < limit)
		limit = sink->edid.max_pixel_khz;
	sink->limit_khz = limit;
}

/*
 * Marks the listed modes the output shows: progressive, with known syncs,
 * inside the signal's limit, held by the display's frames (once stage A6
 * made them) and planned at the clocks the board runs.  Without the
 * clocks (their routine failed) the signal's limit alone decides.
 */
static void
sink_carried(
	struct amdrdna_device *device,
	struct amdrdna_sink *sink)
{
	struct amdrdna_mode_plan_input input;
	struct amdrdna_mode_plan plan;
	const struct amdrdna_display *display;
	const struct amdrdna_timing *mode;
	uint32_t pitch;
	unsigned at;
	int clock_error;
	int error;

	/* The clocks once, and the surface's pitch in pixels (the firmware's). */
	display = &device->display;
	input = (struct amdrdna_mode_plan_input){ 0 };
	clock_error = amdrdna_mode_clocks(device, &input);
	if (clock_error != 0)
		amdrdna_mark("A10 note: the board's clocks were not read (%d); the signal's limit alone decides", clock_error);
	pitch = device->gop.pitch_word + 1U;

	/* Each listed mode. */
	for (at = 0U; at < sink->edid.mode_count; at++) {
		mode = &sink->edid.modes[at];
		sink->carried[at] = false;
		if (mode->pixel_khz == 0U || mode->interlaced || mode->pixel_khz > sink->limit_khz)
			continue;
		if (mode->h_pulse == 0U || mode->v_pulse == 0U)
			continue;

		/* The frames hold its rows and lines (once they exist). */
		if (display->frame_bytes != 0U) {
			if ((uint64_t)mode->width * SINK_PIXEL_BYTES > display->stride)
				continue;
			if ((uint64_t)display->stride * mode->height > display->frame_bytes)
				continue;
		}

		/* The plan at the board's clocks. */
		if (clock_error == 0) {
			amdrdna_mode_timing_input(mode, pitch, &input);
			error = amdrdna_mode_plan(&input, &plan);
			if (error != 0)
				continue;
		}
		sink->carried[at] = true;
	}
}

/*
 * Finds a listed mode of a size the output shows: at the refresh asked
 * for (within half a hertz), or with 0 the preferred one, else the
 * highest refresh.  Returns its index, or AMDRDNA_EDID_MODES.
 */
static unsigned
sink_find(
	const struct amdrdna_sink *sink,
	uint32_t width,
	uint32_t height,
	uint32_t refresh_millihz)
{
	const struct amdrdna_timing *mode;
	unsigned best;
	unsigned at;
	bool same;

	/* Each mode of the size that the output shows. */
	best = AMDRDNA_EDID_MODES;
	for (at = 0U; at < sink->edid.mode_count; at++) {
		mode = &sink->edid.modes[at];
		if (!sink->carried[at] ||
		    mode->width != width ||
		    mode->height != height)
			continue;

		/* A refresh asked for. */
		if (refresh_millihz != 0U) {
			same = sink_same_refresh(mode->refresh_millihz, refresh_millihz);
			if (same)
				return at;
			continue;
		}

		/* None asked for: the preferred mode, else the highest refresh. */
		if (mode->source == AMDRDNA_TIMING_PREFERRED)
			return at;
		if (best == AMDRDNA_EDID_MODES || mode->refresh_millihz > sink->edid.modes[best].refresh_millihz)
			best = at;
	}

	/* Succeeded: the best of the size, or none. */
	return best;
}

/* Tells whether a listed refresh is the one asked for: within half a hertz. */
static bool
sink_same_refresh(
	uint32_t listed,
	uint32_t asked)
{
	/* Too low, or too high. */
	if (listed + SINK_SAME_REFRESH_MILLIHZ < asked)
		return false;
	if (listed > asked + SINK_SAME_REFRESH_MILLIHZ)
		return false;

	/* Succeeded: the same refresh. */
	return true;
}

/*
 * Reads count bytes from offset of the EDID's address on a line (1 to 6)
 * by the I2C engine: the offset written, then the bytes read in one go
 * with a repeated start.
 *
 * Returns 0; EBUSY when another owner has the engine; ENXIO when the sink
 * does not answer; ETIMEDOUT or EIO when the transfer timed out or was
 * aborted.
 *
 * Procedure referenced: Linux v6.19
 * drivers/gpu/drm/amd/display/dc/dce/dce_i2c_hw.c (process_transaction,
 * execute_transaction, dce_i2c_hw_engine_submit_payload,
 * process_channel_reply, get_channel_status).
 */
static int
i2c_read(
	struct amdrdna_device *device,
	unsigned line,
	uint8_t offset,
	uint8_t *bytes,
	unsigned count)
{
	uint32_t word;
	unsigned index;
	int error;

	/* A read the buffer holds (the two addresses and the offset besides). */
	if (line == 0U ||
	    line > AMDRDNA_DISPLAY_PIPES ||
	    count == 0U ||
	    count + 3U > I2C_BUFFER_BYTES)
		return EINVAL;

	/* The engine. */
	error = i2c_take(device, line);
	if (error != 0)
		return error;

	/* The first transaction writes the offset and keeps the bus (no stop). */
	sink_write(device, AMDRDNA_DCN_I2C_TRANSACTION, 0U, I2C_STOP_ON_NACK | I2C_START | (1U << I2C_BYTES_SHIFT));
	sink_write(device, AMDRDNA_DCN_I2C_DATA, 0U, ((uint32_t)(EDID_ADDRESS << 1) << I2C_DATA_SHIFT) | I2C_INDEX_WRITE);
	sink_write(device, AMDRDNA_DCN_I2C_DATA, 0U, (uint32_t)offset << I2C_DATA_SHIFT);

	/* The second reads the bytes and stops. */
	sink_write(device, AMDRDNA_DCN_I2C_TRANSACTION, 1U,
	    I2C_READ | I2C_STOP_ON_NACK | I2C_START | I2C_STOP | ((uint32_t)count << I2C_BYTES_SHIFT));
	sink_write(device, AMDRDNA_DCN_I2C_DATA, 0U, (uint32_t)((EDID_ADDRESS << 1) | 1U) << I2C_DATA_SHIFT);

	/* The status reset, the drives off, two transactions, then go. */
	word = sink_read(device, AMDRDNA_DCN_I2C_CONTROL, 0U);
	sink_write(device, AMDRDNA_DCN_I2C_CONTROL, 0U, word | I2C_STATUS_RESET);
	word = sink_read(device, AMDRDNA_DCN_I2C_LINE_SETUP, line - 1U);
	sink_write(device, AMDRDNA_DCN_I2C_LINE_SETUP, line - 1U, word & ~I2C_DRIVES);
	word = sink_read(device, AMDRDNA_DCN_I2C_CONTROL, 0U);
	word &= ~(I2C_SOFT_RESET | I2C_STATUS_RESET | I2C_SEND_RESET | I2C_GO | I2C_COUNT_FIELD);
	word |= (1U << I2C_COUNT_SHIFT);
	sink_write(device, AMDRDNA_DCN_I2C_CONTROL, 0U, word);
	sink_write(device, AMDRDNA_DCN_I2C_CONTROL, 0U, word | I2C_GO);

	/* The transfer, then the bytes from after the three written ones. */
	error = i2c_wait(device);
	if (error == 0) {
		sink_write(device, AMDRDNA_DCN_I2C_DATA, 0U, I2C_DATA_READ | (3U << I2C_INDEX_SHIFT) | I2C_INDEX_WRITE);
		for (index = 0U; index < count; index++) {
			word = sink_read(device, AMDRDNA_DCN_I2C_DATA, 0U);
			bytes[index] = (uint8_t)((word & I2C_DATA_FIELD) >> I2C_DATA_SHIFT);
		}
	}

	/* The engine given back. */
	i2c_give_back(device, line);
	if (error != 0)
		return error;

	/* Succeeded: the bytes are read. */
	return 0;
}

/*
 * Takes the I2C engine for a line: the line's pad to the engine in I2C
 * mode (its word kept for the give-back), the engine out of reset, the
 * arbitration won, the line's speed and setup, and the line selected.
 *
 * Procedure referenced: Linux v6.19
 * drivers/gpu/drm/amd/display/dc/dce/dce_i2c_hw.c (acquire_engine,
 * setup_engine, set_speed) and display/dc/gpio/hw_ddc.c (set_config of
 * the I2C mode).
 */
static int
i2c_take(
	struct amdrdna_device *device,
	unsigned line)
{
	uint32_t word;
	uint32_t owner;
	uint32_t base;
	uint32_t crystal;
	uint32_t reference;
	uint32_t prescale;

	/* The pad: its word kept; the outputs to the engine, the pull-downs on, the I2C pad mode. */
	word = sink_read(device, AMDRDNA_DCN_DDC_PAD_MASK, line - 1U);
	device->sink.pad_word = word;
	word &= ~(PAD_CLOCK_SOFTWARE | PAD_DATA_SOFTWARE | PAD_AUX_MODE);
	word |= PAD_CLOCK_PULL_DOWN | PAD_DATA_PULL_DOWN;
	sink_write(device, AMDRDNA_DCN_DDC_PAD_MASK, line - 1U, word);

	/* The engine out of soft reset. */
	word = sink_read(device, AMDRDNA_DCN_I2C_CONTROL, 0U);
	sink_write(device, AMDRDNA_DCN_I2C_CONTROL, 0U, word & ~I2C_SOFT_RESET);

	/* The arbitration: software has it, or asks for it and must get it. */
	word = sink_read(device, AMDRDNA_DCN_I2C_ARBITRATION, 0U);
	owner = (word >> I2C_OWNER_SHIFT) & I2C_OWNER_FIELD;
	if (owner == I2C_OWNER_OTHER) {
		sink_write(device, AMDRDNA_DCN_DDC_PAD_MASK, line - 1U, device->sink.pad_word);
		return EBUSY;
	}
	if (owner != I2C_OWNER_SOFTWARE) {
		sink_write(device, AMDRDNA_DCN_I2C_ARBITRATION, 0U, word | I2C_ASK);
		word = sink_read(device, AMDRDNA_DCN_I2C_ARBITRATION, 0U);
		owner = (word >> I2C_OWNER_SHIFT) & I2C_OWNER_FIELD;
		if (owner != I2C_OWNER_SOFTWARE) {
			sink_write(device, AMDRDNA_DCN_DDC_PAD_MASK, line - 1U, device->sink.pad_word);
			return EBUSY;
		}
	}

	/* The speed: the reference over the crystal divider over 100 kHz. */
	word = sink_read(device, AMDRDNA_DCN_TIME_BASE, 0U);
	base = word & TIME_BASE_FIELD;
	crystal = (word >> TIME_CRYSTAL_SHIFT) & TIME_CRYSTAL_FIELD;
	if (crystal == 0U)
		crystal = I2C_CRYSTAL_DIVIDER;
	reference = I2C_REFERENCE_KHZ;
	if (base != 0U)
		reference = base * 1000U;
	prescale = reference / crystal / I2C_SPEED_KHZ;
	word = sink_read(device, AMDRDNA_DCN_I2C_LINE_SPEED, line - 1U);
	word &= ~(I2C_PRESCALE_FIELD | I2C_THRESHOLD_FIELD | I2C_START_STOP_FIELD);
	word |= (prescale << I2C_PRESCALE_SHIFT) | I2C_THRESHOLD | (I2C_START_STOP << I2C_START_STOP_SHIFT);
	sink_write(device, AMDRDNA_DCN_I2C_LINE_SPEED, line - 1U, word);

	/* The line selected, no transaction yet, the status reset. */
	word = sink_read(device, AMDRDNA_DCN_I2C_CONTROL, 0U);
	word &= ~(I2C_GO | I2C_SEND_RESET | I2C_COUNT_FIELD | I2C_LINE_FIELD);
	word |= I2C_STATUS_RESET | ((line - 1U) << I2C_LINE_SHIFT);
	sink_write(device, AMDRDNA_DCN_I2C_CONTROL, 0U, word);

	/* The line's time limit and bus reset length, enabled. */
	word = sink_read(device, AMDRDNA_DCN_I2C_LINE_SETUP, line - 1U);
	word &= ~I2C_LIMIT_FIELD;
	word |= (I2C_LIMIT << I2C_LIMIT_SHIFT) | I2C_RESET_LENGTH | I2C_ENABLE;
	sink_write(device, AMDRDNA_DCN_I2C_LINE_SETUP, line - 1U, word);

	/* Software starts queue behind the hardware's. */
	word = sink_read(device, AMDRDNA_DCN_I2C_ARBITRATION, 0U);
	sink_write(device, AMDRDNA_DCN_I2C_ARBITRATION, 0U, word & ~I2C_NO_QUEUED_GO);

	/* Succeeded: the engine is the driver's. */
	return 0;
}

/*
 * Gives the I2C engine back: reset (a soft reset when software still has
 * it), given back through the arbitration (twice when it stays taken),
 * and the line's pad word put back.
 *
 * Procedure referenced: Linux v6.19
 * drivers/gpu/drm/amd/display/dc/dce/dce_i2c_hw.c (release_engine,
 * cntl_stuck_hw_workaround) and display/dc/gpio/hw_gpio.c
 * (dal_hw_gpio_close: the pad's words restored).
 */
static void
i2c_give_back(
	struct amdrdna_device *device,
	unsigned line)
{
	uint32_t word;
	uint32_t status;

	/* The engine's reset; a soft one when software still owns its state. */
	status = sink_read(device, AMDRDNA_DCN_I2C_STATUS, 0U) & I2C_STATE_FIELD;
	word = sink_read(device, AMDRDNA_DCN_I2C_CONTROL, 0U) | I2C_STATUS_RESET;
	if (status == I2C_STATE_BUSY)
		word |= I2C_SOFT_RESET;
	sink_write(device, AMDRDNA_DCN_I2C_CONTROL, 0U, word);

	/* Given back, and again when it stays taken. */
	word = sink_read(device, AMDRDNA_DCN_I2C_ARBITRATION, 0U);
	sink_write(device, AMDRDNA_DCN_I2C_ARBITRATION, 0U, word | I2C_GIVE_BACK);
	word = sink_read(device, AMDRDNA_DCN_I2C_ARBITRATION, 0U);
	if (((word >> I2C_OWNER_SHIFT) & I2C_OWNER_FIELD) == I2C_OWNER_SOFTWARE)
		sink_write(device, AMDRDNA_DCN_I2C_ARBITRATION, 0U, word | I2C_GIVE_BACK);

	/* The pad as it was. */
	sink_write(device, AMDRDNA_DCN_DDC_PAD_MASK, line - 1U, device->sink.pad_word);
}

/* Waits for the I2C transfer to leave the busy state, and reports how it ended. */
static int
i2c_wait(
	struct amdrdna_device *device)
{
	uint32_t status;
	unsigned step;

	/* Polls the status while the engine works. */
	status = 0U;
	for (step = 0U; step < I2C_WAIT_STEPS; step++) {
		status = sink_read(device, AMDRDNA_DCN_I2C_STATUS, 0U);
		if ((status & I2C_STATE_FIELD) != I2C_STATE_BUSY)
			break;
		kern_usleep_range(I2C_WAIT_STEP_US, I2C_WAIT_STEP_US * 2U);
	}

	/* How it ended: still busy, no answer, a timeout, an abort. */
	if ((status & I2C_STATE_FIELD) == I2C_STATE_BUSY)
		return ETIMEDOUT;
	if ((status & I2C_NACK) != 0U)
		return ENXIO;
	if ((status & I2C_TIMED_OUT) != 0U)
		return ETIMEDOUT;
	if ((status & I2C_ABORTED) != 0U)
		return EIO;

	/* Succeeded: the transfer is done. */
	return 0;
}

/*
 * Reads count bytes (16 at most) of the DPCD at address over an AUX
 * channel (0 to 5) with one native read: the channel enabled and won, the
 * request's three header bytes and length, go, the reply's status byte,
 * then its bytes.
 *
 * Returns 0; EBUSY when the micro-controller has the channel; ENXIO when
 * the sink is unplugged; ETIMEDOUT when no reply came; EIO for a reply that
 * was bad or not an acknowledgement.
 *
 * Procedure referenced: Linux v6.19
 * drivers/gpu/drm/amd/display/dc/dce/dce_aux.c (acquire_engine,
 * submit_channel_request, get_channel_status, read_channel_reply,
 * release_engine).
 */
static int
aux_read(
	struct amdrdna_device *device,
	unsigned channel,
	uint32_t address,
	uint8_t *bytes,
	unsigned count)
{
	uint32_t word;
	uint32_t status;
	uint32_t replied;
	unsigned step;
	unsigned index;
	int error;

	/* A read one request carries. */
	if (channel >= AMDRDNA_DISPLAY_PIPES ||
	    count == 0U ||
	    count > AUX_BYTES_MAX)
		return EINVAL;

	/* The channel enabled, then won from the arbitration. */
	word = sink_read(device, AMDRDNA_DCN_AUX_ARBITRATION, channel);
	if (((word >> AUX_OWNER_SHIFT) & AUX_OWNER_FIELD) == AUX_OWNER_OTHER)
		return EBUSY;
	word = sink_read(device, AMDRDNA_DCN_AUX_CONTROL, channel);
	if ((word & AUX_ENABLE) == 0U)
		sink_write(device, AMDRDNA_DCN_AUX_CONTROL, channel, word | AUX_ENABLE);
	word = sink_read(device, AMDRDNA_DCN_AUX_ARBITRATION, channel);
	sink_write(device, AMDRDNA_DCN_AUX_ARBITRATION, channel, word | AUX_ASK);
	word = sink_read(device, AMDRDNA_DCN_AUX_ARBITRATION, channel);
	if (((word >> AUX_OWNER_SHIFT) & AUX_OWNER_FIELD) != AUX_OWNER_SOFTWARE) {
		error = EBUSY;
		goto give_back;
	}

	/* The last request's done acknowledged; four bytes to send (the header and the length), no delay. */
	word = sink_read(device, AMDRDNA_DCN_AUX_INTERRUPT_CONTROL, channel);
	sink_write(device, AMDRDNA_DCN_AUX_INTERRUPT_CONTROL, channel, word | AUX_DONE_ACK);
	word = sink_read(device, AMDRDNA_DCN_AUX_SEND_CONTROL, channel);
	word &= ~(AUX_DELAY_FIELD | AUX_SEND_FIELD);
	word |= 4U << AUX_SEND_SHIFT;
	sink_write(device, AMDRDNA_DCN_AUX_SEND_CONTROL, channel, word);

	/* The header from index 0 (the command and address bits 19:16, 15:8, 7:0), then the length less one. */
	sink_write(device, AMDRDNA_DCN_AUX_SEND_DATA, channel,
	    AUX_NO_STEP | ((AUX_NATIVE_READ | ((address >> 16) & 0xfU)) << AUX_DATA_SHIFT));
	sink_write(device, AMDRDNA_DCN_AUX_SEND_DATA, channel, ((address >> 8) & 0xffU) << AUX_DATA_SHIFT);
	sink_write(device, AMDRDNA_DCN_AUX_SEND_DATA, channel, (address & 0xffU) << AUX_DATA_SHIFT);
	sink_write(device, AMDRDNA_DCN_AUX_SEND_DATA, channel, (count - 1U) << AUX_DATA_SHIFT);

	/* Go, and the wait for done. */
	word = sink_read(device, AMDRDNA_DCN_AUX_SEND_CONTROL, channel);
	sink_write(device, AMDRDNA_DCN_AUX_SEND_CONTROL, channel, word | AUX_GO);
	status = 0U;
	for (step = 0U; step < AUX_WAIT_STEPS; step++) {
		status = sink_read(device, AMDRDNA_DCN_AUX_SEND_STATUS, channel);
		if ((status & AUX_DONE) != 0U)
			break;
		kern_usleep_range(AUX_WAIT_STEP_US, AUX_WAIT_STEP_US * 2U);
	}

	/* How it ended. */
	replied = (status >> AUX_REPLY_SHIFT) & AUX_REPLY_FIELD;
	error = 0;
	if ((status & AUX_DISCONNECTED) != 0U) {
		error = ENXIO;
	} else if ((status & AUX_DONE) == 0U || (status & AUX_TIMED_OUT) != 0U) {
		error = ETIMEDOUT;
	} else if ((status & AUX_BAD_REPLY) != 0U ||
		   replied == 0U ||
		   replied - 1U < count) {
		error = EIO;
	}

	/* The reply: its status byte must acknowledge; then the bytes. */
	if (error == 0) {
		sink_write(device, AMDRDNA_DCN_AUX_SEND_DATA, channel, AUX_NO_STEP | AUX_DATA_READ);
		word = sink_read(device, AMDRDNA_DCN_AUX_SEND_DATA, channel);
		if ((((word & AUX_DATA_FIELD) >> AUX_DATA_SHIFT) >> 4) != AUX_REPLY_ACK)
			error = EIO;
	}
	for (index = 0U; error == 0 && index < count; index++) {
		word = sink_read(device, AMDRDNA_DCN_AUX_SEND_DATA, channel);
		bytes[index] = (uint8_t)((word & AUX_DATA_FIELD) >> AUX_DATA_SHIFT);
	}

give_back:
	/* The channel given back. */
	word = sink_read(device, AMDRDNA_DCN_AUX_ARBITRATION, channel);
	word &= ~AUX_ASK;
	sink_write(device, AMDRDNA_DCN_AUX_ARBITRATION, channel, word | AUX_GIVE_BACK);
	if (error != 0)
		return error;

	/* Succeeded: the bytes are read. */
	return 0;
}

/* Names a connector kind for the log. */
static const char *
sink_connector_name(
	enum amdrdna_connector_kind kind)
{
	/* The kinds the driver tells apart. */
	switch (kind) {
	case AMDRDNA_CONNECTOR_DP:
		return "DP";
	case AMDRDNA_CONNECTOR_HDMI:
		return "HDMI";
	case AMDRDNA_CONNECTOR_DVI:
		return "DVI";
	default:
		break;
	}

	/* Succeeded: another kind. */
	return "other";
}

/* Reads a display register of the sink's (0 when the display block lacks it). */
static uint32_t
sink_read(
	struct amdrdna_device *device,
	enum amdrdna_dcn_register which,
	unsigned instance)
{
	uint32_t index;
	uint32_t value;

	/* Its index, then the read. */
	index = amdrdna_dcn_index(&device->blocks.blocks[AMDRDNA_BLOCK_DCN], which, instance);
	if (index == 0U)
		return 0U;
	value = amdrdna_read(device, index);

	/* Succeeded: the register's value. */
	return value;
}

/* Writes a display register of the sink's (nothing when the display block lacks it). */
static void
sink_write(
	struct amdrdna_device *device,
	enum amdrdna_dcn_register which,
	unsigned instance,
	uint32_t value)
{
	uint32_t index;

	/* Its index, then the write. */
	index = amdrdna_dcn_index(&device->blocks.blocks[AMDRDNA_BLOCK_DCN], which, instance);
	if (index == 0U)
		return;
	amdrdna_write(device, index, value);
}

/* A register read of the video BIOS's routines. */
static uint32_t
bios_read(
	void *owner,
	uint32_t index)
{
	struct sink_bios *bios;
	uint32_t value;

	/* The device's register. */
	bios = owner;
	value = amdrdna_read(bios->device, index);

	/* Succeeded: its value. */
	return value;
}

/* A register write of the video BIOS's routines. */
static void
bios_write(
	void *owner,
	uint32_t index,
	uint32_t value)
{
	struct sink_bios *bios;

	/* The device's register. */
	bios = owner;
	amdrdna_write(bios->device, index, value);
}

/* A wait of the video BIOS's routines. */
static void
bios_wait(
	void *owner,
	uint32_t microseconds)
{
	UNUSED_PARAMETER(owner);

	/* At least that long. */
	kern_usleep_range(microseconds, microseconds + microseconds / 8U + 1U);
}
