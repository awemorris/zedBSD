/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The table of the display 9 arithmetic: every number the Gen9 mode set,
 * the backlight and the firmware's readout compute, over a grid of
 * inputs, written as one line each.
 *
 * Nothing is checked here.  The table taken from one tree is the record
 * another tree's table is compared with, line for line, so a rewrite of
 * the arithmetic shows every input whose answer it changed.  The inputs
 * are fixed (a grid, and a fixed pseudo-random sequence), so two runs of
 * one tree write the same table.
 */

#include "host-test.h"

#include "../../display/xd-clock.h"
#include "../../display/xd-dbuf.h"
#include "../../display/xd-ddi.h"
#include "../../display/xd-dp.h"
#include "../../display/xd-panel.h"
#include "../../display/xd-platform.h"
#include "../../display/xd-takeover.h"
#include "../../display/xd9-flip.h"
#include "../../display/xd9-outputs.h"
#include "../../device-info.h"
#include "../../intel/display-regs-xd9.h"

#include <uapi/errno.h>

#include <stdio.h>
#include <string.h>

/* How many pseudo-random inputs the sampled parts of the table take. */
#define I915_TABLE_SAMPLES	2000U

/* The DisplayPort rates and the HDMI limit the rows take, the most rows a translation table has. */
#define I915_TABLE_RBR_KHZ	162000U
#define I915_TABLE_HBR_KHZ	270000U
#define I915_TABLE_HBR2_KHZ	540000U
#define I915_TABLE_HDMI_KHZ	300000U
#define I915_TABLE_ROWS_MAX	10U

/* The power classes the rows were first written with: H and S, U (ULT), and Y (ULX). */
#define I915_TABLE_CLASS_H	0
#define I915_TABLE_CLASS_U	1
#define I915_TABLE_CLASS_Y	2

/* The kinds of output the rows were first written with. */
#define I915_TABLE_OUTPUT_DP	1U
#define I915_TABLE_OUTPUT_HDMI	2U
#define I915_TABLE_OUTPUT_EDP	3U

/*
 * One pipe of a watermark row in the form the rows were first written
 * in: the input, the pipe's, the plane's and the cursor's ranges, the
 * levels and the line time.
 */
struct i915_table_pipe {
	int active;
	uint32_t hdisplay;
	uint32_t htotal;
	uint32_t pixel_khz;
	uint32_t plane_width;
	uint32_t ddb_start;
	uint32_t ddb_end;
	uint32_t plane_start;
	uint32_t plane_end;
	uint32_t cursor_start;
	uint32_t cursor_end;
	struct {
		int enable;
		uint32_t blocks;
		uint32_t lines;
		uint32_t min_ddb;
	} level[8];
	uint32_t linetime;
};

static void i915_table_cdclk(FILE *out);
static void i915_table_pick_mode(FILE *out);
static void i915_table_dp_link(FILE *out);
static void i915_table_dpcd_rates(FILE *out);
static void i915_table_dpll(FILE *out);
static void i915_table_buf_trans(FILE *out);
static void i915_table_training(FILE *out);
static void i915_table_watermarks(FILE *out);
static void i915_table_wm_words(FILE *out);
static void i915_table_latency(FILE *out);
static void i915_table_pwm(FILE *out);
static void i915_table_readout(FILE *out);
static void i915_table_flip_numbers(FILE *out);
static void i915_table_print_wm(FILE *out, unsigned row, int error, const struct i915_table_pipe pipes[3]);
static const struct i915_xd_platform *i915_table_row(int power_class, int platform);
static int i915_table_class(uint16_t device);
static void i915_table_skip(unsigned draws);
static void i915_table_print_readout(FILE *out, unsigned row, int error, const struct i915_xd9_fw_readout *readout);
static void i915_table_sv7(struct i915_xd9_fw_registers *registers);
static uint32_t i915_table_random(void);

/*
 * The state of the pseudo-random sequence.
 *
 * Starts at a fixed seed and moves on every draw, so the sampled inputs
 * are the same on every run.
 */
static uint32_t i915_table_seed = 0x2545f491U;

