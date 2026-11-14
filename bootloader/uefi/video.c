/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Selects an explicitly requested GOP mode without changing boot record ABI.
 */

#include "video.h"
#include "bootloader/include/boot-parameter-handoff.h"

/*
 * The EDID base block: its size, the offset of its version, and the offset
 * and size of its first 18-byte descriptor, which holds the preferred timing.
 *
 * Values referenced: VESA Enhanced EDID Standard, Release A, Revision 2
 * (EDID 1.4), the base block's layout (header, version, descriptor block,
 * checksum).
 */
#define EDID_BLOCK_SIZE			128U
#define EDID_HEADER_SIZE		8U
#define EDID_VERSION_OFFSET		0x12U
#define EDID_PREFERRED_OFFSET		0x36U

/*
 * The fields of a detailed timing descriptor, relative to its start: the
 * pixel clock (0 marks a display descriptor instead), the low 8 bits of the
 * horizontal and vertical active sizes, the bytes whose upper nibbles carry
 * their bits 11 to 8, and the flags byte whose bit 7 marks interlace.
 *
 * Values referenced: VESA Enhanced EDID Standard, Release A, Revision 2
 * (EDID 1.4), the detailed timing descriptor's byte layout.
 */
#define EDID_TIMING_CLOCK_LOW		0U
#define EDID_TIMING_CLOCK_HIGH		1U
#define EDID_TIMING_HACTIVE_LOW		2U
#define EDID_TIMING_HACTIVE_HIGH	4U
#define EDID_TIMING_VACTIVE_LOW		5U
#define EDID_TIMING_VACTIVE_HIGH	7U
#define EDID_TIMING_FLAGS		17U
#define EDID_TIMING_INTERLACED		0x80U

/* The most handles one EDID protocol is looked for on. */
#define VIDEO_EDID_HANDLES_MAX		256U

/*
 * The fixed pattern every EDID base block starts with.
 *
 * Values referenced: VESA Enhanced EDID Standard, Release A, Revision 2
 * (EDID 1.4), the base block's header.
 */
static const uint8_t edid_header[EDID_HEADER_SIZE] = {
	0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00
};

static int video_dimension(const char *text, size_t end, size_t *position, UINT32 *dimension);
static int video_parse(const char *text, size_t length, UINT32 *width, UINT32 *height);
static int video_layout_usable(const EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *info, UINTN size);
static int video_mode_matches(const EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *info, UINTN size, UINT32 width, UINT32 height);
static EFI_STATUS video_set(EFI_BOOT_SERVICES *boot, EFI_GRAPHICS_OUTPUT_PROTOCOL *gop, UINT32 width, UINT32 height);
static EFI_STATUS video_fit(EFI_BOOT_SERVICES *boot, EFI_GRAPHICS_OUTPUT_PROTOCOL *gop, UINT32 monitor_width, UINT32 monitor_height);
static int video_edid_size(EFI_BOOT_SERVICES *boot, EFI_GRAPHICS_OUTPUT_PROTOCOL *gop, const EFI_GUID *guid, UINT32 *width, UINT32 *height);
static EFI_HANDLE video_edid_handle(EFI_BOOT_SERVICES *boot, EFI_GRAPHICS_OUTPUT_PROTOCOL *gop, EFI_HANDLE *handles, UINTN count);

/*
 * Selects video=WIDTHxHEIGHT from an already assembled parameter record.
 *
 * Without video=, the mode that best fits the monitor (monitor_width by
 * monitor_height, both nonzero, from its EDID) is set when the firmware has
 * one that fits; failing that, a preferred mode (preferred_width by
 * preferred_height, both nonzero, such as 1920x1080 for the boot splash) is
 * set when the firmware has it, and the current mode is kept when it has not
 * or when setting it fails.  The caller must rebuild its framebuffer mapping
 * after success.
 */
