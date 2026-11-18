/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The host test of the xd DisplayPort link (display/xd-dp.c): the rates
 * both ends offer, an eDP 1.4 panel's rate list, the fallback after a
 * failed training, the choice of a link for a mode, the M/N values of
 * the 5330's panel, the link status, and the training against a model of
 * a sink (and of repeaters in front of it) that asks for higher levels
 * until it recovers the clock and equalizes the channel, or never does.
 */

#include "host-test.h"

#include "../../display/xd-dp.h"
#include "../../intel/display-dpcd-xd.h"

#include <uapi/errno.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* The most DPCD accesses the model records. */
#define I915_XD_DP_TEST_LOG		128U

/*
 * One DPCD access the model saw: a write or a read, its address, its
 * size, and the first byte (written, or answered).
 */
struct i915_xd_dp_test_access {
	int write;
	uint32_t address;
	unsigned size;
	uint8_t first;
};

/*
 * The model of a sink and up to two repeaters in front of it.
 *
 * Each receiver (0 the sink, 1 and 2 the repeaters) keeps the pattern and
 * the levels it was given; it reports the clock recovered once every lane
 * is at swing cr_swing (never with cr_never), and the channel equalized
 * once every lane is at pre-emphasis eq_preemph (never with eq_never),
 * asking for those levels until then.
 */
struct i915_xd_dp_test_sink {
	uint8_t caps[I915_XD_DPCD_CAPS_SIZE];
	uint8_t repeater[I915_XD_DPCD_REPEATER_COMMON_SIZE];
	uint8_t repeater_tx_cap;
	int refuse_opaque;
	uint8_t pattern[3];
	uint8_t set[3][4];
	unsigned lanes;
	uint8_t cr_swing;
	uint8_t eq_preemph;
	int cr_never;
	int eq_never;
	struct i915_xd_dp_test_access log[I915_XD_DP_TEST_LOG];
	unsigned log_count;
	unsigned levels_calls;
	uint8_t levels_last[4];
	uint8_t patterns[16];
	unsigned pattern_calls;
	unsigned prepared;
	unsigned idled;
	unsigned slept_us;
	unsigned errors;
};

/* The model the checks share; each group starts it afresh. */
static struct i915_xd_dp_test_sink i915_xd_dp_test_model;

/* The capabilities a link keeps between trainings. */
static uint8_t i915_xd_dp_test_caps[I915_XD_DPCD_CAPS_SIZE];
static uint8_t i915_xd_dp_test_repeater_caps[I915_XD_DPCD_REPEATER_COMMON_SIZE];
static uint8_t i915_xd_dp_test_repeater_phy[I915_XD_DPCD_REPEATER_MAX][3];

static void i915_xd_dp_test_setup(const uint8_t *caps);
static void i915_xd_dp_test_record(int write, uint32_t address, unsigned size, uint8_t first);
static unsigned i915_xd_dp_test_hop(uint32_t address, uint32_t base);
static int i915_xd_dp_test_read(void *context, uint32_t address, uint8_t *buffer, unsigned size);
static int i915_xd_dp_test_write(void *context, uint32_t address, const uint8_t *buffer, unsigned size);
static int i915_xd_dp_test_probe(void *context, uint32_t address);
static int i915_xd_dp_test_read_caps(void *context, uint8_t *caps);
static void i915_xd_dp_test_levels(void *context, const uint8_t set[4]);
static void i915_xd_dp_test_pattern(void *context, uint8_t pattern);
static void i915_xd_dp_test_prepare(void *context);
static void i915_xd_dp_test_idle(void *context);
static void i915_xd_dp_test_sleep(void *context, unsigned min_us, unsigned max_us);
static void i915_xd_dp_test_note(void *context, unsigned kind, const char *text);
static void i915_xd_dp_test_link(struct i915_xd_dp_link *link, uint32_t rate_khz, unsigned lanes, int find_repeaters);
static void i915_xd_dp_test_rates(void);
static void i915_xd_dp_test_fallback(void);
static void i915_xd_dp_test_choose(void);
static void i915_xd_dp_test_m_n(void);
static void i915_xd_dp_test_status(void);
static void i915_xd_dp_test_train_edp(void);
static void i915_xd_dp_test_train_faults(void);
static void i915_xd_dp_test_train_levels(void);
static void i915_xd_dp_test_train_repeaters(void);

/*
 * Runs every check of the DisplayPort link and reports.
 */
int
main(void)
{
	int failures;

	/* Runs each group of checks. */
	i915_xd_dp_test_rates();
	i915_xd_dp_test_fallback();
	i915_xd_dp_test_choose();
	i915_xd_dp_test_m_n();
	i915_xd_dp_test_status();
	i915_xd_dp_test_train_edp();
	i915_xd_dp_test_train_faults();
	i915_xd_dp_test_train_levels();
	i915_xd_dp_test_train_repeaters();

	/* Reports the result. */
	failures = i915_host_report("host_xd_dp_test");
	if (failures != 0)
		return 1;

	/* Succeeded: every check passed. */
	return 0;
}

