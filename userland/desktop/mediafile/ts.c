/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The MPEG-TS reader (ws177-p028): transport streams of 188-byte packets,
 * and of 192-byte ones with a time code before each (M2TS, Blu-ray and
 * AVCHD).  The first program of the PAT is read; its PMT names the tracks
 * (H.264, H.265, AAC in ADTS, MPEG audio).  Each track's PES packets are
 * put together from the transport packets of its PID: a video PES is one
 * packet (an Annex B access unit, which a decoder reads as it is), an
 * audio PES is cut into its ADTS or MPEG audio frames, one packet each.
 *
 * Times are the PES's 90 kHz PTS and DTS, counted from the earliest PTS
 * at the start of the file (a stream may start anywhere on the 33-bit
 * clock, and may wrap once).  There is no index: the length comes from the
 * last PTS near the end of the file, and a seek looks for its place by
 * halving the file on the times of the first video track's PES packets,
 * then goes back to the key frame before it (the adaptation field's random
 * access indicator, or an IDR or IRAP picture).  A packet whose sync byte
 * is lost is found again; a PES left unfinished by lost packets or by the
 * end of the file is left out and counted (mf_track.dropped_count).
 */

#include "mediafile-private.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>

/* The two sizes of a transport packet: plain, and with a 4-byte time code first (M2TS). */
#define TS_PACKET		188U
#define TS_PACKET_M2TS		192U

/* The byte every transport packet starts with. */
#define TS_SYNC			0x47U

/* The PID of the program association table. */
#define TS_PID_PAT		0x0000U

/* The table IDs of the PAT and a PMT. */
#define TS_TABLE_PAT		0x00U
#define TS_TABLE_PMT		0x02U

/* The stream types of a PMT this reader knows. */
#define TS_TYPE_MPEG1_AUDIO	0x03U
#define TS_TYPE_MPEG2_AUDIO	0x04U
#define TS_TYPE_ADTS		0x0fU
#define TS_TYPE_H264		0x1bU
#define TS_TYPE_HEVC		0x24U

/* A private stream: its kind is told from its first PES (AAC in ADTS, as some writers send it). */
#define TS_TYPE_PRIVATE		0x06U

/* The bytes read at the start of the file to find the tables and each track's first PES. */
#define TS_PROBE_MAX		(8U * 1024U * 1024U)

/* The bytes at the end of the file read to find the last time. */
#define TS_TAIL_MAX		(2U * 1024U * 1024U)

/* The bytes after a place a seek reads to find the first video PES's time there. */
#define TS_SEEK_SCAN		(4U * 1024U * 1024U)

/* The first stretch before a seek's place read for its key frame (doubled until one is found). */
#define TS_SEEK_WINDOW		(256U * 1024U)

/* The bytes read from the file at once into the reader's block. */
#define TS_BLOCK		(64U * 1024U)

/* The largest PES kept; a larger one is left out. */
#define TS_PES_MAX		(16U * 1024U * 1024U)

/* The clock of the PTS and DTS (90 kHz) and where it wraps (33 bits). */
#define TS_CLOCK		90000U
#define TS_WRAP			((int64_t)1 << 33)

/*
 * How far before the start a time may lie (a B-frame's DTS, a track that
 * starts earlier) before it is read as a time after the clock wrapped:
 * about three hours.
 */
#define TS_BEHIND		((int64_t)1 << 30)

/* A time that is not known. */
#define TS_NO_TIME		INT64_MIN

/* The samples of an AAC frame (each raw data block). */
#define TS_AAC_SAMPLES		1024U

/*
 * One track: its PID and stream type, the PES being put together (its
 * bytes, whether its first packet was seen, whether that packet's
 * adaptation field said a decoder can start there, whether its length says
 * it is whole, and whether a packet of it was lost), the continuity counter
 * of its last packet (-1 before the first), and the first and latest PTS
 * seen (raw, 90 kHz).
 */
struct ts_track {
	uint16_t pid;
	unsigned stream_type;
	unsigned char *pes;
	size_t pes_size;
	size_t pes_capacity;
	unsigned pes_open;
	unsigned pes_random;
	unsigned pes_full;
	unsigned pes_damaged;
	int counter;
	int64_t first_pts;
	int64_t last_pts;
};

/*
 * A finished PES being handed out: its track, its payload (in the
 * reader's ready buffer, which a finished PES's bytes are swapped into),
 * the next byte of it, its times (raw, 90 kHz), whether a decoder can
 * start at it, and the samples of the audio frames already handed out.
 */
struct ts_ready {
	unsigned active;
	unsigned track;
	unsigned char *buffer;
	size_t capacity;
	const unsigned char *data;
	size_t size;
	size_t next;
	int64_t pts;
	int64_t dts;
	unsigned key;
	uint64_t samples;
};

/*
 * A walk over the transport packets of the first video track's PID, as a
 * seek reads them: whether a PES is open and where it started, its time,
 * whether it is known to be a key frame, and the NAL scanner's state
 * (zero bytes seen, whether the next byte is a NAL header, whether the
 * picture's first slice was found).
 */
struct ts_walk {
	unsigned open;
	uint64_t position;
	int64_t time;
	unsigned key;
	unsigned zeros;
	unsigned header_next;
	unsigned decided;
};

/*
 * The reader's state: the packet size and where the sync byte is in a
 * packet, the offset of the first packet, the next packet to read, the
 * PMT's PID and whether it was read, the time the file starts at (raw),
 * the track that decides a seek, the tracks, the PES being handed out,
 * and a block of the file read at once.
 */
struct ts_state {
	unsigned packet_size;
	unsigned sync_offset;
	uint64_t base;
	uint64_t position;
	uint16_t pmt_pid;
	unsigned have_pat;
	unsigned have_pmt;
	int64_t start;
	unsigned lead;
	struct ts_track tracks[MF_TRACK_MAX];
	struct ts_ready ready;
	unsigned char *block;
	uint64_t block_offset;
	size_t block_size;
};

/*
 * One transport packet's header as the reader uses it: its PID, whether a
 * PES or section starts in it, whether its adaptation field says a decoder
 * can start there, its continuity counter (which counts the PID's packets,
 * so a lost one shows) and whether the adaptation field says the count
 * starts again, and its payload.
 */
struct ts_packet {
	uint16_t pid;
	unsigned unit_start;
	unsigned random;
	unsigned counter;
	unsigned discontinuity;
	const unsigned char *payload;
	size_t payload_size;
};

/*
 * A reader of the bits of a NAL unit's payload, with the emulation
 * prevention bytes taken out: the bytes, their count, the next bit, and
 * whether a read ran past the end.
 */
struct ts_bits {
	unsigned char bytes[256];
	size_t size;
	size_t bit;
	unsigned failed;
};

/* The sampling rates an ADTS header can name, by index. */
static const uint32_t ts_aac_rates[13] = {
	96000U, 88200U, 64000U, 48000U, 44100U, 32000U, 24000U, 22050U, 16000U, 12000U, 11025U, 8000U, 7350U
};

/* MPEG audio's bit rates in kbit/s, by version (1, then 2 and 2.5), layer (I, II, III) and index. */
static const uint16_t ts_mpeg_bitrates[2][3][15] = {
	{
		{ 0, 32, 64, 96, 128, 160, 192, 224, 256, 288, 320, 352, 384, 416, 448 },
		{ 0, 32, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384 },
		{ 0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320 },
	},
	{
		{ 0, 32, 48, 56, 64, 80, 96, 112, 128, 144, 160, 176, 192, 224, 256 },
		{ 0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160 },
		{ 0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160 },
	},
};

/* MPEG audio's sampling rates of version 1, by index (version 2 halves them, 2.5 quarters them). */
static const uint32_t ts_mpeg_rates[3] = { 44100U, 48000U, 32000U };

