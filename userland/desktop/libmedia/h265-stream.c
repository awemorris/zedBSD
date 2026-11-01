/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The H.265 elementary stream reader of the Vulkan Video path.
 */

#include "h265-stream.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "vcodec-nal.h"

/* The value of a position that is not set: no NAL unit starts there. */
#define STREAM_NO_POSITION ((size_t)-1)

/* The bytes of a NAL unit header (7.3.1.2). */
#define STREAM_NAL_HEADER_BYTES 2U

/* The first nal_unit_type above the VCL types (Table 7-1: 0..31 are VCL). */
#define STREAM_FIRST_NON_VCL 32U

/*
 * The result of reading one slice segment (and of deciding what to do with
 * the picture it belongs to).
 *
 * OK: the segment reads and joins its picture.  DROPPED: the picture is
 * not returned by the rule of its type and is already counted.  BROKEN:
 * the picture is damaged.  REFUSED: the picture names a syntax or a
 * parameter set outside the range of this parse.  NOMEM: slice_rbsp could
 * not be allocated.
 */
enum {
	STREAM_SEGMENT_OK,
	STREAM_SEGMENT_DROPPED,
	STREAM_SEGMENT_BROKEN,
	STREAM_SEGMENT_REFUSED,
	STREAM_SEGMENT_NOMEM
};

/*
 * What a slice segment must agree with about the SPS: whether to compare
 * at all, the SPS id and the SPS generation (so an SPS that changed
 * between two segments of one picture, or inside a coded video sequence,
 * breaks the picture).
 */
struct stream_expect {
	int check;
	unsigned sps_id;
	unsigned generation;
};

/*
 * The picture being gathered by one call of h265_stream_next.
 *
 * open is 1 once a slice segment of the picture was seen (a segment with
 * first_slice_segment_in_pic_flag 0 before any first one opens a picture
 * that is dead from the start).  dead is 1 for a picture that will not be
 * returned: its remaining segments are only walked past to find where its
 * access unit ends.  nomem is 1 when slice_rbsp could not be allocated for
 * it (the call reports ENOMEM when the picture ends).  au_first and au_end
 * are the offsets of the access unit's first byte (its first start code)
 * and of the end of the last NAL unit that belongs to it.  join_end is,
 * after a look ahead found that the NAL units after the last segment
 * belong to the same picture, the buffer position of the segment that
 * proved it (the scan from the first of those NAL units does not look
 * ahead again before it).  sps_generation is the SPS generation the first
 * segment was read with.
 */
struct stream_gather {
	int open;
	int dead;
	int nomem;
	size_t au_first;
	size_t au_end;
	size_t join_end;
	unsigned sps_generation;
};

/* Static function declarations. */
static void stream_gather_init(struct stream_gather *gather);
static int stream_nal_header(const uint8_t *nal, size_t nal_size, unsigned *type, unsigned *layer);
static size_t stream_start_code(const uint8_t *data, size_t nal_offset);
static int stream_damaged_set(const uint8_t *nal, size_t nal_size);
static int stream_store_set(struct h265_stream *stream, unsigned type, const uint8_t *nal, size_t nal_size);
static int stream_lookahead(const struct h265_stream *stream, size_t from, size_t *position);
static int stream_find_sets(struct h265_stream *stream, unsigned pps_id, const struct h265sw_sps **sps, const struct h265sw_pps **pps);
static int stream_check_expect(const struct h265_stream *stream, const struct stream_expect *expect, const struct h265sw_sps *sps);
static int stream_read_long(struct h265_stream *stream, const uint8_t *nal, size_t nal_size, struct h265sw_slice_header *header, const struct h265sw_slice_header *independent, const struct h265sw_sps *sps, const struct h265sw_pps *pps, struct h265sw_nal *info);
static int stream_read_segment(struct h265_stream *stream, const uint8_t *nal, size_t nal_size, struct h265sw_slice_header *header, const struct h265sw_slice_header *independent, const struct stream_expect *expect, struct h265sw_nal *info, const struct h265sw_sps **sps, const struct h265sw_pps **pps);
static int stream_first_segment(struct h265_stream *stream, struct h265_picture *picture, struct stream_gather *gather, const uint8_t *nal, size_t nal_size);
static int stream_later_segment(struct h265_stream *stream, struct h265_picture *picture, struct stream_gather *gather, const uint8_t *nal, size_t nal_size);
static void stream_spoil(struct h265_stream *stream, struct stream_gather *gather, int outcome);
static int stream_vcl(struct h265_stream *stream, struct h265_picture *picture, struct stream_gather *gather, size_t before, const uint8_t *nal, size_t nal_size);
static int stream_step(struct h265_stream *stream, struct h265_picture *picture, struct stream_gather *gather, size_t before, const uint8_t *nal, size_t nal_size);
static int stream_finish(struct h265_stream *stream, struct h265_picture *picture, struct stream_gather *gather);

/*
 * Makes an empty reader: no buffer, no parameter sets, waiting for an IRAP
 * picture.
 *
 * The structure is cleared as a whole (the pointer to the longer slice
 * copy included, so a reader that still owns one must be destroyed first).
 */
void
h265_stream_init(
	struct h265_stream *stream)
{
	/* A missing reader has nothing to make. */
	if (stream == NULL)
		return;

	/* Clears every field: no buffer, no state, no counts. */
	memset(stream, 0, sizeof(*stream));

	/* Empties the parameter set store and the order count state. */
	h265sw_ps_init(&stream->store);
	h265sw_poc_init(&stream->poc);

	/*
	 * The first picture returned is picture 0 of the stream (C.5.2.2) and
	 * must be an IRAP picture, which starts the decoding.
	 */
	stream->first_pending = 1;
}

/*
 * Frees what the reader allocated (slice_rbsp); the reader may be
 * initialized again.
 */
