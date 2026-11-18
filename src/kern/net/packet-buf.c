/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Packet buffers.
 *
 * Packets come from a fixed pool of slots with inline storage, guarded by
 * a spin lock that also disables interrupts because drivers free packets
 * from interrupt context.  A packet has headroom before its data and
 * tailroom after it; the layer offsets index into the storage so that
 * copies keep them.  The HAL and device hooks are weak so that host tests
 * can use the pool without them.
 */

#include "kern/net/packet-buf.h"
#include "kern/net/net-device.h"
#include "kern/test-fault.h"
#include "kern/kmem.h"
#include <kern/kcrt.h>
#include <hal/types.h>

#include <uapi/errno.h>
#include <stdbool.h>

/* The HAL, the devices and the log are optional, so that host tests can use the pool without them. */
extern bool hal_irq_disable(void) __attribute__((weak));
extern void hal_irq_enable(void) __attribute__((weak));
extern void net_device_ref(struct net_device *) __attribute__((weak));
extern void net_device_release(struct net_device *) __attribute__((weak));
extern void kern_logf(const char *format, ...) __attribute__((weak, format(printf, 1, 2)));
extern int hal_pmem_alloc(size_t req_size, size_t req_align, hal_physaddr_t *block) __attribute__((weak));
extern void *hal_pmem_to_kernel(hal_physaddr_t paddr) __attribute__((weak));

/*
 * A standard packet's slot and a jumbo packet's: the packet, its inline
 * storage, and whether it is taken.
 */
struct packet_slot {
	struct packet_buf packet;
	uint8_t storage[PACKET_BUF_STORAGE_SIZE];
	uint8_t used;
};

struct packet_jumbo_slot {
	struct packet_buf packet;
	uint8_t storage[PACKET_BUF_JUMBO_STORAGE_SIZE];
	uint8_t used;
};

/*
 * The slots of both classes and, for each, a stack of the free slots'
 * indices: an allocation takes the top one and a free puts its slot back
 * on top, so both take constant time and the slot used last, still warm
 * in the cache, is used next.  The arrays are allocated once, when the
 * pool is first emptied (packet_buf_pool_init), rather than kept in the
 * kernel's image: a 64-bit machine's pool is a few megabytes, which would
 * count against the image's size limit (amd64's 16 MiB).  packet_count
 * and packet_jumbo_count are the slots each class got: PACKET_BUF_POOL_COUNT
 * and PACKET_BUF_JUMBO_COUNT, or fewer when memory was short (halved until
 * an allocation succeeds).  The arrays and counts are set before the
 * network runs and never change; the pool lock guards the slots' used
 * flags, the stacks and the counts below.
 */
static struct packet_slot *packet_pool;
static struct packet_jumbo_slot *packet_jumbo_pool;
static uint16_t *packet_free_stack;
static uint16_t *packet_jumbo_free_stack;
static unsigned packet_count;
static unsigned packet_jumbo_count;

/*
 * The physical addresses the two arrays start at, when they came straight
 * from physical memory (0: not known, as in a host test): a driver that
 * can reach the memory hands a packet's bytes to its device without
 * copying them (packet_buf_physical).  Set with the arrays.
 */
static hal_physaddr_t packet_pool_physical;
static hal_physaddr_t packet_jumbo_pool_physical;
static unsigned packet_free_count;
static unsigned packet_jumbo_free_count;
static unsigned packet_used;
static unsigned packet_jumbo_used;

/*
 * The most packets of each class ever taken at once, and how many
 * allocations found the class empty (the jumbo class: empty but for the
 * reserve), since the pool was emptied (net.tcpstats shows
 * whether a transfer ran the stack out of packets).  They only grow and
 * are moved under the pool lock with the counts above.
 */
static unsigned packet_used_max;
static unsigned packet_jumbo_used_max;
static uint64_t packet_alloc_failures;
static uint64_t packet_jumbo_alloc_failures;
static atomic_uint_t packet_guard;

static bool packet_lock(void);
static void packet_unlock(bool enabled);
static void packet_pool_allocate(void);
static void *packet_pool_block(size_t bytes, hal_physaddr_t *physical);
static struct packet_buf *packet_take_standard_locked(unsigned flags);
static struct packet_buf *packet_take_jumbo_locked(unsigned flags);
static void packet_reset(struct packet_buf *packet, uint8_t *storage, size_t capacity, size_t headroom);

/*
 * Empties the packet pool, allocating its arrays the first time.
 */
