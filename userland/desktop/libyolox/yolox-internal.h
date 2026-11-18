/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The parts of libyolox its files share: the model as read, the runner,
 * and the functions between the loader, the preprocessing, the CPU
 * backend and the decoding.
 */

#ifndef LIBYOLOX_YOLOX_INTERNAL_H
#define LIBYOLOX_YOLOX_INTERNAL_H

#include <stddef.h>
#include <stdint.h>

#include "yolox.h"

/* The op kinds of a ZNET file. */
#define YOLOX_OP_SPACE_TO_DEPTH		1U
#define YOLOX_OP_CONV			2U
#define YOLOX_OP_MAXPOOL		3U
#define YOLOX_OP_UPSAMPLE		4U
#define YOLOX_OP_ADD			5U
#define YOLOX_OP_CONCAT			6U

/* The activations. */
#define YOLOX_ACT_NONE			0U
#define YOLOX_ACT_SILU			1U

/* The tensor flags. */
#define YOLOX_TENSOR_INPUT		0x1U
#define YOLOX_TENSOR_HEAD_OUTPUT	0x2U

/* The detection levels and the classes of the model. */
#define YOLOX_LEVELS			3U
#define YOLOX_CLASSES			80U

/* The limits on an input image's side. */
#define YOLOX_IMAGE_MAX			16384U

/* The input sizes a runner takes. */
#define YOLOX_SIZE_MIN			64U
#define YOLOX_SIZE_MAX			1024U

/* One tensor of the graph: its channels, its spatial divisor and its flags. */
struct yolox_tensor_info {
	uint32_t channels;
	uint32_t divisor;
	uint32_t flags;
};

/*
 * One op of the graph, checked at load.
 *
 * weights and bias point into the model's float32 values; cin is the
 * channel count of the first input.
 */
struct yolox_op {
	uint32_t kind;
	uint32_t activation;
	uint32_t kernel;
	uint32_t stride;
	uint32_t groups;
	uint32_t input_count;
	uint32_t inputs[4];
	uint32_t output;
	const float *weights;
	uint32_t weight_count;
	const float *bias;
	uint32_t bias_count;
	const char *name;
	uint32_t cin;
};

/*
 * A read ZNET model.
 *
 * values holds every weight and bias as float32 (an fp16 file is widened
 * at load); names holds the op names.  level_tensors[l] is (reg, obj,
 * cls) of level l.
 */
struct yolox_model {
	uint32_t tensor_count;
	uint32_t op_count;
	struct yolox_tensor_info *tensors;
	struct yolox_op *ops;
	float *values;
	char *names;
	uint32_t input_tensor;
	uint32_t default_size;
	uint32_t size_multiple;
	uint32_t pad_value;
	uint32_t strides[YOLOX_LEVELS];
	uint32_t level_tensors[YOLOX_LEVELS][3];
	uint8_t source_sha256[32];
	uint8_t data_sha256[32];
};

/* One candidate box before NMS, in the network's coordinates. */
struct yolox_candidate {
	float x1;
	float y1;
	float x2;
	float y2;
	float score;
	uint32_t class_index;
	uint32_t anchor;
};

/*
 * The geometry of one image's preprocessing.
 *
 * ratio is min(S / height, S / width) in binary64; scaled_width and
 * scaled_height are the resized image's size inside the S x S input.
 */
struct yolox_letterbox {
	double ratio;
	uint32_t scaled_width;
	uint32_t scaled_height;
};

/*
 * A runner.
 *
 * buffers[t] holds tensor t as NCHW float32 for the CPU backend; every
 * tensor is kept, so a debug runner and an ordinary one differ only in
 * what yolox_runner_read_tensor() allows.  candidates has room for every
 * anchor of the input size.
 */
struct yolox_runner {
	const struct yolox_model *model;
	enum yolox_backend backend;
	uint32_t flags;
	uint32_t size;
	float score_threshold;
	float nms_threshold;
	int class_aware;
	float **buffers;
	size_t *buffer_elements;
	struct yolox_candidate *candidates;
	uint32_t anchor_count;
	unsigned executed;
};

/* Reports how many pixels a side of a tensor has at an input size. */
uint32_t yolox_tensor_side(const struct yolox_model *model, uint32_t tensor, uint32_t size);

/* Computes the letterbox of an image at an input size; YOLOX_ERROR_ARGUMENT when it is empty. */
int yolox_letterbox_compute(uint32_t width, uint32_t height, uint32_t size, struct yolox_letterbox *box);

/* Resizes and pads an image into the 3 x S x S RGB float input. */
int yolox_preprocess(const struct yolox_image *image, const void *pixels, uint32_t size, uint32_t pad_value, const struct yolox_letterbox *box, float *input);

/* Runs the model's ops in order on the CPU. */
void yolox_cpu_forward(struct yolox_runner *runner);

/*
 * Decodes the head tensors into candidates above the threshold, runs NMS
 * and writes the kept boxes in the image's coordinates.
 */
int yolox_decode(struct yolox_runner *runner, const struct yolox_letterbox *box, struct yolox_detection *detections, uint32_t capacity, uint32_t *count);

#endif
