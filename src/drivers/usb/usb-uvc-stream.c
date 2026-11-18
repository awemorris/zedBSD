/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The uvc driver's stream: the PROBE/COMMIT negotiation, the transfers,
 * the worker and the halt.
 *
 * A mode is asked for with SET_CUR(PROBE) and read back with
 * GET_CUR(PROBE); the answer is corrected and, for SET_FORMAT, sent back
 * with SET_CUR(COMMIT).  Every start commits the saved answer again, so
 * whatever another open's TRY_FORMAT or a stop left on the device, the
 * stream runs in the mode its owner set.
 *
 * An isochronous stream selects the smallest alternate the host takes
 * whose capacity holds the committed payload, and refuses with ENOSPC
 * when there is none: the device sets the pace, and a smaller alternate
 * would only lose data.  It keeps four transfers of 32 packets on the
 * endpoint; a bulk stream keeps one transfer.  The transfers' callbacks
 * only mark their transfer done and wake the worker.  The worker takes
 * the transfers strictly in the order they were submitted, a ring, so a
 * late wake that finds several done never reads a newer one before an
 * older one.  It hands each payload to the assembler and submits the
 * transfer again.
 *
 * The worker also watches the stream.  A frame missing for long is
 * counted as no signal and is not a fault; transfers that stop finishing
 * altogether are, as is a disconnected device.
 *
 * The halt sets stopping, waits out a submission under way, cancels and
 * drains the transfers, joins the worker and selects alternate 0.  It is
 * harmless when nothing runs.  A transfer that does not drain keeps its
 * buffer and is tried again by the next halt.
 */

#include <drivers/camera/cam.h>
#include <drivers/usb/usb-uvc-desc.h>
#include <drivers/usb/usb-uvc-payload.h>
#include <drivers/usb/usb-uvc.h>
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
#include <uapi/time.h>

/* How long a class request may take. */
#define UVC_CONTROL_TIMEOUT_MS		1000U

/* How many times SET_FORMAT tries the PROBE: some devices stall the first one. */
#define UVC_COMMIT_TRIES		3U

/* How long the halt and the worker wait for a transfer to retire. */
#define UVC_DRAIN_MS			1000U

/* How many times a busy SET_INTERFACE is tried, and the wait between. */
#define UVC_ALTERNATE_TRIES		10U
#define UVC_ALTERNATE_STEP_MS		5U

/* How long a refused submission is tried again before the stream fails. */
#define UVC_RESUBMIT_MS			1000U

/* How often the worker looks at the watchdog while nothing finishes. */
#define UVC_WATCHDOG_STEP_MS		250U

/* Transfers that stop finishing for this long fail the stream. */
#define UVC_TRANSFER_SILENCE_NS		(2000000000ULL)

/* The shortest time without a frame that counts as no signal. */
#define UVC_NO_SIGNAL_MIN_NS		(2000000000ULL)

/* The bulk packet size assumed when the endpoint does not say. */
#define UVC_BULK_PACKET_DEFAULT		512U

/* The thread's priority. */
#define UVC_WORKER_PRIORITY		SCHED_PRIORITY_DEFAULT

static int uvc_stream_probe(struct usb_uvc *uvc, uint8_t *record, size_t length, uint8_t format_index, uint8_t frame_index, uint32_t interval_100ns);
static int uvc_stream_control(struct usb_uvc *uvc, uint8_t request, uint8_t selector, uint8_t *record, size_t length);
static int uvc_stream_select(struct usb_uvc *uvc, unsigned setting);
static int uvc_stream_prepare(struct usb_uvc *uvc);
static void uvc_stream_free(struct usb_uvc *uvc);
static int uvc_stream_join(struct usb_uvc *uvc);
static void uvc_stream_completion(struct drv_usb_urb *urb, void *argument);
static void uvc_stream_worker(void *argument);
static void uvc_stream_take(struct usb_uvc *uvc, unsigned index);
static void uvc_stream_take_isochronous(struct usb_uvc *uvc, unsigned index, uint64_t now);
static void uvc_stream_submit(struct usb_uvc *uvc, unsigned index);
static int uvc_stream_setup(struct usb_uvc *uvc, unsigned index);
static void uvc_stream_watch(struct usb_uvc *uvc, uint64_t now);
static void uvc_stream_fail(struct usb_uvc *uvc, int error);
static int uvc_stream_retire(struct drv_usb_urb *urb);
static void uvc_stream_sleep_ms(unsigned milliseconds);
static uint64_t uvc_stream_now_ns(void);

/*
 * Initializes the stream's lock, wait queue and state.
 */
void
uvc_stream_init(
	struct usb_uvc *uvc)
{
	/* The handshake with the callbacks and the worker. */
	spin_init(&uvc->lock, LOCK_RANK_DEVICE, "uvc stream");
	waitq_init(&uvc->waitq, "uvc stream");

	/* Nothing runs and nothing is committed. */
	uvc->running = 0U;
	uvc->committed = 0U;
	uvc->urb_count = 0U;
}

/*
 * Asks the device for a mode with PROBE, and with commit set also
 * commits it.
 *
 * The answer's format and frame must be the ones asked for: a device
 * that answers with another is refused rather than followed.  The
 * corrected answer fills result; a payload larger than the host's
 * largest alternate is ENOSPC, with result filled, before anything is
 * committed.
 */
