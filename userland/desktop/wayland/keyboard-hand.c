/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The handwriting face's ink and its recognizer (ws102-p008,
 * plan/ws102/design.md §2.7).
 *
 * The ink is the strokes written since it was last cleared, each the
 * points the pen or finger passed through (a point that does not move from
 * the last is not kept).  kwl_hand_recognize answers the candidates, the
 * likeliest first (ws165-p003): the point clouds of hand-cloud.c against
 * the templates of the package hand-hershey, read the first time; then
 * the characters that differ only by their size are put in the order the
 * ink's size on the writing area says (a small c before C), and a small
 * kana is offered before its full size one when the ink is small.
 * kwl_hand_recognize_on (ws165-p005) takes the writing area's top too:
 * the ink's size and place on it count with its shape (hand-cloud.c's
 * hand_recognize_framed).  It also measures the ink (its strokes, points
 * and bounds) for the log.
 *
 * The recognitions run on a pool of threads of their own (hand-1 to
 * hand-N), started when the handwriting face is first shown
 * (kwl_hand_preload): the event loop gives a copy of the ink with its
 * generation (kwl_hand_submit) and takes the answers on its next pass
 * (kwl_hand_take); a newer ink replaces one not started yet, and the
 * answer of an older one is the caller's to drop.  The templates are read
 * once by the first of the pool's threads to need them, the others
 * waiting; so neither the file nor the matching waits on the event loop.
 * Without the pool the event loop recognizes, and reads the templates,
 * itself.  Ink too little to have a shape, and missing or broken
 * templates, are said in the note (ws177-p009).
 */

#include "keyboard.h"
#include "hand-cloud.h"
#include "lane.h"
#include "threads.h"

#include "userland/desktop/paths.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The templates' file (the package hand-hershey), and the largest one read. */
#define HAND_TEMPLATES_PATH	KEILAND_DATADIR "/keiland/hand/hershey.txt"
#define HAND_TEMPLATES_MAX	(1024U * 1024U)

/* The note when no template could be read, and the note for ink too little to read (a tap). */
#define HAND_NO_DATA_NOTE	"No handwriting data"
#define HAND_SCANT_NOTE		"Too small to read"

/* The share of the writing area's height under which ink is small, and the most candidates looked at. */
#define HAND_SMALL_SHARE	0.40f
#define HAND_LOOKED		8U

/*
 * The pool's threads: one fewer than the processors (one at least, the
 * event loop keeps its own), four at most.
 */
#define HAND_POOL_MAX		4U

/*
 * The keys of the pool's jobs: the ink written now (a newer ink replaces
 * the one not started yet) and the templates read ahead (one is enough).
 */
#define HAND_KEY_INK		1U
#define HAND_KEY_TEMPLATES	2U

/*
 * The pairs that differ only by size, the small one first (a small ink
 * takes the small one): the Latin letters' (HAND_SIZES_LATIN of them,
 * told apart by the area itself when it is known), then the kana's (the
 * small kana have no templates).
 */
#define HAND_SIZES_LATIN	8U
static const uint32_t hand_sizes[][2] = {
	{ 'c', 'C' }, { 's', 'S' }, { 'v', 'V' }, { 'w', 'W' }, { 'x', 'X' }, { 'z', 'Z' }, { 'o', 'O' }, { 'p', 'P' },
	{ 0x3041, 0x3042 }, { 0x3043, 0x3044 }, { 0x3045, 0x3046 }, { 0x3047, 0x3048 }, { 0x3049, 0x304a }, { 0x3063, 0x3064 },
	{ 0x3083, 0x3084 }, { 0x3085, 0x3086 }, { 0x3087, 0x3088 }, { 0x308e, 0x308f },
	{ 0x30a1, 0x30a2 }, { 0x30a3, 0x30a4 }, { 0x30a5, 0x30a6 }, { 0x30a7, 0x30a8 }, { 0x30a9, 0x30aa }, { 0x30c3, 0x30c4 },
	{ 0x30e3, 0x30e4 }, { 0x30e5, 0x30e6 }, { 0x30e7, 0x30e8 }, { 0x30ee, 0x30ef }, { 0x30f5, 0x30ab }, { 0x30f6, 0x30b1 }
};

