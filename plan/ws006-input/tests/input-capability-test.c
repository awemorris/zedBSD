/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "kern/input-capability.h"

#include <assert.h>
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define ARRAY_COUNT(array) (sizeof(array) / sizeof((array)[0]))

static int
bit_is_set(const uint8_t *bits, size_t size, unsigned code)
{
	unsigned long word;
	size_t word_index;

	assert(bits != NULL);
	word_index = code / INPUT_BITS_PER_WORD;
	assert((word_index + 1U) * sizeof(word) <= size);
	memcpy(&word, bits + word_index * sizeof(word), sizeof(word));
	return (word & (1UL << (code % INPUT_BITS_PER_WORD))) != 0;
}

static void
expected_bit(uint8_t *bits, size_t size, unsigned code)
{
	unsigned long word;
	size_t word_index;

	word_index = code / INPUT_BITS_PER_WORD;
	assert((word_index + 1U) * sizeof(word) <= size);
	memcpy(&word, bits + word_index * sizeof(word), sizeof(word));
	word |= 1UL << (code % INPUT_BITS_PER_WORD);
	memcpy(bits + word_index * sizeof(word), &word, sizeof(word));
}

static void
expect_bits(const struct input_capability_state *state, uint16_t type,
	    size_t expected_size, const unsigned *codes, size_t code_count)
{
	uint8_t expected[INPUT_KEY_BITS_SIZE];
	const uint8_t *actual = NULL;
	size_t actual_size = 0;
	size_t index;

	assert(expected_size <= sizeof(expected));
	memset(expected, 0, sizeof(expected));
	for (index = 0; index < code_count; index++)
		expected_bit(expected, expected_size, codes[index]);
	assert(input_capability_bits(state, type, &actual, &actual_size) == 0);
	assert(actual != NULL);
	assert(actual_size == expected_size);
	assert(memcmp(actual, expected, expected_size) == 0);
}

static void
expect_abs_equal(const struct input_absinfo *actual,
		 const struct input_absinfo *expected)
{
	assert(actual->value == expected->value);
	assert(actual->minimum == expected->minimum);
	assert(actual->maximum == expected->maximum);
	assert(actual->fuzz == expected->fuzz);
	assert(actual->flat == expected->flat);
	assert(actual->resolution == expected->resolution);
}

static const struct input_capability valid_capabilities[] = {
	{EV_SYN, SYN_REPORT},
	{EV_KEY, KEY_6},
	{EV_KEY, KEY_7},
	{EV_KEY, KEY_A},
	{EV_KEY, BTN_LEFT},
	{EV_KEY, KEY_MAX},
	{EV_REL, REL_X},
	{EV_REL, REL_WHEEL},
	{EV_REL, REL_MAX},
	{EV_ABS, ABS_X},
	{EV_ABS, ABS_MT_SLOT},
	{EV_ABS, ABS_MAX},
};

static const struct input_abs_axis valid_axes[] = {
	{ABS_X, {.value = 320,
		 .minimum = 0,
		 .maximum = 639,
		 .fuzz = 1,
		 .flat = 2,
		 .resolution = 4}},
	{ABS_MT_SLOT, {.value = 0,
		       .minimum = 0,
		       .maximum = 9,
		       .fuzz = 0,
		       .flat = 0,
		       .resolution = 1}},
	{ABS_MAX, {.value = -10,
		   .minimum = -100,
		   .maximum = 100,
		   .fuzz = 3,
		   .flat = 4,
		   .resolution = 5}},
};

