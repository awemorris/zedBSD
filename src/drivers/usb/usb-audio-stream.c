/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * USB Audio Class streaming engine: zero-copy multi-URB xHCI Isochronous
 * pipeline directly on the audio DMA ring, worker threads, feedback
 * endpoint handling, and Bresenham rate generation.
 */

#include <drivers/audio/audio.h>
#include <drivers/usb/usb-audio.h>
#include <drivers/usb/usb.h>

#include <kern/atomic.h>
#include <kern/clock.h>
#include <kern/kcrt.h>
#include <kern/klog.h>
#include <kern/kmem.h>
#include <kern/lock.h>
#include <kern/sched.h>
#include <kern/thread.h>
#include <kern/waitq.h>
#include <uapi/errno.h>

/* Control transfer timeout for sample-rate requests. */
#define USB_AUDIO_CTRL_TIMEOUT_MS	1000U

/* Timeout when draining cancelled URBs during stop or disconnect. */
#define USB_AUDIO_DRAIN_MS		1000U

/* Retries for busy SET_INTERFACE transitions. */
#define USB_AUDIO_ALT_TRIES		10U
#define USB_AUDIO_ALT_STEP_MS		5U

/* Worker wake interval when waiting for completions. */
#define USB_AUDIO_WATCHDOG_MS		250U

/* Worker scheduling priority. */
#define USB_AUDIO_WORKER_PRIORITY	SCHED_PRIORITY_DEFAULT

static const struct uac_as_alt *usb_audio_pick_alt(const struct usb_audio_stream *stream, const struct audio_format *format);
static int usb_audio_select_alt(struct usb_audio_stream *stream, unsigned setting);
static int usb_audio_set_sample_rate(struct usb_audio *audio, const struct uac_as_alt *alt, uint32_t rate);
static struct drv_usb_endpoint *usb_audio_find_ep(struct drv_usb_interface *iface, uint8_t ep_addr);
static int usb_audio_alloc_urbs(struct usb_audio_stream *stream);
static void usb_audio_free_urbs(struct usb_audio_stream *stream);
static int usb_audio_join_worker(struct usb_audio_stream *stream);
static void usb_audio_urb_cb(struct drv_usb_urb *urb, void *argument);
static void usb_audio_fb_cb(struct drv_usb_urb *urb, void *argument);
static void usb_audio_worker(void *argument);
static void usb_audio_process_urb(struct usb_audio_stream *stream, unsigned index);
static void usb_audio_process_fb(struct usb_audio_stream *stream);
static void usb_audio_submit_urb(struct usb_audio_stream *stream, unsigned index);
static void usb_audio_submit_fb(struct usb_audio_stream *stream);
static void usb_audio_advance_hw(struct usb_audio_stream *stream, uint32_t ring_bytes);
static int usb_audio_retire_urb(struct drv_usb_urb *urb);
static void usb_audio_sleep_ms(unsigned milliseconds);

/*
 * Initializes stream synchronization primitives.
 */
void
usb_audio_stream_init(
	struct usb_audio *audio,
	struct usb_audio_stream *stream,
	unsigned capture)
{
	kern_memset(stream, 0, sizeof(*stream));
	stream->audio = audio;
	stream->capture = capture;
	spin_init(&stream->lock, LOCK_RANK_DEVICE, "usb-audio stream");
	waitq_init(&stream->waitq, "usb-audio stream");
}

/*
 * Selects the exact matching native alternate setting, configures sample
 * rate, and binds the DMA ring buffer for zero-copy streaming.
 */
