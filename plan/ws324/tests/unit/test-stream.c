/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Tests of h265-stream: the Annex B reader that splits a byte stream
 * into NAL units, keeps the parameter sets and groups the slice segments
 * into the pictures to decode (7.4.2.4.4, 7.4.2.4.5, 8.1.3, 8.3.1, 8.3.2,
 * C.5.2.2).
 *
 * Inputs are Annex B streams written with h265write.h from values the
 * test chooses.  Expected values are the values written, the rules of the
 * clauses named in each check (worked by hand in the comments), and the
 * byte offsets of the NAL units the test itself appended.  Where the
 * header of h265-stream.h leaves a point open the check says what the
 * implementation decided.
 */

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "h265write.h"
#include "userland/desktop/libmedia/h265-stream.h"

/* The nal_unit_type values the tests name (Table 7-1). */
#define T_TRAIL_N 0U
#define T_TRAIL_R 1U
#define T_TSA_N 2U
#define T_RADL_N 6U
#define T_RADL_R 7U
#define T_RASL_N 8U
#define T_RASL_R 9U
#define T_BLA_W_LP 16U
#define T_BLA_W_RADL 17U
#define T_BLA_N_LP 18U
#define T_IDR_W_RADL 19U
#define T_IDR_N_LP 20U
#define T_CRA 21U
#define T_VPS 32U
#define T_SPS 33U
#define T_PPS 34U
#define T_AUD 35U
#define T_EOS 36U
#define T_EOB 37U
#define T_FD 38U
#define T_PREFIX_SEI 39U
#define T_SUFFIX_SEI 40U

/* The most NAL units one test stream holds, and the most pictures one run collects. */
#define MAX_NALS 1024U
#define MAX_RECS 1024U

/* The state of the pseudo-random generator, fixed so a failure repeats. */
static uint32_t random_state = 0x2B3C4D5EU;

/* The stream being built, and the byte buffer it is built in. */
static struct h265w_stream ws;
static uint8_t sbuf[1U << 20];

/* Where each NAL unit appended to the stream starts (its start code) and ends. */
static size_t nal_start[MAX_NALS];
static size_t nal_end[MAX_NALS];
static unsigned nal_count;

/* The writer structures the stream's parameter sets and slice headers are made from. */
static struct h265w_vps wvps;
static struct h265w_sps wsps;
static struct h265w_pps wpps;
static struct h265w_slice wslice;
static struct h265w_slice_context wctx;

/* The writer of one RBSP. */
static struct bitwriter bw;

/* The reader under test, the picture it fills, and a second reader for the comparisons. */
static struct h265_stream rd;
static struct h265_picture pic;

/*
 * What a returned picture says, kept for the checks (a picture is 11 KiB,
 * so a run keeps this summary of each one instead).
 */
struct rec {
	unsigned nal_type;
	unsigned temporal_id;
	unsigned vps_id;
	unsigned sps_id;
	unsigned pps_id;
	unsigned generation;
	int irap;
	int idr;
	int no_rasl;
	int first;
	int after_eos;
	int noop;
	int pic_output;
	int32_t poc;
	unsigned slice_count;
	size_t offset0;
	size_t size0;
	size_t offset_last;
	size_t size_last;
	size_t au;
	size_t au_size;
	uint64_t index;
	unsigned num_st_before;
	unsigned num_st_after;
	unsigned num_st_foll;
	int32_t st_before0;
	int32_t st_after0;
	int32_t st_foll0;
};

/* The pictures the last run returned. */
static struct rec recs[MAX_RECS];
static unsigned nrecs;

/* The result of the last call of h265_stream_next that was not a picture. */
static int last_error;

/* Returns the next pseudo-random number (xorshift32). */
static uint32_t
random_next(void)
{
	/* The three shifts of the generator. */
	random_state ^= random_state << 13;
	random_state ^= random_state >> 17;
	random_state ^= random_state << 5;

	/* Succeeded: the next number. */
	return random_state;
}

/* Returns a pseudo-random number below count (count above 0). */
static uint32_t
rnd(
	uint32_t count)
{
	/* Succeeded: the number. */
	return random_next() % count;
}

/* Starts a reader afresh (the old one is destroyed first). */
static void
reader_start(void)
{
	/* Frees the longer slice copy a former run may have made, then clears the reader. */
	h265_stream_destroy(&rd);
	h265_stream_init(&rd);
}

/* Appends one NAL unit made from raw bytes and records where it lies; returns its index. */
static unsigned
put_nal(
	unsigned type,
	unsigned layer,
	unsigned tid_plus1,
	const uint8_t *bytes,
	size_t size)
{
	unsigned index;

	/* Notes where the NAL unit starts, appends it, notes where it ends. */
	index = nal_count;
	nal_start[index] = ws.size;
	h265w_stream_bytes(&ws, type, layer, tid_plus1, bytes, size);
	nal_end[index] = ws.size;
	nal_count++;

	/* Succeeded: the NAL unit's index. */
	return index;
}

/* Appends one NAL unit whose RBSP is the written bits of bw; returns its index. */
static unsigned
put_bw(
	unsigned type,
	unsigned layer,
	unsigned tid_plus1)
{
	/* The written bytes, padded with zero bits to a whole byte. */
	return put_nal(type, layer, tid_plus1, bw.data, bitwriter_bytes(&bw));
}

/* Appends a VPS NAL unit from wvps. */
static unsigned
put_vps(void)
{
	/* Writes the VPS and appends it. */
	bitwriter_init(&bw);
	h265w_vps(&bw, &wvps);
	return put_bw(T_VPS, 0U, 1U);
}

/* Appends an SPS NAL unit from wsps. */
static unsigned
put_sps(void)
{
	/* Writes the SPS and appends it. */
	bitwriter_init(&bw);
	h265w_sps(&bw, &wsps);
	return put_bw(T_SPS, 0U, 1U);
}

/* Appends a PPS NAL unit from wpps. */
static unsigned
put_pps(void)
{
	/* Writes the PPS and appends it. */
	bitwriter_init(&bw);
	h265w_pps(&bw, &wpps);
	return put_bw(T_PPS, 0U, 1U);
}

/* Starts the three parameter sets at their defaults (64x64, CTB 16, 16 CTBs). */
static void
sets_default(void)
{
	/* Writer defaults. */
	h265w_vps_default(&wvps);
	h265w_sps_default(&wsps);
	h265w_pps_default(&wpps);
}

/* Starts a stream with the three default parameter sets. */
static void
begin(void)
{
	/* An empty stream, then VPS 0, SPS 0 and PPS 0. */
	h265w_stream_init(&ws, sbuf, sizeof(sbuf));
	nal_count = 0U;
	sets_default();
	put_vps();
	put_sps();
	put_pps();
}

/* Starts the slice header at its defaults for segment first/address and the order count lsb. */
static void
slice_reset(
	unsigned first,
	unsigned address,
	unsigned poc_lsb)
{
	/* An independent I slice of PPS 0 with no reference pictures. */
	h265w_slice_default(&wslice);
	wslice.first_in_pic = first;
	wslice.address = address;
	wslice.poc_lsb = poc_lsb;
	wslice.pps_id = wpps.pps_id;
}

/* Appends the slice segment of wslice as a NAL unit of nal_type and temporal id tid, with some slice data. */
static unsigned
put_slice(
	unsigned nal_type,
	unsigned tid)
{
	static const uint8_t data[4] = { 0xABU, 0xCDU, 0xEFU, 0x80U };
	uint8_t rbsp[BITWRITER_BYTES + 4U];
	size_t header_bytes;

	/* Writes the header with the syntax of the stream's own SPS and PPS. */
	h265w_slice_context_from(&wctx, &wsps, &wpps, nal_type);
	bitwriter_init(&bw);
	h265w_slice(&bw, &wslice, &wctx);
	header_bytes = bitwriter_bytes(&bw);

	/* The header, then four bytes of slice data. */
	memcpy(rbsp, bw.data, header_bytes);
	memcpy(rbsp + header_bytes, data, sizeof(data));
	return put_nal(nal_type, 0U, tid + 1U, rbsp, header_bytes + sizeof(data));
}

/* Appends a first segment of a picture of nal_type with order count lsb. */
static unsigned
put_picture(
	unsigned nal_type,
	unsigned poc_lsb)
{
	/* One independent first segment. */
	slice_reset(1U, 0U, poc_lsb);
	return put_slice(nal_type, 0U);
}

/* Appends a later segment (not the first of its picture) at address. */
static unsigned
put_segment(
	unsigned nal_type,
	unsigned address)
{
	/* An independent segment at address. */
	slice_reset(0U, address, 0U);
	return put_slice(nal_type, 0U);
}

/* Appends a NAL unit of type with a two-byte payload that no parse needs. */
static unsigned
put_plain(
	unsigned type)
{
	static const uint8_t payload[2] = { 0x11U, 0x80U };

	/* The type and a payload. */
	return put_nal(type, 0U, 1U, payload, sizeof(payload));
}

/* Copies what a picture says into a record. */
static void
record(
	const struct h265_picture *picture,
	struct rec *out)
{
	/* The scalar fields. */
	out->nal_type = picture->nal_type;
	out->temporal_id = picture->temporal_id;
	out->vps_id = picture->vps_id;
	out->sps_id = picture->sps_id;
	out->pps_id = picture->pps_id;
	out->generation = picture->generation;
	out->irap = picture->irap;
	out->idr = picture->idr;
	out->no_rasl = picture->no_rasl_output;
	out->first = picture->first;
	out->after_eos = picture->after_eos;
	out->noop = picture->no_output_of_prior_pics;
	out->pic_output = picture->pic_output;
	out->poc = picture->poc;
	out->slice_count = picture->slice_count;
	out->offset0 = picture->slice_offset[0];
	out->size0 = picture->slice_size[0];
	out->offset_last = picture->slice_offset[picture->slice_count - 1U];
	out->size_last = picture->slice_size[picture->slice_count - 1U];
	out->au = picture->access_unit;
	out->au_size = picture->access_unit_size;
	out->index = picture->index;

	/* The order counts of the reference picture set. */
	out->num_st_before = picture->rps.num_st_before;
	out->num_st_after = picture->rps.num_st_after;
	out->num_st_foll = picture->rps.num_st_foll;
	out->st_before0 = picture->rps.st_before[0];
	out->st_after0 = picture->rps.st_after[0];
	out->st_foll0 = picture->rps.st_foll[0];
}

/*
 * Checks what every picture the reader returns must satisfy, whatever the
 * input: its segments lie inside the buffer, in order, after the access
 * unit's start, and the access unit holds them.
 */
static void
check_picture_invariants(
	const struct h265_picture *picture,
	size_t size)
{
	unsigned index;

	/* The segment count is inside its bounds. */
	CHECK(picture->slice_count >= 1U);
	CHECK(picture->slice_count <= H265SW_MAX_SLICE_SEGMENTS);
	if (picture->slice_count < 1U || picture->slice_count > H265SW_MAX_SLICE_SEGMENTS)
		return;

	/* The access unit lies inside the buffer and holds the first segment's NAL unit and its start code. */
	CHECK(picture->access_unit <= size);
	CHECK(picture->access_unit_size <= size - picture->access_unit);
	CHECK(picture->slice_offset[0] >= picture->access_unit + 3U);

	/* Each segment is inside the buffer, inside the access unit, and after the one before it. */
	for (index = 0U; index < picture->slice_count; index++) {
		CHECK(picture->slice_size[index] >= 2U);
		CHECK(picture->slice_offset[index] <= size);
		CHECK(picture->slice_size[index] <= size - picture->slice_offset[index]);
		CHECK(picture->slice_offset[index] + picture->slice_size[index] <= picture->access_unit + picture->access_unit_size);
		if (index > 0U)
			CHECK(picture->slice_offset[index] >= picture->slice_offset[index - 1U] + picture->slice_size[index - 1U] + 3U);
	}

	/* The ids are inside the stores. */
	CHECK(picture->vps_id < H265SW_VPS_IDS);
	CHECK(picture->sps_id < H265SW_SPS_IDS);
	CHECK(picture->pps_id < H265SW_PPS_IDS);
}

