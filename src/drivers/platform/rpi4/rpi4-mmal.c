/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The firmware's H.264 decoder over MMAL (see rpi4-mmal.h).
 *
 * The protocol: requests carry a waiter number
 * the reply echoes; buffers travel as messages whose payload follows as a
 * bulk transfer; a received buffer's bulk is requested in the order the
 * firmware's messages came, its length rounded up to four bytes (eight
 * bytes for an empty end of stream); an input port that has carried bulks
 * is flushed with PORT_FLUSH and an eight-byte dummy bulk.
 *
 * One VCHIQ service serves every decoder.  VCHIQ's receive thread only
 * queues what arrives (replies, returned buffers, events) and counts bulk
 * completions; the caller's thread does everything else, so the client
 * never sends from the receive thread.
 *
 * The bulk memory (the input staging, the scratch and the output buffers)
 * is ordinary cached RAM below 1 GiB, kept coherent by hand: the VPU does
 * not snoop the ARM caches, so what the CPU wrote is cleaned to RAM before
 * a bulk sends it, and a received range is invalidated after its bulk
 * completes.  The CPU never writes the output buffers or the scratch other
 * than that (a short payload copied in is cleaned at once), so no dirty
 * line can be written back over what a bulk brought.  A frame in cached
 * memory is cheap to read; uncached memory is read one bus transaction at a
 * time.
 */

#include "drivers/platform/rpi4/rpi4-firmware.h"
#include "drivers/platform/rpi4/rpi4-mmal.h"
#include "drivers/platform/rpi4/rpi4-mmal-wire.h"
#include "drivers/platform/rpi4/rpi4-vchiq.h"
#include <drivers/generic/dma.h>
#include <kern/clock.h>
#include <kern/dcache.h>
#include <kern/kcrt.h>
#include <kern/klog.h>
#include <kern/lock.h>
#include <kern/sched.h>
#include <kern/waitq.h>
#include <uapi/errno.h>

/*
 * The decoders one boot may have at once.  One: every decoder shares the
 * one service, whose bulk receives pair with the firmware's sends in the
 * order its messages came, and each decoder's caller issues its own
 * receives; two decoders' callers could cross them.
 */
#define MMAL_DECODERS			1U

/* The arrivals a decoder queues before its caller reads them: every input and output buffer and events. */
#define MMAL_QUEUE			48U

/* The most input and output buffers a decoder gives the firmware. */
#define MMAL_INPUT_BUFFERS		16U
#define MMAL_OUTPUT_BUFFERS		8U

/* The output buffers a decoder gives at least, so the firmware never waits for the caller. */
#define MMAL_OUTPUT_MINIMUM		3U

/*
 * The input staging areas: access units in flight at once, each in an area
 * of its own until the firmware has given its buffers back (ws236 p008).
 */
#define MMAL_STAGING_AREAS		4U

/* The bytes of scratch memory: the dummy bulk and an event's bulk data. */
#define MMAL_SCRATCH_BYTES		4096U

/* How long a request, a bulk, or a free buffer is waited for. */
#define MMAL_WAIT_MS			1000U

/* The VPU's window onto the first 1 GiB of RAM, and the bus address of physical zero in it. */
#define MMAL_BUS_RAM			0xc0000000U
#define MMAL_BUS_RAM_LIMIT		0x3fffffffU

/* The largest piece of DMA memory, which must hold a 1080p frame and a staging area. */
#define MMAL_MAX_SEGMENT		(16U * 1024U * 1024U)

/*
 * The client's names for its buffers and components, which the firmware
 * echoes: a tag in the top byte, the decoder, the kind, and the index.
 */
#define MMAL_TOKEN			0x5a000000U
#define MMAL_TOKEN_MASK			0xff000000U
#define MMAL_KIND_INPUT			1U
#define MMAL_KIND_OUTPUT		2U
#define MMAL_KIND_COMPONENT		3U

/*
 * How many request lines the trace writes: every request and its reply's
 * status word while the budget lasts (the first decoders of a boot), so
 * one boot shows where a decoder's set-up stops.  A refused request is
 * always logged.
 */
#define MMAL_TRACE_LINES		96U

/*
 * How many arrival lines the trace writes: every returned buffer and
 * event, and every bulk receive issued and completed, while the budget
 * lasts, so one boot shows where the buffers and the bulks stop.
 */
#define MMAL_ARRIVAL_LINES		400U

/* The lines a decoder writes about frames whose layout is not their target's, at most. */
#define MMAL_LAYOUT_LINES		4U

/* The firmware component that decodes video. */
#define MMAL_COMPONENT_NAME		"ril.video_decode"

/* The parameters the decoder turns off: they would tie time stamps to input order, not to pictures. */
#define MMAL_PARAMETER_TIMESTAMP_FIFO	0x20024U
#define MMAL_PARAMETER_INTERPOLATE	0x2002fU

/* What a bulk transfer's completion belongs to. */
enum mmal_bulk_kind {
	MMAL_BULK_STAGING,
	MMAL_BULK_FRAME,
	MMAL_BULK_SCRATCH
};

/* One message the receive thread queued, with its length. */
struct mmal_message {
	uint32_t size;
	uint8_t bytes[DRV_RPI4_MMAL_MAX_MESSAGE];
};

/* The completion tag of one kind of bulk of one decoder. */
struct mmal_bulk_tag {
	struct drv_rpi4_mmal_decoder *decoder;
	enum mmal_bulk_kind kind;
	unsigned index;
};

/*
 * One input staging area: its memory, and the input buffers and the bulks
 * of the access unit in it that the firmware has not given back.
 */
struct mmal_staging {
	struct drv_dma_buffer memory;
	uint8_t *cpu;
	uint64_t physical;
	uint32_t buffers;
	uint32_t bulks;
};

/*
 * One output buffer: its bounce memory, and whether the firmware has it,
 * a received frame waits in it, or the caller holds it.  arrival orders
 * the frames waiting: the oldest is received first.  direct says that the
 * frame went to its target instead of the bounce memory.
 */
struct mmal_output {
	struct drv_dma_buffer memory;
	bool with_firmware;
	bool ready;
	bool held;
	bool direct;
	uint64_t arrival;
	uint32_t length;
	uint32_t flags;
	int64_t pts;
	uint32_t planes;
	uint32_t plane_offset[2];
	uint32_t plane_pitch[2];
};

/*
 * One target of a decode's frame (see drv_rpi4_mmal_target).
 *
 * kept says that a transfer into the target's run did not complete, so the
 * VPU may still write it.
 */
struct mmal_target {
	struct drv_rpi4_mmal_target target;
	bool used;
	bool kept;
};

/*
 * One decoder.
 *
 * The decoders live in a static table, so a late bulk completion always
 * finds a valid decoder.  A decoder destroyed broken or with a bulk
 * outstanding closes the service (the firmware aborts the service's bulks
 * and drops its components; no completion of it comes after the close),
 * so its entry is free again; memory a bulk may still have written is
 * kept for good.  The queue and the bulk counts are shared with the
 * receive thread under mmal_lock; everything else belongs to the caller's
 * thread.
 */
struct drv_rpi4_mmal_decoder {
	bool used;
	bool broken;
	unsigned slot;
	uint32_t component;
	uint8_t input_info[DRV_RPI4_MMAL_INFO_BYTES];
	uint8_t output_info[DRV_RPI4_MMAL_INFO_BYTES];
	uint32_t input_handle;
	uint32_t output_handle;
	uint32_t input_size;
	uint32_t input_count;
	uint32_t output_size;
	uint32_t output_count;
	bool input_free[MMAL_INPUT_BUFFERS];
	unsigned input_area[MMAL_INPUT_BUFFERS];
	struct mmal_staging areas[MMAL_STAGING_AREAS];
	unsigned current;
	uint32_t input_bytes;
	struct drv_dma_buffer scratch;
	struct mmal_output outputs[MMAL_OUTPUT_BUFFERS];
	struct mmal_target targets[DRV_RPI4_MMAL_TARGETS];
	unsigned layout_lines;
	uint32_t frames;
	uint64_t arrivals;
	const char *step;
	bool output_enabled;
	bool input_sent;
	uint32_t encoding;
	uint32_t width;
	uint32_t height;
	uint32_t crop_width;
	uint32_t crop_height;
	struct mmal_bulk_tag staging_tags[MMAL_STAGING_AREAS];
	struct mmal_bulk_tag frame_tag;
	struct mmal_bulk_tag scratch_tag;
	bool frame_pending;
	bool scratch_pending;
	int32_t frame_actual;
	struct mmal_message queue[MMAL_QUEUE];
	unsigned queue_head;
	unsigned queue_count;
	bool queue_overflow;
};

/*
 * The decoders.  An entry is taken under mmal_open_lock and returned by
 * destroy; see struct drv_rpi4_mmal_decoder for their lifetime.
 */
static struct drv_rpi4_mmal_decoder mmal_decoders[MMAL_DECODERS];

/*
 * The VCHIQ service, the DMA window and the open's serialization.
 *
 * Opened by the first decoder's creation under mmal_open_lock and kept for
 * the kernel's lifetime; NULL until then.
 */
static struct drv_rpi4_vchiq_service *mmal_service;
static struct drv_dma_device *mmal_dma;
static struct mutex mmal_open_lock;
static bool mmal_ready;

/*
 * Whether the decoders hold the firmware's highest core clock
 * (drv_rpi4_firmware_core_clock's video floor).
 *
 * Taken before the first decoder's component is created and given back
 * when the last decoder goes, so the core clock does not change under a
 * decode however the display's floor moves: the firmware's decoder
 * stopped answering, until a reboot, when the clock was lowered and raised
 * in the middle of one (ws236-p011, T1-685).  Changed under
 * mmal_open_lock.
 */
static bool mmal_floor_held;

/* The trace lines left (see MMAL_TRACE_LINES); written under mmal_request_lock. */
static unsigned mmal_trace_left = MMAL_TRACE_LINES;

/* The arrival trace lines left (see MMAL_ARRIVAL_LINES); written by the one decoder's caller. */
static unsigned mmal_arrival_left = MMAL_ARRIVAL_LINES;

/* The firmware's MMAL worker version (GET_VERSION's major), read once with the service; zero if unknown. */
static uint32_t mmal_major;

/*
 * The one request in flight, its reply and the next waiter number.
 *
 * mmal_request_lock serializes requests; the reply's fields change under
 * mmal_lock.  reply_waiter is zero when no request waits.
 */
static struct mutex mmal_request_lock;
static uint32_t mmal_reply_waiter;
static bool mmal_reply_ready;
static struct mmal_message mmal_reply;
static uint32_t mmal_next_waiter;

/*
 * The lock and wait queue of everything the receive thread hands over:
 * replies, the decoders' queues and their bulk counts.
 */
static struct spinlock mmal_lock;
static struct wait_queue mmal_wake;

/*
 * Whether the locks above have been set up: done once, at the first
 * decoder creation, before anything else of this file runs.
 */
static unsigned mmal_locks_ready;

static void service_message(void *context, const void *data, uint32_t size);
static void service_bulk_done(void *context, void *bulk_context, int32_t actual);
static void service_closed(void *context);
static int service_open(void);
static void locks_init(void);
static void service_reset(void);
static int request(struct drv_rpi4_mmal_decoder *decoder, uint8_t *message, size_t size, uint8_t *reply, size_t capacity);
static int request_with_bulk(struct drv_rpi4_mmal_decoder *decoder, uint8_t *message, size_t size, uint64_t physical, struct mmal_bulk_tag *tag, uint8_t *reply, size_t capacity);
static int configure(struct drv_rpi4_mmal_decoder *decoder, uint32_t width, uint32_t height);
static int port_info(struct drv_rpi4_mmal_decoder *decoder, uint32_t type, uint8_t *info);
static int port_set(struct drv_rpi4_mmal_decoder *decoder, uint8_t *info);
static int port_action(struct drv_rpi4_mmal_decoder *decoder, uint32_t port_handle, uint32_t action, const uint8_t *info);
static int set_boolean(struct drv_rpi4_mmal_decoder *decoder, uint32_t port_handle, uint32_t id, uint32_t value);
static int enable_output(struct drv_rpi4_mmal_decoder *decoder);
static int give_output(struct drv_rpi4_mmal_decoder *decoder, unsigned index);
static int free_input(struct drv_rpi4_mmal_decoder *decoder, uint64_t deadline, unsigned *index);
static int pump(struct drv_rpi4_mmal_decoder *decoder);
static int take_message(struct drv_rpi4_mmal_decoder *decoder, struct mmal_message *message);
static int pump_buffers(struct drv_rpi4_mmal_decoder *decoder);
static int take_buffer(struct drv_rpi4_mmal_decoder *decoder, struct mmal_message *message);
static bool buffer_queued(const struct drv_rpi4_mmal_decoder *decoder);
static void trace_arrival(const struct drv_rpi4_mmal_decoder *decoder, const struct mmal_message *message);
static void report_stall(struct drv_rpi4_mmal_decoder *decoder, const char *what);
static int handle_buffer(struct drv_rpi4_mmal_decoder *decoder, const struct mmal_message *message);
static struct mmal_target *frame_target(struct drv_rpi4_mmal_decoder *decoder, const struct mmal_message *message, uint32_t offset, uint32_t bytes);
static int handle_event(struct drv_rpi4_mmal_decoder *decoder, const struct mmal_message *message);
static int format_changed(struct drv_rpi4_mmal_decoder *decoder, const uint8_t *data);
static int receive_bulk(struct drv_rpi4_mmal_decoder *decoder, uint64_t physical, void *cpu, uint32_t bytes, bool frame);
static int wait_bulk(struct drv_rpi4_mmal_decoder *decoder, bool frame);
static int wait_queue_or(struct drv_rpi4_mmal_decoder *decoder, uint64_t deadline);
static int dma_alloc(size_t bytes, struct drv_dma_buffer *buffer);
static void dma_free(struct drv_dma_buffer *buffer);
static uint64_t bus_to_physical(uint64_t bus);
static uint32_t token(struct drv_rpi4_mmal_decoder *decoder, uint32_t kind, uint32_t index);
static struct drv_rpi4_mmal_decoder *decoder_of(uint32_t value);
static uint32_t maximum(uint32_t first, uint32_t second);
static const char *request_name(uint32_t msgid);
static const char *status_name(uint32_t status);
static void release_memory(struct drv_rpi4_mmal_decoder *decoder);
static bool bulks_outstanding(struct drv_rpi4_mmal_decoder *decoder);
static void core_floor(void);

