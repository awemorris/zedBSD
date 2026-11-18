/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The reconstruction of a macroblock in the software H.264 decoder: the
 * preparation of its work with the slice context, the reconstruction of
 * its samples from that work, and the stages a picture's macroblocks go
 * through (preparation, samples, deblocking by rows).
 */

#include "h264sw-recon.h"
#include "h264sw-context.h"
#include "h264sw-deblock.h"
#include "h264sw-interp.h"
#include "h264sw-intra.h"
#include "h264sw-mvpred.h"
#include "h264sw-tables.h"
#include "media.h"
#include "media-os.h"

#include <errno.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

/* The decoded_4x4 value of a macroblock whose sixteen 4x4 blocks all have their motion. */
#define RECON_DECODED_ALL 0xffffU

/* The nonzero value of a macroblock whose sixteen luma 4x4 blocks all count as having coefficients. */
#define RECON_NONZERO_ALL 0xffffU

/* The largest QPY of an 8-bit stream (7.4.3). */
#define RECON_QP_MAX 51

/* The largest value a mode keeps in a byte of the work record; a larger one stays one the predictors refuse. */
#define RECON_MODE_MAX 0xffU

/* The smallest and the largest value of an int16_t, and the number of values between them: a sum is wrapped into this range. */
#define RECON_VECTOR_MIN (-32768)
#define RECON_VECTOR_MAX 32767
#define RECON_VECTOR_SPAN 65536

/* The index in struct h264sw_levelscale's scale4x4 of the Intra Y, Intra Cb, Inter Y and Inter Cb lists (Table 7-2); the Cr list follows the Cb one. */
#define RECON_SCALE4_INTRA_Y 0U
#define RECON_SCALE4_INTRA_CB 1U
#define RECON_SCALE4_INTER_Y 3U
#define RECON_SCALE4_INTER_CB 4U

/* The index in struct h264sw_levelscale's scale8x8 of the Intra Y and Inter Y lists (Table 7-2). */
#define RECON_SCALE8_INTRA_Y 0U
#define RECON_SCALE8_INTER_Y 1U

/* The coded bits of the sixteen luma blocks. */
#define RECON_CODED_LUMA 0xffffU

/* The macroblocks a thread takes at once: few enough that the other threads find work beside it, enough to keep the mutex quiet. */
#define RECON_TAKE_MAX 4U

/*
 * The macroblocks the parsing prepares before it hands them to the
 * threads; the end of a row and the end of the picture hand over the rest
 * as well, so the row is soon in place for its deblocking.  Each hand-over
 * may wake a worker, which costs the parsing thread a system call.
 */
#define RECON_HAND_OVER 32U

/* The rows past the oldest entry not taken that a thread looks through for work its neighbours allow. */
#define RECON_LOOK_ROWS 2U

/* A size of at least one cache line, kept between the parsing thread's own counts and the shared ones. */
#define RECON_CACHE_LINE 64U

/* The state of a place of the ring under threads: free or waiting to be taken, taken by a thread, its samples in place. */
#define RECON_JOB_WAITING 0U
#define RECON_JOB_TAKEN 1U
#define RECON_JOB_DONE 2U

/* What a thread took: nothing, the samples of a group of macroblocks, or a row to deblock. */
#define RECON_TASK_NONE 0U
#define RECON_TASK_SAMPLES 1U
#define RECON_TASK_DEBLOCK 2U

/* The neighbours of struct h264sw_mb_job's neighbours, in the order recon_job_ready looks at them. */
#define RECON_NEIGHBOURS 4U

/*
 * One worker thread of a decoder's stages.
 *
 * owner: the threads it serves.  thread or handle: the thread as the
 * system's threads or the caller's start made it; the record stays in
 * place until the thread is joined.
 */
struct recon_worker {
	struct h264sw_recon_threads *owner;
	struct media_os_thread thread;
	void *handle;
};

/*
 * A piece of work a thread took under the mutex and does outside it.
 *
 * kind: RECON_TASK_*.  picture: the picture it belongs to.  For samples,
 * the entries first .. first + count - 1 of the ring (entry n of the
 * picture is in jobs[n % job_capacity]); for a deblock, the row.
 */
struct recon_task {
	unsigned kind;
	struct h264sw_picture_ctx *picture;
	unsigned first;
	unsigned count;
	unsigned row;
};

/*
 * The threads of a decoder's stages, and the state they share.
 *
 * The parsing thread's own counts, which no other thread reads: written,
 * the entries prepared in the picture so far (in decoding order); handed,
 * those handed over; free_seen, the value of retired it saw last (retired
 * only grows, so the ring has at least the room it shows).  pad keeps
 * them off the cache line of the shared state.
 *
 * Under lock (the mutex), with the picture's rows_done and deblock_row:
 * picture, the picture whose work the threads take, NULL once it is let
 * go; published, the entries handed over; next_take, the oldest entry
 * not taken; retired, the entries before it all have their samples in
 * place and their places in the ring free (retired <= next_take <=
 * published); state, the RECON_JOB_* of each place of the ring
 * (state_capacity places); mb_done, 1 for each macroblock whose samples
 * are in place (mb_done_capacity macroblocks); slice_count, the
 * picture's slices when the work was last handed over; deblocking, 1
 * while a thread deblocks a row; running, the threads doing taken work
 * outside the mutex (the picture is not let go before it is 0); idle, the
 * workers waiting on work; waiting, 1 while the parsing thread waits on
 * progress; quit, 1 when the workers are to end.
 *
 * deblock_view: a copy of the picture made when its work is first handed
 * over (view_ready), which the thread deblocking a row uses alone, with
 * the slice_count of the work handed over (the parsing thread changes the
 * picture's own while the row is deblocked).
 *
 * count: the threads, the caller's included.  given: 1 when the caller's
 * calls make the threads.  workers: count - 1 of them.
 */
struct h264sw_recon_threads {
	unsigned written;
	unsigned handed;
	unsigned free_seen;
	unsigned char pad[RECON_CACHE_LINE];
	struct media_os_mutex lock;
	struct media_os_cond work;
	struct media_os_cond progress;
	struct h264sw_picture_ctx *picture;
	unsigned published;
	unsigned next_take;
	unsigned retired;
	uint8_t *state;
	unsigned state_capacity;
	uint8_t *mb_done;
	unsigned mb_done_capacity;
	unsigned slice_count;
	int deblocking;
	unsigned running;
	unsigned idle;
	int waiting;
	int quit;
	struct h264sw_picture_ctx deblock_view;
	int view_ready;
	unsigned count;
	int given;
	struct media_threads calls;
	struct recon_worker workers[H264SW_RECON_THREADS_MAX - 1U];
};

/*
 * The names of the worker threads, by worker (the caller's thread keeps
 * its own).  Constant for the life of the program.
 */
static const char *const recon_thread_names[H264SW_RECON_THREADS_MAX - 1U] = {
	"h264-decode-1",
	"h264-decode-2",
	"h264-decode-3"
};

/* Forward declarations of the static functions. */
static int recon_check(const struct h264sw_slice_ctx *ctx);
static int recon_floor_div(int value, int divisor);
static int recon_wrap_vector(int value);
static int recon_qp(const struct h264sw_mb *mb);
static int recon_chroma_qp(const struct h264sw_picture_ctx *picture, int qp, unsigned component);
static int recon_has_level(const int32_t *levels, unsigned count);
static int recon_uses_list(unsigned pred, unsigned list);
static uint8_t *recon_sample_at(const struct h264sw_frame *frame, unsigned plane, unsigned x, unsigned y);
static void recon_put_block(uint8_t *dst, size_t dst_stride, const uint8_t *src, unsigned size);
static void recon_store4x4(const int32_t *levels, int16_t *out);
static void recon_store8x8(const int32_t *levels, int16_t *out);
static int recon_prepare_pcm(struct h264sw_slice_ctx *ctx, struct h264sw_mb_job *job);
static int recon_prepare_intra(struct h264sw_slice_ctx *ctx, struct h264sw_mb_job *job);
static void recon_prepare_intra4x4(struct h264sw_slice_ctx *ctx, struct h264sw_mb_job *job);
static void recon_prepare_intra8x8(struct h264sw_slice_ctx *ctx, struct h264sw_mb_job *job);
static void recon_prepare_intra16x16(struct h264sw_slice_ctx *ctx, struct h264sw_mb_job *job);
static void recon_prepare_luma4x4(struct h264sw_slice_ctx *ctx, struct h264sw_mb_job *job);
static void recon_prepare_luma8x8(struct h264sw_slice_ctx *ctx, struct h264sw_mb_job *job);
static void recon_prepare_chroma(struct h264sw_slice_ctx *ctx, struct h264sw_mb_job *job);
static int recon_prepare_inter(struct h264sw_slice_ctx *ctx, struct h264sw_mb_job *job);
static int recon_prepare_refs(struct h264sw_slice_ctx *ctx, struct h264sw_mb_job *job);
static int recon_prepare_quadrant(struct h264sw_slice_ctx *ctx, struct h264sw_mb_job *job, unsigned quadrant, unsigned blocks);
static int recon_check_ref(struct h264sw_slice_ctx *ctx, unsigned list, int ref_idx);
static int recon_motion(struct h264sw_slice_ctx *ctx);
static int recon_motion_pskip(struct h264sw_slice_ctx *ctx);
static int recon_motion_direct(struct h264sw_slice_ctx *ctx);
static int recon_motion_parts(struct h264sw_slice_ctx *ctx);
static int recon_motion_sub(struct h264sw_slice_ctx *ctx);
static int recon_partition(struct h264sw_slice_ctx *ctx, unsigned pred, unsigned part, unsigned sub, unsigned x, unsigned y, unsigned width, unsigned height);
static int recon_vector(struct h264sw_slice_ctx *ctx, unsigned list, int ref_idx, const int16_t *mvd, unsigned x, unsigned y, unsigned width, unsigned height);
static void recon_mark_decoded(struct h264sw_slice_ctx *ctx, unsigned x, unsigned y, unsigned width, unsigned height);
static int recon_uniform(const struct h264sw_mb *mb, unsigned x, unsigned y, unsigned size);
static void recon_residual4x4(const int16_t *coeffs, int dc_present, int32_t dc, const int32_t *scale, int qp, uint8_t *dst, size_t stride);
static void recon_residual8x8(const int16_t *coeffs, const int32_t *scale, int qp, uint8_t *dst, size_t stride);
static void recon_samples_pcm(const struct h264sw_picture_ctx *picture, const struct h264sw_mb_job *job);
static void recon_samples_intra4x4(const struct h264sw_picture_ctx *picture, const struct h264sw_mb_job *job);
static void recon_samples_intra8x8(const struct h264sw_picture_ctx *picture, const struct h264sw_mb_job *job);
static void recon_samples_intra16x16(const struct h264sw_picture_ctx *picture, const struct h264sw_mb_job *job);
static void recon_samples_intra_chroma(const struct h264sw_picture_ctx *picture, const struct h264sw_mb_job *job);
static void recon_samples_chroma_residual(const struct h264sw_picture_ctx *picture, const struct h264sw_mb_job *job, int intra);
static void recon_samples_luma_residual(const struct h264sw_picture_ctx *picture, const struct h264sw_mb_job *job);
static void recon_samples_inter(const struct h264sw_picture_ctx *picture, const struct h264sw_mb_job *job);
static void recon_samples_block(const struct h264sw_picture_ctx *picture, const struct h264sw_mb_job *job, const struct h264sw_mb *mb, unsigned x, unsigned y, unsigned size);
static void recon_deblock_ready(struct h264sw_picture_ctx *picture);
static void recon_reset_counts(struct h264sw_picture_ctx *picture);
static int recon_queue_threads(struct h264sw_slice_ctx *ctx, struct h264sw_recon_threads *threads);
static void recon_hand_over(struct h264sw_recon_threads *threads, struct h264sw_picture_ctx *picture);
static void recon_help(struct h264sw_recon_threads *threads, struct h264sw_picture_ctx *picture, int to_end);
static int recon_take(struct h264sw_recon_threads *threads, struct recon_task *task);
static int recon_deblock_due(const struct h264sw_recon_threads *threads);
static int recon_job_ready(const struct h264sw_recon_threads *threads, const struct h264sw_mb_job *job, unsigned first, unsigned count);
static void recon_run(struct h264sw_recon_threads *threads, const struct recon_task *task);
static void recon_finish(struct h264sw_recon_threads *threads, const struct recon_task *task);
static void recon_wake(struct h264sw_recon_threads *threads);
static void recon_worker_main(void *argument);

/*
 * Reconstructs the current macroblock (ctx->mb_addr, ctx->n, ctx->syntax).
 *
 * The specification is the comment of the declaration in
 * h264sw-recon.h.  The macroblock is prepared, then its samples are
 * reconstructed from the work prepared.
 */
int
h264sw_recon_mb(
	struct h264sw_slice_ctx *ctx)
{
	struct h264sw_mb_job job;
	int error;

