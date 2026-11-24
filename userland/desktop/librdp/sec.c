/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Security info, licensing alert, capability sets, and Share Control/Data
 * PDU codecs (MS-RDPBCGR).
 */

#include "rdp-internal.h"

#include <string.h>

/* TS_INFO_PACKET flags. */
#define INFO_MOUSE			0x00000001U
#define INFO_UNICODE			0x00000010U
#define INFO_LOGONNOTIFY		0x00000040U
#define INFO_AUTOLOGON			0x00000008U
#define INFO_ENABLEWINDOWSKEY		0x00000100U

/* Capability set type identifiers. */
#define CAPSTYPE_GENERAL		0x0001U
#define CAPSTYPE_BITMAP			0x0002U
#define CAPSTYPE_ORDER			0x0003U
#define CAPSTYPE_POINTER		0x0008U
#define CAPSTYPE_SHARE			0x0009U
#define CAPSTYPE_INPUT			0x000DU
#define CAPSTYPE_FONT			0x000EU
#define CAPSTYPE_VIRTUALCHANNEL		0x0014U
#define CAPSTYPE_SURFACE_COMMANDS	0x001CU
#define CAPSTYPE_MULTIFRAGMENTUPDATE	0x001AU
#define CAPSTYPE_LARGE_POINTER		0x001BU
#define CAPSTYPE_FRAME_ACKNOWLEDGE	0x001EU

static int append_utf16le_z(struct rdp_buf *buf, const char *utf8, size_t *out_cb);
static int read_utf16le_z(struct rdp_reader *r, size_t cb, char *out, size_t out_sz);
static int append_capset_general(struct rdp_buf *buf);
static int append_capset_bitmap(struct rdp_buf *buf, uint32_t width, uint32_t height);
static int append_capset_order(struct rdp_buf *buf);
static int append_capset_pointer(struct rdp_buf *buf);
static int append_capset_share(struct rdp_buf *buf, uint16_t channel_id);
static int append_capset_input(struct rdp_buf *buf);
static int append_capset_vc(struct rdp_buf *buf);
static int append_capset_surfcmds(struct rdp_buf *buf);
static int append_capset_multifrag(struct rdp_buf *buf, uint32_t max_size);
static int append_capset_frame_ack(struct rdp_buf *buf);
static int encode_share_data_hdr(struct rdp_buf *buf, uint32_t share_id, uint16_t src_ch, uint8_t pdu_type2, size_t body_len);

/*
 * Encodes a TS_INFO_PACKET with security header for TLS sessions.
 */
int
rdp_sec_encode_info_pkt(
	struct rdp_buf *out,
	const struct rdp_client_config *cfg)
{
	struct rdp_buf body;
	struct rdp_buf strs;
	uint32_t flags;
	size_t cb_dom;
	size_t cb_user;
	size_t cb_pass;
	size_t cb_shell;
	size_t cb_wdir;
	int error;

	/* Initializes buffers for the TS_INFO_PACKET body and UTF-16 strings. */
	rdp_buf_init(&body);
	rdp_buf_init(&strs);

	/* Builds the UTF-16LE credential strings and records byte lengths. */
	error = append_utf16le_z(&strs, cfg->domain, &cb_dom);
	if (error != RDP_OK)
		goto fail;
	error = append_utf16le_z(&strs, cfg->user_name, &cb_user);
	if (error != RDP_OK)
		goto fail;
	error = append_utf16le_z(&strs, cfg->password, &cb_pass);
	if (error != RDP_OK)
		goto fail;
	error = append_utf16le_z(&strs, "", &cb_shell);
	if (error != RDP_OK)
		goto fail;
	error = append_utf16le_z(&strs, "", &cb_wdir);
	if (error != RDP_OK)
		goto fail;

	/* Sets INFO_AUTOLOGON when a non-empty password is supplied. */
	flags = INFO_MOUSE | INFO_UNICODE | INFO_LOGONNOTIFY |
		INFO_ENABLEWINDOWSKEY;
	if (cb_pass > 0U)
		flags |= INFO_AUTOLOGON;

	/* Appends the 4-byte TS_SECURITY_HEADER (SEC_INFO_PKT). */
	error = rdp_buf_append_u16_le(&body, RDP_SEC_INFO_PKT);
	if (error != RDP_OK)
		goto fail;
	error = rdp_buf_append_u16_le(&body, 0U);
	if (error != RDP_OK)
		goto fail;

	/* Appends TS_INFO_PACKET fixed fields and string lengths. */
	error = rdp_buf_append_u32_le(&body, 0U);
	if (error != RDP_OK)
		goto fail;
	error = rdp_buf_append_u32_le(&body, flags);
	if (error != RDP_OK)
		goto fail;
	error = rdp_buf_append_u16_le(&body, (uint16_t)cb_dom);
	if (error != RDP_OK)
		goto fail;
	error = rdp_buf_append_u16_le(&body, (uint16_t)cb_user);
	if (error != RDP_OK)
		goto fail;
	error = rdp_buf_append_u16_le(&body, (uint16_t)cb_pass);
	if (error != RDP_OK)
		goto fail;
	error = rdp_buf_append_u16_le(&body, (uint16_t)cb_shell);
	if (error != RDP_OK)
		goto fail;
	error = rdp_buf_append_u16_le(&body, (uint16_t)cb_wdir);
	if (error != RDP_OK)
		goto fail;
	error = rdp_buf_append(&body, strs.data, strs.len);
	if (error != RDP_OK)
		goto fail;

	/* Appends minimal TS_EXTENDED_INFO_PACKET fields. */
	error = rdp_buf_append_u16_le(&body, 0x0002U);
	if (error != RDP_OK)
		goto fail;
	error = rdp_buf_append_u16_le(&body, 2U);
	if (error != RDP_OK)
		goto fail;
	error = rdp_buf_append_u16_le(&body, 0U);
	if (error != RDP_OK)
		goto fail;
	error = rdp_buf_append_u16_le(&body, 2U);
	if (error != RDP_OK)
		goto fail;
	error = rdp_buf_append_u16_le(&body, 0U);
	if (error != RDP_OK)
		goto fail;
	error = rdp_buf_append_zeros(&body, 172U);
	if (error != RDP_OK)
		goto fail;
	error = rdp_buf_append_u32_le(&body, 0U);
	if (error != RDP_OK)
		goto fail;
	error = rdp_buf_append_u32_le(&body, 0U);
	if (error != RDP_OK)
		goto fail;
	error = rdp_buf_append(out, body.data, body.len);
	if (error != RDP_OK)
		goto fail;
	rdp_buf_free(&body);
	rdp_buf_free(&strs);

	/* Succeeded: TS_INFO_PACKET encoded. */
	return RDP_OK;

fail:
	rdp_buf_free(&body);
	rdp_buf_free(&strs);
	return error;
}

