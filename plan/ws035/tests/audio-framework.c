/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Exercises the production audio framework on the host (ws035-p006).
 *
 * The fixture supplies the allocator, locks, wait queues, user copies,
 * cdev registry and DMA allocator.  A fake driver plays the hardware: it
 * reads the playback ring into a log, writes a counting pattern into the
 * capture ring, and raises an interrupt per fragment when the test
 * advances it.  A sleeping thread lets the "hardware" run through a hook.
 */

#include <drivers/audio/audio.h>
#include <kern/cdev.h>
#include <kern/clock.h>
#include <kern/file.h>
#include <kern/kmem.h>
#include <kern/lock.h>
#include <kern/pmem.h>
#include <kern/poll.h>
#include <kern/uaccess.h>
#include <kern/vm-device.h>
#include <kern/waitq.h>

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FRAGMENT	4096U
#define RING		(FRAGMENT * 8U)
#define LOG_MAX		(1024U * 1024U)

/* The fake hardware of one audio function. */
struct fake_hw {
	uint32_t position[2];
	unsigned prepares[2];
	unsigned starts[2];
	unsigned stops[2];
	unsigned running[2];
	uint8_t *ring[2];
	int start_error;
	struct audio_volume volume;
	unsigned volume_sets;
	uint8_t played[LOG_MAX];
	size_t played_bytes;
	uint32_t capture_counter;
};

/* One registered cdev generation. */
struct fake_node {
	struct cdev cdev;
	unsigned refs;
	unsigned published;
};

/* What the fixture's sleeping thread lets happen. */
enum sleep_mode {
	SLEEP_REFUSE,
	SLEEP_PLAY,
	SLEEP_STUCK,
	SLEEP_SIGNAL,
};

static struct fake_hw hw;
static struct fake_node *nodes[256];
static unsigned node_count;
static unsigned held_spinlocks;
static unsigned poll_notifications;
static unsigned live_allocations;
static unsigned live_dma;
static unsigned live_pmem;
static struct vm_device_mapping *last_mapping;
static uint64_t ticks;
static enum sleep_mode sleep_mode;
static unsigned sleeps;
static struct drv_audio_device *current_device;
static void (*copy_hook)(void);

static const struct audio_format formats[] = {
	{ KERN_AUDIO_FORMAT_S16_LE, 2U, 48000U, 0U },
	{ KERN_AUDIO_FORMAT_S32_LE, 2U, 48000U, 0U },
	{ KERN_AUDIO_FORMAT_S16_LE, 1U, 8000U, 0U },
};

static void fake_advance(int capture, uint32_t bytes, int interrupt);

/* ---------------------------------------------------------------- */
/* Kernel services.                                                 */

void
__libc_assert_fail(const char *expression, const char *file, int line)
{
	fprintf(stderr, "ASSERT %s:%d: %s\n", file, line, expression);
	abort();
}

void *
kern_calloc(size_t count, size_t size)
{
	void *memory;

	memory = calloc(count, size);
	if (memory != NULL)
		live_allocations++;
	return memory;
}

void
kern_free(void *memory)
{
	if (memory == NULL)
		return;
	assert(live_allocations != 0);
	live_allocations--;
	free(memory);
}

void *
kern_memcpy(void *destination, const void *source, size_t count)
{
	void (*hook)(void);

	memcpy(destination, source, count);

	/* A one-shot hook runs "during" the copy, with no lock held. */
	hook = copy_hook;
	if (hook != NULL) {
		assert(held_spinlocks == 0);
		copy_hook = NULL;
		hook();
	}
	return destination;
}

void *
kern_memset(void *destination, int value, size_t count)
{
	return memset(destination, value, count);
}

int
kern_snprintf(char *buffer, size_t size, const char *format, ...)
{
	va_list arguments;
	int length;

	va_start(arguments, format);
	length = vsnprintf(buffer, size, format, arguments);
	va_end(arguments);
	return length;
}

void
spin_init(struct spinlock *lock, enum lock_rank rank, const char *name)
{
	memset(lock, 0, sizeof(*lock));
	lock->rank = rank;
	lock->name = name;
}

unsigned long
spin_lock_irqsave(struct spinlock *lock)
{
	unsigned busy;

	busy = __atomic_exchange_n(&lock->held.value, 1U, __ATOMIC_ACQUIRE);
	assert(busy == 0);
	held_spinlocks++;
	return 1;
}

void
spin_unlock_irqrestore(struct spinlock *lock, unsigned long enabled)
{
	assert(enabled == 1);
	assert(held_spinlocks != 0);
	held_spinlocks--;
	__atomic_store_n(&lock->held.value, 0U, __ATOMIC_RELEASE);
}

int
mutex_init(struct mutex *mutex, enum lock_rank rank, const char *name)
{
	memset(mutex, 0, sizeof(*mutex));
	(void)rank;
	(void)name;
	return 0;
}

int
mutex_lock_interruptible(struct mutex *mutex)
{
	assert(mutex->locked == 0);
	mutex->locked = 1;
	return 0;
}

void
mutex_lock(struct mutex *mutex)
{
	assert(mutex->locked == 0);
	mutex->locked = 1;
}

void
mutex_unlock(struct mutex *mutex)
{
	assert(mutex->locked == 1);
	mutex->locked = 0;
}

void
waitq_init(struct wait_queue *queue, const char *name)
{
	memset(queue, 0, sizeof(*queue));
	queue->name = name;
}

uint64_t
waitq_sequence(const struct wait_queue *queue)
{
	return queue->sequence;
}

void
waitq_wake_all(struct wait_queue *queue)
{
	queue->sequence++;
}

/*
 * Sleeps by letting the fake hardware run, as another CPU would.
 *
 * Each sleep advances time by one tick.  SLEEP_PLAY moves every running
 * stream by one fragment and raises its interrupt; SLEEP_STUCK moves
 * nothing; SLEEP_SIGNAL reports a signal; SLEEP_REFUSE fails the test.
 */
int
waitq_sleep(struct wait_queue *queue, struct spinlock *lock, uint64_t observed,
    uint64_t deadline, unsigned flags)
{
	if (queue->sequence != observed)
		return EAGAIN;
	if (deadline != 0 && ticks >= deadline)
		return ETIMEDOUT;

	sleeps++;
	ticks++;
	assert(held_spinlocks == 1);
	spin_unlock_irqrestore(lock, 1);

	switch (sleep_mode) {
	case SLEEP_PLAY:
		if (hw.running[0])
			fake_advance(0, FRAGMENT, 1);
		if (hw.running[1])
			fake_advance(1, FRAGMENT, 1);
		break;
	case SLEEP_STUCK:
		break;
	case SLEEP_SIGNAL:
		(void)spin_lock_irqsave(lock);
		assert((flags & WAITQ_INTERRUPTIBLE) != 0);
		return EINTR;
	case SLEEP_REFUSE:
	default:
		fprintf(stderr, "unexpected sleep\n");
		abort();
	}

	(void)spin_lock_irqsave(lock);
	if (deadline != 0 && ticks >= deadline)
		return ETIMEDOUT;
	return 0;
}

uint64_t
sched_ticks(void)
{
	return ticks;
}

void
poll_notify(void)
{
	assert(held_spinlocks == 0);
	poll_notifications++;
}

