/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The handwriting recognizer's point clouds (hand-cloud.c, ws165-p002,
 * plan/ws165/phase001/phase.md section 3): a written character and each
 * template are the points of all their strokes taken together (so the
 * order and the number of the strokes do not matter), resampled to
 * HAND_CLOUD_POINTS points spread evenly along the strokes, scaled to fit
 * a unit square (the shape's proportions kept) and moved so that their
 * centre is at the origin.  A written cloud is compared with every
 * template by the greedy matching of $P (Vatavu, Anthony, Wobbrock 2012):
 * each point takes the nearest point of the other cloud not taken yet,
 * from several starting points and both ways, the earlier matches
 * weighing more; the smallest sums are the candidates.
 *
 * The templates are read from text (the package hand-hershey's file,
 * /usr/share/keiland/hand/hershey.txt): a line a template,
 *
 *     U+3042 6000 x,y x,y ... / x,y ...
 *
 * the code point, a number (ignored), and the strokes separated by " / ".
 * A written kana's voicing marks are found apart (hand_recognize_strokes):
 * the dakuten's two short strokes or the handakuten's small ring at the
 * top right are few points of a cloud and weigh too little in it, so they
 * are taken away, the rest is recognized, and the mark is put on the
 * candidates that take it (か + ゛ is が).  A mark written elsewhere (in
 * the body, at the top left) is taken when the rest reads better without
 * it (ws177-p009).  Ink too little to have a shape (a tap) has no
 * candidates: hand_ink_scant tells it, for the caller to say so.
 *
 * A written character's size and place on the writing area tell apart the
 * characters of one shape (c and C, o and the degree sign, . and the
 * middle dot, l and |): hand_recognize_framed takes the area's top and
 * height, and adds to each candidate's distance how far the ink's size
 * and the height of its middle on the area are from its template's on the
 * Hershey glyphs' area (y from -16 to 16; ws165-p005).
 *
 * It knows nothing of the compositor, so the host tests run it alone.
 */

#ifndef KWL_HAND_CLOUD_H
#define KWL_HAND_CLOUD_H

#include <stddef.h>
#include <stdint.h>

/* The points of a cloud, and the most points a stroke list given to hand_cloud_make may have. */
#define HAND_CLOUD_POINTS	32U
#define HAND_CLOUD_INPUT_MAX	8192U

/* One point of a cloud. */
struct hand_cloud_point {
	float x;
	float y;
};

/* A cloud: its points. */
struct hand_cloud {
	struct hand_cloud_point points[HAND_CLOUD_POINTS];
};

/* The box a character's points take: its left, top, right and bottom (y grows downwards). */
struct hand_extent {
	float left;
	float top;
	float right;
	float bottom;
};

/* A template: the character it is (a code point), its cloud and its box on the Hershey glyphs' area. */
struct hand_template {
	uint32_t code;
	struct hand_cloud cloud;
	struct hand_extent extent;
};

/* The area a character is written on: its top and its height, in the points' units (a height of 0: not known). */
struct hand_frame {
	float top;
	float height;
};

/* The templates read: a table of count of them. */
struct hand_templates {
	struct hand_template *items;
	size_t count;
};

/*
 * A stroke list to make a cloud of: count points, and for each point
 * whether it starts a new stroke (the first point always does).
 */
struct hand_cloud_input {
	const float *x;
	const float *y;
	const unsigned char *starts;
	size_t count;
};

int hand_cloud_make(const struct hand_cloud_input *input, struct hand_cloud *cloud);
int hand_ink_scant(const struct hand_cloud_input *input, const struct hand_frame *frame);
float hand_cloud_distance(const struct hand_cloud *written, const struct hand_cloud *template_cloud);
int hand_templates_parse(struct hand_templates *templates, const char *text, size_t length);
void hand_templates_free(struct hand_templates *templates);
size_t hand_recognize(const struct hand_templates *templates, const struct hand_cloud *written, uint32_t *codes, float *distances,
    size_t capacity);
size_t hand_recognize_strokes(const struct hand_templates *templates, const struct hand_cloud_input *input, uint32_t *codes,
    float *distances, size_t capacity);
size_t hand_recognize_framed(const struct hand_templates *templates, const struct hand_cloud_input *input, const struct hand_frame *frame,
    uint32_t *codes, float *distances, size_t capacity);

#endif
