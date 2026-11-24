/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Internal definitions, wire constants, and buffer helpers for librdp.
 */

#ifndef LIBRDP_RDP_INTERNAL_H
#define LIBRDP_RDP_INTERNAL_H

#include "rdp.h"

#define UNUSED_PARAMETER(x)	((void)(x))

/* Maximum buffered frame or PDU size (16 MiB). */
#define RDP_MAX_PDU_SIZE	(16U * 1024U * 1024U)

/* Maximum number of AVC region rectangles accepted in one frame. */
#define RDP_MAX_AVC_RECTS	1024U

/* Static virtual channel chunk size per MS-RDPBCGR. */
#define RDP_VC_CHUNK_SIZE	1600U

/* Static virtual channel flags. */
#define RDP_CHANNEL_FLAG_FIRST	0x00000001U
#define RDP_CHANNEL_FLAG_LAST	0x00000002U

/* MCS static channel IDs assigned by the server. */
#define RDP_MCS_USER_BASE	1001U
#define RDP_MCS_IO_CHANNEL	1003U
#define RDP_MCS_DRDYNVC_CHANNEL	1004U

/* Dynamic virtual channel names. */
#define RDP_DYNVC_NAME_GFX	"Microsoft::Windows::RDS::Graphics"
#define RDP_DYNVC_NAME_DISP	"Microsoft::Windows::RDS::DisplayControl"

/* Fast-path input event types (3-bit code). */
#define RDP_FP_INPUT_SCANCODE	0U
#define RDP_FP_INPUT_MOUSE	1U
#define RDP_FP_INPUT_SYNC	3U
#define RDP_FP_INPUT_UNICODE	4U

/* RDPEGFX command identifiers. */
#define RDPGFX_CMD_WIRE_TO_SURFACE_1	0x0001U
#define RDPGFX_CMD_DELETE_SURFACE	0x0004U
#define RDPGFX_CMD_CREATE_SURFACE	0x0009U
#define RDPGFX_CMD_RESET_GRAPHICS	0x000BU
#define RDPGFX_CMD_MAP_SURFACE_OUTPUT	0x000CU
#define RDPGFX_CMD_START_FRAME		0x000DU
#define RDPGFX_CMD_END_FRAME		0x000EU
#define RDPGFX_CMD_FRAME_ACKNOWLEDGE	0x000FU
#define RDPGFX_CMD_CAPS_ADVERTISE	0x0012U
#define RDPGFX_CMD_CAPS_CONFIRM		0x0013U

/* RDPEGFX capability versions. */
#define RDPGFX_CAPVERSION_81		0x00080105U
#define RDPGFX_CAPVERSION_104		0x000A0400U
#define RDPGFX_CAPVERSION_106		0x000A0600U

/* RDPEGFX capability flags. */
#define RDPGFX_CAPS_FLAG_THINCLIENT	0x00000001U
#define RDPGFX_CAPS_FLAG_AVC420_ENABLED	0x00000010U

/* RDPEGFX pixel format. */
#define RDPGFX_PIXEL_XRGB_8888		0x20U

/* Share Control PDU types. */
#define RDP_PDUTYPE_DEMANDACTIVE	0x0011U
#define RDP_PDUTYPE_CONFIRMACTIVE	0x0013U
#define RDP_PDUTYPE_DEACTIVATEALL	0x0016U
#define RDP_PDUTYPE_DATAPDU		0x0017U

/* Share Data PDU2 types. */
#define RDP_PDUTYPE2_CONTROL		0x14U
#define RDP_PDUTYPE2_SYNCHRONIZE	0x1FU
#define RDP_PDUTYPE2_FONTLIST		0x27U
#define RDP_PDUTYPE2_FONTMAP		0x28U
#define RDP_PDUTYPE2_SET_ERROR_INFO	0x2FU