int
copyin(uintptr_t source, void *destination, size_t size)
{
	if (source == 0)
		return EFAULT;
	memcpy(destination, (const void *)source, size);
	return 0;
}

int
copyout(const void *source, uintptr_t destination, size_t size)
{
	if (destination == 0)
		return EFAULT;
	memcpy((void *)destination, source, size);
	return 0;
}

/* The backend's DMA owner; the framework only borrows it. */
static uint8_t fake_dma_storage[16];
#define FAKE_DMA ((struct drv_dma_device *)fake_dma_storage)

int
drv_dma_alloc_coherent(struct drv_dma_device *device, size_t size,
    size_t alignment, struct drv_dma_buffer *buffer)
{
	assert(device == FAKE_DMA);
	memset(buffer, 0, sizeof(*buffer));
	(void)alignment;
	buffer->address = malloc(size);
	if (buffer->address == NULL)
		return ENOMEM;

	/* Stale bytes prove that the framework silences a new ring. */
	memset(buffer->address, 0x5a, size);
	buffer->device_address = (uintptr_t)buffer->address;
	buffer->size = size;
	live_dma++;
	return 0;
}

void
drv_dma_free_coherent(struct drv_dma_device *device, struct drv_dma_buffer *buffer)
{
	assert(device == FAKE_DMA);
	assert(live_dma != 0);
	live_dma--;
	free(buffer->address);
	buffer->address = NULL;
}

/* Physical pages for the mmap shadow; the "physical" address is the host address. */
int
kern_pmem_alloc(size_t size, size_t alignment, struct kern_pmem *run)
{
	void *memory;

	assert(held_spinlocks == 0);
	memory = aligned_alloc(alignment, size);
	if (memory == NULL)
		return ENOMEM;
	memset(memory, 0x5a, size);
	run->paddr = (hal_physaddr_t)(uintptr_t)memory;
	run->size = size;
	live_pmem++;
	return 0;
}

int
kern_pmem_free(struct kern_pmem *run)
{
	assert(live_pmem != 0);
	live_pmem--;
	free((void *)(uintptr_t)run->paddr);
	run->paddr = 0;
	return 0;
}

void *
kern_pmem_to_kernel(hal_physaddr_t address)
{
	return (void *)(uintptr_t)address;
}

/* Records the one mapping a test asked for; the test releases it. */
int
vm_device_create(struct file *file, uint64_t physical, void *address, size_t bytes,
    uint32_t attributes, uint32_t max_prot, void (*release)(void *), void *owner,
    struct vm_device_mapping **result)
{
	struct vm_device_mapping *mapping;

	assert(file != NULL && release != NULL);
	assert(physical == (uintptr_t)address);
	assert(attributes == 0U);
	mapping = calloc(1, sizeof(*mapping));
	assert(mapping != NULL);
	mapping->file = file;
	mapping->physical = physical;
	mapping->address = address;
	mapping->bytes = bytes;
	mapping->max_prot = max_prot;
	mapping->release = release;
	mapping->owner = owner;
	*result = mapping;
	last_mapping = mapping;
	return 0;
}

static void
mapping_drop(struct vm_device_mapping *mapping)
{
	mapping->release(mapping->owner);
	free(mapping);
}

/* ---------------------------------------------------------------- */
/* cdev registry: the registry and the caller each hold a reference. */

static void
node_put(struct fake_node *node)
{
	assert(node->refs != 0);
	node->refs--;
	if (node->refs == 0) {
		node->cdev.finalizer(node->cdev.data);
		free(node);
	}
}

int
cdev_register_managed(const char *name, dev_t rdev, const struct cdev_ops *ops,
    void *data, cdev_finalizer_t finalizer, struct cdev **result)
{
	struct fake_node *node;
	unsigned index;

	for (index = 0; index < node_count; index++) {
		if (nodes[index] != NULL && nodes[index]->published &&
		    strcmp(nodes[index]->cdev.name, name) == 0)
			return EEXIST;
	}

	node = calloc(1, sizeof(*node));
	assert(node != NULL);
	snprintf(node->cdev.name, sizeof(node->cdev.name), "%s", name);
	node->cdev.rdev = rdev;
	node->cdev.ops = ops;
	node->cdev.data = data;
	node->cdev.finalizer = finalizer;
	node->refs = 2;
	node->published = 1;
	assert(node_count < 256);
	nodes[node_count++] = node;
	*result = &node->cdev;
	return 0;
}

int
cdev_unregister(struct cdev *device)
{
	unsigned index;

	for (index = 0; index < node_count; index++) {
		if (nodes[index] != NULL && &nodes[index]->cdev == device) {
			assert(nodes[index]->published);
			nodes[index]->published = 0;
			nodes[index] = NULL;
			node_put((struct fake_node *)device);
			return 0;
		}
	}
	abort();
}

void
cdev_release(struct cdev *device)
{
	node_put((struct fake_node *)device);
}

static struct fake_node *
node_find(const char *name)
{
	unsigned index;

	for (index = 0; index < node_count; index++) {
		if (nodes[index] != NULL && nodes[index]->published &&
		    strcmp(nodes[index]->cdev.name, name) == 0)
			return nodes[index];
	}
	return NULL;
}

/* ---------------------------------------------------------------- */
/* Fake hardware.                                                   */

static int
fake_prepare(void *private_data, int capture, const struct audio_format *format,
    const struct drv_dma_buffer *buffer, uint32_t fragment_bytes,
    uint32_t fragment_count)
{
	struct fake_hw *fake = private_data;

	assert(held_spinlocks == 0);
	assert(fragment_bytes == FRAGMENT);
	assert(fragment_count * fragment_bytes == RING);
	assert(buffer->size >= RING);
	assert(format->rate != 0);
	fake->ring[capture] = buffer->address;
	fake->position[capture] = 0;
	fake->prepares[capture]++;
	return 0;
}

static int
fake_start(void *private_data, int capture)
{
	struct fake_hw *fake = private_data;

	assert(held_spinlocks == 0);
	if (fake->start_error != 0)
		return fake->start_error;
	fake->starts[capture]++;
	fake->running[capture] = 1;
	return 0;
}

static void
fake_stop(void *private_data, int capture)
{
	struct fake_hw *fake = private_data;

	assert(held_spinlocks == 0);
	fake->stops[capture]++;
	fake->running[capture] = 0;
}

static uint32_t
fake_position(void *private_data, int capture)
{
	struct fake_hw *fake = private_data;

	assert(held_spinlocks == 1);
	return fake->position[capture];
}

static int
fake_set_volume(void *private_data, const struct audio_volume *volume)
{
	struct fake_hw *fake = private_data;

	assert(held_spinlocks == 0);
	fake->volume = *volume;
	fake->volume_sets++;
	return 0;
}

static int
fake_get_volume(void *private_data, struct audio_volume *volume)
{
	struct fake_hw *fake = private_data;

	*volume = fake->volume;
	return 0;
}

/*
 * Moves the hardware by bytes.  Playback copies the ring bytes it passes
 * into the played log; capture writes a 16-bit counter into the ring.
 * An interrupt is raised for every fragment boundary crossed.
 */