/* The memory latencies of the watermark rows, in microseconds: a typical part, and one with level 0 at 0. */
static const uint16_t i915_table_latencies[2][8] = {
	{ 2U, 4U, 10U, 18U, 28U, 40U, 60U, 90U },
	{ 0U, 7U, 15U, 25U, 40U, 60U, 0U, 0U }
};

/*
 * Writes the table to the file the command line names, or to the standard output.
 */
int
main(
	int argc,
	char **argv)
{
	FILE *out;

	/* The file the table goes to. */
	out = stdout;
	if (argc > 1) {
		out = fopen(argv[1], "w");
		if (out == NULL) {
			fprintf(stderr, "%s: cannot write\n", argv[1]);
			return 2;
		}
	}

	/* Each part of the arithmetic in turn. */
	i915_table_cdclk(out);
	i915_table_pick_mode(out);
	i915_table_dp_link(out);
	i915_table_dpcd_rates(out);
	i915_table_dpll(out);
	i915_table_buf_trans(out);
	i915_table_training(out);
	i915_table_watermarks(out);
	i915_table_wm_words(out);
	i915_table_latency(out);
	i915_table_pwm(out);
	i915_table_readout(out);
	i915_table_flip_numbers(out);

	/* The file is complete. */
	if (out != stdout)
		fclose(out);

	/* Succeeded: the whole table is written. */
	return 0;
}

/* Writes the CD clock of every LCPLL1 state, DPLL0 VCO code and CDCLK_CTL select. */
static void
i915_table_cdclk(
	FILE *out)
{
	static const uint32_t lcpll1[3] = { 0U, I915_XD9_LCPLL_ENABLE, I915_XD9_LCPLL_ENABLE | I915_XD9_LCPLL_LOCK };
	uint32_t ctrl1;
	uint32_t cdclk_ctl;
	uint32_t khz;
	unsigned state;
	unsigned rate;
	unsigned select;
	unsigned divider;

	/* Each LCPLL1 state, DPLL0 link rate code, CD clock select and decimal divider. */
	for (state = 0U; state < 3U; state++) {
		for (rate = 0U; rate < 8U; rate++) {
			for (select = 0U; select < 4U; select++) {
				for (divider = 0U; divider < 0x800U; divider += 0x7fU) {
					ctrl1 = (uint32_t)rate << 1;
					cdclk_ctl = ((uint32_t)select << 26) | divider;
					khz = drv_i915_xd9_cdclk_khz(lcpll1[state], ctrl1, cdclk_ctl);
					fprintf(out, "cdclk %08x %08x %08x %u\n", lcpll1[state], ctrl1, cdclk_ctl, khz);
				}
			}
		}
	}
}

/* Writes the mode picked from pseudo-random EDIDs at each pixel clock limit. */
static void
i915_table_pick_mode(
	FILE *out)
{
	static const uint32_t limits[3] = { 0U, I915_TABLE_HDMI_KHZ, 675000U };
	struct i915_display_mode mode;
	uint8_t edid[256];
	unsigned sample;
	unsigned index;
	unsigned limit;
	int error;

	/* Each sample's EDID: random bytes, a CEA extension's tag and DTD offset on half of them. */
	for (sample = 0U; sample < I915_TABLE_SAMPLES / 4U; sample++) {
		for (index = 0U; index < sizeof(edid); index++)
			edid[index] = (uint8_t)i915_table_random();
		if ((sample & 1U) != 0U) {
			edid[128] = 0x02U;
			edid[130] = (uint8_t)(4U + (i915_table_random() % 100U));
		}

		/* The base block alone and both blocks, at each limit. */
		for (limit = 0U; limit < 3U; limit++) {
			memset(&mode, 0, sizeof(mode));
			error = drv_i915_xd9_pick_mode(edid, 256U, limits[limit], &mode);
			fprintf(out, "mode %u %u %d %d %u %u %u %u %u %u %u %u %d %d %u %u %u %d\n",
			    sample,
			    limits[limit],
			    error,
			    mode.clock_khz,
			    mode.hdisplay,
			    mode.hsync_start,
			    mode.hsync_end,
			    mode.htotal,
			    mode.vdisplay,
			    mode.vsync_start,
			    mode.vsync_end,
			    mode.vtotal,
			    mode.hsync_positive,
			    mode.vsync_positive,
			    mode.width_mm,
			    mode.height_mm,
			    mode.descriptor_index,
			    mode.edid_bpc);
		}
	}

	/* No EDID at all: the safe mode. */
	memset(&mode, 0, sizeof(mode));
	error = drv_i915_xd9_pick_mode(NULL, 0U, I915_TABLE_HDMI_KHZ, &mode);
	fprintf(out, "mode none %d %d %u %u\n", error, mode.clock_khz, mode.hdisplay, mode.vdisplay);
}