/*
 * The room a recognition puts the ink's points in, in the recognizer's
 * form: each of the pool's jobs has its own, the event loop the one below.
 */
struct hand_scratch {
	float xs[HAND_CLOUD_INPUT_MAX];
	float ys[HAND_CLOUD_INPUT_MAX];
	unsigned char starts[HAND_CLOUD_INPUT_MAX];
};

/*
 * One job of the pool: the templates read ahead (templates_only), or a
 * recognition of a copy of the ink of a generation on its writing area,
 * with its own room for the points and the answer the thread writes.  The
 * main thread fills it before the submission and reads it after the take.
 */
struct hand_job {
	struct kwl_lane_job lane;
	unsigned templates_only;
	uint64_t generation;
	struct hand_frame frame;
	struct kwl_hand_ink ink;
	struct hand_scratch scratch;
	struct kwl_hand_result result;
};

/* Whom kwl_hand_take gives the answers to (the lane's done is called with it). */
struct hand_taker {
	void (*answer)(void *context, const struct kwl_hand_answer *answer);
	void *context;
};

/*
 * The templates, read once.  hand_state says 0 not read yet, 1 read, -1
 * the read failed; hand_reading says a thread reads them now, and
 * hand_read is signaled when it has.  hand_lock guards the three.  Only
 * the thread that set hand_reading writes hand_templates, and a thread
 * reads them only after it saw hand_state 1 under the lock, which makes
 * the reader's writes visible.
 */
static struct hand_templates hand_templates;
static pthread_mutex_t hand_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t hand_read = PTHREAD_COND_INITIALIZER;
static int hand_state;
static unsigned hand_reading;

/*
 * The pool recognizing the ink, and whether it runs: only the event loop
 * starts it, gives to it, takes from it and stops it, so the flag needs
 * no lock.
 */
static struct kwl_lane hand_pool;
static unsigned hand_pool_running;

/* The event loop's room for the points of the recognitions it runs itself (no pool). */
static struct hand_scratch hand_main_scratch;

/* The pool's jobs: run on a thread of the pool, done and dropped on the main thread. */
static void hand_job_run(struct kwl_lane_job *job);
static void hand_job_done(void *context, struct kwl_lane_job *job);
static void hand_job_drop(struct kwl_lane_job *job);
static const struct kwl_lane_ops hand_job_ops = {
	hand_job_run,
	hand_job_done,
	hand_job_drop
};

static void hand_recognize_ink(const struct kwl_hand_ink *ink, const struct hand_frame *frame, int32_t area, size_t first_pair, struct hand_scratch *scratch, struct kwl_hand_result *result);
static void hand_sized(uint32_t *codes, size_t *count, int small, size_t first_pair);
static void hand_utf8(uint32_t code, char *text, size_t size);
static int hand_templates_ready(void);
static int hand_load(const char *path);
static unsigned hand_pool_size(void);
static void hand_ink_input(const struct kwl_hand_ink *ink, struct hand_cloud_input *input, float *xs, float *ys, unsigned char *starts);

/*
 * Clears the ink: no strokes.
 */
void
kwl_hand_clear(
	struct kwl_hand_ink *ink)
{
	/* No stroke is kept (their points are left as they were, unread). */
	ink->count = 0;
}

/*
 * Begins a stroke at a point.  Returns 1, or 0 when the ink has no room
 * for another stroke (the stroke is then not kept).
 */
int
kwl_hand_begin(
	struct kwl_hand_ink *ink,
	int32_t x,
	int32_t y)
{
	struct kwl_hand_stroke *stroke;

	/* No room for another stroke. */
	if (ink->count >= KWL_HAND_STROKES)
		return 0;

	/* The new stroke with its first point. */
	stroke = &ink->strokes[ink->count];
	stroke->count = 1;
	stroke->points[0].x = (int16_t)x;
	stroke->points[0].y = (int16_t)y;
	ink->count++;

	/* Succeeded: the stroke is kept. */
	return 1;
}

/*
 * Adds a point to the stroke being written.  Returns 1 when it was kept,
 * 0 when there is no stroke, it did not move, or the stroke is full.
 */
