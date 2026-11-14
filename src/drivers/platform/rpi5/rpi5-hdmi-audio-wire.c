/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The arithmetic of the Raspberry Pi 5's HDMI audio (see
 * rpi5-hdmi-audio-wire.h).
 */

#include "drivers/platform/rpi5/rpi5-hdmi-audio-wire.h"

/*
 * The audio clock regeneration's divisor for 48 kHz: CTS is the TMDS rate
 * times N over 128 times the sample rate.
 */
#define CLOCK_DIVISOR			(128ULL * DRV_RPI5_HDMI_AUDIO_RATE)

/* The N the HDMI specification recommends for 48 kHz when nothing better is known. */
#define CLOCK_N_DEFAULT			6144U

/*
 * The range of N the search for an exact CTS walks through:
 * 128 * fs / 1500 to 128 * fs / 300 for 48 kHz.
 */
#define CLOCK_N_MIN			4096U
#define CLOCK_N_MAX			20480U

/* How far a measured TMDS rate may stray from a known rate and still be taken for it, in parts per million. */
#define CLOCK_MATCH_PPM			200ULL

/* The largest sample divider's period count (24 bits) and sample count (eight bits, written less one). */
#define SAMPLE_PERIODS_MAX		0x00ffffffU
#define SAMPLE_SAMPLES_MAX		256U

/*
 * The device tree DMA cell's bits above the DREQ.
 *
 * Values referenced: Raspberry Pi Linux rpi-6.12.y ab4372076bb8
 * drivers/dma/bcm2835-dma.c (the per-request bits the driver reads from
 * the cell: the priority fields, wide source and destination, no write
 * response, wait for outstanding writes, no debug pause, burst).
 */
#define CELL_DREQ_MASK			0x0000001fU
#define CELL_PRIORITY_SHIFT		16U
#define CELL_PANIC_SHIFT		20U
#define CELL_PRIORITY_MASK		0x0000000fU
#define CELL_WIDE_SOURCE		0x01000000U
#define CELL_WIDE_DESTINATION		0x02000000U
#define CELL_NO_WAIT_WRITE		0x08000000U
#define CELL_DRAIN_WRITES		0x10000000U
#define CELL_NO_DEBUG			0x20000000U
#define CELL_BURST			0x40000000U

/*
 * The burst length a cell's burst bit asks for.
 *
 * Value referenced: Raspberry Pi Linux rpi-6.12.y ab4372076bb8
 * drivers/dma/bcm2835-dma.c (the burst length of the cell's burst bit).
 */
#define DMA_BURST_LENGTH		3U

/* A 40-bit address, and the 4 GiB window a ring must stay inside. */
#define DMA_ADDRESS_LIMIT		(1ULL << 40)
#define DMA_WINDOW_SHIFT		32U

/* The highest control block address the channel's 32-bit, five-bit-shifted field can name. */
#define DMA_CB_LIMIT			(1ULL << (32U + DRV_RPI5_HDMI_AUDIO_DMA_CB_SHIFT))

/* The two HDMI ports. */
#define PORT_COUNT			2U

/*
 * The HVS version register's low byte for each stepping.
 *
 * Values referenced: Raspberry Pi Linux rpi-6.12.y ab4372076bb8
 * drivers/gpu/drm/vc4/vc4_regs.h (the HVS version field and the C0 and
 * D0 values).
 */
#define HVS_VERSION_MASK		0x000000ffU
#define HVS_VERSION_C			0x53U
#define HVS_VERSION_D0			0x54U

/*
 * The audio FIFO's thresholds: the panic levels and the DREQ levels, and
 * where each stepping puts them in the threshold register.
 *
 * Values referenced: Raspberry Pi Linux rpi-6.12.y ab4372076bb8
 * drivers/gpu/drm/vc4/vc4_regs.h (the threshold fields of the C and D0
 * layouts), drivers/gpu/drm/vc4/vc4_hdmi.c (the threshold values in
 * vc4_hdmi_audio_prepare).
 */
#define THRESHOLD_PANIC			0x10U
#define THRESHOLD_DREQ			0x1cU
#define THRESHOLD_C_PANIC_HIGH_SHIFT	24U
#define THRESHOLD_C_PANIC_LOW_SHIFT	16U
#define THRESHOLD_C_DREQ_HIGH_SHIFT	8U
#define THRESHOLD_C_DREQ_LOW_SHIFT	0U
#define THRESHOLD_D0_PANIC_HIGH_SHIFT	23U
#define THRESHOLD_D0_PANIC_LOW_SHIFT	15U
#define THRESHOLD_D0_DREQ_HIGH_SHIFT	7U
#define THRESHOLD_D0_DREQ_LOW_SHIFT	0U

/*
 * A TMDS rate the clock regeneration knows, as a fraction of Hz so that
 * the 1000/1001 rates stay exact.
 */
struct clock_rate {
	uint64_t numerator;
	uint32_t denominator;
};

/*
 * A TMDS rate of 8-bit color and the N the HDMI specification gives it
 * for 48 kHz.
 */
struct clock_entry {
	struct clock_rate rate;
	uint32_t n;
};

/*
 * A deep color factor: the TMDS rate is the 8-bit rate times this fraction.
 */
struct clock_factor {
	uint32_t numerator;
	uint32_t denominator;
};

/*
 * The 48 kHz N of each common 8-bit TMDS rate.
 *
 * With these N every CTS is a whole number.  A rate of a deep color mode
 * is one of these times a factor below, and finds its N by search.
 *
 * Values referenced: Raspberry Pi Linux rpi-6.12.y ab4372076bb8
 * drivers/gpu/drm/bridge/synopsys/dw-hdmi.c (the 48 kHz N of 25.175,
 * 27.027, 74.176, 148.352 and 297 MHz in hdmi_compute_n); the HDMI 1.4b
 * audio clock regeneration table (the 1000/1001 rates of 297 and
 * 594 MHz).
 */
