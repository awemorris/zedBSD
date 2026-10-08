/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The Matroska and WebM reader (WS122 p003).  The EBML header names the
 * document type; the segment's Info, Tracks, Cues and SeekHead (the
 * elements before the first cluster, and the cues wherever the seek head
 * says) are read whole; the clusters are then read in order, their
 * SimpleBlocks and BlockGroups giving the packets.  A laced block (Xiph,
 * fixed or EBML lacing) gives one packet a frame.  Matroska stores
 * presentation times only, so a packet's decoding time is its
 * presentation time.  Seeking uses the cues, or reads the clusters' times
 * when there are none.
 */

#include "mediafile-private.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>

/* The element IDs this reader uses (with their length markers, as written). */
#define MKV_EBML		0x1a45dfa3U
#define MKV_DOCTYPE		0x4282U
#define MKV_SEGMENT		0x18538067U
#define MKV_SEEKHEAD		0x114d9b74U
#define MKV_SEEK		0x4dbbU
#define MKV_SEEKID		0x53abU
#define MKV_SEEKPOSITION	0x53acU
#define MKV_INFO		0x1549a966U
#define MKV_TIMECODESCALE	0x2ad7b1U
#define MKV_DURATION		0x4489U
#define MKV_TRACKS		0x1654ae6bU
#define MKV_TRACKENTRY		0xaeU
#define MKV_TRACKNUMBER		0xd7U
#define MKV_TRACKTYPE		0x83U
#define MKV_CODECID		0x86U
#define MKV_CODECPRIVATE	0x63a2U
#define MKV_VIDEO		0xe0U
#define MKV_PIXELWIDTH		0xb0U
#define MKV_PIXELHEIGHT		0xbaU
#define MKV_AUDIO		0xe1U
#define MKV_SAMPLINGFREQUENCY	0xb5U
#define MKV_CHANNELS		0x9fU
#define MKV_CUES		0x1c53bb6bU
#define MKV_CUEPOINT		0xbbU
#define MKV_CUETIME		0xb3U
#define MKV_CUETRACKPOSITIONS	0xb7U
#define MKV_CUETRACK		0xf7U
#define MKV_CUECLUSTERPOSITION	0xf1U
#define MKV_CLUSTER		0x1f43b675U
#define MKV_TIMECODE		0xe7U
#define MKV_SIMPLEBLOCK		0xa3U
#define MKV_BLOCKGROUP		0xa0U
#define MKV_BLOCK		0xa1U
#define MKV_REFERENCEBLOCK	0xfbU

/* A size whose bits are all ones: unknown (a live stream's cluster). */
#define MKV_UNKNOWN_SIZE	UINT64_MAX

/* The largest top-level element read whole (Info, Tracks, Cues, SeekHead). */
#define MKV_ELEMENT_MAX		(16U * 1024U * 1024U)

/* The most frames a laced block holds (its count is one byte). */
#define MKV_LACE_MAX		256U

/* The most cue points kept. */
#define MKV_CUES_MAX		(1024U * 1024U)

/* An element's header as read: its ID, its data's size and where its data starts. */
struct mkv_element {
	uint32_t id;
	uint64_t size;
	uint64_t data;
};

/* A cue point: a time (in the segment's ticks) and the cluster at it (an offset in the file). */
struct mkv_cue {
	int64_t time;
	uint64_t cluster;
};

/*
 * The reader's state: where the segment's data is, the length of a tick,
 * each track's number, the cues, where the first cluster is, where reading
 * is, the current cluster's time, the frames left of a laced block, and
 * which tracks wait for a keyframe after a seek.
 */
struct mkv_state {
	uint64_t segment_start;
	uint64_t segment_end;
	uint64_t timecode_scale;
	uint64_t numbers[MF_TRACK_MAX];
	struct mkv_cue *cues;
	size_t cue_count;
	uint64_t cues_position;
	uint64_t first_cluster;
	uint64_t position;
	int64_t cluster_time;
	size_t lace_sizes[MKV_LACE_MAX];
	size_t lace_offset;
	unsigned lace_count;
	unsigned lace_next;
	unsigned lace_track;
	int64_t lace_time_us;
	int lace_key;
	unsigned waiting[MF_TRACK_MAX];
};

static int mkv_open(struct mf_file *file);
static int mkv_read(struct mf_file *file, struct mf_packet *packet);
static int mkv_seek(struct mf_file *file, int64_t time_us);
static void mkv_close(struct mf_file *file);
static int element_at(struct mf_file *file, uint64_t position, struct mkv_element *element);
static int vint_in(const unsigned char *data, size_t size, size_t *offset, uint64_t *value, int marker);
static int child_next(const unsigned char *data, size_t size, size_t *offset, uint32_t *id, const unsigned char **value, size_t *length);
static uint64_t uint_of(const unsigned char *data, size_t length);
static double float_of(const unsigned char *data, size_t length);
static int read_whole(struct mf_file *file, const struct mkv_element *element, unsigned char **data);
static int check_header(struct mf_file *file, uint64_t *next);
static int read_top(struct mf_file *file, struct mkv_state *state);
static int read_info(struct mf_file *file, struct mkv_state *state, const unsigned char *data, size_t size);
static int read_tracks(struct mf_file *file, struct mkv_state *state, const unsigned char *data, size_t size);
static int read_track_entry(struct mf_file *file, struct mkv_state *state, const unsigned char *data, size_t size);
static void read_track_detail(struct mf_track *track, uint32_t id, const unsigned char *data, size_t size);
static void codec_of(struct mf_track *track);
static void read_seek_head(struct mkv_state *state, const unsigned char *data, size_t size);
static int read_cues(struct mf_file *file, struct mkv_state *state, const unsigned char *data, size_t size);
static int cue_point(struct mkv_state *state, uint64_t lead, const unsigned char *data, size_t size);
static int take_block(struct mf_file *file, struct mkv_state *state, const struct mkv_element *element);
static int open_block(struct mf_file *file, struct mkv_state *state, size_t start, size_t size, int key);
static int read_laces(struct mkv_state *state, const unsigned char *data, size_t size, size_t *offset, unsigned flags);
static int next_lace(struct mf_file *file, struct mkv_state *state, struct mf_packet *packet);
static int64_t ticks_us(const struct mkv_state *state, int64_t ticks);
static int uint_at(struct mf_file *file, uint64_t position, uint64_t size, int64_t *value);
static int cluster_time_at(struct mf_file *file, uint64_t position, uint64_t end, int64_t *time);

