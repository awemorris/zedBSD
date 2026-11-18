/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Runs one transfer on a session's transfer context.
 *
 * A clear writes its colour through the clear registers; a copy runs a
 * background object whose program samples the source.  Either stores the
 * rectangle through the end-of-tile program.  The programs and the
 * shared words are written into memory of the transfer's own, mapped in
 * the session's windows, and returned once it is done.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <kern/kcrt.h>
#include <kern/klog.h>
#include <kern/kmem.h>
#include <kern/lock.h>
#include <uapi/errno.h>

#include "drivers/gpu/powervr/transfer-job.h"
#include "drivers/gpu/powervr/interface.h"
#include "drivers/gpu/powervr/render-command.h"
#include "drivers/gpu/powervr/render-program.h"

/* How long a transfer may take. */
#define TRANSFER_WAIT_MS		2000U

/*
 * The transfer's memory: the copy's shared words in the general window;
 * in the data sequencer's the end-of-tile data, the copy's task program
 * (data, then code after eight words) and its shared register program's
 * code and data; in the shading cluster's the end-of-tile and copy
 * programs.
 */
#define GENERAL_SHARED			0x0U
#define GENERAL_BYTES			0x1000U
#define PDS_EVENT			0x000U
#define PDS_KICK			0x040U
#define PDS_SHARED_CODE			0x080U
#define PDS_SHARED_DATA			0x200U
#define PDS_BYTES			0x1000U
#define PDS_SHARED_CODE_ROOM		(PDS_SHARED_DATA - PDS_SHARED_CODE)
#define PDS_SHARED_DATA_ROOM		(PDS_BYTES - PDS_SHARED_DATA)
#define USC_EOT				0x000U
#define USC_COPY			0x100U
#define USC_BYTES			0x1000U

/*
 * The transfer's memory and programs while it runs; one the core may
 * still run waits on the session's list (next) until its timeline value
 * is reached.
 */
struct transfer_memory {
	struct transfer_memory *next;
	uint32_t value;
	struct powervr_resource *general;
	struct powervr_buffer *pds;
	struct powervr_buffer *usc;
	uint64_t pds_place;
	uint64_t usc_place;
	struct powervr_render_usc program;
	struct powervr_render_kick kick;
	struct powervr_data_shared_program shared;
	struct powervr_transfer_job job;
	struct powervr_transfer_regs regs;
	uint8_t command[POWERVR_TRANSFER_COMMAND_BYTES];
};

/*
 * How one kick of a transfer goes: after the waits the firmware checks
 * (wait_count of them), waited for by the host when wait is set, and the
 * value its timeline reaches once done.
 */
struct transfer_kick {
	struct powervr_sync waits[1];
	unsigned wait_count;
	bool wait;
	uint32_t value;
};

static int transfer_run(struct powervr_render *render, struct powervr_session *session, const struct powervr_transfer_request *request, bool ordered);
static int transfer_one(struct powervr_render *render, struct powervr_session *session, struct transfer_memory *memory, const struct powervr_transfer_request *request, const struct powervr_render_area *area, uint32_t source_y, uint32_t step, uint32_t phase, uint32_t total, struct transfer_kick *kick);
static int transfer_map(struct powervr_render *render, struct powervr_session *session, struct transfer_memory *memory);
static void transfer_release(struct powervr_render *render, struct powervr_session *session, struct transfer_memory *memory);
static void put_words(struct powervr_buffer *buffer, uint32_t offset, const uint32_t *words, unsigned count);
static struct transfer_memory *transfer_keep_take(struct powervr_render *render, struct powervr_session *session);
static bool transfer_keep_put(struct powervr_render *render, struct powervr_session *session, struct transfer_memory *memory);
static void transfers_take(struct powervr_render *render, struct powervr_session *session, int all);
static void put_bytes(struct powervr_buffer *buffer, uint32_t offset, const uint8_t *bytes, size_t count);

/*
 * Runs a transfer, one row at a time when the destination's rows need
 * it (an odd linear row of several rows).  A blit's row keeps the whole
 * rectangle's mapping onto the source, so each row samples where it
 * would in one job.
 *
 * Procedure referenced:
 * Mesa 26.2.5 src/imagination/vulkan/pvr_arch_job_transfer.c
 * (pvr_3d_copy_blit_core: the background object for a copy, the clear
 * registers for a clear, the store through the pixel event).
 */