static int ts_open(struct mf_file *file);
static int ts_read(struct mf_file *file, struct mf_packet *packet);
static int ts_seek(struct mf_file *file, int64_t time_us);
static void ts_close(struct mf_file *file);
static int find_sync(struct mf_file *file, struct ts_state *state);
static int packet_at(struct mf_file *file, struct ts_state *state, uint64_t offset, const unsigned char **bytes);
static int parse_packet(const struct ts_state *state, const unsigned char *bytes, struct ts_packet *packet);
static int next_packet(struct mf_file *file, struct ts_state *state, struct ts_packet *packet);
static int resync(struct mf_file *file, struct ts_state *state);
static int sync_from(struct mf_file *file, struct ts_state *state, uint64_t offset, uint64_t *found);
static int scan_packet(struct mf_file *file, struct ts_state *state, uint64_t *offset, struct ts_packet *packet);
static void read_pat(struct ts_state *state, const struct ts_packet *packet);
static void read_pmt(struct mf_file *file, struct ts_state *state, const struct ts_packet *packet);
static void add_track(struct mf_file *file, struct ts_state *state, unsigned stream_type, uint16_t pid);
static int track_of(const struct mf_file *file, const struct ts_state *state, uint16_t pid);
static int next_pes(struct mf_file *file, struct ts_state *state);
static int feed(struct mf_file *file, struct ts_state *state, unsigned index, const struct ts_packet *packet, int *finished);
static int pes_append(struct ts_track *track, const unsigned char *data, size_t size);
static void finish_pes(struct mf_file *file, struct ts_state *state, unsigned index);
static int pes_times(const unsigned char *pes, size_t size, size_t *payload, int64_t *pts, int64_t *dts);
static int64_t pes_time(const unsigned char *bytes);
static void discard_pes(struct ts_state *state);
static int probe(struct mf_file *file, struct ts_state *state);
static void probe_info(struct mf_file *file, struct ts_state *state);
static void find_length(struct mf_file *file, struct ts_state *state);
static int64_t relative(const struct ts_state *state, int64_t raw);
static int64_t time_us(const struct ts_state *state, int64_t raw);
static int hand_out_audio(struct mf_file *file, struct ts_state *state, struct mf_packet *packet);
static int adts_frame(const unsigned char *data, size_t size, size_t *length, uint32_t *rate, uint32_t *channels, uint32_t *samples);
static int mpeg_audio_frame(const unsigned char *data, size_t size, size_t *length, uint32_t *rate, uint32_t *channels, uint32_t *samples);
static int is_key_picture(unsigned codec, const unsigned char *data, size_t size);
static int nal_feed(struct ts_walk *walk, unsigned codec, const unsigned char *data, size_t size);
static int key_nal(unsigned codec, unsigned header, unsigned *key);
static int h264_size(const unsigned char *data, size_t size, uint32_t *width, uint32_t *height);
static int is_high_profile(unsigned profile);
static void h264_high_profile(struct ts_bits *bits, unsigned *chroma);
static void h264_order(struct ts_bits *bits);
static void bits_load(struct ts_bits *bits, const unsigned char *nal, size_t size);
static unsigned bits_read(struct ts_bits *bits, unsigned count);
static uint32_t bits_golomb(struct ts_bits *bits);
static void bits_skip_scaling(struct ts_bits *bits, unsigned size);
static int find_h264_sps(const unsigned char *data, size_t size, const unsigned char **sps, size_t *sps_size);
static int lead_time_at(struct mf_file *file, struct ts_state *state, uint64_t offset, int64_t *time);
static int find_key(struct mf_file *file, struct ts_state *state, uint64_t from, uint64_t limit, int64_t target, uint64_t *found);
static void walk_finish(const struct ts_walk *walk, int64_t target, uint64_t *found, unsigned *have);

/* The reader as mediafile.c calls it. */
const struct mf_format mf_ts_format = {
	"mpegts",
	ts_open,
	ts_read,
	ts_seek,
	ts_close,
};

/*
 * Tells whether a file's first bytes are a transport stream: a sync byte
 * at the start of three packets in a row, of 188 bytes or of 192 with the
 * time code first.
 */
int
mf_ts_detect(
	const unsigned char *head,
	size_t length)
{
	/* Three plain packets. */
	if (length >= 2U * TS_PACKET + 1U &&
	    head[0] == TS_SYNC &&
	    head[TS_PACKET] == TS_SYNC &&
	    head[2U * TS_PACKET] == TS_SYNC)
		return 1;

	/* Three M2TS packets, each sync byte after its time code. */
	if (length >= 2U * TS_PACKET_M2TS + 5U &&
	    head[4] == TS_SYNC &&
	    head[TS_PACKET_M2TS + 4U] == TS_SYNC &&
	    head[2U * TS_PACKET_M2TS + 4U] == TS_SYNC)
		return 1;

	/* Not a transport stream. */
	return 0;
}

/*
 * Finds the packets, the tables and the tracks, each track's first PES
 * (for its picture size or sound format, and the time the file starts
 * at), and the file's length.
 */
static int
ts_open(
	struct mf_file *file)
{
	struct ts_state *state;
	int error;

	/* The state, kept in the file so that mf_close frees it on any failure. */
	state = calloc(1, sizeof(*state));
	if (state == NULL)
		return ENOMEM;
	file->state = state;

	/* The block the packets are read through. */
	state->block = malloc(TS_BLOCK);
	if (state->block == NULL)
		return ENOMEM;

	/* The packet size and the first packet. */
	error = find_sync(file, state);
	if (error != 0)
		return error;

	/* The tables, the tracks and their first PES. */
	error = probe(file, state);
	if (error != 0)
		return error;

	/* Refuses a stream without a track this reader knows. */
	if (file->track_count == 0)
		return EINVAL;

	/* The length, from the last time near the end. */
	find_length(file, state);

	/* Reading starts again at the first packet. */
	state->position = state->base;
	discard_pes(state);

	/* Succeeded: the tracks are known. */
	return 0;
}

/*
 * Hands out the next packet: the next frame of an audio PES being handed
 * out, else the next finished PES (a video PES whole, an audio PES's first
 * frame).
 */
static int
ts_read(
	struct mf_file *file,
	struct mf_packet *packet)
{
	struct ts_state *state;
	const struct mf_track *info;
	int error;

	/* The PES being handed out, or the next one. */
	state = file->state;
	for (;;) {
		/* A finished PES, else the next one in the file (ENODATA at the end). */
		if (!state->ready.active) {
			error = next_pes(file, state);
			if (error != 0)
				return error;
		}

		/* An audio PES is handed out a frame at a time; a damaged one is let go. */
		info = &file->tracks[state->ready.track];
		if (info->kind == MF_TRACK_AUDIO) {
			error = hand_out_audio(file, state, packet);
			if (error == EAGAIN)
				continue;
			if (error != 0)
				return error;

			/* Succeeded: one audio frame. */
			return 0;
		}

		/* Anything else goes whole. */
		break;
	}

	/* The PES's bytes. */
	error = mf_packet_room(file, state->ready.size);
	if (error != 0)
		return error;

	/* The packet, and the PES is done. */
	memcpy(file->buffer, state->ready.data, state->ready.size);
	packet->track = state->ready.track;
	packet->pts_us = time_us(state, state->ready.pts);
	packet->dts_us = time_us(state, state->ready.dts);
	packet->keyframe = (int)state->ready.key;
	packet->data = file->buffer;
	packet->size = state->ready.size;
	state->ready.active = 0;

	/* Succeeded: one PES. */
	return 0;
}

/*
 * Moves to the last key frame of the first video track (else the first
 * track) presented at or before the time: halves the file on its PES
 * times, then reads back from the place found until a key frame before the
 * time is found.
 */
static int
ts_seek(
	struct mf_file *file,
	int64_t time_us)
{
	struct ts_state *state;
	uint64_t count;
	uint64_t low;
	uint64_t high;
	uint64_t middle;
	uint64_t place;
	uint64_t window;
	uint64_t from;
	uint64_t found;
	int64_t target;
	int64_t time;
	int error;

	/* Nothing half-read is kept, and the time in the clock's units. */
	state = file->state;
	discard_pes(state);
	target = time_us / 100 * 9 + time_us % 100 * 9 / 100;

	/* The packets of the file. */
	count = (file->size - state->base) / state->packet_size;

	/* The last packet from which the first PES of the lead starts at or before the time. */
	low = 0;
	high = count;
	while (high - low > 1U) {
		/* The middle packet's first lead PES; none, or a later one, moves the end down. */
		middle = low + (high - low) / 2U;
		error = lead_time_at(file, state, state->base + middle * state->packet_size, &time);
		if (error != 0 && error != ENODATA)
			return error;
		if (error == ENODATA || time > target)
			high = middle;
		else
			low = middle;
	}

	/* The key frame before the time: a growing stretch before the place is read for it. */
	place = state->base + low * state->packet_size;
	found = state->base;
	window = TS_SEEK_WINDOW;
	for (;;) {
		/* The stretch's start, on a packet. */
		from = state->base;
		if (place - state->base > window)
			from = place - (window / state->packet_size) * state->packet_size;

		/* Its last key frame at or before the time. */
		error = find_key(file, state, from, place + TS_SEEK_SCAN, target, &found);
		if (error != 0 && error != ENOENT)
			return error;
		if (error == 0) {
			state->position = found;
			return 0;
		}

		/* The start of the file is as far back as there is. */
		if (from == state->base)
			break;

		/* A longer stretch. */
		window *= 2U;
	}

	/* Succeeded: no key frame before the time, so the file plays from its start. */
	state->position = state->base;
	return 0;
}

/*
 * Lets the reader's state go.
 */
static void
ts_close(
	struct mf_file *file)
{
	struct ts_state *state;
	unsigned i;

	/* Each track's PES, the ready buffer, the block and the state. */
	state = file->state;
	for (i = 0; i < MF_TRACK_MAX; i++)
		free(state->tracks[i].pes);

	/* The buffers and the state. */
	free(state->ready.buffer);
	free(state->block);
	free(state);
	file->state = NULL;
}

/*
 * Finds the packet size and the first packet: the first offset (within one
 * packet of the start) at which three packets in a row have their sync
 * byte.  Returns 0, or EINVAL when there is none.
 */
