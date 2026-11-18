/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The stream filters of libpdf's reader (stages 1 and 2 of design-pdf.md):
 * FlateDecode through libz-compat and LZWDecode, both with the PNG and TIFF
 * predictors, ASCIIHexDecode, ASCII85Decode, RunLengthDecode and (stage
 * 3) CCITTFaxDecode through the fax decoder (ccitt.c).  DCTDecode is not
 * decoded here: an image's JPEG bytes are handed to the image decoder
 * (image.c) as they are.  Any other filter (JBIG2, JPX) is reported as
 * ENOTSUP.
 *
 * Every output is bounded by PDF_FILTER_OUTPUT_MAX, which stops a small
 * stream that inflates to gigabytes.  A Flate stream that is cut short or
 * damaged keeps the bytes decoded before the damage, as other readers do.
 */

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include <compat/zlib/zlib.h>

#include <pdf.h>

#include "internal.h"

/* The most filters one stream may chain. */
#define PDF_FILTER_CHAIN_MAX 8

/* The first size of an inflated buffer, and the size of the input slices given to inflate. */
#define PDF_FILTER_INITIAL 65536
#define PDF_FILTER_SLICE ((size_t)1 << 30)

/* The PNG predictor types a row starts with. */
#define PDF_PNG_NONE 0
#define PDF_PNG_SUB 1
#define PDF_PNG_UP 2
#define PDF_PNG_AVERAGE 3
#define PDF_PNG_PAETH 4

/*
 * The kinds of filter the reader knows.
 */
enum pdf_filter_kind {
	PDF_FILTER_UNKNOWN = 0,
	PDF_FILTER_FLATE,
	PDF_FILTER_ASCII_HEX,
	PDF_FILTER_ASCII_85,
	PDF_FILTER_LZW,
	PDF_FILTER_RUN_LENGTH,
	PDF_FILTER_CCITT,
	PDF_FILTER_DCT
};

/* The LZW codes that clear the table and end the data, the first code of the table, and its size. */
#define PDF_LZW_CLEAR 256
#define PDF_LZW_END 257
#define PDF_LZW_FIRST 258
#define PDF_LZW_TABLE 4096

/* The widest LZW code, in bits. */
#define PDF_LZW_WIDTH_MAX 12

/*
 * One entry of the LZW string table.
 *
 * A string is its prefix's string followed by one byte, so an entry keeps
 * the code of the prefix, the last byte, the first byte (the byte the next
 * entry may append) and the length.  The table lives on the heap for one
 * decode.
 */
struct pdf_lzw_entry {
	unsigned short prefix;
	unsigned char last;
	unsigned char first;
	unsigned long length;
};

/*
 * The output of a decoder that does not know its size in advance.
 *
 * It grows by doubling up to the decode limit; error keeps the first
 * failure, so that a decoder checks it once per step.
 */
struct pdf_filter_output {
	unsigned char *data;
	size_t size;
	size_t capacity;
	int error;
};

/*
 * The parameters of a predictor, from a filter's /DecodeParms.
 */
struct pdf_predictor {
	long predictor;
	long colors;
	long bits;
	long columns;
};

static int read_chain(struct pdf_document *document, const struct pdf_object *stream, enum pdf_filter_kind *kinds, struct pdf_object **parameters, size_t *count);
static int decrypt_stream(struct pdf_document *document, const struct pdf_object *stream, const unsigned char *data, size_t size, unsigned char **plain, size_t *plain_size);
static enum pdf_filter_kind filter_kind(const struct pdf_object *name);
static int apply_filter(struct pdf_document *document, const struct pdf_object *stream, enum pdf_filter_kind kind, struct pdf_object *parameters, const unsigned char *input, size_t input_size, unsigned char **output, size_t *output_size);
static int inflate_bytes(const unsigned char *input, size_t input_size, unsigned char **output, size_t *output_size);
static int decode_hex(const unsigned char *input, size_t input_size, unsigned char **output, size_t *output_size);
static int decode_ascii85(const unsigned char *input, size_t input_size, unsigned char **output, size_t *output_size);
static void put_ascii85_group(unsigned long group, int bytes, unsigned char *buffer, size_t *produced);
static int decode_lzw(struct pdf_document *document, struct pdf_object *parameters, const unsigned char *input, size_t input_size, unsigned char **output, size_t *output_size);
static int read_lzw(const unsigned char *input, size_t input_size, long early_change, struct pdf_filter_output *output);
static void add_lzw_entry(struct pdf_lzw_entry *table, unsigned int *next_code, unsigned int previous, unsigned int code);
static void put_lzw_string(const struct pdf_lzw_entry *table, unsigned int code, struct pdf_filter_output *output);
static int decode_run_length(const unsigned char *input, size_t input_size, unsigned char **output, size_t *output_size);
static void output_reserve(struct pdf_filter_output *output, size_t more);
static int read_predictor(struct pdf_document *document, struct pdf_object *parameters, struct pdf_predictor *predictor);
static int read_parameter(struct pdf_document *document, struct pdf_object *parameters, const char *key, long fallback, long *value);
static int read_flag(struct pdf_document *document, struct pdf_object *parameters, const char *key, int fallback, int *value);
static int read_ccitt(struct pdf_document *document, const struct pdf_object *stream, struct pdf_object *parameters, struct pdf_ccitt_parameters *ccitt);
static int undo_predictor(const struct pdf_predictor *predictor, unsigned char *data, size_t *size);
static int undo_png(const struct pdf_predictor *predictor, unsigned char *data, size_t *size);
static int undo_tiff(const struct pdf_predictor *predictor, unsigned char *data, size_t size);
static unsigned char paeth(unsigned char left, unsigned char above, unsigned char upper_left);

