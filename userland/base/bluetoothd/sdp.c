/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * bluetoothd's SDP client (ws143-p005 i02, see sdp.h).
 *
 * The PDUs and data elements follow Core 5.4 Vol 3 Part B (sections 3 and
 * 4.7), the HID record's attributes HID Profile 1.1.1 section 5.3 and the
 * PnP record's the Device ID Profile 1.3 (values to be checked against the
 * specifications: plan/ws143/phase005/phase.md section 8).
 */

#include "userland/base/bluetoothd/sdp.h"

#include <errno.h>
#include <string.h>

/* The PDUs (Core Vol 3 Part B §4.2). */
#define SDP_ERROR_RESPONSE		0x01U
#define SDP_SEARCH_ATTRIBUTE_REQUEST	0x06U
#define SDP_SEARCH_ATTRIBUTE_RESPONSE	0x07U

/* A PDU's header: its ID, transaction and parameter length. */
#define SDP_HEADER			5U

/* The longest continuation state, and how often the same one may come back. */
#define SDP_CONTINUATION_MAX		16U
#define SDP_REPEATS_MAX			8U

/* How deep data elements may nest. */
#define SDP_DEPTH_MAX			8U

/* The attributes read (HID Profile 1.1.1 §5.3.4, Device ID Profile 1.3 §5). */
#define SDP_ATTRIBUTE_CLASSES		0x0001U
#define SDP_ATTRIBUTE_PROTOCOLS		0x0004U
#define SDP_ATTRIBUTE_MORE_PROTOCOLS	0x000dU
#define SDP_ATTRIBUTE_NAME		0x0100U
#define SDP_ATTRIBUTE_HID_SUBCLASS	0x0202U
#define SDP_ATTRIBUTE_HID_COUNTRY	0x0203U
#define SDP_ATTRIBUTE_HID_CABLE		0x0204U
#define SDP_ATTRIBUTE_HID_RECONNECT	0x0205U
#define SDP_ATTRIBUTE_HID_DESCRIPTORS	0x0206U
#define SDP_ATTRIBUTE_HID_CONNECTABLE	0x020dU
#define SDP_ATTRIBUTE_HID_BOOT		0x020eU
#define SDP_ATTRIBUTE_PNP_VENDOR	0x0201U
#define SDP_ATTRIBUTE_PNP_PRODUCT	0x0202U
#define SDP_ATTRIBUTE_PNP_VERSION	0x0203U
#define SDP_ATTRIBUTE_PNP_SOURCE	0x0205U

/* The protocol UUID of L2CAP, and the class descriptor type of a report descriptor. */
#define SDP_UUID_L2CAP			0x0100U
#define SDP_DESCRIPTOR_REPORT		0x22U

/* RFCOMM's protocol UUID, and the profiles' list of a record (ws197-p002). */
#define SDP_UUID_RFCOMM			0x0003U
#define SDP_ATTRIBUTE_PROFILES		0x0009U

static int sdp_element(const uint8_t *data, size_t length, struct btd_sdp_element *element);
static int sdp_check(const uint8_t *data, size_t length, unsigned depth);
static int sdp_uint(const struct btd_sdp_element *element, uint32_t *value);
static int sdp_uuid(const struct btd_sdp_element *element, uint32_t *value);
static int sdp_bool(const struct btd_sdp_element *element, int *value);
static int sdp_has_class(const struct btd_sdp_element *classes, uint32_t uuid);
static int sdp_psm(const struct btd_sdp_element *protocols, uint32_t *psm);
static int sdp_find_record(const struct btd_sdp *sdp, uint32_t uuid, struct btd_sdp_element *record);
static int sdp_find_nth(const struct btd_sdp *sdp, uint32_t uuid, unsigned nth, struct btd_sdp_element *record);
static int sdp_record_value(const struct btd_sdp_element *record, uint32_t attribute, struct btd_sdp_element *value);
static int sdp_rfcomm(const struct btd_sdp_element *protocols, uint32_t *channel);
static int sdp_hid_attribute(uint32_t id, const struct btd_sdp_element *value, struct btd_hid_record *record);
static int sdp_descriptor(const struct btd_sdp_element *list, struct btd_hid_record *record);
static uint16_t sdp_be16(const uint8_t *bytes);
static void sdp_put16(uint8_t *bytes, uint16_t value);

/*
 * Starts a query for one service class, its first request carrying the
 * transaction given.
 */
void
btd_sdp_init(
	struct btd_sdp *sdp,
	uint16_t uuid,
	uint16_t transaction)
{
	/* Nothing has come; no continuation yet. */
	memset(sdp, 0, sizeof(*sdp));
	sdp->uuid = uuid;
	sdp->transaction = transaction;
}

