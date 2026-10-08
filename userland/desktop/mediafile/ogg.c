/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The Ogg reader (ws177-p029): Opus (.opus, .ogg), Vorbis (.ogg) and
 * Theora (.ogv) streams of one Ogg physical stream.  The streams are the
 * ones whose first page (BOS) opens the file; their header packets (2 for
 * Opus, 3 for Vorbis and Theora) are read at the start and kept as the
 * track's private data (OpusHead; Vorbis's and Theora's three headers in
 * Xiph lacing, as Matroska's CodecPrivate holds them).  Then the pages are
 * read in the file's order, each checked by its CRC, and each packet that
 * ends on a page is handed out.
 *
 * A packet's time comes from the granule positions: the first packets of
 * a stream (and the first after a seek or a lost page) are timed back from
 * the end their page's granule position gives, the rest go on by each
 * packet's length (an Opus packet's from its TOC byte, a Vorbis packet's
 * from its mode's block size, one frame of Theora).  Opus's pre-skip is
 * taken off its times; a Theora granule position is the key frame's number
 * shifted and the frames since it.
 *
 * There is no index: the length comes from the last granule position of
 * each stream, and a seek halves the file on the first stream's granule
 * positions (the first video stream's: then it goes back to the key frame
 * the granule position names, and the frames before it are not handed
 * out).  A page whose CRC is wrong is passed over, and the packet it broke
 * is left out and counted (mf_track.dropped_count).
 */

#include "mediafile-private.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>

/* A page's fixed header, and the largest page (255 segments of 255 bytes). */
#define OGG_HEADER		27U
#define OGG_PAGE_MAX		(OGG_HEADER + 255U + 255U * 255U)

/* A page's flags: its first packet goes on from the page before, it begins and it ends its stream. */
#define OGG_CONTINUED		0x01U
#define OGG_BEGIN		0x02U

/* The kinds of stream known. */
#define OGG_OPUS		1U
#define OGG_VORBIS		2U
#define OGG_THEORA		3U

/* The largest packet put together (one larger is left out). */
#define OGG_PACKET_MAX		(16U * 1024U * 1024U)

/* The bytes read at the start for the streams' headers. */
#define OGG_PROBE_MAX		(16U * 1024U * 1024U)

/* The bytes at the end read for the last granule positions. */
#define OGG_TAIL_MAX		(1024U * 1024U)

/* The bytes after a place a seek reads for the first stream's next granule position. */
#define OGG_SEEK_SCAN		(4U * 1024U * 1024U)

/* The bytes read at once while looking for a page. */
#define OGG_BLOCK		(64U * 1024U)

/* A granule position that is not known (-1 on a page that ends no packet). */
#define OGG_NO_GRANULE		(-1)

/* The most Vorbis modes. */
#define OGG_MODES_MAX		64U

/* The Theora version from which a granule position counts frames from 1 (3.2.1). */
#define OGG_THEORA_COUNTS	0x030201U

/*
 * One stream of the file: its serial number, kind and track, the header
 * packets still wanted and those kept (laid one after another, with their
 * lengths), the packet being put together across pages (and whether it
 * is known to be broken, or was begun on a page not read), the next page's
 * sequence number, and what its times need: Opus's pre-skip, Vorbis's
 * rate, block sizes and modes, Theora's frame rate, granule shift and
 * version, the block size of the last Vorbis packet, and the granule (in
 * the stream's units, frames for Theora) at which the next packet starts
 * (OGG_NO_GRANULE when it must be found from a page), and, after a seek,
 * the start before which its packets are not handed out.
 */
struct ogg_stream {
	uint32_t serial;
	unsigned kind;
	unsigned track;
	unsigned headers_wanted;
	unsigned headers_kept;
	unsigned char *headers;
	size_t headers_size;
	size_t header_lengths[3];
	unsigned char *partial;
	size_t partial_size;
	size_t partial_capacity;
	unsigned partial_open;
	unsigned partial_broken;
	uint32_t sequence;
	unsigned have_sequence;
	uint32_t preskip;
	uint32_t rate;
	unsigned blocksizes[2];
	unsigned char mode_flags[OGG_MODES_MAX];
	unsigned mode_count;
	uint32_t frame_rate;
	uint32_t frame_scale;
	unsigned shift;
	unsigned old_theora;
	unsigned last_block;
	int64_t next_granule;
	int64_t skip_before;
};

/*
 * A packet of the page being handed out: its stream, where its bytes are
 * in the page's packet buffer, how many, the granule it starts at (in its
 * stream's units) and whether a decoder can start at it.
 */
struct ogg_packet {
	unsigned stream;
	size_t offset;
	size_t size;
	int64_t start;
	unsigned key;
};

/*
 * The reader's state: the streams, the next page to read, the page read
 * (its bytes, its header's fields), the packets it ended (their bytes and
 * the next one to hand out), the stream that decides a seek, and a block
 * for finding pages.
 */
struct ogg_state {
	struct ogg_stream streams[MF_TRACK_MAX];
	unsigned stream_count;
	uint64_t position;
	unsigned char *page;
	size_t page_size;
	size_t page_body;
	unsigned page_flags;
	int64_t page_granule;
	uint32_t page_serial;
	uint32_t page_sequence;
	unsigned page_segments;
	unsigned char *packets;
	size_t packets_size;
	size_t packets_capacity;
	struct ogg_packet queue[255];
	unsigned queue_count;
	unsigned queue_next;
	unsigned lead;
	unsigned char *block;
};

/*
 * A reader of a packet's bits from the last one backwards (for a Vorbis
 * setup header's modes): the bytes, their count and the bits read.
 */
struct ogg_back_bits {
	const unsigned char *bytes;
	size_t size;
	size_t bit;
};