/* The client's part of the VCHIQ service. */
static const struct drv_rpi4_vchiq_service_ops mmal_service_ops = {
	service_message,
	service_bulk_done,
	service_closed
};

/*
 * Creates a decoder for H.264 pictures of up to width by height samples.
 */
int
drv_rpi4_mmal_decoder_create(
	uint32_t width,
	uint32_t height,
	uint32_t input_bytes,
	uint32_t frames,
	struct drv_rpi4_mmal_decoder **result)
{
	struct drv_rpi4_mmal_decoder *decoder;
	uint8_t message[DRV_RPI4_MMAL_MAX_MESSAGE];
	uint8_t reply[DRV_RPI4_MMAL_MAX_MESSAGE];
	unsigned index;
	size_t size;
	uint32_t status;
	int error;

	/* The caller receives nothing on failure. */
	*result = NULL;
	if (width == 0 || height == 0 || width > 4096U || height > 4096U)
		return EINVAL;
	if (input_bytes < 4096U || input_bytes > MMAL_MAX_SEGMENT - 64U)
		return EINVAL;
	if (frames > MMAL_OUTPUT_BUFFERS)
		return EINVAL;

	/* Opens the service the first time and takes a free decoder entry. */
	locks_init();
	mutex_lock(&mmal_open_lock);

	error = service_open();
	decoder = NULL;
	if (error == 0) {
		for (index = 0; index < MMAL_DECODERS; index++) {
			if (!mmal_decoders[index].used) {
				decoder = &mmal_decoders[index];
				kern_memset(decoder, 0, sizeof(*decoder));
				decoder->used = true;
				decoder->slot = index;
				break;
			}
		}

		/* Every entry is in use. */
		if (decoder == NULL)
			error = EBUSY;
	}

	/* The highest core clock, before the component decodes anything. */
	core_floor();

	mutex_unlock(&mmal_open_lock);

	if (error != 0)
		return error;

	/* The tags the bulk completions name: one for each staging area, the frame and the scratch. */
	for (index = 0; index < MMAL_STAGING_AREAS; index++) {
		decoder->staging_tags[index].decoder = decoder;
		decoder->staging_tags[index].kind = MMAL_BULK_STAGING;
		decoder->staging_tags[index].index = index;
	}

	/* The frame's and the scratch's. */
	decoder->frame_tag.decoder = decoder;
	decoder->frame_tag.kind = MMAL_BULK_FRAME;
	decoder->scratch_tag.decoder = decoder;
	decoder->scratch_tag.kind = MMAL_BULK_SCRATCH;

	/* The output buffers the caller asks for at least: the frames it keeps and those in flight. */
	decoder->frames = frames;

	/* The input staging areas (each with room to round a bulk up to four bytes) and the scratch. */
	decoder->input_bytes = input_bytes;
	error = 0;
	for (index = 0; index < MMAL_STAGING_AREAS && error == 0; index++)
		error = dma_alloc((size_t)input_bytes + 64U, &decoder->areas[index].memory);
	if (error == 0)
		error = dma_alloc(MMAL_SCRATCH_BYTES, &decoder->scratch);
	if (error != 0) {
		release_memory(decoder);
		mutex_lock(&mmal_open_lock);

		decoder->used = false;
		core_floor();

		mutex_unlock(&mmal_open_lock);
		return error;
	}

	/* Each area's CPU and physical addresses. */
	for (index = 0; index < MMAL_STAGING_AREAS; index++) {
		decoder->areas[index].cpu = decoder->areas[index].memory.address;
		decoder->areas[index].physical = bus_to_physical(decoder->areas[index].memory.device_address);
	}

	/* Creates the firmware's decoder component. */
	size = drv_rpi4_mmal_build_create(message, 0, token(decoder, MMAL_KIND_COMPONENT, 0), MMAL_COMPONENT_NAME);
	error = request(decoder, message, size, reply, sizeof(reply));
	if (error == 0) {
		status = drv_rpi4_mmal_get32(reply, DRV_RPI4_MMAL_CREATE_REPLY_STATUS);
		if (status != 0)
			error = EIO;
	}

	/* Reports a component the firmware did not create. */
	if (error != 0) {
		kern_logf("rpi4-mmal: %s not created (%d)\n", MMAL_COMPONENT_NAME, error);
		drv_rpi4_mmal_decoder_destroy(decoder);
		return error;
	}

	/* The firmware's handle of the component, named in every later request. */
	decoder->component = drv_rpi4_mmal_get32(reply, DRV_RPI4_MMAL_CREATE_REPLY_HANDLE);

	/* Sets the ports up and starts the component. */
	kern_logf("rpi4-mmal: %s created: handle %u, inputs %u, outputs %u\n", MMAL_COMPONENT_NAME,
		  (unsigned)decoder->component,
		  (unsigned)drv_rpi4_mmal_get32(reply, DRV_RPI4_MMAL_CREATE_REPLY_INPUTS),
		  (unsigned)drv_rpi4_mmal_get32(reply, DRV_RPI4_MMAL_CREATE_REPLY_OUTPUTS));
	error = configure(decoder, width, height);
	if (error != 0) {
		kern_logf("rpi4-mmal: decoder not configured (%d) at step %s\n", error, decoder->step);
		drv_rpi4_mmal_decoder_destroy(decoder);
		return error;
	}

	/* Succeeded: the decoder takes access units. */
	*result = decoder;
	return 0;
}

/*
 * Reports the input staging the next access unit goes into.
 *
 * The staging goes round the areas; an area is reused only after the
 * firmware has given back every input buffer and completed every bulk of
 * the access unit last sent from it.  A timeout of zero only looks.
 */
int
drv_rpi4_mmal_decoder_input(
	struct drv_rpi4_mmal_decoder *decoder,
	unsigned timeout_ms,
	uint8_t **staging,
	uint32_t *capacity)
{
	struct mmal_staging *area;
	unsigned long enabled;
	uint64_t deadline;
	uint64_t observed;
	bool busy;
	int error;

	/* A broken decoder takes nothing more. */
	if (decoder->broken)
		return EIO;
	area = &decoder->areas[decoder->current];

	/*
	 * Waits until the firmware is done with the area.  The wait's
	 * sequence is read in the same hold of the lock as the area's counts,
	 * so a completion between the two is not missed.
	 */
	deadline = sched_ticks() + kern_ms_to_ticks(timeout_ms) + 1U;
	for (;;) {
		error = pump(decoder);
		if (error != 0)
			return error;
		enabled = spin_lock_irqsave(&mmal_lock);

		busy = false;
		if (area->buffers != 0 || area->bulks != 0)
			busy = true;
		error = 0;
		if (busy && timeout_ms == 0) {
			/* Only looking: the area is not free yet. */
			error = ETIMEDOUT;
		} else if (busy && decoder->queue_count == 0 && !decoder->queue_overflow) {
			observed = waitq_sequence(&mmal_wake);
			error = waitq_sleep(&mmal_wake, &mmal_lock, observed, deadline, 0);
			if (error != ETIMEDOUT)
				error = 0;
		}

		spin_unlock_irqrestore(&mmal_lock, enabled);

		if (!busy)
			break;
		if (error != 0 && timeout_ms == 0)
			return error;
		if (error != 0) {
			kern_logf("rpi4-mmal: input staging not given back in %u ms\n", timeout_ms);
			return error;
		}
	}

	/* Succeeded: the caller writes the access unit here. */
	*staging = area->cpu;
	*capacity = decoder->input_bytes;
	return 0;
}

/*
 * Sends the access unit (or, with config, the parameter sets) written into
 * the staging the last input call gave.
 */
int
drv_rpi4_mmal_decoder_send(
	struct drv_rpi4_mmal_decoder *decoder,
	uint32_t bytes,
	int64_t pts,
	int config)
{
	struct mmal_staging *area;
	uint8_t message[DRV_RPI4_MMAL_BUFFER_BYTES];
	unsigned long enabled;
	uint64_t deadline;
	uint32_t offset;
	uint32_t chunk;
	uint32_t flags;
	unsigned index;
	unsigned area_index;
	int error;

	/* Refuses an empty or oversized unit and a broken decoder. */
	if (decoder->broken)
		return EIO;
	if (bytes == 0 || bytes > decoder->input_bytes)
		return EINVAL;
	area_index = decoder->current;
	area = &decoder->areas[area_index];

	/* The bytes the caller wrote reach RAM, where the VPU reads them, before any bulk of them goes. */
	kern_dcache_clean_range(area->cpu, (bytes + 3U) & ~3U);

	/* Sends the unit in pieces of the input buffer size; the last ends the frame. */
	deadline = sched_ticks() + kern_ms_to_ticks(MMAL_WAIT_MS) + 1U;
	offset = 0;
	while (offset < bytes) {
		/* Waits for an input buffer the firmware has given back. */
		error = free_input(decoder, deadline, &index);
		if (error != 0)
			return error;

		/*
		 * The piece and its flags, as the BSD-3 userland's container
		 * reader gives them: the start of the frame on the first, its end
		 * on the last; codec configuration instead for parameter sets.
		 */
		chunk = bytes - offset;
		if (chunk > decoder->input_size)
			chunk = decoder->input_size;
		flags = 0;
		if (config) {
			flags = DRV_RPI4_MMAL_FLAG_CONFIG;
		} else {
			if (offset == 0)
				flags |= DRV_RPI4_MMAL_FLAG_FRAME_START;
			if (offset + chunk == bytes)
				flags |= DRV_RPI4_MMAL_FLAG_FRAME_END;
		}

		/* The buffer is the firmware's until it comes back; the area until its bulk completes. */
		decoder->input_free[index] = false;
		decoder->input_area[index] = area_index;
		enabled = spin_lock_irqsave(&mmal_lock);

		area->buffers++;
		area->bulks++;

		spin_unlock_irqrestore(&mmal_lock, enabled);

		/* The buffer message, then the piece as a bulk of a multiple of four bytes. */
		(void)drv_rpi4_mmal_build_buffer(message, DRV_RPI4_MMAL_MSG_BUFFER_FROM_HOST, 0, decoder->component,
						 decoder->input_handle, token(decoder, MMAL_KIND_INPUT, index),
						 decoder->input_size, chunk, flags, pts);
		error = drv_rpi4_vchiq_send_bulk(mmal_service, message, sizeof(message), DRV_RPI4_VCHIQ_BULK_TRANSMIT,
						 area->physical + offset, (chunk + 3U) & ~3U, &decoder->staging_tags[area_index]);
		if (error != 0) {
			decoder->broken = true;
			kern_logf("rpi4-mmal: input not sent (%d); decoder stopped\n", error);
			return error;
		}

		/* The next piece; the input port has now carried data. */
		offset += chunk;
		decoder->input_sent = true;
	}

	/* The next unit goes into the next area. */
	decoder->current = (area_index + 1U) % MMAL_STAGING_AREAS;

	/* Succeeded: the firmware has the unit. */
	return 0;
}

/*
 * Sends an end of stream: an empty input buffer with the end-of-stream
 * flag and no bulk.
 */
int
drv_rpi4_mmal_decoder_end(
	struct drv_rpi4_mmal_decoder *decoder)
{
	uint8_t message[DRV_RPI4_MMAL_BUFFER_BYTES];
	unsigned long enabled;
	uint64_t deadline;
	unsigned index;
	int error;

	/* A broken decoder takes nothing more. */
	if (decoder->broken)
		return EIO;

	/* An input buffer the firmware has given back. */
	deadline = sched_ticks() + kern_ms_to_ticks(MMAL_WAIT_MS) + 1U;
	error = free_input(decoder, deadline, &index);
	if (error != 0)
		return error;

	/* The buffer counts against the current area until it comes back; it moves no bytes. */
	decoder->input_free[index] = false;
	decoder->input_area[index] = decoder->current;
	enabled = spin_lock_irqsave(&mmal_lock);

	decoder->areas[decoder->current].buffers++;

	spin_unlock_irqrestore(&mmal_lock, enabled);

	/* The empty buffer message. */
	(void)drv_rpi4_mmal_build_buffer(message, DRV_RPI4_MMAL_MSG_BUFFER_FROM_HOST, 0, decoder->component,
					 decoder->input_handle, token(decoder, MMAL_KIND_INPUT, index),
					 decoder->input_size, 0, DRV_RPI4_MMAL_FLAG_EOS, DRV_RPI4_MMAL_TIME_UNKNOWN);
	error = drv_rpi4_vchiq_send(mmal_service, message, sizeof(message));
	if (error != 0) {
		decoder->broken = true;
		kern_logf("rpi4-mmal: empty buffer not sent (%d); decoder stopped\n", error);
		return error;
	}

	/* Succeeded: the firmware outputs what it holds. */
	return 0;
}

/* Waits until deadline for an input buffer the firmware has given back; index receives it. */
static int
free_input(
	struct drv_rpi4_mmal_decoder *decoder,
	uint64_t deadline,
	unsigned *index)
{
	int error;

	/* Takes in what came back, then looks for a free buffer, until the deadline. */
	for (;;) {
		error = pump(decoder);
		if (error != 0)
			return error;
		for (*index = 0; *index < decoder->input_count; (*index)++) {
			if (decoder->input_free[*index])
				return 0;
		}

		/* Waits for something to come back. */
		error = wait_queue_or(decoder, deadline);
		if (error != 0) {
			kern_logf("rpi4-mmal: no input buffer back in %u ms\n", MMAL_WAIT_MS);
			return error;
		}
	}
}

/*
 * Receives the oldest decoded frame not received yet.
 *
 * The frame's buffer is the caller's until it releases the frame's handle.
 */
