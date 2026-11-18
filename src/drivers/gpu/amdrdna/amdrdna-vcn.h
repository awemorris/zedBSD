/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The words the driver and the video engine (VCN 2.0) exchange.
 *
 * The video engine is a small processor of its own inside the GPU that
 * runs a firmware program.  The driver talks to it through three rings in
 * system memory (one for decoding, two for encoding), a page the firmware
 * and the driver share, and messages in memory the decode ring's commands
 * point at.  Everything declared here builds those words in ordinary
 * memory or reads the firmware file's header; nothing touches the
 * hardware, so the same code runs in the kernel and in the host tests.
 */

#ifndef DRIVERS_GPU_AMDRDNA_AMDRDNA_VCN_H
#define DRIVERS_GPU_AMDRDNA_AMDRDNA_VCN_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "drivers/gpu/amdrdna/amdrdna-hw.h"

/*
 * The engine's own memory: the processor's stack (128 KiB) and its
 * working context (512 KiB), which the driver keeps in VRAM next to each
 * other.
 *
 * Values referenced: Linux 7.1 drivers/gpu/drm/amd/amdgpu/amdgpu_vcn.h
 * (AMDGPU_VCN_STACK_SIZE, AMDGPU_VCN_CONTEXT_SIZE).
 */
#define AMDRDNA_VCN_STACK_BYTES		(128U * 1024U)
#define AMDRDNA_VCN_CONTEXT_BYTES	(512U * 1024U)

/*
 * A ring of 4 KiB (1024 words), the page of a message, and the page the
 * firmware shares with the driver.
 *
 * Values referenced: Linux 7.1 drivers/gpu/drm/amd/amdgpu/vcn_v2_0.c
 * (vcn_v2_0_sw_init: amdgpu_ring_init with 512 words) and amdgpu_ring.c
 * (amdgpu_ring_init: the size rounded up from the words times 4 times
 * the two submissions in flight).
 * derived: 512 * 4 * 2 = 4096 bytes.
 */
#define AMDRDNA_VCN_RING_BYTES		4096U
#define AMDRDNA_VCN_RING_WORDS		(AMDRDNA_VCN_RING_BYTES / 4U)
#define AMDRDNA_VCN_PAGE_BYTES		4096U

/*
 * The steps a submission is padded to: 16 words on the decode ring, 64
 * on an encode ring.
 *
 * Values referenced: Linux 7.1 drivers/gpu/drm/amd/amdgpu/vcn_v2_0.c
 * (vcn_v2_0_dec_ring_vm_funcs and vcn_v2_0_enc_ring_vm_funcs: align_mask
 * 0xf and 0x3f).
 */
#define AMDRDNA_VCN_DECODE_STEP_WORDS	16U
#define AMDRDNA_VCN_ENCODE_STEP_WORDS	64U

/*
 * The words each piece of a ring's commands takes: on the decode ring,
 * the start and the end of a packet, an indirect buffer, a fence with
 * its interrupt, and the scratch write of the ring's test; the indirect
 * buffer that hands a message to the firmware; on an encode ring, an
 * indirect buffer, a fence with its interrupt, and the end.
 *
 * Values referenced: Linux 7.1 drivers/gpu/drm/amd/amdgpu/vcn_v2_0.c
 * (vcn_v2_0_dec_ring_insert_start, vcn_v2_0_dec_ring_insert_end,
 * vcn_v2_0_dec_ring_emit_ib, vcn_v2_0_dec_ring_emit_fence,
 * vcn_v2_0_dec_ring_test_ring, vcn_v2_0_enc_ring_emit_ib,
 * vcn_v2_0_enc_ring_emit_fence, vcn_v2_0_enc_ring_insert_end) and
 * amdgpu_vcn.c (amdgpu_vcn_dec_send_msg: 16 words).
 */
#define AMDRDNA_VCN_DECODE_START_WORDS	4U
#define AMDRDNA_VCN_DECODE_END_WORDS	2U
#define AMDRDNA_VCN_DECODE_IB_WORDS	8U
#define AMDRDNA_VCN_DECODE_FENCE_WORDS	14U
#define AMDRDNA_VCN_DECODE_SCRATCH_WORDS	4U
#define AMDRDNA_VCN_MESSAGE_IB_WORDS	16U
#define AMDRDNA_VCN_ENCODE_IB_WORDS	5U
#define AMDRDNA_VCN_ENCODE_FENCE_WORDS	5U
#define AMDRDNA_VCN_ENCODE_END_WORDS	1U

