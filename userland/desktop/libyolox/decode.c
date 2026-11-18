/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The head's decoding and NMS.
 *
 * The anchors are the levels 8, 16 and 32 in order, each row by row.  An
 * anchor's box is cx = (reg0 + x) s, cy = (reg1 + y) s, w = exp(reg2) s,
 * h = exp(reg3) s; its class is the largest sigmoid of the class logits
 * (the lower index on a tie) and its score the sigmoid of the objectness
 * times that.  Anchors at or above the threshold are candidates.
 *
 * NMS is zedBSD's own rule, independent of any library's version: the
 * candidates are sorted by score, high first, then by anchor and class,
 * low first, which is stable and deterministic; each is kept unless it
 * overlaps a kept one (of its class, when classes are told apart) by an
 * IoU above the threshold.  IoU is inter / (area_a + area_b - inter) in
 * float32, areas without "+1", and 0 for an empty union.  The kept boxes
 * are divided by the float32 ratio into the image's coordinates.
 *
 * Procedure referenced: YOLOX yolox/models/yolo_head.py (decode_outputs)
 * and yolox/utils/boxes.py (postprocess) for the box formula and the
 * score; the code is independent.
 */

#include <math.h>
#include <stdlib.h>

#include "yolox-internal.h"

static float decode_sigmoid(float value);
static float decode_iou(const struct yolox_candidate *a, const struct yolox_candidate *b);
static int decode_order(const void *left, const void *right);

/*
 * Decodes the head tensors, runs NMS and writes the kept boxes.
 */
int
yolox_decode(
	struct yolox_runner *runner,
	const struct yolox_letterbox *box,
	struct yolox_detection *detections,
	uint32_t capacity,
	uint32_t *count)
{
	const struct yolox_model *model;
	struct yolox_candidate *candidates;
	const float *reg;
	const float *obj;
	const float *cls;
	uint32_t candidate_count;
	uint32_t kept;
	uint32_t anchor;
	uint32_t level;
	uint32_t side;
	uint32_t area;
	uint32_t place;
	uint32_t x;
	uint32_t y;
	uint32_t c;
	uint32_t i;
	uint32_t j;
	uint32_t best_class;
	float best;
	float probability;
	float score;
	float stride;
	float cx;
	float cy;
	float w;
	float h;
	float ratio;
	float overlap;
	unsigned suppressed;

	/* Every anchor of every level, in order. */
	model = runner->model;
	candidates = runner->candidates;
	candidate_count = 0U;
	anchor = 0U;
	for (level = 0U; level < YOLOX_LEVELS; level++) {
		reg = runner->buffers[model->level_tensors[level][0]];
		obj = runner->buffers[model->level_tensors[level][1]];
		cls = runner->buffers[model->level_tensors[level][2]];
		side = runner->size / model->strides[level];
		area = side * side;
		stride = (float)model->strides[level];
		for (y = 0U; y < side; y++) {
			for (x = 0U; x < side; x++, anchor++) {
				place = y * side + x;

				/* The class with the largest probability, the lower on a tie. */
				best = -1.0f;
				best_class = 0U;
				for (c = 0U; c < YOLOX_CLASSES; c++) {
					probability = decode_sigmoid(cls[(size_t)c * area + place]);
					if (probability > best) {
						best = probability;
						best_class = c;
					}
				}

				/* Below the threshold is not a candidate. */
				score = decode_sigmoid(obj[place]) * best;
				if (score < runner->score_threshold)
					continue;

				/* The box in the network's coordinates. */
				cx = (reg[place] + (float)x) * stride;
				cy = (reg[(size_t)area + place] + (float)y) * stride;
				w = expf(reg[2U * (size_t)area + place]) * stride;
				h = expf(reg[3U * (size_t)area + place]) * stride;
				candidates[candidate_count].x1 = cx - w / 2.0f;
				candidates[candidate_count].y1 = cy - h / 2.0f;
				candidates[candidate_count].x2 = cx + w / 2.0f;
				candidates[candidate_count].y2 = cy + h / 2.0f;
				candidates[candidate_count].score = score;
				candidates[candidate_count].class_index = best_class;
				candidates[candidate_count].anchor = anchor;
				candidate_count++;
			}
		}
	}

	/* The deterministic order. */
	qsort(candidates, candidate_count, sizeof(*candidates), decode_order);

	/* Greedy NMS: kept boxes move to the front in order. */
	kept = 0U;
	for (i = 0U; i < candidate_count; i++) {
		suppressed = 0U;
		for (j = 0U; j < kept; j++) {
			if (runner->class_aware != 0 && candidates[j].class_index != candidates[i].class_index)
				continue;
			overlap = decode_iou(&candidates[j], &candidates[i]);
			if (overlap > runner->nms_threshold) {
				suppressed = 1U;
				break;
			}
		}
		if (suppressed == 0U) {
			candidates[kept] = candidates[i];
			kept++;
		}
	}

	/* The kept boxes in the image's coordinates, as many as there is room for. */
	ratio = (float)box->ratio;
	for (i = 0U; i < kept && i < capacity; i++) {
		detections[i].x1 = candidates[i].x1 / ratio;
		detections[i].y1 = candidates[i].y1 / ratio;
		detections[i].x2 = candidates[i].x2 / ratio;
		detections[i].y2 = candidates[i].y2 / ratio;
		detections[i].score = candidates[i].score;
		detections[i].class_index = candidates[i].class_index;
	}

	/* Succeeded: the count after NMS. */
	*count = kept;
	return 0;
}

/* The logistic function. */
static float
decode_sigmoid(
	float value)
{
	/* 1 / (1 + exp(-x)). */
	return 1.0f / (1.0f + expf(-value));
}

/* The IoU of two boxes in float32. */
static float
decode_iou(
	const struct yolox_candidate *a,
	const struct yolox_candidate *b)
{
	float left;
	float top;
	float right;
	float bottom;
	float inter;
	float uni;

	/* The intersection. */
	left = fmaxf(a->x1, b->x1);
	top = fmaxf(a->y1, b->y1);
	right = fminf(a->x2, b->x2);
	bottom = fminf(a->y2, b->y2);
	inter = 0.0f;
	if (right > left && bottom > top)
		inter = (right - left) * (bottom - top);

	/* The union; an empty one overlaps nothing. */
	uni = (a->x2 - a->x1) * (a->y2 - a->y1) + (b->x2 - b->x1) * (b->y2 - b->y1) - inter;
	if (uni <= 0.0f)
		return 0.0f;

	/* Succeeded: the overlap. */
	return inter / uni;
}

/* Orders candidates by score, high first, then anchor and class, low first. */
static int
decode_order(
	const void *left,
	const void *right)
{
	const struct yolox_candidate *a;
	const struct yolox_candidate *b;

	/* The score first. */
	a = left;
	b = right;
	if (a->score > b->score)
		return -1;
	if (a->score < b->score)
		return 1;

	/* Then the anchor. */
	if (a->anchor < b->anchor)
		return -1;
	if (a->anchor > b->anchor)
		return 1;

	/* Then the class. */
	if (a->class_index < b->class_index)
		return -1;
	if (a->class_index > b->class_index)
		return 1;

	/* Succeeded: the same candidate. */
	return 0;
}