/* Starts a fresh model with the sink's capabilities. */
static void
i915_xd_dp_test_setup(
	const uint8_t *caps)
{
	/* An empty model: no repeaters, the clock at swing 0, the channel at pre-emphasis 0. */
	memset(&i915_xd_dp_test_model, 0, sizeof(i915_xd_dp_test_model));
	memcpy(i915_xd_dp_test_model.caps, caps, I915_XD_DPCD_CAPS_SIZE);
	memset(i915_xd_dp_test_caps, 0, sizeof(i915_xd_dp_test_caps));
	memset(i915_xd_dp_test_repeater_caps, 0, sizeof(i915_xd_dp_test_repeater_caps));
	memset(i915_xd_dp_test_repeater_phy, 0, sizeof(i915_xd_dp_test_repeater_phy));
}

/* Records one DPCD access. */
static void
i915_xd_dp_test_record(
	int write,
	uint32_t address,
	unsigned size,
	uint8_t first)
{
	struct i915_xd_dp_test_access *access;

	/* A full log keeps its first entries. */
	if (i915_xd_dp_test_model.log_count == I915_XD_DP_TEST_LOG)
		return;

	/* The next entry. */
	access = &i915_xd_dp_test_model.log[i915_xd_dp_test_model.log_count];
	access->write = write;
	access->address = address;
	access->size = size;
	access->first = first;
	i915_xd_dp_test_model.log_count++;
}

/* Tells which repeater's range an address is in, by the address of repeater 1's register; 0 when none. */
static unsigned
i915_xd_dp_test_hop(
	uint32_t address,
	uint32_t base)
{
	unsigned hop;

	/* Repeaters 1 and 2. */
	for (hop = 1U; hop <= 2U; hop++) {
		if (address == base + I915_XD_DPCD_REPEATER_STRIDE * (hop - 1U))
			return hop;
	}

	/* Not a repeater's register. */
	return 0U;
}

/* Answers a DPCD read of the model. */
static int
i915_xd_dp_test_read(
	void *context,
	uint32_t address,
	uint8_t *buffer,
	unsigned size)
{
	struct i915_xd_dp_test_sink *model;
	unsigned hop;
	unsigned lane;
	unsigned at;
	uint8_t nibble;
	uint8_t request;
	uint8_t status[6];

	/* The model; every read is answered in full. */
	model = context;
	memset(buffer, 0, size);

	/* The repeaters' common capabilities, a byte or the whole run. */
	if (address >= I915_XD_DPCD_REPEATER_REV && address < I915_XD_DPCD_REPEATER_REV + I915_XD_DPCD_REPEATER_COMMON_SIZE) {
		memcpy(buffer, model->repeater + (address - I915_XD_DPCD_REPEATER_REV), size);
		i915_xd_dp_test_record(0, address, size, buffer[0]);
		return (int)size;
	}

	/* A repeater's capabilities: its read interval and its transmitter capability. */
	hop = i915_xd_dp_test_hop(address, I915_XD_DPCD_REPEATER_INTERVAL(1U));
	if (hop == 0U)
		hop = i915_xd_dp_test_hop(address, I915_XD_DPCD_REPEATER_TX_CAP);
	if (hop != 0U && size <= 3U) {
		if (address == I915_XD_DPCD_REPEATER_TX_CAP + I915_XD_DPCD_REPEATER_STRIDE * (hop - 1U)) {
			buffer[0] = model->repeater_tx_cap;
		} else if (size >= 2U) {
			buffer[1] = model->repeater_tx_cap;
		}

		/* Records the read. */
		i915_xd_dp_test_record(0, address, size, buffer[0]);
		return (int)size;
	}

	/* The link status of the sink or of a repeater. */
	hop = i915_xd_dp_test_hop(address, I915_XD_DPCD_REPEATER_STATUS(1U));
	if (address == I915_XD_DPCD_LANE_STATUS || hop != 0U) {
		memset(status, 0, sizeof(status));
		for (lane = 0U; lane < model->lanes; lane++) {
			nibble = 0U;
			if (!model->cr_never && (model->set[hop][lane] & I915_XD_DPCD_SWING) >= model->cr_swing)
				nibble |= I915_XD_DPCD_LANE_CR;
			if ((model->pattern[hop] & 0x0fU) != I915_XD_DPCD_PATTERN_1 &&
			    (nibble & I915_XD_DPCD_LANE_CR) != 0U &&
			    !model->eq_never &&
			    ((model->set[hop][lane] & I915_XD_DPCD_PREEMPH) >> I915_XD_DPCD_PREEMPH_SHIFT) >= model->eq_preemph)
				nibble |= I915_XD_DPCD_LANE_EQ | I915_XD_DPCD_LANE_LOCKED;
			status[lane >> 1] |= (uint8_t)(nibble << (4U * (lane & 1U)));
			request = (uint8_t)(model->cr_swing | (model->eq_preemph << 2));
			status[4U + (lane >> 1)] |= (uint8_t)(request << (4U * (lane & 1U)));
		}

		/* The lanes are aligned. */
		status[2] = I915_XD_DPCD_ALIGN_DONE;

		/* A repeater's status has no sink status byte. */
		if (hop == 0U) {
			memcpy(buffer, status, size < 6U ? size : 6U);
		} else {
			at = 0U;
			buffer[at++] = status[0];
			buffer[at++] = status[1];
			buffer[at++] = status[2];
			buffer[at++] = status[4];
			buffer[at] = status[5];
		}

		/* Records the read. */
		i915_xd_dp_test_record(0, address, size, buffer[0]);
		return (int)size;
	}

	/* Anything else reads as zeros. */
	i915_xd_dp_test_record(0, address, size, 0U);
	return (int)size;
}