	/* Derives the motion and the modes, checks the references and records the work. */
	error = h264sw_recon_prepare(ctx, &job);
	if (error != 0)
		return error;

	/* Writes the samples from the work. */
	h264sw_recon_samples(ctx->picture, &job);

	/* Succeeded: the macroblock's samples, motion and modes are in place. */
	return 0;
}

/*
 * Prepares the reconstruction of the current macroblock.
 *
 * The specification is the comment of the declaration in
 * h264sw-recon.h.
 */
int
h264sw_recon_prepare(
	struct h264sw_slice_ctx *ctx,
	struct h264sw_mb_job *job)
{
	const struct h264sw_mb *mb;
	unsigned kind;
	unsigned mode;
	unsigned concealed_before;
	int error;

	/* Refuses a context that has no picture, frame or macroblock to work on, and a missing record. */
	error = recon_check(ctx);
	if (error != 0)
		return error;
	if (job == NULL)
		return EINVAL;

	/* Remembers how many macroblocks were concealed, so that this one adds at most one however many of its steps fall back. */
	concealed_before = ctx->concealed;

	/* Starts the record with no coded block, no intra neighbour and no inter layout. */
	job->coded = 0;
	job->neighbours = 0;
	job->layout = 0;

	/* Prepares the macroblock the way its kind says. */
	kind = ctx->syntax.type.kind;
	switch (kind) {
	case H264SW_MB_I_PCM:
		error = recon_prepare_pcm(ctx, job);
		break;
	case H264SW_MB_I_4X4:
	case H264SW_MB_I_8X8:
	case H264SW_MB_I_16X16:
		error = recon_prepare_intra(ctx, job);
		break;
	case H264SW_MB_P_SKIP:
	case H264SW_MB_P:
	case H264SW_MB_P_8X8:
	case H264SW_MB_P_8X8REF0:
	case H264SW_MB_B_SKIP:
	case H264SW_MB_B_DIRECT:
	case H264SW_MB_B:
	case H264SW_MB_B_8X8:
		error = recon_prepare_inter(ctx, job);
		break;
	default:
		/* I_NxN is resolved by the parser, so any other kind is a broken context. */
		error = EINVAL;
		break;
	}

	/* Reports a macroblock that cannot be built at all. */
	if (error != 0)
		return error;

	/* Counts the macroblock once if any of its steps used a stand-in. */
	if (ctx->concealed > concealed_before)
		ctx->concealed = concealed_before + 1U;

	/* Records where the macroblock is and what its samples are made with. */
	mb = ctx->n.current;
	job->mb = mb;
	job->addr = ctx->n.addr;
	job->x = ctx->n.x;
	job->y = ctx->n.y;
	job->kind = (uint8_t)kind;
	job->qp = (uint8_t)recon_qp(mb);
	job->transform_8x8 = mb->transform_8x8;
	job->chroma_pred_mode = mb->chroma_pred_mode;

	/* Records the Intra_16x16 mode; a value too large for the byte stays one its predictor refuses. */
	mode = ctx->syntax.type.intra16x16_mode;
	if (mode > RECON_MODE_MAX)
		mode = RECON_MODE_MAX;
	job->intra16x16_mode = (uint8_t)mode;

	/* Succeeded: the work is recorded. */
	return 0;
}

/*
 * Reconstructs the samples of a prepared macroblock.
 *
 * The specification is the comment of the declaration in
 * h264sw-recon.h.
 */
void
h264sw_recon_samples(
	const struct h264sw_picture_ctx *picture,
	const struct h264sw_mb_job *job)
{
	/* Writes the samples the way the macroblock's kind says. */
	switch (job->kind) {
	case H264SW_MB_I_PCM:
		recon_samples_pcm(picture, job);
		break;
	case H264SW_MB_I_4X4:
		recon_samples_intra4x4(picture, job);
		recon_samples_intra_chroma(picture, job);
		break;
	case H264SW_MB_I_8X8:
		recon_samples_intra8x8(picture, job);
		recon_samples_intra_chroma(picture, job);
		break;
	case H264SW_MB_I_16X16:
		recon_samples_intra16x16(picture, job);
		recon_samples_intra_chroma(picture, job);
		break;
	default:
		/* Every other kind the preparation accepts is an inter kind. */
		recon_samples_inter(picture, job);
		break;
	}
}

/*
 * Empties a picture's stages for a new picture (or a dropped one).
 *
 * The specification is the comment of the declaration in
 * h264sw-recon.h.
 */
void
h264sw_recon_reset(
	struct h264sw_picture_ctx *picture)
{
	struct h264sw_recon_threads *threads;

	/* Without threads the counts are the caller's alone. */
	threads = picture->threads;
	if (threads == NULL) {
		/* Empties the counts at once. */
		recon_reset_counts(picture);
		return;
	}

	/* Gives up the work not taken, waits for the work taken, and empties the counts before the threads see the picture again. */
	media_os_mutex_lock(&threads->lock);

	/* Takes the picture from the threads: nothing more is taken, and the work taken is waited for. */
	threads->picture = NULL;
	while (threads->running > 0U) {
		/* Waits for a thread to finish what it took. */
		threads->waiting = 1;
		media_os_cond_wait(&threads->progress, &threads->lock);
		threads->waiting = 0;
	}

	/* Nothing is prepared, handed over or taken, and no row is being deblocked or has a copy of the picture. */
	threads->written = 0;
	threads->handed = 0;
	threads->free_seen = 0;
	threads->published = 0;
	threads->next_take = 0;
	threads->retired = 0;
	threads->slice_count = 0;
	threads->deblocking = 0;
	threads->view_ready = 0;

	/* Frees every place of the ring. */
	if (threads->state != NULL)
		memset(threads->state, RECON_JOB_WAITING, threads->state_capacity);

	/* Marks no macroblock in place. */
	if (threads->mb_done != NULL)
		memset(threads->mb_done, 0, threads->mb_done_capacity);

	/* Empties the picture's own counts. */
	recon_reset_counts(picture);

	/* The threads take the picture's work once it is handed over. */
	threads->picture = picture;

	media_os_mutex_unlock(&threads->lock);
}

/*
 * Puts the current macroblock through the stages of its picture.
 *
 * The specification is the comment of the declaration in
 * h264sw-recon.h.
 */
int
h264sw_recon_queue(
	struct h264sw_slice_ctx *ctx)
{
	struct h264sw_picture_ctx *picture;
	struct h264sw_mb_job *job;
	unsigned index;
	int error;

	/* Refuses a context whose picture has no ring of work. */
	if (ctx == NULL)
		return EINVAL;
	picture = ctx->picture;
	if (picture == NULL)
		return EINVAL;
	if (picture->jobs == NULL)
		return EINVAL;
	if (picture->job_capacity == 0U)
		return EINVAL;
	if (picture->rows_done == NULL)
		return EINVAL;

	/* With threads the parsing only prepares the work and hands it over. */
	if (picture->threads != NULL) {
		error = recon_queue_threads(ctx, picture->threads);
		if (error != 0)
			return error;

		/* Succeeded: the macroblock is in the stages. */
		return 0;
	}

	/* Makes room in a full ring by reconstructing what waits. */
	if (picture->job_count >= picture->job_capacity)
		h264sw_recon_drain(picture);

	/* Prepares the macroblock into the entry after the last one waiting; an error leaves the ring as it was. */
	index = (picture->job_head + picture->job_count) % picture->job_capacity;
	job = &picture->jobs[index];
	error = h264sw_recon_prepare(ctx, job);
	if (error != 0)
		return error;
	picture->job_count++;

	/* Reconstructs the samples of the work waiting, in decoding order. */
	h264sw_recon_drain(picture);

	/* Deblocks the rows that are ready. */
	recon_deblock_ready(picture);

	/* Succeeded: the macroblock is in the stages. */
	return 0;
}

/*
 * Reconstructs the samples of every macroblock still waiting in the ring of a picture.
 *
 * Each macroblock reconstructed counts in its row's rows_done.
 */
void
h264sw_recon_drain(
	struct h264sw_picture_ctx *picture)
{
	const struct h264sw_mb_job *job;

	/* A picture without a ring has nothing waiting. */
	if (picture->jobs == NULL)
		return;
	if (picture->job_capacity == 0U)
		return;

	/* With threads the caller works beside them until every macroblock is in place and no row is being deblocked. */
	if (picture->threads != NULL) {
		recon_help(picture->threads, picture, 1);
		return;
	}

	/* Takes the oldest work first, until none waits. */
	while (picture->job_count > 0U) {
		/* Writes the macroblock's samples. */
		job = &picture->jobs[picture->job_head];
		h264sw_recon_samples(picture, job);

		/* Counts the macroblock in its row. */
		if (picture->rows_done != NULL)
			picture->rows_done[job->y]++;

		/* Moves to the next entry of the ring. */
		picture->job_head = (picture->job_head + 1U) % picture->job_capacity;
		picture->job_count--;
	}
}

/*
 * Deblocks the rows of a picture not yet deblocked, in order.
 *
 * The specification is the comment of the declaration in
 * h264sw-recon.h.
 */
void
h264sw_recon_deblock_rest(
	struct h264sw_picture_ctx *picture)
{
	/* Filters the rows left when the picture is filtered. */
	if (picture->filter != 0)
		h264sw_deblock_rows(picture, picture->deblock_row, picture->height_mbs);

	/* Every row is deblocked now. */
	picture->deblock_row = picture->height_mbs;
}

/*
 * Starts the threads of a decoder's stages.
 *
 * The specification is the comment of the declaration in
 * h264sw-recon.h.  A mutex or condition that cannot be made counts as no
 * thread started.
 */
int
h264sw_recon_threads_start(
	unsigned count,
	const struct media_threads *calls,
	struct h264sw_recon_threads **made)
{
	struct h264sw_recon_threads *threads;
	struct recon_worker *worker;
	unsigned index;
	int error;

	/* Nothing can be returned without the place to return it. */
	if (made == NULL)
		return EINVAL;
	*made = NULL;

	/* One thread is the caller's alone, and no more than the most are started. */
	if (count <= 1U)
		return 0;
	if (count > H264SW_RECON_THREADS_MAX)
		count = H264SW_RECON_THREADS_MAX;

	/* Allocates the threads' state with every count cleared. */
	threads = calloc(1, sizeof(*threads));
	if (threads == NULL)
		return ENOMEM;

	/* Makes the mutex; without it the caller works alone. */
	error = media_os_mutex_init(&threads->lock);
	if (error != 0) {
		free(threads);
		return 0;
	}

	/* Makes the condition the workers wait for work on. */
	error = media_os_cond_init(&threads->work);
	if (error != 0) {
		media_os_mutex_destroy(&threads->lock);
		free(threads);
		return 0;
	}

	/* Makes the condition the parsing thread waits for progress on. */
	error = media_os_cond_init(&threads->progress);
	if (error != 0) {
		media_os_cond_destroy(&threads->work);
		media_os_mutex_destroy(&threads->lock);
		free(threads);
		return 0;
	}

	/* Keeps the caller's way of making threads. */
	threads->count = 1U;
	if (calls != NULL) {
		threads->calls = *calls;
		threads->given = 1;
	}

	/* Starts the workers; one that cannot start leaves the work to those started before it. */
	for (index = 0U;
	     index + 1U < count;
	     index++) {
		/* Starts the worker with the caller's calls when given, else as a system thread. */
		worker = &threads->workers[index];
		worker->owner = threads;
		if (threads->given) {
			error = threads->calls.start(threads->calls.context, recon_thread_names[index], recon_worker_main, worker, &worker->handle);
		} else {
			error = media_os_thread_start(&worker->thread, recon_worker_main, worker);
		}

		/* A worker that did not start ends the starting. */
		if (error != 0)
			break;

		/* The worker is running and is joined when the threads stop. */
		threads->count++;
	}

	/* No worker started: the caller works alone. */
	if (threads->count == 1U) {
		media_os_cond_destroy(&threads->progress);
		media_os_cond_destroy(&threads->work);
		media_os_mutex_destroy(&threads->lock);
		free(threads);
		return 0;
	}

	/* Succeeded: the workers wait for the first picture's work. */
	*made = threads;
	return 0;
}

/*
 * Stops the threads of a decoder's stages and frees them.
 *
 * The workers are told to end and woken, then joined in order.
 */
void
h264sw_recon_threads_stop(
	struct h264sw_recon_threads *threads)
{
	struct recon_worker *worker;
	unsigned index;

	/* Nothing to stop. */
	if (threads == NULL)
		return;

	/* Tells every worker to end, waking those that wait for work. */
	media_os_mutex_lock(&threads->lock);

	threads->quit = 1;
	media_os_cond_broadcast(&threads->work);

	media_os_mutex_unlock(&threads->lock);