EFI_STATUS
zbl_uefi_video_select(
	EFI_BOOT_SERVICES *boot,
	EFI_GRAPHICS_OUTPUT_PROTOCOL *gop,
	const char *text,
	size_t length,
	UINT32 monitor_width,
	UINT32 monitor_height,
	UINT32 preferred_width,
	UINT32 preferred_height)
{
	UINT32 width;
	UINT32 height;
	EFI_STATUS status;
	int parsed;

	/* The configuration's request, when there is one, must be met. */
	parsed = video_parse(text, length, &width, &height);
	if (parsed < 0)
		return EFI_INVALID_PARAMETER;

	/* Sets the requested mode, which must exist. */
	if (parsed > 0) {
		status = video_set(boot, gop, width, height);
		if (EFI_ERROR(status))
			return status;

		return EFI_SUCCESS;
	}

	/* Else the monitor's own size, or the mode that fits it best. */
	if (monitor_width != 0U && monitor_height != 0U) {
		status = video_fit(boot, gop, monitor_width, monitor_height);
		if (!EFI_ERROR(status))
			return EFI_SUCCESS;
	}

	/* Else the preferred mode, which is only a wish: the current mode stays without it. */
	if (preferred_width != 0U && preferred_height != 0U)
		(void)video_set(boot, gop, preferred_width, preferred_height);

	/* Succeeded: whatever mode is active now is the caller's to validate. */
	return EFI_SUCCESS;
}

/*
 * Reads the size of the monitor a GOP drives from its EDID.
 *
 * The active EDID (after any platform override) is asked for first, then
 * the discovered one (as the monitor sent it).  The size is that of the
 * EDID's preferred timing, the monitor's native one.
 */
int
zbl_uefi_video_monitor_size(
	EFI_BOOT_SERVICES *boot,
	EFI_GRAPHICS_OUTPUT_PROTOCOL *gop,
	UINT32 *width,
	UINT32 *height)
{
	int found;

	/* Takes the EDID the firmware put in force. */
	found = video_edid_size(boot, gop, &EFI_EDID_ACTIVE_PROTOCOL_GUID, width, height);
	if (found)
		return 1;

	/* Else takes the EDID as the monitor sent it. */
	found = video_edid_size(boot, gop, &EFI_EDID_DISCOVERED_PROTOCOL_GUID, width, height);
	if (!found)
		return 0;

	/* Succeeded: the monitor's preferred size. */
	return 1;
}

/*
 * Decodes the active size of an EDID's preferred timing.
 *
 * The block must be an EDID 1.x base block with the fixed header and a
 * checksum that makes its 128 bytes sum to 0 modulo 256.  The preferred
 * timing is the first detailed timing descriptor; an interlaced one gives
 * the size of a field, not of the frame a GOP mode has, and is refused.
 *
 * Procedure referenced: VESA Enhanced EDID Standard, Release A, Revision 2
 * (EDID 1.4), the base block's checksum and the preferred timing mode.
 */
int
zbl_edid_preferred_size(
	const uint8_t *edid,
	size_t size,
	UINT32 *width,
	UINT32 *height)
{
	const uint8_t *timing;
	UINT32 clock;
	UINT32 horizontal;
	UINT32 vertical;
	unsigned sum;
	size_t index;

	/* Refuses an absent or short block. */
	if (edid == 0 || size < EDID_BLOCK_SIZE)
		return 0;

	/* Refuses a block without the EDID header. */
	for (index = 0; index < EDID_HEADER_SIZE; index++) {
		if (edid[index] != edid_header[index])
			return 0;
	}

	/* Refuses a block whose bytes do not sum to 0 modulo 256. */
	sum = 0;
	for (index = 0; index < EDID_BLOCK_SIZE; index++)
		sum += edid[index];
	if ((sum & 0xffU) != 0U)
		return 0;

	/* Refuses an EDID structure of a version other than 1. */
	if (edid[EDID_VERSION_OFFSET] != 1U)
		return 0;

	/* Refuses a first descriptor that is a display descriptor, not a timing. */
	timing = edid + EDID_PREFERRED_OFFSET;
	clock = (UINT32)timing[EDID_TIMING_CLOCK_LOW] |
		((UINT32)timing[EDID_TIMING_CLOCK_HIGH] << 8);
	if (clock == 0U)
		return 0;

	/* Refuses an interlaced timing, whose vertical size is a field's. */
	if ((timing[EDID_TIMING_FLAGS] & EDID_TIMING_INTERLACED) != 0U)
		return 0;

	/* Assembles the 12-bit active sizes from their low bytes and upper nibbles. */
	horizontal = (UINT32)timing[EDID_TIMING_HACTIVE_LOW] |
		     (((UINT32)timing[EDID_TIMING_HACTIVE_HIGH] & 0xf0U) << 4);
	vertical = (UINT32)timing[EDID_TIMING_VACTIVE_LOW] |
		   (((UINT32)timing[EDID_TIMING_VACTIVE_HIGH] & 0xf0U) << 4);

	/* Refuses an empty active area. */
	if (horizontal == 0U || vertical == 0U)
		return 0;

	/* Succeeded: the size of the preferred timing's active area. */
	*width = horizontal;
	*height = vertical;
	return 1;
}