int
usb_audio_stream_prepare(
	struct usb_audio_stream *stream,
	const struct audio_format *format,
	const struct drv_dma_buffer *buffer,
	uint32_t fragment_bytes,
	uint32_t fragment_count)
{
	const struct uac_as_alt *alt;
	unsigned long irq;
	int error;

	if (stream == NULL || format == NULL || buffer == NULL || buffer->address == NULL)
		return EINVAL;

	if (stream->audio->disconnected != 0U)
		return ENODEV;

	alt = usb_audio_pick_alt(stream, format);
	if (alt == NULL)
		return EINVAL;

	/* Selects the target alternate setting and programs the sample rate. */
	error = usb_audio_select_alt(stream, alt->alternate_setting);
	if (error != 0)
		return error;

	error = usb_audio_set_sample_rate(stream->audio, alt, format->rate);
	if (error != 0)
		return error;

	irq = spin_lock_irqsave(&stream->lock);

	stream->active_alt = alt;
	stream->ring = (uint8_t *)buffer->address;
	stream->fragment_bytes = fragment_bytes;
	stream->fragment_count = fragment_count;
	stream->ring_bytes = fragment_bytes * fragment_count;
	stream->format = *format;
	stream->hw_pos = 0U;
	stream->submit_pos = 0U;
	stream->frag_accum = 0U;
	stream->prepared = 1U;

	spin_unlock_irqrestore(&stream->lock, irq);

	/* Succeeded: stream prepared with native alternate and DMA ring. */
	return 0;
}

/*
 * Allocates URBs, starts the worker thread, and begins isochronous transfers.
 */
int
usb_audio_stream_start(
	struct usb_audio_stream *stream)
{
	const struct uac_as_alt *alt;
	unsigned long irq;
	uint32_t interval_shift;
	unsigned all_mask;
	int error;

	if (stream == NULL || stream->prepared == 0U || stream->active_alt == NULL)
		return EINVAL;

	if (stream->audio->disconnected != 0U)
		return ENODEV;

	if (stream->running != 0U)
		return 0;

	alt = stream->active_alt;

	/* Resolves data endpoint and optional feedback IN endpoint. */
	stream->data_ep = usb_audio_find_ep(stream->usb_iface, alt->ep_address);
	if (stream->data_ep == NULL)
		return ENODEV;

	stream->fb_ep = NULL;
	if (stream->capture == 0U && alt->sync_ep_address != 0U)
		stream->fb_ep = usb_audio_find_ep(stream->usb_iface, alt->sync_ep_address);

	/* Computes packet rate and URB geometry for Full-Speed vs High-Speed. */
	if (stream->audio->speed == DRV_USB_SPEED_HIGH ||
	    stream->audio->speed == DRV_USB_SPEED_SUPER ||
	    stream->audio->speed == DRV_USB_SPEED_SUPER_PLUS) {
		stream->packet_count = USB_AUDIO_PACKETS_HS;
		interval_shift = 0U;
		if (alt->ep_interval >= 1U && alt->ep_interval <= 4U)
			interval_shift = alt->ep_interval - 1U;
		stream->packets_per_sec = 8000U >> interval_shift;
	} else {
		stream->packet_count = USB_AUDIO_PACKETS_FS;
		stream->packets_per_sec = 1000U;
	}

	if (stream->packets_per_sec == 0U)
		stream->packets_per_sec = 1000U;

	stream->urb_bytes = (size_t)alt->ep_max_packet * stream->packet_count;
	if (stream->urb_bytes == 0U)
		return EINVAL;

	/* Initializes Bresenham and nominal feedback 16.16 fixed-point rate. */
	usb_audio_bresenham_init(&stream->bresenham, stream->format.rate, stream->packets_per_sec);
	stream->fb_nominal_q16 = (uint32_t)(((uint64_t)stream->format.rate << 16) / stream->packets_per_sec);
	stream->fb_rate_q16 = stream->fb_nominal_q16;
	stream->fb_accum_q16 = 0U;

	error = usb_audio_alloc_urbs(stream);
	if (error != 0) {
		usb_audio_free_urbs(stream);
		return error;
	}

	all_mask = (1U << USB_AUDIO_URBS) - 1U;
	irq = spin_lock_irqsave(&stream->lock);

	stream->stopping = 0U;
	stream->submitting = 0U;
	stream->done_mask = all_mask;
	stream->submitted_mask = 0U;
	stream->next = 0U;
	stream->hw_pos = 0U;
	stream->submit_pos = 0U;
	stream->frag_accum = 0U;
	stream->fb_done = (stream->fb_urb != NULL) ? 1U : 0U;
	stream->fb_submitted = 0U;

	spin_unlock_irqrestore(&stream->lock, irq);

