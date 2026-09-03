/* Intel AX211 private API89 RX MPDU fixture.
 * Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../../../src/drivers/intel-ax211-rx.h"

#define STATUS_CRC_OK		0x00000001U
#define STATUS_OVERRUN_OK	0x00000002U
#define STATUS_MIC_OK		0x00000040U
#define STATUS_CCMP		0x00000200U
#define STATUS_DECRYPTED	0x00000800U
#define STATUS_DUPLICATE	0x00400000U
#define STATUS_CLEAR		(STATUS_CRC_OK | STATUS_OVERRUN_OK)
#define PHY_INFO_TSF_OVERLOAD	0x0100U

static void
test_api89_version(void)
{
	uint8_t bytes[8U] = {
		INTEL_AX211_RX_MPDU_OPCODE, INTEL_AX211_RX_MPDU_GROUP,
		INTEL_AX211_PROTOCOL_UNKNOWN_VERSION,
		INTEL_AX211_RX_MPDU_NOTIFICATION_VERSION,
		0U, 0U, 0U, 0U
	};
	struct intel_ax211_protocol_command_table table;

	assert(intel_ax211_protocol_command_table_parse(bytes, sizeof(bytes),
	    &table) == INTEL_AX211_PROTOCOL_OK);
	assert(intel_ax211_rx_api89_validate(&table) == INTEL_AX211_RX_OK);
	bytes[3U]--;
	assert(intel_ax211_protocol_command_table_parse(bytes, sizeof(bytes),
	    &table) == INTEL_AX211_PROTOCOL_OK);
	assert(intel_ax211_rx_api89_validate(&table) ==
	    INTEL_AX211_RX_UNSUPPORTED);
	assert(intel_ax211_rx_api89_validate(NULL) == INTEL_AX211_RX_INVALID);
}

static void
put_le16(uint8_t *bytes, uint16_t value)
{
	bytes[0] = (uint8_t)value;
	bytes[1] = (uint8_t)(value >> 8);
}

static void
put_le32(uint8_t *bytes, uint32_t value)
{
	bytes[0] = (uint8_t)value;
	bytes[1] = (uint8_t)(value >> 8);
	bytes[2] = (uint8_t)(value >> 16);
	bytes[3] = (uint8_t)(value >> 24);
}

static void
put_le64(uint8_t *bytes, uint64_t value)
{
	put_le32(bytes, (uint32_t)value);
	put_le32(bytes + 4U, (uint32_t)(value >> 32));
}

static struct intel_ax211_protocol_message
make_message(uint8_t *payload, const uint8_t *frame, size_t length,
	uint8_t mac_flags, uint32_t status)
{
	struct intel_ax211_protocol_message message;

	memset(payload, 0, INTEL_AX211_RX_MPDU_DESCRIPTOR_SIZE + length);
	put_le16(payload, (uint16_t)length);
	payload[3U] = mac_flags;
	put_le32(payload + 12U, status);
	payload[40U] = 42U;
	payload[41U] = 55U;
	payload[42U] = 6U;
	put_le32(payload + 44U, UINT32_C(0x89abcdef));
	put_le64(payload + 48U, UINT64_C(0x0123456789abcdef));
	memcpy(payload + INTEL_AX211_RX_MPDU_DESCRIPTOR_SIZE, frame, length);
	memset(&message, 0, sizeof(message));
	message.opcode = INTEL_AX211_RX_MPDU_OPCODE;
	message.group = INTEL_AX211_RX_MPDU_GROUP;
	message.version = INTEL_AX211_RX_MPDU_NOTIFICATION_VERSION;
	message.generation = 17U;
	message.payload = payload;
	message.payload_length = INTEL_AX211_RX_MPDU_DESCRIPTOR_SIZE + length;
	return message;
}

static void
test_clear_management(void)
{
	uint8_t payload[INTEL_AX211_RX_MPDU_DESCRIPTOR_SIZE + 64U];
	uint8_t frame[32U];
	uint8_t output[64U];
	struct intel_ax211_protocol_message message;
	struct intel_ax211_rx_mpdu decoded;

	memset(frame, 0, sizeof(frame));
	frame[0U] = 0x80U;
	frame[24U] = 0x5aU;
	message = make_message(payload, frame, sizeof(frame), 0U, STATUS_CLEAR);
	assert(intel_ax211_rx_mpdu_decode(&message, 17U, output,
	    sizeof(output), &decoded) == INTEL_AX211_RX_OK);
	assert(decoded.frame == output && decoded.length == sizeof(frame));
	assert(memcmp(output, frame, sizeof(frame)) == 0);
	assert(decoded.rssi_dbm == -42 && decoded.channel == 6U);
	assert(decoded.status == STATUS_CLEAR);
	assert(decoded.cipher == INTEL_AX211_RX_CIPHER_NONE);
	assert(decoded.decrypted == 0U && decoded.key_index == 0U);
	assert(decoded.packet_number == 0U);
	assert(decoded.gp2_on_air_rise == UINT32_C(0x89abcdef));
	assert(decoded.tsf == UINT64_C(0x0123456789abcdef));
	assert(decoded.tsf_valid == 1U);
}

static void
test_transport_alignment_padding(void)
{
	uint8_t payload[INTEL_AX211_RX_MPDU_DESCRIPTOR_SIZE + 64U];
	uint8_t frame[32U];
	uint8_t output[64U];
	struct intel_ax211_protocol_message message;
	struct intel_ax211_rx_mpdu decoded;

	memset(frame, 0, sizeof(frame));
	frame[0U] = 0x80U;
	frame[24U] = 0x5aU;
	message = make_message(payload, frame, sizeof(frame), 0U, STATUS_CLEAR);
	memset(payload + message.payload_length, 0xa5U, 4U);
	message.payload_length += 4U;
	assert(intel_ax211_rx_mpdu_decode(&message, 17U, output,
	    sizeof(output), &decoded) == INTEL_AX211_RX_OK);
	assert(decoded.length == sizeof(frame));
	assert(memcmp(output, frame, sizeof(frame)) == 0);
}

static void
test_tsf_overload(void)
{
	uint8_t payload[INTEL_AX211_RX_MPDU_DESCRIPTOR_SIZE + 64U];
	uint8_t frame[32U];
	uint8_t output[64U];
	struct intel_ax211_protocol_message message;
	struct intel_ax211_rx_mpdu decoded;

	memset(frame, 0, sizeof(frame));
	frame[0U] = 0x80U;
	message = make_message(payload, frame, sizeof(frame), 0U, STATUS_CLEAR);
	put_le16(payload + 5U, PHY_INFO_TSF_OVERLOAD);
	put_le32(payload + 44U, UINT32_C(0x76543210));
	put_le64(payload + 48U, UINT64_C(0xfedcba9876543210));
	assert(intel_ax211_rx_mpdu_decode(&message, 17U, output,
	    sizeof(output), &decoded) == INTEL_AX211_RX_OK);
	assert(decoded.gp2_on_air_rise == UINT32_C(0x76543210));
	assert(decoded.tsf == 0U);
	assert(decoded.tsf_valid == 0U);
}

static void
test_padding(void)
{
	uint8_t payload[INTEL_AX211_RX_MPDU_DESCRIPTOR_SIZE + 64U];
	uint8_t padded[30U];
	uint8_t expected[28U];
	uint8_t output[64U];
	struct intel_ax211_protocol_message message;
	struct intel_ax211_rx_mpdu decoded;

	memset(padded, 0, sizeof(padded));
	padded[0U] = 0x50U;
	padded[24U] = 0xaaU;
	padded[25U] = 0xbbU;
	padded[26U] = 1U;
	padded[27U] = 2U;
	padded[28U] = 3U;
	padded[29U] = 4U;
	memcpy(expected, padded, 24U);
	memcpy(expected + 24U, padded + 26U, 4U);
	message = make_message(payload, padded, sizeof(padded), 0x20U,
	    STATUS_CLEAR);
	assert(intel_ax211_rx_mpdu_decode(&message, 17U, output,
	    sizeof(output), &decoded) == INTEL_AX211_RX_OK);
	assert(decoded.length == sizeof(expected));
	assert(memcmp(output, expected, sizeof(expected)) == 0);
}

static void
test_ccmp(void)
{
	uint8_t payload[INTEL_AX211_RX_MPDU_DESCRIPTOR_SIZE + 64U];
	uint8_t frame[48U];
	uint8_t output[64U];
	struct intel_ax211_protocol_message message;
	struct intel_ax211_rx_mpdu decoded;

	memset(frame, 0, sizeof(frame));
	put_le16(frame, 0x4008U);
	frame[24U] = 1U;
	frame[25U] = 2U;
	frame[27U] = 0xa0U;
	frame[28U] = 3U;
	frame[29U] = 4U;
	frame[30U] = 5U;
	frame[31U] = 6U;
	message = make_message(payload, frame, sizeof(frame), 0U,
	    STATUS_CLEAR | STATUS_MIC_OK | STATUS_CCMP | STATUS_DECRYPTED);
	payload[2U] = 0x40U; /* Eight bytes of hardware MIC/CRC trailer. */
	assert(intel_ax211_rx_mpdu_decode(&message, 17U, output,
	    sizeof(output), &decoded) == INTEL_AX211_RX_OK);
	assert(decoded.length == sizeof(frame));
	assert(decoded.cipher == INTEL_AX211_RX_CIPHER_CCMP);
	assert(decoded.decrypted == 1U && decoded.key_index == 2U);
	assert(decoded.packet_number == UINT64_C(0x060504030201));
}

