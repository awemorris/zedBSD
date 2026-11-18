/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The host test of the regression evidence: the survey of the first picture
 * (display/survey.c) on the register model, and the start's record of the
 * display writes (mmio-record.c).
 */

#include "host-test.h"
#include "xd-fake-regs.h"

#include "../../display/survey.h"
#include "../../display/xd-io.h"
#include "../../intel/display-regs-xd.h"
#include "../../mmio.h"
#include "../../mmio-record.h"

#include <uapi/errno.h>
#include <stddef.h>
#include <stdint.h>

/* The surfaces of the two made-up resident buffers. */
#define I915_SURVEY_TEST_BUFFER_A	0x00100000U
#define I915_SURVEY_TEST_BUFFER_B	0x00900000U

/* The pipe A and pipe B register blocks of the plane and the pipe. */
#define I915_SURVEY_TEST_SURF_A		0x7019cU
#define I915_SURVEY_TEST_SURF_B		0x7119cU
#define I915_SURVEY_TEST_SURFLIVE_A	0x701acU
#define I915_SURVEY_TEST_FRMCNT_A	0x70040U
#define I915_SURVEY_TEST_CRC_CTL_A	0x60050U
#define I915_SURVEY_TEST_CRC_RES_A	0x60064U

/* The index of a few registers in the survey's table. */
#define I915_SURVEY_TEST_INDEX_DBUF_S3	10U
#define I915_SURVEY_TEST_INDEX_TRANS	17U
#define I915_SURVEY_TEST_INDEX_LINETIME	28U

static void i915_survey_test_setup(struct i915_xd_io *io);
static void i915_survey_test_scene(struct i915_survey_scene *scene, int display_ver);
static void i915_survey_test_lit_panel(void);
static void i915_survey_test_table(void);
static void i915_survey_test_read(void);
static void i915_survey_test_pipe_choice(void);
static void i915_survey_test_crc(void);
static void i915_survey_test_judge(void);
static void i915_survey_test_record_kinds(void);
static void i915_survey_test_record_runs(void);
static void i915_survey_test_record_limits(void);
static void i915_survey_test_record_digests(void);

/*
 * Runs every check of the survey and the record of the writes.
 */
int
main(void)
{
	int failures;

	/* Runs each group of checks on a fresh model. */
	i915_survey_test_table();
	i915_survey_test_read();
	i915_survey_test_pipe_choice();
	i915_survey_test_crc();
	i915_survey_test_judge();
	i915_survey_test_record_kinds();
	i915_survey_test_record_runs();
	i915_survey_test_record_limits();
	i915_survey_test_record_digests();

	/* Reports the result. */
	failures = i915_host_report("host_xd_survey_test");
	if (failures != 0)
		return 1;

	/* Succeeded: every check passed. */
	return 0;
}

/* Prepares a window that reaches a fresh register model. */
static void
i915_survey_test_setup(
	struct i915_xd_io *io)
{
	struct i915_xd_io_hooks hooks;

	/* Empties the model and points the window at it. */
	i915_xd_fake_reset();
	drv_i915_xd_io_init(io, NULL, NULL);
	i915_xd_fake_hooks(&hooks);
	drv_i915_xd_io_set_hooks(io, &hooks);
}

/* Fills a scene of the made-up resident buffers and no underrun. */
static void
i915_survey_test_scene(
	struct i915_survey_scene *scene,
	int display_ver)
{
	/* The display version and the two buffers. */
	scene->display_ver = display_ver;
	scene->buffer_surf[0] = I915_SURVEY_TEST_BUFFER_A;
	scene->buffer_surf[1] = I915_SURVEY_TEST_BUFFER_B;
	scene->underruns = 0U;
}

/*
 * Sets the model to a lit eDP panel on pipe A of display 13, as the
 * Latitude 5330 shows it after its first picture.
 */
static void
i915_survey_test_lit_panel(void)
{
	/* Power well 1 and the pipe A well are on. */
	i915_xd_fake_set(I915_XD_PWR_WELL_CTL, 0x00000c03U);

	/* The panel is on and the backlight runs at full duty. */
	i915_xd_fake_set(0xc7200U, 0x80000008U);
	i915_xd_fake_set(0xc8250U, 0x80000000U);
	i915_xd_fake_set(0xc8254U, 0x00017700U);
	i915_xd_fake_set(0xc8258U, 0x00017700U);

	/* The transcoder runs and the plane scans out buffer B. */
	i915_xd_fake_set(0x70008U, 0xc0000000U);
	i915_xd_fake_set(I915_SURVEY_TEST_SURF_A, I915_SURVEY_TEST_BUFFER_B);
	i915_xd_fake_set(I915_SURVEY_TEST_SURFLIVE_A, I915_SURVEY_TEST_BUFFER_B);
	i915_xd_fake_set(I915_SURVEY_TEST_FRMCNT_A, 100U);
}