/* A Matroska CodecID (or its prefix) and the codec it names. */
struct mkv_id_codec {
	const char *name;
	unsigned prefix;
	unsigned codec;
};

/* The CodecIDs this reader knows. */
static const struct mkv_id_codec id_codecs[] = {
	{ "V_MPEG4/ISO/AVC", 0U, MF_CODEC_H264 },
	{ "V_MPEGH/ISO/HEVC", 0U, MF_CODEC_HEVC },
	{ "V_AV1", 0U, MF_CODEC_AV1 },
	{ "V_VP9", 0U, MF_CODEC_VP9 },
	{ "V_VP8", 0U, MF_CODEC_VP8 },
	{ "V_MPEG4/ISO/", 1U, MF_CODEC_MPEG4 },
	{ "A_AAC", 1U, MF_CODEC_AAC },
	{ "A_OPUS", 0U, MF_CODEC_OPUS },
	{ "A_MPEG/L3", 0U, MF_CODEC_MP3 },
	{ "A_VORBIS", 0U, MF_CODEC_VORBIS },
	{ "V_THEORA", 0U, MF_CODEC_THEORA },
};

/* The reader as mediafile.c calls it. */
const struct mf_format mf_mkv_format = {
	"matroska",
	mkv_open,
	mkv_read,
	mkv_seek,
	mkv_close,
};

/*
 * Reads the EBML header, finds the segment and reads its elements up to
 * the first cluster (and the cues).
 */
static int
mkv_open(
	struct mf_file *file)
{
	struct mkv_state *state;
	struct mkv_element segment;
	uint64_t position;
	int error;

	/* The state; a tick is a millisecond unless Info says otherwise. */
	state = calloc(1, sizeof(*state));
	if (state == NULL)
		return ENOMEM;

	/* Kept in the file, so that mf_close frees it on any failure. */
	file->state = state;
	state->timecode_scale = 1000000U;

	/* The EBML header, and the document type. */
	error = check_header(file, &position);
	if (error != 0)
		return error;

	/* The segment after it. */
	error = element_at(file, position, &segment);
	if (error != 0 || segment.id != MKV_SEGMENT)
		return EINVAL;

	/* Its data, to the end of the file when its size is unknown or too large. */
	state->segment_start = segment.data;
	state->segment_end = file->size;
	if (segment.size != MKV_UNKNOWN_SIZE && segment.size <= file->size - segment.data)
		state->segment_end = segment.data + segment.size;

	/* The elements before the first cluster. */
	error = read_top(file, state);
	if (error != 0)
		return error;

	/* Refuses a file without a track or a cluster. */
	if (file->track_count == 0 || state->first_cluster == 0)
		return EINVAL;

	/* Succeeded: reading starts at the first cluster. */
	state->position = state->first_cluster;
	return 0;
}

/*
 * Hands out the next frame: of the laced block being read, else of the
 * next block in the clusters.
 */
static int
mkv_read(
	struct mf_file *file,
	struct mf_packet *packet)
{
	struct mkv_state *state;
	struct mkv_element element;
	int error;

	/* Elements one after another until a packet. */
	state = file->state;
	for (;;) {
		/* A frame left of a laced block. */
		if (state->lace_next < state->lace_count) {
			error = next_lace(file, state, packet);
			if (error == EAGAIN)
				continue;

			/* The frame, or why there is none. */
			return error;
		}

		/* The end of the segment. */
		if (state->position >= state->segment_end)
			return ENODATA;

		/* The next element (a damaged tail ends the reading as the end). */
		error = element_at(file, state->position, &element);
		if (error != 0)
			return ENODATA;

		/* A cluster: its children follow. */
		if (element.id == MKV_CLUSTER) {
			state->position = element.data;
			state->cluster_time = 0;
			continue;
		}

		/* Refuses any other element of unknown size, which cannot be stepped over. */
		if (element.size == MKV_UNKNOWN_SIZE || element.size > state->segment_end - element.data)
			return EINVAL;

		/* Reading goes on after this element, whatever it is. */
		state->position = element.data + element.size;

		/* The cluster's time. */
		if (element.id == MKV_TIMECODE) {
			error = uint_at(file, element.data, element.size, &state->cluster_time);
			if (error != 0)
				return error;

			continue;
		}

		/* A block or a block group: its frames. */
		if (element.id == MKV_SIMPLEBLOCK || element.id == MKV_BLOCKGROUP) {
			error = take_block(file, state, &element);
			if (error != 0)
				return error;
		}
	}
}