static void
test_ccmp_padding(void)
{
	uint8_t payload[INTEL_AX211_RX_MPDU_DESCRIPTOR_SIZE + 64U];
	uint8_t padded[50U];
	uint8_t expected[48U];
	uint8_t output[64U];
	struct intel_ax211_protocol_message message;
	struct intel_ax211_rx_mpdu decoded;

	memset(padded, 0, sizeof(padded));
	put_le16(padded, 0x4008U);
	padded[24U] = 1U;
	padded[25U] = 2U;
	padded[27U] = 0xa0U;
	padded[28U] = 3U;
	padded[29U] = 4U;
	padded[30U] = 5U;
	padded[31U] = 6U;
	padded[32U] = 0xaaU;
	padded[33U] = 0xbbU;
	padded[34U] = 0x42U;
	memcpy(expected, padded, 32U);
	memcpy(expected + 32U, padded + 34U, sizeof(expected) - 32U);
	message = make_message(payload, padded, sizeof(padded), 0x20U,
	    STATUS_CLEAR | STATUS_MIC_OK | STATUS_CCMP | STATUS_DECRYPTED);
	payload[2U] = 0x40U; /* Eight bytes of hardware MIC/CRC trailer. */
	assert(intel_ax211_rx_mpdu_decode(&message, 17U, output,
	    sizeof(output), &decoded) == INTEL_AX211_RX_OK);
	assert(decoded.length == sizeof(expected));
	assert(memcmp(output, expected, sizeof(expected)) == 0);
	assert(decoded.cipher == INTEL_AX211_RX_CIPHER_CCMP);
	assert(decoded.packet_number == UINT64_C(0x060504030201));
}