/* Reads every picture of the buffer into recs (the reader is already started and holds the buffer). */
static void
collect(
	size_t size)
{
	int error;

	/* Reads pictures until the buffer is used up. */
	for (;;) {
		error = h265_stream_next(&rd, &pic);
		if (error != 0) {
			last_error = error;
			return;
		}

		/* Keeps the picture's summary and checks what must always hold. */
		if (nrecs < MAX_RECS)
			record(&pic, &recs[nrecs]);
		nrecs++;
		check_picture_invariants(&pic, size);
	}
}

/* Reads the whole stream built in sbuf with a fresh reader. */
static void
run_whole(void)
{
	/* A fresh reader, the buffer, and every picture. */
	reader_start();
	nrecs = 0U;
	h265_stream_input(&rd, sbuf, ws.size);
	collect(ws.size);
}

/* Checks the identity fields of picture i. */
static void
expect_pic(
	unsigned i,
	unsigned nal_type,
	int32_t poc,
	int first,
	int no_rasl,
	int after_eos,
	int noop)
{
	/* The picture must exist. */
	CHECK(i < nrecs);
	if (i >= nrecs)
		return;

	/* Its type, order count and flags. */
	CHECK(recs[i].nal_type == nal_type);
	CHECK(recs[i].poc == poc);
	CHECK(recs[i].first == first);
	CHECK(recs[i].no_rasl == no_rasl);
	CHECK(recs[i].after_eos == after_eos);
	CHECK(recs[i].noop == noop);
}

/* Checks the counts of the last run: before_irap, rasl, reserved, other layer, refused, broken. */
static void
expect_counts(
	unsigned before_irap,
	unsigned rasl,
	unsigned reserved,
	unsigned other_layer,
	unsigned refused,
	unsigned broken)
{
	/* Each count. */
	CHECK(rd.counts.before_irap == before_irap);
	CHECK(rd.counts.rasl_skipped == rasl);
	CHECK(rd.counts.reserved_type == reserved);
	CHECK(rd.counts.other_layer == other_layer);
	CHECK(rd.counts.refused_sets == refused);
	CHECK(rd.counts.broken_pictures == broken);
}

/* Reports whether a non-VCL type starts an access unit after a picture (7.4.2.4.4), from the clause's list. */
static int
type_starts_au(
	unsigned type)
{
	/* AUD, VPS, SPS, PPS, prefix SEI, the reserved 41..44 and the unspecified 48..55. */
	if (type == 35U || type == 32U || type == 33U || type == 34U || type == 39U)
		return 1;
	if (type >= 41U && type <= 44U)
		return 1;
	if (type >= 48U && type <= 55U)
		return 1;
	return 0;
}

/* Reports whether the reader's cursor is at NAL unit n (at its start code or its three-byte prefix). */
static int
cursor_at(
	unsigned n)
{
	/* A four-byte start code has its zero_byte first; the reader may stop at either. */
	if (rd.cursor == nal_start[n])
		return 1;
	if (rd.cursor == nal_start[n] + 1U)
		return 1;
	return 0;
}

/* IDR start: IDR then three TRAIL_R pictures. */
static void
check_idr_start(void)
{
	unsigned index;
	unsigned first_nal;
	unsigned last_nal;

	/* IDR at order count 0, then pictures with order count lsb 1, 2, 3. */
	begin();
	first_nal = 0U;
	put_picture(T_IDR_W_RADL, 0U);
	for (index = 1U; index <= 3U; index++)
		put_picture(T_TRAIL_R, index);
	last_nal = nal_count - 1U;
	run_whole();

	/* Four pictures, in decoding order, the first one the first of the stream. */
	CHECK(nrecs == 4U);
	CHECK(last_error == ENOENT);
	expect_pic(0U, T_IDR_W_RADL, 0, 1, 1, 0, 0);
	expect_pic(1U, T_TRAIL_R, 1, 0, 0, 0, 0);
	expect_pic(2U, T_TRAIL_R, 2, 0, 0, 0, 0);
	expect_pic(3U, T_TRAIL_R, 3, 0, 0, 0, 0);
	expect_counts(0U, 0U, 0U, 0U, 0U, 0U);

	/* The IDR is an IRAP and IDR picture; the others are neither. */
	CHECK(recs[0].irap == 1);
	CHECK(recs[0].idr == 1);
	CHECK(recs[1].irap == 0);
	CHECK(recs[1].idr == 0);

	/* The ids, the picture indexes and the output flag. */
	for (index = 0U; index < 4U; index++) {
		CHECK(recs[index].vps_id == 0U);
		CHECK(recs[index].sps_id == 0U);
		CHECK(recs[index].pps_id == 0U);
		CHECK(recs[index].index == index);
		CHECK(recs[index].pic_output == 1);
		CHECK(recs[index].slice_count == 1U);
		CHECK(recs[index].temporal_id == 0U);
	}

	/* The first picture's access unit starts at the VPS, the others at their own NAL unit. */
	CHECK(recs[0].au == nal_start[first_nal]);
	CHECK(recs[0].au_size == nal_end[3] - nal_start[0]);
	CHECK(recs[1].au == nal_start[4]);
	CHECK(recs[1].au_size == nal_end[4] - nal_start[4]);
	CHECK(recs[3].au == nal_start[last_nal]);
	CHECK(recs[3].au_size == nal_end[last_nal] - nal_start[last_nal]);

	/* The segment's offset and size are the NAL unit's bytes after its four-byte start code. */
	CHECK(recs[0].offset0 == nal_start[3] + 4U);
	CHECK(recs[0].size0 == nal_end[3] - nal_start[3] - 4U);
	CHECK(recs[2].offset0 == nal_start[5] + 4U);
	CHECK(recs[2].size0 == nal_end[5] - nal_start[5] - 4U);

	/* The generation is that of the store after the three parameter sets (3 sets, each new). */
	CHECK(recs[0].generation == 3U);
	CHECK(recs[3].generation == 3U);
	CHECK(rd.returned == 4U);
	CHECK(rd.started == 1);
	CHECK(rd.first_pending == 0);
	CHECK(rd.reason == NULL);
}

/* Segment offsets and sizes with three-byte start codes and a stream cut after the last NAL unit. */
static void
check_three_byte_start_codes(void)
{
	/* The same stream with three-byte start codes. */
	h265w_stream_init(&ws, sbuf, sizeof(sbuf));
	ws.start_code_bytes = 3U;
	nal_count = 0U;
	sets_default();
	put_vps();
	put_sps();
	put_pps();
	put_picture(T_IDR_N_LP, 0U);
	put_picture(T_TRAIL_N, 1U);
	run_whole();

	/* The offsets and the access units are exact with the shorter start code. */
	CHECK(nrecs == 2U);
	expect_pic(0U, T_IDR_N_LP, 0, 1, 1, 0, 0);
	expect_pic(1U, T_TRAIL_N, 1, 0, 0, 0, 0);
	CHECK(recs[0].au == nal_start[0]);
	CHECK(recs[0].offset0 == nal_start[3] + 3U);
	CHECK(recs[0].size0 == nal_end[3] - nal_start[3] - 3U);
	CHECK(recs[0].au_size == nal_end[3] - nal_start[0]);
	CHECK(recs[1].au == nal_start[4]);
	CHECK(recs[1].offset0 == nal_start[4] + 3U);
	CHECK(recs[1].au_size == nal_end[4] - nal_start[4]);
}

/* A reference picture set in the first segment gives the order counts of 8.3.2. */
static void
check_rps_counts(void)
{
	/* IDR, then a picture of lsb 5 whose set has deltas -1 (used), -3 (not used) and +2 (used). */
	begin();
	put_picture(T_IDR_W_RADL, 0U);
	slice_reset(1U, 0U, 5U);
	wslice.st_rps.num_negative_pics = 2U;
	wslice.st_rps.delta_poc_s0_minus1[0] = 0U;
	wslice.st_rps.used_by_curr_pic_s0_flag[0] = 1U;
	wslice.st_rps.delta_poc_s0_minus1[1] = 1U;
	wslice.st_rps.used_by_curr_pic_s0_flag[1] = 0U;
	wslice.st_rps.num_positive_pics = 1U;
	wslice.st_rps.delta_poc_s1_minus1[0] = 1U;
	wslice.st_rps.used_by_curr_pic_s1_flag[0] = 1U;
	put_slice(T_TRAIL_R, 0U);
	run_whole();

	/* DeltaPocS0 = -1, -3; DeltaPocS1 = +2 (7-61, 7-62): before {4}, foll {2}, after {7}. */
	CHECK(nrecs == 2U);
	expect_pic(1U, T_TRAIL_R, 5, 0, 0, 0, 0);
	CHECK(recs[1].num_st_before == 1U);
	CHECK(recs[1].st_before0 == 4);
	CHECK(recs[1].num_st_foll == 1U);
	CHECK(recs[1].st_foll0 == 2);
	CHECK(recs[1].num_st_after == 1U);
	CHECK(recs[1].st_after0 == 7);

	/* The IDR picture's set is empty. */
	CHECK(recs[0].num_st_before == 0U);
	CHECK(recs[0].num_st_after == 0U);
	CHECK(recs[0].num_st_foll == 0U);
}

/* Order counts across the wrap of the lsb (8.3.1) and the Tid0 rule for what is dropped. */
static void
check_poc_sequence(void)
{
	/*
	 * IDR (lsb 0, poc 0), TRAIL lsb 100 (poc 100), TRAIL lsb 200: with
	 * prevTid0Pic (lsb 100, Msb 0), 200 - 100 = 100 is below half of 256,
	 * so Msb stays 0 and poc is 200; TRAIL lsb 3: 3 < 200 and 200 - 3 =
	 * 197 >= 128, so Msb is 256 and poc is 259.
	 */
	begin();
	put_picture(T_IDR_W_RADL, 0U);
	put_picture(T_TRAIL_R, 100U);
	put_picture(T_TRAIL_R, 200U);
	put_picture(T_TRAIL_R, 3U);
	run_whole();
	CHECK(nrecs == 4U);
	expect_pic(1U, T_TRAIL_R, 100, 0, 0, 0, 0);
	expect_pic(2U, T_TRAIL_R, 200, 0, 0, 0, 0);
	expect_pic(3U, T_TRAIL_R, 259, 0, 0, 0, 0);

	/*
	 * A sub-layer non-reference picture and a picture of TemporalId 1 are
	 * not prevTid0Pic: IDR, TRAIL_N lsb 100 (poc 100), TRAIL lsb 200 is read
	 * against (0, 0): 200 - 0 = 200 > 128, so Msb is -256 and poc is -56.
	 */
	begin();
	put_picture(T_IDR_W_RADL, 0U);
	put_picture(T_TRAIL_N, 100U);
	put_picture(T_TRAIL_R, 200U);
	run_whole();
	CHECK(nrecs == 3U);
	expect_pic(2U, T_TRAIL_R, -56, 0, 0, 0, 0);

	/* The same with a picture of TemporalId 1 (TSA_N would be a sub-layer non-reference too). */
	begin();
	put_picture(T_IDR_W_RADL, 0U);
	slice_reset(1U, 0U, 100U);
	put_slice(T_TRAIL_R, 1U);
	put_picture(T_TRAIL_R, 200U);
	run_whole();
	CHECK(nrecs == 3U);
	CHECK(recs[1].temporal_id == 1U);
	expect_pic(2U, T_TRAIL_R, -56, 0, 0, 0, 0);

	/*
	 * A picture left behind does not change the order count state: a
	 * TRAIL picture of lsb 100 whose second segment names another PPS is
	 * broken, so the next picture of lsb 200 is read against (0, 0) and
	 * has poc -56, not 200.
	 */
	begin();
	wpps.pps_id = 1U;
	put_pps();
	wpps.pps_id = 0U;
	put_picture(T_IDR_W_RADL, 0U);
	put_picture(T_TRAIL_R, 100U);
	slice_reset(0U, 8U, 0U);
	wslice.pps_id = 1U;
	put_slice(T_TRAIL_R, 0U);
	put_picture(T_TRAIL_R, 200U);
	run_whole();
	CHECK(nrecs == 2U);
	expect_pic(1U, T_TRAIL_R, -56, 0, 0, 0, 0);
	CHECK(rd.counts.broken_pictures == 1U);

	/* The pic_output_flag of the header is the picture's PicOutputFlag. */
	begin();
	wpps.output_flag_present = 1U;
	put_pps();
	put_picture(T_IDR_W_RADL, 0U);
	slice_reset(1U, 0U, 1U);
	wslice.pic_output = 0U;
	put_slice(T_TRAIL_R, 0U);
	run_whole();
	CHECK(nrecs == 2U);
	CHECK(recs[0].pic_output == 1);
	CHECK(recs[1].pic_output == 0);
}