static int
find_sync(
	struct mf_file *file,
	struct ts_state *state)
{
	static const unsigned sizes[2] = { TS_PACKET, TS_PACKET_M2TS };
	unsigned char head[3U * TS_PACKET_M2TS + 4U];
	size_t length;
	unsigned size;
	unsigned sync;
	unsigned offset;
	unsigned i;
	int error;

	/* The first bytes of the file. */
	length = sizeof(head);
	if (file->size < length)
		length = (size_t)file->size;
	error = mf_read_at(file, 0, head, length);
	if (error != 0)
		return error;

	/* Each packet size, its sync byte first or after a time code. */
	for (i = 0; i < 2U; i++) {
		/* The sync byte's place in a packet. */
		size = sizes[i];
		sync = 0;
		if (size == TS_PACKET_M2TS)
			sync = 4U;

		/* Each first offset within a packet. */
		for (offset = 0; offset < size; offset++) {
			/* Three sync bytes in a row, a packet apart. */
			if ((size_t)offset + sync + 2U * size >= length)
				break;
			if (head[offset + sync] != TS_SYNC)
				continue;
			if (head[offset + sync + size] != TS_SYNC)
				continue;
			if (head[offset + sync + 2U * size] != TS_SYNC)
				continue;

			/* The packets start here. */
			state->packet_size = size;
			state->sync_offset = sync;
			state->base = offset;
			state->position = offset;
			return 0;
		}
	}

	/* No run of packets at the start. */
	return EINVAL;
}

/*
 * Points *bytes at the packet at an offset of the file, through the
 * reader's block.  Returns 0, ENODATA when the file ends before the whole
 * packet, or the reading's error.
 */
static int
packet_at(
	struct mf_file *file,
	struct ts_state *state,
	uint64_t offset,
	const unsigned char **bytes)
{
	size_t length;
	int error;

	/* The end of the file, a packet cut short included. */
	if (offset > file->size || file->size - offset < state->packet_size)
		return ENODATA;

	/* Read through the block when it does not hold the packet. */
	if (offset < state->block_offset ||
	    offset + state->packet_size > state->block_offset + state->block_size) {
		/* As much of the file from the packet as the block holds. */
		length = TS_BLOCK;
		if (file->size - offset < length)
			length = (size_t)(file->size - offset);
		error = mf_read_at(file, offset, state->block, length);
		if (error != 0) {
			state->block_size = 0;
			return error;
		}

		/* The block now holds it. */
		state->block_offset = offset;
		state->block_size = length;
	}

	/* Succeeded: the packet's bytes. */
	*bytes = state->block + (size_t)(offset - state->block_offset);
	return 0;
}

/*
 * Reads a transport packet's header: its PID, whether a unit starts in it,
 * the adaptation field's random access indicator, and its payload.
 * Returns 0, EINVAL when its sync byte is lost, or ENOENT for a packet
 * without a payload this reader uses (none, or marked with an error).
 */
static int
parse_packet(
	const struct ts_state *state,
	const unsigned char *bytes,
	struct ts_packet *packet)
{
	const unsigned char *header;
	unsigned control;
	size_t offset;
	size_t adaptation;

	/* The header after the time code of an M2TS packet. */
	header = bytes + state->sync_offset;
	if (header[0] != TS_SYNC)
		return EINVAL;

	/* A packet the sender marked as damaged carries nothing to use. */
	memset(packet, 0, sizeof(*packet));
	if ((header[1] & 0x80U) != 0U)
		return ENOENT;

	/* The PID, the unit start and whether there is an adaptation field and a payload. */
	packet->pid = (uint16_t)(((unsigned)(header[1] & 0x1fU) << 8) | header[2]);
	packet->unit_start = (header[1] >> 6) & 1U;
	packet->counter = header[3] & 0x0fU;
	control = (header[3] >> 4) & 3U;
	offset = 4U;

	/* The adaptation field: its length, and the random access indicator in its flags. */
	if ((control & 2U) != 0U) {
		adaptation = header[4];
		if (adaptation > TS_PACKET - 5U)
			return ENOENT;
		if (adaptation != 0 && (header[5] & 0x40U) != 0U)
			packet->random = 1U;
		if (adaptation != 0 && (header[5] & 0x80U) != 0U)
			packet->discontinuity = 1U;
		offset = 5U + adaptation;
	}

	/* No payload. */
	if ((control & 1U) == 0U || offset >= TS_PACKET)
		return ENOENT;

	/* Succeeded: the payload is the rest of the packet. */
	packet->payload = header + offset;
	packet->payload_size = TS_PACKET - offset;
	return 0;
}

/*
 * Reads the next transport packet with a payload, finding the packets
 * again after a lost sync byte.  Returns 0, ENODATA at the end of the
 * file, or the reading's error.
 */
static int
next_packet(
	struct mf_file *file,
	struct ts_state *state,
	struct ts_packet *packet)
{
	const unsigned char *bytes;
	int error;

	/* Packets until one has a payload. */
	for (;;) {
		/* The packet at the reading's place. */
		error = packet_at(file, state, state->position, &bytes);
		if (error != 0)
			return error;

		/* Its header; a lost sync byte is found again. */
		error = parse_packet(state, bytes, packet);
		if (error == EINVAL) {
			error = resync(file, state);
			if (error != 0)
				return error;
			continue;
		}

		/* The next packet follows. */
		state->position += state->packet_size;

		/* Succeeded: a packet with a payload. */
		if (error == 0)
			return 0;
	}
}

/*
 * Finds the packets again after a lost sync byte: the reading goes on at
 * the next place after the lost packet's start where two packets in a row
 * have theirs.  Returns 0, ENODATA when the file ends first, or the
 * reading's error.
 */
static int
resync(
	struct mf_file *file,
	struct ts_state *state)
{
	uint64_t found;
	int error;

	/* The next place the packets go on. */
	error = sync_from(file, state, state->position + 1U, &found);
	if (error != 0)
		return error;

	/* Succeeded: the reading goes on there. */
	state->position = found;
	return 0;
}

/*
 * Finds the first place at or after an offset where two packets in a row
 * have their sync bytes.  Returns 0 with the place, ENODATA when the file
 * ends first, or the reading's error.
 */
static int
sync_from(
	struct mf_file *file,
	struct ts_state *state,
	uint64_t offset,
	uint64_t *found)
{
	unsigned char *bytes;
	size_t length;
	size_t span;
	size_t i;
	int error;

	/* The bytes from the offset, a block at a time. */
	span = state->packet_size + state->sync_offset + 1U;
	for (;;) {
		/* The end of the file. */
		if (offset >= file->size || file->size - offset < span)
			return ENODATA;

		/* A block of the file. */
		length = TS_BLOCK;
		if (file->size - offset < length)
			length = (size_t)(file->size - offset);
		error = mf_read_at(file, offset, state->block, length);
		if (error != 0) {
			state->block_size = 0;
			return error;
		}

		/* The block holds what was read. */
		state->block_offset = offset;
		state->block_size = length;
		bytes = state->block;

		/* Each place where a packet could start, its sync byte and the next packet's. */
		for (i = 0; i + span <= length; i++) {
			/* Two sync bytes a packet apart. */
			if (bytes[i + state->sync_offset] != TS_SYNC)
				continue;
			if (bytes[i + state->sync_offset + state->packet_size] != TS_SYNC)
				continue;

			/* Succeeded: the packets go on from here. */
			*found = offset + i;
			return 0;
		}

		/* The next block, overlapping this one by what a test needs. */
		offset += length - span + 1U;
	}
}

/*
 * Reads the packet at *offset for a scan of the file (the length, a
 * seek), moving *offset to where the packets go on when its sync byte is
 * lost.  Returns 0, ENOENT for a packet without a payload this reader
 * uses, ENODATA at the end of the file, or the reading's error.
 */
static int
scan_packet(
	struct mf_file *file,
	struct ts_state *state,
	uint64_t *offset,
	struct ts_packet *packet)
{
	const unsigned char *bytes;
	uint64_t found;
	int error;

	/* The packet at the offset and its header. */
	error = packet_at(file, state, *offset, &bytes);
	if (error != 0)
		return error;
	error = parse_packet(state, bytes, packet);
	if (error != EINVAL)
		return error;

	/* Its sync byte is lost: the next place the packets go on. */
	error = sync_from(file, state, *offset + 1U, &found);
	if (error != 0)
		return error;
	*offset = found;

	/* The packet there. */
	error = packet_at(file, state, *offset, &bytes);
	if (error != 0)
		return error;
	error = parse_packet(state, bytes, packet);
	if (error != 0)
		return error;

	/* Succeeded: a packet with a payload. */
	return 0;
}

/*
 * Reads the program association table: the PMT's PID of the first
 * program.  A table spread over more than one packet is not read.
 */
