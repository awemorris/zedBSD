/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The VisionFive 2's headphone jack.
 *
 * The JH7110's PWM audio converter drives the jack's two channels from
 * GPIO 33 and 34.  Each 32-bit word written to its data register is one
 * stereo frame of two signed 16-bit samples, so the framework's ring is
 * fed to the register as it is.  The converter has no interrupt; the AXI
 * DMA controller moves the ring to the register, paced by the converter's
 * request line, one period a fragment, and its interrupt reports each
 * fragment.
 *
 * The converter's sample clock is the audio root divided down; at 8-bit
 * resolution one sample takes 256 of its cycles, so 48 kHz wants
 * 12.288 MHz.  The rate the divider reaches is logged.
 *
 * Playback only: the jack has no microphone input.  No hardware volume:
 * audiod scales the samples.
 */

#include <drivers/audio/audio.h>
#include <drivers/dma/dw-axi-dmac.h>
#include <drivers/generic/dma.h>
#include <drivers/generic/fdt.h>
#include <drivers/platform/visionfive2/vf2-crg.h>
#include <drivers/platform/visionfive2/vf2-dmac.h>
#include <drivers/platform/visionfive2/vf2-pinctrl.h>
#include <drivers/platform/visionfive2/vf2-pwmdac.h>
#include <kern/device-io.h>
#include <kern/kcrt.h>
#include <kern/klog.h>
#include <kern/pmem.h>
#include <uapi/audio.h>
#include <uapi/errno.h>

/*
 * The converter's registers and the fields of its control register.
 *
 * The data register takes one frame a write.  The control word below
 * selects 8-bit resolution (shift clear), a centred pulse (duty 2), one
 * PWM period a sample (count less one is zero), and signed data (the
 * sample's top bit inverted).
 *
 * Values referenced:
 * Linux v6.12 sound/soc/starfive/jh7110_pwmdac.c (the data and control
 * register offsets; the enable bit, the duty cycle field at bit 2 and its
 * centre value 2, the count field at bit 4, the data mode bit 14 and its
 * inverted-top-bit value; jh7110_pwmdac_init_params: the default
 * settings).
 */
#define PWMDAC_DATA			0x00U
#define PWMDAC_CONTROL			0x04U
#define PWMDAC_CONTROL_ENABLE		0x00000001U
#define PWMDAC_CONTROL_CENTRED		0x00000008U
#define PWMDAC_CONTROL_SIGNED		0x00004000U
#define PWMDAC_REGISTER_BYTES		0x1000U

/*
 * The sample clock for 48 kHz and the PWM cycles in one sample.
 *
 * Values referenced:
 * Linux v6.12 sound/soc/starfive/jh7110_pwmdac.c
 * (jh7110_pwmdac_hw_params: 12288000 Hz with one period a sample for
 * 48000 Hz).
 */
#define PWMDAC_SAMPLE_CLOCK		12288000U
#define PWMDAC_CYCLES_PER_SAMPLE	256U

/* The one rate and shape the jack plays. */
#define PWMDAC_RATE			48000U
#define PWMDAC_FRAME_BYTES		4U

/* The formats the jack plays. */
static const struct audio_format pwmdac_formats[] = {
	{ KERN_AUDIO_FORMAT_S16_LE, 2U, PWMDAC_RATE, 0U }
};

/*
 * The jack: the converter's registers, its DMA channel and the audio
 * device it is published as.
 *
 * Filled by drv_vf2_pwmdac_attach() and kept for good.  Prepared says the
 * channel holds a ring the start may play.
 */
struct vf2_pwmdac {
	volatile uint8_t *registers;
	uint64_t physical;
	uint32_t handshake;
	struct drv_dw_axi_dmac *dmac;
	struct drv_dw_axi_dmac_channel *channel;
	struct drv_audio_ops ops;
	struct drv_audio_device *audio;
	int prepared;
};

/*
 * The board's one jack.
 *
 * Registers is NULL until the attach has mapped the converter.
 */
static struct vf2_pwmdac board_pwmdac;

static int find_converter(const struct drv_fdt *fdt, uint32_t *node);
static int find_dma(const struct drv_fdt *fdt, uint32_t node, uint32_t *controller, uint32_t *handshake);
static int switch_on(const struct drv_fdt *fdt, uint32_t node);
static int set_sample_clock(const struct drv_fdt *fdt, uint32_t node);
static void write_control(struct vf2_pwmdac *pwmdac, uint32_t value);
static void period_done(void *argument);
static int pwmdac_prepare(void *private_data, int capture, const struct audio_format *format, const struct drv_dma_buffer *buffer, uint32_t fragment_bytes, uint32_t fragment_count);
static int pwmdac_start(void *private_data, int capture);
static void pwmdac_stop(void *private_data, int capture);
static uint32_t pwmdac_position(void *private_data, int capture);