void
h265_stream_destroy(
	struct h265_stream *stream)
{
	/* A missing reader owns nothing. */
	if (stream == NULL)
		return;

	/* Returns the longer slice segment copy, if one was made. */
	free(stream->slice_rbsp);
	stream->slice_rbsp = NULL;
	stream->slice_rbsp_capacity = 0U;

	/* The borrowed buffer was never the reader's to keep. */
	stream->data = NULL;
	stream->size = 0U;
	stream->cursor = 0U;
}

/*
 * Gives the reader a new buffer of whole access units (data is NULL only
 * when size is 0) and moves the cursor to its start.
 */
void
h265_stream_input(
	struct h265_stream *stream,
	const uint8_t *data,
	size_t size)
{
	/* A missing reader takes nothing. */
	if (stream == NULL)
		return;

	/* A missing buffer is an empty one. */
	if (data == NULL)
		size = 0U;

	/* Borrows the buffer and starts reading at its first byte. */
	stream->data = data;
	stream->size = size;
	stream->cursor = 0U;
}

/*
 * Reads the parameter sets of a buffer that holds no picture into the
 * store.
 *
 * The NAL units are read up to the first VCL NAL unit of the base layer;
 * the buffer is not kept and the reader's own buffer and cursor are not
 * touched.  A parameter set that does not read is counted in
 * refused_sets, the others are stored, and the call reports EINVAL.
 */
int
h265_stream_parameters(
	struct h265_stream *stream,
	const uint8_t *data,
	size_t size)
{
	size_t cursor;
	const uint8_t *nal;
	size_t nal_size;
	unsigned type;
	unsigned layer;
	int found;
	int error;
	int failed;
	int done;

	/* Refuses a missing reader. */
	if (stream == NULL)
		return EINVAL;

	/* A missing buffer holds no parameter sets. */
	if (data == NULL)
		return 0;

	/* Walks the NAL units up to the first VCL NAL unit. */
	cursor = 0U;
	failed = 0;
	done = 0;
	while (done == 0) {
		/* Finds the next NAL unit of the buffer. */
		found = vcodec_nal_find(data, size, &cursor, &nal, &nal_size);
		if (found == 0)
			break;

		/* Skips a NAL unit whose header does not read; a parameter set that does not is counted and fails the call. */
		error = stream_nal_header(nal, nal_size, &type, &layer);
		if (error != 0) {
			if (stream_damaged_set(nal, nal_size) != 0) {
				stream->counts.refused_sets++;
				stream->reason = "parameter set NAL unit header does not read";
				failed = 1;
			}
			continue;
		}

		/* Skips the NAL units of the other layers. */
		if (layer != 0U) {
			stream->counts.other_layer++;
			continue;
		}

		/* The first VCL NAL unit ends the parameter sets. */
		if (type < STREAM_FIRST_NON_VCL) {
			done = 1;
			continue;
		}

		/* Stores a parameter set; a refused one makes the call fail. */
		if (type == H265SW_NAL_VPS || type == H265SW_NAL_SPS || type == H265SW_NAL_PPS) {
			error = stream_store_set(stream, type, nal, nal_size);
			if (error != 0)
				failed = 1;
		}
	}

	/* Reports a parameter set that did not read. */
	if (failed != 0)
		return EINVAL;

	/* Succeeded: every parameter set of the buffer is stored. */
	return 0;
}

/*
 * Reads the next picture to decode from the buffer.
 *
 * NAL units are read one at a time from the cursor; stream_step decides
 * what each one does to the picture being gathered.  A picture that ends
 * is returned when it can be decoded and otherwise counted and left
 * behind, and the reading goes on with the next one.
 */
int
h265_stream_next(
	struct h265_stream *stream,
	struct h265_picture *picture)
{
	struct stream_gather gather;
	const uint8_t *nal;
	size_t nal_size;
	size_t before;
	int found;
	int ended;
	int returned;

	/* Refuses a missing reader or picture. */
	if (stream == NULL)
		return EINVAL;
	if (picture == NULL)
		return EINVAL;

	/* Starts with no picture being gathered. */
	stream_gather_init(&gather);

	/* Reads NAL units until a picture is returned or the buffer is used up. */
	for (;;) {
		/* Remembers where this NAL unit starts, to come back to it. */
		before = stream->cursor;

		/* A scan that reached the segment a look ahead found is an ordinary scan again. */
		if (gather.join_end != STREAM_NO_POSITION && before >= gather.join_end)
			gather.join_end = STREAM_NO_POSITION;

		/* Finds the next NAL unit of the buffer. */
		found = vcodec_nal_find(stream->data, stream->size, &stream->cursor, &nal, &nal_size);

		/* The end of the buffer ends the picture being gathered, if there is one. */
		ended = 0;
		if (found == 0) {
			if (gather.open == 0)
				return ENOENT;
			ended = 1;
		} else {
			/* Lets the NAL unit act on the picture being gathered. */
			ended = stream_step(stream, picture, &gather, before, nal, nal_size);
		}

		/* A NAL unit that does not end the picture goes on to the next one. */
		if (ended == 0)
			continue;

		/* Turns the ended picture into a result, or leaves it behind. */
		returned = stream_finish(stream, picture, &gather);
		if (returned != 0)
			return 0;

		/* A picture lost to an allocation failure is reported once it has been walked past. */
		if (gather.nomem != 0)
			return ENOMEM;

		/* Starts gathering the next picture. */
		stream_gather_init(&gather);
	}
}

/*
 * Starts again from a random access point (a seek).
 *
 * The buffer and its cursor are left as they are (the caller gives a new
 * buffer when it seeks); the parameter sets and the counts are kept.
 */
void
h265_stream_reset(
	struct h265_stream *stream)
{
	/* A missing reader has nothing to restart. */
	if (stream == NULL)
		return;

	/* Waits for an IRAP picture again; the picture that comes is picture 0. */
	stream->started = 0;
	stream->first_pending = 1;
	stream->rasl_skip = 0;