static int ogg_open(struct mf_file *file);
static int ogg_read(struct mf_file *file, struct mf_packet *packet);
static int ogg_seek(struct mf_file *file, int64_t time_us);
static void ogg_close(struct mf_file *file);
static int read_page(struct mf_file *file, struct ogg_state *state, uint64_t offset);
static int next_page(struct mf_file *file, struct ogg_state *state);
static int find_page(struct mf_file *file, struct ogg_state *state, uint64_t offset, uint64_t limit, uint64_t *found);
static uint32_t page_crc(const unsigned char *page, size_t size);
static uint32_t le32(const unsigned char *bytes);
static int64_t le64(const unsigned char *bytes);
static struct ogg_stream *stream_of(struct ogg_state *state, uint32_t serial, unsigned *index);
static int read_headers(struct mf_file *file, struct ogg_state *state);
static int begin_stream(struct mf_file *file, struct ogg_state *state, const unsigned char *packet, size_t size);
static int keep_header(struct mf_file *file, struct ogg_state *state, unsigned index, const unsigned char *packet, size_t size);
static int read_vorbis_id(struct ogg_stream *stream, struct mf_track *info, const unsigned char *packet, size_t size);
static int read_theora_id(struct ogg_stream *stream, struct mf_track *info, const unsigned char *packet, size_t size);
static void read_vorbis_modes(struct ogg_stream *stream, const unsigned char *packet, size_t size);
static unsigned back_bits(struct ogg_back_bits *bits, unsigned count);
static int keep_private(struct mf_file *file, struct ogg_stream *stream);
static int take_page(struct mf_file *file, struct ogg_state *state, int headers);
static int finish_packet(struct mf_file *file, struct ogg_state *state, unsigned index, int headers);
static void time_packets(struct ogg_state *state, unsigned index, unsigned first);
static int64_t packet_length(struct ogg_stream *stream, const unsigned char *data, size_t size);
static int64_t opus_samples(const unsigned char *data, size_t size);
static int64_t vorbis_samples(struct ogg_stream *stream, const unsigned char *data, size_t size);
static int64_t granule_end(const struct ogg_stream *stream, int64_t granule);
static int64_t granule_us(const struct ogg_stream *stream, int64_t start);
static int packet_key(const struct ogg_stream *stream, const unsigned char *data, size_t size);
static void find_length(struct mf_file *file, struct ogg_state *state);
static void reset_streams(struct ogg_state *state);
static int lead_page_at(struct mf_file *file, struct ogg_state *state, uint64_t offset, uint64_t *page, int64_t *granule);
static int seek_place(struct mf_file *file, struct ogg_state *state, int64_t target, uint64_t *place, int64_t *granule);
static void drop_open(struct mf_file *file, struct ogg_state *state);

/* The reader as mediafile.c calls it. */
const struct mf_format mf_ogg_format = {
	"ogg",
	ogg_open,
	ogg_read,
	ogg_seek,
	ogg_close,
};

/*
 * Tells whether a file's first bytes are an Ogg page that begins a stream.
 */
int
mf_ogg_detect(
	const unsigned char *head,
	size_t length)
{
	int compared;

	/* The capture pattern, version 0, and the flag of a stream's first page. */
	if (length < OGG_HEADER)
		return 0;
	compared = memcmp(head, "OggS", 4);
	if (compared != 0 || head[4] != 0)
		return 0;
	if ((head[5] & OGG_BEGIN) == 0)
		return 0;

	/* An Ogg file. */
	return 1;
}

/*
 * Reads the streams' first pages and header packets, then the length.
 */
static int
ogg_open(
	struct mf_file *file)
{
	struct ogg_state *state;
	int error;

	/* The state, kept in the file so that mf_close frees it on any failure. */
	state = calloc(1, sizeof(*state));
	if (state == NULL)
		return ENOMEM;
	file->state = state;

	/* The page buffer. */
	state->page = malloc(OGG_PAGE_MAX);
	if (state->page == NULL)
		return ENOMEM;

	/* The block for finding pages. */
	state->block = malloc(OGG_BLOCK);
	if (state->block == NULL)
		return ENOMEM;

	/* The streams and their headers. */
	error = read_headers(file, state);
	if (error != 0)
		return error;

	/* Refuses a file without a stream this reader knows. */
	if (file->track_count == 0)
		return EINVAL;

	/* The length, from the last granule positions. */
	find_length(file, state);

	/* Reading starts again at the start; the header packets are passed over. */
	state->position = 0;
	reset_streams(state);

	/* Succeeded: the tracks are known. */
	return 0;
}

/*
 * Hands out the next packet: the next one the page read ended, else the
 * packets of the next page.
 */
static int
ogg_read(
	struct mf_file *file,
	struct mf_packet *packet)
{
	struct ogg_state *state;
	struct ogg_stream *stream;
	struct ogg_packet *item;
	int error;

	/* A packet of a page, reading pages until one ends a packet to hand out. */
	state = file->state;
	for (;;) {
		/* The packets of the next page when this one's are out. */
		if (state->queue_next >= state->queue_count) {
			error = next_page(file, state);
			if (error != 0)
				return error;
			error = take_page(file, state, 0);
			if (error != 0)
				return error;
			continue;
		}

		/* The next packet; after a seek, a stream passes over its packets before the place. */
		item = &state->queue[state->queue_next];
		state->queue_next++;
		stream = &state->streams[item->stream];
		if (stream->skip_before != OGG_NO_GRANULE && item->start < stream->skip_before)
			continue;
		stream->skip_before = OGG_NO_GRANULE;
		break;
	}

	/* Its bytes. */
	error = mf_packet_room(file, item->size);
	if (error != 0)
		return error;

	/* The packet. */
	memcpy(file->buffer, state->packets + item->offset, item->size);
	packet->track = stream->track;
	packet->pts_us = granule_us(stream, item->start);
	packet->dts_us = packet->pts_us;
	packet->keyframe = (int)item->key;
	packet->data = file->buffer;
	packet->size = item->size;

	/* Succeeded: one packet. */
	return 0;
}

/*
 * Moves to the time: halves the file on the first stream's granule
 * positions for the last page that ends at or before it.  For Theora the
 * key frame before the time is the one that page (or the next page, when
 * its key frame is not after the time) names; reading goes back to the
 * last page that ends before that key frame, and the stream's packets
 * before it are not handed out.
 */
static int
ogg_seek(
	struct mf_file *file,
	int64_t time_us)
{
	struct ogg_state *state;
	struct ogg_stream *lead;
	uint64_t place;
	uint64_t next;
	int64_t granule;
	int64_t next_granule;
	int64_t key;
	int64_t next_key;
	int64_t target;
	int error;

	/* The lead stream, and the time in its units: samples, or for Theora the frames through it. */
	state = file->state;
	lead = &state->streams[state->lead];
	reset_streams(state);
	if (lead->kind == OGG_THEORA) {
		target = time_us / 1000000 * (int64_t)lead->frame_rate / (int64_t)lead->frame_scale;
		target += time_us % 1000000 * (int64_t)lead->frame_rate / (int64_t)lead->frame_scale / 1000000;
		target += 1;
	} else {
		target = time_us / 1000000 * (int64_t)lead->rate + time_us % 1000000 * (int64_t)lead->rate / 1000000;
		if (lead->kind == OGG_OPUS)
			target += lead->preskip;
	}

	/* The last page of the lead that ends at or before the time; none: the start. */
	error = seek_place(file, state, target, &place, &granule);
	if (error == ENOENT) {
		state->position = 0;
		return 0;
	}

	/* The reading's failure. */
	if (error != 0)
		return error;

	/* Sound is read from that page. */
	if (lead->kind != OGG_THEORA) {
		state->position = place;
		return 0;
	}

	/* The key frame that page names (counted through it, like the frames). */
	key = granule >> lead->shift;
	if (lead->old_theora)
		key++;

	/* The next page's key frame, when it is not after the time. */
	error = lead_page_at(file, state, place + 1U, &next, &next_granule);
	if (error == 0) {
		next_key = next_granule >> lead->shift;
		if (lead->old_theora)
			next_key++;
		if (next_key <= target && next_key > key)
			key = next_key;
	}

	/* The last page that ends before the key frame; none: the start. */
	error = seek_place(file, state, key - 1, &place, &granule);
	if (error == ENOENT)
		place = 0;
	else if (error != 0)
		return error;

	/* Succeeded: read from there, the lead's packets before the key frame passed over. */
	state->position = place;
	lead->skip_before = key - 1;
	return 0;
}