/*
 * Decodes a TS_INFO_PACKET (with TS_SECURITY_HEADER) into UTF-8 strings.
 */
int
rdp_sec_decode_info_pkt(
	const uint8_t *data,
	size_t len,
	struct rdp_sec_info *info)
{
	struct rdp_reader r;
	uint16_t sec_flags;
	uint16_t sec_hi;
	uint32_t codepage;
	uint16_t cb_dom;
	uint16_t cb_user;
	uint16_t cb_pass;
	uint16_t cb_shell;
	uint16_t cb_wdir;
	int error;

	/* Clears output and initializes the reader. */
	memset(info, 0, sizeof(*info));
	rdp_reader_init(&r, data, len);

	/* Verifies the SEC_INFO_PKT security header flag. */
	error = rdp_read_u16_le(&r, &sec_flags);
	if (error != RDP_OK)
		return error;
	error = rdp_read_u16_le(&r, &sec_hi);
	if (error != RDP_OK)
		return error;

	UNUSED_PARAMETER(sec_hi);
	if ((sec_flags & RDP_SEC_INFO_PKT) == 0U)
		return RDP_ERR_PROTO;

	/* Reads CodePage, flags, and the five UTF-16LE byte lengths. */
	error = rdp_read_u32_le(&r, &codepage);
	if (error != RDP_OK)
		return error;

	UNUSED_PARAMETER(codepage);
	error = rdp_read_u32_le(&r, &info->flags);
	if (error != RDP_OK)
		return error;
	error = rdp_read_u16_le(&r, &cb_dom);
	if (error != RDP_OK)
		return error;
	error = rdp_read_u16_le(&r, &cb_user);
	if (error != RDP_OK)
		return error;
	error = rdp_read_u16_le(&r, &cb_pass);
	if (error != RDP_OK)
		return error;
	error = rdp_read_u16_le(&r, &cb_shell);
	if (error != RDP_OK)
		return error;
	error = rdp_read_u16_le(&r, &cb_wdir);
	if (error != RDP_OK)
		return error;

	/* Decodes domain, user name, and password strings. */
	error = read_utf16le_z(&r, cb_dom, info->domain, sizeof(info->domain));
	if (error != RDP_OK)
		return error;
	error = read_utf16le_z(&r, cb_user, info->user_name,
			       sizeof(info->user_name));
	if (error != RDP_OK)
		return error;
	error = read_utf16le_z(&r, cb_pass, info->password,
			       sizeof(info->password));
	if (error != RDP_OK)
		return error;

	UNUSED_PARAMETER(cb_shell);
	UNUSED_PARAMETER(cb_wdir);

	/* Succeeded: TS_INFO_PACKET decoded. */
	return RDP_OK;
}

/*
 * Encodes a LICENSE_ERROR_ALERT (STATUS_VALID_CLIENT) PDU.
 */
int
rdp_sec_encode_license_valid(
	struct rdp_buf *out)
{
	int error;