int
kwl_hand_add(
	struct kwl_hand_ink *ink,
	int32_t x,
	int32_t y)
{
	struct kwl_hand_stroke *stroke;
	const struct kwl_hand_point *last;

	/* The stroke being written. */
	if (ink->count == 0U)
		return 0;
	stroke = &ink->strokes[ink->count - 1U];

	/* A point where the last one was adds nothing. */
	last = &stroke->points[stroke->count - 1U];
	if (last->x == (int16_t)x && last->y == (int16_t)y)
		return 0;

	/* A full stroke keeps no more. */
	if (stroke->count >= KWL_HAND_POINTS)
		return 0;

	/* The point. */
	stroke->points[stroke->count].x = (int16_t)x;
	stroke->points[stroke->count].y = (int16_t)y;
	stroke->count++;

	/* Succeeded: the point is kept. */
	return 1;
}

/*
 * Counts the points of all the ink's strokes.
 */
unsigned
kwl_hand_points(
	const struct kwl_hand_ink *ink)
{
	unsigned stroke;
	unsigned total;

	/* Each stroke's points. */
	total = 0;
	for (stroke = 0; stroke < ink->count; stroke++)
		total += ink->strokes[stroke].count;

	/* The total. */
	return total;
}

/*
 * Works out the rectangle (x, y, width, height) the ink's points cover;
 * all zero without ink.
 */
void
kwl_hand_bounds(
	const struct kwl_hand_ink *ink,
	int32_t *rect)
{
	const struct kwl_hand_point *point;
	unsigned stroke;
	unsigned index;
	int32_t left;
	int32_t top;
	int32_t right;
	int32_t bottom;
	int any;

	/* The points' extremes. */
	any = 0;
	left = 0;
	top = 0;
	right = 0;
	bottom = 0;
	for (stroke = 0; stroke < ink->count; stroke++) {
		for (index = 0; index < ink->strokes[stroke].count; index++) {
			/* The first point starts the extremes; the others widen them. */
			point = &ink->strokes[stroke].points[index];
			if (!any) {
				left = point->x;
				right = point->x;
				top = point->y;
				bottom = point->y;
				any = 1;
				continue;
			}

			/* Wider across. */
			if (point->x < left)
				left = point->x;
			if (point->x > right)
				right = point->x;

			/* Wider down. */
			if (point->y < top)
				top = point->y;
			if (point->y > bottom)
				bottom = point->y;
		}
	}

	/* The rectangle (a point is one pixel). */
	rect[0] = left;
	rect[1] = top;
	rect[2] = 0;
	rect[3] = 0;
	if (any) {
		rect[2] = right - left + 1;
		rect[3] = bottom - top + 1;
	}
}

/*
 * Starts the pool recognizing the ink, the first time the handwriting face
 * is shown, and has one of its threads read the templates ahead, so that
 * the first recognition finds them read.  wake is the
 * write end of the main thread's wake pipe.  When an earlier read failed,
 * puts the note that there is no data in result at once.  Without the
 * pool the first recognition reads the file on the event loop.
 */
void
kwl_hand_preload(
	struct kwl_hand_result *result,
	int wake)
{
	struct hand_job *job;
	unsigned count;
	int state;
	int error;

	/* The pool, once: its threads stay until kwl_hand_stop. */
	if (!hand_pool_running) {
		count = hand_pool_size();
		error = kwl_lane_start(&hand_pool, "hand", count, wake);
		if (error != 0) {
			printf("KWL OSK hand pool error=%d the ink is recognized in the event loop\n", error);
		} else {
			hand_pool_running = 1U;
			printf("KWL OSK hand pool threads=%u\n", count);
			kwl_threads_report();
		}
	}

	/* Whether the templates were read, or failed. */
	pthread_mutex_lock(&hand_lock);

	state = hand_state;

	pthread_mutex_unlock(&hand_lock);

	/* A read that failed: says so before anything is written. */
	if (state == -1) {
		(void)snprintf(result->note, sizeof(result->note), "%s", HAND_NO_DATA_NOTE);
		return;
	}

	/* Read already. */
	if (state == 1)
		return;

	/* Without the pool the first recognition reads them. */
	if (!hand_pool_running)
		return;

	/* The job reading them ahead (one not started yet is replaced: the read happens once). */
	job = malloc(sizeof(*job));
	if (job == NULL)
		return;
	memset(&job->lane, 0, sizeof(job->lane));
	job->templates_only = 1U;
	job->generation = 0U;

	/* Given to the pool. */
	kwl_lane_submit(&hand_pool, &job->lane, &hand_job_ops, HAND_KEY_TEMPLATES);
}