/* Share Control actions. */
#define RDP_CTRLACTION_REQUEST_CONTROL	0x0001U
#define RDP_CTRLACTION_GRANTED_CONTROL	0x0002U
#define RDP_CTRLACTION_COOPERATE	0x0004U

/* Security header flags. */
#define RDP_SEC_INFO_PKT		0x0040U
#define RDP_SEC_LICENSE_PKT		0x0080U

/*
 * Dynamic byte buffer used to accumulate incoming stream bytes and build
 * outgoing PDUs without fixed-size stack overflows.
 */
struct rdp_buf {
	uint8_t *data;
	size_t len;
	size_t cap;
};

/*
 * Read-only cursor over a wire packet slice.
 */
struct rdp_reader {
	const uint8_t *data;
	size_t len;
	size_t pos;
};

/*
 * Decoded incoming packet envelope from the transport stream.
 *
 * When is_fastpath is zero, payload points to the X.224 TPDU body inside
 * a TPKT frame.  When is_fastpath is nonzero, payload points to the
 * Fast-Path input event stream and event_count holds the event count.
 */
struct rdp_packet {
	int is_fastpath;
	uint8_t event_count;
	const uint8_t *payload;
	size_t payload_len;
};

/*
 * Decoded X.224 Connection Request or Confirm negotiation fields.
 */
struct rdp_x224_neg {
	uint8_t tpdu_code;
	uint8_t neg_type;
	uint8_t neg_flags;
	uint32_t protocols;
};

/*
 * Parameters extracted from MCS Connect-Initial or Connect-Response.
 */
struct rdp_mcs_connect_info {
	uint32_t width;
	uint32_t height;
	uint32_t early_caps;
	uint16_t io_channel;
	uint16_t drdynvc_channel;
	int has_drdynvc;
};

/*
 * Decoded MCS DomainMCSPDU envelope.
 */
struct rdp_mcs_pdu {
	uint8_t choice;
	uint8_t result;
	uint16_t user_id;
	uint16_t channel_id;
	const uint8_t *payload;
	size_t payload_len;
};

/*
 * Decoded TS_INFO_PACKET credential strings (UTF-8).
 */
struct rdp_sec_info {
	uint32_t flags;
	char domain[128];
	char user_name[128];
	char password[128];
};

/*
 * Decoded DemandActive or ConfirmActive summary.
 */
struct rdp_active_pdu {
	uint32_t share_id;
	uint32_t width;
	uint32_t height;
	uint32_t max_multifrag_size;
};

/*
 * Decoded DRDYNVC command message.
 */
struct rdp_dynvc_msg {
	uint8_t cmd;
	uint32_t channel_id;
	uint32_t creation_status;
	uint16_t version;
	uint32_t total_len;
	char channel_name[128];
	const uint8_t *data;
	size_t data_len;
};

/*
 * Static virtual channel reassembly state.
 */
struct rdp_vc_reasm {
	struct rdp_buf buf;
	int active;
};

/*
 * Dynamic virtual channel reassembly slot.
 */
struct rdp_dynvc_stream {
	uint32_t channel_id;
	uint32_t expected_len;
	struct rdp_buf buf;
	int active;
};

/*
 * Client protocol state progression.
 */
enum rdp_client_state {
	RDP_CLI_INIT = 0,
	RDP_CLI_WAIT_X224_CC,
	RDP_CLI_WAIT_TLS,
	RDP_CLI_WAIT_MCS_CONNECT_RSP,
	RDP_CLI_WAIT_ATTACH_CONFIRM,
	RDP_CLI_WAIT_JOIN_IO,
	RDP_CLI_WAIT_JOIN_USER,
	RDP_CLI_WAIT_JOIN_DYNVC,
	RDP_CLI_WAIT_LICENSE,
	RDP_CLI_WAIT_DEMAND_ACTIVE,
	RDP_CLI_WAIT_FINALIZE,
	RDP_CLI_CONNECTED,
	RDP_CLI_DISCONNECTED
};

