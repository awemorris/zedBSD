/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The checks of the HWP register and feedback table decoder
 * (cpux86-hwp.c).  The expected values come from the figures and tables of
 * Intel SDM Vol. 3B rev. 093 (§17.4, §17.6) and from hand calculation; the
 * request and the feedback table inputs are built with regbuild.h and
 * feedbackbuild.h, which do not share the decoder's code.
 */

#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "cpux86-hwp.h"
#include "feedbackbuild.h"
#include "regbuild.h"

/* The state of the pseudo-random generator, fixed so every run is the same. */
static uint32_t rng_state = 0x2545F491U;

/*
 * Gives the next 32 random bits (xorshift32).
 */
static uint32_t
next32(void)
{
	/* Mixes the state with three shift-and-xor steps. */
	rng_state ^= rng_state << 13;
	rng_state ^= rng_state >> 17;
	rng_state ^= rng_state << 5;

	/* Reports the new state. */
	return rng_state;
}

/*
 * Gives 64 random bits from two draws.
 */
static uint64_t
next64(void)
{
	uint64_t high;

	/* The first draw is the high half. */
	high = next32();
	return (high << 32) | next32();
}

/*
 * Builds the register of one request field set from the figure's bit
 * positions, without the decoder.
 */
static uint64_t
build_request(
	uint32_t minimum,
	uint32_t maximum,
	uint32_t desired,
	uint32_t preference,
	uint32_t window,
	uint32_t package_control,
	uint32_t valid)
{
	uint64_t raw;

	/* Places each field at the bits of Figure 17-6. */
	raw = 0U;
	regbuild_put64(&raw, 0U, 8U, minimum);
	regbuild_put64(&raw, 8U, 8U, maximum);
	regbuild_put64(&raw, 16U, 8U, desired);
	regbuild_put64(&raw, 24U, 8U, preference);
	regbuild_put64(&raw, 32U, 10U, window);
	regbuild_put64(&raw, 42U, 1U, package_control);
	regbuild_put64(&raw, 59U, 5U, valid);

	/* Reports the register. */
	return raw;
}

/*
 * Fills a request structure.
 */
static void
fill_request(
	struct cpux86_hwp_request *request,
	uint32_t minimum,
	uint32_t maximum,
	uint32_t desired,
	uint32_t preference,
	uint32_t window,
	uint32_t package_control,
	uint32_t valid)
{
	/* Stores each field. */
	request->minimum = minimum;
	request->maximum = maximum;
	request->desired = desired;
	request->preference = preference;
	request->window = window;
	request->package_control = package_control;
	request->valid = valid;
}

/*
 * Checks IA32_PM_ENABLE.
 */
static void
test_pm_enable(void)
{
	uint32_t enabled;
	uint32_t bit;

	/* Bit 0 alone is the enable flag. */
	enabled = 7U;
	CHECK(cpux86_hwp_enabled(0U, &enabled) == 0);
	CHECK(enabled == 0U);
	CHECK(cpux86_hwp_enabled(1U, &enabled) == 0);
	CHECK(enabled == 1U);
	CHECK(cpux86_hwp_enabled(CPUX86_PM_ENABLE_HWP, &enabled) == 0);
	CHECK(enabled == 1U);

	/* Each reserved bit 1..63 is refused and leaves the output alone. */
	for (bit = 1U; bit < 64U; bit++) {
		enabled = 7U;
		CHECK(cpux86_hwp_enabled((uint64_t)1U << bit, &enabled) == EINVAL);
		CHECK(enabled == 7U);
		CHECK(cpux86_hwp_enabled(((uint64_t)1U << bit) | 1U, &enabled) == EINVAL);
		CHECK(enabled == 7U);
	}

	/* A missing output is refused. */
	CHECK(cpux86_hwp_enabled(1U, NULL) == EINVAL);
}

/*
 * Checks IA32_HWP_CAPABILITIES.
 */
static void
test_capabilities(void)
{
	struct cpux86_hwp_capabilities capabilities;
	uint32_t bit;

	/* The register of an Alder Lake P core: highest 0x38, guaranteed 0x15, efficient 0x14, lowest 1. */
	memset(&capabilities, 0xEE, sizeof(capabilities));
	CHECK(cpux86_hwp_capabilities_decode(0x01141538U, &capabilities) == 0);
	CHECK(capabilities.highest == 0x38U);
	CHECK(capabilities.guaranteed == 0x15U);
	CHECK(capabilities.efficient == 0x14U);
	CHECK(capabilities.lowest == 0x01U);

	/* Four different bytes land in four different fields, and no ordering is checked. */
	CHECK(cpux86_hwp_capabilities_decode(0x04030201U, &capabilities) == 0);
	CHECK(capabilities.highest == 0x01U);
	CHECK(capabilities.guaranteed == 0x02U);
	CHECK(capabilities.efficient == 0x03U);
	CHECK(capabilities.lowest == 0x04U);
	CHECK(cpux86_hwp_capabilities_decode(0xFFFFFFFFU, &capabilities) == 0);
	CHECK(capabilities.highest == 0xFFU);
	CHECK(capabilities.lowest == 0xFFU);
	CHECK(cpux86_hwp_capabilities_decode(0U, &capabilities) == 0);
	CHECK(capabilities.highest == 0U);
	CHECK(capabilities.guaranteed == 0U);
	CHECK(capabilities.efficient == 0U);
	CHECK(capabilities.lowest == 0U);

	/* Each reserved bit 32..63 is refused and leaves the output alone. */
	for (bit = 32U; bit < 64U; bit++) {
		memset(&capabilities, 0xEE, sizeof(capabilities));
		CHECK(cpux86_hwp_capabilities_decode(((uint64_t)1U << bit) | 0x01141538U, &capabilities) == EINVAL);
		CHECK(capabilities.highest == 0xEEEEEEEEU);
		CHECK(capabilities.guaranteed == 0xEEEEEEEEU);
		CHECK(capabilities.efficient == 0xEEEEEEEEU);
		CHECK(capabilities.lowest == 0xEEEEEEEEU);
	}

	/* A missing output is refused. */
	CHECK(cpux86_hwp_capabilities_decode(0U, NULL) == EINVAL);
}

/*
 * Checks the width checks of the request encoder and the bit positions.
 */