/*
 * Decodes a stream's data through its filters.
 *
 * *data is the decoded bytes: the document's own bytes when the stream has
 * no filter, or a buffer the caller frees through *owned (NULL when
 * nothing is to be freed).  With stop_at_dct, a last DCTDecode filter is
 * left undone and *dct is set, so that the image decoder reads the JPEG.
 */
int
pdf_filter_decode(
	struct pdf_document *document,
	const struct pdf_object *stream,
	int stop_at_dct,
	const unsigned char **data,
	size_t *size,
	unsigned char **owned,
	int *dct)
{
	enum pdf_filter_kind kinds[PDF_FILTER_CHAIN_MAX];
	struct pdf_object *parameters[PDF_FILTER_CHAIN_MAX];
	const unsigned char *current;
	unsigned char *current_owned;
	unsigned char *output;
	size_t current_size;
	size_t output_size;
	size_t count;
	size_t index;
	int error;

	/* Starts from the stream's raw bytes: the document's, or an inline image's in its content. */
	*owned = NULL;
	*dct = 0;
	if (stream->bytes != NULL) {
		current = stream->bytes + stream->data_offset;
	} else {
		current = pdf_reader_bytes(document) + stream->data_offset;
	}

	/* Nothing decoded is owned yet. */
	current_size = stream->data_length;
	current_owned = NULL;

	/* An encrypted document's stream is decrypted first. */
	error = decrypt_stream(document, stream, current, current_size, &current_owned, &current_size);
	if (error != 0)
		return error;
	if (current_owned != NULL)
		current = current_owned;

	/* Reads the chain of filters and their parameters. */
	error = read_chain(document, stream, kinds, parameters, &count);
	if (error != 0) {
		free(current_owned);
		return error;
	}

	/* Leaves a last DCTDecode for the image decoder, when asked to. */
	if (stop_at_dct &&
	    count > 0 &&
	    kinds[count - 1] == PDF_FILTER_DCT) {
		count--;
		*dct = 1;
	}

	/* Applies each filter in order, each one's output the next one's input. */
	for (index = 0; index < count; index++) {
		error = apply_filter(document, stream, kinds[index], parameters[index], current, current_size, &output, &output_size);
		if (error != 0) {
			free(current_owned);
			return error;
		}

		/* The filter's output replaces its input, which goes. */
		free(current_owned);
		current = output;
		current_size = output_size;
		current_owned = output;
	}

	/* Succeeded: the decoded bytes, and what the caller frees. */
	*data = current;
	*size = current_size;
	*owned = current_owned;
	return 0;
}

/*
 * Decrypts the data of an encrypted document's stream into a new buffer
 * (*plain stays NULL when the stream is not encrypted: an inline image, a
 * cross-reference stream, or a metadata stream the document leaves
 * plain).
 */
static int
decrypt_stream(
	struct pdf_document *document,
	const struct pdf_object *stream,
	const unsigned char *data,
	size_t size,
	unsigned char **plain,
	size_t *plain_size)
{
	struct pdf_crypt *crypt;
	struct pdf_object *type;
	unsigned char *buffer;
	int is_name;
	int encrypted_metadata;
	int error;

	/* Only a stream of the file, of an encrypted document, is encrypted. */
	*plain = NULL;
	crypt = pdf_reader_crypt(document);
	if (crypt == NULL)
		return 0;
	if (stream->bytes != NULL || stream->number == 0)
		return 0;

	/* A cross-reference stream never is, and a metadata stream only when the document says so. */
	type = pdf_object_get(stream, "Type");
	is_name = pdf_object_is_name(type, "XRef");
	if (is_name)
		return 0;
	is_name = pdf_object_is_name(type, "Metadata");
	encrypted_metadata = pdf_crypt_metadata(crypt);
	if (is_name && !encrypted_metadata)
		return 0;

	/* Decrypts into a buffer as long as the data (the plain bytes are never longer). */
	buffer = malloc(size + 1);
	if (buffer == NULL)
		return ENOMEM;
	error = pdf_crypt_decrypt(crypt, 1, stream->number, stream->generation, data, size, buffer, plain_size);
	if (error != 0) {
		free(buffer);
		return error;
	}

	/* Succeeded: the caller owns the plain bytes. */
	*plain = buffer;
	return 0;
}