/*
 * Lets the reader's state go.
 */
static void
ogg_close(
	struct mf_file *file)
{
	struct ogg_state *state;
	unsigned i;

	/* Each stream's buffers. */
	state = file->state;
	for (i = 0; i < MF_TRACK_MAX; i++) {
		free(state->streams[i].headers);
		free(state->streams[i].partial);
	}

	/* The page, the packets, the block and the state. */
	free(state->page);
	free(state->packets);
	free(state->block);
	free(state);
	file->state = NULL;
}


/*
 * Reads the page at an offset into the page buffer and checks it: its
 * capture pattern, version, segments and CRC; its header's fields are
 * kept.  Returns 0, ENODATA when the file ends before the page does,
 * EINVAL for bytes that are not a page, EBADMSG for a page whose CRC is
 * wrong (its serial number is kept), or the reading's error.
 */
static int
read_page(
	struct mf_file *file,
	struct ogg_state *state,
	uint64_t offset)
{
	unsigned char *page;
	size_t lacing;
	size_t body;
	size_t i;
	uint32_t crc;
	uint32_t computed;
	int compared;
	int error;

	/* The fixed header. */
	page = state->page;
	if (offset > file->size || file->size - offset < OGG_HEADER)
		return ENODATA;
	error = mf_read_at(file, offset, page, OGG_HEADER);
	if (error != 0)
		return error;

	/* The capture pattern and version 0. */
	compared = memcmp(page, "OggS", 4);
	if (compared != 0 || page[4] != 0)
		return EINVAL;

	/* The lacing values. */
	lacing = page[26];
	if (file->size - offset < OGG_HEADER + lacing)
		return ENODATA;
	error = mf_read_at(file, offset + OGG_HEADER, page + OGG_HEADER, lacing);
	if (error != 0)
		return error;

	/* The body's length, the sum of the lacing values. */
	body = 0;
	for (i = 0; i < lacing; i++)
		body += page[OGG_HEADER + i];
	if (file->size - offset < OGG_HEADER + lacing + body)
		return ENODATA;
	error = mf_read_at(file, offset + OGG_HEADER + lacing, page + OGG_HEADER + lacing, body);
	if (error != 0)
		return error;

	/* The header's fields. */
	state->page_size = OGG_HEADER + lacing + body;
	state->page_body = OGG_HEADER + lacing;
	state->page_flags = page[5];
	state->page_granule = le64(page + 6);
	state->page_serial = le32(page + 14);
	state->page_sequence = le32(page + 18);
	state->page_segments = (unsigned)lacing;

	/* The CRC, computed with its own field as zeros. */
	crc = le32(page + 22);
	memset(page + 22, 0, 4);
	computed = page_crc(page, state->page_size);
	if (computed != crc)
		return EBADMSG;

	/* Succeeded: a whole page. */
	return 0;
}

/*
 * Reads the next page at the reading's place, finding the pages again
 * after bytes that are not one or a damaged page (the packet it broke is
 * counted as left out).  Returns 0, ENODATA at the end of the file, or
 * the reading's error.
 */
static int
next_page(
	struct mf_file *file,
	struct ogg_state *state)
{
	struct ogg_stream *stream;
	uint64_t found;
	unsigned index;
	int error;

	/* Pages until a whole one. */
	for (;;) {
		/* The page at the place. */
		error = read_page(file, state, state->position);
		if (error == 0) {
			state->position += state->page_size;
			return 0;
		}

		/* The end of the file: a packet still being put together is left out. */
		if (error == ENODATA) {
			drop_open(file, state);
			return ENODATA;
		}

		/* The reading's failure. */
		if (error != EINVAL && error != EBADMSG)
			return error;

		/* A damaged page of a stream: its packet being put together is broken, its times found again. */
		stream = NULL;
		if (error == EBADMSG)
			stream = stream_of(state, state->page_serial, &index);
		if (stream != NULL) {
			if (stream->partial_open)
				stream->partial_broken = 1U;
			else
				file->tracks[stream->track].dropped_count++;
			stream->next_granule = OGG_NO_GRANULE;
		}

		/* The next page after the place. */
		error = find_page(file, state, state->position + 1U, file->size, &found);
		if (error == ENODATA) {
			drop_open(file, state);
			return ENODATA;
		}

		/* The reading goes on there. */
		if (error != 0)
			return error;
		state->position = found;
	}
}

/*
 * Leaves out the packets still being put together at the end of the file
 * (counted).
 */
static void
drop_open(
	struct mf_file *file,
	struct ogg_state *state)
{
	unsigned i;

	/* Each stream with a packet begun. */
	for (i = 0; i < state->stream_count; i++) {
		/* One begun and not ended. */
		if (!state->streams[i].partial_open)
			continue;
		state->streams[i].partial_open = 0;
		file->tracks[state->streams[i].track].dropped_count++;
	}
}

/*
 * Finds the first whole page (its CRC right) at or after an offset and
 * before a limit; it is left in the page buffer.  Returns 0 with its
 * offset, ENODATA when there is none, or the reading's error.
 */
