/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The handwriting recognizer's point clouds and their matching
 * (hand-cloud.h, ws165-p002).
 *
 * A cloud is made as $P makes one: the strokes' points resampled to
 * HAND_CLOUD_POINTS points an equal distance apart along the strokes (no
 * point is put on the jump from one stroke to the next), scaled by the
 * larger of the width and the height so that the shape keeps its
 * proportions, and moved so that the mean of the points is the origin.
 * Two clouds are compared from every HAND_CLOUD_STEP-th point of one, both
 * ways: going round from that point, each point takes the nearest point
 * of the other cloud not taken yet, and the distance counts the more the
 * earlier the point comes.  The smallest of these sums is the distance.
 */

#include "hand-cloud.h"

#include <errno.h>
#include <float.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

/* How far apart the starting points of the matching are (about the square root of the points). */
#define HAND_CLOUD_STEP		5U

/* The longest template line's part kept while it is read. */
#define HAND_LINE_MAX		16384U

/* The Hershey glyphs' area (the templates' units): its top and height. */
#define HAND_TEMPLATE_TOP	(-16.0f)
#define HAND_TEMPLATE_HEIGHT	32.0f

/*
 * The weight of the size (a factor of e between the ink's and the
 * template's adds this much) and of the place (the middle a whole area's
 * height away adds this much), and the least size counted (a dot's).
 */
#define HAND_SIZE_WEIGHT	1.0f
#define HAND_PLACE_WEIGHT	3.0f
#define HAND_SIZE_LEAST		0.04f

/* The most candidates a stroke list's recognition works out. */
#define HAND_LOOKED		8U

/*
 * The least size of ink that is read on an area, a share of the area's
 * height: less is a slip of the pen, a tap, not a character (backlog-p2
 * line 55).  A full stop written at the size of a template's is four times
 * as large.
 */
#define HAND_INK_LEAST		0.02f

/* The marks a written kana may carry. */
#define HAND_MARK_NONE		0U
#define HAND_MARK_DAKUTEN	1U
#define HAND_MARK_HANDAKUTEN	2U

/* The most strokes looked at for a mark. */
#define HAND_MARK_STROKES	64U

/*
 * A dakuten written away from the top right (backlog-p2 line 58): how
 * near the middles of its two strokes are, a share of the ink's size, and
 * how much nearer to its template the rest without it must be than the
 * whole ink is to its best one.
 */
#define HAND_MARK_PAIR		0.20f
#define HAND_MARK_LOOSE_SHARE	0.80f

/*
 * One stroke of a written character as the voicing marks are looked for:
 * where its points begin and end in the stroke list, its size (its box's
 * longer side), its box's middle, and whether its ends meet as a ring's
 * do.  It lives while one stroke list is looked at.
 */
struct hand_stroke_shape {
	size_t begin;
	size_t end;
	float size;
	float centre_x;
	float centre_y;
	int closed;
};

static float cloud_match(const struct hand_cloud *from, const struct hand_cloud *to, unsigned start, float bound);
static float cloud_distance(const struct hand_cloud *written, const struct hand_cloud *template_cloud, float bound);
static float cloud_length(const struct hand_cloud_input *input);
static int templates_line(struct hand_templates *templates, const char *line, size_t length);
static void cloud_extent(const struct hand_cloud_input *input, struct hand_extent *extent);
static float framed_penalty(const struct hand_extent *ink, const struct hand_frame *frame, const struct hand_extent *glyph);
static size_t recognize_cloud(const struct hand_templates *templates, const struct hand_cloud *written, const struct hand_extent *ink, const struct hand_frame *frame, uint32_t *codes, float *distances, size_t capacity);
static size_t recognize_strokes(const struct hand_templates *templates, const struct hand_cloud_input *input, const struct hand_frame *frame, uint32_t *codes, float *distances, size_t capacity);
static size_t strokes_unmarked_first(const uint32_t *plain, const float *plain_distances, size_t plain_count, uint32_t *codes, float *distances);
static size_t strokes_marked_first(const uint32_t *base, const float *base_distances, size_t base_count, unsigned mark, const uint32_t *plain, const float *plain_distances, size_t plain_count, uint32_t *codes, float *distances, size_t capacity);
static void strokes_body(const struct hand_cloud_input *input, const unsigned char *marked, struct hand_cloud_input *body, float *xs, float *ys, unsigned char *starts);
static int strokes_mark_fits(const uint32_t *base, const float *base_distances, size_t base_count, unsigned mark, const float *plain_distances, size_t plain_count);
static size_t strokes_measure(const struct hand_cloud_input *input, struct hand_stroke_shape *shapes, struct hand_extent *extent);
static int stroke_in_corner(const struct hand_stroke_shape *shape, const struct hand_extent *extent);
static int stroke_is_ring(const struct hand_stroke_shape *shape, float size);
static void stroke_marked(const struct hand_stroke_shape *shape, unsigned char *mark);
static unsigned strokes_mark(const struct hand_cloud_input *input, unsigned char *mark);
static unsigned strokes_mark_anywhere(const struct hand_cloud_input *input, unsigned char *mark);
static uint32_t strokes_voiced(uint32_t code, unsigned mark);
static uint32_t strokes_unvoiced(uint32_t code);

/*
 * Makes a cloud of a stroke list.  Returns 0, or EINVAL for a list without
 * points.
 */