static void
test_request_encode_widths(void)
{
	struct cpux86_hwp_request request;
	uint64_t raw;
	uint64_t expected;
	uint32_t bit;

	/* The top of every width is accepted and lands at its bits. */
	fill_request(&request, 255U, 255U, 255U, 255U, 1023U, 1U, 31U);
	raw = 0x1234U;
	CHECK(cpux86_hwp_request_encode(&request, NULL, &raw) == 0);
	expected = 0U;
	regbuild_put64(&expected, 0U, 8U, 255U);
	regbuild_put64(&expected, 8U, 8U, 255U);
	regbuild_put64(&expected, 16U, 8U, 255U);
	regbuild_put64(&expected, 24U, 8U, 255U);
	regbuild_put64(&expected, 32U, 10U, 1023U);
	regbuild_put64(&expected, 42U, 1U, 1U);
	regbuild_put64(&expected, 59U, 5U, 31U);
	CHECK(raw == expected);

	/* The reserved bits 58:43 stay zero whatever the fields are. */
	for (bit = 43U; bit < 59U; bit++)
		CHECK((raw & ((uint64_t)1U << bit)) == 0U);

	/* Each field alone lands at its own bits. */
	fill_request(&request, 0xA5U, 0U, 0U, 0U, 0U, 0U, 0U);
	CHECK(cpux86_hwp_request_encode(&request, NULL, &raw) == 0);
	CHECK(raw == 0xA5U);
	fill_request(&request, 0U, 0xA5U, 0U, 0U, 0U, 0U, 0U);
	CHECK(cpux86_hwp_request_encode(&request, NULL, &raw) == 0);
	CHECK(raw == 0xA500U);
	fill_request(&request, 0U, 0U, 0xA5U, 0U, 0U, 0U, 0U);
	CHECK(cpux86_hwp_request_encode(&request, NULL, &raw) == 0);
	CHECK(raw == 0xA50000U);
	fill_request(&request, 0U, 0U, 0U, 0xA5U, 0U, 0U, 0U);
	CHECK(cpux86_hwp_request_encode(&request, NULL, &raw) == 0);
	CHECK(raw == 0xA5000000U);
	fill_request(&request, 0U, 0U, 0U, 0U, 0x2A5U, 0U, 0U);
	CHECK(cpux86_hwp_request_encode(&request, NULL, &raw) == 0);
	CHECK(raw == ((uint64_t)0x2A5U << 32));
	fill_request(&request, 0U, 0U, 0U, 0U, 0U, 1U, 0U);
	CHECK(cpux86_hwp_request_encode(&request, NULL, &raw) == 0);
	CHECK(raw == ((uint64_t)1U << 42));

	/* The valid bits 0..4 are bits 59..63. */
	for (bit = 0U; bit < 5U; bit++) {
		fill_request(&request, 0U, 0U, 0U, 0U, 0U, 0U, (uint32_t)1U << bit);
		CHECK(cpux86_hwp_request_encode(&request, NULL, &raw) == 0);
		CHECK(raw == ((uint64_t)1U << (59U + bit)));
	}
	CHECK(CPUX86_HWP_VALID_WINDOW == 0x01U);
	CHECK(CPUX86_HWP_VALID_EPP == 0x02U);
	CHECK(CPUX86_HWP_VALID_DESIRED == 0x04U);
	CHECK(CPUX86_HWP_VALID_MAXIMUM == 0x08U);
	CHECK(CPUX86_HWP_VALID_MINIMUM == 0x10U);

	/* One above the top of each width is refused and leaves the output alone. */
	fill_request(&request, 256U, 0U, 0U, 0U, 0U, 0U, 0U);
	raw = 0x77U;
	CHECK(cpux86_hwp_request_encode(&request, NULL, &raw) == EINVAL);
	CHECK(raw == 0x77U);
	fill_request(&request, 0U, 256U, 0U, 0U, 0U, 0U, 0U);
	CHECK(cpux86_hwp_request_encode(&request, NULL, &raw) == EINVAL);
	CHECK(raw == 0x77U);
	fill_request(&request, 0U, 0U, 256U, 0U, 0U, 0U, 0U);
	CHECK(cpux86_hwp_request_encode(&request, NULL, &raw) == EINVAL);
	CHECK(raw == 0x77U);
	fill_request(&request, 0U, 0U, 0U, 256U, 0U, 0U, 0U);
	CHECK(cpux86_hwp_request_encode(&request, NULL, &raw) == EINVAL);
	CHECK(raw == 0x77U);
	fill_request(&request, 0U, 0U, 0U, 0U, 1024U, 0U, 0U);
	CHECK(cpux86_hwp_request_encode(&request, NULL, &raw) == EINVAL);
	CHECK(raw == 0x77U);
	fill_request(&request, 0U, 0U, 0U, 0U, 0U, 2U, 0U);
	CHECK(cpux86_hwp_request_encode(&request, NULL, &raw) == EINVAL);
	CHECK(raw == 0x77U);
	fill_request(&request, 0U, 0U, 0U, 0U, 0U, 0U, 32U);
	CHECK(cpux86_hwp_request_encode(&request, NULL, &raw) == EINVAL);
	CHECK(raw == 0x77U);

	/* A field far above its width does not wrap into the next field. */
	fill_request(&request, 0x100U, 0U, 0U, 0U, 0U, 0U, 0U);
	CHECK(cpux86_hwp_request_encode(&request, NULL, &raw) == EINVAL);
	fill_request(&request, 0xFFFFFFFFU, 0U, 0U, 0U, 0U, 0U, 0U);
	CHECK(cpux86_hwp_request_encode(&request, NULL, &raw) == EINVAL);
	fill_request(&request, 0U, 0U, 0U, 0U, 0xFFFFFFFFU, 0U, 0U);
	CHECK(cpux86_hwp_request_encode(&request, NULL, &raw) == EINVAL);
	fill_request(&request, 0U, 0U, 0U, 0U, 0U, 0xFFFFFFFFU, 0U);
	CHECK(cpux86_hwp_request_encode(&request, NULL, &raw) == EINVAL);
	fill_request(&request, 0U, 0U, 0U, 0U, 0U, 0U, 0xFFFFFFFFU);
	CHECK(cpux86_hwp_request_encode(&request, NULL, &raw) == EINVAL);
	CHECK(raw == 0x77U);

	/* A missing request or output is refused. */
	CHECK(cpux86_hwp_request_encode(NULL, NULL, &raw) == EINVAL);
	CHECK(cpux86_hwp_request_encode(&request, NULL, NULL) == EINVAL);
	CHECK(raw == 0x77U);
}

/*
 * Checks the order checks of the request encoder against the capabilities.
 */
static void
test_request_encode_order(void)
{
	struct cpux86_hwp_capabilities capabilities;
	struct cpux86_hwp_request request;
	uint64_t raw;

	/* The Alder Lake P core's levels: lowest 1, highest 0x38. */
	capabilities.highest = 0x38U;
	capabilities.guaranteed = 0x15U;
	capabilities.efficient = 0x14U;
	capabilities.lowest = 0x01U;

	/* The whole range, with the hardware choosing the desired level, is accepted. */
	fill_request(&request, 1U, 0x38U, 0U, 0x80U, 0U, 0U, 0U);
	raw = 0U;
	CHECK(cpux86_hwp_request_encode(&request, &capabilities, &raw) == 0);
	CHECK(raw == build_request(1U, 0x38U, 0U, 0x80U, 0U, 0U, 0U));

	/* A minimum below the lowest level is refused. */
	fill_request(&request, 0U, 0x38U, 0U, 0U, 0U, 0U, 0U);
	raw = 0x55U;
	CHECK(cpux86_hwp_request_encode(&request, &capabilities, &raw) == EINVAL);
	CHECK(raw == 0x55U);

	/* A maximum above the highest level is refused. */
	fill_request(&request, 1U, 0x39U, 0U, 0U, 0U, 0U, 0U);
	CHECK(cpux86_hwp_request_encode(&request, &capabilities, &raw) == EINVAL);
	CHECK(raw == 0x55U);

	/* A minimum above the maximum is refused; equal is accepted. */
	fill_request(&request, 0x20U, 0x10U, 0U, 0U, 0U, 0U, 0U);
	CHECK(cpux86_hwp_request_encode(&request, &capabilities, &raw) == EINVAL);
	CHECK(raw == 0x55U);
	fill_request(&request, 0x10U, 0x10U, 0U, 0U, 0U, 0U, 0U);
	CHECK(cpux86_hwp_request_encode(&request, &capabilities, &raw) == 0);

	/* The desired level may be anywhere from the lowest to the highest level, and 0. */
	capabilities.lowest = 4U;
	fill_request(&request, 4U, 0x20U, 3U, 0U, 0U, 0U, 0U);
	raw = 0x55U;
	CHECK(cpux86_hwp_request_encode(&request, &capabilities, &raw) == EINVAL);
	CHECK(raw == 0x55U);
	fill_request(&request, 4U, 0x20U, 4U, 0U, 0U, 0U, 0U);
	CHECK(cpux86_hwp_request_encode(&request, &capabilities, &raw) == 0);
	fill_request(&request, 4U, 0x20U, 0x38U, 0U, 0U, 0U, 0U);
	CHECK(cpux86_hwp_request_encode(&request, &capabilities, &raw) == 0);
	fill_request(&request, 4U, 0x20U, 0x39U, 0U, 0U, 0U, 0U);
	raw = 0x55U;
	CHECK(cpux86_hwp_request_encode(&request, &capabilities, &raw) == EINVAL);
	CHECK(raw == 0x55U);
	fill_request(&request, 4U, 0x20U, 0U, 0U, 0U, 0U, 0U);
	CHECK(cpux86_hwp_request_encode(&request, &capabilities, &raw) == 0);

	/* The desired level is not limited by the minimum and maximum, only by lowest..highest. */
	fill_request(&request, 0x10U, 0x20U, 0x30U, 0U, 0U, 0U, 0U);
	CHECK(cpux86_hwp_request_encode(&request, &capabilities, &raw) == 0);
	fill_request(&request, 0x10U, 0x20U, 5U, 0U, 0U, 0U, 0U);
	CHECK(cpux86_hwp_request_encode(&request, &capabilities, &raw) == 0);

	/* Without capabilities only the widths are checked: lowest above highest means nothing is checked. */
	fill_request(&request, 0U, 0xFFU, 7U, 0U, 0U, 0U, 0U);
	CHECK(cpux86_hwp_request_encode(&request, NULL, &raw) == 0);
	fill_request(&request, 0xFFU, 0U, 0xFFU, 0U, 0U, 0U, 0U);
	CHECK(cpux86_hwp_request_encode(&request, NULL, &raw) == 0);

	/* The full range with lowest 0 and highest 255 accepts every level. */
	capabilities.lowest = 0U;
	capabilities.highest = 255U;
	fill_request(&request, 0U, 255U, 255U, 255U, 1023U, 1U, 31U);
	CHECK(cpux86_hwp_request_encode(&request, &capabilities, &raw) == 0);
}

/*
 * Checks the request decoder and the round trip.
 */