/*
 * Gives the pool a recognition of the ink of a generation, written on an
 * area (its top and height, as kwl_hand_recognize_on takes them): a copy
 * of the ink goes, and a recognition of an older ink not started yet is
 * replaced.  The answer comes from kwl_hand_take.  Returns 0, ENODEV when
 * the pool does not run, or ENOMEM (the caller then recognizes the ink
 * itself).
 */
int
kwl_hand_submit(
	const struct kwl_hand_ink *ink,
	int32_t top,
	int32_t height,
	uint64_t generation)
{
	struct hand_job *job;

	/* Without the pool the caller recognizes. */
	if (!hand_pool_running)
		return ENODEV;

	/* The job, with the ink as it is now and its area. */
	job = malloc(sizeof(*job));
	if (job == NULL)
		return ENOMEM;
	memset(&job->lane, 0, sizeof(job->lane));
	job->templates_only = 0U;
	job->generation = generation;
	job->frame.top = (float)top;
	job->frame.height = (float)height;
	memcpy(&job->ink, ink, sizeof(job->ink));

	/* Given to the pool, in place of an older ink's not started yet. */
	kwl_lane_submit(&hand_pool, &job->lane, &hand_job_ops, HAND_KEY_INK);

	/* Succeeded: the answer comes with a later take. */
	return 0;
}

/*
 * Takes the pool's answers, on the main thread: answer is called for each
 * recognition finished (with context), in the order they finished; the
 * ink in the answer is the job's copy, valid during the call.  Returns how
 * many jobs were taken.
 */
unsigned
kwl_hand_take(
	void (*answer)(void *context, const struct kwl_hand_answer *answer),
	void *context)
{
	struct hand_taker taker;
	unsigned taken;

	/* Without the pool there is nothing to take. */
	if (!hand_pool_running)
		return 0U;

	/* Each finished job given to the answer, and freed. */
	taker.answer = answer;
	taker.context = context;
	taken = kwl_lane_take(&hand_pool, &taker);

	/* Reports the jobs taken. */
	return taken;
}

/*
 * Stops the pool at the end of the session: the recognitions running are
 * finished and every answer not taken is dropped.
 */
void
kwl_hand_stop(void)
{
	/* A pool never started has nothing to stop. */
	if (!hand_pool_running)
		return;

	/* Its threads end, the jobs left freed. */
	kwl_lane_stop(&hand_pool);
	hand_pool_running = 0U;
}

/*
 * Recognizes the ink: the candidates for the character written, the
 * likeliest first.  area is the writing area's height (0 when it is not
 * known: the sizes are then not used); the characters that differ only by
 * size are put in the order the ink's size says (ws165-p003).  Called on
 * the main thread only (the pool's jobs have rooms of their own).
 */
void
kwl_hand_recognize(
	const struct kwl_hand_ink *ink,
	int32_t area,
	struct kwl_hand_result *result)
{
	/* By the shapes, then the sizes' order, in the event loop's room. */
	hand_recognize_ink(ink, NULL, area, 0U, &hand_main_scratch, result);
}

/*
 * Recognizes the ink written on an area (its top and height, in the ink's
 * units; ws165-p005): the size and the place of the ink on the area count
 * with its shape (hand_recognize_framed), so c and C, o and the degree
 * sign, . and the middle dot are told apart; a small kana (not among the
 * templates) is offered before its full size one when the ink is small.
 * Called on the main thread only, when the pool does not run.
 */
void
kwl_hand_recognize_on(
	const struct kwl_hand_ink *ink,
	int32_t top,
	int32_t height,
	struct kwl_hand_result *result)
{
	struct hand_frame frame;

	/* The area, and the small kana by size (the rest is the area's). */
	frame.top = (float)top;
	frame.height = (float)height;
	hand_recognize_ink(ink, &frame, height, HAND_SIZES_LATIN, &hand_main_scratch, result);
}