static const struct clock_entry clock_table[] = {
	{ { 25200000ULL, 1U }, 6144U },
	{ { 25200000000ULL, 1001U }, 6864U },
	{ { 27000000ULL, 1U }, 6144U },
	{ { 27027000ULL, 1U }, 6144U },
	{ { 54000000ULL, 1U }, 6144U },
	{ { 54054000ULL, 1U }, 6144U },
	{ { 74250000ULL, 1U }, 6144U },
	{ { 74250000000ULL, 1001U }, 11648U },
	{ { 148500000ULL, 1U }, 6144U },
	{ { 148500000000ULL, 1001U }, 5824U },
	{ { 297000000ULL, 1U }, 5120U },
	{ { 297000000000ULL, 1001U }, 5824U },
	{ { 594000000ULL, 1U }, 6144U },
	{ { 594000000000ULL, 1001U }, 5824U }
};

/*
 * The deep color factors: 10, 12 and 16 bits per color.  The 8-bit factor
 * is the table itself.
 */
static const struct clock_factor clock_factors[] = {
	{ 5U, 4U },
	{ 3U, 2U },
	{ 2U, 1U }
};

/*
 * The audio DREQ of each HDMI port, by stepping.
 *
 * Values referenced: raspberrypi/firmware 3d301dd boot/bcm2712-rpi-5-b.dtb
 * and boot/bcm2712d0-rpi-5-b.dtb (the dmas cells of the two HDMI nodes).
 */
static const uint32_t dreq_c[PORT_COUNT] = { 10U, 17U };
static const uint32_t dreq_d0[PORT_COUNT] = { 12U, 13U };

static uint32_t parity(uint32_t word);
static bool rate_matches(uint64_t tmds_hz, const struct clock_rate *rate);
static bool find_rate(uint64_t tmds_hz, struct clock_rate *rate, uint32_t *table_n);
static bool find_n(const struct clock_rate *rate, uint32_t *n);
static uint32_t rounded_cts(const struct clock_rate *rate, uint32_t n);
static bool exact_cts(const struct clock_rate *rate, uint32_t n);
static uint64_t distance(uint64_t a, uint64_t b);

/*
 * Fills the 24-byte channel status block of 48 kHz, 16-bit consumer PCM.
 *
 * Byte 0 says consumer use, audio, no copyright asserted, no emphasis;
 * byte 1 the general category; byte 2 no source or channel number; byte 3
 * 48 kHz at the 1000 ppm clock accuracy; byte 4 a 16-bit word out of a
 * 20-bit maximum.  The rest is zero.
 *
 * Values referenced: Raspberry Pi Linux rpi-6.12.y ab4372076bb8
 * include/sound/asoundef.h (the consumer channel status bits),
 * sound/core/pcm_iec958.c (the consumer defaults and the 48 kHz and
 * 16-bit codes).
 */
void
drv_rpi5_hdmi_audio_channel_status(
	uint8_t *status)
{
	uint32_t index;

	/* Starts from a block of zero bits. */
	for (index = 0U; index < DRV_RPI5_HDMI_AUDIO_STATUS_BYTES; index++)
		status[index] = 0U;

	/* Consumer audio without a copyright claim. */
	status[0] = 0x04U;

	/* The 48 kHz sampling frequency code. */
	status[3] = 0x02U;

	/* A 16-bit sample word, the 20-bit maximum. */
	status[4] = 0x02U;
}

/*
 * Makes the IEC 60958 subframe of one 16-bit sample.
 *
 * The sample takes bits 12 to 27 (the top of the 24-bit audio field),
 * validity and user data stay zero, bit 30 carries the frame's channel
 * status bit and bit 31 the even parity of bits 4 to 30.  The left
 * subframe of a block's first frame takes the block preamble.
 *
 * Procedure referenced: alsa-lib f44e0abd5382 src/pcm/pcm_iec958.c
 * (iec958_subframe and iec958_parity).
 */
uint32_t
drv_rpi5_hdmi_audio_subframe(
	int16_t sample,
	bool right,
	uint32_t block_frame,
	const uint8_t *status)
{
	uint32_t word;
	uint32_t status_byte;
	uint32_t status_bit;
	uint32_t odd;

	/* The sample, as its 16 bits, at the top of the audio field. */
	word = (uint32_t)(uint16_t)sample << 12;

	/* The channel status bit this frame carries, low bit of a byte first. */
	status_byte = status[block_frame / 8U];
	status_bit = (status_byte >> (block_frame % 8U)) & 1U;
	if (status_bit != 0U)
		word |= DRV_RPI5_HDMI_AUDIO_SUBFRAME_STATUS;

	/* The parity bit keeps the count of ones in bits 4 to 31 even. */
	odd = parity(word);
	if (odd != 0U)
		word |= DRV_RPI5_HDMI_AUDIO_SUBFRAME_PARITY;

	/* The preamble: right, the left of a block's first frame, or another left. */
	if (right) {
		word |= DRV_RPI5_HDMI_AUDIO_PREAMBLE_RIGHT;
	} else if (block_frame == 0U) {
		word |= DRV_RPI5_HDMI_AUDIO_PREAMBLE_BLOCK;
	} else {
		word |= DRV_RPI5_HDMI_AUDIO_PREAMBLE_LEFT;
	}

	/* Succeeded: the subframe. */
	return word;
}

/*
 * Converts one fragment of 16-bit stereo frames into subframes.
 *
 * serial is the fragment's number counted from the start of the stream,
 * not its place in the ring: a ring of eight 1024-frame fragments is not
 * a whole number of 192-frame blocks, so only the count of frames before
 * the fragment says where its frames fall in a block.  Converting the
 * same fragment again gives the same words.
 */