/* Writes the DP link and its M and N for every sink limit and pixel clock. */
static void
i915_table_dp_link(
	FILE *out)
{
	static const uint32_t sinks[4] = { I915_TABLE_RBR_KHZ, I915_TABLE_HBR_KHZ, I915_TABLE_HBR2_KHZ, 810000U };
	struct i915_xd_dp_choice choice;
	struct i915_xd_dp_m_n m_n;
	uint32_t rate;
	unsigned width;
	uint32_t pixel_khz;
	unsigned sink;
	unsigned lanes;
	int error;

	/* Each sink rate, lane count and pixel clock from 20 MHz to 700 MHz. */
	for (sink = 0U; sink < 4U; sink++) {
		for (lanes = 1U; lanes <= 4U; lanes++) {
			for (pixel_khz = 20000U; pixel_khz <= 700000U; pixel_khz += 2750U) {
				memset(&choice, 0, sizeof(choice));
				memset(&m_n, 0, sizeof(m_n));
				error = drv_i915_xd9_dp_link(sinks[sink], lanes, pixel_khz, &choice, &m_n);
				if (error != 0) {
					memset(&choice, 0, sizeof(choice));
					memset(&m_n, 0, sizeof(m_n));
				}
				fprintf(out, "link %u %u %u %d %u %u %08x %08x %08x %08x %u\n",
				    sinks[sink],
				    lanes,
				    pixel_khz,
				    error,
				    choice.rate_khz,
				    choice.lanes,
				    m_n.data_m,
				    m_n.data_n,
				    m_n.link_m,
				    m_n.link_n,
				    m_n.tu);
			}
		}
	}

	/* The M and N alone at odd clocks on every link. */
	for (sink = 0U; sink < 3U; sink++) {
		for (lanes = 1U; lanes <= 4U; lanes *= 2U) {
			for (pixel_khz = 1U; pixel_khz <= 900000U; pixel_khz = pixel_khz * 3U + 7U) {
				rate = sinks[sink];
				width = lanes;
				drv_i915_xd_dp_m_n(24U * 16U, width, pixel_khz, rate, 1000000U, &m_n);
				fprintf(out, "m_n %u %u %u %08x %08x %08x %08x %u\n",
				    rate,
				    width,
				    pixel_khz,
				    m_n.data_m,
				    m_n.data_n,
				    m_n.link_m,
				    m_n.link_n,
				    m_n.tu);
			}
		}
	}
}

/* Writes the port clock of every DPCD rate code and the code of every port clock. */
static void
i915_table_dpcd_rates(
	FILE *out)
{
	static const uint32_t clocks[8] = { 0U, 81000U, 162000U, 216000U, 270000U, 324000U, 540000U, 810000U };
	unsigned code;
	unsigned clock;

	/* Each code byte. */
	for (code = 0U; code < 256U; code++)
		fprintf(out, "rate_khz %02x %u\n", code, drv_i915_xd9_dp_sink_rate((uint8_t)code));

	/* Each port clock. */
	for (clock = 0U; clock < 8U; clock++)
		fprintf(out, "rate_code %u %02x\n", clocks[clock], drv_i915_xd9_dp_rate_code(clocks[clock]));
}