int
drv_rpi4_mmal_decoder_receive(
	struct drv_rpi4_mmal_decoder *decoder,
	unsigned timeout_ms,
	struct drv_rpi4_mmal_frame *frame,
	const uint8_t **pixels)
{
	struct mmal_output *output;
	uint64_t deadline;
	unsigned oldest;
	unsigned index;
	int error;

	/* A broken decoder gives nothing more. */
	if (decoder->broken)
		return EIO;

	/* Waits for a received frame; of several, the one that came first. */
	deadline = sched_ticks() + kern_ms_to_ticks(timeout_ms) + 1U;
	for (;;) {
		error = pump(decoder);
		if (error != 0)
			return error;

		/* The ready frame of the earliest arrival. */
		oldest = decoder->output_count;
		for (index = 0; index < decoder->output_count; index++) {
			if (!decoder->outputs[index].ready)
				continue;
			if (oldest == decoder->output_count || decoder->outputs[index].arrival < decoder->outputs[oldest].arrival)
				oldest = index;
		}

		/* A received frame is ready. */
		if (oldest < decoder->output_count)
			break;

		/* Only looking: none is ready. */
		if (timeout_ms == 0)
			return ETIMEDOUT;

		/* Waits for an arrival. */
		error = wait_queue_or(decoder, deadline);
		if (error != 0)
			return error;
	}

	/* The frame stays in its buffer until the caller releases it. */
	index = oldest;
	output = &decoder->outputs[index];
	output->ready = false;
	output->held = true;

	/* Describes the frame. */
	kern_memset(frame, 0, sizeof(*frame));
	frame->direct = output->direct;
	frame->handle = index + 1U;
	frame->pts = output->pts;
	frame->flags = output->flags;
	frame->length = output->length;
	frame->encoding = decoder->encoding;
	frame->width = decoder->width;
	frame->height = decoder->height;
	frame->crop_width = decoder->crop_width;
	frame->crop_height = decoder->crop_height;
	frame->planes = output->planes;
	frame->plane_offset[0] = output->plane_offset[0];
	frame->plane_offset[1] = output->plane_offset[1];
	frame->plane_pitch[0] = output->plane_pitch[0];
	frame->plane_pitch[1] = output->plane_pitch[1];
	*pixels = output->memory.address;

	/* Succeeded: the frame is the caller's until it releases it. */
	return 0;
}

/*
 * Sends the frame of one decode to a target instead of the bounce memory.
 */
int
drv_rpi4_mmal_decoder_target(
	struct drv_rpi4_mmal_decoder *decoder,
	const struct drv_rpi4_mmal_target *target)
{
	unsigned index;

	/* The run must lie in the VPU's window, and a frame needs both planes in it. */
	if (target->physical > MMAL_BUS_RAM_LIMIT || target->bytes > MMAL_BUS_RAM_LIMIT + 1U - target->physical)
		return EINVAL;
	if (target->pitch == 0 || target->chroma >= target->bytes)
		return EINVAL;

	/* A free entry takes the target. */
	for (index = 0; index < DRV_RPI4_MMAL_TARGETS; index++) {
		if (!decoder->targets[index].used) {
			decoder->targets[index].target = *target;
			decoder->targets[index].used = true;
			decoder->targets[index].kept = false;
			return 0;
		}
	}

	/* Every entry has a target. */
	return EBUSY;
}

/*
 * Removes the target of one decode.
 */
void
drv_rpi4_mmal_decoder_untarget(
	struct drv_rpi4_mmal_decoder *decoder,
	int64_t pts,
	bool *kept)
{
	unsigned index;

	/* Every entry of the time stamp goes; one whose transfer did not complete is reported. */
	*kept = false;
	for (index = 0; index < DRV_RPI4_MMAL_TARGETS; index++) {
		if (!decoder->targets[index].used || decoder->targets[index].target.pts != pts)
			continue;
		if (decoder->targets[index].kept)
			*kept = true;
		decoder->targets[index].used = false;
	}
}

/*
 * Gives a received frame's buffer back to the firmware.
 */
int
drv_rpi4_mmal_decoder_release(
	struct drv_rpi4_mmal_decoder *decoder,
	unsigned handle)
{
	struct mmal_output *output;
	int error;

	/* Only a frame the caller holds has a buffer to give back. */
	if (handle == 0 || handle > decoder->output_count)
		return EINVAL;
	output = &decoder->outputs[handle - 1U];
	if (!output->held)
		return EINVAL;

	/* The buffer is the firmware's again (a disabled port gets it when enabled). */
	output->held = false;
	if (decoder->broken)
		return EIO;
	error = give_output(decoder, handle - 1U);
	if (error != 0)
		return error;

	/* Succeeded: the frame's memory is no longer the caller's. */
	return 0;
}

/*
 * Drops every picture and reference the decoder holds.
 *
 * The input port has carried bulks, so it is flushed with PORT_FLUSH and a
 * dummy bulk that keeps the flush in order behind them; the output port
 * with a plain flush.  Every buffer the firmware gives back goes back to it
 * (output) or is free again (input).
 */
int
drv_rpi4_mmal_decoder_flush(
	struct drv_rpi4_mmal_decoder *decoder)
{
	uint8_t message[DRV_RPI4_MMAL_MAX_MESSAGE];
	uint8_t reply[DRV_RPI4_MMAL_MAX_MESSAGE];
	unsigned long enabled;
	unsigned index;
	size_t size;
	uint32_t status;
	int error;

	/* A broken decoder cannot be flushed. */
	if (decoder->broken)
		return EIO;

	/*
	 * The input port: after data went in (and on a firmware that has it,
	 * version 15 on), PORT_FLUSH with the dummy bulk that keeps it behind
	 * the data's bulks; else the plain flush.
	 */
	if (decoder->input_sent && mmal_major >= 15U) {
		size = drv_rpi4_mmal_build_buffer(message, DRV_RPI4_MMAL_MSG_PORT_FLUSH, 0, decoder->component,
						  decoder->input_handle, token(decoder, MMAL_KIND_INPUT, 0xfffU), 0, 0, 0,
						  DRV_RPI4_MMAL_TIME_UNKNOWN);
		enabled = spin_lock_irqsave(&mmal_lock);

		decoder->scratch_pending = true;

		spin_unlock_irqrestore(&mmal_lock, enabled);

		error = request_with_bulk(decoder, message, size, bus_to_physical(decoder->scratch.device_address), &decoder->scratch_tag,
					  reply, sizeof(reply));
		if (error == 0) {
			error = wait_bulk(decoder, false);
		} else {
			enabled = spin_lock_irqsave(&mmal_lock);

			decoder->scratch_pending = false;

			spin_unlock_irqrestore(&mmal_lock, enabled);
		}

		/* The flush's status. */
		if (error == 0) {
			status = drv_rpi4_mmal_get32(reply, DRV_RPI4_MMAL_REPLY_STATUS);
			if (status != 0)
				error = EIO;
		}
	} else {
		error = port_action(decoder, decoder->input_handle, DRV_RPI4_MMAL_ACTION_FLUSH, NULL);
	}

	/* Reports an input port that was not flushed. */
	if (error != 0)
		return error;

	/* The output port. */
	error = port_action(decoder, decoder->output_handle, DRV_RPI4_MMAL_ACTION_FLUSH, NULL);
	if (error != 0)
		return error;

	/* Frames received before the flush are dropped and their buffers go back. */
	for (index = 0; index < decoder->output_count; index++) {
		if (decoder->outputs[index].ready || decoder->outputs[index].held) {
			decoder->outputs[index].ready = false;
			decoder->outputs[index].held = false;
			error = give_output(decoder, index);
			if (error != 0)
				return error;
		}
	}

	/* Takes in what the flush gave back. */
	error = pump(decoder);
	if (error != 0)
		return error;

	/* Succeeded: the decoder starts again at the next key frame. */
	return 0;
}

/*
 * Destroys a decoder.
 */
void
drv_rpi4_mmal_decoder_destroy(
	struct drv_rpi4_mmal_decoder *decoder)
{
	uint8_t message[DRV_RPI4_MMAL_MAX_MESSAGE];
	uint8_t reply[DRV_RPI4_MMAL_MAX_MESSAGE];
	bool outstanding;
	size_t size;

	/* Stops the ports and the component, and destroys it, unless the link is broken. */
	if (!decoder->broken && decoder->component != 0) {
		decoder->output_enabled = false;
		(void)port_action(decoder, decoder->input_handle, DRV_RPI4_MMAL_ACTION_DISABLE, NULL);
		(void)port_action(decoder, decoder->output_handle, DRV_RPI4_MMAL_ACTION_DISABLE, NULL);
		(void)pump(decoder);
		size = drv_rpi4_mmal_build_component(message, DRV_RPI4_MMAL_MSG_COMPONENT_DISABLE, 0, decoder->component);
		(void)request(decoder, message, size, reply, sizeof(reply));
		size = drv_rpi4_mmal_build_component(message, DRV_RPI4_MMAL_MSG_COMPONENT_DESTROY, 0, decoder->component);
		(void)request(decoder, message, size, reply, sizeof(reply));
		(void)pump(decoder);
	}

	/*
	 * A broken decoder, or one with a bulk outstanding, closes the
	 * service: the firmware aborts its bulks and drops its component, and
	 * the next decoder opens the service again.  Memory a bulk still
	 * holds after that is kept for good (it is never freed: its handles
	 * go with the entry); the entry is free again either way.
	 */
	outstanding = bulks_outstanding(decoder);
	mutex_lock(&mmal_open_lock);

	if (decoder->broken || outstanding) {
		if (outstanding)
			report_stall(decoder, "destroy");
		kern_logf("rpi4-mmal: decoder %u destroyed %s; the service is closed and opened again\n",
			  decoder->slot, outstanding ? "with a bulk outstanding" : "broken");
		service_reset();
		outstanding = bulks_outstanding(decoder);
	}

	/* Keeps memory a bulk may still write and frees the rest. */
	if (outstanding) {
		kern_logf("rpi4-mmal: decoder %u memory kept for good: a bulk did not complete or abort\n", decoder->slot);
	} else {
		release_memory(decoder);
	}

	/* The entry is free for the next decoder; the last one gives the core clock back. */
	decoder->used = false;
	core_floor();

	mutex_unlock(&mmal_open_lock);
}

/*
 * Takes what the receive thread hands over: a reply wakes its request, a
 * buffer or an event joins its decoder's queue.
 */
static void
service_message(
	void *context,
	const void *data,
	uint32_t size)
{
	struct drv_rpi4_mmal_decoder *decoder;
	struct mmal_message *slot;
	const uint8_t *bytes;
	unsigned long enabled;
	uint32_t magic;
	uint32_t msgid;
	uint32_t owner;
	unsigned index;

	/* Ignores anything that is not an MMAL message. */
	(void)context;
	bytes = data;
	if (size < DRV_RPI4_MMAL_HEADER_BYTES)
		return;
	magic = drv_rpi4_mmal_get32(bytes, DRV_RPI4_MMAL_HEADER_MAGIC);
	if (magic != DRV_RPI4_MMAL_MAGIC)
		return;
	if (size > DRV_RPI4_MMAL_MAX_MESSAGE)
		size = DRV_RPI4_MMAL_MAX_MESSAGE;
	msgid = drv_rpi4_mmal_get32(bytes, DRV_RPI4_MMAL_HEADER_MSGID);

	/* Buffers name their decoder in the driver area, events in the client field. */
	owner = 0;
	if (msgid == DRV_RPI4_MMAL_MSG_BUFFER_TO_HOST && size >= DRV_RPI4_MMAL_BUFFER_CONTEXT + 4U)
		owner = drv_rpi4_mmal_get32(bytes, DRV_RPI4_MMAL_BUFFER_CONTEXT);
	if (msgid == DRV_RPI4_MMAL_MSG_EVENT_TO_HOST && size >= DRV_RPI4_MMAL_EVENT_CLIENT + 4U)
		owner = drv_rpi4_mmal_get32(bytes, DRV_RPI4_MMAL_EVENT_CLIENT);
	decoder = decoder_of(owner);
	enabled = spin_lock_irqsave(&mmal_lock);

	if (decoder != NULL) {
		/* Queues the arrival for the decoder's caller. */
		if (decoder->queue_count < MMAL_QUEUE) {
			index = (decoder->queue_head + decoder->queue_count) % MMAL_QUEUE;
			slot = &decoder->queue[index];
			slot->size = size;
			kern_memcpy(slot->bytes, bytes, size);
			decoder->queue_count++;
		} else {
			decoder->queue_overflow = true;
		}
	} else if (mmal_reply_waiter != 0 &&
		   drv_rpi4_mmal_get32(bytes, DRV_RPI4_MMAL_HEADER_WAITER) == mmal_reply_waiter) {
		/* The reply to the request in flight. */
		mmal_reply.size = size;
		kern_memcpy(mmal_reply.bytes, bytes, size);
		mmal_reply_ready = true;
	}

	/* Wakes whoever waits for a reply or an arrival. */
	waitq_wake_all(&mmal_wake);

	spin_unlock_irqrestore(&mmal_lock, enabled);
}

/* Counts one completed bulk for its decoder. */
static void
service_bulk_done(
	void *context,
	void *bulk_context,
	int32_t actual)
{
	struct mmal_bulk_tag *tag;
	struct drv_rpi4_mmal_decoder *decoder;
	unsigned long enabled;

	/* The tag names the decoder and what the bulk was for. */
	(void)context;
	tag = bulk_context;
	if (tag == NULL)
		return;
	decoder = tag->decoder;
	enabled = spin_lock_irqsave(&mmal_lock);

	if (tag->kind == MMAL_BULK_STAGING) {
		decoder->areas[tag->index].bulks--;
	} else if (tag->kind == MMAL_BULK_FRAME) {
		decoder->frame_pending = false;
		decoder->frame_actual = actual;
	} else {
		decoder->scratch_pending = false;
	}

	/* Wakes whoever waits for the completion. */
	waitq_wake_all(&mmal_wake);

	spin_unlock_irqrestore(&mmal_lock, enabled);
}