/*
 * The kinds of stream a decode session is created for: H.264 (the
 * firmware's fast H.264 path, which the drivers use) and H.265.
 *
 * Values referenced: Mesa 25.0.7 src/amd/common/ac_vcn_dec.h
 * (RDECODE_CODEC_H264_PERF, RDECODE_CODEC_H265).
 */
#define AMDRDNA_VCN_STREAM_H264		0x00000007U
#define AMDRDNA_VCN_STREAM_H265		0x00000010U

/*
 * The bytes of a creation message (a header with one index entry and the
 * creation body) and of a destruction message (the header without its
 * index entry).
 *
 * Values referenced: Mesa 25.0.7 src/amd/common/ac_vcn_dec.h (the
 * message header of six words with one index entry of four words, and the
 * creation body of four words).
 * derived: 6 * 4 + 4 * 4 = 40 bytes of header, 40 + 16 = 56 for the
 * creation, 40 - 16 = 24 for the destruction.
 */
#define AMDRDNA_VCN_CREATE_BYTES	56U
#define AMDRDNA_VCN_DESTROY_BYTES	24U

/*
 * The page the firmware shares with the driver: the bytes the firmware
 * reads of it, and the queues whose "ring reset" mark the driver raises
 * while it sets that queue's ring up.
 *
 * Values referenced: Linux 7.1 drivers/gpu/drm/amd/amdgpu/amdgpu_vcn.h
 * (struct amdgpu_fw_shared and the structures it holds).
 * derived: the structure is 92 bytes long with its natural alignment.
 */
#define AMDRDNA_VCN_SHARED_BYTES	92U
#define AMDRDNA_VCN_QUEUE_DECODE	0U
#define AMDRDNA_VCN_QUEUE_ENCODE	1U
#define AMDRDNA_VCN_QUEUE_ENCODE_LOW	2U

/*
 * The table of register writes the security processor applies when the
 * engine starts under its dynamic power gating: (register, value) pairs
 * in the engine's own register numbering, at most 64 pairs (one page
 * holds them).  The setup names where the engine's window of registers
 * starts (in dwords from BAR 5), the processor's context, the shared page
 * and the graphics block's address layout.
 *
 * Values referenced: Linux 7.1 drivers/gpu/drm/amd/amdgpu/amdgpu_vcn.c
 * (amdgpu_vcn_sw_init: the table's buffer of 64 * 2 * 4 bytes).
 */
#define AMDRDNA_VCN_DPG_WORDS		128U

struct amdrdna_vcn_dpg_setup {
	uint32_t window;
	uint64_t context;
	uint64_t shared;
	uint32_t address_config;
};

/*
 * What the version word of the firmware file says.
 *
 * Newer files name the encoder's version (major, minor), the decoder's
 * version, the VEP's version and a revision; older files name a major
 * and a minor version and a family.  named says which way the word reads.
 */
struct amdrdna_vcn_version {
	bool named;
	unsigned encode_major;
	unsigned encode_minor;
	unsigned decode;
	unsigned vep;
	unsigned revision;
	unsigned major;
	unsigned minor;
	unsigned family;
};

/*
 * What the driver keeps of a checked firmware file: its whole size (the
 * engine's cache window covers the file and a word more), where the
 * program starts in it and how long it is, and the version.
 */
struct amdrdna_vcn_firmware {
	uint32_t file_bytes;
	uint32_t program_at;
	uint32_t program_bytes;
	uint32_t version_word;
	struct amdrdna_vcn_version version;
};

int
amdrdna_vcn_firmware_check(
	const uint8_t *bytes,
	size_t length,
	struct amdrdna_vcn_firmware *firmware);

void
amdrdna_vcn_version_read(
	uint32_t word,
	struct amdrdna_vcn_version *version);

uint32_t
amdrdna_vcn_cache_bytes(
	uint32_t file_bytes);

uint32_t
amdrdna_vcn_decode_ring_control(
	uint32_t ring_bytes);

uint32_t
amdrdna_vcn_dpg_offset(
	uint32_t index);

unsigned
amdrdna_vcn_dpg_table(
	uint32_t *words,
	unsigned capacity,
	const struct amdrdna_vcn_dpg_setup *setup);