	/* No end of sequence is pending or seen: the decoder is empty after a seek. */
	stream->eos_pending = 0;
	stream->eos_seen = 0;

	/* The IRAP picture that comes is handled as a BLA picture (HandleCraAsBlaFlag, 8.1.3). */
	stream->cra_as_bla = 1;

	/* The order count state starts again. */
	h265sw_poc_init(&stream->poc);
}

/* Starts a gather with no picture, no access unit and no look ahead. */
static void
stream_gather_init(
	struct stream_gather *gather)
{
	/* Nothing is gathered yet. */
	gather->open = 0;
	gather->dead = 0;
	gather->nomem = 0;
	gather->au_first = STREAM_NO_POSITION;
	gather->au_end = 0U;
	gather->join_end = STREAM_NO_POSITION;
	gather->sps_generation = 0U;
}

/*
 * Reads the type and layer of a NAL unit from its two header bytes (7.3.1.2).
 *
 * Returns 0 and the type and layer; EINVAL when the unit is shorter than
 * its header, forbidden_zero_bit is 1 or nuh_temporal_id_plus1 is 0.
 */
static int
stream_nal_header(
	const uint8_t *nal,
	size_t nal_size,
	unsigned *type,
	unsigned *layer)
{
	/* A unit without both header bytes has no type. */
	if (nal_size < STREAM_NAL_HEADER_BYTES)
		return EINVAL;

	/* forbidden_zero_bit must be 0. */
	if ((nal[0] & 0x80U) != 0U)
		return EINVAL;

	/* nuh_temporal_id_plus1 must not be 0. */
	if ((nal[1] & 7U) == 0U)
		return EINVAL;

	/* The type is bits 1..6 of the first byte; the layer id spans both bytes. */
	*type = ((unsigned)nal[0] >> 1) & 0x3FU;
	*layer = (((unsigned)nal[0] & 1U) << 5) | ((unsigned)nal[1] >> 3);

	/* Succeeded: the type and the layer id. */
	return 0;
}

/*
 * Reports whether a NAL unit whose header does not read was meant to be a
 * VPS, SPS or PPS, by the type bits of its first byte (1), or not (0).
 */
static int
stream_damaged_set(
	const uint8_t *nal,
	size_t nal_size)
{
	unsigned raw_type;

	/* A unit without a first byte has no type. */
	if (nal_size < 1U)
		return 0;

	/* The type bits are bits 1..6 of the first byte. */
	raw_type = ((unsigned)nal[0] >> 1) & 0x3FU;
	if (raw_type == H265SW_NAL_VPS)
		return 1;
	if (raw_type == H265SW_NAL_SPS)
		return 1;
	if (raw_type == H265SW_NAL_PPS)
		return 1;

	/* Succeeded: it was no parameter set. */
	return 0;
}

/*
 * Reports the offset of the start code of a NAL unit whose bytes begin at
 * nal_offset: three bytes back, and one more when a zero_byte precedes
 * the start code prefix (B.2).
 */
static size_t
stream_start_code(
	const uint8_t *data,
	size_t nal_offset)
{
	size_t start;

	/* The start code prefix is the three bytes before the NAL unit. */
	if (nal_offset < 3U)
		return 0U;
	start = nal_offset - 3U;

	/* A zero byte before the prefix is the zero_byte of a four-byte start code. */
	if (start > 0U && data[start - 1U] == 0U)
		start--;

	/* Succeeded: where the start code begins. */
	return start;
}

/*
 * Stores one VPS, SPS or PPS NAL unit in the store.
 *
 * Returns 0; EINVAL when the NAL unit does not read (its header does not
 * parse, it is longer than the RBSP copy, or the parameter set parse
 * refuses it); the failure is counted in refused_sets and reason says why.
 */
static int
stream_store_set(
	struct h265_stream *stream,
	unsigned type,
	const uint8_t *nal,
	size_t nal_size)
{
	struct h265sw_nal info;
	struct vcodec_bitreader bits;
	int error;

	/* Copies the RBSP of the NAL unit into the reader's buffer. */
	error = h265sw_nal_parse_prefix(nal, nal_size, H265_STREAM_RBSP_BYTES, stream->rbsp, H265_STREAM_RBSP_BYTES, &info);
	if (error != 0) {
		stream->counts.refused_sets++;
		stream->reason = "parameter set NAL unit does not parse";
		return EINVAL;
	}

	/* Refuses a parameter set longer than the copy. */
	if (info.complete == 0) {
		stream->counts.refused_sets++;
		stream->reason = "parameter set NAL unit is too long";
		return EINVAL;
	}

	/* Reads the parameter set of the NAL unit's type into the store. */
	vcodec_bitreader_init(&bits, info.rbsp, info.rbsp_size);
	if (type == H265SW_NAL_VPS) {
		error = h265sw_ps_read_vps(&stream->store, &bits);
	} else if (type == H265SW_NAL_SPS) {
		error = h265sw_ps_read_sps(&stream->store, &bits);
	} else {
		error = h265sw_ps_read_pps(&stream->store, &bits);
	}

	/* Counts a parameter set the parse refused and goes on. */
	if (error != 0) {
		stream->counts.refused_sets++;
		stream->reason = "parameter set was refused";
		return EINVAL;
	}

	/* Succeeded: the parameter set is in the store. */
	return 0;
}

/*
 * Looks past the NAL units that follow a picture's last segment to the
 * next base-layer VCL NAL unit, without storing or counting anything.
 *
 * Returns 1 and the position of that NAL unit in *position when its
 * first_slice_segment_in_pic_flag is 0 (the NAL units in between belong
 * to the picture's access unit); 0 when it is 1, when an end of sequence
 * or end of bitstream NAL unit comes first, or when the buffer ends (the
 * picture ends before them).
 */
