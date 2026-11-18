/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The host check of the Mali firmware interface reader
 * (src/drivers/gpu/mali/mali-interface.c) and of the first OSD plane's
 * decoding (src/drivers/gpu/mali/display/osd.c): a shared section laid
 * out by hand is read back with every part where it was put, a section
 * with a part outside it, no version, too few groups or unlike groups
 * and streams is refused, the request words flip and set the right
 * bits, the timer words are rounded and saturated, and a plane is
 * decoded, matched against a framebuffer and refused when off, on a
 * canvas or showing a window away from its corner.
 *
 * cc -std=c11 -Wall -Wextra -Werror -Isrc -Iinclude \
 *    tests/mali-interface-host-test.c src/drivers/gpu/mali/mali-interface.c \
 *    src/drivers/gpu/mali/display/osd.c -o <out> && <out>
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <uapi/errno.h>

#include "drivers/gpu/mali/display/osd.h"
#include "drivers/gpu/mali/mali-interface.h"

/* The shared section's size and the microcontroller address it is seen at. */
#define SHARED_BYTES		0x10000U
#define SHARED_ADDRESS		0x04000000U

/* Where the hand-made layout puts each part. */
#define GLOBAL_INPUT_AT		0x0200U
#define GLOBAL_OUTPUT_AT	0x0300U
#define GROUP_STRIDE		0x0400U
#define GROUPS			3U
#define STREAMS			8U
#define STREAM_STRIDE		0x0040U
#define GROUP_PARTS_AT		0x8000U
#define STREAM_PARTS_AT		0xa000U

/* The shared section, word aligned like the real one. */
static uint32_t shared_words[SHARED_BYTES / 4U];

/* How many checks failed. */
static int failures;

static void check(int condition, const char *what);
static uint8_t *shared(void);
static void put32(uint32_t offset, uint32_t value);
static void lay_out(void);
static void check_layout(void);
static void check_refusals(void);
static void check_requests(void);
static void check_timers(void);
static void check_plane(void);

/* Runs every check and reports the count of failures. */
int
main(void)
{
	/* The reader, the request words, the timers, then the plane. */
	check_layout();
	check_refusals();
	check_requests();
	check_timers();
	check_plane();

	/* Reports the failures. */
	if (failures != 0) {
		printf("mali-interface: %d checks failed\n", failures);
		return 1;
	}

	/* Succeeded: every check passed. */
	printf("mali-interface: all checks passed\n");
	return 0;
}

/* Counts and reports one failed check. */
static void
check(
	int condition,
	const char *what)
{
	/* A passing check says nothing. */
	if (condition)
		return;

	printf("FAIL: %s\n", what);
	failures++;
}

/* The section's bytes. */
static uint8_t *
shared(void)
{
	/* The words seen as bytes. */
	return (uint8_t *)shared_words;
}

/* Writes one word of the section at a byte offset. */
static void
put32(
	uint32_t offset,
	uint32_t value)
{
	/* The section is in host order, as the reader reads it. */
	shared_words[offset / 4U] = value;
}

/*
 * Lays out a section of three groups of eight streams: the global block,
 * each group's control part a stride apart from 0x1000, its streams'
 * control parts after it, and every input and output part elsewhere.
 */
static void
lay_out(void)
{
	uint32_t group;
	uint32_t stream;
	uint32_t control;
	uint32_t parts;
	uint32_t index;

	/* The global control part. */
	memset(shared_words, 0, sizeof(shared_words));
	put32(MALI_GLB_CONTROL_VERSION, 0x01020003U);
	put32(MALI_GLB_CONTROL_FEATURES, 0x5U);
	put32(MALI_GLB_CONTROL_INPUT, SHARED_ADDRESS + GLOBAL_INPUT_AT);
	put32(MALI_GLB_CONTROL_OUTPUT, SHARED_ADDRESS + GLOBAL_OUTPUT_AT);
	put32(MALI_GLB_CONTROL_GROUPS, GROUPS);
	put32(MALI_GLB_CONTROL_GROUP_STRIDE, GROUP_STRIDE);

	/* Every group and its streams. */
	for (group = 0U; group < GROUPS; group++) {
		control = MALI_IFACE_GROUP_OFFSET + group * GROUP_STRIDE;
		parts = GROUP_PARTS_AT + group * 0x200U;
		put32(control + MALI_CSG_CONTROL_FEATURES, 0x1U);
		put32(control + MALI_CSG_CONTROL_INPUT, SHARED_ADDRESS + parts);
		put32(control + MALI_CSG_CONTROL_OUTPUT, SHARED_ADDRESS + parts + 0x100U);
		put32(control + MALI_CSG_CONTROL_SUSPEND, 0x3000U);
		put32(control + MALI_CSG_CONTROL_STREAMS, STREAMS);
		put32(control + MALI_CSG_CONTROL_STREAM_STRIDE, STREAM_STRIDE);
		for (stream = 0U; stream < STREAMS; stream++) {
			index = group * STREAMS + stream;
			put32(control + MALI_IFACE_STREAM_OFFSET + stream * STREAM_STRIDE + MALI_CS_CONTROL_FEATURES, 0x7005fU);
			put32(control + MALI_IFACE_STREAM_OFFSET + stream * STREAM_STRIDE + MALI_CS_CONTROL_INPUT, SHARED_ADDRESS + STREAM_PARTS_AT + index * 0x200U);
			put32(control + MALI_IFACE_STREAM_OFFSET + stream * STREAM_STRIDE + MALI_CS_CONTROL_OUTPUT, SHARED_ADDRESS + STREAM_PARTS_AT + index * 0x200U + 0x100U);
		}
	}
}