int
powervr_transfer_run(
	struct powervr_render *render,
	struct powervr_session *session,
	const struct powervr_transfer_request *request)
{
	int error;

	/* Done before it returns, not ordered after the session's work. */
	error = transfer_run(render, session, request, false);
	if (error != 0)
		return error;

	/* Succeeded: the transfer is done. */
	return 0;
}

/*
 * Runs a transfer of a session's command buffer: after the session's
 * work before it (the firmware waits for that work's point, no host
 * wait), and left running: its memory waits on the session's list, and
 * the session's last work is the transfer, whose point later work and
 * fences complete after.  A transfer of several kicks (rows) waits for
 * each but the last, which share its memory.
 */
int
powervr_transfer_run_async(
	struct powervr_render *render,
	struct powervr_session *session,
	const struct powervr_transfer_request *request)
{
	int error;

	/* Ordered after the session's work and left running. */
	error = transfer_run(render, session, request, true);
	if (error != 0)
		return error;

	/* Succeeded: the transfer is the core's. */
	return 0;
}

/*
 * Releases the memory of the session's transfers whose timeline values
 * were reached, keeping it mapped for the next ones.
 */
void
powervr_transfers_reap(
	struct powervr_render *render,
	struct powervr_session *session)
{
	/* The finished ones only. */
	transfers_take(render, session, 0);
}