/* CRA at the start: RASL pictures are dropped, RADL and trailing pictures are returned. */
static void
check_cra_start(void)
{
	/*
	 * CRA lsb 8 (first picture: Msb 0, poc 8), RASL_N lsb 6 (dropped),
	 * RADL_R lsb 7 (poc 7), TRAIL lsb 9 (poc 9), RASL_R lsb 5 (dropped,
	 * the CRA's NoRaslOutputFlag still holds).
	 */
	begin();
	put_picture(T_CRA, 8U);
	put_picture(T_RASL_N, 6U);
	put_picture(T_RADL_R, 7U);
	put_picture(T_TRAIL_R, 9U);
	put_picture(T_RASL_R, 5U);
	run_whole();

	/* Three pictures; the CRA is first and has NoRaslOutputFlag 1, and the RASL pictures are counted. */
	CHECK(nrecs == 3U);
	expect_pic(0U, T_CRA, 8, 1, 1, 0, 0);
	expect_pic(1U, T_RADL_R, 7, 0, 0, 0, 0);
	expect_pic(2U, T_TRAIL_R, 9, 0, 0, 0, 0);
	CHECK(recs[0].irap == 1);
	CHECK(recs[0].idr == 0);
	expect_counts(0U, 2U, 0U, 0U, 0U, 0U);

	/* The indexes count the returned pictures only. */
	CHECK(recs[0].index == 0U);
	CHECK(recs[1].index == 1U);
	CHECK(recs[2].index == 2U);

	/* Both IRAP types BLA drop their RASL pictures too, even in the middle of a stream. */
	begin();
	put_picture(T_IDR_W_RADL, 0U);
	put_picture(T_TRAIL_R, 1U);
	put_picture(T_BLA_W_LP, 20U);
	put_picture(T_RASL_N, 18U);
	put_picture(T_RADL_N, 19U);
	run_whole();
	CHECK(nrecs == 4U);
	expect_pic(2U, T_BLA_W_LP, 20, 0, 1, 0, 0);
	expect_pic(3U, T_RADL_N, 19, 0, 0, 0, 0);
	expect_counts(0U, 1U, 0U, 0U, 0U, 0U);

	/* A CRA in the middle of a stream has NoRaslOutputFlag 0, so its RASL pictures are returned. */
	begin();
	put_picture(T_IDR_W_RADL, 0U);
	put_picture(T_TRAIL_R, 1U);
	put_picture(T_CRA, 8U);
	put_picture(T_RASL_N, 6U);
	put_picture(T_TRAIL_R, 9U);
	run_whole();
	CHECK(nrecs == 5U);
	expect_pic(2U, T_CRA, 8, 0, 0, 0, 0);
	expect_pic(3U, T_RASL_N, 6, 0, 0, 0, 0);
	expect_pic(4U, T_TRAIL_R, 9, 0, 0, 0, 0);
	expect_counts(0U, 0U, 0U, 0U, 0U, 0U);
	CHECK(recs[2].irap == 1);

	/*
	 * The first picture is a CRA (its RASL pictures would be dropped), a
	 * later CRA in the middle has NoRaslOutputFlag 0, so the RASL picture
	 * after it is returned.
	 */
	begin();
	put_picture(T_CRA, 8U);
	put_picture(T_TRAIL_R, 9U);
	put_picture(T_CRA, 12U);
	put_picture(T_RASL_R, 10U);
	run_whole();
	CHECK(nrecs == 4U);
	expect_pic(2U, T_CRA, 12, 0, 0, 0, 0);
	expect_pic(3U, T_RASL_R, 10, 0, 0, 0, 0);
	expect_counts(0U, 0U, 0U, 0U, 0U, 0U);
}

/* A stream that starts in the middle: the pictures before the first IRAP picture are dropped. */
static void
check_midstream_start(void)
{
	/* Three pictures (one of two segments) before the IDR, then the IDR and a trailing picture. */
	begin();
	put_picture(T_TRAIL_R, 5U);
	put_picture(T_TRAIL_N, 6U);
	put_picture(T_RASL_N, 4U);
	put_segment(T_RASL_N, 8U);
	put_picture(T_IDR_W_RADL, 0U);
	put_picture(T_TRAIL_R, 1U);
	run_whole();

	/* Three pictures counted before the IRAP picture (the RASL picture too); two are returned. */
	CHECK(nrecs == 2U);
	expect_pic(0U, T_IDR_W_RADL, 0, 1, 1, 0, 0);
	expect_pic(1U, T_TRAIL_R, 1, 0, 0, 0, 0);
	expect_counts(3U, 0U, 0U, 0U, 0U, 0U);

	/* The first picture is first: its order count state is that of the start. */
	CHECK(recs[0].index == 0U);

	/* A CRA after the dropped pictures is the first picture: Msb 0 whatever came before, RASL dropped. */
	begin();
	put_picture(T_TRAIL_R, 250U);
	put_picture(T_CRA, 3U);
	put_picture(T_RASL_N, 1U);
	put_picture(T_TRAIL_R, 4U);
	run_whole();
	CHECK(nrecs == 2U);
	expect_pic(0U, T_CRA, 3, 1, 1, 0, 0);
	expect_pic(1U, T_TRAIL_R, 4, 0, 0, 0, 0);
	expect_counts(1U, 1U, 0U, 0U, 0U, 0U);

	/* Bytes before the first start code are skipped. */
	begin();
	put_picture(T_IDR_W_RADL, 0U);
	memmove(sbuf + 7, sbuf, ws.size);
	memset(sbuf, 0xE7, 7U);
	ws.size += 7U;
	run_whole();
	CHECK(nrecs == 1U);
	expect_pic(0U, T_IDR_W_RADL, 0, 1, 1, 0, 0);
}

/* A reset: the next IRAP picture is first, a CRA is handled as a BLA picture, the order count state starts again. */
static void
check_reset(void)
{
	size_t first_size;
	int error;

	/* IDR and a picture of lsb 200 (poc 200), read; then reset and a CRA of lsb 130, RASL, RADL, TRAIL. */
	begin();
	put_picture(T_IDR_W_RADL, 0U);
	put_picture(T_TRAIL_R, 200U);
	first_size = ws.size;
	put_picture(T_CRA, 130U);
	put_picture(T_RASL_N, 128U);
	put_picture(T_RADL_N, 129U);
	put_picture(T_TRAIL_R, 131U);
	reader_start();
	h265_stream_input(&rd, sbuf, first_size);
	nrecs = 0U;
	collect(first_size);
	CHECK(nrecs == 2U);

	/* After the reset the next IRAP picture is first, with NoRaslOutputFlag 1, and its RASL pictures are dropped. */
	h265_stream_reset(&rd);
	h265_stream_input(&rd, sbuf + first_size, ws.size - first_size);
	nrecs = 0U;
	collect(ws.size - first_size);
	CHECK(nrecs == 3U);
	expect_pic(0U, T_CRA, 130, 1, 1, 0, 0);
	expect_pic(1U, T_RADL_N, 129, 0, 0, 0, 0);
	expect_pic(2U, T_TRAIL_R, 131, 0, 0, 0, 0);
	CHECK(rd.counts.rasl_skipped == 1U);

	/*
	 * Without the reset the same CRA is in the middle of the stream:
	 * NoRaslOutputFlag 0 and its order count is read against prevTid0Pic
	 * (lsb 200, Msb -256 from the picture of lsb 200): 130 is 70 below 200,
	 * within half of 256, so Msb stays -256 and the order count is -126.
	 */
	reader_start();
	h265_stream_input(&rd, sbuf, ws.size);
	nrecs = 0U;
	collect(ws.size);
	CHECK(nrecs == 6U);
	expect_pic(2U, T_CRA, -126, 0, 0, 0, 0);
	CHECK(rd.counts.rasl_skipped == 0U);

	/* A reset before any picture was read, then pictures before the IRAP picture: they are dropped. */
	begin();
	put_picture(T_TRAIL_R, 5U);
	put_picture(T_IDR_W_RADL, 0U);
	reader_start();
	h265_stream_reset(&rd);
	h265_stream_input(&rd, sbuf, ws.size);
	nrecs = 0U;
	collect(ws.size);
	CHECK(nrecs == 1U);
	CHECK(rd.counts.before_irap == 1U);
	expect_pic(0U, T_IDR_W_RADL, 0, 1, 1, 0, 0);

	/* A reset keeps the parameter sets: pictures of a buffer with no parameter sets still decode. */
	begin();
	put_picture(T_IDR_W_RADL, 0U);
	put_picture(T_TRAIL_R, 1U);
	reader_start();
	h265_stream_input(&rd, sbuf, nal_end[2]);
	error = h265_stream_next(&rd, &pic);
	CHECK(error == ENOENT);
	h265_stream_reset(&rd);
	h265_stream_input(&rd, sbuf + nal_start[3], ws.size - nal_start[3]);
	nrecs = 0U;
	collect(ws.size - nal_start[3]);
	CHECK(nrecs == 2U);
	expect_pic(0U, T_IDR_W_RADL, 0, 1, 1, 0, 0);

	/* A reset forgets a pending end of sequence: the first picture after it has after_eos 0. */
	begin();
	put_picture(T_IDR_W_RADL, 0U);
	put_plain(T_EOS);
	put_picture(T_CRA, 8U);
	reader_start();
	h265_stream_input(&rd, sbuf, nal_end[4]);
	error = h265_stream_next(&rd, &pic);
	CHECK(error == 0);
	error = h265_stream_next(&rd, &pic);
	CHECK(error == ENOENT);
	CHECK(rd.eos_pending == 1);
	h265_stream_reset(&rd);
	CHECK(rd.eos_pending == 0);
	h265_stream_input(&rd, sbuf + nal_start[5], ws.size - nal_start[5]);
	error = h265_stream_next(&rd, &pic);
	CHECK(error == 0);
	CHECK(pic.after_eos == 0);
	CHECK(pic.first == 1);
}