/* Reads the hand-made layout back. */
static void
check_layout(void)
{
	struct mali_interfaces interfaces;
	int error;

	/* Every part where it was put. */
	lay_out();
	error = mali_iface_read(shared(), SHARED_ADDRESS, SHARED_BYTES, &interfaces);
	check(error == 0, "layout: read");
	check(interfaces.version == 0x01020003U, "layout: version");
	check(interfaces.group_count == GROUPS, "layout: groups");
	check(interfaces.input == shared() + GLOBAL_INPUT_AT, "layout: global input");
	check(interfaces.output == shared() + GLOBAL_OUTPUT_AT, "layout: global output");
	check(interfaces.groups[2].control == shared() + MALI_IFACE_GROUP_OFFSET + 2U * GROUP_STRIDE, "layout: group 2 control");
	check(interfaces.groups[2].input == shared() + GROUP_PARTS_AT + 0x400U, "layout: group 2 input");
	check(interfaces.groups[1].suspend_bytes == 0x3000U, "layout: suspend size");
	check(interfaces.groups[0].stream_count == STREAMS, "layout: streams");
	check(interfaces.groups[1].streams[7].control == shared() + MALI_IFACE_GROUP_OFFSET + GROUP_STRIDE + MALI_IFACE_STREAM_OFFSET + 7U * STREAM_STRIDE, "layout: stream control");
	check(interfaces.groups[1].streams[7].output == shared() + STREAM_PARTS_AT + 15U * 0x200U + 0x100U, "layout: stream output");
	check(interfaces.groups[0].streams[0].features == 0x7005fU, "layout: stream features");
}

/* Refuses sections a working firmware would not publish. */
static void
check_refusals(void)
{
	struct mali_interfaces interfaces;
	int error;

	/* No version: the firmware did not start. */
	lay_out();
	put32(MALI_GLB_CONTROL_VERSION, 0U);
	error = mali_iface_read(shared(), SHARED_ADDRESS, SHARED_BYTES, &interfaces);
	check(error == EINVAL, "refusal: no version");

	/* A global input before the section. */
	lay_out();
	put32(MALI_GLB_CONTROL_INPUT, SHARED_ADDRESS - 0x100U);
	error = mali_iface_read(shared(), SHARED_ADDRESS, SHARED_BYTES, &interfaces);
	check(error == EINVAL, "refusal: input before the section");

	/* A stream output running past the section's end. */
	lay_out();
	put32(MALI_IFACE_GROUP_OFFSET + MALI_IFACE_STREAM_OFFSET + MALI_CS_CONTROL_OUTPUT, SHARED_ADDRESS + SHARED_BYTES - 0x10U);
	error = mali_iface_read(shared(), SHARED_ADDRESS, SHARED_BYTES, &interfaces);
	check(error == EINVAL, "refusal: output past the end");

	/* Too few groups. */
	lay_out();
	put32(MALI_GLB_CONTROL_GROUPS, 2U);
	error = mali_iface_read(shared(), SHARED_ADDRESS, SHARED_BYTES, &interfaces);
	check(error == EINVAL, "refusal: two groups");

	/* Too few streams in a group. */
	lay_out();
	put32(MALI_IFACE_GROUP_OFFSET + MALI_CSG_CONTROL_STREAMS, 7U);
	error = mali_iface_read(shared(), SHARED_ADDRESS, SHARED_BYTES, &interfaces);
	check(error == EINVAL, "refusal: seven streams");

	/* A group unlike the first. */
	lay_out();
	put32(MALI_IFACE_GROUP_OFFSET + 2U * GROUP_STRIDE + MALI_CSG_CONTROL_SUSPEND, 0x4000U);
	error = mali_iface_read(shared(), SHARED_ADDRESS, SHARED_BYTES, &interfaces);
	check(error == EPROTO, "refusal: unlike group");

	/* A stream unlike the group's first. */
	lay_out();
	put32(MALI_IFACE_GROUP_OFFSET + MALI_IFACE_STREAM_OFFSET + 3U * STREAM_STRIDE + MALI_CS_CONTROL_FEATURES, 0x1fU);
	error = mali_iface_read(shared(), SHARED_ADDRESS, SHARED_BYTES, &interfaces);
	check(error == EPROTO, "refusal: unlike stream");

	/* A section too small for the groups' blocks. */
	lay_out();
	error = mali_iface_read(shared(), SHARED_ADDRESS, 0x800U, &interfaces);
	check(error == EINVAL, "refusal: small section");
}