static int
stream_lookahead(
	const struct h265_stream *stream,
	size_t from,
	size_t *position)
{
	size_t cursor;
	size_t before;
	const uint8_t *nal;
	size_t nal_size;
	unsigned type;
	unsigned layer;
	unsigned first_flag;
	int found;
	int error;
	int decodable;

	/* Scans NAL units one at a time from the first NAL unit after the picture. */
	cursor = from;
	for (;;) {
		/* Finds the next NAL unit; the end of the buffer ends the picture. */
		before = cursor;
		found = vcodec_nal_find(stream->data, stream->size, &cursor, &nal, &nal_size);
		if (found == 0)
			return 0;

		/* Skips a NAL unit whose header does not read, and the other layers. */
		error = stream_nal_header(nal, nal_size, &type, &layer);
		if (error != 0)
			continue;
		if (layer != 0U)
			continue;

		/* Decides at a VCL NAL unit of a decodable type; a reserved type is skipped like the other reserved NAL units. */
		if (type < STREAM_FIRST_NON_VCL) {
			decodable = h265sw_nal_is_decodable(type);
			if (decodable == 0)
				continue;

			/* A segment too short to hold its flag starts a picture of its own. */
			first_flag = 1U;
			if (nal_size >= 3U)
				first_flag = (unsigned)nal[2] >> 7;
			if (first_flag != 0U)
				return 0;

			/* The segment continues the picture: the NAL units before it belong to its access unit. */
			*position = before;
			return 1;
		}

		/* An end of sequence or end of bitstream NAL unit ends the access unit it is in. */
		if (type == H265SW_NAL_EOS || type == H265SW_NAL_EOB)
			return 0;
	}
}

/*
 * Finds the PPS a slice segment names and the SPS that PPS names, and
 * checks the pair (7.4.3.3.1).
 *
 * Returns STREAM_SEGMENT_OK with the two sets; STREAM_SEGMENT_REFUSED
 * when the last PPS or SPS of that id was refused as outside the range;
 * STREAM_SEGMENT_BROKEN when one is missing or the pair does not agree.
 */
static int
stream_find_sets(
	struct h265_stream *stream,
	unsigned pps_id,
	const struct h265sw_sps **sps,
	const struct h265sw_pps **pps)
{
	const struct h265sw_pps *found_pps;
	const struct h265sw_sps *found_sps;
	unsigned sps_id;
	int error;

	/* A PPS id outside the store is no PPS. */
	if (pps_id >= H265SW_PPS_IDS) {
		stream->reason = "PPS id is out of range";
		return STREAM_SEGMENT_BROKEN;
	}

	/* A PPS the parse refused is outside the range, not damage. */
	if (stream->store.pps_refused[pps_id] != 0) {
		stream->reason = "slice names a PPS outside the range";
		return STREAM_SEGMENT_REFUSED;
	}

	/* A PPS never stored breaks the picture. */
	if (stream->store.pps_present[pps_id] == 0) {
		stream->reason = "slice names a PPS that is missing";
		return STREAM_SEGMENT_BROKEN;
	}

	/* Takes the PPS and the id of the SPS it names. */
	found_pps = &stream->store.pps[pps_id];
	sps_id = found_pps->sps_id;
	if (sps_id >= H265SW_SPS_IDS) {
		stream->reason = "SPS id is out of range";
		return STREAM_SEGMENT_BROKEN;
	}

	/* An SPS the parse refused is outside the range, not damage. */
	if (stream->store.sps_refused[sps_id] != 0) {
		stream->reason = "slice names an SPS outside the range";
		return STREAM_SEGMENT_REFUSED;
	}

	/* An SPS never stored breaks the picture. */
	if (stream->store.sps_present[sps_id] == 0) {
		stream->reason = "slice names an SPS that is missing";
		return STREAM_SEGMENT_BROKEN;
	}

	/* Checks the values of the PPS that depend on the SPS. */
	found_sps = &stream->store.sps[sps_id];
	error = h265sw_ps_check_pair(found_sps, found_pps);
	if (error != 0) {
		stream->reason = "PPS does not agree with its SPS";
		return STREAM_SEGMENT_BROKEN;
	}

	/* Succeeded: the pair is usable. */
	*sps = found_sps;
	*pps = found_pps;
	return STREAM_SEGMENT_OK;
}

/*
 * Checks that an SPS is the one the picture or the coded video sequence
 * is bound to (7.4.3.2.1: the active SPS does not change inside a coded
 * video sequence, and its content does not change inside a picture).
 *
 * Returns STREAM_SEGMENT_OK, or STREAM_SEGMENT_BROKEN when expect asks
 * for a comparison and the SPS id or its generation differs.
 */
static int
stream_check_expect(
	const struct h265_stream *stream,
	const struct stream_expect *expect,
	const struct h265sw_sps *sps)
{
	/* Nothing to compare: the SPS is being activated by an IRAP picture. */
	if (expect->check == 0)
		return STREAM_SEGMENT_OK;

	/* A picture that names another SPS than the one in use is damaged. */
	if (sps->id != expect->sps_id)
		return STREAM_SEGMENT_BROKEN;

	/* An SPS whose content changed since is another SPS. */
	if (stream->store.sps_generation[sps->id] != expect->generation)
		return STREAM_SEGMENT_BROKEN;

	/* Succeeded: the SPS is the expected one. */
	return STREAM_SEGMENT_OK;
}

/*
 * Reads a slice segment header again from the longer copy of its NAL
 * unit (H265_STREAM_SLICE_BYTES), after the shorter copy ran out.
 *
 * Returns STREAM_SEGMENT_OK; STREAM_SEGMENT_NOMEM when slice_rbsp cannot
 * be allocated; STREAM_SEGMENT_REFUSED when the header uses a tool outside
 * the range; STREAM_SEGMENT_BROKEN when it still does not read.
 */