static int
find_page(
	struct mf_file *file,
	struct ogg_state *state,
	uint64_t offset,
	uint64_t limit,
	uint64_t *found)
{
	unsigned char *bytes;
	size_t length;
	size_t i;
	int error;

	/* A page right at the offset (the usual case, page after page). */
	if (limit > file->size)
		limit = file->size;
	error = read_page(file, state, offset);
	if (error == 0 && offset < limit) {
		*found = offset;
		return 0;
	}

	/* The reading's failure (bytes that are not a page are looked through below). */
	if (error != 0 &&
	    error != EINVAL &&
	    error != EBADMSG &&
	    error != ENODATA)
		return error;

	/* Else the bytes from the offset, a block at a time. */
	while (offset + OGG_HEADER <= limit) {
		/* A block. */
		length = OGG_BLOCK;
		if (file->size - offset < length)
			length = (size_t)(file->size - offset);
		error = mf_read_at(file, offset, state->block, length);
		if (error != 0)
			return error;
		bytes = state->block;

		/* Each "OggS" in it that starts a whole page. */
		for (i = 0; i + 4U <= length; i++) {
			/* The capture pattern. */
			if (bytes[i] != 'O' || bytes[i + 1] != 'g')
				continue;
			if (bytes[i + 2] != 'g' || bytes[i + 3] != 'S')
				continue;

			/* A whole page there (read_page uses the page buffer, not the block). */
			error = read_page(file, state, offset + i);
			if (error == 0) {
				*found = offset + i;
				return 0;
			}

			/* Bytes that only look like a page are passed; the reading's failure is not. */
			if (error != EINVAL &&
			    error != EBADMSG &&
			    error != ENODATA)
				return error;
		}

		/* The next block, overlapping by the pattern's length. */
		if (length < 4U)
			break;
		offset += length - 3U;
	}

	/* No page. */
	return ENODATA;
}

/*
 * Computes the CRC of a page (Ogg's: the polynomial 0x04c11db7, high bit
 * first, starting from 0, without a final step).
 */
static uint32_t
page_crc(
	const unsigned char *page,
	size_t size)
{
	uint32_t crc;
	size_t i;
	unsigned bit;

	/* Each byte, a bit at a time. */
	crc = 0;
	for (i = 0; i < size; i++) {
		crc ^= (uint32_t)page[i] << 24;
		for (bit = 0; bit < 8U; bit++) {
			/* The high bit out, the polynomial in. */
			if ((crc & 0x80000000U) != 0U)
				crc = (crc << 1) ^ 0x04c11db7U;
			else
				crc <<= 1;
		}
	}

	/* The CRC. */
	return crc;
}

/* Reads a little-endian 32-bit number. */
static uint32_t
le32(
	const unsigned char *bytes)
{
	/* Low byte first. */
	return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) | ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
}

/* Reads a little-endian signed 64-bit number. */
static int64_t
le64(
	const unsigned char *bytes)
{
	uint64_t value;

	/* The high half after the low one. */
	value = (uint64_t)le32(bytes) | ((uint64_t)le32(bytes + 4) << 32);
	return (int64_t)value;
}

/* Finds the stream of a serial number (and its index), or NULL. */
static struct ogg_stream *
stream_of(
	struct ogg_state *state,
	uint32_t serial,
	unsigned *index)
{
	unsigned i;

	/* Each stream. */
	for (i = 0; i < state->stream_count; i++) {
		/* The one with the number. */
		if (state->streams[i].serial == serial) {
			*index = i;
			return &state->streams[i];
		}
	}

	/* None. */
	return NULL;
}

/*
 * Reads the start of the file: the streams' first pages, then their
 * header packets until each stream has all its own (or OGG_PROBE_MAX bytes
 * were read).  The first Theora stream (else the first) decides a seek.
 */
static int
read_headers(
	struct mf_file *file,
	struct ogg_state *state)
{
	unsigned waiting;
	unsigned i;
	int error;

	/* Pages until no stream waits for a header. */
	state->position = 0;
	for (;;) {
		/* Stops after OGG_PROBE_MAX bytes or at the end. */
		if (state->position > OGG_PROBE_MAX)
			break;
		error = next_page(file, state);
		if (error == ENODATA)
			break;
		if (error != 0)
			return error;

		/* The page's headers (and a new stream's first). */
		error = take_page(file, state, 1);
		if (error != 0)
			return error;

		/* Whether a stream still waits for a header; the first pages of the streams come first. */
		waiting = 0;
		for (i = 0; i < state->stream_count; i++) {
			/* One that does. */
			if (state->streams[i].headers_wanted != 0)
				waiting = 1U;
		}

		/* Done when past the first pages and no stream waits. */
		if ((state->page_flags & OGG_BEGIN) == 0 && !waiting)
			break;
	}

	/* A stream without all its headers cannot be played: its track is left as other. */
	for (i = 0; i < state->stream_count; i++) {
		/* One whose headers did not all come. */
		if (state->streams[i].headers_wanted != 0)
			file->tracks[state->streams[i].track].kind = MF_TRACK_OTHER;
	}

	/* The stream that decides a seek. */
	state->lead = 0;
	for (i = state->stream_count; i > 0; i--) {
		/* A Theora stream before the one found so far. */
		if (state->streams[i - 1U].kind == OGG_THEORA)
			state->lead = i - 1U;
	}

	/* Succeeded: the streams are known. */
	return 0;
}

/*
 * Begins a stream from the first packet of its first page: its kind from
 * the packet, a track, and the packet kept as its first header.  A stream
 * of a kind this reader does not know is not read.  Returns 0 or ENOMEM.
 */
static int
begin_stream(
	struct mf_file *file,
	struct ogg_state *state,
	const unsigned char *packet,
	size_t size)
{
	struct ogg_stream *stream;
	struct mf_track *info;
	unsigned index;
	int compared;
	int error;

	/* Room for another stream and track. */
	if (state->stream_count == MF_TRACK_MAX || file->track_count == MF_TRACK_MAX)
		return 0;
	index = state->stream_count;
	stream = &state->streams[index];
	info = &file->tracks[file->track_count];
	memset(stream, 0, sizeof(*stream));
	memset(info, 0, sizeof(*info));

	/* Opus: OpusHead, its channels, pre-skip; 48 kHz always. */
	compared = 1;
	if (size >= 19U)
		compared = memcmp(packet, "OpusHead", 8);
	if (compared == 0) {
		stream->kind = OGG_OPUS;
		stream->headers_wanted = 2U;
		stream->rate = 48000U;
		stream->preskip = (uint32_t)packet[10] | ((uint32_t)packet[11] << 8);
		info->kind = MF_TRACK_AUDIO;
		info->codec = MF_CODEC_OPUS;
		info->sample_rate = 48000U;
		info->channels = packet[9];
		mf_set_codec_name(info, "opus", 4U);
	}

	/* Vorbis: its identification header. */
	compared = 1;
	if (size >= 30U)
		compared = memcmp(packet, "\001vorbis", 7);
	if (compared == 0) {
		error = read_vorbis_id(stream, info, packet, size);
		if (error != 0)
			return 0;
	}

	/* Theora: its identification header. */
	compared = 1;
	if (size >= 42U)
		compared = memcmp(packet, "\200theora", 7);
	if (compared == 0) {
		error = read_theora_id(stream, info, packet, size);
		if (error != 0)
			return 0;
	}

	/* A kind not known: the stream is not read. */
	if (stream->kind == 0)
		return 0;

	/* The stream and its track. */
	stream->serial = state->page_serial;
	stream->track = file->track_count;
	stream->next_granule = OGG_NO_GRANULE;
	stream->skip_before = OGG_NO_GRANULE;
	state->stream_count++;
	file->track_count++;

	/* The packet, its first header. */
	error = keep_header(file, state, index, packet, size);
	if (error != 0)
		return error;

	/* Succeeded: the stream is read from here. */
	return 0;
}