	/* Waits for each worker to end, the way it was started. */
	for (index = 0U;
	     index + 1U < threads->count;
	     index++) {
		/* Joins the worker with the caller's calls when given, else as a system thread. */
		worker = &threads->workers[index];
		if (threads->given) {
			threads->calls.join(threads->calls.context, worker->handle);
		} else {
			media_os_thread_join(&worker->thread);
		}
	}

	/* Frees the records, the conditions, the mutex and the state. */
	media_os_cond_destroy(&threads->progress);
	media_os_cond_destroy(&threads->work);
	media_os_mutex_destroy(&threads->lock);
	free(threads->state);
	free(threads->mb_done);
	free(threads);
}

/*
 * Tells how many threads do a picture's work, the caller's included.
 */
unsigned
h264sw_recon_threads_count(
	const struct h264sw_recon_threads *threads)
{
	/* Without threads the caller does it all. */
	if (threads == NULL)
		return 1U;

	/* The workers and the caller. */
	return threads->count;
}

/*
 * Sizes the threads' records for a ring and a picture.
 *
 * The records only grow; no picture is in the threads' hands, so no
 * worker reads them meanwhile.
 */
int
h264sw_recon_threads_size(
	struct h264sw_recon_threads *threads,
	unsigned job_capacity,
	unsigned size_mbs)
{
	uint8_t *state;
	uint8_t *mb_done;

	/* Without threads there is nothing to size. */
	if (threads == NULL)
		return 0;

	/* Grows the state of the places of the ring, all free. */
	if (threads->state_capacity < job_capacity) {
		state = calloc(job_capacity, 1);
		if (state == NULL)
			return ENOMEM;
		free(threads->state);
		threads->state = state;
		threads->state_capacity = job_capacity;
	}

	/* Grows the marks of the macroblocks in place, none set. */
	if (threads->mb_done_capacity < size_mbs) {
		mb_done = calloc(size_mbs, 1);
		if (mb_done == NULL)
			return ENOMEM;
		free(threads->mb_done);
		threads->mb_done = mb_done;
		threads->mb_done_capacity = size_mbs;
	}

	/* Succeeded: the records fit the sequence. */
	return 0;
}

/* Checks that the context holds a current macroblock inside a frame that has room for it. */
static int
recon_check(
	const struct h264sw_slice_ctx *ctx)
{
	const struct h264sw_picture_ctx *picture;
	const struct h264sw_frame *frame;
	unsigned plane;
	unsigned right;
	unsigned bottom;

	/* Refuses a missing context, picture or current macroblock entry. */
	if (ctx == NULL)
		return EINVAL;
	if (ctx->picture == NULL)
		return EINVAL;
	if (ctx->n.current == NULL)
		return EINVAL;

	/* Refuses a missing frame or a macroblock outside the picture. */
	picture = ctx->picture;
	frame = picture->frame;
	if (frame == NULL)
		return EINVAL;
	if (ctx->n.x >= picture->width_mbs)
		return EINVAL;
	if (ctx->n.y >= picture->height_mbs)
		return EINVAL;

	/* Refuses a frame that is smaller than the macroblock's last sample. */
	right = 16U * (ctx->n.x + 1U);
	bottom = 16U * (ctx->n.y + 1U);
	if (right > frame->width)
		return EINVAL;
	if (bottom > frame->height)
		return EINVAL;

	/* Refuses a frame whose planes are missing. */
	for (plane = 0; plane < 3U; plane++) {
		if (frame->plane[plane] == NULL)
			return EINVAL;
	}

	/* Refuses a frame whose rows overlap. */
	if (frame->stride[0] < frame->width)
		return EINVAL;
	if (frame->stride[1] < frame->width / 2U)
		return EINVAL;
	if (frame->stride[2] < frame->width / 2U)
		return EINVAL;

	/* Succeeded: the macroblock can be written. */
	return 0;
}

/* Divides rounding toward minus infinity, as a right shift of a negative value would in the clauses; the divisor is positive. */
static int
recon_floor_div(
	int value,
	int divisor)
{
	/* A non-negative value divides exactly as the language does. */
	if (value >= 0)
		return value / divisor;

	/* A negative value rounds down: negate, round up, negate again. */
	return -((-value + divisor - 1) / divisor);
}

/* Wraps a sum of a prediction and a difference, which lies within one span of the range, into -32768..32767 by arithmetic. */
static int
recon_wrap_vector(
	int value)
{
	/* A sum below the range comes back up by one span. */
	if (value < RECON_VECTOR_MIN)
		value += RECON_VECTOR_SPAN;

	/* A sum above the range comes back down by one span. */
	if (value > RECON_VECTOR_MAX)
		value -= RECON_VECTOR_SPAN;

	/* The wrapped sum fits an int16_t. */
	return value;
}

/* Gives QPY of the macroblock, held within 0..51 so that the scaling and the tables are indexed safely. */
static int
recon_qp(
	const struct h264sw_mb *mb)
{
	int qp;

	/* The entry keeps QPY in a byte; a broken value is brought to the largest QPY. */
	qp = (int)mb->qp;
	if (qp > RECON_QP_MAX)
		qp = RECON_QP_MAX;

	/* The quantization parameter of the luma blocks. */
	return qp;
}

/* Gives QP'C of a chroma component from QPY, by qPI of 8-309 to 8-312 and Table 8-15; component is 0 for Cb and 1 for Cr. */
static int
recon_chroma_qp(
	const struct h264sw_picture_ctx *picture,
	int qp,
	unsigned component)
{
	int qpi;

	/* qPI is QPY plus the component's offset, clipped to 0..51 (QpBdOffsetC is 0 for 8 bits). */
	qpi = qp + picture->chroma_qp_offset[component];
	if (qpi < 0)
		qpi = 0;
	if (qpi > RECON_QP_MAX)
		qpi = RECON_QP_MAX;

	/* The table gives QPC for qPI. */
	return (int)h264sw_chroma_qp[qpi];
}

/* Tells whether any of count levels is not zero. */
static int
recon_has_level(
	const int32_t *levels,
	unsigned count)
{
	uint32_t bits;
	unsigned index;

	/* Gathers the bits of every level, without a branch per level; most blocks are all zero. */
	bits = 0;

	/* Visits each level once. */
	for (index = 0; index < count; index++)
		bits = bits | (uint32_t)levels[index];

	/* A level that is not zero left a bit. */
	if (bits != 0U)
		return 1;

	/* Every level is zero. */
	return 0;
}

/* Tells whether a partition's prediction mode uses a reference list. */
static int
recon_uses_list(
	unsigned pred,
	unsigned list)
{
	/* Bi-prediction uses both lists. */
	if (pred == H264SW_PRED_BI)
		return 1;

	/* List 0 prediction uses list 0 only. */
	if (pred == H264SW_PRED_L0 && list == 0U)
		return 1;

	/* List 1 prediction uses list 1 only. */
	if (pred == H264SW_PRED_L1 && list == 1U)
		return 1;

	/* Direct prediction and no prediction have nothing to derive here. */
	return 0;
}

/* Gives the address of the sample (x, y) of a plane of a frame. */
static uint8_t *
recon_sample_at(
	const struct h264sw_frame *frame,
	unsigned plane,
	unsigned x,
	unsigned y)
{
	/* The row starts stride bytes after the one above. */
	return frame->plane[plane] + (size_t)y * frame->stride[plane] + x;
}

/* Copies a size by size block of samples into a plane. */
static void
recon_put_block(
	uint8_t *dst,
	size_t dst_stride,
	const uint8_t *src,
	unsigned size)
{
	unsigned row;

	/* Copies the block row by row; the source rows are size bytes apart. */
	for (row = 0; row < size; row++)
		memcpy(dst + (size_t)row * dst_stride, src + (size_t)row * size, size);
}

/* Narrows a 4x4 block's 16 levels into the record, in scan order; the parser kept each within int16_t. */
static void
recon_store4x4(
	const int32_t *levels,
	int16_t *out)
{
	unsigned index;

	/* Copies each level into its 16 bits. */
	for (index = 0; index < 16U; index++)
		out[index] = (int16_t)levels[index];
}

/* Narrows an 8x8 block's 64 levels into the record, in scan order; the parser kept each within int16_t. */
static void
recon_store8x8(
	const int32_t *levels,
	int16_t *out)
{
	unsigned index;

	/* Copies each level into its 16 bits. */
	for (index = 0; index < 64U; index++)
		out[index] = (int16_t)levels[index];
}

/* Prepares an I_PCM macroblock: its samples go into the record (8.3.5). */
static int
recon_prepare_pcm(
	struct h264sw_slice_ctx *ctx,
	struct h264sw_mb_job *job)
{
	struct h264sw_mb *mb;

	/* Keeps the 384 samples the parser read: 256 luma, then 64 Cb and 64 Cr, each in raster order. */
	memcpy(job->u.pcm, ctx->syntax.pcm, sizeof(job->u.pcm));

	/* Records the motion of an intra macroblock, and that every block counts as having coefficients for the deblocking filter. */
	mb = ctx->n.current;
	mb->motion.intra = 1;
	mb->nonzero = (uint16_t)RECON_NONZERO_ALL;

	/* Succeeded: the samples are recorded. */
	return 0;
}

/* Prepares an Intra_4x4, Intra_8x8 or Intra_16x16 macroblock: its modes, its levels and the neighbours its prediction reads (8.3, 8.5). */
static int
recon_prepare_intra(
	struct h264sw_slice_ctx *ctx,
	struct h264sw_mb_job *job)
{
	struct h264sw_mb *mb;
	unsigned parts;
	int usable;

	/* An intra macroblock uses no reference list. */
	mb = ctx->n.current;
	mb->motion.intra = 1;

	/* Finds the neighbouring macroblocks the prediction may read. */
	job->neighbours = (uint8_t)h264sw_intra_neighbours(ctx);

	/* Derives the luma modes and records the luma levels by the macroblock's intra kind. */
	if (ctx->syntax.type.kind == H264SW_MB_I_4X4) {
		recon_prepare_intra4x4(ctx, job);
		recon_prepare_luma4x4(ctx, job);
	} else if (ctx->syntax.type.kind == H264SW_MB_I_8X8) {
		recon_prepare_intra8x8(ctx, job);
		recon_prepare_luma8x8(ctx, job);
	} else {
		recon_prepare_intra16x16(ctx, job);
		recon_prepare_luma4x4(ctx, job);
	}

	/* A chroma mode whose samples are not available gives a flat prediction; the stream is broken. */
	parts = h264sw_intra_parts(job->neighbours, 16U, 0U);
	usable = h264sw_intra_usable(H264SW_INTRA_CHROMA, mb->chroma_pred_mode, parts);
	if (usable == 0)
		ctx->concealed++;

	/* Records the chroma levels. */
	recon_prepare_chroma(ctx, job);

	/* Succeeded: the macroblock's work is recorded. */
	return 0;
}

/* Derives the modes of the sixteen 4x4 blocks of an Intra_4x4 macroblock in luma4x4BlkIdx order (8.3.1.1). */
static void
recon_prepare_intra4x4(
	struct h264sw_slice_ctx *ctx,
	struct h264sw_mb_job *job)
{
	struct h264sw_mb *mb;
	const struct h264sw_mb_syntax *syntax;
	unsigned blk;
	unsigned mode;
	unsigned parts;
	int usable;

	/* Takes the entry being written and the syntax. */
	mb = ctx->n.current;
	syntax = &ctx->syntax;

	/* Derives each block's mode before the next, which reads it as a neighbour. */
	for (blk = 0; blk < 16U; blk++) {
		/* Derives the block's mode and keeps it for the blocks and macroblocks after it, and for the samples. */
		mode = h264sw_intra4x4_mode(ctx, blk, syntax->prev_intra_flag[blk], syntax->rem_intra_mode[blk]);
		mb->intra4x4_mode[blk] = (uint8_t)mode;
		job->intra_mode[blk] = (uint8_t)mode;

		/* A mode whose samples are not available gives a flat prediction; the stream is broken. */
		parts = h264sw_intra_parts(job->neighbours, 4U, blk);
		usable = h264sw_intra_usable(H264SW_INTRA_NXN, mode, parts);
		if (usable == 0)
			ctx->concealed++;
	}
}

/* Derives the modes of the four 8x8 blocks of an Intra_8x8 macroblock in luma8x8BlkIdx order (8.3.2.1). */
static void
recon_prepare_intra8x8(
	struct h264sw_slice_ctx *ctx,
	struct h264sw_mb_job *job)
{
	struct h264sw_mb *mb;
	const struct h264sw_mb_syntax *syntax;
	unsigned blk;
	unsigned part;
	unsigned mode;
	unsigned parts;
	int usable;

	/* Takes the entry being written and the syntax. */
	mb = ctx->n.current;
	syntax = &ctx->syntax;