static int
stream_read_long(
	struct h265_stream *stream,
	const uint8_t *nal,
	size_t nal_size,
	struct h265sw_slice_header *header,
	const struct h265sw_slice_header *independent,
	const struct h265sw_sps *sps,
	const struct h265sw_pps *pps,
	struct h265sw_nal *info)
{
	struct vcodec_bitreader bits;
	int error;

	/* Allocates the longer copy once; it is kept until the reader is destroyed. */
	if (stream->slice_rbsp == NULL) {
		stream->slice_rbsp = malloc(H265_STREAM_SLICE_BYTES);
		if (stream->slice_rbsp == NULL) {
			stream->reason = "no memory for the long slice segment copy";
			return STREAM_SEGMENT_NOMEM;
		}
		stream->slice_rbsp_capacity = H265_STREAM_SLICE_BYTES;
	}

	/* Copies the RBSP of the first H265_STREAM_SLICE_BYTES payload bytes. */
	error = h265sw_nal_parse_prefix(nal, nal_size, H265_STREAM_SLICE_BYTES, stream->slice_rbsp, stream->slice_rbsp_capacity, info);
	if (error != 0) {
		stream->reason = "slice segment NAL unit does not parse";
		return STREAM_SEGMENT_BROKEN;
	}

	/* Reads the first fields again from the longer copy. */
	vcodec_bitreader_init(&bits, info->rbsp, info->rbsp_size);
	error = h265sw_slice_read_start(header, &bits, info);
	if (error != 0) {
		stream->reason = "slice segment header does not read";
		return STREAM_SEGMENT_BROKEN;
	}

	/* Reads the rest of the header from the longer copy. */
	error = h265sw_slice_read_rest(header, &bits, sps, pps, independent);
	if (error == ENOTSUP) {
		stream->reason = "slice segment header uses a tool outside the range";
		return STREAM_SEGMENT_REFUSED;
	}
	if (error != 0) {
		stream->reason = "slice segment header does not read";
		return STREAM_SEGMENT_BROKEN;
	}

	/* Succeeded: the header is read. */
	return STREAM_SEGMENT_OK;
}

/*
 * Reads one slice segment header with the parameter sets it names.
 *
 * Copies the start of the NAL unit's payload as an RBSP, reads the first
 * fields, finds and checks the PPS and SPS, compares the SPS with expect,
 * reads the rest, and reads the header again from the longer copy when
 * the shorter one ran out.  independent is the preceding independent
 * header of the picture (NULL for its first segment).  info, sps and pps
 * are the NAL unit and the sets the header was read with.
 *
 * Returns STREAM_SEGMENT_OK, _REFUSED, _BROKEN or _NOMEM.
 */
static int
stream_read_segment(
	struct h265_stream *stream,
	const uint8_t *nal,
	size_t nal_size,
	struct h265sw_slice_header *header,
	const struct h265sw_slice_header *independent,
	const struct stream_expect *expect,
	struct h265sw_nal *info,
	const struct h265sw_sps **sps,
	const struct h265sw_pps **pps)
{
	struct vcodec_bitreader bits;
	int error;
	int outcome;

	/* Copies the RBSP of the first payload bytes into the reader's buffer. */
	error = h265sw_nal_parse_prefix(nal, nal_size, H265_STREAM_RBSP_BYTES, stream->rbsp, H265_STREAM_RBSP_BYTES, info);
	if (error != 0) {
		stream->reason = "slice segment NAL unit does not parse";
		return STREAM_SEGMENT_BROKEN;
	}

	/* Reads the fields up to the PPS id, to find the parameter sets. */
	vcodec_bitreader_init(&bits, info->rbsp, info->rbsp_size);
	error = h265sw_slice_read_start(header, &bits, info);
	if (error != 0) {
		stream->reason = "slice segment header does not read";
		return STREAM_SEGMENT_BROKEN;
	}

	/* Finds the PPS and the SPS the segment names. */
	outcome = stream_find_sets(stream, header->pps_id, sps, pps);
	if (outcome != STREAM_SEGMENT_OK)
		return outcome;

	/* Checks that the SPS is the one the picture or the sequence is bound to. */
	outcome = stream_check_expect(stream, expect, *sps);
	if (outcome != STREAM_SEGMENT_OK) {
		stream->reason = "slice names another SPS than the one in use";
		return outcome;
	}

	/* Reads the rest of the header. */
	error = h265sw_slice_read_rest(header, &bits, *sps, *pps, independent);
	if (error == 0)
		return STREAM_SEGMENT_OK;

	/* A tool outside the range is not damage. */
	if (error == ENOTSUP) {
		stream->reason = "slice segment header uses a tool outside the range";
		return STREAM_SEGMENT_REFUSED;
	}

	/* A header that failed to read from the whole payload is broken. */
	if (info->complete != 0) {
		stream->reason = "slice segment header does not read";
		return STREAM_SEGMENT_BROKEN;
	}

	/* The short copy may have ended inside the header: reads again from the longer copy. */
	outcome = stream_read_long(stream, nal, nal_size, header, independent, *sps, *pps, info);

	/* Succeeded or failed as the longer copy decided. */
	return outcome;
}

/*
 * Opens a picture with its first slice segment: decides whether the type
 * lets the picture be returned, reads the header, works out
 * NoRaslOutputFlag, the order count and the reference picture set order
 * counts, and fills the picture.
 *
 * Returns STREAM_SEGMENT_OK; _DROPPED for a picture before the first IRAP
 * picture or a RASL picture to skip (counted here); _BROKEN, _REFUSED or
 * _NOMEM as stream_read_segment says.
 */
