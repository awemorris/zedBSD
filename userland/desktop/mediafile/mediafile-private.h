/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * What the container readers (mp4.c, mkv.c, ts.c, ogg.c, avi.c) share with mediafile.c: the
 * open file, its tracks, the packet buffer, the reader of each format,
 * and the helpers that read the file and its big-endian numbers.
 */

#ifndef MEDIAFILE_MEDIAFILE_PRIVATE_H
#define MEDIAFILE_MEDIAFILE_PRIVATE_H

#include "mediafile.h"
#include <stddef.h>
#include <stdint.h>

/* The most tracks a file may have; the rest are left out. */
#define MF_TRACK_MAX		16U

/* The largest packet read (a frame of a 4K video is well below). */
#define MF_PACKET_MAX		(64U * 1024U * 1024U)

/* The largest codec private data kept. */
#define MF_PRIVATE_MAX		(1024U * 1024U)

struct mf_file;

/*
 * A format's reader: it reads the file's header and index (open), hands out
 * the next packet (read), moves to a time (seek) and lets its state go
 * (close).  Each returns 0 or an errno value; read returns ENODATA at the
 * end.
 */
struct mf_format {
	const char *name;
	int (*open)(struct mf_file *file);
	int (*read)(struct mf_file *file, struct mf_packet *packet);
	int (*seek)(struct mf_file *file, int64_t time_us);
	void (*close)(struct mf_file *file);
};

/*
 * An open file: its descriptor (-1 for a source) or its source, its size,
 * its format and that format's state, the tracks found, the length of the
 * presentation, and the buffer the last packet was read into.
 */
struct mf_file {
	int fd;
	struct mf_source source;
	uint64_t size;
	const struct mf_format *format;
	void *state;
	struct mf_track tracks[MF_TRACK_MAX];
	unsigned track_count;
	int64_t duration_us;
	unsigned char *buffer;
	size_t buffer_size;
};

extern const struct mf_format mf_mp4_format;
extern const struct mf_format mf_mkv_format;
extern const struct mf_format mf_ts_format;
extern const struct mf_format mf_ogg_format;
extern const struct mf_format mf_avi_format;

int mf_ts_detect(const unsigned char *head, size_t length);
int mf_ogg_detect(const unsigned char *head, size_t length);
int mf_avi_detect(const unsigned char *head, size_t length);

int mf_read_at(struct mf_file *file, uint64_t offset, void *data, size_t size);
int mf_packet_room(struct mf_file *file, size_t size);
int mf_keep_private(struct mf_track *track, const unsigned char *data, size_t size);
void mf_set_codec_name(struct mf_track *track, const char *name, size_t length);
int64_t mf_scale_us(int64_t value, uint64_t units_per_second);
uint16_t mf_be16(const unsigned char *data);
uint32_t mf_be32(const unsigned char *data);
uint64_t mf_be64(const unsigned char *data);

#endif
