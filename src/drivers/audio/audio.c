/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Audio registration, the /dev/dspN ring and the /dev/mixerN volume node.
 */

#include <drivers/audio/audio.h>
#include <drivers/generic/dma.h>
#include <kern/cdev.h>
#include <kern/clock.h>
#include <kern/file.h>
#include <kern/kcrt.h>
#include <kern/kmem.h>
#include <kern/lock.h>
#include <kern/page.h>
#include <kern/pmem.h>
#include <kern/poll.h>
#include <kern/sched.h>
#include <kern/uaccess.h>
#include <kern/vm-device.h>
#include <kern/waitq.h>
#include <uapi/errno.h>
#include <uapi/fcntl.h>

/* The first dev_t of the audio major; dspN is even and mixerN odd. */
#define AUDIO_DEVICE_BASE		0x000A0000U

/* The largest device number whose two dev_t values stay in the major. */
#define AUDIO_DEVICE_NUMBER_MAX		0x7FFFU

/* The bytes the hardware moves between two interrupts. */
#define AUDIO_FRAGMENT_BYTES		4096U

/* The fragments in one ring, about 170 ms at 48 kHz, S16 and two channels. */
#define AUDIO_FRAGMENT_COUNT		8U

/* The size of one ring. */
#define AUDIO_RING_BYTES		(AUDIO_FRAGMENT_BYTES * AUDIO_FRAGMENT_COUNT)

/* The ring alignment, which covers the HDA 128-byte buffer rule. */
#define AUDIO_RING_ALIGNMENT		4096U

/* The longest a final close waits for written sound to play. */
#define AUDIO_CLOSE_DRAIN_MS		2000U

/* The slack a DRAIN request allows beyond the time its data needs. */
#define AUDIO_DRAIN_MARGIN_MS		2000U

/*
 * The silent fragments a drain lets the hardware fetch after the last
 * written byte.  The DMA position runs ahead of the sound: a controller
 * FIFO, or QEMU's codec buffer of 8 KiB, still holds data the DMA has
 * already fetched, and stopping at the DMA position cuts that tail off.
 */
#define AUDIO_DRAIN_TAIL_FRAGMENTS	2U

/*
 * The fragments a mapped playback stream copies ahead of the hardware.
 * The DMA fetch runs ahead of the sound (see the drain tail), so the
 * fragment after the one being fetched is the nearest safe to fill.
 */
#define AUDIO_MMAP_LEAD_FRAGMENTS	2U

/* The stream index of each direction. */
#define AUDIO_PLAYBACK			0
#define AUDIO_CAPTURE			1

/*
 * One direction of the single open of /dev/dspN.
 *
 * Positions are monotonic 64-bit byte counts since the stream last
 * started, so a reader that falls a whole ring behind is still measured
 * correctly.  The direction mutex serializes read, write and the
 * requests that start or stop the stream; the device spinlock guards
 * every position, counter and flag below it.
 */
struct audio_stream {
	struct mutex lock;
	struct drv_dma_buffer buffer;
	unsigned allocated;
	unsigned running;
	unsigned draining;
	unsigned underrun_active;
	unsigned clearing;
	uint64_t epoch;
	uint64_t hardware;
	uint64_t application;
	uint64_t released;
	uint64_t interrupts;
	uint64_t transferred_before;
	uint32_t underruns;
	uint32_t overruns;

	/*
	 * A mapped direction moves through the shadow ring the caller maps
	 * instead of read or write.  frontier is the byte position, since the
	 * trigger, up to which the shadow and the DMA ring agree: playback
	 * copied the shadow there and silenced it, capture copied the DMA ring
	 * out.  reported counts the fragments a position request has seen.
	 */
	unsigned mapped;
	struct kern_pmem shadow;
	uint8_t *shadow_address;
	uint64_t frontier;
	uint64_t reported;
};

/*
 * One registered audio function and its two nodes.
 *
 * Registration holds one reference and each published cdev generation
 * holds another, so the structure outlives an unregister while an inode
 * still names it.  The backend and its DMA owner are borrowed only while
 * online is set.
 */
struct drv_audio_device {
	struct drv_audio_device *next;
	const struct drv_audio_ops *ops;
	void *private_data;
	unsigned number;
	refcount_t references;
	struct spinlock lock;
	struct wait_queue waitq;
	struct drv_dma_device *dma;
	struct cdev *dsp_node;
	struct cdev *mixer_node;
	unsigned online;
	unsigned dsp_open;
	unsigned playback_open;
	unsigned capture_open;
	unsigned mixer_calls;
	struct audio_format format;
	struct audio_stream streams[2];
};

/*
 * The registered devices, kept to hand out the lowest free number.
 *
 * Register links a device before publishing it and unregister unlinks it
 * after withdrawing both nodes; audio_registry_lock guards the list.
 */
static struct drv_audio_device *audio_devices;

/* Guards audio_devices. */
static struct spinlock audio_registry_lock = {
	{ 0 }, LOCK_RANK_DEVICE, "audio registry", 0, 0
};

/*
 * Forward declaration.
 */
static int audio_ops_validate(const struct drv_audio_ops *ops);
static int audio_format_valid(const struct audio_format *format);
static uint32_t audio_bytes_per_frame(const struct audio_format *format);
static uint64_t audio_align_up(uint64_t value, uint32_t unit);
static int audio_number_reserve(struct drv_audio_device *device);
static void audio_number_release(struct drv_audio_device *device);
static int audio_publish_node(struct drv_audio_device *device, int mixer);
static void audio_device_release(void *argument);
static void audio_stream_init(struct audio_stream *stream);
static void audio_stream_sync_locked(struct drv_audio_device *device, int capture);
static uint32_t audio_playback_room_locked(struct drv_audio_device *device);
static uint32_t audio_capture_available_locked(struct drv_audio_device *device);
static int audio_stream_allocate(struct drv_audio_device *device, int capture);
static void audio_stream_free(struct drv_audio_device *device, int capture);
static int audio_stream_start(struct drv_audio_device *device, int capture);
static void audio_stream_stop(struct drv_audio_device *device, int capture);
static int audio_playback_drain(struct drv_audio_device *device, int closing);
static void audio_ring_copy_in(struct audio_stream *stream, uint64_t total, const void *source, uint32_t bytes);
static void audio_ring_copy_out(struct audio_stream *stream, uint64_t total, void *destination, uint32_t bytes);
static void audio_ring_clear(struct audio_stream *stream, uint64_t from, uint64_t to);
static ssize_t audio_playback_write(struct drv_audio_device *device, const uint8_t *source, size_t size, int nonblocking);
static ssize_t audio_capture_read(struct drv_audio_device *device, uint8_t *destination, size_t size, int nonblocking);
static int audio_space_get(struct drv_audio_device *device, int capture, uintptr_t argument);
static int audio_format_set(struct drv_audio_device *device, uintptr_t argument);
static int audio_flush(struct drv_audio_device *device);
static int audio_dsp_open(struct file *file);
static int audio_dsp_close(struct file *file);
static ssize_t audio_dsp_read(struct file *file, void *buffer, size_t size);
static ssize_t audio_dsp_write(struct file *file, const void *buffer, size_t size);
static int audio_dsp_ioctl(struct file *file, unsigned long command, uintptr_t argument);
static int audio_dsp_poll(struct file *file, short events, short *revents);
static int audio_dsp_mmap(struct file *file, off_t offset, size_t bytes, uint32_t prot, struct vm_device_mapping **result);
static void audio_mapping_release(void *owner);
static int audio_shadow_allocate(struct audio_stream *stream);
static void audio_mmap_advance_locked(struct drv_audio_device *device, int capture);
static int audio_mmap_start(struct drv_audio_device *device, int capture);
static int audio_trigger(struct drv_audio_device *device, uintptr_t argument);
static int audio_mmap_position_get(struct drv_audio_device *device, int capture, uintptr_t argument);
static int audio_caps_get(uintptr_t argument);
static int audio_mixer_open(struct file *file);
static int audio_mixer_close(struct file *file);
static int audio_mixer_ioctl(struct file *file, unsigned long command, uintptr_t argument);
static int audio_mixer_enter(struct drv_audio_device *device);
static void audio_mixer_leave(struct drv_audio_device *device);

/*
 * Registers one audio function and publishes /dev/dspN and /dev/mixerN.
 *
 * The ops table, its formats, private_data and the DMA owner stay
 * borrowed until a successful drv_audio_unregister().
 */
int
drv_audio_register(
	const struct drv_audio_ops *ops,
	void *private_data,
	struct drv_dma_device *dma,
	struct drv_audio_device **result)
{
	struct drv_audio_device *device;
	unsigned long irq;
	int error;

	/* Refuses a missing handle destination, DMA owner or an incomplete backend. */
	if (result == NULL || dma == NULL)
		return EINVAL;

	error = audio_ops_validate(ops);
	if (error != 0)
		return error;

	/* Allocates the device wrapper. */
	device = kern_calloc(1, sizeof(*device));
	if (device == NULL)
		return ENOMEM;