void
drv_rpi5_hdmi_audio_convert(
	const int16_t *pcm,
	uint32_t *words,
	uint64_t serial,
	const uint8_t *status)
{
	uint32_t block_frame;
	uint32_t frame;

	/*
	 * The block frame of the fragment's first frame.  Three fragments are
	 * 3072 frames, sixteen whole blocks, so the serial modulo three is
	 * enough and the product never overflows.
	 */
	block_frame = (uint32_t)((serial % 3U) * DRV_RPI5_HDMI_AUDIO_FRAGMENT_FRAMES) % DRV_RPI5_HDMI_AUDIO_BLOCK_FRAMES;

	/* Converts each frame, left then right, moving through the block. */
	for (frame = 0U; frame < DRV_RPI5_HDMI_AUDIO_FRAGMENT_FRAMES; frame++) {
		/* Both subframes of a frame carry the same block frame. */
		words[frame * 2U] = drv_rpi5_hdmi_audio_subframe(pcm[frame * 2U], false, block_frame, status);
		words[frame * 2U + 1U] = drv_rpi5_hdmi_audio_subframe(pcm[frame * 2U + 1U], true, block_frame, status);

		/* The next frame is the next in the block, and a full block starts again. */
		block_frame++;
		if (block_frame == DRV_RPI5_HDMI_AUDIO_BLOCK_FRAMES)
			block_frame = 0U;
	}
}

/*
 * Moves the serial of the fragment the DMA reads to the ring fragment it
 * reads now.
 *
 * The serial counts every fragment since the start, so it moves by the
 * ring distance from the old fragment to the new one: an interrupt that
 * was missed still moves it the right way.  A gap of a whole ring or more
 * cannot be told from a shorter one.
 */
uint64_t
drv_rpi5_hdmi_audio_advance(
	uint64_t serial,
	uint32_t fragment)
{
	uint32_t current;
	uint32_t steps;

	/* The ring fragment the serial names now. */
	current = (uint32_t)(serial % DRV_RPI5_HDMI_AUDIO_FRAGMENT_COUNT);

	/* The fragments the DMA has gone through since, going round the ring. */
	steps = (fragment + DRV_RPI5_HDMI_AUDIO_FRAGMENT_COUNT - current) % DRV_RPI5_HDMI_AUDIO_FRAGMENT_COUNT;

	/* Succeeded: the serial of the fragment the DMA reads. */
	return serial + steps;
}

/*
 * Works out the TMDS character rate the firmware set the PHY to, from the
 * rate manager's offset, the PLL's reference rate and the VCO divider.
 *
 * The offset is the VCO over the reference, scaled by 2^21; the VCO runs at
 * ten times the divider times the TMDS character rate.  A readback that
 * does not have that form gives no rate.
 *
 * Values referenced: Raspberry Pi Linux rpi-6.12.y ab4372076bb8
 * drivers/gpu/drm/vc4/vc4_hdmi_phy.c (phy_get_rm_offset,
 * vc6_phy_get_vco_freq and the fields vc6_hdmi_phy_init writes).
 */
enum drv_rpi5_hdmi_audio_tmds_status
drv_rpi5_hdmi_audio_tmds_rate(
	uint32_t rate_offset,
	uint32_t reference,
	uint32_t vco_divider,
	uint64_t *tmds_hz)
{
	uint64_t offset;
	uint64_t reference_hz;
	uint64_t divisor;
	uint64_t rate;

	/* Refuses an offset the rate manager does not hold fixed. */
	if ((rate_offset & DRV_RPI5_HDMI_AUDIO_RATE_FIXED) == 0U)
		return DRV_RPI5_HDMI_AUDIO_TMDS_NOT_FIXED;

	/* Refuses a VCO whose divider is off or zero. */
	if ((vco_divider & DRV_RPI5_HDMI_AUDIO_VCO_DIVIDER_ON) == 0U)
		return DRV_RPI5_HDMI_AUDIO_TMDS_NO_DIVIDER;
	if ((vco_divider & DRV_RPI5_HDMI_AUDIO_VCO_DIVIDER_MASK) == 0U)
		return DRV_RPI5_HDMI_AUDIO_TMDS_NO_DIVIDER;

	/* Refuses a PLL without a reference rate. */
	if ((reference & DRV_RPI5_HDMI_AUDIO_REFERENCE_MHZ_MASK) == 0U)
		return DRV_RPI5_HDMI_AUDIO_TMDS_NO_REFERENCE;

	/* The three readbacks as numbers. */
	offset = rate_offset & DRV_RPI5_HDMI_AUDIO_RATE_OFFSET_MASK;
	reference_hz = (uint64_t)(reference & DRV_RPI5_HDMI_AUDIO_REFERENCE_MHZ_MASK) * 1000000ULL;
	divisor = (uint64_t)(vco_divider & DRV_RPI5_HDMI_AUDIO_VCO_DIVIDER_MASK) * 10ULL;
	divisor <<= DRV_RPI5_HDMI_AUDIO_RATE_OFFSET_SHIFT;

	/* The TMDS character rate, rounded to the nearest Hz. */
	rate = (offset * reference_hz + divisor / 2U) / divisor;

	/* Refuses a rate no HDMI mode runs at. */
	if (rate < DRV_RPI5_HDMI_AUDIO_TMDS_MIN)
		return DRV_RPI5_HDMI_AUDIO_TMDS_OUT_OF_RANGE;
	if (rate > DRV_RPI5_HDMI_AUDIO_TMDS_MAX)
		return DRV_RPI5_HDMI_AUDIO_TMDS_OUT_OF_RANGE;

	/* Succeeded: the TMDS character rate. */
	*tmds_hz = rate;
	return DRV_RPI5_HDMI_AUDIO_TMDS_OK;
}

/*
 * Chooses the N and the CTS of the audio clock regeneration for a TMDS
 * character rate.
 *
 * A measured rate close to a known one is taken for it, so the 1000/1001
 * rates get their exact CTS.  hardware_cts asks for the form in which the
 * port measures CTS itself: N must then be a multiple of 128, so it is the
 * default 6144, and the CTS is only the nearest whole number.  Otherwise N
 * comes from the table, or from a search for a whole CTS for a deep color
 * rate; a rate with no whole CTS in reach, or an unknown rate, keeps 6144
 * and the nearest CTS.
 *
 * Values referenced: Raspberry Pi Linux rpi-6.12.y ab4372076bb8
 * drivers/gpu/drm/vc4/vc4_regs.h (the comment on the external CTS bit:
 * N divisible by 128), drivers/gpu/drm/vc4/vc4_hdmi.c (N of 128 fs / 1000
 * in vc4_hdmi_set_n_cts).
 */
