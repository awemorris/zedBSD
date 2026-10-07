/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * ws051-p004a: the host test of the external DP sink probe
 * (src/drivers/gpu/i915/display/dp-ext.c) over a fake sink: the 5330's
 * USB-C monitor (the DPCD a DP 1.4 HBR2 x4 sink reports and the EDID of
 * plan/ws051/tests/m3-5330-20261007), repeaters, branch devices with and
 * without a display behind them, a receiver that does not answer, the
 * lanes the FIA and the VBT allow, the downstream limits, and the protocol
 * converter's writes.
 *
 *   sh plan/ws051/tests/host-dpext.sh
 */

#include "dp-ext.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* The DPCD address space the fake sink covers (up to the repeaters' range and a little more). */
#define FAKE_DPCD_SIZE	0x100000u

/* How many accesses the fake records. */
#define FAKE_ACCESSES	256

/* The Linux errors the fake answers with. */
#define FAKE_ETIMEDOUT	110
#define FAKE_EIO	5

/*
 * The fake sink: its DPCD, its DDC, its EDID, and the accesses in order.
 */
struct fake {
	unsigned char dpcd[FAKE_DPCD_SIZE];
	unsigned char edid[4 * 128];
	int edid_blocks;
	unsigned edid_extensions;
	int ddc_answers;
	int fail_all;
	unsigned read_offset[FAKE_ACCESSES];
	unsigned read_size[FAKE_ACCESSES];
	unsigned reads;
	unsigned write_offset[FAKE_ACCESSES];
	unsigned char write_value[FAKE_ACCESSES];
	unsigned writes;
	unsigned ddc_probes;
	unsigned edid_reads;
};

/* The fake every test case resets; static, as the DPCD is a megabyte. */
static struct fake fake;

/* How many checks failed and ran. */
static int failures;
static int checks;

/* The EDID of the 5330's USB-C monitor, read from the capture. */
static unsigned char monitor_edid[256];

static void check(int cond, const char *what);
static void fake_reset(void);
static long fake_dpcd_read(void *ctx, unsigned offset, uint8_t *buffer, size_t size);
static long fake_dpcd_write(void *ctx, unsigned offset, const uint8_t *buffer, size_t size);
static int fake_dpcd_probe(void *ctx, unsigned offset);
static int fake_read_caps(void *ctx, uint8_t dpcd[I915_DP_EXT_DPCD_SIZE]);
static int fake_ddc_probe(void *ctx);
static int fake_edid_read(void *ctx, uint8_t *buffer, unsigned max_blocks, unsigned *extensions);
static void fake_log(void *ctx, const char *format, ...) __attribute__((format(printf, 2, 3)));
static void env_init(struct i915_dp_ext_env *env);
static void source_5330(struct i915_dp_ext_source *source);
static void sink_monitor(void);
static int load_edid(const char *path);
static void test_monitor(void);
static void test_lanes(void);
static void test_lttpr(void);
static void test_lttpr_bytewise(void);
static void test_dead_receiver(void);
static void test_adapter_without_display(void);
static void test_adapter_with_hdmi(void);
static void test_branch_without_hpd(void);
static void test_rates(void);
static void test_converter(void);
static void test_short_pulse(void);
static void test_fallback(void);
static void test_link_retrain(void);

/*
 * Runs every case; the monitor's EDID comes from the capture named on the
 * command line.
 */
int
main(
	int argc,
	char **argv)
{
	int loaded;

	/* Reads the monitor's EDID. */
	if (argc != 2) {
		fprintf(stderr, "usage: host-dpext DP-2.edid\n");
		return 2;
	}

	loaded = load_edid(argv[1]);
	if (!loaded) {
		fprintf(stderr, "host-dpext: cannot read %s\n", argv[1]);
		return 2;
	}

	/* The cases. */
	test_monitor();
	test_lanes();
	test_lttpr();
	test_lttpr_bytewise();
	test_dead_receiver();
	test_adapter_without_display();
	test_adapter_with_hdmi();
	test_branch_without_hpd();
	test_rates();
	test_converter();
	test_short_pulse();
	test_fallback();
	test_link_retrain();

	/* The verdict. */
	printf("host-dpext: %d checks, %d failures\n", checks, failures);
	if (failures != 0)
		return 1;

	return 0;
}

/* Counts a check and reports a failed one. */
static void
check(
	int cond,
	const char *what)
{
	checks++;
	if (cond)
		return;

	failures++;
	printf("FAIL: %s\n", what);
}

/* Starts the fake sink empty: every DPCD byte 0, no DDC, no EDID. */
static void
fake_reset(void)
{
	memset(&fake, 0, sizeof(fake));
}