/*
 * Moves to the cluster at or before a time: the last cue point not after
 * it, else the last cluster whose time is not after it.  The video tracks
 * then skip their packets until a keyframe.
 */
static int
mkv_seek(
	struct mf_file *file,
	int64_t time_us)
{
	struct mkv_state *state;
	struct mkv_element element;
	uint64_t position;
	uint64_t end;
	uint64_t chosen;
	int64_t ticks;
	int64_t cluster;
	size_t i;
	unsigned t;
	int error;

	/* The time in ticks. */
	state = file->state;
	ticks = time_us * 1000 / (int64_t)state->timecode_scale;
	chosen = state->first_cluster;
	cluster = 0;

	/* The last cue point not after the time. */
	if (state->cue_count != 0) {
		for (i = 0; i < state->cue_count; i++) {
			/* Kept while not after the time. */
			if (state->cues[i].time <= ticks)
				chosen = state->cues[i].cluster;
		}
	} else {
		/* No cues: the clusters' times, one cluster after another. */
		position = state->first_cluster;
		while (position < state->segment_end) {
			/* The next element; a cluster of unknown size ends the walk. */
			error = element_at(file, position, &element);
			if (error != 0 || element.size == MKV_UNKNOWN_SIZE)
				break;

			/* A cluster: its time, and kept while not after the time wanted. */
			if (element.id == MKV_CLUSTER) {
				end = element.data + element.size;
				error = cluster_time_at(file, element.data, end, &cluster);
				if (error == 0 && cluster > ticks)
					break;

				/* Kept while not after the time wanted. */
				if (error == 0)
					chosen = position;
			}

			/* The next top-level element. */
			position = element.data + element.size;
		}
	}

	/* Reading starts at the chosen cluster, with no laced block left. */
	state->position = chosen;
	state->lace_count = 0;
	state->lace_next = 0;

	/* The video tracks wait for a keyframe. */
	for (t = 0; t < file->track_count; t++) {
		state->waiting[t] = 0;
		if (file->tracks[t].kind == MF_TRACK_VIDEO)
			state->waiting[t] = 1U;
	}

	/* Succeeded: the next packet is from the cluster. */
	return 0;
}

/*
 * Lets the reader's state go.
 */
static void
mkv_close(
	struct mf_file *file)
{
	struct mkv_state *state;

	/* The cues and the state. */
	state = file->state;
	free(state->cues);
	free(state);
	file->state = NULL;
}

/*
 * Reads an element's header at a position of the file: its ID (one to
 * four bytes, marker kept) and its size (one to eight bytes, all ones for
 * unknown).  Returns 0, or EINVAL for a header that is damaged or past the
 * end.
 */
static int
element_at(
	struct mf_file *file,
	uint64_t position,
	struct mkv_element *element)
{
	unsigned char header[12];
	size_t length;
	size_t offset;
	uint64_t id;
	uint64_t size;
	int error;

	/* Up to twelve bytes, fewer near the end of the file. */
	if (position >= file->size)
		return EINVAL;

	/* Up to twelve bytes, fewer near the end of the file. */
	length = sizeof(header);
	if (file->size - position < length)
		length = (size_t)(file->size - position);

	/* The bytes. */
	error = mf_read_at(file, position, header, length);
	if (error != 0)
		return error;

	/* The ID, with its marker, at most four bytes. */
	offset = 0;
	error = vint_in(header, length, &offset, &id, 1);
	if (error != 0 || offset > 4U)
		return EINVAL;

	/* The size. */
	error = vint_in(header, length, &offset, &size, 0);
	if (error != 0)
		return EINVAL;

	/* Succeeded: the header. */
	element->id = (uint32_t)id;
	element->size = size;
	element->data = position + offset;
	return 0;
}

/*
 * Reads an EBML variable-length integer at *offset of a buffer: its length
 * is told by the first set bit of the first byte.  With marker, the length
 * marker is kept (as IDs are written); otherwise it is removed, and a value
 * of all ones is MKV_UNKNOWN_SIZE.  Returns 0, or EINVAL.
 */
static int
vint_in(
	const unsigned char *data,
	size_t size,
	size_t *offset,
	uint64_t *value,
	int marker)
{
	unsigned first;
	unsigned length;
	unsigned i;
	uint64_t result;
	uint64_t all_ones;

	/* The first byte, which tells the length. */
	if (*offset >= size)
		return EINVAL;

	/* The first byte, which tells the length. */
	first = data[*offset];
	if (first == 0)
		return EINVAL;

	/* The length: the position of the first set bit. */
	length = 1;
	while ((first & (0x80U >> (length - 1U))) == 0U)
		length++;

	/* Refuses a number past the end of the buffer. */
	if (length > size - *offset)
		return EINVAL;

	/* The value, with or without the marker. */
	result = first;
	if (!marker)
		result = first & (0xffU >> length);

	/* The rest of the bytes, high first. */
	for (i = 1; i < length; i++)
		result = (result << 8) | data[*offset + i];

	/* Past the number. */
	*offset += length;

	/* A size of all ones is unknown. */
	all_ones = (((uint64_t)1U) << (7U * length)) - 1U;
	if (!marker && result == all_ones)
		result = MKV_UNKNOWN_SIZE;

	/* Succeeded: the value. */
	*value = result;
	return 0;
}

/*
 * Reads the next child element of a buffer (a master element read whole):
 * its ID and its data.  Returns 0, ENODATA at the end, or EINVAL for a
 * child that does not fit.
 */