/* Checks the names and addresses of the survey's registers. */
static void
i915_survey_test_table(void)
{
	const char *name;
	unsigned index;
	int named;

	/* Every register has a name. */
	named = 1;
	for (index = 0U; index < I915_SURVEY_REGISTERS; index++) {
		name = drv_i915_survey_register_name(index);
		if (name == NULL)
			named = 0;
	}

	/* Checks every register has a name. */
	i915_host_check(named, "table: every register has a name");
	i915_host_check(drv_i915_survey_register_name(I915_SURVEY_REGISTERS) == NULL, "table: no name past the table");

	/* A pipe register moves with the pipe, a register of the whole display does not. */
	i915_host_check(drv_i915_survey_register_offset(I915_SURVEY_TEST_INDEX_TRANS, 0U) == 0x70008U, "table: TRANS_CONF of pipe A");
	i915_host_check(drv_i915_survey_register_offset(I915_SURVEY_TEST_INDEX_TRANS, 1U) == 0x71008U, "table: TRANS_CONF of pipe B");
	i915_host_check(drv_i915_survey_register_offset(I915_SURVEY_TEST_INDEX_LINETIME, 1U) == 0x45274U, "table: WM_LINETIME of pipe B");
	i915_host_check(drv_i915_survey_register_offset(0U, 3U) == I915_XD_PWR_WELL_CTL, "table: PWR_WELL_CTL is one for the display");
	i915_host_check(drv_i915_survey_register_offset(24U, 0U) == I915_SURVEY_TEST_CRC_CTL_A, "table: PIPE_CRC_CTL of pipe A");
	i915_host_check(drv_i915_survey_register_offset(25U, 1U) == 0x61064U, "table: PIPE_CRC_RES_1 of pipe B");
}

/* Checks a survey of a lit panel and of a pipe whose well is off. */
static void
i915_survey_test_read(void)
{
	struct i915_xd_io io;
	struct i915_survey_scene scene;
	struct i915_survey_reading reading;

	/* A lit panel on display 13: pipe A shows buffer B, every register is read. */
	i915_survey_test_setup(&io);
	i915_survey_test_lit_panel();
	i915_survey_test_scene(&scene, 13);
	drv_i915_survey_read(&io, &scene, &reading);
	i915_host_check(reading.pipe == 0U && reading.pipe_found && reading.pipe_powered, "read: pipe A found and powered");
	i915_host_check(reading.live_buffer == 1U, "read: the live surface is buffer B");
	i915_host_check(reading.read[I915_SURVEY_TEST_INDEX_DBUF_S3], "read: display 13 reads the third DBUF slice");
	i915_host_check(reading.value[I915_SURVEY_TEST_INDEX_TRANS] == 0xc0000000U, "read: TRANS_CONF value");
	i915_host_check(i915_xd_fake_writes() == 0U, "read: a survey writes nothing");

	/* Display 12 has no third slice, and pipe A is under power well 1 there. */
	i915_survey_test_scene(&scene, 12);
	i915_xd_fake_set(I915_XD_PWR_WELL_CTL, 0x00000003U);
	drv_i915_survey_read(&io, &scene, &reading);
	i915_host_check(!reading.read[I915_SURVEY_TEST_INDEX_DBUF_S3], "read: display 12 skips the third DBUF slice");
	i915_host_check(reading.pipe_found && reading.pipe_powered, "read: display 12 pipe A under power well 1");

	/* A pipe whose well is off is neither chosen nor read. */
	i915_survey_test_scene(&scene, 13);
	i915_xd_fake_set(I915_XD_PWR_WELL_CTL, 0x00000003U);
	drv_i915_survey_read(&io, &scene, &reading);
	i915_host_check(!reading.pipe_found && !reading.pipe_powered, "read: pipe A well off");
	i915_host_check(!reading.read[I915_SURVEY_TEST_INDEX_TRANS], "read: no pipe register read with the well off");
	i915_host_check(reading.read[0], "read: the display registers are still read");
	i915_host_check(reading.live_buffer == I915_SURVEY_BUFFER_NONE, "read: no live surface without the pipe");
}