static void
read_pat(
	struct ts_state *state,
	const struct ts_packet *packet)
{
	const unsigned char *section;
	size_t left;
	size_t length;
	size_t i;
	unsigned program;

	/* The section after the pointer field. */
	if (!packet->unit_start || packet->payload_size < 1U)
		return;
	left = packet->payload_size - 1U;
	if (packet->payload[0] > left)
		return;
	section = packet->payload + 1 + packet->payload[0];
	left -= packet->payload[0];

	/* A whole PAT section: its length within the packet, its CRC after the entries. */
	if (left < 12U || section[0] != TS_TABLE_PAT)
		return;
	length = ((size_t)(section[1] & 0x0fU) << 8) | section[2];
	if (length < 9U || 3U + length > left)
		return;

	/* Each program; the first that is not the network's has the PMT. */
	for (i = 8U; i + 4U <= 3U + length - 4U; i += 4U) {
		/* Program 0 names the network information table. */
		program = mf_be16(section + i);
		if (program == 0)
			continue;

		/* The PMT's PID. */
		state->pmt_pid = (uint16_t)(mf_be16(section + i + 2) & 0x1fffU);
		state->have_pat = 1U;
		return;
	}
}

/*
 * Reads the first program's map table: each elementary stream of a type
 * this reader knows becomes a track.  A table spread over more than one
 * packet is not read.
 */
static void
read_pmt(
	struct mf_file *file,
	struct ts_state *state,
	const struct ts_packet *packet)
{
	const unsigned char *section;
	size_t left;
	size_t length;
	size_t end;
	size_t i;
	size_t info;
	unsigned stream_type;
	uint16_t pid;

	/* The section after the pointer field. */
	if (!packet->unit_start || packet->payload_size < 1U)
		return;
	left = packet->payload_size - 1U;
	if (packet->payload[0] > left)
		return;
	section = packet->payload + 1 + packet->payload[0];
	left -= packet->payload[0];

	/* A whole PMT section, its length within the packet. */
	if (left < 16U || section[0] != TS_TABLE_PMT)
		return;
	length = ((size_t)(section[1] & 0x0fU) << 8) | section[2];
	if (length < 13U || 3U + length > left)
		return;

	/* The streams after the program's descriptors, up to the CRC. */
	info = ((size_t)(section[10] & 0x0fU) << 8) | section[11];
	end = 3U + length - 4U;
	state->have_pmt = 1U;
	for (i = 12U + info; i + 5U <= end; i += 5U + info) {
		/* The stream's type, PID and the length of its descriptors. */
		stream_type = section[i];
		pid = (uint16_t)(mf_be16(section + i + 1) & 0x1fffU);
		info = ((size_t)(section[i + 3] & 0x0fU) << 8) | section[i + 4];

		/* A stream of a known type is a track. */
		add_track(file, state, stream_type, pid);
	}
}

/*
 * Adds a track for a stream of the PMT when its type is one this reader
 * knows and there is room.
 */
static void
add_track(
	struct mf_file *file,
	struct ts_state *state,
	unsigned stream_type,
	uint16_t pid)
{
	struct ts_track *track;
	struct mf_track *info;

	/* No more room. */
	if (file->track_count == MF_TRACK_MAX)
		return;

	/* The slot of the next track. */
	track = &state->tracks[file->track_count];
	info = &file->tracks[file->track_count];
	memset(info, 0, sizeof(*info));

	/* The kind and codec of each stream type known. */
	switch (stream_type) {
	case TS_TYPE_H264:
		info->kind = MF_TRACK_VIDEO;
		info->codec = MF_CODEC_H264;
		mf_set_codec_name(info, "h264", 4U);
		break;
	case TS_TYPE_HEVC:
		info->kind = MF_TRACK_VIDEO;
		info->codec = MF_CODEC_HEVC;
		mf_set_codec_name(info, "hevc", 4U);
		break;
	case TS_TYPE_ADTS:
		info->kind = MF_TRACK_AUDIO;
		info->codec = MF_CODEC_AAC;
		mf_set_codec_name(info, "adts", 4U);
		break;
	case TS_TYPE_MPEG1_AUDIO:
	case TS_TYPE_MPEG2_AUDIO:
		info->kind = MF_TRACK_AUDIO;
		info->codec = MF_CODEC_MP3;
		mf_set_codec_name(info, "mpeg-audio", 10U);
		break;
	case TS_TYPE_PRIVATE:
		info->kind = MF_TRACK_OTHER;
		info->codec = MF_CODEC_UNKNOWN;
		mf_set_codec_name(info, "private", 7U);
		break;
	default:
		return;
	}

	/* The track, with no PES and no time yet. */
	track->pid = pid;
	track->stream_type = stream_type;
	track->first_pts = TS_NO_TIME;
	track->last_pts = TS_NO_TIME;
	track->counter = -1;
	file->track_count++;
}

/* Finds the track of a PID: its index, or -1 for a PID of no track. */
static int
track_of(
	const struct mf_file *file,
	const struct ts_state *state,
	uint16_t pid)
{
	unsigned i;

	/* Each track. */
	for (i = 0; i < file->track_count; i++) {
		/* The track of the PID. */
		if (state->tracks[i].pid == pid)
			return (int)i;
	}

	/* No track has it. */
	return -1;
}

/*
 * Reads transport packets until a track's PES is finished and ready to be
 * handed out (state->ready); a PES whose length says it is whole is
 * finished first, and at the end of the file the PES still open are.
 * Returns 0, ENODATA when nothing is left, or the reading's error.
 */
static int
next_pes(
	struct mf_file *file,
	struct ts_state *state)
{
	struct ts_packet packet;
	unsigned i;
	int finished;
	int index;
	int error;

	/* Packets until a PES is finished. */
	for (;;) {
		/* A PES its length says is whole is finished before more is read. */
		for (i = 0; i < file->track_count; i++) {
			/* One that is whole. */
			if (!state->tracks[i].pes_full)
				continue;

			/* Finished; one left out lets the reading go on. */
			finish_pes(file, state, i);
			if (state->ready.active)
				return 0;
		}

		/* The next packet. */
		error = next_packet(file, state, &packet);
		if (error == ENODATA)
			break;
		if (error != 0)
			return error;

		/* The tables, until the first program's PMT has been read. */
		if (!state->have_pat && packet.pid == TS_PID_PAT) {
			read_pat(state, &packet);
			continue;
		}

		/* The first program's PMT, once the PAT has named it. */
		if (state->have_pat &&
		    !state->have_pmt &&
		    packet.pid == state->pmt_pid) {
			read_pmt(file, state, &packet);
			continue;
		}

		/* A packet of a track; others are not read. */
		index = track_of(file, state, packet.pid);
		if (index < 0)
			continue;

		/* Its payload goes into the track's PES; one may be finished by it. */
		error = feed(file, state, (unsigned)index, &packet, &finished);
		if (error != 0)
			return error;
		if (finished)
			return 0;
	}

	/* The end of the file: each PES still open is finished. */
	for (i = 0; i < file->track_count; i++) {
		/* One still open. */
		if (!state->tracks[i].pes_open)
			continue;

		/* Finished; one left out lets the next be tried. */
		finish_pes(file, state, i);
		if (state->ready.active)
			return 0;
	}

	/* Nothing is left. */
	return ENODATA;
}

/*
 * Puts a transport packet's payload into its track's PES: a unit start
 * finishes the PES before it (*finished is set when that one is ready to
 * be handed out) and starts a new one; a payload without a PES started is
 * not used.  Returns 0 or ENOMEM.
 */
static int
feed(
	struct mf_file *file,
	struct ts_state *state,
	unsigned index,
	const struct ts_packet *packet,
	int *finished)
{
	struct ts_track *track;
	size_t length;
	int error;

	/* Nothing is finished yet. */
	track = &state->tracks[index];
	*finished = 0;

	/* A packet sent twice (the same counter) carries nothing new. */
	if (track->counter >= 0 &&
	    !packet->discontinuity &&
	    packet->counter == (unsigned)track->counter)
		return 0;

	/* A packet lost before this one damages the PES being put together. */
	if (track->counter >= 0 &&
	    !packet->discontinuity &&
	    packet->counter != (((unsigned)track->counter + 1U) & 0x0fU))
		track->pes_damaged = track->pes_open;
	track->counter = (int)packet->counter;

	/* A unit start finishes the PES before it. */
	if (packet->unit_start && track->pes_open) {
		finish_pes(file, state, index);
		*finished = (int)state->ready.active;
	}

	/* A unit start begins a PES; a payload without one before it is not used. */
	if (packet->unit_start) {
		track->pes_open = 1U;
		track->pes_random = packet->random;
		track->pes_damaged = 0;
		track->pes_size = 0;
	} else if (!track->pes_open) {
		return 0;
	}

	/* The payload. */
	error = pes_append(track, packet->payload, packet->payload_size);
	if (error != 0)
		return error;

	/* A PES whose length says it is whole is finished next. */
	if (track->pes_size >= 6U) {
		length = mf_be16(track->pes + 4);
		if (length != 0 && track->pes_size >= 6U + length)
			track->pes_full = 1U;
	}

	/* Succeeded: the payload is in the PES. */
	return 0;
}

/*
 * Adds bytes to a track's PES.  Returns 0, or ENOMEM.  A PES that grows
 * past TS_PES_MAX keeps no more bytes and is left out when it is finished.
 */
