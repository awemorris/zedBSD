/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * bluetoothd's SDP server (ws197-p002, see sdps.h).
 *
 * SDP is big-endian.  Every request's data elements are checked with the
 * client's parser (sdp.c) before they are read.  A whole answer is built
 * once and cut to the peer's MTU and the count it asked for; the
 * continuation state bluetoothd gives is three bytes, the database's
 * version and the offset into the whole answer, and is honoured only for
 * the same request against the same records.
 */

#include "userland/base/bluetoothd/sdps.h"
#include "userland/base/bluetoothd/sdp.h"

#include <errno.h>
#include <string.h>

/* The PDUs (Core 5.4 Vol 3 Part B section 4.2). */
#define SDPS_ERROR_RESPONSE		0x01U
#define SDPS_SEARCH_REQUEST		0x02U
#define SDPS_SEARCH_RESPONSE		0x03U
#define SDPS_ATTRIBUTE_REQUEST		0x04U
#define SDPS_ATTRIBUTE_RESPONSE		0x05U
#define SDPS_SEARCH_ATTRIBUTE_REQUEST	0x06U
#define SDPS_SEARCH_ATTRIBUTE_RESPONSE	0x07U

/* A PDU's header (ID, transaction, parameters' length), the longest continuation state, and bluetoothd's own (version and offset). */
#define SDPS_HEADER			5U
#define SDPS_CONTINUATION_MAX		16U
#define SDPS_CONTINUATION		3U

/* The least MaximumAttributeByteCount (Core 5.4 Vol 3 Part B section 4.6.1), and the least MTU of BR/EDR. */
#define SDPS_BYTES_LEAST		7U
#define SDPS_MTU_LEAST			48U

/* The attributes every record carries: its handle, and its browse group (the public browse root). */
#define SDPS_ATTRIBUTE_HANDLE		0x0000U
#define SDPS_ATTRIBUTE_BROWSE		0x0005U
#define SDPS_PUBLIC_BROWSE_ROOT		0x1002U

/* A data element's header bytes: an unsigned 16-bit, a 32-bit, a 16-bit UUID, a sequence with a 16-bit length. */
#define SDPS_ELEMENT_UINT8		0x08U
#define SDPS_ELEMENT_UINT16		0x09U
#define SDPS_ELEMENT_UINT32		0x0aU
#define SDPS_ELEMENT_UUID16		0x19U
#define SDPS_ELEMENT_UUID128		0x1cU
#define SDPS_ELEMENT_TEXT8		0x25U
#define SDPS_ELEMENT_SEQUENCE16		0x36U

/* How deep a record's UUIDs are looked for. */
#define SDPS_DEPTH_MAX			8U

/* One attribute ID or range of a request: from first to last, inclusive. */
struct sdps_range {
	uint16_t first;
	uint16_t last;
};

/*
 * A request read: the UUIDs of its search pattern (in their 128-bit
 * form), its record handle, its most records or bytes, its attribute
 * ranges, and its continuation state (pointing into the PDU).
 */
struct sdps_request {
	unsigned uuid_count;
	uint8_t uuids[BTD_SDPS_PATTERN_MAX][16];
	uint32_t handle;
	size_t most;
	unsigned range_count;
	struct sdps_range ranges[BTD_SDPS_RANGES_MAX];
	size_t parameters_length;
	size_t continuation_length;
	const uint8_t *continuation;
};

static void sdps_raw(struct btd_sdp_writer *writer, const uint8_t *data, size_t length);
static uint16_t sdps_be16(const uint8_t *bytes);
static uint32_t sdps_be32(const uint8_t *bytes);
static void sdps_put16(uint8_t *bytes, size_t value);
static void sdps_put32(uint8_t *bytes, uint32_t value);
static int sdps_uuid128(const struct btd_sdp_element *element, uint8_t *uuid);
static int sdps_pattern(const uint8_t *data, size_t length, size_t *offset, struct sdps_request *request);
static int sdps_ranges(const uint8_t *data, size_t length, size_t *offset, struct sdps_request *request);
static int sdps_continuation(const uint8_t *data, size_t length, size_t offset, struct sdps_request *request);
static int sdps_parse(uint8_t pdu, const uint8_t *data, size_t length, struct sdps_request *request);
static int sdps_matches(const struct btd_sdps_record *record, const struct sdps_request *request);
static int sdps_contains(const uint8_t *data, size_t length, const uint8_t *uuid, unsigned depth);
static int sdps_wanted(const struct sdps_request *request, uint16_t id);
static int sdps_attributes(const struct btd_sdps_record *record, const struct sdps_request *request, struct btd_sdp_writer *writer);
static int sdps_build(struct btd_sdps *server, uint8_t pdu, const struct sdps_request *request);
static const struct btd_sdps_record *sdps_record(const struct btd_sdps_db *db, uint32_t handle);
static size_t sdps_error(uint8_t *answer, size_t size, uint16_t transaction, uint16_t code);

