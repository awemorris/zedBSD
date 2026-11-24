/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * T.125 MCS and T.124 GCC Conference Create encoding and decoding.
 */

#include "rdp-internal.h"

#include <string.h>

/* MCS BER application tags. */
#define MCS_TAG_CONNECT_INITIAL		0x65U
#define MCS_TAG_CONNECT_RESPONSE	0x66U

/* MCS PER DomainMCSPDU choices (upper 6 bits of byte 0). */
#define MCS_CHOICE_ERECT_DOMAIN		1U
#define MCS_CHOICE_DISCONNECT		8U
#define MCS_CHOICE_ATTACH_USER_REQ	10U
#define MCS_CHOICE_ATTACH_USER_CONF	11U
#define MCS_CHOICE_CHANNEL_JOIN_REQ	14U
#define MCS_CHOICE_CHANNEL_JOIN_CONF	15U
#define MCS_CHOICE_SEND_DATA_REQ	25U
#define MCS_CHOICE_SEND_DATA_IND	26U

/* Client and server GCC User Data block types. */
#define UDATA_CS_CORE			0xC001U
#define UDATA_CS_SECURITY		0xC002U
#define UDATA_CS_NET			0xC003U
#define UDATA_SC_CORE			0x0C01U
#define UDATA_SC_SECURITY		0x0C02U
#define UDATA_SC_NET			0x0C03U

/* Early capability flag indicating RDPEGFX over DRDYNVC support. */
#define RNS_UD_CS_SUPPORT_ERRINFO	0x0001U
#define RNS_UD_CS_WANT_32BPP		0x0002U
#define RNS_UD_CS_SUPPORT_DYNVC_GFX	0x0400U

static int ber_append_len(struct rdp_buf *buf, size_t len);
static int ber_append_int(struct rdp_buf *buf, uint32_t val);
static int ber_append_octet_string(struct rdp_buf *buf, const uint8_t *data, size_t len);
static int ber_append_domain_params(struct rdp_buf *buf, uint32_t max_ch, uint32_t max_usr, uint32_t max_tok, uint32_t num_prio, uint32_t min_tp, uint32_t max_ht, uint32_t max_pdu, uint32_t proto_ver);
static int ber_read_len(struct rdp_reader *r, size_t *out_len);
static int ber_read_tlv(struct rdp_reader *r, uint8_t tag, const uint8_t **val, size_t *val_len);
static int per_append_len(struct rdp_buf *buf, size_t len);
static int per_read_len(struct rdp_reader *r, size_t *out_len);
static int append_cs_core(struct rdp_buf *buf, uint32_t width, uint32_t height, int want_drdynvc);
static int append_cs_security(struct rdp_buf *buf);
static int append_cs_net(struct rdp_buf *buf, int want_drdynvc);
static int parse_user_data_blocks(const uint8_t *data, size_t len, struct rdp_mcs_connect_info *info);

/*
 * Encodes an MCS Connect-Initial PDU carrying GCC and RDP user data.
 */
int
rdp_mcs_encode_connect_initial(
	struct rdp_buf *out,
	uint32_t width,
	uint32_t height,
	int want_drdynvc)
{
	struct rdp_buf udata;
	struct rdp_buf gcc;
	struct rdp_buf body;
	uint8_t calling_sel[1];
	uint8_t called_sel[1];
	int error;

	/* Initializes scratch buffers for user data, GCC, and BER body. */
	rdp_buf_init(&udata);
	rdp_buf_init(&gcc);
	rdp_buf_init(&body);

	/* Builds the client GCC User Data blocks (Core, Security, Net). */
	error = append_cs_core(&udata, width, height, want_drdynvc);
	if (error != RDP_OK)
		goto fail;

	error = append_cs_security(&udata);
	if (error != RDP_OK)
		goto fail;

	error = append_cs_net(&udata, want_drdynvc);
	if (error != RDP_OK)
		goto fail;

	/* Wraps user data in the T.124 GCC Conference Create Request header. */
	{
		static const uint8_t gcc_hdr_prefix[7] = {
			0x00U, 0x05U, 0x00U, 0x14U, 0x7CU, 0x00U, 0x01U
		};
		static const uint8_t gcc_hdr_mid[12] = {
			0x00U, 0x08U, 0x00U, 0x10U, 0x00U, 0x01U,
			0xC0U, 0x00U, 0x44U, 0x75U, 0x63U, 0x61U
		};

		error = rdp_buf_append(&gcc, gcc_hdr_prefix, sizeof(gcc_hdr_prefix));
		if (error != RDP_OK)
			goto fail;

		error = per_append_len(&gcc, udata.len + 14U);
		if (error != RDP_OK)
			goto fail;

		error = rdp_buf_append(&gcc, gcc_hdr_mid, sizeof(gcc_hdr_mid));
		if (error != RDP_OK)
			goto fail;

		error = per_append_len(&gcc, udata.len);
		if (error != RDP_OK)
			goto fail;

		error = rdp_buf_append(&gcc, udata.data, udata.len);
		if (error != RDP_OK)
			goto fail;
	}

	/* Encodes calling/called domain selectors and upwardFlag. */
	calling_sel[0] = 0x01U;
	called_sel[0] = 0x01U;
	error = ber_append_octet_string(&body, calling_sel, 1U);
	if (error != RDP_OK)
		goto fail;

	error = ber_append_octet_string(&body, called_sel, 1U);
	if (error != RDP_OK)
		goto fail;

	error = rdp_buf_append_u8(&body, 0x01U);
	if (error != RDP_OK)
		goto fail;

	error = rdp_buf_append_u8(&body, 0x01U);
	if (error != RDP_OK)
		goto fail;

	error = rdp_buf_append_u8(&body, 0xFFU);
	if (error != RDP_OK)
		goto fail;

	/* Appends target, minimum, and maximum MCS DomainParameters. */
	error = ber_append_domain_params(&body, 34U, 2U, 0U, 1U, 0U, 1U, 65535U, 2U);
	if (error != RDP_OK)
		goto fail;

	error = ber_append_domain_params(&body, 1U, 1U, 1U, 1U, 0U, 1U, 1056U, 2U);
	if (error != RDP_OK)
		goto fail;

	error = ber_append_domain_params(&body, 65535U, 64535U, 65535U, 1U, 0U, 1U, 65535U, 2U);
	if (error != RDP_OK)
		goto fail;

	/* Appends the GCC payload as the BER userData OCTET STRING. */
	error = ber_append_octet_string(&body, gcc.data, gcc.len);
	if (error != RDP_OK)
		goto fail;

	/* Emits the outer BER APPLICATION 101 tag and length. */
	error = rdp_buf_append_u8(out, 0x7FU);
	if (error != RDP_OK)
		goto fail;

	error = rdp_buf_append_u8(out, MCS_TAG_CONNECT_INITIAL);
	if (error != RDP_OK)
		goto fail;

	error = ber_append_len(out, body.len);
	if (error != RDP_OK)
		goto fail;

	error = rdp_buf_append(out, body.data, body.len);
	if (error != RDP_OK)
		goto fail;

	rdp_buf_free(&udata);
	rdp_buf_free(&gcc);
	rdp_buf_free(&body);

	/* Succeeded: MCS Connect-Initial encoded. */
	return RDP_OK;

fail:
	rdp_buf_free(&udata);
	rdp_buf_free(&gcc);
	rdp_buf_free(&body);
	return error;
}

