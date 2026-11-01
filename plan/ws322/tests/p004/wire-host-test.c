/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Host check of the PowerVR executor's video wire (ws322-p004): the bytes
 * libvulkan's own video encoders write (plan/tools/rtx-video/wire-encode.c:
 * a parameters create with an SPS and a PPS, a decode, a begin) are read
 * by the executor's video readers (src/drivers/gpu/powervr/vk/vk-video.c,
 * included so its static readers can be called), every field and every
 * byte.  The render node's and the registry's calls the readers do not
 * reach are stubs.
 *
 *   wire-host-test
 */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "src/drivers/gpu/powervr/vk/vk-video.c"

#include "plan/tools/rtx-video/wire-test.h"

static int passes;
static int failures;

static void
check(
	int ok,
	const char *what)
{
	if (ok) {
		passes++;
		return;
	}
	failures++;
	printf("FAIL %s\n", what);
}

/* The kernel's calls, the host's. */
void *kern_malloc(size_t size) { return malloc(size); }
void *kern_calloc(size_t count, size_t size) { return calloc(count, size); }
void kern_free(void *pointer) { free(pointer); }
void kern_logf(const char *format, ...) { (void)format; }

/* The registry and the render node, which the readers under test do not reach. */
int powervr_vk_destroy_plain(struct powervr_vk_session *session, struct i915_wire_reader *reader, enum i915_vk_object_kind kind) { (void)session; (void)reader; (void)kind; abort(); }
void *powervr_vk_payload(struct powervr_vk_session *session, enum i915_vk_object_kind kind, uint64_t identity) { (void)session; (void)kind; (void)identity; return NULL; }
struct powervr_vk_object *powervr_vk_object_find(struct powervr_vk_session *session, enum i915_vk_object_kind kind, uint64_t identity) { (void)session; (void)kind; (void)identity; return NULL; }
int powervr_vk_object_retain(struct powervr_vk_object *object) { (void)object; return 0; }
int powervr_vk_object_release(struct powervr_vk_object *object) { (void)object; return 0; }
uint64_t powervr_vk_create_tail(struct i915_wire_reader *reader) { (void)reader; abort(); }
void powervr_vk_create_reply(struct powervr_vk_session *session, struct i915_wire_writer *reply, enum i915_vk_object_kind kind, uint64_t identity, void *payload, int (*destroy)(struct powervr_vk_session *, void *), int error) { (void)session; (void)reply; (void)kind; (void)identity; (void)payload; (void)destroy; (void)error; abort(); }
const struct drv_powervr_video_ops *powervr_video_engine(void **engine) { (void)engine; return NULL; }
void powervr_vk_query_report(struct powervr_vk_session *session, int32_t status) { (void)session; (void)status; }
int powervr_vk_image_address(const struct powervr_vk_image *image, uint64_t *address) { (void)image; (void)address; return EIO; }
int powervr_vk_transfer_rows(struct powervr_vk_session *session, uint64_t source, uint32_t source_pitch, uint64_t destination, uint32_t destination_pitch, uint32_t row_bytes, uint32_t rows) { (void)session; (void)source; (void)source_pitch; (void)destination; (void)destination_pitch; (void)row_bytes; (void)rows; return EIO; }
int powervr_buffer_create(struct powervr_render *render, uint64_t bytes, struct powervr_buffer **result) { (void)render; (void)bytes; (void)result; return ENOMEM; }
int powervr_resource_make(struct powervr_session *session, struct powervr_buffer *buffer, int mappable, int shareable, struct powervr_resource **result) { (void)session; (void)buffer; (void)mappable; (void)shareable; (void)result; return ENOMEM; }
void powervr_buffer_put(struct powervr_render *render, struct powervr_buffer *buffer) { (void)render; (void)buffer; }
void powervr_resource_unmake(struct powervr_session *session, struct powervr_resource *resource) { (void)session; (void)resource; }
int powervr_session_point_wait(const struct powervr_session *session, unsigned kind, uint32_t value, unsigned milliseconds) { (void)session; (void)kind; (void)value; (void)milliseconds; return 0; }

/* A reader over bytes. */
static void
reader_on(
	struct i915_wire_reader *reader,
	const uint8_t *bytes,
	size_t length)
{
	memset(reader, 0, sizeof(*reader));
	reader->base = bytes;
	reader->size = length;
}