/* NoOutputOfPriorPicsFlag of an IRAP picture (C.5.2.2). */
static void
check_no_output_of_prior_pics(void)
{
	/* The first picture is never given the flag: an IDR with the flag 1 as the first picture has 0. */
	begin();
	slice_reset(1U, 0U, 0U);
	wslice.no_output_of_prior_pics = 1U;
	put_slice(T_IDR_W_RADL, 0U);
	run_whole();
	CHECK(nrecs == 1U);
	expect_pic(0U, T_IDR_W_RADL, 0, 1, 1, 0, 0);

	/* An IDR that is not first takes the flag of its header, 1 or 0. */
	begin();
	put_picture(T_IDR_W_RADL, 0U);
	slice_reset(1U, 0U, 0U);
	wslice.no_output_of_prior_pics = 1U;
	put_slice(T_IDR_N_LP, 0U);
	put_picture(T_IDR_W_RADL, 0U);
	run_whole();
	CHECK(nrecs == 3U);
	expect_pic(1U, T_IDR_N_LP, 0, 0, 1, 0, 1);
	expect_pic(2U, T_IDR_W_RADL, 0, 0, 1, 0, 0);

	/* A BLA picture that is not first takes the flag of its header too. */
	begin();
	put_picture(T_IDR_W_RADL, 0U);
	slice_reset(1U, 0U, 4U);
	wslice.no_output_of_prior_pics = 1U;
	put_slice(T_BLA_N_LP, 0U);
	put_picture(T_BLA_W_RADL, 5U);
	run_whole();
	CHECK(nrecs == 3U);
	expect_pic(1U, T_BLA_N_LP, 4, 0, 1, 0, 1);
	expect_pic(2U, T_BLA_W_RADL, 5, 0, 1, 0, 0);

	/* A CRA picture with NoRaslOutputFlag 1 that is not first (after an end of sequence) has 1 whatever its header says. */
	begin();
	put_picture(T_IDR_W_RADL, 0U);
	put_plain(T_EOS);
	put_picture(T_CRA, 8U);
	run_whole();
	CHECK(nrecs == 2U);
	expect_pic(1U, T_CRA, 8, 0, 1, 1, 1);

	/* A CRA picture with NoRaslOutputFlag 0 has 0 even when its header carries the flag 1. */
	begin();
	put_picture(T_IDR_W_RADL, 0U);
	slice_reset(1U, 0U, 8U);
	wslice.no_output_of_prior_pics = 1U;
	put_slice(T_CRA, 0U);
	run_whole();
	CHECK(nrecs == 2U);
	expect_pic(1U, T_CRA, 8, 0, 0, 0, 0);

	/* A picture that is not IRAP has 0. */
	begin();
	put_picture(T_IDR_W_RADL, 0U);
	put_picture(T_TRAIL_R, 1U);
	run_whole();
	CHECK(recs[1].noop == 0);
}

/* End of sequence and end of bitstream NAL units. */
static void
check_end_of_sequence(void)
{
	unsigned type;
	size_t base;
	int error;

	/* For both end NAL units: IDR, TRAIL, EOS in the TRAIL's access unit, CRA, RASL, TRAIL. */
	for (type = T_EOS; type <= T_EOB; type++) {
		begin();
		put_picture(T_IDR_W_RADL, 0U);
		put_picture(T_TRAIL_R, 1U);
		put_plain(type);
		put_picture(T_CRA, 8U);
		put_picture(T_RASL_N, 6U);
		put_picture(T_TRAIL_R, 9U);
		run_whole();

		/* The picture the end NAL unit ends has after_eos 0; the CRA after it has 1 and NoRaslOutputFlag 1. */
		CHECK(nrecs == 4U);
		expect_pic(0U, T_IDR_W_RADL, 0, 1, 1, 0, 0);
		expect_pic(1U, T_TRAIL_R, 1, 0, 0, 0, 0);
		expect_pic(2U, T_CRA, 8, 0, 1, 1, 1);
		expect_pic(3U, T_TRAIL_R, 9, 0, 0, 0, 0);
		expect_counts(0U, 1U, 0U, 0U, 0U, 0U);

		/* The end NAL unit is inside the access unit of the picture before it, and the cursor went past it. */
		CHECK(recs[1].au == nal_start[4]);
		CHECK(recs[1].au_size == nal_end[5] - nal_start[4]);
		CHECK(recs[2].au == nal_start[6]);

		/* The CRA's order count starts from Msb 0: lsb 8 is poc 8 although the picture before has poc 1. */
		CHECK(recs[2].poc == 8);
	}

	/* An end of sequence NAL unit outside an access unit (no picture was being gathered) is pending too. */
	begin();
	put_picture(T_IDR_W_RADL, 0U);
	put_plain(T_EOS);
	put_plain(T_EOS);
	put_picture(T_CRA, 8U);
	put_picture(T_TRAIL_R, 9U);
	run_whole();
	CHECK(nrecs == 3U);
	expect_pic(1U, T_CRA, 8, 0, 1, 1, 1);
	expect_pic(2U, T_TRAIL_R, 9, 0, 0, 0, 0);

	/* The end NAL unit comes after a suffix SEI of the picture: it still belongs to that picture's access unit. */
	begin();
	put_picture(T_IDR_W_RADL, 0U);
	put_plain(T_SUFFIX_SEI);
	put_plain(T_EOS);
	put_picture(T_CRA, 8U);
	run_whole();
	CHECK(nrecs == 2U);
	CHECK(recs[0].au_size == nal_end[5] - nal_start[0]);
	expect_pic(1U, T_CRA, 8, 0, 1, 1, 1);

	/*
	 * An end NAL unit that follows a NAL unit starting the next access
	 * unit (a PPS): the picture ends before the PPS, the PPS is stored
	 * after the picture is returned, and the end NAL unit is still seen.
	 */
	begin();
	put_picture(T_IDR_W_RADL, 0U);
	wpps.pps_id = 1U;
	put_pps();
	wpps.pps_id = 0U;
	put_plain(T_EOS);
	put_picture(T_CRA, 8U);
	reader_start();
	h265_stream_input(&rd, sbuf, ws.size);
	error = h265_stream_next(&rd, &pic);
	CHECK(error == 0);
	CHECK(pic.after_eos == 0);
	CHECK(rd.store.pps_present[1] == 0);
	CHECK(cursor_at(4U) != 0);
	error = h265_stream_next(&rd, &pic);
	CHECK(error == 0);
	CHECK(pic.after_eos == 1);
	CHECK(pic.no_rasl_output == 1);
	CHECK(rd.store.pps_present[1] != 0);

	/* An end of sequence at the end of a buffer, the next picture in the next buffer. */
	begin();
	put_picture(T_IDR_W_RADL, 0U);
	put_plain(T_EOS);
	base = ws.size;
	put_picture(T_CRA, 8U);
	put_picture(T_RASL_N, 7U);
	put_picture(T_TRAIL_R, 9U);
	reader_start();
	nrecs = 0U;
	h265_stream_input(&rd, sbuf, base);
	collect(base);
	CHECK(nrecs == 1U);
	h265_stream_input(&rd, sbuf + base, ws.size - base);
	collect(ws.size - base);
	CHECK(nrecs == 3U);
	expect_pic(1U, T_CRA, 8, 0, 1, 1, 1);
	expect_pic(2U, T_TRAIL_R, 9, 0, 0, 0, 0);
	CHECK(rd.counts.rasl_skipped == 1U);

	/* A dropped RASL picture whose access unit ends with an end of sequence leaves it pending. */
	begin();
	put_picture(T_IDR_W_RADL, 0U);
	put_picture(T_RASL_N, 3U);
	put_plain(T_EOS);
	put_picture(T_CRA, 8U);
	put_picture(T_RASL_N, 7U);
	run_whole();
	CHECK(nrecs == 2U);
	expect_pic(1U, T_CRA, 8, 0, 1, 1, 1);
	CHECK(rd.counts.rasl_skipped == 2U);

	/* An end of bitstream after a picture before the first IRAP picture: the picture is dropped, the IRAP is first. */
	begin();
	put_picture(T_TRAIL_R, 3U);
	put_plain(T_EOB);
	put_picture(T_IDR_W_RADL, 0U);
	run_whole();
	CHECK(nrecs == 1U);
	expect_pic(0U, T_IDR_W_RADL, 0, 1, 1, 1, 0);

	/* A non-IRAP picture right after an end of sequence is returned with after_eos 1 and NoRaslOutputFlag 0. */
	begin();
	put_picture(T_IDR_W_RADL, 0U);
	put_plain(T_EOS);
	put_picture(T_TRAIL_R, 5U);
	put_picture(T_TRAIL_R, 6U);
	run_whole();
	CHECK(nrecs == 3U);
	expect_pic(1U, T_TRAIL_R, 5, 0, 0, 1, 0);
	expect_pic(2U, T_TRAIL_R, 6, 0, 0, 0, 0);
}

/* After an end of sequence the active SPS may change at the CRA picture (NoRaslOutputFlag 1). */
static void
check_end_of_sequence_activation(void)
{
	/* IDR (SPS 0), EOS, then SPS 0 with another size and a CRA: the new content is activated. */
	begin();
	put_picture(T_IDR_W_RADL, 0U);
	put_plain(T_EOS);
	wsps.width = 128U;
	put_sps();
	wsps.width = 64U;
	put_picture(T_CRA, 8U);
	put_picture(T_TRAIL_R, 9U);
	run_whole();
	CHECK(nrecs == 3U);
	expect_counts(0U, 0U, 0U, 0U, 0U, 0U);
	CHECK(rd.store.sps[0].width == 128U);
}