int
uvc_stream_negotiate(
	struct usb_uvc *uvc,
	const struct drv_cam_mode_request *request,
	struct drv_cam_mode_result *result,
	unsigned commit)
{
	uint8_t record[UVC_PROBE_MAX];
	const struct uvc_format *format;
	const struct uvc_frame *frame;
	uint32_t device_max;
	uint32_t host_max;
	uint32_t interval;
	uint32_t payload;
	uint32_t frame_size;
	unsigned position;
	unsigned attempts;
	unsigned attempt;
	unsigned changed;
	unsigned isochronous;
	size_t length;
	int error;

	/* The format and the exact frame size asked for. */
	format = uvc_desc_find_format(&uvc->stream, request->format);
	if (format == NULL)
		return EINVAL;

	/* The frame of exactly that size. */
	position = 0U;
	frame = uvc_desc_find_frame(format, request->width, request->height, &position);
	if (frame == NULL)
		return EINVAL;

	/* The interval asked for, or the frame's default. */
	interval = request->interval_100ns;
	if (interval == 0U)
		interval = frame->default_interval;

	/* The PROBE exchange; a setting tries it a few times. */
	length = uvc_probe_length(uvc->control.bcd_uvc);
	attempts = 1U;
	if (commit != 0U)
		attempts = UVC_COMMIT_TRIES;
	error = EIO;
	for (attempt = 0U; attempt < attempts; attempt++) {
		error = uvc_stream_probe(uvc, record, length, format->index, frame->index, interval);
		if (error == 0)
			break;
	}

	/* A device that does not answer the PROBE cannot be set. */
	if (error != 0) {
		kern_logf("usb-uvc: %s: PROBE failed (%d)\n", uvc->tag, error);
		return EIO;
	}

	/* The answer must be for the format and the frame asked for. */
	if (record[UVC_PROBE_FORMAT_INDEX] != format->index || record[UVC_PROBE_FRAME_INDEX] != frame->index) {
		kern_logf("usb-uvc: %s: PROBE answered format %u frame %u for %u %u\n", uvc->tag,
		    (unsigned)record[UVC_PROBE_FORMAT_INDEX], (unsigned)record[UVC_PROBE_FRAME_INDEX], (unsigned)format->index,
		    (unsigned)frame->index);
		return EINVAL;
	}

	/* The corrections devices are known to need. */
	isochronous = 0U;
	if (uvc->transport == UVC_TRANSPORT_ISOCHRONOUS)
		isochronous = 1U;
	device_max = uvc_alternate_device_max(uvc->alternates, uvc->alternate_count);
	changed = uvc_probe_fixup(record, format, frame, uvc->control.bcd_uvc, isochronous, uvc->speed, device_max);
	payload = uvc_get32(record, UVC_PROBE_MAX_PAYLOAD);
	frame_size = uvc_get32(record, UVC_PROBE_MAX_FRAME_SIZE);
	if ((changed & (UVC_FIXUP_SIGN_EXTENDED | UVC_FIXUP_CLAMPED | UVC_FIXUP_ESTIMATED)) != 0U)
		kern_logf("usb-uvc: %s: PROBE payload corrected to %u (0x%x)\n", uvc->tag, (unsigned)payload, changed);

	/* The answer as the framework reads it. */
	kern_memset(result, 0, sizeof(*result));
	result->format = format->cam_format;
	result->width = frame->width;
	result->height = frame->height;
	result->interval_100ns = uvc_get32(record, UVC_PROBE_FRAME_INTERVAL);
	result->stride_bytes = uvc_desc_stride(format, frame);
	result->image_bytes = uvc_desc_image_bytes(format, frame, frame_size);
	result->payload_bytes = payload;
	result->mode_index = position;

	/* An isochronous payload no alternate the host takes can carry is refused. */
	if (isochronous != 0U) {
		host_max = uvc_alternate_host_max(uvc->alternates, uvc->alternate_count);
		if (payload > host_max) {
			kern_logf("usb-uvc: %s: no alternate for payload %u (host accepts up to %u)\n", uvc->tag, (unsigned)payload,
			    (unsigned)host_max);
			return ENOSPC;
		}
	}

	/* A trial ends here. */
	if (commit == 0U)
		return 0;

	/* The corrected answer is committed. */
	error = uvc_stream_control(uvc, UVC_SET_CUR, UVC_VS_COMMIT_CONTROL, record, length);
	if (error != 0) {
		kern_logf("usb-uvc: %s: COMMIT failed (%d)\n", uvc->tag, error);
		return EIO;
	}

	/* The committed mode, which every start sends again. */
	kern_memcpy(uvc->commit_record, record, length);
	uvc->commit_length = length;
	uvc->format = format;
	uvc->frame = frame;
	uvc->image_bytes = result->image_bytes;
	uvc->payload_bytes = payload;
	uvc->interval_100ns = result->interval_100ns;
	uvc->committed = 1U;

	/* Succeeded: the device runs this mode at the next start. */
	return 0;
}