/*
 * Starts writing data elements into a buffer.
 */
void
btd_sdp_writer_init(
	struct btd_sdp_writer *writer,
	uint8_t *bytes,
	size_t size)
{
	/* Empty, no sequence open. */
	memset(writer, 0, sizeof(*writer));
	writer->bytes = bytes;
	writer->size = size;
}

/*
 * Writes an 8-bit unsigned integer element.
 */
void
btd_sdp_put_uint8(
	struct btd_sdp_writer *writer,
	uint8_t value)
{
	uint8_t element[2];

	/* The header and the value. */
	element[0] = SDPS_ELEMENT_UINT8;
	element[1] = value;
	sdps_raw(writer, element, sizeof(element));
}

/*
 * Writes a 16-bit unsigned integer element.
 */
void
btd_sdp_put_uint16(
	struct btd_sdp_writer *writer,
	uint16_t value)
{
	uint8_t element[3];

	/* The header and the value. */
	element[0] = SDPS_ELEMENT_UINT16;
	sdps_put16(element + 1, value);
	sdps_raw(writer, element, sizeof(element));
}

/*
 * Writes a 32-bit unsigned integer element.
 */
void
btd_sdp_put_uint32(
	struct btd_sdp_writer *writer,
	uint32_t value)
{
	uint8_t element[5];

	/* The header and the value. */
	element[0] = SDPS_ELEMENT_UINT32;
	sdps_put32(element + 1, value);
	sdps_raw(writer, element, sizeof(element));
}

/*
 * Writes a 16-bit UUID element.
 */
void
btd_sdp_put_uuid16(
	struct btd_sdp_writer *writer,
	uint16_t value)
{
	uint8_t element[3];

	/* The header and the value. */
	element[0] = SDPS_ELEMENT_UUID16;
	sdps_put16(element + 1, value);
	sdps_raw(writer, element, sizeof(element));
}

/*
 * Writes a 128-bit UUID element (16 bytes, most significant first).
 */
void
btd_sdp_put_uuid128(
	struct btd_sdp_writer *writer,
	const uint8_t *value)
{
	uint8_t header[1];

	/* The header and the value. */
	header[0] = SDPS_ELEMENT_UUID128;
	sdps_raw(writer, header, sizeof(header));
	sdps_raw(writer, value, 16U);
}

/*
 * Writes a text element of at most 255 bytes (a longer text marks the
 * writer).
 */
void
btd_sdp_put_text(
	struct btd_sdp_writer *writer,
	const char *text)
{
	uint8_t header[2];
	size_t length;

	/* A length one byte holds. */
	length = strlen(text);
	if (length > 0xffU) {
		writer->overflow = 1;
		return;
	}

	/* The header and the text. */
	header[0] = SDPS_ELEMENT_TEXT8;
	header[1] = (uint8_t)length;
	sdps_raw(writer, header, sizeof(header));
	sdps_raw(writer, (const uint8_t *)text, length);
}

/*
 * Begins a sequence (its 16-bit length is written when it ends).
 */
void
btd_sdp_begin(
	struct btd_sdp_writer *writer)
{
	uint8_t header[3];

	/* Too deep: marked. */
	if (writer->depth >= BTD_SDP_WRITER_DEPTH) {
		writer->overflow = 1;
		return;
	}

	/* Succeeded: its header, its offset kept. */
	writer->open[writer->depth] = writer->used;
	writer->depth++;
	header[0] = SDPS_ELEMENT_SEQUENCE16;
	header[1] = 0U;
	header[2] = 0U;
	sdps_raw(writer, header, sizeof(header));
}

/*
 * Ends the sequence begun last: its length written.
 */
void
btd_sdp_end(
	struct btd_sdp_writer *writer)
{
	size_t start;
	size_t length;

	/* None begun: marked. */
	if (writer->depth == 0U) {
		writer->overflow = 1;
		return;
	}

	/* The innermost open sequence. */
	writer->depth--;
	start = writer->open[writer->depth];