/*
 * Recognizes the ink, on an area when frame is not NULL; the pairs of
 * hand_sizes from the first one given are then put in the order the ink's
 * size on an area of a height (0: none) says.  The ink's points are put in
 * scratch, the caller's own (one per thread).
 */
static void
hand_recognize_ink(
	const struct kwl_hand_ink *ink,
	const struct hand_frame *frame,
	int32_t area,
	size_t first_pair,
	struct hand_scratch *scratch,
	struct kwl_hand_result *result)
{
	struct hand_cloud_input input;
	uint32_t codes[HAND_LOOKED];
	float distances[HAND_LOOKED];
	int32_t bounds[4];
	size_t count;
	unsigned point;
	int small;
	int scant;
	int state;

	/* Nothing yet. */
	memset(result, 0, sizeof(*result));

	/* Without ink, no candidates. */
	if (ink->count == 0U)
		return;

	/* The templates, read by this thread when no other has or does. */
	state = hand_templates_ready();

	/* Without templates, says so instead of answering. */
	if (state != 1) {
		(void)snprintf(result->note, sizeof(result->note), "%s", HAND_NO_DATA_NOTE);
		return;
	}

	/* Gives the recognizer the ink's points, all its strokes (backlog-p2 line 60). */
	hand_ink_input(ink, &input, scratch->xs, scratch->ys, scratch->starts);

	/* Ink too little to have a shape (a tap) is not read; says so (backlog-p2 line 55). */
	scant = hand_ink_scant(&input, frame);
	if (scant) {
		(void)snprintf(result->note, sizeof(result->note), "%s", HAND_SCANT_NOTE);
		return;
	}

	/* The nearest characters (on the area when there is one), put in the order the ink's size says. */
	count = hand_recognize_framed(&hand_templates, &input, frame, codes, distances, HAND_LOOKED);
	kwl_hand_bounds(ink, bounds);
	small = 0;
	if (area > 0 && (float)bounds[3] < HAND_SMALL_SHARE * (float)area && (float)bounds[2] < HAND_SMALL_SHARE * (float)area)
		small = 1;
	if (area > 0)
		hand_sized(codes, &count, small, first_pair);

	/* The first ones, as text. */
	for (point = 0U; point < count && result->count < KWL_HAND_CANDIDATES; point++) {
		hand_utf8(codes[point], result->candidates[result->count], sizeof(result->candidates[0]));
		result->count++;
	}
}

/*
 * Puts the characters that differ only by size in the order the ink's size
 * says: the first candidate's pair (of those from first_pair on), its
 * small one first for small ink and its large one otherwise, the other
 * right after it (added when it was not among the candidates).
 */
static void
hand_sized(
	uint32_t *codes,
	size_t *count,
	int small,
	size_t first_pair)
{
	uint32_t chosen;
	uint32_t other;
	size_t pair;
	size_t index;

	/* The first candidate's pair. */
	if (*count == 0U)
		return;
	for (pair = first_pair; pair < sizeof(hand_sizes) / sizeof(hand_sizes[0]); pair++) {
		if (codes[0] == hand_sizes[pair][0] || codes[0] == hand_sizes[pair][1])
			break;
	}

	/* Not one of the pairs. */
	if (pair == sizeof(hand_sizes) / sizeof(hand_sizes[0]))
		return;

	/* The one the size says, then the other. */
	chosen = hand_sizes[pair][1];
	other = hand_sizes[pair][0];
	if (small) {
		chosen = hand_sizes[pair][0];
		other = hand_sizes[pair][1];
	}

	/* Both taken out of the list where they are. */
	for (index = 0U; index < *count;) {
		if (codes[index] == chosen || codes[index] == other) {
			memmove(&codes[index], &codes[index + 1U], (*count - index - 1U) * sizeof(codes[0]));
			(*count)--;
			continue;
		}

		/* The next one. */
		index++;
	}

	/* Put first, the list kept within its room. */
	if (*count > HAND_LOOKED - 2U)
		*count = HAND_LOOKED - 2U;
	memmove(&codes[2], &codes[0], *count * sizeof(codes[0]));
	codes[0] = chosen;
	codes[1] = other;
	*count += 2U;
}

