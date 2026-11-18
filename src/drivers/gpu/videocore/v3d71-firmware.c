/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The Pi 5 GPU driver's questions to the firmware about the display.
 *
 * The Pi 5's firmware sets up a framebuffer only for a monitor whose EDID
 * it can read.  When the boot has no framebuffer, the report asks the
 * firmware, through its property mailbox, how many displays it set up and
 * whether it reads HDMI 0's EDID, and prints the display lines of the
 * firmware's own log.  It changes nothing.
 *
 * When there is a screen, the display start asks the firmware for the EDID
 * of the port it shows (the firmware owns the HDMI encoder and its DDC
 * bus), keeps the bytes for the display device, and logs what they say.
 *
 * The tag values are those of the Raspberry Pi firmware wiki's mailbox
 * property interface.
 */

#include <stdbool.h>
#include <stdint.h>

#include <drivers/generic/fdt.h>
#include <kern/kcrt.h>
#include <kern/klog.h>
#include <kern/kmem.h>
#include <kern/pmem.h>
#include <uapi/errno.h>

#include "drivers/gpu/display/edid.h"
#include "drivers/gpu/videocore/v3d71-private.h"
#include "drivers/platform/rpi5/rpi5-firmware.h"

/* The display questions of the firmware's property interface. */
#define SCREEN_TAG_DISPLAYS		0x00040013U
#define SCREEN_TAG_EDID			0x00030020U
#define SCREEN_TAG_DISPLAY_EDID		0x00030023U

/*
 * The firmware's display numbers of the HDMI ports, which the per-display
 * EDID tag takes.
 *
 * Values referenced: Raspberry Pi userland,
 * interface/vmcs_host/vc_dispmanx_types.h, DISPMANX_ID_HDMI0 and
 * DISPMANX_ID_HDMI1.
 */
#define SCREEN_DISPLAY_HDMI0		2U
#define SCREEN_DISPLAY_HDMI1		7U

/* The EDID answer's words: the block number, the status (or display number) and the 128 bytes. */
#define SCREEN_EDID_BLOCK_WORD		DRV_RPI5_FIRMWARE_WORD_VALUES
#define SCREEN_EDID_STATUS_WORD		(DRV_RPI5_FIRMWARE_WORD_VALUES + 1U)
#define SCREEN_EDID_DATA_WORD		(DRV_RPI5_FIRMWARE_WORD_VALUES + 2U)

/* The byte of an EDID base block that counts its extension blocks. */
#define SCREEN_EDID_EXTENSIONS		126U

/* The bit of a tag's code the firmware sets when it answered, and the answer's length under it. */
#define SCREEN_TAG_ANSWERED		0x80000000U
#define SCREEN_TAG_LENGTH		0x7fffffffU

/* The words of the display-count request, and of the EDID one (a block number, a status and 128 bytes). */
#define SCREEN_COUNT_WORDS		8U
#define SCREEN_EDID_WORDS		40U

/* The device tree's structure tokens. */
#define SCREEN_FDT_BEGIN_NODE		1U
#define SCREEN_FDT_END_NODE		2U
#define SCREEN_FDT_PROPERTY		3U
#define SCREEN_FDT_NOP			4U

/* The largest firmware log read, and how many of its display lines are printed. */
#define SCREEN_LOG_LIMIT		0x100000U
#define SCREEN_LOG_LINES		24U
#define SCREEN_LOG_LINE_BYTES		120U
#define SCREEN_LOG_SHORTEST		8U

static int screen_edid_block(uint32_t tag, uint32_t block, uint32_t display, uint8_t *bytes);
static int screen_edid_read(uint32_t port, uint32_t block, uint8_t *bytes);

/* Reads a big-endian word of the device tree blob. */
static uint32_t
screen_be32(
	const uint8_t *bytes)
{
	/* Succeeded: the word in the CPU's order. */
	return ((uint32_t)bytes[0] << 24) | ((uint32_t)bytes[1] << 16) | ((uint32_t)bytes[2] << 8) | bytes[3];
}

/* Whether a firmware log line is about the display: its HDMI ports, their EDID and the framebuffer. */
static bool
screen_line_wanted(
	const char *line)
{
	static const char *const words[] = { "HDMI", "EDID", "BSC_", "FB ", "isplay" };
	unsigned index;

	/* A line is wanted when any of the words is in it. */
	for (index = 0; index < sizeof(words) / sizeof(words[0]); index++)
		if (kern_strstr(line, words[index]) != NULL)
			return true;

	/* Not about the display. */
	return false;
}