/* Reads DPCD bytes of the fake, recording the access. */
static long
fake_dpcd_read(
	void *ctx,
	unsigned offset,
	uint8_t *buffer,
	size_t size)
{
	(void)ctx;

	if (fake.reads < FAKE_ACCESSES) {
		fake.read_offset[fake.reads] = offset;
		fake.read_size[fake.reads] = (unsigned)size;
	}
	fake.reads++;

	if (fake.fail_all)
		return -FAKE_ETIMEDOUT;
	if (offset + size > FAKE_DPCD_SIZE)
		return -FAKE_EIO;

	memcpy(buffer, &fake.dpcd[offset], size);
	return (long)size;
}

/* Writes DPCD bytes of the fake, recording each byte. */
static long
fake_dpcd_write(
	void *ctx,
	unsigned offset,
	const uint8_t *buffer,
	size_t size)
{
	size_t i;

	(void)ctx;

	if (fake.fail_all)
		return -FAKE_ETIMEDOUT;

	for (i = 0; i < size; i++) {
		if (fake.writes < FAKE_ACCESSES) {
			fake.write_offset[fake.writes] = offset + (unsigned)i;
			fake.write_value[fake.writes] = buffer[i];
		}
		fake.writes++;
		fake.dpcd[offset + i] = buffer[i];
	}

	return (long)size;
}

/* Wakes the fake: a one-byte read. */
static int
fake_dpcd_probe(
	void *ctx,
	unsigned offset)
{
	uint8_t byte;
	long read;

	read = fake_dpcd_read(ctx, offset, &byte, 1);
	if (read < 0)
		return (int)read;

	return 0;
}

/* Reads the fake's receiver capabilities; a zero revision is an I/O error. */
static int
fake_read_caps(
	void *ctx,
	uint8_t dpcd[I915_DP_EXT_DPCD_SIZE])
{
	long read;

	read = fake_dpcd_read(ctx, 0, dpcd, I915_DP_EXT_DPCD_SIZE);
	if (read < 0)
		return (int)read;
	if (dpcd[0] == 0)
		return -FAKE_EIO;

	return 0;
}

/* Asks the fake's DDC. */
static int
fake_ddc_probe(
	void *ctx)
{
	(void)ctx;

	fake.ddc_probes++;
	return fake.ddc_answers;
}

/* Reads the fake's EDID. */
static int
fake_edid_read(
	void *ctx,
	uint8_t *buffer,
	unsigned max_blocks,
	unsigned *extensions)
{
	unsigned blocks;

	(void)ctx;

	fake.edid_reads++;
	if (fake.edid_blocks <= 0)
		return -FAKE_EIO;

	blocks = (unsigned)fake.edid_blocks;
	if (blocks > max_blocks)
		blocks = max_blocks;

	memcpy(buffer, fake.edid, blocks * 128u);
	*extensions = fake.edid_extensions;
	return (int)blocks;
}

/* Prints the probe's log lines. */
static void
fake_log(
	void *ctx,
	const char *format,
	...)
{
	va_list ap;

	(void)ctx;

	printf("  log: ");
	va_start(ap, format);
	vprintf(format, ap);
	va_end(ap);
}

/* Binds the probe to the fake. */
static void
env_init(
	struct i915_dp_ext_env *env)
{
	memset(env, 0, sizeof(*env));
	env->dpcd_read = fake_dpcd_read;
	env->dpcd_write = fake_dpcd_write;
	env->dpcd_probe = fake_dpcd_probe;
	env->read_caps = fake_read_caps;
	env->ddc_probe = fake_ddc_probe;
	env->edid_read = fake_edid_read;
	env->log = fake_log;
}

/* What the 5330's TC2 offers: HBR3 (the platform and the VBT), 4 lanes, all 4 from the FIA (pin C). */
static void
source_5330(
	struct i915_dp_ext_source *source)
{
	memset(source, 0, sizeof(*source));
	source->max_rate = 810000;
	source->vbt_max_rate = 810000;
	source->max_lanes = 4;
	source->vbt_max_lanes = 0;
	source->fia_lanes = 4;
}

/*
 * The 5330's USB-C monitor (DP-2): a DP 1.4 sink of HBR2 and 4 lanes with
 * enhanced framing, no repeater, not a branch, and its two EDID blocks.
 */
static void
sink_monitor(void)
{
	fake_reset();
	fake.dpcd[0x000] = 0x14;
	fake.dpcd[0x001] = 0x14;
	fake.dpcd[0x002] = 0x84;
	fake.dpcd[0x003] = 0x01;
	fake.dpcd[0x005] = 0x00;
	memcpy(&fake.dpcd[0x400], "\x00\x1a\x2b" "S123  " "\x10\x01\x02", 12);
	memcpy(fake.edid, monitor_edid, sizeof(monitor_edid));
	fake.edid_blocks = 2;
	fake.edid_extensions = 1;
}