/* Writes a code point as UTF-8 (one to three bytes, as the templates have). */
static void
hand_utf8(
	uint32_t code,
	char *text,
	size_t size)
{
	unsigned char bytes[4];

	/* Its bytes. */
	memset(bytes, 0, sizeof(bytes));
	if (code < 0x80U) {
		bytes[0] = (unsigned char)code;
	} else if (code < 0x800U) {
		bytes[0] = (unsigned char)(0xc0U | (code >> 6));
		bytes[1] = (unsigned char)(0x80U | (code & 0x3fU));
	} else {
		bytes[0] = (unsigned char)(0xe0U | (code >> 12));
		bytes[1] = (unsigned char)(0x80U | ((code >> 6) & 0x3fU));
		bytes[2] = (unsigned char)(0x80U | (code & 0x3fU));
	}

	/* As text. */
	(void)snprintf(text, size, "%s", (const char *)bytes);
}

/*
 * Makes sure the templates were read, reading them on this thread when no
 * thread has: a read under way on another thread is waited for.  Returns
 * 1 when they are there, or -1 when the read failed (it is not tried
 * again).
 */
static int
hand_templates_ready(void)
{
	int state;
	int error;

	/* After a read under way; this thread reads when none was done. */
	pthread_mutex_lock(&hand_lock);

	while (hand_reading)
		pthread_cond_wait(&hand_read, &hand_lock);
	state = hand_state;
	if (state == 0)
		hand_reading = 1U;

	pthread_mutex_unlock(&hand_lock);

	/* Read already, or failed. */
	if (state != 0)
		return state;

	/* The file read outside the lock; the others wait on hand_read. */
	error = hand_load(HAND_TEMPLATES_PATH);
	state = 1;
	if (error != 0)
		state = -1;

	/* The outcome for every thread, and the ones waiting woken. */
	pthread_mutex_lock(&hand_lock);

	hand_state = state;
	hand_reading = 0U;
	pthread_cond_broadcast(&hand_read);

	pthread_mutex_unlock(&hand_lock);

	/* Succeeded: the templates are there, or their read failed. */
	return state;
}

/*
 * Reads the templates from a file into hand_templates, on the thread that
 * set hand_reading (hand_templates_ready says the outcome to the others).
 * Returns 0, or an errno value: the file is missing or unreadable, larger
 * than HAND_TEMPLATES_MAX (its end would be cut), or has no template that
 * reads (backlog-p2 line 56).
 */
static int
hand_load(
	const char *path)
{
	FILE *file;
	char *text;
	size_t length;
	int failed;
	int error;

	/* Forgets the templates read before; until the read succeeds there are none. */
	hand_templates_free(&hand_templates);

	/* Opens the file. */
	file = fopen(path, "r");
	if (file == NULL) {
		error = errno;
		printf("KWL OSK hand templates path=%s error=%d\n", path, error);
		return error;
	}

	/* Makes room for its text and one byte more, which tells a file too large. */
	text = malloc(HAND_TEMPLATES_MAX + 1U);
	if (text == NULL) {
		fclose(file);
		return ENOMEM;
	}

	/* Reads its text. */
	length = fread(text, 1U, HAND_TEMPLATES_MAX + 1U, file);
	failed = ferror(file);
	fclose(file);

	/* Refuses a file that could not be read whole. */
	if (failed) {
		free(text);
		printf("KWL OSK hand templates path=%s error=%d\n", path, EIO);
		return EIO;
	}

	/* Refuses a file too large, rather than reading the templates its cut end would garble. */
	if (length > HAND_TEMPLATES_MAX) {
		free(text);
		printf("KWL OSK hand templates path=%s error=%d\n", path, EFBIG);
		return EFBIG;
	}

	/* Reads the templates of it: a broken line or no template at all fails the read. */
	error = hand_templates_parse(&hand_templates, text, length);
	free(text);
	printf("KWL OSK hand templates path=%s count=%lu error=%d\n", path, (unsigned long)hand_templates.count, error);
	if (error != 0)
		return error;

	/* Succeeded: read. */
	return 0;
}

/*
 * Works out the pool's threads from the processors online: one fewer than
 * them, one at least and HAND_POOL_MAX at most.
 */