/* Takes a DPCD write of the model. */
static int
i915_xd_dp_test_write(
	void *context,
	uint32_t address,
	const uint8_t *buffer,
	unsigned size)
{
	struct i915_xd_dp_test_sink *model;
	unsigned hop;
	unsigned lane;

	/* The model records every write. */
	model = context;
	i915_xd_dp_test_record(1, address, size, buffer[0]);

	/* The repeaters' mode; a refused non-transparent mode. */
	if (address == I915_XD_DPCD_REPEATER_MODE) {
		if (buffer[0] == I915_XD_DPCD_REPEATER_OPAQUE && model->refuse_opaque)
			return -EIO;
		model->repeater[I915_XD_DPCD_REPEATER_MODE - I915_XD_DPCD_REPEATER_REV] = buffer[0];
		return (int)size;
	}

	/* The pattern, with the levels after it, of the sink or a repeater. */
	hop = i915_xd_dp_test_hop(address, I915_XD_DPCD_REPEATER_PATTERN(1U));
	if (address == I915_XD_DPCD_PATTERN_SET || hop != 0U) {
		model->pattern[hop] = buffer[0];
		for (lane = 0U; lane + 1U < size; lane++)
			model->set[hop][lane] = buffer[1U + lane];
		return (int)size;
	}

	/* The levels alone. */
	hop = i915_xd_dp_test_hop(address, I915_XD_DPCD_REPEATER_LEVELS(1U));
	if (address == I915_XD_DPCD_LEVELS_SET || hop != 0U) {
		for (lane = 0U; lane < size; lane++)
			model->set[hop][lane] = buffer[lane];
		return (int)size;
	}

	/* The rate and lanes. */
	if (address == I915_XD_DPCD_BW_SET && size == 2U)
		model->lanes = buffer[1] & 0x1fU;
	if (address == I915_XD_DPCD_LANES_SET)
		model->lanes = buffer[0] & 0x1fU;

	/* Succeeded: every other write is taken. */
	return (int)size;
}

/* Answers that the sink is awake. */
static int
i915_xd_dp_test_probe(
	void *context,
	uint32_t address)
{
	/* Records the probe. */
	(void)context;
	i915_xd_dp_test_record(0, address, 1U, 0U);

	/* Succeeded: the sink answers. */
	return 0;
}

/* Answers the receiver capabilities. */
static int
i915_xd_dp_test_read_caps(
	void *context,
	uint8_t *caps)
{
	struct i915_xd_dp_test_sink *model;

	/* The model's capabilities. */
	model = context;
	memcpy(caps, model->caps, I915_XD_DPCD_CAPS_SIZE);
	i915_xd_dp_test_record(0, I915_XD_DPCD_REV, I915_XD_DPCD_CAPS_SIZE, caps[0]);

	/* Succeeded: the capabilities are in place. */
	return 0;
}

/* Takes the source's levels. */
static void
i915_xd_dp_test_levels(
	void *context,
	const uint8_t set[4])
{
	struct i915_xd_dp_test_sink *model;

	/* Counts the call and keeps the levels. */
	model = context;
	model->levels_calls++;
	memcpy(model->levels_last, set, 4U);
}

/* Takes the source's pattern. */
static void
i915_xd_dp_test_pattern(
	void *context,
	uint8_t pattern)
{
	struct i915_xd_dp_test_sink *model;

	/* Keeps the patterns in order. */
	model = context;
	if (model->pattern_calls < sizeof(model->patterns))
		model->patterns[model->pattern_calls] = pattern;
	model->pattern_calls++;
}

/* Counts the source's preparation. */
static void
i915_xd_dp_test_prepare(
	void *context)
{
	struct i915_xd_dp_test_sink *model;

	/* Counts it. */
	model = context;
	model->prepared++;
}

/* Counts the source's idle pattern. */
static void
i915_xd_dp_test_idle(
	void *context)
{
	struct i915_xd_dp_test_sink *model;

	/* Counts it. */
	model = context;
	model->idled++;
}