	/* Derives each block's mode before the next, which reads it as a neighbour. */
	for (blk = 0; blk < 4U; blk++) {
		/* Derives the block's mode and keeps it in each of the block's four 4x4 entries. */
		mode = h264sw_intra8x8_mode(ctx, blk, syntax->prev_intra_flag[blk], syntax->rem_intra_mode[blk]);

		/* Writes the mode into the four 4x4 entries of the block. */
		for (part = 0; part < 4U; part++) {
			mb->intra4x4_mode[4U * blk + part] = (uint8_t)mode;
			job->intra_mode[4U * blk + part] = (uint8_t)mode;
		}

		/* A mode whose samples are not available gives a flat prediction; the stream is broken. */
		parts = h264sw_intra_parts(job->neighbours, 8U, blk);
		usable = h264sw_intra_usable(H264SW_INTRA_NXN, mode, parts);
		if (usable == 0)
			ctx->concealed++;
	}
}

/* Checks the mode of an Intra_16x16 macroblock and records its DC levels (8.3.3, 8.5.2). */
static void
recon_prepare_intra16x16(
	struct h264sw_slice_ctx *ctx,
	struct h264sw_mb_job *job)
{
	struct h264sw_mb *mb;
	const struct h264sw_mb_syntax *syntax;
	unsigned parts;
	int usable;
	int has_dc;

	/* Takes the entry being written and the syntax. */
	mb = ctx->n.current;
	syntax = &ctx->syntax;

	/* A mode whose samples are not available gives a flat prediction; the stream is broken. */
	parts = h264sw_intra_parts(job->neighbours, 16U, 0U);
	usable = h264sw_intra_usable(H264SW_INTRA_16X16, syntax->type.intra16x16_mode, parts);
	if (usable == 0)
		ctx->concealed++;

	/* A DC level that is not zero counts for every block's boundary strength; the levels go into the record. */
	has_dc = recon_has_level(syntax->luma_dc, 16U);
	if (has_dc != 0) {
		mb->nonzero = (uint16_t)RECON_NONZERO_ALL;
		recon_store4x4(syntax->luma_dc, job->luma_dc);
		job->coded = job->coded | ((uint32_t)1U << H264SW_JOB_LUMA_DC);
	}
}

/* Records the levels of each luma 4x4 block that has one, and notes the block as having coefficients (8.5.1, 8.5.2, 8.7.2.1). */
static void
recon_prepare_luma4x4(
	struct h264sw_slice_ctx *ctx,
	struct h264sw_mb_job *job)
{
	struct h264sw_mb *mb;
	const struct h264sw_mb_syntax *syntax;
	unsigned blk;
	int has_level;

	/* Takes the entry being written and the syntax. */
	mb = ctx->n.current;
	syntax = &ctx->syntax;

	/* Visits the sixteen blocks; a block without a level adds nothing. */
	for (blk = 0; blk < 16U; blk++) {
		has_level = recon_has_level(syntax->luma[blk], 16U);
		if (has_level == 0)
			continue;
		recon_store4x4(syntax->luma[blk], &job->u.luma[16U * blk]);
		job->coded = job->coded | ((uint32_t)1U << blk);
		mb->nonzero = (uint16_t)(mb->nonzero | (1U << blk));
	}
}

/* Records the levels of each luma 8x8 block that has one, and notes its four 4x4 blocks as having coefficients (8.5.3, 8.7.2.1). */
static void
recon_prepare_luma8x8(
	struct h264sw_slice_ctx *ctx,
	struct h264sw_mb_job *job)
{
	struct h264sw_mb *mb;
	const struct h264sw_mb_syntax *syntax;
	unsigned blk;
	int has_level;

	/* Takes the entry being written and the syntax. */
	mb = ctx->n.current;
	syntax = &ctx->syntax;

	/* Visits the four blocks; a block without a level adds nothing. */
	for (blk = 0; blk < 4U; blk++) {
		has_level = recon_has_level(syntax->luma8x8[blk], 64U);
		if (has_level == 0)
			continue;
		recon_store8x8(syntax->luma8x8[blk], &job->u.luma[64U * blk]);
		job->coded = job->coded | ((uint32_t)1U << blk);
		mb->nonzero = (uint16_t)(mb->nonzero | (0xfU << (4U * blk)));
	}
}

/* Records the chroma levels of both components: the DC of a component with any level, and each AC block that has one (8.5.4). */
static void
recon_prepare_chroma(
	struct h264sw_slice_ctx *ctx,
	struct h264sw_mb_job *job)
{
	const struct h264sw_mb_syntax *syntax;
	unsigned component;
	unsigned blk;
	unsigned index;
	unsigned bit;
	int any;
	int has_level;

	/* Takes the syntax. */
	syntax = &ctx->syntax;

	/* Each component has its own DC block and four AC blocks. */
	for (component = 0; component < 2U; component++) {
		/* Records each AC block that has a level. */
		any = recon_has_level(syntax->chroma_dc[component], 4U);
		for (blk = 0; blk < 4U; blk++) {
			has_level = recon_has_level(syntax->chroma_ac[component][blk], 16U);
			if (has_level == 0)
				continue;
			any = 1;
			recon_store4x4(syntax->chroma_ac[component][blk], job->chroma_ac[component][blk]);
			bit = H264SW_JOB_CHROMA_AC + 4U * component + blk;
			job->coded = job->coded | ((uint32_t)1U << bit);
		}

		/* A component whose levels are all zero adds nothing to the prediction. */
		if (any == 0)
			continue;

		/* Records the component and its four DC levels, a 2x2 matrix in raster order (8.5.11). */
		job->coded = job->coded | ((uint32_t)1U << (H264SW_JOB_CHROMA + component));
		for (index = 0; index < 4U; index++)
			job->chroma_dc[component][index] = (int16_t)syntax->chroma_dc[component][index];
	}
}

/* Prepares an inter macroblock: its motion, the references of its predicted blocks, then its levels (8.4, 8.5). */
static int
recon_prepare_inter(
	struct h264sw_slice_ctx *ctx,
	struct h264sw_mb_job *job)
{
	int error;

	/* Derives the motion vectors and reference indices of every partition. */
	error = recon_motion(ctx);
	if (error != 0)
		return error;

	/* Checks and records the references and the weights of the blocks the macroblock is predicted in. */
	error = recon_prepare_refs(ctx, job);
	if (error != 0)
		return error;

	/* Records the luma levels by the transform size. */
	if (ctx->n.current->transform_8x8 != 0)
		recon_prepare_luma8x8(ctx, job);
	else
		recon_prepare_luma4x4(ctx, job);

	/* Records the chroma levels. */
	recon_prepare_chroma(ctx, job);

	/* Succeeded: the macroblock's work is recorded. */
	return 0;
}

/*
 * Cuts the macroblock into the largest squares that share one motion and prepares the references of each (8.4.2).
 *
 * The squares are visited in the order their samples are predicted:
 * the whole macroblock, or each 8x8 quadrant, or the four 4x4 blocks of
 * a quadrant whose blocks move differently.  Each square checks its
 * references and counts a stand-in, in that order.
 */
static int
recon_prepare_refs(
	struct h264sw_slice_ctx *ctx,
	struct h264sw_mb_job *job)
{
	const struct h264sw_mb *mb;
	unsigned quadrant;
	unsigned blocks;
	int uniform;
	int error;

	/* Takes the entry being read. */
	mb = ctx->n.current;

	/* A macroblock that moves as one is predicted as one 16x16 block, with the references of its first quadrant. */
	uniform = recon_uniform(mb, 0U, 0U, 16U);
	if (uniform != 0) {
		job->layout = (uint8_t)H264SW_JOB_WHOLE;
		error = recon_prepare_quadrant(ctx, job, 0U, 1U);
		if (error != 0)
			return error;

		/* Succeeded: the macroblock is one block. */
		return 0;
	}

	/* Otherwise each quadrant that moves as one is one block, and the others four blocks. */
	for (quadrant = 0; quadrant < 4U; quadrant++) {
		blocks = 4U;
		uniform = recon_uniform(mb, 8U * (quadrant % 2U), 8U * (quadrant / 2U), 8U);
		if (uniform != 0) {
			job->layout = (uint8_t)(job->layout | (1U << quadrant));
			blocks = 1U;
		}
		error = recon_prepare_quadrant(ctx, job, quadrant, blocks);
		if (error != 0)
			return error;
	}

	/* Succeeded: every quadrant's references are recorded. */
	return 0;
}

/*
 * Checks and records the references and the weights of one 8x8 quadrant.
 *
 * blocks is the number of blocks the quadrant is predicted in (1 or 4):
 * each checks the references of the lists it uses and counts a stand-in
 * as concealed, as each block's prediction reads them.  Then the frame of
 * each list (NULL when unused) and the factors of luma, Cb and Cr go into
 * the record.
 */
static int
recon_prepare_quadrant(
	struct h264sw_slice_ctx *ctx,
	struct h264sw_mb_job *job,
	unsigned quadrant,
	unsigned blocks)
{
	const struct h264sw_mb *mb;
	unsigned block;
	unsigned list;
	unsigned component;
	int ref_idx[2];
	int error;

	/* Takes the reference index of each list from the quadrant. */
	mb = ctx->n.current;
	ref_idx[0] = mb->motion.ref_idx[0][quadrant];
	ref_idx[1] = mb->motion.ref_idx[1][quadrant];

	/* Refuses a quadrant that predicts from no list. */
	if (ref_idx[0] < 0 && ref_idx[1] < 0)
		return EINVAL;

	/* Checks the references of each block, list 0 then list 1. */
	for (block = 0; block < blocks; block++) {
		/* Checks each list the quadrant uses. */
		for (list = 0; list < 2U; list++) {
			if (ref_idx[list] < 0)
				continue;
			error = recon_check_ref(ctx, list, ref_idx[list]);
			if (error != 0)
				return error;
		}
	}

	/* Records the frame of each list the quadrant uses. */
	for (list = 0; list < 2U; list++) {
		job->ref[quadrant][list] = NULL;
		if (ref_idx[list] >= 0)
			job->ref[quadrant][list] = ctx->lists.list[list][ref_idx[list]].frame;
	}

	/* Records the factors of luma, Cb and Cr (8.4.2.3). */
	for (component = 0; component < 3U; component++)
		h264sw_weights_of(&ctx->weights, ref_idx[0], ref_idx[1], component, &job->weights[quadrant][component]);

	/* Succeeded: the quadrant's references are recorded. */
	return 0;
}

/* Checks one reference of a predicted block: inside the list, a frame of the current frame's size with its planes; a stand-in counts as concealed. */
static int
recon_check_ref(
	struct h264sw_slice_ctx *ctx,
	unsigned list,
	int ref_idx)
{
	const struct h264sw_frame *frame;
	const struct h264sw_ref *ref;

	/* Refuses an index outside the list, a missing frame and a frame of another size. */
	frame = ctx->picture->frame;
	if ((unsigned)ref_idx >= ctx->lists.count[list])
		return EINVAL;
	ref = &ctx->lists.list[list][ref_idx];
	if (ref->frame == NULL)
		return EINVAL;
	if (ref->frame->plane[0] == NULL)
		return EINVAL;
	if (ref->frame->plane[1] == NULL)
		return EINVAL;
	if (ref->frame->plane[2] == NULL)
		return EINVAL;
	if (ref->frame->width != frame->width)
		return EINVAL;
	if (ref->frame->height != frame->height)
		return EINVAL;

	/* A position the lists could not fill is predicted from a stand-in, and the macroblock counts as concealed. */
	if (ref->missing != 0)
		ctx->concealed++;

	/* Succeeded: the reference can be predicted from. */
	return 0;
}

/* Derives the motion of the macroblock by its kind and marks all its 4x4 blocks as derived (8.4.1). */
static int
recon_motion(
	struct h264sw_slice_ctx *ctx)
{
	unsigned kind;
	int error;

	/* Takes the kind the derivation is picked by. */
	kind = ctx->syntax.type.kind;

	/* Picks the derivation the macroblock's kind needs. */
	if (kind == H264SW_MB_P_SKIP) {
		error = recon_motion_pskip(ctx);
	} else if (kind == H264SW_MB_B_SKIP || kind == H264SW_MB_B_DIRECT) {
		error = recon_motion_direct(ctx);
	} else if (kind == H264SW_MB_P || kind == H264SW_MB_B) {
		error = recon_motion_parts(ctx);
	} else {
		error = recon_motion_sub(ctx);
	}

	/* Reports a macroblock whose motion cannot be derived. */
	if (error != 0)
		return error;

	/* Every 4x4 block now has its motion. */
	ctx->decoded_4x4 = RECON_DECODED_ALL;

	/* Succeeded: the motion is in the macroblock's entry. */
	return 0;
}

/* Derives the motion of a P_Skip macroblock: reference index 0 of list 0 and the vector of 8.4.1.1 for all sixteen blocks. */
static int
recon_motion_pskip(
	struct h264sw_slice_ctx *ctx)
{
	struct h264sw_motion *motion;
	const struct h264sw_frame *frame;
	int16_t mv[2];
	unsigned blk;
	unsigned quadrant;

	/* Takes the motion record of the macroblock being written. */
	motion = &ctx->n.current->motion;