/* Checks that the pipe showing a resident buffer is the one surveyed. */
static void
i915_survey_test_pipe_choice(void)
{
	struct i915_xd_io io;
	struct i915_survey_scene scene;
	struct i915_survey_reading reading;

	/* Pipe A shows another surface, pipe B shows buffer A: pipe B is surveyed. */
	i915_survey_test_setup(&io);
	i915_survey_test_lit_panel();
	i915_xd_fake_set(I915_XD_PWR_WELL_CTL, 0x00003c03U);
	i915_xd_fake_set(I915_SURVEY_TEST_SURF_A, 0x00500000U);
	i915_xd_fake_set(I915_SURVEY_TEST_SURF_B, I915_SURVEY_TEST_BUFFER_A | 0x8U);
	i915_survey_test_scene(&scene, 13);
	drv_i915_survey_read(&io, &scene, &reading);
	i915_host_check(reading.pipe == 1U && reading.pipe_found, "pipe: pipe B shows buffer A (control bits ignored)");

	/* No pipe shows a resident buffer: pipe A, marked as not found. */
	i915_xd_fake_set(I915_SURVEY_TEST_SURF_B, 0x00700000U);
	drv_i915_survey_read(&io, &scene, &reading);
	i915_host_check(reading.pipe == 0U && !reading.pipe_found, "pipe: none found falls back to pipe A");
}

/* Checks the CRC's start, its two reads a frame apart, and its stop. */
static void
i915_survey_test_crc(void)
{
	struct i915_xd_io io;
	uint32_t crc[2];
	int error;

	/* The start enables the CRC on plane 1 of the pipe. */
	i915_survey_test_setup(&io);
	drv_i915_survey_crc_start(&io, 0U);
	i915_host_check(i915_xd_fake_get(I915_SURVEY_TEST_CRC_CTL_A) == 0x80000000U, "crc: enabled on plane 1");
	drv_i915_survey_crc_start(&io, 1U);
	i915_host_check(i915_xd_fake_get(0x61050U) == 0x80000000U, "crc: pipe B's control");

	/* A pipe whose frame moves after 4 ms gives two CRCs. */
	i915_xd_fake_set(I915_SURVEY_TEST_FRMCNT_A, 500U);
	i915_xd_fake_set(I915_SURVEY_TEST_CRC_RES_A, 0x1234abcdU);
	i915_xd_fake_later(4000U, I915_SURVEY_TEST_FRMCNT_A, 0xffffffffU, 501U);
	error = drv_i915_survey_crc_pair(&io, 0U, crc);
	i915_host_check(error == 0, "crc: a moving pipe gives a pair");
	i915_host_check(crc[0] == 0x1234abcdU && crc[1] == 0x1234abcdU, "crc: both values read");

	/* A pipe whose frame never moves gives no second CRC, after 50 ms at most. */
	error = drv_i915_survey_crc_pair(&io, 0U, crc);
	i915_host_check(error == EAGAIN && crc[1] == 0U, "crc: a still pipe gives no pair");

	/* The stop clears the control. */
	drv_i915_survey_crc_stop(&io, 0U);
	i915_host_check(i915_xd_fake_get(I915_SURVEY_TEST_CRC_CTL_A) == 0U, "crc: stopped");
}

/* Checks the judgement of a lit panel and of each missing piece of evidence. */
static void
i915_survey_test_judge(void)
{
	struct i915_xd_io io;
	struct i915_survey_scene scene;
	struct i915_survey_reading first;
	struct i915_survey_reading second;
	struct i915_survey_verdict verdict;
	uint32_t crc[2];

	/* Two surveys 60 frames apart of a lit panel, with two nonzero CRCs. */
	i915_survey_test_setup(&io);
	i915_survey_test_lit_panel();
	i915_survey_test_scene(&scene, 13);
	drv_i915_survey_read(&io, &scene, &first);
	i915_xd_fake_set(I915_SURVEY_TEST_FRMCNT_A, 160U);
	drv_i915_survey_read(&io, &scene, &second);
	crc[0] = 0x0badf00dU;
	crc[1] = 0x0badf00dU;
	drv_i915_survey_judge(&first, &second, crc, 1, &verdict);
	i915_host_check(verdict.panel_on && verdict.backlight_on && verdict.transcoder_on, "judge: panel, backlight and transcoder on");
	i915_host_check(verdict.frames == 60U && verdict.frames_advanced, "judge: 60 frames between the surveys");
	i915_host_check(verdict.live_is_resident && verdict.crc_nonzero && verdict.no_underruns, "judge: resident surface, CRCs, no underrun");
	drv_i915_survey_log_verdict(&verdict);

	/* Two frames are too few. */
	i915_xd_fake_set(I915_SURVEY_TEST_FRMCNT_A, 102U);
	drv_i915_survey_read(&io, &scene, &second);
	drv_i915_survey_judge(&first, &second, crc, 1, &verdict);
	i915_host_check(!verdict.frames_advanced, "judge: two frames are too few");

	/* A zero CRC, an unread CRC and an underrun each count against the panel. */
	crc[1] = 0U;
	drv_i915_survey_judge(&first, &second, crc, 1, &verdict);
	i915_host_check(!verdict.crc_nonzero, "judge: a zero CRC");
	crc[1] = 1U;
	drv_i915_survey_judge(&first, &second, crc, 0, &verdict);
	i915_host_check(!verdict.crc_nonzero, "judge: an unread CRC");
	second.underruns = 1U;
	drv_i915_survey_judge(&first, &second, crc, 1, &verdict);
	i915_host_check(!verdict.no_underruns, "judge: an underrun");

	/* A backlight with zero duty, and a panel that is off. */
	i915_xd_fake_set(0xc8258U, 0U);
	i915_xd_fake_set(0xc7200U, 0U);
	drv_i915_survey_read(&io, &scene, &second);
	drv_i915_survey_judge(&first, &second, crc, 1, &verdict);
	i915_host_check(!verdict.backlight_on && !verdict.panel_on, "judge: zero duty and the panel off");
	drv_i915_survey_log_verdict(&verdict);
}

