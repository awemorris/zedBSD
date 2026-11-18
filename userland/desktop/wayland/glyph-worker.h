/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The font thread (the glyph worker of ws213-p005): renders glyphs the main
 * thread asks for away from the event loop, on fonts of its own opened over
 * the same bytes, decodes the colour emoji font's glyphs, and reads the
 * fonts the look opens after its start (the fallback and the emoji fonts)
 * from their files.
 */

#ifndef KWL_GLYPH_WORKER_H
#define KWL_GLYPH_WORKER_H

#include <pthread.h>
#include <stddef.h>
#include <stdint.h>

#include <truetype/truetype.h>

#include "threads.h"
#include "../picture/color-glyph.h"

/* The fonts the worker can be given (the look's first font, its fallback and the colour emoji font). */
#define KWL_GLYPH_FONTS		3U

/* The most bytes of a font file the worker reads. */
#define KWL_GLYPH_FILE_MAX	(16U * 1024U * 1024U)

/* What a job asks of the worker. */
#define KWL_GLYPH_TASK_RENDER	0U	/* a glyph's coverage */
#define KWL_GLYPH_TASK_READ	1U	/* a font file's bytes */
#define KWL_GLYPH_TASK_COLOR	2U	/* a colour glyph's pixels */

/*
 * A font as the main thread opened it, for the worker to open its own face
 * over: the font file's bytes (the main thread keeps them unchanged until
 * the worker stops) and the companions it opened with it (NULL: none).
 */
struct kwl_glyph_font {
	const void *data;
	size_t size;
	const char *bold_path;
	const char *next_path;
};

/*
 * One job of the worker, from the main thread's ask to its taking of the
 * result.  The main thread fills the ask: the task, and for a glyph the
 * font, the size in pixels per em, the glyph, the box it measured (the
 * worker draws only a glyph of that box), for a font file its path (kept
 * by the main thread until the job is taken), and its own marks (kind,
 * slot and generation, which the worker leaves alone).  The worker fills
 * the result: error 0 with the coverage (width times height bytes) and the
 * metrics of the box drawn, a colour glyph's image, or a file's bytes (each
 * its own allocation, freed with the job unless the main thread took it),
 * or the reason it could not.  The job belongs to the worker from its
 * submission to its taking.
 */
struct kwl_glyph_job {
	struct kwl_glyph_job *next;
	unsigned task;
	unsigned font;
	unsigned pixels;
	unsigned glyph;
	uint32_t width;
	uint32_t height;
	const char *path;
	unsigned kind;
	unsigned slot;
	uint64_t generation;
	int error;
	struct truetype_glyph metrics;
	uint8_t *bitmap;
	struct kl_color_image image;
	void *data;
	size_t size;
};

/*
 * The worker and what it shares with the main thread.
 *
 * Under lock: the jobs asked for and not rendered yet (in their order), the
 * jobs rendered and not taken (in the same order), the fonts given, the
 * stop request and whether the main thread was woken for the rendered
 * ones.  work wakes the worker.  The faces are the worker's own; wake_main
 * is the main thread's wake pipe's write end; thread is the worker's
 * thread in the compositor's register (font).
 */
struct kwl_glyph_worker {
	pthread_mutex_t lock;
	pthread_cond_t work;
	struct kwl_glyph_job *jobs;
	struct kwl_glyph_job *jobs_tail;
	struct kwl_glyph_job *done;
	struct kwl_glyph_job *done_tail;
	struct kwl_glyph_font fonts[KWL_GLYPH_FONTS];
	struct truetype_face *faces[KWL_GLYPH_FONTS];
	unsigned stop;
	unsigned main_woken;
	int wake_main;
	struct kwl_thread thread;
};

int kwl_glyph_worker_start(struct kwl_glyph_worker *worker, int wake_main);
void kwl_glyph_worker_stop(struct kwl_glyph_worker *worker);
void kwl_glyph_worker_font(struct kwl_glyph_worker *worker, unsigned slot, const struct kwl_glyph_font *font);
void kwl_glyph_worker_submit(struct kwl_glyph_worker *worker, struct kwl_glyph_job *job);
struct kwl_glyph_job *kwl_glyph_worker_take(struct kwl_glyph_worker *worker);
int kwl_glyph_worker_ready(struct kwl_glyph_worker *worker);
void kwl_glyph_job_free(struct kwl_glyph_job *job);

#endif