	/* Initializes an offline device that holds only its registration reference. */
	device->ops = ops;
	device->private_data = private_data;
	device->dma = dma;
	refcount_init(&device->references, 1U);
	spin_init(&device->lock, LOCK_RANK_DEVICE, "audio device");
	waitq_init(&device->waitq, "audio stream");
	device->format = ops->formats[0];
	audio_stream_init(&device->streams[AUDIO_PLAYBACK]);
	audio_stream_init(&device->streams[AUDIO_CAPTURE]);

	/* Takes the lowest free device number. */
	error = audio_number_reserve(device);
	if (error != 0) {
		kern_free(device);
		return error;
	}

	/* An open that finds a published node must also find the device online. */
	device->online = 1U;

	/* Publishes the stream node. */
	error = audio_publish_node(device, 0);
	if (error != 0) {
		audio_number_release(device);
		audio_device_release(device);
		return error;
	}

	/* Publishes the volume node, withdrawing the stream node if it cannot. */
	error = audio_publish_node(device, 1);
	if (error != 0) {
		irq = spin_lock_irqsave(&device->lock);
		device->online = 0U;
		spin_unlock_irqrestore(&device->lock, irq);

		(void)cdev_unregister(device->dsp_node);
		cdev_release(device->dsp_node);
		audio_number_release(device);
		audio_device_release(device);
		return error;
	}

	/* Succeeded: the caller owns the handle until a successful unregister. */
	*result = device;
	return 0;
}

/*
 * Withdraws /dev/dspN and /dev/mixerN and consumes the handle.
 *
 * EBUSY leaves the device registered while the stream node is open or a
 * mixer request is running; the caller retries later.
 */
int
drv_audio_unregister(
	struct drv_audio_device *device)
{
	unsigned long irq;
	int busy;

	/* Refuses a missing handle. */
	if (device == NULL)
		return EINVAL;

	/* Takes the device offline unless an open stream or a volume call still uses the backend. */
	busy = 0;
	irq = spin_lock_irqsave(&device->lock);

	if (device->dsp_open != 0U || device->mixer_calls != 0U)
		busy = 1;
	else
		device->online = 0U;

	spin_unlock_irqrestore(&device->lock, irq);

	/* Reports that the backend is still in use. */
	if (busy != 0)
		return EBUSY;

	/* Withdraws both names; a stale inode keeps only the wrapper alive. */
	(void)cdev_unregister(device->dsp_node);
	cdev_release(device->dsp_node);
	(void)cdev_unregister(device->mixer_node);
	cdev_release(device->mixer_node);

	/* Returns the number; every close already freed its ring to the backend's DMA owner. */
	audio_number_release(device);
	device->dma = NULL;

	/* Drops the registration reference. */
	audio_device_release(device);

	/* Succeeded: the backend is no longer borrowed. */
	return 0;
}

/*
 * Accounts one completed fragment reported by the interrupt handler.
 *
 * The consumed playback range is cleared to silence so that the hardware
 * plays silence, not stale sound, if the application falls behind.
 */
void
drv_audio_interrupt(
	struct drv_audio_device *device,
	int capture)
{
	struct audio_stream *stream;
	unsigned long irq;
	uint64_t from;
	uint64_t to;
	uint64_t epoch;
	int clear;

	/* Selects the stream the fragment belongs to. */
	if (device == NULL)
		return;

	stream = &device->streams[AUDIO_PLAYBACK];
	if (capture != 0)
		stream = &device->streams[AUDIO_CAPTURE];

	/* Counts the fragment, advances the position and claims the consumed range. */
	clear = 0;
	from = 0;
	to = 0;
	epoch = 0;
	irq = spin_lock_irqsave(&device->lock);

	if (stream->running == 0U) {
		spin_unlock_irqrestore(&device->lock, irq);
		return;
	}

	/* The interrupt count is the primary measure of hardware progress. */
	stream->interrupts++;
	audio_stream_sync_locked(device, capture);

	/*
	 * A mapped direction moves whole fragments between the shadow and
	 * the DMA ring here.  Each playback fragment is copied from the
	 * shadow in full, so the DMA ring needs no separate silencing.
	 */
	if (stream->mapped != 0U) {
		audio_mmap_advance_locked(device, capture);
		if (capture == 0)
			stream->released = stream->hardware;
	}

	/*
	 * Clearing marks the consumed range as owned by this handler.  A writer
	 * sees room only up to released, so it cannot race the clear.
	 */
	if (capture == 0 &&
	    stream->mapped == 0U &&
	    stream->clearing == 0U &&
	    stream->hardware > stream->released) {
		from = stream->released;
		to = stream->hardware;
		if (to - from > AUDIO_RING_BYTES)
			from = to - AUDIO_RING_BYTES;

		stream->clearing = 1U;
		epoch = stream->epoch;
		clear = 1;
	}

	waitq_wake_all(&device->waitq);

	spin_unlock_irqrestore(&device->lock, irq);

	/* Silences the consumed range and returns it to writers. */
	if (clear != 0) {
		audio_ring_clear(stream, from, to);

		irq = spin_lock_irqsave(&device->lock);

		/* A stop during the clear has already reset every position. */
		stream->clearing = 0U;
		if (stream->epoch == epoch)
			stream->released = to;

		waitq_wake_all(&device->waitq);

		spin_unlock_irqrestore(&device->lock, irq);
	}

	/* Wakes poll callers waiting for room or data. */
	poll_notify();
}

/* Checks that a backend supplies every required operation and valid formats. */
static int
audio_ops_validate(
	const struct drv_audio_ops *ops)
{
	unsigned index;
	int valid;

	/* Refuses a missing table or a function with no direction. */
	if (ops == NULL)
		return EINVAL;
	if (ops->playback == 0U && ops->capture == 0U)
		return EINVAL;

	/* Refuses a backend that cannot run a ring. */
	if (ops->prepare == NULL || ops->start == NULL)
		return EINVAL;
	if (ops->stop == NULL || ops->position == NULL)
		return EINVAL;

	/* Refuses an empty format list. */
	if (ops->formats == NULL || ops->format_count == 0U)
		return EINVAL;

	/* Every advertised format must be one the framework can frame. */
	for (index = 0; index < ops->format_count; index++) {
		valid = audio_format_valid(&ops->formats[index]);
		if (valid == 0)
			return EINVAL;
	}

	/* Succeeded: the backend can be registered. */
	return 0;
}

/* Reports whether a format is one the framework can frame. */
static int
audio_format_valid(
	const struct audio_format *format)
{
	/* Refuses an unknown encoding. */
	if (format->format != KERN_AUDIO_FORMAT_S16_LE &&
	    format->format != KERN_AUDIO_FORMAT_S32_LE)
		return 0;

	/* Refuses anything but mono or stereo. */
	if (format->channels != 1U && format->channels != 2U)
		return 0;

	/* Refuses a zero rate or a set reserved field. */
	if (format->rate == 0U || format->reserved != 0U)
		return 0;

	/* Succeeded: the format frames evenly into every fragment. */
	return 1;
}

/* Reports the bytes in one frame of a valid format. */
static uint32_t
audio_bytes_per_frame(
	const struct audio_format *format)
{
	/* A 32-bit sample takes four bytes per channel. */
	if (format->format == KERN_AUDIO_FORMAT_S32_LE)
		return 4U * format->channels;

	/* A 16-bit sample takes two bytes per channel. */
	return 2U * format->channels;
}

/*
 * Rounds a byte position up to a whole frame.
 *
 * Every frame is 2, 4 or 8 bytes, a power of two, so the remainder is a
 * mask; a 64-bit division would need a libgcc helper on i386.
 */
static uint64_t
audio_align_up(
	uint64_t value,
	uint32_t unit)
{
	uint32_t remainder;

	/* A position already on a frame boundary stays put. */
	remainder = (uint32_t)value & (unit - 1U);
	if (remainder == 0)
		return value;

	/* Reports the next frame boundary. */
	return value + (unit - remainder);
}

/* Links a device under the lowest number no other device uses. */
static int
audio_number_reserve(
	struct drv_audio_device *device)
{
	struct drv_audio_device *other;
	unsigned long irq;
	unsigned number;
	int used;

	/* Tries each number from zero until one is free. */
	irq = spin_lock_irqsave(&audio_registry_lock);

	for (number = 0; number <= AUDIO_DEVICE_NUMBER_MAX; number++) {
		used = 0;
		for (other = audio_devices; other != NULL; other = other->next) {
			if (other->number == number) {
				used = 1;
				break;
			}
		}

		if (used == 0)
			break;
	}

	if (number > AUDIO_DEVICE_NUMBER_MAX) {
		spin_unlock_irqrestore(&audio_registry_lock, irq);
		return ENOSPC;
	}

	device->number = number;
	device->next = audio_devices;
	audio_devices = device;

	spin_unlock_irqrestore(&audio_registry_lock, irq);

	/* Succeeded: the number belongs to this device until release. */
	return 0;
}

/* Unlinks a device so that its number can be reused. */
static void
audio_number_release(
	struct drv_audio_device *device)
{
	struct drv_audio_device **link;
	unsigned long irq;