	/* Something did not fit: the length means nothing. */
	if (writer->overflow)
		return;

	/* The length of what follows its header, within two bytes. */
	length = writer->used - start - 3U;
	if (length > 0xffffU) {
		writer->overflow = 1;
		return;
	}

	/* Succeeded: the length written. */
	sdps_put16(writer->bytes + start + 1U, length);
}

/*
 * Empties the database: no record, the first handle next.
 */
void
btd_sdps_db_init(
	struct btd_sdps_db *db)
{
	/* No record. */
	memset(db, 0, sizeof(*db));
	db->next_handle = BTD_SDPS_HANDLE_FIRST;
}

/*
 * Offers a record: its attribute pairs (16-bit unsigned IDs in ascending
 * order, above the handle's, each followed by its value).  The handle's
 * attribute is put first and the browse group's (the public browse root)
 * in its place unless the pairs carry one.  Returns 0 with the handle,
 * EINVAL for pairs out of order or malformed, E2BIG, or ENOSPC.
 */
int
btd_sdps_register(
	struct btd_sdps_db *db,
	const uint8_t *pairs,
	size_t length,
	uint32_t *handle)
{
	struct btd_sdps_record *record;
	struct btd_sdp_element id;
	struct btd_sdp_element value;
	struct btd_sdp_writer writer;
	unsigned index;
	size_t offset;
	size_t start;
	uint32_t previous;
	uint32_t number;
	int browse_written;
	int error;

	/* The pairs well formed. */
	error = btd_sdp_check(pairs, length);
	if (error != 0)
		return EINVAL;

	/* A free record. */
	record = NULL;
	for (index = 0U; index < BTD_SDPS_RECORDS_MAX; index++) {
		if (!db->records[index].used) {
			record = &db->records[index];
			break;
		}
	}

	/* None free. */
	if (record == NULL)
		return ENOSPC;

	/* The handle's attribute first. */
	btd_sdp_writer_init(&writer, record->attributes, sizeof(record->attributes));
	btd_sdp_put_uint16(&writer, SDPS_ATTRIBUTE_HANDLE);
	btd_sdp_put_uint32(&writer, db->next_handle);

	/* Each pair, in ascending order, the browse group's put before the first ID above its own. */
	browse_written = 0;
	previous = SDPS_ATTRIBUTE_HANDLE;
	offset = 0U;
	while (offset < length) {
		/* The ID: 16-bit unsigned, above the one before. */
		start = offset;
		error = btd_sdp_element(pairs + offset, length - offset, &id);
		if (error != 0 || id.type != BTD_SDP_TYPE_UINT || id.length != 2U)
			return EINVAL;
		number = sdps_be16(id.value);
		if (number <= previous)
			return EINVAL;
		offset += id.size;

		/* Its value. */
		error = btd_sdp_element(pairs + offset, length - offset, &value);
		if (error != 0)
			return EINVAL;
		offset += value.size;

		/* The browse group before an ID above it. */
		if (!browse_written && number > SDPS_ATTRIBUTE_BROWSE) {
			btd_sdp_put_uint16(&writer, SDPS_ATTRIBUTE_BROWSE);
			btd_sdp_begin(&writer);
			btd_sdp_put_uuid16(&writer, SDPS_PUBLIC_BROWSE_ROOT);
			btd_sdp_end(&writer);
			browse_written = 1;
		}

		/* One given by the caller counts. */
		if (number == SDPS_ATTRIBUTE_BROWSE)
			browse_written = 1;

		/* The pair as given. */
		sdps_raw(&writer, pairs + start, id.size + value.size);
		previous = number;
	}

	/* The browse group last when no ID came after it. */
	if (!browse_written) {
		btd_sdp_put_uint16(&writer, SDPS_ATTRIBUTE_BROWSE);
		btd_sdp_begin(&writer);
		btd_sdp_put_uuid16(&writer, SDPS_PUBLIC_BROWSE_ROOT);
		btd_sdp_end(&writer);
	}

	/* Something did not fit. */
	if (writer.overflow)
		return E2BIG;

	/* Succeeded: offered, the database's version moved on. */
	record->used = 1;
	record->handle = db->next_handle;
	record->length = writer.used;
	*handle = db->next_handle;
	db->next_handle++;
	db->version++;
	return 0;
}

/*
 * Withdraws a record.  Returns 0, or ENOENT.
 */