/* Reads a stream's /Filter and /DecodeParms into parallel arrays. */
static int
read_chain(
	struct pdf_document *document,
	const struct pdf_object *stream,
	enum pdf_filter_kind *kinds,
	struct pdf_object **parameters,
	size_t *count)
{
	struct pdf_object *filter;
	struct pdf_object *decode;
	struct pdf_object *name;
	struct pdf_object *parameter;
	size_t index;
	int error;

	/* Finds the filter, a name or an array of them, and the parameters that go with it. */
	*count = 0;
	error = pdf_reader_resolve_key(document, stream, "Filter", &filter);
	if (error != 0)
		return error;
	error = pdf_reader_resolve_key(document, stream, "DecodeParms", &decode);
	if (error != 0)
		return error;

	/* No filter leaves the bytes as they are. */
	if (filter->type == PDF_OBJECT_NULL)
		return 0;

	/* One filter by its name, with its parameter dictionary. */
	if (filter->type == PDF_OBJECT_NAME) {
		kinds[0] = filter_kind(filter);
		parameters[0] = decode;
		*count = 1;
		return 0;
	}

	/* Refuses a filter that is neither a name nor an array, or a chain longer than the limit. */
	if (filter->type != PDF_OBJECT_ARRAY)
		return PDF_EFORMAT;
	if (filter->count > PDF_FILTER_CHAIN_MAX)
		return ENOTSUP;

	/* Each filter of the array, with the parameters at the same place of the parameter array. */
	for (index = 0; index < filter->count; index++) {
		/* The filter's name. */
		error = pdf_reader_resolve(document, filter->values[index], &name);
		if (error != 0)
			return error;
		if (name->type != PDF_OBJECT_NAME)
			return PDF_EFORMAT;
		kinds[index] = filter_kind(name);

		/* Its parameters, when the parameters are an array long enough. */
		parameter = NULL;
		if (decode->type == PDF_OBJECT_ARRAY && index < decode->count) {
			error = pdf_reader_resolve(document, decode->values[index], &parameter);
			if (error != 0)
				return error;
		}

		/* The filter's parameters, or none. */
		parameters[index] = parameter;
	}

	/* Succeeded: the chain is read. */
	*count = filter->count;
	return 0;
}

/* Tells which filter a name names, including the abbreviations of inline images. */
static enum pdf_filter_kind
filter_kind(
	const struct pdf_object *name)
{
	int is_flate;
	int is_hex;
	int is_known;
	int is_dct;

	/* The Flate names. */
	is_flate = pdf_object_is_name(name, "FlateDecode");
	if (is_flate)
		return PDF_FILTER_FLATE;
	is_flate = pdf_object_is_name(name, "Fl");
	if (is_flate)
		return PDF_FILTER_FLATE;

	/* The hexadecimal names. */
	is_hex = pdf_object_is_name(name, "ASCIIHexDecode");
	if (is_hex)
		return PDF_FILTER_ASCII_HEX;
	is_hex = pdf_object_is_name(name, "AHx");
	if (is_hex)
		return PDF_FILTER_ASCII_HEX;

	/* The base-85 names. */
	is_known = pdf_object_is_name(name, "ASCII85Decode");
	if (is_known)
		return PDF_FILTER_ASCII_85;
	is_known = pdf_object_is_name(name, "A85");
	if (is_known)
		return PDF_FILTER_ASCII_85;

	/* The LZW names. */
	is_known = pdf_object_is_name(name, "LZWDecode");
	if (is_known)
		return PDF_FILTER_LZW;
	is_known = pdf_object_is_name(name, "LZW");
	if (is_known)
		return PDF_FILTER_LZW;

	/* The run-length names. */
	is_known = pdf_object_is_name(name, "RunLengthDecode");
	if (is_known)
		return PDF_FILTER_RUN_LENGTH;
	is_known = pdf_object_is_name(name, "RL");
	if (is_known)
		return PDF_FILTER_RUN_LENGTH;

	/* The fax names. */
	is_known = pdf_object_is_name(name, "CCITTFaxDecode");
	if (is_known)
		return PDF_FILTER_CCITT;
	is_known = pdf_object_is_name(name, "CCF");
	if (is_known)
		return PDF_FILTER_CCITT;

	/* The JPEG names. */
	is_dct = pdf_object_is_name(name, "DCTDecode");
	if (is_dct)
		return PDF_FILTER_DCT;
	is_dct = pdf_object_is_name(name, "DCT");
	if (is_dct)
		return PDF_FILTER_DCT;

	/* Any other filter is not read yet. */
	return PDF_FILTER_UNKNOWN;
}

/*
 * Applies one filter to a buffer, making a new one.  The stream's own
 * dictionary gives the fax filter the image's height when its parameters
 * do not.
 */
static int
apply_filter(
	struct pdf_document *document,
	const struct pdf_object *stream,
	enum pdf_filter_kind kind,
	struct pdf_object *parameters,
	const unsigned char *input,
	size_t input_size,
	unsigned char **output,
	size_t *output_size)
{
	struct pdf_ccitt_parameters ccitt;
	struct pdf_predictor predictor;
	int error;

	/* Nothing is decoded yet. */
	*output = NULL;
	*output_size = 0;

	/* Decodes by the filter's kind. */
	switch (kind) {
	case PDF_FILTER_FLATE:
		/* Reads the predictor before inflating, so that a bad one costs nothing. */
		error = read_predictor(document, parameters, &predictor);
		if (error != 0)
			return error;
		error = inflate_bytes(input, input_size, output, output_size);
		if (error != 0)
			return error;

		/* Undoes the predictor in place. */
		error = undo_predictor(&predictor, *output, output_size);
		if (error != 0) {
			free(*output);
			return error;
		}

		/* Succeeded: the inflated bytes, the predictor undone. */
		return 0;
	case PDF_FILTER_ASCII_HEX:
		error = decode_hex(input, input_size, output, output_size);
		return error;
	case PDF_FILTER_ASCII_85:
		error = decode_ascii85(input, input_size, output, output_size);
		return error;
	case PDF_FILTER_LZW:
		error = decode_lzw(document, parameters, input, input_size, output, output_size);
		return error;
	case PDF_FILTER_RUN_LENGTH:
		error = decode_run_length(input, input_size, output, output_size);
		return error;
	case PDF_FILTER_CCITT:
		/* Reads the parameters before decoding, so that bad ones cost nothing. */
		error = read_ccitt(document, stream, parameters, &ccitt);
		if (error != 0)
			return error;
		error = pdf_ccitt_decode(&ccitt, input, input_size, output, output_size);
		return error;
	case PDF_FILTER_DCT:
	case PDF_FILTER_UNKNOWN:
		break;
	}

	/* A JPEG in the middle of a chain, or a filter the reader does not know. */
	return ENOTSUP;
}