static int
stream_first_segment(
	struct h265_stream *stream,
	struct h265_picture *picture,
	struct stream_gather *gather,
	const uint8_t *nal,
	size_t nal_size)
{
	struct h265sw_nal info;
	struct stream_expect expect;
	const struct h265sw_sps *sps;
	const struct h265sw_pps *pps;
	unsigned type;
	int irap;
	int idr;
	int bla;
	int rasl;
	int no_rasl;
	int error;
	int outcome;
	int32_t poc;

	/* Takes the type from the header bytes (the caller checked the unit has them). */
	type = ((unsigned)nal[0] >> 1) & 0x3FU;
	irap = h265sw_nal_is_irap(type);
	idr = h265sw_nal_is_idr(type);
	bla = h265sw_nal_is_bla(type);
	rasl = h265sw_nal_is_rasl(type);

	/* A picture before the first IRAP picture cannot be decoded (8.1.3). */
	if (irap == 0 && stream->started == 0) {
		stream->counts.before_irap++;
		stream->reason = "picture before the first IRAP picture";
		return STREAM_SEGMENT_DROPPED;
	}

	/* A RASL picture of an IRAP picture with NoRaslOutputFlag 1 is not decoded (8.1.3). */
	if (rasl != 0 && stream->rasl_skip != 0) {
		stream->counts.rasl_skipped++;
		stream->reason = "RASL picture of an IRAP picture that starts the decoding";
		return STREAM_SEGMENT_DROPPED;
	}

	/*
	 * Works out NoRaslOutputFlag (8.1.3): an IRAP picture has it at an IDR
	 * or BLA picture, at the first picture of the stream, after an end of
	 * sequence or end of bitstream NAL unit, and for a CRA picture handled
	 * as a BLA picture; any other picture has 0.
	 */
	no_rasl = 0;
	if (irap != 0) {
		if (idr != 0) {
			no_rasl = 1;
		} else if (bla != 0) {
			no_rasl = 1;
		} else if (stream->first_pending != 0) {
			no_rasl = 1;
		} else if (stream->eos_pending != 0) {
			no_rasl = 1;
		} else if (stream->cra_as_bla != 0) {
			no_rasl = 1;
		}
	}

	/*
	 * An IRAP picture with NoRaslOutputFlag 1 activates its SPS (7.4.3.2.1);
	 * any other picture must use the active one with the content it had.
	 */
	expect.check = 1;
	expect.sps_id = stream->active_sps;
	expect.generation = stream->active_generation;
	if (irap != 0 && no_rasl != 0)
		expect.check = 0;

	/* Reads the header with the parameter sets it names. */
	outcome = stream_read_segment(stream, nal, nal_size, &picture->header, NULL, &expect, &info, &sps, &pps);
	if (outcome != STREAM_SEGMENT_OK)
		return outcome;

	/* Derives PicOrderCntVal (8.3.1). */
	error = h265sw_poc_compute(&stream->poc, &picture->header, no_rasl, sps->log2_max_poc_lsb, &poc);
	if (error != 0) {
		stream->reason = "picture order count is out of range";
		return STREAM_SEGMENT_BROKEN;
	}

	/* Derives the order counts of the reference picture set (8.3.2). */
	error = h265sw_poc_rps(&picture->header, poc, sps->log2_max_poc_lsb, &picture->rps);
	if (error != 0) {
		stream->reason = "reference picture set order count is out of range";
		return STREAM_SEGMENT_BROKEN;
	}

	/* Fills what the picture says about itself. */
	picture->nal_type = picture->header.nal_type;
	picture->temporal_id = picture->header.temporal_id;
	picture->vps_id = sps->vps_id;
	picture->sps_id = sps->id;
	picture->pps_id = pps->id;
	picture->generation = 0U;
	picture->irap = irap;
	picture->idr = idr;
	picture->no_rasl_output = no_rasl;
	picture->poc = poc;
	picture->pic_output = picture->header.pic_output;
	picture->index = 0U;

	/* The reader's state when the segment was read: first picture, after an end of sequence. */
	picture->first = stream->first_pending;
	picture->after_eos = stream->eos_pending;

	/*
	 * NoOutputOfPriorPicsFlag (C.5.2.2 step 1) of an IRAP picture with
	 * NoRaslOutputFlag 1 that is not the first picture: 1 for a CRA
	 * picture, else the flag of the header.
	 */
	picture->no_output_of_prior_pics = 0;
	if (irap != 0 && no_rasl != 0 && picture->first == 0) {
		if (type == H265SW_NAL_CRA) {
			picture->no_output_of_prior_pics = 1;
		} else {
			picture->no_output_of_prior_pics = picture->header.no_output_of_prior_pics;
		}
	}

	/* Records the first segment: its bytes in the buffer. */
	picture->slice_count = 1U;
	picture->slice_offset[0] = (size_t)(nal - stream->data);
	picture->slice_size[0] = nal_size;

	/* Remembers the SPS content, so a change inside the picture is noticed. */
	gather->sps_generation = stream->store.sps_generation[sps->id];

	/* The first segment is independent; later dependent segments copy it. */
	memcpy(&stream->independent, &picture->header, sizeof(stream->independent));

	/* Succeeded: the picture is open. */
	return STREAM_SEGMENT_OK;
}

/*
 * Adds a later slice segment to the picture being gathered: it must have
 * the picture's nal_unit_type and PPS, and the picture may not exceed
 * H265SW_MAX_SLICE_SEGMENTS segments.
 *
 * Returns STREAM_SEGMENT_OK, or _BROKEN, _REFUSED, _NOMEM as
 * stream_read_segment says.
 */