/*
 * Finds where the firmware's own log lies: the two words of the tree's
 * /chosen/log property, its physical base and size.  Returns 0 when found.
 */
static int
screen_log_place(
	const struct drv_fdt *fdt,
	uint32_t *base,
	uint32_t *size)
{
	const uint8_t *structure;
	const char *name;
	uint32_t offset;
	uint32_t token;
	uint32_t length;
	unsigned depth;
	bool chosen;

	/* Walks the structure block, tracking whether the walk is directly inside /chosen. */
	structure = fdt->blob + fdt->structure_offset;
	offset = 0;
	depth = 0;
	chosen = false;
	while (offset + 4U <= fdt->structure_size) {
		token = screen_be32(structure + offset);
		offset += 4U;
		if (token == SCREEN_FDT_BEGIN_NODE) {
			/* A node: its name, padded to a word; /chosen is a child of the root. */
			name = (const char *)(structure + offset);
			length = (uint32_t)kern_strlen(name) + 1U;
			offset += (length + 3U) & ~3U;
			depth++;
			chosen = depth == 2U && kern_strcmp(name, "chosen") == 0;
		} else if (token == SCREEN_FDT_END_NODE) {
			/* The end of a node leaves any /chosen. */
			if (depth == 0)
				return ENOENT;
			depth--;
			chosen = false;
		} else if (token == SCREEN_FDT_PROPERTY) {
			/* A property: its length, its name's offset and its value. */
			if (offset + 8U > fdt->structure_size)
				return ENOENT;
			length = screen_be32(structure + offset);
			name = (const char *)(fdt->blob + fdt->strings_offset + screen_be32(structure + offset + 4U));
			if (chosen && length >= 8U && kern_strcmp(name, "log") == 0) {
				*base = screen_be32(structure + offset + 8U);
				*size = screen_be32(structure + offset + 12U);
				return 0;
			}
			offset += 8U + ((length + 3U) & ~3U);
		} else if (token != SCREEN_FDT_NOP) {
			/* The end of the structure, or a token the walk does not know. */
			return ENOENT;
		}
	}

	/* No log in the tree. */
	return ENOENT;
}

/* Prints the display lines of the firmware's own log: its printable runs that name the display. */
static void
screen_report_log(
	const struct drv_fdt *fdt)
{
	const volatile uint8_t *log;
	void *mapped;
	char line[SCREEN_LOG_LINE_BYTES];
	uint32_t base;
	uint32_t size;
	uint32_t index;
	unsigned length;
	unsigned printed;
	uint8_t byte;

	/* Maps the log the tree names; a missing or oversized one is skipped. */
	if (screen_log_place(fdt, &base, &size) != 0)
		return;
	if (base == 0 || size == 0 || size > SCREEN_LOG_LIMIT)
		return;
	if (kern_device_map(base, size, KERN_DEVICE_UNCACHED, &mapped) != 0)
		return;
	log = mapped;

	/* Splits the log into printable runs and prints the ones about the display. */
	length = 0;
	printed = 0;
	for (index = 0; index <= size && printed < SCREEN_LOG_LINES; index++) {
		byte = index < size ? log[index] : 0U;
		if (byte >= 0x20U && byte <= 0x7eU && length < sizeof(line) - 1U) {
			line[length++] = (char)byte;
			continue;
		}
		line[length] = 0;
		if (length >= SCREEN_LOG_SHORTEST && screen_line_wanted(line)) {
			kern_logf("rpi5-screen: firmware: %s\n", line);
			printed++;
		}
		length = 0;
	}
}

/*
 * Reports why the boot found no screen: how many displays the firmware
 * set up, whether it reads HDMI 0's EDID, and what its own log says about
 * the display (a monitor whose EDID cannot be read gets no display, and
 * so no framebuffer, from the Pi 5's firmware).  Changes nothing.
 */