/* Runs a transfer, ordered after the session's work and left running, or done before it returns. */
static int
transfer_run(
	struct powervr_render *render,
	struct powervr_session *session,
	const struct powervr_transfer_request *request,
	bool ordered)
{
	struct powervr_transfer_context *context;
	struct transfer_memory *memory;
	struct powervr_transfer_request single;
	struct powervr_render_area row;
	struct transfer_kick kick;
	unsigned long enabled;
	uint32_t index;
	uint32_t kicks;
	bool by_rows;
	int error;

	/* A copy names its source. */
	if (request->kind != POWERVR_TRANSFER_CLEAR && request->source == NULL)
		return EINVAL;

	/* The session's transfer context, and the transfer's memory. */
	error = powervr_session_transfer_context(render, session, &context);
	if (error != 0)
		return error;

	/* The memory the session's last transfer kept mapped, else new memory, mapped. */
	memory = transfer_keep_take(render, session);
	if (memory == NULL) {
		memory = kern_calloc(1, sizeof(*memory));
		if (memory == NULL)
			return ENOMEM;

		error = transfer_map(render, session, memory);
		if (error != 0) {
			transfer_release(render, session, memory);
			return error;
		}
	}

	/*
	 * The kicks: the first after the session's earlier work when ordered
	 * (a transfer after the transfer context's own needs no fence); each
	 * waited for but an ordered transfer's last, as they share the memory.
	 */
	by_rows = powervr_transfer_needs_rows(&request->destination, &request->area);
	kicks = 1;
	if (by_rows && request->kind == POWERVR_TRANSFER_COPY)
		kicks = request->area.height < 2U ? request->area.height : 2U;
	else if (by_rows)
		kicks = request->area.height;
	kern_memset(&kick, 0, sizeof(kick));
	kick.wait = !ordered || kicks > 1U;
	if (ordered && session->last_kind != POWERVR_POINT_TRANSFER)
		kick.wait_count = powervr_session_point_sync(session, session->last_kind, session->last_value, &kick.waits[0]);

	/* The whole rectangle, or each of its rows. */
	if (!by_rows) {
		error = transfer_one(render, session, memory, request, &request->area, request->source_y, 1U, 0U, 0U, &kick);
	} else if (request->kind == POWERVR_TRANSFER_COPY) {
		/*
		 * A copy into odd rows: two jobs of doubled line stride, the
		 * even rows and the odd ones, each sampling every second
		 * source row, rather than one job a row.
		 */
		single = *request;
		single.destination.row_pitch = request->destination.row_pitch * 2U;
		single.destination.padded_width = request->destination.padded_width * 2U;
		row = request->area;
		for (index = 0; error == 0 && index < 2U && index < request->area.height; index++) {
			single.destination.address = request->destination.address + (uint64_t)(request->area.y + index) * request->destination.row_pitch;
			row.y = 0;
			row.height = (request->area.height - index + 1U) / 2U;
			single.destination.height = row.height;
			kick.wait = !ordered || index + 1U < kicks;
			error = transfer_one(render, session, memory, &single, &row, request->source_y, 2U, index, request->area.height, &kick);
			kick.wait_count = 0;
		}
	} else {
		/*
		 * A copy's row is a surface of its own one row high at that
		 * row's address: the line stride cannot name an odd row, so a
		 * row of the whole surface would land at the wrong place
		 * (vkmatrix's copy-65 case shows it).  A blit's row keeps the
		 * whole surface and mapping.
		 */
		row = request->area;
		row.height = 1;
		single = *request;
		for (index = 0; error == 0 && index < request->area.height; index++) {
			row.y = request->area.y + index;
			kick.wait = !ordered || index + 1U < kicks;
			if (request->kind == POWERVR_TRANSFER_BLIT) {
				error = transfer_one(render, session, memory, request, &row, request->source_y + index, 1U, 0U, 0U, &kick);
			} else {
				single.destination.address = request->destination.address + (uint64_t)row.y * request->destination.row_pitch;
				single.destination.height = 1;
				row.y = 0;
				error = transfer_one(render, session, memory, &single, &row, request->source_y + index, 1U, 0U, 0U, &kick);
			}
			kick.wait_count = 0;
		}
	}

	/* A transfer the core may still run keeps its memory. */
	if (error == ETIMEDOUT) {
		kern_logf("powervr: transfer did not finish; its memory stays with the core\n");
		session->abandoned = 1;
		return error;
	}

	/* Refused: the memory goes (a kick that was not waited for left no error). */
	if (error != 0) {
		transfer_release(render, session, memory);
		return error;
	}

	/* Left running: the memory waits on the session's list, and the session's last work is the transfer. */
	if (!kick.wait) {
		memory->value = kick.value;
		memory->next = NULL;
		enabled = spin_lock_irqsave(&render->queue_lock);

		if (session->transfers_done_tail != NULL) {
			session->transfers_done_tail->next = memory;
		} else {
			session->transfers_done = memory;
		}
		session->transfers_done_tail = memory;

		spin_unlock_irqrestore(&render->queue_lock, enabled);

		powervr_session_point_set(session, POWERVR_POINT_TRANSFER, kick.value);
		return 0;
	}

	/* Done: the memory is kept mapped for the session's next transfer. */
	if (!transfer_keep_put(render, session, memory))
		transfer_release(render, session, memory);

	/* Succeeded: the transfer is done. */
	return 0;
}

/* Builds and kicks the transfer of one rectangle, waiting for it when the kick says so. */
static int
transfer_one(
	struct powervr_render *render,
	struct powervr_session *session,
	struct transfer_memory *memory,
	const struct powervr_transfer_request *request,
	const struct powervr_render_area *area,
	uint32_t source_y,
	uint32_t step,
	uint32_t phase,
	uint32_t total,
	struct transfer_kick *kick)
{
	struct powervr_transfer_job *job;
	uint32_t words[POWERVR_TRANSFER_MERGE_WORDS];
	uint32_t word_count;
	uint32_t event[POWERVR_RENDER_EVENT_DATA_WORDS];
	uint32_t state[2];
	uint32_t pds_base;
	uint32_t usc_base;
	uint32_t value;
	int error;
	bool resolve;
	bool averaged;
	bool raw;

	/*
	 * A blit from four samples a pixel resolves them (vkCmdResolveImage).
	 * A blit of a format the blit's program does not pack, onto the same
	 * format, moves raw texels: the copy's program (a resolve's of sample
	 * 0, of an integer format) over the blit's mapping, stored as the raw
	 * format (the job a copy's).  A resolve that averages packs as the
	 * blit does.
	 */
	resolve = false;
	raw = false;
	if (request->kind == POWERVR_TRANSFER_BLIT) {
		if (request->source->layout != NULL && request->source->layout->samples == 4U)
			resolve = true;
		raw = powervr_transfer_blits_raw(request->source->format, request->destination.format);
		if (resolve && raw) {
			/* An averaging resolve of a format the blit does not pack is not raw. */
			averaged = powervr_transfer_resolves(request->destination.format);
			if (averaged)
				raw = false;
		}
	}