static void
fake_advance(int capture, uint32_t bytes, int interrupt)
{
	uint32_t index;
	uint32_t position;

	assert(hw.running[capture]);
	for (index = 0; index < bytes; index += 2) {
		position = hw.position[capture];
		if (capture == 0) {
			assert(hw.played_bytes + 2 <= LOG_MAX);
			memcpy(hw.played + hw.played_bytes, hw.ring[0] + position, 2);
			hw.played_bytes += 2;
		} else {
			hw.ring[1][position] = (uint8_t)hw.capture_counter;
			hw.ring[1][position + 1] = (uint8_t)(hw.capture_counter >> 8);
			hw.capture_counter++;
		}

		hw.position[capture] = (position + 2) % RING;
		if (interrupt && hw.position[capture] % FRAGMENT == 0)
			drv_audio_interrupt(current_device, capture);
	}
}

static const struct drv_audio_ops full_ops = {
	1U, 1U, formats, 3U,
	fake_prepare, fake_start, fake_stop, fake_position,
	fake_get_volume, fake_set_volume,
};

static const struct drv_audio_ops plain_ops = {
	1U, 0U, formats, 1U,
	fake_prepare, fake_start, fake_stop, fake_position,
	NULL, NULL,
};

/* ---------------------------------------------------------------- */
/* Test helpers.                                                    */

struct test_file {
	struct file file;
	struct fake_node *node;
};

static int
open_node(struct test_file *opened, const char *name, int flags)
{
	int error;

	memset(opened, 0, sizeof(*opened));
	opened->node = node_find(name);
	if (opened->node == NULL)
		return ENOENT;
	opened->node->refs++;
	opened->file.f_data = opened->node->cdev.data;
	atomic_store_release(&opened->file.f_flags, (unsigned)flags);
	error = opened->node->cdev.ops->open(&opened->file);
	if (error != 0)
		node_put(opened->node);
	return error;
}

static void
close_node(struct test_file *opened)
{
	int error;

	error = opened->node->cdev.ops->close(&opened->file);
	assert(error == 0);
	node_put(opened->node);
}

static ssize_t
do_write(struct test_file *opened, const void *data, size_t size)
{
	return opened->node->cdev.ops->write(&opened->file, data, size);
}

static ssize_t
do_read(struct test_file *opened, void *data, size_t size)
{
	return opened->node->cdev.ops->read(&opened->file, data, size);
}

static int
do_ioctl(struct test_file *opened, unsigned long command, void *argument)
{
	return opened->node->cdev.ops->ioctl(&opened->file, command, (uintptr_t)argument);
}

static struct audio_space
ospace(struct test_file *opened)
{
	struct audio_space space;
	int error;

	error = do_ioctl(opened, KERN_AUDIO_GET_OSPACE, &space);
	assert(error == 0);
	return space;
}

static struct audio_space
ispace(struct test_file *opened)
{
	struct audio_space space;
	int error;

	error = do_ioctl(opened, KERN_AUDIO_GET_ISPACE, &space);
	assert(error == 0);
	return space;
}

static short
poll_node(struct test_file *opened, short events)
{
	short revents;
	int error;

	error = opened->node->cdev.ops->poll(&opened->file, events, &revents);
	assert(error == 0);
	return revents;
}

/* Fills a buffer with 16-bit samples counting from start. */
static void
pattern(uint8_t *buffer, size_t bytes, uint32_t start)
{
	size_t index;

	for (index = 0; index < bytes; index += 2) {
		buffer[index] = (uint8_t)(start + index / 2);
		buffer[index + 1] = (uint8_t)((start + index / 2) >> 8);
	}
}

/* Reports whether bytes hold 16-bit samples counting from start. */
static int
is_pattern(const uint8_t *buffer, size_t bytes, uint32_t start)
{
	size_t index;
	uint16_t sample;

	for (index = 0; index < bytes; index += 2) {
		sample = (uint16_t)(buffer[index] | (buffer[index + 1] << 8));
		if (sample != (uint16_t)(start + index / 2))
			return 0;
	}
	return 1;
}

static int
is_silent(const uint8_t *buffer, size_t bytes)
{
	size_t index;

	for (index = 0; index < bytes; index++) {
		if (buffer[index] != 0)
			return 0;
	}
	return 1;
}

/* Reports whether needle occurs in haystack. */
static int
contains(const uint8_t *haystack, size_t length, const uint8_t *needle, size_t size)
{
	size_t index;

	for (index = 0; index + size <= length; index++) {
		if (memcmp(haystack + index, needle, size) == 0)
			return 1;
	}
	return 0;
}

static void
reset_hw(void)
{
	memset(&hw, 0, sizeof(hw));
	sleep_mode = SLEEP_REFUSE;
}

static struct drv_audio_device *
register_device(const struct drv_audio_ops *ops)
{
	struct drv_audio_device *device;
	int error;

	error = drv_audio_register(ops, &hw, FAKE_DMA, &device);
	assert(error == 0);
	current_device = device;
	return device;
}

static void
unregister_device(struct drv_audio_device *device)
{
	int error;

	error = drv_audio_unregister(device);
	assert(error == 0);
	current_device = NULL;
}

/* ---------------------------------------------------------------- */
/* Tests.                                                           */

static void
test_registration(void)
{
	static const struct audio_format bad_format[] = {
		{ KERN_AUDIO_FORMAT_S16_LE, 3U, 48000U, 0U },
	};
	struct drv_audio_ops ops;
	struct drv_audio_device *first;
	struct drv_audio_device *second;
	int error;

	reset_hw();

	/* No driver means no node. */
	assert(node_find("dsp0") == NULL);
	assert(node_find("mixer0") == NULL);

	/* Incomplete backends are refused. */
	ops = full_ops;
	ops.playback = 0;
	ops.capture = 0;
	assert(drv_audio_register(&ops, &hw, FAKE_DMA, &first) == EINVAL);
	ops = full_ops;
	ops.position = NULL;
	assert(drv_audio_register(&ops, &hw, FAKE_DMA, &first) == EINVAL);
	ops = full_ops;
	ops.format_count = 0;
	assert(drv_audio_register(&ops, &hw, FAKE_DMA, &first) == EINVAL);
	ops = full_ops;
	ops.formats = bad_format;
	ops.format_count = 1;
	assert(drv_audio_register(&ops, &hw, FAKE_DMA, &first) == EINVAL);
	assert(drv_audio_register(&full_ops, &hw, FAKE_DMA, NULL) == EINVAL);
	assert(live_allocations == 0);

	/* Two devices get the numbers 0 and 1 and distinct dev_t values. */
	error = drv_audio_register(&full_ops, &hw, FAKE_DMA, &first);
	assert(error == 0);
	error = drv_audio_register(&plain_ops, &hw, FAKE_DMA, &second);
	assert(error == 0);
	assert(node_find("dsp0") != NULL && node_find("mixer0") != NULL);
	assert(node_find("dsp1") != NULL && node_find("mixer1") != NULL);
	assert(node_find("dsp0")->cdev.rdev == 0x000A0000U);
	assert(node_find("mixer0")->cdev.rdev == 0x000A0001U);
	assert(node_find("dsp1")->cdev.rdev == 0x000A0002U);

	/* A freed number is reused. */
	assert(drv_audio_unregister(first) == 0);
	assert(node_find("dsp0") == NULL);
	error = drv_audio_register(&full_ops, &hw, FAKE_DMA, &first);
	assert(error == 0);
	assert(node_find("dsp0") != NULL);

	assert(drv_audio_unregister(first) == 0);
	assert(drv_audio_unregister(second) == 0);
	assert(live_allocations == 0);
	printf("audio registration: validation, numbering, reuse PASS\n");
}