uint32_t
amdrdna_vcn_ring_room(
	uint32_t read,
	uint32_t write,
	uint32_t words);

unsigned
amdrdna_vcn_decode_start(
	uint32_t *words);

unsigned
amdrdna_vcn_decode_end(
	uint32_t *words);

unsigned
amdrdna_vcn_decode_ib(
	uint32_t *words,
	unsigned context,
	uint64_t address,
	uint32_t length);

unsigned
amdrdna_vcn_decode_fence(
	uint32_t *words,
	uint64_t address,
	uint32_t sequence);

unsigned
amdrdna_vcn_decode_scratch(
	uint32_t *words,
	uint32_t value);

unsigned
amdrdna_vcn_decode_pad(
	uint32_t *words,
	unsigned count);

unsigned
amdrdna_vcn_message_ib(
	uint32_t *words,
	uint64_t message);

unsigned
amdrdna_vcn_encode_ib(
	uint32_t *words,
	unsigned context,
	uint64_t address,
	uint32_t length);

unsigned
amdrdna_vcn_encode_fence(
	uint32_t *words,
	uint64_t address,
	uint32_t sequence);

unsigned
amdrdna_vcn_encode_end(
	uint32_t *words);

unsigned
amdrdna_vcn_encode_pad(
	uint32_t *words,
	unsigned count);

int
amdrdna_vcn_message_create(
	uint8_t *message,
	size_t bytes,
	uint32_t handle,
	uint32_t stream,
	uint32_t width,
	uint32_t height);

int
amdrdna_vcn_message_destroy(
	uint8_t *message,
	size_t bytes,
	uint32_t handle);

void
amdrdna_vcn_shared_init(
	uint8_t *shared);

void
amdrdna_vcn_shared_queue_reset(
	uint8_t *shared,
	unsigned queue,
	bool resetting);

uint32_t
amdrdna_vcn_doorbell_range_word(
	uint32_t old,
	uint32_t first,
	uint32_t count);

/*
 * The decode of one picture: the message the firmware reads (a header
 * with two index entries, the decode body, the codec's body), the
 * feedback buffer it reports into, the table of the inverse transform's
 * scaling lists, and the session context buffer it keeps between
 * pictures.
 *
 * Values referenced: Mesa 25.0.7 src/amd/common/ac_vcn_dec.h (the message
 * header and index entry, the decode body, the H.264 body, the feedback
 * header, RDECODE_SESSION_CONTEXT_SIZE) and src/amd/vulkan/radv_video.c
 * (FB_BUFFER_SIZE, IT_SCALING_TABLE_SIZE).
 * derived: 40 bytes of header (with the decode's index entry), 16 of the
 * codec's index entry, 188 of the decode body and 1124 of the H.264 body:
 * 1368 bytes; the feedback header is 44 bytes.
 */
#define AMDRDNA_VCN_DECODE_H264_BYTES	1368U
#define AMDRDNA_VCN_FEEDBACK_BYTES	2048U
#define AMDRDNA_VCN_FEEDBACK_HEADER_BYTES	44U
#define AMDRDNA_VCN_SCALING_BYTES	992U
#define AMDRDNA_VCN_SESSION_CONTEXT_BYTES	(128U * 1024U)

/*
 * The words of the indirect buffer that runs one decode: a command for
 * each of the eight buffers it names (three register writes of two words
 * each) and the engine's start, padded to the decode ring's step.
 *
 * derived: 8 * 6 + 2 = 50 words, 64 once padded.
 */
#define AMDRDNA_VCN_DECODE_RUN_WORDS	64U

/*
 * The DPB slots an H.264 decode session has at most, the references one
 * picture names at most, and the tag a picture without a slot is
 * decoded under (the one picture the firmware's DPB holds beyond the
 * slots).
 *
 * Values referenced: Mesa 25.0.7 src/amd/common/ac_vcn_dec.h (the 16
 * entries of the H.264 body's reference lists) and
 * src/gallium/drivers/radeonsi/radeon_vcn_dec.c (NUM_H264_REFS 17).
 */
#define AMDRDNA_VCN_H264_SLOTS		17U
#define AMDRDNA_VCN_H264_REFERENCES	16U
#define AMDRDNA_VCN_H264_LOOSE_TAG	17U

