/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The runner: its configuration, its making, synchronous detection and
 * the debug read of a tensor.
 *
 * The CPU backend is the one implemented here.  AUTO chooses it; a
 * Vulkan runner is refused with YOLOX_ERROR_UNSUPPORTED until the Vulkan
 * backend is present, and so are the asynchronous calls, which need it.
 */

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "yolox-internal.h"

/* The defaults of a configuration. */
#define RUNNER_SCORE_DEFAULT		0.3f
#define RUNNER_NMS_DEFAULT		0.3f
#define RUNNER_DISPATCH_MACS_DEFAULT	8000000U

/* The smallest configuration a caller may pass: everything up to the Vulkan fields. */
#define RUNNER_CONFIG_MIN		offsetof(struct yolox_runner_config, get_instance_proc_addr)

static int runner_size_valid(const struct yolox_model *model, uint32_t size);
static int runner_image_valid(const struct yolox_image *image, const void *pixels);

/*
 * Names an error code.
 */
const char *
yolox_reason(
	int code)
{
	/* Decides which code it is. */
	switch (code) {
	case 0:
		return "success";
	case YOLOX_ERROR_ARGUMENT:
		return "invalid argument";
	case YOLOX_ERROR_MEMORY:
		return "out of memory";
	case YOLOX_ERROR_IO:
		return "input/output error";
	case YOLOX_ERROR_FORMAT:
		return "malformed model file";
	case YOLOX_ERROR_UNSUPPORTED:
		return "not supported";
	case YOLOX_ERROR_BUSY:
		return "an execution is in flight";
	case YOLOX_ERROR_TIMEOUT:
		return "timed out";
	case YOLOX_ERROR_NOT_SHARED:
		return "the runner does not share the application's device";
	case YOLOX_ERROR_DEVICE_LOST:
		return "the Vulkan device was lost";
	case YOLOX_ERROR_FUSED:
		return "the tensor does not exist after fusion";
	case YOLOX_ERROR_VULKAN:
		return "Vulkan error";
	case YOLOX_ERROR_NOT_DEBUG:
		return "not a debug runner";
	case YOLOX_ERROR_IDLE:
		return "nothing was submitted";
	default:
		return "unknown error";
	}
}

/*
 * Fills a configuration with its size and the defaults.
 */
void
yolox_runner_config_default(
	struct yolox_runner_config *config)
{
	/* Nothing to fill. */
	if (config == NULL)
		return;

	/* The size, then the defaults; the Vulkan fields are empty. */
	memset(config, 0, sizeof(*config));
	config->struct_size = sizeof(*config);
	config->backend = YOLOX_BACKEND_AUTO;
	config->score_threshold = RUNNER_SCORE_DEFAULT;
	config->nms_threshold = RUNNER_NMS_DEFAULT;
	config->max_dispatch_macs = RUNNER_DISPATCH_MACS_DEFAULT;
}

/*
 * Makes a runner of a model.
 */
int
yolox_runner_create(
	const struct yolox_model *model,
	const struct yolox_runner_config *caller_config,
	struct yolox_runner **result)
{
	struct yolox_runner_config config;
	struct yolox_runner *runner;
	uint32_t index;
	uint32_t side;
	uint32_t level;
	size_t copy;

	/* Refuses missing arguments and a configuration older than the first. */
	if (model == NULL || caller_config == NULL || result == NULL)
		return YOLOX_ERROR_ARGUMENT;
	if (caller_config->struct_size < RUNNER_CONFIG_MIN)
		return YOLOX_ERROR_ARGUMENT;

	/* The caller's fields over the defaults, no more than either knows. */
	yolox_runner_config_default(&config);
	copy = caller_config->struct_size;
	if (copy > sizeof(config))
		copy = sizeof(config);
	memcpy(&config, caller_config, copy);
	config.struct_size = sizeof(config);

	/* The input size: the model's default, or a multiple of 32 from 64 to 1024. */
	if (config.input_size == 0U)
		config.input_size = model->default_size;
	if (runner_size_valid(model, config.input_size) == 0)
		return YOLOX_ERROR_ARGUMENT;