/*
 * Sets the GOP mode that best fits a monitor of a size.
 *
 * The mode of the monitor's own size is taken when the firmware has it.
 * Otherwise the largest mode (by area) that fits inside the monitor's size
 * is taken, one of the monitor's aspect ratio winning a tie, so the picture
 * is never cut off and is scaled up as little as the firmware allows.
 * Returns EFI_UNSUPPORTED when no mode fits.
 */
static EFI_STATUS
video_fit(
	EFI_BOOT_SERVICES *boot,
	EFI_GRAPHICS_OUTPUT_PROTOCOL *gop,
	UINT32 monitor_width,
	UINT32 monitor_height)
{
	EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *info;
	EFI_STATUS status;
	EFI_STATUS release_status;
	UINTN size;
	UINT32 index;
	UINT32 count;
	UINT32 width;
	UINT32 height;
	UINT32 best_width;
	UINT32 best_height;
	UINT64 area;
	UINT64 best_area;
	int usable;
	int proportional;
	int best_proportional;

	/* Refuses incomplete protocol tables. */
	if (boot == 0 || gop == 0)
		return EFI_UNSUPPORTED;

	/* Refuses a protocol without the services the enumeration uses. */
	if (boot->FreePool == 0 ||
	    gop->QueryMode == 0 ||
	    gop->Mode == 0)
		return EFI_UNSUPPORTED;

	/* Refuses a firmware with no modes, or an unbounded number of them. */
	count = gop->Mode->MaxMode;
	if (count == 0 || count > 4096U)
		return EFI_UNSUPPORTED;

	/* Visits each advertised mode, keeping the best fit seen so far. */
	best_width = 0;
	best_height = 0;
	best_area = 0;
	best_proportional = 0;
	for (index = 0; index < count; index++) {
		/* Asks the firmware to describe the mode. */
		info = 0;
		size = 0;
		status = gop->QueryMode(gop, index, &size, &info);
		if (EFI_ERROR(status))
			return status;

		/* Rejects a successful query that did not return an owned buffer. */
		if (info == 0)
			return EFI_DEVICE_ERROR;

		/* Takes what the selection needs, then releases the firmware's description. */
		usable = video_layout_usable(info, size);
		width = 0;
		height = 0;
		if (usable) {
			width = info->HorizontalResolution;
			height = info->VerticalResolution;
		}

		release_status = boot->FreePool(info);
		if (EFI_ERROR(release_status))
			return release_status;

		/* Passes over a layout the kernel does not take and a mode larger than the monitor. */
		if (!usable)
			continue;
		if (width > monitor_width || height > monitor_height)
			continue;

		/* The monitor's own size is the answer. */
		if (width == monitor_width && height == monitor_height) {
			best_width = width;
			best_height = height;
			break;
		}

		/* A mode of the monitor's aspect ratio is scaled without distortion. */
		proportional = 0;
		if ((UINT64)width * monitor_height == (UINT64)height * monitor_width)
			proportional = 1;

		/* Keeps a larger mode, or one of equal area and the monitor's proportions. */
		area = (UINT64)width * height;
		if (area > best_area ||
		    (area == best_area &&
		     proportional &&
		     !best_proportional)) {
			best_width = width;
			best_height = height;
			best_area = area;
			best_proportional = proportional;
		}
	}

	/* Reports that no mode fits inside the monitor. */
	if (best_width == 0U || best_height == 0U)
		return EFI_UNSUPPORTED;

	/* Sets the chosen size. */
	status = video_set(boot, gop, best_width, best_height);
	if (EFI_ERROR(status))
		return status;

	/* Succeeded: the caller can validate the new physical mapping. */
	return EFI_SUCCESS;
}