/*
 * The flags of the parameter sets, of a picture and of a reference as
 * libvulkan packs them on the wire (userland/desktop/libvulkan/video.c),
 * each the declared order of its StdVideo structure from bit 0.
 */
#define AMDRDNA_VCN_SPS_CONSTRAINT_SET0	(1U << 0)
#define AMDRDNA_VCN_SPS_CONSTRAINT_SET1	(1U << 1)
#define AMDRDNA_VCN_SPS_DIRECT_8X8	(1U << 6)
#define AMDRDNA_VCN_SPS_MBAFF		(1U << 7)
#define AMDRDNA_VCN_SPS_FRAME_MBS_ONLY	(1U << 8)
#define AMDRDNA_VCN_SPS_DELTA_POC_ZERO	(1U << 9)
#define AMDRDNA_VCN_SPS_SEPARATE_PLANES	(1U << 10)
#define AMDRDNA_VCN_SPS_GAPS		(1U << 11)
#define AMDRDNA_VCN_SPS_SCALING		(1U << 14)
#define AMDRDNA_VCN_PPS_TRANSFORM_8X8	(1U << 0)
#define AMDRDNA_VCN_PPS_REDUNDANT	(1U << 1)
#define AMDRDNA_VCN_PPS_CONSTRAINED	(1U << 2)
#define AMDRDNA_VCN_PPS_DEBLOCKING	(1U << 3)
#define AMDRDNA_VCN_PPS_WEIGHTED	(1U << 4)
#define AMDRDNA_VCN_PPS_BOTTOM_POC	(1U << 5)
#define AMDRDNA_VCN_PPS_ENTROPY		(1U << 6)
#define AMDRDNA_VCN_PPS_SCALING		(1U << 7)
#define AMDRDNA_VCN_PICTURE_FIELD	(1U << 0)
#define AMDRDNA_VCN_PICTURE_IDR		(1U << 2)
#define AMDRDNA_VCN_PICTURE_REFERENCE	(1U << 4)
#define AMDRDNA_VCN_PICTURE_PAIR	(1U << 5)
#define AMDRDNA_VCN_REFERENCE_TOP	(1U << 0)
#define AMDRDNA_VCN_REFERENCE_BOTTOM	(1U << 1)
#define AMDRDNA_VCN_REFERENCE_LONG_TERM	(1U << 2)
#define AMDRDNA_VCN_REFERENCE_MISSING	(1U << 3)

/*
 * The scaling lists of an H.264 parameter set as sent: which of the
 * twelve lists are present and which of those take the default matrix,
 * and the lists in their scan (zig-zag) order, six 4x4 lists and six
 * 8x8 lists.
 */
struct amdrdna_vcn_h264_scaling {
	uint32_t present_mask;
	uint32_t default_mask;
	uint8_t list4[6][16];
	uint8_t list8[6][64];
};

/* One H.264 sequence parameter set: the fields the decode message reads, and its own scaling lists if any. */
struct amdrdna_vcn_h264_sps {
	uint32_t flags;
	uint32_t profile_idc;
	uint32_t level_idc;
	uint32_t chroma_format_idc;
	uint32_t id;
	uint32_t bit_depth_luma_minus8;
	uint32_t bit_depth_chroma_minus8;
	uint32_t log2_max_frame_num_minus4;
	uint32_t pic_order_cnt_type;
	uint32_t log2_max_pic_order_cnt_lsb_minus4;
	uint32_t max_num_ref_frames;
	uint32_t pic_width_in_mbs_minus1;
	uint32_t pic_height_in_map_units_minus1;
	bool has_scaling;
	struct amdrdna_vcn_h264_scaling scaling;
};

/* One H.264 picture parameter set: the fields the decode message reads, and its own scaling lists if any. */
struct amdrdna_vcn_h264_pps {
	uint32_t flags;
	uint32_t sps_id;
	uint32_t pps_id;
	uint32_t num_ref_idx_l0_default_active_minus1;
	uint32_t num_ref_idx_l1_default_active_minus1;
	uint32_t weighted_bipred_idc;
	int32_t pic_init_qp_minus26;
	int32_t pic_init_qs_minus26;
	int32_t chroma_qp_index_offset;
	int32_t second_chroma_qp_index_offset;
	bool has_scaling;
	struct amdrdna_vcn_h264_scaling scaling;
};