static int
pes_append(
	struct ts_track *track,
	const unsigned char *data,
	size_t size)
{
	unsigned char *pes;
	size_t capacity;

	/* A PES too large to be real keeps growing no more (finish_pes leaves it out). */
	if (track->pes_size > TS_PES_MAX)
		return 0;

	/* Room for the bytes: twice the buffer, or what they need. */
	if (track->pes_capacity - track->pes_size < size) {
		capacity = track->pes_capacity * 2U;
		if (capacity < track->pes_size + size)
			capacity = track->pes_size + size;

		/* The larger buffer. */
		pes = realloc(track->pes, capacity);
		if (pes == NULL)
			return ENOMEM;

		/* The track keeps it. */
		track->pes = pes;
		track->pes_capacity = capacity;
	}

	/* The bytes. */
	memcpy(track->pes + track->pes_size, data, size);
	track->pes_size += size;

	/* Succeeded: the bytes are in the PES. */
	return 0;
}

/*
 * Finishes a track's PES: reads its header and makes it the PES being
 * handed out (state->ready), its bytes swapped into the ready buffer.  A
 * PES without a header, cut short (lost packets, the end of the file) or
 * too large is left out and counted, and state->ready stays inactive.
 */
static void
finish_pes(
	struct mf_file *file,
	struct ts_state *state,
	unsigned index)
{
	struct ts_track *track;
	unsigned char *buffer;
	size_t capacity;
	size_t payload;
	size_t length;
	size_t end;
	int64_t pts;
	int64_t dts;
	int error;

	/* The PES is closed whatever it holds. */
	track = &state->tracks[index];
	track->pes_open = 0;
	track->pes_full = 0;

	/* Its header and where its payload is; one with a lost packet, damaged or too large is left out. */
	error = pes_times(track->pes, track->pes_size, &payload, &pts, &dts);
	if (error != 0 ||
	    track->pes_size > TS_PES_MAX ||
	    track->pes_damaged) {
		file->tracks[index].dropped_count++;
		track->pes_size = 0;
		return;
	}

	/* Where it ends: all that was put together, unless its length says less. */
	end = track->pes_size;
	length = mf_be16(track->pes + 4);
	if (length != 0)
		end = 6U + length;

	/* One cut short of its length (the end of the file) is left out. */
	if (end > track->pes_size || end < payload) {
		file->tracks[index].dropped_count++;
		track->pes_size = 0;
		return;
	}

	/* A PES without a time goes on from the track's last one (the start when there is none yet). */
	if (pts == TS_NO_TIME)
		pts = track->last_pts;
	if (pts == TS_NO_TIME)
		pts = state->start;
	if (dts == TS_NO_TIME)
		dts = pts;

	/* The track's first and latest times. */
	if (track->first_pts == TS_NO_TIME)
		track->first_pts = pts;
	track->last_pts = pts;

	/* Its bytes become the ready buffer's, and the old ready buffer the track's. */
	buffer = state->ready.buffer;
	capacity = state->ready.capacity;
	state->ready.buffer = track->pes;
	state->ready.capacity = track->pes_capacity;
	track->pes = buffer;
	track->pes_capacity = capacity;
	track->pes_size = 0;

	/* The PES being handed out. */
	state->ready.active = 1U;
	state->ready.track = index;
	state->ready.data = state->ready.buffer + payload;
	state->ready.size = end - payload;
	state->ready.next = 0;
	state->ready.pts = pts;
	state->ready.dts = dts;
	state->ready.samples = 0;
	state->ready.key = 1U;

	/* A picture is a key frame when its adaptation field said so or it is an IDR or IRAP picture. */
	if (file->tracks[index].kind == MF_TRACK_VIDEO && !track->pes_random) {
		state->ready.key = (unsigned)is_key_picture(file->tracks[index].codec,
							    state->ready.data,
							    state->ready.size);
	}

}

/*
 * Reads a PES's header: where its payload starts, and its PTS and DTS
 * (TS_NO_TIME when absent).  It needs only the PES's first bytes (a
 * transport packet's payload).  Returns 0, or EINVAL for a PES without a
 * start code or whose header runs past the bytes.
 */
static int
pes_times(
	const unsigned char *pes,
	size_t size,
	size_t *payload,
	int64_t *pts,
	int64_t *dts)
{
	unsigned flags;

	/* No times until they are read. */
	*pts = TS_NO_TIME;
	*dts = TS_NO_TIME;

	/* The start code, the stream ID, the length and the optional header's flags and length. */
	if (size < 9U)
		return EINVAL;
	if (pes[0] != 0 ||
	    pes[1] != 0 ||
	    pes[2] != 1U)
		return EINVAL;

	/* The payload after the optional header. */
	flags = pes[7];
	*payload = 9U + pes[8];
	if (*payload > size)
		return EINVAL;

	/* The PTS, and the DTS after it. */
	if ((flags & 0x80U) != 0U && *payload >= 14U)
		*pts = pes_time(pes + 9);
	if ((flags & 0xc0U) == 0xc0U && *payload >= 19U)
		*dts = pes_time(pes + 14);

	/* Succeeded: the payload's start and its times. */
	return 0;
}

/*
 * Reads a 33-bit time of a PES header (5 bytes, with marker bits).
 */
static int64_t
pes_time(
	const unsigned char *bytes)
{
	int64_t time;

	/* Three bits, fifteen and fifteen, each group before a marker bit. */
	time = (int64_t)((bytes[0] >> 1) & 0x07U) << 30;
	time |= (int64_t)bytes[1] << 22;
	time |= (int64_t)(bytes[2] >> 1) << 15;
	time |= (int64_t)bytes[3] << 7;
	time |= (int64_t)(bytes[4] >> 1);

	/* The time in the 90 kHz clock. */
	return time;
}

/*
 * Lets every PES being put together or handed out go (a seek, or the
 * reading starting again).
 */
static void
discard_pes(
	struct ts_state *state)
{
	unsigned i;

	/* Each track's PES. */
	for (i = 0; i < MF_TRACK_MAX; i++) {
		state->tracks[i].pes_open = 0;
		state->tracks[i].pes_full = 0;
		state->tracks[i].pes_damaged = 0;
		state->tracks[i].pes_size = 0;
		state->tracks[i].counter = -1;
	}

	/* The PES being handed out, and the block (the reading's place moves). */
	state->ready.active = 0;
	state->block_size = 0;
}

/*
 * Reads the start of the file: the tables, then PES packets until each
 * track has had one (or TS_PROBE_MAX bytes were read), for each track's
 * picture size or sound format and the time the file starts at.
 */
static int
probe(
	struct mf_file *file,
	struct ts_state *state)
{
	unsigned i;
	unsigned waiting;
	int64_t earlier;
	int error;

	/* PES packets until every track has had one. */
	state->start = TS_NO_TIME;
	for (;;) {
		/* Stops after TS_PROBE_MAX bytes. */
		if (state->position - state->base > TS_PROBE_MAX)
			break;

		/* The next PES; the end of the file ends the probe. */
		error = next_pes(file, state);
		if (error == ENODATA)
			break;
		if (error != 0)
			return error;

		/* What it tells of its track. */
		probe_info(file, state);
		state->ready.active = 0;

		/* Whether a track still waits for its first PES. */
		waiting = 0;
		for (i = 0; i < file->track_count; i++) {
			/* One without a time yet. */
			if (state->tracks[i].first_pts == TS_NO_TIME)
				waiting = 1U;
		}

		/* Every track has had its first PES. */
		if (file->track_count != 0 && !waiting)
			break;
	}

	/* The file starts at the earliest first time (a track that starts before another is not wrapped). */
	for (i = 0; i < file->track_count; i++) {
		/* A track that had a PES. */
		if (state->tracks[i].first_pts == TS_NO_TIME)
			continue;

		/* The first track that had one starts the file for now. */
		if (state->start == TS_NO_TIME) {
			state->start = state->tracks[i].first_pts;
			continue;
		}

		/* A track that starts before it starts the file. */
		earlier = relative(state, state->tracks[i].first_pts);
		if (earlier < 0)
			state->start = state->tracks[i].first_pts;
	}

	/* A file without times starts at 0. */
	if (state->start == TS_NO_TIME)
		state->start = 0;

	/* A seek is decided by the first video track, else by the first track. */
	state->lead = 0;
	for (i = file->track_count; i > 0; i--) {
		/* A video track before the one found so far. */
		if (file->tracks[i - 1U].kind == MF_TRACK_VIDEO)
			state->lead = i - 1U;
	}

	/* Succeeded: the tracks and the start are known. */
	return 0;
}

/*
 * Reads what a track's first PES tells of it: an H.264 picture's size from
 * its sequence parameter set, a sound's rate and channels from its first
 * frame's header.
 */
static void
probe_info(
	struct mf_file *file,
	struct ts_state *state)
{
	struct mf_track *info;
	size_t length;
	uint32_t samples;
	int error;

	/* The track of the PES; one already known is left. */
	info = &file->tracks[state->ready.track];
	if (info->width != 0 || info->sample_rate != 0)
		return;