/*
 * Decodes an MCS Connect-Initial PDU and extracts client core/net settings.
 */
int
rdp_mcs_decode_connect_initial(
	const uint8_t *data,
	size_t len,
	struct rdp_mcs_connect_info *info)
{
	struct rdp_reader r;
	uint8_t b0;
	uint8_t b1;
	size_t ci_len;
	const uint8_t *slice;
	size_t slice_len;
	const uint8_t *gcc_data;
	size_t gcc_len;
	size_t dummy_len;
	int error;

	/* Clears output and initializes the reader over the Connect-Initial. */
	memset(info, 0, sizeof(*info));
	rdp_reader_init(&r, data, len);

	/* Verifies the BER APPLICATION 101 tag. */
	error = rdp_read_u8(&r, &b0);
	if (error != RDP_OK)
		return error;

	error = rdp_read_u8(&r, &b1);
	if (error != RDP_OK)
		return error;

	if (b0 != 0x7FU || b1 != MCS_TAG_CONNECT_INITIAL)
		return RDP_ERR_PROTO;

	error = ber_read_len(&r, &ci_len);
	if (error != RDP_OK)
		return error;

	if (rdp_reader_rem(&r) < ci_len)
		return RDP_ERR_PROTO;

	/* Skips selectors, upwardFlag, and the three DomainParameters sequences. */
	error = ber_read_tlv(&r, 0x04U, &slice, &slice_len);
	if (error != RDP_OK)
		return error;

	error = ber_read_tlv(&r, 0x04U, &slice, &slice_len);
	if (error != RDP_OK)
		return error;

	error = ber_read_tlv(&r, 0x01U, &slice, &slice_len);
	if (error != RDP_OK)
		return error;

	error = ber_read_tlv(&r, 0x30U, &slice, &slice_len);
	if (error != RDP_OK)
		return error;

	error = ber_read_tlv(&r, 0x30U, &slice, &slice_len);
	if (error != RDP_OK)
		return error;

	error = ber_read_tlv(&r, 0x30U, &slice, &slice_len);
	if (error != RDP_OK)
		return error;

	/* Extracts the userData OCTET STRING containing GCC + RDP blocks. */
	error = ber_read_tlv(&r, 0x04U, &gcc_data, &gcc_len);
	if (error != RDP_OK)
		return error;

	/* Skips the 7-byte T.124 prefix, PER length, and 12-byte key ("Duca"). */
	rdp_reader_init(&r, gcc_data, gcc_len);
	error = rdp_skip_bytes(&r, 7U);
	if (error != RDP_OK)
		return error;

	error = per_read_len(&r, &dummy_len);
	if (error != RDP_OK)
		return error;

	error = rdp_skip_bytes(&r, 12U);
	if (error != RDP_OK)
		return error;

	error = per_read_len(&r, &slice_len);
	if (error != RDP_OK)
		return error;

	if (rdp_reader_rem(&r) < slice_len)
		return RDP_ERR_PROTO;

	/* Parses the Client User Data blocks. */
	error = parse_user_data_blocks(r.data + r.pos, slice_len, info);
	if (error != RDP_OK)
		return error;

	/* Succeeded: MCS Connect-Initial decoded. */
	return RDP_OK;
}

/*
 * Encodes an MCS Connect-Response PDU with SC_CORE, SC_SEC, and SC_NET.
 */