int
btd_sdps_unregister(
	struct btd_sdps_db *db,
	uint32_t handle)
{
	unsigned index;

	/* The record of the handle. */
	for (index = 0U; index < BTD_SDPS_RECORDS_MAX; index++) {
		if (!db->records[index].used || db->records[index].handle != handle)
			continue;

		/* Succeeded: withdrawn, the database's version moved on. */
		memset(&db->records[index], 0, sizeof(db->records[index]));
		db->version++;
		return 0;
	}

	/* No such record. */
	return ENOENT;
}

/*
 * Starts the server of one L2CAP channel (the peer's MTU, at least the
 * least of BR/EDR).
 */
void
btd_sdps_init(
	struct btd_sdps *server,
	const struct btd_sdps_db *db,
	size_t mtu)
{
	/* Nothing kept. */
	memset(server, 0, sizeof(*server));
	server->db = db;
	server->mtu = mtu;
	if (server->mtu < SDPS_MTU_LEAST)
		server->mtu = SDPS_MTU_LEAST;
}

/*
 * Answers one request PDU: the answer (or an error response) is written in
 * answer, at most the peer's MTU.  Returns 0 with its length, or EINVAL for
 * a PDU too short to name its transaction (nothing to answer).
 */
int
btd_sdps_input(
	struct btd_sdps *server,
	const uint8_t *pdu,
	size_t length,
	uint8_t *answer,
	size_t size,
	size_t *answer_length)
{
	struct sdps_request request;
	uint16_t transaction;
	size_t parameters;
	size_t offset;
	size_t portion;
	size_t room;
	size_t most;
	size_t used;
	size_t version;
	uint8_t kind;
	int error;

	/* Nothing answered yet. */
	*answer_length = 0U;

	/* A header to answer. */
	if (length < SDPS_HEADER)
		return EINVAL;
	kind = pdu[0];
	transaction = sdps_be16(pdu + 1);
	parameters = sdps_be16(pdu + 3);

	/* An answer fits the smaller of the buffer and the MTU (a buffer too small for a portion has nothing to answer with). */
	room = server->mtu;
	if (room > size)
		room = size;
	if (room < SDPS_MTU_LEAST)
		return EINVAL;

	/* The parameters' length is what follows the header. */
	if (parameters != length - SDPS_HEADER) {
		server->errors++;
		*answer_length = sdps_error(answer, room, transaction, BTD_SDPS_ERROR_PDU_SIZE);
		return 0;
	}

	/* A request bluetoothd serves, well formed. */
	if (kind != SDPS_SEARCH_REQUEST && kind != SDPS_ATTRIBUTE_REQUEST && kind != SDPS_SEARCH_ATTRIBUTE_REQUEST) {
		server->errors++;
		*answer_length = sdps_error(answer, room, transaction, BTD_SDPS_ERROR_SYNTAX);
		return 0;
	}

	/* Its parameters read. */
	error = sdps_parse(kind, pdu + SDPS_HEADER, parameters, &request);
	if (error != 0) {
		server->errors++;
		*answer_length = sdps_error(answer, room, transaction, (uint16_t)error);
		return 0;
	}

	/* A new request builds its whole answer; a continuation must be of the same request and records. */
	if (request.continuation_length == 0U) {
		error = sdps_build(server, kind, &request);
		if (error == 0 && request.parameters_length > sizeof(server->kept_request))
			error = BTD_SDPS_ERROR_RESOURCES;
		if (error != 0) {
			server->errors++;
			server->kept_pdu = 0U;
			*answer_length = sdps_error(answer, room, transaction, (uint16_t)error);
			return 0;
		}

		/* Kept for its continuations. */
		server->kept_pdu = kind;
		server->kept_version = server->db->version;
		server->kept_request_length = request.parameters_length;
		memcpy(server->kept_request, pdu + SDPS_HEADER, request.parameters_length);
		offset = 0U;
	} else {
		/* bluetoothd's own state: three bytes, the version and an offset within the answer kept. */
		version = server->db->version & 0xffU;
		offset = sdps_be16(request.continuation + 1);
		if (request.continuation_length != SDPS_CONTINUATION ||
		    server->kept_pdu != kind ||
		    request.continuation[0] != version ||
		    (server->kept_version & 0xffU) != version ||
		    request.parameters_length != server->kept_request_length ||
		    offset > server->kept_length) {
			server->errors++;
			*answer_length = sdps_error(answer, room, transaction, BTD_SDPS_ERROR_CONTINUATION);
			return 0;
		}

		/* The same parameters as the request kept. */
		error = memcmp(server->kept_request, pdu + SDPS_HEADER, request.parameters_length);
		if (error != 0) {
			server->errors++;
			*answer_length = sdps_error(answer, room, transaction, BTD_SDPS_ERROR_CONTINUATION);
			return 0;
		}
	}

	/*
	 * The portion: within the MTU less the header, the counts and the
	 * longest continuation state; within the bytes asked for (for a
	 * search, whole handles within the records asked for).
	 */
	most = room - SDPS_HEADER - 4U - 1U - SDPS_CONTINUATION;
	if (kind != SDPS_SEARCH_REQUEST && most > request.most)
		most = request.most;
	if (kind == SDPS_SEARCH_REQUEST)
		most -= most % 4U;
	portion = server->kept_length - offset;
	if (portion > most)
		portion = most;

	/* The answer's header. */
	answer[0] = (uint8_t)(kind + 1U);
	sdps_put16(answer + 1, transaction);
	used = SDPS_HEADER;

	/* A search's counts: all the handles, and those in this answer; else the byte count. */
	if (kind == SDPS_SEARCH_REQUEST) {
		sdps_put16(answer + used, server->kept_length / 4U);
		sdps_put16(answer + used + 2U, portion / 4U);
		used += 4U;
	} else {
		sdps_put16(answer + used, portion);
		used += 2U;
	}

	/* The portion. */
	memcpy(answer + used, server->kept + offset, portion);
	used += portion;

	/* The continuation: none at the end, else the version and the next offset. */
	if (offset + portion == server->kept_length) {
		answer[used] = 0U;
		used++;
	} else {
		answer[used] = SDPS_CONTINUATION;
		answer[used + 1U] = (uint8_t)(server->db->version & 0xffU);
		sdps_put16(answer + used + 2U, offset + portion);
		used += 1U + SDPS_CONTINUATION;
	}

	/* Succeeded: the parameters' length, and the answer. */
	sdps_put16(answer + 3, used - SDPS_HEADER);
	*answer_length = used;
	return 0;
}

