/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
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

static int
input_byte(struct input_buffer *input)
{
	while (input->offset == input->size) {
		ssize_t n = read(input->fd, input->data, sizeof(input->data));
		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0)
			return n ? -1 : -2;
		input->offset = 0;
		input->size = (size_t)n;
	}
	return input->data[input->offset++];
}

static int
output_flush(struct output_buffer *output)
{
	size_t done = 0;
	while (done < output->size) {
		ssize_t n =
		    write(output->fd, output->data + done, output->size - done);
		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0)
			return -1;
		done += (size_t)n;
	}
	output->size = 0;
	return 0;
}

static int
output_byte(struct output_buffer *output, unsigned char byte)
{
	if (output->size == sizeof(output->data) && output_flush(output))
		return -1;
	output->data[output->size++] = byte;
	return 0;
}

static int
code_writer_flush(struct code_writer *writer)
{
	unsigned bytes = (writer->count * writer->width + 7) / 8;
	for (unsigned i = 0; i < bytes; i++)
		if (output_byte(writer->output, writer->packet[i]))
			return -1;
	memset(writer->packet, 0, sizeof(writer->packet));
	writer->count = 0;
	return 0;
}

static int
code_writer_width(struct code_writer *writer, unsigned width)
{
	if (writer->count && code_writer_flush(writer))
		return -1;
	writer->width = width;
	return 0;
}

static int
code_write(struct code_writer *writer, unsigned code)
{
	unsigned bit = writer->count * writer->width;
	for (unsigned i = 0; i < writer->width; i++)
		if (code & (1U << i))
			writer->packet[(bit + i) / 8] |= 1U << ((bit + i) % 8);
	if (++writer->count == 8)
		return code_writer_flush(writer);
	return 0;
}

static int
code_reader_width(struct code_reader *reader, unsigned width)
{
	reader->width = width;
	reader->index = 0;
	reader->count = 0;
	return 0;
}

static int
code_read(struct code_reader *reader, unsigned *code)
{
	unsigned bytes = reader->width;
	if (reader->index == reader->count) {
		unsigned got = 0;
		while (got < bytes) {
			int byte = input_byte(reader->input);
			if (byte == -1)
				return -1;
			if (byte == -2)
				break;
			reader->packet[got++] = (unsigned char)byte;
		}
		if (!got)
			return 0;
		reader->count = got * 8 / reader->width;
		reader->index = 0;
		if (!reader->count) {
			errno = EINVAL;
			return -1;
		}
	}
	*code = 0;
	unsigned bit = reader->index++ * reader->width;
	for (unsigned i = 0; i < reader->width; i++)
		if (reader->packet[(bit + i) / 8] & (1U << ((bit + i) % 8)))
			*code |= 1U << i;
	return 1;
}

static unsigned
hash_pair(unsigned prefix, unsigned suffix)
{
	return ((prefix << 8) ^ suffix ^ (prefix >> 3)) % LZW_HASH_SIZE;
}

int
lzw_compress(int input_fd, int output_fd, unsigned max_bits, int block_mode)
{
	struct input_buffer input = {.fd = input_fd};
	struct output_buffer output = {.fd = output_fd};
	struct code_writer writer = {.output = &output, .width = 9};
	int32_t *hash_codes = NULL;
	uint32_t *hash_prefix = NULL;
	unsigned char *hash_suffix = NULL;
	unsigned next_code, maximum, width_limit = 511;
	int first, byte;
	int status = -1;

	if (max_bits < 9 || max_bits > 16) {
		errno = EINVAL;
		return -1;
	}
	maximum = 1U << max_bits;
	next_code = block_mode ? LZW_FIRST_BLOCK : LZW_FIRST_PLAIN;
	hash_codes = malloc(LZW_HASH_SIZE * sizeof(*hash_codes));
	hash_prefix = malloc(LZW_HASH_SIZE * sizeof(*hash_prefix));
	hash_suffix = malloc(LZW_HASH_SIZE);
	if (!hash_codes || !hash_prefix || !hash_suffix)
		goto out;
	for (size_t i = 0; i < LZW_HASH_SIZE; i++)
		hash_codes[i] = -1;
	if (output_byte(&output, LZW_MAGIC0) ||
	    output_byte(&output, LZW_MAGIC1) ||
	    output_byte(
		&output,
		(unsigned char)(max_bits | (block_mode ? LZW_BLOCK_MODE : 0))))
		goto out;
	first = input_byte(&input);
	if (first == -1)
		goto out;
	if (first == -2) {
		status = output_flush(&output);
		goto out;
	}
	unsigned prefix = (unsigned)first;
	while ((byte = input_byte(&input)) >= 0) {
		unsigned slot = hash_pair(prefix, (unsigned)byte);
		while (hash_codes[slot] >= 0 && (hash_prefix[slot] != prefix ||
						 hash_suffix[slot] != byte))
			slot = slot + 1 == LZW_HASH_SIZE ? 0 : slot + 1;
		if (hash_codes[slot] >= 0) {
			prefix = (unsigned)hash_codes[slot];
			continue;
		}
		if (code_write(&writer, prefix))
			goto out;
		if (next_code > width_limit && writer.width < max_bits) {
			if (code_writer_width(&writer, writer.width + 1))
				goto out;
			width_limit = (1U << writer.width) - 1;
		}
		if (next_code < maximum) {
			hash_codes[slot] = (int32_t)next_code++;
			hash_prefix[slot] = prefix;
			hash_suffix[slot] = (unsigned char)byte;
		}
		prefix = (unsigned)byte;
	}
	if (byte == -1)
		goto out;
	if (code_write(&writer, prefix) || code_writer_flush(&writer) ||
	    output_flush(&output))
		goto out;
	status = 0;
out:
	free(hash_codes);
	free(hash_prefix);
	free(hash_suffix);
	return status;
}