static int
child_next(
	const unsigned char *data,
	size_t size,
	size_t *offset,
	uint32_t *id,
	const unsigned char **value,
	size_t *length)
{
	uint64_t raw_id;
	uint64_t raw_size;
	int error;

	/* The end of the buffer. */
	if (*offset >= size)
		return ENODATA;

	/* The ID. */
	error = vint_in(data, size, offset, &raw_id, 1);
	if (error != 0)
		return EINVAL;

	/* The size, which must fit in what is left. */
	error = vint_in(data, size, offset, &raw_size, 0);
	if (error != 0 || raw_size > size - *offset)
		return EINVAL;

	/* Succeeded: the child, and *offset is past it. */
	*id = (uint32_t)raw_id;
	*value = data + *offset;
	*length = (size_t)raw_size;
	*offset += (size_t)raw_size;
	return 0;
}

/*
 * Reads an unsigned integer element's value (big-endian, up to eight bytes).
 */
static uint64_t
uint_of(
	const unsigned char *data,
	size_t length)
{
	uint64_t value;
	size_t i;

	/* Each byte, high first; longer values keep their low eight bytes. */
	value = 0;
	for (i = 0; i < length; i++)
		value = (value << 8) | data[i];

	/* The value. */
	return value;
}

/*
 * Reads a float element's value (four or eight bytes; 0 for another length).
 */
static double
float_of(
	const unsigned char *data,
	size_t length)
{
	uint32_t bits32;
	uint64_t bits64;
	float single;
	double value;

	/* A single-precision value. */
	if (length == 4U) {
		bits32 = mf_be32(data);
		memcpy(&single, &bits32, sizeof(single));
		return (double)single;
	}

	/* A double-precision value. */
	if (length == 8U) {
		bits64 = mf_be64(data);
		memcpy(&value, &bits64, sizeof(value));
		return value;
	}

	/* No other length is a float. */
	return 0.0;
}

/*
 * Reads a top-level element's data whole into memory.  Returns 0 with
 * *data to free, or EINVAL for an element too large or of unknown size.
 */
static int
read_whole(
	struct mf_file *file,
	const struct mkv_element *element,
	unsigned char **data)
{
	unsigned char *buffer;
	int error;

	/* Refuses an element no real file has so large. */
	*data = NULL;
	if (element->size == MKV_UNKNOWN_SIZE || element->size > MKV_ELEMENT_MAX)
		return EINVAL;

	/* The buffer (one byte at least). */
	buffer = malloc((size_t)element->size + 1U);
	if (buffer == NULL)
		return ENOMEM;

	/* Its bytes. */
	error = mf_read_at(file, element->data, buffer, (size_t)element->size);
	if (error != 0) {
		free(buffer);
		return error;
	}

	/* Succeeded: the data. */
	*data = buffer;
	return 0;
}

/*
 * Checks the EBML header at the start of the file: its document type must
 * be matroska or webm.  Returns 0 with where the next element starts.
 */
static int
check_header(
	struct mf_file *file,
	uint64_t *next)
{
	struct mkv_element header;
	const unsigned char *value;
	unsigned char *data;
	size_t offset;
	size_t length;
	uint32_t id;
	int known;
	int error;
	int step;
	int compared;

	/* The EBML header element, read whole. */
	error = element_at(file, 0, &header);
	if (error != 0 || header.id != MKV_EBML || header.size > 4096U)
		return EINVAL;

	/* Its data. */
	error = read_whole(file, &header, &data);
	if (error != 0)
		return error;

	/* Its document type. */
	known = 0;
	offset = 0;
	for (;;) {
		/* The next child; the end or a damaged one ends the walk. */
		step = child_next(data, (size_t)header.size, &offset, &id, &value, &length);
		if (step != 0)
			break;

		/* Only the document type matters here. */
		if (id != MKV_DOCTYPE)
			continue;

		/* Only matroska and webm are read. */
		if (length == 8U) {
			compared = memcmp(value, "matroska", 8);
			if (compared == 0)
				known = 1;
		} else if (length == 4U) {
			compared = memcmp(value, "webm", 4);
			if (compared == 0)
				known = 1;
		}
	}

	/* The data is no longer needed. */
	free(data);

	/* Refuses another document type. */
	if (!known)
		return EINVAL;

	/* Succeeded: the segment follows. */
	*next = header.data + header.size;
	return 0;
}

/*
 * Reads the segment's top-level elements until the first cluster: Info,
 * Tracks, SeekHead and Cues.  The cues are read where the seek head says
 * when they come after the clusters.
 */
static int
read_top(
	struct mf_file *file,
	struct mkv_state *state)
{
	struct mkv_element element;
	unsigned char *data;
	uint64_t position;
	int error;

	/* Each element up to the first cluster. */
	position = state->segment_start;
	while (position < state->segment_end) {
		/* The next element. */
		error = element_at(file, position, &element);
		if (error != 0)
			return error;

		/* The first cluster ends the header. */
		if (element.id == MKV_CLUSTER) {
			state->first_cluster = position;
			break;
		}

		/* Refuses an element of unknown size before the clusters. */
		if (element.size == MKV_UNKNOWN_SIZE || element.size > state->segment_end - element.data)
			return EINVAL;

		/* The elements read whole. */
		position = element.data + element.size;
		if (element.id != MKV_INFO && element.id != MKV_TRACKS &&
		    element.id != MKV_SEEKHEAD && element.id != MKV_CUES)
			continue;

		/* Its data. */
		error = read_whole(file, &element, &data);
		if (error != 0)
			return error;

		/* Each kind to its reader. */
		if (element.id == MKV_INFO)
			error = read_info(file, state, data, (size_t)element.size);
		else if (element.id == MKV_TRACKS)
			error = read_tracks(file, state, data, (size_t)element.size);
		else if (element.id == MKV_SEEKHEAD)
			read_seek_head(state, data, (size_t)element.size);
		else
			error = read_cues(file, state, data, (size_t)element.size);

		/* The data is no longer needed; a reader's failure ends the open. */
		free(data);
		if (error != 0)
			return error;
	}

