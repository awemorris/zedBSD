/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * A check, in a test kernel, of the core's render path: a session of the
 * driver's own clears a 64x64 linear colour attachment to one colour and
 * draws one triangle of another over it, through its render context and
 * a render target, then reads the attachment back.
 *
 * Every step says one line, so a check that stops shows where: the
 * preparation, the kick, the wait for the fragment's fence with the
 * timelines it read, and the read back with the first pixel that
 * differed.  The TA and 3D stage lines come from the render's end.
 *
 * A job check follows the transfer check on a session of its own: the
 * render job path the Vulkan executor takes (the job runner with the
 * compiler's programs, not this file's fixed ones) clears a 64x64
 * attachment and a D32 depth and draws two triangles in one draw, the
 * nearer first, and reads both back.
 *
 * A transfer check follows on a session of its own: a 64x64 attachment
 * cleared to blue through the transfer context and read back, then
 * copied to a second one and that read back, a line for each step and
 * the transfer stage line from each transfer's end.
 */

#include <stddef.h>
#include <stdint.h>

#include <kern/clock.h>
#include <kern/kcrt.h>
#include <kern/klog.h>
#include <kern/kmem.h>
#include <kern/lock.h>
#include <kern/sched.h>
#include <uapi/errno.h>

#include "drivers/gpu/powervr/render-private.h"
#include "drivers/gpu/powervr/interface.h"
#include "drivers/gpu/powervr/image-state.h"
#include "drivers/gpu/powervr/compiler/pds-encode.h"
#include "drivers/gpu/powervr/render-command.h"
#include "drivers/gpu/powervr/render-kick-layout.h"
#include "drivers/gpu/powervr/render-program.h"
#include "drivers/gpu/powervr/transfer-job.h"
#include "drivers/gpu/powervr/render-job.h"
#include "drivers/gpu/powervr/compiler/shader.h"
#include "drivers/gpu/powervr/vk/vk-draw.h"
#include "userland/tests/vkstage/shader.h"

/*
 * The frame: 64x64 R8G8B8A8 (VK_FORMAT_R8G8B8A8_UNORM 37), linear, rows
 * of 256 bytes; red to clear to and green for the triangle, as the
 * attachment holds them (red in the first byte).
 */
#define CHECK_EDGE		64U
#define CHECK_FORMAT		37U
#define CHECK_ROW_BYTES		(CHECK_EDGE * 4U)
#define CHECK_CLEAR		0xff0000ffU
#define CHECK_TRIANGLE		0xff00ff00U

/* How long the render may take. */
#define CHECK_WAIT_MS		2000U

/*
 * The general buffer: the vertex control stream, the primitive state
 * block, the three vertices, then the attachment.
 */
#define GENERAL_STREAM		0x0U
#define GENERAL_PPP		0x100U
#define GENERAL_TERMINATE_PPP	0x180U
#define GENERAL_VERTICES	0x200U
#define GENERAL_ATTACHMENT	0x1000U
#define GENERAL_BYTES		(GENERAL_ATTACHMENT + CHECK_EDGE * CHECK_ROW_BYTES)

/* How long after the empty kick the geometry registers are read, and how many. */
#define CHECK_PROBE_MS		200U
#define CHECK_PROBE_REGISTERS	26U
#define CHECK_PROBE_CONTEXTS	8U

/*
 * The data sequencer buffer, each program on 16 bytes: the end-of-tile
 * data; the clear's task program (data, then its code after eight
 * words), its shared register program's code and data; the triangle
 * fragment's task program and a shared register program that only ends;
 * the vertex program's data and code.
 */
#define PDS_EVENT		0x000U
#define PDS_CLEAR_KICK		0x040U
#define PDS_CLEAR_SHARED_CODE	0x080U
#define PDS_CLEAR_SHARED_DATA	0x100U
#define PDS_TRIANGLE_KICK	0x200U
#define PDS_NOTHING_CODE	0x240U
#define PDS_NOTHING_DATA	0x250U
#define PDS_VERTEX_DATA		0x300U
#define PDS_VERTEX_CODE		0x400U
#define PDS_BYTES		0x1000U
#define PDS_SHARED_CODE_ROOM	(PDS_CLEAR_SHARED_DATA - PDS_CLEAR_SHARED_CODE)
#define PDS_SHARED_DATA_ROOM	(PDS_TRIANGLE_KICK - PDS_CLEAR_SHARED_DATA)
#define PDS_VERTEX_DATA_ROOM	(PDS_VERTEX_CODE - PDS_VERTEX_DATA)
#define PDS_VERTEX_CODE_ROOM	(PDS_BYTES - PDS_VERTEX_CODE)

/* The shading cluster buffer: the end-of-tile, clear, colour and vertex programs. */
#define USC_EOT			0x000U
#define USC_CLEAR		0x100U
#define USC_COLOUR		0x200U
#define USC_VERTEX		0x300U
#define USC_BYTES		0x1000U

_Static_assert(POWERVR_RENDER_USC_MAX <= 0x100U, "shading cluster program room");

/* What the check holds while it runs. */
struct render_check {
	struct powervr_render *render;
	struct powervr_session *session;
	struct powervr_render_context *context;
	struct powervr_target *target;
	struct powervr_resource *general;
	struct powervr_buffer *pds;
	struct powervr_buffer *usc;
	uint64_t pds_place;
	uint64_t usc_place;
	struct powervr_fragment_setup fragment_setup;
	struct powervr_geometry_setup geometry_setup;
	struct powervr_render_usc program;
	struct powervr_render_kick kick;
	struct powervr_data_shared_program shared;
	struct powervr_data_vertex_program vertex;
	uint8_t geometry[POWERVR_GEOMETRY_BYTES];
	uint8_t fragment[POWERVR_FRAGMENT_BYTES];
	struct powervr_render_marks marks;
};

/* The check's state; the node's start runs it once, alone. */
static struct render_check check_state;

static int check_prepare(struct render_check *check, const char **step);
static int check_programs(struct render_check *check, const char **step);
static int check_triangle(struct render_check *check, uint32_t pds_base, uint32_t usc_base);
static int check_empty(struct render_check *check);
static unsigned get32(const uint8_t *bytes, unsigned offset);
static void dump_object(const char *name, const struct powervr_object *object, uint32_t bytes);
static void dump_context(struct render_check *check);
static void probe_geometry(struct powervr_core *core);
static int check_kick(struct render_check *check);
static int check_read(struct render_check *check);
static void check_release(struct render_check *check);
static int pixel_class(uint32_t x, uint32_t y);
static void transfer_check(struct powervr_render *render, struct powervr_session **kept, struct powervr_resource **kept_memory);
static void transfer_close(struct powervr_render *render, struct powervr_session *session, struct powervr_resource *memory);
static void job_check(struct powervr_render *render);
static int job_compile(struct powervr_vk_graphics *graphics);
static int job_read(const struct powervr_buffer *colour, const struct powervr_buffer *depth);
static int transfer_read(struct powervr_buffer *buffer, uint32_t offset, const struct powervr_buffer *against, uint32_t against_offset, uint32_t value, const char *what);
static void put_words(struct powervr_buffer *buffer, uint32_t offset, const uint32_t *words, unsigned count);
static void put_bytes(struct powervr_buffer *buffer, uint32_t offset, const uint8_t *bytes, size_t count);
static uint32_t get_word(const struct powervr_buffer *buffer, uint32_t offset);