static void
test_ring_space_and_wrap(void)
{
	struct drv_audio_device *device;
	struct test_file dsp;
	struct audio_space space;
	struct audio_buffer_info info;
	static uint8_t data[RING * 4];
	uint32_t delay;
	ssize_t count;
	int error;

	reset_hw();
	device = register_device(&full_ops);
	error = open_node(&dsp, "dsp0", O_WRONLY);
	assert(error == 0);

	/* The ring shape. */
	error = do_ioctl(&dsp, KERN_AUDIO_GET_BUFFER, &info);
	assert(error == 0);
	assert(info.fragment_bytes == FRAGMENT && info.fragment_count == 8);
	assert(info.bytes_per_frame == 4);

	/* An empty ring is all room and no ring exists before the first write. */
	space = ospace(&dsp);
	assert(space.bytes == RING && space.fragments == 8);
	assert(live_dma == 0);

	/* Writing below half a ring does not start the hardware. */
	pattern(data, sizeof(data), 0);
	count = do_write(&dsp, data, 10000);
	assert(count == 10000);
	assert(hw.starts[0] == 0);
	space = ospace(&dsp);
	assert(space.bytes == RING - 10000);
	assert(space.fragments == (RING - 10000) / FRAGMENT);

	/* Crossing half a ring starts it. */
	count = do_write(&dsp, data + 10000, 8000);
	assert(count == 8000);
	assert(hw.starts[0] == 1 && hw.prepares[0] == 1);

	/* Delay is the written bytes the hardware has not played. */
	error = do_ioctl(&dsp, KERN_AUDIO_GET_DELAY, &delay);
	assert(error == 0 && delay == 18000);
	fake_advance(0, FRAGMENT, 1);
	error = do_ioctl(&dsp, KERN_AUDIO_GET_DELAY, &delay);
	assert(error == 0 && delay == 18000 - FRAGMENT);
	space = ospace(&dsp);
	assert(space.bytes == RING - 18000 + FRAGMENT);
	assert(space.transferred == FRAGMENT);

	/* A position inside the fragment counts, but room waits for the interrupt. */
	fake_advance(0, 1000, 0);
	error = do_ioctl(&dsp, KERN_AUDIO_GET_DELAY, &delay);
	assert(error == 0 && delay == 18000 - FRAGMENT - 1000);
	space = ospace(&dsp);
	assert(space.bytes == RING - 18000 + FRAGMENT);
	fake_advance(0, FRAGMENT - 1000, 1);

	/* Fill to the brim across the ring's end; a nonblocking write then stops. */
	atomic_store_release(&dsp.file.f_flags, (unsigned)(O_WRONLY | O_NONBLOCK));
	count = do_write(&dsp, data + 18000, sizeof(data) - 18000);
	assert(count == RING - 18000 + 2 * FRAGMENT);
	count = do_write(&dsp, data, 4);
	assert(count == -EAGAIN);
	assert(!(poll_node(&dsp, POLLOUT) & POLLOUT));

	/* Play everything; the log must be the pattern, in order, across wraps. */
	fake_advance(0, RING, 1);
	assert(poll_node(&dsp, POLLOUT) & POLLOUT);
	assert(hw.played_bytes == 2 * FRAGMENT + RING);
	assert(is_pattern(hw.played, hw.played_bytes, 0));

	/* A blocking write larger than the room completes while the hardware plays. */
	atomic_store_release(&dsp.file.f_flags, (unsigned)O_WRONLY);
	sleep_mode = SLEEP_PLAY;
	count = do_write(&dsp, data + 2 * FRAGMENT + RING, RING * 2);
	assert(count == RING * 2);
	assert(sleeps > 0);
	sleep_mode = SLEEP_REFUSE;
	fake_advance(0, RING, 1);
	assert(is_pattern(hw.played, 2 * FRAGMENT + RING * 3, 0));

	/* Close drains through two silent fragments while the hardware plays. */
	sleep_mode = SLEEP_PLAY;
	close_node(&dsp);
	sleep_mode = SLEEP_REFUSE;
	assert(is_silent(hw.played + 2 * FRAGMENT + RING * 3, hw.played_bytes - (2 * FRAGMENT + RING * 3)));
	assert(live_dma == 0);
	unregister_device(device);
	assert(live_allocations == 0);
	printf("audio ring: room, start at half, delay, wrap, blocking and nonblocking write PASS\n");
}

static void
test_underrun(void)
{
	struct drv_audio_device *device;
	struct test_file dsp;
	struct audio_space space;
	static uint8_t data[RING];
	size_t before;
	ssize_t count;

	reset_hw();
	device = register_device(&full_ops);
	assert(open_node(&dsp, "dsp0", O_WRONLY) == 0);

	pattern(data, sizeof(data), 0);
	count = do_write(&dsp, data, RING / 2);
	assert(count == RING / 2);
	assert(hw.running[0]);

	/* Play past the written data: one underrun, then silence. */
	fake_advance(0, RING / 2 + 3 * FRAGMENT, 1);
	space = ospace(&dsp);
	assert(space.underruns == 1);
	fake_advance(0, 2 * FRAGMENT, 1);
	space = ospace(&dsp);
	assert(space.underruns == 1);
	assert(hw.running[0] && hw.stops[0] == 0);
	assert(is_pattern(hw.played, RING / 2, 0));
	assert(is_silent(hw.played + RING / 2, hw.played_bytes - RING / 2));

	/* New data after an underrun plays soon, and a second episode counts again. */
	before = hw.played_bytes;
	pattern(data, sizeof(data), 5000);
	count = do_write(&dsp, data, FRAGMENT * 2);
	assert(count == FRAGMENT * 2);
	fake_advance(0, 4 * FRAGMENT, 1);
	assert(contains(hw.played + before, hw.played_bytes - before, data, FRAGMENT * 2));
	space = ospace(&dsp);
	assert(space.underruns == 2);

	sleep_mode = SLEEP_PLAY;
	close_node(&dsp);
	sleep_mode = SLEEP_REFUSE;
	unregister_device(device);
	assert(live_allocations == 0);
	printf("audio underrun: counted once per episode, silence played, recovery PASS\n");
}

/* Advances the capture hardware a whole ring during a reader's copy. */
static void
overtake_capture(void)
{
	fake_advance(1, RING, 1);
}