int
lzw_decompress(int input_fd, int output_fd)
{
	struct input_buffer input = {.fd = input_fd};
	struct output_buffer output = {.fd = output_fd};
	struct code_reader reader = {.input = &input, .width = 9};
	uint16_t *prefix = NULL;
	unsigned char *suffix = NULL, *stack = NULL;
	unsigned max_bits, maximum, next_code, width_limit = 511;
	unsigned old_code, code, first_char;
	int block_mode, result;
	int status = -1;

	int m0 = input_byte(&input), m1 = input_byte(&input),
	    flags = input_byte(&input);
	if (m0 != LZW_MAGIC0 || m1 != LZW_MAGIC1 || flags < 0) {
		errno = EINVAL;
		return -1;
	}
	max_bits = (unsigned)flags & 31;
	block_mode = flags & LZW_BLOCK_MODE;
	if (max_bits < 9 || max_bits > 16 || (flags & 0x60)) {
		errno = EINVAL;
		return -1;
	}
	maximum = 1U << max_bits;
	next_code = block_mode ? LZW_FIRST_BLOCK : LZW_FIRST_PLAIN;
	prefix = malloc(maximum * sizeof(*prefix));
	suffix = malloc(maximum);
	stack = malloc(maximum);
	if (!prefix || !suffix || !stack)
		goto out;
	for (unsigned i = 0; i < 256; i++)
		suffix[i] = (unsigned char)i;
	result = code_read(&reader, &old_code);
	if (result < 0)
		goto out;
	if (!result) {
		status = 0;
		goto out;
	}
	if (old_code >= 256) {
		errno = EINVAL;
		goto out;
	}
	first_char = old_code;
	if (output_byte(&output, (unsigned char)old_code))
		goto out;
	for (;;) {
		if (next_code > width_limit && reader.width < max_bits) {
			code_reader_width(&reader, reader.width + 1);
			width_limit = (1U << reader.width) - 1;
		}
		result = code_read(&reader, &code);
		if (result < 0)
			goto out;
		if (!result)
			break;
		if (block_mode && code == LZW_CLEAR) {
			next_code = LZW_FIRST_BLOCK;
			width_limit = 511;
			code_reader_width(&reader, 9);
			result = code_read(&reader, &old_code);
			if (result <= 0 || old_code >= 256) {
				errno = EINVAL;
				goto out;
			}
			first_char = old_code;
			if (output_byte(&output, (unsigned char)old_code))
				goto out;
			continue;
		}
		unsigned incoming = code;
		size_t stack_size = 0;
		if (code == next_code) {
			stack[stack_size++] = (unsigned char)first_char;
			code = old_code;
		} else if (code > next_code) {
			errno = EINVAL;
			goto out;
		}
		while (code >= 256) {
			if (code >= next_code || stack_size == maximum) {
				errno = EINVAL;
				goto out;
			}
			stack[stack_size++] = suffix[code];
			code = prefix[code];
		}
		first_char = suffix[code];
		stack[stack_size++] = (unsigned char)first_char;
		while (stack_size)
			if (output_byte(&output, stack[--stack_size]))
				goto out;
		if (next_code < maximum) {
			prefix[next_code] = (uint16_t)old_code;
			suffix[next_code++] = (unsigned char)first_char;
		}
		old_code = incoming;
	}
	if (output_flush(&output))
		goto out;
	status = 0;
out:
	free(prefix);
	free(suffix);
	free(stack);
	return status;
}
