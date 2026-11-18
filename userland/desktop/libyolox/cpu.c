/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The CPU reference backend.
 *
 * Every ZNET op runs as written, unfused, on NCHW float32 tensors: a
 * general convolution (any kernel, stride and grouping, zero padding of
 * (k - 1) / 2), max pooling over the window clipped to the image, nearest
 * upsampling by two, addition, channel concatenation and the Focus
 * space-to-depth.  It is the standard the GPU backend is checked against,
 * so it is written plainly and independently of the GPU's layout.
 *
 * SiLU is x / (1 + exp(-x)); a large negative x gives an infinite exp and
 * a result of -0, never NaN.
 */

#include <math.h>
#include <string.h>

#include "yolox-internal.h"

static void cpu_conv(const struct yolox_op *op, const float *in, uint32_t in_side, float *out, uint32_t out_side, uint32_t cout);
static void cpu_maxpool(uint32_t kernel, const float *in, uint32_t channels, uint32_t side, float *out);
static void cpu_upsample(const float *in, uint32_t channels, uint32_t side, float *out);
static void cpu_space_to_depth(const float *in, uint32_t channels, uint32_t side, float *out);
static float cpu_silu(float value);

/*
 * Runs the model's ops in order on the CPU.  The input tensor is filled
 * already.
 */
void
yolox_cpu_forward(
	struct yolox_runner *runner)
{
	const struct yolox_model *model;
	const struct yolox_op *op;
	uint32_t in_side;
	uint32_t out_side;
	uint32_t channels;
	uint32_t index;
	uint32_t input;
	size_t elements;
	size_t place;
	size_t count;
	float *out;
	const float *a;
	const float *b;

	/* Each op in the file's order. */
	model = runner->model;
	for (index = 0U; index < model->op_count; index++) {
		op = &model->ops[index];
		in_side = yolox_tensor_side(model, op->inputs[0], runner->size);
		out_side = yolox_tensor_side(model, op->output, runner->size);
		out = runner->buffers[op->output];
		channels = model->tensors[op->output].channels;

		/* Decides what the op does. */
		switch (op->kind) {
		case YOLOX_OP_CONV:
			cpu_conv(op, runner->buffers[op->inputs[0]], in_side, out, out_side, channels);
			break;
		case YOLOX_OP_MAXPOOL:
			cpu_maxpool(op->kernel, runner->buffers[op->inputs[0]], channels, in_side, out);
			break;
		case YOLOX_OP_UPSAMPLE:
			cpu_upsample(runner->buffers[op->inputs[0]], channels, in_side, out);
			break;
		case YOLOX_OP_SPACE_TO_DEPTH:
			cpu_space_to_depth(runner->buffers[op->inputs[0]], op->cin, in_side, out);
			break;
		case YOLOX_OP_ADD:
			/* The element-wise sum of two tensors of one shape. */
			a = runner->buffers[op->inputs[0]];
			b = runner->buffers[op->inputs[1]];
			elements = runner->buffer_elements[op->output];
			for (place = 0U; place < elements; place++)
				out[place] = a[place] + b[place];
			break;
		case YOLOX_OP_CONCAT:
			/* The inputs one after another along the channels. */
			place = 0U;
			for (input = 0U; input < op->input_count; input++) {
				count = runner->buffer_elements[op->inputs[input]];
				memcpy(out + place, runner->buffers[op->inputs[input]], count * sizeof(float));
				place += count;
			}
			break;
		default:
			break;
		}
	}
}

/*
 * One general convolution with its bias and optional SiLU.  Each output
 * channel o of group g reads the group's input channels; samples outside
 * the input are zero.
 */