/* Checks which writes the record keeps, counts or ignores, and whose. */
static void
i915_survey_test_record_kinds(void)
{
	struct i915_mmio gen12;
	struct i915_mmio other_gen12;
	struct i915_mmio gen9;
	struct i915_mmio_record_entry entries[8];
	struct i915_mmio_record_digest digest;
	unsigned count;

	/* Three register blocks: two Gen12 devices and a Gen9 one. */
	gen12.graphics_ver = 12;
	other_gen12.graphics_ver = 12;
	gen9.graphics_ver = 9;

	/* Writes of each kind, the first display write making gen12 the owner. */
	drv_i915_mmio_record_reset();
	drv_i915_mmio_record_write(&gen12, 0x2030U, 1U);
	drv_i915_mmio_record_write(&gen9, 0x45404U, 2U);
	drv_i915_mmio_record_write(&gen12, 0x45404U, 3U);
	drv_i915_mmio_record_write(&other_gen12, 0x45408U, 4U);
	drv_i915_mmio_record_write(&gen12, 0x44408U, 5U);
	drv_i915_mmio_record_write(&gen12, 0x44200U, 6U);
	drv_i915_mmio_record_write(&gen12, 0xc4008U, 7U);
	drv_i915_mmio_record_write(&gen12, 0xc7204U, 8U);
	drv_i915_mmio_record_write(&gen12, 0x1010a0U, 9U);
	drv_i915_mmio_record_write(&gen12, 0x138124U, 10U);
	drv_i915_mmio_record_write(&gen12, 0x164284U, 11U);
	drv_i915_mmio_record_write(&gen12, 0x190010U, 12U);
	drv_i915_mmio_record_mark("xd-test step");

	/* An open record reports no entries. */
	count = drv_i915_mmio_record_entries(entries, 8U);
	i915_host_check(count == 0U, "record: an open record shows no entries");

	/* Closes and reads the record back. */
	drv_i915_mmio_record_close(&digest);
	count = drv_i915_mmio_record_entries(entries, 8U);
	i915_host_check(count == 6U && digest.entries == 6U, "record: five display writes and a mark kept");
	i915_host_check(digest.writes == 5U && digest.interrupt_writes == 3U, "record: interrupt registers counted only");
	i915_host_check(count == 6U && entries[0].offset == 0x45404U && entries[0].value == 3U, "record: the owner's first display write");
	i915_host_check(count == 6U && entries[1].offset == 0xc7204U, "record: a south display write");
	i915_host_check(count == 6U && entries[4].offset == 0x164284U, "record: a PLL write");
	i915_host_check(count == 6U && entries[5].count == 0U && entries[5].mark != NULL, "record: the mark");
	i915_host_check(drv_i915_mmio_record_on == 0, "record: closed");

	/* A closed record takes nothing more. */
	drv_i915_mmio_record_write(&gen12, 0x45404U, 13U);
	drv_i915_mmio_record_close(&digest);
	i915_host_check(digest.entries == 6U, "record: nothing after the close");
	drv_i915_mmio_record_log(1);
}