/* Reads the 256-byte EDID capture. */
static int
load_edid(
	const char *path)
{
	FILE *file;
	size_t read;

	file = fopen(path, "rb");
	if (file == NULL)
		return 0;

	read = fread(monitor_edid, 1, sizeof(monitor_edid), file);
	fclose(file);
	if (read != sizeof(monitor_edid))
		return 0;

	return 1;
}

/*
 * The 5330's monitor: connected with its EDID, HBR2 x4 shared, the first
 * access the wake read at the repeaters' range, no branch access, no DDC
 * poke, and no write.
 */
static void
test_monitor(void)
{
	struct i915_dp_ext_env env;
	struct i915_dp_ext_source source;
	struct i915_dp_ext_sink sink;

	printf("monitor (5330 DP-2)\n");
	env_init(&env);
	source_5330(&source);
	sink_monitor();
	drv_i915_dp_ext_detect(&env, &source, &sink);
	drv_i915_dp_ext_log(&env, &sink, "DP-2");

	check(sink.status == I915_DP_EXT_CONNECTED, "monitor: connected");
	check(sink.step == I915_DP_EXT_STEP_DONE, "monitor: probe done");
	check(fake.read_offset[0] == 0xf0000u && fake.read_size[0] == 1u, "monitor: the wake read is at the repeaters' range");
	check(sink.dpcd_valid && sink.dpcd[0] == 0x14, "monitor: DPCD 1.4");
	check(!sink.lttpr_valid && sink.lttpr_count == 0, "monitor: no repeater");
	check(!sink.branch, "monitor: not a branch");
	check(!sink.has_sink_count, "monitor: no sink count read");
	check(sink.desc_valid && sink.desc.oui[1] == 0x1a && memcmp(sink.desc.device_id, "S123  ", 6) == 0, "monitor: the sink's identification from 0x400");
	check(sink.max_rate == 540000, "monitor: HBR2 is the highest shared rate (debugfs max_link_rate 540000)");
	check(sink.num_sink_rates == 3, "monitor: the sink offers RBR, HBR, HBR2");
	check(sink.num_source_rates == 8, "monitor: the source offers 8 rates up to HBR3");
	check(sink.num_common_rates == 3 && sink.common_rates[0] == 162000 && sink.common_rates[2] == 540000, "monitor: RBR, HBR, HBR2 shared");
	check(sink.max_sink_lanes == 4 && sink.max_lanes == 4, "monitor: 4 lanes (debugfs max_lane_count 4)");
	check(sink.edid_blocks == 2 && sink.edid_extensions == 1, "monitor: two EDID blocks");
	check(memcmp(sink.edid, monitor_edid, sizeof(monitor_edid)) == 0, "monitor: the EDID bytes");
	check(sink.has_hdmi_sink == 0, "monitor: its CTA extension has no HDMI block");
	check(sink.max_tmds_clock_khz == 0 && sink.min_tmds_clock_khz == 0 && sink.max_dotclock_khz == 0 && sink.max_bpc == 0, "monitor: no downstream limits");
	check(fake.ddc_probes == 0, "monitor: no DDC poke for a sink");
	check(fake.writes == 0, "monitor: the probe writes nothing");
	check(drv_i915_dp_ext_mode_valid(&sink, 600000) == I915_DP_EXT_MODE_OK, "monitor: no branch limit on the clock");
}

/* The lanes: the FIA's two (pin D), the VBT's limit, an invalid sink count. */
static void
test_lanes(void)
{
	struct i915_dp_ext_env env;
	struct i915_dp_ext_source source;
	struct i915_dp_ext_sink sink;

	printf("lanes\n");
	env_init(&env);

	source_5330(&source);
	source.fia_lanes = 2;
	sink_monitor();
	drv_i915_dp_ext_detect(&env, &source, &sink);
	check(sink.max_lanes == 2 && sink.max_sink_lanes == 4, "lanes: the FIA's two lanes (pin D) limit a 4-lane sink");

	source_5330(&source);
	source.vbt_max_lanes = 1;
	sink_monitor();
	drv_i915_dp_ext_detect(&env, &source, &sink);
	check(sink.max_lanes == 1, "lanes: the VBT's limit");

	source_5330(&source);
	sink_monitor();
	fake.dpcd[0x002] = 0x83;
	drv_i915_dp_ext_detect(&env, &source, &sink);
	check(sink.max_sink_lanes == 1 && sink.max_lanes == 1, "lanes: a sink count of 3 is invalid and becomes 1");
}