/* The access unit boundaries (7.4.2.4.4). */
static void
check_access_units(void)
{
	unsigned type;
	unsigned seg2;
	int error;

	/*
	 * Between two segments of one picture: a prefix SEI, a PPS of another id
	 * and an SPS resend belong to the access unit; the picture is not cut.
	 */
	begin();
	put_picture(T_IDR_W_RADL, 0U);
	put_plain(T_PREFIX_SEI);
	wpps.pps_id = 1U;
	put_pps();
	wpps.pps_id = 0U;
	put_sps();
	seg2 = put_segment(T_IDR_W_RADL, 8U);
	put_picture(T_TRAIL_R, 1U);
	reader_start();
	h265_stream_input(&rd, sbuf, ws.size);
	nrecs = 0U;
	collect(ws.size);
	CHECK(nrecs == 2U);
	CHECK(recs[0].slice_count == 2U);
	CHECK(recs[0].au == nal_start[0]);
	CHECK(recs[0].au_size == nal_end[seg2] - nal_start[0]);
	CHECK(recs[0].offset_last == nal_start[seg2] + 4U);
	CHECK(recs[0].size_last == nal_end[seg2] - nal_start[seg2] - 4U);
	expect_counts(0U, 0U, 0U, 0U, 0U, 0U);

	/* The PPS between the segments is in the store, and the generation is the value after it (4 sets). */
	CHECK(rd.store.pps_present[1] != 0);
	CHECK(recs[0].generation == 4U);
	CHECK(recs[1].generation == 4U);

	/*
	 * Between two pictures: AUD, VPS, SPS, PPS and prefix SEI; the first
	 * picture is returned with the store as it was, the cursor stops at the
	 * first of those NAL units, and the next picture sees the changes.
	 */
	begin();
	put_picture(T_IDR_W_RADL, 0U);
	put_plain(T_AUD);
	put_vps();
	wsps.log2_max_poc_lsb_minus4 = 3U;
	put_sps();
	wsps.log2_max_poc_lsb_minus4 = 4U;
	wpps.pps_id = 1U;
	put_pps();
	wpps.pps_id = 0U;
	put_plain(T_PREFIX_SEI);
	put_picture(T_TRAIL_R, 1U);
	reader_start();
	h265_stream_input(&rd, sbuf, ws.size);
	error = h265_stream_next(&rd, &pic);
	CHECK(error == 0);
	CHECK(pic.generation == 3U);
	CHECK(pic.access_unit_size == nal_end[3] - nal_start[0]);
	CHECK(cursor_at(4U) != 0);
	CHECK(rd.store.generation == 3U);
	CHECK(rd.store.sps_generation[0] == 1U);
	CHECK(rd.store.pps_present[1] == 0);
	error = h265_stream_next(&rd, &pic);

	/* The second picture sees the new SPS (a changed SPS under the same id needs an IRAP picture, so it is broken). */
	CHECK(error == ENOENT);
	CHECK(rd.counts.broken_pictures == 1U);
	CHECK(rd.store.generation >= 5U);
	CHECK(rd.store.pps_present[1] != 0);

	/* A suffix SEI belongs to the picture before it. */
	begin();
	put_picture(T_IDR_W_RADL, 0U);
	put_plain(T_SUFFIX_SEI);
	put_picture(T_TRAIL_R, 1U);
	run_whole();
	CHECK(nrecs == 2U);
	CHECK(recs[0].au_size == nal_end[4] - nal_start[0]);
	CHECK(recs[1].au == nal_start[5]);

	/* Every non-VCL type between two pictures: whether it starts an access unit decides where the first picture's unit ends. */
	for (type = 32U; type < 64U; type++) {
		if (type == T_EOS || type == T_EOB)
			continue;
		begin();
		put_picture(T_IDR_W_RADL, 0U);
		if (type == 32U) {
			put_vps();
		} else if (type == 33U) {
			put_sps();
		} else if (type == 34U) {
			put_pps();
		} else {
			put_plain(type);
		}
		put_picture(T_TRAIL_R, 1U);
		reader_start();
		h265_stream_input(&rd, sbuf, ws.size);
		error = h265_stream_next(&rd, &pic);
		CHECK(error == 0);
		if (type_starts_au(type) != 0) {
			CHECK(pic.access_unit_size == nal_end[3] - nal_start[0]);
			CHECK(cursor_at(4U) != 0);
		} else {
			CHECK(pic.access_unit_size == nal_end[4] - nal_start[0]);
			CHECK(cursor_at(5U) != 0);
		}
		error = h265_stream_next(&rd, &pic);
		CHECK(error == 0);
		CHECK(pic.slice_count == 1U);
		if (type_starts_au(type) != 0) {
			CHECK(pic.access_unit == nal_start[4]);
		} else {
			CHECK(pic.access_unit == nal_start[5]);
		}
		error = h265_stream_next(&rd, &pic);
		CHECK(error == ENOENT);
		expect_counts(0U, 0U, 0U, 0U, 0U, 0U);
	}

	/* Every non-VCL type between two segments of one picture: the picture is not cut and the unit holds it all. */
	for (type = 32U; type < 64U; type++) {
		if (type == T_EOS || type == T_EOB)
			continue;
		begin();
		put_picture(T_IDR_W_RADL, 0U);
		if (type == 32U) {
			put_vps();
		} else if (type == 33U) {
			put_sps();
		} else if (type == 34U) {
			put_pps();
		} else {
			put_plain(type);
		}
		seg2 = put_segment(T_IDR_W_RADL, 8U);
		put_picture(T_TRAIL_R, 1U);
		run_whole();
		CHECK(nrecs == 2U);
		CHECK(recs[0].slice_count == 2U);
		CHECK(recs[0].au_size == nal_end[seg2] - nal_start[0]);
		CHECK(recs[1].slice_count == 1U);
		expect_counts(0U, 0U, 0U, 0U, 0U, 0U);
	}
}

/* NAL units of other layers and reserved types. */
static void
check_skipped_units(void)
{
	static const uint8_t payload[2] = { 0x11U, 0x80U };
	unsigned type;
	unsigned count;
	unsigned segment;

	/* NAL units of layer 1 of every kind between and inside pictures are counted and change nothing. */
	begin();
	put_picture(T_IDR_W_RADL, 0U);
	put_nal(33U, 1U, 1U, payload, sizeof(payload));
	put_nal(T_EOS, 1U, 1U, payload, sizeof(payload));
	put_nal(T_TRAIL_R, 1U, 1U, payload, sizeof(payload));
	put_nal(T_IDR_W_RADL, 1U, 1U, payload, sizeof(payload));
	put_picture(T_TRAIL_R, 1U);
	put_nal(T_PREFIX_SEI, 1U, 1U, payload, sizeof(payload));
	put_nal(T_CRA, 63U, 1U, payload, sizeof(payload));
	put_picture(T_TRAIL_R, 2U);
	run_whole();
	CHECK(nrecs == 3U);
	expect_pic(1U, T_TRAIL_R, 1, 0, 0, 0, 0);
	expect_pic(2U, T_TRAIL_R, 2, 0, 0, 0, 0);
	expect_counts(0U, 0U, 0U, 6U, 0U, 0U);

	/* An end of sequence of layer 1 does not end the base layer's sequence. */
	CHECK(recs[1].after_eos == 0);

	/* Reserved VCL types 10..15 and 22..31: counted once each, between pictures and between segments, nothing else changes. */
	count = 0U;
	for (type = 10U; type <= 31U; type++) {
		if (type > 15U && type < 22U)
			continue;
		for (segment = 0U; segment < 2U; segment++) {
			begin();
			put_picture(T_IDR_W_RADL, 0U);
			put_nal(type, 0U, 1U, payload, sizeof(payload));
			if (segment != 0U)
				put_segment(T_IDR_W_RADL, 8U);
			put_picture(T_TRAIL_R, 1U);
			run_whole();
			CHECK(nrecs == 2U);
			CHECK(recs[0].slice_count == 1U + segment);
			expect_counts(0U, 0U, 1U, 0U, 0U, 0U);
			count++;
		}
	}
	CHECK(count == 32U);

	/* A reserved VCL type before the first IRAP picture is not a picture to count as before_irap. */
	begin();
	put_nal(10U, 0U, 1U, payload, sizeof(payload));
	put_picture(T_IDR_W_RADL, 0U);
	run_whole();
	CHECK(nrecs == 1U);
	expect_counts(0U, 0U, 1U, 0U, 0U, 0U);
}

/* Parameter sets: storing, generations, refusals. */
static void
check_parameter_sets(void)
{
	static uint8_t big[70000];
	unsigned generation;
	int error;

	/* A resend of the same sets keeps the generation; a different SPS content raises it by one. */
	begin();
	put_picture(T_IDR_W_RADL, 0U);
	put_vps();
	put_sps();
	put_pps();
	put_picture(T_IDR_W_RADL, 0U);
	run_whole();
	CHECK(nrecs == 2U);
	CHECK(recs[0].generation == 3U);
	CHECK(recs[1].generation == 3U);

	/* A different SPS and PPS content between two IDR pictures raises the generation by two. */
	begin();
	put_picture(T_IDR_W_RADL, 0U);
	wsps.max_dec_pic_buffering_minus1[0] = 3U;
	put_sps();
	wpps.init_qp_minus26 = 2;
	put_pps();
	put_picture(T_IDR_W_RADL, 0U);
	run_whole();
	CHECK(nrecs == 2U);
	CHECK(recs[0].generation == 3U);
	CHECK(recs[1].generation == 5U);
	CHECK(rd.store.sps_generation[0] == 2U);

	/* The VPS may be missing: a picture decodes without it. */
	h265w_stream_init(&ws, sbuf, sizeof(sbuf));
	nal_count = 0U;
	sets_default();
	put_sps();
	put_pps();
	put_picture(T_IDR_W_RADL, 0U);
	run_whole();
	CHECK(nrecs == 1U);
	CHECK(rd.store.vps_present[0] == 0);
	expect_counts(0U, 0U, 0U, 0U, 0U, 0U);

	/* A picture's vps_id is that of its SPS. */
	begin();
	wvps.vps_id = 3U;
	wsps.vps_id = 3U;
	nal_count = 0U;
	h265w_stream_init(&ws, sbuf, sizeof(sbuf));
	put_vps();
	put_sps();
	put_pps();
	put_picture(T_IDR_W_RADL, 0U);
	run_whole();
	CHECK(nrecs == 1U);
	CHECK(recs[0].vps_id == 3U);

	/* A picture naming SPS 5 through PPS 9 reports both ids. */
	h265w_stream_init(&ws, sbuf, sizeof(sbuf));
	nal_count = 0U;
	sets_default();
	wsps.sps_id = 5U;
	wpps.sps_id = 5U;
	wpps.pps_id = 9U;
	put_vps();
	put_sps();
	put_pps();
	slice_reset(1U, 0U, 0U);
	put_slice(T_IDR_W_RADL, 0U);
	run_whole();
	CHECK(nrecs == 1U);
	CHECK(recs[0].sps_id == 5U);
	CHECK(recs[0].pps_id == 9U);

	/* h265_stream_parameters stores the sets up to the first VCL NAL unit and not the ones after it. */
	begin();
	put_picture(T_IDR_W_RADL, 0U);
	wpps.pps_id = 2U;
	put_pps();
	wpps.pps_id = 0U;
	reader_start();
	generation = rd.store.generation;
	error = h265_stream_parameters(&rd, sbuf, ws.size);
	CHECK(error == 0);
	CHECK(rd.store.vps_present[0] != 0);
	CHECK(rd.store.sps_present[0] != 0);
	CHECK(rd.store.pps_present[0] != 0);
	CHECK(rd.store.pps_present[2] == 0);
	CHECK(rd.store.generation == generation + 3U);
	CHECK(rd.data == NULL);
	CHECK(rd.cursor == 0U);

	/* A set that does not read fails the call, counted and stored around: the others are stored. */
	h265w_stream_init(&ws, sbuf, sizeof(sbuf));
	nal_count = 0U;
	sets_default();
	put_vps();
	put_nal(33U, 0U, 1U, big, 3U);
	put_pps();
	reader_start();
	error = h265_stream_parameters(&rd, sbuf, ws.size);
	CHECK(error == EINVAL);
	CHECK(rd.store.vps_present[0] != 0);
	CHECK(rd.store.sps_present[0] == 0);
	CHECK(rd.store.pps_present[0] != 0);
	CHECK(rd.counts.refused_sets == 1U);
	CHECK(rd.reason != NULL);

	/* A parameter set longer than the RBSP copy is refused and counted, and the reader goes on. */
	begin();
	memset(big, 0x55, sizeof(big));
	put_nal(34U, 0U, 1U, big, sizeof(big));
	put_picture(T_IDR_W_RADL, 0U);
	run_whole();
	CHECK(nrecs == 1U);
	CHECK(rd.counts.refused_sets == 1U);

	/* A set whose NAL unit header does not read (temporal id 0) is counted as refused, and so is it by h265_stream_parameters. */
	begin();
	put_nal(34U, 0U, 0U, big, 4U);
	put_picture(T_IDR_W_RADL, 0U);
	run_whole();
	CHECK(nrecs == 1U);
	CHECK(rd.counts.refused_sets == 1U);
	reader_start();
	error = h265_stream_parameters(&rd, sbuf, ws.size);
	CHECK(error == EINVAL);
	CHECK(rd.counts.refused_sets == 1U);
	CHECK(rd.store.sps_present[0] != 0);

	/* h265_stream_parameters on an empty buffer and with other layers. */
	reader_start();
	error = h265_stream_parameters(&rd, NULL, 0U);
	CHECK(error == 0);
	error = h265_stream_parameters(&rd, sbuf, 0U);
	CHECK(error == 0);
	h265w_stream_init(&ws, sbuf, sizeof(sbuf));
	nal_count = 0U;
	sets_default();
	put_nal(33U, 1U, 1U, big, 3U);
	put_vps();
	reader_start();
	error = h265_stream_parameters(&rd, sbuf, ws.size);
	CHECK(error == 0);
	CHECK(rd.counts.other_layer == 1U);
	CHECK(rd.store.vps_present[0] != 0);
}