/*
 * Inflates zlib-wrapped bytes into a new buffer, bounded by the decode
 * limit.
 *
 * Output decoded before a damaged or cut-short end is kept; a stream that
 * gives no byte at all is malformed.
 */
static int
inflate_bytes(
	const unsigned char *input,
	size_t input_size,
	unsigned char **output,
	size_t *output_size)
{
	z_stream stream;
	unsigned char *buffer;
	unsigned char *grown;
	size_t capacity;
	size_t produced;
	size_t consumed;
	size_t slice;
	int status;

	/* Starts the inflater. */
	memset(&stream, 0, sizeof(stream));
	status = inflateInit(&stream);
	if (status != Z_OK)
		return ENOMEM;

	/* Allocates the first output buffer. */
	capacity = PDF_FILTER_INITIAL;
	buffer = malloc(capacity);
	if (buffer == NULL) {
		inflateEnd(&stream);
		return ENOMEM;
	}

	/* Inflates until the stream ends, the input runs out, or the data is damaged. */
	produced = 0;
	consumed = 0;
	for (;;) {
		/* Grows the buffer when it is full, up to the decode limit. */
		if (produced == capacity) {
			if (capacity >= PDF_FILTER_OUTPUT_MAX) {
				inflateEnd(&stream);
				free(buffer);
				return ENOMEM;
			}

			/* Doubles the room, bounded by the limit. */
			capacity *= 2;
			if (capacity > PDF_FILTER_OUTPUT_MAX)
				capacity = PDF_FILTER_OUTPUT_MAX;
			grown = realloc(buffer, capacity);
			if (grown == NULL) {
				inflateEnd(&stream);
				free(buffer);
				return ENOMEM;
			}

			/* The grown buffer is the output's. */
			buffer = grown;
		}

		/* Gives inflate the next slice of the input and the free room of the output. */
		slice = input_size - consumed;
		if (slice > PDF_FILTER_SLICE)
			slice = PDF_FILTER_SLICE;
		stream.next_in = (Bytef *)(input + consumed);
		stream.avail_in = (uInt)slice;
		stream.next_out = buffer + produced;
		stream.avail_out = (uInt)(capacity - produced);
		status = inflate(&stream, Z_NO_FLUSH);
		consumed += slice - stream.avail_in;
		produced = capacity - stream.avail_out;

		/* The end of the stream, damaged data, or no progress with the input used up end the inflation. */
		if (status == Z_STREAM_END)
			break;
		if (status != Z_OK && status != Z_BUF_ERROR)
			break;
		if (consumed == input_size && produced < capacity)
			break;
	}

	/* The inflation's state is not needed any more. */
	inflateEnd(&stream);

	/* Refuses a stream that gave nothing. */
	if (produced == 0 && status != Z_STREAM_END) {
		free(buffer);
		return PDF_EFORMAT;
	}

	/* Succeeded: the inflated bytes. */
	*output = buffer;
	*output_size = produced;
	return 0;
}

/*
 * Decodes ASCIIHexDecode: pairs of hexadecimal digits up to '>', white
 * space ignored, an odd last digit followed by 0.
 */
static int
decode_hex(
	const unsigned char *input,
	size_t input_size,
	unsigned char **output,
	size_t *output_size)
{
	unsigned char *buffer;
	unsigned char character;
	size_t index;
	size_t produced;
	int high;
	int digit;

	/* Allocates for the most bytes the input can give (at least one, so that malloc(0) is avoided). */
	buffer = malloc(input_size / 2 + 1);
	if (buffer == NULL)
		return ENOMEM;

	/* Reads the digits in pairs. */
	produced = 0;
	high = -1;
	for (index = 0; index < input_size; index++) {
		/* The end marker ends the data. */
		character = input[index];
		if (character == '>')
			break;

		/* Finds the digit's value; white space and other bytes are skipped. */
		digit = -1;
		if (character >= '0' && character <= '9')
			digit = character - '0';
		if (character >= 'a' && character <= 'f')
			digit = character - 'a' + 10;
		if (character >= 'A' && character <= 'F')
			digit = character - 'A' + 10;
		if (digit < 0)
			continue;

		/* Keeps a first digit, or completes a byte with a second. */
		if (high < 0) {
			high = digit;
		} else {
			buffer[produced] = (unsigned char)(high * 16 + digit);
			produced++;
			high = -1;
		}
	}

	/* A last lone digit is followed by 0. */
	if (high >= 0) {
		buffer[produced] = (unsigned char)(high * 16);
		produced++;
	}

	/* Succeeded: the decoded bytes. */
	*output = buffer;
	*output_size = produced;
	return 0;
}

/*
 * Decodes ASCII85Decode: groups of five characters '!'..'u' make four
 * bytes, 'z' makes four zero bytes, white space is ignored and "~>" ends
 * the data; a last partial group of n characters makes n - 1 bytes.
 *
 * A character outside the alphabet ends the data where it stands, keeping
 * what was decoded before it, as other readers do.
 */