	/* Removes the device from the registry list. */
	irq = spin_lock_irqsave(&audio_registry_lock);

	for (link = &audio_devices; *link != NULL; link = &(*link)->next) {
		if (*link == device) {
			*link = device->next;
			break;
		}
	}

	spin_unlock_irqrestore(&audio_registry_lock, irq);

	/* Succeeded: the number is free. */
	return;
}

/* Publishes one node whose generation retains the device wrapper. */
static int
audio_publish_node(
	struct drv_audio_device *device,
	int mixer)
{
	/* Stream-node dispatch shared by every published generation. */
	static const struct cdev_ops audio_dsp_operations = {
		.open = audio_dsp_open,
		.close = audio_dsp_close,
		.read = audio_dsp_read,
		.write = audio_dsp_write,
		.ioctl = audio_dsp_ioctl,
		.poll = audio_dsp_poll,
		.mmap = audio_dsp_mmap
	};

	/* Volume-node dispatch shared by every published generation. */
	static const struct cdev_ops audio_mixer_operations = {
		.open = audio_mixer_open,
		.close = audio_mixer_close,
		.ioctl = audio_mixer_ioctl
	};
	const struct cdev_ops *operations;
	struct cdev *node;
	char name[32];
	dev_t rdev;
	int error;

	/* Names the node and picks its dev_t within the audio major. */
	if (mixer != 0) {
		kern_snprintf(name, sizeof(name), "mixer%u", device->number);
		rdev = (dev_t)(AUDIO_DEVICE_BASE + device->number * 2U + 1U);
		operations = &audio_mixer_operations;
	} else {
		kern_snprintf(name, sizeof(name), "dsp%u", device->number);
		rdev = (dev_t)(AUDIO_DEVICE_BASE + device->number * 2U);
		operations = &audio_dsp_operations;
	}

	/* The generation keeps the wrapper alive until its finalizer runs. */
	refcount_get(&device->references);
	error = cdev_register_managed(name,
	    rdev,
	    operations,
	    device,
	    audio_device_release,
	    &node);
	if (error != 0) {
		audio_device_release(device);
		return error;
	}

	/* Keeps the caller reference for the matching unregister. */
	if (mixer != 0)
		device->mixer_node = node;
	else
		device->dsp_node = node;

	/* Succeeded: the node is visible in /dev. */
	return 0;
}

/* Frees the wrapper after registration and every node generation let it go. */
static void
audio_device_release(
	void *argument)
{
	struct drv_audio_device *device;
	int last;

	/* Drops one reference and frees the wrapper with the last. */
	device = argument;
	last = refcount_put(&device->references);
	if (last != 0)
		kern_free(device);

	/* Succeeded: this reference no longer retains the wrapper. */
	return;
}

/* Initializes an idle stream with no ring. */
static void
audio_stream_init(
	struct audio_stream *stream)
{
	/* Prepares the direction lock; every position starts at zero. */
	(void)mutex_init(&stream->lock, LOCK_RANK_DEVICE, "audio direction");

	/* Succeeded: the stream is idle. */
	return;
}

/*
 * Brings a running stream's hardware position up to date.
 *
 * The interrupt count gives a floor of whole fragments and position()
 * fills in the part of the fragment in progress.  A position that seems
 * to trail the floor by less than a fragment is the hardware reporting
 * just before the interrupt it already raised, not a whole ring ahead.
 * A position further ahead than the floor means missed interrupts, and
 * that distance is counted.  The device lock is held.
 */
static void
audio_stream_sync_locked(
	struct drv_audio_device *device,
	int capture)
{
	struct audio_stream *stream;
	uint64_t floor;
	uint64_t measured;
	uint32_t bytes_per_frame;
	uint32_t offset;
	uint32_t lead;

	/* An idle stream has no hardware to measure. */
	stream = &device->streams[capture];
	if (stream->running == 0U)
		return;

	/* Reads where the hardware is in the ring. */
	offset = device->ops->position(device->private_data, capture);
	offset %= AUDIO_RING_BYTES;

	/* Places that offset relative to the interrupt floor. */
	floor = stream->interrupts * AUDIO_FRAGMENT_BYTES;
	lead = (uint32_t)((offset + AUDIO_RING_BYTES -
	    (uint32_t)(floor % AUDIO_RING_BYTES)) % AUDIO_RING_BYTES);
	measured = floor;
	if (lead < AUDIO_RING_BYTES - AUDIO_FRAGMENT_BYTES)
		measured += lead;

	/* The position only moves forward. */
	if (measured > stream->hardware)
		stream->hardware = measured;

	/* A mapped direction has no write or read position to account. */
	if (stream->mapped != 0U)
		return;

	/* A moved position must stay on a frame boundary. */
	bytes_per_frame = audio_bytes_per_frame(&device->format);

	/*
	 * Playback: when the hardware reaches the write position the ring is
	 * empty and it plays silence.  One underrun is counted per episode and
	 * the write position follows the hardware so the next write plays
	 * soon.  A drain is expected to run dry and counts nothing.
	 */
	if (capture == 0) {
		if (stream->hardware < stream->application) {
			stream->underrun_active = 0U;
		} else if (stream->draining == 0U) {
			if (stream->underrun_active == 0U) {
				stream->underruns++;
				stream->underrun_active = 1U;
			}

			stream->application =
				audio_align_up(stream->hardware, bytes_per_frame);
		}

		return;
	}

	/*
	 * Capture: once the hardware is within a fragment of overwriting the
	 * oldest unread byte, the reader has fallen too far behind.  It skips
	 * to half a ring behind the hardware and one overrun is counted.
	 */
	if (stream->hardware - stream->application >
	    AUDIO_RING_BYTES - AUDIO_FRAGMENT_BYTES) {
		stream->overruns++;
		stream->application = audio_align_up(
			stream->hardware - AUDIO_RING_BYTES / 2U,
			bytes_per_frame);
	}

	/* Succeeded: the stream's positions are current. */
	return;
}

/* Reports the whole frames a writer may copy now, with the device lock held. */
static uint32_t
audio_playback_room_locked(
	struct drv_audio_device *device)
{
	struct audio_stream *stream;
	uint64_t limit;
	uint32_t room;

	/* A writer may fill up to one ring past the silenced range. */
	stream = &device->streams[AUDIO_PLAYBACK];
	limit = stream->released + AUDIO_RING_BYTES;
	if (limit <= stream->application)
		return 0;

	/* Reports the room rounded down to whole frames. */
	room = (uint32_t)(limit - stream->application);
	room -= room % audio_bytes_per_frame(&device->format);
	return room;
}

/* Reports the whole frames a reader may copy now, with the device lock held. */
static uint32_t
audio_capture_available_locked(
	struct drv_audio_device *device)
{
	struct audio_stream *stream;
	uint32_t available;

	/* A synced stream never holds more than a ring less one fragment. */
	stream = &device->streams[AUDIO_CAPTURE];
	available = (uint32_t)(stream->hardware - stream->application);

	/* Reports the data rounded down to whole frames. */
	available -= available % audio_bytes_per_frame(&device->format);
	return available;
}

/* Allocates a silent ring for one direction on first use. */
static int
audio_stream_allocate(
	struct drv_audio_device *device,
	int capture)
{
	struct audio_stream *stream;
	int error;

	/* A ring already allocated for this open is reused. */
	stream = &device->streams[capture];
	if (stream->allocated != 0U)
		return 0;

	/* Allocates one coherent ring through the backend's DMA constraints. */
	error = drv_dma_alloc_coherent(device->dma,
	    AUDIO_RING_BYTES,
	    AUDIO_RING_ALIGNMENT,
	    &stream->buffer);
	if (error != 0)
		return error;

	/* A new ring plays silence until something is written. */
	kern_memset(stream->buffer.address, 0, AUDIO_RING_BYTES);
	stream->allocated = 1U;

	/* Succeeded: the direction owns a ring until close. */
	return 0;
}

/* Frees a stopped direction's ring. */
static void
audio_stream_free(
	struct drv_audio_device *device,
	int capture)
{
	struct audio_stream *stream;

	/* Returns the ring to the DMA owner. */
	stream = &device->streams[capture];
	if (stream->allocated != 0U) {
		drv_dma_free_coherent(device->dma, &stream->buffer);
		stream->allocated = 0U;
	}

	/*
	 * Returns the shadow of a mapped direction.  Every mapping retains
	 * the file, so none is left by the time close frees it.
	 */
	if (stream->shadow_address != NULL) {
		(void)kern_pmem_free(&stream->shadow);
		stream->shadow_address = NULL;
	}
	stream->mapped = 0U;

	/* Succeeded: the direction owns no ring. */
	return;
}

