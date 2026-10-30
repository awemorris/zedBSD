/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Host check of ws269-p010i: how render-job.c places a job of two draws.
 * The render node's memory, mappings, contexts and kick are stand-ins
 * here: buffers are host memory at addresses handed out in order, and the
 * kick keeps the geometry and fragment commands it is given.  The check
 * follows the stream from the geometry command: each draw's primitive
 * state block and programs, the stream's end, the push constants and the
 * sets' copies, and the scissor table the fragment command names.
 */

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <uapi/errno.h>

#include "drivers/gpu/powervr/render-job.h"
#include "drivers/gpu/powervr/transfer-job.h"
#include "drivers/gpu/powervr/render-command.h"
#include "drivers/gpu/powervr/interface.h"
#include "drivers/gpu/powervr/vk/vk-draw.h"
#include "drivers/gpu/powervr/vk/vk.h"

#define HOST_GENERAL_BASE	0x8000100000ULL
#define HOST_STEP		0x100000ULL

static int failures;
static int passes;
static uint64_t next_general = HOST_GENERAL_BASE;
static uint64_t next_pds = POWERVR_PDS_HEAP_BASE + 0x100000ULL;
static uint64_t next_usc = POWERVR_USC_HEAP_BASE + 0x100000ULL;
static struct powervr_buffer *placed[256];
static uint64_t placed_at[256];
static unsigned placed_count;
static uint8_t kicked_geometry[POWERVR_GEOMETRY_BYTES];
static uint8_t kicked_fragment[POWERVR_FRAGMENT_BYTES];
static struct powervr_render_context context_stand_in;
static struct powervr_target *target_stand_in = (struct powervr_target *)&context_stand_in;
static struct powervr_target_layout layout_stand_in;

static void
expect(
	int good,
	const char *what)
{
	if (good) {
		passes++;
		return;
	}
	failures++;
	printf("FAIL %s\n", what);
}

/* The kernel services, on the host. */
void *kern_malloc(size_t size) { return malloc(size); }
void *kern_calloc(size_t count, size_t size) { return calloc(count, size); }
void kern_free(void *pointer) { free(pointer); }
void kern_logf(const char *format, ...) { va_list list; va_start(list, format); printf("  log: "); vprintf(format, list); va_end(list); }
uint64_t powervr_timing_now(void) { return 0; }
unsigned powervr_target_take_half(struct powervr_target *target) { (void)target; return 0; }
void powervr_target_catalogues(const struct powervr_target *target, unsigned half, uint64_t catalogues[POWERVR_TARGET_CATALOGUES]) { unsigned i; (void)target; (void)half; for (i = 0; i < POWERVR_TARGET_CATALOGUES; i++) catalogues[i] = 0; }
uint32_t powervr_object_address(const struct powervr_object *object, uint32_t offset) { (void)object; return offset; }
uint32_t powervr_object_get32(const struct powervr_object *object, uint32_t offset) { (void)object; (void)offset; return 0; }
uint32_t powervr_target_half_fragment(const struct powervr_target *target, unsigned half) { (void)target; (void)half; return 0; }
void powervr_target_half_note(struct powervr_target *target, unsigned half, uint32_t fragment_value) { (void)target; (void)half; (void)fragment_value; }
void powervr_timing_add_span(enum powervr_timing_part part, uint64_t start, uint64_t end) { (void)part; (void)start; (void)end; }
unsigned long spin_lock_irqsave(struct spinlock *lock) { (void)lock; return 0; }
void spin_unlock_irqrestore(struct spinlock *lock, unsigned long enabled) { (void)lock; (void)enabled; }
/* The host's render finishes at its kick: every point is reached at once. */
int powervr_session_point_reached(const struct powervr_session *session, unsigned kind, uint32_t value) { (void)session; (void)kind; (void)value; return 1; }
int powervr_session_point_wait(const struct powervr_session *session, unsigned kind, uint32_t value, unsigned milliseconds) { (void)session; (void)kind; (void)value; (void)milliseconds; return 0; }
void powervr_session_point_set(struct powervr_session *session, unsigned kind, uint32_t value) { session->last_kind = kind; session->last_value = value; }
void powervr_render_retired(struct powervr_render *render, struct powervr_session *session, const struct powervr_render_marks *marks) { (void)render; (void)session; (void)marks; }
void powervr_timing_add(enum powervr_timing_part part, uint64_t start) { (void)part; (void)start; }
void mutex_lock(struct mutex *mutex) { (void)mutex; }
void mutex_unlock(struct mutex *mutex) { (void)mutex; }

