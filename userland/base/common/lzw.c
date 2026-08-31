/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Implements shared userland lzw support.
 */

#include "userland/base/common/lzw.h"

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define LZW_MAGIC0 0x1f
#define LZW_MAGIC1 0x9d
#define LZW_BLOCK_MODE 0x80
#define LZW_CLEAR 256
#define LZW_FIRST_BLOCK 257
#define LZW_FIRST_PLAIN 256
#define LZW_HASH_SIZE 131071

struct input_buffer {
	int fd;
	unsigned char data[4096];
	size_t offset;
	size_t size;
};

struct output_buffer {
	int fd;
	unsigned char data[4096];
	size_t size;
};

struct code_writer {
	struct output_buffer *output;
	unsigned width;
	unsigned count;
	unsigned char packet[16];
};

struct code_reader {
	struct input_buffer *input;
	unsigned width;
	unsigned index;
	unsigned count;
	unsigned char packet[16];
};

static int output_byte(struct output_buffer *output, unsigned char byte);
static int output_flush(struct output_buffer *output);
static int input_byte(struct input_buffer *input);
static unsigned hash_pair(unsigned prefix, unsigned suffix);
static int code_write(struct code_writer *writer, unsigned code);
static int code_writer_flush(struct code_writer *writer);
static int code_writer_width(struct code_writer *writer, unsigned width);
static int code_read(struct code_reader *reader, unsigned *code);
static int code_reader_width(struct code_reader *reader, unsigned width);

/*
 * Implements the lzw compress operation.
 */
int
lzw_compress(
	int input_fd,
	int output_fd,
	unsigned max_bits,
	int block_mode)
{
	unsigned slot;
	size_t i_index_for;
	struct input_buffer input = {.fd = input_fd};
	struct output_buffer output = {.fd = output_fd};
	struct code_writer writer = {.output = &output, .width = 9};
	int32_t *hash_codes;
	uint32_t *hash_prefix;
	unsigned char *hash_suffix;
	unsigned next_code, maximum, width_limit;
	unsigned prefix;
	int first, byte;
	int status;

	hash_codes = NULL;
	hash_prefix = NULL;
	hash_suffix = NULL;
	width_limit = 511;
	status = -1;

	/* Handles the max bits condition. */
	if (max_bits < 9 || max_bits > 16) {
		errno = EINVAL;

		/* Reports operation failure. */
		return -1;
	}
	maximum = 1U << max_bits;
	next_code = block_mode ? LZW_FIRST_BLOCK : LZW_FIRST_PLAIN;
	hash_codes = malloc(LZW_HASH_SIZE * sizeof(*hash_codes));
	hash_prefix = malloc(LZW_HASH_SIZE * sizeof(*hash_prefix));
	hash_suffix = malloc(LZW_HASH_SIZE);

	/* Handles the hash codes condition. */
	if (!hash_codes || !hash_prefix || !hash_suffix)
		goto out;

	/* Process each remaining element. */
	for (i_index_for = 0; i_index_for < LZW_HASH_SIZE; i_index_for++)
		hash_codes[i_index_for] = -1;

	/* Handles the output byte condition. */
	if (output_byte(&output, LZW_MAGIC0) ||
	    output_byte(&output, LZW_MAGIC1) ||
	    output_byte(
		&output,
		(unsigned char)(max_bits | (block_mode ? LZW_BLOCK_MODE : 0))))
		goto out;
	first = input_byte(&input);

	/* Handles the first condition. */
	if (first == -1)
		goto out;

	/* Handles the first condition. */
	if (first == -2) {
		status = output_flush(&output);
		goto out;
	}

	/* Continue while the operation condition remains true. */
	prefix = (unsigned)first;
	while ((byte = input_byte(&input)) >= 0) {
		/* Continue while the operation condition remains true. */
		slot = hash_pair(prefix, (unsigned)byte);
		while (hash_codes[slot] >= 0 && (hash_prefix[slot] != prefix ||
						 hash_suffix[slot] != byte))
			slot = slot + 1 == LZW_HASH_SIZE ? 0 : slot + 1;

		/* Handles the hash codes condition. */
		if (hash_codes[slot] >= 0) {
			prefix = (unsigned)hash_codes[slot];
			continue;
		}

		/* Handles the code write condition. */
		if (code_write(&writer, prefix))
			goto out;

		/* Handles the next code condition. */
		if (next_code > width_limit && writer.width < max_bits) {
			/* Handles the code writer width condition. */
			if (code_writer_width(&writer, writer.width + 1))
				goto out;
			width_limit = (1U << writer.width) - 1;
		}

		/* Handles the next code condition. */
		if (next_code < maximum) {
			hash_codes[slot] = (int32_t)next_code++;
			hash_prefix[slot] = prefix;
			hash_suffix[slot] = (unsigned char)byte;
		}
		prefix = (unsigned)byte;
	}

	/* Classifies the current byte. */
	if (byte == -1)
		goto out;

	/* Handles the code write condition. */
	if (code_write(&writer, prefix) || code_writer_flush(&writer) ||
	    output_flush(&output))
		goto out;
	status = 0;
out:
	free(hash_codes);
	free(hash_prefix);
	free(hash_suffix);

	/* Returns the computed result. */
	return status;
}