	/* The job: the kind, the destination, the rectangle, the programs' places. */
	pds_base = (uint32_t)(memory->pds_place - POWERVR_PDS_HEAP_BASE);
	usc_base = (uint32_t)(memory->usc_place - POWERVR_USC_HEAP_BASE);
	job = &memory->job;
	kern_memset(job, 0, sizeof(*job));
	job->kind = request->kind;
	if (raw)
		job->kind = POWERVR_TRANSFER_COPY;
	job->destination = request->destination;
	job->area = *area;
	kern_memcpy(job->colour, request->colour, sizeof(job->colour));
	job->source_x = request->source_x;
	job->source_y = source_y;
	job->copy_kick_offset = pds_base + PDS_KICK;
	job->shared_code_offset = pds_base + PDS_SHARED_CODE;
	job->shared_data_offset = pds_base + PDS_SHARED_DATA;
	job->event_data_offset = pds_base + PDS_EVENT;
	if (request->samples == 4U) {
		/* Sample to sample: a copy alone. */
		if (request->kind != POWERVR_TRANSFER_COPY)
			return EINVAL;
		job->samples = 4;
	}

	/* A merging copy writes a twiddled destination whole: refuses another kind, a linear destination and rows. */
	if (request->merge != NULL && (request->kind != POWERVR_TRANSFER_COPY || request->destination.linear || step != 1U))
		return ENOTSUP;

	/* An extracting copy is a copy of one sample a pixel, not merging. */
	if (request->extract_shift != 0) {
		if (request->kind != POWERVR_TRANSFER_COPY)
			return EINVAL;
		if (request->merge != NULL || job->samples == 4U)
			return EINVAL;
	}

	/* A copy or a blit: its program, its task program, its shared words and the program that loads them. */
	if (request->kind != POWERVR_TRANSFER_CLEAR) {
		if (request->merge != NULL) {
			/* A copy into one aspect of a packed depth and stencil keeps the other. */
			error = powervr_transfer_merge_usc(request->merge_shift, request->merge_mask, &memory->program);
		} else if (request->extract_shift != 0) {
			/* A copy of one aspect out of a packed depth and stencil. */
			error = powervr_transfer_extract_usc(request->extract_shift, &memory->program);
		} else if (resolve) {
			/* The resolve of the four samples, or of sample 0. */
			error = powervr_transfer_resolve_usc(request->destination.format, &memory->program);
		} else if (raw) {
			error = powervr_transfer_copy_usc(request->source->format, &memory->program);
		} else if (request->kind == POWERVR_TRANSFER_BLIT) {
			error = powervr_transfer_blit_usc(request->destination.format, &memory->program);
		} else if (request->source->kind == POWERVR_VIEW_VOLUME) {
			error = powervr_transfer_copy_usc_volume(request->source->format, request->source_z, &memory->program);
		} else if (job->samples == 4U) {
			error = powervr_transfer_copy_usc_samples(request->source->format, &memory->program);
		} else {
			error = powervr_transfer_copy_usc(request->source->format, &memory->program);
		}
		if (error != 0)
			return error;

		/* The program's task: for each sample of four (the full rate), else for each pixel. */
		put_bytes(memory->usc, USC_COPY, memory->program.code, memory->program.length);
		if (job->samples == 4U)
			error = powervr_render_fragment_kick_samples(usc_base + USC_COPY, memory->program.temps, 4U, &memory->kick);
		else
			error = powervr_render_kick_build(usc_base + USC_COPY, memory->program.temps, &memory->kick);
		if (error != 0)
			return error;

		put_words(memory->pds, PDS_KICK, memory->kick.data, POWERVR_RENDER_KICK_DATA_WORDS);
		put_words(memory->pds, PDS_KICK + POWERVR_RENDER_KICK_DATA_WORDS * 4U, memory->kick.code, POWERVR_RENDER_KICK_CODE_WORDS);
		word_count = POWERVR_TRANSFER_SHARED_WORDS;
		if (request->merge != NULL) {
			error = powervr_transfer_merge_words(request->source, request->source_x, source_y, area, request->merge, words);
			word_count = POWERVR_TRANSFER_MERGE_WORDS;
		} else if (raw) {
			error = powervr_transfer_raw_blit_words(request->source, &request->source_area, &request->area, request->flip_x, request->flip_y, words);
		} else if (request->kind == POWERVR_TRANSFER_BLIT) {
			error = powervr_transfer_blit_words(request->source, &request->source_area, &request->area, request->flip_x, request->flip_y, request->linear, words);
		} else {
			if (step > 1U)
				error = powervr_transfer_shared_words_rows(request->source, request->source_x, source_y, area, total, step, phase, words);
			else
				error = powervr_transfer_shared_words(request->source, request->source_x, source_y, area, words);
		}
		if (error != 0)
			return error;

		put_words(memory->general->buffer, GENERAL_SHARED, words, word_count);
		error = powervr_transfer_shared_program_words(memory->general->address + GENERAL_SHARED, word_count, &memory->shared);
		if (error != 0)
			return error;

		if (memory->shared.code_words * 4U > PDS_SHARED_CODE_ROOM || memory->shared.data_words * 4U > PDS_SHARED_DATA_ROOM)
			return E2BIG;

		put_words(memory->pds, PDS_SHARED_CODE, memory->shared.code, memory->shared.code_words);
		put_words(memory->pds, PDS_SHARED_DATA, memory->shared.data, memory->shared.data_words);
		job->shared_data_words = memory->shared.data_words;
		job->shared_registers = word_count;
	}

