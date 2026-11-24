/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Wire-format helpers for headless wayland <-> rdpd socketpair IPC.
 */

#include "rdp-headless.h"

#include <stdint.h>

static void put_be32(uint8_t *p, uint32_t v);
static uint32_t get_be32(const uint8_t *p);
static void put_be64(uint8_t *p, uint64_t v);
static uint64_t get_be64(const uint8_t *p);

static void
put_be32(
	uint8_t *p,
	uint32_t v)
{
	p[0] = (uint8_t)(v >> 24);
	p[1] = (uint8_t)((v >> 16) & 0xFFU);
	p[2] = (uint8_t)((v >> 8) & 0xFFU);
	p[3] = (uint8_t)(v & 0xFFU);
}

static uint32_t
get_be32(
	const uint8_t *p)
{
	return ((uint32_t)p[0] << 24) |
	       ((uint32_t)p[1] << 16) |
	       ((uint32_t)p[2] << 8) |
	       (uint32_t)p[3];
}

static void
put_be64(
	uint8_t *p,
	uint64_t v)
{
	put_be32(p, (uint32_t)(v >> 32));
	put_be32(p + 4U, (uint32_t)(v & 0xFFFFFFFFU));
}

static uint64_t
get_be64(
	const uint8_t *p)
{
	return ((uint64_t)get_be32(p) << 32) | (uint64_t)get_be32(p + 4U);
}

/* Encodes a 12-byte control message (RESIZE or FORCE_KEY) in big-endian. */
void
kwl_rdp_pack_ctrl(
	uint8_t out[KWL_RDP_CTRL_HDR_SIZE],
	uint32_t type,
	uint32_t arg1,
	uint32_t arg2)
{
	put_be32(out, type);
	put_be32(out + 4U, arg1);
	put_be32(out + 8U, arg2);
}

/* Decodes a 12-byte control message from big-endian. */
void
kwl_rdp_unpack_ctrl(
	const uint8_t in[KWL_RDP_CTRL_HDR_SIZE],
	uint32_t *type,
	uint32_t *arg1,
	uint32_t *arg2)
{
	*type = get_be32(in);
	*arg1 = get_be32(in + 4U);
	*arg2 = get_be32(in + 8U);
}

/* Encodes a 20-byte frame header in big-endian. */
void
kwl_rdp_pack_frame_hdr(
	uint8_t out[KWL_RDP_FRAME_HDR_SIZE],
	uint32_t flags,
	uint64_t pts_us,
	uint32_t size)
{
	put_be32(out, KWL_RDP_MSG_FRAME);
	put_be32(out + 4U, flags);
	put_be64(out + 8U, pts_us);
	put_be32(out + 16U, size);
}

/* Decodes a 20-byte frame header from big-endian. */
void
kwl_rdp_unpack_frame_hdr(
	const uint8_t in[KWL_RDP_FRAME_HDR_SIZE],
	uint32_t *flags,
	uint64_t *pts_us,
	uint32_t *size)
{
	*flags = get_be32(in + 4U);
	*pts_us = get_be64(in + 8U);
	*size = get_be32(in + 16U);
}

/* Clamps and even-aligns headless dimensions into [320..3840] x [240..2160]. */
void
kwl_rdp_clamp_size(
	uint32_t *width,
	uint32_t *height)
{
	uint32_t w;
	uint32_t h;
	w = *width;
	h = *height;
	if (w < KWL_RDP_MIN_WIDTH)
		w = KWL_RDP_MIN_WIDTH;
	if (w > KWL_RDP_MAX_WIDTH)
		w = KWL_RDP_MAX_WIDTH;
	if (h < KWL_RDP_MIN_HEIGHT)
		h = KWL_RDP_MIN_HEIGHT;
	if (h > KWL_RDP_MAX_HEIGHT)
		h = KWL_RDP_MAX_HEIGHT;
	w &= ~1U;
	h &= ~1U;
	*width = w;
	*height = h;
}