int
hand_cloud_make(
	const struct hand_cloud_input *input,
	struct hand_cloud *cloud)
{
	struct hand_cloud_point last;
	struct hand_cloud_point next;
	float interval;
	float walked;
	float step;
	float min_x;
	float min_y;
	float max_x;
	float max_y;
	float size;
	float mean_x;
	float mean_y;
	size_t made;
	size_t index;

	/* Points to make it of. */
	if (input->count == 0U)
		return EINVAL;

	/* The resampling: a point every interval along the strokes, the first point first. */
	interval = cloud_length(input) / (float)(HAND_CLOUD_POINTS - 1U);
	cloud->points[0].x = input->x[0];
	cloud->points[0].y = input->y[0];
	made = 1U;
	walked = 0.0f;
	last = cloud->points[0];
	for (index = 1U; index < input->count && made < HAND_CLOUD_POINTS; index++) {
		next.x = input->x[index];
		next.y = input->y[index];

		/* A new stroke starts from its own first point: nothing on the jump. */
		if (input->starts[index]) {
			last = next;
			continue;
		}

		/* As many points as fit on the segment from the last point to this one. */
		step = hypotf(next.x - last.x, next.y - last.y);
		while (interval > 0.0f && walked + step >= interval && made < HAND_CLOUD_POINTS) {
			last.x += (interval - walked) / step * (next.x - last.x);
			last.y += (interval - walked) / step * (next.y - last.y);
			cloud->points[made++] = last;
			step = hypotf(next.x - last.x, next.y - last.y);
			walked = 0.0f;
		}

		/* The rest of the segment is walked. */
		walked += step;
		last = next;
	}

	/* A shortfall (rounding, or a dot) is filled with the last point. */
	while (made < HAND_CLOUD_POINTS) {
		cloud->points[made].x = input->x[input->count - 1U];
		cloud->points[made].y = input->y[input->count - 1U];
		made++;
	}

	/* The bounds and the mean. */
	min_x = FLT_MAX;
	min_y = FLT_MAX;
	max_x = -FLT_MAX;
	max_y = -FLT_MAX;
	for (index = 0U; index < HAND_CLOUD_POINTS; index++) {
		min_x = fminf(min_x, cloud->points[index].x);
		min_y = fminf(min_y, cloud->points[index].y);
		max_x = fmaxf(max_x, cloud->points[index].x);
		max_y = fmaxf(max_y, cloud->points[index].y);
	}

	/* The larger side (a dot has none: 1). */
	size = fmaxf(max_x - min_x, max_y - min_y);
	if (size <= 0.0f)
		size = 1.0f;

	/* Scaled into a unit square, the proportions kept. */
	mean_x = 0.0f;
	mean_y = 0.0f;
	for (index = 0U; index < HAND_CLOUD_POINTS; index++) {
		cloud->points[index].x = (cloud->points[index].x - min_x) / size;
		cloud->points[index].y = (cloud->points[index].y - min_y) / size;
		mean_x += cloud->points[index].x;
		mean_y += cloud->points[index].y;
	}

	/* Moved so that the mean is the origin. */
	mean_x /= (float)HAND_CLOUD_POINTS;
	mean_y /= (float)HAND_CLOUD_POINTS;
	for (index = 0U; index < HAND_CLOUD_POINTS; index++) {
		cloud->points[index].x -= mean_x;
		cloud->points[index].y -= mean_y;
	}

	/* Succeeded: the cloud. */
	return 0;
}

/*
 * Gives the distance of a written cloud from a template's: the smallest
 * weighted sum of the greedy matchings, from every HAND_CLOUD_STEP-th point
 * and both ways.
 */
float
hand_cloud_distance(
	const struct hand_cloud *written,
	const struct hand_cloud *template_cloud)
{
	float distance;

	/* Without a bound. */
	distance = cloud_distance(written, template_cloud, FLT_MAX);
	return distance;
}

/*
 * Reads templates from text (hand-cloud.h).  A line that is not a
 * template's (a comment, an empty one) is skipped.  Returns 0, EINVAL for
 * a template line that does not read, ENOENT for text without a template
 * (the recognizer would answer nothing: backlog-p2 line 56), or ENOMEM.
 */
int
hand_templates_parse(
	struct hand_templates *templates,
	const char *text,
	size_t length)
{
	const char *start;
	const char *end;
	int error;

	/* None yet. */
	templates->items = NULL;
	templates->count = 0U;

	/* Each line. */
	start = text;
	while (start < text + length) {
		end = memchr(start, '\n', (size_t)(text + length - start));
		if (end == NULL)
			end = text + length;

		/* A template's. */
		if (end - start > 2 && start[0] == 'U' && start[1] == '+') {
			error = templates_line(templates, start, (size_t)(end - start));
			if (error != 0) {
				hand_templates_free(templates);
				return error;
			}
		}

		/* The next line. */
		start = end + 1;
	}

	/* Refuses text without a single template: a recognizer of nothing. */
	if (templates->count == 0U) {
		hand_templates_free(templates);
		return ENOENT;
	}

	/* Succeeded: the templates. */
	return 0;
}

/* Frees the templates read. */
void
hand_templates_free(
	struct hand_templates *templates)
{
	/* The table. */
	free(templates->items);
	templates->items = NULL;
	templates->count = 0U;
}

/*
 * Gives the characters nearest to a written cloud, the nearest first: at
 * most capacity code points (a character with several templates once, at
 * its best) and their distances.  Returns how many.
 */
size_t
hand_recognize(
	const struct hand_templates *templates,
	const struct hand_cloud *written,
	uint32_t *codes,
	float *distances,
	size_t capacity)
{
	/* Without an area. */
	return recognize_cloud(templates, written, NULL, NULL, codes, distances, capacity);
}