	/* The registers, and the end-of-tile program that emits the back end's state. */
	error = powervr_transfer_regs_build(job, &memory->regs, state);
	if (error != 0)
		return error;

	error = powervr_render_eot_usc(state, &memory->program);
	if (error != 0)
		return error;

	put_bytes(memory->usc, USC_EOT, memory->program.code, memory->program.length);
	error = powervr_render_event_data(usc_base + USC_EOT, memory->program.temps, event);
	if (error != 0)
		return error;

	put_words(memory->pds, PDS_EVENT, event, POWERVR_RENDER_EVENT_DATA_WORDS);

	/* The command, its kick, its wait; the wait says the transfer stage. */
	kern_memset(memory->command, 0, sizeof(memory->command));
	powervr_transfer_command(&memory->regs, memory->command);
	mutex_lock(&render->core->lock);

	error = powervr_session_kick_transfer(render, session, memory->command, kick->waits, kick->wait_count, &value);

	mutex_unlock(&render->core->lock);

	if (error != 0)
		return error;

	/* Its value; waited for unless the transfer is left running. */
	kick->value = value;
	if (!kick->wait)
		return 0;
	error = powervr_transfer_finish(session, value, TRANSFER_WAIT_MS);
	if (error != 0)
		return error;

	/* Succeeded: the rectangle is written. */
	return 0;
}

/* Makes and maps the transfer's memory. */
static int
transfer_map(
	struct powervr_render *render,
	struct powervr_session *session,
	struct transfer_memory *memory)
{
	struct powervr_buffer *buffer;
	int error;

	/* The general memory. */
	error = powervr_buffer_create(render, GENERAL_BYTES, &buffer);
	if (error != 0)
		return error;

	error = powervr_resource_make(session, buffer, 1, 0, &memory->general);
	powervr_buffer_put(render, buffer);
	if (error != 0) {
		memory->general = NULL;
		return error;
	}

	/* The program buffers, in their windows. */
	error = powervr_buffer_create(render, PDS_BYTES, &memory->pds);
	if (error != 0) {
		memory->pds = NULL;
		return error;
	}

	error = powervr_session_program_map(session, POWERVR_PROGRAM_PDS, memory->pds, &memory->pds_place);
	if (error != 0) {
		memory->pds_place = 0;
		return error;
	}

	error = powervr_buffer_create(render, USC_BYTES, &memory->usc);
	if (error != 0) {
		memory->usc = NULL;
		return error;
	}