/*
 * Server protocol state progression.
 */
enum rdp_server_state {
	RDP_SRV_WAIT_X224_CR = 0,
	RDP_SRV_WAIT_TLS,
	RDP_SRV_WAIT_MCS_CONNECT_INIT,
	RDP_SRV_WAIT_ERECT_DOMAIN,
	RDP_SRV_WAIT_ATTACH_USER,
	RDP_SRV_WAIT_JOIN_CHANNELS,
	RDP_SRV_WAIT_INFO_PKT,
	RDP_SRV_WAIT_AUTH_DECISION,
	RDP_SRV_WAIT_CONFIRM_ACTIVE,
	RDP_SRV_WAIT_FINALIZE,
	RDP_SRV_CONNECTED,
	RDP_SRV_DISCONNECTED
};

/* Buffer and wire reader/writer helpers (tpkt.c). */
void rdp_buf_init(struct rdp_buf *buf);
void rdp_buf_free(struct rdp_buf *buf);
void rdp_buf_clear(struct rdp_buf *buf);
int rdp_buf_reserve(struct rdp_buf *buf, size_t extra);
int rdp_buf_append(struct rdp_buf *buf, const void *data, size_t len);
int rdp_buf_append_u8(struct rdp_buf *buf, uint8_t v);
int rdp_buf_append_u16_le(struct rdp_buf *buf, uint16_t v);
int rdp_buf_append_u16_be(struct rdp_buf *buf, uint16_t v);
int rdp_buf_append_u32_le(struct rdp_buf *buf, uint32_t v);
int rdp_buf_append_zeros(struct rdp_buf *buf, size_t count);
void rdp_buf_consume(struct rdp_buf *buf, size_t count);

void rdp_reader_init(struct rdp_reader *r, const uint8_t *data, size_t len);
size_t rdp_reader_rem(const struct rdp_reader *r);
int rdp_read_u8(struct rdp_reader *r, uint8_t *out);
int rdp_read_u16_le(struct rdp_reader *r, uint16_t *out);
int rdp_read_u16_be(struct rdp_reader *r, uint16_t *out);
int rdp_read_u32_le(struct rdp_reader *r, uint32_t *out);
int rdp_read_bytes(struct rdp_reader *r, const uint8_t **out, size_t len);
int rdp_skip_bytes(struct rdp_reader *r, size_t len);

/* TPKT, X.224, and Fast-Path framing (tpkt.c). */
int rdp_stream_next_packet(struct rdp_buf *rx, struct rdp_packet *pkt,
			   size_t *consumed);
int rdp_tpkt_wrap(struct rdp_buf *out, const uint8_t *payload, size_t len);
int rdp_x224_encode_cr(struct rdp_buf *out, uint32_t requested_protocols);
int rdp_x224_encode_cc(struct rdp_buf *out, uint32_t selected_protocol);
int rdp_x224_encode_dt(struct rdp_buf *out, const uint8_t *mcs_data,
		       size_t mcs_len);
int rdp_x224_decode(const uint8_t *tpdu, size_t tpdu_len,
		    struct rdp_x224_neg *neg, const uint8_t **dt_payload,
		    size_t *dt_len);

/* MCS T.125 and GCC codec (mcs.c). */
int rdp_mcs_encode_connect_initial(struct rdp_buf *out, uint32_t width,
				   uint32_t height, int want_drdynvc);
int rdp_mcs_decode_connect_initial(const uint8_t *data, size_t len,
				   struct rdp_mcs_connect_info *info);
int rdp_mcs_encode_connect_response(struct rdp_buf *out,
				    uint16_t drdynvc_channel);
int rdp_mcs_decode_connect_response(const uint8_t *data, size_t len,
				    struct rdp_mcs_connect_info *info);