/* A repeater in front of the monitor: it limits the rate and lanes, and is counted. */
static void
test_lttpr(void)
{
	struct i915_dp_ext_env env;
	struct i915_dp_ext_source source;
	struct i915_dp_ext_sink sink;
	unsigned i;
	unsigned lttpr_reads;

	printf("repeater\n");
	env_init(&env);
	source_5330(&source);
	sink_monitor();
	fake.dpcd[0xf0000] = 0x14;
	fake.dpcd[0xf0001] = 0x0a;
	fake.dpcd[0xf0002] = 0x80;
	fake.dpcd[0xf0004] = 0x02;
	drv_i915_dp_ext_detect(&env, &source, &sink);

	lttpr_reads = 0;
	for (i = 0; i < fake.reads && i < FAKE_ACCESSES; i++) {
		if (fake.read_offset[i] == 0xf0000u && fake.read_size[i] == 8u)
			lttpr_reads++;
	}

	check(sink.status == I915_DP_EXT_CONNECTED, "repeater: connected");
	check(sink.lttpr_valid && sink.lttpr_count == 1, "repeater: one repeater (0x80)");
	check(lttpr_reads == 1, "repeater: DPCD 1.4 reads the capabilities as one block");
	check(sink.max_rate == 270000, "repeater: its HBR limits the rate");
	check(sink.max_lanes == 2, "repeater: its 2 lanes limit the link");

	sink_monitor();
	fake.dpcd[0xf0000] = 0x13;
	fake.dpcd[0xf0001] = 0x0a;
	fake.dpcd[0xf0002] = 0x80;
	drv_i915_dp_ext_detect(&env, &source, &sink);
	check(!sink.lttpr_valid && sink.max_rate == 540000, "repeater: capabilities below revision 1.4 are dropped");

	sink_monitor();
	fake.dpcd[0xf0000] = 0x14;
	fake.dpcd[0xf0002] = 0x81;
	drv_i915_dp_ext_detect(&env, &source, &sink);
	check(sink.lttpr_valid && sink.lttpr_count == 0, "repeater: a count field with two bits counts none");
}

/* A DPCD 1.2 sink: the repeaters' range is read a byte at a time. */
static void
test_lttpr_bytewise(void)
{
	struct i915_dp_ext_env env;
	struct i915_dp_ext_source source;
	struct i915_dp_ext_sink sink;
	unsigned i;
	unsigned byte_reads;

	printf("repeater, DPCD 1.2\n");
	env_init(&env);
	source_5330(&source);
	sink_monitor();
	fake.dpcd[0x000] = 0x12;
	drv_i915_dp_ext_detect(&env, &source, &sink);

	byte_reads = 0;
	for (i = 0; i < fake.reads && i < FAKE_ACCESSES; i++) {
		if (fake.read_offset[i] >= 0xf0000u && fake.read_offset[i] < 0xf0008u && fake.read_size[i] == 1u)
			byte_reads++;
	}

	check(byte_reads == 9, "repeater, DPCD 1.2: the wake read and 8 single-byte reads");
	check(sink.status == I915_DP_EXT_CONNECTED, "repeater, DPCD 1.2: connected");
}

/* A receiver that does not answer: disconnected at the capabilities, with the AUX error. */
static void
test_dead_receiver(void)
{
	struct i915_dp_ext_env env;
	struct i915_dp_ext_source source;
	struct i915_dp_ext_sink sink;

	printf("dead receiver\n");
	env_init(&env);
	source_5330(&source);
	sink_monitor();
	fake.fail_all = 1;
	drv_i915_dp_ext_detect(&env, &source, &sink);
	drv_i915_dp_ext_log(&env, &sink, "DP-2");

	check(sink.status == I915_DP_EXT_DISCONNECTED, "dead receiver: disconnected");
	check(sink.step == I915_DP_EXT_STEP_CAPS && sink.error == -FAKE_ETIMEDOUT, "dead receiver: stopped at the capabilities with -ETIMEDOUT");
	check(fake.edid_reads == 0, "dead receiver: no EDID read");

	sink_monitor();
	fake.dpcd[0x000] = 0x00;
	drv_i915_dp_ext_detect(&env, &source, &sink);
	check(sink.status == I915_DP_EXT_DISCONNECTED && sink.step == I915_DP_EXT_STEP_CAPS, "zero revision: disconnected at the capabilities");
}

/*
 * A USB-C to HDMI adapter (DPCD 1.4 branch, detailed capabilities, one
 * HDMI port with hot plug detect) with nothing behind it: sink count 0 is
 * disconnected, no EDID read.
 */
static void
test_adapter_without_display(void)
{
	struct i915_dp_ext_env env;
	struct i915_dp_ext_source source;
	struct i915_dp_ext_sink sink;

	printf("adapter without a display\n");
	env_init(&env);
	source_5330(&source);
	fake_reset();
	fake.dpcd[0x000] = 0x14;
	fake.dpcd[0x001] = 0x14;
	fake.dpcd[0x002] = 0x84;
	fake.dpcd[0x005] = 0x11;
	fake.dpcd[0x007] = 0x01;
	fake.dpcd[0x080] = 0x0b;
	fake.dpcd[0x081] = 0x78;
	fake.dpcd[0x200] = 0x00;
	memcpy(&fake.dpcd[0x500], "\x00\x1c\xf8" "175\0\0\0" "\x10\x07\x01", 12);
	drv_i915_dp_ext_detect(&env, &source, &sink);
	drv_i915_dp_ext_log(&env, &sink, "DP-1");

	check(sink.branch, "adapter: a branch");
	check(sink.desc_valid && sink.desc.oui[2] == 0xf8, "adapter: the branch's identification from 0x500");
	check(sink.has_sink_count && sink.sink_count == 0, "adapter: sink count 0");
	check(sink.status == I915_DP_EXT_DISCONNECTED && sink.step == I915_DP_EXT_STEP_SINK_COUNT, "adapter: no display is disconnected (H7)");
	check(fake.edid_reads == 0 && fake.ddc_probes == 0, "adapter: neither EDID nor DDC is tried");
}