/*
 * Starts the committed stream: COMMIT again, the alternate, the
 * transfers and the worker.
 *
 * The worker submits the transfers in order as its first work.  A start
 * that fails leaves nothing running.
 */
int
uvc_stream_start(
	struct usb_uvc *uvc)
{
	unsigned long irq;
	unsigned position;
	unsigned all;
	uint32_t host_max;
	int error;

	/* Only a committed mode starts. */
	if (uvc->committed == 0U)
		return EINVAL;

	/* Transfers a halt could not get back are tried again first. */
	if (uvc->urb_count != 0U) {
		error = uvc_stream_halt(uvc);
		if (error != 0)
			return EBUSY;
	}

	/* The device is put back into the owner's mode. */
	error = uvc_stream_control(uvc, UVC_SET_CUR, UVC_VS_COMMIT_CONTROL, uvc->commit_record, uvc->commit_length);
	if (error != 0) {
		kern_logf("usb-uvc: %s: COMMIT at start failed (%d)\n", uvc->tag, error);
		return EIO;
	}

	/* An isochronous stream: the smallest alternate the host takes that carries the payload. */
	if (uvc->transport == UVC_TRANSPORT_ISOCHRONOUS) {
		position = 0U;
		error = uvc_alternate_choose(uvc->alternates, uvc->alternate_count, uvc->payload_bytes, &position);
		if (error != 0) {
			host_max = uvc_alternate_host_max(uvc->alternates, uvc->alternate_count);
			kern_logf("usb-uvc: %s: no alternate for payload %u (host accepts up to %u)\n", uvc->tag,
			    (unsigned)uvc->payload_bytes, (unsigned)host_max);
			return ENOSPC;
		}

		/* SET_INTERFACE; the host takes the endpoint since only accepted alternates are chosen. */
		error = uvc_stream_select(uvc, uvc->alternates[position].setting);
		if (error != 0)
			return EIO;

		/* The endpoint the alternate enabled and its packets. */
		uvc->endpoint = drv_usb_interface_find_endpoint(uvc->stream_interface, DRV_USB_TRANSFER_ISOCHRONOUS, DRV_USB_DIR_IN, NULL);
		/* A packet is an interval's payload: the transactions an interval times their size. */
		uvc->urb_count = UVC_ISOCH_URBS;
		uvc->packet_bytes = uvc->alternates[position].capacity;
		uvc->packet_count = UVC_ISOCH_PACKETS;
		if ((size_t)uvc->packet_count * uvc->packet_bytes > UVC_ISOCH_TRANSFER_MAX)
			uvc->packet_count = (unsigned)(UVC_ISOCH_TRANSFER_MAX / uvc->packet_bytes);
		uvc->urb_bytes = (size_t)uvc->packet_count * uvc->packet_bytes;
	} else {
		/* A bulk stream: one transfer of the payload, in whole packets, at most 64 KiB. */
		uvc->endpoint = uvc->bulk_endpoint;
		uvc->urb_count = UVC_BULK_URBS;
		uvc->packet_count = 0U;
		uvc->packet_bytes = uvc->bulk_packet_bytes;
		if (uvc->packet_bytes == 0U)
			uvc->packet_bytes = UVC_BULK_PACKET_DEFAULT;
		uvc->urb_bytes = uvc->payload_bytes;
		if (uvc->urb_bytes == 0U || uvc->urb_bytes > UVC_BULK_TRANSFER_MAX)
			uvc->urb_bytes = UVC_BULK_TRANSFER_MAX;
		uvc->urb_bytes = (uvc->urb_bytes + uvc->packet_bytes - 1U) / uvc->packet_bytes * uvc->packet_bytes;
		if (uvc->urb_bytes > UVC_BULK_TRANSFER_MAX)
			uvc->urb_bytes = UVC_BULK_TRANSFER_MAX / uvc->packet_bytes * uvc->packet_bytes;
	}

	/* An alternate without its endpoint cannot stream. */
	if (uvc->endpoint == NULL) {
		kern_logf("usb-uvc: %s: no video endpoint at alternate %u\n", uvc->tag, uvc->alternate_setting);
		uvc->urb_count = 0U;
		(void)uvc_stream_select(uvc, 0U);
		return EIO;
	}

	/* The transfers, their buffers and their reservations. */
	error = uvc_stream_prepare(uvc);
	if (error != 0) {
		uvc_stream_free(uvc);
		(void)uvc_stream_select(uvc, 0U);
		return error;
	}

	/* The frame assembly starts afresh. */
	uvc_assembler_start(&uvc->assembler, uvc->camera, uvc->format->cam_format, uvc->image_bytes, 0U);
	uvc->next = 0U;
	uvc->no_signal_reported = 0U;
	uvc->last_completion_ns = uvc_stream_now_ns();
	uvc->assembler.last_done_ns = uvc->last_completion_ns;

	/*
	 * Every transfer is marked done and not submitted, so the worker's
	 * first round submits them all in ring order.
	 */
	all = (1U << uvc->urb_count) - 1U;
	irq = spin_lock_irqsave(&uvc->lock);

	uvc->stopping = 0U;
	uvc->faulted = 0U;
	uvc->submitting = 0U;
	uvc->done_mask = all;
	uvc->submitted_mask = 0U;