/*
 * Runs the driver's own render on the core and reports how it went.
 */
void
powervr_render_check(
	struct powervr_render *render)
{
	struct render_check *check;
	struct powervr_session *kept;
	struct powervr_resource *kept_memory;
	const char *step;
	int error;

	/*
	 * A transfer on a session of its own before the render, kept open
	 * through the render so its memory context holds the page catalogue
	 * set it was given and the render's context has to take another.
	 */
	kern_logf("powervr: render check: transfer before the render (session kept open)\n");
	kept = NULL;
	kept_memory = NULL;
	transfer_check(render, &kept, &kept_memory);

	/* The session, its render context and target, the memory and the programs. */
	check = &check_state;
	kern_memset(check, 0, sizeof(*check));
	check->render = render;
	step = "open";
	error = check_prepare(check, &step);
	if (error == 0)
		error = check_programs(check, &step);
	if (error != 0) {
		kern_logf("powervr: render check: FAILED at %s (%d)\n", step, error);
		check_release(check);
		transfer_close(render, kept, kept_memory);
		return;
	}
	kern_logf("powervr: render check: prepared (64x64 R8G8B8A8, clear and one triangle)\n");

	/* A render of an empty stream first: whether the geometry master resets on its end alone. */
	error = check_empty(check);

	/* The full trace ends the check here, so no later dump pushes it out of the message ring. */
	if (POWERVR_RENDER_CHECK_FULL_TRACE) {
		check_release(check);
		transfer_close(render, kept, kept_memory);
		kern_logf("powervr: render check: full trace taken; the rest of the check is left out\n");
		return;
	}

	/* The kick and the wait for the fragment, then the read back; each says its own line. */
	if (error == 0)
		error = check_kick(check);
	if (error == 0)
		(void)check_read(check);

	/* What the core may still run is kept with the session; otherwise everything goes. */
	check_release(check);
	transfer_close(render, kept, kept_memory);

	/*
	 * A transfer on a fresh session after the render: whether it takes
	 * the page catalogue set the render's context let go and still runs
	 * on it.  Then the job check, on a session of its own.
	 */
	kern_logf("powervr: render check: transfer after the render\n");
	transfer_check(render, NULL, NULL);
	job_check(render);
}

/* Opens the check's session, makes its render context and target, and maps its memory. */
static int
check_prepare(
	struct render_check *check,
	const char **step)
{
	struct powervr_target_shape shape;
	struct powervr_buffer *buffer;
	void *opaque;
	int error;

	/* A session of the check's own. */
	*step = "open";
	error = check->render->ops.open(check->render, &opaque);
	if (error != 0)
		return error;
	check->session = opaque;

	/* Its render context. */
	*step = "render context";
	error = powervr_session_render_context(check->render, check->session, &check->context);
	if (error != 0)
		return error;

	/* A render target of the frame, one sample, one layer. */
	*step = "target";
	shape.width = CHECK_EDGE;
	shape.height = CHECK_EDGE;
	shape.samples = 1;
	shape.layers = 1;
	error = powervr_session_target(check->render, check->session, &shape, &check->target);
	if (error != 0)
		return error;

	/* The general memory, mapped in the session's general window. */
	*step = "memory";
	error = powervr_buffer_create(check->render, GENERAL_BYTES, &buffer);
	if (error != 0)
		return error;

	error = powervr_resource_make(check->session, buffer, 1, 0, &check->general);
	powervr_buffer_put(check->render, buffer);
	if (error != 0) {
		check->general = NULL;
		return error;
	}

	/* The program buffers, mapped in their windows. */
	error = powervr_buffer_create(check->render, PDS_BYTES, &check->pds);
	if (error != 0) {
		check->pds = NULL;
		return error;
	}

	error = powervr_session_program_map(check->session, POWERVR_PROGRAM_PDS, check->pds, &check->pds_place);
	if (error != 0) {
		check->pds_place = 0;
		return error;
	}

	error = powervr_buffer_create(check->render, USC_BYTES, &check->usc);
	if (error != 0) {
		check->usc = NULL;
		return error;
	}

	error = powervr_session_program_map(check->session, POWERVR_PROGRAM_USC, check->usc, &check->usc_place);
	if (error != 0) {
		check->usc_place = 0;
		return error;
	}

	/* Succeeded: the check can write its programs. */
	return 0;
}

/*
 * Writes the render's programs and streams and fills its commands: the
 * end-of-tile program that stores to the attachment, the clear
 * background, the triangle, and the geometry and fragment commands.
 */