/*
 * Implements the lzw decompress operation.
 */
int
lzw_decompress(
	int input_fd,
	int output_fd)
{
	unsigned i_index_for;
	struct input_buffer input = {.fd = input_fd};
	struct output_buffer output = {.fd = output_fd};
	struct code_reader reader = {.input = &input, .width = 9};
	uint16_t *prefix;
	unsigned char *suffix, *stack;
	unsigned max_bits, maximum, next_code, width_limit;
	unsigned old_code, code, first_char;
	unsigned incoming;
	size_t stack_size;
	int block_mode, result;
	int status;
	int m0, m1, flags;

	prefix = NULL;
	suffix = NULL;
	stack = NULL;
	width_limit = 511;
	status = -1;
	m0 = input_byte(&input);
	m1 = input_byte(&input);
	flags = input_byte(&input);

	/* Handles the m0 condition. */
	if (m0 != LZW_MAGIC0 || m1 != LZW_MAGIC1 || flags < 0) {
		errno = EINVAL;

		/* Reports operation failure. */
		return -1;
	}
	max_bits = (unsigned)flags & 31;
	block_mode = flags & LZW_BLOCK_MODE;

	/* Handles the max bits condition. */
	if (max_bits < 9 || max_bits > 16 || (flags & 0x60)) {
		errno = EINVAL;

		/* Reports operation failure. */
		return -1;
	}
	maximum = 1U << max_bits;
	next_code = block_mode ? LZW_FIRST_BLOCK : LZW_FIRST_PLAIN;
	prefix = malloc(maximum * sizeof(*prefix));
	suffix = malloc(maximum);
	stack = malloc(maximum);

	/* Handles the prefix condition. */
	if (!prefix || !suffix || !stack)
		goto out;

	/* Process each element required by the operation. */
	for (i_index_for = 0; i_index_for < 256; i_index_for++)
		suffix[i_index_for] = (unsigned char)i_index_for;
	result = code_read(&reader, &old_code);

	/* Checks the operation result. */
	if (result < 0)
		goto out;

	/* Checks the operation result. */
	if (!result) {
		status = 0;
		goto out;
	}

	/* Handles the old code condition. */
	if (old_code >= 256) {
		errno = EINVAL;
		goto out;
	}
	first_char = old_code;

	/* Handles the output byte condition. */
	if (output_byte(&output, (unsigned char)old_code))
		goto out;

	/* Continue until the operation reaches a terminal state. */
	for (;;) {
		/* Handles the next code condition. */
		if (next_code > width_limit && reader.width < max_bits) {
			code_reader_width(&reader, reader.width + 1);
			width_limit = (1U << reader.width) - 1;
		}
		result = code_read(&reader, &code);

		/* Checks the operation result. */
		if (result < 0)
			goto out;

		/* Checks the operation result. */
		if (!result)
			break;

		/* Handles the block mode condition. */
		if (block_mode && code == LZW_CLEAR) {
			next_code = LZW_FIRST_BLOCK;
			width_limit = 511;
			code_reader_width(&reader, 9);
			result = code_read(&reader, &old_code);

			/* Checks the operation result. */
			if (result <= 0 || old_code >= 256) {
				errno = EINVAL;
				goto out;
			}
			first_char = old_code;

			/* Handles the output byte condition. */
			if (output_byte(&output, (unsigned char)old_code))
				goto out;
			continue;
		}
		incoming = code;
		stack_size = 0;

		/* Handles the code condition. */
		if (code == next_code) {
			stack[stack_size++] = (unsigned char)first_char;
			code = old_code;
		} else if (code > next_code) {
			errno = EINVAL;
			goto out;
		}
		while (code >= 256) {
			/* Handles the code condition. */
			if (code >= next_code || stack_size == maximum) {
				errno = EINVAL;
				goto out;
			}
			stack[stack_size++] = suffix[code];
			code = prefix[code];
		}

		/* Process each remaining element. */
		first_char = suffix[code];
		stack[stack_size++] = (unsigned char)first_char;
		while (stack_size) {
			/* Handles the output byte condition. */
			if (output_byte(&output, stack[--stack_size]))
				goto out;
		}

		/* Handles the next code condition. */
		if (next_code < maximum) {
			prefix[next_code] = (uint16_t)old_code;
			suffix[next_code++] = (unsigned char)first_char;
		}
		old_code = incoming;
	}

	/* Handles the output flush condition. */
	if (output_flush(&output))
		goto out;
	status = 0;
out:
	free(prefix);
	free(suffix);
	free(stack);

	/* Returns the computed result. */
	return status;
}