void
packet_buf_pool_init(
	void)
{
	bool enabled;
	unsigned index;

	/* The arrays, once; a pool that got none has no packets. */
	if (packet_pool == NULL)
		packet_pool_allocate();

	enabled = packet_lock();

	if (packet_pool != NULL)
		kern_memset(packet_pool, 0, (size_t)packet_count * sizeof(packet_pool[0]));
	if (packet_jumbo_pool != NULL)
		kern_memset(packet_jumbo_pool, 0, (size_t)packet_jumbo_count * sizeof(packet_jumbo_pool[0]));

	/* Stacks every slot of each class, the first one on top. */
	for (index = 0; index < packet_count; index++)
		packet_free_stack[index] = (uint16_t)(packet_count - 1U - index);
	for (index = 0; index < packet_jumbo_count; index++)
		packet_jumbo_free_stack[index] = (uint16_t)(packet_jumbo_count - 1U - index);
	packet_free_count = packet_count;
	packet_jumbo_free_count = packet_jumbo_count;

	packet_used = 0;
	packet_jumbo_used = 0;
	packet_used_max = 0;
	packet_jumbo_used_max = 0;
	packet_alloc_failures = 0;
	packet_jumbo_alloc_failures = 0;

	packet_unlock(enabled);
}

/*
 * Allocates an empty standard packet with the requested headroom.
 *
 * The test fault point lets a test make the pool look exhausted.
 */
struct packet_buf *
packet_buf_alloc(
	size_t headroom)
{
	struct packet_buf *packet;
	struct kern_test_fault_result fault;
	bool enabled;

	/* Rejects headroom beyond the slot storage. */
	if (headroom > PACKET_BUF_STORAGE_SIZE)
		return NULL;

	/* Fails on demand under test. */
	if (KERN_TEST_FAULT(KERN_TEST_FAULT_NET_PACKET_ALLOC, UINT32_MAX,
	    UINT32_MAX, &fault))
		return NULL;

	/* Takes the top free slot. */
	enabled = packet_lock();

	packet = packet_take_standard_locked(0);

	packet_unlock(enabled);

	/* Reports none when the pool is exhausted. */
	if (packet == NULL)
		return NULL;

	/* Succeeded: an empty packet with the headroom. */
	packet_reset(packet, packet->storage, PACKET_BUF_STORAGE_SIZE, headroom);
	return packet;
}

/*
 * Allocates an empty packet with the headroom and room for length bytes
 * after it: a standard packet when they fit one, else a jumbo packet.
 *
 * A standard class with none free does not fall back to the jumbo class,
 * nor the other way.  The last PACKET_BUF_JUMBO_RESERVE jumbo packets go
 * only to a caller passing PACKET_BUF_ALLOC_RECEIVE, and the last
 * PACKET_BUF_HOLD_RESERVE standard ones never to one passing
 * PACKET_BUF_ALLOC_HOLD.  On failure *error
 * says why: EMSGSIZE when no packet is that large, ENOBUFS when the class
 * has none free.
 */
struct packet_buf *
packet_buf_alloc_for(
	size_t headroom,
	size_t length,
	unsigned flags,
	int *error)
{
	struct packet_buf *packet;
	struct kern_test_fault_result fault;
	size_t capacity;
	bool enabled;
	bool injected;
	int jumbo;

	/* Refuses a size no packet holds. */
	if (headroom > PACKET_BUF_MAX_STORAGE_SIZE ||
	    length > PACKET_BUF_MAX_STORAGE_SIZE - headroom) {
		*error = EMSGSIZE;
		return NULL;
	}

	/* The class: jumbo only for what a standard packet cannot hold, and only where there are jumbo packets. */
	jumbo = headroom + length > PACKET_BUF_STORAGE_SIZE;
	if (jumbo && PACKET_BUF_JUMBO_COUNT == 0U) {
		*error = EMSGSIZE;
		return NULL;
	}

	/* Fails on demand under test, as an empty pool. */
	injected = KERN_TEST_FAULT(KERN_TEST_FAULT_NET_PACKET_ALLOC, UINT32_MAX, UINT32_MAX, &fault);
	if (injected) {
		*error = ENOBUFS;
		return NULL;
	}

	/* Takes the top free slot of the class. */
	enabled = packet_lock();

	if (jumbo)
		packet = packet_take_jumbo_locked(flags);
	else
		packet = packet_take_standard_locked(flags);

	packet_unlock(enabled);

	/* Reports a class with none free. */
	if (packet == NULL) {
		*error = ENOBUFS;
		return NULL;
	}

	/* Succeeded: an empty packet of the class with the headroom. */
	capacity = PACKET_BUF_STORAGE_SIZE;
	if (jumbo)
		capacity = PACKET_BUF_JUMBO_STORAGE_SIZE;
	packet_reset(packet, packet->storage, capacity, headroom);
	*error = 0;
	return packet;
}