static void
test_capture_and_overrun(void)
{
	struct drv_audio_device *device;
	struct test_file dsp;
	struct audio_space space;
	static uint8_t data[RING];
	uint16_t first;
	ssize_t count;

	reset_hw();
	device = register_device(&full_ops);
	assert(open_node(&dsp, "dsp0", O_RDONLY) == 0);
	atomic_store_release(&dsp.file.f_flags, (unsigned)(O_RDONLY | O_NONBLOCK));

	/* The first read starts capture and finds nothing yet. */
	count = do_read(&dsp, data, 4);
	assert(count == -EAGAIN);
	assert(hw.starts[1] == 1);
	assert(!(poll_node(&dsp, POLLIN) & POLLIN));

	/* One fragment arrives; POLLIN and the data follow. */
	fake_advance(1, FRAGMENT, 1);
	assert(poll_node(&dsp, POLLIN) & POLLIN);
	space = ispace(&dsp);
	assert(space.bytes == FRAGMENT && space.overruns == 0);
	count = do_read(&dsp, data, sizeof(data));
	assert(count == FRAGMENT);
	assert(is_pattern(data, FRAGMENT, 0));

	/* A partial frame is refused. */
	assert(do_read(&dsp, data, 2) == -EINVAL);

	/* The reader falls a whole ring behind: one overrun, and it skips ahead. */
	fake_advance(1, RING, 1);
	space = ispace(&dsp);
	assert(space.overruns == 1);
	assert(space.bytes == RING / 2);
	count = do_read(&dsp, data, sizeof(data));
	assert(count == RING / 2);
	first = (uint16_t)(data[0] | (data[1] << 8));
	assert(first == (FRAGMENT + RING / 2) / 2);
	assert(is_pattern(data, RING / 2, first));

	/* The hardware overtakes a copy: that copy is dropped, not returned damaged. */
	fake_advance(1, FRAGMENT * 2, 1);
	copy_hook = overtake_capture;
	count = do_read(&dsp, data, sizeof(data));
	assert(count > 0);
	first = (uint16_t)(data[0] | (data[1] << 8));
	assert(is_pattern(data, (size_t)count, first));
	space = ispace(&dsp);
	assert(space.overruns == 2);

	/* A blocking read waits for the hardware. */
	atomic_store_release(&dsp.file.f_flags, (unsigned)O_RDONLY);
	sleep_mode = SLEEP_PLAY;
	count = do_read(&dsp, data, FRAGMENT * 3);
	assert(count == FRAGMENT * 3);
	first = (uint16_t)(data[0] | (data[1] << 8));
	assert(is_pattern(data, FRAGMENT * 3, first));
	sleep_mode = SLEEP_REFUSE;

	/* A signal ends a blocking read with nothing read. */
	sleep_mode = SLEEP_SIGNAL;
	count = do_read(&dsp, data, FRAGMENT);
	assert(count == -EINTR);
	sleep_mode = SLEEP_REFUSE;

	/* FLUSH stops capture; the next read starts it again. */
	assert(do_ioctl(&dsp, KERN_AUDIO_FLUSH, NULL) == 0);
	assert(hw.stops[1] == 1 && !hw.running[1]);
	atomic_store_release(&dsp.file.f_flags, (unsigned)(O_RDONLY | O_NONBLOCK));
	assert(do_read(&dsp, data, 4) == -EAGAIN);
	assert(hw.starts[1] == 2);

	close_node(&dsp);
	assert(hw.stops[1] == 2);
	unregister_device(device);
	assert(live_allocations == 0 && live_dma == 0);
	printf("audio capture: start on read, poll, overrun skip, overtaken copy dropped, flush PASS\n");
}

static void
test_format_and_frames(void)
{
	struct drv_audio_device *device;
	struct test_file dsp;
	struct audio_format format;
	struct audio_buffer_info info;
	static uint8_t data[FRAGMENT];

	reset_hw();
	device = register_device(&full_ops);
	assert(open_node(&dsp, "dsp0", O_RDWR) == 0);

	/* The default is the first listed format. */
	assert(do_ioctl(&dsp, KERN_AUDIO_GET_FORMAT, &format) == 0);
	assert(format.format == KERN_AUDIO_FORMAT_S16_LE && format.channels == 2 && format.rate == 48000);

	/* An exact match is accepted and changes the frame size. */
	format.format = KERN_AUDIO_FORMAT_S32_LE;
	assert(do_ioctl(&dsp, KERN_AUDIO_SET_FORMAT, &format) == 0);
	assert(do_ioctl(&dsp, KERN_AUDIO_GET_BUFFER, &info) == 0);
	assert(info.bytes_per_frame == 8);
	assert(do_write(&dsp, data, 4) == -EINVAL);

	/* No rounding: a near miss is refused and the current format comes back. */
	format.format = KERN_AUDIO_FORMAT_S16_LE;
	format.channels = 2;
	format.rate = 44100;
	assert(do_ioctl(&dsp, KERN_AUDIO_SET_FORMAT, &format) == EINVAL);
	assert(format.format == KERN_AUDIO_FORMAT_S32_LE && format.rate == 48000);
	format.rate = 48000;
	format.format = KERN_AUDIO_FORMAT_S16_LE;
	format.reserved = 1;
	assert(do_ioctl(&dsp, KERN_AUDIO_SET_FORMAT, &format) == EINVAL);

	/* Mono 16-bit frames are two bytes; three bytes is a partial frame. */
	assert(format.reserved == 0 && format.format == KERN_AUDIO_FORMAT_S32_LE);
	format.format = KERN_AUDIO_FORMAT_S16_LE;
	format.channels = 1;
	format.rate = 8000;
	assert(do_ioctl(&dsp, KERN_AUDIO_SET_FORMAT, &format) == 0);
	assert(do_write(&dsp, data, 3) == -EINVAL);
	assert(do_write(&dsp, data, 2) == 2);

	/* Pending data makes a format change busy. */
	format.channels = 2;
	format.rate = 48000;
	assert(do_ioctl(&dsp, KERN_AUDIO_SET_FORMAT, &format) == EBUSY);
	assert(do_ioctl(&dsp, KERN_AUDIO_FLUSH, NULL) == 0);
	assert(do_ioctl(&dsp, KERN_AUDIO_SET_FORMAT, &format) == 0);

	/* Unknown requests. */
	assert(do_ioctl(&dsp, KERN_AUDIO_GET_VOLUME, &format) == ENOTTY);

	close_node(&dsp);

	/* A new open starts again from the default format. */
	assert(open_node(&dsp, "dsp0", O_WRONLY) == 0);
	assert(do_ioctl(&dsp, KERN_AUDIO_GET_FORMAT, &format) == 0);
	assert(format.format == KERN_AUDIO_FORMAT_S16_LE && format.channels == 2);
	close_node(&dsp);

	unregister_device(device);
	assert(live_allocations == 0);
	printf("audio format: exact match, no rounding, frame size, busy change PASS\n");
}