/* Reads Vorbis's identification header: channels, rate and block sizes; 0 or EINVAL. */
static int
read_vorbis_id(
	struct ogg_stream *stream,
	struct mf_track *info,
	const unsigned char *packet,
	size_t size)
{
	unsigned small;
	unsigned large;

	/* The block sizes, powers of two from 64 to 8192, the small first. */
	if (size < 30U)
		return EINVAL;
	small = packet[28] & 0x0fU;
	large = packet[28] >> 4;
	if (small < 6U ||
	    large > 13U ||
	    small > large)
		return EINVAL;

	/* The stream. */
	stream->kind = OGG_VORBIS;
	stream->headers_wanted = 3U;
	stream->rate = le32(packet + 12);
	stream->blocksizes[0] = 1U << small;
	stream->blocksizes[1] = 1U << large;
	if (stream->rate == 0)
		return EINVAL;

	/* The track. */
	info->kind = MF_TRACK_AUDIO;
	info->codec = MF_CODEC_VORBIS;
	info->sample_rate = stream->rate;
	info->channels = packet[11];
	mf_set_codec_name(info, "vorbis", 6U);

	/* Succeeded: a Vorbis stream. */
	return 0;
}

/* Reads Theora's identification header: picture size, frame rate, granule shift and version; 0 or EINVAL. */
static int
read_theora_id(
	struct ogg_stream *stream,
	struct mf_track *info,
	const unsigned char *packet,
	size_t size)
{
	uint32_t version;

	/* The version, the frame rate's numerator and denominator. */
	if (size < 42U)
		return EINVAL;
	version = ((uint32_t)packet[7] << 16) | ((uint32_t)packet[8] << 8) | packet[9];
	stream->frame_rate = mf_be32(packet + 22);
	stream->frame_scale = mf_be32(packet + 26);
	if (stream->frame_rate == 0 || stream->frame_scale == 0)
		return EINVAL;

	/* The stream: the granule shift in bytes 40 and 41, and whether its granules count from 0. */
	stream->kind = OGG_THEORA;
	stream->headers_wanted = 3U;
	stream->shift = ((unsigned)(packet[40] & 0x03U) << 3) | (packet[41] >> 5);
	stream->old_theora = version < OGG_THEORA_COUNTS;
	stream->rate = stream->frame_rate;

	/* The track, its picture's size (24 bits each). */
	info->kind = MF_TRACK_VIDEO;
	info->codec = MF_CODEC_THEORA;
	info->width = ((uint32_t)packet[14] << 16) | ((uint32_t)packet[15] << 8) | packet[16];
	info->height = ((uint32_t)packet[17] << 16) | ((uint32_t)packet[18] << 8) | packet[19];
	mf_set_codec_name(info, "theora", 6U);

	/* Succeeded: a Theora stream. */
	return 0;
}

/*
 * Keeps a header packet of a stream; the setup header of Vorbis gives its
 * modes, and the last one makes the track's private data.  Returns 0 or
 * ENOMEM.
 */
static int
keep_header(
	struct mf_file *file,
	struct ogg_state *state,
	unsigned index,
	const unsigned char *packet,
	size_t size)
{
	struct ogg_stream *stream;
	unsigned char *headers;
	int error;

	/* No more wanted, or a header too large to keep. */
	stream = &state->streams[index];
	if (stream->headers_wanted == 0 || stream->headers_kept >= 3U)
		return 0;
	if (size > MF_PRIVATE_MAX)
		return 0;

	/* Kept after the others. */
	headers = realloc(stream->headers, stream->headers_size + size + 1U);
	if (headers == NULL)
		return ENOMEM;
	stream->headers = headers;
	memcpy(stream->headers + stream->headers_size, packet, size);
	stream->headers_size += size;
	stream->header_lengths[stream->headers_kept] = size;
	stream->headers_kept++;
	stream->headers_wanted--;

	/* Vorbis's third header is the setup header, with the modes. */
	if (stream->kind == OGG_VORBIS && stream->headers_kept == 3U)
		read_vorbis_modes(stream, packet, size);

	/* The last header: the private data. */
	if (stream->headers_wanted == 0) {
		error = keep_private(file, stream);
		if (error != 0)
			return error;
	}

	/* Succeeded: the header is kept. */
	return 0;
}

/*
 * Reads the modes of a Vorbis setup header from its end, as they are the
 * last thing in it (FFmpeg's and libvorbis's readers do the same): the
 * framing bit, then modes of 41 bits each back from it (a block flag, a
 * window and a transform type of 16 zero bits, a mapping below 64), as
 * many as the count before them says.  A header that cannot be read
 * leaves the stream without modes (its packets' lengths are then 0).
 */
static void
read_vorbis_modes(
	struct ogg_stream *stream,
	const unsigned char *packet,
	size_t size)
{
	struct ogg_back_bits bits;
	struct ogg_back_bits count_bits;
	size_t framing;
	unsigned count;
	unsigned last_count;
	unsigned mapping;
	unsigned transform;
	unsigned window;
	unsigned counted;
	unsigned bit;
	unsigned i;

	/* The framing bit: the last bit set. */
	bits.bytes = packet;
	bits.size = size;
	bits.bit = 0;
	framing = 0;
	while (bits.bit + 97U <= size * 8U) {
		/* One bit back; the first set one. */
		bit = back_bits(&bits, 1U);
		if (bit != 0) {
			framing = bits.bit;
			break;
		}
	}

	/* No framing bit. */
	if (framing == 0)
		return;

	/* The modes back from it while they look like modes; the count before the last one that agrees. */
	count = 0;
	last_count = 0;
	while (bits.bit + 97U <= size * 8U) {
		/* A mode's mapping, transform and window, read backwards (bit-reversed, still zero or small). */
		mapping = back_bits(&bits, 8U);
		transform = back_bits(&bits, 16U);
		window = back_bits(&bits, 16U);
		if (mapping > 63U ||
		    transform != 0 ||
		    window != 0)
			break;
		(void)back_bits(&bits, 1U);
		count++;
		if (count > OGG_MODES_MAX)
			break;

		/* The count (6 bits) just before these modes, when it is theirs. */
		count_bits = bits;
		counted = back_bits(&count_bits, 6U);
		if (counted + 1U == count)
			last_count = count;
	}

	/* No count agreed. */
	if (last_count == 0)
		return;

	/* The block flags again, from the framing bit, the last mode first. */
	bits.bit = framing;
	for (i = last_count; i > 0; i--) {
		/* Past the mapping and the types, the flag. */
		(void)back_bits(&bits, 40U);
		stream->mode_flags[i - 1U] = (unsigned char)back_bits(&bits, 1U);
	}

	/* The modes are known. */
	stream->mode_count = last_count;
}

