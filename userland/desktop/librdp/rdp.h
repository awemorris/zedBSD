/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Public interface for the I/O-independent RDP protocol core.
 */

#ifndef LIBRDP_RDP_H
#define LIBRDP_RDP_H

#include <stddef.h>
#include <stdint.h>

/* Default TCP port for RDP connections. */
#define RDP_DEFAULT_PORT		3389U

/* Return codes shared by all librdp entry points. */
#define RDP_OK				0
#define RDP_ERR_INVAL			(-1)
#define RDP_ERR_NOMEM			(-2)
#define RDP_ERR_PROTO			(-3)
#define RDP_ERR_STATE			(-4)
#define RDP_ERR_SHORT			(-5)
#define RDP_ERR_UNSUPPORTED		(-6)
#define RDP_ERR_IO			(-7)

/* Negotiation protocol flags from MS-RDPBCGR. */
#define RDP_NEG_PROTO_RDP		0x00000000U
#define RDP_NEG_PROTO_SSL		0x00000001U
#define RDP_NEG_PROTO_HYBRID		0x00000002U

/* Graphics pipeline codec identifiers from MS-RDPEGFX. */
#define RDP_GFX_CODEC_AVC420		0x000BU
#define RDP_GFX_CODEC_AVC444		0x000EU

/* AVC444 encoding mode values carried in the LC field. */
#define RDP_AVC444_MODE_LUMA_CHROMA	0U
#define RDP_AVC444_MODE_LUMA_ONLY	1U
#define RDP_AVC444_MODE_CHROMA_ONLY	2U

/* Fast-path keyboard event flags from MS-RDPBCGR. */
#define RDP_KBD_FLAG_RELEASE		0x01U
#define RDP_KBD_FLAG_EXTENDED		0x02U
#define RDP_KBD_FLAG_EXTENDED1		0x04U

/* Fast-path mouse pointer flags from MS-RDPBCGR. */
#define RDP_PTR_FLAG_WHEEL_ROTATION	0x01FFU
#define RDP_PTR_FLAG_WHEEL_NEGATIVE	0x0100U
#define RDP_PTR_FLAG_WHEEL		0x0200U
#define RDP_PTR_FLAG_HWHEEL		0x0400U
#define RDP_PTR_FLAG_MOVE		0x0800U
#define RDP_PTR_FLAG_BUTTON1		0x1000U
#define RDP_PTR_FLAG_BUTTON2		0x2000U
#define RDP_PTR_FLAG_BUTTON3		0x4000U
#define RDP_PTR_FLAG_DOWN		0x8000U

/* Fast-path keyboard indicator sync flags. */
#define RDP_SYNC_SCROLL_LOCK		0x01U
#define RDP_SYNC_NUM_LOCK		0x02U
#define RDP_SYNC_CAPS_LOCK		0x04U
#define RDP_SYNC_KANA_LOCK		0x08U

/*
 * One inclusive-exclusive dirty rectangle in a graphics frame.
 *
 * The graphics pipeline pairs each rectangle with a quantizer entry so
 * the decoder or encoder can track per-region quality.
 */
struct rdp_avc_rect {
	uint16_t left;
	uint16_t top;
	uint16_t right;
	uint16_t bottom;
};

/*
 * Quantization and quality metadata for one AVC420 region rectangle.
 */
struct rdp_avc_quant {
	uint8_t qp;
	uint8_t quality;
};

/*
 * One H.264 Annex-B layer inside an AVC420 or AVC444 graphics frame.
 *
 * For AVC420 only the luma layer is populated; for AVC444 both luma and
 * chroma layers may be present depending on avc444_mode.
 */
struct rdp_avc_layer {
	uint32_t rect_count;
	const struct rdp_avc_rect *rects;
	const struct rdp_avc_quant *quants;
	const uint8_t *data;
	size_t data_len;
};

/*
 * Decoded or outgoing H.264 graphics payload for one RDPEGFX frame.
 */
struct rdp_avc_stream {
	struct rdp_avc_layer luma;
	struct rdp_avc_layer chroma;
	uint8_t avc444_mode;
};