	spin_unlock_irqrestore(&uvc->lock, irq);

	/* The worker. */
	error = kthread_create(uvc_stream_worker, uvc, UVC_WORKER_PRIORITY, &uvc->worker);
	if (error != 0) {
		uvc->worker = NULL;
		uvc_stream_free(uvc);
		(void)uvc_stream_select(uvc, 0U);
		return error;
	}

	/* Running from now on: the halt has something to stop. */
	uvc->running = 1U;
	thread_start(uvc->worker);

	/* Succeeded: the stream runs. */
	kern_logf("usb-uvc: %s: start alternate %u packet %u payload %u, %u transfers of %u bytes\n", uvc->tag,
	    uvc->alternate_setting, (unsigned)uvc->packet_bytes, (unsigned)uvc->payload_bytes, uvc->urb_count,
	    (unsigned)uvc->urb_bytes);
	return 0;
}

/*
 * Stops the stream's transfers and worker and gives the bandwidth back.
 *
 * Harmless when nothing runs.  A transfer the host controller does not
 * give back keeps its buffer for the next halt.
 */
int
uvc_stream_halt(
	struct usb_uvc *uvc)
{
	enum drv_usb_urb_status status;
	unsigned long irq;
	unsigned submitting;
	unsigned index;
	unsigned retained;
	int join_error;
	int error;

	/* Nothing was prepared, so nothing runs. */
	if (uvc->urb_count == 0U)
		return 0;

	/* stopping admits no more submissions and tells the worker to leave. */
	irq = spin_lock_irqsave(&uvc->lock);

	uvc->stopping = 1U;
	waitq_wake_all(&uvc->waitq);

	spin_unlock_irqrestore(&uvc->lock, irq);

	/* A submission admitted just before is waited out, so its transfer is cancelled below. */
	for (;;) {
		irq = spin_lock_irqsave(&uvc->lock);

		submitting = uvc->submitting;

		spin_unlock_irqrestore(&uvc->lock, irq);

		/* None under way: every transfer that will be submitted is. */
		if (submitting == 0U)
			break;
		uvc_stream_sleep_ms(1U);
	}

	/* Each submitted transfer is cancelled; one cancel ends an isochronous endpoint's all. */
	for (index = 0U; index < uvc->urb_count; index++) {
		if (uvc->urbs[index] == NULL)
			continue;
		status = drv_usb_urb_status(uvc->urbs[index]);
		if (status == DRV_USB_URB_PENDING)
			(void)drv_usb_urb_cancel(uvc->urbs[index]);
	}

	/* Each transfer is drained; one that does not retire is retained. */
	retained = 0U;
	for (index = 0U; index < uvc->urb_count; index++) {
		if (uvc->urbs[index] == NULL)
			continue;
		error = drv_usb_urb_drain(uvc->urbs[index], UVC_DRAIN_MS);
		if (error != 0)
			retained++;
	}

	/* The worker leaves once it sees stopping. */
	join_error = uvc_stream_join(uvc);
	if (join_error != 0)
		kern_logf("usb-uvc: %s: the worker was not joined (%d)\n", uvc->tag, join_error);

	/* The bandwidth goes back; a device that is gone may refuse, which is harmless. */
	if (uvc->transport == UVC_TRANSPORT_ISOCHRONOUS)
		(void)uvc_stream_select(uvc, 0U);
	uvc->running = 0U;

	/* Transfers the host controller still holds keep everything for the next halt. */
	if (retained != 0U) {
		uvc->retained = retained;
		uvc->halt_timeouts++;
		kern_logf("usb-uvc: %s: %u transfers did not retire\n", uvc->tag, retained);
		return EBUSY;
	}

	/* The worker must be gone too before the transfers are freed. */
	if (join_error != 0)
		return join_error;

	/* The transfers and their buffers go. */
	uvc->retained = 0U;
	uvc_stream_free(uvc);

	/* Succeeded: nothing of the stream runs. */
	return 0;
}

/*
 * Gives back the slot the stream was filling.  The worker is not
 * running.
 */
void
uvc_stream_abandon(
	struct usb_uvc *uvc)
{
	/* The assembler's slot goes back to the framework's free queue. */
	uvc_assembler_abandon(&uvc->assembler);
}

/*
 * Sends a PROBE request and reads the device's answer back into record.
 * An answer shorter than the record leaves the rest zero.
 */
static int
uvc_stream_probe(
	struct usb_uvc *uvc,
	uint8_t *record,
	size_t length,
	uint8_t format_index,
	uint8_t frame_index,
	uint32_t interval_100ns)
{
	int error;

	/* SET_CUR(PROBE) with the format, the frame and the interval. */
	uvc_probe_request(record, length, format_index, frame_index, interval_100ns);
	error = uvc_stream_control(uvc, UVC_SET_CUR, UVC_VS_PROBE_CONTROL, record, length);
	if (error != 0)
		return error;

	/* GET_CUR(PROBE) into a cleared record. */
	kern_memset(record, 0, length);
	error = uvc_stream_control(uvc, UVC_GET_CUR, UVC_VS_PROBE_CONTROL, record, length);
	if (error != 0)
		return error;

	/* Succeeded: record holds the device's answer. */
	return 0;
}