int rdp_mcs_encode_erect_domain(struct rdp_buf *out);
int rdp_mcs_encode_attach_user_req(struct rdp_buf *out);
int rdp_mcs_encode_attach_user_confirm(struct rdp_buf *out, uint16_t user_id);
int rdp_mcs_encode_channel_join_req(struct rdp_buf *out, uint16_t user_id,
				    uint16_t channel_id);
int rdp_mcs_encode_channel_join_confirm(struct rdp_buf *out, uint16_t user_id,
					uint16_t channel_id);
int rdp_mcs_encode_send_data_req(struct rdp_buf *out, uint16_t user_id,
				 uint16_t channel_id, const uint8_t *payload,
				 size_t len);
int rdp_mcs_encode_send_data_ind(struct rdp_buf *out, uint16_t user_id,
				 uint16_t channel_id, const uint8_t *payload,
				 size_t len);
int rdp_mcs_encode_disconnect(struct rdp_buf *out, uint8_t reason);
int rdp_mcs_decode_pdu(const uint8_t *data, size_t len,
		       struct rdp_mcs_pdu *pdu);

/* Security, licensing, capabilities, and share PDUs (sec.c). */
int rdp_sec_encode_info_pkt(struct rdp_buf *out, const struct rdp_client_config *cfg);
int rdp_sec_decode_info_pkt(const uint8_t *data, size_t len,
			    struct rdp_sec_info *info);
int rdp_sec_encode_license_valid(struct rdp_buf *out);
int rdp_sec_decode_license_valid(const uint8_t *data, size_t len);
int rdp_sec_encode_demand_active(struct rdp_buf *out, uint32_t share_id,
				 uint32_t width, uint32_t height,
				 uint16_t server_channel);
int rdp_sec_encode_confirm_active(struct rdp_buf *out, uint32_t share_id,
				  uint32_t width, uint32_t height,
				  uint16_t user_channel);
int rdp_sec_decode_active_pdu(const uint8_t *data, size_t len,
			      uint16_t expected_type,
			      struct rdp_active_pdu *out);
int rdp_sec_encode_share_data_sync(struct rdp_buf *out, uint32_t share_id,
				   uint16_t source_channel,
				   uint16_t target_user);
int rdp_sec_encode_share_data_control(struct rdp_buf *out, uint32_t share_id,
				      uint16_t source_channel,
				      uint16_t action, uint16_t grant_id,
				      uint32_t control_id);
int rdp_sec_encode_share_data_fontlist(struct rdp_buf *out, uint32_t share_id,
				       uint16_t source_channel);
int rdp_sec_encode_share_data_fontmap(struct rdp_buf *out, uint32_t share_id,
				      uint16_t source_channel);
int rdp_sec_encode_share_data_error_info(struct rdp_buf *out,
					 uint32_t share_id,
					 uint16_t source_channel,
					 uint32_t error_info);
int rdp_sec_decode_share_hdr(const uint8_t *data, size_t len,
			     uint16_t *pdu_type, uint8_t *pdu_type2,
			     uint32_t *share_id, const uint8_t **body,
			     size_t *body_len);

/* Static VC framing and DRDYNVC (drdynvc.c). */
int rdp_vc_reasm_feed(struct rdp_vc_reasm *reasm, const uint8_t *data,
		      size_t len, const uint8_t **out_msg, size_t *out_len);
void rdp_vc_reasm_reset(struct rdp_vc_reasm *reasm);
int rdp_vc_send_chunked(int (*send_mcs)(void *ctx, const uint8_t *chunk,
					size_t chunk_len),
			void *ctx, const uint8_t *msg, size_t msg_len);
int rdp_dynvc_encode_caps(struct rdp_buf *out, uint16_t version);
int rdp_dynvc_encode_create_req(struct rdp_buf *out, uint32_t channel_id,
				const char *name);
int rdp_dynvc_encode_create_rsp(struct rdp_buf *out, uint32_t channel_id,
				uint32_t status);
int rdp_dynvc_encode_data(struct rdp_buf *out, uint32_t channel_id,
			  const uint8_t *payload, size_t len);