	/* A private stream whose PES starts with an ADTS frame is AAC. */
	if (state->tracks[state->ready.track].stream_type == TS_TYPE_PRIVATE && info->codec == MF_CODEC_UNKNOWN) {
		error = adts_frame(state->ready.data, state->ready.size, &length, &info->sample_rate, &info->channels, &samples);
		if (error != 0)
			return;

		/* The track is sound, in ADTS. */
		info->kind = MF_TRACK_AUDIO;
		info->codec = MF_CODEC_AAC;
		mf_set_codec_name(info, "adts", 4U);
		return;
	}

	/* The size of an H.264 picture. */
	if (info->codec == MF_CODEC_H264) {
		(void)h264_size(state->ready.data, state->ready.size, &info->width, &info->height);
		return;
	}

	/* The rate and channels of an ADTS frame. */
	if (info->codec == MF_CODEC_AAC) {
		error = adts_frame(state->ready.data, state->ready.size, &length, &info->sample_rate, &info->channels, &samples);
		if (error != 0)
			info->sample_rate = 0;
		return;
	}

	/* The rate and channels of an MPEG audio frame. */
	if (info->codec == MF_CODEC_MP3) {
		error = mpeg_audio_frame(state->ready.data, state->ready.size, &length, &info->sample_rate, &info->channels, &samples);
		if (error != 0)
			info->sample_rate = 0;
	}
}

/*
 * Finds the file's length: the latest PTS of any track in the last
 * TS_TAIL_MAX bytes, from the start.
 */
static void
find_length(
	struct mf_file *file,
	struct ts_state *state)
{
	struct ts_packet packet;
	uint64_t offset;
	uint64_t tail;
	int64_t pts;
	int64_t dts;
	int64_t latest;
	int64_t time;
	size_t payload;
	int index;
	int error;

	/* The tail of the file, from a packet. */
	offset = state->base;
	tail = file->size - state->base;
	if (tail > TS_TAIL_MAX)
		offset = state->base + ((tail - TS_TAIL_MAX) / state->packet_size) * state->packet_size;

	/* Each packet of it that starts a PES of a track. */
	latest = 0;
	for (;
	     offset + state->packet_size <= file->size;
	     offset += state->packet_size) {
		/* The packet and its header; the end stops, one without a payload is passed by. */
		error = scan_packet(file, state, &offset, &packet);
		if (error == ENOENT)
			continue;
		if (error != 0)
			break;
		if (!packet.unit_start)
			continue;

		/* A track's PES start, and its time. */
		index = track_of(file, state, packet.pid);
		if (index < 0)
			continue;
		(void)pes_times(packet.payload, packet.payload_size, &payload, &pts, &dts);
		if (pts == TS_NO_TIME)
			continue;

		/* The latest time. */
		time = relative(state, pts);
		if (time > latest)
			latest = time;
	}

	/* The file's length (its tracks' are not known). */
	file->duration_us = mf_scale_us(latest, TS_CLOCK);
}

/*
 * Turns a raw 90 kHz time into one from the file's start, reading a time
 * far ahead on the 33-bit clock as one before the start (a wrap the other
 * way) and a time just behind the start as negative.
 */
static int64_t
relative(
	const struct ts_state *state,
	int64_t raw)
{
	int64_t difference;

	/* The difference on the wrapping clock. */
	difference = (raw - state->start) & (TS_WRAP - 1);

	/* A time just before the start. */
	if (difference >= TS_WRAP - TS_BEHIND)
		difference -= TS_WRAP;

	/* The time from the start. */
	return difference;
}

/* Turns a raw 90 kHz time into microseconds from the file's start. */
static int64_t
time_us(
	const struct ts_state *state,
	int64_t raw)
{
	int64_t time;

	/* From the start, in microseconds. */
	time = relative(state, raw);
	return mf_scale_us(time, TS_CLOCK);
}

/*
 * Hands out the next frame of the audio PES being handed out, timed from
 * the PES's PTS by the samples of the frames before it.  Returns 0, or
 * EAGAIN when the PES has no frame left (or a damaged one, whose rest is
 * let go), or ENOMEM.
 */
static int
hand_out_audio(
	struct mf_file *file,
	struct ts_state *state,
	struct mf_packet *packet)
{
	struct mf_track *info;
	const unsigned char *frame;
	size_t left;
	size_t length;
	uint32_t rate;
	uint32_t channels;
	uint32_t samples;
	int64_t offset;
	int error;

	/* The rest of the PES. */
	info = &file->tracks[state->ready.track];
	frame = state->ready.data + state->ready.next;
	left = state->ready.size - state->ready.next;

	/* The frame's header: its length and samples. */
	if (info->codec == MF_CODEC_AAC)
		error = adts_frame(frame, left, &length, &rate, &channels, &samples);
	else
		error = mpeg_audio_frame(frame, left, &length, &rate, &channels, &samples);

	/* No frame left, or a damaged one: the PES is done. */
	if (error != 0) {
		state->ready.active = 0;
		return EAGAIN;
	}

	/* The frame's bytes. */
	error = mf_packet_room(file, length);
	if (error != 0)
		return error;

	/* The packet, timed (in the clock's units) after the frames before it in the PES. */
	memcpy(file->buffer, frame, length);
	offset = (int64_t)(state->ready.samples * TS_CLOCK / rate);
	packet->track = state->ready.track;
	packet->pts_us = time_us(state, state->ready.pts + offset);
	packet->dts_us = packet->pts_us;
	packet->keyframe = 1;
	packet->data = file->buffer;
	packet->size = length;

	/* The PES moves on past the frame. */
	state->ready.next += length;
	state->ready.samples += samples;

	/* Succeeded: one frame. */
	return 0;
}

/*
 * Reads an ADTS frame's header: the frame's length (with the header), its
 * rate, channels and samples.  Returns 0, or EINVAL when there is no whole
 * frame.
 */
static int
adts_frame(
	const unsigned char *data,
	size_t size,
	size_t *length,
	uint32_t *rate,
	uint32_t *channels,
	uint32_t *samples)
{
	unsigned index;

	/* The fixed header's 7 bytes and its sync word (with layer 0). */
	if (size < 7U)
		return EINVAL;
	if (data[0] != 0xffU || (data[1] & 0xf6U) != 0xf0U)
		return EINVAL;

	/* The rate, by its index. */
	index = (data[2] >> 2) & 0x0fU;
	if (index >= sizeof(ts_aac_rates) / sizeof(ts_aac_rates[0]))
		return EINVAL;
	*rate = ts_aac_rates[index];

	/* The channels, the frame's length (13 bits) and its raw data blocks (1024 samples each). */
	*channels = ((uint32_t)(data[2] & 0x01U) << 2) | (data[3] >> 6);
	*length = ((size_t)(data[3] & 0x03U) << 11) | ((size_t)data[4] << 3) | (data[5] >> 5);
	*samples = TS_AAC_SAMPLES * ((data[6] & 0x03U) + 1U);

	/* Refuses a frame shorter than its header or longer than what is left. */
	if (*length < 7U || *length > size)
		return EINVAL;

	/* Succeeded: one frame. */
	return 0;
}

/*
 * Reads an MPEG audio frame's header (layers I, II and III of versions 1,
 * 2 and 2.5): the frame's length, its rate, channels and samples.
 * Returns 0, or EINVAL when there is no whole frame.
 */
static int
mpeg_audio_frame(
	const unsigned char *data,
	size_t size,
	size_t *length,
	uint32_t *rate,
	uint32_t *channels,
	uint32_t *samples)
{
	unsigned version;
	unsigned layer;
	unsigned bitrate_index;
	unsigned rate_index;
	unsigned padding;
	unsigned table;
	uint32_t bitrate;

	/* The header's 4 bytes and its sync word. */
	if (size < 4U)
		return EINVAL;
	if (data[0] != 0xffU || (data[1] & 0xe0U) != 0xe0U)
		return EINVAL;

	/* The version (3: 1, 2: 2, 0: 2.5), the layer (3: I, 2: II, 1: III), and the indexes. */
	version = (data[1] >> 3) & 3U;
	layer = (data[1] >> 1) & 3U;
	bitrate_index = data[2] >> 4;
	rate_index = (data[2] >> 2) & 3U;
	padding = (data[2] >> 1) & 1U;

	/* Refuses the reserved version and layer, a free or bad bit rate, and the reserved rate. */
	if (version == 1U || layer == 0)
		return EINVAL;
	if (bitrate_index == 0 || bitrate_index == 15U)
		return EINVAL;
	if (rate_index == 3U)
		return EINVAL;

	/* The rate: version 1's, halved for version 2, quartered for 2.5. */
	*rate = ts_mpeg_rates[rate_index];
	if (version == 2U)
		*rate /= 2U;
	else if (version == 0)
		*rate /= 4U;

	/* The bit rate of the version and layer. */
	table = 0;
	if (version != 3U)
		table = 1U;
	bitrate = (uint32_t)ts_mpeg_bitrates[table][3U - layer][bitrate_index] * 1000U;

	/* The samples and the length: layer I in 4-byte slots, layer III of versions 2 and 2.5 half as long. */
	if (layer == 3U) {
		*samples = 384U;
		*length = (12U * bitrate / *rate + padding) * 4U;
	} else if (layer == 1U && version != 3U) {
		*samples = 576U;
		*length = 72U * bitrate / *rate + padding;
	} else {
		*samples = 1152U;
		*length = 144U * bitrate / *rate + padding;
	}

	/* One channel in mono mode, else two. */
	*channels = 2U;
	if ((data[3] >> 6) == 3U)
		*channels = 1U;

	/* Refuses a frame longer than what is left. */
	if (*length < 4U || *length > size)
		return EINVAL;

	/* Succeeded: one frame. */
	return 0;
}

