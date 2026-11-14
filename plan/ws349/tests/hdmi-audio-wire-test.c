/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The host test of the Raspberry Pi 5's HDMI audio arithmetic
 * (src/drivers/platform/rpi5/rpi5-hdmi-audio-wire.c): subframes, the
 * channel status block across fragments and missed interrupts, the order
 * of the core's mmap copy and the conversion, the TMDS rate readback and
 * the audio clock regeneration, the sample divider, the InfoFrame, the DMA
 * ring, the DMA position and the stepping table.
 */

#include "drivers/platform/rpi5/rpi5-hdmi-audio-wire.h"

#include <stdio.h>
#include <string.h>

/* The fragments of PCM and of subframes a whole ring holds. */
#define RING_FRAMES	(DRV_RPI5_HDMI_AUDIO_FRAGMENT_FRAMES * DRV_RPI5_HDMI_AUDIO_FRAGMENT_COUNT)

/* The lead, in fragments, the core keeps its mmap copy ahead of the completed fragments. */
#define CORE_LEAD	2U

/*
 * One TMDS rate of the clock test: the rate the firmware is assumed to
 * program, and the N and CTS expected of it, with and without the port's
 * own CTS measurement.
 */
struct clock_case {
	uint64_t tmds_hz;
	uint32_t n;
	uint32_t cts;
	bool exact;
	uint32_t hardware_cts;
};

/* The checks that failed. */
static unsigned failures;

/* The checks that ran. */
static unsigned checks;

/*
 * The PCM ring the core fills, the subframe ring the DMA reads, and the
 * shadow fragments of the mmap simulation.
 */
static int16_t pcm_ring[RING_FRAMES * 2U];
static uint32_t wire_ring[RING_FRAMES * 2U];

/*
 * The clock cases: the 8-bit rates as the nominal value and as Linux's
 * kHz-rounded value, and the deep color rates.
 */
static const struct clock_case clock_cases[] = {
	{ 25200000ULL, 6144U, 25200U, true, 25200U },
	{ 25174825ULL, 6864U, 28125U, true, 25175U },
	{ 25175000ULL, 6864U, 28125U, true, 25175U },
	{ 27000000ULL, 6144U, 27000U, true, 27000U },
	{ 27027000ULL, 6144U, 27027U, true, 27027U },
	{ 54000000ULL, 6144U, 54000U, true, 54000U },
	{ 74250000ULL, 6144U, 74250U, true, 74250U },
	{ 74175824ULL, 11648U, 140625U, true, 74176U },
	{ 74176000ULL, 11648U, 140625U, true, 74176U },
	{ 148500000ULL, 6144U, 148500U, true, 148500U },
	{ 148351648ULL, 5824U, 140625U, true, 148352U },
	{ 148352000ULL, 5824U, 140625U, true, 148352U },
	{ 297000000ULL, 5120U, 247500U, true, 297000U },
	{ 296703297ULL, 5824U, 281250U, true, 296703U },
	{ 594000000ULL, 6144U, 594000U, true, 594000U },
	{ 593406593ULL, 5824U, 562500U, true, 593407U },
	{ 92812500ULL, 4096U, 61875U, true, 92813U },
	{ 111375000ULL, 6144U, 111375U, true, 111375U },
	{ 185625000ULL, 6144U, 185625U, true, 185625U },
	{ 222750000ULL, 6144U, 222750U, true, 222750U },
	{ 222527473ULL, 11648U, 421875U, true, 222527U },
	{ 371250000ULL, 6144U, 371250U, true, 371250U },
	{ 445500000ULL, 6144U, 445500U, true, 445500U },
	{ 92719780ULL, 6144U, 92720U, false, 92720U },
	{ 100000000ULL, 6144U, 100000U, false, 100000U }
};

static void check(bool good, const char *what);
static uint32_t ones(uint32_t word);
static int16_t pattern(uint64_t serial, uint32_t frame, uint32_t channel);
static void test_subframe(void);
static void test_channel_status(void);
static void test_mmap_order(void);
static void phy_readback(uint64_t tmds_hz, uint32_t *rate_offset, uint32_t *reference, uint32_t *vco_divider);
static void test_clock(void);
static void test_sample_divider(void);
static void test_infoframe(void);
static void test_dma(void);
static void test_locate(void);
static void test_stepping(void);

/*
 * Runs every test and reports the outcome on the last line.
 */
int
main(
	void)
{
	/* Runs each group of checks. */
	test_subframe();
	test_channel_status();
	test_mmap_order();
	test_clock();
	test_sample_divider();
	test_infoframe();
	test_dma();
	test_locate();
	test_stepping();

	/* Reports a failure with the count. */
	if (failures != 0U) {
		printf("hdmi-audio-wire: %u of %u checks FAILED\n", failures, checks);
		return 1;
	}

	/* Succeeded: every check passed. */
	printf("hdmi-audio-wire: %u checks\n", checks);
	printf("hdmi-audio-wire: all PASS\n");
	return 0;
}

/* Records one check and prints a failure. */
static void
check(
	bool good,
	const char *what)
{
	/* Counts the check. */
	checks++;

	/* Reports a failure and counts it. */
	if (!good) {
		printf("FAIL %s\n", what);
		failures++;
	}
}

/* Counts the one bits of a word. */
static uint32_t
ones(
	uint32_t word)
{
	uint32_t count;

	/* Takes the low bit off until none is left. */
	count = 0U;
	while (word != 0U) {
		count += word & 1U;
		word >>= 1;
	}

	/* The count. */
	return count;
}