/* Appends bytes to a writer, or marks it when they do not fit. */
static void
sdps_raw(
	struct btd_sdp_writer *writer,
	const uint8_t *data,
	size_t length)
{
	/* No room: nothing more is written. */
	if (writer->overflow || length > writer->size - writer->used) {
		writer->overflow = 1;
		return;
	}

	/* Succeeded: appended. */
	if (length != 0U)
		memcpy(writer->bytes + writer->used, data, length);
	writer->used += length;
}

/* Reads a 16-bit big-endian value. */
static uint16_t
sdps_be16(
	const uint8_t *bytes)
{
	/* Most significant first. */
	return (uint16_t)(((unsigned)bytes[0] << 8) | bytes[1]);
}

/* Reads a 32-bit big-endian value. */
static uint32_t
sdps_be32(
	const uint8_t *bytes)
{
	/* Most significant first. */
	return ((uint32_t)bytes[0] << 24) | ((uint32_t)bytes[1] << 16) | ((uint32_t)bytes[2] << 8) | (uint32_t)bytes[3];
}

/* Writes a 16-bit big-endian value. */
static void
sdps_put16(
	uint8_t *bytes,
	size_t value)
{
	/* Most significant first. */
	bytes[0] = (uint8_t)((value >> 8) & 0xffU);
	bytes[1] = (uint8_t)(value & 0xffU);
}

/* Writes a 32-bit big-endian value. */
static void
sdps_put32(
	uint8_t *bytes,
	uint32_t value)
{
	/* Most significant first. */
	bytes[0] = (uint8_t)(value >> 24);
	bytes[1] = (uint8_t)(value >> 16);
	bytes[2] = (uint8_t)(value >> 8);
	bytes[3] = (uint8_t)value;
}

/*
 * Gives a UUID element in its 128-bit form: a 16- or 32-bit UUID on the
 * Bluetooth base UUID 00000000-0000-1000-8000-00805f9b34fb (Core 5.4 Vol 3
 * Part B section 2.5.1).  Returns 0 or EINVAL.
 */