void
bcm2712_screen_report(
	const struct drv_fdt *fdt)
{
	uint32_t words[SCREEN_EDID_WORDS];
	int count_error;
	int edid_error;
	uint32_t count;
	uint32_t edid_code;

	/* The number of displays the firmware set up. */
	kern_memset(words, 0, sizeof(words));
	words[DRV_RPI5_FIRMWARE_WORD_SIZE] = SCREEN_COUNT_WORDS * 4U;
	words[DRV_RPI5_FIRMWARE_WORD_TAG] = SCREEN_TAG_DISPLAYS;
	words[DRV_RPI5_FIRMWARE_WORD_CAPACITY] = 4U;
	count_error = drv_rpi5_firmware_message(words, SCREEN_COUNT_WORDS);
	count = words[DRV_RPI5_FIRMWARE_WORD_VALUES];
	if (words[DRV_RPI5_FIRMWARE_WORD_CODE] != DRV_RPI5_FIRMWARE_SUCCESS)
		count_error = EIO;

	/* HDMI 0's first EDID block, as the firmware reads it. */
	kern_memset(words, 0, sizeof(words));
	words[DRV_RPI5_FIRMWARE_WORD_SIZE] = SCREEN_EDID_WORDS * 4U;
	words[DRV_RPI5_FIRMWARE_WORD_TAG] = SCREEN_TAG_EDID;
	words[DRV_RPI5_FIRMWARE_WORD_CAPACITY] = (SCREEN_EDID_WORDS - DRV_RPI5_FIRMWARE_WORD_VALUES - 1U) * 4U;
	words[DRV_RPI5_FIRMWARE_WORD_TAG_CODE] = 4U;
	edid_error = drv_rpi5_firmware_message(words, SCREEN_EDID_WORDS);
	edid_code = words[DRV_RPI5_FIRMWARE_WORD_CODE];
	kern_logf("rpi5-screen: no firmware framebuffer; displays %u (%d), HDMI 0 EDID %s (%d, %x)\n",
		  count, count_error,
		  edid_error == 0 && edid_code == DRV_RPI5_FIRMWARE_SUCCESS && words[DRV_RPI5_FIRMWARE_WORD_VALUES + 1] == 0 ? "read" : "unreadable",
		  edid_error, edid_code);

	/* The firmware's own account of the display. */
	screen_report_log(fdt);
}

/*
 * Asks the firmware for one 128-byte EDID block of a display with one of
 * the EDID tags: the plain tag (HDMI 0, its answer's second word a status
 * that is 0 for a block read) or the per-display one (its second word the
 * display number).  Returns 0 with the block in bytes, EIO when the
 * firmware did not answer with one.
 */
static int
screen_edid_block(
	uint32_t tag,
	uint32_t block,
	uint32_t display,
	uint8_t *bytes)
{
	uint32_t words[SCREEN_EDID_WORDS];
	uint32_t answered;
	int error;

	/* The request: the block, and for the per-display tag the display. */
	kern_memset(words, 0, sizeof(words));
	words[DRV_RPI5_FIRMWARE_WORD_SIZE] = SCREEN_EDID_WORDS * 4U;
	words[DRV_RPI5_FIRMWARE_WORD_TAG] = tag;
	words[DRV_RPI5_FIRMWARE_WORD_CAPACITY] = (SCREEN_EDID_WORDS - DRV_RPI5_FIRMWARE_WORD_VALUES - 1U) * 4U;
	words[DRV_RPI5_FIRMWARE_WORD_TAG_CODE] = 4U;
	words[SCREEN_EDID_BLOCK_WORD] = block;
	if (tag == SCREEN_TAG_DISPLAY_EDID) {
		words[DRV_RPI5_FIRMWARE_WORD_TAG_CODE] = 8U;
		words[SCREEN_EDID_STATUS_WORD] = display;
	}

	/* Sends it; the message and the tag must both be answered, with the whole block. */
	error = drv_rpi5_firmware_message(words, SCREEN_EDID_WORDS);
	if (error != 0)
		return EIO;
	if (words[DRV_RPI5_FIRMWARE_WORD_CODE] != DRV_RPI5_FIRMWARE_SUCCESS)
		return EIO;
	if ((words[DRV_RPI5_FIRMWARE_WORD_TAG_CODE] & SCREEN_TAG_ANSWERED) == 0)
		return EIO;
	answered = words[DRV_RPI5_FIRMWARE_WORD_TAG_CODE] & SCREEN_TAG_LENGTH;
	if (answered < 8U + GPU_EDID_BLOCK_BYTES)
		return EIO;

	/* An answer about another block is not this one. */
	if (words[SCREEN_EDID_BLOCK_WORD] != block)
		return EIO;

	/* The plain tag says by its status whether the block was read. */
	if (tag == SCREEN_TAG_EDID && words[SCREEN_EDID_STATUS_WORD] != 0)
		return EIO;

	/* Copies the block out. */
	kern_memcpy(bytes, &words[SCREEN_EDID_DATA_WORD], GPU_EDID_BLOCK_BYTES);

	/* Succeeded: the block, as the firmware read it. */
	return 0;
}

