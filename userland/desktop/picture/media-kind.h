/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

#ifndef KEILAND_MEDIA_KIND_H
#define KEILAND_MEDIA_KIND_H

#include <stddef.h>
#include <string.h>

/* Shared header classification keeps library imports and Phone drafts consistent. */
#define KL_MEDIA_NONE 0
#define KL_MEDIA_JPEG 1
#define KL_MEDIA_PNG 2
#define KL_MEDIA_GIF 3
#define KL_MEDIA_VIDEO 4

/* Classifies supported media headers without reading original contents through IPC. */
static inline int
kl_media_kind(
	const unsigned char *data,
	size_t size)
{
	int same;

	/* JPEG, PNG and GIF are the photo formats the desktop can decode. */
	if (size >= 3U && data[0] == 0xffU && data[1] == 0xd8U && data[2] == 0xffU)
		return KL_MEDIA_JPEG;
	if (size >= 8U) {
		same = memcmp(data, "\x89PNG\r\n\x1a\n", 8U);
		if (same == 0)
			return KL_MEDIA_PNG;
	}

	/* Either GIF header version is a readable photo container. */
	if (size >= 6U) {
		same = memcmp(data, "GIF8", 4U);
		if (same == 0 && (data[4] == '7' || data[4] == '9') && data[5] == 'a')
			return KL_MEDIA_GIF;
	}

	/* ISO base media brands must describe a video container, not HEIF/AVIF or audio. */
	if (size >= 12U) {
		same = memcmp(data + 4U, "ftyp", 4U);
		if (same == 0) {
			same = memcmp(data + 8U, "isom", 4U);
			if (same == 0)
				return KL_MEDIA_VIDEO;
			same = memcmp(data + 8U, "iso", 3U);
			if (same == 0 && data[11] >= '2' && data[11] <= '9')
				return KL_MEDIA_VIDEO;
			same = memcmp(data + 8U, "mp4", 3U);
			if (same == 0 && (data[11] == '1' || data[11] == '2'))
				return KL_MEDIA_VIDEO;
			same = memcmp(data + 8U, "qt  ", 4U);
			if (same == 0)
				return KL_MEDIA_VIDEO;
			same = memcmp(data + 8U, "M4V ", 4U);
			if (same == 0)
				return KL_MEDIA_VIDEO;
			same = memcmp(data + 8U, "3gp", 3U);
			if (same == 0)
				return KL_MEDIA_VIDEO;
			same = memcmp(data + 8U, "avc1", 4U);
			if (same == 0)
				return KL_MEDIA_VIDEO;
		}

		/* AVI has the RIFF video form tag, rather than the audio form tag. */
		same = memcmp(data, "RIFF", 4U);
		if (same == 0) {
			same = memcmp(data + 8U, "AVI ", 4U);
			if (same == 0)
				return KL_MEDIA_VIDEO;
		}
	}

	/* The EBML signature covers WebM and Matroska video containers. */
	if (size >= 4U && data[0] == 0x1aU && data[1] == 0x45U && data[2] == 0xdfU && data[3] == 0xa3U)
		return KL_MEDIA_VIDEO;

	/* Unrecognized contents cannot enter a photo/video draft or library. */
	return KL_MEDIA_NONE;
}

#endif