/* Writes the DPLL words of every DP port clock and of HDMI clocks from 20 MHz to 650 MHz. */
static void
i915_table_dpll(
	FILE *out)
{
	static const uint32_t clocks[9] = { 0U, 81000U, 162000U, 216000U, 243000U, 270000U, 324000U, 432000U, 540000U };
	static const uint32_t references[2] = { 24000U, 19200U };
	struct i915_xd9_dpll_words words;
	uint32_t pixel_khz;
	unsigned clock;
	unsigned reference;
	int error;

	/* Each DP port clock. */
	for (clock = 0U; clock < 9U; clock++) {
		memset(&words, 0, sizeof(words));
		error = drv_i915_xd9_dpll_dp_words(clocks[clock], &words);
		fprintf(out, "dpll_dp %u %d %08x %08x %08x %u\n", clocks[clock], error, words.ctrl1, words.cfgcr1, words.cfgcr2, words.port_khz);
	}

	/* Each HDMI pixel clock on both references. */
	for (reference = 0U; reference < 2U; reference++) {
		for (pixel_khz = 20000U; pixel_khz <= 650000U; pixel_khz += 250U) {
			memset(&words, 0, sizeof(words));
			error = EINVAL;
			if (references[reference] == 24000U)
				error = drv_i915_xd9_dpll_hdmi_words(pixel_khz, &words);
			fprintf(out, "dpll_hdmi %u %u %d %08x %08x %08x %u\n",
			    references[reference],
			    pixel_khz,
			    error,
			    words.ctrl1,
			    words.cfgcr1,
			    words.cfgcr2,
			    words.port_khz);
		}
	}
}

/* Writes the DDI buffer tables of every part, output and port, and the levels around them. */
static void
i915_table_buf_trans(
	FILE *out)
{
	static const int platforms[4] = { I915_PLATFORM_SKYLAKE, I915_PLATFORM_KABYLAKE, I915_PLATFORM_COFFEELAKE, I915_PLATFORM_COMETLAKE };
	static const uint16_t devices[16] = {
		0x1902U, 0x1906U, 0x190eU, 0x1912U, 0x1916U, 0x191eU, 0x5906U, 0x590eU,
		0x5916U, 0x591eU, 0x5917U, 0x3e92U, 0x3ea0U, 0x9bc5U, 0x9b41U, 0x87c0U
	};
	const struct i915_xd9_buf_trans *table;
	unsigned platform;
	unsigned device;
	unsigned output;
	unsigned port;
	unsigned count;
	unsigned entry;
	unsigned set;
	unsigned row;
	int sku;

	/* The power class of each device id on each platform. */
	for (platform = 0U; platform < 4U; platform++) {
		for (device = 0U; device < 16U; device++)
			fprintf(out, "sku %d %04x %d\n", platforms[platform], devices[device], i915_table_class(devices[device]));
	}

	/* Each class, platform, output kind and port: the entries, and the HDMI default. */
	for (sku = I915_TABLE_CLASS_H; sku <= I915_TABLE_CLASS_Y; sku++) {
		for (platform = 0U; platform < 4U; platform++) {
			fprintf(out, "hdmi_level %d %d %u\n", sku, platforms[platform], drv_i915_xd9_ddi_hdmi_row(i915_table_row(I915_TABLE_CLASS_H, platforms[platform])));
			for (output = I915_TABLE_OUTPUT_DP; output <= I915_TABLE_OUTPUT_EDP; output++) {
				for (port = 0U; port < 5U; port++) {
					count = 0U;
					table = drv_i915_xd9_ddi_buf_table(i915_table_row(sku, platforms[platform]), output, port, &count);
					fprintf(out, "buf_trans %d %d %u %u %u\n", sku, platforms[platform], output, port, count);
					for (entry = 0U; table != NULL && entry < count; entry++)
						fprintf(out, "  %u %08x %08x %u\n", entry, table[entry].trans1, table[entry].trans2, table[entry].iboost);
				}
			}
		}
	}

	/* The table entry of every training set, and the highest swing of every table size. */
	for (set = 0U; set < 256U; set++)
	{
		row = 0U;
		if (drv_i915_xd_combo_level_row((uint8_t)set, &row) != 0)
			row = 0U;
		fprintf(out, "signal_level %02x %u\n", set, row);
	}
	for (count = 0U; count <= I915_TABLE_ROWS_MAX + 1U; count++)
		fprintf(out, "voltage_max %u %u\n", count, drv_i915_xd_dp_swing_max(count));
}