	/* The cues after the clusters, where the seek head said. */
	if (state->cue_count == 0 && state->cues_position != 0) {
		error = element_at(file, state->cues_position, &element);
		if (error != 0 || element.id != MKV_CUES)
			return 0;

		/* Read whole; damaged cues leave the file without them. */
		error = read_whole(file, &element, &data);
		if (error != 0)
			return 0;

		/* Damaged cues leave the file without them. */
		(void)read_cues(file, state, data, (size_t)element.size);
		free(data);
	}

	/* Succeeded: the header is read. */
	return 0;
}

/*
 * Reads the segment's Info: the length of a tick and the presentation's
 * length.
 */
static int
read_info(
	struct mf_file *file,
	struct mkv_state *state,
	const unsigned char *data,
	size_t size)
{
	const unsigned char *value;
	double duration;
	size_t offset;
	size_t length;
	uint32_t id;
	int error;
	uint64_t scale;

	/* Each child. */
	duration = 0.0;
	offset = 0;
	for (;;) {
		/* The next child. */
		error = child_next(data, size, &offset, &id, &value, &length);
		if (error == ENODATA)
			break;

		/* Refuses a damaged Info. */
		if (error != 0)
			return error;

		/* The tick's length in nanoseconds (0 would be meaningless). */
		if (id == MKV_TIMECODESCALE) {
			scale = uint_of(value, length);
			if (scale != 0U)
				state->timecode_scale = scale;
		}

		/* The length in ticks. */
		if (id == MKV_DURATION)
			duration = float_of(value, length);
	}

	/* The length in microseconds, when it is a sane number. */
	if (duration > 0.0 && duration < 1.0e15)
		file->duration_us = (int64_t)(duration * (double)state->timecode_scale / 1000.0);

	/* Succeeded: the Info is read. */
	return 0;
}

/*
 * Reads the Tracks element: each TrackEntry.
 */
static int
read_tracks(
	struct mf_file *file,
	struct mkv_state *state,
	const unsigned char *data,
	size_t size)
{
	const unsigned char *value;
	size_t offset;
	size_t length;
	uint32_t id;
	int error;

	/* Each entry, while there is room for more tracks. */
	offset = 0;
	for (;;) {
		/* The next child. */
		error = child_next(data, size, &offset, &id, &value, &length);
		if (error == ENODATA)
			break;

		/* Refuses a damaged Tracks element. */
		if (error != 0)
			return error;

		/* A track entry. */
		if (id != MKV_TRACKENTRY || file->track_count == MF_TRACK_MAX)
			continue;

		/* The entry. */
		error = read_track_entry(file, state, value, length);
		if (error != 0)
			return error;
	}

	/* Succeeded: the tracks are read. */
	return 0;
}

/*
 * Reads one TrackEntry: its number, kind, codec and private data, and the
 * picture's size or the sound's format.
 */
static int
read_track_entry(
	struct mf_file *file,
	struct mkv_state *state,
	const unsigned char *data,
	size_t size)
{
	struct mf_track *track;
	const unsigned char *value;
	const unsigned char *inner;
	size_t offset;
	size_t inner_offset;
	size_t length;
	size_t inner_length;
	uint64_t number;
	uint64_t type;
	uint32_t id;
	uint32_t inner_id;
	int error;
	int step;

	/* The slot of the next track. */
	track = &file->tracks[file->track_count];
	free((void *)track->private_data);
	memset(track, 0, sizeof(*track));
	number = 0;
	offset = 0;
	for (;;) {
		/* The next child. */
		error = child_next(data, size, &offset, &id, &value, &length);
		if (error == ENODATA)
			break;

		/* Refuses a damaged entry. */
		if (error != 0)
			return error;

		/* The number blocks name the track by, and its kind. */
		if (id == MKV_TRACKNUMBER)
			number = uint_of(value, length);

		/* Its kind: 1 video, 2 sound. */
		if (id == MKV_TRACKTYPE) {
			type = uint_of(value, length);
			if (type == 1U)
				track->kind = MF_TRACK_VIDEO;
			else if (type == 2U)
				track->kind = MF_TRACK_AUDIO;
		}

		/* Its codec's name and private data. */
		if (id == MKV_CODECID)
			mf_set_codec_name(track, (const char *)value, length);

		/* The private data, kept. */
		if (id == MKV_CODECPRIVATE) {
			error = mf_keep_private(track, value, length);
			if (error != 0)
				return error;
		}

		/* The video and audio settings, one level down. */
		if (id != MKV_VIDEO && id != MKV_AUDIO)
			continue;

		/* Its settings. */
		inner_offset = 0;
		for (;;) {
			/* The next setting; the end or a damaged one ends the walk. */
			step = child_next(value, length, &inner_offset, &inner_id, &inner, &inner_length);
			if (step != 0)
				break;

			/* One setting. */
			read_track_detail(track, inner_id, inner, inner_length);
		}
	}

	/* A track without a number cannot be told apart in the blocks: left out. */
	if (number == 0)
		return 0;

	/* The codec from its name, and the track is counted. */
	codec_of(track);
	state->numbers[file->track_count] = number;
	file->track_count++;

	/* Succeeded: the track is read. */
	return 0;
}