	error = kthread_create(usb_audio_worker, stream, USB_AUDIO_WORKER_PRIORITY, &stream->worker);
	if (error != 0) {
		stream->worker = NULL;
		usb_audio_free_urbs(stream);
		return error;
	}

	stream->running = 1U;
	thread_start(stream->worker);
	kern_logf("usb-audio: %s: start %s alt=%u ep=0x%02x attr=0x%02x max=%u int=%u fb=0x%02x rate=%u pkts=%u/s\n",
	    stream->audio->tag, stream->capture ? "IN" : "OUT",
	    alt->alternate_setting, alt->ep_address, alt->ep_attributes,
	    alt->ep_max_packet, alt->ep_interval, alt->sync_ep_address,
	    stream->format.rate, stream->packets_per_sec);

	/* Succeeded: worker thread and zero-copy URB pipeline active. */
	return 0;
}

/*
 * Stops the worker thread, cancels/drains URBs, and resets alternate 0.
 * Returns 0 or EBUSY if an URB did not retire from the HCD.
 */
int
usb_audio_stream_stop(
	struct usb_audio_stream *stream)
{
	unsigned long irq;
	unsigned submitting;
	unsigned index;
	unsigned retained;
	enum drv_usb_urb_status status;
	int join_error;
	int error;

	if (stream == NULL)
		return 0;

	if (stream->running == 0U && stream->retained == 0U && stream->worker == NULL) {
		(void)usb_audio_select_alt(stream, 0U);
		return 0;
	}

	/* Signals worker to stop and forbids new submissions. */
	irq = spin_lock_irqsave(&stream->lock);

	stream->stopping = 1U;
	waitq_wake_all(&stream->waitq);

	spin_unlock_irqrestore(&stream->lock, irq);

	/* Waits for any in-progress submission to finish before cancelling. */
	for (;;) {
		irq = spin_lock_irqsave(&stream->lock);

		submitting = stream->submitting;

		spin_unlock_irqrestore(&stream->lock, irq);

		if (submitting == 0U)
			break;
		usb_audio_sleep_ms(1U);
	}

	/* Cancels all pending data URBs and the feedback URB. */
	for (index = 0U; index < USB_AUDIO_URBS; index++) {
		if (stream->urbs[index] == NULL)
			continue;
		status = drv_usb_urb_status(stream->urbs[index]);
		if (status == DRV_USB_URB_PENDING)
			(void)drv_usb_urb_cancel(stream->urbs[index]);
	}

	if (stream->fb_urb != NULL) {
		status = drv_usb_urb_status(stream->fb_urb);
		if (status == DRV_USB_URB_PENDING)
			(void)drv_usb_urb_cancel(stream->fb_urb);
	}

	/* Drains all URBs and counts any still held by the HCD. */
	retained = 0U;
	for (index = 0U; index < USB_AUDIO_URBS; index++) {
		if (stream->urbs[index] == NULL)
			continue;
		error = drv_usb_urb_drain(stream->urbs[index], USB_AUDIO_DRAIN_MS);
		if (error != 0)
			retained++;
	}

	if (stream->fb_urb != NULL) {
		error = drv_usb_urb_drain(stream->fb_urb, USB_AUDIO_DRAIN_MS);
		if (error != 0)
			retained++;
	}

	join_error = usb_audio_join_worker(stream);
	if (join_error != 0)
		kern_logf("usb-audio: %s: worker join failed (%d)\n", stream->audio->tag, join_error);

	(void)usb_audio_select_alt(stream, 0U);
	stream->running = 0U;

	if (retained != 0U) {
		stream->retained = retained;
		return EBUSY;
	}

	if (join_error != 0)
		return join_error;

	stream->retained = 0U;
	usb_audio_free_urbs(stream);

	/* Succeeded: stream stopped and resources released. */
	return 0;
}

/*
 * Returns the current hardware byte offset within the DMA ring.
 */
uint32_t
usb_audio_stream_position(
	struct usb_audio_stream *stream)
{
	unsigned long irq;
	uint32_t pos;

	if (stream == NULL)
		return 0U;

	irq = spin_lock_irqsave(&stream->lock);

	pos = stream->hw_pos;