/* Adds up the sleeps of the training. */
static void
i915_xd_dp_test_sleep(
	void *context,
	unsigned min_us,
	unsigned max_us)
{
	struct i915_xd_dp_test_sink *model;

	/* The shortest sleep asked for. */
	(void)max_us;
	model = context;
	model->slept_us += min_us;
}

/* Counts the error notes of the training. */
static void
i915_xd_dp_test_note(
	void *context,
	unsigned kind,
	const char *text)
{
	struct i915_xd_dp_test_sink *model;

	/* Counts the errors. */
	(void)text;
	model = context;
	if (kind == I915_XD_DP_NOTE_ERROR)
		model->errors++;
}

/* Describes a training on the model: an ADL-P source (levels 3 and 3, per lane, patterns 3 and 4). */
static void
i915_xd_dp_test_link(
	struct i915_xd_dp_link *link,
	uint32_t rate_khz,
	unsigned lanes,
	int find_repeaters)
{
	/* The link, the source and the stored capabilities. */
	memset(link, 0, sizeof(*link));
	link->rate_khz = rate_khz;
	link->lanes = lanes;
	link->enhanced = 1;
	link->rate_index = -1;
	link->swing_max = 3U;
	link->preemph_max = 3U;
	link->per_lane = 1;
	link->tps3 = 1;
	link->tps4 = 1;
	link->find_repeaters = find_repeaters;
	link->caps = i915_xd_dp_test_caps;
	link->repeater_caps = i915_xd_dp_test_repeater_caps;
	link->repeater_phy_caps = i915_xd_dp_test_repeater_phy;

	/* Both sides are the model. */
	link->sink.read = i915_xd_dp_test_read;
	link->sink.write = i915_xd_dp_test_write;
	link->sink.probe = i915_xd_dp_test_probe;
	link->sink.read_caps = i915_xd_dp_test_read_caps;
	link->sink.context = &i915_xd_dp_test_model;
	link->source.levels = i915_xd_dp_test_levels;
	link->source.pattern = i915_xd_dp_test_pattern;
	link->source.prepare = i915_xd_dp_test_prepare;
	link->source.idle = i915_xd_dp_test_idle;
	link->source.sleep = i915_xd_dp_test_sleep;
	link->source.note = i915_xd_dp_test_note;
	link->source.context = &i915_xd_dp_test_model;
}

/*
 * The rates of both ends: a display 13 source up to HBR3, the VBT's lower
 * limit, a sink up to HBR2 lowered by a repeater to HBR, a sink that
 * offers nothing, and the eDP 1.4 rate list.
 */
static void
i915_xd_dp_test_rates(void)
{
	static const uint8_t edp_table[16] = {
		0xa4U, 0x1fU, 0x30U, 0x2aU, 0xbcU, 0x34U, 0x00U, 0x00U,
		0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U
	};
	struct i915_xd_dp_rates rates;
	uint32_t edp[8];
	unsigned made_up;
	unsigned count;

	/* A source up to HBR3 and an HBR2 sink share RBR, HBR, HBR2. */
	made_up = drv_i915_xd_dp_rates(810000U, 0U, I915_XD_DPCD_BW_HBR2, 0U, &rates);
	i915_host_check(made_up == 0U && rates.source_count == 8U && rates.sink_count == 3U && rates.common_count == 3U &&
			rates.common[0] == 162000U && rates.common[1] == 270000U && rates.common[2] == 540000U,
			"dp: an HBR3 source and an HBR2 sink share RBR, HBR and HBR2");

	/* The VBT's lower limit and a repeater's HBR limit. */
	made_up = drv_i915_xd_dp_rates(810000U, 432000U, I915_XD_DPCD_BW_HBR3, I915_XD_DPCD_BW_HBR, &rates);
	i915_host_check(made_up == 0U && rates.source_count == 5U && rates.source[4] == 432000U &&
			rates.sink_count == 2U && rates.common_count == 2U && rates.common[1] == 270000U,
			"dp: the VBT's limit cuts the source, a repeater's the sink");

	/* A sink with an unknown code below RBR is given RBR. */
	made_up = drv_i915_xd_dp_rates(0U, 0U, 0x05U, 0U, &rates);
	i915_host_check(made_up == I915_XD_DP_RATES_SINK_NONE && rates.sink_count == 1U && rates.common[0] == 162000U,
			"dp: a sink offering nothing is given RBR");

	/* The rate codes. */
	i915_host_check(drv_i915_xd_dp_rate_code(270000U) == 0x0aU && drv_i915_xd_dp_rate_code(810000U) == 0x1eU &&
			drv_i915_xd_dp_code_rate(0x14U) == 540000U && drv_i915_xd_dp_code_rate(0x01U) == 1000000U,
			"dp: rate codes and back");

	/* An eDP 1.4 list of 1.62, 2.16 and 2.7 Gbit/s. */
	count = drv_i915_xd_dp_edp_rates(edp_table, edp, 8U);
	i915_host_check(count == 3U && edp[0] == 162000U && edp[1] == 216000U && edp[2] == 270000U,
			"dp: an eDP 1.4 panel's rate list");
}