/* Flips, sets and compares request words. */
static void
check_requests(void)
{
	uint32_t word;

	/* A flip away from the answer under the mask, the other bits kept. */
	word = mali_iface_toggle(0x00000003U, 0x00000000U, MALI_GLB_PING);
	check(word == 0x00000103U, "requests: ping flips on");
	word = mali_iface_toggle(0x00000103U, 0x00000100U, MALI_GLB_PING);
	check(word == 0x00000003U, "requests: ping flips back");

	/* A set under the mask only. */
	word = mali_iface_update(0x00000106U, MALI_CSG_STATE_START, MALI_CSG_STATE_MASK);
	check(word == 0x00000101U, "requests: state set");

	/* Answered when the masked bits agree. */
	check(mali_iface_acked(0x00000103U, 0x00000100U, MALI_GLB_PING) == 1, "requests: answered");
	check(mali_iface_acked(0x00000103U, 0x00000000U, MALI_GLB_PING) == 0, "requests: not answered");
}

/* Converts times into timer words. */
static void
check_timers(void)
{
	/* 10 ms and 800 µs at 400 MHz, in units of 1024 cycles rounded up. */
	check(mali_iface_timer(10000U, 400000000U) == (3907U | MALI_TIMER_GPU_COUNTER), "timers: power off");
	check(mali_iface_timer(800U, 400000000U) == (313U | MALI_TIMER_GPU_COUNTER), "timers: idle");

	/* No rate and a time too long both give the longest count. */
	check(mali_iface_timer(800U, 0U) == (MALI_TIMER_COUNT_MASK | MALI_TIMER_GPU_COUNTER), "timers: no rate");
	check(mali_iface_timer(UINT64_MAX / 2U, 400000000U) == (MALI_TIMER_COUNT_MASK | MALI_TIMER_GPU_COUNTER), "timers: saturated");
}

/* Decodes the first OSD plane's words. */
static void
check_plane(void)
{
	struct mali_osd_plane plane;
	uint32_t word;
	int error;

	/* A 1920x1080 linear buffer at 0x7f800000, rows of 7680 bytes. */
	memset(&plane, 0, sizeof(plane));
	plane.control = MALI_OSD_CONTROL_ENABLE | MALI_OSD_CONTROL_BLOCK | MALI_OSD_CONTROL_LINEAR;
	plane.window_x = 1919U << MALI_OSD_WINDOW_LAST_SHIFT;
	plane.window_y = 1079U << MALI_OSD_WINDOW_LAST_SHIFT;
	plane.address = 0x7f800000U >> 4;
	plane.stride = 7680U >> 4;
	error = mali_osd_decode(&plane);
	check(error == 0, "plane: decoded");
	check(plane.buffer == 0x7f800000ULL, "plane: buffer");
	check(plane.pitch == 7680U && plane.width == 1920U && plane.height == 1080U, "plane: extent");
	check(mali_osd_shows(&plane, 0x7f800000ULL, 1920U, 1080U, 7680U) == 1, "plane: shows the framebuffer");
	check(mali_osd_shows(&plane, 0x7f800000ULL, 1280U, 720U, 7680U) == 0, "plane: other extent");
	check(mali_osd_shows(&plane, 0x7e000000ULL, 1920U, 1080U, 7680U) == 0, "plane: other buffer");

	/* Off, on a canvas, and a window away from the corner. */
	plane.control = MALI_OSD_CONTROL_BLOCK | MALI_OSD_CONTROL_LINEAR;
	check(mali_osd_decode(&plane) == ENODEV, "plane: off");
	plane.control = MALI_OSD_CONTROL_ENABLE | MALI_OSD_CONTROL_BLOCK;
	check(mali_osd_decode(&plane) == ENOTSUP, "plane: canvas");
	plane.control = MALI_OSD_CONTROL_ENABLE | MALI_OSD_CONTROL_BLOCK | MALI_OSD_CONTROL_LINEAR;
	plane.window_x = 8U | (1927U << MALI_OSD_WINDOW_LAST_SHIFT);
	check(mali_osd_decode(&plane) == ENOTSUP, "plane: window off the corner");

	/* The address word of a flip. */
	error = mali_osd_address_word(0x40001000ULL, &word);
	check(error == 0 && word == 0x04000100U, "plane: address word");
	check(mali_osd_address_word(0x40001008ULL, &word) == EINVAL, "plane: unaligned address");
	check(mali_osd_address_word(0x1000000000ULL, &word) == ERANGE, "plane: address too high");
}