	/* The backend: the CPU for now. */
	if (config.backend == YOLOX_BACKEND_VULKAN)
		return YOLOX_ERROR_UNSUPPORTED;
	if (config.backend != YOLOX_BACKEND_AUTO && config.backend != YOLOX_BACKEND_CPU)
		return YOLOX_ERROR_ARGUMENT;

	/* The runner. */
	runner = calloc(1U, sizeof(*runner));
	if (runner == NULL)
		return YOLOX_ERROR_MEMORY;
	runner->model = model;
	runner->backend = YOLOX_BACKEND_CPU;
	runner->flags = config.flags;
	runner->size = config.input_size;
	runner->score_threshold = config.score_threshold;
	runner->nms_threshold = config.nms_threshold;
	runner->class_aware = config.class_aware;

	/* A buffer for every tensor. */
	runner->buffers = calloc(model->tensor_count, sizeof(*runner->buffers));
	runner->buffer_elements = calloc(model->tensor_count, sizeof(*runner->buffer_elements));
	if (runner->buffers == NULL || runner->buffer_elements == NULL) {
		yolox_runner_free(runner);
		return YOLOX_ERROR_MEMORY;
	}

	for (index = 0U; index < model->tensor_count; index++) {
		side = yolox_tensor_side(model, index, runner->size);
		runner->buffer_elements[index] = (size_t)model->tensors[index].channels * side * side;
		runner->buffers[index] = calloc(runner->buffer_elements[index], sizeof(float));
		if (runner->buffers[index] == NULL) {
			yolox_runner_free(runner);
			return YOLOX_ERROR_MEMORY;
		}
	}

	/* Room for a candidate at every anchor. */
	runner->anchor_count = 0U;
	for (level = 0U; level < YOLOX_LEVELS; level++) {
		side = runner->size / model->strides[level];
		runner->anchor_count += side * side;
	}

	runner->candidates = calloc(runner->anchor_count, sizeof(*runner->candidates));
	if (runner->candidates == NULL) {
		yolox_runner_free(runner);
		return YOLOX_ERROR_MEMORY;
	}

	/* Succeeded: the runner is ready. */
	*result = runner;
	return 0;
}

/*
 * Reports the backend a runner chose.
 */
enum yolox_backend
yolox_runner_backend(
	const struct yolox_runner *runner)
{
	/* The backend made. */
	return runner->backend;
}

/*
 * Frees a runner.
 */
void
yolox_runner_free(
	struct yolox_runner *runner)
{
	uint32_t index;

	/* Nothing to free. */
	if (runner == NULL)
		return;

	/* The tensors, the candidates and the record. */
	if (runner->buffers != NULL) {
		for (index = 0U; index < runner->model->tensor_count; index++)
			free(runner->buffers[index]);
	}
	free(runner->buffers);
	free(runner->buffer_elements);
	free(runner->candidates);
	free(runner);
}

/*
 * Detects objects in a host image.
 */
int
yolox_detect(
	struct yolox_runner *runner,
	const struct yolox_image *image,
	const void *pixels,
	struct yolox_detection *detections,
	uint32_t capacity,
	uint32_t *count)
{
	struct yolox_letterbox box;
	int error;

	/* Refuses missing arguments and a malformed image. */
	if (runner == NULL || count == NULL)
		return YOLOX_ERROR_ARGUMENT;
	if (capacity != 0U && detections == NULL)
		return YOLOX_ERROR_ARGUMENT;
	error = runner_image_valid(image, pixels);
	if (error != 0)
		return error;

	/* The letterbox and the input. */
	error = yolox_letterbox_compute(image->width, image->height, runner->size, &box);
	if (error != 0)
		return error;

	error = yolox_preprocess(image, pixels, runner->size, runner->model->pad_value, &box,
	    runner->buffers[runner->model->input_tensor]);
	if (error != 0)
		return error;

	/* The network, then the decoding. */
	yolox_cpu_forward(runner);
	runner->executed = 1U;
	error = yolox_decode(runner, &box, detections, capacity, count);
	if (error != 0)
		return error;

	/* Succeeded: the detections are written. */
	return 0;
}

