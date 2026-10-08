/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The tags of an m4a file (ws120-p008): what the MP4's movie box says of
 * the song.  The file's top-level boxes are walked with pread to find the
 * movie box (moov), which is read whole; in it, the movie header (mvhd)
 * gives the length, each track's handler (trak/mdia/hdlr) says whether it
 * is sound or pictures, and the iTunes-style list (udta/meta/ilst) gives
 * the title (the item ©nam), the artist (©ART), the album's artist (aART),
 * the album (©alb), the number on the album (trkn) and the cover (covr).
 * Each item holds its value in a data box: four bytes of version and type,
 * four of locale, then the value.
 */

#include "music.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* The largest movie box read (a cover is in it). */
#define TAGS_MOOV_MAX		(64U * 1024U * 1024U)

/* The size of a box's header, and of one with a 64-bit size. */
#define TAGS_HEADER		8U
#define TAGS_HEADER_LARGE	16U

/* The four-character codes of the boxes and items read. */
#define TAGS_MOOV		0x6d6f6f76U	/* "moov" */
#define TAGS_MVHD		0x6d766864U	/* "mvhd" */
#define TAGS_TRAK		0x7472616bU	/* "trak" */
#define TAGS_MDIA		0x6d646961U	/* "mdia" */
#define TAGS_HDLR		0x68646c72U	/* "hdlr" */
#define TAGS_UDTA		0x75647461U	/* "udta" */
#define TAGS_META		0x6d657461U	/* "meta" */
#define TAGS_ILST		0x696c7374U	/* "ilst" */
#define TAGS_DATA		0x64617461U	/* "data" */
#define TAGS_SOUN		0x736f756eU	/* "soun": a track of sound */
#define TAGS_VIDE		0x76696465U	/* "vide": a track of pictures */
#define TAGS_NAME		0xa96e616dU	/* "\251nam": the title */
#define TAGS_ARTIST		0xa9415254U	/* "\251ART": the artist */
#define TAGS_ALBUM_ARTIST	0x61415254U	/* "aART": the album's artist */
#define TAGS_ALBUM		0xa9616c62U	/* "\251alb": the album */
#define TAGS_TRACK		0x74726b6eU	/* "trkn": the number on the album */
#define TAGS_COVER		0x636f7672U	/* "covr": the cover */

/* The size of a data box's type and locale, before its value. */
#define TAGS_DATA_HEAD		8U

/* One box found in a buffer: its type and its payload (after the header). */
struct tags_box {
	uint32_t type;
	const unsigned char *payload;
	size_t size;
};

static int tags_read_moov(int fd, unsigned char **moov, size_t *size);
static int tags_next(const unsigned char *data, size_t size, size_t *offset, struct tags_box *box);
static int tags_child(const unsigned char *data, size_t size, uint32_t type, struct tags_box *box);
static void tags_movie_header(const struct tags_box *box, struct mu_tags *tags);
static void tags_track(const struct tags_box *box, struct mu_tags *tags);
static void tags_meta(const struct tags_box *box, struct mu_tags *tags);
static void tags_item(const struct tags_box *item, struct mu_tags *tags);
static void tags_text(const unsigned char *value, size_t size, char *text);
static uint32_t tags_be32(const unsigned char *bytes);
static uint64_t tags_be64(const unsigned char *bytes);

/*
 * Reads the tags of a file.  Returns 0 (tags holds what the file says;
 * what it does not say is empty, 0 or NULL), or an errno value: the file's
 * own, EINVAL for one that is not an MP4 with a movie box, EFBIG for a
 * movie box larger than read, ENOMEM.
 */
int
mu_tags_read(
	const char *path,
	struct mu_tags *tags)
{
	struct tags_box box;
	unsigned char *moov;
	size_t moov_size;
	size_t offset;
	int found;
	int error;
	int fd;

	/* Nothing known yet. */
	memset(tags, 0, sizeof(*tags));

	/* The movie box. */
	fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return errno;
	error = tags_read_moov(fd, &moov, &moov_size);
	(void)close(fd);
	if (error != 0)
		return error;

	/* Its boxes: the header, the tracks and the user data. */
	offset = 0;
	for (;;) {
		found = tags_next(moov, moov_size, &offset, &box);
		if (!found)
			break;
		if (box.type == TAGS_MVHD)
			tags_movie_header(&box, tags);
		else if (box.type == TAGS_TRAK)
			tags_track(&box, tags);
		else if (box.type == TAGS_UDTA)
			tags_meta(&box, tags);
	}

	/* Succeeded: the box is not needed after. */
	free(moov);
	return 0;
}

/*
 * Frees the cover the tags hold.
 */
void
mu_tags_release(
	struct mu_tags *tags)
{
	/* The cover's bytes. */
	free(tags->cover);
	tags->cover = NULL;
	tags->cover_size = 0;
}