/* A PPS or SPS outside the range of the parse is refused, and a picture that names it is not broken. */
static void
check_refused_sets(void)
{
	/* A PPS with the screen content extension flag: refused as outside the range. */
	begin();
	wpps.pps_id = 1U;
	wpps.extension_present = 1U;
	wpps.scc_extension_flag = 1U;
	put_pps();
	wpps.pps_id = 0U;
	wpps.extension_present = 0U;
	wpps.scc_extension_flag = 0U;
	put_picture(T_IDR_W_RADL, 0U);
	slice_reset(1U, 0U, 1U);
	wslice.pps_id = 1U;
	put_slice(T_TRAIL_R, 0U);
	put_picture(T_TRAIL_R, 2U);
	run_whole();

	/* The picture naming PPS 1 is left behind as outside the range (counted with the refused sets), not as broken. */
	CHECK(nrecs == 2U);
	CHECK(rd.counts.broken_pictures == 0U);
	CHECK(rd.counts.refused_sets == 2U);
	CHECK(rd.store.pps_refused[1] != 0);
	expect_pic(1U, T_TRAIL_R, 2, 0, 0, 0, 0);
}

/* The active SPS (7.4.3.2.1). */
static void
check_sps_activation(void)
{
	/* A second SPS (id 1, another size) and its PPS (id 1), alongside SPS 0 and PPS 0. */
	begin();
	wsps.sps_id = 1U;
	wsps.width = 128U;
	put_sps();
	wsps.sps_id = 0U;
	wsps.width = 64U;
	wpps.pps_id = 1U;
	wpps.sps_id = 1U;
	put_pps();
	wpps.pps_id = 0U;
	wpps.sps_id = 0U;

	/* IDR on SPS 0; a TRAIL on PPS 1 (SPS 1) in the same sequence is broken; a TRAIL on PPS 0 is fine. */
	put_picture(T_IDR_W_RADL, 0U);
	slice_reset(1U, 0U, 1U);
	wslice.pps_id = 1U;
	put_slice(T_TRAIL_R, 0U);
	put_picture(T_TRAIL_R, 2U);

	/* An IDR on PPS 1 activates SPS 1; then a TRAIL on PPS 0 is broken and a TRAIL on PPS 1 is fine. */
	slice_reset(1U, 0U, 0U);
	wslice.pps_id = 1U;
	put_slice(T_IDR_W_RADL, 0U);
	put_picture(T_TRAIL_R, 3U);
	slice_reset(1U, 0U, 4U);
	wslice.pps_id = 1U;
	put_slice(T_TRAIL_R, 0U);
	run_whole();
	CHECK(nrecs == 4U);
	expect_pic(0U, T_IDR_W_RADL, 0, 1, 1, 0, 0);
	expect_pic(1U, T_TRAIL_R, 2, 0, 0, 0, 0);
	expect_pic(2U, T_IDR_W_RADL, 0, 0, 1, 0, 0);
	expect_pic(3U, T_TRAIL_R, 4, 0, 0, 0, 0);
	CHECK(recs[0].sps_id == 0U);
	CHECK(recs[2].sps_id == 1U);
	CHECK(recs[3].sps_id == 1U);
	CHECK(rd.counts.broken_pictures == 2U);
	CHECK(rd.reason != NULL);
	CHECK(rd.active_sps == 1U);
}

/* An SPS with the same id and other content inside a coded video sequence breaks the picture; the next IRAP picture recovers. */
static void
check_sps_change(void)
{
	/* IDR, SPS 0 resent with the same content (fine), SPS 0 with a new size, TRAIL (broken), IDR (new SPS), TRAIL. */
	begin();
	put_picture(T_IDR_W_RADL, 0U);
	put_sps();
	put_picture(T_TRAIL_R, 1U);
	wsps.height = 128U;
	put_sps();
	put_picture(T_TRAIL_R, 2U);
	put_picture(T_IDR_W_RADL, 0U);
	put_picture(T_TRAIL_R, 1U);
	run_whole();
	CHECK(nrecs == 4U);
	expect_pic(0U, T_IDR_W_RADL, 0, 1, 1, 0, 0);
	expect_pic(1U, T_TRAIL_R, 1, 0, 0, 0, 0);
	expect_pic(2U, T_IDR_W_RADL, 0, 0, 1, 0, 0);
	expect_pic(3U, T_TRAIL_R, 1, 0, 0, 0, 0);
	CHECK(rd.counts.broken_pictures == 1U);
	CHECK(recs[2].generation == recs[3].generation);
	CHECK(recs[2].generation > recs[1].generation);

	/* A CRA in the middle of a sequence (NoRaslOutputFlag 0) with a changed SPS content is broken like any other picture. */
	begin();
	put_picture(T_IDR_W_RADL, 0U);
	wsps.height = 128U;
	put_sps();
	put_picture(T_CRA, 8U);
	run_whole();
	CHECK(nrecs == 1U);
	CHECK(rd.counts.broken_pictures == 1U);

	/* A changed SPS between two segments of one picture breaks the picture. */
	begin();
	put_picture(T_IDR_W_RADL, 0U);
	put_picture(T_TRAIL_R, 1U);
	wsps.sub_layer_ordering_info_present = 1U;
	wsps.max_dec_pic_buffering_minus1[0] = 3U;
	put_sps();
	put_segment(T_TRAIL_R, 8U);
	put_picture(T_TRAIL_R, 2U);
	run_whole();
	CHECK(rd.counts.broken_pictures == 2U);
}

/* Slice segments of a picture: counts, dependent segments, mismatches. */
static void
check_slice_segments(void)
{
	unsigned index;
	unsigned count;

	/* Up to 600 segments return in one picture; 601 break it. */
	for (count = 599U; count <= 601U; count++) {
		h265w_stream_init(&ws, sbuf, sizeof(sbuf));
		nal_count = 0U;
		sets_default();
		wsps.width = 640U;
		wsps.height = 640U;
		put_vps();
		put_sps();
		put_pps();
		put_picture(T_IDR_W_RADL, 0U);
		put_picture(T_TRAIL_R, 1U);
		for (index = 1U; index < count; index++)
			put_segment(T_TRAIL_R, index);
		put_picture(T_TRAIL_R, 2U);
		run_whole();
		if (count <= 600U) {
			CHECK(nrecs == 3U);
			CHECK(recs[1].slice_count == count);
			CHECK(rd.counts.broken_pictures == 0U);
			expect_pic(2U, T_TRAIL_R, 2, 0, 0, 0, 0);
		} else {
			CHECK(nrecs == 2U);
			CHECK(rd.counts.broken_pictures == 1U);
			expect_pic(1U, T_TRAIL_R, 2, 0, 0, 0, 0);
		}
	}

	/* The segments' bytes are those of their NAL units, in order. */
	begin();
	put_picture(T_IDR_W_RADL, 0U);
	put_segment(T_IDR_W_RADL, 4U);
	put_segment(T_IDR_W_RADL, 8U);
	run_whole();
	CHECK(nrecs == 1U);
	CHECK(recs[0].slice_count == 3U);
	CHECK(recs[0].offset_last == nal_start[5] + 4U);
	CHECK(recs[0].size_last == nal_end[5] - nal_start[5] - 4U);
	CHECK(recs[0].au_size == nal_end[5] - nal_start[0]);

	/* Dependent segments: the independent header is the last independent one, and the picture keeps the first's header. */
	begin();
	wpps.dependent_slices = 1U;
	put_pps();
	slice_reset(1U, 0U, 0U);
	wslice.slice_qp_delta = 3;
	put_slice(T_IDR_W_RADL, 0U);
	slice_reset(0U, 4U, 0U);
	wslice.dependent = 1U;
	put_slice(T_IDR_W_RADL, 0U);
	slice_reset(0U, 8U, 0U);
	wslice.slice_qp_delta = -5;
	put_slice(T_IDR_W_RADL, 0U);
	slice_reset(0U, 12U, 0U);
	wslice.dependent = 1U;
	put_slice(T_IDR_W_RADL, 0U);
	reader_start();
	h265_stream_input(&rd, sbuf, ws.size);
	nrecs = 0U;
	collect(ws.size);
	CHECK(nrecs == 1U);
	CHECK(recs[0].slice_count == 4U);
	CHECK(pic.header.qp == 26 + 3);
	CHECK(rd.independent.qp == 26 - 5);
	CHECK(pic.header.dependent == 0);

	/* A segment that names another PPS than the first breaks the picture. */
	begin();
	wpps.pps_id = 1U;
	put_pps();
	wpps.pps_id = 0U;
	put_picture(T_IDR_W_RADL, 0U);
	put_picture(T_TRAIL_R, 1U);
	slice_reset(0U, 8U, 0U);
	wslice.pps_id = 1U;
	put_slice(T_TRAIL_R, 0U);
	put_picture(T_TRAIL_R, 2U);
	run_whole();
	CHECK(nrecs == 2U);
	CHECK(rd.counts.broken_pictures == 1U);
	expect_pic(1U, T_TRAIL_R, 2, 0, 0, 0, 0);

	/* A segment with another nal_unit_type breaks the picture. */
	begin();
	put_picture(T_IDR_W_RADL, 0U);
	put_picture(T_TRAIL_R, 1U);
	put_segment(T_TRAIL_N, 8U);
	put_picture(T_TRAIL_R, 2U);
	run_whole();
	CHECK(nrecs == 2U);
	CHECK(rd.counts.broken_pictures == 1U);
	expect_pic(0U, T_IDR_W_RADL, 0, 1, 1, 0, 0);
	expect_pic(1U, T_TRAIL_R, 2, 0, 0, 0, 0);

	/* Segments with first_slice_segment_in_pic_flag 0 and no picture before them are skipped as one broken picture. */
	begin();
	put_segment(T_IDR_W_RADL, 4U);
	put_segment(T_IDR_W_RADL, 8U);
	put_picture(T_IDR_W_RADL, 0U);
	put_picture(T_TRAIL_R, 1U);
	run_whole();
	CHECK(nrecs == 2U);
	CHECK(rd.counts.broken_pictures == 1U);
	CHECK(rd.counts.before_irap == 0U);
	expect_pic(0U, T_IDR_W_RADL, 0, 1, 1, 0, 0);

	/* After an end of sequence the same skip leaves the end pending for the next picture. */
	begin();
	put_picture(T_IDR_W_RADL, 0U);
	put_plain(T_EOS);
	put_segment(T_IDR_W_RADL, 4U);
	put_picture(T_CRA, 8U);
	run_whole();
	CHECK(nrecs == 2U);
	CHECK(rd.counts.broken_pictures == 1U);
	expect_pic(1U, T_CRA, 8, 0, 1, 1, 1);

	/* A segment header that does not read (cut off after the PPS id) breaks the picture. */
	begin();
	put_picture(T_IDR_W_RADL, 0U);
	{
		static const uint8_t cut[2] = { 0xC0U, 0x80U };

		put_nal(T_TRAIL_R, 0U, 1U, cut, sizeof(cut));
	}
	put_picture(T_TRAIL_R, 2U);
	run_whole();
	CHECK(nrecs == 2U);
	CHECK(rd.counts.broken_pictures == 1U);
	expect_pic(1U, T_TRAIL_R, 2, 0, 0, 0, 0);

	/* A segment naming a missing PPS breaks the picture; a segment of a missing SPS too. */
	begin();
	put_picture(T_IDR_W_RADL, 0U);
	slice_reset(1U, 0U, 1U);
	wslice.pps_id = 7U;
	put_slice(T_TRAIL_R, 0U);
	put_picture(T_TRAIL_R, 2U);
	run_whole();
	CHECK(nrecs == 2U);
	CHECK(rd.counts.broken_pictures == 1U);

	/* A slice NAL unit whose header is damaged (forbidden_zero_bit 1, or nuh_temporal_id_plus1 0) spoils the picture it is in. */
	for (index = 0U; index < 2U; index++) {
		static const uint8_t damaged[4] = { 0x11U, 0x22U, 0x33U, 0x80U };
		unsigned damaged_nal;

		begin();
		put_picture(T_IDR_W_RADL, 0U);
		put_picture(T_TRAIL_R, 1U);
		damaged_nal = put_nal(T_TRAIL_R, 0U, (index == 0U) ? 0U : 1U, damaged, sizeof(damaged));
		if (index == 1U)
			sbuf[nal_start[damaged_nal] + 4U] |= 0x80U;
		put_picture(T_TRAIL_R, 2U);
		run_whole();
		CHECK(nrecs == 2U);
		CHECK(rd.counts.broken_pictures == 1U);
		expect_pic(1U, T_TRAIL_R, 2, 0, 0, 0, 0);
	}
}