/*
 * Builds the next ServiceSearchAttributeRequest: the service class, every
 * attribute (0x0000 to 0xffff), the count asked for and the continuation
 * state of the last response.  Returns 0 with the length, or ENOSPC.
 */
int
btd_sdp_request(
	struct btd_sdp *sdp,
	uint8_t *out,
	size_t size,
	size_t *length)
{
	static const uint8_t attributes[] = { 0x35, 0x05, 0x0a, 0x00, 0x00, 0xff, 0xff };
	size_t parameters;
	size_t used;

	/* Refuses a buffer the request does not fit. */
	parameters = 5U + 2U + sizeof(attributes) + 1U + sdp->continuation_length;
	if (size < SDP_HEADER + parameters)
		return ENOSPC;

	/* The header. */
	out[0] = SDP_SEARCH_ATTRIBUTE_REQUEST;
	sdp_put16(&out[1], sdp->transaction);
	sdp_put16(&out[3], (uint16_t)parameters);
	used = SDP_HEADER;

	/* The search pattern: a sequence of one 16-bit UUID. */
	out[used] = 0x35;
	out[used + 1U] = 0x03;
	out[used + 2U] = 0x19;
	sdp_put16(&out[used + 3U], sdp->uuid);
	used += 5U;

	/* The most attribute bytes a response may carry, and every attribute. */
	sdp_put16(&out[used], BTD_SDP_BYTES_ASKED);
	used += 2U;
	memcpy(&out[used], attributes, sizeof(attributes));
	used += sizeof(attributes);

	/* The continuation state (none in the first request). */
	out[used] = (uint8_t)sdp->continuation_length;
	used++;
	memcpy(&out[used], sdp->continuation, sdp->continuation_length);
	used += sdp->continuation_length;

	/* Succeeded: the request's length. */
	*length = used;
	return 0;
}

/*
 * Takes a response: its fragment of the attribute lists is added, and its
 * continuation state is kept for the next request.  Returns BTD_SDP_MORE,
 * BTD_SDP_DONE when the lists are whole (and well formed), or
 * BTD_SDP_FAILED with sdp->why ("protocol", "malformed" or "descriptor").
 */
int
btd_sdp_input(
	struct btd_sdp *sdp,
	const uint8_t *pdu,
	size_t length)
{
	size_t parameters;
	size_t count;
	size_t continuation;
	uint16_t transaction;
	int same;
	int error;

	/* Refuses a PDU shorter than its header, or not as long as its header says. */
	if (length < SDP_HEADER) {
		sdp->why = "malformed";
		return BTD_SDP_FAILED;
	}

	/* The parameters' length must be what follows the header. */
	parameters = sdp_be16(&pdu[3]);
	if (parameters != length - SDP_HEADER) {
		sdp->why = "malformed";
		return BTD_SDP_FAILED;
	}

	/* An error response, another PDU or another transaction ends the query. */
	if (pdu[0] != SDP_SEARCH_ATTRIBUTE_RESPONSE) {
		sdp->why = "protocol";
		return BTD_SDP_FAILED;
	}

	/* Another transaction is not the answer to the request out. */
	transaction = sdp_be16(&pdu[1]);
	if (transaction != sdp->transaction) {
		sdp->why = "protocol";
		return BTD_SDP_FAILED;
	}

	/* The count, the fragment and the continuation state must fill the parameters exactly. */
	if (parameters < 3U) {
		sdp->why = "malformed";
		return BTD_SDP_FAILED;
	}

	/* The count within the parameters. */
	count = sdp_be16(&pdu[SDP_HEADER]);
	if (count > parameters - 3U) {
		sdp->why = "malformed";
		return BTD_SDP_FAILED;
	}

	/* The continuation state's length within the most, ending the parameters. */
	continuation = pdu[SDP_HEADER + 2U + count];
	if (continuation > SDP_CONTINUATION_MAX || 2U + count + 1U + continuation != parameters) {
		sdp->why = "malformed";
		return BTD_SDP_FAILED;
	}

	/* The fragment, kept (records past the most kept are refused). */
	if (count > BTD_SDP_MAX - sdp->used) {
		sdp->why = "descriptor";
		return BTD_SDP_FAILED;
	}

	/* Added after what came before. */
	memcpy(&sdp->lists[sdp->used], &pdu[SDP_HEADER + 2U], count);
	sdp->used += count;

	/* No continuation: the lists are whole, and must be well formed data elements. */
	if (continuation == 0U) {
		error = sdp_check(sdp->lists, sdp->used, 0U);
		if (error != 0) {
			sdp->why = "malformed";
			return BTD_SDP_FAILED;
		}

		/* Done. */
		return BTD_SDP_DONE;
	}

	/* A server giving back the same continuation again and again is not getting on. */
	same = 0;
	if (continuation == sdp->continuation_length)
		same = memcmp(sdp->continuation, &pdu[SDP_HEADER + 3U + count], continuation) == 0;
	if (same) {
		sdp->repeats++;
		if (sdp->repeats >= SDP_REPEATS_MAX) {
			sdp->why = "protocol";
			return BTD_SDP_FAILED;
		}
	}

	/* The state for the next request, in a new transaction. */
	memcpy(sdp->continuation, &pdu[SDP_HEADER + 3U + count], continuation);
	sdp->continuation_length = continuation;
	sdp->transaction++;
	return BTD_SDP_MORE;
}