static int
check_programs(
	struct render_check *check,
	const char **step)
{
	struct powervr_render_surface surface;
	struct powervr_render_area area;
	struct powervr_render_background background;
	const struct powervr_target_layout *layout;
	uint32_t state[2];
	uint32_t event[POWERVR_RENDER_EVENT_DATA_WORDS];
	uint32_t pds_base;
	uint32_t usc_base;
	uint32_t code_word;
	uint64_t general;
	int error;

	/* The places as offsets from the windows' bases. */
	pds_base = (uint32_t)(check->pds_place - POWERVR_PDS_HEAP_BASE);
	usc_base = (uint32_t)(check->usc_place - POWERVR_USC_HEAP_BASE);
	general = check->general->address;

	/* The attachment's back end words, and the end-of-tile program that emits them. */
	*step = "end of tile";
	surface.format = powervr_format_find(CHECK_FORMAT);
	surface.address = general + GENERAL_ATTACHMENT;
	surface.width = CHECK_EDGE;
	surface.height = CHECK_EDGE;
	surface.linear = 1;
	surface.row_pitch = CHECK_ROW_BYTES;
	surface.padded_width = CHECK_EDGE;
	area.x = 0;
	area.y = 0;
	area.width = CHECK_EDGE;
	area.height = CHECK_EDGE;
	error = powervr_render_pbe_words(&surface, &area, state, check->fragment_setup.pbe);
	if (error != 0)
		return error;

	error = powervr_render_eot_usc(state, &check->program);
	if (error != 0)
		return error;

	put_bytes(check->usc, USC_EOT, check->program.code, check->program.length);
	error = powervr_render_event_data(usc_base + USC_EOT, check->program.temps, event);
	if (error != 0)
		return error;

	put_words(check->pds, PDS_EVENT, event, POWERVR_RENDER_EVENT_DATA_WORDS);
	error = powervr_render_event_words(pds_base + PDS_EVENT, &code_word, &check->fragment_setup.event_data, &check->fragment_setup.event_info);
	if (error != 0)
		return error;

	/* The clear background: its program, the task program, the shared register program with the colour. */
	*step = "clear";
	error = powervr_render_clear_usc(&check->program);
	if (error != 0)
		return error;

	put_bytes(check->usc, USC_CLEAR, check->program.code, check->program.length);
	error = powervr_render_kick_build(usc_base + USC_CLEAR, check->program.temps, &check->kick);
	if (error != 0)
		return error;

	put_words(check->pds, PDS_CLEAR_KICK, check->kick.data, POWERVR_RENDER_KICK_DATA_WORDS);
	put_words(check->pds, PDS_CLEAR_KICK + POWERVR_RENDER_KICK_DATA_WORDS * 4U, check->kick.code, POWERVR_RENDER_KICK_CODE_WORDS);
	error = powervr_render_clear_shared(CHECK_CLEAR, &check->shared);
	if (error != 0)
		return error;

	if (check->shared.code_words * 4U > PDS_SHARED_CODE_ROOM || check->shared.data_words * 4U > PDS_SHARED_DATA_ROOM)
		return E2BIG;

	put_words(check->pds, PDS_CLEAR_SHARED_CODE, check->shared.code, check->shared.code_words);
	put_words(check->pds, PDS_CLEAR_SHARED_DATA, check->shared.data, check->shared.data_words);
	background.kick_offset = pds_base + PDS_CLEAR_KICK;
	background.shared_code_offset = pds_base + PDS_CLEAR_SHARED_CODE;
	background.shared_data_offset = pds_base + PDS_CLEAR_SHARED_DATA;
	background.shared_data_words = check->shared.data_words;
	background.shared_registers = 1;
	error = powervr_render_background_words(&background, check->fragment_setup.background);
	if (error != 0)
		return error;

	/* After a partial render the same clear runs again; the frame is small enough not to need one. */
	kern_memcpy(check->fragment_setup.partial_background, check->fragment_setup.background, sizeof(check->fragment_setup.background));

	/* The triangle's programs and streams. */
	*step = "triangle";
	error = check_triangle(check, pds_base, usc_base);
	if (error != 0)
		return error;

	/* The commands: the fragment's registers, the geometry's with the target's region header stride. */
	*step = "commands";
	layout = powervr_target_layout_of(check->target);
	check->fragment_setup.tiles_x = layout->tiles_x;
	check->fragment_setup.tiles_y = layout->tiles_y;
	check->fragment_setup.border_table = powervr_session_border_table(check->session);
	check->geometry_setup.border_table = check->fragment_setup.border_table;
	kern_memset(check->fragment, 0, sizeof(check->fragment));
	powervr_fragment_command(&check->fragment_setup, check->fragment);
	check->geometry_setup.stream = general + GENERAL_STREAM;
	check->geometry_setup.region_header_bytes = layout->region_header_bytes;
	check->geometry_setup.shared_registers = 1;
	kern_memset(check->geometry, 0, sizeof(check->geometry));
	powervr_geometry_command(&check->geometry_setup, check->geometry);

	/* Succeeded: the render is ready to kick. */
	return 0;
}

/*
 * Writes the triangle: its fragment program and task program, a shared
 * register program that only ends, its vertex program and data
 * sequencer program, its three vertices (x and y in pixels as 32-bit
 * floats: (8, 8), (56, 8), (32, 56)), its primitive state block and its
 * vertex control stream.
 */
static int
check_triangle(
	struct render_check *check,
	uint32_t pds_base,
	uint32_t usc_base)
{
	static const uint32_t vertices[3U * POWERVR_RENDER_VERTEX_WORDS] = {
		0x41000000U, 0x41000000U, 0,
		0x42600000U, 0x41000000U, 0,
		0x42000000U, 0x42600000U, 0,
	};
	struct powervr_render_triangle triangle;
	uint32_t ppp[POWERVR_RENDER_PPP_WORDS];
	uint32_t vdm[POWERVR_RENDER_VDM_WORDS];
	uint32_t terminate[POWERVR_RENDER_TERMINATE_STATE_WORDS];
	uint32_t halt;
	uint64_t general;
	int error;

	/* The fragment program and its task program. */
	general = check->general->address;
	error = powervr_render_colour_usc(CHECK_TRIANGLE, &check->program);
	if (error != 0)
		return error;

	put_bytes(check->usc, USC_COLOUR, check->program.code, check->program.length);
	error = powervr_render_kick_build(usc_base + USC_COLOUR, check->program.temps, &check->kick);
	if (error != 0)
		return error;

	put_words(check->pds, PDS_TRIANGLE_KICK, check->kick.data, POWERVR_RENDER_KICK_DATA_WORDS);
	put_words(check->pds, PDS_TRIANGLE_KICK + POWERVR_RENDER_KICK_DATA_WORDS * 4U, check->kick.code, POWERVR_RENDER_KICK_CODE_WORDS);

	/* A shared register program with nothing to load: one halt. */
	error = powervr_pds_halt(0, &halt);
	if (error != 0)
		return error;

	put_words(check->pds, PDS_NOTHING_CODE, &halt, 1);

	/* The vertex program and its data sequencer program over the three vertices. */
	error = powervr_render_vertex_usc(&check->program);
	if (error != 0)
		return error;

	put_bytes(check->usc, USC_VERTEX, check->program.code, check->program.length);
	error = powervr_render_vertex_pds(general + GENERAL_VERTICES, usc_base + USC_VERTEX, check->program.temps, &check->vertex);
	if (error != 0)
		return error;

	if (check->vertex.data_words * 4U > PDS_VERTEX_DATA_ROOM || check->vertex.code_words * 4U > PDS_VERTEX_CODE_ROOM)
		return E2BIG;

	put_words(check->pds, PDS_VERTEX_DATA, check->vertex.data, check->vertex.data_words);
	put_words(check->pds, PDS_VERTEX_CODE, check->vertex.code, check->vertex.code_words);
	put_words(check->general->buffer, GENERAL_VERTICES, vertices, 3U * POWERVR_RENDER_VERTEX_WORDS);

	/* The render's terminate state, which every stream names before its end. */
	powervr_render_terminate_state(CHECK_EDGE, CHECK_EDGE, terminate);
	put_words(check->general->buffer, GENERAL_TERMINATE_PPP, terminate, POWERVR_RENDER_TERMINATE_STATE_WORDS);

	/* The primitive state block and the stream that names it and draws. */
	triangle.ppp_address = general + GENERAL_PPP;
	triangle.terminate_address = general + GENERAL_TERMINATE_PPP;
	triangle.fragment.kick_offset = pds_base + PDS_TRIANGLE_KICK;
	triangle.fragment.shared_code_offset = pds_base + PDS_NOTHING_CODE;
	triangle.fragment.shared_data_offset = pds_base + PDS_NOTHING_DATA;
	triangle.fragment.shared_data_words = 0;
	triangle.fragment.shared_registers = 0;
	triangle.vertex_data_offset = pds_base + PDS_VERTEX_DATA;
	triangle.vertex_code_offset = pds_base + PDS_VERTEX_CODE;
	triangle.vertex_data_words = check->vertex.data_words;
	triangle.vertex_temps = check->vertex.temps;
	error = powervr_render_triangle_ppp(&triangle, ppp);
	if (error != 0)
		return error;

	error = powervr_render_triangle_vdm(&triangle, vdm);
	if (error != 0)
		return error;

	put_words(check->general->buffer, GENERAL_PPP, ppp, POWERVR_RENDER_PPP_WORDS);
	put_words(check->general->buffer, GENERAL_STREAM, vdm, POWERVR_RENDER_VDM_WORDS);

	/* Succeeded: the triangle is written. */
	return 0;
}