/*
 * Issues one class request to the streaming interface.
 *
 * Value(s) referenced: USB Video Class 1.1 §4.2 (bmRequestType 0x21 for
 * SET and 0xA1 for GET, wValue the selector in the high byte, wIndex the
 * interface in the low byte).
 */
static int
uvc_stream_control(
	struct usb_uvc *uvc,
	uint8_t request,
	uint8_t selector,
	uint8_t *record,
	size_t length)
{
	uint8_t request_type;
	uint16_t value;
	uint16_t index;
	size_t actual;
	int error;

	/* GET requests read, SET requests write. */
	request_type = DRV_USB_REQUEST_CLASS | DRV_USB_RECIP_INTERFACE;
	if ((request & DRV_USB_DIR_IN) != 0U)
		request_type |= DRV_USB_DIR_IN;

	/* The selector and the interface. */
	value = (uint16_t)((uint16_t)selector << 8);
	index = (uint16_t)drv_usb_interface_number(uvc->stream_interface);

	/* The request on the default control pipe. */
	actual = 0U;
	error = drv_usb_control(uvc->device, request_type, request, value, index, record, length, UVC_CONTROL_TIMEOUT_MS, &actual);
	if (error != 0)
		return error;

	/* Succeeded: a short answer leaves the rest of the record as it was. */
	return 0;
}

/*
 * Selects an alternate of the streaming interface (SET_INTERFACE), tried
 * again while the default control pipe is busy.
 */
static int
uvc_stream_select(
	struct usb_uvc *uvc,
	unsigned setting)
{
	unsigned tries;
	int error;

	/* SET_INTERFACE, while the control pipe is busy tried again. */
	error = EBUSY;
	for (tries = 0U; tries < UVC_ALTERNATE_TRIES; tries++) {
		error = drv_usb_interface_set_alternate(uvc->stream_interface, setting);
		if (error != EBUSY)
			break;
		uvc_stream_sleep_ms(UVC_ALTERNATE_STEP_MS);
	}

	/* An alternate the core did not select. */
	if (error != 0) {
		kern_logf("usb-uvc: %s: alternate %u not selected (%d)\n", uvc->tag, setting, error);
		return error;
	}

	/* Succeeded: the alternate is the interface's. */
	uvc->alternate_setting = setting;
	return 0;
}

/*
 * Allocates the stream's transfers and buffers and reserves each
 * transfer's host controller storage, so a resubmission allocates
 * nothing.
 */
static int
uvc_stream_prepare(
	struct usb_uvc *uvc)
{
	unsigned index;
	int error;

	/* One transfer and one buffer a ring entry. */
	for (index = 0U; index < uvc->urb_count; index++) {
		uvc->urbs[index] = drv_usb_urb_alloc(uvc->device, uvc->endpoint, uvc->packet_count);
		if (uvc->urbs[index] == NULL)
			return ENOMEM;

		/* The buffer the transfer fills. */
		uvc->buffers[index] = kern_malloc(uvc->urb_bytes);
		if (uvc->buffers[index] == NULL)
			return ENOMEM;

		/* The reservation; a host controller without reservations allocates at each submission instead. */
		error = drv_usb_urb_reserve_transfer(uvc->urbs[index], uvc->urb_bytes);
		if (error != 0 &&
		    error != ENOTSUP &&
		    error != EOPNOTSUPP) {
			kern_logf("usb-uvc: %s: transfer reservation of %u bytes failed (%d)\n", uvc->tag, (unsigned)uvc->urb_bytes, error);
			return error;
		}
	}

	/* Succeeded: the ring is ready. */
	return 0;
}

/* Frees the stream's transfers and buffers, none of them submitted. */
static void
uvc_stream_free(
	struct usb_uvc *uvc)
{
	unsigned index;

	/* Each ring entry. */
	for (index = 0U; index < UVC_URBS_MAX; index++) {
		if (uvc->urbs[index] != NULL)
			drv_usb_urb_free(uvc->urbs[index]);
		if (uvc->buffers[index] != NULL)
			kern_free(uvc->buffers[index]);
		uvc->urbs[index] = NULL;
		uvc->buffers[index] = NULL;
	}

	/* No transfer is left. */
	uvc->urb_count = 0U;
}

/* Wakes the worker to leave and waits for it to end. */
static int
uvc_stream_join(
	struct usb_uvc *uvc)
{
	struct thread *worker;
	unsigned long irq;
	unsigned state;
	int error;

	/* No worker was started. */
	worker = uvc->worker;
	if (worker == NULL)
		return 0;

	/* Wakes it; it sees stopping and leaves. */
	irq = spin_lock_irqsave(&uvc->lock);

	waitq_wake_all(&uvc->waitq);

	spin_unlock_irqrestore(&uvc->lock, irq);

	/* Waits asleep until it has ended. */
	for (;;) {
		state = atomic_raw_load_acquire((volatile unsigned *)&worker->state);
		if (state == THREAD_ZOMBIE)
			break;
		uvc_stream_sleep_ms(1U);
	}

	/* Its end is collected. */
	error = thread_wait(worker, NULL);
	if (error != 0)
		return error;

	/* No worker is left. */
	irq = spin_lock_irqsave(&uvc->lock);