/* Binds the ring to the hardware and starts it moving. */
static int
audio_stream_start(
	struct drv_audio_device *device,
	int capture)
{
	struct audio_stream *stream;
	unsigned long irq;
	int error;

	/* Gives the backend the ring and the current format. */
	stream = &device->streams[capture];
	error = device->ops->prepare(device->private_data,
	    capture,
	    &device->format,
	    &stream->buffer,
	    AUDIO_FRAGMENT_BYTES,
	    AUDIO_FRAGMENT_COUNT);
	if (error != 0)
		return error;

	/* Marks the stream running so the first interrupt is counted. */
	irq = spin_lock_irqsave(&device->lock);

	stream->running = 1U;

	spin_unlock_irqrestore(&device->lock, irq);

	/* Starts the hardware. */
	error = device->ops->start(device->private_data, capture);
	if (error != 0) {
		irq = spin_lock_irqsave(&device->lock);
		stream->running = 0U;
		stream->epoch++;
		spin_unlock_irqrestore(&device->lock, irq);

		return error;
	}

	/* Succeeded: the hardware walks the ring. */
	return 0;
}

/*
 * Stops a direction and discards what it held.
 *
 * The stream is marked idle first so that a late interrupt changes
 * nothing, and the positions are reset only after the hardware has
 * stopped and no interrupt handler is still clearing the ring.
 */
static void
audio_stream_stop(
	struct drv_audio_device *device,
	int capture)
{
	struct audio_stream *stream;
	unsigned long irq;
	uint64_t observed;
	unsigned was_running;

	/* Marks the stream idle; the epoch tells a running clear its range is void. */
	stream = &device->streams[capture];
	irq = spin_lock_irqsave(&device->lock);

	was_running = stream->running;
	stream->running = 0U;
	stream->draining = 0U;
	stream->epoch++;

	spin_unlock_irqrestore(&device->lock, irq);

	/* Waits until the hardware no longer touches the ring. */
	if (was_running != 0U)
		device->ops->stop(device->private_data, capture);

	/* Waits out a clear in progress, then folds the run into the open's totals. */
	irq = spin_lock_irqsave(&device->lock);

	while (stream->clearing != 0U) {
		observed = waitq_sequence(&device->waitq);
		(void)waitq_sleep(&device->waitq, &device->lock, observed, 0, 0);
	}

	stream->transferred_before += stream->hardware;
	stream->hardware = 0;
	stream->application = 0;
	stream->released = 0;
	stream->interrupts = 0;
	stream->underrun_active = 0U;
	stream->frontier = 0;
	stream->reported = 0;
	waitq_wake_all(&device->waitq);

	spin_unlock_irqrestore(&device->lock, irq);

	/* Silences the ring so the next run starts from silence. */
	if (stream->allocated != 0U)
		kern_memset(stream->buffer.address, 0, AUDIO_RING_BYTES);

	/* Wakes poll callers; room has changed. */
	poll_notify();
}

/*
 * Waits until everything written has played, then stops playback.
 *
 * A stream that never reached half a ring is started here, so a short
 * sound still plays.  A final close waits at most two seconds and may not
 * be interrupted; a DRAIN request waits the time its data needs plus a
 * margin and may be interrupted, which leaves the stream playing.
 */
static int
audio_playback_drain(
	struct drv_audio_device *device,
	int closing)
{
	struct audio_stream *stream;
	unsigned long irq;
	uint64_t observed;
	uint64_t deadline;
	uint64_t pending;
	uint64_t target;
	uint64_t milliseconds;
	uint32_t byte_rate;
	unsigned flags;
	int error;

	/* Measures what is still to play, never more than one ring. */
	stream = &device->streams[AUDIO_PLAYBACK];
	irq = spin_lock_irqsave(&device->lock);

	audio_stream_sync_locked(device, AUDIO_PLAYBACK);
	pending = 0;
	if (stream->application > stream->hardware)
		pending = stream->application - stream->hardware;

	spin_unlock_irqrestore(&device->lock, irq);

	/* Nothing written means nothing to wait for. */
	if (pending == 0 && stream->running == 0U)
		return 0;

	/* Starts a stream that never filled half a ring. */
	if (stream->running == 0U) {
		error = audio_stream_start(device, AUDIO_PLAYBACK);
		if (error != 0) {
			audio_stream_stop(device, AUDIO_PLAYBACK);
			return error;
		}
	}

	/* Chooses the deadline and whether a signal may end the wait. */
	if (closing != 0) {
		milliseconds = AUDIO_CLOSE_DRAIN_MS;
		flags = 0;
	} else {
		byte_rate = device->format.rate *
			audio_bytes_per_frame(&device->format);
		milliseconds = ((uint32_t)pending +
		    AUDIO_DRAIN_TAIL_FRAGMENTS * AUDIO_FRAGMENT_BYTES) * 1000U / byte_rate;
		milliseconds += AUDIO_DRAIN_MARGIN_MS;
		flags = WAITQ_INTERRUPTIBLE;
	}

	deadline = sched_ticks() + kern_ms_to_ticks(milliseconds);

	/* Waits for the hardware to pass the last written byte and the silent tail. */
	error = 0;
	irq = spin_lock_irqsave(&device->lock);

	stream->draining = 1U;
	target = stream->application + AUDIO_DRAIN_TAIL_FRAGMENTS * AUDIO_FRAGMENT_BYTES;
	for (;;) {
		audio_stream_sync_locked(device, AUDIO_PLAYBACK);
		if (stream->hardware >= target)
			break;

		observed = waitq_sequence(&device->waitq);
		error = waitq_sleep(&device->waitq, &device->lock, observed,
		    deadline, flags);
		if (error == EAGAIN)
			error = 0;

		if (error != 0)
			break;
	}

	/* An interrupted drain leaves the stream playing as it was. */
	if (error == EINTR)
		stream->draining = 0U;

	spin_unlock_irqrestore(&device->lock, irq);

	/* Reports an interrupted wait without stopping. */
	if (error == EINTR)
		return EINTR;

	/* Stops the hardware whether it finished or ran out of time. */
	audio_stream_stop(device, AUDIO_PLAYBACK);

	/* Reports hardware that did not finish in time. */
	if (error != 0)
		return EIO;

	/* Succeeded: everything written has played. */
	return 0;
}

/* Copies bytes into the ring at a total position, wrapping at its end. */
static void
audio_ring_copy_in(
	struct audio_stream *stream,
	uint64_t total,
	const void *source,
	uint32_t bytes)
{
	uint8_t *ring;
	uint32_t offset;
	uint32_t first;

	/* Splits the copy where it wraps. */
	ring = stream->buffer.address;
	offset = (uint32_t)(total % AUDIO_RING_BYTES);
	first = AUDIO_RING_BYTES - offset;
	if (first > bytes)
		first = bytes;

	/* Copies the part before the end and then the part from the start. */
	kern_memcpy(ring + offset, source, first);
	if (bytes > first)
		kern_memcpy(ring, (const uint8_t *)source + first, bytes - first);
}

/* Copies bytes out of the ring at a total position, wrapping at its end. */
static void
audio_ring_copy_out(
	struct audio_stream *stream,
	uint64_t total,
	void *destination,
	uint32_t bytes)
{
	const uint8_t *ring;
	uint32_t offset;
	uint32_t first;

	/* Splits the copy where it wraps. */
	ring = stream->buffer.address;
	offset = (uint32_t)(total % AUDIO_RING_BYTES);
	first = AUDIO_RING_BYTES - offset;
	if (first > bytes)
		first = bytes;

	/* Copies the part before the end and then the part from the start. */
	kern_memcpy(destination, ring + offset, first);
	if (bytes > first)
		kern_memcpy((uint8_t *)destination + first, ring, bytes - first);
}

/* Silences the ring bytes of a range of total positions no longer needed. */
static void
audio_ring_clear(
	struct audio_stream *stream,
	uint64_t from,
	uint64_t to)
{
	uint8_t *ring;
	uint32_t offset;
	uint32_t bytes;
	uint32_t first;

	/* Splits the clear where it wraps. */
	ring = stream->buffer.address;
	offset = (uint32_t)(from % AUDIO_RING_BYTES);
	bytes = (uint32_t)(to - from);
	first = AUDIO_RING_BYTES - offset;
	if (first > bytes)
		first = bytes;

	/* Clears the part before the end and then the part from the start. */
	kern_memset(ring + offset, 0, first);
	if (bytes > first)
		kern_memset(ring, 0, bytes - first);
}

/*
 * Copies whole frames into the playback ring.
 *
 * The room is decided under the device lock and the copy runs without
 * it; the hardware and the interrupt handler never touch the range a
 * writer was given.  Playback starts once half the ring is filled.
 */