	spin_unlock_irqrestore(&stream->lock, irq);

	/* Succeeded: ring byte offset. */
	return pos;
}

/* Finds the exact native AudioStreaming alternate matching the requested PCM format. */
static const struct uac_as_alt *
usb_audio_pick_alt(
	const struct usb_audio_stream *stream,
	const struct audio_format *format)
{
	unsigned index;
	const struct uac_as_alt *alt;
	uint8_t want_subslot;
	uint8_t want_bits;

	if (format->format == KERN_AUDIO_FORMAT_U8) {
		want_subslot = 1U;
		want_bits = 8U;
	} else if (format->format == KERN_AUDIO_FORMAT_S16_LE) {
		want_subslot = 2U;
		want_bits = 16U;
	} else if (format->format == KERN_AUDIO_FORMAT_S24_3LE) {
		want_subslot = 3U;
		want_bits = 24U;
	} else if (format->format == KERN_AUDIO_FORMAT_S24_LE) {
		want_subslot = 4U;
		want_bits = 24U;
	} else if (format->format == KERN_AUDIO_FORMAT_S32_LE) {
		want_subslot = 4U;
		want_bits = 32U;
	} else {
		return NULL;
	}

	for (index = 0U; index < stream->alt_count; index++) {
		alt = &stream->alts[index];
		if (alt->is_pcm == 0U || alt->ep_address == 0U)
			continue;
		if ((uint32_t)alt->channels != format->channels)
			continue;
		if (alt->subslot_size != want_subslot || alt->bit_resolution != want_bits)
			continue;
		if (uac_desc_alt_supports_rate(alt, format->rate) != 0)
			return alt;
	}

	return NULL;
}

/* Selects an alternate setting on the AudioStreaming interface. */
static int
usb_audio_select_alt(
	struct usb_audio_stream *stream,
	unsigned setting)
{
	unsigned attempt;
	int error;

	if (stream->usb_iface == NULL || stream->audio->disconnected != 0U)
		return 0;

	error = 0;
	for (attempt = 0U; attempt < USB_AUDIO_ALT_TRIES; attempt++) {
		error = drv_usb_interface_set_alternate(stream->usb_iface, setting);
		if (error == 0 || error == ENODEV)
			return 0;
		if (error != EBUSY)
			return error;
		usb_audio_sleep_ms(USB_AUDIO_ALT_STEP_MS);
	}

	return error;
}

/* Sends SET_CUR to configure the endpoint (UAC 1.0) or Clock Source (UAC 2.0) rate. */
static int
usb_audio_set_sample_rate(
	struct usb_audio *audio,
	const struct uac_as_alt *alt,
	uint32_t rate)
{
	uint8_t buf[4];
	int error;

	if (audio->version == UAC_VERSION_1) {
		if (alt->ep_freq_control == 0U)
			return 0;

		buf[0] = (uint8_t)(rate & 0xffU);
		buf[1] = (uint8_t)((rate >> 8) & 0xffU);
		buf[2] = (uint8_t)((rate >> 16) & 0xffU);

		error = drv_usb_control(
			audio->device,
			DRV_USB_DIR_OUT | DRV_USB_REQUEST_CLASS | DRV_USB_RECIP_ENDPOINT,
			UAC1_SET_CUR,
			(uint16_t)(UAC_EP_SAMPLING_FREQ_CONTROL << 8),
			(uint16_t)alt->ep_address,
			buf,
			3U,
			USB_AUDIO_CTRL_TIMEOUT_MS,
			NULL);
		return error;
	}

	if (alt->clock_id == 0U)
		return 0;

	buf[0] = (uint8_t)(rate & 0xffU);
	buf[1] = (uint8_t)((rate >> 8) & 0xffU);
	buf[2] = (uint8_t)((rate >> 16) & 0xffU);
	buf[3] = (uint8_t)((rate >> 24) & 0xffU);

	error = drv_usb_control(
		audio->device,
		DRV_USB_DIR_OUT | DRV_USB_REQUEST_CLASS | DRV_USB_RECIP_INTERFACE,
		UAC2_CUR,
		(uint16_t)(UAC_CS_SAM_FREQ_CONTROL << 8),
		(uint16_t)(((uint16_t)alt->clock_id << 8) | audio->control_iface_num),
		buf,
		4U,
		USB_AUDIO_CTRL_TIMEOUT_MS,
		NULL);
	return error;
}