	/* Refuses a slice whose list 0 has no first entry to predict from. */
	if (ctx->lists.count[0] == 0U)
		return EINVAL;
	frame = ctx->lists.list[0][0].frame;
	if (frame == NULL)
		return EINVAL;

	/* Derives the vector from the neighbours A and B (8.4.1.1). */
	h264sw_mvpred_pskip(ctx, mv);

	/* Gives all sixteen blocks the vector. */
	for (blk = 0; blk < 16U; blk++) {
		motion->mv[0][blk][0] = mv[0];
		motion->mv[0][blk][1] = mv[1];
	}

	/* Gives every quadrant reference index 0 and the id of the first frame of list 0; list 1 stays unused. */
	for (quadrant = 0; quadrant < 4U; quadrant++) {
		motion->ref_idx[0][quadrant] = 0;
		motion->ref_id[0][quadrant] = frame->id;
	}

	/* Succeeded: the skipped macroblock's motion is derived. */
	return 0;
}

/* Derives the motion of a B_Skip or B_Direct_16x16 macroblock by the direct prediction of each of its four quadrants (8.4.1.2). */
static int
recon_motion_direct(
	struct h264sw_slice_ctx *ctx)
{
	unsigned quadrant;
	int error;

	/* Derives the quadrants in increasing order, as the direct prediction requires. */
	for (quadrant = 0; quadrant < 4U; quadrant++) {
		error = h264sw_direct_quadrant(ctx, quadrant);
		if (error != 0)
			return error;
	}

	/* Succeeded: all four quadrants are derived. */
	return 0;
}

/* Derives the motion of a macroblock of one or two macroblock partitions (16x16, 16x8 or 8x16) in mbPartIdx order (8.4.1). */
static int
recon_motion_parts(
	struct h264sw_slice_ctx *ctx)
{
	const struct h264sw_mb_syntax *syntax;
	unsigned part;
	unsigned x;
	unsigned y;
	unsigned width;
	unsigned height;
	int error;

	/* Takes the syntax and the size of the partitions. */
	syntax = &ctx->syntax;
	width = syntax->type.part_width;
	height = syntax->type.part_height;

	/* Refuses a type that does not describe one or two partitions inside the macroblock. */
	if (syntax->type.parts == 0U || syntax->type.parts > 2U)
		return EINVAL;
	if (width == 0U || width > 16U)
		return EINVAL;
	if (height == 0U || height > 16U)
		return EINVAL;

	/* Derives each partition; their indices place them in raster order. */
	for (part = 0; part < syntax->type.parts; part++) {
		x = (part * width) % 16U;
		y = ((part * width) / 16U) * height;
		error = recon_partition(ctx, syntax->type.pred[part], part, 0U, x, y, width, height);
		if (error != 0)
			return error;
	}

	/* Succeeded: both partitions are derived. */
	return 0;
}

/* Derives the motion of a macroblock of four sub-macroblocks, partition by partition in mbPartIdx and subMbPartIdx order (8.4.1). */
static int
recon_motion_sub(
	struct h264sw_slice_ctx *ctx)
{
	const struct h264sw_mb_syntax *syntax;
	const struct h264sw_sub_mb_type *sub;
	unsigned quadrant;
	unsigned part;
	unsigned x;
	unsigned y;
	int error;

	/* Takes the syntax. */
	syntax = &ctx->syntax;

	/* Derives each sub-macroblock in order, so that the later ones see the motion of the earlier ones. */
	for (quadrant = 0; quadrant < 4U; quadrant++) {
		sub = &syntax->sub[quadrant];

		/* A direct sub-macroblock takes its motion from the direct prediction of its quadrant (8.4.1.2). */
		if (sub->pred == H264SW_PRED_DIRECT) {
			error = h264sw_direct_quadrant(ctx, quadrant);
			if (error != 0)
				return error;

			/* Marks the quadrant's blocks as having their motion for the sub-macroblocks after it. */
			recon_mark_decoded(ctx, 8U * (quadrant % 2U), 8U * (quadrant / 2U), 8U, 8U);
			continue;
		}

		/* Refuses a sub-macroblock type that does not describe 1, 2 or 4 partitions inside the 8x8 block. */
		if (sub->parts == 0U || sub->parts > 4U)
			return EINVAL;
		if (sub->part_width != 4U && sub->part_width != 8U)
			return EINVAL;
		if (sub->part_height != 4U && sub->part_height != 8U)
			return EINVAL;

		/* Derives each sub-macroblock partition, placed in raster order inside the 8x8 block. */
		for (part = 0; part < sub->parts; part++) {
			x = 8U * (quadrant % 2U) + (part * sub->part_width) % 8U;
			y = 8U * (quadrant / 2U) + ((part * sub->part_width) / 8U) * sub->part_height;
			error = recon_partition(ctx, sub->pred, quadrant, part, x, y, sub->part_width, sub->part_height);
			if (error != 0)
				return error;
		}
	}

	/* Succeeded: all four sub-macroblocks are derived. */
	return 0;
}

/*
 * Derives the vectors of one partition for each list its prediction mode uses.
 *
 * pred is the partition's prediction mode; part is its mbPartIdx and sub
 * its subMbPartIdx (0 for a macroblock partition), which select its
 * reference indices and motion vector differences in the syntax.  (x, y)
 * is its upper-left luma sample inside the macroblock.
 */
static int
recon_partition(
	struct h264sw_slice_ctx *ctx,
	unsigned pred,
	unsigned part,
	unsigned sub,
	unsigned x,
	unsigned y,
	unsigned width,
	unsigned height)
{
	const struct h264sw_mb_syntax *syntax;
	unsigned list;
	int uses;
	int used;
	int error;

	/* Takes the syntax. */
	syntax = &ctx->syntax;

	/* Refuses a partition that is not made of whole 4x4 blocks inside the macroblock, whatever the type says. */
	if (width == 0U || height == 0U)
		return EINVAL;
	if (width % 4U != 0U || height % 4U != 0U)
		return EINVAL;
	if (x % 4U != 0U || y % 4U != 0U)
		return EINVAL;
	if (x + width > 16U || y + height > 16U)
		return EINVAL;
	if (part >= 4U || sub >= 4U)
		return EINVAL;

	/* Derives the list 0 vector, then the list 1 vector, of the lists the partition uses. */
	used = 0;
	for (list = 0; list < 2U; list++) {
		uses = recon_uses_list(pred, list);
		if (uses == 0)
			continue;
		used = 1;
		error = recon_vector(ctx, list, syntax->ref_idx[list][part], syntax->mvd[list][part][sub], x, y, width, height);
		if (error != 0)
			return error;
	}

	/* Refuses a partition that predicts from no list: it has no motion to give. */
	if (used == 0)
		return EINVAL;

	/* The partition's blocks now have their motion for the partitions after it. */
	recon_mark_decoded(ctx, x, y, width, height);

	/* Succeeded: the partition's vectors are derived. */
	return 0;
}

/* Derives the vector and records the reference of one partition for one list: mvLX = mvpLX + mvdLX (8-172, 8-173). */
static int
recon_vector(
	struct h264sw_slice_ctx *ctx,
	unsigned list,
	int ref_idx,
	const int16_t *mvd,
	unsigned x,
	unsigned y,
	unsigned width,
	unsigned height)
{
	struct h264sw_motion *motion;
	const struct h264sw_frame *frame;
	int16_t mvp[2];
	int mv_x;
	int mv_y;
	unsigned block_x;
	unsigned block_y;
	unsigned blk;
	unsigned quadrant;

	/* Takes the motion record of the macroblock being written. */
	motion = &ctx->n.current->motion;

	/* Refuses a reference index outside the list or naming no frame. */
	if (ref_idx < 0)
		return EINVAL;
	if ((unsigned)ref_idx >= ctx->lists.count[list])
		return EINVAL;
	frame = ctx->lists.list[list][ref_idx].frame;
	if (frame == NULL)
		return EINVAL;

	/* Predicts the vector from the neighbouring partitions (8.4.1.3). */
	h264sw_mvpred(ctx, list, x, y, width, height, ref_idx, mvp);

	/* Adds the difference in int and wraps the sum into the range of int16_t by arithmetic. */
	mv_x = recon_wrap_vector((int)mvp[0] + (int)mvd[0]);
	mv_y = recon_wrap_vector((int)mvp[1] + (int)mvd[1]);

	/* Gives every 4x4 block of the partition the vector, and its quadrant the reference index and the id of the frame. */
	for (block_y = y; block_y < y + height; block_y += 4U) {
		for (block_x = x; block_x < x + width; block_x += 4U) {
			blk = h264sw_blk4x4_at(block_x, block_y);
			motion->mv[list][blk][0] = (int16_t)mv_x;
			motion->mv[list][blk][1] = (int16_t)mv_y;
			quadrant = 2U * (block_y / 8U) + block_x / 8U;
			motion->ref_idx[list][quadrant] = (int8_t)ref_idx;
			motion->ref_id[list][quadrant] = frame->id;
		}
	}

	/* Succeeded: the partition's vector is stored. */
	return 0;
}

/* Marks the 4x4 blocks of a rectangle as having their motion derived, so that the partitions after it may use them as neighbours. */
static void
recon_mark_decoded(
	struct h264sw_slice_ctx *ctx,
	unsigned x,
	unsigned y,
	unsigned width,
	unsigned height)
{
	unsigned block_x;
	unsigned block_y;
	unsigned blk;

	/* Visits each 4x4 block of the rectangle. */
	for (block_y = y; block_y < y + height; block_y += 4U) {
		for (block_x = x; block_x < x + width; block_x += 4U) {
			blk = h264sw_blk4x4_at(block_x, block_y);
			ctx->decoded_4x4 |= (uint32_t)1U << blk;
		}
	}
}

/* Tells whether all 4x4 blocks of a size by size square of the macroblock have the same vectors and reference indices, so one prediction covers them. */
static int
recon_uniform(
	const struct h264sw_mb *mb,
	unsigned x,
	unsigned y,
	unsigned size)
{
	unsigned first;
	unsigned first_quadrant;
	unsigned blk;
	unsigned quadrant;
	unsigned list;
	unsigned block_x;
	unsigned block_y;

	/* Finds the first block of the square and its quadrant, which the others are compared with. */
	first = h264sw_blk4x4_at(x, y);
	first_quadrant = 2U * (y / 8U) + x / 8U;

	/* Compares each block of the square with the first. */
	for (block_y = y; block_y < y + size; block_y += 4U) {
		for (block_x = x; block_x < x + size; block_x += 4U) {
			blk = h264sw_blk4x4_at(block_x, block_y);
			quadrant = 2U * (block_y / 8U) + block_x / 8U;
			for (list = 0; list < 2U; list++) {
				if (mb->motion.mv[list][blk][0] != mb->motion.mv[list][first][0])
					return 0;
				if (mb->motion.mv[list][blk][1] != mb->motion.mv[list][first][1])
					return 0;
				if (mb->motion.ref_idx[list][quadrant] != mb->motion.ref_idx[list][first_quadrant])
					return 0;
			}
		}
	}

	/* One prediction covers the whole square. */
	return 1;
}

/*
 * Adds the residual of one 4x4 block to the samples already in the frame.
 *
 * coeffs are the block's 16 levels in scan order, or NULL for a block
 * without levels.  When dc_present, dc is the block's DC already scaled
 * by 8.5.10 or 8.5.11 and replaces the first coefficient; otherwise the
 * first level is scaled like the others.
 */
static void
recon_residual4x4(
	const int16_t *coeffs,
	int dc_present,
	int32_t dc,
	const int32_t *scale,
	int qp,
	uint8_t *dst,
	size_t stride)
{
	int32_t block[16];
	unsigned index;

	/* Places the levels in raster order (8.5.6), or starts from zero for a block that has only a DC. */
	if (coeffs != NULL) {
		/* Puts the level at each scan position at the raster position of that position. */
		for (index = 0; index < 16U; index++)
			block[h264sw_zigzag4x4[index]] = coeffs[index];
	} else {
		/* Clears every coefficient. */
		for (index = 0; index < 16U; index++)
			block[index] = 0;
	}

	/* The DC of an Intra_16x16 or chroma block comes from its own transform (8.5.10, 8.5.11). */
	if (dc_present != 0)
		block[0] = dc;

	/* Scales the levels, transforms them and adds the residual to the prediction (8.5.12, 8.5.14). */
	h264sw_scale4x4(block, scale, qp, dc_present);
	h264sw_idct4x4(block);
	h264sw_add_residual(dst, stride, block, 4U, dst, stride);
}