/* Memory: host memory never freed, so the check reads it after the job returned it. */
int
powervr_buffer_create(struct powervr_render *render, uint64_t bytes, struct powervr_buffer **result)
{
	struct powervr_buffer *buffer;

	(void)render;
	buffer = calloc(1, sizeof(*buffer));
	buffer->view = calloc(1, (size_t)((bytes + 4095U) & ~4095ULL));
	buffer->bytes = bytes;
	buffer->run.size = (size_t)((bytes + 4095U) & ~4095ULL);
	buffer->references = 1;
	*result = buffer;
	return 0;
}

void powervr_buffer_put(struct powervr_render *render, struct powervr_buffer *buffer) { (void)render; buffer->references--; }

static void
note(struct powervr_buffer *buffer, uint64_t address)
{
	if (placed_count >= sizeof(placed) / sizeof(placed[0]))
		return;
	placed[placed_count] = buffer;
	placed_at[placed_count] = address;
	placed_count++;
}

int
powervr_resource_make(struct powervr_session *session, struct powervr_buffer *buffer, int mappable, int shareable, struct powervr_resource **result)
{
	struct powervr_resource *resource;

	(void)shareable;
	resource = calloc(1, sizeof(*resource));
	resource->session = session;
	resource->buffer = buffer;
	resource->mappable = mappable;
	resource->address = next_general;
	next_general += HOST_STEP;
	note(buffer, resource->address);
	*result = resource;
	return 0;
}

void powervr_resource_unmake(struct powervr_session *session, struct powervr_resource *resource) { (void)session; (void)resource; }
void powervr_session_note_job(struct powervr_session *session, const struct powervr_render_job *job, const struct powervr_render_draw *prefix) { (void)session; (void)job; (void)prefix; }

uint64_t
powervr_session_border_table(const struct powervr_session *session)
{
	(void)session;
	return 0;
}

int
powervr_session_program_map(struct powervr_session *session, enum powervr_program_heap heap, struct powervr_buffer *buffer, uint64_t *address)
{
	(void)session;
	if (heap == POWERVR_PROGRAM_PDS) {
		*address = next_pds;
		next_pds += HOST_STEP;
	} else {
		*address = next_usc;
		next_usc += HOST_STEP;
	}
	note(buffer, *address);
	return 0;
}

void powervr_session_program_unmap(struct powervr_session *session, enum powervr_program_heap heap, struct powervr_buffer *buffer, uint64_t address) { (void)session; (void)heap; (void)buffer; (void)address; }

/* Contexts, targets and the kick. */
int powervr_session_render_context(struct powervr_render *render, struct powervr_session *session, struct powervr_render_context **result) { (void)render; (void)session; *result = &context_stand_in; return 0; }
int powervr_session_target(struct powervr_render *render, struct powervr_session *session, const struct powervr_target_shape *shape, struct powervr_target **result) { (void)render; (void)session; (void)shape; *result = target_stand_in; return 0; }
const struct powervr_target_layout *powervr_target_layout_of(const struct powervr_target *target) { (void)target; return &layout_stand_in; }

int
powervr_session_kick_render(struct powervr_render *render, struct powervr_session *session, struct powervr_target *target, unsigned half, const uint8_t *geometry, const uint8_t *partial, const uint8_t *fragment, const struct powervr_sync *waits, unsigned wait_count, const struct powervr_sync *fragment_waits, unsigned fragment_wait_count, struct powervr_render_marks *marks)
{
	(void)render; (void)session; (void)target; (void)half; (void)partial; (void)waits; (void)wait_count; (void)fragment_waits; (void)fragment_wait_count;
	memcpy(kicked_geometry, geometry, sizeof(kicked_geometry));
	memcpy(kicked_fragment, fragment, sizeof(kicked_fragment));
	marks->geometry_value = 1;
	marks->fragment_value = 2;
	return 0;
}