/* Finds an endpoint on an interface by its bEndpointAddress. */
static struct drv_usb_endpoint *
usb_audio_find_ep(
	struct drv_usb_interface *iface,
	uint8_t ep_addr)
{
	unsigned count;
	unsigned index;
	struct drv_usb_endpoint *ep;

	count = drv_usb_interface_endpoint_count(iface);
	for (index = 0U; index < count; index++) {
		ep = drv_usb_interface_endpoint(iface, index);
		if (ep != NULL && drv_usb_endpoint_address(ep) == ep_addr)
			return ep;
	}

	return NULL;
}

/* Allocates data URBs and optional feedback URB without intermediate buffers. */
static int
usb_audio_alloc_urbs(
	struct usb_audio_stream *stream)
{
	unsigned index;

	for (index = 0U; index < USB_AUDIO_URBS; index++) {
		stream->urbs[index] = drv_usb_urb_alloc(stream->audio->device, stream->data_ep, stream->packet_count);
		if (stream->urbs[index] == NULL)
			return ENOMEM;
	}

	if (stream->fb_ep != NULL) {
		stream->fb_urb = drv_usb_urb_alloc(stream->audio->device, stream->fb_ep, 1U);
		if (stream->fb_urb == NULL)
			return ENOMEM;
	}

	return 0;
}

/* Frees all URBs owned by a stream. */
static void
usb_audio_free_urbs(
	struct usb_audio_stream *stream)
{
	unsigned index;

	for (index = 0U; index < USB_AUDIO_URBS; index++) {
		if (stream->urbs[index] != NULL)
			drv_usb_urb_free(stream->urbs[index]);
		stream->urbs[index] = NULL;
	}

	if (stream->fb_urb != NULL) {
		drv_usb_urb_free(stream->fb_urb);
		stream->fb_urb = NULL;
	}
}

/* Joins the worker thread once stopping is set. */
static int
usb_audio_join_worker(
	struct usb_audio_stream *stream)
{
	struct thread *worker;
	unsigned long irq;
	unsigned state;
	int error;

	worker = stream->worker;
	if (worker == NULL)
		return 0;

	irq = spin_lock_irqsave(&stream->lock);

	waitq_wake_all(&stream->waitq);

	spin_unlock_irqrestore(&stream->lock, irq);

	for (;;) {
		state = atomic_raw_load_acquire((volatile unsigned *)&worker->state);
		if (state == THREAD_ZOMBIE)
			break;
		usb_audio_sleep_ms(1U);
	}

	error = thread_wait(worker, NULL);
	if (error != 0)
		return error;

	irq = spin_lock_irqsave(&stream->lock);

	stream->worker = NULL;

	spin_unlock_irqrestore(&stream->lock, irq);

	return 0;
}

/* Isochronous data URB completion callback: only marks done bit and wakes worker. */
static void
usb_audio_urb_cb(
	struct drv_usb_urb *urb,
	void *argument)
{
	struct usb_audio_stream *stream;
	unsigned long irq;
	unsigned index;

	stream = (struct usb_audio_stream *)argument;
	if (stream == NULL)
		return;

	irq = spin_lock_irqsave(&stream->lock);

	for (index = 0U; index < USB_AUDIO_URBS; index++) {
		if (stream->urbs[index] == urb) {
			stream->done_mask |= (1U << index);
			break;
		}
	}

	waitq_wake_all(&stream->waitq);

	spin_unlock_irqrestore(&stream->lock, irq);
}

/* Feedback IN URB completion callback: only marks fb_done and wakes worker. */
static void
usb_audio_fb_cb(
	struct drv_usb_urb *urb,
	void *argument)
{
	struct usb_audio_stream *stream;
	unsigned long irq;

	(void)urb;
	stream = (struct usb_audio_stream *)argument;
	if (stream == NULL)
		return;

	irq = spin_lock_irqsave(&stream->lock);

	stream->fb_done = 1U;
	waitq_wake_all(&stream->waitq);

	spin_unlock_irqrestore(&stream->lock, irq);
}