/*
 * Renders once with a vertex control stream of its end alone, the
 * triangle's stream put back after: says whether the geometry master
 * finished it and whether the firmware reset the context, with the
 * firmware's dump when it did.  EIO when the context was reset, so the
 * triangle is not kicked behind it.
 */
static int
check_empty(
	struct render_check *check)
{
	uint32_t saved[POWERVR_RENDER_VDM_WORDS];
	uint32_t stream[POWERVR_RENDER_TERMINATE_STREAM_WORDS];
	unsigned index;
	int error;

	/*
	 * In place of the triangle's stream, the stream's end alone as Mesa
	 * ends a render: the terminate state's block, then the end (the
	 * state was written with the triangle).
	 */
	for (index = 0; index < POWERVR_RENDER_VDM_WORDS; index++)
		saved[index] = get_word(check->general->buffer, GENERAL_STREAM + index * 4U);
	error = powervr_render_terminate_stream(check->general->address + GENERAL_TERMINATE_PPP, stream);
	if (error != 0)
		return error;
	put_words(check->general->buffer, GENERAL_STREAM, stream, POWERVR_RENDER_TERMINATE_STREAM_WORDS);

	/* The kick and the wait, as the triangle's. */
	powervr_firmware_trace_mark(check->render->core);
	mutex_lock(&check->render->core->lock);

	error = powervr_session_kick_render(check->render, check->session, check->target, 0, check->geometry, check->fragment, check->fragment, NULL, 0, &check->marks);

	mutex_unlock(&check->render->core->lock);

	/* The core's geometry registers a little after the kick. */
	if (error == 0 && POWERVR_DIAGNOSTICS && !POWERVR_RENDER_CHECK_FULL_TRACE) {
		sched_sleep(sched_ticks() + kern_ms_to_ticks(CHECK_PROBE_MS));
		probe_geometry(check->render->core);
	}

	if (error == 0) {
		error = powervr_render_finish(check->render, check->session, &check->marks, CHECK_WAIT_MS);
		if (POWERVR_DIAGNOSTICS && !POWERVR_RENDER_CHECK_FULL_TRACE) {
			kern_logf("powervr: render check: registers after the wait (%d):\n", error);
			probe_geometry(check->render->core);
		}
	}

	kern_logf("powervr: render check: %s stream: %s (%d), context %s\n", "terminate-state",
		  error == 0 ? "finished" : "FAILED", error,
		  check->session->lost ? "reset" : "kept");

	/* The triangle's stream back. */
	put_words(check->general->buffer, GENERAL_STREAM, saved, POWERVR_RENDER_VDM_WORDS);

	/* A reset or a failure: the dump, and no triangle behind it. */
	if (error == 0 && check->session->lost)
		error = EIO;
	if (error != 0) {
		powervr_firmware_dump(check->render->core, POWERVR_KERNEL_CCB_SLOTS);

		/* The diagnostics: the target, the free list and the context, unless the full trace says it all. */
		if (POWERVR_DIAGNOSTICS && !POWERVR_RENDER_CHECK_FULL_TRACE) {
			powervr_target_dump(check->target);
			powervr_session_global_list_dump(check->session);
			dump_context(check);
		}

		/* The diagnostics: the geometry command's words. */
		if (POWERVR_DIAGNOSTICS) {
			kern_logf("powervr: render check: geometry command: %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x\n",
			  get32(check->geometry, 0), get32(check->geometry, 4), get32(check->geometry, 8), get32(check->geometry, 12),
			  get32(check->geometry, 16), get32(check->geometry, 20), get32(check->geometry, 24), get32(check->geometry, 28),
			  get32(check->geometry, 32), get32(check->geometry, 36), get32(check->geometry, 40), get32(check->geometry, 44),
			  get32(check->geometry, 48), get32(check->geometry, 52), get32(check->geometry, 56), get32(check->geometry, 60),
			  get32(check->geometry, 64), get32(check->geometry, 68), get32(check->geometry, 72), get32(check->geometry, 76),
			  get32(check->geometry, 80), get32(check->geometry, 84), get32(check->geometry, 88), get32(check->geometry, 92),
			  get32(check->geometry, 96), get32(check->geometry, 100), get32(check->geometry, 104), get32(check->geometry, 108));
		}

		/* A timed-out render leaves the session to the core. */
		if (error == ETIMEDOUT)
			check->session->abandoned = 1;

		/* Reports the reset or the failure. */
		return error;
	}

	/* Succeeded: the stream finished with the context kept. */
	return 0;
}

/*
 * Kicks the render and waits for its fragment, saying each step: the
 * kick with its timeline values, then the timelines the wait read.
 */
static int
check_kick(
	struct render_check *check)
{
	struct powervr_render_context *context;
	uint32_t geometry_now;
	uint32_t fragment_now;
	int error;

	/* The kicks, under the core's lock; the partial render runs the fragment's own command. */
	context = check->context;
	powervr_firmware_trace_mark(check->render->core);
	mutex_lock(&check->render->core->lock);

	error = powervr_session_kick_render(check->render, check->session, check->target, 0, check->geometry, check->fragment, check->fragment, NULL, 0, &check->marks);

	mutex_unlock(&check->render->core->lock);

	if (error != 0) {
		kern_logf("powervr: render check: FAILED at kick (%d)\n", error);
		return error;
	}
	kern_logf("powervr: render check: kicked (geometry to %u, fragment to %u)\n", check->marks.geometry_value, check->marks.fragment_value);

	/* The wait; the render's end says the TA and 3D stages. */
	error = powervr_render_finish(check->render, check->session, &check->marks, CHECK_WAIT_MS);
	geometry_now = powervr_object_get32(&context->geometry.timeline, POWERVR_SYNC_ADDRESS);
	fragment_now = powervr_object_get32(&context->fragment.timeline, POWERVR_SYNC_ADDRESS);
	if (error != 0) {
		kern_logf("powervr: render check: FAILED at fence (%d; geometry %u of %u, fragment %u of %u)\n", error, geometry_now, check->marks.geometry_value, fragment_now, check->marks.fragment_value);
		powervr_firmware_dump(check->render->core, POWERVR_KERNEL_CCB_SLOTS);
		check->session->abandoned = 1;
		return error;
	}

	kern_logf("powervr: render check: fence reached (geometry %u, fragment %u), context %s\n", geometry_now, fragment_now,
		  check->session->lost ? "reset" : "kept");

	/* A context the firmware reset: the dump, and with the diagnostics the target and the list. */
	if (check->session->lost) {
		powervr_firmware_dump(check->render->core, POWERVR_KERNEL_CCB_SLOTS);

		/* The diagnostics: the target and the free list. */
		if (POWERVR_DIAGNOSTICS) {
			powervr_target_dump(check->target);
			powervr_session_global_list_dump(check->session);
		}
	}

	/* Succeeded: the render finished. */
	return 0;
}