	uvc->worker = NULL;

	spin_unlock_irqrestore(&uvc->lock, irq);

	/* Succeeded: the worker is gone. */
	return 0;
}

/*
 * Takes one finished transfer: marks it done and wakes the worker.  It
 * may run under the host controller's command gate, so it does nothing
 * else.
 */
static void
uvc_stream_completion(
	struct drv_usb_urb *urb,
	void *argument)
{
	struct usb_uvc *uvc;
	unsigned long irq;
	unsigned index;
	unsigned bit;

	/* The transfer's bit in the ring. */
	uvc = argument;
	bit = 0U;
	for (index = 0U; index < UVC_URBS_MAX; index++) {
		if (uvc->urbs[index] == urb)
			bit = 1U << index;
	}

	/* Marked done, and the worker woken. */
	irq = spin_lock_irqsave(&uvc->lock);

	uvc->done_mask |= bit;
	waitq_wake_all(&uvc->waitq);

	spin_unlock_irqrestore(&uvc->lock, irq);
}

/*
 * The worker: takes the ring's next transfer once it is done, hands its
 * payloads to the assembler and submits it again, until the halt.
 */
static void
uvc_stream_worker(
	void *argument)
{
	struct usb_uvc *uvc;
	unsigned long irq;
	uint64_t observed;
	uint64_t deadline;
	uint64_t now;
	unsigned stopping;
	unsigned ready;
	unsigned bit;

	/* Until the halt. */
	uvc = argument;
	for (;;) {
		/* Whether the ring's next transfer is done; if not, asleep until a completion or the watchdog's step. */
		bit = 1U << uvc->next;
		ready = 0U;
		irq = spin_lock_irqsave(&uvc->lock);

		stopping = uvc->stopping;
		if ((uvc->done_mask & bit) != 0U)
			ready = 1U;
		if (stopping == 0U && ready == 0U) {
			observed = waitq_sequence(&uvc->waitq);
			deadline = sched_ticks() + kern_ms_to_ticks(UVC_WATCHDOG_STEP_MS);
			(void)waitq_sleep(&uvc->waitq, &uvc->lock, observed, deadline, 0U);
			stopping = uvc->stopping;
			if ((uvc->done_mask & bit) != 0U)
				ready = 1U;
		}

		/* Takes the completion the wait was for. */
		if (stopping == 0U && ready != 0U)
			uvc->done_mask &= ~bit;

		spin_unlock_irqrestore(&uvc->lock, irq);

		/* The halt: the transfers are its to cancel. */
		if (stopping != 0U)
			return;

		/* The watchdog, at every round. */
		now = uvc_stream_now_ns();
		uvc_stream_watch(uvc, now);

		/* Nothing finished in this step. */
		if (ready == 0U)
			continue;

		/* The transfer's payloads, then the transfer submitted again, then the next in the ring. */
		uvc_stream_take(uvc, uvc->next);
		uvc_stream_submit(uvc, uvc->next);
		uvc->next = (uvc->next + 1U) % uvc->urb_count;
	}
}

/*
 * Reads one finished transfer of the ring, when it was submitted: an
 * isochronous one packet by packet, a bulk one as a whole.
 */
static void
uvc_stream_take(
	struct usb_uvc *uvc,
	unsigned index)
{
	enum drv_usb_urb_status status;
	struct drv_usb_urb *urb;
	unsigned long irq;
	unsigned submitted;
	uint64_t now;
	size_t actual;
	int error;

	/* Only a transfer that was submitted has anything to read. */
	submitted = 0U;
	irq = spin_lock_irqsave(&uvc->lock);

	if ((uvc->submitted_mask & (1U << index)) != 0U)
		submitted = 1U;
	uvc->submitted_mask &= ~(1U << index);

	spin_unlock_irqrestore(&uvc->lock, irq);

	/* Not submitted yet: the worker's first round. */
	if (submitted == 0U)
		return;

	/* It retires before its bytes are read. */
	urb = uvc->urbs[index];
	error = uvc_stream_retire(urb);
	if (error != 0) {
		kern_logf("usb-uvc: %s: transfer %u did not retire (%d)\n", uvc->tag, index, error);
		uvc_stream_fail(uvc, EIO);
		return;
	}

	/* A finished transfer shows the controller still serves the endpoint. */
	now = uvc_stream_now_ns();
	uvc->last_completion_ns = now;

	/* How it ended. */
	status = drv_usb_urb_status(urb);
	switch (status) {
	case DRV_USB_URB_COMPLETE:
		/* Its bytes, as payloads. */
		if (uvc->transport == UVC_TRANSPORT_ISOCHRONOUS) {
			uvc_stream_take_isochronous(uvc, index, now);
		} else {
			actual = drv_usb_urb_actual_length(urb);
			uvc_assembler_bulk(&uvc->assembler, uvc->buffers[index], actual, uvc->urb_bytes, uvc->payload_bytes, now);
		}

		break;
	case DRV_USB_URB_CANCELLED:
		/* The halt's cancel; nothing to read. */
		break;
	case DRV_USB_URB_DISCONNECTED:
		/* The device is gone; the detach follows. */
		uvc_stream_fail(uvc, ENODEV);
		break;
	default:
		/* An isochronous transfer ended by the halt's recovery loses its packets; a bulk one fails the stream. */
		if (uvc->transport == UVC_TRANSPORT_ISOCHRONOUS) {
			uvc_assembler_packet_lost(&uvc->assembler, UVC_PACKET_ERROR);
		} else {
			kern_logf("usb-uvc: %s: bulk transfer ended with status %d\n", uvc->tag, (int)status);
			uvc_stream_fail(uvc, EIO);
		}

		break;
	}
}