/* Makes a sample that names its fragment serial, frame and channel, so stale data shows. */
static int16_t
pattern(
	uint64_t serial,
	uint32_t frame,
	uint32_t channel)
{
	uint32_t value;

	/* Mixes the three into sixteen bits. */
	value = (uint32_t)(serial * 7919U) + frame * 2U + channel;

	/* The sample. */
	return (int16_t)(uint16_t)(value & 0xffffU);
}

/* Checks the subframes of known samples, their parity and their preambles. */
static void
test_subframe(
	void)
{
	uint8_t zero[DRV_RPI5_HDMI_AUDIO_STATUS_BYTES];
	uint8_t status[DRV_RPI5_HDMI_AUDIO_STATUS_BYTES];
	uint32_t word;
	int sample;
	bool even;

	/* A block of zero status bits, and the real block. */
	memset(zero, 0, sizeof(zero));
	drv_rpi5_hdmi_audio_channel_status(status);

	/* Known words: silence at a block's start, the extremes, a mixed sample. */
	check(drv_rpi5_hdmi_audio_subframe(0, false, 0U, zero) == 0x00000008U, "subframe of 0, left, block start");
	check(drv_rpi5_hdmi_audio_subframe(0, true, 0U, zero) == 0x00000004U, "subframe of 0, right, block start");
	check(drv_rpi5_hdmi_audio_subframe(0x7fff, false, 1U, zero) == 0x87fff002U, "subframe of 0x7fff, left");
	check(drv_rpi5_hdmi_audio_subframe(-32768, true, 1U, zero) == 0x88000004U, "subframe of 0x8000, right");
	check(drv_rpi5_hdmi_audio_subframe(0x1234, false, 7U, zero) == 0x81234002U, "subframe of 0x1234, left");

	/* The channel status bit of frame 2 (byte 0 bit 2) and its effect on parity. */
	check(drv_rpi5_hdmi_audio_subframe(0, false, 2U, status) == 0xc0000002U, "status bit of frame 2");
	check(drv_rpi5_hdmi_audio_subframe(0, false, 3U, status) == 0x00000002U, "no status bit in frame 3");

	/* Every sample keeps bits 4 to 31 even. */
	even = true;
	for (sample = -32768; sample <= 32767; sample++) {
		word = drv_rpi5_hdmi_audio_subframe((int16_t)sample, false, 2U, status);
		if ((ones(word >> 4) & 1U) != 0U)
			even = false;
	}
	check(even, "parity of every sample");
}

/*
 * Checks that the channel status block runs on through fragments, across
 * the end of the ring, through a second conversion of a fragment, and
 * through missed interrupts.
 */
static void
test_channel_status(
	void)
{
	uint8_t status[DRV_RPI5_HDMI_AUDIO_STATUS_BYTES];
	uint8_t decoded[DRV_RPI5_HDMI_AUDIO_STATUS_BYTES];
	int16_t pcm[DRV_RPI5_HDMI_AUDIO_FRAGMENT_FRAMES * 2U];
	uint32_t words[DRV_RPI5_HDMI_AUDIO_FRAGMENT_FRAMES * 2U];
	uint32_t again[DRV_RPI5_HDMI_AUDIO_FRAGMENT_FRAMES * 2U];
	uint64_t serial;
	uint64_t global;
	uint64_t tracked;
	uint32_t frame;
	uint32_t block_frame;
	uint32_t left_preamble;
	uint32_t status_bit;
	bool preambles;
	bool status_ok;
	bool same;
	bool advance_ok;
	uint32_t gap;
	uint32_t round_index;

	/* The status block and a silent fragment. */
	drv_rpi5_hdmi_audio_channel_status(status);
	memset(pcm, 0, sizeof(pcm));

	/* The block the status function fills. */
	check(status[0] == 0x04U && status[1] == 0x00U && status[2] == 0x00U, "status bytes 0 to 2");
	check(status[3] == 0x02U && status[4] == 0x02U, "status bytes 3 and 4");
	check(status[5] == 0x00U && status[23] == 0x00U, "status bytes 5 to 23");

	/*
	 * Converts 24 fragments, three times round the ring, and checks every
	 * frame against its place in the stream: the block preamble exactly at
	 * a multiple of 192 frames, the status bit of its block frame.
	 */
	preambles = true;
	status_ok = true;
	memset(decoded, 0, sizeof(decoded));
	for (serial = 0U; serial < 24U; serial++) {
		drv_rpi5_hdmi_audio_convert(pcm, words, serial, status);
		for (frame = 0U; frame < DRV_RPI5_HDMI_AUDIO_FRAGMENT_FRAMES; frame++) {
			global = serial * DRV_RPI5_HDMI_AUDIO_FRAGMENT_FRAMES + frame;
			block_frame = (uint32_t)(global % DRV_RPI5_HDMI_AUDIO_BLOCK_FRAMES);
			left_preamble = words[frame * 2U] & 0x0fU;
			if (block_frame == 0U && left_preamble != DRV_RPI5_HDMI_AUDIO_PREAMBLE_BLOCK)
				preambles = false;
			if (block_frame != 0U && left_preamble != DRV_RPI5_HDMI_AUDIO_PREAMBLE_LEFT)
				preambles = false;
			if ((words[frame * 2U + 1U] & 0x0fU) != DRV_RPI5_HDMI_AUDIO_PREAMBLE_RIGHT)
				preambles = false;

			status_bit = (words[frame * 2U] >> 30) & 1U;
			if (status_bit != ((words[frame * 2U + 1U] >> 30) & 1U))
				status_ok = false;
			if (status_bit != ((uint32_t)(status[block_frame / 8U] >> (block_frame % 8U)) & 1U))
				status_ok = false;
			if (serial == 23U && status_bit != 0U)
				decoded[block_frame / 8U] |= (uint8_t)(1U << (block_frame % 8U));
		}
	}
	check(preambles, "block preambles across 24 fragments");
	check(status_ok, "status bits across 24 fragments");

	/* The last fragment ends a block (24 * 1024 = 128 * 192), so its last 192 frames are one whole block. */
	check(memcmp(decoded, status, sizeof(status)) == 0, "status block decoded from a fragment's frames");

	/* A second conversion of the same fragment gives the same words. */
	drv_rpi5_hdmi_audio_convert(pcm, words, 13U, status);
	drv_rpi5_hdmi_audio_convert(pcm, again, 13U, status);
	same = memcmp(words, again, sizeof(words)) == 0;
	check(same, "converting a fragment again");

	/* The serial follows the DMA through 1 and 3 missed interrupts, across the ring's end. */
	advance_ok = true;
	for (gap = 1U; gap <= 4U; gap += 3U) {
		tracked = 5U;
		for (round_index = 0U; round_index < 40U; round_index++) {
			serial = tracked + gap + 1U;
			tracked = drv_rpi5_hdmi_audio_advance(tracked, (uint32_t)(serial % DRV_RPI5_HDMI_AUDIO_FRAGMENT_COUNT));
			if (tracked != serial)
				advance_ok = false;
		}
	}
	check(advance_ok, "serial through missed interrupts");
	check(drv_rpi5_hdmi_audio_advance(14U, 6U) == 14U, "serial with no movement");
	check(drv_rpi5_hdmi_audio_advance(15U, 0U) == 16U, "serial across the ring's end");
}