/*
 * Gives the characters nearest to a written cloud as hand_recognize does;
 * with an area (frame not NULL), each distance has how far the ink's box
 * on the area is from the template's added (framed_penalty).
 */
static size_t
recognize_cloud(
	const struct hand_templates *templates,
	const struct hand_cloud *written,
	const struct hand_extent *ink,
	const struct hand_frame *frame,
	uint32_t *codes,
	float *distances,
	size_t capacity)
{
	size_t count;
	size_t index;
	size_t place;
	size_t seen;
	float distance;
	float bound;
	float penalty;
	int duplicate;

	/* Each template against the cloud, given up once it is farther than the farthest kept. */
	count = 0U;
	for (index = 0U; index < templates->count; index++) {
		bound = FLT_MAX;
		if (count == capacity)
			bound = distances[count - 1U];

		/* The size and the place first: a template already too far is not matched. */
		penalty = 0.0f;
		if (frame != NULL)
			penalty = framed_penalty(ink, frame, &templates->items[index].extent);
		if (penalty >= bound)
			continue;
		distance = cloud_distance(written, &templates->items[index].cloud, bound - penalty);
		distance += penalty;
		if (distance >= bound)
			continue;

		/* A character kept already keeps its better distance. */
		duplicate = 0;
		for (seen = 0U; seen < count; seen++) {
			if (codes[seen] != templates->items[index].code)
				continue;
			duplicate = 1;
			if (distance < distances[seen]) {
				memmove(&codes[seen], &codes[seen + 1U], (count - seen - 1U) * sizeof(codes[0]));
				memmove(&distances[seen], &distances[seen + 1U], (count - seen - 1U) * sizeof(distances[0]));
				count--;
				duplicate = 0;
			}

			/* One of a character is enough. */
			break;
		}

		/* A worse duplicate is not kept. */
		if (duplicate)
			continue;

		/* Its place among the nearest, when it is one of them. */
		place = count;
		while (place > 0U && distances[place - 1U] > distance)
			place--;
		if (place >= capacity)
			continue;
		if (count < capacity)
			count++;
		memmove(&codes[place + 1U], &codes[place], (count - place - 1U) * sizeof(codes[0]));
		memmove(&distances[place + 1U], &distances[place], (count - place - 1U) * sizeof(distances[0]));
		codes[place] = templates->items[index].code;
		distances[place] = distance;
	}

	/* The nearest. */
	return count;
}

/*
 * Recognizes a stroke list: its voicing mark found apart and put on the
 * candidates of the rest that take it (hand-cloud.h), then the
 * candidates of the whole ink after them.  Returns how many, at most
 * capacity, the nearest first.
 */
size_t
hand_recognize_strokes(
	const struct hand_templates *templates,
	const struct hand_cloud_input *input,
	uint32_t *codes,
	float *distances,
	size_t capacity)
{
	/* Without an area. */
	return recognize_strokes(templates, input, NULL, codes, distances, capacity);
}

/*
 * Recognizes a stroke list as hand_recognize_strokes does; with an area
 * (frame not NULL), the size and the place of the ink (or of the rest
 * without its mark) count too (recognize_cloud).  Ink too little to have
 * a shape (hand_ink_scant) has no candidates.
 */
static size_t
recognize_strokes(
	const struct hand_templates *templates,
	const struct hand_cloud_input *input,
	const struct hand_frame *frame,
	uint32_t *codes,
	float *distances,
	size_t capacity)
{
	static float xs[HAND_CLOUD_INPUT_MAX];
	static float ys[HAND_CLOUD_INPUT_MAX];
	static unsigned char starts[HAND_CLOUD_INPUT_MAX];
	unsigned char marked[HAND_CLOUD_INPUT_MAX];
	struct hand_cloud_input body;
	struct hand_cloud cloud;
	struct hand_extent extent;
	uint32_t base[HAND_LOOKED];
	float base_distances[HAND_LOOKED];
	uint32_t plain[HAND_LOOKED];
	float plain_distances[HAND_LOOKED];
	size_t base_count;
	size_t plain_count;
	size_t count;
	unsigned mark;
	int scant;
	int loose;
	int fits;
	int error;

	/* Answers nothing for ink too little to have a shape (backlog-p2 line 55). */
	scant = hand_ink_scant(input, frame);
	if (scant)
		return 0U;

	/* No more candidates are worked out than the tables below hold. */
	if (capacity > HAND_LOOKED)
		capacity = HAND_LOOKED;

	/* Finds the whole ink's candidates. */
	plain_count = 0U;
	cloud_extent(input, &extent);
	error = hand_cloud_make(input, &cloud);
	if (error == 0)
		plain_count = recognize_cloud(templates, &cloud, &extent, frame, plain, plain_distances, capacity);

	/* Looks for a voicing mark at the top right, where a mark is written. */
	mark = HAND_MARK_NONE;
	loose = 0;
	if (input->count <= HAND_CLOUD_INPUT_MAX)
		mark = strokes_mark(input, marked);

	/*
	 * Looks for one written anywhere else (backlog-p2 line 58); such a mark
	 * is taken only when the rest without it reads better, below.
	 */
	if (mark == HAND_MARK_NONE && input->count <= HAND_CLOUD_INPUT_MAX) {
		mark = strokes_mark_anywhere(input, marked);
		loose = 1;
	}

	/* Without a mark, answers the whole ink's candidates, those without a mark first. */
	if (mark == HAND_MARK_NONE) {
		count = strokes_unmarked_first(plain, plain_distances, plain_count, codes, distances);

		/* Succeeded: the candidates of the ink as it is. */
		return count;
	}

	/* Recognizes the rest of the ink without the mark's strokes. */
	strokes_body(input, marked, &body, xs, ys, starts);
	base_count = 0U;
	cloud_extent(&body, &extent);
	error = hand_cloud_make(&body, &cloud);
	if (error == 0)
		base_count = recognize_cloud(templates, &cloud, &extent, frame, base, base_distances, HAND_LOOKED);

	/* Gives up a mark found away from the top right unless the rest is a kana that takes it, read better alone. */
	if (loose) {
		fits = strokes_mark_fits(base, base_distances, base_count, mark, plain_distances, plain_count);
		if (!fits) {
			count = strokes_unmarked_first(plain, plain_distances, plain_count, codes, distances);

			/* Succeeded: the candidates of the ink as it is, the small strokes not a mark. */
			return count;
		}
	}

	/* Answers the rest's candidates that take the mark, with the mark, then the whole ink's. */
	count = strokes_marked_first(base, base_distances, base_count, mark, plain, plain_distances, plain_count, codes, distances, capacity);

	/* Succeeded: the candidates of the ink read with its mark. */
	return count;
}