static int
sdps_uuid128(
	const struct btd_sdp_element *element,
	uint8_t *uuid)
{
	static const uint8_t base[16] = {
		0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x10U, 0x00U,
		0x80U, 0x00U, 0x00U, 0x80U, 0x5fU, 0x9bU, 0x34U, 0xfbU
	};

	/* A UUID. */
	if (element->type != BTD_SDP_TYPE_UUID)
		return EINVAL;

	/* 128 bits as they are. */
	if (element->length == 16U) {
		memcpy(uuid, element->value, 16U);
		return 0;
	}

	/* The short forms on the base. */
	memcpy(uuid, base, sizeof(base));
	if (element->length == 2U) {
		uuid[2] = element->value[0];
		uuid[3] = element->value[1];
	} else {
		memcpy(uuid, element->value, 4U);
	}

	/* Succeeded: the long form. */
	return 0;
}

/* Reads a ServiceSearchPattern: a sequence of 1 to 12 UUIDs.  Returns 0 or an SDP error code. */
static int
sdps_pattern(
	const uint8_t *data,
	size_t length,
	size_t *offset,
	struct sdps_request *request)
{
	struct btd_sdp_element pattern;
	struct btd_sdp_element uuid;
	size_t inner;
	int error;

	/* A sequence. */
	error = btd_sdp_element(data + *offset, length - *offset, &pattern);
	if (error != 0 || pattern.type != BTD_SDP_TYPE_SEQUENCE)
		return BTD_SDPS_ERROR_SYNTAX;
	*offset += pattern.size;

	/* Each UUID of it, 1 to 12. */
	inner = 0U;
	while (inner < pattern.length) {
		if (request->uuid_count >= BTD_SDPS_PATTERN_MAX)
			return BTD_SDPS_ERROR_SYNTAX;
		error = btd_sdp_element(pattern.value + inner, pattern.length - inner, &uuid);
		if (error != 0)
			return BTD_SDPS_ERROR_SYNTAX;
		error = sdps_uuid128(&uuid, request->uuids[request->uuid_count]);
		if (error != 0)
			return BTD_SDPS_ERROR_SYNTAX;
		request->uuid_count++;
		inner += uuid.size;
	}

	/* Succeeded: at least one. */
	if (request->uuid_count == 0U)
		return BTD_SDPS_ERROR_SYNTAX;
	return 0;
}

/* Reads an AttributeIDList: a sequence of 16-bit IDs and 32-bit ranges (first in the upper half).  Returns 0 or an SDP error code. */
static int
sdps_ranges(
	const uint8_t *data,
	size_t length,
	size_t *offset,
	struct sdps_request *request)
{
	struct btd_sdp_element list;
	struct btd_sdp_element entry;
	struct sdps_range *range;
	size_t inner;
	uint32_t value;
	int error;

	/* A sequence. */
	error = btd_sdp_element(data + *offset, length - *offset, &list);
	if (error != 0 || list.type != BTD_SDP_TYPE_SEQUENCE)
		return BTD_SDPS_ERROR_SYNTAX;
	*offset += list.size;

	/* Each ID or range, ascending order not required. */
	inner = 0U;
	while (inner < list.length) {
		if (request->range_count >= BTD_SDPS_RANGES_MAX)
			return BTD_SDPS_ERROR_SYNTAX;
		error = btd_sdp_element(list.value + inner, list.length - inner, &entry);
		if (error != 0 || entry.type != BTD_SDP_TYPE_UINT)
			return BTD_SDPS_ERROR_SYNTAX;
		range = &request->ranges[request->range_count];

		/* An ID, or a range from its upper half to its lower half. */
		if (entry.length == 2U) {
			range->first = sdps_be16(entry.value);
			range->last = range->first;
		} else if (entry.length == 4U) {
			value = sdps_be32(entry.value);
			range->first = (uint16_t)(value >> 16);
			range->last = (uint16_t)(value & 0xffffU);
			if (range->first > range->last)
				return BTD_SDPS_ERROR_SYNTAX;
		} else {
			return BTD_SDPS_ERROR_SYNTAX;
		}

		/* Counted. */
		request->range_count++;
		inner += entry.size;
	}

	/* Succeeded: at least one. */
	if (request->range_count == 0U)
		return BTD_SDPS_ERROR_SYNTAX;
	return 0;
}

/* Reads the continuation state that ends the parameters: its length byte (at most 16) and its bytes.  Returns 0 or an SDP error code. */
static int
sdps_continuation(
	const uint8_t *data,
	size_t length,
	size_t offset,
	struct sdps_request *request)
{
	/* Its length byte. */
	if (offset >= length)
		return BTD_SDPS_ERROR_SYNTAX;
	request->continuation_length = data[offset];
	if (request->continuation_length > SDPS_CONTINUATION_MAX)
		return BTD_SDPS_ERROR_CONTINUATION;