/* Notes that the firmware closed the service: nothing more can be sent. */
static void
service_closed(
	void *context)
{
	/* Every later request and buffer fails on the closed service. */
	(void)context;
	kern_logf("rpi4-mmal: the firmware closed the MMAL service\n");
}

/*
 * Opens the MMAL service and the DMA window once.
 *
 * The caller holds mmal_open_lock.
 */
static int
service_open(void)
{
	struct drv_dma_constraints constraints;
	uint8_t message[DRV_RPI4_MMAL_MAX_MESSAGE];
	uint8_t reply[DRV_RPI4_MMAL_MAX_MESSAGE];
	int error;

	/* Done once. */
	if (mmal_ready)
		return 0;

	/*
	 * The window decoders allocate their bulk memory in.  It is marked
	 * coherent so the CPU reaches it through the cached direct map; the
	 * client keeps it coherent with the VPU by hand (see the top of the
	 * file).
	 */
	if (mmal_dma == NULL) {
		kern_memset(&constraints, 0, sizeof(constraints));
		constraints.address_bits = 32U;
		constraints.max_segment_size = MMAL_MAX_SEGMENT;
		constraints.coherent = 1;
		error = drv_dma_device_create_window(&constraints, MMAL_BUS_RAM, MMAL_BUS_RAM_LIMIT, &mmal_dma);
		if (error != 0)
			return error;
	}

	/* The service, which waits for VCHIQ's connection. */
	error = drv_rpi4_vchiq_open(DRV_RPI4_MMAL_SERVICE_FOURCC, DRV_RPI4_MMAL_VERSION, DRV_RPI4_MMAL_VERSION_MIN,
				    &mmal_service_ops, NULL, &mmal_service);
	if (error != 0)
		return error;

	/* Logs the firmware's MMAL version. */
	drv_rpi4_mmal_header(message, DRV_RPI4_MMAL_MSG_GET_VERSION, 0);
	error = request(NULL, message, DRV_RPI4_MMAL_HEADER_BYTES, reply, sizeof(reply));
	if (error == 0) {
		mmal_major = drv_rpi4_mmal_get32(reply, DRV_RPI4_MMAL_VERSION_MAJOR);
		kern_logf("rpi4-mmal: firmware MMAL %u.%u (service version %u)\n",
			  (unsigned)drv_rpi4_mmal_get32(reply, DRV_RPI4_MMAL_VERSION_MAJOR),
			  (unsigned)drv_rpi4_mmal_get32(reply, DRV_RPI4_MMAL_VERSION_MINOR),
			  (unsigned)drv_rpi4_vchiq_peer_version(mmal_service));
	}

	/* Succeeded: decoders can be created. */
	mmal_ready = true;
	return 0;
}

/*
 * Closes the service so that the next decoder opens it again: the
 * firmware aborts the service's bulks and drops its components.
 *
 * The caller holds mmal_open_lock and no decoder is in use but the one
 * being destroyed.
 */
static void
service_reset(void)
{
	int error;

	/* Nothing to close before the first open. */
	if (!mmal_ready)
		return;

	/* Closes it; a firmware that does not agree in time is logged, and the service is opened anew all the same. */
	drv_rpi4_vchiq_report(mmal_service);
	error = drv_rpi4_vchiq_close(mmal_service);
	if (error != 0)
		kern_logf("rpi4-mmal: the service did not close cleanly (%d)\n", error);
	mmal_service = NULL;
	mmal_ready = false;
}