/*
 * Reads the preferred size from the EDID one protocol carries for a GOP.
 * Returns 1 with the size, or 0 when there is none to read.
 */
static int
video_edid_size(
	EFI_BOOT_SERVICES *boot,
	EFI_GRAPHICS_OUTPUT_PROTOCOL *gop,
	const EFI_GUID *guid,
	UINT32 *width,
	UINT32 *height)
{
	EFI_EDID_PROTOCOL *edid;
	EFI_HANDLE *handles;
	EFI_HANDLE chosen;
	EFI_STATUS status;
	EFI_STATUS release_status;
	UINTN count;
	int found;

	/* Refuses incomplete protocol tables. */
	if (boot == 0 || gop == 0 || width == 0 || height == 0)
		return 0;

	/* Refuses a firmware without the services the search uses. */
	if (boot->LocateHandleBuffer == 0 ||
	    boot->HandleProtocol == 0 ||
	    boot->FreePool == 0)
		return 0;

	/* Lists the handles that carry the protocol. */
	handles = 0;
	count = 0;
	status = boot->LocateHandleBuffer(ByProtocol, guid, 0, &count, &handles);
	if (EFI_ERROR(status) || handles == 0)
		return 0;

	/* Picks the handle whose EDID describes this GOP's monitor. */
	chosen = video_edid_handle(boot, gop, handles, count);

	/* Asks that handle for its EDID. */
	edid = 0;
	if (chosen != 0) {
		status = boot->HandleProtocol(chosen, guid, (void **)&edid);
		if (EFI_ERROR(status))
			edid = 0;
	}

	/* Releases the firmware's list of handles; the EDID is not in it. */
	release_status = boot->FreePool(handles);
	if (EFI_ERROR(release_status))
		return 0;

	/* Refuses an absent or empty EDID. */
	if (edid == 0 || edid->Edid == 0 || edid->SizeOfEdid == 0U)
		return 0;

	/* Decodes the preferred timing's size. */
	found = zbl_edid_preferred_size(edid->Edid, edid->SizeOfEdid, width, height);
	if (!found)
		return 0;

	/* Succeeded: the monitor's preferred size. */
	return 1;
}

/*
 * Picks, among the handles that carry an EDID protocol, the one of a GOP.
 *
 * The protocol is installed on the handle of the GOP whose monitor it
 * describes.  The GOP the loader draws on may instead be the console's,
 * which spans every output and carries no EDID; with one EDID in the
 * system it still describes the only monitor, so that one is taken.  With
 * several and none on this GOP's handle, the monitor is unknown.
 */
static EFI_HANDLE
video_edid_handle(
	EFI_BOOT_SERVICES *boot,
	EFI_GRAPHICS_OUTPUT_PROTOCOL *gop,
	EFI_HANDLE *handles,
	UINTN count)
{
	EFI_GRAPHICS_OUTPUT_PROTOCOL *candidate;
	EFI_STATUS status;
	UINTN index;

	/* Refuses an empty or unbounded list. */
	if (count == 0 || count > VIDEO_EDID_HANDLES_MAX)
		return 0;

	/* The one EDID in the system is the one monitor's. */
	if (count == 1)
		return handles[0];

	/* Looks for the handle that carries this very GOP. */
	for (index = 0; index < count; index++) {
		/* Asks the handle for its GOP. */
		candidate = 0;
		status = boot->HandleProtocol(handles[index], &EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID, (void **)&candidate);
		if (EFI_ERROR(status))
			continue;

		/* The handle is this GOP's. */
		if (candidate == gop)
			return handles[index];
	}

	/* No handle is this GOP's: the monitor is not known. */
	return 0;
}

