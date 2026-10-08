/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The AVI reader (ws177-p030): a RIFF 'AVI ' file, its header list (hdrl:
 * the streams' strh and strf) and its packets in the movi list, found by
 * the idx1 index when the file has one (its offsets from the movi list or
 * from the file's start, whichever points at the chunks), else by reading
 * the movi list's chunks one after another.  The movi lists of OpenDML's
 * further RIFF 'AVIX' parts are read the same way (their indexes are not).
 *
 * Video is MPEG-4 Part 2, H.264 (Annex B), H.265 or Motion JPEG; sound is
 * PCM, MP3 or AAC.  A video chunk is a frame, timed by its number at the
 * stream's rate over its scale; a sound chunk is timed by the bytes before
 * it over the stream's sample size, or by its number when the size is 0
 * (each chunk a frame).  An empty chunk (a frame dropped) takes its time
 * without a packet.  The packets are handed out in the file's order; an
 * index entry that points outside the file is left out and counted
 * (mf_track.dropped_count).
 */

#include "mediafile-private.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>

/* The most streams an AVI file is read for. */
#define AVI_STREAMS_MAX		100U

/* The most packets kept (a day of 120 fps video and its sound is about 20 million). */
#define AVI_PACKETS_MAX		(32U * 1024U * 1024U)

/* The largest header list read into memory. */
#define AVI_HEADERS_MAX		(1024U * 1024U)

/* An idx1 entry's flag of a key frame. */
#define AVI_KEYFRAME		0x10U

/* The sound formats (wFormatTag) known. */
#define AVI_FORMAT_PCM		0x0001U
#define AVI_FORMAT_MP3		0x0055U
#define AVI_FORMAT_AAC		0x00ffU
#define AVI_FORMAT_ADTS		0x1600U
#define AVI_FORMAT_AAC_LATM	0x706dU

/*
 * One packet in the file's order: where its bytes are, how many, its
 * track, its time in its stream's units (the frame's number, or the
 * blocks or chunks before it), and whether a decoder can start at it.
 */
struct avi_packet {
	uint64_t offset;
	uint32_t size;
	uint8_t track;
	uint8_t key;
	int64_t time;
};

/*
 * One stream of the file as it is read: the track it is (or -1 for one
 * not read), whether it is video, its scale and rate, its sample size (0:
 * each chunk is one), and the units counted so far (frames, blocks or
 * chunks).
 */
struct avi_stream {
	int track;
	unsigned video;
	uint32_t scale;
	uint32_t rate;
	uint32_t sample_size;
	uint64_t counted;
	uint64_t bytes;
};

/*
 * The reader's state: the streams, the packets in the file's order (room
 * for capacity), the next one to hand out, and the stream of each track.
 */
struct avi_state {
	struct avi_stream streams[AVI_STREAMS_MAX];
	unsigned stream_count;
	struct avi_packet *packets;
	size_t count;
	size_t capacity;
	size_t next;
	unsigned stream_of_track[MF_TRACK_MAX];
};

/* A chunk of a RIFF file: its four-character code, where its data starts and how long it is. */
struct avi_chunk {
	char id[4];
	uint64_t data;
	uint64_t size;
};

static int avi_open(struct mf_file *file);
static int avi_read(struct mf_file *file, struct mf_packet *packet);
static int avi_seek(struct mf_file *file, int64_t time_us);
static void avi_close(struct mf_file *file);
static int chunk_at(struct mf_file *file, uint64_t position, uint64_t end, struct avi_chunk *chunk);
static uint64_t chunk_next(const struct avi_chunk *chunk);
static uint32_t le32(const unsigned char *bytes);
static uint16_t le16(const unsigned char *bytes);
static int read_hdrl(struct mf_file *file, struct avi_state *state, const struct avi_chunk *hdrl);
static int read_strl(struct mf_file *file, struct avi_state *state, const unsigned char *data, size_t size);
static void read_video_format(struct mf_track *info, const unsigned char *strf, size_t size);
static int is_data_id(const char *id);
static void read_sound_format(struct mf_track *info, const unsigned char *strf, size_t size);
static int same_code(const unsigned char *code, const char *name);
static int read_idx1(struct mf_file *file, struct avi_state *state, const struct avi_chunk *idx1, uint64_t movi);
static int scan_movi(struct mf_file *file, struct avi_state *state, uint64_t start, uint64_t end, int depth);
static int stream_of_id(const char *id, unsigned *stream);
static int add_packet(struct mf_file *file, struct avi_state *state, unsigned stream, uint64_t offset, uint64_t size, int key);
static int chunk_key(struct mf_file *file, const struct mf_track *info, uint64_t offset, uint64_t size);
static int64_t packet_us(const struct avi_state *state, const struct avi_packet *packet);

/* The reader as mediafile.c calls it. */
const struct mf_format mf_avi_format = {
	"avi",
	avi_open,
	avi_read,
	avi_seek,
	avi_close,
};

/*
 * Tells whether a file's first bytes are an AVI file's: RIFF and 'AVI '.
 */
int
mf_avi_detect(
	const unsigned char *head,
	size_t length)
{
	int riff;
	int avi;

	/* RIFF, its size, then the form type. */
	if (length < 12U)
		return 0;
	riff = memcmp(head, "RIFF", 4);
	avi = memcmp(head + 8, "AVI ", 4);
	if (riff != 0 || avi != 0)
		return 0;

	/* An AVI file. */
	return 1;
}

/*
 * Reads the header list, then the packets: by idx1 when it has them, else
 * by the movi list's chunks; then the further AVIX parts' movi lists.
 */
static int
avi_open(
	struct mf_file *file)
{
	struct avi_state *state;
	struct avi_stream *stream;
	struct mf_track *info;
	struct avi_chunk chunk;
	struct avi_chunk hdrl;
	struct avi_chunk idx1;
	unsigned char type[4];
	uint64_t position;
	uint64_t end;
	uint64_t movi_start;
	uint64_t movi_end;
	unsigned have_hdrl;
	unsigned have_idx1;
	unsigned have_movi;
	unsigned i;
	int compared;
	int error;

	/* The state, kept in the file so that mf_close frees it on any failure. */
	state = calloc(1, sizeof(*state));
	if (state == NULL)
		return ENOMEM;
	file->state = state;

	/* The first RIFF's chunks: its size may run past a file cut short. */
	error = chunk_at(file, 0, file->size, &chunk);
	if (error != 0)
		return EINVAL;
	end = chunk.data + chunk.size;
	if (end > file->size)
		end = file->size;
	have_hdrl = 0;
	have_idx1 = 0;
	have_movi = 0;
	movi_start = 0;
	movi_end = 0;
	memset(&hdrl, 0, sizeof(hdrl));
	memset(&idx1, 0, sizeof(idx1));
	for (position = 12U; position + 8U <= end; position = chunk_next(&chunk)) {
		/* The next chunk; a damaged one ends the first part. */
		error = chunk_at(file, position, end, &chunk);
		if (error != 0)
			break;

		/* The index. */
		compared = memcmp(chunk.id, "idx1", 4);
		if (compared == 0) {
			idx1 = chunk;
			have_idx1 = 1U;
			continue;
		}

		/* A list: its type. */
		compared = memcmp(chunk.id, "LIST", 4);
		if (compared != 0 || chunk.size < 4U)
			continue;
		error = mf_read_at(file, chunk.data, type, 4U);
		if (error != 0)
			return error;

		/* The header list. */
		compared = memcmp(type, "hdrl", 4);
		if (compared == 0 && !have_hdrl) {
			hdrl = chunk;
			have_hdrl = 1U;
			continue;
		}

		/* The packets' list (the first). */
		compared = memcmp(type, "movi", 4);
		if (compared == 0 && !have_movi) {
			movi_start = chunk.data;
			movi_end = chunk.data + chunk.size;
			if (movi_end > end)
				movi_end = end;
			have_movi = 1U;
		}
	}

	/* Refuses a file without its header list or its packets' list. */
	if (!have_hdrl || !have_movi)
		return EINVAL;

	/* The streams. */
	error = read_hdrl(file, state, &hdrl);
	if (error != 0)
		return error;
	if (file->track_count == 0)
		return EINVAL;

	/* The packets: by the index, else by the list's chunks. */
	error = ENOENT;
	if (have_idx1)
		error = read_idx1(file, state, &idx1, movi_start);
	if (error == ENOENT)
		error = scan_movi(file, state, movi_start + 4U, movi_end, 0);
	if (error != 0)
		return error;

	/* The further parts (OpenDML's RIFF AVIX), their movi lists read chunk by chunk. */
	for (position = end + (end & 1U); position + 12U <= file->size; position = chunk_next(&chunk)) {
		/* A RIFF part; anything else ends them. */
		error = chunk_at(file, position, file->size, &chunk);
		if (error != 0)
			break;
		compared = memcmp(chunk.id, "RIFF", 4);
		if (compared != 0)
			break;

		/* Its lists, the movi one read. */
		error = scan_movi(file, state, chunk.data + 4U, chunk.data + chunk.size, 0);
		if (error != 0)
			return error;
	}

	/* Each track's length: the units counted, at its rate; the file is as long as its longest. */
	for (i = 0; i < state->stream_count; i++) {
		/* A stream read. */
		stream = &state->streams[i];
		if (stream->track < 0)
			continue;

		/* Its length. */
		info = &file->tracks[stream->track];
		info->duration_us = mf_scale_us((int64_t)(stream->counted * stream->scale), stream->rate);
		if (info->duration_us > file->duration_us)
			file->duration_us = info->duration_us;
	}

	/* Succeeded: the packets are known. */
	return 0;
}

/*
 * Hands out the next packet in the file's order.
 */
static int
avi_read(
	struct mf_file *file,
	struct mf_packet *packet)
{
	struct avi_state *state;
	struct avi_packet *next;
	int error;

	/* The end. */
	state = file->state;
	if (state->next >= state->count)
		return ENODATA;

	/* Its bytes. */
	next = &state->packets[state->next];
	error = mf_packet_room(file, next->size);
	if (error != 0)
		return error;
	error = mf_read_at(file, next->offset, file->buffer, next->size);
	if (error != 0)
		return error;

	/* The packet, and the reading moves on. */
	packet->track = next->track;
	packet->pts_us = packet_us(state, next);
	packet->dts_us = packet->pts_us;
	packet->keyframe = next->key;
	packet->data = file->buffer;
	packet->size = next->size;
	state->next++;

	/* Succeeded: one packet. */
	return 0;
}

/*
 * Moves to the last key frame of the first video track (else the first
 * track) at or before the time; the packets are handed out from it in the
 * file's order.
 */
static int
avi_seek(
	struct mf_file *file,
	int64_t time_us)
{
	struct avi_state *state;
	unsigned lead;
	unsigned t;
	size_t i;
	size_t chosen;
	unsigned found;
	int64_t time;

	/* The track that decides: the first video track, else the first. */
	state = file->state;
	lead = 0;
	for (t = file->track_count; t > 0; t--) {
		/* A video track before the one found so far. */
		if (file->tracks[t - 1U].kind == MF_TRACK_VIDEO)
			lead = t - 1U;
	}

	/* Its last key frame at or before the time (its first when none is). */
	chosen = 0;
	found = 0;
	for (i = 0; i < state->count; i++) {
		/* A key frame of the lead. */
		if (state->packets[i].track != lead || !state->packets[i].key)
			continue;

		/* The first one, and each one not after the time. */
		time = packet_us(state, &state->packets[i]);
		if (!found || time <= time_us)
			chosen = i;
		found = 1U;
		if (time > time_us)
			break;
	}

	/* Succeeded: the next packet is that one. */
	state->next = chosen;
	return 0;
}

/*
 * Lets the reader's state go.
 */
static void
avi_close(
	struct mf_file *file)
{
	struct avi_state *state;

	/* The packets and the state. */
	state = file->state;
	free(state->packets);
	free(state);
	file->state = NULL;
}

/*
 * Reads a chunk's header at a position (before end).  Returns 0, or
 * EINVAL when there is no whole header, or the reading's error.  The
 * chunk's size may run past end (a file cut short); its user checks.
 */
static int
chunk_at(
	struct mf_file *file,
	uint64_t position,
	uint64_t end,
	struct avi_chunk *chunk)
{
	unsigned char header[8];
	int error;

	/* The header's 8 bytes. */
	if (position > end || end - position < 8U)
		return EINVAL;
	error = mf_read_at(file, position, header, 8U);
	if (error != 0)
		return error;

	/* Its code, and where its data is. */
	memcpy(chunk->id, header, 4);
	chunk->data = position + 8U;
	chunk->size = le32(header + 4);

	/* Succeeded: the chunk. */
	return 0;
}

/* Tells where the chunk after one starts (chunks are padded to an even size). */
static uint64_t
chunk_next(
	const struct avi_chunk *chunk)
{
	uint64_t next;

	/* After its data and its padding byte. */
	next = chunk->data + chunk->size + (chunk->size & 1U);
	return next;
}

/* Reads a little-endian 32-bit number. */
static uint32_t
le32(
	const unsigned char *bytes)
{
	/* Low byte first. */
	return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) | ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
}

/* Reads a little-endian 16-bit number. */
static uint16_t
le16(
	const unsigned char *bytes)
{
	/* Low byte first. */
	return (uint16_t)((unsigned)bytes[0] | ((unsigned)bytes[1] << 8));
}

/*
 * Reads the header list: each stream's list (strl) makes a stream, a track
 * for each one this reader can hand out.  Returns 0, EINVAL for a list too
 * large or damaged, ENOMEM, or the reading's error.
 */
static int
read_hdrl(
	struct mf_file *file,
	struct avi_state *state,
	const struct avi_chunk *hdrl)
{
	unsigned char *data;
	uint64_t size;
	size_t position;
	size_t length;
	int compared;
	int error;

	/* The list, whole (its size may run past a file cut short). */
	size = hdrl->size;
	if (hdrl->data + size > file->size)
		size = file->size - hdrl->data;
	if (size > AVI_HEADERS_MAX || size < 4U)
		return EINVAL;
	data = malloc((size_t)size);
	if (data == NULL)
		return ENOMEM;
	error = mf_read_at(file, hdrl->data, data, (size_t)size);
	if (error != 0) {
		free(data);
		return error;
	}

	/* Its chunks after the list type: each strl list. */
	for (position = 4U; position + 8U <= size; position += 8U + length + (length & 1U)) {
		/* A chunk's length; one running past the list ends it. */
		length = le32(data + position + 4);
		if (length > size - position - 8U)
			break;

		/* A stream's list. */
		compared = memcmp(data + position, "LIST", 4);
		if (compared != 0 || length < 4U)
			continue;
		compared = memcmp(data + position + 8, "strl", 4);
		if (compared != 0)
			continue;

		/* The stream. */
		error = read_strl(file, state, data + position + 12, length - 4U);
		if (error != 0) {
			free(data);
			return error;
		}
	}

	/* Succeeded: the streams are known. */
	free(data);
	return 0;
}

/*
 * Reads a stream's list: its header (strh: its type, scale, rate and
 * sample size) and its format (strf).  A stream of a kind or codec this
 * reader does not know keeps its number without a track.
 */
static int
read_strl(
	struct mf_file *file,
	struct avi_state *state,
	const unsigned char *data,
	size_t size)
{
	struct avi_stream *stream;
	struct mf_track *info;
	const unsigned char *strh;
	const unsigned char *strf;
	size_t strf_size;
	size_t position;
	size_t length;
	int compared;

	/* The stream's number is its place. */
	if (state->stream_count == AVI_STREAMS_MAX)
		return 0;
	stream = &state->streams[state->stream_count];
	memset(stream, 0, sizeof(*stream));
	stream->track = -1;
	state->stream_count++;

	/* Its strh and strf. */
	strh = NULL;
	strf = NULL;
	strf_size = 0;
	for (position = 0; position + 8U <= size; position += 8U + length + (length & 1U)) {
		/* A chunk's length; one running past the list ends it. */
		length = le32(data + position + 4);
		if (length > size - position - 8U)
			break;

		/* The header, 48 bytes at least. */
		compared = memcmp(data + position, "strh", 4);
		if (compared == 0 && length >= 48U)
			strh = data + position + 8;

		/* The format. */
		compared = memcmp(data + position, "strf", 4);
		if (compared == 0) {
			strf = data + position + 8;
			strf_size = length;
		}
	}

	/* A stream without both, or without a rate, is not read. */
	if (strh == NULL || strf == NULL)
		return 0;
	stream->scale = le32(strh + 20);
	stream->rate = le32(strh + 24);
	stream->sample_size = le32(strh + 44);
	if (stream->scale == 0 ||
	    stream->rate == 0 ||
	    file->track_count == MF_TRACK_MAX)
		return 0;

	/* Its track, by its type: video or sound. */
	info = &file->tracks[file->track_count];
	memset(info, 0, sizeof(*info));
	compared = memcmp(strh, "vids", 4);
	if (compared == 0) {
		stream->video = 1U;
		read_video_format(info, strf, strf_size);
	}

	/* A sound stream. */
	compared = memcmp(strh, "auds", 4);
	if (compared == 0)
		read_sound_format(info, strf, strf_size);

	/* A codec not known: no track. */
	if (info->codec == MF_CODEC_UNKNOWN) {
		free((void *)info->private_data);
		memset(info, 0, sizeof(*info));
		return 0;
	}

	/* The track. */
	stream->track = (int)file->track_count;
	state->stream_of_track[file->track_count] = state->stream_count - 1U;
	file->track_count++;

	/* Succeeded: the stream is read. */
	return 0;
}

/*
 * Reads a video stream's format (a BITMAPINFOHEADER): its size and codec
 * by its compression's four-character code, the bytes after the header as
 * its private data (MPEG-4's VOL headers).
 */
static void
read_video_format(
	struct mf_track *info,
	const unsigned char *strf,
	size_t size)
{
	static const char *const mpeg4[] = { "XVID", "DIVX", "DX50", "FMP4", "MP4V", "M4S2" };
	static const char *const h264[] = { "H264", "X264", "AVC1", "DAVC" };
	static const char *const hevc[] = { "HEVC", "H265", "HVC1" };
	const unsigned char *code;
	int32_t height;
	size_t i;
	int same;

	/* The header's 40 bytes. */
	if (size < 40U)
		return;
	info->kind = MF_TRACK_VIDEO;
	info->width = le32(strf + 4);
	height = (int32_t)le32(strf + 8);
	if (height < 0)
		height = -height;
	info->height = (uint32_t)height;
	code = strf + 16;
	mf_set_codec_name(info, (const char *)code, 4U);

	/* MPEG-4 Part 2 by its codes, in any case. */
	for (i = 0; i < sizeof(mpeg4) / sizeof(mpeg4[0]); i++) {
		same = same_code(code, mpeg4[i]);
		if (same)
			info->codec = MF_CODEC_MPEG4;
	}

	/* H.264. */
	for (i = 0; i < sizeof(h264) / sizeof(h264[0]); i++) {
		same = same_code(code, h264[i]);
		if (same)
			info->codec = MF_CODEC_H264;
	}

	/* H.265. */
	for (i = 0; i < sizeof(hevc) / sizeof(hevc[0]); i++) {
		same = same_code(code, hevc[i]);
		if (same)
			info->codec = MF_CODEC_HEVC;
	}

	/* Motion JPEG. */
	same = same_code(code, "MJPG");
	if (same)
		info->codec = MF_CODEC_MJPEG;

	/* MPEG-4's headers after the format (H.264 in AVI carries its own in the stream). */
	if (info->codec == MF_CODEC_MPEG4 && size > 40U)
		(void)mf_keep_private(info, strf + 40, size - 40U);
}

/*
 * Reads a sound stream's format (a WAVEFORMATEX): its codec by its format
 * tag, its rate and channels, and AAC's AudioSpecificConfig after it.
 */
static void
read_sound_format(
	struct mf_track *info,
	const unsigned char *strf,
	size_t size)
{
	unsigned tag;
	unsigned bits;
	size_t extra;

	/* The format's 16 bytes. */
	if (size < 16U)
		return;
	info->kind = MF_TRACK_AUDIO;
	tag = le16(strf);
	info->channels = le16(strf + 2);
	info->sample_rate = le32(strf + 4);
	bits = le16(strf + 14);

	/* The codec by its tag; PCM's name says its samples. */
	if (tag == AVI_FORMAT_PCM) {
		info->codec = MF_CODEC_PCM;
		if (bits == 8U)
			mf_set_codec_name(info, "pcm_u8", 6U);
		else if (bits == 24U)
			mf_set_codec_name(info, "pcm_s24le", 9U);
		else
			mf_set_codec_name(info, "pcm_s16le", 9U);
	} else if (tag == AVI_FORMAT_MP3) {
		info->codec = MF_CODEC_MP3;
		mf_set_codec_name(info, "mp3", 3U);
	} else if (tag == AVI_FORMAT_AAC || tag == AVI_FORMAT_AAC_LATM) {
		info->codec = MF_CODEC_AAC;
		mf_set_codec_name(info, "aac", 3U);
	} else if (tag == AVI_FORMAT_ADTS) {
		info->codec = MF_CODEC_AAC;
		mf_set_codec_name(info, "adts", 4U);
	}

	/* AAC's AudioSpecificConfig, the extra bytes after the 18 of the format. */
	if (tag != AVI_FORMAT_AAC || size < 18U)
		return;
	extra = le16(strf + 16);
	if (extra != 0 && extra <= size - 18U)
		(void)mf_keep_private(info, strf + 18, extra);
}

/* Tells whether a four-character code is a name, in any case. */
static int
same_code(
	const unsigned char *code,
	const char *name)
{
	unsigned i;
	unsigned letter;

	/* Each character, the code's made upper case. */
	for (i = 0; i < 4U; i++) {
		letter = code[i];
		if (letter >= 'a' && letter <= 'z')
			letter -= 'a' - 'A';
		if (letter != (unsigned char)name[i])
			return 0;
	}

	/* The same. */
	return 1;
}

/*
 * Reads the idx1 index: each entry of a stream read is a packet, its
 * offset from the movi list or from the file's start (the first entry
 * tells which: the one at which its chunk's code is).  Returns 0, ENOENT
 * for an index that cannot be used (the chunks are read instead), ENOMEM,
 * or the reading's error.
 */
static int
read_idx1(
	struct mf_file *file,
	struct avi_state *state,
	const struct avi_chunk *idx1,
	uint64_t movi)
{
	unsigned char entry[16];
	unsigned char code[4];
	uint64_t entries;
	uint64_t base;
	uint64_t i;
	uint64_t offset;
	uint64_t size;
	uint32_t flags;
	unsigned stream;
	int compared;
	int known;
	int key;
	int error;

	/* The entries (16 bytes each) the file holds. */
	entries = idx1->size / 16U;
	if (idx1->data + entries * 16U > file->size)
		entries = (file->size - idx1->data) / 16U;
	if (entries == 0)
		return ENOENT;

	/* The base: the movi list's type (the usual), else the file's start. */
	error = mf_read_at(file, idx1->data, entry, 16U);
	if (error != 0)
		return error;
	base = movi;
	error = mf_read_at(file, movi + le32(entry + 8), code, 4U);
	compared = 1;
	if (error == 0)
		compared = memcmp(code, entry, 4);
	if (compared != 0) {
		base = 0;
		error = mf_read_at(file, le32(entry + 8), code, 4U);
		if (error != 0)
			return ENOENT;
		compared = memcmp(code, entry, 4);
		if (compared != 0)
			return ENOENT;
	}

	/* Each entry of a stream read. */
	for (i = 0; i < entries; i++) {
		/* The entry. */
		error = mf_read_at(file, idx1->data + i * 16U, entry, 16U);
		if (error != 0)
			return error;
		known = stream_of_id((const char *)entry, &stream);
		if (!known)
			continue;

		/* A stream read. */
		if (stream >= state->stream_count || state->streams[stream].track < 0)
			continue;

		/* Its packet: the chunk's data after its 8-byte header, its size and its key frame flag. */
		offset = base + le32(entry + 8) + 8U;
		size = le32(entry + 12);
		flags = le32(entry + 4);
		key = 0;
		if ((flags & AVI_KEYFRAME) != 0U)
			key = 1;
		error = add_packet(file, state, stream, offset, size, key);
		if (error != 0)
			return error;
	}

	/* Succeeded: the packets are known. */
	return 0;
}

/*
 * Reads the chunks between two positions: a data chunk of a stream read
 * is a packet, a movi or rec list is read inside (twice deep at most),
 * the rest is passed over.  Returns 0, ENOMEM or the reading's error.
 */
static int
scan_movi(
	struct mf_file *file,
	struct avi_state *state,
	uint64_t start,
	uint64_t end,
	int depth)
{
	struct avi_chunk chunk;
	unsigned char type[4];
	uint64_t position;
	unsigned stream;
	int compared;
	int known;
	int key;
	int error;

	/* Each chunk; a damaged one ends the reading. */
	if (end > file->size)
		end = file->size;
	for (position = start; position + 8U <= end; position = chunk_next(&chunk)) {
		/* The chunk. */
		error = chunk_at(file, position, end, &chunk);
		if (error != 0)
			return 0;

		/* A list: movi and rec are read inside. */
		compared = memcmp(chunk.id, "LIST", 4);
		if (compared == 0) {
			if (chunk.size < 4U || depth >= 2)
				continue;
			error = mf_read_at(file, chunk.data, type, 4U);
			if (error != 0)
				return 0;
			compared = memcmp(type, "movi", 4);
			if (compared != 0)
				compared = memcmp(type, "rec ", 4);
			if (compared != 0)
				continue;
			error = scan_movi(file, state, chunk.data + 4U, chunk.data + chunk.size, depth + 1);
			if (error != 0)
				return error;
			continue;
		}

		/* A data chunk of a stream read (dc, db, wb). */
		known = stream_of_id(chunk.id, &stream);
		if (!known)
			continue;

		/* A stream read. */
		if (stream >= state->stream_count || state->streams[stream].track < 0)
			continue;

		/* Whether a decoder can start at it, from its bytes. */
		key = chunk_key(file, &file->tracks[state->streams[stream].track], chunk.data, chunk.size);
		error = add_packet(file, state, stream, chunk.data, chunk.size, key);
		if (error != 0)
			return error;
	}

	/* Succeeded: the chunks are read. */
	return 0;
}

/*
 * Reads a data chunk's code: two digits of its stream and dc, db (video)
 * or wb (sound).  Returns 1 with the stream's number, 0 for another chunk.
 */
static int
stream_of_id(
	const char *id,
	unsigned *stream)
{
	int data;

	/* Two digits. */
	if (id[0] < '0' || id[0] > '9')
		return 0;
	if (id[1] < '0' || id[1] > '9')
		return 0;

	/* Video or sound data. */
	data = is_data_id(id);
	if (!data)
		return 0;

	/* The stream's number. */
	*stream = (unsigned)(id[0] - '0') * 10U + (unsigned)(id[1] - '0');
	return 1;
}

/* Tells whether a chunk's last two characters are dc, db (video) or wb (sound). */
static int
is_data_id(
	const char *id)
{
	/* Video: dc (compressed) or db (uncompressed). */
	if (id[2] == 'd' && id[3] == 'c')
		return 1;
	if (id[2] == 'd' && id[3] == 'b')
		return 1;

	/* Sound: wb. */
	if (id[2] == 'w' && id[3] == 'b')
		return 1;

	/* Another chunk. */
	return 0;
}

/*
 * Adds a packet of a stream at the end of the list, timed by the stream's
 * units counted so far; one outside the file (or too large) is counted as
 * left out, its units still counted.  Returns 0, EINVAL for too many
 * packets, or ENOMEM.
 */
static int
add_packet(
	struct mf_file *file,
	struct avi_state *state,
	unsigned stream_number,
	uint64_t offset,
	uint64_t size,
	int key)
{
	struct avi_stream *stream;
	struct avi_packet *packets;
	struct mf_track *info;
	size_t capacity;
	int64_t time;

	/* The stream's time for this packet, and the units it adds. */
	stream = &state->streams[stream_number];
	info = &file->tracks[stream->track];
	time = (int64_t)stream->counted;
	if (stream->video || stream->sample_size == 0) {
		stream->counted++;
	} else {
		stream->bytes += size;
		stream->counted = stream->bytes / stream->sample_size;
	}

	/* An empty chunk (a frame dropped: the one before is shown again) is time without a packet. */
	if (size == 0)
		return 0;

	/* A packet outside the file, or too large, is left out. */
	if (offset > file->size ||
	    size > file->size - offset ||
	    size > MF_PACKET_MAX) {
		info->dropped_count++;
		return 0;
	}

	/* Room: twice the list. */
	if (state->count == state->capacity) {
		if (state->capacity >= AVI_PACKETS_MAX)
			return EINVAL;
		capacity = state->capacity * 2U;
		if (capacity < 1024U)
			capacity = 1024U;
		packets = realloc(state->packets, capacity * sizeof(*packets));
		if (packets == NULL)
			return ENOMEM;
		state->packets = packets;
		state->capacity = capacity;
	}

	/* The packet. */
	state->packets[state->count].offset = offset;
	state->packets[state->count].size = (uint32_t)size;
	state->packets[state->count].track = (uint8_t)stream->track;
	state->packets[state->count].key = (uint8_t)(key != 0);
	state->packets[state->count].time = time;
	state->count++;
	info->packet_count++;

	/* Succeeded: the packet is listed. */
	return 0;
}

/*
 * Tells from a chunk's first bytes whether a decoder can start at it, for
 * a file without an index: an H.264 IDR picture, an MPEG-4 I-VOP; every
 * other frame and all sound can.
 */
static int
chunk_key(
	struct mf_file *file,
	const struct mf_track *info,
	uint64_t offset,
	uint64_t size)
{
	unsigned char bytes[512];
	size_t length;
	size_t i;
	int error;

	/* Sound, and video without inner frames. */
	if (info->kind != MF_TRACK_VIDEO)
		return 1;
	if (info->codec != MF_CODEC_H264 && info->codec != MF_CODEC_MPEG4)
		return 1;

	/* The chunk's first bytes; an empty frame repeats the one before. */
	if (size == 0)
		return 0;
	length = sizeof(bytes);
	if (size < length)
		length = (size_t)size;
	error = mf_read_at(file, offset, bytes, length);
	if (error != 0)
		return 0;

	/* The first slice or VOP after a start code. */
	for (i = 0; i + 4U < length; i++) {
		/* A start code. */
		if (bytes[i] != 0 || bytes[i + 1] != 0)
			continue;
		if (bytes[i + 2] != 1U)
			continue;

		/* H.264: an IDR slice (5) is key, another slice (1) is not. */
		if (info->codec == MF_CODEC_H264 && (bytes[i + 3] & 0x1fU) == 5U)
			return 1;
		if (info->codec == MF_CODEC_H264 && (bytes[i + 3] & 0x1fU) == 1U)
			return 0;

		/* MPEG-4: a VOP (B6) is key when its coding type (the next two bits) is I (0). */
		if (info->codec == MF_CODEC_MPEG4 && bytes[i + 3] == 0xb6U)
			return (bytes[i + 4] >> 6) == 0;
	}

	/* No picture found: not known as a key frame. */
	return 0;
}

/* Turns a packet's time in its stream's units into microseconds. */
static int64_t
packet_us(
	const struct avi_state *state,
	const struct avi_packet *packet)
{
	const struct avi_stream *stream;
	int64_t time;

	/* The units at the stream's rate over its scale. */
	stream = &state->streams[state->stream_of_track[packet->track]];
	time = mf_scale_us(packet->time * (int64_t)stream->scale, stream->rate);
	return time;
}