int
rdp_mcs_encode_connect_response(
	struct rdp_buf *out,
	uint16_t drdynvc_channel)
{
	struct rdp_buf udata;
	struct rdp_buf gcc;
	struct rdp_buf body;
	uint8_t call_id[1];
	int error;

	/* Initializes scratch buffers for server user data, GCC, and BER body. */
	rdp_buf_init(&udata);
	rdp_buf_init(&gcc);
	rdp_buf_init(&body);

	/* Appends SC_CORE (0x0C01, length 12). */
	error = rdp_buf_append_u16_le(&udata, UDATA_SC_CORE);
	if (error != RDP_OK)
		goto fail;

	error = rdp_buf_append_u16_le(&udata, 12U);
	if (error != RDP_OK)
		goto fail;

	error = rdp_buf_append_u32_le(&udata, 0x00080004U);
	if (error != RDP_OK)
		goto fail;

	error = rdp_buf_append_u32_le(&udata, RDP_NEG_PROTO_SSL);
	if (error != RDP_OK)
		goto fail;

	/* Appends SC_SECURITY (0x0C02, length 12, encryptionMethod=0). */
	error = rdp_buf_append_u16_le(&udata, UDATA_SC_SECURITY);
	if (error != RDP_OK)
		goto fail;

	error = rdp_buf_append_u16_le(&udata, 12U);
	if (error != RDP_OK)
		goto fail;

	error = rdp_buf_append_u32_le(&udata, 0U);
	if (error != RDP_OK)
		goto fail;

	error = rdp_buf_append_u32_le(&udata, 0U);
	if (error != RDP_OK)
		goto fail;

	/* Appends SC_NET (0x0C03) announcing the drdynvc static channel ID. */
	if (drdynvc_channel != 0U) {
		error = rdp_buf_append_u16_le(&udata, UDATA_SC_NET);
		if (error != RDP_OK)
			goto fail;

		error = rdp_buf_append_u16_le(&udata, 12U);
		if (error != RDP_OK)
			goto fail;

		error = rdp_buf_append_u16_le(&udata, RDP_MCS_IO_CHANNEL);
		if (error != RDP_OK)
			goto fail;

		error = rdp_buf_append_u16_le(&udata, 1U);
		if (error != RDP_OK)
			goto fail;

		error = rdp_buf_append_u16_le(&udata, drdynvc_channel);
		if (error != RDP_OK)
			goto fail;

		error = rdp_buf_append_u16_le(&udata, 0U);
		if (error != RDP_OK)
			goto fail;
	} else {
		error = rdp_buf_append_u16_le(&udata, UDATA_SC_NET);
		if (error != RDP_OK)
			goto fail;

		error = rdp_buf_append_u16_le(&udata, 8U);
		if (error != RDP_OK)
			goto fail;

		error = rdp_buf_append_u16_le(&udata, RDP_MCS_IO_CHANNEL);
		if (error != RDP_OK)
			goto fail;

		error = rdp_buf_append_u16_le(&udata, 0U);
		if (error != RDP_OK)
			goto fail;
	}

	/* Wraps server user data in GCC Conference Create Response. */
	{
		static const uint8_t gcc_rsp_hdr[14] = {
			0x00U, 0x05U, 0x00U, 0x14U, 0x7CU, 0x00U, 0x01U,
			0x2AU, 0x14U, 0x76U, 0x0AU, 0x01U, 0x01U, 0x00U
		};
		static const uint8_t gcc_rsp_key[6] = {
			0x01U, 0xC0U, 0x4DU, 0x63U, 0x44U, 0x6EU
		};

		error = rdp_buf_append(&gcc, gcc_rsp_hdr, sizeof(gcc_rsp_hdr));
		if (error != RDP_OK)
			goto fail;

		error = rdp_buf_append(&gcc, gcc_rsp_key, sizeof(gcc_rsp_key));
		if (error != RDP_OK)
			goto fail;

		error = per_append_len(&gcc, udata.len);
		if (error != RDP_OK)
			goto fail;

		error = rdp_buf_append(&gcc, udata.data, udata.len);
		if (error != RDP_OK)
			goto fail;
	}

	/* Encodes result (ENUMERATED 0 = rt-successful) and calledConnectId. */
	call_id[0] = 0U;
	error = rdp_buf_append_u8(&body, 0x0AU);
	if (error != RDP_OK)
		goto fail;

	error = rdp_buf_append_u8(&body, 0x01U);
	if (error != RDP_OK)
		goto fail;

	error = rdp_buf_append_u8(&body, 0x00U);
	if (error != RDP_OK)
		goto fail;

	error = rdp_buf_append_u8(&body, 0x02U);
	if (error != RDP_OK)
		goto fail;

	error = rdp_buf_append_u8(&body, 0x01U);
	if (error != RDP_OK)
		goto fail;

	error = rdp_buf_append(&body, call_id, 1U);
	if (error != RDP_OK)
		goto fail;

	error = ber_append_domain_params(&body, 34U, 3U, 0U, 1U, 0U, 1U, 65535U, 2U);
	if (error != RDP_OK)
		goto fail;

	error = ber_append_octet_string(&body, gcc.data, gcc.len);
	if (error != RDP_OK)
		goto fail;

	/* Emits outer BER APPLICATION 102 tag and length. */
	error = rdp_buf_append_u8(out, 0x7FU);
	if (error != RDP_OK)
		goto fail;

	error = rdp_buf_append_u8(out, MCS_TAG_CONNECT_RESPONSE);
	if (error != RDP_OK)
		goto fail;

	error = ber_append_len(out, body.len);
	if (error != RDP_OK)
		goto fail;

	error = rdp_buf_append(out, body.data, body.len);
	if (error != RDP_OK)
		goto fail;

	rdp_buf_free(&udata);
	rdp_buf_free(&gcc);
	rdp_buf_free(&body);

	/* Succeeded: MCS Connect-Response encoded. */
	return RDP_OK;

fail:
	rdp_buf_free(&udata);
	rdp_buf_free(&gcc);
	rdp_buf_free(&body);
	return error;
}

/*
 * Decodes an MCS Connect-Response PDU and extracts server channel IDs.
 */
int
rdp_mcs_decode_connect_response(
	const uint8_t *data,
	size_t len,
	struct rdp_mcs_connect_info *info)
{
	struct rdp_reader r;
	uint8_t b0;
	uint8_t b1;
	size_t cr_len;
	const uint8_t *slice;
	size_t slice_len;
	const uint8_t *gcc_data;
	size_t gcc_len;
	int error;

	/* Clears output and initializes the reader. */
	memset(info, 0, sizeof(*info));
	rdp_reader_init(&r, data, len);