int rdp_dynvc_encode_close_req(struct rdp_buf *out, uint32_t channel_id);
int rdp_dynvc_decode(const uint8_t *data, size_t len,
		     struct rdp_dynvc_msg *msg);
int rdp_dynvc_stream_feed(struct rdp_dynvc_stream *stream,
			  const struct rdp_dynvc_msg *msg,
			  const uint8_t **out_data, size_t *out_len);
void rdp_dynvc_stream_reset(struct rdp_dynvc_stream *stream);

/* MS-RDPEGFX codecs (rdpegfx.c). */
int rdp_gfx_encode_caps_advertise(struct rdp_buf *out);
int rdp_gfx_decode_caps_advertise(const uint8_t *data, size_t len,
				  uint32_t *best_version);
int rdp_gfx_encode_caps_confirm(struct rdp_buf *out, uint32_t version);
int rdp_gfx_decode_caps_confirm(const uint8_t *data, size_t len,
				uint32_t *version);
int rdp_gfx_encode_reset_graphics(struct rdp_buf *out, uint32_t width,
				  uint32_t height);
int rdp_gfx_encode_create_surface(struct rdp_buf *out, uint16_t surface_id,
				  uint16_t width, uint16_t height);
int rdp_gfx_encode_delete_surface(struct rdp_buf *out, uint16_t surface_id);
int rdp_gfx_encode_map_surface(struct rdp_buf *out, uint16_t surface_id);
int rdp_gfx_encode_start_frame(struct rdp_buf *out, uint32_t timestamp_ms,
			       uint32_t frame_id);
int rdp_gfx_encode_end_frame(struct rdp_buf *out, uint32_t frame_id);
int rdp_gfx_encode_wire_to_surface_avc(struct rdp_buf *out, uint16_t surface_id,
				       uint16_t codec_id, uint16_t width,
				       uint16_t height,
				       const struct rdp_avc_stream *avc);
int rdp_gfx_encode_frame_ack(struct rdp_buf *out, uint32_t queue_depth,
			     uint32_t frame_id, uint32_t total_decoded);
int rdp_gfx_decode_header(const uint8_t *data, size_t len, uint16_t *cmd_id,
			  const uint8_t **body, size_t *body_len);
int rdp_gfx_decode_avc_pdu(const uint8_t *body, size_t body_len,
			   uint16_t *codec_id, struct rdp_avc_stream *out,
			   struct rdp_avc_rect *luma_rects,
			   struct rdp_avc_quant *luma_quants,
			   struct rdp_avc_rect *chroma_rects,
			   struct rdp_avc_quant *chroma_quants,
			   uint32_t max_rects);
int rdp_gfx_decode_frame_ack(const uint8_t *body, size_t body_len,
			     uint32_t *queue_depth, uint32_t *frame_id,
			     uint32_t *total_decoded);

/* MS-RDPEDISP codecs (rdpedisp.c). */
int rdp_disp_encode_caps(struct rdp_buf *out);
int rdp_disp_encode_monitor_layout(struct rdp_buf *out, uint32_t width,
				   uint32_t height);
int rdp_disp_decode_monitor_layout(const uint8_t *data, size_t len,
				   uint32_t *width, uint32_t *height);

/* Fast-Path input PDU codecs (input.c). */
int rdp_input_encode_key(struct rdp_buf *out, uint16_t scancode,
			 uint16_t flags);
int rdp_input_encode_unicode(struct rdp_buf *out, uint16_t codepoint,
			     uint16_t flags);
int rdp_input_encode_mouse(struct rdp_buf *out, uint16_t flags, uint16_t x,
			   uint16_t y);
int rdp_input_encode_sync(struct rdp_buf *out, uint32_t toggle_flags);
int rdp_input_decode_events(const uint8_t *data, size_t len,
			    uint8_t event_count,
			    const struct rdp_callbacks *cb, void *user);

#endif