/*
 * Simulates the audio core's mmap copy and the driver's conversion:
 * playback starts with fragments 0 and 1 copied and converted; at each
 * interrupt the core copies one more fragment, two ahead, and then the
 * driver converts the fragment the DMA is in and the next.  The DMA must
 * then always read the subframes of the PCM the application wrote for that
 * fragment.  The same run with the conversion before the core's copy must
 * read stale sound, which shows the test sees the order.
 */
static void
test_mmap_order(
	void)
{
	uint8_t status[DRV_RPI5_HDMI_AUDIO_STATUS_BYTES];
	uint64_t copied;
	uint64_t dma;
	uint64_t serial;
	uint32_t ring_index;
	uint32_t frame;
	uint32_t order;
	uint32_t expected;
	uint32_t slot;
	bool fresh;

	/* The status block the conversion uses. */
	drv_rpi5_hdmi_audio_channel_status(status);

	/* Order 0 converts after the core's copy (the driver's order), order 1 before it. */
	for (order = 0U; order < 2U; order++) {
		memset(pcm_ring, 0, sizeof(pcm_ring));
		memset(wire_ring, 0, sizeof(wire_ring));

		/* The start: the core takes the lead fragments, prepare converts the ring, start converts 0 and 1 again. */
		for (copied = 0U; copied < CORE_LEAD; copied++) {
			for (frame = 0U; frame < DRV_RPI5_HDMI_AUDIO_FRAGMENT_FRAMES; frame++) {
				pcm_ring[(copied * DRV_RPI5_HDMI_AUDIO_FRAGMENT_FRAMES + frame) * 2U] = pattern(copied, frame, 0U);
				pcm_ring[(copied * DRV_RPI5_HDMI_AUDIO_FRAGMENT_FRAMES + frame) * 2U + 1U] = pattern(copied, frame, 1U);
			}
		}
		for (serial = 0U; serial < DRV_RPI5_HDMI_AUDIO_FRAGMENT_COUNT; serial++) {
			drv_rpi5_hdmi_audio_convert(pcm_ring + serial * DRV_RPI5_HDMI_AUDIO_FRAGMENT_FRAMES * 2U,
			    wire_ring + serial * DRV_RPI5_HDMI_AUDIO_FRAGMENT_FRAMES * 2U, serial, status);
		}

		/*
		 * Runs 30 fragments.  When fragment dma ends the DMA moves on to
		 * dma + 1 at once and reads it while the interrupt is still on its
		 * way, so that fragment must already hold its own sound before the
		 * handler runs.
		 */
		fresh = true;
		for (dma = 0U; dma < 30U; dma++) {
			/* The DMA moves to the next fragment and starts reading it. */
			serial = dma + 1U;
			ring_index = (uint32_t)(serial % DRV_RPI5_HDMI_AUDIO_FRAGMENT_COUNT);
			for (frame = 0U; frame < DRV_RPI5_HDMI_AUDIO_FRAGMENT_FRAMES; frame++) {
				slot = (ring_index * DRV_RPI5_HDMI_AUDIO_FRAGMENT_FRAMES + frame) * 2U;
				expected = (uint32_t)(uint16_t)pattern(serial, frame, 0U);
				if (((wire_ring[slot] >> 12) & 0xffffU) != expected)
					fresh = false;
			}

			/* The interrupt for the end of fragment dma. */
			if (order == 1U) {
				drv_rpi5_hdmi_audio_convert(pcm_ring + (serial % DRV_RPI5_HDMI_AUDIO_FRAGMENT_COUNT) * DRV_RPI5_HDMI_AUDIO_FRAGMENT_FRAMES * 2U,
				    wire_ring + (serial % DRV_RPI5_HDMI_AUDIO_FRAGMENT_COUNT) * DRV_RPI5_HDMI_AUDIO_FRAGMENT_FRAMES * 2U, serial, status);
				drv_rpi5_hdmi_audio_convert(pcm_ring + ((serial + 1U) % DRV_RPI5_HDMI_AUDIO_FRAGMENT_COUNT) * DRV_RPI5_HDMI_AUDIO_FRAGMENT_FRAMES * 2U,
				    wire_ring + ((serial + 1U) % DRV_RPI5_HDMI_AUDIO_FRAGMENT_COUNT) * DRV_RPI5_HDMI_AUDIO_FRAGMENT_FRAMES * 2U, serial + 1U, status);
			}

			/* The core copies up to the lead past the completed fragments. */
			for (; copied < serial + CORE_LEAD; copied++) {
				ring_index = (uint32_t)(copied % DRV_RPI5_HDMI_AUDIO_FRAGMENT_COUNT);
				for (frame = 0U; frame < DRV_RPI5_HDMI_AUDIO_FRAGMENT_FRAMES; frame++) {
					pcm_ring[(ring_index * DRV_RPI5_HDMI_AUDIO_FRAGMENT_FRAMES + frame) * 2U] = pattern(copied, frame, 0U);
					pcm_ring[(ring_index * DRV_RPI5_HDMI_AUDIO_FRAGMENT_FRAMES + frame) * 2U + 1U] = pattern(copied, frame, 1U);
				}
			}

			/* The driver's order: convert the fragment the DMA is in and the next, after the copy. */
			if (order == 0U) {
				drv_rpi5_hdmi_audio_convert(pcm_ring + (serial % DRV_RPI5_HDMI_AUDIO_FRAGMENT_COUNT) * DRV_RPI5_HDMI_AUDIO_FRAGMENT_FRAMES * 2U,
				    wire_ring + (serial % DRV_RPI5_HDMI_AUDIO_FRAGMENT_COUNT) * DRV_RPI5_HDMI_AUDIO_FRAGMENT_FRAMES * 2U, serial, status);
				drv_rpi5_hdmi_audio_convert(pcm_ring + ((serial + 1U) % DRV_RPI5_HDMI_AUDIO_FRAGMENT_COUNT) * DRV_RPI5_HDMI_AUDIO_FRAGMENT_FRAMES * 2U,
				    wire_ring + ((serial + 1U) % DRV_RPI5_HDMI_AUDIO_FRAGMENT_COUNT) * DRV_RPI5_HDMI_AUDIO_FRAGMENT_FRAMES * 2U, serial + 1U, status);
			}
		}

		/* The driver's order plays every fragment's sound; the other order does not. */
		if (order == 0U)
			check(fresh, "conversion after the core's copy plays fresh sound");
		else
			check(!fresh, "conversion before the core's copy plays stale sound");
	}
}