/*
 * Reads one EDID block of an HDMI port: by the per-display tag, and for
 * HDMI 0 by the plain tag when the firmware does not know that one.
 */
static int
screen_edid_read(
	uint32_t port,
	uint32_t block,
	uint8_t *bytes)
{
	uint32_t display;
	int error;

	/* The port's display number. */
	display = SCREEN_DISPLAY_HDMI0;
	if (port != 0)
		display = SCREEN_DISPLAY_HDMI1;

	/* The per-display tag first; a block it gives is the answer. */
	error = screen_edid_block(SCREEN_TAG_DISPLAY_EDID, block, display, bytes);
	if (error == 0)
		return 0;

	/* Only HDMI 0 has the plain tag to fall back on. */
	if (port != 0)
		return error;
	error = screen_edid_block(SCREEN_TAG_EDID, block, 0, bytes);
	if (error != 0)
		return error;

	/* Succeeded: the block, by the plain tag. */
	return 0;
}

/*
 * Reads the EDID of the port the display shows from the firmware and keeps
 * it in the display record: the base block and as many of the extensions
 * it counts as the record holds, the whole blocks read before the first
 * the firmware would not give.  Bytes that do not decode as an EDID are not
 * kept.  Logs what the EDID says.  Thread context, before the display
 * device is published.
 */
void
bcm2712_screen_edid(
	struct bcm2711_display *display)
{
	struct gpu_edid *decoded;
	uint32_t blocks;
	uint32_t block;
	uint32_t wanted;
	int error;

	/* The driver asked: whatever is read below is what the device gives. */
	display->edid_kept = true;
	display->edid_bytes = 0;
	kern_memset(display->edid, 0, sizeof(display->edid));
	display->width_mm = 0;
	display->height_mm = 0;

	/* The base block. */
	error = screen_edid_read(display->port, 0, display->edid);
	if (error != 0) {
		kern_logf("hvs6: HDMI%u EDID unreadable (%d)\n", display->port, error);
		return;
	}

	/* The extensions the base block counts, as many as the record holds. */
	wanted = 1U + display->edid[SCREEN_EDID_EXTENSIONS];
	if (wanted > BCM2711_EDID_BYTES / GPU_EDID_BLOCK_BYTES)
		wanted = BCM2711_EDID_BYTES / GPU_EDID_BLOCK_BYTES;

	/* Reads each extension until one is refused. */
	blocks = 1U;
	for (block = 1U; block < wanted; block++) {
		error = screen_edid_read(display->port, block, display->edid + block * GPU_EDID_BLOCK_BYTES);
		if (error != 0)
			break;
		blocks++;
	}

	/* Decodes them; the decode refuses a base block without the header or with a bad checksum. */
	decoded = kern_calloc(1U, sizeof(*decoded));
	if (decoded == NULL) {
		kern_logf("hvs6: HDMI%u EDID not decoded (no memory)\n", display->port);
		return;
	}
	error = gpu_edid_decode(display->edid, blocks * GPU_EDID_BLOCK_BYTES, decoded);
	if (error != 0) {
		kern_logf("hvs6: HDMI%u EDID is not one (%d)\n", display->port, error);
		kern_memset(display->edid, 0, sizeof(display->edid));
		kern_free(decoded);
		return;
	}

	/* Keeps the blocks and the size, and says what they hold. */
	display->edid_bytes = blocks * GPU_EDID_BLOCK_BYTES;
	display->width_mm = decoded->width_mm;
	display->height_mm = decoded->height_mm;
	kern_logf("hvs6: HDMI%u EDID %u bytes of %u, %s %04x serial %08x \"%s\", %ux%u mm, %s, preferred %ux%u %u kHz, %u timings\n",
		  display->port, display->edid_bytes, wanted * GPU_EDID_BLOCK_BYTES, decoded->vendor, decoded->product,
		  decoded->serial, decoded->name, decoded->width_mm, decoded->height_mm, decoded->hdmi != 0 ? "HDMI" : "DVI",
		  decoded->preferred.h_active, decoded->preferred.v_active, decoded->preferred.pixel_khz, decoded->timing_count);
	kern_free(decoded);
}