/*
 * Event and transport callbacks supplied by the caller.
 *
 * send_bytes is mandatory; all other callbacks are optional.
 */
struct rdp_callbacks {
	int (*send_bytes)(void *user, const uint8_t *data, size_t len);
	void (*on_tls_required)(void *user);
	void (*on_connected)(void *user, uint32_t width, uint32_t height);
	void (*on_avc_frame)(void *user, uint32_t frame_id, uint16_t codec_id,
			     const struct rdp_avc_stream *avc);
	void (*on_frame_ack)(void *user, uint32_t frame_id,
			     uint32_t queue_depth, uint32_t total_decoded);
	void (*on_resize)(void *user, uint32_t width, uint32_t height);
	void (*on_input_key)(void *user, uint16_t scancode, uint16_t flags);
	void (*on_input_unicode)(void *user, uint16_t codepoint,
				 uint16_t flags);
	void (*on_input_mouse)(void *user, uint16_t flags, uint16_t x,
			       uint16_t y);
	void (*on_input_sync)(void *user, uint32_t toggle_flags);
	void (*on_auth_request)(void *user, const char *domain,
				const char *user_name, const char *password);
	void (*on_disconnect)(void *user, uint32_t error_info);
};

/*
 * Client connection parameters passed to rdp_client_connect().
 */
struct rdp_client_config {
	uint32_t width;
	uint32_t height;
	const char *domain;
	const char *user_name;
	const char *password;
	const char *client_name;
};

/*
 * Server configuration passed to rdp_server_create().
 */
struct rdp_server_config {
	uint32_t width;
	uint32_t height;
};

struct rdp_client;
struct rdp_server;

/* Client lifecycle and I/O entry points. */
struct rdp_client *rdp_client_create(const struct rdp_callbacks *cb,
				     void *user);
void rdp_client_destroy(struct rdp_client *client);
int rdp_client_connect(struct rdp_client *client,
		       const struct rdp_client_config *cfg);
int rdp_client_tls_established(struct rdp_client *client);
int rdp_client_feed(struct rdp_client *client, const uint8_t *data,
		    size_t len);
int rdp_client_needs_tls(const struct rdp_client *client);
int rdp_client_is_connected(const struct rdp_client *client);
int rdp_client_disconnect(struct rdp_client *client);

/* Client input, acknowledgement, and resize requests. */
int rdp_client_send_key(struct rdp_client *client, uint16_t scancode,
			uint16_t flags);
int rdp_client_send_unicode(struct rdp_client *client, uint16_t codepoint,
			    uint16_t flags);
int rdp_client_send_mouse(struct rdp_client *client, uint16_t flags,
			  uint16_t x, uint16_t y);
int rdp_client_send_sync(struct rdp_client *client, uint32_t toggle_flags);
int rdp_client_send_frame_ack(struct rdp_client *client, uint32_t frame_id,
			      uint32_t queue_depth);
int rdp_client_request_resize(struct rdp_client *client, uint32_t width,
			      uint32_t height);

/* Server lifecycle and I/O entry points. */
struct rdp_server *rdp_server_create(const struct rdp_callbacks *cb,
				     void *user,
				     const struct rdp_server_config *cfg);
void rdp_server_destroy(struct rdp_server *server);
int rdp_server_tls_established(struct rdp_server *server);
int rdp_server_feed(struct rdp_server *server, const uint8_t *data,
		    size_t len);
int rdp_server_needs_tls(const struct rdp_server *server);
int rdp_server_is_connected(const struct rdp_server *server);
int rdp_server_accept_auth(struct rdp_server *server);
int rdp_server_reject_auth(struct rdp_server *server, uint32_t error_info);
int rdp_server_disconnect(struct rdp_server *server, uint32_t error_info);

/* Server graphics output and flow-control queries. */
int rdp_server_send_avc_frame(struct rdp_server *server, uint16_t codec_id,
			      const struct rdp_avc_stream *avc,
			      uint32_t timestamp_ms, uint32_t *out_frame_id);
int rdp_server_resize(struct rdp_server *server, uint32_t width,
		      uint32_t height);
uint32_t rdp_server_unacked_frames(const struct rdp_server *server);

#endif