	error = powervr_session_program_map(session, POWERVR_PROGRAM_USC, memory->usc, &memory->usc_place);
	if (error != 0) {
		memory->usc_place = 0;
		return error;
	}

	/* Succeeded: the memory is mapped. */
	return 0;
}

/*
 * Takes memory a session's finished transfer kept mapped, its buffers as
 * they are (each transfer writes what it reads of them), the rest
 * cleared; the finished transfers' memory goes there first.  NULL when
 * none is kept.
 */
static struct transfer_memory *
transfer_keep_take(
	struct powervr_render *render,
	struct powervr_session *session)
{
	struct transfer_memory *memory;
	struct transfer_memory kept;
	unsigned long enabled;

	/* The finished transfers' memory, kept or freed. */
	transfers_take(render, session, 0);

	/* The newest kept memory, off the session. */
	memory = NULL;
	enabled = spin_lock_irqsave(&render->queue_lock);

	if (session->transfer_keep_count != 0U) {
		session->transfer_keep_count--;
		memory = session->transfer_keep[session->transfer_keep_count];
		session->transfer_keep[session->transfer_keep_count] = NULL;
	}

	spin_unlock_irqrestore(&render->queue_lock, enabled);

	if (memory == NULL)
		return NULL;

	/* Its mapped buffers stay; the rest is cleared for the new transfer. */
	kern_memset(&kept, 0, sizeof(kept));
	kept.general = memory->general;
	kept.pds = memory->pds;
	kept.usc = memory->usc;
	kept.pds_place = memory->pds_place;
	kept.usc_place = memory->usc_place;
	kern_memcpy(memory, &kept, sizeof(kept));

	/* Succeeded: memory ready to be written again. */
	return memory;
}

/*
 * Keeps a finished transfer's mapped memory for the session's next one
 * (mapping three buffers a transfer cost more than the copy of a glyph),
 * while the session keeps fewer than POWERVR_TRANSFER_KEEP and it and the
 * core are well.  Reports whether it did.
 */
static bool
transfer_keep_put(
	struct powervr_render *render,
	struct powervr_session *session,
	struct transfer_memory *memory)
{
	unsigned long enabled;
	bool kept;

	/* Only whole memory. */
	if (memory->general == NULL || memory->pds == NULL || memory->usc == NULL ||
	    memory->pds_place == 0U || memory->usc_place == 0U)
		return false;

	/* Into a free place, when the session and the core are well. */
	kept = false;
	enabled = spin_lock_irqsave(&render->queue_lock);

	if (session->transfer_keep_count < POWERVR_TRANSFER_KEEP && !render->faulted && !session->lost && !session->isolated) {
		session->transfer_keep[session->transfer_keep_count] = memory;
		session->transfer_keep_count++;
		kept = true;
	}

	spin_unlock_irqrestore(&render->queue_lock, enabled);

	/* Succeeded: whether it was kept. */
	return kept;
}

/*
 * Takes the memory of the session's transfers the core finished, oldest
 * first, keeping it mapped for the next ones or freeing it; with all set
 * (the session's work is over) every one.  Nothing is taken of a lost or
 * isolated session, or on a faulted device, before then: the core may
 * still reach it.
 */
static void
transfers_take(
	struct powervr_render *render,
	struct powervr_session *session,
	int all)
{
	struct transfer_memory *memory;
	unsigned long enabled;
	int reached;

	/* Oldest first, while there is one to take. */
	for (;;) {
		enabled = spin_lock_irqsave(&render->queue_lock);

		memory = session->transfers_done;
		reached = 0;
		if (memory != NULL) {
			reached = powervr_session_point_reached(session, POWERVR_POINT_TRANSFER, memory->value);
			if (render->faulted || session->lost || session->isolated)
				reached = 0;
		}
		if (memory != NULL && (reached || all)) {
			session->transfers_done = memory->next;
			if (session->transfers_done == NULL)
				session->transfers_done_tail = NULL;
		}

		spin_unlock_irqrestore(&render->queue_lock, enabled);

		/* Nothing more, or the oldest still runs. */
		if (memory == NULL || (!reached && !all))
			return;

		/* Kept for the next transfer, or freed (all of them when the session goes). */
		memory->next = NULL;
		if (all || !transfer_keep_put(render, session, memory))
			transfer_release(render, session, memory);
	}
}