/*
 * Brings up the converter, its pins and its DMA channel and publishes the
 * jack as an audio device.
 */
int
drv_vf2_pwmdac_attach(
	const struct drv_fdt *fdt,
	int board_default_coherent)
{
	struct vf2_pwmdac *pwmdac;
	struct drv_dma_device *dma;
	uint32_t controller_node;
	uint32_t node;
	uint64_t size;
	void *mapped;
	int error;

	/* Refuses a missing tree or a second attachment. */
	pwmdac = &board_pwmdac;
	if (fdt == NULL)
		return EINVAL;
	if (pwmdac->registers != NULL)
		return EALREADY;

	/* Finds the enabled converter. */
	error = find_converter(fdt, &node);
	if (error != 0)
		return error;

	/* Finds the DMA controller and the request line the converter names. */
	error = find_dma(fdt, node, &controller_node, &pwmdac->handshake);
	if (error != 0)
		return error;

	/* Brings up the DMA controller. */
	error = drv_vf2_dmac_get(fdt, controller_node, board_default_coherent, &pwmdac->dmac);
	if (error != 0) {
		kern_logf("vf2-pwmdac: no DMA controller (%d)\n", error);
		return error;
	}

	/* Takes a channel of the controller. */
	error = drv_dw_axi_dmac_channel_get(pwmdac->dmac, &pwmdac->channel);
	if (error != 0)
		return error;

	/* Gives the converter its pins, clocks and reset. */
	error = switch_on(fdt, node);
	if (error != 0)
		return error;

	/* Sets the converter's sample clock. */
	error = set_sample_clock(fdt, node);
	if (error != 0)
		return error;

	/* Finds the converter's registers. */
	error = drv_fdt_reg(fdt, node, 0, &pwmdac->physical, &size);
	if (error != 0)
		return error;

	/* Refuses a register block smaller than the converter's. */
	if (size < PWMDAC_REGISTER_BYTES)
		return ERANGE;

	/* Maps the registers. */
	error = kern_device_map(pwmdac->physical, (size_t)size, KERN_DEVICE_UNCACHED, &mapped);
	if (error != 0)
		return error;

	/*
	 * Keeps the mapping, which also marks the jack attached, and leaves the
	 * converter configured but silent.
	 */
	pwmdac->registers = mapped;
	write_control(pwmdac, PWMDAC_CONTROL_CENTRED | PWMDAC_CONTROL_SIGNED);

	/* Describes the jack's operations to the audio framework. */
	pwmdac->ops.playback = 1U;
	pwmdac->ops.capture = 0U;
	pwmdac->ops.formats = pwmdac_formats;
	pwmdac->ops.format_count = sizeof(pwmdac_formats) / sizeof(pwmdac_formats[0]);
	pwmdac->ops.prepare = pwmdac_prepare;
	pwmdac->ops.start = pwmdac_start;
	pwmdac->ops.stop = pwmdac_stop;
	pwmdac->ops.position = pwmdac_position;

	/* Publishes the jack, its ring coming from the DMA controller's memory. */
	dma = drv_dw_axi_dmac_dma_device(pwmdac->dmac);
	error = drv_audio_register(&pwmdac->ops, pwmdac, dma, &pwmdac->audio);
	if (error != 0)
		return error;

	/* Reports the converter and its request line. */
	kern_logf("vf2-pwmdac: %llx, DMA request %u\n", (unsigned long long)pwmdac->physical, pwmdac->handshake);

	/* Succeeded: the jack is published. */
	return 0;
}

/* Finds the first converter the tree enables. */
static int
find_converter(
	const struct drv_fdt *fdt,
	uint32_t *node)
{
	uint32_t candidate;
	bool enabled;
	int error;

	/* Walks the converters until one is enabled. */
	candidate = DRV_FDT_NO_NODE;
	for (;;) {
		/* Moves to the next converter; none left means the board has no jack. */
		error = drv_fdt_find_compatible(fdt, "starfive,jh7110-pwmdac", candidate, &candidate);
		if (error != 0)
			return ENODEV;

		/* Stops at a converter the tree enables. */
		enabled = drv_fdt_node_enabled(fdt, candidate);
		if (enabled)
			break;
	}

	/* Succeeded: the converter's node. */
	*node = candidate;
	return 0;
}