static void
test_bitsets_and_queries(void)
{
	static const unsigned event_types[] = {EV_SYN, EV_KEY, EV_REL, EV_ABS};
	static const unsigned key_codes[] = {
	    KEY_6, KEY_7, KEY_A, BTN_LEFT, KEY_MAX};
	static const unsigned rel_codes[] = {REL_X, REL_WHEEL, REL_MAX};
	static const unsigned abs_codes[] = {ABS_X, ABS_MT_SLOT, ABS_MAX};
	struct input_capability_state state;
	const uint8_t *pointer = (const uint8_t *)(uintptr_t)1U;
	size_t size = 123U;
	struct input_absinfo info;

	assert(input_capability_state_init(&state, valid_capabilities,
	    ARRAY_COUNT(valid_capabilities), valid_axes,
	    ARRAY_COUNT(valid_axes)) == 0);
	expect_bits(&state, 0, INPUT_EVENT_BITS_SIZE, event_types,
	    ARRAY_COUNT(event_types));
	expect_bits(&state, EV_KEY, INPUT_KEY_BITS_SIZE, key_codes,
	    ARRAY_COUNT(key_codes));
	expect_bits(&state, EV_REL, INPUT_REL_BITS_SIZE, rel_codes,
	    ARRAY_COUNT(rel_codes));
	expect_bits(&state, EV_ABS, INPUT_ABS_BITS_SIZE, abs_codes,
	    ARRAY_COUNT(abs_codes));

	assert(input_capability_bits(&state, EV_MSC, &pointer, &size) ==
	    EINVAL);
	assert(pointer == (const uint8_t *)(uintptr_t)1U && size == 123U);
	assert(input_capability_bits(NULL, EV_KEY, &pointer, &size) == EINVAL);
	assert(input_capability_bits(&state, EV_KEY, NULL, &size) == EINVAL);
	assert(input_capability_bits(&state, EV_KEY, &pointer, NULL) == EINVAL);

	memset(&info, 0xa5, sizeof(info));
	assert(input_capability_abs_info(&state, ABS_Y, &info) == ENOENT);
	for (size_t index = 0; index < sizeof(info); index++)
		assert(((const uint8_t *)&info)[index] == 0xa5U);
	assert(input_capability_abs_info(NULL, ABS_X, &info) == EINVAL);
	assert(input_capability_abs_info(&state, ABS_X, NULL) == EINVAL);
	assert(input_capability_abs_info(&state, ABS_MAX + 1U, &info) ==
	    EINVAL);
}

static void
test_copy_windows(void)
{
	static const uint8_t source[] = {0x11, 0x22, 0x33, 0x44};
	uint8_t destination[8];

	memset(destination, 0xa5, sizeof(destination));
	assert(input_capability_copy(source, sizeof(source), 0,
	    destination + 2, 2) == 0);
	assert(destination[0] == 0xa5 && destination[1] == 0xa5);
	assert(destination[2] == 0x11 && destination[3] == 0x22);
	assert(destination[4] == 0xa5 && destination[7] == 0xa5);

	memset(destination, 0xa5, sizeof(destination));
	assert(input_capability_copy(source, sizeof(source), 0,
	    destination + 2, sizeof(source)) == 0);
	assert(memcmp(destination + 2, source, sizeof(source)) == 0);
	assert(destination[1] == 0xa5 && destination[6] == 0xa5);

	memset(destination, 0xa5, sizeof(destination));
	assert(input_capability_copy(source, sizeof(source), 0,
	    destination + 1, 6) == 0);
	assert(memcmp(destination + 1, source, sizeof(source)) == 0);
	assert(destination[5] == 0 && destination[6] == 0);
	assert(destination[0] == 0xa5 && destination[7] == 0xa5);

	memset(destination, 0xa5, sizeof(destination));
	assert(input_capability_copy(source, sizeof(source), 2,
	    destination + 2, 4) == 0);
	assert(destination[2] == 0x33 && destination[3] == 0x44);
	assert(destination[4] == 0 && destination[5] == 0);
	assert(destination[1] == 0xa5 && destination[6] == 0xa5);

	memset(destination, 0xa5, sizeof(destination));
	assert(input_capability_copy(NULL, 0, 0, destination + 2, 3) == 0);
	assert(destination[2] == 0 && destination[3] == 0 &&
	    destination[4] == 0);
	assert(destination[1] == 0xa5 && destination[5] == 0xa5);

	memset(destination, 0xa5, sizeof(destination));
	assert(input_capability_copy(NULL, 1, 0, destination, 1) == EINVAL);
	assert(input_capability_copy(source, sizeof(source), 0, NULL, 1) ==
	    EINVAL);
	assert(input_capability_copy(source, sizeof(source), SIZE_MAX,
	    destination, 2) == EINVAL);
	for (size_t index = 0; index < sizeof(destination); index++)
		assert(destination[index] == 0xa5U);
	assert(input_capability_copy(NULL, 0, SIZE_MAX, NULL, 0) == 0);
}