/*
 * Hands an isochronous transfer's packets to the assembler in order.  A
 * packet with no bytes is an interval in which the device had nothing to
 * send, which is normal, and is not a payload.
 */
static void
uvc_stream_take_isochronous(
	struct usb_uvc *uvc,
	unsigned index,
	uint64_t now)
{
	enum drv_usb_iso_status result;
	struct drv_usb_urb *urb;
	unsigned packet;
	size_t actual;
	size_t offset;
	size_t length;
	int error;

	/* Each packet: a payload, nothing, or a loss. */
	urb = uvc->urbs[index];
	for (packet = 0U; packet < uvc->packet_count; packet++) {
		error = drv_usb_urb_iso_result(urb, packet, &actual, &result);
		if (error != 0)
			continue;

		/* Decides what the packet was. */
		switch (result) {
		case DRV_USB_ISO_OK:
		case DRV_USB_ISO_SHORT:
			/* An empty interval carries no payload. */
			if (actual == 0U)
				break;

			/* The packet's bytes at its place in the buffer. */
			error = drv_usb_urb_iso_packet(urb, packet, &offset, &length);
			if (error != 0)
				break;
			if (actual > length)
				actual = length;
			uvc_assembler_payload(&uvc->assembler, uvc->buffers[index] + offset, actual, now);
			break;
		case DRV_USB_ISO_MISSED:
			uvc_assembler_packet_lost(&uvc->assembler, UVC_PACKET_MISSED);
			break;
		case DRV_USB_ISO_ERROR:
			uvc_assembler_packet_lost(&uvc->assembler, UVC_PACKET_ERROR);
			break;
		default:
			break;
		}
	}
}

/*
 * Submits one transfer of the ring again.
 *
 * A refusal while the endpoint recovers from a halt or the controller
 * prepares to suspend is tried again for up to a second; a disconnected
 * device or a longer refusal fails the stream.
 */
static void
uvc_stream_submit(
	struct usb_uvc *uvc,
	unsigned index)
{
	unsigned long irq;
	unsigned admitted;
	unsigned stopping;
	unsigned waited;
	int error;

	/* Admitted only while the stream runs and has not failed. */
	admitted = 0U;
	irq = spin_lock_irqsave(&uvc->lock);

	if (uvc->stopping == 0U && uvc->faulted == 0U) {
		uvc->submitting++;
		admitted = 1U;
	}

	spin_unlock_irqrestore(&uvc->lock, irq);

	/* A halting or failed stream submits nothing. */
	if (admitted == 0U)
		return;

	/* Set up and submitted, a busy refusal tried again a millisecond later. */
	error = EBUSY;
	for (waited = 0U; waited < UVC_RESUBMIT_MS; waited++) {
		error = uvc_stream_setup(uvc, index);
		if (error == 0)
			error = drv_usb_urb_submit(uvc->urbs[index]);
		if (error != EBUSY && error != EAGAIN)
			break;

		/* The halt does not wait for the retries. */
		irq = spin_lock_irqsave(&uvc->lock);

		stopping = uvc->stopping;

		spin_unlock_irqrestore(&uvc->lock, irq);

		/* A halt that came meanwhile ends the retries. */
		if (stopping != 0U)
			break;
		uvc_stream_sleep_ms(1U);
	}

	/* The submission is no longer under way; a submitted transfer is marked so. */
	irq = spin_lock_irqsave(&uvc->lock);

	if (error == 0)
		uvc->submitted_mask |= 1U << index;
	uvc->submitting--;

	spin_unlock_irqrestore(&uvc->lock, irq);

	/* A transfer submitted is the whole of it. */
	if (error == 0)
		return;

	/* A refusal the halt caused is not a failure. */
	if (error == EBUSY || error == EAGAIN) {
		irq = spin_lock_irqsave(&uvc->lock);

		stopping = uvc->stopping;

		spin_unlock_irqrestore(&uvc->lock, irq);

		/* A stream being halted is not reported as failed. */
		if (stopping != 0U)
			return;
	}

	/* The stream fails: a gone device with ENODEV, anything else with EIO. */
	uvc->resubmit_failures++;
	kern_logf("usb-uvc: %s: transfer %u not submitted (%d)\n", uvc->tag, index, error);
	if (error == ENODEV) {
		uvc_stream_fail(uvc, ENODEV);
	} else {
		uvc_stream_fail(uvc, EIO);
	}
}