static ssize_t
audio_playback_write(
	struct drv_audio_device *device,
	const uint8_t *source,
	size_t size,
	int nonblocking)
{
	struct audio_stream *stream;
	unsigned long irq;
	uint64_t observed;
	uint64_t start_total;
	uint32_t bytes_per_frame;
	uint32_t room;
	uint32_t chunk;
	size_t done;
	int start_needed;
	int error;

	/* A partial frame would shift every later channel. */
	stream = &device->streams[AUDIO_PLAYBACK];
	bytes_per_frame = audio_bytes_per_frame(&device->format);
	if (size % bytes_per_frame != 0)
		return -EINVAL;

	/* Allocates the ring on the first write of this open. */
	error = audio_stream_allocate(device, AUDIO_PLAYBACK);
	if (error != 0)
		return -error;

	/* Copies until the request is done, blocking for room unless asked not to. */
	done = 0;
	while (done < size) {
		irq = spin_lock_irqsave(&device->lock);

		audio_stream_sync_locked(device, AUDIO_PLAYBACK);
		room = audio_playback_room_locked(device);

		/*
		 * A full ring with no hardware running would never drain, which
		 * happens after a failed start; start it now or report why not.
		 */
		if (room == 0 && stream->running == 0U) {
			spin_unlock_irqrestore(&device->lock, irq);
			error = audio_stream_start(device, AUDIO_PLAYBACK);
			if (error != 0)
				break;

			continue;
		}

		if (room == 0) {
			if (nonblocking != 0) {
				spin_unlock_irqrestore(&device->lock, irq);
				break;
			}

			observed = waitq_sequence(&device->waitq);
			error = waitq_sleep(&device->waitq, &device->lock, observed,
			    0, WAITQ_INTERRUPTIBLE);
			spin_unlock_irqrestore(&device->lock, irq);
			if (error == EINTR)
				break;

			error = 0;
			continue;
		}

		chunk = room;
		if (chunk > size - done)
			chunk = (uint32_t)(size - done);

		start_total = stream->application;

		spin_unlock_irqrestore(&device->lock, irq);

		/* Copies the chunk into room nothing else touches. */
		audio_ring_copy_in(stream, start_total, source + done, chunk);

		/*
		 * Publishes the chunk.  If an underrun moved the write position
		 * during the copy, the part the hardware already passed is lost
		 * and only the part ahead of it counts.
		 */
		irq = spin_lock_irqsave(&device->lock);

		if (stream->application < start_total + chunk)
			stream->application = start_total + chunk;
		if (stream->application > stream->hardware)
			stream->underrun_active = 0U;

		start_needed = 0;
		if (stream->running == 0U &&
		    stream->application >= AUDIO_RING_BYTES / 2U)
			start_needed = 1;

		spin_unlock_irqrestore(&device->lock, irq);

		done += chunk;

		/* Starts playback once half the ring is filled. */
		if (start_needed != 0) {
			error = audio_stream_start(device, AUDIO_PLAYBACK);
			if (error != 0)
				break;
		}
	}

	/* Reports the bytes accepted even when a later chunk failed. */
	if (done > 0)
		return (ssize_t)done;

	/* Reports why nothing was accepted. */
	if (error != 0)
		return -error;
	if (nonblocking != 0)
		return -EAGAIN;

	/* Succeeded: an empty request accepts nothing. */
	return 0;
}

/*
 * Copies whole captured frames out of the ring.
 *
 * The first read starts capture.  The copy runs without the device lock,
 * so the hardware may overwrite the range meanwhile; the position is
 * checked again afterwards and a copy the hardware overtook is discarded
 * and counted as an overrun rather than returned damaged.
 */
static ssize_t
audio_capture_read(
	struct drv_audio_device *device,
	uint8_t *destination,
	size_t size,
	int nonblocking)
{
	struct audio_stream *stream;
	unsigned long irq;
	uint64_t observed;
	uint64_t start_total;
	uint32_t bytes_per_frame;
	uint32_t available;
	uint32_t chunk;
	size_t done;
	int error;

	/* A partial frame would shift every later channel. */
	stream = &device->streams[AUDIO_CAPTURE];
	bytes_per_frame = audio_bytes_per_frame(&device->format);
	if (size % bytes_per_frame != 0)
		return -EINVAL;

	/* Allocates the ring on the first read of this open. */
	error = audio_stream_allocate(device, AUDIO_CAPTURE);
	if (error != 0)
		return -error;

	/* Starts capture on the first read after open or a flush. */
	if (stream->running == 0U) {
		error = audio_stream_start(device, AUDIO_CAPTURE);
		if (error != 0)
			return -error;
	}

	/* Copies until the request is done, blocking for data unless asked not to. */
	done = 0;
	while (done < size) {
		irq = spin_lock_irqsave(&device->lock);

		audio_stream_sync_locked(device, AUDIO_CAPTURE);
		available = audio_capture_available_locked(device);
		if (available == 0) {
			if (nonblocking != 0) {
				spin_unlock_irqrestore(&device->lock, irq);
				break;
			}

			observed = waitq_sequence(&device->waitq);
			error = waitq_sleep(&device->waitq, &device->lock, observed,
			    0, WAITQ_INTERRUPTIBLE);
			spin_unlock_irqrestore(&device->lock, irq);
			if (error == EINTR)
				break;

			error = 0;
			continue;
		}

		chunk = available;
		if (chunk > size - done)
			chunk = (uint32_t)(size - done);

		start_total = stream->application;

		spin_unlock_irqrestore(&device->lock, irq);

		/* Copies the chunk while the hardware keeps writing elsewhere. */
		audio_ring_copy_out(stream, start_total, destination + done, chunk);

		/* Keeps the chunk only if no overrun moved the reader during the copy. */
		irq = spin_lock_irqsave(&device->lock);

		audio_stream_sync_locked(device, AUDIO_CAPTURE);
		if (stream->application != start_total) {
			spin_unlock_irqrestore(&device->lock, irq);
			continue;
		}

		stream->application += chunk;

		spin_unlock_irqrestore(&device->lock, irq);

		done += chunk;
	}

	/* Reports the bytes delivered even when a later wait ended early. */
	if (done > 0)
		return (ssize_t)done;

	/* Reports why nothing was delivered. */
	if (error != 0)
		return -error;
	if (nonblocking != 0)
		return -EAGAIN;

	/* Succeeded: an empty request delivers nothing. */
	return 0;
}

/* Copies the room or data of one direction to the caller. */
static int
audio_space_get(
	struct drv_audio_device *device,
	int capture,
	uintptr_t argument)
{
	struct audio_stream *stream;
	struct audio_space space;
	unsigned long irq;
	int error;

	/* Samples the direction's position and counters. */
	stream = &device->streams[capture];
	kern_memset(&space, 0, sizeof(space));
	irq = spin_lock_irqsave(&device->lock);

	audio_stream_sync_locked(device, capture);
	if (capture != 0)
		space.bytes = audio_capture_available_locked(device);
	else
		space.bytes = audio_playback_room_locked(device);

	space.transferred = stream->transferred_before + stream->hardware;
	space.underruns = stream->underruns;
	space.overruns = stream->overruns;

	spin_unlock_irqrestore(&device->lock, irq);

	/* Expresses the bytes in whole fragments as well. */
	space.fragment_bytes = AUDIO_FRAGMENT_BYTES;
	space.fragments = space.bytes / AUDIO_FRAGMENT_BYTES;

	/* Publishes the snapshot. */
	error = copyout(&space, argument, sizeof(space));
	if (error != 0)
		return error;

	/* Succeeded: the caller has the snapshot. */
	return 0;
}

/*
 * Accepts a format only when the hardware lists exactly that format.
 *
 * On a mismatch the current format is returned with EINVAL, so the caller
 * sees what stays in effect.  A direction holding data refuses the change.
 */
static int
audio_format_set(
	struct drv_audio_device *device,
	uintptr_t argument)
{
	struct audio_format requested;
	struct audio_format current;
	struct audio_stream *playback;
	struct audio_stream *capture;
	unsigned long irq;
	unsigned index;
	int matched;
	int busy;
	int error;

	/* Reads the request. */
	error = copyin(argument, &requested, sizeof(requested));
	if (error != 0)
		return error;

	/* Looks for an exact match in the backend's list. */
	matched = 0;
	for (index = 0; index < device->ops->format_count; index++) {
		current = device->ops->formats[index];
		if (current.format == requested.format &&
		    current.channels == requested.channels &&
		    current.rate == requested.rate &&
		    requested.reserved == 0U) {
			matched = 1;
			break;
		}
	}

	/* Holds both directions still while the format changes. */
	playback = &device->streams[AUDIO_PLAYBACK];
	capture = &device->streams[AUDIO_CAPTURE];
	error = mutex_lock_interruptible(&playback->lock);
	if (error != 0)
		return error;

	error = mutex_lock_interruptible(&capture->lock);
	if (error != 0) {
		mutex_unlock(&playback->lock);
		return error;
	}

	/* Installs the format unless a direction is running or holds data. */
	busy = 0;
	irq = spin_lock_irqsave(&device->lock);

	if (playback->running != 0U || playback->application != 0)
		busy = 1;
	if (capture->running != 0U)
		busy = 1;

	if (busy == 0 && matched != 0)
		device->format = requested;

	current = device->format;

	spin_unlock_irqrestore(&device->lock, irq);

	mutex_unlock(&capture->lock);
	mutex_unlock(&playback->lock);

	/* Refuses a change while sound is in flight. */
	if (busy != 0)
		return EBUSY;

	/* Reports the format that stays in effect after a mismatch. */
	if (matched == 0) {
		(void)copyout(&current, argument, sizeof(current));
		return EINVAL;
	}

	/* Succeeded: the next start uses the new format. */
	return 0;
}