/* Adds the residual of one 8x8 block of 64 levels in scan order to the samples already in the frame (8.5.7, 8.5.13, 8.5.14). */
static void
recon_residual8x8(
	const int16_t *coeffs,
	const int32_t *scale,
	int qp,
	uint8_t *dst,
	size_t stride)
{
	int32_t block[64];
	unsigned index;

	/* Places the levels in raster order (8.5.7). */
	for (index = 0; index < 64U; index++)
		block[h264sw_zigzag8x8[index]] = coeffs[index];

	/* Scales the levels, transforms them and adds the residual to the prediction (8.5.13, 8.5.14). */
	h264sw_scale8x8(block, scale, qp);
	h264sw_idct8x8(block);
	h264sw_add_residual(dst, stride, block, 8U, dst, stride);
}

/* Writes the samples of an I_PCM macroblock from the record (8.3.5). */
static void
recon_samples_pcm(
	const struct h264sw_picture_ctx *picture,
	const struct h264sw_mb_job *job)
{
	const struct h264sw_frame *frame;
	uint8_t *dst;
	unsigned row;
	unsigned plane;
	unsigned size;
	unsigned offset;

	/* Takes the frame; the samples start at the record's first byte. */
	frame = picture->frame;
	offset = 0;

	/* Copies the 256 luma samples, then the 64 samples of Cb and of Cr, each in raster order. */
	for (plane = 0; plane < 3U; plane++) {
		/* The block's side in this plane. */
		size = 16U;
		if (plane != 0U)
			size = 8U;

		/* Copies the rows of the block to the macroblock's place. */
		dst = recon_sample_at(frame, plane, size * job->x, size * job->y);
		for (row = 0; row < size; row++)
			memcpy(dst + (size_t)row * frame->stride[plane], job->u.pcm + offset + (size_t)row * size, size);
		offset += size * size;
	}
}

/* Predicts and reconstructs the sixteen 4x4 blocks of an Intra_4x4 macroblock in luma4x4BlkIdx order (8.3.1.2, 8.5.1). */
static void
recon_samples_intra4x4(
	const struct h264sw_picture_ctx *picture,
	const struct h264sw_mb_job *job)
{
	const struct h264sw_frame *frame;
	struct h264sw_intra_edge edge;
	const int32_t *scale;
	uint8_t pred[16];
	uint8_t *dst;
	unsigned blk;
	unsigned x;
	unsigned y;
	unsigned parts;
	int qp;

	/* Takes the frame, QPY and the scaling of the Intra Y list. */
	frame = picture->frame;
	qp = job->qp;
	scale = picture->levelscale.scale4x4[RECON_SCALE4_INTRA_Y][qp % 6];

	/* Reconstructs each block before the next, because a later block reads the samples of an earlier one as its edge. */
	for (blk = 0; blk < 16U; blk++) {
		/* Gathers the block's edge from the frame and predicts the block; a mode that cannot be used gives 128, counted when the work was prepared. */
		x = 16U * job->x + h264sw_blk4x4_x[blk];
		y = 16U * job->y + h264sw_blk4x4_y[blk];
		parts = h264sw_intra_parts(job->neighbours, 4U, blk);
		h264sw_intra_edge_from(frame, 0U, x, y, 4U, parts, &edge);
		(void)h264sw_intra_pred4x4(job->intra_mode[blk], &edge, pred);

		/* Puts the prediction into the frame. */
		dst = recon_sample_at(frame, 0, x, y);
		recon_put_block(dst, frame->stride[0], pred, 4U);

		/* Adds the residual of a block that has levels. */
		if ((job->coded & ((uint32_t)1U << blk)) != 0U)
			recon_residual4x4(&job->u.luma[16U * blk], 0, 0, scale, qp, dst, frame->stride[0]);
	}
}

/* Predicts and reconstructs the four 8x8 blocks of an Intra_8x8 macroblock in luma8x8BlkIdx order (8.3.2.2, 8.5.3). */
static void
recon_samples_intra8x8(
	const struct h264sw_picture_ctx *picture,
	const struct h264sw_mb_job *job)
{
	const struct h264sw_frame *frame;
	struct h264sw_intra_edge edge;
	const int32_t *scale;
	uint8_t pred[64];
	uint8_t *dst;
	unsigned blk;
	unsigned x;
	unsigned y;
	unsigned parts;
	int qp;

	/* Takes the frame, QPY and the scaling of the Intra Y 8x8 list. */
	frame = picture->frame;
	qp = job->qp;
	scale = picture->levelscale.scale8x8[RECON_SCALE8_INTRA_Y][qp % 6];

	/* Reconstructs each block before the next, because a later block reads the samples of an earlier one as its edge. */
	for (blk = 0; blk < 4U; blk++) {
		/* Gathers the block's edge from the frame and predicts the block; a mode that cannot be used gives 128, counted when the work was prepared. */
		x = 16U * job->x + 8U * (blk % 2U);
		y = 16U * job->y + 8U * (blk / 2U);
		parts = h264sw_intra_parts(job->neighbours, 8U, blk);
		h264sw_intra_edge_from(frame, 0U, x, y, 8U, parts, &edge);
		(void)h264sw_intra_pred8x8(job->intra_mode[4U * blk], &edge, pred);

		/* Puts the prediction into the frame. */
		dst = recon_sample_at(frame, 0, x, y);
		recon_put_block(dst, frame->stride[0], pred, 8U);

		/* Adds the residual of a block that has levels. */
		if ((job->coded & ((uint32_t)1U << blk)) != 0U)
			recon_residual8x8(&job->u.luma[64U * blk], scale, qp, dst, frame->stride[0]);
	}
}

/* Predicts the whole luma block of an Intra_16x16 macroblock and adds its DC and AC residual (8.3.3, 8.5.2). */
static void
recon_samples_intra16x16(
	const struct h264sw_picture_ctx *picture,
	const struct h264sw_mb_job *job)
{
	const struct h264sw_frame *frame;
	struct h264sw_intra_edge edge;
	const int32_t *scale;
	const int16_t *ac;
	int32_t dc[16];
	uint8_t pred[256];
	uint8_t *origin;
	uint8_t *dst;
	unsigned blk;
	unsigned block_x;
	unsigned block_y;
	unsigned index;
	unsigned parts;
	int32_t block_dc;
	int qp;

	/* Gathers the macroblock's edge from the frame and predicts the whole block; a mode that cannot be used gives 128, counted when the work was prepared. */
	frame = picture->frame;
	parts = h264sw_intra_parts(job->neighbours, 16U, 0U);
	h264sw_intra_edge_from(frame, 0U, 16U * job->x, 16U * job->y, 16U, parts, &edge);
	(void)h264sw_intra_pred16x16(job->intra16x16_mode, &edge, pred);

	/* Puts the prediction into the frame. */
	origin = recon_sample_at(frame, 0, 16U * job->x, 16U * job->y);
	recon_put_block(origin, frame->stride[0], pred, 16U);

	/* A macroblock without a DC or an AC level has no residual. */
	if ((job->coded & (RECON_CODED_LUMA | ((uint32_t)1U << H264SW_JOB_LUMA_DC))) == 0U)
		return;

	/* Takes QPY and the scaling of the Intra Y list. */
	qp = job->qp;
	scale = picture->levelscale.scale4x4[RECON_SCALE4_INTRA_Y][qp % 6];

	/* Places the DC levels in a 4x4 matrix (8.5.6) and transforms and scales them (8.5.10); without levels the DC is zero. */
	for (index = 0; index < 16U; index++)
		dc[index] = 0;
	if ((job->coded & ((uint32_t)1U << H264SW_JOB_LUMA_DC)) != 0U) {
		/* Puts the level at each scan position at the raster position of that position. */
		for (index = 0; index < 16U; index++)
			dc[h264sw_zigzag4x4[index]] = job->luma_dc[index];
		h264sw_luma_dc(dc, scale[0], qp);
	}

	/* Adds each block's residual: the DC from the matrix at the block's row and column, and the AC levels the block carries (8.5.2). */
	for (blk = 0; blk < 16U; blk++) {
		block_x = h264sw_blk4x4_x[blk];
		block_y = h264sw_blk4x4_y[blk];
		block_dc = dc[4U * (block_y / 4U) + block_x / 4U];
		ac = NULL;
		if ((job->coded & ((uint32_t)1U << blk)) != 0U)
			ac = &job->u.luma[16U * blk];

		/* A block with neither AC levels nor a DC has a residual of zero. */
		if (ac == NULL) {
			if (block_dc == 0)
				continue;
		}
		dst = origin + (size_t)block_y * frame->stride[0] + block_x;
		recon_residual4x4(ac, 1, block_dc, scale, qp, dst, frame->stride[0]);
	}
}

/* Predicts the Cb and Cr blocks of an intra macroblock with intra_chroma_pred_mode (8.3.4) and adds their residual (8.5.4). */
static void
recon_samples_intra_chroma(
	const struct h264sw_picture_ctx *picture,
	const struct h264sw_mb_job *job)
{
	const struct h264sw_frame *frame;
	struct h264sw_intra_edge edge;
	uint8_t pred[64];
	uint8_t *dst;
	unsigned plane;
	unsigned parts;

	/* Takes the frame and the parts of the edge, which are those of the whole macroblock. */
	frame = picture->frame;
	parts = h264sw_intra_parts(job->neighbours, 16U, 0U);

	/* Predicts each chroma component from its own edge. */
	for (plane = 1; plane < 3U; plane++) {
		/* Gathers the component's edge and predicts the 8x8 block; a mode that cannot be used gives 128, counted when the work was prepared. */
		h264sw_intra_edge_from(frame, plane, 8U * job->x, 8U * job->y, 8U, parts, &edge);
		(void)h264sw_intra_pred_chroma(job->chroma_pred_mode, &edge, pred);

		/* Puts the prediction into the frame. */
		dst = recon_sample_at(frame, plane, 8U * job->x, 8U * job->y);
		recon_put_block(dst, frame->stride[plane], pred, 8U);
	}

	/* Adds the DC and AC residual of both components. */
	recon_samples_chroma_residual(picture, job, 1);
}

/* Adds the chroma residual of both components to the prediction already in the frame (8.5.4, 8.5.8, 8.5.11). */
static void
recon_samples_chroma_residual(
	const struct h264sw_picture_ctx *picture,
	const struct h264sw_mb_job *job,
	int intra)
{
	const struct h264sw_frame *frame;
	const int32_t *scale;
	const int16_t *ac;
	int32_t dc[4];
	uint8_t *origin;
	uint8_t *dst;
	size_t stride;
	unsigned component;
	unsigned blk;
	unsigned list;
	unsigned index;
	unsigned bit;
	int qp;

	/* Takes the frame. */
	frame = picture->frame;

	/* Each component has its own quantization parameter, scaling list and DC block. */
	for (component = 0; component < 2U; component++) {
		/* A component whose levels are all zero adds nothing to the prediction. */
		if ((job->coded & ((uint32_t)1U << (H264SW_JOB_CHROMA + component))) == 0U)
			continue;

		/* Finds QP'C (8.5.8) and the scaling of the component's list: Intra or Inter, Cb then Cr. */
		qp = recon_chroma_qp(picture, job->qp, component);
		list = RECON_SCALE4_INTER_CB + component;
		if (intra != 0)
			list = RECON_SCALE4_INTRA_CB + component;
		scale = picture->levelscale.scale4x4[list][qp % 6];

		/* Transforms and scales the four DC levels, a 2x2 matrix in raster order (8.5.11). */
		for (index = 0; index < 4U; index++)
			dc[index] = job->chroma_dc[component][index];
		h264sw_chroma_dc(dc, scale[0], qp);

		/* Adds each 4x4 block's residual: the DC of the same index and the AC levels the block carries (8.5.4). */
		stride = frame->stride[1U + component];
		origin = recon_sample_at(frame, 1U + component, 8U * job->x, 8U * job->y);

		/* Visits the four 4x4 blocks of the component. */
		for (blk = 0; blk < 4U; blk++) {
			ac = NULL;
			bit = H264SW_JOB_CHROMA_AC + 4U * component + blk;
			if ((job->coded & ((uint32_t)1U << bit)) != 0U)
				ac = job->chroma_ac[component][blk];

			/* A block with neither AC levels nor a DC has a residual of zero. */
			if (ac == NULL) {
				if (dc[blk] == 0)
					continue;
			}
			dst = origin + (size_t)(4U * (blk / 2U)) * stride + 4U * (blk % 2U);
			recon_residual4x4(ac, 1, dc[blk], scale, qp, dst, stride);
		}
	}
}