void
drv_rpi5_hdmi_audio_clock(
	uint64_t tmds_hz,
	bool hardware_cts,
	struct drv_rpi5_hdmi_audio_clock *clock)
{
	struct clock_rate rate;
	uint32_t table_n;
	uint32_t n;
	bool known;
	bool found;

	/* Takes the measured rate for a known one when it is close enough. */
	known = find_rate(tmds_hz, &rate, &table_n);

	/* An unknown rate keeps the default N and the CTS of the measured rate. */
	if (!known) {
		rate.numerator = tmds_hz;
		rate.denominator = 1U;
		clock->n = CLOCK_N_DEFAULT;
		clock->cts = rounded_cts(&rate, CLOCK_N_DEFAULT);
		clock->exact = false;
		return;
	}

	/* The port's own CTS measurement needs an N that is a multiple of 128. */
	if (hardware_cts) {
		clock->n = CLOCK_N_DEFAULT;
		clock->cts = rounded_cts(&rate, CLOCK_N_DEFAULT);
		clock->exact = exact_cts(&rate, CLOCK_N_DEFAULT);
		return;
	}

	/* An 8-bit rate takes the table's N. */
	if (table_n != 0U) {
		clock->n = table_n;
		clock->cts = rounded_cts(&rate, table_n);
		clock->exact = exact_cts(&rate, table_n);
		return;
	}

	/* A deep color rate searches for an N with a whole CTS. */
	found = find_n(&rate, &n);
	if (!found) {
		clock->n = CLOCK_N_DEFAULT;
		clock->cts = rounded_cts(&rate, CLOCK_N_DEFAULT);
		clock->exact = false;
		return;
	}

	/* Succeeded: an N whose CTS is whole. */
	clock->n = n;
	clock->cts = rounded_cts(&rate, n);
	clock->exact = true;
}

/*
 * Chooses the audio sample divider: the sample period is periods over
 * samples cycles of the audio clock, the closest fraction to the clock
 * over the sample rate that the register's fields hold.
 *
 * The walk goes through the continued fraction of clock / rate; when the
 * next convergent would no longer fit, the largest fitting intermediate
 * fraction is compared with the last convergent.
 *
 * Values referenced: Raspberry Pi Linux rpi-6.12.y ab4372076bb8
 * drivers/gpu/drm/vc4/vc4_regs.h (the sample divider's fields),
 * drivers/gpu/drm/vc4/vc4_hdmi.c (the best approximation and its limits in
 * vc4_hdmi_audio_set_mai_clock).
 */
void
drv_rpi5_hdmi_audio_sample_divider(
	uint32_t clock_hz,
	uint32_t rate,
	uint32_t *periods,
	uint32_t *samples)
{
	uint64_t numerator;
	uint64_t denominator;
	uint64_t remainder;
	uint64_t term;
	uint64_t p_before;
	uint64_t q_before;
	uint64_t p_last;
	uint64_t q_last;
	uint64_t p_next;
	uint64_t q_next;
	uint64_t steps;
	uint64_t steps_q;
	uint64_t p_middle;
	uint64_t q_middle;
	uint64_t error_last;
	uint64_t error_middle;

	/* The walk starts before the first convergent. */
	numerator = clock_hz;
	denominator = rate;
	p_before = 0U;
	q_before = 1U;
	p_last = 1U;
	q_last = 0U;

	/* Goes through the continued fraction while its convergents fit. */
	while (denominator != 0U) {
		/* The next term of the continued fraction and the convergent it makes. */
		term = numerator / denominator;
		remainder = numerator % denominator;
		p_next = term * p_last + p_before;
		q_next = term * q_last + q_before;

		/* Stops at the first convergent that no longer fits. */
		if (p_next > SAMPLE_PERIODS_MAX || q_next > SAMPLE_SAMPLES_MAX)
			break;

		/* Moves on to the convergent, and to the rest of the fraction. */
		p_before = p_last;
		q_before = q_last;
		p_last = p_next;
		q_last = q_next;
		numerator = denominator;
		denominator = remainder;
	}

	/* An exact fraction that fits needs no intermediate one. */
	if (denominator == 0U) {
		*periods = (uint32_t)p_last;
		*samples = (uint32_t)q_last;
		return;
	}

	/* The largest number of steps from the convergent before the last that still fits. */
	steps = (SAMPLE_PERIODS_MAX - p_before) / p_last;
	if (q_last != 0U) {
		steps_q = (SAMPLE_SAMPLES_MAX - q_before) / q_last;
		if (steps_q < steps)
			steps = steps_q;
	}

	/* No intermediate fraction fits, so the last convergent stands. */
	if (steps == 0U) {
		*periods = (uint32_t)p_last;
		*samples = (uint32_t)q_last;
		return;
	}

	/* The intermediate fraction, which is the only choice before any convergent. */
	p_middle = steps * p_last + p_before;
	q_middle = steps * q_last + q_before;
	if (q_last == 0U) {
		*periods = (uint32_t)p_middle;
		*samples = (uint32_t)q_middle;
		return;
	}

	/*
	 * The errors of the two candidates against clock / rate, both scaled
	 * by rate times the product of their sample counts.
	 */
	error_last = distance(p_last * rate, q_last * clock_hz) * q_middle;
	error_middle = distance(p_middle * rate, q_middle * clock_hz) * q_last;

	/* Takes the intermediate fraction when it is strictly closer. */
	if (error_middle < error_last) {
		*periods = (uint32_t)p_middle;
		*samples = (uint32_t)q_middle;
		return;
	}

	/* Succeeded: the last convergent is the closest. */
	*periods = (uint32_t)p_last;
	*samples = (uint32_t)q_last;
}

