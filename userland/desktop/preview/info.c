/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * What a media file is (ws157-p013; preview.h --info): the media library's
 * mediastorage does not read the contents of the photos and videos it
 * keeps (the user's decision of 2026-10-11); it asks keiland-preview, in
 * its sandbox, for this.  The first PREVIEW_INFO_MAX bytes of the input
 * are read (from its start, whatever its offset), its kind told by its
 * first bytes (picture/media-kind.h), a picture's size read from its
 * header (PNG's IHDR, GIF's screen, JPEG's first frame header), and a
 * JPEG's date from its EXIF (photos/exif.c).  A video's size and date come
 * from its header too (ws157-p016): an MP4 or QuickTime file's movie box
 * (moov: mvhd's creation time, the video track's tkhd and its rotation),
 * found by walking the boxes with pread wherever in the file it is; an
 * AVI's main header (avih).  Nothing is decoded.
 *
 * The output is text, one field a line:
 *
 *   keiland-preview-info 1
 *   kind=jpeg|png|gif|video|none
 *   width=N
 *   height=N
 *   taken=N          (seconds of the camera's calendar as ph_time has them; empty when unknown)
 *   zone=utc         (only when taken is UTC, as an MP4's is: the caller makes it local)
 */

#include "preview.h"

#include "userland/desktop/photos/photos.h"
#include "userland/desktop/picture/media-kind.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* The JPEG markers: a frame header's range (less DHT, JPG and DAC), APP1, start of scan, end of image. */
#define INFO_SOF_FIRST		0xc0U
#define INFO_SOF_LAST		0xcfU
#define INFO_DHT		0xc4U
#define INFO_JPG		0xc8U
#define INFO_DAC		0xccU
#define INFO_APP1		0xe1U
#define INFO_SOS		0xdaU
#define INFO_EOI		0xd9U

/* The longest output. */
#define INFO_TEXT_MAX		256U

/* ISO base media: the most boxes walked, a box header's bytes, the seconds from 1904 to 1970. */
#define INFO_BOX_MAX		512U
#define INFO_BOX_HEADER		16U
#define INFO_MP4_EPOCH		2082844800ULL

/* The largest side a video may say it has. */
#define INFO_SIDE_MAX		65535L

/* What a video's header says. */
struct info_video {
	long width;
	long height;
	int dated;
	ph_time taken;
	unsigned boxes;
};

static int info_read(int input, unsigned char **data, size_t *size);
static void info_png(const unsigned char *data, size_t size, long *width, long *height);
static void info_gif(const unsigned char *data, size_t size, long *width, long *height);
static void info_jpeg(const unsigned char *data, size_t size, long *width, long *height, int *dated, ph_time *taken);
static int info_write(int output, const char *text, size_t length);
static void info_video(int input, const unsigned char *data, size_t size, struct info_video *video);
static void info_mp4(int input, struct info_video *video);
static int info_box(int input, uint64_t offset, uint64_t end, uint64_t *body, uint64_t *next, char type[4]);
static int info_pread(int input, uint64_t offset, unsigned char *data, size_t size);
static void info_mvhd(int input, uint64_t body, uint64_t end, struct info_video *video);
static void info_trak(int input, uint64_t body, uint64_t end, struct info_video *video);
static int info_tkhd(int input, uint64_t body, uint64_t end, long *width, long *height);
static int info_video_track(int input, uint64_t body, uint64_t end, struct info_video *video);
static void info_avi(const unsigned char *data, size_t size, long *width, long *height);
static uint32_t info_be32(const unsigned char *data);
static uint32_t info_le32(const unsigned char *data);

/*
 * Tells what the input is, into the output.  Returns the exit status
 * (PREVIEW_OK even for a file of no kind: kind=none says so).
 */
int
preview_info(
	int input,
	int output)
{
	struct info_video video;
	char text[INFO_TEXT_MAX];
	char date[32];
	const char *zone;
	unsigned char *data;
	const char *name;
	ph_time taken;
	size_t size;
	long width;
	long height;
	int dated;
	int kind;
	int length;
	int error;

	/* The input's start. */
	error = info_read(input, &data, &size);
	if (error == ENOMEM)
		return PREVIEW_NO_MEMORY;
	if (error != 0)
		return PREVIEW_DAMAGED;

	/* Its kind, and what its header says. */
	width = 0L;
	height = 0L;
	dated = 0;
	taken = 0;
	zone = "";
	kind = kl_media_kind(data, size);
	switch (kind) {
	case KL_MEDIA_JPEG:
		name = "jpeg";
		info_jpeg(data, size, &width, &height, &dated, &taken);
		break;
	case KL_MEDIA_PNG:
		name = "png";
		info_png(data, size, &width, &height);
		break;
	case KL_MEDIA_GIF:
		name = "gif";
		info_gif(data, size, &width, &height);
		break;
	case KL_MEDIA_VIDEO:
		name = "video";
		info_video(input, data, size, &video);
		width = video.width;
		height = video.height;
		dated = video.dated;
		taken = video.taken;
		if (dated)
			zone = "zone=utc\n";
		break;
	default:
		name = "none";
		break;
	}

	/* The bytes are no longer needed. */
	free(data);

	/* The date, when there is one. */
	date[0] = '\0';
	if (dated)
		(void)snprintf(date, sizeof(date), "%lld", (long long)taken);

	/* The lines. */
	length = snprintf(text, sizeof(text), "keiland-preview-info 1\nkind=%s\nwidth=%ld\nheight=%ld\ntaken=%s\n%s", name, width, height, date, zone);
	if (length < 0 || (size_t)length >= sizeof(text))
		return PREVIEW_NO_OUTPUT;
	error = info_write(output, text, (size_t)length);
	if (error != 0)
		return PREVIEW_NO_OUTPUT;

	/* Succeeded: told. */
	return PREVIEW_OK;
}

/* Reads up to PREVIEW_INFO_MAX bytes from the input's start; 0, ENOMEM or EIO. */
static int
info_read(
	int input,
	unsigned char **data,
	size_t *size)
{
	unsigned char *buffer;
	ssize_t got;
	size_t length;

	/* The room. */
	*data = NULL;
	*size = 0U;
	buffer = malloc(PREVIEW_INFO_MAX);
	if (buffer == NULL)
		return ENOMEM;

	/* Read from the start until the room is full or the file ends. */
	length = 0U;
	while (length < PREVIEW_INFO_MAX) {
		got = pread(input, buffer + length, PREVIEW_INFO_MAX - length, (off_t)length);
		if (got < 0 && errno == EINTR)
			continue;
		if (got < 0) {
			free(buffer);
			return EIO;
		}

		/* The end. */
		if (got == 0)
			break;
		length += (size_t)got;
	}

	/* Succeeded: the start of the file. */
	*data = buffer;
	*size = length;
	return 0;
}

/* Reads a PNG's size from its IHDR (the first chunk, at byte 8). */
static void
info_png(
	const unsigned char *data,
	size_t size,
	long *width,
	long *height)
{
	unsigned long across;
	unsigned long down;
	int same;

	/* The IHDR's fields, big-endian, where the format puts them. */
	if (size < 24U)
		return;
	same = memcmp(data + 12, "IHDR", 4U);
	if (same != 0)
		return;
	across = ((unsigned long)data[16] << 24) | ((unsigned long)data[17] << 16) | ((unsigned long)data[18] << 8) | data[19];
	down = ((unsigned long)data[20] << 24) | ((unsigned long)data[21] << 16) | ((unsigned long)data[22] << 8) | data[23];

	/* A sane size only (PNG's limit is 2^31 - 1). */
	if (across > 0x7fffffffUL || down > 0x7fffffffUL)
		return;
	*width = (long)across;
	*height = (long)down;
}

/* Reads a GIF's size from its logical screen (bytes 6 to 9, little-endian). */
static void
info_gif(
	const unsigned char *data,
	size_t size,
	long *width,
	long *height)
{
	/* The screen's width and height. */
	if (size < 10U)
		return;
	*width = (long)((unsigned)data[6] | ((unsigned)data[7] << 8));
	*height = (long)((unsigned)data[8] | ((unsigned)data[9] << 8));
}

/*
 * Walks a JPEG's marker segments up to the first scan: the first frame
 * header gives the size, an APP1 with EXIF the date.
 */
static void
info_jpeg(
	const unsigned char *data,
	size_t size,
	long *width,
	long *height,
	int *dated,
	ph_time *taken)
{
	size_t offset;
	size_t length;
	unsigned marker;
	int error;
	int frame;

	/* After the start of image, each segment until the scan or the end. */
	offset = 2U;
	frame = 0;
	while (offset + 4U <= size) {
		/* A marker, its fill bytes skipped. */
		if (data[offset] != 0xffU)
			return;
		marker = data[offset + 1U];
		if (marker == 0xffU) {
			offset++;
			continue;
		}

		/* The scan or the end: nothing more in the header. */
		if (marker == INFO_SOS || marker == INFO_EOI)
			return;

		/* Markers without a length. */
		if (marker == 0x01U || (marker >= 0xd0U && marker <= 0xd8U)) {
			offset += 2U;
			continue;
		}

		/* The segment's length, its two bytes included, within the data. */
		length = ((size_t)data[offset + 2U] << 8) | data[offset + 3U];
		if (length < 2U || offset + 2U + length > size)
			return;

		/* The first frame header: precision, height, width. */
		if (!frame &&
		    marker >= INFO_SOF_FIRST &&
		    marker <= INFO_SOF_LAST &&
		    marker != INFO_DHT &&
		    marker != INFO_JPG &&
		    marker != INFO_DAC &&
		    length >= 7U) {
			*height = (long)(((unsigned)data[offset + 5U] << 8) | data[offset + 6U]);
			*width = (long)(((unsigned)data[offset + 7U] << 8) | data[offset + 8U]);
			frame = 1;
		}

		/* An APP1 holding EXIF: the date (a block without a readable one is passed by). */
		if (marker == INFO_APP1 && !*dated) {
			error = ph_exif_date(data + offset + 4U, length - 2U, taken);
			if (error == 0)
				*dated = 1;
		}

		/* The next segment. */
		offset += 2U + length;
	}
}

/* Writes all the text with write(2) (no stdio on the output). */
static int
info_write(
	int output,
	const char *text,
	size_t length)
{
	ssize_t wrote;
	size_t done;

	/* Until it is all written. */
	done = 0U;
	while (done < length) {
		wrote = write(output, text + done, length - done);
		if (wrote < 0 && errno == EINTR)
			continue;
		if (wrote <= 0)
			return EIO;
		done += (size_t)wrote;
	}

	/* Succeeded: written. */
	return 0;
}

/*
 * Reads a video's header: an AVI's from the bytes read, an MP4's or a
 * QuickTime file's from its boxes (its date is UTC).
 */
static void
info_video(
	int input,
	const unsigned char *data,
	size_t size,
	struct info_video *video)
{
	int riff;

	/* Nothing known yet. */
	memset(video, 0, sizeof(*video));

	/* An AVI. */
	riff = 1;
	if (size >= 12U)
		riff = memcmp(data, "RIFF", 4U);
	if (riff == 0) {
		info_avi(data, size, &video->width, &video->height);
		return;
	}

	/* An MP4 or QuickTime file. */
	info_mp4(input, video);
}

/*
 * Walks an MP4's top-level boxes to its movie box and reads it: mvhd for
 * the date, each trak for the video track's size.  A file cut short or
 * malformed leaves what was found.
 */
static void
info_mp4(
	int input,
	struct info_video *video)
{
	uint64_t offset;
	uint64_t body;
	uint64_t next;
	uint64_t child;
	char type[4];
	int error;
	int same;

	/* Each top-level box, until moov. */
	offset = 0U;
	for (;;) {
		error = info_box(input, offset, UINT64_MAX, &body, &next, type);
		if (error != 0 || video->boxes++ >= INFO_BOX_MAX)
			return;
		same = memcmp(type, "moov", 4U);
		if (same == 0)
			break;
		offset = next;
	}

	/* The movie box's children: the header and the tracks. */
	for (child = body; child < next;) {
		error = info_box(input, child, next, &body, &offset, type);
		if (error != 0 || video->boxes++ >= INFO_BOX_MAX)
			return;
		same = memcmp(type, "mvhd", 4U);
		if (same == 0)
			info_mvhd(input, body, offset, video);
		same = memcmp(type, "trak", 4U);
		if (same == 0 && video->width == 0L)
			info_trak(input, body, offset, video);
		child = offset;
	}
}

/*
 * Reads the box header at offset, which must end by end: its body's start,
 * the next box's offset and its type.  0, or EINVAL for a box that does not
 * fit or cannot be read.
 */
static int
info_box(
	int input,
	uint64_t offset,
	uint64_t end,
	uint64_t *body,
	uint64_t *next,
	char type[4])
{
	unsigned char header[INFO_BOX_HEADER];
	uint64_t size;
	uint64_t length;
	int error;

	/* The size and the type. */
	error = info_pread(input, offset, header, 8U);
	if (error != 0)
		return error;
	memcpy(type, header + 4, 4U);
	size = info_be32(header);
	length = 8U;

	/* A 64-bit size follows; 0 runs to the file's end (only the last box). */
	if (size == 1U) {
		error = info_pread(input, offset + 8U, header + 8, 8U);
		if (error != 0)
			return error;
		size = ((uint64_t)info_be32(header + 8) << 32) | info_be32(header + 12);
		length = 16U;
	} else if (size == 0U) {
		size = end - offset;
	}

	/* Within its parent, past its own header, without wrapping. */
	if (size < length || size > end - offset)
		return EINVAL;

	/* Succeeded: the box. */
	*body = offset + length;
	*next = offset + size;
	return 0;
}

/* Reads exactly size bytes at offset; 0, or EIO for a short or failed read. */
static int
info_pread(
	int input,
	uint64_t offset,
	unsigned char *data,
	size_t size)
{
	ssize_t got;
	size_t done;

	/* Refuses an offset the system cannot name. */
	if (offset > (uint64_t)INT64_MAX - size)
		return EIO;

	/* Until it is all read. */
	done = 0U;
	while (done < size) {
		got = pread(input, data + done, size - done, (off_t)(offset + done));
		if (got < 0 && errno == EINTR)
			continue;
		if (got <= 0)
			return EIO;
		done += (size_t)got;
	}

	/* Succeeded: read. */
	return 0;
}

/*
 * Reads the movie header's creation time (seconds since 1904, UTC) as a
 * date; 0, or a time before 1970, is no date.
 */
static void
info_mvhd(
	int input,
	uint64_t body,
	uint64_t end,
	struct info_video *video)
{
	unsigned char data[12];
	uint64_t created;
	int error;

	/* The version and, after the flags, the creation time of its width. */
	if (end - body < sizeof(data))
		return;
	error = info_pread(input, body, data, sizeof(data));
	if (error != 0)
		return;
	if (data[0] == 1U) {
		created = ((uint64_t)info_be32(data + 4) << 32) | info_be32(data + 8);
	} else if (data[0] == 0U) {
		created = info_be32(data + 4);
	} else {
		return;
	}

	/* Before 1970, or beyond what a date can be, is unknown. */
	if (created <= INFO_MP4_EPOCH || created - INFO_MP4_EPOCH > (uint64_t)INT32_MAX * 4U)
		return;

	/* Succeeded: the date, in UTC. */
	video->taken = (ph_time)(created - INFO_MP4_EPOCH);
	video->dated = 1;
}

/*
 * Reads a track: its header's size, kept when the track's handler says it
 * is video.
 */
static void
info_trak(
	int input,
	uint64_t body,
	uint64_t end,
	struct info_video *video)
{
	uint64_t child;
	uint64_t inner;
	uint64_t next;
	long width;
	long height;
	char type[4];
	int sized;
	int is_video;
	int error;
	int same;

	/* The track's children: tkhd and mdia. */
	width = 0L;
	height = 0L;
	sized = 0;
	is_video = 0;
	for (child = body; child < end;) {
		error = info_box(input, child, end, &inner, &next, type);
		if (error != 0 || video->boxes++ >= INFO_BOX_MAX)
			return;
		same = memcmp(type, "tkhd", 4U);
		if (same == 0) {
			error = info_tkhd(input, inner, next, &width, &height);
			if (error == 0)
				sized = 1;
		}

		/* The media box says what kind of track it is. */
		same = memcmp(type, "mdia", 4U);
		if (same == 0)
			is_video = info_video_track(input, inner, next, video);
		child = next;
	}

	/* Succeeded: a video track's size. */
	if (sized && is_video) {
		video->width = width;
		video->height = height;
	}
}

/*
 * Reads a track header's size (16.16 fixed point) as shown: a matrix that
 * turns it a quarter turn swaps the sides.  0, or EINVAL.
 */
static int
info_tkhd(
	int input,
	uint64_t body,
	uint64_t end,
	long *width,
	long *height)
{
	unsigned char data[96];
	size_t matrix;
	size_t length;
	uint32_t across;
	uint32_t down;
	uint32_t first;
	uint32_t second;
	int error;

	/* The version gives where the matrix and the size are. */
	error = info_pread(input, body, data, 1U);
	if (error != 0)
		return error;
	if (data[0] == 1U) {
		matrix = 52U;
	} else if (data[0] == 0U) {
		matrix = 40U;
	} else {
		return EINVAL;
	}

	/* Up to the height, within the box. */
	length = matrix + 44U;
	if (end - body < length)
		return EINVAL;
	error = info_pread(input, body, data, length);
	if (error != 0)
		return error;

	/* The sides, whole pixels, within reason. */
	across = info_be32(data + matrix + 36U) >> 16;
	down = info_be32(data + matrix + 40U) >> 16;
	if ((long)across > INFO_SIDE_MAX || (long)down > INFO_SIDE_MAX)
		return EINVAL;

	/* A quarter turn: the matrix's a is 0 and its b is not. */
	first = info_be32(data + matrix);
	second = info_be32(data + matrix + 4U);
	if (first == 0U && second != 0U) {
		*width = (long)down;
		*height = (long)across;
		return 0;
	}

	/* Succeeded: the sides as stored. */
	*width = (long)across;
	*height = (long)down;
	return 0;
}

/* Tells whether a track's media box has the video handler ("vide"). */
static int
info_video_track(
	int input,
	uint64_t body,
	uint64_t end,
	struct info_video *video)
{
	unsigned char data[12];
	uint64_t child;
	uint64_t inner;
	uint64_t next;
	char type[4];
	int error;
	int same;

	/* The media box's children, to hdlr. */
	for (child = body; child < end;) {
		error = info_box(input, child, end, &inner, &next, type);
		if (error != 0 || video->boxes++ >= INFO_BOX_MAX)
			return 0;
		same = memcmp(type, "hdlr", 4U);
		if (same == 0)
			break;
		child = next;
	}

	/* No handler. */
	if (child >= end)
		return 0;

	/* The handler type after the version, the flags and pre_defined. */
	if (next - inner < sizeof(data))
		return 0;
	error = info_pread(input, inner, data, sizeof(data));
	if (error != 0)
		return 0;
	same = memcmp(data + 8, "vide", 4U);

	/* Succeeded: whether it is video. */
	return same == 0;
}

/*
 * Reads an AVI's size from its main header (avih), the first chunk of its
 * header list: RIFF, size, AVI, LIST, size, hdrl, avih, size, then the
 * header, whose width and height are its ninth and tenth words.
 */
static void
info_avi(
	const unsigned char *data,
	size_t size,
	long *width,
	long *height)
{
	uint32_t across;
	uint32_t down;
	int list;
	int header;

	/* The fixed layout up to the header's size fields. */
	if (size < 72U)
		return;
	list = memcmp(data + 12, "LIST", 4U);
	header = memcmp(data + 20, "hdrlavih", 8U);
	if (list != 0 || header != 0)
		return;
	across = info_le32(data + 64);
	down = info_le32(data + 68);

	/* Within reason. */
	if ((long)across > INFO_SIDE_MAX || (long)down > INFO_SIDE_MAX)
		return;
	*width = (long)across;
	*height = (long)down;
}

/* A big-endian 32-bit number. */
static uint32_t
info_be32(
	const unsigned char *data)
{
	/* The four bytes, most significant first. */
	return ((uint32_t)data[0] << 24) | ((uint32_t)data[1] << 16) | ((uint32_t)data[2] << 8) | data[3];
}

/* A little-endian 32-bit number. */
static uint32_t
info_le32(
	const unsigned char *data)
{
	/* The four bytes, least significant first. */
	return (uint32_t)data[0] | ((uint32_t)data[1] << 8) | ((uint32_t)data[2] << 16) | ((uint32_t)data[3] << 24);
}