/*
 * Reads the HID record of a whole query for 0x1124: its channels must be
 * the fixed PSMs (control 0x0011 and interrupt 0x0013), it must carry a
 * report descriptor.  The descriptor points into the query.  Returns 0,
 * ENOENT when there is no usable HID record, E2BIG for a descriptor past
 * 4096 bytes, or EINVAL for an attribute of the wrong form.
 */
int
btd_sdp_hid(
	const struct btd_sdp *sdp,
	struct btd_hid_record *record)
{
	struct btd_sdp_element found;
	struct btd_sdp_element pair;
	struct btd_sdp_element value;
	uint32_t id;
	uint32_t psm;
	size_t offset;
	int control;
	int interrupt;
	int error;

	/* The record of the class. */
	error = sdp_find_record(sdp, BTD_SDP_UUID_HID, &found);
	if (error != 0)
		return error;

	/* Its attributes, in pairs of ID and value. */
	control = 0;
	interrupt = 0;
	offset = 0;
	while (offset < found.length) {
		/* The ID. */
		error = sdp_element(&found.value[offset], found.length - offset, &pair);
		if (error != 0)
			return EINVAL;
		error = sdp_uint(&pair, &id);
		if (error != 0)
			return EINVAL;
		offset += pair.size;

		/* Its value. */
		error = sdp_element(&found.value[offset], found.length - offset, &value);
		if (error != 0)
			return EINVAL;
		offset += value.size;

		/* The channels, which must be HID's fixed ones. */
		if (id == SDP_ATTRIBUTE_PROTOCOLS || id == SDP_ATTRIBUTE_MORE_PROTOCOLS) {
			error = sdp_psm(&value, &psm);
			if (error != 0)
				return ENOENT;
			if (id == SDP_ATTRIBUTE_PROTOCOLS && psm == BTD_SDP_PSM_CONTROL)
				control = 1;
			if (id == SDP_ATTRIBUTE_MORE_PROTOCOLS && psm == BTD_SDP_PSM_INTERRUPT)
				interrupt = 1;
			continue;
		}

		/* The others. */
		error = sdp_hid_attribute(id, &value, record);
		if (error != 0)
			return error;
	}

	/* A record without both channels or without a descriptor cannot be used. */
	if (!control || !interrupt || record->descriptor == NULL)
		return ENOENT;

	/* Succeeded: the HID record. */
	record->hid = 1;
	return 0;
}

/*
 * Reads the PnP Information record of a whole query for 0x1200: the
 * vendor ID's source, the vendor, the product and the version.  Returns 0,
 * ENOENT when there is none, or EINVAL.
 */
int
btd_sdp_pnp(
	const struct btd_sdp *sdp,
	struct btd_hid_record *record)
{
	struct btd_sdp_element found;
	struct btd_sdp_element pair;
	struct btd_sdp_element value;
	uint32_t id;
	uint32_t number;
	size_t offset;
	int error;

	/* The record of the class. */
	error = sdp_find_record(sdp, BTD_SDP_UUID_PNP, &found);
	if (error != 0)
		return error;

	/* Its attributes, the numbers read where they are 16-bit unsigned. */
	offset = 0;
	while (offset < found.length) {
		/* The ID and the value. */
		error = sdp_element(&found.value[offset], found.length - offset, &pair);
		if (error != 0)
			return EINVAL;
		error = sdp_uint(&pair, &id);
		if (error != 0)
			return EINVAL;
		offset += pair.size;
		error = sdp_element(&found.value[offset], found.length - offset, &value);
		if (error != 0)
			return EINVAL;
		offset += value.size;

		/* Only the four numbers bluetoothd keeps. */
		if (id != SDP_ATTRIBUTE_PNP_VENDOR && id != SDP_ATTRIBUTE_PNP_PRODUCT && id != SDP_ATTRIBUTE_PNP_VERSION && id != SDP_ATTRIBUTE_PNP_SOURCE)
			continue;
		error = sdp_uint(&value, &number);
		if (error != 0 || number > 0xffffU)
			return EINVAL;

		/* Kept by ID. */
		if (id == SDP_ATTRIBUTE_PNP_VENDOR)
			record->vendor = (uint16_t)number;
		else if (id == SDP_ATTRIBUTE_PNP_PRODUCT)
			record->product = (uint16_t)number;
		else if (id == SDP_ATTRIBUTE_PNP_VERSION)
			record->version = (uint16_t)number;
		else
			record->vendor_source = (uint16_t)number;
	}