	/* Its bytes end the parameters exactly. */
	if (offset + 1U + request->continuation_length != length)
		return BTD_SDPS_ERROR_PDU_SIZE;

	/* Succeeded: the parameters before it are the request's. */
	request->continuation = data + offset + 1U;
	request->parameters_length = offset;
	return 0;
}

/* Reads a request's parameters by its PDU.  Returns 0 or an SDP error code. */
static int
sdps_parse(
	uint8_t pdu,
	const uint8_t *data,
	size_t length,
	struct sdps_request *request)
{
	size_t offset;
	int error;

	/* Nothing read yet. */
	memset(request, 0, sizeof(*request));
	offset = 0U;

	/* A search's pattern, or an attribute request's handle. */
	if (pdu == SDPS_ATTRIBUTE_REQUEST) {
		if (length < 4U)
			return BTD_SDPS_ERROR_SYNTAX;
		request->handle = sdps_be32(data);
		offset = 4U;
	} else {
		error = sdps_pattern(data, length, &offset, request);
		if (error != 0)
			return error;
	}

	/* The most records or bytes. */
	if (length - offset < 2U)
		return BTD_SDPS_ERROR_SYNTAX;
	request->most = sdps_be16(data + offset);
	offset += 2U;
	if (pdu == SDPS_SEARCH_REQUEST && request->most == 0U)
		return BTD_SDPS_ERROR_SYNTAX;
	if (pdu != SDPS_SEARCH_REQUEST && request->most < SDPS_BYTES_LEAST)
		return BTD_SDPS_ERROR_SYNTAX;

	/* The attributes asked for. */
	if (pdu != SDPS_SEARCH_REQUEST) {
		error = sdps_ranges(data, length, &offset, request);
		if (error != 0)
			return error;
	}

	/* Succeeded: the continuation state last. */
	error = sdps_continuation(data, length, offset, request);
	if (error != 0)
		return error;
	return 0;
}

/* Tells whether a record holds every UUID of the request's pattern somewhere among its attributes' values. */
static int
sdps_matches(
	const struct btd_sdps_record *record,
	const struct sdps_request *request)
{
	unsigned index;
	int found;

	/* Each UUID of the pattern. */
	for (index = 0U; index < request->uuid_count; index++) {
		found = sdps_contains(record->attributes, record->length, request->uuids[index], 0U);
		if (!found)
			return 0;
	}

	/* Succeeded: all of them. */
	return 1;
}

/* Tells whether data elements hold a UUID (in any of its forms), looking into sequences and alternatives. */
static int
sdps_contains(
	const uint8_t *data,
	size_t length,
	const uint8_t *uuid,
	unsigned depth)
{
	struct btd_sdp_element element;
	uint8_t long_form[16];
	size_t offset;
	int differs;
	int found;
	int error;

	/* Not deeper than the most. */
	if (depth > SDPS_DEPTH_MAX)
		return 0;

	/* Each element. */
	offset = 0U;
	while (offset < length) {
		error = btd_sdp_element(data + offset, length - offset, &element);
		if (error != 0)
			return 0;
		offset += element.size;

		/* A UUID: compared in its long form. */
		if (element.type == BTD_SDP_TYPE_UUID) {
			error = sdps_uuid128(&element, long_form);
			if (error != 0)
				continue;
			differs = memcmp(long_form, uuid, sizeof(long_form));
			if (differs == 0)
				return 1;
			continue;
		}

		/* A sequence or an alternative: looked into. */
		if (element.type == BTD_SDP_TYPE_SEQUENCE || element.type == BTD_SDP_TYPE_ALTERNATIVE) {
			found = sdps_contains(element.value, element.length, uuid, depth + 1U);
			if (found)
				return 1;
		}
	}

	/* Not there. */
	return 0;
}

/* Tells whether a request asks for an attribute ID. */
static int
sdps_wanted(
	const struct sdps_request *request,
	uint16_t id)
{
	unsigned index;

	/* Each ID or range. */
	for (index = 0U; index < request->range_count; index++) {
		if (id >= request->ranges[index].first && id <= request->ranges[index].last)
			return 1;
	}

	/* Not asked for. */
	return 0;
}

/* Writes a record's attribute list of the IDs asked for: a sequence of pairs.  Returns the count of pairs written. */
static int
sdps_attributes(
	const struct btd_sdps_record *record,
	const struct sdps_request *request,
	struct btd_sdp_writer *writer)
{
	struct btd_sdp_element id;
	struct btd_sdp_element value;
	uint16_t number;
	size_t offset;
	int count;
	int wanted;