/*
 * Recognizes a stroke list written on an area as hand_recognize_strokes
 * does, each template's distance with how far the ink's size and place on
 * the area are from the template's on the Hershey glyphs' area added: a
 * small c before C, a dot low on the area before the middle dot.  An area
 * of no height is recognized without it.  Returns how many candidates, at
 * most capacity, the likeliest first.
 */
size_t
hand_recognize_framed(
	const struct hand_templates *templates,
	const struct hand_cloud_input *input,
	const struct hand_frame *frame,
	uint32_t *codes,
	float *distances,
	size_t capacity)
{
	/* An area of no height is none. */
	if (frame == NULL || frame->height <= 0.0f)
		return recognize_strokes(templates, input, NULL, codes, distances, capacity);
	return recognize_strokes(templates, input, frame, codes, distances, capacity);
}

/*
 * Tells whether a stroke list is too little ink to have a shape
 * (backlog-p2 line 55): a single point, strokes that never move, or on an
 * area (frame not NULL, of a height) ink smaller than HAND_INK_LEAST of
 * it.  Such ink is not recognized; the caller says so instead.
 */
int
hand_ink_scant(
	const struct hand_cloud_input *input,
	const struct hand_frame *frame)
{
	struct hand_extent extent;
	float length;
	float size;

	/* A single point has no shape. */
	if (input->count < 2U)
		return 1;

	/* Strokes that never move have none either. */
	length = cloud_length(input);
	if (length <= 0.0f)
		return 1;

	/* Without an area there is no measure of small: any movement has a shape. */
	if (frame == NULL)
		return 0;
	if (frame->height <= 0.0f)
		return 0;

	/* Ink smaller than its share of the area is a slip of the pen. */
	cloud_extent(input, &extent);
	size = fmaxf(extent.right - extent.left, extent.bottom - extent.top);
	if (size < HAND_INK_LEAST * frame->height)
		return 1;

	/* Succeeded: enough ink to read. */
	return 0;
}

/*
 * Gives the distance of two clouds (hand_cloud_distance's), or bound
 * once every way has passed it.
 */
static float
cloud_distance(
	const struct hand_cloud *written,
	const struct hand_cloud *template_cloud,
	float bound)
{
	float best;
	float one;
	float other;
	unsigned start;

	/* Each starting point, each way; a sum past the best so far is given up. */
	best = bound;
	for (start = 0U; start < HAND_CLOUD_POINTS; start += HAND_CLOUD_STEP) {
		one = cloud_match(written, template_cloud, start, best);
		best = fminf(best, one);
		other = cloud_match(template_cloud, written, start, best);
		best = fminf(best, other);
	}

	/* The smallest, or the bound. */
	return best;
}

/*
 * Matches one cloud's points with the other's, greedily from start round
 * to it again: the weighted sum of the distances, or bound as soon as the
 * sum passes it.
 */
static float
cloud_match(
	const struct hand_cloud *from,
	const struct hand_cloud *to,
	unsigned start,
	float bound)
{
	unsigned char taken[HAND_CLOUD_POINTS];
	unsigned index;
	unsigned other;
	unsigned nearest;
	float sum;
	float weight;
	float distance;
	float smallest;
	float dx;
	float dy;

	/* Nothing taken yet. */
	memset(taken, 0, sizeof(taken));
	sum = 0.0f;
	index = start;

	/* Each point in turn, the earlier weighing more. */
	do {
		smallest = FLT_MAX;
		nearest = 0U;
		for (other = 0U; other < HAND_CLOUD_POINTS; other++) {
			if (taken[other])
				continue;
			dx = from->points[index].x - to->points[other].x;
			dy = from->points[index].y - to->points[other].y;
			distance = dx * dx + dy * dy;
			if (distance < smallest) {
				smallest = distance;
				nearest = other;
			}
		}

		/* Taken, its distance weighed by how early the point came. */
		taken[nearest] = 1U;
		weight = 1.0f - (float)((index + HAND_CLOUD_POINTS - start) % HAND_CLOUD_POINTS) / (float)HAND_CLOUD_POINTS;
		sum += weight * sqrtf(smallest);
		if (sum >= bound)
			return bound;
		index = (index + 1U) % HAND_CLOUD_POINTS;
	} while (index != start);

	/* The sum. */
	return sum;
}