/* The fallback after a failed training: a lower rate, then half the lanes at the top. */
static void
i915_xd_dp_test_fallback(void)
{
	static const uint32_t common[3] = { 162000U, 270000U, 540000U };
	uint32_t rate;
	unsigned lanes;
	int lower;

	/* HBR2 x4 falls to HBR x4. */
	lower = drv_i915_xd_dp_fallback(common, 3U, 540000U, 4U, &rate, &lanes);
	i915_host_check(lower == 1 && rate == 270000U && lanes == 4U, "dp: HBR2 x4 falls to HBR x4");

	/* RBR x4 falls to HBR2 x2. */
	lower = drv_i915_xd_dp_fallback(common, 3U, 162000U, 4U, &rate, &lanes);
	i915_host_check(lower == 1 && rate == 540000U && lanes == 2U, "dp: RBR x4 falls to HBR2 x2");

	/* RBR x1 has nothing lower. */
	lower = drv_i915_xd_dp_fallback(common, 3U, 162000U, 1U, &rate, &lanes);
	i915_host_check(lower == 0, "dp: RBR x1 has nothing lower");
}

/* The choice of a link for a mode: the deepest colour, the lowest rate, the fewest lanes. */
static void
i915_xd_dp_test_choose(void)
{
	static const uint32_t common[3] = { 162000U, 270000U, 540000U };
	struct i915_xd_dp_choice choice;
	int error;

	/* 1920x1080@60 (148.5 MHz) at 8 bpc needs 445500 kB/s: RBR x4 carries 648000. */
	error = drv_i915_xd_dp_choose(148500U, 8U, 6U, common, 3U, 0U, 4U, &choice);
	i915_host_check(error == 0 && choice.rate_khz == 162000U && choice.lanes == 4U && choice.bpp == 24U &&
			choice.need_kbps == 445500U && choice.carry_kbps == 648000U,
			"dp: 1080p60 at 8 bpc on RBR x4");

	/* Two lanes at most: HBR x2 (540000 kB/s). */
	error = drv_i915_xd_dp_choose(148500U, 8U, 6U, common, 3U, 0U, 2U, &choice);
	i915_host_check(error == 0 && choice.rate_khz == 270000U && choice.lanes == 2U, "dp: 1080p60 on two lanes takes HBR");

	/* One lane of RBR carries no 1080p60, even at 6 bpc. */
	error = drv_i915_xd_dp_choose(148500U, 8U, 6U, common, 3U, 162000U, 1U, &choice);
	i915_host_check(error == ENOSPC, "dp: a mode no link carries");

	/* 4K60 (594 MHz, 1782000 kB/s at 8 bpc) on HBR2 x4 (2160000). */
	error = drv_i915_xd_dp_choose(594000U, 8U, 6U, common, 3U, 0U, 4U, &choice);
	i915_host_check(error == 0 && choice.rate_khz == 540000U && choice.lanes == 4U && choice.bpp == 24U,
			"dp: 4K60 takes HBR2 x4 at 8 bpc");

	/* Under an HBR limit a 400 MHz mode drops to 6 bpc (900000 kB/s on HBR x4, 1080000). */
	error = drv_i915_xd_dp_choose(400000U, 8U, 6U, common, 3U, 270000U, 4U, &choice);
	i915_host_check(error == 0 && choice.rate_khz == 270000U && choice.lanes == 4U && choice.bpp == 18U,
			"dp: a mode too wide for 8 bpc under the fallback's limit drops to 6 bpc");
}

/* The 5330's panel: 140.8 MHz at 18 bpp on HBR x2 gives the M/N values its firmware set. */
static void
i915_xd_dp_test_m_n(void)
{
	struct i915_xd_dp_m_n m_n;

	/* The values against the dump of the 5330's registers. */
	drv_i915_xd_dp_m_n(18U * 16U, 2U, 140800U, 270000U, 1000000U, &m_n);
	i915_host_check(m_n.tu == 64U && m_n.data_m == 0x4b17e4U && m_n.data_n == 0x800000U &&
			m_n.link_m == 0x42bfeU && m_n.link_n == 0x80000U,
			"dp: the 5330's panel M/N (data 0x4b17e4/0x800000, link 0x42bfe/0x80000)");

	/* The bandwidth arithmetic. */
	i915_host_check(drv_i915_xd_dp_need_kbps(140800U, 18U) == 316800U && drv_i915_xd_dp_carry_kbps(270000U, 2U) == 540000U &&
			drv_i915_xd_dp_symbol_khz(270000U) == 270000U && drv_i915_xd_dp_symbol_bits(1000000U) == 32U,
			"dp: the data rates and the symbol clock");
}