/* Finds the file's top-level movie box and reads its payload; 0 or an errno value. */
static int
tags_read_moov(
	int fd,
	unsigned char **moov,
	size_t *size)
{
	unsigned char header[TAGS_HEADER_LARGE];
	struct stat status;
	uint64_t offset;
	uint64_t length;
	uint32_t type;
	size_t header_size;
	ssize_t got;
	int error;

	/* The file's size bounds the walk. */
	*moov = NULL;
	*size = 0;
	error = fstat(fd, &status);
	if (error != 0)
		return errno;

	/* Each top-level box, by its header. */
	offset = 0;
	while (offset + TAGS_HEADER <= (uint64_t)status.st_size) {
		/* The header: the size and the type, and a 64-bit size when the size is 1. */
		got = pread(fd, header, sizeof(header), (off_t)offset);
		if (got < (ssize_t)TAGS_HEADER)
			return EINVAL;
		length = tags_be32(header);
		type = tags_be32(header + 4);
		header_size = TAGS_HEADER;
		if (length == 1U) {
			if (got < (ssize_t)TAGS_HEADER_LARGE)
				return EINVAL;
			length = tags_be64(header + 8);
			header_size = TAGS_HEADER_LARGE;
		}

		/* A size of 0 reaches the end of the file. */
		if (length == 0U)
			length = (uint64_t)status.st_size - offset;

		/* A box smaller than its header, or past the end, ends the walk. */
		if (length < header_size || length > (uint64_t)status.st_size - offset)
			return EINVAL;

		/* Not the movie box: the next one. */
		if (type != TAGS_MOOV) {
			offset += length;
			continue;
		}

		/* The movie box's payload, whole. */
		if (length - header_size > TAGS_MOOV_MAX)
			return EFBIG;
		*size = (size_t)(length - header_size);
		*moov = malloc(*size + 1U);
		if (*moov == NULL)
			return ENOMEM;
		got = pread(fd, *moov, *size, (off_t)(offset + header_size));
		if (got != (ssize_t)*size) {
			free(*moov);
			*moov = NULL;
			return EINVAL;
		}

		/* Succeeded: the box is read. */
		return 0;
	}

	/* No movie box. */
	return EINVAL;
}

/*
 * Finds the box at an offset in a buffer and moves the offset past it.
 * Returns 1 for a box, 0 at the end or at a box that does not fit.
 */
static int
tags_next(
	const unsigned char *data,
	size_t size,
	size_t *offset,
	struct tags_box *box)
{
	uint64_t length;
	size_t header_size;
	size_t left;

	/* A header's room. */
	if (*offset > size)
		return 0;
	left = size - *offset;
	if (left < TAGS_HEADER)
		return 0;

	/* The size and the type; a 64-bit size when the size is 1. */
	length = tags_be32(data + *offset);
	box->type = tags_be32(data + *offset + 4);
	header_size = TAGS_HEADER;
	if (length == 1U) {
		if (left < TAGS_HEADER_LARGE)
			return 0;
		length = tags_be64(data + *offset + 8);
		header_size = TAGS_HEADER_LARGE;
	}

	/* A size of 0 reaches the end of the buffer. */
	if (length == 0U)
		length = left;

	/* The box within the buffer. */
	if (length < header_size || length > left)
		return 0;

	/* Found: its payload, and the offset past it. */
	box->payload = data + *offset + header_size;
	box->size = (size_t)length - header_size;
	*offset += (size_t)length;
	return 1;
}

/* Finds the first box of a type among the boxes of a buffer; 1 when found. */
static int
tags_child(
	const unsigned char *data,
	size_t size,
	uint32_t type,
	struct tags_box *box)
{
	size_t offset;
	int found;

	/* Each box, until the type. */
	offset = 0;
	for (;;) {
		found = tags_next(data, size, &offset, box);
		if (!found)
			return 0;
		if (box->type == type)
			return 1;
	}
}

/* Reads the length from the movie header: its time scale and duration (32 or 64 bits by its version). */
static void
tags_movie_header(
	const struct tags_box *box,
	struct mu_tags *tags)
{
	uint64_t duration;
	uint32_t scale;

	/* Version 1: 64-bit times, the scale at 20 and the duration at 24. */
	if (box->size >= 32U && box->payload[0] == 1U) {
		scale = tags_be32(box->payload + 20);
		duration = tags_be64(box->payload + 24);
	} else if (box->size >= 20U) {
		/* Version 0: 32-bit times, the scale at 12 and the duration at 16. */
		scale = tags_be32(box->payload + 12);
		duration = tags_be32(box->payload + 16);
	} else {
		return;
	}

	/* The length in milliseconds. */
	if (scale == 0U)
		return;
	tags->duration_ms = (int64_t)(duration / scale * 1000U + duration % scale * 1000U / scale);
}

/* Notes the kind of a track from its media's handler: sound or pictures. */
static void
tags_track(
	const struct tags_box *box,
	struct mu_tags *tags)
{
	struct tags_box media;
	struct tags_box handler;
	uint32_t kind;
	int found;