/*
 * Reads the attachment back: pixels well inside the triangle hold its
 * colour, pixels well outside the clear colour; those near an edge are
 * not judged.  Says the first pixel that differed.
 */
static int
check_read(
	struct render_check *check)
{
	struct powervr_buffer *buffer;
	uint32_t want;
	uint32_t got;
	uint32_t x;
	uint32_t y;
	unsigned judged;
	int inside;

	/* Every pixel the triangle's edges leave clear of doubt, row by row. */
	buffer = check->general->buffer;
	judged = 0;
	for (y = 0; y < CHECK_EDGE; y++) {
		for (x = 0; x < CHECK_EDGE; x++) {
			inside = pixel_class(x, y);
			if (inside < 0)
				continue;

			/* The colour the pixel should hold, and the one it holds. */
			want = CHECK_CLEAR;
			if (inside)
				want = CHECK_TRIANGLE;
			got = get_word(buffer, GENERAL_ATTACHMENT + y * CHECK_ROW_BYTES + x * 4U);
			judged++;
			if (got != want) {
				kern_logf("powervr: render check: FAILED at read back: pixel (%u, %u) is 0x%08x, want 0x%08x\n", x, y, got, want);
				return EIO;
			}
		}
	}

	/* Succeeded: every judged pixel holds what it should. */
	kern_logf("powervr: render check: OK (%u pixels judged)\n", judged);
	return 0;
}

/* Unmaps and returns the check's memory and closes its session, unless the core may still run it. */
static void
check_release(
	struct render_check *check)
{
	struct powervr_render *render;

	/* A render that ran out of time may still run: its memory stays held, and the close keeps the session. */
	render = check->render;
	if (check->session != NULL && check->session->abandoned) {
		render->ops.close(render, check->session);
		return;
	}

	/* The program buffers and the general memory. */
	if (check->usc != NULL) {
		if (check->usc_place != 0U)
			powervr_session_program_unmap(check->session, POWERVR_PROGRAM_USC, check->usc, check->usc_place);
		powervr_buffer_put(render, check->usc);
	}

	if (check->pds != NULL) {
		if (check->pds_place != 0U)
			powervr_session_program_unmap(check->session, POWERVR_PROGRAM_PDS, check->pds, check->pds_place);
		powervr_buffer_put(render, check->pds);
	}

	if (check->general != NULL)
		powervr_resource_unmake(check->session, check->general);

	/* The session, whose close returns the context and the target. */
	if (check->session != NULL)
		render->ops.close(render, check->session);
}

/*
 * Says whether a pixel's centre lies inside the triangle (1), outside it
 * (0), or within about a pixel of an edge (-1).  The vertices are (8, 8),
 * (56, 8) and (32, 56); each edge's function is taken at twice the
 * pixel grid and compared against a margin of the edge's length.
 */
static int
pixel_class(
	uint32_t x,
	uint32_t y)
{
	static const int32_t corner[3][2] = { { 8, 8 }, { 56, 8 }, { 32, 56 } };
	int32_t px;
	int32_t py;
	int32_t ex;
	int32_t ey;
	int32_t side;
	int32_t margin;
	unsigned edge;
	int inside;

	/* The centre at twice the grid. */
	px = (int32_t)x * 2 + 1;
	py = (int32_t)y * 2 + 1;
	inside = 1;

	/* Each edge: the signed area against the edge's length, a pixel's margin either way. */
	for (edge = 0; edge < 3U; edge++) {
		ex = corner[(edge + 1U) % 3U][0] - corner[edge][0];
		ey = corner[(edge + 1U) % 3U][1] - corner[edge][1];
		side = ex * (py - corner[edge][1] * 2) - ey * (px - corner[edge][0] * 2);
		margin = 4 * ((ex < 0 ? -ex : ex) + (ey < 0 ? -ey : ey));
		if (side > -margin && side < margin)
			return -1;
		if (side < 0)
			inside = 0;
	}

	/* Succeeded: inside or outside. */
	return inside;
}

/*
 * The job check: one render job of the executor's path on a session of
 * its own, a clear to blue and a D32 depth cleared to 1.0, and one draw
 * of two triangles with the depth test: a red one at depth 0.25 first,
 * then a larger green one at 0.75 behind it.  The colour and the depth
 * are read back at a few pixels.
 *
 * Procedure referenced: zedBSD userland/tests/vkstage/main.c (stage_depth:
 * the shaders, a D32 depth cleared to 1.0, the nearer drawn first).
 */
static void
job_check(
	struct powervr_render *render)
{
	/* Two triangles of seven floats a vertex: x, y, z, then red, green, blue, alpha. */
	static const uint32_t vertices[6U * 7U] = {
		0xbf000000U, 0xbf000000U, 0x3e800000U, 0x3f800000U, 0, 0, 0x3f800000U,
		0x3f000000U, 0xbf000000U, 0x3e800000U, 0x3f800000U, 0, 0, 0x3f800000U,
		0, 0x3f000000U, 0x3e800000U, 0x3f800000U, 0, 0, 0x3f800000U,
		0xbf666666U, 0xbf666666U, 0x3f400000U, 0, 0x3f800000U, 0, 0x3f800000U,
		0x3f666666U, 0xbf666666U, 0x3f400000U, 0, 0x3f800000U, 0, 0x3f800000U,
		0, 0x3f666666U, 0x3f400000U, 0, 0x3f800000U, 0, 0x3f800000U,
	};
	struct powervr_vk_graphics *graphics;
	struct powervr_render_job *job;
	struct powervr_session *session;
	struct powervr_resource *memory;
	struct powervr_buffer *buffer;
	uint64_t colour;
	uint64_t depth;
	uint64_t vertex;
	void *opaque;
	int error;

	/* The pipeline: the two shaders compiled with their keys, the fixed state. */
	session = NULL;
	memory = NULL;
	graphics = kern_calloc(1, sizeof(*graphics));
	job = kern_calloc(1, sizeof(*job));
	error = 0;
	if (graphics == NULL || job == NULL)
		error = ENOMEM;
	if (error == 0)
		error = job_compile(graphics);
	if (error != 0) {
		kern_logf("powervr: job check: FAILED at compile (%d)\n", error);
		goto done;
	}

	/* A session; its memory: the colour, the depth, the vertices. */
	error = render->ops.open(render, &opaque);
	if (error == 0) {
		session = opaque;
		error = powervr_buffer_create(render, CHECK_EDGE * CHECK_ROW_BYTES * 2U + 0x1000U, &buffer);
	}
	if (error == 0) {
		error = powervr_resource_make(session, buffer, 1, 0, &memory);
		powervr_buffer_put(render, buffer);
		if (error != 0)
			memory = NULL;
	}
	if (error != 0) {
		kern_logf("powervr: job check: FAILED at prepare (%d)\n", error);
		goto done;
	}
	colour = memory->address;
	depth = colour + CHECK_EDGE * CHECK_ROW_BYTES;
	vertex = depth + CHECK_EDGE * CHECK_ROW_BYTES;
	put_words(memory->buffer, (uint32_t)(vertex - colour), vertices, 6U * 7U);