	/* Succeeded: the PnP numbers. */
	record->pnp = 1;
	return 0;
}

/*
 * Counts the records of a class in a whole query (ws197-p002: a phone's
 * MAS instances).
 */
unsigned
btd_sdp_records(
	const struct btd_sdp *sdp,
	uint16_t uuid)
{
	struct btd_sdp_element record;
	unsigned count;
	int error;

	/* Each one found in turn. */
	count = 0U;
	for (;;) {
		error = sdp_find_nth(sdp, uuid, count, &record);
		if (error != 0)
			break;
		count++;
	}

	/* Succeeded: the count. */
	return count;
}

/*
 * Reads the RFCOMM server channel of the nth record of a class (its
 * ProtocolDescriptorList: L2CAP, then RFCOMM with the channel, RFCOMM 1.2
 * section 7.2).  Returns 0, ENOENT, or EINVAL for a channel outside 1 to
 * 30.
 */
int
btd_sdp_rfcomm_channel(
	const struct btd_sdp *sdp,
	uint16_t uuid,
	unsigned nth,
	unsigned *channel)
{
	struct btd_sdp_element record;
	struct btd_sdp_element protocols;
	uint32_t value;
	int error;

	/* The record and its ProtocolDescriptorList. */
	error = sdp_find_nth(sdp, uuid, nth, &record);
	if (error != 0)
		return error;
	error = sdp_record_value(&record, SDP_ATTRIBUTE_PROTOCOLS, &protocols);
	if (error != 0)
		return error;

	/* The RFCOMM descriptor's channel. */
	error = sdp_rfcomm(&protocols, &value);
	if (error != 0)
		return error;
	if (value < 1U || value > 30U)
		return EINVAL;

	/* Succeeded: the channel. */
	*channel = value;
	return 0;
}

/*
 * Reads the version of a profile that the nth record of a class lists in
 * its BluetoothProfileDescriptorList (pairs of the profile's UUID and a
 * 16-bit version).  Returns 0, ENOENT, or EINVAL.
 */
int
btd_sdp_profile_version(
	const struct btd_sdp *sdp,
	uint16_t uuid,
	unsigned nth,
	uint16_t profile,
	uint16_t *version)
{
	struct btd_sdp_element record;
	struct btd_sdp_element profiles;
	struct btd_sdp_element descriptor;
	struct btd_sdp_element named;
	struct btd_sdp_element number;
	uint32_t value;
	size_t offset;
	int error;

	/* The record and its profiles. */
	error = sdp_find_nth(sdp, uuid, nth, &record);
	if (error != 0)
		return error;
	error = sdp_record_value(&record, SDP_ATTRIBUTE_PROFILES, &profiles);
	if (error != 0)
		return error;
	if (profiles.type != BTD_SDP_TYPE_SEQUENCE)
		return EINVAL;

	/* Each descriptor: the profile's UUID, then its version. */
	offset = 0;
	while (offset < profiles.length) {
		error = sdp_element(&profiles.value[offset], profiles.length - offset, &descriptor);
		if (error != 0 || descriptor.type != BTD_SDP_TYPE_SEQUENCE)
			return EINVAL;
		offset += descriptor.size;

		/* The profile named. */
		error = sdp_element(descriptor.value, descriptor.length, &named);
		if (error != 0)
			return EINVAL;
		error = sdp_uuid(&named, &value);
		if (error != 0 || value != profile)
			continue;

		/* Succeeded: its version. */
		error = sdp_element(&descriptor.value[named.size], descriptor.length - named.size, &number);
		if (error != 0)
			return EINVAL;
		error = sdp_uint(&number, &value);
		if (error != 0 || value > 0xffffU)
			return EINVAL;
		*version = (uint16_t)value;
		return 0;
	}

	/* The profile is not listed. */
	return ENOENT;
}

/*
 * Reads an unsigned attribute (1, 2 or 4 bytes) of the nth record of a
 * class: MAP's MASInstanceID and SupportedMessageTypes, the profiles'
 * SupportedFeatures.  Returns 0, ENOENT, or EINVAL.
 */
int
btd_sdp_uint_attribute(
	const struct btd_sdp *sdp,
	uint16_t uuid,
	unsigned nth,
	uint16_t attribute,
	uint32_t *value)
{
	struct btd_sdp_element record;
	struct btd_sdp_element found;
	int error;

	/* The record and the attribute. */
	error = sdp_find_nth(sdp, uuid, nth, &record);
	if (error != 0)
		return error;
	error = sdp_record_value(&record, attribute, &found);
	if (error != 0)
		return error;

	/* Succeeded: an unsigned number. */
	error = sdp_uint(&found, value);
	if (error != 0)
		return EINVAL;
	return 0;
}