	/* Verifies the BER APPLICATION 102 tag. */
	error = rdp_read_u8(&r, &b0);
	if (error != RDP_OK)
		return error;

	error = rdp_read_u8(&r, &b1);
	if (error != RDP_OK)
		return error;

	if (b0 != 0x7FU || b1 != MCS_TAG_CONNECT_RESPONSE)
		return RDP_ERR_PROTO;

	error = ber_read_len(&r, &cr_len);
	if (error != RDP_OK)
		return error;

	if (rdp_reader_rem(&r) < cr_len)
		return RDP_ERR_PROTO;

	/* Checks ENUMERATED result == 0 (rt-successful). */
	error = ber_read_tlv(&r, 0x0AU, &slice, &slice_len);
	if (error != RDP_OK || slice_len != 1U || slice[0] != 0U)
		return RDP_ERR_PROTO;

	/* Skips calledConnectId and DomainParameters. */
	error = ber_read_tlv(&r, 0x02U, &slice, &slice_len);
	if (error != RDP_OK)
		return error;

	error = ber_read_tlv(&r, 0x30U, &slice, &slice_len);
	if (error != RDP_OK)
		return error;

	/* Extracts userData OCTET STRING containing GCC response + server blocks. */
	error = ber_read_tlv(&r, 0x04U, &gcc_data, &gcc_len);
	if (error != RDP_OK)
		return error;

	/* Skips the 20-byte GCC Conference Create Response header. */
	rdp_reader_init(&r, gcc_data, gcc_len);
	error = rdp_skip_bytes(&r, 20U);
	if (error != RDP_OK)
		return error;

	error = per_read_len(&r, &slice_len);
	if (error != RDP_OK)
		return error;

	if (rdp_reader_rem(&r) < slice_len)
		return RDP_ERR_PROTO;

	/* Parses the Server User Data blocks. */
	error = parse_user_data_blocks(r.data + r.pos, slice_len, info);
	if (error != RDP_OK)
		return error;

	/* Succeeded: MCS Connect-Response decoded. */
	return RDP_OK;
}

/*
 * Encodes an MCS ErectDomainRequest PDU.
 */
int
rdp_mcs_encode_erect_domain(
	struct rdp_buf *out)
{
	static const uint8_t pdu[5] = {
		(uint8_t)(MCS_CHOICE_ERECT_DOMAIN << 2), 0x01U, 0x00U, 0x01U, 0x00U
	};
	int error;

	/* Appends the fixed 5-byte ErectDomainRequest PDU. */
	error = rdp_buf_append(out, pdu, sizeof(pdu));
	if (error != RDP_OK)
		return error;

	/* Succeeded: ErectDomainRequest encoded. */
	return RDP_OK;
}

/*
 * Encodes an MCS AttachUserRequest PDU.
 */
int
rdp_mcs_encode_attach_user_req(
	struct rdp_buf *out)
{
	int error;

	/* Appends the single-byte AttachUserRequest PDU. */
	error = rdp_buf_append_u8(out, (uint8_t)(MCS_CHOICE_ATTACH_USER_REQ << 2));
	if (error != RDP_OK)
		return error;

	/* Succeeded: AttachUserRequest encoded. */
	return RDP_OK;
}

/*
 * Encodes an MCS AttachUserConfirm PDU carrying user_id.
 */
int
rdp_mcs_encode_attach_user_confirm(
	struct rdp_buf *out,
	uint16_t user_id)
{
	int error;

	/* Rejects user IDs below the MCS base 1001. */
	if (user_id < RDP_MCS_USER_BASE)
		return RDP_ERR_INVAL;

	/* Sets initiator-present bit (bit 1) with result=0 (rt-successful). */
	error = rdp_buf_append_u8(out, (uint8_t)((MCS_CHOICE_ATTACH_USER_CONF << 2) | 0x02U));
	if (error != RDP_OK)
		return error;

	error = rdp_buf_append_u8(out, 0x00U);
	if (error != RDP_OK)
		return error;

	error = rdp_buf_append_u16_be(out, (uint16_t)(user_id - RDP_MCS_USER_BASE));
	if (error != RDP_OK)
		return error;

	/* Succeeded: AttachUserConfirm encoded. */
	return RDP_OK;
}

/*
 * Encodes an MCS ChannelJoinRequest PDU.
 */
int
rdp_mcs_encode_channel_join_req(
	struct rdp_buf *out,
	uint16_t user_id,
	uint16_t channel_id)
{
	int error;

	/* Rejects user IDs below the MCS base 1001. */
	if (user_id < RDP_MCS_USER_BASE)
		return RDP_ERR_INVAL;

	/* Emits choice, relative initiator, and requested channel ID. */
	error = rdp_buf_append_u8(out, (uint8_t)(MCS_CHOICE_CHANNEL_JOIN_REQ << 2));
	if (error != RDP_OK)
		return error;

	error = rdp_buf_append_u16_be(out, (uint16_t)(user_id - RDP_MCS_USER_BASE));
	if (error != RDP_OK)
		return error;

	error = rdp_buf_append_u16_be(out, channel_id);
	if (error != RDP_OK)
		return error;

	/* Succeeded: ChannelJoinRequest encoded. */
	return RDP_OK;
}

/*
 * Encodes an MCS ChannelJoinConfirm PDU.
 */