/* The link status: clock recovery, equalization, and a repeater's layout. */
static void
i915_xd_dp_test_status(void)
{
	static const uint8_t recovered[6] = { 0x11U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U };
	static const uint8_t equalized[6] = { 0x77U, 0x77U, 0x01U, 0x00U, 0x00U, 0x00U };
	static const uint8_t unaligned[6] = { 0x77U, 0x77U, 0x00U, 0x00U, 0x00U, 0x00U };
	static const uint8_t caps[15] = { 0x14U, 0x14U, 0xc4U, 0x81U };
	struct i915_xd_dp_link link;
	uint8_t status[6];
	int error;

	/* Two lanes with the clock; four without the third. */
	i915_host_check(drv_i915_xd_dp_clock_recovered(recovered, 2U) == 1 && drv_i915_xd_dp_clock_recovered(recovered, 4U) == 0,
			"dp: clock recovery on the lanes in use");

	/* Four equalized lanes, but only when aligned. */
	i915_host_check(drv_i915_xd_dp_equalized(equalized, 4U) == 1 && drv_i915_xd_dp_equalized(unaligned, 4U) == 0,
			"dp: equalization needs the lanes aligned");

	/* A repeater's status gets the sink's layout. */
	i915_xd_dp_test_setup(caps);
	i915_xd_dp_test_link(&link, 540000U, 2U, 0);
	i915_xd_dp_test_model.lanes = 2U;
	i915_xd_dp_test_model.cr_swing = 1U;
	i915_xd_dp_test_model.eq_preemph = 1U;
	i915_xd_dp_test_model.set[1][0] = 0x09U;
	i915_xd_dp_test_model.set[1][1] = 0x09U;
	i915_xd_dp_test_model.pattern[1] = 0x01U;
	error = drv_i915_xd_dp_read_status(&link.sink, 1U, status);
	i915_host_check(error == 0 && status[0] == 0x11U && status[2] == 0x01U && status[3] == 0x00U && status[4] == 0x55U &&
			i915_xd_dp_test_model.log[0].address == 0xf0030U && i915_xd_dp_test_model.log[0].size == 5U,
			"dp: a repeater's five bytes read into the sink's layout");
}

/*
 * The 5330's panel (DPCD 1.1, HBR, two lanes, no training interval) at
 * HBR x2: the DPCD accesses of the old modeset path's trace, in order.
 */
static void
i915_xd_dp_test_train_edp(void)
{
	static const uint8_t panel[15] = { 0x11U, 0x0aU, 0x02U, 0x41U, 0x00U, 0x00U, 0x01U, 0x00U, 0x02U };
	static const struct i915_xd_dp_test_access expected[] = {
		{ 0, 0x000U, 15U, 0x11U },
		{ 1, 0x107U, 2U, 0x00U },
		{ 1, 0x100U, 2U, 0x0aU },
		{ 1, 0x102U, 3U, 0x21U },
		{ 0, 0x202U, 6U, 0x00U },
		{ 1, 0x103U, 2U, 0x01U },
		{ 0, 0x202U, 6U, 0x11U },
		{ 1, 0x102U, 3U, 0x22U },
		{ 0, 0x202U, 6U, 0x77U },
	};
	struct i915_xd_dp_link link;
	unsigned index;
	int same;
	int error;

	/* A sink that recovers the clock at swing 1 and equalizes at once. */
	i915_xd_dp_test_setup(panel);
	i915_xd_dp_test_link(&link, 270000U, 2U, 0);
	link.preemph_max = 3U;
	i915_xd_dp_test_model.cr_swing = 1U;
	error = drv_i915_xd_dp_train(&link);

	/* The accesses in order. */
	same = i915_xd_dp_test_model.log_count == sizeof(expected) / sizeof(expected[0]);
	for (index = 0U; same && index < sizeof(expected) / sizeof(expected[0]); index++) {
		if (i915_xd_dp_test_model.log[index].write != expected[index].write ||
		    i915_xd_dp_test_model.log[index].address != expected[index].address ||
		    i915_xd_dp_test_model.log[index].size != expected[index].size ||
		    i915_xd_dp_test_model.log[index].first != expected[index].first)
			same = 0;
	}

	/* Checks the panel trains at HBR x2 with the old path's DPCD accesses. */
	i915_host_check(error == 0 && same, "dp: the panel trains at HBR x2 with the old path's DPCD accesses");

	/* The source: prepared, pattern 1 then 2 without scrambling, levels at the start and once after, idle once. */
	i915_host_check(i915_xd_dp_test_model.prepared == 1U && i915_xd_dp_test_model.idled == 1U &&
			i915_xd_dp_test_model.pattern_calls == 2U && i915_xd_dp_test_model.patterns[0] == 0x21U &&
			i915_xd_dp_test_model.patterns[1] == 0x22U && i915_xd_dp_test_model.levels_calls == 2U &&
			link.set[0] == 0x01U && link.set[1] == 0x01U && link.set[3] == 0x01U,
			"dp: the source's preparation, patterns, levels and idle pattern");

	/* The waits: 100 us for clock recovery, 400 us for equalization (no interval), twice and once. */
	i915_host_check(i915_xd_dp_test_model.slept_us == 2U * 100U + 400U && i915_xd_dp_test_model.errors == 0U,
			"dp: the default waits of a sink without a read interval");

	/* The end of the training: the sink's pattern off, then the port's. */
	drv_i915_xd_dp_train_end(&link);
	i915_host_check(i915_xd_dp_test_model.log[i915_xd_dp_test_model.log_count - 1U].address == 0x102U &&
			i915_xd_dp_test_model.log[i915_xd_dp_test_model.log_count - 1U].first == 0x00U &&
			i915_xd_dp_test_model.patterns[2] == 0x00U,
			"dp: the end turns the sink's and the port's pattern off");
}