static int
decode_ascii85(
	const unsigned char *input,
	size_t input_size,
	unsigned char **output,
	size_t *output_size)
{
	unsigned char *buffer;
	unsigned char character;
	unsigned long group;
	size_t index;
	size_t produced;
	int digits;

	/*
	 * Allocates for the most bytes the input can give: four per 'z',
	 * which is more than the four per five digits.
	 */
	if (input_size > PDF_FILTER_OUTPUT_MAX / 4)
		return ENOMEM;
	buffer = malloc(input_size * 4 + 4);
	if (buffer == NULL)
		return ENOMEM;

	/* Reads the characters, a group of five at a time. */
	produced = 0;
	group = 0;
	digits = 0;
	for (index = 0; index < input_size; index++) {
		character = input[index];

		/* The end marker ends the data. */
		if (character == '~')
			break;

		/* White space is skipped. */
		if (character <= ' ')
			continue;

		/* 'z' stands for four zero bytes, and only between groups. */
		if (character == 'z') {
			if (digits != 0)
				break;
			memset(buffer + produced, 0, 4);
			produced += 4;
			continue;
		}

		/* Anything else outside the alphabet ends the data. */
		if (character > 'u')
			break;

		/* Adds the digit; the fifth completes a group of four bytes. */
		group = group * 85 + (unsigned long)(character - '!');
		digits++;
		if (digits == 5) {
			put_ascii85_group(group, 4, buffer, &produced);
			group = 0;
			digits = 0;
		}
	}

	/* A last partial group is padded with the highest digit and gives one byte fewer than its digits. */
	if (digits > 1) {
		index = (size_t)digits;
		while (index < 5) {
			group = group * 85 + 84;
			index++;
		}

		/* Stores the bytes the digits given make. */
		put_ascii85_group(group, digits - 1, buffer, &produced);
	}

	/* Succeeded: the decoded bytes. */
	*output = buffer;
	*output_size = produced;
	return 0;
}

/*
 * Stores the first bytes of a base-85 group, most significant first.
 *
 * A group above 2^32 - 1 (possible only in damaged data) keeps its low 32
 * bits.
 */
static void
put_ascii85_group(
	unsigned long group,
	int bytes,
	unsigned char *buffer,
	size_t *produced)
{
	int index;

	/* Stores the bytes from the most significant. */
	group &= 0xffffffffUL;
	for (index = 0; index < bytes; index++) {
		buffer[*produced] = (unsigned char)((group >> (24 - 8 * index)) & 0xff);
		(*produced)++;
	}
}

/* Decodes LZWDecode with its /EarlyChange and predictor parameters. */
static int
decode_lzw(
	struct pdf_document *document,
	struct pdf_object *parameters,
	const unsigned char *input,
	size_t input_size,
	unsigned char **output,
	size_t *output_size)
{
	struct pdf_filter_output decoded;
	struct pdf_predictor predictor;
	long early_change;
	int error;

	/* Reads the predictor before decoding, so that a bad one costs nothing. */
	error = read_predictor(document, parameters, &predictor);
	if (error != 0)
		return error;

	/* Reads when the code width grows: one code early (1, the default) or exactly at the table's size (0). */
	error = read_parameter(document, parameters, "EarlyChange", 1, &early_change);
	if (error != 0)
		return error;
	if (early_change != 0)
		early_change = 1;

	/* Decodes the codes. */
	memset(&decoded, 0, sizeof(decoded));
	error = read_lzw(input, input_size, early_change, &decoded);
	if (error != 0) {
		free(decoded.data);
		return error;
	}

	/* Undoes the predictor in place. */
	error = undo_predictor(&predictor, decoded.data, &decoded.size);
	if (error != 0) {
		free(decoded.data);
		return error;
	}

	/* Succeeded: the decoded bytes. */
	*output = decoded.data;
	*output_size = decoded.size;
	return 0;
}

/*
 * Reads LZW codes of 9 to 12 bits, most significant bit first, into an
 * output.
 *
 * A code the table cannot have yet, or the input running out, ends the
 * data and keeps what was decoded before it.
 */
static int
read_lzw(
	const unsigned char *input,
	size_t input_size,
	long early_change,
	struct pdf_filter_output *output)
{
	struct pdf_lzw_entry *table;
	unsigned long bit_buffer;
	unsigned long mask;
	unsigned int code;
	unsigned int previous;
	unsigned int next_code;
	unsigned int width;
	size_t index;
	int bits;
	int have_previous;

	/* Allocates the table. */
	table = malloc(sizeof(*table) * PDF_LZW_TABLE);
	if (table == NULL)
		return ENOMEM;

	/* The first 256 entries are the single bytes. */
	for (code = 0; code < 256; code++) {
		table[code].prefix = 0;
		table[code].last = (unsigned char)code;
		table[code].first = (unsigned char)code;
		table[code].length = 1;
	}

	/* Reads the codes until the end code, the end of the input, or damage. */
	next_code = PDF_LZW_FIRST;
	width = 9;
	previous = 0;
	have_previous = 0;
	bit_buffer = 0;
	bits = 0;
	index = 0;
	for (;;) {
		/* Gathers enough bits for one code; a code cut short by the end of the input ends the data. */
		while (bits < (int)width && index < input_size) {
			bit_buffer = ((bit_buffer << 8) | input[index]) & 0xffffffUL;
			bits += 8;
			index++;
		}

		/* Takes the code from the top of the gathered bits. */
		if (bits < (int)width)
			break;
		mask = (1UL << width) - 1;
		code = (unsigned int)((bit_buffer >> (bits - (int)width)) & mask);
		bits -= (int)width;

		/* The clear code empties the table and returns to 9-bit codes. */
		if (code == PDF_LZW_CLEAR) {
			next_code = PDF_LZW_FIRST;
			width = 9;
			have_previous = 0;
			continue;
		}

		/* The end code ends the data. */
		if (code == PDF_LZW_END)
			break;

		/* A code beyond the entry being defined is damage. */
		if (code > next_code)
			break;

		/* Defines the next entry from the previous string; a first code after a clear must be a single byte. */
		if (have_previous) {
			add_lzw_entry(table, &next_code, previous, code);
		} else if (code >= 256) {
			break;
		}

		/* Writes this code's string. */
		put_lzw_string(table, code, output);
		if (output->error != 0)
			break;
		previous = code;
		have_previous = 1;

		/* Widens the codes when the table reaches the next power of two (one code early by default). */
		if (width < PDF_LZW_WIDTH_MAX) {
			if (next_code + (unsigned int)early_change >= (1U << width))
				width++;
		}
	}