/*
 * Reads count bits backwards from the end of a packet, the bit after the
 * last first (as a reader of the bytes reversed, high bit first, would).
 */
static unsigned
back_bits(
	struct ogg_back_bits *bits,
	unsigned count)
{
	unsigned value;
	size_t byte;
	unsigned i;

	/* Each bit. */
	value = 0;
	for (i = 0; i < count; i++) {
		/* Past the start: zero bits. */
		value <<= 1;
		if (bits->bit >= bits->size * 8U)
			continue;

		/* The byte from the end and its bit from the high one. */
		byte = bits->size - 1U - bits->bit / 8U;
		value |= (bits->bytes[byte] >> (7U - bits->bit % 8U)) & 1U;
		bits->bit++;
	}

	/* The bits read. */
	return value;
}

/*
 * Makes a stream's track's private data from its headers: OpusHead for
 * Opus, the three headers in Xiph lacing for Vorbis and Theora.  Returns
 * 0 or ENOMEM.
 */
static int
keep_private(
	struct mf_file *file,
	struct ogg_stream *stream)
{
	struct mf_track *info;
	unsigned char *laced;
	size_t length;
	size_t at;
	size_t n;
	unsigned i;
	int error;

	/* Opus: its first header. */
	info = &file->tracks[stream->track];
	if (stream->kind == OGG_OPUS) {
		error = mf_keep_private(info, stream->headers, stream->header_lengths[0]);
		if (error == ENOMEM)
			return error;
		return 0;
	}

	/* The others: 2, the first two lengths in pieces of 255, then the three headers. */
	length = 1U + stream->header_lengths[0] / 255U + 1U + stream->header_lengths[1] / 255U + 1U + stream->headers_size;
	laced = malloc(length);
	if (laced == NULL)
		return ENOMEM;
	laced[0] = 2U;
	at = 1U;
	for (i = 0; i < 2U; i++) {
		/* A length: 255 for each whole 255, then the rest. */
		for (n = stream->header_lengths[i]; n >= 255U; n -= 255U) {
			laced[at] = 255U;
			at++;
		}

		/* The rest. */
		laced[at] = (unsigned char)n;
		at++;
	}

	/* The headers after the lengths. */
	memcpy(laced + at, stream->headers, stream->headers_size);

	/* Kept by the track. */
	error = mf_keep_private(info, laced, length);
	free(laced);
	if (error == ENOMEM)
		return error;

	/* Succeeded: the private data is kept (one too large is not). */
	return 0;
}

/*
 * Takes the page read: its packets are put together in its stream's
 * buffer and the ones that end on it are timed and queued (while the
 * headers are read, a stream's header packets are kept instead, and a new
 * stream's first page begins it).  A page of a stream not read, or a lost
 * page before it, is passed over (the broken packet counted).  Returns 0
 * or ENOMEM.
 */
static int
take_page(
	struct mf_file *file,
	struct ogg_state *state,
	int headers)
{
	struct ogg_stream *stream;
	const unsigned char *lacing;
	const unsigned char *body;
	size_t offset;
	size_t length;
	unsigned char *partial;
	unsigned skipping;
	unsigned index;
	unsigned first;
	unsigned i;
	int error;

	/* Nothing queued from the page before. */
	state->queue_count = 0;
	state->queue_next = 0;
	state->packets_size = 0;
	lacing = state->page + OGG_HEADER;
	body = state->page + state->page_body;

	/* The page's stream; a new one's first page begins it, while the headers are read. */
	stream = stream_of(state, state->page_serial, &index);
	if (stream == NULL) {
		if (!headers || (state->page_flags & OGG_BEGIN) == 0)
			return 0;
		length = 0;
		for (i = 0; i < state->page_segments; i++) {
			/* The first packet runs to the first lacing value below 255. */
			length += lacing[i];
			if (lacing[i] < 255U)
				break;
		}

		/* The stream begun from it. */
		error = begin_stream(file, state, body, length);
		return error;
	}

	/* A page lost before this one breaks the packet begun and the times. */
	if (stream->have_sequence && state->page_sequence != stream->sequence) {
		if (stream->partial_open)
			file->tracks[stream->track].dropped_count++;
		stream->partial_open = 0;
		stream->next_granule = OGG_NO_GRANULE;
	}

	/* The page after this one is wanted next. */
	stream->sequence = state->page_sequence + 1U;
	stream->have_sequence = 1U;

	/* A packet begun that this page does not go on with never ended. */
	if ((state->page_flags & OGG_CONTINUED) == 0 && stream->partial_open) {
		file->tracks[stream->track].dropped_count++;
		stream->partial_open = 0;
	}

	/* A page that goes on with a packet begun on a page not read: that piece is passed over. */
	skipping = 0;
	if ((state->page_flags & OGG_CONTINUED) != 0 && !stream->partial_open)
		skipping = 1U;

	/* Each segment. */
	first = state->queue_count;
	offset = 0;
	for (i = 0; i < state->page_segments; i++) {
		/* The segment's bytes. */
		length = lacing[i];

		/* The rest of a packet not read. */
		if (skipping) {
			offset += length;
			if (length < 255U)
				skipping = 0;
			continue;
		}

		/* A new packet. */
		if (!stream->partial_open) {
			stream->partial_open = 1U;
			stream->partial_broken = 0;
			stream->partial_size = 0;
		}

		/* The bytes added, unless the packet grew too large (then it is broken). */
		if (stream->partial_size + length > OGG_PACKET_MAX)
			stream->partial_broken = 1U;
		if (!stream->partial_broken && stream->partial_capacity - stream->partial_size < length) {
			partial = realloc(stream->partial, stream->partial_size + length + 4096U);
			if (partial == NULL)
				return ENOMEM;
			stream->partial = partial;
			stream->partial_capacity = stream->partial_size + length + 4096U;
		}

		/* The segment's bytes in the packet. */
		if (!stream->partial_broken) {
			memcpy(stream->partial + stream->partial_size, body + offset, length);
			stream->partial_size += length;
		}

		/* The next segment. */
		offset += length;

		/* A lacing value below 255 ends the packet. */
		if (length < 255U) {
			stream->partial_open = 0;
			error = finish_packet(file, state, index, headers);
			if (error != 0)
				return error;
		}
	}

	/* The times of the packets the page ended. */
	if (!headers)
		time_packets(state, index, first);

	/* Succeeded: the page is taken. */
	return 0;
}