/* Gives the length of the strokes (the jumps between them not counted). */
static float
cloud_length(
	const struct hand_cloud_input *input)
{
	float length;
	size_t index;

	/* Each segment within a stroke. */
	length = 0.0f;
	for (index = 1U; index < input->count; index++) {
		if (input->starts[index])
			continue;
		length += hypotf(input->x[index] - input->x[index - 1U], input->y[index] - input->y[index - 1U]);
	}

	/* The sum. */
	return length;
}

/*
 * Reads one template's line ("U+XXXX NUMBER x,y x,y / x,y ...") into a
 * new template.  Returns 0, EINVAL or ENOMEM.
 *
 * The words are found by walking the copy rather than with strtok, whose
 * hidden state another thread could share: the compositor reads the
 * templates on a thread of its own (keyboard-hand.c, backlog-p2 line 59).
 */
static int
templates_line(
	struct hand_templates *templates,
	const char *line,
	size_t length)
{
	static float xs[HAND_CLOUD_INPUT_MAX];
	static float ys[HAND_CLOUD_INPUT_MAX];
	static unsigned char starts[HAND_CLOUD_INPUT_MAX];
	struct hand_cloud_input input;
	struct hand_template *grown;
	char copy[HAND_LINE_MAX];
	char *word;
	char *end;
	unsigned long code;
	long x;
	long y;
	size_t count;
	int fresh;
	int error;

	/* Refuses a line longer than the copy it is cut in. */
	if (length >= sizeof(copy))
		return EINVAL;
	memcpy(copy, line, length);
	copy[length] = '\0';

	/* Reads the code point, which must be a whole word of a real character. */
	code = strtoul(copy + 2, &end, 16);
	if (end == copy + 2)
		return EINVAL;
	if (*end != ' ')
		return EINVAL;
	if (code == 0UL || code > 0x10ffffUL)
		return EINVAL;

	/* Skips the Hershey number, the word after the code point. */
	word = strchr(end + 1, ' ');
	if (word == NULL)
		return EINVAL;

	/* Reads the points word by word; a lone "/" starts a new stroke. */
	count = 0U;
	fresh = 1;
	while (*word != '\0') {
		/* The spaces between the words. */
		if (*word == ' ') {
			word++;
			continue;
		}

		/* A stroke's end: the next point is the first of another one. */
		if (word[0] == '/') {
			if (word[1] != ' ' && word[1] != '\0')
				return EINVAL;
			fresh = 1;
			word++;
			continue;
		}

		/* Refuses a template with more points than a cloud is made of. */
		if (count >= HAND_CLOUD_INPUT_MAX)
			return EINVAL;

		/* A point "x,y", which must end at a space or at the line's end. */
		x = strtol(word, &end, 10);
		if (end == word || *end != ',')
			return EINVAL;
		word = end + 1;
		y = strtol(word, &end, 10);
		if (end == word)
			return EINVAL;
		if (*end != ' ' && *end != '\0')
			return EINVAL;
		word = end;

		/* Keeps the point, marked when it starts a stroke. */
		xs[count] = (float)x;
		ys[count] = (float)y;
		starts[count] = (unsigned char)fresh;
		fresh = 0;
		count++;
	}

	/* Grows the table by one place for it. */
	grown = realloc(templates->items, (templates->count + 1U) * sizeof(templates->items[0]));
	if (grown == NULL)
		return ENOMEM;
	templates->items = grown;

	/* Makes its cloud; a template without points is refused there. */
	input.x = xs;
	input.y = ys;
	input.starts = starts;
	input.count = count;
	error = hand_cloud_make(&input, &templates->items[templates->count].cloud);
	if (error != 0)
		return error;

	/* Publishes it with its character and its box. */
	templates->items[templates->count].code = (uint32_t)code;
	cloud_extent(&input, &templates->items[templates->count].extent);
	templates->count++;

	/* Succeeded: one more template. */
	return 0;
}

/*
 * Puts a recognition's candidates in order without a mark: the
 * characters without a mark first, as a voiced one needs its mark, then
 * the voiced ones.  Returns how many, plain_count.
 */
static size_t
strokes_unmarked_first(
	const uint32_t *plain,
	const float *plain_distances,
	size_t plain_count,
	uint32_t *codes,
	float *distances)
{
	uint32_t unvoiced;
	size_t count;
	size_t index;

	/* Copies the characters without a mark first. */
	count = 0U;
	for (index = 0U; index < plain_count; index++) {
		unvoiced = strokes_unvoiced(plain[index]);
		if (unvoiced != 0U)
			continue;
		codes[count] = plain[index];
		distances[count] = plain_distances[index];
		count++;
	}

	/* Then copies the voiced ones. */
	for (index = 0U; index < plain_count; index++) {
		unvoiced = strokes_unvoiced(plain[index]);
		if (unvoiced == 0U)
			continue;
		codes[count] = plain[index];
		distances[count] = plain_distances[index];
		count++;
	}

	/* Succeeded: all of them, in the new order. */
	return count;
}

/*
 * Puts a recognition's candidates in order with a mark: the rest's
 * candidates that take the mark, with it (か and a dakuten: が), then the
 * whole ink's not given already.  Returns how many, at most capacity.
 */