static void
test_key_state(void)
{
	struct input_capability_state state;
	const uint8_t *bits = NULL;
	size_t size = 0;

	assert(input_capability_state_init(&state, valid_capabilities,
	    ARRAY_COUNT(valid_capabilities), valid_axes,
	    ARRAY_COUNT(valid_axes)) == 0);
	assert(input_capability_key_state(&state, &bits, &size) == 0);
	assert(size == INPUT_KEY_BITS_SIZE);
	for (size_t index = 0; index < size; index++)
		assert(bits[index] == 0);

	assert(input_capability_event(&state, EV_KEY, KEY_A, 1) == 1);
	assert(bit_is_set(bits, size, KEY_A));
	assert(input_capability_event(&state, EV_KEY, KEY_A, 2) == 1);
	assert(bit_is_set(bits, size, KEY_A));
	assert(input_capability_event(&state, EV_KEY, KEY_A, 99) == 0);
	assert(bit_is_set(bits, size, KEY_A));
	assert(input_capability_event(&state, EV_KEY, KEY_A, 0) == 1);
	assert(!bit_is_set(bits, size, KEY_A));

	assert(input_capability_event(&state, EV_KEY, KEY_MAX, 2) == 1);
	assert(bit_is_set(bits, size, KEY_MAX));
	assert(input_capability_event(&state, EV_KEY, KEY_MAX, 0) == 1);
	assert(!bit_is_set(bits, size, KEY_MAX));

	assert(input_capability_event(&state, EV_KEY, KEY_B, 1) == 0);
	assert(!bit_is_set(bits, size, KEY_B));
	assert(input_capability_event(&state, EV_KEY, KEY_MAX + 1U, 1) == 0);
	assert(input_capability_event(&state, EV_REL, REL_X, 1) == 1);
	assert(input_capability_event(&state, EV_SYN, SYN_REPORT, 0) == 1);
	assert(input_capability_event(&state, EV_SYN, SYN_DROPPED, 0) == 0);
	assert(input_capability_event(&state, EV_SYN, SYN_REPORT, 1) == 0);
	assert(input_capability_event(NULL, EV_KEY, KEY_A, 1) == 0);
	for (size_t index = 0; index < size; index++)
		assert(bits[index] == 0);

	assert(input_capability_key_state(NULL, &bits, &size) == EINVAL);
	assert(input_capability_key_state(&state, NULL, &size) == EINVAL);
	assert(input_capability_key_state(&state, &bits, NULL) == EINVAL);
}

static void
test_absolute_state(void)
{
	struct input_capability_state state;
	struct input_absinfo info;
	struct input_absinfo expected = valid_axes[0].info;

	assert(input_capability_state_init(&state, valid_capabilities,
	    ARRAY_COUNT(valid_capabilities), valid_axes,
	    ARRAY_COUNT(valid_axes)) == 0);
	assert(input_capability_abs_info(&state, ABS_X, &info) == 0);
	expect_abs_equal(&info, &expected);

	assert(input_capability_event(&state, EV_ABS, ABS_X, 511) == 1);
	expected.value = 511;
	assert(input_capability_abs_info(&state, ABS_X, &info) == 0);
	expect_abs_equal(&info, &expected);

	assert(input_capability_event(&state, EV_ABS, ABS_Y, 9) == 0);
	assert(input_capability_event(&state, EV_ABS, ABS_MAX + 1U, 9) == 0);
	assert(input_capability_event(&state, EV_KEY, ABS_X, 1) == 0);
	assert(input_capability_abs_info(&state, ABS_X, &info) == 0);
	expect_abs_equal(&info, &expected);

	assert(input_capability_abs_info(&state, ABS_MT_SLOT, &info) == 0);
	expect_abs_equal(&info, &valid_axes[1].info);
	assert(input_capability_abs_info(&state, ABS_MAX, &info) == 0);
	expect_abs_equal(&info, &valid_axes[2].info);
}