/* Adds the luma residual of an inter macroblock to its prediction, with 4x4 or 8x8 transform blocks (8.5.1, 8.5.3). */
static void
recon_samples_luma_residual(
	const struct h264sw_picture_ctx *picture,
	const struct h264sw_mb_job *job)
{
	const struct h264sw_frame *frame;
	const int32_t *scale;
	uint8_t *dst;
	unsigned blk;
	int qp;

	/* Takes the frame and QPY. */
	frame = picture->frame;
	qp = job->qp;

	/* An 8x8 transform has one block of 64 levels per quadrant. */
	if (job->transform_8x8 != 0) {
		scale = picture->levelscale.scale8x8[RECON_SCALE8_INTER_Y][qp % 6];

		/* Adds the residual of each quadrant that has levels. */
		for (blk = 0; blk < 4U; blk++) {
			if ((job->coded & ((uint32_t)1U << blk)) == 0U)
				continue;
			dst = recon_sample_at(frame, 0, 16U * job->x + 8U * (blk % 2U), 16U * job->y + 8U * (blk / 2U));
			recon_residual8x8(&job->u.luma[64U * blk], scale, qp, dst, frame->stride[0]);
		}

		/* Succeeded: the 8x8 blocks are added. */
		return;
	}

	/* A 4x4 transform has one block of 16 levels per 4x4 block. */
	scale = picture->levelscale.scale4x4[RECON_SCALE4_INTER_Y][qp % 6];

	/* Adds the residual of each block that has levels. */
	for (blk = 0; blk < 16U; blk++) {
		if ((job->coded & ((uint32_t)1U << blk)) == 0U)
			continue;
		dst = recon_sample_at(frame, 0, 16U * job->x + h264sw_blk4x4_x[blk], 16U * job->y + h264sw_blk4x4_y[blk]);
		recon_residual4x4(&job->u.luma[16U * blk], 0, 0, scale, qp, dst, frame->stride[0]);
	}
}

/* Reconstructs an inter macroblock: the prediction of each block the work was cut in, then the residual (8.4.2, 8.5). */
static void
recon_samples_inter(
	const struct h264sw_picture_ctx *picture,
	const struct h264sw_mb_job *job)
{
	const struct h264sw_mb *mb;
	unsigned quadrant;
	unsigned part;
	unsigned x;
	unsigned y;

	/* Takes the entry whose motion vectors the prediction reads. */
	mb = job->mb;

	/* A macroblock that moves as one is predicted as one 16x16 block. */
	if ((job->layout & H264SW_JOB_WHOLE) != 0U) {
		recon_samples_block(picture, job, mb, 0U, 0U, 16U);
	} else {
		/* Otherwise each quadrant that moves as one is predicted as one block, and the others block by block. */
		for (quadrant = 0; quadrant < 4U; quadrant++) {
			x = 8U * (quadrant % 2U);
			y = 8U * (quadrant / 2U);
			if ((job->layout & (1U << quadrant)) != 0U) {
				recon_samples_block(picture, job, mb, x, y, 8U);
				continue;
			}

			/* Predicts the quadrant block by block when its blocks move differently. */
			for (part = 0; part < 4U; part++)
				recon_samples_block(picture, job, mb, x + 4U * (part % 2U), y + 4U * (part / 2U), 4U);
		}
	}

	/* Adds the luma residual. */
	recon_samples_luma_residual(picture, job);

	/* Adds the chroma residual. */
	recon_samples_chroma_residual(picture, job, 0);
}

/*
 * Predicts one square block of an inter macroblock from its reference frames (8.4.2.1 to 8.4.2.3).
 *
 * (x, y) is the block's upper-left luma sample inside the macroblock
 * and size its width and height (16, 8 or 4).  The block's frames and
 * weight factors come from the quadrant it lies in and its vectors from
 * its first 4x4 block.  A block predicted from one list without weights
 * is interpolated straight into the frame (the default prediction of one
 * list is the list's samples, 8-271 and 8-272); otherwise each list's
 * samples are interpolated apart and weighted into the frame.
 */
static void
recon_samples_block(
	const struct h264sw_picture_ctx *picture,
	const struct h264sw_mb_job *job,
	const struct h264sw_mb *mb,
	unsigned x,
	unsigned y,
	unsigned size)
{
	const struct h264sw_frame *frame;
	const struct h264sw_frame *ref;
	const struct h264sw_weight_factors *factors;
	uint8_t luma[2][256];
	uint8_t chroma[2][2][64];
	const uint8_t *prediction[2];
	uint8_t *dst[3];
	uint8_t *out;
	size_t out_stride;
	unsigned quadrant;
	unsigned blk;
	unsigned list;
	unsigned component;
	unsigned chroma_size;
	int direct[3];
	int single;
	int mv_x;
	int mv_y;
	int whole_x;
	int whole_y;
	int plane_x;
	int plane_y;

	/* Takes the frame, the block's quadrant and first 4x4 block, and the chroma block size. */
	frame = picture->frame;
	quadrant = 2U * (y / 8U) + x / 8U;
	blk = h264sw_blk4x4_at(x, y);
	chroma_size = size / 2U;

	/* Finds where the block's samples go in each plane. */
	dst[0] = recon_sample_at(frame, 0, 16U * job->x + x, 16U * job->y + y);
	dst[1] = recon_sample_at(frame, 1, 8U * job->x + x / 2U, 8U * job->y + y / 2U);
	dst[2] = recon_sample_at(frame, 2, 8U * job->x + x / 2U, 8U * job->y + y / 2U);

	/* Finds out whether the block is predicted from one list only. */
	single = 0;
	if (job->ref[quadrant][0] == NULL)
		single = 1;
	if (job->ref[quadrant][1] == NULL)
		single = 1;

	/* A block predicted from one list goes straight into the frame in each plane whose factors are the default ones. */
	for (component = 0; component < 3U; component++) {
		direct[component] = 0;
		if (single == 0)
			continue;
		if (job->weights[quadrant][component].weighted == 0)
			direct[component] = 1;
	}

	/* Predicts the block from each list it uses: luma at quarter samples, Cb and Cr at eighth samples. */
	prediction[0] = NULL;
	prediction[1] = NULL;
	for (list = 0; list < 2U; list++) {
		ref = job->ref[quadrant][list];
		if (ref == NULL)
			continue;

		/* Splits the luma vector into its whole-sample part and its quarter-sample fraction, rounding down (8-223 to 8-226). */
		mv_x = mb->motion.mv[list][blk][0];
		mv_y = mb->motion.mv[list][blk][1];
		whole_x = recon_floor_div(mv_x, 4);
		whole_y = recon_floor_div(mv_y, 4);
		plane_x = (int)(16U * job->x + x) + whole_x;
		plane_y = (int)(16U * job->y + y) + whole_y;

		/* Interpolates the luma into the frame or into the list's buffer. */
		out = luma[list];
		out_stride = size;
		if (direct[0] != 0) {
			out = dst[0];
			out_stride = frame->stride[0];
		}
		h264sw_interp_luma(ref->plane[0], ref->stride[0], ref->width, ref->height, plane_x, plane_y, (unsigned)(mv_x - 4 * whole_x), (unsigned)(mv_y - 4 * whole_y), size, size, out, out_stride);
		prediction[list] = luma[list];

		/* The chroma vector of a frame is the luma vector in eighth chroma samples (8.4.1.4); splits it likewise (8-227 to 8-230). */
		whole_x = recon_floor_div(mv_x, 8);
		whole_y = recon_floor_div(mv_y, 8);
		plane_x = (int)(8U * job->x + x / 2U) + whole_x;
		plane_y = (int)(8U * job->y + y / 2U) + whole_y;

		/* Interpolates Cb, then Cr. */
		for (component = 0; component < 2U; component++) {
			/* Interpolates the component into the frame or into the list's buffer. */
			out = chroma[list][component];
			out_stride = chroma_size;
			if (direct[1U + component] != 0) {
				out = dst[1U + component];
				out_stride = frame->stride[1U + component];
			}
			h264sw_interp_chroma(ref->plane[1U + component], ref->stride[1U + component], ref->width / 2U, ref->height / 2U, plane_x, plane_y, (unsigned)(mv_x - 8 * whole_x), (unsigned)(mv_y - 8 * whole_y), chroma_size, chroma_size, out, out_stride);
		}
	}

	/* Weights the luma predictions into the frame (8.4.2.3), unless they went there already. */
	if (direct[0] == 0) {
		factors = &job->weights[quadrant][0];
		h264sw_weights_apply(factors, prediction[0], prediction[1], size, size, size, dst[0], frame->stride[0]);
	}

	/* Weights the Cb and Cr predictions likewise with the chroma factors. */
	for (component = 0; component < 2U; component++) {
		if (direct[1U + component] != 0)
			continue;
		factors = &job->weights[quadrant][1U + component];
		prediction[0] = NULL;
		prediction[1] = NULL;
		if (job->ref[quadrant][0] != NULL)
			prediction[0] = chroma[0][component];
		if (job->ref[quadrant][1] != NULL)
			prediction[1] = chroma[1][component];
		h264sw_weights_apply(factors, prediction[0], prediction[1], chroma_size, chroma_size, chroma_size, dst[1U + component], frame->stride[1U + component]);
	}
}

/*
 * Deblocks the rows of a picture that are ready, in order.
 *
 * Row r is ready when it is the first row not deblocked, and its
 * macroblocks and those of row r + 1 are all in place: the intra
 * prediction of row r + 1 reads row r before the filter changes it.  The
 * last row waits for the end of the picture.
 */
static void
recon_deblock_ready(
	struct h264sw_picture_ctx *picture)
{
	unsigned row;

	/* Moves down the rows while each is ready. */
	while (picture->deblock_row + 1U < picture->height_mbs) {
		/* Stops at a row, or a row below it, that still lacks a macroblock. */
		row = picture->deblock_row;
		if (picture->rows_done[row] < picture->width_mbs)
			break;
		if (picture->rows_done[row + 1U] < picture->width_mbs)
			break;

		/* Filters the row when the picture is filtered. */
		if (picture->filter != 0)
			h264sw_deblock_rows(picture, row, row + 1U);
		picture->deblock_row = row + 1U;
	}
}

/* Empties the counts of a picture's stages: no work waits, no row has a macroblock in place or is deblocked. */
static void
recon_reset_counts(
	struct h264sw_picture_ctx *picture)
{
	unsigned row;

	/* No work waits. */
	picture->job_head = 0;
	picture->job_count = 0;

	/* No row is deblocked. */
	picture->deblock_row = 0;

	/* A picture without the row counts has nothing more to empty. */
	if (picture->rows_done == NULL)
		return;

	/* No row has a macroblock in place. */
	for (row = 0; row < picture->height_mbs; row++)
		picture->rows_done[row] = 0;
}

/*
 * Prepares the current macroblock into the ring and hands the work over in groups.
 *
 * The place of the ring is reused only once the entry that had it has
 * its samples in place; the parsing thread works beside the threads
 * until then.
 */
static int
recon_queue_threads(
	struct h264sw_slice_ctx *ctx,
	struct h264sw_recon_threads *threads)
{
	struct h264sw_picture_ctx *picture;
	struct h264sw_mb_job *job;
	int error;

	/* Makes room when the ring is full of entries that may not have their samples yet. */
	picture = ctx->picture;
	if (threads->written - threads->free_seen >= picture->job_capacity)
		recon_help(threads, picture, 0);

	/* Prepares the macroblock into the next place; an error leaves the ring as it was. */
	job = &picture->jobs[threads->written % picture->job_capacity];
	error = h264sw_recon_prepare(ctx, job);
	if (error != 0)
		return error;
	threads->written++;

	/* Hands over the work prepared once a group is ready, or a row ends. */
	if (threads->written - threads->handed >= RECON_HAND_OVER ||
	    job->x + 1U == picture->width_mbs) {
		media_os_mutex_lock(&threads->lock);

		recon_hand_over(threads, picture);

		media_os_mutex_unlock(&threads->lock);
	}

	/* Succeeded: the macroblock is in the ring. */
	return 0;
}

/*
 * Hands the entries prepared over to the threads (the mutex held).
 *
 * The first hand-over of a picture also makes the copy of the picture
 * the deblocking uses; no row of it is being deblocked before then.
 */
static void
recon_hand_over(
	struct h264sw_recon_threads *threads,
	struct h264sw_picture_ctx *picture)
{
	/* Copies the picture for the deblocking once. */
	if (threads->view_ready == 0) {
		threads->deblock_view = *picture;
		threads->view_ready = 1;
	}

	/*
	 * Publishes the entries prepared and the slices they belong to (each
	 * entry's slice is recorded before its macroblocks are parsed), and
	 * takes the room the threads have freed.
	 */
	threads->published = threads->written;
	threads->handed = threads->written;
	threads->slice_count = picture->slice_count;
	threads->free_seen = threads->retired;

	/* Wakes a worker for the work. */
	recon_wake(threads);
}

/*
 * Works beside the threads from the parsing thread.
 *
 * Everything prepared is handed over first.  With to_end 0 it returns
 * as soon as the ring has room for one more entry; with to_end 1, once
 * every entry has its samples in place and no row is being deblocked,
 * and then the threads let the picture go.
 */