/*
 * The same adapter with an HDMI TV behind it: connected by its sink count,
 * the EDID's HDMI block found, the port's 300 MHz TMDS limit and 12 bpc.
 */
static void
test_adapter_with_hdmi(void)
{
	struct i915_dp_ext_env env;
	struct i915_dp_ext_source source;
	struct i915_dp_ext_sink sink;
	unsigned char *ext;
	int error;
	unsigned i;
	int saw0;
	int saw1;
	int saw2;

	printf("adapter with an HDMI display\n");
	env_init(&env);
	source_5330(&source);
	fake_reset();
	fake.dpcd[0x000] = 0x14;
	fake.dpcd[0x001] = 0x14;
	fake.dpcd[0x002] = 0x84;
	fake.dpcd[0x005] = 0x11;
	fake.dpcd[0x007] = 0x01;
	fake.dpcd[0x080] = 0x0b;
	fake.dpcd[0x081] = 0x78;
	fake.dpcd[0x082] = 0x02;
	fake.dpcd[0x200] = 0x41;
	fake.dpcd[0x3052] = 0x7f;
	memcpy(fake.edid, monitor_edid, 128);
	ext = &fake.edid[128];
	ext[0] = 0x02;
	ext[1] = 0x03;
	ext[2] = 4 + 6 + 3;
	ext[4] = 0x65;
	ext[5] = 0x03;
	ext[6] = 0x0c;
	ext[7] = 0x00;
	ext[8] = 0x10;
	ext[9] = 0x00;
	ext[10] = 0x22;
	ext[11] = 0x09;
	ext[12] = 0x07;
	fake.edid_blocks = 2;
	fake.edid_extensions = 1;
	drv_i915_dp_ext_detect(&env, &source, &sink);
	drv_i915_dp_ext_log(&env, &sink, "DP-1");

	check(sink.status == I915_DP_EXT_CONNECTED, "adapter + HDMI: connected");
	check(sink.sink_count == 1, "adapter + HDMI: sink count 1 (bit 6 of the field is not part of the count)");
	check(fake.ddc_probes == 0, "adapter + HDMI: the port's hot plug detect makes the count enough");
	check(sink.has_hdmi_sink == 1, "adapter + HDMI: the HDMI vendor block");
	check(sink.max_tmds_clock_khz == 300000 && sink.min_tmds_clock_khz == 25000, "adapter + HDMI: TMDS 25-300 MHz from the detailed port");
	check(sink.max_bpc == 12, "adapter + HDMI: 12 bpc");
	check(drv_i915_dp_ext_mode_valid(&sink, 297000) == I915_DP_EXT_MODE_OK, "adapter + HDMI: 4K30 (297 MHz) passes");
	check(drv_i915_dp_ext_mode_valid(&sink, 594000) == I915_DP_EXT_MODE_CLOCK_HIGH, "adapter + HDMI: 4K60 (594 MHz) is above the port");
	check(drv_i915_dp_ext_mode_valid(&sink, 20000) == I915_DP_EXT_MODE_CLOCK_LOW, "adapter + HDMI: 20 MHz is below the port");

	error = drv_i915_dp_ext_configure_converter(&env, &sink);
	saw0 = 0;
	saw1 = 0;
	saw2 = 0;
	for (i = 0; i < fake.writes && i < FAKE_ACCESSES; i++) {
		if (fake.write_offset[i] == 0x3050u && fake.write_value[i] == 0x01)
			saw0 = 1;
		if (fake.write_offset[i] == 0x3051u && fake.write_value[i] == 0x00)
			saw1 = 1;
		if (fake.write_offset[i] == 0x3052u && fake.write_value[i] == 0x0f)
			saw2 = 1;
	}

	check(error == 0, "converter: configured");
	check(fake.writes == 3, "converter: three writes");
	check(saw0, "converter: HDMI output selected (0x3050 = 1)");
	check(saw1, "converter: no 4:2:0 conversion (0x3051 = 0)");
	check(saw2, "converter: RGB to YCbCr off, the other bits kept (0x3052 0x7f -> 0x0f)");
}

/*
 * A branch whose port does not detect its display: the DDC decides; a VGA
 * port without an answer is unknown; a DP port without one is ignored.
 */
