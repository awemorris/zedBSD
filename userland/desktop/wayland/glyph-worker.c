/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The font thread (the glyph worker of ws213-p005, the work thread W of
 * plan/ws213/phase001 section 5).
 *
 * The main thread decides a glyph's cell and metrics on its own fonts and
 * asks the worker for the coverage; the worker renders the glyphs in the
 * order they were asked for, on faces of its own opened over the same font
 * bytes (a face is not shared between threads: its size is state), and
 * hands them back in that order.  It also decodes a colour glyph (the
 * emoji font's PNG, scaled to the size) and reads a font file whole, so
 * that the event loop never waits for either.  The worker touches no
 * Vulkan, no host image and nothing of the compositor's but this record:
 * the main thread writes the coverage into the atlas at the start of a
 * walk, and opens its own face over a font's bytes.
 */

#include "glyph-worker.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void *worker_main(void *argument);
static void worker_render(struct kwl_glyph_worker *worker, struct kwl_glyph_job *job);
static void worker_color(struct kwl_glyph_worker *worker, struct kwl_glyph_job *job);
static void worker_read(struct kwl_glyph_job *job);
static struct truetype_face *worker_face(struct kwl_glyph_worker *worker, unsigned slot);
static void worker_wake(int descriptor);
static void worker_free_list(struct kwl_glyph_job *job);

/*
 * Starts the worker; wake_main is the write end of the main thread's wake
 * pipe, written once for the glyphs rendered since the main thread last
 * took them.  Returns 0, or the errno value of making its lock, its signal
 * or its thread (the worker is then not running and needs no stop).
 */
int
kwl_glyph_worker_start(
	struct kwl_glyph_worker *worker,
	int wake_main)
{
	int error;

	/* An empty record. */
	memset(worker, 0, sizeof(*worker));
	worker->wake_main = wake_main;

	/* The lock of the lists and the fonts. */
	error = pthread_mutex_init(&worker->lock, NULL);
	if (error != 0)
		return error;

	/* The signal of a job asked for or of the stop. */
	error = pthread_cond_init(&worker->work, NULL);
	if (error != 0) {
		pthread_mutex_destroy(&worker->lock);
		return error;
	}

	/* The thread, which waits for the first job, in the register as the font thread. */
	error = kwl_thread_start(&worker->thread, "font", worker_main, worker);
	if (error != 0) {
		pthread_cond_destroy(&worker->work);
		pthread_mutex_destroy(&worker->lock);
		return error;
	}

	/* Succeeded: the worker renders what it is asked for. */
	return 0;
}

/*
 * Stops the worker and waits for it: the glyph it is rendering is finished
 * and every job not taken is freed.  Its faces close; the font bytes they
 * were opened over are the main thread's to free after this returns.
 */
void
kwl_glyph_worker_stop(
	struct kwl_glyph_worker *worker)
{
	unsigned slot;

	/* A worker never started has nothing to stop. */
	if (!worker->thread.started)
		return;

	/* The request, seen when the worker next looks for a job. */
	pthread_mutex_lock(&worker->lock);

	worker->stop = 1U;
	pthread_cond_signal(&worker->work);

	pthread_mutex_unlock(&worker->lock);

	/* The thread ends; nothing touches the record but this thread from here. */
	kwl_thread_join(&worker->thread);

	/* The jobs not rendered and the jobs not taken. */
	worker_free_list(worker->jobs);
	worker_free_list(worker->done);
	worker->jobs = NULL;
	worker->jobs_tail = NULL;
	worker->done = NULL;
	worker->done_tail = NULL;

	/* The worker's own faces. */
	for (slot = 0U; slot < KWL_GLYPH_FONTS; slot++) {
		if (worker->faces[slot] != NULL)
			truetype_close(worker->faces[slot]);
		worker->faces[slot] = NULL;
	}

	/* The lock and the signal. */
	pthread_cond_destroy(&worker->work);
	pthread_mutex_destroy(&worker->lock);
}

/*
 * Gives the worker a font the main thread opened, at its slot (the main
 * thread's face index), before any job names it.  The worker opens its own
 * face over the same bytes when a job first needs it.
 */
void
kwl_glyph_worker_font(
	struct kwl_glyph_worker *worker,
	unsigned slot,
	const struct kwl_glyph_font *font)
{
	/* A slot past the fonts is not kept. */
	if (slot >= KWL_GLYPH_FONTS)
		return;

	/* The font, read by the worker under the same lock. */
	pthread_mutex_lock(&worker->lock);

	worker->fonts[slot] = *font;

	pthread_mutex_unlock(&worker->lock);
}

/* Asks the worker for a glyph, after every glyph asked for before it; the job is the worker's until it is taken. */
void
kwl_glyph_worker_submit(
	struct kwl_glyph_worker *worker,
	struct kwl_glyph_job *job)
{
	/* The job joins the end of the list, and the worker wakes for it. */
	job->next = NULL;
	pthread_mutex_lock(&worker->lock);

	if (worker->jobs_tail == NULL) {
		worker->jobs = job;
	} else {
		worker->jobs_tail->next = job;
	}

	/* The new job is the last. */
	worker->jobs_tail = job;
	pthread_cond_signal(&worker->work);

	pthread_mutex_unlock(&worker->lock);
}

/*
 * Takes every glyph rendered so far, in the order they were asked for, as
 * a list through next (NULL: none); the caller frees each with
 * kwl_glyph_job_free.  The worker wakes the main thread again for the next
 * one rendered.
 */
struct kwl_glyph_job *
kwl_glyph_worker_take(
	struct kwl_glyph_worker *worker)
{
	struct kwl_glyph_job *taken;

	/*
	 * The list, emptied; main_woken cleared means the next glyph rendered
	 * writes the wake pipe again.
	 */
	pthread_mutex_lock(&worker->lock);

	taken = worker->done;
	worker->done = NULL;
	worker->done_tail = NULL;
	worker->main_woken = 0U;

	pthread_mutex_unlock(&worker->lock);

	/* The glyphs rendered, oldest first. */
	return taken;
}

/* Tells whether glyphs are rendered and waiting to be taken (1) or not (0). */
int
kwl_glyph_worker_ready(
	struct kwl_glyph_worker *worker)
{
	int ready;

	/* Whether the list of rendered glyphs has any. */
	ready = 0;
	pthread_mutex_lock(&worker->lock);

	if (worker->done != NULL)
		ready = 1;

	pthread_mutex_unlock(&worker->lock);

	/* The answer. */
	return ready;
}

/*
 * Frees a job and its coverage.
 */
void
kwl_glyph_job_free(
	struct kwl_glyph_job *job)
{
	/* No job. */
	if (job == NULL)
		return;

	/* The coverage, a colour image and a file's bytes not taken, then the job. */
	free(job->bitmap);
	free(job->image.pixels);
	free(job->data);
	free(job);
}

/* Runs the worker: renders each job in its order until asked to stop. */
static void *
worker_main(
	void *argument)
{
	struct kwl_glyph_worker *worker;
	struct kwl_glyph_job *job;
	unsigned wake;

	/* The worker's record. */
	worker = argument;

	/* Each job in the order asked for, until the stop. */
	for (;;) {
		/* The next job, or the stop. */
		pthread_mutex_lock(&worker->lock);

		while (worker->jobs == NULL && !worker->stop)
			pthread_cond_wait(&worker->work, &worker->lock);
		job = NULL;
		if (!worker->stop) {
			job = worker->jobs;
			worker->jobs = job->next;
			if (worker->jobs == NULL)
				worker->jobs_tail = NULL;
			job->next = NULL;
		}

		pthread_mutex_unlock(&worker->lock);

		/* The stop ends the thread; the jobs left are the stopping thread's to free. */
		if (job == NULL)
			break;

		/* The job's work, away from every lock. */
		if (job->task == KWL_GLYPH_TASK_READ) {
			worker_read(job);
		} else if (job->task == KWL_GLYPH_TASK_COLOR) {
			worker_color(worker, job);
		} else {
			worker_render(worker, job);
		}

		/*
		 * The job joins the rendered ones in its order; the main thread is
		 * woken once for the rendered glyphs it has not taken yet.
		 */
		pthread_mutex_lock(&worker->lock);

		if (worker->done_tail == NULL) {
			worker->done = job;
		} else {
			worker->done_tail->next = job;
		}

		/* The job is the last rendered, and wakes the main thread unless it was woken already. */
		worker->done_tail = job;
		wake = 0U;
		if (!worker->main_woken) {
			worker->main_woken = 1U;
			wake = 1U;
		}

		pthread_mutex_unlock(&worker->lock);

		/* The main thread's poll returns. */
		if (wake)
			worker_wake(worker->wake_main);
	}

	/* The thread ends at the stop. */
	return NULL;
}

/*
 * Renders one job's glyph on the worker's face of its font: the box must be
 * the one the main thread measured, so that the cell it chose holds it.
 * The job's error says why it could not be.
 */
static void
worker_render(
	struct kwl_glyph_worker *worker,
	struct kwl_glyph_job *job)
{
	struct truetype_face *face;
	struct truetype_glyph metrics;
	uint8_t *bitmap;
	size_t bytes;
	int error;

	/* The worker's own face of the font. */
	job->error = 0;
	job->bitmap = NULL;
	face = worker_face(worker, job->font);
	if (face == NULL) {
		job->error = ENOENT;
		return;
	}

	/* The glyph's box at the size. */
	error = truetype_set_pixel_size(face, job->pixels);
	if (error != 0) {
		job->error = error;
		return;
	}

	/* The box at that size. */
	error = truetype_glyph_metrics(face, job->glyph, &metrics);
	if (error != 0) {
		job->error = error;
		return;
	}

	/* A box other than the main thread's is not drawn: the cell was chosen for that one. */
	if (metrics.width != job->width || metrics.height != job->height) {
		job->error = EINVAL;
		return;
	}

	/* An empty box has nothing to draw. */
	if (metrics.width == 0U || metrics.height == 0U) {
		job->error = EINVAL;
		return;
	}

	/* The coverage's bytes, one a pixel. */
	bytes = (size_t)metrics.width * (size_t)metrics.height;
	bitmap = calloc(1, bytes);
	if (bitmap == NULL) {
		job->error = ENOMEM;
		return;
	}

	/* The glyph's coverage. */
	error = truetype_render_glyph(face, job->glyph, &metrics, bitmap, metrics.width, bytes);
	if (error != 0) {
		free(bitmap);
		job->error = error;
		return;
	}

	/* Succeeded: the coverage and the box drawn are the job's. */
	job->metrics = metrics;
	job->bitmap = bitmap;
}

/*
 * Decodes one job's colour glyph on the worker's face of the emoji font
 * and scales it to the size: the image must be of the box the main thread
 * measured, so that the cell it chose holds it.  The job's error says why
 * it could not be.
 */
static void
worker_color(
	struct kwl_glyph_worker *worker,
	struct kwl_glyph_job *job)
{
	struct truetype_face *face;
	int error;

	/* The worker's own face of the font. */
	job->error = 0;
	memset(&job->image, 0, sizeof(job->image));
	face = worker_face(worker, job->font);
	if (face == NULL) {
		job->error = ENOENT;
		return;
	}

	/* The glyph's PNG decoded and scaled. */
	error = kl_color_glyph(face, job->glyph, job->pixels, &job->image);
	if (error != 0) {
		job->error = error;
		return;
	}

	/* An image of another box than the main thread's is not drawn: the cell was chosen for that one. */
	if ((uint32_t)job->image.width != job->width || (uint32_t)job->image.height != job->height) {
		free(job->image.pixels);
		memset(&job->image, 0, sizeof(job->image));
		job->error = EINVAL;
		return;
	}

	/* Succeeded: the pixels are the job's. */
	return;
}

/*
 * Reads one job's font file whole (an ordinary file, to KWL_GLYPH_FILE_MAX
 * bytes) into an allocation of the job's.  The job's error says why it
 * could not be.
 */
static void
worker_read(
	struct kwl_glyph_job *job)
{
	struct stat status;
	unsigned char *data;
	ssize_t count;
	size_t capacity;
	size_t length;
	int descriptor;
	int regular;
	int error;

	/* The file; opening a FIFO or a device does not wait for a writer. */
	job->error = 0;
	job->data = NULL;
	job->size = 0U;
	if (job->path == NULL) {
		job->error = ENOENT;
		return;
	}

	/* Opens the file. */
	descriptor = open(job->path, O_RDONLY | O_CLOEXEC | O_NONBLOCK);
	if (descriptor < 0) {
		job->error = errno;
		return;
	}

	/* Only an ordinary file, of a size bounded by KWL_GLYPH_FILE_MAX. */
	error = fstat(descriptor, &status);
	if (error != 0) {
		job->error = errno;
		close(descriptor);
		return;
	}

	/* Refuses what is not a regular file with bytes in it. */
	regular = S_ISREG(status.st_mode);
	if (!regular || status.st_size <= 0) {
		job->error = EINVAL;
		close(descriptor);
		return;
	}

	/* Reads the file's size, up to the most a font file may have. */
	capacity = KWL_GLYPH_FILE_MAX;
	if ((uint64_t)status.st_size < KWL_GLYPH_FILE_MAX)
		capacity = (size_t)status.st_size;

	/* Its bytes. */
	data = malloc(capacity);
	if (data == NULL) {
		job->error = ENOMEM;
		close(descriptor);
		return;
	}

	/* Read to the end or the bound: normally one read. */
	length = 0U;
	for (;;) {
		count = read(descriptor, data + length, capacity - length);
		if (count < 0 && errno == EINTR)
			continue;
		if (count <= 0)
			break;
		length += (size_t)count;
		if (length == capacity)
			break;
	}

	/* The file is no longer needed. */
	close(descriptor);

	/* An empty or unreadable file has nothing to use. */
	if (length == 0U) {
		free(data);
		job->error = EIO;
		return;
	}

	/* Succeeded: the bytes are the job's. */
	job->data = data;
	job->size = length;
}

/* Gives the worker's face of a font, opening it over the font's bytes the first time (NULL: none). */
static struct truetype_face *
worker_face(
	struct kwl_glyph_worker *worker,
	unsigned slot)
{
	struct kwl_glyph_font font;
	struct truetype_face *face;
	int error;

	/* A slot past the fonts has none. */
	if (slot >= KWL_GLYPH_FONTS)
		return NULL;

	/* The face opened already. */
	if (worker->faces[slot] != NULL)
		return worker->faces[slot];

	/* The font as the main thread gave it. */
	pthread_mutex_lock(&worker->lock);

	font = worker->fonts[slot];

	pthread_mutex_unlock(&worker->lock);

	/* A font not given has no face. */
	if (font.data == NULL)
		return NULL;

	/* The face over the font's bytes. */
	error = truetype_open(font.data, font.size, 0U, &face);
	if (error != 0)
		return NULL;

	/* Its companions, as the main thread opened them. */
	if (font.bold_path != NULL || font.next_path != NULL)
		(void)truetype_open_companions(face, font.bold_path, font.next_path);

	/* Succeeded: kept until the worker stops. */
	worker->faces[slot] = face;
	return face;
}

/* Writes one byte to the main thread's wake pipe; a full pipe already wakes its reader. */
static void
worker_wake(
	int descriptor)
{
	unsigned char byte;
	ssize_t written;

	/* No pipe: the main thread looks on its own. */
	if (descriptor < 0)
		return;

	/* One byte; EAGAIN means the reader has bytes to wake on already. */
	byte = 1U;
	do {
		written = write(descriptor, &byte, 1U);
	} while (written < 0 && errno == EINTR);
}

/* Frees a list of jobs through next. */
static void
worker_free_list(
	struct kwl_glyph_job *job)
{
	struct kwl_glyph_job *next;

	/* Each job and its coverage. */
	while (job != NULL) {
		next = job->next;
		kwl_glyph_job_free(job);
		job = next;
	}
}