/* The parameters create: the SPS's and the PPS's every field. */
static void
test_parameters(void)
{
	struct i915_wire_reader reader;
	struct video_chain chain;
	const struct vpu_h264_sps *sps;
	const struct vpu_h264_pps *pps;
	uint8_t bytes[8192];
	size_t length;
	uint64_t present;
	uint32_t type;
	uint32_t index;
	int lists;

	length = wire_encode_parameters(bytes, sizeof(bytes));
	check(length != 0, "parameters encoded");
	reader_on(&reader, bytes, length);
	memset(&chain, 0, sizeof(chain));
	present = drv_i915_wire_read_u64(&reader);
	type = drv_i915_wire_read_u32(&reader);
	video_read_chain(&reader, &chain, 0);
	(void)drv_i915_wire_read_u32(&reader);
	(void)drv_i915_wire_read_u64(&reader);
	check(drv_i915_wire_read_u64(&reader) == 0x70U, "parameters' session");
	check(present == 1 && type == VIDEO_TYPE_PARAMETERS_CREATE, "parameters' type");
	check(reader.error == 0 && reader.offset == reader.size, "parameters read to the end");
	check(chain.has_h264_create && chain.max_sps == 4 && chain.max_pps == 8 && chain.add_error == 0, "H.264 create");
	check(chain.new_sps_count == 1 && chain.new_pps != NULL && chain.new_pps->next == NULL, "one SPS, one PPS");
	if (chain.new_sps_count != 1 || chain.new_pps == NULL) {
		video_chain_free(&chain);
		return;
	}
	sps = chain.new_sps[0];
	check(sps->profile_idc == 100 && sps->id == 1 && sps->chroma_format_idc == 1, "SPS profile, id, chroma");
	check(sps->log2_max_frame_num_minus4 == 4 && sps->pic_order_cnt_type == 1 && sps->log2_max_pic_order_cnt_lsb_minus4 == 2, "SPS numbering");
	check(sps->offset_count == 2 && sps->offsets[0] == 3 && sps->offsets[1] == -4, "SPS order count cycle");
	check(sps->max_num_ref_frames == 4 && sps->pic_width_in_mbs_minus1 == 119 && sps->pic_height_in_map_units_minus1 == 67, "SPS size");
	check((sps->flags & (VPU_H264_SPS_FRAME_MBS_ONLY | VPU_H264_SPS_DIRECT_8X8 | VPU_H264_SPS_SCALING)) ==
	      (VPU_H264_SPS_FRAME_MBS_ONLY | VPU_H264_SPS_DIRECT_8X8 | VPU_H264_SPS_SCALING), "SPS flags");
	check(sps->has_scaling && sps->scaling.present_mask == 0x41 && sps->scaling.default_mask == 0x40, "SPS scaling masks");
	lists = 1;
	for (index = 0; index < 16; index++) {
		if (sps->scaling.list4[0][index] != index + 1U)
			lists = 0;
	}
	check(lists, "SPS scaling list");
	pps = &chain.new_pps->pps;
	check(pps->sps_id == 1 && pps->pps_id == 2 && pps->num_ref_idx_l0_default_active_minus1 == 3, "PPS ids and references");
	check(pps->weighted_bipred_idc == 2 && pps->pic_init_qp_minus26 == -5 && pps->chroma_qp_index_offset == -2 && pps->second_chroma_qp_index_offset == 3, "PPS prediction and quantizers");
	check(pps->flags == (VPU_H264_PPS_CABAC | VPU_H264_PPS_TRANSFORM_8X8) && !pps->has_scaling, "PPS flags");
	video_chain_free(&chain);
}

/* The decode: the range, the output, the setup slot, the references, the picture. */
static void
test_decode(void)
{
	struct i915_wire_reader reader;
	struct powervr_vk_record entry;
	const struct video_record *video;
	uint8_t bytes[8192];
	size_t length;
	int error;

	length = wire_encode_decode(bytes, sizeof(bytes));
	check(length != 0, "decode encoded");
	reader_on(&reader, bytes, length);
	memset(&entry, 0, sizeof(entry));
	error = powervr_vk_record_video(NULL, NULL, GPU_OP_CMD_DECODE_VIDEO, &reader, &entry);
	check(error == 0 && reader.error == 0 && reader.offset == reader.size, "decode read to the end");
	video = entry.data;
	if (video == NULL)
		return;
	check(video->offset == 256 && video->range == 4096, "decode's range");
	check(video->destination.view == 0x50 && video->destination.width == 1920 && video->destination.height == 1088, "decode's output");
	check(video->has_setup && video->setup.index == 3 && video->setup.has_picture && video->setup.resource.view == 0x50, "decode's setup slot");
	check(video->slot_count == 2 && video->slots[0].index == 1 && video->slots[1].index == 2, "decode's references");
	check(video->slots[0].resource.view == 0x51 && video->slots[1].resource.view == 0x52, "references' views");
	check(video->has_picture && video->picture.flags == (VIDEO_PICTURE_IDR | (1U << 4)), "picture's flags");
	check(video->picture.sps_id == 1 && video->picture.pps_id == 2, "picture's sets");
	check(video->picture.poc[0] == 10 && video->picture.poc[1] == INT32_MAX, "picture's order counts");
	powervr_vk_video_drop(&entry);
	check(entry.data == NULL, "decode dropped");
}

/* The begin: the session, the parameters, the three slots. */
static void
test_begin(void)
{
	struct i915_wire_reader reader;
	struct powervr_vk_record entry;
	const struct video_record *video;
	uint8_t bytes[8192];
	size_t length;
	int error;

	length = wire_encode_begin(bytes, sizeof(bytes));
	check(length != 0, "begin encoded");
	reader_on(&reader, bytes, length);
	memset(&entry, 0, sizeof(entry));
	error = powervr_vk_record_video(NULL, NULL, GPU_OP_CMD_BEGIN_VIDEO_CODING, &reader, &entry);
	check(error == 0 && reader.error == 0 && reader.offset == reader.size, "begin read to the end");
	video = entry.data;
	if (video == NULL)
		return;
	check(video->slot_count == 3, "begin's slots");
	check(video->slots[0].index == 4 && video->slots[0].has_picture, "slot 4 with a picture");
	check(video->slots[1].index == 6 && !video->slots[1].has_picture, "slot 6 without");
	check(video->slots[2].index == -1 && video->slots[2].has_picture, "no slot with a picture");
	powervr_vk_video_drop(&entry);
}

int
main(void)
{
	test_parameters();
	test_decode();
	test_begin();
	printf("wire: %d passed, %d failed\n", passes, failures);
	return failures == 0 ? 0 : 1;
}