/* Discards everything in flight and stops each open direction. */
static int
audio_flush(
	struct drv_audio_device *device)
{
	struct audio_stream *stream;
	int error;

	/* Stops playback without playing what was written. */
	if (device->playback_open != 0U) {
		stream = &device->streams[AUDIO_PLAYBACK];
		error = mutex_lock_interruptible(&stream->lock);
		if (error != 0)
			return error;

		audio_stream_stop(device, AUDIO_PLAYBACK);
		mutex_unlock(&stream->lock);
	}

	/* Stops capture; the next read starts it again. */
	if (device->capture_open != 0U) {
		stream = &device->streams[AUDIO_CAPTURE];
		error = mutex_lock_interruptible(&stream->lock);
		if (error != 0)
			return error;

		audio_stream_stop(device, AUDIO_CAPTURE);
		mutex_unlock(&stream->lock);
	}

	/* Succeeded: both directions are idle and empty. */
	return 0;
}

/* Admits the single open of /dev/dspN in the directions it asks for. */
static int
audio_dsp_open(
	struct file *file)
{
	struct drv_audio_device *device;
	unsigned long irq;
	unsigned playback;
	unsigned capture;
	int mode;
	int error;

	/* Reads the directions from the access mode. */
	device = file->f_data;
	mode = file_status_flags_get(file) & O_ACCMODE;
	playback = 0U;
	capture = 0U;
	if (mode == O_WRONLY) {
		playback = 1U;
	} else if (mode == O_RDONLY) {
		capture = 1U;
	} else if (mode == O_RDWR) {
		/*
		 * O_RDWR takes the directions the hardware has.  The system maps
		 * no device opened write-only, so this is how a playback-only
		 * device is mapped.
		 */
		playback = device->ops->playback != 0U;
		capture = device->ops->capture != 0U;
	} else {
		return EINVAL;
	}

	/* Refuses a direction the hardware does not have. */
	if (playback != 0U && device->ops->playback == 0U)
		return ENODEV;
	if (capture != 0U && device->ops->capture == 0U)
		return ENODEV;

	/* Claims the device and resets what one open reports. */
	error = 0;
	irq = spin_lock_irqsave(&device->lock);

	if (device->online == 0U) {
		error = ENODEV;
	} else if (device->dsp_open != 0U) {
		error = EBUSY;
	} else {
		device->dsp_open = 1U;
		device->playback_open = playback;
		device->capture_open = capture;
		device->format = device->ops->formats[0];
		device->streams[AUDIO_PLAYBACK].transferred_before = 0;
		device->streams[AUDIO_PLAYBACK].underruns = 0;
		device->streams[AUDIO_PLAYBACK].overruns = 0;
		device->streams[AUDIO_CAPTURE].transferred_before = 0;
		device->streams[AUDIO_CAPTURE].underruns = 0;
		device->streams[AUDIO_CAPTURE].overruns = 0;
	}

	spin_unlock_irqrestore(&device->lock, irq);

	/* Reports a withdrawn or already open device. */
	if (error != 0)
		return error;

	/* Succeeded: this file is the device's one open. */
	return 0;
}

/*
 * Ends the single open.
 *
 * Written sound plays for at most two seconds before the stream stops, so
 * a broken hardware cannot keep the device busy forever.
 */
static int
audio_dsp_close(
	struct file *file)
{
	struct drv_audio_device *device;
	struct audio_stream *stream;
	unsigned long irq;

	/* Plays out, stops and frees playback. */
	device = file->f_data;
	if (device->playback_open != 0U) {
		stream = &device->streams[AUDIO_PLAYBACK];
		mutex_lock(&stream->lock);
		if (stream->mapped == 0U)
			(void)audio_playback_drain(device, 1);
		audio_stream_stop(device, AUDIO_PLAYBACK);
		audio_stream_free(device, AUDIO_PLAYBACK);
		mutex_unlock(&stream->lock);
	}

	/* Stops and frees capture. */
	if (device->capture_open != 0U) {
		stream = &device->streams[AUDIO_CAPTURE];
		mutex_lock(&stream->lock);
		audio_stream_stop(device, AUDIO_CAPTURE);
		audio_stream_free(device, AUDIO_CAPTURE);
		mutex_unlock(&stream->lock);
	}

	/* Releases the device for the next open and for unregister. */
	irq = spin_lock_irqsave(&device->lock);

	device->dsp_open = 0U;
	device->playback_open = 0U;
	device->capture_open = 0U;
	waitq_wake_all(&device->waitq);

	spin_unlock_irqrestore(&device->lock, irq);

	/* Succeeded: the device can be opened again. */
	return 0;
}

/* Reads captured sound. */
static ssize_t
audio_dsp_read(
	struct file *file,
	void *buffer,
	size_t size)
{
	struct drv_audio_device *device;
	struct audio_stream *stream;
	ssize_t count;
	int nonblocking;
	int error;

	/* Refuses a read on an open without capture. */
	device = file->f_data;
	if (device->capture_open == 0U)
		return -EBADF;

	/* Reads the blocking mode once for the whole request. */
	nonblocking = 0;
	if ((file_status_flags_get(file) & O_NONBLOCK) != 0)
		nonblocking = 1;

	/* Reads with the capture direction held against other readers. */
	stream = &device->streams[AUDIO_CAPTURE];
	error = mutex_lock_interruptible(&stream->lock);
	if (error != 0)
		return -error;

	/* A mapped direction moves through the mapping only. */
	if (stream->mapped != 0U)
		count = -EBUSY;
	else
		count = audio_capture_read(device, buffer, size, nonblocking);
	mutex_unlock(&stream->lock);

	/* Reports the bytes read or the reason none were. */
	return count;
}

/* Writes sound for playback. */
static ssize_t
audio_dsp_write(
	struct file *file,
	const void *buffer,
	size_t size)
{
	struct drv_audio_device *device;
	struct audio_stream *stream;
	ssize_t count;
	int nonblocking;
	int error;

	/* Refuses a write on an open without playback. */
	device = file->f_data;
	if (device->playback_open == 0U)
		return -EBADF;

	/* Reads the blocking mode once for the whole request. */
	nonblocking = 0;
	if ((file_status_flags_get(file) & O_NONBLOCK) != 0)
		nonblocking = 1;

	/* Writes with the playback direction held against other writers. */
	stream = &device->streams[AUDIO_PLAYBACK];
	error = mutex_lock_interruptible(&stream->lock);
	if (error != 0)
		return -error;

	/* A mapped direction moves through the mapping only. */
	if (stream->mapped != 0U)
		count = -EBUSY;
	else
		count = audio_playback_write(device, buffer, size, nonblocking);
	mutex_unlock(&stream->lock);

	/* Reports the bytes written or the reason none were. */
	return count;
}

/* Carries out one stream request. */
static int
audio_dsp_ioctl(
	struct file *file,
	unsigned long command,
	uintptr_t argument)
{
	struct drv_audio_device *device;
	struct audio_stream *stream;
	struct audio_format format;
	struct audio_buffer_info info;
	unsigned long irq;
	uint32_t delay;
	int error;

	/* Dispatches on the request. */
	device = file->f_data;
	switch (command) {
	case KERN_AUDIO_GET_FORMAT:
		irq = spin_lock_irqsave(&device->lock);
		format = device->format;
		spin_unlock_irqrestore(&device->lock, irq);

		error = copyout(&format, argument, sizeof(format));
		break;

	case KERN_AUDIO_SET_FORMAT:
		error = audio_format_set(device, argument);
		break;

	case KERN_AUDIO_GET_BUFFER:
		kern_memset(&info, 0, sizeof(info));
		irq = spin_lock_irqsave(&device->lock);
		info.bytes_per_frame = audio_bytes_per_frame(&device->format);
		spin_unlock_irqrestore(&device->lock, irq);

		info.fragment_bytes = AUDIO_FRAGMENT_BYTES;
		info.fragment_count = AUDIO_FRAGMENT_COUNT;
		error = copyout(&info, argument, sizeof(info));
		break;

	case KERN_AUDIO_GET_OSPACE:
		error = EBADF;
		if (device->playback_open != 0U)
			error = audio_space_get(device, AUDIO_PLAYBACK, argument);
		break;

	case KERN_AUDIO_GET_ISPACE:
		error = EBADF;
		if (device->capture_open != 0U)
			error = audio_space_get(device, AUDIO_CAPTURE, argument);
		break;

	case KERN_AUDIO_DRAIN:
		if (device->playback_open == 0U) {
			error = EBADF;
			break;
		}

		stream = &device->streams[AUDIO_PLAYBACK];
		error = mutex_lock_interruptible(&stream->lock);
		if (error != 0)
			break;

		/* A mapped stream has no written end to wait for. */
		if (stream->mapped != 0U)
			error = EBUSY;
		else
			error = audio_playback_drain(device, 0);
		mutex_unlock(&stream->lock);
		break;

	case KERN_AUDIO_FLUSH:
		error = audio_flush(device);
		break;

	case KERN_AUDIO_GET_DELAY:
		if (device->playback_open == 0U) {
			error = EBADF;
			break;
		}

		stream = &device->streams[AUDIO_PLAYBACK];
		delay = 0;
		irq = spin_lock_irqsave(&device->lock);
		audio_stream_sync_locked(device, AUDIO_PLAYBACK);
		if (stream->application > stream->hardware)
			delay = (uint32_t)(stream->application - stream->hardware);
		spin_unlock_irqrestore(&device->lock, irq);

		error = copyout(&delay, argument, sizeof(delay));
		break;

	case KERN_AUDIO_GET_CAPS:
		error = audio_caps_get(argument);
		break;

	case KERN_AUDIO_GET_OPTR:
		error = EBADF;
		if (device->playback_open != 0U)
			error = audio_mmap_position_get(device, AUDIO_PLAYBACK, argument);
		break;

	case KERN_AUDIO_GET_IPTR:
		error = EBADF;
		if (device->capture_open != 0U)
			error = audio_mmap_position_get(device, AUDIO_CAPTURE, argument);
		break;

	case KERN_AUDIO_SET_TRIGGER:
		error = audio_trigger(device, argument);
		break;

	default:
		error = ENOTTY;
		break;
	}

	/* Reports a refused or failed request. */
	if (error != 0)
		return error;

	/* Succeeded: the request is complete. */
	return 0;
}