/*
 * Draws what the rows of the training decisions took: those decisions are
 * the xd code's now (xd-dp.c, with their own host tests), and their rows
 * are no longer written, but their draws keep the later rows comparable
 * with the record.
 */
static void
i915_table_training(
	FILE *out)
{
	(void)out;

	/* Six bytes for each of the samples. */
	i915_table_skip(I915_TABLE_SAMPLES * 6U);
}

/* Writes the DDB split and watermarks of pseudo-random sets of lit pipes. */
static void
i915_table_watermarks(
	FILE *out)
{
	static const uint32_t widths[6] = { 640U, 1280U, 1920U, 2560U, 3840U, 4096U };
	struct i915_xd9_pipe_plan plans[3];
	struct i915_table_pipe pipes[3];
	uint32_t width;
	unsigned sample;
	unsigned pipe;
	unsigned latency;
	unsigned level;
	int error;

	/* Each sample: every pipe lit or dark, its mode's width, total and clock, the plane as wide as the mode. */
	for (sample = 0U; sample < I915_TABLE_SAMPLES; sample++) {
		memset(pipes, 0, sizeof(pipes));
		for (pipe = 0U; pipe < 3U; pipe++) {
			if ((i915_table_random() % 3U) == 0U)
				continue;
			width = widths[i915_table_random() % 6U];
			pipes[pipe].active = 1;
			pipes[pipe].hdisplay = width;
			pipes[pipe].htotal = width + 32U + i915_table_random() % 600U;
			pipes[pipe].pixel_khz = 25000U + i915_table_random() % 575000U;
			pipes[pipe].plane_width = width;
		}

		/* The computation on one of the latency sets. */
		latency = sample & 1U;
		memset(plans, 0, sizeof(plans));
		for (pipe = 0U; pipe < 3U; pipe++) {
			plans[pipe].active = pipes[pipe].active;
			plans[pipe].hdisplay = pipes[pipe].hdisplay;
			plans[pipe].htotal = pipes[pipe].htotal;
			plans[pipe].pixel_khz = pipes[pipe].pixel_khz;
			plans[pipe].plane_width = pipes[pipe].plane_width;
		}
		error = drv_i915_xd9_dbuf_plan(i915_table_row(I915_TABLE_CLASS_H, I915_PLATFORM_KABYLAKE), i915_table_latencies[latency], plans, 3U);
		for (pipe = 0U; pipe < 3U; pipe++) {
			if (!plans[pipe].computed)
				continue;
			pipes[pipe].ddb_start = plans[pipe].pipe_ddb.start;
			pipes[pipe].ddb_end = plans[pipe].pipe_ddb.end;
			pipes[pipe].plane_start = plans[pipe].plane_ddb.start;
			pipes[pipe].plane_end = plans[pipe].plane_ddb.end;
			pipes[pipe].cursor_start = plans[pipe].cursor_ddb.start;
			pipes[pipe].cursor_end = plans[pipe].cursor_ddb.end;
			pipes[pipe].linetime = plans[pipe].linetime;
			for (level = 0U; level < 8U; level++) {
				pipes[pipe].level[level].enable = plans[pipe].wm.level[level].enable;
				pipes[pipe].level[level].blocks = plans[pipe].wm.level[level].blocks;
				pipes[pipe].level[level].lines = plans[pipe].wm.level[level].lines;
				pipes[pipe].level[level].min_ddb = plans[pipe].wm.level[level].min_ddb;
			}
		}
		i915_table_print_wm(out, sample, error, pipes);
	}
}

/*
 * Draws what the rows of the watermark and DDB words took: the words are
 * written by the xd code now (xd-dbuf.c), and their rows are no longer
 * written, but their draws keep the later rows comparable with the
 * record.
 */
static void
i915_table_wm_words(
	FILE *out)
{
	(void)out;

	/* Four draws for each level, two for each range. */
	i915_table_skip((I915_TABLE_SAMPLES / 4U) * 6U);
}