	/* The table goes; the output keeps the bytes. */
	free(table);

	/* An empty result still gets a buffer, so that the caller has one to free. */
	output_reserve(output, 1);

	/* Refuses output past the decode limit. */
	if (output->error != 0)
		return output->error;

	/* Succeeded: the decoded bytes are in the output. */
	return 0;
}

/*
 * Defines the next LZW entry: the previous code's string followed by the
 * first byte of the current code's string.
 *
 * When the current code is the entry being defined, its first byte is the
 * previous string's first byte.  A full table defines nothing more.
 */
static void
add_lzw_entry(
	struct pdf_lzw_entry *table,
	unsigned int *next_code,
	unsigned int previous,
	unsigned int code)
{
	struct pdf_lzw_entry *entry;

	/* A full table keeps its entries until the next clear code. */
	if (*next_code >= PDF_LZW_TABLE)
		return;

	/* The new string: the previous one and one byte more. */
	entry = &table[*next_code];
	entry->prefix = (unsigned short)previous;
	entry->first = table[previous].first;
	entry->length = table[previous].length + 1;

	/* The appended byte is the first of the current string, which for the entry itself is the previous string's first. */
	if (code == *next_code) {
		entry->last = table[previous].first;
	} else {
		entry->last = table[code].first;
	}

	/* The entry is defined; the next code is the one after it. */
	(*next_code)++;
}

/* Appends one LZW table entry's string to an output, walking its prefixes from the last byte. */
static void
put_lzw_string(
	const struct pdf_lzw_entry *table,
	unsigned int code,
	struct pdf_filter_output *output)
{
	unsigned long length;
	unsigned long position;

	/* Makes room for the whole string. */
	length = table[code].length;
	output_reserve(output, length);
	if (output->error != 0)
		return;

	/* Writes the bytes backwards from the last one. */
	position = length;
	while (position > 0) {
		position--;
		output->data[output->size + position] = table[code].last;
		code = table[code].prefix;
	}

	/* The string is the output's newest part. */
	output->size += length;
}

/*
 * Decodes RunLengthDecode: a length byte n below 128 copies the next
 * n + 1 bytes, above 128 repeats the next byte 257 - n times, and 128 ends
 * the data.
 */
static int
decode_run_length(
	const unsigned char *input,
	size_t input_size,
	unsigned char **output,
	size_t *output_size)
{
	struct pdf_filter_output decoded;
	size_t index;
	size_t count;
	unsigned char length;

	/* Reads the runs until the end marker, the end of the input, or the decode limit. */
	memset(&decoded, 0, sizeof(decoded));
	index = 0;
	while (index < input_size) {
		length = input[index];
		index++;

		/* 128 ends the data. */
		if (length == 128)
			break;

		/* A literal run copies what the input still has of it. */
		if (length < 128) {
			count = (size_t)length + 1;
			if (count > input_size - index)
				count = input_size - index;
			output_reserve(&decoded, count);
			if (decoded.error != 0)
				break;
			memcpy(decoded.data + decoded.size, input + index, count);
			decoded.size += count;
			index += count;
			continue;
		}

		/* A repeated run needs its byte. */
		if (index >= input_size)
			break;
		count = 257 - (size_t)length;
		output_reserve(&decoded, count);
		if (decoded.error != 0)
			break;
		memset(decoded.data + decoded.size, input[index], count);
		decoded.size += count;
		index++;
	}

	/* An empty result still gets a buffer, so that the caller has one to free. */
	output_reserve(&decoded, 1);

	/* Refuses output past the decode limit. */
	if (decoded.error != 0) {
		free(decoded.data);
		return decoded.error;
	}

	/* Succeeded: the decoded bytes. */
	*output = decoded.data;
	*output_size = decoded.size;
	return 0;
}

/*
 * Makes room for more bytes in a growing output, up to the decode limit;
 * a failure is kept in output->error.
 */
static void
output_reserve(
	struct pdf_filter_output *output,
	size_t more)
{
	unsigned char *grown;
	size_t capacity;

	/* A failed output stays failed. */
	if (output->error != 0)
		return;

	/* Refuses to pass the decode limit. */
	if (more > PDF_FILTER_OUTPUT_MAX - output->size) {
		output->error = ENOMEM;
		return;
	}

	/* The room is already there. */
	if (output->size + more <= output->capacity)
		return;

	/* Doubles the capacity until it holds the bytes, bounded by the limit. */
	capacity = output->capacity;
	if (capacity < PDF_FILTER_INITIAL)
		capacity = PDF_FILTER_INITIAL;
	while (capacity < output->size + more)
		capacity *= 2;
	if (capacity > PDF_FILTER_OUTPUT_MAX)
		capacity = PDF_FILTER_OUTPUT_MAX;

	/* Grows the buffer. */
	grown = realloc(output->data, capacity);
	if (grown == NULL) {
		output->error = ENOMEM;
		return;
	}

	/* The grown buffer is the output's. */
	output->data = grown;
	output->capacity = capacity;
}

