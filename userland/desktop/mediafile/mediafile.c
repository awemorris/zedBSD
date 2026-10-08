/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The media file reader's common part (WS122 p003): it opens the file,
 * tells its format from the first bytes, hands the work to that format's
 * reader (mp4.c, mkv.c, ts.c, ogg.c, avi.c) and gives the helpers both use.
 */

#include "mediafile-private.h"
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* The first bytes of a file read to tell its format: three transport packets of 192 bytes. */
#define MF_HEAD_SIZE		(3U * 192U + 8U)

static const struct mf_format *format_of(const unsigned char *head, size_t length);
static int mediafile_start(struct mf_file *opened);

/*
 * Opens a media file and reads its tracks.  Returns 0 with *file set, or
 * an errno value: EINVAL for a file of no format this reader knows, or one that
 * is damaged.
 */
int
mf_open(
	const char *path,
	struct mf_file **file)
{
	struct mf_file *opened;
	struct stat status;
	int error;

	/* The state, with no descriptor yet. */
	*file = NULL;
	opened = calloc(1, sizeof(*opened));
	if (opened == NULL)
		return ENOMEM;

	/* No descriptor until the open succeeds. */
	opened->fd = -1;

	/* The file and its size. */
	opened->fd = open(path, O_RDONLY | O_CLOEXEC);
	if (opened->fd < 0) {
		error = errno;
		mf_close(opened);
		return error;
	}

	/* Its size; a file too small to hold a header is not a media file. */
	error = fstat(opened->fd, &status);
	if (error != 0 || status.st_size < 12) {
		mf_close(opened);
		return EINVAL;
	}

	/* Its format, header and index. */
	opened->size = (uint64_t)status.st_size;
	error = mediafile_start(opened);
	if (error != 0) {
		mf_close(opened);
		return error;
	}

	/* Succeeded: the file is open. */
	*file = opened;
	return 0;
}

/*
 * Opens a file whose bytes come from a source (ws121-p002) and reads its
 * header and index through it.  Returns 0 or an errno value as mf_open
 * does, or the source's (ECANCELED, EIO).
 */
int
mf_open_source(
	const struct mf_source *source,
	struct mf_file **file)
{
	struct mf_file *opened;
	int error;

	/* The state, reading through the source. */
	*file = NULL;
	if (source == NULL || source->read_at == NULL || source->size < 12U)
		return EINVAL;
	opened = calloc(1, sizeof(*opened));
	if (opened == NULL)
		return ENOMEM;
	opened->fd = -1;
	opened->source = *source;
	opened->size = source->size;

	/* Its format, header and index. */
	error = mediafile_start(opened);
	if (error != 0) {
		mf_close(opened);
		return error;
	}

	/* Succeeded: the file is open. */
	*file = opened;
	return 0;
}

/*
 * Reports how many tracks the file has.
 */
unsigned
mf_track_count(
	const struct mf_file *file)
{
	/* The tracks found when it was opened. */
	return file->track_count;
}

/*
 * Reports one track, or NULL for an index past the last.
 */
const struct mf_track *
mf_track(
	const struct mf_file *file,
	unsigned index)
{
	/* Refuses an index past the last. */
	if (index >= file->track_count)
		return NULL;

	/* The track. */
	return &file->tracks[index];
}

/*
 * Reports the presentation's length in microseconds (0 when unknown).
 */
int64_t
mf_duration_us(
	const struct mf_file *file)
{
	/* What the header said. */
	return file->duration_us;
}

/*
 * Names the file's format.
 */
const char *
mf_format_name(
	const struct mf_file *file)
{
	/* The reader's name. */
	return file->format->name;
}

/*
 * Reads the next packet in the file's order.  Returns 0, ENODATA at the
 * end, or another errno value for a damaged file.
 */
int
mf_read(
	struct mf_file *file,
	struct mf_packet *packet)
{
	int error;

	/* The format's reader. */
	memset(packet, 0, sizeof(*packet));
	error = file->format->read(file, packet);
	if (error != 0)
		return error;

	/* Succeeded: the packet is in the buffer. */
	return 0;
}

