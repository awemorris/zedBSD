/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The packets of a container made into what a decoder reads without its
 * codec's private data (WS122 p004): the player gives libavcodec no
 * extradata (that would need the decoder context's fields), so the
 * configuration travels in the stream itself.
 *
 *   - H.264 and H.265 in MP4 and Matroska are NAL units each after a length
 *     of 1, 2 or 4 bytes, with the parameter sets in the avcC or hvcC
 *     record.  They become an Annex B stream: each NAL unit after the start
 *     code 00 00 00 01, and before a key frame the parameter sets.
 *   - AAC is raw access units with an AudioSpecificConfig.  Each unit gets
 *     an ADTS header made from the configuration.
 *   - MPEG-4 Part 2 carries its VOS and VOL headers in the private data;
 *     they go before each key frame.
 *
 * Everything here works on bytes alone and is tested on the host
 * (once plan/ws122/tests/host-bitstream.c, in the git history).  Moved into libmedia from Video
 * Player (ws177-p031).
 */

#include "private.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

/* The start code before each NAL unit of an Annex B stream. */
static const unsigned char bitstream_start[4] = { 0x00U, 0x00U, 0x00U, 0x01U };

/* The sampling rates an ADTS header can name, by index. */
static const uint32_t bitstream_rates[13] = {
	96000U, 88200U, 64000U, 48000U, 44100U, 32000U, 24000U, 22050U, 16000U, 12000U, 11025U, 8000U, 7350U
};

static int bitstream_put(struct app_bitstream *stream, const unsigned char *bytes, size_t size);
static int bitstream_avcc(struct app_bitstream *stream, const unsigned char *data, size_t size);
static int bitstream_hvcc(struct app_bitstream *stream, const unsigned char *data, size_t size);
static int bitstream_asc(struct app_bitstream *stream, const unsigned char *data, size_t size);

/*
 * Prepares the conversion of a track's packets from its codec and private
 * data.  Returns 0, or EINVAL for private data that cannot be read.
 */
int
app_bitstream_open(
	struct app_bitstream *stream,
	unsigned codec,
	const unsigned char *private_data,
	size_t private_size)
{
	int error;

	/* Nothing yet: packets pass as they are. */
	memset(stream, 0, sizeof(*stream));
	stream->codec = codec;
	if (private_data == NULL || private_size == 0U)
		return 0;

	/* Each codec's record. */
	switch (codec) {
	case MEDIA_CODEC_H264:
		error = bitstream_avcc(stream, private_data, private_size);
		break;
	case MEDIA_CODEC_HEVC:
		error = bitstream_hvcc(stream, private_data, private_size);
		break;
	case MEDIA_CODEC_AAC:
		error = bitstream_asc(stream, private_data, private_size);
		break;
	case MEDIA_CODEC_MPEG4:
		/* The VOS and VOL headers as they are, before each key frame. */
		error = bitstream_put(stream, private_data, private_size);
		if (error == 0) {
			stream->prefix = stream->output;
			stream->prefix_size = stream->output_size;
			stream->output = NULL;
			stream->output_size = 0;
			stream->output_room = 0;
		}

		/* Kept as the prefix. */
		break;
	default:
		error = 0;
		break;
	}

	/* Reports a record that could not be read. */
	if (error != 0) {
		app_bitstream_close(stream);
		return error;
	}

	/* Succeeded: packets are converted from here on. */
	return 0;
}

/*
 * Converts one packet.  The result stays valid until the next call or the
 * close.  Returns 0, EINVAL for a packet whose lengths run past its end,
 * or ENOMEM.
 */
int
app_bitstream_convert(
	struct app_bitstream *stream,
	const unsigned char *data,
	size_t size,
	int keyframe,
	const unsigned char **result,
	size_t *result_size)
{
	unsigned char header[7];
	uint32_t frame_length;
	size_t offset;
	size_t length;
	unsigned index;
	int error;

	/* What is built goes. */
	stream->output_size = 0;

	/* The parameter sets or headers before a key frame. */
	if (keyframe && stream->prefix_size != 0U) {
		error = bitstream_put(stream, stream->prefix, stream->prefix_size);
		if (error != 0)
			return error;
	}

	/* H.264 and H.265 with lengths: each NAL unit after a start code. */
	if (stream->length_size != 0U) {
		offset = 0;
		while (offset < size) {
			if (size - offset < stream->length_size)
				return EINVAL;
			length = 0;
			for (index = 0; index < stream->length_size; index++)
				length = (length << 8) | data[offset + index];
			offset += stream->length_size;
			if (length > size - offset)
				return EINVAL;
			error = bitstream_put(stream, bitstream_start, sizeof(bitstream_start));
			if (error != 0)
				return error;
			error = bitstream_put(stream, data + offset, length);
			if (error != 0)
				return error;
			offset += length;
		}

		/* The NAL units, ready. */
		*result = stream->output;
		*result_size = stream->output_size;
		return 0;
	}

	/* AAC with a configuration: an ADTS header before the unit. */
	if (stream->adts) {
		frame_length = (uint32_t)size + 7U;
		if (frame_length > 0x1fffU)
			return EINVAL;
		header[0] = 0xffU;
		header[1] = 0xf1U;
		header[2] = (unsigned char)(((stream->adts_profile & 0x3U) << 6) | ((stream->adts_rate_index & 0xfU) << 2) | ((stream->adts_channels >> 2) & 0x1U));
		header[3] = (unsigned char)(((stream->adts_channels & 0x3U) << 6) | ((frame_length >> 11) & 0x3U));
		header[4] = (unsigned char)((frame_length >> 3) & 0xffU);
		header[5] = (unsigned char)(((frame_length & 0x7U) << 5) | 0x1fU);
		header[6] = 0xfcU;
		error = bitstream_put(stream, header, sizeof(header));
		if (error != 0)
			return error;
	}

	/* The packet's own bytes after whatever went before them. */
	if (stream->output_size == 0U) {
		*result = data;
		*result_size = size;
		return 0;
	}

	/* Otherwise after them. */
	error = bitstream_put(stream, data, size);
	if (error != 0)
		return error;

	/* Succeeded: the converted packet. */
	*result = stream->output;
	*result_size = stream->output_size;
	return 0;
}

/*
 * Frees what the conversion holds.
 */
void
app_bitstream_close(
	struct app_bitstream *stream)
{
	/* The prefix and the output. */
	free(stream->prefix);
	free(stream->output);
	memset(stream, 0, sizeof(*stream));
}

/* Appends bytes to the output, making room; 0 or ENOMEM. */
static int
bitstream_put(
	struct app_bitstream *stream,
	const unsigned char *bytes,
	size_t size)
{
	unsigned char *grown;
	size_t room;

	/* Room, doubled as it is needed (64 MiB at most, the largest packet mediafile hands out with its prefix). */
	if (size > MEDIA_BITSTREAM_MAX || stream->output_size > MEDIA_BITSTREAM_MAX - size)
		return ENOMEM;
	if (stream->output_size + size > stream->output_room) {
		room = stream->output_room;
		if (room == 0U)
			room = 4096U;
		while (room < stream->output_size + size)
			room *= 2U;
		grown = realloc(stream->output, room);
		if (grown == NULL)
			return ENOMEM;
		stream->output = grown;
		stream->output_room = room;
	}

	/* The bytes. */
	memcpy(stream->output + stream->output_size, bytes, size);
	stream->output_size += size;
	return 0;
}

/* Reads an avcC record: the length's size and the SPS and PPS as Annex B; 0 or EINVAL. */
static int
bitstream_avcc(
	struct app_bitstream *stream,
	const unsigned char *data,
	size_t size)
{
	size_t offset;
	size_t length;
	unsigned count;
	unsigned set;
	unsigned kind;
	int error;

	/* The version, the length's size, and the SPS that follow. */
	if (size < 7U || data[0] != 1U)
		return EINVAL;
	stream->length_size = (unsigned)(data[4] & 0x3U) + 1U;
	if (stream->length_size == 3U)
		return EINVAL;
	offset = 5;

	/* The SPS, then the PPS: a count, then each set after its 16-bit length. */
	for (kind = 0; kind < 2U; kind++) {
		if (offset >= size)
			return EINVAL;
		count = data[offset];
		if (kind == 0U)
			count &= 0x1fU;
		offset++;
		for (set = 0; set < count; set++) {
			if (size - offset < 2U)
				return EINVAL;
			length = ((size_t)data[offset] << 8) | data[offset + 1U];
			offset += 2U;
			if (length > size - offset)
				return EINVAL;
			error = bitstream_put(stream, bitstream_start, sizeof(bitstream_start));
			if (error != 0)
				return error;
			error = bitstream_put(stream, data + offset, length);
			if (error != 0)
				return error;
			offset += length;
		}
	}

	/* The sets become the prefix of each key frame. */
	stream->prefix = stream->output;
	stream->prefix_size = stream->output_size;
	stream->output = NULL;
	stream->output_size = 0;
	stream->output_room = 0;

	/* Succeeded. */
	return 0;
}

/* Reads an hvcC record: the length's size and the VPS, SPS and PPS as Annex B; 0 or EINVAL. */
static int
bitstream_hvcc(
	struct app_bitstream *stream,
	const unsigned char *data,
	size_t size)
{
	size_t offset;
	size_t length;
	unsigned arrays;
	unsigned array;
	unsigned count;
	unsigned unit;
	int error;

	/* The fixed part: 22 bytes, the length's size in the low bits of the 22nd, the arrays' count in the 23rd. */
	if (size < 23U || data[0] != 1U)
		return EINVAL;
	stream->length_size = (unsigned)(data[21] & 0x3U) + 1U;
	if (stream->length_size == 3U)
		return EINVAL;
	arrays = data[22];
	offset = 23;

	/* Each array: its type, a count, then each unit after its 16-bit length. */
	for (array = 0; array < arrays; array++) {
		if (size - offset < 3U)
			return EINVAL;
		count = ((unsigned)data[offset + 1U] << 8) | data[offset + 2U];
		offset += 3U;
		for (unit = 0; unit < count; unit++) {
			if (size - offset < 2U)
				return EINVAL;
			length = ((size_t)data[offset] << 8) | data[offset + 1U];
			offset += 2U;
			if (length > size - offset)
				return EINVAL;
			error = bitstream_put(stream, bitstream_start, sizeof(bitstream_start));
			if (error != 0)
				return error;
			error = bitstream_put(stream, data + offset, length);
			if (error != 0)
				return error;
			offset += length;
		}
	}

	/* The units become the prefix of each key frame. */
	stream->prefix = stream->output;
	stream->prefix_size = stream->output_size;
	stream->output = NULL;
	stream->output_size = 0;
	stream->output_room = 0;

	/* Succeeded. */
	return 0;
}

/*
 * Reads an AudioSpecificConfig for the ADTS header: the object type (as the
 * profile, 1 to 4), the rate's index and the channels.  An object type ADTS
 * cannot name (HE-AAC's 5 and 29 say SBR) is given as AAC LC at the core's
 * rate, which the decoder plays with SBR found in the stream.
 */
static int
bitstream_asc(
	struct app_bitstream *stream,
	const unsigned char *data,
	size_t size)
{
	uint32_t rate;
	unsigned object;
	unsigned index;
	unsigned channels;
	unsigned found;

	/* The object type (5 bits), the rate's index (4 bits), and the channels (4 bits). */
	if (size < 2U)
		return EINVAL;
	object = (unsigned)(data[0] >> 3);
	index = (unsigned)(((data[0] & 0x7U) << 1) | (data[1] >> 7));
	channels = (unsigned)((data[1] >> 3) & 0xfU);
	if (object == 31U)
		return EINVAL;

	/* An explicit rate (index 15) is found among the ones ADTS names. */
	if (index == 15U) {
		if (size < 5U)
			return EINVAL;
		rate = ((uint32_t)(data[1] & 0x7fU) << 17) | ((uint32_t)data[2] << 9) | ((uint32_t)data[3] << 1) | ((uint32_t)data[4] >> 7);
		found = 13U;
		for (index = 0; index < 13U; index++) {
			if (bitstream_rates[index] == rate) {
				found = index;
				break;
			}
		}

		/* A rate ADTS cannot name. */
		if (found == 13U)
			return EINVAL;
		index = found;
	}

	/* An index past the table. */
	if (index > 12U)
		return EINVAL;

	/* The profile ADTS carries: the object type less one, LC for any it cannot name. */
	if (object < 1U || object > 4U)
		object = 2U;
	stream->adts = 1;
	stream->adts_profile = object - 1U;
	stream->adts_rate_index = index;
	stream->adts_channels = channels;

	/* Succeeded. */
	return 0;
}