/* Writes the latencies decoded from pseudo-random PCODE words, with and without IPC. */
static void
i915_table_latency(
	FILE *out)
{
	uint16_t latency[8];
	uint32_t low;
	uint32_t high;
	unsigned sample;
	int ipc;

	/* Each sample's two words, both ways. */
	for (sample = 0U; sample < I915_TABLE_SAMPLES / 4U; sample++) {
		low = i915_table_random();
		high = i915_table_random();
		if ((sample % 5U) == 0U)
			low &= 0xffU;
		for (ipc = 0; ipc <= 1; ipc++) {
			memset(latency, 0, sizeof(latency));
			drv_i915_xd9_wm_latency(low, high, ipc, latency);
			fprintf(out, "latency %08x %08x %d %u %u %u %u %u %u %u %u\n",
			    low,
			    high,
			    ipc,
			    latency[0],
			    latency[1],
			    latency[2],
			    latency[3],
			    latency[4],
			    latency[5],
			    latency[6],
			    latency[7]);
		}
	}
}

/* Writes the backlight PWM's decode and its brightness mapping both ways. */
static void
i915_table_pwm(
	FILE *out)
{
	struct i915_xd9_pwm pwm;
	uint32_t control;
	uint32_t period;
	uint32_t word;
	int cnp;
	uint32_t percent;
	uint32_t duty;
	unsigned sample;
	int error;

	/* Each sample: either PCH layout, a control, a period and a duty. */
	for (sample = 0U; sample < I915_TABLE_SAMPLES / 4U; sample++) {
		memset(&pwm, 0, sizeof(pwm));
		cnp = (int)(sample & 1U);
		control = i915_table_random() & 0xa0000000U;
		period = i915_table_random();
		if ((sample % 7U) == 0U)
			period &= 0xffffU;
		word = i915_table_random() & 0xfffffU;
		error = drv_i915_xd9_pwm_decode(control, period, word, !cnp, &pwm);
		fprintf(out, "pwm %d %08x %08x %08x %d %d %d %u %u %u\n",
		    cnp,
		    control,
		    period,
		    word,
		    error,
		    pwm.running,
		    pwm.active_low,
		    pwm.period,
		    pwm.floor,
		    pwm.duty);

		/* The mapping of a usable PWM: each brightness's duty, and the brightness of a few duties. */
		if (error != 0 || pwm.period == 0U)
			continue;
		for (percent = 0U; percent <= 101U; percent += 3U)
			fprintf(out, "  duty %u %u\n", percent, drv_i915_xd9_pwm_duty(&pwm, percent));
		for (duty = 0U; duty <= pwm.period + pwm.period / 8U; duty += pwm.period / 16U + 1U)
			fprintf(out, "  percent %u %u\n", duty, drv_i915_xd9_pwm_percent(&pwm, duty));
	}
}

/* Writes the readout of the SV7's firmware display with pseudo-random changes to its registers. */
static void
i915_table_readout(
	FILE *out)
{
	struct i915_xd9_fw_registers registers;
	struct i915_xd9_fw_readout readout;
	uint32_t *words;
	unsigned sample;
	unsigned changes;
	unsigned change;
	unsigned word;
	unsigned count;
	int error;

	/* The SV7 itself. */
	i915_table_sv7(&registers);
	memset(&readout, 0, sizeof(readout));
	error = drv_i915_xd9_fw_decode(&registers, &readout);
	i915_table_print_readout(out, 0U, error, &readout);

	/* Each sample: the SV7 with one to three words flipped in one bit, or replaced. */
	words = (uint32_t *)&registers;
	count = (unsigned)(sizeof(registers) / sizeof(uint32_t));
	for (sample = 1U; sample <= I915_TABLE_SAMPLES; sample++) {
		i915_table_sv7(&registers);
		changes = 1U + i915_table_random() % 3U;
		for (change = 0U; change < changes; change++) {
			word = i915_table_random() % count;
			if ((i915_table_random() & 1U) != 0U) {
				words[word] ^= 1U << (i915_table_random() % 32U);
			} else {
				words[word] = i915_table_random();
			}
		}

		/* The decode of the changed registers. */
		memset(&readout, 0, sizeof(readout));
		error = drv_i915_xd9_fw_decode(&registers, &readout);
		i915_table_print_readout(out, sample, error, &readout);
	}
}