/*
 * Frees the transfer memory a session kept mapped and its transfers'
 * memory, when the session ends (its work is over).
 */
void
powervr_transfer_forget(
	struct powervr_render *render,
	struct powervr_session *session)
{
	struct transfer_memory *memory;
	unsigned long enabled;

	/* The transfers' memory, then the kept memory, unmapped and freed. */
	transfers_take(render, session, 1);
	for (;;) {
		memory = NULL;
		enabled = spin_lock_irqsave(&render->queue_lock);

		if (session->transfer_keep_count != 0U) {
			session->transfer_keep_count--;
			memory = session->transfer_keep[session->transfer_keep_count];
			session->transfer_keep[session->transfer_keep_count] = NULL;
		}

		spin_unlock_irqrestore(&render->queue_lock, enabled);

		if (memory == NULL)
			return;
		transfer_release(render, session, memory);
	}
}

/* Unmaps and returns the transfer's memory and its bookkeeping. */
static void
transfer_release(
	struct powervr_render *render,
	struct powervr_session *session,
	struct transfer_memory *memory)
{
	/* The program buffers. */
	if (memory->usc != NULL) {
		if (memory->usc_place != 0U)
			powervr_session_program_unmap(session, POWERVR_PROGRAM_USC, memory->usc, memory->usc_place);
		powervr_buffer_put(render, memory->usc);
	}

	if (memory->pds != NULL) {
		if (memory->pds_place != 0U)
			powervr_session_program_unmap(session, POWERVR_PROGRAM_PDS, memory->pds, memory->pds_place);
		powervr_buffer_put(render, memory->pds);
	}

	/* The general memory and the bookkeeping. */
	if (memory->general != NULL)
		powervr_resource_unmake(session, memory->general);
	kern_free(memory);
}

/*
 * Writes words through a buffer's uncached view, little-endian: a word a
 * store where the place is a word's (each uncached store is a bus write),
 * else a byte at a time.
 */
static void
put_words(
	struct powervr_buffer *buffer,
	uint32_t offset,
	const uint32_t *words,
	unsigned count)
{
	volatile uint32_t *place;
	unsigned index;

	/* A word's place: one store a word (the processor is little-endian, as the core reads them). */
	if ((offset & 3U) == 0U) {
		place = (volatile uint32_t *)(buffer->view + offset);
		for (index = 0; index < count; index++)
			place[index] = words[index];
		return;
	}

	/* Otherwise one byte at a time, low byte first. */
	for (index = 0; index < count; index++) {
		buffer->view[offset + index * 4U + 0U] = (uint8_t)words[index];
		buffer->view[offset + index * 4U + 1U] = (uint8_t)(words[index] >> 8);
		buffer->view[offset + index * 4U + 2U] = (uint8_t)(words[index] >> 16);
		buffer->view[offset + index * 4U + 3U] = (uint8_t)(words[index] >> 24);
	}
}

/*
 * Writes bytes through a buffer's uncached view: the bytes up to a word's
 * place one at a time, then whole words assembled from them, then the
 * bytes left.
 */
static void
put_bytes(
	struct powervr_buffer *buffer,
	uint32_t offset,
	const uint8_t *bytes,
	size_t count)
{
	volatile uint32_t *place;
	uint32_t word;
	size_t index;

	/* The bytes before a word's place. */
	index = 0;
	while (index < count && ((offset + index) & 3U) != 0U) {
		buffer->view[offset + index] = bytes[index];
		index++;
	}

	/* Whole words, little-endian. */
	place = (volatile uint32_t *)(buffer->view + offset + index);
	while (index + 4U <= count) {
		word = (uint32_t)bytes[index];
		word |= (uint32_t)bytes[index + 1U] << 8;
		word |= (uint32_t)bytes[index + 2U] << 16;
		word |= (uint32_t)bytes[index + 3U] << 24;
		*place = word;
		place++;
		index += 4U;
	}

	/* The bytes left. */
	while (index < count) {
		buffer->view[offset + index] = bytes[index];
		index++;
	}
}