static void
test_open_rules(void)
{
	struct drv_audio_device *device;
	struct drv_audio_device *playback_only;
	struct test_file dsp;
	struct test_file other;
	struct test_file mixer;
	uint8_t data[8] = { 0 };

	reset_hw();
	device = register_device(&full_ops);

	/* One open at a time. */
	assert(open_node(&dsp, "dsp0", O_WRONLY) == 0);
	assert(open_node(&other, "dsp0", O_RDONLY) == EBUSY);

	/* The mixer opens while the stream is open. */
	assert(open_node(&mixer, "mixer0", O_RDONLY) == 0);

	/* Wrong directions. */
	assert(do_read(&dsp, data, 4) == -EBADF);
	assert(do_ioctl(&dsp, KERN_AUDIO_GET_ISPACE, data) == EBADF);

	/* Unregister is busy while the stream is open. */
	assert(drv_audio_unregister(device) == EBUSY);

	/* Close frees the device for the next open. */
	close_node(&dsp);
	assert(open_node(&other, "dsp0", O_RDONLY) == 0);
	close_node(&other);

	/*
	 * A device without capture refuses a capture open.  O_RDWR takes the
	 * directions the device has, so a playback-only device can still be
	 * mapped (the system maps no write-only device open); read is then EBADF.
	 */
	current_device = NULL;
	assert(drv_audio_register(&plain_ops, &hw, FAKE_DMA, &playback_only) == 0);
	assert(open_node(&other, "dsp1", O_RDONLY) == ENODEV);
	assert(open_node(&other, "dsp1", O_RDWR) == 0);
	assert(do_read(&other, data, 4) == -EBADF);
	close_node(&other);
	assert(open_node(&other, "dsp1", O_WRONLY) == 0);
	close_node(&other);
	assert(drv_audio_unregister(playback_only) == 0);

	/* After unregister a still-open mixer is refused, and its node frees the device. */
	unregister_device(device);
	assert(do_ioctl(&mixer, KERN_AUDIO_GET_VOLUME, data) == ENODEV);
	assert(node_find("dsp0") == NULL);
	assert(live_allocations != 0);
	close_node(&mixer);
	assert(live_allocations == 0);
	printf("audio open: single open EBUSY, directions, busy unregister, stale mixer PASS\n");
}

static void
test_drain_and_flush(void)
{
	struct drv_audio_device *device;
	struct test_file dsp;
	struct audio_space space;
	static uint8_t data[RING];
	uint64_t start;
	ssize_t count;

	reset_hw();
	device = register_device(&full_ops);
	assert(open_node(&dsp, "dsp0", O_WRONLY) == 0);
	pattern(data, sizeof(data), 0);

	/* A short sound below a fragment: DRAIN starts it, plays it and two silent fragments, stops. */
	count = do_write(&dsp, data, 1000);
	assert(count == 1000 && hw.starts[0] == 0);
	sleep_mode = SLEEP_PLAY;
	assert(do_ioctl(&dsp, KERN_AUDIO_DRAIN, NULL) == 0);
	sleep_mode = SLEEP_REFUSE;
	assert(hw.starts[0] == 1 && hw.stops[0] == 1 && !hw.running[0]);
	assert(hw.played_bytes == 3 * FRAGMENT);
	assert(is_pattern(hw.played, 1000, 0));
	assert(is_silent(hw.played + 1000, 3 * FRAGMENT - 1000));
	space = ospace(&dsp);
	assert(space.bytes == RING && space.underruns == 0);
	assert(space.transferred == 3 * FRAGMENT);

	/* A drained stream starts again at the next half ring. */
	count = do_write(&dsp, data, RING / 2);
	assert(count == RING / 2 && hw.starts[0] == 2);

	/* FLUSH discards: stops at once, the room comes back, nothing more plays. */
	assert(do_ioctl(&dsp, KERN_AUDIO_FLUSH, NULL) == 0);
	assert(hw.stops[0] == 2);
	space = ospace(&dsp);
	assert(space.bytes == RING);
	assert(space.transferred == 3 * FRAGMENT);

	/* A signal ends DRAIN without stopping the stream. */
	count = do_write(&dsp, data, RING / 2);
	assert(count == RING / 2 && hw.running[0]);
	sleep_mode = SLEEP_SIGNAL;
	assert(do_ioctl(&dsp, KERN_AUDIO_DRAIN, NULL) == EINTR);
	assert(hw.running[0]);

	/* Stuck hardware: DRAIN gives up after its deadline and stops. */
	sleep_mode = SLEEP_STUCK;
	start = ticks;
	assert(do_ioctl(&dsp, KERN_AUDIO_DRAIN, NULL) == EIO);
	assert(!hw.running[0] && hw.stops[0] == 3);
	assert(ticks - start >= KERN_MS_TO_TICKS(2000U));

	/* Stuck hardware at close: close returns within two seconds and frees the device. */
	count = do_write(&dsp, data, RING / 2);
	assert(count == RING / 2 && hw.running[0]);
	start = ticks;
	close_node(&dsp);
	assert(ticks - start >= KERN_MS_TO_TICKS(2000U));
	assert(ticks - start <= KERN_MS_TO_TICKS(2000U) + 1);
	assert(!hw.running[0]);
	sleep_mode = SLEEP_REFUSE;
	assert(open_node(&dsp, "dsp0", O_WRONLY) == 0);

	/* Close of a healthy stream plays out what was written. */
	hw.played_bytes = 0;
	count = do_write(&dsp, data, RING / 2 + 100);
	assert(count == RING / 2 + 100);
	sleep_mode = SLEEP_PLAY;
	close_node(&dsp);
	sleep_mode = SLEEP_REFUSE;
	assert(is_pattern(hw.played, RING / 2 + 100, 0));
	assert(live_dma == 0);

	unregister_device(device);
	assert(live_allocations == 0);
	printf("audio drain: short sound, pad, restart, flush, EINTR, stuck deadline, close bound PASS\n");
}

static void
test_positions(void)
{
	struct drv_audio_device *device;
	struct test_file dsp;
	struct audio_space space;
	static uint8_t data[RING];
	ssize_t count;

	reset_hw();
	device = register_device(&full_ops);
	assert(open_node(&dsp, "dsp0", O_WRONLY) == 0);
	count = do_write(&dsp, data, RING);
	assert(count == RING);

	/* Two and a half fragments with one interrupt: the missed ones still count. */
	fake_advance(0, FRAGMENT, 1);
	fake_advance(0, FRAGMENT + FRAGMENT / 2, 0);
	space = ospace(&dsp);
	assert(space.transferred == 2 * FRAGMENT + FRAGMENT / 2);

	/* A position just short of an interrupt it already raised is not a ring ahead. */
	fake_advance(0, FRAGMENT / 2 - 4, 0);
	drv_audio_interrupt(device, 0);
	drv_audio_interrupt(device, 0);
	space = ospace(&dsp);
	assert(space.transferred == 3 * FRAGMENT);

	/* The count never goes backwards. */
	fake_advance(0, 4, 0);
	space = ospace(&dsp);
	assert(space.transferred == 3 * FRAGMENT);

	/* An interrupt for an idle stream changes nothing. */
	assert(do_ioctl(&dsp, KERN_AUDIO_FLUSH, NULL) == 0);
	drv_audio_interrupt(device, 0);
	drv_audio_interrupt(device, 1);
	space = ospace(&dsp);
	assert(space.bytes == RING);

	close_node(&dsp);
	unregister_device(device);
	printf("audio positions: missed interrupt, lagging position, monotonic, idle interrupt PASS\n");
}

static void
test_poll_wake(void)
{
	struct drv_audio_device *device;
	struct test_file dsp;
	static uint8_t data[RING];
	unsigned before;

	reset_hw();
	device = register_device(&full_ops);
	assert(open_node(&dsp, "dsp0", O_WRONLY) == 0);
	assert(poll_node(&dsp, POLLOUT | POLLIN) == POLLOUT);
	assert(do_write(&dsp, data, RING) == RING);
	assert(poll_node(&dsp, POLLOUT) == 0);

	/* An interrupt notifies poll and gives back a fragment of room. */
	before = poll_notifications;
	fake_advance(0, FRAGMENT, 1);
	assert(poll_notifications > before);
	assert(poll_node(&dsp, POLLOUT) == POLLOUT);

	sleep_mode = SLEEP_PLAY;
	close_node(&dsp);
	sleep_mode = SLEEP_REFUSE;
	unregister_device(device);
	printf("audio poll: POLLOUT by room, interrupt wakes poll PASS\n");
}