	/* Appends TS_SECURITY_HEADER with SEC_LICENSE_PKT. */
	error = rdp_buf_append_u16_le(out, RDP_SEC_LICENSE_PKT);
	if (error != RDP_OK)
		return error;
	error = rdp_buf_append_u16_le(out, 0U);
	if (error != RDP_OK)
		return error;

	/* Appends LICENSE_PREAMBLE (ERROR_ALERT = 0xFF, version 2.0, size 16). */
	error = rdp_buf_append_u8(out, 0xFFU);
	if (error != RDP_OK)
		return error;
	error = rdp_buf_append_u8(out, 0x02U);
	if (error != RDP_OK)
		return error;
	error = rdp_buf_append_u16_le(out, 16U);
	if (error != RDP_OK)
		return error;

	/* Appends STATUS_VALID_CLIENT (7), ST_NO_TRANSITION (2), empty blob. */
	error = rdp_buf_append_u32_le(out, 7U);
	if (error != RDP_OK)
		return error;
	error = rdp_buf_append_u32_le(out, 2U);
	if (error != RDP_OK)
		return error;
	error = rdp_buf_append_u16_le(out, 0x0004U);
	if (error != RDP_OK)
		return error;
	return rdp_buf_append_u16_le(out, 0U);
}

/*
 * Validates a LICENSE_ERROR_ALERT PDU from the server.
 */
int
rdp_sec_decode_license_valid(
	const uint8_t *data,
	size_t len)
{
	struct rdp_reader r;
	uint16_t sec_flags;
	uint16_t sec_hi;
	uint8_t msg_type;
	uint8_t preamble_flags;
	uint16_t msg_size;
	uint32_t err_code;
	int error;

	/* Initializes the reader and checks SEC_LICENSE_PKT. */
	rdp_reader_init(&r, data, len);
	error = rdp_read_u16_le(&r, &sec_flags);
	if (error != RDP_OK)
		return error;
	error = rdp_read_u16_le(&r, &sec_hi);
	if (error != RDP_OK)
		return error;

	UNUSED_PARAMETER(sec_hi);
	if ((sec_flags & RDP_SEC_LICENSE_PKT) == 0U)
		return RDP_ERR_PROTO;

	/* Reads LICENSE_PREAMBLE and verifies ERROR_ALERT + STATUS_VALID_CLIENT. */
	error = rdp_read_u8(&r, &msg_type);
	if (error != RDP_OK)
		return error;
	error = rdp_read_u8(&r, &preamble_flags);
	if (error != RDP_OK)
		return error;
	error = rdp_read_u16_le(&r, &msg_size);
	if (error != RDP_OK)
		return error;

	UNUSED_PARAMETER(preamble_flags);
	UNUSED_PARAMETER(msg_size);
	if (msg_type != 0xFFU)
		return RDP_ERR_PROTO;
	error = rdp_read_u32_le(&r, &err_code);
	if (error != RDP_OK)
		return error;
	if (err_code != 7U)
		return RDP_ERR_PROTO;

	/* Succeeded: server granted valid client license status. */
	return RDP_OK;
}

/*
 * Encodes a DemandActivePDU carrying server capability sets.
 */
int
rdp_sec_encode_demand_active(
	struct rdp_buf *out,
	uint32_t share_id,
	uint32_t width,
	uint32_t height,
	uint16_t server_channel)
{
	struct rdp_buf caps;
	size_t total_len;
	int error;

	/* Encodes the capability sets into a temporary buffer. */
	rdp_buf_init(&caps);
	error = append_capset_general(&caps);
	if (error != RDP_OK)
		goto fail;
	error = append_capset_bitmap(&caps, width, height);
	if (error != RDP_OK)
		goto fail;
	error = append_capset_order(&caps);
	if (error != RDP_OK)
		goto fail;
	error = append_capset_pointer(&caps);
	if (error != RDP_OK)
		goto fail;
	error = append_capset_share(&caps, server_channel);
	if (error != RDP_OK)
		goto fail;
	error = append_capset_input(&caps);
	if (error != RDP_OK)
		goto fail;
	error = append_capset_vc(&caps);
	if (error != RDP_OK)
		goto fail;
	error = append_capset_surfcmds(&caps);
	if (error != RDP_OK)
		goto fail;
	error = append_capset_multifrag(&caps, RDP_MAX_PDU_SIZE);
	if (error != RDP_OK)
		goto fail;
	error = append_capset_frame_ack(&caps);
	if (error != RDP_OK)
		goto fail;

	/* Writes the Share Control Header and DemandActivePDU fields. */
	total_len = 6U + 4U + 2U + 2U + 4U + 4U + caps.len + 4U;
	error = rdp_buf_append_u16_le(out, (uint16_t)total_len);
	if (error != RDP_OK)
		goto fail;
	error = rdp_buf_append_u16_le(out, RDP_PDUTYPE_DEMANDACTIVE);
	if (error != RDP_OK)
		goto fail;
	error = rdp_buf_append_u16_le(out, server_channel);
	if (error != RDP_OK)
		goto fail;
	error = rdp_buf_append_u32_le(out, share_id);
	if (error != RDP_OK)
		goto fail;
	error = rdp_buf_append_u16_le(out, 4U);
	if (error != RDP_OK)
		goto fail;
	error = rdp_buf_append_u16_le(out, (uint16_t)(4U + caps.len));
	if (error != RDP_OK)
		goto fail;
	error = rdp_buf_append(out, "RDP", 4U);
	if (error != RDP_OK)
		goto fail;
	error = rdp_buf_append_u16_le(out, 10U);
	if (error != RDP_OK)
		goto fail;
	error = rdp_buf_append_u16_le(out, 0U);
	if (error != RDP_OK)
		goto fail;
	error = rdp_buf_append(out, caps.data, caps.len);
	if (error != RDP_OK)
		goto fail;
	error = rdp_buf_append_u32_le(out, 0U);
	if (error != RDP_OK)
		goto fail;
	rdp_buf_free(&caps);

	/* Succeeded: DemandActivePDU encoded. */
	return RDP_OK;

fail:
	rdp_buf_free(&caps);
	return error;
}

