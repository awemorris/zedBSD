/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The ZNET loader.
 *
 * A ZNET file comes from outside, so every field is checked before it is
 * used: the header, every table and section inside the file, every op's
 * kind, shape and weight counts, and the graph (each tensor made once,
 * every input made before it is read, the shapes of each op consistent,
 * the head's tensors where the header says).  Any violation is
 * YOLOX_ERROR_FORMAT.  Offsets plus sizes are computed in 64 bits.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "yolox-internal.h"

/* The header's fixed values. */
#define ZNET_HEADER_SIZE		256U
#define ZNET_VERSION			1U
#define ZNET_FLAG_FP16			0x1U
#define ZNET_SECTION_ALIGN		64U
#define ZNET_TENSOR_ENTRY		16U
#define ZNET_OP_ENTRY			64U
#define ZNET_FILE_MAX			(64U * 1024U * 1024U)

/* The limits on a graph's contents. */
#define ZNET_OPS_MAX			1024U
#define ZNET_TENSORS_MAX		1024U
#define ZNET_CHANNELS_MAX		4096U
#define ZNET_DIVISOR_MAX		32U

/* The header's field offsets (design section 2.3). */
#define ZNET_AT_VERSION			4U
#define ZNET_AT_HEADER_SIZE		8U
#define ZNET_AT_FLAGS			12U
#define ZNET_AT_TENSOR_COUNT		16U
#define ZNET_AT_OP_COUNT		20U
#define ZNET_AT_TENSOR_OFFSET		24U
#define ZNET_AT_OP_OFFSET		28U
#define ZNET_AT_DATA_OFFSET		32U
#define ZNET_AT_DATA_SIZE		36U
#define ZNET_AT_NAME_OFFSET		40U
#define ZNET_AT_NAME_SIZE		44U
#define ZNET_AT_INPUT			48U
#define ZNET_AT_COLOR			52U
#define ZNET_AT_DEFAULT_SIZE		56U
#define ZNET_AT_MULTIPLE		60U
#define ZNET_AT_PAD			64U
#define ZNET_AT_CLASSES			68U
#define ZNET_AT_LEVELS			72U
#define ZNET_AT_STRIDES			76U
#define ZNET_AT_LEVEL_TENSORS		92U
#define ZNET_AT_RESERVED		140U
#define ZNET_AT_SOURCE_SHA		160U
#define ZNET_AT_DATA_SHA		192U

/* The file read into memory and the counts the checks share. */
struct znet_file {
	const uint8_t *bytes;
	uint64_t size;
	uint32_t flags;
	uint32_t element_size;
	uint64_t data_offset;
	uint64_t data_size;
	uint64_t name_offset;
	uint64_t name_size;
};

static int znet_read_file(const char *path, uint8_t **bytes, uint64_t *size);
static uint32_t znet_u32(const uint8_t *bytes, uint64_t offset);
static int znet_inside(const struct znet_file *file, uint64_t offset, uint64_t size);
static int znet_check_header(struct znet_file *file, struct yolox_model *model);
static int znet_read_tensors(const struct znet_file *file, struct yolox_model *model);
static int znet_read_ops(const struct znet_file *file, struct yolox_model *model);
static int znet_read_values(const struct znet_file *file, struct yolox_model *model, const uint8_t *entry, struct yolox_op *op, uint32_t *next_value);
static int znet_check_op(const struct yolox_model *model, const struct yolox_op *op);
static int znet_check_graph(const struct yolox_model *model);
static float znet_half_to_float(uint16_t half);
static int znet_power_of_two(uint32_t value);

/*
 * Reads and checks a ZNET file.
 */
int
yolox_model_load(
	const char *path,
	struct yolox_model **result)
{
	struct znet_file file;
	struct yolox_model *model;
	uint8_t *bytes;
	uint64_t size;
	int error;

	/* Refuses missing arguments. */
	if (path == NULL || result == NULL)
		return YOLOX_ERROR_ARGUMENT;

	/* The file, whole. */
	bytes = NULL;
	size = 0U;
	error = znet_read_file(path, &bytes, &size);
	if (error != 0)
		return error;

	/* The model's record. */
	model = calloc(1U, sizeof(*model));
	if (model == NULL) {
		free(bytes);
		return YOLOX_ERROR_MEMORY;
	}

	/* The header, then the tensors, the ops and the graph. */
	memset(&file, 0, sizeof(file));
	file.bytes = bytes;
	file.size = size;
	error = znet_check_header(&file, model);
	if (error == 0)
		error = znet_read_tensors(&file, model);
	if (error == 0)
		error = znet_read_ops(&file, model);
	if (error == 0)
		error = znet_check_graph(model);
	free(bytes);
	if (error != 0) {
		yolox_model_free(model);
		return error;
	}

	/* Succeeded: the model is checked. */
	*result = model;
	return 0;
}

/*
 * Frees a model.
 */
void
yolox_model_free(
	struct yolox_model *model)
{
	/* Nothing to free. */
	if (model == NULL)
		return;

	/* The tables, the values and the names. */
	free(model->tensors);
	free(model->ops);
	free(model->values);
	free(model->names);
	free(model);
}

/*
 * Reports how many pixels a side of a tensor has at an input size.
 */
uint32_t
yolox_tensor_side(
	const struct yolox_model *model,
	uint32_t tensor,
	uint32_t size)
{
	/* The input size over the tensor's divisor. */
	return size / model->tensors[tensor].divisor;
}

/* Reads a whole file of at most 64 MiB. */
static int
znet_read_file(
	const char *path,
	uint8_t **bytes,
	uint64_t *size)
{
	FILE *stream;
	uint8_t *buffer;
	long length;
	size_t got;
	int status;

	/* The file. */
	stream = fopen(path, "rb");
	if (stream == NULL)
		return YOLOX_ERROR_IO;

	/* Its length, from its end. */
	status = fseek(stream, 0L, SEEK_END);
	length = -1L;
	if (status == 0)
		length = ftell(stream);
	if (length < (long)ZNET_HEADER_SIZE || (unsigned long)length > ZNET_FILE_MAX) {
		fclose(stream);
		return YOLOX_ERROR_FORMAT;
	}

	/* Back to its start. */
	status = fseek(stream, 0L, SEEK_SET);
	if (status != 0) {
		fclose(stream);
		return YOLOX_ERROR_IO;
	}

	/* The bytes. */
	buffer = malloc((size_t)length);
	if (buffer == NULL) {
		fclose(stream);
		return YOLOX_ERROR_MEMORY;
	}

	got = fread(buffer, 1U, (size_t)length, stream);
	fclose(stream);
	if (got != (size_t)length) {
		free(buffer);
		return YOLOX_ERROR_IO;
	}

	/* Succeeded: the caller owns the bytes. */
	*bytes = buffer;
	*size = (uint64_t)length;
	return 0;
}

/* Reads a little-endian 32-bit field. */
static uint32_t
znet_u32(
	const uint8_t *bytes,
	uint64_t offset)
{
	uint32_t value;

	/* The lowest byte comes first. */
	value = (uint32_t)bytes[offset];
	value |= (uint32_t)bytes[offset + 1U] << 8;
	value |= (uint32_t)bytes[offset + 2U] << 16;
	value |= (uint32_t)bytes[offset + 3U] << 24;

	/* Succeeded: the field. */
	return value;
}

/* Reports whether a range lies inside the file. */
static int
znet_inside(
	const struct znet_file *file,
	uint64_t offset,
	uint64_t size)
{
	/* The range's end, in 64 bits, must not pass the file's. */
	if (offset > file->size)
		return 0;
	if (size > file->size - offset)
		return 0;

	/* Succeeded: inside. */
	return 1;
}

/* Checks the header and copies its fields into the model. */
static int
znet_check_header(
	struct znet_file *file,
	struct yolox_model *model)
{
	const uint8_t *bytes;
	uint64_t offset;
	uint64_t size;
	uint32_t level;
	uint32_t index;
	uint32_t value;

	/* The magic, the version and the header's size. */
	bytes = file->bytes;
	if (memcmp(bytes, "ZNET", 4U) != 0)
		return YOLOX_ERROR_FORMAT;
	if (znet_u32(bytes, ZNET_AT_VERSION) != ZNET_VERSION)
		return YOLOX_ERROR_FORMAT;
	if (znet_u32(bytes, ZNET_AT_HEADER_SIZE) != ZNET_HEADER_SIZE)
		return YOLOX_ERROR_FORMAT;

	/* The flags: only fp16 is defined. */
	file->flags = znet_u32(bytes, ZNET_AT_FLAGS);
	if ((file->flags & ~ZNET_FLAG_FP16) != 0U)
		return YOLOX_ERROR_FORMAT;
	file->element_size = 4U;
	if ((file->flags & ZNET_FLAG_FP16) != 0U)
		file->element_size = 2U;

	/* The reserved bytes are zero. */
	for (index = ZNET_AT_RESERVED; index < ZNET_AT_SOURCE_SHA; index++) {
		if (bytes[index] != 0U)
			return YOLOX_ERROR_FORMAT;
	}

	/* The counts. */
	model->tensor_count = znet_u32(bytes, ZNET_AT_TENSOR_COUNT);
	model->op_count = znet_u32(bytes, ZNET_AT_OP_COUNT);
	if (model->tensor_count == 0U || model->tensor_count > ZNET_TENSORS_MAX)
		return YOLOX_ERROR_FORMAT;
	if (model->op_count == 0U || model->op_count > ZNET_OPS_MAX)
		return YOLOX_ERROR_FORMAT;

	/* The tensor table, inside the file and aligned. */
	offset = znet_u32(bytes, ZNET_AT_TENSOR_OFFSET);
	size = (uint64_t)model->tensor_count * ZNET_TENSOR_ENTRY;
	if (offset % ZNET_SECTION_ALIGN != 0U || znet_inside(file, offset, size) == 0)
		return YOLOX_ERROR_FORMAT;

	/* The op table. */
	offset = znet_u32(bytes, ZNET_AT_OP_OFFSET);
	size = (uint64_t)model->op_count * ZNET_OP_ENTRY;
	if (offset % ZNET_SECTION_ALIGN != 0U || znet_inside(file, offset, size) == 0)
		return YOLOX_ERROR_FORMAT;

	/* The data. */
	file->data_offset = znet_u32(bytes, ZNET_AT_DATA_OFFSET);
	file->data_size = znet_u32(bytes, ZNET_AT_DATA_SIZE);
	if (file->data_offset % ZNET_SECTION_ALIGN != 0U || znet_inside(file, file->data_offset, file->data_size) == 0)
		return YOLOX_ERROR_FORMAT;

	/* The names. */
	file->name_offset = znet_u32(bytes, ZNET_AT_NAME_OFFSET);
	file->name_size = znet_u32(bytes, ZNET_AT_NAME_SIZE);
	if (file->name_offset % ZNET_SECTION_ALIGN != 0U || znet_inside(file, file->name_offset, file->name_size) == 0)
		return YOLOX_ERROR_FORMAT;
	if (file->name_size == 0U || bytes[file->name_offset + file->name_size - 1U] != 0U)
		return YOLOX_ERROR_FORMAT;

	/* The input: an RGB tensor of the table. */
	model->input_tensor = znet_u32(bytes, ZNET_AT_INPUT);
	if (model->input_tensor >= model->tensor_count)
		return YOLOX_ERROR_FORMAT;
	if (znet_u32(bytes, ZNET_AT_COLOR) != 0U)
		return YOLOX_ERROR_FORMAT;

	/* The input sizes. */
	model->size_multiple = znet_u32(bytes, ZNET_AT_MULTIPLE);
	model->default_size = znet_u32(bytes, ZNET_AT_DEFAULT_SIZE);
	if (model->size_multiple != ZNET_DIVISOR_MAX)
		return YOLOX_ERROR_FORMAT;
	if (model->default_size < YOLOX_SIZE_MIN || model->default_size > YOLOX_SIZE_MAX)
		return YOLOX_ERROR_FORMAT;
	if (model->default_size % model->size_multiple != 0U)
		return YOLOX_ERROR_FORMAT;
	model->pad_value = znet_u32(bytes, ZNET_AT_PAD);
	if (model->pad_value > 255U)
		return YOLOX_ERROR_FORMAT;

	/* The classes and the levels. */
	if (znet_u32(bytes, ZNET_AT_CLASSES) != YOLOX_CLASSES)
		return YOLOX_ERROR_FORMAT;
	if (znet_u32(bytes, ZNET_AT_LEVELS) != YOLOX_LEVELS)
		return YOLOX_ERROR_FORMAT;

	/* The strides 8, 16 and 32, and an unused fourth. */
	for (level = 0U; level < YOLOX_LEVELS; level++) {
		value = znet_u32(bytes, ZNET_AT_STRIDES + 4U * level);
		if (value != (8U << level))
			return YOLOX_ERROR_FORMAT;
		model->strides[level] = value;
	}

	if (znet_u32(bytes, ZNET_AT_STRIDES + 12U) != 0U)
		return YOLOX_ERROR_FORMAT;

	/* The head's tensors of each level. */
	for (level = 0U; level < YOLOX_LEVELS; level++) {
		for (index = 0U; index < 3U; index++) {
			value = znet_u32(bytes, ZNET_AT_LEVEL_TENSORS + 12U * level + 4U * index);
			if (value >= model->tensor_count)
				return YOLOX_ERROR_FORMAT;
			model->level_tensors[level][index] = value;
		}
	}

	/* The digests, kept for the record. */
	memcpy(model->source_sha256, bytes + ZNET_AT_SOURCE_SHA, sizeof(model->source_sha256));
	memcpy(model->data_sha256, bytes + ZNET_AT_DATA_SHA, sizeof(model->data_sha256));

	/* The names, copied. */
	model->names = malloc((size_t)file->name_size);
	if (model->names == NULL)
		return YOLOX_ERROR_MEMORY;
	memcpy(model->names, bytes + file->name_offset, (size_t)file->name_size);

	/* Succeeded: the header is checked. */
	return 0;
}

/* Reads the tensor table. */
static int
znet_read_tensors(
	const struct znet_file *file,
	struct yolox_model *model)
{
	struct yolox_tensor_info *tensor;
	uint64_t entry;
	uint32_t index;

	/* The table. */
	model->tensors = calloc(model->tensor_count, sizeof(*model->tensors));
	if (model->tensors == NULL)
		return YOLOX_ERROR_MEMORY;

	/* Each entry: channels, a power-of-two divisor, known flags and a zero word. */
	for (index = 0U; index < model->tensor_count; index++) {
		entry = znet_u32(file->bytes, ZNET_AT_TENSOR_OFFSET) + (uint64_t)index * ZNET_TENSOR_ENTRY;
		tensor = &model->tensors[index];
		tensor->channels = znet_u32(file->bytes, entry);
		tensor->divisor = znet_u32(file->bytes, entry + 4U);
		tensor->flags = znet_u32(file->bytes, entry + 8U);
		if (tensor->channels == 0U || tensor->channels > ZNET_CHANNELS_MAX)
			return YOLOX_ERROR_FORMAT;
		if (tensor->divisor == 0U || tensor->divisor > ZNET_DIVISOR_MAX || znet_power_of_two(tensor->divisor) == 0)
			return YOLOX_ERROR_FORMAT;
		if ((tensor->flags & ~(YOLOX_TENSOR_INPUT | YOLOX_TENSOR_HEAD_OUTPUT)) != 0U)
			return YOLOX_ERROR_FORMAT;
		if (znet_u32(file->bytes, entry + 12U) != 0U)
			return YOLOX_ERROR_FORMAT;
	}

	/* The input tensor is flagged, three channels at full size. */
	tensor = &model->tensors[model->input_tensor];
	if ((tensor->flags & YOLOX_TENSOR_INPUT) == 0U || tensor->channels != 3U || tensor->divisor != 1U)
		return YOLOX_ERROR_FORMAT;

	/* Succeeded: the tensors are read. */
	return 0;
}

/* Reads the op table and the ops' weights. */
static int
znet_read_ops(
	const struct znet_file *file,
	struct yolox_model *model)
{
	const uint8_t *entry;
	struct yolox_op *op;
	uint64_t total;
	uint32_t next_value;
	uint32_t index;
	uint32_t input;
	uint32_t name;
	int error;

	/* The op table. */
	model->ops = calloc(model->op_count, sizeof(*model->ops));
	if (model->ops == NULL)
		return YOLOX_ERROR_MEMORY;

	/* Room for every value of the data section as float32. */
	total = file->data_size / file->element_size;
	model->values = malloc((size_t)(total + 1U) * sizeof(float));
	if (model->values == NULL)
		return YOLOX_ERROR_MEMORY;

	/* Each op: its fields, its name and its values. */
	next_value = 0U;
	for (index = 0U; index < model->op_count; index++) {
		entry = file->bytes + znet_u32(file->bytes, ZNET_AT_OP_OFFSET) + (uint64_t)index * ZNET_OP_ENTRY;
		op = &model->ops[index];
		op->kind = znet_u32(entry, 0U);
		op->activation = znet_u32(entry, 4U);
		op->kernel = znet_u32(entry, 8U);
		op->stride = znet_u32(entry, 12U);
		op->groups = znet_u32(entry, 16U);
		op->input_count = znet_u32(entry, 20U);
		if (op->input_count == 0U || op->input_count > 4U)
			return YOLOX_ERROR_FORMAT;

		/* The inputs and the output name tensors of the table. */
		for (input = 0U; input < 4U; input++) {
			op->inputs[input] = znet_u32(entry, 24U + 4U * input);
			if (input < op->input_count && op->inputs[input] >= model->tensor_count)
				return YOLOX_ERROR_FORMAT;
		}

		op->output = znet_u32(entry, 40U);
		if (op->output >= model->tensor_count)
			return YOLOX_ERROR_FORMAT;
		op->cin = model->tensors[op->inputs[0]].channels;

		/* The name, a string inside the name table. */
		name = znet_u32(entry, 60U);
		if (name >= file->name_size)
			return YOLOX_ERROR_FORMAT;
		op->name = model->names + name;

		/* The weights and the bias. */
		error = znet_read_values(file, model, entry, op, &next_value);
		if (error != 0)
			return error;

		/* The op's own rules. */
		error = znet_check_op(model, op);
		if (error != 0)
			return error;
	}

	/* Succeeded: the ops are read. */
	return 0;
}

/*
 * Reads one op's weights and bias from the data section into the model's
 * float32 values.  Their offsets must lie inside the data section on the
 * element's boundary.
 */
static int
znet_read_values(
	const struct znet_file *file,
	struct yolox_model *model,
	const uint8_t *entry,
	struct yolox_op *op,
	uint32_t *next_value)
{
	const uint8_t *source;
	float *target;
	uint64_t offset;
	uint64_t count;
	uint64_t bytes;
	uint32_t field;
	uint32_t index;
	uint16_t half;

	/* The weights, then the bias. */
	for (field = 0U; field < 2U; field++) {
		offset = znet_u32(entry, 44U + 8U * field);
		count = znet_u32(entry, 48U + 8U * field);
		if (field == 0U)
			op->weight_count = (uint32_t)count;
		else
			op->bias_count = (uint32_t)count;

		/* An op without values. */
		if (count == 0U)
			continue;

		/* Inside the data section and aligned. */
		bytes = count * file->element_size;
		if (offset % file->element_size != 0U)
			return YOLOX_ERROR_FORMAT;
		if (offset > file->data_size || bytes > file->data_size - offset)
			return YOLOX_ERROR_FORMAT;
		if ((uint64_t)*next_value + count > file->data_size / file->element_size)
			return YOLOX_ERROR_FORMAT;

		/* Widened to float32 into the model's values. */
		source = file->bytes + file->data_offset + offset;
		target = model->values + *next_value;
		for (index = 0U; index < count; index++) {
			if (file->element_size == 2U) {
				half = (uint16_t)(source[2U * index] | (source[2U * index + 1U] << 8));
				target[index] = znet_half_to_float(half);
			} else {
				memcpy(&target[index], source + 4U * index, sizeof(float));
			}
		}

		/* The op points at its values. */
		if (field == 0U)
			op->weights = target;
		else
			op->bias = target;
		*next_value += (uint32_t)count;
	}

	/* Succeeded: the values are read. */
	return 0;
}

/* Checks one op's kind, activation, shape and value counts. */
static int
znet_check_op(
	const struct yolox_model *model,
	const struct yolox_op *op)
{
	const struct yolox_tensor_info *in;
	const struct yolox_tensor_info *out;
	uint64_t expected;
	uint32_t cout;

	/* Only a conv has an activation, values, a kernel and groups. */
	in = &model->tensors[op->inputs[0]];
	out = &model->tensors[op->output];
	cout = out->channels;
	if (op->activation != YOLOX_ACT_NONE && op->activation != YOLOX_ACT_SILU)
		return YOLOX_ERROR_FORMAT;
	if (op->kind != YOLOX_OP_CONV && op->activation != YOLOX_ACT_NONE)
		return YOLOX_ERROR_FORMAT;
	if (op->kind != YOLOX_OP_CONV && (op->weight_count != 0U || op->bias_count != 0U))
		return YOLOX_ERROR_FORMAT;

	/* The rules of each kind. */
	switch (op->kind) {
	case YOLOX_OP_CONV:
		if (op->input_count != 1U)
			return YOLOX_ERROR_FORMAT;
		if (op->kernel != 1U && op->kernel != 3U)
			return YOLOX_ERROR_FORMAT;
		if (op->stride != 1U && op->stride != 2U)
			return YOLOX_ERROR_FORMAT;
		if (op->groups != 1U && op->groups != op->cin)
			return YOLOX_ERROR_FORMAT;
		if (op->groups == op->cin && op->groups != 1U && cout != op->cin)
			return YOLOX_ERROR_FORMAT;
		if (out->divisor != in->divisor * op->stride)
			return YOLOX_ERROR_FORMAT;
		expected = (uint64_t)cout * (op->cin / op->groups) * op->kernel * op->kernel;
		if (op->weight_count != expected || op->bias_count != cout)
			return YOLOX_ERROR_FORMAT;
		break;
	case YOLOX_OP_MAXPOOL:
		if (op->input_count != 1U || op->stride != 1U)
			return YOLOX_ERROR_FORMAT;
		if (op->kernel != 5U && op->kernel != 9U && op->kernel != 13U)
			return YOLOX_ERROR_FORMAT;
		if (cout != op->cin || out->divisor != in->divisor)
			return YOLOX_ERROR_FORMAT;
		break;
	case YOLOX_OP_UPSAMPLE:
		if (op->input_count != 1U || cout != op->cin)
			return YOLOX_ERROR_FORMAT;
		if (in->divisor < 2U || out->divisor * 2U != in->divisor)
			return YOLOX_ERROR_FORMAT;
		break;
	case YOLOX_OP_SPACE_TO_DEPTH:
		if (op->input_count != 1U || cout != 4U * op->cin)
			return YOLOX_ERROR_FORMAT;
		if (out->divisor != in->divisor * 2U)
			return YOLOX_ERROR_FORMAT;
		break;
	case YOLOX_OP_ADD:
		if (op->input_count != 2U)
			return YOLOX_ERROR_FORMAT;
		break;
	case YOLOX_OP_CONCAT:
		if (op->input_count < 2U)
			return YOLOX_ERROR_FORMAT;
		break;
	default:
		return YOLOX_ERROR_FORMAT;
	}

	/* Succeeded: the op is well formed. */
	return 0;
}

/*
 * Checks the graph: each tensor made by exactly one op (the input by
 * none), every input made before it is read (so there is no cycle), the
 * shapes of add and concat, and the head's tensors.
 */
static int
znet_check_graph(
	const struct yolox_model *model)
{
	const struct yolox_tensor_info *tensor;
	const struct yolox_op *op;
	uint8_t *made;
	uint32_t channels;
	uint32_t index;
	uint32_t input;
	uint32_t level;
	uint32_t expected[3];
	int error;

	/* Which tensors exist so far: only the input at the start. */
	made = calloc(model->tensor_count, 1U);
	if (made == NULL)
		return YOLOX_ERROR_MEMORY;
	made[model->input_tensor] = 1U;

	/* Each op in order. */
	error = 0;
	for (index = 0U; index < model->op_count && error == 0; index++) {
		op = &model->ops[index];

		/* Its inputs exist already. */
		for (input = 0U; input < op->input_count; input++) {
			if (made[op->inputs[input]] == 0U)
				error = YOLOX_ERROR_FORMAT;
		}

		/* Its output is made only here. */
		if (made[op->output] != 0U)
			error = YOLOX_ERROR_FORMAT;
		made[op->output] = 1U;

		/* An add's two inputs and output have one shape. */
		if (error == 0 && op->kind == YOLOX_OP_ADD) {
			for (input = 0U; input < 2U; input++) {
				tensor = &model->tensors[op->inputs[input]];
				if (tensor->channels != model->tensors[op->output].channels)
					error = YOLOX_ERROR_FORMAT;
				if (tensor->divisor != model->tensors[op->output].divisor)
					error = YOLOX_ERROR_FORMAT;
			}
		}

		/* A concat's inputs share the output's divisor and sum to its channels. */
		if (error == 0 && op->kind == YOLOX_OP_CONCAT) {
			channels = 0U;
			for (input = 0U; input < op->input_count; input++) {
				tensor = &model->tensors[op->inputs[input]];
				channels += tensor->channels;
				if (tensor->divisor != model->tensors[op->output].divisor)
					error = YOLOX_ERROR_FORMAT;
			}
			if (channels != model->tensors[op->output].channels)
				error = YOLOX_ERROR_FORMAT;
		}
	}

	/* Every tensor is made. */
	for (index = 0U; index < model->tensor_count && error == 0; index++) {
		if (made[index] == 0U)
			error = YOLOX_ERROR_FORMAT;
	}
	free(made);
	if (error != 0)
		return error;

	/* The head's tensors: flagged, 4, 1 and 80 channels, at the level's stride. */
	expected[0] = 4U;
	expected[1] = 1U;
	expected[2] = YOLOX_CLASSES;
	for (level = 0U; level < YOLOX_LEVELS; level++) {
		for (index = 0U; index < 3U; index++) {
			tensor = &model->tensors[model->level_tensors[level][index]];
			if ((tensor->flags & YOLOX_TENSOR_HEAD_OUTPUT) == 0U)
				return YOLOX_ERROR_FORMAT;
			if (tensor->channels != expected[index] || tensor->divisor != model->strides[level])
				return YOLOX_ERROR_FORMAT;
		}
	}

	/* Succeeded: the graph is consistent. */
	return 0;
}

/* Widens an IEEE binary16 value. */
static float
znet_half_to_float(
	uint16_t half)
{
	uint32_t sign;
	uint32_t exponent;
	uint32_t mantissa;
	uint32_t bits;
	float value;

	/* The fields. */
	sign = (uint32_t)(half >> 15) << 31;
	exponent = (half >> 10) & 0x1fU;
	mantissa = half & 0x3ffU;

	/* Decides the kind of value. */
	if (exponent == 0U && mantissa == 0U) {
		/* A zero. */
		bits = sign;
	} else if (exponent == 0U) {
		/* A subnormal, normalized. */
		exponent = 127U - 15U + 1U;
		while ((mantissa & 0x400U) == 0U) {
			mantissa <<= 1;
			exponent--;
		}
		bits = sign | (exponent << 23) | ((mantissa & 0x3ffU) << 13);
	} else if (exponent == 0x1fU) {
		/* An infinity or a NaN. */
		bits = sign | 0x7f800000U | (mantissa << 13);
	} else {
		/* A normal value. */
		bits = sign | ((exponent + 127U - 15U) << 23) | (mantissa << 13);
	}

	/* Succeeded: the float32. */
	memcpy(&value, &bits, sizeof(value));
	return value;
}

/* Reports whether a value is a power of two. */
static int
znet_power_of_two(
	uint32_t value)
{
	/* A power of two has one bit set. */
	if (value == 0U)
		return 0;
	if ((value & (value - 1U)) != 0U)
		return 0;

	/* Succeeded: one bit. */
	return 1;
}
