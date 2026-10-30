/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The host test of the packet pool's two classes (src/kern/net/packet-buf.c
 * built as the kernel builds it): which class an allocation takes, the
 * jumbo reserve for the drivers' receive, the errors and counts of an
 * empty class, constant-time free back to the slot used next, and copies
 * of jumbo packets.  Built twice by run-packet-buf-host-test.sh: as a
 * 64-bit machine (jumbo packets) and as a 32-bit board (none).  Prints
 * "packet-buf-host-test: PASS" or the checks that failed.
 */

#include "kern/net/packet-buf.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <uapi/errno.h>

#include <stdint.h>

/* How many checks failed. */
static unsigned failures;

static void check(int condition, const char *what);
static void test_classes(void);
static void test_reserve(void);
static void test_standard_exhaustion(void);
static void test_hold_reserve(void);
static void test_copy(void);

/* The kcrt copies are the host's. */
void *
kern_memcpy(
	void *destination,
	const void *source,
	size_t count)
{
	return memcpy(destination, source, count);
}

/* The kcrt fill is the host's. */
void *
kern_memset(
	void *destination,
	int value,
	size_t count)
{
	return memset(destination, value, count);
}

/* The kernel's zeroed allocation is the host's. */
void *
kern_calloc(
	size_t count,
	size_t size)
{
	return calloc(count, size);
}

/* A 32-bit board's atomics enter and leave nothing on the host's one thread. */
uintptr_t
hal_atomic_enter(void)
{
	return 0;
}

/* Leaves what hal_atomic_enter entered. */
void
hal_atomic_leave(
	uintptr_t cookie)
{
	(void)cookie;
}

/* Runs every test and reports the result. */
int
main(void)
{
	/* Each test starts from an empty pool. */
	packet_buf_pool_init();
	test_classes();
	packet_buf_pool_init();
	test_reserve();
	packet_buf_pool_init();
	test_standard_exhaustion();
	packet_buf_pool_init();
	test_hold_reserve();
	packet_buf_pool_init();
	test_copy();

	/* The verdict. */
	if (failures != 0) {
		printf("packet-buf-host-test: FAIL (%u)\n", failures);
		return 1;
	}

	/* Succeeded: every check held. */
	printf("packet-buf-host-test: PASS (jumbo %u, reserve %u)\n", PACKET_BUF_JUMBO_COUNT, PACKET_BUF_JUMBO_RESERVE);
	return 0;
}

/* Counts and prints one failed check. */
static void
check(
	int condition,
	const char *what)
{
	/* A check that holds says nothing. */
	if (condition)
		return;
	printf("  FAIL: %s\n", what);
	failures++;
}

/* Which class each size takes, and what no class holds. */
static void
test_classes(void)
{
	struct packet_buf_pool_usage usage;
	struct packet_buf *small;
	struct packet_buf *large;
	struct packet_buf *again;
	struct packet_buf *none;
	unsigned jumbo_expected;
	int error;

	/* A standard frame takes a standard packet. */
	small = packet_buf_alloc_for(PACKET_BUF_DEFAULT_HEADROOM, 1514U, 0, &error);
	check(small != NULL && error == 0 && small->capacity == PACKET_BUF_STORAGE_SIZE, "1514 bytes take a standard packet");
	check(small != NULL && packet_buf_append(small, 1514U) != NULL, "a standard packet takes 1514 bytes after its headroom");

	/* A jumbo frame takes a jumbo packet, or none where there are none. */
	large = packet_buf_alloc_for(PACKET_BUF_DEFAULT_HEADROOM, 9014U, 0, &error);
	if (PACKET_BUF_JUMBO_COUNT != 0U) {
		check(large != NULL && error == 0 && large->capacity == PACKET_BUF_JUMBO_STORAGE_SIZE,
		    "9014 bytes take a jumbo packet");
		check(large != NULL && packet_buf_append(large, 9014U) != NULL, "a jumbo packet takes 9014 bytes");
	} else {
		check(large == NULL && error == EMSGSIZE, "no jumbo packets: 9014 bytes are EMSGSIZE");
	}

	/* Beyond the largest packet is EMSGSIZE. */
	none = packet_buf_alloc_for(PACKET_BUF_DEFAULT_HEADROOM, PACKET_BUF_MAX_STORAGE_SIZE, PACKET_BUF_ALLOC_RECEIVE,
	    &error);
	check(none == NULL && error == EMSGSIZE, "beyond the largest packet is EMSGSIZE");

	/* The counts see both. */
	packet_buf_pool_usage(&usage);
	check(usage.used == 1U, "one standard packet is counted");
	jumbo_expected = 0U;
	if (PACKET_BUF_JUMBO_COUNT != 0U)
		jumbo_expected = 1U;
	check(usage.jumbo_used == jumbo_expected, "the jumbo packet is counted");
	check(packet_buf_in_use() == usage.used + usage.jumbo_used, "in use counts both classes");

	/* A freed slot is the next one taken. */
	packet_buf_free(small);
	again = packet_buf_alloc(0);
	check(again == small, "the slot freed last is taken next");
	packet_buf_free(again);
	packet_buf_free(large);
	packet_buf_pool_usage(&usage);
	check(usage.used == 0U && usage.jumbo_used == 0U, "every packet is back");
}