/*
 * Takes apart the data element at the start of the bytes for another part
 * of bluetoothd (the SDP server, ws197-p002).  Returns 0 or EINVAL.
 */
int
btd_sdp_element(
	const uint8_t *data,
	size_t length,
	struct btd_sdp_element *element)
{
	int error;

	/* The element. */
	error = sdp_element(data, length, element);
	if (error != 0)
		return error;

	/* Succeeded: taken apart. */
	return 0;
}

/*
 * Checks that bytes are a list of well formed data elements nested at most
 * eight deep, for another part of bluetoothd.  Returns 0 or EINVAL.
 */
int
btd_sdp_check(
	const uint8_t *data,
	size_t length)
{
	int error;

	/* The elements. */
	error = sdp_check(data, length, 0U);
	if (error != 0)
		return error;

	/* Succeeded: well formed. */
	return 0;
}

/*
 * Takes apart the data element at the start of the bytes: its type, its
 * value and its whole size, checked against the bytes and the sizes the
 * type allows.  Returns 0 or EINVAL.
 */
static int
sdp_element(
	const uint8_t *data,
	size_t length,
	struct btd_sdp_element *element)
{
	static const size_t fixed[5] = { 1U, 2U, 4U, 8U, 16U };
	unsigned index;
	size_t header;
	size_t value;

	/* Refuses nothing at all. */
	if (length == 0U)
		return EINVAL;
	element->type = data[0] >> 3;
	index = data[0] & 0x07U;

	/* Nil has no value; the other fixed sizes; then lengths in 1, 2 or 4 bytes. */
	header = 1U;
	if (element->type == BTD_SDP_TYPE_NIL) {
		if (index != 0U)
			return EINVAL;
		value = 0U;
	} else if (index <= 4U) {
		value = fixed[index];
	} else if (index == 5U) {
		if (length < 2U)
			return EINVAL;
		value = data[1];
		header = 2U;
	} else if (index == 6U) {
		if (length < 3U)
			return EINVAL;
		value = sdp_be16(&data[1]);
		header = 3U;
	} else {
		if (length < 5U)
			return EINVAL;
		value = ((size_t)data[1] << 24) | ((size_t)data[2] << 16) | ((size_t)data[3] << 8) | data[4];
		header = 5U;
	}

	/* The sizes each type allows: numbers fixed, UUIDs 2, 4 or 16, booleans 1, the rest a length. */
	if ((element->type == BTD_SDP_TYPE_UINT || element->type == BTD_SDP_TYPE_SINT) && index > 4U)
		return EINVAL;
	if (element->type == BTD_SDP_TYPE_UUID && index != 1U && index != 2U && index != 4U)
		return EINVAL;
	if (element->type == BTD_SDP_TYPE_BOOL && index != 0U)
		return EINVAL;
	if ((element->type == BTD_SDP_TYPE_TEXT || element->type == BTD_SDP_TYPE_SEQUENCE || element->type == BTD_SDP_TYPE_ALTERNATIVE || element->type == BTD_SDP_TYPE_URL) && index < 5U)
		return EINVAL;
	if (element->type > BTD_SDP_TYPE_URL)
		return EINVAL;

	/* The value must be within the bytes. */
	if (value > length - header)
		return EINVAL;

	/* Succeeded: the element. */
	element->value = &data[header];
	element->length = value;
	element->size = header + value;
	return 0;
}

/*
 * Checks that the bytes are a list of well formed data elements, each
 * within what holds it, nested at most eight deep.  Returns 0 or EINVAL.
 */
static int
sdp_check(
	const uint8_t *data,
	size_t length,
	unsigned depth)
{
	struct btd_sdp_element element;
	size_t offset;
	int error;

	/* Refuses nesting past the most. */
	if (depth > SDP_DEPTH_MAX)
		return EINVAL;

	/* Each element, and those a sequence or an alternative holds. */
	offset = 0;
	while (offset < length) {
		error = sdp_element(&data[offset], length - offset, &element);
		if (error != 0)
			return error;
		if (element.type == BTD_SDP_TYPE_SEQUENCE || element.type == BTD_SDP_TYPE_ALTERNATIVE) {
			error = sdp_check(element.value, element.length, depth + 1U);
			if (error != 0)
				return error;
		}

		/* The next element. */
		offset += element.size;
	}

	/* Succeeded: well formed. */
	return 0;
}