/* The transfer context and kick: each kick counted. */
static unsigned transfer_kicks;
int powervr_session_transfer_context(struct powervr_render *render, struct powervr_session *session, struct powervr_transfer_context **result) { (void)render; (void)session; *result = (struct powervr_transfer_context *)&context_stand_in; return 0; }
int powervr_session_kick_transfer(struct powervr_render *render, struct powervr_session *session, const uint8_t *command, const struct powervr_sync *waits, unsigned wait_count, uint32_t *value) { (void)render; (void)session; (void)command; (void)waits; (void)wait_count; transfer_kicks++; *value = transfer_kicks; return 0; }
unsigned powervr_session_point_sync(const struct powervr_session *session, unsigned kind, uint32_t value, struct powervr_sync *sync) { (void)session; (void)kind; (void)value; (void)sync; return 0; }
int powervr_transfer_finish(struct powervr_session *session, uint32_t value, unsigned milliseconds) { (void)session; (void)value; (void)milliseconds; return 0; }

int powervr_render_finish(struct powervr_render *render, struct powervr_session *session, const struct powervr_render_marks *marks, unsigned milliseconds) { (void)render; (void)session; (void)marks; (void)milliseconds; return 0; }

/* Reads a word of device memory through the stand-in's placed buffers. */
static uint32_t
device_read(uint64_t address)
{
	unsigned index;
	uint64_t offset;
	const volatile uint8_t *view;

	for (index = 0; index < placed_count; index++) {
		if (address >= placed_at[index] && address < placed_at[index] + placed[index]->run.size) {
			offset = address - placed_at[index];
			view = placed[index]->view;
			return (uint32_t)view[offset] | (uint32_t)view[offset + 1] << 8 | (uint32_t)view[offset + 2] << 16 | (uint32_t)view[offset + 3] << 24;
		}
	}
	printf("FAIL no memory at 0x%llx\n", (unsigned long long)address);
	failures++;
	return 0;
}

static uint64_t
read64(const uint8_t *bytes, unsigned offset)
{
	uint64_t value;
	unsigned index;

	value = 0;
	for (index = 0; index < 8U; index++)
		value |= (uint64_t)bytes[offset + index] << (8U * index);
	return value;
}

