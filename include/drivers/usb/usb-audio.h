/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * USB Audio Class (UAC 1.0 and 2.0) driver definitions.
 *
 * usb-audio.c binds the AudioControl interface, claims associated
 * AudioStreaming interfaces, registers /dev/dspN and /dev/mixerN via
 * drv_audio_register(), and handles Feature Unit volume/mute controls.
 * usb-audio-stream.c runs the playback and capture Isochronous URB
 * pipelines, worker threads, and feedback endpoint handling.
 */

#ifndef KERN_DRIVERS_USB_AUDIO_H
#define KERN_DRIVERS_USB_AUDIO_H

#include <stddef.h>
#include <stdint.h>

#include <drivers/audio/audio.h>
#include <drivers/usb/usb-audio-desc.h>
#include <drivers/usb/usb-audio-pcm.h>
#include <drivers/usb/usb.h>
#include <kern/lock.h>
#include <kern/waitq.h>

struct thread;

/* Isochronous pipeline sizing: 4 URBs of up to 32 packets each. */
#define USB_AUDIO_URBS			4U
#define USB_AUDIO_PACKETS_FS		8U
#define USB_AUDIO_PACKETS_HS		32U
#define USB_AUDIO_PACKETS_MAX		32U
#define USB_AUDIO_FORMATS_MAX		32U
#define USB_AUDIO_STREAM_IFACES_MAX	4U

/* One direction (playback or capture) of a USB Audio device. */
struct usb_audio_stream {
	struct usb_audio *audio;
	unsigned capture;
	unsigned available;
	struct drv_usb_interface *usb_iface;
	uint8_t iface_number;
	struct uac_as_alt alts[UAC_ALTS_MAX];
	unsigned alt_count;
	const struct uac_as_alt *active_alt;

	/* Ring buffer state supplied by audio framework prepare(). */
	uint8_t *ring;
	uint32_t fragment_bytes;
	uint32_t fragment_count;
	uint32_t ring_bytes;
	struct audio_format format;
	unsigned prepared;

	/* Handshake with URB completion callbacks and worker thread. */
	struct spinlock lock;
	struct wait_queue waitq;
	struct thread *worker;
	unsigned running;
	unsigned stopping;
	unsigned submitting;
	unsigned done_mask;
	unsigned submitted_mask;
	unsigned next;
	uint32_t hw_pos;
	uint32_t frag_accum;

	/* Isochronous data URBs (zero-copy directly on DMA ring). */
	struct drv_usb_endpoint *data_ep;
	unsigned packet_count;
	size_t urb_bytes;
	struct drv_usb_urb *urbs[USB_AUDIO_URBS];
	struct drv_usb_iso_packet packets[USB_AUDIO_URBS][USB_AUDIO_PACKETS_MAX];
	uint32_t submit_pos;
	unsigned retained;

	/* Feedback IN endpoint for asynchronous OUT streams. */
	struct drv_usb_endpoint *fb_ep;
	struct drv_usb_urb *fb_urb;
	uint8_t fb_buf[8];
	unsigned fb_done;
	unsigned fb_submitted;
	uint32_t fb_nominal_q16;
	uint32_t fb_rate_q16;
	uint32_t fb_accum_q16;

	/* Bresenham rate state for non-feedback streams. */
	struct uac_bresenham bresenham;
	uint32_t packets_per_sec;
};

/* Cached Feature Unit volume range in 1/256 dB units. */
struct usb_audio_vol_range {
	unsigned valid;
	int16_t min_db;
	int16_t max_db;
	int16_t res_db;
};

/* One bound USB Audio Class device. */
struct usb_audio {
	struct drv_usb_interface *control_iface;
	uint8_t control_iface_num;
	struct drv_usb_device *device;
	enum uac_version version;
	enum drv_usb_speed speed;
	char tag[16];
	char product[64];

	struct uac_control_desc control;
	struct drv_usb_interface *claimed_ifaces[USB_AUDIO_STREAM_IFACES_MAX];
	unsigned claimed_count;

	struct usb_audio_stream playback;
	struct usb_audio_stream capture;

	struct audio_format formats[USB_AUDIO_FORMATS_MAX];
	unsigned format_count;

	struct drv_audio_ops ops;
	struct drv_audio_device *audio_dev;
	struct mutex mutex;
	unsigned disconnected;
	unsigned unregister_pending;

	struct usb_audio_vol_range vol_range;
	struct audio_volume cached_vol;
};

/*
 * Registers the USB Audio Class driver with the USB core.
 */
int
drv_usb_audio_driver_register(void);

/*
 * Initializes stream synchronization primitives.
 */
void
usb_audio_stream_init(
	struct usb_audio *audio,
	struct usb_audio_stream *stream,
	unsigned capture);

/*
 * Selects the best matching alternate setting, configures sample rate, and
 * binds the DMA ring buffer for a stream.
 */
int
usb_audio_stream_prepare(
	struct usb_audio_stream *stream,
	const struct audio_format *format,
	const struct drv_dma_buffer *buffer,
	uint32_t fragment_bytes,
	uint32_t fragment_count);

/*
 * Allocates URBs, starts the worker thread, and begins isochronous transfers.
 */
int
usb_audio_stream_start(
	struct usb_audio_stream *stream);

/*
 * Stops the worker thread, cancels/drains URBs, and resets alternate 0.
 * Returns 0 or EBUSY if an URB did not retire from the HCD.
 */
int
usb_audio_stream_stop(
	struct usb_audio_stream *stream);

/*
 * Returns the current hardware byte offset within the DMA ring.
 */
uint32_t
usb_audio_stream_position(
	struct usb_audio_stream *stream);

#endif