static void
test_registration_validation(void)
{
	struct input_capability_state state;
	const struct input_capability duplicate_capabilities[] = {
	    {EV_KEY, KEY_A}, {EV_KEY, KEY_A}};
	const struct input_capability duplicate_sync[] = {
	    {EV_SYN, SYN_REPORT}, {EV_SYN, SYN_CONFIG}};
	const struct input_capability invalid_sync[] = {{EV_SYN, SYN_CONFIG}};
	const struct input_capability invalid_type[] = {{EV_MSC, 0}};
	const struct input_capability invalid_key[] = {
	    {EV_KEY, KEY_MAX + 1U}};
	const struct input_capability invalid_rel[] = {
	    {EV_REL, REL_MAX + 1U}};
	const struct input_capability invalid_abs[] = {
	    {EV_ABS, ABS_MAX + 1U}};
	const struct input_abs_axis duplicate_axes[] = {
	    {ABS_X, {.minimum = 0, .maximum = 1}},
	    {ABS_X, {.minimum = 0, .maximum = 1}},
	};
	const struct input_abs_axis missing_capability[] = {
	    {ABS_Y, {.minimum = 0, .maximum = 1}}};
	const struct input_abs_axis invalid_axis[] = {
	    {ABS_MAX + 1U, {.minimum = 0, .maximum = 1}}};
	const struct input_abs_axis reversed_range[] = {
	    {ABS_X, {.value = 5, .minimum = 10, .maximum = 1}}};
	const struct input_abs_axis value_outside_range[] = {
	    {ABS_X, {.value = 2, .minimum = 0, .maximum = 1}}};
	const struct input_abs_axis negative_fuzz[] = {
	    {ABS_X, {.minimum = 0, .maximum = 1, .fuzz = -1}}};
	const struct input_abs_axis negative_flat[] = {
	    {ABS_X, {.minimum = 0, .maximum = 1, .flat = -1}}};
	const struct input_abs_axis negative_resolution[] = {
	    {ABS_X, {.minimum = 0, .maximum = 1, .resolution = -1}}};
	const struct input_capability only_abs_x[] = {{EV_ABS, ABS_X}};
	const struct input_capability only_key_a[] = {{EV_KEY, KEY_A}};
	const struct input_capability only_sync[] = {{EV_SYN, SYN_REPORT}};

	assert(input_capability_state_init(NULL, valid_capabilities,
	    ARRAY_COUNT(valid_capabilities), valid_axes,
	    ARRAY_COUNT(valid_axes)) == EINVAL);
	assert(input_capability_state_init(&state, NULL, 1, NULL, 0) ==
	    EINVAL);
	assert(input_capability_state_init(&state, NULL, 0, NULL, 1) ==
	    EINVAL);
	assert(input_capability_state_init(&state, NULL, 0, NULL, 0) == EINVAL);
	assert(input_capability_state_init(&state, only_key_a,
	    ARRAY_COUNT(only_key_a), NULL, 0) == EINVAL);
	assert(input_capability_state_init(&state, only_sync,
	    ARRAY_COUNT(only_sync), NULL, 0) == 0);
	assert(input_capability_state_init(&state, duplicate_capabilities,
	    ARRAY_COUNT(duplicate_capabilities), NULL, 0) == EINVAL);
	assert(input_capability_state_init(&state, duplicate_sync,
	    ARRAY_COUNT(duplicate_sync), NULL, 0) == EINVAL);
	assert(input_capability_state_init(&state, invalid_sync,
	    ARRAY_COUNT(invalid_sync), NULL, 0) == EINVAL);
	assert(input_capability_state_init(&state, invalid_type,
	    ARRAY_COUNT(invalid_type), NULL, 0) == EINVAL);
	assert(input_capability_state_init(&state, invalid_key,
	    ARRAY_COUNT(invalid_key), NULL, 0) == EINVAL);
	assert(input_capability_state_init(&state, invalid_rel,
	    ARRAY_COUNT(invalid_rel), NULL, 0) == EINVAL);
	assert(input_capability_state_init(&state, invalid_abs,
	    ARRAY_COUNT(invalid_abs), NULL, 0) == EINVAL);
	assert(input_capability_state_init(&state, only_abs_x,
	    ARRAY_COUNT(only_abs_x), duplicate_axes,
	    ARRAY_COUNT(duplicate_axes)) == EINVAL);
	assert(input_capability_state_init(&state, only_abs_x,
	    ARRAY_COUNT(only_abs_x), missing_capability,
	    ARRAY_COUNT(missing_capability)) == EINVAL);
	assert(input_capability_state_init(&state, only_abs_x,
	    ARRAY_COUNT(only_abs_x), invalid_axis,
	    ARRAY_COUNT(invalid_axis)) == EINVAL);
	assert(input_capability_state_init(&state, only_abs_x,
	    ARRAY_COUNT(only_abs_x), reversed_range,
	    ARRAY_COUNT(reversed_range)) == EINVAL);
	assert(input_capability_state_init(&state, only_abs_x,
	    ARRAY_COUNT(only_abs_x), value_outside_range,
	    ARRAY_COUNT(value_outside_range)) == EINVAL);
	assert(input_capability_state_init(&state, only_abs_x,
	    ARRAY_COUNT(only_abs_x), negative_fuzz,
	    ARRAY_COUNT(negative_fuzz)) == EINVAL);
	assert(input_capability_state_init(&state, only_abs_x,
	    ARRAY_COUNT(only_abs_x), negative_flat,
	    ARRAY_COUNT(negative_flat)) == EINVAL);
	assert(input_capability_state_init(&state, only_abs_x,
	    ARRAY_COUNT(only_abs_x), negative_resolution,
	    ARRAY_COUNT(negative_resolution)) == EINVAL);
	assert(input_capability_state_init(&state, only_abs_x,
	    INPUT_CAPABILITY_COUNT_MAX + 1U, NULL, 0) == EINVAL);
	assert(input_capability_state_init(&state, only_abs_x, 1,
	    reversed_range, ABS_MAX + 2U) == EINVAL);
	assert(input_capability_state_init(&state, only_abs_x, 1, NULL, 0) ==
	    EINVAL);
}

int
main(void)
{
	test_bitsets_and_queries();
	test_copy_windows();
	test_key_state();
	test_absolute_state();
	test_registration_validation();
	puts("WS006 input capability/state: PASS");
	return 0;
}