/* Writes the flip display's window sizes, pixel clocks and port clocks. */
static void
i915_table_flip_numbers(
	FILE *out)
{
	uint32_t bytes;
	uint32_t link_m;
	uint32_t link_n;
	uint32_t port_khz;
	unsigned rate;
	unsigned sample;

	/* The display window of buffers from one page to 64 MiB. */
	for (bytes = 1U; bytes <= 64U * 1024U * 1024U; bytes = bytes * 2U + 4095U)
		fprintf(out, "window %u %u\n", bytes, drv_i915_xd9_flip_window_pages(bytes));

	/* The port clock of every DPLL_CTRL1 link rate code. */
	for (rate = 0U; rate < 8U; rate++)
		fprintf(out, "port_khz %u %u\n", rate, drv_i915_xd9_dpll_rate_port_khz(rate));

	/* The pixel clock of pseudo-random link M and N on the DP port clocks. */
	for (sample = 0U; sample < I915_TABLE_SAMPLES / 4U; sample++) {
		link_m = i915_table_random() & 0xffffffU;
		link_n = i915_table_random() & 0xffffffU;
		if ((sample % 9U) == 0U)
			link_n = 0U;
		port_khz = drv_i915_xd9_dpll_rate_port_khz(i915_table_random() % 6U);
		fprintf(out, "pixel_khz %u %u %u %u\n", link_m, link_n, port_khz, drv_i915_xd9_dp_pixel_khz(link_m, link_n, port_khz));
	}
}

/* Writes one watermark row: the error and each pipe's DDB, levels and line time. */
static void
i915_table_print_wm(
	FILE *out,
	unsigned row,
	int error,
	const struct i915_table_pipe pipes[3])
{
	const struct i915_table_pipe *wm;
	unsigned pipe;
	unsigned level;

	/* The row's outcome. */
	fprintf(out, "wm %u %d\n", row, error);

	/* Each pipe that is lit. */
	for (pipe = 0U; pipe < 3U; pipe++) {
		wm = &pipes[pipe];
		if (!wm->active)
			continue;
		fprintf(out, "  pipe %u %u %u %u %u ddb %u %u plane %u %u cursor %u %u linetime %u\n",
		    pipe,
		    wm->hdisplay,
		    wm->htotal,
		    wm->pixel_khz,
		    wm->plane_width,
		    wm->ddb_start,
		    wm->ddb_end,
		    wm->plane_start,
		    wm->plane_end,
		    wm->cursor_start,
		    wm->cursor_end,
		    wm->linetime);
		for (level = 0U; level < 8U; level++) {
			fprintf(out, "    level %u %d %u %u %u\n",
			    level,
			    wm->level[level].enable,
			    wm->level[level].blocks,
			    wm->level[level].lines,
			    wm->level[level].min_ddb);
		}
	}
}

/* Writes one readout row. */
static void
i915_table_print_readout(
	FILE *out,
	unsigned row,
	int error,
	const struct i915_xd9_fw_readout *readout)
{
	const char *reason;

	/* A readout the flips accept has no reason. */
	reason = readout->reason;
	if (reason == NULL)
		reason = "-";

	/* Succeeded: every field. */
	fprintf(out, "readout %u %d %s %u %u %u %u %u %u %d %08x %u %d %u %u %u %u %d\n",
	    row,
	    error,
	    reason,
	    readout->lit_pipes,
	    readout->pipe,
	    readout->transcoder,
	    readout->width,
	    readout->height,
	    readout->stride_bytes,
	    readout->rgbx,
	    readout->surf,
	    readout->refresh_millihz,
	    readout->refresh_measured,
	    readout->pixel_khz,
	    readout->htotal,
	    readout->port,
	    readout->dpll,
	    readout->scaled);
}

/*
 * Fills the registers of a Let's Note SV7 (Kaby Lake R) as its firmware
 * leaves them: the 1920x1200 eDP panel on transcoder EDP and pipe A, DDI A
 * clocked by DPLL0 at HBR, the first plane on a linear XRGB8888 surface.
 */
static void
i915_table_sv7(
	struct i915_xd9_fw_registers *registers)
{
	memset(registers, 0, sizeof(*registers));