/* Reads a Flate filter's predictor parameters, with the defaults of the PDF reference. */
static int
read_predictor(
	struct pdf_document *document,
	struct pdf_object *parameters,
	struct pdf_predictor *predictor)
{
	int error;

	/* The predictor, 1 (none) by default. */
	error = read_parameter(document, parameters, "Predictor", 1, &predictor->predictor);
	if (error != 0)
		return error;

	/* The colours a sample has, 1 by default. */
	error = read_parameter(document, parameters, "Colors", 1, &predictor->colors);
	if (error != 0)
		return error;

	/* The bits a colour takes, 8 by default. */
	error = read_parameter(document, parameters, "BitsPerComponent", 8, &predictor->bits);
	if (error != 0)
		return error;

	/* The samples in a row, 1 by default. */
	error = read_parameter(document, parameters, "Columns", 1, &predictor->columns);
	if (error != 0)
		return error;

	/* Refuses parameters no row can be made of. */
	if (predictor->colors < 1 || predictor->colors > 32)
		return PDF_EFORMAT;
	if (predictor->bits != 1 &&
	    predictor->bits != 2 &&
	    predictor->bits != 4 &&
	    predictor->bits != 8 &&
	    predictor->bits != 16)
		return PDF_EFORMAT;
	if (predictor->columns < 1 || predictor->columns > 1048576)
		return PDF_EFORMAT;

	/* Succeeded: the predictor is known. */
	return 0;
}

/* Reads one integer of a parameter dictionary, or a default when it is absent. */
static int
read_parameter(
	struct pdf_document *document,
	struct pdf_object *parameters,
	const char *key,
	long fallback,
	long *value)
{
	struct pdf_object *found;
	int error;

	/* Absent parameters give the default. */
	*value = fallback;
	if (parameters == NULL)
		return 0;
	if (parameters->type != PDF_OBJECT_DICTIONARY)
		return 0;

	/* Finds the key's value. */
	error = pdf_reader_resolve_key(document, parameters, key, &found);
	if (error != 0)
		return error;

	/* A missing key gives the default; anything but an integer is malformed. */
	if (found->type == PDF_OBJECT_NULL)
		return 0;
	if (found->type != PDF_OBJECT_INTEGER)
		return PDF_EFORMAT;

	/* Succeeded: the parameter's value. */
	*value = found->integer;
	return 0;
}

/* Reads one flag of a parameter dictionary, or a default when it is absent. */
static int
read_flag(
	struct pdf_document *document,
	struct pdf_object *parameters,
	const char *key,
	int fallback,
	int *value)
{
	struct pdf_object *found;
	int error;

	/* Absent parameters give the default. */
	*value = fallback;
	if (parameters == NULL)
		return 0;
	if (parameters->type != PDF_OBJECT_DICTIONARY)
		return 0;

	/* Finds the key's value. */
	error = pdf_reader_resolve_key(document, parameters, key, &found);
	if (error != 0)
		return error;

	/* A missing key gives the default; anything but a boolean is malformed. */
	if (found->type == PDF_OBJECT_NULL)
		return 0;
	if (found->type != PDF_OBJECT_BOOLEAN)
		return PDF_EFORMAT;

	/* Succeeded: the flag's value. */
	*value = found->boolean;
	return 0;
}

/*
 * Reads a fax filter's parameters with the defaults of the PDF reference.
 * Without /Rows the image's /Height (or an inline image's /H) is the
 * number of rows, so that rows the data lacks come out white rather than
 * as missing samples.
 */
static int
read_ccitt(
	struct pdf_document *document,
	const struct pdf_object *stream,
	struct pdf_object *parameters,
	struct pdf_ccitt_parameters *ccitt)
{
	struct pdf_object *height;
	int error;

	/* The coding: negative Group 4, zero Group 3 one-dimensional, positive Group 3 mixed. */
	error = read_parameter(document, parameters, "K", 0, &ccitt->k);
	if (error != 0)
		return error;

	/* The row's width, 1728 (a fax line) by default. */
	error = read_parameter(document, parameters, "Columns", 1728, &ccitt->columns);
	if (error != 0)
		return error;

	/* The rows, 0 (as many as the data holds) by default. */
	error = read_parameter(document, parameters, "Rows", 0, &ccitt->rows);
	if (error != 0)
		return error;

	/* The damaged rows tolerated, none by default. */
	error = read_parameter(document, parameters, "DamagedRowsBeforeError", 0, &ccitt->damaged_rows);
	if (error != 0)
		return error;

	/* Whether rows have end-of-line codes, false by default. */
	error = read_flag(document, parameters, "EndOfLine", 0, &ccitt->end_of_line);
	if (error != 0)
		return error;

	/* Whether rows start on a byte, false by default. */
	error = read_flag(document, parameters, "EncodedByteAlign", 0, &ccitt->byte_align);
	if (error != 0)
		return error;

	/* Whether the data ends with an end-of-block code, true by default. */
	error = read_flag(document, parameters, "EndOfBlock", 1, &ccitt->end_of_block);
	if (error != 0)
		return error;

	/* Whether 1 is black, false by default. */
	error = read_flag(document, parameters, "BlackIs1", 0, &ccitt->black_is_1);
	if (error != 0)
		return error;

	/* Rows given are the image's. */
	if (ccitt->rows > 0)
		return 0;