/* Reports room for a fragment of playback or a fragment of captured data. */
static int
audio_dsp_poll(
	struct file *file,
	short events,
	short *revents)
{
	struct drv_audio_device *device;
	struct audio_stream *stream;
	unsigned long irq;
	uint32_t bytes;

	/* A poll result needs storage owned by the caller. */
	if (revents == NULL)
		return EINVAL;

	/* Samples both directions under one lock. */
	*revents = 0;
	device = file->f_data;
	irq = spin_lock_irqsave(&device->lock);

	/*
	 * A mapped direction is ready once its frontier has moved a fragment
	 * the caller has not yet asked about.
	 */
	if (device->playback_open != 0U) {
		stream = &device->streams[AUDIO_PLAYBACK];
		audio_stream_sync_locked(device, AUDIO_PLAYBACK);
		if (stream->mapped != 0U)
			bytes = stream->frontier / AUDIO_FRAGMENT_BYTES > stream->reported ?
			    AUDIO_FRAGMENT_BYTES : 0U;
		else
			bytes = audio_playback_room_locked(device);
		if (bytes >= AUDIO_FRAGMENT_BYTES)
			*revents |= events & (POLLOUT | POLLWRNORM);
	}

	if (device->capture_open != 0U) {
		stream = &device->streams[AUDIO_CAPTURE];
		audio_stream_sync_locked(device, AUDIO_CAPTURE);
		if (stream->mapped != 0U)
			bytes = stream->frontier / AUDIO_FRAGMENT_BYTES > stream->reported ?
			    AUDIO_FRAGMENT_BYTES : 0U;
		else
			bytes = audio_capture_available_locked(device);
		if (bytes >= AUDIO_FRAGMENT_BYTES)
			*revents |= events & (POLLIN | POLLRDNORM);
	}

	spin_unlock_irqrestore(&device->lock, irq);

	/* Succeeded: the readiness bits reflect the current positions. */
	return 0;
}


/*
 * Maps the shadow ring of one direction.
 *
 * As in OSS, the protection chooses the ring: PROT_WRITE maps playback
 * and PROT_READ alone maps capture.  The system refuses to map a device
 * opened write-only, so playback is mapped from an O_RDWR open, which
 * may map both rings.  A direction may be mapped only before read or
 * write has moved it.  The first mapping allocates the DMA ring and the
 * shadow; both stay until close, which every mapping outlives because
 * it retains the file.
 */
static int
audio_dsp_mmap(
	struct file *file,
	off_t offset,
	size_t bytes,
	uint32_t prot,
	struct vm_device_mapping **result)
{
	struct drv_audio_device *device;
	struct audio_stream *stream;
	unsigned long irq;
	uint32_t maximum;
	int capture;
	int busy;
	int error;

	/* The protection chooses the ring; the open must have that direction. */
	device = file->f_data;
	if ((prot & KERN_PROT_WRITE) != 0) {
		if (device->playback_open == 0U)
			return EACCES;
		capture = AUDIO_PLAYBACK;
		maximum = KERN_PROT_READ | KERN_PROT_WRITE;
	} else {
		if (device->capture_open == 0U)
			return EINVAL;
		capture = AUDIO_CAPTURE;
		maximum = KERN_PROT_READ;
	}

	/* Refuses a range that is not whole pages from the start of the ring. */
	if (offset != 0 || bytes == 0 || bytes > AUDIO_RING_BYTES ||
	    (bytes & (KERN_PAGE_SIZE - 1U)) != 0)
		return EINVAL;

	/* Refuses access beyond reading and writing. */
	if ((prot & ~maximum) != 0)
		return EACCES;

	/* Holds the direction still while it becomes mapped. */
	stream = &device->streams[capture];
	error = mutex_lock_interruptible(&stream->lock);
	if (error != 0)
		return error;

	/* A direction already moving through read or write cannot switch. */
	irq = spin_lock_irqsave(&device->lock);
	busy = 0;
	if (stream->mapped == 0U &&
	    (stream->running != 0U || stream->application != 0))
		busy = 1;
	spin_unlock_irqrestore(&device->lock, irq);

	error = 0;
	if (busy != 0)
		error = EBUSY;

	/* Allocates the DMA ring and the shadow on the first mapping. */
	if (error == 0)
		error = audio_stream_allocate(device, capture);
	if (error == 0)
		error = audio_shadow_allocate(stream);

	/* Hands the shadow to the VM; the mapping retains the file. */
	if (error == 0)
		error = vm_device_create(file,
		    stream->shadow.paddr,
		    stream->shadow_address,
		    bytes,
		    0U,
		    maximum,
		    audio_mapping_release,
		    device,
		    result);

	/* From here read and write are refused until close. */
	if (error == 0) {
		irq = spin_lock_irqsave(&device->lock);
		stream->mapped = 1U;
		spin_unlock_irqrestore(&device->lock, irq);
	}

	mutex_unlock(&stream->lock);

	/* Reports a refused or failed mapping. */
	if (error != 0)
		return error;

	/* Succeeded: the caller maps the shadow ring. */
	return 0;
}

/*
 * Ends one mapping.  The shadow belongs to the open and is freed at
 * close, which the mapping's file reference has kept from running.
 */
static void
audio_mapping_release(
	void *owner)
{
	(void)owner;
}

/* Allocates a silent shadow ring the VM can map. */
static int
audio_shadow_allocate(
	struct audio_stream *stream)
{
	int error;

	/* An earlier mapping of this open already has one. */
	if (stream->shadow_address != NULL)
		return 0;

	/* Allocates whole pages that have a kernel address. */
	error = kern_pmem_alloc(AUDIO_RING_BYTES, KERN_PAGE_SIZE, &stream->shadow);
	if (error != 0)
		return error;

	stream->shadow_address = kern_pmem_to_kernel(stream->shadow.paddr);
	if (stream->shadow_address == NULL) {
		(void)kern_pmem_free(&stream->shadow);
		return ENOMEM;
	}

	/* A new mapping reads as silence. */
	kern_memset(stream->shadow_address, 0, AUDIO_RING_BYTES);

	/* Succeeded: the direction owns a shadow until close. */
	return 0;
}

/*
 * Moves a mapped direction's frontier to where its interrupts put it.
 *
 * Playback copies each fragment from the shadow into the DMA ring a lead
 * ahead of the hardware and silences it in the shadow, so a writer that
 * falls behind is heard as silence, not as the sound of a ring ago.
 * Capture copies each completed fragment out to the shadow.  A frontier
 * more than a ring behind skips to the last ring.  The device lock is
 * held.
 */
static void
audio_mmap_advance_locked(
	struct drv_audio_device *device,
	int capture)
{
	struct audio_stream *stream;
	uint8_t *ring;
	uint64_t target;
	uint32_t offset;

	/* Playback runs a lead ahead of the completed fragments; capture follows them. */
	stream = &device->streams[capture];
	ring = stream->buffer.address;
	target = stream->interrupts * AUDIO_FRAGMENT_BYTES;
	if (capture == 0)
		target += AUDIO_MMAP_LEAD_FRAGMENTS * AUDIO_FRAGMENT_BYTES;

	/* Only the last ring of a long gap still has a place in the ring. */
	if (target > stream->frontier + AUDIO_RING_BYTES)
		stream->frontier = target - AUDIO_RING_BYTES;

	/* Moves one whole fragment at a time. */
	while (stream->frontier < target) {
		offset = (uint32_t)(stream->frontier % AUDIO_RING_BYTES);
		if (capture == 0) {
			kern_memcpy(ring + offset, stream->shadow_address + offset,
			    AUDIO_FRAGMENT_BYTES);
			kern_memset(stream->shadow_address + offset, 0,
			    AUDIO_FRAGMENT_BYTES);
		} else {
			kern_memcpy(stream->shadow_address + offset, ring + offset,
			    AUDIO_FRAGMENT_BYTES);
		}

		stream->frontier += AUDIO_FRAGMENT_BYTES;
	}
}