/*
 * Says whether a video PES's picture is one a decoder can start at: its
 * first slice is an IDR picture's (H.264) or an IRAP picture's (H.265).
 */
static int
is_key_picture(
	unsigned codec,
	const unsigned char *data,
	size_t size)
{
	struct ts_walk walk;
	int key;

	/* The NAL units of the picture until its first slice. */
	memset(&walk, 0, sizeof(walk));
	key = nal_feed(&walk, codec, data, size);

	/* Whether that slice is a key picture's. */
	return key;
}

/*
 * Feeds bytes of a picture to the NAL scanner of a walk: finds each start
 * code (also across calls) and reads the NAL header after it until the
 * picture's first slice tells whether it is a key picture.  Returns 1 when
 * it is, 0 otherwise (also while undecided).
 */
static int
nal_feed(
	struct ts_walk *walk,
	unsigned codec,
	const unsigned char *data,
	size_t size)
{
	size_t i;
	unsigned key;
	int slice;

	/* Each byte, until the first slice is found. */
	for (i = 0; i < size && !walk->decided; i++) {
		/* A NAL header after a start code: a slice decides. */
		if (walk->header_next) {
			walk->header_next = 0;
			slice = key_nal(codec, data[i], &key);
			if (slice) {
				walk->decided = 1U;
				walk->key = key;
			}

			/* Another NAL unit: the next start code is looked for. */
			continue;
		}

		/* A zero byte may begin a start code. */
		if (data[i] == 0) {
			walk->zeros++;
			continue;
		}

		/* A 1 after two zero bytes ends one: a NAL header follows. */
		if (data[i] == 1U && walk->zeros >= 2U)
			walk->header_next = 1U;
		walk->zeros = 0;
	}

	/* Whether the picture is a key picture (as far as is known). */
	return (int)walk->key;
}

/*
 * Reads a NAL header's type: returns 1 when it is a slice (and *key says
 * whether of a key picture), 0 for another NAL unit.
 */
static int
key_nal(
	unsigned codec,
	unsigned header,
	unsigned *key)
{
	unsigned type;

	/* H.265: the type is bits 1 to 6; 0 to 9 are slices of other pictures, 16 to 21 of IRAP pictures. */
	*key = 0;
	if (codec == MF_CODEC_HEVC) {
		type = (header >> 1) & 0x3fU;
		if (type >= 16U && type <= 21U) {
			*key = 1U;
			return 1;
		}

		/* Another picture's slice, else not a slice. */
		if (type <= 9U)
			return 1;
		return 0;
	}

	/* H.264: the type is the low 5 bits; 5 is an IDR picture's slice. */
	type = header & 0x1fU;
	if (type == 5U) {
		*key = 1U;
		return 1;
	}

	/* 1 to 4 are other pictures' slices. */
	if (type >= 1U && type <= 4U)
		return 1;

	/* Not a slice. */
	return 0;
}

/*
 * Finds an H.264 stream's picture size in its sequence parameter set: the
 * macroblocks, the field coding and the cropping.  Returns 0, or EINVAL
 * when there is no set or it cannot be read.
 */
static int
h264_size(
	const unsigned char *data,
	size_t size,
	uint32_t *width,
	uint32_t *height)
{
	struct ts_bits bits;
	const unsigned char *sps;
	size_t sps_size;
	unsigned i;
	unsigned profile;
	unsigned chroma;
	unsigned frame_only;
	int high;
	unsigned cropped;
	uint32_t width_mbs;
	uint32_t height_units;
	uint32_t crop[4];
	uint32_t unit_x;
	uint32_t unit_y;
	uint32_t cut;
	int error;

	/* The set, its bits without the emulation prevention bytes. */
	error = find_h264_sps(data, size, &sps, &sps_size);
	if (error != 0)
		return error;
	bits_load(&bits, sps, sps_size);

	/* The profile, then the constraints and level and the set's ID, which do not matter here. */
	profile = bits_read(&bits, 8U);
	(void)bits_read(&bits, 16U);
	(void)bits_golomb(&bits);

	/* The high profiles' chroma format (4:2:0 for the others), bit depths and scaling matrices. */
	chroma = 1U;
	high = is_high_profile(profile);
	if (high)
		h264_high_profile(&bits, &chroma);

	/* The frame numbers and the picture order count. */
	(void)bits_golomb(&bits);
	h264_order(&bits);

	/* The reference frames and gaps, then the size in macroblocks. */
	(void)bits_golomb(&bits);
	(void)bits_read(&bits, 1U);
	width_mbs = bits_golomb(&bits) + 1U;
	height_units = bits_golomb(&bits) + 1U;

	/* Whether there are only frames (else the adaptive flag), and the direct 8x8 flag. */
	frame_only = bits_read(&bits, 1U);
	if (!frame_only)
		(void)bits_read(&bits, 1U);
	(void)bits_read(&bits, 1U);

	/* The cropping: left, right, top and bottom, in the chroma format's units. */
	memset(crop, 0, sizeof(crop));
	cropped = bits_read(&bits, 1U);
	for (i = 0; i < 4U && cropped; i++)
		crop[i] = bits_golomb(&bits);

	/* Refuses a set that could not be read or a size no picture has. */
	if (bits.failed || width_mbs > 1024U || height_units > 1024U)
		return EINVAL;

	/* The cropping units: two samples across for 4:2:0 and 4:2:2, two down for 4:2:0, doubled for fields. */
	unit_x = 1U;
	unit_y = 2U - frame_only;
	if (chroma == 1U || chroma == 2U)
		unit_x = 2U;
	if (chroma == 1U)
		unit_y *= 2U;

	/* The width: the macroblocks less the cropping across. */
	*width = width_mbs * 16U;
	cut = unit_x * (crop[0] + crop[1]);
	if (cut < *width)
		*width -= cut;

	/* The height: the macroblocks (of each field) less the cropping down. */
	*height = height_units * 16U * (2U - frame_only);
	cut = unit_y * (crop[2] + crop[3]);
	if (cut < *height)
		*height -= cut;

	/* Succeeded: the picture's size. */
	return 0;
}

/*
 * Says whether a profile's sequence parameter sets have the high profiles'
 * part (the chroma format, bit depths and scaling matrices).
 */
static int
is_high_profile(
	unsigned profile)
{
	static const unsigned char profiles[] = { 100, 110, 122, 244, 44, 83, 86, 118, 128, 138, 139, 134, 135 };
	size_t i;

	/* Each profile with the part. */
	for (i = 0; i < sizeof(profiles); i++) {
		/* The profile is one of them. */
		if (profiles[i] == profile)
			return 1;
	}

	/* Another profile. */
	return 0;
}

/*
 * Reads the part of a sequence parameter set only the high profiles have:
 * the chroma format, the bit depths and the scaling matrices.
 */
static void
h264_high_profile(
	struct ts_bits *bits,
	unsigned *chroma)
{
	unsigned lists;
	unsigned present;
	unsigned i;

	/* The chroma format, and 4:4:4's separate planes flag. */
	*chroma = bits_golomb(bits);
	if (*chroma == 3U)
		(void)bits_read(bits, 1U);

	/* The bit depths of luma and chroma, and the lossless flag. */
	(void)bits_golomb(bits);
	(void)bits_golomb(bits);
	(void)bits_read(bits, 1U);

	/* Whether there are scaling matrices: 8 lists, 12 for 4:4:4. */
	present = bits_read(bits, 1U);
	if (!present)
		return;
	lists = 8U;
	if (*chroma == 3U)
		lists = 12U;

	/* Each list that is present: the first six of 16 entries, the rest of 64. */
	for (i = 0; i < lists && !bits->failed; i++) {
		/* Whether this list is present. */
		present = bits_read(bits, 1U);
		if (!present)
			continue;

		/* Skipped by its size. */
		if (i < 6U)
			bits_skip_scaling(bits, 16U);
		else
			bits_skip_scaling(bits, 64U);
	}
}

/*
 * Reads a sequence parameter set's picture order count: its kind, and the
 * fields of kinds 0 and 1.
 */
static void
h264_order(
	struct ts_bits *bits)
{
	uint32_t kind;
	uint32_t cycle;
	uint32_t i;

	/* The kind. */
	kind = bits_golomb(bits);

	/* Kind 0: the length of the count's low bits. */
	if (kind == 0) {
		(void)bits_golomb(bits);
		return;
	}

	/* Kinds other than 1 have no fields. */
	if (kind != 1U)
		return;

	/* Kind 1: a flag, two offsets and a cycle of offsets. */
	(void)bits_read(bits, 1U);
	(void)bits_golomb(bits);
	(void)bits_golomb(bits);
	cycle = bits_golomb(bits);
	for (i = 0; i < cycle && !bits->failed; i++)
		(void)bits_golomb(bits);
}