static void
test_request_decode(void)
{
	struct cpux86_hwp_request request;
	struct cpux86_hwp_request again;
	uint64_t raw;
	uint64_t encoded;
	uint32_t bit;
	uint32_t round;

	/* A register built from the figure decodes to the fields it was built from. */
	raw = build_request(0x11U, 0x22U, 0x33U, 0x44U, 0x155U, 1U, 0x15U);
	memset(&request, 0xEE, sizeof(request));
	CHECK(cpux86_hwp_request_decode(raw, &request) == 0);
	CHECK(request.minimum == 0x11U);
	CHECK(request.maximum == 0x22U);
	CHECK(request.desired == 0x33U);
	CHECK(request.preference == 0x44U);
	CHECK(request.window == 0x155U);
	CHECK(request.package_control == 1U);
	CHECK(request.valid == 0x15U);

	/* Every reserved bit 43..58 is refused and leaves the output alone. */
	for (bit = 43U; bit < 59U; bit++) {
		memset(&request, 0xEE, sizeof(request));
		CHECK(cpux86_hwp_request_decode(raw | ((uint64_t)1U << bit), &request) == EINVAL);
		CHECK(request.minimum == 0xEEEEEEEEU);
		CHECK(request.valid == 0xEEEEEEEEU);
	}

	/* The register of an all-ones request except the reserved bits decodes. */
	CHECK(cpux86_hwp_request_decode(build_request(255U, 255U, 255U, 255U, 1023U, 1U, 31U), &request) == 0);
	CHECK(request.window == 1023U);
	CHECK(request.valid == 31U);

	/* A missing output is refused. */
	CHECK(cpux86_hwp_request_decode(0U, NULL) == EINVAL);

	/* Random fields survive encode then decode, and the decoded request encodes to the same register. */
	for (round = 0U; round < 10000U; round++) {
		fill_request(&request,
		    next32() & 0xFFU,
		    next32() & 0xFFU,
		    next32() & 0xFFU,
		    next32() & 0xFFU,
		    next32() & 0x3FFU,
		    next32() & 1U,
		    next32() & 0x1FU);
		encoded = 0U;
		CHECK(cpux86_hwp_request_encode(&request, NULL, &encoded) == 0);
		CHECK(encoded == build_request(request.minimum, request.maximum, request.desired, request.preference,
		    request.window, request.package_control, request.valid));
		memset(&again, 0xEE, sizeof(again));
		CHECK(cpux86_hwp_request_decode(encoded, &again) == 0);
		CHECK(again.minimum == request.minimum);
		CHECK(again.maximum == request.maximum);
		CHECK(again.desired == request.desired);
		CHECK(again.preference == request.preference);
		CHECK(again.window == request.window);
		CHECK(again.package_control == request.package_control);
		CHECK(again.valid == request.valid);
	}
}

/*
 * Checks the activity window encoder and decoder.
 */
static void
test_window(void)
{
	static const uint32_t powers[8] = {
		1U, 10U, 100U, 1000U, 10000U, 100000U, 1000000U, 10000000U
	};
	struct window_entry {
		uint32_t value;
		uint32_t field;
	};
	struct window_entry *entries;
	uint32_t *best_field;
	uint32_t field;
	uint32_t decoded;
	uint32_t expected;
	uint32_t count;
	uint32_t index;
	uint32_t inner;
	uint32_t input;
	uint32_t next;
	struct window_entry swap;
	uint32_t round;

	/* The examples of the header. */
	field = 0xEEU;
	CHECK(cpux86_hwp_window_encode(0U, &field) == 0);
	CHECK(field == 0U);
	CHECK(cpux86_hwp_window_encode(1U, &field) == 0);
	CHECK(field == 1U);
	CHECK(cpux86_hwp_window_encode(127U, &field) == 0);
	CHECK(field == 127U);
	CHECK(cpux86_hwp_window_encode(128U, &field) == 0);
	CHECK(field == 127U);
	CHECK(cpux86_hwp_window_encode(1270U, &field) == 0);
	CHECK(field == ((1U << 7) | 127U));
	CHECK(cpux86_hwp_window_encode(1275U, &field) == 0);
	CHECK(field == ((1U << 7) | 127U));
	CHECK(cpux86_hwp_window_encode(100U, &field) == 0);
	CHECK(field == 100U);
	CHECK(cpux86_hwp_window_encode(1000U, &field) == 0);
	CHECK(field == ((1U << 7) | 100U));
	CHECK(cpux86_hwp_window_encode(CPUX86_HWP_WINDOW_MAX_US, &field) == 0);
	CHECK(field == ((7U << 7) | 127U));
	CHECK(CPUX86_HWP_WINDOW_MAX_US == 127U * 10000000U);

	/* Above the maximum is out of range and leaves the output alone. */
	field = 0xEEU;
	CHECK(cpux86_hwp_window_encode(CPUX86_HWP_WINDOW_MAX_US + 1U, &field) == ERANGE);
	CHECK(field == 0xEEU);
	CHECK(cpux86_hwp_window_encode(0xFFFFFFFFU, &field) == ERANGE);
	CHECK(field == 0xEEU);
	CHECK(cpux86_hwp_window_encode(1U, NULL) == EINVAL);

	/* Every one of the 1024 fields decodes to mantissa times ten to the exponent. */
	for (field = 0U; field < 1024U; field++) {
		decoded = 0xEEEEEEEEU;
		CHECK(cpux86_hwp_window_decode(field, &decoded) == 0);
		CHECK(decoded == (field % 128U) * powers[field / 128U]);
	}

	/* A field above 10 bits is refused and leaves the output alone. */
	decoded = 0xEEU;
	CHECK(cpux86_hwp_window_decode(1024U, &decoded) == EINVAL);
	CHECK(cpux86_hwp_window_decode(0xFFFFFFFFU, &decoded) == EINVAL);
	CHECK(decoded == 0xEEU);
	CHECK(cpux86_hwp_window_decode(0U, NULL) == EINVAL);

	/* Lists every representable window with its smallest field, in ascending order of window. */
	entries = (struct window_entry *)malloc(1024U * sizeof(*entries));
	best_field = (uint32_t *)malloc(1024U * sizeof(*best_field));
	CHECK(entries != NULL);
	CHECK(best_field != NULL);
	if (entries == NULL || best_field == NULL) {
		free(entries);
		free(best_field);
		return;
	}
	for (field = 0U; field < 1024U; field++) {
		entries[field].value = (field % 128U) * powers[field / 128U];
		entries[field].field = field;
	}
	for (index = 1U; index < 1024U; index++) {
		for (inner = index; inner > 0U; inner--) {
			if (entries[inner - 1U].value < entries[inner].value)
				break;
			if (entries[inner - 1U].value == entries[inner].value &&
			    entries[inner - 1U].field < entries[inner].field)
				break;
			swap = entries[inner - 1U];
			entries[inner - 1U] = entries[inner];
			entries[inner] = swap;
		}
	}
	count = 0U;
	for (index = 0U; index < 1024U; index++) {
		if (count != 0U && entries[count - 1U].value == entries[index].value)
			continue;
		entries[count] = entries[index];
		best_field[count] = entries[index].field;
		count++;
	}

	/* Every input 0..2000000 gets the largest representable window not above it, with the smallest field. */
	index = 0U;
	for (input = 0U; input <= 2000000U; input++) {
		while (index + 1U < count && entries[index + 1U].value <= input)
			index++;
		field = 0xEEEEU;
		CHECK(cpux86_hwp_window_encode(input, &field) == 0);
		if (input == 0U) {
			CHECK(field == 0U);
		} else {
			CHECK(field == best_field[index]);
		}
		CHECK(cpux86_hwp_window_decode(field, &decoded) == 0);
		CHECK(decoded == entries[index].value);
		CHECK(decoded <= input);
		if (index + 1U < count)
			CHECK(entries[index + 1U].value > input);
	}

	/* Random inputs up to the maximum behave the same. */
	for (round = 0U; round < 20000U; round++) {
		input = next32() % (CPUX86_HWP_WINDOW_MAX_US + 1U);
		index = 0U;
		next = count;
		while (index + 1U < next) {
			inner = (index + next) / 2U;
			if (entries[inner].value <= input)
				index = inner;
			else
				next = inner;
		}
		expected = entries[index].value;
		CHECK(cpux86_hwp_window_encode(input, &field) == 0);
		CHECK(cpux86_hwp_window_decode(field, &decoded) == 0);
		CHECK(decoded == expected);
		CHECK(decoded <= input);
		if (index + 1U < count)
			CHECK(entries[index + 1U].value > input);
		if (input != 0U)
			CHECK(field == best_field[index]);
	}

	/* The edge of every representable window and the values next to it. */
	for (index = 1U; index < count; index++) {
		input = entries[index].value;
		CHECK(cpux86_hwp_window_encode(input, &field) == 0);
		CHECK(field == best_field[index]);
		CHECK(cpux86_hwp_window_encode(input - 1U, &field) == 0);
		CHECK(cpux86_hwp_window_decode(field, &decoded) == 0);
		CHECK(decoded == entries[index - 1U].value);
		if (input < CPUX86_HWP_WINDOW_MAX_US) {
			CHECK(cpux86_hwp_window_encode(input + 1U, &field) == 0);
			CHECK(cpux86_hwp_window_decode(field, &decoded) == 0);
			if (index + 1U < count && entries[index + 1U].value == input + 1U) {
				CHECK(decoded == input + 1U);
			} else {
				CHECK(decoded == input);
			}
		}
	}

	free(entries);
	free(best_field);
}

