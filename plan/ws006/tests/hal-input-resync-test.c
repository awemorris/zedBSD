/* WS006 IN-T35: bounded physical HAL resync streams. */
/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include <hal/hal.h>

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#if defined(TEST_PCAT)
void pcat_input_ownership_test_reset(void);
void pcat_input_ownership_test_key(unsigned, unsigned, int);
void pcat_input_ownership_test_caps(int);
void pcat_input_ownership_test_rebuild(void);
void pcat_input_ownership_test_repeat(const char *);
int pcat_input_ownership_test_pop(struct hal_key_event *);
#define test_reset pcat_input_ownership_test_reset
#define test_rebuild pcat_input_ownership_test_rebuild
#define test_pop pcat_input_ownership_test_pop
#elif defined(TEST_PC98)
void pc98_input_ownership_test_reset(void);
void pc98_input_ownership_test_raw(uint8_t);
void pc98_input_ownership_test_rebuild(void);
int pc98_input_ownership_test_pop(struct hal_key_event *);
#define test_reset pc98_input_ownership_test_reset
#define test_rebuild pc98_input_ownership_test_rebuild
#define test_pop pc98_input_ownership_test_pop
#elif defined(TEST_X68K)
void x68k_input_ownership_test_reset(void);
void x68k_input_ownership_test_raw(uint8_t);
void x68k_input_ownership_test_rebuild(void);
int x68k_input_ownership_test_pop(struct hal_key_event *);
#define test_reset x68k_input_ownership_test_reset
#define test_rebuild x68k_input_ownership_test_rebuild
#define test_pop x68k_input_ownership_test_pop
#else
#error select a physical HAL fixture
#endif

static int
snapshot(const struct hal_key_event *event)
{
	return event->flags ==
	    (HAL_KEY_EVENT_PRESS | HAL_KEY_EVENT_SNAPSHOT);
}

static void
assert_complete_stream(int expect_caps, const char *held,
	const char *absent)
{
	struct hal_key_event event;
	unsigned count = 0;
	int saw_held = 0, saw_end = 0;

	assert(test_pop(&event));
	assert((event.flags & HAL_KEY_EVENT_RESYNC) != 0);
	assert(((event.flags & HAL_KEY_EVENT_LOCK_CAPS) != 0) == expect_caps);
	while (test_pop(&event)) {
		count++;
		if (event.flags == HAL_KEY_EVENT_RESYNC_END) {
			saw_end = 1;
			break;
		}
		assert(snapshot(&event));
		if (strcmp(event.symbol, held) == 0)
			saw_held = 1;
		assert(absent == NULL || strcmp(event.symbol, absent) != 0);
	}
	assert(saw_end && saw_held);
	/* No held-key universe can exceed 256 scans plus the end marker. */
	assert(count <= 257U);
}

static void
test_modifier_first_and_restart(void)
{
	struct hal_key_event event;

	test_reset();
#if defined(TEST_PCAT)
	pcat_input_ownership_test_caps(1);
	pcat_input_ownership_test_key(0, 0x2a, 1); /* left shift */
	pcat_input_ownership_test_key(0, 0x1e, 1); /* a */
#elif defined(TEST_PC98)
	pc98_input_ownership_test_raw(0x70); /* left shift */
	pc98_input_ownership_test_raw(0x71); /* caps lock held + locked */
	pc98_input_ownership_test_raw(0x1d); /* a */
#else
	x68k_input_ownership_test_raw(0x70); /* left shift */
	x68k_input_ownership_test_raw(0x5d); /* caps lock held + locked */
	x68k_input_ownership_test_raw(0x1e); /* a */
#endif
	test_rebuild();
	assert(test_pop(&event));
	assert((event.flags & HAL_KEY_EVENT_RESYNC) != 0);
	assert((event.flags & HAL_KEY_EVENT_LOCK_CAPS) != 0);
	assert(test_pop(&event));
	assert(snapshot(&event));
	assert(strcmp(event.symbol, "leftshift") == 0 ||
	    strcmp(event.symbol, "capslock") == 0);

	/* A second overflow/rebuild discards the partial prior transaction. */
#if defined(TEST_PCAT)
	pcat_input_ownership_test_key(0, 0x2a, 0);
	pcat_input_ownership_test_key(0, 0x1e, 0);
	pcat_input_ownership_test_key(0, 0x30, 1); /* b */
#elif defined(TEST_PC98)
	pc98_input_ownership_test_raw(0xf0);
	pc98_input_ownership_test_raw(0x9d);
	pc98_input_ownership_test_raw(0x2d);
#else
	x68k_input_ownership_test_raw(0xf0);
	x68k_input_ownership_test_raw(0x9e);
	x68k_input_ownership_test_raw(0x2e);
#endif
	test_rebuild();
	assert_complete_stream(1, "b", "a");
}

static void
test_actual_ring_overflow(void)
{
	unsigned index;

	test_reset();
#if defined(TEST_PCAT)
	pcat_input_ownership_test_key(0, 0x1e, 1);
	for (index = 0; index < 600U; index++)
		pcat_input_ownership_test_repeat("a");
#elif defined(TEST_PC98)
	for (index = 0; index < 400U; index++)
		pc98_input_ownership_test_raw(0x1d); /* make/repeat a */
#else
	for (index = 0; index < 400U; index++)
		x68k_input_ownership_test_raw(0x1e); /* make/repeat a */
#endif
	assert_complete_stream(0, "a", NULL);
}

int
main(void)
{
	test_modifier_first_and_restart();
	test_actual_ring_overflow();
#if defined(TEST_PCAT)
	puts("WS006 PC/AT physical HAL resync: PASS");
#elif defined(TEST_PC98)
	puts("WS006 PC-98 physical HAL resync: PASS");
#else
	puts("WS006 X68000 physical HAL resync: PASS");
#endif
	return 0;
}