/*
 * Packs the sample divider into its register word: the periods in bits 8
 * to 31, the samples less one in bits 0 to 7.
 *
 * Values referenced: Raspberry Pi Linux rpi-6.12.y ab4372076bb8
 * drivers/gpu/drm/vc4/vc4_regs.h (the sample divider's fields).
 */
uint32_t
drv_rpi5_hdmi_audio_sample_word(
	uint32_t periods,
	uint32_t samples)
{
	uint32_t word;

	/* The periods above the samples less one. */
	word = (periods & SAMPLE_PERIODS_MAX) << 8;
	word |= (samples - 1U) & 0xffU;

	/* Succeeded: the register word. */
	return word;
}

/*
 * Builds the audio InfoFrame of plain PCM with a channel count: the header,
 * the checksum and the payload, fourteen bytes.
 *
 * The coding type, the sample rate and the sample size say "refer to the
 * stream", the channel allocation is the front pair, and the checksum
 * makes every byte add up to zero.
 *
 * Values referenced: Raspberry Pi Linux rpi-6.12.y ab4372076bb8
 * include/linux/hdmi.h (the audio InfoFrame's type, version and length),
 * drivers/video/hdmi.c (the payload bits and the checksum in
 * hdmi_audio_infoframe_pack_only), sound/soc/codecs/hdmi-codec.c (the
 * stream-referred fields).
 */
void
drv_rpi5_hdmi_audio_infoframe(
	uint8_t *frame,
	uint32_t channels)
{
	uint32_t index;
	uint32_t sum;

	/* Starts from an all-zero frame. */
	for (index = 0U; index < DRV_RPI5_HDMI_AUDIO_INFOFRAME_BYTES; index++)
		frame[index] = 0U;

	/* The header: type, version and payload length. */
	frame[0] = DRV_RPI5_HDMI_AUDIO_INFOFRAME_TYPE;
	frame[1] = DRV_RPI5_HDMI_AUDIO_INFOFRAME_VERSION;
	frame[2] = DRV_RPI5_HDMI_AUDIO_INFOFRAME_LENGTH;

	/* The first payload byte holds the channel count less one; one channel is coded as zero too. */
	if (channels >= 2U)
		frame[4] = (uint8_t)((channels - 1U) & 0x07U);

	/* The checksum makes the sum of all fourteen bytes a multiple of 256. */
	sum = 0U;
	for (index = 0U; index < DRV_RPI5_HDMI_AUDIO_INFOFRAME_BYTES; index++)
		sum += frame[index];
	frame[3] = (uint8_t)((256U - (sum & 0xffU)) & 0xffU);
}

/*
 * Lays an InfoFrame out as the nine words of its packet RAM slot.
 *
 * Each seven bytes take two words, three bytes in the first and four in
 * the second, low byte first; the words after the frame are zero, since
 * the port sends the whole slot.
 *
 * Procedure referenced: Raspberry Pi Linux rpi-6.12.y ab4372076bb8
 * drivers/gpu/drm/vc4/vc4_hdmi.c (vc4_hdmi_write_infoframe).
 */
void
drv_rpi5_hdmi_audio_packet_words(
	const uint8_t *frame,
	uint32_t *words)
{
	uint32_t group;
	uint32_t index;
	const uint8_t *bytes;

	/* Packs each group of seven bytes into two words. */
	for (group = 0U; group < DRV_RPI5_HDMI_AUDIO_INFOFRAME_BYTES / 7U; group++) {
		/* The group's first byte. */
		bytes = frame + group * 7U;

		/* The first word holds three bytes. */
		words[group * 2U] = (uint32_t)bytes[0];
		words[group * 2U] |= (uint32_t)bytes[1] << 8;
		words[group * 2U] |= (uint32_t)bytes[2] << 16;

		/* The second word holds four. */
		words[group * 2U + 1U] = (uint32_t)bytes[3];
		words[group * 2U + 1U] |= (uint32_t)bytes[4] << 8;
		words[group * 2U + 1U] |= (uint32_t)bytes[5] << 16;
		words[group * 2U + 1U] |= (uint32_t)bytes[6] << 24;
	}

	/* Clears the rest of the slot. */
	for (index = (DRV_RPI5_HDMI_AUDIO_INFOFRAME_BYTES / 7U) * 2U; index < DRV_RPI5_HDMI_AUDIO_PACKET_SLOT_WORDS; index++)
		words[index] = 0U;
}

/*
 * Reports the byte offset, in the packet RAM, of the slot an InfoFrame
 * type goes into: the type less 0x80 is the slot number.
 *
 * Value referenced: Raspberry Pi Linux rpi-6.12.y ab4372076bb8
 * drivers/gpu/drm/vc4/vc4_hdmi.c (the packet id of an InfoFrame type).
 */
uint32_t
drv_rpi5_hdmi_audio_packet_slot(
	uint32_t type)
{
	/* The slot's offset: its number times the slot size. */
	return (type - 0x80U) * DRV_RPI5_HDMI_AUDIO_PACKET_SLOT_BYTES;
}

/*
 * Reads what a device tree DMA cell asks of its channel.
 */
void
drv_rpi5_hdmi_audio_dma_decode(
	uint32_t cell,
	struct drv_rpi5_hdmi_audio_dma_request *request)
{
	/* The DREQ and the two priorities. */
	request->dreq = cell & CELL_DREQ_MASK;
	request->priority = (cell >> CELL_PRIORITY_SHIFT) & CELL_PRIORITY_MASK;
	request->panic_priority = (cell >> CELL_PANIC_SHIFT) & CELL_PRIORITY_MASK;

	/* Starts from narrow single reads and writes that wait for their response. */
	request->wide_source = false;
	request->wide_destination = false;
	request->wait_write = true;
	request->burst = false;
	request->drain_writes = false;
	request->no_debug = false;