int
rdp_mcs_encode_channel_join_confirm(
	struct rdp_buf *out,
	uint16_t user_id,
	uint16_t channel_id)
{
	int error;

	/* Rejects user IDs below the MCS base 1001. */
	if (user_id < RDP_MCS_USER_BASE)
		return RDP_ERR_INVAL;

	/* Sets channelId-present bit (bit 1) with result=0 (rt-successful). */
	error = rdp_buf_append_u8(out, (uint8_t)((MCS_CHOICE_CHANNEL_JOIN_CONF << 2) | 0x02U));
	if (error != RDP_OK)
		return error;

	error = rdp_buf_append_u8(out, 0x00U);
	if (error != RDP_OK)
		return error;

	error = rdp_buf_append_u16_be(out, (uint16_t)(user_id - RDP_MCS_USER_BASE));
	if (error != RDP_OK)
		return error;

	error = rdp_buf_append_u16_be(out, channel_id);
	if (error != RDP_OK)
		return error;

	error = rdp_buf_append_u16_be(out, channel_id);
	if (error != RDP_OK)
		return error;

	/* Succeeded: ChannelJoinConfirm encoded. */
	return RDP_OK;
}

/*
 * Encodes an MCS SendDataRequest PDU from client to server.
 */
int
rdp_mcs_encode_send_data_req(
	struct rdp_buf *out,
	uint16_t user_id,
	uint16_t channel_id,
	const uint8_t *payload,
	size_t len)
{
	int error;

	/* Rejects user IDs below the MCS base 1001. */
	if (user_id < RDP_MCS_USER_BASE)
		return RDP_ERR_INVAL;

	/* Emits SendDataRequest header, priority=high, segmentation=begin|end. */
	error = rdp_buf_append_u8(out, (uint8_t)(MCS_CHOICE_SEND_DATA_REQ << 2));
	if (error != RDP_OK)
		return error;

	error = rdp_buf_append_u16_be(out, (uint16_t)(user_id - RDP_MCS_USER_BASE));
	if (error != RDP_OK)
		return error;

	error = rdp_buf_append_u16_be(out, channel_id);
	if (error != RDP_OK)
		return error;

	error = rdp_buf_append_u8(out, 0x70U);
	if (error != RDP_OK)
		return error;

	error = per_append_len(out, len);
	if (error != RDP_OK)
		return error;

	error = rdp_buf_append(out, payload, len);
	if (error != RDP_OK)
		return error;

	/* Succeeded: SendDataRequest encoded. */
	return RDP_OK;
}

/*
 * Encodes an MCS SendDataIndication PDU from server to client.
 */
int
rdp_mcs_encode_send_data_ind(
	struct rdp_buf *out,
	uint16_t user_id,
	uint16_t channel_id,
	const uint8_t *payload,
	size_t len)
{
	int error;

	/* Rejects user IDs below the MCS base 1001. */
	if (user_id < RDP_MCS_USER_BASE)
		return RDP_ERR_INVAL;

	/* Emits SendDataIndication header, priority=high, segmentation=begin|end. */
	error = rdp_buf_append_u8(out, (uint8_t)(MCS_CHOICE_SEND_DATA_IND << 2));
	if (error != RDP_OK)
		return error;

	error = rdp_buf_append_u16_be(out, (uint16_t)(user_id - RDP_MCS_USER_BASE));
	if (error != RDP_OK)
		return error;

	error = rdp_buf_append_u16_be(out, channel_id);
	if (error != RDP_OK)
		return error;

	error = rdp_buf_append_u8(out, 0x70U);
	if (error != RDP_OK)
		return error;

	error = per_append_len(out, len);
	if (error != RDP_OK)
		return error;

	error = rdp_buf_append(out, payload, len);
	if (error != RDP_OK)
		return error;

	/* Succeeded: SendDataIndication encoded. */
	return RDP_OK;
}

/*
 * Encodes an MCS DisconnectProviderUltimatum PDU.
 */
int
rdp_mcs_encode_disconnect(
	struct rdp_buf *out,
	uint8_t reason)
{
	uint8_t hdr;
	int error;

	/* Packs the disconnect choice and 3-bit reason into two bytes. */
	hdr = (uint8_t)((MCS_CHOICE_DISCONNECT << 2) | ((reason >> 1) & 0x03U));
	error = rdp_buf_append_u8(out, hdr);
	if (error != RDP_OK)
		return error;

	error = rdp_buf_append_u8(out, (uint8_t)((reason & 0x01U) << 7));
	if (error != RDP_OK)
		return error;

	/* Succeeded: DisconnectProviderUltimatum encoded. */
	return RDP_OK;
}

/*
 * Decodes a PER-encoded MCS DomainMCSPDU.
 */
int
rdp_mcs_decode_pdu(
	const uint8_t *data,
	size_t len,
	struct rdp_mcs_pdu *pdu)
{
	struct rdp_reader r;
	uint8_t b0;
	uint8_t res;
	uint16_t rel_id;
	uint16_t ch_id;
	uint8_t flags;
	size_t pay_len;
	int error;

	/* Clears output and initializes the cursor. */
	memset(pdu, 0, sizeof(*pdu));
	rdp_reader_init(&r, data, len);

	/* Reads the DomainMCSPDU choice byte. */
	error = rdp_read_u8(&r, &b0);
	if (error != RDP_OK)
		return error;

	pdu->choice = (uint8_t)((b0 >> 2) & 0x3FU);