/*
 * Checks the request source (Table 17-2).
 */
static void
test_source(void)
{
	uint32_t source;

	/* The four rows of the table. */
	source = 0xEEU;
	CHECK(cpux86_hwp_source(0U, 0U, &source) == 0);
	CHECK(source == CPUX86_HWP_SOURCE_THREAD);
	CHECK(cpux86_hwp_source(0U, 1U, &source) == 0);
	CHECK(source == CPUX86_HWP_SOURCE_PACKAGE);
	CHECK(cpux86_hwp_source(1U, 0U, &source) == 0);
	CHECK(source == CPUX86_HWP_SOURCE_PACKAGE);
	CHECK(cpux86_hwp_source(1U, 1U, &source) == 0);
	CHECK(source == CPUX86_HWP_SOURCE_THREAD);

	/* A bit above 1 or a missing output is refused, leaving the output alone. */
	source = 0xEEU;
	CHECK(cpux86_hwp_source(2U, 0U, &source) == EINVAL);
	CHECK(cpux86_hwp_source(0U, 2U, &source) == EINVAL);
	CHECK(cpux86_hwp_source(0xFFFFFFFFU, 1U, &source) == EINVAL);
	CHECK(source == 0xEEU);
	CHECK(cpux86_hwp_source(0U, 0U, NULL) == EINVAL);
}

/*
 * Checks IA32_HWP_STATUS and its acknowledgement.
 */
static void
test_status(void)
{
	uint32_t events;
	uint64_t raw;
	uint32_t bit;
	uint32_t all;

	/* The five defined bits, one at a time, as in Figure 17-9. */
	events = 0xEEU;
	CHECK(cpux86_hwp_status_decode(0U, &events) == 0);
	CHECK(events == 0U);
	CHECK(cpux86_hwp_status_decode(1U, &events) == 0);
	CHECK(events == CPUX86_HWP_STATUS_GUARANTEED);
	CHECK(cpux86_hwp_status_decode(4U, &events) == 0);
	CHECK(events == CPUX86_HWP_STATUS_EXCURSION);
	CHECK(cpux86_hwp_status_decode(8U, &events) == 0);
	CHECK(events == CPUX86_HWP_STATUS_HIGHEST);
	CHECK(cpux86_hwp_status_decode(0x10U, &events) == 0);
	CHECK(events == CPUX86_HWP_STATUS_PECI_ENTRY);
	CHECK(cpux86_hwp_status_decode(0x20U, &events) == 0);
	CHECK(events == CPUX86_HWP_STATUS_PECI_EXIT);
	CHECK(cpux86_hwp_status_decode(0x3DU, &events) == 0);
	CHECK(events == 0x3DU);

	/* Highest_Change is bit 3 and nothing else is. */
	CHECK(CPUX86_HWP_STATUS_HIGHEST == ((uint32_t)1U << 3));
	CHECK(CPUX86_HWP_STATUS_DEFINED == 0x3DU);

	/* Bit 1 and bits 6..63 are refused, leaving the output alone. */
	events = 0xEEU;
	CHECK(cpux86_hwp_status_decode(2U, &events) == EINVAL);
	CHECK(cpux86_hwp_status_decode(0x3FU, &events) == EINVAL);
	for (bit = 6U; bit < 64U; bit++)
		CHECK(cpux86_hwp_status_decode((uint64_t)1U << bit, &events) == EINVAL);
	CHECK(events == 0xEEU);
	CHECK(cpux86_hwp_status_decode(0U, NULL) == EINVAL);

	/* Acknowledging nothing keeps every defined bit at 1, which leaves the events alone. */
	raw = 0xEEU;
	CHECK(cpux86_hwp_status_ack(0U, &raw) == 0);
	CHECK(raw == 0x3DU);

	/* Acknowledging everything writes 0. */
	CHECK(cpux86_hwp_status_ack(0x3DU, &raw) == 0);
	CHECK(raw == 0U);

	/* Acknowledging the guaranteed change leaves the other four bits at 1. */
	CHECK(cpux86_hwp_status_ack(CPUX86_HWP_STATUS_GUARANTEED, &raw) == 0);
	CHECK(raw == 0x3CU);
	CHECK(cpux86_hwp_status_ack(CPUX86_HWP_STATUS_HIGHEST, &raw) == 0);
	CHECK(raw == 0x35U);

	/* Each defined bit, one at a time, is the defined set without that bit. */
	for (bit = 0U; bit < 6U; bit++) {
		all = (uint32_t)1U << bit;
		if ((all & 0x3DU) == 0U)
			continue;
		CHECK(cpux86_hwp_status_ack(all, &raw) == 0);
		CHECK(raw == (uint64_t)(0x3DU & ~all));
	}

	/* A bit outside the defined set is refused, leaving the output alone. */
	raw = 0xEEU;
	CHECK(cpux86_hwp_status_ack(2U, &raw) == EINVAL);
	CHECK(cpux86_hwp_status_ack(0x40U, &raw) == EINVAL);
	CHECK(cpux86_hwp_status_ack(0x80000000U, &raw) == EINVAL);
	CHECK(cpux86_hwp_status_ack(0xFFFFFFFFU, &raw) == EINVAL);
	CHECK(raw == 0xEEU);
	CHECK(cpux86_hwp_status_ack(0U, NULL) == EINVAL);
}

/*
 * Builds the leaf 06H decoded values of a processor with the feedback interface.
 */
static void
fill_power(
	struct cpux86_power *power,
	uint32_t thread_director,
	uint32_t classes,
	uint32_t capabilities,
	uint32_t pages)
{
	/* Starts from a processor with the interface and nothing else. */
	memset(power, 0, sizeof(*power));
	power->features = CPUX86_POWER_HW_FEEDBACK;
	if (thread_director != 0U)
		power->features |= CPUX86_POWER_THREAD_DIRECTOR;
	power->feedback_classes = classes;
	power->feedback_capabilities = capabilities;
	power->feedback_pages = pages;
}

/*
 * Checks one layout against numbers worked out by hand.
 */
static void
check_layout(
	uint32_t thread_director,
	uint32_t classes,
	uint32_t capabilities,
	uint32_t pages,
	uint32_t expect_capabilities,
	uint32_t expect_classes,
	uint32_t expect_header,
	uint32_t expect_row,
	uint32_t expect_rows)
{
	struct cpux86_power power;
	struct cpux86_feedback_layout layout;

	/* Decodes the layout of the processor. */
	fill_power(&power, thread_director, classes, capabilities, pages);
	memset(&layout, 0xEE, sizeof(layout));
	CHECK(cpux86_feedback_layout(&power, &layout) == 0);
	CHECK(layout.capabilities == expect_capabilities);
	CHECK(layout.classes == expect_classes);
	CHECK(layout.header_size == expect_header);
	CHECK(layout.row_size == expect_row);
	CHECK(layout.table_size == pages * 4096U);
	CHECK(layout.row_count == expect_rows);
}

/*
 * Checks the layout of the feedback table.
 */