/*
 * Encodes a ConfirmActivePDU carrying client capability sets.
 */
int
rdp_sec_encode_confirm_active(
	struct rdp_buf *out,
	uint32_t share_id,
	uint32_t width,
	uint32_t height,
	uint16_t user_channel)
{
	struct rdp_buf caps;
	size_t total_len;
	int error;

	/* Encodes the client capability sets into a temporary buffer. */
	rdp_buf_init(&caps);
	error = append_capset_general(&caps);
	if (error != RDP_OK)
		goto fail;
	error = append_capset_bitmap(&caps, width, height);
	if (error != RDP_OK)
		goto fail;
	error = append_capset_order(&caps);
	if (error != RDP_OK)
		goto fail;
	error = append_capset_pointer(&caps);
	if (error != RDP_OK)
		goto fail;
	error = append_capset_share(&caps, user_channel);
	if (error != RDP_OK)
		goto fail;
	error = append_capset_input(&caps);
	if (error != RDP_OK)
		goto fail;
	error = append_capset_vc(&caps);
	if (error != RDP_OK)
		goto fail;
	error = append_capset_surfcmds(&caps);
	if (error != RDP_OK)
		goto fail;
	error = append_capset_multifrag(&caps, RDP_MAX_PDU_SIZE);
	if (error != RDP_OK)
		goto fail;
	error = append_capset_frame_ack(&caps);
	if (error != RDP_OK)
		goto fail;

	/* Writes the Share Control Header and ConfirmActivePDU fields. */
	total_len = 6U + 4U + 2U + 2U + 4U + 4U + caps.len;
	error = rdp_buf_append_u16_le(out, (uint16_t)total_len);
	if (error != RDP_OK)
		goto fail;
	error = rdp_buf_append_u16_le(out, RDP_PDUTYPE_CONFIRMACTIVE);
	if (error != RDP_OK)
		goto fail;
	error = rdp_buf_append_u16_le(out, user_channel);
	if (error != RDP_OK)
		goto fail;
	error = rdp_buf_append_u32_le(out, share_id);
	if (error != RDP_OK)
		goto fail;
	error = rdp_buf_append_u16_le(out, 0x03EAU);
	if (error != RDP_OK)
		goto fail;
	error = rdp_buf_append_u16_le(out, 4U);
	if (error != RDP_OK)
		goto fail;
	error = rdp_buf_append_u16_le(out, (uint16_t)(4U + caps.len));
	if (error != RDP_OK)
		goto fail;
	error = rdp_buf_append(out, "RDP", 4U);
	if (error != RDP_OK)
		goto fail;
	error = rdp_buf_append_u16_le(out, 10U);
	if (error != RDP_OK)
		goto fail;
	error = rdp_buf_append_u16_le(out, 0U);
	if (error != RDP_OK)
		goto fail;
	error = rdp_buf_append(out, caps.data, caps.len);
	if (error != RDP_OK)
		goto fail;
	rdp_buf_free(&caps);

	/* Succeeded: ConfirmActivePDU encoded. */
	return RDP_OK;

fail:
	rdp_buf_free(&caps);
	return error;
}

/*
 * Decodes a DemandActivePDU or ConfirmActivePDU and extracts capabilities.
 */
int
rdp_sec_decode_active_pdu(
	const uint8_t *data,
	size_t len,
	uint16_t expected_type,
	struct rdp_active_pdu *out)
{
	struct rdp_reader r;
	uint16_t total_len;
	uint16_t pdu_type;
	uint16_t pdu_src;
	uint16_t src_desc_len;
	uint16_t comb_caps_len;
	uint16_t num_caps;
	uint16_t pad;
	uint16_t i;
	uint16_t cap_type;
	uint16_t cap_len;
	const uint8_t *cap_body;
	struct rdp_reader cr;
	uint16_t w16;
	uint16_t h16;
	int error;

