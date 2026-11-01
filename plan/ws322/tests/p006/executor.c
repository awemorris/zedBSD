/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The executor side of ps-host-test's round trip: the PowerVR executor's
 * video readers (src/drivers/gpu/powervr/vk/vk-video.c, included so its
 * static readers can be called, and vk-video-h265.c) read libvulkan's
 * bytes, and the engine's writers (src/drivers/vpu/vpu-h265-ps.c) write
 * the sets the executor took back into NAL units.  The render node's and
 * the registry's calls the readers do not reach are stubs.
 */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "src/drivers/gpu/powervr/vk/vk-video.c"

#include "plan/ws322/tests/p006/ps-test.h"

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

/*
 * Reads a parameters create as video_parameters_create does up to its
 * session ([type][chain][flags][template][session]), then writes the
 * first set of each kind the H.265 add carried.
 */
int
test_executor_sets(
	const uint8_t *wire,
	size_t length,
	struct test_units *units)
{
	struct i915_wire_reader reader;
	struct video_chain chain;
	const struct video_h265_added *added;
	uint32_t type;

	/* The create info, read whole. */
	memset(units, 0, sizeof(*units));
	memset(&chain, 0, sizeof(chain));
	reader_on(&reader, wire, length);
	type = drv_i915_wire_read_u32(&reader);
	video_read_chain(&reader, &chain, 0);
	(void)drv_i915_wire_read_u32(&reader);
	(void)drv_i915_wire_read_u64(&reader);
	(void)drv_i915_wire_read_u64(&reader);
	if (reader.error != 0 || reader.offset != reader.size || type != VIDEO_TYPE_PARAMETERS_CREATE) {
		video_chain_free(&chain);
		return 1;
	}
	if (!chain.has_h265_create || chain.h265_added == NULL || chain.add_error != 0 ||
	    chain.max_vps != 16U || chain.max_sps != 16U || chain.max_pps != 64U) {
		video_chain_free(&chain);
		return 2;
	}

	/* The first set of each kind, written back. */
	added = chain.h265_added;
	units->vps_count = added->vps_count;
	units->sps_count = added->sps_count;
	units->pps_count = 0;
	if (added->pps != NULL)
		units->pps_count = 1;
	if (added->vps_count != 0)
		units->vps_error = vpu_h265_write_vps(added->vps[0], units->vps, sizeof(units->vps), &units->vps_bytes);
	if (added->sps_count != 0)
		units->sps_error = vpu_h265_write_sps(added->sps[0], units->sps, sizeof(units->sps), &units->sps_bytes);
	if (added->pps != NULL)
		units->pps_error = vpu_h265_write_pps(&added->pps->pps, units->pps, sizeof(units->pps), &units->pps_bytes);
	video_chain_free(&chain);
	return 0;
}

/*
 * Reads a decode as powervr_vk_record_video records it (with no command
 * buffer: read and dropped after the record is looked at).
 */
int
test_executor_decode(
	const uint8_t *wire,
	size_t length,
	struct test_picture *picture)
{
	struct i915_wire_reader reader;
	struct powervr_vk_record entry;
	const struct video_record *video;
	uint32_t index;
	int error;

	/* The record. */
	memset(picture, 0, sizeof(*picture));
	memset(&entry, 0, sizeof(entry));
	reader_on(&reader, wire, length);
	error = powervr_vk_record_video(NULL, NULL, GPU_OP_CMD_DECODE_VIDEO, &reader, &entry);
	if (error != 0 || reader.error != 0 || reader.offset != reader.size) {
		powervr_vk_video_drop(&entry);
		return 1;
	}

	/* What it kept. */
	video = entry.data;
	picture->has_picture = video->has_h265_picture;
	picture->flags = video->h265_picture.flags;
	picture->vps_id = video->h265_picture.vps_id;
	picture->sps_id = video->h265_picture.sps_id;
	picture->pps_id = video->h265_picture.pps_id;
	picture->poc = video->h265_picture.poc;
	picture->setup = -1;
	if (video->has_setup)
		picture->setup = video->setup.index;
	picture->reference_count = video->slot_count;
	for (index = 0; index < video->slot_count && index < 16U; index++)
		picture->references[index] = video->slots[index].index;
	powervr_vk_video_drop(&entry);
	return 0;
}