	/* Dispatches according to the MCS PDU choice. */
	if (pdu->choice == MCS_CHOICE_ERECT_DOMAIN ||
	    pdu->choice == MCS_CHOICE_ATTACH_USER_REQ ||
	    pdu->choice == MCS_CHOICE_DISCONNECT) {
		return RDP_OK;
	} else if (pdu->choice == MCS_CHOICE_ATTACH_USER_CONF) {
		error = rdp_read_u8(&r, &res);
		if (error != RDP_OK)
			return error;

		pdu->result = res;
		if ((b0 & 0x02U) != 0U) {
			error = rdp_read_u16_be(&r, &rel_id);
			if (error != RDP_OK)
				return error;

			pdu->user_id = (uint16_t)(RDP_MCS_USER_BASE + rel_id);
		}

		return RDP_OK;
	} else if (pdu->choice == MCS_CHOICE_CHANNEL_JOIN_REQ) {
		error = rdp_read_u16_be(&r, &rel_id);
		if (error != RDP_OK)
			return error;

		error = rdp_read_u16_be(&r, &ch_id);
		if (error != RDP_OK)
			return error;

		pdu->user_id = (uint16_t)(RDP_MCS_USER_BASE + rel_id);
		pdu->channel_id = ch_id;
		return RDP_OK;
	} else if (pdu->choice == MCS_CHOICE_CHANNEL_JOIN_CONF) {
		error = rdp_read_u8(&r, &res);
		if (error != RDP_OK)
			return error;

		pdu->result = res;
		error = rdp_read_u16_be(&r, &rel_id);
		if (error != RDP_OK)
			return error;

		error = rdp_skip_bytes(&r, 2U);
		if (error != RDP_OK)
			return error;

		pdu->user_id = (uint16_t)(RDP_MCS_USER_BASE + rel_id);
		if ((b0 & 0x02U) != 0U) {
			error = rdp_read_u16_be(&r, &ch_id);
			if (error != RDP_OK)
				return error;

			pdu->channel_id = ch_id;
		}

		return RDP_OK;
	} else if (pdu->choice == MCS_CHOICE_SEND_DATA_REQ ||
		   pdu->choice == MCS_CHOICE_SEND_DATA_IND) {
		error = rdp_read_u16_be(&r, &rel_id);
		if (error != RDP_OK)
			return error;

		error = rdp_read_u16_be(&r, &ch_id);
		if (error != RDP_OK)
			return error;

		error = rdp_read_u8(&r, &flags);
		if (error != RDP_OK)
			return error;

		UNUSED_PARAMETER(flags);
		error = per_read_len(&r, &pay_len);
		if (error != RDP_OK)
			return error;

		if (rdp_reader_rem(&r) < pay_len)
			return RDP_ERR_PROTO;

		pdu->user_id = (uint16_t)(RDP_MCS_USER_BASE + rel_id);
		pdu->channel_id = ch_id;
		pdu->payload = r.data + r.pos;
		pdu->payload_len = pay_len;

		/* Succeeded: SendData PDU decoded. */
		return RDP_OK;
	}

	return RDP_ERR_PROTO;
}

/* Appends a BER definite length field. */
static int
ber_append_len(
	struct rdp_buf *buf,
	size_t len)
{
	int error;

	if (len < 0x80U) {
		error = rdp_buf_append_u8(buf, (uint8_t)len);
		return error;
	}

	if (len <= 0xFFU) {
		error = rdp_buf_append_u8(buf, 0x81U);
		if (error != RDP_OK)
			return error;

		return rdp_buf_append_u8(buf, (uint8_t)len);
	}

	if (len <= 0xFFFFU) {
		error = rdp_buf_append_u8(buf, 0x82U);
		if (error != RDP_OK)
			return error;

		return rdp_buf_append_u16_be(buf, (uint16_t)len);
	}

	return RDP_ERR_INVAL;
}

/* Appends a BER INTEGER TLV for an unsigned 32-bit value. */
static int
ber_append_int(
	struct rdp_buf *buf,
	uint32_t val)
{
	uint8_t raw[4];
	size_t n;
	int error;

	/* Encodes minimal big-endian representation keeping sign bit zero. */
	if (val <= 0x7FU) {
		raw[0] = (uint8_t)val;
		n = 1U;
	} else if (val <= 0x7FFFU) {
		raw[0] = (uint8_t)((val >> 8) & 0xFFU);
		raw[1] = (uint8_t)(val & 0xFFU);
		n = 2U;
	} else if (val <= 0x7FFFFFU) {
		raw[0] = (uint8_t)((val >> 16) & 0xFFU);
		raw[1] = (uint8_t)((val >> 8) & 0xFFU);
		raw[2] = (uint8_t)(val & 0xFFU);
		n = 3U;
	} else {
		raw[0] = (uint8_t)((val >> 24) & 0xFFU);
		raw[1] = (uint8_t)((val >> 16) & 0xFFU);
		raw[2] = (uint8_t)((val >> 8) & 0xFFU);
		raw[3] = (uint8_t)(val & 0xFFU);
		n = 4U;
	}

	error = rdp_buf_append_u8(buf, 0x02U);
	if (error != RDP_OK)
		return error;

	error = rdp_buf_append_u8(buf, (uint8_t)n);
	if (error != RDP_OK)
		return error;

	return rdp_buf_append(buf, raw, n);
}

/* Appends a BER OCTET STRING TLV. */
static int
ber_append_octet_string(
	struct rdp_buf *buf,
	const uint8_t *data,
	size_t len)
{
	int error;

	error = rdp_buf_append_u8(buf, 0x04U);
	if (error != RDP_OK)
		return error;

	error = ber_append_len(buf, len);
	if (error != RDP_OK)
		return error;

	return rdp_buf_append(buf, data, len);
}