	/* Clears output and initializes the cursor. */
	memset(out, 0, sizeof(*out));
	rdp_reader_init(&r, data, len);

	/* Reads and validates the Share Control Header. */
	error = rdp_read_u16_le(&r, &total_len);
	if (error != RDP_OK)
		return error;
	error = rdp_read_u16_le(&r, &pdu_type);
	if (error != RDP_OK)
		return error;
	error = rdp_read_u16_le(&r, &pdu_src);
	if (error != RDP_OK)
		return error;

	UNUSED_PARAMETER(total_len);
	UNUSED_PARAMETER(pdu_src);
	if (pdu_type != expected_type)
		return RDP_ERR_PROTO;

	/* Reads shareId and skips originatorId on ConfirmActivePDU. */
	error = rdp_read_u32_le(&r, &out->share_id);
	if (error != RDP_OK)
		return error;
	if (expected_type == RDP_PDUTYPE_CONFIRMACTIVE) {
		error = rdp_skip_bytes(&r, 2U);
		if (error != RDP_OK)
			return error;
	}

	/* Reads sourceDescriptor and combinedCapabilities lengths. */
	error = rdp_read_u16_le(&r, &src_desc_len);
	if (error != RDP_OK)
		return error;
	error = rdp_read_u16_le(&r, &comb_caps_len);
	if (error != RDP_OK)
		return error;

	UNUSED_PARAMETER(comb_caps_len);
	error = rdp_skip_bytes(&r, src_desc_len);
	if (error != RDP_OK)
		return error;
	error = rdp_read_u16_le(&r, &num_caps);
	if (error != RDP_OK)
		return error;
	error = rdp_read_u16_le(&r, &pad);
	if (error != RDP_OK)
		return error;

	UNUSED_PARAMETER(pad);

	/* Scans capability sets for Bitmap dimensions and MultifragmentUpdate. */
	for (i = 0; i < num_caps; i++) {
		error = rdp_read_u16_le(&r, &cap_type);
		if (error != RDP_OK)
			return error;
		error = rdp_read_u16_le(&r, &cap_len);
		if (error != RDP_OK)
			return error;
		if (cap_len < 4U)
			return RDP_ERR_PROTO;
		error = rdp_read_bytes(&r, &cap_body, (size_t)(cap_len - 4U));
		if (error != RDP_OK)
			return error;
		rdp_reader_init(&cr, cap_body, (size_t)(cap_len - 4U));
		if (cap_type == CAPSTYPE_BITMAP && cap_len >= 16U) {
			error = rdp_skip_bytes(&cr, 8U);
			if (error != RDP_OK)
				return error;
			error = rdp_read_u16_le(&cr, &w16);
			if (error != RDP_OK)
				return error;
			error = rdp_read_u16_le(&cr, &h16);
			if (error != RDP_OK)
				return error;
			out->width = w16;
			out->height = h16;
		} else if (cap_type == CAPSTYPE_MULTIFRAGMENTUPDATE && cap_len >= 8U) {
			error = rdp_read_u32_le(&cr, &out->max_multifrag_size);
			if (error != RDP_OK)
				return error;
		}
	}

	/* Succeeded: active PDU decoded. */
	return RDP_OK;
}

/*
 * Encodes a Share Data SynchronizePDU.
 */
int
rdp_sec_encode_share_data_sync(
	struct rdp_buf *out,
	uint32_t share_id,
	uint16_t source_channel,
	uint16_t target_user)
{
	int error;

	/* Writes the Share Data Header and 4-byte Synchronize body. */
	error = encode_share_data_hdr(out, share_id, source_channel,
				      RDP_PDUTYPE2_SYNCHRONIZE, 4U);
	if (error != RDP_OK)
		return error;
	error = rdp_buf_append_u16_le(out, 1U);
	if (error != RDP_OK)
		return error;
	return rdp_buf_append_u16_le(out, target_user);
}

/*
 * Encodes a Share Data ControlPDU.
 */
int
rdp_sec_encode_share_data_control(
	struct rdp_buf *out,
	uint32_t share_id,
	uint16_t source_channel,
	uint16_t action,
	uint16_t grant_id,
	uint32_t control_id)
{
	int error;

	/* Writes the Share Data Header and 8-byte Control body. */
	error = encode_share_data_hdr(out, share_id, source_channel,
				      RDP_PDUTYPE2_CONTROL, 8U);
	if (error != RDP_OK)
		return error;
	error = rdp_buf_append_u16_le(out, action);
	if (error != RDP_OK)
		return error;
	error = rdp_buf_append_u16_le(out, grant_id);
	if (error != RDP_OK)
		return error;
	return rdp_buf_append_u32_le(out, control_id);
}

/*
 * Encodes a Share Data FontListPDU.
 */