static void
test_branch_without_hpd(void)
{
	struct i915_dp_ext_env env;
	struct i915_dp_ext_source source;
	struct i915_dp_ext_sink sink;

	printf("branch without hot plug detect\n");
	env_init(&env);
	source_5330(&source);

	fake_reset();
	fake.dpcd[0x000] = 0x12;
	fake.dpcd[0x001] = 0x0a;
	fake.dpcd[0x002] = 0x82;
	fake.dpcd[0x005] = 0x01;
	fake.dpcd[0x007] = 0x01;
	fake.dpcd[0x080] = 0x03;
	fake.dpcd[0x200] = 0x01;
	fake.ddc_answers = 1;
	drv_i915_dp_ext_detect(&env, &source, &sink);
	check(sink.status == I915_DP_EXT_CONNECTED && fake.ddc_probes == 1, "branch: a DDC that answers is a display");
	check(sink.max_tmds_clock_khz == 300000 && sink.max_bpc == 8, "branch: an HDMI port without details is 300 MHz, 8 bpc");
	check(sink.max_rate == 270000 && sink.max_lanes == 2, "branch: HBR x2");

	fake_reset();
	fake.dpcd[0x000] = 0x12;
	fake.dpcd[0x001] = 0x0a;
	fake.dpcd[0x002] = 0x82;
	fake.dpcd[0x005] = 0x01;
	fake.dpcd[0x007] = 0x01;
	fake.dpcd[0x080] = 0x01;
	fake.dpcd[0x200] = 0x01;
	drv_i915_dp_ext_detect(&env, &source, &sink);
	check(sink.status == I915_DP_EXT_UNKNOWN, "branch: a silent VGA port is unknown");

	fake_reset();
	fake.dpcd[0x000] = 0x12;
	fake.dpcd[0x001] = 0x0a;
	fake.dpcd[0x002] = 0x82;
	fake.dpcd[0x005] = 0x01;
	fake.dpcd[0x007] = 0x01;
	fake.dpcd[0x080] = 0x00;
	fake.dpcd[0x200] = 0x01;
	drv_i915_dp_ext_detect(&env, &source, &sink);
	check(sink.status == I915_DP_EXT_DISCONNECTED && sink.step == I915_DP_EXT_STEP_BRANCH, "branch: a silent DP port is ignored");
	check(fake.edid_reads == 0, "branch: no EDID read after it is ignored");

	fake_reset();
	fake.dpcd[0x000] = 0x10;
	fake.dpcd[0x001] = 0x06;
	fake.dpcd[0x002] = 0x81;
	fake.dpcd[0x005] = 0x05;
	fake.edid_blocks = 1;
	memcpy(fake.edid, monitor_edid, 128);
	drv_i915_dp_ext_detect(&env, &source, &sink);
	check(!sink.has_sink_count, "DPCD 1.0 branch: no sink count");
	check(fake.ddc_probes == 1, "DPCD 1.0 branch: the DDC is asked");
	check(sink.status == I915_DP_EXT_DISCONNECTED, "DPCD 1.0 branch: a silent TMDS port is ignored");
	check(sink.max_tmds_clock_khz == 0, "DPCD 1.0 branch: no limits once ignored");
}

/* The rates: the VBT's limit, a sink's invalid rate code, the source's limit. */
static void
test_rates(void)
{
	struct i915_dp_ext_env env;
	struct i915_dp_ext_source source;
	struct i915_dp_ext_sink sink;

	printf("rates\n");
	env_init(&env);

	source_5330(&source);
	source.vbt_max_rate = 270000;
	sink_monitor();
	drv_i915_dp_ext_detect(&env, &source, &sink);
	check(sink.num_source_rates == 3 && sink.max_rate == 270000, "rates: the VBT's HBR limit");

	source_5330(&source);
	sink_monitor();
	fake.dpcd[0x001] = 0x1e;
	drv_i915_dp_ext_detect(&env, &source, &sink);
	check(sink.max_rate == 810000 && sink.num_common_rates == 4, "rates: an HBR3 sink shares HBR3");

	source_5330(&source);
	sink_monitor();
	fake.dpcd[0x001] = 0x00;
	drv_i915_dp_ext_detect(&env, &source, &sink);
	check(sink.num_sink_rates == 1 && sink.sink_rates[0] == 162000 && sink.max_rate == 162000, "rates: a sink with no rate is given RBR");

	source_5330(&source);
	source.max_rate = 0;
	source.vbt_max_rate = 0;
	sink_monitor();
	drv_i915_dp_ext_detect(&env, &source, &sink);
	check(sink.num_source_rates == 8, "rates: no limit offers the whole table");
}