/*
 * Takes a reference on a packet.
 */
void
packet_buf_ref(
	struct packet_buf *packet)
{
	bool enabled;

	/* Ignores a missing packet. */
	if (packet == NULL)
		return;

	/* Counts the reference under the pool lock. */
	enabled = packet_lock();
	refcount_get(&packet->refcount);
	packet_unlock(enabled);
}

/*
 * Drops a reference on a packet and returns the last one to the pool.
 *
 * The device reference and the control block are released outside the
 * pool lock.
 */
void
packet_buf_free(
	struct packet_buf *packet)
{
	struct net_device *device;
	void *control;
	void (*control_release)(void *);
	uintptr_t address;
	uintptr_t first;
	uintptr_t end;
	size_t index;
	bool enabled;

	device = NULL;
	control = NULL;
	control_release = NULL;

	/* Ignores a missing packet. */
	if (packet == NULL)
		return;

	/* Only the last reference frees the slot. */
	enabled = packet_lock();
	if (!refcount_put(&packet->refcount)) {
		packet_unlock(enabled);
		return;
	}

	/* Detaches the device and the control block, which are released outside the lock. */
	device = packet->device;
	control = packet->control;
	control_release = packet->control_release;
	packet->device = NULL;
	packet->control = NULL;
	packet->control_release = NULL;

	/*
	 * Puts a standard packet's slot, found from the packet's address, back
	 * on its stack.  A class whose array was never allocated (no jumbo
	 * packets on a 32-bit board, or memory short) has no range to look in.
	 */
	address = (uintptr_t)packet;
	if (packet_pool != NULL) {
		first = (uintptr_t)&packet_pool[0];
		end = (uintptr_t)&packet_pool[packet_count];
		if (address >= first && address < end) {
			index = (address - first) / sizeof(packet_pool[0]);
			if (&packet_pool[index].packet == packet && packet_pool[index].used != 0) {
				packet_pool[index].used = 0;
				packet_free_stack[packet_free_count] = (uint16_t)index;
				packet_free_count++;
				packet_used--;
			}
		}
	}

	/* The same for a jumbo packet. */
	if (packet_jumbo_pool != NULL) {
		first = (uintptr_t)&packet_jumbo_pool[0];
		end = (uintptr_t)&packet_jumbo_pool[packet_jumbo_count];
		if (address >= first && address < end) {
			index = (address - first) / sizeof(packet_jumbo_pool[0]);
			if (&packet_jumbo_pool[index].packet == packet && packet_jumbo_pool[index].used != 0) {
				packet_jumbo_pool[index].used = 0;
				packet_jumbo_free_stack[packet_jumbo_free_count] = (uint16_t)index;
				packet_jumbo_free_count++;
				packet_jumbo_used--;
			}
		}
	}

	packet_unlock(enabled);

	/* Releases what the packet held. */
	if (device != NULL && net_device_release != NULL)
		net_device_release(device);
	if (control != NULL && control_release != NULL)
		control_release(control);
}

/*
 * Reports the bytes available before the packet data.
 */
size_t
packet_buf_headroom(
	const struct packet_buf *packet)
{
	/* A missing or inconsistent packet has no headroom. */
	if (packet == NULL || packet->data < packet->storage)
		return 0;

	/* Reports the distance from the storage to the data. */
	return (size_t)(packet->data - packet->storage);
}

/*
 * Reports the bytes available after the packet data.
 */
size_t
packet_buf_tailroom(
	const struct packet_buf *packet)
{
	size_t headroom;

	/* A missing or inconsistent packet has no tailroom. */
	if (packet == NULL)
		return 0;
	headroom = packet_buf_headroom(packet);
	if (headroom > packet->capacity ||
	    packet->length > packet->capacity - headroom)
		return 0;

	/* Reports the capacity left after the headroom and the data. */
	return packet->capacity - headroom - packet->length;
}

/*
 * Extends the packet data into the headroom.
 */
void *
packet_buf_push(
	struct packet_buf *packet,
	size_t length)
{
	/* Rejects a missing packet or more than the headroom. */
	if (packet == NULL || length > packet_buf_headroom(packet))
		return NULL;

	/* Moves the data start back. */
	packet->data -= length;
	packet->length += length;

	/* Reports the new data start. */
	return packet->data;
}