/* Reads an unsigned integer of 1, 2 or 4 bytes.  Returns 0 or EINVAL. */
static int
sdp_uint(
	const struct btd_sdp_element *element,
	uint32_t *value)
{
	/* Refuses another type, or a size past 32 bits. */
	if (element->type != BTD_SDP_TYPE_UINT || element->length > 4U)
		return EINVAL;

	/* The number, most significant byte first. */
	if (element->length == 1U)
		*value = element->value[0];
	else if (element->length == 2U)
		*value = sdp_be16(element->value);
	else
		*value = ((uint32_t)element->value[0] << 24) | ((uint32_t)element->value[1] << 16) | ((uint32_t)element->value[2] << 8) | element->value[3];

	/* Succeeded. */
	return 0;
}

/*
 * Reads a UUID as its 16- or 32-bit short form: a 128-bit one must be on
 * the Bluetooth base UUID.  Returns 0 or EINVAL.
 */
static int
sdp_uuid(
	const struct btd_sdp_element *element,
	uint32_t *value)
{
	static const uint8_t base[12] = { 0x00, 0x00, 0x10, 0x00, 0x80, 0x00, 0x00, 0x80, 0x5f, 0x9b, 0x34, 0xfb };
	int different;

	/* Refuses another type. */
	if (element->type != BTD_SDP_TYPE_UUID)
		return EINVAL;

	/* The short forms. */
	if (element->length == 2U) {
		*value = sdp_be16(element->value);
		return 0;
	}

	/* The 32-bit form. */
	if (element->length == 4U) {
		*value = ((uint32_t)element->value[0] << 24) | ((uint32_t)element->value[1] << 16) | ((uint32_t)element->value[2] << 8) | element->value[3];
		return 0;
	}

	/* A long one off the base UUID names no class bluetoothd knows. */
	different = memcmp(&element->value[4], base, sizeof(base));
	if (different != 0)
		return EINVAL;

	/* Succeeded: its first 32 bits. */
	*value = ((uint32_t)element->value[0] << 24) | ((uint32_t)element->value[1] << 16) | ((uint32_t)element->value[2] << 8) | element->value[3];
	return 0;
}

/* Reads a boolean.  Returns 0 or EINVAL. */
static int
sdp_bool(
	const struct btd_sdp_element *element,
	int *value)
{
	/* Refuses another type. */
	if (element->type != BTD_SDP_TYPE_BOOL || element->length != 1U)
		return EINVAL;

	/* Any value but 0 is true. */
	*value = element->value[0] != 0U;
	return 0;
}

/* Reports whether a ServiceClassIDList names a class. */
static int
sdp_has_class(
	const struct btd_sdp_element *classes,
	uint32_t uuid)
{
	struct btd_sdp_element element;
	uint32_t value;
	size_t offset;
	int error;

	/* Only a sequence lists classes. */
	if (classes->type != BTD_SDP_TYPE_SEQUENCE)
		return 0;

	/* Each UUID of it. */
	offset = 0;
	while (offset < classes->length) {
		error = sdp_element(&classes->value[offset], classes->length - offset, &element);
		if (error != 0)
			return 0;
		offset += element.size;
		error = sdp_uuid(&element, &value);
		if (error == 0 && value == uuid)
			return 1;
	}

	/* Not listed. */
	return 0;
}

/*
 * Reads the L2CAP PSM of a ProtocolDescriptorList (a sequence of protocol
 * descriptors, the L2CAP one a sequence of its UUID and the PSM), or of
 * the first list of an AdditionalProtocolDescriptorLists.  Returns 0 or
 * ENOENT.
 */
static int
sdp_psm(
	const struct btd_sdp_element *protocols,
	uint32_t *psm)
{
	struct btd_sdp_element list;
	struct btd_sdp_element descriptor;
	struct btd_sdp_element uuid;
	struct btd_sdp_element number;
	uint32_t value;
	size_t offset;
	int error;

	/* Refuses anything but a sequence. */
	if (protocols->type != BTD_SDP_TYPE_SEQUENCE || protocols->length == 0U)
		return ENOENT;
	list = *protocols;

	/* The additional lists hold lists: the first one. */
	error = sdp_element(protocols->value, protocols->length, &descriptor);
	if (error != 0 || descriptor.type != BTD_SDP_TYPE_SEQUENCE || descriptor.length == 0U)
		return ENOENT;
	error = sdp_element(descriptor.value, descriptor.length, &uuid);
	if (error == 0 && uuid.type == BTD_SDP_TYPE_SEQUENCE)
		list = descriptor;

	/* Each protocol descriptor: the one of L2CAP carries the PSM after its UUID. */
	offset = 0;
	while (offset < list.length) {
		error = sdp_element(&list.value[offset], list.length - offset, &descriptor);
		if (error != 0 || descriptor.type != BTD_SDP_TYPE_SEQUENCE)
			return ENOENT;
		offset += descriptor.size;
		error = sdp_element(descriptor.value, descriptor.length, &uuid);
		if (error != 0)
			return ENOENT;
		error = sdp_uuid(&uuid, &value);
		if (error != 0 || value != SDP_UUID_L2CAP)
			continue;
		error = sdp_element(&descriptor.value[uuid.size], descriptor.length - uuid.size, &number);
		if (error != 0)
			return ENOENT;
		error = sdp_uint(&number, psm);
		if (error != 0)
			return ENOENT;
		return 0;
	}

	/* No L2CAP. */
	return ENOENT;
}