/* Worker thread loop: processes completed URBs in ring order and resubmits. */
static void
usb_audio_worker(
	void *argument)
{
	struct usb_audio_stream *stream;
	unsigned long irq;
	uint64_t observed;
	uint64_t deadline;
	unsigned stopping;
	unsigned ready;
	unsigned fb_ready;
	unsigned bit;

	stream = (struct usb_audio_stream *)argument;
	for (;;) {
		bit = 1U << stream->next;
		ready = 0U;
		fb_ready = 0U;

		irq = spin_lock_irqsave(&stream->lock);

		stopping = stream->stopping;
		if ((stream->done_mask & bit) != 0U)
			ready = 1U;
		if (stream->fb_done != 0U)
			fb_ready = 1U;

		if (stopping == 0U && ready == 0U && fb_ready == 0U) {
			observed = waitq_sequence(&stream->waitq);
			deadline = sched_ticks() + kern_ms_to_ticks(USB_AUDIO_WATCHDOG_MS);
			(void)waitq_sleep(&stream->waitq, &stream->lock, observed, deadline, 0U);
			stopping = stream->stopping;
			if ((stream->done_mask & bit) != 0U)
				ready = 1U;
			if (stream->fb_done != 0U)
				fb_ready = 1U;
		}

		if (stopping == 0U && ready != 0U)
			stream->done_mask &= ~bit;
		if (stopping == 0U && fb_ready != 0U)
			stream->fb_done = 0U;

		spin_unlock_irqrestore(&stream->lock, irq);

		if (stopping != 0U)
			return;

		if (fb_ready != 0U) {
			usb_audio_process_fb(stream);
			usb_audio_submit_fb(stream);
		}

		if (ready != 0U) {
			usb_audio_process_urb(stream, stream->next);
			usb_audio_submit_urb(stream, stream->next);
			stream->next = (stream->next + 1U) % USB_AUDIO_URBS;
		}
	}
}

/* Retires a completed data URB and advances ring position for capture IN. */
static void
usb_audio_process_urb(
	struct usb_audio_stream *stream,
	unsigned index)
{
	struct drv_usb_urb *urb;
	unsigned long irq;
	unsigned submitted;
	enum drv_usb_urb_status status;
	enum drv_usb_iso_status pkt_status;
	size_t actual;
	uint32_t total_actual;
	unsigned packet;
	int error;

	submitted = 0U;
	irq = spin_lock_irqsave(&stream->lock);

	if ((stream->submitted_mask & (1U << index)) != 0U)
		submitted = 1U;
	stream->submitted_mask &= ~(1U << index);

	spin_unlock_irqrestore(&stream->lock, irq);

	if (submitted == 0U)
		return;

	urb = stream->urbs[index];
	error = usb_audio_retire_urb(urb);
	if (error != 0)
		return;

	status = drv_usb_urb_status(urb);
	if (status != DRV_USB_URB_COMPLETE) {
		kern_logf("usb-audio: %s: urb %u ended status=%d\n",
		    stream->audio->tag, index, (int)status);
		return;
	}

	if (stream->capture != 0U) {
		total_actual = 0U;
		for (packet = 0U; packet < stream->packet_count; packet++) {
			error = drv_usb_urb_iso_result(urb, packet, &actual, &pkt_status);
			if (error != 0 || actual == 0U)
				continue;
			if (pkt_status == DRV_USB_ISO_OK || pkt_status == DRV_USB_ISO_SHORT)
				total_actual += (uint32_t)actual;
		}
		if (total_actual > 0U)
			usb_audio_advance_hw(stream, total_actual);
	}
}

/* Decodes a completed feedback IN URB and updates the OUT packet rate. */
static void
usb_audio_process_fb(
	struct usb_audio_stream *stream)
{
	unsigned long irq;
	unsigned submitted;
	size_t actual;
	uint32_t q16;
	int error;

	if (stream->fb_urb == NULL)
		return;

	submitted = 0U;
	irq = spin_lock_irqsave(&stream->lock);