/*
 * Removes bytes from the front of the packet data.
 */
void *
packet_buf_pull(
	struct packet_buf *packet,
	size_t length)
{
	void *data;

	/* Rejects a missing packet or more than the data. */
	if (packet == NULL || length > packet->length)
		return NULL;

	/* Moves the data start forward. */
	packet->data += length;
	packet->length -= length;
	data = packet->data;

	/* Reports the new data start. */
	return data;
}

/*
 * Extends the packet data into the tailroom.
 */
void *
packet_buf_append(
	struct packet_buf *packet,
	size_t length)
{
	void *tail;

	/* Rejects a missing packet or more than the tailroom. */
	if (packet == NULL || length > packet_buf_tailroom(packet))
		return NULL;

	/* Extends the data. */
	tail = packet->data + packet->length;
	packet->length += length;

	/* Reports the start of the appended bytes. */
	return tail;
}

/*
 * Shortens the packet data to a length.
 */
int
packet_buf_trim(
	struct packet_buf *packet,
	size_t length)
{
	/* Rejects a missing packet or a length beyond the data. */
	if (packet == NULL || length > packet->length)
		return EINVAL;

	packet->length = length;

	/* Reports the trimmed packet. */
	return 0;
}

/*
 * Copies a packet with its headroom, offsets, device, and source address.
 */
struct packet_buf *
packet_buf_copy(
	const struct packet_buf *source)
{
	struct packet_buf *copy;
	size_t headroom;
	void *data;
	int error;

	/* There is nothing to copy without a source. */
	if (source == NULL)
		return NULL;

	/* Allocates a packet of the class the data needs, with the same headroom, and copies the data. */
	headroom = packet_buf_headroom(source);
	copy = packet_buf_alloc_for(headroom, source->length, 0, &error);
	if (copy == NULL)
		return NULL;
	data = packet_buf_append(copy, source->length);
	if (data == NULL) {
		packet_buf_free(copy);
		return NULL;
	}

	kern_memcpy(data, source->data, source->length);

	/* Copies the layer offsets, protocol, flags, device, and source. */
	copy->l2_offset = source->l2_offset;
	copy->l3_offset = source->l3_offset;
	copy->l4_offset = source->l4_offset;
	copy->l3_length = source->l3_length;
	copy->protocol = source->protocol;
	copy->flags = source->flags;
	copy->device = source->device;
	if (copy->device != NULL && net_device_ref != NULL)
		net_device_ref(copy->device);
	copy->source_length = source->source_length;
	kern_memcpy(copy->source_address, source->source_address,
	    sizeof(copy->source_address));

	/* Reports the copy. */
	return copy;
}

/*
 * Copies a region of a packet's storage into a new packet without headroom.
 */
struct packet_buf *
packet_buf_copy_region(
	const struct packet_buf *source,
	size_t offset,
	size_t length)
{
	struct packet_buf *copy;
	void *data;
	int error;

	/* Rejects a missing source or a region outside its storage. */
	if (source == NULL ||
	    offset > source->capacity ||
	    length > source->capacity - offset)
		return NULL;

	/* Allocates a packet of the class the region needs and copies the region as its data. */
	copy = packet_buf_alloc_for(0, length, 0, &error);
	if (copy == NULL)
		return NULL;
	data = packet_buf_append(copy, length);
	if (data == NULL) {
		packet_buf_free(copy);
		return NULL;
	}

	kern_memcpy(data, source->storage + offset, length);

	/* Copies the protocol, flags, device, and source. */
	copy->protocol = source->protocol;
	copy->flags = source->flags;
	copy->device = source->device;
	if (copy->device != NULL && net_device_ref != NULL)
		net_device_ref(copy->device);
	copy->source_length = source->source_length;
	kern_memcpy(copy->source_address, source->source_address,
	    sizeof(copy->source_address));

	/* Reports the copy. */
	return copy;
}

/*
 * Reports the number of packets in use.
 */
unsigned
packet_buf_in_use(
	void)
{
	bool enabled;
	unsigned result;

	/* Samples the counts of both classes under the pool lock. */
	enabled = packet_lock();
	result = packet_used + packet_jumbo_used;
	packet_unlock(enabled);

	/* Reports the sampled count. */
	return result;
}

/*
 * Reports each class's size, the packets taken now and the most ever
 * taken, how many allocations found no packet free, and the jumbo reserve.
 */