/*
 * Finds the record of a class in a query's whole lists (a sequence of
 * records, each a sequence of attribute pairs).  Returns 0 with the
 * record, or ENOENT.
 */
static int
sdp_find_record(
	const struct btd_sdp *sdp,
	uint32_t uuid,
	struct btd_sdp_element *record)
{
	int error;

	/* The first of the class. */
	error = sdp_find_nth(sdp, uuid, 0U, record);
	if (error != 0)
		return error;

	/* Succeeded: the record. */
	return 0;
}

/*
 * Finds the nth record (from 0) of a class in a query's whole lists (a
 * phone may list several MAS instances, ws197-p002).  Returns 0 with the
 * record, or ENOENT.
 */
static int
sdp_find_nth(
	const struct btd_sdp *sdp,
	uint32_t uuid,
	unsigned nth,
	struct btd_sdp_element *record)
{
	struct btd_sdp_element lists;
	struct btd_sdp_element value;
	unsigned seen;
	size_t offset;
	int listed;
	int error;

	/* The lists: one sequence. */
	error = sdp_element(sdp->lists, sdp->used, &lists);
	if (error != 0 || lists.type != BTD_SDP_TYPE_SEQUENCE)
		return ENOENT;

	/* Each record, counting those that list the class. */
	seen = 0U;
	offset = 0;
	while (offset < lists.length) {
		error = sdp_element(&lists.value[offset], lists.length - offset, record);
		if (error != 0 || record->type != BTD_SDP_TYPE_SEQUENCE)
			return ENOENT;
		offset += record->size;

		/* Its ServiceClassIDList. */
		error = sdp_record_value(record, SDP_ATTRIBUTE_CLASSES, &value);
		if (error != 0)
			continue;
		listed = sdp_has_class(&value, uuid);
		if (!listed)
			continue;

		/* The one asked for. */
		if (seen == nth)
			return 0;
		seen++;
	}

	/* No such record of the class. */
	return ENOENT;
}

/*
 * Finds an attribute's value in a record's pairs of ID and value.  Returns
 * 0 with the value, or ENOENT (also for pairs that are not well formed).
 */
static int
sdp_record_value(
	const struct btd_sdp_element *record,
	uint32_t attribute,
	struct btd_sdp_element *value)
{
	struct btd_sdp_element pair;
	uint32_t id;
	size_t inner;
	int error;

	/* Each pair, until the one of the attribute. */
	inner = 0;
	while (inner < record->length) {
		/* The ID. */
		error = sdp_element(&record->value[inner], record->length - inner, &pair);
		if (error != 0)
			return ENOENT;
		inner += pair.size;

		/* Its value. */
		error = sdp_element(&record->value[inner], record->length - inner, value);
		if (error != 0)
			return ENOENT;
		inner += value->size;

		/* Succeeded: the one asked for. */
		error = sdp_uint(&pair, &id);
		if (error == 0 && id == attribute)
			return 0;
	}

	/* Not in the record. */
	return ENOENT;
}

/*
 * Reads one of the HID record's attributes bluetoothd keeps (the others
 * are passed over).  Returns 0, EINVAL or E2BIG.
 */
static int
sdp_hid_attribute(
	uint32_t id,
	const struct btd_sdp_element *value,
	struct btd_hid_record *record)
{
	uint32_t number;
	size_t length;
	int error;

	/* The flags. */
	if (id == SDP_ATTRIBUTE_HID_CABLE)
		return sdp_bool(value, &record->virtual_cable);
	if (id == SDP_ATTRIBUTE_HID_RECONNECT)
		return sdp_bool(value, &record->reconnect_initiate);
	if (id == SDP_ATTRIBUTE_HID_CONNECTABLE)
		return sdp_bool(value, &record->normally_connectable);
	if (id == SDP_ATTRIBUTE_HID_BOOT)
		return sdp_bool(value, &record->boot_device);

	/* The subclass and the country, one byte each. */
	if (id == SDP_ATTRIBUTE_HID_SUBCLASS || id == SDP_ATTRIBUTE_HID_COUNTRY) {
		error = sdp_uint(value, &number);
		if (error != 0 || number > 0xffU)
			return EINVAL;
		if (id == SDP_ATTRIBUTE_HID_SUBCLASS)
			record->subclass = (uint8_t)number;
		else
			record->country = (uint8_t)number;
		return 0;
	}