/*
 * Starts a mapped direction.  Playback first takes the lead fragments the
 * caller filled before the trigger, so they are the first to play.
 */
static int
audio_mmap_start(
	struct drv_audio_device *device,
	int capture)
{
	struct audio_stream *stream;
	unsigned long irq;
	int error;

	/* Takes the first fragments while the hardware is still stopped. */
	stream = &device->streams[capture];
	irq = spin_lock_irqsave(&device->lock);

	stream->frontier = 0;
	stream->reported = 0;
	if (capture == 0) {
		stream->interrupts = 0;
		audio_mmap_advance_locked(device, capture);
		stream->released = 0;
	}

	spin_unlock_irqrestore(&device->lock, irq);

	/* Starts the hardware on the prepared ring. */
	error = audio_stream_start(device, capture);
	if (error != 0) {
		audio_stream_stop(device, capture);
		return error;
	}

	/* Succeeded: the direction moves through the mapping. */
	return 0;
}

/*
 * Starts or stops each mapped direction of the open as the trigger bits
 * say.  A bit for a direction the open lacks, or has not mapped, is
 * refused before anything changes.
 */
static int
audio_trigger(
	struct drv_audio_device *device,
	uintptr_t argument)
{
	struct audio_stream *stream;
	uint32_t bits;
	uint32_t bit;
	int capture;
	int wanted;
	int error;

	/* Reads the request. */
	error = copyin(argument, &bits, sizeof(bits));
	if (error != 0)
		return error;

	/* Refuses an unknown bit. */
	if ((bits & ~(KERN_AUDIO_TRIGGER_OUTPUT | KERN_AUDIO_TRIGGER_INPUT)) != 0)
		return EINVAL;

	/* Refuses a bit for a direction that is not open and mapped. */
	if ((bits & KERN_AUDIO_TRIGGER_OUTPUT) != 0 &&
	    (device->playback_open == 0U ||
	     device->streams[AUDIO_PLAYBACK].mapped == 0U))
		return EINVAL;
	if ((bits & KERN_AUDIO_TRIGGER_INPUT) != 0 &&
	    (device->capture_open == 0U ||
	     device->streams[AUDIO_CAPTURE].mapped == 0U))
		return EINVAL;

	/* Brings each mapped direction to the state its bit asks for. */
	for (capture = AUDIO_PLAYBACK; capture <= AUDIO_CAPTURE; capture++) {
		stream = &device->streams[capture];
		bit = capture == AUDIO_PLAYBACK ?
		    KERN_AUDIO_TRIGGER_OUTPUT : KERN_AUDIO_TRIGGER_INPUT;
		if (stream->mapped == 0U)
			continue;

		error = mutex_lock_interruptible(&stream->lock);
		if (error != 0)
			return error;

		wanted = (bits & bit) != 0;
		if (wanted != 0 && stream->running == 0U)
			error = audio_mmap_start(device, capture);
		else if (wanted == 0 && stream->running != 0U)
			audio_stream_stop(device, capture);

		mutex_unlock(&stream->lock);
		if (error != 0)
			return error;
	}

	/* Succeeded: each mapped direction runs as asked. */
	return 0;
}

/* Copies the frontier of one mapped direction to the caller. */
static int
audio_mmap_position_get(
	struct drv_audio_device *device,
	int capture,
	uintptr_t argument)
{
	struct audio_mmap_position position;
	struct audio_stream *stream;
	unsigned long irq;
	uint64_t fragments;

	/* A direction that is not mapped has no frontier. */
	stream = &device->streams[capture];
	if (stream->mapped == 0U)
		return EINVAL;

	/* Samples the frontier and counts the fragments it moved since last asked. */
	kern_memset(&position, 0, sizeof(position));
	irq = spin_lock_irqsave(&device->lock);

	audio_stream_sync_locked(device, capture);
	fragments = stream->frontier / AUDIO_FRAGMENT_BYTES;
	position.bytes = stream->frontier;
	position.fragments = (uint32_t)(fragments - stream->reported);
	position.offset = (uint32_t)(stream->frontier % AUDIO_RING_BYTES);
	stream->reported = fragments;

	spin_unlock_irqrestore(&device->lock, irq);

	/* Publishes the snapshot. */
	return copyout(&position, argument, sizeof(position));
}

/* Reports what the stream node can do. */
static int
audio_caps_get(
	uintptr_t argument)
{
	struct audio_caps caps;

	/* The mapping is a shadow copied one fragment per interrupt. */
	kern_memset(&caps, 0, sizeof(caps));
	caps.caps = KERN_AUDIO_CAP_MMAP | KERN_AUDIO_CAP_TRIGGER |
	    KERN_AUDIO_CAP_MMAP_COPY;
	caps.mmap_bytes = AUDIO_RING_BYTES;
	caps.mmap_lead_bytes = AUDIO_MMAP_LEAD_FRAGMENTS * AUDIO_FRAGMENT_BYTES;

	/* Publishes the report. */
	return copyout(&caps, argument, sizeof(caps));
}

/* Admits any number of opens of /dev/mixerN while the device is registered. */
static int
audio_mixer_open(
	struct file *file)
{
	struct drv_audio_device *device;
	unsigned long irq;
	unsigned online;

	/* Samples whether the backend is still registered. */
	device = file->f_data;
	irq = spin_lock_irqsave(&device->lock);

	online = device->online;

	spin_unlock_irqrestore(&device->lock, irq);

	/* Refuses a withdrawn device. */
	if (online == 0U)
		return ENODEV;

	/* Succeeded: volume requests may follow. */
	return 0;
}

/* Ends one mixer open, which holds nothing between requests. */
static int
audio_mixer_close(
	struct file *file)
{
	/* A mixer open owns no state. */
	(void)file;

	/* Succeeded: nothing to release. */
	return 0;
}

/* Reads or changes the hardware volume. */
static int
audio_mixer_ioctl(
	struct file *file,
	unsigned long command,
	uintptr_t argument)
{
	struct drv_audio_device *device;
	struct audio_volume volume;
	int mode;
	int error;

	/* Dispatches on the request. */
	device = file->f_data;
	switch (command) {
	case KERN_AUDIO_GET_VOLUME:
		error = audio_mixer_enter(device);
		if (error != 0)
			break;

		/* A backend without a volume reports the unadjusted level. */
		volume.left = 100U;
		volume.right = 100U;
		volume.muted = 0U;
		volume.reserved = 0U;
		if (device->ops->get_volume != NULL)
			error = device->ops->get_volume(device->private_data, &volume);

		audio_mixer_leave(device);
		if (error != 0)
			break;

		error = copyout(&volume, argument, sizeof(volume));
		break;

	case KERN_AUDIO_SET_VOLUME:
		mode = file_status_flags_get(file) & O_ACCMODE;
		if (mode == O_RDONLY) {
			error = EBADF;
			break;
		}

		error = copyin(argument, &volume, sizeof(volume));
		if (error != 0)
			break;

		if (volume.left > 100U || volume.right > 100U) {
			error = EINVAL;
			break;
		}

		if (volume.muted > 1U || volume.reserved != 0U) {
			error = EINVAL;
			break;
		}

		/* Attenuating in the ring would lag by a whole ring, so it is refused. */
		if (device->ops->set_volume == NULL) {
			error = ENOTSUP;
			break;
		}

		error = audio_mixer_enter(device);
		if (error != 0)
			break;

		error = device->ops->set_volume(device->private_data, &volume);
		audio_mixer_leave(device);
		break;

	default:
		error = ENOTTY;
		break;
	}

	/* Reports a refused or failed request. */
	if (error != 0)
		return error;

	/* Succeeded: the request is complete. */
	return 0;
}

/* Keeps the backend registered for the duration of one volume call. */
static int
audio_mixer_enter(
	struct drv_audio_device *device)
{
	unsigned long irq;
	int error;

	/*
	 * mixer_calls holds off unregister, which refuses with EBUSY while it
	 * is nonzero, so the backend stays valid through the call.
	 */
	error = 0;
	irq = spin_lock_irqsave(&device->lock);

	if (device->online == 0U)
		error = ENODEV;
	else
		device->mixer_calls++;

	spin_unlock_irqrestore(&device->lock, irq);

	/* Refuses a withdrawn device. */
	if (error != 0)
		return error;

	/* Succeeded: the caller may call the backend. */
	return 0;
}

/* Ends one volume call. */
static void
audio_mixer_leave(
	struct drv_audio_device *device)
{
	unsigned long irq;

	/* Lets unregister proceed once no volume call remains. */
	irq = spin_lock_irqsave(&device->lock);

	device->mixer_calls--;

	spin_unlock_irqrestore(&device->lock, irq);

	/* Succeeded: the call no longer holds the backend. */
	return;
}