int
main(void)
{
	static struct powervr_render_job job;
	static struct powervr_vk_graphics graphics;
	static struct powervr_vk_descriptor_set set;
	static uint8_t vertex_code[64];
	static uint8_t fragment_code[48];
	static uint8_t set_bytes[32];
	static struct powervr_core core;
	static struct powervr_render render;
	struct powervr_session session;
	uint64_t stream;
	uint64_t ppp[2];
	uint64_t scissors;
	uint32_t word;
	uint32_t at;
	unsigned draw;
	unsigned index;

	/* A 64x64 R8G8B8A8 linear colour cleared, two triangles of three vertices each. */
	memset(&session, 0, sizeof(session));
	render.core = &core;
	layout_stand_in.tiles_x = 4;
	layout_stand_in.tiles_y = 4;
	layout_stand_in.region_header_bytes = 0x2000;
	job.width = 64;
	job.height = 64;
	job.area.width = 64;
	job.area.height = 64;
	job.color_count = 1;
	job.colors[0].surface.format = powervr_format_find(37);
	job.colors[0].surface.address = 0x8800000000ULL;
	job.colors[0].surface.width = 64;
	job.colors[0].surface.height = 64;
	job.colors[0].surface.linear = 1;
	job.colors[0].surface.row_pitch = 256;
	job.colors[0].surface.padded_width = 64;
	job.colors[0].load_op = 1;
	job.colors[0].clear[0] = 0xff0000ffU;
	job.depth.load_op = 2;
	job.depth.store_op = 1;

	/* The pipeline: one attribute of three words, no requests, no varyings. */
	for (index = 0; index < sizeof(vertex_code); index++)
		vertex_code[index] = (uint8_t)(0x10U + index);
	for (index = 0; index < sizeof(fragment_code); index++)
		fragment_code[index] = (uint8_t)(0x80U + index);
	graphics.fixed.topology = 3;
	graphics.fixed.cull_mode = 0;
	graphics.fixed.binding_count = 1;
	graphics.fixed.bindings[0].binding = 0;
	graphics.fixed.bindings[0].stride = 12;
	graphics.fixed.attribute_count = 1;
	graphics.fixed.attributes[0].location = 0;
	graphics.fixed.attributes[0].binding = 0;
	graphics.fixed.attributes[0].format = 106;
	graphics.vertex.length = sizeof(vertex_code);
	graphics.vertex.temps = 4;
	graphics.vertex.vertex_inputs = 3;
	graphics.vertex.vertex_outputs = 4;
	graphics.vertex.input_count = 1;
	graphics.vertex.inputs[0].location = 0;
	graphics.vertex.inputs[0].components = 3;
	graphics.vertex.inputs[0].reg = 0;
	graphics.vertex.vertex_index_reg = POWERVR_SHADER_NO_REG;
	graphics.vertex.instance_index_reg = POWERVR_SHADER_NO_REG;
	graphics.fragment.length = sizeof(fragment_code);
	graphics.fragment.temps = 2;
	graphics.vertex_code = vertex_code;
	graphics.fragment_code = fragment_code;
	graphics.varyings.output_select = 4;
	graphics.fixed.dynamic_viewport = true;
	graphics.fixed.dynamic_scissor = true;

	/* One set of 32 bytes. */
	for (index = 0; index < sizeof(set_bytes); index++)
		set_bytes[index] = (uint8_t)(0xc0U + index);
	set.bytes = set_bytes;
	set.layout.bytes = sizeof(set_bytes);

	/* Two draws: the second with a set, push constants, another scissor. */
	job.draw_count = 2;
	for (draw = 0; draw < 2U; draw++) {
		job.draws[draw].pipeline = &graphics;
		job.draws[draw].vertex_addresses[0] = 0x8900000000ULL + draw * 0x1000U;
		job.draws[draw].count = 3;
		job.draws[draw].instances = 1;
		job.draws[draw].viewport.width = 64.0f;
		job.draws[draw].viewport.height = 64.0f;
		job.draws[draw].viewport.maxDepth = 1.0f;
		job.draws[draw].scissor.extent.width = 64U - draw * 32U;
		job.draws[draw].scissor.extent.height = 64;
	}
	job.draws[1].sets[0] = &set;
	job.draws[1].push_bytes = 8;
	job.draws[1].push[0] = 0x5a;
	job.draws[1].push[7] = 0xa5;

	expect(powervr_render_job_run(&render, &session, &job) == 0, "the job runs");

	/* The geometry's stream: draw 0's PPP block word, its address, ..., then draw 1's, then the end. */
	stream = read64(kicked_geometry, POWERVR_GEOMETRY_STREAM_BASE);
	at = 0;
	for (draw = 0; draw < 2U; draw++) {
		word = device_read(stream + at);
		expect((word >> 29) == 0U, "a draw starts with a PPP state block");
		ppp[draw] = ((uint64_t)(word & 0xffU) << 32) | device_read(stream + at + 4U);
		expect(((word >> 8) & 0x1fffffU) != 0U, "the block has words");
		/* Skips the block's words to the index list's count, 3. */
		while (device_read(stream + at) != 3U && at < 0x200U)
			at += 4U;
		expect(device_read(stream + at) == 3U, "the draw's count");
		at += 4U;
	}
	/* The terminate state's block (three words), then the end. */
	expect((device_read(stream + at) >> 29) == 0U && ((device_read(stream + at) >> 8) & 0xffU) == 3U, "the terminate state's block after the second draw");
	expect(device_read(stream + at + 8U) == POWERVR_VDM_TERMINATE, "the stream ends after the terminate state");
	{
		uint64_t state;

		state = ((uint64_t)(device_read(stream + at) & 0xffU) << 32) | device_read(stream + at + 4U);
		expect(device_read(state) == (1U << 25), "the terminate state's header names the terminate only");
		expect(device_read(state + 8U) == 0U, "the terminate state's render target and left clip are zero");
	}
	expect(ppp[1] - ppp[0] == 0x1000U, "the draws' blocks one slot apart");
	expect(device_read(ppp[0]) != 0U, "draw 0's block is written");

	/* The second draw's push constants and its set's copy, after the two slots. */
	expect((device_read(ppp[1] + 0x100U) & 0xffU) == 0x5aU && (device_read(ppp[1] + 0x104U) >> 24) == 0xa5U, "push constants in draw 1's slot");
	expect(device_read(ppp[0] + 0x2000U) == 0xc3c2c1c0U && device_read(ppp[0] + 0x201cU) == 0xdfdedddcU, "the set's copy after the slots");

	/* The scissor table the fragment command names: one entry a draw, the second half as wide. */
	scissors = read64(kicked_fragment, POWERVR_FRAGMENT_SCISSOR_BASE);
	expect(scissors != 0U && device_read(scissors) != 0U, "the scissor table is named and written");
	expect(device_read(scissors) != device_read(scissors + 8U) || device_read(scissors + 4U) != device_read(scissors + 12U), "the two scissors differ");
	expect(read64(kicked_fragment, POWERVR_FRAGMENT_DBIAS_BASE) == scissors - 0x100U, "the depth bias table before it");

	/* The shaders' code, placed once for the pipeline both draws use: the vertex code at the start of the shading cluster buffer, the fragment's after it. */
	for (index = 0; index < placed_count; index++) {
		if (placed_at[index] >= POWERVR_USC_HEAP_BASE && placed[index]->view[0] == 0x10U && placed[index]->view[64] == 0x80U)
			break;
	}
	expect(index < placed_count, "the pipeline's code once: vertex, then fragment");

	/*
	 * A pipeline with its own viewport and scissor (vkstage, vkdemo): the
	 * draws' are left empty, as nothing sets them, and the job takes the
	 * pipeline's, the same for both draws.
	 */
	graphics.fixed.dynamic_viewport = false;
	graphics.fixed.dynamic_scissor = false;
	graphics.fixed.viewport.width = 64.0f;
	graphics.fixed.viewport.height = 64.0f;
	graphics.fixed.viewport.maxDepth = 1.0f;
	graphics.fixed.scissor.extent.width = 64;
	graphics.fixed.scissor.extent.height = 64;
	for (draw = 0; draw < 2U; draw++) {
		memset(&job.draws[draw].viewport, 0, sizeof(job.draws[draw].viewport));
		memset(&job.draws[draw].scissor, 0, sizeof(job.draws[draw].scissor));
	}
	expect(powervr_render_job_run(&render, &session, &job) == 0, "the job with the pipeline's viewport runs");
	scissors = read64(kicked_fragment, POWERVR_FRAGMENT_SCISSOR_BASE);
	expect(scissors != 0U && device_read(scissors) != 0U, "the pipeline's scissor is written");
	expect(device_read(scissors) == device_read(scissors + 8U) && device_read(scissors + 4U) == device_read(scissors + 12U), "both draws have the pipeline's scissor");

	/*
	 * An attribute of two floats into an input of three: the DMA loads
	 * the two, the third comes from the slot's defaults (0, 0, 0, 1.0).
	 */
	graphics.fixed.attributes[0].format = 103;
	expect(powervr_render_job_run(&render, &session, &job) == 0, "the job of a narrower attribute runs");
	stream = read64(kicked_geometry, POWERVR_GEOMETRY_STREAM_BASE);
	word = device_read(stream);
	ppp[0] = ((uint64_t)(word & 0xffU) << 32) | device_read(stream + 4U);
	expect(device_read(ppp[0] + 0x308U) == 0U && device_read(ppp[0] + 0x30cU) == 0x3f800000U, "the float defaults in the draw's slot");
	expect(device_read(ppp[0] + 0x31cU) == 1U, "the integer defaults after them");
	graphics.fixed.attributes[0].format = 106;

	/* An sRGB colour cleared: two clear words, a job that runs; a 128-bit colour is refused. */
	job.colors[0].surface.format = powervr_format_find(43);
	expect(powervr_render_job_run(&render, &session, &job) == 0, "the job clearing an sRGB colour runs");
	job.colors[0].surface.format = powervr_format_find(109);
	expect(powervr_render_job_run(&render, &session, &job) == ENOTSUP, "a clear of four words is refused");
	job.colors[0].surface.format = powervr_format_find(37);

	/*
	 * A loaded colour: no background load, the job's first draw the load
	 * draw (six vertices) over the framebuffer, sampling the attachment's
	 * view, before the job's own two draws.
	 */
	{
		static struct powervr_image_layout colour_layout;

		memset(&colour_layout, 0, sizeof(colour_layout));
		colour_layout.row_pitch = 256;
		colour_layout.padded_width = 64;
		colour_layout.padded_height = 64;
		colour_layout.layer_bytes = 0x4000;
		job.colors[0].load_op = 0;
		job.colors[0].view.format = powervr_format_find(37);
		job.colors[0].view.layout = &colour_layout;
		job.colors[0].view.address = 0x8800000000ULL;
		job.colors[0].view.width = 64;
		job.colors[0].view.height = 64;
		job.colors[0].view.image_levels = 1;
		job.colors[0].view.level_count = 1;
		job.colors[0].view.layer_count = 1;
		job.colors[0].view.linear = true;
		expect(powervr_render_job_run(&render, &session, &job) == 0, "the job of a loaded colour runs");
		stream = read64(kicked_geometry, POWERVR_GEOMETRY_STREAM_BASE);
		at = 0;
		while (device_read(stream + at) != 6U && at < 0x200U)
			at += 4U;
		expect(device_read(stream + at) == 6U, "the load draw's six vertices come first");
		while (device_read(stream + at) != 3U && at < 0x400U)
			at += 4U;
		expect(device_read(stream + at) == 3U, "then the job's own draws");
		job.colors[0].load_op = 1;
	}

	/* A blit of three rows to a linear destination of an odd row: one job a row. */
	{
		struct powervr_transfer_request blit;
		struct powervr_view_shape source;
		struct powervr_image_layout source_layout;

		memset(&source_layout, 0, sizeof(source_layout));
		source_layout.row_pitch = 64;
		source_layout.padded_width = 16;
		source_layout.padded_height = 16;
		source_layout.layer_bytes = 0x400;
		memset(&source, 0, sizeof(source));
		source.layout = &source_layout;
		source.format = powervr_format_find(37);
		source.address = 0x8a00000000ULL;
		source.width = 16;
		source.height = 16;
		source.image_levels = 1;
		source.level_count = 1;
		source.layer_count = 1;
		memset(&blit, 0, sizeof(blit));
		blit.kind = POWERVR_TRANSFER_BLIT;
		blit.destination.format = powervr_format_find(37);
		blit.destination.address = 0x8b00000000ULL;
		blit.destination.width = 63;
		blit.destination.height = 3;
		blit.destination.linear = 1;
		blit.destination.row_pitch = 63 * 4;
		blit.destination.padded_width = 63;
		blit.area.width = 63;
		blit.area.height = 3;
		blit.source = &source;
		blit.source_area.width = 16;
		blit.source_area.height = 16;
		blit.linear = true;
		transfer_kicks = 0;
		expect(powervr_transfer_run(&render, &session, &blit) == 0, "the blit to odd linear rows runs");
		expect(transfer_kicks == 3U, "one transfer a row");
	}

	printf("job-host-test: %d passed, %d failed\n", passes, failures);
	return failures != 0;
}