/*
 * Makes the PHY readback the firmware would leave for a TMDS rate: the VCO
 * in the middle of the dividers that keep it within 8 to 12 GHz, its offset
 * over the 54 MHz reference scaled by 2^21 and truncated.
 */
static void
phy_readback(
	uint64_t tmds_hz,
	uint32_t *rate_offset,
	uint32_t *reference,
	uint32_t *vco_divider)
{
	uint64_t low;
	uint64_t high;
	uint64_t divider;
	uint64_t vco;
	uint64_t offset;

	/* The smallest divider that lifts the VCO to 8 GHz, and the largest that keeps it under 12 GHz. */
	low = 0U;
	while (tmds_hz * low * 10U < 8000000000ULL)
		low++;
	high = low;
	while (tmds_hz * (high + 1U) * 10U < 12000000000ULL)
		high++;
	divider = low + (high - low) / 2U;
	vco = tmds_hz * divider * 10U;

	/* The offset, truncated as the firmware computes it. */
	offset = (vco << 21) / 54000000ULL;

	/* The three registers. */
	*rate_offset = (uint32_t)offset | DRV_RPI5_HDMI_AUDIO_RATE_FIXED;
	*reference = 54U | 0x2000U;
	*vco_divider = (uint32_t)divider | DRV_RPI5_HDMI_AUDIO_VCO_DIVIDER_ON;
}