/* The H.264 information of the picture a decode writes: its flags, the sets it names, its frame number and order counts. */
struct amdrdna_vcn_h264_picture {
	uint32_t flags;
	uint32_t sps_id;
	uint32_t pps_id;
	uint32_t frame_num;
	int32_t poc[2];
};

/*
 * One reference of a picture: the tag the firmware knows its picture by
 * (its DPB slot), its flags, its frame number (or long-term frame
 * index) and its order counts.
 */
struct amdrdna_vcn_h264_reference {
	uint32_t tag;
	uint32_t flags;
	uint32_t frame_num;
	int32_t poc[2];
};

/*
 * What the H.264 body of one decode message is made from: the picture's
 * sets, the picture, the tag it is decoded under, and its references.
 */
struct amdrdna_vcn_h264_decode {
	const struct amdrdna_vcn_h264_sps *sps;
	const struct amdrdna_vcn_h264_pps *pps;
	struct amdrdna_vcn_h264_picture picture;
	uint32_t tag;
	uint32_t reference_count;
	struct amdrdna_vcn_h264_reference references[AMDRDNA_VCN_H264_REFERENCES];
};

/*
 * What the decode body of one message says of a picture's buffers: the
 * session's handle and the picture's number, the picture's size, the
 * session's largest size (the hidden DPB's layout), and the bytes of the
 * bitstream, the hidden DPB, the decoder's context and the target; the
 * target's row pitch (both planes, in bytes) and where its chroma plane
 * starts after its luma plane.
 */
struct amdrdna_vcn_frame {
	uint32_t handle;
	uint32_t serial;
	uint32_t width;
	uint32_t height;
	uint32_t max_width;
	uint32_t max_height;
	uint32_t bitstream_bytes;
	uint32_t dpb_bytes;
	uint32_t context_bytes;
	uint32_t target_bytes;
	uint32_t pitch;
	uint32_t chroma_offset;
};

/*
 * The GPU addresses of the buffers one decode names: the session
 * context, the message, the hidden DPB, the decoder's context, the
 * bitstream (its first slice), the target, the feedback and the scaling
 * table.
 */
struct amdrdna_vcn_buffers {
	uint64_t session_context;
	uint64_t message;
	uint64_t dpb;
	uint64_t context;
	uint64_t bitstream;
	uint64_t target;
	uint64_t feedback;
	uint64_t scaling;
};

/*
 * The DPB slots an H.265 decode session has at most, the references one
 * picture names at most, and the tag a picture without a slot is decoded
 * under: the standard's DPB of at most 16 pictures with the current one
 * among them, and the picture past the slots the firmware's DPB holds.
 * The entries of each of a picture's three current reference sets, the
 * mark of an entry that names no reference, the columns and rows of tiles
 * a decode message holds, and the column widths and row heights a picture
 * parameter set carries.
 *
 * Values referenced: Recommendation ITU-T H.265 (A.4.2, MaxDpbSize at most
 * 16); Mesa 25.0.7 src/amd/common/ac_vcn_dec.h (the H.265 body's
 * ref_pic_list of 16, the three sets of 8, column_width_minus1 of 19 and
 * row_height_minus1 of 21) and src/amd/vulkan/radv_video.c (get_h265_msg:
 * curr_idx and ref_pic_list hold DPB slots, up to 16);
 * include/libc/vulkan/vk_video/vulkan_video_codec_h265std.h
 * (STD_VIDEO_H265_MAX_NUM_LIST_REF, STD_VIDEO_H265_NO_REFERENCE_PICTURE,
 * the tile lists).
 */
#define AMDRDNA_VCN_H265_SLOTS		16U
#define AMDRDNA_VCN_H265_REFERENCES	15U
#define AMDRDNA_VCN_H265_LOOSE_TAG	16U
#define AMDRDNA_VCN_H265_SET_ENTRIES	8U
#define AMDRDNA_VCN_H265_NO_REFERENCE	0xffU
#define AMDRDNA_VCN_H265_TILE_COLUMNS	19U
#define AMDRDNA_VCN_H265_TILE_ROWS	21U
#define AMDRDNA_VCN_H265_COLUMN_WIDTHS	19U
#define AMDRDNA_VCN_H265_ROW_HEIGHTS	21U