/*
 * Finishes a packet put together in a stream's buffer: a broken one is
 * counted, a header is kept (while the headers are read) or passed over,
 * and any other is queued (when packets are handed out).  Returns 0 or
 * ENOMEM.
 */
static int
finish_packet(
	struct mf_file *file,
	struct ogg_state *state,
	unsigned index,
	int headers)
{
	struct ogg_stream *stream;
	struct ogg_packet *item;
	unsigned char *packets;
	const unsigned char *data;
	size_t size;
	size_t capacity;
	int header;
	int compared;
	int error;

	/* A broken packet is left out. */
	stream = &state->streams[index];
	if (stream->partial_broken) {
		file->tracks[stream->track].dropped_count++;
		return 0;
	}

	/* Whether it is a header: Opus's two by their names, Vorbis's and Theora's by their first bit. */
	data = stream->partial;
	size = stream->partial_size;
	header = 0;
	if (stream->kind == OGG_OPUS && size >= 8U) {
		compared = memcmp(data, "OpusHead", 8);
		if (compared == 0)
			header = 1;
		compared = memcmp(data, "OpusTags", 8);
		if (compared == 0)
			header = 1;
	}

	/* A Vorbis header's first byte is odd. */
	if (stream->kind == OGG_VORBIS && size >= 1U) {
		if ((data[0] & 0x01U) != 0U)
			header = 1;
	}

	/* A Theora header's first byte has its high bit. */
	if (stream->kind == OGG_THEORA && size >= 1U) {
		if ((data[0] & 0x80U) != 0U)
			header = 1;
	}

	/* A header: kept while the headers are read, else passed over. */
	if (header) {
		if (!headers)
			return 0;
		error = keep_header(file, state, index, data, size);
		return error;
	}

	/* While the headers are read, nothing else is wanted. */
	if (headers)
		return 0;

	/* Room in the page's packet buffer. */
	if (state->packets_capacity - state->packets_size < size + 1U) {
		capacity = state->packets_size + size + 65536U;
		packets = realloc(state->packets, capacity);
		if (packets == NULL)
			return ENOMEM;
		state->packets = packets;
		state->packets_capacity = capacity;
	}

	/* Queued (a page ends at most 255 packets). */
	item = &state->queue[state->queue_count];
	item->stream = index;
	item->offset = state->packets_size;
	item->size = size;
	item->start = 0;
	item->key = (unsigned)packet_key(stream, data, size);
	if (size != 0)
		memcpy(state->packets + state->packets_size, data, size);
	state->packets_size += size;
	state->queue_count++;

	/* Succeeded: the packet is queued. */
	return 0;
}

/*
 * Times the packets a page ended (from first in the queue): each one's
 * length in its stream's units, and its start going on from the packet
 * before; when that is not known, back from the end the page's granule
 * position gives.
 */
static void
time_packets(
	struct ogg_state *state,
	unsigned index,
	unsigned first)
{
	struct ogg_stream *stream;
	struct ogg_packet *item;
	int64_t lengths[255];
	int64_t total;
	int64_t start;
	unsigned i;

	/* A page that ended no packet (a header's page among them) tells nothing of the times. */
	stream = &state->streams[index];
	if (first >= state->queue_count)
		return;

	/* Each packet's length, in order (a Vorbis packet's depends on the one before). */
	total = 0;
	for (i = first; i < state->queue_count; i++) {
		item = &state->queue[i];
		lengths[i] = packet_length(stream, state->packets + item->offset, item->size);
		total += lengths[i];
	}

	/* The start of the first: going on, else back from the page's end, else unknown (0). */
	start = 0;
	if (stream->next_granule != OGG_NO_GRANULE)
		start = stream->next_granule;
	else if (state->page_granule != OGG_NO_GRANULE)
		start = granule_end(stream, state->page_granule) - total;

	/* Each start, one after another. */
	for (i = first; i < state->queue_count; i++) {
		state->queue[i].start = start;
		start += lengths[i];
	}

	/* The next packet goes on from there, when this page's times were known. */
	if (stream->next_granule != OGG_NO_GRANULE || state->page_granule != OGG_NO_GRANULE)
		stream->next_granule = start;
}

/* Tells a packet's length in its stream's units: samples, or one frame of Theora. */
static int64_t
packet_length(
	struct ogg_stream *stream,
	const unsigned char *data,
	size_t size)
{
	int64_t length;

	/* By the stream's kind. */
	length = 1;
	if (stream->kind == OGG_OPUS)
		length = opus_samples(data, size);
	else if (stream->kind == OGG_VORBIS)
		length = vorbis_samples(stream, data, size);

	/* The length. */
	return length;
}

/*
 * Tells an Opus packet's samples (48 kHz) from its TOC byte: the frame's
 * length by the configuration, times the frames the code says.
 */
static int64_t
opus_samples(
	const unsigned char *data,
	size_t size)
{
	static const unsigned silk[4] = { 480U, 960U, 1920U, 2880U };
	static const unsigned hybrid[2] = { 480U, 960U };
	static const unsigned celt[4] = { 120U, 240U, 480U, 960U };
	unsigned config;
	unsigned frame;
	unsigned frames;

	/* An empty packet has none. */
	if (size == 0)
		return 0;

	/* The frame's length by the configuration: SILK, hybrid, CELT. */
	config = data[0] >> 3;
	if (config < 12U)
		frame = silk[config & 3U];
	else if (config < 16U)
		frame = hybrid[config & 1U];
	else
		frame = celt[config & 3U];

	/* The frames: one, two, or the count in the next byte. */
	frames = 1U;
	if ((data[0] & 3U) == 1U || (data[0] & 3U) == 2U)
		frames = 2U;
	if ((data[0] & 3U) == 3U && size >= 2U)
		frames = data[1] & 0x3fU;

	/* The samples. */
	return (int64_t)frame * frames;
}

/*
 * Tells a Vorbis packet's samples: a quarter of the block size before it
 * (its own when there is none) and a quarter of its own, its block size
 * from its mode's flag.
 */
static int64_t
vorbis_samples(
	struct ogg_stream *stream,
	const unsigned char *data,
	size_t size)
{
	unsigned mode;
	unsigned bits;
	unsigned block;
	unsigned before;
	unsigned i;

	/* A stream without modes, an empty or a header packet: none. */
	if (stream->mode_count == 0 ||
	    size == 0 ||
	    (data[0] & 0x01U) != 0U)
		return 0;

	/* The mode, in the bits after the packet type (low bits first), as many as the modes need. */
	bits = 0;
	while ((1U << bits) < stream->mode_count)
		bits++;
	mode = 0;
	for (i = 0; i < bits; i++) {
		/* Bit 1 + i of the packet (past its end: 0). */
		if ((1U + i) / 8U >= size)
			break;
		if (((data[(1U + i) / 8U] >> ((1U + i) % 8U)) & 1U) != 0U)
			mode |= 1U << i;
	}

	/* A mode the stream does not have. */
	if (mode >= stream->mode_count)
		return 0;

	/* Its block size, and the one before (its own for the first). */
	block = stream->blocksizes[stream->mode_flags[mode]];
	before = stream->last_block;
	if (before == 0)
		before = block;
	stream->last_block = block;

	/* The samples it adds. */
	return (int64_t)(before / 4U + block / 4U);
}

