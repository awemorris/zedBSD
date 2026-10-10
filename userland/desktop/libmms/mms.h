/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* Shared, OS-independent MIME decoding and construction for MMS transport. */
#ifndef LIBMMS_MMS_H
#define LIBMMS_MMS_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

/* Bounds on the MIME wire body, total decoded media and displayed caption. */
#define MMS_INPUT_MAX (16U * 1024U * 1024U)
#define MMS_MEDIA_MAX (8U * 1024U * 1024U)
#define MMS_TEXT_MAX 16384U
#define MMS_MEDIA_COUNT 16U
#define MMS_TYPE_MAX 96U
#define MMS_NAME_MAX 256U

/* One independently allocated image/video payload and its MIME properties. */
struct mms_media {
	char type[MMS_TYPE_MAX];
	char name[MMS_NAME_MAX];
	uint8_t *data;
	size_t length;
};

/* A complete caption plus media leaves, owned until mms_release. */
struct mms_document {
	char text[MMS_TEXT_MAX + 1U];
	size_t text_length;
	int truncated;
	struct mms_media media[MMS_MEDIA_COUNT];
	size_t count;
	size_t bytes;
};

int mms_parse(const uint8_t *input, size_t length, struct mms_document *document);
void mms_release(struct mms_document *document);
int mms_extract_text(const uint8_t *input, size_t length, char *output, size_t size, size_t *used, int *truncated);
int mms_write(FILE *output, const char *type, const char *name, const uint8_t *data, size_t length, const char *caption);

#endif