static int
stream_later_segment(
	struct h265_stream *stream,
	struct h265_picture *picture,
	struct stream_gather *gather,
	const uint8_t *nal,
	size_t nal_size)
{
	struct h265sw_slice_header segment;
	struct h265sw_nal info;
	struct stream_expect expect;
	const struct h265sw_sps *sps;
	const struct h265sw_pps *pps;
	unsigned type;
	int outcome;

	/* Refuses a picture of more segments than Table A.8 allows. */
	if (picture->slice_count >= H265SW_MAX_SLICE_SEGMENTS) {
		stream->reason = "picture has too many slice segments";
		return STREAM_SEGMENT_BROKEN;
	}

	/* Every segment of a picture has the same nal_unit_type (7.4.2.4.5). */
	type = ((unsigned)nal[0] >> 1) & 0x3FU;
	if (type != picture->nal_type) {
		stream->reason = "slice segments of one picture differ in nal_unit_type";
		return STREAM_SEGMENT_BROKEN;
	}

	/* The segment must use the SPS the first segment used, with its content. */
	expect.check = 1;
	expect.sps_id = picture->sps_id;
	expect.generation = gather->sps_generation;

	/* Reads the header; a dependent segment copies the fields of the last independent one. */
	outcome = stream_read_segment(stream, nal, nal_size, &segment, &stream->independent, &expect, &info, &sps, &pps);
	if (outcome != STREAM_SEGMENT_OK)
		return outcome;

	/* Every segment of a picture names the same PPS (7.4.7.1). */
	if (segment.pps_id != picture->pps_id) {
		stream->reason = "slice segments of one picture name different PPSs";
		return STREAM_SEGMENT_BROKEN;
	}

	/* Records the segment: its bytes in the buffer. */
	picture->slice_offset[picture->slice_count] = (size_t)(nal - stream->data);
	picture->slice_size[picture->slice_count] = nal_size;
	picture->slice_count++;

	/* An independent segment is the one dependent segments copy from now on. */
	if (segment.dependent == 0)
		memcpy(&stream->independent, &segment, sizeof(stream->independent));

	/* Succeeded: the segment joined the picture. */
	return STREAM_SEGMENT_OK;
}

/*
 * Marks the picture being gathered as one that is not returned and counts
 * it by the reason the outcome names (_DROPPED was counted by its rule).
 */
static void
stream_spoil(
	struct h265_stream *stream,
	struct stream_gather *gather,
	int outcome)
{
	/* The rest of the picture's segments are only walked past. */
	gather->dead = 1;

	/* Counts the picture under the reason it is left behind. */
	if (outcome == STREAM_SEGMENT_BROKEN) {
		stream->counts.broken_pictures++;
	} else if (outcome == STREAM_SEGMENT_REFUSED) {
		stream->counts.refused_sets++;
	} else if (outcome == STREAM_SEGMENT_NOMEM) {
		stream->counts.broken_pictures++;
		gather->nomem = 1;
	}
}

/*
 * Acts on one base-layer VCL NAL unit that was read.
 *
 * A reserved type is counted and skipped.  A segment that starts a new
 * picture ends the picture being gathered (the cursor goes back to it).
 * Otherwise the segment opens a picture or joins the one being gathered.
 *
 * Returns 1 when the picture being gathered ended, else 0.
 */
static int
stream_vcl(
	struct h265_stream *stream,
	struct h265_picture *picture,
	struct stream_gather *gather,
	size_t before,
	const uint8_t *nal,
	size_t nal_size)
{
	size_t nal_end;
	unsigned type;
	unsigned first_flag;
	int decodable;
	int outcome;

	/* Takes the type and where the NAL unit ends. */
	type = ((unsigned)nal[0] >> 1) & 0x3FU;
	nal_end = (size_t)(nal - stream->data) + nal_size;

	/* A reserved VCL type is skipped (7.4.2.2), as part of the access unit it is in. */
	decodable = h265sw_nal_is_decodable(type);
	if (decodable == 0) {
		stream->counts.reserved_type++;
		stream->reason = "reserved VCL NAL unit type";
		if (gather->open != 0)
			gather->au_end = nal_end;
		return 0;
	}

	/* Reads first_slice_segment_in_pic_flag, the top bit of the first payload byte; a unit without one starts a picture. */
	first_flag = 1U;
	if (nal_size >= 3U)
		first_flag = (unsigned)nal[2] >> 7;

	/* A segment that begins a new picture ends the one being gathered; the cursor returns to it. */
	if (gather->open != 0 && first_flag != 0U) {
		stream->cursor = before;
		return 1;
	}

	/* Opens the picture this segment belongs to. */
	if (gather->open == 0) {
		gather->open = 1;
		gather->au_end = nal_end;

		/* A segment that is not the first of its picture belongs to a picture whose start was lost. */
		if (first_flag == 0U) {
			stream->counts.broken_pictures++;
			stream->reason = "slice segment without its picture's first segment";
			gather->dead = 1;
			return 0;
		}

		/* Reads the picture's first segment. */
		outcome = stream_first_segment(stream, picture, gather, nal, nal_size);
		if (outcome != STREAM_SEGMENT_OK)
			stream_spoil(stream, gather, outcome);
		return 0;
	}

	/* The segment belongs to the picture being gathered. */
	gather->au_end = nal_end;

	/* A picture that is left behind is only walked past. */
	if (gather->dead != 0)
		return 0;

	/* Adds the segment to the picture. */
	outcome = stream_later_segment(stream, picture, gather, nal, nal_size);
	if (outcome != STREAM_SEGMENT_OK)
		stream_spoil(stream, gather, outcome);

	/* Succeeded: the picture goes on. */
	return 0;
}

/*
 * Acts on one NAL unit that was read.
 *
 * A NAL unit of another layer is counted and skipped.  A VCL NAL unit is
 * handled by stream_vcl.  An end of sequence or end of bitstream NAL unit
 * ends the picture being gathered (or is pending for the next picture).  A
 * NAL unit that may start an access unit, when it follows a picture's
 * segment, makes the reader look ahead to the next segment: if that
 * segment starts a picture, this picture ends and the cursor goes back
 * to the NAL unit (so its parameter sets are stored after the picture
 * is returned); otherwise the NAL units belong to the access unit and
 * are read again with join_end set.  Parameter sets are stored as they
 * are read.
 *
 * Returns 1 when the picture being gathered ended, else 0.
 */