/* Sets up one transfer of the ring: packets of the alternate's size, or one bulk read. */
static int
uvc_stream_setup(
	struct usb_uvc *uvc,
	unsigned index)
{
	struct drv_usb_iso_packet *packets;
	unsigned packet;
	int error;

	/* A bulk transfer may end short: the end of a payload. */
	if (uvc->transport != UVC_TRANSPORT_ISOCHRONOUS) {
		error = drv_usb_urb_setup(uvc->urbs[index], uvc->buffers[index], uvc->urb_bytes, DRV_USB_URB_SHORT_OK, 0U,
		    uvc_stream_completion, uvc);
		if (error != 0)
			return error;
		return 0;
	}

	/* The packets, one after another in the buffer. */
	packets = uvc->packets[index];
	kern_memset(packets, 0, sizeof(uvc->packets[index]));
	for (packet = 0U; packet < uvc->packet_count; packet++) {
		packets[packet].offset = packet * uvc->packet_bytes;
		packets[packet].length = uvc->packet_bytes;
	}

	/* The transfer over the whole buffer, without a timeout. */
	error = drv_usb_urb_setup(uvc->urbs[index], uvc->buffers[index], uvc->urb_bytes, 0U, 0U, uvc_stream_completion, uvc);
	if (error != 0)
		return error;

	/* Its packets. */
	error = drv_usb_urb_setup_isochronous(uvc->urbs[index], packets, uvc->packet_count);
	if (error != 0)
		return error;

	/* Succeeded: the transfer can be submitted. */
	return 0;
}

/*
 * Watches the stream.
 *
 * Transfers that stop finishing for two seconds mean the controller or
 * the device is gone, and the stream fails.  A frame missing for
 * max(2 s, 4 frame intervals) is counted once as no signal (a capture
 * device without input, a covered lens) until frames come again.
 */
static void
uvc_stream_watch(
	struct usb_uvc *uvc,
	uint64_t now)
{
	unsigned long irq;
	unsigned faulted;
	uint64_t limit;

	/* A failed stream is no longer watched. */
	irq = spin_lock_irqsave(&uvc->lock);

	faulted = uvc->faulted;

	spin_unlock_irqrestore(&uvc->lock, irq);

	/* A stream that already failed has nothing left to watch. */
	if (faulted != 0U)
		return;

	/* Transfers that no longer finish. */
	if (now > uvc->last_completion_ns && now - uvc->last_completion_ns > UVC_TRANSFER_SILENCE_NS) {
		kern_logf("usb-uvc: %s: no transfer finished for 2 s\n", uvc->tag);
		uvc_stream_fail(uvc, EIO);
		return;
	}

	/* The time a frame may be missing. */
	limit = (uint64_t)uvc->interval_100ns * 100U * 4U;
	if (limit < UVC_NO_SIGNAL_MIN_NS)
		limit = UVC_NO_SIGNAL_MIN_NS;

	/* Frames came again since the last report. */
	if (now <= uvc->assembler.last_done_ns || now - uvc->assembler.last_done_ns <= limit) {
		uvc->no_signal_reported = 0U;
		return;
	}

	/* Already counted. */
	if (uvc->no_signal_reported != 0U)
		return;

	/* Counted once, and the pollers told. */
	uvc->no_signal++;
	uvc->no_signal_reported = 1U;
	drv_cam_source_counters_changed(uvc->camera);
}

/* Fails the stream: no more submissions, and the framework is told. */
static void
uvc_stream_fail(
	struct usb_uvc *uvc,
	int error)
{
	unsigned long irq;
	unsigned already;

	/* faulted stops the submissions and the watchdog. */
	irq = spin_lock_irqsave(&uvc->lock);

	already = uvc->faulted;
	uvc->faulted = 1U;

	spin_unlock_irqrestore(&uvc->lock, irq);

	/* The framework is told once; its owner stops the stream. */
	if (already == 0U)
		drv_cam_source_fault(uvc->camera, error);
}

/*
 * Waits, asleep a millisecond at a time, until a transfer has retired.
 * Returns 0, or ETIMEDOUT after a second.
 */
static int
uvc_stream_retire(
	struct drv_usb_urb *urb)
{
	unsigned waited;
	int retired;

	/* Until retired or the time is up. */
	for (waited = 0U; waited < UVC_DRAIN_MS; waited++) {
		retired = drv_usb_urb_retired(urb);
		if (retired)
			return 0;
		uvc_stream_sleep_ms(1U);
	}

	/* Still the host controller's. */
	return ETIMEDOUT;
}

/* Sleeps the calling thread for about the milliseconds given. */
static void
uvc_stream_sleep_ms(
	unsigned milliseconds)
{
	uint64_t ticks;
	uint64_t now;

	/* At least one tick. */
	ticks = kern_ms_to_ticks(milliseconds);
	if (ticks == 0U)
		ticks = 1U;

	/* The tick count the sleep starts from. */
	now = sched_ticks();

	/* Until the tick count passes the deadline. */
	sched_sleep(now + ticks);
}

/* Reads the monotonic clock in nanoseconds; 0 when it cannot be read. */
static uint64_t
uvc_stream_now_ns(
	void)
{
	struct timespec now;
	int error;

	/* The clock. */
	error = kern_clock_gettime(CLOCK_MONOTONIC, &now);
	if (error != 0)
		return 0U;

	/* Succeeded: the time in nanoseconds. */
	return (uint64_t)now.tv_sec * KERN_NSEC_PER_SEC + (uint64_t)now.tv_nsec;
}
