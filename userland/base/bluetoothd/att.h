/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * bluetoothd's Attribute Protocol (ws143-p005 i02, plan/ws143/phase005/
 * phase.md sections 4.5 and 9.14), without system calls: the client's
 * requests built, the PDUs taken apart with every length checked against
 * the opcode's form, and the smallest server, which answers the peer's
 * requests (bluetoothd holds no attributes of its own).  ATT is
 * little-endian (Core 5.4 Vol 3 Part F section 3.4, values to be checked
 * against it).
 */

#ifndef BLUETOOTHD_ATT_H
#define BLUETOOTHD_ATT_H

#include <stddef.h>
#include <stdint.h>

/* The default MTU of an LE link, and the one bluetoothd asks for. */
#define BTD_ATT_MTU_DEFAULT		23U
#define BTD_ATT_MTU			185U

/* The opcodes. */
#define BTD_ATT_ERROR			0x01U
#define BTD_ATT_MTU_REQUEST		0x02U
#define BTD_ATT_MTU_RESPONSE		0x03U
#define BTD_ATT_FIND_INFO_REQUEST	0x04U
#define BTD_ATT_FIND_INFO_RESPONSE	0x05U
#define BTD_ATT_FIND_BY_VALUE_REQUEST	0x06U
#define BTD_ATT_READ_BY_TYPE_REQUEST	0x08U
#define BTD_ATT_READ_BY_TYPE_RESPONSE	0x09U
#define BTD_ATT_READ_REQUEST		0x0aU
#define BTD_ATT_READ_RESPONSE		0x0bU
#define BTD_ATT_READ_BLOB_REQUEST	0x0cU
#define BTD_ATT_READ_BLOB_RESPONSE	0x0dU
#define BTD_ATT_READ_MULTIPLE_REQUEST	0x0eU
#define BTD_ATT_READ_GROUP_REQUEST	0x10U
#define BTD_ATT_READ_GROUP_RESPONSE	0x11U
#define BTD_ATT_WRITE_REQUEST		0x12U
#define BTD_ATT_WRITE_RESPONSE		0x13U
#define BTD_ATT_PREPARE_WRITE_REQUEST	0x16U
#define BTD_ATT_EXECUTE_WRITE_REQUEST	0x18U
#define BTD_ATT_NOTIFICATION		0x1bU
#define BTD_ATT_INDICATION		0x1dU
#define BTD_ATT_CONFIRMATION		0x1eU
#define BTD_ATT_WRITE_COMMAND		0x52U
#define BTD_ATT_SIGNED_WRITE_COMMAND	0xd2U

/* The command flag of an opcode: no answer is sent to a command. */
#define BTD_ATT_COMMAND_FLAG		0x40U

/* The error codes. */
#define BTD_ATT_INVALID_HANDLE		0x01U
#define BTD_ATT_READ_NOT_PERMITTED	0x02U
#define BTD_ATT_INVALID_PDU		0x04U
#define BTD_ATT_INSUFFICIENT_AUTHENTICATION 0x05U
#define BTD_ATT_REQUEST_NOT_SUPPORTED	0x06U
#define BTD_ATT_INVALID_OFFSET		0x07U
#define BTD_ATT_INSUFFICIENT_AUTHORIZATION 0x08U
#define BTD_ATT_NOT_FOUND		0x0aU
#define BTD_ATT_NOT_LONG		0x0bU
#define BTD_ATT_INSUFFICIENT_KEY_SIZE	0x0cU
#define BTD_ATT_INSUFFICIENT_ENCRYPTION	0x0fU

/* Find Information's formats: 16-bit or 128-bit UUIDs. */
#define BTD_ATT_FORMAT_16		1U
#define BTD_ATT_FORMAT_128		2U

/*
 * A PDU taken apart (pointing into it): its opcode; an error response's
 * request, handle and code; an MTU; a request's handles, offset and type;
 * a list's elements (their count and each one's length); a value.
 */
struct btd_att_pdu {
	uint8_t opcode;
	uint8_t request;
	uint8_t error;
	uint16_t handle;
	uint16_t end;
	uint16_t mtu;
	uint16_t offset;
	uint16_t uuid;
	unsigned format;
	const uint8_t *list;
	size_t count;
	size_t element;
	const uint8_t *value;
	size_t length;
};

size_t btd_att_build_mtu(uint8_t *out, size_t size, uint8_t opcode, uint16_t mtu);
size_t btd_att_build_range(uint8_t *out, size_t size, uint8_t opcode, uint16_t start, uint16_t end, uint16_t uuid);
size_t btd_att_build_read(uint8_t *out, size_t size, uint16_t handle, uint16_t offset, int blob);
size_t btd_att_build_write(uint8_t *out, size_t size, uint8_t opcode, uint16_t handle, const uint8_t *value, size_t length);
size_t btd_att_build_confirmation(uint8_t *out, size_t size);
size_t btd_att_build_error(uint8_t *out, size_t size, uint8_t request, uint16_t handle, uint8_t error);
int btd_att_parse(const uint8_t *pdu, size_t length, struct btd_att_pdu *parsed);
int btd_att_answer(const uint8_t *request, size_t length, uint8_t *out, size_t size, size_t *out_length);

#endif