void
packet_buf_pool_usage(
	struct packet_buf_pool_usage *usage)
{
	bool enabled;

	/* Samples the counts together under the pool lock. */
	enabled = packet_lock();

	usage->total = packet_count;
	usage->used = packet_used;
	usage->used_max = packet_used_max;
	usage->alloc_failures = packet_alloc_failures;
	usage->jumbo_total = packet_jumbo_count;
	usage->jumbo_used = packet_jumbo_used;
	usage->jumbo_used_max = packet_jumbo_used_max;
	usage->jumbo_reserve = PACKET_BUF_JUMBO_RESERVE;
	usage->jumbo_alloc_failures = packet_jumbo_alloc_failures;

	packet_unlock(enabled);
}

/* Disables interrupts, when the HAL is present, and takes the pool lock. */
static bool
packet_lock(
	void)
{
	bool enabled;

	/* Without the HAL there are no interrupts to disable. */
	if (hal_irq_disable != NULL)
		enabled = hal_irq_disable();
	else
		enabled = false;

	/* Spins for the lock. */
	while (!atomic_try_acquire_zero(&packet_guard))
		__asm__ volatile("" ::: "memory");

	/* Reports whether interrupts were enabled. */
	return enabled;
}

/* Releases the pool lock and restores the interrupt state. */
static void
packet_unlock(
	bool enabled)
{
	atomic_store_release(&packet_guard, 0);

	/* Re-enables interrupts only when they were enabled before. */
	if (enabled && hal_irq_enable != NULL)
		hal_irq_enable();
}

/*
 * Takes the top free standard slot and counts it, or counts the failure.
 * The caller holds the pool lock.
 */
static struct packet_buf *
packet_take_standard_locked(
	unsigned flags)
{
	struct packet_slot *slot;
	unsigned left;

	/* The slots this caller may take: those beyond the reserve for one that holds its packet. */
	left = packet_free_count;
	if ((flags & PACKET_BUF_ALLOC_HOLD) != 0) {
		if (left > PACKET_BUF_HOLD_RESERVE)
			left -= PACKET_BUF_HOLD_RESERVE;
		else
			left = 0;
	}

	/* None left to this caller is counted, for net.tcpstats. */
	if (left == 0) {
		packet_alloc_failures++;
		return NULL;
	}

	/* The slot on top of the stack, marked taken. */
	packet_free_count--;
	slot = &packet_pool[packet_free_stack[packet_free_count]];
	slot->used = 1;
	packet_used++;
	if (packet_used > packet_used_max)
		packet_used_max = packet_used;

	/* Succeeded: the slot's packet, its storage pointer set. */
	slot->packet.storage = slot->storage;
	return &slot->packet;
}

/*
 * Takes the top free jumbo slot and counts it, or counts the failure.  The
 * last PACKET_BUF_JUMBO_RESERVE are only for a driver's receive.  The
 * caller holds the pool lock.
 */
static struct packet_buf *
packet_take_jumbo_locked(
	unsigned flags)
{
	struct packet_jumbo_slot *slot;
	unsigned left;

	/* The slots this caller may take: all for a receive, those beyond the reserve otherwise. */
	left = packet_jumbo_free_count;
	if ((flags & PACKET_BUF_ALLOC_RECEIVE) == 0) {
		if (left > PACKET_BUF_JUMBO_RESERVE)
			left -= PACKET_BUF_JUMBO_RESERVE;
		else
			left = 0;
	}

	/* None left to this caller is counted, for net.tcpstats. */
	if (left == 0) {
		packet_jumbo_alloc_failures++;
		return NULL;
	}

	/* The slot on top of the stack, marked taken. */
	packet_jumbo_free_count--;
	slot = &packet_jumbo_pool[packet_jumbo_free_stack[packet_jumbo_free_count]];
	slot->used = 1;
	packet_jumbo_used++;
	if (packet_jumbo_used > packet_jumbo_used_max)
		packet_jumbo_used_max = packet_jumbo_used;

	/* Succeeded: the slot's packet, its storage pointer set. */
	slot->packet.storage = slot->storage;
	return &slot->packet;
}

/*
 * Resets a packet just taken: no data, the headroom before it, no layer
 * offsets, one reference.
 */
static void
packet_reset(
	struct packet_buf *packet,
	uint8_t *storage,
	size_t capacity,
	size_t headroom)
{
	/* Clears every field, then sets the storage and the data after the headroom. */
	kern_memset(packet, 0, sizeof(*packet));
	packet->storage = storage;
	packet->data = storage + headroom;
	packet->capacity = capacity;
	packet->l2_offset = PACKET_OFFSET_NONE;
	packet->l3_offset = PACKET_OFFSET_NONE;
	packet->l4_offset = PACKET_OFFSET_NONE;
	refcount_init(&packet->refcount, 1);
}