/* Checks the TMDS rate readback and the N and CTS of each rate. */
static void
test_clock(
	void)
{
	struct drv_rpi5_hdmi_audio_clock clock;
	uint32_t rate_offset;
	uint32_t reference;
	uint32_t vco_divider;
	uint64_t tmds_hz;
	uint64_t gap;
	enum drv_rpi5_hdmi_audio_tmds_status readback;
	uint32_t index;
	bool readback_ok;
	bool software_ok;
	bool hardware_ok;
	bool whole_ok;

	/* Each rate: the readback comes back within 2 Hz, and the clock is the expected one. */
	readback_ok = true;
	software_ok = true;
	hardware_ok = true;
	whole_ok = true;
	for (index = 0U; index < sizeof(clock_cases) / sizeof(clock_cases[0]); index++) {
		phy_readback(clock_cases[index].tmds_hz, &rate_offset, &reference, &vco_divider);
		tmds_hz = 0U;
		readback = drv_rpi5_hdmi_audio_tmds_rate(rate_offset, reference, vco_divider, &tmds_hz);
		gap = tmds_hz > clock_cases[index].tmds_hz ? tmds_hz - clock_cases[index].tmds_hz : clock_cases[index].tmds_hz - tmds_hz;
		if (readback != DRV_RPI5_HDMI_AUDIO_TMDS_OK || gap > 2U) {
			printf("  readback of %llu gave %llu (status %d)\n",
			    (unsigned long long)clock_cases[index].tmds_hz, (unsigned long long)tmds_hz, (int)readback);
			readback_ok = false;
		}

		drv_rpi5_hdmi_audio_clock(tmds_hz, false, &clock);
		if (clock.n != clock_cases[index].n || clock.cts != clock_cases[index].cts || clock.exact != clock_cases[index].exact) {
			printf("  clock of %llu: N %u CTS %u exact %d\n",
			    (unsigned long long)clock_cases[index].tmds_hz, clock.n, clock.cts, (int)clock.exact);
			software_ok = false;
		}
		if (clock.exact && (clock.n < 4096U || clock.n > 20480U))
			whole_ok = false;

		drv_rpi5_hdmi_audio_clock(tmds_hz, true, &clock);
		if (clock.n != 6144U || clock.cts != clock_cases[index].hardware_cts || (clock.n % 128U) != 0U) {
			printf("  hardware clock of %llu: N %u CTS %u\n",
			    (unsigned long long)clock_cases[index].tmds_hz, clock.n, clock.cts);
			hardware_ok = false;
		}
	}
	check(readback_ok, "TMDS rate readback of every rate");
	check(software_ok, "N and CTS of every rate");
	check(hardware_ok, "N and CTS for the port's own CTS");
	check(whole_ok, "exact N within the recommended range");

	/* A readback the rate manager does not hold fixed. */
	phy_readback(74250000ULL, &rate_offset, &reference, &vco_divider);
	readback = drv_rpi5_hdmi_audio_tmds_rate(rate_offset & ~DRV_RPI5_HDMI_AUDIO_RATE_FIXED, reference, vco_divider, &tmds_hz);
	check(readback == DRV_RPI5_HDMI_AUDIO_TMDS_NOT_FIXED, "offset without the fixed bit");

	/* A VCO divider that is off, or zero. */
	readback = drv_rpi5_hdmi_audio_tmds_rate(rate_offset, reference, vco_divider & ~DRV_RPI5_HDMI_AUDIO_VCO_DIVIDER_ON, &tmds_hz);
	check(readback == DRV_RPI5_HDMI_AUDIO_TMDS_NO_DIVIDER, "divider off");
	readback = drv_rpi5_hdmi_audio_tmds_rate(rate_offset, reference, DRV_RPI5_HDMI_AUDIO_VCO_DIVIDER_ON, &tmds_hz);
	check(readback == DRV_RPI5_HDMI_AUDIO_TMDS_NO_DIVIDER, "divider zero");

	/* A PLL without a reference. */
	readback = drv_rpi5_hdmi_audio_tmds_rate(rate_offset, 0x2000U, vco_divider, &tmds_hz);
	check(readback == DRV_RPI5_HDMI_AUDIO_TMDS_NO_REFERENCE, "reference zero");

	/* Rates outside 25 to 600 MHz. */
	phy_readback(20000000ULL, &rate_offset, &reference, &vco_divider);
	readback = drv_rpi5_hdmi_audio_tmds_rate(rate_offset, reference, vco_divider, &tmds_hz);
	check(readback == DRV_RPI5_HDMI_AUDIO_TMDS_OUT_OF_RANGE, "rate below 25 MHz");
	phy_readback(700000000ULL, &rate_offset, &reference, &vco_divider);
	readback = drv_rpi5_hdmi_audio_tmds_rate(rate_offset, reference, vco_divider, &tmds_hz);
	check(readback == DRV_RPI5_HDMI_AUDIO_TMDS_OUT_OF_RANGE, "rate above 600 MHz");
}