	/* Power wells 1 and 2 up, no DC state, DPLL0 at link rate 1350 for DDI A. */
	registers->power = 0xf0000000U;
	registers->cdclk = 0x000003ffU;
	registers->dpll_ctrl1 = (I915_XD9_LINK_RATE_1350 << 1) | 1U;
	registers->dpll_ctrl2 = 1U;

	/* The eDP transcoder: DP SST on DDI A from pipe A, running, with its totals and link M and N. */
	registers->transcoder_read[I915_XD9_TRANS_EDP] = 1;
	registers->ddi_func[I915_XD9_TRANS_EDP] = 0x80000000U | (2U << 24) | (4U << 12);
	registers->transconf[I915_XD9_TRANS_EDP] = 0xc0000000U;
	registers->htotal[I915_XD9_TRANS_EDP] = ((2080U - 1U) << 16) | (1920U - 1U);
	registers->vtotal[I915_XD9_TRANS_EDP] = ((1235U - 1U) << 16) | (1200U - 1U);
	registers->link_m[I915_XD9_TRANS_EDP] = 299038U;
	registers->link_n[I915_XD9_TRANS_EDP] = 0x80000U;

	/* Transcoders A to C are read and off. */
	registers->transcoder_read[0] = 1;
	registers->transcoder_read[1] = 1;
	registers->transcoder_read[2] = 1;

	/* Pipe A's first plane: on, XRGB8888, linear, 1920x1200, stride 120 units, surface 0. */
	registers->pipe_read[0] = 1;
	registers->pipe_src[0] = ((1920U - 1U) << 16) | (1200U - 1U);
	registers->plane_ctl[0] = 0x80000000U | (4U << 24) | (1U << 13);
	registers->plane_stride[0] = 120U;
	registers->plane_size[0] = ((1200U - 1U) << 16) | (1920U - 1U);

	/* Pipes B and C are read and off. */
	registers->pipe_read[1] = 1;
	registers->pipe_read[2] = 1;
}

/* Reports the next number of the fixed pseudo-random sequence. */
static uint32_t
i915_table_random(void)
{
	/* One step of a 32-bit xorshift. */
	i915_table_seed ^= i915_table_seed << 13;
	i915_table_seed ^= i915_table_seed >> 17;
	i915_table_seed ^= i915_table_seed << 5;

	/* Succeeded: the new state is the number. */
	return i915_table_seed;
}

/*
 * Reports the display 9 row of a power class the rows were first written
 * with, on a platform: Skylake's, or Kaby Lake's, which Coffee Lake and
 * Comet Lake share.
 */
static const struct i915_xd_platform *
i915_table_row(
	int power_class,
	int platform)
{
	unsigned kind;
	unsigned xd_class;

	/* The kind. */
	kind = I915_XD_KIND_KBL;
	if (platform == I915_PLATFORM_SKYLAKE)
		kind = I915_XD_KIND_SKL;

	/* The class. */
	xd_class = I915_XD_POWER_CLASS_H;
	if (power_class == I915_TABLE_CLASS_U)
		xd_class = I915_XD_POWER_CLASS_U;
	else if (power_class == I915_TABLE_CLASS_Y)
		xd_class = I915_XD_POWER_CLASS_Y;

	/* Succeeded: the row. */
	return drv_i915_xd_platform_display9_class(kind, xd_class);
}

/* Reports the power class of a device id in the form the rows were first written with. */
static int
i915_table_class(
	uint16_t device)
{
	struct i915_xd_choice choice;
	int error;

	/* An id with no row is H and S. */
	error = drv_i915_xd_platform_choose_display9(device, 0U, 0, 0U, &choice);
	if (error != 0)
		return I915_TABLE_CLASS_H;

	/* Succeeded: U, Y, or H and S. */
	if (choice.platform->power_class == I915_XD_POWER_CLASS_U)
		return I915_TABLE_CLASS_U;
	if (choice.platform->power_class == I915_XD_POWER_CLASS_Y)
		return I915_TABLE_CLASS_Y;
	return I915_TABLE_CLASS_H;
}

/* Draws from the pseudo-random sequence without using the draws. */
static void
i915_table_skip(
	unsigned draws)
{
	unsigned draw;

	/* Succeeded: each draw. */
	for (draw = 0U; draw < draws; draw++)
		(void)i915_table_random();
}