	/* The media box, then its handler. */
	found = tags_child(box->payload, box->size, TAGS_MDIA, &media);
	if (!found)
		return;
	found = tags_child(media.payload, media.size, TAGS_HDLR, &handler);
	if (!found || handler.size < 12U)
		return;

	/* The handler's type: after the version, the flags and four bytes of pre-definition. */
	kind = tags_be32(handler.payload + 8);
	if (kind == TAGS_SOUN)
		tags->has_sound = 1;
	else if (kind == TAGS_VIDE)
		tags->has_video = 1;
}

/* Reads the items of the user data's metadata list. */
static void
tags_meta(
	const struct tags_box *box,
	struct mu_tags *tags)
{
	struct tags_box meta;
	struct tags_box list;
	struct tags_box item;
	const unsigned char *children;
	size_t children_size;
	size_t offset;
	uint32_t version;
	int found;

	/* The metadata box. */
	found = tags_child(box->payload, box->size, TAGS_META, &meta);
	if (!found)
		return;

	/* A full box (four bytes of version and flags, all zero) before its boxes, or QuickTime's without them. */
	children = meta.payload;
	children_size = meta.size;
	version = 1;
	if (meta.size >= 4U)
		version = tags_be32(meta.payload);
	if (version == 0U) {
		children = meta.payload + 4;
		children_size = meta.size - 4U;
	}

	/* The list of items. */
	found = tags_child(children, children_size, TAGS_ILST, &list);
	if (!found)
		return;

	/* Each item. */
	offset = 0;
	for (;;) {
		found = tags_next(list.payload, list.size, &offset, &item);
		if (!found)
			break;
		tags_item(&item, tags);
	}
}

/* Reads one item of the list: its first data box's value, into the field the item's type names. */
static void
tags_item(
	const struct tags_box *item,
	struct mu_tags *tags)
{
	struct tags_box data;
	const unsigned char *value;
	size_t size;
	int found;

	/* The data box, and its value after the type and the locale. */
	found = tags_child(item->payload, item->size, TAGS_DATA, &data);
	if (!found || data.size < TAGS_DATA_HEAD)
		return;
	value = data.payload + TAGS_DATA_HEAD;
	size = data.size - TAGS_DATA_HEAD;

	/* The field. */
	switch (item->type) {
	case TAGS_NAME:
		tags_text(value, size, tags->title);
		break;
	case TAGS_ARTIST:
		tags_text(value, size, tags->artist);
		break;
	case TAGS_ALBUM_ARTIST:
		tags_text(value, size, tags->album_artist);
		break;
	case TAGS_ALBUM:
		tags_text(value, size, tags->album);
		break;
	case TAGS_TRACK:
		/* Two bytes of padding, the number, the count of the album's tracks. */
		if (size >= 4U)
			tags->track = (int)((unsigned)value[2] << 8 | (unsigned)value[3]);
		break;
	case TAGS_COVER:
		/* The first picture, when there is none yet (has_cover tells there is one, for a reader that does not keep it). */
		if (tags->cover != NULL || size == 0U || size > MU_COVER_MAX)
			break;
		tags->has_cover = 1;
		tags->cover = malloc(size);
		if (tags->cover == NULL)
			break;
		memcpy(tags->cover, value, size);
		tags->cover_size = size;
		break;
	default:
		break;
	}
}

/*
 * Copies a UTF-8 value into a text field: as much as fits, not cutting a
 * character, and stopping at a NUL or a control character.
 */
static void
tags_text(
	const unsigned char *value,
	size_t size,
	char *text)
{
	size_t length;

	/* The bytes up to the end, a NUL or a control character, within the field. */
	length = 0;
	while (length < size && length < MU_TEXT_MAX - 1U && value[length] >= 0x20U && value[length] != 0x7fU)
		length++;

	/* Not ending inside a character: the continuation bytes of a cut character go. */
	if (length < size && length == MU_TEXT_MAX - 1U) {
		while (length > 0U && (value[length] & 0xc0U) == 0x80U)
			length--;
	}

	/* The text. */
	memcpy(text, value, length);
	text[length] = '\0';
}

/* Reads a big-endian 32-bit number. */
static uint32_t
tags_be32(
	const unsigned char *bytes)
{
	uint32_t number;

	/* The most significant byte first. */
	number = (uint32_t)bytes[0] << 24;
	number |= (uint32_t)bytes[1] << 16;
	number |= (uint32_t)bytes[2] << 8;
	number |= (uint32_t)bytes[3];
	return number;
}

/* Reads a big-endian 64-bit number. */
static uint64_t
tags_be64(
	const unsigned char *bytes)
{
	uint64_t number;

	/* The high half, then the low half. */
	number = (uint64_t)tags_be32(bytes) << 32;
	number |= (uint64_t)tags_be32(bytes + 4);
	return number;
}