/* Checks the sample divider at 108 MHz and against a search of every sample count. */
static void
test_sample_divider(
	void)
{
	static const uint32_t clocks[] = { 108000000U, 108000001U, 54000000U, 27000000U, 99999989U, 216000000U };
	static const uint32_t rates[] = { 48000U, 44100U, 32000U, 96000U, 192000U, 11025U };
	uint32_t periods;
	uint32_t samples;
	uint32_t clock_index;
	uint32_t rate_index;
	uint32_t trial_samples;
	uint64_t trial_periods;
	uint64_t best_numerator;
	uint64_t best_denominator;
	uint64_t error_numerator;
	uint64_t chosen_numerator;
	bool best_ok;

	/* 108 MHz over 48 kHz is exactly 2250 over 1. */
	drv_rpi5_hdmi_audio_sample_divider(108000000U, 48000U, &periods, &samples);
	check(periods == 2250U && samples == 1U, "sample divider of 108 MHz at 48 kHz");
	check(drv_rpi5_hdmi_audio_sample_word(2250U, 1U) == 0x0008ca00U, "sample divider word");

	/*
	 * For each clock and rate the chosen fraction is no farther from
	 * clock / rate than the best fraction of any sample count up to 256.
	 * The errors compare as |p * rate - q * clock| / q.
	 */
	best_ok = true;
	for (clock_index = 0U; clock_index < sizeof(clocks) / sizeof(clocks[0]); clock_index++) {
		for (rate_index = 0U; rate_index < sizeof(rates) / sizeof(rates[0]); rate_index++) {
			drv_rpi5_hdmi_audio_sample_divider(clocks[clock_index], rates[rate_index], &periods, &samples);
			if (samples == 0U || samples > 256U || periods > 0x00ffffffU) {
				best_ok = false;
				continue;
			}

			best_numerator = ~0ULL;
			best_denominator = 1U;
			for (trial_samples = 1U; trial_samples <= 256U; trial_samples++) {
				trial_periods = ((uint64_t)clocks[clock_index] * trial_samples + rates[rate_index] / 2U) / rates[rate_index];
				if (trial_periods > 0x00ffffffU)
					continue;
				error_numerator = trial_periods * rates[rate_index];
				if (error_numerator > (uint64_t)clocks[clock_index] * trial_samples)
					error_numerator -= (uint64_t)clocks[clock_index] * trial_samples;
				else
					error_numerator = (uint64_t)clocks[clock_index] * trial_samples - error_numerator;
				if (best_numerator == ~0ULL || error_numerator * best_denominator < best_numerator * trial_samples) {
					best_numerator = error_numerator;
					best_denominator = trial_samples;
				}
			}

			chosen_numerator = (uint64_t)periods * rates[rate_index];
			if (chosen_numerator > (uint64_t)clocks[clock_index] * samples)
				chosen_numerator -= (uint64_t)clocks[clock_index] * samples;
			else
				chosen_numerator = (uint64_t)clocks[clock_index] * samples - chosen_numerator;
			if (chosen_numerator * best_denominator > best_numerator * samples) {
				printf("  divider %u/%u: %u/%u is not the closest\n",
				    clocks[clock_index], rates[rate_index], periods, samples);
				best_ok = false;
			}
		}
	}
	check(best_ok, "sample divider is the closest fraction");
}

/* Checks the audio InfoFrame's bytes, its checksum and its packet RAM words. */
static void
test_infoframe(
	void)
{
	static const uint8_t expected[DRV_RPI5_HDMI_AUDIO_INFOFRAME_BYTES] = {
		0x84U, 0x01U, 0x0aU, 0x70U, 0x01U, 0x00U, 0x00U,
		0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U
	};
	uint8_t frame[DRV_RPI5_HDMI_AUDIO_INFOFRAME_BYTES];
	uint32_t words[DRV_RPI5_HDMI_AUDIO_PACKET_SLOT_WORDS];
	uint32_t index;
	uint32_t sum;
	bool rest_zero;

	/* The stereo frame's bytes and checksum. */
	drv_rpi5_hdmi_audio_infoframe(frame, 2U);
	check(memcmp(frame, expected, sizeof(expected)) == 0, "stereo InfoFrame bytes");
	sum = 0U;
	for (index = 0U; index < DRV_RPI5_HDMI_AUDIO_INFOFRAME_BYTES; index++)
		sum += frame[index];
	check((sum & 0xffU) == 0U, "InfoFrame checksum");

	/* The eight-channel frame's channel count and checksum. */
	drv_rpi5_hdmi_audio_infoframe(frame, 8U);
	sum = 0U;
	for (index = 0U; index < DRV_RPI5_HDMI_AUDIO_INFOFRAME_BYTES; index++)
		sum += frame[index];
	check(frame[4] == 0x07U && (sum & 0xffU) == 0U, "eight-channel InfoFrame");

	/* The stereo frame in the packet RAM slot, the rest of the slot zero. */
	drv_rpi5_hdmi_audio_infoframe(frame, 2U);
	for (index = 0U; index < DRV_RPI5_HDMI_AUDIO_PACKET_SLOT_WORDS; index++)
		words[index] = 0xdeadbeefU;
	drv_rpi5_hdmi_audio_packet_words(frame, words);
	check(words[0] == 0x000a0184U, "packet word 0");
	check(words[1] == 0x00000170U, "packet word 1");
	check(words[2] == 0x00000000U && words[3] == 0x00000000U, "packet words 2 and 3");
	rest_zero = true;
	for (index = 4U; index < DRV_RPI5_HDMI_AUDIO_PACKET_SLOT_WORDS; index++) {
		if (words[index] != 0U)
			rest_zero = false;
	}
	check(rest_zero, "packet slot rest zero");

	/* The audio slot is number 4, 0x90 bytes in. */
	check(drv_rpi5_hdmi_audio_packet_slot(0x84U) == 0x90U, "audio packet slot offset");
}