static size_t
strokes_marked_first(
	const uint32_t *base,
	const float *base_distances,
	size_t base_count,
	unsigned mark,
	const uint32_t *plain,
	const float *plain_distances,
	size_t plain_count,
	uint32_t *codes,
	float *distances,
	size_t capacity)
{
	uint32_t voiced;
	size_t count;
	size_t index;
	size_t seen;
	int duplicate;

	/* Copies the rest's candidates that take the mark, each with the mark. */
	count = 0U;
	for (index = 0U; index < base_count && count < capacity; index++) {
		voiced = strokes_voiced(base[index], mark);
		if (voiced == 0U)
			continue;
		codes[count] = voiced;
		distances[count] = base_distances[index];
		count++;
	}

	/* Then copies the whole ink's candidates, each character once. */
	for (index = 0U; index < plain_count && count < capacity; index++) {
		/* Looks for the character among those given already. */
		duplicate = 0;
		for (seen = 0U; seen < count; seen++) {
			if (codes[seen] == plain[index])
				duplicate = 1;
		}

		/* A character given already is not given twice. */
		if (duplicate)
			continue;
		codes[count] = plain[index];
		distances[count] = plain_distances[index];
		count++;
	}

	/* Succeeded: the candidates with the mark first. */
	return count;
}

/*
 * Copies the points of a stroke list not marked in marked into body (its
 * tables xs, ys and starts): the rest of a character without its voicing
 * mark, each stroke still starting where it did.
 */
static void
strokes_body(
	const struct hand_cloud_input *input,
	const unsigned char *marked,
	struct hand_cloud_input *body,
	float *xs,
	float *ys,
	unsigned char *starts)
{
	size_t index;
	int fresh;

	/* Copies each point not of the mark; the first one copied of a stroke starts it. */
	body->count = 0U;
	fresh = 1;
	for (index = 0U; index < input->count; index++) {
		if (input->starts[index])
			fresh = 1;
		if (marked[index])
			continue;
		xs[body->count] = input->x[index];
		ys[body->count] = input->y[index];
		starts[body->count] = (unsigned char)fresh;
		fresh = 0;
		body->count++;
	}

	/* Points the body at its tables. */
	body->x = xs;
	body->y = ys;
	body->starts = starts;
}

/*
 * Tells whether a mark found away from the top right is one: the rest
 * without it has a candidate that takes it, and that candidate is nearer
 * to the rest, by HAND_MARK_LOOSE_SHARE, than the whole ink's best is to
 * the whole ink (the small strokes of シ are not a dakuten: シ as written
 * is nearer to its template than its long stroke alone is to し).
 */
static int
strokes_mark_fits(
	const uint32_t *base,
	const float *base_distances,
	size_t base_count,
	unsigned mark,
	const float *plain_distances,
	size_t plain_count)
{
	uint32_t voiced;
	size_t index;

	/* Finds the rest's first candidate that takes the mark. */
	for (index = 0U; index < base_count; index++) {
		voiced = strokes_voiced(base[index], mark);
		if (voiced != 0U)
			break;
	}

	/* A rest that is no kana taking the mark: the small strokes are the character's own. */
	if (index == base_count)
		return 0;

	/* Ink with no candidates of its own as a whole is read with the mark. */
	if (plain_count == 0U)
		return 1;

	/* The rest read alone no better than the whole ink: the small strokes belong to it. */
	if (base_distances[index] >= HAND_MARK_LOOSE_SHARE * plain_distances[0])
		return 0;

	/* Succeeded: the small strokes are a mark. */
	return 1;
}

/*
 * Measures the strokes of a stroke list for the voicing marks: each
 * stroke's points, size, middle and whether it closes on itself, into
 * shapes, and the box of the whole ink into extent.  Returns how many
 * strokes, or 0 for a list of more than HAND_MARK_STROKES of them (no
 * mark is looked for in it).
 */
static size_t
strokes_measure(
	const struct hand_cloud_input *input,
	struct hand_stroke_shape *shapes,
	struct hand_extent *extent)
{
	struct hand_extent box;
	struct hand_cloud_input stroke;
	struct hand_stroke_shape *shape;
	size_t strokes;
	size_t index;
	float gap;

	/* Finds where each stroke begins and ends. */
	strokes = 0U;
	for (index = 0U; index < input->count; index++) {
		if (!input->starts[index])
			continue;
		if (strokes == HAND_MARK_STROKES)
			return 0U;
		if (strokes > 0U)
			shapes[strokes - 1U].end = index;
		shapes[strokes].begin = index;
		strokes++;
	}

	/* The last stroke ends with the list. */
	if (strokes > 0U)
		shapes[strokes - 1U].end = input->count;

	/* Measures each stroke: its size, its middle, and how far its ends are apart. */
	for (index = 0U; index < strokes; index++) {
		shape = &shapes[index];
		stroke.x = input->x + shape->begin;
		stroke.y = input->y + shape->begin;
		stroke.starts = input->starts + shape->begin;
		stroke.count = shape->end - shape->begin;
		cloud_extent(&stroke, &box);
		shape->size = fmaxf(box.right - box.left, box.bottom - box.top);
		shape->centre_x = 0.5f * (box.left + box.right);
		shape->centre_y = 0.5f * (box.top + box.bottom);

		/* A stroke of five points or more whose ends meet closes on itself. */
		gap = hypotf(input->x[shape->end - 1U] - input->x[shape->begin], input->y[shape->end - 1U] - input->y[shape->begin]);
		shape->closed = 0;
		if (gap <= 0.35f * shape->size && stroke.count >= 5U)
			shape->closed = 1;
	}

	/* Measures the whole ink. */
	cloud_extent(input, extent);