/*
 * Sets the GOP mode of a size with a supported pixel layout, keeping the
 * current mode when it already is one.  QueryMode buffers belong to
 * firmware pool storage and are released before SetMode.  Returns
 * EFI_UNSUPPORTED when the firmware has no such mode.
 */
static EFI_STATUS
video_set(
	EFI_BOOT_SERVICES *boot,
	EFI_GRAPHICS_OUTPUT_PROTOCOL *gop,
	UINT32 width,
	UINT32 height)
{
	EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *info;
	EFI_STATUS status;
	EFI_STATUS release_status;
	UINTN size;
	UINT32 index;
	UINT32 count;
	int matches;

	/* Refuses incomplete protocol tables and an unbounded firmware enumeration. */
	if (boot == 0 || gop == 0)
		return EFI_UNSUPPORTED;

	/* Refuses a protocol without the services the selection uses. */
	if (boot->FreePool == 0 ||
	    gop->QueryMode == 0 ||
	    gop->SetMode == 0 ||
	    gop->Mode == 0)
		return EFI_UNSUPPORTED;

	/* Refuses a firmware with no modes, or an unbounded number of them. */
	count = gop->Mode->MaxMode;
	if (count == 0 || count > 4096U)
		return EFI_UNSUPPORTED;

	/* Avoids clearing the display when the selected mode is already active. */
	matches = video_mode_matches(gop->Mode->Info, gop->Mode->SizeOfInfo, width, height);
	if (matches && gop->Mode->Mode < count)
		return EFI_SUCCESS;

	/* Visits each advertised mode, retaining no pool allocation across calls. */
	for (index = 0; index < count; index++) {
		/* Asks the firmware to describe the mode. */
		info = 0;
		size = 0;
		status = gop->QueryMode(gop, index, &size, &info);
		if (EFI_ERROR(status))
			return status;

		/* Rejects a successful query that did not return an owned buffer. */
		if (info == 0)
			return EFI_DEVICE_ERROR;

		/* Checks the mode, then releases the firmware's description of it. */
		matches = video_mode_matches(info, size, width, height);
		release_status = boot->FreePool(info);
		if (EFI_ERROR(release_status))
			return release_status;

		/* Changes only to a mode whose layout the linear framebuffer supports. */
		if (matches) {
			status = gop->SetMode(gop, index);
			if (EFI_ERROR(status))
				return status;

			/* Firmware must actually publish the requested active dimensions. */
			if (gop->Mode == 0)
				return EFI_DEVICE_ERROR;

			/* The active mode is the one asked for. */
			matches = video_mode_matches(gop->Mode->Info, gop->Mode->SizeOfInfo, width, height);
			if (!matches || gop->Mode->Mode != index)
				return EFI_DEVICE_ERROR;

			/* Succeeded: the caller can validate the new physical mapping. */
			return EFI_SUCCESS;
		}
	}

	/* Reports that no compatible mode supplies the requested geometry. */
	return EFI_UNSUPPORTED;
}

/* Consumes one bounded decimal dimension without accepting signs or padding. */
static int
video_dimension(
	const char *text,
	size_t end,
	size_t *position,
	UINT32 *dimension)
{
	size_t start;
	UINT32 pixels;
	UINT32 digit;

	/* Requires a canonical positive decimal number. */
	start = *position;
	if (start == end ||
	    text[start] < '1' ||
	    text[start] > '9')
		return 0;

	/* Bounds arithmetic before multiplying, including arbitrarily long input. */
	pixels = 0;
	while (*position < end && text[*position] >= '0' && text[*position] <= '9') {
		/* Refuses a dimension beyond 16384 pixels. */
		digit = (UINT32)(text[*position] - '0');
		if (pixels > (16384U - digit) / 10U)
			return 0;

		/* Takes the digit and moves past it. */
		pixels = pixels * 10U + digit;
		(*position)++;
	}

	/* Succeeded: the bounded positive dimension belongs to this token. */
	*dimension = pixels;
	return 1;
}