static unsigned
hand_pool_size(void)
{
	unsigned processors;
	unsigned count;

	/* The processors, the event loop's left to it. */
	processors = kwl_cpu_count();
	count = 1U;
	if (processors > 1U)
		count = processors - 1U;

	/* Within the most the recognitions use. */
	if (count > HAND_POOL_MAX)
		count = HAND_POOL_MAX;

	/* Succeeded: the pool's threads. */
	return count;
}

/*
 * Runs a job of the pool on its thread: the templates read (waiting for
 * another thread's read), then the ink's recognition into the job's answer.
 */
static void
hand_job_run(
	struct kwl_lane_job *lane_job)
{
	struct hand_job *job;

	/* The job of its own kind. */
	job = (struct hand_job *)lane_job;

	/* A job of the templates alone reads them. */
	if (job->templates_only) {
		(void)hand_templates_ready();
		return;
	}

	/* The ink's candidates on its area, in the job's own room. */
	hand_recognize_ink(&job->ink, &job->frame, (int32_t)job->frame.height, HAND_SIZES_LATIN, &job->scratch, &job->result);
}

/*
 * Applies a job the pool finished, on the main thread: a recognition's
 * answer is given to the taker's answer; then the job is freed.
 */
static void
hand_job_done(
	void *context,
	struct kwl_lane_job *lane_job)
{
	const struct hand_taker *taker;
	struct kwl_hand_answer answer;
	struct hand_job *job;

	/* The taker, and the job of its own kind. */
	taker = context;
	job = (struct hand_job *)lane_job;

	/* A recognition's answer, with its generation and ink. */
	if (!job->templates_only) {
		answer.generation = job->generation;
		answer.ink = &job->ink;
		answer.result = job->result;
		taker->answer(taker->context, &answer);
	}

	/* The job's memory. */
	free(job);
}

/* Frees a job not run or not applied (replaced by a newer one, or the pool stopped), on the main thread. */
static void
hand_job_drop(
	struct kwl_lane_job *lane_job)
{
	/* The job's memory. */
	free(lane_job);
}

/*
 * Puts the ink's points in the recognizer's form (xs, ys and starts, of
 * HAND_CLOUD_INPUT_MAX each): all of them, or for ink of more points than
 * that (KWL_HAND_STROKES strokes of KWL_HAND_POINTS) every step-th point
 * of each stroke with its first and its last, so that every stroke still
 * counts (backlog-p2 line 60).
 */
static void
hand_ink_input(
	const struct kwl_hand_ink *ink,
	struct hand_cloud_input *input,
	float *xs,
	float *ys,
	unsigned char *starts)
{
	const struct kwl_hand_stroke *stroke;
	unsigned total;
	unsigned step;
	unsigned room;
	unsigned index;
	unsigned point;
	int kept;

	/*
	 * Works out the step: a stroke keeps at most one point in step and its
	 * last, so the room for the points is what is left after a last point
	 * of every stroke and a first point that may not fall on the step.
	 */
	total = kwl_hand_points(ink);
	room = HAND_CLOUD_INPUT_MAX - 2U * KWL_HAND_STROKES;
	step = 1U;
	if (total > HAND_CLOUD_INPUT_MAX)
		step = (total + room - 1U) / room;

	/* Copies each stroke's kept points, its first marked as the stroke's start. */
	input->count = 0U;
	for (index = 0U; index < ink->count; index++) {
		stroke = &ink->strokes[index];
		for (point = 0U; point < stroke->count; point++) {
			/* The first point, every step-th one and the last one are kept. */
			kept = 0;
			if (point % step == 0U)
				kept = 1;
			else if (point + 1U == stroke->count)
				kept = 1;

			/* A point between the steps is left out. */
			if (!kept)
				continue;

			/* The bound holds by the step; it is checked all the same. */
			if (input->count >= HAND_CLOUD_INPUT_MAX)
				break;
			xs[input->count] = (float)stroke->points[point].x;
			ys[input->count] = (float)stroke->points[point].y;
			starts[input->count] = 0U;
			if (point == 0U)
				starts[input->count] = 1U;
			input->count++;
		}
	}

	/* Points the input at its tables. */
	input->x = xs;
	input->y = ys;
	input->starts = starts;
}