/*
 * Reads one setting of a track's Video or Audio element.
 */
static void
read_track_detail(
	struct mf_track *track,
	uint32_t id,
	const unsigned char *data,
	size_t size)
{
	double rate;

	/* The picture's width. */
	if (id == MKV_PIXELWIDTH)
		track->width = (uint32_t)uint_of(data, size);

	/* Its height. */
	if (id == MKV_PIXELHEIGHT)
		track->height = (uint32_t)uint_of(data, size);

	/* The channels. */
	if (id == MKV_CHANNELS)
		track->channels = (uint32_t)uint_of(data, size);

	/* The sampling rate, a float. */
	if (id == MKV_SAMPLINGFREQUENCY) {
		rate = float_of(data, size);
		if (rate > 0.0 && rate < 1000000.0)
			track->sample_rate = (uint32_t)rate;
	}
}

/*
 * Tells the codec from a track's CodecID.
 */
static void
codec_of(
	struct mf_track *track)
{
	size_t length;
	size_t i;
	int compared;

	/* The codecs by their Matroska names (AAC and MPEG-4 Part 2 by their prefixes). */
	for (i = 0; i < sizeof(id_codecs) / sizeof(id_codecs[0]); i++) {
		/* The whole name, or its prefix. */
		length = strlen(id_codecs[i].name);
		if (id_codecs[i].prefix)
			compared = strncmp(track->codec_name, id_codecs[i].name, length);
		else
			compared = strcmp(track->codec_name, id_codecs[i].name);

		/* The name's codec. */
		if (compared == 0) {
			track->codec = id_codecs[i].codec;
			return;
		}
	}
}

/*
 * Reads the SeekHead: where the cues are, when they are listed.
 */
static void
read_seek_head(
	struct mkv_state *state,
	const unsigned char *data,
	size_t size)
{
	const unsigned char *value;
	const unsigned char *inner;
	size_t offset;
	size_t inner_offset;
	size_t length;
	size_t inner_length;
	uint32_t id;
	uint32_t inner_id;
	uint64_t target;
	uint64_t position;
	int step;

	/* Each Seek: an element's ID and its position in the segment. */
	offset = 0;
	for (;;) {
		/* The next child; the end or a damaged one ends the walk. */
		step = child_next(data, size, &offset, &id, &value, &length);
		if (step != 0)
			break;

		/* Only Seek entries. */
		if (id != MKV_SEEK)
			continue;

		/* Its ID (as written) and position. */
		target = 0;
		position = 0;
		inner_offset = 0;
		for (;;) {
			/* The next child; the end or a damaged one ends the walk. */
			step = child_next(value, length, &inner_offset, &inner_id, &inner, &inner_length);
			if (step != 0)
				break;

			/* The element sought. */
			if (inner_id == MKV_SEEKID)
				target = uint_of(inner, inner_length);

			/* Where it is, from the start of the segment's data. */
			if (inner_id == MKV_SEEKPOSITION)
				position = uint_of(inner, inner_length);
		}

		/* The cues' place in the file. */
		if (target == MKV_CUES && position < state->segment_end - state->segment_start)
			state->cues_position = state->segment_start + position;
	}
}

/*
 * Reads the Cues: the cue points of the first video track (of every track
 * when there is no video), each a time and a cluster.
 */
static int
read_cues(
	struct mf_file *file,
	struct mkv_state *state,
	const unsigned char *data,
	size_t size)
{
	const unsigned char *value;
	size_t offset;
	size_t length;
	uint64_t lead;
	uint32_t id;
	unsigned t;
	int error;

	/* The track whose cues are kept: the first video track (0 for any). */
	lead = 0;
	for (t = 0; t < file->track_count; t++) {
		/* The first video track. */
		if (file->tracks[t].kind == MF_TRACK_VIDEO) {
			lead = state->numbers[t];
			break;
		}
	}

	/* Each cue point. */
	offset = 0;
	for (;;) {
		/* The next child. */
		error = child_next(data, size, &offset, &id, &value, &length);
		if (error == ENODATA)
			break;

		/* Refuses damaged cues. */
		if (error != 0)
			return error;

		/* A cue point. */
		if (id != MKV_CUEPOINT)
			continue;

		/* The point. */
		error = cue_point(state, lead, value, length);
		if (error != 0)
			return error;
	}

	/* Succeeded: the cues are read. */
	return 0;
}

/*
 * Reads one CuePoint and keeps its time and cluster when it is the lead
 * track's (or any track's when lead is 0).
 */