/* The longest slice segment headers (D12): the second copy. */
static uint8_t long_rbsp[300000];
static size_t long_bits;

/* Appends one bit to the long header. */
static void
long_bit(
	unsigned bit)
{
	/* The bit lands at the next position of the zeroed buffer. */
	if (bit != 0U)
		long_rbsp[long_bits >> 3] |= (uint8_t)(0x80U >> (long_bits & 7U));
	long_bits++;
}

/* Appends the count low bits of value, most significant first. */
static void
long_put(
	uint32_t value,
	unsigned count)
{
	unsigned i;

	/* Each bit in turn. */
	for (i = count; i > 0U; i--)
		long_bit((value >> (i - 1U)) & 1U);
}

/* Appends an Exp-Golomb code ue(v). */
static void
long_ue(
	uint32_t value)
{
	uint64_t code;
	unsigned length;
	unsigned i;

	/* codeNum + 1 in 2 * length + 1 bits, length being its bits after the top one. */
	code = (uint64_t)value + 1U;
	length = 0U;
	while ((code >> (length + 1U)) != 0U)
		length++;
	for (i = 0U; i < length; i++)
		long_bit(0U);
	for (i = length + 1U; i > 0U; i--)
		long_bit((unsigned)((code >> (i - 1U)) & 1U));
}

/*
 * Builds the RBSP of a first segment of a picture whose header carries
 * count entry points of value offset (offset_len 32 bits), after the
 * header of an IDR picture of wsps and wpps, then slice_data bytes of
 * slice data.  The header before the entry points is written by the syntax
 * writer with no entry point (its length is found from the last set bit);
 * the entries are appended here, since the writer's buffer holds 64 KiB.
 */
static size_t
long_build(
	unsigned count,
	uint32_t offset,
	unsigned slice_data)
{
	size_t prefix;
	size_t last_one;
	size_t bit;
	unsigned i;

	/* The header with no entry point: its bits end with ue(0) = 1 and the alignment bit 1, then zeros. */
	h265w_slice_default(&wslice);
	wslice.pps_id = wpps.pps_id;
	wslice.num_entry_point_offsets = 0U;
	h265w_slice_context_from(&wctx, &wsps, &wpps, T_IDR_W_RADL);
	bitwriter_init(&bw);
	h265w_slice(&bw, &wslice, &wctx);
	last_one = 0U;
	for (bit = 0U; bit < bw.bits; bit++) {
		if ((bw.data[bit >> 3] & (0x80U >> (bit & 7U))) != 0U)
			last_one = bit;
	}
	prefix = last_one - 1U;

	/* Copies the bits before ue(0), then writes the entry points and byte_alignment(). */
	memset(long_rbsp, 0, sizeof(long_rbsp));
	long_bits = 0U;
	for (bit = 0U; bit < prefix; bit++)
		long_bit((bw.data[bit >> 3] >> (7U - (bit & 7U))) & 1U);
	long_ue(count);
	if (count > 0U) {
		long_ue(31U);
		for (i = 0U; i < count; i++)
			long_put(offset, 32U);
	}
	long_bit(1U);
	while ((long_bits & 7U) != 0U)
		long_bit(0U);

	/* Slice data after the header: bytes that are not zero. */
	for (i = 0U; i < slice_data; i++)
		long_rbsp[long_bits / 8U + i] = (uint8_t)(0x40U + (i & 0x3FU));
	return long_bits / 8U + slice_data;
}

/* Slice segment headers longer than the first RBSP copy are read again from the second copy. */
static void
check_long_headers(void)
{
	static const uint32_t offsets[3] = { 0x01020304U, 0x00000100U, 0x7F7F7F7FU };
	size_t rbsp_size;
	unsigned count;
	unsigned variant;
	unsigned long_count;
	uint8_t *saved;

	/* A picture of 20 tile columns (320 wide) and 850 CTB rows (13 600 high) with tiles and wavefronts both on. */
	h265w_stream_init(&ws, sbuf, sizeof(sbuf));
	nal_count = 0U;
	sets_default();
	wsps.width = 320U;
	wsps.height = 13600U;
	wpps.tiles = 1U;
	wpps.entropy_sync = 1U;
	wpps.num_tile_columns_minus1 = 19U;
	wpps.num_tile_rows_minus1 = 0U;
	wpps.uniform_spacing = 1U;
	put_vps();
	put_sps();
	put_pps();

	/* The header of 16 999 entry points of 32 bits is above 64 KiB; the picture reads, from the second copy. */
	for (variant = 0U; variant < 3U; variant++) {
		reader_start();
		ws.size = nal_end[2];
		nal_count = 3U;
		rbsp_size = long_build(16999U, offsets[variant], 40U);
		put_nal(T_IDR_W_RADL, 0U, 1U, long_rbsp, rbsp_size);
		h265_stream_input(&rd, sbuf, ws.size);
		nrecs = 0U;
		collect(ws.size);
		CHECK(rd.slice_rbsp != NULL);
		CHECK(rd.slice_rbsp_capacity == H265_STREAM_SLICE_BYTES);
		CHECK(rd.counts.broken_pictures == 0U);
		CHECK(nrecs == 1U);
		if (nrecs == 1U) {
			CHECK(recs[0].slice_count == 1U);
			CHECK(pic.header.num_entry_points == 16999U);
			CHECK(pic.header.offset_len == 32U);
		}
	}

	/* The long copy is made once: the pointer is the same for the next long header. */
	reader_start();
	nrecs = 0U;
	ws.size = nal_end[2];
	nal_count = 3U;
	rbsp_size = long_build(16999U, 0x01020304U, 40U);
	put_nal(T_IDR_W_RADL, 0U, 1U, long_rbsp, rbsp_size);
	put_nal(T_IDR_W_RADL, 0U, 1U, long_rbsp, rbsp_size);
	h265_stream_input(&rd, sbuf, ws.size);
	collect(ws.size);
	CHECK(nrecs == 2U);
	CHECK(recs[0].slice_count == 1U);
	CHECK(rd.slice_rbsp != NULL);
	CHECK(pic.header.num_entry_points == 16999U);
	CHECK(pic.header.data_offset > H265_STREAM_RBSP_BYTES);
	saved = rd.slice_rbsp;
	h265_stream_input(&rd, sbuf, ws.size);
	collect(ws.size);
	CHECK(rd.slice_rbsp == saved);

	/*
	 * Entry point counts around the size of the first copy: whether the
	 * header just fits or just runs over, the header reads, with and without
	 * slice data after it.  The copy holds 65 536 payload bytes.
	 */
	long_count = 0U;
	for (count = 16300U; count <= 16400U; count++) {
		reader_start();
		nrecs = 0U;
		ws.size = nal_end[2];
		nal_count = 3U;
		rbsp_size = long_build(count, 0x01020304U, (count & 1U) != 0U ? 0U : 100U);
		put_nal(T_IDR_W_RADL, 0U, 1U, long_rbsp, rbsp_size);
		h265_stream_input(&rd, sbuf, ws.size);
		collect(ws.size);
		CHECK(nrecs == 1U);
		if (nrecs == 1U) {
			CHECK(pic.header.num_entry_points == count);
			if (rd.slice_rbsp != NULL)
				long_count++;
		}
	}

	/* Some of those headers needed the second copy, and some did not. */
	CHECK(long_count > 0U);
	CHECK(long_count < 101U);

	/* A header of 16 000 entry points fits the first copy: no second copy is allocated. */
	reader_start();
	nrecs = 0U;
	ws.size = nal_end[2];
	nal_count = 3U;
	rbsp_size = long_build(16000U, 0x01020304U, 5000U);
	put_nal(T_IDR_W_RADL, 0U, 1U, long_rbsp, rbsp_size);
	h265_stream_input(&rd, sbuf, ws.size);
	collect(ws.size);
	CHECK(nrecs == 1U);
	CHECK(rd.slice_rbsp == NULL);
	CHECK(pic.header.num_entry_points == 16000U);
	CHECK(pic.slice_size[0] > 64000U);

	/* A header with more entry points than the picture allows (17 000) is broken after the second copy. */
	reader_start();
	nrecs = 0U;
	ws.size = nal_end[2];
	nal_count = 3U;
	rbsp_size = long_build(17000U, 0x01020304U, 40U);
	put_nal(T_IDR_W_RADL, 0U, 1U, long_rbsp, rbsp_size);
	h265_stream_input(&rd, sbuf, ws.size);
	collect(ws.size);
	CHECK(nrecs == 0U);
	CHECK(rd.counts.broken_pictures == 1U);
	CHECK(rd.slice_rbsp != NULL);

	/* Destroy frees the long copy and the reader may start again. */
	h265_stream_destroy(&rd);
	CHECK(rd.slice_rbsp == NULL);
	CHECK(rd.slice_rbsp_capacity == 0U);
	h265_stream_init(&rd);
	CHECK(rd.slice_rbsp == NULL);
}

/* The summaries of two runs of the same stream must be the same picture for picture. */
static int
same_recs(
	const struct rec *a,
	const struct rec *b,
	size_t shift)
{
	/* Every field; the offsets of the second run are shifted by the start of its buffer. */
	if (a->nal_type != b->nal_type || a->temporal_id != b->temporal_id)
		return 0;
	if (a->vps_id != b->vps_id || a->sps_id != b->sps_id || a->pps_id != b->pps_id)
		return 0;
	if (a->generation != b->generation)
		return 0;
	if (a->irap != b->irap || a->idr != b->idr || a->no_rasl != b->no_rasl)
		return 0;
	if (a->first != b->first || a->after_eos != b->after_eos || a->noop != b->noop)
		return 0;
	if (a->pic_output != b->pic_output || a->poc != b->poc)
		return 0;
	if (a->slice_count != b->slice_count || a->index != b->index)
		return 0;
	if (a->offset0 != b->offset0 + shift || a->size0 != b->size0)
		return 0;
	if (a->offset_last != b->offset_last + shift || a->size_last != b->size_last)
		return 0;
	if (a->au != b->au + shift || a->au_size != b->au_size)
		return 0;
	if (a->num_st_before != b->num_st_before || a->num_st_after != b->num_st_after || a->num_st_foll != b->num_st_foll)
		return 0;
	if (a->st_before0 != b->st_before0 || a->st_after0 != b->st_after0 || a->st_foll0 != b->st_foll0)
		return 0;
	return 1;
}

/* The start of each access unit of the long scenario: the offsets of its AUDs. */
static size_t au_starts[64];
static unsigned au_count;

/* Appends an access unit delimiter and notes where it starts. */
static void
put_aud(void)
{
	/* The AUD starts the access unit. */
	au_starts[au_count] = ws.size;
	au_count++;
	put_plain(T_AUD);
}