	/* The service's name in the primary language, cut to what is kept. */
	if (id == SDP_ATTRIBUTE_NAME) {
		if (value->type != BTD_SDP_TYPE_TEXT)
			return EINVAL;
		length = value->length;
		if (length >= sizeof(record->name))
			length = sizeof(record->name) - 1U;
		memcpy(record->name, value->value, length);
		record->name[length] = '\0';
		return 0;
	}

	/* The report descriptor. */
	if (id == SDP_ATTRIBUTE_HID_DESCRIPTORS)
		return sdp_descriptor(value, record);

	/* Not one bluetoothd keeps. */
	return 0;
}

/*
 * Reads the report descriptor of a HIDDescriptorList: a sequence of
 * sequences, each a class descriptor type and its bytes (as text).
 * Returns 0, EINVAL, or E2BIG past 4096 bytes.
 */
static int
sdp_descriptor(
	const struct btd_sdp_element *list,
	struct btd_hid_record *record)
{
	struct btd_sdp_element entry;
	struct btd_sdp_element kind;
	struct btd_sdp_element bytes;
	uint32_t type;
	size_t offset;
	int error;

	/* Refuses anything but a sequence. */
	if (list->type != BTD_SDP_TYPE_SEQUENCE)
		return EINVAL;

	/* Each entry, until the report descriptor's. */
	offset = 0;
	while (offset < list->length) {
		error = sdp_element(&list->value[offset], list->length - offset, &entry);
		if (error != 0 || entry.type != BTD_SDP_TYPE_SEQUENCE)
			return EINVAL;
		offset += entry.size;
		error = sdp_element(entry.value, entry.length, &kind);
		if (error != 0)
			return EINVAL;
		error = sdp_uint(&kind, &type);
		if (error != 0)
			return EINVAL;
		error = sdp_element(&entry.value[kind.size], entry.length - kind.size, &bytes);
		if (error != 0 || bytes.type != BTD_SDP_TYPE_TEXT)
			return EINVAL;
		if (type != SDP_DESCRIPTOR_REPORT)
			continue;

		/* Refuses a descriptor past what the input bridge takes. */
		if (bytes.length > BTD_SDP_DESCRIPTOR_MAX)
			return E2BIG;
		record->descriptor = bytes.value;
		record->descriptor_size = bytes.length;
		return 0;
	}

	/* None: the record cannot be used (btd_sdp_hid reports it). */
	return 0;
}

/*
 * Reads the RFCOMM server channel of a ProtocolDescriptorList: the
 * descriptor whose UUID is RFCOMM's carries the channel after it.  Returns
 * 0 or ENOENT.
 */
static int
sdp_rfcomm(
	const struct btd_sdp_element *protocols,
	uint32_t *channel)
{
	struct btd_sdp_element descriptor;
	struct btd_sdp_element uuid;
	struct btd_sdp_element number;
	uint32_t value;
	size_t offset;
	int error;

	/* A sequence of descriptors. */
	if (protocols->type != BTD_SDP_TYPE_SEQUENCE)
		return ENOENT;

	/* Each descriptor, until RFCOMM's. */
	offset = 0;
	while (offset < protocols->length) {
		error = sdp_element(&protocols->value[offset], protocols->length - offset, &descriptor);
		if (error != 0 || descriptor.type != BTD_SDP_TYPE_SEQUENCE)
			return ENOENT;
		offset += descriptor.size;

		/* Its UUID. */
		error = sdp_element(descriptor.value, descriptor.length, &uuid);
		if (error != 0)
			return ENOENT;
		error = sdp_uuid(&uuid, &value);
		if (error != 0 || value != SDP_UUID_RFCOMM)
			continue;

		/* Succeeded: the channel after it. */
		error = sdp_element(&descriptor.value[uuid.size], descriptor.length - uuid.size, &number);
		if (error != 0)
			return ENOENT;
		error = sdp_uint(&number, channel);
		if (error != 0)
			return ENOENT;
		return 0;
	}

	/* No RFCOMM. */
	return ENOENT;
}

/* Reads a big-endian 16-bit number. */
static uint16_t
sdp_be16(
	const uint8_t *bytes)
{
	/* Most significant byte first. */
	return (uint16_t)(((unsigned)bytes[0] << 8) | bytes[1]);
}

/* Writes a big-endian 16-bit number. */
static void
sdp_put16(
	uint8_t *bytes,
	uint16_t value)
{
	/* Most significant byte first. */
	bytes[0] = (uint8_t)(value >> 8);
	bytes[1] = (uint8_t)value;
}
