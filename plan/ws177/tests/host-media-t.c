/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * ws177-p027〜p030 (案 T): dumps what the media file reader
 * (userland/desktop/mediafile) finds in a file, for host-media-t.py to
 * compare with what ffprobe finds in it.
 *
 *     host-media-t FILE [SEEK_US...]
 *
 * Prints the format and length, each track (with the packets its index
 * left out), each packet (track, times, keyframe, size, the Adler-32 of
 * its bytes, which ffprobe's -show_data_hash adler32 also prints) and how the reading ended; then, for each SEEK_US, the seek and
 * the first packet of each track after it.  A file the reader refuses
 * prints "OPEN error=N" and exits with 1.
 */

#include "userland/desktop/mediafile/mediafile.h"
#include <stdio.h>
#include <stdlib.h>

/* The most tracks the seeks report a first packet for. */
#define HOST_TRACKS	16U

int main(int argc, char **argv);
static void print_tracks(const struct mf_file *file);
static void print_packet(const char *tag, const struct mf_packet *packet);
static void print_seek(struct mf_file *file, long long seek_us);

/*
 * Opens the file, prints what is in it, and seeks when asked.
 */
int
main(
	int argc,
	char **argv)
{
	struct mf_file *file;
	struct mf_packet packet;
	unsigned count;
	int i;
	int error;

	/* The file. */
	if (argc < 2) {
		fprintf(stderr, "usage: host-media-t FILE [SEEK_US...]\n");
		return 2;
	}

	/* Opens it; a refusal is printed. */
	error = mf_open(argv[1], &file);
	if (error != 0) {
		printf("OPEN error=%d\n", error);
		return 1;
	}

	/* The format, the length and each track. */
	print_tracks(file);

	/* Each packet to the end. */
	count = 0;
	for (;;) {
		/* The next one, or the end. */
		error = mf_read(file, &packet);
		if (error != 0)
			break;

		/* Printed and counted. */
		print_packet("PACKET", &packet);
		count++;
	}

	/* How reading ended (ENODATA at the end). */
	printf("END error=%d packets=%u\n", error, count);

	/* Each seek asked for. */
	for (i = 2; i < argc; i++)
		print_seek(file, strtoll(argv[i], NULL, 10));

	/* Succeeded: everything is printed. */
	mf_close(file);
	return 0;
}

/* Prints the format, the length and each track. */
static void
print_tracks(
	const struct mf_file *file)
{
	const struct mf_track *track;
	unsigned tracks;
	unsigned i;

	/* The format and length. */
	tracks = mf_track_count(file);
	printf("FORMAT %s duration_us=%lld tracks=%u\n",
	       mf_format_name(file),
	       (long long)mf_duration_us(file),
	       tracks);

	/* Each track. */
	for (i = 0; i < tracks; i++) {
		/* One line. */
		track = mf_track(file, i);
		printf("TRACK %u kind=%u codec=%u name=%s width=%u height=%u rate=%u channels=%u private=%zu packets=%llu dropped=%llu duration_us=%lld\n",
		       i,
		       track->kind,
		       track->codec,
		       track->codec_name,
		       track->width,
		       track->height,
		       track->sample_rate,
		       track->channels,
		       track->private_size,
		       (unsigned long long)track->packet_count,
		       (unsigned long long)track->dropped_count,
		       (long long)track->duration_us);
	}
}

/* Seeks and prints the first packet after it of each track (until each has one or the file ends). */
static void
print_seek(
	struct mf_file *file,
	long long seek_us)
{
	struct mf_packet packet;
	unsigned seen[HOST_TRACKS];
	unsigned tracks;
	unsigned left;
	unsigned i;
	int error;

	/* The seek. */
	error = mf_seek(file, (int64_t)seek_us);
	printf("SEEK %lld error=%d\n", seek_us, error);
	if (error != 0)
		return;

	/* No track has its first packet yet. */
	tracks = mf_track_count(file);
	if (tracks > HOST_TRACKS)
		tracks = HOST_TRACKS;
	for (i = 0; i < tracks; i++)
		seen[i] = 0;

	/* Reads until each track has shown its first packet. */
	left = tracks;
	while (left != 0) {
		/* The next packet, or the end. */
		error = mf_read(file, &packet);
		if (error != 0)
			break;

		/* A track's first after the seek is printed. */
		if (packet.track < tracks && !seen[packet.track]) {
			seen[packet.track] = 1U;
			left--;
			print_packet("AFTER", &packet);
		}
	}
}

/* Prints one packet: its track, times, keyframe flag, size and the Adler-32 of its bytes. */
static void
print_packet(
	const char *tag,
	const struct mf_packet *packet)
{
	unsigned long low;
	unsigned long high;
	size_t i;

	/* The Adler-32 of the bytes, which tells the right ones were read. */
	low = 1;
	high = 0;
	for (i = 0; i < packet->size; i++) {
		low = (low + packet->data[i]) % 65521UL;
		high = (high + low) % 65521UL;
	}

	/* The line. */
	printf("%s track=%u pts=%lld dts=%lld key=%d size=%zu adler32=%08lx\n",
	       tag,
	       packet->track,
	       (long long)packet->pts_us,
	       (long long)packet->dts_us,
	       packet->keyframe,
	       packet->size,
	       (high << 16) | low);
}