static void
test_hardware_trailer_normalization(void)
{
	uint8_t payload[INTEL_AX211_RX_MPDU_DESCRIPTOR_SIZE + 64U];
	uint8_t frame[40U];
	uint8_t output[64U];
	struct intel_ax211_protocol_message message;
	struct intel_ax211_rx_mpdu decoded;

	memset(frame, 0, sizeof(frame));
	frame[0U] = 0x80U;
	message = make_message(payload, frame, 32U, 0U, STATUS_CLEAR);
	payload[2U] = 0x20U; /* Four-byte FCS, absent from common output. */
	assert(intel_ax211_rx_mpdu_decode(&message, 17U, output,
	    sizeof(output), &decoded) == INTEL_AX211_RX_OK);
	assert(decoded.length == 28U);

	memset(frame, 0, sizeof(frame));
	put_le16(frame, 0x4008U);
	frame[24U] = 1U;
	frame[25U] = 2U;
	frame[27U] = 0xa0U;
	frame[28U] = 3U;
	frame[29U] = 4U;
	frame[30U] = 5U;
	frame[31U] = 6U;
	frame[32U] = 0xaaU;
	message = make_message(payload, frame, sizeof(frame), 0U,
	    STATUS_CLEAR | STATUS_MIC_OK | STATUS_CCMP | STATUS_DECRYPTED);
	/* Firmware stripped the hardware trailer; restore the common 8-byte
	 * verified-MIC shape without manufacturing payload bytes. */
	assert(intel_ax211_rx_mpdu_decode(&message, 17U, output,
	    sizeof(output), &decoded) == INTEL_AX211_RX_OK);
	assert(decoded.length == sizeof(frame) + 8U);
	assert(memcmp(output, frame, sizeof(frame)) == 0);
	assert(output[40U] == 0U && output[47U] == 0U);
}