/* The jumbo packets a sender may take, and the reserve for a receive. */
static void
test_reserve(void)
{
	struct packet_buf_pool_usage usage;
	struct packet_buf *taken[PACKET_BUF_JUMBO_COUNT + 1U];
	struct packet_buf *extra;
	unsigned index;
	unsigned count;
	int error;

	/* Nothing to test without jumbo packets. */
	if (PACKET_BUF_JUMBO_COUNT == 0U)
		return;

	/* A sender takes all but the reserve. */
	count = 0;
	for (index = 0; index < PACKET_BUF_JUMBO_COUNT; index++) {
		taken[count] = packet_buf_alloc_for(0, 4000U, 0, &error);
		if (taken[count] == NULL)
			break;
		count++;
	}

	/* The sender stopped at the reserve. */
	check(count == PACKET_BUF_JUMBO_COUNT - PACKET_BUF_JUMBO_RESERVE, "a sender takes all but the reserve");
	check(error == ENOBUFS, "a sender past the reserve is ENOBUFS");

	/* A receive takes the reserve. */
	for (index = 0; index < PACKET_BUF_JUMBO_RESERVE; index++) {
		taken[count] = packet_buf_alloc_for(0, 4000U, PACKET_BUF_ALLOC_RECEIVE, &error);
		check(taken[count] != NULL, "a receive takes a reserved jumbo packet");
		if (taken[count] != NULL)
			count++;
	}

	/* None is left, not even to a receive, and the standard class is untouched. */
	extra = packet_buf_alloc_for(0, 4000U, PACKET_BUF_ALLOC_RECEIVE, &error);
	check(extra == NULL && error == ENOBUFS, "an empty jumbo class is ENOBUFS");
	extra = packet_buf_alloc_for(0, 1000U, 0, &error);
	check(extra != NULL, "an empty jumbo class leaves the standard class");
	packet_buf_free(extra);

	/* The counts. */
	packet_buf_pool_usage(&usage);
	check(usage.jumbo_used == PACKET_BUF_JUMBO_COUNT && usage.jumbo_used_max == PACKET_BUF_JUMBO_COUNT,
	    "every jumbo packet is counted taken");
	check(usage.jumbo_alloc_failures == 2U, "two jumbo failures are counted");
	check(usage.jumbo_reserve == PACKET_BUF_JUMBO_RESERVE && usage.jumbo_total == PACKET_BUF_JUMBO_COUNT,
	    "the reserve and the total are reported");

	/* Freed, a sender may take them again. */
	for (index = 0; index < count; index++)
		packet_buf_free(taken[index]);
	extra = packet_buf_alloc_for(0, 4000U, 0, &error);
	check(extra != NULL, "freed jumbo packets are taken again");
	packet_buf_free(extra);
	packet_buf_pool_usage(&usage);
	check(usage.jumbo_used == 0U, "every jumbo packet is back");
}

/* An empty standard class does not fall back to the jumbo one. */
static void
test_standard_exhaustion(void)
{
	struct packet_buf_pool_usage usage;
	struct packet_buf *taken[PACKET_BUF_POOL_COUNT];
	struct packet_buf *extra;
	unsigned index;
	int error;

	/* Every standard packet taken. */
	for (index = 0; index < PACKET_BUF_POOL_COUNT; index++) {
		taken[index] = packet_buf_alloc(PACKET_BUF_DEFAULT_HEADROOM);
		check(taken[index] != NULL, "every standard packet can be taken");
	}

	/* The next one fails both ways, and the jumbo class is not used for it. */
	extra = packet_buf_alloc(0);
	check(extra == NULL, "an empty standard class gives none");
	extra = packet_buf_alloc_for(0, 100U, 0, &error);
	check(extra == NULL && error == ENOBUFS, "an empty standard class is ENOBUFS, not a jumbo packet");
	packet_buf_pool_usage(&usage);
	check(usage.alloc_failures == 2U && usage.jumbo_used == 0U, "the failures are the standard class's");
	check(usage.used_max == PACKET_BUF_POOL_COUNT, "the most taken is the whole class");

	/* All given back. */
	for (index = 0; index < PACKET_BUF_POOL_COUNT; index++)
		packet_buf_free(taken[index]);
	packet_buf_pool_usage(&usage);
	check(usage.used == 0U, "every standard packet is back");
}