/*
 * Reads the converter's dmas property: the controller's node and the
 * request line, which is the controller's handshake number, not one of
 * its channels.
 */
static int
find_dma(
	const struct drv_fdt *fdt,
	uint32_t node,
	uint32_t *controller,
	uint32_t *handshake)
{
	const uint8_t *value;
	uint32_t length;
	uint32_t phandle;
	int error;

	/* Reads the property. */
	error = drv_fdt_property(fdt, node, "dmas", &value, &length);
	if (error != 0)
		return error;

	/* Refuses anything but one controller phandle and one cell. */
	if (length != 8U)
		return EINVAL;

	/* Takes the phandle and the request line. */
	phandle = (uint32_t)drv_fdt_cells_value(value, 0, 1);
	*handshake = (uint32_t)drv_fdt_cells_value(value, 1, 1);

	/* The controller the phandle names. */
	error = drv_fdt_find_phandle(fdt, phandle, controller);
	if (error != 0)
		return error;

	/* Succeeded: the controller and the request line. */
	return 0;
}

/*
 * Routes the converter's two outputs to their pins, runs its bus and
 * sample clocks and releases its reset.
 *
 * Procedure referenced:
 * Linux v6.12 sound/soc/starfive/jh7110_pwmdac.c
 * (jh7110_pwmdac_crg_enable: enable the apb and core clocks, then
 * release the apb reset).
 */
static int
switch_on(
	const struct drv_fdt *fdt,
	uint32_t node)
{
	struct drv_vf2_clock bus_clock;
	struct drv_vf2_clock sample_clock;
	struct drv_vf2_reset reset;
	unsigned changed;
	int error;

	/* Gives GPIO 33 and 34 the converter's left and right outputs. */
	error = drv_vf2_pinctrl_apply_default(fdt, node, &changed);
	if (error != 0) {
		kern_logf("vf2-pwmdac: pins not routed (%d)\n", error);
		return error;
	}

	/* Finds the bus clock. */
	error = drv_vf2_clock_lookup(fdt, node, "apb", &bus_clock);
	if (error != 0)
		return error;

	/* Finds the sample clock. */
	error = drv_vf2_clock_lookup(fdt, node, "core", &sample_clock);
	if (error != 0)
		return error;

	/* Runs the bus clock. */
	error = drv_vf2_clock_enable(&bus_clock);
	if (error != 0)
		return error;

	/* Runs the sample clock. */
	error = drv_vf2_clock_enable(&sample_clock);
	if (error != 0)
		return error;

	/* Finds the converter's bus reset. */
	error = drv_vf2_reset_lookup(fdt, node, NULL, &reset);
	if (error != 0)
		return error;

	/* Releases it. */
	error = drv_vf2_reset_deassert(&reset);
	if (error != 0) {
		kern_logf("vf2-pwmdac: reset stays asserted (%d)\n", error);
		return error;
	}

	/* Succeeded: the converter runs. */
	return 0;
}

/*
 * Sets the sample clock as close to 48 kHz times 256 as the divider
 * reaches without exceeding it, and logs the sample rate it gives.
 */
static int
set_sample_clock(
	const struct drv_fdt *fdt,
	uint32_t node)
{
	struct drv_vf2_clock clock;
	uint64_t rate;
	int error;

	/* Finds the sample clock. */
	error = drv_vf2_clock_lookup(fdt, node, "core", &clock);
	if (error != 0)
		return error;

	/* Sets its divider. */
	error = drv_vf2_clock_set_rate(&clock, PWMDAC_SAMPLE_CLOCK);
	if (error != 0) {
		kern_logf("vf2-pwmdac: sample clock not set (%d)\n", error);
		return error;
	}

	/* Logs the rate reached, which differs from 48 kHz when PLL2 is not a multiple of it. */
	error = drv_vf2_clock_get_rate(&clock, &rate);
	if (error != 0)
		return error;
	kern_logf("vf2-pwmdac: sample clock %llu Hz, %llu samples/s\n",
		  (unsigned long long)rate,
		  (unsigned long long)(rate / PWMDAC_CYCLES_PER_SAMPLE));

	/* Succeeded: the sample clock runs at its rate. */
	return 0;
}

/* Writes the converter's control register. */
static void
write_control(
	struct vf2_pwmdac *pwmdac,
	uint32_t value)
{
	/* Writes the register. */
	kern_mmio_write32(pwmdac->registers + PWMDAC_CONTROL, value);
}

/* Reports one finished fragment to the audio framework. */
static void
period_done(
	void *argument)
{
	struct vf2_pwmdac *pwmdac;

	/* Counts the fragment for playback. */
	pwmdac = argument;
	drv_audio_interrupt(pwmdac->audio, 0);
}