static void
test_layout(void)
{
	struct cpux86_power power;
	struct cpux86_feedback_layout layout;
	uint32_t bit;

	/* Without Thread Director: 16-byte header, 8-byte rows, (4096 - 16) / 8 rows. */
	check_layout(0U, 0U, 0x03U, 1U, 2U, 1U, 16U, 8U, 510U);
	check_layout(0U, 4U, 0x03U, 1U, 2U, 1U, 16U, 8U, 510U);

	/* The Alder Lake P core's table: EAX has Thread Director, ECX[15:8] is 4. */
	check_layout(1U, 4U, 0x03U, 1U, 2U, 4U, 16U, 8U, 510U);

	/* Five classes: 8 + 10 = 18 rounds to 24, a 10-byte row rounds to 16. */
	check_layout(1U, 5U, 0x03U, 1U, 2U, 5U, 24U, 16U, 254U);

	/* Three capabilities (EDX[7:0] = 7, bit 2 counted) and four classes: header 20 -> 24, row 12 -> 16. */
	check_layout(1U, 4U, 0x07U, 1U, 3U, 4U, 24U, 16U, 254U);

	/* 255 classes in a 16-page table: header 8 + 510 = 518 -> 520, row 510 -> 512, (65536 - 520) / 512 = 126. */
	check_layout(1U, 255U, 0x03U, 16U, 2U, 255U, 520U, 512U, 126U);

	/* Eight capabilities in 255 classes still leave one row: header 2048, row 2040. */
	check_layout(1U, 255U, 0xFFU, 1U, 8U, 255U, 2048U, 2040U, 1U);

	/* The EDX[7:0] bits are counted wherever they are, bits 0 and 1 among them. */
	check_layout(0U, 0U, 0x83U, 1U, 3U, 1U, 16U, 8U, 510U);
	check_layout(0U, 0U, 0x0FU, 1U, 4U, 1U, 16U, 8U, 510U);

	/* Two classes in two pages: header 8 + 4 = 12 -> 16, row 4 -> 8, (8192 - 16) / 8 = 1022 rows. */
	check_layout(1U, 2U, 0x03U, 2U, 2U, 2U, 16U, 8U, 1022U);

	/* A missing input or output is refused. */
	fill_power(&power, 0U, 1U, 0x03U, 1U);
	CHECK(cpux86_feedback_layout(NULL, &layout) == EINVAL);
	CHECK(cpux86_feedback_layout(&power, NULL) == EINVAL);

	/* A processor without the interface is not supported, whatever else is wrong. */
	fill_power(&power, 1U, 0U, 0x03U, 1U);
	power.features &= ~CPUX86_POWER_HW_FEEDBACK;
	memset(&layout, 0xEE, sizeof(layout));
	CHECK(cpux86_feedback_layout(&power, &layout) == ENOTSUP);
	CHECK(layout.header_size == 0xEEEEEEEEU);
	power.feedback_capabilities = 0U;
	power.feedback_pages = 0U;
	CHECK(cpux86_feedback_layout(&power, &layout) == ENOTSUP);
	power.feedback_capabilities = 0x01U;
	CHECK(cpux86_feedback_layout(&power, &layout) == ENOTSUP);
	power.features = 0U;
	CHECK(cpux86_feedback_layout(&power, &layout) == ENOTSUP);
	power.features = CPUX86_POWER_DEFINED & ~CPUX86_POWER_HW_FEEDBACK;
	CHECK(cpux86_feedback_layout(&power, &layout) == ENOTSUP);
	CHECK(layout.header_size == 0xEEEEEEEEU);

	/* An interface with no capability is not supported, and nothing else is looked at. */
	fill_power(&power, 1U, 0U, 0x00U, 0U);
	CHECK(cpux86_feedback_layout(&power, &layout) == ENOTSUP);
	CHECK(layout.header_size == 0xEEEEEEEEU);

	/* Bit 0 alone, bit 1 alone and bit 2 alone are refused. */
	fill_power(&power, 0U, 1U, 0x01U, 1U);
	CHECK(cpux86_feedback_layout(&power, &layout) == EINVAL);
	fill_power(&power, 0U, 1U, 0x02U, 1U);
	CHECK(cpux86_feedback_layout(&power, &layout) == EINVAL);
	fill_power(&power, 0U, 1U, 0x04U, 1U);
	CHECK(cpux86_feedback_layout(&power, &layout) == EINVAL);
	for (bit = 2U; bit < 8U; bit++) {
		fill_power(&power, 0U, 1U, (uint32_t)1U << bit, 1U);
		CHECK(cpux86_feedback_layout(&power, &layout) == EINVAL);
	}
	CHECK(layout.header_size == 0xEEEEEEEEU);

	/* Thread Director with no class is refused; without Thread Director the class count is ignored. */
	fill_power(&power, 1U, 0U, 0x03U, 1U);
	CHECK(cpux86_feedback_layout(&power, &layout) == EINVAL);
	CHECK(layout.header_size == 0xEEEEEEEEU);
	fill_power(&power, 0U, 0U, 0x03U, 1U);
	CHECK(cpux86_feedback_layout(&power, &layout) == 0);
	CHECK(layout.classes == 1U);

	/* A table with no page, or too small for the header and one row, is refused. */
	fill_power(&power, 1U, 4U, 0x03U, 0U);
	memset(&layout, 0xEE, sizeof(layout));
	CHECK(cpux86_feedback_layout(&power, &layout) == EINVAL);
	CHECK(layout.header_size == 0xEEEEEEEEU);
	fill_power(&power, 1U, 256U, 0xFFU, 1U);
	CHECK(cpux86_feedback_layout(&power, &layout) == EINVAL);
	fill_power(&power, 1U, 300U, 0xFFU, 1U);
	CHECK(cpux86_feedback_layout(&power, &layout) == EINVAL);
	fill_power(&power, 1U, 0xFFFFFFFFU, 0xFFU, 16U);
	CHECK(cpux86_feedback_layout(&power, &layout) == EINVAL);
	fill_power(&power, 1U, 0xFFFFFFFFU, 0xFFU, 0xFFFFFFFFU);
	CHECK(cpux86_feedback_layout(&power, &layout) == EINVAL);
	fill_power(&power, 0U, 1U, 0x03U, 0xFFFFFFFFU);
	CHECK(cpux86_feedback_layout(&power, &layout) == EINVAL);
	CHECK(layout.header_size == 0xEEEEEEEEU);

	/* A page count above the 16 the processor reports still gives a table that fits 32 bits. */
	fill_power(&power, 0U, 1U, 0x03U, 100U);
	CHECK(cpux86_feedback_layout(&power, &layout) == 0);
	CHECK(layout.table_size == 100U * 4096U);
}

/*
 * Reports the value a test cell holds, distinct for every row, class and capability.
 */
static uint8_t
cell_value(
	uint32_t row,
	uint32_t class_index,
	uint32_t capability)
{
	/* Mixes the three indexes so no two neighbouring cells agree. */
	return (uint8_t)((row * 37U + class_index * 11U + capability * 5U + 1U) & 0xFFU);
}

/*
 * Reports the change byte of a test table.
 */
static uint8_t
change_value(
	uint32_t class_index,
	uint32_t capability)
{
	/* Mixes the two indexes. */
	return (uint8_t)((class_index * 3U + capability + 1U) & 0xFFU);
}

/*
 * Writes a whole table with feedbackbuild.h and reads every cell and change byte back.
 */
