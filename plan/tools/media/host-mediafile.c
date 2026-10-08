/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * ws122-p003: dumps what the media file reader (userland/desktop/mediafile)
 * finds in a file, for run-host-mediafile.sh to compare with what the file
 * was made with (make-media.py).
 *
 *     host-mediafile FILE [SEEK_US]
 *
 * With HOST_MEDIAFILE_SOURCE=1 in the environment, the file is opened
 * through a source (media_file_open_source, ws121-p002) whose reader is pread.
 * Prints the format, each track, and each packet (track, times, keyframe,
 * size, the sum of its bytes); with SEEK_US, then seeks there and prints
 * the next three packets.  A file the reader refuses prints "OPEN error=N"
 * and exits with 1.
 */

#include "userland/desktop/mediafile/mediafile.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

int main(int argc, char **argv);
static void print_packet(const char *tag, const struct media_packet *packet);
static int open_source(const char *path, struct media_file **file);
static int source_read_at(void *context, uint64_t offset, void *data, size_t size);

/* The descriptor the source reads (HOST_MEDIAFILE_SOURCE). */
static int source_fd = -1;

/*
 * Opens the file, prints what is in it, and seeks when asked.
 */
int
main(
	int argc,
	char **argv)
{
	const struct media_track *track;
	struct media_file *file;
	struct media_packet packet;
	long long seek_us;
	unsigned i;
	unsigned count;
	unsigned tracks;
	int error;

	/* The file, and the time to seek to. */
	if (argc < 2) {
		fprintf(stderr, "usage: host-mediafile FILE [SEEK_US]\n");
		return 2;
	}

	/* Opens it (through a source when asked); a refusal is printed. */
	if (getenv("HOST_MEDIAFILE_SOURCE") != NULL)
		error = open_source(argv[1], &file);
	else
		error = media_file_open(argv[1], &file);
	if (error != 0) {
		printf("OPEN error=%d\n", error);
		return 1;
	}

	/* The format, the length and each track. */
	tracks = media_file_track_count(file);
	printf("FORMAT %s duration_us=%lld tracks=%u\n",
	       media_file_format_name(file),
	       (long long)media_file_duration_us(file),
	       tracks);
	for (i = 0; i < tracks; i++) {
		/* One track. */
		track = media_file_track(file, i);
		printf("TRACK %u kind=%u codec=%u name=%s width=%u height=%u rate=%u channels=%u private=%zu packets=%llu\n",
		       i,
		       track->kind,
		       track->codec,
		       track->codec_name,
		       track->width,
		       track->height,
		       track->sample_rate,
		       track->channels,
		       track->private_size,
		       (unsigned long long)track->packet_count);
	}

	/* Each packet. */
	count = 0;
	for (;;) {
		/* The next one, or the end. */
		error = media_file_read(file, &packet);
		if (error != 0)
			break;

		/* Printed and counted. */
		print_packet("PACKET", &packet);
		count++;
	}

	/* How reading ended (ENODATA at the end). */
	printf("END error=%d packets=%u\n", error, count);

	/* The seek, and the three packets after it. */
	if (argc >= 3) {
		seek_us = strtoll(argv[2], NULL, 10);
		error = media_file_seek(file, (int64_t)seek_us);
		printf("SEEK %lld error=%d\n", seek_us, error);
		for (i = 0; i < 3U; i++) {
			/* One packet after the seek. */
			error = media_file_read(file, &packet);
			if (error != 0)
				break;

			/* Printed. */
			print_packet("AFTER", &packet);
		}
	}

	/* Succeeded: everything is printed. */
	media_file_close(file);
	if (source_fd >= 0)
		(void)close(source_fd);
	return 0;
}

/* Opens a file through a source that reads it with pread. */
static int
open_source(
	const char *path,
	struct media_file **file)
{
	struct media_source source;
	struct stat status;

	/* The descriptor and the size. */
	source_fd = open(path, O_RDONLY);
	if (source_fd < 0)
		return errno;
	if (fstat(source_fd, &status) != 0)
		return errno;

	/* The source. */
	source.read_at = source_read_at;
	source.size = (uint64_t)status.st_size;
	source.context = &source_fd;
	return media_file_open_source(&source, file);
}

/* Reads all the bytes asked at an offset. */
static int
source_read_at(
	void *context,
	uint64_t offset,
	void *data,
	size_t size)
{
	ssize_t got;
	int fd;

	/* One read of them all (a test file of the host). */
	fd = *(int *)context;
	got = pread(fd, data, size, (off_t)offset);
	if (got != (ssize_t)size)
		return EIO;
	return 0;
}

/*
 * Prints one packet: its track, times, keyframe flag, size and the sum of
 * its bytes.
 */
static void
print_packet(
	const char *tag,
	const struct media_packet *packet)
{
	unsigned long sum;
	size_t i;

	/* The sum of the bytes, which tells the right ones were read. */
	sum = 0;
	for (i = 0; i < packet->size; i++)
		sum += packet->data[i];

	/* The line. */
	printf("%s track=%u pts=%lld dts=%lld key=%d size=%zu sum=%lu\n",
	       tag,
	       packet->track,
	       (long long)packet->pts_us,
	       (long long)packet->dts_us,
	       packet->keyframe,
	       packet->size,
	       sum);
}