/* Sinks that never recover the clock or never equalize: the training fails, and how. */
static void
i915_xd_dp_test_train_faults(void)
{
	static const uint8_t old_sink[15] = { 0x12U, 0x14U, 0x44U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x01U };
	static const uint8_t new_sink[15] = { 0x14U, 0x1eU, 0xc4U, 0x81U };
	struct i915_xd_dp_link link;
	int error;

	/* Clock recovery never: the same request five times ends it (pattern 1 only, four status reads). */
	i915_xd_dp_test_setup(old_sink);
	i915_xd_dp_test_link(&link, 540000U, 4U, 0);
	i915_xd_dp_test_model.cr_never = 1;
	i915_xd_dp_test_model.cr_swing = 2U;
	error = drv_i915_xd_dp_train(&link);
	i915_host_check(error == EIO && i915_xd_dp_test_model.pattern_calls == 1U && i915_xd_dp_test_model.idled == 1U &&
			i915_xd_dp_test_model.slept_us == 6U * 4000U,
			"dp: a clock never recovered fails when the same request came five times (4 ms interval)");

	/* Equalization never: pattern 3 (DPCD 1.2 with the bit), five tries, the failure is reported. */
	i915_xd_dp_test_setup(old_sink);
	i915_xd_dp_test_link(&link, 540000U, 4U, 0);
	i915_xd_dp_test_model.eq_never = 1;
	error = drv_i915_xd_dp_train(&link);
	i915_host_check(error == EIO && i915_xd_dp_test_model.pattern_calls == 2U && i915_xd_dp_test_model.patterns[1] == 0x23U &&
			i915_xd_dp_test_model.slept_us == 4000U + 5U * 4000U,
			"dp: a channel never equalized fails after five tries of pattern 3");

	/* A DPCD 1.4 sink with pattern 4 at HBR3: pattern 4 is sent scrambled, CR waits 100 us. */
	i915_xd_dp_test_setup(new_sink);
	i915_xd_dp_test_link(&link, 810000U, 4U, 0);
	error = drv_i915_xd_dp_train(&link);
	i915_host_check(error == 0 && i915_xd_dp_test_model.patterns[1] == 0x07U && i915_xd_dp_test_model.slept_us == 100U + 400U,
			"dp: a DPCD 1.4 sink trains with pattern 4");

	/* A DPCD 1.4 sink that never recovers the clock at the top levels gives up when the lanes are at the top. */
	i915_xd_dp_test_setup(new_sink);
	i915_xd_dp_test_link(&link, 810000U, 4U, 0);
	i915_xd_dp_test_model.cr_never = 1;
	i915_xd_dp_test_model.cr_swing = 3U;
	error = drv_i915_xd_dp_train(&link);
	i915_host_check(error == EIO && link.set[0] == (0x03U | 0x04U),
			"dp: the top levels end clock recovery (swing 3 marked as the top)");
}

/*
 * The levels a source sets: a source that sets all lanes alike takes the
 * highest request, and the swing is limited by the pre-emphasis.
 */
static void
i915_xd_dp_test_train_levels(void)
{
	static const uint8_t sink[15] = { 0x12U, 0x0aU, 0x04U, 0x00U };
	struct i915_xd_dp_link link;
	int error;

	/* Swing 3 with pre-emphasis 1 is cut to swing 2; the source's maximum swing 2 marks the top. */
	i915_xd_dp_test_setup(sink);
	i915_xd_dp_test_link(&link, 270000U, 4U, 0);
	link.per_lane = 0;
	link.swing_max = 2U;
	i915_xd_dp_test_model.cr_swing = 3U;
	i915_xd_dp_test_model.eq_preemph = 1U;
	i915_xd_dp_test_model.cr_never = 1;
	error = drv_i915_xd_dp_train(&link);
	i915_host_check(error == EIO && link.set[0] == (0x02U | 0x04U | 0x08U) && link.set[3] == link.set[0],
			"dp: swing 3 with pre-emphasis 1 is swing 2 at the source's top");
}