static void
check_table(
	uint32_t thread_director,
	uint32_t classes,
	uint32_t capability_bits,
	uint32_t pages)
{
	struct cpux86_power power;
	struct cpux86_feedback_layout layout;
	uint8_t *table;
	size_t table_size;
	uint64_t timestamp;
	uint32_t flags;
	uint32_t value;
	uint32_t row;
	uint32_t class_index;
	uint32_t capability;

	/* Works out the layout the processor would have. */
	fill_power(&power, thread_director, classes, capability_bits, pages);
	CHECK(cpux86_feedback_layout(&power, &layout) == 0);
	table_size = (size_t)layout.table_size;
	table = (uint8_t *)malloc(table_size);
	CHECK(table != NULL);
	if (table == NULL)
		return;

	/* Fills the memory with a pattern, so padding and untouched bytes stand out. */
	memset(table, 0xA5, table_size);
	feedbackbuild_timestamp(table, ((uint64_t)0xA1B2C3D4U << 32) | 0x05060708U);
	for (class_index = 0U; class_index < layout.classes; class_index++) {
		for (capability = 0U; capability < layout.capabilities; capability++)
			feedbackbuild_change(table, table_size, layout.capabilities, class_index, capability,
			    change_value(class_index, capability));
	}
	for (row = 0U; row < layout.row_count; row++) {
		for (class_index = 0U; class_index < layout.classes; class_index++) {
			for (capability = 0U; capability < layout.capabilities; capability++)
				feedbackbuild_cell(table, table_size, layout.capabilities, layout.classes, row, class_index,
				    capability, cell_value(row, class_index, capability));
		}
	}

	/* The timestamp is the first eight bytes, least significant first. */
	timestamp = 0U;
	CHECK(cpux86_feedback_timestamp(table, table_size, &layout, &timestamp) == 0);
	CHECK(timestamp == (((uint64_t)0xA1B2C3D4U << 32) | 0x05060708U));

	/* Every change byte is read back. */
	for (class_index = 0U; class_index < layout.classes; class_index++) {
		for (capability = 0U; capability < layout.capabilities; capability++) {
			flags = 0xEEEEU;
			CHECK(cpux86_feedback_change(table, table_size, &layout, class_index, capability, &flags) == 0);
			CHECK(flags == change_value(class_index, capability));
		}
	}

	/* Every cell is read back, including the last cell of the last row. */
	for (row = 0U; row < layout.row_count; row++) {
		for (class_index = 0U; class_index < layout.classes; class_index++) {
			for (capability = 0U; capability < layout.capabilities; capability++) {
				value = 0xEEEEU;
				CHECK(cpux86_feedback_cell(table, table_size, &layout, row, class_index, capability, &value) == 0);
				CHECK(value == cell_value(row, class_index, capability));
			}
		}
	}

	/* An index one past the layout is refused and leaves the output alone. */
	value = 0xEEEEU;
	CHECK(cpux86_feedback_cell(table, table_size, &layout, layout.row_count, 0U, 0U, &value) == EINVAL);
	CHECK(cpux86_feedback_cell(table, table_size, &layout, 0U, layout.classes, 0U, &value) == EINVAL);
	CHECK(cpux86_feedback_cell(table, table_size, &layout, 0U, 0U, layout.capabilities, &value) == EINVAL);
	CHECK(value == 0xEEEEU);
	flags = 0xEEEEU;
	CHECK(cpux86_feedback_change(table, table_size, &layout, layout.classes, 0U, &flags) == EINVAL);
	CHECK(cpux86_feedback_change(table, table_size, &layout, 0U, layout.capabilities, &flags) == EINVAL);
	CHECK(flags == 0xEEEEU);

	/* A table one byte short of the last cell refuses it; a table that ends right after it reads it. */
	row = layout.row_count - 1U;
	class_index = layout.classes - 1U;
	capability = layout.capabilities - 1U;
	table_size = feedbackbuild_cell_offset(layout.capabilities, layout.classes, row, class_index, capability);
	value = 0xEEEEU;
	CHECK(cpux86_feedback_cell(table, table_size, &layout, row, class_index, capability, &value) == EINVAL);
	CHECK(value == 0xEEEEU);
	CHECK(cpux86_feedback_cell(table, table_size + 1U, &layout, row, class_index, capability, &value) == 0);
	CHECK(value == cell_value(row, class_index, capability));

	/* A change byte one byte past the table is refused. */
	table_size = 8U + layout.capabilities * (layout.classes - 1U) + layout.capabilities - 1U;
	flags = 0xEEEEU;
	CHECK(cpux86_feedback_change(table, table_size, &layout, layout.classes - 1U, layout.capabilities - 1U, &flags) == EINVAL);
	CHECK(flags == 0xEEEEU);
	CHECK(cpux86_feedback_change(table, table_size + 1U, &layout, layout.classes - 1U, layout.capabilities - 1U, &flags) == 0);

	/* NULL arguments are refused. */
	CHECK(cpux86_feedback_cell(NULL, layout.table_size, &layout, 0U, 0U, 0U, &value) == EINVAL);
	CHECK(cpux86_feedback_cell(table, layout.table_size, NULL, 0U, 0U, 0U, &value) == EINVAL);
	CHECK(cpux86_feedback_cell(table, layout.table_size, &layout, 0U, 0U, 0U, NULL) == EINVAL);
	CHECK(cpux86_feedback_change(NULL, layout.table_size, &layout, 0U, 0U, &flags) == EINVAL);
	CHECK(cpux86_feedback_change(table, layout.table_size, NULL, 0U, 0U, &flags) == EINVAL);
	CHECK(cpux86_feedback_change(table, layout.table_size, &layout, 0U, 0U, NULL) == EINVAL);
	CHECK(cpux86_feedback_timestamp(NULL, layout.table_size, &layout, &timestamp) == EINVAL);
	CHECK(cpux86_feedback_timestamp(table, layout.table_size, NULL, &timestamp) == EINVAL);
	CHECK(cpux86_feedback_timestamp(table, layout.table_size, &layout, NULL) == EINVAL);

	free(table);
}

/*
 * Checks the table readers on tables built the way the processor writes them.
 */
static void
test_table(void)
{
	struct cpux86_power power;
	struct cpux86_feedback_layout layout;
	uint8_t bytes[64];
	uint64_t timestamp;

	/* The shapes of a plain table, the Alder Lake P core's table, five classes, three capabilities. */
	check_table(0U, 0U, 0x03U, 1U);
	check_table(1U, 4U, 0x03U, 1U);
	check_table(1U, 5U, 0x03U, 1U);
	check_table(1U, 4U, 0x07U, 1U);
	check_table(1U, 255U, 0x03U, 16U);
	check_table(1U, 255U, 0xFFU, 1U);
	check_table(1U, 8U, 0x03U, 3U);

	/* The timestamp needs only the header: 8 bytes at least, and the header's size. */
	fill_power(&power, 0U, 1U, 0x03U, 1U);
	CHECK(cpux86_feedback_layout(&power, &layout) == 0);
	memset(bytes, 0, sizeof(bytes));
	feedbackbuild_timestamp(bytes, ((uint64_t)0x80000001U << 32) | 0x0000FF01U);
	timestamp = 0x1234U;
	CHECK(cpux86_feedback_timestamp(bytes, 7U, &layout, &timestamp) == EINVAL);
	CHECK(cpux86_feedback_timestamp(bytes, 8U, &layout, &timestamp) == EINVAL);
	CHECK(cpux86_feedback_timestamp(bytes, 15U, &layout, &timestamp) == EINVAL);
	CHECK(timestamp == 0x1234U);
	CHECK(cpux86_feedback_timestamp(bytes, 16U, &layout, &timestamp) == 0);
	CHECK(timestamp == (((uint64_t)0x80000001U << 32) | 0x0000FF01U));
	feedbackbuild_timestamp(bytes, 0U);
	CHECK(cpux86_feedback_timestamp(bytes, 16U, &layout, &timestamp) == 0);
	CHECK(timestamp == 0U);
	feedbackbuild_timestamp(bytes, ~(uint64_t)0U);
	CHECK(cpux86_feedback_timestamp(bytes, 16U, &layout, &timestamp) == 0);
	CHECK(timestamp == ~(uint64_t)0U);
}

/*
 * Checks readers against layouts that lie about the table.
 */
static void
test_forged_layout(void)
{
	struct cpux86_feedback_layout layout;
	uint8_t *table;
	uint32_t value;
	uint32_t flags;
	uint64_t timestamp;
	uint32_t round;
	uint32_t error;
	size_t table_size;

	/* A table of exactly 4096 bytes, so the sanitizer sees every read outside it. */
	table = (uint8_t *)malloc(4096U);
	CHECK(table != NULL);
	if (table == NULL)
		return;
	memset(table, 0x5A, 4096U);

	/* A row size of 0x80000000. */
	layout.capabilities = 2U;
	layout.classes = 4U;
	layout.header_size = 16U;
	layout.row_size = 0x80000000U;
	layout.table_size = 4096U;
	layout.row_count = 0xFFFFFFFFU;
	value = 0xEEU;
	CHECK(cpux86_feedback_cell(table, 4096U, &layout, 1U, 0U, 0U, &value) == EINVAL);
	CHECK(cpux86_feedback_cell(table, 4096U, &layout, 0xFFFFFFFEU, 3U, 1U, &value) == EINVAL);
	CHECK(value == 0xEEU);
	CHECK(cpux86_feedback_cell(table, 4096U, &layout, 0U, 3U, 1U, &value) == 0);
	CHECK(value == 0x5AU);

	/* Capabilities and classes of 0xFFFFFFFF. */
	layout.capabilities = 0xFFFFFFFFU;
	layout.classes = 0xFFFFFFFFU;
	layout.header_size = 8U;
	layout.row_size = 8U;
	layout.row_count = 0xFFFFFFFFU;
	value = 0xEEU;
	CHECK(cpux86_feedback_cell(table, 4096U, &layout, 0xFFFFFFFEU, 0xFFFFFFFEU, 0xFFFFFFFEU, &value) == EINVAL);
	CHECK(cpux86_feedback_cell(table, 4096U, &layout, 0U, 0xFFFFFFFEU, 0xFFFFFFFEU, &value) == EINVAL);
	CHECK(cpux86_feedback_cell(table, 4096U, &layout, 0U, 0U, 0xFFFFFFFEU, &value) == EINVAL);
	CHECK(cpux86_feedback_cell(table, 4096U, &layout, 0xFFFFFFFEU, 0U, 0U, &value) == EINVAL);
	CHECK(value == 0xEEU);
	flags = 0xEEU;
	CHECK(cpux86_feedback_change(table, 4096U, &layout, 0xFFFFFFFEU, 0xFFFFFFFEU, &flags) == EINVAL);
	CHECK(cpux86_feedback_change(table, 4096U, &layout, 0xFFFFFFFEU, 0U, &flags) == EINVAL);
	CHECK(flags == 0xEEU);

	/* A header size of 0xFFFFFFFF. */
	layout.capabilities = 2U;
	layout.classes = 4U;
	layout.header_size = 0xFFFFFFFFU;
	layout.row_size = 8U;
	layout.row_count = 1U;
	value = 0xEEU;
	CHECK(cpux86_feedback_cell(table, 4096U, &layout, 0U, 0U, 0U, &value) == EINVAL);
	CHECK(cpux86_feedback_timestamp(table, 4096U, &layout, &timestamp) == EINVAL);
	CHECK(value == 0xEEU);

	/* A row count of 0xFFFFFFFF with a large row. */
	layout.header_size = 16U;
	layout.row_size = 0xFFFFFFF8U;
	layout.row_count = 0xFFFFFFFFU;
	CHECK(cpux86_feedback_cell(table, 4096U, &layout, 0xFFFFFFFEU, 0U, 0U, &value) == EINVAL);
	CHECK(cpux86_feedback_cell(table, 4096U, &layout, 1U, 0U, 0U, &value) == EINVAL);
	CHECK(value == 0xEEU);

	/* A zero-sized table has no byte to read. */
	layout.capabilities = 2U;
	layout.classes = 1U;
	layout.header_size = 16U;
	layout.row_size = 8U;
	layout.row_count = 510U;
	CHECK(cpux86_feedback_cell(table, 0U, &layout, 0U, 0U, 0U, &value) == EINVAL);
	CHECK(cpux86_feedback_change(table, 0U, &layout, 0U, 0U, &flags) == EINVAL);
	CHECK(cpux86_feedback_timestamp(table, 0U, &layout, &timestamp) == EINVAL);

	/* Random layouts and indexes: an error, or a byte inside the table. */
	for (round = 0U; round < 20000U; round++) {
		layout.capabilities = next32();
		layout.classes = next32();
		layout.header_size = next32();
		layout.row_size = next32();
		layout.table_size = next32();
		layout.row_count = next32();
		if ((round & 1U) != 0U) {
			layout.capabilities &= 0xFU;
			layout.classes &= 0xFFU;
			layout.header_size &= 0xFFFU;
			layout.row_size &= 0xFFFU;
			layout.row_count &= 0x3FFU;
		}
		table_size = (size_t)(next32() % 4097U);
		if (table_size > 4096U)
			table_size = 4096U;
		error = (uint32_t)cpux86_feedback_cell(table, table_size, &layout, next32() & 0x3FFU, next32() & 0xFFU,
		    next32() & 0xFU, &value);
		CHECK(error == 0U || error == EINVAL);
		error = (uint32_t)cpux86_feedback_change(table, table_size, &layout, next32() & 0xFFU, next32() & 0xFU,
		    &flags);
		CHECK(error == 0U || error == EINVAL);
		error = (uint32_t)cpux86_feedback_timestamp(table, table_size, &layout, &timestamp);
		CHECK(error == 0U || error == EINVAL);
		error = (uint32_t)cpux86_feedback_cell(table, table_size, &layout, next32(), next32(), next32(), &value);
		CHECK(error == 0U || error == EINVAL);
	}

	free(table);
}