static void
test_volume(void)
{
	struct drv_audio_device *device;
	struct test_file mixer;
	struct test_file reader;
	struct audio_volume volume;

	/* A backend without volume: unadjusted level, and SET is ENOTSUP. */
	reset_hw();
	device = register_device(&plain_ops);
	assert(open_node(&mixer, "mixer0", O_RDWR) == 0);
	assert(do_ioctl(&mixer, KERN_AUDIO_GET_VOLUME, &volume) == 0);
	assert(volume.left == 100 && volume.right == 100 && volume.muted == 0);
	volume.left = 50;
	assert(do_ioctl(&mixer, KERN_AUDIO_SET_VOLUME, &volume) == ENOTSUP);
	close_node(&mixer);
	unregister_device(device);

	/* A backend with volume receives valid requests only. */
	reset_hw();
	device = register_device(&full_ops);
	assert(open_node(&mixer, "mixer0", O_RDWR) == 0);
	assert(open_node(&reader, "mixer0", O_RDONLY) == 0);
	volume.left = 30;
	volume.right = 70;
	volume.muted = 1;
	volume.reserved = 0;
	assert(do_ioctl(&mixer, KERN_AUDIO_SET_VOLUME, &volume) == 0);
	assert(hw.volume_sets == 1 && hw.volume.left == 30 && hw.volume.right == 70 && hw.volume.muted == 1);
	memset(&volume, 0, sizeof(volume));
	assert(do_ioctl(&reader, KERN_AUDIO_GET_VOLUME, &volume) == 0);
	assert(volume.left == 30 && volume.muted == 1);
	volume.left = 101;
	assert(do_ioctl(&mixer, KERN_AUDIO_SET_VOLUME, &volume) == EINVAL);
	volume.left = 10;
	volume.muted = 2;
	assert(do_ioctl(&mixer, KERN_AUDIO_SET_VOLUME, &volume) == EINVAL);
	volume.muted = 0;
	assert(do_ioctl(&reader, KERN_AUDIO_SET_VOLUME, &volume) == EBADF);
	assert(do_ioctl(&mixer, KERN_AUDIO_DRAIN, NULL) == ENOTTY);
	assert(hw.volume_sets == 1);
	close_node(&reader);
	close_node(&mixer);
	unregister_device(device);
	assert(live_allocations == 0);
	printf("audio volume: ENOTSUP without backend volume, validation, read-only mixer PASS\n");
}

static void
test_start_failure(void)
{
	struct drv_audio_device *device;
	struct test_file dsp;
	static uint8_t data[RING];

	reset_hw();
	device = register_device(&full_ops);
	assert(open_node(&dsp, "dsp0", O_WRONLY) == 0);
	hw.start_error = EIO;

	/* The bytes accepted before the failed start are reported. */
	assert(do_write(&dsp, data, RING / 2) == RING / 2);
	assert(!hw.running[0]);
	assert(do_write(&dsp, data, RING / 2) == RING / 2);
	assert(!hw.running[0]);

	/* A full ring with no hardware reports the start error instead of sleeping. */
	assert(do_write(&dsp, data, 4) == -EIO);
	hw.start_error = 0;
	sleep_mode = SLEEP_PLAY;
	close_node(&dsp);
	sleep_mode = SLEEP_REFUSE;
	assert(hw.stops[0] == 1);
	assert(live_dma == 0);
	unregister_device(device);
	assert(live_allocations == 0);
	printf("audio start failure: accepted bytes reported, full ring reports EIO, close still frees PASS\n");
}

/* ws035-p049: fills the mapping from filled up to target, a fragment at a time. */
static void
app_fill(uint8_t *shadow, uint64_t *filled, uint64_t target)
{
	while (*filled < target) {
		pattern(shadow + (*filled % RING), FRAGMENT, (uint32_t)(*filled / 2));
		*filled += FRAGMENT;
	}
}

static int
do_mmap(struct test_file *opened, off_t offset, size_t bytes, uint32_t prot,
    struct vm_device_mapping **mapping)
{
	return opened->node->cdev.ops->mmap(&opened->file, offset, bytes, prot, mapping);
}