/* Checks the folding of long runs of consecutive registers. */
static void
i915_survey_test_record_runs(void)
{
	struct i915_mmio gen12;
	struct i915_mmio_record_entry entries[8];
	struct i915_mmio_record_digest digest;
	unsigned count;
	unsigned i;

	/* A display 12 register block. */
	gen12.graphics_ver = 12;

	/* Five consecutive timing registers stay five writes. */
	drv_i915_mmio_record_reset();
	for (i = 0U; i < 5U; i++)
		drv_i915_mmio_record_write(&gen12, 0x60000U + 4U * i, i);

	/* A firmware program of 1000 dwords becomes one run. */
	for (i = 0U; i < 1000U; i++)
		drv_i915_mmio_record_write(&gen12, 0x80000U + 4U * i, 2U);

	/* A mark ends a run; the next consecutive write starts anew. */
	drv_i915_mmio_record_mark("xd-test after the program");
	drv_i915_mmio_record_write(&gen12, 0x80000U + 4000U, 3U);

	/* Reads the record back. */
	drv_i915_mmio_record_close(&digest);
	count = drv_i915_mmio_record_entries(entries, 8U);
	i915_host_check(count == 8U, "runs: five writes, one run, a mark and a write");
	i915_host_check(count == 8U && entries[4].offset == 0x60010U && entries[4].count == 1U, "runs: short sequences are kept");
	i915_host_check(count == 8U && entries[5].offset == 0x80000U && entries[5].count == 1000U, "runs: the program is one run");
	i915_host_check(count == 8U && entries[5].value == 2000U, "runs: the run keeps the sum of its values");
	i915_host_check(count == 8U && entries[7].count == 1U, "runs: a mark ends the run");
	i915_host_check(digest.writes == 1006U && digest.runs == 1U, "runs: the digest counts the run's writes");
	drv_i915_mmio_record_log(1);
}

/* Checks the full ring and the write limit. */
static void
i915_survey_test_record_limits(void)
{
	struct i915_mmio gen12;
	struct i915_mmio_record_digest digest;
	unsigned i;

	/* A display 12 register block. */
	gen12.graphics_ver = 12;

	/* Writes to one register past the ring's size are dropped. */
	drv_i915_mmio_record_reset();
	for (i = 0U; i < I915_MMIO_RECORD_ENTRIES + 10U; i++)
		drv_i915_mmio_record_write(&gen12, 0x45404U, i);
	drv_i915_mmio_record_close(&digest);
	i915_host_check(digest.entries == I915_MMIO_RECORD_ENTRIES && digest.dropped == 10U, "limits: a full ring drops");
	i915_host_check(!digest.closed_early, "limits: reported, not closed early");

	/* A device that writes many registers without a report closes the record itself. */
	drv_i915_mmio_record_reset();
	for (i = 0U; i < I915_MMIO_RECORD_WRITE_LIMIT; i++)
		drv_i915_mmio_record_write(&gen12, 0x2030U, i);
	i915_host_check(drv_i915_mmio_record_on == 0, "limits: closed after the write limit");
	drv_i915_mmio_record_close(&digest);
	i915_host_check(digest.closed_early && digest.entries == 0U, "limits: closed early with nothing kept");
}

/* Checks that the order digest sees a swap and the set digest does not. */
static void
i915_survey_test_record_digests(void)
{
	struct i915_mmio gen12;
	struct i915_mmio_record_digest first;
	struct i915_mmio_record_digest swapped;
	struct i915_mmio_record_digest changed;

	/* A display 12 register block. */
	gen12.graphics_ver = 12;

	/* Two writes in one order. */
	drv_i915_mmio_record_reset();
	drv_i915_mmio_record_write(&gen12, 0x45404U, 1U);
	drv_i915_mmio_record_write(&gen12, 0x46010U, 2U);
	drv_i915_mmio_record_close(&first);

	/* The same two writes swapped. */
	drv_i915_mmio_record_reset();
	drv_i915_mmio_record_write(&gen12, 0x46010U, 2U);
	drv_i915_mmio_record_write(&gen12, 0x45404U, 1U);
	drv_i915_mmio_record_close(&swapped);

	/* One value changed. */
	drv_i915_mmio_record_reset();
	drv_i915_mmio_record_write(&gen12, 0x45404U, 1U);
	drv_i915_mmio_record_write(&gen12, 0x46010U, 3U);
	drv_i915_mmio_record_close(&changed);

	/* Compares the digests. */
	i915_host_check(first.order_digest != swapped.order_digest, "digests: the order digest sees a swap");
	i915_host_check(first.set_digest == swapped.set_digest, "digests: the set digest ignores a swap");
	i915_host_check(first.set_digest != changed.set_digest, "digests: the set digest sees a changed value");
}