/*
 * Moves to the last place a decoder can start at or before a time, so that
 * the next mf_read returns its packets.
 */
int
mf_seek(
	struct mf_file *file,
	int64_t time_us)
{
	int error;

	/* Times before the start mean the start. */
	if (time_us < 0)
		time_us = 0;

	/* The format's reader. */
	error = file->format->seek(file, time_us);
	if (error != 0)
		return error;

	/* Succeeded: the next packet is from there. */
	return 0;
}

/*
 * Closes the file and lets everything it holds go.
 */
void
mf_close(
	struct mf_file *file)
{
	unsigned i;

	/* Nothing to close. */
	if (file == NULL)
		return;

	/* The format's state. */
	if (file->format != NULL && file->state != NULL)
		file->format->close(file);

	/* The tracks' private data (a slot of a track left out may hold some too). */
	for (i = 0; i < MF_TRACK_MAX; i++)
		free((void *)file->tracks[i].private_data);

	/* The descriptor, the buffer and the file itself. */
	if (file->fd >= 0)
		(void)close(file->fd);

	/* The packet buffer and the state. */
	free(file->buffer);
	free(file);
}

/*
 * Reads size bytes at an offset of the file, all of them.  Returns 0, or
 * EINVAL when they would run past the end and EIO when the read fails.
 */
int
mf_read_at(
	struct mf_file *file,
	uint64_t offset,
	void *data,
	size_t size)
{
	unsigned char *cursor;
	ssize_t got;
	int error;

	/* Refuses a range past the end of the file. */
	if (offset > file->size || size > file->size - offset)
		return EINVAL;

	/* A source reads for itself. */
	if (file->fd < 0 && file->source.read_at != NULL) {
		error = file->source.read_at(file->source.context, offset, data, size);
		return error;
	}

	/* Reads until every byte is in. */
	cursor = data;
	while (size != 0) {
		/* One read; an interrupted one is tried again. */
		got = pread(file->fd, cursor, size, (off_t)offset);
		if (got < 0 && errno == EINTR)
			continue;

		/* A failure, or the file shrank under the reader. */
		if (got <= 0)
			return EIO;

		/* Moves on by what was read. */
		cursor += got;
		offset += (uint64_t)got;
		size -= (size_t)got;
	}

	/* Succeeded: every byte was read. */
	return 0;
}

/*
 * Makes the packet buffer hold at least size bytes.  Returns 0, EINVAL for
 * a packet larger than MF_PACKET_MAX, or ENOMEM.
 */
int
mf_packet_room(
	struct mf_file *file,
	size_t size)
{
	unsigned char *buffer;

	/* Refuses a packet no real stream has. */
	if (size > MF_PACKET_MAX)
		return EINVAL;

	/* The buffer is large enough already. */
	if (size <= file->buffer_size && file->buffer != NULL)
		return 0;

	/* A larger one (one byte at least, so that an empty packet has a buffer). */
	buffer = realloc(file->buffer, size + 1U);
	if (buffer == NULL)
		return ENOMEM;

	/* Succeeded: the buffer holds the packet. */
	file->buffer = buffer;
	file->buffer_size = size;
	return 0;
}

/*
 * Keeps a copy of a track's codec private data.  Returns 0, EINVAL for
 * data larger than MF_PRIVATE_MAX, or ENOMEM.
 */
int
mf_keep_private(
	struct mf_track *track,
	const unsigned char *data,
	size_t size)
{
	unsigned char *copy;

	/* Refuses private data no codec has. */
	if (size > MF_PRIVATE_MAX)
		return EINVAL;

	/* Nothing to keep. */
	if (size == 0)
		return 0;

	/* The copy. */
	copy = malloc(size);
	if (copy == NULL)
		return ENOMEM;

	/* Replaces what was kept before. */
	memcpy(copy, data, size);
	free((void *)track->private_data);
	track->private_data = copy;
	track->private_size = size;

	/* Succeeded: the track holds the data. */
	return 0;
}

/*
 * Keeps the container's name of a track's codec, cut to what fits.
 */