/* Builds a stream of ten access units, each opening with an AUD, that exercises most rules. */
static void
build_scenario(void)
{
	/* The sets, the IDR in two segments with a suffix SEI. */
	h265w_stream_init(&ws, sbuf, sizeof(sbuf));
	nal_count = 0U;
	au_count = 0U;
	sets_default();
	put_aud();
	put_vps();
	put_sps();
	put_pps();
	put_picture(T_IDR_W_RADL, 0U);
	put_segment(T_IDR_W_RADL, 8U);
	put_plain(T_SUFFIX_SEI);

	/* A trailing picture, then a picture of two segments with a PPS and an SEI between them. */
	put_aud();
	put_picture(T_TRAIL_R, 1U);
	put_aud();
	put_picture(T_TRAIL_R, 2U);
	put_plain(T_PREFIX_SEI);
	wpps.pps_id = 1U;
	put_pps();
	wpps.pps_id = 0U;
	put_segment(T_TRAIL_R, 8U);

	/* A picture ended by an end of sequence, a CRA after it with its RASL and RADL pictures. */
	put_aud();
	put_picture(T_TRAIL_R, 3U);
	put_plain(T_EOS);
	put_aud();
	put_picture(T_CRA, 10U);
	put_aud();
	put_picture(T_RASL_N, 8U);
	put_aud();
	put_picture(T_RADL_R, 9U);
	put_aud();
	put_picture(T_TRAIL_R, 11U);

	/* A picture with a reserved type and a layer 1 unit, a new SPS content for an IDR, a picture of a missing PPS. */
	put_aud();
	put_nal(10U, 0U, 1U, (const uint8_t *)"\x11\x80", 2U);
	put_nal(T_TRAIL_R, 1U, 1U, (const uint8_t *)"\x11\x80", 2U);
	put_picture(T_TRAIL_R, 12U);
	put_aud();
	wsps.max_dec_pic_buffering_minus1[0] = 3U;
	put_sps();
	put_picture(T_IDR_N_LP, 0U);
	put_aud();
	slice_reset(1U, 0U, 1U);
	wslice.pps_id = 9U;
	put_slice(T_TRAIL_R, 0U);

	/* An end of bitstream and a CRA after it. */
	put_aud();
	put_picture(T_TRAIL_R, 2U);
	put_plain(T_EOB);
	put_aud();
	put_picture(T_CRA, 6U);
	put_aud();
	put_picture(T_TRAIL_R, 7U);
}

/* Giving the reader one access unit at a time returns the same pictures as the whole stream at once. */
static void
check_split_input(void)
{
	static struct rec whole[MAX_RECS];
	unsigned whole_count;
	unsigned i;
	unsigned j;
	size_t start;
	size_t end;
	unsigned pos;
	struct h265_stream_counts counts;
	int error;

	/* Reads the whole scenario at once. */
	build_scenario();
	run_whole();
	whole_count = nrecs;
	CHECK(whole_count == 12U);
	for (i = 0U; i < whole_count && i < MAX_RECS; i++)
		whole[i] = recs[i];
	counts = rd.counts;
	CHECK(counts.broken_pictures == 1U);
	CHECK(counts.rasl_skipped == 1U);
	CHECK(counts.reserved_type == 1U);
	CHECK(counts.other_layer == 1U);
	CHECK(au_count == 14U);

	/* Reads it again with one buffer per access unit. */
	reader_start();
	pos = 0U;
	for (i = 0U; i < au_count; i++) {
		start = au_starts[i];
		end = (i + 1U < au_count) ? au_starts[i + 1U] : ws.size;
		h265_stream_input(&rd, sbuf + start, end - start);
		for (;;) {
			error = h265_stream_next(&rd, &pic);
			if (error != 0)
				break;
			check_picture_invariants(&pic, end - start);
			record(&pic, &recs[0]);
			CHECK(pos < whole_count);
			if (pos < whole_count)
				CHECK(same_recs(&whole[pos], &recs[0], start) != 0);
			pos++;
		}
		CHECK(error == ENOENT);
	}
	CHECK(pos == whole_count);

	/* The counts and the state are the same too. */
	CHECK(memcmp(&rd.counts, &counts, sizeof(counts)) == 0);
	CHECK(rd.returned == whole_count);

	/* Reading the whole stream with a buffer per picture of two access units gives the same pictures. */
	reader_start();
	pos = 0U;
	for (i = 0U; i < au_count; i += 2U) {
		start = au_starts[i];
		j = (i + 2U < au_count) ? i + 2U : au_count;
		end = (j < au_count) ? au_starts[j] : ws.size;
		h265_stream_input(&rd, sbuf + start, end - start);
		for (;;) {
			error = h265_stream_next(&rd, &pic);
			if (error != 0)
				break;
			record(&pic, &recs[0]);
			if (pos < whole_count)
				CHECK(same_recs(&whole[pos], &recs[0], start) != 0);
			pos++;
		}
	}
	CHECK(pos == whole_count);

	/* The scenario's pictures, from the clauses: poc values in order, flags where the rules put them. */
	CHECK(whole[0].poc == 0 && whole[0].first == 1 && whole[0].no_rasl == 1);
	CHECK(whole[1].poc == 1);
	CHECK(whole[2].poc == 2 && whole[2].slice_count == 2U);
	CHECK(whole[3].poc == 3);
	CHECK(whole[4].poc == 10 && whole[4].after_eos == 1 && whole[4].no_rasl == 1 && whole[4].noop == 1);
	CHECK(whole[5].poc == 9);
	CHECK(whole[6].poc == 11);
	CHECK(whole[7].poc == 12);
}

/* The entry points of the API: missing arguments, empty buffers, repeated calls after the end. */
static void
check_api(void)
{
	int error;

	/* A buffer with no NAL unit gives ENOENT, again and again. */
	reader_start();
	h265_stream_input(&rd, NULL, 0U);
	error = h265_stream_next(&rd, &pic);
	CHECK(error == ENOENT);
	error = h265_stream_next(&rd, &pic);
	CHECK(error == ENOENT);
	h265_stream_input(&rd, sbuf, 0U);
	error = h265_stream_next(&rd, &pic);
	CHECK(error == ENOENT);

	/* Missing arguments are refused, never dereferenced. */
	error = h265_stream_next(NULL, &pic);
	CHECK(error == EINVAL);
	error = h265_stream_next(&rd, NULL);
	CHECK(error == EINVAL);
	error = h265_stream_parameters(NULL, sbuf, 0U);
	CHECK(error == EINVAL);
	h265_stream_input(NULL, sbuf, 0U);
	h265_stream_reset(NULL);
	h265_stream_destroy(NULL);
	h265_stream_init(NULL);

	/* Garbage with no start code gives ENOENT. */
	memset(sbuf, 0x77, 100U);
	h265_stream_input(&rd, sbuf, 100U);
	error = h265_stream_next(&rd, &pic);
	CHECK(error == ENOENT);

	/* Only parameter sets: they are stored and the call gives ENOENT. */
	begin();
	reader_start();
	h265_stream_input(&rd, sbuf, ws.size);
	error = h265_stream_next(&rd, &pic);
	CHECK(error == ENOENT);
	CHECK(rd.store.vps_present[0] != 0);
	CHECK(rd.store.sps_present[0] != 0);
	CHECK(rd.store.pps_present[0] != 0);
	CHECK(rd.store.generation == 3U);

	/* A fresh reader is empty: nothing started, the first picture pending. */
	h265_stream_init(&rd);
	CHECK(rd.started == 0);
	CHECK(rd.first_pending == 1);
	CHECK(rd.eos_pending == 0);
	CHECK(rd.eos_seen == 0);
	CHECK(rd.cra_as_bla == 0);
	CHECK(rd.returned == 0U);
	CHECK(rd.reason == NULL);
	CHECK(rd.slice_rbsp == NULL);

	/* After a reset the reader waits for an IRAP picture and handles a CRA picture as a BLA picture. */
	h265_stream_reset(&rd);
	CHECK(rd.started == 0);
	CHECK(rd.first_pending == 1);
	CHECK(rd.cra_as_bla == 1);
}

/* Checks what every returned picture must satisfy, for the fuzz runs; returns the pictures counted. */
static unsigned
drain(
	size_t size)
{
	unsigned count;
	int error;

	/* Reads until the end of the buffer, with a bound against a reader that never ends. */
	count = 0U;
	for (;;) {
		error = h265_stream_next(&rd, &pic);
		if (error != 0)
			break;
		check_picture_invariants(&pic, size);
		count++;
		if (count > 5000U) {
			CHECK(count <= 5000U);
			break;
		}
	}
	return count;
}

/* A stream cut at every byte, bytes flipped, and random bytes: the reader never fails a check or a sanitizer. */
static void
check_garbage(void)
{
	size_t whole_size;
	size_t cut;
	unsigned round;
	unsigned flips;
	unsigned count;
	size_t length;
	size_t i;
	uint8_t *copy;
	static uint8_t mutated[1U << 16];

	/* The scenario cut at every byte (with the end of the cut a buffer end). */
	build_scenario();
	whole_size = ws.size;
	CHECK(whole_size < sizeof(mutated));
	if (whole_size >= sizeof(mutated))
		return;
	for (cut = 0U; cut <= whole_size; cut++) {
		reader_start();
		h265_stream_input(&rd, sbuf, cut);
		count = drain(cut);
		CHECK(count <= 64U);
	}

	/* The scenario with one to four bytes changed, including bytes of NAL unit headers and slice headers. */
	for (round = 0U; round < 6000U; round++) {
		memcpy(mutated, sbuf, whole_size);
		flips = 1U + rnd(4U);
		while (flips > 0U) {
			mutated[rnd((uint32_t)whole_size)] = (uint8_t)rnd(256U);
			flips--;
		}
		reader_start();
		h265_stream_input(&rd, mutated, whole_size);
		count = drain(whole_size);
		CHECK(count <= 64U);
	}

	/* The scenario with one bit flipped, at every bit of its first 700 bytes. */
	for (i = 0U; i < 700U * 8U && i / 8U < whole_size; i++) {
		memcpy(mutated, sbuf, whole_size);
		mutated[i / 8U] ^= (uint8_t)(0x80U >> (i % 8U));
		reader_start();
		h265_stream_input(&rd, mutated, whole_size);
		count = drain(whole_size);
		CHECK(count <= 64U);
	}

	/* Random bytes, with the values that matter to NAL units over-represented. */
	copy = mutated;
	for (round = 0U; round < 6000U; round++) {
		length = rnd(400U);
		for (i = 0U; i < length; i++) {
			switch (rnd(6U)) {
			case 0:
				copy[i] = 0U;
				break;
			case 1:
				copy[i] = 1U;
				break;
			case 2:
				copy[i] = 3U;
				break;
			default:
				copy[i] = (uint8_t)rnd(256U);
				break;
			}
		}
		reader_start();
		h265_stream_input(&rd, copy, length);
		count = drain(length);
		CHECK(count <= 400U);
	}
}

/* Runs every test and reports the checks. */
int
main(void)
{
	/* The tests in the order of the clauses they check. */
	check_idr_start();
	check_three_byte_start_codes();
	check_rps_counts();
	check_poc_sequence();
	check_cra_start();
	check_midstream_start();
	check_reset();
	check_no_output_of_prior_pics();
	check_end_of_sequence();
	check_end_of_sequence_activation();
	check_access_units();
	check_skipped_units();
	check_parameter_sets();
	check_refused_sets();
	check_sps_activation();
	check_sps_change();
	check_slice_segments();
	check_long_headers();
	check_split_input();
	check_api();
	check_garbage();

	/* Frees the long copy a test may have left and reports. */
	h265_stream_destroy(&rd);
	return check_finish("stream");
}
