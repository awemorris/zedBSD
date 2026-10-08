/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Reads a media file's container without FFmpeg (WS122 p003): MP4 and MOV
 * (ISO BMFF, also fragmented, ws177-p027), Matroska and WebM, MPEG-TS
 * (ws177-p028) and Ogg (ws177-p029).  It finds the tracks and hands out their
 * packets, the compressed frames, with their times, in the order the file
 * stores them; it does not decode.  The player chooses a decoder from a
 * track's codec and private data (the H.264 avcC, the AAC
 * AudioSpecificConfig and so on).
 *
 * A file is read with pread; the memory it takes grows with the number of
 * packets in an MP4's index, not with the file's size.  Times are in
 * microseconds from the start of the presentation.
 */

#ifndef MEDIAFILE_MEDIAFILE_H
#define MEDIAFILE_MEDIAFILE_H

#include <stddef.h>
#include <stdint.h>

/* The kinds of track. */
#define MF_TRACK_OTHER		0U
#define MF_TRACK_VIDEO		1U
#define MF_TRACK_AUDIO		2U

/* The codecs a track can be known to carry. */
#define MF_CODEC_UNKNOWN	0U
#define MF_CODEC_H264		1U
#define MF_CODEC_HEVC		2U
#define MF_CODEC_AV1		3U
#define MF_CODEC_VP9		4U
#define MF_CODEC_VP8		5U
#define MF_CODEC_MPEG4		6U
#define MF_CODEC_AAC		7U
#define MF_CODEC_OPUS		8U
#define MF_CODEC_MP3		9U
#define MF_CODEC_VORBIS		10U
#define MF_CODEC_THEORA		11U

/* The longest codec name a track keeps (an MP4 four-character code or a Matroska CodecID). */
#define MF_CODEC_NAME_MAX	32U

/*
 * One track: its kind and codec (and the container's name for the codec),
 * the picture's size or the sound's rate and channels, the codec's private
 * data as the container holds it (an avcC, an esds's decoder-specific
 * information, a Matroska CodecPrivate; NULL when there is none), its
 * length, how many packets it has when the container says (an MP4's
 * index; 0 when unknown), and how many packets its index names that were
 * left out because they lie outside the file (a file cut short or a
 * damaged index; ws177-p027).
 */
struct mf_track {
	unsigned kind;
	unsigned codec;
	char codec_name[MF_CODEC_NAME_MAX];
	uint32_t width;
	uint32_t height;
	uint32_t sample_rate;
	uint32_t channels;
	const unsigned char *private_data;
	size_t private_size;
	int64_t duration_us;
	uint64_t packet_count;
	uint64_t dropped_count;
};

/*
 * One packet: its track's index, its presentation and decoding times, whether
 * a decoder can start at it, and its bytes, which stay valid until the
 * next mf_read, mf_seek or mf_close.
 */
struct mf_packet {
	unsigned track;
	int64_t pts_us;
	int64_t dts_us;
	int keyframe;
	const unsigned char *data;
	size_t size;
};

/*
 * A source of a file's bytes other than a file of the system (ws121-p002:
 * a page's media read over the network): its reader of bytes at an offset
 * (all of them; 0, or ECANCELED when the reading was stopped, EIO when
 * they cannot be had, EINVAL past the end), the whole size, and the
 * reader's context.  It is called from the thread that reads the file.
 */
struct mf_source {
	int (*read_at)(void *context, uint64_t offset, void *data, size_t size);
	uint64_t size;
	void *context;
};

struct mf_file;

int mf_open(const char *path, struct mf_file **file);
int mf_open_source(const struct mf_source *source, struct mf_file **file);
unsigned mf_track_count(const struct mf_file *file);
const struct mf_track *mf_track(const struct mf_file *file, unsigned index);
int64_t mf_duration_us(const struct mf_file *file);
const char *mf_format_name(const struct mf_file *file);
int mf_read(struct mf_file *file, struct mf_packet *packet);
int mf_seek(struct mf_file *file, int64_t time_us);
void mf_close(struct mf_file *file);

#endif