/* The converter of a sink that is no branch, or of a DPCD 1.2 branch, is left alone. */
static void
test_converter(void)
{
	struct i915_dp_ext_env env;
	struct i915_dp_ext_source source;
	struct i915_dp_ext_sink sink;
	int error;

	printf("converter\n");
	env_init(&env);
	source_5330(&source);
	sink_monitor();
	drv_i915_dp_ext_detect(&env, &source, &sink);
	error = drv_i915_dp_ext_configure_converter(&env, &sink);
	check(error == 0 && fake.writes == 0, "converter: a sink is no converter");

	fake_reset();
	fake.dpcd[0x000] = 0x12;
	fake.dpcd[0x001] = 0x0a;
	fake.dpcd[0x002] = 0x82;
	fake.dpcd[0x005] = 0x01;
	fake.dpcd[0x007] = 0x01;
	fake.dpcd[0x080] = 0x0b;
	fake.dpcd[0x200] = 0x01;
	fake.edid_blocks = 1;
	memcpy(fake.edid, monitor_edid, 128);
	drv_i915_dp_ext_detect(&env, &source, &sink);
	error = drv_i915_dp_ext_configure_converter(&env, &sink);
	check(sink.status == I915_DP_EXT_CONNECTED, "converter: a DPCD 1.2 branch with a display");
	check(error == 0 && fake.writes == 0, "converter: a DPCD 1.2 branch has no converter controls");

	fake.fail_all = 1;
	sink.dpcd[0] = 0x14;
	error = drv_i915_dp_ext_configure_converter(&env, &sink);
	check(error == -FAKE_ETIMEDOUT, "converter: the first failed write is reported");
}

/*
 * ws051-p005a: a short pulse (IRQ_HPD) of a connected sink: handled when
 * nothing changed, the service interrupts acknowledged; a detection is
 * asked for changed capabilities, a changed sink count, a failed read or a
 * sink the last probe did not find.
 */
static void
test_short_pulse(void)
{
	struct i915_dp_ext_env env;
	struct i915_dp_ext_source source;
	struct i915_dp_ext_sink sink;
	int handled;

	printf("short pulse\n");
	env_init(&env);
	source_5330(&source);

	sink_monitor();
	drv_i915_dp_ext_detect(&env, &source, &sink);
	fake.writes = 0;
	handled = drv_i915_dp_ext_short_pulse(&env, &sink);
	check(handled == 1 && fake.writes == 0, "short pulse: nothing changed, nothing raised: handled");

	fake.dpcd[0x201] = 0x40;
	fake.dpcd[0x2005] = 0x01;
	handled = drv_i915_dp_ext_short_pulse(&env, &sink);
	check(handled == 1, "short pulse: service interrupts raised: handled");
	check(fake.writes == 2 && fake.write_offset[0] == 0x201u && fake.write_value[0] == 0x40 && fake.write_offset[1] == 0x2005u && fake.write_value[1] == 0x01, "short pulse: both vectors acknowledged by writing them back");

	fake.dpcd[0x001] = 0x0a;
	handled = drv_i915_dp_ext_short_pulse(&env, &sink);
	check(handled == 0, "short pulse: changed capabilities ask for a detection");

	sink_monitor();
	drv_i915_dp_ext_detect(&env, &source, &sink);
	fake.fail_all = 1;
	handled = drv_i915_dp_ext_short_pulse(&env, &sink);
	check(handled == 0, "short pulse: a failed read asks for a detection");

	fake_reset();
	fake.dpcd[0x000] = 0x14;
	fake.dpcd[0x001] = 0x14;
	fake.dpcd[0x002] = 0x84;
	fake.dpcd[0x005] = 0x11;
	fake.dpcd[0x007] = 0x01;
	fake.dpcd[0x080] = 0x0b;
	fake.dpcd[0x200] = 0x01;
	fake.edid_blocks = 1;
	memcpy(fake.edid, monitor_edid, 128);
	drv_i915_dp_ext_detect(&env, &source, &sink);
	check(sink.status == I915_DP_EXT_CONNECTED, "short pulse: the adapter with a display is connected");
	handled = drv_i915_dp_ext_short_pulse(&env, &sink);
	check(handled == 1, "short pulse: the adapter's count is the same: handled");
	fake.dpcd[0x200] = 0x00;
	handled = drv_i915_dp_ext_short_pulse(&env, &sink);
	check(handled == 0, "short pulse: the display behind the adapter went: a detection");

	drv_i915_dp_ext_forget(&sink);
	handled = drv_i915_dp_ext_short_pulse(&env, &sink);
	check(handled == 0, "short pulse: a sink not found by the last probe is detected afresh");
}

/*
 * The link-training fallback (ws051-p004b) walks the 5330 monitor's links
 * as Linux v6.8.12 does: HBR2 x4 to HBR x4 to RBR x4, then HBR2 x2 ... RBR
 * x2, HBR2 x1 ... RBR x1, and nothing after one lane at RBR; a rate the
 * sink does not share halves the lanes.
 */