/* Appends an MCS DomainParameters BER SEQUENCE. */
static int
ber_append_domain_params(
	struct rdp_buf *buf,
	uint32_t max_ch,
	uint32_t max_usr,
	uint32_t max_tok,
	uint32_t num_prio,
	uint32_t min_tp,
	uint32_t max_ht,
	uint32_t max_pdu,
	uint32_t proto_ver)
{
	struct rdp_buf seq;
	int error;

	rdp_buf_init(&seq);

	error = ber_append_int(&seq, max_ch);
	if (error != RDP_OK)
		goto fail;

	error = ber_append_int(&seq, max_usr);
	if (error != RDP_OK)
		goto fail;

	error = ber_append_int(&seq, max_tok);
	if (error != RDP_OK)
		goto fail;

	error = ber_append_int(&seq, num_prio);
	if (error != RDP_OK)
		goto fail;

	error = ber_append_int(&seq, min_tp);
	if (error != RDP_OK)
		goto fail;

	error = ber_append_int(&seq, max_ht);
	if (error != RDP_OK)
		goto fail;

	error = ber_append_int(&seq, max_pdu);
	if (error != RDP_OK)
		goto fail;

	error = ber_append_int(&seq, proto_ver);
	if (error != RDP_OK)
		goto fail;

	error = rdp_buf_append_u8(buf, 0x30U);
	if (error != RDP_OK)
		goto fail;

	error = ber_append_len(buf, seq.len);
	if (error != RDP_OK)
		goto fail;

	error = rdp_buf_append(buf, seq.data, seq.len);
	if (error != RDP_OK)
		goto fail;

	rdp_buf_free(&seq);
	return RDP_OK;

fail:
	rdp_buf_free(&seq);
	return error;
}

/* Reads a BER definite length field. */
static int
ber_read_len(
	struct rdp_reader *r,
	size_t *out_len)
{
	uint8_t b;
	uint8_t count;
	uint8_t i;
	size_t val;
	int error;

	error = rdp_read_u8(r, &b);
	if (error != RDP_OK)
		return error;

	if ((b & 0x80U) == 0U) {
		*out_len = (size_t)b;
		return RDP_OK;
	}

	count = (uint8_t)(b & 0x7FU);
	if (count == 0U || count > 4U)
		return RDP_ERR_PROTO;

	val = 0;
	for (i = 0; i < count; i++) {
		error = rdp_read_u8(r, &b);
		if (error != RDP_OK)
			return error;

		val = (val << 8) | (size_t)b;
	}

	*out_len = val;
	return RDP_OK;
}

/* Reads a BER tag-length-value element and verifies its expected tag. */
static int
ber_read_tlv(
	struct rdp_reader *r,
	uint8_t expected_tag,
	const uint8_t **val,
	size_t *val_len)
{
	uint8_t tag;
	size_t len;
	int error;

	error = rdp_read_u8(r, &tag);
	if (error != RDP_OK)
		return error;

	if (tag != expected_tag)
		return RDP_ERR_PROTO;

	error = ber_read_len(r, &len);
	if (error != RDP_OK)
		return error;

	error = rdp_read_bytes(r, val, len);
	if (error != RDP_OK)
		return error;

	*val_len = len;
	return RDP_OK;
}

/* Appends a PER length determinant (up to 16383 bytes). */
static int
per_append_len(
	struct rdp_buf *buf,
	size_t len)
{
	int error;

	if (len < 0x80U)
		return rdp_buf_append_u8(buf, (uint8_t)len);

	if (len < 0x4000U) {
		error = rdp_buf_append_u8(buf, (uint8_t)(0x80U | (len >> 8)));
		if (error != RDP_OK)
			return error;

		return rdp_buf_append_u8(buf, (uint8_t)(len & 0xFFU));
	}

	return RDP_ERR_INVAL;
}

/* Reads a PER length determinant. */
static int
per_read_len(
	struct rdp_reader *r,
	size_t *out_len)
{
	uint8_t b0;
	uint8_t b1;
	int error;

	error = rdp_read_u8(r, &b0);
	if (error != RDP_OK)
		return error;

	if ((b0 & 0x80U) == 0U) {
		*out_len = (size_t)b0;
		return RDP_OK;
	}

	if ((b0 & 0x40U) != 0U)
		return RDP_ERR_PROTO;

	error = rdp_read_u8(r, &b1);
	if (error != RDP_OK)
		return error;

	*out_len = (((size_t)(b0 & 0x3FU)) << 8) | (size_t)b1;
	return RDP_OK;
}

/* Appends the Client Core Data (TS_UD_CS_CORE) block. */
static int
append_cs_core(
	struct rdp_buf *buf,
	uint32_t width,
	uint32_t height,
	int want_drdynvc)
{
	uint16_t early_caps;
	uint8_t cname[32];
	int error;

	early_caps = RNS_UD_CS_SUPPORT_ERRINFO | RNS_UD_CS_WANT_32BPP;
	if (want_drdynvc != 0)
		early_caps |= RNS_UD_CS_SUPPORT_DYNVC_GFX;

	memset(cname, 0, sizeof(cname));
	cname[0] = 'z';
	cname[2] = 'e';
	cname[4] = 'd';
	cname[6] = 'B';
	cname[8] = 'S';
	cname[10] = 'D';

	error = rdp_buf_append_u16_le(buf, UDATA_CS_CORE);
	if (error != RDP_OK)
		return error;

	error = rdp_buf_append_u16_le(buf, 216U);
	if (error != RDP_OK)
		return error;

	error = rdp_buf_append_u32_le(buf, 0x00080004U);
	if (error != RDP_OK)
		return error;

	error = rdp_buf_append_u16_le(buf, (uint16_t)width);
	if (error != RDP_OK)
		return error;

	error = rdp_buf_append_u16_le(buf, (uint16_t)height);
	if (error != RDP_OK)
		return error;

	error = rdp_buf_append_u16_le(buf, 0xCA01U);
	if (error != RDP_OK)
		return error;

	error = rdp_buf_append_u16_le(buf, 0xAA03U);
	if (error != RDP_OK)
		return error;

	error = rdp_buf_append_u32_le(buf, 0x00000409U);
	if (error != RDP_OK)
		return error;

	error = rdp_buf_append_u32_le(buf, 2600U);
	if (error != RDP_OK)
		return error;

	error = rdp_buf_append(buf, cname, sizeof(cname));
	if (error != RDP_OK)
		return error;

	error = rdp_buf_append_u32_le(buf, 4U);
	if (error != RDP_OK)
		return error;

	error = rdp_buf_append_u32_le(buf, 0U);
	if (error != RDP_OK)
		return error;

	error = rdp_buf_append_u32_le(buf, 12U);
	if (error != RDP_OK)
		return error;

	error = rdp_buf_append_zeros(buf, 64U);
	if (error != RDP_OK)
		return error;

	error = rdp_buf_append_u16_le(buf, 0xCA01U);
	if (error != RDP_OK)
		return error;

	error = rdp_buf_append_u16_le(buf, 1U);
	if (error != RDP_OK)
		return error;

	error = rdp_buf_append_u32_le(buf, 0U);
	if (error != RDP_OK)
		return error;

	error = rdp_buf_append_u16_le(buf, 24U);
	if (error != RDP_OK)
		return error;

	error = rdp_buf_append_u16_le(buf, 0x000FU);
	if (error != RDP_OK)
		return error;

	error = rdp_buf_append_u16_le(buf, early_caps);
	if (error != RDP_OK)
		return error;

	error = rdp_buf_append_zeros(buf, 64U);
	if (error != RDP_OK)
		return error;

	error = rdp_buf_append_u8(buf, 0U);
	if (error != RDP_OK)
		return error;

	error = rdp_buf_append_u8(buf, 0U);
	if (error != RDP_OK)
		return error;

	return rdp_buf_append_u32_le(buf, RDP_NEG_PROTO_SSL);
}