/*
 * Describes the framework's ring to the DMA channel: one period a
 * fragment, from the ring to the converter's data register, a frame a
 * beat.
 */
static int
pwmdac_prepare(
	void *private_data,
	int capture,
	const struct audio_format *format,
	const struct drv_dma_buffer *buffer,
	uint32_t fragment_bytes,
	uint32_t fragment_count)
{
	struct drv_dw_axi_dmac_cyclic cyclic;
	struct vf2_pwmdac *pwmdac;
	int error;

	/* Refuses capture, which the jack does not have. */
	pwmdac = private_data;
	if (capture != 0)
		return ENOTSUP;

	/* Refuses anything but the one format the jack plays, and fragments of part frames. */
	if (format == NULL || buffer == NULL)
		return EINVAL;
	if (format->format != KERN_AUDIO_FORMAT_S16_LE ||
	    format->channels != 2U ||
	    format->rate != PWMDAC_RATE)
		return EINVAL;
	if (fragment_bytes % PWMDAC_FRAME_BYTES != 0)
		return EINVAL;

	/* Describes the ring as one period a fragment toward the data register. */
	kern_memset(&cyclic, 0, sizeof(cyclic));
	cyclic.ring_address = buffer->device_address;
	cyclic.period_bytes = fragment_bytes;
	cyclic.period_count = fragment_count;
	cyclic.device_address = pwmdac->physical + PWMDAC_DATA;
	cyclic.device_width = PWMDAC_FRAME_BYTES;
	cyclic.handshake = pwmdac->handshake;
	cyclic.period_done = period_done;
	cyclic.argument = pwmdac;

	/* Builds the channel's cyclic chain over the ring. */
	error = drv_dw_axi_dmac_cyclic_prepare(pwmdac->channel, &cyclic);
	if (error != 0) {
		kern_logf("vf2-pwmdac: ring not usable (%d)\n", error);
		return error;
	}

	/* Succeeded: the ring is ready to play, which lets the start run. */
	pwmdac->prepared = 1;
	return 0;
}

/*
 * Starts the DMA channel on the ring, then the converter, whose requests
 * pull the frames.
 *
 * Procedure referenced:
 * Linux v6.12 sound/soc/starfive/jh7110_pwmdac.c (jh7110_pwmdac_trigger
 * and jh7110_pwmdac_set: write the settings and set the enable bit on
 * start).
 */
static int
pwmdac_start(
	void *private_data,
	int capture)
{
	struct vf2_pwmdac *pwmdac;
	int error;

	/* Playback only, after a prepare. */
	pwmdac = private_data;
	if (capture != 0)
		return ENOTSUP;
	if (pwmdac->prepared == 0)
		return EINVAL;

	/* Starts the channel from the ring's first fragment. */
	error = drv_dw_axi_dmac_cyclic_start(pwmdac->channel);
	if (error != 0)
		return error;

	/* Turns the converter on with its settings. */
	write_control(pwmdac, PWMDAC_CONTROL_CENTRED | PWMDAC_CONTROL_SIGNED | PWMDAC_CONTROL_ENABLE);

	/* Succeeded: the jack plays. */
	return 0;
}

/*
 * Stops the converter, then the DMA channel; when this returns the
 * channel no longer reads the ring.
 *
 * Procedure referenced:
 * Linux v6.12 sound/soc/starfive/jh7110_pwmdac.c (jh7110_pwmdac_stop:
 * clear the enable bit on stop).
 */
static void
pwmdac_stop(
	void *private_data,
	int capture)
{
	struct vf2_pwmdac *pwmdac;

	/* Playback only. */
	pwmdac = private_data;
	if (capture != 0)
		return;

	/* Silences the converter and stops its requests. */
	write_control(pwmdac, PWMDAC_CONTROL_CENTRED | PWMDAC_CONTROL_SIGNED);

	/* Stops the channel and waits until it is idle. */
	drv_dw_axi_dmac_cyclic_stop(pwmdac->channel);
}

/* Tells where the DMA channel reads in the framework's ring. */
static uint32_t
pwmdac_position(
	void *private_data,
	int capture)
{
	struct vf2_pwmdac *pwmdac;
	uint32_t position;

	/* Playback only. */
	pwmdac = private_data;
	if (capture != 0)
		return 0;

	/* Succeeded: the start of the fragment being played. */
	position = drv_dw_axi_dmac_cyclic_position(pwmdac->channel);
	return position;
}