static void
cpu_conv(
	const struct yolox_op *op,
	const float *in,
	uint32_t in_side,
	float *out,
	uint32_t out_side,
	uint32_t cout)
{
	const float *weights;
	uint32_t per_group_in;
	uint32_t per_group_out;
	uint32_t group;
	uint32_t o;
	uint32_t i;
	uint32_t y;
	uint32_t x;
	uint32_t ky;
	uint32_t kx;
	int32_t pad;
	int32_t sy;
	int32_t sx;
	float sum;

	/* The grouping and the padding. */
	per_group_in = op->cin / op->groups;
	per_group_out = cout / op->groups;
	pad = (int32_t)(op->kernel - 1U) / 2;

	/* Each output sample. */
	for (o = 0U; o < cout; o++) {
		group = o / per_group_out;
		for (y = 0U; y < out_side; y++) {
			for (x = 0U; x < out_side; x++) {
				/* The bias, then every tap of the group's inputs. */
				sum = op->bias[o];
				for (i = 0U; i < per_group_in; i++) {
					weights = op->weights + ((size_t)o * per_group_in + i) * op->kernel * op->kernel;
					for (ky = 0U; ky < op->kernel; ky++) {
						sy = (int32_t)(y * op->stride + ky) - pad;
						if (sy < 0 || sy >= (int32_t)in_side)
							continue;
						for (kx = 0U; kx < op->kernel; kx++) {
							sx = (int32_t)(x * op->stride + kx) - pad;
							if (sx < 0 || sx >= (int32_t)in_side)
								continue;
							sum += weights[ky * op->kernel + kx] *
							    in[((size_t)(group * per_group_in + i) * in_side + (uint32_t)sy) * in_side + (uint32_t)sx];
						}
					}
				}

				/* The activation. */
				if (op->activation == YOLOX_ACT_SILU)
					sum = cpu_silu(sum);
				out[((size_t)o * out_side + y) * out_side + x] = sum;
			}
		}
	}
}

/* Max pooling with stride 1 over the window clipped to the image. */
static void
cpu_maxpool(
	uint32_t kernel,
	const float *in,
	uint32_t channels,
	uint32_t side,
	float *out)
{
	const float *plane;
	uint32_t c;
	uint32_t y;
	uint32_t x;
	int32_t half;
	int32_t dy;
	int32_t dx;
	int32_t sy;
	int32_t sx;
	float best;
	float value;

	/* Each output sample: the largest input of its window inside the image. */
	half = (int32_t)kernel / 2;
	for (c = 0U; c < channels; c++) {
		plane = in + (size_t)c * side * side;
		for (y = 0U; y < side; y++) {
			for (x = 0U; x < side; x++) {
				best = plane[(size_t)y * side + x];
				for (dy = -half; dy <= half; dy++) {
					sy = (int32_t)y + dy;
					if (sy < 0 || sy >= (int32_t)side)
						continue;
					for (dx = -half; dx <= half; dx++) {
						sx = (int32_t)x + dx;
						if (sx < 0 || sx >= (int32_t)side)
							continue;
						value = plane[(size_t)sy * side + (uint32_t)sx];
						if (value > best)
							best = value;
					}
				}
				out[((size_t)c * side + y) * side + x] = best;
			}
		}
	}
}

/* Nearest upsampling by two: out[y][x] = in[y / 2][x / 2]. */
static void
cpu_upsample(
	const float *in,
	uint32_t channels,
	uint32_t side,
	float *out)
{
	uint32_t c;
	uint32_t y;
	uint32_t x;
	uint32_t out_side;

	/* Each output sample. */
	out_side = side * 2U;
	for (c = 0U; c < channels; c++) {
		for (y = 0U; y < out_side; y++) {
			for (x = 0U; x < out_side; x++)
				out[((size_t)c * out_side + y) * out_side + x] = in[((size_t)c * side + (y >> 1)) * side + (x >> 1)];
		}
	}
}

/*
 * Focus: each 2 x 2 patch's samples become channels, the patches in the
 * order top-left, bottom-left, top-right, bottom-right; output channel
 * patch * C + c.
 */
static void
cpu_space_to_depth(
	const float *in,
	uint32_t channels,
	uint32_t side,
	float *out)
{
	uint32_t out_side;
	uint32_t patch;
	uint32_t c;
	uint32_t y;
	uint32_t x;
	uint32_t row;
	uint32_t column;

	/* Each patch position and channel. */
	out_side = side / 2U;
	for (patch = 0U; patch < 4U; patch++) {
		row = patch & 1U;
		column = patch >> 1;
		for (c = 0U; c < channels; c++) {
			for (y = 0U; y < out_side; y++) {
				for (x = 0U; x < out_side; x++) {
					out[((size_t)(patch * channels + c) * out_side + y) * out_side + x] =
					    in[((size_t)c * side + 2U * y + row) * side + 2U * x + column];
				}
			}
		}
	}
}

/* SiLU: x / (1 + exp(-x)). */
static float
cpu_silu(
	float value)
{
	/* An infinite exp gives -0, not NaN. */
	return value / (1.0f + expf(-value));
}