/* Appends the Client Security Data (TS_UD_CS_SEC) block. */
static int
append_cs_security(
	struct rdp_buf *buf)
{
	int error;

	error = rdp_buf_append_u16_le(buf, UDATA_CS_SECURITY);
	if (error != RDP_OK)
		return error;

	error = rdp_buf_append_u16_le(buf, 12U);
	if (error != RDP_OK)
		return error;

	error = rdp_buf_append_u32_le(buf, 0U);
	if (error != RDP_OK)
		return error;

	return rdp_buf_append_u32_le(buf, 0U);
}

/* Appends the Client Network Data (TS_UD_CS_NET) block requesting drdynvc. */
static int
append_cs_net(
	struct rdp_buf *buf,
	int want_drdynvc)
{
	static const uint8_t drdynvc_name[8] = {
		'd', 'r', 'd', 'y', 'n', 'v', 'c', '\0'
	};
	int error;

	if (want_drdynvc == 0)
		return RDP_OK;

	error = rdp_buf_append_u16_le(buf, UDATA_CS_NET);
	if (error != RDP_OK)
		return error;

	error = rdp_buf_append_u16_le(buf, 20U);
	if (error != RDP_OK)
		return error;

	error = rdp_buf_append_u32_le(buf, 1U);
	if (error != RDP_OK)
		return error;

	error = rdp_buf_append(buf, drdynvc_name, sizeof(drdynvc_name));
	if (error != RDP_OK)
		return error;

	return rdp_buf_append_u32_le(buf, 0xC0800000U);
}

/* Parses GCC User Data blocks for width, height, and drdynvc channel ID. */
static int
parse_user_data_blocks(
	const uint8_t *data,
	size_t len,
	struct rdp_mcs_connect_info *info)
{
	struct rdp_reader r;
	uint16_t btype;
	uint16_t blen;
	const uint8_t *blk;
	struct rdp_reader br;
	uint16_t w16;
	uint16_t h16;
	uint32_t ch_count;
	uint32_t i;
	const uint8_t *cname;
	uint16_t io_ch;
	uint16_t sc_count;
	uint16_t ch_id;
	int error;

	rdp_reader_init(&r, data, len);

	/* Iterates over each type-length-value User Data block. */
	while (rdp_reader_rem(&r) >= 4U) {
		error = rdp_read_u16_le(&r, &btype);
		if (error != RDP_OK)
			return error;

		error = rdp_read_u16_le(&r, &blen);
		if (error != RDP_OK)
			return error;

		if (blen < 4U || rdp_reader_rem(&r) < (size_t)(blen - 4U))
			return RDP_ERR_PROTO;

		error = rdp_read_bytes(&r, &blk, (size_t)(blen - 4U));
		if (error != RDP_OK)
			return error;

		rdp_reader_init(&br, blk, (size_t)(blen - 4U));

		if (btype == UDATA_CS_CORE && blen >= 12U) {
			error = rdp_skip_bytes(&br, 4U);
			if (error != RDP_OK)
				return error;

			error = rdp_read_u16_le(&br, &w16);
			if (error != RDP_OK)
				return error;

			error = rdp_read_u16_le(&br, &h16);
			if (error != RDP_OK)
				return error;

			info->width = w16;
			info->height = h16;
		} else if (btype == UDATA_CS_NET && blen >= 8U) {
			error = rdp_read_u32_le(&br, &ch_count);
			if (error != RDP_OK)
				return error;

			for (i = 0; i < ch_count && rdp_reader_rem(&br) >= 12U; i++) {
				error = rdp_read_bytes(&br, &cname, 8U);
				if (error != RDP_OK)
					return error;

				error = rdp_skip_bytes(&br, 4U);
				if (error != RDP_OK)
					return error;

				if (memcmp(cname, "drdynvc", 7U) == 0)
					info->has_drdynvc = 1;
			}
		} else if (btype == UDATA_SC_NET && blen >= 8U) {
			error = rdp_read_u16_le(&br, &io_ch);
			if (error != RDP_OK)
				return error;

			error = rdp_read_u16_le(&br, &sc_count);
			if (error != RDP_OK)
				return error;

			info->io_channel = io_ch;
			if (sc_count > 0U && rdp_reader_rem(&br) >= 2U) {
				error = rdp_read_u16_le(&br, &ch_id);
				if (error != RDP_OK)
					return error;

				info->drdynvc_channel = ch_id;
				info->has_drdynvc = 1;
			}
		}
	}

	return RDP_OK;
}