	if (stream->fb_submitted != 0U)
		submitted = 1U;
	stream->fb_submitted = 0U;

	spin_unlock_irqrestore(&stream->lock, irq);

	if (submitted == 0U)
		return;

	error = usb_audio_retire_urb(stream->fb_urb);
	if (error != 0)
		return;

	if (drv_usb_urb_status(stream->fb_urb) != DRV_USB_URB_COMPLETE)
		return;

	actual = drv_usb_urb_actual_length(stream->fb_urb);
	if (usb_audio_decode_feedback(stream->fb_buf, actual, stream->fb_nominal_q16, &q16) == 0)
		stream->fb_rate_q16 = q16;
	else
		stream->fb_rate_q16 = stream->fb_nominal_q16;
}

/* Submits one zero-copy data URB pointing directly at the DMA ring. */
static void
usb_audio_submit_urb(
	struct usb_audio_stream *stream,
	unsigned index)
{
	uint32_t pkt_bytes[USB_AUDIO_PACKETS_MAX];
	uint32_t iso_offsets[USB_AUDIO_PACKETS_MAX];
	uint32_t iso_lengths[USB_AUDIO_PACKETS_MAX];
	uint32_t frame_bytes;
	uint32_t frames;
	uint32_t buf_offset;
	uint32_t buf_length;
	uint32_t total_ring_bytes;
	uint32_t next_pos;
	unsigned long irq;
	unsigned admitted;
	unsigned packet;
	int error;

	admitted = 0U;
	irq = spin_lock_irqsave(&stream->lock);

	if (stream->stopping == 0U && stream->audio->disconnected == 0U) {
		stream->submitting++;
		admitted = 1U;
	}

	spin_unlock_irqrestore(&stream->lock, irq);

	if (admitted == 0U)
		return;

	frame_bytes = (uint32_t)stream->active_alt->channels * stream->active_alt->subslot_size;
	for (packet = 0U; packet < stream->packet_count; packet++) {
		if (stream->capture == 0U) {
			if (stream->fb_ep != NULL)
				frames = usb_audio_feedback_next(stream->fb_rate_q16, &stream->fb_accum_q16);
			else
				frames = usb_audio_bresenham_next(&stream->bresenham);

			pkt_bytes[packet] = frames * frame_bytes;
			if (pkt_bytes[packet] > stream->active_alt->ep_max_packet) {
				frames = stream->active_alt->ep_max_packet / frame_bytes;
				pkt_bytes[packet] = frames * frame_bytes;
			}
		} else {
			pkt_bytes[packet] = stream->active_alt->ep_max_packet;
		}
		if (pkt_bytes[packet] == 0U)
			pkt_bytes[packet] = frame_bytes;
	}

	buf_offset = 0U;
	buf_length = 0U;
	total_ring_bytes = 0U;
	next_pos = stream->submit_pos;
	usb_audio_plan_iso_urb(
		stream->submit_pos,
		stream->ring_bytes,
		frame_bytes,
		stream->packet_count,
		pkt_bytes,
		&buf_offset,
		&buf_length,
		iso_offsets,
		iso_lengths,
		&total_ring_bytes,
		&next_pos);

	kern_memset(stream->packets[index], 0, sizeof(stream->packets[index]));
	for (packet = 0U; packet < stream->packet_count; packet++) {
		stream->packets[index][packet].offset = iso_offsets[packet];
		stream->packets[index][packet].length = iso_lengths[packet];
	}

	error = drv_usb_urb_setup(
		stream->urbs[index],
		stream->ring + buf_offset,
		buf_length,
		0U,
		0U,
		usb_audio_urb_cb,
		stream);
	if (error != 0) {
		kern_logf("usb-audio: %s: setup urb %u err=%d off=%u len=%u\n",
		    stream->audio->tag, index, error, buf_offset, buf_length);
	} else {
		error = drv_usb_urb_setup_isochronous(stream->urbs[index], stream->packets[index], stream->packet_count);
		if (error != 0) {
			kern_logf("usb-audio: %s: iso urb %u err=%d pkts=%u len0=%u\n",
			    stream->audio->tag, index, error, stream->packet_count, iso_lengths[0]);
		} else {
			error = drv_usb_urb_submit(stream->urbs[index]);
			if (error != 0) {
				kern_logf("usb-audio: %s: submit urb %u failed err=%d len=%u\n",
				    stream->audio->tag, index, error, buf_length);
			}
		}
	}

	irq = spin_lock_irqsave(&stream->lock);

	if (error == 0) {
		stream->submitted_mask |= (1U << index);
		stream->submit_pos = next_pos;
	}
	stream->submitting--;

	spin_unlock_irqrestore(&stream->lock, irq);

	if (error == 0 && stream->capture == 0U && total_ring_bytes > 0U)
		usb_audio_advance_hw(stream, total_ring_bytes);
}