/*
 * Tells where a page's granule position ends, in the stream's units: the
 * samples for sound, the frames through it for Theora (the key frame's
 * number and the frames since it).
 */
static int64_t
granule_end(
	const struct ogg_stream *stream,
	int64_t granule)
{
	int64_t frames;

	/* Sound: the samples. */
	if (stream->kind != OGG_THEORA)
		return granule;

	/* Theora: the key frame's count and the frames after it (counted from 0 before version 3.2.1). */
	frames = (granule >> stream->shift) + (granule & (((int64_t)1 << stream->shift) - 1));
	if (stream->old_theora)
		frames++;

	/* The frames through the page. */
	return frames;
}

/* Turns a packet's start in its stream's units into microseconds (Opus's pre-skip taken off). */
static int64_t
granule_us(
	const struct ogg_stream *stream,
	int64_t start)
{
	int64_t time;

	/* Theora: frames at the frame rate. */
	if (stream->kind == OGG_THEORA) {
		time = mf_scale_us(start * (int64_t)stream->frame_scale, stream->frame_rate);
		return time;
	}

	/* Opus: after its pre-skip. */
	if (stream->kind == OGG_OPUS)
		start -= stream->preskip;

	/* Samples at the rate. */
	time = mf_scale_us(start, stream->rate);
	return time;
}

/* Tells whether a decoder can start at a packet: every sound packet, a Theora intra frame. */
static int
packet_key(
	const struct ogg_stream *stream,
	const unsigned char *data,
	size_t size)
{
	/* Sound. */
	if (stream->kind != OGG_THEORA)
		return 1;

	/* A Theora data packet whose frame type bit is 0 (an empty packet repeats the frame before). */
	if (size == 0)
		return 0;
	if ((data[0] & 0x40U) != 0U)
		return 0;

	/* An intra frame. */
	return 1;
}

/*
 * Finds the file's length and each track's: the last granule position of
 * each stream in the last OGG_TAIL_MAX bytes.
 */
static void
find_length(
	struct mf_file *file,
	struct ogg_state *state)
{
	struct ogg_stream *stream;
	struct mf_track *info;
	uint64_t offset;
	uint64_t found;
	int64_t length;
	unsigned index;
	int error;

	/* Each page of the tail. */
	offset = 0;
	if (file->size > OGG_TAIL_MAX)
		offset = file->size - OGG_TAIL_MAX;
	for (;;) {
		/* The next page; none ends the search. */
		error = find_page(file, state, offset, file->size, &found);
		if (error != 0)
			break;
		offset = found + state->page_size;

		/* A page of a stream that ends a packet. */
		stream = stream_of(state, state->page_serial, &index);
		if (stream == NULL || state->page_granule == OGG_NO_GRANULE)
			continue;

		/* The stream's length so far. */
		length = granule_us(stream, granule_end(stream, state->page_granule));
		info = &file->tracks[stream->track];
		if (length > info->duration_us)
			info->duration_us = length;
		if (length > file->duration_us)
			file->duration_us = length;
	}
}

/*
 * Lets every stream's packet being put together, its page count and its
 * times go (the reading moves), and the queue.
 */
static void
reset_streams(
	struct ogg_state *state)
{
	unsigned i;

	/* Each stream. */
	for (i = 0; i < state->stream_count; i++) {
		state->streams[i].partial_open = 0;
		state->streams[i].partial_size = 0;
		state->streams[i].have_sequence = 0;
		state->streams[i].last_block = 0;
		state->streams[i].next_granule = OGG_NO_GRANULE;
		state->streams[i].skip_before = OGG_NO_GRANULE;
	}

	/* Nothing queued. */
	state->queue_count = 0;
	state->queue_next = 0;
}

/*
 * Finds the first page of the lead stream with a granule position at or
 * after an offset (within OGG_SEEK_SCAN bytes).  Returns 0 with its offset
 * and granule position, ENODATA when there is none, or the reading's
 * error.
 */
static int
lead_page_at(
	struct mf_file *file,
	struct ogg_state *state,
	uint64_t offset,
	uint64_t *page,
	int64_t *granule)
{
	uint64_t limit;
	uint64_t found;
	uint32_t serial;
	int error;

	/* Pages from the offset to the limit. */
	serial = state->streams[state->lead].serial;
	limit = offset + OGG_SEEK_SCAN;
	for (;;) {
		/* The next page. */
		error = find_page(file, state, offset, limit, &found);
		if (error != 0)
			return error;
		offset = found + state->page_size;

		/* The lead's, ending a packet. */
		if (state->page_serial != serial || state->page_granule == OGG_NO_GRANULE)
			continue;

		/* Succeeded: the page. */
		*page = found;
		*granule = state->page_granule;
		return 0;
	}
}

/*
 * Finds the last page of the lead stream whose end (granule_end) is at or
 * before a target, by halving the file.  Returns 0 with its offset and
 * granule position, ENOENT when no page ends that early, or the reading's
 * error.
 */
static int
seek_place(
	struct mf_file *file,
	struct ogg_state *state,
	int64_t target,
	uint64_t *place,
	int64_t *granule)
{
	struct ogg_stream *lead;
	uint64_t low;
	uint64_t high;
	uint64_t middle;
	uint64_t page;
	int64_t found;
	int64_t end;
	int error;

	/* Halves the file: a page at or after low ends in time, none at or after high does. */
	lead = &state->streams[state->lead];
	low = 0;
	high = file->size;
	while (high - low > 1U) {
		/* The first lead page from the middle. */
		middle = low + (high - low) / 2U;
		error = lead_page_at(file, state, middle, &page, &found);
		if (error != 0 && error != ENODATA)
			return error;
		end = target + 1;
		if (error == 0)
			end = granule_end(lead, found);
		if (end > target)
			high = middle;
		else
			low = middle;
	}

	/* The page from low, when it ends in time. */
	error = lead_page_at(file, state, low, &page, &found);
	if (error == ENODATA)
		return ENOENT;
	if (error != 0)
		return error;
	end = granule_end(lead, found);
	if (end > target)
		return ENOENT;

	/* Succeeded: the page. */
	*place = page;
	*granule = found;
	return 0;
}