static void
test_mmap_playback(void)
{
	struct drv_audio_device *device;
	struct vm_device_mapping *mapping;
	struct vm_device_mapping *second;
	struct audio_mmap_position position;
	struct audio_caps caps;
	struct test_file dsp;
	uint8_t chunk[FRAGMENT];
	uint64_t filled;
	uint32_t bits;
	uint8_t *shadow;
	unsigned index;
	int error;

	reset_hw();
	device = register_device(&full_ops);
	error = open_node(&dsp, "dsp0", O_RDWR);
	assert(error == 0);

	/* The node reports a copied mapping of one ring, two fragments ahead. */
	error = do_ioctl(&dsp, KERN_AUDIO_GET_CAPS, &caps);
	assert(error == 0);
	assert(caps.caps == (KERN_AUDIO_CAP_MMAP | KERN_AUDIO_CAP_TRIGGER |
	    KERN_AUDIO_CAP_MMAP_COPY));
	assert(caps.mmap_bytes == RING && caps.mmap_lead_bytes == 2U * FRAGMENT);

	/* Bad ranges and rights are refused; the whole ring maps read-write. */
	assert(do_mmap(&dsp, 4096, RING, KERN_PROT_READ, &mapping) == EINVAL);
	assert(do_mmap(&dsp, 0, 100, KERN_PROT_READ, &mapping) == EINVAL);
	assert(do_mmap(&dsp, 0, RING * 2, KERN_PROT_READ, &mapping) == EINVAL);
	assert(do_mmap(&dsp, 0, RING, KERN_PROT_EXEC, &mapping) == EACCES);
	error = do_mmap(&dsp, 0, RING, KERN_PROT_READ | KERN_PROT_WRITE, &mapping);
	assert(error == 0);
	assert(mapping->bytes == RING);
	assert(mapping->max_prot == (KERN_PROT_READ | KERN_PROT_WRITE));
	shadow = mapping->address;
	assert(is_silent(shadow, RING));

	/* A mapped direction refuses write and drain; an unmapped position request fails before. */
	memset(chunk, 0, sizeof(chunk));
	assert(do_write(&dsp, chunk, sizeof(chunk)) == -EBUSY);
	assert(do_ioctl(&dsp, KERN_AUDIO_DRAIN, NULL) == EBUSY);
	bits = KERN_AUDIO_TRIGGER_INPUT;
	assert(do_ioctl(&dsp, KERN_AUDIO_SET_TRIGGER, &bits) == EINVAL);

	/* The caller fills four fragments, then the trigger takes the first two. */
	filled = 0;
	app_fill(shadow, &filled, 4U * FRAGMENT);
	bits = KERN_AUDIO_TRIGGER_OUTPUT;
	error = do_ioctl(&dsp, KERN_AUDIO_SET_TRIGGER, &bits);
	assert(error == 0);
	assert(hw.starts[0] == 1 && hw.running[0] == 1);
	assert(is_pattern(hw.ring[0], 2U * FRAGMENT, 0));
	assert(is_silent(shadow, 2U * FRAGMENT));
	assert(is_pattern(shadow + 2U * FRAGMENT, 2U * FRAGMENT, FRAGMENT));

	error = do_ioctl(&dsp, KERN_AUDIO_GET_OPTR, &position);
	assert(error == 0);
	assert(position.bytes == 2U * FRAGMENT && position.fragments == 2U);
	assert(position.offset == 2U * FRAGMENT);
	assert((poll_node(&dsp, POLLOUT) & POLLOUT) == 0);

	/* A caller that keeps four fragments ahead hears one continuous count. */
	for (index = 0; index < 20U; index++) {
		error = do_ioctl(&dsp, KERN_AUDIO_GET_OPTR, &position);
		assert(error == 0);
		app_fill(shadow, &filled, position.bytes + 4U * FRAGMENT);
		fake_advance(0, FRAGMENT, 1);
		assert((poll_node(&dsp, POLLOUT) & POLLOUT) != 0);
	}
	assert(hw.played_bytes == 20U * FRAGMENT);
	assert(is_pattern(hw.played, 20U * FRAGMENT, 0));

	/* A caller that stops filling is heard as silence, not as an old ring. */
	for (index = 0; index < 10U; index++)
		fake_advance(0, FRAGMENT, 1);
	assert(is_silent(hw.played + hw.played_bytes - 4U * FRAGMENT, 4U * FRAGMENT));

	/* The trigger stops it; close neither drains nor leaks. */
	bits = 0;
	error = do_ioctl(&dsp, KERN_AUDIO_SET_TRIGGER, &bits);
	assert(error == 0);
	assert(hw.running[0] == 0);
	mapping_drop(mapping);
	close_node(&dsp);
	assert(live_pmem == 0);

	/* A direction write has moved cannot be mapped afterwards. */
	error = open_node(&dsp, "dsp0", O_WRONLY);
	assert(error == 0);
	memset(chunk, 0, sizeof(chunk));
	assert(do_write(&dsp, chunk, sizeof(chunk)) == (ssize_t)sizeof(chunk));
	assert(do_mmap(&dsp, 0, RING, KERN_PROT_WRITE, &mapping) == EBUSY);
	assert(do_ioctl(&dsp, KERN_AUDIO_GET_OPTR, &position) == EINVAL);
	sleep_mode = SLEEP_PLAY;
	close_node(&dsp);

	/* The protection picks the ring; an open lacking that direction is refused. */
	error = open_node(&dsp, "dsp0", O_WRONLY);
	assert(error == 0);
	assert(do_mmap(&dsp, 0, RING, KERN_PROT_READ, &mapping) == EINVAL);
	close_node(&dsp);
	error = open_node(&dsp, "dsp0", O_RDONLY);
	assert(error == 0);
	assert(do_mmap(&dsp, 0, RING, KERN_PROT_WRITE, &mapping) == EACCES);
	close_node(&dsp);

	/* An O_RDWR open maps both rings and one trigger starts both. */
	error = open_node(&dsp, "dsp0", O_RDWR);
	assert(error == 0);
	error = do_mmap(&dsp, 0, RING, KERN_PROT_READ | KERN_PROT_WRITE, &mapping);
	assert(error == 0 && mapping->max_prot == (KERN_PROT_READ | KERN_PROT_WRITE));
	error = do_mmap(&dsp, 0, RING, KERN_PROT_READ, &second);
	assert(error == 0 && second->max_prot == KERN_PROT_READ);
	assert(second->address != mapping->address);
	bits = KERN_AUDIO_TRIGGER_OUTPUT | KERN_AUDIO_TRIGGER_INPUT;
	error = do_ioctl(&dsp, KERN_AUDIO_SET_TRIGGER, &bits);
	assert(error == 0);
	assert(hw.running[0] == 1 && hw.running[1] == 1);
	fake_advance(1, FRAGMENT, 1);
	error = do_ioctl(&dsp, KERN_AUDIO_GET_IPTR, &position);
	assert(error == 0 && position.bytes == FRAGMENT);
	mapping_drop(second);
	mapping_drop(mapping);
	close_node(&dsp);
	assert(live_pmem == 0);

	unregister_device(device);
	printf("audio mmap playback: PASS\n");
}

static void
test_mmap_capture(void)
{
	struct drv_audio_device *device;
	struct vm_device_mapping *mapping;
	struct audio_mmap_position position;
	struct test_file dsp;
	uint8_t chunk[FRAGMENT];
	uint32_t bits;
	uint8_t *shadow;
	int error;

	reset_hw();
	device = register_device(&full_ops);
	error = open_node(&dsp, "dsp0", O_RDONLY);
	assert(error == 0);

	/* Capture maps for reading only. */
	assert(do_mmap(&dsp, 0, RING, KERN_PROT_READ | KERN_PROT_WRITE, &mapping) == EACCES);
	error = do_mmap(&dsp, 0, RING, KERN_PROT_READ, &mapping);
	assert(error == 0);
	assert(mapping->max_prot == KERN_PROT_READ);
	shadow = mapping->address;
	assert(do_read(&dsp, chunk, sizeof(chunk)) == -EBUSY);

	/* Three completed fragments are in the mapping, in order. */
	bits = KERN_AUDIO_TRIGGER_INPUT;
	error = do_ioctl(&dsp, KERN_AUDIO_SET_TRIGGER, &bits);
	assert(error == 0);
	assert(hw.running[1] == 1);
	fake_advance(1, 3U * FRAGMENT, 1);
	assert((poll_node(&dsp, POLLIN) & POLLIN) != 0);
	error = do_ioctl(&dsp, KERN_AUDIO_GET_IPTR, &position);
	assert(error == 0);
	assert(position.bytes == 3U * FRAGMENT && position.fragments == 3U);
	assert(position.offset == 3U * FRAGMENT);
	assert(is_pattern(shadow, 3U * FRAGMENT, 0));
	assert((poll_node(&dsp, POLLIN) & POLLIN) == 0);

	/* Past a whole ring the mapping wraps and the count continues. */
	fake_advance(1, RING, 1);
	error = do_ioctl(&dsp, KERN_AUDIO_GET_IPTR, &position);
	assert(error == 0);
	assert(position.bytes == 3U * FRAGMENT + RING && position.fragments == 8U);
	assert(is_pattern(shadow, 3U * FRAGMENT, RING / 2U));
	assert(is_pattern(shadow + 3U * FRAGMENT, RING - 3U * FRAGMENT,
	    3U * FRAGMENT / 2U));

	mapping_drop(mapping);
	close_node(&dsp);
	assert(live_pmem == 0);
	unregister_device(device);
	printf("audio mmap capture: PASS\n");
}

int
main(void)
{
	test_registration();
	test_ring_space_and_wrap();
	test_underrun();
	test_capture_and_overrun();
	test_format_and_frames();
	test_open_rules();
	test_drain_and_flush();
	test_positions();
	test_poll_wake();
	test_volume();
	test_start_failure();
	test_mmap_playback();
	test_mmap_capture();
	assert(held_spinlocks == 0);
	assert(live_allocations == 0 && live_dma == 0 && live_pmem == 0);
	printf("audio framework: all PASS\n");
	return 0;
}