/*
 * Submits an image in a device buffer; this needs the Vulkan backend.
 */
int
yolox_submit_buffer(
	struct yolox_runner *runner,
	const struct yolox_image *image,
	VkBuffer buffer,
	VkDeviceSize offset,
	VkSemaphore wait)
{
	(void)image;
	(void)buffer;
	(void)offset;
	(void)wait;

	/* Refuses a missing runner. */
	if (runner == NULL)
		return YOLOX_ERROR_ARGUMENT;

	/* A CPU runner shares no device. */
	return YOLOX_ERROR_NOT_SHARED;
}

/*
 * Collects a submitted image's detections; nothing is ever submitted to
 * a CPU runner.
 */
int
yolox_collect(
	struct yolox_runner *runner,
	uint64_t timeout_ns,
	struct yolox_detection *detections,
	uint32_t capacity,
	uint32_t *count)
{
	(void)timeout_ns;
	(void)detections;
	(void)capacity;
	(void)count;

	/* Refuses a missing runner. */
	if (runner == NULL)
		return YOLOX_ERROR_ARGUMENT;

	/* Nothing is in flight. */
	return YOLOX_ERROR_IDLE;
}

/*
 * Copies a tensor of the last execution as NCHW float32.
 */
int
yolox_runner_read_tensor(
	struct yolox_runner *runner,
	uint32_t tensor,
	float *values,
	size_t count)
{
	/* Refuses missing arguments and an unknown tensor. */
	if (runner == NULL || values == NULL)
		return YOLOX_ERROR_ARGUMENT;
	if (tensor >= runner->model->tensor_count)
		return YOLOX_ERROR_ARGUMENT;

	/* Only a debug runner, after an execution, with the tensor's exact size. */
	if ((runner->flags & YOLOX_RUNNER_DEBUG) == 0U)
		return YOLOX_ERROR_NOT_DEBUG;
	if (runner->executed == 0U)
		return YOLOX_ERROR_IDLE;
	if (count != runner->buffer_elements[tensor])
		return YOLOX_ERROR_ARGUMENT;

	/* Succeeded: the tensor is copied. */
	memcpy(values, runner->buffers[tensor], count * sizeof(float));
	return 0;
}

/* Reports whether an input size is one the model takes. */
static int
runner_size_valid(
	const struct yolox_model *model,
	uint32_t size)
{
	/* From 64 to 1024, a multiple of the model's. */
	if (size < YOLOX_SIZE_MIN || size > YOLOX_SIZE_MAX)
		return 0;
	if (size % model->size_multiple != 0U)
		return 0;

	/* Succeeded: the size is taken. */
	return 1;
}

/* Checks an image's layout, size and row length. */
static int
runner_image_valid(
	const struct yolox_image *image,
	const void *pixels)
{
	uint64_t pixel_bytes;

	/* Refuses missing arguments and an empty or oversized image. */
	if (image == NULL || pixels == NULL)
		return YOLOX_ERROR_ARGUMENT;
	if (image->width == 0U || image->height == 0U)
		return YOLOX_ERROR_ARGUMENT;
	if (image->width > YOLOX_IMAGE_MAX || image->height > YOLOX_IMAGE_MAX)
		return YOLOX_ERROR_ARGUMENT;

	/* The pixel's size by its layout. */
	if (image->format == YOLOX_PIXEL_RGB8) {
		pixel_bytes = 3U;
	} else if (image->format == YOLOX_PIXEL_RGBA8 || image->format == YOLOX_PIXEL_BGRA8) {
		pixel_bytes = 4U;
	} else {
		return YOLOX_ERROR_ARGUMENT;
	}

	/* A row holds the whole width. */
	if ((uint64_t)image->row_bytes < pixel_bytes * image->width)
		return YOLOX_ERROR_ARGUMENT;

	/* Succeeded: the image can be read. */
	return 0;
}