/*
 * The bytes of an H.265 decode message: the header with its two index
 * entries, the decode body and the H.265 body.
 *
 * Values referenced: Mesa 25.0.7 src/amd/common/ac_vcn_dec.h (the H.265
 * body).
 * derived: 56 + 188 + 296 = 540 bytes.
 */
#define AMDRDNA_VCN_DECODE_H265_BYTES	540U

/*
 * The flags of the H.265 parameter sets, of a picture and of a reference
 * as libvulkan packs them on the wire (userland/desktop/libvulkan/video.c),
 * each the declared order of its StdVideo structure from bit 0; and the
 * flags of the range and screen content extensions' tools, which Main
 * streams leave clear.
 */
#define AMDRDNA_VCN_H265_SPS_SEPARATE_PLANES	(1U << 1)
#define AMDRDNA_VCN_H265_SPS_SCALING		(1U << 4)
#define AMDRDNA_VCN_H265_SPS_SCALING_DATA	(1U << 5)
#define AMDRDNA_VCN_H265_SPS_AMP		(1U << 6)
#define AMDRDNA_VCN_H265_SPS_SAO		(1U << 7)
#define AMDRDNA_VCN_H265_SPS_PCM		(1U << 8)
#define AMDRDNA_VCN_H265_SPS_PCM_FILTER_OFF	(1U << 9)
#define AMDRDNA_VCN_H265_SPS_LONG_TERM		(1U << 10)
#define AMDRDNA_VCN_H265_SPS_TEMPORAL_MVP	(1U << 11)
#define AMDRDNA_VCN_H265_SPS_STRONG_INTRA	(1U << 12)
#define AMDRDNA_VCN_H265_SPS_EXTENSION_TOOLS	0x3fff8000U
#define AMDRDNA_VCN_H265_PPS_DEPENDENT		(1U << 0)
#define AMDRDNA_VCN_H265_PPS_OUTPUT_FLAG	(1U << 1)
#define AMDRDNA_VCN_H265_PPS_SIGN_HIDING	(1U << 2)
#define AMDRDNA_VCN_H265_PPS_CABAC_INIT		(1U << 3)
#define AMDRDNA_VCN_H265_PPS_CONSTRAINED	(1U << 4)
#define AMDRDNA_VCN_H265_PPS_TRANSFORM_SKIP	(1U << 5)
#define AMDRDNA_VCN_H265_PPS_CU_QP_DELTA	(1U << 6)
#define AMDRDNA_VCN_H265_PPS_SLICE_CHROMA_QP	(1U << 7)
#define AMDRDNA_VCN_H265_PPS_WEIGHTED		(1U << 8)
#define AMDRDNA_VCN_H265_PPS_WEIGHTED_BI	(1U << 9)
#define AMDRDNA_VCN_H265_PPS_BYPASS		(1U << 10)
#define AMDRDNA_VCN_H265_PPS_TILES		(1U << 11)
#define AMDRDNA_VCN_H265_PPS_ENTROPY_SYNC	(1U << 12)
#define AMDRDNA_VCN_H265_PPS_UNIFORM		(1U << 13)
#define AMDRDNA_VCN_H265_PPS_FILTER_TILES	(1U << 14)
#define AMDRDNA_VCN_H265_PPS_FILTER_SLICES	(1U << 15)
#define AMDRDNA_VCN_H265_PPS_DEBLOCK_OVERRIDE	(1U << 17)
#define AMDRDNA_VCN_H265_PPS_DEBLOCK_OFF	(1U << 18)
#define AMDRDNA_VCN_H265_PPS_SCALING_DATA	(1U << 19)
#define AMDRDNA_VCN_H265_PPS_LIST_CHANGES	(1U << 20)
#define AMDRDNA_VCN_H265_PPS_HEADER_EXTENSION	(1U << 21)
#define AMDRDNA_VCN_H265_PPS_EXTENSION_TOOLS	0x7f800000U
#define AMDRDNA_VCN_H265_PICTURE_IRAP		(1U << 0)
#define AMDRDNA_VCN_H265_PICTURE_IDR		(1U << 1)
#define AMDRDNA_VCN_H265_PICTURE_REFERENCE	(1U << 2)
#define AMDRDNA_VCN_H265_PICTURE_SPS_RPS	(1U << 3)
#define AMDRDNA_VCN_H265_REFERENCE_LONG_TERM	(1U << 0)
#define AMDRDNA_VCN_H265_REFERENCE_UNUSED	(1U << 1)