static void
test_fallback(void)
{
	static const int want_rate[] = { 270000, 162000, 540000, 270000, 162000, 540000, 270000, 162000 };
	static const int want_lanes[] = { 4, 4, 2, 2, 2, 1, 1, 1 };
	struct i915_dp_ext_sink sink;
	int rate;
	int lanes;
	int max_rate;
	int max_lanes;
	int steps;
	int lowered;
	int walk_ok;

	/* The monitor's shared rates: RBR, HBR, HBR2. */
	memset(&sink, 0, sizeof(sink));
	sink.common_rates[0] = 162000;
	sink.common_rates[1] = 270000;
	sink.common_rates[2] = 540000;
	sink.num_common_rates = 3;

	/* Walks every fallback from HBR2 x4 until nothing is left. */
	rate = 540000;
	lanes = 4;
	steps = 0;
	walk_ok = 1;
	for (;;) {
		lowered = drv_i915_dp_ext_fallback_values(&sink, rate, lanes, &max_rate, &max_lanes);
		if (!lowered)
			break;

		/* Each step is the next one of the Linux walk. */
		if (steps >= 8 || max_rate != want_rate[steps] || max_lanes != want_lanes[steps])
			walk_ok = 0;
		steps++;
		rate = max_rate;
		lanes = max_lanes;
		if (steps > 16)
			break;
	}
	check(walk_ok && steps == 8, "fallback: HBR2 x4 down to RBR x1 in the Linux order, 8 steps");
	check(rate == 162000 && lanes == 1, "fallback: one lane at RBR is the last link");

	/* A rate the sink does not share halves the lanes at the highest rate. */
	lowered = drv_i915_dp_ext_fallback_values(&sink, 810000, 4, &max_rate, &max_lanes);
	check(lowered && max_rate == 540000 && max_lanes == 2, "fallback: an unshared rate halves the lanes at HBR2");
}

/*
 * ws051-p005b: whether a trained link must be trained again, from the
 * sink's link status (DPCD 0x202 onwards): all lanes done and aligned, a
 * lane of the link that lost its lock, a lane past the link's count that
 * did, lost alignment, a status that cannot be read, and lane counts out
 * of range.
 */
static void
test_link_retrain(void)
{
	struct i915_dp_ext_env env;
	int needs;

	/* Four lanes with clock recovery, equalisation and symbol lock, aligned: nothing to do. */
	fake_reset();
	env_init(&env);
	fake.dpcd[0x202] = 0x77;
	fake.dpcd[0x203] = 0x77;
	fake.dpcd[0x204] = 0x01;
	needs = drv_i915_dp_ext_link_needs_retrain(&env, 4);
	check(needs == 0, "retrain: a trained four-lane link needs nothing");
	check(fake.reads == 1 && fake.read_offset[0] == 0x202 && fake.read_size[0] == 6, "retrain: the status is one read of 6 bytes at 0x202");

	/* Lane 3 lost its symbol lock: a four-lane link needs it, a two-lane link does not. */
	fake.dpcd[0x203] = 0x37;
	needs = drv_i915_dp_ext_link_needs_retrain(&env, 4);
	check(needs == 1, "retrain: lane 3 without symbol lock on four lanes");
	needs = drv_i915_dp_ext_link_needs_retrain(&env, 2);
	check(needs == 0, "retrain: lane 3 is not one of two lanes");

	/* Lane 1 lost clock recovery: a two-lane link needs it, a one-lane link does not. */
	fake.dpcd[0x202] = 0x67;
	needs = drv_i915_dp_ext_link_needs_retrain(&env, 2);
	check(needs == 1, "retrain: lane 1 without clock recovery on two lanes");
	needs = drv_i915_dp_ext_link_needs_retrain(&env, 1);
	check(needs == 0, "retrain: lane 1 is not the one lane");

	/* The lanes lost their alignment. */
	fake.dpcd[0x202] = 0x77;
	fake.dpcd[0x203] = 0x77;
	fake.dpcd[0x204] = 0x00;
	needs = drv_i915_dp_ext_link_needs_retrain(&env, 1);
	check(needs == 1, "retrain: lost inter-lane alignment");

	/* A status that cannot be read asks for nothing. */
	fake.fail_all = 1;
	needs = drv_i915_dp_ext_link_needs_retrain(&env, 4);
	check(needs == 0, "retrain: an unreadable status asks for nothing");
	fake.fail_all = 0;

	/* Lane counts out of 1 to 4 ask for nothing and read nothing. */
	fake.reads = 0;
	needs = drv_i915_dp_ext_link_needs_retrain(&env, 0);
	check(needs == 0, "retrain: lane count 0");
	needs = drv_i915_dp_ext_link_needs_retrain(&env, 5);
	check(needs == 0, "retrain: lane count 5");
	check(fake.reads == 0, "retrain: no read for a bad lane count");
}