	/* Succeeded: the strokes measured. */
	return strokes;
}

/* Tells whether a stroke is at the top right of the ink, where a voicing mark is written. */
static int
stroke_in_corner(
	const struct hand_stroke_shape *shape,
	const struct hand_extent *extent)
{
	float width;
	float height;

	/* The ink's size. */
	width = extent->right - extent->left;
	height = extent->bottom - extent->top;

	/* Left of the right 45% of the ink. */
	if (shape->centre_x < extent->left + 0.55f * width)
		return 0;

	/* Below the top 40% of the ink. */
	if (shape->centre_y > extent->top + 0.40f * height)
		return 0;

	/* Succeeded: in the corner. */
	return 1;
}

/* Tells whether a stroke is a small ring of ink of a size, as a handakuten is. */
static int
stroke_is_ring(
	const struct hand_stroke_shape *shape,
	float size)
{
	/* A stroke that does not close on itself. */
	if (!shape->closed)
		return 0;

	/* Larger than a quarter of the ink. */
	if (shape->size > 0.25f * size)
		return 0;

	/* So small that its closing tells nothing (a dot). */
	if (shape->size < 0.05f * size)
		return 0;

	/* Succeeded: a ring. */
	return 1;
}

/* Marks the points of one stroke in mark. */
static void
stroke_marked(
	const struct hand_stroke_shape *shape,
	unsigned char *mark)
{
	size_t point;

	/* Each of its points. */
	for (point = shape->begin; point < shape->end; point++)
		mark[point] = 1U;
}

/*
 * Finds a voicing mark among the strokes: small strokes at the top right
 * of the ink, two or more open ones a dakuten, a ring a handakuten.
 * Marks their points in mark and returns HAND_MARK_*.
 */
static unsigned
strokes_mark(
	const struct hand_cloud_input *input,
	unsigned char *mark)
{
	struct hand_stroke_shape shapes[HAND_MARK_STROKES];
	unsigned char is_small[HAND_MARK_STROKES];
	struct hand_extent extent;
	size_t strokes;
	size_t index;
	size_t small;
	size_t rings;
	float size;
	int corner;
	int ring;

	/* Measures the strokes; nothing is marked yet. */
	memset(mark, 0, input->count);
	strokes = strokes_measure(input, shapes, &extent);
	size = fmaxf(extent.right - extent.left, extent.bottom - extent.top);

	/* A single stroke, or ink without a size, has no mark. */
	if (strokes < 2U)
		return HAND_MARK_NONE;
	if (size <= 0.0f)
		return HAND_MARK_NONE;

	/* Finds the strokes small and at the top right, and which of them are rings. */
	small = 0U;
	rings = 0U;
	for (index = 0U; index < strokes; index++) {
		/* A stroke away from the corner is the character's. */
		is_small[index] = 0U;
		corner = stroke_in_corner(&shapes[index], &extent);
		if (!corner)
			continue;

		/* A small ring, or an open stroke smaller still. */
		ring = stroke_is_ring(&shapes[index], size);
		if (ring) {
			is_small[index] = 1U;
			rings++;
		} else if (shapes[index].size <= 0.18f * size) {
			is_small[index] = 1U;
		}

		/* Counts the small ones. */
		if (is_small[index])
			small++;
	}

	/* No small stroke, every stroke small, or a single open one: no mark. */
	if (small == 0U)
		return HAND_MARK_NONE;
	if (small == strokes)
		return HAND_MARK_NONE;
	if (rings == 0U && small < 2U)
		return HAND_MARK_NONE;

	/* Marks the small strokes' points. */
	for (index = 0U; index < strokes; index++) {
		if (is_small[index])
			stroke_marked(&shapes[index], mark);
	}

	/* A ring is a handakuten. */
	if (rings != 0U)
		return HAND_MARK_HANDAKUTEN;

	/* Open strokes are a dakuten. */
	return HAND_MARK_DAKUTEN;
}

/*
 * Finds a voicing mark written anywhere on the ink (backlog-p2 line 58):
 * a small ring is a handakuten, two small open strokes near each other a
 * dakuten.  Marks their points in mark and returns HAND_MARK_*; the
 * caller still asks whether the rest reads better without them
 * (strokes_mark_fits), as such strokes may be the character's own.
 */
static unsigned
strokes_mark_anywhere(
	const struct hand_cloud_input *input,
	unsigned char *mark)
{
	struct hand_stroke_shape shapes[HAND_MARK_STROKES];
	struct hand_extent extent;
	size_t strokes;
	size_t index;
	size_t other;
	float size;
	float apart;
	int ring;

	/* Measures the strokes; nothing is marked yet. */
	memset(mark, 0, input->count);
	strokes = strokes_measure(input, shapes, &extent);
	size = fmaxf(extent.right - extent.left, extent.bottom - extent.top);

	/* A single stroke, or ink without a size, has no mark. */
	if (strokes < 2U)
		return HAND_MARK_NONE;
	if (size <= 0.0f)
		return HAND_MARK_NONE;

	/* Takes the first small ring as a handakuten. */
	for (index = 0U; index < strokes; index++) {
		ring = stroke_is_ring(&shapes[index], size);
		if (!ring)
			continue;
		stroke_marked(&shapes[index], mark);

		/* Succeeded: a handakuten. */
		return HAND_MARK_HANDAKUTEN;
	}

	/* A dakuten needs a stroke of the character besides its two. */
	if (strokes < 3U)
		return HAND_MARK_NONE;

	/* Takes the first two small strokes near each other as a dakuten. */
	for (index = 0U; index < strokes; index++) {
		/* A stroke too large for a dakuten's. */
		if (shapes[index].size > 0.18f * size)
			continue;
		for (other = index + 1U; other < strokes; other++) {
			/* Another stroke too large, or too far away to be of one mark. */
			if (shapes[other].size > 0.18f * size)
				continue;
			apart = hypotf(shapes[other].centre_x - shapes[index].centre_x, shapes[other].centre_y - shapes[index].centre_y);
			if (apart > HAND_MARK_PAIR * size)
				continue;
			stroke_marked(&shapes[index], mark);
			stroke_marked(&shapes[other], mark);

			/* Succeeded: a dakuten. */
			return HAND_MARK_DAKUTEN;
		}
	}

	/* No mark. */
	return HAND_MARK_NONE;
}