/* Submits the feedback IN URB for an asynchronous OUT stream. */
static void
usb_audio_submit_fb(
	struct usb_audio_stream *stream)
{
	unsigned long irq;
	unsigned admitted;
	size_t fb_len;
	int error;

	if (stream->fb_urb == NULL)
		return;

	admitted = 0U;
	irq = spin_lock_irqsave(&stream->lock);

	if (stream->stopping == 0U && stream->audio->disconnected == 0U) {
		stream->submitting++;
		admitted = 1U;
	}

	spin_unlock_irqrestore(&stream->lock, irq);

	if (admitted == 0U)
		return;

	fb_len = (stream->audio->speed == DRV_USB_SPEED_FULL) ? 3U : 4U;
	if (fb_len > stream->active_alt->sync_ep_max_packet && stream->active_alt->sync_ep_max_packet > 0U)
		fb_len = stream->active_alt->sync_ep_max_packet;

	stream->packets[0][0].offset = 0U;
	stream->packets[0][0].length = fb_len;

	error = drv_usb_urb_setup(stream->fb_urb, stream->fb_buf, fb_len, DRV_USB_URB_SHORT_OK, 0U, usb_audio_fb_cb, stream);
	if (error == 0)
		error = drv_usb_urb_setup_isochronous(stream->fb_urb, &stream->packets[0][0], 1U);
	if (error == 0)
		error = drv_usb_urb_submit(stream->fb_urb);

	irq = spin_lock_irqsave(&stream->lock);

	if (error == 0)
		stream->fb_submitted = 1U;
	stream->submitting--;

	spin_unlock_irqrestore(&stream->lock, irq);
}

/* Advances hw_pos and notifies the audio framework whenever a fragment completes. */
static void
usb_audio_advance_hw(
	struct usb_audio_stream *stream,
	uint32_t ring_bytes)
{
	unsigned long irq;
	unsigned interrupts;

	if (stream->ring_bytes == 0U || stream->fragment_bytes == 0U)
		return;

	interrupts = 0U;
	irq = spin_lock_irqsave(&stream->lock);

	stream->hw_pos = (stream->hw_pos + ring_bytes) % stream->ring_bytes;
	stream->frag_accum += ring_bytes;
	while (stream->frag_accum >= stream->fragment_bytes) {
		stream->frag_accum -= stream->fragment_bytes;
		interrupts++;
	}

	spin_unlock_irqrestore(&stream->lock, irq);

	while (interrupts > 0U) {
		if (stream->audio->audio_dev != NULL)
			drv_audio_interrupt(stream->audio->audio_dev, (int)stream->capture);
		interrupts--;
	}
}

/* Waits until an URB has retired from the HCD. */
static int
usb_audio_retire_urb(
	struct drv_usb_urb *urb)
{
	unsigned waited;

	for (waited = 0U; waited < USB_AUDIO_DRAIN_MS; waited++) {
		if (drv_usb_urb_retired(urb) != 0)
			return 0;
		usb_audio_sleep_ms(1U);
	}

	return ETIMEDOUT;
}

/* Sleeps for approximately the requested number of milliseconds. */
static void
usb_audio_sleep_ms(
	unsigned milliseconds)
{
	uint64_t ticks;
	uint64_t now;

	ticks = kern_ms_to_ticks(milliseconds);
	if (ticks == 0U)
		ticks = 1U;

	now = sched_ticks();
	sched_sleep(now + ticks);
}