	/* Without them, the image's height: an image XObject's /Height. */
	error = pdf_reader_resolve_key(document, stream, "Height", &height);
	if (error != 0)
		return error;

	/* Or an inline image's /H. */
	if (height->type == PDF_OBJECT_NULL) {
		error = pdf_reader_resolve_key(document, stream, "H", &height);
		if (error != 0)
			return error;
	}

	/* A positive height is the number of rows. */
	if (height->type == PDF_OBJECT_INTEGER && height->integer > 0)
		ccitt->rows = height->integer;

	/* Succeeded: the parameters are read. */
	return 0;
}

/* Undoes a predictor in place; the PNG ones also drop each row's type byte. */
static int
undo_predictor(
	const struct pdf_predictor *predictor,
	unsigned char *data,
	size_t *size)
{
	int error;

	/* No predictor leaves the bytes as they are. */
	if (predictor->predictor == 1)
		return 0;

	/* The TIFF predictor. */
	if (predictor->predictor == 2) {
		error = undo_tiff(predictor, data, *size);
		if (error != 0)
			return error;
		return 0;
	}

	/* The PNG predictors, whichever one the parameter names (each row names its own). */
	if (predictor->predictor >= 10 && predictor->predictor <= 15) {
		error = undo_png(predictor, data, size);
		if (error != 0)
			return error;
		return 0;
	}

	/* Any other predictor is malformed. */
	return PDF_EFORMAT;
}

/*
 * Undoes the PNG predictors row by row, compacting the rows over their
 * type bytes; an incomplete last row is dropped.
 */
static int
undo_png(
	const struct pdf_predictor *predictor,
	unsigned char *data,
	size_t *size)
{
	unsigned char *row;
	const unsigned char *previous;
	unsigned char left;
	unsigned char above;
	unsigned char upper_left;
	size_t row_bytes;
	size_t sample_bytes;
	size_t rows;
	size_t index;
	size_t column;
	unsigned type;

	/* The bytes of a row without its type byte, and of one whole sample (at least one). */
	row_bytes = ((size_t)predictor->colors * (size_t)predictor->bits * (size_t)predictor->columns + 7) / 8;
	sample_bytes = ((size_t)predictor->colors * (size_t)predictor->bits + 7) / 8;
	rows = *size / (row_bytes + 1);

	/* Decodes each row into its place in the compacted output. */
	previous = NULL;
	for (index = 0; index < rows; index++) {
		/* Moves the row's bytes over the type bytes before it. */
		type = data[index * (row_bytes + 1)];
		row = data + index * row_bytes;
		memmove(row, data + index * (row_bytes + 1) + 1, row_bytes);

		/* Adds back what each byte was predicted from. */
		for (column = 0; column < row_bytes; column++) {
			/* The bytes to the left, above and above-left, zero outside the image. */
			left = 0;
			if (column >= sample_bytes)
				left = row[column - sample_bytes];
			above = 0;
			if (previous != NULL)
				above = previous[column];
			upper_left = 0;
			if (previous != NULL && column >= sample_bytes)
				upper_left = previous[column - sample_bytes];

			/* Adds the prediction of the row's type. */
			switch (type) {
			case PDF_PNG_NONE:
				break;
			case PDF_PNG_SUB:
				row[column] = (unsigned char)(row[column] + left);
				break;
			case PDF_PNG_UP:
				row[column] = (unsigned char)(row[column] + above);
				break;
			case PDF_PNG_AVERAGE:
				row[column] = (unsigned char)(row[column] + (unsigned char)(((unsigned)left + (unsigned)above) / 2));
				break;
			case PDF_PNG_PAETH:
				row[column] = (unsigned char)(row[column] + paeth(left, above, upper_left));
				break;
			default:
				/* An unknown type is malformed. */
				return PDF_EFORMAT;
			}
		}

		/* The row is the next row's row above. */
		previous = row;
	}

	/* Succeeded: the rows without their type bytes. */
	*size = rows * row_bytes;
	return 0;
}

/* Undoes the TIFF predictor, which only 8-bit samples use here. */
static int
undo_tiff(
	const struct pdf_predictor *predictor,
	unsigned char *data,
	size_t size)
{
	size_t row_bytes;
	size_t colors;
	size_t start;
	size_t index;

	/* Other sample sizes are not read yet. */
	if (predictor->bits != 8)
		return ENOTSUP;

	/* Adds each sample to the one of the same colour to its left, row by row. */
	colors = (size_t)predictor->colors;
	row_bytes = colors * (size_t)predictor->columns;
	for (start = 0; start + row_bytes <= size; start += row_bytes) {
		for (index = colors; index < row_bytes; index++)
			data[start + index] = (unsigned char)(data[start + index] + data[start + index - colors]);
	}

	/* Succeeded: the samples are restored. */
	return 0;
}

/* Chooses the Paeth predictor of PNG: the neighbour closest to left + above - upper left. */
static unsigned char
paeth(
	unsigned char left,
	unsigned char above,
	unsigned char upper_left)
{
	int estimate;
	int to_left;
	int to_above;
	int to_upper_left;

	/* The distances of the three neighbours to the estimate. */
	estimate = (int)left + (int)above - (int)upper_left;
	to_left = abs(estimate - (int)left);
	to_above = abs(estimate - (int)above);
	to_upper_left = abs(estimate - (int)upper_left);

	/* The left one wins ties, then the one above. */
	if (to_left <= to_above && to_left <= to_upper_left)
		return left;
	if (to_above <= to_upper_left)
		return above;

	/* The upper left one is the closest. */
	return upper_left;
}