/* Sets the locks and the wait queue up the first time. */
static void
locks_init(void)
{
	unsigned expected;
	unsigned state;
	int claimed;

	/* Exactly one caller sets them up; decoders are created from thread context. */
	expected = 0;
	claimed = __atomic_compare_exchange_n(&mmal_locks_ready, &expected, 1U, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
	if (!claimed) {
		for (;;) {
			state = __atomic_load_n(&mmal_locks_ready, __ATOMIC_ACQUIRE);
			if (state == 2U)
				break;
			kern_usleep_range(10U, 20U);
		}

		return;
	}

	/* The caller that claimed the set-up makes the locks. */
	spin_init(&mmal_lock, LOCK_RANK_DEVICE, "rpi4 mmal");
	waitq_init(&mmal_wake, "rpi4 mmal");
	(void)mutex_init(&mmal_open_lock, LOCK_RANK_DEVICE, "rpi4 mmal open");
	(void)mutex_init(&mmal_request_lock, LOCK_RANK_DEVICE, "rpi4 mmal request");

	/* Publishes the set-up locks. */
	__atomic_store_n(&mmal_locks_ready, 2U, __ATOMIC_RELEASE);
}

/*
 * Sends one request and waits for its reply.
 *
 * Procedure referenced: the Raspberry Pi userland interface/mmal/vc/ (the
 * MMAL client's messages, waiters, bulk transfers and port flush).
 */

static int
request(
	struct drv_rpi4_mmal_decoder *decoder,
	uint8_t *message,
	size_t size,
	uint8_t *reply,
	size_t capacity)
{
	int error;

	/* A request without a bulk is a request with none. */
	error = request_with_bulk(decoder, message, size, 0, NULL, reply, capacity);
	if (error != 0)
		return error;

	/* Succeeded: reply holds the answer. */
	return 0;
}

/*
 * Sends one request, optionally followed by an eight-byte bulk from
 * physical whose completion tag is tag, and waits up to MMAL_WAIT_MS for
 * its reply.
 *
 * While it waits, the buffers the firmware gives the decoder back are
 * taken in (a frame's bulk received): the firmware may give a frame back
 * before it answers, and not answer until that frame's bulk is received.
 * decoder is NULL for a request of no decoder.  An unanswered request
 * leaves the decoder broken: the firmware's state is unknown.
 */
static int
request_with_bulk(
	struct drv_rpi4_mmal_decoder *decoder,
	uint8_t *message,
	size_t size,
	uint64_t physical,
	struct mmal_bulk_tag *tag,
	uint8_t *reply,
	size_t capacity)
{
	unsigned long enabled;
	uint64_t deadline;
	uint64_t observed;
	uint32_t waiter;
	uint32_t msgid;
	uint32_t word;
	size_t copied;
	bool pending;
	int taken;
	int error;

	/* One request at a time; it is numbered, and its reply is awaited. */
	mutex_lock(&mmal_request_lock);

	enabled = spin_lock_irqsave(&mmal_lock);

	mmal_next_waiter++;
	if (mmal_next_waiter == 0)
		mmal_next_waiter = 1;
	waiter = mmal_next_waiter;
	mmal_reply_waiter = waiter;
	mmal_reply_ready = false;

	spin_unlock_irqrestore(&mmal_lock, enabled);

	/* Sends the request, with its bulk when it has one. */
	drv_rpi4_mmal_put32(message, DRV_RPI4_MMAL_HEADER_WAITER, waiter);
	if (physical != 0) {
		error = drv_rpi4_vchiq_send_bulk(mmal_service, message, (uint32_t)size, DRV_RPI4_VCHIQ_BULK_TRANSMIT,
						 physical, 8U, tag);
	} else {
		error = drv_rpi4_vchiq_send(mmal_service, message, (uint32_t)size);
	}

	/* Waits for the reply, taking the decoder's returned buffers in meanwhile. */
	deadline = sched_ticks() + kern_ms_to_ticks(MMAL_WAIT_MS) + 1U;
	enabled = spin_lock_irqsave(&mmal_lock);

	while (error == 0 && !mmal_reply_ready) {
		pending = false;
		if (decoder != NULL && !decoder->broken)
			pending = buffer_queued(decoder);
		if (pending) {
			spin_unlock_irqrestore(&mmal_lock, enabled);

			taken = pump_buffers(decoder);
			if (taken != 0)
				kern_logf("rpi4-mmal: a returned buffer failed during %s (%d)\n",
					  request_name(drv_rpi4_mmal_get32(message, DRV_RPI4_MMAL_HEADER_MSGID)), taken);
			enabled = spin_lock_irqsave(&mmal_lock);

			continue;
		}

		/* Sleeps until the reply, an arrival or the deadline. */
		observed = waitq_sequence(&mmal_wake);
		error = waitq_sleep(&mmal_wake, &mmal_lock, observed, deadline, 0);
		if (error == ETIMEDOUT && mmal_reply_ready)
			error = 0;
		if (error == ETIMEDOUT)
			break;
		error = 0;
	}

	/* Copies the reply out, the rest of the caller's buffer zeroed. */
	copied = 0;
	if (error == 0) {
		copied = mmal_reply.size;
		if (copied > capacity)
			copied = capacity;
		kern_memcpy(reply, mmal_reply.bytes, copied);
		kern_memset(reply + copied, 0, capacity - copied);
	}

	/* No request waits any more. */
	mmal_reply_waiter = 0;

	spin_unlock_irqrestore(&mmal_lock, enabled);

	mutex_unlock(&mmal_request_lock);

	/* Reports a request that went unanswered. */
	msgid = drv_rpi4_mmal_get32(message, DRV_RPI4_MMAL_HEADER_MSGID);
	if (error != 0) {
		kern_logf("rpi4-mmal: request %u (%s) not answered (%d)\n", (unsigned)msgid, request_name(msgid), error);
		if (decoder != NULL) {
			report_stall(decoder, request_name(msgid));
			decoder->broken = true;
		}

		return error;
	}

	/*
	 * Traces the request and the word where its reply's status is (byte
	 * 24; GET_VERSION's flags there), while the budget lasts.
	 */
	word = 0;
	if (copied >= DRV_RPI4_MMAL_REPLY_BYTES)
		word = drv_rpi4_mmal_get32(reply, DRV_RPI4_MMAL_REPLY_STATUS);
	if (mmal_trace_left > 0 && msgid != DRV_RPI4_MMAL_MSG_GET_VERSION) {
		mmal_trace_left--;
		kern_logf("rpi4-mmal: trace %s (%u bytes) -> %u bytes, status %u (%s)\n",
			  request_name(msgid), (unsigned)size, (unsigned)copied, (unsigned)word, status_name(word));
	}

	/* Succeeded: reply holds the answer. */
	return 0;
}

/*
 * Sets the ports up and starts the component: H.264 in, NV12 out, the time
 * stamps tied to pictures, the buffers counted and sized, and the output
 * buffers given to the firmware.
 */
static int
configure(
	struct drv_rpi4_mmal_decoder *decoder,
	uint32_t width,
	uint32_t height)
{
	uint8_t message[DRV_RPI4_MMAL_MAX_MESSAGE];
	uint8_t reply[DRV_RPI4_MMAL_MAX_MESSAGE];
	uint8_t *format;
	uint8_t *es;
	uint32_t count;
	size_t size;
	unsigned index;
	uint32_t status;
	int error;

	/* Reads both ports. */
	decoder->step = "port info";
	error = port_info(decoder, DRV_RPI4_MMAL_PORT_INPUT, decoder->input_info);
	if (error != 0)
		return error;
	error = port_info(decoder, DRV_RPI4_MMAL_PORT_OUTPUT, decoder->output_info);
	if (error != 0)
		return error;
	decoder->input_handle = drv_rpi4_mmal_get32(decoder->input_info, DRV_RPI4_MMAL_INFO_PORT_HANDLE);
	decoder->output_handle = drv_rpi4_mmal_get32(decoder->output_info, DRV_RPI4_MMAL_INFO_PORT_HANDLE);

	/* The input: H.264 video of the stream's size. */
	format = decoder->input_info + DRV_RPI4_MMAL_INFO_PORT + DRV_RPI4_MMAL_PORT_BYTES;
	es = format + DRV_RPI4_MMAL_FORMAT_BYTES;
	drv_rpi4_mmal_put32(format, DRV_RPI4_MMAL_FORMAT_TYPE, DRV_RPI4_MMAL_ES_VIDEO);
	drv_rpi4_mmal_put32(format, DRV_RPI4_MMAL_FORMAT_ENCODING, DRV_RPI4_MMAL_ENCODING_H264);
	drv_rpi4_mmal_put32(format, DRV_RPI4_MMAL_FORMAT_VARIANT, 0);
	drv_rpi4_mmal_put32(format, DRV_RPI4_MMAL_FORMAT_EXTRADATA_SIZE, 0);
	drv_rpi4_mmal_put32(es, DRV_RPI4_MMAL_ES_WIDTH, width);
	drv_rpi4_mmal_put32(es, DRV_RPI4_MMAL_ES_HEIGHT, height);
	drv_rpi4_mmal_put32(es, DRV_RPI4_MMAL_ES_CROP_X, 0);
	drv_rpi4_mmal_put32(es, DRV_RPI4_MMAL_ES_CROP_Y, 0);
	drv_rpi4_mmal_put32(es, DRV_RPI4_MMAL_ES_CROP_WIDTH, width);
	drv_rpi4_mmal_put32(es, DRV_RPI4_MMAL_ES_CROP_HEIGHT, height);
	decoder->step = "input format";
	error = port_set(decoder, decoder->input_info);
	if (error != 0)
		return error;

	/* The output: NV12 of the stream's size, rows in 32-byte strides and 16-row heights. */
	format = decoder->output_info + DRV_RPI4_MMAL_INFO_PORT + DRV_RPI4_MMAL_PORT_BYTES;
	es = format + DRV_RPI4_MMAL_FORMAT_BYTES;
	drv_rpi4_mmal_put32(format, DRV_RPI4_MMAL_FORMAT_TYPE, DRV_RPI4_MMAL_ES_VIDEO);
	drv_rpi4_mmal_put32(format, DRV_RPI4_MMAL_FORMAT_ENCODING, DRV_RPI4_MMAL_ENCODING_NV12);
	drv_rpi4_mmal_put32(format, DRV_RPI4_MMAL_FORMAT_VARIANT, 0);
	drv_rpi4_mmal_put32(es, DRV_RPI4_MMAL_ES_WIDTH, (width + 31U) & ~31U);
	drv_rpi4_mmal_put32(es, DRV_RPI4_MMAL_ES_HEIGHT, (height + 15U) & ~15U);
	drv_rpi4_mmal_put32(es, DRV_RPI4_MMAL_ES_CROP_X, 0);
	drv_rpi4_mmal_put32(es, DRV_RPI4_MMAL_ES_CROP_Y, 0);
	drv_rpi4_mmal_put32(es, DRV_RPI4_MMAL_ES_CROP_WIDTH, width);
	drv_rpi4_mmal_put32(es, DRV_RPI4_MMAL_ES_CROP_HEIGHT, height);
	decoder->step = "output format";
	error = port_set(decoder, decoder->output_info);
	if (error != 0) {
		/* Without NV12, I420 (the decoder's own default) shows whether the rest works. */
		kern_logf("rpi4-mmal: NV12 output refused; trying I420\n");
		drv_rpi4_mmal_put32(format, DRV_RPI4_MMAL_FORMAT_ENCODING, DRV_RPI4_MMAL_ENCODING_I420);
		decoder->step = "output format I420";
		error = port_set(decoder, decoder->output_info);
	}

	/* Reports an output format the firmware refused in both encodings. */
	if (error != 0)
		return error;

	/* Time stamps follow pictures, not input order, and none are invented (best effort). */
	(void)set_boolean(decoder, decoder->input_handle, MMAL_PARAMETER_TIMESTAMP_FIFO, 0);
	(void)set_boolean(decoder, decoder->input_handle, MMAL_PARAMETER_INTERPOLATE, 0);

	/* Reads the ports again for the buffer sizes the formats imply. */
	decoder->step = "port info again";
	error = port_info(decoder, DRV_RPI4_MMAL_PORT_INPUT, decoder->input_info);
	if (error != 0)
		return error;
	error = port_info(decoder, DRV_RPI4_MMAL_PORT_OUTPUT, decoder->output_info);
	if (error != 0)
		return error;

	/* The input buffers: the firmware's recommendation, within the client's table. */
	decoder->input_size = maximum(drv_rpi4_mmal_get32(decoder->input_info, DRV_RPI4_MMAL_INFO_PORT + DRV_RPI4_MMAL_PORT_BUFFER_SIZE_RECOMMENDED),
				      drv_rpi4_mmal_get32(decoder->input_info, DRV_RPI4_MMAL_INFO_PORT + DRV_RPI4_MMAL_PORT_BUFFER_SIZE_MIN));
	if (decoder->input_size < 4096U)
		decoder->input_size = 65536U;
	count = maximum(drv_rpi4_mmal_get32(decoder->input_info, DRV_RPI4_MMAL_INFO_PORT + DRV_RPI4_MMAL_PORT_BUFFER_NUM_RECOMMENDED),
			drv_rpi4_mmal_get32(decoder->input_info, DRV_RPI4_MMAL_INFO_PORT + DRV_RPI4_MMAL_PORT_BUFFER_NUM_MIN));
	if (count < 2U)
		count = 2U;
	if (count > MMAL_INPUT_BUFFERS)
		count = MMAL_INPUT_BUFFERS;
	decoder->input_count = count;
	drv_rpi4_mmal_put32(decoder->input_info, DRV_RPI4_MMAL_INFO_PORT + DRV_RPI4_MMAL_PORT_BUFFER_NUM, count);
	drv_rpi4_mmal_put32(decoder->input_info, DRV_RPI4_MMAL_INFO_PORT + DRV_RPI4_MMAL_PORT_BUFFER_SIZE, decoder->input_size);
	for (index = 0; index < count; index++)
		decoder->input_free[index] = true;

	/* Enables the input, then the output with its bounce buffers, then the component. */
	decoder->step = "input enable";
	error = port_action(decoder, decoder->input_handle, DRV_RPI4_MMAL_ACTION_ENABLE, decoder->input_info);
	if (error != 0)
		return error;
	decoder->step = "output enable";
	error = enable_output(decoder);
	if (error != 0)
		return error;
	decoder->step = "component enable";
	size = drv_rpi4_mmal_build_component(message, DRV_RPI4_MMAL_MSG_COMPONENT_ENABLE, 0, decoder->component);
	error = request(decoder, message, size, reply, sizeof(reply));
	if (error == 0) {
		status = drv_rpi4_mmal_get32(reply, DRV_RPI4_MMAL_REPLY_STATUS);
		if (status != 0)
			error = EIO;
	}

	/* Reports a component that did not start. */
	if (error != 0)
		return error;

	/* Succeeded: the decoder runs. */
	kern_logf("rpi4-mmal: decoder %u: %ux%u, input %u x %u bytes, output %u x %u bytes\n",
		  decoder->slot, (unsigned)width, (unsigned)height,
		  (unsigned)decoder->input_count, (unsigned)decoder->input_size,
		  (unsigned)decoder->output_count, (unsigned)decoder->output_size);
	return 0;
}

/* Reads one port's info into a port info reply. */
static int
port_info(
	struct drv_rpi4_mmal_decoder *decoder,
	uint32_t type,
	uint8_t *info)
{
	uint8_t message[DRV_RPI4_MMAL_MAX_MESSAGE];
	const uint8_t *port;
	const uint8_t *format;
	const uint8_t *es;
	uint32_t status;
	uint32_t found;
	size_t size;
	int error;

	/* Asks for the first port of the type. */
	size = drv_rpi4_mmal_build_info_get(message, 0, decoder->component, type, 0);
	error = request(decoder, message, size, info, DRV_RPI4_MMAL_INFO_BYTES);
	if (error != 0)
		return error;

	/*
	 * Refuses a port the firmware reports an error for.  The reply's
	 * found word is logged, not required: the BSD-3 userland client reads
	 * only the status.
	 */
	status = drv_rpi4_mmal_get32(info, DRV_RPI4_MMAL_INFO_STATUS);
	found = drv_rpi4_mmal_get32(info, DRV_RPI4_MMAL_INFO_FOUND);
	port = info + DRV_RPI4_MMAL_INFO_PORT;
	format = port + DRV_RPI4_MMAL_PORT_BYTES;
	es = format + DRV_RPI4_MMAL_FORMAT_BYTES;
	kern_logf("rpi4-mmal: port %u: status %u (%s) found %u handle %u buffers min %u/%u rec %u/%u now %u/%u encoding %08x %ux%u crop %ux%u\n",
		  (unsigned)type, (unsigned)status, status_name(status), (unsigned)found,
		  (unsigned)drv_rpi4_mmal_get32(info, DRV_RPI4_MMAL_INFO_PORT_HANDLE),
		  (unsigned)drv_rpi4_mmal_get32(port, DRV_RPI4_MMAL_PORT_BUFFER_NUM_MIN),
		  (unsigned)drv_rpi4_mmal_get32(port, DRV_RPI4_MMAL_PORT_BUFFER_SIZE_MIN),
		  (unsigned)drv_rpi4_mmal_get32(port, DRV_RPI4_MMAL_PORT_BUFFER_NUM_RECOMMENDED),
		  (unsigned)drv_rpi4_mmal_get32(port, DRV_RPI4_MMAL_PORT_BUFFER_SIZE_RECOMMENDED),
		  (unsigned)drv_rpi4_mmal_get32(port, DRV_RPI4_MMAL_PORT_BUFFER_NUM),
		  (unsigned)drv_rpi4_mmal_get32(port, DRV_RPI4_MMAL_PORT_BUFFER_SIZE),
		  (unsigned)drv_rpi4_mmal_get32(format, DRV_RPI4_MMAL_FORMAT_ENCODING),
		  (unsigned)drv_rpi4_mmal_get32(es, DRV_RPI4_MMAL_ES_WIDTH),
		  (unsigned)drv_rpi4_mmal_get32(es, DRV_RPI4_MMAL_ES_HEIGHT),
		  (unsigned)drv_rpi4_mmal_get32(es, DRV_RPI4_MMAL_ES_CROP_WIDTH),
		  (unsigned)drv_rpi4_mmal_get32(es, DRV_RPI4_MMAL_ES_CROP_HEIGHT));
	if (status != 0)
		return ENODEV;

	/* Succeeded: info describes the port. */
	return 0;
}

/* Sets one port's format from an edited port info reply. */
static int
port_set(
	struct drv_rpi4_mmal_decoder *decoder,
	uint8_t *info)
{
	uint8_t message[DRV_RPI4_MMAL_MAX_MESSAGE];
	uint8_t reply[DRV_RPI4_MMAL_MAX_MESSAGE];
	uint32_t status;
	size_t size;
	int error;

	/* Sends the edited blocks back. */
	size = drv_rpi4_mmal_build_info_set(message, 0, decoder->component, info);
	error = request(decoder, message, size, reply, sizeof(reply));
	if (error != 0)
		return error;

	/* Reports a format the firmware refused. */
	status = drv_rpi4_mmal_get32(reply, DRV_RPI4_MMAL_INFO_STATUS);
	if (status != 0) {
		kern_logf("rpi4-mmal: port format %08x refused (status %u)\n",
			  (unsigned)drv_rpi4_mmal_get32(info, DRV_RPI4_MMAL_INFO_PORT + DRV_RPI4_MMAL_PORT_BYTES + DRV_RPI4_MMAL_FORMAT_ENCODING),
			  (unsigned)status);
		return EIO;
	}

	/* Succeeded: the port has the format. */
	return 0;
}

/* Runs one port action. */
static int
port_action(
	struct drv_rpi4_mmal_decoder *decoder,
	uint32_t port_handle,
	uint32_t action,
	const uint8_t *info)
{
	uint8_t message[DRV_RPI4_MMAL_MAX_MESSAGE];
	uint8_t reply[DRV_RPI4_MMAL_MAX_MESSAGE];
	size_t size;
	uint32_t status;
	int error;

	/* Sends the action and checks its status. */
	size = drv_rpi4_mmal_build_action(message, 0, decoder->component, port_handle, action, info);
	error = request(decoder, message, size, reply, sizeof(reply));
	if (error == 0) {
		status = drv_rpi4_mmal_get32(reply, DRV_RPI4_MMAL_REPLY_STATUS);
		if (status != 0)
			error = EIO;
	}

	/* Reports an action that failed. */
	if (error != 0) {
		kern_logf("rpi4-mmal: port action %u failed (%d)\n", (unsigned)action, error);
		return error;
	}

	/* Succeeded: the action is done. */
	return 0;
}

/* Sets one boolean parameter of a port, logging a refusal. */
static int
set_boolean(
	struct drv_rpi4_mmal_decoder *decoder,
	uint32_t port_handle,
	uint32_t id,
	uint32_t value)
{
	uint8_t message[DRV_RPI4_MMAL_MAX_MESSAGE];
	uint8_t reply[DRV_RPI4_MMAL_MAX_MESSAGE];
	size_t size;
	uint32_t status;
	int error;

	/* Sends the parameter and checks its status. */
	size = drv_rpi4_mmal_build_parameter32(message, 0, decoder->component, port_handle, id, value);
	error = request(decoder, message, size, reply, sizeof(reply));
	if (error == 0) {
		status = drv_rpi4_mmal_get32(reply, DRV_RPI4_MMAL_REPLY_STATUS);
		if (status != 0)
			error = EIO;
	}

	/* Reports a parameter the firmware refused. */
	if (error != 0) {
		kern_logf("rpi4-mmal: parameter %x not set (%d)\n", (unsigned)id, error);
		return error;
	}

	/* Succeeded: the parameter is set. */
	return 0;
}

/*
 * Enables the output port with as many bounce buffers as the firmware
 * recommends (at least three, at most MMAL_OUTPUT_BUFFERS) of the size it
 * wants, and gives them all to it.
 */
static int
enable_output(
	struct drv_rpi4_mmal_decoder *decoder)
{
	uint8_t *es;
	uint32_t size;
	uint32_t count;
	uint32_t frame;
	unsigned index;
	bool replaced;
	int error;

	/* The size: the firmware's, but at least a whole NV12 frame of the format. */
	es = decoder->output_info + DRV_RPI4_MMAL_INFO_PORT + DRV_RPI4_MMAL_PORT_BYTES + DRV_RPI4_MMAL_FORMAT_BYTES;
	frame = drv_rpi4_mmal_get32(es, DRV_RPI4_MMAL_ES_WIDTH) * drv_rpi4_mmal_get32(es, DRV_RPI4_MMAL_ES_HEIGHT) * 3U / 2U;
	size = maximum(drv_rpi4_mmal_get32(decoder->output_info, DRV_RPI4_MMAL_INFO_PORT + DRV_RPI4_MMAL_PORT_BUFFER_SIZE_RECOMMENDED),
		       drv_rpi4_mmal_get32(decoder->output_info, DRV_RPI4_MMAL_INFO_PORT + DRV_RPI4_MMAL_PORT_BUFFER_SIZE_MIN));
	size = maximum(size, frame);
	size = (size + 4095U) & ~4095U;
	count = maximum(drv_rpi4_mmal_get32(decoder->output_info, DRV_RPI4_MMAL_INFO_PORT + DRV_RPI4_MMAL_PORT_BUFFER_NUM_MIN),
			drv_rpi4_mmal_get32(decoder->output_info, DRV_RPI4_MMAL_INFO_PORT + DRV_RPI4_MMAL_PORT_BUFFER_NUM_RECOMMENDED));
	count = maximum(count, MMAL_OUTPUT_MINIMUM);
	count = maximum(count, decoder->frames);
	if (count > MMAL_OUTPUT_BUFFERS)
		count = MMAL_OUTPUT_BUFFERS;

	/* Replaces the bounce buffers when the size grew; none may be with the firmware. */
	replaced = false;
	if (size > decoder->output_size || count != decoder->output_count) {
		replaced = true;
		for (index = 0; index < decoder->output_count; index++) {
			if (decoder->outputs[index].with_firmware) {
				kern_logf("rpi4-mmal: output buffer %u not given back before a resize\n", index);
				return EIO;
			}

			/* A frame the caller still reads cannot lose its memory. */
			if (decoder->outputs[index].held) {
				kern_logf("rpi4-mmal: output buffer %u held by the caller at a resize\n", index);
				return EIO;
			}
		}

		/* Frees the old buffers. */
		for (index = 0; index < MMAL_OUTPUT_BUFFERS; index++) {
			if (decoder->outputs[index].memory.address != NULL)
				dma_free(&decoder->outputs[index].memory);
			kern_memset(&decoder->outputs[index], 0, sizeof(decoder->outputs[index]));
		}

		/* Allocates the new ones. */
		for (index = 0; index < count; index++) {
			error = dma_alloc(size, &decoder->outputs[index].memory);
			if (error != 0)
				return error;
		}

		/* The size and count the port is enabled with. */
		decoder->output_size = size;
		decoder->output_count = count;
	}

	/* The format the frames will have, until a format change says otherwise. */
	decoder->encoding = drv_rpi4_mmal_get32(decoder->output_info, DRV_RPI4_MMAL_INFO_PORT + DRV_RPI4_MMAL_PORT_BYTES + DRV_RPI4_MMAL_FORMAT_ENCODING);
	decoder->width = drv_rpi4_mmal_get32(es, DRV_RPI4_MMAL_ES_WIDTH);
	decoder->height = drv_rpi4_mmal_get32(es, DRV_RPI4_MMAL_ES_HEIGHT);
	decoder->crop_width = drv_rpi4_mmal_get32(es, DRV_RPI4_MMAL_ES_CROP_WIDTH);
	decoder->crop_height = drv_rpi4_mmal_get32(es, DRV_RPI4_MMAL_ES_CROP_HEIGHT);

	/* Enables the port with the count and size. */
	drv_rpi4_mmal_put32(decoder->output_info, DRV_RPI4_MMAL_INFO_PORT + DRV_RPI4_MMAL_PORT_BUFFER_NUM, count);
	drv_rpi4_mmal_put32(decoder->output_info, DRV_RPI4_MMAL_INFO_PORT + DRV_RPI4_MMAL_PORT_BUFFER_SIZE, size);
	error = port_action(decoder, decoder->output_handle, DRV_RPI4_MMAL_ACTION_ENABLE, decoder->output_info);
	if (error != 0)
		return error;
	decoder->output_enabled = true;

	/*
	 * Gives every buffer to the firmware but those whose memory was kept
	 * and that hold a frame: one received before the port was disabled
	 * (the format change's first frame comes right after the event), and
	 * the one the caller holds.  They go back when the caller is done.
	 */
	for (index = 0; index < count; index++) {
		decoder->outputs[index].with_firmware = false;
		if (!replaced && (decoder->outputs[index].ready || decoder->outputs[index].held))
			continue;
		decoder->outputs[index].ready = false;
		decoder->outputs[index].held = false;
		error = give_output(decoder, index);
		if (error != 0)
			return error;
	}

	/* Succeeded: the firmware can output frames. */
	return 0;
}

/* Gives one empty output buffer to the firmware. */
static int
give_output(
	struct drv_rpi4_mmal_decoder *decoder,
	unsigned index)
{
	uint8_t message[DRV_RPI4_MMAL_BUFFER_BYTES];
	int error;

	/* A disabled port takes no buffers; it gets them all when enabled again. */
	if (!decoder->output_enabled)
		return 0;

	/* An empty buffer message without a bulk. */
	(void)drv_rpi4_mmal_build_buffer(message, DRV_RPI4_MMAL_MSG_BUFFER_FROM_HOST, 0, decoder->component,
					 decoder->output_handle, token(decoder, MMAL_KIND_OUTPUT, index),
					 decoder->output_size, 0, 0, DRV_RPI4_MMAL_TIME_UNKNOWN);
	error = drv_rpi4_vchiq_send(mmal_service, message, sizeof(message));
	if (error != 0) {
		decoder->broken = true;
		return error;
	}

	/* Succeeded: the firmware has the buffer. */
	decoder->outputs[index].with_firmware = true;
	return 0;
}

/*
 * Handles everything queued for the decoder, in the order it came.
 */
static int
pump(
	struct drv_rpi4_mmal_decoder *decoder)
{
	struct mmal_message message;
	uint32_t msgid;
	int error;

	/* Handles one arrival at a time until the queue is empty. */
	for (;;) {
		error = take_message(decoder, &message);
		if (error == EAGAIN)
			break;
		if (error != 0)
			return error;
		trace_arrival(decoder, &message);
		msgid = drv_rpi4_mmal_get32(message.bytes, DRV_RPI4_MMAL_HEADER_MSGID);
		if (msgid == DRV_RPI4_MMAL_MSG_BUFFER_TO_HOST) {
			error = handle_buffer(decoder, &message);
		} else {
			error = handle_event(decoder, &message);
		}

		/* A failed arrival leaves the decoder's state unknown. */
		if (error != 0) {
			decoder->broken = true;
			return error;
		}
	}

	/* Succeeded: nothing waits. */
	return 0;
}

/*
 * Takes in the decoder's returned buffers only, oldest first, leaving its
 * events queued.  It stops before an event whose data comes as a bulk:
 * that bulk is the firmware's next, and is received before any later one.
 */
static int
pump_buffers(
	struct drv_rpi4_mmal_decoder *decoder)
{
	struct mmal_message message;
	int error;

	/* One buffer at a time until none can be taken. */
	for (;;) {
		error = take_buffer(decoder, &message);
		if (error == EAGAIN)
			break;
		if (error != 0)
			return error;
		trace_arrival(decoder, &message);
		error = handle_buffer(decoder, &message);
		if (error != 0) {
			decoder->broken = true;
			return error;
		}
	}

	/* Succeeded: no buffer waits before such an event. */
	return 0;
}

/*
 * Takes the oldest queued buffer out of the queue, moving the arrivals
 * after it up; EAGAIN when there is none before an event that carries a
 * bulk.
 */
static int
take_buffer(
	struct drv_rpi4_mmal_decoder *decoder,
	struct mmal_message *message)
{
	unsigned long enabled;
	unsigned position;
	unsigned index;
	unsigned next;
	uint32_t msgid;
	uint32_t length;
	int error;

	/* Looks from the oldest arrival for a buffer. */
	error = EAGAIN;
	enabled = spin_lock_irqsave(&mmal_lock);

	for (position = 0; position < decoder->queue_count && !decoder->queue_overflow; position++) {
		index = (decoder->queue_head + position) % MMAL_QUEUE;
		msgid = drv_rpi4_mmal_get32(decoder->queue[index].bytes, DRV_RPI4_MMAL_HEADER_MSGID);
		if (msgid != DRV_RPI4_MMAL_MSG_BUFFER_TO_HOST) {
			length = drv_rpi4_mmal_get32(decoder->queue[index].bytes, DRV_RPI4_MMAL_EVENT_LENGTH);
			if (length > DRV_RPI4_MMAL_EVENT_DATA_BYTES)
				break;
			continue;
		}

		/* Copies the buffer out and moves each later arrival one place up. */
		*message = decoder->queue[index];
		for (; position + 1U < decoder->queue_count; position++) {
			index = (decoder->queue_head + position) % MMAL_QUEUE;
			next = (index + 1U) % MMAL_QUEUE;
			decoder->queue[index] = decoder->queue[next];
		}

		/* One arrival fewer: the buffer is taken. */
		decoder->queue_count--;
		error = 0;
		break;
	}

	spin_unlock_irqrestore(&mmal_lock, enabled);

	/* Reports that no buffer could be taken. */
	if (error != 0)
		return error;

	/* Succeeded: message holds the buffer. */
	return 0;
}

/*
 * Reports whether take_buffer would take a buffer: one is queued before
 * any event that carries a bulk.  The caller holds mmal_lock.
 */
static bool
buffer_queued(
	const struct drv_rpi4_mmal_decoder *decoder)
{
	unsigned position;
	unsigned index;
	uint32_t msgid;
	uint32_t length;

	/* The arrivals take_buffer would look at, in its order. */
	if (decoder->queue_overflow)
		return false;
	for (position = 0; position < decoder->queue_count; position++) {
		index = (decoder->queue_head + position) % MMAL_QUEUE;
		msgid = drv_rpi4_mmal_get32(decoder->queue[index].bytes, DRV_RPI4_MMAL_HEADER_MSGID);
		if (msgid == DRV_RPI4_MMAL_MSG_BUFFER_TO_HOST)
			return true;
		length = drv_rpi4_mmal_get32(decoder->queue[index].bytes, DRV_RPI4_MMAL_EVENT_LENGTH);
		if (length > DRV_RPI4_MMAL_EVENT_DATA_BYTES)
			return false;
	}

	/* Succeeded: none. */
	return false;
}

/* Traces one arrival (a returned buffer or an event) while the budget lasts. */
static void
trace_arrival(
	const struct drv_rpi4_mmal_decoder *decoder,
	const struct mmal_message *message)
{
	uint32_t msgid;
	uint32_t value;
	const char *kind;

	/* Only while the budget lasts. */
	if (mmal_arrival_left == 0)
		return;
	mmal_arrival_left--;

	/* A buffer: whose, its length, flags, time and short payload; an event: its command and length. */
	msgid = drv_rpi4_mmal_get32(message->bytes, DRV_RPI4_MMAL_HEADER_MSGID);
	if (msgid != DRV_RPI4_MMAL_MSG_BUFFER_TO_HOST) {
		kern_logf("rpi4-mmal: in %u: event %08x len %u\n", decoder->slot,
			  (unsigned)drv_rpi4_mmal_get32(message->bytes, DRV_RPI4_MMAL_EVENT_CMD),
			  (unsigned)drv_rpi4_mmal_get32(message->bytes, DRV_RPI4_MMAL_EVENT_LENGTH));
		return;
	}

	/* A buffer: whose, its length, flags, time and short payload. */
	value = drv_rpi4_mmal_get32(message->bytes, DRV_RPI4_MMAL_BUFFER_CONTEXT);
	kind = "other";
	if (((value >> 12) & 0xfU) == MMAL_KIND_INPUT)
		kind = "input";
	if (((value >> 12) & 0xfU) == MMAL_KIND_OUTPUT)
		kind = "output";
	kern_logf("rpi4-mmal: in %u: %s %u len %u off %u flags %x pts %lld short %u zero_copy %u\n",
		  decoder->slot, kind, (unsigned)(value & 0xfffU),
		  (unsigned)drv_rpi4_mmal_get32(message->bytes, DRV_RPI4_MMAL_BUFFER_LENGTH),
		  (unsigned)drv_rpi4_mmal_get32(message->bytes, DRV_RPI4_MMAL_BUFFER_OFFSET),
		  (unsigned)drv_rpi4_mmal_get32(message->bytes, DRV_RPI4_MMAL_BUFFER_FLAGS),
		  (long long)drv_rpi4_mmal_get64(message->bytes, DRV_RPI4_MMAL_BUFFER_PTS),
		  (unsigned)drv_rpi4_mmal_get32(message->bytes, DRV_RPI4_MMAL_BUFFER_PAYLOAD_IN_MESSAGE),
		  (unsigned)drv_rpi4_mmal_get32(message->bytes, DRV_RPI4_MMAL_BUFFER_ZERO_COPY));
}

/*
 * Logs what a stalled decoder waits with: its queued arrivals, its bulks
 * and buffers, and the service's bulk queues.
 */
static void
report_stall(
	struct drv_rpi4_mmal_decoder *decoder,
	const char *what)
{
	struct mmal_message message;
	unsigned long enabled;
	unsigned position;
	unsigned count;
	unsigned with_firmware;
	unsigned free_inputs;
	unsigned index;
	uint32_t msgid;
	uint32_t bulks;
	uint32_t buffers;
	bool frame_pending;
	bool scratch_pending;

	/* The counts the receive thread shares. */
	enabled = spin_lock_irqsave(&mmal_lock);

	count = decoder->queue_count;
	frame_pending = decoder->frame_pending;
	scratch_pending = decoder->scratch_pending;
	bulks = 0;
	buffers = 0;
	for (index = 0; index < MMAL_STAGING_AREAS; index++) {
		bulks += decoder->areas[index].bulks;
		buffers += decoder->areas[index].buffers;
	}

	spin_unlock_irqrestore(&mmal_lock, enabled);

	/* The buffers each side has. */
	with_firmware = 0;
	for (index = 0; index < decoder->output_count; index++) {
		if (decoder->outputs[index].with_firmware)
			with_firmware++;
	}

	/* The input buffers the firmware has given back. */
	free_inputs = 0;
	for (index = 0; index < decoder->input_count; index++) {
		if (decoder->input_free[index])
			free_inputs++;
	}

	/* The counts in one line. */
	kern_logf("rpi4-mmal: stall at %s: decoder %u queued %u, frame bulk %u, scratch bulk %u, staging bulks %u buffers %u, outputs with firmware %u/%u (enabled %u), inputs free %u/%u\n",
		  what, decoder->slot, count, frame_pending ? 1U : 0U, scratch_pending ? 1U : 0U,
		  (unsigned)bulks, (unsigned)buffers,
		  with_firmware, (unsigned)decoder->output_count, decoder->output_enabled ? 1U : 0U,
		  free_inputs, (unsigned)decoder->input_count);

	/* The queued arrivals, oldest first (at most eight). */
	for (position = 0; position < count && position < 8U; position++) {
		enabled = spin_lock_irqsave(&mmal_lock);

		message = decoder->queue[(decoder->queue_head + position) % MMAL_QUEUE];

		spin_unlock_irqrestore(&mmal_lock, enabled);

		msgid = drv_rpi4_mmal_get32(message.bytes, DRV_RPI4_MMAL_HEADER_MSGID);
		kern_logf("rpi4-mmal: stall: queued %u: %s\n", position, msgid == DRV_RPI4_MMAL_MSG_BUFFER_TO_HOST ? "buffer" : "event");
		if (mmal_arrival_left == 0)
			mmal_arrival_left = 1U;
		trace_arrival(decoder, &message);
	}

	/* The service's bulk queues. */
	if (mmal_service != NULL)
		drv_rpi4_vchiq_report(mmal_service);
}

/* Takes the oldest queued arrival; EAGAIN when there is none. */
static int
take_message(
	struct drv_rpi4_mmal_decoder *decoder,
	struct mmal_message *message)
{
	unsigned long enabled;
	bool overflow;
	bool empty;

	/* Copies the oldest arrival out of the queue. */
	enabled = spin_lock_irqsave(&mmal_lock);

	overflow = decoder->queue_overflow;
	empty = true;
	if (decoder->queue_count != 0)
		empty = false;
	if (!empty && !overflow) {
		*message = decoder->queue[decoder->queue_head];
		decoder->queue_head = (decoder->queue_head + 1U) % MMAL_QUEUE;
		decoder->queue_count--;
	}

	spin_unlock_irqrestore(&mmal_lock, enabled);

	/* A lost arrival leaves the decoder's state unknown. */
	if (overflow) {
		kern_logf("rpi4-mmal: decoder %u queue overflow; decoder stopped\n", decoder->slot);
		decoder->broken = true;
		return EIO;
	}

	/* Nothing is queued. */
	if (empty)
		return EAGAIN;

	/* Succeeded: message holds the arrival. */
	return 0;
}

/*
 * Takes one buffer the firmware gave back.
 *
 * An input buffer is free again.  An output buffer with data (or an empty
 * end of stream) has its bytes received now, in arrival order, into its
 * bounce memory and waits for the caller; an empty one (a flush, a
 * disable) goes back to the firmware while the port is enabled.
 */
static int
handle_buffer(
	struct drv_rpi4_mmal_decoder *decoder,
	const struct mmal_message *message)
{
	struct mmal_output *output;
	struct mmal_target *target;
	unsigned long enabled;
	uint32_t value;
	uint32_t kind;
	uint32_t index;
	uint32_t length;
	uint32_t offset;
	uint32_t flags;
	uint32_t bytes;
	uint32_t payload;
	int error;

	/* The buffer's name, and what it carries. */
	value = drv_rpi4_mmal_get32(message->bytes, DRV_RPI4_MMAL_BUFFER_CONTEXT);
	kind = (value >> 12) & 0xfU;
	index = value & 0xfffU;
	length = drv_rpi4_mmal_get32(message->bytes, DRV_RPI4_MMAL_BUFFER_LENGTH);
	offset = drv_rpi4_mmal_get32(message->bytes, DRV_RPI4_MMAL_BUFFER_OFFSET);
	flags = drv_rpi4_mmal_get32(message->bytes, DRV_RPI4_MMAL_BUFFER_FLAGS);
	payload = drv_rpi4_mmal_get32(message->bytes, DRV_RPI4_MMAL_BUFFER_PAYLOAD_IN_MESSAGE);
	if (payload > DRV_RPI4_MMAL_SHORT_DATA_BYTES) {
		kern_logf("rpi4-mmal: buffer with a %u-byte payload in its message\n", (unsigned)payload);
		return EIO;
	}

	/*
	 * An input buffer comes back: it and its staging area's count are
	 * free.  Data it carries (the BSD-3 userland client takes any
	 * buffer's data the same way) is in the message when it is short,
	 * else a bulk received into the scratch; either is dropped.
	 */
	if (kind == MMAL_KIND_INPUT) {
		if (index >= decoder->input_count)
			return 0;
		if ((length != 0 || (flags & DRV_RPI4_MMAL_FLAG_EOS) != 0) && payload == 0) {
			bytes = (length + 3U) & ~3U;
			if (bytes == 0)
				bytes = 8U;
			if (bytes > MMAL_SCRATCH_BYTES)
				return EIO;
			error = receive_bulk(decoder, bus_to_physical(decoder->scratch.device_address), decoder->scratch.address, bytes, false);
			if (error != 0)
				return error;
		}

		/*
		 * The firmware could not fetch the buffer's bulk (T1-685: four
		 * in a row when the core clock changed under a decode, and the
		 * decoder stopped answering after them).  The unit is lost;
		 * logged so a stall that follows can be traced to it.
		 */
		if ((flags & DRV_RPI4_MMAL_FLAG_TRANSMISSION_FAILED) != 0)
			kern_logf("rpi4-mmal: input %u came back unread: the firmware's transfer of it failed\n", (unsigned)index);

		/* The buffer is free, and its staging area has one buffer fewer. */
		decoder->input_free[index] = true;
		enabled = spin_lock_irqsave(&mmal_lock);

		decoder->areas[decoder->input_area[index]].buffers--;

		spin_unlock_irqrestore(&mmal_lock, enabled);

		return 0;
	}

	/* Anything else must be one of the output buffers. */
	if (kind != MMAL_KIND_OUTPUT || index >= decoder->output_count)
		return 0;
	output = &decoder->outputs[index];
	output->with_firmware = false;

	/* An empty buffer that is not an end of stream goes straight back. */
	if (length == 0 && (flags & DRV_RPI4_MMAL_FLAG_EOS) == 0) {
		error = give_output(decoder, index);
		return error;
	}

	/*
	 * A short payload came in the message: copied to the start of the
	 * bounce memory, without a bulk, and cleaned, so no dirty line of it
	 * is written back over a later frame's bulk.
	 */
	if (payload != 0) {
		if (payload > decoder->output_size)
			return EIO;
		kern_memcpy(output->memory.address, message->bytes + DRV_RPI4_MMAL_BUFFER_SHORT_DATA, payload);
		kern_dcache_clean_range(output->memory.address, payload);
		length = payload;
		offset = 0;
	}

	/* The bulk: four-byte multiples, eight bytes for an empty end of stream. */
	bytes = (length + 3U) & ~3U;
	if (bytes == 0)
		bytes = 8U;
	if ((uint64_t)offset + bytes > decoder->output_size) {
		kern_logf("rpi4-mmal: frame of %u bytes at %u overruns its %u-byte buffer\n",
			  (unsigned)length, (unsigned)offset, (unsigned)decoder->output_size);
		return EIO;
	}

	/* A frame whose decode has a target of its layout goes there instead (a short payload never does). */
	target = NULL;
	if (payload == 0)
		target = frame_target(decoder, message, offset, bytes);

	/*
	 * Receives the frame into its target, or into the buffer's bounce
	 * memory; a short payload is already there.  A transfer into a target
	 * that did not complete leaves the target kept: the VPU may still
	 * write its run.
	 */
	error = 0;
	output->direct = false;
	if (target != NULL) {
		error = receive_bulk(decoder, target->target.physical, NULL, bytes, true);
		if (error != 0 && decoder->frame_pending)
			target->kept = true;
		output->direct = true;
	} else if (payload == 0) {
		error = receive_bulk(decoder,
				     bus_to_physical(output->memory.device_address) + offset,
				     (uint8_t *)output->memory.address + offset,
				     bytes,
				     true);
	}

	/* Reports a frame not received. */
	if (error != 0)
		return error;

	/* The frame waits for the caller, with its layout and its place among the arrivals. */
	output->ready = true;
	decoder->arrivals++;
	output->arrival = decoder->arrivals;
	output->length = length;
	output->flags = flags;
	output->pts = (int64_t)drv_rpi4_mmal_get64(message->bytes, DRV_RPI4_MMAL_BUFFER_PTS);
	output->planes = drv_rpi4_mmal_get32(message->bytes, DRV_RPI4_MMAL_BUFFER_PLANES);
	output->plane_offset[0] = drv_rpi4_mmal_get32(message->bytes, DRV_RPI4_MMAL_BUFFER_PLANE_OFFSET) + offset;
	output->plane_offset[1] = drv_rpi4_mmal_get32(message->bytes, DRV_RPI4_MMAL_BUFFER_PLANE_OFFSET + 4U) + offset;
	output->plane_pitch[0] = drv_rpi4_mmal_get32(message->bytes, DRV_RPI4_MMAL_BUFFER_PLANE_PITCH);
	output->plane_pitch[1] = drv_rpi4_mmal_get32(message->bytes, DRV_RPI4_MMAL_BUFFER_PLANE_PITCH + 4U);

	/* Succeeded: the frame is received. */
	return 0;
}

/*
 * Finds the target of a frame's decode, if it has one and the frame's layout is the target's.
 *
 * The frame must start the buffer, its luma rows must be the target's pitch
 * apart and its chroma plane at the target's offset, and its bulk must fit
 * the target's run.  A frame that does not fit is said a few times and goes
 * to the bounce memory.
 */
static struct mmal_target *
frame_target(
	struct drv_rpi4_mmal_decoder *decoder,
	const struct mmal_message *message,
	uint32_t offset,
	uint32_t bytes)
{
	struct mmal_target *target;
	int64_t pts;
	uint32_t planes;
	uint32_t luma;
	uint32_t chroma;
	uint32_t pitch;
	unsigned index;

	/* The decode's target by the frame's time stamp. */
	pts = (int64_t)drv_rpi4_mmal_get64(message->bytes, DRV_RPI4_MMAL_BUFFER_PTS);
	target = NULL;
	for (index = 0; index < DRV_RPI4_MMAL_TARGETS; index++) {
		if (decoder->targets[index].used && decoder->targets[index].target.pts == pts)
			target = &decoder->targets[index];
	}

	/* A decode without a target uses the bounce memory. */
	if (target == NULL)
		return NULL;

	/* The frame's layout as the message describes it. */
	planes = drv_rpi4_mmal_get32(message->bytes, DRV_RPI4_MMAL_BUFFER_PLANES);
	luma = drv_rpi4_mmal_get32(message->bytes, DRV_RPI4_MMAL_BUFFER_PLANE_OFFSET);
	chroma = drv_rpi4_mmal_get32(message->bytes, DRV_RPI4_MMAL_BUFFER_PLANE_OFFSET + 4U);
	pitch = drv_rpi4_mmal_get32(message->bytes, DRV_RPI4_MMAL_BUFFER_PLANE_PITCH);

	/* An NV12 frame starts the buffer with its luma, as the target's run starts. */
	if (decoder->encoding != DRV_RPI4_MMAL_FRAME_NV12 ||
	    offset != 0 ||
	    planes < 2U ||
	    luma != 0)
		target = NULL;

	/* Its rows and chroma plane are where the target has them. */
	if (target != NULL) {
		if (pitch != target->target.pitch || chroma != target->target.chroma)
			target = NULL;
	}

	/* Its bulk fits the run. */
	if (target != NULL && bytes > target->target.bytes)
		target = NULL;

	/* A frame of another layout goes to the bounce memory, said a few times. */
	if (target == NULL) {
		if (decoder->layout_lines < MMAL_LAYOUT_LINES) {
			decoder->layout_lines++;
			kern_logf("rpi4-mmal: frame %lld (offset %u, planes %u, luma %u, pitch %u, chroma %u, %u bytes) is not its target's layout\n",
				  (long long)pts, (unsigned)offset, (unsigned)planes, (unsigned)luma, (unsigned)pitch, (unsigned)chroma, (unsigned)bytes);
		}

		/* The bounce memory takes the frame. */
		return NULL;
	}

	/* Succeeded: the frame goes to its target. */
	return target;
}

/*
 * Takes one event: its data comes in the message, or (beyond 256 bytes)
 * as a bulk received now into the scratch.  A format change reconfigures
 * the output; other events are logged.
 */
static int
handle_event(
	struct drv_rpi4_mmal_decoder *decoder,
	const struct mmal_message *message)
{
	const uint8_t *data;
	uint32_t command;
	uint32_t length;
	int error;

	/* The event's command and data. */
	command = drv_rpi4_mmal_get32(message->bytes, DRV_RPI4_MMAL_EVENT_CMD);
	length = drv_rpi4_mmal_get32(message->bytes, DRV_RPI4_MMAL_EVENT_LENGTH);
	data = message->bytes + DRV_RPI4_MMAL_EVENT_DATA;
	if (length > DRV_RPI4_MMAL_EVENT_DATA_BYTES) {
		if (length > MMAL_SCRATCH_BYTES)
			return EIO;
		error = receive_bulk(decoder, bus_to_physical(decoder->scratch.device_address), decoder->scratch.address, (length + 3U) & ~3U, false);
		if (error != 0)
			return error;
		data = decoder->scratch.address;
	}

	/* A format change needs its whole description. */
	if (command == DRV_RPI4_MMAL_EVENT_FORMAT_CHANGED && length >= DRV_RPI4_MMAL_CHANGED_BYTES) {
		error = format_changed(decoder, data);
		return error;
	}

	/* Succeeded: any other event is only reported. */
	kern_logf("rpi4-mmal: decoder %u event %08x (%u bytes)\n", decoder->slot, (unsigned)command, (unsigned)length);
	return 0;
}

/*
 * Reconfigures the output for a new format: disables the port (the
 * firmware gives every buffer back), sets the format and the buffer needs
 * the event gives, and enables it again with its buffers.
 */
static int
format_changed(
	struct drv_rpi4_mmal_decoder *decoder,
	const uint8_t *data)
{
	uint8_t *port;
	int error;

	/* Disables the output; its buffers come back empty and stay with the client. */
	decoder->output_enabled = false;
	error = port_action(decoder, decoder->output_handle, DRV_RPI4_MMAL_ACTION_DISABLE, NULL);
	if (error != 0)
		return error;

	/* Takes the new format and buffer needs into the output's port info. */
	port = decoder->output_info + DRV_RPI4_MMAL_INFO_PORT;
	kern_memcpy(port + DRV_RPI4_MMAL_PORT_BYTES, data + DRV_RPI4_MMAL_CHANGED_FORMAT, DRV_RPI4_MMAL_FORMAT_BYTES);
	kern_memcpy(port + DRV_RPI4_MMAL_PORT_BYTES + DRV_RPI4_MMAL_FORMAT_BYTES, data + DRV_RPI4_MMAL_CHANGED_ES, DRV_RPI4_MMAL_ES_BYTES);
	drv_rpi4_mmal_put32(port + DRV_RPI4_MMAL_PORT_BYTES, DRV_RPI4_MMAL_FORMAT_EXTRADATA_SIZE, 0);
	drv_rpi4_mmal_put32(port, DRV_RPI4_MMAL_PORT_BUFFER_SIZE_MIN, drv_rpi4_mmal_get32(data, DRV_RPI4_MMAL_CHANGED_SIZE_MIN));
	drv_rpi4_mmal_put32(port, DRV_RPI4_MMAL_PORT_BUFFER_NUM_MIN, drv_rpi4_mmal_get32(data, DRV_RPI4_MMAL_CHANGED_NUM_MIN));
	drv_rpi4_mmal_put32(port, DRV_RPI4_MMAL_PORT_BUFFER_SIZE_RECOMMENDED, drv_rpi4_mmal_get32(data, DRV_RPI4_MMAL_CHANGED_SIZE_RECOMMENDED));
	drv_rpi4_mmal_put32(port, DRV_RPI4_MMAL_PORT_BUFFER_NUM_RECOMMENDED, drv_rpi4_mmal_get32(data, DRV_RPI4_MMAL_CHANGED_NUM_RECOMMENDED));
	error = port_set(decoder, decoder->output_info);
	if (error != 0)
		return error;

	/* Absorbs the buffers the disable gave back before reusing their memory. */
	error = pump(decoder);
	if (error != 0)
		return error;

	/* Enables the output again with buffers of the new size. */
	error = enable_output(decoder);
	if (error != 0)
		return error;

	/* Succeeded: frames of the new format follow. */
	kern_logf("rpi4-mmal: decoder %u output %08x %ux%u (crop %ux%u), %u x %u bytes\n",
		  decoder->slot, (unsigned)decoder->encoding, (unsigned)decoder->width, (unsigned)decoder->height,
		  (unsigned)decoder->crop_width, (unsigned)decoder->crop_height,
		  (unsigned)decoder->output_count, (unsigned)decoder->output_size);
	return 0;
}

/*
 * Receives one bulk the firmware is about to send, and waits for it.
 *
 * frame says whether it is a frame (else the scratch's); cpu is the cached
 * view of physical, invalidated once the bytes are there, or NULL for a
 * target's run, which the CPU reaches only uncached.
 */
static int
receive_bulk(
	struct drv_rpi4_mmal_decoder *decoder,
	uint64_t physical,
	void *cpu,
	uint32_t bytes,
	bool frame)
{
	struct mmal_bulk_tag *tag;
	unsigned long enabled;
	int error;

	/* Marks the bulk outstanding before it can complete. */
	tag = &decoder->scratch_tag;
	if (frame)
		tag = &decoder->frame_tag;
	enabled = spin_lock_irqsave(&mmal_lock);

	if (frame) {
		decoder->frame_pending = true;
		decoder->frame_actual = 0;
	} else {
		decoder->scratch_pending = true;
	}

	spin_unlock_irqrestore(&mmal_lock, enabled);

	/* Traces the receive while the budget lasts. */
	if (mmal_arrival_left > 0) {
		mmal_arrival_left--;
		kern_logf("rpi4-mmal: bulk %u: receive %u bytes into the %s\n", decoder->slot, (unsigned)bytes, frame ? "frame" : "scratch");
	}

	/* Queues the receive without a message. */
	error = drv_rpi4_vchiq_send_bulk(mmal_service, NULL, 0, DRV_RPI4_VCHIQ_BULK_RECEIVE, physical, bytes, tag);
	if (error != 0) {
		enabled = spin_lock_irqsave(&mmal_lock);

		decoder->frame_pending = false;
		decoder->scratch_pending = false;

		spin_unlock_irqrestore(&mmal_lock, enabled);

		return error;
	}

	/* Waits for the firmware's transfer. */
	error = wait_bulk(decoder, frame);
	if (error != 0)
		return error;

	/*
	 * Drops every cache line of the range (a speculative read may have
	 * filled one while the VPU wrote RAM), so the CPU reads what came.  A
	 * target's run has no cached view to drop.
	 */
	if (cpu != NULL)
		kern_dcache_invalidate_range(cpu, bytes);

	/* Succeeded: the bytes are in memory. */
	return 0;
}

/* Waits for the outstanding frame or scratch bulk; a timeout leaves it outstanding. */
static int
wait_bulk(
	struct drv_rpi4_mmal_decoder *decoder,
	bool frame)
{
	unsigned long enabled;
	uint64_t deadline;
	uint64_t observed;
	bool pending;
	int32_t actual;
	int error;

	/* Sleeps until the completion or the deadline. */
	deadline = sched_ticks() + kern_ms_to_ticks(MMAL_WAIT_MS) + 1U;
	error = 0;
	enabled = spin_lock_irqsave(&mmal_lock);

	for (;;) {
		pending = decoder->scratch_pending;
		if (frame)
			pending = decoder->frame_pending;
		if (!pending)
			break;
		observed = waitq_sequence(&mmal_wake);
		error = waitq_sleep(&mmal_wake, &mmal_lock, observed, deadline, 0);
		if (error == ETIMEDOUT)
			break;
		error = 0;
	}

	/* The transfer's actual length, for a frame. */
	actual = decoder->frame_actual;

	spin_unlock_irqrestore(&mmal_lock, enabled);

	/* A bulk that did not complete keeps its memory until the service closes. */
	if (error != 0) {
		kern_logf("rpi4-mmal: bulk not completed in %u ms; decoder stopped\n", MMAL_WAIT_MS);
		report_stall(decoder, frame ? "frame bulk" : "scratch bulk");
		decoder->broken = true;
		return error;
	}

	/* Traces the completion while the budget lasts. */
	if (mmal_arrival_left > 0) {
		mmal_arrival_left--;
		kern_logf("rpi4-mmal: bulk %u: %s received (%d)\n", decoder->slot, frame ? "frame" : "scratch", frame ? (int)actual : 0);
	}

	/* An aborted frame bulk is a lost frame. */
	if (frame && actual < 0)
		return EIO;

	/* Succeeded: the transfer completed. */
	return 0;
}

/* Sleeps until something is queued for the decoder or the deadline passes. */
static int
wait_queue_or(
	struct drv_rpi4_mmal_decoder *decoder,
	uint64_t deadline)
{
	unsigned long enabled;
	uint64_t observed;
	int error;

	/* Sleeps only while the queue is empty. */
	error = 0;
	enabled = spin_lock_irqsave(&mmal_lock);

	if (decoder->queue_count == 0 && !decoder->queue_overflow) {
		observed = waitq_sequence(&mmal_wake);
		error = waitq_sleep(&mmal_wake, &mmal_lock, observed, deadline, 0);
		if (error != ETIMEDOUT)
			error = 0;
		if (decoder->queue_count != 0)
			error = 0;
	}

	spin_unlock_irqrestore(&mmal_lock, enabled);

	/* Reports a deadline that passed with nothing queued. */
	if (error != 0)
		return error;

	/* Succeeded: the caller looks at the queue again. */
	return 0;
}

/* Allocates zeroed cached DMA memory in the VPU's window. */
static int
dma_alloc(
	size_t bytes,
	struct drv_dma_buffer *buffer)
{
	int error;

	/* Page-aligned memory below 1 GiB, reached through the cached direct map. */
	error = drv_dma_alloc_coherent(mmal_dma, bytes, 4096U, buffer);
	if (error != 0) {
		kern_memset(buffer, 0, sizeof(*buffer));
		kern_logf("rpi4-mmal: no %u bytes of DMA memory (%d)\n", (unsigned)bytes, error);
		return error;
	}

	/* The zeros reach RAM and leave the cache, so no dirty line can later land on what a bulk brought. */
	kern_memset(buffer->address, 0, bytes);
	kern_dcache_invalidate_range(buffer->address, bytes);

	/* Succeeded: the memory is zero, in RAM. */
	return 0;
}

/*
 * Frees DMA memory with no cache line of it left: a dirty line (the push's
 * scratch half of a staging area is never cleaned) written back later
 * would land on the memory's next owner, which may not use the cache.
 */
static void
dma_free(
	struct drv_dma_buffer *buffer)
{
	/* Writes back and drops every line, then gives the memory back. */
	kern_dcache_invalidate_range(buffer->address, buffer->size);
	drv_dma_free_coherent(mmal_dma, buffer);
}

/* Converts a bus address in the VPU's window to the physical address. */
static uint64_t
bus_to_physical(
	uint64_t bus)
{
	/* The window maps physical zero at MMAL_BUS_RAM. */
	return bus - MMAL_BUS_RAM;
}

/* Makes the client's name for a decoder's buffer or component. */
static uint32_t
token(
	struct drv_rpi4_mmal_decoder *decoder,
	uint32_t kind,
	uint32_t index)
{
	/* The tag, the decoder's slot, the kind and the index. */
	return MMAL_TOKEN | ((uint32_t)decoder->slot << 16) | (kind << 12) | (index & 0xfffU);
}

/* Finds the decoder a name belongs to, or NULL. */
static struct drv_rpi4_mmal_decoder *
decoder_of(
	uint32_t value)
{
	uint32_t slot;

	/* Only the client's names carry the tag. */
	if ((value & MMAL_TOKEN_MASK) != MMAL_TOKEN)
		return NULL;
	slot = (value >> 16) & 0xffU;
	if (slot >= MMAL_DECODERS)
		return NULL;

	/* A free or retired entry takes no arrivals. */
	if (!mmal_decoders[slot].used)
		return NULL;

	/* Succeeded: the decoder of the slot. */
	return &mmal_decoders[slot];
}

/* Reports the larger of two values. */
static uint32_t
maximum(
	uint32_t first,
	uint32_t second)
{
	/* The first unless the second is larger. */
	if (second > first)
		return second;

	/* Succeeded: the first is the larger. */
	return first;
}

/* Frees a decoder's DMA memory; nothing of it may be outstanding. */
static void
release_memory(
	struct drv_rpi4_mmal_decoder *decoder)
{
	unsigned index;

	/* The staging areas. */
	for (index = 0; index < MMAL_STAGING_AREAS; index++) {
		if (decoder->areas[index].memory.address != NULL)
			dma_free(&decoder->areas[index].memory);
	}

	/* The scratch and the bounce buffers. */
	if (decoder->scratch.address != NULL)
		dma_free(&decoder->scratch);
	for (index = 0; index < MMAL_OUTPUT_BUFFERS; index++) {
		if (decoder->outputs[index].memory.address != NULL)
			dma_free(&decoder->outputs[index].memory);
	}
}

/* Reports whether any bulk of the decoder may still move memory. */
static bool
bulks_outstanding(
	struct drv_rpi4_mmal_decoder *decoder)
{
	unsigned long enabled;
	unsigned index;
	bool outstanding;

	/* The staging bulks and the frame and scratch receives. */
	enabled = spin_lock_irqsave(&mmal_lock);

	outstanding = false;
	for (index = 0; index < MMAL_STAGING_AREAS; index++) {
		if (decoder->areas[index].bulks != 0)
			outstanding = true;
	}

	/* The frame and scratch receives. */
	if (decoder->frame_pending || decoder->scratch_pending)
		outstanding = true;

	spin_unlock_irqrestore(&mmal_lock, enabled);

	/* Succeeded: whether memory is still the firmware's. */
	return outstanding;
}

/* Names a worker request for the log. */
static const char *
request_name(
	uint32_t msgid)
{
	/* The requests this client sends. */
	switch (msgid) {
	case DRV_RPI4_MMAL_MSG_GET_VERSION:
		return "GET_VERSION";
	case DRV_RPI4_MMAL_MSG_COMPONENT_CREATE:
		return "COMPONENT_CREATE";
	case DRV_RPI4_MMAL_MSG_COMPONENT_DESTROY:
		return "COMPONENT_DESTROY";
	case DRV_RPI4_MMAL_MSG_COMPONENT_ENABLE:
		return "COMPONENT_ENABLE";
	case DRV_RPI4_MMAL_MSG_COMPONENT_DISABLE:
		return "COMPONENT_DISABLE";
	case DRV_RPI4_MMAL_MSG_PORT_INFO_GET:
		return "PORT_INFO_GET";
	case DRV_RPI4_MMAL_MSG_PORT_INFO_SET:
		return "PORT_INFO_SET";
	case DRV_RPI4_MMAL_MSG_PORT_ACTION:
		return "PORT_ACTION";
	case DRV_RPI4_MMAL_MSG_PORT_PARAMETER_SET:
		return "PORT_PARAMETER_SET";
	case DRV_RPI4_MMAL_MSG_PORT_FLUSH:
		return "PORT_FLUSH";
	default:
		return "?";
	}
}

/* Names an MMAL status (MMAL_STATUS_T, the userland's mmal_types.h) for the log. */
static const char *
status_name(
	uint32_t status)
{
	/* The statuses in their order. */
	static const char *const names[16] = {
		"SUCCESS", "ENOMEM", "ENOSPC", "EINVAL", "ENOSYS", "ENOENT", "ENXIO", "EIO",
		"ESPIPE", "ECORRUPT", "ENOTREADY", "ECONFIG", "EISCONN", "ENOTCONN", "EAGAIN", "EFAULT"
	};

	/* Any other value is unknown. */
	if (status >= 16U)
		return "?";

	/* Succeeded: the status's name. */
	return names[status];
}

/*
 * Makes the video floor of the core clock match the decoders: held while
 * an entry is in use, given back when none is.  The caller holds
 * mmal_open_lock.  A refused change is logged and leaves the floor as it
 * was; the decoder works at the clock the firmware runs.
 */
static void
core_floor(void)
{
	const char *what;
	unsigned index;
	uint32_t hz;
	bool wanted;
	int error;

	/* Whether any decoder entry is in use. */
	wanted = false;
	for (index = 0; index < MMAL_DECODERS; index++) {
		if (mmal_decoders[index].used) {
			wanted = true;
			break;
		}
	}

	/* Nothing to change. */
	if (wanted == mmal_floor_held)
		return;

	/* The highest rate while decoders exist; no floor afterwards. */
	hz = 0U;
	what = "given back";
	if (wanted) {
		hz = DRV_RPI4_FIRMWARE_CORE_MAXIMUM;
		what = "taken";
	}

	/* Asks the firmware through the floors of the core clock. */
	error = drv_rpi4_firmware_core_clock(DRV_RPI4_FIRMWARE_CORE_VIDEO, hz);
	if (error != 0) {
		kern_logf("rpi4-mmal: core clock floor %s refused (%d)\n", what, error);
		return;
	}

	/* Succeeded: the floor matches the decoders. */
	mmal_floor_held = wanted;
	return;
}