/*
 * The scaling lists of an H.265 parameter set as sent (StdVideoH265ScalingLists):
 * the six 4x4, six 8x8, six 16x16 and two 32x32 lists, each in the
 * order the standard's syntax carries its coefficients (the up-right
 * diagonal scan), and the DC coefficients of the 16x16 and 32x32 lists.
 */
struct amdrdna_vcn_h265_scaling {
	uint8_t list4[6][16];
	uint8_t list8[6][64];
	uint8_t list16[6][64];
	uint8_t list32[2][64];
	uint8_t dc16[6];
	uint8_t dc32[2];
};

/*
 * One H.265 sequence parameter set: its key (the video and sequence
 * parameter set ids), the fields the decode message reads, the DPB size of
 * its highest sub-layer (has_buffering clear when the set came without
 * its buffering limits), and its own scaling lists if any.
 */
struct amdrdna_vcn_h265_sps {
	uint32_t flags;
	uint32_t vps_id;
	uint32_t id;
	uint32_t chroma_format_idc;
	uint32_t width;
	uint32_t height;
	uint32_t bit_depth_luma_minus8;
	uint32_t bit_depth_chroma_minus8;
	uint32_t log2_max_pic_order_cnt_lsb_minus4;
	uint32_t log2_min_luma_coding_block_size_minus3;
	uint32_t log2_diff_max_min_luma_coding_block_size;
	uint32_t log2_min_luma_transform_block_size_minus2;
	uint32_t log2_diff_max_min_luma_transform_block_size;
	uint32_t max_transform_hierarchy_depth_inter;
	uint32_t max_transform_hierarchy_depth_intra;
	uint32_t num_short_term_ref_pic_sets;
	uint32_t num_long_term_ref_pics_sps;
	uint32_t pcm_sample_bit_depth_luma_minus1;
	uint32_t pcm_sample_bit_depth_chroma_minus1;
	uint32_t log2_min_pcm_luma_coding_block_size_minus3;
	uint32_t log2_diff_max_min_pcm_luma_coding_block_size;
	uint32_t max_sub_layers_minus1;
	bool has_buffering;
	uint32_t max_dec_pic_buffering_minus1;
	bool has_scaling;
	struct amdrdna_vcn_h265_scaling scaling;
};

/*
 * One H.265 picture parameter set: its key, the fields the decode
 * message reads, the tiles' column widths and row heights as sent, and
 * its own scaling lists if any.
 */
struct amdrdna_vcn_h265_pps {
	uint32_t flags;
	uint32_t vps_id;
	uint32_t sps_id;
	uint32_t id;
	uint32_t num_extra_slice_header_bits;
	uint32_t num_ref_idx_l0_default_active_minus1;
	uint32_t num_ref_idx_l1_default_active_minus1;
	int32_t init_qp_minus26;
	uint32_t diff_cu_qp_delta_depth;
	int32_t pps_cb_qp_offset;
	int32_t pps_cr_qp_offset;
	int32_t pps_beta_offset_div2;
	int32_t pps_tc_offset_div2;
	uint32_t log2_parallel_merge_level_minus2;
	uint32_t num_tile_columns_minus1;
	uint32_t num_tile_rows_minus1;
	uint32_t column_width_minus1[AMDRDNA_VCN_H265_COLUMN_WIDTHS];
	uint32_t row_height_minus1[AMDRDNA_VCN_H265_ROW_HEIGHTS];
	bool has_scaling;
	struct amdrdna_vcn_h265_scaling scaling;
};

/*
 * The H.265 information of the picture a decode writes: its flags, the
 * sets it names, the size of the slice header's own short-term set (in
 * reference pictures and in bits), its order count, and the DPB slots of
 * its three current reference sets (AMDRDNA_VCN_H265_NO_REFERENCE past
 * the last).
 */
struct amdrdna_vcn_h265_picture {
	uint32_t flags;
	uint32_t vps_id;
	uint32_t sps_id;
	uint32_t pps_id;
	uint32_t num_delta_pocs_of_ref_rps_idx;
	int32_t poc;
	uint32_t num_bits_for_st_ref_pic_set_in_slice;
	uint8_t before[AMDRDNA_VCN_H265_SET_ENTRIES];
	uint8_t after[AMDRDNA_VCN_H265_SET_ENTRIES];
	uint8_t long_term[AMDRDNA_VCN_H265_SET_ENTRIES];
};