int
rdp_sec_encode_share_data_fontlist(
	struct rdp_buf *out,
	uint32_t share_id,
	uint16_t source_channel)
{
	int error;

	/* Writes the Share Data Header and empty FontList body. */
	error = encode_share_data_hdr(out, share_id, source_channel,
				      RDP_PDUTYPE2_FONTLIST, 8U);
	if (error != RDP_OK)
		return error;
	error = rdp_buf_append_u16_le(out, 0U);
	if (error != RDP_OK)
		return error;
	error = rdp_buf_append_u16_le(out, 0U);
	if (error != RDP_OK)
		return error;
	error = rdp_buf_append_u16_le(out, 3U);
	if (error != RDP_OK)
		return error;
	return rdp_buf_append_u16_le(out, 50U);
}

/*
 * Encodes a Share Data FontMapPDU.
 */
int
rdp_sec_encode_share_data_fontmap(
	struct rdp_buf *out,
	uint32_t share_id,
	uint16_t source_channel)
{
	int error;

	/* Writes the Share Data Header and empty FontMap body. */
	error = encode_share_data_hdr(out, share_id, source_channel,
				      RDP_PDUTYPE2_FONTMAP, 8U);
	if (error != RDP_OK)
		return error;
	error = rdp_buf_append_u16_le(out, 0U);
	if (error != RDP_OK)
		return error;
	error = rdp_buf_append_u16_le(out, 0U);
	if (error != RDP_OK)
		return error;
	error = rdp_buf_append_u16_le(out, 3U);
	if (error != RDP_OK)
		return error;
	return rdp_buf_append_u16_le(out, 4U);
}

/*
 * Encodes a Share Data SetErrorInfoPDU.
 */
int
rdp_sec_encode_share_data_error_info(
	struct rdp_buf *out,
	uint32_t share_id,
	uint16_t source_channel,
	uint32_t error_info)
{
	int error;

	/* Writes the Share Data Header and 4-byte errorInfo code. */
	error = encode_share_data_hdr(out, share_id, source_channel,
				      RDP_PDUTYPE2_SET_ERROR_INFO, 4U);
	if (error != RDP_OK)
		return error;
	return rdp_buf_append_u32_le(out, error_info);
}

/*
 * Decodes a Share Control Header (and Share Data Header when pduType is Data).
 */
int
rdp_sec_decode_share_hdr(
	const uint8_t *data,
	size_t len,
	uint16_t *pdu_type,
	uint8_t *pdu_type2,
	uint32_t *share_id,
	const uint8_t **body,
	size_t *body_len)
{
	struct rdp_reader r;
	uint16_t total_len;
	uint16_t src;
	uint8_t pad1;
	uint8_t stream_id;
	uint16_t uncomp_len;
	uint8_t comp_type;
	uint16_t comp_len;
	int error;

	/* Initializes outputs and cursor. */
	*pdu_type = 0U;
	*pdu_type2 = 0U;
	*share_id = 0U;
	*body = NULL;
	*body_len = 0U;
	rdp_reader_init(&r, data, len);

	/* Reads the 6-byte Share Control Header. */
	error = rdp_read_u16_le(&r, &total_len);
	if (error != RDP_OK)
		return error;
	error = rdp_read_u16_le(&r, pdu_type);
	if (error != RDP_OK)
		return error;
	error = rdp_read_u16_le(&r, &src);
	if (error != RDP_OK)
		return error;

	UNUSED_PARAMETER(total_len);
	UNUSED_PARAMETER(src);

	/* Returns immediately for non-Data PDUs such as DemandActive. */
	if (*pdu_type != RDP_PDUTYPE_DATAPDU) {
		*body = r.data + r.pos;
		*body_len = rdp_reader_rem(&r);
		return RDP_OK;
	}

	/* Reads the 12-byte Share Data Header tail. */
	error = rdp_read_u32_le(&r, share_id);
	if (error != RDP_OK)
		return error;
	error = rdp_read_u8(&r, &pad1);
	if (error != RDP_OK)
		return error;
	error = rdp_read_u8(&r, &stream_id);
	if (error != RDP_OK)
		return error;
	error = rdp_read_u16_le(&r, &uncomp_len);
	if (error != RDP_OK)
		return error;
	error = rdp_read_u8(&r, pdu_type2);
	if (error != RDP_OK)
		return error;
	error = rdp_read_u8(&r, &comp_type);
	if (error != RDP_OK)
		return error;
	error = rdp_read_u16_le(&r, &comp_len);
	if (error != RDP_OK)
		return error;

	UNUSED_PARAMETER(pad1);
	UNUSED_PARAMETER(stream_id);
	UNUSED_PARAMETER(uncomp_len);
	UNUSED_PARAMETER(comp_type);
	UNUSED_PARAMETER(comp_len);
	*body = r.data + r.pos;
	*body_len = rdp_reader_rem(&r);

	/* Succeeded: Share Data Header decoded. */
	return RDP_OK;
}

