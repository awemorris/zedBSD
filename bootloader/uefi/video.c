/* -*- mode: c; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* Selects an explicitly requested GOP mode without changing boot record ABI. */
#include "video.h"
#include "bootloader/include/boot-parameter-handoff.h"

static int video_dimension(const char *text, size_t end, size_t *position, UINT32 *dimension);
static int video_parse(const char *text, size_t length, UINT32 *width, UINT32 *height);
static int video_mode_matches(const EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *info, UINTN size, UINT32 width, UINT32 height);

/*
 * Selects video=WIDTHxHEIGHT from an already assembled parameter record.
 * QueryMode buffers belong to firmware pool storage and are released before
 * SetMode. The caller must rebuild its framebuffer mapping after success.
 */
EFI_STATUS
zbl_uefi_video_select(
	EFI_BOOT_SERVICES *boot,
	EFI_GRAPHICS_OUTPUT_PROTOCOL *gop,
	const char *text,
	size_t length)
{
	EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *info;
	EFI_STATUS status;
	EFI_STATUS release_status;
	UINTN size;
	UINT32 width;
	UINT32 height;
	UINT32 index;
	UINT32 count;
	int parsed;
	int matches;

	/* Leaves the firmware mode alone unless the configuration requests one. */
	parsed = video_parse(text, length, &width, &height);
	if (parsed < 0)
		return EFI_INVALID_PARAMETER;

	if (parsed == 0)
		return EFI_SUCCESS;

	/* Refuses incomplete protocol tables and an unbounded firmware enumeration. */
	if (boot == 0 || gop == 0)
		return EFI_UNSUPPORTED;

	if (boot->FreePool == 0 || gop->QueryMode == 0 || gop->SetMode == 0 || gop->Mode == 0)
		return EFI_UNSUPPORTED;

	count = gop->Mode->MaxMode;
	if (count == 0 || count > 4096U)
		return EFI_UNSUPPORTED;

	/* Avoids clearing the display when the selected mode is already active. */
	matches = video_mode_matches(gop->Mode->Info, gop->Mode->SizeOfInfo, width, height);
	if (matches && gop->Mode->Mode < count)
		return EFI_SUCCESS;

	/* Visits each advertised mode, retaining no pool allocation across calls. */
	for (index = 0; index < count; index++) {
		info = 0;
		size = 0;
		status = gop->QueryMode(gop, index, &size, &info);
		if (EFI_ERROR(status))
			return status;

		/* Rejects a successful query that did not return an owned buffer. */
		if (info == 0)
			return EFI_DEVICE_ERROR;

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
	if (start == end || text[start] < '1' || text[start] > '9')
		return 0;

	/* Bounds arithmetic before multiplying, including arbitrarily long input. */
	pixels = 0;
	while (*position < end && text[*position] >= '0' && text[*position] <= '9') {
		digit = (UINT32)(text[*position] - '0');
		if (pixels > (16384U - digit) / 10U)
			return 0;

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

	if (text[length] != '\0')
		return -1;

	/* Inspects complete tokens so a path containing video= is never a setting. */
	position = 0;
	found = 0;
	while (position < length) {
		start = position;
		while (position < length && text[position] != ' ') {
			/* Rejects interior terminators and non-printable transport bytes. */
			if ((unsigned char)text[position] < 33 || (unsigned char)text[position] > 126)
				return -1;

			position++;
		}

		end = position;
		if (position < length)
			position++;

		/* Ignores unrelated tokens while rejecting duplicate video directives. */
		if (end - start < 6U)
			continue;

		if (text[start] != 'v' || text[start + 1] != 'i' || text[start + 2] != 'd' ||
		    text[start + 3] != 'e' || text[start + 4] != 'o' || text[start + 5] != '=')
			continue;

		if (found)
			return -1;

		/* Requires the entire value to be WIDTHxHEIGHT. */
		start += 6U;
		valid = video_dimension(text, end, &start, width);
		if (!valid || start == end || text[start] != 'x')
			return -1;

		start++;
		valid = video_dimension(text, end, &start, height);
		if (!valid || start != end)
			return -1;

		found = 1;
	}

	/* Succeeded: distinguishes absence from one valid explicit request. */
	return found;
}

/* Admits only the RGBX/BGRX linear formats understood by the kernel handoff. */
static int
video_mode_matches(
	const EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *info,
	UINTN size,
	UINT32 width,
	UINT32 height)
{
	/* Never reads beyond a short firmware information record. */
	if (info == 0 || size < sizeof(*info))
		return 0;

	if (info->HorizontalResolution != width || info->VerticalResolution != height)
		return 0;

	if (info->PixelsPerScanLine < width)
		return 0;

	if (info->PixelFormat != PixelRedGreenBlueReserved8BitPerColor &&
	    info->PixelFormat != PixelBlueGreenRedReserved8BitPerColor)
		return 0;

	/* Succeeded: the requested visible geometry has a supported pixel layout. */
	return 1;
}
