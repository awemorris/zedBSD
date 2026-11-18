/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The host test of the xd display buffer and watermarks
 * (display/xd-dbuf.c) on the register model (xd-fake-regs.c): the 5330's
 * panel with the latencies its firmware reports (the watermark words and
 * the buffer range of the driver's run), the slices and joining of sets
 * of pipes on Alder Lake-P and Tiger Lake, the shares of two pipes, the
 * cursor's part, the levels the vertical blank or the allocation cut,
 * the slices' power, the bus's joining and the data box credits.
 */

#include "host-test.h"
#include "xd-fake-regs.h"

#include "../../display/xd-dbuf.h"
#include "../../display/xd-io.h"
#include "../../display/xd-platform.h"
#include "../../intel/display-regs-xd.h"

#include <uapi/errno.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* The devices the checks describe: the 5330's Alder Lake-P and a Tiger Lake. */
#define I915_XD_DBUF_TEST_ADLP		0x46a6U
#define I915_XD_DBUF_TEST_ADLP_REVID	0x0cU
#define I915_XD_DBUF_TEST_TGL		0x9a49U
#define I915_XD_DBUF_TEST_TGL_REVID	0x01U

/* The window the checks write through. */
static struct i915_xd_io i915_xd_dbuf_test_io;

/*
 * The memory firmware of the model: the two latency words it answers,
 * the SAGV block time, a refusal of the latencies, and the commands it
 * answered.
 */
struct i915_xd_dbuf_test_firmware {
	uint32_t words[2];
	uint32_t block_us;
	int refuse_latency;
	unsigned commands;
};

/* The memory firmware the mailbox rule answers from. */
static struct i915_xd_dbuf_test_firmware i915_xd_dbuf_test_memory_firmware;

static const struct i915_xd_platform *i915_xd_dbuf_test_setup(uint16_t device, uint8_t revid);
static void i915_xd_dbuf_test_pipe(struct i915_xd_wm_pipe *pipe);
static void i915_xd_dbuf_test_panel(void);
static void i915_xd_dbuf_test_slices(void);
static void i915_xd_dbuf_test_cuts(void);
static void i915_xd_dbuf_test_writes(void);
static void i915_xd_dbuf_test_memory(void);
static void i915_xd_dbuf_test_firmware(uint32_t offset, uint32_t value, void *context);

/*
 * Runs every check of the display buffer and watermarks and reports.
 */
int
main(void)
{
	int failures;

	/* Runs each group of checks. */
	i915_xd_dbuf_test_panel();
	i915_xd_dbuf_test_slices();
	i915_xd_dbuf_test_cuts();
	i915_xd_dbuf_test_writes();
	i915_xd_dbuf_test_memory();

	/* Reports the result. */
	failures = i915_host_report("host_xd_dbuf_test");
	if (failures != 0)
		return 1;

	/* Succeeded: every check passed. */
	return 0;
}

/* Starts a fresh model and a window on it; returns the device's description. */
static const struct i915_xd_platform *
i915_xd_dbuf_test_setup(
	uint16_t device,
	uint8_t revid)
{
	struct i915_xd_io_hooks hooks;
	struct i915_xd_choice choice;
	int error;

	/* A fresh model and a window that reaches it. */
	i915_xd_fake_reset();
	i915_xd_fake_hooks(&hooks);
	drv_i915_xd_io_init(&i915_xd_dbuf_test_io, NULL, NULL);
	drv_i915_xd_io_set_hooks(&i915_xd_dbuf_test_io, &hooks);

	/* The description. */
	error = drv_i915_xd_platform_choose(device, revid, 0, 0U, &choice);
	if (error != 0)
		return NULL;

	/* Succeeded: the description. */
	return choice.platform;
}

/*
 * The 5330's pipe A: six levels with the latencies its firmware reports
 * (3, 54, 83, 102, 147, 147 us), IPC, a SAGV block time of 35 us, the
 * panel's 140.8 MHz and line total of 2080.
 */
static void
i915_xd_dbuf_test_pipe(
	struct i915_xd_wm_pipe *pipe)
{
	static const uint16_t latency[6] = { 3U, 54U, 83U, 102U, 147U, 147U };

	/* The pipe. */
	memset(pipe, 0, sizeof(*pipe));
	pipe->levels = 6U;
	memcpy(pipe->latency_us, latency, sizeof(latency));
	pipe->ipc = 1;
	pipe->sagv_block_us = 35U;
	pipe->pixel_rate_khz = 140800U;
	pipe->htotal = 2080U;
}

/*
 * The 5330's panel alone on pipe A: the bus joined to all four slices,
 * the whole buffer the pipe's, the cursor's 32 blocks at the end, and the
 * plane's watermark words and range of the driver's run.
 */
static void
i915_xd_dbuf_test_panel(void)
{
	static const uint32_t words[6] = {
		0x80004010U, 0x80010041U, 0x80018061U, 0x8001c071U, 0x800280a1U, 0x800280a1U
	};
	const struct i915_xd_platform *platform;
	struct i915_xd_wm_pipe pipe;
	struct i915_xd_wm_surface surface;
	struct i915_xd_plane_wm wm;
	struct i915_xd_ddb range;
	struct i915_xd_ddb plane_ddb;
	struct i915_xd_ddb cursor_ddb;
	uint16_t cursor;
	unsigned deepest;
	unsigned level;
	int sagv_too_short;
	int joined;
	int error;
	int same;
	uint32_t word;

	/* Alder Lake-P. */
	platform = i915_xd_dbuf_test_setup(I915_XD_DBUF_TEST_ADLP, I915_XD_DBUF_TEST_ADLP_REVID);
	if (platform == NULL) {
		i915_host_check(0, "dbuf: the Alder Lake-P description");
		return;
	}

	/* Pipe A alone joins the bus and takes all four slices, the whole 4096 blocks. */
	joined = drv_i915_xd_dbuf_joined(platform, 0x1U);
	i915_host_check(joined == 1 && drv_i915_xd_dbuf_slices(platform, 0U, 0x1U, 1) == 0xfU, "dbuf: pipe A alone joins the bus to four slices");
	drv_i915_xd_dbuf_pipe_range(platform, 0xfU, 0U, 1920U, 1920U, &range);
	i915_host_check(range.start == 0U && range.end == 4096U, "dbuf: the lone pipe's share is the whole buffer");

	/* The plane's watermarks: a linear 1920-pixel XRGB8888 surface. */
	i915_xd_dbuf_test_pipe(&pipe);
	surface.width = 1920U;
	surface.cpp = 4U;
	surface.layout = I915_XD_WM_LINEAR;
	surface.pixel_rate_khz = 140800U;
	surface.minimal = 0;
	drv_i915_xd_wm_plane(platform, &pipe, &surface, &wm);

	/* Every level fits the 48-line blank (one line of frame start delay). */
	error = drv_i915_xd_wm_vblank_level(&pipe, 140800U, 48U, 1U, wm.level[0].lines, &deepest, &sagv_too_short);
	i915_host_check(error == 0 && deepest == 5U && sagv_too_short == 0, "dbuf: every level and the SAGV fit the blank");
	drv_i915_xd_wm_cut(&wm, pipe.levels, deepest, sagv_too_short);

	/* The cursor's 36 blocks (its deepest level's) and the plane's range [0, 4060). */
	cursor = drv_i915_xd_wm_cursor_blocks(platform, &pipe, 1U);
	error = drv_i915_xd_wm_allocate(&pipe, &range, cursor, &wm, 1920U * 1080U * 4U, &plane_ddb, &cursor_ddb);
	i915_host_check(error == 0 && cursor == 36U && plane_ddb.start == 0U && plane_ddb.end == 4060U &&
			cursor_ddb.start == 4060U && cursor_ddb.end == 4096U,
			"dbuf: the cursor's 36 blocks at the end, the plane's range up to them");

	/* The words of the driver's run. */
	drv_i915_xd_wm_write_plane(&i915_xd_dbuf_test_io, platform, 0U, pipe.levels, &wm, 0, &plane_ddb);
	same = 1;
	for (level = 0U; level < 6U; level++) {
		/* The level's word as the model holds it. */
		word = i915_xd_fake_get(0x70240U + 4U * level);
		if (word != words[level])
			same = 0;
	}

	/* Checks the six level words of the driver's run (0x80004010 ... 0x800280a1). */
	i915_host_check(same, "dbuf: the six level words of the driver's run (0x80004010 ... 0x800280a1)");
	i915_host_check(i915_xd_fake_get(0x70268U) == 0x8000001eU && i915_xd_fake_get(0x70258U) == 0x8000c031U &&
			i915_xd_fake_get(0x7025cU) == 0x8000003fU && i915_xd_fake_get(0x7027cU) == 0x0fdb0000U,
			"dbuf: the transition 0x8000001e, the SAGV pair 0x8000c031/0x8000003f, the range 0x0fdb0000");
}

/*
 * The slices and joining of sets of pipes, and the shares of two pipes on
 * Alder Lake-P (each on its own half) and on Tiger Lake (two slices).
 */
static void
i915_xd_dbuf_test_slices(void)
{
	const struct i915_xd_platform *platform;
	const struct i915_xd_platform *tgl;
	struct i915_xd_ddb range;

	/* Alder Lake-P: A and B do not join; A takes slices 1-2, B 3-4. */
	platform = i915_xd_dbuf_test_setup(I915_XD_DBUF_TEST_ADLP, I915_XD_DBUF_TEST_ADLP_REVID);
	if (platform == NULL) {
		i915_host_check(0, "dbuf: the Alder Lake-P description");
		return;
	}

	/* Checks pipes A and B split the bus, A on slices 1-2 and B on 3-4. */
	i915_host_check(drv_i915_xd_dbuf_joined(platform, 0x3U) == 0U &&
			drv_i915_xd_dbuf_slices(platform, 0U, 0x3U, 0) == 0x3U && drv_i915_xd_dbuf_slices(platform, 1U, 0x3U, 0) == 0xcU,
			"dbuf: pipes A and B split the bus, A on slices 1-2 and B on 3-4");

	/* B's share counts from the start of slice 3. */
	drv_i915_xd_dbuf_pipe_range(platform, 0xcU, 0U, 1920U, 1920U, &range);
	i915_host_check(range.start == 0U && range.end == 2048U && drv_i915_xd_dbuf_mbus_offset(platform, 0xcU) == 2048U,
			"dbuf: B's share of slices 3-4 counts from slice 3");

	/* Two pipes on the same slices share by width. */
	drv_i915_xd_dbuf_pipe_range(platform, 0x3U, 1920U, 3200U, 3200U, &range);
	i915_host_check(range.start == 1228U && range.end == 2048U, "dbuf: the second of two pipes on a half shares by width");

	/* Tiger Lake: never joined; A alone takes both slices, A and B one each. */
	tgl = i915_xd_dbuf_test_setup(I915_XD_DBUF_TEST_TGL, I915_XD_DBUF_TEST_TGL_REVID);
	if (tgl == NULL) {
		i915_host_check(0, "dbuf: the Tiger Lake description");
		return;
	}

	/* Checks Tiger Lake's slices (A alone both, A and B one each), never joined. */
	i915_host_check(drv_i915_xd_dbuf_joined(tgl, 0x1U) == 0U && drv_i915_xd_dbuf_slices(tgl, 0U, 0x1U, 0) == 0x3U &&
			drv_i915_xd_dbuf_slices(tgl, 0U, 0x3U, 0) == 0x2U && drv_i915_xd_dbuf_slices(tgl, 1U, 0x3U, 0) == 0x1U,
			"dbuf: Tiger Lake's slices (A alone both, A and B one each), never joined");
	drv_i915_xd_dbuf_slice_range(tgl, 0x2U, &range);
	i915_host_check(range.start == 1024U && range.end == 2048U, "dbuf: Tiger Lake's slice 2 spans [1024, 2048)");
}

/*
 * The levels the blank and the allocation cut: a short blank allows only
 * level 0 and no SAGV; a small share holds only the shallow levels, the
 * deeper ones turned off with the blocks of the one below; a share too
 * small for level 0 is refused.
 */
static void
i915_xd_dbuf_test_cuts(void)
{
	const struct i915_xd_platform *platform;
	struct i915_xd_wm_pipe pipe;
	struct i915_xd_wm_surface surface;
	struct i915_xd_plane_wm wm;
	struct i915_xd_ddb range;
	struct i915_xd_ddb plane_ddb;
	struct i915_xd_ddb cursor_ddb;
	unsigned deepest;
	int sagv_too_short;
	int error;

	/* Alder Lake-P, the panel's plane. */
	platform = i915_xd_dbuf_test_setup(I915_XD_DBUF_TEST_ADLP, I915_XD_DBUF_TEST_ADLP_REVID);
	if (platform == NULL) {
		i915_host_check(0, "dbuf: the Alder Lake-P description");
		return;
	}

	/* The 5330's pipe and plane. */
	i915_xd_dbuf_test_pipe(&pipe);
	surface.width = 1920U;
	surface.cpp = 4U;
	surface.layout = I915_XD_WM_LINEAR;
	surface.pixel_rate_khz = 140800U;
	surface.minimal = 0;
	drv_i915_xd_wm_plane(platform, &pipe, &surface, &wm);

	/* A 3-line blank: level 0 only (it counts no latency), and no SAGV. */
	error = drv_i915_xd_wm_vblank_level(&pipe, 140800U, 3U, 1U, wm.level[0].lines, &deepest, &sagv_too_short);
	i915_host_check(error == 0 && deepest == 0U && sagv_too_short == 1, "dbuf: a short blank allows level 0 only and no SAGV");
	drv_i915_xd_wm_cut(&wm, pipe.levels, deepest, sagv_too_short);
	i915_host_check(wm.level[1].enable == 0U && wm.level[1].blocks != 0U && wm.sagv.enable == 0U,
			"dbuf: the cut levels keep their blocks; the SAGV level goes off");

	/* A share of 132 blocks (100 after the cursor): levels 0 and 1 hold, the rest take level 1's blocks. */
	drv_i915_xd_wm_plane(platform, &pipe, &surface, &wm);
	range.start = 0U;
	range.end = 132U;
	error = drv_i915_xd_wm_allocate(&pipe, &range, 32U, &wm, 1U, &plane_ddb, &cursor_ddb);
	i915_host_check(error == 0 && plane_ddb.end == 100U && wm.level[1].enable == 1U && wm.level[2].enable == 0U &&
			wm.level[2].blocks == wm.level[1].blocks && wm.level[5].lines == wm.level[1].lines,
			"dbuf: a small share keeps levels 0 and 1; deeper levels take level 1's blocks and lines");

	/* A share too small for level 0 (fresh watermarks). */
	drv_i915_xd_wm_plane(platform, &pipe, &surface, &wm);
	range.end = 40U;
	error = drv_i915_xd_wm_allocate(&pipe, &range, 32U, &wm, 1U, &plane_ddb, &cursor_ddb);
	i915_host_check(error == EINVAL, "dbuf: a share too small for level 0 is refused");
}

/*
 * The writes: the slices' power with a slice that does not follow, the
 * bus's joining and tracker service, and the data box credits of Alder
 * Lake-P (joined and split) and Tiger Lake.
 */
static void
i915_xd_dbuf_test_writes(void)
{
	const struct i915_xd_platform *platform;
	const struct i915_xd_platform *tgl;
	uint8_t late;

	/* Alder Lake-P: slices 1 and 2 on, 3 and 4 off; slice 2 does not follow. */
	platform = i915_xd_dbuf_test_setup(I915_XD_DBUF_TEST_ADLP, I915_XD_DBUF_TEST_ADLP_REVID);
	if (platform == NULL) {
		i915_host_check(0, "dbuf: the Alder Lake-P description");
		return;
	}

	/* The slices' states follow their requests; slice 3 is on from the firmware. */
	i915_xd_fake_rule_mirror(0x45008U, 0x80000000U, 0x45008U, -1, 0U);
	i915_xd_fake_set(0x44300U, 0xc0000000U);
	i915_xd_fake_rule_mirror(0x44300U, 0x80000000U, 0x44300U, -1, 0U);
	late = drv_i915_xd_dbuf_power(&i915_xd_dbuf_test_io, platform, 0x3U);
	i915_host_check(late == 0x2U && (i915_xd_fake_get(0x45008U) & 0xc0000000U) == 0xc0000000U &&
			(i915_xd_fake_get(0x44fe8U) & 0x80000000U) != 0U && (i915_xd_fake_get(0x44300U) & 0xc0000000U) == 0U,
			"dbuf: the slices' power follows the request; a slice that does not is reported");

	/* The bus joined: 1x4 hashing, join, no pipe; tracker service 3 (ratio 4) on every slice. */
	drv_i915_xd_dbuf_mbus(&i915_xd_dbuf_test_io, platform, 1);
	i915_host_check(i915_xd_fake_get(0x4438cU) == 0xdc000000U && (i915_xd_fake_get(0x44304U) & 0x00070000U) == 0x00030000U,
			"dbuf: the bus joined (0xdc000000) and the tracker service of a ratio of 4");

	/* Split: 2x2 hashing, no join; tracker service 1. */
	drv_i915_xd_dbuf_mbus(&i915_xd_dbuf_test_io, platform, 0);
	i915_host_check(i915_xd_fake_get(0x4438cU) == 0x1c000000U && (i915_xd_fake_get(0x45008U) & 0x00070000U) == 0x00010000U,
			"dbuf: the bus split and the tracker service of a ratio of 2");

	/* The data box credits. */
	i915_host_check(drv_i915_xd_dbuf_dbox_word(platform, 1) == 0x01038806U && drv_i915_xd_dbuf_dbox_word(platform, 0) == 0x01038804U,
			"dbuf: Alder Lake-P's data box credits, joined 0x01038806 and split 0x01038804");
	tgl = i915_xd_dbuf_test_setup(I915_XD_DBUF_TEST_TGL, I915_XD_DBUF_TEST_TGL_REVID);
	i915_host_check(tgl != NULL && drv_i915_xd_dbuf_dbox_word(tgl, 0) == 0x01038c02U, "dbuf: Tiger Lake's data box credits 0x01038c02");

	/* Tiger Lake powers its two slices only; the bus is not touched. */
	if (tgl != NULL) {
		late = drv_i915_xd_dbuf_power(&i915_xd_dbuf_test_io, tgl, 0x1U);
		drv_i915_xd_dbuf_mbus(&i915_xd_dbuf_test_io, tgl, 1);
		i915_host_check(i915_xd_fake_get(0x44300U) == 0U && i915_xd_fake_get(0x4438cU) == 0U,
				"dbuf: Tiger Lake writes neither slices 3-4 nor the bus control");
	}
}

/* Answers a mailbox command: the latency word asked for, the SAGV block time, or a refusal. */
static void
i915_xd_dbuf_test_firmware(
	uint32_t offset,
	uint32_t value,
	void *context)
{
	struct i915_xd_dbuf_test_firmware *firmware;
	uint32_t data;
	uint32_t status;

	UNUSED_PARAMETER(offset);

	/* Only a command handed over is answered. */
	firmware = context;
	if ((value & I915_XD_GTDRIVER_MAILBOX_BUSY) == 0U)
		return;

	/* The latency words, by the word asked for, and the block time. */
	firmware->commands++;
	status = I915_XD_GTDRIVER_STATUS_OK;
	data = i915_xd_fake_get(I915_XD_GTDRIVER_DATA0);
	if ((value & 0xffU) == I915_XD_MAILBOX_READ_LATENCY) {
		i915_xd_fake_set(I915_XD_GTDRIVER_DATA0, firmware->words[data & 1U]);
		if (firmware->refuse_latency)
			status = I915_XD_GTDRIVER_STATUS_REJECTED;
	} else if ((value & 0xffU) == I915_XD_MAILBOX_SAGV_BLOCK_TIME) {
		i915_xd_fake_set(I915_XD_GTDRIVER_DATA0, firmware->block_us);
	}

	/* The answer: no busy bit, the status. */
	i915_xd_fake_set(I915_XD_GTDRIVER_MAILBOX, status);
}

/*
 * The memory's report: the 5330's firmware words (level 0 reads 0 us, so
 * 3 us go to each of its six levels; levels 6 and 7 are kept as read),
 * its SAGV block time, a level that reads 0 us cutting the deeper ones,
 * an SAGV the display does not control, a refused mailbox.
 */
static void
i915_xd_dbuf_test_memory(void)
{
	static const uint16_t expect[8] = { 3, 54, 83, 102, 147, 147, 144, 144 };
	const struct i915_xd_platform *platform;
	struct i915_xd_dbuf_test_firmware *firmware;
	struct i915_xd_memory memory;
	unsigned index;
	int same;
	int error;

	/* The 5330's firmware. */
	platform = i915_xd_dbuf_test_setup(I915_XD_DBUF_TEST_ADLP, I915_XD_DBUF_TEST_ADLP_REVID);
	firmware = &i915_xd_dbuf_test_memory_firmware;
	memset(firmware, 0, sizeof(*firmware));
	firmware->words[0] = 0U | (51U << 8) | (80U << 16) | (99U << 24);
	firmware->words[1] = 144U | (144U << 8) | (144U << 16) | (144U << 24);
	firmware->block_us = 35U;
	i915_xd_fake_rule_call(I915_XD_GTDRIVER_MAILBOX, i915_xd_dbuf_test_firmware, firmware);
	error = drv_i915_xd_memory_read(&i915_xd_dbuf_test_io, platform, 1, 0, &memory);
	same = 1;
	for (index = 0U; index < 8U; index++) {
		if (memory.latency[index] != expect[index])
			same = 0;
	}

	/* Checks the 5330's latencies, 3 us added to its six levels. */
	i915_host_check(error == 0 && memory.valid && memory.levels == 6U && same, "memory: the 5330's latencies, 3 us added to its six levels");
	i915_host_check(memory.sagv_block_us == 35U && memory.sagv_error == 0 && firmware->commands == 3U, "memory: the SAGV block time, then the two latency words");

	/* A level that reads 0 us turns off the deeper levels of the display. */
	firmware->words[0] = 2U | (51U << 8) | (0U << 16) | (99U << 24);
	error = drv_i915_xd_memory_read(&i915_xd_dbuf_test_io, platform, 1, 1, &memory);
	i915_host_check(error == 0 && memory.latency[0] == 3U && memory.latency[1] == 51U && memory.latency[2] == 0U &&
	    memory.latency[5] == 0U && memory.latency[6] == 144U && memory.levels == 6U,
	    "memory: a level of 0 us turns the deeper ones off, the DIMM adjustment adds 1 us to level 0");

	/* An SAGV the display does not control blocks nothing; a refused mailbox leaves the latencies unread. */
	error = drv_i915_xd_memory_read(&i915_xd_dbuf_test_io, platform, 0, 0, &memory);
	i915_host_check(error == 0 && memory.sagv_block_us == 0U, "memory: an SAGV not controlled blocks nothing");
	firmware->refuse_latency = 1;
	error = drv_i915_xd_memory_read(&i915_xd_dbuf_test_io, platform, 1, 0, &memory);
	i915_host_check(error != 0 && !memory.valid && memory.sagv_block_us == 35U, "memory: refused latencies are not valid, the block time is still read");
}