/* Appends a null-terminated UTF-16LE string and returns byte length sans null. */
static int
append_utf16le_z(
	struct rdp_buf *buf,
	const char *utf8,
	size_t *out_cb)
{
	size_t i;
	size_t slen;
	int error;
	if (utf8 == NULL || utf8[0] == '\0') {
		*out_cb = 0U;
		return rdp_buf_append_u16_le(buf, 0U);
	}
	slen = strlen(utf8);
	for (i = 0; i < slen; i++) {
		error = rdp_buf_append_u16_le(buf, (uint8_t)utf8[i]);
		if (error != RDP_OK)
			return error;
	}
	*out_cb = slen * 2U;
	return rdp_buf_append_u16_le(buf, 0U);
}

/* Reads a null-terminated UTF-16LE string of cb bytes plus 2-byte NUL. */
static int
read_utf16le_z(
	struct rdp_reader *r,
	size_t cb,
	char *out,
	size_t out_sz)
{
	size_t nchars;
	size_t i;
	size_t pos;
	uint16_t ch;
	int error;
	if ((cb & 1U) != 0U)
		return RDP_ERR_PROTO;
	nchars = cb / 2U;
	pos = 0U;
	for (i = 0; i < nchars; i++) {
		error = rdp_read_u16_le(r, &ch);
		if (error != RDP_OK)
			return error;
		if (pos + 1U < out_sz)
			out[pos++] = (ch < 128U) ? (char)ch : '?';
	}
	out[pos] = '\0';

	/* Consumes the mandatory 2-byte UTF-16LE NUL terminator. */
	return rdp_read_u16_le(r, &ch);
}

/* Appends the General Capability Set (24 bytes). */
static int
append_capset_general(
	struct rdp_buf *buf)
{
	int error;
	error = rdp_buf_append_u16_le(buf, CAPSTYPE_GENERAL);
	if (error != RDP_OK)
		return error;
	error = rdp_buf_append_u16_le(buf, 24U);
	if (error != RDP_OK)
		return error;
	error = rdp_buf_append_u16_le(buf, 1U);
	if (error != RDP_OK)
		return error;
	error = rdp_buf_append_u16_le(buf, 3U);
	if (error != RDP_OK)
		return error;
	error = rdp_buf_append_u16_le(buf, 0x0200U);
	if (error != RDP_OK)
		return error;
	error = rdp_buf_append_zeros(buf, 6U);
	if (error != RDP_OK)
		return error;
	error = rdp_buf_append_u16_le(buf, 0x041DU);
	if (error != RDP_OK)
		return error;
	return rdp_buf_append_zeros(buf, 6U);
}

/* Appends the Bitmap Capability Set (28 bytes). */
static int
append_capset_bitmap(
	struct rdp_buf *buf,
	uint32_t width,
	uint32_t height)
{
	int error;
	error = rdp_buf_append_u16_le(buf, CAPSTYPE_BITMAP);
	if (error != RDP_OK)
		return error;
	error = rdp_buf_append_u16_le(buf, 28U);
	if (error != RDP_OK)
		return error;
	error = rdp_buf_append_u16_le(buf, 32U);
	if (error != RDP_OK)
		return error;
	error = rdp_buf_append_u16_le(buf, 1U);
	if (error != RDP_OK)
		return error;
	error = rdp_buf_append_u16_le(buf, 1U);
	if (error != RDP_OK)
		return error;
	error = rdp_buf_append_u16_le(buf, 1U);
	if (error != RDP_OK)
		return error;
	error = rdp_buf_append_u16_le(buf, (uint16_t)width);
	if (error != RDP_OK)
		return error;
	error = rdp_buf_append_u16_le(buf, (uint16_t)height);
	if (error != RDP_OK)
		return error;
	error = rdp_buf_append_u16_le(buf, 0U);
	if (error != RDP_OK)
		return error;
	error = rdp_buf_append_u16_le(buf, 1U);
	if (error != RDP_OK)
		return error;
	error = rdp_buf_append_u16_le(buf, 1U);
	if (error != RDP_OK)
		return error;
	return rdp_buf_append_zeros(buf, 6U);
}

/* Appends the Order Capability Set (88 bytes, legacy orders disabled). */
static int
append_capset_order(
	struct rdp_buf *buf)
{
	int error;
	error = rdp_buf_append_u16_le(buf, CAPSTYPE_ORDER);
	if (error != RDP_OK)
		return error;
	error = rdp_buf_append_u16_le(buf, 88U);
	if (error != RDP_OK)
		return error;
	return rdp_buf_append_zeros(buf, 84U);
}

/* Appends the Pointer Capability Set (10 bytes). */
static int
append_capset_pointer(
	struct rdp_buf *buf)
{
	int error;
	error = rdp_buf_append_u16_le(buf, CAPSTYPE_POINTER);
	if (error != RDP_OK)
		return error;
	error = rdp_buf_append_u16_le(buf, 10U);
	if (error != RDP_OK)
		return error;
	error = rdp_buf_append_u16_le(buf, 1U);
	if (error != RDP_OK)
		return error;
	error = rdp_buf_append_u16_le(buf, 20U);
	if (error != RDP_OK)
		return error;
	return rdp_buf_append_u16_le(buf, 20U);
}