/* Parses exactly one optional video token from bounded, space-delimited text. */
static int
video_parse(
	const char *text,
	size_t length,
	UINT32 *width,
	UINT32 *height)
{
	size_t position;
	size_t start;
	size_t end;
	int found;
	int valid;

	/* Requires the transport's bounded NUL-terminated text contract. */
	if (text == 0 || length > KERN_BOOT_PARAMETERS_TEXT_MAX)
		return -1;

	/* Refuses a text that is not terminated where its length says. */
	if (text[length] != '\0')
		return -1;

	/* Inspects complete tokens so a path containing video= is never a setting. */
	position = 0;
	found = 0;
	while (position < length) {
		/* Finds the end of the token. */
		start = position;
		while (position < length && text[position] != ' ') {
			/* Rejects interior terminators and non-printable transport bytes. */
			if ((unsigned char)text[position] < 33 || (unsigned char)text[position] > 126)
				return -1;

			position++;
		}

		/* Moves past the separating space. */
		end = position;
		if (position < length)
			position++;

		/* Ignores unrelated tokens while rejecting duplicate video directives. */
		if (end - start < 6U)
			continue;

		/* A token that is not video= belongs to someone else. */
		if (text[start] != 'v' ||
		    text[start + 1] != 'i' ||
		    text[start + 2] != 'd' ||
		    text[start + 3] != 'e' ||
		    text[start + 4] != 'o' ||
		    text[start + 5] != '=')
			continue;

		/* Refuses a second video= token. */
		if (found)
			return -1;

		/* Requires the entire value to be WIDTHxHEIGHT. */
		start += 6U;
		valid = video_dimension(text, end, &start, width);
		if (!valid ||
		    start == end ||
		    text[start] != 'x')
			return -1;

		/* The height follows the 'x' and ends the token. */
		start++;
		valid = video_dimension(text, end, &start, height);
		if (!valid || start != end)
			return -1;

		/* The token is the one request. */
		found = 1;
	}

	/* Succeeded: distinguishes absence from one valid explicit request. */
	return found;
}

/* Admits only nonempty modes in the RGBX/BGRX linear formats the kernel handoff takes. */
static int
video_layout_usable(
	const EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *info,
	UINTN size)
{
	/* Never reads beyond a short firmware information record. */
	if (info == 0 || size < sizeof(*info))
		return 0;

	/* The mode is empty. */
	if (info->HorizontalResolution == 0U || info->VerticalResolution == 0U)
		return 0;

	/* The mode's rows are narrower than its width. */
	if (info->PixelsPerScanLine < info->HorizontalResolution)
		return 0;

	/* The mode's pixel layout is not one the kernel takes. */
	if (info->PixelFormat != PixelRedGreenBlueReserved8BitPerColor &&
	    info->PixelFormat != PixelBlueGreenRedReserved8BitPerColor)
		return 0;

	/* Succeeded: the kernel can draw on the mode. */
	return 1;
}

/* Admits a mode of a size in a layout the kernel handoff takes. */
static int
video_mode_matches(
	const EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *info,
	UINTN size,
	UINT32 width,
	UINT32 height)
{
	int usable;

	/* The mode's layout is one the kernel cannot take. */
	usable = video_layout_usable(info, size);
	if (!usable)
		return 0;

	/* The mode is of another size. */
	if (info->HorizontalResolution != width || info->VerticalResolution != height)
		return 0;

	/* Succeeded: the requested visible geometry has a supported pixel layout. */
	return 1;
}