	/* Reads 128 bits at a time from memory. */
	if ((cell & CELL_WIDE_SOURCE) != 0U)
		request->wide_source = true;

	/* Writes 128 bits at a time to the peripheral. */
	if ((cell & CELL_WIDE_DESTINATION) != 0U)
		request->wide_destination = true;

	/* Lets writes go without waiting for their response. */
	if ((cell & CELL_NO_WAIT_WRITE) != 0U)
		request->wait_write = false;

	/* Moves data in bursts. */
	if ((cell & CELL_BURST) != 0U)
		request->burst = true;

	/* Drains the outstanding writes at the end of each block. */
	if ((cell & CELL_DRAIN_WRITES) != 0U)
		request->drain_writes = true;

	/* Keeps running when the debugger pauses the system. */
	if ((cell & CELL_NO_DEBUG) != 0U)
		request->no_debug = true;
}

/*
 * Makes the channel status word that runs the channel with a request's
 * priorities.  The same word with the interrupt bit added clears an
 * interrupt without stopping the channel.
 *
 * Procedure referenced: Raspberry Pi Linux rpi-6.12.y ab4372076bb8
 * drivers/dma/bcm2835-dma.c (bcm2835_dma_start_desc and
 * bcm2835_dma_callback for a 40-bit channel).
 */
uint32_t
drv_rpi5_hdmi_audio_dma_run(
	const struct drv_rpi5_hdmi_audio_dma_request *request)
{
	uint32_t word;

	/* Running, protected, at the request's priorities. */
	word = DRV_RPI5_HDMI_AUDIO_DMA_CS_RUN | DRV_RPI5_HDMI_AUDIO_DMA_CS_PROTECTION;
	word |= (request->priority & CELL_PRIORITY_MASK) << DRV_RPI5_HDMI_AUDIO_DMA_CS_PRIORITY_SHIFT;
	word |= (request->panic_priority & CELL_PRIORITY_MASK) << DRV_RPI5_HDMI_AUDIO_DMA_CS_PANIC_SHIFT;

	/* Waits for the writes at the end of each block when asked. */
	if (request->drain_writes)
		word |= DRV_RPI5_HDMI_AUDIO_DMA_CS_DRAIN_WRITES;

	/* Ignores the debugger's pause when asked. */
	if (request->no_debug)
		word |= DRV_RPI5_HDMI_AUDIO_DMA_CS_NO_DEBUG;

	/* Succeeded: the run word. */
	return word;
}

/*
 * Builds a ring of count control blocks: block i moves fragment i of the
 * subframe ring to the audio FIFO, paced by the port's DREQ, raises an
 * interrupt at its end and goes on to block i + 1, the last back to the
 * first.  cb_bus is the bus address of cbs[0].
 *
 * The ring is refused when a control block is misaligned or beyond what
 * the channel's control block field can name, when an address is beyond
 * 40 bits, or when the data crosses a 4 GiB boundary (the position is
 * read from two registers that are not read at once).
 *
 * Procedure referenced: Raspberry Pi Linux rpi-6.12.y ab4372076bb8
 * drivers/dma/bcm2835-dma.c (bcm2835_dma_prep_dma_cyclic and
 * bcm2835_dma_create_cb_chain for a 40-bit channel).
 */
bool
drv_rpi5_hdmi_audio_dma_ring(
	struct drv_rpi5_hdmi_audio_dma_cb *cbs,
	uint32_t count,
	uint64_t cb_bus,
	uint64_t data_bus,
	uint32_t fragment_bytes,
	uint64_t fifo_bus,
	const struct drv_rpi5_hdmi_audio_dma_request *request)
{
	uint64_t ring_bytes;
	uint64_t ring_end;
	uint64_t source;
	uint64_t next_bus;
	uint32_t transfer;
	uint32_t source_info;
	uint32_t destination_info;
	uint32_t index;
	uint32_t next;

	/* Refuses an empty ring. */
	if (count == 0U || fragment_bytes == 0U)
		return false;

	/* Refuses control blocks the channel cannot name. */
	if ((cb_bus % DRV_RPI5_HDMI_AUDIO_DMA_CB_ALIGN) != 0U)
		return false;
	if (cb_bus + (uint64_t)count * sizeof(struct drv_rpi5_hdmi_audio_dma_cb) > DMA_CB_LIMIT)
		return false;

	/* Refuses data or a FIFO beyond 40 bits. */
	ring_bytes = (uint64_t)count * fragment_bytes;
	ring_end = data_bus + ring_bytes;
	if (ring_end > DMA_ADDRESS_LIMIT)
		return false;
	if (fifo_bus >= DMA_ADDRESS_LIMIT)
		return false;

	/* Refuses data that crosses a 4 GiB boundary. */
	if ((data_bus >> DMA_WINDOW_SHIFT) != ((ring_end - 1U) >> DMA_WINDOW_SHIFT))
		return false;

	/* The transfer word every block shares: interrupt, write response, DREQ pacing. */
	transfer = DRV_RPI5_HDMI_AUDIO_DMA_TI_INTERRUPT | DRV_RPI5_HDMI_AUDIO_DMA_TI_PACE_WRITE;
	transfer |= (request->dreq & DRV_RPI5_HDMI_AUDIO_DMA_TI_DREQ_MASK) << DRV_RPI5_HDMI_AUDIO_DMA_TI_DREQ_SHIFT;
	if (request->wait_write)
		transfer |= DRV_RPI5_HDMI_AUDIO_DMA_TI_WAIT_WRITE;

	/* The source reads move on through memory, wide and in bursts when asked. */
	source_info = DRV_RPI5_HDMI_AUDIO_DMA_INCREMENT;
	if (request->wide_source)
		source_info |= DRV_RPI5_HDMI_AUDIO_DMA_WIDE;
	if (request->burst)
		source_info |= DMA_BURST_LENGTH << DRV_RPI5_HDMI_AUDIO_DMA_BURST_SHIFT;

	/* The destination writes stay on the FIFO, wide and in bursts when asked. */
	destination_info = (uint32_t)(fifo_bus >> 32) & DRV_RPI5_HDMI_AUDIO_DMA_HIGH_MASK;
	if (request->wide_destination)
		destination_info |= DRV_RPI5_HDMI_AUDIO_DMA_WIDE;
	if (request->burst)
		destination_info |= DMA_BURST_LENGTH << DRV_RPI5_HDMI_AUDIO_DMA_BURST_SHIFT;

	/* Fills each block and links it to the next, the last to the first. */
	for (index = 0U; index < count; index++) {
		/* The block that follows, the first after the last. */
		next = index + 1U;
		if (next == count)
			next = 0U;

		/* The fragment this block reads and the bus address of the next block. */
		source = data_bus + (uint64_t)index * fragment_bytes;
		next_bus = cb_bus + (uint64_t)next * sizeof(struct drv_rpi5_hdmi_audio_dma_cb);

		/* The block's eight words. */
		cbs[index].transfer = transfer;
		cbs[index].source = (uint32_t)source;
		cbs[index].source_info = source_info | ((uint32_t)(source >> 32) & DRV_RPI5_HDMI_AUDIO_DMA_HIGH_MASK);
		cbs[index].destination = (uint32_t)fifo_bus;
		cbs[index].destination_info = destination_info;
		cbs[index].length = fragment_bytes;
		cbs[index].next = (uint32_t)(next_bus >> DRV_RPI5_HDMI_AUDIO_DMA_CB_SHIFT);
		cbs[index].reserved = 0U;
	}

	/* Succeeded: the ring is ready for the channel. */
	return true;
}