/* Appends the Share Capability Set (8 bytes). */
static int
append_capset_share(
	struct rdp_buf *buf,
	uint16_t channel_id)
{
	int error;
	error = rdp_buf_append_u16_le(buf, CAPSTYPE_SHARE);
	if (error != RDP_OK)
		return error;
	error = rdp_buf_append_u16_le(buf, 8U);
	if (error != RDP_OK)
		return error;
	error = rdp_buf_append_u16_le(buf, channel_id);
	if (error != RDP_OK)
		return error;
	return rdp_buf_append_u16_le(buf, 0U);
}

/* Appends the Input Capability Set (88 bytes). */
static int
append_capset_input(
	struct rdp_buf *buf)
{
	int error;
	error = rdp_buf_append_u16_le(buf, CAPSTYPE_INPUT);
	if (error != RDP_OK)
		return error;
	error = rdp_buf_append_u16_le(buf, 88U);
	if (error != RDP_OK)
		return error;

	/* INPUT_FLAG_SCANCODES | MOUSEX | FASTPATH_INPUT2 | UNICODE. */
	error = rdp_buf_append_u16_le(buf, 0x0035U);
	if (error != RDP_OK)
		return error;
	return rdp_buf_append_zeros(buf, 82U);
}

/* Appends the Virtual Channel Capability Set (12 bytes). */
static int
append_capset_vc(
	struct rdp_buf *buf)
{
	int error;
	error = rdp_buf_append_u16_le(buf, CAPSTYPE_VIRTUALCHANNEL);
	if (error != RDP_OK)
		return error;
	error = rdp_buf_append_u16_le(buf, 12U);
	if (error != RDP_OK)
		return error;
	error = rdp_buf_append_u32_le(buf, 0U);
	if (error != RDP_OK)
		return error;
	return rdp_buf_append_u32_le(buf, RDP_VC_CHUNK_SIZE);
}

/* Appends the Surface Commands Capability Set (12 bytes). */
static int
append_capset_surfcmds(
	struct rdp_buf *buf)
{
	int error;
	error = rdp_buf_append_u16_le(buf, CAPSTYPE_SURFACE_COMMANDS);
	if (error != RDP_OK)
		return error;
	error = rdp_buf_append_u16_le(buf, 12U);
	if (error != RDP_OK)
		return error;
	error = rdp_buf_append_u32_le(buf, 0x00000052U);
	if (error != RDP_OK)
		return error;
	return rdp_buf_append_u32_le(buf, 0U);
}

/* Appends the Multifragment Update Capability Set (8 bytes). */
static int
append_capset_multifrag(
	struct rdp_buf *buf,
	uint32_t max_size)
{
	int error;
	error = rdp_buf_append_u16_le(buf, CAPSTYPE_MULTIFRAGMENTUPDATE);
	if (error != RDP_OK)
		return error;
	error = rdp_buf_append_u16_le(buf, 8U);
	if (error != RDP_OK)
		return error;
	return rdp_buf_append_u32_le(buf, max_size);
}

/* Appends the Frame Acknowledge Capability Set (8 bytes). */
static int
append_capset_frame_ack(
	struct rdp_buf *buf)
{
	int error;
	error = rdp_buf_append_u16_le(buf, CAPSTYPE_FRAME_ACKNOWLEDGE);
	if (error != RDP_OK)
		return error;
	error = rdp_buf_append_u16_le(buf, 8U);
	if (error != RDP_OK)
		return error;
	return rdp_buf_append_u32_le(buf, 4U);
}

/* Appends an 18-byte Share Data Header for a Share Data PDU. */
static int
encode_share_data_hdr(
	struct rdp_buf *buf,
	uint32_t share_id,
	uint16_t src_ch,
	uint8_t pdu_type2,
	size_t body_len)
{
	size_t total_len;
	int error;
	total_len = 18U + body_len;
	error = rdp_buf_append_u16_le(buf, (uint16_t)total_len);
	if (error != RDP_OK)
		return error;
	error = rdp_buf_append_u16_le(buf, RDP_PDUTYPE_DATAPDU);
	if (error != RDP_OK)
		return error;
	error = rdp_buf_append_u16_le(buf, src_ch);
	if (error != RDP_OK)
		return error;
	error = rdp_buf_append_u32_le(buf, share_id);
	if (error != RDP_OK)
		return error;
	error = rdp_buf_append_u8(buf, 0U);
	if (error != RDP_OK)
		return error;
	error = rdp_buf_append_u8(buf, 1U);
	if (error != RDP_OK)
		return error;
	error = rdp_buf_append_u16_le(buf, (uint16_t)(4U + body_len));
	if (error != RDP_OK)
		return error;
	error = rdp_buf_append_u8(buf, pdu_type2);
	if (error != RDP_OK)
		return error;
	error = rdp_buf_append_u8(buf, 0U);
	if (error != RDP_OK)
		return error;
	return rdp_buf_append_u16_le(buf, 0U);
}