static int
stream_step(
	struct h265_stream *stream,
	struct h265_picture *picture,
	struct stream_gather *gather,
	size_t before,
	const uint8_t *nal,
	size_t nal_size)
{
	size_t nal_offset;
	size_t nal_end;
	size_t position;
	unsigned type;
	unsigned layer;
	unsigned raw_type;
	int error;
	int starts;
	int continues;
	int decodable;
	int damaged;
	int ended;

	/* Takes where the NAL unit lies in the buffer. */
	nal_offset = (size_t)(nal - stream->data);
	nal_end = nal_offset + nal_size;

	/* Reads the NAL unit header. */
	error = stream_nal_header(nal, nal_size, &type, &layer);
	if (error != 0) {
		stream->reason = "NAL unit header does not read";

		/* A parameter set that does not read is counted and the reader goes on. */
		damaged = stream_damaged_set(nal, nal_size);
		if (damaged != 0)
			stream->counts.refused_sets++;

		/* A damaged slice segment spoils the picture being gathered. */
		if (gather->open != 0 && gather->dead == 0 && nal_size >= 1U) {
			raw_type = ((unsigned)nal[0] >> 1) & 0x3FU;
			decodable = h265sw_nal_is_decodable(raw_type);
			if (decodable != 0)
				stream_spoil(stream, gather, STREAM_SEGMENT_BROKEN);
		}
		return 0;
	}

	/* The other layers are skipped (the base layer's decoding does not read them). */
	if (layer != 0U) {
		stream->counts.other_layer++;
		return 0;
	}

	/* The first NAL unit read for a picture is where its access unit starts. */
	if (gather->au_first == STREAM_NO_POSITION)
		gather->au_first = stream_start_code(stream->data, nal_offset);

	/* A VCL NAL unit is a slice segment (or a reserved type). */
	if (type < STREAM_FIRST_NON_VCL) {
		ended = stream_vcl(stream, picture, gather, before, nal, nal_size);
		return ended;
	}

	/*
	 * An end of sequence or end of bitstream NAL unit ends the access
	 * unit it is in: the next picture gets after_eos and NoRaslOutputFlag.
	 */
	if (type == H265SW_NAL_EOS || type == H265SW_NAL_EOB) {
		if (gather->open == 0) {
			/* No picture is being gathered: the unit is pending for the next picture. */
			stream->eos_pending = 1;
			gather->au_first = STREAM_NO_POSITION;
			return 0;
		}

		/* The unit belongs to the access unit of the picture being gathered. */
		stream->eos_seen = 1;
		gather->au_end = nal_end;
		return 1;
	}

	/* A NAL unit that may start an access unit, after a picture's segment, needs a look ahead. */
	starts = h265sw_nal_starts_access_unit(type);
	if (gather->open != 0 && starts != 0 && gather->join_end == STREAM_NO_POSITION) {
		continues = stream_lookahead(stream, before, &position);
		if (continues == 0) {
			/* The next picture's access unit starts here: the picture ends and this unit is read again later. */
			stream->cursor = before;
			return 1;
		}

		/* The units up to the segment belong to this picture: reads them again, storing the parameter sets. */
		stream->cursor = before;
		gather->join_end = position;
		return 0;
	}

	/* Stores a parameter set as it is read; a refused one is counted by the store. */
	if (type == H265SW_NAL_VPS || type == H265SW_NAL_SPS || type == H265SW_NAL_PPS)
		error = stream_store_set(stream, type, nal, nal_size);

	/* The unit belongs to the access unit of the picture being gathered. */
	if (gather->open != 0)
		gather->au_end = nal_end;

	/* Succeeded: the picture goes on. */
	return 0;
}

/*
 * Turns the picture that ended into the result: returns 1 with the
 * picture filled when it can be decoded, else 0 (it was left behind).
 *
 * A returned picture is recorded as prevTid0Pic when it qualifies, starts
 * the decoding at an IRAP picture, activates the SPS at an IRAP picture
 * with NoRaslOutputFlag 1, and consumes the pending end of sequence.  A
 * picture left behind keeps the order count state and leaves a seen end
 * of sequence pending.
 */
static int
stream_finish(
	struct h265_stream *stream,
	struct h265_picture *picture,
	struct stream_gather *gather)
{
	/* A gather with no picture has nothing to finish. */
	if (gather->open == 0)
		return 0;

	/* A picture left behind: an end of sequence in its access unit is still pending. */
	if (gather->dead != 0) {
		if (stream->eos_seen != 0) {
			stream->eos_pending = 1;
			stream->eos_seen = 0;
		}
		return 0;
	}

	/* Takes the access unit's range and the store's generation after the parameter sets of the access unit. */
	picture->access_unit = gather->au_first;
	picture->access_unit_size = gather->au_end - gather->au_first;
	picture->generation = stream->store.generation;

	/* Numbers the picture among those returned. */
	picture->index = stream->returned;
	stream->returned++;

	/* Records the picture's order count for the pictures after it (8.3.1). */
	h265sw_poc_finish(&stream->poc, &picture->header, picture->poc);

	/* An IRAP picture starts the decoding; its NoRaslOutputFlag decides about its RASL pictures and its SPS. */
	if (picture->irap != 0) {
		stream->started = 1;
		stream->cra_as_bla = 0;
		stream->rasl_skip = picture->no_rasl_output;
		if (picture->no_rasl_output != 0) {
			stream->active_sps = picture->sps_id;
			stream->active_generation = gather->sps_generation;
		}
	}

	/* The picture was returned: it was the first one, and the pending end of sequence is the one its own access unit had. */
	stream->first_pending = 0;
	stream->eos_pending = stream->eos_seen;
	stream->eos_seen = 0;

	/* Succeeded: the picture is ready for the caller. */
	return 1;
}