static void
test_header_shapes(void)
{
	uint8_t payload[INTEL_AX211_RX_MPDU_DESCRIPTOR_SIZE + 64U];
	uint8_t frame[40U];
	uint8_t output[64U];
	struct intel_ax211_protocol_message message;
	struct intel_ax211_rx_mpdu decoded;

	memset(frame, 0, sizeof(frame));
	put_le16(frame, 0x8388U);
	message = make_message(payload, frame, sizeof(frame), 0U, STATUS_CLEAR);
	assert(intel_ax211_rx_mpdu_decode(&message, 17U, output,
	    sizeof(output), &decoded) == INTEL_AX211_RX_OK);
	put_le16(frame, 0x00d4U);
	message = make_message(payload, frame, 10U, 0U, STATUS_CLEAR);
	assert(intel_ax211_rx_mpdu_decode(&message, 17U, output,
	    sizeof(output), &decoded) == INTEL_AX211_RX_OK);
}

static void
test_envelope_rejections(void)
{
	uint8_t payload[INTEL_AX211_RX_MPDU_DESCRIPTOR_SIZE + 64U];
	uint8_t frame[32U];
	uint8_t output[64U];
	struct intel_ax211_protocol_message message;
	struct intel_ax211_rx_mpdu decoded;

	memset(frame, 0, sizeof(frame));
	frame[0U] = 0x80U;
	message = make_message(payload, frame, sizeof(frame), 0U, STATUS_CLEAR);
	assert(intel_ax211_rx_mpdu_decode(&message, 16U, output,
	    sizeof(output), &decoded) == INTEL_AX211_RX_STALE);
	message.group = 1U;
	assert(intel_ax211_rx_mpdu_decode(&message, 17U, output,
	    sizeof(output), &decoded) == INTEL_AX211_RX_UNSUPPORTED);
	message.group = 0U;
	message.version = 4U;
	assert(intel_ax211_rx_mpdu_decode(&message, 17U, output,
	    sizeof(output), &decoded) == INTEL_AX211_RX_UNSUPPORTED);
	message.version = INTEL_AX211_RX_MPDU_NOTIFICATION_VERSION;
	message.flags = INTEL_AX211_PROTOCOL_COMMAND_FAILED_MASK;
	assert(intel_ax211_rx_mpdu_decode(&message, 17U, output,
	    sizeof(output), &decoded) == INTEL_AX211_RX_FAILED);
	message.flags = 0U;
	message.payload_length = INTEL_AX211_RX_MPDU_DESCRIPTOR_SIZE - 1U;
	assert(intel_ax211_rx_mpdu_decode(&message, 17U, output,
	    sizeof(output), &decoded) == INTEL_AX211_RX_TRUNCATED);
	message.payload_length = INTEL_AX211_RX_MPDU_DESCRIPTOR_SIZE +
	    sizeof(frame) - 1U;
	assert(intel_ax211_rx_mpdu_decode(&message, 17U, output,
	    sizeof(output), &decoded) == INTEL_AX211_RX_TRUNCATED);
	message.payload_length += 2U;
	assert(intel_ax211_rx_mpdu_decode(&message, 17U, output,
	    sizeof(output), &decoded) == INTEL_AX211_RX_OK);
	message.payload_length--;
	assert(intel_ax211_rx_mpdu_decode(&message, 17U, output, 31U,
	    &decoded) == INTEL_AX211_RX_BUFFER_TOO_SMALL);
	put_le16(payload, INTEL_AX211_RX_MPDU_FRAME_MAX + 1U);
	assert(intel_ax211_rx_mpdu_decode(&message, 17U, output,
	    sizeof(output), &decoded) == INTEL_AX211_RX_OVERSIZED);
}