/*
 * Allocates the arrays of both classes: as many slots as the build asks
 * for, halved until the memory is there (and reported when it was short),
 * down to none.
 */
static void
packet_pool_allocate(
	void)
{
	size_t bytes;
	unsigned count;

	/* The standard slots and their stack, in one block. */
	for (count = PACKET_BUF_POOL_COUNT; count != 0; count /= 2U) {
		bytes = (size_t)count * (sizeof(packet_pool[0]) + sizeof(uint16_t));
		packet_pool = packet_pool_block(bytes, &packet_pool_physical);
		if (packet_pool != NULL)
			break;
	}

	/* Records how many slots the block holds, and stacks them free. */
	packet_count = count;
	if (packet_pool != NULL)
		packet_free_stack = (uint16_t *)(void *)(packet_pool + count);

	/* The jumbo slots and their stack, where the build has them. */
	for (count = PACKET_BUF_JUMBO_COUNT; count != 0; count /= 2U) {
		bytes = (size_t)count * (sizeof(packet_jumbo_pool[0]) + sizeof(uint16_t));
		packet_jumbo_pool = packet_pool_block(bytes, &packet_jumbo_pool_physical);
		if (packet_jumbo_pool != NULL)
			break;
	}

	/* Records how many jumbo slots the block holds, and stacks them free. */
	packet_jumbo_count = count;
	if (packet_jumbo_pool != NULL)
		packet_jumbo_free_stack = (uint16_t *)(void *)(packet_jumbo_pool + count);

	/* A short pool is worth saying, where there is a log. */
	if ((packet_count != PACKET_BUF_POOL_COUNT || packet_jumbo_count != PACKET_BUF_JUMBO_COUNT) &&
	    kern_logf != NULL)
		kern_logf("net: packet pool short of memory: %u of %u packets, %u of %u jumbo\n", packet_count,
		    (unsigned)PACKET_BUF_POOL_COUNT, packet_jumbo_count, (unsigned)PACKET_BUF_JUMBO_COUNT);
}

/*
 * Takes one array's block: whole pages of physical memory, whose address
 * is kept, or (without the HAL, in a host test, or when that fails) the
 * kernel's allocator, with no address known.  The block is zeroed.
 */
static void *
packet_pool_block(
	size_t bytes,
	hal_physaddr_t *physical)
{
	hal_physaddr_t block;
	void *address;
	int error;

	/* Physical pages, mapped through the direct map, when the HAL is there. */
	*physical = 0;
	if (hal_pmem_alloc != NULL && hal_pmem_to_kernel != NULL) {
		error = hal_pmem_alloc(bytes, 4096U, &block);
		if (error == 0) {
			address = hal_pmem_to_kernel(block);
			if (address != NULL) {
				kern_memset(address, 0, bytes);
				*physical = block;
				return address;
			}
		}
	}

	/* The kernel's allocator otherwise. */
	address = kern_calloc(1, bytes);

	/* The block, or none. */
	return address;
}

/*
 * Reports the physical address of a packet's data, for a driver that
 * hands the bytes to its device in place.  ENOENT for a packet whose
 * pool's physical address is not known or that is not the pool's.
 */
int
packet_buf_physical(
	const struct packet_buf *packet,
	uint64_t *physical)
{
	uintptr_t address;
	uintptr_t first;
	uintptr_t end;

	/* A standard packet's data, by its distance from the array's start (an array never allocated has no address). */
	address = (uintptr_t)packet->data;
	if (packet_pool != NULL && packet_pool_physical != 0) {
		first = (uintptr_t)packet_pool;
		end = (uintptr_t)&packet_pool[packet_count];
		if (address >= first && address < end) {
			*physical = (uint64_t)packet_pool_physical + (address - first);
			return 0;
		}
	}

	/* The same for a jumbo packet. */
	if (packet_jumbo_pool != NULL && packet_jumbo_pool_physical != 0) {
		first = (uintptr_t)packet_jumbo_pool;
		end = (uintptr_t)&packet_jumbo_pool[packet_jumbo_count];
		if (address >= first && address < end) {
			*physical = (uint64_t)packet_jumbo_pool_physical + (address - first);
			return 0;
		}
	}

	/* Not known. */
	return ENOENT;
}