static void
recon_help(
	struct h264sw_recon_threads *threads,
	struct h264sw_picture_ctx *picture,
	int to_end)
{
	struct recon_task task;
	int took;

	/* Works beside the threads under the mutex, which the work itself is done outside of. */
	media_os_mutex_lock(&threads->lock);

	/* Hands over everything prepared first. */
	recon_hand_over(threads, picture);

	/* Takes work until the ring has room or the picture's work is over. */
	for (;;) {
		/* Stops once the oldest entries have freed a place, when only room is wanted. */
		if (to_end == 0 &&
		    threads->written - threads->retired < picture->job_capacity)
			break;

		/* Does a piece of work outside the mutex, waking a worker for any more. */
		took = recon_take(threads, &task);
		if (took != 0) {
			recon_wake(threads);
			media_os_mutex_unlock(&threads->lock);
			recon_run(threads, &task);
			media_os_mutex_lock(&threads->lock);
			recon_finish(threads, &task);
			continue;
		}

		/* Stops once nothing is taken, nothing waits and no row is due: the picture's work is over. */
		if (to_end != 0 &&
		    threads->running == 0U &&
		    threads->retired == threads->published)
			break;

		/* Waits for a thread to finish what it took. */
		threads->waiting = 1;
		media_os_cond_wait(&threads->progress, &threads->lock);
		threads->waiting = 0;
	}

	/* Keeps the room seen. */
	threads->free_seen = threads->retired;

	/* Lets an ended picture go, so that its concealment and its last rows are the caller's alone. */
	if (to_end != 0)
		threads->picture = NULL;

	media_os_mutex_unlock(&threads->lock);
}

/*
 * Takes a piece of the picture's work (the mutex held).
 *
 * A row due for its deblocking comes first, while its samples are still
 * in the caches; a picture left unfiltered only counts its rows.  Then
 * the oldest entry handed over and not taken whose intra neighbours have
 * their samples in place, with the entries after it whose neighbours are
 * in place or in the group, up to RECON_TAKE_MAX; the search goes no
 * further than RECON_LOOK_ROWS rows past the oldest entry not taken.
 * Returns 1 with the task, or 0 when nothing can be taken now.
 */
static int
recon_take(
	struct h264sw_recon_threads *threads,
	struct recon_task *task)
{
	struct h264sw_picture_ctx *picture;
	const struct h264sw_mb_job *job;
	unsigned capacity;
	unsigned look_end;
	unsigned entry;
	unsigned first;
	unsigned count;
	int due;
	int ready;

	/* Nothing to take between pictures or before the first hand-over. */
	task->kind = RECON_TASK_NONE;
	picture = threads->picture;
	if (picture == NULL)
		return 0;
	if (threads->published == 0U)
		return 0;

	/* Takes the next row due for its deblocking; an unfiltered picture's rows are only counted. */
	for (;;) {
		/* Stops at a row not due yet. */
		due = recon_deblock_due(threads);
		if (due == 0)
			break;

		/* Counts a row of a picture left unfiltered as deblocked. */
		if (picture->filter == 0) {
			picture->deblock_row++;
			continue;
		}

		/*
		 * The row is this thread's: deblocking keeps every other row
		 * waiting (the rows are deblocked one at a time, in order), and
		 * running keeps the picture from being let go under it.
		 */
		threads->deblocking = 1;
		threads->running++;
		threads->deblock_view.slice_count = threads->slice_count;
		task->kind = RECON_TASK_DEBLOCK;
		task->picture = picture;
		task->row = picture->deblock_row;

		/* Succeeded: a row to deblock. */
		return 1;
	}

	/* Finds the oldest entry not taken that its neighbours allow, within the rows looked through. */
	capacity = picture->job_capacity;
	look_end = threads->next_take + RECON_LOOK_ROWS * picture->width_mbs + RECON_TAKE_MAX;
	if (look_end > threads->published)
		look_end = threads->published;
	first = look_end;
	for (entry = threads->next_take;
	     entry < look_end;
	     entry++) {
		/* Skips an entry another thread took. */
		if (threads->state[entry % capacity] != RECON_JOB_WAITING)
			continue;

		/* Stops at the first entry whose neighbours have their samples. */
		job = &picture->jobs[entry % capacity];
		ready = recon_job_ready(threads, job, entry, 0U);
		if (ready != 0) {
			first = entry;
			break;
		}
	}

	/* Nothing can be taken now. */
	if (first == look_end)
		return 0;

	/* Takes the entries that follow it too, while each waits and its neighbours are in place or in the group. */
	count = 1U;
	while (count < RECON_TAKE_MAX && first + count < threads->published) {
		/* Stops at an entry another thread took. */
		entry = first + count;
		if (threads->state[entry % capacity] != RECON_JOB_WAITING)
			break;

		/* Stops at an entry whose neighbours are not in place yet. */
		job = &picture->jobs[entry % capacity];
		ready = recon_job_ready(threads, job, first, count);
		if (ready == 0)
			break;
		count++;
	}

	/* Marks the group taken. */
	for (entry = first;
	     entry < first + count;
	     entry++)
		threads->state[entry % capacity] = RECON_JOB_TAKEN;

	/* Moves the oldest entry not taken past the entries taken. */
	while (threads->next_take < threads->published &&
	       threads->state[threads->next_take % capacity] != RECON_JOB_WAITING)
		threads->next_take++;

	/* The group is this thread's; running keeps the picture from being let go under it. */
	threads->running++;
	task->kind = RECON_TASK_SAMPLES;
	task->picture = picture;
	task->first = first;
	task->count = count;

	/* Succeeded: a group of macroblocks to reconstruct. */
	return 1;
}

/*
 * Tells whether the next row is due for its deblocking (the mutex held).
 *
 * It is when no row is being deblocked, it is not the last row (which
 * waits for the end of the picture), and it and the row below it have all
 * their macroblocks in place.
 */
static int
recon_deblock_due(
	const struct h264sw_recon_threads *threads)
{
	const struct h264sw_picture_ctx *picture;
	unsigned row;

	/* No row is due without a picture, before its work is handed over, or while a row is being deblocked. */
	picture = threads->picture;
	if (picture == NULL)
		return 0;
	if (threads->published == 0U)
		return 0;
	if (threads->deblocking != 0)
		return 0;

	/* The last row waits for the end of the picture. */
	row = picture->deblock_row;
	if (row + 1U >= picture->height_mbs)
		return 0;

	/* The row and the row below it must have all their macroblocks in place. */
	if (picture->rows_done[row] < picture->width_mbs)
		return 0;
	if (picture->rows_done[row + 1U] < picture->width_mbs)
		return 0;

	/* Succeeded: the row is due. */
	return 1;
}

/*
 * Tells whether an entry's samples can be reconstructed now (the mutex held).
 *
 * Each neighbour its intra prediction reads (the H264SW_JOB_* bits)
 * must have its samples in place, or be one of the count entries from
 * first, which the same thread reconstructs before it.  An inter or
 * I_PCM macroblock reads no neighbour.
 */
static int
recon_job_ready(
	const struct h264sw_recon_threads *threads,
	const struct h264sw_mb_job *job,
	unsigned first,
	unsigned count)
{
	const struct h264sw_picture_ctx *picture;
	unsigned addr[RECON_NEIGHBOURS];
	unsigned bit[RECON_NEIGHBOURS];
	unsigned width;
	unsigned index;
	unsigned entry;
	int found;

	/* A macroblock that reads no neighbour is ready. */
	if (job->neighbours == 0U)
		return 1;

	/* The neighbours' addresses: A left, B above, C above right, D above left (only those available are marked). */
	picture = threads->picture;
	width = picture->width_mbs;
	bit[0] = H264SW_JOB_A;
	addr[0] = job->addr - 1U;
	bit[1] = H264SW_JOB_B;
	addr[1] = job->addr - width;
	bit[2] = H264SW_JOB_C;
	addr[2] = job->addr - width + 1U;
	bit[3] = H264SW_JOB_D;
	addr[3] = job->addr - width - 1U;

	/* Looks at each neighbour marked. */
	for (index = 0U;
	     index < RECON_NEIGHBOURS;
	     index++) {
		/* Skips a neighbour the prediction does not read, one outside the picture, and one in place. */
		if ((job->neighbours & bit[index]) == 0U)
			continue;
		if (addr[index] >= threads->mb_done_capacity)
			continue;
		if (threads->mb_done[addr[index]] != 0U)
			continue;

		/* Looks for the neighbour in the group. */
		found = 0;
		for (entry = first;
		     entry < first + count;
		     entry++) {
			/* Stops at the neighbour's entry. */
			if (picture->jobs[entry % picture->job_capacity].addr == addr[index]) {
				found = 1;
				break;
			}
		}

		/* A neighbour neither in place nor in the group keeps the entry waiting. */
		if (found == 0)
			return 0;
	}

	/* Succeeded: every neighbour read is in place or comes first. */
	return 1;
}

/*
 * Does a piece of work outside the mutex.
 *
 * The samples of each macroblock of a group in order, or the
 * deblocking of a row with the copy of the picture.
 */
static void
recon_run(
	struct h264sw_recon_threads *threads,
	const struct recon_task *task)
{
	const struct h264sw_picture_ctx *picture;
	unsigned entry;

	/* Deblocks the row with the copy of the picture. */
	if (task->kind == RECON_TASK_DEBLOCK) {
		h264sw_deblock_rows(&threads->deblock_view, task->row, task->row + 1U);
		return;
	}

	/* Reconstructs the samples of each macroblock of the group, in decoding order. */
	picture = task->picture;
	for (entry = task->first;
	     entry < task->first + task->count;
	     entry++)
		h264sw_recon_samples(picture, &picture->jobs[entry % picture->job_capacity]);
}

/*
 * Records a piece of work done (the mutex held) and wakes who waits for it.
 *
 * A row deblocked lets the next one be due.  Each macroblock of a group
 * is marked in place and counted in its row; the oldest entries with
 * their samples in place free their places of the ring.
 */
static void
recon_finish(
	struct h264sw_recon_threads *threads,
	const struct recon_task *task)
{
	struct h264sw_picture_ctx *picture;
	const struct h264sw_mb_job *job;
	unsigned capacity;
	unsigned entry;

	/* Records the work done by its kind. */
	picture = task->picture;
	capacity = picture->job_capacity;
	if (task->kind == RECON_TASK_DEBLOCK) {
		/* The row is deblocked, and another thread may deblock the next one. */
		picture->deblock_row = task->row + 1U;
		threads->deblocking = 0;
	} else {
		/* Marks each macroblock of the group in place, in its row too. */
		for (entry = task->first;
		     entry < task->first + task->count;
		     entry++) {
			/* The entry, its macroblock and its row. */
			job = &picture->jobs[entry % capacity];
			threads->state[entry % capacity] = RECON_JOB_DONE;
			threads->mb_done[job->addr] = 1U;
			picture->rows_done[job->y]++;
		}

		/* Frees the places of the oldest entries that have their samples in place. */
		while (threads->retired < threads->published &&
		       threads->state[threads->retired % capacity] == RECON_JOB_DONE) {
			threads->state[threads->retired % capacity] = RECON_JOB_WAITING;
			threads->retired++;
		}
	}

	/* The thread holds no work of the picture any more. */
	threads->running--;

	/* Tells the parsing thread something moved, when it waits for that. */
	if (threads->waiting != 0)
		media_os_cond_signal(&threads->progress);

	/* Wakes a worker for work the progress may have allowed. */
	recon_wake(threads);
}

/*
 * Wakes one waiting worker when there may be work for it (the mutex held).
 *
 * Work may be an entry not taken (whose neighbours may still keep it
 * waiting: the worker then waits again) or a row due.  Each worker that
 * takes work wakes the next in turn, so a group handed over wakes as many
 * workers as it keeps busy.
 */
static void
recon_wake(
	struct h264sw_recon_threads *threads)
{
	int due;

	/* No worker waits. */
	if (threads->idle == 0U)
		return;

	/* Wakes one for an entry not taken. */
	if (threads->picture != NULL &&
	    threads->next_take < threads->published) {
		media_os_cond_signal(&threads->work);
		return;
	}

	/* Wakes one for a row due. */
	due = recon_deblock_due(threads);
	if (due != 0)
		media_os_cond_signal(&threads->work);
}

/*
 * Runs one worker: takes work while there is any, waits for work otherwise, until it is told to end.
 */
static void
recon_worker_main(
	void *argument)
{
	struct recon_worker *worker;
	struct h264sw_recon_threads *threads;
	struct recon_task task;
	int took;

	/* The threads the worker serves. */
	worker = argument;
	threads = worker->owner;

	/* Takes and does work until told to end. */
	media_os_mutex_lock(&threads->lock);

	while (threads->quit == 0) {
		/* Does a piece of work outside the mutex, waking another worker for any more. */
		took = recon_take(threads, &task);
		if (took != 0) {
			recon_wake(threads);
			media_os_mutex_unlock(&threads->lock);
			recon_run(threads, &task);
			media_os_mutex_lock(&threads->lock);
			recon_finish(threads, &task);
			continue;
		}

		/* Waits for work, counted as idle so that it is woken for it. */
		threads->idle++;
		media_os_cond_wait(&threads->work, &threads->lock);
		threads->idle--;
	}

	media_os_mutex_unlock(&threads->lock);
}