static void
test_frame_rejections(void)
{
	uint8_t payload[INTEL_AX211_RX_MPDU_DESCRIPTOR_SIZE + 64U];
	uint8_t frame[40U];
	uint8_t output[64U];
	struct intel_ax211_protocol_message message;
	struct intel_ax211_rx_mpdu decoded;

	memset(frame, 0, sizeof(frame));
	frame[0U] = 0x80U;
	message = make_message(payload, frame, 32U, 0U, STATUS_OVERRUN_OK);
	assert(intel_ax211_rx_mpdu_decode(&message, 17U, output,
	    sizeof(output), &decoded) == INTEL_AX211_RX_FAILED);
	message = make_message(payload, frame, 32U, 0U,
	    STATUS_CLEAR | STATUS_DUPLICATE);
	assert(intel_ax211_rx_mpdu_decode(&message, 17U, output,
	    sizeof(output), &decoded) == INTEL_AX211_RX_DUPLICATE);
	message = make_message(payload, frame, 32U, 0x40U, STATUS_CLEAR);
	assert(intel_ax211_rx_mpdu_decode(&message, 17U, output,
	    sizeof(output), &decoded) == INTEL_AX211_RX_UNSUPPORTED);
	message = make_message(payload, frame, 32U, 0U, STATUS_CLEAR | 0x300U);
	assert(intel_ax211_rx_mpdu_decode(&message, 17U, output,
	    sizeof(output), &decoded) == INTEL_AX211_RX_UNSUPPORTED);
	message = make_message(payload, frame, 32U, 0U, STATUS_CLEAR);
	payload[42U] = 0U;
	assert(intel_ax211_rx_mpdu_decode(&message, 17U, output,
	    sizeof(output), &decoded) == INTEL_AX211_RX_FAILED);

	memset(frame, 0, sizeof(frame));
	put_le16(frame, 0x4008U);
	frame[27U] = 0x20U;
	message = make_message(payload, frame, sizeof(frame), 0U,
	    STATUS_CLEAR | STATUS_MIC_OK | STATUS_CCMP | STATUS_DECRYPTED);
	assert(intel_ax211_rx_mpdu_decode(&message, 17U, output,
	    sizeof(output), &decoded) == INTEL_AX211_RX_FAILED);
	frame[24U] = 1U;
	message = make_message(payload, frame, sizeof(frame), 0U,
	    STATUS_CLEAR | STATUS_CCMP | STATUS_DECRYPTED);
	assert(intel_ax211_rx_mpdu_decode(&message, 17U, output,
	    sizeof(output), &decoded) == INTEL_AX211_RX_FAILED);
	put_le16(frame, 0x000cU);
	message = make_message(payload, frame, sizeof(frame), 0U, STATUS_CLEAR);
	assert(intel_ax211_rx_mpdu_decode(&message, 17U, output,
	    sizeof(output), &decoded) == INTEL_AX211_RX_UNSUPPORTED);
	memset(frame, 0, sizeof(frame));
	frame[0U] = 0x80U;
	message = make_message(payload, frame, 25U, 0x20U, STATUS_CLEAR);
	assert(intel_ax211_rx_mpdu_decode(&message, 17U, output,
	    sizeof(output), &decoded) == INTEL_AX211_RX_TRUNCATED);
}

int
main(void)
{
	test_api89_version();
	test_clear_management();
	test_transport_alignment_padding();
	test_tsf_overload();
	test_padding();
	test_ccmp();
	test_ccmp_padding();
	test_hardware_trailer_normalization();
	test_header_shapes();
	test_envelope_rejections();
	test_frame_rejections();
	puts("intel ax211 RX MPDU fixture: PASS");
	return 0;
}