static int
cue_point(
	struct mkv_state *state,
	uint64_t lead,
	const unsigned char *data,
	size_t size)
{
	struct mkv_cue *grown;
	const unsigned char *value;
	const unsigned char *inner;
	size_t offset;
	size_t inner_offset;
	size_t length;
	size_t inner_length;
	uint32_t id;
	uint32_t inner_id;
	uint64_t time;
	uint64_t track;
	uint64_t cluster;
	int found;
	int step;

	/* Its time and the first position of the lead track. */
	time = 0;
	cluster = 0;
	found = 0;
	offset = 0;
	for (;;) {
		/* The next child; the end or a damaged one ends the walk. */
		step = child_next(data, size, &offset, &id, &value, &length);
		if (step != 0)
			break;

		/* The time. */
		if (id == MKV_CUETIME)
			time = uint_of(value, length);

		/* A track's position; only the first matching one. */
		if (id != MKV_CUETRACKPOSITIONS || found)
			continue;

		/* The track and its cluster. */
		track = 0;
		inner_offset = 0;
		for (;;) {
			/* The next child; the end or a damaged one ends the walk. */
			step = child_next(value, length, &inner_offset, &inner_id, &inner, &inner_length);
			if (step != 0)
				break;

			/* The track. */
			if (inner_id == MKV_CUETRACK)
				track = uint_of(inner, inner_length);

			/* Its cluster, from the start of the segment's data. */
			if (inner_id == MKV_CUECLUSTERPOSITION)
				cluster = uint_of(inner, inner_length);
		}

		/* Kept when it is the lead track's. */
		if (lead == 0 || track == lead)
			found = 1;
	}

	/* Not the lead track's, a cluster outside the segment, or too many cues: skipped. */
	if (!found || cluster >= state->segment_end - state->segment_start ||
	    state->cue_count >= MKV_CUES_MAX || time > (uint64_t)INT64_MAX)
		return 0;

	/* One more cue. */
	grown = realloc(state->cues, (state->cue_count + 1U) * sizeof(*grown));
	if (grown == NULL)
		return ENOMEM;

	/* Its time and cluster. */
	state->cues = grown;
	state->cues[state->cue_count].time = (int64_t)time;
	state->cues[state->cue_count].cluster = state->segment_start + cluster;
	state->cue_count++;

	/* Succeeded: the cue is kept. */
	return 0;
}

/*
 * Reads a SimpleBlock or a BlockGroup into the packet buffer and opens
 * its block: its frames are then handed out by next_lace.
 */
static int
take_block(
	struct mf_file *file,
	struct mkv_state *state,
	const struct mkv_element *element)
{
	const unsigned char *value;
	size_t block_start;
	size_t block_size;
	size_t offset;
	size_t length;
	uint32_t id;
	int key;
	int error;
	int step;

	/* The element's bytes in the packet buffer. */
	error = mf_packet_room(file, (size_t)element->size);
	if (error != 0)
		return error;

	/* Its bytes. */
	error = mf_read_at(file, element->data, file->buffer, (size_t)element->size);
	if (error != 0)
		return error;

	/* A SimpleBlock is the block; its keyframe flag is in its header. */
	if (element->id == MKV_SIMPLEBLOCK) {
		error = open_block(file, state, 0, (size_t)element->size, -1);
		return error;
	}

	/* A BlockGroup: its Block, a keyframe unless it references another. */
	block_start = 0;
	block_size = 0;
	key = 1;
	offset = 0;
	for (;;) {
		/* The next child; the end or a damaged one ends the walk. */
		step = child_next(file->buffer, (size_t)element->size, &offset, &id, &value, &length);
		if (step != 0)
			break;

		/* The block itself. */
		if (id == MKV_BLOCK) {
			block_start = (size_t)(value - file->buffer);
			block_size = length;
		}

		/* A reference: not a keyframe. */
		if (id == MKV_REFERENCEBLOCK)
			key = 0;
	}

	/* A group without a block gives nothing. */
	if (block_size == 0)
		return 0;

	/* The block. */
	error = open_block(file, state, block_start, block_size, key);
	return error;
}

/*
 * Opens a block in the packet buffer (at start, size bytes): its track, its
 * time, its keyframe flag (from the header when key is -1) and its frames.
 * A block of an unknown track is skipped.
 */
static int
open_block(
	struct mf_file *file,
	struct mkv_state *state,
	size_t start,
	size_t size,
	int key)
{
	const unsigned char *data;
	uint64_t number;
	size_t offset;
	unsigned flags;
	unsigned t;
	int16_t relative;
	int error;

	/* The track's number. */
	data = file->buffer + start;
	offset = 0;
	error = vint_in(data, size, &offset, &number, 0);
	if (error != 0 || size - offset < 3U)
		return 0;

	/* The track it names; another track's block is skipped. */
	for (t = 0; t < file->track_count; t++) {
		/* The track with that number. */
		if (state->numbers[t] == number)
			break;
	}

	/* A block of another track is skipped. */
	if (t == file->track_count)
		return 0;

	/* The time relative to the cluster, and the flags. */
	relative = (int16_t)mf_be16(data + offset);
	flags = data[offset + 2];
	offset += 3U;
	if (key < 0) {
		/* A SimpleBlock's keyframe flag. */
		key = 0;
		if ((flags & 0x80U) != 0U)
			key = 1;
	}

	/* The frames, one, or several when laced. */
	error = read_laces(state, data, size, &offset, flags);
	if (error != 0)
		return 0;

	/* Succeeded: next_lace hands out the frames. */
	state->lace_offset = start + offset;
	state->lace_next = 0;
	state->lace_track = t;
	state->lace_time_us = ticks_us(state, state->cluster_time + relative);
	state->lace_key = key;
	return 0;
}

/*
 * Reads a block's lacing: the frames' count and sizes (Xiph, fixed or EBML
 * lacing), or one frame of the rest of the block.  Returns 0, or EINVAL for
 * sizes that do not fit.
 */