/*
 * Tells which fragment the DMA reads, and where it is in the 16-bit PCM
 * ring, from the channel's source address registers.
 *
 * A subframe is twice a sample's bytes, so the PCM offset is half the
 * subframe offset.  The end of the ring is its start.  An address outside
 * the ring gives false.
 */
bool
drv_rpi5_hdmi_audio_dma_locate(
	uint32_t source,
	uint32_t source_info,
	uint64_t data_bus,
	uint32_t fragment_bytes,
	uint32_t count,
	uint32_t *fragment,
	uint32_t *pcm_offset)
{
	uint64_t address;
	uint64_t offset;
	uint64_t ring_bytes;

	/* The 40-bit source address from its two halves. */
	address = ((uint64_t)(source_info & DRV_RPI5_HDMI_AUDIO_DMA_HIGH_MASK) << 32) | source;

	/* Refuses an address below the ring. */
	if (address < data_bus)
		return false;

	/* Refuses an address beyond the ring's end. */
	offset = address - data_bus;
	ring_bytes = (uint64_t)count * fragment_bytes;
	if (offset > ring_bytes)
		return false;

	/* The end of the ring wraps to its start. */
	if (offset == ring_bytes)
		offset = 0U;

	/* Succeeded: the fragment and the PCM offset. */
	*fragment = (uint32_t)(offset / fragment_bytes);
	*pcm_offset = (uint32_t)(offset / 2U);
	return true;
}

/*
 * Tells the chip's stepping from the HVS version register.
 *
 * Procedure referenced: Raspberry Pi Linux rpi-6.12.y ab4372076bb8
 * drivers/gpu/drm/vc4/vc4_hvs.c (the D0 check in vc4_hvs_bind).
 */
enum drv_rpi5_hdmi_audio_stepping
drv_rpi5_hdmi_audio_stepping(
	uint32_t hvs_version)
{
	uint32_t version;

	/* The version is the low byte. */
	version = hvs_version & HVS_VERSION_MASK;

	/* Names the stepping the version belongs to. */
	if (version == HVS_VERSION_C)
		return DRV_RPI5_HDMI_AUDIO_STEPPING_C;
	if (version == HVS_VERSION_D0)
		return DRV_RPI5_HDMI_AUDIO_STEPPING_D0;

	/* A version neither stepping has. */
	return DRV_RPI5_HDMI_AUDIO_STEPPING_UNKNOWN;
}

/*
 * Gives the audio DREQ of an HDMI port for the stepping, and tells whether
 * the device tree's DMA cell agrees.
 *
 * The firmware may hand a device tree made for the other stepping, so the
 * stepping's table decides.  An unknown stepping or port gives DREQ 0 and
 * false.
 */
bool
drv_rpi5_hdmi_audio_dreq(
	enum drv_rpi5_hdmi_audio_stepping stepping,
	uint32_t port,
	uint32_t tree_cell,
	uint32_t *dreq)
{
	/* Refuses a port the chip does not have. */
	*dreq = 0U;
	if (port >= PORT_COUNT)
		return false;

	/* Takes the stepping's DREQ. */
	if (stepping == DRV_RPI5_HDMI_AUDIO_STEPPING_C) {
		*dreq = dreq_c[port];
	} else if (stepping == DRV_RPI5_HDMI_AUDIO_STEPPING_D0) {
		*dreq = dreq_d0[port];
	} else {
		return false;
	}

	/* Reports a device tree made for the other stepping. */
	if ((tree_cell & CELL_DREQ_MASK) != *dreq)
		return false;

	/* Succeeded: the device tree agrees with the stepping. */
	return true;
}

/*
 * Packs the audio FIFO's panic and DREQ thresholds into the threshold
 * register word of the stepping.  An unknown stepping gives false.
 */