/* Checks the DMA cell reading, the run word and the control block ring. */
static void
test_dma(
	void)
{
	struct drv_rpi5_hdmi_audio_dma_request request;
	struct drv_rpi5_hdmi_audio_dma_cb cbs[DRV_RPI5_HDMI_AUDIO_FRAGMENT_COUNT];
	uint64_t cb_bus;
	uint64_t data_bus;
	uint64_t fifo_bus;
	uint32_t index;
	uint32_t next;
	bool built;
	bool blocks_ok;

	/* The D0 HDMI0 cell: DREQ 12, priority 0xa, panic 0xf, wide reads, burst, write response. */
	drv_rpi5_hdmi_audio_dma_decode(0x41fa000cU, &request);
	check(request.dreq == 12U && request.priority == 0x0aU && request.panic_priority == 0x0fU, "cell DREQ and priorities");
	check(request.wide_source && !request.wide_destination && request.burst, "cell transfer shape");
	check(request.wait_write && !request.drain_writes && !request.no_debug, "cell channel behavior");
	check(drv_rpi5_hdmi_audio_dma_run(&request) == 0x00fa0301U, "run word");

	/* The C1 HDMI0 cell names DREQ 10. */
	drv_rpi5_hdmi_audio_dma_decode(0x41fa000aU, &request);
	check(request.dreq == 10U, "C1 cell DREQ");

	/* A ring above 4 GiB, to the HDMI audio FIFO of port 0. */
	drv_rpi5_hdmi_audio_dma_decode(0x41fa000cU, &request);
	cb_bus = 0x100000040ULL;
	data_bus = 0x120000000ULL;
	fifo_bus = 0x107c72001cULL;
	built = drv_rpi5_hdmi_audio_dma_ring(cbs, DRV_RPI5_HDMI_AUDIO_FRAGMENT_COUNT, cb_bus, data_bus,
	    DRV_RPI5_HDMI_AUDIO_WIRE_FRAGMENT_BYTES, fifo_bus, &request);
	check(built, "ring built");

	/* Each block's words and its link to the next, the last to the first. */
	blocks_ok = true;
	for (index = 0U; index < DRV_RPI5_HDMI_AUDIO_FRAGMENT_COUNT; index++) {
		next = (index + 1U) % DRV_RPI5_HDMI_AUDIO_FRAGMENT_COUNT;
		if (cbs[index].transfer != 0x00009805U)
			blocks_ok = false;
		if (cbs[index].source != 0x20000000U + index * DRV_RPI5_HDMI_AUDIO_WIRE_FRAGMENT_BYTES)
			blocks_ok = false;
		if (cbs[index].source_info != 0x00005301U)
			blocks_ok = false;
		if (cbs[index].destination != 0x7c72001cU || cbs[index].destination_info != 0x00000310U)
			blocks_ok = false;
		if (cbs[index].length != DRV_RPI5_HDMI_AUDIO_WIRE_FRAGMENT_BYTES || cbs[index].reserved != 0U)
			blocks_ok = false;
		if (cbs[index].next != (uint32_t)((cb_bus + next * 32U) >> 5))
			blocks_ok = false;
	}
	check(blocks_ok, "control block words and links");
	check(cbs[DRV_RPI5_HDMI_AUDIO_FRAGMENT_COUNT - 1U].next == (uint32_t)(cb_bus >> 5), "last block links to the first");

	/* A request narrowed to 32-bit reads without bursts and without the write response. */
	request.wide_source = false;
	request.burst = false;
	request.wait_write = false;
	built = drv_rpi5_hdmi_audio_dma_ring(cbs, DRV_RPI5_HDMI_AUDIO_FRAGMENT_COUNT, cb_bus, data_bus,
	    DRV_RPI5_HDMI_AUDIO_WIRE_FRAGMENT_BYTES, fifo_bus, &request);
	check(built && cbs[0].transfer == 0x00009801U && cbs[0].source_info == 0x00001001U && cbs[0].destination_info == 0x00000010U,
	    "narrowed request");
	drv_rpi5_hdmi_audio_dma_decode(0x41fa000cU, &request);

	/* Refusals: a misaligned block, data across 4 GiB, data or FIFO beyond 40 bits, blocks beyond the field. */
	built = drv_rpi5_hdmi_audio_dma_ring(cbs, 8U, cb_bus + 8U, data_bus, 8192U, fifo_bus, &request);
	check(!built, "misaligned control block refused");
	built = drv_rpi5_hdmi_audio_dma_ring(cbs, 8U, cb_bus, 0xfffff000ULL, 8192U, fifo_bus, &request);
	check(!built, "data across 4 GiB refused");
	built = drv_rpi5_hdmi_audio_dma_ring(cbs, 8U, cb_bus, 0xfffff0000ULL, 4096U, fifo_bus, &request);
	check(built, "data ending exactly at 4 GiB taken");
	built = drv_rpi5_hdmi_audio_dma_ring(cbs, 8U, cb_bus, 0xfffffff0000ULL, 8192U, fifo_bus, &request);
	check(!built, "data beyond 40 bits refused");
	built = drv_rpi5_hdmi_audio_dma_ring(cbs, 8U, cb_bus, data_bus, 8192U, 0x10000000000ULL, &request);
	check(!built, "FIFO beyond 40 bits refused");
	built = drv_rpi5_hdmi_audio_dma_ring(cbs, 8U, 0x2000000000ULL, data_bus, 8192U, fifo_bus, &request);
	check(!built, "control blocks beyond the field refused");
	built = drv_rpi5_hdmi_audio_dma_ring(cbs, 0U, cb_bus, data_bus, 8192U, fifo_bus, &request);
	check(!built, "empty ring refused");
}