/* One reference of an H.265 picture: the tag the firmware knows its picture by (its DPB slot), its flags and its order count. */
struct amdrdna_vcn_h265_reference {
	uint32_t tag;
	uint32_t flags;
	int32_t poc;
};

/*
 * What the H.265 body of one decode message is made from: the picture's
 * sets, the picture, the tag it is decoded under, its references, and
 * its three current reference sets as places in the references
 * (AMDRDNA_VCN_H265_NO_REFERENCE for an entry that names none).
 */
struct amdrdna_vcn_h265_decode {
	const struct amdrdna_vcn_h265_sps *sps;
	const struct amdrdna_vcn_h265_pps *pps;
	struct amdrdna_vcn_h265_picture picture;
	uint32_t tag;
	uint32_t reference_count;
	struct amdrdna_vcn_h265_reference references[AMDRDNA_VCN_H265_REFERENCES];
	uint8_t before[AMDRDNA_VCN_H265_SET_ENTRIES];
	uint8_t after[AMDRDNA_VCN_H265_SET_ENTRIES];
	uint8_t long_term[AMDRDNA_VCN_H265_SET_ENTRIES];
};

unsigned
amdrdna_vcn_create_run(
	uint32_t *words,
	uint64_t session_context,
	uint64_t message);

unsigned
amdrdna_vcn_decode_run(
	uint32_t *words,
	const struct amdrdna_vcn_buffers *buffers);

int
amdrdna_vcn_message_decode_h264(
	uint8_t *message,
	size_t bytes,
	const struct amdrdna_vcn_frame *frame,
	const struct amdrdna_vcn_h264_decode *h264,
	uint8_t *scaling,
	size_t scaling_bytes);

void
amdrdna_vcn_feedback_init(
	uint8_t *feedback);

uint32_t
amdrdna_vcn_feedback_status(
	const uint8_t *feedback);

int
amdrdna_vcn_h264_profile(
	const struct amdrdna_vcn_h264_sps *sps,
	uint32_t *profile);

uint32_t
amdrdna_vcn_h264_level(
	uint32_t level_idc);

uint64_t
amdrdna_vcn_h264_dpb_bytes(
	uint32_t width,
	uint32_t height,
	uint32_t slots);

uint64_t
amdrdna_vcn_h264_context_bytes(
	uint32_t width,
	uint32_t height,
	uint32_t slots);

void
amdrdna_vcn_h264_scaling(
	const struct amdrdna_vcn_h264_sps *sps,
	const struct amdrdna_vcn_h264_pps *pps,
	uint8_t scaling_4x4[6][16],
	uint8_t scaling_8x8[2][64]);

size_t
amdrdna_vcn_h264_gap_slice(
	uint8_t *bytes,
	size_t room,
	const struct amdrdna_vcn_h264_sps *sps,
	const struct amdrdna_vcn_h264_pps *pps,
	uint32_t frame_num,
	int32_t poc);

void
amdrdna_vcn_h264_gap_pps(
	const struct amdrdna_vcn_h264_pps *pps,
	struct amdrdna_vcn_h264_pps *gap);

int
amdrdna_vcn_message_decode_h265(
	uint8_t *message,
	size_t bytes,
	const struct amdrdna_vcn_frame *frame,
	const struct amdrdna_vcn_h265_decode *h265,
	uint8_t *scaling,
	size_t scaling_bytes);

int
amdrdna_vcn_h265_tiles(
	const struct amdrdna_vcn_h265_sps *sps,
	const struct amdrdna_vcn_h265_pps *pps,
	uint32_t *columns,
	uint32_t *rows);

void
amdrdna_vcn_h265_scaling(
	const struct amdrdna_vcn_h265_sps *sps,
	const struct amdrdna_vcn_h265_pps *pps,
	uint8_t *table,
	uint8_t dc16[6],
	uint8_t dc32[2]);

uint64_t
amdrdna_vcn_h265_dpb_bytes(
	uint32_t width,
	uint32_t height,
	uint32_t slots);

uint64_t
amdrdna_vcn_h265_context_bytes(
	uint32_t width,
	uint32_t height,
	uint32_t slots);

#endif