bool
drv_rpi5_hdmi_audio_thresholds(
	enum drv_rpi5_hdmi_audio_stepping stepping,
	uint32_t *word)
{
	/* The C layout: six-bit fields at 24, 16, 8 and 0. */
	if (stepping == DRV_RPI5_HDMI_AUDIO_STEPPING_C) {
		*word = THRESHOLD_PANIC << THRESHOLD_C_PANIC_HIGH_SHIFT;
		*word |= THRESHOLD_PANIC << THRESHOLD_C_PANIC_LOW_SHIFT;
		*word |= THRESHOLD_DREQ << THRESHOLD_C_DREQ_HIGH_SHIFT;
		*word |= THRESHOLD_DREQ << THRESHOLD_C_DREQ_LOW_SHIFT;
		return true;
	}

	/* The D0 layout: seven-bit fields at 23, 15, 7 and 0. */
	if (stepping == DRV_RPI5_HDMI_AUDIO_STEPPING_D0) {
		*word = THRESHOLD_PANIC << THRESHOLD_D0_PANIC_HIGH_SHIFT;
		*word |= THRESHOLD_PANIC << THRESHOLD_D0_PANIC_LOW_SHIFT;
		*word |= THRESHOLD_DREQ << THRESHOLD_D0_DREQ_HIGH_SHIFT;
		*word |= THRESHOLD_DREQ << THRESHOLD_D0_DREQ_LOW_SHIFT;
		return true;
	}

	/* A stepping without a known layout. */
	*word = 0U;
	return false;
}

/* Reports the parity of bits 4 to 30 of a subframe: 1 when their ones are odd. */
static uint32_t
parity(
	uint32_t word)
{
	uint32_t bits;
	uint32_t ones;

	/* Counts the ones of bits 4 to 30. */
	bits = (word >> 4) & 0x07ffffffU;
	ones = 0U;
	while (bits != 0U) {
		ones += bits & 1U;
		bits >>= 1;
	}

	/* The low bit of the count. */
	return ones & 1U;
}

/* Tells whether a measured rate lies within the match tolerance of a known one. */
static bool
rate_matches(
	uint64_t tmds_hz,
	const struct clock_rate *rate)
{
	uint64_t scaled;
	uint64_t gap;

	/* The measured rate on the known rate's denominator. */
	scaled = tmds_hz * rate->denominator;
	gap = distance(scaled, rate->numerator);

	/* Close enough when the gap is within the tolerance of the known rate. */
	if (gap * 1000000ULL <= rate->numerator * CLOCK_MATCH_PPM)
		return true;

	/* Too far. */
	return false;
}

/*
 * Finds the known rate a measured rate stands for: an 8-bit rate of the
 * table (its N in table_n), or one of them times a deep color factor
 * (table_n 0).
 */
static bool
find_rate(
	uint64_t tmds_hz,
	struct clock_rate *rate,
	uint32_t *table_n)
{
	uint32_t entry;
	uint32_t factor;
	struct clock_rate candidate;
	bool matches;

	/* Tries the 8-bit rates first. */
	*table_n = 0U;
	for (entry = 0U; entry < sizeof(clock_table) / sizeof(clock_table[0]); entry++) {
		/* Takes the first 8-bit rate the measured rate is close to. */
		matches = rate_matches(tmds_hz, &clock_table[entry].rate);
		if (matches) {
			*rate = clock_table[entry].rate;
			*table_n = clock_table[entry].n;
			return true;
		}
	}

	/* Then each 8-bit rate times each deep color factor. */
	for (factor = 0U; factor < sizeof(clock_factors) / sizeof(clock_factors[0]); factor++) {
		for (entry = 0U; entry < sizeof(clock_table) / sizeof(clock_table[0]); entry++) {
			/* The 8-bit rate times the factor, and whether the measured rate is close to it. */
			candidate.numerator = clock_table[entry].rate.numerator * clock_factors[factor].numerator;
			candidate.denominator = clock_table[entry].rate.denominator * clock_factors[factor].denominator;
			matches = rate_matches(tmds_hz, &candidate);
			if (matches) {
				*rate = candidate;
				return true;
			}
		}
	}

	/* No known rate is close. */
	return false;
}

/*
 * Searches for the N nearest the default, within the recommended range,
 * that makes the CTS of a rate a whole number.
 */
static bool
find_n(
	const struct clock_rate *rate,
	uint32_t *n)
{
	uint32_t step;
	uint32_t below;
	uint32_t above;
	bool whole;

	/* Walks outward from the default, the smaller N first at each distance. */
	for (step = 0U; step <= CLOCK_N_MAX - CLOCK_N_DEFAULT; step++) {
		/* The two N at this distance from the default. */
		below = CLOCK_N_DEFAULT - step;
		above = CLOCK_N_DEFAULT + step;

		/* Tries the N below the default while it stays in range. */
		if (step <= CLOCK_N_DEFAULT - CLOCK_N_MIN) {
			whole = exact_cts(rate, below);
			if (whole) {
				*n = below;
				return true;
			}
		}

		/* Then the N above. */
		whole = exact_cts(rate, above);
		if (whole) {
			*n = above;
			return true;
		}
	}

	/* No N in the range gives a whole CTS. */
	return false;
}

/* Reports the CTS of a rate and an N, rounded to the nearest whole number. */
static uint32_t
rounded_cts(
	const struct clock_rate *rate,
	uint32_t n)
{
	uint64_t divisor;

	/* CTS = rate * N / (128 * 48000), the rate being a fraction. */
	divisor = (uint64_t)rate->denominator * CLOCK_DIVISOR;

	/* The CTS rounded half up. */
	return (uint32_t)((rate->numerator * n + divisor / 2U) / divisor);
}

/* Tells whether the CTS of a rate and an N is a whole number. */
static bool
exact_cts(
	const struct clock_rate *rate,
	uint32_t n)
{
	uint64_t divisor;

	/* The CTS's divisor. */
	divisor = (uint64_t)rate->denominator * CLOCK_DIVISOR;

	/* Whole when the divisor divides the product exactly. */
	if ((rate->numerator * n) % divisor == 0U)
		return true;

	/* A fraction is left. */
	return false;
}

/* Reports the distance between two numbers. */
static uint64_t
distance(
	uint64_t a,
	uint64_t b)
{
	/* The larger less the smaller. */
	if (a > b)
		return a - b;

	/* The other way round. */
	return b - a;
}