/* Checks the fragment and the PCM offset the source address gives. */
static void
test_locate(
	void)
{
	uint64_t data_bus;
	uint32_t fragment;
	uint32_t pcm_offset;
	bool located;

	/* A ring above 4 GiB. */
	data_bus = 0x120000000ULL;

	/* The start, inside fragment 0, the boundary to fragment 1, inside fragment 7. */
	located = drv_rpi5_hdmi_audio_dma_locate(0x20000000U, 0x5301U, data_bus, 8192U, 8U, &fragment, &pcm_offset);
	check(located && fragment == 0U && pcm_offset == 0U, "position at the start");
	located = drv_rpi5_hdmi_audio_dma_locate(0x20000100U, 0x5301U, data_bus, 8192U, 8U, &fragment, &pcm_offset);
	check(located && fragment == 0U && pcm_offset == 0x80U, "position inside fragment 0");
	located = drv_rpi5_hdmi_audio_dma_locate(0x20002000U, 0x5301U, data_bus, 8192U, 8U, &fragment, &pcm_offset);
	check(located && fragment == 1U && pcm_offset == 4096U, "position at fragment 1");
	located = drv_rpi5_hdmi_audio_dma_locate(0x2000fffcU, 0x5301U, data_bus, 8192U, 8U, &fragment, &pcm_offset);
	check(located && fragment == 7U && pcm_offset == 0x7ffeU, "position at the end of fragment 7");

	/* The end of the ring is its start. */
	located = drv_rpi5_hdmi_audio_dma_locate(0x20010000U, 0x5301U, data_bus, 8192U, 8U, &fragment, &pcm_offset);
	check(located && fragment == 0U && pcm_offset == 0U, "position at the end of the ring");

	/* Outside the ring: below it, beyond it, a different high byte. */
	located = drv_rpi5_hdmi_audio_dma_locate(0x1ffffffcU, 0x5301U, data_bus, 8192U, 8U, &fragment, &pcm_offset);
	check(!located, "position below the ring");
	located = drv_rpi5_hdmi_audio_dma_locate(0x20010004U, 0x5301U, data_bus, 8192U, 8U, &fragment, &pcm_offset);
	check(!located, "position beyond the ring");
	located = drv_rpi5_hdmi_audio_dma_locate(0x20000000U, 0x5302U, data_bus, 8192U, 8U, &fragment, &pcm_offset);
	check(!located, "position under another high byte");
}

/* Checks the stepping, the DREQ table against the tree's cell, and the thresholds. */
static void
test_stepping(
	void)
{
	uint32_t dreq;
	uint32_t word;
	bool agrees;
	bool known;

	/* The HVS version's low byte names the stepping. */
	check(drv_rpi5_hdmi_audio_stepping(0x53U) == DRV_RPI5_HDMI_AUDIO_STEPPING_C, "stepping C");
	check(drv_rpi5_hdmi_audio_stepping(0x12345654U) == DRV_RPI5_HDMI_AUDIO_STEPPING_D0, "stepping D0 from the low byte");
	check(drv_rpi5_hdmi_audio_stepping(0x55U) == DRV_RPI5_HDMI_AUDIO_STEPPING_UNKNOWN, "stepping unknown");

	/* Each tree with its own stepping agrees. */
	agrees = drv_rpi5_hdmi_audio_dreq(DRV_RPI5_HDMI_AUDIO_STEPPING_C, 0U, 0x41fa000aU, &dreq);
	check(agrees && dreq == 10U, "C tree, HDMI0");
	agrees = drv_rpi5_hdmi_audio_dreq(DRV_RPI5_HDMI_AUDIO_STEPPING_C, 1U, 0x41fa0011U, &dreq);
	check(agrees && dreq == 17U, "C tree, HDMI1");
	agrees = drv_rpi5_hdmi_audio_dreq(DRV_RPI5_HDMI_AUDIO_STEPPING_D0, 0U, 0x41fa000cU, &dreq);
	check(agrees && dreq == 12U, "D0 tree, HDMI0");
	agrees = drv_rpi5_hdmi_audio_dreq(DRV_RPI5_HDMI_AUDIO_STEPPING_D0, 1U, 0x41fa000dU, &dreq);
	check(agrees && dreq == 13U, "D0 tree, HDMI1");

	/* The C1 tree on a D0 chip disagrees, and the stepping's DREQ wins. */
	agrees = drv_rpi5_hdmi_audio_dreq(DRV_RPI5_HDMI_AUDIO_STEPPING_D0, 0U, 0x41fa000aU, &dreq);
	check(!agrees && dreq == 12U, "C1 tree on a D0 chip, HDMI0");
	agrees = drv_rpi5_hdmi_audio_dreq(DRV_RPI5_HDMI_AUDIO_STEPPING_D0, 1U, 0x41fa0011U, &dreq);
	check(!agrees && dreq == 13U, "C1 tree on a D0 chip, HDMI1");

	/* An unknown stepping or port gives nothing. */
	agrees = drv_rpi5_hdmi_audio_dreq(DRV_RPI5_HDMI_AUDIO_STEPPING_UNKNOWN, 0U, 0x41fa000cU, &dreq);
	check(!agrees && dreq == 0U, "unknown stepping");
	agrees = drv_rpi5_hdmi_audio_dreq(DRV_RPI5_HDMI_AUDIO_STEPPING_D0, 2U, 0x41fa000cU, &dreq);
	check(!agrees && dreq == 0U, "unknown port");

	/* The thresholds of each layout. */
	known = drv_rpi5_hdmi_audio_thresholds(DRV_RPI5_HDMI_AUDIO_STEPPING_C, &word);
	check(known && word == 0x10101c1cU, "C thresholds");
	known = drv_rpi5_hdmi_audio_thresholds(DRV_RPI5_HDMI_AUDIO_STEPPING_D0, &word);
	check(known && word == 0x08080e1cU, "D0 thresholds");
	known = drv_rpi5_hdmi_audio_thresholds(DRV_RPI5_HDMI_AUDIO_STEPPING_UNKNOWN, &word);
	check(!known && word == 0U, "unknown thresholds");
}
