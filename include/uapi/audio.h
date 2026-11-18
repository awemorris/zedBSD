/* -*- mode: c; c-file-style: "linux"; tab-width: 8; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * OSS-style audio requests for /dev/dspN and /dev/mixerN.
 *
 * The names follow OSS soundcard.h, but the encoding is zedBSD's own and
 * no OSS binary compatibility is intended.  The kernel accepts only the
 * formats the hardware takes; it neither converts nor mixes.
 */

#ifndef KERN_UAPI_AUDIO_H
#define KERN_UAPI_AUDIO_H

#include <stdint.h>
#include <uapi/ioctl.h>

#define KERN_AUDIO_IOC_GROUP		'A'

/* Sample encodings. */
#define KERN_AUDIO_FORMAT_S16_LE	1U
#define KERN_AUDIO_FORMAT_S32_LE	2U

/* One exact hardware format; reserved must be zero. */
struct audio_format {
	uint32_t format;
	uint32_t channels;
	uint32_t rate;
	uint32_t reserved;
};

/* The ring shape chosen by the framework. */
struct audio_buffer_info {
	uint32_t fragment_bytes;
	uint32_t fragment_count;
	uint32_t bytes_per_frame;
	uint32_t reserved;
};

/*
 * Immediate room (playback) or data (capture), in the manner of OSS
 * audio_buf_info.  transferred counts every byte the hardware moved
 * since open; underruns and overruns count since open.
 */
struct audio_space {
	uint32_t fragments;
	uint32_t fragment_bytes;
	uint32_t bytes;
	uint32_t reserved;
	uint64_t transferred;
	uint32_t underruns;
	uint32_t overruns;
};

/* Volume in percent; 0..100 per channel and a mute flag of 0 or 1. */
struct audio_volume {
	uint32_t left;
	uint32_t right;
	uint32_t muted;
	uint32_t reserved;
};

#define KERN_AUDIO_GET_FORMAT	_IOR(KERN_AUDIO_IOC_GROUP, 1, struct audio_format)
#define KERN_AUDIO_SET_FORMAT	_IOWR(KERN_AUDIO_IOC_GROUP, 2, struct audio_format)
#define KERN_AUDIO_GET_BUFFER	_IOR(KERN_AUDIO_IOC_GROUP, 3, struct audio_buffer_info)
#define KERN_AUDIO_GET_OSPACE	_IOR(KERN_AUDIO_IOC_GROUP, 4, struct audio_space)
#define KERN_AUDIO_GET_ISPACE	_IOR(KERN_AUDIO_IOC_GROUP, 5, struct audio_space)
#define KERN_AUDIO_DRAIN	_IO(KERN_AUDIO_IOC_GROUP, 6)
#define KERN_AUDIO_FLUSH	_IO(KERN_AUDIO_IOC_GROUP, 7)

/*
 * Bytes written but not yet played: the write position minus the
 * hardware position.  Hardware FIFO and codec latency are not included.
 */
#define KERN_AUDIO_GET_DELAY	_IOR(KERN_AUDIO_IOC_GROUP, 8, uint32_t)

/*
 * Memory-mapped streaming, in the manner of OSS mmap.
 *
 * A direction may map its ring with mmap(fd, 0, mmap_bytes, ...) before
 * read or write has moved it; read or write of that direction is then
 * refused with EBUSY until close.  As in OSS, PROT_WRITE maps the playback
 * ring and PROT_READ alone the capture ring.  A device opened write-only
 * cannot be mapped, so playback maps from an O_RDWR open.  SET_TRIGGER starts and stops a mapped
 * direction.  GET_OPTR and GET_IPTR report the frontier: for playback the
 * position up to which the device has taken the mapping's bytes (a writer
 * fills ahead of it, and a taken fragment is silenced in the mapping), for
 * capture the position up to which the mapping holds captured bytes.
 *
 * With KERN_AUDIO_CAP_MMAP_COPY the mapping is a copy of the DMA ring,
 * moved one fragment per interrupt; the frontier then runs mmap_lead_bytes
 * ahead of the hardware for playback.  Without it the mapping is the DMA
 * ring itself.  The interface is the same either way.
 */
#define KERN_AUDIO_CAP_MMAP		0x1U
#define KERN_AUDIO_CAP_TRIGGER		0x2U
#define KERN_AUDIO_CAP_MMAP_COPY	0x4U

#define KERN_AUDIO_TRIGGER_OUTPUT	0x1U
#define KERN_AUDIO_TRIGGER_INPUT	0x2U

/* What the stream node can do, and the mapping's shape. */
struct audio_caps {
	uint32_t caps;
	uint32_t mmap_bytes;
	uint32_t mmap_lead_bytes;
	uint32_t reserved;
};

/*
 * The frontier of a mapped direction, in the manner of OSS count_info:
 * bytes since the trigger, whole fragments it moved since the previous
 * request of this kind, and its offset within the mapping.
 */
struct audio_mmap_position {
	uint64_t bytes;
	uint32_t fragments;
	uint32_t offset;
};

#define KERN_AUDIO_GET_CAPS	_IOR(KERN_AUDIO_IOC_GROUP, 9, struct audio_caps)
#define KERN_AUDIO_GET_OPTR	_IOR(KERN_AUDIO_IOC_GROUP, 10, struct audio_mmap_position)
#define KERN_AUDIO_GET_IPTR	_IOR(KERN_AUDIO_IOC_GROUP, 11, struct audio_mmap_position)
#define KERN_AUDIO_SET_TRIGGER	_IOW(KERN_AUDIO_IOC_GROUP, 12, uint32_t)

#define KERN_AUDIO_GET_VOLUME	_IOR(KERN_AUDIO_IOC_GROUP, 16, struct audio_volume)
#define KERN_AUDIO_SET_VOLUME	_IOW(KERN_AUDIO_IOC_GROUP, 17, struct audio_volume)

#endif