static int
read_laces(
	struct mkv_state *state,
	const unsigned char *data,
	size_t size,
	size_t *offset,
	unsigned flags)
{
	uint64_t value;
	int64_t difference;
	size_t total;
	size_t before;
	unsigned lacing;
	unsigned count;
	unsigned i;
	unsigned byte;
	int error;

	/* No lacing: one frame. */
	lacing = (flags >> 1) & 3U;
	if (lacing == 0U) {
		state->lace_count = 1;
		state->lace_sizes[0] = size - *offset;
		return 0;
	}

	/* The count of frames. */
	if (*offset >= size)
		return EINVAL;

	/* The count of frames. */
	count = data[*offset] + 1U;
	(*offset)++;
	total = 0;

	/* Xiph lacing: each size but the last as bytes summed until one below 255. */
	if (lacing == 1U) {
		for (i = 0; i + 1U < count; i++) {
			state->lace_sizes[i] = 0;
			do {
				/* One more byte of the size. */
				if (*offset >= size)
					return EINVAL;

				/* The byte is added. */
				byte = data[*offset];
				(*offset)++;
				state->lace_sizes[i] += byte;
			} while (byte == 255U);

			/* The size so far is counted. */
			total += state->lace_sizes[i];
		}
	}

	/* EBML lacing: the first size, then signed differences. */
	if (lacing == 3U) {
		error = vint_in(data, size, offset, &value, 0);
		if (error != 0 || value > size)
			return EINVAL;

		/* The first size. */
		state->lace_sizes[0] = (size_t)value;
		total = (size_t)value;
		for (i = 1; i + 1U < count; i++) {
			/* The difference, stored biased by half of its range. */
			before = *offset;
			error = vint_in(data, size, offset, &value, 0);
			if (error != 0)
				return EINVAL;

			/* The difference, biased by half the range of a number of that length. */
			difference = (int64_t)value - (((int64_t)1 << (7U * (unsigned)(*offset - before) - 1U)) - 1);
			if ((int64_t)state->lace_sizes[i - 1U] + difference < 0)
				return EINVAL;

			/* The size: the previous one and the difference. */
			state->lace_sizes[i] = (size_t)((int64_t)state->lace_sizes[i - 1U] + difference);
			total += state->lace_sizes[i];
		}
	}

	/* Fixed lacing: equal sizes. */
	if (lacing == 2U) {
		if ((size - *offset) % count != 0U)
			return EINVAL;

		/* Each frame but the last gets the same size. */
		for (i = 0; i + 1U < count; i++) {
			state->lace_sizes[i] = (size - *offset) / count;
			total += state->lace_sizes[i];
		}
	}

	/* The last frame takes the rest; refuses sizes larger than the block. */
	if (total > size - *offset)
		return EINVAL;

	/* Succeeded: the sizes are known. */
	state->lace_sizes[count - 1U] = size - *offset - total;
	state->lace_count = count;
	return 0;
}

/*
 * Hands out the next frame of the open block.  Returns EAGAIN for a frame
 * skipped (a video track waiting for a keyframe after a seek).
 */
static int
next_lace(
	struct mf_file *file,
	struct mkv_state *state,
	struct mf_packet *packet)
{
	size_t size;

	/* The frame's size, and the next one starts after it. */
	size = state->lace_sizes[state->lace_next];
	state->lace_next++;
	packet->data = file->buffer + state->lace_offset;
	state->lace_offset += size;

	/* A video track waiting for a keyframe skips the others. */
	if (state->waiting[state->lace_track] && !state->lace_key)
		return EAGAIN;

	/* A keyframe ends the waiting. */
	state->waiting[state->lace_track] = 0;

	/* The packet. */
	packet->track = state->lace_track;
	packet->pts_us = state->lace_time_us;
	packet->dts_us = state->lace_time_us;
	packet->keyframe = state->lace_key;
	packet->size = size;

	/* Succeeded: one packet. */
	return 0;
}

/*
 * Turns a count of ticks into microseconds.
 */
static int64_t
ticks_us(
	const struct mkv_state *state,
	int64_t ticks)
{
	/* Ticks of timecode_scale nanoseconds each (a whole number of microseconds as files write it). */
	if (state->timecode_scale % 1000U == 0U)
		return ticks * (int64_t)(state->timecode_scale / 1000U);

	/* Another tick, through nanoseconds. */
	return ticks * (int64_t)state->timecode_scale / 1000;
}

/*
 * Reads an unsigned integer element's value (up to eight bytes) from the
 * file.
 */
static int
uint_at(
	struct mf_file *file,
	uint64_t position,
	uint64_t size,
	int64_t *value)
{
	unsigned char bytes[8];
	int error;

	/* Refuses a value longer than eight bytes. */
	if (size > sizeof(bytes))
		return EINVAL;

	/* Its bytes. */
	error = mf_read_at(file, position, bytes, (size_t)size);
	if (error != 0)
		return error;

	/* Succeeded: the value. */
	*value = (int64_t)uint_of(bytes, (size_t)size);
	return 0;
}

/*
 * Reads a cluster's time: the first Timecode among its children, from a
 * position up to end.
 */
static int
cluster_time_at(
	struct mf_file *file,
	uint64_t position,
	uint64_t end,
	int64_t *time)
{
	struct mkv_element element;
	int error;

	/* Each child until the time. */
	while (position < end) {
		/* The next child. */
		error = element_at(file, position, &element);
		if (error != 0 || element.size == MKV_UNKNOWN_SIZE || element.size > end - element.data)
			return EINVAL;

		/* The time. */
		if (element.id == MKV_TIMECODE) {
			error = uint_at(file, element.data, element.size, time);
			return error;
		}

		/* The next child. */
		position = element.data + element.size;
	}

	/* A cluster without a time. */
	return EINVAL;
}