	/* The job: the colour cleared to blue and stored, the depth cleared to 1.0 and stored, one draw. */
	job->area.width = CHECK_EDGE;
	job->area.height = CHECK_EDGE;
	job->width = CHECK_EDGE;
	job->height = CHECK_EDGE;
	job->color_count = 1;
	job->colors[0].surface.format = powervr_format_find(CHECK_FORMAT);
	job->colors[0].surface.address = colour;
	job->colors[0].surface.width = CHECK_EDGE;
	job->colors[0].surface.height = CHECK_EDGE;
	job->colors[0].surface.linear = 1;
	job->colors[0].surface.row_pitch = CHECK_ROW_BYTES;
	job->colors[0].surface.padded_width = CHECK_EDGE;
	job->colors[0].load_op = 1;
	job->colors[0].clear[0] = 0xffff0000U;
	job->depth.present = 1;
	job->depth.format = powervr_format_find(126);
	job->depth.address = depth;
	job->depth.width = CHECK_EDGE;
	job->depth.height = CHECK_EDGE;
	job->depth.physical_width = CHECK_EDGE;
	job->depth.physical_height = CHECK_EDGE;
	job->depth.layer_bytes = CHECK_EDGE * CHECK_ROW_BYTES;
	job->depth.load_op = 1;
	job->depth.store_op = 0;
	job->depth.clear_depth = 0x3f800000U;
	job->draw_count = 1;
	job->draws[0].pipeline = graphics;
	job->draws[0].vertex_addresses[0] = vertex;
	job->draws[0].count = 6;
	job->draws[0].instances = 1;
	job->draws[0].viewport.width = 64.0f;
	job->draws[0].viewport.height = 64.0f;
	job->draws[0].viewport.maxDepth = 1.0f;
	job->draws[0].scissor.extent.width = CHECK_EDGE;
	job->draws[0].scissor.extent.height = CHECK_EDGE;
	kern_logf("powervr: job check: prepared (64x64 R8G8B8A8 and D32, one draw of two triangles, %u and %u bytes of shader)\n", (unsigned)graphics->vertex.length, (unsigned)graphics->fragment.length);

	/* The job: its kick, then its fence (the job runs on after the kick). */
	error = powervr_render_job_run(render, session, job);
	if (error == 0) {
		error = powervr_session_point_wait(session, session->last_kind, session->last_value, POWERVR_PROGRESS_MS);
		if (error == 0)
			powervr_render_jobs_reap(render, session, 0);
	}
	if (error == ETIMEDOUT)
		session->abandoned = 1;
	if (error == ETIMEDOUT) {
		kern_logf("powervr: job check: FAILED at fence (%d)\n", error);
		goto done;
	}
	if (error != 0) {
		kern_logf("powervr: job check: FAILED at kick (%d)\n", error);
		goto done;
	}
	kern_logf("powervr: job check: kicked, fence reached\n");

	/* The read back. */
	(void)job_read(memory->buffer, memory->buffer);

done:
	/* A job the core may still run keeps its memory with the session. */
	if (memory != NULL && !session->abandoned)
		powervr_resource_unmake(session, memory);
	if (session != NULL)
		render->ops.close(render, session);
	if (graphics != NULL) {
		kern_free(graphics->vertex_code);
		kern_free(graphics->fragment_code);
	}
	kern_free(graphics);
	kern_free(job);
}

/*
 * Compiles the job check's two shaders with their keys and fills the
 * fixed state: a triangle list, no culling, the depth test LESS with
 * writes, one binding of 28 bytes with the position (R32G32B32_SFLOAT
 * 106) and the colour (R32G32B32A32_SFLOAT 109), one R8G8B8A8 output.
 */
static int
job_compile(
	struct powervr_vk_graphics *graphics)
{
	struct powervr_shader_workspace *workspace;
	struct powervr_draw_fixed *fixed;
	int error;

	/* The fixed state and the keys. */
	fixed = &graphics->fixed;
	fixed->topology = 3;
	fixed->depth_test = 1;
	fixed->depth_write = 1;
	fixed->depth_compare = 1;
	fixed->viewport.width = 64.0f;
	fixed->viewport.height = 64.0f;
	fixed->viewport.maxDepth = 1.0f;
	fixed->scissor.extent.width = CHECK_EDGE;
	fixed->scissor.extent.height = CHECK_EDGE;
	fixed->binding_count = 1;
	fixed->bindings[0].stride = 28;
	fixed->attribute_count = 2;
	fixed->attributes[0].location = 0;
	fixed->attributes[0].format = 106;
	fixed->attributes[1].location = 1;
	fixed->attributes[1].format = 109;
	fixed->attributes[1].offset = 12;
	fixed->vertex_key.attribute_formats[0] = 106;
	fixed->vertex_key.attribute_formats[1] = 109;
	fixed->vertex_key.samples = 1;
	fixed->fragment_key.output_formats[0] = CHECK_FORMAT;
	fixed->fragment_key.blend[0].write_mask = 0xf;
	fixed->fragment_key.samples = 1;

	/* The compiler's memory and the code's. */
	workspace = kern_malloc(powervr_shader_workspace_size());
	graphics->vertex_code = kern_malloc(0x4000);
	graphics->fragment_code = kern_malloc(0x4000);
	if (workspace == NULL || graphics->vertex_code == NULL || graphics->fragment_code == NULL) {
		kern_free(workspace);
		return ENOMEM;
	}

	/* The vertex shader, the fragment shader, the varyings between them. */
	error = powervr_shader_compile(vkstage_colour_vert, sizeof(vkstage_colour_vert) / 4U, POWERVR_SHADER_VERTEX, &fixed->vertex_key, workspace, &graphics->vertex, graphics->vertex_code, 0x4000);
	if (error == 0)
		error = powervr_shader_compile(vkstage_colour_frag, sizeof(vkstage_colour_frag) / 4U, POWERVR_SHADER_FRAGMENT, &fixed->fragment_key, workspace, &graphics->fragment, graphics->fragment_code, 0x4000);
	if (error == 0)
		error = powervr_draw_varyings_build(&graphics->vertex, &graphics->fragment, &graphics->varyings);
	kern_free(workspace);
	if (error != 0)
		return error;

	/* Succeeded: the pipeline. */
	return 0;
}

/*
 * Reads the job check's colour and depth back at four pixels: inside the
 * red triangle, inside the green one only (twice), and in the corner the
 * clear leaves.  The depth is read as rows of 32-bit floats; the layout
 * a linear depth store writes is not confirmed on the hardware.
 */