/*
 * Checks the feedback pointer.
 */
static void
test_pointer(void)
{
	uint64_t raw;
	uint32_t bits;
	uint32_t bit;

	/* A page frame is the address with the valid bit. */
	raw = 0xEEU;
	CHECK(cpux86_feedback_pointer_encode(0x1000U, 36U, &raw) == 0);
	CHECK(raw == 0x1001U);
	CHECK(cpux86_feedback_pointer_encode(0U, 36U, &raw) == 0);
	CHECK(raw == 1U);
	CHECK(cpux86_feedback_pointer_encode(0x123456000U, 36U, &raw) == 0);
	CHECK(raw == 0x123456001U);

	/* The last page below 2^bits is valid, the address 2^bits is not. */
	CHECK(cpux86_feedback_pointer_encode(((uint64_t)1U << 36) - 0x1000U, 36U, &raw) == 0);
	CHECK(raw == ((((uint64_t)1U << 36) - 0x1000U) | 1U));
	raw = 0xEEU;
	CHECK(cpux86_feedback_pointer_encode((uint64_t)1U << 36, 36U, &raw) == EINVAL);
	CHECK(cpux86_feedback_pointer_encode(((uint64_t)1U << 36) + 0x1000U, 36U, &raw) == EINVAL);
	CHECK(cpux86_feedback_pointer_encode(~(uint64_t)0U - 0xFFFU, 36U, &raw) == EINVAL);
	CHECK(raw == 0xEEU);

	/* The widest and the narrowest address widths. */
	CHECK(cpux86_feedback_pointer_encode(((uint64_t)1U << 52) - 0x1000U, 52U, &raw) == 0);
	CHECK(raw == ((((uint64_t)1U << 52) - 0x1000U) | 1U));
	raw = 0xEEU;
	CHECK(cpux86_feedback_pointer_encode((uint64_t)1U << 52, 52U, &raw) == EINVAL);
	CHECK(cpux86_feedback_pointer_encode(0U, 12U, &raw) == 0);
	CHECK(raw == 1U);
	raw = 0xEEU;
	CHECK(cpux86_feedback_pointer_encode(0x1000U, 12U, &raw) == EINVAL);
	CHECK(raw == 0xEEU);

	/* An address width outside 12..52 is refused. */
	for (bits = 0U; bits < 12U; bits++)
		CHECK(cpux86_feedback_pointer_encode(0U, bits, &raw) == EINVAL);
	CHECK(cpux86_feedback_pointer_encode(0U, 53U, &raw) == EINVAL);
	CHECK(cpux86_feedback_pointer_encode(0U, 64U, &raw) == EINVAL);
	CHECK(cpux86_feedback_pointer_encode(0U, 0xFFFFFFFFU, &raw) == EINVAL);
	CHECK(raw == 0xEEU);

	/* An address with any of bits 11:0 set is refused. */
	for (bit = 0U; bit < 12U; bit++)
		CHECK(cpux86_feedback_pointer_encode(0x2000U | ((uint64_t)1U << bit), 36U, &raw) == EINVAL);
	CHECK(raw == 0xEEU);

	/* A missing output is refused. */
	CHECK(cpux86_feedback_pointer_encode(0x1000U, 36U, NULL) == EINVAL);
}

/*
 * Checks the feedback configuration (Table 17-10).
 */
static void
test_config(void)
{
	uint64_t raw;
	uint32_t enable;
	uint32_t thread_director;
	uint32_t supported;

	/* The valid combinations: off, HFI, HFI with Thread Director. */
	raw = 0xEEU;
	CHECK(cpux86_feedback_config_encode(0U, 0U, 0U, &raw) == 0);
	CHECK(raw == 0U);
	CHECK(cpux86_feedback_config_encode(0U, 0U, 1U, &raw) == 0);
	CHECK(raw == 0U);
	CHECK(cpux86_feedback_config_encode(1U, 0U, 0U, &raw) == 0);
	CHECK(raw == 1U);
	CHECK(cpux86_feedback_config_encode(1U, 0U, 1U, &raw) == 0);
	CHECK(raw == 1U);
	CHECK(cpux86_feedback_config_encode(1U, 1U, 1U, &raw) == 0);
	CHECK(raw == 3U);

	/* Thread Director without enable is refused, supported or not. */
	raw = 0xEEU;
	CHECK(cpux86_feedback_config_encode(0U, 1U, 0U, &raw) == EINVAL);
	CHECK(cpux86_feedback_config_encode(0U, 1U, 1U, &raw) == EINVAL);
	CHECK(raw == 0xEEU);

	/* Thread Director without support is not supported. */
	CHECK(cpux86_feedback_config_encode(1U, 1U, 0U, &raw) == ENOTSUP);
	CHECK(raw == 0xEEU);

	/* An argument above 1 is refused first. */
	CHECK(cpux86_feedback_config_encode(2U, 0U, 0U, &raw) == EINVAL);
	CHECK(cpux86_feedback_config_encode(1U, 2U, 1U, &raw) == EINVAL);
	CHECK(cpux86_feedback_config_encode(1U, 1U, 2U, &raw) == EINVAL);
	CHECK(cpux86_feedback_config_encode(1U, 0U, 2U, &raw) == EINVAL);
	CHECK(cpux86_feedback_config_encode(0xFFFFFFFFU, 0xFFFFFFFFU, 0xFFFFFFFFU, &raw) == EINVAL);
	CHECK(raw == 0xEEU);

	/* A missing output is refused. */
	CHECK(cpux86_feedback_config_encode(1U, 0U, 0U, NULL) == EINVAL);

	/* Every one of the eight combinations answers as the rules say. */
	for (enable = 0U; enable < 2U; enable++) {
		for (thread_director = 0U; thread_director < 2U; thread_director++) {
			for (supported = 0U; supported < 2U; supported++) {
				raw = 0xEEU;
				if (thread_director == 1U && enable == 0U) {
					CHECK(cpux86_feedback_config_encode(enable, thread_director, supported, &raw) == EINVAL);
				} else if (thread_director == 1U && supported == 0U) {
					CHECK(cpux86_feedback_config_encode(enable, thread_director, supported, &raw) == ENOTSUP);
				} else {
					CHECK(cpux86_feedback_config_encode(enable, thread_director, supported, &raw) == 0);
					CHECK(raw == (uint64_t)(enable | (thread_director << 1)));
				}
			}
		}
	}
}