/*
 * The copies a sender holds (PACKET_BUF_ALLOC_HOLD) leave the
 * last PACKET_BUF_HOLD_RESERVE standard packets, which the drivers'
 * receive and the other callers still take.
 */
static void
test_hold_reserve(void)
{
	struct packet_buf_pool_usage usage;
	struct packet_buf *taken[PACKET_BUF_POOL_COUNT];
	struct packet_buf *extra;
	unsigned held;
	unsigned count;
	int error;

	/* Holders take all but the reserve. */
	held = 0;
	for (;;) {
		extra = packet_buf_alloc_for(64U, 1460U, PACKET_BUF_ALLOC_HOLD, &error);
		if (extra == NULL)
			break;
		taken[held++] = extra;
	}

	/* The holders stopped at the reserve. */
	check(held == PACKET_BUF_POOL_COUNT - PACKET_BUF_HOLD_RESERVE, "holders take all but the reserve");
	check(error == ENOBUFS, "a holder refused is ENOBUFS");
	packet_buf_pool_usage(&usage);
	check(usage.alloc_failures == 1U, "the holder refused is counted");

	/* The reserve goes to a receive and to a caller that does not hold. */
	count = held;
	extra = packet_buf_alloc_for(0, 1514U, PACKET_BUF_ALLOC_RECEIVE, &error);
	check(extra != NULL, "a receive takes a reserved packet");
	if (extra != NULL)
		taken[count++] = extra;
	while (count < PACKET_BUF_POOL_COUNT) {
		extra = packet_buf_alloc_for(64U, 100U, 0, &error);
		check(extra != NULL, "a passing packet takes a reserved one");
		if (extra == NULL)
			break;
		taken[count++] = extra;
	}

	/* All given back. */
	while (count != 0)
		packet_buf_free(taken[--count]);
	packet_buf_pool_usage(&usage);
	check(usage.used == 0U, "every standard packet is back");
}

/* A copy of a jumbo packet is a jumbo packet with the same bytes. */
static void
test_copy(void)
{
	struct packet_buf *source;
	struct packet_buf *copy;
	struct packet_buf *region;
	uint8_t *data;
	unsigned index;
	int same;
	int error;

	/* Nothing to test without jumbo packets. */
	if (PACKET_BUF_JUMBO_COUNT == 0U)
		return;

	/* A jumbo frame with a pattern. */
	source = packet_buf_alloc_for(PACKET_BUF_DEFAULT_HEADROOM, 9000U, PACKET_BUF_ALLOC_RECEIVE, &error);
	check(source != NULL, "a jumbo source");
	if (source == NULL)
		return;
	data = packet_buf_append(source, 9000U);
	for (index = 0; index < 9000U; index++)
		data[index] = (uint8_t)(index * 7U);

	/* The whole copy and a region of it. */
	copy = packet_buf_copy(source);
	check(copy != NULL && copy->length == 9000U && copy->capacity == PACKET_BUF_JUMBO_STORAGE_SIZE,
	    "a copy of a jumbo packet is jumbo");
	same = copy != NULL;
	for (index = 0; same && index < 9000U; index++)
		same = copy->data[index] == data[index];
	check(same, "the copy has the same bytes");
	region = packet_buf_copy_region(source, PACKET_BUF_DEFAULT_HEADROOM, 3000U);
	check(region != NULL && region->length == 3000U && region->capacity == PACKET_BUF_JUMBO_STORAGE_SIZE,
	    "a 3000-byte region is jumbo");

	/* Given back. */
	packet_buf_free(region);
	packet_buf_free(copy);
	packet_buf_free(source);
	check(packet_buf_in_use() == 0U, "every packet is back after the copies");
}

/*
 * The kernel's lock-word claim and spin hint, which the kernel core defines
 * with the HAL.  The host's exchange stands in for the port's sequence.
 */
int
atomic_raw_try_acquire_zero(
	volatile unsigned *value)
{
	return __atomic_exchange_n(value, 1U, __ATOMIC_ACQUIRE) == 0U;
}

/* The host has no spin instruction to give. */
void
atomic_spin_hint(
	void)
{
}