/*
 * Two repeaters in front of an external sink: they are switched to
 * transparent and then non-transparent mode, their capabilities read,
 * and trained from repeater 2 down before the sink.  A refused switch
 * trains no repeater.
 */
static void
i915_xd_dp_test_train_repeaters(void)
{
	static const uint8_t sink[15] = { 0x14U, 0x14U, 0xc4U, 0x81U };
	struct i915_xd_dp_link link;
	unsigned index;
	unsigned pattern_writes[3];
	int error;

	/* Two repeaters (count bit 6), revision 1.4, HBR2. */
	i915_xd_dp_test_setup(sink);
	i915_xd_dp_test_model.repeater[0] = 0x14U;
	i915_xd_dp_test_model.repeater[1] = 0x14U;
	i915_xd_dp_test_model.repeater[2] = 0x40U;
	i915_xd_dp_test_model.repeater_tx_cap = 0x01U;
	i915_xd_dp_test_link(&link, 540000U, 4U, 1);
	error = drv_i915_xd_dp_train(&link);

	/* The order of the first accesses: probe, caps, common caps, transparent, opaque, each repeater's caps, caps. */
	i915_host_check(error == 0 && link.repeaters == 2U &&
			i915_xd_dp_test_model.log[0].address == 0xf0000U && i915_xd_dp_test_model.log[1].address == 0x000U &&
			i915_xd_dp_test_model.log[2].address == 0xf0000U && i915_xd_dp_test_model.log[2].size == 8U &&
			i915_xd_dp_test_model.log[3].write == 1 && i915_xd_dp_test_model.log[3].first == 0x55U &&
			i915_xd_dp_test_model.log[4].write == 1 && i915_xd_dp_test_model.log[4].first == 0xaaU &&
			i915_xd_dp_test_model.log[5].address == 0xf0020U && i915_xd_dp_test_model.log[6].address == 0xf0070U &&
			i915_xd_dp_test_model.log[7].address == 0x000U &&
			i915_xd_dp_test_repeater_phy[0][1] == 0x01U,
			"dp: two repeaters are switched to non-transparent mode and their capabilities read");

	/* Repeater 2's pattern, then repeater 1's, then the sink's: the first write of each. */
	for (index = 0U; index < 3U; index++)
		pattern_writes[index] = 0U;
	for (index = 0U; index < i915_xd_dp_test_model.log_count; index++) {
		if (!i915_xd_dp_test_model.log[index].write)
			continue;
		if (i915_xd_dp_test_model.log[index].address == 0xf0060U && pattern_writes[2] == 0U)
			pattern_writes[2] = index;
		if (i915_xd_dp_test_model.log[index].address == 0xf0010U && pattern_writes[1] == 0U)
			pattern_writes[1] = index;
		if (i915_xd_dp_test_model.log[index].address == 0x102U && pattern_writes[0] == 0U)
			pattern_writes[0] = index;
	}

	/* Checks repeater 2, then repeater 1, then the sink are trained. */
	i915_host_check(pattern_writes[2] != 0U && pattern_writes[2] < pattern_writes[1] && pattern_writes[1] < pattern_writes[0],
			"dp: repeater 2, then repeater 1, then the sink are trained");

	/* The source drives only repeater 2: levels at its start and none for the others' starts. */
	i915_host_check(i915_xd_dp_test_model.levels_calls == 1U, "dp: the source sets levels only for the repeater it drives");

	/* A refused non-transparent mode goes back to transparent and trains only the sink. */
	i915_xd_dp_test_setup(sink);
	i915_xd_dp_test_model.repeater[0] = 0x14U;
	i915_xd_dp_test_model.repeater[2] = 0x80U;
	i915_xd_dp_test_model.refuse_opaque = 1;
	i915_xd_dp_test_link(&link, 540000U, 4U, 1);
	error = drv_i915_xd_dp_train(&link);
	i915_host_check(error == 0 && link.repeaters == 0U && i915_xd_dp_test_model.log[5].first == 0x55U &&
			i915_xd_dp_test_repeater_caps[2] == 0x00U,
			"dp: a refused switch leaves the repeater transparent and trains only the sink");

	/* A link that is up keeps transparent repeaters as they are: no mode write. */
	i915_xd_dp_test_setup(sink);
	i915_xd_dp_test_model.repeater[0] = 0x14U;
	i915_xd_dp_test_model.repeater[2] = 0x80U;
	i915_xd_dp_test_model.repeater[3] = 0x55U;
	i915_xd_dp_test_link(&link, 540000U, 4U, 1);
	link.active = 1;
	error = drv_i915_xd_dp_train(&link);
	i915_host_check(error == 0 && link.repeaters == 0U && i915_xd_dp_test_model.log[3].address == 0x000U,
			"dp: an active link keeps its transparent repeaters' mode");
}