	/* The sequence of the pairs asked for (the record's pairs are well formed: register checked them). */
	count = 0;
	btd_sdp_begin(writer);
	offset = 0U;
	while (offset < record->length) {
		(void)btd_sdp_element(record->attributes + offset, record->length - offset, &id);
		(void)btd_sdp_element(record->attributes + offset + id.size, record->length - offset - id.size, &value);

		/* Asked for: written as it is. */
		number = sdps_be16(id.value);
		wanted = sdps_wanted(request, number);
		if (wanted) {
			sdps_raw(writer, record->attributes + offset, id.size + value.size);
			count++;
		}

		/* The next pair. */
		offset += id.size + value.size;
	}

	/* The list closed. */
	btd_sdp_end(writer);

	/* Succeeded: the count. */
	return count;
}

/*
 * Builds a request's whole answer into the server: a search's handles, an
 * attribute request's list, a search-attribute request's lists (records
 * with no attribute asked for are left out).  Returns 0 or an SDP error
 * code.
 */
static int
sdps_build(
	struct btd_sdps *server,
	uint8_t pdu,
	const struct sdps_request *request)
{
	const struct btd_sdps_record *record;
	struct btd_sdp_writer writer;
	size_t before;
	unsigned index;
	unsigned found;
	int matches;
	int count;

	/* Into the answer kept. */
	btd_sdp_writer_init(&writer, server->kept, sizeof(server->kept));

	/* A search: the handles of the matching records, at most the count asked. */
	if (pdu == SDPS_SEARCH_REQUEST) {
		found = 0U;
		for (index = 0U; index < BTD_SDPS_RECORDS_MAX; index++) {
			record = &server->db->records[index];
			if (!record->used || found >= request->most)
				continue;
			matches = sdps_matches(record, request);
			if (!matches)
				continue;
			sdps_put32(server->kept + writer.used, record->handle);
			writer.used += 4U;
			found++;
		}

		/* Succeeded: the handles. */
		server->kept_length = writer.used;
		return 0;
	}

	/* An attribute request: the one record's list. */
	if (pdu == SDPS_ATTRIBUTE_REQUEST) {
		record = sdps_record(server->db, request->handle);
		if (record == NULL)
			return BTD_SDPS_ERROR_HANDLE;
		(void)sdps_attributes(record, request, &writer);
		if (writer.overflow)
			return BTD_SDPS_ERROR_RESOURCES;
		server->kept_length = writer.used;
		return 0;
	}

	/* A search-attribute request: a sequence of the matching records' lists. */
	btd_sdp_begin(&writer);
	for (index = 0U; index < BTD_SDPS_RECORDS_MAX; index++) {
		record = &server->db->records[index];
		if (!record->used)
			continue;
		matches = sdps_matches(record, request);
		if (!matches)
			continue;

		/* Its list, taken back when it holds no attribute asked for. */
		before = writer.used;
		count = sdps_attributes(record, request, &writer);
		if (count == 0 && !writer.overflow)
			writer.used = before;
	}

	/* The lists closed. */
	btd_sdp_end(&writer);
	if (writer.overflow)
		return BTD_SDPS_ERROR_RESOURCES;

	/* Succeeded: the whole answer. */
	server->kept_length = writer.used;
	return 0;
}

/* Finds a record by its handle, or NULL. */
static const struct btd_sdps_record *
sdps_record(
	const struct btd_sdps_db *db,
	uint32_t handle)
{
	unsigned index;

	/* Each record offered. */
	for (index = 0U; index < BTD_SDPS_RECORDS_MAX; index++) {
		if (db->records[index].used && db->records[index].handle == handle)
			return &db->records[index];
	}

	/* None. */
	return NULL;
}

/* Writes an error response; returns its length (0 when the buffer is too small). */
static size_t
sdps_error(
	uint8_t *answer,
	size_t size,
	uint16_t transaction,
	uint16_t code)
{
	/* The header and the code. */
	if (size < SDPS_HEADER + 2U)
		return 0U;
	answer[0] = SDPS_ERROR_RESPONSE;
	sdps_put16(answer + 1, transaction);
	sdps_put16(answer + 3, 2U);
	sdps_put16(answer + 5, code);

	/* Succeeded: its length. */
	return SDPS_HEADER + 2U;
}