/*
 * Loads a NAL unit's payload (after its header byte) into a bit reader,
 * taking out the emulation prevention bytes (the 3 of 00 00 03).
 */
static void
bits_load(
	struct ts_bits *bits,
	const unsigned char *nal,
	size_t size)
{
	unsigned zeros;
	size_t i;

	/* An empty reader. */
	memset(bits, 0, sizeof(*bits));

	/* Each byte after the header, as many as the reader holds. */
	zeros = 0;
	for (i = 1; i < size && bits->size < sizeof(bits->bytes); i++) {
		/* A 3 after two zero bytes is not the payload's. */
		if (zeros >= 2U && nal[i] == 3U) {
			zeros = 0;
			continue;
		}

		/* Zero bytes are counted for the next test. */
		if (nal[i] == 0)
			zeros++;
		else
			zeros = 0;

		/* The byte. */
		bits->bytes[bits->size] = nal[i];
		bits->size++;
	}
}

/*
 * Finds the first sequence parameter set (NAL type 7) in an Annex B
 * stream: its bytes from the NAL header to the next start code.
 */
static int
find_h264_sps(
	const unsigned char *data,
	size_t size,
	const unsigned char **sps,
	size_t *sps_size)
{
	size_t i;
	size_t start;
	size_t end;

	/* Each start code. */
	for (i = 0; i + 3U < size; i++) {
		/* 00 00 01. */
		if (data[i] != 0 || data[i + 1] != 0)
			continue;
		if (data[i + 2] != 1U)
			continue;

		/* A NAL header of type 7. */
		if ((data[i + 3] & 0x1fU) != 7U)
			continue;

		/* The set runs to the next start code (00 00 00 or 00 00 01), or to the end. */
		start = i + 3U;
		end = size;
		for (i = start; i + 2U < size; i++) {
			/* Two zero bytes and a 0 or 1 end it. */
			if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] <= 1U) {
				end = i;
				break;
			}
		}

		/* Succeeded: the set's bytes. */
		*sps = data + start;
		*sps_size = end - start;
		return 0;
	}

	/* No set in the picture. */
	return EINVAL;
}

/* Reads count bits (at most 32), marking the reader failed past its end. */
static unsigned
bits_read(
	struct ts_bits *bits,
	unsigned count)
{
	unsigned value;
	unsigned i;
	unsigned byte;

	/* Each bit, high first. */
	value = 0;
	for (i = 0; i < count; i++) {
		/* Past the end: the reader fails. */
		if (bits->bit / 8U >= bits->size) {
			bits->failed = 1U;
			return 0;
		}

		/* The bit. */
		byte = bits->bytes[bits->bit / 8U];
		value = (value << 1) | ((byte >> (7U - bits->bit % 8U)) & 1U);
		bits->bit++;
	}

	/* The bits read. */
	return value;
}

/* Reads an unsigned Exp-Golomb number (also the bits of a signed one). */
static uint32_t
bits_golomb(
	struct ts_bits *bits)
{
	unsigned zeros;
	unsigned bit;
	uint32_t rest;

	/* The leading zero bits, at most 31. */
	zeros = 0;
	for (;;) {
		/* One bit; a 1 ends the zeros. */
		bit = bits_read(bits, 1U);
		if (bit != 0 || bits->failed)
			break;

		/* One more zero; more than 31 is no number of a parameter set. */
		zeros++;
		if (zeros > 31U) {
			bits->failed = 1U;
			return 0;
		}
	}

	/* The number: 2^zeros - 1 plus the bits after the 1. */
	rest = bits_read(bits, zeros);
	return ((uint32_t)1 << zeros) - 1U + rest;
}

/* Skips a scaling list of a sequence parameter set. */
static void
bits_skip_scaling(
	struct ts_bits *bits,
	unsigned size)
{
	unsigned i;
	int last;
	int next;
	int delta;
	uint32_t code;

	/* Each entry while the deltas go on. */
	last = 8;
	next = 8;
	for (i = 0; i < size && !bits->failed; i++) {
		/* A delta, signed. */
		if (next != 0) {
			code = bits_golomb(bits);
			if (code > 512U) {
				bits->failed = 1U;
				return;
			}

			/* The code 0, 1, 2, 3, 4 is the delta 0, 1, -1, 2, -2. */
			delta = (int)((code + 1U) / 2U);
			if ((code & 1U) == 0)
				delta = -delta;
			next = (last + delta + 256) % 256;
		}

		/* The entry. */
		if (next != 0)
			last = next;
	}
}

/*
 * Finds the time of the first PES of the lead track that starts in the
 * TS_SEEK_SCAN bytes from an offset (a packet's).  Returns 0 with the time
 * from the file's start, ENODATA when there is none, or the reading's
 * error.
 */
static int
lead_time_at(
	struct mf_file *file,
	struct ts_state *state,
	uint64_t offset,
	int64_t *time)
{
	struct ts_packet packet;
	struct ts_track *lead;
	uint64_t limit;
	size_t payload;
	int64_t pts;
	int64_t dts;
	int error;

	/* Each packet of the stretch. */
	lead = &state->tracks[state->lead];
	limit = offset + TS_SEEK_SCAN;
	for (;
	     offset < limit;
	     offset += state->packet_size) {
		/* The packet and its header; one without a payload is passed by. */
		error = scan_packet(file, state, &offset, &packet);
		if (error == ENOENT)
			continue;
		if (error != 0)
			return error;
		if (!packet.unit_start || packet.pid != lead->pid)
			continue;

		/* The PES's time; one without is passed by. */
		(void)pes_times(packet.payload, packet.payload_size, &payload, &pts, &dts);
		if (pts == TS_NO_TIME)
			continue;

		/* Succeeded: the time from the start. */
		*time = relative(state, pts);
		return 0;
	}

	/* No PES of the lead in the stretch. */
	return ENODATA;
}

/*
 * Reads the lead track's PES packets from an offset (a packet's) and finds
 * the last key frame presented at or before the target time, stopping at
 * the first PES after it or at the limit.  Returns 0 with the offset of
 * the key frame's first packet, ENOENT when there is none, or the
 * reading's error.
 */
static int
find_key(
	struct mf_file *file,
	struct ts_state *state,
	uint64_t from,
	uint64_t limit,
	int64_t target,
	uint64_t *found)
{
	struct ts_packet packet;
	struct ts_walk walk;
	struct ts_track *lead;
	struct mf_track *info;
	uint64_t offset;
	unsigned have;
	size_t payload;
	int64_t pts;
	int64_t dts;
	int error;

	/* No PES open and no key frame found yet. */
	lead = &state->tracks[state->lead];
	info = &file->tracks[state->lead];
	memset(&walk, 0, sizeof(walk));
	have = 0;

	/* Each packet to the limit or the end of the file. */
	for (offset = from;
	     offset < limit;
	     offset += state->packet_size) {
		/* The packet and its header; the end stops, one without a payload is passed by. */
		error = scan_packet(file, state, &offset, &packet);
		if (error == ENOENT)
			continue;
		if (error == ENODATA)
			break;
		if (error != 0)
			return error;
		if (packet.pid != lead->pid)
			continue;

		/* A packet in the middle of a PES: more of its picture for the scanner. */
		if (!packet.unit_start) {
			if (walk.open && info->kind == MF_TRACK_VIDEO)
				(void)nal_feed(&walk, info->codec, packet.payload, packet.payload_size);
			continue;
		}

		/* A new PES finishes the one before it. */
		walk_finish(&walk, target, found, &have);

		/* Its time; past the target, the search is over. */
		(void)pes_times(packet.payload, packet.payload_size, &payload, &pts, &dts);
		memset(&walk, 0, sizeof(walk));
		if (pts == TS_NO_TIME)
			continue;
		walk.time = relative(state, pts);
		if (walk.time > target)
			break;

		/* The PES is walked: its start, and whether it is a key frame. */
		walk.open = 1U;
		walk.position = offset;
		walk.key = packet.random;
		walk.decided = packet.random;
		if (info->kind != MF_TRACK_VIDEO) {
			walk.key = 1U;
			walk.decided = 1U;
		}

		/* The picture's first bytes for the scanner. */
		if (payload < packet.payload_size && info->kind == MF_TRACK_VIDEO)
			(void)nal_feed(&walk, info->codec, packet.payload + payload, packet.payload_size - payload);
	}

	/* The last PES walked. */
	walk_finish(&walk, target, found, &have);

	/* No key frame at or before the target. */
	if (!have)
		return ENOENT;

	/* Succeeded: where the key frame starts. */
	return 0;
}

/* Keeps a walked PES's place when it is a key frame at or before the target. */
static void
walk_finish(
	const struct ts_walk *walk,
	int64_t target,
	uint64_t *found,
	unsigned *have)
{
	/* A PES walked that is a key frame. */
	if (!walk->open || !walk->key)
		return;

	/* Not after the target. */
	if (walk->time > target)
		return;

	/* The latest such place. */
	*found = walk->position;
	*have = 1U;
}