void
mf_set_codec_name(
	struct mf_track *track,
	const char *name,
	size_t length)
{
	/* Cut to the room, with its terminator. */
	if (length >= sizeof(track->codec_name))
		length = sizeof(track->codec_name) - 1U;

	/* The name. */
	memcpy(track->codec_name, name, length);
	track->codec_name[length] = '\0';
}

/*
 * Turns a count of units (units_per_second of them a second) into
 * microseconds without overflowing for any length a file can describe.
 */
int64_t
mf_scale_us(
	int64_t value,
	uint64_t units_per_second)
{
	int64_t whole;
	int64_t rest;

	/* No scale: no time. */
	if (units_per_second == 0)
		return 0;

	/* Whole seconds and the rest, each scaled on its own. */
	whole = value / (int64_t)units_per_second;
	rest = value % (int64_t)units_per_second;

	/* Their sum in microseconds. */
	return whole * 1000000 + rest * 1000000 / (int64_t)units_per_second;
}

/*
 * Reads a big-endian 16-bit number.
 */
uint16_t
mf_be16(
	const unsigned char *data)
{
	/* High byte first. */
	return (uint16_t)(((unsigned)data[0] << 8) | data[1]);
}

/*
 * Reads a big-endian 32-bit number.
 */
uint32_t
mf_be32(
	const unsigned char *data)
{
	/* High byte first. */
	return ((uint32_t)data[0] << 24) | ((uint32_t)data[1] << 16) |
	       ((uint32_t)data[2] << 8) | data[3];
}

/*
 * Reads a big-endian 64-bit number.
 */
uint64_t
mf_be64(
	const unsigned char *data)
{
	/* The high half first. */
	return ((uint64_t)mf_be32(data) << 32) | mf_be32(data + 4);
}

/*
 * Tells a file's format from its first bytes: Matroska's EBML header, an
 * MP4 box whose type is one an MP4 starts with, an AVI file's RIFF header,
 * an Ogg page that begins a stream, or a transport stream's packets.  NULL for none of them.
 */
static const struct mf_format *
format_of(
	const unsigned char *head,
	size_t length)
{
	int transport;
	int ogg;
	int avi;
	int compared;

	/* Too short for either header. */
	if (length < 8U)
		return NULL;

	/* Matroska and WebM start with the EBML element. */
	if (head[0] == 0x1aU && head[1] == 0x45U && head[2] == 0xdfU && head[3] == 0xa3U)
		return &mf_mkv_format;

	/* An MP4's first box: its file type, or a movie, data or padding box. */
	compared = memcmp(head + 4, "ftyp", 4);
	if (compared == 0)
		return &mf_mp4_format;

	/* Some writers put the movie or the data first. */
	compared = memcmp(head + 4, "moov", 4);
	if (compared == 0)
		return &mf_mp4_format;

	/* Or the data, with the movie after it. */
	compared = memcmp(head + 4, "mdat", 4);
	if (compared == 0)
		return &mf_mp4_format;

	/* An AVI file: RIFF and 'AVI ' (ws177-p030). */
	avi = mf_avi_detect(head, length);
	if (avi)
		return &mf_avi_format;

	/* An Ogg file: its first page begins a stream (ws177-p029). */
	ogg = mf_ogg_detect(head, length);
	if (ogg)
		return &mf_ogg_format;

	/* A transport stream: packets of 188 or 192 bytes, each with its sync byte (ws177-p028). */
	transport = mf_ts_detect(head, length);
	if (transport)
		return &mf_ts_format;

	/* Neither format. */
	return NULL;
}

/* Tells the format from the first bytes and reads its header and index; 0 or an errno value. */
static int
mediafile_start(
	struct mf_file *opened)
{
	unsigned char head[MF_HEAD_SIZE];
	size_t length;
	int error;

	/* The first bytes tell the format (a short file has fewer). */
	length = sizeof(head);
	if (opened->size < length)
		length = (size_t)opened->size;
	error = mf_read_at(opened, 0, head, length);
	if (error != 0)
		return error;

	/* Refuses a format this reader does not know. */
	opened->format = format_of(head, length);
	if (opened->format == NULL)
		return EINVAL;

	/* The format's header and index. */
	error = opened->format->open(opened);
	return error;
}
