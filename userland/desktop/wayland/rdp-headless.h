/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Headless offscreen Vulkan output, H.264 encoder, and RDP IPC channel
 * for the Wayland compositor (--headless --rdp-fd=N).
 */

#ifndef KWL_RDP_HEADLESS_H
#define KWL_RDP_HEADLESS_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* IPC message types between wayland --headless and rdpd. */
#define KWL_RDP_MSG_FRAME	1U
#define KWL_RDP_MSG_RESIZE	2U
#define KWL_RDP_MSG_FORCE_KEY	3U

/* Frame flags in KWL_RDP_MSG_FRAME header. */
#define KWL_RDP_FRAME_KEY	1U

/* Maximum headless output dimensions (4K UHD). */
#define KWL_RDP_MAX_WIDTH	3840U
#define KWL_RDP_MAX_HEIGHT	2160U
#define KWL_RDP_MIN_WIDTH	320U
#define KWL_RDP_MIN_HEIGHT	240U

/* Wire header sizes (bytes). */
#define KWL_RDP_FRAME_HDR_SIZE	20U
#define KWL_RDP_CTRL_HDR_SIZE	12U
struct kwl_server;
struct kwl_rdp;

/* Encodes a 12-byte control message (RESIZE or FORCE_KEY) in big-endian. */
void kwl_rdp_pack_ctrl(uint8_t out[KWL_RDP_CTRL_HDR_SIZE], uint32_t type, uint32_t arg1, uint32_t arg2);

/* Decodes a 12-byte control message from big-endian. */
void kwl_rdp_unpack_ctrl(const uint8_t in[KWL_RDP_CTRL_HDR_SIZE], uint32_t *type, uint32_t *arg1, uint32_t *arg2);

/* Encodes a 20-byte frame header in big-endian. */
void kwl_rdp_pack_frame_hdr(uint8_t out[KWL_RDP_FRAME_HDR_SIZE], uint32_t flags, uint64_t pts_us, uint32_t size);

/* Decodes a 20-byte frame header from big-endian. */
void kwl_rdp_unpack_frame_hdr(const uint8_t in[KWL_RDP_FRAME_HDR_SIZE], uint32_t *flags, uint64_t *pts_us, uint32_t *size);

/* Clamps and even-aligns headless dimensions into [320..3840] x [240..2160]. */
void kwl_rdp_clamp_size(uint32_t *width, uint32_t *height);

/* Initializes headless Vulkan offscreen image, staging buffer, and H.264 encoder. */
int kwl_rdp_open(struct kwl_server *server, void *device, void *physical, void *queue, uint32_t queue_family, uint32_t width, uint32_t height, void **out_image);

/* Recreates offscreen image and encoder for a dynamic resize. */
int kwl_rdp_resize(struct kwl_server *server, uint32_t width, uint32_t height, void **out_image);

/* Recreates compositor targets and offscreen image for a dynamic resize. */
void kwl_compose_headless_resize(struct kwl_server *server, uint32_t width, uint32_t height);

/* Reads back the rendered offscreen image, encodes H.264 Annex-B AU, and sends it on rdp_fd. */
int kwl_rdp_encode_and_send(struct kwl_rdp *rdp, void *image, uint32_t width, uint32_t height, uint64_t pts_us);

/* Polls rdp_fd for RESIZE / FORCE_KEY control messages from rdpd. */
void kwl_rdp_poll(struct kwl_server *server);

/* Releases all headless Vulkan and encoder resources. */
void kwl_rdp_close(struct kwl_server *server);

#ifdef __cplusplus
}
#endif

#endif