/* Supports the output byte operation. */
static int
output_byte(
	struct output_buffer *output,
	unsigned char byte)
{
	/* Handles a failed output flush operation. */
	if (output->size == sizeof(output->data) && output_flush(output))
		return -1;
	output->data[output->size++] = byte;

	/* Reports successful completion. */
	return 0;
}

/* Supports the output flush operation. */
static int
output_flush(
	struct output_buffer *output)
{
	ssize_t n;
	size_t done;

	/* Process each remaining element. */
	done = 0;
	while (done < output->size) {
		n = write(output->fd, output->data + done, output->size - done);

		/* Handles the reported system error. */
		if (n < 0 && errno == EINTR)
			continue;

		/* Checks the current item count. */
		if (n <= 0)
			return -1;
		done += (size_t)n;
	}
	output->size = 0;

	/* Reports successful completion. */
	return 0;
}

/* Supports the input byte operation. */
static int
input_byte(
	struct input_buffer *input)
{
	ssize_t n;

	/* Process each remaining element. */
	while (input->offset == input->size) {
		n = read(input->fd, input->data, sizeof(input->data));

		/* Handles the reported system error. */
		if (n < 0 && errno == EINTR)
			continue;

		/* Checks the current item count. */
		if (n <= 0)
			return n ? -1 : -2;
		input->offset = 0;
		input->size = (size_t)n;
	}

	/* Returns the computed result. */
	return input->data[input->offset++];
}

/* Supports the hash pair operation. */
static unsigned
hash_pair(
	unsigned prefix,
	unsigned suffix)
{
	/* Returns the computed result. */
	return ((prefix << 8) ^ suffix ^ (prefix >> 3)) % LZW_HASH_SIZE;
}

/* Supports the code write operation. */
static int
code_write(
	struct code_writer *writer,
	unsigned code)
{
	int function_result;
	unsigned i_index_for;
	unsigned bit;

	/* Process each element required by the operation. */
	bit = writer->count * writer->width;
	for (i_index_for = 0; i_index_for < writer->width; i_index_for++) {
		/* Handles the code condition. */
		if (code & (1U << i_index_for))
			writer->packet[(bit + i_index_for) / 8] |= 1U << ((bit + i_index_for) % 8);
	}

	/* Handles the writer condition. */
	if (++writer->count == 8) {
		/* Obtains the code writer flush result. */
		function_result = code_writer_flush(writer);

		/* Returns the computed result. */
		return function_result;
	}

	/* Reports successful completion. */
	return 0;
}

/* Supports the code writer flush operation. */
static int
code_writer_flush(
	struct code_writer *writer)
{
	unsigned i_index_for;
	unsigned bytes;

	/* Process each element required by the operation. */
	bytes = (writer->count * writer->width + 7) / 8;
	for (i_index_for = 0; i_index_for < bytes; i_index_for++) {
		/* Handles a failed output byte operation. */
		if (output_byte(writer->output, writer->packet[i_index_for]))
			return -1;
	}
	memset(writer->packet, 0, sizeof(writer->packet));
	writer->count = 0;

	/* Reports successful completion. */
	return 0;
}

/* Supports the code writer width operation. */
static int
code_writer_width(
	struct code_writer *writer,
	unsigned width)
{
	/* Handles a failed code writer flush operation. */
	if (writer->count && code_writer_flush(writer))
		return -1;
	writer->width = width;

	/* Reports successful completion. */
	return 0;
}

/* Supports the code read operation. */
static int
code_read(
	struct code_reader *reader,
	unsigned *code)
{
	int byte;
	unsigned got;
	unsigned i_index_for;
	unsigned bit, bytes;

	bytes = reader->width;

	/* Handles the reader condition. */
	if (reader->index == reader->count) {
		/* Continue while the operation condition remains true. */
		got = 0;
		while (got < bytes) {
			byte = input_byte(reader->input);

			/* Classifies the current byte. */
			if (byte == -1)
				return -1;

			/* Classifies the current byte. */
			if (byte == -2)
				break;
			reader->packet[got++] = (unsigned char)byte;
		}

		/* Handles the got condition. */
		if (!got)
			return 0;
		reader->count = got * 8 / reader->width;
		reader->index = 0;

		/* Handles the reader condition. */
		if (!reader->count) {
			errno = EINVAL;

			/* Reports operation failure. */
			return -1;
		}
	}

	/* Process each element required by the operation. */
	*code = 0;
	bit = reader->index++ * reader->width;
	for (i_index_for = 0; i_index_for < reader->width; i_index_for++) {
		/* Handles the reader condition. */
		if (reader->packet[(bit + i_index_for) / 8] & (1U << ((bit + i_index_for) % 8)))
			*code |= 1U << i_index_for;
	}

	/* Reports operation failure. */
	return 1;
}

/* Supports the code reader width operation. */
static int
code_reader_width(
	struct code_reader *reader,
	unsigned width)
{
	reader->width = width;
	reader->index = 0;
	reader->count = 0;

	/* Reports successful completion. */
	return 0;
}