static int
job_read(
	const struct powervr_buffer *colour,
	const struct powervr_buffer *depth)
{
	static const uint32_t at[4][4] = {
		{ 32, 20, 0xff0000ffU, 0x3e800000U },
		{ 32, 54, 0xff00ff00U, 0x3f400000U },
		{ 32, 10, 0xff00ff00U, 0x3f400000U },
		{ 2, 2, 0xffff0000U, 0x3f800000U },
	};
	uint32_t got;
	uint32_t offset;
	unsigned index;
	int depth_bad;

	/* The colours first. */
	for (index = 0; index < 4U; index++) {
		offset = at[index][1] * CHECK_ROW_BYTES + at[index][0] * 4U;
		got = get_word(colour, offset);
		if (got != at[index][2]) {
			kern_logf("powervr: job check: FAILED at colour read back: pixel (%u, %u) is 0x%08x, want 0x%08x\n", at[index][0], at[index][1], got, at[index][2]);
			return EIO;
		}
	}

	/* Then the depths, said apart: the store's layout is unconfirmed. */
	depth_bad = 0;
	for (index = 0; index < 4U && !depth_bad; index++) {
		offset = CHECK_EDGE * CHECK_ROW_BYTES + at[index][1] * CHECK_ROW_BYTES + at[index][0] * 4U;
		got = get_word(depth, offset);
		if (got != at[index][3]) {
			kern_logf("powervr: job check: FAILED at depth read back: pixel (%u, %u) is 0x%08x, want 0x%08x (rows of floats assumed)\n", at[index][0], at[index][1], got, at[index][3]);
			depth_bad = 1;
		}
	}
	if (depth_bad)
		return EIO;

	/* Succeeded: the colours and depths are as drawn. */
	kern_logf("powervr: job check: OK (4 pixels of colour and depth)\n");
	return 0;
}

/*
 * The transfer check: a clear to blue (0xffff0000 as the attachment
 * holds it) of a 64x64 linear attachment, read back, then its copy to a
 * second attachment, read back against the first.
 */
static void
transfer_check(
	struct powervr_render *render,
	struct powervr_session **kept,
	struct powervr_resource **kept_memory)
{
	static struct powervr_transfer_request request;
	struct powervr_session *session;
	struct powervr_resource *memory;
	struct powervr_buffer *buffer;
	struct powervr_image_shape shape;
	struct powervr_image_layout layout;
	struct powervr_view_shape source;
	const struct powervr_format *format;
	void *opaque;
	int error;

	/* A session and two attachments of its own. */
	session = NULL;
	memory = NULL;
	format = powervr_format_find(CHECK_FORMAT);
	error = render->ops.open(render, &opaque);
	if (error == 0) {
		session = opaque;
		error = powervr_buffer_create(render, 2U * CHECK_EDGE * CHECK_ROW_BYTES, &buffer);
	}
	if (error == 0) {
		error = powervr_resource_make(session, buffer, 1, 0, &memory);
		powervr_buffer_put(render, buffer);
		if (error != 0)
			memory = NULL;
	}
	if (error != 0) {
		kern_logf("powervr: transfer check: FAILED at prepare (%d)\n", error);
		if (session != NULL)
			render->ops.close(render, session);
		return;
	}
	kern_logf("powervr: transfer check: prepared (64x64 R8G8B8A8, clear and copy)\n");

	/* The clear of the first attachment. */
	kern_memset(&request, 0, sizeof(request));
	request.kind = POWERVR_TRANSFER_CLEAR;
	request.destination.format = format;
	request.destination.address = memory->address;
	request.destination.width = CHECK_EDGE;
	request.destination.height = CHECK_EDGE;
	request.destination.linear = 1;
	request.destination.row_pitch = CHECK_ROW_BYTES;
	request.destination.padded_width = CHECK_EDGE;
	request.area.width = CHECK_EDGE;
	request.area.height = CHECK_EDGE;
	request.colour[2] = 0x3f800000U;
	request.colour[3] = 0x3f800000U;
	error = powervr_transfer_run(render, session, &request);
	if (error != 0) {
		kern_logf("powervr: transfer check: FAILED at clear (%d)\n", error);
	} else {
		kern_logf("powervr: transfer check: clear finished\n");
		error = transfer_read(memory->buffer, 0, NULL, 0, 0xffff0000U, "clear");
	}

	/* The copy of the first attachment to the second. */
	if (error == 0) {
		shape.width = CHECK_EDGE;
		shape.height = CHECK_EDGE;
		shape.depth = 1;
		shape.levels = 1;
		shape.layers = 1;
		shape.linear = 1;
		shape.volume = 0;
		error = powervr_image_lay_out(&shape, format, &layout);
		if (error != 0)
			kern_logf("powervr: transfer check: FAILED at copy layout (%d)\n", error);
	}
	if (error == 0) {
		kern_memset(&source, 0, sizeof(source));
		source.format = format;
		source.layout = &layout;
		source.address = memory->address;
		source.width = CHECK_EDGE;
		source.height = CHECK_EDGE;
		source.image_levels = 1;
		source.level_count = 1;
		source.layer_count = 1;
		source.linear = 1;
		request.kind = POWERVR_TRANSFER_COPY;
		request.destination.address = memory->address + CHECK_EDGE * CHECK_ROW_BYTES;
		request.source = &source;
		error = powervr_transfer_run(render, session, &request);
		if (error != 0) {
			kern_logf("powervr: transfer check: FAILED at copy (%d)\n", error);
		} else {
			kern_logf("powervr: transfer check: copy finished\n");
			(void)transfer_read(memory->buffer, CHECK_EDGE * CHECK_ROW_BYTES, memory->buffer, 0, 0, "copy");
		}
	}

	/* The session stays open for the caller when asked; otherwise it goes now. */
	if (kept != NULL) {
		*kept = session;
		*kept_memory = memory;
		return;
	}

	transfer_close(render, session, memory);
}

/* Closes a transfer check's session; a transfer the core may still run keeps the memory with it. */
static void
transfer_close(
	struct powervr_render *render,
	struct powervr_session *session,
	struct powervr_resource *memory)
{
	if (session == NULL)
		return;
	if (!session->abandoned)
		powervr_resource_unmake(session, memory);
	render->ops.close(render, session);
}

/*
 * Reads an attachment back against a value or against another
 * attachment, and says the first pixel that differed or that all
 * matched.
 */
static int
transfer_read(
	struct powervr_buffer *buffer,
	uint32_t offset,
	const struct powervr_buffer *against,
	uint32_t against_offset,
	uint32_t value,
	const char *what)
{
	uint32_t want;
	uint32_t got;
	uint32_t at;
	uint32_t x;
	uint32_t y;

	/* Every pixel, row by row. */
	for (y = 0; y < CHECK_EDGE; y++) {
		for (x = 0; x < CHECK_EDGE; x++) {
			at = y * CHECK_ROW_BYTES + x * 4U;
			want = value;
			if (against != NULL)
				want = get_word(against, against_offset + at);
			got = get_word(buffer, offset + at);
			if (got != want) {
				kern_logf("powervr: transfer check: FAILED at %s read back: pixel (%u, %u) is 0x%08x, want 0x%08x\n", what, x, y, got, want);
				return EIO;
			}
		}
	}

	/* Succeeded: every pixel matched. */
	kern_logf("powervr: transfer check: %s OK\n", what);
	return 0;
}

/* Writes words through a buffer's uncached view, low byte first. */
static void
put_words(
	struct powervr_buffer *buffer,
	uint32_t offset,
	const uint32_t *words,
	unsigned count)
{
	unsigned index;

	/* One word at a time. */
	for (index = 0; index < count; index++) {
		buffer->view[offset + index * 4U + 0U] = (uint8_t)words[index];
		buffer->view[offset + index * 4U + 1U] = (uint8_t)(words[index] >> 8);
		buffer->view[offset + index * 4U + 2U] = (uint8_t)(words[index] >> 16);
		buffer->view[offset + index * 4U + 3U] = (uint8_t)(words[index] >> 24);
	}
}