/*
 * Checks the update flag and its acknowledgement in IA32_PACKAGE_THERM_STATUS.
 */
static void
test_updated_ack(void)
{
	uint32_t updated;
	uint64_t value;
	uint64_t raw;
	uint32_t bit;
	uint32_t round;

	/* Bit 26 alone decides the flag. */
	updated = 0xEEU;
	CHECK(cpux86_feedback_updated(0U, &updated) == 0);
	CHECK(updated == 0U);
	CHECK(cpux86_feedback_updated((uint64_t)1U << 26, &updated) == 0);
	CHECK(updated == 1U);
	CHECK(cpux86_feedback_updated(~((uint64_t)1U << 26), &updated) == 0);
	CHECK(updated == 0U);
	CHECK(cpux86_feedback_updated(~(uint64_t)0U, &updated) == 0);
	CHECK(updated == 1U);
	CHECK(cpux86_feedback_updated(0U, NULL) == EINVAL);
	CHECK(CPUX86_PACKAGE_THERM_FEEDBACK == 26U);

	/* The acknowledgement keeps the log bits 3, 5, 7, 9 and 11 at 1, whatever was read. */
	value = 0xEEU;
	CHECK(cpux86_feedback_ack(0U, &value) == 0);
	CHECK(value == 0xAA8U);
	CHECK(cpux86_feedback_ack(~(uint64_t)0U, &value) == 0);
	CHECK(value == 0xAAAU);

	/* Only bit 1 follows what was read; bit 26 and every other bit is 0. */
	CHECK(cpux86_feedback_ack(2U, &value) == 0);
	CHECK(value == 0xAAAU);
	CHECK(cpux86_feedback_ack((uint64_t)1U << 26, &value) == 0);
	CHECK(value == 0xAA8U);
	CHECK(cpux86_feedback_ack(((uint64_t)1U << 26) | 2U, &value) == 0);
	CHECK(value == 0xAAAU);
	for (bit = 0U; bit < 64U; bit++) {
		if (bit == 1U)
			continue;
		CHECK(cpux86_feedback_ack((uint64_t)1U << bit, &value) == 0);
		CHECK(value == 0xAA8U);
	}

	/* The value never has bit 26 set, and its set bits are among the log bits and bit 1. */
	for (round = 0U; round < 1000U; round++) {
		raw = next64();
		CHECK(cpux86_feedback_ack(raw, &value) == 0);
		CHECK((value & ((uint64_t)1U << 26)) == 0U);
		CHECK((value & ~(uint64_t)0xAAAU) == 0U);
		CHECK((value & 0xAA8U) == 0xAA8U);
		CHECK((value & 2U) == (raw & 2U));
	}

	/* A missing output is refused. */
	CHECK(cpux86_feedback_ack(0U, NULL) == EINVAL);
}

/*
 * Checks that the answers are only ever the ones the header names, for random input.
 */
static void
test_random(void)
{
	uint64_t raw;
	uint32_t round;
	uint32_t error;
	uint32_t field;
	uint32_t source;
	uint32_t number;
	uint64_t out;
	struct cpux86_hwp_capabilities capabilities;
	struct cpux86_hwp_request request;
	struct cpux86_hwp_request again;
	struct cpux86_power power;
	struct cpux86_feedback_layout layout;

	/* Feeds 64-bit values to every register function. */
	for (round = 0U; round < 20000U; round++) {
		raw = next64();
		if ((round & 3U) == 0U)
			raw &= 0x00000000FFFFFFFFU;
		if ((round & 3U) == 1U)
			raw &= 0x07FFFFFFFFFFFFFFU;

		error = (uint32_t)cpux86_hwp_enabled(raw, &number);
		CHECK(error == 0U || error == EINVAL);
		error = (uint32_t)cpux86_hwp_capabilities_decode(raw, &capabilities);
		CHECK(error == 0U || error == EINVAL);
		error = (uint32_t)cpux86_hwp_status_decode(raw, &number);
		CHECK(error == 0U || error == EINVAL);
		error = (uint32_t)cpux86_hwp_status_ack((uint32_t)raw, &out);
		CHECK(error == 0U || error == EINVAL);
		error = (uint32_t)cpux86_feedback_updated(raw, &number);
		CHECK(error == 0U);
		error = (uint32_t)cpux86_feedback_ack(raw, &out);
		CHECK(error == 0U);

		/* A request that decodes encodes back to the same register. */
		error = (uint32_t)cpux86_hwp_request_decode(raw, &request);
		CHECK(error == 0U || error == EINVAL);
		if (error == 0U) {
			CHECK(cpux86_hwp_request_encode(&request, NULL, &out) == 0);
			CHECK(out == raw);
			CHECK(cpux86_hwp_request_decode(out, &again) == 0);
			CHECK(again.window == request.window);
		}

		error = (uint32_t)cpux86_hwp_window_decode((uint32_t)raw, &number);
		CHECK(error == 0U || error == EINVAL);
		error = (uint32_t)cpux86_hwp_window_encode((uint32_t)(raw >> 32), &field);
		CHECK(error == 0U || error == ERANGE);
		error = (uint32_t)cpux86_hwp_source((uint32_t)raw, (uint32_t)(raw >> 32), &source);
		CHECK(error == 0U || error == EINVAL);
		error = (uint32_t)cpux86_feedback_pointer_encode(raw, (uint32_t)(next32() % 70U), &out);
		CHECK(error == 0U || error == EINVAL);
		error = (uint32_t)cpux86_feedback_config_encode((uint32_t)raw & 3U, (uint32_t)(raw >> 8) & 3U,
		    (uint32_t)(raw >> 16) & 3U, &out);
		CHECK(error == 0U || error == EINVAL || error == ENOTSUP);

		/* A request with random fields, even out of width, answers only 0 or EINVAL. */
		fill_request(&request, next32(), next32(), next32(), next32(), next32(), next32(), next32());
		if ((round & 1U) != 0U)
			fill_request(&request, next32() & 0x1FFU, next32() & 0x1FFU, next32() & 0x1FFU, next32() & 0x1FFU,
			    next32() & 0x7FFU, next32() & 3U, next32() & 0x3FU);
		capabilities.highest = next32() & 0xFFU;
		capabilities.guaranteed = next32() & 0xFFU;
		capabilities.efficient = next32() & 0xFFU;
		capabilities.lowest = next32() & 0xFFU;
		error = (uint32_t)cpux86_hwp_request_encode(&request, &capabilities, &out);
		CHECK(error == 0U || error == EINVAL);
		if (error == 0U) {
			CHECK(request.minimum >= capabilities.lowest);
			CHECK(request.minimum <= request.maximum);
			CHECK(request.maximum <= capabilities.highest);
			CHECK(request.desired == 0U ||
			    (request.desired >= capabilities.lowest && request.desired <= capabilities.highest));
		}

		/* A processor's leaf 06H values, forged, give a layout that fits the table or an error. */
		power.features = next32();
		power.thermal_thresholds = next32();
		power.counters = next32();
		power.feedback_classes = (round & 1U) != 0U ? next32() & 0xFFU : next32();
		power.feedback_capabilities = (round & 2U) != 0U ? next32() & 0xFFU : next32();
		power.feedback_pages = (round & 4U) != 0U ? (next32() & 0x1FU) : next32();
		power.feedback_row = next32();
		error = (uint32_t)cpux86_feedback_layout(&power, &layout);
		CHECK(error == 0U || error == EINVAL || error == ENOTSUP);
		if (error == 0U) {
			CHECK(layout.row_count >= 1U);
			CHECK((uint64_t)layout.header_size + (uint64_t)layout.row_count * layout.row_size <= layout.table_size);
			CHECK(layout.header_size % 8U == 0U);
			CHECK(layout.row_size % 8U == 0U);
			CHECK(layout.header_size == 8U + layout.row_size);
			CHECK(layout.table_size == power.feedback_pages * 4096U);
		}
	}
}

/*
 * Runs the HWP and feedback decoder checks.
 */
int
main(void)
{
	/* Runs every group of checks. */
	test_pm_enable();
	test_capabilities();
	test_request_encode_widths();
	test_request_encode_order();
	test_request_decode();
	test_window();
	test_source();
	test_status();
	test_layout();
	test_table();
	test_forged_layout();
	test_pointer();
	test_config();
	test_updated_ack();
	test_random();

	/* Reports the summary line. */
	return check_finish("hwp");
}