/* Gives a kana with a mark (が for か and the dakuten), or 0 for one that does not take it. */
static uint32_t
strokes_voiced(
	uint32_t code,
	unsigned mark)
{
	static const uint16_t dakuten[] = {
		0x304b, 0x304d, 0x304f, 0x3051, 0x3053, 0x3055, 0x3057, 0x3059, 0x305b, 0x305d, 0x305f, 0x3061, 0x3064,
		0x3066, 0x3068, 0x306f, 0x3072, 0x3075, 0x3078, 0x307b
	};
	static const uint16_t handakuten[] = { 0x306f, 0x3072, 0x3075, 0x3078, 0x307b };
	uint32_t base;
	uint32_t shift;
	size_t index;

	/* Katakana as hiragana (the same distance apart). */
	shift = 0U;
	base = code;
	if (code >= 0x30a1U && code <= 0x30f6U) {
		shift = 0x60U;
		base = code - shift;
	}

	/* The dakuten: the next code point. */
	if (mark == HAND_MARK_DAKUTEN) {
		for (index = 0U; index < sizeof(dakuten) / sizeof(dakuten[0]); index++) {
			if (dakuten[index] == base)
				return base + 1U + shift;
		}

		/* One that does not take it. */
		return 0U;
	}

	/* The handakuten: two on. */
	for (index = 0U; index < sizeof(handakuten) / sizeof(handakuten[0]); index++) {
		if (handakuten[index] == base)
			return base + 2U + shift;
	}

	/* None. */
	return 0U;
}

/* Gives the kana without its mark for a voiced one (か for が), or 0 for a character without a mark. */
static uint32_t
strokes_unvoiced(
	uint32_t code)
{
	uint32_t base;
	uint32_t dakuten;
	uint32_t handakuten;

	/* Each kana that takes a mark, with each mark it takes. */
	for (base = 0x304bU; base <= 0x307bU; base++) {
		dakuten = strokes_voiced(base, HAND_MARK_DAKUTEN);
		handakuten = strokes_voiced(base, HAND_MARK_HANDAKUTEN);
		if (dakuten == code || handakuten == code)
			return base;
		dakuten = strokes_voiced(base + 0x60U, HAND_MARK_DAKUTEN);
		handakuten = strokes_voiced(base + 0x60U, HAND_MARK_HANDAKUTEN);
		if (dakuten == code || handakuten == code)
			return base + 0x60U;
	}

	/* Not a voiced kana. */
	return 0U;
}

/* Finds the box all the points of a stroke list take (an empty list: all zero). */
static void
cloud_extent(
	const struct hand_cloud_input *input,
	struct hand_extent *extent)
{
	size_t index;

	/* None. */
	memset(extent, 0, sizeof(*extent));
	if (input->count == 0U)
		return;

	/* Each point widens it. */
	extent->left = input->x[0];
	extent->right = input->x[0];
	extent->top = input->y[0];
	extent->bottom = input->y[0];
	for (index = 1U; index < input->count; index++) {
		extent->left = fminf(extent->left, input->x[index]);
		extent->right = fmaxf(extent->right, input->x[index]);
		extent->top = fminf(extent->top, input->y[index]);
		extent->bottom = fmaxf(extent->bottom, input->y[index]);
	}
}

/*
 * Gives how far a written character's size and place on its area are from
 * a template's on the Hershey glyphs' area: the size (the longer side, a
 * share of the area's height) as the logarithm of their ratio, and the
 * height of the middle, a share of the area's, each weighed.
 */
static float
framed_penalty(
	const struct hand_extent *ink,
	const struct hand_frame *frame,
	const struct hand_extent *glyph)
{
	float ink_size;
	float glyph_size;
	float ink_middle;
	float glyph_middle;

	/* The sizes, a dot's at least. */
	ink_size = fmaxf(ink->right - ink->left, ink->bottom - ink->top) / frame->height;
	glyph_size = fmaxf(glyph->right - glyph->left, glyph->bottom - glyph->top) / HAND_TEMPLATE_HEIGHT;
	ink_size = fmaxf(ink_size, HAND_SIZE_LEAST);
	glyph_size = fmaxf(glyph_size, HAND_SIZE_LEAST);

	/* The middles' heights on their areas. */
	ink_middle = ((ink->top + ink->bottom) * 0.5f - frame->top) / frame->height;
	glyph_middle = ((glyph->top + glyph->bottom) * 0.5f - HAND_TEMPLATE_TOP) / HAND_TEMPLATE_HEIGHT;

	/* Weighed. */
	return HAND_SIZE_WEIGHT * fabsf(logf(ink_size / glyph_size)) + HAND_PLACE_WEIGHT * fabsf(ink_middle - glyph_middle);
}