/* Writes bytes through a buffer's uncached view. */
static void
put_bytes(
	struct powervr_buffer *buffer,
	uint32_t offset,
	const uint8_t *bytes,
	size_t count)
{
	size_t index;

	/* One byte at a time. */
	for (index = 0; index < count; index++)
		buffer->view[offset + index] = bytes[index];
}

/* Reads a little-endian word through a buffer's uncached view. */
static uint32_t
get_word(
	const struct powervr_buffer *buffer,
	uint32_t offset)
{
	uint32_t word;

	/* Low byte first. */
	word = buffer->view[offset];
	word |= (uint32_t)buffer->view[offset + 1U] << 8;
	word |= (uint32_t)buffer->view[offset + 2U] << 16;
	word |= (uint32_t)buffer->view[offset + 3U] << 24;

	/* Succeeded: the word. */
	return word;
}

/* Reads a little-endian word of a command. */
static unsigned
get32(
	const uint8_t *bytes,
	unsigned offset)
{
	/* The four bytes, low first. */
	return (unsigned)bytes[offset] | (unsigned)bytes[offset + 1U] << 8 | (unsigned)bytes[offset + 2U] << 16 | (unsigned)bytes[offset + 3U] << 24;
}

/* Prints an object's words from its start, eight to a line, with a name. */
static void
dump_object(
	const char *name,
	const struct powervr_object *object,
	uint32_t bytes)
{
	uint32_t offset;

	/* Eight words a line, at the object's firmware address. */
	for (offset = 0; offset < bytes; offset += 32U) {
		kern_logf("powervr: %s 0x%x +%03x: %08x %08x %08x %08x %08x %08x %08x %08x\n", name,
			  (unsigned)powervr_object_address(object, 0), (unsigned)offset,
			  (unsigned)powervr_object_get32(object, offset), (unsigned)powervr_object_get32(object, offset + 4U),
			  (unsigned)powervr_object_get32(object, offset + 8U), (unsigned)powervr_object_get32(object, offset + 12U),
			  (unsigned)powervr_object_get32(object, offset + 16U), (unsigned)powervr_object_get32(object, offset + 20U),
			  (unsigned)powervr_object_get32(object, offset + 24U), (unsigned)powervr_object_get32(object, offset + 28U));
	}
}

/* Prints the render context's record and its geometry state, and the geometry command buffer's first commands. */
static void
dump_context(
	struct render_check *check)
{
	/* The record (the geometry and fragment common parts and the switch state), the geometry state. */
	dump_object("context", &check->context->record, POWERVR_RENDER_CONTEXT_BYTES);
	dump_object("geometry state", &check->context->geometry_state, POWERVR_GEOMETRY_STATE_BYTES);

	/* The geometry command buffer's control and its first 256 bytes. */
	dump_object("geometry ccb control", &check->context->geometry.control, 32U);
	dump_object("geometry ccb", &check->context->geometry.buffer, 256U);
}

/*
 * Prints the core's geometry and memory registers, read together then
 * printed, and then the page catalogue base of each MMU mapping context.
 *
 * Values referenced:
 * StarFive Linux JH7110_VisionFive2_6.12.y_devel 4cecf169f38e
 * drivers/gpu/drm/img/img-rogue/hwdefs/rogue/km/rgx_cr_defs_km.h
 * (RGX_CR_TIMER 0x0160, RGX_CR_EVENT_STATUS 0x0130,
 * RGX_CR_VDM_CONTEXT_STORE_STATUS 0x0430, RGX_CR_PPP_SIGNATURE 0x5020,
 * RGX_CR_TE_SIGNATURE 0x5028, RGX_CR_VCE_CHECKSUM 0x5030,
 * RGX_CR_BIF_CAT_BASE0 to 7 0x1200 to 0x1238, RGX_CR_BIF_CAT_BASE_INDEX
 * 0x1240, RGX_CR_BIF_FAULT_BANK0_MMU_STATUS 0x12B0, _REQ_STATUS 0x12B8,
 * RGX_CR_BIF_CTRL 0x12A8, RGX_CR_BIF_MMU_STATUS 0x12D0,
 * RGX_CR_BIF_STATUS_MMU 0x1358, RGX_CR_MMU_CBASE_MAPPING_CONTEXT 0xE140,
 * RGX_CR_MMU_CBASE_MAPPING 0xE148, RGX_CR_MMU_FAULT_STATUS 0xE150,
 * RGX_CR_MMU_FAULT_STATUS_META 0xE158); the firmware's own recovery
 * checks read 0x40E0 and 0x4600 for the geometry master.
 *
 * Procedure referenced:
 * the same tree, services/server/devices/rogue/rgxstartstop.c
 * (RGXInitBIF: on a core with RGX_CR_MMU_CBASE_MAPPING the context id
 * goes to the mapping context register, read back as a fence, then the
 * catalogue base goes to the mapping register).
 */
static void
probe_geometry(
	struct powervr_core *core)
{
	static const uint32_t offsets[CHECK_PROBE_REGISTERS] = {
		0x0160U, 0x0130U, 0x0430U, 0x40e0U, 0x4600U, 0x5020U, 0x5028U, 0x5030U,
		0x1200U, 0x1208U, 0x1210U, 0x1218U, 0x1220U, 0x1228U, 0x1230U, 0x1238U,
		0x1240U, 0x12a8U, 0x12b0U, 0x12b8U, 0x12d0U, 0x1358U, 0xe140U, 0xe150U,
		0xe158U, 0x0160U,
	};
	uint64_t values[CHECK_PROBE_REGISTERS];
	uint32_t bases[CHECK_PROBE_CONTEXTS];
	uint32_t saved;
	unsigned index;

	/* All first, then the lines. */
	for (index = 0; index < CHECK_PROBE_REGISTERS; index++)
		values[index] = powervr_read64(core, offsets[index]);

	/* One line a register. */
	for (index = 0; index < CHECK_PROBE_REGISTERS; index++)
		kern_logf("powervr: render check: register 0x%04x = 0x%016llx\n", (unsigned)offsets[index], (unsigned long long)values[index]);

	/* Each mapping context's catalogue base, then the context the register held put back. */
	saved = powervr_read32(core, 0xe140U);
	for (index = 0; index < CHECK_PROBE_CONTEXTS; index++) {
		powervr_write32(core, 0xe140U, index);
		(void)powervr_read32(core, 0xe140U);
		bases[index] = powervr_read32(core, 0xe148U);
	}

	powervr_write32(core, 0xe140U, saved);
	(void)powervr_read32(core, 0xe140U);
	kern_logf("powervr: render check: mapping context 0x%x; catalogue bases 0-7: %x %x %x %x %x %x %x %x\n", (unsigned)saved,
		  (unsigned)bases[0], (unsigned)bases[1], (unsigned)bases[2], (unsigned)bases[3],
		  (unsigned)bases[4], (unsigned)bases[5], (unsigned)bases[6], (unsigned)bases[7]);
}
